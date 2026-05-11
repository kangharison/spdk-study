/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK accel 서브시스템 JSON-RPC 핸들러 (accel_rpc.c)
 *
 * === 파일의 역할 ===
 * accel(가속) 서브시스템의 모든 JSON-RPC 메서드 핸들러를 정의한다. 외부
 * 클라이언트(rpc.py, REST 게이트웨이 등)가 SPDK 데몬에 보내는
 * `accel_get_opc_assignments`, `accel_get_module_info`, `accel_assign_opc`,
 * `accel_crypto_key_create/get/destroy`, `accel_set_driver`,
 * `accel_set_options`, `accel_get_stats` 같은 메서드를 처리하는
 * 콜백을 SPDK_RPC_REGISTER로 등록한다. 핵심 로직은 accel.c가 제공하는
 * 공개 API를 호출하고 응답을 JSON으로 직렬화하는 얇은 레이어이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [클라이언트(rpc.py)] -- JSON-RPC over Unix socket --> [SPDK rpc 서버 (lib/rpc)]
 *      → 메서드 디스패치 → [본 파일의 핸들러] → spdk_accel_*() / accel_get_stats()
 *      → 응답 빌드 → spdk_jsonrpc_end_result로 클라이언트에 회신.
 * 실행 컨텍스트: SPDK rpc 서버는 보통 메인 reactor 스레드에서 단일 스레드로 동작.
 * 따라서 본 파일의 모든 핸들러는 단일 스레드 호출이 보장된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `accel_internal.h`(통계 구조 등 비공개), `spdk/accel_module.h`(모듈 인터페이스),
 *   `spdk/rpc.h`(RPC 등록 매크로 SPDK_RPC_REGISTER), `spdk/util.h`,
 *   `spdk/event.h`, `spdk/string.h`, `spdk/env.h`.
 * - 본 파일에 의존: 외부 RPC 클라이언트만 의존. 내부 코드 의존성 없음.
 * - 데이터 흐름: 클라이언트 JSON 요청 → params 디코드(rpc_*_decoders) →
 *   accel API 호출 → 결과를 JSON writer로 직렬화 → 회신.
 * - 공유 자료구조: accel_stats(통계), spdk_accel_crypto_key(암호 키),
 *   g_modules_opc_override(opcode→모듈 assignment).
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_accel_get_opc_assignments: 각 opcode가 어느 모듈에 할당됐는지 조회.
 * - rpc_accel_get_module_info: 등록된 모든 모듈과 그 모듈이 지원하는 op 목록.
 * - rpc_accel_assign_opc: 특정 opcode를 특정 모듈에 강제 할당(시작 전에만).
 * - rpc_accel_crypto_key_create/destroy/get: 암호 키 lifecycle 관리.
 * - rpc_accel_set_driver: 글로벌 accel driver(시퀀스 실행기) 선택.
 * - rpc_accel_set_options: cache/풀 크기 등 런타임 옵션 변경(시작 전에만).
 * - rpc_accel_get_stats / rpc_accel_get_stats_done: 비동기 통계 조회 + 완료 콜백.
 */

/* [한국어] 본 파일이 사용하는 비공개 자료구조 — accel_stats, module_info,
 *  _accel_for_each_module 등. 외부 헤더에는 노출되지 않는다. */
#include "accel_internal.h"
/* [한국어] accel 모듈 인터페이스(spdk_accel_module_if 등) — 모듈 이름 조회 등 사용. */
#include "spdk/accel_module.h"

/* [한국어] SPDK_RPC_REGISTER 매크로와 spdk_jsonrpc_* API. */
#include "spdk/rpc.h"
/* [한국어] SPDK_COUNTOF/offsetof 등 유틸 매크로. */
#include "spdk/util.h"
/* [한국어] SPDK_RPC_RUNTIME / SPDK_RPC_STARTUP 상수와 이벤트/앱 관련 헬퍼. */
#include "spdk/event.h"
/* [한국어] 표준 인클루드(string.h, stdint.h 등 모음). */
#include "spdk/stdinc.h"
/* [한국어] spdk_strerror / spdk_memset_s — 안전한 키 워이프에 spdk_memset_s 사용. */
#include "spdk/string.h"
/* [한국어] SPDK env(DPDK 추상). 본 파일에서는 직접 사용은 적으나 일관성 위해 포함. */
#include "spdk/env.h"

/*
 * [한국어]
 * rpc_accel_get_opc_assignments - 각 opcode → 담당 모듈 이름 매핑 조회.
 *
 * @request: JSON-RPC 요청 핸들. 응답은 spdk_jsonrpc_*로 보낸다.
 * @params: JSON 파라미터 (이 메서드는 파라미터 없음을 요구).
 *
 * 응답 포맷: { "copy": "software", "fill": "idxd", ... } 형태의 단일 객체.
 *  각 키는 opcode 이름, 값은 현재 그 opcode를 담당하는 모듈 이름.
 *
 * 호출 컨텍스트: rpc 서버 스레드(메인 reactor). SPDK_RPC_RUNTIME 단계에 등록되어
 *  framework 시작 후 언제든 호출 가능.
 */
