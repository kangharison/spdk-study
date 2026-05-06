/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.  All rights reserved.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/**
 * \file
 * SPDK Initialization Helper
 */

/*
 * [한국어 설명] SPDK subsystem 초기화·종료 + JSON-RPC 서버 라이프사이클 헬퍼 헤더 (init.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 부팅 후 "subsystem 그래프 전체를 올바른 의존 순서로 초기화/종료"하는
 * 진입점과, 동시에 그 동안 외부에서 명령을 받을 JSON-RPC 서버 라이프사이클을 정의한다.
 * SPDK는 bdev, accel, nvmf, vhost, sock, iscsi, scheduler, scsi, env_dpdk, keyring 등
 * 수많은 서브시스템으로 macro-level 분리되어 있고, 각각은 C 생성자
 * (`__attribute__((constructor))`)로 자기를 등록하면서 의존하는 다른 서브시스템 이름을
 * 선언한다(예: bdev → accel, vhost → bdev). `spdk_subsystem_init()`은 이 그래프를
 * 위상정렬해 init_fn을 차례로 호출하고, 모든 비동기 init이 완료되면 사용자 콜백을 부른다.
 * 종료 단계도 역순으로 fini_fn을 호출하여 의존 모듈이 살아있는 상태에서 자원이 해제되는
 * 사용 후 해제(use-after-free) 사고를 방지한다.
 * RPC 서버 함수들은 init과 독립적으로 시작/정지/일시정지가 가능하며, JSON config 자동
 * 적용(`spdk_subsystem_load_config`) 또한 RPC 메서드를 재생하는 형태로 구현되어 있다.
 * RPC 서버는 SPDK가 노출하는 거의 모든 관리 기능(bdev 생성, NVMe attach, NVMe-oF
 * subsystem 추가 등)의 단일 통제 평면이며, 본 헤더는 그 입구를 정의한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (부팅):
 *   main() → spdk_app_start(opts, start_fn, ctx) [event.h]
 *     → spdk_env_init() (DPDK EAL 초기화)
 *     → reactor 가동 후 main lcore에서 start_fn 호출
 *       → (자동) spdk_subsystem_init(cb, arg) [본 헤더]
 *         → 등록된 모든 subsystem의 init_fn(SPDK_INIT_COMPLETE) 비동기 체인
 *         → 마지막 subsystem init 완료 시 cb(rc, arg) 호출
 *       → (자동) spdk_subsystem_load_config(json_config_file 내용, ...)
 *       → (자동) spdk_rpc_initialize(opts.rpc_addr) — RPC 서버를 RUNTIME phase로 승격
 * 호출 체인 (종료):
 *     SIGTERM/SIGINT → spdk_app_start_shutdown → spdk_subsystem_fini(cb, arg) [본 헤더]
 *         → 등록 역순으로 fini_fn 호출 → cb(arg) → spdk_app_stop(0) → reactor 종료.
 * RPC phase 모델:
 *   STARTUP phase: spdk_subsystem_init 진행 중 — 일부 부트스트랩 RPC만 허용 (예:
 *     framework_start_init, bdev_set_options 같이 init 전에 적용해야 하는 옵션).
 *   RUNTIME phase: 모든 subsystem이 init 완료된 후 — 일반 관리 RPC 전부 허용.
 *   spdk_rpc_load_config / framework_start_init 메서드가 phase 전환 트리거.
 * 실행 컨텍스트: 본 헤더의 모든 함수는 SPDK app thread (= main lcore의 spdk_thread)
 * 에서 호출되어야 한다. cross-thread에서 부르면 subsystem 등록 리스트(전역 TAILQ)와
 * RPC 서버 상태가 보호되지 않아 UB(undefined behavior) 발생.
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/stdinc.h, spdk/queue.h, spdk/log.h, spdk/assert.h.
 * 의존(구현): lib/init/(subsystem.c, subsystem_rpc.c, rpc.c — 이 헤더의 실체).
 * 의존받는 쪽:
 *   - lib/event/app.c (spdk_app_start 내부에서 spdk_subsystem_init/load_config/
 *     spdk_rpc_initialize 호출)
 *   - 모든 SPDK 메인 앱 (app/spdk_tgt, app/nvmf_tgt, app/iscsi_tgt, app/vhost 등)
 *   - 사용자 정의 앱 (예: 테스트 코드, 스토리지 매니저)
 * 데이터 흐름:
 *   각 subsystem 모듈(예: lib/bdev/bdev.c)이 SPDK_SUBSYSTEM_REGISTER(g_bdev_subsystem)
 *   매크로로 자기 정보(`struct spdk_subsystem`: name, init_fn, fini_fn, write_config_json,
 *   get_ctx_size 등)를 lib/init의 전역 TAILQ에 푸시 → spdk_subsystem_init이 그 리스트를
 *   의존성 그래프로 변환하고 위상정렬·순회하며 각 init_fn을 호출 → 각 모듈은 끝나면
 *   spdk_subsystem_init_next() 호출(내부 헤더 spdk_internal/init.h)으로 다음 모듈을
 *   진행시킨다. 본 헤더는 그 *공개* 트리거만 노출.
 * 공유 자료구조:
 *   - lib/init 전역 TAILQ<spdk_subsystem> g_subsystems — 등록 순서 보존.
 *   - 전역 TAILQ<spdk_subsystem_depend> g_subsystems_deps — depends_on 엣지.
 *   둘 다 SPDK_SUBSYSTEM_REGISTER / SPDK_SUBSYSTEM_DEPEND 가 컴파일 타임 생성자에서
 *   채우므로 스레드 보호 불필요(단일 스레드 등록).
 *
 * === 주요 함수/구조체 요약 ===
 * - SPDK_DEFAULT_RPC_ADDR: 기본 RPC 소켓 경로(/var/tmp/spdk.sock).
 * - struct spdk_rpc_opts: RPC 서버 부가 옵션(로그 파일/레벨). ABI를 위해 size 필드 보유.
 * - spdk_rpc_initialize / spdk_rpc_finish / spdk_rpc_server_finish: RPC 서버 시작·정지.
 * - spdk_rpc_server_pause / spdk_rpc_server_resume: 운영 중 RPC 서버 일시정지·재개.
 * - typedef spdk_subsystem_init_fn / spdk_subsystem_fini_fn: 비동기 완료 콜백 시그니처.
 * - spdk_subsystem_init(cb_fn, cb_arg): 모든 subsystem을 의존 순서대로 비동기 초기화.
 * - spdk_subsystem_load_config(json, size, cb, arg, stop_on_error): JSON에서
 *   RPC 시퀀스를 읽어 재생하여 구성을 적용한 뒤 init 완료를 보고.
 * - spdk_subsystem_fini(cb_fn, cb_arg): 등록 역순으로 모든 subsystem 종료.
 * - spdk_subsystem_exists(name): 특정 subsystem 등록 여부 조회 (옵셔널 의존성 검사용).
 */

