/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI bdev 모듈의 RPC 핸들러 (bdev_iscsi_rpc.c)
 *
 * === 파일의 역할 ===
 * bdev_iscsi 모듈의 JSON-RPC 인터페이스를 정의한다. 사용자가 SPDK 의 rpc.py 또는
 * 직접 JSON-RPC 클라이언트로 보낼 수 있는 세 가지 명령 — bdev_iscsi_set_options,
 * bdev_iscsi_create, bdev_iscsi_delete — 의 파라미터를 파싱하고, bdev_iscsi.c 의
 * 공개 API (bdev_iscsi_set_opts, create_iscsi_disk, delete_iscsi_disk) 를 호출한
 * 뒤 응답을 작성한다. JSON 디코더 테이블, 응답 콜백, 메모리 정리까지 모두 본 파일이
 * 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   JSON-RPC client (rpc.py 등) → SPDK JSON-RPC 서버 (lib/jsonrpc) →
 *   [이 파일의 RPC 핸들러] → bdev_iscsi.c 의 공개 API → libiscsi → TCP/iSCSI target.
 * 모든 RPC 호출은 RPC 처리 스레드 (보통 spdk_app 의 main reactor) 에서 동기적으로
 * 디코딩되지만, bdev_iscsi_create 는 비동기로 동작하므로 응답은 콜백(bdev_iscsi_create_cb)
 * 에서 추후 작성된다 (spdk_jsonrpc_request* 가 그 동안 살아있어야 함).
 *
 * === 타 모듈과의 연결 ===
 * - bdev_iscsi.h: bdev_iscsi_get_opts / bdev_iscsi_set_opts / create_iscsi_disk /
 *   delete_iscsi_disk / spdk_bdev_iscsi_opts 노출.
 * - lib/jsonrpc (spdk/rpc.h, spdk/jsonrpc.h): SPDK_RPC_REGISTER 매크로로 핸들러 등록,
 *   spdk_jsonrpc_send_* 로 응답.
 * - lib/json (spdk/json.h): JSON decoder (spdk_json_decode_object, spdk_json_decode_string,
 *   spdk_json_decode_uint64) 사용.
 * - lib/bdev: spdk_bdev_get_name 으로 응답에 bdev 이름 포함.
 * 데이터 흐름: JSON request → spdk_json_val* → decoder 테이블 통해 C 구조체로 변환 →
 * bdev_iscsi.c API → 비동기 동작 → 콜백에서 JSON 응답 작성.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_bdev_iscsi_set_options(): timeout_sec 옵션 갱신.
 * - rpc_bdev_iscsi_create(): name/initiator_iqn/url 받아 create_iscsi_disk() 호출.
 *   완료 콜백 bdev_iscsi_create_cb 가 응답 작성.
 * - rpc_bdev_iscsi_delete(): name 받아 delete_iscsi_disk() 호출. 완료 콜백
 *   rpc_bdev_iscsi_delete_cb 가 응답 작성.
 * - struct rpc_bdev_iscsi_create / rpc_delete_iscsi: 디코드 결과 임시 보관 구조체.
 *   디코드 후 bdev_iscsi.c 의 API 가 자체적으로 strdup 하므로 RPC 핸들러는 호출
 *   직후 free_rpc_* 로 해제 가능.
 */

#include "bdev_iscsi.h"           /* [한국어] 본 모듈의 공개 인터페이스 — set_opts/create/delete 선언. */
#include "spdk/rpc.h"             /* [한국어] SPDK_RPC_REGISTER 매크로, RPC enable 단계 상수. */
#include "spdk/util.h"            /* [한국어] SPDK_COUNTOF 매크로. */
#include "spdk/string.h"          /* [한국어] spdk_strerror 등. */

#include "spdk/log.h"             /* [한국어] SPDK_ERRLOG. */

/* [한국어] bdev_iscsi_set_options RPC 의 JSON decoder 테이블.
 * 각 엔트리: { JSON 키, 구조체 내 offset, 디코더 함수, optional 여부 }.
 * optional=true → 키가 없어도 에러 아님 (기존 값 유지). */
static const struct spdk_json_object_decoder rpc_bdev_iscsi_set_options_decoders[] = {
	{"timeout_sec", offsetof(struct spdk_bdev_iscsi_opts, timeout_sec), spdk_json_decode_uint64, true},
	/* [한국어] timeout_sec — UINT64, optional. 없으면 기존 g_opts.timeout_sec 그대로. */
};

/*
 * [한국어]
 * rpc_bdev_iscsi_set_options - bdev_iscsi_set_options RPC 핸들러.
 *
 * @request: spdk_jsonrpc_request — 응답 작성에 사용.
 * @params: JSON params 객체 (NULL 허용).
 *
 * 흐름:
 *   1. 현재 옵션을 bdev_iscsi_get_opts 로 가져옴 (필드 누락 시 기존 값 보존).
 *   2. params 가 있으면 decoder 로 덮어쓰기.
 *   3. bdev_iscsi_set_opts 호출 — EPERM 이면 "이미 연결 중" 에러 메시지 사용.
 *   4. 성공 시 true 응답.
 *
 * 실행 컨텍스트: RPC 서버 스레드 (보통 main reactor). 동기 처리 — 호출 직후 응답 작성.
 */
