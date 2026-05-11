/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] fsdev(파일시스템 디바이스) JSON-RPC 핸들러 (fsdev_rpc.c)
 *
 * === 파일의 역할 ===
 * fsdev 코어의 전역 옵션(spdk_fsdev_opts — IO 풀 크기, 캐시 크기 등)을
 * JSON-RPC를 통해 사용자 도구(rpc.py)나 외부 매니저가 조회/변경할 수 있게 하는
 * 두 개의 RPC 메서드("fsdev_get_opts", "fsdev_set_opts")를 등록한다.
 * fsdev 자체의 기능 RPC(예: fsdev_aio_create)는 각 모듈(module/fsdev/aio 등)에
 * 분리되어 있으며, 본 파일은 코어 옵션만 다룬다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   사용자 rpc.py (or HTTP/Unix client)
 *     → SPDK JSON-RPC 서버 (lib/jsonrpc + lib/rpc)
 *       → 본 파일의 핸들러 등록 매크로 SPDK_RPC_REGISTER("fsdev_get/set_opts", ...)
 *         → spdk_fsdev_get_opts / spdk_fsdev_set_opts (lib/fsdev/fsdev.c)
 *           → 전역 g_opts 구조체에 영향
 * SPDK_RPC_RUNTIME 단계로 등록 — 즉, 부팅 후 정상 운용 단계에서만 호출 가능.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/rpc, spdk/jsonrpc, spdk/util, spdk/fsdev (공개 옵션 API).
 * - 의존받음: 사용자 RPC 클라이언트.
 * - 데이터 흐름: 클라이언트 → JSON 디코드 → spdk_fsdev_set/get_opts → 결과를 JSON으로 응답.
 * - 공유 자료구조: spdk_fsdev_opts (lib/fsdev/fsdev.c의 g_opts 전역).
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_fsdev_get_opts          : 현재 옵션을 JSON으로 응답 (인자 없음).
 * - rpc_fsdev_set_opts          : JSON 입력으로 풀/캐시 크기를 변경.
 * - struct rpc_fsdev_set_opts   : set_opts JSON 입력 디코드 임시 구조체.
 * - rpc_fsdev_set_opts_decoders : JSON 필드 → C 멤버 매핑 디코더 테이블.
 */

#include "spdk/stdinc.h" /* [한국어] 표준 헤더 (size_t, NULL 등). */
#include "spdk/log.h"    /* [한국어] SPDK_ERRLOG/INFOLOG. */
#include "spdk/rpc.h"    /* [한국어] SPDK_RPC_REGISTER 매크로. */
#include "spdk/util.h"   /* [한국어] SPDK_COUNTOF, offsetof 래퍼 등. */
#include "spdk/fsdev.h"  /* [한국어] spdk_fsdev_opts, get/set_opts 공개 API. */

/*
 * [한국어]
 * rpc_fsdev_get_opts - "fsdev_get_opts" RPC 핸들러 — 현재 fsdev 코어 옵션을 JSON으로 응답.
 *
 * @request: JSON-RPC 요청 객체 (응답 빌더의 입력).
 * @params : JSON 인자 — 본 메서드는 인자를 받지 않으므로 NULL이어야 함.
 *
 * 인자가 들어오면 즉시 INVALID_PARAMS 에러로 응답한다. 정상 경로에서는
 * spdk_fsdev_get_opts()로 현재 g_opts를 복사 받아 두 필드(fsdev_io_pool_size,
 * fsdev_io_cache_size)를 JSON 객체로 직렬화해 응답한다. 실행 컨텍스트는 RPC 서버
 * thread (보통 마스터 코어).
 *
 * 호출 체인:
 *   JSON-RPC 서버 → [rpc_fsdev_get_opts] → spdk_fsdev_get_opts → 응답 JSON write
 */
static void
rpc_fsdev_get_opts(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w; /* [한국어] 응답 JSON 빌더 컨텍스트. begin_result 후 사용. */
	struct spdk_fsdev_opts opts = {}; /* [한국어] 코어에서 가져올 현재 옵션 사본 — 0 초기화. */
	int rc; /* [한국어] get_opts 결과 코드 임시 변수. */

	if (params) {
		/* [한국어] 본 RPC는 파라미터를 허용하지 않음 — 인자 들어왔으면 클라이언트 오류로 즉시 거부. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'fsdev_get_opts' requires no arguments");
		/* [한국어] 표준 JSON-RPC 에러 객체(-32602 invalid params)를 클라이언트에 전송 후 종료. */
		return;
	}

	rc = spdk_fsdev_get_opts(&opts, sizeof(opts));
	/* [한국어] 코어에 현재 옵션 사본 요청. sizeof(opts)를 함께 넘겨 ABI 호환(미래 필드 추가 대비). */
	if (rc) {
		/* [한국어] 0 외의 반환은 ABI 불일치/내부 오류 — 그 원인을 그대로 클라이언트에 노출. */
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "spdk_fsdev_get_opts failed with %d", rc);
		return;
	}

	w = spdk_jsonrpc_begin_result(request); /* [한국어] 성공 응답 시작 — JSON write 컨텍스트 획득. */
	spdk_json_write_object_begin(w); /* [한국어] '{' 시작 — 응답을 단일 JSON 객체로 감싼다. */
	spdk_json_write_named_uint32(w, "fsdev_io_pool_size", opts.fsdev_io_pool_size);
	/* [한국어] "fsdev_io_pool_size": <전체 fsdev IO mempool 크기> 항목 출력. */
	spdk_json_write_named_uint32(w, "fsdev_io_cache_size", opts.fsdev_io_cache_size);
	/* [한국어] "fsdev_io_cache_size": <per-thread cache 크기> 항목 출력. */
	spdk_json_write_object_end(w); /* [한국어] '}' 종결. */
	spdk_jsonrpc_end_result(request, w); /* [한국어] 응답 송신을 완료(서버가 클라이언트로 flush). */
}
SPDK_RPC_REGISTER("fsdev_get_opts", rpc_fsdev_get_opts, SPDK_RPC_RUNTIME)
/* [한국어] RPC 메서드 "fsdev_get_opts" 등록 — RUNTIME 단계(부팅 완료 후 호출 가능)에서만.
 * 매크로는 .init_array 등록자를 만들어 SPDK 시작 시 자동으로 핸들러 테이블에 추가. */

