/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 관리(state machine) 프레임워크 코어 구현 (ftl_mngt.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 ftl_mngt.h가 노출하는 process / step 상태 머신의 코어 엔진을 구현한다.
 * process는 step 시퀀스를 가진 비동기 상태 머신이며, 본 엔진은 다음을 책임진다:
 *  - process 메모리 할당 (allocate_mngt) 및 step 큐 초기화 (init_step)
 *  - action 큐(앞으로 실행할 step)와 rollback 큐(이미 실행된 step의 cleanup)를 별도 관리
 *  - 모든 step.action / step.cleanup을 dev->core_thread에서 spdk_thread_send_msg로 dispatch
 *  - next_step / fail_step / continue_step / skip_step API 처리
 *  - sub-process(call_process) 호출과 child 완료 콜백(child_cb) 처리
 *  - 정상 종료(finish_msg)와 실패 후 rollback 자동 트리거
 *  - step 타이밍/로그 출력 (trace_step, tsc_to_ms)
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 컨텍스트: process_execute는 임의 thread에서 호출 가능하지만, 모든 step.action /
 * cleanup은 dev->core_thread로 dispatch되어 lockless 운영. caller 콜백은 caller가
 * process_execute를 호출한 thread에서 실행 (caller.thread 보존).
 *   호출 흐름:
 *     ftl_mngt_process_execute → allocate_mngt + init_step×N + invoke_init_handler
 *       → action_execute → spdk_thread_send_msg(core_thread, action_msg)
 *       → core_thread: action_msg → step.action(dev, mngt)
 *       → step.action 안에서 ftl_mngt_next_step → action_next
 *       → action_done(현 step을 done 큐로 + cleanup이 있으면 rollback 큐 head에 push)
 *       → action_execute 반복
 *       → action 큐 비면 ftl_mngt_finish → caller.thread로 finish_msg dispatch
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/queue.h(TAILQ), spdk/thread.h(spdk_thread_send_msg, spdk_get_ticks),
 *   spdk/env.h, ftl_core.h(spdk_ftl_dev->core_thread, conf.name), ftl_mngt.h(API).
 * - 의존받음: lib/ftl/mngt/ 모든 step 파일 (ftl_mngt_*.c).
 * - 데이터 흐름: process가 동작하는 동안 dev 구조체에 데이터 누적 — 본 엔진은 dev에 직접
 *   쓰지 않고 step.action에 위임만.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ftl_mngt_step_status : step 단위 시간/상태 (start, stop, status, silent).
 * - struct ftl_mngt_step        : 런타임 step 인스턴스 (ctx, desc, action/rollback status).
 * - struct ftl_mngt_process     : 진행 중 process (4개 step 큐, caller 정보, dev, 등).
 * - get_current_step()          : action 또는 rollback 큐의 head step 반환.
 * - init_step()                 : 단일 step 인스턴스 alloc + ctx alloc + action 큐 push.
 * - free_mngt()                 : 모든 step과 ctx 해제, mngt 자체 해제.
 * - allocate_mngt()             : process 메모리 + 4개 큐 초기화.
 * - invoke_init_handler()       : process_desc.init_handler 호출 (있으면).
 * - _ftl_mngt_process_execute() : process_execute의 내부 구현.
 * - ftl_mngt_process_execute()/rollback() : 외부 진입점.
 * - get_dev/step_ctx/process_ctx/caller_ctx/alloc_step_ctx : 컨텍스트 접근 API.
 * - next/skip/continue/fail_step : 진행 제어 API.
 * - call_process(_rollback) / child_cb : sub-process 호출과 완료 처리.
 * - tsc_to_ms / trace_step       : 시간 측정과 로그.
 * - finish_msg / ftl_mngt_finish : process 정상 종료 및 caller cb 호출.
 * - action_next/msg/execute/done : action 큐 진행 엔진 4종.
 * - rollback_next/msg/execute/done : rollback 큐 진행 엔진 4종.
 */

#include "spdk/queue.h"
/* [한국어] TAILQ 매크로 — 양방향 리스트(step 큐)에 사용. */
#include "spdk/assert.h"
/* [한국어] SPDK assert 매크로. */
#include "spdk/env.h"
/* [한국어] spdk_get_ticks/ticks_hz — 시간 측정용. */

#include "ftl_mngt.h"
/* [한국어] 본 파일이 구현할 외부 API 정의. */
#include "ftl_core.h"
/* [한국어] spdk_ftl_dev (dev->core_thread, dev->conf.name) — dispatch 대상 thread. */

/*
 * [한국어]
 * struct ftl_mngt_step_status - 단일 step 실행 시 기록하는 상태/시간/큐 entry.
 *
 * 각 step은 action.status와 rollback.status를 따로 가지므로 본 구조체가 두 번 박힘.
 * TAILQ_ENTRY가 ftl_mngt_step을 가리키는 이유: action_queue / rollback_queue가 같은
 * step 객체를 다른 entry 필드로 동시에 가리킴 (DLL 노드 두 개로 두 큐 동시 멤버).
 */
struct ftl_mngt_step_status {
	uint64_t start;
	/* [한국어] step 진입 직전 spdk_get_ticks (TSC). 0이면 미시작.
	 * 설정자: action_msg/rollback_msg가 step.action 호출 직전 셋팅(0이면).
	 * 읽는 자: trace_step에서 duration 계산. */

	uint64_t stop;
	/* [한국어] step 완료 시점 TSC.
	 * 설정자: action_done/rollback_done. 읽는 자: trace_step. */

	int status;
	/* [한국어] step 결과 (0=성공, -1=실패).
	 * 설정자: action_done/rollback_done. 읽는 자: trace_step + caller status 결정. */

	int silent;
	/* [한국어] true면 trace_step이 로그 출력 생략 (rollback만 silent로 진행하는 경우 등).
	 * 설정자: skip_step / call_process / process_rollback 시 true 셋팅. */

	TAILQ_ENTRY(ftl_mngt_step) entry;
	/* [한국어] step을 큐에 묶는 양방향 링크. action.entry는 action_queue용,
	 * rollback.entry는 rollback_queue용. 한 step이 두 큐에 동시 등록 가능. */
};

/*
 * [한국어]
 * struct ftl_mngt_step - 런타임 step 인스턴스.
 *
 * desc(static const)로부터 ctx_size만큼 ctx를 calloc해 보유. 한 step은 action 큐와
 * rollback 큐에 동시에 들어갈 수 있어 두 entry 필드(action.entry / rollback.entry)를 가짐.
 */
struct ftl_mngt_step {
	void *ctx;
	/* [한국어] step ctx 메모리 — 첫 init_step에서 calloc(desc->ctx_size).
	 * step.action 안에서 ftl_mngt_alloc_step_ctx로 재할당 가능. */

	const struct ftl_mngt_step_desc *desc;
	/* [한국어] 정적 desc 포인터 (name, ctx_size, action, cleanup). */

	struct ftl_mngt_step_status action;
	/* [한국어] action 실행 시 시간/상태. action.entry로 action 큐에 묶임. */

	struct ftl_mngt_step_status rollback;
	/* [한국어] cleanup(rollback) 실행 시 시간/상태. rollback.entry로 rollback 큐에 묶임. */
};

/*
 * [한국어]
 * struct ftl_mngt_process - 진행 중인 process 한 인스턴스.
 *
 * 외부에서는 불투명 포인터(struct ftl_mngt_process *)로만 접근.
 */
struct ftl_mngt_process {
	struct spdk_ftl_dev *dev;
	/* [한국어] 대상 디바이스. step.action에 (dev, mngt)로 전달. */

	int status;
	/* [한국어] process 누적 status. fail_step에서 -1로. caller cb의 status로 전달. */

	bool silent;
	/* [한국어] sub-process / rollback이면 true — 시작/끝 로그 생략. */

	bool rollback;
	/* [한국어] true면 현재 rollback 모드 (action 대신 rollback 큐 처리). */

	bool continuing;
	/* [한국어] continue_step 중복 호출 방지 플래그.
	 * (한 step.action 안에서 continue_step을 두 번 호출해도 한 번만 dispatch). */

	struct  {
		ftl_mngt_completion cb;
		/* [한국어] 호출자 콜백. process 종료 시 caller.thread에서 실행. */
		void *cb_ctx;
		/* [한국어] 콜백 인자. */
		struct spdk_thread *thread;
		/* [한국어] process_execute 호출자의 thread — finish 시 메시지 dispatch 대상. */
	} caller;

	void *ctx;
	/* [한국어] process ctx — process_desc.ctx_size로 자동 할당.
	 * step에서 ftl_mngt_get_process_ctx로 접근. */

	uint64_t tsc_start;
	/* [한국어] process 시작 시점 TSC. */
	uint64_t tsc_stop;
	/* [한국어] process 종료 시점 TSC. */

	const struct ftl_mngt_process_desc *desc;
	/* [한국어] 정적 process desc. */

	TAILQ_HEAD(, ftl_mngt_step) action_queue_todo;
	/* [한국어] 앞으로 실행할 step 큐. init_step에서 INSERT_TAIL, action_done에서 REMOVE. */
	TAILQ_HEAD(, ftl_mngt_step) action_queue_done;
	/* [한국어] 이미 실행된 action들의 보관 큐. free_mngt에서 일괄 free. */
	TAILQ_HEAD(, ftl_mngt_step) rollback_queue_todo;
	/* [한국어] cleanup이 있는 step들. action_done에서 INSERT_HEAD해 자동으로 역순 정렬됨. */
	TAILQ_HEAD(, ftl_mngt_step) rollback_queue_done;
	/* [한국어] 이미 cleanup된 step 큐. */

	struct {
		struct ftl_mngt_step step;
		struct ftl_mngt_step_desc desc;
	} cleanup;
	/* [한국어] process_desc.error_handler용 가짜 step (name="Handle ERROR").
	 * fail 발생 시 rollback 큐에서 마지막에 실행되어 추가 cleanup 수행. */

	struct ftl_mng_tracer *tracer;
	/* [한국어] tracer hook (현재 미사용 — 향후 trace 확장용). */
};

static void action_next(struct ftl_mngt_process *mngt);
static void action_msg(void *ctx);
static void action_execute(struct ftl_mngt_process *mngt);
static void action_done(struct ftl_mngt_process *mngt, int status);
static void rollback_next(struct ftl_mngt_process *mngt);
static void rollback_msg(void *ctx);
static void rollback_execute(struct ftl_mngt_process *mngt);
static void rollback_done(struct ftl_mngt_process *mngt, int status);
/* [한국어] action / rollback 엔진 4종의 forward 선언. 서로 호출하므로 미리 선언. */

/*
 * [한국어]
 * get_current_step - 현재 처리 중인 step 반환 (rollback 모드에 따라 다른 큐).
 */
static inline struct ftl_mngt_step *
get_current_step(struct ftl_mngt_process *mngt)
{
	if (!mngt->rollback) {
		return TAILQ_FIRST(&mngt->action_queue_todo);
	} else {
		return TAILQ_FIRST(&mngt->rollback_queue_todo);
	}
}

/*
 * [한국어]
 * init_step - 단일 step 인스턴스 alloc 후 action 큐에 INSERT_TAIL.
 *
 * @mngt: 대상 process. @desc: step desc.
 * @return: 0=성공, -ENOMEM=실패.
 *
 * desc.ctx_size > 0이면 step->ctx도 calloc.
 */
static int
init_step(struct ftl_mngt_process *mngt,
	  const struct ftl_mngt_step_desc *desc)
{
	struct ftl_mngt_step *step;

	step = calloc(1, sizeof(*step));
	if (!step) {
		return -ENOMEM;
	}

	/* Initialize the step's argument */
	if (desc->ctx_size) {
		step->ctx = calloc(1, desc->ctx_size);
		if (!step->ctx) {
			free(step);
			return -ENOMEM;
		}
	}
	step->desc = desc;
	TAILQ_INSERT_TAIL(&mngt->action_queue_todo, step, action.entry);
	/* [한국어] 처음에는 모든 step이 action 큐 todo에 순서대로 들어감. */

	return 0;
}

/*
 * [한국어]
 * free_mngt - process가 보유한 모든 step과 ctx 해제 후 mngt 자체 해제.
 *
 * action_queue_todo + action_queue_done의 모든 step을 잔여 정리. rollback 큐는 done이
 * 별개지만 같은 step 객체를 공유하므로 별도 free 안 함.
 */
static void
free_mngt(struct ftl_mngt_process *mngt)
{
	TAILQ_HEAD(, ftl_mngt_step) steps;

	if (!mngt) {
		return;
	}

	TAILQ_INIT(&steps);
	TAILQ_CONCAT(&steps, &mngt->action_queue_todo, action.entry);
	TAILQ_CONCAT(&steps, &mngt->action_queue_done, action.entry);
	/* [한국어] 두 큐를 합쳐 한 번에 순회. */

	while (!TAILQ_EMPTY(&steps)) {
		struct ftl_mngt_step *step = TAILQ_FIRST(&steps);
		TAILQ_REMOVE(&steps, step, action.entry);

		free(step->ctx);
		free(step);
	}

	free(mngt->ctx);
	free(mngt);
}

/*
 * [한국어]
 * allocate_mngt - process 메모리 + ctx + 4 큐 초기화. (step은 별도 init_step에서.)
 *
 * @silent: sub-process / rollback이면 true → 시작/끝 로그 생략.
 */
static struct ftl_mngt_process *
allocate_mngt(struct spdk_ftl_dev *dev, const struct ftl_mngt_process_desc *pdesc,
	      ftl_mngt_completion cb, void *cb_ctx, bool silent)
{
	struct ftl_mngt_process *mngt;

	/* Initialize management process */
	mngt = calloc(1, sizeof(*mngt));
	if (!mngt) {
		goto error;
	}
	mngt->dev = dev;
	mngt->silent = silent;
	mngt->caller.cb = cb;
	mngt->caller.cb_ctx = cb_ctx;
	mngt->caller.thread = spdk_get_thread();
	/* [한국어] 호출자 thread 보존 — 완료 시 콜백을 이 thread에서 실행. */

	/* Initialize process context */
	if (pdesc->ctx_size) {
		mngt->ctx = calloc(1, pdesc->ctx_size);
		if (!mngt->ctx) {
			goto error;
		}
	}
	mngt->tsc_start = spdk_get_ticks();
	mngt->desc = pdesc;
	TAILQ_INIT(&mngt->action_queue_todo);
	TAILQ_INIT(&mngt->action_queue_done);
	TAILQ_INIT(&mngt->rollback_queue_todo);
	TAILQ_INIT(&mngt->rollback_queue_done);

	return mngt;
error:
	free_mngt(mngt);
	return NULL;
}

/*
 * [한국어]
 * invoke_init_handler - process_desc.init_handler가 있으면 호출.
 *
 * init_ctx와 init_handler는 always 함께 (둘 중 하나만 NULL이면 ftl_bug = abort).
 * caller가 둘 다 NULL로 호출했으면 안전하게 0 반환.
 */
static int
invoke_init_handler(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
		    const struct ftl_mngt_process_desc *pdesc, void *init_ctx)
{
	int rc = 0;

	if (init_ctx || pdesc->init_handler) {
		ftl_bug(!init_ctx);
		ftl_bug(!pdesc->init_handler);
		/* [한국어] 둘 중 하나만 셋팅된 케이스는 사용자 실수 — assert로 abort. */
		rc = pdesc->init_handler(dev, mngt, init_ctx);
	}

	return rc;
}

/*
 * [한국어]
 * _ftl_mngt_process_execute - process_execute / call_process의 공통 내부 구현.
 *
 * 단계:
 *  1) allocate_mngt.
 *  2) error_handler가 있으면 가짜 step(cleanup.step)을 rollback 큐 head에 push.
 *  3) desc.steps 배열을 순회해 init_step(action 큐 INSERT_TAIL).
 *  4) invoke_init_handler.
 *  5) action_execute 트리거 (코어 스레드로 dispatch).
 *
 * 실패 시 free_mngt로 모두 정리 후 음수 errno 반환.
 */
static int
_ftl_mngt_process_execute(struct spdk_ftl_dev *dev, const struct ftl_mngt_process_desc *pdesc,
			  ftl_mngt_completion cb, void *cb_ctx, bool silent, void *init_ctx)
{
	const struct ftl_mngt_step_desc *sdesc;
	struct ftl_mngt_process *mngt;
	int rc = 0;

	mngt = allocate_mngt(dev, pdesc, cb, cb_ctx, silent);
	if (!mngt) {
		rc = -ENOMEM;
		goto error;
	}

	if (pdesc->error_handler) {
		/* Initialize a step for error handler */
		/* [한국어] error_handler를 마치 cleanup인 것처럼 rollback 큐에 등록.
		 * fail 시 rollback 진행 끝에 마지막으로 실행되어 추가 정리 수행. */
		mngt->cleanup.step.desc = &mngt->cleanup.desc;
		mngt->cleanup.desc.name = "Handle ERROR";
		mngt->cleanup.desc.cleanup = pdesc->error_handler;

		/* Queue error handler to the rollback queue, it will be executed at the end */
		/* [한국어] HEAD에 push하면 rollback 진행 시 다른 step cleanup이 모두 끝난 뒤
		 * 마지막에 본 가짜 step이 처리된다(INSERT_HEAD 패턴이 역순 cleanup의 핵심). */
		TAILQ_INSERT_HEAD(&mngt->rollback_queue_todo, &mngt->cleanup.step,
				  rollback.entry);
	}

	/* Initialize steps */
	sdesc = mngt->desc->steps;
	while (sdesc->action) {
		/* [한국어] sentinel(action == NULL)까지 step 인스턴스 생성. */
		rc = init_step(mngt, sdesc);
		if (rc) {
			goto error;
		}
		sdesc++;
	}

	rc = invoke_init_handler(dev, mngt, pdesc, init_ctx);
	if (rc) {
		goto error;
	}

	action_execute(mngt);
	/* [한국어] 첫 step 실행을 코어 스레드로 dispatch. */
	return 0;
error:
	free_mngt(mngt);
	return rc;
}

/*
 * [한국어]
 * ftl_mngt_process_execute - 외부 진입 wrapper. silent=false, init_ctx=NULL.
 */
int
ftl_mngt_process_execute(struct spdk_ftl_dev *dev, const struct ftl_mngt_process_desc *pdesc,
			 ftl_mngt_completion cb, void *cb_ctx)
{
	return _ftl_mngt_process_execute(dev, pdesc, cb, cb_ctx, false, NULL);
}

/*
 * [한국어]
 * ftl_mngt_process_rollback - process를 rollback 모드로 시작.
 *
 * cleanup이 정의된 step만 인스턴스화한 뒤 rollback 큐에 INSERT_HEAD로 역순 정렬해
 * cleanup 함수만 역순 호출. action은 호출 안 됨.
 *
 * 사용 예: shutdown desc 끝의 ftl_mngt_rollback_device가 startup desc의 cleanup만
 * 역순 실행해 startup이 만든 자원을 일괄 해제.
 */
int
ftl_mngt_process_rollback(struct spdk_ftl_dev *dev, const struct ftl_mngt_process_desc *pdesc,
			  ftl_mngt_completion cb, void *cb_ctx)
{
	const struct ftl_mngt_step_desc *sdesc;
	struct ftl_mngt_process *mngt;
	int rc = 0;

	mngt = allocate_mngt(dev, pdesc, cb, cb_ctx, true);
	if (!mngt) {
		rc = -ENOMEM;
		goto error;
	}

	/* Initialize steps for rollback */
	sdesc = mngt->desc->steps;
	while (sdesc->action) {
		if (!sdesc->cleanup) {
			/* [한국어] cleanup 없는 step은 rollback에서 의미 없음 — skip. */
			sdesc++;
			continue;
		}
		rc = init_step(mngt, sdesc);
		if (rc) {
			goto error;
		}
		sdesc++;
	}

	/* Build rollback list */
	struct ftl_mngt_step *step;
	TAILQ_FOREACH(step, &mngt->action_queue_todo, action.entry) {
		step->action.silent = true;
		/* [한국어] rollback 모드에서 action은 실행 안 하니 silent로. */
		TAILQ_INSERT_HEAD(&mngt->rollback_queue_todo, step,
				  rollback.entry);
		/* [한국어] HEAD에 push하면 desc 순서의 역순으로 큐가 만들어짐. */
	}

	mngt->rollback = true;
	rollback_execute(mngt);
	return 0;
error:
	free_mngt(mngt);
	return rc;
}

/*
 * [한국어]
 * ftl_mngt_get_dev - mngt에서 dev 포인터 추출.
 */
struct spdk_ftl_dev *
ftl_mngt_get_dev(struct ftl_mngt_process *mngt)
{
	return mngt->dev;
}

/*
 * [한국어]
 * ftl_mngt_alloc_step_ctx - 현재 step의 ctx를 새 size로 재할당 (이전 ctx free).
 *
 * realloc이 아니라 free + new calloc — 데이터 보존 안 됨. 호출자는 새로 채워야 함.
 */
int
ftl_mngt_alloc_step_ctx(struct ftl_mngt_process *mngt, size_t size)
{
	struct ftl_mngt_step *step = get_current_step(mngt);
	void *arg = calloc(1, size);

	if (!arg) {
		return -ENOMEM;
	}

	free(step->ctx);
	step->ctx = arg;

	return 0;
}

/*
 * [한국어]
 * ftl_mngt_get_step_ctx - 현재 step의 ctx 포인터 반환.
 */
void *
ftl_mngt_get_step_ctx(struct ftl_mngt_process *mngt)
{
	return get_current_step(mngt)->ctx;
}

/*
 * [한국어]
 * ftl_mngt_get_process_ctx - process ctx (process_desc.ctx_size로 자동 할당) 포인터.
 */
void *
ftl_mngt_get_process_ctx(struct ftl_mngt_process *mngt)
{
	return mngt->ctx;
}

/*
 * [한국어]
 * ftl_mngt_get_caller_ctx - caller가 process_execute에 넘긴 cb_ctx 그대로 반환.
 *
 * sub-process의 step에서 부모 데이터에 접근할 때 사용 (call_process 시 child_cb에
 * mngt가 들어가므로 부모 ctx에는 별도 init_ctx로 접근).
 */
void *
ftl_mngt_get_caller_ctx(struct ftl_mngt_process *mngt)
{
	return mngt->caller.cb_ctx;
}

/*
 * [한국어]
 * ftl_mngt_next_step - 현재 step 성공 종료, 다음 step으로.
 *
 * rollback 모드면 rollback_next, 아니면 action_next로 dispatch.
 */
void
ftl_mngt_next_step(struct ftl_mngt_process *mngt)
{
	if (false == mngt->rollback) {
		action_next(mngt);
	} else {
		rollback_next(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_skip_step - 현재 step을 silent로 표시하고 next로.
 *
 * silent로 표시되면 trace_step이 로그 출력 생략. 동작상 next_step과 같음.
 */
void
ftl_mngt_skip_step(struct ftl_mngt_process *mngt)
{
	if (mngt->rollback) {
		get_current_step(mngt)->rollback.silent = true;
	} else {
		get_current_step(mngt)->action.silent = true;
	}
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_continue_step - 같은 step.action을 다시 호출하도록 예약.
 *
 * polling 패턴(예: layout_upgrade가 region 하나씩 처리), batch 처리(self_test의 4096 LBA
 * 단위 pin) 등에 사용. 같은 step.action 안에서 두 번 호출해도 한 번만 dispatch
 * (continuing 플래그).
 */
void
ftl_mngt_continue_step(struct ftl_mngt_process *mngt)
{

	if (!mngt->continuing) {
		if (false == mngt->rollback) {
			action_execute(mngt);
		} else {
			rollback_execute(mngt);
		}
	}

	mngt->continuing = true;
	/* [한국어] action_msg/rollback_msg 진입 시 false로 리셋되어 다음 step부터는 다시 동작. */
}

/*
 * [한국어]
 * child_cb - sub-process 완료 콜백. parent에 결과 전파.
 *
 * @ctx: parent ftl_mngt_process. @status: child 결과.
 */
static void
child_cb(struct spdk_ftl_dev *dev, void *ctx, int status)
{
	struct ftl_mngt_process *parent = ctx;

	if (status) {
		ftl_mngt_fail_step(parent);
	} else {
		ftl_mngt_next_step(parent);
	}
}

/*
 * [한국어]
 * ftl_mngt_call_process - 현재 step 안에서 sub-process를 호출.
 *
 * @mngt:     부모 mngt 핸들.
 * @pdesc:    sub-process desc.
 * @init_ctx: sub-process init_handler에 전달할 인자 (없으면 NULL).
 *
 * 동작:
 *  - silent=true로 sub-process 시작. child_cb를 caller 콜백으로 등록 (parent를 ctx로 전달).
 *  - sub-process 시작 자체가 실패하면 부모 step을 fail.
 *  - 성공 시 부모 현재 step을 silent로 마킹 (sub-process가 진행하는 동안 부모 step 로그 생략).
 */
void
ftl_mngt_call_process(struct ftl_mngt_process *mngt,
		      const struct ftl_mngt_process_desc *pdesc,
		      void *init_ctx)
{
	if (_ftl_mngt_process_execute(mngt->dev, pdesc, child_cb, mngt, true, init_ctx)) {
		ftl_mngt_fail_step(mngt);
	} else {
		if (mngt->rollback) {
			get_current_step(mngt)->rollback.silent = true;
		} else {
			get_current_step(mngt)->action.silent = true;
		}
	}
}

/*
 * [한국어]
 * ftl_mngt_call_process_rollback - sub-process를 rollback 모드로 호출.
 *
 * call_process와 동일하지만 process_rollback을 사용 — sub-process의 cleanup만 역순 실행.
 */
void
ftl_mngt_call_process_rollback(struct ftl_mngt_process *mngt,
			       const struct ftl_mngt_process_desc *pdesc)
{
	if (ftl_mngt_process_rollback(mngt->dev, pdesc, child_cb, mngt)) {
		ftl_mngt_fail_step(mngt);
	} else {
		if (mngt->rollback) {
			get_current_step(mngt)->rollback.silent = true;
		} else {
			get_current_step(mngt)->action.silent = true;
		}
	}
}

/*
 * [한국어]
 * ftl_mngt_fail_step - 현재 step 실패 처리 — 후속 step 중단, rollback 트리거.
 *
 * 동작:
 *  1) mngt->status = -1.
 *  2) 현재 단계가 action이면 action_done(-1), rollback이면 rollback_done(-1)로
 *     현재 step을 done 큐로 옮기고 시간/상태 기록.
 *  3) rollback 모드로 전환.
 *  4) rollback_execute로 rollback 큐 진행 시작.
 *
 * cleanup 안에서 fail_step을 호출하면 그 cleanup만 중단되고 다음 cleanup이 계속 진행됨
 * (rollback_done이 한 번 더 호출되어 다음 step으로 이동).
 */
void
ftl_mngt_fail_step(struct ftl_mngt_process *mngt)
{
	mngt->status = -1;

	if (false == mngt->rollback) {
		action_done(mngt, -1);
	} else {
		rollback_done(mngt, -1);
	}

	mngt->rollback = true;
	rollback_execute(mngt);
}

/*
 * [한국어]
 * tsc_to_ms - TSC 값을 millisecond float으로 변환.
 */
static inline float
tsc_to_ms(uint64_t tsc)
{
	float ms = tsc;
	ms /= (float)spdk_get_ticks_hz();
	/* [한국어] tsc_hz는 부팅 시 측정된 TSC tick 빈도(Hz). */
	ms *= 1000.0;
	return ms;
}

/*
 * [한국어]
 * trace_step - step 단위 로그 출력 (이름 / 소요 시간 / 결과).
 *
 * @rollback: true면 rollback 큐 처리 중 — silent 플래그도 별도 보유.
 *
 * silent면 출력 생략 (skip된 step / call_process 부모 step 등).
 */
static void
trace_step(struct spdk_ftl_dev *dev, struct ftl_mngt_step *step, bool rollback)
{
	uint64_t duration;
	const char *what = rollback ? "Rollback" : "Action";
	int silent = rollback ? step->rollback.silent : step->action.silent;

	if (silent) {
		return;
	}

	FTL_NOTICELOG(dev, "%s\n", what);
	FTL_NOTICELOG(dev, "\t name:     %s\n", step->desc->name);
	duration = step->action.stop - step->action.start;
	/* [한국어] 주의: rollback 모드에서도 action.* 필드의 시간을 출력 (rollback.* 시간이
	 * 별도로 있으나 trace 코드는 통일된 출력을 위해 action.* 사용 — 약간의 버그 가능성). */
	FTL_NOTICELOG(dev, "\t duration: %.3f ms\n", tsc_to_ms(duration));
	FTL_NOTICELOG(dev, "\t status:   %d\n", step->action.status);
}

/*
 * [한국어]
 * finish_msg - process 종료 시 caller.thread에서 실행되는 cleanup + caller cb.
 *
 * 동작:
 *  1) silent 아니면 dev->conf.name 임시 복사 (caller cb가 dev_free 해버릴 수 있어서).
 *  2) caller.cb(dev, cb_ctx, status) 호출 — caller가 결과 받음.
 *  3) deinit_handler 있으면 호출.
 *  4) silent 아니면 종료 로그 출력 (이름 / 소요 / status).
 *  5) free_mngt + devname free.
 */
static void
finish_msg(void *ctx)
{
	struct ftl_mngt_process *mngt = ctx;
	char *devname = NULL;

	if (!mngt->silent && mngt->dev->conf.name) {
		/* the callback below can free the device so make a temp copy of the name */
		/* [한국어] caller cb가 spdk_ftl_dev_free를 호출해 dev를 해제할 수 있으므로
		 * 로그 출력에 쓸 이름을 미리 복사. */
		devname = strdup(mngt->dev->conf.name);
	}

	mngt->caller.cb(mngt->dev, mngt->caller.cb_ctx, mngt->status);
	/* [한국어] caller에 process 종료 통지 — 이 호출이 dev를 free할 수도 있다. */

	if (mngt->desc->deinit_handler) {
		mngt->desc->deinit_handler(mngt->dev, mngt);
	}

	if (!mngt->silent) {
		/* TODO: refactor the logging macros to pass just the name instead of device */
		struct spdk_ftl_dev tmpdev = {
			.conf = {
				.name = devname
			}
		};
		/* [한국어] dev가 free됐을 가능성 — 임시 stack dev로 로그용 가짜 디바이스 생성. */

		FTL_NOTICELOG(&tmpdev, "Management process finished, name '%s', duration = %.3f ms, result %d\n",
			      mngt->desc->name,
			      tsc_to_ms(mngt->tsc_stop - mngt->tsc_start),
			      mngt->status);
	}
	free_mngt(mngt);
	free(devname);
}

/*
 * [한국어]
 * ftl_mngt_finish - process 종료 트리거. caller.thread로 finish_msg dispatch.
 */
void
ftl_mngt_finish(struct ftl_mngt_process *mngt)
{
	mngt->tsc_stop = spdk_get_ticks();
	spdk_thread_send_msg(mngt->caller.thread, finish_msg, mngt);
}

/*
 * Actions
 */
/*
 * [한국어]
 * action_next - 현재 step 성공 처리 후 다음 step 진행 (또는 종료).
 */
static void
action_next(struct ftl_mngt_process *mngt)
{
	if (TAILQ_EMPTY(&mngt->action_queue_todo)) {
		/* Nothing to do, finish the management process */
		/* [한국어] 모든 step 완료 — process 정상 종료. */
		ftl_mngt_finish(mngt);
		return;
	} else {
		action_done(mngt, 0);
		/* [한국어] 현재 step을 done 큐로 이동 + cleanup 있으면 rollback 큐 push. */
		action_execute(mngt);
		/* [한국어] 다음 step.action을 코어 스레드로 dispatch. */
	}
}

/*
 * [한국어]
 * action_msg - 코어 스레드에서 실행되는 step.action 호출 wrapper.
 *
 * @ctx: ftl_mngt_process.
 *
 * 동작:
 *  1) continuing 플래그 false로 리셋.
 *  2) action_queue_todo 비었으면 finish.
 *  3) head step 시작 시간 (start) 기록(0이면 — continue_step 케이스는 0이 아닐 수 있음).
 *  4) step.action(dev, mngt) 호출 — 결과는 step 안에서 next/fail/skip/continue로 신호.
 */
static void
action_msg(void *ctx)
{
	struct ftl_mngt_process *mngt = ctx;
	struct ftl_mngt_step *step;

	mngt->continuing = false;

	if (TAILQ_EMPTY(&mngt->action_queue_todo)) {
		ftl_mngt_finish(mngt);
		return;
	}

	step = TAILQ_FIRST(&mngt->action_queue_todo);
	if (!step->action.start) {
		/* [한국어] 첫 진입 — 시작 시간 기록. continue_step으로 재진입 시는 보존. */
		step->action.start = spdk_get_ticks();
	}
	step->desc->action(mngt->dev, mngt);
}

/*
 * [한국어]
 * action_execute - step.action 실행을 dev->core_thread로 dispatch.
 *
 * 모든 step.action이 코어 스레드에서 실행되도록 강제 — lockless 운영의 핵심.
 */
static void
action_execute(struct ftl_mngt_process *mngt)
{
	spdk_thread_send_msg(mngt->dev->core_thread, action_msg, mngt);
}

/*
 * [한국어]
 * action_done - 현재 step을 action_queue_todo에서 done 큐로 이동 + 시간/상태 기록.
 *
 * cleanup이 정의된 step이면 rollback 큐 head에 INSERT_HEAD해 자동으로 역순 정렬됨
 * (LIFO — 마지막 실행된 step이 가장 먼저 cleanup됨).
 */
static void
action_done(struct ftl_mngt_process *mngt, int status)
{
	struct ftl_mngt_step *step;

	assert(!TAILQ_EMPTY(&mngt->action_queue_todo));
	step = TAILQ_FIRST(&mngt->action_queue_todo);
	TAILQ_REMOVE(&mngt->action_queue_todo, step, action.entry);

	TAILQ_INSERT_TAIL(&mngt->action_queue_done, step, action.entry);
	if (step->desc->cleanup) {
		TAILQ_INSERT_HEAD(&mngt->rollback_queue_todo, step,
				  rollback.entry);
		/* [한국어] HEAD push로 LIFO 순서 — 가장 최근 실행된 step이 가장 먼저 cleanup됨. */
	}

	step->action.stop = spdk_get_ticks();
	step->action.status = status;

	trace_step(mngt->dev, step, false);
}

/*
 * Rollback
 */
/*
 * [한국어]
 * rollback_next - rollback 진행: 현재 cleanup 성공 처리 후 다음 cleanup으로.
 */
static void
rollback_next(struct ftl_mngt_process *mngt)
{
	if (TAILQ_EMPTY(&mngt->rollback_queue_todo)) {
		/* Nothing to do, finish the management process */
		/* [한국어] 모든 rollback 완료 — process 종료. */
		ftl_mngt_finish(mngt);
		return;
	} else {
		rollback_done(mngt, 0);
		rollback_execute(mngt);
	}
}

/*
 * [한국어]
 * rollback_msg - 코어 스레드에서 실행되는 step.cleanup 호출 wrapper.
 *
 * action_msg와 동일한 패턴이지만 step.desc->cleanup을 호출.
 */
static void
rollback_msg(void *ctx)
{
	struct ftl_mngt_process *mngt = ctx;
	struct ftl_mngt_step *step;

	mngt->continuing = false;

	if (TAILQ_EMPTY(&mngt->rollback_queue_todo)) {
		ftl_mngt_finish(mngt);
		return;
	}

	step = TAILQ_FIRST(&mngt->rollback_queue_todo);
	if (!step->rollback.start) {
		step->rollback.start = spdk_get_ticks();
	}
	step->desc->cleanup(mngt->dev, mngt);
}

/*
 * [한국어]
 * rollback_execute - cleanup 실행을 dev->core_thread로 dispatch.
 */
static void
rollback_execute(struct ftl_mngt_process *mngt)
{
	spdk_thread_send_msg(mngt->dev->core_thread, rollback_msg, mngt);
}

/*
 * [한국어]
 * rollback_done - 현재 cleanup step을 rollback_queue_todo → done으로 이동 + 시간 기록.
 */
void
rollback_done(struct ftl_mngt_process *mngt, int status)
{
	struct ftl_mngt_step *step;

	assert(!TAILQ_EMPTY(&mngt->rollback_queue_todo));
	step = TAILQ_FIRST(&mngt->rollback_queue_todo);
	TAILQ_REMOVE(&mngt->rollback_queue_todo, step, rollback.entry);
	TAILQ_INSERT_TAIL(&mngt->rollback_queue_done, step, rollback.entry);

	step->rollback.stop = spdk_get_ticks();
	step->rollback.status = status;

	trace_step(mngt->dev, step,  true);
}