static void
rpc_bdev_iscsi_set_options(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct spdk_bdev_iscsi_opts opts;          /* [한국어] 디코드 대상 구조체. 기존 값 베이스 + 덮어쓰기. */
	int rc;

	bdev_iscsi_get_opts(&opts);                 /* [한국어] 현재 g_opts 복사 — optional 필드 보존. */
	if (params && spdk_json_decode_object(params, rpc_bdev_iscsi_set_options_decoders,
					      SPDK_COUNTOF(rpc_bdev_iscsi_set_options_decoders),
					      &opts)) {
		/* [한국어] 디코드 실패 — JSON 형식 오류 또는 타입 불일치. */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = bdev_iscsi_set_opts(&opts);            /* [한국어] 실제 옵션 적용 — 현재 항상 0 반환. */
	if (rc == -EPERM) {
		/* [한국어] 향후 "이미 LUN 이 connect 된 상태" 등 거절 케이스용 분기 (현재는 미사용). */
		spdk_jsonrpc_send_error_response(request, -EPERM,
						 "RPC not permitted with iscsi already connected");
	} else if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));  /* [한국어] 그 외 에러. */
	} else {
		spdk_jsonrpc_send_bool_response(request, true);                     /* [한국어] 성공 — { "result": true }. */
	}

	return;
}
SPDK_RPC_REGISTER("bdev_iscsi_set_options", rpc_bdev_iscsi_set_options,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
/* [한국어] RPC 등록 매크로 — constructor 단계에서 SPDK JSON-RPC 서버 디스패처에 추가.
 * STARTUP|RUNTIME → spdk_app 시작 직후(initial config 로딩 단계)와 운영 중 모두 허용. */

/* [한국어] bdev_iscsi_create RPC 파라미터 — 디코더가 strdup 으로 채움. 디코드 후
 * create_iscsi_disk() 가 자체적으로 다시 strdup 하므로 호출 직후 free 안전. */
struct rpc_bdev_iscsi_create {
	char *name;
	/* [한국어] 생성할 bdev 이름.
	 * 설정자: spdk_json_decode_string (strdup).
	 * 읽는 자: create_iscsi_disk 의 bdev_name 인자.
	 * 동기화: 단일 RPC 호출 동안만 사용 — race 없음. */

	char *initiator_iqn;
	/* [한국어] iSCSI initiator IQN — 본 SPDK 인스턴스의 식별자.
	 * 설정자: 디코더. 읽는 자: create_iscsi_disk. */

	char *url;
	/* [한국어] iSCSI URL — host/target/lun + 선택적 user:passwd.
	 * 형식: iscsi://[user:passwd@]host[:port]/target-iqn/lun-id. */
};

/* [한국어] bdev_iscsi_create RPC 의 디코더 테이블. optional 누락 → 모두 필수. */
static const struct spdk_json_object_decoder rpc_bdev_iscsi_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_iscsi_create, name), spdk_json_decode_string},                /* [한국어] bdev 이름 필수. */
	{"initiator_iqn", offsetof(struct rpc_bdev_iscsi_create, initiator_iqn), spdk_json_decode_string}, /* [한국어] IQN 필수. */
	{"url", offsetof(struct rpc_bdev_iscsi_create, url), spdk_json_decode_string},                   /* [한국어] URL 필수. */
};

/*
 * [한국어]
 * free_rpc_bdev_iscsi_create - 디코더가 strdup 한 문자열들 해제.
 *
 * @req: 해제할 구조체. 각 필드는 NULL 일 수 있음 (free(NULL) 은 안전).
 */
static void
free_rpc_bdev_iscsi_create(struct rpc_bdev_iscsi_create *req)
{
	free(req->name);
	free(req->initiator_iqn);
	free(req->url);
}

/*
 * [한국어]
 * bdev_iscsi_create_cb - 비동기 create 완료 콜백. RPC 응답 작성.
 *
 * @cb_arg: spdk_jsonrpc_request* — create_iscsi_disk 호출 시 RPC 요청을 그대로 전달.
 * @bdev: 생성된 bdev (성공 시), NULL (실패).
 * @status: 0=성공, >0=SCSI status (iSCSI 응답 오류), <0=음수 errno.
 *
 * status 의 부호에 따라 다른 종류의 에러 응답을 작성한다. 성공 시 결과로 bdev 이름을
 * JSON 문자열로 반환. 본 콜백 실행 시점에는 conn_req 가 이미 해제되어 있으므로
 * bdev 포인터로부터 이름을 가져온다.
 */
