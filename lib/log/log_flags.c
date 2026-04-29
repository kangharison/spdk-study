/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK 컴포넌트별 디버그 로그 플래그 토글 구현 (log_flags.c)
 *
 * === 파일의 역할 ===
 * SPDK의 컴포넌트(서브시스템) 단위 DEBUG 로그를 켜고 끄는 동적 플래그 테이블을 관리한다.
 * SPDK 코드는 SPDK_DEBUGLOG(flag, ...) 매크로를 사용하는데, 이 매크로는 해당 flag의
 * spdk_log_flag::enabled가 true일 때만 spdk_log를 호출한다. 본 파일은 (1) 그 플래그를
 * 보관하는 전역 TAILQ(g_log_flags), (2) 등록(register), (3) 이름·glob 매칭 토글
 * (set/clear), (4) 이터레이션, (5) `--logflag <flag>` 같은 CLI 도움말 출력기를 제공한다.
 * `--logflag bdev`나 RPC `log_set_flag`로 사용자가 런타임에 컴포넌트별 디버그 출력을
 * 선택적으로 활성화할 수 있게 하는 핵심 메커니즘이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 모든 컴포넌트(.c 파일)는 자신만의 로그 플래그를 등록한다:
 *   include/spdk/log.h 의 SPDK_LOG_REGISTER_COMPONENT(flag) 매크로가 .data 섹션에
 *   "struct spdk_log_flag SPDK_LOG_##flag" 인스턴스를 정의하고,
 *   __attribute__((constructor)) 함수로 spdk_log_register_flag(name, &flag)을 호출한다.
 *   → main() 진입 전에 모든 컴포넌트의 플래그가 g_log_flags TAILQ에 정렬 삽입된다.
 *
 * 호출 체인 (등록):
 *   _SPDK_LOG_##flag_register() (constructor) → spdk_log_register_flag → TAILQ_INSERT
 * 호출 체인 (토글):
 *   사용자 / RPC `log_set_flag` / `--logflag <name>` CLI
 *      → spdk_log_set_flag / spdk_log_clear_flag → log_set_flag → flag->enabled 갱신
 * 호출 체인 (조회):
 *   SPDK_DEBUGLOG(bdev, ...) 매크로 전개 시 SPDK_LOG_bdev.enabled 검사 (이 파일 외부)
 *   spdk_log_get_flag(name) → get_log_flag → flag->enabled 반환
 *
 * 실행 컨텍스트: 호스트 유저스페이스. 등록은 process startup(constructor 단계, 단일
 * 스레드)에서 일어나고, 토글은 RPC 핸들러(주로 RPC 스레드) 또는 부트스트랩에서 일어난다.
 * 조회(SPDK_DEBUGLOG)는 어떤 reactor/lcore에서든 일어남 — enabled는 단순 bool 읽기라
 * race가 잠시 옛 값을 보여줄 수 있으나, 디버그 출력의 한두 줄 누락/추가는 허용.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: libc <fnmatch.h>(fnmatch 와일드카드 매칭), <strings.h>(strcasecmp), TAILQ 매크로
 *         (spdk/queue.h 통해 BSD-style intrusive 큐). g_log_flags의 노드(struct spdk_log_flag)
 *         는 외부 컴포넌트가 .data에 정의한 정적 객체이므로 본 파일은 메모리 소유자가 아님.
 * - 의존되는 쪽: SPDK 내 거의 모든 .c 파일의 SPDK_LOG_REGISTER_COMPONENT 매크로,
 *               그리고 SPDK_DEBUGLOG/SPDK_LOGDUMP 매크로의 enabled 조회.
 *               module/event/rpc 의 RPC `log_set_flag`/`log_get_flags` 핸들러.
 *               app/spdk_tgt 등 메인 앱의 `--logflag` CLI 옵션 파서가 spdk_log_usage를 호출.
 * - 공유 상태: g_log_flags(TAILQ 헤드, 프로세스 전역 단일).
 *
 * === 주요 함수/구조체 요약 ===
 *   - g_log_flags : 모든 컴포넌트 플래그를 이름순으로 정렬해 보관하는 TAILQ. 초기값은
 *                   TAILQ_HEAD_INITIALIZER로 빈 리스트.
 *   - spdk_log_register_flag(name, flag) : constructor에서 호출. 중복 검사 + 이름 정렬
 *                   삽입(strcasecmp 비교).
 *   - get_log_flag(name) : 이름으로 한 개를 찾는 내부 헬퍼(strcasecmp 비교).
 *   - spdk_log_get_flag(name) : 외부 공개 — 해당 플래그가 활성인지 bool 반환.
 *   - spdk_log_set_flag / spdk_log_clear_flag : 한 이름(또는 glob 패턴, "all")으로 켜기/끄기.
 *                   내부 log_set_flag(name, value)에 위임. fnmatch 와일드카드 지원.
 *   - spdk_log_get_first_flag / spdk_log_get_next_flag : RPC `log_get_flags`가 사용하는
 *                   이터레이터.
 *   - spdk_log_usage(f, log_arg) : `--help` 출력에 등록된 모든 플래그 이름을 줄 바꿈을
 *                   고려해 100자 폭으로 보기 좋게 출력.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 인클루드 묶음 — <stdio.h>(fprintf/snprintf), <string.h>(strlen),
 * <strings.h>(strcasecmp), <stdint.h>(uint64_t), <stdbool.h>(bool), <fnmatch.h>(fnmatch),
 * <assert.h>(assert) 등을 모아 가져온다. 이 파일이 쓰는 모든 libc API가 여기서 충당. */