#ifndef SPDK_INIT_H
/* [한국어] include guard 시작 — 동일 TU(translation unit)에서 본 헤더가 여러 번
 * include될 때 중복 선언으로 인한 컴파일 오류를 방지. SPDK는 모든 공개 헤더에
 * SPDK_<NAME>_H 컨벤션을 사용한다. */
#define SPDK_INIT_H
/* [한국어] 가드 매크로 정의. 이후 같은 헤더가 다시 들어오면 #ifndef가 거짓이 되어
 * 본 블록 전체가 스킵된다. */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 헤더 모음 (stdint/stddef/stdio/stdbool 등). 본 헤더가 사용하는
 * size_t (struct spdk_rpc_opts.size), FILE* (log_file), ssize_t
 * (spdk_subsystem_load_config의 json_size), bool (stop_on_error,
 * spdk_subsystem_exists 반환값) 모두 여기서 가져온다. */
#include "spdk/queue.h"
/* [한국어] BSD TAILQ/STAILQ 매크로 모음. 본 공개 헤더 자체에서는 직접 매크로를
 * 사용하지 않지만, 구현부(lib/init/subsystem.c)가 spdk_subsystem 등록 리스트를
 * TAILQ로 관리하고, 본 헤더의 사용자(앱) 측에서도 동일 매크로를 쓰는 일이 흔해
 * 종속성 일관성을 위해 포함. */
#include "spdk/log.h"
/* [한국어] enum spdk_log_level 정의 — struct spdk_rpc_opts.log_level 필드의 타입.
 * SPDK_LOG_DISABLED, SPDK_LOG_ERROR, ..., SPDK_LOG_DEBUG 의 정의를 가져오기 위해 필요. */
#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT 매크로 — struct spdk_rpc_opts의 sizeof를 컴파일 타임에
 * 24바이트로 못박아 ABI 안정성을 강제한다. 누군가 필드를 추가하면 빌드가 깨지면서
 * SPDK_STATIC_ASSERT 줄도 같이 갱신해야 한다는 신호가 된다. */

#ifdef __cplusplus
/* [한국어] C++에서 이 헤더를 인클루드할 때 함수 심볼이 C++ name mangling 규칙으로
 * 변형되지 않도록 extern "C" 블록을 연다. SPDK 라이브러리는 C로 컴파일되어
 * 'spdk_subsystem_init' 같은 평탄 심볼로 export 되므로, C++ 사용자가 매칭되는
 * 심볼을 찾으려면 extern "C" 가 필수. */
