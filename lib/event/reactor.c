/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK Reactor 코어 구현 (reactor.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 의 핵심 실행 모델인 "lockless run-to-completion reactor" 를
 * 구현한다. SPDK 의 기본 디자인은 인터럽트와 컨텍스트 스위치를 회피하는
 * polled-mode 이며, 1 코어 = 1 reactor = 1 OS pthread (pinned) 의 1:1 대응
 * 으로 동작한다. 각 reactor 는 자신에게 배치된 spdk_thread (lw_thread) 들을
 * polling 루프에서 라운드로빈으로 호출하고, 다른 reactor 로부터 수신한
 * spdk_event 들을 SPSC/MPSC ring(spdk_ring) 에서 꺼내 실행한다.
 * 인터럽트 모드(interrupt mode) 도 옵션으로 지원하여 epoll/eventfd 기반
 * 이벤트 대기 모델로도 동작 가능하다 (idle CPU 100% 회피용).
 * 또한 코어 간 부하분산을 담당하는 scheduler 모듈과 코어 주파수를
 * 조절하는 governor 모듈의 등록·조회 진입점도 이 파일에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 부팅 흐름은 spdk_app_start() → spdk_reactors_init() → spdk_reactors_start()
 * → 각 코어에서 reactor_run() 무한 루프 진입. 이 파일은 그 중 reactors_init/
 * start/fini, reactor_run, _reactor_run, event_queue_run_batch, scheduler 페이즈
 * 콜백들을 담당한다. 호출 체인은 위로는 lib/event/app.c (spdk_app_start) 가
 * spdk_reactors_init/start 를 호출하고, 아래로는 lib/thread (spdk_thread_poll,
 * spdk_thread_create) 와 lib/env_dpdk (spdk_env_thread_launch_pinned, spdk_ring,
 * spdk_mempool) 를 사용한다. 실행 컨텍스트는 호스트 유저스페이스 단일 OS 프로세스
 * 내의 pthread 들이며, 각 reactor pthread 는 spdk_env_thread_launch_pinned 에
 * 의해 특정 lcore 에 핀되어 다른 코어로 마이그레이션되지 않는다. 메인 코어는
 * 별도 pthread 를 띄우지 않고 호출 컨텍스트(현재 스레드) 자체가 reactor 가 된다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/thread: spdk_thread (poller, message, fd_group) 를 reactor 가 polling
 *   호출. spdk_thread_create 시 thread_lib 가 reactor_thread_op (NEW/RESCHED)
 *   콜백으로 _reactor_schedule_thread 를 호출해 코어 배치를 결정.
 * - lib/event/app.c: 이 파일의 spdk_reactors_init/start/stop/fini 를 호출.
 * - lib/env_dpdk: spdk_ring(MP-SC) 를 events 큐로, spdk_mempool 를 spdk_event
 *   할당 풀로 사용. spdk_env_thread_launch_pinned 로 reactor pthread 핀.
 * - lib/util/cpuset: 코어 마스크 (notify_cpuset, isolated_core_mask) 표현.
 * - lib/util/fd_group, eventfd: interrupt mode 에서 events_fd/resched_fd 로
 *   다른 reactor 가 깨울 수 있도록 사용 (epoll → poll/wait).
 * - scheduler/governor 플러그인: g_scheduler_list/g_governor_list 에 TAILQ 로
 *   등록되며, scheduler_static.c 등이 spdk_scheduler_register 호출.
 * - 핵심 자료구조 spdk_reactor 는 lib/event/event_internal.h 와
 *   include/spdk_internal/event.h 에 정의되어 있고, lw_thread 는
 *   include/spdk_internal/thread.h 에 정의된다 (이 파일은 사용자).
 * 데이터 흐름: spdk_event_call() → 목적지 reactor->events ring enqueue
 *  (필요 시 events_fd write 로 깨움) → 그 reactor 의 _reactor_run() 안의
 *  event_queue_run_batch() 가 dequeue 후 event->fn 실행 → spdk_thread_poll()
 *  로 lw_thread 들을 라운드로빈 polling.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_reactors_init / spdk_reactors_fini: g_reactors 배열 할당·해제,
 *   spdk_thread_lib_init_ext 호출, 이벤트 mempool 생성.
 * - spdk_reactors_start / _reactors_stop / spdk_reactors_stop: 각 코어에
 *   reactor pthread 핀 후 reactor_run 진입, 종료 시 events_fd 로 깨워 루프 탈출.
 * - reactor_run / _reactor_run / reactor_interrupt_run: 핵심 polling 루프.
 *   _reactor_run 은 polling 모드 한 라운드 (events → threads).
 * - event_queue_run_batch: SPDK_EVENT_BATCH_SIZE(8) 만큼 events ring dequeue
 *   후 event->fn 콜백 실행. mempool 로 반환.
 * - spdk_event_allocate / spdk_event_call: cross-reactor 메시지 전송 API.
 *   mempool 에서 event 를 받아 ring 에 push (interrupt mode 면 eventfd write).
 * - _reactor_schedule_thread / _schedule_thread / reactor_thread_op:
 *   spdk_thread 신규 생성/재스케줄 시 코어 배치 결정 (라운드로빈 + cpumask).
 * - _reactors_scheduler_gather_metrics → _reactors_scheduler_balance →
 *   _reactors_scheduler_update_core_mode → _reactors_scheduler_fini:
 *   주기적으로 g_scheduler_period_in_tsc 마다 메인 reactor 가 트리거하는
 *   3-phase 스케줄링 파이프라인 (메트릭 수집 → balance → 적용).
 * - spdk_reactor_set_interrupt_mode / _reactor_set_interrupt_mode 등:
 *   reactor 의 polling↔interrupt 모드 전환 상태머신.
 * - spdk_for_each_reactor / on_reactor / end_reactor: 모든 코어를 순회하며
 *   콜백을 실행하고 마지막에 완료 콜백을 호출하는 비동기 chain.
 * - struct spdk_reactor: 핵심 per-CPU 상태 (lcore, threads list, events ring,
 *   tsc 통계, interrupt mode 상태, fd_group, eventfd 들). 64B 정렬 필수.
 */

#include "spdk/stdinc.h"        /* [한국어] SPDK 의 표준 라이브러리 통합 헤더 — stdio/stdlib/string/errno 등 platform-neutral 한 표준 C 헤더를 한 번에 가져옴. SPDK 모든 .c 파일이 첫 줄에 포함하는 관습. */
#include "spdk/likely.h"        /* [한국어] spdk_likely / spdk_unlikely 분기 힌트 매크로 — GCC __builtin_expect 래퍼. polling 루프 hot-path 에서 분기 예측 최적화에 사용. */

#include "event_internal.h"     /* [한국어] event 라이브러리 내부 전용 선언 (lib/event/event_internal.h). spdk_reactor 구조체 정의가 여기에 있음 — 외부에 노출되지 않는 reactor 의 내부 필드를 다루기 위함. */

#include "spdk_internal/event.h"        /* [한국어] event 서브시스템의 internal API (스케줄러/거버너 등록 인터페이스, app start/stop 헬퍼). 일반 사용자에는 비공개지만 SPDK 내부 모듈 간 공유. */
#include "spdk_internal/usdt.h"         /* [한국어] User-Statically-Defined Tracing 매크로 (SPDK_DTRACE_PROBE*). DTrace/SystemTap 등으로 event 실행 등을 외부에서 트레이싱할 수 있게 함. */

#include "spdk/log.h"           /* [한국어] SPDK_ERRLOG / SPDK_NOTICELOG / SPDK_DEBUGLOG 매크로 — 컴포넌트 단위 로깅 유틸. SPDK_LOG_REGISTER_COMPONENT(reactor) 가 파일 끝에 있음. */
#include "spdk/thread.h"        /* [한국어] spdk_thread / poller / message API — reactor 가 polling 호출하는 spdk_thread_poll, spdk_thread_create, spdk_thread_send_msg 등을 사용. */
#include "spdk/env.h"           /* [한국어] DPDK EAL 추상화 API — spdk_ring (MPSC ring), spdk_mempool, spdk_env_thread_launch_pinned (코어 핀), spdk_get_ticks (TSC), spdk_cpuset 등 사용. */
#include "spdk/util.h"          /* [한국어] SPDK_COUNTOF, SPDK_SEC_TO_USEC 등 일반 유틸 매크로. 단위 변환·배열 크기 계산 용. */
#include "spdk/scheduler.h"     /* [한국어] spdk_scheduler / spdk_governor 공개 인터페이스. _scheduler_find, scheduler->balance, scheduler->init/deinit 등이 호출됨. */
#include "spdk/string.h"        /* [한국어] spdk_strerror — errno 를 안전히 문자열로 변환 (strerror 의 thread-safe 래퍼). 에러 로그에서 사용. */
#include "spdk/fd_group.h"      /* [한국어] epoll 추상화 (spdk_fd_group_create/nest/wait). interrupt mode 에서 reactor 가 events_fd/resched_fd 를 epoll 로 대기하는 데 사용. */
#include "spdk/trace.h"         /* [한국어] SPDK trace 인프라 — spdk_trace_record, spdk_trace_register_owner_type. 스케줄러 페이즈를 트레이싱. */
#include "spdk_internal/trace_defs.h"   /* [한국어] TRACE_SCHEDULER_* tpoint id 정의. trace 등록 시 사용하는 enum 상수. */

#ifdef __linux__               /* [한국어] Linux 한정 헤더 (prctl, eventfd). interrupt mode 와 thread name 설정에 사용. */
#include <sys/prctl.h>          /* [한국어] prctl(PR_SET_NAME, ...) — POSIX thread 이름을 "reactor_<lcore>" 로 설정해 top/htop 등에서 식별 가능하게. */
#include <sys/eventfd.h>        /* [한국어] eventfd(2) — interrupt mode 에서 다른 reactor 가 깨우기 위한 file descriptor 생성. EFD_NONBLOCK | EFD_CLOEXEC 사용. */
#endif

#ifdef __FreeBSD__             /* [한국어] FreeBSD 한정 — pthread_set_name_np 가 다른 헤더에 있음. */
#include <pthread_np.h>         /* [한국어] FreeBSD non-portable pthread 확장 — 스레드 이름 설정용. */
#endif

#define SPDK_EVENT_BATCH_SIZE		8       /* [한국어] event_queue_run_batch 가 한 번에 처리할 spdk_event 최대 개수. 8개씩 묶어 처리해 polling 루프의 캐시 효율과 latency 균형을 맞춤. 너무 크면 thread polling 지연, 작으면 ring API 호출 오버헤드 증가. */

static struct spdk_reactor *g_reactors;          /* [한국어] 모든 reactor 의 per-CPU 배열 (포인터). spdk_reactors_init 에서 posix_memalign(64) 로 할당, lcore 별로 reactor_construct 됨. spdk_reactor_get(lcore) 가 &g_reactors[lcore] 를 반환. */
static uint32_t g_reactor_count;                 /* [한국어] g_reactors 배열의 크기 = spdk_env_get_last_core() + 1. lcore 인덱스 상한 검증에 사용. */
static struct spdk_cpuset g_reactor_core_mask;   /* [한국어] 실제로 reactor 가 동작 중인 코어들의 비트맵. spdk_reactors_start 에서 launch_pinned 성공한 코어를 set. spdk_app_get_core_mask() 의 백킹 스토리지. */
static enum spdk_reactor_state	g_reactor_state = SPDK_REACTOR_STATE_UNINITIALIZED;     /* [한국어] reactor 시스템 전체의 상태머신. UNINIT → INITIALIZED → RUNNING → EXITING → SHUTDOWN. reactor_run() 메인 루프가 RUNNING 상태일 때만 polling 을 계속함. */

static bool g_framework_context_switch_monitor_enabled = true;  /* [한국어] reactor 가 1초마다 getrusage 로 nvcsw/nivcsw 를 찍을지 여부. polling 모델에서 컨텍스트 스위치가 발생했다면 비정상 신호이므로 가시성 제공. 멀티스레드 race 가능하지만 토글 정도는 무해. */

static struct spdk_mempool *g_spdk_event_mempool = NULL;        /* [한국어] spdk_event 객체 할당 풀 (DPDK rte_mempool 기반). EVENT_MSG_MEMPOOL_SIZE = 16383 개. spdk_event_allocate 가 get, event_queue_run_batch 가 put_bulk. 모든 reactor 가 공유 (lockless mempool). */

TAILQ_HEAD(, spdk_scheduler) g_scheduler_list           /* [한국어] 등록된 모든 scheduler 플러그인의 TAILQ. spdk_scheduler_register 로 추가, _scheduler_find 가 이름 조회. */
	= TAILQ_HEAD_INITIALIZER(g_scheduler_list);

static struct spdk_scheduler *g_scheduler = NULL;       /* [한국어] 현재 활성 scheduler. spdk_scheduler_set 으로 변경. _reactors_scheduler_balance 가 ->balance() 호출. NULL 이면 스케줄링 비활성. */
static struct spdk_reactor *g_scheduling_reactor;       /* [한국어] 스케줄링 페이즈를 트리거할 책임을 진 reactor (보통 메인 reactor). reactor_run 루프에서 자기 자신이 g_scheduling_reactor 인지 확인하고, 주기마다 _reactors_scheduler_gather_metrics 호출. */
bool g_scheduling_in_progress = false;                  /* [한국어] 3-phase 스케줄링이 진행 중인지 가드. true 면 새 라운드 트리거 안 함. NOTE: extern (헤더에 선언). 단일 reactor 가 set/clear 하므로 lock 불필요. */
static uint64_t g_scheduler_period_in_tsc = 0;          /* [한국어] 스케줄링 주기 (TSC 단위). 0 이면 비활성. spdk_scheduler_set_period 가 us 를 받아 TSC 로 변환. reactor_run 루프가 (tsc_last - last_sched) 와 비교. */
static uint64_t g_scheduler_period_in_us;               /* [한국어] 스케줄링 주기 (microsecond, 사용자 노출용). spdk_scheduler_get_period 가 그대로 반환. */
static uint32_t g_scheduler_core_number;                /* [한국어] _reactors_scheduler_update_core_mode 가 코어를 순차 순회할 때의 진행 인덱스. interrupt 모드 전환이 비동기 콜백 체인이므로 상태로 유지해야 함. */
static struct spdk_scheduler_core_info *g_core_infos = NULL;    /* [한국어] 코어 별 메트릭/스레드 정보 배열 (calloc, 크기 = g_reactor_count). gather_metrics 가 채우고 balance 가 읽음. spdk_reactors_fini 에서 free. */
static struct spdk_cpuset g_scheduler_isolated_core_mask;       /* [한국어] 스케줄러가 thread 를 옮기지 못하는 격리 코어 비트맵. _threads_reschedule 에서 src/dst 가 isolated 면 이동 거부. */

TAILQ_HEAD(, spdk_governor) g_governor_list             /* [한국어] 등록된 governor (CPU 주파수 조절 모듈) 리스트. spdk_governor_register 로 추가. */
	= TAILQ_HEAD_INITIALIZER(g_governor_list);

static struct spdk_governor *g_governor = NULL;         /* [한국어] 현재 활성 governor. spdk_governor_set 으로 변경. NULL 이면 주파수 조절 안 함. */

static int reactor_interrupt_init(struct spdk_reactor *reactor);        /* [한국어] forward decl — interrupt mode 용 fd_group/eventfd 초기화. Linux 만 실제 구현, 그 외 -ENOTSUP. */
static void reactor_interrupt_fini(struct spdk_reactor *reactor);       /* [한국어] forward decl — fd_group 해제 및 eventfd close. */
static void end_reactor(void *arg1, void *arg2);                        /* [한국어] forward decl — spdk_for_each_reactor 의 마지막 콜백 (cpl 호출 후 call_reactor 컨텍스트 free). */

static pthread_mutex_t g_stopping_reactors_mtx = PTHREAD_MUTEX_INITIALIZER;     /* [한국어] g_stopping_reactors 플래그 보호 mutex. spdk_for_each_reactor 진입과 shutdown 트리거 간 race 방지. */
static bool g_stopping_reactors = false;                                /* [한국어] 종료 중 새 for_each_reactor 작업이 시작되지 않도록 차단하는 플래그. 종료 시점의 메모리 누수 방지. */

/*
 * [한국어]
 * _scheduler_find - 등록된 scheduler 리스트에서 이름으로 검색
 *
 * @name: 찾고자 하는 scheduler 이름 (예: "static", "dynamic"). NULL 불가.
 * @return: 일치하는 scheduler 포인터 또는 NULL (없을 시).
 *
 * scheduler 플러그인은 SPDK_SCHEDULER_REGISTER 매크로 (constructor) 를 통해
 * g_scheduler_list TAILQ 에 등록된다. spdk_scheduler_set 에서 이름 → 객체
 * 룩업용으로 호출. 단일 호출자는 보통 메인 스레드(스케줄링 reactor)에서만
 * 호출하므로 별도 락 없이 TAILQ_FOREACH 사용.
 *
 * 호출 체인:
 *   spdk_scheduler_set → [_scheduler_find]
 */
static struct spdk_scheduler *
_scheduler_find(const char *name)
{
	struct spdk_scheduler *tmp;        /* [한국어] 순회 변수 — 리스트의 각 scheduler 노드. */

	TAILQ_FOREACH(tmp, &g_scheduler_list, link) {   /* [한국어] g_scheduler_list 의 모든 노드를 link 필드를 따라 순회 (BSD TAILQ 매크로). */
		if (strcmp(name, tmp->name) == 0) {     /* [한국어] 문자열 비교 — scheduler 이름은 사용자 노출 식별자이며 빌드 시점에 결정된 정적 문자열. */
			return tmp;                     /* [한국어] 일치 시 즉시 반환. 동일 이름 중복 등록은 _scheduler_register 에서 거부됨. */
		}
	}

	return NULL;                            /* [한국어] 끝까지 못 찾으면 NULL — 호출자가 -EINVAL 처리. */
}

/*
 * [한국어]
 * spdk_scheduler_set - 활성 scheduler 를 이름으로 교체
 *
 * @name: 활성화할 scheduler 이름. NULL 이면 명시적으로 비활성화 (스케줄링 정지).
 * @return: 0 성공, -EINVAL 이름이 등록되지 않음, scheduler->init() 실패 시 그 rc.
 *
 * SPDK 런타임 중 scheduler 를 동적으로 교체할 수 있도록 한다 (RPC 등으로).
 * 동작 단계: 기존 scheduler 가 있으면 deinit() → 새 scheduler->init() 호출 →
 * 성공이면 g_scheduler 갱신, 실패면 가능하면 이전 것을 다시 init() 으로 복구.
 * 실행 컨텍스트: 보통 RPC 핸들러나 시스템 초기화 경로 — 메인 reactor.
 *
 * 호출 체인:
 *   RPC handler / app init → [spdk_scheduler_set] → _scheduler_find → scheduler->init/deinit
 */
int
spdk_scheduler_set(const char *name)
{
	struct spdk_scheduler *scheduler;       /* [한국어] 새로 활성화할 scheduler. */
	int rc = 0;                             /* [한국어] 반환 코드. ->init() 실패 시 음수. */

	/* NULL scheduler was specifically requested */
	if (name == NULL) {                     /* [한국어] NULL 이면 "스케줄링 비활성화" 의미. RPC 로 명시적 OFF 가능. */
		if (g_scheduler) {              /* [한국어] 기존 활성 scheduler 가 있으면 정상 종료시킨다. */
			g_scheduler->deinit();  /* [한국어] 플러그인 ->deinit 콜백 — 내부 상태 (타이머, 통계) 해제. */
		}
		g_scheduler = NULL;             /* [한국어] 전역 포인터 클리어 — _reactors_scheduler_balance 가 NULL 보고 cancel. */
		return 0;
	}

	scheduler = _scheduler_find(name);      /* [한국어] 이름으로 등록 리스트 검색. */
	if (scheduler == NULL) {                /* [한국어] 못 찾으면 사용자 입력 오류. */
		SPDK_ERRLOG("Requested scheduler is missing\n");
		return -EINVAL;
	}

	if (g_scheduler == scheduler) {         /* [한국어] 이미 같은 scheduler 가 활성 — 아무것도 안 하고 OK. */
		return 0;
	}

	if (g_scheduler) {                      /* [한국어] 기존 scheduler 가 있으면 deinit 후 교체. */
		g_scheduler->deinit();
	}

	rc = scheduler->init();                 /* [한국어] 새 scheduler 의 init — 내부 자료구조 (코어 부하 추적기 등) 초기화. */
	if (rc == 0) {                          /* [한국어] init 성공 — 활성 포인터 갱신. */
		g_scheduler = scheduler;
	} else {
		/* Could not switch to the new scheduler, so keep the old
		 * one. We need to check if it wasn't NULL, and ->init() it again.
		 */
		if (g_scheduler) {              /* [한국어] init 실패했지만 이전 것을 deinit 했으므로 복구 필요. */
			SPDK_ERRLOG("Could not ->init() '%s' scheduler, reverting to '%s'\n",
				    name, g_scheduler->name);
			g_scheduler->init();    /* [한국어] 이전 scheduler 를 다시 init — 실패해도 별도 처리 안 함 (best-effort). */
		} else {
			SPDK_ERRLOG("Could not ->init() '%s' scheduler.\n", name);
		}
	}

	return rc;                              /* [한국어] init 의 반환값을 그대로 사용자에게 전달. */
}

/*
 * [한국어]
 * spdk_scheduler_get - 현재 활성 scheduler 포인터 반환
 *
 * @return: 활성 scheduler (없으면 NULL).
 *
 * 단순 getter. RPC dump_scheduler 등에서 이름 조회 용도. lock 없음 — 포인터
 * 읽기는 원자적이고, 토글되는 상황은 정적이라고 가정.
 *
 * 호출 체인:
 *   _reactors_scheduler_balance / RPC → [spdk_scheduler_get]
 */
struct spdk_scheduler *
spdk_scheduler_get(void)
{
	return g_scheduler;     /* [한국어] 전역 포인터 그대로 반환. */
}

/*
 * [한국어]
 * spdk_scheduler_get_period - 스케줄링 주기를 us 단위로 반환
 *
 * @return: 마이크로초 단위 주기 (0 이면 비활성).
 *
 * RPC/CLI 표시용 getter.
 */
uint64_t
spdk_scheduler_get_period(void)
{
	return g_scheduler_period_in_us;        /* [한국어] us 단위 그대로 반환 — 사람이 보는 단위. */
}

/*
 * [한국어]
 * spdk_scheduler_set_period - 스케줄링 주기를 설정
 *
 * @period: 마이크로초 단위 주기. 0 이면 스케줄링 비활성화.
 *
 * us 를 받아 reactor_run 에서 비교용으로 쓰는 TSC 단위로 변환해 둔다.
 * spdk_get_ticks_hz() = TSC 의 초당 틱 수. 따라서 period_us / 1e6 * hz 로
 * TSC 변환. reactor_run 의 hot-path 에서는 매번 변환을 피하기 위해 미리 계산.
 *
 * 호출 체인:
 *   RPC / app init → [spdk_scheduler_set_period]
 */
void
spdk_scheduler_set_period(uint64_t period)
{
	g_scheduler_period_in_us = period;                                                      /* [한국어] 사용자 노출 us 값 저장. */
	g_scheduler_period_in_tsc = period * spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;            /* [한국어] TSC 변환: us * (TSC/sec) / (us/sec) = TSC. SPDK_SEC_TO_USEC = 1,000,000. */
}

/*
 * [한국어]
 * spdk_scheduler_register - scheduler 플러그인 등록 (constructor 시점)
 *
 * @scheduler: 등록할 scheduler 객체 (정적 저장). name 필드가 고유해야 함.
 *
 * 보통 SPDK_SCHEDULER_REGISTER 매크로가 __attribute__((constructor)) 로 호출.
 * 동일 이름 중복 등록은 assert(false) 로 즉시 죽임 (개발 단계 오류).
 *
 * 호출 체인:
 *   SPDK_SCHEDULER_REGISTER (constructor) → [spdk_scheduler_register]
 */
void
spdk_scheduler_register(struct spdk_scheduler *scheduler)
{
	if (_scheduler_find(scheduler->name)) {         /* [한국어] 동일 이름 사전 검사. */
		SPDK_ERRLOG("scheduler named '%s' already registered.\n", scheduler->name);
		assert(false);                          /* [한국어] 디버그 빌드에서는 즉시 abort — 빌드 구성 오류 신호. */
		return;
	}

	TAILQ_INSERT_TAIL(&g_scheduler_list, scheduler, link);  /* [한국어] 리스트 끝에 삽입. 등록 순서 = 발견 순서. */
}

/*
 * [한국어]
 * spdk_scheduler_get_scheduling_lcore - 스케줄링을 트리거하는 reactor 의 lcore
 *
 * @return: g_scheduling_reactor->lcore (보통 메인 코어).
 *
 * scheduler 콜백 체인이 이 코어로 돌아오는 종착점을 알려준다 (gather_metrics
 * 페이즈가 한 바퀴 돌고 첫 코어 = 스케줄링 코어로 돌아오면 phase2 진입).
 */
uint32_t
spdk_scheduler_get_scheduling_lcore(void)
{
	return g_scheduling_reactor->lcore;     /* [한국어] 포인터 역참조. NULL 체크 없음 — reactors_init 후 항상 유효 가정. */
}

/*
 * [한국어]
 * spdk_scheduler_set_scheduling_lcore - 스케줄링 트리거 코어 변경
 *
 * @core: 새 스케줄링 reactor 의 lcore.
 * @return: true 성공, false core 가 유효 reactor 가 아님.
 *
 * 메인 코어가 아닌 다른 코어에서 스케줄링을 돌리고 싶을 때 사용 (예: RPC).
 *
 * 호출 체인:
 *   RPC framework_set_scheduler → [spdk_scheduler_set_scheduling_lcore]
 */
bool
spdk_scheduler_set_scheduling_lcore(uint32_t core)
{
	struct spdk_reactor *reactor = spdk_reactor_get(core);  /* [한국어] core 인덱스 → reactor 객체 확인. */
	if (reactor == NULL) {                                  /* [한국어] 미등록/미사용 코어면 실패. */
		SPDK_ERRLOG("Failed to set scheduling reactor. Reactor(lcore:%d) does not exist", core);
		return false;
	}

	g_scheduling_reactor = reactor;                         /* [한국어] 전역 포인터 교체 — race 가능하나 빈번하지 않으며 다음 라운드부터 적용. */
	return true;
}

/*
 * [한국어]
 * scheduler_set_isolated_core_mask - 스케줄러가 thread 를 옮기지 않는 격리 코어 설정
 *
 * @isolated_core_mask: 격리 코어 비트맵 (값 전달, 내부에서 g_ 마스크에 복사).
 * @return: true 성공, false 격리 마스크가 app 코어 마스크의 부분집합이 아님.
 *
 * isolated 코어에 배치된 thread 는 _threads_reschedule 에서 재배치 거부됨.
 * 일부 워크로드(예: NVMe-oF poller 코어)를 dynamic scheduler 가 옮기지 못하게.
 *
 * 호출 체인:
 *   RPC framework_set_scheduler isolated_core_mask=... → [scheduler_set_isolated_core_mask]
 */
bool
scheduler_set_isolated_core_mask(struct spdk_cpuset isolated_core_mask)
{
	struct spdk_cpuset tmp_mask;            /* [한국어] (app_mask | isolated_mask) 임시 계산용. */

	spdk_cpuset_copy(&tmp_mask, spdk_app_get_core_mask());  /* [한국어] app 코어 마스크 복사. */
	spdk_cpuset_or(&tmp_mask, &isolated_core_mask);         /* [한국어] OR 연산 — 격리가 app 마스크의 부분집합이면 결과 == app 마스크. */
	if (spdk_cpuset_equal(&tmp_mask, spdk_app_get_core_mask()) == false) {  /* [한국어] OR 결과가 원본과 다르다면 격리 비트가 app 외부 — 잘못된 입력. */
		SPDK_ERRLOG("Isolated core mask is not included in app core mask.\n");
		return false;
	}
	spdk_cpuset_copy(&g_scheduler_isolated_core_mask, &isolated_core_mask); /* [한국어] 검증 통과 후 글로벌에 복사. */
	return true;
}

/*
 * [한국어]
 * scheduler_get_isolated_core_mask - 격리 코어 마스크의 hex 문자열 반환
 *
 * @return: TLS 버퍼에 포맷된 16진 마스크 문자열 (재호출 시 덮어쓰임).
 *
 * RPC framework_get_scheduler 응답 등에 사용.
 */
const char *
scheduler_get_isolated_core_mask(void)
{
	return spdk_cpuset_fmt(&g_scheduler_isolated_core_mask);        /* [한국어] hex 문자열 포맷 (TLS 버퍼). */
}

/*
 * [한국어]
 * scheduler_is_isolated_core - 특정 코어가 격리 마스크에 포함되는지 검사
 *
 * @core: 검사할 lcore.
 * @return: true 격리, false 일반 코어.
 *
 * _reactors_scheduler_gather_metrics 가 core_info->isolated 플래그를 채울 때
 * 사용. balance 단계에서 이 플래그를 보고 thread 이동 결정.
 *
 * 호출 체인:
 *   _reactors_scheduler_gather_metrics → [scheduler_is_isolated_core]
 */
static bool
scheduler_is_isolated_core(uint32_t core)
{
	return spdk_cpuset_get_cpu(&g_scheduler_isolated_core_mask, core);      /* [한국어] 비트맵에서 해당 비트 조회 — O(1). */
}

/*
 * [한국어]
 * reactor_construct - 단일 reactor 객체 초기화
 *
 * @reactor: posix_memalign 으로 할당된 g_reactors[lcore] 슬롯 포인터.
 * @lcore: 이 reactor 가 핀될 논리 코어 번호.
 *
 * spdk_reactors_init 에서 SPDK_ENV_FOREACH_CORE 로 코어마다 1회 호출된다.
 * 동작 단계:
 *   1) lcore 저장 및 is_valid 플래그 set (spdk_reactor_get 검증용).
 *   2) lw_thread TAILQ 초기화, notify_cpuset 0 으로 초기화.
 *   3) events ring (MP-SC, 65536 슬롯) 생성 — 다른 reactor 들이 push 가능.
 *   4) interrupt 모드 facilities (fd_group, eventfd 2개) 항상 준비.
 *   5) full interrupt 모드면 모든 코어에 대해 notify 비트 set + in_interrupt=true.
 * 실행 컨텍스트: 메인 reactor(아직 polling 시작 전, 단일 스레드 컨텍스트).
 *
 * 호출 체인:
 *   spdk_reactors_init → [reactor_construct] → spdk_ring_create / reactor_interrupt_init
 */
static void
reactor_construct(struct spdk_reactor *reactor, uint32_t lcore)
{
	reactor->lcore = lcore;                         /* [한국어] 자신의 코어 번호 저장. spdk_reactor_get 의 인덱스와 일치해야 함. */
	reactor->flags.is_valid = true;                 /* [한국어] 유효 슬롯 마킹 — spdk_reactor_get 이 false 면 NULL 반환. */

	TAILQ_INIT(&reactor->threads);                  /* [한국어] lw_thread 리스트 헤더 초기화. _schedule_thread 가 INSERT_TAIL. */
	reactor->thread_count = 0;                      /* [한국어] 현재 배치된 lw_thread 수 카운터 시작값. */
	spdk_cpuset_zero(&reactor->notify_cpuset);      /* [한국어] notify_cpuset = "이 reactor 가 다른 reactor 에 메시지 보낼 때 eventfd 깨워야 하는지 결정하는 비트맵". 초기엔 모두 0 (polling 끼리는 깨울 필요 없음). */

	reactor->events = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 65536, SPDK_ENV_NUMA_ID_ANY);  /* [한국어] cross-reactor 이벤트 큐. MP-SC = Multi-Producer Single-Consumer (DPDK rte_ring 기반, lockless). 크기 65536 (2^16) — power-of-2 필수. NUMA_ID_ANY = 첫 가용 NUMA 노드. */
	if (reactor->events == NULL) {
		SPDK_ERRLOG("Failed to allocate events ring\n");
		assert(false);                          /* [한국어] reactor 의 핵심 자료구조 없으면 진행 불가 — 디버그 abort. */
	}

	/* Always initialize interrupt facilities for reactor */
	if (reactor_interrupt_init(reactor) != 0) {     /* [한국어] fd_group + 2 eventfd 생성. polling 모드에서도 eventfd 가 필요할 수 있어 항상 시도. */
		/* Reactor interrupt facilities are necessary if setting app to interrupt mode. */
		if (spdk_interrupt_mode_is_enabled()) { /* [한국어] interrupt mode 강제 활성 시 fd_group 실패는 치명적. */
			SPDK_ERRLOG("Failed to prepare intr facilities\n");
			assert(false);
		}
		return;                                 /* [한국어] polling 모드면 fd_group 없어도 돌릴 수 있으므로 그대로 반환. */
	}

	/* If application runs with full interrupt ability,
	 * all reactors are going to run in interrupt mode.
	 */
	if (spdk_interrupt_mode_is_enabled()) {         /* [한국어] 앱이 interrupt mode 로 시작되면 모든 reactor 를 interrupt 로. */
		uint32_t i;

		SPDK_ENV_FOREACH_CORE(i) {              /* [한국어] 모든 활성 코어에 대해 notify 비트 set — 모든 cross-reactor 통신이 eventfd 깨움 필요. */
			spdk_cpuset_set_cpu(&reactor->notify_cpuset, i, true);
		}
		reactor->in_interrupt = true;           /* [한국어] reactor 자신을 interrupt 모드로 시작. reactor_run 루프가 epoll wait 사용. */
	}
}

