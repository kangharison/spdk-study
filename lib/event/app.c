/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK 애플리케이션 부트스트랩/라이프사이클 핵심 구현 (app.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 기반 모든 애플리케이션(nvmf_tgt, iscsi_tgt, vhost, spdk_tgt 등)의
 * "부트스트랩 엔트리포인트"를 제공한다. 즉 main()에서 호출되는 spdk_app_opts_init,
 * spdk_app_parse_args, spdk_app_start, spdk_app_stop, spdk_app_fini 5종 API의
 * 실체가 모두 이 파일에 정의되어 있다.
 * 핵심 책무는 (1) 명령행/JSON 설정 파싱, (2) DPDK EAL 환경 초기화, (3) 로그/트레이스
 * 인프라 구성, (4) reactor 프레임워크(코어당 1개)의 기동, (5) subsystem JSON 설정
 * 적용 및 init/fini 시퀀스 호출, (6) RPC 서버(JSON-RPC over UNIX socket) 부팅,
 * (7) 시그널(SIGINT/SIGTERM)을 통한 graceful shutdown 처리, (8) CPU 코어 락 파일을
 * 통한 다중 SPDK 프로세스 간 코어 충돌 방지, (9) /proc/stat 기반 코어 사용률 통계
 * 수집의 시드값 등록을 단일 흐름으로 묶는 것이다.
 * 이 파일이 없으면 main()에서 직접 EAL/log/reactor/subsystem 각각을 손으로 초기화해야
 * 하며 SPDK의 표준 라이프사이클(STARTUP→RUNTIME→FINI 상태 전이)이 사라진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출자(상위): app/<target>/<target>.c 의 main() — 예) app/spdk_tgt/spdk_tgt.c.
 * 일반적 호출 순서:
 *   main()
 *     → spdk_app_opts_init(&opts)         // 기본값 채움
 *     → spdk_app_parse_args(argc,argv,&opts,...) // -m, -c, --json 등 파싱
 *     → spdk_app_start(&opts, app_started, NULL) // 블로킹: reactor 기동
 *           ↳ app_setup_env() → spdk_env_init() (DPDK eal_init)
 *           ↳ spdk_reactors_init() (lib/event/reactor.c)
 *           ↳ spdk_thread_create("app_thread", ...) (lib/thread)
 *           ↳ spdk_thread_send_msg(app_thread, bootstrap_fn)
 *                 ↳ spdk_subsystem_load_config() / spdk_subsystem_init() (lib/init)
 *                       ↳ 모든 subsystem(bdev, nvmf, vhost...)이 의존성 토폴로지 순으로 init
 *                 ↳ app_start_application() → start_fn(arg1)  // 사용자 콜백
 *           ↳ spdk_reactors_start() // 무한 폴링 루프, spdk_app_stop()까지 블록
 *     → (return) spdk_app_fini()         // trace/reactor/env/log cleanup, core unlock
 * 실행 컨텍스트: 호스트 유저스페이스의 메인 프로세스. spdk_app_start() 진입 후에는
 * "app_thread"라는 SPDK thread(=reactor 위에 배치된 논리 스레드)에서 subsystem init이
 * 진행되며, RPC 콜백 또한 동일 app_thread에서 실행된다. 시그널 핸들러는 시그널을
 * 받은 어떤 코어에서든 실행될 수 있어 spdk_thread_send_critical_msg로 app_thread에
 * 안전 전달한다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/event/reactor.c: spdk_reactors_init/start/stop/fini를 호출해 reactor framework의
 *   라이프사이클을 운영. 코어 i = reactor i = 무한 폴링 루프이며, 이 파일이 그 위에
 *   "app_thread"라는 첫 SPDK thread를 띄운다.
 * - lib/init/subsystem.c: spdk_subsystem_init/fini, spdk_subsystem_load_config(JSON 파싱)
 *   를 호출해 STARTUP 단계 RPC들과 RUNTIME 단계 RPC들을 순차 실행하여 모듈을 부팅.
 * - lib/log/log.c: spdk_log_set_level, spdk_log_open/close — 콘솔/syslog 출력 채널 결정.
 * - lib/trace/trace.c: spdk_trace_init — /dev/shm 기반 트레이스 버퍼를 매핑.
 * - lib/env_dpdk/: spdk_env_init이 DPDK EAL을 띄움(hugepage, --base-virtaddr, mem-channels 등).
 *   spdk_env_get_core_count()로 활성 lcore 개수를 확인.
 * - lib/rpc/: spdk_rpc_initialize/server_pause/server_resume — UNIX socket 기반 JSON-RPC 서버.
 *   STARTUP/RUNTIME 두 상태에 따라 허용 RPC 셋이 달라짐.
 * - lib/thread/: spdk_thread_create("app_thread"), spdk_thread_send_msg/critical_msg —
 *   다른 스레드에서 안전하게 app_thread로 작업을 전달하기 위한 lockless 메시지 큐.
 * - 공유 자료구조: 본 파일의 g_spdk_app(전역 1개) — 다른 곳에서 직접 접근하진 않지만
 *   spdk_app_get_shm_id()/spdk_app_start_shutdown() 같은 함수로 노출되며,
 *   spdk_app_opts(공개 헤더 include/spdk/event.h)는 main()이 채워 spdk_app_start로 전달.
 * 데이터 흐름: argv → spdk_app_opts → g_spdk_app 일부 + DPDK env_opts → reactor/log/trace
 * → subsystem JSON config → 모듈별 RPC 콜백 호출 → 사용자 start_fn 진입.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_app_opts_init(opts, opts_size): opts 구조체에 안전한 디폴트값을 채움. ABI 호환성을
 *   위해 opts_size 기반 SET_FIELD 매크로로 필드별 존재 여부를 검사.
 * - spdk_app_parse_args(argc,argv,opts,...): getopt_long으로 -m/-c/--json/-r 등 표준 SPDK
 *   플래그를 파싱하여 opts에 반영. 앱 고유 short option/long option도 합쳐 받음.
 * - spdk_app_start(opts, fn, arg): 부트스트랩 메인. env→reactors→app_thread→subsystem init
 *   →start_fn 순으로 부팅하고 spdk_reactors_start()로 무한 루프 진입. 종료 시 g_spdk_app.rc 반환.
 * - spdk_app_stop(rc): 외부에서 호출되어 app_thread에 stop 메시지를 전송, subsystem_fini→
 *   reactors_stop을 트리거.
 * - spdk_app_fini(): spdk_app_start return 후 main()이 호출, trace/reactor/env/log cleanup +
 *   /var/tmp/spdk_cpu_lock_* 파일 unlink.
 * - struct spdk_app(파일 내부): 부팅 중·후 필요한 전역 상태(JSON 데이터 포인터, RPC 주소,
 *   shm_id, shutdown_cb, 종료 코드 등)를 보관. 모든 코어에서 읽히지만 수정은 app_thread에
 *   국한되도록 디자인.
 * - struct g_initial_stat[CORE]: /proc/stat 기반 부팅 시점의 user/sys/irq tick 시드. 이후
 *   spdk_app_get_proc_stat()이 현재값과 차이를 반환해 코어 사용률을 계산.
 * - g_core_locks[CORE]: /var/tmp/spdk_cpu_lock_NNN 파일 디스크립터. 다중 SPDK 프로세스가
 *   같은 코어를 점유하지 못하도록 fcntl(F_SETLK) 기반 락을 보유.
 */

#include "spdk/stdinc.h"   /* [한국어] SPDK가 표준 C 헤더를 단일 파일로 묶은 entry. fopen/strdup/getopt 등 표준 라이브러리 의존성을 한 번에 포함시켜 OS별 헤더 분기를 추상화. */
#include "spdk/version.h"  /* [한국어] SPDK_VERSION_STRING 매크로 — --version 옵션 처리에서 출력. 빌드 시 git tag 기반으로 정의됨. */

#include "spdk_internal/event.h"  /* [한국어] event/reactor 서브시스템의 internal API. spdk_reactors_init/start/stop/fini 등 외부에 노출하지 않는 부트스트랩 헬퍼 선언이 들어 있다. */

#include "spdk/assert.h"      /* [한국어] SPDK_STATIC_ASSERT — spdk_app_opts ABI 크기 검증(컴파일 타임)에 사용. */
#include "spdk/env.h"         /* [한국어] DPDK EAL 추상화. spdk_env_init/opts, spdk_env_get_core_count, SPDK_ENV_FOREACH_CORE 등을 통해 hugepage/PCI/lcore 토폴로지에 접근. */
#include "spdk/init.h"        /* [한국어] subsystem init/fini API (spdk_subsystem_init, spdk_subsystem_load_config). lib/init/subsystem.c가 구현 본체. */
#include "spdk/log.h"         /* [한국어] SPDK_ERRLOG/NOTICELOG/WARNLOG 매크로와 spdk_log_set_level/print_level/open/close. 출력 단계와 syslog 연계 제어. */
#include "spdk/thread.h"      /* [한국어] SPDK thread(논리 스레드, reactor 위에 올라감), spdk_thread_send_msg/critical_msg, spdk_thread_get_app_thread API. */
#include "spdk/trace.h"       /* [한국어] /dev/shm 기반 트레이스 버퍼 초기화·tpoint mask 제어 — spdk_trace_init, spdk_trace_set_tpoints 등. */
#include "spdk/string.h"      /* [한국어] spdk_strerror, spdk_strtol, spdk_strsepq, spdk_sprintf_alloc, spdk_strarray_from_string 등 SPDK 표준 문자열 유틸. */
#include "spdk/scheduler.h"   /* [한국어] g_scheduling_in_progress 등 스케줄러 상태. 종료 시 스케줄링 중이면 fini를 지연시키기 위해 참조. */
#include "spdk/rpc.h"         /* [한국어] JSON-RPC 서버 API: spdk_rpc_initialize/server_pause/server_resume, spdk_rpc_set_state, SPDK_RPC_REGISTER 매크로. */
#include "spdk/util.h"        /* [한국어] SPDK_COUNTOF, spdk_max, spdk_u64_is_pow2 등 일반 매크로/인라인 유틸. */
#include "spdk/file.h"        /* [한국어] spdk_posix_file_load_from_name — JSON config 파일을 한 번에 메모리로 적재. */
#include "spdk/config.h"      /* [한국어] 빌드 옵션 매크로(SPDK_CONFIG_MAX_LCORES 등) — configure 결과로 자동 생성된 헤더. */
#include "event_internal.h"   /* [한국어] event 서브시스템 디렉토리 내부 공용 정의(app_get_proc_stat 등 같은 디렉토리 모듈 간 약속). */

/* [한국어] 내부 syslog 기록 임계 — NOTICE 이상은 syslog로 보관. 기본값을 INFO보다 낮춰 지나치게 시끄러운 디버그 라인 방지. */
#define SPDK_APP_DEFAULT_LOG_LEVEL		SPDK_LOG_NOTICE
/* [한국어] 콘솔(stderr) 인쇄 임계 — INFO 이상이 사용자에게 보임. --silence-noticelog 옵션으로 WARN까지 올릴 수 있음. */
#define SPDK_APP_DEFAULT_LOG_PRINT_LEVEL	SPDK_LOG_INFO
/* [한국어] 코어당 트레이스 엔트리 기본 개수. 0이면 트레이스 비활성화. 2의 거듭제곱이어야 하며 lib/trace 가 검증. */
#define SPDK_APP_DEFAULT_NUM_TRACE_ENTRIES	SPDK_DEFAULT_NUM_TRACE_ENTRIES

/* [한국어] DPDK -m(--mem-size) 기본값. -1은 "DPDK 자동 결정(=가용 hugepage 전체)"을 의미. */
#define SPDK_APP_DPDK_DEFAULT_MEM_SIZE		-1
/* [한국어] DPDK --main-core 기본값. -1은 "지정 안 함 → 첫 lcore가 main"을 의미. */
#define SPDK_APP_DPDK_DEFAULT_MAIN_CORE		-1
/* [한국어] DPDK -n(--mem-channels) 기본값. -1은 "DPDK가 NUMA/메모리 컨트롤러 자동 검출"을 의미. */
#define SPDK_APP_DPDK_DEFAULT_MEM_CHANNEL	-1
/* [한국어] -m/--cpumask 기본값: 0x1 = 코어 0 한 개만 사용. 단일 코어 부팅이 SPDK 디폴트 안전값. */
#define SPDK_APP_DPDK_DEFAULT_CORE_MASK		"0x1"
/* [한국어] DPDK --base-virtaddr 기본값(0x200000000000 = 32TiB 영역). hugepage가 매핑될 가상 주소 시작점.
 *         값이 너무 작으면 ASLR과 충돌할 수 있어 충분히 큰 영역을 디폴트로 둔다. */
#define SPDK_APP_DPDK_DEFAULT_BASE_VIRTADDR	0x200000000000
/* [한국어] enable_coredump=true일 때 RLIMIT_CORE로 설정할 코어 덤프 최대 크기(0x1_4000_0000 = 5 GiB).
 *         과거 RLIM_INFINITY를 썼으나 의도치 않은 거대 코어덤프로 디스크 가득참 사례가 있어 5GiB 캡. */
#define SPDK_APP_DEFAULT_CORE_LIMIT		0x140000000 /* 5 GiB */

/* For core counts <= 63, the message memory pool size is set to
 * SPDK_DEFAULT_MSG_MEMPOOL_SIZE.
 * For core counts > 63, the message memory pool size is depended on
 * number of cores. Per core, it is calculated as SPDK_MSG_MEMPOOL_CACHE_SIZE
 * multiplied by factor of 4 to have space for multiple spdk threads running
 * on single core (e.g  iscsi + nvmf + vhost ). */
/* [한국어] 코어당 메시지 풀 슬롯 추정치. SPDK thread 간 send_msg 메시지 객체가 담길 mempool 크기 산정에 사용.
 *         계수 4는 한 코어에 iscsi/nvmf/vhost 등 여러 SPDK thread가 공존할 수 있다는 가정 — 각 thread 별
 *         캐시 라인 분량(SPDK_MSG_MEMPOOL_CACHE_SIZE) × 4 만큼 확보해야 메시지 부족(EAGAIN)을 막는다. */
#define SPDK_APP_PER_CORE_MSG_MEMPOOL_SIZE	(4 * SPDK_MSG_MEMPOOL_CACHE_SIZE)

/*
 * [한국어]
 * struct spdk_app — 애플리케이션 부팅 ~ 종료 동안 살아 있는 전역 라이프사이클 상태.
 * 한 프로세스에 정확히 하나만 존재하며 g_spdk_app 전역으로 보관된다. spdk_app_start
 * 진입 시 0-초기화 후 opts 값으로 채워지고, 종료 시 spdk_app_stop이 stopped=true로
 * 마킹한 뒤 subsystem_fini → reactors_stop 시퀀스를 트리거한다.
 * 모든 코어/스레드에서 읽힐 수 있지만, 쓰기는 거의 모두 app_thread(메인 SPDK thread)
 * 한 곳에서만 일어나도록 디자인되어 별도 락이 없다(app_thread serializes).
 */
struct spdk_app {
	void				*json_data;
	/* [한국어] JSON config 파일을 통째로 메모리에 적재한 버퍼 포인터.
	 * 설정자: spdk_app_start()에서 spdk_posix_file_load_from_name()으로 로드, 또는 opts->json_data를
	 *          calloc + memcpy로 복사. NULL이면 JSON 설정 없이 RPC만으로 동작.
	 * 읽는 자: bootstrap_fn(), app_subsystem_init_done()에서 spdk_subsystem_load_config()에 전달.
	 * 값 범위: NULL 또는 malloc된 바이트 버퍼. 사용 후 free()되어 NULL로 리셋됨.
	 * 동기화: 최초 설정은 spdk_app_start 호출 스레드에서, 이후 읽기는 app_thread에서만 수행. */

	size_t				json_data_size;
	/* [한국어] json_data 버퍼의 길이(바이트).
	 * 설정자: spdk_app_start. 읽는 자: spdk_subsystem_load_config 호출 시 인자.
	 * 값 범위: 0(설정 없음) ~ 파일 크기. 동기화: json_data와 동일하게 app_thread 단독 접근. */

	bool				json_config_ignore_errors;
	/* [한국어] JSON config 적용 중 RPC 실패 시 무시하고 진행할지 여부.
	 * 설정자: spdk_app_start가 opts->json_config_ignore_errors를 그대로 복사.
	 * 읽는 자: spdk_subsystem_load_config의 stop_on_error 인자(부정 형태)로 전달.
	 * 값 범위: false=한 항목이라도 실패 시 전체 중단(보수적, 기본값), true=경고만 남기고 계속.
	 * 동기화: 부팅 직후 1회 설정, 이후 읽기 전용. */

	bool				stopped;
	/* [한국어] spdk_app_stop이 이미 실행되었는지 표시하는 멱등성 플래그.
	 * 설정자: app_stop()에서 true로 마킹. 초기값 false는 spdk_app_start에서 memset으로 셋.
	 * 읽는 자: app_stop() 자기 자신 — 두 번째 호출 시 즉시 return하는 가드.
	 * 값 범위: false→true 단방향 전이. 동기화: app_thread에서만 갱신되어 토어 없음. */

	const char			*rpc_addr;
	/* [한국어] JSON-RPC 서버가 listen할 UNIX socket 경로(예: /var/tmp/spdk.sock) 또는 NULL.
	 * 설정자: spdk_app_start가 opts->rpc_addr를 복사. NULL이면 --no-rpc-server와 동등.
	 * 읽는 자: app_do_spdk_subsystem_init, rpc_framework_start_init_cpl 등 — RPC 시작/일시중지에 사용.
	 * 값 범위: 정적 문자열 또는 사용자 제공 포인터. 동기화: 부팅 후 read-only. */

	const char			**rpc_allowlist;
	/* [한국어] 허용되는 RPC 메서드 이름 화이트리스트(NULL 종료). NULL이면 모든 RPC 허용.
	 * 설정자: spdk_app_start가 opts->rpc_allowlist를 복사. --rpcs-allowed 파싱 결과.
	 * 읽는 자: app_subsystem_init_done이 spdk_rpc_set_allowlist에 전달.
	 * 값 범위: NULL 또는 spdk_strarray로 할당된 문자열 배열.
	 * 동기화: 부팅 후 read-only — 이후 자유롭게 다중 코어에서 읽힘. */

	FILE				*rpc_log_file;
	/* [한국어] RPC 호출 로그를 기록할 FILE* (NULL이면 로그 안 남김).
	 * 설정자: opts->rpc_log_file 복사. 읽는 자: app_do_spdk_subsystem_init에서 spdk_rpc_initialize에 전달.
	 * 값 범위: NULL 또는 fopen된 FILE*. 동기화: rpc 모듈 내부에서 사용 — 본 파일은 forward만. */

	enum spdk_log_level		rpc_log_level;
	/* [한국어] rpc_log_file 출력의 최소 로그 레벨(DEBUG~ERROR).
	 * 설정자: opts->rpc_log_level. 읽는 자: spdk_rpc_initialize 호출 시 opts.log_level로 전달.
	 * 값 범위: SPDK_LOG_DISABLED, SPDK_LOG_DEBUG, ..., SPDK_LOG_ERROR.
	 * 동기화: 부팅 후 read-only. */

	int				shm_id;
	/* [한국어] /dev/shm 트레이스 버퍼/DPDK shared config의 식별자(다중 SPDK 인스턴스 구분용).
	 * 설정자: opts->shm_id 복사 (-i 또는 --shm-id 플래그). 음수=비활성/사용 안 함.
	 * 읽는 자: spdk_app_get_shm_id() 공개 API, spdk_app_setup_trace에서 SHM 이름 합성.
	 * 값 범위: -1(미지정, pid 사용) 또는 0 이상 정수.
	 * 동기화: 부팅 후 read-only. 외부 도구(spdk_trace)가 이 값으로 동일 SHM에 접근. */

	spdk_app_shutdown_cb		shutdown_cb;
	/* [한국어] SIGINT/SIGTERM 수신 또는 spdk_app_start_shutdown 호출 시 가장 먼저 호출되는 사용자 콜백.
	 * 설정자: opts->shutdown_cb. 읽는 자: app_start_shutdown — 호출 후 NULL로 클리어해 멱등성 확보.
	 * 값 범위: NULL이면 곧장 spdk_app_stop(0). non-NULL이면 콜백이 graceful 시퀀스를 직접 책임짐.
	 * 동기화: app_thread의 critical msg 큐로 전달돼 app_thread에서만 호출됨. */

