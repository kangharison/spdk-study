/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK trace JSON-RPC 핸들러 구현 (trace_rpc.c)
 *
 * === 파일의 역할 ===
 * SPDK의 JSON-RPC 서버가 외부 클라이언트(rpc.py / scripts)로부터 받는
 * trace 관련 명령을 처리하는 핸들러들을 정의한다. trace는 hot path
 * 성능을 보존하기 위해 컴파일 타임에 비활성이지만, 운영 중에 특정 group/tpoint를
 * 동적으로 켜고 끌 수 있어야 디버깅·프로파일링이 가능하다. 본 파일은 그
 * "운영 인터페이스" 역할을 하여, JSON 요청 → struct 디코딩 → spdk_trace_*
 * 공개 API 호출 → JSON 응답 흐름을 6개 RPC 메서드로 노출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 흐름:
 *   외부 client (scripts/rpc.py trace_set_tpoint_mask name=bdev tpoint_mask=0x1)
 *     → JSON over Unix socket
 *     → spdk_jsonrpc_server (lib/jsonrpc)
 *     → SPDK_RPC_REGISTER로 등록된 본 핸들러 dispatch
 *     → spdk_trace_set_tpoints/clear/enable_tpoint_group 호출 (lib/trace/trace_flags.c)
 *     → g_trace_file->tpoint_mask[group_id] 비트 갱신
 *   다음에 hot path가 실행되면 spdk_trace_tpoint_enabled() fast-check가
 *   변경된 mask를 즉시 반영하여 record/skip 결정을 바꾼다.
 * SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME: 부팅 단계와 정상 운영 양쪽에서 호출 가능.
 * 실행 컨텍스트: jsonrpc 서버 스레드 (보통 메인 reactor) 단일 스레드 — 동기화 부담 없음.
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더):
 *   spdk/rpc.h  — SPDK_RPC_REGISTER 매크로, spdk_jsonrpc_request 타입.
 *   spdk/util.h — SPDK_COUNTOF, spdk_u64log2 (mask → bit index 변환).
 *   spdk/trace.h — spdk_trace_* 공개 API.
 *   spdk/log.h  — SPDK_DEBUGLOG (디코드 실패 진단 출력).
 *   trace_internal.h — trace_get_shm_name (shm 경로 조회).
 * 의존하는 모듈: scripts/rpc/trace.py가 본 파일이 노출하는 메서드명을
 *                Python wrapper로 감싼다. SPDK_RPC_REGISTER가 자동 등록하므로
 *                별도 호출 코드 없음 (.data 섹션 + constructor).
 * 데이터 흐름: client JSON → spdk_json_decode_object → struct rpc_tpoint_group
 *              → spdk_trace_create_tpoint_group_mask → set/clear → bool 응답.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct rpc_tpoint_group: 두 RPC가 공유하는 입력 파라미터 묶음 (name, tpoint_mask).
 *   - rpc_trace_set_tpoint_mask:    group 내 일부 tpoint OR 활성.
 *   - rpc_trace_clear_tpoint_mask:  group 내 일부 tpoint AND-NOT 비활성.
 *   - rpc_trace_enable_tpoint_group:  group 전체 활성 (모든 tpoint=1).
 *   - rpc_trace_disable_tpoint_group: group 전체 비활성 (모든 tpoint=0).
 *   - rpc_trace_get_tpoint_group_mask: group별 enabled 여부와 mask를 JSON 응답.
 *   - rpc_trace_get_info: shm 경로 + group 별 mask 요약. spdk_trace 도구가
 *                         바이너리 위치를 찾기 위해 가장 먼저 호출.
 */