/*
 * [한국어]
 * spdk_reactor_get - lcore 번호로 reactor 객체 조회
 *
 * @lcore: 논리 코어 번호 (0 이상 g_reactor_count 미만).
 * @return: 유효 reactor 포인터 또는 NULL (배열 미초기화/범위 초과/유효하지 않음).
 *
 * 모든 코어 간 메시징/스케줄링이 이 함수로 reactor 핸들을 얻는다. is_valid
 * 플래그를 검사해 "코어 마스크에 포함되지 않은 슬롯" 을 걸러낸다 (reactors_init
 * 가 SPDK_ENV_FOREACH_CORE 로만 reactor_construct 호출하므로 해당 코어만 valid).
 *
 * 호출 체인:
 *   광범위 — spdk_event_call, _reactor_set_interrupt_mode, scheduler 등
 */
struct spdk_reactor *
spdk_reactor_get(uint32_t lcore)
{
	struct spdk_reactor *reactor;           /* [한국어] 반환 후보 포인터. */

	if (g_reactors == NULL) {               /* [한국어] reactors_init 호출 전 또는 fini 후 — 잘못된 사용. */
		SPDK_WARNLOG("Called spdk_reactor_get() while the g_reactors array was NULL!\n");
		return NULL;
	}

	if (lcore >= g_reactor_count) {         /* [한국어] 배열 인덱스 범위 검사 — out-of-bounds 방지. */
		return NULL;
	}

	reactor = &g_reactors[lcore];           /* [한국어] 배열 인덱싱 — lcore 번호 = 배열 인덱스 (희소 매핑이지만 메모리는 모든 슬롯 할당). */

	if (reactor->flags.is_valid == false) { /* [한국어] reactor_construct 가 호출되지 않은 슬롯 (코어 마스크 외 코어). */
		return NULL;
	}

	return reactor;
}

static int reactor_thread_op(struct spdk_thread *thread, enum spdk_thread_op op);       /* [한국어] forward decl — spdk_thread_lib 가 thread NEW/RESCHED 시 호출. */
static bool reactor_thread_op_supported(enum spdk_thread_op op);                        /* [한국어] forward decl — 어떤 op 를 지원하는지 thread_lib 에 알림. */

/* Power of 2 minus 1 is optimal for memory consumption */
#define EVENT_MSG_MEMPOOL_SHIFT 14 /* 2^14 = 16384 */                            /* [한국어] mempool 크기 지수. DPDK rte_mempool 은 (2^N - 1) 가 메모리 효율 최적 (cache_size 정렬). */
#define EVENT_MSG_MEMPOOL_SIZE ((1 << EVENT_MSG_MEMPOOL_SHIFT) - 1)              /* [한국어] 16383 = spdk_event 객체 수. 모든 reactor 가 공유. 한 라운드에 16383 개를 넘는 인플라이트 이벤트는 발생하지 않는다고 가정. */

