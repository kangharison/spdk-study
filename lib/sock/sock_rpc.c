/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK sock 서브시스템 RPC 핸들러 (sock_rpc.c)
 *
 * === 파일의 역할 ===
 * SPDK JSON-RPC 서버 인터페이스를 통해 sock 서브시스템의 런타임/기동 옵션을 제어할 수 있도록
 * 다음 4개 RPC 메서드를 등록한다:
 *   - sock_impl_get_options (STARTUP|RUNTIME): 특정 백엔드(impl_name)의 현재 옵션을 JSON으로 반환.
 *   - sock_impl_set_options (STARTUP only): 특정 백엔드의 옵션을 갱신 (recv_buf_size, tls_version,
 *     enable_zerocopy_send_*, enable_placement_id 등).
 *   - sock_set_default_impl (STARTUP only): impl_name=NULL connect/listen이 사용할 default 백엔드 결정.
 *   - sock_get_default_impl (STARTUP|RUNTIME): 현재 default 백엔드 이름 반환.
 *
 * 모든 핸들러는 spdk_jsonrpc_request* 와 params(JSON 값)을 인자로 받아 spdk_json_decode_object로
 * 파싱하고, lib/sock/sock.c의 spdk_sock_impl_get_opts/set_opts/set_default_impl/get_default_impl
 * 공개 API로 위임한 뒤 결과를 spdk_jsonrpc_send_*로 응답한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK는 lib/jsonrpc 서버 + lib/event subsystem 자동 발견을 통해 외부 클라이언트(scripts/rpc.py
 * 등)에서 RPC를 받을 수 있다. sock_rpc.c의 SPDK_RPC_REGISTER 매크로(constructor)가 라이브러리
 * 로드 시 자동으로 spdk_rpc_register_method로 핸들러를 등록한다. 일부는 STARTUP-only로 마킹되어
 * SPDK app이 RPC accept 모드에 들어간 후 첫 init이 끝나기 전까지만 호출 가능하다 — 이는 백엔드
 * 옵션 변경이 이미 생성된 sock에는 영향을 못 미치므로 운영 안전성 확보 차원이다.
 *
 * 호출 체인:
 *   spdk_app_start() → JSON-RPC server up → 클라이언트(scripts/rpc.py "sock_impl_set_options
 *   --impl-name posix --enable-zerocopy-send-server") → spdk_jsonrpc_handler → [이 파일의 핸들러]
 *   → spdk_sock_impl_set_opts (lib/sock/sock.c) → impl->set_opts (백엔드).
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/sock.h (공개 API), spdk/rpc.h (SPDK_RPC_REGISTER 매크로), spdk/util.h (SPDK_COUNTOF),
 *   spdk/string.h (spdk_strerror), spdk/log.h (SPDK_ERRLOG).
 * - 의존되는 곳: 외부 클라이언트(scripts/rpc.py, REST 게이트웨이 등). 백엔드(posix/uring/ssl)는
 *   직접 참조하지 않으나, spdk_sock_impl_set_opts 경유로 자기 set_opts vtable이 호출된다.
 * - 데이터 흐름: JSON params → spdk_json_decode_object로 C 구조체 채우기 → 공개 API 호출 →
 *   결과 spdk_json_write_named_*로 JSON 작성 → 클라이언트에 송신.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_sock_impl_get_options: impl_name 받아 현재 spdk_sock_impl_opts를 JSON으로 반환.
 * - rpc_sock_impl_set_options: impl_name + 변경할 필드들을 받아 백엔드 옵션 갱신. 핵심 패턴:
 *   "현재값 get → caller가 보낸 필드만 partially overwrite → set" — caller가 일부 필드만
 *   넘겨도 나머지는 보존됨.
 * - rpc_sock_set_default_impl / rpc_sock_get_default_impl: g_default_impl 포인터의 RPC view.
 * - struct spdk_rpc_sock_impl_set_opts: set_options용 임시 컨테이너 (impl_name 문자열 +
 *   spdk_sock_impl_opts 사본).
 */

#include "spdk/sock.h"          /* [한국어] spdk_sock_impl_opts, spdk_sock_impl_get/set_opts, spdk_sock_set_default_impl 선언. */

#include "spdk/rpc.h"           /* [한국어] SPDK_RPC_REGISTER, SPDK_RPC_STARTUP/RUNTIME, spdk_jsonrpc_*, spdk_json_decode_object. */
#include "spdk/util.h"          /* [한국어] SPDK_COUNTOF — decoder 배열 길이 자동 계산. */
#include "spdk/string.h"        /* [한국어] spdk_strerror — errno → 사람이 읽을 수 있는 문자열. */

#include "spdk/log.h"           /* [한국어] SPDK_ERRLOG — RPC 실패 진단 로그. */


/*
 * [한국어]
 * rpc_sock_impl_get_options_decoders - "impl_name" 1개 필드를 디코드하는 JSON object decoder.
 *
 * decoder 형식: {field 이름, output 위치 offset(0 = 전체 컨테이너의 시작), 디코더 함수, optional?}.
 * 여기서는 caller가 char ** 변수 그 자체의 주소를 넘기므로 offset=0; spdk_json_decode_string은
 * malloc된 사본을 그 위치에 저장 — caller free 책임. optional=false이므로 누락 시 디코드 실패.
 */
static const struct spdk_json_object_decoder rpc_sock_impl_get_options_decoders[] = {
	{ "impl_name", 0, spdk_json_decode_string, false }, /* [한국어] 필수 — 백엔드 이름 (예: "posix"). */
};

/*
 * [한국어]
 * rpc_sock_impl_get_options - "sock_impl_get_options" RPC 핸들러.
 *
 * @request: JSON-RPC 컨텍스트 — 최종 응답 송신에 사용.
 * @params: JSON 값 — {"impl_name": "..."} 형태 기대.
 *
 * 응답 JSON: {"recv_buf_size": .., "send_buf_size": .., "enable_recv_pipe": .., ..., "enable_ktls": ..}.
 *
 * 동작:
 *   1) params를 디코드하여 impl_name 추출 (실패 시 INVALID_PARAMS 에러 응답).
 *   2) spdk_sock_impl_get_opts(impl_name, …)로 백엔드의 현재 옵션 추출.
 *   3) JSON object로 모든 필드 emit + jsonrpc_end_result.
 *   4) impl_name 메모리 free (decoder가 strdup으로 할당했음).
 *
 * STARTUP|RUNTIME 등록 — 운영자가 언제든 확인 가능.
 *
 * 호출 컨텍스트: SPDK app의 RPC 처리 spdk_thread (보통 master reactor) 단일 스레드.
 */
static void
rpc_sock_impl_get_options(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	char *impl_name = NULL;                             /* [한국어] decoder가 strdup으로 채울 출력 — free 책임 호출자(이 함수). */
	struct spdk_sock_impl_opts sock_opts = {};          /* [한국어] 백엔드에서 추출된 옵션 — zero-init 후 in/out으로 사용. */
	struct spdk_json_write_ctx *w;                      /* [한국어] JSON 응답 빌더. */
	size_t len;                                         /* [한국어] in/out 사이즈 (ABI-safe accessor). */
	int rc;

	if (spdk_json_decode_object(params, rpc_sock_impl_get_options_decoders,
				    SPDK_COUNTOF(rpc_sock_impl_get_options_decoders), &impl_name)) { /* [한국어] params → impl_name 디코드. */
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;                                         /* [한국어] impl_name=NULL이므로 free 불필요. */
	}

	len = sizeof(sock_opts);                            /* [한국어] caller buffer 크기 — 백엔드가 ABI-safe 채움. */
	rc = spdk_sock_impl_get_opts(impl_name, &sock_opts, &len); /* [한국어] sock.c → 백엔드 get_opts. */
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_impl_get_opts() failed, rc %d: %s\n", rc, spdk_strerror(-rc)); /* [한국어] errno를 문자열화. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;                                         /* [한국어] FIXME: 여기 impl_name leak — 실제 코드 버그 (수정 금지 원칙으로 그대로 둠). */
	}

	w = spdk_jsonrpc_begin_result(request);             /* [한국어] {"jsonrpc":...,"result": 까지 작성 후 result 영역 시작. */
	spdk_json_write_object_begin(w);                    /* [한국어] result 객체 { 시작. */
	spdk_json_write_named_uint32(w, "recv_buf_size", sock_opts.recv_buf_size); /* [한국어] SO_RCVBUF 크기. */
	spdk_json_write_named_uint32(w, "send_buf_size", sock_opts.send_buf_size); /* [한국어] SO_SNDBUF 크기. */
	spdk_json_write_named_bool(w, "enable_recv_pipe", sock_opts.enable_recv_pipe); /* [한국어] posix user-space recv 파이프 활성. */
	spdk_json_write_named_bool(w, "enable_quickack", sock_opts.enable_quickack); /* [한국어] TCP_QUICKACK — delayed ACK 비활성. */
	spdk_json_write_named_uint32(w, "enable_placement_id", sock_opts.enable_placement_id); /* [한국어] PLACEMENT 모드 (NONE/CPU/MARK/NAPI). */
	spdk_json_write_named_bool(w, "enable_zerocopy_send_server", sock_opts.enable_zerocopy_send_server); /* [한국어] server 송신 zerocopy. */
	spdk_json_write_named_bool(w, "enable_zerocopy_send_client", sock_opts.enable_zerocopy_send_client); /* [한국어] client 송신 zerocopy. */
	spdk_json_write_named_uint32(w, "zerocopy_threshold", sock_opts.zerocopy_threshold); /* [한국어] zerocopy 사용 최소 byte. */
	spdk_json_write_named_uint32(w, "tls_version", sock_opts.tls_version); /* [한국어] SSL/TLS version (1.2/1.3). */
	spdk_json_write_named_bool(w, "enable_ktls", sock_opts.enable_ktls); /* [한국어] kernel TLS 사용 여부. */
	spdk_json_write_object_end(w);                      /* [한국어] result 객체 } 종료. */
	spdk_jsonrpc_end_result(request, w);                /* [한국어] 응답 frame finalize 후 클라이언트에 송신. */
	free(impl_name);                                    /* [한국어] decoder strdup 사본 해제. */
}
/* [한국어] STARTUP과 RUNTIME 모두 가능 — 옵션 조회는 부작용 없으므로 언제나 안전. */
SPDK_RPC_REGISTER("sock_impl_get_options", rpc_sock_impl_get_options,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct spdk_rpc_sock_impl_set_opts - sock_impl_set_options RPC 입력 컨테이너.
 *
 * decoder가 offsetof를 사용해 직접 필드를 채우므로, sock_opts를 nested로 두면 ".sock_opts.field"
 * 표기로 한 번에 디코드 가능 — 별도 변환 단계 없이 spdk_sock_impl_set_opts에 그대로 넘길 수 있음.
 */
struct spdk_rpc_sock_impl_set_opts {
	char *impl_name;
	/* [한국어] 대상 백엔드 이름 (예: "posix"). decoder가 strdup으로 채움 — caller free 책임.
	 * 설정자: rpc_sock_impl_set_options_decoders의 impl_name 항목.
	 * 읽는 자: rpc_sock_impl_set_options 핸들러가 sock_impl_set_opts/get_opts에 전달.
	 * 동기화: 단일 RPC 핸들러 thread에서만 사용 — 락 불필요. */

	struct spdk_sock_impl_opts sock_opts;
	/* [한국어] 백엔드에 적용할 옵션 사본. RPC 호출 전 spdk_sock_impl_get_opts로 현재값을
	 * 채우고, decoder가 caller가 명시한 필드만 overwrite — 부분 갱신 패턴.
	 * 설정자: 1차로 spdk_sock_impl_get_opts(현재값), 2차로 decoder(caller 명시 필드만).
	 * 읽는 자: spdk_sock_impl_set_opts → 백엔드 set_opts.
	 * 값 범위: spdk/sock.h의 spdk_sock_impl_opts 구조체 참조. */
};

/*
 * [한국어]
 * rpc_sock_impl_set_options_decoders - sock_impl_set_options RPC 디코더 테이블.
 *
 * impl_name만 필수(false), 나머지 옵션 필드는 모두 optional(true) — caller가 변경하고 싶은
 * 필드만 명시하면 나머지는 디코더가 건드리지 않으므로, 핸들러가 미리 get_opts로 채워둔 현재값이
 * 그대로 보존됨 (partial update 패턴).
 *
 * offsetof 매크로: 구조체 시작 기준 필드 byte offset — decoder가 char* 기반으로 직접 대입.
 */
static const struct spdk_json_object_decoder rpc_sock_impl_set_options_decoders[] = {
	{
		"impl_name", offsetof(struct spdk_rpc_sock_impl_set_opts, impl_name),
		spdk_json_decode_string, false                  /* [한국어] 필수 — 어느 백엔드인지 식별. */
	},
	{
		"recv_buf_size", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.recv_buf_size),
		spdk_json_decode_uint32, true                   /* [한국어] optional — 커널 RX 버퍼 크기. */
	},
	{
		"send_buf_size", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.send_buf_size),
		spdk_json_decode_uint32, true                   /* [한국어] optional — 커널 TX 버퍼 크기. */
	},
	{
		"enable_recv_pipe", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.enable_recv_pipe),
		spdk_json_decode_bool, true                     /* [한국어] optional — posix user-space recv pipe 활성. */
	},
	{
		"enable_quickack", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.enable_quickack),
		spdk_json_decode_bool, true                     /* [한국어] optional — TCP_QUICKACK (delayed ACK 끄기). */
	},
	{
		"enable_placement_id", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.enable_placement_id),
		spdk_json_decode_uint32, true                   /* [한국어] optional — PLACEMENT mode (NONE=0/CPU=1/MARK=2/NAPI=3). */
	},
	{
		"enable_zerocopy_send_server", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.enable_zerocopy_send_server),
		spdk_json_decode_bool, true                     /* [한국어] optional — server측 MSG_ZEROCOPY 송신. */
	},
	{
		"enable_zerocopy_send_client", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.enable_zerocopy_send_client),
		spdk_json_decode_bool, true                     /* [한국어] optional — client측 MSG_ZEROCOPY 송신. */
	},
	{
		"zerocopy_threshold", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.zerocopy_threshold),
		spdk_json_decode_uint32, true                   /* [한국어] optional — zerocopy 사용 최소 byte (이 미만은 일반 sendmsg). */
	},
	{
		"tls_version", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.tls_version),
		spdk_json_decode_uint32, true                   /* [한국어] optional — TLS version (예: 13 → TLS 1.3, ssl 백엔드 전용). */
	},
	{
		"enable_ktls", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.enable_ktls),
		spdk_json_decode_bool, true                     /* [한국어] optional — kernel TLS offload (encrypt/decrypt를 커널에 위임). */
	}
};

