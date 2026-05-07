/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK Thread/Poller/Message/IO Channel/iobuf/Spinlock/Interrupt 구현 본체 (thread.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 의 가장 핵심적인 실행 모델인 "spdk_thread" 의 구현 본체이다. include/spdk/thread.h
 * 에 선언된 거의 모든 공개 API 가 여기서 구현되며, 6 개의 거대 영역을 책임진다 — (1) spdk_thread
 * 객체의 lifecycle (create/start/exit/destroy) 과 메타데이터 관리, (2) cross-thread 메시지
 * 전달(spdk_thread_send_msg) 을 위한 lockless rte_ring 기반 SPSC/MPSC 큐, (3) poller(보통/
 * timed/paused) 등록·실행·일시정지·해제와 RB tree 기반 timer wheel, (4) per-device, per-thread
 * I/O channel 캐싱(io_channel_tree) 과 io_device 등록·해제·refcnt 관리, (5) iobuf 는 별도
 * 파일(iobuf.c) 로 분리되어 있으나 spdk_thread 자체는 iobuf 채널을 노출, (6) spinlock 디버그
 * 래퍼(spdk_spinlock) 와 fd_group/epoll 기반 interrupt 모드 통합. 이 파일이 없으면 SPDK 의 모든
 * 비동기 모델 — bdev I/O submit/complete, NVMe qpair polling, RPC dispatch, scheduler 등이
 * 동작하지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 실행 스택은 다음과 같이 계층화된다:
 *   [DPDK EAL 초기화] → [spdk_thread_lib_init] → [코어별 reactor pthread 생성]
 *     → [reactor 가 1 spdk_thread 을 hosting 하며 spdk_thread_poll() 무한 루프]
 *       → [poller 실행 → 메시지 ring drain → timed poller 실행 → post_poller_handler]
 *         → [bdev/NVMe/sock 같은 상위 모듈이 위 컨텍스트에서 동작]
 * 즉 thread.c 는 "reactor 와 어플리케이션 모듈" 사이의 프레임이다. lib/event/reactor.c 가
 * spdk_thread_poll() 을 호출하고, bdev/NVMe 등은 spdk_thread_send_msg() 와 spdk_get_io_channel()
 * 를 호출하여 thread.c 에 의존한다. include/spdk/thread.h 의 모든 함수가 여기서 1:1 로 매핑되며,
 * 헤더 주석에서 설명한 lockless 설계, period_us=0 즉시 폴링, RB tree timed poller, io_channel
 * per-thread 캐시 + ref count, fd_group epoll interrupt 모드 등이 본 파일에서 실제로 구현된다.
 * 실행 컨텍스트는 호스트 유저스페이스 — DPDK pthread 위에서 동작하지만 OS 스레드 스케줄러로의
 * yield 는 회피한다 (busy poll).
 *
 * === 타 모듈과의 연결 ===
 * - 의존하는 모듈: lib/env_dpdk/ (spdk_ring/spdk_mempool/spdk_get_ticks 가 DPDK rte_ring/
 *   rte_mempool/rdtsc 로 위임), lib/util/fd_group.c (epoll 백엔드), lib/log/ (로깅), lib/trace/
 *   (DTRACE_PROBE), spdk/cpuset.h (스레드 affinity), spdk/queue.h (TAILQ/SLIST), spdk/tree.h
 *   (RB tree intrusive 매크로 — timed_pollers/io_channels/io_devices/thread_links 4 종에 사용).
 * - 이 파일에 의존하는 모듈: 거의 모든 SPDK 라이브러리. lib/event/reactor.c 가 가장 직접적인
 *   소비자이며 spdk_thread_create()/spdk_thread_poll()/spdk_thread_send_msg() 를 통해 reactor
 *   루프를 구성. lib/bdev/bdev.c, lib/nvme/nvme.c, lib/nvmf/, lib/blob/, lib/sock/ 등은 모두
 *   spdk_get_io_channel() 로 per-thread 자료구조를 캐싱하고 spdk_thread_send_msg() 로 cross-core
 *   메시지를 주고받는다.
 * - 데이터 흐름: 외부 → spdk_thread_send_msg() → MPSC ring(thread->messages) → 타깃 thread 의
 *   spdk_thread_poll() → msg_queue_run_batch() → msg->fn(arg) 호출. timer 는 RB tree
 *   (timed_pollers) 에 next_run_tick 키로 정렬되어 first_timed_poller cache 로 O(1) 검사.
 * - 공유 핵심 자료구조: g_threads (TAILQ, devlist_mutex 보호), g_io_devices (RB, devlist_mutex
 *   보호), g_spdk_msg_mempool (DPDK lockless mempool), tls_thread (per-pthread TLS).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_thread_create / spdk_thread_exit / spdk_thread_destroy / _free_thread:
 *   spdk_thread 객체의 lifecycle. create 는 64KB rte_ring + per-thread msg cache + 선택적
 *   fd_group 을 할당. exit 는 모든 poller/io_channel 이 정리될 때까지 EXITING 상태로 polling.
 * - spdk_thread_poll / thread_poll: hot path. 매 루프 critical_msg → 메시지 batch → active
 *   poller 역순 → timed poller (cache 우선) → post_poller_handler 순. busy_tsc/idle_tsc 누적.
 * - spdk_thread_send_msg / spdk_thread_send_critical_msg / msg_queue_run_batch: lockless
 *   cross-thread 메시지. local_thread 의 msg_cache 에서 hot msg 재사용 → spdk_ring_enqueue.
 *   in_interrupt 면 eventfd write 로 epoll wakeup.
 * - spdk_poller_register / poller_register / spdk_poller_unregister / poller_insert_timer:
 *   poller 등록과 RB tree 삽입. period_us=0 → active_pollers TAILQ, 양수 → timed_pollers RB
 *   tree (next_run_tick 정렬). 인터럽트 모드면 timerfd_create / eventfd 도 함께 등록.
 * - spdk_io_device_register / spdk_get_io_channel / spdk_put_io_channel / io_device_free:
 *   per-thread cache + ref count 모델. io_channels RB tree key=ch->dev (각 thread 안에서),
 *   io_device->threads RB tree 는 어떤 thread 들이 채널을 가졌는지 역방향 인덱스.
 * - spdk_for_each_thread / spdk_for_each_channel / _on_thread / _call_channel: send_msg 체인을
 *   사용한 비동기 fan-out. for_each_count 로 in-flight 보호.
 * - spdk_interrupt_register* / spdk_thread_set_interrupt_mode / thread_interrupt_create:
 *   epoll 기반 interrupt 모드. fd_group 에 eventfd/timerfd 를 nest. polling 과 interrupt 사이
 *   동적 전환을 지원하기 위해 모든 poller 의 set_intr_cb_fn 콜백 사용.
 * - spdk_spin_init / spdk_spin_lock / spdk_spin_unlock: pthread_spinlock 래퍼 + 디버그 검사
 *   (deadlock/migration/lock count). DEBUG 빌드는 backtrace 까지 캡처.
 * - 핵심 구조체:
 *   * spdk_thread: 메시지 ring/poller list/RB tree/io_channels/cpumask/state/in_interrupt 등을
 *     모은 거대 구조체. 끝에 user ctx 를 가변 길이로 둔다 (1 reactor=1 thread).
 *   * spdk_poller: state/period_ticks/next_run_tick/fn/arg + RB_ENTRY + TAILQ_ENTRY 동시 보유
 *     (active TAILQ 와 timed RB tree 모두에 들어갈 수 있도록).
 *   * spdk_io_channel: dev/thread/ref/destroy_ref + RB_ENTRY (thread 의 io_channels tree 키).
 *   * io_device: io_device 식별자 + create/destroy/unregister cb + refcnt + threads RB tree.
 *   * thread_link: io_device 가 어떤 thread 들에게 채널을 발급했는지 추적 (id 기반 정렬).
 *   * spdk_interrupt: efd/fgrp/thread/fn/arg — fd_group 에 등록된 인터럽트 핸들러 래퍼.
 */

#include "spdk/stdinc.h"  /* [한국어] SPDK 표준 헤더 모음 (stdint, stdbool, errno, string, unistd 등 일괄 include) */

#include "spdk/env.h"     /* [한국어] DPDK 추상화: spdk_ring/spdk_mempool/spdk_get_ticks/spdk_get_ticks_hz 사용 */
#include "spdk/likely.h"  /* [한국어] spdk_likely/spdk_unlikely (__builtin_expect) — hot path 분기 예측 힌트 */
#include "spdk/queue.h"   /* [한국어] BSD sys/queue.h 의 SPDK 래퍼: TAILQ_/SLIST_/RB_/STAILQ_ 매크로 */
#include "spdk/string.h"  /* [한국어] spdk_strerror 등 문자열 헬퍼 */
#include "spdk/thread.h"  /* [한국어] 본 파일이 구현하는 공개 API 헤더 — 시그니처 100% 일치해야 함 */
#include "spdk/trace.h"   /* [한국어] SPDK 트레이스 시스템 (TRACE_REGISTER_FN, spdk_trace_record) */
#include "spdk/util.h"    /* [한국어] SPDK_CONTAINEROF, SPDK_ALIGN_CEIL, SPDK_COUNTOF 등 유틸 매크로 */
#include "spdk/fd_group.h" /* [한국어] epoll 기반 fd_group — interrupt 모드의 백엔드 (eventfd/timerfd 다중 대기) */

#include "spdk/log.h"     /* [한국어] SPDK_ERRLOG/INFOLOG/DEBUGLOG/NOTICELOG 매크로 */
#include "spdk_internal/thread.h"   /* [한국어] 내부 헤더 — spdk_thread 의 비공개 멤버(io_channel 등)에 접근 */
#include "spdk_internal/usdt.h"     /* [한국어] User Statically-Defined Tracing — DTrace 마크 (msg_exec, timerfd_exec 등) */
#include "thread_internal.h"        /* [한국어] thread.c/iobuf.c 사이의 전용 내부 헤더 */

#include "spdk_internal/trace_defs.h" /* [한국어] TRACE_THREAD_IOCH_GET/PUT 등 trace tpoint 정의 */

#ifdef __linux__
#include <sys/timerfd.h>  /* [한국어] timerfd_create/timerfd_settime — period poller 의 epoll 친화적 타이머 */
#include <sys/eventfd.h>  /* [한국어] eventfd — busy poller wakeup 및 thread msg_fd 알림 채널 */
#endif

#ifdef SPDK_HAVE_EXECINFO_H
#include <execinfo.h>     /* [한국어] backtrace/backtrace_symbols — DEBUG 빌드 spinlock 호출 스택 캡처 */
#endif

/* [한국어] 한 번의 spdk_thread_poll() 에서 메시지 ring 으로부터 dequeue 할 최대 메시지 수.
 * 너무 크면 한 라운드의 latency 가 늘고, 너무 작으면 CPU 분기·함수 호출 오버헤드가 늘어남.
 * 8 은 cache line(64B) 안에 들어가는 8개의 포인터(64bit) 라는 의미와도 부합. */
#define SPDK_MSG_BATCH_SIZE		8

/* [한국어] io_device 이름 최대 길이 (NUL 제외 256B). 트레이스/로그에서 식별자로 사용. */
#define SPDK_MAX_DEVICE_NAME_LEN	256

/* [한국어] spdk_thread_exit() 호출 후 모든 poller/io_channel 정리를 기다리는 최대 시간(초).
 * 타임아웃 시 강제로 EXITED 로 전환하여 데드락 방지. */
#define SPDK_THREAD_EXIT_TIMEOUT_SEC	5

/* [한국어] poller 이름 최대 길이 (NUL 제외 256B). spdk_poller_register_named() 인자. */
#define SPDK_MAX_POLLER_NAME_LEN	256

/* [한국어] thread 이름 최대 길이 (NUL 제외 256B). spdk_thread_create() 인자. */
#define SPDK_MAX_THREAD_NAME_LEN	256

/* [한국어] 어플리케이션 메인 thread 포인터. 가장 처음 spdk_thread_create() 된 객체가 자동으로 등록되며
 * (CAS 로 race-free), spdk_thread_lib_fini() 시점까지 free 가 보류된다. RPC/init/fini 등은 보통 이
 * thread 위에서 동작.
 * 설정자: spdk_thread_create() 의 __atomic_compare_exchange_n (NULL → first thread).
 * 읽는 자: spdk_thread_get_app_thread(), spdk_thread_is_app_thread().
 * 동기화: 첫 등록은 CAS, 이후 read-only — atomic 보장 충분. */
static struct spdk_thread *g_app_thread;

/*
 * [한국어] spdk_interrupt 구조체 — fd_group 에 등록된 단일 fd 의 SPDK 인터럽트 핸들러.
 * spdk_thread 의 fgrp(epoll fd) 에 nest 되어, 해당 fd 가 readable 해지면 fn(arg) 가 호출된다.
 * efd 와 fgrp 는 상호배타 — 단일 fd 인터럽트면 efd 사용, 다른 fd_group 을 nest 하면 fgrp 사용.
 */
struct spdk_interrupt {
	int			efd;
	/* [한국어] 인터럽트 소스 file descriptor (eventfd/timerfd/socketfd 등).
	 * 설정자: spdk_interrupt_register_ext() / alloc_interrupt() 가 efd 인자를 그대로 저장.
	 * 읽는 자: spdk_fd_group 에 등록될 때 키로 사용, spdk_interrupt_unregister() 가 spdk_fd_group_remove 호출.
	 * 값 범위: 유효한 양수 fd 또는 -1 (fgrp 모드일 때).
	 * 동기화: 소유 thread 한정 사용 — wrong_thread() 검사로 cross-thread 오용 방지. */

	struct spdk_fd_group	*fgrp;
	/* [한국어] efd 대신 다른 fd_group 전체를 nest 할 때 사용하는 포인터 (spdk_interrupt_register_fd_group).
	 * 설정자: spdk_interrupt_register_fd_group() / alloc_interrupt(efd=-1, fgrp=given).
	 * 읽는 자: spdk_interrupt_unregister() 가 spdk_fd_group_unnest 호출.
	 * 값 범위: 유효한 fd_group 포인터 또는 NULL (efd 모드).
	 * 동기화: efd 와 동일하게 소유 thread 한정. */

	struct spdk_thread	*thread;
	/* [한국어] 이 인터럽트를 등록한 spdk_thread (자동으로 spdk_get_thread() 캡처).
	 * 설정자: alloc_interrupt() 진입 시점의 tls_thread.
	 * 읽는 자: _interrupt_wrapper() 에서 콜백 실행 전 spdk_set_thread() 로 컨텍스트 복원, wrong_thread 검사.
	 * 값 범위: NULL 불가 — 비-SPDK thread 에서 호출 시 alloc_interrupt 가 NULL 반환.
	 * 동기화: 변경되지 않음 (immutable after alloc). */

	spdk_interrupt_fn	fn;
	/* [한국어] fd 가 readable 해질 때 호출될 사용자 콜백 함수.
	 * 설정자: spdk_interrupt_register*() 의 인자.
	 * 읽는 자: _interrupt_wrapper() 가 fd_group 으로부터 wakeup 시 호출.
	 * 값 범위: 유효한 함수 포인터 (fgrp 모드에서는 NULL 가능 — wrapper 가 fd_group 자체 콜백을 사용).
	 * 동기화: 변경 없음. */

	void			*arg;
	/* [한국어] fn 호출 시 첫 인자로 전달되는 사용자 컨텍스트.
	 * 설정자/읽는 자: 위 fn 과 동일 lifecycle. NULL 가능. */

	char			name[SPDK_MAX_POLLER_NAME_LEN + 1];
	/* [한국어] 디버그/트레이스용 이름. 미지정 시 fn 포인터를 hex 로 인쇄.
	 * 설정자: alloc_interrupt() 의 snprintf. NUL 종료 보장. */
};

/*
 * [한국어] poller 의 5개 상태 머신.
 * 정상 흐름: WAITING(등록 후) → RUNNING(fn 실행 중) → WAITING(끝나고 다시 대기).
 * 일시정지: WAITING/RUNNING → PAUSING(예약) → spdk_thread_poll 이 PAUSED 로 옮기며 paused_pollers 로 이동.
 * 해제: 어느 상태에서든 → UNREGISTERED → 다음 poll 라운드에 free.
 * 상태는 항상 poller 의 소유 thread 에서만 접근하므로 atomic 불필요.
 */
enum spdk_poller_state {
	/* [한국어] 등록되었으나 현재 fn 을 실행 중이 아닌 상태 — active_pollers TAILQ 또는 timed_pollers RB tree 에 존재. */
	/* The poller is registered with a thread but not currently executing its fn. */
	SPDK_POLLER_STATE_WAITING,

	/* [한국어] poller->fn(arg) 가 실행 중. fn 안에서 자기 자신을 unregister/pause 하는 경우를 감지하기 위해 마킹. */
	/* The poller is currently running its fn. */
	SPDK_POLLER_STATE_RUNNING,

	/* [한국어] fn 실행 도중 spdk_poller_unregister() 가 호출된 상태. fn 종료 후 free 처리. */
	/* The poller was unregistered during the execution of its fn. */
	SPDK_POLLER_STATE_UNREGISTERED,

	/* [한국어] spdk_poller_pause() 호출 후, 다음 실행 시점까지 PAUSING 으로 마킹된 상태.
	 * 다음 라운드의 thread_execute_poller 가 active/timed list 에서 빼서 paused_pollers 로 옮긴다. */
	/* The poller is in the process of being paused.  It will be paused
	 * during the next time it's supposed to be executed.
	 */
	SPDK_POLLER_STATE_PAUSING,

	/* [한국어] 일시정지 완료 — paused_pollers TAILQ 에 존재하며 spdk_poller_resume() 까지 대기. */
	/* The poller is registered but currently paused.  It's on the
	 * paused_pollers list.
	 */
	SPDK_POLLER_STATE_PAUSED,
};

/*
 * [한국어] spdk_poller — SPDK 의 폴링 단위. period 에 따라 두 컬렉션 중 하나에만 들어감:
 * (1) period_us = 0 (period_ticks = 0) → active_pollers TAILQ (매 라운드 호출, raw busy poll)
 * (2) period_us > 0 → timed_pollers RB tree (next_run_tick 기준 ordered, 만기 도래 시만 호출)
 * paused_pollers TAILQ 는 위 둘과 별개로, pause 된 동안 들어가는 보류 큐. tailq/node 두 ENTRY 를 모두
 * 가지므로 같은 poller 를 active <-> paused 로 옮기거나 timed <-> paused 로 옮길 때 같은 객체를 재사용.
 */
struct spdk_poller {
	TAILQ_ENTRY(spdk_poller)	tailq;
	/* [한국어] active_pollers 또는 paused_pollers TAILQ 에 들어갈 때 사용하는 링크.
	 * 동일 poller 가 두 큐를 동시에 점유하지는 않음 (state 로 구분). */

	RB_ENTRY(spdk_poller)		node;
	/* [한국어] timed_pollers RB tree 에 들어갈 때 사용하는 RB 노드 (key=next_run_tick).
	 * sys/tree.h 매크로 — 부모 포인터 하위 비트에 색상 인코딩 (3 word 최적화). */

	/* [한국어] 현재 상태 (위 enum). 항상 poller->thread 에서만 read/write — TLS 모델로 lock 불필요. */
	/* Current state of the poller; should only be accessed from the poller's thread. */
	enum spdk_poller_state		state;

	uint64_t			period_ticks;
	/* [한국어] 호출 주기 (TSC 틱 단위). convert_us_to_ticks() 로 us → tick 변환.
	 * 0 이면 active_pollers (raw busy poll), 양수면 timed. */

	uint64_t			next_run_tick;
	/* [한국어] timed poller 의 다음 만기 시점 (절대 TSC). RB tree 정렬 키.
	 * 설정자: poller_insert_timer (now + period_ticks).
	 * 읽는 자: thread_poll() 핫 루프 — first_timed_poller cache 와 비교. */

	uint64_t			run_count;
	/* [한국어] fn 호출 누적 횟수. spdk_poller_get_stats() 노출. RPC monitoring 용. */

	uint64_t			busy_count;
	/* [한국어] fn 이 양수(>0) 를 반환한 횟수 — 실제 일을 한 라운드. busy_count/run_count 로 idle 비율 추정. */

	uint64_t			id;
	/* [한국어] thread 내에서의 monotonic poller ID (1 부터). thread->next_poller_id 가 발급.
	 * 0 까지 wrap 시 경고 로그 후 1 부터 재시작 (ID 충돌 가능). spdk_poller_get_id() 노출. */

	spdk_poller_fn			fn;
	/* [한국어] poller 본체 — int(*fn)(void *arg). 0=idle, >0=busy, -1=내부 디버그. */

	void				*arg;
	/* [한국어] fn 호출 시 전달되는 사용자 컨텍스트. */

	struct spdk_thread		*thread;
	/* [한국어] poller 가 등록된 spdk_thread. 등록 시점의 spdk_get_thread() 가 캡처되며 변경 불가.
	 * unregister/pause/resume 모두 이 thread 에서만 가능 — wrong_thread() 검사. */

	struct spdk_interrupt		*intr;
	/* [한국어] interrupt 모드일 때 이 poller 의 timerfd/eventfd 인터럽트 핸들러.
	 * polling 모드에서는 NULL. period>0 → timerfd, period=0 → eventfd(busy). */

	spdk_poller_set_interrupt_mode_cb set_intr_cb_fn;
	/* [한국어] polling↔interrupt 모드 전환 시 호출될 콜백. period_poller_set_interrupt_mode 또는
	 * busy_poller_set_interrupt_mode 가 기본 — 사용자 정의는 spdk_poller_register_interrupt() 로 교체. */

	void				*set_intr_cb_arg;
	/* [한국어] set_intr_cb_fn 의 cb_arg. 기본 콜백은 NULL. */

	char				name[SPDK_MAX_POLLER_NAME_LEN + 1];
	/* [한국어] poller 이름. 미지정 시 fn 의 hex 주소. spdk_poller_get_name() 및 RPC 출력. */
};

/*
 * [한국어] spdk_thread 의 lifecycle 상태 머신.
 * RUNNING (정상) → EXITING (spdk_thread_exit 호출 후, poller/io_channel 정리 대기)
 *                → EXITED (정리 완료 또는 timeout, spdk_thread_destroy 가능).
 */
enum spdk_thread_state {
	/* [한국어] 정상 동작 — spdk_thread_poll() 이 메시지/poller 처리. */
	/* The thread is processing poller and message by spdk_thread_poll(). */
	SPDK_THREAD_STATE_RUNNING,

	/* [한국어] 종료 진행 중. exit_timeout_tsc 까지 매 라운드 thread_exit() 가 검사하여 모든 자원이
	 * 정리되면 EXITED 로 전환. unregister 보류 카운트가 0 이 되거나 timeout 시 강제 EXITED. */
	/* The thread is in the process of termination. It reaps unregistering
	 * poller are releasing I/O channel.
	 */
	SPDK_THREAD_STATE_EXITING,

	/* [한국어] 종료 완료 — spdk_thread_destroy() 호출 가능, send_msg 시 abort. */
	/* The thread is exited. It is ready to call spdk_thread_destroy(). */
	SPDK_THREAD_STATE_EXITED,
};

/*
 * [한국어] post-poller handler — spdk_thread_poll 의 한 라운드에서 active poller 1 개를 실행하고
 * 그 직후 한 번만 호출되는 콜백. 보통 어떤 poller 가 batch 작업을 시작한 뒤 같은 라운드에서 마무리해야
 * 할 때 사용 (예: bdev I/O completion 일괄 처리). 최대 4 개 등록 (SPDK_THREAD_MAX_POST_POLLER_HANDLERS).
 */
struct spdk_thread_post_poller_handler {
	spdk_post_poller_fn fn;
	/* [한국어] post-poller 콜백 함수. NULL 이면 슬롯 비어있음. */
	void *fn_arg;
	/* [한국어] fn 호출 시 전달될 사용자 컨텍스트. */
};

/* [한국어] 한 라운드에서 등록 가능한 post-poller handler 최대 개수. 4 는 보통 충분 — bdev/NVMe/
 * 사용자 등 케이스 합쳐 4 개 미만. 초과 시 spdk_thread_register_post_poller_handler 가 ERRLOG. */
#define SPDK_THREAD_MAX_POST_POLLER_HANDLERS (4)

/*
 * [한국어] spdk_thread — SPDK 의 논리 스레드 객체. 1 reactor=1 spdk_thread 모델.
 * 모든 poller/message/io_channel 컨테이너를 한곳에 모아 cache locality 를 극대화하며,
 * 사용자 컨텍스트 ctx[] 는 가변 길이로 끝에 배치된다 (헤더의 SPDK_STATIC_ASSERT 가 8B 정렬 강제).
 * 구조체 자체는 SPDK_CACHE_LINE_SIZE 정렬로 할당되어 false sharing 회피.
 */
struct spdk_thread {
	uint64_t			tsc_last;
	/* [한국어] 직전 spdk_thread_poll() 라운드의 종료 TSC. busy/idle 시간 누적의 시작 시점.
	 * 설정자: thread_update_stats() 가 라운드 끝의 spdk_get_ticks() 를 저장.
	 * 읽는 자: 다음 라운드 thread_poll() 시작 시 누적 구간 계산. */

	struct spdk_thread_stats	stats;
	/* [한국어] busy_tsc / idle_tsc 누적 통계 — scheduler 가 부하 균형 결정에 사용 (busy_ratio).
	 * spdk_thread_get_stats() 로 RPC 노출. */

	/* [한국어] period=0 인 active poller 의 TAILQ. round-robin: 매 라운드 tail 에서 head 방향으로
	 * 순회 (TAILQ_FOREACH_REVERSE_SAFE) 하여 가장 오래된 것부터 처리. */
	/*
	 * Contains pollers actively running on this thread.  Pollers
	 *  are run round-robin. The thread takes one poller from the head
	 *  of the ring, executes it, then puts it back at the tail of
	 *  the ring.
	 */
	TAILQ_HEAD(active_pollers_head, spdk_poller)	active_pollers;

	/* [한국어] period>0 인 timed poller 의 RB tree. key=next_run_tick (ascending).
	 * 만기 검사는 RB_MIN 대신 first_timed_poller 캐시로 O(1). */
	/**
	 * Contains pollers running on this thread with a periodic timer.
	 */
	RB_HEAD(timed_pollers_tree, spdk_poller)	timed_pollers;

	struct spdk_poller				*first_timed_poller;
	/* [한국어] timed_pollers RB tree 의 최좌측(가장 빠른 만기) 캐시. RB_MIN 호출 절약 hot path 최적화.
	 * 설정자: poller_insert_timer/poller_remove_timer/thread_poll 안에서 갱신.
	 * 읽는 자: thread_poll() 의 timed poller 루프 진입 — now < first->next_run_tick 이면 skip. */

	/* [한국어] 일시정지된 poller TAILQ. resume 시 thread_insert_poller 로 active/timed 로 환원. */
	/*
	 * Contains paused pollers.  Pollers on this queue are waiting until
	 * they are resumed (in which case they're put onto the active/timer
	 * queues) or unregistered.
	 */
	TAILQ_HEAD(paused_pollers_head, spdk_poller)	paused_pollers;

	struct spdk_thread_post_poller_handler		pp_handlers[SPDK_THREAD_MAX_POST_POLLER_HANDLERS];
	/* [한국어] post-poller handler 슬롯 4개. 한 active poller 실행 직후 thread_run_pp_handlers 에서 일괄 호출. */

	struct spdk_ring		*messages;
	/* [한국어] cross-thread 메시지 ring (DPDK rte_ring 래퍼, MP-SC, 64K slots).
	 * 설정자: spdk_thread_create() 가 spdk_ring_create(SPDK_RING_TYPE_MP_SC, 65536, …).
	 * 읽는 자: msg_queue_run_batch (소비자=소유 thread 만), spdk_ring_count.
	 * 동기화: producer 다중 (lockless CAS), consumer 단일 — DPDK rte_ring 의 SP/MP/SC/MC 모드 중 MPSC. */

	uint8_t				num_pp_handlers;
	/* [한국어] 현재 등록된 post-poller handler 수 (0..4). thread_run_pp_handlers 진입 시 MAX 로 임시 세팅하여
	 * 콜백 안에서 추가 등록되지 않도록 함 (재진입 방지). */

	int				msg_fd;
	/* [한국어] interrupt 모드의 메시지 wakeup eventfd. polling 모드에서는 -1.
	 * 설정자: thread_interrupt_create() 에서 eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC).
	 * 읽는 자: thread_send_msg_notification 이 write 로 상대 thread 깨움, thread_interrupt_msg_process 가 read 로 클리어. */

	SLIST_HEAD(, spdk_msg)		msg_cache;
	/* [한국어] 이 thread 가 보유한 spdk_msg 구조체 hot cache (LIFO SLIST).
	 * spdk_thread_send_msg() 가 mempool 대신 여기서 먼저 꺼내고, msg_queue_run_batch 가 처리 후 다시 push.
	 * 설정자: spdk_thread_create() 의 bulk fill, send_msg/run_batch.
	 * 동기화: 이 thread 자신에서만 접근 — lockless. */

	size_t				msg_cache_count;
	/* [한국어] msg_cache SLIST 길이. SPDK_MSG_MEMPOOL_CACHE_SIZE 까지만 채움. */

	spdk_msg_fn			critical_msg;
	/* [한국어] 일반 메시지 ring 이 가득찼거나 우회해야 하는 긴급 메시지 슬롯 (단일 슬롯).
	 * 설정자: spdk_thread_send_critical_msg() 가 NULL→fn 으로 atomic CAS.
	 * 읽는 자: thread_poll() 가 매 라운드 시작에 검사·실행·NULL 클리어. */

	uint64_t			id;
	/* [한국어] 글로벌 monotonic thread ID (1 부터). g_thread_id 발급. spdk_thread_get_id() 노출. */

	uint64_t			next_poller_id;
	/* [한국어] 이 thread 에서 다음 등록될 poller 에 부여될 ID. wrap 시 1 부터 재시작 (경고). */

	enum spdk_thread_state		state;
	/* [한국어] RUNNING/EXITING/EXITED 상태. spdk_thread_exit/_thread_exit/thread_exit 가 전이 책임. */

	int				pending_unregister_count;
	/* [한국어] 보류 중인 io_device unregister 콜백 수. 0 이 되어야 thread 가 EXITED 로 전환 가능. */

	uint32_t			for_each_count;
	/* [한국어] 진행 중인 spdk_for_each_thread/spdk_for_each_channel 횟수 (이 thread 가 originator).
	 * 0 이 되어야 thread_exit 가 EXITED 로 전이 가능. */

	RB_HEAD(io_channel_tree, spdk_io_channel)	io_channels;
	/* [한국어] 이 thread 가 보유한 모든 io_channel 의 RB tree (key=ch->dev 주소).
	 * 같은 io_device 에 대해 중복 발급되지 않도록 spdk_get_io_channel 이 RB_FIND 로 캐시 검색.
	 * 동기화: 소유 thread 한정 — 하지만 g_devlist_mutex 안에서 RB_INSERT/REMOVE/FOREACH (cross-thread iter 보호). */

	TAILQ_ENTRY(spdk_thread)			tailq;
	/* [한국어] 글로벌 g_threads TAILQ 링크 (devlist_mutex 보호). */

	char				name[SPDK_MAX_THREAD_NAME_LEN + 1];
	/* [한국어] thread 이름. 미지정 시 thread 포인터의 hex. */

	struct spdk_cpuset		cpumask;
	/* [한국어] 이 thread 가 실행 가능한 CPU 코어 비트맵. scheduler 가 reschedule 결정에 사용.
	 * 설정자: spdk_thread_create(cpumask) / spdk_thread_set_cpumask().
	 * 미지정 시 spdk_cpuset_negate (모든 코어 허용). */

	uint64_t			exit_timeout_tsc;
	/* [한국어] EXITING 진입 후 강제 EXITED 로 전환할 deadline TSC. SPDK_THREAD_EXIT_TIMEOUT_SEC=5s. */

	int32_t				lock_count;
	/* [한국어] 현재 보유 중인 spdk_spinlock 수. spdk_thread 가 CPU 를 떠날 때(lock 잡은 채 스레드 마이그레이션)
	 * 0 이어야 함 — SPIN_ASSERT(SPIN_ERR_HOLD_DURING_SWITCH) 가 검사. 디버그 안전망. */

	/* [한국어] 현재 thread 가 특정 pthread/CPU 에 bound 되어 있는지 — bound 시 scheduler 가 옮기지 않음. */
	/* spdk_thread is bound to current CPU core. */
	bool				is_bound;

	/* [한국어] 현재 interrupt 모드인지. spdk_thread_set_interrupt_mode 로 동적 전환 가능. */
	/* Indicates whether this spdk_thread currently runs in interrupt. */
	bool				in_interrupt;

	bool				poller_unregistered;
	/* [한국어] interrupt 모드에서 poller unregister 메시지가 in-flight 인지 표시 (중복 send_msg 방지). */

	struct spdk_fd_group		*fgrp;
	/* [한국어] interrupt 모드의 epoll 백엔드 fd_group. msg_fd, 모든 poller/interrupt 의 efd 가 nest.
	 * 설정자: thread_interrupt_create() 가 spdk_fd_group_create 로 생성.
	 * 읽는 자: spdk_thread_poll() 의 interrupt 분기에서 spdk_fd_group_wait(0). */

	uint16_t			trace_id;
	/* [한국어] spdk_trace 시스템에서 이 thread 를 식별하는 owner ID. spdk_trace_register_owner 가 발급. */

	uint8_t				reserved[6];
	/* [한국어] 8B 정렬 패딩 — ctx[] 가 8B 정렬되도록 보장 (SPDK_STATIC_ASSERT). */

	/* [한국어] 가변 길이 사용자 컨텍스트. spdk_thread_lib_init(ctx_sz) 가 결정. spdk_thread_get_ctx() 노출. */
	/* User context allocated at the end */
	uint8_t				ctx[0];
};

/* [한국어] ctx[] 가 8B 정렬되도록 spdk_thread 자체 크기가 8B 배수임을 컴파일 타임 검증.
 * 가변 길이 ctx 의 정렬은 ctx 전체가 SPDK_CACHE_LINE_SIZE 정렬된 후 ctx 시작 오프셋이 8B 배수여야 가능. */
/*
 * Assert that spdk_thread struct is 8 byte aligned to ensure
 * the user ctx is also 8-byte aligned.
 */
SPDK_STATIC_ASSERT((sizeof(struct spdk_thread)) % 8 == 0, "Incorrect size");

/* [한국어] 글로벌 자료구조(g_threads, g_io_devices, io_channel_tree, dev->threads) 보호 mutex.
 * 일반적인 hot path (send_msg, poller execute) 는 mutex 를 잡지 않음 — 이 mutex 는 register/
 * unregister/iter (저빈도) 경로에서만 사용. PTHREAD_MUTEX_INITIALIZER 로 정적 초기화. */
static pthread_mutex_t g_devlist_mutex = PTHREAD_MUTEX_INITIALIZER;

/* [한국어] spdk_thread_lib_init() 에 등록된 새 thread 알림 콜백 (legacy 단순 버전).
 * 새로 spdk_thread_create() 가 성공하면 호출되어 reactor framework 에 thread 를 등록할 기회 제공. */
static spdk_new_thread_fn g_new_thread_fn = NULL;

/* [한국어] spdk_thread_lib_init_ext() 에 등록된 thread op 콜백 (모던 인터페이스).
 * NEW/RESCHED 등 다양한 op 를 dispatch 할 수 있어 thread migration / cpumask 변경 지원. */
static spdk_thread_op_fn g_thread_op_fn = NULL;

/* [한국어] 특정 op 가 지원되는지 framework 에 묻는 콜백. RESCHED 미지원이면 spdk_thread_set_cpumask 가 -ENOTSUP. */
static spdk_thread_op_supported_fn g_thread_op_supported_fn;

/* [한국어] spdk_thread 에 부착될 사용자 ctx 의 크기 (바이트). spdk_thread_lib_init 진입 시 결정.
 * 0 이면 spdk_thread_get_ctx() 가 NULL 반환. */
static size_t g_ctx_sz = 0;

/* [한국어] 글로벌 monotonic thread ID 카운터. spdk_thread_create() 에서 g_thread_id++ 로 발급.
 * UINT64_MAX 도달 시 wrap 되어 0 → 더 이상 생성 불가 (재시작 필요). g_devlist_mutex 보호. */
/* Monotonic increasing ID is set to each created thread beginning at 1. Once the
 * ID exceeds UINT64_MAX, further thread creation is not allowed and restarting
 * SPDK application is required.
 */
static uint64_t g_thread_id = 1;

/*
 * [한국어] spdk_spinlock 디버그 시 사용되는 에러 코드. abort() 직전 SPDK_ERRLOG 로 출력됨.
 * 모든 코드는 g_spin_abort_fn(__posix_abort) 를 통해 abort() — 진짜로 SPDK 가 죽는 즉시 진단 가능.
 */
enum spin_error {
	SPIN_ERR_NONE,                  /* [한국어] 정상 (사용되지 않는 placeholder, 0 = 에러 없음). */
	/* [한국어] 비-SPDK pthread 에서 spdk_spin_* 를 호출 — TLS 검사 실패. */
	/* Trying to use an SPDK lock while not on an SPDK thread */
	SPIN_ERR_NOT_SPDK_THREAD,
	/* [한국어] 같은 SPDK thread 가 이미 보유한 spinlock 을 다시 lock — recursive 미지원, 데드락. */
	/* Trying to lock a lock already held by this SPDK thread */
	SPIN_ERR_DEADLOCK,
	/* [한국어] 보유하지 않은 thread 가 unlock 시도 — 소유권 위배. */
	/* Trying to unlock a lock not held by this SPDK thread */
	SPIN_ERR_WRONG_THREAD,
	/* [한국어] 내부 pthread_spin_init/lock/unlock/destroy 가 errno 반환. */
	/* pthread_spin_*() returned an error */
	SPIN_ERR_PTHREAD,
	/* [한국어] 보유 중인 spinlock 을 destroy 시도 — 메모리 누수·UB 위험. */
	/* Trying to destroy a lock that is held */
	SPIN_ERR_LOCK_HELD,
	/* [한국어] thread->lock_count 가 음수가 되거나 inconsistent — internal bug. */
	/* lock_count is invalid */
	SPIN_ERR_LOCK_COUNT,
	/* [한국어] spinlock 을 잡은 채로 spdk_thread 가 CPU 를 떠나려 함 (poller fn 종료, send_msg 등).
	 * 다른 SPDK thread 가 같은 pthread 를 hosting 하면 데드락 가능 → 모든 매 라운드 SPIN_ASSERT 검사. */
	/*
	 * An spdk_thread may migrate to another pthread. A spinlock held across migration leads to
	 * undefined behavior. A spinlock held when an SPDK thread goes off CPU would lead to
	 * deadlock when another SPDK thread on the same pthread tries to take that lock.
	 */
	SPIN_ERR_HOLD_DURING_SWITCH,
	/* [한국어] destroy 된 spinlock 을 재사용 시도 (re-init 없이). */
	/* Trying to use a lock that was destroyed (but not re-initialized) */
	SPIN_ERR_DESTROYED,
	/* [한국어] init 되지 않은 spinlock 사용 시도. */
	/* Trying to use a lock that is not initialized */
	SPIN_ERR_NOT_INITIALIZED,

	/* [한국어] enum 길이 sentinel — 실제 에러 아님. SPDK_COUNTOF 보다는 spin_error_strings 배열 검사용. */
	/* Must be last, not an actual error code */
	SPIN_ERR_LAST
};

/* [한국어] SPIN_ERROR_STRING 에서 enum → 사람 읽는 문자열 매핑. designated initializer 로 인덱스 명시. */
static const char *spin_error_strings[] = {
	[SPIN_ERR_NONE]			= "No error",
	[SPIN_ERR_NOT_SPDK_THREAD]	= "Not an SPDK thread",
	[SPIN_ERR_DEADLOCK]		= "Deadlock detected",
	[SPIN_ERR_WRONG_THREAD]		= "Unlock on wrong SPDK thread",
	[SPIN_ERR_PTHREAD]		= "Error from pthread_spinlock",
	[SPIN_ERR_LOCK_HELD]		= "Destroying a held spinlock",
	[SPIN_ERR_LOCK_COUNT]		= "Lock count is invalid",
	[SPIN_ERR_HOLD_DURING_SWITCH]	= "Lock(s) held while SPDK thread going off CPU",
	[SPIN_ERR_DESTROYED]		= "Lock has been destroyed",
	[SPIN_ERR_NOT_INITIALIZED]	= "Lock has not been initialized",
};

/* [한국어] 안전한 인덱싱 매크로 — 잘못된 err 값이면 "Unknown error" 반환 (배열 OOB 회피). */
#define SPIN_ERROR_STRING(err) (err < 0 || err >= SPDK_COUNTOF(spin_error_strings)) \
				? "Unknown error" : spin_error_strings[err]

/*
 * [한국어]
 * __posix_abort - 기본 spinlock 어설션 실패 핸들러.
 *
 * @err: 발생한 spin_error 코드 (호출자가 SPDK_ERRLOG 로 이미 출력함).
 *
 * abort(3) 를 호출하여 즉시 SIGABRT 로 프로세스 종료. core dump 가 생성되어 디버거로 분석 가능.
 * 단위 테스트에서는 g_spin_abort_fn 을 다른 핸들러로 교체하여 abort 회피 가능.
 */
static void
__posix_abort(enum spin_error err)
{
	abort();
}

/* [한국어] spin abort 핸들러 타입 — 테스트에서 g_spin_abort_fn 을 교체할 수 있도록 함수 포인터로 분리. */
typedef void (*spin_abort)(enum spin_error err);
/* [한국어] 현재 활성 abort 핸들러. 기본은 __posix_abort. */
spin_abort g_spin_abort_fn = __posix_abort;

/* [한국어] 모든 spin assert 매크로의 공통 골격.
 * cond 가 거짓이면 SPDK_ERRLOG → extra_log → abort_fn(err) → ret. spdk_unlikely 로 예측 분기. */
#define SPIN_ASSERT_IMPL(cond, err, extra_log, ret) \
	do { \
		if (spdk_unlikely(!(cond))) { \
			SPDK_ERRLOG("unrecoverable spinlock error %d: %s (%s)\n", err, \
				    SPIN_ERROR_STRING(err), #cond); \
			extra_log; \
			g_spin_abort_fn(err); \
			ret; \
		} \
	} while (0)
/* [한국어] 위 + sspin_stacks_print(init/lock/unlock 백트레이스) 로 호출 스택 인쇄 후 return. */
#define SPIN_ASSERT_LOG_STACKS(cond, err, lock) \
	SPIN_ASSERT_IMPL(cond, err, sspin_stacks_print(sspin), return)
/* [한국어] 검사 실패 시 지정 값 반환 (ex. spdk_spin_held → false). */
#define SPIN_ASSERT_RETURN(cond, err, ret)	SPIN_ASSERT_IMPL(cond, err, , return ret)
/* [한국어] 검사 실패 시 abort 만 — 추가 로그 없음. hot path (poller exec, send_msg) 에서 사용. */
#define SPIN_ASSERT(cond, err)			SPIN_ASSERT_IMPL(cond, err, ,)

/*
 * [한국어] thread_link — io_device 가 어떤 spdk_thread 들에게 channel 을 발급했는지 역방향 인덱스.
 * io_device->threads (RB tree, key=thread->id) 에 등재되며, spdk_for_each_channel 이 thread 별로 순회할 때 사용.
 */
struct thread_link {
	struct spdk_thread *thread;
	/* [한국어] 채널을 발급받은 thread 포인터. _call_channel 에서 send_msg 대상으로 사용. */
	uint64_t id;
	/* [한국어] thread->id 의 사본 (RB tree key). thread 포인터를 비교하는 대신 id 비교로 안정적 정렬. */
	RB_ENTRY(thread_link)	node;
	/* [한국어] thread_link_tree RB 노드. */
};

/*
 * [한국어] io_device — bdev/NVMe/sock 등 SPDK 모듈이 다루는 가상 디바이스의 메타데이터.
 * spdk_io_device_register() 가 g_io_devices RB tree 에 삽입. 각 thread 별 spdk_io_channel 은 이 객체를
 * dev 로 가리키고, dev->refcnt 는 살아있는 채널 수 + pending unregister.
 */
struct io_device {
	void				*io_device;
	/* [한국어] 사용자 모듈이 제공한 디바이스 식별자 (예: spdk_bdev*, spdk_nvme_ctrlr*).
	 * RB tree 키 (포인터 값 비교). create_cb 인자로 전달. */

	char				name[SPDK_MAX_DEVICE_NAME_LEN + 1];
	/* [한국어] 디버그/로그용 이름. spdk_io_device_get_name() 노출. */

	spdk_io_channel_create_cb	create_cb;
	/* [한국어] 새 io_channel 의 ctx 영역을 초기화하는 사용자 콜백. spdk_get_io_channel cache miss 시 호출. */

	spdk_io_channel_destroy_cb	destroy_cb;
	/* [한국어] io_channel ref 가 0 되어 해제 시 호출되는 사용자 콜백. put_io_channel 안에서 호출. */

	spdk_io_device_unregister_cb	unregister_cb;
	/* [한국어] spdk_io_device_unregister(unregister_cb) — 모든 채널이 정리된 후 호출됨. */

	struct spdk_thread		*unregister_thread;
	/* [한국어] unregister_cb 를 실행할 thread (보통 unregister 호출자). _finish_unregister 가 send_msg 대상. */

	uint32_t			ctx_size;
	/* [한국어] io_channel 뒤에 따라붙는 사용자 ctx 의 크기. spdk_io_channel_get_ctx() 가 ch+1 리턴. */

	uint32_t			for_each_count;
	/* [한국어] 진행 중인 spdk_for_each_channel 작업 수. unregister 가 0 까지 대기 (pending_unregister 사용). */

	RB_ENTRY(io_device)		node;
	/* [한국어] g_io_devices RB tree 노드. */

	uint32_t			refcnt;
	/* [한국어] 살아있는 io_channel 수 + pending. 0 이 되어야 io_device_free 호출. devlist_mutex 보호. */

	bool				pending_unregister;
	/* [한국어] for_each_count > 0 인 동안 unregister 가 호출되었으면 true. for_each 완료 후 자동 unregister 재시도. */

	bool				unregistered;
	/* [한국어] g_io_devices 에서 RB_REMOVE 됨 — 새 channel 발급 불가. 채널이 모두 해제되면 free. */

	RB_HEAD(thread_link_tree, thread_link) threads;
	/* [한국어] 이 io_device 에 대해 채널을 가진 thread 들의 RB tree (key=thread_id).
	 * spdk_for_each_channel 이 RB_MIN/RB_NFIND 로 순회. */
};

/*
 * [한국어]
 * thread_link_compare - thread_link RB tree 정렬 비교자.
 *
 * @tl1, tl2: 비교할 두 thread_link.
 * @return: -1/+1 (id 비교). 같은 id 가 나올 일은 없음 (thread id 는 글로벌 unique).
 *
 * 호출 컨텍스트: RB_INSERT/RB_FIND/RB_NFIND 매크로 내부에서 자동 호출.
 */
static inline int
thread_link_compare(struct thread_link *tl1, struct thread_link *tl2)
{
	return (tl1->id < tl2->id ? -1 : tl1->id > tl2->id); /* [한국어] id 오름차순. */
}

/* [한국어] RB tree 함수 family 생성 (thread_link_tree_RB_INSERT/FIND/REMOVE/MIN/NEXT/PREV/NFIND 등). static 한정. */
RB_GENERATE_STATIC(thread_link_tree, thread_link, node, thread_link_compare);

/* [한국어] 글로벌 io_device RB tree. 모든 spdk_io_device_register() 가 여기에 삽입.
 * 동기화: devlist_mutex (cross-thread 접근). hot path 의 spdk_get_io_channel 도 mutex 보호. */
static RB_HEAD(io_device_tree, io_device) g_io_devices = RB_INITIALIZER(g_io_devices);

/*
 * [한국어]
 * io_device_cmp - io_device RB tree 정렬 비교자.
 *
 * @dev1, dev2: 비교할 두 io_device.
 * @return: -1/+1 (io_device 포인터 값 비교). 포인터가 unique 키.
 */
static int
io_device_cmp(struct io_device *dev1, struct io_device *dev2)
{
	return (dev1->io_device < dev2->io_device ? -1 : dev1->io_device > dev2->io_device);
}

/* [한국어] io_device_tree RB family 생성. */
RB_GENERATE_STATIC(io_device_tree, io_device, node, io_device_cmp);

/*
 * [한국어]
 * io_channel_cmp - per-thread io_channels RB tree 정렬 비교자.
 *
 * @ch1, ch2: 비교할 두 channel.
 * @return: -1/+1 (ch->dev 포인터 비교). dev 가 unique 키 — 한 thread 안에서 한 io_device 당 채널 하나.
 */
static int
io_channel_cmp(struct spdk_io_channel *ch1, struct spdk_io_channel *ch2)
{
	return (ch1->dev < ch2->dev ? -1 : ch1->dev > ch2->dev);
}

/* [한국어] io_channel_tree RB family 생성. spdk_thread.io_channels 가 이 타입. */
RB_GENERATE_STATIC(io_channel_tree, spdk_io_channel, node, io_channel_cmp);

/*
 * [한국어] spdk_msg — cross-thread 메시지 단위. spdk_thread_send_msg(thread, fn, arg) 가 fn/arg 만 채워서
 * 타깃 thread 의 messages ring 으로 전달. 메시지 본체는 g_spdk_msg_mempool 에서 lockless 할당.
 */
struct spdk_msg {
	spdk_msg_fn		fn;     /* [한국어] 타깃 thread 에서 실행될 함수 포인터. */
	void			*arg;   /* [한국어] fn 호출 시 인자. */

	SLIST_ENTRY(spdk_msg)	link;
	/* [한국어] thread->msg_cache (LIFO SLIST) 링크. ring 에 enqueue 될 때는 이 link 미사용. */
};

/* [한국어] 모든 spdk_msg 를 풀링하는 글로벌 lockless mempool (DPDK rte_mempool 래퍼).
 * SPDK_DEFAULT_MSG_MEMPOOL_SIZE (보통 256K) slot. per-thread cache 와 함께 hot path 락프리. */
static struct spdk_mempool *g_spdk_msg_mempool = NULL;

/* [한국어] 전체 spdk_thread 들의 글로벌 TAILQ. spdk_for_each_thread / spdk_thread_get_by_id 가 순회.
 * 동기화: g_devlist_mutex. */
static TAILQ_HEAD(, spdk_thread) g_threads = TAILQ_HEAD_INITIALIZER(g_threads);
/* [한국어] 캐시된 thread count. spdk_thread_get_count() 가 mutex 없이 즉시 반환 (best-effort). */
static uint32_t g_thread_count = 0;

/* [한국어] 현재 pthread 가 hosting 중인 spdk_thread 의 TLS 포인터 (__thread).
 * 한 pthread 가 여러 spdk_thread 를 sequential 하게 host 할 수 있으므로, spdk_thread_poll() 진입/탈출 시
 * tls_thread 를 swap. spdk_get_thread() 가 이 값을 반환. */
static __thread struct spdk_thread *tls_thread = NULL;

/*
 * [한국어]
 * thread_trace - SPDK trace tpoint 등록 (이 파일 전용).
 *
 * 동기/배경: SPDK trace 시스템에 thread 그룹의 owner 타입과 두 tpoint(THREAD_IOCH_GET/PUT) 정의를
 *           등록한다. 이후 spdk_trace_record() 호출 시 이 메타데이터로 빈 자리에 기록되어
 *           spdk_trace_tool 등으로 사후 분석 가능.
 * 동작 단계: (1) tpoint 옵션 배열 정의 (이름/ID/owner/object/argc/arg desc),
 *          (2) spdk_trace_register_owner_type 으로 't' 약자 등록,
 *          (3) spdk_trace_register_description_ext 으로 tpoint 메타데이터 등록.
 * 실행 컨텍스트: SPDK_TRACE_REGISTER_FN 이 트레이스 시스템 초기화 단계에서 자동 호출 (constructor).
 * caller: SPDK trace 초기화 루틴 (lib/trace).
 */
static void
thread_trace(void)
{
	/* [한국어] tpoint 옵션 배열. 각 항목은 trace tool 이 보여줄 텍스트와 인자 메타데이터. */
	struct spdk_trace_tpoint_opts opts[] = {
		{
			/* [한국어] io_channel get 이벤트 — 채널 ref 증가. refcnt 인자(4B) 기록. */
			"THREAD_IOCH_GET", TRACE_THREAD_IOCH_GET,
			OWNER_TYPE_NONE, OBJECT_NONE, 0,
			{{ "refcnt", SPDK_TRACE_ARG_TYPE_INT, 4 }}
		},
		{
			/* [한국어] io_channel put 이벤트 — 채널 ref 감소. */
			"THREAD_IOCH_PUT", TRACE_THREAD_IOCH_PUT,
			OWNER_TYPE_NONE, OBJECT_NONE, 0,
			{{ "refcnt", SPDK_TRACE_ARG_TYPE_INT, 4 }}
		}
	};

	/* [한국어] spdk_trace 가 thread owner 를 't' 약자로 표시하도록 등록. */
	spdk_trace_register_owner_type(OWNER_TYPE_THREAD, 't');
	/* [한국어] tpoint 메타데이터 일괄 등록. */
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
}
/* [한국어] SPDK trace constructor 매크로 — 라이브러리 로드 시 thread_trace() 자동 호출. */
SPDK_TRACE_REGISTER_FN(thread_trace, "thread", TRACE_GROUP_THREAD)

/*
 * [한국어]
 * timed_poller_compare - timed poller RB tree 정렬 비교자.
 *
 * @poller1, poller2: 비교할 두 timed poller.
 * @return: -1 또는 +1 — **0 을 반환하지 않음**. 같은 next_run_tick 인 경우 +1 로 강제하여 우측 서브트리에
 *          중복 삽입 가능. RB_REMOVE 는 키가 아닌 노드 포인터를 받으므로 동등 키가 여럿 있어도 괜찮음.
 *          이렇게 하면 RB_INSERT 가 NULL 을 반환해 (성공) duplicate 검사를 통과.
 *
 * 동기/배경: 여러 poller 가 동시에 같은 만기 시점을 가지는 정상 케이스 (e.g., 동일 period 로 등록된 두 poller)
 *           를 데이터 손실 없이 처리하기 위함.
 */
/*
 * If this compare function returns zero when two next_run_ticks are equal,
 * the macro RB_INSERT() returns a pointer to the element with the same
 * next_run_tick.
 *
 * Fortunately, the macro RB_REMOVE() takes not a key but a pointer to the element
 * to remove as a parameter.
 *
 * Hence we allow RB_INSERT() to insert elements with the same keys on the right
 * side by returning 1 when two next_run_ticks are equal.
 */
static inline int
timed_poller_compare(struct spdk_poller *poller1, struct spdk_poller *poller2)
{
	if (poller1->next_run_tick < poller2->next_run_tick) {
		return -1;     /* [한국어] poller1 이 더 일찍 만기 — 좌측 서브트리. */
	} else {
		return 1;      /* [한국어] poller1 ≥ poller2 — 우측 서브트리 (== 인 경우도 우측). */
	}
}

/* [한국어] timed_pollers_tree RB family 생성. spdk_thread.timed_pollers 가 이 타입. */
RB_GENERATE_STATIC(timed_pollers_tree, spdk_poller, node, timed_poller_compare);

/*
 * [한국어]
 * _get_thread - 현재 pthread 의 tls_thread 반환 (inline).
 *
 * @return: 현재 hosting 중인 spdk_thread, 또는 비-SPDK pthread 면 NULL.
 *
 * 거의 모든 함수가 가장 먼저 호출하는 헬퍼 — TLS 1회 fetch 로 끝. spdk_get_thread() 의 내부 구현.
 */
static inline struct spdk_thread *
_get_thread(void)
{
	return tls_thread;     /* [한국어] __thread 변수 직접 접근 (한 번의 GS/FS segment 액세스). */
}

/*
 * [한국어]
 * _thread_lib_init - thread library 의 공통 초기화 헬퍼.
 *
 * @ctx_sz: 모든 spdk_thread 의 끝에 부착될 사용자 ctx 의 크기 (g_ctx_sz 에 저장).
 * @msg_mempool_sz: g_spdk_msg_mempool 의 슬롯 수 (메시지 동시 in-flight 한계).
 * @return: 0 성공, -ENOMEM 실패.
 *
 * 동기/배경: spdk_thread_lib_init / spdk_thread_lib_init_ext 의 공통 본체. ctx 크기 저장 후
 *           프로세스별 unique 한 mempool 이름("msgpool_<pid>")으로 spdk_mempool_create.
 * 동작 단계: (1) g_ctx_sz 저장, (2) mempool 이름 생성, (3) spdk_mempool_create — DPDK rte_mempool
 *          기반, per-thread cache 비활성(우리가 직접 관리), NUMA any.
 * 실행 컨텍스트: SPDK 초기화 단계의 메인 thread (보통 reactor 시작 전).
 */
static int
_thread_lib_init(size_t ctx_sz, size_t msg_mempool_sz)
{
	char mempool_name[SPDK_MAX_MEMZONE_NAME_LEN];  /* [한국어] DPDK memzone 이름 길이 한계 (보통 32). */

	g_ctx_sz = ctx_sz;                              /* [한국어] 전역에 ctx 크기 저장 — spdk_thread_create 에서 사용. */

	/* [한국어] mempool 이름은 프로세스 unique 해야 hugepage memzone 충돌 회피 (PID 사용). */
	snprintf(mempool_name, sizeof(mempool_name), "msgpool_%d", getpid());
	/* [한국어] DPDK rte_mempool 래퍼 — lockless 슬롯 할당. cache 0 = SPDK 가 spdk_thread::msg_cache 로 직접 관리. */
	g_spdk_msg_mempool = spdk_mempool_create(mempool_name, msg_mempool_sz,
			     sizeof(struct spdk_msg),
			     0, /* No cache. We do our own. */
			     SPDK_ENV_NUMA_ID_ANY);          /* [한국어] NUMA 무관 — 어느 노드든 hugepage 사용. */

	SPDK_DEBUGLOG(thread, "spdk_msg_mempool was created with size: %zu\n",
		      msg_mempool_sz);

	if (!g_spdk_msg_mempool) {
		/* [한국어] hugepage 부족 또는 이름 충돌 — fatal. */
		SPDK_ERRLOG("spdk_msg_mempool creation failed\n");
		return -ENOMEM;
	}

	return 0;
}

/* [한국어] 전방선언 — interrupt 모드 helper. _free_thread/spdk_thread_create 가 호출. */
static void thread_interrupt_destroy(struct spdk_thread *thread);
static int thread_interrupt_create(struct spdk_thread *thread);

/*
 * [한국어]
 * _free_thread - spdk_thread 객체와 모든 부속 자원을 해제.
 *
 * @thread: 해제할 thread (g_threads TAILQ 에 들어있는 상태).
 *
 * 동기/배경: spdk_thread_destroy() 또는 spdk_thread_create() 실패 경로 / spdk_thread_lib_fini() 의
 *           app_thread 처리에서 호출. 잔여 io_channel/poller 가 있으면 경고만 하고 free.
 * 동작 단계: (1) 잔여 io_channel 경고, (2) active/timed/paused poller 모두 free,
 *          (3) g_threads TAILQ 에서 제거 (mutex), (4) msg_cache 비우기, (5) interrupt fgrp 해제,
 *          (6) message ring 해제, (7) thread 자체 free.
 * 실행 컨텍스트: 일반적으로 thread 자신이거나 lib_fini 의 메인. mutex 안에서 g_thread_count-- 만 atomic.
 * caller: spdk_thread_destroy(), spdk_thread_lib_fini(), spdk_thread_create() 의 실패 경로.
 */
static void
_free_thread(struct spdk_thread *thread)
{
	struct spdk_io_channel *ch;     /* [한국어] 잔여 io_channel 경고 출력용 변수. */
	struct spdk_msg *msg;           /* [한국어] msg_cache 비우기 루프 변수. */
	struct spdk_poller *poller, *ptmp; /* [한국어] poller 순회 + 다음 노드 안전 보관 (FOREACH_SAFE). */

	/* [한국어] 잔여 io_channel 검사 — 정상 종료라면 모두 spdk_put_io_channel 으로 해제됐어야 함.
	 * 남아있다면 사용자 모듈이 release 를 빠뜨린 것. ERRLOG 로 알림 (free 는 하지 않음 — leak 의도적 노출). */
	RB_FOREACH(ch, io_channel_tree, &thread->io_channels) {
		SPDK_ERRLOG("thread %s still has channel for io_device %s\n",
			    thread->name, ch->dev->name);
	}

	/* [한국어] active_pollers 순회·해제. UNREGISTERED 상태가 아니면 leak 가능성 경고.
	 * SAFE 변형 — 루프 안에서 TAILQ_REMOVE 해도 다음 노드 ptmp 가 보존됨. */
	TAILQ_FOREACH_SAFE(poller, &thread->active_pollers, tailq, ptmp) {
		if (poller->state != SPDK_POLLER_STATE_UNREGISTERED) {
			SPDK_WARNLOG("active_poller %s still registered at thread exit\n",
				     poller->name);
		}
		TAILQ_REMOVE(&thread->active_pollers, poller, tailq);  /* [한국어] 큐에서 제거. */
		free(poller);                                          /* [한국어] poller 자체 free (이름 + state 메타데이터). */
	}

	/* [한국어] timed_pollers RB tree 순회·해제. */
	RB_FOREACH_SAFE(poller, timed_pollers_tree, &thread->timed_pollers, ptmp) {
		if (poller->state != SPDK_POLLER_STATE_UNREGISTERED) {
			SPDK_WARNLOG("timed_poller %s still registered at thread exit\n",
				     poller->name);
		}
		RB_REMOVE(timed_pollers_tree, &thread->timed_pollers, poller);  /* [한국어] RB tree 에서 제거. */
		free(poller);
	}

	/* [한국어] paused_pollers 도 동일하게 정리 — 일시정지 상태에서 thread 가 종료되면 모두 leak 으로 간주, 경고. */
	TAILQ_FOREACH_SAFE(poller, &thread->paused_pollers, tailq, ptmp) {
		SPDK_WARNLOG("paused_poller %s still registered at thread exit\n", poller->name);
		TAILQ_REMOVE(&thread->paused_pollers, poller, tailq);
		free(poller);
	}

	/* [한국어] 글로벌 thread 리스트에서 제거 — devlist_mutex 보호. */
	pthread_mutex_lock(&g_devlist_mutex);
	assert(g_thread_count > 0);            /* [한국어] 카운트 0 미만 방지 — 내부 일관성 검사. */
	g_thread_count--;                      /* [한국어] 글로벌 카운터 감소. */
	TAILQ_REMOVE(&g_threads, thread, tailq); /* [한국어] TAILQ 에서 제거. */
	pthread_mutex_unlock(&g_devlist_mutex);

	/* [한국어] per-thread msg_cache (LIFO SLIST) 비우기 — 각 msg 를 글로벌 mempool 로 반환. */
	msg = SLIST_FIRST(&thread->msg_cache);
	while (msg != NULL) {
		SLIST_REMOVE_HEAD(&thread->msg_cache, link); /* [한국어] head 제거 (O(1)). */

		assert(thread->msg_cache_count > 0);
		thread->msg_cache_count--;
		spdk_mempool_put(g_spdk_msg_mempool, msg);    /* [한국어] mempool 로 반환 (lockless). */

		msg = SLIST_FIRST(&thread->msg_cache);        /* [한국어] 다음 head. */
	}

	assert(thread->msg_cache_count == 0);  /* [한국어] 일관성 검증. */

	/* [한국어] interrupt 모드면 fgrp/msg_fd/eventfd 정리. */
	if (spdk_interrupt_mode_is_enabled()) {
		thread_interrupt_destroy(thread);
	}

	spdk_ring_free(thread->messages);  /* [한국어] DPDK rte_ring 해제. */
	free(thread);                      /* [한국어] posix_memalign 으로 할당된 thread 본체 + ctx 해제. */
}

/*
 * [한국어]
 * spdk_thread_lib_init - thread library 초기화 (legacy 인터페이스).
 *
 * @new_thread_fn: 새 thread 가 생성되었음을 reactor framework 에 알릴 콜백 (NULL 가능).
 * @ctx_sz: 모든 spdk_thread 에 공통으로 부착될 사용자 ctx 크기.
 * @return: 0 성공, -ENOMEM 실패.
 *
 * 동기/배경: 단순한 reactor framework 가 spdk_thread_create 시점에 자기 자료구조에 등록할 수 있도록 콜백 제공.
 *           모던 코드는 spdk_thread_lib_init_ext 사용 권장 (RESCHED 등 다양한 op 지원).
 * 동작 단계: 중복 init 방지 assert → new_thread_fn 저장 → _thread_lib_init 본체 위임.
 * 실행 컨텍스트: 메인 thread (reactor 시작 전 1회).
 */
int
spdk_thread_lib_init(spdk_new_thread_fn new_thread_fn, size_t ctx_sz)
{
	assert(g_new_thread_fn == NULL);  /* [한국어] 중복 init 금지. */
	assert(g_thread_op_fn == NULL);   /* [한국어] _ext 와 동시 사용 금지. */

	if (new_thread_fn == NULL) {
		SPDK_INFOLOG(thread, "new_thread_fn was not specified at spdk_thread_lib_init\n");
	} else {
		g_new_thread_fn = new_thread_fn;  /* [한국어] 콜백 등록. */
	}

	/* [한국어] 기본 mempool 크기로 공통 초기화 위임. */
	return _thread_lib_init(ctx_sz, SPDK_DEFAULT_MSG_MEMPOOL_SIZE);
}

/*
 * [한국어]
 * spdk_thread_lib_init_ext - thread library 확장 초기화.
 *
 * @thread_op_fn: NEW/RESCHED 등 op dispatch 콜백 (framework 가 처리).
 * @thread_op_supported_fn: 특정 op 가 지원되는지 묻는 쿼리 콜백.
 * @ctx_sz: 사용자 ctx 크기.
 * @msg_mempool_sz: 메시지 풀 크기 (in-flight 한계).
 * @return: 0 성공, -EINVAL/-ENOMEM 실패.
 *
 * 동기/배경: lib/event/reactor.c 가 사용하는 모던 인터페이스 — RESCHED 로 thread migration,
 *           NEW 로 사용자 모듈 hook 등. 두 콜백은 함께 NULL 또는 함께 비-NULL 이어야 함.
 * 동작 단계: 검증 → 글로벌 슬롯 저장 → _thread_lib_init.
 */
int
spdk_thread_lib_init_ext(spdk_thread_op_fn thread_op_fn,
			 spdk_thread_op_supported_fn thread_op_supported_fn,
			 size_t ctx_sz, size_t msg_mempool_sz)
{
	assert(g_new_thread_fn == NULL);
	assert(g_thread_op_fn == NULL);
	assert(g_thread_op_supported_fn == NULL);

	/* [한국어] op fn 과 supported fn 의 짝을 강제 — 한쪽만 있으면 inconsistent. */
	if ((thread_op_fn != NULL) != (thread_op_supported_fn != NULL)) {
		SPDK_ERRLOG("Both must be defined or undefined together.\n");
		return -EINVAL;
	}

	if (thread_op_fn == NULL && thread_op_supported_fn == NULL) {
		SPDK_INFOLOG(thread, "thread_op_fn and thread_op_supported_fn were not specified\n");
	} else {
		g_thread_op_fn = thread_op_fn;                /* [한국어] op dispatch 콜백 등록. */
		g_thread_op_supported_fn = thread_op_supported_fn; /* [한국어] capability 질의 콜백 등록. */
	}

	return _thread_lib_init(ctx_sz, msg_mempool_sz);
}

/*
 * [한국어]
 * spdk_thread_lib_fini - thread library 종료, 자원 해제.
 *
 * 동기/배경: 모든 reactor 종료 후 메인 thread 가 1회 호출. 등록된 io_device 가 남아있으면 ERRLOG (사용자 모듈 leak).
 *           app_thread 는 일반 spdk_thread_destroy 시점에 free 보류했으므로 여기서 마지막으로 해제.
 * 동작 단계: 잔여 io_device 경고 → 글로벌 콜백 슬롯 NULL → app_thread free → mempool free.
 * 실행 컨텍스트: 메인 thread.
 */
void
spdk_thread_lib_fini(void)
{
	struct io_device *dev;

	/* [한국어] g_io_devices 에 남아있는 디바이스 — 사용자가 spdk_io_device_unregister 빠뜨림. */
	RB_FOREACH(dev, io_device_tree, &g_io_devices) {
		SPDK_ERRLOG("io_device %s not unregistered\n", dev->name);
	}

	g_new_thread_fn = NULL;             /* [한국어] 글로벌 콜백 슬롯 비우기 — 재 init 가능 상태. */
	g_thread_op_fn = NULL;
	g_thread_op_supported_fn = NULL;
	g_ctx_sz = 0;
	if (g_app_thread != NULL) {
		_free_thread(g_app_thread);    /* [한국어] 보류해두었던 app_thread 마지막 해제. */
		g_app_thread = NULL;
	}

	if (g_spdk_msg_mempool) {
		spdk_mempool_free(g_spdk_msg_mempool);  /* [한국어] DPDK rte_mempool 해제. */
		g_spdk_msg_mempool = NULL;
	}
}

/*
 * [한국어]
 * spdk_thread_create - 새 spdk_thread 객체 생성.
 *
 * @name: thread 이름 (NULL 가능 — 포인터 hex 사용).
 * @cpumask: 실행 가능 CPU 비트맵 (NULL 가능 — 모든 코어 허용).
 * @return: 생성된 spdk_thread 또는 NULL (mempool/ring 할당 실패).
 *
 * 동기/배경: reactor framework 가 코어별로 1번 호출하여 그 코어를 hosting 할 thread 객체를 만든다.
 *           **이 함수는 thread 의 "생성"만 담당** — 실제 실행은 다른 코어의 reactor pthread 가
 *           spdk_set_thread() + spdk_thread_poll() 로 진행. 따라서 cache line 정렬을 강제하여
 *           다른 코어와의 false sharing 회피.
 * 동작 단계:
 *   (1) ctx 포함 크기 계산 후 cache line aligned posix_memalign (다른 코어 사용 가정).
 *   (2) cpumask, 컬렉션(io_channels/active/timed/paused/msg_cache) 초기화.
 *   (3) tsc_last, next_poller_id 초기화.
 *   (4) message ring 생성 (MPSC, 64K slots) — 실패 시 free 후 NULL.
 *   (5) g_spdk_msg_mempool 에서 SPDK_MSG_MEMPOOL_CACHE_SIZE 만큼 bulk get → msg_cache 채움 (실패 OK).
 *   (6) name 복사, trace_id 등록.
 *   (7) g_devlist_mutex 안에서 g_thread_id 발급 + g_threads insert + g_thread_count++.
 *   (8) interrupt 모드면 fgrp 생성 (in_interrupt=true 로 시작).
 *   (9) g_new_thread_fn / g_thread_op_fn(NEW) 호출 — framework hook.
 *   (10) state=RUNNING. 첫 thread 면 g_app_thread 에 atomic CAS.
 * 실행 컨텍스트: 보통 메인 또는 framework 의 init thread. cross-core 자료 준비.
 * caller: lib/event/reactor.c 의 코어 spawn 시점, 사용자 직접 호출 가능.
 */
struct spdk_thread *
spdk_thread_create(const char *name, const struct spdk_cpuset *cpumask)
{
	struct spdk_thread *thread, *null_thread;
	/* [한국어] 헤더(sizeof spdk_thread) + 사용자 ctx_sz 를 cache line 크기(64B) 로 정렬 — false sharing 방지. */
	size_t size = SPDK_ALIGN_CEIL(sizeof(*thread) + g_ctx_sz, SPDK_CACHE_LINE_SIZE);
	struct spdk_msg *msgs[SPDK_MSG_MEMPOOL_CACHE_SIZE]; /* [한국어] mempool bulk get 임시 버퍼. */
	int rc = 0, i;

	/* [한국어] cache line aligned 할당 — 이 객체를 사용할 다른 코어가 인접 데이터로 false sharing 안 겪도록.
	 * posix_memalign 은 alignment 8 의 배수 강제, SPDK_CACHE_LINE_SIZE=64. */
	/* Since this spdk_thread object will be used by another core, ensure that it won't share a
	 * cache line with any other object allocated on this core */
	rc = posix_memalign((void **)&thread, SPDK_CACHE_LINE_SIZE, size);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to allocate memory for thread\n");
		return NULL;
	}
	memset(thread, 0, size);  /* [한국어] 모든 필드 0 초기화 — bool/포인터/카운터 명시적 zero. */

	if (cpumask) {
		spdk_cpuset_copy(&thread->cpumask, cpumask); /* [한국어] 사용자 지정 cpumask 복사. */
	} else {
		spdk_cpuset_negate(&thread->cpumask);        /* [한국어] 미지정 → 모든 코어 허용 (~0). */
	}

	RB_INIT(&thread->io_channels);     /* [한국어] io_channel RB tree (key=ch->dev). */
	TAILQ_INIT(&thread->active_pollers); /* [한국어] period=0 raw busy poller TAILQ. */
	RB_INIT(&thread->timed_pollers);   /* [한국어] period>0 timed poller RB tree (key=next_run_tick). */
	TAILQ_INIT(&thread->paused_pollers); /* [한국어] 일시정지된 poller TAILQ. */
	SLIST_INIT(&thread->msg_cache);    /* [한국어] msg LIFO cache (lockless, owner-only). */
	thread->msg_cache_count = 0;

	thread->tsc_last = spdk_get_ticks(); /* [한국어] 첫 spdk_thread_poll 의 idle 측정 시작점. */

	/* [한국어] poller ID 1 부터 시작. UINT64_MAX wrap 시 next_poller_id=1 로 재시작 + 경고. */
	/* Monotonic increasing ID is set to each created poller beginning at 1. Once the
	 * ID exceeds UINT64_MAX a warning message is logged
	 */
	thread->next_poller_id = 1;

	/* [한국어] message ring 생성 — Multi-Producer Single-Consumer, 64K slots. 다른 모든 thread 가
	 * producer (send_msg), 본 thread 만 consumer (poll). DPDK rte_ring 위임 — lockless CAS. */
	thread->messages = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 65536, SPDK_ENV_NUMA_ID_ANY);
	if (!thread->messages) {
		SPDK_ERRLOG("Unable to allocate memory for message ring\n");
		free(thread);
		return NULL;
	}

	/* [한국어] 글로벌 mempool 에서 bulk 로 SPDK_MSG_MEMPOOL_CACHE_SIZE 개 미리 가져와 per-thread cache 채움. */
	/* Fill the local message pool cache. */
	rc = spdk_mempool_get_bulk(g_spdk_msg_mempool, (void **)msgs, SPDK_MSG_MEMPOOL_CACHE_SIZE);
	if (rc == 0) {
		/* [한국어] cache 채우기에 성공 — SLIST_INSERT_HEAD 로 LIFO 순서로 push.
		 * 실패해도 OK — send_msg 시 mempool 직접 사용하면서 organically 채워짐. */
		/* If we can't populate the cache it's ok. The cache will get filled
		 * up organically as messages are passed to the thread. */
		for (i = 0; i < SPDK_MSG_MEMPOOL_CACHE_SIZE; i++) {
			SLIST_INSERT_HEAD(&thread->msg_cache, msgs[i], link);
			thread->msg_cache_count++;
		}
	}

	/* [한국어] 이름 설정 — NULL 이면 thread 포인터 hex (디버그용). */
	if (name) {
		snprintf(thread->name, sizeof(thread->name), "%s", name);
	} else {
		snprintf(thread->name, sizeof(thread->name), "%p", thread);
	}

	/* [한국어] SPDK trace 시스템에 thread owner 로 등록 — 이후 trace_record 시 owner_id. */
	thread->trace_id = spdk_trace_register_owner(OWNER_TYPE_THREAD, thread->name);

	/* [한국어] 글로벌 자료구조 보호 — id 발급 + g_threads 등록은 mutex 안에서 atomic 처리. */
	pthread_mutex_lock(&g_devlist_mutex);
	if (g_thread_id == 0) {
		/* [한국어] UINT64_MAX 오버플로 후 wrap. 더 이상 unique id 보장 불가 — fatal. */
		SPDK_ERRLOG("Thread ID rolled over. Further thread creation is not allowed.\n");
		pthread_mutex_unlock(&g_devlist_mutex);
		_free_thread(thread);
		return NULL;
	}
	thread->id = g_thread_id++;            /* [한국어] post-increment — 첫 thread id=1. */
	TAILQ_INSERT_TAIL(&g_threads, thread, tailq); /* [한국어] 글로벌 리스트 끝에 추가. */
	g_thread_count++;                      /* [한국어] 캐시된 카운터 증가. */
	pthread_mutex_unlock(&g_devlist_mutex);

	SPDK_DEBUGLOG(thread, "Allocating new thread (%" PRIu64 ", %s)\n",
		      thread->id, thread->name);

	/* [한국어] interrupt 모드는 처음부터 in_interrupt=true 로 시작 (msg_fd, fgrp 사용 강제). */
	if (spdk_interrupt_mode_is_enabled()) {
		thread->in_interrupt = true;
		rc = thread_interrupt_create(thread);
		if (rc != 0) {
			_free_thread(thread);
			return NULL;
		}
	}

	/* [한국어] framework 의 새 thread hook 호출. legacy(g_new_thread_fn) 또는 모던(g_thread_op_fn) 중 하나. */
	if (g_new_thread_fn) {
		rc = g_new_thread_fn(thread);
	} else if (g_thread_op_supported_fn && g_thread_op_supported_fn(SPDK_THREAD_OP_NEW)) {
		rc = g_thread_op_fn(thread, SPDK_THREAD_OP_NEW);
	}

	if (rc != 0) {
		/* [한국어] framework 가 거부 (rsc 부족 등) — 위 자원 모두 해제 후 NULL. */
		_free_thread(thread);
		return NULL;
	}

	thread->state = SPDK_THREAD_STATE_RUNNING; /* [한국어] 정상 동작 상태로 전환 — poll 가능. */

	/* [한국어] 가장 처음 생성된 thread 를 g_app_thread 로 등록. CAS 로 race 방지 (멀티 코어가 거의 동시에
	 * 생성하는 시나리오). null_thread=NULL 이 expected → g_app_thread 가 NULL 일 때만 thread 로 set. */
	/* If this is the first thread, save it as the app thread.  Use an atomic
	 * compare + exchange to guard against crazy users who might try to
	 * call spdk_thread_create() simultaneously on multiple threads.
	 */
	null_thread = NULL;
	__atomic_compare_exchange_n(&g_app_thread, &null_thread, thread, false,
				    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);

	return thread;
}