/*
 * [한국어]
 * spdk_reactors_init - reactor 시스템 전체 초기화 (메인 스레드에서 1회 호출)
 *
 * @msg_mempool_size: spdk_thread 메시지 mempool 크기 — spdk_thread_lib_init_ext 에 전달.
 * @return: 0 성공, 음수 실패 (alloc/init 실패).
 *
 * 동작 단계:
 *   1) g_spdk_event_mempool 생성 (16383 개 spdk_event, 프로세스별 고유 이름).
 *   2) g_reactors 배열 posix_memalign(64) 으로 할당 (cache-line 정렬).
 *   3) g_core_infos 배열 calloc.
 *   4) spdk_thread_lib_init_ext 호출 — reactor_thread_op 콜백을 thread lib 에 등록.
 *      (lw_thread 컨텍스트 크기를 알려줘 thread lib 가 spdk_thread 뒤에 reserve).
 *   5) 모든 활성 코어에 대해 reactor_construct.
 *   6) 메인 코어를 g_scheduling_reactor 로 지정.
 *   7) 상태를 INITIALIZED 로 전이.
 * 실행 컨텍스트: 메인 pthread (앱 부팅, 아직 reactor_run 진입 전).
 * 에러 경로: 단계별 cleanup (할당된 자원만 해제 후 음수 반환).
 *
 * 호출 체인:
 *   spdk_app_start (lib/event/app.c) → [spdk_reactors_init]
 */
int
spdk_reactors_init(size_t msg_mempool_size)
{
	struct spdk_reactor *reactor;           /* [한국어] 메인 reactor 포인터 (g_scheduling_reactor 지정용). */
	int rc;                                 /* [한국어] 시스템 콜 반환 코드. */
	uint32_t i, current_core;               /* [한국어] 코어 순회 인덱스, 현재 코어 번호. */
	char mempool_name[32];                  /* [한국어] mempool 이름 ("evtpool_<pid>"). 32바이트면 충분. */

	snprintf(mempool_name, sizeof(mempool_name), "evtpool_%d", getpid());   /* [한국어] PID 기반 고유 이름 — 같은 호스트에 여러 SPDK 인스턴스 공존 시 mempool 이름 충돌 방지. */
	g_spdk_event_mempool = spdk_mempool_create(mempool_name,
			       EVENT_MSG_MEMPOOL_SIZE,                          /* [한국어] 16383 개 — 충분히 크지만 hugepage 사용량 합리적. */
			       sizeof(struct spdk_event),                       /* [한국어] 객체 크기 — spdk_event 한 개. */
			       SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,                 /* [한국어] per-core cache 기본값 — 빈번한 alloc/free 의 lockless 경로. */
			       SPDK_ENV_NUMA_ID_ANY);                           /* [한국어] NUMA 어디든 OK. */

	if (g_spdk_event_mempool == NULL) {     /* [한국어] hugepage 부족, 이름 충돌 등으로 실패 가능. */
		SPDK_ERRLOG("spdk_event_mempool creation failed\n");
		return -1;
	}

	/* struct spdk_reactor must be aligned on 64 byte boundary */
	g_reactor_count = spdk_env_get_last_core() + 1;                         /* [한국어] 코어 번호의 최댓값 + 1 = 배열 크기. 코어 ID 가 sparse 일 수 있어 비트맵 아닌 dense 배열. */
	rc = posix_memalign((void **)&g_reactors, 64,                           /* [한국어] 64B (cache line) 정렬 — false sharing 방지. spdk_reactor 의 hot 필드들이 코어별로 독립적인 캐시 라인을 가져야 함. */
			    g_reactor_count * sizeof(struct spdk_reactor));
	if (rc != 0) {
		SPDK_ERRLOG("Could not allocate array size=%u for g_reactors\n",
			    g_reactor_count);
		spdk_mempool_free(g_spdk_event_mempool);                        /* [한국어] cleanup: 이미 만든 mempool 해제. */
		return -1;
	}

	g_core_infos = calloc(g_reactor_count, sizeof(*g_core_infos));          /* [한국어] 스케줄러 메트릭 배열. calloc 으로 0 초기화. */
	if (g_core_infos == NULL) {
		SPDK_ERRLOG("Could not allocate memory for g_core_infos\n");
		spdk_mempool_free(g_spdk_event_mempool);
		free(g_reactors);                                               /* [한국어] cleanup: 앞서 할당한 g_reactors 해제. */
		return -ENOMEM;
	}

	memset(g_reactors, 0, (g_reactor_count) * sizeof(struct spdk_reactor)); /* [한국어] posix_memalign 은 0 초기화 보장 안 함 — 명시적 zero-out. is_valid=false 등이 0 으로. */

	rc = spdk_thread_lib_init_ext(reactor_thread_op, reactor_thread_op_supported,
				      sizeof(struct spdk_lw_thread), msg_mempool_size); /* [한국어] thread lib 에 reactor 콜백 등록. lw_thread 크기를 알려주면 spdk_thread 객체 뒤에 reserve 되어 spdk_thread_get_ctx 로 접근 가능. */
	if (rc != 0) {
		SPDK_ERRLOG("Initialize spdk thread lib failed\n");
		spdk_mempool_free(g_spdk_event_mempool);
		free(g_reactors);
		free(g_core_infos);
		return rc;
	}

	SPDK_ENV_FOREACH_CORE(i) {              /* [한국어] 활성 코어만 (g_reactor_count 슬롯 중 일부). */
		reactor_construct(&g_reactors[i], i);   /* [한국어] 각 코어별 reactor 초기화. */
	}

	current_core = spdk_env_get_current_core();     /* [한국어] 메인 스레드가 핀된 lcore. */
	reactor = spdk_reactor_get(current_core);       /* [한국어] 메인 reactor 객체. */
	assert(reactor != NULL);                        /* [한국어] 메인 코어는 반드시 활성. */
	g_scheduling_reactor = reactor;                 /* [한국어] 기본 스케줄링 트리거를 메인 reactor 로 지정. */

	g_reactor_state = SPDK_REACTOR_STATE_INITIALIZED;       /* [한국어] 상태 전이 — 이 후 spdk_reactors_start 가능. */

	return 0;
}

/*
 * [한국어]
 * spdk_reactors_fini - reactors_init 의 모든 자원 해제
 *
 * spdk_reactors_start 후 reactor_run 루프가 종료된 뒤 (g_reactor_state = SHUTDOWN)
 * spdk_app_fini 가 호출. UNINITIALIZED 면 idempotent (스킵).
 *
 * 동작 단계:
 *   1) spdk_thread_lib_fini.
 *   2) 각 reactor 의 events ring, fd_group, core_info->thread_infos 해제.
 *   3) g_spdk_event_mempool / g_reactors / g_core_infos 해제.
 * 실행 컨텍스트: 메인 스레드 (모든 reactor pthread 가 join 된 후).
 *
 * 호출 체인:
 *   spdk_app_fini → [spdk_reactors_fini]
 */
void
spdk_reactors_fini(void)
{
	uint32_t i;                             /* [한국어] 순회 인덱스. */
	struct spdk_reactor *reactor;           /* [한국어] 현재 처리 중 reactor. */

	if (g_reactor_state == SPDK_REACTOR_STATE_UNINITIALIZED) {      /* [한국어] init 안 했거나 이미 fini 됨 — 안전한 no-op. */
		return;
	}

	spdk_thread_lib_fini();                 /* [한국어] thread lib 의 콜백 등록 해제 — 이후 spdk_thread_create 호출 불가. */

	SPDK_ENV_FOREACH_CORE(i) {
		reactor = spdk_reactor_get(i);
		assert(reactor != NULL);
		assert(reactor->thread_count == 0);     /* [한국어] 정상 종료라면 모든 lw_thread 가 destroy 되어 0이어야 함. */
		if (reactor->events != NULL) {
			spdk_ring_free(reactor->events);        /* [한국어] DPDK ring 해제 — ring 내부 미처리 이벤트는 mempool free 시 자동 회수. */
		}

		reactor_interrupt_fini(reactor);        /* [한국어] eventfd close + fd_group destroy. */

		if (g_core_infos != NULL) {
			free(g_core_infos[i].thread_infos);     /* [한국어] gather_metrics 가 calloc 한 thread_infos — 정상 종료 시엔 이미 NULL 일 가능성 높음. */
		}
	}

	spdk_mempool_free(g_spdk_event_mempool);        /* [한국어] event mempool 해제 — hugepage 영역 회수. */

	free(g_reactors);                       /* [한국어] reactor 배열 해제 (posix_memalign 은 free 와 호환). */
	g_reactors = NULL;                      /* [한국어] dangling 방지 — spdk_reactor_get 이 NULL 반환하도록. */
	free(g_core_infos);
	g_core_infos = NULL;
}

static void _reactor_set_interrupt_mode(void *arg1, void *arg2);        /* [한국어] forward decl — target reactor 자신의 코어에서 실행되어 in_interrupt 플래그 토글 + thread 들에 fd_group 연결/해제. */

/*
 * [한국어]
 * _reactor_set_notify_cpuset - 모든 reactor 의 notify_cpuset 의 target 비트 갱신
 *
 * @arg1: target 으로 전환되는 reactor 포인터.
 * @arg2: unused.
 *
 * spdk_for_each_reactor 의 fn 콜백으로 모든 코어에서 실행된다. 각 reactor 가
 * 자신의 notify_cpuset 의 target->lcore 비트를 target->new_in_interrupt 값으로
 * 갱신 — 이후 다른 reactor 가 target 으로 이벤트 보낼 때 eventfd 로 깨워야
 * 하는지 결정. (interrupt 모드인 reactor 는 polling 안 하므로 깨워줘야 함.)
 *
 * 호출 체인:
 *   spdk_reactor_set_interrupt_mode → spdk_for_each_reactor → on_reactor → [_reactor_set_notify_cpuset]
 */
static void
_reactor_set_notify_cpuset(void *arg1, void *arg2)
{
	struct spdk_reactor *target = arg1;                                                     /* [한국어] 모드를 전환하는 대상. */
	struct spdk_reactor *reactor = spdk_reactor_get(spdk_env_get_current_core());           /* [한국어] 현재 코어의 reactor — 이 콜백이 실행 중인 reactor. */

	assert(reactor != NULL);
	spdk_cpuset_set_cpu(&reactor->notify_cpuset, target->lcore, target->new_in_interrupt);  /* [한국어] target 으로 보낼 때 깨워야 하는지 비트 set/clear. set=interrupt 모드, clear=polling. */
}

/*
 * [한국어]
 * _event_call - spdk_event_allocate + spdk_event_call 헬퍼
 *
 * @lcore: 이벤트 실행 대상 코어.
 * @fn: 콜백 함수.
 * @arg1, arg2: 콜백 인자.
 *
 * 한 줄짜리 호출 패턴을 짧게 해주는 내부 헬퍼.
 */
static void
_event_call(uint32_t lcore, spdk_event_fn fn, void *arg1, void *arg2)
{
	struct spdk_event *ev;          /* [한국어] mempool 에서 받을 이벤트. */

	ev = spdk_event_allocate(lcore, fn, arg1, arg2);        /* [한국어] mempool get + 필드 채움. */
	assert(ev);                                             /* [한국어] mempool 고갈 가정 안 함 — 디버그에서 abort. */
	spdk_event_call(ev);                                    /* [한국어] 대상 reactor 의 events ring 으로 push (필요 시 eventfd 깨움). */
}

/*
 * [한국어]
 * _reactor_set_notify_cpuset_cpl - notify_cpuset 갱신 라운드 완료 콜백
 *
 * @arg1: target reactor.
 *
 * spdk_for_each_reactor 가 모든 코어를 돌고 끝났을 때 호출. 두 시나리오:
 *  - 폴링→인터럽트로 전환: notify 비트가 모두 set 되었으므로 이제 target 의
 *    실제 모드 토글 (_reactor_set_interrupt_mode) 을 target 코어에서 실행.
 *  - 인터럽트→폴링으로 전환: 이미 _reactor_set_interrupt_mode 가 끝난 후
 *    notify 비트를 clear 한 상태이므로 사용자 콜백 호출.
 */
static void
_reactor_set_notify_cpuset_cpl(void *arg1, void *arg2)
{
	struct spdk_reactor *target = arg1;     /* [한국어] 모드 전환 대상. */

	if (target->new_in_interrupt == false) {        /* [한국어] interrupt → polling 시나리오: notify clear 가 이미 끝났고 모드 토글도 됐음. */
		target->set_interrupt_mode_in_progress = false;        /* [한국어] 인플라이트 플래그 해제 — 다음 전환 요청 허용. */
		_event_call(spdk_scheduler_get_scheduling_lcore(), target->set_interrupt_mode_cb_fn,
			    target->set_interrupt_mode_cb_arg, NULL);   /* [한국어] 사용자 완료 콜백을 스케줄링 reactor 에서 실행. */
	} else {
		_event_call(target->lcore, _reactor_set_interrupt_mode, target, NULL);  /* [한국어] polling → interrupt: notify set 끝났으니 이제 target 코어에서 실제 토글. */
	}
}

/*
 * [한국어]
 * _reactor_set_thread_interrupt_mode - lw_thread 마다 호출되어 thread 모드 정렬
 *
 * @ctx: reactor 포인터 (in_interrupt 값 사용).
 *
 * spdk_thread_send_msg 로 thread 자기 자신의 컨텍스트에서 실행되어
 * 자기 thread 의 interrupt 모드를 reactor 와 일치시킨다.
 *
 * 호출 체인:
 *   _reactor_set_interrupt_mode / _schedule_thread → spdk_thread_send_msg → [_reactor_set_thread_interrupt_mode]
 */
static void
_reactor_set_thread_interrupt_mode(void *ctx)
{
	struct spdk_reactor *reactor = ctx;     /* [한국어] thread 가 속한 reactor. */

	spdk_thread_set_interrupt_mode(reactor->in_interrupt);  /* [한국어] thread lib API — thread 의 poller 들이 interrupt vs polling 모드로 동작 변경. */
}

/*
 * [한국어]
 * _reactor_set_interrupt_mode - target reactor 의 in_interrupt 플래그 토글
 *
 * @arg1: target reactor.
 * @arg2: unused.
 *
 * target 의 자기 코어에서 실행 (assert 로 검증). 동작 단계:
 *   1) in_interrupt = new_in_interrupt 토글.
 *   2) full interrupt mode 면 모든 lw_thread 의 fd_group 을 reactor->fgrp 에
 *      nest/unnest (epoll 계층 구조 — reactor fgrp 가 부모, thread fgrp 가 자식).
 *   3) interrupt → polling 이면 tsc_last refresh 후 모든 reactor 의 notify
 *      비트 clear. polling → interrupt 이면 events_fd / resched_fd 양쪽 깨워서
 *      혹시라도 처리 못한 펜딩 이벤트가 있으면 즉시 처리하도록 (race 보호).
 *
 * 호출 체인:
 *   spdk_reactor_set_interrupt_mode 또는 _reactor_set_notify_cpuset_cpl
 *      → _event_call(target->lcore) → [_reactor_set_interrupt_mode]
 */
static void
_reactor_set_interrupt_mode(void *arg1, void *arg2)
{
	struct spdk_reactor *target = arg1;             /* [한국어] 모드 전환 대상. */
	struct spdk_thread *thread;                     /* [한국어] 순회 중 thread 핸들. */
	struct spdk_fd_group *grp;                      /* [한국어] thread 의 fd_group (poller 들이 epoll 으로 노출하는 fd 들의 묶음). */
	struct spdk_lw_thread *lw_thread, *tmp;         /* [한국어] TAILQ_FOREACH_SAFE 용. */

	assert(target == spdk_reactor_get(spdk_env_get_current_core()));        /* [한국어] 반드시 target 자기 코어에서 실행되어야 함 — fd_group 조작은 owner 코어 내에서만. */
	assert(target != NULL);
	assert(target->in_interrupt != target->new_in_interrupt);               /* [한국어] 동일 모드로 토글하는 case 는 호출 측에서 차단. */
	SPDK_DEBUGLOG(reactor, "Do reactor set on core %u from %s to state %s\n",
		      target->lcore, target->in_interrupt ? "intr" : "poll", target->new_in_interrupt ? "intr" : "poll");

	target->in_interrupt = target->new_in_interrupt;        /* [한국어] 실제 모드 비트 토글 — 이후 reactor_run 의 분기가 다르게 동작. */

	if (spdk_interrupt_mode_is_enabled()) {                 /* [한국어] full interrupt 모드(앱 전체)일 때만 thread fd_group 정렬. */
		/* Align spdk_thread with reactor to interrupt mode or poll mode */
		TAILQ_FOREACH_SAFE(lw_thread, &target->threads, link, tmp) {    /* [한국어] _SAFE: 콜백이 리스트를 수정해도 안전. */
			thread = spdk_thread_get_from_ctx(lw_thread);           /* [한국어] lw_thread → spdk_thread (thread lib 가 ctx 뒤에 reserve). */
			if (target->in_interrupt) {                             /* [한국어] polling → interrupt: thread 의 fd_group 을 reactor fgrp 에 자식으로 등록. */
				grp = spdk_thread_get_interrupt_fd_group(thread);
				spdk_fd_group_nest(target->fgrp, grp);          /* [한국어] epoll 계층화 — reactor 가 wait 하면 thread fd 들도 함께 모니터링. */
			} else {
				grp = spdk_thread_get_interrupt_fd_group(thread);
				spdk_fd_group_unnest(target->fgrp, grp);        /* [한국어] interrupt → polling: 자식 분리. */
			}

			spdk_thread_send_msg(thread, _reactor_set_thread_interrupt_mode, target);       /* [한국어] thread 에 자기 컨텍스트에서 모드 토글하라고 메시지 — owner thread 가 직접 spdk_thread_set_interrupt_mode 호출. */
		}
	}

	if (target->new_in_interrupt == false) {                /* [한국어] interrupt → polling: 통계 정확성을 위해 tsc_last refresh 후 notify 비트 clear 라운드. */
		/* Reactor is no longer in interrupt mode. Refresh the tsc_last to accurately
		 * track reactor stats. */
		target->tsc_last = spdk_get_ticks();            /* [한국어] interrupt 모드에서 멈춘 동안 누적된 시간을 idle/busy 로 잘못 잡지 않도록 reset. */
		spdk_for_each_reactor(_reactor_set_notify_cpuset, target, NULL, _reactor_set_notify_cpuset_cpl); /* [한국어] 모든 reactor 의 notify 비트 clear (target.lcore 로 보낼 때 깨움 불필요). */
	} else {                                                /* [한국어] polling → interrupt: 펜딩 이벤트가 있을 수 있으므로 양쪽 fd 모두 깨움. */
		uint64_t notify = 1;                            /* [한국어] eventfd 의 의미 없는 값 — 8B 카운터 증가만 되면 됨. */
		int rc = 0;

		/* Always trigger spdk_event and resched event in case of race condition */
		rc = write(target->events_fd, &notify, sizeof(notify));         /* [한국어] events_fd 깨움 — interrupt 모드 진입 직후 펜딩 spdk_event 가 있으면 즉시 처리. */
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify event queue: %s.\n", spdk_strerror(errno));
		}
		rc = write(target->resched_fd, &notify, sizeof(notify));        /* [한국어] resched_fd 깨움 — 마찬가지로 펜딩 reschedule 처리. */
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify reschedule: %s.\n", spdk_strerror(errno));
		}

		target->set_interrupt_mode_in_progress = false;                 /* [한국어] 전환 완료 — 다음 요청 허용. */
		_event_call(spdk_scheduler_get_scheduling_lcore(), target->set_interrupt_mode_cb_fn,
			    target->set_interrupt_mode_cb_arg, NULL);           /* [한국어] 사용자 완료 콜백을 스케줄링 reactor 에서 호출. */
	}
}

