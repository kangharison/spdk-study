/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.  All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/**
 * \file
 * Event framework public API.
 *
 * See @ref event_components for an overview of the SPDK event framework API.
 */

/*
 * [한국어 설명] SPDK 이벤트 프레임워크 공개 API 헤더 (event.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 애플리케이션의 라이프사이클(부팅·실행·종료)을 정의하는 최상위 공개 API다.
 * 핵심은 (1) `spdk_app_opts` — 애플리케이션 부팅 옵션 컨테이너, (2) `spdk_app_start()` —
 * DPDK EAL 기반 환경 초기화 + 모든 reactor(코어당 1개) 기동 + 사용자 진입점 호출까지를
 * 한 번에 처리하는 메인 부트스트랩, (3) `spdk_event_*` API — 다른 lcore에서 함수를 비동기로
 * 실행시키는 cross-core 메시지 패싱 프리미티브이다. 또한 `--cpumask`, `-r`(RPC), `-t`(tracepoint)
 * 같은 SPDK 표준 CLI 옵션을 파싱하는 `spdk_app_parse_args()` 헬퍼와 트레이스 셋업 진입점도
 * 함께 제공한다. SPDK의 모든 메인 애플리케이션(nvmf_tgt, vhost, iscsi_tgt, spdk_tgt 등)은
 * 이 헤더만 인클루드하면 reactor·subsystem·RPC 서버까지 한 번에 부팅할 수 있다.
 *
 * 또한 이 헤더는 thread.h 의 `spdk_thread`/`spdk_poller`/`spdk_msg_fn` 추상화를 외부로 전달하는
 * 얇은 어댑터 역할도 겸한다 — 사용자 코드는 직접 thread.h 를 인클루드하지 않더라도
 * event.h 만으로 부팅·콜백·종료까지 모든 일생을 표현할 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 부팅 흐름의 가장 바깥 껍질이다. 호출 체인은 다음과 같다:
 *   main() (app/nvmf_tgt 등)
 *     → spdk_app_opts_init() (이 헤더)
 *     → spdk_app_parse_args() (이 헤더, getopt 래퍼)
 *     → spdk_app_start(opts, start_fn, ctx) (이 헤더)
 *         → spdk_env_init() (DPDK EAL 초기화: hugepage, lcore, mempool)
 *         → spdk_thread_lib_init() (lib/thread)
 *         → 각 lcore 에 reactor 생성 → polling loop 진입
 *         → main lcore 의 spdk_thread 에서 start_fn(ctx) 호출
 *             → 일반적으로 start_fn 내부에서 spdk_subsystem_init() 호출 (init.h)
 *     → (사용자 코드 실행, polling)
 *     → spdk_app_stop(rc) → reactor exit → spdk_app_start() 반환
 *     → spdk_app_fini() (정리)
 * 실행 컨텍스트: 호스트 유저스페이스, DPDK EAL 이 만든 lcore pthread 들 위에서 동작.
 * 1 코어 = 1 reactor = 1 (또는 N 개의) spdk_thread 모델이며, 각 reactor 는 무한 폴링 루프에서
 * (a) 이 헤더의 spdk_event 를 dequeue 하여 실행, (b) 자기 위에 호스팅된 spdk_thread 의
 * spdk_thread_poll() 을 호출 (= 메시지 처리 + poller 호출) 의 두 작업을 번갈아 수행한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/cpuset.h(CPU 마스크), spdk/init.h(subsystem 초기화 인터페이스), spdk/thread.h
 * (`spdk_msg_fn` 타입과 spdk_thread API), spdk/log.h(`spdk_log_level`, `spdk_log_cb`),
 * spdk/queue.h(BSD TAILQ 매크로), spdk/assert.h(`SPDK_STATIC_ASSERT`).
 * 의존받는 쪽: lib/event/(reactor.c, app.c — 이 헤더의 구현체), 모든 SPDK 메인 앱(app/*),
 * 일부 예제(examples/) 그리고 RPC 핸들러(`framework_*` 그룹).
 * 데이터 흐름: 사용자가 채운 `spdk_app_opts` → `spdk_app_start()`가 그 값을 DPDK EAL CLI
 * (`-c`, `-n`, `-m`, `--huge-dir` 등) 인자로 변환 → DPDK 가 hugepage/lcore 를 셋업 →
 * SPDK reactor 가 깨어나면서 사용자 `start_fn` 콜백 실행. 종료 시 `spdk_app_stop(rc)`이
 * 모든 reactor 에 SHUTDOWN 메시지를 보내고 메인 reactor 는 `rc`를 반환.
 *
 * 핵심 인사이트 — spdk_event_call vs spdk_thread_send_msg 의 차이:
 *   - spdk_event_call (이 헤더): "lcore" 단위 디스패치. 대상은 reactor 이며, 그 reactor 에
 *     호스팅된 어떤 spdk_thread 위에서 실행되는지는 명시되지 않는다(reactor 가 polling 루프에서
 *     event 를 직접 처리). 저수준이며 lcore-affinity 가 명확한 인프라 코드(예: reactor scheduler,
 *     subsystem fini 의 fan-out)에서 주로 사용.
 *   - spdk_thread_send_msg (thread.h): "spdk_thread" 단위 디스패치. 대상 thread 가 어느
 *     reactor 에 있든 lib/thread 가 lockless ring 으로 메시지를 전달하고, 대상 thread 의
 *     spdk_thread_poll() 이 다음 라운드에서 fn(ctx) 를 호출한다. bdev_io 콜백, channel 관리 등
 *     상위 모듈은 거의 전적으로 이 API 를 사용한다.
 *   둘 다 lockless ring 기반이고 비동기·1회성이라는 공통점이 있지만, 디스패치 단위(thread vs
 *   reactor)와 인자 개수(1 vs 2)와 typical caller(infra vs library) 가 다르다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_app_opts: 애플리케이션 부팅 옵션 패킹 구조체. ABI 호환을 위해
 *   `opts_size`(caller 가 본 sizeof) 필드와 `reservedNN[N]` 홀들을 포함하며 packed 정렬.
 * - spdk_app_opts_init(): 모든 필드를 SPDK 기본값으로 채움 (반드시 가장 먼저 호출).
 * - spdk_app_parse_args(): argc/argv 를 받아 표준 SPDK 옵션을 opts 에 반영하고 앱 고유 옵션은
 *   사용자 콜백으로 위임. SPDK_APP_GETOPT_STRING 이 표준 short option 집합을 정의.
 * - spdk_app_start(): EAL 초기화 + reactor 기동 + start_fn 호출. 블로킹 함수.
 * - spdk_app_stop()/spdk_app_start_shutdown(): 비동기 종료 트리거.
 * - spdk_event_allocate()/spdk_event_call(): 임의 lcore 에서 함수를 실행시키는
 *   cross-core 메시지 프리미티브 (lockless ring 기반). spdk_thread API 의 저수준 형태.
 * - spdk_app_setup_trace(): tpoint 그룹 마스크에 따라 trace 공유 메모리 셋업.
 * - spdk_framework_enable_context_switch_monitor(): polled-mode reactor 의 비정상 cs 감지 토글.
 */

#ifndef SPDK_EVENT_H            /* [한국어] include guard 시작 — 동일 TU 에서 event.h 가 두 번 이상 인클루드되어도 선언이 중복되지 않게 한다 (C 헤더 표준 패턴). */
#define SPDK_EVENT_H            /* [한국어] include guard 매크로 정의 — 위 #ifndef 의 짝. 두 번째 인클루드부터는 #ifndef 가 false 가 되어 본문이 모두 스킵됨. */

#include "spdk/stdinc.h"        /* [한국어] SPDK 표준 헤더 모음(stddef, stdint, stdbool, stdio, sys/types 등)을 한 번에 끌어옴 — 플랫폼 의존을 한 곳에 격리. 이 파일에서 size_t/uint8_t/uint32_t/uint64_t/bool/FILE 등이 모두 여기서 옴. */

#include "spdk/cpuset.h"        /* [한국어] `struct spdk_cpuset` 정의 — `spdk_app_get_core_mask()`, `spdk_app_parse_core_mask()` 가 반환·기록하는 비트마스크 표현. SPDK_CPUSET_SIZE 코어까지 표현 가능. */
#include "spdk/init.h"          /* [한국어] subsystem init/fini 인터페이스 — `spdk_app_start` 가 내부적으로(또는 사용자 start_fn 에서) 호출하는 후속 단계. delay_subsystem_init=true 모드의 RPC 매트릭스를 정의. */
#include "spdk/queue.h"         /* [한국어] BSD TAILQ/SLIST 매크로 — 본 헤더 자체는 직접 사용하지 않으나 init.h/thread.h 등 후속 인클루드와의 매크로 충돌·중복 정의를 방지하기 위해 일관된 순서로 포함. */
#include "spdk/log.h"           /* [한국어] `enum spdk_log_level`, `spdk_log_cb` 타입 — `spdk_app_opts.print_level`, `.rpc_log_level`, `.log` 필드에서 사용. */
#include "spdk/thread.h"        /* [한국어] `spdk_msg_fn`(=void(*)(void*ctx)), `struct spdk_thread`, `struct spdk_poller` — `spdk_app_start` 의 `start_fn` 시그니처와 본 헤더의 poller 전방 선언에 필요. event.h 와 thread.h 는 사실상 한 묶음의 실행 모델을 형성한다. */
#include "spdk/assert.h"        /* [한국어] `SPDK_STATIC_ASSERT` — 아래에서 `spdk_app_opts` 의 sizeof 를 컴파일 타임에 253 으로 고정해 패딩 변동/필드 순서 변경에 의한 ABI 깨짐을 빌드 단계에서 차단. */

