/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] uring bdev 모듈의 JSON-RPC 핸들러 (bdev_uring_rpc.c)
 *
 * === 파일의 역할 ===
 * 사용자가 rpc.py 또는 다른 SPDK RPC 클라이언트로 호출하는 세 가지 JSON-RPC 메서드의
 * 핸들러를 구현한다: bdev_uring_create / bdev_uring_rescan / bdev_uring_delete.
 * 각 핸들러는 JSON params 를 디코드하고, bdev_uring.c 가 제공한 핵심 함수
 * (create_uring_bdev / bdev_uring_rescan / delete_uring_bdev) 를 호출한 뒤,
 * JSON 응답(또는 에러)을 클라이언트에 돌려준다. RPC 계층은 thin adapter 로,
 * 비즈니스 로직은 본체 모듈에 위임된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * RPC 흐름:
 *   [rpc.py / 클라이언트] → JSON-RPC over Unix socket
 *   → [SPDK RPC 서버 (lib/rpc)] → 메서드 디스패치
 *   → [본 파일: rpc_bdev_uring_create/rescan/delete]
 *   → [bdev_uring.c: create_uring_bdev / bdev_uring_rescan / delete_uring_bdev]
 *   → [lib/bdev → io_uring → Linux 커널]
 * 실행 컨텍스트: SPDK app thread (RPC 서버 폴러가 실행되는 thread). 모든 핸들러는
 * 동일한 app thread 에서 실행되므로 lockless.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/rpc.h: SPDK_RPC_REGISTER 매크로로 메서드 등록, JSON 응답 헬퍼 사용.
 * - spdk/jsonrpc.h: spdk_jsonrpc_request, spdk_jsonrpc_send_error_response 등.
 * - spdk/util.h: SPDK_COUNTOF, offsetof 매크로.
 * - bdev_uring.h: 본체 모듈의 공개 함수(create/delete/rescan) 와 bdev_uring_opts 구조.
 * - 데이터 흐름: JSON params 객체 → 디코더 테이블 → rpc_create_uring 구조체 →
 *   bdev_uring_opts 로 복사 → create_uring_bdev → 성공 시 JSON 응답에 bdev 이름 echo.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_bdev_uring_create: name/filename/block_size/uuid 파라미터로 새 bdev 생성.
 * - rpc_bdev_uring_rescan: 백킹 디바이스 크기 변경 인지.
 * - rpc_bdev_uring_delete: name 으로 bdev 비동기 unregister, 완료 콜백에서 응답.
 * - struct rpc_create_uring/rpc_rescan_uring/rpc_delete_uring: 각 RPC 의 입력 파라미터.
 * - free_rpc_*: 디코더가 strdup 한 문자열 메모리 해제 헬퍼.
 */

#include "bdev_uring.h"   /* [한국어] 모듈 본체의 공개 API — create_uring_bdev/delete_uring_bdev/bdev_uring_rescan/bdev_uring_opts/spdk_delete_uring_complete 선언. */
#include "spdk/rpc.h"     /* [한국어] SPDK_RPC_REGISTER, spdk_jsonrpc_send_*, spdk_jsonrpc_request 등 RPC 코어. */
#include "spdk/util.h"    /* [한국어] SPDK_COUNTOF (디코더 테이블 길이 산출) 등. */
#include "spdk/string.h"  /* [한국어] spdk_strerror — 음수 errno 를 사람이 읽을 수 있는 문자열로. */
#include "spdk/log.h"     /* [한국어] SPDK_ERRLOG — RPC 처리 중 발생 에러 로그. */

/* Structure to hold the parameters for this RPC method. */
/* [한국어] bdev_uring_create RPC 의 입력 파라미터 컨테이너. JSON 디코더가 채움. */
struct rpc_create_uring {
	char *name;
	/* [한국어] 새 bdev 의 SPDK 내부 식별자 (예: "Uring0"). 디코더가 strdup 한 메모리.
	 * 설정자: spdk_json_decode_string (필수 필드).
	 * 읽는 자: rpc_bdev_uring_create 가 opts.name 으로 전달.
	 * 값 범위: NULL 이면 디코드 실패. UTF-8 문자열.
	 * 동기화: app thread 내 단일 RPC 처리. */

	char *filename;
	/* [한국어] 백킹 디바이스/파일 경로 (예: "/dev/sda", "/var/tmp/foo.bin").
	 * 설정자/읽는 자: 위와 동일.
	 * 값 범위: 절대 또는 상대 경로 문자열. */

