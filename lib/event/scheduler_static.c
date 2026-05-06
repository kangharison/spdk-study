/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK 정적(static) 스케줄러 구현 (scheduler_static.c)
 *
 * === 파일의 역할 ===
 * SPDK 이벤트 프레임워크가 제공하는 "static(정적)" 스레드 스케줄러를 구현한다.
 * SPDK 응용 프로그램이 시작될 때 각 spdk_thread는 spdk_app_opts.reactor_mask로
 * 지정된 어떤 reactor(=lcore=논리 코어)에 처음 배치되는데, static 스케줄러는
 * 한 번 배치된 위치를 영구적으로 고정시키는 역할을 한다. 즉 어떤 lcore의
 * 부하가 높아져도, 다른 lcore의 부하가 낮아져도 스레드를 절대 옮기지 않으며
 * 단순히 "처음 배치된 lcore(initial_lcore)" 정보를 유지하다가 다른 스케줄러
 * (dynamic 등)로 전환되었다가 다시 static으로 돌아오는 경우 그 원래 매핑을
 * 복원하는 데 사용된다. dynamic/gscheduler처럼 부하에 따라 thread를 코어 간
 * 이동시키는 동적 스케줄러와 정확히 대비된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 실행 모델은 [reactor(코어 1개) ↔ spdk_thread N개] 구조이며, 어떤
 * spdk_thread가 어느 reactor에 배치되는지를 결정하는 주체가 스케줄러이다.
 * 호출 체인은 다음과 같다:
 *   spdk_app_start()
 *     → reactor_run() (각 lcore의 reactor 메인 루프)
 *       → 주기적으로 _reactors_scheduler_gather_metrics() 호출
 *         → 등록된 spdk_scheduler 객체의 ->balance() 콜백 호출
 *           → 본 파일의 balance_static() 가 실행됨
 * SPDK_SCHEDULER_REGISTER 매크로가 constructor 시점에 본 모듈을 이벤트
 * 라이브러리에 자동 등록하므로, 사용자가 RPC framework_set_scheduler 로
 * "static" 을 선택하면 즉시 활성화된다. 본 파일은 lib/event/ 의 일부이며,
 * lib/scheduler/ 의 다른 스케줄러 구현(dynamic, gscheduler)들과 인터페이스
 * 를 공유한다.
 *
 * === 타 모듈과의 연결 ===
 *  - lib/thread/: spdk_thread / spdk_lw_thread 의 정의를 가져온다. 본 파일은
 *    spdk_lw_thread::initial_lcore 필드에 직접 접근하여 매핑을 보존/복원한다.
 *  - lib/event/reactor.c: reactor 메인 루프가 ->balance 콜백을 호출하는 주체.
 *    spdk_reactor_get(core) 를 통해 해당 lcore에 reactor가 존재하는지 검증한다.
 *  - lib/scheduler/scheduler.c: spdk_scheduler 등록 테이블 관리, 본 파일의
 *    SPDK_SCHEDULER_REGISTER 가 사용하는 등록 인프라.
 *  - JSON-RPC: framework_set_scheduler RPC 가 set_opts 콜백을 통해
 *    "mappings" 옵션 (예: "1,0:2,1:3,2") 을 본 파일에 전달한다.
 *
 *  데이터 흐름:
 *    [user] rpc.py framework_set_scheduler --name static --mappings "tid,core:..."
 *      → app_rpc.c::rpc_framework_set_scheduler()
 *        → spdk_scheduler_set("static") + scheduler->set_opts(params)
 *          → 본 파일 set_opts_static()
 *            → spdk_lw_thread->initial_lcore 갱신
 *              → spdk_scheduler_set_period(1) 로 즉시 balance 트리거
 *                → balance_static() 가 실제 매핑을 적용
 *
 * === 주요 함수/구조체 요약 ===
 *  - init_static():      스케줄러 활성화 시 호출. 첫 로드면 period=0(스케줄링 안 함),
 *                        다른 스케줄러에서 복귀한 경우면 period=1로 즉시 한 번 balance.
 *  - deinit_static():    스케줄러 비활성화(다른 스케줄러로 전환) 시 호출. 다음에
 *                        다시 static으로 돌아오면 매핑을 복원해야 함을 표시(g_first_load=false).
 *  - balance_static():   각 thread의 lcore를 initial_lcore로 되돌린 뒤 period=0으로 비활성화.
 *  - set_opts_static():  "mappings" JSON 옵션을 파싱하여 thread→core 매핑을 갱신.
 *                        2-pass 검증(전부 valid) 후에만 적용하는 방식.
 *  - struct spdk_scheduler scheduler: 스케줄러 vtable. SPDK_SCHEDULER_REGISTER 로 등록.
 *  - g_first_load:       이 스케줄러가 처음 로드된 적이 있는지(즉, deinit이 한 번이라도
 *                        호출되었는지)를 기록하는 전역 플래그. init 동작 분기에 사용.
 */