#ifdef __cplusplus              /* [한국어] C++ 컴파일러로 이 헤더를 인클루드할 때만 진입 — C 함수 심볼이 이름 맹글링되는 것을 막기 위함. */
extern "C" {                    /* [한국어] 이하 선언들을 C linkage 로 강제 — SPDK 라이브러리는 C 로 빌드되지만 사용자 앱은 C++ 일 수 있어 양쪽 호환을 위해 필수. */
#endif

/**
 * Event handler function.
 *
 * \param arg1 Argument 1.
 * \param arg2 Argument 2.
 */
/*
 * [한국어]
 * spdk_event_fn - cross-core 이벤트 핸들러의 함수 포인터 타입.
 *
 * @arg1: 사용자가 spdk_event_allocate 에 전달한 첫 번째 인자 (불투명 포인터).
 *        보통 작업 대상 객체(예: bdev_io, ctrlr) 의 포인터로 사용.
 * @arg2: 두 번째 인자 (보통 콜백 컨텍스트 또는 부가 파라미터).
 *        spdk_thread_send_msg 가 인자 1 개만 받는 것과 달리 event 는 2 개를 받는다 — 가벼운
 *        디스패치를 위해 별도 컨텍스트 구조체 할당 없이 한 번에 두 포인터를 전달할 수 있다.
 * @return: void — 결과는 arg1/arg2 가 가리키는 컨텍스트의 필드에 기록되거나
 *          또 다른 spdk_event_call 로 호출자 lcore 로 회신됨.
 *
 * 이 콜백은 `spdk_event_call()` 이 큐잉하면 *대상 lcore* 의 reactor 폴링 루프에서 실행된다.
 * 즉 함수 본문이 실행되는 스레드 컨텍스트는 호출자의 스레드가 아니라 `spdk_event_allocate(lcore=…)`
 * 로 지정된 lcore 의 reactor 임에 주의. 본문 안에서는 그 lcore 에 묶인 자원만 안전하게 만질 수 있다.
 * 실행 시점은 비동기 — 호출자 lcore 와 수신자 lcore 는 락 없이 lockless ring 한 번 push/pop 으로
 * 동기화되며, 메시지 순서는 같은 (송신, 수신) lcore 쌍 내에서 FIFO 가 보장된다.
 */
typedef void (*spdk_event_fn)(void *arg1, void *arg2);  /* [한국어] cross-core 함수 호출 시 사용되는 callback signature — spdk_event 객체 안에 fn/arg1/arg2 가 함께 패킹되어 ring 으로 전달된다. */

/**
 * \brief An event is a function that is passed to and called on an lcore.
 */
/*
 * [한국어]
 * struct spdk_event - 다른 lcore 에서 실행할 함수 + 인자 두 개를 묶어 캡슐화하는 불투명 구조체.
 *
 * 정의는 lib/event/event.c 내부에 숨겨져 있으며 사용자는 포인터만 다룬다.
 * 메모리는 `spdk_event_allocate()` 가 lockless mempool(rte_mempool 기반) 에서 할당하고
 * `spdk_event_call()` 이 대상 lcore 의 ring 에 push 하면서 소유권을 이전한다.
 * 실행 후에는 reactor 가 자동으로 mempool 에 반환하므로 사용자는 free 할 필요 없다.
 *
 * 멀티스레드 접근: spdk_event_allocate 는 어떤 스레드에서 호출되어도 안전(MPSC ring),
 * spdk_event_call 도 동일. 한 spdk_event 객체는 단 한 번만 call 되어야 한다 (재사용 금지).
 */
struct spdk_event;              /* [한국어] 이벤트 객체 전방 선언 — 실제 정의는 lib/event/. 사용자 코드는 항상 포인터로만 접근하며 필드 직접 액세스는 금지. */

/**
 * \brief A poller is a function that is repeatedly called on an lcore.
 */
/*
 * [한국어]
 * struct spdk_poller - reactor 폴링 루프에서 매 반복마다(또는 일정 주기로) 호출되는 콜백 핸들.
 *
 * 정의는 lib/thread/thread.c 에 위치 (자세한 인터페이스는 spdk/thread.h 참조).
 * 본 헤더에서는 전방 선언만 두어 다른 헤더와의 의존을 줄이고 옵션 구조체 등에서 포인터로
 * 다룰 수 있게 한다. Polled-mode 설계의 핵심 — 인터럽트 없이 NVMe CQ, 소켓, RPC 등을
 * 주기적으로 검사하는 콜백 컨테이너이며, 한 spdk_thread 에 종속(thread affinity) 된다.
 */
struct spdk_poller;             /* [한국어] poller 전방 선언 — 실제 정의는 spdk/thread.h 의 인터페이스를 통해 lib/thread 에 숨겨짐. */

/**
 * Callback function for customized shutdown handling of application.
 */
/*
 * [한국어]
 * spdk_app_shutdown_cb - 시그널(SIGINT/SIGTERM)이나 spdk_app_start_shutdown() 호출 시
 * 기본 종료 절차 대신 실행할 사용자 정의 종료 콜백 타입.
 *
 * @return: void.
 *
 * `spdk_app_opts.shutdown_cb` 로 등록하면 시그널 핸들러가 이를 호출하고, 사용자는 그 안에서
 * 자원 정리 후 직접 `spdk_app_stop(rc)` 을 불러야 reactor 가 멈춘다. NULL 이면 SPDK 가
 * 기본 절차(subsystem_fini → spdk_app_stop(0)) 를 수행한다.
 *
 * 실행 컨텍스트: 시그널 핸들러가 직접 호출하는 것이 아니라, 시그널 핸들러는 self-pipe/eventfd
 * 로 메인 reactor 를 깨우고 그 reactor 가 메시지로 이 콜백을 동기 호출한다 → 콜백 본문은
 * 일반 spdk_thread 컨텍스트에서 실행되므로 SPDK API 를 자유롭게 사용해도 안전하다.
 */
typedef void (*spdk_app_shutdown_cb)(void);  /* [한국어] 사용자 정의 종료 콜백 타입 — opts.shutdown_cb 에 저장되어 종료 트리거 시 한 번 호출됨. */

/**
 * Signal handler function.
 *
 * \param signal Signal number.
 */
/*
 * [한국어]
 * spdk_sighandler_t - POSIX `sa_handler` 와 동일한 형식의 시그널 핸들러 타입.
 *
 * @signal: 전달된 시그널 번호 (SIGINT, SIGTERM, SIGUSR1 등).
 * @return: void.
 *
 * SPDK 내부에서 시그널 → 종료 경로를 사용자 정의로 바꾸고 싶을 때 쓰이는 보조 타입.
 * (현재 헤더 내 다른 함수 시그니처에는 직접 노출되지 않지만 ABI 차원에서 정의해 둠 —
 * lib/event 내부의 sigaction 등록 시 캐스팅 대상으로 사용.)
 */
typedef void (*spdk_sighandler_t)(int signal);  /* [한국어] POSIX 시그널 핸들러 함수 포인터 타입 — sigaction(2) 의 sa_handler 와 호환되도록 시그니처 일치. */

/**
 * \brief Event framework initialization options
 */
/*
 * [한국어]
 * struct spdk_app_opts - SPDK 애플리케이션 부팅 옵션 컨테이너.
 *
 * 이 구조체는 사용자 → spdk_app_start() 사이에서 모든 부팅 파라미터를 전달하는 컨테이너이며,
 * DPDK EAL 인자(예: `-c`, `-n`, `-m`, `--proc-type`, `--huge-dir`)에 1:1 매핑되는 필드를
 * 다수 포함한다. ABI 호환을 위해 `__attribute__((packed))` + 명시적 reserved 홀 + opts_size
 * 필드를 갖는 점이 특징이다. 새 필드는 보통 끝(opts_size 다음) 에 추가되며, 라이브러리는
 * caller 가 본 opts_size 까지만 읽고 그 이후는 기본값으로 채운다.
 *
 * ABI 안전 4중 장치:
 *   1) `__attribute__((packed))` — 컴파일러가 자동 패딩을 넣어 sizeof 가 변하는 것을 막음.
 *   2) 명시적 `reservedNN[N]` 홀 — 향후 필드 추가용 예비 영역, 0 으로 초기화 보장.
 *   3) 마지막의 `opts_size` 필드 — caller 가 본 sizeof 를 라이브러리에 전달.
 *   4) 파일 하단의 `SPDK_STATIC_ASSERT(sizeof(...) == 253)` — 빌드 시점 sizeof 고정.
 *
 * 사용 흐름:
 *   spdk_app_opts opts; spdk_app_opts_init(&opts, sizeof(opts));
 *   opts.name = "my_app"; opts.reactor_mask = "0xF"; ...;
 *   spdk_app_start(&opts, my_start_fn, ctx);
 *
 * 동기화: 한 부팅 호출에서 단일 스레드(보통 main()) 만이 이 구조체를 채우고 spdk_app_start 에
 * 넘긴다. spdk_app_start 반환 후에는 라이브러리가 내부 사본을 보유하므로 원본을 자유롭게
 * 해제·재사용 가능. 별도 락 없음.
 */
struct spdk_app_opts {
	const char *name;
	/* [한국어] 애플리케이션 이름 (예: "nvmf_tgt"). 로그/trace 파일명/proc title 에 쓰임.
	 * 설정자: 사용자 코드가 spdk_app_opts_init 후에 직접 대입.
	 * 읽는 자: spdk_app_start 내부에서 DPDK EAL 과 trace 서브시스템에 전달, RPC 응답에서도 노출.
	 * 값 범위: NULL 가능 (NULL 이면 SPDK 가 기본 이름 "spdk" 를 부여). 권장은 앱별 식별 가능한 이름.
	 * 동기화: 부팅 시 1 회만 읽힘. 이후 read-only 전역으로 취급. */

	const char *json_config_file;
	/* [한국어] 부팅 시 자동 로드할 JSON 설정 파일 경로 (선택). NULL 이면 RPC 로만 구성.
	 * 설정자: 사용자 또는 `--json` CLI 옵션을 통해 spdk_app_parse_args 가 채움.
	 * 읽는 자: subsystem 초기화 단계에서 RPC 를 재생(replay) 하기 위해 읽힘 (lib/init).
	 * 값 범위: 유효한 파일 경로 또는 NULL. json_data 와 동시 사용 불가 (둘 중 하나만 지정).
	 * 동기화: 부팅 시 1 회 읽힘. */

	bool json_config_ignore_errors;
	/* [한국어] JSON 설정 RPC 중 일부가 실패해도 부팅을 계속할지 여부.
	 * 설정자: 사용자 또는 `--json-ignore-init-errors` CLI 옵션.
	 * 읽는 자: spdk_subsystem_load_config 의 stop_on_error 인자로 전달.
	 * 값 범위: false=하나라도 실패하면 종료(기본), true=실패 무시하고 계속.
	 * 동기화: 부팅 시 1 회 읽힘. */

	/* Hole at bytes 17-23. */
	uint8_t	reserved17[7];
	/* [한국어] ABI alignment 용 패딩 — 새 필드 추가 시 이 영역을 사용할 수 있다.
	 * 설정자: spdk_app_opts_init 이 0 으로 초기화 (memset).
	 * 읽는 자: 라이브러리는 무시. 향후 필드 추가 시 일부 바이트를 새 필드로 전용.
	 * 값 범위: 항상 0 으로 유지되어야 함 (기본값=0).
	 * 동기화: read-only after init. */

	const char *rpc_addr; /* Can be UNIX domain socket path or IP address + TCP port */
	/* [한국어] JSON-RPC 서버가 listen 할 주소.
	 * 설정자: 사용자 또는 `-r` CLI 옵션. 기본 SPDK_DEFAULT_RPC_ADDR(/var/tmp/spdk.sock).
	 * 읽는 자: spdk_rpc_initialize() 에 전달되어 listen 소켓 생성.
	 * 값 범위: "/path/to/sock" (Unix domain) 또는 "1.2.3.4:5260" (TCP) 형식. NULL 이면 기본값.
	 * 동기화: 부팅 시 1 회 읽힘. */

	const char *reactor_mask;
	/* [한국어] reactor 를 띄울 CPU 코어의 16 진수 비트마스크 문자열 (DPDK `-c` 인자와 동일).
	 * 설정자: 사용자 또는 `-m` CLI 옵션. 예: "0xF" → core 0~3 에 reactor 4 개.
	 * 읽는 자: env_dpdk 가 EAL `-c` 옵션으로 변환하여 lcore 활성화에 사용.
	 * 값 범위: 0x 로 시작하는 hex 또는 [c1,c2,c3] 콤마 리스트. NULL 이면 모든 코어 사용.
	 * 동기화: 부팅 시 1 회만 평가. 이후 활성 코어 변경은 RPC `framework_set_scheduler` 로. */

	const char *tpoint_group_mask;
	/* [한국어] 활성화할 trace point 그룹의 비트마스크 문자열.
	 * 설정자: 사용자 또는 `--tpoint-group-mask` 옵션. 예: "0xFFFF" 또는 "bdev,nvmf".
	 * 읽는 자: spdk_app_setup_trace() 가 파싱하여 trace 공유 메모리 영역에 반영.
	 * 값 범위: hex 문자열 또는 그룹명 콤마 리스트. NULL 이면 트레이스 비활성.
	 * 동기화: 부팅 시 1 회 읽힘 (이후 RPC `trace_enable_tpoint_group` 로 동적 변경 가능). */

	int shm_id;
	/* [한국어] 다중 SPDK 프로세스가 같은 hugepage/lcore 자원을 공유할 때의 식별자.
	 * 설정자: 사용자 또는 `-i` CLI 옵션.
	 * 읽는 자: env_dpdk 가 EAL `--proc-type=secondary` + `--file-prefix` 에 매핑, trace 공유메모리 키에도 사용.
	 * 값 범위: -1=primary 단독(기본), >=0=multi-process group ID. 같은 ID 끼리만 자원 공유.
	 * 동기화: 부팅 시 1 회 읽힘. */

	/* Hole at bytes 52-55. */
	uint8_t			reserved52[4];
	/* [한국어] ABI 패딩 — 향후 4 바이트 정수 필드 추가용 예약 영역.
	 * 설정자: spdk_app_opts_init 이 0 초기화. 읽는 자: 없음. 동기화: read-only. */

	spdk_app_shutdown_cb	shutdown_cb;
	/* [한국어] SIGINT/SIGTERM 또는 spdk_app_start_shutdown() 호출 시 실행될 사용자 콜백.
	 * 설정자: 사용자 코드 (필요 시 직접 함수 포인터 대입).
	 * 읽는 자: 시그널 디스패처 / spdk_app_start_shutdown() 내부.
	 * 값 범위: 함수 포인터 또는 NULL(=SPDK 기본 종료 절차 사용).
	 * 동기화: 부팅 시 1 회 읽혀 lib/event 의 전역 변수로 캐시. */

	bool			enable_coredump;
	/* [한국어] DPDK 가 hugepage 영역을 코어덤프에 포함시킬지(MADV_DONTDUMP 해제) 여부.
	 * 설정자: 사용자. 기본 true (디버깅 편의).
	 * 읽는 자: env_dpdk init 에서 hugepage segment 마다 madvise() 호출 분기.
	 * 값 범위: true=hugepage 포함(기본), false=제외(코어덤프 크기 절약, 운영 환경 권장).
	 * 동기화: 부팅 시 1 회 읽힘. */

	/* Hole at bytes 65-67. */
	uint8_t			reserved65[3];
	/* [한국어] bool 다음 4 바이트 정수 정렬을 위한 패딩 — packed 구조체이므로 컴파일러가
	 * 자동 정렬하지 않아 직접 reserved 를 둠.
	 * 설정자: init 시 0. 읽는 자: 없음. 동기화: read-only. */

	int			mem_channel;
	/* [한국어] DPDK 메모리 채널 수 (`-n` 옵션). NUMA 인터리빙 결정에 영향.
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk 가 EAL 에 전달.
	 * 값 범위: -1=자동 감지(기본), 1..N=명시적 채널 수.
	 * 동기화: 부팅 시 1 회 읽힘. */

	int			main_core;
	/* [한국어] DPDK main lcore 번호 (`--main-lcore`).
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk 가 EAL 에 전달, SPDK 는 이 lcore 에서 start_fn 을 실행.
	 * 값 범위: -1=reactor_mask 의 첫 비트 자동 선택(기본), 0..N=특정 코어.
	 * 동기화: 부팅 시 1 회 읽힘 — main lcore 변경은 재시작 필요. */

	int			mem_size;
	/* [한국어] DPDK 가 사전 할당할 hugepage 메모리 크기 (MiB). `-s` 옵션과 매핑.
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk → EAL `--socket-mem`/`-m`.
	 * 값 범위: -1=동적(기본, hugepage 가용량 전부), >=0=고정 MiB.
	 * 동기화: 부팅 시 1 회 읽힘. */

	bool			no_pci;
	/* [한국어] DPDK PCI 디바이스 probe 를 비활성화할지 (`--no-pci`).
	 * 설정자: 사용자 또는 `-u` CLI 옵션.
	 * 읽는 자: env_dpdk 가 EAL --no-pci 로 변환.
	 * 값 범위: false=PCI probe(기본), true=NVMe 등 PCI 디바이스 probe 안 함(memory bdev 만 쓸 때 유용).
	 * 동기화: 부팅 시 1 회 읽힘. */

	bool			hugepage_single_segments;
	/* [한국어] 모든 hugepage 를 IOVA-연속 단일 세그먼트로 강제할지 (`--single-file-segments`).
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk 가 EAL --single-file-segments 로 변환.
	 * 값 범위: false=다중 세그먼트(기본), true=DPDK 18.05+ 의 메모리 hotplug 호환 모드.
	 * 동기화: 부팅 시 1 회 읽힘. */

	bool			unlink_hugepage;
	/* [한국어] EAL 이 종료 시 hugepage 백업 파일을 unlink 할지 (`--huge-unlink`).
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk 가 EAL --huge-unlink 옵션으로 변환.
	 * 값 범위: false=파일 유지(기본, 다음 부팅 시 재사용), true=프로세스 종료 후 hugepage 파일 정리.
	 * 동기화: 부팅 시 1 회 읽힘. */

	bool			no_huge;
	/* [한국어] hugepage 없이 일반 anonymous 메모리로 EAL 을 띄울지 (`--no-huge`).
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk 가 EAL --no-huge 옵션으로 변환.
	 * 값 범위: false=hugepage 사용(기본), true=hugepage 미사용 (테스트/CI 용도, NVMe 성능 회귀 발생).
	 * 동기화: 부팅 시 1 회 읽힘. */

	/* Hole at bytes 84-87. */
	uint8_t			reserved84[4];
	/* [한국어] 4 바이트 정렬 패딩 — 다음 포인터 필드(hugedir) 의 8 바이트 정렬을 맞추기 위한 buffer.
	 * 설정자: init 시 0. 읽는 자: 없음. 동기화: read-only. */

	const char		*hugedir;
	/* [한국어] hugepage 마운트 디렉토리 경로 (`--huge-dir`).
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk 가 EAL 에 전달.
	 * 값 범위: "/dev/hugepages" 등 유효한 hugetlbfs 마운트 경로. NULL 이면 EAL 이 기본 위치 탐색.
	 * 동기화: 부팅 시 1 회 읽힘. */

	enum spdk_log_level	print_level;
	/* [한국어] stderr 출력 로그 레벨 임계값 (이 레벨 이상의 메시지만 출력).
	 * 설정자: 사용자 또는 `-L` CLI 옵션.
	 * 읽는 자: spdk_log 모듈이 부팅 시 spdk_log_set_print_level() 로 적용.
	 * 값 범위: SPDK_LOG_DISABLED..SPDK_LOG_DEBUG. 기본 SPDK_LOG_NOTICE.
	 * 동기화: 부팅 시 적용 후 RPC `log_set_print_level` 로 런타임 변경 가능 — 변경은 atomic write. */

	/* Hole at bytes 100-103. */
	uint8_t			reserved100[4];
	/* [한국어] enum 후 8 바이트 정렬 패딩 — 다음 size_t/포인터 정렬용.
	 * 설정자: init 시 0. 읽는 자: 없음. 동기화: read-only. */

	size_t			num_pci_addr;
	/* [한국어] pci_blocked/pci_allowed 배열의 원소 수.
	 * 설정자: 사용자 — 두 배열 중 사용하는 쪽의 길이를 직접 지정.
	 * 읽는 자: env_dpdk 가 배열 순회 시 이 값으로 길이를 결정.
	 * 값 범위: 0..N. 두 배열은 동일 길이를 공유한다고 가정.
	 * 동기화: 부팅 시 1 회 읽힘. */

	struct spdk_pci_addr	*pci_blocked;
	/* [한국어] probe 에서 제외할 PCI 주소 배열 (`-B` 옵션).
	 * 설정자: 사용자 — 외부에서 할당한 배열 포인터 대입.
	 * 읽는 자: env_dpdk 가 num_pci_addr 만큼 순회하며 EAL `--block` 으로 변환.
	 * 값 범위: 유효한 spdk_pci_addr 배열 포인터 또는 NULL. NULL 이면 차단 목록 없음.
	 * 동기화: 부팅 시 1 회 읽힘 — 배열 메모리는 spdk_app_start 반환까지 유효해야 함. */

	struct spdk_pci_addr	*pci_allowed;
	/* [한국어] probe 할 PCI 주소 화이트리스트 (`-A` 옵션). NULL 이면 전체 허용.
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk 가 num_pci_addr 만큼 순회하며 EAL `--allow` 로 변환.
	 * 값 범위: 유효한 spdk_pci_addr 배열 포인터 또는 NULL.
	 * 동기화: 부팅 시 1 회 읽힘. pci_blocked 와 동시 사용 시 EAL 이 에러. */

	const char		*iova_mode;
	/* [한국어] DPDK IOVA 모드 강제 ("pa"=물리주소, "va"=가상주소).
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk 가 EAL `--iova-mode` 로 변환.
	 * 값 범위: "pa" | "va" | NULL(=자동). VFIO/UIO 환경에 따라 자동 결정 — 일반적으로 vfio-pci=va, uio=pa.
	 * 동기화: 부팅 시 1 회 읽힘. */

	/* Wait for the associated RPC before initializing subsystems
	 * when this flag is enabled.
	 */
	bool			delay_subsystem_init;
	/* [한국어] true 이면 subsystem 초기화를 RPC `framework_start_init` 수신까지 지연.
	 * 설정자: 사용자 또는 `--wait-for-rpc` CLI 옵션.
	 * 읽는 자: lib/init 의 spdk_subsystem_init() 가 이 플래그를 보고 초기 RPC 매트릭스를 제한.
	 * 값 범위: false=즉시 init(기본), true=RPC 대기 모드.
	 * 의미: 부팅 직후 제한된 RPC 셋(framework_*, log_*) 만 활성화 → 사용자가 사전 설정 RPC 를 보낸 뒤
	 *       명시적으로 init 트리거. 자동화 스크립트에서 동적 구성에 유용.
	 * 동기화: 부팅 시 1 회 읽힘. */

	/* Hole at bytes 137-143. */
	uint8_t			reserved137[7];
	/* [한국어] 8 바이트 정렬 패딩 — 다음 uint64_t 필드(num_entries) 정렬용.
	 * 설정자: init 시 0. 읽는 자: 없음. 동기화: read-only. */

	/* Number of trace entries allocated for each core */
	uint64_t		num_entries;
	/* [한국어] 코어당 trace ring buffer 에 할당할 엔트리 수.
	 * 설정자: 사용자 또는 `--num-trace-entries` CLI 옵션.
	 * 읽는 자: spdk_app_setup_trace() 가 trace 공유메모리 매핑 크기 계산에 사용.
	 * 값 범위: 2 의 거듭제곱이 권장 (ring buffer 인덱싱). 0 이면 SPDK 기본값(SPDK_DEFAULT_NUM_TRACE_ENTRIES) 사용.
	 * 동기화: 부팅 시 1 회 읽힘. */

	/** Opaque context for use of the env implementation. */
	void			*env_context;
	/* [한국어] env 구현체(DPDK 등)가 자체 사용하는 불투명 컨텍스트.
	 * 설정자: 커스텀 env 백엔드를 사용하는 사용자 코드.
	 * 읽는 자: env 구현체 (lib/env_dpdk 또는 사용자 정의 env).
	 * 값 범위: NULL 또는 env 구현체가 정의한 임의 포인터.
	 * 동기화: env 구현체 책임. 일반 사용자는 NULL 로 두면 됨. */

	/**
	 * for passing user-provided log call
	 */
	spdk_log_cb		*log;
	/* [한국어] SPDK 내부 로그 호출을 사용자 함수로 가로채기 위한 콜백 포인터.
	 * 설정자: 사용자 코드 (예: syslog 대신 자체 로거로 보내고 싶을 때).
	 * 읽는 자: spdk_log 모듈이 부팅 시 spdk_log_open(log) 로 등록.
	 * 값 범위: 함수 포인터 또는 NULL. NULL 이면 기본 stderr/syslog 출력.
	 * 동기화: 부팅 시 1 회 등록 후 모든 로그 호출 경로에서 atomic 하게 사용. */

	uint64_t		base_virtaddr;
	/* [한국어] DPDK 가 hugepage 매핑을 시작할 기준 가상주소 (`--base-virtaddr`).
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk 가 EAL --base-virtaddr 옵션으로 변환.
	 * 값 범위: 0=EAL 자동(기본), 그 외=고정 가상주소(ASLR 충돌 회피·디버깅 시).
	 * 동기화: 부팅 시 1 회 읽힘. */

	/**
	 * The size of spdk_app_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 *
	 * New fields should usually be added at the end of this structure. The only exception is
	 * if using bytes from a reserved byte array after opts_size. In that case it is OK to use
	 * some of those bytes, as long as the default value is specified as 0.
	 */
	size_t opts_size;
	/* [한국어] 호출자가 컴파일 시점에 본 sizeof(struct spdk_app_opts) 값.
	 * 설정자: 사용자 (반드시 sizeof(opts) 로 설정 — spdk_app_opts_init 의 두 번째 인자가 그대로 저장됨).
	 * 읽는 자: 라이브러리 내부 — 이 크기까지만 신뢰하고 그 이후는 기본값으로 채움.
	 * 값 범위: 양의 정수, 보통 sizeof(struct spdk_app_opts). 0 은 잘못된 사용.
	 * 의미: 라이브러리가 새 필드를 추가해도 구버전 헤더로 빌드된 앱이 그대로 동작 — 4 중 ABI 안전장치의 핵심.
	 * 동기화: 부팅 시 1 회 읽힘. */

	/**
	 * Disable default signal handlers.
	 * If set to `true`, the shutdown process is not started implicitly by
	 * process signals, hence the application is responsible for calling
	 * spdk_app_start_shutdown().
	 *
	 * Default is `false`.
	 */
	bool disable_signal_handlers;
	/* [한국어] true 이면 SPDK 가 SIGINT/SIGTERM 핸들러를 등록하지 않음.
	 * 설정자: 사용자 코드.
	 * 읽는 자: spdk_app_start 의 시그널 셋업 분기.
	 * 값 범위: false=SPDK 가 sigaction(2) 등록(기본), true=호스트 앱이 자체 처리.
	 * 의미: SPDK 를 라이브러리로 임베드하는 호스트 앱이 자체 시그널 처리를 하고 싶을 때 사용.
	 *       이 경우 사용자가 직접 spdk_app_start_shutdown() 을 호출해야 종료가 시작된다.
	 * 동기화: 부팅 시 1 회 읽힘. */

	bool interrupt_mode;
	/* [한국어] 폴링 대신 인터럽트 모드(eventfd/epoll) 로 reactor 를 동작시킬지.
	 * 설정자: 사용자.
	 * 읽는 자: lib/thread 의 spdk_thread_lib_init_ext, lib/event 의 reactor 메인 루프.
	 * 값 범위: false=폴링(기본, CPU 100% 점유, 최저 지연), true=인터럽트(저전력, 더 높은 지연 ~us 단위).
	 * 의미: spdk_thread.h 의 fd_group epoll 통합과 연결된 옵션 — true 일 때 reactor 가
	 *       epoll_wait 로 sleep 가능.
	 * 동기화: 부팅 시 1 회 읽힘 — 모드 전환은 재시작 필요. */

	bool enforce_numa;
	/* [한국어] true 이면 NUMA 노드 미스매치(다른 노드의 메모리 사용) 시 에러로 처리.
	 * 설정자: 사용자 또는 `--enforce-numa` CLI 옵션.
	 * 읽는 자: lib/event, env_dpdk 가 NUMA 검증 시 분기.
	 * 값 범위: false=경고만 출력(기본), true=실패로 부팅 중단.
	 * 의미: 성능 회귀 방지용 안전장치 — DRAM 과 NIC/NVMe 가 다른 NUMA 노드에 있으면 큰 성능 손실.
	 * 동기화: 부팅 시 1 회 읽힘. */

	/* Hole at byte 187. */
	uint8_t reserved187[1];
	/* [한국어] 1 바이트 정렬 패딩 — 다음 uint32_t 필드(num_trace_threads) 의 4 바이트 정렬용.
	 * 설정자: init 시 0. 읽는 자: 없음. 동기화: read-only. */

	/* Number of threads for SPDK tracing */
	uint32_t num_trace_threads;
	/* [한국어] tracing 이 사용할 보조 스레드 수.
	 * 설정자: 사용자.
	 * 읽는 자: spdk_app_setup_trace() 가 trace flush 스레드 풀 생성에 사용.
	 * 값 범위: 0=trace 가 별도 스레드를 띄우지 않음(기본), >0=백그라운드 flush 스레드 개수.
	 * 동기화: 부팅 시 1 회 읽힘. */

	/**
	 * The allocated size for the message pool used by the threading library.
	 *
	 * Default is `SPDK_DEFAULT_MSG_MEMPOOL_SIZE`.
	 */
	size_t msg_mempool_size;
	/* [한국어] spdk_thread_send_msg 가 사용하는 메시지 mempool 크기 (메시지 객체 수).
	 * 설정자: 사용자.
	 * 읽는 자: spdk_thread_lib_init 에서 rte_mempool_create() 호출 시 사용.
	 * 값 범위: 0=SPDK_DEFAULT_MSG_MEMPOOL_SIZE 사용(기본), >0=명시적 메시지 수.
	 * 의미: 너무 작으면 cross-thread 메시지 폭주 시 spdk_thread_send_msg 가 -ENOMEM 으로 실패 →
	 *       호출자는 재시도 또는 fallback 경로 필요. 기본값은 일반 워크로드에 충분.
	 * 동기화: 부팅 시 1 회 읽힘. */

	/*
	 *  If non-NULL, a string array of allowed RPC methods.
	 */
	const char **rpc_allowlist;
	/* [한국어] 허용할 RPC 메서드 이름의 NULL-종단 문자열 배열.
	 * 설정자: 사용자 (보안 강화 시).
	 * 읽는 자: spdk_rpc 모듈이 매 RPC 호출마다 메서드명을 이 배열과 비교.
	 * 값 범위: NULL=모든 RPC 허용(기본), 비-NULL=화이트리스트 (배열 마지막 원소는 NULL).
	 * 동기화: 부팅 시 등록 후 RPC 핸들러에서 read-only 로 사용. */

	/**
	 * Used to pass vf_token to vfio_pci driver through DPDK.
	 * The vf_token is an UUID that shared between SR-IOV PF and VF.
	 */
	const char		*vf_token;
	/* [한국어] SR-IOV VF 사용 시 PF 와 공유하는 UUID 토큰.
	 * 설정자: 사용자 — VF 를 vfio-pci 로 바인딩한 환경에서 PF 와 동일한 UUID 지정.
	 * 읽는 자: env_dpdk 가 vfio-pci 드라이버에 전달 (EAL --vfio-vf-token).
	 * 값 범위: UUID 문자열 ("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx") 또는 NULL.
	 * 동기화: 부팅 시 1 회 읽힘. */

	/**
	 * Used to store lcore to CPU mappig to pass it to DPDK
	 */
	const char *lcore_map; /* lcore mapping */
	/* [한국어] DPDK lcore→물리 CPU 매핑 문자열 (`--lcores` 옵션).
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk 가 EAL --lcores 인자로 변환.
	 * 값 범위: "0@(0,1),1@2" 형식 — lcore 0 을 CPU 0,1 에 핀, lcore 1 을 CPU 2 에 핀. NULL 이면 자동 매핑.
	 * 동기화: 부팅 시 1 회 읽힘. */

	/**
	 * Log level for JSON RPC.
	 */
	enum spdk_log_level rpc_log_level;
	/* [한국어] JSON-RPC 호출/응답을 별도 로그 레벨로 기록.
	 * 설정자: 사용자.
	 * 읽는 자: spdk_rpc 의 요청/응답 로깅 경로.
	 * 값 범위: SPDK_LOG_DISABLED..SPDK_LOG_DEBUG. 기본 SPDK_LOG_DISABLED (RPC 로그 끔).
	 * 동기화: 부팅 시 1 회 읽힘 — 런타임 변경은 RPC `log_set_level` 로. */

	/**
	 * If non-NULL, a pointer to JSON RPC log file.
	 */
	FILE *rpc_log_file;
	/* [한국어] RPC 로그를 별도 파일로 보낼 때의 FILE*.
	 * 설정자: 사용자 코드 (fopen 등으로 미리 연 파일 핸들 전달).
	 * 읽는 자: spdk_rpc 의 로깅 경로 — rpc_log_level 에 도달한 로그를 fprintf 로 이 파일에 기록.
	 * 값 범위: 유효한 FILE* 또는 NULL (기본). NULL 이면 표준 SPDK 로그 채널로만 출력.
	 * 동기화: spdk_app_start 가 시작된 후 닫지 말 것 — 라이브러리가 fclose 책임. */

	/**
	 * Raw JSON configuration data and its size.
	 * Cannot be used simultaneously with json_config_file option.
	 */
	void *json_data;
	/* [한국어] 메모리에 들고 있는 JSON 설정 원본 포인터.
	 * 설정자: 사용자 — 미리 메모리에 로드한 JSON 버퍼 전달.
	 * 읽는 자: lib/init 이 json_data_size 만큼 파싱하여 RPC replay 수행.
	 * 값 범위: 유효한 메모리 포인터 또는 NULL. json_config_file 과 동시 사용 불가 (둘 중 하나만).
	 * 동기화: spdk_app_start 가 파싱 완료할 때까지 메모리가 유효해야 함. */

	size_t json_data_size;
	/* [한국어] json_data 가 가리키는 버퍼의 바이트 수.
	 * 설정자: 사용자.
	 * 읽는 자: lib/init 의 JSON 파서.
	 * 값 범위: json_data 가 NULL 이면 무시. 그렇지 않으면 양의 정수.
	 * 동기화: 부팅 시 1 회 읽힘. */

	/**
	 * If set, disable CPU claiming.
	 */
	bool disable_cpumask_locks;
	/* [한국어] true 이면 CPU mask 가 겹치는 다른 SPDK 프로세스 검출(파일락) 비활성화.
	 * 설정자: 사용자.
	 * 읽는 자: lib/event 의 부팅 시 cpumask 락 획득 분기.
	 * 값 범위: false=락 검사 수행(기본, 같은 코어 점유 시 부팅 실패), true=검사 스킵.
	 * 의미: 다중 SPDK 프로세스가 의도적으로 같은 코어를 공유할 때 (예: 테스트 환경) 만 사용.
	 * 동기화: 부팅 시 1 회 읽힘. */
} __attribute__((packed));      /* [한국어] 정렬 hole 이 들어가지 않도록 강제 → 명시적 reserved 배열로 ABI 를 통제. 컴파일러 자동 패딩이 들어가면 sizeof 가 변해 SPDK_STATIC_ASSERT 가 깨진다. */

SPDK_STATIC_ASSERT(sizeof(struct spdk_app_opts) == 253, "Incorrect size");
/* [한국어] 컴파일 타임 sizeof 검증 — 누군가 필드를 추가/이동하다 패딩이 바뀌면 빌드가 깨져 ABI 사고를 차단.
 * 새 필드 추가 시 이 상수 값도 함께 갱신해야 한다. 253 은 현재 모든 필드의 누적 오프셋과 일치.
 * SPDK_STATIC_ASSERT 는 spdk/assert.h 에서 _Static_assert (C11) 로 매핑됨. */

/**
 * Initialize the default value of opts
 *
 * \param opts Data structure where SPDK will initialize the default options.
 * \param opts_size Must be set to sizeof(struct spdk_app_opts).
 */
/*
 * [한국어]
 * spdk_app_opts_init - spdk_app_opts 에 SPDK 기본값을 채워넣는 초기화 함수.
 *
 * @opts:      사용자가 스택/힙에 잡은 spdk_app_opts (NULL 불가).
 * @opts_size: sizeof(struct spdk_app_opts). ABI 호환을 위해 caller 시점의 sizeof 를 전달.
 *             이 값은 opts->opts_size 로도 저장되어 spdk_app_start 가 신뢰할 바이트 수의 근거가 된다.
 * @return:    void.
 *
 * 반드시 spdk_app_start 호출 전에 가장 먼저 호출되어야 한다. 이후 사용자는 필요한 필드만
 * 덮어쓰면 된다. 이 함수가 호출 직후라면 모든 reserved 영역도 0 으로 초기화된다 (memset 보장).
 *
 * 실행 컨텍스트: 부팅 직전, 보통 main() 의 단일 스레드. spdk_env_init 보다 먼저 호출 가능.
 *               별도 락 불필요 (각 호출이 자기 opts 만 다룸).
 *
 * 호출 체인: main() → [spdk_app_opts_init] → spdk_app_parse_args() → spdk_app_start()
 */
void spdk_app_opts_init(struct spdk_app_opts *opts, size_t opts_size);

/**
 * Start the framework.
 *
 * Before calling this function, opts must be initialized by
 * spdk_app_opts_init(). Once started, the framework will call start_fn on
 * an spdk_thread running on the current system thread with the
 * argument provided.
 *
 * If opts->delay_subsystem_init is set
 * (e.g. through --wait-for-rpc flag in spdk_app_parse_args())
 * this function will only start a limited RPC server accepting
 * only a few RPC commands - mostly related to pre-initialization.
 * With this option, the framework won't be started and start_fn
 * won't be called until the user sends an `rpc_framework_start_init`
 * RPC command, which marks the pre-initialization complete and
 * allows start_fn to be finally called.
 *
 * This call will block until spdk_app_stop() is called. If an error
 * condition occurs during the initialization code within spdk_app_start(),
 * this function will immediately return before invoking start_fn.
 *
 * \param opts_user Initialization options used for this application. It should not be
 *             NULL. And the opts_size value inside the opts structure should not be zero.
 * \param start_fn Entry point that will execute on an internally created thread
 *                 once the framework has been started.
 * \param ctx Argument passed to function start_fn.
 *
 * \return 0 on success or non-zero on failure.
 */
/*
 * [한국어]
 * spdk_app_start - SPDK 애플리케이션의 메인 부트스트랩. 블로킹 호출.
 *
 * @opts_user: 사용자가 채운 spdk_app_opts. NULL 불가, opts_size 필드 0 불가.
 *             내부에서 사본을 만들기 때문에 호출 후 해제·재사용 가능.
 * @start_fn:  부팅 완료 후 main lcore 의 spdk_thread 에서 실행될 사용자 진입점.
 *             시그니처는 `spdk_msg_fn` (== void(*)(void *ctx)) — 인자 1 개.
 *             (cf. spdk_event_fn 은 인자 2 개. start_fn 이 spdk_msg_fn 인 이유는 spdk_thread
 *              컨텍스트에서 실행되기 때문이며, spdk_thread_send_msg 와 같은 시그니처를 공유.)
 * @ctx:       start_fn 에 그대로 전달될 사용자 컨텍스트 포인터.
 * @return:    0=정상 종료(spdk_app_stop(0)), 비-0=초기화 실패 또는 spdk_app_stop(rc) 의 rc.
 *
 * 동작 단계:
 *  1) opts 검증 → spdk_env_init() (DPDK EAL: hugepage, lcore pthread, mempool 셋업).
 *  2) spdk_thread_lib_init() — message pool 등 thread 인프라 셋업 (msg_mempool_size 사용).
 *  3) reactor_mask 가 가리키는 모든 lcore 에 reactor 객체 생성·기동 (1 lcore = 1 reactor pthread).
 *  4) RPC 서버 시작 (`rpc_addr` listen). delay_subsystem_init=true 면 제한 RPC 매트릭스만 노출.
 *  5) main lcore 의 spdk_thread (= "app thread") 에 메시지로 start_fn(ctx) 실행 예약.
 *  6) 메인 시스템 스레드는 main reactor 의 폴링 루프로 진입 → 블로킹.
 *  7) spdk_app_stop(rc) 수신 시 모든 reactor 종료 → 함수 반환.
 *
 * 실행 컨텍스트: 호출 스레드 = main DPDK lcore 의 reactor 스레드가 됨.
 *               cross-thread 호출은 안전하지 않다 (단일 진입점). 스레드 affinity 는
 *               main_core 옵션으로 결정되며, 호출자는 이 함수가 반환할 때까지 다른 SPDK API 를
 *               병렬로 호출하지 말 것.
 *
 * 에러 처리: 어느 단계에서 실패하든 그 단계까지의 자원을 역순으로 해제한 후 비-0 반환.
 *           spdk_app_fini() 호출 책임은 caller 에게 있음.
 *
 * 호출 체인: main() → [spdk_app_start] → spdk_env_init → reactor_run → start_fn(ctx)
 *           → … → spdk_app_stop(rc) → reactor_stop → 반환.
 */
int spdk_app_start(struct spdk_app_opts *opts_user, spdk_msg_fn start_fn,
		   void *ctx);

/**
 * Perform final shutdown operations on an application using the event framework.
 */
/*
 * [한국어]
 * spdk_app_fini - spdk_app_start 반환 이후 호출하여 잔여 자원을 정리한다.
 *
 * @return: void.
 *
 * spdk_thread_lib_fini, env 정리, RPC 잔여 cleanup, 시그널 핸들러 복원 등을 수행.
 * main() 이 종료되기 직전에 호출. spdk_app_start 가 0 이 아닌 값을 반환해도 정리를 위해
 * 호출하는 것이 안전하다 (idempotent — 이미 정리된 자원은 스킵).
 *
 * 실행 컨텍스트: spdk_app_start 가 반환한 직후, 단일 스레드. 다른 SPDK API 와 병행 호출 금지
 *               (모든 reactor 가 이미 종료된 상태여야 함).
 *
 * 호출 체인: main() → spdk_app_start() → [spdk_app_fini] → return from main()
 */
void spdk_app_fini(void);

/**
 * Start shutting down the framework.
 *
 * Typically this function is not called directly, and the shutdown process is
 * started implicitly by a process signal. But in applications that are using
 * SPDK for a subset of its process threads, this function can be called in lieu
 * of a signal.
 */
/*
 * [한국어]
 * spdk_app_start_shutdown - 비동기 종료 프로세스 트리거 (graceful shutdown).
 *
 * @return: void. 즉시 반환되며 실제 종료는 백그라운드(메인 reactor) 에서 진행.
 *
 * 일반적으로는 SIGINT/SIGTERM 시그널 핸들러가 자동 호출하지만, SPDK 를 라이브러리로
 * 임베드해서 시그널 핸들러를 사용하지 않거나(disable_signal_handlers=true) 사용자 정의
 * 종료 트리거가 필요한 경우 직접 호출한다.
 *
 * 동작:
 *   - shutdown_cb 등록되어 있으면 메인 reactor 메시지로 호출 → 콜백 본문에서 spdk_app_stop 필요.
 *   - 등록되어 있지 않으면 spdk_subsystem_fini → spdk_app_stop(0) 자동 진행.
 *
 * 실행 컨텍스트: 어느 스레드에서 호출해도 안전 — 내부에서 spdk_thread_send_msg 로 메인
 *               spdk_thread 에 위임되므로 시그널 핸들러 안에서도 호출 가능.
 *               (단, 같은 종료 절차의 이중 호출 방지 atomic flag 가 내부에 존재.)
 *
 * 호출 체인: 시그널/사용자 코드 → [spdk_app_start_shutdown]
 *           → 메인 spdk_thread 메시지 → shutdown_cb 또는 subsystem_fini → spdk_app_stop(0)
 */
void spdk_app_start_shutdown(void);

/**
 * Stop the framework.
 *
 * This does not wait for all threads to exit. Instead, it kicks off the shutdown
 * process and returns. Once the shutdown process is complete, spdk_app_start()
 * will return.
 *
 * \param rc The rc value specified here will be returned to caller of spdk_app_start().
 */
/*
 * [한국어]
 * spdk_app_stop - 종료 프로세스를 시작하고 즉시 반환한다 (non-blocking).
 *
 * @rc: spdk_app_start 의 반환값으로 전달될 종료 코드 (0=정상, 비-0=에러).
 * @return: void. 모든 스레드가 종료되기를 기다리지 않는다.
 *
 * spdk_app_start_shutdown 과 거의 같지만 종료 코드를 직접 지정할 수 있다는 점이 다르다.
 * 보통 사용자 start_fn 또는 그 자식 콜백이 작업 완료/에러 처리 후 호출한다.
 *
 * 동작: rc 를 전역에 저장 → 모든 reactor 에 STOP 메시지 fan-out → 각 reactor 가 자기
 *      polling 루프를 빠져나옴 → 메인 reactor 가 spdk_app_start 의 스택을 풀고 rc 반환.
 *
 * 실행 컨텍스트: SPDK thread 위에서 호출 권장. cross-thread 안전성은 내부 메시지로 보장됨.
 *
 * 호출 체인: 사용자 콜백 → [spdk_app_stop(rc)] → reactor_stop 메시지 → spdk_app_start 반환.
 */
void spdk_app_stop(int rc);

/**
 * Return the shared memory id for this application.
 *
 * \return shared memory id.
 */
/*
 * [한국어]
 * spdk_app_get_shm_id - 현재 애플리케이션의 공유메모리 ID(opts.shm_id) 반환.
 *
 * @return: shm_id. -1 이면 single-process, >=0 이면 multi-process group ID.
 *
 * trace/RPC 등이 같은 그룹에 속한 secondary 프로세스를 식별할 때 사용.
 * 부팅 시점의 opts.shm_id 가 라이브러리 전역에 캐시되며 이후 변경되지 않는다.
 *
 * 실행 컨텍스트: 어느 스레드에서나 호출 가능 (read-only 전역 값).
 */
int spdk_app_get_shm_id(void);

/**
 * Convert a string containing a CPU core mask into a bitmask
 *
 * \param mask String containing a CPU core mask.
 * \param cpumask Bitmask of CPU cores.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_app_parse_core_mask - "0xF" 같은 hex 문자열 또는 "[0,1,2]" 리스트를
 *                            spdk_cpuset 비트맵으로 파싱.
 *
 * @mask:    "0x..." 또는 "[c1,c2,..]" 형식의 NULL-종단 문자열.
 * @cpumask: 결과를 받을 spdk_cpuset (caller 소유, 사전 zero-init 권장).
 * @return:  0=성공, -1=파싱 실패(잘못된 형식 또는 NULL).
 *
 * 사용자 인자 검증·표시용 헬퍼. 실제 reactor 마스크 적용은 spdk_app_start 내부에서 수행.
 *
 * 실행 컨텍스트: 어디서나 호출 가능 (순수 문자열 변환, 부수효과 없음, 재진입 안전).
 */
int spdk_app_parse_core_mask(const char *mask, struct spdk_cpuset *cpumask);

/**
 * Get the mask of the CPU cores active for this application
 *
 * \return the bitmask of the active CPU cores.
 */
/*
 * [한국어]
 * spdk_app_get_core_mask - 현재 활성 reactor 가 도는 코어들의 비트맵 반환.
 *
 * @return: spdk_cpuset 포인터 (라이브러리 소유 — 수정·해제 금지, read-only).
 *
 * 활성 코어 목록을 RPC/UI 에 노출하거나 코어별 통계 수집 시 활용.
 *
 * 실행 컨텍스트: spdk_app_start 이후에만 의미 있음 (그 전에는 NULL 또는 빈 마스크).
 *               read-only 이므로 어느 스레드에서나 호출 가능.
 */
const struct spdk_cpuset *spdk_app_get_core_mask(void);

#define SPDK_APP_GETOPT_STRING "c:de:ghi:m:n:p:r:s:uvA:B:L:RW:"
/* [한국어] SPDK 표준 short option 집합 — getopt(3) 형식.
 * 의미 매핑:
 *   c=config(JSON 설정 파일), d=debug enable, e=tpoint(트레이스포인트),
 *   g=single seg(hugepage 단일 세그먼트), h=help, i=shm_id, m=core mask(reactor_mask),
 *   n=mem channels(mem_channel), p=main core, r=rpc addr, s=mem size,
 *   u=no_pci, v=version, A=pci allow, B=pci block, L=log flag,
 *   R=hugepage unlink, W=disable warnings.
 * 사용자 앱이 자기 옵션을 추가할 때 이 문자열의 글자와 충돌해서는 안 된다 — spdk_app_parse_args 의
 * getopt_str 인자에 추가 글자만 넘기면 라이브러리가 두 문자열을 합쳐 getopt_long 에 사용한다. */

enum spdk_app_parse_args_rvals {
	SPDK_APP_PARSE_ARGS_HELP = 0,
	/* [한국어] 사용자가 `-h` 를 입력해 도움말이 출력된 경우.
	 * 설정자: spdk_app_parse_args 의 분기.
	 * 읽는 자: main() 의 후처리 — 보통 0 을 반환하며 정상 종료.
	 * 값: 0 (enum 첫 항목, 명시 지정). */

	SPDK_APP_PARSE_ARGS_SUCCESS = 1,
	/* [한국어] 정상 파싱 — opts 에 모든 값이 반영되었으며 spdk_app_start 로 진행 가능.
	 * 설정자: spdk_app_parse_args 의 정상 종료 경로.
	 * 읽는 자: main() — 이 값을 받으면 spdk_app_start 호출.
	 * 값: 1. */

	SPDK_APP_PARSE_ARGS_FAIL = 2
	/* [한국어] 잘못된 옵션/사용자 콜백 실패.
	 * 설정자: spdk_app_parse_args 의 에러 경로 (잘못된 short opt, parse 콜백 비-0 반환 등).
	 * 읽는 자: main() — 이 값을 받으면 비-0 종료 코드로 프로세스 종료해야 함.
	 * 값: 2. */
};
typedef enum spdk_app_parse_args_rvals spdk_app_parse_args_rvals_t;
/* [한국어] enum 의 typedef alias — typedef 형태로 함수 시그니처(spdk_app_parse_args 의 반환형)
 * 에 노출하기 위한 관례. C 에서 enum 타입을 직접 쓰면 "enum X" 로 명시해야 하므로 typedef 가 편의. */

/**
 * Helper function for parsing arguments and printing usage messages.
 *
 * \param argc Count of arguments in argv parameter array.
 * \param argv Array of command line arguments.
 * \param opts Default options for the application.
 * \param getopt_str String representing the app-specific command line parameters.
 *        Characters in this string must not conflict with characters in SPDK_APP_GETOPT_STRING.
 *        This argument is optional.
 * \param app_long_opts Array of full-name parameters. This argument is optional.
 * \param parse Function pointer to call if an argument in getopt_str is found.
 *        This argument is optional but only if getopt_str is not provided.
 * \param usage Function pointer to print usage messages for app-specific command
 *        line parameters. This argument is optional.
 *\return SPDK_APP_PARSE_ARGS_FAIL on failure, SPDK_APP_PARSE_ARGS_SUCCESS on
 *        success, SPDK_APP_PARSE_ARGS_HELP if '-h' passed as an option.
 */
/*
 * [한국어]
 * spdk_app_parse_args - argc/argv 를 받아 SPDK 표준 옵션을 opts 에 채우고
 *                       앱 고유 옵션은 사용자 parse 콜백으로 위임하는 통합 헬퍼.
 *
 * @argc / @argv:   main() 이 받은 그대로 전달.
 * @opts:           spdk_app_opts_init 후 사용자가 일부 기본값을 덮어쓴 상태로 전달.
 *                  파싱 결과(reactor_mask, rpc_addr 등)가 이 구조체에 누적 반영된다.
 * @getopt_str:     앱 고유 short option 추가 문자열(예: "f:b"). NULL 이면 표준만.
 *                  SPDK_APP_GETOPT_STRING 의 글자와 겹치면 안 된다.
 * @app_long_opts:  getopt_long 용 long option 배열. NULL 이면 표준 long opt 만.
 * @parse:          getopt 가 앱 고유 옵션을 만나면 호출할 콜백 (ch=옵션 글자, arg=optarg).
 *                  반환 0=계속, 비-0=실패 → spdk_app_parse_args 가 FAIL 반환.
 * @usage:          `-h` 입력 시 앱 고유 옵션 부분의 도움말을 인쇄할 콜백 (옵션).
 * @return:         HELP / SUCCESS / FAIL — main() 이 분기 처리.
 *
 * 실행 컨텍스트: 부팅 직전, 단일 스레드 (보통 main()). 부수효과는 opts 에 쓰기 +
 *               stdout 으로 도움말 출력뿐이라 안전.
 *
 * 호출 체인: main() → spdk_app_opts_init → [spdk_app_parse_args] → spdk_app_start.
 */
spdk_app_parse_args_rvals_t spdk_app_parse_args(int argc, char **argv,
		struct spdk_app_opts *opts, const char *getopt_str,
		const struct option *app_long_opts, int (*parse)(int ch, char *arg),
		void (*usage)(void));

/**
 * Print usage strings for common SPDK command line options.
 *
 * May only be called after spdk_app_parse_args().
 */
/*
 * [한국어]
 * spdk_app_usage - SPDK 공통 옵션의 도움말 텍스트를 stdout 에 출력.
 *
 * @return: void.
 *
 * 일반적으로 사용자의 usage 콜백 안에서 호출된다 (앱 고유 옵션 위/아래에 배치).
 * spdk_app_parse_args 이전에 호출하면 일부 동적 디폴트 값(예: 자동 감지된 huge_dir)이
 * 채워지지 않아 정확하지 않을 수 있다.
 *
 * 실행 컨텍스트: 단일 스레드, stdout 출력만 — 재진입 비안전 (printf 의 stdout 락에 의존).
 */
void spdk_app_usage(void);

/**
 * Allocate an event to be passed to spdk_event_call().
 *
 * \param lcore Lcore to run this event.
 * \param fn Function used to execute event.
 * \param arg1 Argument passed to function fn.
 * \param arg2 Argument passed to function fn.
 *
 * \return a pointer to the allocated event.
 */
/*
 * [한국어]
 * spdk_event_allocate - 다른 lcore 에서 실행할 함수+인자 묶음(spdk_event)을 mempool 에서 할당.
 *
 * @lcore: 이벤트 실행 대상 lcore (반드시 활성 reactor 가 있는 코어 — opts.reactor_mask 에 포함).
 * @fn:    그 lcore 에서 호출될 콜백 (signature: spdk_event_fn — 인자 2 개).
 * @arg1, @arg2: 콜백에 그대로 전달될 인자 (소유권은 이벤트가 실행될 때까지 호출자에게 있음 —
 *               즉 이 인자들이 가리키는 메모리는 fn 이 호출되는 시점까지 유효해야 함).
 * @return: 할당된 spdk_event* (실패 시 NULL — mempool 고갈, 거의 발생하지 않게 사전 할당).
 *
 * SPDK 의 cross-core 메시지 패싱 프리미티브. 내부적으로 lockless ring 기반이라 락 없이
 * 다중 생산자가 동시에 호출 가능. spdk_event_call 로 실제 enqueue 한 뒤 잊으면 됨
 * (이벤트 객체는 실행 후 자동 free).
 *
 * spdk_thread_send_msg 와의 차이:
 *   - 디스패치 단위가 thread 가 아닌 lcore 라 — 어느 spdk_thread 에서 실행될지는 reactor 가 결정.
 *   - 인자가 1 개(send_msg) 가 아닌 2 개 → 별도 컨텍스트 구조체 할당 없이 두 포인터 전달 가능.
 *   - 일반적으로 reactor 인프라/scheduler 등 저수준 코드가 사용. 라이브러리 코드는 send_msg 선호.
 *
 * 실행 컨텍스트: 어떤 SPDK 스레드에서 호출해도 안전 (락 없는 SPSC/MPSC ring).
 *               파라미터 검증만 수행하므로 빠르게 반환.
 *
 * 사용 예: NVMe 완료 콜백을 다른 코어로 디스패치할 때, RPC 결과를 main 코어로 보낼 때.
 */
struct spdk_event *spdk_event_allocate(uint32_t lcore, spdk_event_fn fn,
				       void *arg1, void *arg2);

/**
 * Pass the given event to the associated lcore and call the function.
 *
 * \param event Event to execute.
 */
/*
 * [한국어]
 * spdk_event_call - 할당된 이벤트를 대상 lcore 의 ring 에 enqueue (실제 디스패치).
 *
 * @event: spdk_event_allocate 가 반환한 포인터. 호출 후 소유권은 SPDK 이벤트 시스템에 이전 —
 *         caller 는 이 포인터에 더 이상 접근하면 안 된다.
 * @return: void. 실패 케이스가 사실상 없도록 ring 을 사전 할당해 둠 (mempool 고갈 시점에 이미
 *         allocate 가 NULL 을 반환했을 것).
 *
 * 동작: 대상 lcore 의 lockless ring 에 이벤트 푸시 → 그 lcore 의 reactor 폴링 루프가
 *       다음 iter 에서 dequeue 후 fn(arg1, arg2) 실행 → 이벤트 객체를 mempool 에 반환.
 *
 * 실행 컨텍스트: 어떤 스레드에서 호출해도 안전 (lockless MPSC ring).
 *               단, allocate 후 call 전에 lcore 가 죽으면 누수 가능 → 반드시 짝지어 호출.
 *               시그널 핸들러에서는 호출 금지 (rte_ring 이 async-signal-safe 가 아님).
 */
void spdk_event_call(struct spdk_event *event);

/**
 * Enable or disable monitoring of context switches.
 *
 * \param enabled True to enable, false to disable.
 */
/*
 * [한국어]
 * spdk_framework_enable_context_switch_monitor - reactor 스레드의 컨텍스트 스위치
 *                                                  발생을 감시하는 기능을 켜고 끈다.
 *
 * @enabled: true=감시 ON (reactor 스레드의 nvcsw/nivcsw 변화 추적), false=OFF.
 * @return:  void.
 *
 * Polled-mode reactor 는 원칙적으로 컨텍스트 스위치가 없어야 함 (CPU 100% 점유 의도).
 * 스위치가 발생한다면 시그널·페이지폴트·시스콜로 인한 성능 저하 신호다. ON 상태에선
 * 일정 주기로 /proc/[pid]/task/[tid]/status 를 읽어 voluntary/involuntary cs 카운터를
 * 비교하며, 비정상 스위치 발견 시 경고 로그(SPDK_WARNLOG) 를 띄운다.
 *
 * 실행 컨텍스트: 어느 SPDK 스레드에서나 호출 가능 (내부에 atomic 토글). 모니터 자체는
 *               별도 poller 로 구현되어 메인 reactor 에 등록됨.
 */
void spdk_framework_enable_context_switch_monitor(bool enabled);

/**
 * Return whether context switch monitoring is enabled.
 *
 * \return true if enabled or false otherwise.
 */
/*
 * [한국어]
 * spdk_framework_context_switch_monitor_enabled - 위 모니터링의 현재 상태를 조회.
 *
 * @return: true=감시 중, false=비활성.
 *
 * RPC `framework_get_config` 같은 곳에서 현재 설정을 보고할 때 사용.
 *
 * 실행 컨텍스트: read-only — 어느 스레드에서나 호출 가능.
 */
bool spdk_framework_context_switch_monitor_enabled(void);

/**
 * Set up SPDK tracing for the application.
 *
 * Initializes the trace shared memory region and enables tracepoint groups or
 * individual tracepoints as specified in the spdk_app_opts structure.
 *
 * \param opts Application options structure, must be initialized.
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_app_setup_trace - opts.tpoint_group_mask / opts.num_entries 에 따라 trace 인프라 셋업.
 *
 * @opts:   spdk_app_opts_init 호출 후의 opts. tpoint_group_mask, num_entries, num_trace_threads,
 *          shm_id 등을 입력으로 사용.
 * @return: 0=성공, -1=공유메모리 매핑 실패 또는 잘못된 마스크.
 *
 * 동작: shm_open(2) 으로 trace 영역(코어당 num_entries 슬롯) 매핑 → 지정 그룹의 tpoint 를 enable
 *       (per-tpoint 비트마스크 설정).
 * 일반적으로 spdk_app_start 가 자동 호출하므로 사용자가 명시적으로 부를 일은 드물지만,
 * 임베디드 환경에서 spdk_env 만 띄우고 trace 는 따로 설정할 때 직접 호출 가능.
 *
 * 실행 컨텍스트: spdk_env_init 이후, reactor 시작 전에만 호출해야 안전 — 이미 reactor 가
 *               trace 를 쓰고 있으면 매핑 변경이 데이터 레이스를 유발한다.
 */
int spdk_app_setup_trace(struct spdk_app_opts *opts);

#ifdef __cplusplus              /* [한국어] C++ extern "C" 블록 종결 — 위 #ifdef __cplusplus 와 짝. */
}
#endif

#endif                          /* [한국어] include guard 종결 (SPDK_EVENT_H) — 파일 첫 줄의 #ifndef 와 짝. */