/*
 * [한국어]
 * spdk_thread_get_app_thread - 어플리케이션 메인 thread 반환.
 *
 * @return: 가장 처음 spdk_thread_create() 된 thread 또는 NULL (아직 생성 전).
 *
 * RPC, init/fini 콜백 등이 메인 thread 컨텍스트로 진행할 때 사용.
 */
struct spdk_thread *
spdk_thread_get_app_thread(void)
{
	return g_app_thread; /* [한국어] atomic load 불필요 — 첫 등록 후 read-only. */
}

/*
 * [한국어]
 * spdk_thread_is_app_thread - 주어진 thread (또는 현재 thread) 가 app thread 인지 확인.
 *
 * @thread: 확인할 thread (NULL 이면 현재 thread 사용).
 * @return: true/false.
 */
bool
spdk_thread_is_app_thread(struct spdk_thread *thread)
{
	if (thread == NULL) {
		thread = _get_thread();  /* [한국어] NULL 입력은 "현재 thread" 의미. */
	}

	return g_app_thread == thread;
}

/*
 * [한국어]
 * spdk_thread_bind - thread 를 현재 CPU 코어에 고정/해제.
 *
 * @thread: 대상 thread.
 * @bind: true=현재 코어에 핀. scheduler 가 reschedule 시도하지 않음.
 *
 * 사용처: NVMe-oF target 에서 polling thread 를 특정 코어에 고정하여 latency 일관성 보장.
 */