	int				rc;
	/* [한국어] spdk_app_start가 main()에 반환할 종료 코드.
	 * 설정자: app_stop()이 arg1(=stop 호출자가 넘긴 rc)으로 갱신. 단, 이미 0 아닌 값이면 덮어쓰지 않음.
	 * 읽는 자: spdk_app_start의 마지막 return 문.
	 * 값 범위: 0=정상, 그 외=오류. 동기화: app_thread에서만 갱신됨. */
};

/* [한국어] 프로세스 전역 단일 spdk_app 인스턴스. spdk_app_start에서 0-clear 후 opts로 채워지고,
 *         spdk_app_fini까지 살아 있으며 모든 코어에서 read-only로 참조됨. */
static struct spdk_app g_spdk_app;
/* [한국어] subsystem init 완료 후 호출할 사용자 진입 콜백(spdk_app_start의 두 번째 인자).
 *         설정자: spdk_app_start. 읽는 자: app_start_application — app_thread에서 g_start_fn(g_start_arg) 호출. */
static spdk_msg_fn g_start_fn = NULL;
/* [한국어] g_start_fn에 전달될 사용자 컨텍스트(spdk_app_start의 세 번째 인자). 설정/읽는 자는 위와 동일. */
static void *g_start_arg = NULL;
/* [한국어] --wait-for-rpc 사용 시 true. 부팅 직후 subsystem_init을 보류하고 RPC만 띄워 사용자가
 *         framework_start_init RPC를 명시적으로 보낼 때까지 대기하도록 한다. */
static bool g_delay_subsystem_init = false;
/* [한국어] SIGINT/SIGTERM이 한 번 수신되었음을 표시. 두 번째 시그널은 무시(중복 graceful shutdown 방지).
 *         시그널 핸들러 컨텍스트에서 쓰이므로 단순 bool로 두어도 race 영향 적지만, 이중 처리만 막으면 됨. */
static bool g_shutdown_sig_received = false;
/* [한국어] argv[0] 보관. usage()에서 "<exe> [options]"로 출력하기 위해 spdk_app_parse_args에서 캐시. */
static char *g_executable_name;
/* [한국어] usage()에서 디폴트 메모리 크기를 출력할 때 참조하기 위한 opts 백업. spdk_app_parse_args 시작 시 memcpy. */
static struct spdk_app_opts g_default_opts;
/* [한국어] /var/tmp/spdk_cpu_lock_NNN 파일을 잡고 있는 fd 배열(코어 인덱스). -1=비어 있음, 그 외=잠금 보유.
 *         claim_cpu_cores/unclaim_cpu_cores가 갱신. 다중 SPDK 프로세스가 같은 코어를 점유하지 못하도록 fcntl(F_SETLK). */
static int g_core_locks[SPDK_CONFIG_MAX_LCORES];

/* [한국어] 부팅 직후 /proc/stat에서 읽은 코어별 user/sys/irq jiffies 시드.
 *         이후 app_get_proc_stat이 (현재값 - 시드)로 SPDK 실행 동안의 상대 사용률을 산출.
 *         배열 인덱스 = lcore id (0 ~ SPDK_CONFIG_MAX_LCORES-1). */
static struct {
	uint64_t irq;
	/* [한국어] hardirq+softirq 누적 카운트(시드값). parse_proc_stat에서 합산. */
	uint64_t usr;
	/* [한국어] user(+nice 제외) jiffies 시드. */
	uint64_t sys;
	/* [한국어] system jiffies 시드. */
} g_initial_stat[SPDK_CONFIG_MAX_LCORES];

/*
 * [한국어]
 * spdk_app_get_shm_id - 현재 SPDK 인스턴스에 부여된 SHM 식별자 반환.
 *
 * @return: -1=미지정(pid 기반 SHM), 0 이상=명시적 -i/--shm-id로 지정된 값.
 *
 * 외부 도구(spdk_trace, spdk_top 등)가 어떤 /dev/shm 트레이스 버퍼·DPDK 공유 설정을 열어야 할지
 * 결정할 때 RPC 또는 직접 라이브러리 호출로 이 값을 조회한다. 단순한 게터이며 어떤 스레드에서도
 * 호출 가능(g_spdk_app.shm_id는 부팅 후 read-only).
 *
 * 호출 체인:
 *   spdk_top/RPC handler → [spdk_app_get_shm_id]
 */
int
spdk_app_get_shm_id(void)
{
	return g_spdk_app.shm_id;  /* [한국어] 단순 read — 부팅 시 spdk_app_start가 셋한 값을 그대로 반환. */
}

/* append one empty option to indicate the end of the array */
/*
 * [한국어]
 * g_cmdline_options - getopt_long()이 인식할 SPDK 표준 long option 테이블.
 * 각 항목 위 #define으로 부여된 OPT_IDX(=val 필드, char 또는 256+ 정수)로 switch-case 분기됨.
 * 256 미만은 short option(예: 'c'='--config')과 1:1 매칭, 256 이상은 long-only 옵션.
 * 마지막 빈 엔트리({0,0,0,0})는 spdk_app_parse_args에서 calloc 후 memcpy로 추가됨에 주의.
 */
static const struct option g_cmdline_options[] = {
#define CONFIG_FILE_OPT_IDX	'c'  /* [한국어] -c/--config: legacy/JSON config 파일 경로(JSON_CONFIG_OPT_IDX와 같은 처리). */
	{"config",			required_argument,	NULL, CONFIG_FILE_OPT_IDX},
#define LIMIT_COREDUMP_OPT_IDX 'd'  /* [한국어] -d/--limit-coredump: enable_coredump=false, 즉 RLIMIT_CORE 변경하지 않음. */
	{"limit-coredump",		no_argument,		NULL, LIMIT_COREDUMP_OPT_IDX},
#define TPOINT_GROUP_OPT_IDX 'e'  /* [한국어] -e/--tpoint-group: 트레이스 포인트 그룹 마스크 문자열(예: "nvmf:0x1,bdev:0xF"). */
	{"tpoint-group",		required_argument,	NULL, TPOINT_GROUP_OPT_IDX},
#define SINGLE_FILE_SEGMENTS_OPT_IDX 'g'  /* [한국어] -g/--single-file-segments: DPDK가 hugepage 매핑을 단일 파일에 모음(공간 효율). */
	{"single-file-segments",	no_argument,		NULL, SINGLE_FILE_SEGMENTS_OPT_IDX},
#define HELP_OPT_IDX		'h'  /* [한국어] -h/--help: usage() 출력 후 SPDK_APP_PARSE_ARGS_HELP 반환 — 호출자는 main()을 정상 종료. */
	{"help",			no_argument,		NULL, HELP_OPT_IDX},
#define SHM_ID_OPT_IDX		'i'  /* [한국어] -i/--shm-id: 다중 SPDK 인스턴스 식별용 SHM ID. trace 파일 이름·DPDK config에 반영. */
	{"shm-id",			required_argument,	NULL, SHM_ID_OPT_IDX},
#define CPUMASK_OPT_IDX		'm'  /* [한국어] -m/--cpumask: hex 마스크(0xF) 또는 [0,1,10] 리스트 형식의 사용 코어 지정. lcore_map과 상호 배타. */
	{"cpumask",			required_argument,	NULL, CPUMASK_OPT_IDX},
#define MEM_CHANNELS_OPT_IDX	'n'  /* [한국어] -n/--mem-channels: DPDK가 사용할 NUMA/메모리 채널 수. -1이면 자동. */
	{"mem-channels",		required_argument,	NULL, MEM_CHANNELS_OPT_IDX},
#define MAIN_CORE_OPT_IDX	'p'  /* [한국어] -p/--main-core: DPDK main lcore 번호. SPDK는 이 코어를 app_thread 배치 후보로 사용. */
	{"main-core",			required_argument,	NULL, MAIN_CORE_OPT_IDX},
#define RPC_SOCKET_OPT_IDX	'r'  /* [한국어] -r/--rpc-socket: JSON-RPC 서버 listen 경로(UNIX socket). 기본 /var/tmp/spdk.sock. */
	{"rpc-socket",			required_argument,	NULL, RPC_SOCKET_OPT_IDX},
#define MEM_SIZE_OPT_IDX	's'  /* [한국어] -s/--mem-size: DPDK가 미리 잡을 hugepage 메모리(MB). -1이면 가용 hugepage 전체. */
	{"mem-size",			required_argument,	NULL, MEM_SIZE_OPT_IDX},
#define NO_PCI_OPT_IDX		'u'  /* [한국어] -u/--no-pci: DPDK PCI probe 비활성화(메모리/소켓 모드 등 PCI 디바이스 불요 시). */
	{"no-pci",			no_argument,		NULL, NO_PCI_OPT_IDX},
#define VERSION_OPT_IDX		'v'  /* [한국어] -v/--version: SPDK_VERSION_STRING 출력 후 HELP와 동일하게 정상 종료. */
	{"version",			no_argument,		NULL, VERSION_OPT_IDX},
#define PCI_BLOCKED_OPT_IDX	'B'  /* [한국어] -B/--pci-blocked: 차단할 PCI BDF(BUS:DEV.FUNC). 여러 번 사용 가능. -A와 상호 배타. */
	{"pci-blocked",			required_argument,	NULL, PCI_BLOCKED_OPT_IDX},
#define LOGFLAG_OPT_IDX		'L'  /* [한국어] -L/--logflag: 디버그용 로그 플래그 활성화(예: -L bdev). DEBUG 빌드에선 print_level까지 DEBUG로. */
	{"logflag",			required_argument,	NULL, LOGFLAG_OPT_IDX},
#define HUGE_UNLINK_OPT_IDX	'R'  /* [한국어] -R/--huge-unlink: hugepage backing 파일을 init 직후 unlink — 비정상 종료 시 잔여 파일 방지. */
	{"huge-unlink",			no_argument,		NULL, HUGE_UNLINK_OPT_IDX},
#define PCI_ALLOWED_OPT_IDX	'A'  /* [한국어] -A/--pci-allowed: 허용할 PCI BDF(allowlist). 여러 번 사용 가능. -B와 상호 배타. */
	{"pci-allowed",			required_argument,	NULL, PCI_ALLOWED_OPT_IDX},
#define INTERRUPT_MODE_OPT_IDX 256  /* [한국어] --interrupt-mode: 모든 poller가 인터럽트 모드 지원 시 CPU 사용률 절감(폴링→이벤트). */
	{"interrupt-mode",		no_argument,		NULL, INTERRUPT_MODE_OPT_IDX},
#define SILENCE_NOTICELOG_OPT_IDX 257  /* [한국어] --silence-noticelog: print_level을 WARN으로 올려 NOTICE 라인을 stderr에서 숨김. */
	{"silence-noticelog",		no_argument,		NULL, SILENCE_NOTICELOG_OPT_IDX},
#define WAIT_FOR_RPC_OPT_IDX	258  /* [한국어] --wait-for-rpc: subsystem init을 보류, framework_start_init RPC 수신 시까지 대기. */
	{"wait-for-rpc",		no_argument,		NULL, WAIT_FOR_RPC_OPT_IDX},
#define HUGE_DIR_OPT_IDX	259  /* [한국어] --huge-dir: hugetlbfs mount 경로 명시(여러 mount 환경에서 특정 mount만 사용). */
	{"huge-dir",			required_argument,	NULL, HUGE_DIR_OPT_IDX},
#define NUM_TRACE_ENTRIES_OPT_IDX	260  /* [한국어] --num-trace-entries: 코어당 트레이스 엔트리 수(2의 거듭제곱). 0이면 트레이스 비활성화. */
	{"num-trace-entries",		required_argument,	NULL, NUM_TRACE_ENTRIES_OPT_IDX},
#define JSON_CONFIG_OPT_IDX		262  /* [한국어] --json: JSON config 파일 경로. -c/--config의 alias. */
	{"json",			required_argument,	NULL, JSON_CONFIG_OPT_IDX},
#define JSON_CONFIG_IGNORE_INIT_ERRORS_IDX	263  /* [한국어] --json-ignore-init-errors: subsystem init 실패해도 다음으로 진행(디버그용). */
	{"json-ignore-init-errors",	no_argument,		NULL, JSON_CONFIG_IGNORE_INIT_ERRORS_IDX},
#define IOVA_MODE_OPT_IDX	264  /* [한국어] --iova-mode pa|va: DPDK IOVA 매핑 모드(physical addr vs virtual addr). VFIO 환경 등에서 결정. */
	{"iova-mode",			required_argument,	NULL, IOVA_MODE_OPT_IDX},
#define BASE_VIRTADDR_OPT_IDX	265  /* [한국어] --base-virtaddr: DPDK가 hugepage를 매핑할 가상주소 시작점. ASLR 충돌 회피용 큰 값 권장. */
	{"base-virtaddr",		required_argument,	NULL, BASE_VIRTADDR_OPT_IDX},
#define ENV_CONTEXT_OPT_IDX	266  /* [한국어] --env-context: env 구현(환경)에 그대로 전달되는 불투명 문자열(예: DPDK 추가 EAL 인자 패스스루). */
	{"env-context",			required_argument,	NULL, ENV_CONTEXT_OPT_IDX},
#define DISABLE_CPUMASK_LOCKS_OPT_IDX	267  /* [한국어] --disable-cpumask-locks: /var/tmp/spdk_cpu_lock_NNN 락 파일 사용 안 함(테스트/컨테이너용). */
	{"disable-cpumask-locks",	no_argument,		NULL, DISABLE_CPUMASK_LOCKS_OPT_IDX},
#define RPCS_ALLOWED_OPT_IDX	268  /* [한국어] --rpcs-allowed: 콤마로 구분된 RPC 메서드 화이트리스트(보안 강화). */
	{"rpcs-allowed",		required_argument,	NULL, RPCS_ALLOWED_OPT_IDX},
#define ENV_VF_TOKEN_OPT_IDX 269  /* [한국어] --vfio-vf-token: SR-IOV PF/VF가 공유하는 VFIO VF 토큰(UUID). vfio-pci 드라이버에서 사용. */
	{"vfio-vf-token",		required_argument,	NULL, ENV_VF_TOKEN_OPT_IDX},
#define MSG_MEMPOOL_SIZE_OPT_IDX 270  /* [한국어] --msg-mempool-size: 전역 메시지 풀 슬롯 수 명시. 미지정 시 코어 수 기반 자동 산정. */
	{"msg-mempool-size",		required_argument,	NULL, MSG_MEMPOOL_SIZE_OPT_IDX},
#define LCORES_OPT_IDX	271  /* [한국어] --lcores: DPDK lcore→CPU 매핑 형식 ("(5-7)@(10-12)" 등). cpumask와 상호 배타. */
	{"lcores",			required_argument,	NULL, LCORES_OPT_IDX},
#define NO_HUGE_OPT_IDX	272  /* [한국어] --no-huge: hugepage 미사용 모드(개발/테스트용. 성능 저하). */
	{"no-huge",			no_argument,		NULL, NO_HUGE_OPT_IDX},
#define NO_RPC_SERVER_OPT_IDX	273  /* [한국어] --no-rpc-server: rpc_addr=NULL로 만들어 RPC 서버 자체를 띄우지 않음. */
	{"no-rpc-server",		no_argument,		NULL, NO_RPC_SERVER_OPT_IDX},
#define ENFORCE_NUMA_OPT_IDX 274  /* [한국어] --enforce-numa: 지정된 NUMA 노드에서만 메모리 할당 허용(NUMA 정합성 강제). */
	{"enforce-numa",		no_argument,		NULL, ENFORCE_NUMA_OPT_IDX},
};

#if defined(__FreeBSD__)
/*
 * [한국어]
 * parse_proc_stat (FreeBSD 분기) - sysctl(kern.cp_times)로 코어별 CPU 시간 통계 추출.
 *
 * @core: 조회할 lcore 번호.
 * @user: out — user 모드 jiffies (FreeBSD CPUSTATES 인덱스 0).
 * @sys:  out — system 모드 jiffies (인덱스 2, kernel).
 * @irq:  out — interrupt 모드 jiffies (인덱스 3).
 * @return: 0=성공, -1=실패(sysctl 오류, 코어 범위 초과, malloc 실패).
 *
 * FreeBSD에는 /proc/stat이 기본 마운트되지 않으므로 sysctl로 동일 정보를 수집.
 * kern.cp_times는 모든 CPU의 CPUSTATES(=5)개 카운터를 하나의 long 배열에 직렬화해 반환.
 * 호출 컨텍스트: 부팅 시 init_proc_stat(시드 적재) 또는 RPC/이벤트 컨텍스트에서 app_get_proc_stat
 * (현재 통계 조회) — 모두 짧은 시간 내 반환되므로 reactor 폴링에 큰 영향 없음.
 *
 * 호출 체인:
 *   init_proc_stat / app_get_proc_stat → [parse_proc_stat] → libc(sysctlbyname) → kernel
 */
static int
parse_proc_stat(unsigned int core, uint64_t *user, uint64_t *sys, uint64_t *irq)
{
	size_t len, ncpu = 0;  /* [한국어] len: sysctl out 길이, ncpu: hw.ncpu(전체 코어 수). */
	long *cp_times;        /* [한국어] kern.cp_times의 직렬화된 long 배열을 받을 버퍼. */

	len = sizeof(ncpu);    /* [한국어] sysctl 첫 호출 — ncpu(uint) 한 개를 받기 위한 길이 지정. */
	if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) < 0) {  /* [한국어] FreeBSD 커널에 가용 CPU 수 질의. */
		return -1;     /* [한국어] sysctl 실패 — 시스템 정보 접근 불가. */
	}
	if (core >= ncpu) {    /* [한국어] 호출자가 비정상적으로 큰 코어 번호를 넘긴 경우 방어. */
		return -1;
	}
	/*
	 * kern.cp_times returns 5 (CPUSTATES) values per cpu in a single line. E.g. for 4 cpus
	 *   kern.cp_times: 37924 0 2593 961 9270910 19400 0 1341 4 9291643 868 0 1338 2 9310180 938 0 1382 9 9309979
	 * Values in order are:
	 *   user nice system interrupt idle
	 */
	len = ncpu * CPUSTATES * sizeof(long);  /* [한국어] 모든 CPU의 5개 카운터를 담을 바이트 길이 계산. */
	cp_times = malloc(len);                 /* [한국어] sysctl 결과를 받을 임시 힙 버퍼. */
	if (cp_times == NULL) {                 /* [한국어] OOM 가드. */
		return -1;
	}
	if (sysctlbyname("kern.cp_times", cp_times, &len, NULL, 0) < 0) {  /* [한국어] 실제 카운터 일괄 조회. */
		free(cp_times);  /* [한국어] 실패 시 해제 후 즉시 리턴. */
		return -1;
	}

	*user = (uint64_t)cp_times[core * CPUSTATES + 0];  /* [한국어] FreeBSD CPUSTATES_USER=0. */
	*sys = (uint64_t)cp_times[core * CPUSTATES + 2];   /* [한국어] CPUSTATES_SYS=2 (nice는 인덱스 1, 본 함수는 무시). */
	*irq = (uint64_t)cp_times[core * CPUSTATES + 3];   /* [한국어] CPUSTATES_INTR=3, FreeBSD엔 softirq 구분 없어 단일 값. */

	free(cp_times);  /* [한국어] 임시 버퍼 해제 — 원본 데이터는 이미 out 파라미터에 복사됨. */
	return 0;
}
#elif defined(__linux__)
/*
 * [한국어]
 * parse_proc_stat (Linux 분기) - /proc/stat을 읽어 지정 lcore의 user/sys/irq+softirq 통계 추출.
 *
 * @core: 조회할 lcore 번호.
 * @user: out — user 모드 jiffies.
 * @sys:  out — system 모드 jiffies.
 * @irq:  out — irq + softirq 합산 jiffies.
 * @return: 0=성공, -1=fopen 실패 또는 해당 코어를 찾지 못함.
 *
 * /proc/stat 형식: 첫 줄은 "cpu" 합계, 그 뒤로 "cpuN user nice system idle iowait irq softirq steal guest guest_nice".
 * 일부 코어가 비활성(offline)된 환경에선 N이 연속 번호가 아닐 수 있어 cpu 번호를 명시적으로 비교한다.
 * fscanf의 '*'는 "값을 읽되 저장하지 않음"이라 nice/idle/iowait/steal/guest/guest_nice는 스킵된다.
 *
 * 호출 체인:
 *   init_proc_stat / app_get_proc_stat → [parse_proc_stat] → libc(fopen/fscanf) → procfs
 */
