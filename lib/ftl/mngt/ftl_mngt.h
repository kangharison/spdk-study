/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 관리(state machine) 핵심 API 헤더 (ftl_mngt.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 FTL의 상태 머신 프레임워크 전체를 정의한다 — process(여러 step의 시퀀스)와
 * step(단일 작업 단위)의 디스크립터, 콜백 시그니처, 진행 제어 함수
 * (next/skip/continue/fail), 컨텍스트 접근 함수, 그리고 외부에서 호출하는 디바이스
 * 라이프사이클 진입점(startup/shutdown/trim)을 노출한다. 모든 FTL 라이프사이클 동작
 * (init, recovery, shutdown, upgrade, trim 등)은 이 프레임워크 위에서 step 배열로
 * 선언적으로 표현된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 본 헤더는 lib/ftl/mngt/ 디렉토리 모든 .c 파일에 #include되며, ftl_core.c (외부 진입),
 * lib/ftl/upgrade/, ftl_internal.h를 통한 내부 호출 등에서도 참조한다.
 *   외부 호출 패턴:
 *     spdk_ftl_dev_init  → ftl_mngt_call_dev_startup → process_execute(desc_startup)
 *     spdk_ftl_dev_free  → ftl_mngt_call_dev_shutdown → process_execute(desc_shutdown)
 *     spdk_ftl_unmap     → ftl_mngt_trim → process_execute(desc_trim)
 *   내부 step 함수에서:
 *     ftl_mngt_get_dev / get_step_ctx / get_process_ctx 로 dev/state 접근
 *     ftl_mngt_next_step / fail_step / skip_step / continue_step 으로 진행 제어
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h, spdk/ftl.h (spdk_ftl_dev forward, spdk_ftl_fn 콜백 타입).
 * - 의존받음: lib/ftl/ 전체 디렉토리(ftl_core.c, mngt/*, upgrade/*, recovery 등).
 * - 데이터 흐름: 호출자가 ftl_mngt_completion 콜백을 등록 → 모든 step이 끝나면(또는
 *   실패 후 rollback이 끝나면) 콜백이 dev/cb_ctx/status로 호출되어 결과 통보.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ftl_mngt_step_desc    : 단일 step 설명자 (name, ctx_size, action, cleanup).
 * - struct ftl_mngt_process_desc : process 설명자 (name, ctx_size, init/deinit/error
 *   handler, steps[]).
 * - typedef ftl_mngt_fn          : step action/cleanup 시그니처.
 * - typedef ftl_mngt_init_fn     : process 초기화 핸들러 시그니처.
 * - typedef ftl_mngt_completion  : caller 완료 콜백 시그니처.
 * - ftl_mngt_process_execute()/rollback(): process 실행/롤백 트리거.
 * - ftl_mngt_get_dev/step_ctx/process_ctx/caller_ctx(): step 안에서 컨텍스트 접근.
 * - ftl_mngt_next/skip/continue/fail_step()/finish(): step 진행 제어.
 * - ftl_mngt_call_process(_rollback)(): step 안에서 다른 process를 sub-process로 호출.
 * - ftl_mngt_call_dev_startup/shutdown(): 외부 진입점.
 * - ftl_mngt_trim()              : trim 외부 진입점.
 */

#ifndef FTL_MNGT_H
#define FTL_MNGT_H

#include "spdk/stdinc.h"
/* [한국어] 표준 C/POSIX 정의(uint, size_t 등). */
#include "spdk/ftl.h"
/* [한국어] FTL 공개 API — spdk_ftl_dev forward 선언, spdk_ftl_fn 콜백 시그니처. */

struct spdk_ftl_dev;
/* [한국어] FTL 디바이스 — 정의는 lib/ftl/ftl_core.h. 본 헤더는 포인터만 사용. */
struct ftl_mngt_process;
/* [한국어] 진행 중 process의 핸들 — 내부 정의는 lib/ftl/mngt/ftl_mngt.c.
 * 외부에서는 불투명 포인터로만 다룸. */

/**
 * The FTL management callback function
 *
 * @param dev FTL device
 * @param mngt FTL management handle
 */
/*
 * [한국어]
 * ftl_mngt_fn - step의 action 또는 cleanup으로 등록되는 함수의 시그니처.
 *
 * 모든 step은 이 시그니처를 가지며, dev로 디바이스 상태 접근, mngt로 진행 제어
 * (next/fail/skip/continue) 호출 가능. 비동기 동작이라면 즉시 반환하고 콜백에서
 * 진행 제어를 수행해도 됨.
 */
typedef void (*ftl_mngt_fn)(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/**
 * The FTL management init function
 *
 * @param dev FTL device
 * @param mngt FTL management handle
 * @param init_ctx The initialization context
 *
 * @return Initialization status
 * @retval 0 initialization successful, the process can be executed
 * @retval non-zero an error occurred during initialization, fail the process
 */
/*
 * [한국어]
 * ftl_mngt_init_fn - process 시작 직전 한 번 호출되는 초기화 핸들러 시그니처.
 *
 * caller가 init_ctx를 process_execute에 넘기면 본 함수가 process_ctx에 정보를 복사하거나
 * 추가 자원을 할당. 실패 시 비-0 반환 → process 자체가 시작되지 않고 에러로 종료.
 */
typedef int (*ftl_mngt_init_fn)(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
				void *init_ctx);

/**
 * The FTL management process completion callback function
 *
 * @param dev FTL device
 * @param ctx Caller context
 * @param status The operation result of the management process
 */
/*
 * [한국어]
 * ftl_mngt_completion - 외부 호출자에게 process 완료를 통지하는 콜백 시그니처.
 *
 * @dev: 디바이스, @ctx: caller가 process_execute에 넘긴 cb_ctx, @status: 0=성공/음수=실패.
 * 모든 step이 끝나거나 실패 후 rollback이 끝난 시점에 호출됨.
 * 실행 컨텍스트: 일반적으로 코어 스레드(또는 process를 시작한 thread).
 */
typedef void (*ftl_mngt_completion)(struct spdk_ftl_dev *dev, void *ctx, int status);

/**
 * The FTL management step descriptor
 */
/*
 * [한국어]
 * struct ftl_mngt_step_desc - process 안의 단일 step을 기술하는 디스크립터.
 *
 * process_desc.steps[] 배열의 원소로 등장. action == NULL이면 배열 종단(sentinel).
 */
struct ftl_mngt_step_desc {
	/**
	 * Name of the step
	 */
	const char *name;
	/* [한국어] 디버그/로그용 step 이름. 진입/완료/실패 시 로그에 출력됨. */

	/**
	 * Size of the step argument (context)
	 *
	 * The step context will be allocated before execution of step's
	 * callback.
	 *
	 * @note The context can be reallocated (freed and newly allocated
	 * when calling ftl_mngt_alloc_step_ctx). The main usage is the ability
	 * to set this value to 0 and only allocate as needed if the step is
	 * going to be extremely similar - eg. recovery from shared memory and
	 * disk - in case of shm all the data is already available in memory, while
	 * recovery from disk needs extra context to be able to synchronize IO. This
	 * allows for saving a little bit of time on alloc/dealloc in the cases where
	 * execution time may be critical.
	 * @note It doesn't work like realloc
	 * @note The context can be retrieved within callback when calling
	 * ftl_mngt_get_step_ctx
	 */
	size_t ctx_size;
	/* [한국어] step에 자동 할당되는 보조 컨텍스트 메모리 크기.
	 * 0이면 미할당 — 필요 시 step action 안에서 ftl_mngt_alloc_step_ctx로 동적 할당.
	 * 설정자: 정적 desc 선언. 읽는 자: 프레임워크가 step 진입 직전 calloc.
	 * 값 범위: 0 ~ 임의(보통 ftl_mngt_p2l_md_ctx 같이 수십~수백 바이트). */

	/**
	 * Step callback function
	 */
	ftl_mngt_fn action;
	/* [한국어] step의 본 동작 함수. 진행/실패/skip은 step 안에서 진행 제어 API로 신호.
	 * NULL이면 배열 sentinel(다음 step 없음). */

	/**
	 * It the step requires cleanup this is right place to put your handler.
	 * When a FTL management process fails cleanup callbacks are executed
	 * in rollback procedure. Cleanup functions are executed in reverse
	 * order to actions already called.
	 */
	ftl_mngt_fn cleanup;
	/* [한국어] step 실패 시 롤백을 위한 cleanup 함수. process가 어느 step에서 실패하면
	 * 그때까지 실행된 step들의 cleanup이 역순으로 호출되어 자원을 해제.
	 * 자원 할당이 없는 step은 cleanup이 NULL이어도 됨. */
};

/**
 * The FTL management process descriptor
 */
/*
 * [한국어]
 * struct ftl_mngt_process_desc - process(step 시퀀스) 디스크립터.
 *
 * 정적 const 변수로 선언되며 ftl_mngt_process_execute의 인자로 전달.
 */
struct ftl_mngt_process_desc {
	/**
	 * The name of the process
	 */
	const char *name;
	/* [한국어] 디버그/로그용 process 이름 (예: "FTL startup", "FTL shutdown"). */

	/**
	 * Size of the process argument (context)
	 *
	 * The process context will be allocated before execution of the first
	 * step
	 *
	 * @note To get context of the process within FTL management callback,
	 * execute ftl_mngt_get_process_ctx
	 */
	size_t ctx_size;
	/* [한국어] process 전체에서 공유되는 ctx 크기. 첫 step 실행 전 calloc.
	 * 모든 step은 ftl_mngt_get_process_ctx로 동일 메모리 접근. */

	/**
	 * Pointer to the additional error handler when the process fails
	 */
	ftl_mngt_fn error_handler;
	/* [한국어] process가 실패하면(어떤 step에서든 fail_step) cleanup 체인 후 추가로
	 * 호출되는 핸들러. 예: shutdown은 error_handler에 ftl_mngt_rollback_device 등록. */

	/**
	 * The initialization handler is invoked when the management process is
	 * being created before the execution of the process
	 */
	ftl_mngt_init_fn init_handler;
	/* [한국어] process 시작 직전 한 번 호출되는 초기화 핸들러. caller가 넘긴 init_ctx로
	 * process_ctx를 채우는 용도. 0 반환 시 진행, 비-0 반환 시 process 시작 실패. */

	/**
	 * When the process wants to cleanup, for example free resources which
	 * were allocated in init_handler, the deinit_handler can be provided
	 */
	ftl_mngt_fn deinit_handler;
	/* [한국어] process 끝(성공/실패 무관)에 호출되는 정리 핸들러. init_handler에서
	 * 할당한 자원 등을 해제. */

	/**
	 * The FTL process steps
	 *
	 * The process context will be allocated before execution of the first
	 * step
	 *
	 * @note The step array terminator shall end with action equals NULL
	 */
	struct ftl_mngt_step_desc steps[];
	/* [한국어] step 배열. flexible array member — 정적 선언 시 끝에 빈 `{}`로 sentinel
	 * (action == NULL) 표시 필요. */
};

/**
 * @brief Executes the FTL management process defined by the process descriptor
 *
 * In case of an error all already executed steps will have their rollback functions
 * called in reverse order.
 *
 * @param dev FTL device
 * @param process The descriptor of process to be executed
 * @param cb Caller callback
 * @param cb_ctx Caller context
 *
 * @return Result of invoking the operation
 * @retval 0 - The FTL management process has been started
 * @retval Non-zero An error occurred when starting The FTL management process
 */
/*
 * [한국어]
 * ftl_mngt_process_execute - process를 시작.
 *
 * 비동기: 본 함수는 process 시작만 함. 모든 step이 끝나면 cb(dev, cb_ctx, 0) 호출.
 * 실패 시 step의 cleanup이 역순 실행되고 마지막에 cb(dev, cb_ctx, 음수) 호출.
 */
int ftl_mngt_process_execute(struct spdk_ftl_dev *dev,
			     const struct ftl_mngt_process_desc *process,
			     ftl_mngt_completion cb, void *cb_ctx);

/**
 * @brief Executes rollback on the FTL management process defined by the process
 * descriptor
 *
 * All cleanup function from steps will be executed in reversed order
 *
 * @param dev FTL device
 * @param process The descriptor of process to be rollback
 * @param cb Caller callback
 * @param cb_ctx Caller context
 *
 * @return Result of invoking the rollback operation
 * @retval 0 - Rollback of the FTL management process has been started
 * @retval Non-zero An error occurred when starting the rollback
 */
/*
 * [한국어]
 * ftl_mngt_process_rollback - process를 롤백 모드로 시작 (cleanup만 역순 실행).
 *
 * 정상 흐름이 아닌 곳에서 자원 정리만 필요할 때 사용.
 */
int ftl_mngt_process_rollback(struct spdk_ftl_dev *dev,
			      const struct ftl_mngt_process_desc *process,
			      ftl_mngt_completion cb, void *cb_ctx);

/*
 * FTL management API for steps
 */

/**
 * @brief Gets FTL device
 *
 * @param mngt FTL management handle
 *
 * @note This function can be invoked within step handler only
 *
 * @return FTL device
 */
/* [한국어] step 안에서 dev 포인터 획득 (action 시그니처에 이미 dev가 있으므로 잘 안 씀,
 * sub-context에서 mngt만 받았을 때 사용). */
struct spdk_ftl_dev *ftl_mngt_get_dev(struct ftl_mngt_process *mngt);

/**
 * @brief Allocates a context for the management step
 *
 * @param mngt FTL management handle
 * @param size Size of the step context
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @return Operation result
 * @retval 0 Operation successful
 * @retval Non-zero Operation failure
 */
/* [한국어] step 안에서 step ctx를 동적으로 할당. 같은 step 안에서 재호출하면 이전 ctx는
 * free되고 새로 alloc(realloc 아님 — 데이터 보존 안 됨). 실패 시 비-0 반환. */
int ftl_mngt_alloc_step_ctx(struct ftl_mngt_process *mngt, size_t size);

/**
 * @brief Gets the management step context
 *
 * @param mngt FTL management handle
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @return Context of the step containing pointer to buffer and its size
 */
/* [한국어] 현재 step의 ctx 메모리 포인터 획득. step.ctx_size > 0이거나
 * alloc_step_ctx 호출했어야 NULL이 아닌 값 반환. */
void *ftl_mngt_get_step_ctx(struct ftl_mngt_process *mngt);

/**
 * @brief Gets the management process context
 *
 * @param mngt FTL management handle
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @return Context of the process containing pointer to buffer and its size
 */
/* [한국어] process 전체에서 공유되는 ctx (process_desc.ctx_size로 자동 할당) 포인터. */
void *ftl_mngt_get_process_ctx(struct ftl_mngt_process *mngt);

/**
 * @brief Gets the caller context
 *
 * @param mngt FTL management handle
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @return Pointer to the caller context
 */
/* [한국어] sub-process일 경우 caller(부모 process)가 call_process로 넘긴 init_ctx 포인터.
 * 부모-자식 process 간 데이터 전달 용도. */
void *ftl_mngt_get_caller_ctx(struct ftl_mngt_process *mngt);

/**
 * @brief Finishes the management process immediately
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @param mngt FTL management handle of process to be finished
 */
/* [한국어] 남은 step이 있어도 즉시 process를 정상 종료. 보통 조건부 조기 종료 시 사용. */
void ftl_mngt_finish(struct ftl_mngt_process *mngt);

/**
 * @brief Completes the step currently in progress and jump to a next one
 *
 * If no more steps to be executed then the management process is finished and
 * caller callback is invoked
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @param mngt FTL management handle
 */
/* [한국어] 현재 step 성공 — 다음 step으로 진행. 마지막 step이었으면 caller cb 호출. */
void ftl_mngt_next_step(struct ftl_mngt_process *mngt);

/**
 * @brief Skips the step currently in progress and jump to a next one
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @param mngt FTL management handle
 */
/* [한국어] 현재 step을 건너뛰고 다음 step으로 (action을 실행 안 한 것으로 처리).
 * 조건부로 step이 무의미할 때 사용. cleanup도 호출 안 됨. */
void ftl_mngt_skip_step(struct ftl_mngt_process *mngt);

/**
 * @brief Continue the step currently in progress
 *
 * This causes invoking the same step handler in next iteration of the
 * management process. This mechanism can be used by a job when polling for
 * something.
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @param mngt FTL management handle
 */
/* [한국어] 같은 step.action을 다시 호출하도록 예약 — polling 패턴이나 batch 반복용.
 * step ctx는 보존됨. */
void ftl_mngt_continue_step(struct ftl_mngt_process *mngt);

/**
 * @brief Fail the step currently in progress.
 *
 * It stops executing all steps and starts the rollback procedure (calling
 * the cleanup functions of all already executed steps).
 * If executed from a cleanup function, it will stop executing and the following
 * cleanup functions (if any) will be executed.
 *
 * @param mngt FTL management handle
 */
/* [한국어] 현재 step 실패 — 후속 step 실행 중단, 이미 실행된 step의 cleanup을 역순 호출.
 * cleanup 안에서 fail_step을 호출하면 그 cleanup만 중단하고 나머지는 계속 실행. */
void ftl_mngt_fail_step(struct ftl_mngt_process *mngt);

/**
 * @brief Calls another management process
 *
 * Ends the current step and executes specified process and finally continues
 * executing the the remaining steps
 *
 * @param mngt The management handle
 * @param process The management process to be called
 * @param init_ctx Process initialization context
 *
 * @note If the initialization procedure is required then both init_ctx and
 * init_handler in the process descriptor must be provided.
 */
/* [한국어] 현재 step 안에서 sub-process 호출. sub-process가 끝나면 자동으로 다음
 * step으로 진행. layout_upgrade가 region 단위 sub-process를 호출할 때 사용하는 패턴. */
void ftl_mngt_call_process(struct ftl_mngt_process *mngt,
			   const struct ftl_mngt_process_desc *process,
			   void *init_ctx);

/**
 * @brief Calls rollback steps of another management process
 *
 * Ends the current step and executes rollback steps of specified process
 * and finally continues executing the remaining steps in the original process
 *
 * @param mngt The management handle
 * @param process The management process to be called to execute rollback
 */
/* [한국어] sub-process를 롤백 모드로 호출 (cleanup만 역순 실행). */
void ftl_mngt_call_process_rollback(struct ftl_mngt_process *mngt,
				    const struct ftl_mngt_process_desc *process);

/*
 * The specific management functions
 */
/**
 * @brief Starts up a FTL instance
 *
 * @param dev FTL device
 * @param cb Caller callback
 * @param cb_cntx Caller context
 *
 * @return Operation result
 * @retval 0 The operation successful has started
 * @retval Non-zero Startup failure
 */
/* [한국어] FTL 디바이스 startup 외부 진입점. spdk_ftl_dev_init이 호출.
 * 적절한 startup desc를 골라(첫 init / 정상 / dirty recovery / SHM 등) process_execute. */
int ftl_mngt_call_dev_startup(struct spdk_ftl_dev *dev, ftl_mngt_completion cb, void *cb_cntx);

/*
 * The specific management functions
 */
/**
 * @brief Issue trim on FTL instance
 *
 * @param dev FTL device
 * @param cb Caller callback
 * @param cb_cntx Caller context
 *
 * @return Operation result
 * @retval 0 The operation successful has started
 * @retval Non-zero Startup failure
 */
/* [한국어] LBA 범위에 trim/discard 발행. LBA→PBA 매핑을 unmap으로 표시.
 * lba/num_blocks를 컨텍스트에 담아 trim sub-process 실행. */
int ftl_mngt_trim(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t num_blocks, spdk_ftl_fn cb,
		  void *cb_cntx);

/**
 * @brief Shuts down a FTL instance
 *
 * @param dev FTL device
 * @param cb Caller callback
 * @param cb_cntx Caller context
 *
 * @return Operation result
 * @retval 0 The operation successful has started
 * @retval Non-zero Shutdown failure
 */
/* [한국어] FTL 디바이스 shutdown 외부 진입점. dev->conf.fast_shutdown으로 desc 선택. */
int ftl_mngt_call_dev_shutdown(struct spdk_ftl_dev *dev, ftl_mngt_completion cb, void *cb_cntx);

#endif /* LIB_FTL_FTL_MNGT_H */
