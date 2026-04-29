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
 * 실행시키는 cross-core 메시지 패싱 프리미티브이다.
 * 또한 `--cpumask`, `-r`(RPC), `-t`(tracepoint) 같은 SPDK 표준 CLI 옵션을 파싱하는
 * `spdk_app_parse_args()` 헬퍼와 트레이스 셋업 진입점도 함께 제공한다.
 * SPDK의 모든 메인 애플리케이션(nvmf_tgt, vhost, iscsi_tgt, spdk_tgt 등)은 이 헤더만
 * 인클루드하면 reactor·subsystem·RPC 서버까지 한 번에 부팅할 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 부팅 흐름의 가장 바깥 껍질이다. 호출 체인은 다음과 같다:
 *   main() (app/nvmf_tgt 등)
 *     → spdk_app_opts_init() (이 헤더)
 *     → spdk_app_parse_args() (이 헤더, getopt 래퍼)
 *     → spdk_app_start(opts, start_fn, ctx) (이 헤더)
 *         → spdk_env_init() (DPDK EAL 초기화: hugepage, lcore, mempool)
 *         → spdk_thread_lib_init() (lib/thread)
 *         → 각 lcore에 reactor 생성 → polling loop 진입
 *         → main lcore의 spdk_thread에서 start_fn(ctx) 호출
 *             → 일반적으로 start_fn 내부에서 spdk_subsystem_init() 호출 (init.h)
 *     → (사용자 코드 실행, polling)
 *     → spdk_app_stop(rc) → reactor exit → spdk_app_start() 반환
 *     → spdk_app_fini() (정리)
 * 실행 컨텍스트: 호스트 유저스페이스, DPDK EAL이 만든 lcore pthread들 위에서 동작.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/cpuset.h(CPU 마스크), spdk/init.h(subsystem 초기화 인터페이스), spdk/thread.h
 * (`spdk_msg_fn` 타입과 spdk_thread API), spdk/log.h(`spdk_log_level`, `spdk_log_cb`),
 * spdk/queue.h(BSD TAILQ 매크로), spdk/assert.h(`SPDK_STATIC_ASSERT`).
 * 의존받는 쪽: lib/event/(reactor.c, app.c — 이 헤더의 구현체), 모든 SPDK 메인 앱(app/*),
 * 일부 예제(examples/) 그리고 RPC 핸들러(`framework_*` 그룹).
 * 데이터 흐름: 사용자가 채운 `spdk_app_opts` → `spdk_app_start()`가 그 값을 DPDK EAL CLI
 * (`-c`, `-n`, `-m`, `--huge-dir` 등) 인자로 변환 → DPDK가 hugepage/lcore를 셋업 →
 * SPDK reactor가 깨어나면서 사용자 `start_fn` 콜백 실행. 종료 시 `spdk_app_stop(rc)`이
 * 모든 reactor에 SHUTDOWN 메시지를 보내고 메인 reactor는 `rc`를 반환.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_app_opts: 애플리케이션 부팅 옵션 패킹 구조체. ABI 호환을 위해
 *   `opts_size`(caller가 본 sizeof) 필드와 `reservedNN[N]` 홀들을 포함하며 packed 정렬.
 * - spdk_app_opts_init(): 모든 필드를 SPDK 기본값으로 채움 (반드시 가장 먼저 호출).
 * - spdk_app_parse_args(): argc/argv를 받아 표준 SPDK 옵션을 opts에 반영하고 앱 고유 옵션은
 *   사용자 콜백으로 위임. SPDK_APP_GETOPT_STRING이 표준 short option 집합을 정의.
 * - spdk_app_start(): EAL 초기화 + reactor 기동 + start_fn 호출. 블로킹 함수.
 * - spdk_app_stop()/spdk_app_start_shutdown(): 비동기 종료 트리거.
 * - spdk_event_allocate()/spdk_event_call(): 임의 lcore에서 함수를 실행시키는
 *   cross-core 메시지 프리미티브 (lockless ring 기반). spdk_thread API의 저수준 형태.
 * - spdk_app_setup_trace(): tpoint 그룹 마스크에 따라 trace 공유 메모리 셋업.
 */

#ifndef SPDK_EVENT_H            /* [한국어] include guard 시작 — 다중 인클루드 방지 (헤더 표준 패턴). */
#define SPDK_EVENT_H            /* [한국어] include guard 매크로 정의 — 동일 TU에서 두 번째 인클루드부터는 무시됨. */

#include "spdk/stdinc.h"        /* [한국어] SPDK 표준 헤더 모음(stddef, stdint, stdbool, stdio, sys/types 등)을 한 번에 끌어옴 — 플랫폼 의존을 한 곳에 격리. */

#include "spdk/cpuset.h"        /* [한국어] `struct spdk_cpuset` 정의 — `spdk_app_get_core_mask()`, `spdk_app_parse_core_mask()`의 비트마스크 표현. */
#include "spdk/init.h"          /* [한국어] subsystem init/fini 인터페이스 — `spdk_app_start`가 내부적으로(또는 사용자 start_fn에서) 호출하는 후속 단계. */
#include "spdk/queue.h"         /* [한국어] BSD TAILQ/SLIST 매크로 — 본 헤더 자체는 직접 사용하지 않으나 타 헤더 호환을 위해 일관 인클루드. */
#include "spdk/log.h"           /* [한국어] `enum spdk_log_level`, `spdk_log_cb` 타입 — `spdk_app_opts.print_level`, `.log` 필드에서 사용. */
#include "spdk/thread.h"        /* [한국어] `spdk_msg_fn`, `spdk_poller` 등 — `spdk_app_start`의 `start_fn` 시그니처와 poller 전방 선언에 필요. */
#include "spdk/assert.h"        /* [한국어] `SPDK_STATIC_ASSERT` — `spdk_app_opts`의 sizeof를 컴파일 타임에 고정해 ABI 깨짐을 방지. */

#ifdef __cplusplus              /* [한국어] C++에서 인클루드될 때 이름 맹글링을 피하기 위해 extern "C" 블록을 연다. */
extern "C" {                    /* [한국어] 이하 선언들을 C linkage로 강제 — SPDK는 C 구현이지만 사용자 앱은 C++일 수 있음. */
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
 * @arg1: 사용자가 spdk_event_allocate에 전달한 첫 번째 인자 (불투명 포인터).
 * @arg2: 두 번째 인자 (보통 컨텍스트 또는 콜백 인자).
 * @return: void — 결과는 보통 arg1/arg2가 가리키는 컨텍스트 안의 필드로 보고됨.
 *
 * 이 콜백은 `spdk_event_call()`이 큐잉하면 *대상 lcore* 의 reactor 폴링 루프에서 실행된다.
 * 즉 함수 본문이 실행되는 스레드 컨텍스트는 호출자의 스레드가 아니라 `spdk_event_allocate(lcore=…)`
 * 로 지정된 lcore의 reactor임에 주의. 본문 안에서는 그 lcore에 묶인 자원만 안전하게 만질 수 있다.
 */
typedef void (*spdk_event_fn)(void *arg1, void *arg2);  /* [한국어] cross-core 함수 호출 시 사용되는 callback signature. */

/**
 * \brief An event is a function that is passed to and called on an lcore.
 */
/*
 * [한국어]
 * struct spdk_event - 다른 lcore에서 실행할 함수 + 인자 두 개를 묶어 캡슐화하는 불투명 구조체.
 * 정의는 lib/event/ 내부에 숨겨져 있으며 사용자는 포인터만 다룬다.
 * 메모리는 `spdk_event_allocate()`가 lockless mempool에서 할당하고
 * `spdk_event_call()`이 대상 lcore의 ring에 push하면서 소유권을 이전한다.
 */
struct spdk_event;              /* [한국어] 이벤트 객체 전방 선언 — 사용자 코드는 포인터로만 접근. */

/**
 * \brief A poller is a function that is repeatedly called on an lcore.
 */
/*
 * [한국어]
 * struct spdk_poller - reactor 폴링 루프에서 매 반복마다(또는 일정 주기로) 호출되는 콜백 핸들.
 * 정의는 lib/thread/에 위치. 본 헤더에서는 전방 선언만 두어 다른 헤더와의 의존을 줄임.
 * Polled-mode 설계의 핵심 — 인터럽트 없이 NVMe CQ, 소켓, RPC 등을 주기적으로 검사한다.
 */
struct spdk_poller;             /* [한국어] poller 전방 선언 — 실제 정의는 spdk/thread.h. */

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
 * `spdk_app_opts.shutdown_cb`로 등록하면 시그널 핸들러가 이를 호출하고, 사용자는 그 안에서
 * 자원 정리 후 직접 `spdk_app_stop(rc)`을 불러야 reactor가 멈춘다. NULL이면 SPDK가
 * 기본 절차(subsystem_fini → spdk_app_stop(0))를 수행한다.
 */
typedef void (*spdk_app_shutdown_cb)(void);  /* [한국어] 사용자 정의 종료 콜백 타입 — opts.shutdown_cb에 저장. */

/**
 * Signal handler function.
 *
 * \param signal Signal number.
 */
/*
 * [한국어]
 * spdk_sighandler_t - POSIX `sa_handler`와 동일한 형식의 시그널 핸들러 타입.
 *
 * @signal: 전달된 시그널 번호 (SIGINT, SIGTERM, SIGUSR1 등).
 * @return: void.
 *
 * SPDK 내부에서 시그널 → 종료 경로를 사용자 정의로 바꾸고 싶을 때 쓰이는 보조 타입.
 * (현재 헤더 내 다른 함수 시그니처에는 직접 노출되지 않지만 ABI 차원에서 정의해 둠.)
 */
typedef void (*spdk_sighandler_t)(int signal);  /* [한국어] POSIX 시그널 핸들러 함수 포인터 타입. */

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
 * 필드를 갖는 점이 특징이다. 새 필드는 보통 끝(opts_size 다음)에 추가되며, 라이브러리는
 * caller가 본 opts_size까지만 읽고 그 이후는 기본값으로 채운다.
 *
 * 사용 흐름:
 *   spdk_app_opts opts; spdk_app_opts_init(&opts, sizeof(opts));
 *   opts.name = "my_app"; opts.reactor_mask = "0xF"; ...;
 *   spdk_app_start(&opts, my_start_fn, ctx);
 */
struct spdk_app_opts {
	const char *name;
	/* [한국어] 애플리케이션 이름 (예: "nvmf_tgt"). 로그/trace 파일명/proc title에 쓰임.
	 * 설정자: 사용자 코드가 spdk_app_opts_init 후에 직접 대입.
	 * 읽는 자: spdk_app_start 내부에서 DPDK EAL과 trace 서브시스템에 전달.
	 * 값 범위: NULL 불가 권장 (NULL이면 SPDK가 기본 이름을 부여). */

	const char *json_config_file;
	/* [한국어] 부팅 시 자동 로드할 JSON 설정 파일 경로 (선택). NULL이면 RPC로만 구성.
	 * 설정자: 사용자 또는 `--json` CLI 옵션을 통해 spdk_app_parse_args가 채움.
	 * 읽는 자: subsystem 초기화 단계에서 RPC를 재생(replay)하기 위해 읽힘.
	 * 값 범위: 유효한 파일 경로 또는 NULL. json_data와 동시 사용 불가. */

	bool json_config_ignore_errors;
	/* [한국어] JSON 설정 RPC 중 일부가 실패해도 부팅을 계속할지 여부.
	 * 설정자: 사용자 또는 `--json-ignore-init-errors` CLI 옵션.
	 * 읽는 자: spdk_subsystem_load_config의 stop_on_error로 전달.
	 * 값 범위: false=하나라도 실패하면 종료, true=무시하고 계속. */

	/* Hole at bytes 17-23. */
	uint8_t	reserved17[7];
	/* [한국어] ABI alignment용 패딩 — 새 필드 추가 시 이 영역을 사용할 수 있다.
	 * 0으로 초기화 보장. packed 구조체에서 후속 필드의 정렬·sizeof를 안정화한다. */

	const char *rpc_addr; /* Can be UNIX domain socket path or IP address + TCP port */
	/* [한국어] JSON-RPC 서버가 listen할 주소.
	 * 설정자: 사용자 또는 `-r` CLI 옵션. 기본 SPDK_DEFAULT_RPC_ADDR(/var/tmp/spdk.sock).
	 * 읽는 자: spdk_rpc_initialize()에 전달.
	 * 값 범위: "/path/to/sock" (Unix domain) 또는 "1.2.3.4:5260" (TCP) 형식. */

	const char *reactor_mask;
	/* [한국어] reactor를 띄울 CPU 코어의 16진수 비트마스크 문자열 (DPDK `-c` 인자와 동일).
	 * 설정자: 사용자 또는 `-m` CLI 옵션. 예: "0xF" → core 0~3에 reactor 4개.
	 * 읽는 자: env_dpdk가 EAL `-c` 옵션으로 변환하여 lcore 활성화에 사용.
	 * 값 범위: 0x로 시작하는 hex 또는 [c1,c2,c3] 콤마 리스트. NULL이면 모든 코어. */

	const char *tpoint_group_mask;
	/* [한국어] 활성화할 trace point 그룹의 비트마스크 문자열.
	 * 설정자: 사용자 또는 `--tpoint-group-mask` 옵션. 예: "0xFFFF" 또는 "bdev,nvmf".
	 * 읽는 자: spdk_app_setup_trace()가 파싱하여 trace 공유 메모리 영역에 반영.
	 * 값 범위: hex 또는 그룹명 콤마 리스트. NULL이면 트레이스 비활성. */

	int shm_id;
	/* [한국어] 다중 SPDK 프로세스가 같은 hugepage/lcore 자원을 공유할 때의 식별자.
	 * 설정자: 사용자 또는 `-i` CLI 옵션.
	 * 읽는 자: env_dpdk가 EAL `--proc-type=secondary` + `--file-prefix`에 매핑.
	 * 값 범위: -1=primary 단독, >=0=multi-process group ID. */

	/* Hole at bytes 52-55. */
	uint8_t			reserved52[4];
	/* [한국어] ABI 패딩 — 향후 4바이트 정수 필드 추가용 예약 영역. */

	spdk_app_shutdown_cb	shutdown_cb;
	/* [한국어] SIGINT/SIGTERM 또는 spdk_app_start_shutdown() 호출 시 실행될 사용자 콜백.
	 * 설정자: 사용자 코드.
	 * 읽는 자: 시그널 핸들러 / spdk_app_start_shutdown() 내부.
	 * 값 범위: 함수 포인터 또는 NULL(=SPDK 기본 종료 절차 사용). */

	bool			enable_coredump;
	/* [한국어] DPDK가 hugepage 영역을 코어덤프에 포함시킬지(MADV_DONTDUMP 해제) 여부.
	 * 설정자: 사용자. 기본 true (디버깅 편의).
	 * 읽는 자: env_dpdk init.
	 * 값 범위: true=hugepage 포함, false=제외(코어덤프 크기 절약). */

	/* Hole at bytes 65-67. */
	uint8_t			reserved65[3];
	/* [한국어] bool 다음 4바이트 정수 정렬을 위한 패딩. */

	int			mem_channel;
	/* [한국어] DPDK 메모리 채널 수 (`-n` 옵션). NUMA 인터리빙 결정에 영향.
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk가 EAL에 전달.
	 * 값 범위: -1=자동 감지, 1..N=명시적 채널 수. */

	int			main_core;
	/* [한국어] DPDK main lcore 번호 (`--main-lcore`).
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk가 EAL에 전달, SPDK는 이 lcore에서 start_fn을 실행.
	 * 값 범위: -1=reactor_mask의 첫 비트, 0..N=특정 코어. */

	int			mem_size;
	/* [한국어] DPDK가 사전 할당할 hugepage 메모리 크기 (MiB). `-s` 옵션과 매핑.
	 * 설정자: 사용자.
	 * 읽는 자: env_dpdk → EAL `--socket-mem`/`-m`.
	 * 값 범위: -1=동적, >=0=고정 MiB. */

	bool			no_pci;
	/* [한국어] DPDK PCI 디바이스 probe를 비활성화할지 (`--no-pci`).
	 * 읽는 자: env_dpdk.
	 * 값 범위: true=NVMe 등 PCI 디바이스 probe 안 함(memory bdev만 쓸 때 유용). */

	bool			hugepage_single_segments;
	/* [한국어] 모든 hugepage를 IOVA-연속 단일 세그먼트로 강제할지 (`--single-file-segments`).
	 * 값 범위: true=DPDK 18.05+의 메모리 hotplug 호환 모드. */

	bool			unlink_hugepage;
	/* [한국어] EAL이 종료 시 hugepage 백업 파일을 unlink할지 (`--huge-unlink`).
	 * 값 범위: true=프로세스 종료 후 hugepage 파일 정리. */

	bool			no_huge;
	/* [한국어] hugepage 없이 일반 anonymous 메모리로 EAL을 띄울지 (`--no-huge`).
	 * 값 범위: true=hugepage 미사용 (테스트/CI 용도, 성능 저하). */

	/* Hole at bytes 84-87. */
	uint8_t			reserved84[4];
	/* [한국어] 4바이트 정렬 패딩. */

	const char		*hugedir;
	/* [한국어] hugepage 마운트 디렉토리 경로 (`--huge-dir`).
	 * 값 범위: "/dev/hugepages" 등. NULL이면 EAL이 기본 위치 탐색. */

	enum spdk_log_level	print_level;
	/* [한국어] stderr 출력 로그 레벨 임계값 (이 이상의 레벨만 출력).
	 * 값 범위: SPDK_LOG_DISABLED..SPDK_LOG_DEBUG. 기본 SPDK_LOG_NOTICE. */

	/* Hole at bytes 100-103. */
	uint8_t			reserved100[4];
	/* [한국어] enum 후 정렬 패딩. */

	size_t			num_pci_addr;
	/* [한국어] pci_blocked/pci_allowed 배열의 원소 수.
	 * 두 배열은 동일 길이를 공유한다. */

	struct spdk_pci_addr	*pci_blocked;
	/* [한국어] probe에서 제외할 PCI 주소 배열 (`-B` 옵션).
	 * 읽는 자: env_dpdk가 EAL `--block`으로 변환. */

	struct spdk_pci_addr	*pci_allowed;
	/* [한국어] probe할 PCI 주소 화이트리스트 (`-A` 옵션). NULL이면 전체.
	 * 읽는 자: env_dpdk가 EAL `--allow`로 변환. */

	const char		*iova_mode;
	/* [한국어] DPDK IOVA 모드 강제 ("pa"=물리주소, "va"=가상주소).
	 * 값 범위: "pa"|"va"|NULL(=자동). VFIO/UIO 환경에 따라 자동 결정. */

	/* Wait for the associated RPC before initializing subsystems
	 * when this flag is enabled.
	 */
	bool			delay_subsystem_init;
	/* [한국어] true이면 subsystem 초기화를 RPC `framework_start_init` 수신까지 지연.
	 * 설정자: `--wait-for-rpc` CLI 옵션.
	 * 의미: 부팅 직후 제한된 RPC 셋만 활성화 → 사용자가 사전 설정 RPC를 보낸 뒤
	 *       명시적으로 init 트리거. 자동화 스크립트에서 동적 구성에 유용. */

	/* Hole at bytes 137-143. */
	uint8_t			reserved137[7];
	/* [한국어] 8바이트 정렬 패딩 (다음 uint64_t 필드를 위해). */

	/* Number of trace entries allocated for each core */
	uint64_t		num_entries;
	/* [한국어] 코어당 trace ring buffer에 할당할 엔트리 수.
	 * 읽는 자: spdk_app_setup_trace().
	 * 값 범위: 2의 거듭제곱이 권장. 0이면 SPDK 기본값 사용. */

	/** Opaque context for use of the env implementation. */
	void			*env_context;
	/* [한국어] env 구현체(DPDK 등)가 자체 사용하는 불투명 컨텍스트.
	 * 일반 사용자는 NULL로 두면 됨. 커스텀 env 백엔드를 만들 때 활용. */

	/**
	 * for passing user-provided log call
	 */
	spdk_log_cb		*log;
	/* [한국어] SPDK 내부 로그 호출을 사용자 함수로 가로채기 위한 콜백 포인터.
	 * 읽는 자: spdk_log 모듈이 등록 시 사용.
	 * NULL이면 기본 stderr/syslog 출력. */

	uint64_t		base_virtaddr;
	/* [한국어] DPDK가 hugepage 매핑을 시작할 기준 가상주소 (`--base-virtaddr`).
	 * ASLR과 충돌을 피하거나 디버깅 시 주소 고정에 사용.
	 * 0이면 EAL이 자동 결정. */

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
	 * 설정자: 사용자(반드시 sizeof(opts)로 설정해야 함, spdk_app_opts_init이 채움).
	 * 읽는 자: 라이브러리 내부 — 이 크기까지만 신뢰하고 이후는 기본값으로 채움.
	 * 의미: 라이브러리가 새 필드를 추가해도 구버전 헤더로 빌드된 앱이 그대로 동작. */

	/**
	 * Disable default signal handlers.
	 * If set to `true`, the shutdown process is not started implicitly by
	 * process signals, hence the application is responsible for calling
	 * spdk_app_start_shutdown().
	 *
	 * Default is `false`.
	 */
	bool disable_signal_handlers;
	/* [한국어] true이면 SPDK가 SIGINT/SIGTERM 핸들러를 등록하지 않음.
	 * 호스트 앱이 자체 시그널 처리를 하고 싶을 때 사용 (예: SPDK를 라이브러리로 임베드).
	 * 이 경우 사용자가 직접 spdk_app_start_shutdown()을 호출해야 종료가 시작된다. */

	bool interrupt_mode;
	/* [한국어] 폴링 대신 인터럽트 모드(eventfd/epoll)로 reactor를 동작시킬지.
	 * 값 범위: false=폴링(기본, CPU 100%), true=인터럽트(저전력, 고지연).
	 * 읽는 자: lib/thread, lib/event의 reactor 메인 루프. */

	bool enforce_numa;
	/* [한국어] true이면 NUMA 노드 미스매치(다른 노드의 메모리 사용) 시 에러.
	 * 값 범위: false=경고만, true=실패. 성능 회귀 방지용 안전장치. */

	/* Hole at byte 187. */
	uint8_t reserved187[1];
	/* [한국어] 1바이트 정렬 패딩. */

	/* Number of threads for SPDK tracing */
	uint32_t num_trace_threads;
	/* [한국어] tracing이 사용할 보조 스레드 수.
	 * 0이면 trace 기능이 별도 스레드를 띄우지 않음. */

	/**
	 * The allocated size for the message pool used by the threading library.
	 *
	 * Default is `SPDK_DEFAULT_MSG_MEMPOOL_SIZE`.
	 */
	size_t msg_mempool_size;
	/* [한국어] spdk_thread_send_msg가 사용하는 메시지 mempool 크기 (메시지 수).
	 * 설정자: 사용자.
	 * 읽는 자: spdk_thread_lib_init.
	 * 너무 작으면 cross-thread 메시지 폭주 시 send 실패 발생. */

	/*
	 *  If non-NULL, a string array of allowed RPC methods.
	 */
	const char **rpc_allowlist;
	/* [한국어] 허용할 RPC 메서드 이름의 NULL-종단 문자열 배열.
	 * NULL이면 모든 RPC 허용. 보안 강화를 위한 화이트리스트. */

	/**
	 * Used to pass vf_token to vfio_pci driver through DPDK.
	 * The vf_token is an UUID that shared between SR-IOV PF and VF.
	 */
	const char		*vf_token;
	/* [한국어] SR-IOV VF 사용 시 PF와 공유하는 UUID 토큰.
	 * 읽는 자: env_dpdk가 vfio-pci 드라이버에 전달. */

	/**
	 * Used to store lcore to CPU mappig to pass it to DPDK
	 */
	const char *lcore_map; /* lcore mapping */
	/* [한국어] DPDK lcore→물리 CPU 매핑 문자열 (`--lcores` 옵션).
	 * 예: "0@(0,1),1@2" — lcore 0을 CPU 0,1에 핀, lcore 1을 CPU 2에 핀.
	 * 읽는 자: env_dpdk EAL 인자 변환. */

	/**
	 * Log level for JSON RPC.
	 */
	enum spdk_log_level rpc_log_level;
	/* [한국어] JSON-RPC 호출/응답을 별도 로그 레벨로 기록.
	 * 값 범위: SPDK_LOG_DISABLED..SPDK_LOG_DEBUG. */

	/**
	 * If non-NULL, a pointer to JSON RPC log file.
	 */
	FILE *rpc_log_file;
	/* [한국어] RPC 로그를 별도 파일로 보낼 때의 FILE*.
	 * NULL이면 표준 SPDK 로그로만 출력. */

	/**
	 * Raw JSON configuration data and its size.
	 * Cannot be used simultaneously with json_config_file option.
	 */
	void *json_data;
	/* [한국어] 메모리에 들고 있는 JSON 설정 원본 포인터.
	 * json_config_file과 동시 사용 불가 (둘 중 하나만). */

	size_t json_data_size;
	/* [한국어] json_data가 가리키는 버퍼의 바이트 수. */

	/**
	 * If set, disable CPU claiming.
	 */
	bool disable_cpumask_locks;
	/* [한국어] true이면 CPU mask가 겹치는 다른 SPDK 프로세스 검출(파일락) 비활성화.
	 * 다중 프로세스가 의도적으로 같은 코어를 공유할 때만 사용. */
} __attribute__((packed));      /* [한국어] 정렬 hole이 들어가지 않도록 강제 → 명시적 reserved 배열로 ABI를 통제. */

SPDK_STATIC_ASSERT(sizeof(struct spdk_app_opts) == 253, "Incorrect size");
/* [한국어] 컴파일 타임 sizeof 검증 — 누군가 필드를 추가/이동하다 패딩이 바뀌면 빌드가 깨져 ABI 사고를 차단.
 * 새 필드 추가 시 이 상수 값도 함께 갱신해야 한다. */

/**
 * Initialize the default value of opts
 *
 * \param opts Data structure where SPDK will initialize the default options.
 * \param opts_size Must be set to sizeof(struct spdk_app_opts).
 */
/*
 * [한국어]
 * spdk_app_opts_init - spdk_app_opts에 SPDK 기본값을 채워넣는 초기화 함수.
 *
 * @opts: 사용자가 스택/힙에 잡은 spdk_app_opts (NULL 불가).
 * @opts_size: sizeof(struct spdk_app_opts). ABI 호환을 위해 caller 시점의 sizeof를 전달.
 * @return: void. 단, opts->opts_size 필드에 전달된 opts_size가 저장된다.
 *
 * 반드시 spdk_app_start 호출 전에 가장 먼저 호출되어야 한다. 이후 사용자는 필요한 필드만
 * 덮어쓰면 된다. 이 함수가 호출 직후라면 모든 reserved 영역도 0으로 초기화된다.
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
 * @start_fn:  부팅 완료 후 main lcore의 spdk_thread에서 실행될 사용자 진입점.
 *             시그니처는 `spdk_msg_fn` (== void(*)(void *ctx)).
 * @ctx:       start_fn에 그대로 전달될 사용자 컨텍스트 포인터.
 * @return:    0=정상 종료(spdk_app_stop(0)), 비-0=초기화 실패 또는 spdk_app_stop(rc).
 *
 * 동작 단계:
 *  1) opts 검증 → spdk_env_init() (DPDK EAL: hugepage, lcore pthread, mempool).
 *  2) spdk_thread_lib_init() — message pool 등 thread 인프라 셋업.
 *  3) reactor_mask가 가리키는 모든 lcore에 reactor 객체 생성·기동.
 *  4) RPC 서버 시작 (`rpc_addr` listen). delay_subsystem_init=true면 제한 RPC만.
 *  5) main lcore의 spdk_thread에 메시지로 start_fn(ctx) 실행 예약.
 *  6) 메인 시스템 스레드는 main reactor의 폴링 루프로 진입 → 블로킹.
 *  7) spdk_app_stop(rc) 수신 시 모든 reactor 종료 → 함수 반환.
 *
 * 실행 컨텍스트: 호출 스레드 = main DPDK lcore의 reactor 스레드가 됨.
 *               cross-thread 호출은 안전하지 않다 (단일 진입점).
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
 * spdk_thread_lib_fini, env 정리, RPC 잔여 cleanup 등을 수행. main()이 종료되기 직전에 호출.
 * spdk_app_start가 0이 아닌 값을 반환해도 정리를 위해 호출하는 것이 안전하다.
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
 * @return: void. 즉시 반환되며 실제 종료는 백그라운드에서 진행.
 *
 * 일반적으로는 SIGINT/SIGTERM 시그널 핸들러가 자동 호출하지만, SPDK를 라이브러리로
 * 임베드해서 시그널 핸들러를 사용하지 않거나(disable_signal_handlers=true) 사용자 정의
 * 종료 트리거가 필요한 경우 직접 호출한다.
 *
 * 동작: shutdown_cb 등록되어 있으면 호출, 아니면 spdk_subsystem_fini → spdk_app_stop(0).
 *
 * 호출 컨텍스트: 어느 스레드에서 호출해도 안전 (내부에서 spdk_thread_send_msg로 메인 스레드에 위임).
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
 * @rc: spdk_app_start의 반환값으로 전달될 종료 코드 (0=정상, 음수=에러).
 * @return: void. 모든 스레드가 종료되기를 기다리지 않는다.
 *
 * spdk_app_start_shutdown과 거의 같지만 종료 코드를 직접 지정할 수 있다는 점이 다르다.
 * 보통 사용자 start_fn 또는 그 자식 콜백이 작업 완료/에러 처리 후 호출.
 *
 * 호출 컨텍스트: SPDK thread 위에서 호출 권장. cross-thread 안전성은 내부적으로 보장됨.
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
 * @return: shm_id. -1이면 single-process, >=0이면 multi-process group ID.
 *
 * trace/RPC 등이 같은 그룹에 속한 secondary 프로세스를 식별할 때 사용.
 * 호출 컨텍스트 제약 없음(읽기 전용 전역 값).
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
 * 호출 컨텍스트: 어디서나 호출 가능 (순수 문자열 변환).
 */
int spdk_app_parse_core_mask(const char *mask, struct spdk_cpuset *cpumask);

/**
 * Get the mask of the CPU cores active for this application
 *
 * \return the bitmask of the active CPU cores.
 */
/*
 * [한국어]
 * spdk_app_get_core_mask - 현재 활성 reactor가 도는 코어들의 비트맵 반환.
 *
 * @return: spdk_cpuset 포인터 (라이브러리 소유 — 수정·해제 금지).
 *
 * 활성 코어 목록을 RPC/UI에 노출하거나 코어별 통계 수집 시 활용.
 * 호출 컨텍스트: spdk_app_start 이후에만 의미 있음.
 */
const struct spdk_cpuset *spdk_app_get_core_mask(void);

#define SPDK_APP_GETOPT_STRING "c:de:ghi:m:n:p:r:s:uvA:B:L:RW:"
/* [한국어] SPDK 표준 short option 집합 — getopt(3) 형식.
 * 의미: c=config, d=debug enable, e=tpoint, g=single seg, h=help, i=shm_id, m=core mask,
 *       n=mem channels, p=main core, r=rpc addr, s=mem size, u=no_pci, v=version,
 *       A=pci allow, B=pci block, L=log flag, R=hugepage unlink, W=disable warnings.
 * 사용자 앱이 자기 옵션을 추가할 때 이 문자열의 글자와 충돌해서는 안 된다. */

enum spdk_app_parse_args_rvals {
	SPDK_APP_PARSE_ARGS_HELP = 0,
	/* [한국어] 사용자가 `-h`를 입력해 도움말이 출력된 경우 — 보통 main()이 0을 반환하며 종료. */

	SPDK_APP_PARSE_ARGS_SUCCESS = 1,
	/* [한국어] 정상 파싱 — opts에 모든 값이 반영되었으며 spdk_app_start로 진행 가능. */

	SPDK_APP_PARSE_ARGS_FAIL = 2
	/* [한국어] 잘못된 옵션/사용자 콜백 실패 — main()은 비-0으로 종료해야 한다. */
};
typedef enum spdk_app_parse_args_rvals spdk_app_parse_args_rvals_t;
/* [한국어] enum의 typedef alias — typedef 형태로 함수 시그니처에 노출하기 위한 관례. */

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
 * spdk_app_parse_args - argc/argv를 받아 SPDK 표준 옵션을 opts에 채우고
 *                       앱 고유 옵션은 사용자 parse 콜백으로 위임하는 통합 헬퍼.
 *
 * @argc / @argv: main()이 받은 그대로 전달.
 * @opts:         spdk_app_opts_init 후 사용자가 일부 기본값을 덮어쓴 상태로 전달.
 *                파싱 결과(reactor_mask, rpc_addr 등)가 이 구조체에 누적 반영된다.
 * @getopt_str:   앱 고유 short option 추가 문자열(예: "f:b"). NULL이면 표준만.
 *                SPDK_APP_GETOPT_STRING의 글자와 겹치면 안 된다.
 * @app_long_opts: getopt_long용 long option 배열. NULL이면 표준 long opt만.
 * @parse:        getopt가 앱 고유 옵션을 만나면 호출할 콜백 (ch=옵션 글자, arg=optarg).
 * @usage:        `-h` 입력 시 앱 고유 옵션 부분의 도움말을 인쇄할 콜백.
 * @return: HELP / SUCCESS / FAIL — main()이 분기 처리.
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
 * spdk_app_usage - SPDK 공통 옵션의 도움말 텍스트를 stdout에 출력.
 *
 * @return: void.
 *
 * 일반적으로 사용자의 usage 콜백 안에서 호출된다 (앱 고유 옵션 위/아래에 배치).
 * spdk_app_parse_args 이전에 호출하면 일부 동적 디폴트 값이 채워지지 않아 정확하지 않을 수 있다.
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
 * spdk_event_allocate - 다른 lcore에서 실행할 함수+인자 묶음(spdk_event)을 mempool에서 할당.
 *
 * @lcore: 이벤트 실행 대상 lcore (반드시 활성 reactor가 있는 코어).
 * @fn:    그 lcore에서 호출될 콜백 (signature: spdk_event_fn).
 * @arg1, @arg2: 콜백에 그대로 전달될 인자 (소유권은 이벤트가 실행될 때까지 호출자에게 있음).
 * @return: 할당된 spdk_event* (실패 시 NULL — mempool 고갈).
 *
 * SPDK의 cross-core 메시지 패싱 프리미티브. 내부적으로 lockless ring 기반이라 락 없이
 * 다중 생산자가 동시에 호출 가능. spdk_event_call로 실제 enqueue 한 뒤 잊으면 됨
 * (이벤트 객체는 실행 후 자동 free).
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
 * spdk_event_call - 할당된 이벤트를 대상 lcore의 ring에 enqueue (실제 디스패치).
 *
 * @event: spdk_event_allocate가 반환한 포인터. 호출 후 소유권은 SPDK 이벤트 시스템에 이전.
 * @return: void. 실패 케이스가 사실상 없도록 ring을 사전 할당해 둠.
 *
 * 동작: 대상 lcore의 lockless ring에 이벤트 푸시 → 그 lcore의 reactor 폴링 루프가
 *       다음 iter에서 dequeue 후 fn(arg1, arg2) 실행 → mempool에 반환.
 *
 * 호출 컨텍스트: 어떤 스레드에서 호출해도 안전 (락 없는 SPSC/MPSC ring).
 *               단, allocate 후 call 전에 lcore가 죽으면 누수 가능 → 반드시 짝지어 호출.
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
 * @return: void.
 *
 * Polled-mode reactor는 원칙적으로 컨텍스트 스위치가 없어야 함 (CPU 100% 점유 의도).
 * 스위치가 발생한다면 시그널·페이지폴트·시스콜로 인한 성능 저하 신호다. ON 상태에선
 * 일정 주기로 /proc 읽어 스위치 카운터를 증가시키며, 비정상 스위치 시 경고 로그를 띄운다.
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
 * spdk_app_setup_trace - opts.tpoint_group_mask / opts.num_entries에 따라 trace 인프라 셋업.
 *
 * @opts:   spdk_app_opts_init 호출 후의 opts. tpoint_group_mask, num_entries, num_trace_threads,
 *          shm_id 등을 입력으로 사용.
 * @return: 0=성공, -1=공유메모리 매핑 실패 또는 잘못된 마스크.
 *
 * 동작: shm_open으로 trace 영역(코어당 num_entries 슬롯) 매핑 → 지정 그룹의 tpoint를 enable.
 * 일반적으로 spdk_app_start가 자동 호출하므로 사용자가 명시적으로 부를 일은 드물지만,
 * 임베디드 환경에서 spdk_env만 띄우고 trace는 따로 설정할 때 직접 호출 가능.
 *
 * 호출 컨텍스트: spdk_env_init 이후, reactor 시작 전에만 호출해야 안전.
 */
int spdk_app_setup_trace(struct spdk_app_opts *opts);

#ifdef __cplusplus              /* [한국어] C++ extern "C" 블록 종결. */
}
#endif

#endif                          /* [한국어] include guard 종결 (SPDK_EVENT_H). */