static int
parse_proc_stat(unsigned int core, uint64_t *user, uint64_t *sys, uint64_t *irq)
{
	FILE *f;                        /* [한국어] /proc/stat 스트림. */
	uint64_t i, soft_irq = 0, cpu = 0;  /* [한국어] i: 라인 카운터, soft_irq: softirq jiffies, cpu: 라인의 코어 번호. */
	int rc, found = 0;             /* [한국어] rc: fscanf 매칭 수, found: 일치 코어 발견 여부. */

	f = fopen("/proc/stat", "r");  /* [한국어] procfs를 read-only로 열기 — 실패 시 procfs 미마운트 환경. */
	if (!f) {
		return -1;
	}

	for (i = 0; i <= core + 1; i++) {  /* [한국어] core+1 라인까지 스캔(첫 줄은 합계 cpu, 그 다음부터 cpu0...). */
		/* scanf discards input with '*' in format,
		 * cpu;user;nice;system;idle;iowait;irq;softirq;steal;guest;guest_nice */
		rc = fscanf(f, "cpu%li %li %*i %li %*i %*i %li %li %*i %*i %*i\n",
			    &cpu, user, sys, irq, &soft_irq);
		/* [한국어] 5개 필드 매칭 기대: cpu번호(li), user(li), system(li), irq(li), softirq(li).
		 *         nice/idle/iowait/steal/guest/guest_nice는 '*i'로 스킵. */
		if (rc == EOF) {            /* [한국어] 파일 끝 — 해당 코어 라인이 없음. */
			fclose(f);
			return -1;
		}
		if (rc != 5) {              /* [한국어] 첫 합계 "cpu " 라인은 'cpu%li'에 매칭되지 못해 1로 나옴 — 스킵. */
			continue;
		}

		/* some cores can be disabled, list may not be in order */
		if (cpu == core) {          /* [한국어] 라벨이 일치하면 발견 — 종료. */
			found = 1;
			break;
		}
	}

	*irq += soft_irq;  /* [한국어] hardirq에 softirq를 더해 단일 'irq' 값으로 통합 — Linux의 두 카운터를 합쳐 FreeBSD와 의미 일치. */

	fclose(f);          /* [한국어] 스트림 닫기 — fd leak 방지. */
	return found ? 0 : -1;  /* [한국어] 발견 못 했다면 (코어 offline 등) -1. */
}
#else
/*
 * [한국어]
 * parse_proc_stat (그 외 OS) - 통계 미지원 더미.
 *
 * Windows 등에서는 /proc/stat / kern.cp_times가 없어 항상 -1 반환.
 * 호출자(init_proc_stat / app_get_proc_stat)는 -1을 받아 NOTICELOG만 남기고 통계 비활성화로 진행.
 */
static int
parse_proc_stat(unsigned int core, uint64_t *user, uint64_t *sys, uint64_t *irq)
{
	return -1;  /* [한국어] OS가 통계 인터페이스를 제공하지 않는다는 의미. */
}
#endif

/*
 * [한국어]
 * init_proc_stat - 코어 한 개에 대해 시드 CPU 통계를 g_initial_stat에 저장.
 *
 * @core: 시드를 기록할 lcore 번호.
 * @return: 0=성공, -1=범위 초과 또는 parse_proc_stat 실패.
 *
 * spdk_app_start의 SPDK_ENV_FOREACH_CORE 루프에서 활성 코어마다 1회 호출되어,
 * "SPDK가 올라간 시점"의 baseline tick 값을 저장한다. 이후 app_get_proc_stat이
 * 현재 누적값에서 baseline을 빼서 "SPDK 가동 후 사용된 jiffies"만 보고할 수 있다.
 *
 * 호출 체인:
 *   spdk_app_start → SPDK_ENV_FOREACH_CORE → [init_proc_stat] → parse_proc_stat
 */
static int
init_proc_stat(unsigned int core)
{
	uint64_t usr, sys, irq;  /* [한국어] parse_proc_stat이 채워줄 임시 변수. */

	if (core >= SPDK_CONFIG_MAX_LCORES) {  /* [한국어] g_initial_stat 배열 OOB 가드 — 컴파일 시 정해진 최대 코어 수 초과 시 거부. */
		return -1;
	}

	if (parse_proc_stat(core, &usr, &sys, &irq) < 0) {  /* [한국어] OS별 분기 호출 — 실패 시 통계 비활성화. */
		return -1;
	}

	g_initial_stat[core].irq = irq;  /* [한국어] baseline irq 누적치 저장. */
	g_initial_stat[core].usr = usr;  /* [한국어] baseline user 누적치 저장. */
	g_initial_stat[core].sys = sys;  /* [한국어] baseline system 누적치 저장. */

	return 0;
}

/*
 * [한국어]
 * app_get_proc_stat - 지정 코어의 "SPDK 가동 이후" CPU 통계 델타를 반환.
 *
 * @core: 조회 코어. @usr/@sys/@irq: out 델타.
 * @return: 0=성공, -1=실패.
 *
 * 본 함수는 lib/event 내부 다른 모듈(reactor 통계 RPC 등)에서 호출해 SPDK 자체가
 * 소비한 CPU 시간을 사용자에게 보고한다. baseline은 init_proc_stat에서 저장됨.
 *
 * 호출 체인:
 *   reactor 통계 RPC handler / event_internal 사용자 → [app_get_proc_stat] → parse_proc_stat
 */
int
app_get_proc_stat(unsigned int core, uint64_t *usr, uint64_t *sys, uint64_t *irq)
{
	uint64_t _usr, _sys, _irq;  /* [한국어] 절대값 임시 — 델타 계산을 위해 분리. */

	if (core >= SPDK_CONFIG_MAX_LCORES) {  /* [한국어] 범위 가드. */
		return -1;
	}

	if (parse_proc_stat(core, &_usr, &_sys, &_irq) < 0) {  /* [한국어] 현재 시점 통계 다시 읽음. */
		return -1;
	}

	*irq = _irq - g_initial_stat[core].irq;  /* [한국어] 부팅 시점부터의 누적 irq 사용량(jiffies). */
	*usr = _usr - g_initial_stat[core].usr;  /* [한국어] 누적 user 시간. */
	*sys = _sys - g_initial_stat[core].sys;  /* [한국어] 누적 system 시간. */

	return 0;
}

/*
 * [한국어]
 * app_start_shutdown - app_thread에서 안전하게 실행되는 graceful shutdown 디스패처.
 *
 * @ctx: 미사용(시그니처 호환). 호출자는 NULL을 넘김.
 *
 * 시그널 핸들러나 외부 모듈이 spdk_app_start_shutdown()을 호출하면 spdk_thread_send_critical_msg
 * 가 본 함수를 app_thread에 enqueue한다. 사용자 정의 shutdown_cb가 있으면 그걸 우선 호출(이후
 * 앱이 자체적으로 spdk_app_stop 호출 책임), 없으면 즉시 spdk_app_stop(0).
 *
 * 호출 체인:
 *   spdk_app_start_shutdown → spdk_thread_send_critical_msg → [app_start_shutdown(app_thread)]
 *     → user shutdown_cb / spdk_app_stop
 */
static void
app_start_shutdown(void *ctx)
{
	if (g_spdk_app.shutdown_cb) {       /* [한국어] 사용자 정의 graceful 핸들러가 등록돼 있는지 확인. */
		g_spdk_app.shutdown_cb();   /* [한국어] 사용자 콜백 호출 — 사용자가 자기 자원 정리 후 spdk_app_stop 호출 책임. */
		g_spdk_app.shutdown_cb = NULL;  /* [한국어] 두 번째 시그널이 와도 콜백을 다시 부르지 않도록 초기화(멱등성). */
	} else {                            /* [한국어] 콜백이 없으면 곧장 종료 시퀀스 진입. */
		spdk_app_stop(0);           /* [한국어] rc=0(정상 종료)로 stop 메시지 전송 — app_thread에서 app_stop 실행. */
	}
}

/*
 * [한국어]
 * spdk_app_start_shutdown - 외부에서 호출 가능한 비동기 shutdown 트리거.
 *
 * 어느 스레드에서든 안전하게 호출할 수 있는 진입점. critical_msg 큐는 일반 send_msg보다
 * 우선순위가 높아 app_thread가 바쁜 작업 중이어도 빠르게 처리된다.
 *
 * 호출 체인:
 *   external (시그널 핸들러, 다른 SPDK thread) → [spdk_app_start_shutdown] → spdk_thread_send_critical_msg
 */
void
spdk_app_start_shutdown(void)
{
	/* [한국어] critical_msg는 lockless mpsc 큐의 우선순위 슬롯에 들어가며, app_thread가 다음 폴 사이클에서 즉시 처리. */
	spdk_thread_send_critical_msg(spdk_thread_get_app_thread(), app_start_shutdown);
}

/*
 * [한국어]
 * __shutdown_signal - SIGINT/SIGTERM 시그널 핸들러.
 *
 * @signo: 시그널 번호(미사용).
 *
 * 시그널 핸들러 컨텍스트(async-signal-safe 제약!)에서 실행되므로 최소한의 작업만 수행.
 * spdk_thread_send_critical_msg는 lockless ring 기반이라 시그널 컨텍스트에서 호출해도 안전하다.
 * 첫 시그널만 처리하고 두 번째부터는 무시 — 사용자가 강제 종료를 원하면 SIGKILL을 보내야 함.
 *
 * 호출 체인:
 *   kernel signal delivery → [__shutdown_signal] → spdk_app_start_shutdown → app_thread (app_start_shutdown)
 */
static void
__shutdown_signal(int signo)
{
	if (!g_shutdown_sig_received) {       /* [한국어] 멱등성 — 두 번째 SIGINT 등은 무시(첫 graceful shutdown 진행 중). */
		g_shutdown_sig_received = true;  /* [한국어] 플래그 셋 — 같은 시그널 핸들러 재진입 시 스킵. */
		spdk_app_start_shutdown();    /* [한국어] critical msg로 app_thread에 graceful shutdown 작업 위임. */
	}
}

/*
 * [한국어]
 * app_opts_validate - 앱이 추가한 short option 문자열이 SPDK 표준 옵션과 충돌하는지 검사.
 *
 * @app_opts: 앱 고유 short option 문자열(예: "T:V" 형태). getopt 제어 문자 ':', '+', '-' 포함 가능.
 * @return: 0=충돌 없음, 그 외=중복된 첫 문자(에러 메시지에 사용).
 *
 * spdk_app_parse_args가 앱 고유 옵션을 SPDK 표준 옵션 문자열과 strcat하기 전에 호출.
 * 충돌이 있으면 사용자가 같은 short flag에 두 의미를 부여한 것이므로 부팅을 거부.
 *
 * 호출 체인:
 *   spdk_app_parse_args → [app_opts_validate] (SPDK_APP_GETOPT_STRING 와 비교)
 */
static int
app_opts_validate(const char *app_opts)
{
	int i = 0, j;  /* [한국어] i=app_opts 인덱스, j=SPDK 표준 문자열 인덱스. */

	for (i = 0; app_opts[i] != '\0'; i++) {  /* [한국어] 앱 측 옵션 문자열을 한 글자씩 순회. */
		/* ignore getopt control characters */
		if (app_opts[i] == ':' || app_opts[i] == '+' || app_opts[i] == '-') {
			continue;  /* [한국어] ':' = required_argument 마커, '+'/'-' = getopt 모드 스위치 — 비교 대상 아님. */
		}

		for (j = 0; SPDK_APP_GETOPT_STRING[j] != '\0'; j++) {  /* [한국어] SPDK가 이미 사용 중인 short flag 집합과 일대일 비교. */
			if (app_opts[i] == SPDK_APP_GETOPT_STRING[j]) {
				return app_opts[i];  /* [한국어] 첫 충돌 문자를 반환 — 호출자가 에러 메시지로 출력. */
			}
		}
	}
	return 0;  /* [한국어] 끝까지 충돌 없음. */
}

/*
 * [한국어]
 * calculate_mempool_size - 메시지 풀 크기 자동 산정 또는 사용자 지정값 채택.
 *
 * @opts: out — 최종 결정된 msg_mempool_size를 기록할 opts.
 * @opts_user: in — 사용자가 spdk_app_start에 넘긴 원본 opts(미지정 여부 판단용).
 *
 * SPDK thread 간 메시지(spdk_msg)는 사전 할당된 mempool에서 빌려 쓴다. 코어가 많을수록
 * 메시지 동시성이 커지므로 64코어 초과 시 코어당 4×SPDK_MSG_MEMPOOL_CACHE_SIZE 만큼 확보.
 * 사용자가 명시 지정했으면 자동 계산을 건너뛰고 그 값을 그대로 사용.
 * 호출 시점은 spdk_env_init() 이후(코어 수가 확정된 시점)여야 한다.
 *
 * 호출 체인:
 *   spdk_app_start → spdk_env_init() → [calculate_mempool_size] → spdk_reactors_init(opts->msg_mempool_size)
 */
static void
calculate_mempool_size(struct spdk_app_opts *opts,
		       struct spdk_app_opts *opts_user)
{
	uint32_t core_count = spdk_env_get_core_count();  /* [한국어] DPDK가 활성화한 lcore 개수 — env_init 이후에만 유효. */

	if (!opts_user->msg_mempool_size) {  /* [한국어] 사용자가 0(기본값) 그대로 두면 자동 계산. */
		/* The user didn't specify msg_mempool_size, so let's calculate it.
		   Set the default (SPDK_DEFAULT_MSG_MEMPOOL_SIZE) if less than
		   64 cores, and use 4k per core otherwise */
		/* [한국어] max(상수 디폴트, 코어수×코어당 풀 크기) — 작은 시스템엔 상수가, 큰 시스템엔 비례 항이 우세. */
		opts->msg_mempool_size = spdk_max(SPDK_DEFAULT_MSG_MEMPOOL_SIZE,
						  core_count * SPDK_APP_PER_CORE_MSG_MEMPOOL_SIZE);
	} else {
		opts->msg_mempool_size = opts_user->msg_mempool_size;  /* [한국어] 사용자 명시값 그대로 채택. */
	}
}

/*
 * [한국어]
 * spdk_app_opts_init - 사용자 spdk_app_opts 구조체에 안전한 디폴트값을 채워주는 공개 API.
 *
 * @opts: 채울 opts 구조체 포인터(사용자 스택 또는 힙).
 * @opts_size: sizeof(struct spdk_app_opts) — ABI 호환성 검사를 위해 필수.
 *
 * 사용자는 main()에서 이 함수를 호출해 디폴트값을 받은 뒤 필요한 필드만 변경하고
 * spdk_app_start에 넘긴다. SET_FIELD 매크로는 opts_size를 검사해 오래된 SPDK 헤더로
 * 빌드된 사용자 코드와도 안전하게 동작(미래에 추가된 필드는 OOB 쓰기 방지).
 * 호출 컨텍스트: main 스레드, 부팅 전 단 한 번.
 *
 * 호출 체인:
 *   user main() → [spdk_app_opts_init] → (user customize) → spdk_app_start
 */
void
spdk_app_opts_init(struct spdk_app_opts *opts, size_t opts_size)
{
	if (!opts) {  /* [한국어] NULL 가드 — 사용자 실수 보호. */
		SPDK_ERRLOG("opts should not be NULL\n");
		return;
	}

	if (!opts_size) {  /* [한국어] opts_size=0이면 SET_FIELD가 모든 필드를 OOB 판정 → 의미 없음. */
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return;
	}

	memset(opts, 0, opts_size);     /* [한국어] 사용자 스택의 비초기화 메모리를 0으로 클리어 — false/NULL 디폴트 부여. */
	opts->opts_size = opts_size;    /* [한국어] 이후 spdk_app_start가 ABI 검사를 위해 읽음 — 사용자가 변경하면 안 됨. */

	/* [한국어] SET_FIELD: 컴파일 시점의 필드 오프셋이 사용자가 넘긴 opts_size 안에 들어갈 때만 안전하게 대입.
	 *         즉 SPDK 라이브러리가 사용자 코드보다 새 필드를 더 가지고 있어도 OOB 쓰기를 발생시키지 않음. */
#define SET_FIELD(field, value) \
	if (offsetof(struct spdk_app_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = value; \
	} \

	SET_FIELD(enable_coredump, true);                                /* [한국어] 기본은 코어 덤프 활성. -d 플래그로 비활성화. */
	SET_FIELD(shm_id, -1);                                           /* [한국어] -1=미지정 → pid 기반 SHM 이름. */
	SET_FIELD(mem_size, SPDK_APP_DPDK_DEFAULT_MEM_SIZE);             /* [한국어] -1=DPDK 자동 결정. */
	SET_FIELD(main_core, SPDK_APP_DPDK_DEFAULT_MAIN_CORE);           /* [한국어] -1=DPDK 기본. */
	SET_FIELD(mem_channel, SPDK_APP_DPDK_DEFAULT_MEM_CHANNEL);       /* [한국어] -1=DPDK 기본. */
	SET_FIELD(base_virtaddr, SPDK_APP_DPDK_DEFAULT_BASE_VIRTADDR);   /* [한국어] 32TiB 영역 — ASLR 충돌 회피. */
	SET_FIELD(print_level, SPDK_APP_DEFAULT_LOG_PRINT_LEVEL);        /* [한국어] INFO 이상이 stderr로 출력. */
	SET_FIELD(rpc_addr, SPDK_DEFAULT_RPC_ADDR);                      /* [한국어] /var/tmp/spdk.sock — RPC 서버 listen 기본 경로. */
	SET_FIELD(num_entries, SPDK_APP_DEFAULT_NUM_TRACE_ENTRIES);      /* [한국어] 기본 트레이스 엔트리 수. */
	SET_FIELD(num_trace_threads, 0);                                 /* [한국어] 0=spdk_thread별 트레이스 미할당(코어별만). */
	SET_FIELD(delay_subsystem_init, false);                          /* [한국어] 기본은 부팅 직후 subsystem init 진행. */
	SET_FIELD(disable_signal_handlers, false);                       /* [한국어] 기본은 SIGINT/SIGTERM 핸들러 설치. */
	SET_FIELD(interrupt_mode, false);                                /* [한국어] 기본은 폴링 모드(SPDK 표준). */
	SET_FIELD(enforce_numa, false);                                  /* [한국어] NUMA 강제 미적용 → DPDK가 가용 NUMA에서 자유 할당. */
	/* Don't set msg_mempool_size here, it is set or calculated later */
	/* [한국어] msg_mempool_size는 calculate_mempool_size에서 코어 수 기반 자동 산정 또는 사용자 명시값으로 결정 — 여기선 0으로 두어 "미지정" 표시. */
	SET_FIELD(rpc_allowlist, NULL);                                  /* [한국어] NULL=모든 RPC 허용. */
	SET_FIELD(rpc_log_file, NULL);                                   /* [한국어] NULL=RPC 호출 로그 미기록. */
	SET_FIELD(rpc_log_level, SPDK_LOG_DISABLED);                     /* [한국어] 로그 출력 자체 비활성. */
	SET_FIELD(disable_cpumask_locks, false);                         /* [한국어] 기본은 /var/tmp/spdk_cpu_lock_NNN 락 사용. */
#undef SET_FIELD
}

/*
 * [한국어]
 * app_setup_signal_handlers - SIGPIPE 무시 + SIGINT/SIGTERM graceful shutdown 핸들러 설치.
 *
 * @opts: app opts(현재 본문에서 사용은 안 하지만 향후 확장 대비 인자 보존).
 * @return: 0=성공, 음수=sigaction 실패.
 *
 * 부팅 직후(reactor·app_thread 생성 후) 한 번만 호출. SIGPIPE는 RPC 클라이언트 갑작스런 단절
 * 등에서 프로세스가 죽지 않도록 무시 처리. SIGINT/SIGTERM은 동일 핸들러(__shutdown_signal)에
 * 매핑해 graceful shutdown을 트리거. 마지막에 pthread_sigmask로 두 시그널 차단 해제 — 일부
 * 라이브러리가 미리 마스킹해 두었을 가능성 대비.
 *
 * 호출 체인:
 *   spdk_app_start → [app_setup_signal_handlers] → libc(sigaction, pthread_sigmask)
 */
static int
app_setup_signal_handlers(struct spdk_app_opts *opts)
{
	struct sigaction	sigact;   /* [한국어] sigaction 시스템콜 인자 — 핸들러와 동작 플래그 명세. */
	sigset_t		sigmask;  /* [한국어] 마지막 sigprocmask 호출에 쓸 unblock 셋. */
	int			rc;

	sigemptyset(&sigmask);                 /* [한국어] 빈 셋으로 초기화 후 SIGINT/SIGTERM만 추가할 예정. */
	memset(&sigact, 0, sizeof(sigact));    /* [한국어] sa_flags 등 부수 필드 0으로 — SA_RESTART 등은 사용 안 함. */
	sigemptyset(&sigact.sa_mask);          /* [한국어] 핸들러 실행 중 추가로 막을 시그널 없음. */

	sigact.sa_handler = SIG_IGN;           /* [한국어] SIGPIPE는 무시. RPC 클라이언트가 socket 갑자기 닫아도 프로세스 죽지 않게. */
	rc = sigaction(SIGPIPE, &sigact, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("sigaction(SIGPIPE) failed\n");
		return rc;
	}

	/* Install the same handler for SIGINT and SIGTERM */
	g_shutdown_sig_received = false;       /* [한국어] 매 부팅마다 시그널 수신 플래그 리셋(spdk_app_start 재진입 가능). */
	sigact.sa_handler = __shutdown_signal; /* [한국어] graceful shutdown 핸들러로 교체. */
	rc = sigaction(SIGINT, &sigact, NULL); /* [한국어] Ctrl+C — 사용자 인터럽트. */
	if (rc < 0) {
		SPDK_ERRLOG("sigaction(SIGINT) failed\n");
		return rc;
	}
	sigaddset(&sigmask, SIGINT);           /* [한국어] 후속 unblock 대상에 등록. */

	rc = sigaction(SIGTERM, &sigact, NULL);  /* [한국어] systemd/kill 등이 보내는 정상 종료 시그널. */
	if (rc < 0) {
		SPDK_ERRLOG("sigaction(SIGTERM) failed\n");
		return rc;
	}
	sigaddset(&sigmask, SIGTERM);          /* [한국어] 후속 unblock 대상에 등록. */

	/* [한국어] 다른 라이브러리가 사전에 두 시그널을 블록했을 수 있어 현재 스레드에서 명시적으로 unblock.
	 *         시그널 라우팅은 프로세스 단위가 아닌 스레드 단위 마스크에 영향받음. */
	pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL);

	return 0;
}