/*
 * [한국어]
 * spdk_reactor_set_interrupt_mode - reactor 모드 전환 진입점 (외부 API)
 *
 * @lcore: 모드 전환 대상 코어.
 * @new_in_interrupt: true=interrupt, false=polling.
 * @cb_fn: 전환 완료 시 호출될 콜백 (스케줄링 reactor 에서 실행).
 * @cb_arg: cb_fn 의 첫 인자.
 * @return: 0 시작 성공, -EINVAL/-ENOTSUP/-EPERM/-EBUSY.
 *
 * 호출 측 제약:
 *   - 반드시 g_scheduling_reactor 코어에서 호출 (-EPERM 검사).
 *   - target 의 fgrp 가 있어야 함 (interrupt facility 가 init 되어 있어야).
 *   - 이미 같은 모드면 즉시 cb_fn 호출 (no-op).
 *   - 진행 중이면 -EBUSY.
 *
 * 두 시나리오:
 *   - poll → poll(no-op) / poll → interrupt: notify_cpuset set 라운드 후 토글.
 *   - interrupt → poll: 토글 먼저 한 다음 notify_cpuset clear 라운드.
 *   순서가 다른 이유는 race 보호 — interrupt 측이 깨워질 수 있는 상태를 항상
 *   유지해야 데드락이 안 생김.
 *
 * 호출 체인:
 *   _reactors_scheduler_update_core_mode → [spdk_reactor_set_interrupt_mode]
 */
int
spdk_reactor_set_interrupt_mode(uint32_t lcore, bool new_in_interrupt,
				spdk_reactor_set_interrupt_mode_cb cb_fn, void *cb_arg)
{
	struct spdk_reactor *target;            /* [한국어] 모드 전환 대상 reactor. */

	target = spdk_reactor_get(lcore);
	if (target == NULL) {                   /* [한국어] 잘못된 코어 번호. */
		return -EINVAL;
	}

	/* Eventfd has to be supported in order to use interrupt functionality. */
	if (target->fgrp == NULL) {             /* [한국어] interrupt facilities 미초기화 (Linux 가 아니거나 init 실패) — interrupt 모드 불가. */
		return -ENOTSUP;
	}

	if (spdk_env_get_current_core() != g_scheduling_reactor->lcore) {       /* [한국어] 스케줄링 reactor 만이 모드 전환 권한 — 그렇지 않으면 동시성 가정이 깨짐. */
		SPDK_ERRLOG("It is only permitted within scheduling reactor.\n");
		return -EPERM;
	}

	if (target->in_interrupt == new_in_interrupt) {         /* [한국어] 이미 같은 모드 — 즉시 완료 콜백. */
		cb_fn(cb_arg, NULL);
		return 0;
	}

	if (target->set_interrupt_mode_in_progress) {           /* [한국어] 이미 전환이 인플라이트 — 중복 요청 거부. */
		SPDK_NOTICELOG("Reactor(%u) is already in progress to set interrupt mode\n", lcore);
		return -EBUSY;
	}
	target->set_interrupt_mode_in_progress = true;          /* [한국어] 인플라이트 마킹 — 콜백 체인이 끝날 때 false 로 복귀. */

	target->new_in_interrupt = new_in_interrupt;            /* [한국어] 목표 모드 저장 — 콜백들이 참조. */
	target->set_interrupt_mode_cb_fn = cb_fn;               /* [한국어] 완료 콜백 저장. */
	target->set_interrupt_mode_cb_arg = cb_arg;             /* [한국어] 완료 콜백 인자 저장. */

	SPDK_DEBUGLOG(reactor, "Starting reactor event from %d to %d\n",
		      spdk_env_get_current_core(), lcore);

	if (new_in_interrupt == false) {                /* [한국어] interrupt → polling: 토글 먼저, 그 다음 notify clear. */
		/* For potential race cases, when setting the reactor to poll mode,
		 * first change the mode of the reactor and then clear the corresponding
		 * bit of the notify_cpuset of each reactor.
		 */
		_event_call(lcore, _reactor_set_interrupt_mode, target, NULL);  /* [한국어] target 자기 코어에서 토글 — 그 안에서 spdk_for_each_reactor(notify_cpuset clear) 호출. */
	} else {                                        /* [한국어] polling → interrupt: notify set 먼저, 그 다음 토글. */
		/* For race cases, when setting the reactor to interrupt mode, first set the
		 * corresponding bit of the notify_cpuset of each reactor and then change the mode.
		 */
		spdk_for_each_reactor(_reactor_set_notify_cpuset, target, NULL, _reactor_set_notify_cpuset_cpl);        /* [한국어] 모든 reactor 가 notify 비트 set 후 _cpl 에서 토글로 진행. */
	}

	return 0;
}

/*
 * [한국어]
 * spdk_event_allocate - cross-reactor 이벤트 객체 할당 (mempool get)
 *
 * @lcore: 이벤트가 실행될 대상 코어. event->lcore 에 저장.
 * @fn: 콜백 함수 (event_queue_run_batch 가 dequeue 후 호출).
 * @arg1, arg2: 콜백 인자 (불투명 포인터).
 * @return: 채워진 spdk_event 또는 NULL (mempool 고갈 / 잘못된 lcore).
 *
 * 호출 컨텍스트: 임의의 reactor / non-reactor 컨텍스트. mempool 은 lockless
 * MP-safe. 주의: 반환된 event 는 spdk_event_call 로 enqueue 해야 메모리 회수됨
 * (호출 안 하고 버리면 mempool leak).
 *
 * 호출 체인:
 *   _event_call / 사용자 코드 → [spdk_event_allocate] → spdk_mempool_get
 */
struct spdk_event *
spdk_event_allocate(uint32_t lcore, spdk_event_fn fn, void *arg1, void *arg2)
{
	struct spdk_event *event = NULL;                                /* [한국어] mempool 에서 받을 객체. */
	struct spdk_reactor *reactor = spdk_reactor_get(lcore);         /* [한국어] 대상 코어 검증용. */

	if (!reactor) {                         /* [한국어] 잘못된 lcore — 디버그 abort. */
		assert(false);
		return NULL;
	}

	event = spdk_mempool_get(g_spdk_event_mempool);                 /* [한국어] DPDK rte_mempool_get — per-core cache 우선. */
	if (event == NULL) {                    /* [한국어] mempool 고갈 — 16383 개 모두 인플라이트 (이론상 불가). */
		assert(false);
		return NULL;
	}

	event->lcore = lcore;                   /* [한국어] 대상 코어 — event_queue_run_batch 의 reactor 와 일치 가정. */
	event->fn = fn;                         /* [한국어] 실행할 함수. */
	event->arg1 = arg1;                     /* [한국어] 첫 인자. */
	event->arg2 = arg2;                     /* [한국어] 둘째 인자. */

	return event;
}

/*
 * [한국어]
 * spdk_event_call - 이벤트를 대상 reactor 의 ring 에 push (필요 시 깨움)
 *
 * @event: spdk_event_allocate 로 받은 객체. 호출 후 소유권은 ring 으로 이양.
 *
 * 핵심 cross-reactor 메시지 전달 API. SPDK 의 lockless 메시징 기반.
 *
 * 동작 단계:
 *   1) 대상 reactor 의 events ring 에 enqueue (MP-SC, lockless).
 *   2) 호출이 reactor 컨텍스트 외부거나, 호출자 reactor 의 notify_cpuset 에서
 *      대상 코어 비트가 set 이면 events_fd 에 write — interrupt 모드인 대상이
 *      epoll wait 에서 깨어나도록.
 *
 * 실행 컨텍스트: reactor pthread, DPDK lcore, 또는 임의 외부 스레드.
 * 동기화: ring 자체는 lockless. eventfd write 는 8B 카운터 atomic.
 *
 * 호출 체인:
 *   광범위 — 다른 코어에 작업을 디스패치하는 모든 곳.
 */
void
spdk_event_call(struct spdk_event *event)
{
	int rc;                                 /* [한국어] 시스템 콜/ring API 반환값. */
	struct spdk_reactor *reactor;           /* [한국어] 대상 reactor. */
	struct spdk_reactor *local_reactor = NULL;      /* [한국어] 호출자 reactor (없을 수도 있음). */
	uint32_t current_core = spdk_env_get_current_core();    /* [한국어] 호출 시점의 lcore (DPDK 외 스레드면 SPDK_ENV_LCORE_ID_ANY). */

	reactor = spdk_reactor_get(event->lcore);

	assert(reactor != NULL);
	assert(reactor->events != NULL);

	rc = spdk_ring_enqueue(reactor->events, (void **)&event, 1, NULL);      /* [한국어] DPDK rte_ring_enqueue — lockless MPSC. count 매개변수 NULL 이면 검사 없이 그냥 enqueue. */
	if (rc != 1) {
		assert(false);                  /* [한국어] ring 가득참 — 65536 슬롯 가정 시 매우 비정상. */
	}

	if (current_core != SPDK_ENV_LCORE_ID_ANY) {            /* [한국어] reactor 컨텍스트 내부면 local_reactor 식별. */
		local_reactor = spdk_reactor_get(current_core);
	}

	/* If spdk_event_call isn't called on a reactor, always send a notification.
	 * If it is called on a reactor, send a notification if the destination reactor
	 * is indicated in interrupt mode state.
	 */
	if (spdk_unlikely(local_reactor == NULL) ||                                                     /* [한국어] reactor 외부 스레드 — 안전하게 항상 깨움. */
	    spdk_unlikely(spdk_cpuset_get_cpu(&local_reactor->notify_cpuset, event->lcore))) {          /* [한국어] 호출자 reactor 의 notify 비트 — 대상이 interrupt 모드면 set 되어 있음. */
		uint64_t notify = 1;            /* [한국어] eventfd 카운터 증가 값. */

		rc = write(reactor->events_fd, &notify, sizeof(notify));        /* [한국어] eventfd write — 대상 reactor 의 epoll/poll 을 깨움. polling 모드 reactor 면 무해 (그냥 카운터 증가). */
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify event queue: %s.\n", spdk_strerror(errno));
		}
	}
}

/*
 * [한국어]
 * event_queue_run_batch - reactor 의 events ring 에서 한 배치 dequeue 후 실행
 *
 * @arg: spdk_reactor * (fd_group 콜백 시그니처 호환을 위해 void *).
 * @return: 처리한 이벤트 수 (음수면 -errno).
 *
 * polling 루프의 첫 단계 — 다른 reactor/외부에서 보낸 메시지를 처리한다.
 * SPDK_EVENT_BATCH_SIZE = 8 개씩 dequeue. interrupt 모드면 dequeue 후에도
 * ring 에 잔여가 있으면 events_fd 를 다시 write 해서 다음 epoll wait 에서
 * 깨어나도록 (level-trigger 효과 시뮬레이션).
 *
 * 동작 단계:
 *   1) (interrupt 모드) ring dequeue → 잔여 있으면 events_fd 재깨움.
 *   2) (polling 모드) 그냥 ring dequeue.
 *   3) 각 event 에 대해 fn(arg1, arg2) 호출. 호출 시점의 spdk_thread 는 NULL
 *      (이벤트는 thread 컨텍스트가 아닌 reactor 자체 컨텍스트).
 *   4) 처리한 이벤트 객체들을 mempool 로 bulk 반환.
 *
 * 실행 컨텍스트: 자기 reactor 의 polling 루프 (또는 fd_group wait 콜백).
 *
 * 호출 체인:
 *   _reactor_run / fd_group_wait 콜백 → [event_queue_run_batch] → event->fn
 */
static inline int
event_queue_run_batch(void *arg)
{
	struct spdk_reactor *reactor = arg;             /* [한국어] 자기 reactor. */
	size_t count, i;                                /* [한국어] dequeue 한 개수, 순회 인덱스. */
	void *events[SPDK_EVENT_BATCH_SIZE];            /* [한국어] dequeue 임시 배열 — 스택 위 8개 슬롯. */

#ifdef DEBUG
	/*
	 * spdk_ring_dequeue() fills events and returns how many entries it wrote,
	 * so we will never actually read uninitialized data from events, but just to be sure
	 * (and to silence a static analyzer false positive), initialize the array to NULL pointers.
	 */
	memset(events, 0, sizeof(events));              /* [한국어] 정적 분석기 false positive 방지 — release 빌드에서는 생략. */
#endif

	/* Operate event notification if this reactor currently runs in interrupt state */
	if (spdk_unlikely(reactor->in_interrupt)) {     /* [한국어] interrupt 모드: 잔여 이벤트 처리를 위해 self-notify 필요. */
		uint64_t notify = 1;
		int rc;

		count = spdk_ring_dequeue(reactor->events, events, SPDK_EVENT_BATCH_SIZE);      /* [한국어] 최대 8 개 dequeue. */

		if (spdk_ring_count(reactor->events) != 0) {            /* [한국어] dequeue 후에도 잔여 있으면 다음 wait 에서 즉시 깨어나도록. */
			/* Trigger new notification if there are still events in event-queue waiting for processing. */
			rc = write(reactor->events_fd, &notify, sizeof(notify));        /* [한국어] eventfd 재증가 — epoll level-trigger 흉내. */
			if (rc < 0) {
				SPDK_ERRLOG("failed to notify event queue: %s.\n", spdk_strerror(errno));
				return -errno;
			}
		}
	} else {                                        /* [한국어] polling 모드: 단순 dequeue. */
		count = spdk_ring_dequeue(reactor->events, events, SPDK_EVENT_BATCH_SIZE);
	}

	if (count == 0) {                       /* [한국어] dequeue 0개 — 빠른 반환. */
		return 0;
	}

	for (i = 0; i < count; i++) {
		struct spdk_event *event = events[i];

		assert(event != NULL);                          /* [한국어] dequeue 가 NULL 을 채울 일은 없지만 방어적. */
		assert(spdk_get_thread() == NULL);              /* [한국어] event 실행 시점에는 어떤 spdk_thread 도 활성이 아니어야 함 — thread 별 메시지는 spdk_thread_send_msg 의 영역. */
		SPDK_DTRACE_PROBE3(event_exec, event->fn,
				   event->arg1, event->arg2);   /* [한국어] USDT probe — 외부 트레이서가 이벤트 실행을 관측. */
		event->fn(event->arg1, event->arg2);            /* [한국어] 콜백 실행 — 콜백 안에서 더 많은 spdk_event_call 가능. */
	}

	spdk_mempool_put_bulk(g_spdk_event_mempool, events, count);     /* [한국어] 처리 완료된 이벤트들을 mempool 로 bulk 반환 — per-core cache 효율. */

	return (int)count;
}

/* 1s */
#define CONTEXT_SWITCH_MONITOR_PERIOD 1000000   /* [한국어] 컨텍스트 스위치 모니터 주기 (us, 1초). reactor_run 루프가 이 주기마다 getrusage 호출. polling 모델은 컨텍스트 스위치가 0 인 게 정상이므로 비정상 신호 가시화. */

/*
 * [한국어]
 * get_rusage - reactor 스레드의 컨텍스트 스위치 횟수를 로깅
 *
 * @reactor: 자기 reactor (자기 스레드의 rusage 만 측정).
 * @return: 항상 -1 (return 값 사용 안 함).
 *
 * RUSAGE_THREAD 로 호출 스레드(=현재 reactor) 의 voluntary/involuntary 컨텍스트
 * 스위치 횟수를 읽고, 마지막 측정 이후 증가가 있으면 INFOLOG. polling 모델에서
 * 컨텍스트 스위치는 system call(write 등) 로 발생할 수 있으나 이상적으론 0.
 *
 * 호출 체인:
 *   reactor_run → [get_rusage] → getrusage(2)
 */
static int
get_rusage(struct spdk_reactor *reactor)
{
	struct rusage		rusage;          /* [한국어] getrusage 결과 임시 저장. */

	if (getrusage(RUSAGE_THREAD, &rusage) != 0) {   /* [한국어] RUSAGE_THREAD = 호출 스레드 한정 통계. RUSAGE_SELF 는 프로세스 전체. */
		return -1;
	}

	if (rusage.ru_nvcsw != reactor->rusage.ru_nvcsw || rusage.ru_nivcsw != reactor->rusage.ru_nivcsw) {  /* [한국어] 직전 측정 대비 변화 있을 때만 로그 — 노이즈 방지. */
		SPDK_INFOLOG(reactor,
			     "Reactor %d: %ld voluntary context switches and %ld involuntary context switches in the last second.\n",
			     reactor->lcore, rusage.ru_nvcsw - reactor->rusage.ru_nvcsw,
			     rusage.ru_nivcsw - reactor->rusage.ru_nivcsw);
	}
	reactor->rusage = rusage;       /* [한국어] 다음 비교를 위해 현재 값 저장. */

	return -1;                      /* [한국어] 호출자가 사용하지 않음 — 항상 -1. */
}

/*
 * [한국어]
 * spdk_framework_enable_context_switch_monitor - 모니터 켜기/끄기 (외부 API)
 *
 * @enable: true 면 매 초 getrusage, false 면 비활성.
 *
 * 글로벌 bool 토글. 다른 스레드가 약간 늦게 보이는 것은 무해.
 */
void
spdk_framework_enable_context_switch_monitor(bool enable)
{
	/* This global is being read by multiple threads, so this isn't
	 * strictly thread safe. However, we're toggling between true and
	 * false here, and if a thread sees the value update later than it
	 * should, it's no big deal. */
	g_framework_context_switch_monitor_enabled = enable;    /* [한국어] 단순 store — atomicity 불필요 (bool 자체 atomicity 가정). */
}

/*
 * [한국어]
 * spdk_framework_context_switch_monitor_enabled - 모니터 활성화 여부 조회
 *
 * @return: 현재 enabled 상태.
 */
