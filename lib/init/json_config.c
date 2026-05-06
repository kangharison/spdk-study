/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK JSON 구성 파일 로더 (json_config.c)
 *
 * === 파일의 역할 ===
 * SPDK 데몬은 부팅 시 사용자가 제공한 JSON 구성 파일을 읽어, 그 안의 RPC 명령
 * 시퀀스를 자기 자신에게 전송함으로써 초기 상태(서브시스템 별 설정)를 구축할 수 있다.
 * 본 파일은 그 로더의 핵심 — JSON 파싱, 임시 RPC 소켓 생성, JSON-RPC 클라이언트
 * 연결, 서브시스템·메서드 단위 RPC 발행을 담당한다.
 * 로더는 두 단계(STARTUP / RUNTIME) 로 동작한다:
 *   1. STARTUP phase: 서브시스템 init 이전에 호출 가능한 RPC(예: bdev_*_create) 만 발행.
 *      모두 끝나면 spdk_subsystem_init() 을 호출하여 서브시스템들이 실제로 init 진입.
 *   2. RUNTIME phase: 서브시스템 init 완료 후 호출 가능한 RPC 만 발행 (모든 RUNTIME-only
 *      메서드, 그리고 STARTUP|RUNTIME 모두 허용 메서드는 1단계에서 이미 호출되었으므로 skip).
 * 로더 자체는 JSON-RPC client 를 띄워 socket loopback 으로 자기 자신에게 RPC 를 보낸다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 흐름에서:
 *   spdk_app_start (app/lib)
 *     → spdk_subsystem_load_config(json, ..., cb_fn)   # 본 파일
 *         → json_config_prepare_ctx
 *             → parse_json (raw JSON → spdk_json_val 배열)
 *             → spdk_rpc_initialize(임시 UDS path)      # rpc.c
 *             → spdk_jsonrpc_client_connect (loopback)
 *             → poller 등록 → 연결 완료 시 app_json_config_load_subsystem
 *         → 각 subsystem-config 엔트리에 대해 RPC 송신
 *         → subsystems_it=NULL 이고 STARTUP 이면 spdk_subsystem_init 호출 → RUNTIME 진입
 *         → 두 번째 패스에서 RUNTIME 메서드 발행
 *         → app_json_config_load_done 으로 cb_fn 호출 후 정리
 * 실행 컨텍스트: 모든 함수가 SPDK app thread (master reactor) 단독 호출.
 *
 * === 타 모듈과의 연결 ===
 * - lib/jsonrpc: spdk_jsonrpc_client_* (요청 직렬화, 송신, 응답 폴링).
 * - lib/init/rpc.c: spdk_rpc_initialize / server_finish (임시 UDS 서버 관리).
 * - lib/init/subsystem.c: spdk_subsystem_init (STARTUP→RUNTIME 전이),
 *                         spdk_subsystem_exists (없는 메서드 처리 분기).
 * - lib/json: spdk_json_parse, spdk_json_decode_object, spdk_json_array_first/next 등.
 * - include/spdk/rpc.h: spdk_rpc_get_state, spdk_rpc_set_state, spdk_rpc_get_method_state_mask.
 * - 데이터 흐름: JSON 파일 → 메모리 → spdk_json_val 배열 → 한 메서드씩 RPC 요청 직렬화 →
 *   임시 소켓을 통해 자기 자신의 RPC 서버로 전송 → 서버는 등록된 핸들러 dispatch →
 *   응답을 클라이언트가 폴링 → 응답 무시(또는 에러 처리) → 다음 메서드로 진행.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_subsystem_load_config(): 외부 진입점. 사용자 JSON 데이터를 받아 로딩 ctx 준비.
 * - json_config_prepare_ctx(): 파싱·임시 RPC 서버·클라이언트 셋업, 연결 poller 등록.
 * - app_json_config_load_subsystem(): 다음 서브시스템 엔트리 진입. NULL 도달 시 STARTUP→RUNTIME 전이 또는 종료.
 * - app_json_config_load_subsystem_config_entry(): 한 config 엔트리(RPC 메서드)를 발행.
 *   메서드 미존재/state-mask 불일치/이미 호출됨 등은 skip.
 * - rpc_client_poller(): JSON-RPC 응답 폴링 + 콜백 실행.
 * - struct load_json_config_ctx: 로딩 작업 전체 상태 (JSON 데이터, 진행 포인터, 클라이언트, 콜백).
 */

#include "spdk/stdinc.h"        /* [한국어] 표준 C 라이브러리 (assert, string, sys/un.h, getpid 등) */

#include "spdk/init.h"          /* [한국어] spdk_subsystem_load_config / init 등 공개 API prototype */
#include "spdk/util.h"          /* [한국어] SPDK_COUNTOF, SPDK_SIZEOF_MEMBER 등 매크로 */
#include "spdk/file.h"          /* [한국어] 파일 I/O 유틸 — 본 파일에서는 직접 사용 적음 (헤더 호환) */
#include "spdk/log.h"           /* [한국어] SPDK_ERRLOG, SPDK_WARNLOG, SPDK_NOTICELOG, SPDK_DEBUGLOG 등 */
#include "spdk/env.h"           /* [한국어] spdk_get_ticks / spdk_get_ticks_hz (타임아웃 계산), DPDK 환경 */
#include "spdk/thread.h"        /* [한국어] spdk_thread_send_msg, SPDK_POLLER_REGISTER, spdk_thread_get_app_thread 등 */
#include "spdk/jsonrpc.h"       /* [한국어] spdk_jsonrpc_client_*, spdk_jsonrpc_begin_request 등 — 자체 RPC client */
#include "spdk/rpc.h"           /* [한국어] spdk_rpc_get_state/set_state, SPDK_RPC_STARTUP/RUNTIME, get_method_state_mask 등 */
#include "spdk/string.h"        /* [한국어] 문자열 유틸 */

#include "spdk_internal/event.h"  /* [한국어] init 라이브러리 내부 이벤트 헤더 (subsystem 진입점 prototype) */

/* [한국어] DEBUG 로그를 "app_config" 컴포넌트로 라우팅 — 사용자가 SPDK_LOG_DEBUG=app_config 로 활성화 가능 */
#define SPDK_DEBUG_APP_CFG(...) SPDK_DEBUGLOG(app_config, __VA_ARGS__)

/* JSON configuration format is as follows
 *
 * {
 *  "subsystems" : [                          <<== *subsystems JSON array
 *    {                                       <<== *subsystems_it array entry pointer (iterator)
 *      "subsystem": "<< SUBSYSTEM NAME >>",
 *      "config": [                           <<== *config JSON array
 *         {                                  <<== *config_it array entry pointer (iterator)
 *           "method": "<< METHOD NAME >>",   <<== *method
 *           "params": { << PARAMS >> }       <<== *params
 *         },
 *         << MORE "config" ARRAY ENTRIES >>
 *      ]
 *    },
 *    << MORE "subsystems" ARRAY ENTRIES >>
 *  ]
 *
 *  << ANYTHING ELSE IS IGNORED IN ROOT OBJECT>>
 * }
 *
 */
/* [한국어] 위 도식은 SPDK JSON 구성 파일의 표준 형식. 핵심:
 *  - 최상위 객체에 "subsystems" 배열.
 *  - 각 원소는 {"subsystem":"<name>", "config":[ {"method":..., "params":{...}}, ... ]}.
 *  - 본 파일의 ctx 의 subsystems / subsystems_it / config / config_it 가 그 위치를 가리킨다. */

/* [한국어] forward declaration — 동일 파일 내 함수 사이의 비동기 콜백 chain 을 풀기 위한 */
struct load_json_config_ctx;
typedef void (*client_resp_handler)(struct load_json_config_ctx *,
				    struct spdk_jsonrpc_client_response *);

/* [한국어] UDS 경로 최대 길이 = sizeof(sockaddr_un.sun_path). 통상 108B. */
#define RPC_SOCKET_PATH_MAX SPDK_SIZEOF_MEMBER(struct sockaddr_un, sun_path)

/* 1s connections timeout */
/* [한국어] 임시 RPC 서버에 연결 시도 시 1초 안에 connect 성공 못하면 timeout (마이크로초 단위). */
#define RPC_CLIENT_CONNECT_TIMEOUT_US (1U * 1000U * 1000U)

