/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] error 가상 bdev의 JSON-RPC 핸들러 (vbdev_error_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK error injection vbdev 모듈의 JSON-RPC 인터페이스를 정의한다. error vbdev은
 * 기존 base bdev 위에 한 층 덧대어, 클라이언트가 지정한 조건(io_type, error_type, 카운트,
 * 큐 깊이, 데이터 손상 오프셋 등)에 맞춰 I/O 결과를 인위적으로 실패/지연/손상시키는 가상
 * 디바이스이다. 장애 주입(fault injection) 테스트에 사용된다.
 * 본 파일은 다음 세 RPC를 등록한다:
 *   1) bdev_error_create        — error vbdev 생성.
 *   2) bdev_error_delete        — error vbdev 제거.
 *   3) bdev_error_inject_error  — 가동 중인 error vbdev에 새 장애 주입 규칙 설정/해제.
 * 그 외에 io_type / error_type 두 enum 문자열을 디코딩하는 보조 디코더를 정의한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [클라이언트] → SPDK RPC 디스패처 → [이 파일의 핸들러]
 *   → vbdev_error_create / vbdev_error_delete / vbdev_error_inject_error (vbdev_error.c)
 *   → spdk_bdev_register / unregister / per-vbdev mutex 보호된 규칙 갱신.
 * 실행 컨텍스트는 SPDK RPC poller(메인 reactor). inject_error는 vbdev 내부의 lock으로
 * 동시 접근을 막는다(I/O path와 RPC path의 race 방지).
 *
 * === 타 모듈과의 연결 ===
 * - include: spdk/stdinc.h, spdk/string.h, spdk/rpc.h, spdk/util.h, spdk/log.h, vbdev_error.h.
 * - 의존: vbdev_error.c(실제 동작과 enum 정의), lib/jsonrpc, lib/rpc, lib/bdev.
 * - 데이터 흐름: 클라이언트 JSON → 본 파일에서 디코딩(문자열 → enum 매핑) →
 *   vbdev_error_inject_error()가 vbdev_error_inject_opts를 vbdev 내부에 보관 →
 *   I/O path에서 그 옵션에 따라 결과를 변형.
 *
 * === 주요 함수/구조체 요약 ===
 * - `rpc_error_bdev_decode_io_type()`     : "read"/"write"/"flush"/"unmap"/"all"/"clear" 매핑.
 * - `rpc_error_bdev_decode_error_type()`  : "failure"/"pending"/"corrupt_data"/"nomem" 매핑.
 * - `rpc_bdev_error_create()`             : create RPC 핸들러.
 * - `rpc_bdev_error_delete()`             : delete RPC 핸들러(비동기 콜백 사용).
 * - `rpc_bdev_error_inject_error()`       : 장애 주입 규칙 설정/해제 RPC 핸들러.
 * - `struct rpc_error_information`        : inject_error의 입력 컨테이너 (옵션 묶음 포함).
 */

/* [한국어] POSIX 표준 헤더 묶음(stdint, errno, string 등). */
#include "spdk/stdinc.h"
/* [한국어] spdk_strerror — errno 문자열 변환. */
#include "spdk/string.h"
/* [한국어] SPDK_RPC_REGISTER, JSON 디코딩/응답 API. */
#include "spdk/rpc.h"
/* [한국어] SPDK_COUNTOF 등 유틸 매크로. */
#include "spdk/util.h"
/* [한국어] SPDK_NOTICELOG/ERRLOG. */
#include "spdk/log.h"
/* [한국어] vbdev_error 모듈의 공개 헤더 — vbdev_error_create/delete/inject_error 선언과
 *           VBDEV_IO_FAILURE 등 enum 정의, struct vbdev_error_inject_opts. */
#include "vbdev_error.h"