bool
spdk_framework_context_switch_monitor_enabled(void)
{
	return g_framework_context_switch_monitor_enabled;
}

/*
 * [한국어]
 * _set_thread_name - POSIX 스레드 이름 설정 (플랫폼별 분기)
 *
 * @thread_name: 16자 이하 이름 (Linux 제한).
 *
 * reactor pthread 가 자신의 이름을 "reactor_<lcore>" 로 변경 — top/htop 에서
 * 식별 가능. Linux: prctl(PR_SET_NAME), FreeBSD: pthread_set_name_np, 그 외:
 * pthread_setname_np (POSIX).
 *
 * 호출 체인:
 *   reactor_run → [_set_thread_name] → prctl/pthread_setname_np
 */
static void
_set_thread_name(const char *thread_name)
{
#if defined(__linux__)
	prctl(PR_SET_NAME, thread_name, 0, 0, 0);               /* [한국어] Linux: prctl 로 호출 스레드 이름 설정. /proc/self/task/<tid>/comm 에 반영. */
#elif defined(__FreeBSD__)
	pthread_set_name_np(pthread_self(), thread_name);       /* [한국어] FreeBSD non-portable extension. */
#else
	pthread_setname_np(pthread_self(), thread_name);        /* [한국어] POSIX 표준 (반환값 무시). */
#endif
}

/*
 * [한국어]
 * _init_thread_stats - lw_thread 의 한 스케줄링 주기 통계 갱신
 *
 * @reactor: lw_thread 가 속한 reactor (현재 사용 안 함, 시그니처 유지).
 * @lw_thread: 통계를 갱신할 lw_thread.
 *
 * total_stats 의 누적값을 새로 읽어, 직전 값과의 차이를 current_stats 에 저장.
 * scheduler 가 current_stats 만 보면 이번 주기의 부하만 알 수 있음.
 *
 * 동작 단계:
 *   1) prev_total_stats 에 직전 누적값 백업.
 *   2) spdk_set_thread(thread) 후 spdk_thread_get_stats 로 새 누적값 읽음.
 *   3) current = total - prev.
 *
 * 실행 컨텍스트: gather_metrics 페이즈 — 자기 reactor 의 lw_thread 들 처리.
 *
 * 호출 체인:
 *   _reactors_scheduler_gather_metrics → [_init_thread_stats] → spdk_thread_get_stats
 */
static void
_init_thread_stats(struct spdk_reactor *reactor, struct spdk_lw_thread *lw_thread)
{
	struct spdk_thread *thread = spdk_thread_get_from_ctx(lw_thread);       /* [한국어] lw_thread → spdk_thread. */
	struct spdk_thread_stats prev_total_stats;                              /* [한국어] 갱신 전 누적값 백업. */

	/* Read total_stats before updating it to calculate stats during the last scheduling period. */
	prev_total_stats = lw_thread->total_stats;      /* [한국어] 구조체 복사 — 값 단위. */

	spdk_set_thread(thread);                                /* [한국어] thread context 설정 — get_stats 가 TLS 의 현재 thread 를 읽음. */
	spdk_thread_get_stats(&lw_thread->total_stats);         /* [한국어] thread 의 누적 busy/idle TSC 읽어 total_stats 갱신. */
	spdk_set_thread(NULL);                                  /* [한국어] context 클리어 — event 실행 시점은 thread context 없어야 한다는 가정 유지. */

	lw_thread->current_stats.busy_tsc = lw_thread->total_stats.busy_tsc - prev_total_stats.busy_tsc;        /* [한국어] 이번 주기 동안 busy 였던 TSC. */
	lw_thread->current_stats.idle_tsc = lw_thread->total_stats.idle_tsc - prev_total_stats.idle_tsc;        /* [한국어] 이번 주기 동안 idle 였던 TSC. */
}

/*
 * [한국어]
 * _threads_reschedule_thread - 단일 thread 재배치 요청 마킹
 *
 * @thread_info: 스케줄러가 결정한 새 lcore 정보를 담은 객체.
 *
 * thread_id 로 spdk_thread 핸들 복원 (그 사이 destroy 됐으면 그냥 skip).
 * lw_thread->lcore 와 resched 플래그를 set — 실제 이동은 reactor_run 의
 * reactor_post_process_lw_thread 가 _reactor_schedule_thread 호출로 수행.
 *
 * 호출 체인:
 *   _reactors_scheduler_fini → _threads_reschedule → [_threads_reschedule_thread]
 */
static void
_threads_reschedule_thread(struct spdk_scheduler_thread_info *thread_info)
{
	struct spdk_lw_thread *lw_thread;       /* [한국어] thread 의 lw 컨텍스트. */
	struct spdk_thread *thread;             /* [한국어] thread 핸들. */

	thread = spdk_thread_get_by_id(thread_info->thread_id);         /* [한국어] thread_id 는 안정적 64-bit ID. 핸들 복원. */
	if (thread == NULL) {
		/* Thread no longer exists. */
		return;                                 /* [한국어] gather → balance 사이에 destroy 됐으면 무시. */
	}
	lw_thread = spdk_thread_get_ctx(thread);
	assert(lw_thread != NULL);

	lw_thread->lcore = thread_info->lcore;          /* [한국어] 원하는 새 lcore 표시 — 0 인덱스 부터 g_reactor_count 미만. */
	lw_thread->resched = true;                      /* [한국어] 재배치 요청 플래그 — reactor_run 이 다음 라운드에 처리. */
}

/*
 * [한국어]
 * _threads_reschedule - balance 결과를 토대로 thread 들의 lcore 재마킹
 *
 * @cores_info: 코어별 thread 배치 결정 결과 배열.
 *
 * scheduler->balance() 가 thread_info[*].lcore 를 새 코어로 갱신해두면, 이
 * 함수가 원래 코어와 비교하여 다르면 _threads_reschedule_thread 호출.
 * isolated 코어 보호: src 또는 dst 가 isolated 면 스킵하고 ERRLOG.
 *
 * 호출 체인:
 *   _reactors_scheduler_fini → [_threads_reschedule]
 */
static void
_threads_reschedule(struct spdk_scheduler_core_info *cores_info)
{
	struct spdk_scheduler_core_info *core;          /* [한국어] 현재 순회 중 core_info. */
	struct spdk_scheduler_thread_info *thread_info; /* [한국어] core 안의 j 번째 thread_info. */
	uint32_t i, j;                                  /* [한국어] 외부/내부 인덱스. */

	SPDK_ENV_FOREACH_CORE(i) {
		core = &cores_info[i];
		for (j = 0; j < core->threads_count; j++) {
			thread_info = &core->thread_infos[j];
			if (thread_info->lcore != i) {                  /* [한국어] balance 가 다른 코어로 옮기겠다고 결정한 경우. */
				if (core->isolated || cores_info[thread_info->lcore].isolated) {        /* [한국어] src 또는 dst 가 isolated 면 정책 위반. */
					SPDK_ERRLOG("A thread cannot be moved from an isolated core or \
								moved to an isolated core. Skip rescheduling thread\n");
					continue;
				}
				_threads_reschedule_thread(thread_info);        /* [한국어] 실제 thread 에 resched 마킹. */
			}
		}
		core->threads_count = 0;                /* [한국어] 사용 후 카운터 reset. */
		free(core->thread_infos);               /* [한국어] gather 가 calloc 한 배열 해제. */
		core->thread_infos = NULL;
	}
}

/*
 * [한국어]
 * _reactors_scheduler_fini - scheduler 라운드 마무리 (Phase 4)
 *
 * balance 결정 적용 + g_scheduling_in_progress 플래그 해제. 다음 주기 진입 가능.
 */
static void
_reactors_scheduler_fini(void)
{
	/* Reschedule based on the balancing output */
	_threads_reschedule(g_core_infos);              /* [한국어] balance 결과 적용. */

	g_scheduling_in_progress = false;               /* [한국어] 새 라운드 트리거 가능 상태로 복귀. */
}

/*
 * [한국어]
 * _reactors_scheduler_update_core_mode - Phase 3: 결정된 interrupt 모드 적용
 *
 * @ctx1, ctx2: spdk_reactor_set_interrupt_mode 의 cb 시그니처용 (사용 안 함).
 *
 * 비동기 chain. 하나의 코어 모드 전환이 끝나면 콜백으로 자기 자신이 다시
 * 불려서 다음 코어를 찾아 set_interrupt_mode 호출. 모든 코어가 일치하면
 * Phase 4 (_reactors_scheduler_fini) 진입.
 *
 * g_scheduler_core_number 는 chain 진행 상태 (다음 검사할 lcore).
 */
static void
_reactors_scheduler_update_core_mode(void *ctx1, void *ctx2)
{
	struct spdk_reactor *reactor;
	uint32_t i;
	int rc = 0;

	for (i = g_scheduler_core_number; i < SPDK_ENV_LCORE_ID_ANY; i = spdk_env_get_next_core(i)) {   /* [한국어] 진행 인덱스부터 끝까지 순회. SPDK_ENV_LCORE_ID_ANY = UINT32_MAX. */
		reactor = spdk_reactor_get(i);
		assert(reactor != NULL);
		if (reactor->in_interrupt != g_core_infos[i].interrupt_mode) {  /* [한국어] balance 가 결정한 모드와 현재 모드가 다르면 전환 필요. */
			/* Switch next found reactor to new state */
			rc = spdk_reactor_set_interrupt_mode(i, g_core_infos[i].interrupt_mode,
							     _reactors_scheduler_update_core_mode, NULL);       /* [한국어] 비동기 모드 전환 시작 — 완료 후 자기 자신이 콜백으로 호출됨. */
			if (rc == 0) {
				/* Set core to start with after callback completes */
				g_scheduler_core_number = spdk_env_get_next_core(i);    /* [한국어] 다음 라운드는 i 의 다음 코어부터 — 진행 위치 저장. */
				return;
			}
		}
	}
	_reactors_scheduler_fini();             /* [한국어] 모든 코어 처리 완료 — Phase 4 로 진입. */
}

/*
 * [한국어]
 * _reactors_scheduler_cancel - 진행 중 라운드 취소 (메모리만 해제)
 *
 * @arg1, arg2: unused.
 *
 * gather_metrics 중간에 실패(예: thread_infos 할당 실패) 했을 때 호출.
 * 모든 core_info->thread_infos 해제 + g_scheduling_in_progress 해제.
 *
 * 호출 체인:
 *   _reactors_scheduler_gather_metrics(에러시) / _reactors_scheduler_balance(에러시)
 *      → _event_call(scheduling_lcore) → [_reactors_scheduler_cancel]
 */
static void
_reactors_scheduler_cancel(void *arg1, void *arg2)
{
	struct spdk_scheduler_core_info *core;
	uint32_t i;

	SPDK_ENV_FOREACH_CORE(i) {
		core = &g_core_infos[i];
		core->threads_count = 0;        /* [한국어] gather 가 채운 카운터 무효화. */
		free(core->thread_infos);       /* [한국어] gather 가 calloc 한 배열 해제. */
		core->thread_infos = NULL;
	}

	g_scheduling_in_progress = false;       /* [한국어] 다음 라운드 가능. */
}

/*
 * [한국어]
 * _reactors_scheduler_balance - Phase 2: 부하분산 결정 호출
 *
 * @arg1, arg2: unused.
 *
 * scheduling reactor 에서 실행. scheduler->balance() 플러그인 콜백을 호출해
 * g_core_infos 의 thread_info[*].lcore 가 새 배치 결과로 갱신되게 한다.
 * 그 후 Phase 3 (update_core_mode) 진입.
 *
 * 가드: 셧다운 중이거나 scheduler 가 NULL 이면 즉시 cancel.
 */
static void
_reactors_scheduler_balance(void *arg1, void *arg2)
{
	struct spdk_scheduler *scheduler = spdk_scheduler_get();

	if (g_reactor_state != SPDK_REACTOR_STATE_RUNNING || scheduler == NULL) {       /* [한국어] 셧다운 또는 비활성 — 안전하게 cancel. */
		_reactors_scheduler_cancel(NULL, NULL);
		return;
	}

	scheduler->balance(g_core_infos, g_reactor_count);      /* [한국어] 플러그인 콜백 — thread_info[*].lcore 변경 가능. */

	g_scheduler_core_number = spdk_env_get_first_core();    /* [한국어] update_core_mode 의 진행 인덱스 시작값. */
	_reactors_scheduler_update_core_mode(NULL, NULL);       /* [한국어] Phase 3 진입. */
}

/* Phase 1 of thread scheduling is to gather metrics on the existing threads */
/*
 * [한국어]
 * _reactors_scheduler_gather_metrics - Phase 1: 메트릭 수집 (코어 순회 chain)
 *
 * @arg1, arg2: unused.
 *
 * 자기 코어의 reactor 통계와 lw_thread 통계를 g_core_infos[lcore] 에 채운 후,
 * 다음 코어로 _event_call. 한 바퀴 돌고 scheduling_lcore 로 돌아오면 Phase 2
 * (_reactors_scheduler_balance) 로 진입.
 *
 * 동작 단계:
 *   1) 자기 reactor 의 idle/busy_tsc 차분, in_interrupt, isolated 플래그 채움.
 *   2) thread_count > 0 면 thread_infos 배열 calloc.
 *      각 lw_thread 에 대해 _init_thread_stats 로 current_stats 갱신, copy.
 *   3) 다음 코어 결정 (현재 코어의 다음 활성 코어, 끝이면 첫 코어로 wrap).
 *   4) 다음 코어가 scheduling_lcore 면 balance 호출, 아니면 자기 자신 chain.
 *
 * 호출 체인 (chain):
 *   reactor_run(scheduling reactor) → [gather_metrics]
 *      → _event_call(next core) → [gather_metrics] → ... → _event_call(balance)
 */
static void
_reactors_scheduler_gather_metrics(void *arg1, void *arg2)
{
	struct spdk_scheduler_core_info *core_info;     /* [한국어] 자기 코어의 메트릭 슬롯. */
	struct spdk_lw_thread *lw_thread;               /* [한국어] 순회 중 lw_thread. */
	struct spdk_thread *thread;                     /* [한국어] thread 핸들. */
	struct spdk_reactor *reactor;                   /* [한국어] 자기 reactor. */
	uint32_t next_core;                             /* [한국어] chain 의 다음 코어. */
	uint32_t i = 0;                                 /* [한국어] thread_infos 배열 인덱스. */

	reactor = spdk_reactor_get(spdk_env_get_current_core());
	assert(reactor != NULL);
	core_info = &g_core_infos[reactor->lcore];
	core_info->lcore = reactor->lcore;                                                      /* [한국어] 자기 코어 번호 명시. */
	core_info->current_idle_tsc = reactor->idle_tsc - core_info->total_idle_tsc;            /* [한국어] 이번 주기 idle TSC = 누적 - 직전 누적. */
	core_info->total_idle_tsc = reactor->idle_tsc;                                          /* [한국어] 누적값 갱신. */
	core_info->current_busy_tsc = reactor->busy_tsc - core_info->total_busy_tsc;            /* [한국어] 이번 주기 busy TSC. */
	core_info->total_busy_tsc = reactor->busy_tsc;
	core_info->interrupt_mode = reactor->in_interrupt;                                      /* [한국어] 현재 모드 — balance 가 다음 모드 결정용으로 참조. */
	core_info->threads_count = 0;
	core_info->isolated = scheduler_is_isolated_core(reactor->lcore);                       /* [한국어] isolated 마스크 캐싱. */

	SPDK_DEBUGLOG(reactor, "Gathering metrics on %u\n", reactor->lcore);

	spdk_trace_record(TRACE_SCHEDULER_CORE_STATS, reactor->trace_id, 0, 0,
			  core_info->current_busy_tsc,
			  core_info->current_idle_tsc);                                         /* [한국어] tracing — 외부 분석 도구가 코어별 busy/idle 추적. */

	if (reactor->thread_count > 0) {        /* [한국어] thread 가 있는 코어만 thread_infos 할당. */
		core_info->thread_infos = calloc(reactor->thread_count, sizeof(*core_info->thread_infos));
		if (core_info->thread_infos == NULL) {
			SPDK_ERRLOG("Failed to allocate memory when gathering metrics on %u\n", reactor->lcore);

			/* Cancel this round of schedule work */
			_event_call(spdk_scheduler_get_scheduling_lcore(), _reactors_scheduler_cancel, NULL, NULL);  /* [한국어] 메모리 부족 — 라운드 취소를 scheduling reactor 에서 실행. */
			return;
		}

		TAILQ_FOREACH(lw_thread, &reactor->threads, link) {     /* [한국어] reactor 의 모든 lw_thread 순회 — 안전 (이 reactor 에서만 수정). */
			_init_thread_stats(reactor, lw_thread);         /* [한국어] current_stats 갱신. */

			core_info->thread_infos[i].lcore = lw_thread->lcore;            /* [한국어] 현재 lcore 기록 — balance 가 새 lcore 로 덮어쓸 수 있음. */
			thread = spdk_thread_get_from_ctx(lw_thread);
			assert(thread != NULL);
			core_info->thread_infos[i].thread_id = spdk_thread_get_id(thread);      /* [한국어] 안정적 thread ID 저장 — balance 후에도 thread 식별 가능. */
			core_info->thread_infos[i].total_stats = lw_thread->total_stats;
			core_info->thread_infos[i].current_stats = lw_thread->current_stats;
			core_info->threads_count++;
			assert(core_info->threads_count <= reactor->thread_count);

			spdk_trace_record(TRACE_SCHEDULER_THREAD_STATS, spdk_thread_get_trace_id(thread), 0, 0,
					  lw_thread->current_stats.busy_tsc,
					  lw_thread->current_stats.idle_tsc);                   /* [한국어] thread 별 통계 trace. */

			i++;
		}
	}

	next_core = spdk_env_get_next_core(reactor->lcore);
	if (next_core == UINT32_MAX) {                  /* [한국어] 마지막 코어였으면 첫 코어로 wrap. */
		next_core = spdk_env_get_first_core();
	}

	/* If we've looped back around to the scheduler thread, move to the next phase */
	if (next_core == spdk_scheduler_get_scheduling_lcore()) {
		/* Phase 2 of scheduling is rebalancing - deciding which threads to move where */
		_event_call(next_core, _reactors_scheduler_balance, NULL, NULL);        /* [한국어] 한 바퀴 돌았음 — balance 페이즈로 진입. */
		return;
	}

	_event_call(next_core, _reactors_scheduler_gather_metrics, NULL, NULL); /* [한국어] 다음 코어로 chain 계속. */
}