#include "spdk/stdinc.h"          /* [한국어] 표준 C 헤더 일괄 포함 (stdint, stdbool 등) - SPDK 내부 의존성. */
#include "spdk/likely.h"          /* [한국어] spdk_likely/unlikely 분기 힌트 매크로 - 본 파일에서는 직접 사용은 없으나 SPDK 헤더 컨벤션. */
#include "spdk/event.h"           /* [한국어] spdk_app_*, reactor 인프라 공개 API - reactor_get 등을 위해 필요. */
#include "spdk/log.h"             /* [한국어] SPDK_ERRLOG/DEBUGLOG 매크로 정의 - 검증 실패 시 에러 로그 출력에 사용. */
#include "spdk/env.h"             /* [한국어] DPDK 추상화 (lcore 정보 등) - SPDK_ENV_FOREACH_CORE 등의 매크로 정의. */
#include "spdk/string.h"          /* [한국어] spdk_strtol 등 SPDK 문자열 헬퍼 - "mappings" 문자열 파싱에 필수. */
#include "spdk/scheduler.h"       /* [한국어] spdk_scheduler 구조체와 등록 매크로 - 본 모듈이 구현하는 인터페이스의 정의. */

#include "spdk_internal/event.h"  /* [한국어] 이벤트 라이브러리 내부 API (spdk_reactor_get 등) - 외부 사용자에게는 공개되지 않음. */
#include "event_internal.h"       /* [한국어] lib/event/ 내부 헤더 - spdk_lw_thread 정의 (initial_lcore 필드를 직접 만지기 위해 필요). */

/*
 * [한국어]
 * g_first_load - 이 스케줄러가 처음 로드된 후 한 번이라도 deinit 되었는지를 표시하는 플래그.
 *
 * 설정자: deinit_static() 이 false 로 설정 (즉, "이미 한 번 사용했음"을 의미).
 * 읽는 자: init_static() 이 분기 결정에 사용.
 * 값 범위: true (아직 한 번도 deinit 안 됨, 즉 이번이 최초 활성화) /
 *           false (전에 사용되었다가 다른 스케줄러로 갔다가 돌아옴).
 * 동기화: 스케줄러 init/deinit 은 스케줄링 코어(스케줄러 전용 단일 thread)에서만
 *          호출되므로 락이 필요 없다. SPDK 의 단일 코어 기준 직렬 실행 모델에 의존.
 */
static bool g_first_load = true;

/*
 * [한국어]
 * init_static - "static" 스케줄러를 활성화할 때 호출되는 초기화 콜백.
 *
 * @return: 0 (성공). 본 구현은 실패 경로가 없다.
 *
 * 동작:
 *  - 최초 로드 (g_first_load == true): static 스케줄러는 본질적으로 "스케줄링을 하지
 *    않는" 스케줄러이므로 spdk_scheduler_set_period(0) 으로 주기적 balance 호출을
 *    꺼버린다. period=0 은 reactor 가 ->balance 를 절대 호출하지 않게 만든다.
 *  - 재진입 (이전에 dynamic 등 다른 스케줄러를 거쳐 다시 static 으로 돌아온 경우):
 *    그 동안 thread 들이 다른 lcore 로 옮겨졌을 수 있으므로, period=1 (즉시 한 번
 *    실행) 로 balance_static() 을 트리거하여 initial_lcore 매핑을 복원한다.
 *    balance 한 번이 끝나면 다시 period=0 으로 끄는 것은 balance_static() 의
 *    마지막 라인이 담당한다.
 *
 * 호출 체인:
 *   spdk_scheduler_set("static")
 *     → spdk_scheduler 의 ->init 콜백
 *       → init_static() (본 함수)
 *
 * 실행 컨텍스트: 스케줄링 코어(spdk_scheduler_get_scheduling_lcore() 가 가리키는
 *               단일 lcore) 위의 스케줄러 thread 에서 호출. 동시 호출 없음.
 */