#include "spdk/log.h"
/* [한국어] struct spdk_log_flag 정의, SPDK_LOG_REGISTER_COMPONENT 매크로, 본 파일이
 * 정의하는 모든 외부 함수의 프로토타입, SPDK_ERRLOG 등 매크로를 가져온다. queue.h가
 * 트랜시티브로 인클루드되어 TAILQ_HEAD/TAILQ_FOREACH 등도 사용 가능. */

static TAILQ_HEAD(spdk_log_flag_head,
		  spdk_log_flag) g_log_flags = TAILQ_HEAD_INITIALIZER(g_log_flags);
/* [한국어] 프로세스 전역의 컴포넌트 플래그 리스트.
 * 자료구조: BSD TAILQ — head + 노드 양방향 연결. 노드 자체는 외부에 .data 정적 객체로
 *           존재하므로 본 리스트는 단순 연결 — 메모리 할당/해제 없음.
 * 정렬: 이름 오름차순(strcasecmp). spdk_log_register_flag가 삽입 시 정렬 유지.
 * 설정자: spdk_log_register_flag (constructor 단계, 단일 스레드).
 * 읽는 자: get_log_flag, log_set_flag, spdk_log_*flag, spdk_log_usage (런타임).
 * 동기화: 등록은 모두 main() 진입 전에 끝나고 이후엔 read-only처럼 다뤄진다(구조 변경
 *         없음, enabled bool만 토글). 따라서 락 불필요.
 * spdk_log_flag_head는 TAILQ_LAST의 type 인자로 쓰기 위해 named tag를 부여한 형태. */

/*
 * [한국어]
 * get_log_flag - 이름(대소문자 구분 없음)으로 g_log_flags에서 플래그 1개를 찾는다.
 *
 * @name: 찾을 컴포넌트 이름 (예: "bdev", "nvme", "rdma").
 * @return: 일치하는 spdk_log_flag* 또는 NULL.
 *
 * 단순 선형 탐색 — 등록된 컴포넌트 수가 수십~수백 개 수준이라 O(n)으로 충분.
 * strcasecmp로 비교하므로 "BDEV"/"bdev"/"Bdev" 모두 같은 항목으로 취급된다.
 * 내부 전용 — 외부에는 spdk_log_get_flag(bool 반환)가 노출.
 *
 * 호출 체인:
 *   spdk_log_register_flag (중복 검사) / spdk_log_get_flag → get_log_flag → TAILQ 순회
 */
