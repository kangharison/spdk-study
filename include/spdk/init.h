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
 * SPDK는 bdev, accel, nvmf, vhost, sock, iscsi 등 수많은 서브시스템으로 macro-level
 * 분리되어 있고, 각각은 C 생성자(`__attribute__((constructor))`)로 자기를 등록하면서
 * 의존하는 다른 서브시스템 이름을 선언한다. `spdk_subsystem_init()`은 이 그래프를
 * 위상정렬해 init_fn을 차례로 호출하고, 모든 비동기 init이 완료되면 사용자 콜백을 부른다.
 * 종료 단계도 역순으로 fini_fn을 호출하여 자원 누수를 방지한다.
 * RPC 서버 함수들은 init과 독립적으로 시작/정지/일시정지가 가능하며, JSON config 자동
 * 적용(`spdk_subsystem_load_config`) 또한 RPC 메서드를 재생하는 형태로 구현되어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   main() → spdk_app_start(opts, start_fn, ctx) [event.h]
 *     → reactor 가동 후 main lcore에서 start_fn 호출
 *       (또는 spdk_app_start 내부에서 자동) → spdk_subsystem_init(cb, arg) [본 헤더]
 *         → 등록된 모든 subsystem의 init_fn(SPDK_INIT_COMPLETE) 비동기 체인
 *         → 마지막 subsystem init 완료 시 cb(rc, arg) 호출
 *   종료 시:
 *     SIGTERM → spdk_app_start_shutdown → spdk_subsystem_fini(cb, arg) [본 헤더]
 *         → 등록 역순으로 fini_fn 호출 → cb(arg)
 *   부팅 시 RPC:
 *     spdk_rpc_initialize(addr) → 별도 thread에서 listen, accept, JSON 디코드.
 * 실행 컨텍스트: 모든 함수는 SPDK app thread (main lcore의 spdk_thread)에서 호출되어야
 * 한다. cross-thread에서 부르면 subsystem 등록 리스트가 보호되지 않아 UB.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h, spdk/queue.h, spdk/log.h, spdk/assert.h.
 * 의존받는 쪽: lib/init/(subsystem.c, rpc.c — 이 헤더의 구현체), lib/event/app.c
 * (spdk_app_start 내부에서 spdk_subsystem_init 호출), 모든 SPDK 메인 앱.
 * 데이터 흐름: 각 서브시스템 모듈(예: lib/bdev/bdev.c)이 SPDK_SUBSYSTEM_REGISTER 매크로로
 * 자기 정보(`struct spdk_subsystem`: name, init_fn, fini_fn, write_config_json 등)를
 * lib/init의 전역 TAILQ에 푸시 → spdk_subsystem_init이 그 리스트를 위상정렬·순회하며
 * 각 init_fn을 호출 → 각 모듈은 끝나면 spdk_subsystem_init_next() 호출(내부 헤더)
 * 으로 다음 모듈을 진행시킨다. 본 헤더는 그 *공개* 트리거만 노출.
 *
 * === 주요 함수/구조체 요약 ===
 * - SPDK_DEFAULT_RPC_ADDR: 기본 RPC 소켓 경로(/var/tmp/spdk.sock).
 * - struct spdk_rpc_opts: RPC 서버 부가 옵션(로그 파일/레벨). ABI를 위해 size 필드 보유.
 * - spdk_rpc_initialize / spdk_rpc_finish / spdk_rpc_server_finish: RPC 서버 시작·정지.
 * - spdk_subsystem_init(cb_fn, cb_arg): 모든 subsystem을 의존 순서대로 비동기 초기화.
 * - spdk_subsystem_load_config(json, size, cb, arg, stop_on_error): JSON에서
 *   RPC 시퀀스를 읽어 재생하여 구성을 적용한 뒤 init 완료를 보고.
 * - spdk_subsystem_fini(cb_fn, cb_arg): 등록 역순으로 모든 subsystem 종료.
 * - spdk_subsystem_exists(name): 특정 subsystem 등록 여부 조회 (옵셔널 의존성 검사용).
 * - spdk_rpc_server_pause/resume: 살아있는 RPC 서버의 폴링을 잠시 멈추거나 재개.
 */