void
spdk_thread_bind(struct spdk_thread *thread, bool bind)
{
	thread->is_bound = bind;  /* [한국어] 단순 bool 저장. scheduler 가 매 라운드 검사. */
}

/*
 * [한국어]
 * spdk_thread_is_bound - thread 의 bind 상태 조회.
 */
bool
spdk_thread_is_bound(struct spdk_thread *thread)
{
	return thread->is_bound;
}

/*
 * [한국어]
 * spdk_set_thread - 현재 pthread 의 TLS thread 포인터 설정.
 *
 * @thread: 이 pthread 가 hosting 할 spdk_thread (NULL 가능 — TLS 클리어).
 *
 * 동기/배경: 한 pthread 가 여러 spdk_thread 를 sequentially host 할 수 있도록 reactor 가 매 라운드
 *           swap. spdk_thread_poll() 도 진입/탈출 시 자동 swap.
 * 실행 컨텍스트: reactor 또는 일반 pthread (cross-thread 진입 시점).
 */
void
spdk_set_thread(struct spdk_thread *thread)
{
	tls_thread = thread; /* [한국어] __thread 변수 단순 대입 (atomic 불필요 — TLS 는 pthread 자기 자신). */
}

/*
 * [한국어]
 * thread_exit - 매 라운드 호출되어 EXITING → EXITED 전환 가능 여부 검사.
 *
 * @thread: 종료 진행 중인 thread (state == EXITING).
 * @now: 현재 TSC.
 *
 * 동기/배경: spdk_thread_exit() 가 호출되면 즉시 EXITED 가 아니라 EXITING 으로 표시되고, 매
 *           spdk_thread_poll() 라운드에서 이 함수가 호출되어 모든 pending(메시지/poller/channel/
 *           for_each/unregister) 이 정리되었는지 확인. 모두 비면 EXITED. timeout 시 강제 EXITED.
 * 동작 단계: deadline 검사 → message ring → for_each → active/timed/paused poller → io_channel →
 *          pending unregister 순으로 검사. 하나라도 있으면 INFOLOG 후 return.
 * 실행 컨텍스트: thread_poll 안에서 호출되므로 thread 자신.
 * caller: thread_poll(), _thread_exit().
 */