static struct spdk_log_flag *
get_log_flag(const char *name)
{
	struct spdk_log_flag *flag;
	/* [한국어] 순회 커서. */

	TAILQ_FOREACH(flag, &g_log_flags, tailq) {
		/* [한국어] BSD TAILQ 매크로 — head→next 방향으로 노드를 하나씩 순회. */
		if (strcasecmp(name, flag->name) == 0) {
			/* [한국어] 대소문자 무시 일치 → 사용자가 "BDEV"라고 쳐도 OK. */
			return flag;
		}
	}

	return NULL;
	/* [한국어] 못 찾으면 NULL — 호출자가 NULL 체크 후 적절히 처리. */
}

/*
 * [한국어]
 * spdk_log_register_flag - 컴포넌트 플래그를 g_log_flags에 정렬 삽입. constructor에서 호출.
 *
 * @name: 컴포넌트 이름 (NULL 금지).
 * @flag: 외부에 정적으로 정의된 spdk_log_flag* (NULL 금지).
 *        flag->name은 보통 #flag(매크로 #-stringification)로 채워져 있어야 한다.
 *        SPDK_LOG_REGISTER_COMPONENT 매크로가 자동으로 채움.
 * @return: 없음. 실패는 SPDK_ERRLOG + assert로 알림 (잘못된 사용은 개발 단계에서 잡힘).
 *
 * 동작:
 *   1) NULL 인자 체크.
 *   2) 중복 등록 체크 — 이미 같은 이름이 있으면 ERRLOG 후 반환.
 *   3) strcasecmp 비교로 알파벳 순(case-insensitive) 정렬 삽입.
 *      iter->name > flag->name인 첫 노드 앞에 끼워넣고 종료.
 *      그런 노드가 없으면 끝에 붙임.
 *
 * 실행 컨텍스트: 거의 항상 __attribute__((constructor)) 단계 — main() 진입 전, 단일
 * 스레드에서 모듈 로드 순으로 호출됨. 따라서 동시성 고려 불필요.
 *
 * 호출 체인:
 *   __attribute__((constructor)) _SPDK_LOG_##flag_register
 *      → spdk_log_register_flag → TAILQ_INSERT_BEFORE/TAIL
 */
void
spdk_log_register_flag(const char *name, struct spdk_log_flag *flag)
{
	struct spdk_log_flag *iter;
	/* [한국어] 정렬 위치 탐색용 커서. */

	if (name == NULL || flag == NULL) {
		/* [한국어] 잘못된 호출 — SPDK_LOG_REGISTER_COMPONENT 매크로 잘못 썼거나 빌드 결함. */
		SPDK_ERRLOG("missing spdk_log_flag parameters\n");
		assert(false);
		/* [한국어] 디버그 빌드에서 즉시 abort — 개발 중 빨리 잡기 위함. release에서는 무시. */
		return;
	}

	if (get_log_flag(name)) {
		/* [한국어] 중복 등록 — 두 컴포넌트가 같은 이름을 쓰는 경우. 위와 동일하게 빠른 실패. */
		SPDK_ERRLOG("duplicate spdk_log_flag '%s'\n", name);
		assert(false);
		return;
	}

	TAILQ_FOREACH(iter, &g_log_flags, tailq) {
		/* [한국어] 정렬 유지 삽입 위치 탐색 — 새 flag보다 사전식으로 큰 첫 노드 찾기. */
		if (strcasecmp(iter->name, flag->name) > 0) {
			TAILQ_INSERT_BEFORE(iter, flag, tailq);
			/* [한국어] 그 노드 앞에 새로 끼움 — 결과적으로 알파벳 오름차순 유지.
			 * spdk_log_usage가 사용자에게 도움말을 출력할 때 보기 좋게 정렬되도록. */
			return;
		}
	}

	TAILQ_INSERT_TAIL(&g_log_flags, flag, tailq);
	/* [한국어] 더 큰 노드가 없었음 → 새 flag가 가장 큰 이름 → 끝에 붙임. */
}