/*
 * [한국어]
 * app_start_application - 모든 subsystem 초기화 완료 후 사용자 진입점(start_fn) 호출.
 *
 * @rc: 직전 단계(SPDK_RPC_RUNTIME 단계 RPC 로드)의 결과.
 * @arg1: 미사용(콜백 시그니처 호환).
 *
 * 부팅 시퀀스 마지막 단계. 여기서 g_start_fn(g_start_arg)이 실행되며 사용자 코드가
 * 본격적으로 bdev/nvmf/iscsi 등을 사용하기 시작한다. RPC 서버는 부팅 동안 pause되어 있어
 * 외부에서 임의 RPC가 들어오는 것을 방지했는데, 여기서 resume해 일반 동작 모드로 전환.
 * 호출 컨텍스트: app_thread (assert로 검증).
 *
 * 호출 체인:
 *   app_subsystem_init_done → spdk_subsystem_load_config(RUNTIME) → [app_start_application] → g_start_fn
 */
static void
app_start_application(int rc, void *arg1)
{
	assert(spdk_thread_is_app_thread(NULL));  /* [한국어] 반드시 app_thread에서 실행 — subsystem 콜백 모델 약속 검증. */

	if (rc) {  /* [한국어] RUNTIME RPC 로드 실패 — 사용자 진입점 진입하지 않고 즉시 종료. */
		SPDK_ERRLOG("Failed to load subsystems for RUNTIME state with code: %d\n", rc);
		spdk_app_stop(rc);
		return;
	}

	if (g_spdk_app.rpc_addr) {                       /* [한국어] RPC 서버를 띄운 경우에만 resume 필요. */
		spdk_rpc_server_resume(g_spdk_app.rpc_addr);  /* [한국어] 부팅 중 일시중지된 listen socket을 다시 활성화 — 외부 클라이언트 접속 허용. */
	}

	g_start_fn(g_start_arg);  /* [한국어] 사용자 진입점 호출 — 사용자 코드는 app_thread에서 실행된다. */
}

/*
 * [한국어]
 * app_subsystem_init_done - 모든 subsystem의 init이 끝났을 때 호출되는 콜백.
 *
 * @rc: subsystem_init 누적 결과(0=성공, 그 외=일부 모듈 실패).
 * @arg1: 미사용.
 *
 * STARTUP→RUNTIME 상태 전이를 수행: rpc_allowlist 적용, RPC 상태를 RUNTIME으로 변경,
 * JSON config가 있으면 RUNTIME 단계 RPC들을 마저 로드. 모두 끝나면 app_start_application을
 * 호출해 사용자 진입점으로 넘어감. JSON config가 없으면 곧장 app_start_application 호출.
 *
 * 호출 체인:
 *   spdk_subsystem_init → [app_subsystem_init_done(app_thread)]
 *     → spdk_subsystem_load_config(RUNTIME) → app_start_application
 */
static void
app_subsystem_init_done(int rc, void *arg1)
{
	if (rc) {  /* [한국어] 모듈 의존성 그래프 init 중 어떤 모듈이 실패한 경우. */
		SPDK_ERRLOG("Subsystem initialization failed with code: %d\n", rc);
		spdk_app_stop(rc);
		return;
	}

	spdk_rpc_set_allowlist(g_spdk_app.rpc_allowlist);  /* [한국어] --rpcs-allowed로 지정된 화이트리스트를 RPC 서버에 적용. */
	spdk_rpc_set_state(SPDK_RPC_RUNTIME);              /* [한국어] STARTUP→RUNTIME — STARTUP 전용 RPC는 더 이상 받지 않음. */

	if (g_spdk_app.json_data) {                        /* [한국어] JSON config가 있으면 RUNTIME 섹션 RPC들을 마저 로드. */
		/* Load SPDK_RPC_RUNTIME RPCs from config file */
		assert(spdk_rpc_get_state() == SPDK_RPC_RUNTIME);  /* [한국어] 위에서 셋한 상태 검증 — 다른 경로로 변경됐으면 버그. */
		/* [한국어] config 두 단계 로드 모델: STARTUP은 bootstrap_fn에서, RUNTIME은 여기서. 완료 시 app_start_application 호출. */
		spdk_subsystem_load_config(g_spdk_app.json_data, g_spdk_app.json_data_size,
					   app_start_application, NULL,
					   !g_spdk_app.json_config_ignore_errors);
		free(g_spdk_app.json_data);                /* [한국어] config 데이터를 lib/init이 내부 복사해 사용하므로 여기서 안전 해제. */
		g_spdk_app.json_data = NULL;               /* [한국어] dangling 방지. */
	} else {
		app_start_application(0, NULL);            /* [한국어] config 없음 — 직접 사용자 진입점으로. */
	}
}

/*
 * [한국어]
 * app_do_spdk_subsystem_init - STARTUP RPC 로드 완료 후 RPC 서버 부트 + subsystem_init 트리거.
 *
 * @rc: 직전 단계(STARTUP 단계 RPC 로드)의 결과.
 * @arg1: 미사용.
 *
 * bootstrap_fn → spdk_subsystem_load_config(STARTUP) 의 완료 콜백으로 호출됨.
 * RPC 주소가 있으면 RPC 서버를 본격 init한 뒤 잠시 pause(부팅 중 외부 영향 차단). 단,
 * --wait-for-rpc 모드면 여기서 멈춰 외부 RPC(framework_start_init)가 명시적으로 들어오기를 기다림.
 * 일반 모드에선 즉시 spdk_subsystem_init을 호출해 모듈 init 단계로 진입.
 *
 * 호출 체인:
 *   bootstrap_fn → spdk_subsystem_load_config(STARTUP) → [app_do_spdk_subsystem_init(app_thread)]
 *     → spdk_rpc_initialize / spdk_subsystem_init → app_subsystem_init_done
 */
static void
app_do_spdk_subsystem_init(int rc, void *arg1)
{
	struct spdk_rpc_opts opts;  /* [한국어] RPC 서버 옵션(log_level, log_file 등). 호출자별로 채워야 함. */

	if (rc) {  /* [한국어] STARTUP RPC 로드 중 오류면 즉시 종료. */
		spdk_app_stop(rc);
		return;
	}

	if (g_spdk_app.rpc_addr) {  /* [한국어] RPC 서버를 띄울 경우의 분기. */
		opts.size = SPDK_SIZEOF(&opts, log_level);  /* [한국어] ABI 호환: log_level 필드까지의 크기를 명시 — 이후 추가 필드 미사용 의미. */
		opts.log_file = g_spdk_app.rpc_log_file;
		opts.log_level = g_spdk_app.rpc_log_level;

		rc = spdk_rpc_initialize(g_spdk_app.rpc_addr, &opts);  /* [한국어] UNIX socket bind/listen + 핸들러 디스패처 등록. */
		if (rc) {
			spdk_app_stop(rc);
			return;
		}
		if (g_delay_subsystem_init) {  /* [한국어] --wait-for-rpc — RPC 서버는 띄웠지만 subsystem_init은 외부 트리거 대기. */
			return;
		}
		spdk_rpc_server_pause(g_spdk_app.rpc_addr);  /* [한국어] subsystem_init 동안 외부 요청 차단(부팅 일관성). */
	} else {
		SPDK_DEBUGLOG(app_rpc, "RPC server not started\n");  /* [한국어] --no-rpc-server 모드의 흔적 로그. */
	}
	spdk_subsystem_init(app_subsystem_init_done, NULL);  /* [한국어] lib/init이 의존성 토폴로지 순으로 모든 subsystem init 호출. */
}

/*
 * [한국어]
 * app_opts_add_pci_addr - --pci-allowed/--pci-blocked 인자 BDF 문자열을 파싱해 동적 배열에 추가.
 *
 * @opts: in/out — opts->num_pci_addr를 +1 갱신.
 * @list: in/out 더블 포인터 — opts->pci_allowed 또는 opts->pci_blocked를 가리킴.
 * @bdf: "0000:81:00.0" 형식의 PCI 주소 문자열.
 * @return: 0=성공, -ENOMEM=realloc 실패, -EINVAL=BDF 파싱 실패.
 *
 * realloc 기반 가변 배열 패턴: 기존 배열 끝에 한 칸을 늘려 새 PCI 주소를 파싱해 넣는다.
 * BDF 포맷은 lib/util/pci_addr.c가 검사하며, 잘못된 형식이면 호출자가 free()로 정리해야 함.
 *
 * 호출 체인:
 *   spdk_app_parse_args (case PCI_ALLOWED/BLOCKED_OPT_IDX) → [app_opts_add_pci_addr]
 *     → spdk_pci_addr_parse
 */
static int
app_opts_add_pci_addr(struct spdk_app_opts *opts, struct spdk_pci_addr **list, char *bdf)
{
	struct spdk_pci_addr *tmp = *list;     /* [한국어] 기존 배열 시작 주소 — realloc 후 업데이트됨. */
	size_t i = opts->num_pci_addr;          /* [한국어] 현재 길이 — 이 인덱스에 새 항목을 쓸 예정. */

	tmp = realloc(tmp, sizeof(*tmp) * (i + 1));  /* [한국어] +1 슬롯 확장. NULL이면 호출자 free()로 정리. */
	if (tmp == NULL) {
		SPDK_ERRLOG("realloc error\n");
		return -ENOMEM;
	}

	*list = tmp;  /* [한국어] 호출자가 보유한 포인터 갱신 — realloc이 위치를 옮겼을 수 있음. */
	if (spdk_pci_addr_parse(*list + i, bdf) < 0) {  /* [한국어] BDF 문자열을 struct spdk_pci_addr로 파싱. */
		SPDK_ERRLOG("Invalid address %s\n", bdf);
		return -EINVAL;
	}

	opts->num_pci_addr++;  /* [한국어] 길이 갱신 — 다음 호출 시 새로운 i 값으로 사용. */
	return 0;
}

/*
 * [한국어]
 * app_setup_env - spdk_app_opts → spdk_env_opts 매핑 + spdk_env_init(=DPDK EAL 부팅) 호출.
 *
 * @opts: NULL이면 "재초기화 모드"(이미 한 번 부팅된 프로세스에서 spdk_app_start가 다시 불릴 때),
 *        non-NULL이면 정상 부팅.
 * @return: 0=성공, 음수=DPDK EAL 초기화 실패.
 *
 * spdk_env_init 내부에서 DPDK rte_eal_init가 호출되며 이 시점에 hugepage를 매핑하고 PCI
 * 디바이스를 probe하며 lcore 토폴로지를 결정한다. 따라서 본 함수가 끝나야
 * spdk_env_get_core_count() 같은 API가 의미 있는 값을 반환한다.
 * pci_blocked/pci_allowed 배열은 spdk_env_init이 내부 복사해 사용하므로 호출 직후 free 가능.
 *
 * 호출 체인:
 *   spdk_app_start → [app_setup_env] → spdk_env_init → DPDK rte_eal_init → /dev/hugepages 매핑
 */
static int
app_setup_env(struct spdk_app_opts *opts)
{
	struct spdk_env_opts env_opts = {};  /* [한국어] DPDK 추상화 옵션 — opts(spdk_app_opts) 필드를 변환해 채울 예정. */
	int rc;

	if (opts == NULL) {  /* [한국어] 재초기화 경로 — spdk_app_start가 g_env_was_setup=true일 때 NULL을 넘김. */
		rc = spdk_env_init(NULL);  /* [한국어] DPDK 측에서 idempotent하게 동작하도록 NULL 인자로 호출. */
		if (rc != 0) {
			SPDK_ERRLOG("Unable to reinitialize SPDK env\n");
		}

		return rc;
	}

	env_opts.opts_size = sizeof(env_opts);     /* [한국어] ABI 호환 — env 측이 새로운 필드를 추가했더라도 안전. */
	spdk_env_opts_init(&env_opts);             /* [한국어] env 측 디폴트값(예: lcore_map=NULL) 채움. */

	/* [한국어] 이하 앱 옵션 → env 옵션 1:1 매핑. 의미는 옵션 정의의 인라인 주석 참고. */
	env_opts.name = opts->name;                /* [한국어] DPDK rte_eal_init의 program name(로그/공유 메모리 prefix에 사용). */
	env_opts.core_mask = opts->reactor_mask;   /* [한국어] -m 마스크 — DPDK가 활성 lcore를 결정. */
	env_opts.lcore_map = opts->lcore_map;      /* [한국어] --lcores 매핑 문자열. core_mask와 상호 배타. */
	env_opts.shm_id = opts->shm_id;            /* [한국어] DPDK shared config + 트레이스 SHM 식별자. */
	env_opts.mem_channel = opts->mem_channel;  /* [한국어] -n. */
	env_opts.main_core = opts->main_core;      /* [한국어] -p. */
	env_opts.mem_size = opts->mem_size;        /* [한국어] -s (MB). */
	env_opts.hugepage_single_segments = opts->hugepage_single_segments;  /* [한국어] -g. */
	env_opts.unlink_hugepage = opts->unlink_hugepage;                    /* [한국어] -R. */
	env_opts.hugedir = opts->hugedir;          /* [한국어] --huge-dir. */
	env_opts.no_pci = opts->no_pci;            /* [한국어] -u. */
	env_opts.num_pci_addr = opts->num_pci_addr; /* [한국어] pci_blocked/allowed 배열의 길이. */
	env_opts.pci_blocked = opts->pci_blocked;  /* [한국어] BDF 차단 리스트. */
	env_opts.pci_allowed = opts->pci_allowed;  /* [한국어] BDF 허용 리스트. */
	env_opts.base_virtaddr = opts->base_virtaddr;  /* [한국어] hugepage 매핑 가상주소 시작점. */
	env_opts.env_context = opts->env_context;  /* [한국어] DPDK 추가 EAL 인자 패스스루. */
	env_opts.iova_mode = opts->iova_mode;      /* [한국어] pa/va. */
	env_opts.vf_token = opts->vf_token;        /* [한국어] VFIO VF 토큰. */
	env_opts.no_huge = opts->no_huge;          /* [한국어] hugepage 미사용 모드. */
	env_opts.enforce_numa = opts->enforce_numa;  /* [한국어] NUMA 강제. */

	rc = spdk_env_init(&env_opts);             /* [한국어] DPDK EAL 본격 부팅 — 시간이 가장 오래 걸리는 단계 중 하나. */
	free(env_opts.pci_blocked);                /* [한국어] env_init이 내부 복사 후 사용했으므로 사본 free 안전. */
	free(env_opts.pci_allowed);

	if (rc < 0) {
		SPDK_ERRLOG("Unable to initialize SPDK env\n");
		if (getuid() != 0) {  /* [한국어] hugepage/PCI 자원은 root 권한이 흔히 필요 — 사용자 친화적 힌트 출력. */
			SPDK_ERRLOG("You may need to run as root\n");
		}
	}

	return rc;
}

/*
 * [한국어]
 * spdk_app_setup_trace - 트레이스 SHM 생성 + tpoint 그룹/마스크 활성화.
 *
 * @opts: shm_id, name, num_entries, num_trace_threads, tpoint_group_mask 사용.
 * @return: 0=성공, -1=실패(SHM 생성/마스크 파싱).
 *
 * SPDK는 /dev/shm/<name><base>{pid|id} 라는 메모리 매핑 파일에 코어별 트레이스 링버퍼를 둔다.
 * 외부 도구(spdk_trace, spdk_top)가 동일 파일을 mmap해 실시간 이벤트를 읽을 수 있다.
 * tpoint_group_mask 옵션은 "group:mask,group:mask,..." 또는 "group" 형태를 받아 파싱.
 *
 * 호출 체인:
 *   spdk_app_start → [spdk_app_setup_trace] → spdk_trace_init / spdk_trace_set_tpoints
 */