#include "spdk/rpc.h"           /* [한국어] SPDK_RPC_REGISTER, spdk_jsonrpc_send_*: RPC 자동 등록·응답 API. */
#include "spdk/util.h"          /* [한국어] SPDK_COUNTOF (배열 크기), spdk_u64log2 (mask 비트 위치 추출). */
#include "spdk/trace.h"         /* [한국어] spdk_trace_set/clear_tpoints, _group_mask 등 공개 API. */
#include "spdk/log.h"           /* [한국어] SPDK_DEBUGLOG(trace, ...) — 디코드 실패 시 진단 메시지. */
#include "trace_internal.h"     /* [한국어] trace_get_shm_name() — RPC 응답에 shm 경로 포함용. */

/* [한국어] 두 set/clear RPC가 공유하는 입력 파라미터 구조체.
 * spdk_json_decode_object가 JSON 키 → 이 구조체 필드로 채운다. */
struct rpc_tpoint_group {
	char *name;
	/* [한국어] tpoint group의 이름 (예: "bdev", "nvme", "all").
	 * 설정자: spdk_json_decode_string이 strdup으로 할당 → 호출자가 free 책임.
	 * 읽는 자: spdk_trace_create_tpoint_group_mask가 reg_fn 리스트를 매칭.
	 * 값 범위: SPDK_TRACE_REGISTER_FN으로 등록된 group 이름 또는 "all".
	 * NULL이면 디코드 실패 — invalid 응답으로 분기. */

	uint64_t tpoint_mask;
	/* [한국어] group 내 활성/비활성화할 tpoint 비트마스크 (group당 최대 64 tpoint).
	 * 설정자: spdk_json_decode_uint64 (선택적 필드 — 생략 시 0).
	 * 읽는 자: spdk_trace_set_tpoints/clear_tpoints가 OR/AND-NOT 적용.
	 * 값 범위: 0 ~ UINT64_MAX. enable/disable_tpoint_group RPC에서는 사용되지 않음. */
};

/*
 * [한국어]
 * free_rpc_tpoint_group - rpc_tpoint_group의 동적 할당 필드 해제 (RAII 보조).
 *
 * @p: 해제할 구조체 포인터 (구조체 자체는 stack 위 — name만 strdup된 상태).
 *
 * spdk_json_decode_string이 strdup으로 할당한 name을 free.
 * 정상 경로와 invalid 경로 양쪽에서 호출되므로 멱등성을 위해 free(NULL)도 안전.
 * 컨텍스트: RPC 핸들러 종료 직전 단일 스레드 호출.
 */
static void
free_rpc_tpoint_group(struct rpc_tpoint_group *p)
{
	free(p->name);  /* [한국어] free(NULL)은 no-op이므로 디코드 실패 시에도 안전. */
}

/* [한국어] set/clear_tpoint_mask RPC의 JSON 디코더 테이블.
 * 각 entry: {key 이름, 구조체 내 offset, 디코더 함수, optional flag}.
 * "name"은 필수, "tpoint_mask"는 4번째 인자 true로 optional 표시. */
static const struct spdk_json_object_decoder rpc_trace_set_tpoint_mask_decoders[] = {
	{"name", offsetof(struct rpc_tpoint_group, name), spdk_json_decode_string},
	/* [한국어] JSON "name" → req.name (필수). 미존재 시 decode_object가 비-0 반환. */
	{"tpoint_mask", offsetof(struct rpc_tpoint_group, tpoint_mask), spdk_json_decode_uint64, true},
	/* [한국어] JSON "tpoint_mask" → req.tpoint_mask (선택, 4번째 true=optional).
	 * 생략 시 zero-init 그대로 유지(=0) → set은 no-op, clear도 no-op. */
};