/*
 * [한국어]
 * rpc_error_bdev_decode_io_type - JSON 문자열을 SPDK_BDEV_IO_TYPE_* enum값으로 매핑.
 *
 * @val: JSON 문자열 토큰 (raw key/value).
 * @out: 결과를 받을 uint32_t 포인터(rpc_error_information.opts.io_type).
 * @return: 0=성공, -EINVAL=알 수 없는 문자열.
 *
 * 본 함수는 spdk_json_object_decoder의 사용자 정의 decode 함수로 등록된다. 디코더 테이블에서
 * 표준 spdk_json_decode_string 대신 이 함수를 지정해, 문자열을 즉시 enum으로 변환하도록 한다.
 *
 * 매핑:
 *   "read"  → SPDK_BDEV_IO_TYPE_READ
 *   "write" → SPDK_BDEV_IO_TYPE_WRITE
 *   "flush" → SPDK_BDEV_IO_TYPE_FLUSH
 *   "unmap" → SPDK_BDEV_IO_TYPE_UNMAP
 *   "all"   → 0xffffffff (모든 io_type에 적용)
 *   "clear" → 0          (해당 vbdev의 모든 규칙 해제)
 *
 * 호출 체인: spdk_json_decode_object → [이 함수].
 * 실행 컨텍스트: SPDK RPC poller.
 */
static int
rpc_error_bdev_decode_io_type(const struct spdk_json_val *val, void *out)
{
	uint32_t *io_type = out;  /* [한국어] decode 결과를 저장할 위치(out은 opts.io_type 주소). */

	/* [한국어] 각 분기는 JSON 문자열과 정확 일치 비교. spdk_json_strequal은 길이/내용 모두 비교. */
	if (spdk_json_strequal(val, "read") == true) {
		*io_type = SPDK_BDEV_IO_TYPE_READ;        /* [한국어] 읽기 I/O에만 장애 주입. */
	} else if (spdk_json_strequal(val, "write") == true) {
		*io_type = SPDK_BDEV_IO_TYPE_WRITE;       /* [한국어] 쓰기 I/O에만 장애 주입. */
	} else if (spdk_json_strequal(val, "flush") == true) {
		*io_type = SPDK_BDEV_IO_TYPE_FLUSH;       /* [한국어] 플러시 명령에만 적용. */
	} else if (spdk_json_strequal(val, "unmap") == true) {
		*io_type = SPDK_BDEV_IO_TYPE_UNMAP;       /* [한국어] UNMAP/discard에만 적용. */
	} else if (spdk_json_strequal(val, "all") == true) {
		*io_type = 0xffffffff;                    /* [한국어] 비트마스크 전부 1 — 모든 종류. */
	} else if (spdk_json_strequal(val, "clear") == true) {
		*io_type = 0;                             /* [한국어] 0 = 해당 vbdev의 규칙 모두 해제 신호. */
	} else {
		/* [한국어] 정의되지 않은 문자열 — 입력 잘못. 로그 후 -EINVAL. */
		SPDK_NOTICELOG("Invalid parameter value: io_type\n");
		return -EINVAL;
	}

	return 0;  /* [한국어] 디코딩 성공. */
}

/*
 * [한국어]
 * rpc_error_bdev_decode_error_type - JSON 문자열을 VBDEV_IO_* enum값으로 매핑.
 *
 * @val: JSON 토큰.
 * @out: 결과를 받을 uint32_t 포인터(opts.error_type).
 * @return: 0=성공, -EINVAL=알 수 없는 문자열.
 *
 * 매핑:
 *   "failure"      → VBDEV_IO_FAILURE      (즉시 실패 응답)
 *   "pending"      → VBDEV_IO_PENDING      (응답을 보내지 않고 영원히 대기 시뮬레이션)
 *   "corrupt_data" → VBDEV_IO_CORRUPT_DATA (데이터 버퍼 일부 변조 후 성공 응답)
 *   "nomem"        → VBDEV_IO_NOMEM        (-ENOMEM 반환 시뮬레이션)
 *
 * 호출 체인: spdk_json_decode_object → [이 함수]. 실행 컨텍스트: SPDK RPC poller.
 */