int
spdk_app_setup_trace(struct spdk_app_opts *opts)
{
	char		shm_name[64];                              /* [한국어] /dev/shm 식별자(앱 이름 + base + id/pid). */
	uint64_t	tpoint_group_mask, tpoint_mask = -1ULL;    /* [한국어] tpoint_mask 디폴트는 모든 비트(=그룹 내 모든 tpoint 활성). */
	char		*end = NULL, *tpoint_group_mask_str, *tpoint_group_str = NULL;  /* [한국어] strtoull/strsepq 작업용 포인터들. */
	char		*tp_g_str, *tpoint_group, *tpoints;        /* [한국어] tp_g_str: 원본 strdup 보존, group/tpoints: 파싱 결과 부분 문자열. */
	bool		error_found = false;                       /* [한국어] 파싱 도중 잘못된 토큰 발견 여부. */
	uint64_t	group_id;                                  /* [한국어] 비트 위치를 그룹 ID로 환원해 spdk_trace_set_tpoints에 전달. */

	if (opts->shm_id >= 0) {  /* [한국어] 사용자가 -i로 명시적 SHM ID를 줬으면 그 ID로 SHM 이름 합성. */
		snprintf(shm_name, sizeof(shm_name), "/%s%s%d", opts->name,
			 SPDK_TRACE_SHM_NAME_BASE, opts->shm_id);
	} else {  /* [한국어] 미지정이면 pid 기반(다중 인스턴스 자동 충돌 회피). */
		snprintf(shm_name, sizeof(shm_name), "/%s%spid%d", opts->name,
			 SPDK_TRACE_SHM_NAME_BASE, (int)getpid());
	}

	if (spdk_trace_init(shm_name, opts->num_entries, opts->num_trace_threads) != 0) {
		/* [한국어] /dev/shm 파일 생성 + mmap + 코어별 ringbuffer 초기화. 실패 시 -1. */
		return -1;
	}

	if (opts->tpoint_group_mask == NULL) {  /* [한국어] -e 옵션 미지정 → 트레이스 인프라만 깔고 tpoint 활성화는 RPC로 미룰 수 있음. */
		return 0;
	}

	tpoint_group_mask_str = strdup(opts->tpoint_group_mask);  /* [한국어] strsepq가 원본을 변경하므로 strdup으로 사본 작업. */
	if (tpoint_group_mask_str == NULL) {
		SPDK_ERRLOG("Unable to get string of tpoint group mask from opts.\n");
		return -1;
	}
	/* Save a pointer to the original value of the tpoint group mask string
	 * to free later, because spdk_strsepq() modifies given char*. */
	tp_g_str = tpoint_group_mask_str;  /* [한국어] strsepq가 옮겨도 free 가능한 원본 시작점 보관. */
	while ((tpoint_group_str = spdk_strsepq(&tpoint_group_mask_str, ",")) != NULL) {
		/* [한국어] ',' 구분자로 그룹 단위 토큰 분리. 각 토큰은 "group:tpoints" 또는 "group". */
		if (strchr(tpoint_group_str, ':')) {  /* [한국어] ':' 포함 → 그룹+tpoint 마스크 형식. */
			/* Get the tpoint group mask */
			tpoint_group = spdk_strsepq(&tpoint_group_str, ":");  /* [한국어] 콜론 앞부분: 그룹 마스크 또는 그룹 이름. */
			/* Get the tpoint mask inside that group */
			tpoints = spdk_strsepq(&tpoint_group_str, ":");       /* [한국어] 콜론 뒷부분: 그룹 내부 tpoint 마스크(16진수). */

			errno = 0;
			tpoint_group_mask = strtoull(tpoint_group, &end, 16);  /* [한국어] hex 그룹 마스크로 먼저 시도. */
			if (*end != '\0' || errno) {  /* [한국어] hex가 아니면 그룹 이름으로 간주해 lib/trace에 조회. */
				tpoint_group_mask = spdk_trace_create_tpoint_group_mask(tpoint_group);
				if (tpoint_group_mask == 0) {
					error_found = true;
					break;
				}
			}
			/* Check if tpoint group mask has only one bit set.
			 * This is to avoid enabling individual tpoints in
			 * more than one tracepoint group at once. */
			if (!spdk_u64_is_pow2(tpoint_group_mask)) {
				/* [한국어] tpoint 마스크는 그룹 내부 비트에 의미가 있으므로 두 그룹을 동시에 지정하면 모호 — 거부. */
				SPDK_ERRLOG("Tpoint group mask: %s contains multiple tpoint groups.\n", tpoint_group);
				SPDK_ERRLOG("This is not supported, to prevent from activating tpoints by mistake.\n");
				error_found = true;
				break;
			}

			errno = 0;
			tpoint_mask = strtoull(tpoints, &end, 16);  /* [한국어] 그룹 내부 마스크 파싱(hex). */
			if (*end != '\0' || errno) {
				error_found = true;
				break;
			}
		} else {  /* [한국어] ':' 없음 → 그룹 전체 활성화. */
			errno = 0;
			tpoint_group_mask = strtoull(tpoint_group_str, &end, 16);
			if (*end != '\0' || errno) {  /* [한국어] hex 실패 시 그룹 이름으로 시도. */
				tpoint_group_mask = spdk_trace_create_tpoint_group_mask(tpoint_group_str);
				if (tpoint_group_mask == 0) {
					error_found = true;
					break;
				}
			}
			tpoint_mask = -1ULL;  /* [한국어] 그룹 내 모든 tpoint 활성. */
		}

		for (group_id = 0; group_id < SPDK_TRACE_MAX_GROUP_ID; ++group_id) {
			/* [한국어] 비트가 셋된 그룹마다 tpoint_mask로 활성화 — 사실상 한 그룹만 셋되어 있어야 유효. */
			if (tpoint_group_mask & (1 << group_id)) {
				spdk_trace_set_tpoints(group_id, tpoint_mask);
			}
		}
	}

	if (error_found) {  /* [한국어] 파싱 중 한 번이라도 실패했으면 전체 실패 처리(부분 적용된 tpoint는 그대로 두고 종료). */
		SPDK_ERRLOG("invalid tpoint mask %s\n", opts->tpoint_group_mask);
		free(tp_g_str);
		return -1;
	} else {
		SPDK_NOTICELOG("Tracepoint Group Mask %s specified.\n", opts->tpoint_group_mask);
		/* [한국어] 사용자가 spdk_trace 도구로 어떻게 캡처할지 친절하게 안내 — shm_id가 있으면 -i, 없으면 -p로 pid 사용. */
		SPDK_NOTICELOG("Use 'spdk_trace -s %s %s %d' to capture a snapshot of events at runtime.\n",
			       opts->name,
			       opts->shm_id >= 0 ? "-i" : "-p",
			       opts->shm_id >= 0 ? opts->shm_id : getpid());
#if defined(__linux__)
		/* [한국어] Linux에서만 /dev/shm가 디렉토리로 노출되므로 직접 cp 가능 — 안내 문자열에 포함. */
		SPDK_NOTICELOG("'spdk_trace' without parameters will also work if this is the only\n");
		SPDK_NOTICELOG("SPDK application currently running.\n");
		SPDK_NOTICELOG("Or copy /dev/shm%s for offline analysis/debug.\n", shm_name);
#endif
	}
	free(tp_g_str);  /* [한국어] strdup된 작업 사본 해제. */

	return 0;
}

/*
 * [한국어]
 * bootstrap_fn - app_thread에서 처음 실행되는 부트스트랩 진입점.
 *
 * @arg1: 미사용. spdk_thread_send_msg 콜백 시그니처 호환.
 *
 * spdk_app_start의 거의 마지막에서 spdk_thread_send_msg로 app_thread에 enqueue된다. 즉
 * 이 함수가 처음 실행되는 시점에 reactor가 이미 폴링을 시작했고, app_thread도 살아 있다.
 * 역할: STARTUP 단계 RPC들(예: bdev_set_options 등 init 전에 미리 적용해야 하는 항목)을
 *       JSON config에서 적용하고, 완료되면 app_do_spdk_subsystem_init 호출.
 *       JSON config가 없으면 즉시 app_do_spdk_subsystem_init 호출.
 *
 * 호출 체인:
 *   spdk_app_start → spdk_thread_send_msg(app_thread, bootstrap_fn) → [bootstrap_fn(app_thread)]
 *     → spdk_subsystem_load_config / app_do_spdk_subsystem_init
 */
static void
bootstrap_fn(void *arg1)
{
	spdk_rpc_set_allowlist(g_spdk_app.rpc_allowlist);  /* [한국어] STARTUP 단계에서도 화이트리스트 적용 — 외부 RPC가 들어와도 차단됨. */

	if (g_spdk_app.json_data) {
		/* Load SPDK_RPC_STARTUP RPCs from config file */
		assert(spdk_rpc_get_state() == SPDK_RPC_STARTUP);  /* [한국어] 부팅 직후이므로 RPC 상태는 STARTUP이어야 함. */
		/* [한국어] config 파일에서 STARTUP 단계 RPC들만 우선 실행. 완료 콜백은 app_do_spdk_subsystem_init. */
		spdk_subsystem_load_config(g_spdk_app.json_data, g_spdk_app.json_data_size,
					   app_do_spdk_subsystem_init, NULL,
					   !g_spdk_app.json_config_ignore_errors);
	} else {
		app_do_spdk_subsystem_init(0, NULL);  /* [한국어] config 없음 — 곧장 RPC 서버 부트 + subsystem_init. */
	}
}

/*
 * [한국어]
 * app_copy_opts - 사용자 opts(opts_user)를 SPDK 내부 opts(opts_local)로 ABI 안전하게 복사.
 *
 * @opts: out — 채울 내부 opts(spdk_app_opts_init로 디폴트가 먼저 채워진 뒤 사용자 값으로 덮임).
 * @opts_user: in — 사용자가 spdk_app_start에 전달한 opts.
 * @opts_size: 사용자 측 opts 구조체 크기(opts_user->opts_size). 더 큰 사용자 측 또는 더 작은 사용자 측을 모두 안전 처리.
 *
 * SPDK가 ABI 호환성을 유지하기 위한 핵심 메커니즘. SPDK 라이브러리 측의 sizeof(struct spdk_app_opts)와
 * 사용자 컴파일 시점의 sizeof가 다를 수 있어, 매 필드의 (offset+size)가 사용자가 알리는 opts_size 안에
 * 들어갈 때만 복사한다. 끝의 SPDK_STATIC_ASSERT는 새 필드가 추가될 때 개발자가 SET_FIELD 추가를
 * 잊지 않도록 강제하는 컴파일 타임 체크.
 *
 * 호출 체인:
 *   spdk_app_start → [app_copy_opts] → 이후 모든 부팅 단계는 opts_local만 참조
 */
static void
app_copy_opts(struct spdk_app_opts *opts, struct spdk_app_opts *opts_user, size_t opts_size)
{
	spdk_app_opts_init(opts, sizeof(*opts));  /* [한국어] 내부 opts에 디폴트값을 먼저 채움(공개 API 재사용). */
	opts->opts_size = opts_size;              /* [한국어] 이후 SET_FIELD가 사용자 opts_size 기준으로 OOB 검사하도록 덮어씀. */

	/* [한국어] SET_FIELD 매크로: 사용자가 알리는 opts_size 안에 해당 필드가 들어갈 때만 1:1 복사.
	 *         사용자가 더 오래된 SPDK 헤더를 썼더라도 OOB 읽기를 일으키지 않도록 보호. */
#define SET_FIELD(field) \
        if (offsetof(struct spdk_app_opts, field) + sizeof(opts->field) <= (opts->opts_size)) { \
		opts->field = opts_user->field; \
	} \

	SET_FIELD(name);                /* [한국어] 앱 이름 — DPDK program name, 트레이스 SHM, 로그 prefix에 쓰임. */
	SET_FIELD(json_config_file);    /* [한국어] -c/--json/--config 인자로 들어온 JSON config 파일 경로. */
	SET_FIELD(json_config_ignore_errors);  /* [한국어] init 오류 무시 플래그. */
	SET_FIELD(rpc_addr);                   /* [한국어] -r/--rpc-socket 경로 또는 NULL(--no-rpc-server). */
	SET_FIELD(reactor_mask);               /* [한국어] -m hex/리스트 마스크. */
	SET_FIELD(lcore_map);                  /* [한국어] --lcores 매핑 문자열. */
	SET_FIELD(tpoint_group_mask);          /* [한국어] -e tpoint 마스크 문자열. */
	SET_FIELD(shm_id);                     /* [한국어] -i SHM ID. */
	SET_FIELD(shutdown_cb);                /* [한국어] graceful shutdown 콜백. */
	SET_FIELD(enable_coredump);            /* [한국어] -d로 false 가능. */
	SET_FIELD(mem_channel);                /* [한국어] -n. */
	SET_FIELD(main_core);                  /* [한국어] -p. */
	SET_FIELD(mem_size);                   /* [한국어] -s (MB). */
	SET_FIELD(no_pci);                     /* [한국어] -u. */
	SET_FIELD(hugepage_single_segments);   /* [한국어] -g. */
	SET_FIELD(unlink_hugepage);            /* [한국어] -R. */
	SET_FIELD(no_huge);                    /* [한국어] --no-huge. */
	SET_FIELD(hugedir);                    /* [한국어] --huge-dir. */
	SET_FIELD(print_level);                /* [한국어] stderr 출력 임계 — --silence-noticelog로 변경. */
	SET_FIELD(num_pci_addr);               /* [한국어] pci_blocked/allowed 배열 길이. */
	SET_FIELD(pci_blocked);                /* [한국어] -B BDF 차단 배열. */
	SET_FIELD(pci_allowed);                /* [한국어] -A BDF 허용 배열. */
	SET_FIELD(iova_mode);                  /* [한국어] --iova-mode. */
	SET_FIELD(delay_subsystem_init);       /* [한국어] --wait-for-rpc. */
	SET_FIELD(num_entries);                /* [한국어] --num-trace-entries. */
	SET_FIELD(num_trace_threads);          /* [한국어] thread별 트레이스 슬롯 추가 할당량. */
	SET_FIELD(env_context);                /* [한국어] --env-context. */
	SET_FIELD(log);                        /* [한국어] spdk_log_open에 넘길 사용자 정의 로그 콜백 셋. */
	SET_FIELD(base_virtaddr);              /* [한국어] --base-virtaddr. */
	SET_FIELD(disable_signal_handlers);    /* [한국어] true면 SIGINT/SIGTERM 자체 처리 안 함(외부에서 처리). */
	SET_FIELD(interrupt_mode);             /* [한국어] --interrupt-mode. */
	SET_FIELD(enforce_numa);               /* [한국어] --enforce-numa. */
	SET_FIELD(msg_mempool_size);           /* [한국어] 메시지 풀 슬롯 수(0이면 자동 산정). */
	SET_FIELD(rpc_allowlist);              /* [한국어] --rpcs-allowed로 만든 화이트리스트. */
	SET_FIELD(vf_token);                   /* [한국어] --vfio-vf-token. */
	SET_FIELD(rpc_log_file);               /* [한국어] RPC 호출 로그 파일. */
	SET_FIELD(rpc_log_level);              /* [한국어] RPC 호출 로그 임계. */
	SET_FIELD(json_data);                  /* [한국어] 파일 대신 in-memory JSON config 버퍼. */
	SET_FIELD(json_data_size);             /* [한국어] json_data 길이. */
	SET_FIELD(disable_cpumask_locks);      /* [한국어] --disable-cpumask-locks. */

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	/* [한국어] 컴파일 타임 가드 — sizeof(spdk_app_opts)가 변하면(=새 필드가 추가됐다면)
	 *         SET_FIELD도 함께 추가하라는 강제 신호. 단순히 253→260 등으로 고치기만 하면 안 되고
	 *         반드시 위 SET_FIELD 목록도 갱신해야 한다. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_app_opts) == 253, "Incorrect size");

#undef SET_FIELD
}

/*
 * [한국어]
 * unclaim_cpu_cores - 본 프로세스가 점유했던 코어 락 파일들을 close + unlink.
 *
 * @failed_core: 실패한 코어 번호를 기록할 out 파라미터(NULL 가능).
 * @return: 0=전체 성공, -1=실패(첫 실패 시점에서 즉시 반환).
 *
 * spdk_app_fini와 framework_disable_cpumask_locks RPC에서 호출. 락 파일은 단순 close만으로도
 * fcntl 락이 풀리지만 명시적으로 unlink까지 해 다른 프로세스가 같은 경로를 재사용하기 쉽게 한다.
 * /var/tmp/spdk_cpu_lock_NNN 형식 파일은 다중 SPDK 프로세스 간 코어 충돌 방지에 사용된다.
 *
 * 호출 체인:
 *   spdk_app_fini / rpc_framework_disable_cpumask_locks → [unclaim_cpu_cores] → libc(close, unlink)
 */
static int
unclaim_cpu_cores(uint32_t *failed_core)
{
	char core_name[40];  /* [한국어] /var/tmp/spdk_cpu_lock_NNN 경로 합성 버퍼. */
	uint32_t i;
	int rc;

	for (i = 0; i < SPDK_CONFIG_MAX_LCORES; i++) {  /* [한국어] 모든 가능한 코어 슬롯을 순회. */
		if (g_core_locks[i] != -1 && g_core_locks[i] != 0) {  /* [한국어] -1=미점유, 0=초기화 안 된 슬롯 — 실제 fd는 양수. */
			snprintf(core_name, sizeof(core_name), "/var/tmp/spdk_cpu_lock_%03d", i);
			rc = close(g_core_locks[i]);  /* [한국어] close가 fcntl 락도 함께 해제. */
			if (rc) {
				SPDK_ERRLOG("Failed to close lock fd for core %d, errno: %d\n", i, errno);
				goto error;
			}

			g_core_locks[i] = -1;  /* [한국어] 슬롯을 "비어 있음"으로 표시 — 재진입 안전성. */
			rc = unlink(core_name);  /* [한국어] 파일도 제거 — 잔여 파일이 다른 프로세스에 혼란 주지 않게. */
			if (rc) {
				SPDK_ERRLOG("Failed to unlink lock fd for core %d, errno: %d\n", i, errno);
				goto error;
			}
		}
	}

	return 0;

error:
	if (failed_core != NULL) {
		/* Set number of core we failed to claim. */
		*failed_core = i;  /* [한국어] 호출자가 사용자에게 어떤 코어에서 실패했는지 보고할 수 있게 설정. */
	}
	return -1;
}

/*
 * [한국어]
 * claim_cpu_cores - 본 프로세스가 사용할 lcore마다 /var/tmp/spdk_cpu_lock_NNN 락 파일 생성/잠금.
 *
 * @failed_core: 실패 시 어떤 코어에서 실패했는지 기록할 out 파라미터(NULL 가능).
 * @return: 0=모든 코어 잠금 성공, -1=어딘가에서 실패(이 경우 이미 잡은 락은 unclaim_cpu_cores로 풀림).
 *
 * 다중 SPDK 프로세스가 같은 lcore를 폴링하면 둘 다 100% CPU를 먹어 의미가 없으므로
 * 시작 시점에 코어별로 advisory file lock(fcntl F_SETLK, F_WRLCK)을 잡는다. 다른 프로세스가
 * 같은 코어를 시도하면 그 프로세스의 PID를 mmap된 락 파일에서 읽어 "누가 점유 중인지"
 * 사용자에게 안내한다. close/unlink는 unclaim_cpu_cores가 담당.
 *
 * 호출 체인:
 *   spdk_app_start / rpc_framework_enable_cpumask_locks → [claim_cpu_cores]
 *     → libc(open/ftruncate/mmap/fcntl)
 */
static int
claim_cpu_cores(uint32_t *failed_core)
{
	char core_name[40];           /* [한국어] /var/tmp/spdk_cpu_lock_NNN 합성 버퍼. */
	int core_fd, pid;             /* [한국어] core_fd: 새로 연 fd, pid: 락 보유 중인 다른 프로세스 PID. */
	int *core_map;                /* [한국어] 락 파일을 mmap한 4바이트 영역 — PID 저장용. */
	uint32_t core;

	struct flock core_lock = {    /* [한국어] fcntl(F_SETLK)에 넘길 락 명세 — 파일 전체에 write lock. */
		.l_type = F_WRLCK,    /* [한국어] write 락(=배타적). 같은 fd가 자기 자신과 충돌하진 않음. */
		.l_whence = SEEK_SET, /* [한국어] l_start 기준점 = 파일 시작. */
		.l_start = 0,         /* [한국어] 잠금 영역 시작 오프셋 0. */
		.l_len = 0,           /* [한국어] len=0은 EOF까지 = 파일 전체 잠금이라는 POSIX 관용. */
	};

	SPDK_ENV_FOREACH_CORE(core) {  /* [한국어] DPDK가 활성화한 lcore만 순회 — 인덱스 비연속 가능. */
		if (g_core_locks[core] != -1) {
			/* If this core is locked already, do not try lock it again. */
			continue;  /* [한국어] 이미 본 프로세스가 잡고 있으면 건너뜀(idempotent). */
		}

		snprintf(core_name, sizeof(core_name), "/var/tmp/spdk_cpu_lock_%03d", core);
		core_fd = open(core_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);  /* [한국어] 없으면 생성, 0600 권한. */
		if (core_fd == -1) {
			SPDK_ERRLOG("Could not open %s (%s).\n", core_name, spdk_strerror(errno));
			/* Return number of core we failed to claim. */
			goto error;
		}

		if (ftruncate(core_fd, sizeof(int)) != 0) {  /* [한국어] PID 한 개를 담을 수 있도록 4바이트 확보. */
			SPDK_ERRLOG("Could not truncate %s (%s).\n", core_name, spdk_strerror(errno));
			close(core_fd);
			goto error;
		}

		core_map = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, core_fd, 0);
		/* [한국어] 락 파일을 4바이트로 mmap — 다른 프로세스가 우리 PID를 읽거나 우리가 그들의 PID를 읽기 위함. */
		if (core_map == MAP_FAILED) {
			SPDK_ERRLOG("Could not mmap core %s (%s).\n", core_name, spdk_strerror(errno));
			close(core_fd);
			goto error;
		}

		if (fcntl(core_fd, F_SETLK, &core_lock) != 0) {
			/* [한국어] 다른 프로세스가 이미 락 보유 중 — errno=EAGAIN/EACCES. mmap에 쓰인 PID로 누군지 보고. */
			pid = *core_map;
			SPDK_ERRLOG("Cannot create lock on core %" PRIu32 ", probably process %d has claimed it.\n",
				    core, pid);
			munmap(core_map, sizeof(int));
			close(core_fd);
			goto error;
		}

		/* We write the PID to the core lock file so that other processes trying
		* to claim the same core will know what process is holding the lock. */
		*core_map = (int)getpid();      /* [한국어] 자신의 PID를 락 파일에 기록 — 다른 프로세스가 진단용으로 읽음. */
		munmap(core_map, sizeof(int));  /* [한국어] 더 이상 매핑 필요 없음(쓰기 완료). 락은 fd가 닫힐 때까지 유효. */
		g_core_locks[core] = core_fd;   /* [한국어] fd를 보관 — fd가 살아 있는 동안 fcntl 락이 유지된다. */
		/* Keep core_fd open to maintain the lock. */
	}

	return 0;

error:
	if (failed_core != NULL) {
		/* Set number of core we failed to claim. */
		*failed_core = core;  /* [한국어] 사용자에게 실패 위치 보고. */
	}
	unclaim_cpu_cores(NULL);  /* [한국어] 부분적으로 잡은 락들을 모두 해제(롤백). */
	return -1;
}