static int _reactor_schedule_thread(struct spdk_thread *thread);        /* [한국어] forward decl — thread 를 코어 중 하나에 배치하는 라운드로빈/cpumask 알고리즘. */
static uint64_t g_rusage_period;        /* [한국어] context-switch 모니터 주기 (TSC 단위). spdk_reactors_start 가 us → TSC 변환해 저장. */

/*
 * [한국어]
 * _reactor_remove_lw_thread - reactor 의 thread 리스트에서 lw_thread 제거
 *
 * @reactor: 소유 reactor (자기 코어).
 * @lw_thread: 제거할 lw_thread.
 *
 * thread 종료/재배치 시점에 호출. interrupt 모드라면 thread fd_group 도
 * reactor fgrp 에서 unnest. thread 객체 자체는 호출자가 destroy 또는 재
 * schedule.
 *
 * 호출 체인:
 *   reactor_post_process_lw_thread → [_reactor_remove_lw_thread]
 *   reactor_run(종료 정리) → [_reactor_remove_lw_thread]
 */
static void
_reactor_remove_lw_thread(struct spdk_reactor *reactor, struct spdk_lw_thread *lw_thread)
{
	struct spdk_thread	*thread = spdk_thread_get_from_ctx(lw_thread);  /* [한국어] thread 핸들. */
	struct spdk_fd_group	*grp;                                           /* [한국어] thread 의 interrupt fd_group. */

	TAILQ_REMOVE(&reactor->threads, lw_thread, link);       /* [한국어] BSD TAILQ 매크로 — link 필드로 제거. */
	assert(reactor->thread_count > 0);                      /* [한국어] 0 이면 누군가 카운트를 잘못 다룸 — 디버그 abort. */
	reactor->thread_count--;

	/* Operate thread intr if running with full interrupt ability */
	if (spdk_interrupt_mode_is_enabled()) {
		if (reactor->in_interrupt) {
			grp = spdk_thread_get_interrupt_fd_group(thread);
			spdk_fd_group_unnest(reactor->fgrp, grp);       /* [한국어] reactor fgrp 에서 자식 fd_group 분리. */
		}
	}
}

/*
 * [한국어]
 * reactor_post_process_lw_thread - polling 한 번 후 thread 의 종료/재배치 처리
 *
 * @reactor: 자기 reactor.
 * @lw_thread: spdk_thread_poll 한 직후의 lw_thread.
 * @return: true 처리 (제거/재스케줄), false 그대로 유지.
 *
 * 두 시나리오:
 *   - thread 가 exited && idle: 안전하게 destroy.
 *   - resched && !bound: 다른 코어로 옮기기 (_reactor_schedule_thread).
 * spdk_unlikely 로 hot-path 분기 예측 최적화.
 *
 * 호출 체인:
 *   _reactor_run / reactor_schedule_thread_event → [reactor_post_process_lw_thread]
 */
static bool
reactor_post_process_lw_thread(struct spdk_reactor *reactor, struct spdk_lw_thread *lw_thread)
{
	struct spdk_thread *thread = spdk_thread_get_from_ctx(lw_thread);

	if (spdk_unlikely(spdk_thread_is_exited(thread) &&
			  spdk_thread_is_idle(thread))) {       /* [한국어] thread 종료 절차 끝나고 모든 메시지/poller 처리됨 — 파괴 가능. */
		_reactor_remove_lw_thread(reactor, lw_thread);
		spdk_thread_destroy(thread);                    /* [한국어] thread 자원 해제 (poller, msg ring 등). */
		return true;
	}

	if (spdk_unlikely(lw_thread->resched && !spdk_thread_is_bound(thread))) {       /* [한국어] 재배치 요청 — bound 코어 아니어야 이동 가능. */
		lw_thread->resched = false;
		_reactor_remove_lw_thread(reactor, lw_thread);  /* [한국어] 현재 reactor 에서 빼고. */
		_reactor_schedule_thread(thread);               /* [한국어] 새 코어로 배치 (라운드로빈 + cpumask). */
		return true;
	}

	return false;
}

/*
 * [한국어]
 * reactor_interrupt_run - interrupt 모드에서 epoll/poll wait 한 번
 *
 * @reactor: 자기 reactor.
 *
 * fd_group_wait 가 events_fd / resched_fd / nested thread fd 들을 epoll 로
 * 모니터링하고 깨어나면 등록된 콜백 실행. block_timeout=-1 = 무한 대기.
 *
 * 호출 체인:
 *   reactor_run(loop, in_interrupt true) → [reactor_interrupt_run] → spdk_fd_group_wait → epoll_wait
 */
static void
reactor_interrupt_run(struct spdk_reactor *reactor)
{
	int block_timeout = -1; /* _EPOLL_WAIT_FOREVER */       /* [한국어] -1 = epoll_wait 무한 대기. eventfd 깨어남까지 CPU 0%. */

	spdk_fd_group_wait(reactor->fgrp, block_timeout);
}

/*
 * [한국어]
 * _reactor_run - polling 모드 한 라운드 (이벤트 + 모든 thread)
 *
 * @reactor: 자기 reactor.
 *
 * polling 모드의 hot-path. reactor_run 의 무한 루프가 매 iteration 마다 호출.
 *
 * 동작 단계:
 *   1) event_queue_run_batch — 다른 reactor 에서 보낸 메시지 8개 처리.
 *   2) thread 리스트가 비어있으면 idle_tsc 만 가산하고 반환.
 *   3) 모든 lw_thread 에 대해 spdk_thread_poll(0, tsc_last) 호출.
 *      반환값: 0 idle, >0 일을 함, <0 에러.
 *   4) thread 의 last_tsc 를 받아 idle/busy 누적 갱신.
 *   5) reactor_post_process_lw_thread 로 종료/재배치 처리.
 *
 * 실행 컨텍스트: 자기 reactor pthread (run-to-completion, 양보 없음).
 */
static void
_reactor_run(struct spdk_reactor *reactor)
{
	struct spdk_thread	*thread;
	struct spdk_lw_thread	*lw_thread, *tmp;       /* [한국어] _SAFE 순회 — post_process 가 리스트 수정 가능. */
	uint64_t		now;                    /* [한국어] thread poll 후의 last TSC. */
	int			rc;                     /* [한국어] thread_poll 반환값 (idle/busy/error). */

	event_queue_run_batch(reactor);                 /* [한국어] 1) 이벤트 8개 처리. */

	/* If no threads are present on the reactor,
	 * tsc_last gets outdated. Update it to track
	 * thread execution time correctly. */
	if (spdk_unlikely(TAILQ_EMPTY(&reactor->threads))) {    /* [한국어] thread 0 인 reactor — idle 시간만 누적. */
		now = spdk_get_ticks();                         /* [한국어] 현재 TSC. */
		reactor->idle_tsc += now - reactor->tsc_last;   /* [한국어] 직전 갱신 ~ 지금까지 = idle. */
		reactor->tsc_last = now;
		return;
	}

	TAILQ_FOREACH_SAFE(lw_thread, &reactor->threads, link, tmp) {
		thread = spdk_thread_get_from_ctx(lw_thread);
		rc = spdk_thread_poll(thread, 0, reactor->tsc_last);    /* [한국어] thread 의 poller 들을 한 번 호출. max_msgs=0 (제한 없음), now=tsc_last. */

		now = spdk_thread_get_last_tsc(thread);                 /* [한국어] thread 가 자기 작업 끝낸 시점 TSC. */
		if (rc == 0) {                                          /* [한국어] thread 가 idle — 일 안 했음. */
			reactor->idle_tsc += now - reactor->tsc_last;
		} else if (rc > 0) {                                    /* [한국어] thread 가 일을 함 (poller/msg). */
			reactor->busy_tsc += now - reactor->tsc_last;
		}
		reactor->tsc_last = now;                                /* [한국어] 다음 thread 측정용 시점 갱신. rc<0 (에러) 인 경우는 누적하지 않음. */

		reactor_post_process_lw_thread(reactor, lw_thread);     /* [한국어] 종료/재배치 후처리. */
	}
}

/*
 * [한국어]
 * reactor_run - reactor pthread 의 진입점 (무한 polling 루프)
 *
 * @arg: spdk_reactor * (spdk_env_thread_launch_pinned 콜백 시그니처).
 * @return: 0 (정상 종료).
 *
 * SPDK 의 핵심 실행 루프. 1 코어 = 1 reactor = 1 pthread (pinned).
 *
 * 동작 단계:
 *   1) 초기화: pthread 이름 "reactor_<lcore>" 로 변경, trace owner 등록,
 *      tsc_last 시작값.
 *   2) 무한 while(1):
 *      a) interrupt 모드면 reactor_interrupt_run (epoll wait), 아니면 _reactor_run (polling 1라운드).
 *      b) 매 1초마다 get_rusage 로 컨텍스트 스위치 모니터.
 *      c) scheduling reactor 이면 g_scheduler_period_in_tsc 마다 gather_metrics 트리거.
 *      d) g_reactor_state != RUNNING 이면 break.
 *   3) 종료 정리:
 *      a) 자신의 lw_thread 들에 spdk_thread_exit 호출 (app thread 제외).
 *         app thread 외에는 호출자가 미리 exit 했어야 — 안 했으면 ERRLOG.
 *      b) 모든 thread 가 destroy 될 때까지 polling 계속.
 *
 * 실행 컨텍스트: pinned pthread on lcore. 메인 코어는 spdk_reactors_start 의
 * 호출 컨텍스트 자체가 reactor_run 진입.
 *
 * 호출 체인:
 *   spdk_reactors_start → spdk_env_thread_launch_pinned → [reactor_run]
 */
static int
reactor_run(void *arg)
{
	struct spdk_reactor	*reactor = arg;         /* [한국어] 자기 reactor. */
	struct spdk_thread	*thread;                /* [한국어] 종료 정리용 임시. */
	struct spdk_lw_thread	*lw_thread, *tmp;       /* [한국어] 종료 정리용 _SAFE 순회. */
	char			thread_name[32];        /* [한국어] "reactor_<lcore>" 버퍼. */
	uint64_t		last_sched = 0;         /* [한국어] 마지막 스케줄링 트리거 TSC. */

	SPDK_NOTICELOG("Reactor started on core %u\n", reactor->lcore);

	/* Rename the POSIX thread because the reactor is tied to the POSIX
	 * thread in the SPDK event library.
	 */
	snprintf(thread_name, sizeof(thread_name), "reactor_%u", reactor->lcore);
	_set_thread_name(thread_name);                          /* [한국어] top/htop 가시성. */

	reactor->trace_id = spdk_trace_register_owner(OWNER_TYPE_REACTOR, thread_name); /* [한국어] trace 시스템에 owner 등록. spdk_trace_record 의 owner_id 로 사용. */

	reactor->tsc_last = spdk_get_ticks();                   /* [한국어] 첫 polling 시점 TSC — 이후 idle/busy 누적의 기준. */

	while (1) {
		/* Execute interrupt process fn if this reactor currently runs in interrupt state */
		if (spdk_unlikely(reactor->in_interrupt)) {     /* [한국어] interrupt 모드 (예외) — epoll wait. */
			reactor_interrupt_run(reactor);
		} else {                                        /* [한국어] polling 모드 (기본) — _reactor_run. */
			_reactor_run(reactor);
		}

		if (g_framework_context_switch_monitor_enabled) {
			if ((reactor->last_rusage + g_rusage_period) < reactor->tsc_last) {     /* [한국어] 마지막 측정 후 1초 경과. */
				get_rusage(reactor);
				reactor->last_rusage = reactor->tsc_last;       /* [한국어] 다음 측정 기준. */
			}
		}

		if (spdk_unlikely(g_scheduler_period_in_tsc > 0 &&                      /* [한국어] 스케줄링 활성. */
				  (reactor->tsc_last - last_sched) > g_scheduler_period_in_tsc &&       /* [한국어] 주기 만큼 경과. */
				  reactor == g_scheduling_reactor &&                    /* [한국어] 자기 자신이 트리거 책임자 reactor. */
				  !g_scheduling_in_progress)) {                         /* [한국어] 직전 라운드 미종료면 skip. */
			last_sched = reactor->tsc_last;
			g_scheduling_in_progress = true;
			spdk_trace_record(TRACE_SCHEDULER_PERIOD_START, 0, 0, 0);
			_reactors_scheduler_gather_metrics(NULL, NULL);                 /* [한국어] Phase 1 직접 호출 (자기 코어부터 시작). */
		}

		if (g_reactor_state != SPDK_REACTOR_STATE_RUNNING) {            /* [한국어] EXITING/SHUTDOWN 으로 전이됐으면 루프 탈출. */
			break;
		}
	}

	TAILQ_FOREACH(lw_thread, &reactor->threads, link) {
		thread = spdk_thread_get_from_ctx(lw_thread);
		/* All threads should have already had spdk_thread_exit() called on them, except
		 * for the app thread.
		 */
		if (spdk_thread_is_running(thread)) {                   /* [한국어] 아직 exit 호출 안 됐음. */
			if (!spdk_thread_is_app_thread(thread)) {       /* [한국어] app thread 는 정상적으로 마지막까지 살아있을 수 있음. */
				SPDK_ERRLOG("spdk_thread_exit() was not called on thread '%s'\n",
					    spdk_thread_get_name(thread));
				SPDK_ERRLOG("This will result in a non-zero exit code in a future release.\n");
			}
			spdk_set_thread(thread);                        /* [한국어] thread 컨텍스트 set 후 exit 호출 (스스로 exit 처럼). */
			spdk_thread_exit(thread);
		}
	}

	while (!TAILQ_EMPTY(&reactor->threads)) {                       /* [한국어] 모든 thread 가 destroy 될 때까지 polling. */
		TAILQ_FOREACH_SAFE(lw_thread, &reactor->threads, link, tmp) {
			thread = spdk_thread_get_from_ctx(lw_thread);
			spdk_set_thread(thread);
			if (spdk_thread_is_exited(thread)) {            /* [한국어] exit 절차 끝났으면 destroy. */
				_reactor_remove_lw_thread(reactor, lw_thread);
				spdk_thread_destroy(thread);
			} else {
				if (spdk_unlikely(reactor->in_interrupt)) {     /* [한국어] interrupt 모드면 fd_group_wait. */
					reactor_interrupt_run(reactor);
				} else {
					spdk_thread_poll(thread, 0, 0);         /* [한국어] polling 으로 thread 의 종료 메시지 처리 진행. */
				}
			}
		}
	}

	return 0;
}

/*
 * [한국어]
 * spdk_app_parse_core_mask - 사용자 입력 코어 마스크 파싱 + 유효 코어와 AND
 *
 * @mask: hex 문자열 코어 마스크 (예: "0xff").
 * @cpumask: 결과를 받을 cpuset 포인터.
 * @return: 0 성공, 음수 파싱 실패.
 *
 * 사용자가 지정한 마스크를 spdk_cpuset 으로 파싱한 뒤, 실제 활성 reactor
 * 코어 마스크 (g_reactor_core_mask) 와 AND 연산해 유효 부분만 남긴다.
 *
 * 호출 체인:
 *   bdev_module/RPC → [spdk_app_parse_core_mask]
 */
int
spdk_app_parse_core_mask(const char *mask, struct spdk_cpuset *cpumask)
{
	int ret;
	const struct spdk_cpuset *validmask;

	ret = spdk_cpuset_parse(cpumask, mask);                 /* [한국어] hex 문자열 → 비트맵. */
	if (ret < 0) {
		return ret;
	}

	validmask = spdk_app_get_core_mask();                   /* [한국어] 실제 활성 reactor 마스크. */
	spdk_cpuset_and(cpumask, validmask);                    /* [한국어] AND — 사용자 요청과 활성 코어의 교집합. */

	return 0;
}

/*
 * [한국어]
 * spdk_app_get_core_mask - 활성 reactor 코어 마스크 반환
 *
 * @return: g_reactor_core_mask 의 const 포인터.
 *
 * spdk_reactors_start 가 launch_pinned 성공한 코어를 set 한 결과.
 */
const struct spdk_cpuset *
spdk_app_get_core_mask(void)
{
	return &g_reactor_core_mask;
}

/*
 * [한국어]
 * spdk_reactors_start - 모든 reactor 시작 (블로킹, 종료까지 반환 안 함)
 *
 * SPDK 부팅 시퀀스의 마지막 단계. 메인 코어 외 각 코어에 reactor pthread 를
 * 핀하여 띄우고, 메인 코어는 호출 컨텍스트에서 직접 reactor_run 진입.
 * 모든 reactor 가 종료되면 thread_wait_all 로 join 후 SHUTDOWN 으로 전이.
 *
 * 동작 단계:
 *   1) g_rusage_period 를 us → TSC 변환.
 *   2) 상태 RUNNING 으로 전이, g_stopping_reactors 리셋.
 *   3) 메인 코어가 아닌 모든 코어에 spdk_env_thread_launch_pinned(reactor_run).
 *   4) 모든 활성 코어를 g_reactor_core_mask 에 set.
 *   5) 메인 코어에서 직접 reactor_run 진입 — 이 호출이 메인 reactor 루프.
 *   6) reactor_run 반환 후 spdk_env_thread_wait_all 로 다른 reactor pthread join.
 *   7) 상태 SHUTDOWN 전이.
 *
 * 실행 컨텍스트: 메인 pthread (앱 부팅 컨텍스트). 반환 시점에 모든 reactor 종료됨.
 *
 * 호출 체인:
 *   spdk_app_start (lib/event/app.c) → spdk_reactors_init → [spdk_reactors_start]
 */
void
spdk_reactors_start(void)
{
	struct spdk_reactor *reactor;
	uint32_t i, current_core;
	int rc;

	g_rusage_period = (CONTEXT_SWITCH_MONITOR_PERIOD * spdk_get_ticks_hz()) / SPDK_SEC_TO_USEC;     /* [한국어] 1,000,000 us * (TSC/sec) / (us/sec) = TSC. */
	g_reactor_state = SPDK_REACTOR_STATE_RUNNING;
	/* Reinitialize to false, in case the app framework is restarting in the same process. */
	g_stopping_reactors = false;            /* [한국어] 같은 프로세스에서 SPDK 가 stop → start 재시작될 수 있음. */

	current_core = spdk_env_get_current_core();
	SPDK_ENV_FOREACH_CORE(i) {
		if (i != current_core) {                /* [한국어] 메인 코어 제외 — 다른 코어에 pthread 핀. */
			reactor = spdk_reactor_get(i);
			if (reactor == NULL) {
				continue;
			}

			rc = spdk_env_thread_launch_pinned(reactor->lcore, reactor_run, reactor);       /* [한국어] DPDK rte_eal_remote_launch — 코어에 pthread 시작 + sched_setaffinity 로 핀. */
			if (rc < 0) {
				SPDK_ERRLOG("Unable to start reactor thread on core %u\n", reactor->lcore);
				assert(false);
				return;
			}
		}
		spdk_cpuset_set_cpu(&g_reactor_core_mask, i, true);     /* [한국어] 활성 코어 마스크에 추가 — spdk_app_get_core_mask 가 반환. */
	}

	/* Start the main reactor */
	reactor = spdk_reactor_get(current_core);
	assert(reactor != NULL);
	reactor_run(reactor);                   /* [한국어] 메인 코어에서 직접 진입 — 종료까지 반환 안 함. */

	spdk_env_thread_wait_all();             /* [한국어] DPDK rte_eal_mp_wait_lcore — 모든 다른 reactor pthread join. */

	g_reactor_state = SPDK_REACTOR_STATE_SHUTDOWN;
}

