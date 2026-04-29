/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK 자체 RPC 시스템 공개 API (rpc.h) — 약 156 라인
 *
 * === 파일의 역할 ===
 * SPDK가 사용자 도구(rpc.py, spdk_top, 외부 오케스트레이터)와 통신하는
 * "관리(control-plane) 인터페이스"의 최상위 추상화. spdk/jsonrpc.h(JSON-RPC
 * 2.0 트랜스포트)를 한 단계 감싸 다음을 제공한다:
 *   (1) 메서드 등록 — SPDK_RPC_REGISTER(method, handler, state_mask) 매크로가
 *       __attribute__((constructor))로 main() 이전에 자동 등록.
 *   (2) 메서드 가시성 제어 — STARTUP/RUNTIME 두 단계의 state mask로,
 *       "RPC 서버가 STARTUP 상태일 때만 받는" 메서드(framework_start_init 등)와
 *       "RUNTIME에서만 받는" 메서드(bdev_get_bdevs 등)를 분리.
 *   (3) 별칭(deprecated alias) 등록 — 메서드 이름 변경 시 구버전 클라이언트 호환.
 *   (4) Allowlist — 보안상 일부 메서드만 노출하고 나머지를 차단.
 *   (5) RPC 서버 lifecycle — listen/accept/close.
 * 본 헤더는 표준 SPDK 앱(spdk_tgt, nvmf_tgt, vhost 등)이 RPC를 활성화하는 진입점이며,
 * 각 서브시스템(bdev, nvme, sock, ...)은 본 매크로로 자기 메서드를 등록한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 앱 시작 시퀀스:
 *   spdk_app_start() → 서브시스템 초기화 (constructor 실행으로 모든
 *   SPDK_RPC_REGISTER 메서드가 전역 표에 등록됨) → spdk_rpc_server_listen()
 *   ("/var/tmp/spdk.sock"로 listen) → reactor poller가 spdk_rpc_server_accept()
 *   주기 호출 → 클라이언트 요청 도착 → spdk_jsonrpc_handle_request_fn(jsonrpc.h)
 *   디스패치 콜백이 method 이름으로 표 룩업 → 매치된 spdk_rpc_method_handler 호출.
 * 클라이언트 측: scripts/rpc.py → Python JSON-RPC 클라이언트 → Unix domain socket →
 * 위 디스패치 → 핸들러 → JSON 응답 → rpc.py가 사용자에게 출력.
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/stdinc.h(uint32_t/bool 등), spdk/jsonrpc.h(JSON-RPC 트랜스포트와
 *             spdk_jsonrpc_request, spdk_json_val 타입).
 * 의존(구현): lib/rpc/rpc.c가 메서드 등록 표(global linked list)와 디스패치 콜백 구현.
 *             별칭/state_mask/allowlist 검사도 여기서 수행. lib/rpc/rpc_internal.h 참고.
 * 데이터 흐름: 호환 클라이언트(rpc.py 등)의 JSON 입력 → jsonrpc 파싱 → method 룩업 →
 *             핸들러가 params 디코드 → 작업 수행 → 응답 작성 → 송신.
 * 공유 자료구조: 전역 method 등록 표(struct spdk_rpc_method 리스트), 별칭 표,
 *             현재 RPC 서버 state(uint32_t bitmask), allowlist(string 배열).
 *             모두 lib/rpc/rpc.c에 정의되며 SPDK app thread에서만 다뤄진다.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_rpc_server: 불투명 — listen 소켓 + jsonrpc_server 포함.
 *   - spdk_rpc_verify_methods: 등록 표의 정합성 검사(중복/잘못된 별칭 탐지).
 *   - spdk_rpc_server_listen / _accept / _close: 서버 lifecycle.
 *   - typedef spdk_rpc_method_handler: (request, params) 시그니처의 핸들러.
 *   - spdk_rpc_register_method: 메서드 + state_mask 등록(보통 매크로로 호출).
 *   - spdk_rpc_register_alias_deprecated: 구버전 별칭 등록.
 *   - spdk_rpc_is_method_allowed / _get_method_state_mask: 가시성 검사 헬퍼.
 *   - SPDK_RPC_STARTUP / SPDK_RPC_RUNTIME: state 비트 정의.
 *   - SPDK_RPC_REGISTER / SPDK_RPC_REGISTER_ALIAS_DEPRECATED: constructor 자동 등록.
 *   - spdk_rpc_set_state / _get_state: 서버의 현재 state 토글.
 *   - spdk_rpc_set_allowlist: 보안용 메서드 화이트리스트 적용.
 */

#ifndef SPDK_RPC_CONFIG_H_      /* [한국어] include 가드 — 다중 포함 방지(파일명과 다른 이름은 역사적 유산) */
#define SPDK_RPC_CONFIG_H_

