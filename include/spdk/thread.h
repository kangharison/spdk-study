/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Thread
 */

/*
 * [한국어 설명] SPDK 스레드/폴러/I/O 채널/메시지 패싱 공개 API (thread.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK의 모든 라이브러리(bdev, nvme, nvmf, blob, sock 등)가 의존하는 핵심
 * 실행 모델 인터페이스를 정의한다. 구체적으로 다음 5개 영역의 공개 API를 제공한다:
 *   1) spdk_thread - reactor에 배치되는 stackless 논리 스레드 추상화 (생성/소멸/poll)
 *   2) spdk_poller - 한 spdk_thread에 등록되어 주기적으로 호출되는 콜백 (busy-poll)
 *   3) spdk_io_channel / spdk_io_device - per-thread I/O 자원 관리 (lockless 핵심)
 *   4) message passing (spdk_thread_send_msg) - cross-thread 호출을 위한 ring 기반 비동기 메시지
 *   5) spdk_iobuf / spdk_spinlock / spdk_interrupt - 버퍼 풀, 안전 스핀락, fd_group 인터럽트 통합
 * SPDK가 "kernel-bypass + polled-mode + lockless"라는 3대 원칙을 구현할 때, 그 lockless 측면은
 * 거의 전적으로 이 파일이 제공하는 API의 사용 규약(스레드 affinity, message passing)에 의존한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 관점에서 이 헤더는 SPDK 스택의 정중앙에 위치한다:
 *   [Application] -> spdk_app_start() -> spdk_thread_lib_init()
 *      -> 각 CPU 코어마다 reactor 생성 -> reactor가 N개 spdk_thread 호스팅
 *         -> spdk_thread_poll() 무한 루프
 *             -> 메시지 큐(rte_ring) 처리 + 등록된 모든 poller 호출
 *                -> nvme/bdev/nvmf 모듈의 I/O 완료 폴링 함수가 여기서 실행됨
 * 실행 컨텍스트는 호스트 유저스페이스이며, DPDK EAL이 미리 핀(pinning)한 pthread 위에서
 * SPDK reactor가 동작한다. 한 pthread에는 여러 spdk_thread가 시분할로 올라갈 수 있고,
 * scheduler가 부하 균형을 위해 spdk_thread를 다른 reactor로 마이그레이션할 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 헤더(아래 #include): spdk/env.h(DPDK 환경), spdk/cpuset.h(코어 마스크),
 * spdk/fd_group.h(epoll 기반 인터럽트 모드), spdk/likely.h(분기 예측 매크로),
 * spdk/util.h, spdk/assert.h, spdk/stdinc.h, spdk/config.h(빌드 설정 매크로).
 * 이 헤더에 의존하는 모듈: lib/bdev(bdev_io 발행/완료에 io_channel 사용),
 * lib/nvme(qpair를 io_channel로 래핑), lib/nvmf(transport poll group 관리),
 * lib/blob, lib/sock, module/bdev/* 모든 백엔드, app/* 앱 프레임워크 전반.
 * 데이터 흐름: 앱 코드가 spdk_get_io_channel()로 채널 얻음 -> bdev_io 제출 -> 해당 채널의
 * 백엔드 콜백 호출 -> 비동기 완료는 같은 spdk_thread의 poller가 폴링 -> 콜백 실행.
 * 공유 상태: spdk_io_device는 전역 트리에 등록되고, 채널은 (device, thread)별 1개씩 캐시되며
 * 참조 카운팅으로 수명 관리.
 *
 * === 주요 함수/구조체 요약 ===
 *   spdk_thread_create()      - 이름과 cpumask로 새 spdk_thread 할당. 첫 번째는 app thread.
 *   spdk_thread_poll()        - 한 라운드의 메시지/poller 처리 (reactor 루프의 본체).
 *   spdk_thread_send_msg()    - 다른 spdk_thread에 lockless로 비동기 함수 호출 요청 (ring 기반).
 *   spdk_poller_register()    - 현재 스레드에 주기 콜백 등록. period_us=0이면 매 라운드 호출.
 *   spdk_io_device_register() - 글로벌 io_device 등록 + per-thread create/destroy 콜백 지정.
 *   spdk_get_io_channel()     - 현재 스레드용 io_channel 획득 (없으면 create_cb로 생성, 있으면 ref+1).
 *   spdk_put_io_channel()     - 채널 참조 해제. 마지막 참조면 destroy_cb 비동기 호출.
 *   spdk_for_each_channel()   - io_device의 모든 채널에 대해 각 소속 스레드에서 fn을 직렬 실행.
 *   spdk_iobuf_get()/put()    - thread-local cache가 있는 small/large 버퍼 풀에서 할당/반납.
 *   spdk_spinlock             - "lock 후 yield 금지" 규약을 강제하는 스레드-aware 스핀락.
 *   spdk_interrupt_register() - epoll fd_group에 fd를 등록해 인터럽트 모드를 지원.
 *   주요 자료구조: struct spdk_thread (불투명), struct spdk_poller (불투명),
 *                struct spdk_io_channel (불투명, ctx는 SPDK_IO_CHANNEL_STRUCT_SIZE 뒤에 위치),
 *                struct spdk_spinlock, struct spdk_iobuf_channel/pool_cache.
 */

#ifndef SPDK_THREAD_H_
/* [한국어] 헤더 가드 매크로 - 같은 컴파일 단위에서 thread.h가 다중 include될 때
 * 중복 선언 에러를 방지한다. SPDK는 모든 공개 헤더에 SPDK_<NAME>_H_ 형태의
 * 가드 컨벤션을 사용한다. */
#define SPDK_THREAD_H_

#include "spdk/config.h"
/* [한국어] SPDK 빌드 설정 매크로 모음. 본 헤더에서는 SPDK_CONFIG_MAX_NUMA_NODES를
 * spdk_iobuf_channel.cache 배열 크기 결정에 사용한다 (NUMA 노드별 캐시). */
#include "spdk/fd_group.h"
/* [한국어] epoll fd_group 추상화. SPDK 21.07+의 인터럽트 모드에서
 * spdk_interrupt_register()가 fd 단위로 등록한 이벤트들을 한 fd_group으로 묶어
 * spdk_thread_get_interrupt_fd_group()로 외부 epoll에 합성할 수 있게 한다. */
#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 인클루드 (stdint, stddef, sys/queue 등). STAILQ_ENTRY 매크로와
 * uint64_t 같은 정수 타입을 본 헤더에서 사용하기 위해 필요. */
#include "spdk/assert.h"
/* [한국어] assert/SPDK_STATIC_ASSERT 매크로. spdk_io_channel_get_ctx()의
 * 인라인 함수 안에서 NULL 검증용 assert(false)를 호출한다. */
#include "spdk/cpuset.h"
/* [한국어] CPU affinity 비트마스크 구조체 spdk_cpuset 정의. spdk_thread_create()의
 * cpumask 인자와 spdk_thread_set_cpumask()/get_cpumask() API에서 사용. */
#include "spdk/env.h"
/* [한국어] DPDK 환경 추상화. struct spdk_event_handler_opts(인터럽트 등록 옵션
 * 확장체)와 spdk_get_ticks() 등 시각 관련 API가 여기에 정의되어 있다. */
#include "spdk/util.h"
/* [한국어] container_of, SPDK_COUNTOF 등 일반 유틸 매크로. 본 헤더의 인라인
 * 함수에서 직접적으로 호출되지는 않지만 의존 트리상 포함된다. */
#include "spdk/likely.h"
/* [한국어] spdk_likely/spdk_unlikely 분기 예측 매크로. spdk_io_channel_get_ctx()의
 * NULL 체크에서 spdk_unlikely를 사용해 hot path를 최적화한다. */

#ifdef __cplusplus
/* [한국어] C++에서 이 헤더를 include할 때 함수들을 C linkage로 노출하여
 * 이름 맹글링을 방지한다 - SPDK 라이브러리는 C로 컴파일되기 때문. */