	uint32_t block_size;
	/* [한국어] 블록 크기 (옵션). 0 이면 OS 자동 검출.
	 * 설정자: spdk_json_decode_uint32 (optional=true).
	 * 읽는 자: create_uring_bdev 가 검증 후 bdev.blocklen 으로 사용.
	 * 값 범위: 512, 1024, 2048, 4096 등 2의 거듭제곱. 0 = autodetect. */

	struct spdk_uuid uuid;
	/* [한국어] bdev UUID (옵션). 디폴트 zero — create 가 zero 면 무시.
	 * 설정자: spdk_json_decode_uuid.
	 * 읽는 자: create_uring_bdev 가 zero 가 아니면 bdev.uuid 로 복사. */
};

/* Free the allocated memory resource after the RPC handling. */
/*
 * [한국어]
 * free_rpc_create_uring - 디코더가 할당한 strdup 메모리 해제.
 *
 * @r: 컨테이너 (스택 변수, 자체는 해제 안 함).
 *
 * uuid/block_size 같은 값 타입은 해제 불필요.
 */
static void
free_rpc_create_uring(struct rpc_create_uring *r)
{
	free(r->name);                                   /* [한국어] strdup 된 name. */
	free(r->filename);                               /* [한국어] strdup 된 filename. */
}

/* Structure to decode the input parameters for this RPC method. */
/* [한국어] JSON params → struct rpc_create_uring 매핑 디코더 테이블.
 * 각 엔트리: {JSON key, offsetof, 디코더 함수, optional 여부}. true = optional. */
static const struct spdk_json_object_decoder rpc_bdev_uring_create_decoders[] = {
	{"name", offsetof(struct rpc_create_uring, name), spdk_json_decode_string},                    /* [한국어] 필수. */
	{"filename", offsetof(struct rpc_create_uring, filename), spdk_json_decode_string},            /* [한국어] 필수. */
	{"block_size", offsetof(struct rpc_create_uring, block_size), spdk_json_decode_uint32, true},  /* [한국어] 옵션 — 누락 시 0 (auto). */
	{"uuid", offsetof(struct rpc_create_uring, uuid), spdk_json_decode_uuid, true},                /* [한국어] 옵션 — 누락 시 zero. */
};

/* Decode the parameters for this RPC method and properly create the uring
 * device. Error status returned in the failed cases.
 */
/*
 * [한국어]
 * rpc_bdev_uring_create - JSON-RPC bdev_uring_create 핸들러.
 *
 * @request: jsonrpc 요청 (응답을 보낼 채널).
 * @params: 입력 파라미터 JSON 객체.
 *
 * 흐름: params 디코드 → opts 구조체 구성 → create_uring_bdev 호출 → 성공 시 bdev 이름 echo.
 * 실패 시 SPDK_JSONRPC_ERROR_INTERNAL_ERROR 응답 후 cleanup.
 * 컨텍스트: app thread (RPC 서버 폴러).
 */
static void
rpc_bdev_uring_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_create_uring req = {};                                /* [한국어] 스택 컨테이너 zero init — 디코더가 채움. */
	struct spdk_json_write_ctx *w;                                   /* [한국어] 응답 JSON writer. */
	struct spdk_bdev *bdev;                                          /* [한국어] 생성된 bdev 포인터. */
	struct bdev_uring_opts opts = {};                                /* [한국어] 본체에 넘길 옵션 구조체. */

	if (spdk_json_decode_object(params, rpc_bdev_uring_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_uring_create_decoders),
				    &req)) {                             /* [한국어] 디코드 실패 — 필수 필드 누락/타입 불일치 등. */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	opts.block_size = req.block_size;                                /* [한국어] 본체 인터페이스로 복사. */
	opts.filename = req.filename;
	opts.name = req.name;
	opts.uuid = req.uuid;

	bdev = create_uring_bdev(&opts);                                 /* [한국어] 실제 모듈 본체 호출 — open/검증/등록 전부 여기서. */
	if (!bdev) {
		SPDK_ERRLOG("Unable to create URING bdev from file %s\n", req.filename);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to create URING bdev.");
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);                          /* [한국어] 성공 응답 시작 — result 필드 작성 준비. */
	spdk_json_write_string(w, req.name);                             /* [한국어] 결과값으로 생성된 bdev 이름 echo. */
	spdk_jsonrpc_end_result(request, w);                             /* [한국어] flush — 클라이언트에 전송. */

cleanup:
	free_rpc_create_uring(&req);                                     /* [한국어] 모든 경로에서 strdup 메모리 해제. */
}
/* [한국어] RPC 메서드 등록 — SPDK_RPC_RUNTIME 는 런타임(=app 시작 후) 단계에서 호출 허용 카테고리. */
SPDK_RPC_REGISTER("bdev_uring_create", rpc_bdev_uring_create, SPDK_RPC_RUNTIME)