static void
thread_exit(struct spdk_thread *thread, uint64_t now)
{
	struct spdk_poller *poller;
	struct spdk_io_channel *ch;

	/* [한국어] deadline 도달 — 강제 EXITED 전환 (어떤 자원이 영원히 정리 안 되는 버그 방어). */
	if (now >= thread->exit_timeout_tsc) {
		SPDK_ERRLOG("thread %s got timeout, and move it to the exited state forcefully\n",
			    thread->name);
		goto exited;
	}

	/* [한국어] 메시지 ring 비어있어야 함 — 다른 thread 가 이미 보낸 메시지가 처리되어야. */
	if (spdk_ring_count(thread->messages) > 0) {
		SPDK_INFOLOG(thread, "thread %s still has messages\n", thread->name);
		return;
	}

	/* [한국어] 진행 중인 for_each 가 없어야 함 — orig_thread 가 in-flight 면 free 시 dangling. */
	if (thread->for_each_count > 0) {
		SPDK_INFOLOG(thread, "thread %s is still executing %u for_each_channels/threads\n",
			     thread->name, thread->for_each_count);
		return;
	}

	/* [한국어] active_pollers 중 UNREGISTERED 이외가 있으면 종료 불가. */
	TAILQ_FOREACH(poller, &thread->active_pollers, tailq) {
		if (poller->state != SPDK_POLLER_STATE_UNREGISTERED) {
			SPDK_INFOLOG(thread,
				     "thread %s still has active poller %s\n",
				     thread->name, poller->name);
			return;
		}
	}

	/* [한국어] timed_pollers 도 동일 검사. */
	RB_FOREACH(poller, timed_pollers_tree, &thread->timed_pollers) {
		if (poller->state != SPDK_POLLER_STATE_UNREGISTERED) {
			SPDK_INFOLOG(thread,
				     "thread %s still has active timed poller %s\n",
				     thread->name, poller->name);
			return;
		}
	}

	/* [한국어] paused_pollers 가 하나라도 있으면 종료 불가 — 사용자가 unregister 잊음. */
	TAILQ_FOREACH(poller, &thread->paused_pollers, tailq) {
		SPDK_INFOLOG(thread,
			     "thread %s still has paused poller %s\n",
			     thread->name, poller->name);
		return;
	}

	/* [한국어] io_channel 이 남아있으면 종료 불가 — 사용자 모듈이 put 하지 않음. */
	RB_FOREACH(ch, io_channel_tree, &thread->io_channels) {
		SPDK_INFOLOG(thread,
			     "thread %s still has channel for io_device %s\n",
			     thread->name, ch->dev->name);
		return;
	}

	/* [한국어] 보류된 io_device unregister 콜백이 모두 실행되어야 함. */
	if (thread->pending_unregister_count > 0) {
		SPDK_INFOLOG(thread,
			     "thread %s is still unregistering io_devices\n",
			     thread->name);
		return;
	}

exited:
	thread->state = SPDK_THREAD_STATE_EXITED;  /* [한국어] EXITED — destroy 가능 상태. */
	/* [한국어] interrupt 모드면 framework 에 RESCHED op 호출 — fgrp 에서 빠지도록 유도. */
	if (spdk_unlikely(thread->in_interrupt)) {
		g_thread_op_fn(thread, SPDK_THREAD_OP_RESCHED);
	}
}

/* [한국어] 전방선언 — interrupt 모드에서 _thread_exit 가 self-send_msg 로 재시도. */
static void _thread_exit(void *ctx);

/*
 * [한국어]
 * spdk_thread_exit - thread 종료 요청 (비동기).
 *
 * @thread: 종료할 thread (반드시 호출자=tls_thread 와 같아야 함).
 * @return: 0 (이미 종료중이어도 OK).
 *
 * 동기/배경: thread 를 즉시 destroy 할 수 없는 이유 — 잔여 poller/channel/메시지 정리 시간 필요.
 *           이 함수는 EXITING 으로 마킹만 하고, 실제 EXITED 전이는 매 spdk_thread_poll 라운드의
 *           thread_exit() 에서 검사·진행. interrupt 모드는 send_msg 로 wakeup.
 * 동작 단계: assert(tls_thread==thread) → 이미 EXITING 이상이면 noop → exit_timeout 5초 deadline →
 *          state=EXITING → interrupt 모드면 _thread_exit self-msg.
 */
int
spdk_thread_exit(struct spdk_thread *thread)
{
	SPDK_DEBUGLOG(thread, "Exit thread %s\n", thread->name);

	assert(tls_thread == thread); /* [한국어] 본인이 본인을 종료해야 함 (cross-thread 종료 금지). */

	if (thread->state >= SPDK_THREAD_STATE_EXITING) {
		/* [한국어] 이미 종료 진행 중 — 멱등성. */
		SPDK_INFOLOG(thread,
			     "thread %s is already exiting\n",
			     thread->name);
		return 0;
	}

	/* [한국어] 5초 deadline 설정 — 이 시간 내에 정리 안 되면 강제 EXITED. */
	thread->exit_timeout_tsc = spdk_get_ticks() + (spdk_get_ticks_hz() *
				   SPDK_THREAD_EXIT_TIMEOUT_SEC);
	thread->state = SPDK_THREAD_STATE_EXITING;

	/* [한국어] interrupt 모드는 polling 루프가 메시지 wakeup 에 의존 — self-msg 로 다음 라운드 깨움. */
	if (spdk_interrupt_mode_is_enabled()) {
		spdk_thread_send_msg(thread, _thread_exit, thread);
	}

	return 0;
}

/*
 * [한국어]
 * spdk_thread_is_running - thread 가 RUNNING 상태인지.
 */
bool
spdk_thread_is_running(struct spdk_thread *thread)
{
	return thread->state == SPDK_THREAD_STATE_RUNNING;
}

/*
 * [한국어]
 * spdk_thread_is_exited - thread 가 EXITED 상태인지 (destroy 가능).
 */
bool
spdk_thread_is_exited(struct spdk_thread *thread)
{
	return thread->state == SPDK_THREAD_STATE_EXITED;
}

/*
 * [한국어]
 * spdk_thread_destroy - EXITED 상태 thread 의 자원 해제.
 *
 * @thread: 해제할 thread (반드시 EXITED 상태).
 *
 * 동기/배경: spdk_thread_exit + thread_exit 로 EXITED 가 된 후, framework 가 호출하여 _free_thread.
 *           단, app_thread 는 여기서 free 하지 않고 spdk_thread_lib_fini 까지 보존 (lifetime 안전성).
 */
void
spdk_thread_destroy(struct spdk_thread *thread)
{
	assert(thread != NULL);
	SPDK_DEBUGLOG(thread, "Destroy thread %s\n", thread->name);

	assert(thread->state == SPDK_THREAD_STATE_EXITED); /* [한국어] EXITED 가 아니면 invariant 위반. */

	if (tls_thread == thread) {
		tls_thread = NULL; /* [한국어] 자기 자신을 destroy 시 TLS 클리어 — 이후 _get_thread NULL. */
	}

	/* [한국어] app_thread 는 lib_fini 까지 free 보류 — RPC 콜백이 app_thread 포인터 비교에 사용. */
	/* To be safe, do not free the app thread until spdk_thread_lib_fini(). */
	if (thread != g_app_thread) {
		_free_thread(thread);
	}
}

/*
 * [한국어]
 * spdk_thread_get_ctx - thread 의 사용자 ctx 영역 포인터 반환.
 *
 * @thread: 대상 thread.
 * @return: ctx 시작 주소 (g_ctx_sz>0 일 때) 또는 NULL.
 *
 * spdk_thread_lib_init(ctx_sz) 가 0 보다 큰 경우, 각 thread 끝에 ctx[0] 가변 영역이 부착되어 있음.
 */
void *
spdk_thread_get_ctx(struct spdk_thread *thread)
{
	if (g_ctx_sz > 0) {
		return thread->ctx;  /* [한국어] flexible array member 시작 주소. */
	}

	return NULL;             /* [한국어] ctx_sz=0 이면 ctx 영역이 할당되지 않음. */
}

/*
 * [한국어]
 * spdk_thread_get_cpumask - thread 의 cpumask 포인터 반환.
 */
struct spdk_cpuset *
spdk_thread_get_cpumask(struct spdk_thread *thread)
{
	return &thread->cpumask;
}

/*
 * [한국어]
 * spdk_thread_set_cpumask - 현재 thread 의 cpumask 변경하고 reschedule 트리거.
 *
 * @cpumask: 새 cpumask.
 * @return: 0 성공, -ENOTSUP (framework 미지원), -EINVAL (비-SPDK thread).
 *
 * 동기/배경: scheduler 가 thread migration 결정 — 이 thread 가 어느 코어에서 실행될 수 있는지 변경.
 *           실제 이동은 framework 의 RESCHED op 가 비동기로 처리.
 * 실행 컨텍스트: 자기 자신 thread 컨텍스트.
 */
int
spdk_thread_set_cpumask(struct spdk_cpuset *cpumask)
{
	struct spdk_thread *thread;

	/* [한국어] framework 가 RESCHED op 를 지원해야 함 — 미지원이면 cpumask 만 바꿀 수 없음. */
	if (!g_thread_op_supported_fn || !g_thread_op_supported_fn(SPDK_THREAD_OP_RESCHED)) {
		SPDK_ERRLOG("Framework does not support reschedule operation.\n");
		assert(false);
		return -ENOTSUP;
	}

	thread = spdk_get_thread();
	if (!thread) {
		SPDK_ERRLOG("Called from non-SPDK thread\n");
		assert(false);
		return -EINVAL;
	}

	spdk_cpuset_copy(&thread->cpumask, cpumask);  /* [한국어] cpumask 즉시 복사. */

	/* [한국어] framework 에 reschedule 요청. 같은 라운드에서 여러 번 호출되면 마지막 cpumask 적용. */
	/* Invoke framework's reschedule operation. If this function is called multiple times
	 * in a single spdk_thread_poll() context, the last cpumask will be used in the
	 * reschedule operation.
	 */
	g_thread_op_fn(thread, SPDK_THREAD_OP_RESCHED);

	return 0;
}

/*
 * [한국어]
 * spdk_thread_get_from_ctx - 사용자 ctx 포인터로부터 spdk_thread 복원.
 *
 * @ctx: spdk_thread_get_ctx() 가 반환했던 포인터.
 * @return: 해당 spdk_thread.
 *
 * SPDK_CONTAINEROF 매크로로 ctx 시작 - offsetof(spdk_thread, ctx) 계산. 콜백이 ctx 만 받았을 때 thread 복원에 사용.
 */
struct spdk_thread *
spdk_thread_get_from_ctx(void *ctx)
{
	if (ctx == NULL) {
		assert(false);
		return NULL;
	}

	assert(g_ctx_sz > 0); /* [한국어] ctx_sz=0 이면 이 함수 호출 자체가 invariant 위반. */

	return SPDK_CONTAINEROF(ctx, struct spdk_thread, ctx); /* [한국어] container_of 패턴. */
}

/*
 * [한국어]
 * msg_queue_run_batch - 메시지 ring 에서 batch 단위로 dequeue 후 fn(arg) 일괄 실행 (hot path).
 *
 * @thread: 대상 thread (자신).
 * @max_msgs: 한 라운드 처리 메시지 상한 (0 이면 SPDK_MSG_BATCH_SIZE=8 적용).
 * @return: 실제 처리한 메시지 수.
 *
 * 동기/배경: spdk_thread_poll 의 hot path. lockless rte_ring 에서 한 번에 8 개를 가져와 cache locality
 *           최대화. interrupt 모드일 때는 dequeue 후에도 ring 에 남은 메시지가 있으면 self-wakeup
 *           write(eventfd) 로 다음 epoll 라운드에 다시 깨어남 (level-trigger 보장).
 * 동작 단계:
 *   (1) DEBUG 빌드 — messages 배열 NULL 초기화 (정적 분석기 false positive 방지).
 *   (2) max_msgs 캡 8 적용.
 *   (3) spdk_ring_dequeue 로 batch get (1 atomic CAS).
 *   (4) interrupt 모드 + ring 에 더 있으면 write(msg_fd) — 즉시 다음 wakeup.
 *   (5) 각 msg->fn(arg) 호출 + DTRACE_PROBE + spinlock holding 검사.
 *   (6) 처리 완료 msg 는 per-thread cache 에 LIFO push (재사용); cache 가득찬 msg 는 mempool 반환.
 * Hot path 노트: 한 라운드에 1 ring 액세스 + 최대 8 fn 호출 + per-msg 1 SLIST/mempool 반환. 모두 lockless.
 */
static inline uint32_t
msg_queue_run_batch(struct spdk_thread *thread, uint32_t max_msgs)
{
	unsigned count, i;
	void *messages[SPDK_MSG_BATCH_SIZE]; /* [한국어] 8 개 포인터 = 64B (cache line). */
	uint64_t notify = 1;                 /* [한국어] eventfd 에 쓸 값 — 1 만 증가 (counter 모드). */
	int rc;

#ifdef DEBUG
	/* [한국어] DEBUG 빌드만 — dequeue 가 못 채운 슬롯에 대해 정적 분석기가 uninit 경고하는 걸 회피.
	 * production 에서는 dequeue 가 정확히 count 만큼 채우므로 사용 안 됨. */
	/*
	 * spdk_ring_dequeue() fills messages and returns how many entries it wrote,
	 * so we will never actually read uninitialized data from events, but just to be sure
	 * (and to silence a static analyzer false positive), initialize the array to NULL pointers.
	 */
	memset(messages, 0, sizeof(messages));
#endif

	/* [한국어] max_msgs 0 은 "기본값" 의미 — SPDK_MSG_BATCH_SIZE=8 적용. */
	if (max_msgs > 0) {
		max_msgs = spdk_min(max_msgs, SPDK_MSG_BATCH_SIZE); /* [한국어] 8 초과는 안전상 제한. */
	} else {
		max_msgs = SPDK_MSG_BATCH_SIZE;
	}

	/* [한국어] DPDK rte_ring lockless dequeue (MC 모드). 단일 consumer 이므로 SC 변형. */
	count = spdk_ring_dequeue(thread->messages, messages, max_msgs);
	/* [한국어] interrupt 모드에서 한 번 dequeue 후에도 잔여가 있으면 epoll 이 다시 wakeup 되도록 self-write.
	 * eventfd 는 level-trigger — 0 이 아닌 동안 계속 readable. dequeue 가 read 와 별개라 명시 알림 필요. */
	if (spdk_unlikely(thread->in_interrupt) &&
	    spdk_ring_count(thread->messages) != 0) {
		rc = write(thread->msg_fd, &notify, sizeof(notify));
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify msg_queue: %s.\n", spdk_strerror(errno));
		}
	}
	if (count == 0) {
		return 0; /* [한국어] 빈 ring — 빠르게 반환 (가장 빈번한 idle case). */
	}

	/* [한국어] dequeue 한 메시지들을 차례로 실행. */
	for (i = 0; i < count; i++) {
		struct spdk_msg *msg = messages[i];

		assert(msg != NULL); /* [한국어] dequeue 가 채운 영역은 항상 비-NULL. */

		SPDK_DTRACE_PROBE2(msg_exec, msg->fn, msg->arg); /* [한국어] DTrace probe — perf 분석용. */

		msg->fn(msg->arg); /* [한국어] 실제 사용자 콜백 실행. 이 안에서 다른 send_msg/poller 등록 가능. */

		/* [한국어] 콜백이 spinlock 을 잡은 채로 빠져나오면 fatal — 다음 thread 가 같은 락 시도 시 데드락. */
		SPIN_ASSERT(thread->lock_count == 0, SPIN_ERR_HOLD_DURING_SWITCH);

		/* [한국어] msg 객체 재사용 — per-thread LIFO cache. cache 안 차면 head 에 push (LIFO=hot reuse). */
		if (thread->msg_cache_count < SPDK_MSG_MEMPOOL_CACHE_SIZE) {
			/* [한국어] 가장 최근 처리한 msg 는 곧 다시 send_msg 에서 꺼내서 hot — LIFO 가 cache 친화적. */
			/* Insert the messages at the head. We want to re-use the hot
			 * ones. */
			SLIST_INSERT_HEAD(&thread->msg_cache, msg, link);
			thread->msg_cache_count++;
		} else {
			spdk_mempool_put(g_spdk_msg_mempool, msg); /* [한국어] cache 포화 — 글로벌 mempool 반환. */
		}
	}

	return count;
}

/*
 * [한국어]
 * poller_insert_timer - timed poller 를 RB tree 에 삽입하고 first_timed_poller 캐시 갱신.
 *
 * @thread: 대상 thread.
 * @poller: 삽입할 poller (period_ticks > 0).
 * @now: 현재 TSC — next_run_tick 계산 기준.
 *
 * 동기/배경: timed poller 는 next_run_tick 기준으로 정렬되어 thread_poll 이 RB_MIN/cache 만 보고 만기 검사 가능.
 *           timed_poller_compare 가 동일 키에 대해 +1 반환 → 우측 삽입 → first 캐시 비교 시 '< 만' 검사로 충분.
 * 동작 단계: next_run_tick 계산 → RB_INSERT → cache 갱신 (비어있거나 새 것이 더 빠르면).
 * 호출 빈도: poller register 1회, timed poller 실행 후 재삽입 매 라운드 → hot path.
 */
static void
poller_insert_timer(struct spdk_thread *thread, struct spdk_poller *poller, uint64_t now)
{
	struct spdk_poller *tmp __attribute__((unused)); /* [한국어] release 빌드에서 미사용 경고 억제. */

	poller->next_run_tick = now + poller->period_ticks; /* [한국어] 다음 만기 = 현재 + period. */

	/* [한국어] timed_pollers RB tree 에 next_run_tick 키로 삽입. timed_poller_compare 가 동일 키에 +1
	 * 반환하므로 RB_INSERT 는 항상 NULL 리턴 (duplicate fail 없음). */
	/*
	 * Insert poller in the thread's timed_pollers tree by next scheduled run time
	 * as its key.
	 */
	tmp = RB_INSERT(timed_pollers_tree, &thread->timed_pollers, poller);
	assert(tmp == NULL); /* [한국어] 동일 노드 중복 삽입은 invariant 위반. */

	/* [한국어] first_timed_poller 캐시 갱신 — 만기 검사 hot path 의 O(1) 를 위해.
	 * 동일 next_run_tick 인 새 poller 는 우측 삽입되므로 first 보다 작아질 수 없음. */
	/* Update the cache only if it is empty or the inserted poller is earlier than it.
	 * RB_MIN() is not necessary here because all pollers, which has exactly the same
	 * next_run_tick as the existing poller, are inserted on the right side.
	 */
	if (thread->first_timed_poller == NULL ||
	    poller->next_run_tick < thread->first_timed_poller->next_run_tick) {
		thread->first_timed_poller = poller;
	}
}

/*
 * [한국어]
 * poller_remove_timer - timed poller 를 RB tree 에서 제거 + 캐시 갱신.
 *
 * @thread: 대상 thread.
 * @poller: 제거할 poller.
 *
 * 호출 빈도: poller unregister/pause/interrupt 모드 전환 — non-hot path. cache 가 변경되어야 하면 RB_MIN 호출.
 */
static inline void
poller_remove_timer(struct spdk_thread *thread, struct spdk_poller *poller)
{
	struct spdk_poller *tmp __attribute__((unused));

	tmp = RB_REMOVE(timed_pollers_tree, &thread->timed_pollers, poller); /* [한국어] RB_REMOVE 는 노드 포인터로 제거 — duplicate key OK. */
	assert(tmp != NULL); /* [한국어] 트리에 없는 노드 제거는 invariant 위반. */

	/* [한국어] 캐시 = 제거 대상이었으면 RB_MIN 으로 새 최좌측 찾기. RB_MIN 은 leftmost 따라가는 O(log n). */
	/* This function is not used in any case that is performance critical.
	 * Update the cache simply by RB_MIN() if it needs to be changed.
	 */
	if (thread->first_timed_poller == poller) {
		thread->first_timed_poller = RB_MIN(timed_pollers_tree, &thread->timed_pollers);
	}
}