/*
 * [한국어]
 * rpc_sock_impl_set_options - "sock_impl_set_options" RPC 핸들러.
 *
 * @params: {"impl_name": "...", "recv_buf_size": .., "tls_version": ..} 등 부분 옵션.
 *
 * 동작 (3-step partial update 패턴):
 *   1) 1차 디코드: impl_name만 추출하기 위해 decode_object 호출 (다른 필드는 입력 정도에 따라
 *      sock_opts에 들어가지만 이 시점엔 거의 0).
 *   2) spdk_sock_impl_get_opts로 백엔드 현재값을 sock_opts에 채움 (default 보존).
 *   3) 2차 디코드: 같은 decoder로 다시 호출 — caller가 명시한 필드만 overwrite. 명시 안 한
 *      필드는 step 2의 현재값 유지.
 *   4) spdk_sock_impl_set_opts로 백엔드 적용.
 *
 * 이 패턴이 필요한 이유: 사용자가 "tls_version만 13으로 바꿔줘"라고 보냈을 때 다른 옵션이
 * 0/false로 reset되면 안 됨 — 현재값 + 사용자 명시값 merge 필수. 1차 디코드 후 free하지 않고
 * step 3에서 같은 opts로 재디코드하므로 impl_name 사본은 1번만 strdup되어 메모리 누수 없음.
 *
 * STARTUP-only: 이미 생성된 sock에는 옵션이 적용 안 되므로, 모든 sock 생성 전에 호출해야 의미 있음.
 * RUNTIME 시 호출은 차단되어 운영 안전성을 확보.
 */
