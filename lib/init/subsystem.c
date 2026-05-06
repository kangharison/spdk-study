/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK 서브시스템 등록·초기화·종료 코어 (subsystem.c)
 *
 * === 파일의 역할 ===
 * SPDK 의 "subsystem" 추상은 NVMe / NVMe-oF / bdev / sock / vhost 등 각 기능 모듈을
 * 한 단위로 묶어 부팅과 종료를 일관되게 관리하기 위한 메커니즘이다. 본 파일은 그
 * 코어 — 모듈이 자기 자신을 정적으로 등록할 수 있는 글로벌 리스트(g_subsystems),
 * 의존성 그래프(g_subsystems_deps), 위상 정렬(subsystem_sort), 비동기 init/fini
 * 체이닝(spdk_subsystem_init_next / spdk_subsystem_fini_next), 그리고 외부 진입점
 * (spdk_subsystem_init / spdk_subsystem_fini) 을 구현한다.
 * 모듈은 SPDK_SUBSYSTEM_REGISTER 매크로(컴파일러 constructor) 로 main() 진입 전에
 * spdk_add_subsystem() 을 호출해 자기를 등록하고, 의존하는 다른 서브시스템이 있다면
 * SPDK_SUBSYSTEM_DEPEND 매크로로 spdk_add_subsystem_depend() 를 호출하여 (name,
 * depends_on) 쌍을 추가한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 순서:
 *   spdk_app_start (app/lib)
 *     → 각 서브시스템의 constructor 가 자동 실행되어 g_subsystems / g_subsystems_deps 채움
 *     → spdk_subsystem_init(cb_fn, cb_arg)         # 본 파일
 *         → 의존성 검증 (모든 (name, depends_on) 가 존재하는지)
 *         → subsystem_sort()                      # 의존성 만족하는 순서로 위상 정렬
 *         → spdk_subsystem_init_next(0)
 *             → 각 subsystem->init() 비동기 호출
 *             → 각 init() 은 끝나면 다시 spdk_subsystem_init_next() 콜백
 *             → 모든 init 완료 시 cb_fn(0, cb_arg) 호출 → 서비스 ready
 * 종료:
 *   spdk_subsystem_fini(cb_fn, cb_arg)             # 본 파일
 *     → spdk_subsystem_fini_next() 가 역순 순회하며 fini() 호출
 *     → 모두 끝나면 cb_fn(cb_arg)
 * 실행 컨텍스트: 모든 함수가 SPDK app thread (master reactor) 단독 호출.
 * 비동기 init/fini 도 결과 콜백이 같은 app thread 에서 실행되도록 모듈이 보장한다.
 *
 * === 타 모듈과의 연결 ===
 * - 모든 서브시스템 모듈 (lib/bdev, lib/nvmf, module/bdev/*, lib/sock 등):
 *   SPDK_SUBSYSTEM_REGISTER 로 자신의 spdk_subsystem(이름, init, fini, write_config_json)
 *   을 g_subsystems 에 등록.
 * - lib/init/json_config.c: spdk_subsystem_init / spdk_subsystem_exists 호출자.
 *   STARTUP→RUNTIME 전이 시 spdk_subsystem_init 트리거.
 * - lib/init/subsystem_rpc.c: framework_get_subsystems / framework_get_config 가
 *   subsystem_get_first/next, subsystem_find, subsystem_config_json 을 호출.
 * - 공유 자료구조: g_subsystems(TAILQ), g_subsystems_deps(TAILQ), 그리고 비동기 진행
 *   상태(g_next_subsystem, g_subsystems_initialized, g_subsystems_init_interrupted,
 *   g_subsystem_start_fn/arg, g_subsystem_stop_fn/arg). 모두 app thread 단독 접근.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_add_subsystem(): 서브시스템 객체를 g_subsystems 에 등록 (constructor 단계).
 * - spdk_add_subsystem_depend(): 의존 관계를 g_subsystems_deps 에 등록 (constructor 단계).
 * - subsystem_sort(): Kahn-스타일 위상 정렬 — 의존성이 모두 sorted_list 에 있으면 추출,
 *   반복. O(N^2 * D) 단순 구현이지만 N 이 작아 충분.
 * - spdk_subsystem_init(): init 시퀀스 진입점. 의존성 검증 → 정렬 → init_next(0).
 * - spdk_subsystem_init_next(): 비동기 init 체이닝의 콜백. 각 모듈의 init() 후 이걸 호출하면 다음으로 진행.
 * - spdk_subsystem_fini() / fini_next(): 종료 체이닝 (역순).
 * - subsystem_config_json(): 모듈의 write_config_json 콜백을 안전하게 호출 (NULL 시 null 출력).
 */

#include "spdk/stdinc.h"        /* [한국어] 표준 C 라이브러리 (assert, string 등) */

#include "spdk/init.h"          /* [한국어] spdk_subsystem_init / fini 등 본 파일이 정의하는 공개 API prototype */
#include "spdk/log.h"           /* [한국어] SPDK_ERRLOG 매크로 — 에러 메시지 출력 */
#include "spdk/queue.h"         /* [한국어] BSD-style TAILQ 매크로 (TAILQ_HEAD, FOREACH, INSERT_TAIL 등) */
#include "spdk/thread.h"        /* [한국어] spdk_thread_is_app_thread (assert 검증용) */

#include "spdk_internal/init.h" /* [한국어] spdk_subsystem / spdk_subsystem_depend 구조체 정의 (init 라이브러리 내부) */
#include "spdk/env.h"           /* [한국어] DPDK 환경 추상화 — 이 파일에서는 직접 사용은 적으나 헤더 호환 */

#include "spdk/json.h"          /* [한국어] spdk_json_write_ctx / spdk_json_write_null — write_config_json 처리에 사용 */

#include "subsystem.h"          /* [한국어] 본 init 라이브러리 내부 헤더 — subsystem_find/get_first/get_next 등 prototype */

/* [한국어] 서브시스템 리스트의 타입 정의. spdk_subsystem 구조체를 next 링크로 연결.
 * TAILQ 는 BSD-style 양방향 큐로 head/tail 둘 다 O(1) 접근 가능. */
TAILQ_HEAD(spdk_subsystem_list, spdk_subsystem);

/* [한국어] 등록된 모든 서브시스템의 글로벌 헤드.
 * 설정자: spdk_add_subsystem() (constructor 시점에 모든 모듈이 자기 자신 등록),
 *         subsystem_sort() (위상 정렬 후 swap).
 * 읽는 자: spdk_subsystem_init/_next, spdk_subsystem_fini_next, subsystem_find,
 *          subsystem_get_first/next, subsystem_rpc.c.
 * 동기화: constructor 단계는 단일 스레드(main 진입 전), 이후엔 app thread 단독 접근. */
struct spdk_subsystem_list g_subsystems = TAILQ_HEAD_INITIALIZER(g_subsystems);

/* [한국어] 의존성 리스트의 타입 정의. 각 노드는 (name, depends_on) 한 쌍. */
TAILQ_HEAD(spdk_subsystem_depend_list, spdk_subsystem_depend);

/* [한국어] 등록된 모든 (name, depends_on) 의존성 쌍의 글로벌 헤드.
 * 설정자: spdk_add_subsystem_depend() (constructor 단계).
 * 읽는 자: spdk_subsystem_init (검증), subsystem_sort (정렬 판단), subsystem_rpc.c.
 * 동기화: app thread 단독. */
struct spdk_subsystem_depend_list g_subsystems_deps = TAILQ_HEAD_INITIALIZER(g_subsystems_deps);

/* [한국어] 비동기 init/fini 진행 중 "현재 처리 중인" 서브시스템 포인터.
 * 설정자: spdk_subsystem_init_next/fini_next 가 매 단계마다 갱신.
 * 읽는 자: 같은 두 함수가 콜백 진입 시 다음 단계 결정에 사용.
 * 값 범위: NULL(아직 시작 안함 또는 끝남) 또는 g_subsystems 의 한 노드.
 * 동기화: app thread 단독 접근 — 비동기 init() 콜백도 app thread 에서 진행 보장. */
static struct spdk_subsystem *g_next_subsystem;

/* [한국어] 모든 서브시스템의 init() 가 정상 완료되었는지 표시.
 * 설정자: spdk_subsystem_init_next 가 마지막 노드 다음(=NULL) 도달 시 true.
 * 읽는 자: spdk_subsystem_fini_next 가 fini 시작 위치 결정에 사용. */
static bool g_subsystems_initialized = false;

/* [한국어] init 진행 중에 fini 가 호출되어 init 시퀀스가 중단되었는지 표시.
 * 설정자: spdk_subsystem_fini_next 가 init 미완 상태에서 호출되면 true 로 토글.
 * 읽는 자: spdk_subsystem_init_next 가 진입 시 검사하여 즉시 return.
 * 의도: 부팅 도중 강제 shutdown 시 진행 중이던 init 콜백이 늦게 들어와도 안전 종료. */
static bool g_subsystems_init_interrupted = false;

/* [한국어] init 시퀀스 완료 시 호출할 사용자 콜백.
 * 설정자: spdk_subsystem_init() 진입 시 사용자 인자로 저장.
 * 읽는 자: spdk_subsystem_init_next 가 마지막에 호출 (또는 에러 시 즉시 호출). */
static spdk_subsystem_init_fn g_subsystem_start_fn = NULL;

/* [한국어] g_subsystem_start_fn 의 컨텍스트 인자. */
static void *g_subsystem_start_arg = NULL;

/* [한국어] fini 시퀀스 완료 시 호출할 사용자 콜백.
 * 설정자: spdk_subsystem_fini() 진입 시 저장.
 * 읽는 자: spdk_subsystem_fini_next 가 마지막에 호출. */
static spdk_msg_fn g_subsystem_stop_fn = NULL;

/* [한국어] g_subsystem_stop_fn 의 컨텍스트 인자. */
static void *g_subsystem_stop_arg = NULL;

/*
 * [한국어]
 * spdk_add_subsystem - 서브시스템을 글로벌 리스트에 등록 (공개 API)
 *
 * @subsystem: 등록할 spdk_subsystem 구조체 포인터 (정적 인스턴스).
 * @return: void.
 *
 * 각 서브시스템 모듈은 SPDK_SUBSYSTEM_REGISTER(name) 매크로로 컴파일러 constructor
 * 함수를 정의하고, 그 안에서 본 함수를 호출하여 자기 spdk_subsystem 객체를 등록한다.
 * 모든 등록은 main() 진입 전에 완료되며, 등록 순서는 링크 순서에 따라 비결정적이지만
 * 이후 subsystem_sort() 가 의존성 그래프를 보고 정렬하므로 등록 순서는 무관하다.
 *
 * 호출 체인:
 *   __attribute__((constructor)) (각 모듈) → [spdk_add_subsystem]
 */
void
spdk_add_subsystem(struct spdk_subsystem *subsystem)
{
	/* [한국어] 단순히 글로벌 리스트의 tail 에 삽입. constructor 단계라 race condition 없음. */
	TAILQ_INSERT_TAIL(&g_subsystems, subsystem, tailq);
}

/*
 * [한국어]
 * spdk_add_subsystem_depend - 서브시스템 의존성을 글로벌 리스트에 등록 (공개 API)
 *
 * @depend: (name, depends_on) 쌍을 보유한 spdk_subsystem_depend 구조체 (정적 인스턴스).
 * @return: void.
 *
 * SPDK_SUBSYSTEM_DEPEND(name, depends_on) 매크로가 정의한 constructor 가 호출.
 * 의존성은 subsystem_sort() 의 위상 정렬과 spdk_subsystem_init() 의 검증에 사용.
 *
 * 호출 체인:
 *   __attribute__((constructor)) → [spdk_add_subsystem_depend]
 */
void
spdk_add_subsystem_depend(struct spdk_subsystem_depend *depend)
{
	/* [한국어] 의존성 리스트 tail 에 삽입 — 등록 순서는 서비스 동작에 영향 없음 */
	TAILQ_INSERT_TAIL(&g_subsystems_deps, depend, tailq);
}

/*
 * [한국어]
 * _subsystem_find - 주어진 리스트에서 이름으로 서브시스템 검색 (내부 유틸)
 *
 * @list: 검색 대상 spdk_subsystem_list (g_subsystems 또는 정렬용 임시 리스트).
 * @name: 찾을 서브시스템 이름.
 * @return: 매칭되는 spdk_subsystem* 또는 NULL.
 *
 * 단순 선형 검색. 서브시스템 수가 작으므로(보통 5~15개) 충분히 빠름.
 * subsystem_sort() 가 정렬 진행 중인 임시 sorted_list 에 검색해야 하므로 list 인자를
 * 외부에서 받는다. 외부 노출은 subsystem_find(name) 래퍼만 허용 (g_subsystems 고정).
 *
 * 호출 체인:
 *   subsystem_find / subsystem_sort → [_subsystem_find]
 */
static struct spdk_subsystem *
_subsystem_find(struct spdk_subsystem_list *list, const char *name)
{
	struct spdk_subsystem *iter;             /* [한국어] 순회용 임시 포인터 */

	TAILQ_FOREACH(iter, list, tailq) {       /* [한국어] 리스트의 모든 노드 순회 */
		if (strcmp(name, iter->name) == 0) { /* [한국어] 이름 정확 일치 검사 */
			return iter;
		}
	}

	return NULL;                             /* [한국어] 미발견 */
}

/*
 * [한국어]
 * subsystem_find - 글로벌 리스트에서 이름으로 검색 (init 라이브러리 내부 API)
 *
 * @name: 찾을 서브시스템 이름.
 * @return: 매칭되는 spdk_subsystem* 또는 NULL.
 *
 * subsystem_rpc.c (framework_get_config) 와 spdk_subsystem_init (의존성 검증) 등에서
 * g_subsystems 검색 단축 경로로 사용.
 *
 * 호출 체인:
 *   spdk_subsystem_init / spdk_subsystem_exists / rpc_framework_get_config → [subsystem_find]
 *     → _subsystem_find
 */
struct spdk_subsystem *
subsystem_find(const char *name)
{
	return _subsystem_find(&g_subsystems, name);  /* [한국어] 글로벌 리스트에 위임 */
}

/*
 * [한국어]
 * spdk_subsystem_exists - 주어진 이름의 서브시스템 등록 여부 확인 (공개 API)
 *
 * @name: 확인할 서브시스템 이름.
 * @return: true=등록됨, false=없음.
 *
 * json_config.c 가 RPC 메서드 미발견 시 "그 메서드의 서브시스템이 이 빌드에 링크
 * 안 되어 있나?" 를 검사하기 위해 호출. 미링크면 그 RPC 호출을 단순 skip 한다.
 *
 * 호출 체인:
 *   json_config.c → [spdk_subsystem_exists] → subsystem_find
 */
bool
spdk_subsystem_exists(const char *name)
{
	assert(spdk_thread_is_app_thread(NULL));  /* [한국어] app thread 에서만 안전하게 호출 가능 */

	return subsystem_find(name) != NULL;     /* [한국어] 검색 결과를 boolean 으로 변환 */
}

/*
 * [한국어]
 * subsystem_get_first - 정렬된 글로벌 서브시스템 리스트의 첫 노드
 *
 * @return: 첫 spdk_subsystem* 또는 NULL (없을 때).
 *
 * subsystem_rpc.c 가 framework_get_subsystems 응답을 작성하는 데 사용.
 * 호출 시점이 spdk_subsystem_init() 이후라면 이미 위상 정렬되어 있어 init 순서대로 노출됨.
 *
 * 호출 체인:
 *   subsystem_rpc.c → [subsystem_get_first]
 */
struct spdk_subsystem *
subsystem_get_first(void)
{
	return TAILQ_FIRST(&g_subsystems);       /* [한국어] TAILQ 의 head 노드 반환 (없으면 NULL) */
}

/*
 * [한국어]
 * subsystem_get_next - 다음 서브시스템 노드 반환
 *
 * @cur_subsystem: 현재 노드.
 * @return: 다음 노드 또는 NULL.
 *
 * subsystem_rpc.c 의 순회 루프에서 사용.
 *
 * 호출 체인:
 *   subsystem_rpc.c → [subsystem_get_next]
 */
struct spdk_subsystem *
subsystem_get_next(struct spdk_subsystem *cur_subsystem)
{
	return TAILQ_NEXT(cur_subsystem, tailq);  /* [한국어] tailq 링크의 next 포인터 반환 */
}


/*
 * [한국어]
 * subsystem_get_first_depend - 의존성 리스트의 첫 노드
 *
 * @return: 첫 spdk_subsystem_depend* 또는 NULL.
 *
 * subsystem_rpc.c 가 모든 (name, depends_on) 쌍을 응답에 포함시킬 때 사용.
 *
 * 호출 체인:
 *   subsystem_rpc.c → [subsystem_get_first_depend]
 */
struct spdk_subsystem_depend *
subsystem_get_first_depend(void)
{
	return TAILQ_FIRST(&g_subsystems_deps);  /* [한국어] 의존성 리스트의 head */
}

/*
 * [한국어]
 * subsystem_get_next_depend - 다음 의존성 노드
 *
 * @cur_depend: 현재 의존성 노드.
 * @return: 다음 노드 또는 NULL.
 *
 * 호출 체인:
 *   subsystem_rpc.c → [subsystem_get_next_depend]
 */
struct spdk_subsystem_depend *
subsystem_get_next_depend(struct spdk_subsystem_depend *cur_depend)
{
	return TAILQ_NEXT(cur_depend, tailq);    /* [한국어] tailq 링크의 next */
}

/*
 * [한국어]
 * subsystem_sort - 의존성 그래프에 따라 g_subsystems 를 위상 정렬 (내부 유틸)
 *
 * @return: void (성공). 사이클이 있으면 무한 루프 가능 — 사전 검증된 입력 가정.
 *
 * Kahn-알고리즘 류 위상 정렬. 알고리즘:
 *   while (g_subsystems 비어있지 않음):
 *     for each subsystem in g_subsystems:
 *       if 의존성이 없거나 모든 의존성이 sorted_list 에 이미 있으면:
 *         g_subsystems 에서 제거 → sorted_list tail 에 추가
 *   g_subsystems ↔ sorted_list 교체
 *
 * 결과: g_subsystems 의 노드 순서가 "의존하는 게 먼저, 의존받는 게 뒤" 로 정렬.
 * O(N^2 * D) 단순 구현이지만 N (서브시스템 수) 가 작아 충분.
 *
 * 사이클 안전성: 호출자(spdk_subsystem_init) 가 미리 모든 의존성 이름의 존재만 검증
 * 하고 사이클은 검증하지 않으므로, 사이클 입력 시 본 함수는 무한 루프에 빠진다 —
 * SPDK 빌드 시 의존성 사이클이 없도록 모듈 작성자가 보장해야 한다.
 *
 * 호출 체인:
 *   spdk_subsystem_init → [subsystem_sort]
 */
static void
subsystem_sort(void)
{
	bool has_dependency, all_dependencies_met;       /* [한국어] 현재 노드가 의존성을 가졌는지 / 모두 충족됐는지 */
	struct spdk_subsystem *subsystem, *subsystem_tmp;  /* [한국어] FOREACH_SAFE 의 현재/임시 포인터 */
	struct spdk_subsystem_depend *subsystem_dep;     /* [한국어] 의존성 순회용 포인터 */
	struct spdk_subsystem_list sorted_list;          /* [한국어] 정렬 결과를 모으는 임시 리스트 */

	TAILQ_INIT(&sorted_list);                        /* [한국어] 임시 리스트 초기화 (head=tail=NULL) */
	/* We will move subsystems from the original g_subsystems TAILQ to the temporary
	 * sorted_list one at a time. We can only move a subsystem if it either (a) has no
	 * dependencies, or (b) all of its dependencies have already been moved to the
	 * sorted_list.
	 *
	 * Once all of the subsystems have been moved to the temporary list, we will move
	 * the list as-is back to the original g_subsystems TAILQ - they will now be sorted
	 * in the order which they must be initialized.
	 */
	/* [한국어] 외부 루프: g_subsystems 가 빌 때까지 반복. 매 외부 iteration 마다 적어도 1개는
	 * 옮겨져야 종료(=DAG 가정). 사이클 시 아무 것도 안 옮겨지면 무한 루프 → DAG 사전 검증 필수. */
	while (!TAILQ_EMPTY(&g_subsystems)) {
		/* [한국어] 내부 루프: 현재 g_subsystems 의 모든 노드를 검사. FOREACH_SAFE 는 순회 중 제거 안전. */
		TAILQ_FOREACH_SAFE(subsystem, &g_subsystems, tailq, subsystem_tmp) {
			has_dependency = false;          /* [한국어] 이 서브시스템이 등록된 의존성을 가졌는가? */
			all_dependencies_met = true;     /* [한국어] 가졌다면 그 모든 의존성이 sorted_list 에 이미 있는가? */
			/* [한국어] 글로벌 의존성 리스트 전체를 훑어 현재 subsystem 이름과 일치하는 (name,depends_on) 만 검사 */
			TAILQ_FOREACH(subsystem_dep, &g_subsystems_deps, tailq) {
				if (strcmp(subsystem->name, subsystem_dep->name) == 0) {
					has_dependency = true;  /* [한국어] 적어도 하나의 의존성을 가짐 */
					/* [한국어] depends_on 이 sorted_list 에 아직 없다면 — 아직 옮길 수 없음 */
					if (!_subsystem_find(&sorted_list, subsystem_dep->depends_on)) {
						/* We found a dependency that isn't in the sorted_list yet.
						 * Clear the flag and break from the inner loop, we know
						 * we can't move this subsystem to the sorted_list yet.
						 */
						all_dependencies_met = false;
						break;          /* [한국어] 더 검사할 필요 없음 — 이 노드는 이번 iteration 에서 skip */
					}
				}
			}

			/* [한국어] 의존성이 아예 없거나, 모든 의존성이 충족된 경우만 옮긴다. */
			if (!has_dependency || all_dependencies_met) {
				TAILQ_REMOVE(&g_subsystems, subsystem, tailq);          /* [한국어] 원본에서 제거 */
				TAILQ_INSERT_TAIL(&sorted_list, subsystem, tailq);      /* [한국어] 정렬 리스트의 tail 에 추가 */
			}
		}
	}

	/* [한국어] sorted_list 가 정답. g_subsystems 와 통째로 swap — 노드 포인터들의 tailq 링크는 sorted_list 기준으로 갱신되어 있음.
	 * TAILQ_SWAP 은 head/tail 포인터만 교환하는 O(1) 연산. */
	TAILQ_SWAP(&sorted_list, &g_subsystems, spdk_subsystem, tailq);
}

/*
 * [한국어]
 * spdk_subsystem_init_next - 비동기 init 체이닝의 진행 콜백 (공개 API)
 *
 * @rc: 직전 서브시스템의 init 결과. 0=성공, 음수=실패.
 * @return: void.
 *
 * 각 서브시스템의 init() 함수는 비동기 작업을 트리거한 뒤 반환되며, 작업 완료 시
 * 본 함수를 호출하여 다음 서브시스템으로 진행한다 (콜백 체인 패턴). 첫 호출은
 * spdk_subsystem_init() 가 spdk_subsystem_init_next(0) 으로 트리거.
 *
 * 분기 처리:
 *   1. g_subsystems_init_interrupted (init 중 fini 호출됨) → 즉시 return — 정렬된 종료는 fini_next 에서 시작
 *   2. rc != 0 → 에러 보고 후 사용자 콜백을 에러 코드로 호출, 진행 중단
 *   3. 처음 호출 (g_next_subsystem == NULL) → 첫 노드로 시작
 *   4. 일반 호출 → next 노드로
 *   5. next 가 NULL (모든 노드 처리 완료) → g_subsystems_initialized=true 후 사용자 콜백(0) 호출
 *   6. 다음 노드의 init() 호출 (없으면 재귀로 다음 단계 — 단, 자기 호출은 비추천이지만 init=NULL 흔하지 않음)
 *
 * 실행 컨텍스트: app thread. 비동기 init 콜백은 모듈이 같은 thread 로 돌아오도록 책임짐.
 *
 * 호출 체인:
 *   spdk_subsystem_init / 서브시스템 모듈의 init 콜백 → [spdk_subsystem_init_next]
 *     → 다음 subsystem->init() (정의되어 있으면) 또는 사용자 cb_fn (마지막)
 */
void
spdk_subsystem_init_next(int rc)
{
	assert(spdk_thread_is_app_thread(NULL));  /* [한국어] app thread 단독 접근 강제 */

	/* The initialization is interrupted by the spdk_subsystem_fini, so just return */
	/* [한국어] init 진행 중 fini 가 호출되어 인터럽트된 경우 — 늦게 들어오는 init 콜백을 무시.
	 * 종료 시퀀스는 spdk_subsystem_fini_next 가 별도로 진행 중. */
	if (g_subsystems_init_interrupted) {
		return;
	}

	if (rc) {                                 /* [한국어] 직전 서브시스템 init 실패 — 진행 중단, 사용자 콜백을 에러로 호출 */
		SPDK_ERRLOG("Init subsystem %s failed\n", g_next_subsystem->name);
		g_subsystem_start_fn(rc, g_subsystem_start_arg);
		return;
	}

	if (!g_next_subsystem) {                  /* [한국어] 첫 호출 — 아직 시작 안함 → 정렬된 리스트의 첫 노드 선택 */
		g_next_subsystem = TAILQ_FIRST(&g_subsystems);
	} else {                                  /* [한국어] 진행 중 — 현재 노드의 다음 노드로 이동 */
		g_next_subsystem = TAILQ_NEXT(g_next_subsystem, tailq);
	}

	if (!g_next_subsystem) {                  /* [한국어] 모든 노드 처리 완료 — 부팅 성공 */
		g_subsystems_initialized = true;
		g_subsystem_start_fn(0, g_subsystem_start_arg);  /* [한국어] 사용자 콜백을 0(성공)으로 호출 */
		return;
	}

	if (g_next_subsystem->init) {             /* [한국어] 모듈이 init 콜백을 정의한 경우 — 호출 후 비동기 결과 대기 */
		g_next_subsystem->init();
	} else {                                  /* [한국어] init 콜백이 NULL 이면 즉시 다음 단계로 (재귀 호출) */
		spdk_subsystem_init_next(0);
	}
}

/*
 * [한국어]
 * spdk_subsystem_init - 서브시스템 init 시퀀스 진입점 (공개 API)
 *
 * @cb_fn: 모든 서브시스템 init 완료 시 호출될 사용자 콜백.
 * @cb_arg: cb_fn 의 컨텍스트 인자.
 * @return: void. 결과는 cb_fn(rc, cb_arg) 로 비동기 통보.
 *
 * (1) 의존성 그래프의 모든 (name, depends_on) 가 실제 등록된 서브시스템인지 검증
 *     — 미발견이면 즉시 cb_fn(-1) 호출.
 * (2) subsystem_sort() — 의존성 위상 정렬.
 * (3) spdk_subsystem_init_next(0) — 첫 노드부터 init 체인 시작.
 *
 * 실행 컨텍스트: SPDK app thread (master reactor). spdk_app_start 와 json_config.c 가 호출.
 *
 * 호출 체인:
 *   spdk_app_start / json_config.c (subsystem_init_done 직전 framework_start_init) → [spdk_subsystem_init]
 *     → subsystem_sort, spdk_subsystem_init_next(0)
 */
void
spdk_subsystem_init(spdk_subsystem_init_fn cb_fn, void *cb_arg)
{
	struct spdk_subsystem_depend *dep;        /* [한국어] 의존성 검증 순회용 */

	assert(spdk_thread_is_app_thread(NULL));  /* [한국어] app thread 단독 호출 강제 */

	g_subsystem_start_fn = cb_fn;             /* [한국어] 완료 콜백 저장 — 비동기 체인 끝에서 호출됨 */
	g_subsystem_start_arg = cb_arg;

	/* Verify that all dependency name and depends_on subsystems are registered */
	/* [한국어] 사전 검증 — 모든 의존성 노드의 name/depends_on 이 g_subsystems 에 실제로 존재해야 함.
	 * 누락 시 즉시 에러 콜백 — 의미 없는 정렬/init 시도를 방지. */
	TAILQ_FOREACH(dep, &g_subsystems_deps, tailq) {
		if (!subsystem_find(dep->name)) {
			SPDK_ERRLOG("subsystem %s is missing\n", dep->name);
			g_subsystem_start_fn(-1, g_subsystem_start_arg);  /* [한국어] 사용자 콜백 -1(실패) */
			return;
		}
		if (!subsystem_find(dep->depends_on)) {
			SPDK_ERRLOG("subsystem %s dependency %s is missing\n",
				    dep->name, dep->depends_on);
			g_subsystem_start_fn(-1, g_subsystem_start_arg);
			return;
		}
	}

	subsystem_sort();                         /* [한국어] 의존성 충족하는 순서로 위상 정렬 */

	spdk_subsystem_init_next(0);              /* [한국어] 비동기 init 체인 시작 — 첫 노드의 init() 부터 호출됨 */
}

/*
 * [한국어]
 * spdk_subsystem_fini_next - 비동기 fini 체이닝의 진행 콜백 (공개 API)
 *
 * @return: void.
 *
 * fini 는 init 의 역순으로 진행한다 (TAILQ_LAST → PREV → ...). 각 서브시스템의
 * fini() 콜백은 자신의 종료가 끝나면 본 함수를 다시 호출하여 다음(=이전) 노드로 진행.
 *
 * 진행 분기:
 *   1. g_next_subsystem == NULL (첫 호출 또는 init 도중 인터럽트):
 *      - g_subsystems_initialized==true → 정상 init 후 종료 → 마지막 노드부터 시작
 *      - 그렇지 않으면 → init 시퀀스가 첫 노드 진입 전이므로 종료할 게 없음 (변동 없이 진행)
 *   2. 두 번째 이후 호출:
 *      - init 이 끝났거나 이미 인터럽트된 경우 → 한 칸 앞(이전 노드)으로 이동
 *      - 그렇지 않으면(=init 진행 중인데 fini 가 들어온 첫 호출) → interrupted=true 표시
 *        (g_next_subsystem 은 그대로 — 현재 init 중인 노드부터 fini)
 *   3. fini 콜백이 정의된 첫 노드를 찾을 때까지 루프, 찾으면 호출 후 return
 *      (콜백 안에서 끝나면 다시 본 함수 호출)
 *   4. 모든 노드의 fini 가 끝나면 사용자 stop 콜백 호출
 *
 * 실행 컨텍스트: app thread.
 *
 * 호출 체인:
 *   spdk_subsystem_fini / 서브시스템 모듈의 fini 콜백 → [spdk_subsystem_fini_next]
 *     → 이전 subsystem->fini() 또는 사용자 stop_fn (마지막)
 */
void
spdk_subsystem_fini_next(void)
{
	assert(spdk_thread_is_app_thread(NULL));  /* [한국어] app thread 단독 호출 */

	if (!g_next_subsystem) {                  /* [한국어] 진입 시 진행 포인터가 NULL — 첫 호출 또는 init 미시작 상태 */
		/* If the initialized flag is false, then we've failed to initialize
		 * the very first subsystem and no de-init is needed
		 */
		/* [한국어] 정상 init 까지 끝났다면 마지막 노드부터 시작; 아니면(init 도중 실패 등) 종료할 노드 없음 */
		if (g_subsystems_initialized) {
			g_next_subsystem = TAILQ_LAST(&g_subsystems, spdk_subsystem_list);
		}
	} else {                                  /* [한국어] 이미 진행 중 (이전 노드의 fini 가 본 함수를 다시 호출했거나, init 중 fini 가 들어옴) */
		if (g_subsystems_initialized || g_subsystems_init_interrupted) {
			/* [한국어] 정상 종료 진행 중 — 한 칸 이전 노드로 이동 (역순 순회) */
			g_next_subsystem = TAILQ_PREV(g_next_subsystem, spdk_subsystem_list, tailq);
		} else {
			/* [한국어] init 진행 중에 fini 가 처음 들어옴 — 인터럽트 표시.
			 * 이 시점부터 spdk_subsystem_init_next 의 늦게 들어오는 콜백은 무시된다.
			 * g_next_subsystem 은 그대로 — 현재 init 시도 중인 노드부터 fini 시작. */
			g_subsystems_init_interrupted = true;
		}
	}

	while (g_next_subsystem) {                /* [한국어] fini 콜백이 정의된 다음 노드를 찾을 때까지 역순으로 skip */
		if (g_next_subsystem->fini) {     /* [한국어] fini 정의된 첫 노드 — 호출 후 비동기 결과 대기 */
			g_next_subsystem->fini();
			return;
		}
		/* [한국어] fini 가 NULL 인 노드는 그냥 넘어감 */
		g_next_subsystem = TAILQ_PREV(g_next_subsystem, spdk_subsystem_list, tailq);
	}

	/* [한국어] 모든 노드의 fini 완료 — 사용자 stop 콜백 호출 */
	g_subsystem_stop_fn(g_subsystem_stop_arg);
	return;
}

/*
 * [한국어]
 * spdk_subsystem_fini - 서브시스템 종료 시퀀스 진입점 (공개 API)
 *
 * @cb_fn: 모든 서브시스템 fini 완료 시 호출될 사용자 콜백 (spdk_msg_fn 시그니처).
 * @cb_arg: cb_fn 의 컨텍스트 인자.
 * @return: void.
 *
 * SPDK 종료 흐름에서 spdk_app_stop 또는 사용자 코드가 호출. 콜백 저장 후
 * spdk_subsystem_fini_next() 로 종료 체인 시작.
 *
 * 호출 체인:
 *   spdk_app_stop / 사용자 → [spdk_subsystem_fini] → spdk_subsystem_fini_next
 */
void
spdk_subsystem_fini(spdk_msg_fn cb_fn, void *cb_arg)
{
	assert(spdk_thread_is_app_thread(NULL));  /* [한국어] app thread 단독 호출 */

	g_subsystem_stop_fn = cb_fn;              /* [한국어] 완료 콜백 저장 */
	g_subsystem_stop_arg = cb_arg;

	spdk_subsystem_fini_next();               /* [한국어] 종료 체인 시작 */
}

/*
 * [한국어]
 * subsystem_config_json - 한 서브시스템의 write_config_json 콜백을 안전하게 호출
 *
 * @w: JSON 작성 컨텍스트.
 * @subsystem: 대상 서브시스템 (NULL 가능).
 * @return: void.
 *
 * 호출 시점에 서브시스템이 자기 현재 활성 설정을 JSON 으로 직렬화하여 외부에 전달.
 * write_config_json 콜백이 NULL 이거나 subsystem 자체가 NULL 이면 JSON null 을 출력.
 * subsystem_rpc.c 의 framework_get_config 핸들러가 호출.
 *
 * 호출 체인:
 *   rpc_framework_get_config → [subsystem_config_json]
 *     → subsystem->write_config_json(w) (모듈별 콜백)
 */
void
subsystem_config_json(struct spdk_json_write_ctx *w, struct spdk_subsystem *subsystem)
{
	if (subsystem && subsystem->write_config_json) {  /* [한국어] subsystem 과 콜백 둘 다 존재해야 위임 호출 */
		subsystem->write_config_json(w);
	} else {
		/* [한국어] 미정의 — 표준적인 "정보 없음" 표현으로 JSON null 출력 (응답 호환성) */
		spdk_json_write_null(w);
	}
}