/*
 * [한국어]
 * thread_insert_poller - poller 를 period 에 따라 active_pollers 또는 timed_pollers 에 삽입.
 *
 * period_ticks=0 → active TAILQ tail, period>0 → timed RB tree (now 기준 만기).
 */
static void
thread_insert_poller(struct spdk_thread *thread, struct spdk_poller *poller)
{
	if (poller->period_ticks) {
		poller_insert_timer(thread, poller, spdk_get_ticks()); /* [한국어] 현재 TSC 기준 첫 만기 계산. */
	} else {
		TAILQ_INSERT_TAIL(&thread->active_pollers, poller, tailq); /* [한국어] active 끝에 추가 — 다음 라운드 reverse 순회. */
	}
}

/*
 * [한국어]
 * thread_update_stats - poller 실행 결과에 따라 busy_tsc / idle_tsc 누적 (hot path).
 *
 * @end, @start: 실행 시작/끝 TSC — 차이가 누적량.
 * @rc: poller fn 의 마지막 반환값 (0=idle, >0=busy, -1=무시).
 *
 * scheduler 가 busy_ratio = busy_tsc / (busy_tsc + idle_tsc) 로 부하 균형 결정.
 */
static inline void
thread_update_stats(struct spdk_thread *thread, uint64_t end,
		    uint64_t start, int rc)
{
	if (rc == 0) {
		/* [한국어] idle 라운드 — 누적. */
		/* Poller status idle */
		thread->stats.idle_tsc += end - start;
	} else if (rc > 0) {
		/* [한국어] busy 라운드 — 누적. */
		/* Poller status busy */
		thread->stats.busy_tsc += end - start;
	}
	/* [한국어] tsc_last 갱신 — 다음 라운드 누적 시작점. */
	/* Store end time to use it as start time of the next spdk_thread_poll(). */
	thread->tsc_last = end;
}

/*
 * [한국어]
 * thread_execute_poller - active_pollers TAILQ 의 한 poller 를 실행 (hot path).
 *
 * @thread: 소유 thread.
 * @poller: 실행할 poller (active_pollers 에 들어있음).
 * @return: poller fn 의 반환값 (0/양수/-1) — busy 판정용.
 *
 * 동기/배경: thread_poll 이 매 라운드 active_pollers 를 reverse 순회하며 호출.
 *           사전 상태 검사 (UNREGISTERED/PAUSING/WAITING), fn 실행, 사후 상태 검사 (state 가 fn 안에서 바뀌었을 수 있음).
 * 동작 단계:
 *   사전: UNREGISTERED→remove+free / PAUSING→active 에서 빼서 paused 로 이동 / WAITING→fn 호출 진행.
 *   본체: state=RUNNING → fn(arg) → spinlock holding 검사 → run_count++ / busy_count++.
 *   사후: state 재검사 — fn 안에서 unregister/pause 호출 가능.
 *         RUNNING 그대로면 WAITING 으로 복귀.
 * 호출 빈도: 매 라운드 N (active poller 수) — 매우 hot.
 */
static inline int
thread_execute_poller(struct spdk_thread *thread, struct spdk_poller *poller)
{
	int rc;

	/* [한국어] 사전 상태 검사 — 다른 poller 의 fn 안에서 이 poller 가 unregister/pause 됐을 수 있음. */
	switch (poller->state) {
	case SPDK_POLLER_STATE_UNREGISTERED:
		/* [한국어] 이전 라운드에 unregister 마킹된 active poller — 이번 라운드에서 안전하게 제거·해제. */
		TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
		free(poller);
		return 0;
	case SPDK_POLLER_STATE_PAUSING:
		/* [한국어] pause 요청됨 — active 큐에서 빼서 paused 큐로 이동. */
		TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
		TAILQ_INSERT_TAIL(&thread->paused_pollers, poller, tailq);
		poller->state = SPDK_POLLER_STATE_PAUSED;
		return 0;
	case SPDK_POLLER_STATE_WAITING:
		break; /* [한국어] 정상 대기 → fn 호출 진행. */
	default:
		assert(false); /* [한국어] RUNNING/PAUSED 가 active 큐에서 발견되면 invariant 위반. */
		break;
	}

	poller->state = SPDK_POLLER_STATE_RUNNING; /* [한국어] fn 실행 마킹 — 재진입 감지용. */
	rc = poller->fn(poller->arg); /* [한국어] 사용자 콜백. 0=idle, >0=busy, -1=disabled (DEBUG only). */

	/* [한국어] fn 안에서 spinlock 잡은 채 빠져나오면 fatal — thread migration 시 데드락 가능. */
	SPIN_ASSERT(thread->lock_count == 0, SPIN_ERR_HOLD_DURING_SWITCH);

	poller->run_count++; /* [한국어] 실행 횟수 누적 (RPC stats). */
	if (rc > 0) {
		poller->busy_count++; /* [한국어] 일을 한 라운드만 별도 누적. */
	}

#ifdef DEBUG
	if (rc == -1) {
		/* [한국어] -1 반환은 디버그 로그용 sentinel — 실제 처리는 0 처럼 idle 취급. */
		SPDK_DEBUGLOG(thread, "Poller %s returned -1\n", poller->name);
	}
#endif

	/* [한국어] fn 안에서 자기 자신 unregister/pause 했을 수 있으므로 사후 처리. */
	switch (poller->state) {
	case SPDK_POLLER_STATE_UNREGISTERED:
		/* [한국어] fn 안에서 unregister — 즉시 제거·해제. */
		TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
		free(poller);
		break;
	case SPDK_POLLER_STATE_PAUSING:
		/* [한국어] fn 안에서 pause — paused 로 이동. */
		TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
		TAILQ_INSERT_TAIL(&thread->paused_pollers, poller, tailq);
		poller->state = SPDK_POLLER_STATE_PAUSED;
		break;
	case SPDK_POLLER_STATE_PAUSED:
	case SPDK_POLLER_STATE_WAITING:
		break; /* [한국어] 이미 다른 곳에서 정리된 상태. */
	case SPDK_POLLER_STATE_RUNNING:
		poller->state = SPDK_POLLER_STATE_WAITING; /* [한국어] 정상 종료 → 대기 상태로. */
		break;
	default:
		assert(false);
		break;
	}

	return rc;
}

/*
 * [한국어]
 * thread_execute_timed_poller - timed poller (RB tree 노드) 를 실행하고 만기 검사 후 재삽입 (hot path).
 *
 * @thread: 소유 thread.
 * @poller: 만기가 도래한 timed poller (호출 전 RB tree 에서 이미 제거됨).
 * @now: 현재 TSC — 다음 만기 계산 기준.
 * @return: poller fn 반환값.
 *
 * 동기/배경: thread_poll 의 timed loop 가 first_timed_poller 부터 만기 도래한 모든 noder 를 RB_REMOVE 후
 *           이 함수에 위임. WAITING 으로 종료된 경우 poller_insert_timer 로 재삽입 (RB tree 안에 중복 노드 없음).
 * 동작 단계: thread_execute_poller 와 유사하나 active TAILQ 가 아닌 RB tree 라서 REMOVE 호출이 없음.
 *          (사전에 thread_poll 이 미리 RB_REMOVE 하고 들어옴.)
 * 호출 빈도: 만기 도래 timed poller 수 — 보통 한 라운드에 0~수 개.
 */
static inline int
thread_execute_timed_poller(struct spdk_thread *thread, struct spdk_poller *poller,
			    uint64_t now)
{
	int rc;

	/* [한국어] 사전 상태 검사. RB tree 에서는 이미 제거됨 — TAILQ_REMOVE 불필요. */
	switch (poller->state) {
	case SPDK_POLLER_STATE_UNREGISTERED:
		free(poller); /* [한국어] 이미 unregister 마킹 — RB 에서도 빠진 상태이므로 바로 free. */
		return 0;
	case SPDK_POLLER_STATE_PAUSING:
		/* [한국어] pause 요청 — paused 큐로 이동. RB tree 에서는 이미 제거되어 있으므로 PAUSED 로 마킹만. */
		TAILQ_INSERT_TAIL(&thread->paused_pollers, poller, tailq);
		poller->state = SPDK_POLLER_STATE_PAUSED;
		return 0;
	case SPDK_POLLER_STATE_WAITING:
		break;
	default:
		assert(false);
		break;
	}

	poller->state = SPDK_POLLER_STATE_RUNNING;
	rc = poller->fn(poller->arg); /* [한국어] timed poller 본체 호출 (예: heartbeat, stats flush). */

	SPIN_ASSERT(thread->lock_count == 0, SPIN_ERR_HOLD_DURING_SWITCH);

	poller->run_count++;
	if (rc > 0) {
		poller->busy_count++;
	}

#ifdef DEBUG
	if (rc == -1) {
		SPDK_DEBUGLOG(thread, "Timed poller %s returned -1\n", poller->name);
	}
#endif

	/* [한국어] fn 종료 후 상태에 따라 처리 분기. */
	switch (poller->state) {
	case SPDK_POLLER_STATE_UNREGISTERED:
		free(poller);  /* [한국어] fn 안에서 unregister — RB tree 에 이미 없으므로 바로 free. */
		break;
	case SPDK_POLLER_STATE_PAUSING:
		TAILQ_INSERT_TAIL(&thread->paused_pollers, poller, tailq);
		poller->state = SPDK_POLLER_STATE_PAUSED;
		break;
	case SPDK_POLLER_STATE_PAUSED:
		break;
	case SPDK_POLLER_STATE_RUNNING:
		poller->state = SPDK_POLLER_STATE_WAITING;
	/* [한국어] fallthrough — RUNNING 종료 후에도 WAITING 케이스의 재삽입 로직 실행. */
	/* fallthrough */
	case SPDK_POLLER_STATE_WAITING:
		poller_insert_timer(thread, poller, now); /* [한국어] now+period 로 재삽입. */
		break;
	default:
		assert(false);
		break;
	}

	return rc;
}

/*
 * [한국어]
 * thread_run_pp_handlers - 한 active poller 실행 직후 등록된 post-poller handler 들을 일괄 호출.
 *
 * @thread: 대상 thread.
 *
 * 동기/배경: 어떤 poller 가 batch 작업을 시작한 뒤 같은 라운드 안에서 마무리해야 할 때 사용 (예: bdev I/O
 *           completion 일괄 처리). active poller 1 개 실행마다 검사하여 num_pp_handlers > 0 이면 호출.
 * 재진입 방지: 호출 시작 시 num_pp_handlers 를 MAX 로 임시 세팅하여 콜백 안에서 추가 등록 차단.
 */
static inline void
thread_run_pp_handlers(struct spdk_thread *thread)
{
	uint8_t i, count = thread->num_pp_handlers; /* [한국어] 현재 등록 수 캡처. */

	/* [한국어] 콜백 실행 중에는 새 등록이 ERRLOG (등록 슬롯 가득찬 것처럼 보이게). 콜백 끝나면 0 으로 리셋. */
	/* Set to max value to prevent new handlers registration within the callback */
	thread->num_pp_handlers = SPDK_THREAD_MAX_POST_POLLER_HANDLERS;

	for (i = 0; i < count; i++) {
		thread->pp_handlers[i].fn(thread->pp_handlers[i].fn_arg); /* [한국어] 콜백 호출. */
		thread->pp_handlers[i].fn = NULL; /* [한국어] 슬롯 비우기 — 다음 라운드 재사용. */
	}

	thread->num_pp_handlers = 0; /* [한국어] 카운트 리셋. */
}

/*
 * [한국어]
 * thread_poll - 한 라운드의 poller/메시지 처리 본체 (HOT PATH 의 핵심).
 *
 * @thread: 폴링할 thread (자신).
 * @max_msgs: 메시지 ring 에서 한 번에 처리할 최대 수.
 * @now: 현재 TSC.
 * @return: 0=idle, 1+=busy (어떤 일이라도 했으면).
 *
 * 처리 우선순위 (한 라운드에서 빠르게 단조 진행):
 *   (1) critical_msg — 단일 슬롯 긴급 메시지.
 *   (2) message ring — msg_queue_run_batch (최대 max_msgs/8).
 *   (3) active_pollers — TAILQ_FOREACH_REVERSE_SAFE 로 tail→head, 각 poller 후 pp_handler 실행.
 *   (4) timed_pollers — first_timed_poller cache 부터 만기 도래한 것들을 RB_REMOVE 후 실행.
 *
 * 호출 빈도: spdk_thread_poll 가 매번 호출 → reactor 루프 = 사실상 매 nano 초.
 *           이 함수의 분기/메모리 접근은 모두 cache locality 와 prefetch 친화적이어야 함.
 *           critical_msg/in_interrupt/EXITING 같은 분기는 spdk_unlikely 로 mark.
 */
static int
thread_poll(struct spdk_thread *thread, uint32_t max_msgs, uint64_t now)
{
	uint32_t msg_count;
	struct spdk_poller *poller, *tmp;
	spdk_msg_fn critical_msg;
	int rc = 0;

	thread->tsc_last = now; /* [한국어] thread_update_stats 가 다시 갱신 — 동시에 라운드 시작점 기록. */

	/* [한국어] (1) critical_msg: 한 슬롯짜리 긴급 메시지. atomic CAS 로 NULL→fn 등록되며 여기서 읽고 클리어. */
	critical_msg = thread->critical_msg;
	if (spdk_unlikely(critical_msg != NULL)) {
		critical_msg(NULL);          /* [한국어] critical 콜백은 인자 NULL — emergency only. */
		thread->critical_msg = NULL; /* [한국어] 다음 producer 가 다시 CAS 가능하도록. */
		rc = 1;                      /* [한국어] busy 표시. */
	}

	/* [한국어] (2) message ring drain — 8 개 batch. */
	msg_count = msg_queue_run_batch(thread, max_msgs);
	if (msg_count) {
		rc = 1;
	}

	/* [한국어] (3) active_pollers 순회. REVERSE_SAFE: tail 에서 head 방향. SAFE 변형은 루프 안에서
	 * REMOVE 해도 다음 노드 보존. round-robin 효과 — 새로 등록된 poller 가 tail 에 들어가므로
	 * 곧 실행될 수 있음. */
	TAILQ_FOREACH_REVERSE_SAFE(poller, &thread->active_pollers,
				   active_pollers_head, tailq, tmp) {
		int poller_rc;

		poller_rc = thread_execute_poller(thread, poller);
		if (poller_rc > rc) {
			rc = poller_rc; /* [한국어] busy 우선 (1>0, 양수 우선). */
		}
		if (thread->num_pp_handlers) {
			thread_run_pp_handlers(thread); /* [한국어] 등록된 post-poller 핸들러 일괄 호출. */
		}
	}

	/* [한국어] (4) timed_pollers — first_timed_poller cache 부터 시작. now < first.next_run_tick 이면 break. */
	poller = thread->first_timed_poller;
	while (poller != NULL) {
		int timer_rc = 0;

		if (now < poller->next_run_tick) {
			break; /* [한국어] 첫 만기조차 미래 — 더 볼 것 없음 (정렬 트리이므로). */
		}

		/* [한국어] RB tree 에서 다음 노드 미리 캡처 → REMOVE → execute → (필요시) 재삽입. */
		tmp = RB_NEXT(timed_pollers_tree, &thread->timed_pollers, poller);
		RB_REMOVE(timed_pollers_tree, &thread->timed_pollers, poller);

		/* [한국어] cache 갱신 — REMOVE 한 게 first 였다면 tmp 가 새 first.
		 * 만약 cache 가 이미 다른 노드로 갱신되었다면(e.g., 콜백 안에서 새 timer 삽입) 그대로 둠. */
		/* Update the cache to the next timed poller in the list
		 * only if the current poller is still the closest, otherwise,
		 * do nothing because the cache has been already updated.
		 */
		if (thread->first_timed_poller == poller) {
			thread->first_timed_poller = tmp;
		}

		timer_rc = thread_execute_timed_poller(thread, poller, now);
		if (timer_rc > rc) {
			rc = timer_rc;
		}

		poller = tmp; /* [한국어] 다음 시간 만기 후보. */
	}

	return rc;
}

/*
 * [한국어]
 * _thread_remove_pollers - interrupt 모드에서 unregister 마킹된 poller 들을 일괄 청소 (메시지 콜백).
 *
 * @ctx: 대상 thread (send_msg 컨텍스트).
 *
 * 동기/배경: interrupt 모드에서는 폴링 루프가 항상 도는 것이 아니므로, unregister 시 즉시 정리 메시지를 보내야 함.
 *           이 콜백은 active/timed pollers 를 순회하며 UNREGISTERED 상태인 것들을 free.
 */
static void
_thread_remove_pollers(void *ctx)
{
	struct spdk_thread *thread = ctx;
	struct spdk_poller *poller, *tmp;

	/* [한국어] active_pollers 정리. */
	TAILQ_FOREACH_REVERSE_SAFE(poller, &thread->active_pollers,
				   active_pollers_head, tailq, tmp) {
		if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
			TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
			free(poller);
		}
	}

	/* [한국어] timed_pollers 정리. */
	RB_FOREACH_SAFE(poller, timed_pollers_tree, &thread->timed_pollers, tmp) {
		if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
			poller_remove_timer(thread, poller);
			free(poller);
		}
	}

	thread->poller_unregistered = false; /* [한국어] in-flight 플래그 클리어 — 다음 unregister 가 새 메시지 보낼 수 있음. */
}

/*
 * [한국어]
 * _thread_exit - interrupt 모드에서 EXITING thread 의 정리 진행을 self-msg 로 반복.
 *
 * @ctx: 대상 thread.
 *
 * polling 모드는 thread_poll 매 라운드마다 thread_exit 검사하지만, interrupt 모드는 wakeup 이 필요 → self-send_msg.
 * 정리 안 끝났으면 자기 자신에게 다시 send_msg → 다음 epoll wakeup 후 재검사.
 */
static void
_thread_exit(void *ctx)
{
	struct spdk_thread *thread = ctx;

	assert(thread->state == SPDK_THREAD_STATE_EXITING);

	thread_exit(thread, spdk_get_ticks());

	if (thread->state != SPDK_THREAD_STATE_EXITED) {
		/* [한국어] 아직 정리 미완 — 자기 자신에 메시지 다시 → epoll wakeup 다음 라운드 재시도. */
		spdk_thread_send_msg(thread, _thread_exit, thread);
	}
}

/*
 * [한국어]
 * spdk_thread_poll - reactor 가 매 라운드 호출하는 메인 진입점 (HOT PATH).
 *
 * @thread: 폴링할 thread.
 * @max_msgs: 메시지 batch 한도 (0 이면 8).
 * @now: 현재 TSC (0 이면 spdk_get_ticks 자동 호출).
 * @return: 0=idle / 1+=busy.
 *
 * 동기/배경: reactor pthread 가 매 사이클 이 함수를 호출하여 hosting 중인 spdk_thread 의 모든 작업 처리.
 *           tls_thread 를 swap 하여 같은 pthread 가 여러 spdk_thread 를 sequentially host 가능.
 * 동작 단계:
 *   (1) orig_thread 백업 + tls_thread = thread (모든 콜백이 spdk_get_thread() 로 알 수 있도록).
 *   (2) now=0 인 경우 spdk_get_ticks 호출.
 *   (3) polling 모드: thread_poll() 호출. 콜백 안에서 in_interrupt 로 전환되면 한 번 더 폴링
 *      (전환 시점 race 방지). EXITING 이면 thread_exit 호출.
 *   (4) interrupt 모드: spdk_fd_group_wait(0) — non-block epoll, wakeup 된 fd 의 핸들러 호출.
 *   (5) thread_update_stats 로 busy/idle TSC 누적.
 *   (6) tls_thread 복원.
 * 호출 빈도: per-core 매 microsecond — 가장 hot path.
 * caller: lib/event/reactor.c 의 reactor 메인 루프.
 */
int
spdk_thread_poll(struct spdk_thread *thread, uint32_t max_msgs, uint64_t now)
{
	struct spdk_thread *orig_thread;
	int rc;

	orig_thread = _get_thread();   /* [한국어] 호출 전 hosting 중이던 thread (대개 NULL 또는 다른 thread). */
	tls_thread = thread;           /* [한국어] 이 라운드 동안 spdk_get_thread()=thread. */

	if (now == 0) {
		now = spdk_get_ticks(); /* [한국어] caller 가 0 패스 시 자동 측정. */
	}

	/* [한국어] polling 모드 — 가장 흔한 경로 (likely). */
	if (spdk_likely(!thread->in_interrupt)) {
		rc = thread_poll(thread, max_msgs, now);
		/* [한국어] 콜백이 in_interrupt 로 동적 전환했을 가능성 — race 회피용으로 한 번 더 처리.
		 * 전환 직후 send_msg 가 들어왔지만 notification 안 갔을 수도 있어 보수적으로 한 번 더 폴링. */
		if (spdk_unlikely(thread->in_interrupt)) {
			/* The thread transitioned to interrupt mode during the above poll.
			 * Poll it one more time in case that during the transition time
			 * there is msg received without notification.
			 */
			rc = thread_poll(thread, max_msgs, now);
		}

		/* [한국어] EXITING 상태면 thread_exit 검사 — 정리 완료 시 EXITED 전이. */
		if (spdk_unlikely(thread->state == SPDK_THREAD_STATE_EXITING)) {
			thread_exit(thread, now);
		}
	} else {
		/* [한국어] interrupt 모드 — fd_group_wait(timeout=0) 로 non-block epoll.
		 * 깨어난 fd 의 콜백 (thread_interrupt_msg_process / interrupt poller wrapper) 이 모두 호출됨. */
		/* Non-block wait on thread's fd_group */
		rc = spdk_fd_group_wait(thread->fgrp, 0);
	}

	/* [한국어] busy/idle TSC 누적 — scheduler 가 이 통계로 부하 균형. */
	thread_update_stats(thread, spdk_get_ticks(), now, rc);

	tls_thread = orig_thread; /* [한국어] TLS 복원 — caller 컨텍스트로 돌아감. */

	return rc;
}

/*
 * [한국어]
 * spdk_thread_next_poller_expiration - 다음 timed poller 만기 TSC 반환.
 *
 * @return: 가장 빠른 만기 TSC 또는 0 (timed poller 없음).
 *
 * scheduler/이벤트 루프가 다음 wakeup 시각 결정 시 사용 (sleep 시간 산정).
 */