static int
rpc_error_bdev_decode_error_type(const struct spdk_json_val *val, void *out)
{
	uint32_t *error_type = out;  /* [한국어] decode 결과 저장 위치. */

	if (spdk_json_strequal(val, "failure") == true) {
		*error_type = VBDEV_IO_FAILURE;       /* [한국어] 즉시 -EIO 류 실패 응답. */
	} else if (spdk_json_strequal(val, "pending") == true) {
		*error_type = VBDEV_IO_PENDING;       /* [한국어] 응답 보류(타임아웃 시나리오). */
	} else if (spdk_json_strequal(val, "corrupt_data") == true) {
		*error_type = VBDEV_IO_CORRUPT_DATA;  /* [한국어] 데이터 변조 — 무결성 검사기 테스트용. */
	} else if (spdk_json_strequal(val, "nomem") == true) {
		*error_type = VBDEV_IO_NOMEM;         /* [한국어] -ENOMEM 시뮬레이션 — 큐 재시도 경로 검증. */
	} else {
		SPDK_NOTICELOG("Invalid parameter value: error_type\n");
		return -EINVAL;
	}

	return 0;
}

/* [한국어] bdev_error_create RPC 입력. JSON: { "base_name":"Nvme0n1", "uuid":"..." }. */
struct rpc_bdev_error_create {
	char *base_name;
	/* [한국어] error vbdev이 stack될 base bdev 이름.
	 * 설정자: 디코더 strdup. 읽는 자: vbdev_error_create()가 spdk_bdev_get_by_name 호출.
	 * 값 범위: NULL 가능. 동기화: 단일 RPC 호출 내. */

	struct spdk_uuid uuid;
	/* [한국어] 새 vbdev에 부여할 UUID(옵션 — 0이면 자동 생성).
	 * 설정자: spdk_json_decode_uuid. 읽는 자: vbdev_error_create. */
};

/*
 * [한국어]
 * free_rpc_bdev_error_create - create RPC의 strdup 문자열 해제.
 *
 * @req: 정리 대상. @return: 없음.
 */
static void
free_rpc_bdev_error_create(struct rpc_bdev_error_create *req)
{
	free(req->base_name);  /* [한국어] base_name 해제. */
}

/* [한국어] create RPC 디코더 — uuid는 옵션. */
static const struct spdk_json_object_decoder rpc_bdev_error_create_decoders[] = {
	{"base_name", offsetof(struct rpc_bdev_error_create, base_name), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_bdev_error_create, uuid), spdk_json_decode_uuid, true},
};

/*
 * [한국어]
 * rpc_bdev_error_create - "bdev_error_create" RPC 핸들러.
 *
 * @request: RPC 컨텍스트. @params: JSON 입력. @return: void.
 *
 * 동작: 디코딩 → vbdev_error_create() 호출 → 결과에 따라 boolean true 또는 errno 응답.
 * 응답이 단순 boolean이라는 점에서 다른 모듈(name 문자열 응답)과 다르다.
 *
 * 호출 체인: RPC dispatcher → [이 함수] → vbdev_error_create() (vbdev_error.c)
 *                                          → spdk_bdev_open_ext + spdk_bdev_register.
 * 실행 컨텍스트: SPDK RPC poller.
 */
static void
rpc_bdev_error_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_error_create req = {};  /* [한국어] 입력 임시 — 0 초기화로 옵션 필드 안전. */
	int rc = 0;                             /* [한국어] vbdev_error_create 반환 코드. */

	/* [한국어] JSON 디코딩 실패 → INTERNAL_ERROR. */
	if (spdk_json_decode_object(params, rpc_bdev_error_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_error_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] 핵심 호출 — base bdev 위에 error vbdev 등록. 새 vbdev 이름은
	 *           "EE_<base_name>" 패턴으로 vbdev_error.c가 자동 생성한다. */
	rc = vbdev_error_create(req.base_name, &req.uuid);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	/* [한국어] 성공 → boolean true. */
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_error_create(&req);  /* [한국어] strdup 정리. */
}
/* [한국어] "bdev_error_create" RPC 등록. */
SPDK_RPC_REGISTER("bdev_error_create", rpc_bdev_error_create, SPDK_RPC_RUNTIME)

