/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/**
 * \file
 * JSON-RPC 2.0 server implementation
 */

/*
 * [한국어 설명] JSON-RPC 2.0 server/client 공개 API (jsonrpc.h) — 약 357 라인
 *
 * === 파일의 역할 ===
 * SPDK가 사용하는 JSON-RPC 2.0(https://www.jsonrpc.org/specification) 프로토콜의
 * 서버/클라이언트 추상화를 정의한다. 본 헤더는 두 가지 측면을 모두 다룬다:
 *   (1) Server: spdk_jsonrpc_server_listen()으로 listen 소켓을 만들고,
 *       spdk_jsonrpc_server_poll()을 reactor poller에서 주기 호출하여 accept/recv/
 *       parse/dispatch까지 일괄 처리한다. 사용자 콜백
 *       spdk_jsonrpc_handle_request_fn이 메서드 이름과 파라미터를 받아 응답을 작성.
 *   (2) Client: spdk_jsonrpc_client_connect()로 서버에 연결하고,
 *       spdk_jsonrpc_client_create_request()/_send_request()로 요청을 보내고,
 *       spdk_jsonrpc_client_poll()/_get_response()로 응답을 회수한다.
 * 이 헤더의 모든 함수는 단일 SPDK app thread에서만 호출되어야 한다(헤더 상단 NOTE).
 * 응답은 spdk_jsonrpc_begin_result()(=spdk_json_write_ctx 반환) → JSON writer로
 * 작성 → spdk_jsonrpc_end_result()로 마감하는 패턴을 따른다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 control-plane을 담당하는 핵심 인프라. 일반적인 호출 흐름:
 *   rpc.py(클라이언트) → Unix domain socket "/var/tmp/spdk.sock" → spdk_jsonrpc_server →
 *   handle_request 콜백(보통 spdk/rpc.h의 spdk_rpc_register_method()로 등록된 핸들러로
 *   디스패치) → 핸들러가 spdk_jsonrpc_begin_result/end_result로 응답 작성 →
 *   소켓을 통해 클라이언트로 송신.
 * 본 헤더는 spdk/json.h(파서/writer)와 spdk/log.h(RPC 트레이스 로그)에 의존한다.
 * 상위 추상(spdk/rpc.h)은 본 API 위에 메서드 등록 표(state mask 포함)와 표준 socket
 * path 선택을 얹는다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/stdinc.h(POSIX 타입 socklen_t/sockaddr/FILE 등),
 *             spdk/json.h(파서/writer 타입과 spdk_json_val),
 *             spdk/log.h(spdk_log_level, RPC 트레이스용 로그 레벨/파일 설정).
 * 의존(구현): lib/jsonrpc/jsonrpc_server.c, jsonrpc_server_tcp.c, jsonrpc_client.c,
 *             jsonrpc_client_tcp.c가 본 API의 실체를 제공. listen/accept/recv/send
 *             등 POSIX 소켓 시스템 호출을 직접 사용한다.
 * 데이터 흐름: 소켓 byte stream → spdk_json_parse → JSON 토큰 → 메서드 디스패치 →
 *             핸들러 → spdk_json_write_ctx → 소켓 송신.
 * 공유 자료구조: spdk_jsonrpc_server (listen 소켓 + 활성 conn 리스트),
 *             spdk_jsonrpc_server_conn (TCP/UDS 연결 + 요청 큐),
 *             spdk_jsonrpc_request (현재 처리 중 요청 + 응답 writer 핸들),
 *             spdk_jsonrpc_client/_request/_response. 모두 단일 thread에서만 다뤄진다.
 *
 * === 주요 함수/구조체 요약 ===
 *   - 에러 코드 매크로: SPDK_JSONRPC_ERROR_PARSE_ERROR/_INVALID_REQUEST/_METHOD_NOT_FOUND/
 *     _INVALID_PARAMS/_INTERNAL_ERROR/_INVALID_STATE.
 *   - struct spdk_jsonrpc_server: 불투명 — listen 소켓 + accept/recv 상태.
 *   - struct spdk_jsonrpc_request: 들어온 요청 1건 + 응답 작성 컨텍스트.
 *   - struct spdk_jsonrpc_server_conn: 한 클라이언트와의 연결.
 *   - struct spdk_jsonrpc_client / _client_request / _client_response: 클라이언트 측.
 *   - typedef spdk_jsonrpc_handle_request_fn: 서버측 메서드 디스패치 콜백 시그니처.
 *   - typedef spdk_jsonrpc_conn_closed_fn: 연결 종료 시 호출 콜백.
 *   - typedef spdk_jsonrpc_client_response_parser: 클라이언트측 result 파서.
 *   - spdk_jsonrpc_server_listen / _server_poll / _server_shutdown: server lifecycle.
 *   - spdk_jsonrpc_get_conn / _conn_add_close_cb / _conn_del_close_cb: 연결 관리.
 *   - spdk_jsonrpc_begin_result / _end_result / _send_bool_response: 정상 응답.
 *   - spdk_jsonrpc_send_error_response / _send_error_response_fmt: 에러 응답.
 *   - spdk_jsonrpc_begin_request / _end_request: 클라이언트측 요청 작성.
 *   - spdk_jsonrpc_client_connect / _close: 클라이언트 lifecycle.
 *   - spdk_jsonrpc_client_create_request / _free_request / _send_request: 요청 송신.
 *   - spdk_jsonrpc_client_poll / _get_response / _free_response: 응답 수신.
 *   - spdk_jsonrpc_set_log_level / _set_log_file: RPC 트레이스 로그 설정.
 */