extern "C" {
/* [한국어] 이하 모든 선언은 C linkage로 처리. */
#endif

#define SPDK_DEFAULT_RPC_ADDR "/var/tmp/spdk.sock"
/* [한국어] SPDK가 제공하는 기본 JSON-RPC 리스닝 주소 (Unix domain socket 경로).
 * 사용자: spdk_app_opts.rpc_addr나 spdk_rpc_initialize의 listen_addr를 NULL로 두면
 *         라이브러리가 이 경로를 자동 사용한다.
 * 경로 선택 이유: /var/tmp는 대부분의 리눅스 배포판에서 tmpfs 또는 일반 사용자
 *               쓰기 가능 디렉토리라 권한 충돌이 적고, 리부트 후에도 stale 소켓이
 *               자동 정리되며, 컨테이너 내부에서도 공유 볼륨으로 mount 하기 쉽다.
 * 다중 인스턴스: 동일 호스트에서 SPDK 앱을 여러 개 띄울 때는 각각 다른 경로
 *              (예: /var/tmp/spdk1.sock, /var/tmp/spdk2.sock)를 명시적으로 지정해야
 *              한다 — 같은 경로를 사용하면 EADDRINUSE 로 두 번째 앱이 실패. */

/**
 * Structure with optional parameters for the JSON-RPC server initialization.
 */
/*
 * [한국어]
 * struct spdk_rpc_opts - JSON-RPC 서버 부팅 옵션 컨테이너.
 *
 * 설계 의도: spdk_app_opts와 동일한 versioned-options 패턴. ABI 호환을 위해
 * 첫 필드를 size로 두고 caller가 자기 빌드 시점의 sizeof(struct spdk_rpc_opts)를
 * 채워 넘기게 한다. 라이브러리는 size 필드까지만 신뢰하고 그 이후 필드는 라이브러리
 * 빌드 시점의 기본값으로 채워서 사용한다. 이로써 라이브러리가 새 버전에서 필드를
 * 추가해도 옛 caller 바이너리가 그대로 동작.
 *
 * 사용 패턴:
 *   struct spdk_rpc_opts opts = {};
 *   opts.size = SPDK_SIZEOF(&opts, log_level); // 또는 sizeof(opts)
 *   opts.log_file = fopen("/var/log/spdk_rpc.log", "a");
 *   opts.log_level = SPDK_LOG_INFO;
 *   spdk_rpc_initialize(SPDK_DEFAULT_RPC_ADDR, &opts);
 */
struct spdk_rpc_opts {
	/* Size of this structure in bytes. */
	size_t size;
	/* [한국어] 호출자(=사용자 코드) 시점의 sizeof(struct spdk_rpc_opts) 값.
	 * 설정자: 사용자 — 반드시 sizeof로 채워야 함.
	 * 읽는 자: spdk_rpc_initialize 내부 — 이 크기까지의 필드만 신뢰하고,
	 *         넘는 영역은 미초기화로 가정해 라이브러리 기본값으로 덮어씀.
	 * 값 범위: > 0. 0이면 라이브러리가 호환 모드로 모두 기본값 처리.
	 * 동기화: caller 스택/힙 변수이며 단일 스레드에서 채우므로 별도 락 없음. */

	/*
	 * A JSON-RPC log file pointer. The default value is NULL and used
	 * when options are omitted.
	 */
	FILE *log_file;
	/* [한국어] RPC 호출/응답을 별도 파일로 남기고 싶을 때의 FILE* 핸들.
	 * 설정자: 사용자가 fopen으로 열어 전달.
	 * 읽는 자: lib/init의 RPC 서버 핸들러 — 메서드 디스패치 직전/직후에 fwrite로
	 *         JSON 요청·응답을 추가 기록.
	 * 값 범위: 유효 FILE* 또는 NULL(=별도 RPC 로그 파일 사용 안 함, 메인 로그만 사용).
	 * 동기화: 라이브러리는 이 FILE*에 대해 스스로 락을 잡지 않으므로 사용자가
	 *         setvbuf 또는 외부 락으로 다중 작성자 방지를 책임진다. */

	/*
	 * JSON-RPC log level. Default value is SPDK_LOG_DISABLED and used
	 * when options are omitted.
	 */
	enum spdk_log_level log_level;
	/* [한국어] RPC 서버가 호출 자체를 로깅할 때 적용할 로그 레벨.
	 * 설정자: 사용자.
	 * 읽는 자: RPC 서버가 메서드 디스패치 시 spdk_log(level, ...) 형태로 기록하며,
	 *         설정된 레벨 이상의 메시지만 실제로 로그 출력 대상이 된다.
	 * 값 범위: SPDK_LOG_DISABLED < ERROR < WARN < NOTICE < INFO < DEBUG.
	 *         DISABLED면 RPC 별도 로그 없음(에러도 메인 SPDK 로그로만 흐름).
	 * 동기화: 단일 스레드(SPDK app thread)에서 RPC 처리하므로 별도 보호 불필요. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_rpc_opts) == 24, "Incorrect size");
/* [한국어] sizeof를 컴파일 타임에 24바이트로 고정 — ABI(Application Binary Interface)
 * 일관성 보증. 64비트 리눅스 기준: size_t(8) + FILE*(8) + enum(4) + padding(4) = 24.
 * 누군가 새 필드를 추가하면 sizeof가 변하면서 이 어서션이 깨져 빌드 실패가 발생,
 * 그러면 함께 이 상수도 갱신하고 — 더 중요하게 — 옛 caller가 size로 신구 필드를
 * 구분할 수 있도록 라이브러리 코드에서 size 비교 로직을 추가해야 한다는 신호가 된다. */

/**
 * Create SPDK JSON-RPC server listening at provided address and start polling it for connections.
 *
 * This function should be called from the SPDK app thread.
 *
 * The RPC server is optional and is independent of subsystem initialization.
 * The RPC server can be started and stopped at any time.
 *
 * \param listen_addr Path to a unix domain socket to listen on
 * \param opts Options for JSON-RPC server initialization. If NULL, default values are used.
 *
 * \return Negated errno on failure. 0 on success.
 */
/*
 * [한국어]
 * spdk_rpc_initialize - JSON-RPC 서버를 listen_addr에 띄우고 connection 폴러를 등록.
 *
 * @listen_addr: Unix domain socket 경로 (예: SPDK_DEFAULT_RPC_ADDR) 또는
 *               (구현이 지원하면) "tcp://addr:port" 같은 다른 transport. NULL은 비허용.
 * @opts:        RPC 로그 파일/레벨 등 추가 옵션. NULL이면 모든 필드 기본값 사용.
 * @return:      0=성공.
 *               음수(-errno) =bind/listen 실패 등. 대표적 실패:
 *                 -EADDRINUSE  이미 같은 경로에 다른 SPDK가 떠 있음.
 *                 -EACCES      권한 부족(소켓 생성 불가).
 *                 -ENOENT      상위 디렉토리 부재.
 *                 -ENOMEM      서버 컨텍스트/poller 할당 실패.
 *
 * 동작 흐름 (lib/init/rpc.c):
 *   1) socket(AF_UNIX, SOCK_STREAM, 0) 으로 listen 소켓 생성.
 *   2) bind() + listen() — 같은 SPDK 인스턴스에 여러 listen_addr 동시 운영 가능
 *      (예: 일반 RPC + 외부 노출용 보조 RPC).
 *   3) accept를 처리할 SPDK poller(period_us=0, busy-poll) 등록.
 *   4) 새 클라이언트 연결 시: JSON 디코드(spdk_json_decode_*) → 등록된 RPC 메서드
 *      디스패치(SPDK_RPC_REGISTER로 추가된 핸들러 테이블) → 결과를 JSON으로 인코딩하여
 *      응답.
 *
 * RPC phase 와의 관계:
 *   - 사용자가 spdk_subsystem_init 완료 *전에* 직접 본 함수를 부르면 RPC는
 *     STARTUP phase 메서드만 처리(framework_start_init 등 부트스트랩 한정).
 *   - subsystem_init 완료 후 본 함수를 부르면(또는 자동 phase 승격 후) 모든
 *     RUNTIME 메서드가 활성화된다. 일반 spdk_app_start 흐름에서는 라이브러리가
 *     적절한 시점에 자동 호출.
 *
 * 호출 컨텍스트: SPDK app thread (= main lcore의 spdk_thread)에서만 호출 가능.
 *               다른 스레드에서 부르면 RPC 메서드 등록 테이블/poller 등록 자료구조
 *               보호가 깨질 수 있다(lockless 설계).
 *
 * 호출 체인: spdk_app_start 내부 또는 사용자 start_fn → [spdk_rpc_initialize]
 *           → spdk_jsonrpc_server_listen → spdk_poller_register(accept_poller).
 */
int spdk_rpc_initialize(const char *listen_addr,
			const struct spdk_rpc_opts *opts);

/**
 * Stop SPDK JSON-RPC servers and stop polling for new connections on all addresses.
 *
 * This function should be called from the SPDK app thread.
 */
/*
 * [한국어]
 * spdk_rpc_finish - 모든 RPC 서버를 정지하고 listening 소켓·poller·연결을 정리.
 *
 * @return: void.
 *
 * 동작:
 *   1) 모든 listen_addr에 대해 accept_poller 해제.
 *   2) 각 listen 소켓 close + Unix domain socket 파일 unlink.
 *   3) 진행 중이던 클라이언트 연결도 close하고 in-flight JSON 응답은 폐기.
 *
 * 다중 listen_addr를 띄운 경우(spdk_rpc_initialize를 여러 번 호출했어도) 한 번에
 * 모두 정지된다. 특정 한 서버만 정지하려면 spdk_rpc_server_finish 사용.
 *
 * 호출 컨텍스트: SPDK app thread.
 * 에러 경로: 본 함수는 void 반환이며 일부 정리 실패는 로그만 남기고 진행한다
 *          — 종료 단계의 cleanup은 best-effort 정책.
 *
 * 호출 체인: spdk_app_start_shutdown → spdk_subsystem_fini 직전/직후 → [spdk_rpc_finish].
 *           보통 spdk_subsystem_fini 종료 시점 직후에 라이브러리가 자동 호출하지만,
 *           사용자가 명시적으로 RPC만 먼저 끄고 싶을 때 직접 호출도 가능.
 */
void spdk_rpc_finish(void);

/**
 * Stop SPDK JSON-RPC server and stop polling for new connections on provided address.
 *
 * This function should be called from the SPDK app thread.
 *
 * \param listen_addr Path to a unix domain socket.
 */
/*
 * [한국어]
 * spdk_rpc_server_finish - 특정 listen_addr에 묶인 RPC 서버 하나만 정지.
 *
 * @listen_addr: spdk_rpc_initialize 시 사용한 경로와 *문자열 비교 동일*해야 매칭됨
 *               (정규화 미수행 — "/var/tmp/spdk.sock"과 "//var/tmp/spdk.sock" 다름).
 * @return: void. 매칭되는 서버가 없어도 조용히 무시(에러 아님).
 *
 * 사용 시나리오: 다중 RPC 엔드포인트(예: 일반 관리 RPC + 보안 격리된 별도 RPC)를
 * 운영하다가 일부 엔드포인트만 끄고 싶을 때. 또는 phase 전환 중 임시 RPC 서버를
 * 생성했다가 회수하는 경우.
 *
 * 호출 컨텍스트: SPDK app thread.
 *
 * 호출 체인: 사용자 코드 또는 spdk_subsystem_load_config 내부 cleanup
 *           → [spdk_rpc_server_finish] → spdk_poller_unregister + close.
 */
void spdk_rpc_server_finish(const char *listen_addr);

typedef void (*spdk_subsystem_init_fn)(int rc, void *ctx);
/* [한국어] subsystem 초기화 완료 콜백 함수 포인터 타입.
 * @rc:  0=전체 init 성공, 음수(-errno)=실패한 subsystem이 있음.
 *       대표적 실패값: -ENOMEM (메모리 부족), -EIO (디바이스 probe 실패),
 *       -EINVAL (config 파싱 오류).
 * @ctx: spdk_subsystem_init() 또는 spdk_subsystem_load_config()에 전달했던
 *       cb_arg 값 그대로 — 사용자 컨텍스트 복원용.
 *
 * 호출 스레드: SPDK app thread (init 시퀀스를 시작한 그 스레드와 동일).
 *            cross-thread 콜백은 발생하지 않음 — message passing 불필요.
 * 호출 시점: 마지막 subsystem이 init_next()를 호출한 직후, 또는 도중 누군가
 *           실패 rc로 init_next를 호출한 직후 (stop_on_error 정책에 따라 분기).
 *
 * 사용자 동작 가이드:
 *   - rc == 0: 자기 워크로드 시작(I/O 발행, NVMe attach, RPC 메서드 사용 등).
 *   - rc != 0: 보통 spdk_app_stop(rc) 호출하여 graceful shutdown 트리거. */

/**
 * Begin the initialization process for all SPDK subsystems.
 *
 * This function should be called from the SPDK app thread.
 *
 * SPDK is divided into subsystems at a macro-level and each subsystem automatically registers
 * itself with this library at start up using a C constructor. Further, each subsystem can declare
 * other subsystems that it depends on. Calling this function will correctly initialize all
 * subsystems that are present, in the required order.
 *
 * \param cb_fn Function called when the process is complete.
 * \param cb_arg User context passed to cb_fn.
 */
/*
 * [한국어]
 * spdk_subsystem_init - 등록된 모든 subsystem을 의존성 위상정렬 순서로 비동기 초기화.
 *
 * @cb_fn:  모든 init 완료(또는 첫 실패) 시 호출될 콜백 (NULL 비허용).
 * @cb_arg: cb_fn에 그대로 전달될 사용자 컨텍스트(불투명 포인터).
 * @return: void. 동작 자체는 비동기 — 즉시 반환되고 결과는 cb_fn으로 보고.
 *
 * 동작 단계:
 *   1) lib/init이 보유한 전역 TAILQ<spdk_subsystem> g_subsystems 와
 *      TAILQ<spdk_subsystem_depend> g_subsystems_deps를 위상정렬해 init 순서 결정.
 *      예: env_dpdk → scheduler → accel → bdev → vmd → vhost/nvmf/iscsi/scsi.
 *   2) 첫 노드의 init_fn을 호출. 각 init_fn은 동기 작업(메모리 풀 생성)과
 *      비동기 작업(NVMe probe, 소켓 listen, RDMA queue pair 생성)이 섞여 있다.
 *   3) 각 subsystem이 자기 init 끝나면 spdk_subsystem_init_next(rc) 호출
 *      (내부 헤더 spdk_internal/init.h) → 다음 노드 init_fn 진행.
 *   4) 마지막 노드까지 완료되면 cb_fn(0, cb_arg).
 *      도중 누군가 init_next(음수)를 호출하면 즉시 cb_fn(rc, cb_arg) 호출 후 중단.
 *      (실패한 노드 이전까지는 init된 상태 — graceful shutdown 시 fini 처리 필요.)
 *
 * 비동기 콜백 모델 이유: 각 subsystem init이 RPC 응답·디바이스 probe·RDMA QP
 * 생성 등 시간이 걸리는 작업을 포함할 수 있어 동기 호출로는 reactor 폴링이 막히고
 * 다른 subsystem의 poller가 굶주린다(starvation). 비동기로 풀어 init 중에도
 * NVMe completion 폴링 같은 background 작업이 진행되도록 보장.
 *
 * SPDK_SUBSYSTEM_REGISTER 매크로(내부 헤더):
 *   SPDK_SUBSYSTEM_REGISTER(NAME)  →  __attribute__((constructor)) 로 자기 정보를
 *   g_subsystems에 push. SPDK_SUBSYSTEM_DEPEND(NAME, OTHER) → g_subsystems_deps에
 *   엣지(NAME → OTHER) 추가. 이 두 매크로는 컴파일 타임에 main() 진입 전에 실행되어
 *   spdk_subsystem_init이 호출될 시점에는 그래프가 완성되어 있다.
 *
 * 호출 컨텍스트: SPDK app thread. 그래프 자료구조는 단일 스레드 등록 + 단일 스레드
 *               순회를 가정하므로 락 없음.
 *
 * 호출 체인: spdk_app_start 또는 사용자 start_fn → [spdk_subsystem_init]
 *           → 각 subsystem init_fn → … → cb_fn(rc, cb_arg) → 사용자 워크로드 시작.
 */
void spdk_subsystem_init(spdk_subsystem_init_fn cb_fn, void *cb_arg);

/**
 * Loads RPC configuration from provided JSON for current RPC state.
 *
 * This function should be called from the SPDK app thread.
 *
 * The function will automatically start a JSON RPC server for configuration purposes and then stop
 * it. JSON data will be copied, so parsing will not disturb the original memory.
 *
 * \param json Raw JSON data.
 * \param json_size Size of JSON data.
 * \param cb_fn Function called when the process is complete.
 * \param cb_arg User context passed to cb_fn.
 * \param stop_on_error Whether to stop initialization if one of the JSON RPCs fails.
 */
/*
 * [한국어]
 * spdk_subsystem_load_config - JSON 텍스트에 적힌 RPC 시퀀스를 재생해 구성을 자동 적용.
 *
 * @json:           RPC 메서드 호출들이 배열로 인코딩된 JSON 버퍼
 *                  (예: `spdk_tgt`로 dump 한 결과 또는 사용자 설정파일 내용).
 *                  스키마: { "subsystems": [ { "subsystem": "bdev", "config": [ {RPC1}, ... ] }, ... ] }
 * @json_size:      json 버퍼의 바이트 수. ssize_t로 음수가 sentinel 의미를 가질 수
 *                  있도록 설계 — 일반적으로 strlen(json) 사용.
 * @cb_fn:          모든 RPC 재생 완료 시 호출될 콜백
 *                  (spdk_subsystem_init_fn과 동일 시그니처).
 * @cb_arg:         cb_fn에 전달될 사용자 컨텍스트.
 * @stop_on_error:  true=하나라도 실패하면 즉시 중단 후 cb_fn에 음수 rc 전달.
 *                  false=실패는 로그로 남기고 계속 진행 (json_config_ignore_errors와 매핑).
 *                  false는 부팅 시 일부 RPC가 실패해도 시스템을 부분 가동하고 싶을 때 사용.
 * @return: void. 비동기.
 *
 * 동작:
 *   1) json 데이터를 SPDK 내부 버퍼로 복사 — caller는 호출 직후 메모리 해제 가능.
 *   2) 임시(loopback) RPC 서버를 띄워 자기 자신에게 RPC를 in-process로 발행.
 *      이렇게 RPC 디스패처를 재사용하므로, JSON config와 외부 RPC가 동일한
 *      코드 경로를 거치게 되어 "config로 만든 것과 RPC로 만든 것이 다르다"는
 *      불일치를 원천 차단한다.
 *   3) JSON에 명시된 순서대로 메서드 디스패치. STARTUP phase RPC가 끝나면
 *      framework_start_init이 호출되어 RUNTIME phase로 승격, 이후 RUNTIME RPC들이
 *      재생된다.
 *   4) 모든 메서드 처리가 끝나면 임시 RPC 서버 정리 → cb_fn 호출.
 *
 * 호출 시점:
 *   - 일반적으로 spdk_app_opts.json_config_file 또는 json_data가 설정된 경우
 *     spdk_app_start가 spdk_subsystem_init 완료 직후 내부적으로 본 함수를 호출하여
 *     부팅 시점 자동 설정을 적용한다.
 *   - 사용자 코드가 직접 호출해 런타임에 추가 구성을 적용할 수도 있다 — 단,
 *     이미 초기화된 subsystem 위에서 동작하므로 충돌하는 RPC(같은 이름의 bdev
 *     중복 생성 등)는 stop_on_error 정책에 따라 처리.
 *
 * 호출 컨텍스트: SPDK app thread.
 * 에러 경로: stop_on_error=true 시 첫 실패에서 중단 → cb_fn(-rc, cb_arg).
 *          false 시 실패 무시하고 계속 → 모든 RPC 처리 후 cb_fn(0, cb_arg).
 *
 * 호출 체인: spdk_app_start → [spdk_subsystem_load_config]
 *           → 임시 RPC 서버 → 각 메서드 핸들러 → … → cb_fn.
 */
void spdk_subsystem_load_config(void *json, ssize_t json_size, spdk_subsystem_init_fn cb_fn,
				void *cb_arg, bool stop_on_error);

typedef void (*spdk_subsystem_fini_fn)(void *ctx);
/* [한국어] subsystem 종료 완료 콜백 함수 포인터 타입.
 * @ctx: spdk_subsystem_fini에 전달했던 cb_arg 값 그대로 — 사용자 컨텍스트 복원용.
 *
 * 호출 스레드: SPDK app thread (fini 시퀀스를 시작한 그 스레드와 동일).
 *
 * init 콜백과 달리 rc 인자가 없는 이유: fini는 best-effort cleanup이며 부분 실패
 * 가 있더라도 진행을 계속하는 방향으로 설계되어 있어 단일 종합 실패 코드를
 * 정의하지 않는다. 개별 subsystem이 자기 fini_fn 안에서 에러를 만나면 SPDK_ERRLOG로
 * 기록만 하고 fini_next()를 호출해 다음 단계로 진행한다.
 *
 * 사용자 동작 가이드:
 *   - 보통 spdk_app_stop(0) 또는 reactor 종료를 트리거하기 위해 호출 흐름의
 *     마지막에 위치 (spdk_app_start 사용 시 라이브러리가 자동 처리). */

/**
 * Tear down all of the subsystems in the correct order.
 *
 * This function should be called from the SPDK app thread.
 *
 * \param cb_fn Function called when the process is complete.
 * \param cb_arg User context passed to cb_fn
 */
/*
 * [한국어]
 * spdk_subsystem_fini - 등록 역순으로 모든 subsystem을 비동기 종료.
 *
 * @cb_fn:  모든 fini 완료 시 호출될 콜백 (spdk_subsystem_fini_fn 시그니처).
 * @cb_arg: cb_fn에 그대로 전달될 사용자 컨텍스트.
 * @return: void. 비동기.
 *
 * 동작:
 *   1) 위상정렬된 init 순서의 *역방향*으로 fini_fn을 차례 호출.
 *      예: vhost/nvmf → bdev → accel → scheduler → env_dpdk.
 *      이유: 의존하는 모듈(상위)이 의존되는 모듈(하위)보다 먼저 사라져야
 *           하위 자원에 매달린 위 모듈의 in-flight I/O가 dangling reference로
 *           되지 않는다(use-after-free 방지).
 *   2) 각 subsystem은 fini_fn 안에서 자기 자원(I/O qpair, mempool, RPC 메서드
 *      등록 해제, 소켓 close 등)을 정리하고 spdk_subsystem_fini_next() 호출 →
 *      다음 노드를 진행시킨다.
 *   3) 마지막 노드까지 완료되면 cb_fn(cb_arg) 호출.
 *
 * 비동기 이유: in-flight I/O 완료 대기가 필요할 수 있고(예: NVMe abort 후
 * outstanding completion 수신), reactor를 막지 않고 정리하기 위해.
 *
 * 호출 컨텍스트: SPDK app thread.
 * 에러 처리: 개별 fini_fn 실패는 로그로 남기고 진행 — 본 함수의 cb_fn에 별도 rc 없음.
 *
 * 호출 체인: SIGINT/SIGTERM → spdk_app_start_shutdown
 *           → [spdk_subsystem_fini] → 각 fini_fn → cb_fn → spdk_app_stop(0)
 *           → reactor 루프 탈출 → spdk_env_fini → main() 반환.
 */
void spdk_subsystem_fini(spdk_subsystem_fini_fn cb_fn, void *cb_arg);

/**
 * Check if the specified subsystem exists in the application.
 *
 * This function should be called from the SPDK app thread.
 *
 * \param name Name of the subsystem to look for
 * \return true if it exists, false if not
 */
/*
 * [한국어]
 * spdk_subsystem_exists - 이름으로 subsystem 등록 여부 조회.
 *
 * @name:   대소문자 구분 subsystem 이름 (예: "bdev", "nvmf", "iscsi", "vhost",
 *          "scheduler", "accel"). 정확 일치 검색.
 * @return: true=g_subsystems에 동일 이름 노드 존재.
 *          false=없음(빌드에 포함되지 않았거나 이름 오타).
 *
 * 사용 시나리오 (옵셔널 의존성):
 *   - 어떤 모듈이 다른 모듈의 기능을 *있다면 사용*하고 없다면 폴백하고 싶을 때.
 *   - 예: 어떤 bdev 모듈이 vhost를 통해 노출 가능하지만 vhost가 빌드되지 않았다면
 *        그냥 일반 bdev로만 동작하도록 분기.
 *   - SPDK_SUBSYSTEM_DEPEND 는 *필수* 의존성을 표현 — 없으면 빌드/런타임 에러.
 *     반면 본 함수는 *옵셔널* 의존성을 표현하기 위한 런타임 조회 수단.
 *
 * 호출 컨텍스트: SPDK app thread (등록 리스트는 단일 스레드 순회 가정).
 * 호출 시점 제약: spdk_subsystem_init 호출 시점 이후에 사용해야 의미 있음
 *               (그 전까지는 그래프가 자료구조에는 있지만 phase 정보가 미완).
 */
bool spdk_subsystem_exists(const char *name);

/**
 * Pause polling RPC server with given address.
 *
 * This function should be called from the SPDK app thread.
 *
 * \param listen_addr Address, on which RPC server listens for connections.
 */
/*
 * [한국어]
 * spdk_rpc_server_pause - 지정 주소에 묶인 RPC 서버의 accept/poll을 일시 중단.
 *
 * @listen_addr: spdk_rpc_initialize 시 사용한 주소와 *문자열 비교 동일*해야 매칭.
 * @return: void. 매칭 실패 시 조용히 무시.
 *
 * 동작 의미:
 *   - pause 중에는 새 클라이언트 연결의 accept_poller가 멈춘다 → 큐에 쌓인
 *     SYN은 backlog까지만 대기하고 그 이후는 클라이언트가 ECONNREFUSED 또는
 *     timeout 경험.
 *   - 기존에 이미 연결되어 in-flight 상태인 요청은 계속 처리된다 — 정상 응답
 *     완료 후 자연스럽게 idle.
 *
 * 사용 시나리오:
 *   - subsystem reload, hot-plug 디바이스 추가, NUMA scheduling 변경 등 시스템이
 *     전이 상태에 있는 동안 외부 RPC가 끼어들지 않도록 일시 정지.
 *   - 백업·snapshot 같은 일관성이 중요한 동작 직전 RPC 게이트.
 *
 * 호출 컨텍스트: SPDK app thread.
 *
 * 호출 체인: 사용자 코드 또는 내부 transition 핸들러 → [spdk_rpc_server_pause]
 *           → spdk_poller_pause(accept_poller).
 */
void spdk_rpc_server_pause(const char *listen_addr);

/**
 * Resume polling RPC server with given address.
 *
 * This function should be called from the SPDK app thread.
 *
 * \param listen_addr Address, on which RPC server listens for connections.
 */
/*
 * [한국어]
 * spdk_rpc_server_resume - 일시 중단된 RPC 서버 폴링을 재개.
 *
 * @listen_addr: pause 시 사용한 주소와 동일해야 매칭.
 * @return: void. 매칭 없거나 이미 실행 중이면 무시(에러 아님 — 멱등성).
 *
 * 동작: pause로 멈췄던 accept_poller를 다시 가동. backlog에 쌓여있던 SYN이 있다면
 *      즉시 accept되어 처리된다. 클라이언트 측에서는 일시적인 stall 후 정상 응답으로
 *      복귀하는 것처럼 보인다.
 *
 * 호출 컨텍스트: SPDK app thread. pause/resume은 항상 짝을 이루어 호출되는 것이
 *               원칙이며, transition 핸들러에서 시작점에 pause, 종료점에 resume.
 *
 * 호출 체인: 사용자 코드 또는 내부 transition 핸들러 → [spdk_rpc_server_resume]
 *           → spdk_poller_resume(accept_poller).
 */
void spdk_rpc_server_resume(const char *listen_addr);

#ifdef __cplusplus
/* [한국어] C++에서 열었던 extern "C" 블록 종결. */
}
#endif

#endif
/* [한국어] include guard 종결 (SPDK_INIT_H). 본 헤더의 모든 선언 영역 끝. */