/* [한국어] bdev_error_delete RPC 입력 — 단일 필드 name. */
struct rpc_delete_error {
	char *name;
	/* [한국어] 삭제 대상 error vbdev 이름.
	 * 설정자: 디코더. 읽는 자: vbdev_error_delete. 동기화: 단일 RPC 호출 내. */
};

/*
 * [한국어]
 * free_rpc_delete_error - delete RPC strdup 정리. @r: 대상.
 */
static void
free_rpc_delete_error(struct rpc_delete_error *r)
{
	free(r->name);  /* [한국어] 이름 해제. */
}

/* [한국어] delete 디코더 — 단일 필수 필드 name. */
static const struct spdk_json_object_decoder rpc_bdev_error_delete_decoders[] = {
	{"name", offsetof(struct rpc_delete_error, name), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_error_delete_cb - bdev_unregister 비동기 완료 후 클라이언트에 응답.
 *
 * @cb_arg   : 보존된 spdk_jsonrpc_request 포인터.
 * @bdeverrno: 0=성공, 음수=errno.
 * @return   : 없음.
 *
 * 호출 체인: vbdev_error_delete → spdk_bdev_unregister → 완료 → [이 콜백].
 * 실행 컨텍스트: bdev unregister 종료 thread.
 */
static void
rpc_bdev_error_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;  /* [한국어] 보존된 RPC 요청 컨텍스트. */

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);                         /* [한국어] 성공. */
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));  /* [한국어] 실패. */
	}
}

/*
 * [한국어]
 * rpc_bdev_error_delete - "bdev_error_delete" RPC 핸들러.
 *
 * @request, @params, @return: 표준 RPC 핸들러 시그니처.
 *
 * 디코딩 후 vbdev_error_delete()로 비동기 unregister 시작. 응답은 콜백에서.
 *
 * 호출 체인: RPC dispatcher → [이 함수] → vbdev_error_delete() → unregister → 콜백.
 * 실행 컨텍스트: SPDK RPC poller.
 */
static void
rpc_bdev_error_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_delete_error req = {NULL};  /* [한국어] 입력 임시. */

	if (spdk_json_decode_object(params, rpc_bdev_error_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_error_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] 비동기 unregister 시작. request를 cb_arg로 보존. */
	vbdev_error_delete(req.name, rpc_bdev_error_delete_cb, request);

cleanup:
	free_rpc_delete_error(&req);  /* [한국어] strdup 정리. */
}
/* [한국어] "bdev_error_delete" RPC 등록. */
SPDK_RPC_REGISTER("bdev_error_delete", rpc_bdev_error_delete, SPDK_RPC_RUNTIME)

/* [한국어] bdev_error_inject_error RPC 입력. JSON 예:
 *           { "name":"EE_Nvme0n1", "io_type":"read", "error_type":"failure",
 *             "num":10, "queue_depth":4, "corrupt_offset":0, "corrupt_value":0xff } */
struct rpc_error_information {
	char *name;
	/* [한국어] 대상 error vbdev 이름.
	 * 설정자: 디코더. 읽는 자: vbdev_error_inject_error. 동기화: 단일 RPC 호출 내. */

	struct vbdev_error_inject_opts opts;
	/* [한국어] 장애 주입 옵션 묶음(io_type, error_type, error_num, error_qd, corrupt_*).
	 * 디코더 테이블이 각 하위 필드(opts.io_type, opts.error_type 등)를 직접 채운다.
	 * 읽는 자: vbdev_error_inject_error()가 vbdev 내부 슬롯에 mutex 보호하에 저장.
	 * 값 범위/의미는 vbdev_error.c 정의 참고.
	 * 동기화: vbdev 내부 lock으로 I/O path와 동기화. */
};