static int
init_static(void)
{
	/* [한국어] 처음 활성화되는 경우인지 (한 번도 deinit 되지 않은 상태). */
	if (g_first_load) {
		/* There is no scheduling performed by static scheduler,
		 * do not set the scheduling period. */
		/* [한국어] period=0 ⇒ reactor 가 ->balance() 를 호출하지 않음.
		 *  static 의 본질은 "어떤 스케줄링도 하지 않음" 이므로 주기적 호출을 꺼둔다.
		 *  spdk_scheduler_set_period(0) 의 인자 단위는 마이크로초이며 0 은 "off" 의미. */
		spdk_scheduler_set_period(0);
	} else {
		/* Schedule a balance to happen immediately, so that
		 * we can reset each thread's lcore back to its original
		 * state.
		 */
		/* [한국어] 다른 스케줄러를 거쳤다가 돌아온 경우.
		 *  period=1 (1us, 즉 다음 reactor tick 에서) 로 balance_static() 을 한 번 호출
		 *  하여 모든 thread 의 lcore 를 initial_lcore 로 되돌린다. balance 가 끝나면
		 *  스스로 period=0 으로 다시 꺼버린다 (balance_static 마지막 라인 참조). */
		spdk_scheduler_set_period(1);
	}

	return 0; /* [한국어] 본 init 은 실패 경로가 없으므로 항상 성공. */
}

/*
 * [한국어]
 * deinit_static - "static" 스케줄러가 비활성화 (다른 스케줄러로 교체) 될 때 호출.
 *
 * 동작: 단순히 g_first_load = false 로 표시. 다음 번에 다시 static 으로 돌아오면
 *       init_static() 이 "재진입 경로" 로 진입하여 initial_lcore 복원을 트리거한다.
 *
 * 호출 체인:
 *   spdk_scheduler_set(<other>)
 *     → 이전 스케줄러의 ->deinit 콜백
 *       → deinit_static() (본 함수)
 *
 * 실행 컨텍스트: 스케줄링 코어 단일 thread.
 */
static void
deinit_static(void)
{
	/* [한국어] "한 번 deinit 된 적 있음" 을 영구 기록.
	 *  이 플래그는 init_static 에서만 읽히며, 다음 활성화 시 복원 동작을 트리거하기 위함. */
	g_first_load = false;
}

/*
 * [한국어]
 * balance_static - 모든 thread 의 현재 lcore 를 initial_lcore 로 되돌리는 콜백.
 *
 * @cores: reactor 별 정보 배열 (각 항목이 한 lcore 에 대응).
 * @core_count: 배열 길이 (= 활성 reactor 개수).
 *
 * 동기/배경:
 *  static 스케줄러의 핵심 의무는 "처음 배치된 lcore 에 thread 를 영구 고정" 이다.
 *  하지만 사용자가 dynamic 스케줄러로 잠시 전환하면 thread 가 다른 lcore 로 옮겨질
 *  수 있다. 이후 다시 static 으로 돌아오면, 본 함수가 한 번 실행되어 모든 thread 를
 *  원래 자리로 복귀시킨다. 또한 set_opts_static() 이 mappings 옵션으로 initial_lcore
 *  를 갱신했을 때도 본 함수가 호출되어 새 매핑을 실제로 적용한다.
 *
 * 동작:
 *  1) 모든 reactor 를 순회.
 *  2) 각 reactor 의 interrupt_mode 를 false 로 강제(static 은 polling 만 지원).
 *  3) 그 reactor 위에 올라간 모든 thread 에 대해 thread_info->lcore 를
 *     lw_thread->initial_lcore 로 덮어쓴다. reactor 측이 이 lcore 값을 보고
 *     실제 thread 이주(migration) 를 수행한다.
 *  4) 마지막에 spdk_scheduler_set_period(0) 으로 주기적 balance 호출을 다시 끈다.
 *
 * 호출 체인:
 *   reactor_run() → ... → ->balance 콜백 → balance_static()
 *     → 호출 후 SPDK 스케줄러 코어가 thread_info->lcore 변경을 보고 thread 이주 수행
 *
 * 실행 컨텍스트: 스케줄링 lcore (단일). thread 자체는 자기 reactor 위에 있지만,
 *               여기서는 메타데이터(thread_info) 만 갱신하므로 안전.
 */