static void
bdev_iscsi_create_cb(void *cb_arg, struct spdk_bdev *bdev, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;       /* [한국어] 원래 RPC 요청 핸들 복원. */
	struct spdk_json_write_ctx *w;

	if (status > 0) {
		/* [한국어] >0 = iSCSI/SCSI 레벨 에러 (CHECK_CONDITION 등). 사용자가 잘못된
		 * URL/IQN 을 보냈을 가능성 → INVALID_PARAMS. */
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "iSCSI error (%d).", status);
	} else if (status < 0) {
		/* [한국어] <0 = 음수 errno (-ENOMEM, -EINVAL 등). spdk_strerror 로 메시지 변환. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(-status));
	} else {
		/* [한국어] 성공 — { "result": "<bdev_name>" }. */
		w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_string(w, spdk_bdev_get_name(bdev));
		spdk_jsonrpc_end_result(request, w);
	}
}

/*
 * [한국어]
 * rpc_bdev_iscsi_create - bdev_iscsi_create RPC 디스패처.
 *
 * @request: RPC 요청.
 * @params: JSON params (NULL 이면 디코드 실패).
 *
 * 디코드 → create_iscsi_disk 호출 → 비동기 진행. RPC 응답은 bdev_iscsi_create_cb
 * 에서 추후 작성 (request 가 그 동안 살아있음 — SPDK JSON-RPC 가 보장).
 * create_iscsi_disk 가 즉시 음수 리턴하면 (예: -EINVAL, -ENOMEM) 동기 에러 응답.
 */
static void
rpc_bdev_iscsi_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_iscsi_create req = {};       /* [한국어] zero-init — free_rpc_* 가 free(NULL) 안전하게 처리. */
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_iscsi_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_iscsi_create_decoders),
				    &req)) {
		/* [한국어] 디코드 실패 — 필수 키 누락/타입 오류. */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] 비동기 시작. 완료는 bdev_iscsi_create_cb 에서. */
	rc = create_iscsi_disk(req.name, req.url, req.initiator_iqn, bdev_iscsi_create_cb, request);
	if (rc) {
		/* [한국어] 시작 자체가 실패 — 즉시 동기 에러 응답. cb_fn 호출되지 않음. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}

cleanup:
	free_rpc_bdev_iscsi_create(&req);            /* [한국어] req 의 문자열들 해제 — create_iscsi_disk 가 이미 복사함. */
}
SPDK_RPC_REGISTER("bdev_iscsi_create", rpc_bdev_iscsi_create, SPDK_RPC_RUNTIME)
/* [한국어] RPC 등록 — RUNTIME 만 (STARTUP 단계에는 reactor 미준비) — 운영 중 dynamic add. */

/* [한국어] bdev_iscsi_delete RPC 파라미터. name 만 필요. */
struct rpc_delete_iscsi {
	char *name;
	/* [한국어] 삭제할 bdev 이름. 디코더가 strdup, free_rpc_delete_iscsi 가 free. */
};

/*
 * [한국어]
 * free_rpc_delete_iscsi - delete RPC 파라미터의 strdup 문자열 해제.
 */
static void
free_rpc_delete_iscsi(struct rpc_delete_iscsi *r)
{
	free(r->name);
}

/* [한국어] bdev_iscsi_delete 디코더 테이블. name 필수. */
static const struct spdk_json_object_decoder rpc_bdev_iscsi_delete_decoders[] = {
	{"name", offsetof(struct rpc_delete_iscsi, name), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_iscsi_delete_cb - 비동기 delete 완료 콜백.
 *
 * @cb_arg: spdk_jsonrpc_request*.
 * @bdeverrno: 0=성공, 음수=실패 (-ENODEV 등).
 *
 * unregister 가 모든 채널 정리 + bdev_iscsi_destruct 완료 후 호출.
 */
static void
rpc_bdev_iscsi_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);                                /* [한국어] 성공 → { "result": true }. */
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno)); /* [한국어] 실패 — errno 메시지. */
	}
}

/*
 * [한국어]
 * rpc_bdev_iscsi_delete - bdev_iscsi_delete RPC 디스패처.
 *
 * @request: RPC 요청.
 * @params: JSON params.
 *
 * 이름만 디코드 → delete_iscsi_disk 호출. 비동기로 unregister 진행되며 완료는
 * rpc_bdev_iscsi_delete_cb 에서.
 */
static void
rpc_bdev_iscsi_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_delete_iscsi req = {NULL};         /* [한국어] name 만 NULL 로 초기화. */

	if (spdk_json_decode_object(params, rpc_bdev_iscsi_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_iscsi_delete_decoders),
				    &req)) {
		/* [한국어] 디코드 실패 — name 키 누락 등. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	delete_iscsi_disk(req.name, rpc_bdev_iscsi_delete_cb, request);  /* [한국어] 비동기 unregister 시작. */

cleanup:
	free_rpc_delete_iscsi(&req);                  /* [한국어] strdup name 해제 — delete_iscsi_disk 내부에서 이름 lookup 시 사용 후 보존 불필요. */
}
SPDK_RPC_REGISTER("bdev_iscsi_delete", rpc_bdev_iscsi_delete, SPDK_RPC_RUNTIME)
/* [한국어] RPC 등록 — RUNTIME 만. dynamic remove. */