/* [한국어] bdev_uring_rescan RPC 입력. name 한 개 필드. */
struct rpc_rescan_uring {
	char *name;
	/* [한국어] 재스캔 대상 bdev 이름.
	 * 설정자: spdk_json_decode_string.
	 * 읽는 자: bdev_uring_rescan.
	 * 값 범위: 등록된 uring bdev 이름. 아니면 -ENODEV 에러. */
};

/* [한국어] rescan 의 디코더 테이블 — name 필수. */
static const struct spdk_json_object_decoder rpc_bdev_uring_rescan_decoders[] = {
	{"name", offsetof(struct rpc_rescan_uring, name), spdk_json_decode_string},  /* [한국어] 필수. */
};

/*
 * [한국어]
 * rpc_bdev_uring_rescan - JSON-RPC bdev_uring_rescan 핸들러. 백킹 디바이스 크기 변경 인지.
 *
 * @request/@params: jsonrpc 표준.
 *
 * 본체 bdev_uring_rescan 을 호출. 성공 시 bool true 응답, 실패 시 errno 그대로 전달.
 * 컨텍스트: app thread.
 */
static void
rpc_bdev_uring_rescan(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_rescan_uring req = {NULL};                            /* [한국어] name=NULL 초기화. */
	int bdeverrno;                                                   /* [한국어] 본체 반환 에러. */

	if (spdk_json_decode_object(params, rpc_bdev_uring_rescan_decoders,
				    SPDK_COUNTOF(rpc_bdev_uring_rescan_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdeverrno = bdev_uring_rescan(req.name);                         /* [한국어] 본체 호출 — 동기 완료. */
	if (bdeverrno) {
		spdk_jsonrpc_send_error_response(request, bdeverrno,
						 spdk_strerror(-bdeverrno));  /* [한국어] errno 문자열화. */
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);                  /* [한국어] 성공 — true bool 응답. */
cleanup:
	free(req.name);                                                  /* [한국어] strdup 해제. */
}
SPDK_RPC_REGISTER("bdev_uring_rescan", rpc_bdev_uring_rescan, SPDK_RPC_RUNTIME)

/* [한국어] bdev_uring_delete RPC 입력. */
struct rpc_delete_uring {
	char *name;
	/* [한국어] 삭제할 bdev 이름.
	 * 설정자: spdk_json_decode_string.
	 * 읽는 자: delete_uring_bdev.
	 * 값 범위: 등록된 uring bdev 이름. */
};

/*
 * [한국어]
 * free_rpc_delete_uring - delete RPC 컨테이너 메모리 해제.
 */
static void
free_rpc_delete_uring(struct rpc_delete_uring *req)
{
	free(req->name);                                                 /* [한국어] strdup 해제. */
}

/* [한국어] delete 의 디코더 테이블. */
static const struct spdk_json_object_decoder rpc_bdev_uring_delete_decoders[] = {
	{"name", offsetof(struct rpc_delete_uring, name), spdk_json_decode_string},  /* [한국어] 필수. */
};

/*
 * [한국어]
 * _rpc_bdev_uring_delete_cb - delete_uring_bdev 의 비동기 완료 콜백.
 *
 * @cb_arg: jsonrpc_request* (delete 호출 시 박아둠).
 * @bdeverrno: unregister 결과.
 *
 * delete 는 unregister → 모든 채널 close → destruct 까지 비동기 — 본 콜백이 마지막에 발사.
 */
static void
_rpc_bdev_uring_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;                   /* [한국어] 원래 RPC 요청 핸들 복원. */

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);          /* [한국어] 성공. */
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));  /* [한국어] 실패 — errno 전달. */
	}

}

/*
 * [한국어]
 * rpc_bdev_uring_delete - JSON-RPC bdev_uring_delete 핸들러.
 *
 * delete_uring_bdev 호출 후 응답은 비동기 콜백에서 발사하므로 본 함수는 응답을 직접
 * 보내지 않는다 (decode 실패 시만 즉시 에러 응답). 컨텍스트: app thread.
 */
static void
rpc_bdev_uring_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_delete_uring req = {NULL};                            /* [한국어] 디코딩 컨테이너. */

	if (spdk_json_decode_object(params, rpc_bdev_uring_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_uring_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	delete_uring_bdev(req.name, _rpc_bdev_uring_delete_cb, request); /* [한국어] 비동기 시작 — 완료 콜백에서 응답 전송. */

cleanup:
	free_rpc_delete_uring(&req);                                     /* [한국어] req.name 해제 (delete_uring_bdev 내부에서 또 strdup 했을 것이므로 안전). */
}
SPDK_RPC_REGISTER("bdev_uring_delete", rpc_bdev_uring_delete, SPDK_RPC_RUNTIME)