/*
 * Currently there is no timeout in SPDK for any RPC command. This result that
 * we can't put a hard limit during configuration load as it most likely randomly fail.
 * So just print WARNLOG every 10s. */
/* [한국어] RPC 요청 타임아웃은 "확정 실패" 가 아니라 10초마다 WARNLOG 만 출력하는 soft timeout.
 * 일부 RPC(예: nvmf_create_transport, 초기화가 오래 걸리는 명령)는 자연스럽게 수 초 걸릴 수 있어
 * 강한 타임아웃을 두면 정상 사용자에게 false positive 가 생긴다. */
#define RPC_CLIENT_REQUEST_TIMEOUT_US (10U * 1000 * 1000)

/* [한국어] JSON 구성 로딩 작업 전체 상태를 묶은 컨텍스트.
 * spdk_subsystem_load_config 호출마다 1개 할당되어 콜백 체인을 따라 전달된다. */
struct load_json_config_ctx {
	/* Thread used during configuration. */
	spdk_subsystem_init_fn cb_fn;
	/* [한국어] 사용자가 제공한 완료 콜백 — 모든 로딩 종료 시 cb_fn(rc, cb_arg) 로 호출.
	 * 설정자: spdk_subsystem_load_config / json_config_prepare_ctx.
	 * 읽는 자: app_json_config_load_done.
	 * 동기화: app thread 단독 접근. */

	void *cb_arg;
	/* [한국어] cb_fn 의 컨텍스트 인자.
	 * 설정자/읽는 자: 위 cb_fn 과 동일. */

	bool stop_on_error;
	/* [한국어] true=어떤 RPC 가 에러 응답하면 즉시 로딩 중단 후 cb_fn(-EINVAL).
	 *         false=에러 무시하고 다음 엔트리로 진행 (best-effort).
	 * 설정자: spdk_subsystem_load_config 인자.
	 * 읽는 자: rpc_client_poller, app_json_config_load_subsystem_config_entry. */

	/* Current subsystem */
	struct spdk_json_val *subsystems; /* "subsystems" array */
	/* [한국어] 파싱된 JSON 의 "subsystems" 배열 시작 토큰 포인터.
	 * 설정자: json_config_prepare_ctx 가 spdk_json_find_array 결과로 저장.
	 * 읽는 자: subsystem_init_done 가 RUNTIME 전이 시 첫 번째 패스 종료 후 다시 array_first 로 초기화. */

	struct spdk_json_val *subsystems_it; /* current subsystem array position in "subsystems" array */
	/* [한국어] 현재 처리 중인 서브시스템 엔트리 포인터.
	 * 설정자: 매 서브시스템 엔트리 처리 후 spdk_json_next 로 갱신.
	 * 읽는 자: app_json_config_load_subsystem.
	 * 값 범위: subsystems 배열의 객체 노드 또는 NULL(끝). */

	struct spdk_json_val *subsystem_name; /* current subsystem name */
	/* [한국어] 현재 서브시스템의 "subsystem" 키 값(string) 포인터.
	 * 설정자: spdk_json_decode_object → cap_string.
	 * 읽는 자: 디버그 로깅, subsystem_name_str 채우기. */

	char subsystem_name_str[128];
	/* [한국어] subsystem_name 의 NUL-종결 사본 — spdk_subsystem_exists 등에 넘기기 위함.
	 * 설정자: app_json_config_load_subsystem 가 snprintf 로 채움.
	 * 읽는 자: app_json_config_load_subsystem_config_entry (서브시스템 존재 여부 검사 시).
	 * 동기화: ctx 단독 사용 — app thread. */

	/* Current "config" entry we are processing */
	struct spdk_json_val *config; /* "config" array */
	/* [한국어] 현재 서브시스템의 "config" 배열 시작 포인터 (또는 NULL/JSON null).
	 * 설정자: subsystem_decoders 의 cap_array_or_null 디코더. */

	struct spdk_json_val *config_it; /* current config position in "config" array */
	/* [한국어] config 배열에서 현재 처리 중인 엔트리(RPC 명령) 포인터.
	 * 설정자: 매 엔트리 처리 후 spdk_json_next 로 진행.
	 * 읽는 자: app_json_config_load_subsystem_config_entry. */

	/* Current request id we are sending. */
	uint32_t rpc_request_id;
	/* [한국어] JSON-RPC 요청의 id 필드 (현재는 항상 0 으로 보내는 듯하나 필드 자체는 보유).
	 * 응답 매칭에 사용 가능 — 본 로더는 순차 발행이라 id 매칭이 필수는 아님. */

	/* Whole configuration file read and parsed. */
	size_t json_data_size;
	/* [한국어] 사용자 입력 JSON 의 크기 (바이트).
	 * 설정자: parse_json. */

	char *json_data;
	/* [한국어] 사용자 JSON 데이터의 heap 사본 (parse 가 in-place 로 작성하므로 사본 필요).
	 * 설정자: parse_json 의 calloc/memcpy.
	 * 해제자: app_json_config_load_done 의 free.
	 * 동기화: ctx 와 함께 단독 소유. */

	size_t values_cnt;
	/* [한국어] spdk_json_val 토큰 개수 — 첫 번째 spdk_json_parse 가 반환.
	 * 설정자: parse_json. */

	struct spdk_json_val *values;
	/* [한국어] JSON 토큰 배열 — 두 번째 spdk_json_parse 가 채움.
	 * 본 배열의 인덱스/포인터로 subsystems / config 등을 가리킨다.
	 * 설정자: parse_json (calloc). 해제자: app_json_config_load_done. */

	char rpc_socket_path_temp[RPC_SOCKET_PATH_MAX + 1];
	/* [한국어] 임시 RPC 서버 UDS 경로 — "<SPDK_DEFAULT_RPC_ADDR>.<pid>_<ticks>_config" 형식.
	 * 충돌 방지를 위해 pid 와 현재 ticks 로 유일성 보장.
	 * 설정자: json_config_prepare_ctx 의 snprintf.
	 * 읽는 자: spdk_rpc_initialize / server_finish, jsonrpc_client_connect. */

	struct spdk_jsonrpc_client *client_conn;
	/* [한국어] 자기 자신에게 발행하는 JSON-RPC 클라이언트 핸들 (loopback).
	 * 설정자: json_config_prepare_ctx 의 spdk_jsonrpc_client_connect.
	 * 해제자: app_json_config_load_done 의 spdk_jsonrpc_client_close. */

	struct spdk_poller *client_conn_poller;
	/* [한국어] 클라이언트 연결/응답 폴링 poller (rpc_client_connect_poller → rpc_client_poller 로 전환).
	 * 설정자: json_config_prepare_ctx (connect poller), rpc_client_connect_poller (응답 poller 로 교체).
	 * 해제자: app_json_config_load_done. */

	client_resp_handler client_resp_cb;
	/* [한국어] 직전에 보낸 RPC 의 응답 도착 시 호출할 콜백.
	 * 설정자: client_send_request 가 매 송신마다 갱신.
	 * 읽는 자: rpc_client_poller. 한 번 호출 후 NULL 로 클리어 — 한 시점에 in-flight 요청은 1개만. */

	/* Timeout for current RPC client action. */
	uint64_t timeout;
	/* [한국어] 현재 RPC 작업의 타임아웃 만료 ticks (절대값).
	 * 설정자: rpc_client_set_timeout (현재 ticks + delta).
	 * 읽는 자: rpc_client_check_timeout. */

	/* Signals that the code should follow deprecated path of execution. */
	bool initalize_subsystems;
	/* [한국어] true=설정 로딩 완료 후 spdk_subsystem_init 호출하여 STARTUP→RUNTIME 전이까지 수행 (deprecated path).
	 *         false=서브시스템 초기화는 호출자가 따로 한다 (현재의 권장 path).
	 * 설정자: json_config_prepare_ctx 인자. spdk_subsystem_load_config 는 항상 false 로 호출.
	 * 읽는 자: app_json_config_load_subsystem. */
};

/* [한국어] forward declaration — 본 함수와 _config_entry 사이의 상호 콜백 체인 참조용 */
static void app_json_config_load_subsystem(void *_ctx);