struct rpc_fsdev_set_opts {
	/* [한국어] "fsdev_set_opts" RPC 입력 JSON을 디코딩할 임시 컨테이너.
	 * 두 필드 모두 필수(required) — decoder 테이블에서 false(=optional 아님)로 표시. */

	uint32_t fsdev_io_pool_size;
	/* [한국어] 클라이언트가 요청한 새 IO mempool 전체 크기 (slot 개수).
	 * 너무 작으면 IO 고갈, 너무 크면 메모리 낭비. 코어가 검증/적용. */

	uint32_t fsdev_io_cache_size;
	/* [한국어] per-thread cache 크기 — mempool에서 thread-local 캐시로 미리 빼두는 슬롯 수.
	 * 보통 pool 크기보다 작아야 하며, 코어가 두 값을 함께 검증. */
};

static const struct spdk_json_object_decoder rpc_fsdev_set_opts_decoders[] = {
	/* [한국어] JSON object → struct rpc_fsdev_set_opts 디코딩 매핑.
	 * 각 항목: { "JSON 필드명", 멤버 오프셋, 디코더 함수, optional? }. false = 필수. */
	{"fsdev_io_pool_size", offsetof(struct rpc_fsdev_set_opts, fsdev_io_pool_size), spdk_json_decode_uint32, false},
	/* [한국어] "fsdev_io_pool_size" 값이 uint32로 디코드되어 fsdev_io_pool_size에 저장. */
	{"fsdev_io_cache_size", offsetof(struct rpc_fsdev_set_opts, fsdev_io_cache_size), spdk_json_decode_uint32, false},
	/* [한국어] "fsdev_io_cache_size" 동일. */
};

/*
 * [한국어]
 * rpc_fsdev_set_opts - "fsdev_set_opts" RPC 핸들러 — fsdev 코어 옵션 갱신.
 *
 * @request: JSON-RPC 요청.
 * @params : { "fsdev_io_pool_size": N, "fsdev_io_cache_size": M } 형태의 JSON.
 *
 * 1) 입력 디코드 → 실패 시 INVALID_PARAMS 응답.
 * 2) 현재 옵션 가져와 사본 만들고 사용자 입력 두 필드만 덮어씀(미래에 추가될 다른 필드 보존).
 * 3) spdk_fsdev_set_opts 호출 — 코어가 검증 후 g_opts에 반영.
 * 4) 성공 시 true bool 응답.
 *
 * 호출 체인:
 *   JSON-RPC 서버 → [rpc_fsdev_set_opts] → spdk_fsdev_set_opts
 */
static void
rpc_fsdev_set_opts(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_fsdev_set_opts req = {}; /* [한국어] 클라이언트 입력 디코드 결과 — 0 초기화. */
	int rc; /* [한국어] set/get_opts 결과 임시. */
	struct spdk_fsdev_opts opts = {}; /* [한국어] 적용 직전의 옵션 사본. 미래 필드 보존을 위해 get으로 채운 뒤 덮어쓴다. */

	if (spdk_json_decode_object(params, rpc_fsdev_set_opts_decoders,
				    SPDK_COUNTOF(rpc_fsdev_set_opts_decoders),
				    &req)) {
		/* [한국어] 디코드 실패 — 필수 필드 누락/타입 불일치 등. 로그 + 표준 INVALID_PARAMS. */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = spdk_fsdev_get_opts(&opts, sizeof(opts));
	/* [한국어] 현재 옵션 가져와 베이스라인으로 사용 — 미래 추가될 필드를 그대로 유지. */
	if (rc) {
		/* [한국어] 코어 측 ABI/내부 오류 시 즉시 실패 응답. */
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "spdk_fsdev_get_opts failed with %d", rc);
		return;
	}

	opts.fsdev_io_pool_size = req.fsdev_io_pool_size;
	/* [한국어] 사용자 지정 풀 크기로 덮어씀. */
	opts.fsdev_io_cache_size = req.fsdev_io_cache_size;
	/* [한국어] 사용자 지정 캐시 크기로 덮어씀. */

	rc = spdk_fsdev_set_opts(&opts);
	/* [한국어] 코어에 옵션 적용 요청 — 코어가 값 검증 + 부팅 완료 후 변경 허용 여부 등을 체크. */
	if (rc) {
		/* [한국어] 검증 실패/타이밍 부적절 등 — 그 원인 코드를 클라이언트에 그대로 전달. */
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "spdk_fsdev_set_opts failed with %d", rc);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	/* [한국어] 성공 시 true 단일값 JSON 응답. SPDK RPC의 관용적 성공 신호. */
}
SPDK_RPC_REGISTER("fsdev_set_opts", rpc_fsdev_set_opts, SPDK_RPC_RUNTIME)
/* [한국어] "fsdev_set_opts" RPC 등록 — 마찬가지로 RUNTIME 단계에서만 호출 가능. */