uint64_t
spdk_thread_next_poller_expiration(struct spdk_thread *thread)
{
	struct spdk_poller *poller;

	poller = thread->first_timed_poller; /* [한국어] cache 직접 사용 — O(1). */
	if (poller) {
		return poller->next_run_tick;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_thread_has_active_pollers - active poller 가 하나라도 있는지.
 */
int
spdk_thread_has_active_pollers(struct spdk_thread *thread)
{
	return !TAILQ_EMPTY(&thread->active_pollers); /* [한국어] TAILQ 비어있는지 검사 — 1 (있음) 또는 0. */
}

/*
 * [한국어]
 * thread_has_unpaused_pollers - active 또는 timed poller 가 있는지 (paused 제외).
 */
static bool
thread_has_unpaused_pollers(struct spdk_thread *thread)
{
	if (TAILQ_EMPTY(&thread->active_pollers) &&
	    RB_EMPTY(&thread->timed_pollers)) {
		return false;
	}

	return true;
}

/*
 * [한국어]
 * spdk_thread_has_pollers - active/timed/paused 중 하나라도 있는지.
 */
bool
spdk_thread_has_pollers(struct spdk_thread *thread)
{
	if (!thread_has_unpaused_pollers(thread) &&
	    TAILQ_EMPTY(&thread->paused_pollers)) {
		return false;
	}

	return true;
}

/*
 * [한국어]
 * spdk_thread_is_idle - thread 가 완전히 idle 인지 (메시지/poller/critical 모두 없음).
 *
 * scheduler 가 이 thread 를 다른 코어로 옮기거나 sleep 시킬 수 있는지 결정.
 */
bool
spdk_thread_is_idle(struct spdk_thread *thread)
{
	if (spdk_ring_count(thread->messages) ||         /* [한국어] 메시지 ring 잔여. */
	    thread_has_unpaused_pollers(thread) ||       /* [한국어] active/timed poller 존재. */
	    thread->critical_msg != NULL) {              /* [한국어] critical_msg 보류. */
		return false;
	}

	return true;
}

/*
 * [한국어]
 * spdk_thread_get_count - 캐시된 thread 수 반환 (lock 없음).
 *
 * 정확성보다 속도 우선 — 호출 직후 변경될 수 있으니 best-effort.
 */
uint32_t
spdk_thread_get_count(void)
{
	/*
	 * Return cached value of the current thread count.  We could acquire the
	 *  lock and iterate through the TAILQ of threads to count them, but that
	 *  count could still be invalidated after we release the lock.
	 */
	return g_thread_count;
}

/*
 * [한국어]
 * spdk_get_thread - 현재 pthread 가 hosting 중인 spdk_thread 반환 (TLS).
 */
struct spdk_thread *
spdk_get_thread(void)
{
	return _get_thread();
}

/*
 * [한국어]
 * spdk_thread_get_name - thread 이름 반환.
 */
const char *
spdk_thread_get_name(const struct spdk_thread *thread)
{
	return thread->name;
}

/*
 * [한국어]
 * spdk_thread_get_id - thread ID 반환.
 */
uint64_t
spdk_thread_get_id(const struct spdk_thread *thread)
{
	return thread->id;
}

/*
 * [한국어]
 * spdk_thread_get_by_id - ID 로 thread 검색.
 *
 * @id: 1..g_thread_id-1 범위.
 * @return: 매칭되는 thread 또는 NULL.
 *
 * O(N) — g_threads TAILQ 선형 탐색. 보통 RPC/디버그 경로에서만 사용.
 */
struct spdk_thread *
spdk_thread_get_by_id(uint64_t id)
{
	struct spdk_thread *thread;

	if (id == 0 || id >= g_thread_id) {
		/* [한국어] 0 은 invalid sentinel, g_thread_id 이상은 아직 발급되지 않음. */
		SPDK_ERRLOG("invalid thread id: %" PRIu64 ".\n", id);
		return NULL;
	}
	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_FOREACH(thread, &g_threads, tailq) {
		if (thread->id == id) {
			break;
		}
	}
	pthread_mutex_unlock(&g_devlist_mutex);
	return thread;
}

/*
 * [한국어]
 * spdk_thread_get_stats - 현재 thread 의 busy_tsc/idle_tsc 통계 복사.
 *
 * @stats: 출력 버퍼.
 * @return: 0 성공, -EINVAL.
 */
int
spdk_thread_get_stats(struct spdk_thread_stats *stats)
{
	struct spdk_thread *thread;

	thread = _get_thread();
	if (!thread) {
		SPDK_ERRLOG("No thread allocated\n");
		return -EINVAL;
	}

	if (stats == NULL) {
		return -EINVAL;
	}

	*stats = thread->stats; /* [한국어] 구조체 복사 (16B). race 가능 — RPC 디스플레이용으로만 안전. */

	return 0;
}

/*
 * [한국어]
 * spdk_thread_get_last_tsc - 마지막 spdk_thread_poll 종료 시점 TSC.
 *
 * @thread: 대상 (NULL 이면 현재).
 */
uint64_t
spdk_thread_get_last_tsc(struct spdk_thread *thread)
{
	if (thread == NULL) {
		thread = _get_thread();
	}

	return thread->tsc_last;
}

/*
 * [한국어]
 * thread_send_msg_notification - interrupt 모드에서 타깃 thread 의 epoll wakeup 트리거.
 *
 * @target_thread: 메시지 수신 thread.
 *
 * 동기/배경: polling 모드에서는 메시지를 ring 에 enqueue 만 해도 다음 spdk_thread_poll 라운드에서 처리.
 *           interrupt 모드는 fd_group_wait 에서 sleep 중일 수 있으므로 msg_fd(eventfd) 에 write 하여 wakeup.
 *           polling 이 likely 한 hot path 이므로 spdk_likely 분기 후 즉시 return.
 */
static inline void
thread_send_msg_notification(const struct spdk_thread *target_thread)
{
	uint64_t notify = 1;
	int rc;

	/* [한국어] interrupt mode 비활성 — 알림 불필요. */
	/* Not necessary to do notification if interrupt facility is not enabled */
	if (spdk_likely(!spdk_interrupt_mode_is_enabled())) {
		return;
	}

	/* [한국어] thread 가 동적으로 polling↔interrupt 사이를 전환 가능하므로 매번 검사.
	 * polling 인 동안은 ring enqueue 만으로 충분, interrupt 면 eventfd write 로 wakeup. */
	/* When each spdk_thread can switch between poll and interrupt mode dynamically,
	 * after sending thread msg, it is necessary to check whether target thread runs in
	 * interrupt mode and then decide whether do event notification.
	 */
	if (spdk_unlikely(target_thread->in_interrupt)) {
		/* [한국어] eventfd write — 8B counter 증가, level-trigger 보장 (counter>0 동안 readable). */
		rc = write(target_thread->msg_fd, &notify, sizeof(notify));
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify msg_queue: %s.\n", spdk_strerror(errno));
			abort(); /* [한국어] 알림 실패 = 데드락 위험 — 즉시 abort. */
		}
	}
}

/*
 * [한국어]
 * spdk_thread_send_msg - 타깃 thread 에 비동기 메시지 전송 (cross-thread, lockless).
 *
 * @thread: 메시지 수신자 (자기 자신도 가능 — self-msg).
 * @fn: 타깃 thread 컨텍스트에서 실행될 함수.
 * @ctx: fn 호출 시 인자.
 * @return: 0 성공 (실패는 abort, 즉 invariant 보장).
 *
 * 동기/배경: SPDK 의 cross-thread 통신 핵심. **ring(MPSC, 64K) 와 per-thread msg_cache 의 조합으로 lockless**.
 *           원자성 모델: producer 는 cache 또는 mempool 에서 msg 객체 획득 → ring 에 enqueue (CAS) →
 *           interrupt 모드면 wakeup. 단일 atomic CAS + 1 메모리 store.
 * 동작 단계:
 *   (1) state=EXITED 면 abort (invariant — 종료된 thread 에 send 금지).
 *   (2) local_thread 의 msg_cache 에서 hot msg 재사용 시도 (LIFO pop).
 *   (3) cache miss 면 글로벌 mempool 에서 lockless 할당.
 *   (4) msg->fn/arg 설정.
 *   (5) spdk_ring_enqueue 1 개 — DPDK rte_ring CAS.
 *   (6) interrupt 모드면 thread_send_msg_notification.
 *
 * Hot path 노트: cache hit case 는 atomic 0회 (cache 는 owner 만 접근 → lockfree), enqueue 의 1 atomic CAS 만.
 *                 mempool 도 자체 lockless ring. 따라서 producer 측은 1 RMW 에 가까움.
 */
int
spdk_thread_send_msg(const struct spdk_thread *thread, spdk_msg_fn fn, void *ctx)
{
	struct spdk_thread *local_thread;
	struct spdk_msg *msg;
	int rc;

	assert(thread != NULL);

	/* [한국어] EXITED thread 에 메시지 보내면 영원히 처리되지 않음 — silent drop 대신 abort. */
	if (spdk_unlikely(thread->state == SPDK_THREAD_STATE_EXITED)) {
		SPDK_ERRLOG("Thread %s is marked as exited.\n", thread->name);
		abort();
	}

	local_thread = _get_thread(); /* [한국어] producer 가 SPDK thread 면 자기 cache 사용 가능. */

	msg = NULL;
	if (local_thread != NULL) {
		if (local_thread->msg_cache_count > 0) {
			/* [한국어] cache 히트 — owner-only LIFO pop. atomic 불필요 (자기 thread 만 접근). */
			msg = SLIST_FIRST(&local_thread->msg_cache);
			assert(msg != NULL);
			SLIST_REMOVE_HEAD(&local_thread->msg_cache, link);
			local_thread->msg_cache_count--;
		}
	}

	if (msg == NULL) {
		/* [한국어] cache miss — 글로벌 mempool 에서 lockless 할당 (DPDK rte_mempool). */
		msg = spdk_mempool_get(g_spdk_msg_mempool);
		if (!msg) {
			/* [한국어] mempool 고갈 — 시스템 한계 도달, 진행 불가. */
			SPDK_ERRLOG("msg could not be allocated\n");
			abort();
		}
	}

	msg->fn = fn;     /* [한국어] 실행 함수 저장. */
	msg->arg = ctx;   /* [한국어] 사용자 컨텍스트. */

	/* [한국어] ring 에 1개 enqueue — DPDK rte_ring 의 MP variant (Multi-Producer Single-Consumer 의 P 측).
	 * NULL 인자는 free_space 미사용 (반환되는 잔여 슬롯 수 무시). */
	rc = spdk_ring_enqueue(thread->messages, (void **)&msg, 1, NULL);
	if (rc != 1) {
		/* [한국어] ring full — 64K slot 도 못 받았다면 시스템 과부하. abort. */
		SPDK_ERRLOG("msg could not be enqueued\n");
		abort();
	}

	thread_send_msg_notification(thread); /* [한국어] interrupt 모드면 wakeup. polling 은 noop. */

	return 0;
}

/*
 * [한국어]
 * spdk_thread_send_critical_msg - 단일 슬롯의 긴급 메시지 전송 (atomic CAS, 메시지 ring 우회).
 *
 * @thread: 수신자.
 * @fn: 실행 함수 (인자는 NULL 고정).
 * @return: 0 성공 (이미 critical_msg 가 set 되어 있으면 abort).
 *
 * 동기/배경: 메시지 ring 이 가득 찼거나 mempool 고갈 등 긴급 상황에서 사용. critical_msg 슬롯은 한 번에 1개만 보유 가능.
 *           thread_poll 매 라운드 시작에 검사·실행·NULL 클리어.
 */
int
spdk_thread_send_critical_msg(struct spdk_thread *thread, spdk_msg_fn fn)
{
	spdk_msg_fn expected = NULL;

	/* [한국어] CAS: NULL → fn 으로 atomic 교체. 이미 비-NULL 이면 false → 슬롯 점유 충돌 → abort.
	 * SEQ_CST 로 release/acquire 순서 보장. */
	if (!__atomic_compare_exchange_n(&thread->critical_msg, &expected, fn, false, __ATOMIC_SEQ_CST,
					 __ATOMIC_SEQ_CST)) {
		abort();
	}

	thread_send_msg_notification(thread); /* [한국어] interrupt 모드 wakeup. */

	return 0;
}

#ifdef __linux__
/*
 * [한국어]
 * interrupt_timerfd_process - timed poller 의 interrupt 모드 wrapper.
 *
 * @arg: spdk_poller (콜백 등록 시 우리가 넘긴 값).
 * @return: poller fn 의 반환값 (0/양수/-1).
 *
 * 동기/배경: interrupt 모드에서는 thread_poll 의 timed loop 가 동작하지 않으므로, period 마다 timerfd
 *           가 readable 해지면 epoll 이 이 wrapper 를 호출 → read 로 만기 카운터 클리어 → poller fn 호출.
 */
static int
interrupt_timerfd_process(void *arg)
{
	struct spdk_poller *poller = arg;
	uint64_t exp;
	int rc;

	/* [한국어] timerfd 의 만기 카운터 read — level-trigger 클리어. exp 는 누적 만기 횟수. */
	/* clear the level of interval timer */
	rc = read(poller->intr->efd, &exp, sizeof(exp));
	if (rc < 0) {
		if (rc == -EAGAIN) {
			return 0; /* [한국어] non-block 인데 데이터 없음 — 스푸리어스 wakeup 처리. */
		}

		return rc; /* [한국어] read 에러 그대로 전달. */
	}

	SPDK_DTRACE_PROBE2(timerfd_exec, poller->fn, poller->arg); /* [한국어] DTrace probe. */

	return poller->fn(poller->arg); /* [한국어] poller 본체 실행. */
}

/*
 * [한국어]
 * period_poller_interrupt_init - 주기 poller 의 timerfd 생성·등록 (interrupt 모드).
 *
 * @poller: 대상 (period_ticks > 0).
 * @return: 0 성공, -errno 실패.
 *
 * timerfd_create(CLOCK_MONOTONIC, NONBLOCK|CLOEXEC) → spdk_interrupt_register 로 fgrp 등록.
 * 실제 만기 시간 설정은 period_poller_set_interrupt_mode 가 timerfd_settime 으로 진행.
 */
static int
period_poller_interrupt_init(struct spdk_poller *poller)
{
	int timerfd;

	SPDK_DEBUGLOG(thread, "timerfd init for periodic poller %s\n", poller->name);
	/* [한국어] timerfd_create — Linux 전용 시스템 호출. CLOCK_MONOTONIC 은 시스템 부팅 후 단조 증가 시간.
	 * TFD_NONBLOCK 으로 read 비차단, TFD_CLOEXEC 로 fork-exec 시 자동 close. */
	timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (timerfd < 0) {
		return -errno;
	}

	/* [한국어] fd_group 에 등록 — readable 시 interrupt_timerfd_process 호출. */
	poller->intr = spdk_interrupt_register(timerfd, interrupt_timerfd_process, poller, poller->name);
	if (poller->intr == NULL) {
		close(timerfd);
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * period_poller_set_interrupt_mode - polling↔interrupt 동적 전환 시 timerfd 설정.
 *
 * @poller: 대상 (period > 0).
 * @cb_arg: 미사용 (콜백 인터페이스 호환).
 * @interrupt_mode: true=interrupt 진입(timerfd_settime arm), false=polling 복귀(timerfd disarm + RB tree 재삽입).
 *
 * 동기/배경: 동적 전환 시 timerfd 가 epoll wakeup 의 trigger 역할을 해야 함. interrupt 진입 시 만기 시점 계산하여 arm,
 *           polling 복귀 시 disarm 후 남은 시간으로 RB tree 에 다시 삽입 (만기 정렬 유지).
 */
static void
period_poller_set_interrupt_mode(struct spdk_poller *poller, void *cb_arg, bool interrupt_mode)
{
	int timerfd;
	uint64_t now_tick = spdk_get_ticks();    /* [한국어] 현재 TSC. */
	uint64_t ticks = spdk_get_ticks_hz();    /* [한국어] 1초당 TSC 틱 수 (CPU 주파수). */
	int ret;
	struct itimerspec new_tv = {};           /* [한국어] timerfd_settime 인자 — 새 만기 설정. */
	struct itimerspec old_tv = {};           /* [한국어] disarm 시 남은 시간 보존용. */

	assert(poller->intr != NULL);            /* [한국어] init 단계에서 intr 가 생성되어 있어야 함. */
	assert(poller->period_ticks != 0);       /* [한국어] period 0 은 busy poller — 별개 함수. */

	timerfd = poller->intr->efd;             /* [한국어] init 에서 저장한 timerfd. */

	assert(timerfd >= 0);

	SPDK_DEBUGLOG(thread, "timerfd set poller %s into %s mode\n", poller->name,
		      interrupt_mode ? "interrupt" : "poll");

	if (interrupt_mode) {
		/* [한국어] interrupt 진입 — 반복 만기 timer arm. interval 은 period_ticks 를 sec+nsec 으로 분해. */
		/* Set repeated timer expiration */
		new_tv.it_interval.tv_sec = poller->period_ticks / ticks;
		new_tv.it_interval.tv_nsec = poller->period_ticks % ticks * SPDK_SEC_TO_NSEC / ticks;

		/* [한국어] 첫 만기 시점 결정. 등록 직후거나 과거 만기면 보정. */
		/* Update next timer expiration */
		if (poller->next_run_tick == 0) {
			poller->next_run_tick = now_tick + poller->period_ticks;
		} else if (poller->next_run_tick < now_tick) {
			/* [한국어] 이미 만기 — it_value 가 0 이 되면 timer disarm 으로 간주되므로 1us 후로 강제. */
			/* Set next_run_tick to now + 1us to make sure new_tv.it_value is never zeroed */
			poller->next_run_tick = now_tick + ticks / SPDK_SEC_TO_USEC;
		}

		/* [한국어] it_value = 다음 만기까지 남은 시간 — sec+nsec 분해. */
		new_tv.it_value.tv_sec = (poller->next_run_tick - now_tick) / ticks;
		new_tv.it_value.tv_nsec = (poller->next_run_tick - now_tick) % ticks * SPDK_SEC_TO_NSEC / ticks;

		/* [한국어] timerfd_settime — flags=0(상대 시간), new_tv 를 적용, old_tv 는 NULL (안 가져옴). */
		ret = timerfd_settime(timerfd, 0, &new_tv, NULL);
		if (ret < 0) {
			SPDK_ERRLOG("Failed to arm timerfd: error(%d)\n", errno);
			assert(false);
		}
	} else {
		/* [한국어] polling 복귀 — timer disarm. new_tv 는 모두 0 이므로 stop. old_tv 에 남은 시간 캡처. */
		/* Disarm the timer */
		ret = timerfd_settime(timerfd, 0, &new_tv, &old_tv);
		if (ret < 0) {
			/* [한국어] disarm 실패는 timerfd 손상 — fatal 디버그. */
			/* timerfd_settime's failure indicates that the timerfd is in error */
			SPDK_ERRLOG("Failed to disarm timerfd: error(%d)\n", errno);
			assert(false);
		}

		/* [한국어] 남은 시간을 RB tree 의 next_run_tick 으로 환원. poller_insert_timer 가 now+period 로 계산하므로
		 * now_tick 을 역산: now - period + (남은 sec*ticks + 남은 nsec*ticks/SEC_TO_NSEC). */
		/* In order to reuse poller_insert_timer, fix now_tick, so next_run_tick would be
		 * now_tick + ticks * old_tv.it_value.tv_sec + (ticks * old_tv.it_value.tv_nsec) / SPDK_SEC_TO_NSEC
		 */
		now_tick = now_tick - poller->period_ticks + ticks * old_tv.it_value.tv_sec + \
			   (ticks * old_tv.it_value.tv_nsec) / SPDK_SEC_TO_NSEC;
		poller_remove_timer(poller->thread, poller); /* [한국어] 혹시 모를 기존 노드 제거. */
		poller_insert_timer(poller->thread, poller, now_tick); /* [한국어] 환원된 만기로 재삽입. */
	}
}

/*
 * [한국어]
 * poller_interrupt_fini - poller 의 interrupt 자원(timerfd/eventfd, fd_group 등록) 해제.
 *
 * @poller: 대상.
 *
 * 호출 컨텍스트: spdk_poller_unregister, spdk_poller_register_interrupt(교체) 등.
 */
static void
poller_interrupt_fini(struct spdk_poller *poller)
{
	int fd;

	SPDK_DEBUGLOG(thread, "interrupt fini for poller %s\n", poller->name);
	assert(poller->intr != NULL);
	fd = poller->intr->efd;                       /* [한국어] close 할 fd 보존. */
	spdk_interrupt_unregister(&poller->intr);     /* [한국어] fd_group 에서 제거 + intr 해제. NULL 세팅. */
	close(fd);                                    /* [한국어] fd 닫기 — 리눅스 자원 회수. */
}

/*
 * [한국어]
 * busy_poller_interrupt_init - period=0 (busy) poller 의 eventfd 생성·등록.
 *
 * @poller: 대상.
 * @return: 0 성공.
 *
 * busy poller 는 매 라운드 호출되어야 하므로 interrupt 모드에서는 "항상 readable" 한 eventfd 를 만든다.
 * write 만 하고 read 안 하면 level-trigger 가 계속 fire — 결과적으로 매 epoll wakeup 마다 호출됨.
 */
static int
busy_poller_interrupt_init(struct spdk_poller *poller)
{
	int busy_efd;

	SPDK_DEBUGLOG(thread, "busy_efd init for busy poller %s\n", poller->name);
	/* [한국어] eventfd(initval=0, NONBLOCK|CLOEXEC). counter 가 0 인 동안은 not readable. */
	busy_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (busy_efd < 0) {
		SPDK_ERRLOG("Failed to create eventfd for Poller(%s).\n", poller->name);
		return -errno;
	}

	/* [한국어] fd_group 에 직접 poller->fn 을 콜백으로 등록 — wrapper 없이 fn 이 곧 인터럽트 핸들러. */
	poller->intr = spdk_interrupt_register(busy_efd, poller->fn, poller->arg, poller->name);
	if (poller->intr == NULL) {
		close(busy_efd);
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * busy_poller_set_interrupt_mode - busy poller 의 polling↔interrupt 전환 시 eventfd write/read.
 *
 * @poller: 대상 (period=0).
 * @cb_arg: 미사용.
 * @interrupt_mode: true=write 로 trigger 영구화, false=read 로 클리어.
 */
static void
busy_poller_set_interrupt_mode(struct spdk_poller *poller, void *cb_arg, bool interrupt_mode)
{
	int busy_efd = poller->intr->efd;
	uint64_t notify = 1;
	int rc __attribute__((unused)); /* [한국어] read 결과 미사용 — 컴파일러 경고 억제. */

	assert(busy_efd >= 0);

	if (interrupt_mode) {
		/* [한국어] eventfd write — read 없이 write 만 하면 counter>0 유지 → level-trigger 항상 fire.
		 * 결과적으로 epoll 가 매 wakeup 마다 이 fd 를 readable 로 봄 → busy poller fn 이 매 라운드 호출. */
		/* Write without read on eventfd will get it repeatedly triggered. */
		if (write(busy_efd, &notify, sizeof(notify)) < 0) {
			SPDK_ERRLOG("Failed to set busy wait for Poller(%s).\n", poller->name);
		}
	} else {
		/* [한국어] polling 복귀 — counter 클리어하여 fd 가 not readable 상태로. polling 루프가 직접 fn 호출. */
		/* Read on eventfd will clear its level triggering. */
		rc = read(busy_efd, &notify, sizeof(notify));
	}
}

#else
/* [한국어] non-Linux — interrupt 모드 미지원. 모든 함수 스텁. */

static int
period_poller_interrupt_init(struct spdk_poller *poller)
{
	return -ENOTSUP;
}

static void
period_poller_set_interrupt_mode(struct spdk_poller *poller, void *cb_arg, bool interrupt_mode)
{
}

static void
poller_interrupt_fini(struct spdk_poller *poller)
{
}

static int
busy_poller_interrupt_init(struct spdk_poller *poller)
{
	return -ENOTSUP;
}

static void
busy_poller_set_interrupt_mode(struct spdk_poller *poller, void *cb_arg, bool interrupt_mode)
{
}

#endif

/*
 * [한국어]
 * spdk_poller_register_interrupt - 기존 poller 에 사용자 정의 interrupt 모드 콜백 등록.
 *
 * @poller: 대상 poller (이미 register 되어 있어야 함).
 * @cb_fn: polling↔interrupt 전환 시 호출될 사용자 콜백 (내장 timerfd/busy 콜백 대체).
 * @cb_arg: cb_fn 인자.
 *
 * 동기/배경: 일부 모듈은 자기 fd (e.g., NVMe completion fd, socket fd) 가 wakeup trigger 가 되어야 함.
 *           이 함수로 기본 timerfd/eventfd 콜백을 제거하고 사용자 콜백 등록. interrupt 모드 비활성이면 noop.
 */
void
spdk_poller_register_interrupt(struct spdk_poller *poller,
			       spdk_poller_set_interrupt_mode_cb cb_fn,
			       void *cb_arg)
{
	assert(poller != NULL);
	assert(spdk_get_thread() == poller->thread); /* [한국어] poller 소유 thread 에서만 호출 가능. */

	if (!spdk_interrupt_mode_is_enabled()) {
		return; /* [한국어] interrupt 모드 비활성 — 등록할 의미 없음. */
	}

	/* [한국어] 기존 timerfd/eventfd 정리 — 사용자 콜백이 자기 fd 를 따로 관리할 것. */
	/* If this poller already had an interrupt, clean the old one up. */
	if (poller->intr != NULL) {
		poller_interrupt_fini(poller);
	}

	poller->set_intr_cb_fn = cb_fn;     /* [한국어] 사용자 콜백 저장. */
	poller->set_intr_cb_arg = cb_arg;

	/* [한국어] thread 가 이미 interrupt 모드면 즉시 진입 호출 — 즉시 mode 동기화. */
	/* Set poller into interrupt mode if thread is in interrupt. */
	if (poller->thread->in_interrupt && poller->set_intr_cb_fn) {
		poller->set_intr_cb_fn(poller, poller->set_intr_cb_arg, true);
	}
}

/*
 * [한국어]
 * convert_us_to_ticks - microsecond → CPU tick 변환.
 *
 * @us: 변환할 microsecond 값.
 * @return: ticks (us=0 이면 0).
 *
 * 분리 계산으로 정밀도 손실 최소화: ticks = (us / 1e6) * hz + ((us % 1e6) * hz) / 1e6.
 * 첫 항은 큰 값(초 단위), 둘째 항은 1us 미만 잔여 — 64bit 곱셈 오버플로 회피.
 */
static uint64_t
convert_us_to_ticks(uint64_t us)
{
	uint64_t quotient, remainder, ticks;

	if (us) {
		quotient = us / SPDK_SEC_TO_USEC;     /* [한국어] us / 1e6 = sec. */
		remainder = us % SPDK_SEC_TO_USEC;    /* [한국어] 잔여 us. */
		ticks = spdk_get_ticks_hz();          /* [한국어] CPU 주파수 (tick/sec). */

		return ticks * quotient + (ticks * remainder) / SPDK_SEC_TO_USEC;
	} else {
		return 0;                             /* [한국어] period=0 (busy poller) 의미. */
	}
}

/*
 * [한국어]
 * poller_register - poller 등록 본체 (spdk_poller_register / _named 의 공통 구현).
 *
 * @fn: poller 본체 함수.
 * @arg: fn 인자.
 * @period_microseconds: 호출 주기 us (0=busy poller).
 * @name: poller 이름 (NULL 이면 fn hex).
 * @return: 등록된 spdk_poller 또는 NULL (allocation 실패).
 *
 * 동작 단계:
 *   (1) 현재 thread 검증, EXITED 검사.
 *   (2) calloc 으로 poller 객체 생성, name/fn/arg/period 저장, ID 발급.
 *   (3) interrupt 모드면 period 에 따라 timerfd 또는 eventfd 등록.
 *   (4) thread_insert_poller — period 0 → active TAILQ, period>0 → timed RB tree.
 *
 * 실행 컨텍스트: 등록할 thread 자신 (cross-thread 등록 불가 — wrong_thread).
 */
static struct spdk_poller *
poller_register(spdk_poller_fn fn,
		void *arg,
		uint64_t period_microseconds,
		const char *name)
{
	struct spdk_thread *thread;
	struct spdk_poller *poller;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false); /* [한국어] 비-SPDK thread 에서 호출 불가. */
		return NULL;
	}

	if (spdk_unlikely(thread->state == SPDK_THREAD_STATE_EXITED)) {
		SPDK_ERRLOG("thread %s is marked as exited\n", thread->name);
		return NULL;
	}

	poller = calloc(1, sizeof(*poller)); /* [한국어] zero-init — state=WAITING(0), 모든 카운터 0. */
	if (poller == NULL) {
		SPDK_ERRLOG("Poller memory allocation failed\n");
		return NULL;
	}

	/* [한국어] 이름 — NULL 이면 fn 의 hex (디버그). */
	if (name) {
		snprintf(poller->name, sizeof(poller->name), "%s", name);
	} else {
		snprintf(poller->name, sizeof(poller->name), "%p", fn);
	}

	poller->state = SPDK_POLLER_STATE_WAITING; /* [한국어] 등록 직후 대기 상태. */
	poller->fn = fn;
	poller->arg = arg;
	poller->thread = thread;
	poller->intr = NULL;
	/* [한국어] poller ID wrap 처리 — 0 도달 시 1 부터 재시작 (id 충돌 가능 — 경고만). */
	if (thread->next_poller_id == 0) {
		SPDK_WARNLOG("Poller ID rolled over. Poller ID is duplicated.\n");
		thread->next_poller_id = 1;
	}
	poller->id = thread->next_poller_id++;

	poller->period_ticks = convert_us_to_ticks(period_microseconds);

	/* [한국어] interrupt 모드 활성 — period 에 따라 timerfd 또는 eventfd 등록. */
	if (spdk_interrupt_mode_is_enabled()) {
		int rc;

		if (period_microseconds) {
			rc = period_poller_interrupt_init(poller); /* [한국어] timerfd. */
			if (rc < 0) {
				SPDK_ERRLOG("Failed to register interruptfd for periodic poller: %s\n", spdk_strerror(-rc));
				free(poller);
				return NULL;
			}

			poller->set_intr_cb_fn = period_poller_set_interrupt_mode;
			poller->set_intr_cb_arg = NULL;

		} else {
			/* [한국어] busy poller — 항상 readable 한 eventfd 로 매 wakeup fire. */
			/* If the poller doesn't have a period, create interruptfd that's always
			 * busy automatically when running in interrupt mode.
			 */
			rc = busy_poller_interrupt_init(poller);
			if (rc > 0) {
				SPDK_ERRLOG("Failed to register interruptfd for busy poller: %s\n", spdk_strerror(-rc));
				free(poller);
				return NULL;
			}

			poller->set_intr_cb_fn = busy_poller_set_interrupt_mode;
			poller->set_intr_cb_arg = NULL;
		}

		/* [한국어] 등록 시점 thread 가 이미 interrupt 면 즉시 동기화 호출. */
		/* Set poller into interrupt mode if thread is in interrupt. */
		if (poller->thread->in_interrupt) {
			poller->set_intr_cb_fn(poller, poller->set_intr_cb_arg, true);
		}
	}

	thread_insert_poller(thread, poller); /* [한국어] period 에 따라 active TAILQ 또는 timed RB tree. */

	return poller;
}

/*
 * [한국어]
 * spdk_poller_register - 이름 없이 poller 등록.
 */
struct spdk_poller *
spdk_poller_register(spdk_poller_fn fn,
		     void *arg,
		     uint64_t period_microseconds)
{
	return poller_register(fn, arg, period_microseconds, NULL);
}

/*
 * [한국어]
 * spdk_poller_register_named - 이름 지정 poller 등록.
 */
struct spdk_poller *
spdk_poller_register_named(spdk_poller_fn fn,
			   void *arg,
			   uint64_t period_microseconds,
			   const char *name)
{
	return poller_register(fn, arg, period_microseconds, name);
}

/*
 * [한국어]
 * wrong_thread - thread affinity 위반 시 abort + 진단 로그.
 *
 * @func: 호출자 함수 이름 (__func__).
 * @name: 대상 객체 이름 (poller/channel/interrupt 등).
 * @thread: 대상이 등록된 thread.
 * @curthread: 현재 실행 중인 thread.
 *
 * SPDK 의 thread affinity 모델을 위반한 호출 (cross-thread API 사용) 을 즉시 발견·abort.
 */
static void
wrong_thread(const char *func, const char *name, struct spdk_thread *thread,
	     struct spdk_thread *curthread)
{
	if (thread == NULL) {
		SPDK_ERRLOG("%s(%s) called with NULL thread\n", func, name);
		abort();
	}
	SPDK_ERRLOG("%s(%s) called from wrong thread %s:%" PRIu64 " (should be "
		    "%s:%" PRIu64 ")\n", func, name, curthread->name, curthread->id,
		    thread->name, thread->id);
	assert(false); /* [한국어] DEBUG 빌드는 즉시 abort, release 는 ERRLOG 만. */
}

/*
 * [한국어]
 * spdk_poller_unregister - poller 등록 해제 (실제 정리는 다음 spdk_thread_poll 라운드).
 *
 * @ppoller: poller 포인터의 포인터. 호출 후 *ppoller 는 NULL 로 set (use-after-free 방지).
 *
 * 동기/배경: 즉시 free 하지 않고 SPDK_POLLER_STATE_UNREGISTERED 로 마킹만. 다음 thread_poll 라운드에
 *           thread_execute_poller / thread_execute_timed_poller 가 안전하게 free.
 *           interrupt 모드는 polling 루프가 자주 안 돌므로 _thread_remove_pollers 를 send_msg.
 *           PAUSED 상태였다면 active 큐로 이동시켜 정리 흐름을 통일.
 */
void
spdk_poller_unregister(struct spdk_poller **ppoller)
{
	struct spdk_thread *thread;
	struct spdk_poller *poller;

	poller = *ppoller;
	if (poller == NULL) {
		return; /* [한국어] 이미 unregister 된 경우 — 멱등성. */
	}

	*ppoller = NULL; /* [한국어] caller 의 포인터 즉시 NULL — dangling 사용 방지. */

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return;
	}

	if (poller->thread != thread) {
		/* [한국어] 다른 thread 의 poller 를 unregister 시도 — 위반. */
		wrong_thread(__func__, poller->name, poller->thread, thread);
		return;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		/* [한국어] interrupt 자원(timerfd/eventfd, fd_group 등록) 즉시 해제. */
		/* Release the interrupt resource for period or busy poller */
		if (poller->intr != NULL) {
			poller_interrupt_fini(poller);
		}

		/* [한국어] 이미 보낸 정리 메시지가 있으면 추가 송신 안 함 — 한 라운드에 한 번만 send. */
		/* If there is not already a pending poller removal, generate
		 * a message to go process removals. */
		if (!thread->poller_unregistered) {
			thread->poller_unregistered = true;
			spdk_thread_send_msg(thread, _thread_remove_pollers, thread);
		}
	}

	/* [한국어] PAUSED 상태였다면 active 로 이동 — thread_execute_poller 가 UNREGISTERED 처리. */
	/* If the poller was paused, put it on the active_pollers list so that
	 * its unregistration can be processed by spdk_thread_poll().
	 */
	if (poller->state == SPDK_POLLER_STATE_PAUSED) {
		TAILQ_REMOVE(&thread->paused_pollers, poller, tailq);
		TAILQ_INSERT_TAIL(&thread->active_pollers, poller, tailq);
		poller->period_ticks = 0; /* [한국어] active 로 옮겨졌으니 timed 재삽입 회피. */
	}

	/* [한국어] 마킹만 — 실제 free 는 다음 thread_poll 라운드. */
	/* Simply set the state to unregistered. The poller will get cleaned up
	 * in a subsequent call to spdk_thread_poll().
	 */
	poller->state = SPDK_POLLER_STATE_UNREGISTERED;
}

/*
 * [한국어]
 * spdk_poller_pause - poller 일시정지 마킹.
 *
 * @poller: 대상.
 *
 * 동기/배경: 즉시 큐 이동하지 않고 PAUSING 으로 마킹만. 다음 thread_poll 라운드의 thread_execute_poller
 *           가 active/timed 큐에서 빼서 paused 로 이동. 이 방식은 자기 자신을 pause 하거나 다른 poller 의
 *           fn 안에서 pause 해도 TAILQ_FOREACH_REVERSE_SAFE / RB iteration 을 깨뜨리지 않음.
 */
void
spdk_poller_pause(struct spdk_poller *poller)
{
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return;
	}

	if (poller->thread != thread) {
		wrong_thread(__func__, poller->name, poller->thread, thread);
		return;
	}

	/* [한국어] 상태 마킹만 — 실제 큐 이동은 다음 thread_execute_poller 가 처리.
	 * iteration 안전성과 자기 자신 pause 모두 지원. */
	/* We just set its state to SPDK_POLLER_STATE_PAUSING and let
	 * spdk_thread_poll() move it. It allows a poller to be paused from
	 * another one's context without breaking the TAILQ_FOREACH_REVERSE_SAFE
	 * iteration, or from within itself without breaking the logic to always
	 * remove the closest timed poller in the TAILQ_FOREACH_SAFE iteration.
	 */
	switch (poller->state) {
	case SPDK_POLLER_STATE_PAUSED:
	case SPDK_POLLER_STATE_PAUSING:
		break; /* [한국어] 이미 pause 중/완료 — 멱등성. */
	case SPDK_POLLER_STATE_RUNNING:
	case SPDK_POLLER_STATE_WAITING:
		poller->state = SPDK_POLLER_STATE_PAUSING; /* [한국어] 정상 상태 → PAUSING 마킹. */
		break;
	default:
		assert(false); /* [한국어] UNREGISTERED 상태에서 pause 시도는 invariant 위반. */
		break;
	}
}

/*
 * [한국어]
 * spdk_poller_resume - 일시정지된 poller 를 활성화.
 *
 * @poller: 대상.
 *
 * PAUSED 상태면 paused 큐에서 빼서 active/timed 로 환원, PAUSING 상태면 다시 WAITING 으로 flip 만.
 */
void
spdk_poller_resume(struct spdk_poller *poller)
{
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return;
	}

	if (poller->thread != thread) {
		wrong_thread(__func__, poller->name, poller->thread, thread);
		return;
	}

	/* [한국어] PAUSED → 큐에서 빼서 active/timed 재삽입 후 WAITING. PAUSING → 그대로 WAITING. */
	/* If a poller is paused it has to be removed from the paused pollers
	 * list and put on the active list or timer tree depending on its
	 * period_ticks.  If a poller is still in the process of being paused,
	 * we just need to flip its state back to waiting, as it's already on
	 * the appropriate list or tree.
	 */
	switch (poller->state) {
	case SPDK_POLLER_STATE_PAUSED:
		TAILQ_REMOVE(&thread->paused_pollers, poller, tailq);
		thread_insert_poller(thread, poller); /* [한국어] period 에 따라 active/timed 환원. */
	/* [한국어] fallthrough — 이어서 state 를 WAITING 으로 setting. */
	/* fallthrough */
	case SPDK_POLLER_STATE_PAUSING:
		poller->state = SPDK_POLLER_STATE_WAITING;
		break;
	case SPDK_POLLER_STATE_RUNNING:
	case SPDK_POLLER_STATE_WAITING:
		break; /* [한국어] 이미 활성 — 멱등성. */
	default:
		assert(false);
		break;
	}
}