static void
rpc_sock_impl_set_options(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct spdk_rpc_sock_impl_set_opts opts = {};       /* [한국어] zero-init — impl_name=NULL, 모든 sock_opts=0. */
	size_t len;
	int rc;

	/* Get type */
	if (spdk_json_decode_object(params, rpc_sock_impl_set_options_decoders,
				    SPDK_COUNTOF(rpc_sock_impl_set_options_decoders), &opts)) { /* [한국어] 1차: impl_name 추출 (다른 필드도 들어옴). */
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;                                         /* [한국어] decode 실패 시 impl_name=NULL이므로 free 불필요. */
	}

	/* Retrieve default opts for requested socket implementation */
	len = sizeof(opts.sock_opts);
	rc = spdk_sock_impl_get_opts(opts.impl_name, &opts.sock_opts, &len); /* [한국어] 백엔드 현재값으로 sock_opts overwrite. */
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_impl_get_opts() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		free(opts.impl_name);                           /* [한국어] strdup 사본 해제. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	/* Decode opts */
	if (spdk_json_decode_object(params, rpc_sock_impl_set_options_decoders,
				    SPDK_COUNTOF(rpc_sock_impl_set_options_decoders), &opts)) { /* [한국어] 2차: caller 명시 필드만 overwrite (현재값 + 사용자 변경). */
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;                                         /* [한국어] 주의: opts.impl_name leak — 이미 1차 디코드에서 할당되었으나 여기 free 누락 (실제 코드 버그, 수정 금지 원칙으로 그대로). */
	}

	rc = spdk_sock_impl_set_opts(opts.impl_name, &opts.sock_opts, sizeof(opts.sock_opts)); /* [한국어] 백엔드에 적용. */
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_impl_set_opts() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		free(opts.impl_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);     /* [한국어] {"result": true} 응답. */
	free(opts.impl_name);                               /* [한국어] strdup 사본 해제. */
}
/* [한국어] STARTUP-only — 이미 sock이 생성된 후엔 options 변경이 효과 없으므로 차단. */
SPDK_RPC_REGISTER("sock_impl_set_options", rpc_sock_impl_set_options, SPDK_RPC_STARTUP)