/*
 * [한국어]
 * spdk_log_get_flag - 컴포넌트 플래그 활성 여부를 외부에 bool로 노출.
 *
 * @name: 컴포넌트 이름.
 * @return: 등록되어 있고 enabled==true면 true, 그 외 false.
 *
 * 등록되지 않은 이름이면 false — 사용자가 typo한 경우에도 죽지 않고 조용히 비활성 취급.
 * RPC 응답이나 사용자 코드가 "현재 bdev 디버그 켜져 있나?" 같은 질문을 할 때 사용.
 *
 * 호출 체인:
 *   사용자 / RPC `log_get_flags` 핸들러 → spdk_log_get_flag → get_log_flag
 */
bool
spdk_log_get_flag(const char *name)
{
	struct spdk_log_flag *flag = get_log_flag(name);
	/* [한국어] 이름으로 노드 찾기 — 없으면 NULL. */

	if (flag && flag->enabled) {
		/* [한국어] NULL 체크와 enabled 검사를 분리 — 미등록은 false로 떨어진다. */
		return true;
	}

	return false;
}

/*
 * [한국어]
 * log_set_flag - 한 이름(또는 "all", 또는 glob 패턴)에 매칭되는 모든 플래그를 토글.
 *
 * @name:  컴포넌트 이름, "all" 키워드, 또는 fnmatch glob 패턴 (예: "nvm*").
 * @value: true=활성화, false=비활성화.
 * @return: 0=하나 이상 매칭됨, -EINVAL=어떤 플래그도 매칭되지 않음.
 *
 * 동작:
 *   1) "all"이면 모든 플래그를 일괄 토글하고 0 반환.
 *   2) 그 외에는 fnmatch(FNM_CASEFOLD)로 와일드카드(`*`,`?`) 매칭을 허용.
 *      매칭된 플래그가 하나라도 있으면 rc=0으로 갱신, 없으면 -EINVAL 유지.
 *
 * 실행 컨텍스트: RPC 핸들러 스레드 또는 main 부트스트랩. 토글 중에 다른 lcore가 enabled
 * 를 읽을 수 있지만, bool 1바이트 쓰기는 워드 단위로 atomic이라 읽기 측은 옛 값/새 값
 * 둘 중 하나만 본다 — 일시적으로 한두 줄의 디버그 출력 누락/추가 가능 (허용).
 *
 * 호출 체인:
 *   spdk_log_set_flag / clear_flag → log_set_flag → fnmatch + flag->enabled 갱신
 */
static int
log_set_flag(const char *name, bool value)
{
	struct spdk_log_flag *flag;
	/* [한국어] 순회 커서. */
	int rc = -EINVAL;
	/* [한국어] 기본값은 실패(-EINVAL = 매칭 없음). 매칭이 1건이라도 있으면 0으로 덮어씀. */

	if (strcasecmp(name, "all") == 0) {
		/* [한국어] 특수 키워드 "all" — 모든 컴포넌트 일괄 토글. RPC `log_set_flag all`이나
		 * `--logflag all` 케이스 — 항상 성공으로 처리. */
		TAILQ_FOREACH(flag, &g_log_flags, tailq) {
			flag->enabled = value;
			/* [한국어] 단순 bool 대입. */
		}
		return 0;
	}

	TAILQ_FOREACH(flag, &g_log_flags, tailq) {
		if (fnmatch(name, flag->name, FNM_CASEFOLD) == 0) {
			/* [한국어] fnmatch — POSIX glob 매칭. FNM_CASEFOLD로 대소문자 무시.
			 * "nvme*"이면 "nvme", "nvme_pcie", "nvme_tcp" 등 모두 매칭. */
			flag->enabled = value;
			rc = 0;
			/* [한국어] 한 건이라도 매칭되면 성공 — 계속 순회해 다른 매칭도 모두 토글. */
		}
	}

	return rc;
	/* [한국어] 0이면 하나 이상 토글, -EINVAL이면 아무것도 매칭 안 됨(사용자에게 typo 알림). */
}