/*
 * [한국어]
 * rpc_trace_set_tpoint_mask - "trace_set_tpoint_mask" RPC 핸들러: group 내 일부 tpoint 활성.
 *
 * @request: jsonrpc 서버가 전달한 응답 컨텍스트 (성공/실패 응답 송신용).
 * @params:  JSON 입력 파라미터 ({"name": "<group>", "tpoint_mask": <u64>}).
 *
 * 요청 예: {"name": "bdev", "tpoint_mask": "0xF"}
 *   → bdev group의 하위 4개 tpoint를 활성화.
 *
 * 동작 단계:
 *   1) JSON 객체를 rpc_tpoint_group으로 디코드 (실패 시 invalid).
 *   2) name이 NULL이면 invalid (사실상 1)에서 걸리지만 방어적 검사).
 *   3) name → group bit mask 변환 (예: "bdev" → 1<<bdev_id).
 *   4) mask가 0(=알 수 없는 이름)이면 invalid.
 *   5) spdk_u64log2로 single-bit mask를 group_id 정수로 변환 후 set_tpoints 호출.
 *      - g_trace_file->tpoint_mask[group_id] |= req.tpoint_mask 적용.
 *   6) bool true 응답 송신.
 * 컨텍스트: jsonrpc 서버 스레드 단일 호출. lock 불필요.
 * 호출 체인: client → jsonrpc_server → rpc_trace_set_tpoint_mask
 *           → spdk_trace_create_tpoint_group_mask → spdk_trace_set_tpoints.
 */