/*
 * [한국어]
 * spdk_app_start - SPDK 애플리케이션의 메인 부트스트랩. main()이 호출하는 블로킹 함수.
 *
 * @opts_user: 사용자가 spdk_app_opts_init + 커스터마이즈한 옵션. NULL/비정상 검사 후 내부로 복사.
 * @start_fn: subsystem init 후 app_thread에서 실행할 사용자 진입 콜백.
 * @arg1: start_fn에 전달할 사용자 컨텍스트.
 * @return: 종료 코드(spdk_app_stop 호출 시 인자로 전달된 값) — main이 그대로 exit code로 사용.
 *
 * 부팅 시퀀스(요약):
 *   (1) opts 검증/내부 사본 생성  (2) DPDK env init (hugepage/PCI 매핑)
 *   (3) calculate_mempool_size + spdk_log_open  (4) /var/tmp/spdk_cpu_lock 락 점유
 *   (5) spdk_reactors_init (코어당 reactor)  (6) trace SHM 생성
 *   (7) "app_thread" SPDK thread 생성  (8) /proc/stat baseline 캡처
 *   (9) signal 핸들러 설치  (10) JSON config 로드  (11) bootstrap_fn enqueue
 *   (12) spdk_reactors_start() — spdk_app_stop까지 블로킹 실행
 *   (13) g_env_was_setup=true (재진입 대비)
 * 호출 컨텍스트: main 스레드. 진입 후엔 reactor가 폴링을 시작하고 app_thread에서 부팅 후속 작업이 진행.
 *
 * 호출 체인:
 *   user main() → [spdk_app_start] → app_setup_env / spdk_reactors_init / bootstrap_fn
 *     → spdk_reactors_start (block) → ... → spdk_app_stop → return
 */
int
spdk_app_start(struct spdk_app_opts *opts_user, spdk_msg_fn start_fn,
	       void *arg1)
{
	int			rc;
	char			*tty;                          /* [한국어] stderr가 tty인지 확인할 때 사용. */
	struct spdk_cpuset	tmp_cpumask = {};              /* [한국어] app_thread를 어느 코어에 묶을지 결정할 비트셋. */
	static bool		g_env_was_setup = false;       /* [한국어] 같은 프로세스에서 spdk_app_start가 두 번째 호출되는지 표식 — env_init의 재초기화 경로 분기. */
	struct spdk_app_opts opts_local = {};                  /* [한국어] 사용자 opts의 내부 사본 — ABI 안전 복사 대상. */
	struct spdk_app_opts *opts = &opts_local;              /* [한국어] 함수 전반에서 사용할 단일 포인터 별칭. */
	uint32_t i, core;                                      /* [한국어] i: g_core_locks 초기화 루프, core: SPDK_ENV_FOREACH_CORE. */

	if (!opts_user) {  /* [한국어] 사용자 opts 누락 — 부팅 거부. */
		SPDK_ERRLOG("opts_user should not be NULL\n");
		return 1;
	}

	if (!opts_user->opts_size) {  /* [한국어] ABI 검사용 opts_size가 0이면 SET_FIELD가 모든 필드를 OOB 판정 — 무의미. */
		SPDK_ERRLOG("The opts_size in opts_user structure should not be zero value\n");
		return 1;
	}

	if (opts_user->name == NULL) {  /* [한국어] DPDK program name으로 필요 — NULL이면 부팅 거부. */
		SPDK_ERRLOG("spdk_app_opts::name not specified\n");
		return 1;
	}

	app_copy_opts(opts, opts_user, opts_user->opts_size);  /* [한국어] 사용자 opts → 내부 opts 사본(ABI 안전). 이후 함수는 opts만 사용. */

	if (!start_fn) {  /* [한국어] 사용자 진입점 누락 — 의미 없는 부팅. */
		SPDK_ERRLOG("start_fn should not be NULL\n");
		return 1;
	}

	if (!opts->rpc_addr && opts->delay_subsystem_init) {
		/* [한국어] --wait-for-rpc는 RPC로 framework_start_init를 받아 부팅을 진행하는 모드.
		 *         RPC 서버가 없으면 영원히 대기 → 모순된 조합. */
		SPDK_ERRLOG("Cannot use '--wait-for-rpc' if no RPC server is going to be started.\n");
		return 1;
	}

	if (!(opts->lcore_map || opts->reactor_mask)) {
		/* Set default CPU mask */
		opts->reactor_mask = SPDK_APP_DPDK_DEFAULT_CORE_MASK;  /* [한국어] -m / --lcores 둘 다 미지정 → 기본 0x1(코어 0). */
	}

	tty = ttyname(STDERR_FILENO);  /* [한국어] stderr 디바이스 경로 조회 — /dev/tty 계열 여부 판단용. */
	if (opts->print_level > SPDK_LOG_WARN &&
	    isatty(STDERR_FILENO) &&
	    tty &&
	    !strncmp(tty, "/dev/tty", strlen("/dev/tty"))) {
		/* [한국어] 사용자가 INFO/NOTICE 출력을 켠 채로 콘솔 tty에 stderr를 그대로 두면 화면이 매우 시끄럽다.
		 *         의도치 않은 상태일 가능성이 높아 10초 지연 + 경고 출력. */
		printf("Warning: printing stderr to console terminal without -q option specified.\n");
		printf("Suggest using --silence-noticelog to disable logging to stderr and\n");
		printf("monitor syslog, or redirect stderr to a file.\n");
		printf("(Delaying for 10 seconds...)\n");
		sleep(10);
	}

	spdk_log_set_print_level(opts->print_level);  /* [한국어] stderr 인쇄 임계 적용. */

#ifndef SPDK_NO_RLIMIT
	if (opts->enable_coredump) {
		struct rlimit core_limits;  /* [한국어] setrlimit 인자 — soft/hard 동시 설정. */

		core_limits.rlim_cur = core_limits.rlim_max = SPDK_APP_DEFAULT_CORE_LIMIT;  /* [한국어] 5GiB 캡. */
		setrlimit(RLIMIT_CORE, &core_limits);  /* [한국어] 코어 덤프 최대 크기 설정 — 비정상 종료 시 디버그 도움. */
	}
#endif

	if (opts->interrupt_mode) {
		spdk_interrupt_mode_enable();  /* [한국어] poller 구현체에 인터럽트 모드 우선 사용 신호 — eventfd/uintr 기반 wakeup. */
	}

	memset(&g_spdk_app, 0, sizeof(g_spdk_app));  /* [한국어] 전역 라이프사이클 상태 0-clear — 재진입 대응. */

	/* [한국어] opts → g_spdk_app 매핑(부팅 후 다른 함수가 참조할 필드만 골라 복사). */
	g_spdk_app.json_config_ignore_errors = opts->json_config_ignore_errors;
	g_spdk_app.rpc_addr = opts->rpc_addr;
	g_spdk_app.rpc_allowlist = opts->rpc_allowlist;
	g_spdk_app.rpc_log_file = opts->rpc_log_file;
	g_spdk_app.rpc_log_level = opts->rpc_log_level;
	g_spdk_app.shm_id = opts->shm_id;
	g_spdk_app.shutdown_cb = opts->shutdown_cb;
	g_spdk_app.rc = 0;
	g_spdk_app.stopped = false;

	spdk_log_set_level(SPDK_APP_DEFAULT_LOG_LEVEL);  /* [한국어] 내부 로그 임계 NOTICE 고정 — print_level과는 별개. */

	/* Pass NULL to app_setup_env if SPDK app has been set up, in order to
	 * indicate that this is a reinitialization.
	 */
	if (app_setup_env(g_env_was_setup ? NULL : opts) < 0) {
		/* [한국어] 첫 호출이면 정상 init, 두 번째 호출이면 NULL로 reinit 경로. 실패 시 부팅 abort. */
		return 1;
	}

	/* Calculate mempool size now that the env layer has configured the core count
	 * for the application */
	/* [한국어] 메시지 풀 크기는 활성 코어 수에 의존 → env_init 이후로 미뤄야 옳다. */
	calculate_mempool_size(opts, opts_user);

	spdk_log_open(opts->log);  /* [한국어] syslog/사용자 콜백 채널 활성화 — 이 시점부터 SPDK_*LOG가 본격 동작. */

	/* Initialize each lock to -1 to indicate "empty" status */
	for (i = 0; i < SPDK_CONFIG_MAX_LCORES; i++) {
		g_core_locks[i] = -1;  /* [한국어] -1 = "잠금 미보유" 표식. claim_cpu_cores가 양수 fd로 덮어씀. */
	}

	if (!opts->disable_cpumask_locks) {
		if (claim_cpu_cores(NULL)) {  /* [한국어] /var/tmp/spdk_cpu_lock_NNN 잠금 시도 — 다중 SPDK 인스턴스 충돌 방지. */
			SPDK_ERRLOG("Unable to acquire lock on assigned core mask - exiting.\n");
			return 1;
		}
	} else {
		SPDK_NOTICELOG("CPU core locks deactivated.\n");  /* [한국어] 컨테이너/CI 환경에서 락 파일 권한이 막힐 때 우회. */
	}

	SPDK_NOTICELOG("Total cores available: %d\n", spdk_env_get_core_count());  /* [한국어] 사용자 진단용 정보 — DPDK가 결정한 활성 lcore 수. */

	if ((rc = spdk_reactors_init(opts->msg_mempool_size)) != 0) {
		/* [한국어] lib/event/reactor.c가 코어당 reactor 구조체와 메시지 풀을 할당. 실패 시 OOM/PCI/NUMA 문제 가능. */
		SPDK_ERRLOG("Reactor Initialization failed: rc = %d\n", rc);
		return 1;
	}

	spdk_cpuset_set_cpu(&tmp_cpumask, spdk_env_get_current_core(), true);
	/* [한국어] app_thread를 현재(=메인) 코어에 고정 배치. 이렇게 하면 부팅 동안 컨텍스트 스위치 비용 최소화. */

	/*
	 * Disable and ignore trace setup if setting num_entries
	 * to be 0.
	 *
	 * Note the call to spdk_app_setup_trace() is located here
	 * ahead of app_setup_signal_handlers().
	 * That's because there is not an easy/direct clean
	 * way of unwinding alloc'd resources that can occur
	 * in app_setup_signal_handlers().
	 */
	if (opts->num_entries != 0 && spdk_app_setup_trace(opts) != 0) {
		/* [한국어] num_entries=0이면 트레이스 비활성화. 그 외엔 SHM 생성 + tpoint 마스크 적용. */
		return 1;
	}

	/* Now that the reactors have been initialized, we can create the app thread. */
	spdk_thread_create("app_thread", &tmp_cpumask);  /* [한국어] reactor가 폴 사이클에서 발견할 첫 SPDK thread 생성 — 부팅 후속 작업의 무대. */
	if (!spdk_thread_get_app_thread()) {  /* [한국어] thread 라이브러리 내부에서 "첫 thread"가 app_thread로 마킹되었는지 확인. */
		SPDK_ERRLOG("Unable to create an spdk_thread for initialization\n");
		return 1;
	}

	SPDK_ENV_FOREACH_CORE(core) {
		rc = init_proc_stat(core);  /* [한국어] 활성 코어마다 baseline 통계 캡처. 실패는 치명적이지 않으니 NOTICE만. */
		if (rc) {
			SPDK_NOTICELOG("Unable to parse CPU statistics [core: %d].\n", core);
		}
	}

	if (!opts->disable_signal_handlers && app_setup_signal_handlers(opts) != 0) {
		/* [한국어] 사용자가 disable=true로 두면(예: 테스트 러너) 전혀 건드리지 않음. */
		return 1;
	}

	g_delay_subsystem_init = opts->delay_subsystem_init;  /* [한국어] --wait-for-rpc 모드를 부팅 단계 함수가 참조할 수 있도록 전역에 반영. */
	g_start_fn = start_fn;                                /* [한국어] 사용자 진입점 — app_start_application에서 호출됨. */
	g_start_arg = arg1;                                   /* [한국어] 사용자 컨텍스트. */

	if (opts->json_config_file != NULL) {
		if (opts->json_data) {
			/* [한국어] 파일 경로와 in-memory 데이터를 동시에 줄 수는 없음(혼동 방지). */
			SPDK_ERRLOG("App opts json_config_file and json_data are mutually exclusive\n");
			return 1;
		}

		g_spdk_app.json_data = spdk_posix_file_load_from_name(opts->json_config_file,
				       &g_spdk_app.json_data_size);
		/* [한국어] 파일 전체를 한 번에 메모리로 적재 — bootstrap_fn이 lib/init에 그대로 넘김. */
		if (!g_spdk_app.json_data) {
			SPDK_ERRLOG("Read JSON configuration file %s failed: %s\n",
				    opts->json_config_file, spdk_strerror(errno));
			return 1;
		}
	} else if (opts->json_data) {
		g_spdk_app.json_data = calloc(1, opts->json_data_size);  /* [한국어] 사용자 버퍼를 우리가 관리하는 사본으로 복사 — 사용자 측 lifecycle 분리. */
		if (!g_spdk_app.json_data) {
			SPDK_ERRLOG("Failed to allocate JSON data buffer\n");
			return 1;
		}

		memcpy(g_spdk_app.json_data, opts->json_data, opts->json_data_size);
		g_spdk_app.json_data_size = opts->json_data_size;
	}

	spdk_thread_send_msg(spdk_thread_get_app_thread(), bootstrap_fn, NULL);
	/* [한국어] app_thread의 메시지 큐에 bootstrap_fn을 넣음. reactor가 폴 사이클에서 이를 dequeue 후 실행 → 부팅 후속 시퀀스. */

	/* This blocks until spdk_app_stop is called */
	spdk_reactors_start();
	/* [한국어] 메인 코어가 reactor 폴링 루프 진입. 다른 코어들도 각자 reactor를 시작.
	 *         spdk_app_stop이 호출되면 spdk_reactors_stop이 트리거되고 폴링이 종료되어 여기서 리턴된다. */

	g_env_was_setup = true;  /* [한국어] 다음 호출(같은 프로세스에서 spdk_app_start 재호출)에선 env reinit 경로로 진입. */

	return g_spdk_app.rc;  /* [한국어] app_stop이 마지막에 셋한 종료 코드를 main에 반환. */
}

/*
 * [한국어]
 * spdk_app_fini - main()이 spdk_app_start return 후 호출하는 최종 정리 함수.
 *
 * 트레이스 SHM 정리 → reactor 자료구조 free → DPDK env 종료 → 로그 close →
 * /var/tmp/spdk_cpu_lock_NNN 락 파일 close + unlink. 호출 시점에는 이미
 * spdk_reactors_start가 리턴된 상태여야 하며, app_thread 등 SPDK thread는 이미 정리되어 있다.
 *
 * 호출 체인:
 *   user main() → [spdk_app_fini] → spdk_trace_cleanup / spdk_reactors_fini / spdk_env_fini /
 *                                   spdk_log_close / unclaim_cpu_cores
 */
void
spdk_app_fini(void)
{
	spdk_trace_cleanup();      /* [한국어] /dev/shm 트레이스 파일 munmap + close. */
	spdk_reactors_fini();      /* [한국어] reactor 메시지 풀, poller 슬롯, scheduler 자원 해제. */
	spdk_env_fini();           /* [한국어] DPDK rte_eal_cleanup — hugepage 매핑 해제(다음 부팅 위해). */
	spdk_log_close();          /* [한국어] syslog/사용자 로그 채널 닫기. */
	unclaim_cpu_cores(NULL);   /* [한국어] 코어 락 파일 close+unlink. NULL=실패 코어 보고 받지 않음. */
}

/*
 * [한국어]
 * subsystem_fini_done - 모든 subsystem_fini 콜백이 끝났을 때 호출되는 종료 단계 콜백.
 *
 * @arg1: 미사용.
 *
 * RPC 서버를 종료해 외부 클라이언트 접속을 끊고, reactor 폴링 루프 정지 신호를 보낸다.
 * spdk_reactors_stop이 비동기로 모든 reactor에 stop 메시지를 전파하면 결국
 * spdk_reactors_start (메인 스레드 블록 지점)가 return해 spdk_app_start로 빠져나간다.
 *
 * 호출 체인:
 *   _start_subsystem_fini → spdk_subsystem_fini → [subsystem_fini_done(app_thread)]
 *     → spdk_rpc_finish / spdk_reactors_stop
 */
static void
subsystem_fini_done(void *arg1)
{
	spdk_rpc_finish();          /* [한국어] RPC 서버 listen socket close + 핸들러 테이블 해제. */
	spdk_reactors_stop(NULL);   /* [한국어] reactor에 stop 메시지 전파 — 이후 spdk_reactors_start가 return. */
}

/*
 * [한국어]
 * _start_subsystem_fini - subsystem_fini를 시작하기 전에 스케줄링이 진행 중이지 않은지 확인.
 *
 * @arg1: 미사용.
 *
 * SPDK 스케줄러가 reactor/poller 재배치 중이면 spdk_subsystem_fini를 호출해도 안전하지 않다.
 * 이 경우 자기 자신을 다시 app_thread 큐에 enqueue해 다음 폴 사이클에 재시도한다(busy-wait 형태).
 *
 * 호출 체인:
 *   app_stop → [_start_subsystem_fini(app_thread)] → spdk_subsystem_fini
 */
static void
_start_subsystem_fini(void *arg1)
{
	if (g_scheduling_in_progress) {
		/* [한국어] 스케줄러 작업 중 — 한 사이클 더 기다린 뒤 재시도. send_msg는 lockless이므로 무한 루프가 되어도 부담 작음. */
		spdk_thread_send_msg(spdk_thread_get_app_thread(), _start_subsystem_fini, NULL);
		return;
	}

	spdk_subsystem_fini(subsystem_fini_done, NULL);  /* [한국어] 모든 subsystem을 의존성 역순으로 fini → 끝나면 subsystem_fini_done 호출. */
}

/*
 * [한국어]
 * log_deprecation_hits - 종료 시 deprecation 카운터를 사용자에게 경고로 출력.
 *
 * @ctx: 미사용. @dep: 등록된 deprecation 항목.
 * @return: 0=계속 순회.
 *
 * spdk_log_for_each_deprecation의 반복 콜백. 한 번이라도 deprecated API가 호출됐다면
 * WARN 로그를 남긴다. 사용자/운영자가 미래 SPDK 버전에서 깨질 코드를 알 수 있도록 함.
 *
 * 호출 체인:
 *   app_stop → spdk_log_for_each_deprecation → [log_deprecation_hits]
 */
static int
log_deprecation_hits(void *ctx, struct spdk_deprecation *dep)
{
	uint64_t hits = spdk_deprecation_get_hits(dep);  /* [한국어] 이 deprecation이 런타임에 호출된 누적 횟수. */

	if (hits == 0) {
		return 0;  /* [한국어] 한 번도 안 쓴 deprecation은 보고 안 함(노이즈 감소). */
	}

	SPDK_WARNLOG("%s: deprecation '%s' scheduled for removal in %s hit %" PRIu64 " times\n",
		     spdk_deprecation_get_tag(dep), spdk_deprecation_get_description(dep),
		     spdk_deprecation_get_remove_release(dep), hits);
	return 0;
}

/*
 * [한국어]
 * app_stop - app_thread에서 실행되는 실제 종료 디스패처.
 *
 * @arg1: spdk_app_stop이 보낸 종료 코드(intptr_t로 인코딩).
 *
 * 멱등성 보장(stopped 플래그), 종료 코드 누적(첫 0이 아닌 값을 우선), JSON 데이터 free,
 * deprecation 보고, _start_subsystem_fini 트리거 순으로 진행된다.
 *
 * 호출 체인:
 *   spdk_app_stop → spdk_thread_send_msg → [app_stop(app_thread)]
 *     → _start_subsystem_fini → spdk_subsystem_fini → subsystem_fini_done → spdk_reactors_stop
 */