static void
balance_static(struct spdk_scheduler_core_info *cores, uint32_t core_count)
{
	struct spdk_scheduler_core_info *core_info;     /* [한국어] cores[i] 에 대한 별칭 - 가독성을 위해 임시 포인터로 분리. */
	struct spdk_scheduler_thread_info *thread_info; /* [한국어] core_info->thread_infos[j] 에 대한 별칭 - 한 reactor 위에 올라가 있는 한 spdk_thread 의 메타. */
	struct spdk_lw_thread *lw_thread;               /* [한국어] spdk_thread 의 컨텍스트 영역(lightweight wrapper) - initial_lcore 가 여기에 들어 있음. */
	struct spdk_thread *thread;                     /* [한국어] thread_id 로부터 lookup 된 실제 spdk_thread 핸들. */
	uint32_t i, j;                                  /* [한국어] 각각 reactor 인덱스 / 그 reactor 안의 thread 인덱스. */

	/* [한국어] 모든 활성 reactor (=lcore) 를 순회. */
	for (i = 0; i < core_count; i++) {
		core_info = &cores[i]; /* [한국어] 현재 reactor 의 정보 슬롯을 가리킴. */
		core_info->interrupt_mode = false; /* [한국어] static 은 폴링 모드 전용 - reactor 가 인터럽트 모드로 들어가지 않게 강제. */
		/* [한국어] 이 reactor 에 올라가 있는 모든 thread 메타를 순회. */
		for (j = 0; j < core_info->threads_count; j++) {
			thread_info = &core_info->thread_infos[j];                /* [한국어] j 번째 thread 의 메타 슬롯. */
			thread = spdk_thread_get_by_id(thread_info->thread_id);    /* [한국어] thread_id (uint64_t) → spdk_thread* 로 lookup. */
			lw_thread = spdk_thread_get_ctx(thread);                   /* [한국어] spdk_thread 의 SPDK_LW_THREAD 컨텍스트(lw_thread) 추출. */
			thread_info->lcore = lw_thread->initial_lcore;             /* [한국어] 이 thread 의 목표 lcore 를 "원래 배치된 lcore" 로 복원.
			                                                            *  스케줄러 상위 레이어가 이 lcore 값을 보고 thread 를 실제로 이주시킨다. */
		}
	}

	/* We've restored the original state now, so we don't need to
	 * balance() anymore.
	 */
	/* [한국어] 복원이 끝났으므로 더 이상 주기적 balance 가 필요 없다.
	 *  period=0 으로 ->balance 호출을 꺼서 정적 모드의 본래 상태("아무것도 안 함") 로 회귀. */
	spdk_scheduler_set_period(0);
}

/*
 * [한국어]
 * static_sched_decoders - "static" 스케줄러의 set_opts JSON 디코딩 표.
 *
 * 의미:
 *   {"mappings": 0, spdk_json_decode_string, true}
 *     - "mappings" 키를 문자열로 읽고 (NULL 가능, true=optional).
 *     - offset 0 은 "포인터 자체에 결과를 쓴다" 는 의미 (decode_object_relaxed 의 외부 변수 직접 쓰기 패턴).
 *
 * "mappings" 형식: "<thread_id>,<lcore>:<thread_id>,<lcore>:..."
 *   예) "1,0:2,1:3,2"  ⇒ thread1→lcore0, thread2→lcore1, thread3→lcore2.
 *
 * 동기화: const 전역 (읽기 전용), 모든 코어에서 동시에 읽혀도 안전.
 */
static const struct spdk_json_object_decoder static_sched_decoders[] = {
	{"mappings", 0, spdk_json_decode_string, true},
	/* [한국어] mappings 키 - 사용자가 지정한 thread→lcore 매핑 문자열. optional. */
};