/*
 * [한국어]
 * spdk_log_set_flag - 이름/패턴으로 플래그를 활성화한다.
 *
 * @name: 이름, "all", 또는 fnmatch 패턴.
 * @return: 0 / -EINVAL (log_set_flag 동일).
 *
 * 호출 체인:
 *   RPC `log_set_flag` 핸들러 / `--logflag <name>` CLI 파서 → spdk_log_set_flag
 *      → log_set_flag(name, true)
 */
int
spdk_log_set_flag(const char *name)
{
	return log_set_flag(name, true);
	/* [한국어] true 전달 — 활성화. */
}

/*
 * [한국어]
 * spdk_log_clear_flag - 이름/패턴으로 플래그를 비활성화한다.
 *
 * @name: 이름, "all", 또는 fnmatch 패턴.
 * @return: 0 / -EINVAL.
 *
 * 호출 체인:
 *   RPC `log_clear_flag` 핸들러 → spdk_log_clear_flag → log_set_flag(name, false)
 */
int
spdk_log_clear_flag(const char *name)
{
	return log_set_flag(name, false);
	/* [한국어] false 전달 — 비활성화. */
}

/*
 * [한국어]
 * spdk_log_get_first_flag - 등록된 첫 플래그를 반환 (이터레이션 시작).
 *
 * @return: g_log_flags의 첫 노드 또는 NULL(빈 리스트).
 *
 * RPC `log_get_flags` 핸들러가 모든 플래그 상태를 JSON으로 덤프할 때 사용.
 * spdk_log_get_first_flag → spdk_log_get_next_flag 의 쌍으로 순회 패턴.
 *
 * 호출 체인:
 *   RPC `log_get_flags` → spdk_log_get_first_flag → TAILQ_FIRST
 */
struct spdk_log_flag *
spdk_log_get_first_flag(void)
{
	return TAILQ_FIRST(&g_log_flags);
	/* [한국어] BSD TAILQ_FIRST 매크로 — head->tqh_first 반환 (NULL이면 빈 리스트). */
}

/*
 * [한국어]
 * spdk_log_get_next_flag - 이터레이션 중 다음 플래그를 반환.
 *
 * @flag: 직전 노드 포인터 (NULL 금지 — TAILQ_NEXT 동작 가정).
 * @return: 다음 노드 또는 NULL(끝).
 *
 * 호출 체인:
 *   RPC `log_get_flags` 순회 루프 → spdk_log_get_next_flag → TAILQ_NEXT
 */
struct spdk_log_flag *
spdk_log_get_next_flag(struct spdk_log_flag *flag)
{
	return TAILQ_NEXT(flag, tailq);
	/* [한국어] tailq 필드(struct에 박힌 TAILQ_ENTRY)를 통해 다음 노드 포인터 반환. */
}

/*
 * [한국어]
 * spdk_log_usage - `--help` 출력에 등록된 모든 컴포넌트 플래그 이름을 보기 좋게 출력.
 *
 * @f:       출력 FILE* (보통 stdout).
 * @log_arg: 출력 첫 줄에 박힐 짧은 옵션 문자열 (예: "-L"). 호출자가 자기 옵션 이름을 전달.
 * @return:  없음.
 *
 * 출력 양식 예시 (100자 폭 줄바꿈):
 *    -L, --logflag <flag>      enable log flag (all, accel, accel_dsa, ..., bdev,
 *                              bdev_iscsi, bdev_lvol, ..., vmd_pci)
 *
 * 동작:
 *   1) 첫 줄에 옵션 prefix("-L, --logflag <flag>      enable log flag (all, ") 출력.
 *   2) g_log_flags TAILQ를 순회하며 각 이름을 ", "로 구분해 붙임.
 *   3) 다음 항목을 추가했을 때 100자를 넘으면 줄바꿈 + 27칸 들여쓰기(LINE_PREFIX) 후 계속.
 *   4) 마지막 항목 뒤에는 ", "를 붙이지 않고 ")\n"으로 닫음.
 *
 * 호출 체인:
 *   app/spdk_tgt 등 메인 앱의 `--help` 옵션 파서 → spdk_log_usage
 *      → fprintf(f) (g_log_flags 순회하며 한 줄씩)
 */