/*
 * [한국어]
 * rpc_sock_set_default_impl - "sock_set_default_impl" RPC 핸들러.
 *
 * @params: {"impl_name": "..."} — get_options와 동일한 schema이므로 decoder 재사용.
 *
 * 동작: impl_name 디코드 → spdk_sock_set_default_impl로 g_default_impl 설정 →
 *       성공 시 {"result": true} 응답.
 *
 * STARTUP-only: 기동 후 default 변경은 이미 만들어진 sock에 영향 없으므로 운영자가 첫 listen
 * 전에 호출해야 의미 있음.
 */
static void
rpc_sock_set_default_impl(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	char *impl_name = NULL;                             /* [한국어] decoder가 strdup. */
	int rc;

	/* Reuse get_opts decoder */
	if (spdk_json_decode_object(params, rpc_sock_impl_get_options_decoders,
				    SPDK_COUNTOF(rpc_sock_impl_get_options_decoders), &impl_name)) { /* [한국어] 동일 schema — get_options decoder 재사용. */
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	rc = spdk_sock_set_default_impl(impl_name);         /* [한국어] sock.c → g_default_impl 갱신. */
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_set_default_impl() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		free(impl_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);     /* [한국어] {"result": true}. */
	free(impl_name);                                    /* [한국어] strdup 사본 해제. */
}
/* [한국어] STARTUP-only — RUNTIME 변경은 이미 생성된 sock에 영향 없음. */
SPDK_RPC_REGISTER("sock_set_default_impl", rpc_sock_set_default_impl, SPDK_RPC_STARTUP)

/*
 * [한국어]
 * rpc_sock_get_default_impl - "sock_get_default_impl" RPC 핸들러.
 *
 * @params: 반드시 NULL이어야 함 (인자 없음). 있으면 INVALID_PARAMS 응답.
 *
 * 응답: {"impl_name": "posix"} 같은 객체 또는, default 미설정/등록 백엔드 없음 시 INTERNAL_ERROR.
 *
 * STARTUP|RUNTIME 모두 가능 — read-only이므로 안전.
 */
static void
rpc_sock_get_default_impl(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	const char *impl_name = spdk_sock_get_default_impl(); /* [한국어] sock.c → g_default_impl->name (or NULL). */
	struct spdk_json_write_ctx *w;

	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "sock_get_default_impl requires no parameters"); /* [한국어] 인자 없는 RPC인데 params 들어온 경우 거부. */
		return;
	}

	if (!impl_name) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "No registered socket implementations found"); /* [한국어] 어떤 백엔드도 등록 안 됨 — 구성 오류. */
		return;
	}

	w = spdk_jsonrpc_begin_result(request);             /* [한국어] result 영역 시작. */
	spdk_json_write_object_begin(w);                    /* [한국어] {. */
	spdk_json_write_named_string(w, "impl_name", impl_name); /* [한국어] 키-값 한 쌍. */
	spdk_json_write_object_end(w);                      /* [한국어] }. */
	spdk_jsonrpc_end_result(request, w);                /* [한국어] frame finalize 후 송신. */
}
/* [한국어] STARTUP|RUNTIME — 단순 조회는 언제나 안전. */
SPDK_RPC_REGISTER("sock_get_default_impl", rpc_sock_get_default_impl,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