#ifndef SPDK_JSONRPC_H_       /* [한국어] include 가드 — 다중 포함 방지 */
#define SPDK_JSONRPC_H_

#include "spdk/stdinc.h"      /* [한국어] POSIX/표준 타입(sockaddr, socklen_t, FILE, int32_t 등) */

#include "spdk/json.h"        /* [한국어] spdk_json_val/_write_ctx/_val_type — 요청·응답 표현 기반 */
#include "spdk/log.h"         /* [한국어] enum spdk_log_level — RPC 트레이스 로그 레벨 설정용 */

/**
 * It is required to invoke API functions on the SPDK app thread;
 * otherwise, race conditions may lead to undefined behavior.
 */
/* [한국어] 본 헤더의 모든 API는 단일 SPDK app thread에서만 호출해야 한다.
 * 내부 자료구조(연결 리스트, 요청 큐, 응답 writer)가 lockless 설계이며,
 * 다른 thread에서 동시 호출하면 race condition으로 정의되지 않은 동작 발생. */

#ifdef __cplusplus
extern "C" {                  /* [한국어] C++에서 본 헤더를 그대로 사용하도록 C 링키지 강제 */
#endif

/* Defined error codes in JSON-RPC specification 2.0 */
/* [한국어] JSON-RPC 2.0 사양 §5.1에 정의된 표준 에러 코드. 응답 객체의 error.code에 사용.
 * 클라이언트(rpc.py)는 이 코드들을 보고 사용자에게 분류된 에러 메시지를 출력한다.
 * 사양에 정의된 -32768 ~ -32000 범위는 예약 — 사용자 정의 코드는 이 범위 밖에 있어야 한다. */
#define SPDK_JSONRPC_ERROR_PARSE_ERROR		-32700
/* [한국어] 서버가 받은 텍스트가 valid JSON이 아님(spdk_json_parse가 INVALID 반환).
 * 보통 클라이언트의 직렬화 버그 또는 전송 중 byte 손상. */
#define SPDK_JSONRPC_ERROR_INVALID_REQUEST	-32600
/* [한국어] 받은 JSON은 valid이지만 JSON-RPC 2.0 요청 객체 스키마 위반.
 * 예: jsonrpc 필드가 "2.0"이 아님, method 필드 누락, 등. */
#define SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND	-32601
/* [한국어] 요청한 method가 서버에 등록되어 있지 않음. spdk/rpc.h의 등록 표 lookup 실패 시. */
#define SPDK_JSONRPC_ERROR_INVALID_PARAMS	-32602
/* [한국어] params 객체가 핸들러의 디코더 표와 매치하지 않음
 * (필수 필드 누락, 타입 불일치, relaxed 아닌데 unknown 키 등). */
#define SPDK_JSONRPC_ERROR_INTERNAL_ERROR	-32603
/* [한국어] 서버 내부 에러(파싱/디스패치는 통과했지만 핸들러 본체에서 ENOMEM 등). */

/* Custom error codes in SPDK

 * Error codes from and including -32768 to -32000 are reserved for
 * predefined errors, hence custom error codes must be outside of the range.
 */
#define SPDK_JSONRPC_ERROR_INVALID_STATE	-1
/* [한국어] SPDK 정의 사용자 코드 — 메서드는 존재하지만 현재 RPC 서버 state(STARTUP/
 * RUNTIME)에서 호출이 허용되지 않음. spdk/rpc.h의 state mask 검사에서 사용. */

/* [한국어] 불투명 핸들 타입 — 정의는 lib/jsonrpc/jsonrpc_internal.h에 있음.
 * 호출자는 포인터만 다루고 내부 필드 접근 불가. */
struct spdk_jsonrpc_server;
/* [한국어] listen 소켓 + accept된 연결 리스트 + 디스패치 콜백을 담는 서버 핸들. */
struct spdk_jsonrpc_request;
/* [한국어] 들어온 요청 1건 + 응답을 작성할 spdk_json_write_ctx를 담는 처리 컨텍스트.
 * 핸들러는 이 핸들을 받아 응답을 빌드하며, end_result 호출 시점에 해제된다. */

struct spdk_jsonrpc_client;
/* [한국어] 클라이언트측 연결 핸들 — fd + 송신 큐 + 수신 파서 상태. */
struct spdk_jsonrpc_client_request;
/* [한국어] 클라이언트측 요청 객체 — id + method + 작성 중인 JSON. send_request 후
 * 라이브러리가 free 책임을 인수. */

/*
 * [한국어] 클라이언트가 spdk_jsonrpc_client_get_response()로 회수하는 응답 객체.
 * 모든 필드는 응답 byte buffer 안의 토큰을 가리키는 zero-copy 포인터이며,
 * spdk_jsonrpc_client_free_response()까지 유효하다.
 */
struct spdk_jsonrpc_client_response {
	struct spdk_json_val *version;
	/* [한국어] "jsonrpc" 필드 토큰 — 항상 STRING "2.0"이어야 함.
	 * 설정자: 라이브러리(응답 파서). 읽는 자: 호출자(필요시 검증).
	 * 값 범위: STRING 토큰 또는 NULL(필드 없음 — invalid). */

	struct spdk_json_val *id;
	/* [한국어] "id" 필드 토큰 — 클라이언트가 send_request 시 부여한 id의 echo.
	 * 다중 inflight 요청을 매칭할 때 사용. NUMBER 또는 STRING 토큰. */

	struct spdk_json_val *result;
	/* [한국어] "result" 필드 토큰 — 정상 응답이면 채워지고 error는 NULL.
	 * 임의 JSON 값이 들어올 수 있으므로 호출자가 spdk_json_decode_*로 풀어 사용. */

	struct spdk_json_val *error;
	/* [한국어] "error" 필드 토큰 — 에러 응답이면 채워지고 result는 NULL.
	 * {code, message[, data]} 객체 — 호출자가 code 필드로 분기 처리. */
};

/**
 * User callback to handle a single JSON-RPC request.
 *
 * The user should respond by calling one of spdk_jsonrpc_begin_result() or
 * spdk_jsonrpc_send_error_response().
 *
 * \param request JSON-RPC request to handle.
 * \param method Function to handle the request.
 * \param params Parameters passed to the function 'method'.
 */
/*
 * [한국어] 서버 dispatch 콜백 — spdk_jsonrpc_server_listen()에 1회 등록되어, 들어오는
 * 모든 요청에 대해 호출된다. SPDK 본 사용처(spdk/rpc.h)는 이 단일 콜백 안에서 method 이름을
 * 룩업해 spdk_rpc_method_handler로 추가 디스패치한다.
 *
 * @request: 요청 컨텍스트. 핸들러는 반드시 (정상)spdk_jsonrpc_begin_result+_end_result
 *           또는 (에러)spdk_jsonrpc_send_error_response_*를 호출해 응답을 반환해야 한다.
 *           응답 없이 함수를 빠져나가면 클라이언트가 영원히 대기.
 * @method:  요청의 method 토큰(STRING) — 원본 buffer 내부 zero-copy.
 * @params:  요청의 params 토큰(객체/배열/null) — 핸들러별 디코더 표로 풀어 사용.
 *
 * 호출 컨텍스트: SPDK app thread(server_poll을 부른 thread).
 */
typedef void (*spdk_jsonrpc_handle_request_fn)(
	struct spdk_jsonrpc_request *request,
	const struct spdk_json_val *method,
	const struct spdk_json_val *params);

struct spdk_jsonrpc_server_conn;
/* [한국어] 한 클라이언트와의 연결을 표현하는 불투명 핸들. 정의는 lib/jsonrpc/ 내부.
 * fd + 수신 buffer + 활성 요청 리스트 + 종료 콜백 리스트를 보관. */

/*
 * [한국어] 연결 종료 시 호출되는 사용자 콜백.
 * @conn: 종료된 연결(이미 닫히는 중 — 콜백 안에서 send 금지).
 * @arg:  spdk_jsonrpc_conn_add_close_cb()에 전달했던 사용자 컨텍스트.
 *
 * 사용 예: 연결에 묶인 비동기 작업(예: live tracing 스트림)을 정리할 때.
 */
typedef void (*spdk_jsonrpc_conn_closed_fn)(struct spdk_jsonrpc_server_conn *conn, void *arg);

/**
 * Function for specific RPC method response parsing handlers.
 *
 * \param parser_ctx context where analysis are put.
 * \param result json values responded to this method.
 *
 * \return 0 on success.
 *         SPDK_JSON_PARSE_INVALID on failure.
 */
/*
 * [한국어] 클라이언트측 result 파서 콜백 시그니처.
 * @parser_ctx: 호출자 컨텍스트(파싱 결과를 채울 자료구조).
 * @result:     응답의 result 토큰(spdk_jsonrpc_client_response.result).
 * @return:     0 성공, SPDK_JSON_PARSE_INVALID 실패.
 *
 * 메서드별로 응답 형태가 다르므로, 클라이언트는 메서드별 파서를 작성해 사용한다.
 */
typedef int (*spdk_jsonrpc_client_response_parser)(
	void *parser_ctx,
	const struct spdk_json_val *result);

/**
 * Create a JSON-RPC server listening on the required address.
 *
 * \param domain Socket family.
 * \param protocol Protocol.
 * \param listen_addr Listening address.
 * \param addrlen Length of address.
 * \param handle_request User callback to handle a JSON-RPC request.
 *
 * \return a pointer to the JSON-RPC server.
 */
/*
 * [한국어]
 * spdk_jsonrpc_server_listen - 지정 주소에서 JSON-RPC 서버 listen 시작.
 *
 * @domain:         소켓 family — AF_UNIX(SPDK 표준) 또는 AF_INET/AF_INET6.
 * @protocol:       소켓 protocol — 보통 0(default for SOCK_STREAM).
 * @listen_addr:    bind할 주소(sockaddr_un / sockaddr_in 등을 sockaddr*로 캐스팅).
 * @addrlen:        주소 구조체 크기.
 * @handle_request: 요청마다 호출될 dispatch 콜백.
 * @return:         서버 핸들 또는 NULL(socket()/bind()/listen() 실패).
 *
 * 내부 동작: socket(domain, SOCK_STREAM, protocol) → bind → listen → 핸들 반환.
 * 이후 spdk_jsonrpc_server_poll()을 SPDK poller로 등록해 주기 호출하면 동작.
 *
 * 호출 체인: spdk_rpc_server_listen → spdk_jsonrpc_server_listen → POSIX socket()
 */
struct spdk_jsonrpc_server *spdk_jsonrpc_server_listen(int domain, int protocol,
		struct sockaddr *listen_addr, socklen_t addrlen, spdk_jsonrpc_handle_request_fn handle_request);

/**
 * Poll the requests to the JSON-RPC server.
 *
 * This function does accept, receive, handle the requests and reply to them.
 *
 * \param server JSON-RPC server.
 *
 * \return 0 on success.
 */
/*
 * [한국어]
 * spdk_jsonrpc_server_poll - 한 번의 polling 사이클 — accept/recv/parse/dispatch/send.
 *
 * @server: spdk_jsonrpc_server_listen() 반환 핸들.
 * @return: 0 성공.
 *
 * 본 함수는 non-blocking — 처리할 일이 없으면 즉시 반환. SPDK reactor의 poller에
 * 등록되어 매 iteration 호출되는 것이 표준 사용 패턴(polled-mode 철학에 부합).
 * 한 번의 호출에서 여러 연결의 다중 요청을 batch로 처리할 수 있다.
 */
int spdk_jsonrpc_server_poll(struct spdk_jsonrpc_server *server);

/**
 * Shutdown the JSON-RPC server.
 *
 * \param server JSON-RPC server.
 */
/*
 * [한국어]
 * spdk_jsonrpc_server_shutdown - listen 소켓 + 모든 활성 연결을 닫고 핸들 free.
 * @server: 해제할 서버. 호출 후 server는 무효 — 다시 사용 금지.
 * 등록된 모든 conn_close 콜백이 호출된다.
 */
void spdk_jsonrpc_server_shutdown(struct spdk_jsonrpc_server *server);

/**
 * Return connection associated to \c request
 *
 * \param request JSON-RPC request
 * \return JSON RPC server connection
 */
/*
 * [한국어]
 * spdk_jsonrpc_get_conn - 요청을 보낸 연결 핸들 반환.
 * @request: 핸들러가 받은 요청 컨텍스트.
 * @return: 연결 핸들(NULL 아님).
 *
 * 사용 예: 연결 종료 시 핸들러의 비동기 작업을 취소하기 위해 close_cb 등록할 때.
 */
struct spdk_jsonrpc_server_conn *spdk_jsonrpc_get_conn(struct spdk_jsonrpc_request *request);

/**
 * Add callback called when connection is closed. Pair of  \c cb and \c ctx must be unique or error is returned.
 * Registered callback is called only once and there is no need to call  \c spdk_jsonrpc_conn_del_close_cb
 * inside from \c cb.
 *
 * \note Current implementation allow only one close callback per connection.
 *
 * \param conn JSON RPC server connection
 * \param cb callback function
 * \param ctx argument for \c cb
 *
 * \return 0 on success, or negated errno code:
 *  -EEXIST \c cb and \c ctx is already registered
 *  -ENOTCONN Callback can't be added because connection is closed.
 *  -ENOSPC no more space to register callback.
 */
/*
 * [한국어]
 * spdk_jsonrpc_conn_add_close_cb - 연결 종료 시 호출될 cleanup 콜백 등록.
 *
 * @conn: 대상 연결.
 * @cb:   종료 시 호출될 함수.
 * @ctx:  cb 호출 시 전달될 사용자 데이터.
 * @return: 0 성공, -EEXIST(중복 등록), -ENOTCONN(이미 종료), -ENOSPC(용량 초과).
 *
 * 콜백은 정확히 1회 호출되며, 그 안에서 _del_close_cb를 부를 필요 없음(자동 해제).
 * 현 구현은 연결당 1개 콜백만 지원(주의).
 *
 * 사용 예: spdk_trace의 live tracing 메서드처럼 비동기 데이터 스트림이 연결 끊김에
 * 따라 정리되어야 할 때 등록.
 */
int spdk_jsonrpc_conn_add_close_cb(struct spdk_jsonrpc_server_conn *conn,
				   spdk_jsonrpc_conn_closed_fn cb, void *ctx);

/**
 * Remove registered close callback.
 *
 * \param conn JSON RPC server connection
 * \param cb callback function
 * \param ctx argument for \c cb
 *
 * \return 0 on success, or negated errno code:
 *  -ENOENT \c cb and \c ctx pair is not registered
 */
/*
 * [한국어]
 * spdk_jsonrpc_conn_del_close_cb - 등록된 close 콜백 제거.
 *
 * @conn/@cb/@ctx: add_close_cb와 동일한 (cb,ctx) 쌍을 지정해야 일치.
 * @return: 0 성공, -ENOENT 미등록.
 *
 * 비동기 작업이 정상 완료되어 연결 종료 시 cleanup이 더 이상 필요 없을 때 호출.
 */
int spdk_jsonrpc_conn_del_close_cb(struct spdk_jsonrpc_server_conn *conn,
				   spdk_jsonrpc_conn_closed_fn cb, void *ctx);

/**
 * Begin building a response to a JSON-RPC request.
 *
 * If this function returns non-NULL, the user must call spdk_jsonrpc_end_result()
 * on the request after writing the desired response object to the spdk_json_write_ctx.
 *
 * \param request JSON-RPC request to respond to.

 * \return Non-NULL pointer to JSON write context to write the response object to.
 */
/*
 * [한국어]
 * spdk_jsonrpc_begin_result - 정상 응답 작성 시작 — JSON writer 컨텍스트 반환.
 *
 * @request: 핸들러가 받은 요청.
 * @return:  spdk_json_write_ctx 포인터(NULL 아님).
 *
 * 핸들러는 반환된 writer로 result 값을 작성한 뒤 spdk_jsonrpc_end_result()를 호출해
 * 마감/송신해야 한다. 응답 외피("jsonrpc":"2.0","id":..,"result":..)는 라이브러리가
 * 자동으로 둘러싼다 — 핸들러는 result 값(객체/배열/스칼라)만 작성.
 *
 * 호출 체인: rpc_handler → spdk_jsonrpc_begin_result → spdk_json_write_* → _end_result
 */
struct spdk_json_write_ctx *spdk_jsonrpc_begin_result(struct spdk_jsonrpc_request *request);

/**
 * Complete and send a JSON-RPC response.
 *
 * \param request Request to complete the response for.
 * \param w JSON write context returned from spdk_jsonrpc_begin_result().
 */
/*
 * [한국어]
 * spdk_jsonrpc_end_result - begin_result로 시작한 응답을 마감하고 클라이언트에 송신.
 * @request: 응답할 요청. 호출 후 request는 무효 — 다시 사용 금지.
 * @w:       begin_result가 반환한 writer.
 *
 * 내부 동작: writer의 마지막 토큰을 출력 → 응답 외피 닫기 → 소켓 송신 큐에 enqueue →
 * request/writer 자원 해제.
 */
void spdk_jsonrpc_end_result(struct spdk_jsonrpc_request *request, struct spdk_json_write_ctx *w);

/**
 * Complete a JSON-RPC response and write bool result.
 *
 * \param request Request to complete the response for.
 * \param value Write bool result value.
 */
/*
 * [한국어]
 * spdk_jsonrpc_send_bool_response - "result": true/false 한 줄 응답.
 * @request: 응답할 요청.
 * @value:   true/false.
 *
 * "어떤 메서드가 성공했는지만 알리면 되는" 명령형 RPC(예: bdev_delete)에서 자주 쓰임.
 * begin_result+write_bool+end_result 3단계의 단축형.
 */
void spdk_jsonrpc_send_bool_response(struct spdk_jsonrpc_request *request, bool value);

/**
 * Send an error response to a JSON-RPC request.
 *
 * This is shorthand for spdk_jsonrpc_begin_result() + spdk_jsonrpc_end_result()
 * with an error object.
 *
 * \param request JSON-RPC request to respond to.
 * \param error_code Integer error code to return (may be one of the
 * SPDK_JSONRPC_ERROR_ errors, or a custom error code).
 * \param msg String error message to return.
 */
/*
 * [한국어]
 * spdk_jsonrpc_send_error_response - 에러 응답 한 번에 송신.
 *
 * @request:    응답할 요청. 호출 후 무효.
 * @error_code: SPDK_JSONRPC_ERROR_* 또는 사용자 정의 코드(-32768~-32000 범위 밖).
 * @msg:        사람이 읽을 수 있는 에러 메시지(NUL-종결).
 *
 * 응답 외피는 {"jsonrpc":"2.0","id":..,"error":{"code":...,"message":"..."}} 형태로 자동 생성.
 */
void spdk_jsonrpc_send_error_response(struct spdk_jsonrpc_request *request,
				      int error_code, const char *msg);

/**
 * Send an error response to a JSON-RPC request.
 *
 * This is shorthand for printf() + spdk_jsonrpc_send_error_response().
 *
 * \param request JSON-RPC request to respond to.
 * \param error_code Integer error code to return (may be one of the
 * SPDK_JSONRPC_ERROR_ errors, or a custom error code).
 * \param fmt Printf-like format string.
 */
/*
 * [한국어]
 * spdk_jsonrpc_send_error_response_fmt - send_error_response의 printf 변종.
 *
 * @request/@error_code: 위와 동일.
 * @fmt: printf 포맷 문자열.
 * GCC __format__으로 컴파일 타임 포맷 검증.
 */
void spdk_jsonrpc_send_error_response_fmt(struct spdk_jsonrpc_request *request,
		int error_code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/**
 * Begin building a JSON-RPC request.
 *
 * If this function returns non-NULL, the user must call spdk_jsonrpc_end_request()
 * on the request after writing the desired request object to the spdk_json_write_ctx.
 *
 * \param request JSON-RPC request.
 * \param id ID index for the request. If < 0 skip ID.
 * \param method Name of the RPC method. If NULL caller will have to create "method" key.
 *
 * \return JSON write context or NULL in case of error.
 */
/*
 * [한국어]
 * spdk_jsonrpc_begin_request - 클라이언트 요청 객체 작성 시작.
 *
 * @request: client_create_request로 만든 빈 요청 객체.
 * @id:      요청 id — 0 이상이면 응답 매칭에 사용. 음수면 notification(응답 없음).
 * @method:  메서드 이름 — NULL이면 호출자가 직접 "method" 키를 작성해야 함(드문 경우).
 * @return:  파라미터를 작성할 JSON writer 또는 NULL(메모리 부족).
 *
 * 호출자는 반환된 writer로 params 값(객체/배열)을 작성한 뒤 spdk_jsonrpc_end_request로
 * 마감해야 한다. 외피 {"jsonrpc":"2.0","id":N,"method":"...","params":...}의 외곽은
 * 라이브러리가 자동 생성.
 */
struct spdk_json_write_ctx *
spdk_jsonrpc_begin_request(struct spdk_jsonrpc_client_request *request, int32_t id,
			   const char *method);

/**
 * Complete a JSON-RPC request.
 *
 * \param request JSON-RPC request.
 * \param w JSON write context returned from spdk_jsonrpc_begin_request().
 */
/*
 * [한국어]
 * spdk_jsonrpc_end_request - 요청 객체 작성 마감(아직 송신은 안 함).
 * @request: 작성 중인 요청.
 * @w:       begin_request가 반환한 writer.
 *
 * 마감 후 spdk_jsonrpc_client_send_request로 실제 네트워크 전송.
 */
void spdk_jsonrpc_end_request(struct spdk_jsonrpc_client_request *request,
			      struct spdk_json_write_ctx *w);

/**
 * Connect to the specified RPC server.
 *
 * \param addr RPC socket address.
 * \param addr_family Protocol families of address.
 *
 * \return JSON-RPC client on success, NULL on failure and errno set to indicate
 * the cause of the error.
 */
/*
 * [한국어]
 * spdk_jsonrpc_client_connect - 지정 주소의 RPC 서버에 연결.
 *
 * @addr:        주소 문자열 — AF_UNIX는 path("/var/tmp/spdk.sock"),
 *               AF_INET은 "ip:port" 형태.
 * @addr_family: AF_UNIX / AF_INET / AF_INET6.
 * @return:      클라이언트 핸들 또는 NULL(errno에 실패 사유).
 *
 * 내부 동작: socket() → connect() → non-blocking 전환. SPDK 자체 도구(rpc.py 백엔드,
 * spdk_top, nvmf_tgt rpc 호출 등)가 사용.
 */
struct spdk_jsonrpc_client *spdk_jsonrpc_client_connect(const char *addr, int addr_family);

/**
 * Close JSON-RPC connection and free \c client object.
 *
 * This function is not thread safe and should only be called from one thread at
 * a time while no other threads are actively \c client object.
 *
 * \param client JSON-RPC client.
 */
/*
 * [한국어]
 * spdk_jsonrpc_client_close - 연결을 닫고 client 객체 해제.
 * @client: 해제할 클라이언트. 호출 후 무효 — 다시 사용 금지.
 *
 * thread-safe하지 않다 — 다른 스레드가 동일 client에 접근 중이면 안 됨.
 * 미수신 응답은 모두 폐기된다.
 */
void spdk_jsonrpc_client_close(struct spdk_jsonrpc_client *client);

/**
 * Create one JSON-RPC request. Returned request must be passed to
 * \c spdk_jsonrpc_client_send_request when done or to \c spdk_jsonrpc_client_free_request
 * if discaded.
 *
 * \return pointer to JSON-RPC request object.
 */
/*
 * [한국어]
 * spdk_jsonrpc_client_create_request - 새 요청 객체 할당.
 * @return: 빈 요청 또는 NULL(메모리 부족).
 *
 * 반환된 객체는 begin_request로 작성하고 send_request로 송신(소유권 이전) 또는
 * free_request로 폐기해야 한다.
 */
struct spdk_jsonrpc_client_request *spdk_jsonrpc_client_create_request(void);

/**
 * Free one JSON-RPC request.
 *
 * \param req pointer to JSON-RPC request object.
 */
/*
 * [한국어]
 * spdk_jsonrpc_client_free_request - 요청 객체 폐기(송신하지 않을 때).
 * @req: 해제할 요청. send_request에 넘긴 경우는 호출 금지(이중 free).
 */
void spdk_jsonrpc_client_free_request(struct spdk_jsonrpc_client_request *req);

/**
 * Send the JSON-RPC request in JSON-RPC client. Library takes ownership of the
 * request object and will free it when done.
 *
 * This function is not thread safe and should only be called from one thread at
 * a time while no other threads are actively \c client object.
 *
 * \param client JSON-RPC client.
 * \param req JSON-RPC request.
 *
 * \return 0 on success or negative error code.
 * -ENOSPC - no space left to queue another request. Try again later.
 */
/*
 * [한국어]
 * spdk_jsonrpc_client_send_request - 작성된 요청을 송신 큐에 enqueue.
 *
 * @client: 클라이언트.
 * @req:    end_request로 마감된 요청 — 호출 성공 후 라이브러리가 소유.
 * @return: 0 성공, -ENOSPC(큐 가득 — 잠시 후 재시도).
 *
 * 실제 네트워크 송신은 _client_poll()이 처리. 비동기 모델이므로 호출 직후 응답을
 * 기다리는 것이 아니라 _poll() 후 _get_response()로 회수.
 */
int spdk_jsonrpc_client_send_request(struct spdk_jsonrpc_client *client,
				     struct spdk_jsonrpc_client_request *req);

/**
 * Poll the JSON-RPC client. When any response is available use
 * \c spdk_jsonrpc_client_get_response to retrieve it.
 *
 * This function is not thread safe and should only be called from one thread at
 * a time while no other threads are actively \c client object.
 *
 * \param client JSON-RPC client.
 * \param timeout Time in milliseconds this function will block. -1 block forever, 0 don't block.
 *
 * \return If no error occurred, this function returns a non-negative number indicating how
 * many ready responses can be retrieved. If an error occurred, this function returns one of
 * the following negated errno values:
 *  -ENOTCONN - not connected yet. Try again later.
 *  -EINVAL - response is detected to be invalid. Client connection should be terminated.
 *  -ENOSPC - no space to receive another response. User need to retrieve waiting responses.
 *  -EIO - connection terminated (or other critical error). Client connection should be terminated.
 *  -ENOMEM - out of memory
 */
/*
 * [한국어]
 * spdk_jsonrpc_client_poll - 송신/수신 처리 1회 + (옵션)blocking 대기.
 *
 * @client:  클라이언트.
 * @timeout: 밀리초. -1=무한 blocking, 0=non-blocking, >0=최대 N ms 대기.
 * @return:  >=0 회수 가능한 응답 수, 또는 음수 에러:
 *           -ENOTCONN(미연결, 재시도), -EINVAL(invalid 응답 — 연결 폐기 권장),
 *           -ENOSPC(수신 버퍼 가득 — 응답 회수 후 재호출),
 *           -EIO(연결 끊김 — 폐기 필요), -ENOMEM(메모리 부족).
 *
 * 내부 동작: select/poll 또는 timeout만큼 대기 → recv → JSON 파싱 → 응답 큐에 enqueue.
 */
int spdk_jsonrpc_client_poll(struct spdk_jsonrpc_client *client, int timeout);

/**
 * Return JSON RPC response object representing next available response from client connection.
 * Returned pointer must be freed using \c spdk_jsonrpc_client_free_response
 *
 * This function is not thread safe and should only be called from one thread at
 * a time while no other threads are actively \c client object.
 *
 * \param client
 * \return pointer to JSON RPC response object or NULL if no response available.
 */
/*
 * [한국어]
 * spdk_jsonrpc_client_get_response - 큐에서 응답 1건 회수.
 * @client: 클라이언트.
 * @return: 응답 객체 또는 NULL(가용 응답 없음).
 *
 * 회수한 응답은 사용 후 spdk_jsonrpc_client_free_response()로 해제 책임.
 * 응답 객체의 토큰 포인터들은 free 호출 전까지만 유효.
 */
struct spdk_jsonrpc_client_response *spdk_jsonrpc_client_get_response(struct spdk_jsonrpc_client
		*client);

/**
 * Free response object obtained from \c spdk_jsonrpc_client_get_response
 *
 * \param resp pointer to JSON RPC response object. If NULL no operation is performed.
 */
/*
 * [한국어]
 * spdk_jsonrpc_client_free_response - 응답 객체와 백킹 byte buffer 해제.
 * @resp: get_response 반환값. NULL 안전(아무 일도 안 함).
 */
void spdk_jsonrpc_client_free_response(struct spdk_jsonrpc_client_response *resp);

/**
 * Set the log level used by the JSON-RPC server to log RPC request and response objects.
 *
 * NOTE: This function should be called only before starting the JSON-RPC server.
 * Users should set the level set by this function higher than the level set by
 * spdk_log_set_print_level() or spdk_log_set_level().
 *
 * \param level Log level used to log RPC objects.
 */
/*
 * [한국어]
 * spdk_jsonrpc_set_log_level - RPC 트레이스 로그 레벨 설정.
 *
 * @level: 이 레벨 이상에서 RPC 요청/응답 본문이 로그에 남는다.
 *
 * 디버깅 시 모든 RPC 트래픽을 기록하기 위한 토글. 일반 spdk_log 임계값과 별개로
 * 관리되며, 이 값을 spdk_log_set_print_level/_level보다 *높게* 설정해야 본 트레이스가
 * 일반 로그에 묻히지 않고 출력된다(주의 — NOTE 참조).
 *
 * 서버 시작 전(spdk_jsonrpc_server_listen 이전)에 호출해야 함.
 */
void spdk_jsonrpc_set_log_level(enum spdk_log_level level);

/**
 * Set the log file used by the JSON-RPC server to log RPC request and response objects.
 *
 * NOTE: This function should be called only before starting the JSON-RPC server.
 *
 * \param file Log file pointer used to log RPC objects.
 */
/*
 * [한국어]
 * spdk_jsonrpc_set_log_file - RPC 트레이스 로그 출력 파일 설정.
 * @file: fopen된 FILE 포인터(stderr/stdout 가능). NULL이면 트레이스 비활성.
 *
 * 서버 시작 전에 호출해야 함. 라이브러리는 file을 fclose 하지 않음 — 호출자 책임.
 */
void spdk_jsonrpc_set_log_file(FILE *file);

#ifdef __cplusplus
}                       /* [한국어] extern "C" 종결 */
#endif

#endif                  /* [한국어] SPDK_JSONRPC_H_ — include 가드 종결 */