static void
app_stop(void *arg1)
{
	if (g_spdk_app.rc == 0) {
		g_spdk_app.rc = (int)(intptr_t)arg1;  /* [한국어] 첫 비-제로 rc 보존: 한 번 에러로 종료된 사실을 후속 호출이 덮지 못하게 함. */
	}

	if (g_spdk_app.stopped) {  /* [한국어] 이미 종료 중이면 즉시 return — 두 번째 SIGINT 등 중복 호출 방어. */
		SPDK_NOTICELOG("spdk_app_stop called twice\n");
		return;
	}

	free(g_spdk_app.json_data);  /* [한국어] config가 다 안 쓰였더라도 메모리 회수(예외 경로). NULL이면 free는 no-op. */

	g_spdk_app.stopped = true;   /* [한국어] 멱등성 플래그 셋. */
	spdk_log_for_each_deprecation(NULL, log_deprecation_hits);  /* [한국어] deprecation 사용 통계 출력. */
	_start_subsystem_fini(NULL);  /* [한국어] subsystem 역순 fini 시작. */
}

/*
 * [한국어]
 * spdk_app_stop - 어느 컨텍스트에서든 호출 가능한 외부 종료 API.
 *
 * @rc: 종료 코드. 0=정상 종료, 그 외=오류.
 *
 * 종료 작업은 반드시 subsystem_init이 실행된 동일 thread(=app_thread)에서 수행되어야 하므로
 * 현재 thread가 어느 곳이든 spdk_thread_send_msg로 app_thread에 위임한다.
 *
 * 호출 체인:
 *   external (사용자 코드, RPC 핸들러, app_start_shutdown) → [spdk_app_stop]
 *     → spdk_thread_send_msg → app_stop(app_thread)
 */
void
spdk_app_stop(int rc)
{
	if (rc) {
		SPDK_WARNLOG("spdk_app_stop'd on non-zero\n");  /* [한국어] 비정상 종료 — 디버깅 단서로 stderr에 한 줄 남김. */
	}

	/*
	 * We want to run spdk_subsystem_fini() from the same thread where spdk_subsystem_init()
	 * was called.
	 */
	/* [한국어] rc를 intptr_t로 인코딩해 콜백 인자(void*)로 우회 전달 — 메시지 단위 할당 회피. */
	spdk_thread_send_msg(spdk_thread_get_app_thread(), app_stop, (void *)(intptr_t)rc);
}

/*
 * [한국어]
 * usage_memory_size - usage()의 -s 옵션 디폴트 값 표시 부분만 출력.
 *
 * Linux에서는 -s 미지정 시 DPDK가 동적으로 메모리를 잡으므로 0 표시.
 * 비-Linux(FreeBSD 등)에선 미지정 = "사용 가능 hugepage 전체" 의미가 더 명확해 그렇게 표시.
 *
 * 호출 체인: usage → [usage_memory_size] → printf
 */
static void
usage_memory_size(void)
{
#ifndef __linux__
	if (g_default_opts.mem_size <= 0) {
		printf("all hugepage memory)\n");  /* [한국어] FreeBSD: -1=가용 hugepage 전부. */
	} else
#endif
	{
		/* [한국어] Linux 또는 mem_size>0인 경우. mem_size가 음수이면 0으로 표시(미지정 안내). */
		printf("%dMB)\n", g_default_opts.mem_size >= 0 ? g_default_opts.mem_size : 0);
	}
}

/*
 * [한국어]
 * usage - --help / -h 시 출력되는 전체 옵션 도움말.
 *
 * @app_usage: 앱이 자체 추가 옵션이 있을 때 그 도움말을 출력하는 콜백(NULL 허용).
 *
 * SPDK 표준 옵션은 카테고리(CPU/Configuration/Memory/PCI/Log/Trace/Other)로 묶어 출력하고,
 * 마지막에 앱 고유 옵션을 별도 섹션으로 출력. 출력만 하고 부팅에는 영향을 주지 않음.
 *
 * 호출 체인:
 *   spdk_app_parse_args(case HELP_OPT_IDX) / spdk_app_usage → [usage] → printf / spdk_log_usage / spdk_trace_mask_usage
 */
static void
usage(void (*app_usage)(void))
{
	printf("%s [options]\n", g_executable_name);  /* [한국어] argv[0] 기반 사용법 헤더. */
	/* Keep entries inside categories roughly sorted by frequency of use. */
	printf("\nCPU options:\n");
	printf(" -m, --cpumask <mask or list>    core mask (like 0xF) or core list of '[]' embraced for DPDK\n");
	printf("                                 (like [0,1,10])\n");
	printf("     --lcores <list>       lcore to CPU mapping list. The list is in the format:\n");
	printf("                           <lcores[@CPUs]>[<,lcores[@CPUs]>...]\n");
	printf("                           lcores and cpus list are grouped by '(' and ')', e.g '--lcores \"(5-7)@(10-12)\"'\n");
	printf("                           Within the group, '-' is used for range separator,\n");
	printf("                           ',' is used for single number separator.\n");
	printf("                           '( )' can be omitted for single element group,\n");
	printf("                           '@' can be omitted if cpus and lcores have the same value\n");
	printf("     --disable-cpumask-locks    Disable CPU core lock files.\n");
	printf("     --interrupt-mode      set app to interrupt mode (Warning: CPU usage will be reduced only if all\n");
	printf("                           pollers in the app support interrupt mode)\n");
	printf(" -p, --main-core <id>      main (primary) core for DPDK\n");

	printf("\nConfiguration options:\n");
	printf(" -c, --config, --json  <config>     JSON config file\n");
	printf(" -r, --rpc-socket <path>   RPC listen address (default %s)\n", SPDK_DEFAULT_RPC_ADDR);
	printf("     --no-rpc-server       skip RPC server initialization. This option ignores '--rpc-socket' value.\n");
	printf("     --wait-for-rpc        wait for RPCs to initialize subsystems\n");
	printf("     --rpcs-allowed	   comma-separated list of permitted RPCS\n");
	printf("     --json-ignore-init-errors    don't exit on invalid config entry\n");

	printf("\nMemory options:\n");
	printf("     --iova-mode <pa/va>   set IOVA mode ('pa' for IOVA_PA and 'va' for IOVA_VA)\n");
	printf("     --base-virtaddr <addr>      the base virtual address for DPDK (default: 0x200000000000)\n");
	printf("     --huge-dir <path>     use a specific hugetlbfs mount to reserve memory from\n");
	printf(" -R, --huge-unlink         unlink huge files after initialization\n");
	printf(" -n, --mem-channels <num>  number of memory channels used for DPDK\n");
	printf(" -s, --mem-size <size>     memory size in MB for DPDK (default: ");
	usage_memory_size();
	printf("     --msg-mempool-size <size>  global message memory pool size in count (default: %d)\n",
	       SPDK_DEFAULT_MSG_MEMPOOL_SIZE);
	printf("     --no-huge             run without using hugepages\n");
	printf("     --enforce-numa        enforce NUMA allocations from the specified NUMA node\n");
	printf(" -i, --shm-id <id>         shared memory ID (optional)\n");
	printf(" -g, --single-file-segments   force creating just one hugetlbfs file\n");

	printf("\nPCI options:\n");
	printf(" -A, --pci-allowed <bdf>   pci addr to allow (-B and -A cannot be used at the same time)\n");
	printf(" -B, --pci-blocked <bdf>   pci addr to block (can be used more than once)\n");
	printf(" -u, --no-pci              disable PCI access\n");
	printf("     --vfio-vf-token       VF token (UUID) shared between SR-IOV PF and VFs for vfio_pci driver\n");

	printf("\nLog options:\n");
	spdk_log_usage(stdout, "-L");
	printf("     --silence-noticelog   disable notice level logging to stderr\n");

	printf("\nTrace options:\n");
	printf("     --num-trace-entries <num>   number of trace entries for each core, must be power of 2,\n");
	printf("                                 setting 0 to disable trace (default %d)\n",
	       SPDK_APP_DEFAULT_NUM_TRACE_ENTRIES);
	printf("                                 Tracepoints vary in size and can use more than one trace entry.\n");
	spdk_trace_mask_usage(stdout, "-e");

	printf("\nOther options:\n");
	printf(" -h, --help                show this usage\n");
	printf(" -v, --version             print SPDK version\n");
	printf(" -d, --limit-coredump      do not set max coredump size to RLIM_INFINITY\n");
	printf("     --env-context         Opaque context for use of the env implementation\n");

	if (app_usage) {  /* [한국어] 앱 측 콜백이 있으면 별도 섹션으로 출력 — 표준 옵션과 시각적으로 분리. */
		printf("\nApplication specific:\n");
		app_usage();
	}
}

/*
 * [한국어]
 * spdk_app_parse_args - argv를 파싱해 spdk_app_opts에 반영. main()이 spdk_app_start 직전 호출.
 *
 * @argc/@argv: main 인자 그대로.
 * @opts: 호출 전 spdk_app_opts_init된 상태여야 함. 본 함수가 결과를 채움.
 * @app_getopt_str: 앱 고유 short option 문자열(예: "T:V"). NULL 가능.
 * @app_long_opts: 앱 고유 long option 배열(NULL 종료). NULL 가능.
 * @app_parse: 앱 고유 옵션이 들어왔을 때 호출될 콜백(int ch, char *arg → 0 성공/그 외 실패).
 * @app_usage: --help 시 앱 고유 도움말 출력 콜백. NULL 가능.
 * @return: SPDK_APP_PARSE_ARGS_SUCCESS/FAIL/HELP. main은 HELP/FAIL이면 즉시 exit.
 *
 * 동작 단계:
 *   (1) g_default_opts에 backup, json config 파일 readability 검사
 *   (2) SPDK 기본 long option + 앱 long option을 합친 배열 동적 생성
 *   (3) 앱 short option과 SPDK short option 충돌 검사 + 합치기
 *   (4) getopt_long 루프 — case 별 opts 필드 갱신
 *   (5) 앱 고유 옵션은 default 절에서 app_parse 콜백에 위임
 *
 * 호출 체인:
 *   user main() → [spdk_app_parse_args] → getopt_long → app_parse / usage
 */
spdk_app_parse_args_rvals_t
spdk_app_parse_args(int argc, char **argv, struct spdk_app_opts *opts,
		    const char *app_getopt_str, const struct option *app_long_opts,
		    int (*app_parse)(int ch, char *arg),
		    void (*app_usage)(void))
{
	int ch, rc, opt_idx, global_long_opts_len, app_long_opts_len;
	struct option *cmdline_options;            /* [한국어] SPDK + 앱 long option을 합친 동적 배열 — calloc으로 할당. */
	char *cmdline_short_opts = NULL;           /* [한국어] 합친 short option 문자열 — spdk_sprintf_alloc. */
	char *shm_id_str = NULL;                   /* [한국어] -i 인자 임시 변수(음수 처리용). */
	enum spdk_app_parse_args_rvals retval = SPDK_APP_PARSE_ARGS_FAIL;  /* [한국어] 실패가 디폴트 — 명시적으로 SUCCESS 셋해야 정상. */
	long int tmp;                              /* [한국어] strtol 결과 임시(음수 검사 후 캐스트). */

	memcpy(&g_default_opts, opts, sizeof(g_default_opts));  /* [한국어] usage()가 디폴트 값을 표시하기 위해 백업. */

	if (opts->json_config_file && access(opts->json_config_file, R_OK) != 0) {
		/* [한국어] JSON config 파일을 읽을 수 없으면 무시하고 진행 — 사용자가 후에 RPC로 동적 init할 수 있도록 fail-soft. */
		SPDK_WARNLOG("Can't read JSON configuration file '%s'\n", opts->json_config_file);
		opts->json_config_file = NULL;
	}

	if (app_long_opts == NULL) {
		app_long_opts_len = 0;  /* [한국어] 앱 측 long option 없음. */
	} else {
		for (app_long_opts_len = 0;
		     app_long_opts[app_long_opts_len].name != NULL;
		     app_long_opts_len++);  /* [한국어] NULL 종단까지 길이 카운트(getopt long_options 관용). */
	}

	global_long_opts_len = SPDK_COUNTOF(g_cmdline_options);  /* [한국어] SPDK 표준 long option 개수. */

	cmdline_options = calloc(global_long_opts_len + app_long_opts_len + 1, sizeof(*cmdline_options));
	/* [한국어] +1은 getopt_long이 요구하는 NULL 종단 엔트리. calloc이라 마지막 슬롯이 자동으로 0이 됨. */
	if (!cmdline_options) {
		SPDK_ERRLOG("Out of memory\n");
		return SPDK_APP_PARSE_ARGS_FAIL;
	}

	memcpy(&cmdline_options[0], g_cmdline_options, sizeof(g_cmdline_options));  /* [한국어] SPDK 옵션을 앞 부분에 복사. */
	if (app_long_opts) {
		memcpy(&cmdline_options[global_long_opts_len], app_long_opts,
		       app_long_opts_len * sizeof(*app_long_opts));  /* [한국어] 그 뒤에 앱 옵션 이어 붙임. */
	}

	if (app_getopt_str != NULL) {
		ch = app_opts_validate(app_getopt_str);  /* [한국어] 앱 short option이 SPDK 표준과 충돌하는지 검사. */
		if (ch) {
			SPDK_ERRLOG("Duplicated option '%c' between app-specific command line parameter and generic spdk opts.\n",
				    ch);
			goto out;
		}

		if (!app_parse) {
			/* [한국어] 앱이 옵션을 추가했다면 그걸 처리할 콜백도 반드시 제공해야 함. */
			SPDK_ERRLOG("Parse function is required when app-specific command line parameters are provided.\n");
			goto out;
		}
	}

	cmdline_short_opts = spdk_sprintf_alloc("%s%s", app_getopt_str, SPDK_APP_GETOPT_STRING);
	/* [한국어] 앱 short opts와 SPDK short opts를 단순 결합. spdk_sprintf_alloc은 malloc + snprintf 헬퍼. */
	if (!cmdline_short_opts) {
		SPDK_ERRLOG("Out of memory\n");
		goto out;
	}

	g_executable_name = argv[0];  /* [한국어] usage()/spdk_app_usage()가 argv[0]를 보여주기 위해 보관. */

	while ((ch = getopt_long(argc, argv, cmdline_short_opts, cmdline_options, &opt_idx)) != -1) {
		/* [한국어] getopt_long: 한 옵션을 파싱하고 ch에 short flag(또는 OPT_IDX)를 반환. -1=끝. */
		switch (ch) {
		case CONFIG_FILE_OPT_IDX:
		case JSON_CONFIG_OPT_IDX:
			/* [한국어] -c와 --json은 동일 처리 — JSON config 파일 경로 저장(read-only 검증은 함수 시작부에서 이미 수행). */
			opts->json_config_file = optarg;
			break;
		case JSON_CONFIG_IGNORE_INIT_ERRORS_IDX:
			opts->json_config_ignore_errors = true;  /* [한국어] subsystem init 중 일부 RPC 실패해도 계속 진행. */
			break;
		case LIMIT_COREDUMP_OPT_IDX:
			opts->enable_coredump = false;  /* [한국어] -d: setrlimit으로 RLIMIT_CORE 변경하지 않음 → OS 기본값 유지. */
			break;
		case TPOINT_GROUP_OPT_IDX:
			opts->tpoint_group_mask = optarg;  /* [한국어] -e: 트레이스 마스크 문자열 저장 — spdk_app_setup_trace에서 파싱. */
			break;
		case SINGLE_FILE_SEGMENTS_OPT_IDX:
			opts->hugepage_single_segments = true;  /* [한국어] -g: hugepage 매핑 파일 단일화. */
			break;
		case HELP_OPT_IDX:
			usage(app_usage);                    /* [한국어] 표준+앱 옵션 도움말 출력. */
			retval = SPDK_APP_PARSE_ARGS_HELP;   /* [한국어] 호출자(main)에게 "정상 종료"를 알림. */
			goto out;
		case SHM_ID_OPT_IDX:
			shm_id_str = optarg;
			/* a negative shm-id disables shared configuration file */
			if (optarg[0] == '-') {
				shm_id_str++;  /* [한국어] '-' 부호를 건너뛰고 양수 부분만 spdk_strtol에 넘김 — strtol 음수 처리 우회 패턴. */
			}
			/* check if the positive value of provided shm_id can be parsed as
			 * an integer
			 */
			opts->shm_id = spdk_strtol(shm_id_str, 0);  /* [한국어] base 0 = "0x"/"0" prefix 자동 감지. */
			if (opts->shm_id < 0) {
				SPDK_ERRLOG("Invalid shared memory ID %s\n", optarg);
				goto out;
			}
			if (optarg[0] == '-') {
				opts->shm_id = -opts->shm_id;  /* [한국어] 사용자가 '-'를 줬다면 음수로 복원 → 공유 config 비활성 의미. */
			}
			break;
		case CPUMASK_OPT_IDX:
			if (opts->lcore_map) {
				/* [한국어] -m과 --lcores는 의미가 겹쳐 충돌 — 명시적으로 하나만 허용. */
				SPDK_ERRLOG("lcore map and core mask can't be set simultaneously\n");
				goto out;
			}
			opts->reactor_mask = optarg;
			break;
		case LCORES_OPT_IDX:
			if (opts->reactor_mask) {
				SPDK_ERRLOG("lcore map and core mask can't be set simultaneously\n");
				goto out;
			}
			opts->lcore_map = optarg;
			break;
		case DISABLE_CPUMASK_LOCKS_OPT_IDX:
			opts->disable_cpumask_locks = true;  /* [한국어] /var/tmp/spdk_cpu_lock 파일 사용 안 함. */
			break;
		case MEM_CHANNELS_OPT_IDX:
			opts->mem_channel = spdk_strtol(optarg, 0);
			if (opts->mem_channel < 0) {
				SPDK_ERRLOG("Invalid memory channel %s\n", optarg);
				goto out;
			}
			break;
		case MAIN_CORE_OPT_IDX:
			opts->main_core = spdk_strtol(optarg, 0);
			if (opts->main_core < 0) {
				SPDK_ERRLOG("Invalid main core %s\n", optarg);
				goto out;
			}
			break;
		case SILENCE_NOTICELOG_OPT_IDX:
			opts->print_level = SPDK_LOG_WARN;  /* [한국어] NOTICE/INFO 라인을 stderr에서 숨김 — daemon 모드 권장 설정. */
			break;
		case RPC_SOCKET_OPT_IDX:
			opts->rpc_addr = optarg;  /* [한국어] -r: UNIX socket 또는 TCP주소 형식 가능(lib/rpc 가 검증). */
			break;
		case NO_RPC_SERVER_OPT_IDX:
			opts->rpc_addr = NULL;  /* [한국어] --no-rpc-server: NULL이면 spdk_rpc_initialize 호출하지 않음. */
			break;
		case ENFORCE_NUMA_OPT_IDX:
			opts->enforce_numa = true;
			break;
		case MEM_SIZE_OPT_IDX: {
			uint64_t mem_size_mb;            /* [한국어] -s 결과를 MB 단위로 보관할 임시 변수. */
			bool mem_size_has_prefix;        /* [한국어] "5G" 같은 단위 prefix가 있었는지 여부. */

			rc = spdk_parse_capacity(optarg, &mem_size_mb, &mem_size_has_prefix);
			/* [한국어] 사람 친화적 용량 파서: "1024", "1G", "512M" 등을 바이트로 환산하고 prefix 유무를 알려줌. */
			if (rc != 0) {
				SPDK_ERRLOG("invalid memory pool size `-s %s`\n", optarg);
				usage(app_usage);
				goto out;
			}

			if (mem_size_has_prefix) {
				/* the mem size is in MB by default, so if a prefix was
				 * specified, we need to manually convert to MB.
				 */
				/* [한국어] -s는 기본 MB 해석. prefix(G/M/...)가 있으면 spdk_parse_capacity가 바이트로 반환했으니 MB로 환산. */
				mem_size_mb /= 1024 * 1024;
			}

			if (mem_size_mb > INT_MAX) {
				/* [한국어] opts->mem_size는 int이므로 INT_MAX 초과 입력 거부 — 사실상 2 PiB 이상 비현실 값. */
				SPDK_ERRLOG("invalid memory pool size `-s %s`\n", optarg);
				usage(app_usage);
				goto out;
			}

			opts->mem_size = (int) mem_size_mb;  /* [한국어] DPDK가 받는 mem_size 필드(MB 단위 int)에 저장. */
			break;
		}
		case MSG_MEMPOOL_SIZE_OPT_IDX:
			tmp = spdk_strtol(optarg, 10);  /* [한국어] base 10 강제. msg_mempool_size는 양수만 의미 있음. */
			if (tmp <= 0) {
				SPDK_ERRLOG("Invalid message memory pool size %s\n", optarg);
				goto out;
			}

			opts->msg_mempool_size = (size_t)tmp;  /* [한국어] 명시값 채택 → calculate_mempool_size에서 자동 산정 우회. */
			break;

		case NO_PCI_OPT_IDX:
			opts->no_pci = true;  /* [한국어] DPDK PCI probe 비활성. */
			break;
		case WAIT_FOR_RPC_OPT_IDX:
			opts->delay_subsystem_init = true;  /* [한국어] subsystem_init을 외부 framework_start_init RPC 시까지 보류. */
			break;
		case PCI_BLOCKED_OPT_IDX:
			if (opts->pci_allowed) {
				/* [한국어] -A와 -B는 의미상 상호 배타. 이전에 -A로 누적된 배열 정리 후 에러. */
				free(opts->pci_allowed);
				opts->pci_allowed = NULL;
				SPDK_ERRLOG("-B and -A cannot be used at the same time\n");
				usage(app_usage);
				goto out;
			}

			rc = app_opts_add_pci_addr(opts, &opts->pci_blocked, optarg);  /* [한국어] BDF 파싱 후 동적 배열에 추가. */
			if (rc != 0) {
				free(opts->pci_blocked);  /* [한국어] 부분 누적 상태 정리. */
				opts->pci_blocked = NULL;
				goto out;
			}
			break;

		case NO_HUGE_OPT_IDX:
			opts->no_huge = true;  /* [한국어] hugepage 미사용(테스트/CI 환경용). */
			break;

		case LOGFLAG_OPT_IDX:
			rc = spdk_log_set_flag(optarg);  /* [한국어] 모듈별 디버그 플래그 활성화(예: "bdev", "nvme"). */
			if (rc < 0) {
				SPDK_ERRLOG("unknown flag: %s\n", optarg);
				usage(app_usage);
				goto out;
			}
#ifdef DEBUG
			/* [한국어] DEBUG 빌드에선 -L 사용 시 콘솔 출력 임계도 자동으로 DEBUG로 낮춰 SPDK_DEBUGLOG가 실제로 보이게 함. */
			opts->print_level = SPDK_LOG_DEBUG;
#endif
			break;
		case HUGE_UNLINK_OPT_IDX:
			opts->unlink_hugepage = true;  /* [한국어] hugepage backing 파일을 init 직후 unlink → 비정상 종료 시 잔여 파일 방지. */
			break;
		case PCI_ALLOWED_OPT_IDX:
			if (opts->pci_blocked) {
				free(opts->pci_blocked);
				opts->pci_blocked = NULL;
				SPDK_ERRLOG("-B and -W cannot be used at the same time\n");
				usage(app_usage);
				goto out;
			}

			rc = app_opts_add_pci_addr(opts, &opts->pci_allowed, optarg);
			if (rc != 0) {
				free(opts->pci_allowed);
				opts->pci_allowed = NULL;
				goto out;
			}
			break;
		case BASE_VIRTADDR_OPT_IDX:
			tmp = spdk_strtoll(optarg, 0);  /* [한국어] base 0 → "0x..." hex 가능. */
			if (tmp <= 0) {
				SPDK_ERRLOG("Invalid base-virtaddr %s\n", optarg);
				usage(app_usage);
				goto out;
			}
			opts->base_virtaddr = (uint64_t)tmp;
			break;
		case HUGE_DIR_OPT_IDX:
			opts->hugedir = optarg;  /* [한국어] 다중 hugetlbfs mount 환경에서 특정 mount 강제. */
			break;
		case IOVA_MODE_OPT_IDX:
			opts->iova_mode = optarg;  /* [한국어] "pa" 또는 "va". DPDK env_init이 내부적으로 검증. */
			break;
		case NUM_TRACE_ENTRIES_OPT_IDX:
			tmp = spdk_strtoll(optarg, 0);
			if (tmp < 0) {
				SPDK_ERRLOG("Invalid num-trace-entries %s\n", optarg);
				usage(app_usage);
				goto out;
			}
			opts->num_entries = (uint64_t)tmp;
			if (opts->num_entries > 0 && opts->num_entries & (opts->num_entries - 1)) {
				/* [한국어] 트레이스 ringbuffer 인덱스가 비트 마스크로 wrap되므로 2의 거듭제곱이어야 함. */
				SPDK_ERRLOG("num-trace-entries must be power of 2\n");
				usage(app_usage);
				goto out;
			}
			break;
		case ENV_CONTEXT_OPT_IDX:
			opts->env_context = optarg;  /* [한국어] env 백엔드(주로 DPDK)에 그대로 패스스루되는 추가 인자 문자열. */
			break;
		case RPCS_ALLOWED_OPT_IDX:
			/* [한국어] 콤마 분리 → const char* 배열 형태로 변환. NULL 종단. */
			opts->rpc_allowlist = (const char **)spdk_strarray_from_string(optarg, ",");
			if (opts->rpc_allowlist == NULL) {
				SPDK_ERRLOG("Invalid --rpcs-allowed argument\n");
				usage(app_usage);
				goto out;
			}
			break;
		case ENV_VF_TOKEN_OPT_IDX:
			opts->vf_token = optarg;  /* [한국어] VFIO VF token UUID 문자열. */
			break;
		case INTERRUPT_MODE_OPT_IDX:
			opts->interrupt_mode = true;
			break;
		case VERSION_OPT_IDX:
			printf(SPDK_VERSION_STRING"\n");  /* [한국어] git tag 기반 버전 문자열을 한 줄로 출력. */
			retval = SPDK_APP_PARSE_ARGS_HELP;  /* [한국어] HELP와 동일하게 main이 정상 종료하도록 신호. */
			goto out;
		case '?':
			/*
			 * In the event getopt() above detects an option
			 * in argv that is NOT in the getopt_str,
			 * getopt() will return a '?' indicating failure.
			 */
			/* [한국어] 알 수 없는 옵션 → usage 출력 후 FAIL 반환(retval은 디폴트 FAIL). */
			usage(app_usage);
			goto out;
		default:
			if (!app_parse) {
				/* [한국어] 앱 옵션 콜백이 없는데 디폴트 절에 들어온 글자가 있으면 SPDK 측에서 처리 못 함. */
				SPDK_ERRLOG("Unsupported app-specific command line parameter '%c'.\n", ch);
				goto out;
			}

			rc = app_parse(ch, optarg);  /* [한국어] 앱이 직접 파싱 — 0=성공, 그 외=실패. */
			if (rc) {
				SPDK_ERRLOG("Parsing app-specific command line parameter '%c' failed: %d\n", ch, rc);
				goto out;
			}
		}
	}

	retval = SPDK_APP_PARSE_ARGS_SUCCESS;  /* [한국어] 모든 옵션 정상 처리 — 호출자에게 SUCCESS 통보. */
out:
	if (retval != SPDK_APP_PARSE_ARGS_SUCCESS) {
		/* [한국어] 실패/HELP 경로에선 누적된 동적 자원을 정리. SUCCESS는 spdk_app_start가 자원 소유권을 이어받음. */
		free(opts->pci_blocked);
		opts->pci_blocked = NULL;
		free(opts->pci_allowed);
		opts->pci_allowed = NULL;
		spdk_strarray_free((char **)opts->rpc_allowlist);
		opts->rpc_allowlist = NULL;
	}
	free(cmdline_short_opts);  /* [한국어] spdk_sprintf_alloc 결과 해제. */
	free(cmdline_options);     /* [한국어] calloc된 long option 동적 배열 해제. */
	return retval;
}