static void
rpc_accel_get_opc_assignments(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;        /* [한국어] JSON 응답 빌더 핸들. */
	enum spdk_accel_opcode opcode;        /* [한국어] 모든 opcode를 순회하기 위한 변수. */
	const char *name, *module_name = NULL;
	/* [한국어] 각각 opcode 문자열 이름과 담당 모듈 이름 (out 파라미터). */
	int rc;

	if (params != NULL) {
		/* [한국어] 본 메서드는 파라미터를 받지 않으므로 잘못된 사용에 대해 즉시 에러 회신. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "accel_get_opc_assignments requires no parameters");
		return;
	}

	/* [한국어] 응답 빌더 시작 — 이 시점부터 spdk_json_write_*로 결과 객체를 채운다. */
	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);                    /* [한국어] '{' 출력. */
	for (opcode = 0; opcode < SPDK_ACCEL_OPC_LAST; opcode++) {
		/* [한국어] opcode → 사람이 읽는 문자열 이름 ("copy", "fill" 등). */
		name = spdk_accel_get_opcode_name(opcode);
		if (name != NULL) {
			/* [한국어] opcode → 담당 모듈 이름 조회. 미할당 상태면 -ENOENT. */
			rc = spdk_accel_get_opc_module_name(opcode, &module_name);
			if (rc == 0) {
				/* [한국어] "name": "module_name" 키-값 쌍 출력. */
				spdk_json_write_named_string(w, name, module_name);
			} else {
				/* This isn't fatal but throw an informational message if we
				 * can't get an module name right now */
				/* [한국어] 모듈이 아직 할당되지 않은 opcode — 응답에서는 누락하고 알림 로그만. */
				SPDK_NOTICELOG("FYI error (%d) getting module name.\n", rc);
			}
		} else {
			/* this should never happen */
			/* [한국어] 모든 opcode에 이름이 정의되어 있어야 정상. 정의 누락 시 즉시 abort. */
			SPDK_ERRLOG("Invalid opcode (%d)).\n", opcode);
			assert(0);
		}
	}
	spdk_json_write_object_end(w);                      /* [한국어] '}' 출력. */

	/* [한국어] 응답 빌드 종료 + 클라이언트로 송신. */
	spdk_jsonrpc_end_result(request, w);
}
/* [한국어] RPC 등록 매크로 — JSON-RPC 메서드명 "accel_get_opc_assignments"를
 *  rpc_accel_get_opc_assignments에 매핑. SPDK_RPC_RUNTIME은 SPDK 시작 후 언제든 호출 가능. */