/*
 * [한국어]
 * _reactors_stop - 모든 reactor 의 events_fd 를 깨워 무한 루프 탈출 유도
 *
 * @arg1, arg2: spdk_for_each_reactor 의 cpl 시그니처용 (사용 안 함).
 *
 * 동작 단계:
 *   1) g_reactor_state = EXITING — reactor_run 의 break 조건 만족.
 *   2) 모든 코어에 대해 events_fd write — interrupt 모드 reactor 깨움.
 *
 * spdk_for_each_reactor 의 cpl 콜백으로 호출 — 모든 코어에서 nop 실행이
 * 끝난 후에야 종료 시작 (펜딩 작업 처리 후 안전 종료).
 *
 * 호출 체인:
 *   spdk_reactors_stop → spdk_for_each_reactor → cpl: [_reactors_stop]
 */
static void
_reactors_stop(void *arg1, void *arg2)
{
	uint32_t i;
	int rc;
	struct spdk_reactor *reactor;
	struct spdk_reactor *local_reactor;
	uint64_t notify = 1;

	g_reactor_state = SPDK_REACTOR_STATE_EXITING;           /* [한국어] reactor_run 의 break 트리거. */
	local_reactor = spdk_reactor_get(spdk_env_get_current_core());  /* [한국어] 호출 reactor (스케줄링 reactor). */

	SPDK_ENV_FOREACH_CORE(i) {
		/* If spdk_event_call isn't called  on a reactor, always send a notification.
		 * If it is called on a reactor, send a notification if the destination reactor
		 * is indicated in interrupt mode state.
		 */
		if (local_reactor == NULL || spdk_cpuset_get_cpu(&local_reactor->notify_cpuset, i)) {   /* [한국어] interrupt 모드 reactor 만 깨우면 됨 (polling 은 다음 iteration 에 자체 break). */
			reactor = spdk_reactor_get(i);
			assert(reactor != NULL);
			rc = write(reactor->events_fd, &notify, sizeof(notify));        /* [한국어] eventfd 깨움 — fd_group_wait 가 반환되어 break 검사. */
			if (rc < 0) {
				SPDK_ERRLOG("failed to notify event queue for reactor(%u): %s.\n", i, spdk_strerror(errno));
				continue;
			}
		}
	}
}

/*
 * [한국어]
 * nop - 빈 콜백 (spdk_for_each_reactor 의 fn 자리표시용)
 */
static void
nop(void *arg1, void *arg2)
{
}

/*
 * [한국어]
 * spdk_reactors_stop - reactor 종료 시퀀스 시작 (외부 API)
 *
 * @arg1: unused (시그니처 호환).
 *
 * spdk_for_each_reactor(nop, ..., _reactors_stop) — 모든 코어를 한 바퀴 돌고
 * 마지막에 _reactors_stop 가 EXITING 상태로 전이 + eventfd 깨움. 이 패턴은
 * 모든 코어가 자기 큐의 펜딩 작업을 처리한 후 안전하게 종료시킨다.
 */
void
spdk_reactors_stop(void *arg1)
{
	spdk_for_each_reactor(nop, NULL, NULL, _reactors_stop);
}

static pthread_mutex_t g_scheduler_mtx = PTHREAD_MUTEX_INITIALIZER;     /* [한국어] g_next_core 라운드로빈 인덱스 보호 mutex. 여러 reactor 가 동시에 새 thread 를 schedule 할 때 race 방지. */
static uint32_t g_next_core = UINT32_MAX;       /* [한국어] 라운드로빈 next core 인덱스. UINT32_MAX = "처음" 의미 — 첫 호출 시 spdk_env_get_first_core 로 초기화. */

/*
 * [한국어]
 * _schedule_thread - thread 를 자기 reactor 의 thread 리스트에 추가
 *
 * @arg1: spdk_lw_thread * (등록할 lw_thread).
 * @arg2: unused.
 *
 * spdk_event 콜백으로 호출됨 (target 코어에서). 동작 단계:
 *   1) total_stats 갱신 — 이동 시점의 누적값.
 *   2) initial_lcore 가 ANY 면 현재 코어로 설정.
 *   3) lcore = current_core, TAILQ_INSERT_TAIL.
 *   4) full interrupt 모드면 reactor fgrp 에 thread fd_group nest + thread 에
 *      메시지 보내 모드 정렬.
 *
 * 실행 컨텍스트: target reactor (자기 코어). lockless.
 *
 * 호출 체인:
 *   _reactor_schedule_thread → spdk_event_call(target) → [_schedule_thread]
 */
static void
_schedule_thread(void *arg1, void *arg2)
{
	struct spdk_lw_thread *lw_thread = arg1;
	struct spdk_thread *thread;
	struct spdk_reactor *reactor;
	uint32_t current_core;
	struct spdk_fd_group *grp;

	current_core = spdk_env_get_current_core();
	reactor = spdk_reactor_get(current_core);
	assert(reactor != NULL);

	/* Update total_stats to reflect state of thread
	* at the end of the move. */
	thread = spdk_thread_get_from_ctx(lw_thread);
	spdk_set_thread(thread);
	spdk_thread_get_stats(&lw_thread->total_stats);         /* [한국어] 이동 직후 시점의 누적 stats — 다음 gather 의 prev 기준. */
	spdk_set_thread(NULL);

	if (lw_thread->initial_lcore == SPDK_ENV_LCORE_ID_ANY) {        /* [한국어] 처음 schedule 되는 thread 면 initial 기록. */
		lw_thread->initial_lcore = current_core;
	}
	lw_thread->lcore = current_core;                                /* [한국어] 실제 배치된 코어 갱신. */

	TAILQ_INSERT_TAIL(&reactor->threads, lw_thread, link);          /* [한국어] reactor 의 thread 리스트에 추가 — _reactor_run 이 다음 iter 에 polling. */
	reactor->thread_count++;

	/* Operate thread intr if running with full interrupt ability */
	if (spdk_interrupt_mode_is_enabled()) {
		int rc;

		if (reactor->in_interrupt) {                            /* [한국어] interrupt 모드 reactor 면 thread fd 도 epoll 계층화. */
			grp = spdk_thread_get_interrupt_fd_group(thread);
			rc = spdk_fd_group_nest(reactor->fgrp, grp);
			if (rc < 0) {
				SPDK_ERRLOG("Failed to schedule spdk_thread: %s.\n", spdk_strerror(-rc));
			}
		}

		/* Align spdk_thread with reactor to interrupt mode or poll mode */
		spdk_thread_send_msg(thread, _reactor_set_thread_interrupt_mode, reactor);      /* [한국어] thread 에 자기 모드를 reactor 와 정렬하라는 메시지. */
	}
}

/*
 * [한국어]
 * _reactor_schedule_thread - 새 thread 또는 재배치 thread 의 코어 결정 + dispatch
 *
 * @thread: 배치할 spdk_thread.
 * @return: 0 성공, -1 실패.
 *
 * 핵심 알고리즘 (라운드로빈 + cpumask):
 *   1) thread 의 cpumask, 현재 lcore (재배치면 의도한 코어), initial_lcore 추출.
 *   2) lw_thread 0 으로 reset (단, initial_lcore 는 보존).
 *   3) full interrupt 비활성 시 polling reactor 만 후보 — interrupt 모드인
 *      reactor 는 thread polling 안 하므로 제외.
 *   4) g_scheduler_mtx 락 후, core == ANY 면 g_next_core 라운드로빈으로
 *      cpumask 안의 첫 코어 선택.
 *   5) 결정된 core 로 _schedule_thread 이벤트 dispatch (spdk_event_call).
 *   6) trace 기록 (이동된 경우).
 *
 * 호출 컨텍스트: thread NEW (spdk_thread_create 첫 호출) 또는 재배치 (resched).
 *
 * 호출 체인:
 *   reactor_thread_op(NEW) → [_reactor_schedule_thread] → _schedule_thread (target core)
 */
static int
_reactor_schedule_thread(struct spdk_thread *thread)
{
	uint32_t core, initial_core;
	struct spdk_lw_thread *lw_thread;
	struct spdk_event *evt = NULL;
	struct spdk_cpuset *cpumask;
	uint32_t i;
	struct spdk_reactor *local_reactor = NULL;
	uint32_t current_lcore = spdk_env_get_current_core();
	struct spdk_cpuset polling_cpumask;             /* [한국어] polling 모드인 reactor 들의 마스크. */
	struct spdk_cpuset valid_cpumask;               /* [한국어] 사용자 cpumask ∩ polling. */

	cpumask = spdk_thread_get_cpumask(thread);

	lw_thread = spdk_thread_get_ctx(thread);
	assert(lw_thread != NULL);
	core = lw_thread->lcore;                /* [한국어] 호출자가 지정한 의도 코어 (ANY 면 자유 선택). */
	initial_core = lw_thread->initial_lcore;
	memset(lw_thread, 0, sizeof(*lw_thread));       /* [한국어] lw_thread 재초기화 — total_stats 등 클리어. */
	lw_thread->initial_lcore = initial_core;        /* [한국어] initial 만 보존. */

	if (current_lcore != SPDK_ENV_LCORE_ID_ANY) {
		local_reactor = spdk_reactor_get(current_lcore);
		assert(local_reactor);
	}

	/* When interrupt ability of spdk_thread is not enabled and the current
	 * reactor runs on DPDK thread, skip reactors which are in interrupt mode.
	 */
	if (!spdk_interrupt_mode_is_enabled() && local_reactor != NULL) {
		/* Get the cpumask of all reactors in polling */
		spdk_cpuset_zero(&polling_cpumask);
		SPDK_ENV_FOREACH_CORE(i) {
			spdk_cpuset_set_cpu(&polling_cpumask, i, true);         /* [한국어] 모든 활성 코어 set. */
		}
		spdk_cpuset_xor(&polling_cpumask, &local_reactor->notify_cpuset);       /* [한국어] notify_cpuset (interrupt 모드 비트) 를 XOR — interrupt 코어를 제외. */

		if (core == SPDK_ENV_LCORE_ID_ANY) {
			/* Get the cpumask of all valid reactors which are suggested and also in polling */
			spdk_cpuset_copy(&valid_cpumask, &polling_cpumask);
			spdk_cpuset_and(&valid_cpumask, spdk_thread_get_cpumask(thread));       /* [한국어] 사용자 cpumask 와 polling 의 교집합. */

			/* If there are any valid reactors, spdk_thread should be scheduled
			 * into one of the valid reactors.
			 * If there is no valid reactors, spdk_thread should be scheduled
			 * into one of the polling reactors.
			 */
			if (spdk_cpuset_count(&valid_cpumask) != 0) {
				cpumask = &valid_cpumask;       /* [한국어] 사용자 의도 + polling 가능. */
			} else {
				cpumask = &polling_cpumask;     /* [한국어] 사용자 의도 코어가 모두 interrupt 모드 — 차선책. */
			}
		} else if (!spdk_cpuset_get_cpu(&polling_cpumask, core)) {
			/* If specified reactor is not in polling, spdk_thread should be scheduled
			 * into one of the polling reactors.
			 */
			core = SPDK_ENV_LCORE_ID_ANY;           /* [한국어] 지정 코어가 interrupt — ANY 로 fallback. */
			cpumask = &polling_cpumask;
		}
	}

	pthread_mutex_lock(&g_scheduler_mtx);           /* [한국어] g_next_core 라운드로빈 보호. */
	if (core == SPDK_ENV_LCORE_ID_ANY) {
		for (i = 0; i < spdk_env_get_core_count(); i++) {       /* [한국어] 최대 코어 수만큼 시도. */
			if (g_next_core >= g_reactor_count) {           /* [한국어] wrap-around. */
				g_next_core = spdk_env_get_first_core();
			}
			core = g_next_core;
			g_next_core = spdk_env_get_next_core(g_next_core);      /* [한국어] 다음 라운드용 인덱스 전진. */

			if (spdk_cpuset_get_cpu(cpumask, core)) {       /* [한국어] cpumask 에 포함된 코어면 채택. */
				break;
			}
		}
	}

	evt = spdk_event_allocate(core, _schedule_thread, lw_thread, NULL);     /* [한국어] target 코어로 보낼 이벤트. */

	if (current_lcore != core) {
		spdk_trace_record(TRACE_SCHEDULER_MOVE_THREAD, spdk_thread_get_trace_id(thread), 0, 0,
				  current_lcore, core);                 /* [한국어] thread 가 다른 코어로 이동 — trace 기록. */
	}

	pthread_mutex_unlock(&g_scheduler_mtx);

	assert(evt != NULL);
	if (evt == NULL) {
		SPDK_ERRLOG("Unable to schedule thread on requested core mask.\n");
		return -1;
	}

	lw_thread->tsc_start = spdk_get_ticks();        /* [한국어] thread 의 첫 시작 TSC — 통계용. */

	spdk_event_call(evt);                           /* [한국어] target reactor 의 events ring 에 push. */

	return 0;
}

/*
 * [한국어]
 * _reactor_request_thread_reschedule - thread 가 자기 자신을 재배치 요청
 *
 * @thread: 호출자 thread 자신 (spdk_get_thread() 와 일치 검증).
 *
 * thread 가 자기 컨텍스트에서 RESCHED 요청 시 호출. lw_thread 의 resched
 * 플래그 set + lcore = ANY (다음 라운드에 재배치). interrupt 모드면
 * resched_fd 깨움 (epoll wait 가 즉시 반환되어 reactor_schedule_thread_event
 * 콜백이 post_process 수행).
 *
 * 호출 체인:
 *   reactor_thread_op(RESCHED) → [_reactor_request_thread_reschedule]
 */
static void
_reactor_request_thread_reschedule(struct spdk_thread *thread)
{
	struct spdk_lw_thread *lw_thread;
	struct spdk_reactor *reactor;
	uint32_t current_core;

	assert(thread == spdk_get_thread());            /* [한국어] thread 가 자기 자신에 대해 요청해야 함 — 다른 thread 가 임의로 RESCHED 요청 불가. */

	lw_thread = spdk_thread_get_ctx(thread);

	assert(lw_thread != NULL);
	lw_thread->resched = true;
	lw_thread->lcore = SPDK_ENV_LCORE_ID_ANY;       /* [한국어] 새 코어를 라운드로빈으로 결정. */

	current_core = spdk_env_get_current_core();
	reactor = spdk_reactor_get(current_core);
	assert(reactor != NULL);

	/* Send a notification if the destination reactor is indicated in intr mode state */
	if (spdk_unlikely(spdk_cpuset_get_cpu(&reactor->notify_cpuset, reactor->lcore))) {       /* [한국어] 자기 reactor 가 interrupt 모드면 자기 resched_fd 를 깨워서 즉시 처리. */
		uint64_t notify = 1;

		if (write(reactor->resched_fd, &notify, sizeof(notify)) < 0) {
			SPDK_ERRLOG("failed to notify reschedule: %s.\n", spdk_strerror(errno));
		}
	}
}

/*
 * [한국어]
 * reactor_thread_op - spdk_thread_lib 가 thread 라이프사이클 이벤트 시 호출
 *
 * @thread: 대상 thread.
 * @op: SPDK_THREAD_OP_NEW (신규 생성) 또는 SPDK_THREAD_OP_RESCHED.
 * @return: 0 성공, -ENOTSUP 미지원 op.
 *
 * spdk_reactors_init 에서 spdk_thread_lib_init_ext 를 통해 thread lib 에
 * 등록한 콜백. thread lib 가 spdk_thread_create 시 NEW, send_msg 등으로
 * RESCHED 발생 시 이 함수를 호출.
 *
 * 호출 체인:
 *   spdk_thread_create / RESCHED 트리거 → [reactor_thread_op] → _reactor_schedule_thread/_request_reschedule
 */
static int
reactor_thread_op(struct spdk_thread *thread, enum spdk_thread_op op)
{
	struct spdk_lw_thread *lw_thread;

	switch (op) {
	case SPDK_THREAD_OP_NEW:
		lw_thread = spdk_thread_get_ctx(thread);
		lw_thread->lcore = SPDK_ENV_LCORE_ID_ANY;               /* [한국어] 신규 thread — 코어 미결정. */
		lw_thread->initial_lcore = SPDK_ENV_LCORE_ID_ANY;       /* [한국어] 첫 schedule 시 결정. */
		return _reactor_schedule_thread(thread);
	case SPDK_THREAD_OP_RESCHED:
		_reactor_request_thread_reschedule(thread);
		return 0;
	default:
		return -ENOTSUP;
	}
}

/*
 * [한국어]
 * reactor_thread_op_supported - thread lib 에 지원 op 알림
 *
 * @op: 검사할 op.
 * @return: NEW/RESCHED 만 true.
 */
static bool
reactor_thread_op_supported(enum spdk_thread_op op)
{
	switch (op) {
	case SPDK_THREAD_OP_NEW:
	case SPDK_THREAD_OP_RESCHED:
		return true;
	default:
		return false;
	}
}

/*
 * [한국어]
 * struct call_reactor - spdk_for_each_reactor 의 chain 컨텍스트
 *
 * 모든 코어 순회의 진행 상태와 사용자 콜백을 담는다.
 */
struct call_reactor {
	uint32_t cur_core;
	/* [한국어] chain 의 다음에 방문할 lcore.
	 * 설정자: spdk_for_each_reactor 가 first_core 로 초기화, on_reactor 가 next_core 로 갱신.
	 * 읽는 자: on_reactor 가 _event_call 의 target 으로 사용.
	 * 값 범위: 활성 lcore 또는 g_reactor_count (종료 신호).
	 * 동기화: chain 시점의 단일 reactor 만 접근 — 락 불필요. */