/*
 * [한국어]
 * app_json_config_load_done - 로딩 작업 종료 + 자원 정리 + 사용자 콜백 호출
 *
 * @ctx: 로딩 작업 컨텍스트.
 * @rc: 결과 코드 (0=성공, 음수=실패).
 * @return: void.
 *
 * (1) 클라이언트 응답 poller 해제, (2) jsonrpc client 종료, (3) 임시 RPC 서버 종료,
 * (4) 사용자 cb_fn(rc, cb_arg) 호출, (5) ctx 의 동적 자원 해제.
 *
 * 호출 체인:
 *   여러 에러 경로 / 정상 종료 경로 → [app_json_config_load_done]
 *     → spdk_poller_unregister, spdk_jsonrpc_client_close, spdk_rpc_server_finish, ctx->cb_fn
 */
static void
app_json_config_load_done(struct load_json_config_ctx *ctx, int rc)
{
	spdk_poller_unregister(&ctx->client_conn_poller);  /* [한국어] connect/응답 poller 해제 — NULL 안전 */
	if (ctx->client_conn != NULL) {                    /* [한국어] connect 가 성공했었다면 client 종료 */
		spdk_jsonrpc_client_close(ctx->client_conn);
	}

	spdk_rpc_server_finish(ctx->rpc_socket_path_temp);  /* [한국어] 임시 UDS 서버 종료 — rpc.c 가 STAILQ 에서 제거 */

	SPDK_DEBUG_APP_CFG("Config load finished with rc %d\n", rc);
	ctx->cb_fn(rc, ctx->cb_arg);                       /* [한국어] 사용자 완료 콜백 호출 */

	free(ctx->json_data);                              /* [한국어] JSON 텍스트 사본 해제 */
	free(ctx->values);                                 /* [한국어] JSON 토큰 배열 해제 */
	free(ctx);                                         /* [한국어] ctx 자체 해제 */
}

/*
 * [한국어]
 * rpc_client_set_timeout - 현재 시각으로부터 timeout_us 후를 ctx->timeout 에 저장
 *
 * @ctx: 로딩 컨텍스트.
 * @timeout_us: 마이크로초 단위 timeout.
 * @return: void.
 *
 * SPDK 내부 시간은 spdk_get_ticks (TSC 기반 카운터). spdk_get_ticks_hz 가 ticks/sec.
 * timeout_us 마이크로초를 ticks 로 환산하여 현재 ticks 에 더한다.
 *
 * 호출 체인:
 *   client_send_request, rpc_client_poller (timeout 초기화/리셋) → [rpc_client_set_timeout]
 */
static void
rpc_client_set_timeout(struct load_json_config_ctx *ctx, uint64_t timeout_us)
{
	/* [한국어] 절대 만료 시각 = 현재 ticks + (timeout_us * ticks_per_sec / 1_000_000) */
	ctx->timeout = spdk_get_ticks() + timeout_us * spdk_get_ticks_hz() / (1000 * 1000);
}

/*
 * [한국어]
 * rpc_client_check_timeout - 현재 시각이 ctx->timeout 을 넘었는지 검사
 *
 * @ctx: 로딩 컨텍스트.
 * @return: 0=아직 시간 남음, -ETIMEDOUT=만료됨.
 *
 * 호출 체인:
 *   rpc_client_poller, rpc_client_connect_poller → [rpc_client_check_timeout]
 */
static int
rpc_client_check_timeout(struct load_json_config_ctx *ctx)
{
	if (ctx->timeout < spdk_get_ticks()) {       /* [한국어] 절대 만료 시각이 현재보다 이전이면 expired */
		SPDK_WARNLOG("RPC client command timeout.\n");
		return -ETIMEDOUT;
	}

	return 0;
}

/* [한국어] 에러 응답을 사람이 읽기 좋은 문자열로 직렬화하기 위한 임시 버퍼. */
struct json_write_buf {
	char data[1024];
	/* [한국어] 직렬화된 JSON 텍스트가 누적될 고정 버퍼.
	 * 설정자: json_write_stdout 콜백이 매 호출마다 cur_off 위치에 snprintf.
	 * 값 범위: 1024B 이하. 초과 시 잘림. */

	unsigned cur_off;
	/* [한국어] 다음 쓰기 위치 (이미 채운 바이트 수).
	 * 설정자: json_write_stdout 매 호출마다 누적 증가. */
};

/*
 * [한국어]
 * json_write_stdout - spdk_json_write_begin 의 출력 콜백 — 버퍼에 누적
 *
 * @cb_ctx: json_write_buf* — 출력 버퍼.
 * @data: 작성할 JSON 텍스트 조각.
 * @size: 조각 크기.
 * @return: 0=정상 (snprintf 가 모두 썼다면), -1=잘림/에러.
 *
 * spdk_json_write_begin 이 작성 중 호출하는 stream-write 콜백 인터페이스. 본 함수는
 * 그 출력을 stdout 에 보내는 게 아니라 스택 버퍼에 누적해 SPDK_ERRLOG 로 한 번에
 * 출력하기 위함 (RPC 에러 응답 사람이 보기 좋게 표현 목적).
 *
 * 호출 체인:
 *   spdk_json_write_val/end (rpc_client_poller 의 에러 처리 경로) → [json_write_stdout]
 */
static int
json_write_stdout(void *cb_ctx, const void *data, size_t size)
{
	struct json_write_buf *buf = cb_ctx;        /* [한국어] void* 콜백 컨텍스트를 실제 타입으로 캐스팅 */
	size_t rc;                                  /* [한국어] snprintf 반환값 (실제 쓴 바이트 수) */

	/* [한국어] 버퍼 잔여 공간만큼 snprintf 로 텍스트 누적. */
	rc = snprintf(buf->data + buf->cur_off, sizeof(buf->data) - buf->cur_off,
		      "%s", (const char *)data);
	if (rc > 0) {
		buf->cur_off += rc;                 /* [한국어] 다음 쓰기 위치 갱신 */
	}
	return rc == size ? 0 : -1;                 /* [한국어] 모두 썼으면 0, 잘렸으면 -1 (호출 측이 작성 중단) */
}

/*
 * [한국어]
 * rpc_client_poller - JSON-RPC 응답 폴링 + 도착 시 콜백 실행 (poller 콜백)
 *
 * @arg: load_json_config_ctx*.
 * @return: SPDK_POLLER_BUSY (이번 tick 에 작업 발생).
 *
 * connect 성공 후 100us 주기 poller 로 등록되어 응답을 폴링한다.
 * (1) spdk_jsonrpc_client_poll(ctx->client_conn, 0): non-blocking 응답 수신 시도.
 *     반환값 0=응답 없음, >0=응답 도착, <0=에러.
 * (2) 응답 없음이면 timeout 체크 (10s 마다 WARNLOG, soft timeout 이라 진행 계속).
 * (3) 에러면 로딩 종료 (app_json_config_load_done(rc)).
 * (4) 응답 도착이면 spdk_jsonrpc_client_get_response 로 응답 객체 획득.
 *     resp->error 가 있으면 사람이 읽기 좋게 SPDK_ERRLOG 로 출력.
 *     stop_on_error 면 즉시 종료, 아니면 client_resp_cb (직전 송신 시 등록된 핸들러) 호출.
 *
 * 실행 컨텍스트: app thread poller (100 us 주기).
 *
 * 호출 체인:
 *   reactor poll loop → [rpc_client_poller]
 *     → spdk_jsonrpc_client_poll, ctx->client_resp_cb, app_json_config_load_done
 */