#include "spdk/stdinc.h"        /* [한국어] uint32_t/bool 등 표준 타입 */

#include "spdk/jsonrpc.h"       /* [한국어] spdk_jsonrpc_request, spdk_json_val — 핸들러 시그니처에서 사용 */

/**
 * It is required to invoke API functions on the SPDK app thread;
 * otherwise, race conditions may lead to undefined behavior.
 */
/* [한국어] 본 헤더 모든 API는 SPDK app thread에서만 호출. 메서드 등록 표가 lockless. */

#ifdef __cplusplus
extern "C" {                    /* [한국어] C++에서도 본 헤더 사용 가능하도록 C 링키지 강제 */
#endif

struct spdk_rpc_server;
/* [한국어] RPC 서버 핸들 — 불투명 타입. 정의는 lib/rpc/rpc.c.
 * 내부에 spdk_jsonrpc_server, listen 주소, 활성 연결 리스트 등을 보관.
 * 한 SPDK 앱이 여러 서버를 동시에 가질 수 있다(예: local UDS + 원격 TCP). */

/**
 * Verify correctness of registered RPC methods and aliases.
 *
 * Incorrect registrations include:
 * - multiple RPC methods registered with the same name
 * - RPC alias registered with a method that does not exist
 * - RPC alias registered that points to another alias
 *
 * \return true if registrations are all correct, false otherwise
 */
/*
 * [한국어]
 * spdk_rpc_verify_methods - constructor로 자동 등록된 메서드/별칭 표의 정합성 점검.
 *
 * @return: 모두 정상이면 true, 하나라도 위반이면 false.
 *
 * 검출하는 오류:
 *   - 같은 이름의 메서드가 두 번 이상 등록됨(constructor 충돌).
 *   - 존재하지 않는 메서드를 가리키는 별칭.
 *   - 별칭이 다른 별칭을 가리킴(체이닝 금지 — 단일 hop만 허용).
 *
 * SPDK app 시작 시 spdk_rpc_server_listen() 직전에 호출해 빌드 오류를 조기 탐지.
 *
 * 호출 체인: spdk_app_start → spdk_rpc_initialize → spdk_rpc_verify_methods
 */
bool spdk_rpc_verify_methods(void);

/**
 * Start listening for RPC connections on given address.
 *
 * \param listen_addr Listening address.
 *
 * \return new RPC server or NULL on failure.
 */
/*
 * [한국어]
 * spdk_rpc_server_listen - 지정 주소에서 RPC 서버 listen 시작.
 *
 * @listen_addr: AF_UNIX path(예: "/var/tmp/spdk.sock", 기본) 또는 IP:port.
 * @return:      서버 핸들 또는 NULL(실패 — errno에 사유, 보통 EADDRINUSE).
 *
 * 내부에서 주소 형식을 보고 적절한 family(AF_UNIX/AF_INET)를 선택해
 * spdk_jsonrpc_server_listen()을 호출하고, 그 위에 메서드 등록 표 디스패치 콜백을 장착.
 */
struct spdk_rpc_server *spdk_rpc_server_listen(const char *listen_addr);

/**
 * Poll the RPC server.
 *
 * \param server RPC server, which will be polled for connections.
 */
/*
 * [한국어]
 * spdk_rpc_server_accept - 한 번의 polling iteration — 신규 연결 accept + 활성 연결 처리.
 * @server: spdk_rpc_server_listen() 반환 핸들.
 *
 * 내부적으로 spdk_jsonrpc_server_poll()을 호출. SPDK reactor의 poller로 등록되어
 * 매 iteration 호출되는 것이 표준 패턴. non-blocking — 처리할 일이 없으면 즉시 반환.
 */
void spdk_rpc_server_accept(struct spdk_rpc_server *server);

/**
 * Stop a server from listening and free it.
 *
 * \param server RPC server, which will be stopped and be freed.
 */
/*
 * [한국어]
 * spdk_rpc_server_close - 서버 종료 + 자원 해제.
 * @server: 닫을 서버. 호출 후 server는 무효 — 다시 사용 금지.
 *
 * 내부적으로 spdk_jsonrpc_server_shutdown 호출 → listen 소켓/모든 연결 닫힘 →
 * UDS의 경우 socket 파일도 unlink. SPDK app 종료 시 호출.
 */
void spdk_rpc_server_close(struct spdk_rpc_server *server);

/**
 * Function signature for RPC request handlers.
 *
 * \param request RPC request to handle.
 * \param params Parameters associated with the RPC request.
 */
/*
 * [한국어] RPC 메서드 핸들러 시그니처 — 디스패치 시점에 호출되는 사용자 함수.
 *
 * @request: 응답을 작성할 jsonrpc 요청 컨텍스트(spdk/jsonrpc.h 참조).
 * @params:  요청의 params 토큰(객체/배열/null) — 핸들러가 spdk_json_decode_object 등으로 풀어 사용.
 *
 * 핸들러는 반드시 spdk_jsonrpc_begin_result+_end_result 또는 _send_error_response_*로
 * 응답해야 한다. 호출 컨텍스트는 SPDK app thread.
 */
typedef void (*spdk_rpc_method_handler)(struct spdk_jsonrpc_request *request,
					const struct spdk_json_val *params);

/**
 * Register an RPC method.
 *
 * \param method Name for the registered method.
 * \param func Function registered for this method to handle the RPC request.
 * \param state_mask State mask of the registered method. If the bit of the state of
 * the RPC server is set in the state_mask, the method is allowed. Otherwise, it is rejected.
 */
/*
 * [한국어]
 * spdk_rpc_register_method - 메서드를 전역 디스패치 표에 등록.
 *
 * @method:     RPC method 이름(string literal — 표 lifetime 동안 유효해야).
 * @func:       처리 핸들러.
 * @state_mask: 가시성 비트마스크(SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME 등).
 *              현재 서버 state(spdk_rpc_get_state)와 AND가 0이 아니면 호출 허용.
 *
 * 보통 직접 호출하지 않고 SPDK_RPC_REGISTER 매크로를 통해 constructor에서 자동 호출.
 */
void spdk_rpc_register_method(const char *method, spdk_rpc_method_handler func,
			      uint32_t state_mask);

/**
 * Register a deprecated alias for an RPC method.
 *
 * \param method Name for the registered method.
 * \param alias Alias for the registered method.
 */
/*
 * [한국어]
 * spdk_rpc_register_alias_deprecated - 구버전 클라이언트용 별칭 등록.
 *
 * @method: 실제 메서드 이름(이미 register_method로 등록되어 있어야 함).
 * @alias:  과거 이름. 클라이언트가 alias를 호출하면 method로 라우팅 + deprecation 경고.
 *
 * 메서드 이름이 변경될 때 호환성 유지에 사용. 별칭의 별칭은 금지(verify_methods가 거부).
 * 보통 SPDK_RPC_REGISTER_ALIAS_DEPRECATED 매크로로 호출.
 */
void spdk_rpc_register_alias_deprecated(const char *method, const char *alias);

/**
 * Check if \c method is allowed for \c state_mask
 *
 * \param method Method name
 * \param state_mask state mask to check against
 * \return 0 if method is allowed or negative error code:
 * -EPERM method is not allowed
 * -ENOENT method not found
 */
/*
 * [한국어]
 * spdk_rpc_is_method_allowed - 주어진 state에서 메서드 호출 가능 여부 검사.
 *
 * @method:     검사할 메서드 이름.
 * @state_mask: 비교할 state 비트마스크(보통 spdk_rpc_get_state 결과).
 * @return:     0 허용, -EPERM 거부(state 불일치), -ENOENT 메서드 없음.
 *
 * 디스패치 콜백이 핸들러 호출 전 본 함수로 검사 → 거부 시 INVALID_STATE 에러 응답.
 */
int spdk_rpc_is_method_allowed(const char *method, uint32_t state_mask);

/**
 * Return state mask of the method
 *
 * \param method Method name
 * \param[out] state_mask State mask of the method
 * \retval 0 if method is found and \b state_mask is filled
 * \retval -ENOENT if method is not found
 */
/*
 * [한국어]
 * spdk_rpc_get_method_state_mask - 메서드 등록 시 지정된 state_mask 조회.
 *
 * @method:        조회할 메서드 이름.
 * @state_mask:    [out] 등록된 마스크.
 * @return:        0 성공, -ENOENT 메서드 없음.
 *
 * rpc_get_methods 같은 introspection RPC에서 메서드별 가시성을 클라이언트에 노출할 때 사용.
 */
int spdk_rpc_get_method_state_mask(const char *method, uint32_t *state_mask);

#define SPDK_RPC_STARTUP	0x1
/* [한국어] RPC 서버 state 비트 — STARTUP 단계.
 * spdk_app_start()로 들어가서 framework_start_init이 호출되기 전까지의 phase.
 * "초기 설정만 받는 메서드"(예: bdev_set_options, sock_impl_set_options)가 이 비트로 등록된다. */
#define SPDK_RPC_RUNTIME	0x2
/* [한국어] RPC 서버 state 비트 — RUNTIME 단계.
 * framework_start_init 완료 후 정상 운영 phase. 대부분의 조회/I/O 관련 메서드가 이 비트로 등록.
 * STARTUP|RUNTIME 둘 다 켜면 양쪽 phase에서 모두 호출 가능(드물게 사용). */

/* Give SPDK_RPC_REGISTER a higher execution priority than
 * SPDK_RPC_REGISTER_ALIAS_DEPRECATED to ensure all of the RPCs are registered
 * before we try registering any aliases.  Some older versions of clang may
 * otherwise execute the constructors in a different order than
 * defined in the source file (see issue #892).
 */
/*
 * [한국어] 메서드 등록을 main() 이전에 자동 수행하는 매크로.
 *
 * __attribute__((constructor(1000)))은 GCC/Clang의 ELF init array 우선순위 1000으로
 * 등록. 별칭 매크로는 1001을 사용하므로 "메서드 먼저 등록 → 별칭 나중 등록" 순서를 보장.
 * (clang 일부 버전은 소스 순서를 무시하고 constructor 순서를 재배열할 수 있어 명시적 priority가 필요)
 *
 * 사용 예:
 *   static void rpc_bdev_get_bdevs(struct spdk_jsonrpc_request *r, const struct spdk_json_val *p) { ... }
 *   SPDK_RPC_REGISTER("bdev_get_bdevs", rpc_bdev_get_bdevs, SPDK_RPC_RUNTIME)
 *
 * 함수명에 ##func로 결합되어 동일 핸들러를 두 번 등록할 수 없도록 컴파일러가 보호.
 */
#define SPDK_RPC_REGISTER(method, func, state_mask) \
static void __attribute__((constructor(1000))) rpc_register_##func(void) \
{ \
	spdk_rpc_register_method(method, func, state_mask); \
}

/*
 * [한국어] 별칭(deprecated) 자동 등록 매크로.
 *
 * @method: 실제 메서드 식별자(stringify로 #method 변환).
 * @alias:  구버전 이름 식별자(stringify로 #alias 변환).
 *
 * priority 1001 — 모든 SPDK_RPC_REGISTER(1000) 등록이 끝난 뒤 실행되어, 존재하는 메서드를
 * 가리킴이 보장된다.
 *
 * 사용 예:
 *   SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_get_bdevs, get_bdevs)
 *   → 클라이언트가 "get_bdevs"를 호출하면 "bdev_get_bdevs"로 라우팅 + 경고 로그.
 */
#define SPDK_RPC_REGISTER_ALIAS_DEPRECATED(method, alias) \
static void __attribute__((constructor(1001))) rpc_register_##alias(void) \
{ \
	spdk_rpc_register_alias_deprecated(#method, #alias); \
}

/**
 * Set the state mask of the RPC server. Any RPC method whose state mask is
 * equal to the state of the RPC server is allowed.
 *
 * \param state_mask New state mask of the RPC server.
 */
/*
 * [한국어]
 * spdk_rpc_set_state - RPC 서버의 현재 state 비트마스크 설정.
 *
 * @state_mask: 새 state(SPDK_RPC_STARTUP / SPDK_RPC_RUNTIME 또는 둘의 조합).
 *
 * 호출 타이밍:
 *   - spdk_app_start 직후에 STARTUP으로 설정 → STARTUP 마스크 메서드만 허용.
 *   - framework_start_init 완료 후 RUNTIME으로 전환 → RUNTIME 메서드 활성화.
 * 본 함수는 등록 표를 변경하지 않고 단지 "어떤 비트가 켜진 메서드가 허용되는가"의
 * 기준만 바꾼다. 디스패치 시 spdk_rpc_is_method_allowed로 검사.
 */
void spdk_rpc_set_state(uint32_t state_mask);

/**
 * Get the current state of the RPC server.
 *
 * \return The current state of the RPC server.
 */
/*
 * [한국어]
 * spdk_rpc_get_state - 현재 RPC 서버 state 비트마스크 조회.
 * @return: 현재 state. 디스패치 콜백이 method 허용 여부 판정에 사용.
 */
uint32_t spdk_rpc_get_state(void);

/*
 * Mark only the given RPC methods as allowed.
 *
 * \param rpc_allowlist string array of method names, terminated with a NULL.
 */
/*
 * [한국어]
 * spdk_rpc_set_allowlist - 지정된 메서드만 허용하는 화이트리스트 설정.
 *
 * @rpc_allowlist: NULL-종결 문자열 배열(예: {"bdev_get_bdevs","spdk_get_version",NULL}).
 *
 * 보안 강화 옵션 — production 환경에서 위험한 admin 메서드를 잠그고 read-only API만
 * 노출하는 등의 용도. 호출 후에는 allowlist에 없는 메서드는 state_mask와 무관하게 거부.
 * 매개변수 배열은 호출 후에도 lifetime 동안 유효해야 함(라이브러리가 복사하지 않을 수 있음).
 */
void spdk_rpc_set_allowlist(const char **rpc_allowlist);

#ifdef __cplusplus
}                       /* [한국어] extern "C" 종결 */
#endif

#endif                  /* [한국어] SPDK_RPC_CONFIG_H_ — include 가드 종결 */