	spdk_event_fn fn;
	/* [한국어] 각 코어에서 실행할 사용자 콜백.
	 * 설정자: spdk_for_each_reactor.
	 * 읽는 자: on_reactor 가 매 코어에서 호출. */

	void *arg1;
	/* [한국어] fn / cpl 의 첫 인자.
	 * 설정자: spdk_for_each_reactor.
	 * 읽는 자: on_reactor / end_reactor. */

	void *arg2;
	/* [한국어] fn / cpl 의 둘째 인자. */

	uint32_t orig_core;
	/* [한국어] spdk_for_each_reactor 호출이 시작된 코어 — chain 끝나면 이 코어로 돌아와 cpl 실행.
	 * 설정자: spdk_for_each_reactor.
	 * 읽는 자: on_reactor 가 end_reactor 이벤트 dispatch 시 target. */

	spdk_event_fn cpl;
	/* [한국어] 모든 코어 순회 완료 후 호출되는 완료 콜백.
	 * 설정자: spdk_for_each_reactor.
	 * 읽는 자: end_reactor 가 호출. */
};

/*
 * [한국어]
 * on_reactor - 각 코어에서 실행되어 fn 호출 후 다음 코어로 이벤트 dispatch
 *
 * @arg1: call_reactor *.
 * @arg2: unused.
 *
 * 동작 단계:
 *   1) 사용자 fn(arg1, arg2) 호출.
 *   2) cur_core = next_core 로 갱신.
 *   3) 모든 코어 순회 끝났으면 (cur_core >= g_reactor_count) end_reactor 이벤트를
 *      orig_core 로 dispatch.
 *   4) 아니면 다음 코어로 on_reactor 이벤트 dispatch.
 *
 * 호출 체인:
 *   spdk_for_each_reactor → _event_call → [on_reactor] → ... chain ... → end_reactor
 */
static void
on_reactor(void *arg1, void *arg2)
{
	struct call_reactor *cr = arg1;
	struct spdk_event *evt;

	cr->fn(cr->arg1, cr->arg2);             /* [한국어] 자기 코어에서 사용자 콜백 실행. */

	cr->cur_core = spdk_env_get_next_core(cr->cur_core);    /* [한국어] 다음 활성 코어 (없으면 UINT32_MAX). */

	if (cr->cur_core >= g_reactor_count) {                  /* [한국어] 마지막 코어 지났음 → 종료 단계. */
		SPDK_DEBUGLOG(reactor, "Completed reactor iteration\n");

		evt = spdk_event_allocate(cr->orig_core, end_reactor, cr, NULL);        /* [한국어] orig_core 에서 cpl 실행할 이벤트. */
	} else {
		SPDK_DEBUGLOG(reactor, "Continuing reactor iteration to %d\n",
			      cr->cur_core);

		evt = spdk_event_allocate(cr->cur_core, on_reactor, arg1, NULL);        /* [한국어] 다음 코어로 chain. */
	}
	assert(evt != NULL);
	spdk_event_call(evt);
}

/*
 * [한국어]
 * end_reactor - 모든 코어 순회 완료 후 cpl 호출 + 컨텍스트 free
 *
 * @arg1: call_reactor *.
 * @arg2: unused.
 *
 * 호출 체인:
 *   on_reactor (마지막) → _event_call(orig_core) → [end_reactor] → cpl + free(cr)
 */
static void
end_reactor(void *arg1, void *arg2)
{
	struct call_reactor *cr = arg1;
	(void)arg2;

	cr->cpl(cr->arg1, cr->arg2);            /* [한국어] 사용자 완료 콜백. */

	free(cr);                               /* [한국어] chain 컨텍스트 해제 — 메모리 leak 방지. */
}

/*
 * [한국어]
 * spdk_for_each_reactor - 모든 reactor 에서 순차로 fn 실행 후 cpl 호출
 *
 * @fn: 각 reactor 에서 실행할 함수.
 * @arg1: fn 의 첫 인자 (cpl 도 동일하게 받음).
 * @arg2: fn 의 둘째 인자.
 * @cpl: 모든 reactor 가 fn 을 끝낸 후 호출자 코어에서 실행할 완료 콜백.
 *
 * 비동기 chain 패턴 — fn 은 코어 순서대로 실행되고, 마지막에 cpl 이 호출된 곳에서 호출됨.
 *
 * 셧다운 가드: g_stopping_reactors 가 true 면 무시 (이미 종료 진행 중).
 *  cpl == _reactors_stop 이면 마지막 한 번을 위해 g_stopping_reactors = true.
 *
 * 호출 체인:
 *   광범위 — RPC 핸들러, 모드 전환, 종료 등이 모든 코어에 대해 작업할 때.
 */
void
spdk_for_each_reactor(spdk_event_fn fn, void *arg1, void *arg2, spdk_event_fn cpl)
{
	struct call_reactor *cr;

	/* When the application framework is shutting down, we will send one
	 * final for_each_reactor operation with completion callback _reactors_stop,
	 * to flush any existing for_each_reactor operations to avoid any memory
	 * leaks. We use a mutex here to protect a boolean flag that will ensure
	 * we don't start any more operations once we've started shutting down.
	 */
	pthread_mutex_lock(&g_stopping_reactors_mtx);   /* [한국어] g_stopping_reactors 플래그 보호. */
	if (g_stopping_reactors) {                      /* [한국어] 이미 종료 시작됨 — 새 작업 거부. */
		pthread_mutex_unlock(&g_stopping_reactors_mtx);
		return;
	} else if (cpl == _reactors_stop) {             /* [한국어] 종료 트리거 (spdk_reactors_stop) — 마지막 작업이므로 set. */
		g_stopping_reactors = true;
	}
	pthread_mutex_unlock(&g_stopping_reactors_mtx);

	cr = calloc(1, sizeof(*cr));            /* [한국어] chain 컨텍스트 동적 할당 — chain 끝까지 살아 있어야 함. */
	if (!cr) {
		SPDK_ERRLOG("Unable to perform reactor iteration\n");
		cpl(arg1, arg2);                /* [한국어] 할당 실패 시 즉시 cpl 호출 — 호출자가 진행을 멈추도록. */
		return;
	}

	cr->fn = fn;
	cr->arg1 = arg1;
	cr->arg2 = arg2;
	cr->cpl = cpl;
	cr->orig_core = spdk_env_get_current_core();    /* [한국어] cpl 을 실행할 호출자 코어. */
	cr->cur_core = spdk_env_get_first_core();       /* [한국어] chain 시작 코어. */

	SPDK_DEBUGLOG(reactor, "Starting reactor iteration from %d\n", cr->orig_core);

	_event_call(cr->cur_core, on_reactor, cr, NULL);        /* [한국어] 첫 코어로 chain 시작. */
}

#ifdef __linux__
/*
 * [한국어]
 * reactor_schedule_thread_event - resched_fd 깨어남 시 호출되는 fd_group 콜백
 *
 * @arg: spdk_reactor *.
 * @return: 처리한 thread 수.
 *
 * interrupt 모드 reactor 의 resched_fd 가 write 되면 epoll 가 깨어나 이 콜백
 * 호출. 모든 lw_thread 를 순회하며 post_process (재배치/destroy) 수행.
 *
 * 호출 체인:
 *   spdk_fd_group_wait → epoll_wait → [reactor_schedule_thread_event]
 */
static int
reactor_schedule_thread_event(void *arg)
{
	struct spdk_reactor *reactor = arg;
	struct spdk_lw_thread *lw_thread, *tmp;
	uint32_t count = 0;

	assert(reactor->in_interrupt);          /* [한국어] interrupt 모드에서만 fd_group_wait 가 호출됨. */

	TAILQ_FOREACH_SAFE(lw_thread, &reactor->threads, link, tmp) {
		count += reactor_post_process_lw_thread(reactor, lw_thread) ? 1 : 0;    /* [한국어] true 반환 (재배치/destroy 발생) 시 카운트. */
	}

	return count;
}

/*
 * [한국어]
 * reactor_interrupt_init - reactor 의 interrupt 인프라 초기화 (Linux 한정)
 *
 * @reactor: 초기화할 reactor.
 * @return: 0 성공, 음수 실패 (-EBADF, fd_group 에러).
 *
 * 동작 단계:
 *   1) spdk_fd_group_create — epoll 인스턴스 + 콜백 dispatch 인프라.
 *   2) resched_fd = eventfd (EFD_NONBLOCK | EFD_CLOEXEC).
 *   3) resched_fd 를 fd_group 에 추가 — 깨어나면 reactor_schedule_thread_event 콜백.
 *   4) events_fd = eventfd.
 *   5) events_fd 를 fd_group 에 추가 — 깨어나면 event_queue_run_batch 콜백.
 * EFD_NONBLOCK = read 시 blocking 안 함, EFD_CLOEXEC = exec 시 자동 close.
 *
 * 에러 경로: 단계별 cleanup (이미 만든 자원만 해제 후 음수 반환).
 *
 * 호출 체인:
 *   reactor_construct → [reactor_interrupt_init]
 */
static int
reactor_interrupt_init(struct spdk_reactor *reactor)
{
	struct spdk_event_handler_opts opts = {};       /* [한국어] fd_group 콜백 옵션 — fd_type 등. */
	int rc;

	rc = spdk_fd_group_create(&reactor->fgrp);      /* [한국어] epoll fd 생성. fgrp 가 NULL 이면 interrupt 모드 비활성. */
	if (rc != 0) {
		return rc;
	}

	reactor->resched_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);   /* [한국어] reschedule 트리거용 eventfd. 카운터 0 시작. */
	if (reactor->resched_fd < 0) {
		rc = -EBADF;
		goto err;
	}

	spdk_fd_group_get_default_event_handler_opts(&opts, sizeof(opts));      /* [한국어] 기본 옵션 채움. */
	opts.fd_type = SPDK_FD_TYPE_EVENTFD;                                    /* [한국어] eventfd 라고 명시 — fd_group 의 read 처리 방식 결정. */

	rc = SPDK_FD_GROUP_ADD_EXT(reactor->fgrp, reactor->resched_fd,
				   reactor_schedule_thread_event, reactor, &opts);      /* [한국어] resched_fd 가 깨어나면 reactor_schedule_thread_event 호출. */
	if (rc) {
		close(reactor->resched_fd);
		goto err;
	}

	reactor->events_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);    /* [한국어] cross-reactor 이벤트 알림용 eventfd. */
	if (reactor->events_fd < 0) {
		spdk_fd_group_remove(reactor->fgrp, reactor->resched_fd);
		close(reactor->resched_fd);

		rc = -EBADF;
		goto err;
	}

	rc = SPDK_FD_GROUP_ADD_EXT(reactor->fgrp, reactor->events_fd,
				   event_queue_run_batch, reactor, &opts);      /* [한국어] events_fd 깨어남 시 event_queue_run_batch — 펜딩 이벤트 처리. */
	if (rc) {
		spdk_fd_group_remove(reactor->fgrp, reactor->resched_fd);
		close(reactor->resched_fd);
		close(reactor->events_fd);
		goto err;
	}

	return 0;

err:
	spdk_fd_group_destroy(reactor->fgrp);   /* [한국어] cleanup — fd_group 자체 해제. */
	reactor->fgrp = NULL;
	return rc;
}
#else
/*
 * [한국어]
 * reactor_interrupt_init (Linux 외 플랫폼) - 항상 -ENOTSUP
 *
 * eventfd 가 없는 플랫폼 (FreeBSD 등) 에서는 interrupt mode 미지원.
 */
static int
reactor_interrupt_init(struct spdk_reactor *reactor)
{
	return -ENOTSUP;
}
#endif

/*
 * [한국어]
 * reactor_interrupt_fini - interrupt 인프라 해제 (idempotent)
 *
 * @reactor: 정리할 reactor.
 *
 * fgrp 가 NULL 이면 init 안 된 상태 — no-op. 그 외 events_fd/resched_fd 를
 * fd_group 에서 제거 후 close, fd_group 자체 destroy.
 *
 * 호출 체인:
 *   spdk_reactors_fini → [reactor_interrupt_fini]
 */
static void
reactor_interrupt_fini(struct spdk_reactor *reactor)
{
	struct spdk_fd_group *fgrp = reactor->fgrp;

	if (!fgrp) {
		return;
	}

	spdk_fd_group_remove(fgrp, reactor->events_fd);         /* [한국어] events_fd 콜백 제거. */
	spdk_fd_group_remove(fgrp, reactor->resched_fd);        /* [한국어] resched_fd 콜백 제거. */

	close(reactor->events_fd);                              /* [한국어] eventfd close. */
	close(reactor->resched_fd);

	spdk_fd_group_destroy(fgrp);                            /* [한국어] epoll fd 해제. */
	reactor->fgrp = NULL;
}

/*
 * [한국어]
 * _governor_find - g_governor_list 에서 이름으로 governor 검색
 *
 * @name: governor 이름.
 * @return: 일치 governor 또는 NULL.
 */
static struct spdk_governor *
_governor_find(const char *name)
{
	struct spdk_governor *governor, *tmp;   /* [한국어] _SAFE 버전 사용 — 콜백이 리스트 수정해도 안전. */

	TAILQ_FOREACH_SAFE(governor, &g_governor_list, link, tmp) {
		if (strcmp(name, governor->name) == 0) {
			return governor;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * spdk_governor_set - 활성 governor 교체 (CPU 주파수 조절 모듈)
 *
 * @name: 활성화할 governor 이름. NULL 이면 명시적 비활성.
 * @return: 0 성공, -EINVAL 미등록, init 실패 시 그 rc.
 *
 * scheduler 와 비슷한 패턴이지만, init 성공 후에 기존 deinit 한다는 점이 다름
 * (scheduler 는 deinit 먼저, init 후 fail 시 복구). 코어 주파수 조절은 시스템
 * 전역 영향을 미치므로 새 것이 확실히 init 된 후에만 기존 것 deinit.
 *
 * 호출 체인:
 *   RPC framework_set_governor → [spdk_governor_set]
 */
int
spdk_governor_set(const char *name)
{
	struct spdk_governor *governor;
	int rc = 0;

	/* NULL governor was specifically requested */
	if (name == NULL) {
		if (g_governor) {
			g_governor->deinit();
		}
		g_governor = NULL;
		return 0;
	}

	governor = _governor_find(name);
	if (governor == NULL) {
		return -EINVAL;
	}

	if (g_governor == governor) {           /* [한국어] 이미 같음 — no-op. */
		return 0;
	}

	rc = governor->init();                  /* [한국어] 새 governor init 시도. */
	if (rc == 0) {                          /* [한국어] 성공 시에만 기존 deinit + 교체. */
		if (g_governor) {
			g_governor->deinit();
		}
		g_governor = governor;
	}

	return rc;
}

/*
 * [한국어]
 * spdk_governor_get - 현재 활성 governor 반환
 */
struct spdk_governor *
spdk_governor_get(void)
{
	return g_governor;
}

/*
 * [한국어]
 * spdk_governor_register - governor 플러그인 등록 (constructor 시점)
 *
 * 동일 이름 중복 등록은 assert(false).
 */
void
spdk_governor_register(struct spdk_governor *governor)
{
	if (_governor_find(governor->name)) {
		SPDK_ERRLOG("governor named '%s' already registered.\n", governor->name);
		assert(false);
		return;
	}

	TAILQ_INSERT_TAIL(&g_governor_list, governor, link);
}

SPDK_LOG_REGISTER_COMPONENT(reactor)    /* [한국어] "reactor" 로그 컴포넌트 등록 — SPDK_DEBUGLOG(reactor, ...) 가 이 이름의 활성화 여부에 따라 출력. */

/*
 * [한국어]
 * scheduler_trace - SPDK trace 시스템에 스케줄러 tpoint 등록
 *
 * spdk_trace_register_description_ext 로 4개 tpoint 의 메타데이터 등록:
 *  - SCHEDULER_PERIOD_START: 라운드 시작 (인자 없음).
 *  - SCHEDULER_CORE_STATS: 코어별 busy/idle TSC.
 *  - SCHEDULER_THREAD_STATS: thread 별 busy/idle TSC.
 *  - SCHEDULER_MOVE_THREAD: thread 이동 (src/dst lcore).
 * OWNER_TYPE_REACTOR 도 'r' 식별자로 등록 — trace 출력 시 owner 타입 식별.
 *
 * 호출 체인:
 *   SPDK_TRACE_REGISTER_FN constructor → [scheduler_trace]
 */
static void
scheduler_trace(void)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"SCHEDULER_PERIOD_START", TRACE_SCHEDULER_PERIOD_START,
			OWNER_TYPE_NONE, OBJECT_NONE, 0,                /* [한국어] owner/object 없음 — 글로벌 이벤트. */
			{

			}
		},
		{
			"SCHEDULER_CORE_STATS", TRACE_SCHEDULER_CORE_STATS,
			OWNER_TYPE_REACTOR, OBJECT_NONE, 0,             /* [한국어] reactor 단위 owner. */
			{
				{ "busy", SPDK_TRACE_ARG_TYPE_INT, 8},  /* [한국어] busy_tsc, 8B. */
				{ "idle", SPDK_TRACE_ARG_TYPE_INT, 8}   /* [한국어] idle_tsc, 8B. */
			}
		},
		{
			"SCHEDULER_THREAD_STATS", TRACE_SCHEDULER_THREAD_STATS,
			OWNER_TYPE_THREAD, OBJECT_NONE, 0,              /* [한국어] thread 단위 owner. */
			{
				{ "busy", SPDK_TRACE_ARG_TYPE_INT, 8},
				{ "idle", SPDK_TRACE_ARG_TYPE_INT, 8}
			}
		},
		{
			"SCHEDULER_MOVE_THREAD", TRACE_SCHEDULER_MOVE_THREAD,
			OWNER_TYPE_THREAD, OBJECT_NONE, 0,
			{
				{ "src", SPDK_TRACE_ARG_TYPE_INT, 8 },  /* [한국어] 출발 lcore. */
				{ "dst", SPDK_TRACE_ARG_TYPE_INT, 8 }   /* [한국어] 도착 lcore. */
			}
		}
	};

	spdk_trace_register_owner_type(OWNER_TYPE_REACTOR, 'r');                /* [한국어] reactor 타입 식별자 'r' — trace dump 시 표시. */
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));          /* [한국어] tpoint 메타데이터 등록 — SPDK_COUNTOF = 배열 크기 매크로. */

}

SPDK_TRACE_REGISTER_FN(scheduler_trace, "scheduler", TRACE_GROUP_SCHEDULER)      /* [한국어] constructor 매크로 — 부팅 시 scheduler_trace 자동 호출. trace group "scheduler" 로 묶임. */