/*
 * [한국어] poller 메타데이터 단순 getter (RPC/디버그용).
 */
const char *
spdk_poller_get_name(struct spdk_poller *poller)
{
	return poller->name;
}

uint64_t
spdk_poller_get_id(struct spdk_poller *poller)
{
	return poller->id;
}

/*
 * [한국어]
 * spdk_poller_get_state_str - poller 상태를 사람이 읽는 문자열로 반환.
 */
const char *
spdk_poller_get_state_str(struct spdk_poller *poller)
{
	switch (poller->state) {
	case SPDK_POLLER_STATE_WAITING:
		return "waiting";
	case SPDK_POLLER_STATE_RUNNING:
		return "running";
	case SPDK_POLLER_STATE_UNREGISTERED:
		return "unregistered";
	case SPDK_POLLER_STATE_PAUSING:
		return "pausing";
	case SPDK_POLLER_STATE_PAUSED:
		return "paused";
	default:
		return NULL;
	}
}

uint64_t
spdk_poller_get_period_ticks(struct spdk_poller *poller)
{
	return poller->period_ticks;
}

/*
 * [한국어]
 * spdk_poller_get_stats - run/busy 카운터 복사.
 */
void
spdk_poller_get_stats(struct spdk_poller *poller, struct spdk_poller_stats *stats)
{
	stats->run_count = poller->run_count;
	stats->busy_count = poller->busy_count;
}

/*
 * [한국어] iterator 함수 family — RPC/디버그에서 thread 의 poller/io_channel 을 열거.
 * RB_MIN/RB_NEXT 또는 TAILQ_FIRST/NEXT 의 단순 래퍼 — 모두 read-only.
 */
struct spdk_poller *
spdk_thread_get_first_active_poller(struct spdk_thread *thread)
{
	return TAILQ_FIRST(&thread->active_pollers);
}

struct spdk_poller *
spdk_thread_get_next_active_poller(struct spdk_poller *prev)
{
	return TAILQ_NEXT(prev, tailq);
}

struct spdk_poller *
spdk_thread_get_first_timed_poller(struct spdk_thread *thread)
{
	return RB_MIN(timed_pollers_tree, &thread->timed_pollers);
}

/* [한국어] 주의: prev->thread 의 timed_pollers 를 사용 (caller 가 prev 의 thread 컨텍스트일 것). */
struct spdk_poller *
spdk_thread_get_next_timed_poller(struct spdk_poller *prev)
{
	return RB_NEXT(timed_pollers_tree, &thread->timed_pollers, prev);
}

struct spdk_poller *
spdk_thread_get_first_paused_poller(struct spdk_thread *thread)
{
	return TAILQ_FIRST(&thread->paused_pollers);
}

struct spdk_poller *
spdk_thread_get_next_paused_poller(struct spdk_poller *prev)
{
	return TAILQ_NEXT(prev, tailq);
}

struct spdk_io_channel *
spdk_thread_get_first_io_channel(struct spdk_thread *thread)
{
	return RB_MIN(io_channel_tree, &thread->io_channels);
}

struct spdk_io_channel *
spdk_thread_get_next_io_channel(struct spdk_io_channel *prev)
{
	return RB_NEXT(io_channel_tree, &thread->io_channels, prev);
}

/*
 * [한국어]
 * spdk_thread_get_trace_id - thread 의 trace owner ID 반환 (spdk_trace 기록 인자).
 */
uint16_t
spdk_thread_get_trace_id(struct spdk_thread *thread)
{
	return thread->trace_id;
}

struct call_thread {
	struct spdk_thread *cur_thread;
	spdk_msg_fn fn;
	void *ctx;

	struct spdk_thread *orig_thread;
	spdk_msg_fn cpl;
};

static void
_back_to_orig_thread(void *ctx)
{
	struct call_thread *ct = ctx;

	assert(ct->orig_thread->for_each_count > 0);
	ct->orig_thread->for_each_count--;

	if (ct->cpl) {
		ct->cpl(ct->ctx);
	}
	free(ctx);
}

static void
_on_thread(void *ctx)
{
	struct call_thread *ct = ctx;

	ct->fn(ct->ctx);

	pthread_mutex_lock(&g_devlist_mutex);
	ct->cur_thread = TAILQ_NEXT(ct->cur_thread, tailq);
	while (ct->cur_thread && ct->cur_thread->state != SPDK_THREAD_STATE_RUNNING) {
		SPDK_DEBUGLOG(thread, "thread %s is not running but still not destroyed.\n",
			      ct->cur_thread->name);
		ct->cur_thread = TAILQ_NEXT(ct->cur_thread, tailq);
	}
	pthread_mutex_unlock(&g_devlist_mutex);

	if (!ct->cur_thread) {
		SPDK_DEBUGLOG(thread, "Completed thread iteration\n");

		spdk_thread_send_msg(ct->orig_thread, _back_to_orig_thread, ctx);
	} else {
		SPDK_DEBUGLOG(thread, "Continuing thread iteration to %s\n",
			      ct->cur_thread->name);

		spdk_thread_send_msg(ct->cur_thread, _on_thread, ctx);
	}
}

void
spdk_for_each_thread(spdk_msg_fn fn, void *ctx, spdk_msg_fn cpl)
{
	struct call_thread *ct;
	struct spdk_thread *thread;

	ct = calloc(1, sizeof(*ct));
	if (!ct) {
		SPDK_ERRLOG("Unable to perform thread iteration\n");
		cpl(ctx);
		return;
	}

	ct->fn = fn;
	ct->ctx = ctx;
	ct->cpl = cpl;

	thread = _get_thread();
	if (!thread) {
		SPDK_ERRLOG("No thread allocated\n");
		free(ct);
		cpl(ctx);
		return;
	}
	ct->orig_thread = thread;

	ct->orig_thread->for_each_count++;

	pthread_mutex_lock(&g_devlist_mutex);
	ct->cur_thread = TAILQ_FIRST(&g_threads);
	pthread_mutex_unlock(&g_devlist_mutex);

	SPDK_DEBUGLOG(thread, "Starting thread iteration from %s\n",
		      ct->orig_thread->name);

	spdk_thread_send_msg(ct->cur_thread, _on_thread, ct);
}

static inline void
poller_set_interrupt_mode(struct spdk_poller *poller, bool interrupt_mode)
{
	if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
		return;
	}

	if (poller->set_intr_cb_fn) {
		poller->set_intr_cb_fn(poller, poller->set_intr_cb_arg, interrupt_mode);
	}
}

void
spdk_thread_set_interrupt_mode(bool enable_interrupt)
{
	struct spdk_thread *thread = _get_thread();
	struct spdk_poller *poller, *tmp;

	assert(thread);
	assert(spdk_interrupt_mode_is_enabled());

	SPDK_NOTICELOG("Set spdk_thread (%s) to %s mode from %s mode.\n",
		       thread->name,  enable_interrupt ? "intr" : "poll",
		       thread->in_interrupt ? "intr" : "poll");

	if (thread->in_interrupt == enable_interrupt) {
		return;
	}

	/* Set pollers to expected mode */
	RB_FOREACH_SAFE(poller, timed_pollers_tree, &thread->timed_pollers, tmp) {
		poller_set_interrupt_mode(poller, enable_interrupt);
	}
	TAILQ_FOREACH_SAFE(poller, &thread->active_pollers, tailq, tmp) {
		poller_set_interrupt_mode(poller, enable_interrupt);
	}
	/* All paused pollers will go to work in interrupt mode */
	TAILQ_FOREACH_SAFE(poller, &thread->paused_pollers, tailq, tmp) {
		poller_set_interrupt_mode(poller, enable_interrupt);
	}

	thread->in_interrupt = enable_interrupt;
	return;
}

static struct io_device *
io_device_get(void *io_device)
{
	struct io_device find = {};

	find.io_device = io_device;
	return RB_FIND(io_device_tree, &g_io_devices, &find);
}

void
spdk_io_device_register(void *io_device, spdk_io_channel_create_cb create_cb,
			spdk_io_channel_destroy_cb destroy_cb, uint32_t ctx_size,
			const char *name)
{
	struct io_device *dev, *tmp;
	struct spdk_thread *thread;

	assert(io_device != NULL);
	assert(create_cb != NULL);
	assert(destroy_cb != NULL);

	thread = spdk_get_thread();
	if (!thread) {
		SPDK_ERRLOG("called from non-SPDK thread\n");
		assert(false);
		return;
	}

	dev = calloc(1, sizeof(struct io_device));
	if (dev == NULL) {
		SPDK_ERRLOG("could not allocate io_device\n");
		return;
	}

	dev->io_device = io_device;
	if (name) {
		snprintf(dev->name, sizeof(dev->name), "%s", name);
	} else {
		snprintf(dev->name, sizeof(dev->name), "%p", dev);
	}
	dev->create_cb = create_cb;
	dev->destroy_cb = destroy_cb;
	dev->unregister_cb = NULL;
	dev->ctx_size = ctx_size;
	dev->for_each_count = 0;
	dev->unregistered = false;
	dev->refcnt = 0;
	RB_INIT(&dev->threads);

	SPDK_DEBUGLOG(thread, "Registering io_device %s (%p) on thread %s\n",
		      dev->name, dev->io_device, thread->name);

	pthread_mutex_lock(&g_devlist_mutex);
	tmp = RB_INSERT(io_device_tree, &g_io_devices, dev);
	if (tmp != NULL) {
		SPDK_ERRLOG("io_device %p already registered (old:%s new:%s)\n",
			    io_device, tmp->name, dev->name);
		free(dev);
	}

	pthread_mutex_unlock(&g_devlist_mutex);
}

static void
_finish_unregister(void *arg)
{
	struct io_device *dev = arg;
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	assert(thread == dev->unregister_thread);

	SPDK_DEBUGLOG(thread, "Finishing unregistration of io_device %s (%p) on thread %s\n",
		      dev->name, dev->io_device, thread->name);

	assert(thread->pending_unregister_count > 0);
	thread->pending_unregister_count--;

	dev->unregister_cb(dev->io_device);
	free(dev);
}

static void
io_device_free(struct io_device *dev)
{
	if (dev->unregister_cb == NULL) {
		free(dev);
	} else {
		assert(dev->unregister_thread != NULL);
		SPDK_DEBUGLOG(thread, "io_device %s (%p) needs to unregister from thread %s\n",
			      dev->name, dev->io_device, dev->unregister_thread->name);
		spdk_thread_send_msg(dev->unregister_thread, _finish_unregister, dev);
	}
}