/*
 * [한국어]
 * spdk_app_usage - 앱 외부에서 표준 SPDK 옵션 도움말만 출력하고 싶을 때 호출.
 *
 * spdk_app_parse_args가 한 번이라도 호출돼 g_executable_name이 셋된 이후에만 동작.
 *
 * 호출 체인:
 *   user code → [spdk_app_usage] → usage(NULL)
 */
void
spdk_app_usage(void)
{
	if (g_executable_name == NULL) {  /* [한국어] argv[0]이 없으면 usage 출력 자체가 의미 없음. */
		SPDK_ERRLOG("%s not valid before calling spdk_app_parse_args()\n", __func__);
		return;
	}

	usage(NULL);  /* [한국어] 앱 측 usage 콜백 없이 표준 옵션만 출력. */
}

/*
 * [한국어]
 * rpc_framework_start_init_cpl - framework_start_init RPC의 subsystem_init 완료 콜백.
 *
 * @rc: subsystem_init 결과(0=성공, 그 외=실패).
 * @arg1: 원래 요청 객체(spdk_jsonrpc_request *).
 *
 * --wait-for-rpc 모드에서 외부 클라이언트가 framework_start_init RPC를 보낸 뒤 init이 끝났을 때
 * 응답을 돌려주기 위한 비동기 완료 핸들러. 실패 시 RPC 서버를 다시 resume해 사용자가 추가 RPC로
 * 복구를 시도할 수 있게 한다(보통은 init 실패 → 앱 종료지만, 디버깅 환경에서는 유용).
 *
 * 호출 체인:
 *   rpc_framework_start_init → spdk_subsystem_init → [rpc_framework_start_init_cpl(app_thread)]
 *     → spdk_jsonrpc_send_bool_response / app_subsystem_init_done
 */
static void
rpc_framework_start_init_cpl(int rc, void *arg1)
{
	struct spdk_jsonrpc_request *request = arg1;  /* [한국어] arg1을 캐스팅 — 응답 송신 채널. */

	assert(spdk_thread_is_app_thread(NULL));  /* [한국어] subsystem 콜백은 반드시 app_thread에서 실행되어야 함. */

	if (rc) {
		if (g_spdk_app.rpc_addr) {
			spdk_rpc_server_resume(g_spdk_app.rpc_addr);  /* [한국어] init 실패 → RPC 서버 다시 활성화해 외부 복구 RPC 수신. */
		}
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "framework_initialization failed");

		app_subsystem_init_done(rc, NULL);  /* [한국어] 일반 부팅 경로의 init_done 콜백도 함께 호출 — 종료 시퀀스 진입. */
		return;
	}

	app_subsystem_init_done(0, NULL);  /* [한국어] 정상 → STARTUP→RUNTIME 전이 + 사용자 진입점 호출 트리거. */

	spdk_jsonrpc_send_bool_response(request, true);  /* [한국어] RPC 클라이언트에 true 응답 — 부팅 성공. */
}

/*
 * [한국어]
 * rpc_framework_start_init - "framework_start_init" RPC 핸들러.
 *
 * @request: JSON-RPC 요청 객체.
 * @params: 인자 — 본 RPC는 인자를 받지 않으므로 NULL이어야 함.
 *
 * --wait-for-rpc 모드의 핵심 진입점. STARTUP 상태에서만 등록되어 있어, 한 번 호출되어
 * RUNTIME으로 전이하면 더는 호출 불가. RPC 서버를 잠시 pause해 init 동안 다른 RPC가
 * 들어오는 것을 막는다.
 *
 * JSON 스키마: 요청 params=null, 성공 시 응답 result=true, 실패 시 error 객체 반환.
 *
 * 호출 체인:
 *   external client (JSON-RPC) → spdk_rpc_dispatcher → [rpc_framework_start_init(app_thread)]
 *     → spdk_subsystem_init → rpc_framework_start_init_cpl
 */
static void
rpc_framework_start_init(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "framework_start_init requires no parameters");
		return;
	}

	spdk_rpc_server_pause(g_spdk_app.rpc_addr);  /* [한국어] init 동안 외부 RPC 차단(부팅 일관성). */
	spdk_subsystem_init(rpc_framework_start_init_cpl, request);  /* [한국어] init 시작. 완료 콜백에서 응답 송신. */
}
/* [한국어] STARTUP 상태에서만 노출되는 RPC — RUNTIME에서는 ENOENT로 거부. */
SPDK_RPC_REGISTER("framework_start_init", rpc_framework_start_init, SPDK_RPC_STARTUP)

/*
 * [한국어]
 * struct subsystem_init_poller_ctx - framework_wait_init RPC가 RUNTIME 진입을 기다리며 사용하는 컨텍스트.
 *
 * 폴러를 직접 만들어 매 폴 사이클마다 RPC 상태를 확인. RUNTIME으로 바뀌면 응답 전송 후 자기 자신 unregister.
 */
struct subsystem_init_poller_ctx {
	struct spdk_poller *init_poller;
	/* [한국어] 이 컨텍스트를 폴링하는 SPDK poller 핸들. SPDK_POLLER_REGISTER가 채움.
	 * 설정자: rpc_framework_wait_init. 읽는 자: rpc_subsystem_init_poller_ctx가 unregister 시 전달.
	 * 값 범위: 비-NULL 폴러 핸들. 동기화: app_thread 단독 접근. */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답을 돌려보낼 원래 RPC 요청 객체.
	 * 설정자: 핸들러 진입 시. 읽는 자: 폴러가 RUNTIME 진입 후 응답 송신 시.
	 * 값 범위: 비-NULL. 사용 후 lib/jsonrpc가 내부적으로 free.
	 * 동기화: 한 요청은 단일 thread에서만 처리됨. */
};

/*
 * [한국어]
 * rpc_subsystem_init_poller_ctx - framework_wait_init용 폴러 콜백. RPC 상태를 매 폴 사이클 확인.
 *
 * @ctx: subsystem_init_poller_ctx*.
 * @return: SPDK_POLLER_BUSY 고정 — 매 사이클마다 의미 있는 작업이 있다고 보고(스케줄러가 sleep로 빠지지 않게).
 *
 * RUNTIME 상태가 되면 응답 보내고 자기 자신을 unregister. 그 외엔 다음 폴 사이클에 다시 호출됨.
 * 호출 컨텍스트: poller가 등록된 SPDK thread(보통 app_thread).
 *
 * 호출 체인:
 *   spdk_thread_poll → [rpc_subsystem_init_poller_ctx] → spdk_jsonrpc_send_bool_response / spdk_poller_unregister
 */
static int
rpc_subsystem_init_poller_ctx(void *ctx)
{
	struct subsystem_init_poller_ctx *poller_ctx = ctx;  /* [한국어] void*에서 실제 타입으로 캐스팅. */

	if (spdk_rpc_get_state() == SPDK_RPC_RUNTIME) {  /* [한국어] STARTUP→RUNTIME 전이 감지. */
		spdk_jsonrpc_send_bool_response(poller_ctx->request, true);  /* [한국어] 호출자에게 init 완료 통지. */
		spdk_poller_unregister(&poller_ctx->init_poller);  /* [한국어] 폴러 자기 자신 해제 — 더 이상 호출되지 않음. */
		free(poller_ctx);  /* [한국어] malloc된 컨텍스트 회수. */
	}

	return SPDK_POLLER_BUSY;  /* [한국어] 항상 BUSY 반환 — 다른 sleep 가능 폴러보다 우선 폴 받음. */
}

/*
 * [한국어]
 * rpc_framework_wait_init - "framework_wait_init" RPC 핸들러. 부팅 진행을 동기화.
 *
 * @request/@params: 표준 시그니처. params는 NULL이어야 함은 명시되지 않았으나 사용 안 함.
 *
 * 외부 부팅 스크립트(예: rpc.py + framework_start_init 후 framework_wait_init)에서 init 완료를
 * 폴링 없이 한 번의 RPC로 기다릴 수 있도록 하는 동기화 도우미. 이미 RUNTIME이면 즉시 true 응답,
 * 아직 STARTUP이면 폴러를 등록해 RUNTIME 진입 시점에 응답을 보낸다.
 *
 * STARTUP과 RUNTIME 양 상태에서 모두 등록되어 있어 언제 호출되어도 동작.
 *
 * 호출 체인:
 *   external client → spdk_rpc_dispatcher → [rpc_framework_wait_init(app_thread)]
 *     → SPDK_POLLER_REGISTER → rpc_subsystem_init_poller_ctx (폴 사이클마다)
 */
static void
rpc_framework_wait_init(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct subsystem_init_poller_ctx *ctx;

	if (spdk_rpc_get_state() == SPDK_RPC_RUNTIME) {
		spdk_jsonrpc_send_bool_response(request, true);  /* [한국어] 이미 RUNTIME → 즉시 응답. */
	} else {
		ctx = malloc(sizeof(struct subsystem_init_poller_ctx));  /* [한국어] 폴러 라이프타임 동안 살아 있어야 하므로 힙 할당. */
		if (ctx == NULL) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Unable to allocate memory for the request context\n");
			return;
		}
		ctx->request = request;
		/* [한국어] 0us 주기 = "매 폴 사이클마다 호출". RUNTIME 진입 시점에서 한 번만 일을 함. */
		ctx->init_poller = SPDK_POLLER_REGISTER(rpc_subsystem_init_poller_ctx, ctx, 0);
	}
}
/* [한국어] 양 상태에서 호출 가능 — RUNTIME에서도 호출하면 즉시 true 응답해 멱등성 확보. */
SPDK_RPC_REGISTER("framework_wait_init", rpc_framework_wait_init,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_framework_disable_cpumask_locks - "framework_disable_cpumask_locks" RPC 핸들러.
 *
 * @request/@params: params는 반드시 null.
 *
 * 부팅 후 동적으로 /var/tmp/spdk_cpu_lock_NNN 파일들을 close+unlink한다. 운영 중 다른 SPDK
 * 프로세스를 같은 코어에서 띄울 일이 생기는 경우(예: 일시적 디버그 인스턴스)에 사용. 실패한
 * 코어 번호는 에러 메시지에 담아 클라이언트에 보고.
 *
 * 호출 체인:
 *   external client → spdk_rpc_dispatcher → [rpc_framework_disable_cpumask_locks(app_thread)]
 *     → unclaim_cpu_cores
 */
static void
rpc_framework_disable_cpumask_locks(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	char msg[128];        /* [한국어] 실패 시 사용자에게 보여줄 에러 메시지 버퍼. */
	int rc;
	uint32_t failed_core; /* [한국어] unclaim_cpu_cores가 실패한 코어 번호를 기록. */

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "framework_disable_cpumask_locks"
						 "requires no arguments");
		return;
	}

	rc = unclaim_cpu_cores(&failed_core);
	if (rc) {
		snprintf(msg, sizeof(msg), "Failed to unclaim CPU core: %" PRIu32, failed_core);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, msg);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);  /* [한국어] 모든 락 해제 성공 응답. */
}
/* [한국어] STARTUP/RUNTIME 양쪽에서 호출 가능 — 부팅 단계와 운영 단계 모두 유효한 동작. */
SPDK_RPC_REGISTER("framework_disable_cpumask_locks", rpc_framework_disable_cpumask_locks,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_framework_enable_cpumask_locks - "framework_enable_cpumask_locks" RPC 핸들러.
 *
 * @request/@params: params는 반드시 null.
 *
 * disable의 역동작. /var/tmp/spdk_cpu_lock_NNN 파일을 다시 만들고 fcntl 락을 잡는다.
 * --disable-cpumask-locks로 부팅했거나 위 disable RPC로 락을 푼 뒤 다시 보호 모드로 전환할 때 사용.
 *
 * 호출 체인:
 *   external client → spdk_rpc_dispatcher → [rpc_framework_enable_cpumask_locks(app_thread)]
 *     → claim_cpu_cores
 */
static void
rpc_framework_enable_cpumask_locks(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	char msg[128];
	int rc;
	uint32_t failed_core;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "framework_enable_cpumask_locks"
						 "requires no arguments");
		return;
	}

	rc = claim_cpu_cores(&failed_core);
	if (rc) {
		snprintf(msg, sizeof(msg), "Failed to claim CPU core: %" PRIu32, failed_core);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, msg);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);  /* [한국어] 모든 코어 잠금 성공. */
}
SPDK_RPC_REGISTER("framework_enable_cpumask_locks", rpc_framework_enable_cpumask_locks,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