static int
rpc_client_poller(void *arg)
{
	struct load_json_config_ctx *ctx = arg;             /* [한국어] poller 컨텍스트 캐스팅 */
	struct spdk_jsonrpc_client_response *resp;          /* [한국어] 도착한 응답 객체 (성공 시) */
	client_resp_handler cb;                             /* [한국어] 응답 처리 콜백 임시 보관 */
	int rc;                                             /* [한국어] poll 결과 / 타임아웃 검사 결과 */

	assert(spdk_thread_is_app_thread(NULL));            /* [한국어] app thread 단독 실행 강제 */

	rc = spdk_jsonrpc_client_poll(ctx->client_conn, 0); /* [한국어] non-blocking 응답 수신 (timeout=0) */
	if (rc == 0) {                                      /* [한국어] 아직 응답 없음 — 타임아웃 검사로 분기 */
		rc = rpc_client_check_timeout(ctx);
		if (rc == -ETIMEDOUT) {
			/* [한국어] soft timeout — WARNLOG 만 찍고 다음 10초 윈도우로 reset 후 진행 */
			rpc_client_set_timeout(ctx, RPC_CLIENT_REQUEST_TIMEOUT_US);
			rc = 0;
		}
	}

	if (rc == 0) {
		/* No response yet */
		return SPDK_POLLER_BUSY;                    /* [한국어] 응답 없음 + 타임아웃 아님 → 다음 tick 까지 대기 */
	} else if (rc < 0) {                                /* [한국어] poll 에러 (연결 끊김 등) — 로딩 즉시 종료 */
		app_json_config_load_done(ctx, rc);
		return SPDK_POLLER_BUSY;
	}

	resp = spdk_jsonrpc_client_get_response(ctx->client_conn);  /* [한국어] 도착한 응답 객체 추출 */
	assert(resp);

	if (resp->error) {                                  /* [한국어] 응답에 error 필드 — 서버가 RPC 에러 응답 */
		struct json_write_buf buf = {};             /* [한국어] 에러 객체를 사람이 보기 좋게 직렬화할 버퍼 */
		struct spdk_json_write_ctx *w = spdk_json_write_begin(json_write_stdout,
						&buf, SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);

		if (w == NULL) {                            /* [한국어] 직렬화기 할당 실패 — 미상의 에러로 표기 */
			SPDK_ERRLOG("error response: (?)\n");
		} else {
			spdk_json_write_val(w, resp->error); /* [한국어] error 객체를 JSON 텍스트로 작성 */
			spdk_json_write_end(w);              /* [한국어] writer 종료 (flush) */
			SPDK_ERRLOG("error response: \n%s\n", buf.data);  /* [한국어] 사용자에게 노출 */
		}
	}

	if (resp->error && ctx->stop_on_error) {            /* [한국어] 에러 + stop_on_error → 즉시 종료 */
		spdk_jsonrpc_client_free_response(resp);
		app_json_config_load_done(ctx, -EINVAL);
	} else {
		/* We have response so we must have callback for it. */
		/* [한국어] 응답을 받았으면 직전 송신 시 등록한 콜백이 반드시 있어야 함 */
		cb = ctx->client_resp_cb;
		assert(cb != NULL);

		/* Mark we are done with this handler. */
		ctx->client_resp_cb = NULL;                 /* [한국어] 콜백 1회성 — NULL 로 클리어 */
		cb(ctx, resp);                              /* [한국어] 실제 응답 처리 콜백 호출 (resp 의 free 책임은 콜백 측) */
	}


	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * rpc_client_connect_poller - 임시 RPC 서버에 연결될 때까지 대기하는 poller
 *
 * @_ctx: load_json_config_ctx*.
 * @return: SPDK_POLLER_BUSY 또는 SPDK_POLLER_IDLE.
 *
 * spdk_jsonrpc_client_connect 는 비동기 연결 — connect 가 진행되는 동안 poll 이
 * -ENOTCONN 을 반환한다. 연결되면 poll 결과가 다른 값 (>=0 또는 다른 에러) 으로 바뀜.
 * 연결되면 본 poller 를 unregister 하고, rpc_client_poller (응답 폴링) 로 교체한 뒤
 * 첫 RPC 발행을 시작 (app_json_config_load_subsystem).
 *
 * 호출 체인:
 *   reactor poll loop (100us) → [rpc_client_connect_poller]
 *     → 성공: SPDK_POLLER_REGISTER(rpc_client_poller), app_json_config_load_subsystem
 *     → timeout: app_json_config_load_done(-ETIMEDOUT)
 */
static int
rpc_client_connect_poller(void *_ctx)
{
	struct load_json_config_ctx *ctx = _ctx;
	int rc;

	rc = spdk_jsonrpc_client_poll(ctx->client_conn, 0);  /* [한국어] non-blocking 연결 진행 시도 */
	if (rc != -ENOTCONN) {
		/* We are connected. Start regular poller and issue first request */
		spdk_poller_unregister(&ctx->client_conn_poller);   /* [한국어] connect poller 해제 */
		/* [한국어] 응답 poller 로 교체 — 100us 주기로 응답 수신 처리 */
		ctx->client_conn_poller = SPDK_POLLER_REGISTER(rpc_client_poller, ctx, 100);
		app_json_config_load_subsystem(ctx);                /* [한국어] 첫 서브시스템 엔트리 처리 시작 */
	} else {
		/* [한국어] 아직 연결 진행 중 — 타임아웃만 검사 */
		rc = rpc_client_check_timeout(ctx);
		if (rc) {
			/* [한국어] 1초 안에 connect 못했으면 종료 */
			app_json_config_load_done(ctx, rc);
		}

		return SPDK_POLLER_IDLE;                            /* [한국어] 일을 안했음(=대기 중) — SPDK 통계상 idle */
	}

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * client_send_request - 한 RPC 요청을 자기 자신의 RPC 서버로 송신
 *
 * @ctx: 로딩 컨텍스트.
 * @request: 직렬화된 spdk_jsonrpc_client_request (이미 method/params 작성 완료).
 * @client_resp_cb: 응답 도착 시 호출할 콜백.
 * @return: 0=송신 성공, 음수=에러.
 *
 * 송신 전 다음 작업:
 *  - ctx->client_resp_cb 에 응답 콜백 등록 (rpc_client_poller 가 사용)
 *  - timeout 카운터 리셋 (10초)
 *  - spdk_jsonrpc_client_send_request: 요청 객체를 클라이언트의 outgoing 큐에 enqueue.
 *    실제 전송은 jsonrpc 라이브러리 내부 poller 가 진행.
 *
 * 호출 체인:
 *   app_json_config_load_subsystem_config_entry → [client_send_request]
 *     → spdk_jsonrpc_client_send_request
 */
static int
client_send_request(struct load_json_config_ctx *ctx, struct spdk_jsonrpc_client_request *request,
		    client_resp_handler client_resp_cb)
{
	int rc;

	assert(spdk_thread_is_app_thread(NULL));    /* [한국어] app thread 단독 호출 */

	ctx->client_resp_cb = client_resp_cb;       /* [한국어] 응답 도착 시 호출할 콜백 등록 */
	rpc_client_set_timeout(ctx, RPC_CLIENT_REQUEST_TIMEOUT_US);  /* [한국어] 10초 soft timeout 시작 */
	rc = spdk_jsonrpc_client_send_request(ctx->client_conn, request);  /* [한국어] 요청 전송 큐에 추가 */

	if (rc) {
		SPDK_DEBUG_APP_CFG("Sending request to client failed (%d)\n", rc);
	}

	return rc;
}

/*
 * [한국어]
 * cap_string - "JSON 값이 string 이면 포인터를 캡처" 디코더 (커스텀 디코더 함수)
 *
 * @val: 디코딩 대상 JSON 토큰.
 * @out: spdk_json_val** 위치 — 캡처할 포인터의 주소.
 * @return: 0=정상, -EINVAL=string 아님.
 *
 * spdk_json_decode_object 가 디코더 테이블의 함수 포인터로 호출. 일반적인 디코더는
 * 값을 사본으로 만들어 저장하지만, 본 디코더는 토큰 포인터 자체를 캡처 — 이후
 * raw JSON 텍스트를 직접 dump 할 수 있게 하기 위함 (params 그대로 RPC 로 forwarding).
 *
 * 호출 체인:
 *   spdk_json_decode_object → [cap_string]
 */
static int
cap_string(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;    /* [한국어] out 은 spdk_json_val** 의 void* 포장 */

	if (val->type != SPDK_JSON_VAL_STRING) {    /* [한국어] 타입 검사 — string 아니면 거부 */
		return -EINVAL;
	}

	*vptr = val;                                /* [한국어] 토큰 포인터 캡처 (값 사본 X) */
	return 0;
}

/*
 * [한국어]
 * cap_object - "JSON 값이 object 시작이면 포인터 캡처" 디코더
 *
 * @val: 디코딩 대상 토큰.
 * @out: spdk_json_val** 위치.
 * @return: 0=정상, -EINVAL=object 아님.
 *
 * params 객체를 통째로 raw 로 RPC 에 전달하기 위한 캡처 디코더.
 *
 * 호출 체인:
 *   spdk_json_decode_object → [cap_object]
 */
static int
cap_object(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;

	if (val->type != SPDK_JSON_VAL_OBJECT_BEGIN) {  /* [한국어] '{' 시작 토큰만 허용 */
		return -EINVAL;
	}

	*vptr = val;
	return 0;
}


/*
 * [한국어]
 * cap_array_or_null - "JSON 값이 array 또는 null 이면 포인터 캡처" 디코더
 *
 * @val: 디코딩 대상.
 * @out: spdk_json_val** 위치.
 * @return: 0=정상, -EINVAL=둘 다 아님.
 *
 * "config" 키가 빈 서브시스템(아직 설정 없음)을 표현할 때 null 도 허용하기 위함.
 *
 * 호출 체인:
 *   spdk_json_decode_object → [cap_array_or_null]
 */
static int
cap_array_or_null(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;

	if (val->type != SPDK_JSON_VAL_ARRAY_BEGIN && val->type != SPDK_JSON_VAL_NULL) {
		return -EINVAL;
	}

	*vptr = val;
	return 0;
}

/* [한국어] config 배열의 한 엔트리(=하나의 RPC 명령)를 디코딩한 결과를 담는 임시 구조체.
 * 매 엔트리마다 스택에 할당되어 디코딩 후 즉시 사용/해제. */
struct config_entry {
	char *method;
	/* [한국어] 호출할 RPC 메서드 이름 (예: "bdev_malloc_create").
	 * 설정자: spdk_json_decode_string (strdup).
	 * 해제자: 본 엔트리 처리 끝 free(cfg.method).
	 * 동기화: 단일 엔트리 처리 내 임시 — 동기화 무관. */

	struct spdk_json_val *params;
	/* [한국어] params 객체 토큰 포인터 (없으면 NULL — optional).
	 * 설정자: cap_object.
	 * 읽는 자: 본 엔트리 처리 시 raw JSON 으로 RPC 요청에 forwarding.
	 * 값 범위: ctx->values 배열 안의 토큰 포인터 (사본 아님). */
};

/* [한국어] config 엔트리 디코더 테이블 — "method" 는 string 필수, "params" 는 object 옵션. */
static struct spdk_json_object_decoder jsonrpc_cmd_decoders[] = {
	{"method", offsetof(struct config_entry, method), spdk_json_decode_string},
	{"params", offsetof(struct config_entry, params), cap_object, true}
	/* [한국어] 네 번째 인자 true = optional. params 없으면 cfg.params=NULL. */
};

/* [한국어] forward declaration */
static void app_json_config_load_subsystem_config_entry(void *_ctx);

/*
 * [한국어]
 * app_json_config_load_subsystem_config_entry_next - 한 엔트리 응답 처리 후 다음 엔트리로 진행
 *
 * @ctx: 로딩 컨텍스트.
 * @resp: 직전 RPC 의 응답 (success 또는 ignored error in best-effort mode).
 * @return: void.
 *
 * client_resp_cb 로 등록되어 응답 도착 시 rpc_client_poller 가 호출.
 * 응답 자체에는 관심 없고(JSON 구성 로딩은 fire-and-forget), 다음 config_it 로 이동
 * 후 다음 엔트리 처리를 트리거한다.
 *
 * 호출 체인:
 *   rpc_client_poller (응답 도착) → [app_json_config_load_subsystem_config_entry_next]
 *     → spdk_jsonrpc_client_free_response, spdk_json_next, app_json_config_load_subsystem_config_entry
 */
static void
app_json_config_load_subsystem_config_entry_next(struct load_json_config_ctx *ctx,
		struct spdk_jsonrpc_client_response *resp)
{
	/* Don't care about the response */
	spdk_jsonrpc_client_free_response(resp);    /* [한국어] 응답 객체 즉시 해제 — 내용 무관 */

	ctx->config_it = spdk_json_next(ctx->config_it);  /* [한국어] 다음 config 엔트리로 진행 (없으면 NULL) */
	app_json_config_load_subsystem_config_entry(ctx); /* [한국어] 다음 엔트리 처리 (또는 서브시스템 종료) */
}

/* Load "config" entry */
/*
 * [한국어]
 * app_json_config_load_subsystem_config_entry - 한 RPC 명령(=config 배열의 한 엔트리)을 발행
 *
 * @_ctx: load_json_config_ctx*.
 * @return: void.
 *
 * (1) ctx->config_it == NULL → 현재 서브시스템 모든 엔트리 처리 완료 → 다음 서브시스템으로.
 * (2) 엔트리를 jsonrpc_cmd_decoders 로 디코드 → method/params 캡처. 실패 시 종료.
 * (3) spdk_rpc_get_method_state_mask: 등록된 메서드의 허용 phase 마스크 조회.
 *     - -ENOENT (메서드 없음): stop_on_error 면 에러로 종료, 아니면 skip.
 *       단 stop_on_error 인데 해당 서브시스템 자체가 빌드에 미링크인 경우 skip 허용 (다른 앱과 호환).
 * (4) 현재 phase 가 메서드의 허용 phase 와 맞지 않으면 skip.
 * (5) STARTUP|RUNTIME 둘 다 허용된 메서드는 STARTUP 에서 한 번 호출됐으므로 RUNTIME 에서 skip.
 * (6) 정상 경로: jsonrpc 요청 빌드 → method, params (raw forwarding) 작성 → client_send_request.
 *
 * raw forwarding: cfg.params 가 가리키는 JSON 텍스트를 그대로 요청에 복사. 파싱·검증은
 * 서버측 핸들러가 다시 한다 — 로더는 키/값 하나하나 모르고 통과시킴.
 *
 * "Invoke later to avoid recursion" 패턴:
 *  - 동기 skip 들이 연쇄될 때 깊은 콜 스택을 피하기 위해 spdk_thread_send_msg 로 재예약.
 *  - app thread 의 message queue 에 self 메시지 enqueue → 다음 reactor tick 에 처리.
 *
 * 호출 체인:
 *   app_json_config_load_subsystem (시작) / 본 함수 자기 재예약 / next 콜백
 *     → [app_json_config_load_subsystem_config_entry]
 *     → client_send_request → rpc_client_poller → next → 본 함수
 */
static void
app_json_config_load_subsystem_config_entry(void *_ctx)
{
	struct load_json_config_ctx *ctx = _ctx;
	struct spdk_jsonrpc_client_request *rpc_request;     /* [한국어] 발행할 RPC 요청 객체 */
	struct spdk_json_write_ctx *w;                       /* [한국어] 요청 빌드용 JSON writer */
	struct config_entry cfg = {};                        /* [한국어] 디코딩 결과 (zero-init — free(NULL) 안전) */
	struct spdk_json_val *params_end;                    /* [한국어] params 객체 끝 토큰 (raw 복사 길이 계산용) */
	size_t params_len = 0;                               /* [한국어] params 의 raw JSON 텍스트 길이(B) */
	uint32_t state_mask = 0, cur_state_mask, startup_runtime = SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME;
	/* [한국어] state_mask: 메서드의 허용 phase 비트 마스크 (SPDK_RPC_STARTUP=1, SPDK_RPC_RUNTIME=2 등).
	 * cur_state_mask: 현재 SPDK 의 RPC 상태.
	 * startup_runtime: STARTUP|RUNTIME 둘 다 허용된 메서드 식별용 매스크. */
	int rc;

	if (ctx->config_it == NULL) {                        /* [한국어] 현재 서브시스템의 config 엔트리 모두 처리 완료 */
		SPDK_DEBUG_APP_CFG("Subsystem '%.*s': configuration done.\n", ctx->subsystem_name->len,
				   (char *)ctx->subsystem_name->start);
		ctx->subsystems_it = spdk_json_next(ctx->subsystems_it);  /* [한국어] 다음 서브시스템으로 */
		/* Invoke later to avoid recursion */
		/* [한국어] 다음 서브시스템 처리는 자기 메시지 큐로 재예약 — 깊은 호출 스택 회피 */
		spdk_thread_send_msg(spdk_thread_get_app_thread(), app_json_config_load_subsystem, ctx);
		return;
	}

	/* [한국어] 한 config 엔트리(JSON 객체) 디코드 — method 필수, params 옵션 */
	if (spdk_json_decode_object(ctx->config_it, jsonrpc_cmd_decoders,
				    SPDK_COUNTOF(jsonrpc_cmd_decoders), &cfg)) {
		SPDK_ERRLOG("Failed to decode config entry\n");
		app_json_config_load_done(ctx, -EINVAL);     /* [한국어] 디코드 실패 — 로딩 즉시 종료 */
		goto out;
	}

	rc = spdk_rpc_get_method_state_mask(cfg.method, &state_mask);  /* [한국어] 등록된 메서드의 허용 phase 조회 */
	if (rc == -ENOENT) {                                 /* [한국어] 그런 메서드 등록 안 됨 */
		if (!ctx->stop_on_error) {                   /* [한국어] best-effort 모드 — 단순 skip */
			/* Invoke later to avoid recursion */
			ctx->config_it = spdk_json_next(ctx->config_it);
			spdk_thread_send_msg(spdk_thread_get_app_thread(), app_json_config_load_subsystem_config_entry,
					     ctx);
		} else if (!spdk_subsystem_exists(ctx->subsystem_name_str)) {
			/* If the subsystem does not exist, just skip it, even
			 * if we are supposed to stop_on_error. Users may generate
			 * a JSON config from one application, and want to use parts
			 * of it in another application that may not have all of the
			 * same subsystems linked - for example, nvmf_tgt => bdevperf.
			 * That's OK, we don't need to throw an error, since any nvmf
			 * configuration wouldn't be used by bdevperf anyways. That is
			 * different than if some subsystem does exist in bdevperf and
			 * one of its RPCs fails.
			 */
			/* [한국어] stop_on_error 라도 해당 서브시스템 자체가 이 빌드에 미링크면 skip 허용 —
			 * 다른 앱(예: nvmf_tgt) 의 JSON 을 다른 앱(예: bdevperf) 에 부분 사용하는 시나리오 보존. */
			SPDK_NOTICELOG("Skipping method '%s' because its subsystem '%s' "
				       "is not linked into this application.\n",
				       cfg.method, ctx->subsystem_name_str);
			/* Invoke later to avoid recursion */
			ctx->config_it = spdk_json_next(ctx->config_it);
			spdk_thread_send_msg(spdk_thread_get_app_thread(), app_json_config_load_subsystem_config_entry,
					     ctx);
		} else {
			/* [한국어] 메서드는 없는데 서브시스템은 존재 — 진짜 미정의 메서드. 에러로 종료. */
			SPDK_ERRLOG("Method '%s' was not found\n", cfg.method);
			app_json_config_load_done(ctx, rc);
		}
		goto out;
	}
	cur_state_mask = spdk_rpc_get_state();               /* [한국어] 현재 SPDK 의 RPC 상태 (STARTUP/RUNTIME) */
	if ((state_mask & cur_state_mask) != cur_state_mask) {  /* [한국어] 현재 phase 가 메서드 허용 phase 에 포함 안됨 — skip */
		SPDK_DEBUG_APP_CFG("Method '%s' not allowed -> skipping\n", cfg.method);
		/* Invoke later to avoid recursion */
		ctx->config_it = spdk_json_next(ctx->config_it);
		spdk_thread_send_msg(spdk_thread_get_app_thread(), app_json_config_load_subsystem_config_entry,
				     ctx);
		goto out;
	}
	if ((state_mask & startup_runtime) == startup_runtime && cur_state_mask == SPDK_RPC_RUNTIME) {
		/* Some methods are allowed to be run in both STARTUP and RUNTIME states.
		 * We should not call such methods twice, so ignore the second attempt in RUNTIME state */
		/* [한국어] STARTUP|RUNTIME 모두 허용된 메서드는 첫 패스(STARTUP) 에서 이미 실행됐음 — 두번째 패스 skip */
		SPDK_DEBUG_APP_CFG("Method '%s' has already been run in STARTUP state\n", cfg.method);
		/* Invoke later to avoid recursion */
		ctx->config_it = spdk_json_next(ctx->config_it);
		spdk_thread_send_msg(spdk_thread_get_app_thread(), app_json_config_load_subsystem_config_entry,
				     ctx);
		goto out;
	}

	SPDK_DEBUG_APP_CFG("\tmethod: %s\n", cfg.method);

	if (cfg.params) {
		/* Get _END by skipping params and going back by one element. */
		/* [한국어] params 토큰부터 spdk_json_val_len 만큼 건너뛰고 1 뒤로 — 이게 OBJECT_END '}' 위치 */
		params_end = cfg.params + spdk_json_val_len(cfg.params) - 1;

		/* Need to add one character to include '}' */
		/* [한국어] raw 텍스트 길이 = (END start - params start) + 1 (== '}' 포함) */
		params_len = params_end->start - cfg.params->start + 1;

		SPDK_DEBUG_APP_CFG("\tparams: %.*s\n", (int)params_len, (char *)cfg.params->start);
	}

	rpc_request = spdk_jsonrpc_client_create_request();   /* [한국어] 빈 요청 객체 할당 (jsonrpc 라이브러리) */
	if (!rpc_request) {
		app_json_config_load_done(ctx, -errno);       /* [한국어] 할당 실패 — errno 그대로 종료 코드로 */
		goto out;
	}

	/* [한국어] JSON-RPC 요청 빌드 시작 — id, method 필드 작성 시작점. id=0 (rpc_request_id 미사용) */
	w = spdk_jsonrpc_begin_request(rpc_request, ctx->rpc_request_id, NULL);
	if (!w) {
		spdk_jsonrpc_client_free_request(rpc_request);  /* [한국어] writer 할당 실패 — 요청도 회수 */
		app_json_config_load_done(ctx, -ENOMEM);
		goto out;
	}

	spdk_json_write_named_string(w, "method", cfg.method);  /* [한국어] "method":"<method_name>" 작성 */

	if (cfg.params) {
		/* No need to parse "params". Just dump the whole content of "params"
		 * directly into the request and let the remote side verify it. */
		/* [한국어] params 객체를 raw JSON 으로 그대로 forwarding — 키/값 검증은 서버 핸들러 책임. */
		spdk_json_write_name(w, "params");
		spdk_json_write_val_raw(w, cfg.params->start, params_len);
	}

	spdk_jsonrpc_end_request(rpc_request, w);             /* [한국어] 요청 빌드 종료 (구분자 마무리) */

	/* [한국어] 송신 — 응답 시 본 함수의 다음 단계 콜백(_next) 등록 */
	rc = client_send_request(ctx, rpc_request, app_json_config_load_subsystem_config_entry_next);
	if (rc != 0) {
		app_json_config_load_done(ctx, -rc);          /* [한국어] 송신 실패 — 종료 */
		goto out;
	}
out:
	free(cfg.method);                                     /* [한국어] strdup 된 method 문자열 해제 (cfg.method=NULL 이면 무해) */
}

/*
 * [한국어]
 * subsystem_init_done - spdk_subsystem_init 의 완료 콜백 — STARTUP→RUNTIME 전이 + 두번째 패스 시작
 *
 * @rc: subsystem init 결과 (0=성공).
 * @arg1: load_json_config_ctx*.
 * @return: void.
 *
 * deprecated path 의 일부 — initalize_subsystems=true 일 때만 사용.
 * 첫 번째 패스(STARTUP) 가 끝난 뒤 spdk_subsystem_init 이 호출되어 모든 서브시스템이
 * 실제로 init 완료한 후, 본 콜백이 RUNTIME 상태로 전환하고 subsystems_it 을 다시
 * 처음으로 리셋해 두 번째 패스(RUNTIME 메서드 발행) 를 시작한다.
 *
 * 호출 체인:
 *   spdk_subsystem_init (subsystem.c) → spdk_subsystem_init_next (체인 끝) → [subsystem_init_done]
 *     → spdk_rpc_set_state(SPDK_RPC_RUNTIME), app_json_config_load_subsystem
 */
static void
subsystem_init_done(int rc, void *arg1)
{
	struct load_json_config_ctx *ctx = arg1;

	if (rc) {                                             /* [한국어] 서브시스템 init 실패 — 로딩도 실패 처리 */
		app_json_config_load_done(ctx, rc);
		return;
	}

	spdk_rpc_set_state(SPDK_RPC_RUNTIME);                 /* [한국어] RPC 상태를 RUNTIME 으로 전환 — 이후 RUNTIME-only 메서드 호출 가능 */
	/* Another round. This time for RUNTIME methods */
	SPDK_DEBUG_APP_CFG("'framework_start_init' done - continuing configuration\n");

	assert(ctx != NULL);
	if (ctx->subsystems) {
		ctx->subsystems_it = spdk_json_array_first(ctx->subsystems);  /* [한국어] 두 번째 패스 시작점 — 첫 서브시스템 */
	}

	app_json_config_load_subsystem(ctx);                  /* [한국어] 두 번째 패스 (RUNTIME 메서드 발행) 시작 */
}

/* [한국어] 한 서브시스템 객체(=subsystems 배열의 한 원소)를 디코딩하는 디코더 테이블.
 * "subsystem":"<name>" string + "config":[...] array(or null). */
static struct spdk_json_object_decoder subsystem_decoders[] = {
	{"subsystem", offsetof(struct load_json_config_ctx, subsystem_name), cap_string},
	{"config", offsetof(struct load_json_config_ctx, config), cap_array_or_null}
};

/*
 * Start loading subsystem pointed by ctx->subsystems_it. This must point to the
 * beginning of the "subsystem" object in "subsystems" array or be NULL. If it is
 * NULL then no more subsystems to load.
 *
 * If "initalize_subsystems" is unset, then the function performs one iteration
 * and does not call subsystem initialization.
 *
 * There are two iterations, when "initalize_subsystems" context flag is set:
 *
 * In first iteration only STARTUP RPC methods are used, other methods are ignored. When
 * allsubsystems are walked the ctx->subsystems_it became NULL and "framework_start_init"
 * is called to let the SPDK move to RUNTIME state (initialize all subsystems) and
 * second iteration begins.
 *
 * In second iteration "subsystems" array is walked through again, this time only
 * RUNTIME RPC methods are used. When ctx->subsystems_it became NULL second time it
 * indicate that there is no more subsystems to load. The cb_fn is called to finish
 * configuration.
 */
/*
 * [한국어]
 * app_json_config_load_subsystem - 다음 서브시스템 엔트리 진입 (또는 패스 전환/종료)
 *
 * @_ctx: load_json_config_ctx*.
 * @return: void.
 *
 * (1) ctx->subsystems_it == NULL:
 *     - initalize_subsystems=true && 현재 STARTUP → spdk_subsystem_init 호출 → 두 번째 패스로 진입
 *     - 그 외 → 로딩 종료 (cb_fn(0)).
 * (2) 그렇지 않으면 현재 엔트리를 subsystem_decoders 로 디코드 (subsystem 이름 + config 배열).
 *     실패 시 종료.
 * (3) subsystem_name 의 NUL-종결 사본 만들기 (subsystem_name_str).
 * (4) 첫 config 엔트리부터 처리 시작 (app_json_config_load_subsystem_config_entry).
 *
 * 호출 체인:
 *   rpc_client_connect_poller / config_entry_next (재예약) / subsystem_init_done
 *     → [app_json_config_load_subsystem]
 *     → spdk_subsystem_init (deprecated path) 또는 app_json_config_load_subsystem_config_entry
 */
static void
app_json_config_load_subsystem(void *_ctx)
{
	struct load_json_config_ctx *ctx = _ctx;

	if (ctx->subsystems_it == NULL) {                    /* [한국어] 현재 패스의 모든 서브시스템 처리 완료 */
		if (ctx->initalize_subsystems && spdk_rpc_get_state() == SPDK_RPC_STARTUP) {
			/* [한국어] STARTUP 패스 끝 + initalize 모드 — subsystem_init 호출하여 RUNTIME 전이 */
			SPDK_DEBUG_APP_CFG("No more entries for current state, calling 'framework_start_init'\n");
			spdk_subsystem_init(subsystem_init_done, ctx);
		} else {
			/* [한국어] 그 외(현재 비-deprecated path 또는 두 번째 패스 끝) — 정상 종료 */
			SPDK_DEBUG_APP_CFG("No more entries for current state\n");
			app_json_config_load_done(ctx, 0);
		}

		return;
	}

	/* Capture subsystem name and config array */
	/* [한국어] 현재 서브시스템 엔트리 디코드 — subsystem_name, config 캡처 */
	if (spdk_json_decode_object(ctx->subsystems_it, subsystem_decoders,
				    SPDK_COUNTOF(subsystem_decoders), ctx)) {
		SPDK_ERRLOG("Failed to parse subsystem configuration\n");
		app_json_config_load_done(ctx, -EINVAL);
		return;
	}

	/* [한국어] subsystem_name 토큰(start, len) 을 NUL-종결 문자열로 복사 — 후속 함수가 사용 */
	snprintf(ctx->subsystem_name_str, sizeof(ctx->subsystem_name_str),
		 "%.*s", ctx->subsystem_name->len, (char *)ctx->subsystem_name->start);

	SPDK_DEBUG_APP_CFG("Loading subsystem '%s' configuration\n", ctx->subsystem_name_str);

	/* Get 'config' array first configuration entry */
	/* [한국어] config 배열의 첫 엔트리(=첫 RPC 명령) 시작점 — 빈 배열/null 이면 NULL */
	ctx->config_it = spdk_json_array_first(ctx->config);
	app_json_config_load_subsystem_config_entry(ctx);     /* [한국어] 첫 엔트리 처리 시작 */
}

/*
 * [한국어]
 * parse_json - 사용자 JSON 데이터를 파싱하여 ctx->values 토큰 배열로 채움
 *
 * @json: 사용자 입력 (raw JSON 텍스트).
 * @json_size: 입력 크기 (바이트).
 * @ctx: 채울 ctx (json_data, values, values_cnt 가 채워짐).
 * @return: 0=성공, -EINVAL=파싱 실패.
 *
 * 2단계 파싱 패턴:
 *  - 1패스: spdk_json_parse(values=NULL) — 토큰 개수만 카운트.
 *  - 토큰 배열 calloc.
 *  - 2패스: spdk_json_parse(values=배열) — 실제 토큰 채우기.
 * spdk_json_parse 는 in-place 디코드를 수행할 수 있으므로 json_data 사본을 먼저 만든다.
 *
 * SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS: SPDK 의 JSON 확장으로 // 와 / * * / 주석 허용.
 *
 * 호출 체인:
 *   json_config_prepare_ctx → [parse_json]
 *     → spdk_json_parse (lib/json) ×2
 */
static int
parse_json(void *json, ssize_t json_size, struct load_json_config_ctx *ctx)
{
	void *end;                                            /* [한국어] 파싱 종료 위치 (사용 안 하지만 API 가 요구) */
	ssize_t rc;                                           /* [한국어] 파싱 반환값 (토큰 수 또는 음수 에러) */

	if (!json || json_size <= 0) {
		SPDK_ERRLOG("JSON data cannot be empty\n");
		goto err;
	}

	ctx->json_data = calloc(1, json_size);                /* [한국어] in-place 파싱용 사본 (zero-init) */
	if (!ctx->json_data) {
		goto err;
	}
	memcpy(ctx->json_data, json, json_size);              /* [한국어] 사용자 입력 복사 */
	ctx->json_data_size = json_size;

	/* [한국어] 1패스 — 토큰 개수만 알아낸다 (values=NULL, values_cnt=0). 반환값=토큰 수. */
	rc = spdk_json_parse(ctx->json_data, ctx->json_data_size, NULL, 0, &end,
			     SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	if (rc < 0) {
		SPDK_ERRLOG("Parsing JSON configuration failed (%zd)\n", rc);
		goto err;
	}

	ctx->values_cnt = rc;                                 /* [한국어] 토큰 수 저장 */
	ctx->values = calloc(ctx->values_cnt, sizeof(struct spdk_json_val));  /* [한국어] 토큰 배열 할당 */
	if (ctx->values == NULL) {
		SPDK_ERRLOG("Out of memory\n");
		goto err;
	}

	/* [한국어] 2패스 — 토큰 배열을 실제로 채움. 두 패스의 결과 토큰 수가 일치해야 함. */
	rc = spdk_json_parse(ctx->json_data, ctx->json_data_size, ctx->values,
			     ctx->values_cnt, &end,
			     SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	if ((size_t)rc != ctx->values_cnt) {
		SPDK_ERRLOG("Parsing JSON configuration failed (%zd)\n", rc);
		goto err;
	}

	return 0;
err:
	free(ctx->values);                                    /* [한국어] 부분 할당된 자원 회수 (json_data 는 호출자 측 ctx 정리에서 처리) */
	return -EINVAL;
}

/*
 * [한국어]
 * json_config_prepare_ctx - 로딩 컨텍스트 할당 + 파싱 + 임시 RPC 서버/클라이언트 셋업
 *
 * @cb_fn: 사용자 완료 콜백.
 * @cb_arg: cb_fn 인자.
 * @stop_on_error: 에러 시 중단 여부.
 * @json: 사용자 JSON 텍스트.
 * @json_size: 텍스트 크기.
 * @initalize_subsystems: deprecated 모드 플래그 (true=spdk_subsystem_init 도 호출).
 * @return: void (실패도 cb_fn 으로 통보).
 *
 * 셋업 단계:
 *  1. ctx 할당.
 *  2. parse_json 으로 JSON 파싱.
 *  3. "subsystems" 배열 위치 찾기 — 미존재/타입 오류 처리.
 *  4. 임시 UDS 경로 생성 — `<DEFAULT>.<pid>_<ticks>_config` 로 유일성 보장.
 *  5. spdk_rpc_initialize: 그 경로에 RPC 서버 띄움 (자기 자신이 받음).
 *  6. spdk_jsonrpc_client_connect: 그 경로로 클라이언트 연결 시작 (비동기).
 *  7. connect timeout 1초 설정 + connect poller 등록.
 *
 * 임시 소켓을 쓰는 이유: 정식 관리 소켓을 그대로 쓰면 동시에 외부 클라이언트가 끼어들 수
 * 있어 부팅 중 race condition 위험. 별도 임시 소켓으로 격리.
 *
 * 호출 체인:
 *   spdk_subsystem_load_config → [json_config_prepare_ctx]
 *     → parse_json, spdk_rpc_initialize (rpc.c), spdk_jsonrpc_client_connect, SPDK_POLLER_REGISTER
 */
static void
json_config_prepare_ctx(spdk_subsystem_init_fn cb_fn, void *cb_arg, bool stop_on_error, void *json,
			ssize_t json_size, bool initalize_subsystems)
{
	struct load_json_config_ctx *ctx = calloc(1, sizeof(*ctx));   /* [한국어] 컨텍스트 zero-init 할당 */
	int rc;

	if (!ctx) {
		cb_fn(-ENOMEM, cb_arg);                       /* [한국어] 할당 실패 — 즉시 사용자 콜백으로 통보 */
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->stop_on_error = stop_on_error;
	ctx->initalize_subsystems = initalize_subsystems;

	rc = parse_json(json, json_size, ctx);                /* [한국어] JSON 파싱 — values 채움 */
	if (rc < 0) {
		goto fail;
	}

	/* Capture subsystems array */
	/* [한국어] 최상위 객체에서 "subsystems" 배열 검색 */
	rc = spdk_json_find_array(ctx->values, "subsystems", NULL, &ctx->subsystems);
	switch (rc) {
	case 0:
		/* Get first subsystem */
		ctx->subsystems_it = spdk_json_array_first(ctx->subsystems);  /* [한국어] 첫 서브시스템 엔트리 위치 */
		if (ctx->subsystems_it == NULL) {
			/* [한국어] 빈 subsystems 배열 — 별 작업 없이 바로 종료 (NOTICE 로 알림) */
			SPDK_NOTICELOG("'subsystems' configuration is empty\n");
		}
		break;
	case -EPROTOTYPE:
		SPDK_ERRLOG("Invalid JSON configuration: not enclosed in {}.\n");  /* [한국어] 최상위가 객체가 아님 */
		goto fail;
	case -ENOENT:
		SPDK_WARNLOG("No 'subsystems' key JSON configuration file.\n");    /* [한국어] subsystems 키 누락 — 경고만, 진행 */
		break;
	case -EDOM:
		SPDK_ERRLOG("Invalid JSON configuration: 'subsystems' should be an array.\n");
		goto fail;
	default:
		SPDK_ERRLOG("Failed to parse JSON configuration.\n");
		goto fail;
	}

	/* FIXME: rpc client should use socketpair() instead of this temporary socket nonsense */
	/* [한국어] 임시 UDS 경로 생성 — `<SPDK_DEFAULT_RPC_ADDR>.<pid>_<ticks>_config`.
	 * pid 와 ticks 조합으로 동일 호스트의 다른 SPDK 인스턴스/동시 호출과 충돌 회피. */
	rc = snprintf(ctx->rpc_socket_path_temp, sizeof(ctx->rpc_socket_path_temp),
		      "%s.%d_%"PRIu64"_config", SPDK_DEFAULT_RPC_ADDR, getpid(), spdk_get_ticks());
	if (rc >= (int)sizeof(ctx->rpc_socket_path_temp)) {
		SPDK_ERRLOG("Socket name create failed\n");                    /* [한국어] 경로 너무 김 — sun_path 한도 초과 */
		goto fail;
	}

	rc = spdk_rpc_initialize(ctx->rpc_socket_path_temp, NULL);             /* [한국어] 임시 RPC 서버 띄우기 (rpc.c) */
	if (rc) {
		goto fail;
	}

	ctx->client_conn = spdk_jsonrpc_client_connect(ctx->rpc_socket_path_temp, AF_UNIX);  /* [한국어] 그 서버에 클라이언트 연결 시작 (비동기) */
	if (ctx->client_conn == NULL) {
		SPDK_ERRLOG("Failed to connect to '%s'\n", ctx->rpc_socket_path_temp);
		goto fail;
	}

	rpc_client_set_timeout(ctx, RPC_CLIENT_CONNECT_TIMEOUT_US);            /* [한국어] connect 타임아웃 1초 */
	/* [한국어] connect 진행 polling 시작 — 100us 주기. 연결되면 응답 poller 로 자동 전환. */
	ctx->client_conn_poller = SPDK_POLLER_REGISTER(rpc_client_connect_poller, ctx, 100);
	return;

fail:
	app_json_config_load_done(ctx, -EINVAL);                               /* [한국어] 셋업 실패 경로 — 부분 자원 정리 + 사용자 콜백 */
}

/*
 * [한국어]
 * spdk_subsystem_load_config - SPDK JSON 구성 파일 로딩 진입점 (공개 API)
 *
 * @json: 사용자 JSON 텍스트.
 * @json_size: 텍스트 크기 (바이트).
 * @cb_fn: 모든 로딩 완료 시 호출될 사용자 콜백 (rc, cb_arg).
 * @cb_arg: cb_fn 인자.
 * @stop_on_error: true=어떤 RPC 에러 시 즉시 중단, false=best-effort.
 * @return: void. 결과는 cb_fn 으로 비동기 통보.
 *
 * 호출자(예: spdk_app_start 또는 사용자)가 파일을 읽어 메모리에 올린 뒤 본 함수에 전달.
 * 본 함수는 initalize_subsystems=false 로만 호출 — 즉 STARTUP 메서드 한 번 발행하고
 * 끝낸다. 서브시스템 init / RUNTIME 메서드는 호출자가 별도 단계에서 처리하는 게
 * 권장 패턴. (deprecated path 인 두 번째 패스는 다른 진입점이 사용.)
 *
 * 실행 컨텍스트: SPDK app thread (master reactor) 단독 호출.
 *
 * 호출 체인:
 *   사용자/spdk_app_start → [spdk_subsystem_load_config] → json_config_prepare_ctx
 */
void
spdk_subsystem_load_config(void *json, ssize_t json_size, spdk_subsystem_init_fn cb_fn,
			   void *cb_arg, bool stop_on_error)
{
	assert(cb_fn);                                       /* [한국어] cb_fn 은 필수 — 결과 통보 채널 */
	assert(spdk_thread_is_app_thread(NULL));             /* [한국어] app thread 단독 호출 강제 */

	json_config_prepare_ctx(cb_fn, cb_arg, stop_on_error, json, json_size, false);
	/* [한국어] initalize_subsystems=false — 본 진입점은 init 호출 안 함. 호출자가 따로 진행. */
}

/* [한국어] "app_config" 로그 컴포넌트 등록 — SPDK_DEBUGLOG(app_config, ...) 가 활성화 가능해짐. */
SPDK_LOG_REGISTER_COMPONENT(app_config)