void
spdk_io_device_unregister(void *io_device, spdk_io_device_unregister_cb unregister_cb)
{
	struct io_device *dev;
	uint32_t refcnt;
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (!thread) {
		SPDK_ERRLOG("called from non-SPDK thread\n");
		assert(false);
		return;
	}

	pthread_mutex_lock(&g_devlist_mutex);
	dev = io_device_get(io_device);
	if (!dev) {
		SPDK_ERRLOG("io_device %p not found\n", io_device);
		assert(false);
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

	/* The for_each_count check differentiates the user attempting to unregister the
	 * device a second time, from the internal call to this function that occurs
	 * after the for_each_count reaches 0.
	 */
	if (dev->pending_unregister && dev->for_each_count > 0) {
		SPDK_ERRLOG("io_device %p already has a pending unregister\n", io_device);
		assert(false);
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

	dev->unregister_cb = unregister_cb;
	dev->unregister_thread = thread;

	if (dev->for_each_count > 0) {
		SPDK_WARNLOG("io_device %s (%p) has %u for_each calls outstanding\n",
			     dev->name, io_device, dev->for_each_count);
		dev->pending_unregister = true;
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

	dev->unregistered = true;
	RB_REMOVE(io_device_tree, &g_io_devices, dev);
	refcnt = dev->refcnt;
	pthread_mutex_unlock(&g_devlist_mutex);

	SPDK_DEBUGLOG(thread, "Unregistering io_device %s (%p) from thread %s\n",
		      dev->name, dev->io_device, thread->name);

	if (unregister_cb) {
		thread->pending_unregister_count++;
	}

	if (refcnt > 0) {
		/* defer deletion */
		return;
	}

	io_device_free(dev);
}

const char *
spdk_io_device_get_name(struct io_device *dev)
{
	return dev->name;
}

static struct spdk_io_channel *
thread_get_io_channel(struct spdk_thread *thread, struct io_device *dev)
{
	struct spdk_io_channel find = {};

	find.dev = dev;
	return RB_FIND(io_channel_tree, &thread->io_channels, &find);
}

struct spdk_io_channel *
spdk_get_io_channel(void *io_device)
{
	struct spdk_io_channel *ch;
	struct thread_link *thr_link;
	struct spdk_thread *thread;
	struct io_device *dev;
	int rc;
	bool do_remove_dev = false;

	pthread_mutex_lock(&g_devlist_mutex);
	dev = io_device_get(io_device);
	if (dev == NULL) {
		SPDK_ERRLOG("could not find io_device %p\n", io_device);
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	thread = _get_thread();
	if (!thread) {
		SPDK_ERRLOG("No thread allocated\n");
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	if (spdk_unlikely(thread->state == SPDK_THREAD_STATE_EXITED)) {
		SPDK_ERRLOG("Thread %s is marked as exited\n", thread->name);
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	ch = thread_get_io_channel(thread, dev);
	if (ch != NULL) {
		ch->ref++;

		SPDK_DEBUGLOG(thread, "Get io_channel %p for io_device %s (%p) on thread %s refcnt %u\n",
			      ch, dev->name, dev->io_device, thread->name, ch->ref);

		/*
		 * An I/O channel already exists for this device on this
		 *  thread, so return it.
		 */
		pthread_mutex_unlock(&g_devlist_mutex);
		spdk_trace_record(TRACE_THREAD_IOCH_GET, 0, 0,
				  (uint64_t)spdk_io_channel_get_ctx(ch), ch->ref);
		return ch;
	}

	ch = calloc(1, sizeof(*ch) + dev->ctx_size);
	if (ch == NULL) {
		SPDK_ERRLOG("could not calloc spdk_io_channel\n");
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	thr_link = calloc(1, sizeof(struct thread_link));
	if (thr_link == NULL) {
		free(ch);
		SPDK_ERRLOG("could not calloc thread_link\n");
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	ch->dev = dev;
	ch->destroy_cb = dev->destroy_cb;
	ch->thread = thread;
	ch->ref = 1;
	ch->destroy_ref = 0;
	RB_INSERT(io_channel_tree, &thread->io_channels, ch);

	SPDK_DEBUGLOG(thread, "Get io_channel %p for io_device %s (%p) on thread %s refcnt %u\n",
		      ch, dev->name, dev->io_device, thread->name, ch->ref);

	dev->refcnt++;

	thr_link->thread = thread;
	thr_link->id = thread->id;
	if (RB_INSERT(thread_link_tree, &dev->threads, thr_link)) {
		assert(false);
	}

	pthread_mutex_unlock(&g_devlist_mutex);

	rc = dev->create_cb(io_device, (uint8_t *)ch + sizeof(*ch));
	if (rc != 0) {
		pthread_mutex_lock(&g_devlist_mutex);
		RB_REMOVE(io_channel_tree, &ch->thread->io_channels, ch);
		dev->refcnt--;
		free(ch);
		RB_REMOVE(thread_link_tree, &dev->threads, thr_link);
		free(thr_link);
		SPDK_ERRLOG("could not create io_channel for io_device %s (%p): %s (rc=%d)\n",
			    dev->name, io_device, spdk_strerror(-rc), rc);
		if (dev->unregistered && dev->refcnt == 0) {
			/* During invokation of create_cb dev was unregistered, but was not removed due to refcnt */
			do_remove_dev = true;
		}
		pthread_mutex_unlock(&g_devlist_mutex);
		if (do_remove_dev) {
			io_device_free(dev);
		}
		return NULL;
	}

	spdk_trace_record(TRACE_THREAD_IOCH_GET, 0, 0, (uint64_t)spdk_io_channel_get_ctx(ch), 1);
	return ch;
}

static void
put_io_channel(void *arg)
{
	struct spdk_io_channel *ch = arg;
	bool do_remove_dev = true;
	struct spdk_thread *thread;
	struct thread_link *thr_link, *ptmp;

	thread = spdk_get_thread();
	if (!thread) {
		SPDK_ERRLOG("called from non-SPDK thread\n");
		assert(false);
		return;
	}

	SPDK_DEBUGLOG(thread,
		      "Releasing io_channel %p for io_device %s (%p) on thread %s\n",
		      ch, ch->dev->name, ch->dev->io_device, thread->name);

	assert(ch->thread == thread);

	ch->destroy_ref--;

	if (ch->ref > 0 || ch->destroy_ref > 0) {
		/*
		 * Another reference to the associated io_device was requested
		 *  after this message was sent but before it had a chance to
		 *  execute.
		 */
		return;
	}

	pthread_mutex_lock(&g_devlist_mutex);
	RB_REMOVE(io_channel_tree, &ch->thread->io_channels, ch);
	RB_FOREACH_SAFE(thr_link, thread_link_tree, &ch->dev->threads, ptmp) {
		if (thr_link->thread == thread) {
			RB_REMOVE(thread_link_tree, &ch->dev->threads, thr_link);
			free(thr_link);
			break;
		}
	}
	pthread_mutex_unlock(&g_devlist_mutex);

	/* Don't hold the devlist mutex while the destroy_cb is called. */
	ch->destroy_cb(ch->dev->io_device, spdk_io_channel_get_ctx(ch));

	pthread_mutex_lock(&g_devlist_mutex);
	ch->dev->refcnt--;

	if (!ch->dev->unregistered) {
		do_remove_dev = false;
	}

	if (ch->dev->refcnt > 0) {
		do_remove_dev = false;
	}

	pthread_mutex_unlock(&g_devlist_mutex);

	if (do_remove_dev) {
		io_device_free(ch->dev);
	}
	free(ch);
}

void
spdk_put_io_channel(struct spdk_io_channel *ch)
{
	struct spdk_thread *thread;

	spdk_trace_record(TRACE_THREAD_IOCH_PUT, 0, 0,
			  (uint64_t)spdk_io_channel_get_ctx(ch), ch->ref);

	thread = spdk_get_thread();
	if (!thread) {
		SPDK_ERRLOG("called from non-SPDK thread\n");
		assert(false);
		return;
	}

	if (ch->thread != thread) {
		wrong_thread(__func__, "ch", ch->thread, thread);
		return;
	}

	SPDK_DEBUGLOG(thread,
		      "Putting io_channel %p for io_device %s (%p) on thread %s refcnt %u\n",
		      ch, ch->dev->name, ch->dev->io_device, thread->name, ch->ref);

	ch->ref--;

	if (ch->ref == 0) {
		ch->destroy_ref++;
		spdk_thread_send_msg(thread, put_io_channel, ch);
	}
}

struct spdk_io_channel *
spdk_io_channel_ref(struct spdk_io_channel *ch)
{
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (spdk_unlikely(ch->thread != thread)) {
		wrong_thread(__func__, "ch", ch->thread, thread);
		return NULL;
	}

	ch->ref++;
	return ch;
}

struct spdk_io_channel *
spdk_io_channel_from_ctx(void *ctx)
{
	return (struct spdk_io_channel *)((uint8_t *)ctx - sizeof(struct spdk_io_channel));
}

struct spdk_thread *
spdk_io_channel_get_thread(struct spdk_io_channel *ch)
{
	return ch->thread;
}

void *
spdk_io_channel_get_io_device(struct spdk_io_channel *ch)
{
	return ch->dev->io_device;
}

const char *
spdk_io_channel_get_io_device_name(struct spdk_io_channel *ch)
{
	return spdk_io_device_get_name(ch->dev);
}

int
spdk_io_channel_get_ref_count(struct spdk_io_channel *ch)
{
	return ch->ref;
}

struct spdk_io_channel_iter {
	void *io_device;
	struct io_device *dev;
	spdk_channel_msg fn;
	int status;
	void *ctx;
	struct spdk_io_channel *ch;

	struct spdk_thread *cur_thread;

	struct spdk_thread *orig_thread;
	spdk_channel_for_each_cpl cpl;
};

void *
spdk_io_channel_iter_get_io_device(struct spdk_io_channel_iter *i)
{
	return i->io_device;
}

struct spdk_io_channel *
spdk_io_channel_iter_get_channel(struct spdk_io_channel_iter *i)
{
	return i->ch;
}

void *
spdk_io_channel_iter_get_ctx(struct spdk_io_channel_iter *i)
{
	return i->ctx;
}

static void
_call_completion(void *ctx)
{
	struct spdk_io_channel_iter *i = ctx;

	assert(i->orig_thread->for_each_count > 0);
	i->orig_thread->for_each_count--;

	if (i->cpl != NULL) {
		i->cpl(i, i->status);
	}
	free(i);
}

static void
_call_channel(void *ctx)
{
	struct spdk_io_channel_iter *i = ctx;

	/*
	 * It is possible that the channel was deleted before this
	 *  message had a chance to execute.  If so, skip calling
	 *  the fn() on this thread.
	 */
	pthread_mutex_lock(&g_devlist_mutex);
	i->ch = thread_get_io_channel(i->cur_thread, i->dev);
	pthread_mutex_unlock(&g_devlist_mutex);

	if (i->ch) {
		i->fn(i);
	} else {
		spdk_for_each_channel_continue(i, 0);
	}
}

void
spdk_for_each_channel(void *io_device, spdk_channel_msg fn, void *ctx,
		      spdk_channel_for_each_cpl cpl)
{
	struct spdk_io_channel_iter *i;
	struct thread_link *thr_link;

	i = calloc(1, sizeof(*i));
	if (!i) {
		SPDK_ERRLOG("Unable to allocate iterator\n");
		assert(false);
		return;
	}

	i->io_device = io_device;
	i->fn = fn;
	i->ctx = ctx;
	i->cpl = cpl;
	i->orig_thread = _get_thread();

	i->orig_thread->for_each_count++;

	pthread_mutex_lock(&g_devlist_mutex);
	i->dev = io_device_get(io_device);
	if (i->dev == NULL) {
		SPDK_ERRLOG("could not find io_device %p\n", io_device);
		assert(false);
		i->status = -ENODEV;
		goto end;
	}

	/* Do not allow new for_each operations if we are already waiting to unregister
	 * the device for other for_each operations to complete.
	 */
	if (i->dev->pending_unregister) {
		SPDK_ERRLOG("io_device %p has a pending unregister\n", io_device);
		i->status = -ENODEV;
		goto end;
	}

	thr_link = RB_MIN(thread_link_tree, &i->dev->threads);
	if (thr_link != NULL) {
		i->dev->for_each_count++;
		i->cur_thread = thr_link->thread;
		spdk_thread_send_msg(i->cur_thread, _call_channel, i);
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

end:
	pthread_mutex_unlock(&g_devlist_mutex);

	spdk_thread_send_msg(i->orig_thread, _call_completion, i);
}

static void
__pending_unregister(void *arg)
{
	struct io_device *dev = arg;

	assert(dev->pending_unregister);
	assert(dev->for_each_count == 0);
	spdk_io_device_unregister(dev->io_device, dev->unregister_cb);
}

static struct spdk_thread *
io_dev_get_next_thread(struct io_device *dev, struct spdk_thread *thread)
{
	struct thread_link find = {}, *res;

	find.id = thread->id + 1;
	res = RB_NFIND(thread_link_tree, &dev->threads, &find);
	return res ? res->thread : NULL;
}

void
spdk_for_each_channel_continue(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_thread *thread;
	struct io_device *dev;

	assert(i->cur_thread == spdk_get_thread());

	i->status = status;

	pthread_mutex_lock(&g_devlist_mutex);
	dev = i->dev;
	if (status) {
		goto end;
	}

	thread = io_dev_get_next_thread(i->dev, i->cur_thread);
	if (thread != NULL) {
		i->cur_thread = thread;
		spdk_thread_send_msg(i->cur_thread, _call_channel, i);
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

end:
	dev->for_each_count--;
	i->ch = NULL;
	pthread_mutex_unlock(&g_devlist_mutex);

	spdk_thread_send_msg(i->orig_thread, _call_completion, i);

	pthread_mutex_lock(&g_devlist_mutex);
	if (dev->pending_unregister && dev->for_each_count == 0) {
		spdk_thread_send_msg(dev->unregister_thread, __pending_unregister, dev);
	}
	pthread_mutex_unlock(&g_devlist_mutex);
}

static void
thread_interrupt_destroy(struct spdk_thread *thread)
{
	struct spdk_fd_group *fgrp = thread->fgrp;

	SPDK_INFOLOG(thread, "destroy fgrp for thread (%s)\n", thread->name);

	if (thread->msg_fd < 0) {
		return;
	}

	spdk_fd_group_remove(fgrp, thread->msg_fd);
	close(thread->msg_fd);
	thread->msg_fd = -1;

	spdk_fd_group_destroy(fgrp);
	thread->fgrp = NULL;
}

#ifdef __linux__
static int
thread_interrupt_msg_process(void *arg)
{
	struct spdk_thread *thread = arg;
	struct spdk_thread *orig_thread;
	uint32_t msg_count;
	spdk_msg_fn critical_msg;
	int rc = 0;
	uint64_t notify = 1;

	assert(spdk_interrupt_mode_is_enabled());

	orig_thread = spdk_get_thread();
	spdk_set_thread(thread);

	critical_msg = thread->critical_msg;
	if (spdk_unlikely(critical_msg != NULL)) {
		critical_msg(NULL);
		thread->critical_msg = NULL;
		rc = 1;
	}

	msg_count = msg_queue_run_batch(thread, 0);
	if (msg_count) {
		rc = 1;
	}

	SPIN_ASSERT(thread->lock_count == 0, SPIN_ERR_HOLD_DURING_SWITCH);
	if (spdk_unlikely(!thread->in_interrupt)) {
		/* The thread transitioned to poll mode in a msg during the above processing.
		 * Clear msg_fd since thread messages will be polled directly in poll mode.
		 */
		rc = read(thread->msg_fd, &notify, sizeof(notify));
		if (rc < 0 && errno != EAGAIN) {
			SPDK_ERRLOG("failed to acknowledge msg queue: %s.\n", spdk_strerror(errno));
		}
	}

	spdk_set_thread(orig_thread);
	return rc;
}

static int
thread_interrupt_create(struct spdk_thread *thread)
{
	struct spdk_event_handler_opts opts = {};
	int rc;

	SPDK_INFOLOG(thread, "Create fgrp for thread (%s)\n", thread->name);

	rc = spdk_fd_group_create(&thread->fgrp);
	if (rc) {
		thread->msg_fd = -1;
		return rc;
	}

	thread->msg_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (thread->msg_fd < 0) {
		rc = -errno;
		spdk_fd_group_destroy(thread->fgrp);
		thread->fgrp = NULL;

		return rc;
	}

	spdk_fd_group_get_default_event_handler_opts(&opts, sizeof(opts));
	opts.fd_type = SPDK_FD_TYPE_EVENTFD;

	return SPDK_FD_GROUP_ADD_EXT(thread->fgrp, thread->msg_fd,
				     thread_interrupt_msg_process, thread, &opts);
}
#else
static int
thread_interrupt_create(struct spdk_thread *thread)
{
	return -ENOTSUP;
}
#endif

static int
_interrupt_wrapper(void *ctx)
{
	struct spdk_interrupt *intr = ctx;
	struct spdk_thread *orig_thread, *thread;
	int rc;

	orig_thread = spdk_get_thread();
	thread = intr->thread;

	spdk_set_thread(thread);

	SPDK_DTRACE_PROBE4(interrupt_fd_process, intr->name, intr->efd,
			   intr->fn, intr->arg);

	rc = intr->fn(intr->arg);

	SPIN_ASSERT(thread->lock_count == 0, SPIN_ERR_HOLD_DURING_SWITCH);

	spdk_set_thread(orig_thread);

	return rc;
}

struct spdk_interrupt *
spdk_interrupt_register(int efd, spdk_interrupt_fn fn,
			void *arg, const char *name)
{
	return spdk_interrupt_register_for_events(efd, SPDK_INTERRUPT_EVENT_IN, fn, arg, name);
}

struct spdk_interrupt *
spdk_interrupt_register_for_events(int efd, uint32_t events, spdk_interrupt_fn fn, void *arg,
				   const char *name)
{
	struct spdk_event_handler_opts opts = {};

	spdk_fd_group_get_default_event_handler_opts(&opts, sizeof(opts));
	opts.events = events;
	opts.fd_type = SPDK_FD_TYPE_DEFAULT;

	return spdk_interrupt_register_ext(efd, fn, arg, name, &opts);
}

static struct spdk_interrupt *
alloc_interrupt(int efd, struct spdk_fd_group *fgrp, spdk_interrupt_fn fn, void *arg,
		const char *name)
{
	struct spdk_thread *thread;
	struct spdk_interrupt *intr;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return NULL;
	}

	if (spdk_unlikely(thread->state != SPDK_THREAD_STATE_RUNNING)) {
		SPDK_ERRLOG("thread %s is marked as exited\n", thread->name);
		return NULL;
	}

	intr = calloc(1, sizeof(*intr));
	if (intr == NULL) {
		SPDK_ERRLOG("Interrupt handler allocation failed\n");
		return NULL;
	}

	if (name) {
		snprintf(intr->name, sizeof(intr->name), "%s", name);
	} else {
		snprintf(intr->name, sizeof(intr->name), "%p", fn);
	}

	assert(efd < 0 || fgrp == NULL);
	intr->efd = efd;
	intr->fgrp = fgrp;
	intr->thread = thread;
	intr->fn = fn;
	intr->arg = arg;

	return intr;
}

struct spdk_interrupt *
spdk_interrupt_register_ext(int efd, spdk_interrupt_fn fn, void *arg, const char *name,
			    struct spdk_event_handler_opts *opts)
{
	struct spdk_interrupt *intr;
	int ret;

	intr = alloc_interrupt(efd, NULL, fn, arg, name);
	if (intr == NULL) {
		return NULL;
	}

	ret = spdk_fd_group_add_ext(intr->thread->fgrp, efd,
				    _interrupt_wrapper, intr, intr->name, opts);
	if (ret != 0) {
		SPDK_ERRLOG("thread %s: failed to add fd %d: %s\n",
			    intr->thread->name, efd, spdk_strerror(-ret));
		free(intr);
		return NULL;
	}

	return intr;
}

static int
interrupt_fd_group_wrapper(void *wrap_ctx, spdk_fd_fn cb_fn, void *cb_ctx)
{
	struct spdk_interrupt *intr = wrap_ctx;
	struct spdk_thread *orig_thread, *thread;
	int rc;

	orig_thread = spdk_get_thread();
	thread = intr->thread;

	spdk_set_thread(thread);
	rc = cb_fn(cb_ctx);
	SPIN_ASSERT(thread->lock_count == 0, SPIN_ERR_HOLD_DURING_SWITCH);
	spdk_set_thread(orig_thread);

	return rc;
}

struct spdk_interrupt *
spdk_interrupt_register_fd_group(struct spdk_fd_group *fgrp, const char *name)
{
	struct spdk_interrupt *intr;
	int rc;

	intr = alloc_interrupt(-1, fgrp, NULL, NULL, name);
	if (intr == NULL) {
		return NULL;
	}

	rc = spdk_fd_group_set_wrapper(fgrp, interrupt_fd_group_wrapper, intr);
	if (rc != 0) {
		SPDK_ERRLOG("thread %s: failed to set wrapper for fd_group %d: %s\n",
			    intr->thread->name, spdk_fd_group_get_fd(fgrp), spdk_strerror(-rc));
		free(intr);
		return NULL;
	}

	rc = spdk_fd_group_nest(intr->thread->fgrp, fgrp);
	if (rc != 0) {
		SPDK_ERRLOG("thread %s: failed to nest fd_group %d: %s\n",
			    intr->thread->name, spdk_fd_group_get_fd(fgrp), spdk_strerror(-rc));
		spdk_fd_group_set_wrapper(fgrp, NULL, NULL);
		free(intr);
		return NULL;
	}

	return intr;
}

void
spdk_interrupt_unregister(struct spdk_interrupt **pintr)
{
	struct spdk_thread *thread;
	struct spdk_interrupt *intr;

	intr = *pintr;
	if (intr == NULL) {
		return;
	}

	*pintr = NULL;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return;
	}

	if (intr->thread != thread) {
		wrong_thread(__func__, intr->name, intr->thread, thread);
		return;
	}

	if (intr->fgrp != NULL) {
		assert(intr->efd < 0);
		spdk_fd_group_unnest(thread->fgrp, intr->fgrp);
		spdk_fd_group_set_wrapper(thread->fgrp, NULL, NULL);
	} else {
		spdk_fd_group_remove(thread->fgrp, intr->efd);
	}

	free(intr);
}

int
spdk_interrupt_set_event_types(struct spdk_interrupt *intr,
			       enum spdk_interrupt_event_types event_types)
{
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return -EINVAL;
	}

	if (intr->thread != thread) {
		wrong_thread(__func__, intr->name, intr->thread, thread);
		return -EINVAL;
	}

	if (intr->efd < 0) {
		assert(false);
		return -EINVAL;
	}

	return spdk_fd_group_event_modify(thread->fgrp, intr->efd, event_types);
}

int
spdk_thread_get_interrupt_fd(struct spdk_thread *thread)
{
	return spdk_fd_group_get_fd(thread->fgrp);
}

struct spdk_fd_group *
spdk_thread_get_interrupt_fd_group(struct spdk_thread *thread)
{
	return thread->fgrp;
}

static bool g_interrupt_mode = false;

int
spdk_interrupt_mode_enable(void)
{
	/* It must be called once prior to initializing the threading library.
	 * g_spdk_msg_mempool will be valid if thread library is initialized.
	 */
	if (g_spdk_msg_mempool) {
		SPDK_ERRLOG("Failed due to threading library is already initialized.\n");
		return -1;
	}

#ifdef __linux__
	SPDK_NOTICELOG("Set SPDK running in interrupt mode.\n");
	g_interrupt_mode = true;
	return 0;
#else
	SPDK_ERRLOG("SPDK interrupt mode supports only Linux platform now.\n");
	g_interrupt_mode = false;
	return -ENOTSUP;
#endif
}

bool
spdk_interrupt_mode_is_enabled(void)
{
	return g_interrupt_mode;
}

#define SSPIN_DEBUG_STACK_FRAMES 16

struct sspin_stack {
	void *addrs[SSPIN_DEBUG_STACK_FRAMES];
	uint32_t depth;
};

struct spdk_spinlock_internal {
	struct sspin_stack init_stack;
	struct sspin_stack lock_stack;
	struct sspin_stack unlock_stack;
};

static void
sspin_init_internal(struct spdk_spinlock *sspin)
{
#ifdef DEBUG
	sspin->internal = calloc(1, sizeof(*sspin->internal));
#endif
}

static void
sspin_fini_internal(struct spdk_spinlock *sspin)
{
#ifdef DEBUG
	free(sspin->internal);
	sspin->internal = NULL;
#endif
}

#if defined(DEBUG) && defined(SPDK_HAVE_EXECINFO_H)
#define SSPIN_GET_STACK(sspin, which) \
	do { \
		if (sspin->internal != NULL) { \
			struct sspin_stack *stack = &sspin->internal->which ## _stack; \
			stack->depth = backtrace(stack->addrs, SPDK_COUNTOF(stack->addrs)); \
		} \
	} while (0)
#else
#define SSPIN_GET_STACK(sspin, which) do { } while (0)
#endif

static void
sspin_stack_print(const char *title, const struct sspin_stack *sspin_stack)
{
#ifdef SPDK_HAVE_EXECINFO_H
	char **stack;
	size_t i;

	stack = backtrace_symbols(sspin_stack->addrs, sspin_stack->depth);
	if (stack == NULL) {
		SPDK_ERRLOG("Out of memory while allocate stack for %s\n", title);
		return;
	}
	SPDK_ERRLOG("  %s:\n", title);
	for (i = 0; i < sspin_stack->depth; i++) {
		/*
		 * This does not print line numbers. In gdb, use something like "list *0x444b6b" or
		 * "list *sspin_stack->addrs[0]".  Or more conveniently, load the spdk gdb macros
		 * and use use "print *sspin" or "print sspin->internal.lock_stack".  See
		 * gdb_macros.md in the docs directory for details.
		 */
		SPDK_ERRLOG("    #%" PRIu64 ": %s\n", i, stack[i]);
	}
	free(stack);
#endif /* SPDK_HAVE_EXECINFO_H */
}

static void
sspin_stacks_print(const struct spdk_spinlock *sspin)
{
	if (sspin->internal == NULL) {
		return;
	}
	SPDK_ERRLOG("spinlock %p\n", sspin);
	sspin_stack_print("Lock initialized at", &sspin->internal->init_stack);
	sspin_stack_print("Last locked at", &sspin->internal->lock_stack);
	sspin_stack_print("Last unlocked at", &sspin->internal->unlock_stack);
}

void
spdk_spin_init(struct spdk_spinlock *sspin)
{
	int rc;

	memset(sspin, 0, sizeof(*sspin));
	rc = pthread_spin_init(&sspin->spinlock, PTHREAD_PROCESS_PRIVATE);
	SPIN_ASSERT_LOG_STACKS(rc == 0, SPIN_ERR_PTHREAD, sspin);
	sspin_init_internal(sspin);
	SSPIN_GET_STACK(sspin, init);
	sspin->initialized = true;
}

void
spdk_spin_destroy(struct spdk_spinlock *sspin)
{
	int rc;

	SPIN_ASSERT_LOG_STACKS(!sspin->destroyed, SPIN_ERR_DESTROYED, sspin);
	SPIN_ASSERT_LOG_STACKS(sspin->initialized, SPIN_ERR_NOT_INITIALIZED, sspin);
	SPIN_ASSERT_LOG_STACKS(sspin->thread == NULL, SPIN_ERR_LOCK_HELD, sspin);

	rc = pthread_spin_destroy(&sspin->spinlock);
	SPIN_ASSERT_LOG_STACKS(rc == 0, SPIN_ERR_PTHREAD, sspin);

	sspin_fini_internal(sspin);
	sspin->initialized = false;
	sspin->destroyed = true;
}

void
spdk_spin_lock(struct spdk_spinlock *sspin)
{
	struct spdk_thread *thread = spdk_get_thread();
	int rc;

	SPIN_ASSERT_LOG_STACKS(!sspin->destroyed, SPIN_ERR_DESTROYED, sspin);
	SPIN_ASSERT_LOG_STACKS(sspin->initialized, SPIN_ERR_NOT_INITIALIZED, sspin);
	SPIN_ASSERT_LOG_STACKS(thread != NULL, SPIN_ERR_NOT_SPDK_THREAD, sspin);
	SPIN_ASSERT_LOG_STACKS(thread != sspin->thread, SPIN_ERR_DEADLOCK, sspin);

	rc = pthread_spin_lock(&sspin->spinlock);
	SPIN_ASSERT_LOG_STACKS(rc == 0, SPIN_ERR_PTHREAD, sspin);

	sspin->thread = thread;
	sspin->thread->lock_count++;

	SSPIN_GET_STACK(sspin, lock);
}

void
spdk_spin_unlock(struct spdk_spinlock *sspin)
{
	struct spdk_thread *thread = spdk_get_thread();
	int rc;

	SPIN_ASSERT_LOG_STACKS(!sspin->destroyed, SPIN_ERR_DESTROYED, sspin);
	SPIN_ASSERT_LOG_STACKS(sspin->initialized, SPIN_ERR_NOT_INITIALIZED, sspin);
	SPIN_ASSERT_LOG_STACKS(thread != NULL, SPIN_ERR_NOT_SPDK_THREAD, sspin);
	SPIN_ASSERT_LOG_STACKS(thread == sspin->thread, SPIN_ERR_WRONG_THREAD, sspin);

	SPIN_ASSERT_LOG_STACKS(thread->lock_count > 0, SPIN_ERR_LOCK_COUNT, sspin);
	thread->lock_count--;
	sspin->thread = NULL;

	SSPIN_GET_STACK(sspin, unlock);

	rc = pthread_spin_unlock(&sspin->spinlock);
	SPIN_ASSERT_LOG_STACKS(rc == 0, SPIN_ERR_PTHREAD, sspin);
}

bool
spdk_spin_held(struct spdk_spinlock *sspin)
{
	struct spdk_thread *thread = spdk_get_thread();

	SPIN_ASSERT_RETURN(thread != NULL, SPIN_ERR_NOT_SPDK_THREAD, false);

	return sspin->thread == thread;
}

void
spdk_thread_register_post_poller_handler(spdk_post_poller_fn fn, void *fn_arg)
{
	struct spdk_thread *thr;

	thr = _get_thread();
	assert(thr);
	if (spdk_unlikely(thr->num_pp_handlers == SPDK_THREAD_MAX_POST_POLLER_HANDLERS)) {
		SPDK_ERRLOG("Too many handlers registered");
		return;
	}

	thr->pp_handlers[thr->num_pp_handlers].fn = fn;
	thr->pp_handlers[thr->num_pp_handlers].fn_arg = fn_arg;
	thr->num_pp_handlers++;
}

SPDK_LOG_REGISTER_COMPONENT(thread)