#ifndef SPDK_INIT_H             /* [한국어] include guard 시작 — 다중 인클루드 방지. */
#define SPDK_INIT_H             /* [한국어] include guard 매크로 정의. */

#include "spdk/stdinc.h"        /* [한국어] 표준 헤더 모음 — size_t, FILE*, ssize_t, bool 등에 필요. */
#include "spdk/queue.h"         /* [한국어] BSD TAILQ 매크로 — subsystem 등록 리스트(내부)와 호환을 위해 인클루드. */
#include "spdk/log.h"           /* [한국어] enum spdk_log_level — spdk_rpc_opts.log_level 필드에 사용. */
#include "spdk/assert.h"        /* [한국어] SPDK_STATIC_ASSERT — spdk_rpc_opts 의 sizeof를 컴파일 타임에 고정. */

#ifdef __cplusplus              /* [한국어] C++에서 인클루드 시 이름 맹글링 회피를 위해 extern "C" 블록을 연다. */
extern "C" {                    /* [한국어] 이하 모든 선언은 C linkage. */
#endif

#define SPDK_DEFAULT_RPC_ADDR "/var/tmp/spdk.sock"
/* [한국어] SPDK가 제공하는 기본 JSON-RPC 리스닝 주소 (Unix domain socket).
 * 사용자가 spdk_app_opts.rpc_addr나 spdk_rpc_initialize의 listen_addr를 NULL로 두면
 * 라이브러리가 이 경로를 사용한다. /var/tmp는 tmpfs라 권한 충돌이 적고 컨테이너 친화적이다.
 * 동일 호스트에서 SPDK 인스턴스를 여러 개 띄울 때는 명시적으로 다른 경로를 지정해야 한다. */

/**
 * Structure with optional parameters for the JSON-RPC server initialization.
 */
/*
 * [한국어]
 * struct spdk_rpc_opts - JSON-RPC 서버 부팅 옵션 컨테이너.
 *
 * spdk_app_opts와 마찬가지로 ABI 호환을 위해 첫 필드를 size로 두고 caller가 본
 * sizeof를 전달하도록 설계. 라이브러리는 size까지만 신뢰하고 나머지는 기본값으로 채운다.
 */
struct spdk_rpc_opts {
	/* Size of this structure in bytes. */
	size_t size;
	/* [한국어] 호출자 시점의 sizeof(struct spdk_rpc_opts).
	 * 설정자: 사용자(반드시 sizeof로 채움).
	 * 읽는 자: spdk_rpc_initialize 내부 — 이 크기까지만 필드 신뢰.
	 * 값 범위: >0. 0이면 라이브러리가 호환 모드로 모두 기본값 처리. */

	/*
	 * A JSON-RPC log file pointer. The default value is NULL and used
	 * when options are omitted.
	 */
	FILE *log_file;
	/* [한국어] RPC 호출/응답을 별도 파일로 남기고 싶을 때의 FILE* 핸들.
	 * 설정자: 사용자가 fopen으로 열어 전달.
	 * 읽는 자: lib/init의 RPC 서버 핸들러 — 메서드 호출 직전 fwrite.
	 * 값 범위: 유효 FILE* 또는 NULL(=별도 RPC 로그 파일 사용 안 함). */

	/*
	 * JSON-RPC log level. Default value is SPDK_LOG_DISABLED and used
	 * when options are omitted.
	 */
	enum spdk_log_level log_level;
	/* [한국어] RPC 호출 자체에 적용할 로그 레벨.
	 * 설정자: 사용자.
	 * 읽는 자: RPC 서버가 호출 디스패치 시 spdk_log에 이 레벨로 기록.
	 * 값 범위: SPDK_LOG_DISABLED..SPDK_LOG_DEBUG. DISABLED면 RPC 별도 로그 없음. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_rpc_opts) == 24, "Incorrect size");
/* [한국어] sizeof 컴파일 타임 고정 — ABI 일관성. 필드 추가 시 이 상수도 함께 갱신. */

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
 *               "tcp://addr:port" 같은 다른 transport (구현 의존).
 * @opts:        RPC 로그 파일/레벨 등 추가 옵션. NULL이면 기본값.
 * @return:      0=성공, 음수(-errno)=bind/listen 실패 등.
 *
 * 동작: 소켓 bind → listen → SPDK poller로 accept 처리 등록. 새 클라이언트가 붙으면
 *       JSON 디코드 → 등록된 RPC 메서드 디스패치 → JSON 응답 인코딩.
 *
 * 호출 컨텍스트: SPDK app thread (= main lcore의 spdk_thread)에서만 호출 가능.
 *               다른 스레드에서 부르면 RPC 메서드 등록 테이블 보호가 깨질 수 있다.
 *
 * 호출 체인: spdk_app_start 내부 또는 사용자 start_fn → [spdk_rpc_initialize] → poller 등록.
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
 * 다중 listen_addr를 띄운 경우에도 한 번에 모두 정지된다.
 * 호출 컨텍스트: SPDK app thread.
 *
 * 호출 체인: spdk_app_start_shutdown → spdk_subsystem_fini 직전/직후 → [spdk_rpc_finish].
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
 * @listen_addr: spdk_rpc_initialize 시 사용한 경로와 동일해야 매칭됨.
 * @return: void. 매칭되는 서버가 없어도 조용히 무시.
 *
 * 다중 RPC 엔드포인트(예: 일반 RPC + 보안 격리된 관리 RPC)를 운영할 때 일부만 끌 때 사용.
 * 호출 컨텍스트: SPDK app thread.
 */
void spdk_rpc_server_finish(const char *listen_addr);

typedef void (*spdk_subsystem_init_fn)(int rc, void *ctx);
/* [한국어] subsystem 초기화 완료 콜백 타입.
 * @rc:  0=전체 init 성공, 음수=실패한 subsystem이 있음 (-errno).
 * @ctx: spdk_subsystem_init/load_config에 전달했던 cb_arg 그대로.
 * 호출 스레드: SPDK app thread (init 시퀀스를 시작한 그 스레드와 동일).
 * 사용자 코드는 이 콜백 안에서 자기 워크로드 시작(I/O 발행 등) 단계로 진입한다. */

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
 * @cb_fn:  모든 init 완료(또는 첫 실패) 시 호출될 콜백.
 * @cb_arg: cb_fn에 그대로 전달될 사용자 컨텍스트.
 * @return: void. 동작 자체는 비동기 — 즉시 반환되고 결과는 cb_fn으로 보고.
 *
 * 동작:
 *   1) 등록 리스트(SPDK_SUBSYSTEM_REGISTER로 push된 spdk_subsystem들)를 의존성 그래프로 변환.
 *   2) 위상정렬 후 첫 노드의 init_fn(spdk_subsystem_init_next) 호출.
 *   3) 각 subsystem이 자기 init 끝나면 spdk_subsystem_init_next 호출 → 다음 노드 진행.
 *   4) 마지막 노드까지 완료되면 cb_fn(0, cb_arg). 도중 실패면 cb_fn(-errno, cb_arg).
 *
 * 비동기 콜백 모델 이유: 각 subsystem init이 RPC 응답·디바이스 probe 등 시간이 걸리는
 * 작업을 포함할 수 있어 동기 호출로는 reactor 폴링이 막히기 때문.
 *
 * 호출 체인: spdk_app_start 또는 사용자 start_fn → [spdk_subsystem_init] → 각 subsystem init_fn
 *           → … → cb_fn(rc, cb_arg) → 사용자 워크로드 시작.
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
 * @json:           RPC 메서드 호출들이 배열로 인코딩된 JSON 버퍼 (예: spdk_tgt config dump 결과).
 * @json_size:      json 버퍼의 바이트 수. 음수 ssize_t 허용은 -1 등 sentinel용.
 * @cb_fn:          모든 RPC 재생 완료 시 호출될 콜백 (spdk_subsystem_init_fn과 동일 시그니처).
 * @cb_arg:         cb_fn에 전달될 사용자 컨텍스트.
 * @stop_on_error:  true=하나라도 실패하면 즉시 중단 후 cb_fn에 음수 rc 전달.
 *                  false=실패는 로그로 남기고 계속 진행 (json_config_ignore_errors와 매핑).
 * @return: void. 비동기.
 *
 * 동작:
 *   - json 데이터를 SPDK 내부 버퍼로 복사 (caller 메모리 즉시 free 가능).
 *   - 임시 RPC 서버를 띄워 자기 자신에게 RPC를 발행하는 형태로 메서드를 재생.
 *   - 모든 메서드 처리가 끝나면 임시 서버 정리 → cb_fn 호출.
 *
 * 일반적으로 spdk_app_opts.json_config_file 또는 json_data가 설정된 경우 spdk_app_start가
 * 내부적으로 이 함수를 호출하여 부팅 직후 자동 설정을 적용한다.
 */