void
spdk_log_usage(FILE *f, const char *log_arg)
{
#define LINE_PREFIX			"                           "
	/* [한국어] 줄바꿈 후 정렬용 들여쓰기 — 27칸 공백. 첫 줄의 "  -L, --logflag ..." 다음
	 * 항목 시작 위치에 맞춰 보기 좋게 정렬한다. */
#define ENTRY_SEPARATOR			", "
	/* [한국어] 항목 사이 구분자 — ", " (콤마+공백, 2자). */
#define MAX_LINE_LENGTH			100
	/* [한국어] 한 줄 최대 폭 — 보통 터미널 80자보다 약간 큰 100자로 설정. 이 폭을 넘으면 강제 개행. */
	uint64_t prefix_len = strlen(LINE_PREFIX);
	/* [한국어] LINE_PREFIX 길이 캐시 — 새 줄 시작 시 curr_line_len의 초기값으로 사용. */
	uint64_t separator_len = strlen(ENTRY_SEPARATOR);
	/* [한국어] ", " 길이(=2) 캐시 — 줄바꿈 결정 식에서 사용. */
	const char *first_entry = "--logflag <flag>      enable log flag (all, ";
	/* [한국어] 첫 줄 후미 텍스트 — `log_arg, ` 뒤에 붙는 고정 부분. "all"은 특수 키워드라 명시. */
	uint64_t curr_line_len;
	/* [한국어] 현재 줄에 누적된 글자 수 — 다음 항목을 출력해도 100자 미만인지 검사용. */
	uint64_t curr_entry_len;
	/* [한국어] 다음에 출력할 항목(플래그 이름)의 길이. */
	struct spdk_log_flag *flag;
	/* [한국어] 순회 커서. */
	char first_line[MAX_LINE_LENGTH] = {};
	/* [한국어] 첫 줄 임시 버퍼 — snprintf로 만들고 그 길이로 curr_line_len 초기화. */

	snprintf(first_line, sizeof(first_line), " %s, %s", log_arg, first_entry);
	/* [한국어] 첫 줄 시작 — " <log_arg>, --logflag <flag>      enable log flag (all, ". */
	fprintf(f, "%s", first_line);
	/* [한국어] 첫 줄 출력 — 아직 개행 없음, 이어서 플래그 이름들이 같은 줄에 붙음. */
	curr_line_len = strlen(first_line);
	/* [한국어] 첫 줄 길이로 누적값 초기화. */

	TAILQ_FOREACH(flag, &g_log_flags, tailq) {
		/* [한국어] 모든 등록 플래그 순회 — 이미 알파벳순으로 정렬되어 있음. */
		curr_entry_len = strlen(flag->name);
		/* [한국어] 다음 항목 길이. */
		if ((curr_line_len + curr_entry_len + separator_len) > MAX_LINE_LENGTH) {
			/* [한국어] 다음 항목까지 추가하면 100자 초과 → 줄바꿈 + 들여쓰기. */
			fprintf(f, "\n%s", LINE_PREFIX);
			curr_line_len = prefix_len;
			/* [한국어] 새 줄 시작 위치를 들여쓰기 폭으로 리셋. */
		}

		fprintf(f, "%s", flag->name);
		/* [한국어] 플래그 이름 출력. */
		curr_line_len += curr_entry_len;
		/* [한국어] 누적 길이 갱신. */

		if (TAILQ_LAST(&g_log_flags, spdk_log_flag_head) == flag) {
			/* [한국어] 마지막 항목이면 ", " 분리자 없이 break — 뒤에 ")"이 붙어야 깔끔. */
			break;
		}

		fprintf(f, "%s", ENTRY_SEPARATOR);
		/* [한국어] 마지막이 아니면 ", " 출력. */
		curr_line_len += separator_len;
		/* [한국어] 누적 길이 갱신. */
	}

	fprintf(f, ")\n");
	/* [한국어] 닫는 괄호 + 개행 — 도움말 출력 마무리. */
}