/*
 * [한국어]
 * set_opts_static - JSON-RPC framework_set_scheduler 의 추가 옵션 ("mappings") 처리.
 *
 * @opts: framework_set_scheduler RPC 의 전체 params (NULL 가능).
 * @return: 0 = 성공, -EINVAL = 파싱/검증 실패, -ENOMEM = 메모리 부족.
 *
 * 동기/배경:
 *  사용자가 "이 thread 는 저 lcore 에 고정해달라" 는 매핑을 동적으로 바꾸고 싶을 때,
 *  framework_set_scheduler 의 옵션으로 "mappings" 문자열을 넘긴다. 본 함수가
 *  파싱/검증/적용을 담당한다.
 *
 * 알고리즘 (2-pass):
 *  1) opts 에서 "mappings" 문자열을 추출 (decode_object_relaxed 로 다른 키도 허용).
 *  2) Pass 1: 문자열을 thread_id,core 쌍으로 토큰화하면서 "모두 valid 한가" 검증.
 *     - 한 쌍이라도 invalid 면 전체 거부 (atomic 갱신을 위해).
 *     - valid 조건: thread_id 는 양수 + 실존하는 spdk_thread, core 는 spdk_reactor_get != NULL,
 *                   core 가 thread 의 cpumask 에 포함됨.
 *  3) Pass 2: 모두 valid 가 확인된 mappings (사본 copy) 를 다시 토큰화하면서
 *     실제로 lw_thread->initial_lcore 를 갱신.
 *  4) period=1 로 balance_static() 을 즉시 트리거하여 새 매핑이 reactor 에 반영되도록 함.
 *
 * Pass 1 / Pass 2 분리 이유:
 *  - 도중에 invalid 가 발견되었는데 일부는 이미 갱신했다면 "부분 적용" 상태가 된다.
 *    이를 피하기 위해 검증 → 적용 을 분리. strtok_r 이 원본 문자열을 파괴적으로
 *    수정하므로 사본 (copy) 을 따로 만들어 두 번째 패스에 사용한다.
 *
 * 호출 체인:
 *   client → JSON-RPC framework_set_scheduler --name static --mappings "..."
 *     → app_rpc.c::rpc_framework_set_scheduler()
 *       → scheduler->set_opts(params)
 *         → set_opts_static() (본 함수)
 *           → spdk_scheduler_set_period(1)
 *             → reactor 가 다음 tick 에 balance_static() 호출
 *
 * 실행 컨텍스트: RPC 핸들러 thread (보통 RPC 가 등록된 spdk_thread).
 *               thread_set_cpumask 와 달리 cross-thread 메시지 송신은 없는데,
 *               이는 lw_thread->initial_lcore 갱신 자체는 단순 메모리 쓰기이고
 *               실제 마이그레이션은 balance_static() 이 스케줄링 lcore 에서
 *               하기 때문이다.
 */