static void
rpc_trace_set_tpoint_mask(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_tpoint_group req = {};      /* [한국어] zero-init (name=NULL, tpoint_mask=0). */
	uint64_t tpoint_group_mask = 0;        /* [한국어] name → 해당 group의 single-bit mask. */

	if (spdk_json_decode_object(params, rpc_trace_set_tpoint_mask_decoders,
				    SPDK_COUNTOF(rpc_trace_set_tpoint_mask_decoders), &req)) {
		/* [한국어] JSON 파싱 실패 (필수 필드 누락, 타입 불일치 등) → invalid 분기. */
		SPDK_DEBUGLOG(trace, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.name == NULL) {
		/* [한국어] 방어적 검사 — decode_object가 필수 필드 누락을 잡지만 이중 안전. */
		SPDK_DEBUGLOG(trace, "flag was NULL\n");
		goto invalid;
	}

	tpoint_group_mask = spdk_trace_create_tpoint_group_mask(req.name);
	/* [한국어] reg_fn 리스트 순회로 name 매칭 → 해당 group의 1<<tgroup_id 비트 반환.
	 * 매칭 실패 시 0 반환. "all"이면 모든 group 비트 OR 반환 (set에서는 의미 없음). */
	if (tpoint_group_mask == 0) {
		/* [한국어] 알 수 없는 group 이름 → invalid 응답. */
		goto invalid;
	}

	spdk_trace_set_tpoints(spdk_u64log2(tpoint_group_mask), req.tpoint_mask);
	/* [한국어] u64log2(single-bit-mask) = group_id 정수 (예: 0x4 → 2).
	 * "all"의 경우 multi-bit가 들어와 log2가 최상위 비트만 반환 → 단일 group만 set됨
	 * (이 RPC는 "all" 사용 의도가 아니므로 의도된 동작). */

	free_rpc_tpoint_group(&req);            /* [한국어] strdup된 name 해제. */

	spdk_jsonrpc_send_bool_response(request, true);  /* [한국어] {"result": true} 응답. */
	return;

invalid:
	/* [한국어] 디코드/매칭 실패 공통 처리 — JSON-RPC error -32602 (Invalid params). */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_tpoint_group(&req);            /* [한국어] 부분 디코드된 name도 안전하게 해제. */
}
/* [한국어] SPDK_RPC_REGISTER: 메서드 이름 "trace_set_tpoint_mask"을 jsonrpc 서버에 등록.
 * 플래그: STARTUP|RUNTIME — 부팅 단계와 정상 운영 양쪽에서 수신 가능.
 * 매크로 내부에 constructor가 있어 main 진입 전 자동 등록. */
SPDK_RPC_REGISTER("trace_set_tpoint_mask", rpc_trace_set_tpoint_mask,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_trace_clear_tpoint_mask - "trace_clear_tpoint_mask" RPC: group 내 일부 tpoint 비활성.
 *
 * @request/@params: set_tpoint_mask와 동일 시그니처.
 *
 * set의 대칭 — 동일 디코더 테이블 재사용.
 * 동작은 set과 같지만 마지막 단계에서 clear_tpoints를 호출 (mask AND-NOT 적용).
 * 사용 예: 디버깅 종료 시 켜두었던 tpoint를 다시 끄는 용도.
 * 호출 체인: client → jsonrpc_server → rpc_trace_clear_tpoint_mask
 *           → spdk_trace_create_tpoint_group_mask → spdk_trace_clear_tpoints.
 */
static void
rpc_trace_clear_tpoint_mask(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_tpoint_group req = {};      /* [한국어] zero-init. */
	uint64_t tpoint_group_mask = 0;        /* [한국어] group 매칭 결과의 single-bit mask. */

	if (spdk_json_decode_object(params, rpc_trace_set_tpoint_mask_decoders,
				    SPDK_COUNTOF(rpc_trace_set_tpoint_mask_decoders), &req)) {
		/* [한국어] set과 동일한 디코더 재사용 (입력 스키마가 같음). */
		SPDK_DEBUGLOG(trace, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.name == NULL) {
		/* [한국어] 방어적 NULL 검사. */
		SPDK_DEBUGLOG(trace, "flag was NULL\n");
		goto invalid;
	}

	tpoint_group_mask = spdk_trace_create_tpoint_group_mask(req.name);
	/* [한국어] 이름 → group의 single-bit mask 변환. */
	if (tpoint_group_mask == 0) {
		goto invalid;  /* [한국어] 알 수 없는 group → invalid. */
	}

	spdk_trace_clear_tpoints(spdk_u64log2(tpoint_group_mask), req.tpoint_mask);
	/* [한국어] g_trace_file->tpoint_mask[group_id] &= ~req.tpoint_mask 적용. */

	free_rpc_tpoint_group(&req);            /* [한국어] name 해제. */

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_tpoint_group(&req);
}
SPDK_RPC_REGISTER("trace_clear_tpoint_mask", rpc_trace_clear_tpoint_mask,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
/* [한국어] STARTUP|RUNTIME — set과 동일하게 양쪽에서 호출 가능. */

/* [한국어] enable/disable_tpoint_group RPC의 디코더 — name만 요구 (mask 무관).
 * group 단위 on/off라 tpoint_mask 필드는 의미 없음. */
static const struct spdk_json_object_decoder rpc_trace_enable_tpoint_group_decoders[] = {
	{"name", offsetof(struct rpc_tpoint_group, name), spdk_json_decode_string},
	/* [한국어] 필수 필드. "all"도 허용 — 모든 group 일괄 활성/비활성. */
};

/*
 * [한국어]
 * rpc_trace_enable_tpoint_group - "trace_enable_tpoint_group" RPC: group의 모든 tpoint 활성.
 *
 * @request: jsonrpc 응답 컨텍스트.
 * @params:  {"name": "<group>"} 또는 {"name": "all"}.
 *
 * 요청 예: {"name": "bdev"} → bdev group의 64개 tpoint를 모두 활성.
 *          {"name": "all"}  → 등록된 모든 group을 일괄 활성.
 *
 * 동작:
 *   1) JSON 디코드 → req.name.
 *   2) spdk_trace_enable_tpoint_group(name) 호출 — 내부에서 reg_fn 매칭 후
 *      g_trace_file->tpoint_mask[group_id] = -1ULL 설정.
 *   3) 0 반환이면 성공, 비-0이면 invalid (g_trace_file NULL 또는 알 수 없는 이름).
 * 컨텍스트: jsonrpc 서버 단일 스레드.
 */
static void
rpc_trace_enable_tpoint_group(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_tpoint_group req = {};      /* [한국어] zero-init. tpoint_mask는 사용 안 함. */

	if (spdk_json_decode_object(params, rpc_trace_enable_tpoint_group_decoders,
				    SPDK_COUNTOF(rpc_trace_enable_tpoint_group_decoders), &req)) {
		/* [한국어] "name"만 요구하는 디코더 사용. */
		SPDK_DEBUGLOG(trace, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.name == NULL) {
		/* [한국어] 방어적 검사. */
		SPDK_DEBUGLOG(trace, "flag was NULL\n");
		goto invalid;
	}

	if (spdk_trace_enable_tpoint_group(req.name)) {
		/* [한국어] 0이 아니면 실패 — trace 미초기화 또는 이름 불일치.
		 * 내부적으로 set_tpoint_group_mask(mask)가 비트별 set_tpoints(i, -1ULL)를
		 * 일괄 적용하여 64개 tpoint 비트를 모두 1로 만듦. */
		goto invalid;
	}

	free_rpc_tpoint_group(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_tpoint_group(&req);
}
SPDK_RPC_REGISTER("trace_enable_tpoint_group", rpc_trace_enable_tpoint_group,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
/* [한국어] STARTUP|RUNTIME — 부팅 직후/운영 중 모두에서 켤 수 있도록 등록. */

/*
 * [한국어]
 * rpc_trace_disable_tpoint_group - "trace_disable_tpoint_group" RPC: group의 모든 tpoint 비활성.
 *
 * enable의 대칭. 내부에서 spdk_trace_disable_tpoint_group → clear_tpoint_group_mask
 * → 비트별 clear_tpoints(i, -1ULL)로 64개 tpoint 비트를 0으로 만듦.
 * 동일 디코더(enable과 같은 "name"-only 스키마) 재사용.
 */
static void
rpc_trace_disable_tpoint_group(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_tpoint_group req = {};      /* [한국어] zero-init. */

	if (spdk_json_decode_object(params, rpc_trace_enable_tpoint_group_decoders,
				    SPDK_COUNTOF(rpc_trace_enable_tpoint_group_decoders), &req)) {
		/* [한국어] enable과 동일한 디코더 재사용. */
		SPDK_DEBUGLOG(trace, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.name == NULL) {
		SPDK_DEBUGLOG(trace, "flag was NULL\n");
		goto invalid;
	}

	if (spdk_trace_disable_tpoint_group(req.name)) {
		/* [한국어] 비-0이면 실패 — invalid 응답. */
		goto invalid;
	}

	free_rpc_tpoint_group(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_tpoint_group(&req);
}
SPDK_RPC_REGISTER("trace_disable_tpoint_group", rpc_trace_disable_tpoint_group,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_trace_get_tpoint_group_mask - "trace_get_tpoint_group_mask" RPC: 현재 활성 group 요약.
 *
 * @request: 응답 컨텍스트.
 * @params:  반드시 NULL (파라미터 없음). 비어있지 않으면 invalid.
 *
 * 응답 JSON 예:
 *   {
 *     "tpoint_group_mask": "0x5",          // 현재 활성 group 비트맵
 *     "bdev":   {"enabled": true,  "mask": "0x1"},
 *     "nvme":   {"enabled": false, "mask": "0x4"},
 *     "thread": {"enabled": false, "mask": "0x8"}
 *     ...
 *   }
 *
 * 동작:
 *   1) 파라미터 검증 — 없어야 함.
 *   2) jsonrpc_begin_result로 응답 writer 획득.
 *   3) 전체 group_mask 계산 (어느 group이라도 tpoint 1개 이상 활성이면 비트 set).
 *   4) reg_fn 리스트 순회 — 각 group마다 enabled/mask 객체 출력.
 *   5) 응답 closing.
 * 컨텍스트: jsonrpc 서버 단일 스레드 (메인 reactor).
 */
static void
rpc_trace_get_tpoint_group_mask(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	uint64_t tpoint_group_mask;            /* [한국어] 활성 group 전체 비트맵. */
	char mask_str[20];                     /* [한국어] "0xFFFFFFFFFFFFFFFF" 표현 위 충분한 길이. */
	bool enabled;                          /* [한국어] 각 group의 enable 여부 (tpoint 1개라도 set). */
	struct spdk_json_write_ctx *w;         /* [한국어] JSON 출력 writer 컨텍스트. */
	struct spdk_trace_register_fn *register_fn;  /* [한국어] reg_fn 리스트 순회 커서. */

	if (params != NULL) {
		/* [한국어] 본 RPC는 파라미터를 받지 않음 — 들어오면 명세 위반. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "trace_get_tpoint_group_mask requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);                 /* [한국어] JSON writer 시작. */
	tpoint_group_mask = spdk_trace_get_tpoint_group_mask(); /* [한국어] 전체 활성 group 비트맵 조회. */

	spdk_json_write_object_begin(w);                        /* [한국어] 응답 객체 '{' 시작. */

	snprintf(mask_str, sizeof(mask_str), "0x%" PRIx64, tpoint_group_mask);
	/* [한국어] u64 → 16진수 문자열 변환. PRIx64 매크로로 플랫폼 독립. */
	spdk_json_write_named_string(w, "tpoint_group_mask", mask_str);
	/* [한국어] "tpoint_group_mask": "<hex>" 키-값 출력. */

	register_fn = spdk_trace_get_first_register_fn();       /* [한국어] reg_fn 리스트 head. */
	while (register_fn) {
		enabled = spdk_trace_get_tpoint_mask(register_fn->tgroup_id) != 0;
		/* [한국어] 해당 group의 tpoint_mask 64bit이 0이면 모든 tpoint off → enabled=false. */

		spdk_json_write_named_object_begin(w, register_fn->name);
		/* [한국어] 그룹 이름을 키로 하는 nested 객체 시작 (예: "bdev": { ... }). */
		spdk_json_write_named_bool(w, "enabled", enabled);
		/* [한국어] "enabled": true/false 출력. */

		snprintf(mask_str, sizeof(mask_str), "0x%lx", (1UL << register_fn->tgroup_id));
		/* [한국어] group의 single-bit mask 표현 (사용자가 set_tpoint_mask에 넘길 수 있도록). */
		spdk_json_write_named_string(w, "mask", mask_str);
		spdk_json_write_object_end(w);                  /* [한국어] nested 객체 '}' 닫기. */

		register_fn = spdk_trace_get_next_register_fn(register_fn);
		/* [한국어] 다음 reg_fn으로 이동 (singly linked list). */
	}

	spdk_json_write_object_end(w);                          /* [한국어] 최상위 응답 객체 '}' 닫기. */
	spdk_jsonrpc_end_result(request, w);                    /* [한국어] writer flush + 송신. */
}
SPDK_RPC_REGISTER("trace_get_tpoint_group_mask", rpc_trace_get_tpoint_group_mask,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
/* [한국어] STARTUP|RUNTIME — 어느 단계에서든 상태 조회 가능. */

/*
 * [한국어]
 * rpc_trace_get_info - "trace_get_info" RPC: trace 전체 메타데이터 + 정확한 tpoint mask 조회.
 *
 * @request: 응답 컨텍스트.
 * @params:  반드시 NULL.
 *
 * 응답 JSON 예:
 *   {
 *     "tpoint_shm_path":   "/dev/shm/_trace.spdk_tgt.12345",
 *     "tpoint_group_mask": "0x5",
 *     "bdev":   {"mask": "0x1", "tpoint_mask": "0xFFFFFFFFFFFFFFFF"},
 *     "nvme":   {"mask": "0x4", "tpoint_mask": "0x0"}
 *     ...
 *   }
 *
 * get_tpoint_group_mask와 차이점:
 *   1) shm 경로 추가 → spdk_trace 도구가 mmap 위치를 알 수 있음.
 *   2) "enabled" bool 대신 "tpoint_mask" 64-bit hex → 정확히 어떤 tpoint들이 켜져 있는지 노출.
 * scripts/spdk_trace 등이 이 RPC로 메타데이터 + shm 경로를 한 번에 받아 dump 시작.
 */
static void
rpc_trace_get_info(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	char shm_path[128];                    /* [한국어] "/dev/shm/<shm_name>" 전체 경로 buffer. */
	uint64_t tpoint_group_mask;            /* [한국어] 활성 group 전체 비트맵. */
	uint64_t tpoint_mask;                  /* [한국어] 각 group 내 tpoint 활성 비트마스크. */
	char tpoint_mask_str[20];              /* [한국어] tpoint_mask hex 문자열 buffer. */
	char mask_str[20];                     /* [한국어] group mask hex 문자열 buffer. */
	struct spdk_json_write_ctx *w;         /* [한국어] JSON writer. */
	struct spdk_trace_register_fn *register_fn;  /* [한국어] reg_fn 순회 커서. */

	if (params != NULL) {
		/* [한국어] 파라미터를 받지 않는 RPC. 들어오면 invalid. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "trace_get_info requires no parameters");
		return;
	}

	snprintf(shm_path, sizeof(shm_path), "/dev/shm%s", trace_get_shm_name());
	/* [한국어] /dev/shm prefix + g_shm_name 결합 → 외부 도구가 mmap할 절대 경로 생성.
	 * trace_get_shm_name은 trace_internal.h가 노출, trace.c에서 정의. */
	tpoint_group_mask = spdk_trace_get_tpoint_group_mask();
	/* [한국어] 활성 group들의 single-bit OR 결과. */

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);                        /* [한국어] '{' 시작. */
	spdk_json_write_named_string(w, "tpoint_shm_path", shm_path);
	/* [한국어] 외부 spdk_trace 바이너리는 이 경로를 받아 그대로 mmap. */

	snprintf(mask_str, sizeof(mask_str), "0x%" PRIx64, tpoint_group_mask);
	spdk_json_write_named_string(w, "tpoint_group_mask", mask_str);
	/* [한국어] 활성 group 요약 hex. */

	register_fn = spdk_trace_get_first_register_fn();
	while (register_fn) {

		tpoint_mask = spdk_trace_get_tpoint_mask(register_fn->tgroup_id);
		/* [한국어] 이 group의 64-bit tpoint mask 정확값 조회.
		 * 사용자가 어떤 tpoint 비트가 켜져 있는지 정확히 보고, 부분 조정에 사용. */

		spdk_json_write_named_object_begin(w, register_fn->name);
		/* [한국어] group 이름을 key로 nested 객체 시작 (예: "bdev": {). */
		snprintf(mask_str, sizeof(mask_str), "0x%lx", (1UL << register_fn->tgroup_id));
		spdk_json_write_named_string(w, "mask", mask_str);
		/* [한국어] group의 single-bit ID (set/clear_tpoint_mask RPC 입력에 매칭). */
		snprintf(tpoint_mask_str, sizeof(tpoint_mask_str), "0x%lx", tpoint_mask);
		spdk_json_write_named_string(w, "tpoint_mask", tpoint_mask_str);
		/* [한국어] group 내 64-bit tpoint 활성 mask hex 표현. */
		spdk_json_write_object_end(w);                  /* [한국어] nested 객체 '}' 닫기. */

		register_fn = spdk_trace_get_next_register_fn(register_fn);
		/* [한국어] 다음 reg_fn 노드. */
	}

	spdk_json_write_object_end(w);                          /* [한국어] 최상위 객체 '}' 닫기. */
	spdk_jsonrpc_end_result(request, w);                    /* [한국어] flush + 응답 송신. */
}
SPDK_RPC_REGISTER("trace_get_info", rpc_trace_get_info,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
/* [한국어] STARTUP|RUNTIME — 단계 무관 호출 가능. */