void spdk_subsystem_load_config(void *json, ssize_t json_size, spdk_subsystem_init_fn cb_fn,
				void *cb_arg, bool stop_on_error);

typedef void (*spdk_subsystem_fini_fn)(void *ctx);
/* [한국어] subsystem 종료 완료 콜백 타입.
 * @ctx: spdk_subsystem_fini에 전달한 cb_arg 그대로.
 * 호출 스레드: SPDK app thread.
 * init 콜백과 달리 rc 인자가 없는 이유: fini는 best-effort cleanup이며 부분 실패가
 * 있더라도 진행을 계속하는 방향으로 설계되어 있어 단일 종합 실패 코드를 정의하지 않음. */

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
 * @cb_fn:  모든 fini 완료 시 호출될 콜백.
 * @cb_arg: cb_fn에 그대로 전달될 사용자 컨텍스트.
 * @return: void. 비동기.
 *
 * 의존성 그래프의 역방향으로 진행 — 예: nvmf_tgt가 bdev에 의존하면 nvmf_tgt를 먼저 내림.
 * 각 subsystem은 fini_fn 안에서 자기 자원(I/O qpair, mempool, RPC 메서드 등록 해제 등)을
 * 정리하고 spdk_subsystem_fini_next()를 호출해 다음 노드를 진행시킨다.
 *
 * 호출 체인: SIGINT → spdk_app_start_shutdown → [spdk_subsystem_fini] → cb_fn → spdk_app_stop(0).
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
 * @name:   대소문자 구분 subsystem 이름 (예: "bdev", "nvmf", "iscsi").
 * @return: true=존재, false=없음.
 *
 * 옵셔널 의존성 검사에 사용 — 예: vhost subsystem이 nvmf의 기능을 활용하고 싶지만
 * nvmf가 빌드/링크되지 않았다면 다른 경로로 폴백.
 *
 * 호출 컨텍스트: SPDK app thread (등록 리스트 보호 위해).
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
 * @listen_addr: spdk_rpc_initialize 시 사용한 주소와 동일해야 매칭.
 * @return: void.
 *
 * pause 중에는 새 연결은 받지 않지만 기존 연결의 in-flight 요청은 처리되도록 허용.
 * subsystem reload 같은 상태 전이 동안 외부 RPC 간섭을 막을 때 사용.
 *
 * 호출 컨텍스트: SPDK app thread.
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
 * @return: void. 매칭 없거나 이미 실행 중이면 무시.
 *
 * 호출 컨텍스트: SPDK app thread. pause/resume은 짝을 이루어 호출되는 것이 원칙.
 */
void spdk_rpc_server_resume(const char *listen_addr);

#ifdef __cplusplus              /* [한국어] C++ extern "C" 블록 종결. */
}
#endif

#endif                          /* [한국어] include guard 종결 (SPDK_INIT_H). */