static int
set_opts_static(const struct spdk_json_val *opts)
{
	char *tok, *mappings = NULL, *copy, *sp = NULL; /* [한국어] tok=현재 토큰, mappings=원본(strtok 가 파괴), copy=사본(2-pass용), sp=strtok_r 의 saveptr. */
	bool valid;                                      /* [한국어] Pass 1 검증 결과 - 한 쌍이라도 invalid 면 false 가 됨. */

	/* [한국어] params 가 비어있으면 옵션 변경 없이 정상 종료. (mappings 만 옵션이므로). */
	if (opts != NULL) {
		/* [한국어] static_sched_decoders 표를 따라 "mappings" 만 추출.
		 *  decode_object_relaxed: 모르는 다른 키가 있어도 무시 (framework_set_scheduler
		 *  공통 키인 name/period 등이 같이 들어오기 때문에 relaxed 가 필요). */
		if (spdk_json_decode_object_relaxed(opts, static_sched_decoders,
						    SPDK_COUNTOF(static_sched_decoders),
						    &mappings)) {
			SPDK_ERRLOG("Decoding scheduler opts JSON failed\n"); /* [한국어] JSON 형식 자체가 잘못됨. */
			return -EINVAL;                                        /* [한국어] -EINVAL 반환 → RPC 가 invalid params 응답. */
		}
	}

	/* [한국어] params 는 있었지만 "mappings" 키가 없는 경우 → 옵션 없음으로 정상 종료. */
	if (mappings == NULL) {
		return 0;
	}

	/* [한국어] strtok_r 은 원본을 파괴하므로 Pass 2 용 사본을 미리 떠둔다.
	 *  Pass 1 은 mappings 를 파괴하면서 검증, Pass 2 는 copy 를 파괴하면서 실제 적용. */
	copy = strdup(mappings);
	if (copy == NULL) {
		free(mappings);     /* [한국어] strdup 실패 시 mappings 도 해제하여 누수 방지. */
		return -ENOMEM;     /* [한국어] 메모리 부족 - 호출자에 그대로 전달. */
	}

	valid = true; /* [한국어] 낙관적 시작 - 한 번이라도 invalid 발견하면 false 로 떨어짐. */
	/* [한국어] Pass 1 시작: ':' 로 쌍을 분리.
	 *  "1,0:2,1" 의 경우 첫 토큰은 "1,0", 다음은 "2,1". */
	tok = strtok_r(mappings, ":", &sp);
	while (tok) {
		struct spdk_lw_thread *lw_thread = NULL; /* [한국어] 검증 중 발견된 lw_thread (NULL 이면 invalid). */
		struct spdk_thread *thread;              /* [한국어] thread_id 로 lookup 한 thread. */
		int thread_id, core;                     /* [한국어] 현재 토큰에서 파싱한 thread_id 와 core 번호. */

		/* [한국어] 첫 번째 ':' 로 분리된 토큰 ("1,0") 안에서 ',' 앞부분 ("1") 이 곧 thread_id.
		 *  단 spdk_strtol 은 strtok 이 자르기 전 전체 토큰을 받지만 ',' 가 숫자 파싱을 멈추므로 OK. */
		thread_id = spdk_strtol(tok, 10);
		if (thread_id > 0) { /* [한국어] thread_id 는 양수만 유효 (0 은 허용 안 함). */
			thread = spdk_thread_get_by_id(thread_id); /* [한국어] thread_id → spdk_thread* lookup. 실존 검사. */
			if (thread != NULL) {
				lw_thread = spdk_thread_get_ctx(thread); /* [한국어] valid 한 thread 의 lw_thread 컨텍스트 추출. */
			}
		}
		if (lw_thread == NULL) {
			SPDK_ERRLOG("invalid thread ID '%s' in mappings '%s'\n", tok, copy); /* [한국어] tok=잘린 일부지만 디버그용으로 충분. copy 는 원본 보존본. */
			valid = false; /* [한국어] 한 쌍 invalid → 전체 거부. */
			break;
		}

		/* [한국어] 두 번째 strtok_r 호출: 같은 sp 를 이어 사용하되 구분자를 ',' 로 바꿔
		 *  "1,0" 의 ',' 다음 부분 "0" 을 토큰으로 얻는다. NULL 첫 인자가 "이전 saveptr 이어서". */
		tok = strtok_r(NULL, ",", &sp);
		core = spdk_strtol(tok, 10);
		/* [한국어] core 검증: 음수 아니고, 해당 lcore 에 reactor 가 실제로 존재해야 함.
		 *  spdk_reactor_get(core) == NULL ⇒ reactor_mask 에 포함 안 된 lcore. */
		if (core < 0 || spdk_reactor_get(core) == NULL) {
			SPDK_ERRLOG("invalid core number '%s' in mappings '%s'\n", tok, copy);
			valid = false;
			break;
		}

		/* [한국어] thread 의 cpumask 가 그 core 를 허용하는지 확인.
		 *  spdk_thread 는 생성 시 부여된 cpumask 외부 lcore 로는 옮길 수 없다. */
		if (!spdk_cpuset_get_cpu(spdk_thread_get_cpumask(thread), core)) {
			SPDK_ERRLOG("core %d not in thread %d cpumask\n", core, thread_id);
			valid = false;
			break;
		}

		/* [한국어] 다음 쌍으로 진행. ':' 로 다시 분리. */
		tok = strtok_r(NULL, ":", &sp);
	}
	free(mappings); /* [한국어] Pass 1 에서 파괴된 원본은 더 이상 필요 없음 - 즉시 해제. */
	if (!valid) {
		free(copy); /* [한국어] 검증 실패 시 사본도 해제하고 종료. */
		return -EINVAL;
	}

	/* [한국어] Pass 2: 모두 valid 임이 확인됨 → 실제 적용.
	 *  여기서는 검증을 다시 하지 않고 단순히 lw_thread->initial_lcore 만 갱신. */
	tok = strtok_r(copy, ":", &sp);
	while (tok) {
		struct spdk_lw_thread *lw_thread = NULL; /* [한국어] Pass 1 에서 valid 가 보장되므로 NULL 체크는 생략됨. */
		struct spdk_thread *thread;
		int thread_id, core;

		thread_id = spdk_strtol(tok, 10);          /* [한국어] thread_id 추출 (Pass 1 와 동일). */
		thread = spdk_thread_get_by_id(thread_id); /* [한국어] thread lookup. */
		lw_thread = spdk_thread_get_ctx(thread);   /* [한국어] lw_thread 컨텍스트. */
		tok = strtok_r(NULL, ",", &sp);            /* [한국어] core 토큰으로 진행. */
		core = spdk_strtol(tok, 10);               /* [한국어] core 번호 추출. */
		/* initial_lcore saves the static scheduler's lcore mapping.
		 * This is used to restore the previous mapping if we
		 * change to another scheduler and then back. So we can just
		 * change the ->initial_lcore here and kick the scheduler to
		 * put the new mapping into effect.
		 */
		/* [한국어] initial_lcore 는 "이 thread 의 영구 거점 lcore" 의미.
		 *  여기서 갱신하면 다음 balance_static() 이 thread 를 새 lcore 로 이주시킨다.
		 *  실제 이주는 reactor 가 thread_info->lcore 변화를 감지해 수행한다. */
		lw_thread->initial_lcore = core;
		tok = strtok_r(NULL, ":", &sp); /* [한국어] 다음 쌍으로. */
	}
	free(copy); /* [한국어] Pass 2 의 사본도 사용 완료 - 해제. */

	/* We have updated some core placements, so kick the scheduler to
	 * apply those new placements.
	 */
	/* [한국어] period=1 (1us) 로 설정하여 다음 reactor tick 에서 balance_static() 을 한 번 호출.
	 *  balance_static() 은 thread_info->lcore 를 initial_lcore 로 동기화한 뒤 period=0 으로 다시 꺼버린다.
	 *  결국 한 번의 balance 만 일어나고 정적 모드로 복귀. */
	spdk_scheduler_set_period(1);
	return 0; /* [한국어] 성공. RPC 가 success 응답 반환. */
}