extern "C" {
#endif

/**
 * Pollers should always return a value of this type
 * indicating whether they did real work or not.
 */
/*
 * [한국어]
 * spdk_thread_poller_rc - 폴러 콜백의 반환값 enum.
 *
 * 모든 spdk_poller_fn은 이 enum 중 하나를 반환해야 한다. 이 값은 reactor의 통계
 * 수집(busy_tsc/idle_tsc)과 인터럽트 모드 전환 결정의 근거가 된다. 값이 IDLE이면
 * scheduler가 해당 코어를 sleep시키거나 다른 spdk_thread로 양보할 수 있다고 판단한다.
 *
 * 사용 컨텍스트: spdk_poller가 등록된 spdk_thread의 spdk_thread_poll() 루프 내부.
 */
enum spdk_thread_poller_rc {
	SPDK_POLLER_IDLE,
	/* [한국어] 이번 호출에서 처리할 작업이 없었음 (예: NVMe CQ가 비어있음).
	 * 통계의 idle_tsc에 누적되며, 인터럽트 모드/저전력 모드 진입 판단에 사용된다.
	 * 값: 0 (enum 첫 항목이므로 묵시적). */

	SPDK_POLLER_BUSY,
	/* [한국어] 이번 호출에서 실제로 작업이 처리되었음 (예: CQE 1개 이상 완료).
	 * 통계의 busy_tsc에 누적되며, scheduler가 해당 코어를 깨어있게 유지하도록 한다.
	 * 값: 1. */
};

/**
 * A stackless, lightweight thread.
 */
/*
 * [한국어]
 * struct spdk_thread - 불투명(opaque) 전방 선언.
 *
 * 실제 정의는 lib/thread/thread.c 내부의 비공개 영역에 있다. 외부 코드는 포인터로만
 * 다루고 spdk_thread_*() 접근자를 통해서만 필드에 접근한다. "stackless"란 OS 스레드처럼
 * 자체 스택을 가지지 않고 reactor의 실행 스택 위에서 실행됨을 의미한다 - 즉 한 pthread에
 * 여러 spdk_thread가 시분할로 올라가도 추가 스택 메모리가 들지 않는다.
 */
struct spdk_thread;

/**
 * A function repeatedly called on the same spdk_thread.
 */
/*
 * [한국어]
 * struct spdk_poller - 불투명 전방 선언. lib/thread/thread.c에서 정의.
 *
 * spdk_thread에 등록되어 spdk_thread_poll() 루프마다 (또는 period_us 주기마다) 호출되는
 * 콜백을 표현한다. 한 spdk_poller는 정확히 하나의 spdk_thread에 종속되며, 다른 스레드로
 * 마이그레이션되지 않는다. 이는 lockless 설계의 핵심 - poller 콜백 내부에서는
 * 같은 spdk_thread의 자료구조에 락 없이 접근 가능하다.
 */
struct spdk_poller;

struct spdk_io_channel_iter;
/* [한국어] spdk_for_each_channel()의 순회 상태를 담는 불투명 구조체.
 * spdk_channel_msg 콜백에 전달되어 현재 채널/장치/사용자 컨텍스트를 꺼낼 수 있게 해준다. */

/**
 * A function that is called each time a new thread is created.
 * The implementer of this function should frequently call
 * spdk_thread_poll() on the thread provided.
 *
 * \param thread The new spdk_thread.
 */
/*
 * [한국어]
 * spdk_new_thread_fn - 새 spdk_thread 생성 시 호출되는 등록 콜백 타입.
 *
 * @thread: 갓 생성된 spdk_thread 포인터. 호출자(scheduler/reactor)는 이 콜백 내부에서
 *          해당 thread를 어느 reactor(코어)에 배치할지 결정해야 한다.
 * @return: 0 성공, 음수 errno 실패. 실패 시 spdk_thread_create()도 실패 처리.
 *
 * 등록 경로: spdk_thread_lib_init(new_thread_fn, ...) 호출 시 전역으로 저장.
 * 호출 시점: spdk_thread_create()가 새 thread를 만든 직후 동기적으로 호출.
 * 콜백 내 의무: 이 thread를 어떤 pthread/reactor의 스케줄 큐에 넣어 spdk_thread_poll()이
 *              주기적으로 호출되도록 보장해야 한다 (안 그러면 thread는 동작하지 않음).
 */
typedef int (*spdk_new_thread_fn)(struct spdk_thread *thread);

/**
 * SPDK thread operation type.
 */
/*
 * [한국어]
 * spdk_thread_op - scheduler/reactor가 spdk_thread에 대해 수행할 수 있는 동작 종류.
 *
 * spdk_thread_lib_init_ext()의 thread_op_fn 콜백이 이 enum을 받아 동작을 수행한다.
 * 이 추상화 덕분에 SPDK는 reactor 구현(lib/event)과 thread 라이브러리(lib/thread)를
 * 분리할 수 있다 - lib/thread는 "이런 작업을 해주세요"라고 요청만 하고, 실제 코어 배치는
 * 외부 reactor가 결정한다.
 */
enum spdk_thread_op {
	/* Called each time a new thread is created. The implementer of this operation
	 * should frequently call spdk_thread_poll() on the thread provided.
	 */
	SPDK_THREAD_OP_NEW,
	/* [한국어] 새 spdk_thread가 생성됐으니 reactor에 배치해 달라는 요청.
	 * spdk_new_thread_fn과 동일한 의미를 op enum 형태로 일반화한 것.
	 * 외부 구현은 cpumask를 보고 적절한 reactor 큐에 등록해야 한다. */

	/* Called when SPDK thread needs to be rescheduled. (e.g., when cpumask of the
	 * SPDK thread is updated.
	 */
	SPDK_THREAD_OP_RESCHED,
	/* [한국어] cpumask 변경 등으로 스레드를 다른 reactor로 옮겨야 함을 알림.
	 * spdk_thread_set_cpumask() 호출 시 발생. 외부 scheduler가 이 op을 지원해야
	 * 동적 부하 균형이 가능하며, 미지원 시 spdk_thread_set_cpumask()는 -ENOTSUP 반환. */
};

/**
 * Function to be called for SPDK thread operation.
 */
/*
 * [한국어]
 * spdk_thread_op_fn - thread operation 디스패처 콜백 타입.
 *
 * @thread: 작업 대상 spdk_thread 포인터.
 * @op:     수행할 작업 종류 (SPDK_THREAD_OP_NEW / SPDK_THREAD_OP_RESCHED).
 * @return: 0 성공, 음수 errno 실패.
 *
 * 등록: spdk_thread_lib_init_ext(thread_op_fn, ...). 호출자: lib/thread 내부.
 * 일반적인 구현은 lib/event/reactor.c가 담당하며, 이는 새 thread를 reactor의 lcore_thread
 * 큐에 enqueue하거나 RESCHED 시에는 다른 reactor의 큐로 이동시킨다.
 */
typedef int (*spdk_thread_op_fn)(struct spdk_thread *thread, enum spdk_thread_op op);

/**
 * Function to check whether the SPDK thread operation is supported.
 */
/*
 * [한국어]
 * spdk_thread_op_supported_fn - 특정 op이 지원되는지 질의하는 콜백.
 *
 * @op:     검사할 작업 종류.
 * @return: true면 지원, false면 미지원. 미지원 op이 호출되면 lib/thread는 해당 API를
 *          -ENOTSUP으로 거부한다 (예: RESCHED 미지원 시 spdk_thread_set_cpumask 거부).
 */
typedef bool (*spdk_thread_op_supported_fn)(enum spdk_thread_op op);

/**
 * A function that will be called on the target thread.
 *
 * \param ctx Context passed as arg to spdk_thread_pass_msg().
 */
/*
 * [한국어]
 * spdk_msg_fn - cross-thread 메시지로 전달되는 함수 시그니처.
 *
 * @ctx: spdk_thread_send_msg(thread, fn, ctx)의 ctx 인자가 그대로 전달됨.
 *
 * 실행 컨텍스트: 메시지를 받은 target spdk_thread의 spdk_thread_poll() 내부에서 호출.
 * 즉 "다른 스레드에서 이 함수를 실행해 달라"는 요청을 표현한다. 이 콜백 안에서는 target
 * thread의 자료구조에 락 없이 접근 가능 (SPDK lockless 패턴의 핵심).
 * 메모리 수명: ctx는 호출자가 동적 할당한 경우 콜백 내부에서 free 해야 함 - send_msg는
 * ctx를 복사하지 않고 포인터만 전달하므로.
 */
typedef void (*spdk_msg_fn)(void *ctx);

/**
 * Function to be called to pass a message to a thread.
 *
 * \param fn Callback function for a thread.
 * \param ctx Context passed to fn.
 * \param thread_ctx Context for the thread.
 */
/*
 * [한국어]
 * spdk_thread_pass_msg - 메시지 전달 메커니즘을 외부에서 주입할 수 있게 하는 함수 포인터 타입.
 *
 * @fn:         target에서 실행될 콜백.
 * @ctx:        fn에 전달될 인자.
 * @thread_ctx: target spdk_thread의 컨텍스트 포인터.
 *
 * 일반적인 SPDK 사용에서는 lib/thread 내장 ring 기반 구현이 쓰이지만, 이 typedef는 외부
 * 메시지 전송 백엔드(예: 단위 테스트 가짜 큐)를 끼워 넣을 수 있게 하기 위한 추상화이다.
 */
typedef void (*spdk_thread_pass_msg)(spdk_msg_fn fn, void *ctx,
				     void *thread_ctx);

/**
 * Callback function for a poller.
 *
 * \param ctx Context passed as arg to spdk_poller_register().
 * \return value of type `enum spdk_thread_poller_rc` (ex: SPDK_POLLER_IDLE
 * if no work was done or SPDK_POLLER_BUSY if work was done.)
 */
/*
 * [한국어]
 * spdk_poller_fn - poller 콜백 시그니처.
 *
 * @ctx:    spdk_poller_register(fn, arg, ...)의 arg가 그대로 전달.
 * @return: enum spdk_thread_poller_rc 값. IDLE/BUSY로 작업 유무 보고.
 *
 * 실행 컨텍스트: 등록된 spdk_thread의 spdk_thread_poll() 내부에서 직접 호출 (별도 스레드 없음).
 * 호출 빈도: period_us=0이면 매 라운드마다, >0이면 해당 마이크로초 단위 타임슬롯에 호출.
 * 주의사항: 이 콜백 안에서 sleep/blocking 호출은 절대 금지 (전체 reactor가 멈춤).
 *           장시간 작업은 작은 단위로 쪼개고 다음 호출에서 이어가는 패턴(state machine) 사용.
 */
typedef int (*spdk_poller_fn)(void *ctx);

/**
 * Callback function to set poller into interrupt mode or back to poll mode.
 *
 * \param poller Poller to set interrupt or poll mode.
 * \param cb_arg Argument passed to the callback function.
 * \param interrupt_mode Set interrupt mode for true, or poll mode for false
 */
/*
 * [한국어]
 * spdk_poller_set_interrupt_mode_cb - poller가 인터럽트<->폴 모드를 전환할 때 호출되는 콜백.
 *
 * @poller:         모드 전환 대상 poller.
 * @cb_arg:         spdk_poller_register_interrupt() 시 같이 등록한 인자.
 * @interrupt_mode: true=인터럽트 모드 진입, false=폴 모드로 복귀.
 *
 * 등록 경로: spdk_poller_register_interrupt(). 모듈은 이 콜백에서 자신의 fd를 epoll에 등록/해제하는
 *           작업을 수행 (예: NVMe 드라이버는 eventfd 등록 또는 doorbell 폴링 재개).
 * 실행 컨텍스트: 모드 전환을 트리거한 spdk_thread 위에서 동기 호출.
 */
typedef void (*spdk_poller_set_interrupt_mode_cb)(struct spdk_poller *poller, void *cb_arg,
		bool interrupt_mode);

/**
 * Mark that the poller is capable of entering interrupt mode.
 *
 * When registering the poller set interrupt callback, the callback will get
 * executed immediately if its spdk_thread is in the interrupt mode.
 *
 * Callers may pass NULL for the cb_fn, signifying that no callback is
 * necessary when the interrupt mode changes.
 *
 * \param poller The poller to register callback function.
 * \param cb_fn Callback function called when the poller must transition into or out of interrupt mode
 * \param cb_arg Argument passed to the callback function.
 */
/*
 * [한국어]
 * spdk_poller_register_interrupt - poller에 인터럽트 모드 전환 콜백을 등록.
 *
 * @poller: 이미 spdk_poller_register*()로 등록된 poller.
 * @cb_fn:  모드 전환 시 호출될 콜백 (NULL이면 콜백 없이 전환만 표시).
 * @cb_arg: cb_fn에 전달될 사용자 컨텍스트.
 *
 * 동기/배경: SPDK 21.07+에서 도입된 인터럽트 모드는 polled-mode의 CPU 사용 트레이드오프를
 * 완화하기 위함이다. 이 함수는 poller가 인터럽트 모드를 지원함을 lib/thread에 알려, 모드
 * 전환 시점에 모듈이 fd를 epoll에 등록/해제할 기회를 준다. 등록 시점에 이미 thread가
 * 인터럽트 모드면 cb_fn이 즉시 호출되어 일관된 상태를 보장한다.
 * 실행 컨텍스트: poller가 등록된 spdk_thread 위에서 호출되어야 함.
 * 호출 체인: 모듈(예: bdev_nvme) 초기화 -> spdk_poller_register_named() -> 본 함수.
 */
void spdk_poller_register_interrupt(struct spdk_poller *poller,
				    spdk_poller_set_interrupt_mode_cb cb_fn,
				    void *cb_arg);

/**
 * I/O channel creation callback.
 *
 * \param io_device I/O device associated with this channel.
 * \param ctx_buf Context for the I/O device.
 */
/*
 * [한국어]
 * spdk_io_channel_create_cb - io_device의 채널 생성 콜백 타입.
 *
 * @io_device: spdk_io_device_register()의 io_device 포인터 (디바이스 식별자).
 * @ctx_buf:   spdk_io_device_register()의 ctx_size만큼 자동 할당된 채널별 컨텍스트 버퍼.
 *             모듈은 여기에 NVMe qpair, ring 핸들 등 per-thread 자원을 초기화.
 * @return:    0 성공, 음수 errno 실패. 실패 시 spdk_get_io_channel()이 NULL 반환.
 *
 * 호출 시점: spdk_get_io_channel(io_device)가 처음으로 (해당 thread, device) 쌍에 호출될 때.
 * 실행 컨텍스트: 호출자(=채널을 요청한 spdk_thread). 이 콜백에서 만든 자원은 그 thread에 고정됨.
 * 호출 체인: 앱 -> spdk_get_io_channel() -> 본 콜백 -> 모듈별 자원 할당.
 */
typedef int (*spdk_io_channel_create_cb)(void *io_device, void *ctx_buf);

/**
 * I/O channel destruction callback.
 *
 * \param io_device I/O device associated with this channel.
 * \param ctx_buf Context for the I/O device.
 */
/*
 * [한국어]
 * spdk_io_channel_destroy_cb - 채널의 마지막 참조 해제 시 호출되는 정리 콜백.
 *
 * @io_device: 디바이스 포인터.
 * @ctx_buf:   create_cb에서 초기화했던 채널 컨텍스트 버퍼. 콜백은 이 안의 자원을 해제해야 함.
 *
 * 호출 시점: spdk_put_io_channel()이 ref 카운트를 0으로 떨어뜨린 직후 (비동기 메시지로 dispatch).
 * 실행 컨텍스트: 채널이 소속된 spdk_thread 위 (create_cb이 호출된 그 스레드).
 * 주의: 이 콜백 후 ctx_buf 메모리 자체는 lib/thread가 free 처리.
 */
typedef void (*spdk_io_channel_destroy_cb)(void *io_device, void *ctx_buf);

/**
 * I/O device unregister callback.
 *
 * \param io_device Unregistered I/O device.
 */
/*
 * [한국어]
 * spdk_io_device_unregister_cb - io_device의 모든 채널이 정리된 후 호출되는 최종 콜백.
 *
 * @io_device: spdk_io_device_unregister()로 unregister된 디바이스 포인터.
 *
 * 호출 시점: spdk_io_device_unregister() 호출 후, 잔여 채널들이 모두 destroy되고 나면 호출됨.
 *           즉 unregister는 즉시가 아니라 deferred 방식. 모듈은 이 콜백에서 io_device 자체를 free.
 * 실행 컨텍스트: unregister를 호출한 thread 위.
 */
typedef void (*spdk_io_device_unregister_cb)(void *io_device);

/**
 * Called on the appropriate thread for each channel associated with io_device.
 *
 * \param i I/O channel iterator.
 */
/*
 * [한국어]
 * spdk_channel_msg - spdk_for_each_channel() 순회 콜백 타입.
 *
 * @i: 순회 상태 객체. spdk_io_channel_iter_get_channel()/get_io_device()/get_ctx()로 정보 획득.
 *
 * 실행 컨텍스트: 각 채널이 소속된 spdk_thread (즉 이 콜백은 매번 다른 스레드에서 호출됨).
 * 콜백은 작업 후 반드시 spdk_for_each_channel_continue(i, status)를 호출해 다음 채널로 진행시켜야
 * 한다 - 그렇지 않으면 순회가 멈춘다 (비동기 작업도 가능하게 하는 패턴).
 */
typedef void (*spdk_channel_msg)(struct spdk_io_channel_iter *i);

/**
 * spdk_for_each_channel() callback.
 *
 * \param i I/O channel iterator.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_channel_for_each_cpl - spdk_for_each_channel() 전체 완료 콜백.
 *
 * @i:      순회 상태 객체 (이 시점에는 더 이상 채널 컨텍스트는 유효하지 않음).
 * @status: 0=정상 완료, 음수=중간에 _continue(i, err)로 중단된 결과.
 *
 * 실행 컨텍스트: spdk_for_each_channel()을 처음 호출한 originating thread.
 * 모든 채널 처리가 끝나면 메시지가 다시 originating thread로 라우팅되어 이 콜백이 호출.
 */
typedef void (*spdk_channel_for_each_cpl)(struct spdk_io_channel_iter *i, int status);

#define SPDK_IO_CHANNEL_STRUCT_SIZE	96
/* [한국어] spdk_io_channel 헤더 부분의 크기 (바이트). 채널은 [헤더 96B][모듈 컨텍스트] 레이아웃이며,
 * spdk_io_channel_get_ctx(ch)는 단순히 (uint8_t*)ch + 96으로 컨텍스트 시작 주소를 계산한다.
 * 이 매크로는 ABI에 노출되어 있어 헤더 크기 변경 시 메이저 버전 bump 필요.
 * 96B에는 STAILQ 노드, ref count, dev/thread 포인터, destroy_cb 큐잉 정보 등이 포함된다. */

/**
 * Message memory pool size definitions
 */
#define SPDK_MSG_MEMPOOL_CACHE_SIZE	1024
/* [한국어] 메시지 mempool의 per-thread 캐시 크기. spdk_thread_send_msg()가 발행하는
 * 메시지 객체는 DPDK rte_mempool에서 할당되며, 각 spdk_thread가 1024개까지 로컬 캐시.
 * 캐시 덕분에 mempool 글로벌 락 없이 빠른 할당/반납이 가능 - lockless의 또 다른 축. */

/* Power of 2 minus 1 is optimal for memory consumption */
#define SPDK_DEFAULT_MSG_MEMPOOL_SIZE (262144 - 1)
/* [한국어] 메시지 mempool의 전체 객체 수 기본값 (2^18 - 1 = 262143).
 * "2의 거듭제곱 - 1"은 DPDK rte_mempool의 메모리 효율 최적화 권장값
 * (내부 ring이 2의 거듭제곱이므로, 객체 수가 ring 크기에 1만큼 모자라야 메모리 낭비 최소). */

/**
 * Initialize the threading library. Must be called once prior to allocating any threads.
 *
 * \param new_thread_fn Called each time a new SPDK thread is created. The implementer
 * is expected to frequently call spdk_thread_poll() on the provided thread.
 * \param ctx_sz For each thread allocated, an additional region of memory of
 * size ctx_size will also be allocated, for use by the thread scheduler. A pointer
 * to this region may be obtained by calling spdk_thread_get_ctx().
 *
 * \return 0 on success. Negated errno on failure.
 */
/*
 * [한국어]
 * spdk_thread_lib_init - SPDK threading 라이브러리 초기화 (간단 버전).
 *
 * @new_thread_fn: 새 thread 생성 시 호출될 콜백 (NULL이면 thread는 만들어지지만 어디에도
 *                 배치되지 않으니 사실상 사용 불가).
 * @ctx_sz:        각 thread에 부가될 scheduler 컨텍스트 크기 (바이트). spdk_thread_get_ctx()로 접근.
 * @return:        0 성공, 음수 errno 실패 (예: ENOMEM = mempool 할당 실패).
 *
 * 호출 시점: spdk_app_start() 내부 또는 사용자가 직접 SPDK env 초기화 후 한 번만 호출.
 * 동작 단계: 1) 메시지 mempool 생성 (SPDK_DEFAULT_MSG_MEMPOOL_SIZE) 2) 전역 콜백 저장
 *           3) io_device 트리 초기화 4) iobuf 풀 lazy init 준비.
 * 호출 체인: app/* main -> spdk_app_start() -> spdk_thread_lib_init() -> 이후 spdk_thread_create() 가능.
 * lockless 의미: 이 시점에 만들어지는 mempool은 DPDK rte_mempool로, 각 thread의 cache(1024)를 갖춰
 *               글로벌 락 없이 메시지 객체를 할당/반납 가능하게 한다.
 */
int spdk_thread_lib_init(spdk_new_thread_fn new_thread_fn, size_t ctx_sz);

/**
 * Initialize the threading library. Must be called once prior to allocating any threads
 *
 * Both thread_op_fn and thread_op_type_supported_fn have to be specified or not
 * specified together.
 *
 * \param thread_op_fn Called for SPDK thread operation.
 * \param thread_op_supported_fn Called to check whether the SPDK thread operation is supported.
 * \param ctx_sz For each thread allocated, for use by the thread scheduler. A pointer
 * to this region may be obtained by calling spdk_thread_get_ctx().
 * \param msg_mempool_size Size of the allocated spdk_msg_mempool.
 *
 * \return 0 on success. Negated errno on failure.
 */
/*
 * [한국어]
 * spdk_thread_lib_init_ext - 확장 인자를 받는 lib_init (op enum + mempool 크기 지정).
 *
 * @thread_op_fn:           thread 작업 디스패처 콜백 (NEW/RESCHED 처리). NULL이면 미사용.
 * @thread_op_supported_fn: 특정 op 지원 여부 질의 콜백. thread_op_fn과 함께 NULL이거나 함께 비-NULL이어야 함.
 * @ctx_sz:                 thread별 scheduler 컨텍스트 크기.
 * @msg_mempool_size:       메시지 mempool의 객체 수 (0이면 SPDK_DEFAULT_MSG_MEMPOOL_SIZE 사용).
 * @return:                 0 성공, 음수 errno.
 *
 * lib_init() 대비 차이점: 새 일반화된 op 콜백을 받고, mempool 크기를 메모리 제약 환경에서
 * 줄일 수 있게 한다. lib/event/reactor.c의 reactor 부팅 코드가 주로 사용한다.
 */
int spdk_thread_lib_init_ext(spdk_thread_op_fn thread_op_fn,
			     spdk_thread_op_supported_fn thread_op_supported_fn,
			     size_t ctx_sz, size_t msg_mempool_size);

/**
 * Release all resources associated with this library.
 */
/*
 * [한국어]
 * spdk_thread_lib_fini - threading 라이브러리 종료. 모든 thread가 exit/destroy된 후 호출.
 *
 * 정리 대상: 메시지 mempool, 등록된 io_device 잔여, iobuf 풀 등.
 * 호출 시점: spdk_app_stop() 마지막 단계 또는 사용자 정의 정리 코드. 이후 어떤 thread API도
 *           호출하면 안 됨.
 */
void spdk_thread_lib_fini(void);

/**
 * Creates a new SPDK thread object.
 *
 * Note that the first thread created via spdk_thread_create() will be designated as
 * the app thread.  Other SPDK libraries may place restrictions on certain APIs to
 * only be called in the context of this app thread.
 *
 * \param name Human-readable name for the thread; can be retrieved with spdk_thread_get_name().
 * The string is copied, so the pointed-to data only needs to be valid during the
 * spdk_thread_create() call. May be NULL to specify no name.
 * \param cpumask Optional mask of CPU cores on which to schedule this thread. This is only
 * a suggestion to the scheduler. The value is copied, so cpumask may be released when
 * this function returns. May be NULL if no mask is required.
 *
 * \return a pointer to the allocated thread on success or NULL on failure..
 */
/*
 * [한국어]
 * spdk_thread_create - 새 spdk_thread 객체를 할당하고 reactor에 배치 요청.
 *
 * @name:    디버그/관측용 이름 (NULL 가능). 내부에서 strdup되므로 호출자는 수명 신경 X.
 * @cpumask: 어느 CPU 코어들 위에서 실행할지 힌트 (NULL이면 모든 코어 가능). scheduler는 이를
 *           제안으로만 받아들이며, 강제는 spdk_thread_bind()로 별도 지정.
 * @return:  성공 시 새 spdk_thread 포인터, 실패 시 NULL.
 *
 * 동작 단계:
 *   1) struct spdk_thread + ctx_sz 만큼의 메모리 할당
 *   2) 메시지 큐(rte_ring)와 poller 트리(timed/active) 초기화
 *   3) cpumask 복사
 *   4) 등록된 new_thread_fn(또는 thread_op_fn(NEW))을 호출해 reactor에 인계
 *   5) "최초로 생성된 thread"라면 app thread로 표시 (spdk_thread_get_app_thread()로 조회 가능)
 * 실행 컨텍스트: 어느 SPDK thread에서든 호출 가능 (app thread 강제 아님). 단 lib_init이 선행되어야 함.
 * 호출 체인: 사용자 코드/RPC 핸들러 -> 본 함수 -> new_thread_fn 콜백 -> reactor enqueue.
 * 에러 경로: 메모리 부족이나 new_thread_fn 실패 시 부분 자원 정리 후 NULL 반환.
 */
struct spdk_thread *spdk_thread_create(const char *name, const struct spdk_cpuset *cpumask);

/**
 * Return the app thread.
 *
 * The app thread is the first thread created using spdk_thread_create().
 *
 * \return a pointer to the app thread, or NULL if no thread has been created yet.
 */
/*
 * [한국어]
 * spdk_thread_get_app_thread - 첫 번째로 생성된 spdk_thread (app thread) 핸들 반환.
 *
 * @return: app thread 포인터, 아직 어떤 thread도 만들어지지 않았으면 NULL.
 *
 * 용도: 일부 SPDK API들(특히 RPC 핸들러, bdev_register, subsystem 초기화)은 "반드시 app thread에서
 *       호출되어야 한다"는 제약이 있다. 이 함수는 그 thread에 send_msg를 보내거나 비교용으로 사용.
 * thread affinity 측면: cross-thread 호출을 strict 하게 통제하기 위한 anchor 역할.
 */
struct spdk_thread *spdk_thread_get_app_thread(void);

/**
 * Check if the specified spdk_thread is the app thread.
 *
 * \param thread The thread to check. If NULL, check the current spdk_thread.
 * \return true if the specified spdk_thread is the app thread, false otherwise.
 */
/*
 * [한국어]
 * spdk_thread_is_app_thread - 주어진(또는 현재) spdk_thread가 app thread인지 검사.
 *
 * @thread: 검사 대상. NULL이면 spdk_get_thread()로 현재 thread를 사용.
 * @return: app thread이면 true.
 *
 * 사용 예: SPDK_BDEV_REGISTER 같은 매크로 내부의 assert(spdk_thread_is_app_thread(NULL))처럼
 *         "이 API는 app thread 전용"을 강제하기 위한 가드.
 */
bool spdk_thread_is_app_thread(struct spdk_thread *thread);

/**
 * Force the current system thread to act as if executing the given SPDK thread.
 *
 * \param thread The thread to set.
 */
/*
 * [한국어]
 * spdk_set_thread - 현재 OS 스레드(pthread)에 spdk_thread 컨텍스트를 임시로 결합.
 *
 * @thread: 컨텍스트로 설정할 spdk_thread. NULL이면 unbind.
 *
 * 동기/배경: spdk_get_thread() 같은 TLS 기반 컨텍스트 조회가 동작하려면 현재 pthread가 어느
 *          spdk_thread 위에 있는지 알아야 한다. reactor 루프는 thread를 dispatch하기 직전 본 함수로
 *          컨텍스트를 swap한다. 단위 테스트에서는 reactor 없이 임의의 spdk_thread를 흉내내기 위해
 *          호출.
 * 주의: 일반 응용 코드는 이 함수를 직접 호출하지 말 것 - reactor 내부 구현용.
 */
void spdk_set_thread(struct spdk_thread *thread);

/**
 * Bind or unbind spdk_thread to its current CPU core.
 *
 * If spdk_thread is bound, it couldn't be rescheduled to other CPU cores until it is unbound.
 *
 * \param thread The thread to bind or not.
 * \param bind true for bind, false for unbind.
 */
/*
 * [한국어]
 * spdk_thread_bind - spdk_thread를 현재 코어에 강제 핀(pin)/언핀.
 *
 * @thread: 대상 스레드.
 * @bind:   true=현재 reactor에 고정, false=다시 마이그레이션 가능.
 *
 * 사용 사례: NVMe qpair처럼 코어 마이그레이션 비용이 매우 큰 자원을 보유한 thread는 일시적으로
 *          bind=true로 마이그레이션을 차단한다 (예: I/O 진행 중). bind=false로 풀면 scheduler가
 *          다시 부하 균형을 위해 옮길 수 있다.
 * 영향: bind 상태에서는 SPDK_THREAD_OP_RESCHED 무시.
 */
void spdk_thread_bind(struct spdk_thread *thread, bool bind);

/**
 * Returns whether the thread is bound to its current CPU core.
 *
 * \param thread The thread to query.
 *
 * \return true if bound, false otherwise
 */
/*
 * [한국어]
 * spdk_thread_is_bound - thread가 현재 코어에 핀된 상태인지 조회.
 *
 * @thread: 질의 대상 스레드.
 * @return: bind 상태면 true.
 *
 * 용도: scheduler가 마이그레이션 결정 전에 본 함수로 가능 여부 확인.
 */
bool spdk_thread_is_bound(struct spdk_thread *thread);

/**
 * Mark the thread as exited, failing all future spdk_thread_send_msg(),
 * spdk_poller_register(), and spdk_get_io_channel() calls. May only be called
 * within an spdk poller or message.
 *
 * All I/O channel references associated with the thread must be released
 * using spdk_put_io_channel(), and all active pollers associated with the thread
 * should be unregistered using spdk_poller_unregister(), prior to calling
 * this function. This function will complete these processing. The completion can
 * be queried by spdk_thread_is_exited().
 *
 * Note that this function must not be called on the app thread until after it
 * has been called for all other threads.
 *
 * \param thread The thread to exit.
 *
 * \return always 0. (return value was deprecated but keep it for ABI compatibility.)
 */
/*
 * [한국어]
 * spdk_thread_exit - thread를 "종료 진행 중" 상태로 표시 (실제 destroy는 별도).
 *
 * @thread: 종료할 스레드.
 * @return: 항상 0 (구버전 호환을 위한 반환값 유지).
 *
 * 동작: 이후 spdk_thread_send_msg/poller_register/get_io_channel은 거부됨. 단, 잔여 메시지와
 *      채널 정리(destroy_cb 호출)는 spdk_thread_poll()이 계속 진행. 정리가 모두 끝나면
 *      spdk_thread_is_exited(thread)가 true 반환 -> 그 후에야 spdk_thread_destroy() 호출 가능.
 * 호출 컨텍스트: 본 함수는 반드시 해당 thread 자신의 poller나 message 콜백 안에서 호출 (TLS
 *               컨텍스트가 그 thread여야 함).
 * 순서 제약: app thread는 모든 다른 thread가 exit된 후에야 exit 가능 (RPC, bdev 정리 의존성).
 */
int spdk_thread_exit(struct spdk_thread *thread);

/**
 * Returns whether the thread is marked as exited.
 *
 * A thread is exited only after it has spdk_thread_exit() called on it, and
 * it has been polled until any outstanding operations targeting this
 * thread have completed.  This may include poller unregistrations, io channel
 * unregistrations, or outstanding spdk_thread_send_msg calls.
 *
 * \param thread The thread to query.
 *
 * \return true if marked as exited, false otherwise.
 */
/*
 * [한국어]
 * spdk_thread_is_exited - thread가 완전히 종료(잔여 정리 포함)되었는지 검사.
 *
 * @thread: 질의 대상.
 * @return: exit_called && 모든 잔여 작업 정리 완료시 true.
 *
 * 폴링 패턴: scheduler/reactor는 spdk_thread_exit() 호출 후 주기적으로 본 함수로 확인하다가
 *          true가 되면 spdk_thread_destroy()를 호출해 메모리를 해제한다.
 */
bool spdk_thread_is_exited(struct spdk_thread *thread);

/**
 * Returns whether the thread is still running.
 *
 * A thread is considered running until it has * spdk_thread_exit() called on it.
 *
 * \param thread The thread to query.
 *
 * \return true if still running, false otherwise.
 */
/*
 * [한국어]
 * spdk_thread_is_running - thread가 아직 spdk_thread_exit()을 호출하지 않은 상태인지 검사.
 *
 * @thread: 질의 대상.
 * @return: exit 미요청이면 true. (잔여 정리 중이라도 exit 호출되었으면 false.)
 *
 * is_exited와의 차이: is_running=false는 "exit 호출됨" 시점에 즉시 false. is_exited는 정리가
 *                   끝난 후에야 true. 둘 사이의 중간 구간에는 둘 다 false인 짧은 윈도우 존재.
 */
bool spdk_thread_is_running(struct spdk_thread *thread);

/**
 * Destroy a thread, releasing all of its resources. May only be called
 * on a thread previously marked as exited.
 *
 * \param thread The thread to destroy.
 *
 */
/*
 * [한국어]
 * spdk_thread_destroy - 종료된 thread의 자원을 최종 해제.
 *
 * @thread: spdk_thread_is_exited(thread)==true인 스레드만 가능 (아니면 assert/abort).
 *
 * 해제 대상: 메시지 ring, poller 트리, ctx 메모리, 이름 문자열, fd_group(인터럽트 모드).
 * 호출 컨텍스트: 외부 (예: scheduler thread). 자기 자신을 destroy하면 안 됨.
 */
void spdk_thread_destroy(struct spdk_thread *thread);

/**
 * Return a pointer to this thread's context.
 *
 * \param thread The thread on which to get the context.
 *
 * \return a pointer to the per-thread context, or NULL if there is
 * no per-thread context.
 */
/*
 * [한국어]
 * spdk_thread_get_ctx - thread 생성 시 lib_init의 ctx_sz로 예약된 scheduler 컨텍스트 반환.
 *
 * @thread: 대상.
 * @return: ctx 영역 시작 포인터, ctx_sz==0이었으면 NULL.
 *
 * 용도: scheduler 구현체(예: lib/event/scheduler_dynamic.c)가 thread별 통계/우선순위 등을 저장.
 *      일반 사용자 코드는 사용 X.
 */
void *spdk_thread_get_ctx(struct spdk_thread *thread);

/**
 * Get the thread's cpumask.
 *
 * \param thread The thread to get the cpumask for.
 *
 * \return cpuset pointer
 */
/*
 * [한국어]
 * spdk_thread_get_cpumask - 현재 설정된 cpumask 반환 (읽기 전용 포인터).
 *
 * @thread: 대상.
 * @return: spdk_cpuset 포인터 (수정 금지). 변경하려면 spdk_thread_set_cpumask() 사용.
 */
struct spdk_cpuset *spdk_thread_get_cpumask(struct spdk_thread *thread);

/**
 * Set the current thread's cpumask to the specified value. The thread may be
 * rescheduled to one of the CPUs specified in the cpumask.
 *
 * This API requires SPDK thread operation supports SPDK_THREAD_OP_RESCHED.
 *
 * \param cpumask The new cpumask for the thread.
 *
 * \return 0 on success, negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_thread_set_cpumask - 현재 spdk_thread의 cpumask를 변경하고 reschedule 요청.
 *
 * @cpumask: 새 affinity mask (호출자가 소유, 내부에서 복사).
 * @return:  0 성공, -ENOTSUP=RESCHED 미지원, 기타 음수 errno.
 *
 * 동작: 새 mask를 thread에 저장하고 thread_op_fn(thread, RESCHED)를 호출. scheduler는 현재 코어가
 *      mask에 포함되지 않으면 thread를 mask 내 코어로 마이그레이션. 호출자 thread 자기 자신에 대해서만
 *      유효 (현재 spdk_thread).
 */
int spdk_thread_set_cpumask(struct spdk_cpuset *cpumask);

/**
 * Return the thread object associated with the context handle previously
 * obtained by calling spdk_thread_get_ctx().
 *
 * \param ctx A context previously obtained by calling spdk_thread_get_ctx()
 *
 * \return The associated thread.
 */
/*
 * [한국어]
 * spdk_thread_get_from_ctx - ctx 포인터로부터 역으로 spdk_thread 핸들을 복원.
 *
 * @ctx:    이전에 spdk_thread_get_ctx()로 얻은 포인터.
 * @return: 그 ctx를 소유한 spdk_thread.
 *
 * 패턴: container_of 매크로처럼, scheduler가 ctx 리스트를 순회하면서 각 ctx의 thread를 회수할 때 사용.
 */
struct spdk_thread *spdk_thread_get_from_ctx(void *ctx);

/**
 * Perform one iteration worth of processing on the thread. This includes
 * both expired and continuous pollers as well as messages. If the thread
 * has exited, return immediately.
 *
 * \param thread The thread to process
 * \param max_msgs The maximum number of messages that will be processed.
 *                 Use 0 to process the default number of messages (8).
 * \param now The current time, in ticks. Optional. If 0 is passed, this
 *            function will call spdk_get_ticks() to get the current time.
 *            The current time is used as start time and this function
 *            will call spdk_get_ticks() at its end to know end time to
 *            measure run time of this function.
 *
 * \return 1 if work was done. 0 if no work was done.
 */
/*
 * [한국어]
 * spdk_thread_poll - thread의 메시지·poller를 한 라운드 처리. SPDK reactor 루프의 본체.
 *
 * @thread:   처리할 spdk_thread.
 * @max_msgs: 이 라운드에서 처리할 최대 메시지 수. 0이면 기본값 8 사용. 메시지가 많으면 8개씩 끊어
 *            poller도 공평히 호출되도록 한다.
 * @now:      현재 TSC 값. 0이면 함수가 spdk_get_ticks()로 직접 측정. 외부에서 measure-once 패턴을
 *            지원하기 위함 (한 라운드 시작 ticks를 reactor가 이미 알고 있을 때).
 * @return:   1=메시지 또는 poller가 실제 작업 수행 (BUSY), 0=모두 idle (IDLE → 인터럽트 모드 진입 후보).
 *
 * 동작 단계:
 *   1) thread를 현재 pthread의 TLS에 set (spdk_set_thread).
 *   2) 메시지 ring에서 max_msgs개 dequeue → 각 메시지의 fn(ctx) 호출 (cross-thread 통신 처리).
 *   3) timed poller 트리에서 만료된 poller들 호출 (period_us 기반 RB-tree).
 *   4) active poller 리스트(period_us=0) 순회 호출.
 *   5) post_poller handler 실행 (등록되어 있다면 1회성 후처리).
 *   6) interrupt fd_group이 있으면 epoll_wait timeout=0 처리 (non-blocking).
 *   7) busy/idle TSC 누적 → spdk_thread_stats 갱신.
 *   8) 마지막 TSC 기록 (spdk_thread_get_last_tsc).
 * 실행 컨텍스트: reactor가 소유한 pthread. 한 라운드 동안 thread가 그 pthread의 TLS에 묶여 있음.
 * 호출 체인: lib/event/reactor.c의 reactor_run() → reactor_thread_op_supported 검사 후 본 함수 호출.
 * lockless 핵심: 메시지 ring은 MPMC rte_ring으로 dequeue가 lockless, poller 리스트는 thread 단독 소유.
 * 인터럽트 모드: thread가 SPDK_THREAD_OP_INTERRUPT 모드면 reactor는 spdk_thread_get_interrupt_fd()를
 *               외부 epoll에 추가하고, 이벤트 발생 시에만 본 함수를 호출해 idle CPU 회피.
 */
int spdk_thread_poll(struct spdk_thread *thread, uint32_t max_msgs, uint64_t now);

/**
 * Return the number of ticks until the next timed poller
 * would expire. Timed pollers are pollers for which
 * period_microseconds is greater than 0.
 *
 * \param thread The thread to check poller expiration times on
 *
 * \return Number of ticks. If no timed pollers, return 0.
 */
/*
 * [한국어]
 * spdk_thread_next_poller_expiration - 다음 timed poller 만료까지 남은 TSC.
 *
 * @thread: 검사 대상.
 * @return: 다음 만료까지 ticks (CPU TSC 단위). timed poller가 없으면 0.
 *
 * 활용: 인터럽트 모드 진입 시 epoll_wait 타임아웃을 산출. scheduler가 idle 코어를 sleep시킬 때
 *       다음 만료 시각까지만 자도록 결정.
 */
uint64_t spdk_thread_next_poller_expiration(struct spdk_thread *thread);

/**
 * Returns whether there are any active pollers (pollers for which
 * period_microseconds equals 0) registered to be run on the thread.
 *
 * \param thread The thread to check.
 *
 * \return 1 if there is at least one active poller, 0 otherwise.
 */
/*
 * [한국어]
 * spdk_thread_has_active_pollers - period_us=0인 "busy poller" 등록 여부.
 *
 * @thread: 검사 대상.
 * @return: 1=하나 이상 존재, 0=없음.
 *
 * 의미: active poller가 있으면 thread는 매 라운드 100% CPU 사용 (전형적인 polled-mode).
 *       0개라면 timed poller만 있으니 인터럽트 모드 전환 가능 (scheduler 결정 근거).
 */
int spdk_thread_has_active_pollers(struct spdk_thread *thread);

/**
 * Returns whether there are any pollers registered to be run
 * on the thread.
 *
 * \param thread The thread to check.
 *
 * \return true if there is any active poller, false otherwise.
 */
/*
 * [한국어]
 * spdk_thread_has_pollers - active든 timed든 등록된 poller가 하나라도 있는지.
 *
 * @thread: 검사 대상.
 * @return: true면 어떤 poller라도 존재, false면 메시지 처리 외 work 없음.
 *
 * has_active_pollers와의 차이: 본 함수는 timed poller(period_us>0)도 카운트.
 */
bool spdk_thread_has_pollers(struct spdk_thread *thread);

/**
 * Returns whether there are scheduled operations to be run on the thread.
 *
 * \param thread The thread to check.
 *
 * \return true if there are no scheduled operations, false otherwise.
 */
/*
 * [한국어]
 * spdk_thread_is_idle - thread가 즉시 처리할 작업이 없는지 검사.
 *
 * @thread: 검사 대상.
 * @return: true=메시지 큐 비고 만료된 timed poller도 없음, false=처리할 일 있음.
 *
 * 활용: scheduler가 thread를 다른 코어로 마이그레이션 직전 "안전" 시점 확인.
 */
bool spdk_thread_is_idle(struct spdk_thread *thread);

/**
 * Get count of allocated threads.
 */
/*
 * [한국어]
 * spdk_thread_get_count - 현재 살아있는(=destroy되지 않은) spdk_thread의 총 개수.
 *
 * @return: 양의 정수. 0이면 lib_init 후 아직 thread 생성 전 또는 모두 destroy된 상태.
 *
 * 활용: app shutdown 진행 정도 확인 (모든 thread exit/destroy 완료 시 0).
 */
uint32_t spdk_thread_get_count(void);

/**
 * Get a handle to the current thread.
 *
 * This handle may be passed to other threads and used as the target of
 * spdk_thread_send_msg().
 *
 * \sa spdk_io_channel_get_thread()
 *
 * \return a pointer to the current thread on success or NULL on failure.
 */
/*
 * [한국어]
 * spdk_get_thread - 현재 pthread의 TLS에 바인딩된 spdk_thread 반환.
 *
 * @return: 현재 컨텍스트의 spdk_thread, 컨텍스트 없는 임의 pthread에서는 NULL.
 *
 * 구현: pthread-specific key (또는 __thread 변수)에서 읽음. spdk_set_thread() 또는 spdk_thread_poll()이
 *       이 값을 갱신한다.
 * 활용: 1) 다른 thread로 메시지 보낼 때 자기 자신 핸들 확보 2) 현재 thread에 poller 등록 시 묵시적 대상.
 *      3) assert 문에서 호출 컨텍스트 검증.
 */
struct spdk_thread *spdk_get_thread(void);

/**
 * Get a thread's name.
 *
 * \param thread Thread to query.
 *
 * \return the name of the thread.
 */
/*
 * [한국어]
 * spdk_thread_get_name - thread 이름 문자열 반환 (디버깅/로그용).
 *
 * @thread: 대상.
 * @return: spdk_thread_create() 시 strdup된 문자열 포인터. 호출자는 해제 금지.
 */
const char *spdk_thread_get_name(const struct spdk_thread *thread);

/**
 * Get a thread's ID.
 *
 * \param thread Thread to query.
 *
 * \return the ID of the thread..
 */
/*
 * [한국어]
 * spdk_thread_get_id - thread의 단조 증가 식별자.
 *
 * @thread: 대상.
 * @return: lib_init 이후 단조 증가하는 64비트 ID. 1부터 시작 (0은 invalid 값).
 *
 * 용도: trace event/RPC 응답 등 외부에서 thread를 stable하게 가리키는 키 (포인터는 destroy 후 재사용
 *       가능하므로 안전하지 않음).
 */
uint64_t spdk_thread_get_id(const struct spdk_thread *thread);

/**
 * Get the thread by the ID.
 *
 * \param id ID of the thread.
 * \return Thread whose ID matches or NULL otherwise.
 */
/*
 * [한국어]
 * spdk_thread_get_by_id - get_id()의 역함수. ID로 thread 핸들 검색.
 *
 * @id:     찾을 thread ID.
 * @return: 일치하는 spdk_thread 또는 NULL (해당 ID의 thread가 destroy된 경우).
 *
 * 동시성: 글로벌 thread 트리(스핀락 보호) 검색. 호출 시점과 반환 직후 사이에 thread가 destroy될 수
 *         있으니 이후 사용 시점에는 spdk_thread_send_msg가 EINVAL 반환할 수 있다.
 */
struct spdk_thread *spdk_thread_get_by_id(uint64_t id);

/*
 * [한국어]
 * struct spdk_thread_stats - thread별 누적 실행 시간 통계.
 *
 * spdk_thread_get_stats()가 이 구조체를 채워 반환한다. RPC framework_get_reactors가 모든 thread에
 * 본 통계를 모아 reactor 부하 분석/스케줄러 입력으로 사용.
 */
struct spdk_thread_stats {
	uint64_t busy_tsc;
	/* [한국어] poller가 SPDK_POLLER_BUSY를 반환했거나 메시지를 실제로 처리한 라운드들의 누적 TSC.
	 * 설정자: spdk_thread_poll() 본체가 한 라운드 끝에서 (end_tsc - start_tsc)를 가산.
	 * 읽는 자: scheduler/RPC가 부하 비율(busy/(busy+idle)) 계산.
	 * 단위: CPU TSC ticks (rdtsc 카운터). spdk_get_ticks_hz()로 초로 환산 가능.
	 * 동기화: thread 단독 소유 필드라 락 없이 읽기/쓰기. */
	uint64_t idle_tsc;
	/* [한국어] poller가 SPDK_POLLER_IDLE만 반환하고 메시지도 없었던 라운드들의 누적 TSC.
	 * 설정자: spdk_thread_poll() (위와 동일).
	 * 읽는 자: scheduler가 idle 비율 임계 초과 시 thread를 다른 reactor로 통합 결정.
	 * 동기화: 단독 소유. */
};

/**
 * Get statistics about the current thread.
 *
 * Copy cumulative thread stats values to the provided thread stats structure.
 *
 * \param stats User's thread_stats structure.
 */
/*
 * [한국어]
 * spdk_thread_get_stats - 현재 thread의 누적 통계를 사용자 버퍼로 복사.
 *
 * @stats:  출력 버퍼 (호출자 할당). busy_tsc/idle_tsc가 채워짐.
 * @return: 0 성공, -EINVAL=현재 spdk_thread 컨텍스트가 없음.
 *
 * 호출 컨텍스트: 반드시 spdk_thread 위에서 호출 (TLS 컨텍스트 필요). cross-thread 통계가 필요하면
 *               대상 thread로 send_msg를 보내 거기서 본 함수를 호출하는 패턴 사용.
 * 호출 체인: RPC thread_get_stats / framework_get_reactors -> 각 thread send_msg -> 본 함수.
 */
int spdk_thread_get_stats(struct spdk_thread_stats *stats);

/**
 * Return the TSC value from the end of the last time this thread was polled.
 *
 * \param thread Thread to query.  If NULL, use current thread.
 *
 * \return TSC value from the end of the last time this thread was polled.
 */
/*
 * [한국어]
 * spdk_thread_get_last_tsc - 마지막 spdk_thread_poll() 종료 시점의 TSC 값 반환.
 *
 * @thread: 대상. NULL이면 현재 thread.
 * @return: 그 thread의 마지막 polling 라운드 종료 TSC.
 *
 * 활용: poller 콜백 안에서 "마지막 polling 시각으로부터 얼마나 지났는지" 계산. spdk_get_ticks()를
 *      매번 호출하지 않고 캐시된 값을 사용해 syscall 절감.
 */
uint64_t spdk_thread_get_last_tsc(struct spdk_thread *thread);

/**
 * Send a message to the given thread.
 *
 * The message will be sent asynchronously - i.e. spdk_thread_send_msg will always return
 * prior to `fn` being called.
 *
 * Errors are handled internally and are fatal. Calling code can skip checking the return
 * value as it has been left only for compatibility.
 *
 * \param thread The target thread.
 * \param fn This function will be called on the given thread.
 * \param ctx This context will be passed to fn when called.
 *
 * \return 0 left for API compatibility
 */
/*
 * [한국어]
 * spdk_thread_send_msg - target spdk_thread에 비동기 메시지 발행 (lockless ring 기반).
 *
 * @thread: 메시지 수신 thread (현재 thread도 가능).
 * @fn:     target에서 실행될 콜백.
 * @ctx:    fn에 전달될 인자 포인터 (수명 관리는 호출자 책임).
 * @return: 0 (호환성 유지용 고정 반환값).
 *
 * 동기/배경: SPDK lockless 설계의 핵심. cross-thread에서 자료구조에 직접 접근하는 대신, 메시지로
 *          target thread에서 실행하게 만들어 자료구조 락을 제거한다.
 * 동작 단계:
 *   1) 메시지 mempool(rte_mempool)에서 spdk_msg 객체 dequeue (per-thread cache 1024개로 빠름)
 *   2) {fn, ctx}를 객체에 기록
 *   3) target->messages ring(rte_ring, MPSC)에 enqueue
 *   4) 즉시 반환 (콜백 실행은 target thread의 다음 spdk_thread_poll() 라운드에서 발생)
 * 실행 컨텍스트: 어느 thread/pthread에서 호출해도 안전 (signal handler 제외 - 그건 critical_msg 사용).
 * 에러 경로: mempool/ring 고갈 시 내부적으로 fatal abort (return value로 보고하지 않음).
 * 사용 예: spdk_bdev_io_complete()가 다른 thread에서 발생한 완료를 원래 thread로 라우팅할 때.
 */
int spdk_thread_send_msg(const struct spdk_thread *thread, spdk_msg_fn fn, void *ctx);

/**
 * Send a message to the given thread. Only one critical message can be outstanding at the same
 * time. It's intended to use this function in any cases that might interrupt the execution of the
 * application, such as signal handlers.
 *
 * The message will be sent asynchronously - i.e. spdk_thread_send_critical_msg will always return
 * prior to `fn` being called.
 *
 *  Errors are handled internally and are fatal. Calling code can skip checking the return
 * value as it has been left only for compatibility.
 *
 * \param thread The target thread.
 * \param fn This function will be called on the given thread.
 *
 * \return 0 left for API compatibility
 */
/*
 * [한국어]
 * spdk_thread_send_critical_msg - signal handler 등에서 안전한 단일-슬롯 메시지 발행.
 *
 * @thread: target.
 * @fn:     실행될 콜백 (ctx 인자 없음).
 * @return: 0 (호환성).
 *
 * 동기/배경: 일반 send_msg는 mempool에서 메시지를 동적 할당한다. 이는 signal handler에서는
 *          async-signal-safe하지 않다 (mempool 내부 락/atomic 때문). critical_msg는 thread별로
 *          미리 예약된 1슬롯을 사용하며 락 없이 atomic exchange로 swap하기 때문에 SIGINT/SIGTERM
 *          핸들러 안에서도 안전하게 호출 가능하다.
 * 제약: 이름대로 한 thread당 동시에 1개만 outstanding 가능. 이미 슬롯 점유 시 실패하지만 fatal abort.
 * 사용 예: spdk_app_start()가 등록하는 SIGINT 핸들러 -> app thread에 정상 종료 메시지 전송.
 */
int spdk_thread_send_critical_msg(struct spdk_thread *thread, spdk_msg_fn fn);

/**
 * Run the msg callback on the given thread. If this happens to be the current
 * thread, the callback is executed immediately; otherwise a message is sent to
 * the thread, and it's run asynchronously.
 *
 * Errors are handled internally and are fatal. Calling code can skip checking the return
 * value as it has been left only for compatibility.
 *
 * \param thread The target thread.
 * \param fn This function will be called on the given thread.
 * \param ctx This context will be passed to fn when called.
 *
 * \return 0 left for API compatibility
 */
/*
 * [한국어]
 * spdk_thread_exec_msg - "현재 thread면 즉시, 아니면 send_msg" 자동 선택 헬퍼 (인라인).
 *
 * @thread: 실행 대상. NULL은 abort 대상 (assert).
 * @fn:     콜백.
 * @ctx:    인자.
 * @return: 0.
 *
 * 동기: 호출 코드가 매번 "지금 내가 target에 있는가?"를 분기하는 보일러플레이트 제거. 단,
 *      "즉시 호출" 분기에선 fn이 동기 실행됨에 주의 - send_msg는 항상 비동기인 것과 달라
 *      재진입성/락 순서 분석이 달라질 수 있다.
 */
static inline int
spdk_thread_exec_msg(const struct spdk_thread *thread, spdk_msg_fn fn, void *ctx)
{
	assert(thread != NULL);
	/* [한국어] thread 인자가 NULL이면 절대 호출되어선 안 됨 - 디버그 빌드에서 abort.
	 * release 빌드에선 다음 분기에서 spdk_get_thread() != NULL이면 send_msg로 가는데
	 * 거기서 NULL 검사로 fatal 처리됨. */

	if (spdk_get_thread() == thread) {
		/* [한국어] 현재 pthread의 spdk_thread 컨텍스트가 target과 일치하면, 메시지 큐를 거치지 않고
		 * 콜백을 동기 실행. 이는 hot path 최적화 - cross-thread가 아닐 때 ring 비용을 절약한다.
		 * 주의: fn 안에서 또 같은 thread에 exec_msg하면 재귀 동기 호출이 일어나므로 콜백 작성 시 유의. */
		fn(ctx);
		/* [한국어] 즉시 호출이므로 fn 종료 후 바로 0을 반환해 호출자에게 완료 통지. */
		return 0;
	}

	/* [한국어] 다른 thread가 target이면 lockless ring을 통한 비동기 전달로 fall-through.
	 * send_msg가 0을 반환하므로 본 함수도 0을 반환한다. */
	return spdk_thread_send_msg(thread, fn, ctx);
}

/**
 * Send a message to each thread, serially.
 *
 * The message is sent asynchronously - i.e. spdk_for_each_thread will return
 * prior to `fn` being called on each thread.
 *
 * \param fn This is the function that will be called on each thread.
 * \param ctx This context will be passed to fn when called.
 * \param cpl This will be called on the originating thread after `fn` has been
 * called on each thread. Optional - may be NULL.
 */
/*
 * [한국어]
 * spdk_for_each_thread - 모든 spdk_thread에 직렬로 메시지를 발행.
 *
 * @fn:  각 thread에서 호출될 콜백.
 * @ctx: fn에 전달될 인자 (모든 thread가 같은 ctx 공유 - 동시성 주의).
 * @cpl: 모든 thread에서 fn 실행이 끝나면 originating thread에서 호출되는 완료 콜백 (NULL 가능).
 *
 * 동기/배경: 전역 상태 갱신(예: 모든 thread의 stat 리셋)이나 시스템 차원 셧다운 시퀀스 처리.
 * 동작: 내부적으로 thread 리스트를 캡처 후 첫 thread에 send_msg -> 그 thread는 fn 실행 후 다음
 *      thread로 send_msg 릴레이 -> 마지막 thread 후 cpl을 originating thread로 send_msg.
 * 직렬성 보장: 두 fn 콜백이 동시에 실행되지 않음 (한 thread 처리 후 다음 thread로 넘김).
 * ctx 수명: 마지막 cpl이 호출될 때까지 유효해야 함. 보통 cpl 안에서 free.
 */
void spdk_for_each_thread(spdk_msg_fn fn, void *ctx, spdk_msg_fn cpl);

/**
 * Set current spdk_thread into interrupt mode or back to poll mode.
 *
 * Only valid when thread interrupt facility is enabled by
 * spdk_interrupt_mode_enable().
 *
 * \param enable_interrupt Set interrupt mode for true, or poll mode for false
 */
/*
 * [한국어]
 * spdk_thread_set_interrupt_mode - 현재 thread를 인터럽트 모드/폴 모드로 전환.
 *
 * @enable_interrupt: true=인터럽트(epoll 대기), false=폴(busy loop).
 *
 * 전제: spdk_interrupt_mode_enable()로 전체 인터럽트 facility가 켜져 있어야 함. 안 그러면 무시.
 * 효과: 모든 등록된 poller에 대해 등록된 set_interrupt_mode_cb 호출 -> 각 모듈이 fd를 epoll에 등록/해제.
 *      thread의 fd_group은 reactor가 외부 epoll에 합성하여 이벤트 발생 시에만 spdk_thread_poll() 호출.
 * CPU 트레이드오프: polled-mode 대비 latency 증가 (epoll 깨움 비용)하지만 idle 코어 100% 사용을 회피.
 */
void spdk_thread_set_interrupt_mode(bool enable_interrupt);

/**
 * Get trace id.
 *
 * \param thread Thread to get trace_id from.
 *
 * \return Trace id of the specified thread.
 */
/*
 * [한국어]
 * spdk_thread_get_trace_id - SPDK trace 프레임워크용 16비트 owner ID.
 *
 * @thread: 대상.
 * @return: 16비트 trace owner. 추적 이벤트 기록 시 owner 필드에 들어가 추후 thread별 분석 가능.
 *
 * 64비트 ID와의 차이: trace 이벤트 헤더 공간 절약을 위해 16비트 압축 형태. 동시 살아있는 thread 수가
 *                  2^16을 넘지 않는다는 가정 하에 이 ID를 발급한다.
 */
uint16_t spdk_thread_get_trace_id(struct spdk_thread *thread);

/**
 * Register a poller on the current thread.
 *
 * The poller can be unregistered by calling spdk_poller_unregister().
 *
 * \param fn This function will be called every `period_microseconds`.
 * \param arg Argument passed to fn.
 * \param period_microseconds How often to call `fn`. If 0, call `fn` as often
 *  as possible.
 *
 * \return a pointer to the poller registered on the current thread on success
 * or NULL on failure.
 */
/*
 * [한국어]
 * spdk_poller_register - 현재 thread에 poller 등록 (이름 자동 설정 없음).
 *
 * @fn:                  주기적으로 호출될 콜백 (spdk_poller_fn 시그니처).
 * @arg:                 fn에 전달될 사용자 컨텍스트.
 * @period_microseconds: 호출 주기 (us). 0이면 매 spdk_thread_poll() 라운드마다 호출.
 * @return:              등록된 poller 핸들. NULL이면 실패 (메모리 부족 등).
 *
 * 등록 위치: 호출 시점의 현재 spdk_thread (spdk_get_thread() 결과). 다른 thread에 등록하려면 send_msg
 *          패턴 사용.
 * 동작: 새 spdk_poller 객체 할당 -> period에 따라 active 리스트(0us) 또는 timed RB-tree에 삽입 ->
 *      spdk_thread_poll()이 다음 라운드부터 호출.
 * 사용 예: bdev_nvme 모듈이 NVMe CQ 폴링 함수를 0us 주기로 등록 -> reactor 매 라운드 CQ 검사.
 */
struct spdk_poller *spdk_poller_register(spdk_poller_fn fn,
		void *arg,
		uint64_t period_microseconds);

/**
 * Register a poller on the current thread with arbitrary name.
 *
 * The poller can be unregistered by calling spdk_poller_unregister().
 *
 * \param fn This function will be called every `period_microseconds`.
 * \param arg Argument passed to fn.
 * \param period_microseconds How often to call `fn`. If 0, call `fn` as often
 *  as possible.
 * \param name Human readable name for the poller. Pointer of the poller function
 * name is set if NULL.
 *
 * \return a pointer to the poller registered on the current thread on success
 * or NULL on failure.
 */
/*
 * [한국어]
 * spdk_poller_register_named - 사용자 지정 이름으로 poller 등록.
 *
 * @fn:                  콜백.
 * @arg:                 인자.
 * @period_microseconds: 주기 (0=매 라운드).
 * @name:                디버그/관측용 이름 (NULL이면 fn 함수 포인터의 디버그 심볼 사용).
 * @return:              등록된 poller, NULL=실패.
 *
 * register()와의 차이: name 인자만 추가. SPDK_POLLER_REGISTER 매크로가 #fn으로 자동 stringification.
 * 활용: trace 도구 / RPC framework_get_pollers가 이 이름을 그대로 노출 -> 운영 가시성.
 */
struct spdk_poller *spdk_poller_register_named(spdk_poller_fn fn,
		void *arg,
		uint64_t period_microseconds,
		const char *name);

/*
 * \brief Register a poller on the current thread with setting its name
 * to the string of the poller function name. The poller being registered
 * should return a value of type `enum spdk_thread_poller_rc`. See
 * \ref spdk_poller_fn for more information.
 */
/* [한국어] SPDK_POLLER_REGISTER - poller 함수명을 이름으로 자동 사용하는 등록 매크로.
 * #fn으로 함수 식별자를 문자열화하여 register_named에 전달한다.
 * SPDK 코드 전반에서 이 매크로가 표준 등록 방식이며, 이름 일관성과 가독성을 위해 권장.
 * 예) SPDK_POLLER_REGISTER(bdev_nvme_poll, ctx, 0); -> 이름 = "bdev_nvme_poll". */
#define SPDK_POLLER_REGISTER(fn, arg, period_microseconds)	\
	spdk_poller_register_named(fn, arg, period_microseconds, #fn)

/**
 * Unregister a poller on the current thread.
 *
 * This function will also write NULL to the spdk_poller pointer pointed
 * to by ppoller, to help encourage a poller pointer not getting reused
 * after it has been unregistered.
 *
 * It is OK to pass a ppoller parameter that points to NULL, in this case
 * the function is a nop.
 *
 * \param ppoller The poller to unregister.
 */
/*
 * [한국어]
 * spdk_poller_unregister - 등록된 poller 해제 (포인터에 NULL 자동 기록).
 *
 * @ppoller: poller 포인터의 주소 (이중 포인터). 함수가 *ppoller=NULL로 갱신하여 dangling 사용 방지.
 *
 * 동작: 1) *ppoller가 NULL이면 nop 2) 그 외에는 poller를 리스트에서 제거하고 destroy 메시지를 큐잉
 *      3) 즉시 *ppoller=NULL로 표시 (호출자가 다음 줄에서 스스로 NULL 검사 가능).
 * 호출 컨텍스트: 반드시 poller가 등록된 thread 위에서.
 * deferred destroy: poller 콜백이 실행 중이면 즉시 free하지 않고 다음 spdk_thread_poll() 끝에서 처리.
 */
void spdk_poller_unregister(struct spdk_poller **ppoller);

/**
 * Pause a poller on the current thread.
 *
 * The poller is not run until it is resumed with spdk_poller_resume().  It is
 * perfectly fine to pause an already paused poller.
 *
 * \param poller The poller to pause.
 */
/*
 * [한국어]
 * spdk_poller_pause - poller를 일시 정지 (resume 전까지 호출 안 됨).
 *
 * @poller: 일시 정지할 poller. 이미 paused면 nop.
 *
 * 활용: 디바이스 reset 진행 중 일시적으로 폴링 중단, 전원 관리 시퀀스 등.
 * 호출 컨텍스트: poller가 등록된 thread 위.
 */
void spdk_poller_pause(struct spdk_poller *poller);

/**
 * Resume a poller on the current thread.
 *
 * Resumes a poller paused with spdk_poller_pause().  It is perfectly fine to
 * resume an unpaused poller.
 *
 * \param poller The poller to resume.
 */
/*
 * [한국어]
 * spdk_poller_resume - 일시 정지된 poller를 다시 활성화.
 *
 * @poller: resume할 poller. paused 상태가 아니면 nop.
 *
 * 호출 컨텍스트: poller가 등록된 thread 위.
 */
void spdk_poller_resume(struct spdk_poller *poller);

/**
 * Register the opaque io_device context as an I/O device.
 *
 * After an I/O device is registered, it can return I/O channels using the
 * spdk_get_io_channel() function.
 *
 * \param io_device The pointer to io_device context.
 * \param create_cb Callback function invoked to allocate any resources required
 * for a new I/O channel.
 * \param destroy_cb Callback function invoked to release the resources for an
 * I/O channel.
 * \param ctx_size The size of the context buffer allocated to store references
 * to allocated I/O channel resources.
 * \param name A string name for the device used only for debugging. Optional -
 * may be NULL.
 */
/*
 * [한국어]
 * spdk_io_device_register - 모듈을 "I/O 디바이스"로 등록 (per-thread 채널 생성 콜백 동반).
 *
 * @io_device:  디바이스 식별자 포인터. 보통 모듈의 디바이스 구조체 주소를 그대로 사용.
 * @create_cb:  새 (thread, device) 쌍에 대해 첫 spdk_get_io_channel 호출 시 실행될 채널 초기화 콜백.
 * @destroy_cb: 채널의 마지막 ref 해제 시 호출될 정리 콜백.
 * @ctx_size:   채널마다 자동 할당될 컨텍스트 버퍼 크기. spdk_io_channel_get_ctx로 접근.
 * @name:       디버그용 이름 (RPC framework_get_io_devices에 노출).
 *
 * 동기/배경: lockless 설계의 또 다른 핵심. "디바이스"는 하나지만 그 디바이스를 사용하는 각 thread는
 *          자기 전용 channel을 받아 거기에 lockless로 접근한다. 채널이 thread에 고정되어 있으니
 *          모듈은 채널 컨텍스트의 자료구조에 락 없이 접근 가능.
 * 동작: 글로벌 io_device 레지스트리(스핀락 보호 RB-tree)에 등록 -> 이후 spdk_get_io_channel(io_device)
 *      호출이 가능해진다.
 * 호출 컨텍스트: 일반적으로 app thread에서 모듈 init 단계에 호출. 다른 thread에서도 가능하나 권장 X.
 */
void spdk_io_device_register(void *io_device, spdk_io_channel_create_cb create_cb,
			     spdk_io_channel_destroy_cb destroy_cb, uint32_t ctx_size,
			     const char *name);

/**
 * Unregister the opaque io_device context as an I/O device.
 *
 * The actual unregistration might be deferred until all active I/O channels are
 * destroyed.
 *
 * \param io_device The pointer to io_device context.
 * \param unregister_cb An optional callback function invoked to release any
 * references to this I/O device.
 */
/*
 * [한국어]
 * spdk_io_device_unregister - io_device 등록 해제 (deferred - 잔여 채널 정리 후 완료).
 *
 * @io_device:     해제할 디바이스 포인터.
 * @unregister_cb: 모든 채널 destroy_cb 완료 후 호출될 최종 콜백 (NULL 가능 - 호출자가 polling 가능).
 *
 * deferred 의미: 본 함수 호출 즉시 새 spdk_get_io_channel은 차단되지만, 기존 채널들의 destroy는 각
 *               해당 thread에서 별도로 진행된다. 모든 채널이 사라진 시점에 unregister_cb이 호출되어
 *               비로소 io_device 메모리도 free 가능.
 * 호출 패턴: bdev unregister 시퀀스 내부 단계. 잔여 I/O 처리를 위해 비동기 정리가 필수.
 */
void spdk_io_device_unregister(void *io_device, spdk_io_device_unregister_cb unregister_cb);

/**
 * Get an I/O channel for the specified io_device to be used by the calling thread.
 *
 * The io_device context pointer specified must have previously been registered
 * using spdk_io_device_register(). If an existing I/O channel does not exist
 * yet for the given io_device on the calling thread, it will allocate an I/O
 * channel and invoke the create_cb function pointer specified in spdk_io_device_register().
 * If an I/O channel already exists for the given io_device on the calling thread,
 * its reference is returned rather than creating a new I/O channel.
 *
 * \param io_device The pointer to io_device context.
 *
 * \return a pointer to the I/O channel for this device on success or NULL on failure.
 */
/*
 * [한국어]
 * spdk_get_io_channel - 현재 thread용 io_channel 획득 (없으면 생성, 있으면 ref+1).
 *
 * @io_device: 채널을 얻을 디바이스 (이미 register된 것).
 * @return:    채널 포인터. NULL이면 device 미등록/create_cb 실패/메모리 부족 등.
 *
 * 핵심 패턴: per-thread 캐시 + 참조 카운팅.
 *   1) 현재 thread의 io_channel 트리에서 io_device 키로 검색
 *   2) 있으면 ref_count++ 후 반환 (RCU-like fast path)
 *   3) 없으면 ctx_size만큼 메모리 할당 + create_cb 호출 -> 트리에 삽입 -> 반환
 * 호출 컨텍스트: 어떤 spdk_thread에서든 호출 가능. 단 같은 채널을 다른 thread로 옮기면 안 됨 (affinity).
 * lockless 의미: 채널이 한 thread에 고정되므로 채널 컨텍스트 내부 자료구조는 락 없이 접근 가능.
 *               이게 SPDK가 NVMe qpair, bdev_io 풀 등을 락 없이 운용하는 비결.
 * 호출 체인: 앱 -> spdk_bdev_get_io_channel -> 본 함수 -> bdev 모듈의 create_cb (qpair 생성 등).
 */
struct spdk_io_channel *spdk_get_io_channel(void *io_device);

/**
 * Release a reference to an I/O channel. This happens asynchronously.
 *
 * This must be called on the same thread that called spdk_get_io_channel()
 * for the specified I/O channel. If this releases the last reference to the
 * I/O channel, The destroy_cb function specified in spdk_io_device_register()
 * will be invoked to release any associated resources.
 *
 * \param ch I/O channel to release a reference.
 */
/*
 * [한국어]
 * spdk_put_io_channel - 채널 참조 해제. 마지막 참조라면 destroy_cb 비동기 호출.
 *
 * @ch: 해제할 채널.
 *
 * 동작: 1) ref_count-- 2) ref_count==0이면 채널을 thread 트리에서 제거 후 destroy_cb 호출 메시지 큐잉
 *      3) destroy_cb 종료 후 ctx 메모리 free.
 * 스레드 affinity: 반드시 spdk_get_io_channel()을 호출했던 동일 thread에서 호출해야 함. 다른 thread에서
 *                 호출하면 트리 검색 실패 또는 데이터 레이스 발생.
 * 비동기성: docstring "happens asynchronously"는 destroy_cb 실행이 즉시가 아닌 send_msg 큐잉 후 다음
 *          poll 라운드에서 일어남을 의미. 본 함수 자체는 동기적 ref count 감소.
 */
void spdk_put_io_channel(struct spdk_io_channel *ch);

/**
 * Take a reference to an existing I/O channel.
 *
 * This can be called on an existing io_channel that was previously returned from
 * spdk_get_io_channel(). This must be called on the same thread that called
 * spdk_get_io_channel() for the specified I/O channel. spdk_put_io_channel() must
 * be called to release the reference when it is no longer needed.
 *
 * \param ch The existing I/O channel to reference.
 *
 * \return The same I/O channel pointer that was passed in.
 */
/*
 * [한국어]
 * spdk_io_channel_ref - 기존 채널의 참조 카운트만 증가 (create_cb 재호출 X).
 *
 * @ch:     기존 채널.
 * @return: 입력과 동일한 ch 포인터 (체이닝 편의).
 *
 * vs spdk_get_io_channel: get_io_channel은 트리 검색 후 없으면 생성하지만, ref()는 이미 손에 든 채널의
 *                       카운트만 +1. 빠른 경로.
 * 사용 예: bdev_io_submit()이 채널을 잡고 비동기 작업 동안 추가 ref를 잡아 채널의 조기 destroy 방지.
 * 짝: 반드시 같은 횟수만큼 spdk_put_io_channel() 호출 필요.
 */
struct spdk_io_channel *spdk_io_channel_ref(struct spdk_io_channel *ch);

/**
 * Get the context buffer associated with an I/O channel.
 *
 * \param ch I/O channel.
 *
 * \return a pointer to the context buffer.
 */
/*
 * [한국어]
 * spdk_io_channel_get_ctx - 채널 헤더 뒤의 모듈 컨텍스트 버퍼 주소 계산 (인라인).
 *
 * @ch:     채널 포인터.
 * @return: ctx 시작 주소 (= ch + 96B), ch가 NULL이면 NULL (assert).
 *
 * 메모리 레이아웃: 채널은 [헤더 96B][모듈 ctx_size 바이트] 연속 메모리 한 덩어리로 할당.
 * 인라인 이유: hot path. NVMe I/O 발행마다 호출되므로 함수 호출 비용 제거.
 */
static inline void *
spdk_io_channel_get_ctx(struct spdk_io_channel *ch)
{
	if (spdk_unlikely(!ch)) {
		/* [한국어] NULL 채널은 프로그래밍 오류 - 디버그 빌드에서 abort. release 빌드에선 NULL 반환해
		 * 호출자에 경계 처리 기회. spdk_unlikely는 분기 예측을 false 쪽으로 (hot path는 ch!=NULL). */
		assert(false);
		return NULL;
	}

	/* [한국어] 헤더 크기(96B)만큼 더한 주소를 모듈 컨텍스트로 반환. uint8_t* 캐스팅으로 byte 단위 산술 보장 -
	 * struct spdk_io_channel 자체는 불투명이라 sizeof 사용 불가하므로 SPDK_IO_CHANNEL_STRUCT_SIZE 매크로
	 * 의존. 매크로 값이 라이브러리와 헤더 사이에 일치해야 하므로 ABI 고정. */
	return (uint8_t *)ch + SPDK_IO_CHANNEL_STRUCT_SIZE;
}

/**
 * Get I/O channel from the context buffer. This is the inverse of
 * spdk_io_channel_get_ctx().
 *
 * \param ctx The pointer to the context buffer.
 *
 * \return a pointer to the I/O channel associated with the context buffer.
 */
/*
 * [한국어]
 * spdk_io_channel_from_ctx - get_ctx의 역함수. 모듈 컨텍스트 포인터에서 채널 헤더 복원.
 *
 * @ctx:    이전에 get_ctx로 얻은 포인터.
 * @return: 그 ctx를 보유한 spdk_io_channel.
 *
 * 단순 산술(ctx - 96)이지만 헤더 비공개 구조체라 정식 API로 노출.
 */
struct spdk_io_channel *spdk_io_channel_from_ctx(void *ctx);

/**
 * Get the thread associated with an I/O channel.
 *
 * \param ch I/O channel.
 *
 * \return a pointer to the thread associated with the I/O channel
 */
/*
 * [한국어]
 * spdk_io_channel_get_thread - 채널이 묶여 있는 spdk_thread 반환.
 *
 * @ch:     질의 대상 채널.
 * @return: 채널 생성 시점에 spdk_get_io_channel을 호출한 그 thread.
 *
 * 활용: bdev_io 완료 시 "이 I/O를 처음 발행한 thread로 콜백을 라우팅"하기 위해 사용. 그 thread로
 *      send_msg를 보내 거기서 cb_fn 실행.
 */
struct spdk_thread *spdk_io_channel_get_thread(struct spdk_io_channel *ch);

/**
 * Call 'fn' on each channel associated with io_device.
 *
 * This happens asynchronously, so fn may be called after spdk_for_each_channel
 * returns. 'fn' will be called for each channel serially, such that two calls
 * to 'fn' will not overlap in time. After 'fn' has been called, call
 * spdk_for_each_channel_continue() to continue iterating.
 *
 * \param io_device 'fn' will be called on each channel associated with this io_device.
 * \param fn Called on the appropriate thread for each channel associated with io_device.
 * \param ctx Context buffer registered to spdk_io_channel_iter that can be obtained
 * form the function spdk_io_channel_iter_get_ctx().
 * \param cpl Called on the thread that spdk_for_each_channel was initially called
 * from when 'fn' has been called on each channel. Optional - may be NULL.
 */
/*
 * [한국어]
 * spdk_for_each_channel - 한 io_device의 모든 채널에 대해 각 소속 thread에서 fn 직렬 호출.
 *
 * @io_device: 순회 대상 디바이스.
 * @fn:        각 채널에서 호출될 콜백. iter로부터 채널/ctx 추출.
 * @ctx:       모든 콜백이 공유할 컨텍스트 (spdk_io_channel_iter_get_ctx()로 접근).
 * @cpl:       전체 순회 완료 후 originating thread에서 호출되는 콜백 (NULL 가능).
 *
 * 동기/배경: bdev reset, qos 변경, qpair drain 등 "모든 thread의 채널에 동일 작업 적용" 시 사용.
 *           각 채널은 서로 다른 thread에 있으므로, 실행을 그 thread로 위임해 lockless 유지.
 * 동작: 1) 채널 리스트 캡처(전역 락 잠시 사용) 2) 첫 채널의 thread에 send_msg -> fn 실행
 *      3) fn이 spdk_for_each_channel_continue(i, 0) 호출하면 다음 채널로 릴레이
 *      4) 마지막 채널 후 originating thread로 cpl 라우팅.
 * 직렬성: 두 fn이 동시에 실행되지 않음 - 하나가 _continue 호출 후에야 다음 fn 시작.
 */
void spdk_for_each_channel(void *io_device, spdk_channel_msg fn, void *ctx,
			   spdk_channel_for_each_cpl cpl);

/**
 * Get io_device from the I/O channel iterator.
 *
 * \param i I/O channel iterator.
 *
 * \return a pointer to the io_device.
 */
/*
 * [한국어]
 * spdk_io_channel_iter_get_io_device - iterator에서 io_device 포인터 추출.
 *
 * @i:      현재 iter (콜백 인자).
 * @return: spdk_for_each_channel()에 전달했던 io_device.
 *
 * 활용: 콜백 안에서 디바이스 컨텍스트(예: bdev) 접근.
 */
void *spdk_io_channel_iter_get_io_device(struct spdk_io_channel_iter *i);

/**
 * Get I/O channel from the I/O channel iterator.
 *
 * \param i I/O channel iterator.
 *
 * \return a pointer to the I/O channel.
 */
/*
 * [한국어]
 * spdk_io_channel_iter_get_channel - 현재 순회 중인 채널 추출.
 *
 * @i:      iter.
 * @return: 현재 콜백이 처리 중인 채널 포인터. 콜백이 끝난 후엔 무효해질 수 있음.
 */
struct spdk_io_channel *spdk_io_channel_iter_get_channel(struct spdk_io_channel_iter *i);

/**
 * Get context buffer from the I/O channel iterator.
 *
 * \param i I/O channel iterator.
 *
 * \return a pointer to the context buffer.
 */
/*
 * [한국어]
 * spdk_io_channel_iter_get_ctx - for_each_channel 호출 시 전달한 사용자 ctx 추출.
 *
 * @i:      iter.
 * @return: spdk_for_each_channel(io_device, fn, ctx, cpl)의 ctx.
 *
 * 사용 패턴: 모든 콜백/cpl이 공유하는 작업 상태 (예: 카운터, 결과 배열) 보관처.
 */
void *spdk_io_channel_iter_get_ctx(struct spdk_io_channel_iter *i);

/**
 * Get the io_device for the specified I/O channel.
 *
 * \param ch I/O channel.
 *
 * \return a pointer to the io_device for the I/O channel
 */
/*
 * [한국어]
 * spdk_io_channel_get_io_device - 채널이 속한 io_device 포인터 반환.
 *
 * @ch:     질의 대상.
 * @return: spdk_get_io_channel(io_device) 시 사용했던 io_device.
 *
 * 활용: 채널만 가지고 디바이스 컨텍스트(예: bdev_disk) 회수가 필요할 때.
 */
void *spdk_io_channel_get_io_device(struct spdk_io_channel *ch);

/**
 * Helper function to iterate all channels for spdk_for_each_channel().
 *
 * \param i I/O channel iterator.
 * \param status Status for the I/O channel iterator;
 * for non 0 status remaining iterations are terminated.
 */
/*
 * [한국어]
 * spdk_for_each_channel_continue - 한 채널 처리를 마치고 다음 채널로 진행시키는 시그널.
 *
 * @i:      현재 iter (fn 콜백에서 받은 것).
 * @status: 0=정상, 음수=중단(이후 채널 처리 건너뛰고 cpl로 직행). 보통 errno 값.
 *
 * 동기/배경: 비동기 작업(예: 채널에서 I/O drain)을 지원하기 위해 명시적 continue 모델. fn 콜백이
 *          비동기 작업을 시작하고 즉시 반환해도, 작업 완료 콜백에서 _continue를 호출하면 다음 채널로
 *          진행. 동기 작업이라면 fn 끝에서 바로 _continue.
 * 보장: 본 함수가 호출되지 않으면 spdk_for_each_channel은 영원히 멈춰 있음 (의도한 흐름 제어).
 */
void spdk_for_each_channel_continue(struct spdk_io_channel_iter *i, int status);

/**
 * A representative for registered interrupt file descriptor.
 */
/*
 * [한국어]
 * struct spdk_interrupt - 등록된 인터럽트 fd의 핸들 (불투명).
 *
 * 실체는 lib/thread/thread.c 내부. fd_group(epoll wrapper) 항목 + 콜백 + 사용자 컨텍스트를 묶는다.
 * 인터럽트 모드 / interrupt-driven 폴링 모드에서 fd가 readable해질 때 콜백을 트리거한다.
 */
struct spdk_interrupt;

/**
 * Callback function registered for interrupt file descriptor.
 *
 * \param ctx Context passed as arg to spdk_interrupt_register().
 *
 * \return 0 to indicate that interrupt took place but no events were found;
 * positive to indicate that interrupt took place and some events were processed;
 * negative if no event information is provided.
 */
/*
 * [한국어]
 * spdk_interrupt_fn - 인터럽트 콜백 시그니처 (epoll에서 fd가 ready될 때 호출).
 *
 * @ctx:    spdk_interrupt_register()의 arg 인자.
 * @return: 0=인터럽트 발생했으나 처리 가능한 이벤트 없음, 양수=실제 처리됨, 음수=정보 없음/추적 안 함.
 *
 * 실행 컨텍스트: 인터럽트가 등록된 spdk_thread의 spdk_thread_poll() 내부 epoll dispatch 단계.
 *               OS의 시그널 핸들러 컨텍스트가 아님 (그래서 일반 SPDK API 호출 가능).
 */
typedef int (*spdk_interrupt_fn)(void *ctx);

/**
 * Register an spdk_interrupt on the current thread.
 *
 * The provided function will be called any time a SPDK_INTERRUPT_EVENT_IN event
 * triggers on the associated file descriptor.
 *
 * \param efd File descriptor of the spdk_interrupt.
 * \param fn Called each time there are events in spdk_interrupt.
 * \param arg Function argument for fn.
 * \param name Human readable name for the spdk_interrupt. Pointer of the spdk_interrupt
 * name is set if NULL.
 *
 * \return a pointer to the spdk_interrupt registered on the current thread on success
 * or NULL on failure.
 */
/*
 * [한국어]
 * spdk_interrupt_register - fd 1개에 EPOLLIN 인터럽트 콜백 등록 (가장 단순한 변형).
 *
 * @efd:    모니터링할 fd (eventfd, timerfd, signalfd, socket 등). 보통 eventfd_create()로 생성.
 * @fn:     fd가 ready될 때 호출될 spdk_interrupt_fn.
 * @arg:    fn에 전달될 컨텍스트.
 * @name:   디버깅 이름. NULL이면 fn 함수 포인터 주소 사용.
 * @return: spdk_interrupt 핸들 (unregister에 사용). NULL=실패.
 *
 * 동작: 현재 thread의 fd_group에 fd를 EPOLLIN(레벨 트리거)으로 add. spdk_thread_poll()이 epoll_wait
 *       으로 ready된 fd 중 본 fd가 있으면 fn(arg) 호출.
 * 사용 예: NVMe completion eventfd, vhost socket fd, RPC 서버 listen fd 등.
 * 호출 컨텍스트: 등록 thread = 콜백 실행 thread. 다른 thread에 등록은 send_msg 위임 필요.
 */
struct spdk_interrupt *spdk_interrupt_register(int efd, spdk_interrupt_fn fn,
		void *arg, const char *name);

/**
 * Register an spdk_interrupt with specific event types on the current thread.
 *
 * The provided function will be called any time one of specified event types triggers on
 * the associated file descriptor.
 * Event types argument is a bit mask composed by ORing together
 * enum spdk_interrupt_event_types values.
 *
 * \param efd File descriptor of the spdk_interrupt.
 * \param events Event notification types.
 * \param fn Called each time there are events in spdk_interrupt.
 * \param arg Function argument for fn.
 * \param name Human readable name for the spdk_interrupt. Pointer of the spdk_interrupt
 * name is set if NULL.
 *
 * \return a pointer to the spdk_interrupt registered on the current thread on success
 * or NULL on failure.
 */
/*
 * [한국어]
 * spdk_interrupt_register_for_events - 특정 event mask로 인터럽트 등록.
 *
 * @efd:    fd.
 * @events: SPDK_INTERRUPT_EVENT_IN/OUT/ET 비트 OR. ET=Edge-Trigger 모드 (한 번만 알림).
 * @fn:     콜백.
 * @arg:    콜백 컨텍스트.
 * @name:   디버깅 이름.
 * @return: spdk_interrupt 핸들 또는 NULL.
 *
 * vs register: 위 변형은 EPOLLIN 고정이지만, 본 함수는 IN/OUT/ET 조합 지정. EPOLLOUT은 write-ready 알림용
 *             (예: 큰 RPC 응답 송신 backpressure 감지).
 * EPOLLET (Edge-Trigger): 한 ready 이벤트당 1번만 콜백 호출. 콜백이 모든 데이터를 한 번에 처리해야 함.
 *                        Level-Trigger 대비 시스템 콜 수 감소.
 */
struct spdk_interrupt *spdk_interrupt_register_for_events(int efd, uint32_t events,
		spdk_interrupt_fn fn, void *arg, const char *name);

/**
 * Register an spdk_interrupt with specific event type stated in spdk_event_handler_opts argument
 * on the current thread.
 *
 * The provided function will be called any time one of specified event types from
 * spdk_event_handler_opts argument triggers on the associated file descriptor.
 * Event types argument in spdk_event_handler_opts is a bit mask composed by ORing together
 * enum spdk_interrupt_event_types values.
 *
 * \param efd File descriptor of the spdk_interrupt.
 * \param fn Called each time there are events in spdk_interrupt.
 * \param arg Function argument for fn.
 * \param name Human readable name for the spdk_interrupt. Pointer of the spdk_interrupt
 * name is set if NULL.
 * \param opts Extended event handler option.
 *
 * \return a pointer to the spdk_interrupt registered on the current thread on success
 * or NULL on failure.
 */
/*
 * [한국어]
 * spdk_interrupt_register_ext - 확장 옵션(spdk_event_handler_opts)을 받는 변형.
 *
 * @efd:    fd.
 * @fn:     콜백.
 * @arg:    콜백 컨텍스트.
 * @name:   디버깅 이름.
 * @opts:   확장 옵션 구조체 (event mask, fd_type, opts_size 등 — env.h 참조).
 *          ABI 안전성 위해 size 필드를 가지므로 향후 필드 추가에도 호환.
 * @return: spdk_interrupt 핸들 또는 NULL.
 *
 * 사용처: BPF event ring 같은 fd 종류별 특수 옵션 필요 시.
 */
struct spdk_interrupt *spdk_interrupt_register_ext(int efd, spdk_interrupt_fn fn, void *arg,
		const char *name, struct spdk_event_handler_opts *opts);

/*
 * \brief Register an spdk_interrupt on the current thread with setting its name
 * to the string of the spdk_interrupt function name.
 */
/* [한국어] SPDK_INTERRUPT_REGISTER 매크로 - register()를 fn 이름으로 자동 명명. 일반 사용 패턴.
 * spdk_interrupt_register(efd, fn, arg, "fn") 와 동일하나 함수명을 자동 stringify. */
#define SPDK_INTERRUPT_REGISTER(efd, fn, arg)	\
	spdk_interrupt_register(efd, fn, arg, #fn)

/*
 * \brief Register an spdk_interrupt on the current thread with specific event types
 * and with setting its name to the string of the spdk_interrupt function name.
 */
/* [한국어] register_for_events()의 이름 자동 명명 매크로. EPOLLOUT 또는 ET 필요한 경우 사용. */
#define SPDK_INTERRUPT_REGISTER_FOR_EVENTS(efd, events, fn, arg)	\
	spdk_interrupt_register_for_events(efd, events, fn, arg, #fn)

/*
 * \brief Register an spdk_interrupt on the current thread with specific event types provided
 * in opts and with setting its name to the string of the spdk_interrupt function name.
 */
/* [한국어] register_ext()의 이름 자동 명명 매크로. 확장 opts 구조체 사용 시. */
#define SPDK_INTERRUPT_REGISTER_EXT(efd, fn, arg, opts)	\
	spdk_interrupt_register_ext(efd, fn, arg, #fn, opts)

/**
 * Register an interrupt listening for all events associated with an fd_group on current thread.
 *
 * \param fgrp fd_group describing the events to listen for.
 * \param name Name of the interrupt.
 *
 * return Pointer to spdk_interrupt or NULL in case of failure.
 */
/*
 * [한국어]
 * spdk_interrupt_register_fd_group - 외부 fd_group 전체를 한 인터럽트로 묶어 등록.
 *
 * @fgrp: 이미 채워진 spdk_fd_group (여러 fd가 들어있는 컨테이너).
 * @name: 디버깅 이름.
 * @return: spdk_interrupt 핸들 또는 NULL.
 *
 * 활용: 모듈이 자체 fd_group을 관리(예: nvmf RDMA poll group이 여러 QP fd 묶음)하고, 그 group이
 *      ready되면 모듈 자체 dispatcher를 호출하고 싶을 때. 외부 fd_group을 thread의 상위 fd_group에
 *      자식으로 nest. 트리 형태의 epoll 합성으로 effiicient bulk wakeup.
 */
struct spdk_interrupt *spdk_interrupt_register_fd_group(struct spdk_fd_group *fgrp,
		const char *name);

/**
 * Unregister an spdk_interrupt on the current thread.
 *
 * \param pintr The spdk_interrupt to unregister.
 */
/*
 * [한국어]
 * spdk_interrupt_unregister - 인터럽트 등록 해제 + 호출자 변수에 NULL 기록.
 *
 * @pintr: 인터럽트 포인터 변수 주소. *pintr==NULL이면 no-op.
 *
 * 동작: fd_group에서 efd 제거 → spdk_interrupt 메모리 free → *pintr=NULL.
 *       fd 자체는 close하지 않음 (호출자가 별도로 close 책임).
 * 호출 컨텍스트: 등록 thread 위에서만.
 */
void spdk_interrupt_unregister(struct spdk_interrupt **pintr);

/*
 * [한국어]
 * enum spdk_interrupt_event_types - register_for_events()의 event mask 비트값.
 *
 * Linux에서는 EPOLL* 매크로 그대로 매핑되어 epoll_ctl에 그대로 전달됨. 비-Linux 빌드는 placeholder
 * 숫자만 정의 (실제 인터럽트 모드는 Linux 전용 기능).
 */
enum spdk_interrupt_event_types {
#ifdef __linux__
	/* [한국어] Linux 빌드: 실제 epoll 비트값 그대로 사용. */
	SPDK_INTERRUPT_EVENT_IN = EPOLLIN,
	/* [한국어] read-ready 이벤트. 가장 흔한 사용 (eventfd, timerfd, socket recv). EPOLLIN=0x001. */
	SPDK_INTERRUPT_EVENT_OUT = EPOLLOUT,
	/* [한국어] write-ready 이벤트. socket send buffer가 비었을 때 알림. backpressure 감지에 사용. EPOLLOUT=0x004. */
	SPDK_INTERRUPT_EVENT_ET = EPOLLET
	/* [한국어] Edge-Trigger 플래그. ready 상태 변화 순간에만 1번 알림. 콜백이 EAGAIN까지 read해야 함.
	 * 시스템 콜 절감 효과 크지만 사용 까다로움. EPOLLET = (1u<<31). */
#else
	/* [한국어] 비-Linux 빌드 (FreeBSD 등): epoll 없음 → 더미 값. 실제로는 인터럽트 모드 미지원. */
	SPDK_INTERRUPT_EVENT_IN =  0x001,
	SPDK_INTERRUPT_EVENT_OUT = 0x004,
	SPDK_INTERRUPT_EVENT_ET = 1u << 31
#endif
};

/**
 * Change the event_types associated with the spdk_interrupt on the current thread.
 *
 * \param intr The pointer to the spdk_interrupt registered on the current thread.
 * \param event_types New event_types for the spdk_interrupt.
 *
 * \return 0 if success or -errno if failed.
 */
/*
 * [한국어]
 * spdk_interrupt_set_event_types - 등록된 인터럽트의 event mask를 동적 변경.
 *
 * @intr:        대상 인터럽트.
 * @event_types: 새 mask (SPDK_INTERRUPT_EVENT_*의 OR).
 * @return:      0 성공, 음수 errno 실패.
 *
 * 활용: socket이 send buffer 가득 찰 때만 EPOLLOUT 추가, 비워지면 제거하는 backpressure 패턴.
 *      매번 register/unregister보다 효율적. epoll_ctl(MOD)로 매핑됨.
 * 호출 컨텍스트: intr이 등록된 thread 위에서만.
 */
int spdk_interrupt_set_event_types(struct spdk_interrupt *intr,
				   enum spdk_interrupt_event_types event_types);

/**
 * Return a file descriptor that becomes ready whenever any of the registered
 * interrupt file descriptors are ready
 *
 * \param thread The thread to get.
 *
 * \return The spdk_interrupt fd of thread itself.
 */
/*
 * [한국어]
 * spdk_thread_get_interrupt_fd - thread의 모든 인터럽트를 합성한 epoll fd 반환 (외부 epoll 통합용).
 *
 * @thread: 대상.
 * @return: thread의 fd_group이 노출하는 epoll fd. 이 fd가 ready되면 thread 내부 어떤 fd든 ready 상태.
 *
 * 활용: SPDK를 외부 이벤트 루프(예: glib mainloop, libuv)에 임베드할 때, 해당 루프의 fd watcher에
 *      이 fd를 추가해 SPDK가 polling 필요할 때 wakeup. 이 fd 가 ready되면 외부 루프가
 *      spdk_thread_poll(thread, 0, 0)를 호출.
 * 인터럽트 모드 전제: spdk_interrupt_mode_enable()이 사전에 호출되어 있어야 의미 있음.
 */
int spdk_thread_get_interrupt_fd(struct spdk_thread *thread);

/**
 * Return an fd_group that becomes ready whenever any of the registered
 * interrupt file descriptors are ready
 *
 *
 * \param thread The thread to get.
 *
 * \return The spdk_fd_group of the thread itself.
 */
/*
 * [한국어]
 * spdk_thread_get_interrupt_fd_group - thread의 fd_group 자체를 반환 (fd 단일 정수 대신).
 *
 * @thread: 대상.
 * @return: spdk_fd_group 포인터. 이를 다시 다른 fd_group의 child로 nest 가능 (계층적 epoll).
 *
 * 활용: 모듈이 자기 fd_group을 thread fd_group 안에 nest하면, 모듈 fd가 ready될 때 thread 전체 wakeup.
 */
struct spdk_fd_group *spdk_thread_get_interrupt_fd_group(struct spdk_thread *thread);

/**
 * Set SPDK run as event driven mode
 *
 * \return 0 on success or -errno on failure
 */
/*
 * [한국어]
 * spdk_interrupt_mode_enable - SPDK 글로벌 설정으로 인터럽트 모드 활성화 (옵트인).
 *
 * @return: 0 성공, 음수 errno 실패.
 *
 * 시점: 일반적으로 spdk_app_start() 이전에 호출 (--interrupt-mode 옵션 처리 시 또는 명시적 코드).
 *      활성화되어야 spdk_thread_set_interrupt_mode()가 작동, fd_group 생성 등 인프라 코드 활성화.
 *      한 번 활성화되면 비활성화 불가 (단방향 스위치).
 */
int spdk_interrupt_mode_enable(void);

/**
 * Reports whether interrupt mode is set.
 *
 * \return True if interrupt mode is set, false otherwise.
 */
/*
 * [한국어]
 * spdk_interrupt_mode_is_enabled - 인터럽트 모드 활성화 여부 질의.
 *
 * @return: true=활성, false=폴 모드 only.
 */
bool spdk_interrupt_mode_is_enabled(void);

/**
 * A spinlock augmented with safety checks for use with SPDK.
 *
 * SPDK code that uses spdk_spinlock runs from an SPDK thread, which itself is associated with a
 * pthread. There are typically many SPDK threads associated with each pthread. The SPDK application
 * may migrate SPDK threads between pthreads from time to time to balance the load on those threads.
 * Migration of SPDK threads only happens when the thread is off CPU, and as such it is only safe to
 * hold a lock so long as an SPDK thread stays on CPU.
 *
 * It is not safe to lock a spinlock, return from the event or poller, then unlock it at some later
 * time because:
 *
 *   - Even though the SPDK thread may be the same, the SPDK thread may be running on different
 *     pthreads during lock and unlock. A pthread spinlock may consider this to be an unlock by a
 *     non-owner, which results in undefined behavior.
 *   - A lock that is acquired by a poller or event may be needed by another poller or event that
 *     runs on the same pthread. This can lead to deadlock or detection of deadlock.
 *   - A lock that is acquired by a poller or event that is needed by another poller or event that
 *     runs on a second pthread will block the second pthread from doing any useful work until the
 *     lock is released. Because the lock holder and the lock acquirer are on the same pthread, this
 *     would lead to deadlock.
 *
 * If an SPDK spinlock is used erroneously, the program will abort.
 */
/*
 * [한국어]
 * struct spdk_spinlock - "lock 후 yield 금지" 규약을 강제하는 thread-aware 스핀락.
 *
 * 일반 pthread_spinlock_t는 이 규약을 모르므로 SPDK 환경에서 다음 4가지 silent 버그 가능:
 *   1) thread 마이그레이션으로 lock holder pthread != unlock pthread → undefined behavior.
 *   2) 같은 pthread에서 다른 spdk_thread가 동시에 같은 lock 시도 → 데드락.
 *   3) 다른 pthread의 spdk_thread가 이 lock 대기 → 그 pthread 전체 정지 (모든 다른 spdk_thread 봉쇄).
 *   4) lock 잡고 poller 반환 → 다음 라운드에 unlock하려는 패턴은 항상 위험.
 *
 * 본 구조체는 lock holder thread를 기록하고, 잘못된 사용 발견 시 abort()로 프로그램을 끊어 fail-fast.
 * 따라서 SPDK 코드 내부 동기화는 반드시 본 spdk_spinlock 사용 (pthread_spinlock 직접 사용 금지).
 */
struct spdk_spinlock {
	pthread_spinlock_t spinlock;
	/* [한국어] 실제 OS 수준 spinlock. lock/unlock의 atomic 근본 수단.
	 * 설정자: spdk_spin_init() 1회 초기화.
	 * 읽는 자: spdk_spin_lock/unlock에서 pthread_spin_lock/unlock 호출.
	 * 동기화: 자기 자신이 동기화 primitive. pthread_spinlock_t는 PTHREAD_PROCESS_PRIVATE로 init. */

	struct spdk_thread *thread;
	/* [한국어] 현재 lock을 보유한 spdk_thread (NULL이면 unlocked).
	 * 설정자: spdk_spin_lock()이 락 획득 후 atomic_store, spdk_spin_unlock()이 release 직전 NULL set.
	 * 읽는 자: spdk_spin_held() (현재 thread == 이 필드 비교).
	 * 동기화: spinlock 보호 하에서만 갱신. 잘못된 thread가 unlock 시도 시 abort. */

	struct spdk_spinlock_internal *internal;
	/* [한국어] 디버그/트래킹용 비공개 상태 (held lock 리스트, 호출자 주소 등).
	 * 설정자: spdk_spin_init()이 lazy alloc.
	 * 읽는 자: spdk_spin_lock/unlock의 검증 코드.
	 * 동기화: thread 단독 소유 또는 spinlock 보호. 디버그 빌드에서만 의미 있는 정보. */

	bool initialized;
	/* [한국어] init 호출됐는지 여부. destroy 후 재사용 또는 init 누락 검출.
	 * 설정자: spdk_spin_init()=true, spdk_spin_destroy()=false.
	 * 읽는 자: lock/unlock 진입부의 assert. */

	bool destroyed;
	/* [한국어] destroy 호출됐는지 여부. destroyed lock에 lock 시도하면 abort.
	 * 설정자: spdk_spin_destroy()=true.
	 * 읽는 자: lock/unlock 진입부의 assert. use-after-destroy 검출용. */
};

/**
 * Initialize an spdk_spinlock.
 *
 * \param sspin The SPDK spinlock to initialize.
 */
/*
 * [한국어]
 * spdk_spin_init - spdk_spinlock 초기화.
 *
 * @sspin: 초기화할 spinlock 메모리 (호출자가 소유).
 *
 * 동작: pthread_spin_init(PTHREAD_PROCESS_PRIVATE) + thread=NULL + initialized=true.
 * 사용 패턴: bdev/nvme/nvmf의 글로벌 등록 트리, ctrlr 리스트 등 cross-thread 공유 자료구조 보호용.
 * 호출 컨텍스트: 임의 thread (보통 모듈 init 단계).
 */
void spdk_spin_init(struct spdk_spinlock *sspin);

/**
 * Destroy an spdk_spinlock.
 *
 * \param sspin The SPDK spinlock to initialize.
 */
/*
 * [한국어]
 * spdk_spin_destroy - spinlock 파괴.
 *
 * @sspin: 파괴할 spinlock.
 *
 * 사전 조건: 락이 unlocked 상태여야 함 (held 상태 destroy는 abort).
 * 호출 컨텍스트: 임의 thread.
 */
void spdk_spin_destroy(struct spdk_spinlock *sspin);

/**
 * Lock an SPDK spin lock.
 *
 * \param sspin An SPDK spinlock.
 */
/*
 * [한국어]
 * spdk_spin_lock - spinlock 획득. 잘못된 사용은 abort.
 *
 * @sspin: 잠글 spinlock.
 *
 * 검증 사항: 1) 같은 spdk_thread가 재귀적으로 lock 시도 → abort (데드락 예방).
 *           2) 인자 spinlock이 init 안 됨/destroyed → abort.
 * 동작: pthread_spin_lock(busy-wait spin) → 획득 후 sspin->thread = 현재 spdk_thread.
 * 임계 구간 규약: lock 잡은 채 poller/event에서 return하지 말 것 (마이그레이션으로 unlock thread 달라질 위험).
 *               임계 구간은 짧고 동기적이어야 함.
 * 호출 컨텍스트: 반드시 spdk_thread 컨텍스트 (spdk_get_thread() != NULL).
 */
void spdk_spin_lock(struct spdk_spinlock *sspin);

/**
 * Unlock an SPDK spinlock.
 *
 * \param sspin An SPDK spinlock.
 */
/*
 * [한국어]
 * spdk_spin_unlock - spinlock 해제. 잘못된 thread/상태에서 호출 시 abort.
 *
 * @sspin: 해제할 spinlock.
 *
 * 검증: lock 보유 thread != 현재 spdk_thread → abort. unlocked 상태에서 unlock → abort.
 * 동작: sspin->thread=NULL → pthread_spin_unlock.
 */
void spdk_spin_unlock(struct spdk_spinlock *sspin);

/**
 * Determine if the caller holds this SPDK spinlock.
 *
 * \param sspin An SPDK spinlock.
 * \return true if spinlock is held by this thread, else false
 */
/*
 * [한국어]
 * spdk_spin_held - 현재 spdk_thread가 sspin을 보유 중인지 검사.
 *
 * @sspin:  검사할 spinlock.
 * @return: true면 현재 thread가 보유, false면 미보유 (다른 thread가 보유 또는 unlocked).
 *
 * 활용: assert(spdk_spin_held(&lock)) 패턴으로 "이 함수는 lock 보유 상태에서만 호출됨" 강제.
 */
bool spdk_spin_held(struct spdk_spinlock *sspin);

/*
 * [한국어]
 * struct spdk_iobuf_opts - iobuf 풀 초기화 옵션 (small/large 두 등급의 버퍼 풀 크기 설정).
 *
 * iobuf는 SPDK의 통합 데이터 버퍼 풀이다. NVMe-oF/bdev/sock 등이 I/O payload 버퍼를 자체 mempool로
 * 만들지 않고 본 시설로 일원화. small과 large 두 사이즈를 두는 이유: 작은 메타 I/O와 큰 read/write를
 * 동일 풀에서 처리하면 fragmentation 발생, 두 풀로 나누면 burst 시에도 충분한 풀 가용성 보장.
 *
 * ABI 안전성: opts_size 필드로 caller가 인지한 구조체 크기 전달, 라이브러리는 그 이후 필드를 기본값으로
 *            채움 → 향후 필드 추가 시 caller 재컴파일 없이 호환.
 */
struct spdk_iobuf_opts {
	/** Maximum number of small buffers */
	uint64_t small_pool_count;
	/* [한국어] small 버퍼 풀의 최대 객체 수.
	 * 설정자: 사용자가 spdk_iobuf_set_opts() 또는 RPC iobuf_set_options로 지정.
	 * 읽는 자: spdk_iobuf_initialize()가 rte_mempool 생성 시 size 인자로 사용.
	 * 값 범위: 양의 정수 (보통 8192~65536). 0이면 라이브러리 기본값.
	 * 동기화: init 시점 1회 설정, 이후 read-only. */
	/** Maximum number of large buffers */
	uint64_t large_pool_count;
	/* [한국어] large 버퍼 풀의 최대 객체 수.
	 * 설정자: 사용자.
	 * 읽는 자: initialize().
	 * 값 범위: 양의 정수. 보통 small_pool_count/8 정도가 합리적 (대형 I/O 빈도 낮음).
	 * 동기화: init 1회. */
	/** Size of a single small buffer */
	uint32_t small_bufsize;
	/* [한국어] small 버퍼 1개의 바이트 크기.
	 * 일반적 값: 8192 (8KB) 또는 NVMe 기본 4096(4KB).
	 * 사용 패턴: spdk_iobuf_get(ch, len, ...)가 len <= small_bufsize면 small 풀, 초과면 large 풀.
	 * 값 범위: hugepage 정렬 권장 (2MB의 약수). */
	/** Size of a single large buffer */
	uint32_t large_bufsize;
	/* [한국어] large 버퍼 1개의 바이트 크기.
	 * 일반적 값: 131072 (128KB) — NVMe MDTS 기본 상한과 매치.
	 * 사용 패턴: bdev_io 큰 write/read의 임시 staging buffer.
	 * 제약: spdk_iobuf_get(len > large_bufsize)는 정의되지 않음 (호출자 책임으로 size 제한). */

	/**
	 * The size of spdk_iobuf_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
	/* [한국어] caller가 알고 있는 구조체 크기 (sizeof(struct spdk_iobuf_opts) at compile time).
	 * 설정자: 사용자가 set_opts 호출 시 sizeof로 채움 (관용적으로 SPDK_INIT_OPTS_VERIFY 매크로 사용).
	 * 읽는 자: 라이브러리가 "이 크기 안의 필드까지만 유효, 이후는 기본값"으로 해석.
	 * 의미: caller가 새로운 라이브러리 binary와 옛 binary 어느 쪽과도 ABI 호환. */

	/** Enable per-NUMA node buffer pools */
	uint8_t	enable_numa;
	/* [한국어] NUMA 노드별로 별도 풀을 만들지 여부 (bool 1B).
	 * 설정자: 사용자 (멀티-NUMA 시스템 + RDMA NIC affinity 보존이 중요한 경우 1).
	 * 읽는 자: initialize()가 enable_numa=1이면 SPDK_CONFIG_MAX_NUMA_NODES 만큼 풀을 만들고, 각 풀이
	 *          해당 노드의 hugepage에서 alloc → DMA 할 때 NUMA-local 메모리 보장.
	 * 값 범위: 0/1. 0이면 단일 글로벌 풀 (기본). */
};

/*
 * [한국어]
 * struct spdk_iobuf_pool_stats - iobuf 풀 한 등급(small 또는 large)의 사용 통계.
 *
 * 각 카운터는 풀에서 buf get을 시도한 결과 분류. 모니터링/RPC iobuf_get_stats에서 노출.
 */
struct spdk_iobuf_pool_stats {
	/** Buffer got from local per-thread cache */
	uint64_t	cache;
	/* [한국어] thread-local STAILQ 캐시에서 hit한 횟수 (가장 빠른 path, 락·atomic 없음).
	 * 설정자: spdk_iobuf_get()이 cache pop 성공 시 ++.
	 * 읽는 자: get_stats() RPC.
	 * 의미: 이 값이 cache+main+retry 중 압도적으로 많아야 hot path가 효율적. */
	/** Buffer got from the main shared pool */
	uint64_t	main;
	/* [한국어] cache miss 후 글로벌 spdk_ring(rte_ring)에서 batch dequeue 성공 횟수.
	 * 설정자: spdk_iobuf_get()의 ring miss 후 batch refill 경로.
	 * 읽는 자: 모니터링.
	 * 의미: cache 대비 비율이 높으면 cache_size를 키우는 게 좋다는 신호. */
	/** Buffer missed and request to get buffer was queued */
	uint64_t	retry;
	/* [한국어] cache miss + 글로벌 풀도 비어 wait queue에 entry를 등록한 횟수.
	 * 설정자: spdk_iobuf_get()이 ring 고갈 시 ++.
	 * 의미: 이 값이 늘면 small/large_pool_count가 부족하다는 강력한 신호 (튜닝 필요).
	 * 동기화: 풀별 atomic 또는 thread-local 누적 후 합산. */
};

/*
 * [한국어]
 * struct spdk_iobuf_module_stats - 한 모듈(예: bdev_nvmf, sock)의 iobuf 사용 통계.
 *
 * RPC iobuf_get_stats가 모든 등록 모듈에 대해 본 구조체 배열을 반환.
 */
struct spdk_iobuf_module_stats {
	struct spdk_iobuf_pool_stats	small_pool;
	/* [한국어] 이 모듈의 small 풀 사용 통계. 작은 I/O 패턴 분석용. */
	struct spdk_iobuf_pool_stats	large_pool;
	/* [한국어] 이 모듈의 large 풀 사용 통계. payload 큰 워크로드일수록 카운트 증가. */
	const char			*module;
	/* [한국어] 모듈 이름 문자열 포인터 (spdk_iobuf_register_module()에 등록한 그 값).
	 * 동기화: read-only after register. */
};

struct spdk_iobuf_entry;
/* [한국어] iobuf wait queue entry의 전방 선언. 풀이 비었을 때 caller가 본 구조체를 wait queue에 등록하고,
 * 다른 caller가 spdk_iobuf_put() 시 버퍼가 본 entry로 전달되어 cb_fn 호출 → 비동기 backpressure 처리. */

typedef void (*spdk_iobuf_get_cb)(struct spdk_iobuf_entry *entry, void *buf);
/* [한국어] iobuf wait queue가 깨어났을 때 호출되는 콜백 타입.
 *
 * @entry: 호출자가 처음 spdk_iobuf_get(... entry, cb_fn)에 넘겼던 그 entry. container_of로 caller가
 *         자기 큰 컨텍스트 구조체로 복원.
 * @buf:   드디어 확보된 버퍼 포인터.
 *
 * 실행 컨텍스트: 버퍼를 release(spdk_iobuf_put)한 thread 위 (entry를 enqueue했던 thread와 일반적으로 동일,
 *               iobuf는 thread 단독 wait queue를 가짐). */

/** iobuf queue entry */
/*
 * [한국어]
 * struct spdk_iobuf_entry - wait queue 노드. caller가 자기 컨텍스트에 임베드해 사용.
 *
 * 사용 패턴: caller가 자기 구조체 안에 spdk_iobuf_entry를 멤버로 두고, get() 실패 시 wait queue에
 *           등록 → 버퍼 가용 시 cb_fn(&entry, buf)가 호출되어 caller가 entry로부터 자기 구조체 복원.
 */
struct spdk_iobuf_entry {
	spdk_iobuf_get_cb		cb_fn;
	/* [한국어] wait queue가 깨워질 때 호출될 콜백.
	 * 설정자: caller가 get() 호출 직전에 채움.
	 * 읽는 자: spdk_iobuf_put()이 wait queue에서 entry를 pop할 때 cb_fn(entry, buf) 호출.
	 * 동기화: entry를 큐에 넣은 후 unregister 전까지 read-only. */

	const void			*module;
	/* [한국어] 이 entry를 등록한 모듈 식별자 (spdk_iobuf_register_module의 반환과 동일).
	 * 설정자: spdk_iobuf_get() 내부에서 channel.module로 채움.
	 * 읽는 자: 통계 집계 또는 wait queue 정렬 시 모듈별 분리.
	 * 동기화: entry 수명 동안 read-only. */

	STAILQ_ENTRY(spdk_iobuf_entry)	stailq;
	/* [한국어] BSD STAILQ 노드 (sys/queue.h). pool_cache->queue 리스트에 연결될 때 사용.
	 * 설정자/읽는 자: STAILQ_INSERT_TAIL/STAILQ_REMOVE_HEAD가 push/pop 시 갱신.
	 * 동기화: 같은 thread의 wait queue만 다루므로 락 불필요. */
};

/*
 * [한국어]
 * struct spdk_iobuf_buffer - 풀에 들어가는 버퍼 자신의 헤더.
 *
 * 실제 페이로드 영역은 본 구조체 뒤에 인접 배치 (alloc 시 통합 chunk). pool에 free 상태로 머무는 동안
 * 본 구조체의 STAILQ 노드를 통해 cache 리스트에 묶여 있다.
 */
struct spdk_iobuf_buffer {
	STAILQ_ENTRY(spdk_iobuf_buffer)	stailq;
	/* [한국어] cache STAILQ 노드. 사용 중에는 의미 없고 free 상태에서만 cache 리스트 멤버로 작동.
	 * 풀 사용자에게 반환되는 포인터는 본 구조체 직후 주소(=payload). put 시점에 다시 본 헤더로 캐스팅. */
};

typedef STAILQ_HEAD(, spdk_iobuf_entry) spdk_iobuf_entry_stailq_t;
/* [한국어] entry 리스트 헤드 타입. wait queue용. */
typedef STAILQ_HEAD(, spdk_iobuf_buffer) spdk_iobuf_buffer_stailq_t;
/* [한국어] buffer 리스트 헤드 타입. cache용. */

/*
 * [한국어]
 * struct spdk_iobuf_pool_cache - 한 등급(small 또는 large) 풀의 thread-local 캐시 + 글로벌 풀 + wait queue.
 *
 * 핵심 설계: thread-local STAILQ에 cache_size 만큼의 free 버퍼를 미리 캐시 → get()/put() hot path가
 *            락·atomic 없이 동작. 캐시 miss는 글로벌 spdk_ring(MPMC lockless)에서 batch refill.
 */
struct spdk_iobuf_pool_cache {
	/** Buffer pool */
	struct spdk_ring		*pool;
	/* [한국어] 글로벌 lockless ring (DPDK rte_ring 래퍼). 여기서 batch dequeue로 캐시 채움.
	 * 설정자: spdk_iobuf_initialize()가 모든 thread 공유 단일 ring 생성.
	 * 읽는 자: cache miss 시 spdk_ring_dequeue_bulk(IOBUF_BATCH_SIZE).
	 * 동기화: ring 자체가 lockless (rte_ring SPMC/MPMC). */
	/** Buffer cache */
	spdk_iobuf_buffer_stailq_t	cache;
	/* [한국어] thread-local free 버퍼 STAILQ. get()이 STAILQ_FIRST로 pop, put()이 INSERT_HEAD로 push.
	 * 설정자: get/put + ring batch refill/flush.
	 * 동기화: thread 단독 소유 → 락 없이 안전. iobuf API 진입부에서 spdk_get_thread() 일치 assert. */
	/** Number of elements in the cache */
	uint32_t			cache_count;
	/* [한국어] 현재 cache STAILQ에 들어있는 객체 수.
	 * 설정자: get/put이 ++/-- 또는 batch refill/flush 시 ±IOBUF_BATCH_SIZE.
	 * 읽는 자: put()이 cache_count > cache_size + IOBUF_BATCH_SIZE 면 batch flush 결정. */
	/** Size of the cache */
	uint32_t			cache_size;
	/* [한국어] 캐시 정상 운영 크기 (목표 수준).
	 * 설정자: spdk_iobuf_channel_init(small_cache_size, large_cache_size)로 결정.
	 * 의미: put()이 cache_count <= cache_size이면 항상 cache로 push (글로벌 ring 안 거침).
	 *      cache_count가 cache_size + IOBUF_BATCH_SIZE 초과하면 batch flush 발생 (메모리 리밸런스). */
	/** Buffer wait queue */
	spdk_iobuf_entry_stailq_t	*queue;
	/* [한국어] 풀 고갈 시 caller entry가 머무는 wait queue 포인터 (모듈별로 공유).
	 * 설정자: spdk_iobuf_register_module()이 모듈마다 1개 wait queue 할당.
	 * 읽는 자: get/put이 풀 비/풀 직전 상태에서 enqueue/dequeue.
	 * 동기화: same-thread 보장으로 락 없음. */
	/** Buffer size */
	uint32_t			bufsize;
	/* [한국어] 이 풀이 다루는 1개 버퍼 크기 (small_bufsize 또는 large_bufsize 사본).
	 * 설정자: channel_init.
	 * 읽는 자: get()이 요청 len과 비교해 small/large 풀 선택. */
	/** Pool usage statistics */
	struct spdk_iobuf_pool_stats	stats;
	/* [한국어] cache/main/retry 카운터 (위 spdk_iobuf_pool_stats 참고). */
};

/*
 * [한국어]
 * struct spdk_iobuf_node_cache - 한 NUMA 노드의 small + large 두 등급 캐시 묶음.
 *
 * NUMA 활성 시 spdk_iobuf_channel.cache[] 배열에 노드별로 1개씩 들어감. NUMA 비활성이면 [0]만 사용.
 */
struct spdk_iobuf_node_cache {
	/** Small buffer memory pool cache */
	struct spdk_iobuf_pool_cache	small;
	/* [한국어] small 등급 풀 캐시. 설정/읽는 자: spdk_iobuf_get/put이 len 따라 선택. */
	/** Large buffer memory pool cache */
	struct spdk_iobuf_pool_cache	large;
	/* [한국어] large 등급 풀 캐시. */
};

#ifndef SPDK_CONFIG_MAX_NUMA_NODES
/* Set this default temporarily, for users that may pull latest code without
 * re-running configure.
 */
/* [한국어] SPDK_CONFIG_MAX_NUMA_NODES 미정의 빌드 환경(즉 ./configure 안 다시 돌린 호환 케이스)에서
 * 임시 기본값 1을 설정. 정상 빌드 흐름에서는 spdk/config.h가 이 매크로를 시스템 NUMA 토폴로지 기반으로
 * 정의 (예: 2-socket Xeon이면 2). */
#define SPDK_CONFIG_MAX_NUMA_NODES 1
#endif

/** iobuf channel */
/*
 * [한국어]
 * struct spdk_iobuf_channel - 한 spdk_thread × 한 모듈의 iobuf 인터페이스.
 *
 * 사용 패턴: 모듈이 spdk_get_io_channel(my_io_device)로 io_channel 얻은 직후, 그 안의 ctx에서
 *            spdk_iobuf_channel_init(&iobuf_ch, "module_name", small_cache, large_cache) 호출.
 *            이후 spdk_iobuf_get/put을 본 ch에 호출.
 */
struct spdk_iobuf_channel {
	/** Module pointer */
	const void			*module;
	/* [한국어] 이 채널을 소유한 모듈의 식별자 (spdk_iobuf_register_module 반환).
	 * 설정자: spdk_iobuf_channel_init().
	 * 읽는 자: get()이 entry->module 채울 때, get_stats()가 모듈 분류 시.
	 * 동기화: 채널 수명 동안 read-only. */

	/** Parent IO channel */
	struct spdk_io_channel		*parent;
	/* [한국어] 이 iobuf 채널이 결합된 spdk_io_channel (모듈의 일반 io_channel).
	 * 설정자: channel_init이 spdk_get_io_channel(iobuf_io_device) 결과 저장.
	 * 읽는 자: iobuf API 진입부 assert에서 spdk_io_channel_get_thread(parent)==spdk_get_thread() 검증
	 *          → "이 함수는 채널 소유 thread에서만 호출됨" 강제. */

	/* Buffer cache */
	struct spdk_iobuf_node_cache	cache[SPDK_CONFIG_MAX_NUMA_NODES];
	/* [한국어] NUMA 노드별 small/large 캐시 배열. enable_numa=0이면 [0] 한 슬롯만 사용.
	 * 설정자: channel_init이 활성 NUMA 노드 수만큼 초기화 + ring batch refill로 채움.
	 * 읽는 자: get/put이 IOBUF_FOREACH_NUMA_ID 매크로로 노드 순회 (현재 thread NUMA 우선).
	 * 동기화: thread 단독 소유 → 락 없음. NUMA 분산은 hugepage NUMA-local 보장과 cross-socket
	 *         캐시라인 ping-pong 방지를 동시에 달성. */
};

/**
 * Initialize and allocate iobuf pools.
 *
 * \return 0 on success, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_iobuf_initialize - small/large 글로벌 버퍼 풀 할당 + 인프라 초기화.
 *
 * @return: 0 성공, 음수 errno (-ENOMEM=hugepage 부족 등).
 *
 * 동작: spdk_iobuf_set_opts()로 받은 설정값 또는 기본값으로 두 spdk_ring 풀 생성, 각 풀에
 *      pool_count개의 spdk_iobuf_buffer + payload chunk를 hugepage에서 alloc.
 *      enable_numa 시 노드별 풀 분리.
 * 호출 시점: spdk_subsystem_init 단계 (subsystem 의존성에서 가장 먼저 init되는 모듈 중 하나).
 */
int spdk_iobuf_initialize(void);

typedef void (*spdk_iobuf_finish_cb)(void *cb_arg);
/* [한국어] iobuf 정리 완료 콜백 타입. spdk_iobuf_finish가 모든 풀 정리 후 호출. */

/**
 * Clean up and free iobuf pools.
 *
 * \param cb_fn Callback to be executed once the clean up is completed.
 * \param cb_arg Callback argument.
 */
/*
 * [한국어]
 * spdk_iobuf_finish - iobuf 모든 자원 해제 (앱 shutdown 단계).
 *
 * @cb_fn:  정리 완료 후 호출될 콜백 (NULL 가능).
 * @cb_arg: cb_fn 컨텍스트.
 *
 * 비동기: 모든 모듈 unregister 후 두 풀 free, 마지막에 cb_fn 호출. cb_fn은 호출자 thread (보통 app thread).
 */
void spdk_iobuf_finish(spdk_iobuf_finish_cb cb_fn, void *cb_arg);

/**
 * Set iobuf options.  These options will be used during `spdk_iobuf_initialize()`.
 *
 * \param opts Options describing the size of the pools to reserve.
 *
 * \return 0 on success, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_iobuf_set_opts - 풀 옵션 설정 (initialize 이전에 호출).
 *
 * @opts:   사용자가 채운 spdk_iobuf_opts (opts_size로 ABI 안전).
 * @return: 0 성공, 음수 errno (-EINVAL=값 검증 실패, -EBUSY=이미 init된 후 호출).
 *
 * 호출 시점: spdk_subsystem_init 직전 또는 RPC iobuf_set_options 처리 단계.
 *           이미 initialize된 후에는 변경 불가 (재시작 필요).
 */
int spdk_iobuf_set_opts(const struct spdk_iobuf_opts *opts);

/**
 * Get iobuf options.
 *
 * \param opts Output parameter for options.
 * \param opts_size sizeof(*opts)
 */
/*
 * [한국어]
 * spdk_iobuf_get_opts - 현재 적용된 풀 옵션 조회.
 *
 * @opts:      caller 제공 출력 구조체.
 * @opts_size: caller가 인지한 sizeof(*opts) (ABI 안전).
 */
void spdk_iobuf_get_opts(struct spdk_iobuf_opts *opts, size_t opts_size);

/**
 * Register a module as an iobuf pool user.  Only registered users can request buffers from the
 * iobuf pool.
 *
 * \name Name of the module.
 *
 * \return 0 on success, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_iobuf_register_module - 모듈 이름을 iobuf 사용자로 등록 + wait queue 1쌍 할당.
 *
 * @name:   모듈 이름 (전역 unique). 통계/디버깅에 사용.
 * @return: 0 성공, 음수 errno (-EEXIST=중복 등록).
 *
 * 의무: spdk_iobuf_channel_init(... name)을 호출하기 전에 반드시 본 함수로 등록 필수. 미등록 모듈이
 *       채널을 init하면 -ENOENT 반환.
 * 호출 시점: bdev_module_init 같은 모듈별 init 단계.
 */
int spdk_iobuf_register_module(const char *name);

/**
 * Unregister an iobuf pool user from a module.
 *
 * \name Name of the module.
 *
 * \return 0 on success, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_iobuf_unregister_module - 모듈 등록 해제 + wait queue 정리.
 *
 * @name:   register와 동일한 모듈 이름.
 * @return: 0 성공, 음수 errno.
 *
 * 사전 의무: 해당 모듈의 모든 spdk_iobuf_channel을 fini한 후에만 호출.
 */
int spdk_iobuf_unregister_module(const char *name);

/**
 * Initialize an iobuf channel.
 *
 * \param ch iobuf channel to initialize.
 * \param name Name of the module registered via `spdk_iobuf_register_module()`.
 * \param small_cache_size Number of small buffers to be cached by this channel.
 * \param large_cache_size Number of large buffers to be cached by this channel.
 *
 * \return 0 on success, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_iobuf_channel_init - 모듈의 io_channel.create_cb 안에서 호출하는 iobuf 부속 채널 초기화.
 *
 * @ch:                초기화할 spdk_iobuf_channel (caller 메모리, 보통 channel ctx 안에 임베드).
 * @name:              register_module()로 등록한 모듈 이름.
 * @small_cache_size:  small 버퍼를 thread-local에 미리 캐시할 개수.
 * @large_cache_size:  large 버퍼 캐시 개수.
 * @return:            0 성공, 음수 errno.
 *
 * ★ 동작: 1) 글로벌 풀에서 batch dequeue로 small_cache_size, large_cache_size만큼 버퍼를 thread-local
 *           STAILQ에 채움. 2) parent io_channel을 spdk_get_io_channel(iobuf_io_dev)로 획득해 캐시.
 *
 * 호출 컨텍스트: io_channel.create_cb 내부 (즉 채널을 사용할 spdk_thread 위).
 * 캐시 크기 가이드: 작으면 매번 글로벌 ring 접근 → contention. 너무 크면 메모리 낭비. 일반적으로
 *                  thread당 64~256 정도가 합리적.
 */
int spdk_iobuf_channel_init(struct spdk_iobuf_channel *ch, const char *name,
			    uint32_t small_cache_size, uint32_t large_cache_size);

/**
 * Release resources tied to an iobuf channel.
 *
 * \param ch iobuf channel.
 */
/*
 * [한국어]
 * spdk_iobuf_channel_fini - iobuf 채널 해제 (모듈의 io_channel.destroy_cb에서 호출).
 *
 * @ch: 정리할 채널.
 *
 * 동작: thread-local 캐시의 모든 버퍼를 글로벌 ring으로 flush, parent io_channel을 put → ch 사용 종료.
 *      wait queue에 남아있는 entry는 caller 책임으로 spdk_iobuf_entry_abort로 미리 취소해야 함.
 */
void spdk_iobuf_channel_fini(struct spdk_iobuf_channel *ch);

typedef int (*spdk_iobuf_for_each_entry_fn)(struct spdk_iobuf_channel *ch,
		struct spdk_iobuf_entry *entry, void *ctx);
/* [한국어] for_each_entry 콜백 타입. wait queue 순회 중 각 entry에 대해 호출.
 * @return: 0=계속, 음수=즉시 중단 후 그 status 반환. */

/**
 * Iterate over all entries on a given channel and execute a callback on those that were requested.
 * The iteration is stopped if the callback returns non-zero status.
 *
 * \param ch iobuf channel to iterate over.
 * \param cb_fn Callback to execute on each entry on the channel that was requested.
 * \param cb_ctx Argument passed to `cb_fn`.
 *
 * \return status of the last callback.
 */
/*
 * [한국어]
 * spdk_iobuf_for_each_entry - 채널의 wait queue 순회.
 *
 * @ch:     순회할 iobuf 채널.
 * @cb_fn:  각 entry에 호출될 콜백.
 * @cb_ctx: cb_fn에 전달될 컨텍스트.
 * @return: 마지막 cb_fn 반환값.
 *
 * 활용: shutdown 시 미완료 wait entry 검출, 디버깅용 dump.
 * 동시성: 채널 단독 thread 보장 → 락 없음.
 */
int spdk_iobuf_for_each_entry(struct spdk_iobuf_channel *ch,
			      spdk_iobuf_for_each_entry_fn cb_fn, void *cb_ctx);

/**
 * Abort an outstanding request waiting for a buffer.
 *
 * \param ch iobuf channel on which the entry is waiting.
 * \param entry Entry to remove from the wait queue.
 * \param len Length of the requested buffer (must be the exact same value as specified in
 *            `spdk_iobuf_get()`.
 */
/*
 * [한국어]
 * spdk_iobuf_entry_abort - wait queue에서 entry 제거 (cb_fn 호출 안 함).
 *
 * @ch:    entry가 등록된 채널.
 * @entry: 제거할 entry.
 * @len:   원래 spdk_iobuf_get()에 넘긴 같은 값. 어느 풀(small/large) 큐인지 결정에 사용.
 *
 * 사용 시나리오: I/O가 timeout, 사용자 cancel 등으로 더 이상 버퍼가 필요 없을 때. cb_fn은 호출되지
 *               않으므로 caller가 자기 컨텍스트를 직접 정리.
 * 호출 컨텍스트: 채널 소유 thread 위에서만.
 */
void spdk_iobuf_entry_abort(struct spdk_iobuf_channel *ch, struct spdk_iobuf_entry *entry,
			    uint64_t len);

/**
 * Get a buffer from the iobuf pool. If no buffers are available and entry with cb_fn provided
 * then the request is queued until a buffer becomes available.
 *
 * \param ch iobuf channel.
 * \param len Length of the buffer to retrieve. The user is responsible for making sure the length
 *            doesn't exceed large_bufsize.
 * \param entry Wait queue entry (optional).
 * \param cb_fn Callback to be executed once a buffer becomes available. If a buffer is available
 *              immediately, it is NOT executed. Mandatory only if entry provided.
 *
 * \return pointer to a buffer or NULL if no buffers are currently available.
 */
/*
 * [한국어]
 * spdk_iobuf_get - 풀에서 버퍼 1개 획득 (★ I/O hot path).
 *
 * @ch:     iobuf 채널.
 * @len:    요청 크기 (≤ small_bufsize면 small 풀, 초과면 large 풀에서 할당).
 * @entry:  실패 시 wait queue 등록할 entry (옵션, NULL이면 큐잉 안 함).
 * @cb_fn:  버퍼 가용 시 호출될 콜백 (entry와 짝). entry 제공 시 필수.
 * @return: 즉시 사용 가능한 버퍼 포인터, NULL=풀 비고 (entry==NULL이거나 큐잉 후) 즉시 결과 없음.
 *
 * ★ 3-tier 동작:
 *   1) thread-local cache STAILQ 검사 → 있으면 pop 즉시 반환 (가장 빠른 path, 락 없음). stats.cache++.
 *   2) cache 비면 글로벌 ring에서 IOBUF_BATCH_SIZE 단위 batch dequeue → cache 채우고 1개 반환. stats.main++.
 *   3) ring도 비면 entry를 wait queue에 enqueue 후 NULL 반환. stats.retry++.
 *      → 다른 caller의 spdk_iobuf_put() 시 본 entry로 버퍼 전달되며 cb_fn 호출.
 *
 * 즉시 가용 vs 비동기: 1)/2)는 즉시 반환된 버퍼, 3)은 NULL→cb_fn 비동기. caller는 NULL 반환 받으면
 *                    자기 자신의 작업을 cb_fn으로 미루는 식으로 backpressure 처리.
 *
 * 호출 컨텍스트: 채널 소유 thread 위에서만.
 * 사용 예: bdev_io payload 버퍼 확보, sock egress staging 버퍼 확보.
 */
void *spdk_iobuf_get(struct spdk_iobuf_channel *ch, uint64_t len, struct spdk_iobuf_entry *entry,
		     spdk_iobuf_get_cb cb_fn);

/**
 * Release a buffer back to the iobuf pool.  If there are outstanding requests waiting for a buffer,
 * this buffer will be passed to one of them.
 *
 * \param ch iobuf channel.
 * \param buf Buffer to release
 * \param len Length of the buffer (must be the exact same value as specified in `spdk_iobuf_get()`).
 */
/*
 * [한국어]
 * spdk_iobuf_put - 사용 끝난 버퍼 반납. wait queue에 대기자 있으면 그쪽으로 직접 전달.
 *
 * @ch:  iobuf 채널.
 * @buf: spdk_iobuf_get()로 받은 버퍼.
 * @len: get 시 넘긴 동일한 length (small/large 풀 분류용 - 잘못 넘기면 풀 손상).
 *
 * 동작 우선순위:
 *   1) wait queue에 대기 entry 있으면 → entry pop → cb_fn(entry, buf) 호출 (글로벌 ring 우회, 가장 빠른 backpressure 해소).
 *   2) wait queue 비고 cache 여유 있으면 → cache STAILQ에 push (cache_count++).
 *   3) cache_count > cache_size + IOBUF_BATCH_SIZE면 → IOBUF_BATCH_SIZE 만큼 cache → 글로벌 ring batch enqueue로 flush.
 * 호출 컨텍스트: 채널 소유 thread 위에서만.
 */
void spdk_iobuf_put(struct spdk_iobuf_channel *ch, void *buf, uint64_t len);

typedef void (*spdk_iobuf_get_stats_cb)(struct spdk_iobuf_module_stats *modules,
					uint32_t num_modules, void *cb_arg);
/* [한국어] iobuf 통계 수집 완료 콜백. modules 배열은 num_modules 개.
 * 콜백 반환 후 modules 배열은 라이브러리가 free (caller가 보관하려면 deepcopy 필수). */

/**
 * Get iobuf statistics.
 *
 * \param cb_fn Callback to be executed once stats are gathered.
 * \param cb_arg Argument passed to the callback function.
 *
 * \return 0 on success, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_iobuf_get_stats - 모든 모듈에 대한 iobuf 사용 통계 비동기 수집.
 *
 * @cb_fn:  통계 집계 후 호출될 콜백.
 * @cb_arg: cb_fn 컨텍스트.
 * @return: 0 즉시 성공 (디스패치 OK), 음수 errno (-ENOMEM 등).
 *
 * 비동기 이유: 각 thread의 채널 stats를 spdk_for_each_thread로 모아야 하므로 비동기. cb_fn은
 *             originating thread (caller)에서 실행됨.
 * 호출처: RPC iobuf_get_stats가 본 함수로 결과 수집 후 JSON으로 응답.
 */
int spdk_iobuf_get_stats(spdk_iobuf_get_stats_cb cb_fn, void *cb_arg);

typedef void (*spdk_post_poller_fn)(void *fn_arg);
/* [한국어] post-poller handler 콜백 타입. 한 번 호출되면 자동 deregister (one-shot). */

/**
 * Register a function to be called after the current SPDK poller has completed. Once called,
 * this function is de-registered and won't called until the next registration call.
 *
 * \param fn Function to call
 * \param fn_arg Function argument
 */
/*
 * [한국어]
 * spdk_thread_register_post_poller_handler - 현재 poller 콜백 종료 직후 1회 실행될 후처리 핸들러 등록.
 *
 * @fn:     실행될 함수 (one-shot).
 * @fn_arg: 컨텍스트.
 *
 * 사용 시나리오: poller 콜백 안에서 lazy하게 정리 작업을 발견했지만, 현재 poller 컨텍스트에서 실행하면
 *               재진입 위험이 있을 때. 본 함수로 등록하면 lib/thread가 poller 호출 종료 직후 (같은
 *               spdk_thread_poll 라운드 내, 다음 poller 호출 전) fn(fn_arg) 호출.
 * 한 번만 등록 가능: 이미 등록된 핸들러가 있으면 덮어씀 (이전 핸들러 silently lost).
 *                 호출되면 자동 deregister → 다음 라운드부터는 호출 안 됨.
 * 호출 컨텍스트: 현재 poller 콜백 안에서 호출. 등록한 thread에서만 실행됨.
 */
void spdk_thread_register_post_poller_handler(spdk_post_poller_fn fn, void *fn_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_THREAD_H_ */