/* [한국어] inject_error 디코더 테이블.
 *           - io_type/error_type: 사용자 정의 디코더(문자열 → enum).
 *           - num/queue_depth/corrupt_offset/corrupt_value: 옵션(true 플래그) — 미지정 시 0.
 *             단, opts.error_num은 이 RPC의 init 값으로 1을 기본값으로 둔다(아래 init {}). */
static const struct spdk_json_object_decoder rpc_bdev_error_inject_error_decoders[] = {
	{"name", offsetof(struct rpc_error_information, name), spdk_json_decode_string},
	{"io_type", offsetof(struct rpc_error_information, opts.io_type), rpc_error_bdev_decode_io_type},
	{"error_type", offsetof(struct rpc_error_information, opts.error_type), rpc_error_bdev_decode_error_type},
	{"num", offsetof(struct rpc_error_information, opts.error_num), spdk_json_decode_uint32, true},
	{"queue_depth", offsetof(struct rpc_error_information, opts.error_qd), spdk_json_decode_uint64, true},
	{"corrupt_offset", offsetof(struct rpc_error_information, opts.corrupt_offset), spdk_json_decode_uint64, true},
	{"corrupt_value", offsetof(struct rpc_error_information, opts.corrupt_value), spdk_json_decode_uint8, true},
};

/*
 * [한국어]
 * free_rpc_error_information - inject_error RPC strdup 정리.
 *
 * @p: 정리 대상. @return: 없음.
 */
static void
free_rpc_error_information(struct rpc_error_information *p)
{
	free(p->name);  /* [한국어] vbdev 이름 해제. */
}

/*
 * [한국어]
 * rpc_bdev_error_inject_error - "bdev_error_inject_error" RPC 핸들러.
 *
 * @request, @params, @return: 표준 시그니처.
 *
 * 동작:
 *   1) 입력을 디코딩(opts.error_num 기본값 1로 초기화 — 한 번 발동 후 자동 해제 효과).
 *   2) vbdev_error_inject_error()로 vbdev 내부 규칙 슬롯에 mutex 보호하에 갱신.
 *   3) 결과에 따라 boolean true 또는 errno 응답.
 *
 * 사용 시나리오:
 *   - io_type="all", error_type="clear" → 모든 규칙 해제.
 *   - error_num=N → 다음 N개의 매칭 I/O에만 적용.
 *   - queue_depth=K → 큐에 K개 이상 대기 중일 때만 발동.
 *
 * 호출 체인: RPC dispatcher → [이 함수] → vbdev_error_inject_error() (vbdev_error.c).
 * 실행 컨텍스트: SPDK RPC poller(vbdev 내부 lock과 동기화).
 */
static void
rpc_bdev_error_inject_error(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	/* [한국어] error_num을 기본 1로 초기화 — 클라이언트가 num을 생략해도 한 번만 발동되도록. */
	struct rpc_error_information req = {.opts.error_num = 1};
	int rc = 0;  /* [한국어] inject_error 반환 코드. */

	/* [한국어] JSON 디코딩 실패 → INTERNAL_ERROR. */
	if (spdk_json_decode_object(params, rpc_bdev_error_inject_error_decoders,
				    SPDK_COUNTOF(rpc_bdev_error_inject_error_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] vbdev 내부 mutex로 보호된 슬롯에 새 규칙을 저장. -ENODEV/-EINVAL 가능. */
	rc = vbdev_error_inject_error(req.name, &req.opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	/* [한국어] 성공 → boolean true. */
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_error_information(&req);  /* [한국어] strdup 정리. */
}
/* [한국어] "bdev_error_inject_error" RPC 등록. */
SPDK_RPC_REGISTER("bdev_error_inject_error", rpc_bdev_error_inject_error, SPDK_RPC_RUNTIME)