/*
 * [한국어]
 * scheduler - "static" 스케줄러의 vtable.
 *
 * 필드 설명:
 *  - .name: "static" - 사용자가 framework_set_scheduler --name static 으로 선택할 식별자.
 *  - .init: 활성화 시 호출되는 콜백.
 *  - .deinit: 비활성화(다른 스케줄러로 교체) 시 호출되는 콜백.
 *  - .balance: 주기적 (period > 0) 또는 일회성 (period=1 즉시 트리거) 호출되는 핵심 콜백.
 *  - .set_opts: framework_set_scheduler 의 추가 옵션 ("mappings") 처리 콜백.
 *  - (.get_opts 는 정의하지 않음 - static 은 노출할 영구 옵션이 없으므로.)
 *
 * 동기화: const 전역으로 정의되며 (변경되지 않음 - SPDK 가 내부에서 등록만 함),
 *         SPDK_SCHEDULER_REGISTER 가 constructor 시점에 lib/scheduler 의 등록 테이블에 추가.
 */
static struct spdk_scheduler scheduler = {
	.name = "static",                /* [한국어] 식별자 - rpc.py framework_set_scheduler --name static. */
	.init = init_static,             /* [한국어] 활성화 콜백 - period 설정. */
	.deinit = deinit_static,         /* [한국어] 비활성화 콜백 - g_first_load 갱신. */
	.balance = balance_static,       /* [한국어] thread→lcore 복원 콜백. */
	.set_opts = set_opts_static,     /* [한국어] mappings 옵션 처리 콜백. */
};
/* [한국어] SPDK_SCHEDULER_REGISTER 매크로:
 *  - constructor (__attribute__((constructor))) 시점에 본 스케줄러를 lib/scheduler 의
 *    전역 등록 리스트에 추가한다.
 *  - 사용자가 framework_set_scheduler 로 "static" 을 지정하면 spdk_scheduler_set("static")
 *    이 이 리스트에서 본 객체를 찾아 활성화한다.
 *  - 즉 본 .c 파일을 빌드에 포함시키기만 하면 자동으로 사용 가능해진다 (별도 init 함수 호출 불필요). */
SPDK_SCHEDULER_REGISTER(scheduler);