SPDK_RPC_REGISTER("accel_get_opc_assignments", rpc_accel_get_opc_assignments, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_dump_module_info - 한 모듈의 (이름, 지원 op 목록) 정보를 JSON 객체로 출력.
 *
 * @info: _accel_for_each_module이 채워준 모듈 정보. info->w가 활성 JSON writer.
 *
 * 호출 컨텍스트: rpc_accel_get_module_info → _accel_for_each_module → [본 함수].
 */
static void
rpc_dump_module_info(struct module_info *info)
{
	struct spdk_json_write_ctx *w = info->w;            /* [한국어] writer alias. */
	const char *name;
	uint32_t i;

	spdk_json_write_object_begin(w);                    /* [한국어] 한 모듈 객체 시작. */

	spdk_json_write_named_string(w, "module", info->name);  /* [한국어] "module": 모듈명. */
	/* [한국어] "supported ops": [ ... ] 배열 시작. */
	spdk_json_write_named_array_begin(w, "supported ops");

	for (i = 0; i < info->num_ops; i++) {
		/* [한국어] 각 지원 opcode를 문자열로 변환해 배열에 추가. */
		name = spdk_accel_get_opcode_name(info->ops[i]);
		if (name != NULL) {
			spdk_json_write_string(w, name);
		} else {
			/* this should never happen */
			/* [한국어] 알 수 없는 opcode — 사양 위반. */
			SPDK_ERRLOG("Invalid opcode (%d)).\n", info->ops[i]);
			assert(0);
		}
	}

	spdk_json_write_array_end(w);                       /* [한국어] 배열 종료 ']'. */
	spdk_json_write_object_end(w);                      /* [한국어] 모듈 객체 종료 '}'. */
}

/*
 * [한국어]
 * rpc_accel_get_module_info - 등록된 모든 accel 모듈과 각각이 지원하는 op 목록을 반환.
 *
 * @request: RPC 요청 핸들.
 * @params: 파라미터 없음.
 *
 * 응답: [ { "module": "...", "supported ops": [...] }, ... ]
 *
 * 호출 체인: 클라이언트 → [rpc_accel_get_module_info] → _accel_for_each_module → rpc_dump_module_info
 */
static void
rpc_accel_get_module_info(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct module_info info;

	if (params != NULL) {
		/* [한국어] 파라미터 미지원 — 잘못 호출 시 즉시 에러. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "accel_get_module_info requires no parameters");
		return;
	}

	/* [한국어] writer 시작 — info.w를 통해 콜백에 전달. */
	info.w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(info.w);                /* [한국어] 외곽 '[' (모듈 목록). */

	/* [한국어] 모든 모듈을 순회하며 각 모듈마다 rpc_dump_module_info 호출. */
	_accel_for_each_module(&info, rpc_dump_module_info);

	spdk_json_write_array_end(info.w);                  /* [한국어] ']' 종료. */
	spdk_jsonrpc_end_result(request, info.w);
}
SPDK_RPC_REGISTER("accel_get_module_info", rpc_accel_get_module_info, SPDK_RPC_RUNTIME)

/*
 * [한국어] struct rpc_accel_assign_opc — `accel_assign_opc` RPC의 입력 파라미터.
 *  클라이언트는 opname(문자열), module(문자열) 두 키를 보내야 한다.
 */
struct rpc_accel_assign_opc {
	char *opname;
	/* [한국어] 할당하려는 opcode의 사람이 읽는 이름 (예: "copy", "fill").
	 * 설정자: spdk_json_decode_object가 strdup으로 채움. 해제: free_accel_assign_opc. */
	char *module;
	/* [한국어] 할당 대상 모듈 이름 (예: "software", "idxd"). 설정자/해제: 위와 동일. */
};

/* [한국어] 위 구조체의 JSON 디코딩 스키마. 각 항목은 {키, 오프셋, 디코더}로 구성.
 *  세 번째 인자(없으면 false)가 true면 옵셔널 필드. */
static const struct spdk_json_object_decoder rpc_accel_assign_opc_decoders[] = {
	{"opname", offsetof(struct rpc_accel_assign_opc, opname), spdk_json_decode_string},
	{"module", offsetof(struct rpc_accel_assign_opc, module), spdk_json_decode_string},
};

/*
 * [한국어]
 * free_accel_assign_opc - 디코더가 strdup으로 할당한 문자열 메모리 해제.
 *
 * 호출 컨텍스트: 핸들러 종료 시점(성공/실패 모두 cleanup label에서 호출).
 */
static void
free_accel_assign_opc(struct rpc_accel_assign_opc *r)
{
	free(r->opname);
	free(r->module);
}

/*
 * [한국어]
 * rpc_accel_assign_opc - 특정 opcode를 특정 모듈로 강제 할당.
 *
 * @request: RPC 요청.
 * @params: { "opname": "...", "module": "..." }.
 *
 * 시작 단계 RPC(SPDK_RPC_STARTUP) — 일반적으로 framework 초기화 전에만 호출 가능.
 *  framework 시작 후에는 spdk_accel_assign_opc가 -EINVAL을 돌려준다.
 *
 * 호출 체인: 클라이언트 → [rpc_accel_assign_opc] → spdk_accel_assign_opc
 */
static void
rpc_accel_assign_opc(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_accel_assign_opc req = {};        /* [한국어] 디코딩 결과를 받을 버퍼. */
	const char *opcode_str;
	enum spdk_accel_opcode opcode;
	bool found = false;
	int rc;

	/* [한국어] params를 위 스키마로 디코드. 키 누락이나 타입 미스매치면 음수 반환. */
	if (spdk_json_decode_object(params, rpc_accel_assign_opc_decoders,
				    SPDK_COUNTOF(rpc_accel_assign_opc_decoders),
				    &req)) {
		SPDK_DEBUGLOG(accel, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] 사용자가 보낸 opname을 enum으로 역매핑. 모든 opcode를 순회하며 strcmp. */
	for (opcode = 0; opcode < SPDK_ACCEL_OPC_LAST; opcode++) {
		opcode_str = spdk_accel_get_opcode_name(opcode);
		assert(opcode_str != NULL);                /* [한국어] 모든 opcode에 이름이 있어야 정상. */
		if (strcmp(opcode_str, req.opname) == 0) {
			found = true;
			break;
		}
	}

	if (found == false) {
		/* [한국어] 알 수 없는 opname — 잘못된 입력. */
		SPDK_DEBUGLOG(accel, "Invalid operation name\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] accel.c의 공개 API 호출 — 내부 g_modules_opc_override 배열에 strdup해 저장. */
	rc = spdk_accel_assign_opc(opcode, req.module);
	if (rc) {
		/* [한국어] 보통 framework 이미 시작됨(-EINVAL) 또는 OOM. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "error assigning opcode");
		goto cleanup;
	}

	SPDK_NOTICELOG("Operation %s will be assigned to module %s\n", req.opname, req.module);
	/* [한국어] 성공 응답: 단일 boolean true. */
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	/* [한국어] 디코딩 strdup 해제 — 모든 경로에서 도달. */
	free_accel_assign_opc(&req);

}
/* [한국어] STARTUP 단계 등록 — framework가 본격 동작하기 전에만 적용 의미가 있음. */
SPDK_RPC_REGISTER("accel_assign_opc", rpc_accel_assign_opc, SPDK_RPC_STARTUP)

/*
 * [한국어] struct rpc_accel_crypto_key_create — `accel_crypto_key_create` 입력.
 *  내부에 spdk_accel_crypto_key_create_param을 wrap해서 디코더가 직접 채우도록 한다.
 *  param 필드들: cipher(알고리즘 문자열, 예 AES_XTS), hex_key/hex_key2(원시 키 hex),
 *  tweak_mode(XTS tweak 모드), key_name(고유 키 식별자).
 */
struct rpc_accel_crypto_key_create {
	struct spdk_accel_crypto_key_create_param param;
	/* [한국어] 디코더가 채울 키 생성 파라미터. spdk_accel_crypto_key_create의 입력. */
};

/* [한국어] JSON 디코딩 스키마.
 *  - cipher/key/name: 필수 (네 번째 인자 없으면 default false=필수).
 *  - key2/tweak_mode: 옵셔널 (네 번째 인자 true). */
static const struct spdk_json_object_decoder rpc_accel_crypto_key_create_decoders[] = {
	{"cipher", offsetof(struct rpc_accel_crypto_key_create, param.cipher), spdk_json_decode_string},
	{"key", offsetof(struct rpc_accel_crypto_key_create, param.hex_key),   spdk_json_decode_string},
	{"key2", offsetof(struct rpc_accel_crypto_key_create, param.hex_key2), spdk_json_decode_string, true},
	{"tweak_mode", offsetof(struct rpc_accel_crypto_key_create, param.tweak_mode), spdk_json_decode_string, true},
	{"name", offsetof(struct rpc_accel_crypto_key_create, param.key_name), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_accel_crypto_key_create - 새 암호 키 객체를 생성하고 keyring에 등록.
 *
 * @request: RPC 요청.
 * @params: 위 스키마.
 *
 * 보안: 핸들러 종료 시 hex_key / hex_key2 메모리는 spdk_memset_s(non-optimizable
 *  memset)로 0 클리어 후 free하여 키 잔류를 막는다.
 *
 * 호출 체인: 클라이언트 → [본 함수] → spdk_accel_crypto_key_create
 */
static void
rpc_accel_crypto_key_create(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_accel_crypto_key_create req = {};       /* [한국어] 디코딩 결과 컨테이너. */
	size_t key_size;
	int rc;

	/* [한국어] params 디코드. 필수 필드 누락 시 즉시 에러. */
	if (spdk_json_decode_object(params, rpc_accel_crypto_key_create_decoders,
				    SPDK_COUNTOF(rpc_accel_crypto_key_create_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] 실제 키 객체 생성 — 모듈에 위임(보통 sw 모듈 또는 mlx5 등). */
	rc = spdk_accel_crypto_key_create(&req.param);
	if (rc) {
		/* [한국어] 키 생성 실패 — 잘못된 키 길이/cipher 미지원/이미 같은 이름 등. */
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "failed to create DEK, rc %d", rc);
	} else {
		/* [한국어] 성공: true 반환. */
		spdk_jsonrpc_send_bool_response(request, true);
	}

cleanup:
	free(req.param.cipher);                            /* [한국어] cipher 문자열 해제. */
	if (req.param.hex_key) {
		/* [한국어] 키 길이만큼 strnlen으로 측정 후 0 클리어 — 잔류 방지. */
		key_size = strnlen(req.param.hex_key, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
		/* [한국어] spdk_memset_s는 컴파일러 최적화에 의해 제거되지 않도록 보장된 wipe. */
		spdk_memset_s(req.param.hex_key, key_size, 0, key_size);
		free(req.param.hex_key);
	}
	if (req.param.hex_key2) {
		/* [한국어] 두 번째 키(있으면) 동일 처리. */
		key_size = strnlen(req.param.hex_key2, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
		spdk_memset_s(req.param.hex_key2, key_size, 0, key_size);
		free(req.param.hex_key2);
	}
	free(req.param.tweak_mode);                        /* [한국어] tweak_mode 문자열 해제. */
	free(req.param.key_name);                          /* [한국어] 키 이름 해제. */
}
SPDK_RPC_REGISTER("accel_crypto_key_create", rpc_accel_crypto_key_create, SPDK_RPC_RUNTIME)

/*
 * [한국어] struct rpc_accel_crypto_keys_get_ctx — accel_crypto_keys_get 입력.
 *  단일 옵셔널 필드 key_name(있으면 해당 키만 조회, 없으면 전체 keyring).
 */
struct rpc_accel_crypto_keys_get_ctx {
	char *key_name;
	/* [한국어] 옵셔널 키 이름. NULL이면 모든 키를 덤프. */
};

/* [한국어] keys_get 디코딩 스키마 — key_name은 옵셔널(true). */
static const struct spdk_json_object_decoder rpc_accel_crypto_keys_get_decoders[] = {
	{"key_name", offsetof(struct rpc_accel_crypto_keys_get_ctx, key_name), spdk_json_decode_string, true},
};

/*
 * [한국어]
 * rpc_accel_crypto_keys_get - 단일 키 또는 전체 keyring 정보를 JSON 배열로 반환.
 *
 * 호출 체인: 클라이언트 → [본 함수] → _accel_crypto_key(s)_dump_param
 */
static void
rpc_accel_crypto_keys_get(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_accel_crypto_keys_get_ctx req = {};
	struct spdk_accel_crypto_key *key = NULL;
	struct spdk_json_write_ctx *w;

	/* [한국어] params가 있으면 디코드. 없으면(=전체 조회) skip. */
	if (params && spdk_json_decode_object(params, rpc_accel_crypto_keys_get_decoders,
					      SPDK_COUNTOF(rpc_accel_crypto_keys_get_decoders),
					      &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		free(req.key_name);
		return;
	}

	if (req.key_name) {
		/* [한국어] 특정 키 조회 — 글로벌 keyring에서 이름으로 찾음. */
		key = spdk_accel_crypto_key_get(req.key_name);
		free(req.key_name);
		if (!key) {
			/* [한국어] 이름이 존재하지 않음. */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "key was not found");
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);                    /* [한국어] '[' (키 메타 객체 배열). */

	if (key) {
		/* [한국어] 단일 키 — 1개 객체만 덤프. */
		_accel_crypto_key_dump_param(w, key);
	} else {
		/* [한국어] 전체 — keyring 순회 덤프. */
		_accel_crypto_keys_dump_param(w);
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("accel_crypto_keys_get", rpc_accel_crypto_keys_get, SPDK_RPC_RUNTIME)

/* [한국어] destroy 디코딩 스키마 — key_name은 필수. */
static const struct spdk_json_object_decoder rpc_accel_crypto_key_destroy_decoders[] = {
	{"key_name", offsetof(struct rpc_accel_crypto_keys_get_ctx, key_name), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_accel_crypto_key_destroy - 이름으로 지정된 키를 keyring에서 제거하고 자원 해제.
 */
static void
rpc_accel_crypto_key_destroy(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_accel_crypto_keys_get_ctx req = {};
	struct spdk_accel_crypto_key *key = NULL;
	int rc;

	/* [한국어] 필수 key_name 디코드. 누락 시 에러. */
	if (spdk_json_decode_object(params, rpc_accel_crypto_key_destroy_decoders,
				    SPDK_COUNTOF(rpc_accel_crypto_key_destroy_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		free(req.key_name);
		return;
	}

	/* [한국어] keyring에서 키 객체를 찾는다. 못 찾으면 INVALID_PARAMS. */
	key = spdk_accel_crypto_key_get(req.key_name);
	if (!key) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "No key object found");
		free(req.key_name);
		return;

	}
	/* [한국어] 키 객체 파괴 — 모듈 deinit 호출 후 keyring에서 제거 + 자원 해제. */
	rc = spdk_accel_crypto_key_destroy(key);
	if (rc) {
		/* [한국어] 모듈 측에서 거절하거나 참조 카운트 문제 등. */
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to destroy key, rc %d", rc);
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}

	free(req.key_name);
}
SPDK_RPC_REGISTER("accel_crypto_key_destroy", rpc_accel_crypto_key_destroy, SPDK_RPC_RUNTIME)

/*
 * [한국어] struct rpc_accel_set_driver — `accel_set_driver` RPC의 입력 파라미터.
 *
 *  accel "driver"는 시퀀스(체이닝된 task 묶음) 실행을 담당하는 글로벌 컴포넌트로,
 *  특정 HW(예: dpdk_cryptodev, mlx5)에서 시퀀스를 한 번에 처리할 때 활성화한다.
 *  드라이버를 지정하지 않으면 코어가 task 단위로 모듈에 분배하는 기본 경로가 동작한다.
 */
struct rpc_accel_set_driver {
	char *name;
	/* [한국어] 활성화할 driver의 이름 문자열 (예: "mlx5", "dpdk_cryptodev").
	 * 설정자: spdk_json_decode_string이 strdup으로 채움.
	 * 읽는 자: spdk_accel_set_driver()가 등록된 driver 목록에서 이름 매칭에 사용.
	 * 값 범위: 옵셔널. NULL/빈 문자열이면 "no driver"로 해석되어 기본 경로로 폴백.
	 * 동기화: STARTUP 단계 단일 스레드 호출이므로 별도 락 불필요. 해제: free_rpc_accel_set_driver. */
};

/* [한국어] set_driver 디코딩 스키마 — name은 옵셔널(true).
 *  파라미터를 빈 객체 {}로 호출하면 driver를 disable한다는 의미가 된다. */
static const struct spdk_json_object_decoder rpc_accel_set_driver_decoders[] = {
	{"name", offsetof(struct rpc_accel_set_driver, name), spdk_json_decode_string, true},
};

/*
 * [한국어]
 * free_rpc_accel_set_driver - 디코더가 strdup으로 할당한 name 문자열을 해제.
 *
 * @r: 해제 대상 입력 구조체.
 *
 * 호출 컨텍스트: rpc_accel_set_driver의 cleanup label에서 모든 경로가 도달.
 */
static void
free_rpc_accel_set_driver(struct rpc_accel_set_driver *r)
{
	free(r->name);                                     /* [한국어] strdup된 driver 이름 해제. */
}

/*
 * [한국어]
 * rpc_accel_set_driver - 글로벌 accel 시퀀스 driver를 선택.
 *
 * @request: RPC 요청 핸들.
 * @params: { "name": "driver_name" }  (옵셔널, 미지정 시 driver 비활성화).
 *
 * driver는 시퀀스 단위 실행을 가속할 HW를 지정한다. 미지정 시 accel 코어가
 *  task 단위로 모듈에 분배하는 기본 경로(submit_tasks)가 동작한다.
 *  STARTUP 단계 RPC — framework가 본격 동작하기 전에만 의미가 있음.
 *
 * 호출 체인: 클라이언트 → [본 함수] → spdk_accel_set_driver
 */
static void
rpc_accel_set_driver(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_accel_set_driver req = {};              /* [한국어] 디코딩 결과 컨테이너 — name 초기 NULL. */
	int rc;

	/* [한국어] params 디코드. 키 누락 자체는 옵셔널이므로 정상이지만, 타입 미스매치 등은 에러. */
	if (spdk_json_decode_object(params, rpc_accel_set_driver_decoders,
				    SPDK_COUNTOF(rpc_accel_set_driver_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		return;                                    /* [한국어] strdup가 실패한 단계라 free 불필요. */
	}

	/* [한국어] 코어에 driver 활성화를 위임. 등록되지 않은 이름이면 -ENODEV 등 음수 반환. */
	rc = spdk_accel_set_driver(req.name);
	if (rc != 0) {
		/* [한국어] -rc로 errno 변환 후 사람이 읽는 메시지를 함께 회신. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;                              /* [한국어] strdup된 name을 반드시 해제하기 위해 cleanup으로. */
	}

	/* [한국어] 성공 로그 — name이 NULL/빈 문자열이면 "none"으로 표기. */
	SPDK_NOTICELOG("Using accel driver: %s\n", req.name && req.name[0] ? req.name : "none");
	spdk_jsonrpc_send_bool_response(request, true);    /* [한국어] 성공: 단일 boolean true 회신. */
cleanup:
	free_rpc_accel_set_driver(&req);                   /* [한국어] strdup된 name 해제 — 모든 경로에서 호출. */
}
/* [한국어] STARTUP 단계 등록 — driver 선택은 framework 가동 전에만 적용된다. */
SPDK_RPC_REGISTER("accel_set_driver", rpc_accel_set_driver, SPDK_RPC_STARTUP)

/*
 * [한국어] struct rpc_accel_opts — `accel_set_options` RPC의 입력 파라미터.
 *
 *  실제 코어가 사용하는 spdk_accel_opts는 packed 구조라서 디코더 콜백이 사용하는
 *  포인터가 misaligned가 될 수 있다(undefined behavior). 그래서 RPC 전용으로
 *  unpacked 사본을 두고, 디코드 후 packed 원본으로 복사한다.
 */
struct rpc_accel_opts {
	uint32_t	small_cache_size;
	/* [한국어] iobuf 작은 버퍼 풀의 per-channel 캐시 크기.
	 * 설정자: 디코더가 채움. 읽는 자: spdk_accel_set_opts → 코어 g_opts에 반영.
	 * 값 범위: 0~UINT32_MAX. 0이면 코어 기본값 유지(미적용)이 아니라 0으로 강제됨에 주의.
	 * 동기화: STARTUP 단계 단일 스레드 호출. */

	uint32_t	large_cache_size;
	/* [한국어] iobuf 큰 버퍼 풀의 per-channel 캐시 크기. 의미/동기화 위와 동일. */

	uint32_t	task_count;
	/* [한국어] 채널당 task 풀 크기 (`spdk_accel_task` 객체의 사전 할당 수).
	 * 설정자: 디코더. 읽는 자: 코어가 mempool 생성 시 사용.
	 * 값 범위: 0보다 커야 의미. 너무 작으면 백프레셔(retry.task) 빈발. */

	uint32_t	sequence_count;
	/* [한국어] 채널당 sequence 풀 크기 (`accel_sequence` 객체).
	 * 너무 작으면 retry.sequence 카운터가 늘고 시퀀스 시작 실패가 잦아진다. */

	uint32_t	buf_count;
	/* [한국어] 채널당 accel_buffer 디스크립터 풀 크기.
	 * iobuf와는 별개로 sequence 내부에서 가상→실 버퍼 매핑을 추적하는 메타. */
};

/* [한국어] set_options 디코딩 스키마 — 모든 항목 옵셔널(true).
 *  지정되지 않은 필드는 코어의 현재값(spdk_accel_get_opts로 미리 읽어둠)이 유지된다. */
static const struct spdk_json_object_decoder rpc_accel_set_options_decoders[] = {
	{"small_cache_size", offsetof(struct rpc_accel_opts, small_cache_size), spdk_json_decode_uint32, true},
	{"large_cache_size", offsetof(struct rpc_accel_opts, large_cache_size), spdk_json_decode_uint32, true},
	{"task_count", offsetof(struct rpc_accel_opts, task_count), spdk_json_decode_uint32, true},
	{"sequence_count", offsetof(struct rpc_accel_opts, sequence_count), spdk_json_decode_uint32, true},
	{"buf_count", offsetof(struct rpc_accel_opts, buf_count), spdk_json_decode_uint32, true},
};

/*
 * [한국어]
 * rpc_accel_set_options - accel 코어의 풀/캐시 크기 옵션을 변경.
 *
 * @request: RPC 요청.
 * @params: { "small_cache_size": N, "large_cache_size": N, "task_count": N,
 *            "sequence_count": N, "buf_count": N }  (모두 옵셔널).
 *
 * STARTUP 단계 RPC — framework가 시작되어 mempool이 만들어지면 더 이상 변경 불가.
 *  현재값을 먼저 spdk_accel_get_opts로 읽어 RPC 전용 unpacked 사본을 채운 뒤,
 *  디코더가 클라이언트 지정 필드만 덮어쓰고, 다시 packed 원본으로 복사해 set_opts 호출.
 *
 * 호출 체인: 클라이언트 → [본 함수] → spdk_accel_set_opts
 */
static void
rpc_accel_set_options(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_accel_opts rpc_opts;                    /* [한국어] 디코더가 채울 unpacked 사본. */
	struct spdk_accel_opts opts;                       /* [한국어] 실제 코어가 사용하는 packed 원본. */
	int rc;

	/* We can't pass spdk_accel_opts directly to spdk_json_decode_object(), because that
	 * structure is packed, leading to undefined behavior due to misaligned pointer access */
	/* [한국어] packed 구조체의 필드 주소는 misaligned일 수 있어 디코더의 포인터 캐스트가 UB를 유발한다.
	 *  따라서 unpacked rpc_opts에 일단 현재값을 복사해 두고, 디코드 → 다시 opts로 옮긴다. */
	spdk_accel_get_opts(&opts, sizeof(opts));          /* [한국어] 코어로부터 현재 옵션 읽기. */
	rpc_opts.small_cache_size = opts.small_cache_size; /* [한국어] 디폴트로 현재값을 복사 — 미지정 필드 보존. */
	rpc_opts.large_cache_size = opts.large_cache_size; /* [한국어] 동일 — 큰 캐시 크기 디폴트. */
	rpc_opts.task_count = opts.task_count;             /* [한국어] 동일 — task 풀 크기 디폴트. */
	rpc_opts.sequence_count = opts.sequence_count;     /* [한국어] 동일 — sequence 풀 크기 디폴트. */
	rpc_opts.buf_count = opts.buf_count;               /* [한국어] 동일 — buf 디스크립터 풀 크기 디폴트. */

	/* [한국어] 클라이언트가 보낸 필드만 rpc_opts 위에 덮어씀. 모든 필드는 옵셔널. */
	if (spdk_json_decode_object(params, rpc_accel_set_options_decoders,
				    SPDK_COUNTOF(rpc_accel_set_options_decoders), &rpc_opts)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		return;                                    /* [한국어] 디코드 실패 — opts 변경 없이 종료. */
	}

	/* [한국어] unpacked → packed 복사. 이제 클라이언트 지정값 + 미지정 디폴트가 합쳐져 있다. */
	opts.small_cache_size = rpc_opts.small_cache_size;
	opts.large_cache_size = rpc_opts.large_cache_size;
	opts.task_count = rpc_opts.task_count;
	opts.sequence_count = rpc_opts.sequence_count;
	opts.buf_count = rpc_opts.buf_count;

	/* [한국어] 코어에 옵션 반영. framework 시작 후라면 -EINVAL을 돌려준다. */
	rc = spdk_accel_set_opts(&opts);
	if (rc != 0) {
		/* [한국어] 음수 errno를 양수 코드로 변환해 메시지와 함께 회신. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);    /* [한국어] 성공: true. */
}
/* [한국어] STARTUP 단계 등록 — 풀/캐시는 framework 시작 후 변경 불가. */
SPDK_RPC_REGISTER("accel_set_options", rpc_accel_set_options, SPDK_RPC_STARTUP)

/*
 * [한국어]
 * rpc_accel_get_stats_done - accel_get_stats의 비동기 완료 콜백.
 *                            모든 reactor의 채널 통계가 합산된 결과를 JSON으로 직렬화.
 *
 * @stats: accel_get_stats가 spdk_for_each_channel을 통해 모든 채널의 통계를 누적한 결과.
 *         포인터는 콜백 종료 후 무효 — 본 함수 내부에서만 안전하게 접근 가능.
 * @cb_arg: 호출자가 넘긴 컨텍스트 — 여기서는 RPC 요청 핸들 그대로.
 *
 * 응답 포맷:
 *   {
 *     "sequence_executed": N, "sequence_failed": N,
 *     "sequence_outstanding": N, "task_outstanding": N,
 *     "operations": [ { "opcode": "...", "module_name": "...",
 *                       "executed": N, "failed": N, "num_bytes": N }, ... ],
 *     "retry_task": N, "retry_sequence": N, "retry_iobuf": N, "retry_bufdesc": N
 *   }
 *
 * 실행 컨텍스트: spdk_for_each_channel의 done 콜백 — 호출 요청을 시작한 스레드(메인 reactor).
 * 호출 체인: rpc_accel_get_stats → accel_get_stats → (모든 채널 순회) → [본 함수]
 */
static void
rpc_accel_get_stats_done(struct accel_stats *stats, void *cb_arg)
{
	struct spdk_jsonrpc_request *request = cb_arg;     /* [한국어] cb_arg를 RPC 요청 핸들로 복원. */
	struct spdk_json_write_ctx *w;                     /* [한국어] JSON 응답 빌더. */
	const char *module_name;                           /* [한국어] opcode → 담당 모듈 이름 임시 저장. */
	int i, rc;

	w = spdk_jsonrpc_begin_result(request);            /* [한국어] 응답 빌드 시작. */
	spdk_json_write_object_begin(w);                   /* [한국어] 최상위 '{' 시작. */

	/* [한국어] 시퀀스 카운터들 — 합산된 누적값. */
	spdk_json_write_named_uint64(w, "sequence_executed", stats->sequence_executed);
	/* [한국어] 정상 완료된 시퀀스 누적 수. */
	spdk_json_write_named_uint64(w, "sequence_failed", stats->sequence_failed);
	/* [한국어] 실패 시퀀스 누적 수. */
	spdk_json_write_named_uint64(w, "sequence_outstanding", stats->sequence_outstanding);
	/* [한국어] 현재 처리 중 시퀀스 게이지(즉시값). */
	spdk_json_write_named_uint64(w, "task_outstanding", stats->task_outstanding);
	/* [한국어] 현재 처리 중 task 게이지. */

	/* [한국어] opcode별 통계 배열 시작. */
	spdk_json_write_named_array_begin(w, "operations");
	for (i = 0; i < SPDK_ACCEL_OPC_LAST; ++i) {
		/* [한국어] 한 번도 사용되지 않은 opcode는 응답에서 제외(잡음 줄이기). */
		if (stats->operations[i].executed + stats->operations[i].failed == 0) {
			continue;
		}
		/* [한국어] opcode → 담당 모듈 이름 조회. 미할당이면 음수 반환 → 응답에서 스킵. */
		rc = spdk_accel_get_opc_module_name(i, &module_name);
		if (rc) {
			continue;
		}
		spdk_json_write_object_begin(w);           /* [한국어] 한 opcode 객체 '{' 시작. */
		/* [한국어] "opcode": 사람이 읽는 op 이름. */
		spdk_json_write_named_string(w, "opcode", spdk_accel_get_opcode_name(i));
		/* [한국어] "module_name": 담당 모듈 이름. */
		spdk_json_write_named_string(w, "module_name", module_name);
		/* [한국어] "executed": 성공 누적 수. */
		spdk_json_write_named_uint64(w, "executed", stats->operations[i].executed);
		/* [한국어] "failed": 실패 누적 수. */
		spdk_json_write_named_uint64(w, "failed", stats->operations[i].failed);
		/* [한국어] "num_bytes": 처리 바이트 누계. */
		spdk_json_write_named_uint64(w, "num_bytes", stats->operations[i].num_bytes);
		spdk_json_write_object_end(w);             /* [한국어] opcode 객체 '}' 종료. */
	}
	spdk_json_write_array_end(w);                      /* [한국어] operations 배열 ']' 종료. */

	/* [한국어] 자원 부족 리트라이 카운터들 — 백프레셔 진단용. */
	spdk_json_write_named_uint64(w, "retry_task", stats->retry.task);
	/* [한국어] task 풀 비어 _get_task가 NULL을 반환한 회수. */
	spdk_json_write_named_uint64(w, "retry_sequence", stats->retry.sequence);
	/* [한국어] sequence 풀 비어 신규 시퀀스 할당 실패한 회수. */
	spdk_json_write_named_uint64(w, "retry_iobuf", stats->retry.iobuf);
	/* [한국어] iobuf 부족으로 시퀀스가 backlog로 enqueue된 회수. */
	spdk_json_write_named_uint64(w, "retry_bufdesc", stats->retry.bufdesc);
	/* [한국어] accel_buffer 디스크립터 풀 부족으로 발급 실패한 회수. */

	spdk_json_write_object_end(w);                     /* [한국어] 최상위 '}' 종료. */
	spdk_jsonrpc_end_result(request, w);               /* [한국어] 응답 송신. */
}

/*
 * [한국어]
 * rpc_accel_get_stats - 모든 reactor 채널의 accel 통계를 비동기로 수집해 JSON으로 회신.
 *
 * @request: RPC 요청. cb_arg로 그대로 전달되어 done 콜백에서 사용.
 * @params: 파라미터 없음(현재 디코더 미사용).
 *
 * 비동기 모델: accel_get_stats는 spdk_for_each_channel을 통해 모든 채널을 순회하므로
 *  본 함수는 즉시 반환하고 실제 응답은 done 콜백에서 송신된다. 사용자는 단일 RPC 응답으로
 *  체감하지만 내부적으로는 채널 수만큼의 fan-out/fan-in이 일어난다.
 *
 * 호출 체인: 클라이언트 → [본 함수] → accel_get_stats(spdk_for_each_channel) → ... →
 *           rpc_accel_get_stats_done → spdk_jsonrpc_end_result
 */
static void
rpc_accel_get_stats(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	int rc;

	/* [한국어] 통계 수집 시작 — done 콜백에 request를 넘겨 응답을 그쪽에서 보내게 함. */
	rc = accel_get_stats(rpc_accel_get_stats_done, request);
	if (rc != 0) {
		/* [한국어] 채널 컨텍스트 할당 실패(-ENOMEM 등) — 즉시 에러 회신.
		 *  이 경로에서는 done 콜백이 호출되지 않으므로 여기서 응답을 마무리해야 한다. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
}
/* [한국어] RUNTIME 등록 — framework가 동작 중일 때만 의미 있는 통계 조회. */
SPDK_RPC_REGISTER("accel_get_stats", rpc_accel_get_stats, SPDK_RPC_RUNTIME)
