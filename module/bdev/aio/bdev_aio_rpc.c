/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] AIO bdev 모듈의 JSON-RPC 핸들러 (bdev_aio_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK의 JSON-RPC를 통해 "AIO bdev"(Linux libaio 기반 비동기
 * I/O bdev)를 생성/리스캔/삭제하기 위한 세 RPC 메서드 핸들러를 등록·구현한다.
 *   - "bdev_aio_create" : 일반 파일이나 블록 디바이스 경로를 받아 AIO bdev로 만든다.
 *                         spdk_bdev_wait_for_examine 콜백이 호출된 뒤에야 응답한다.
 *   - "bdev_aio_rescan" : 백엔드 디바이스 크기/속성 변경을 다시 검출한다 (동기).
 *   - "bdev_aio_delete" : 비동기로 AIO bdev를 unregister한다.
 * AIO bdev는 io_submit/io_getevents 시스템 호출(libaio)을 폴링 모드로 사용해
 * 일반 파일이나 /dev/sdX 등을 SPDK bdev 트리에 노출시키는 모듈이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (생성, 비동기):
 *   [JSON-RPC 클라이언트] → SPDK JSON-RPC 서버
 *     → rpc_bdev_aio_create()  ← (이 파일)
 *       → spdk_json_decode_object()
 *       → create_aio_bdev()       (bdev_aio.c)
 *         → io_setup() / open() / spdk_bdev_register()
 *       → spdk_bdev_wait_for_examine(rpc_bdev_aio_create_cb, ctx)
 *         → ... examine 비동기 ... → rpc_bdev_aio_create_cb()
 *           → spdk_jsonrpc_begin_result() / end_result()
 * 호출 체인 (삭제, 비동기):
 *   → rpc_bdev_aio_delete() → bdev_aio_delete()
 *     → spdk_bdev_unregister_by_name() → 비동기 → _rpc_bdev_aio_delete_cb()
 * 실행 컨텍스트: SPDK JSON-RPC 서버 스레드 (보통 마스터 reactor).
 *
 * === 타 모듈과의 연결 ===
 * - 의존: bdev_aio.h (create_aio_bdev, bdev_aio_delete, bdev_aio_rescan,
 *   spdk_delete_aio_complete 콜백 타입), spdk/rpc.h, spdk/util.h, spdk/string.h,
 *   spdk/log.h.
 * - 데이터 흐름: 클라이언트 JSON → rpc_construct_aio → create_aio_bdev() →
 *   bdev 코어 등록 → wait_for_examine 콜백에서 응답 송신.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct rpc_construct_aio       : create RPC의 입력 필드 묶음.
 * - struct rpc_construct_aio_ctx   : create는 비동기이므로 RPC request와 입력을
 *   힙에 보존해야 한다. ctx가 그 컨테이너다.
 * - rpc_bdev_aio_create_decoders[] : create의 JSON 디코더 표.
 * - rpc_bdev_aio_create_cb()       : examine 완료 후 응답 송신 + 메모리 해제.
 * - rpc_bdev_aio_create()          : create RPC 진입점.
 * - rpc_bdev_aio_rescan()          : rescan RPC 진입점 (동기).
 * - struct rpc_delete_aio + 관련 : 삭제 RPC 처리.
 */

#include "bdev_aio.h"
/* [한국어] 같은 모듈의 내부 헤더. create_aio_bdev / bdev_aio_delete /
 *           bdev_aio_rescan / spdk_delete_aio_complete 선언. */
#include "spdk/rpc.h"
/* [한국어] SPDK JSON-RPC 서버 API. SPDK_RPC_REGISTER 매크로 포함. */
#include "spdk/util.h"
/* [한국어] SPDK_COUNTOF 등 유틸. */
#include "spdk/string.h"
/* [한국어] spdk_strerror — errno → 메시지 변환. */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG/SPDK_DEBUGLOG 매크로. */

struct rpc_construct_aio {
	/* [한국어] "bdev_aio_create" RPC의 입력 필드 묶음. JSON 디코더가 직접 채운다. */

	char *name;
	/* [한국어] 생성될 AIO bdev의 이름. 필수.
	 * 설정자: 디코더(strdup). 읽는 자: create_aio_bdev → bdev->name. */

	char *filename;
	/* [한국어] 백엔드 파일/디바이스 경로 (예: "/dev/sdb", "/var/tmp/disk.img"). 필수.
	 * 설정자: 디코더(strdup). 읽는 자: create_aio_bdev에서 open(2) 대상.
	 * 동기화: 한 번 설정되면 변경되지 않음. */

	uint32_t block_size;
	/* [한국어] 블록 크기 바이트 (선택). 0이면 백엔드의 기본값 사용
	 *           (블록 디바이스는 BLKSSZGET, 일반 파일은 4096). */

	bool readonly;
	/* [한국어] true면 read-only 모드로 open (O_RDONLY). 기본 false. */

	bool fallocate;
	/* [한국어] true면 UNMAP/WRITE_ZEROES를 fallocate(PUNCH_HOLE) 호출로 매핑.
	 *           파일 시스템이 지원해야 함. 블록 디바이스에는 무관. */

	struct spdk_uuid uuid;
	/* [한국어] bdev UUID (선택). 0이면 자동 할당. */

	bool nowait;
	/* [한국어] true면 open 시 O_NONBLOCK과 유사 동작 (백엔드가 차단되지 않도록). */
};

struct rpc_construct_aio_ctx {
	/* [한국어] create는 spdk_bdev_wait_for_examine 콜백이 끝난 뒤 응답을 보내야 하는
	 *           비동기 처리이므로, 입력과 RPC request를 힙에 보존해 콜백까지 살려둔다. */

	struct rpc_construct_aio req;
	/* [한국어] 디코드된 입력. examine 콜백에서 name을 응답에 사용하려면 살아 있어야 함. */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 콜백에서 응답을 보낼 때 필요한 RPC 요청 핸들. */
};

/*
 * [한국어]
 * free_rpc_construct_aio - rpc_construct_aio_ctx 전체 해제 (이름/파일명 포함).
 *
 * @ctx: calloc으로 잡은 컨텍스트.
 * @return: 없음.
 *
 * 호출 체인:
 *   rpc_bdev_aio_create_cb() (성공 종료) → [이 함수]
 *   rpc_bdev_aio_create()    (디코드/생성 실패) → [이 함수]
 */
static void
free_rpc_construct_aio(struct rpc_construct_aio_ctx *ctx)
{
	free(ctx->req.name);
	/* [한국어] 디코더가 strdup한 이름 해제. */
	free(ctx->req.filename);
	/* [한국어] 파일/디바이스 경로 해제. */
	free(ctx);
	/* [한국어] 컨텍스트 자체 해제 (calloc된 블록). */
}

static const struct spdk_json_object_decoder rpc_bdev_aio_create_decoders[] = {
	/* [한국어] "bdev_aio_create" 입력 디코더 표. name/filename 필수, 나머지 선택. */
	{"name", offsetof(struct rpc_construct_aio, name), spdk_json_decode_string},
	/* [한국어] bdev 이름 (필수). */
	{"filename", offsetof(struct rpc_construct_aio, filename), spdk_json_decode_string},
	/* [한국어] 백엔드 파일/디바이스 경로 (필수). */
	{"block_size", offsetof(struct rpc_construct_aio, block_size), spdk_json_decode_uint32, true},
	/* [한국어] 블록 크기 (선택, 0이면 자동). */
	{"readonly", offsetof(struct rpc_construct_aio, readonly), spdk_json_decode_bool, true},
	/* [한국어] 읽기 전용 (선택, 기본 false). */
	{"fallocate", offsetof(struct rpc_construct_aio, fallocate), spdk_json_decode_bool, true},
	/* [한국어] UNMAP을 fallocate로 매핑 (선택, 기본 false). */
	{"uuid", offsetof(struct rpc_construct_aio, uuid), spdk_json_decode_uuid, true},
	/* [한국어] UUID (선택). */
	{"nowait", offsetof(struct rpc_construct_aio, nowait), spdk_json_decode_bool, true},
	/* [한국어] open 차단 회피 옵션 (선택). */
};

/*
 * [한국어]
 * rpc_bdev_aio_create_cb - spdk_bdev_wait_for_examine 비동기 완료 콜백.
 *
 * @cb_arg: rpc_bdev_aio_create()에서 등록한 rpc_construct_aio_ctx*.
 * @return: 없음.
 *
 * 동기: AIO bdev가 등록된 직후, bdev 코어의 모듈 examine 단계가 모두 끝날 때까지
 *       기다린 다음 클라이언트에 응답을 보내야 한다 (등록만 했다가 바로 응답하면
 *       lvol/raid 같은 상위 모듈이 아직 초기화 안 된 상태로 보일 수 있음).
 *       wait_for_examine은 모든 examine이 끝나면 이 콜백을 호출한다.
 *
 * 실행 컨텍스트: bdev 코어가 examine을 끝낸 SPDK thread (보통 마스터 reactor).
 *
 * 호출 체인:
 *   rpc_bdev_aio_create() → spdk_bdev_wait_for_examine(this) → [이 함수]
 *     → spdk_jsonrpc_begin_result() / end_result()
 *     → free_rpc_construct_aio()
 */
static void
rpc_bdev_aio_create_cb(void *cb_arg)
{
	struct rpc_construct_aio_ctx *ctx = cb_arg;
	/* [한국어] cb_arg에서 컨텍스트 복원. */
	struct spdk_jsonrpc_request *request = ctx->request;
	/* [한국어] RPC 요청 핸들. 응답 송신에 필요. */
	struct spdk_json_write_ctx *w;
	/* [한국어] 응답 본문 작성용 라이터. */

	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] result 본문 시작. */
	spdk_json_write_string(w, ctx->req.name);
	/* [한국어] 새 bdev의 이름을 result로 송신. 클라이언트가 이후 식별자로 사용. */
	spdk_jsonrpc_end_result(request, w);
	/* [한국어] 응답 송신 종료. */
	free_rpc_construct_aio(ctx);
	/* [한국어] 컨텍스트(이름/파일명/구조체) 전체 해제. */
}

/*
 * [한국어]
 * rpc_bdev_aio_create - "bdev_aio_create" JSON-RPC 메서드 핸들러.
 *
 * @request: RPC 요청.
 * @params : 클라이언트 JSON params.
 * @return : 없음. 비동기 — 응답은 rpc_bdev_aio_create_cb에서 송신.
 *
 * 동작 단계:
 *   1) ctx를 calloc으로 할당 (콜백까지 살아 있어야 하는 비동기 처리).
 *   2) JSON params → ctx->req 디코딩. 실패 시 즉시 에러 응답 후 ctx 해제.
 *   3) ctx->request 저장.
 *   4) create_aio_bdev() 동기 호출 — io_setup/open/register까지.
 *   5) 성공 시 spdk_bdev_wait_for_examine 등록 → 콜백에서 응답.
 *      실패 시 즉시 에러 응답 + ctx 해제.
 *
 * 실행 컨텍스트: SPDK JSON-RPC 서버 스레드.
 *
 * 호출 체인:
 *   spdk_jsonrpc_server_handle_req() → [이 함수]
 *     → calloc → spdk_json_decode_object → create_aio_bdev
 *     → spdk_bdev_wait_for_examine → 비동기 → rpc_bdev_aio_create_cb
 */
static void
rpc_bdev_aio_create(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_construct_aio_ctx *ctx;
	/* [한국어] 비동기 처리에 사용할 힙 컨텍스트 포인터. */
	int rc;
	/* [한국어] create_aio_bdev의 반환 코드. */

	ctx = calloc(1, sizeof(*ctx));
	/* [한국어] 0으로 초기화된 ctx 할당. 이후 examine 콜백까지 살아남아야 한다. */
	if (!ctx) {
		/* [한국어] 메모리 부족 시 즉시 에러 응답. */
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_aio_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_aio_create_decoders),
				    &ctx->req)) {
		/* [한국어] JSON params를 ctx->req로 디코딩. 실패 시 if 진입. */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		/* [한국어] 디버그 빌드뿐 아니라 항상 출력되는 에러 로그. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		free_rpc_construct_aio(ctx);
		/* [한국어] 부분 디코드된 ctx 정리 후 즉시 반환. */
		return;
	}

	ctx->request = request;
	/* [한국어] examine 콜백에서 응답을 보내려면 request 핸들을 보존해둔다. */
	rc = create_aio_bdev(ctx->req.name, ctx->req.filename, ctx->req.block_size,
			     ctx->req.readonly, ctx->req.fallocate, &ctx->req.uuid, ctx->req.nowait);
	/* [한국어] 핵심 호출: 일반 파일/블록 디바이스를 열고 io_setup으로 AIO 컨텍스트 생성,
	 *           spdk_bdev_register까지 동기 수행. 실패 시 -errno 반환. */
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		/* [한국어] 실패 응답 송신. */
		free_rpc_construct_aio(ctx);
		/* [한국어] 콜백이 호출되지 않으므로 여기서 ctx 해제. */
		return;
	}

	spdk_bdev_wait_for_examine(rpc_bdev_aio_create_cb, ctx);
	/* [한국어] 모든 모듈의 examine 단계가 끝나길 기다린 뒤 콜백 호출.
	 *           클라이언트는 이 콜백 시점에야 "이 bdev가 완전히 사용 가능"으로 보장된다. */
}
SPDK_RPC_REGISTER("bdev_aio_create", rpc_bdev_aio_create, SPDK_RPC_RUNTIME)
/* [한국어] create 메서드 등록. RUNTIME 상태에서만 호출 가능. */

struct rpc_rescan_aio {
	/* [한국어] "bdev_aio_rescan" 입력. 이름 1개. */
	char *name;
	/* [한국어] 다시 스캔할 AIO bdev 이름.
	 * 설정자: 디코더. 읽는 자: bdev_aio_rescan. */
};

static const struct spdk_json_object_decoder rpc_bdev_aio_rescan_decoders[] = {
	/* [한국어] rescan 디코더 표. name 필수. */
	{"name", offsetof(struct rpc_rescan_aio, name), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_aio_rescan - "bdev_aio_rescan" 핸들러 (동기).
 *
 * @request: RPC 요청.
 * @params : {"name": "AIO0"}.
 * @return : 없음. 동기적으로 응답.
 *
 * 동작: 백엔드 파일/디바이스 크기 변경(리사이즈) 등을 다시 읽어
 *       bdev->blockcnt를 갱신한다. 블록 디바이스가 외부에서 리사이즈된 경우 사용.
 *
 * 실행 컨텍스트: SPDK JSON-RPC 서버 스레드.
 */
static void
rpc_bdev_aio_rescan(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_rescan_aio req = {NULL};
	/* [한국어] name=NULL 초기화. */
	int bdeverrno;
	/* [한국어] bdev_aio_rescan의 반환 코드 (0=성공, 음수=-errno). */

	if (spdk_json_decode_object(params, rpc_bdev_aio_rescan_decoders,
				    SPDK_COUNTOF(rpc_bdev_aio_rescan_decoders),
				    &req)) {
		/* [한국어] JSON 디코드. 실패 시 INTERNAL_ERROR. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdeverrno = bdev_aio_rescan(req.name);
	/* [한국어] 동기 rescan. 대상이 없거나 다른 모듈의 bdev면 음수 반환. */
	if (bdeverrno) {
		spdk_jsonrpc_send_error_response(request, bdeverrno,
						 spdk_strerror(-bdeverrno));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	/* [한국어] 성공 응답. */
cleanup:
	free(req.name);
	/* [한국어] strdup된 name 해제. NULL 안전. */
}
SPDK_RPC_REGISTER("bdev_aio_rescan", rpc_bdev_aio_rescan, SPDK_RPC_RUNTIME)
/* [한국어] rescan 메서드 등록. */

struct rpc_delete_aio {
	/* [한국어] "bdev_aio_delete" 입력. 이름 1개. */
	char *name;
	/* [한국어] 삭제 대상 bdev 이름.
	 * 설정자: 디코더(strdup). 읽는 자: bdev_aio_delete. */
};

/*
 * [한국어]
 * free_rpc_delete_aio - rpc_delete_aio.name 해제 헬퍼.
 *
 * @r: 정리할 구조체.
 * @return: 없음.
 */
static void
free_rpc_delete_aio(struct rpc_delete_aio *r)
{
	free(r->name);
	/* [한국어] strdup된 name 해제. */
}

static const struct spdk_json_object_decoder rpc_bdev_aio_delete_decoders[] = {
	/* [한국어] delete 디코더 표. name 필수. */
	{"name", offsetof(struct rpc_delete_aio, name), spdk_json_decode_string},
};

/*
 * [한국어]
 * _rpc_bdev_aio_delete_cb - bdev_aio_delete의 비동기 완료 콜백.
 *
 * @cb_arg   : delete 호출 시 넘긴 spdk_jsonrpc_request*.
 * @bdeverrno: 0=성공, 음수=-errno.
 * @return   : 없음.
 *
 * 동기: bdev 코어가 unregister + bdev_aio destruct를 끝낸 시점에 호출.
 *       fd close, io_destroy 등이 모두 끝났음을 보장.
 *
 * 실행 컨텍스트: bdev 코어가 unregister를 완료한 SPDK thread.
 *
 * 호출 체인:
 *   bdev_aio_delete() → spdk_bdev_unregister_by_name() → 비동기 →
 *     bdev_aio_destruct() → [이 함수] → spdk_jsonrpc_send_*_response()
 */
static void
_rpc_bdev_aio_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	/* [한국어] cb_arg에서 RPC 요청 복원. */

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
		/* [한국어] 성공 — true 응답. */
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
		/* [한국어] 실패 — errno + 메시지 응답. */
	}
}

/*
 * [한국어]
 * rpc_bdev_aio_delete - "bdev_aio_delete" RPC 핸들러.
 *
 * @request: RPC 요청. 콜백 cb_arg로 그대로 전달.
 * @params : {"name": "AIO0"}.
 * @return : 없음. 결과는 비동기 콜백으로 통지.
 *
 * 호출 체인:
 *   spdk_jsonrpc_server_handle_req() → [이 함수]
 *     → spdk_json_decode_object()
 *     → bdev_aio_delete() → 비동기 → _rpc_bdev_aio_delete_cb()
 */
static void
rpc_bdev_aio_delete(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_delete_aio req = {NULL};
	/* [한국어] name=NULL 초기화. */

	if (spdk_json_decode_object(params, rpc_bdev_aio_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_aio_delete_decoders),
				    &req)) {
		/* [한국어] 디코드 실패 시 INTERNAL_ERROR. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev_aio_delete(req.name, _rpc_bdev_aio_delete_cb, request);
	/* [한국어] 비동기 삭제 시작. 즉시 반환되고 결과는 콜백으로. */

cleanup:
	free_rpc_delete_aio(&req);
	/* [한국어] req.name 해제. delete가 내부 lookup 후 더 이상 참조하지 않으므로 안전. */
}
SPDK_RPC_REGISTER("bdev_aio_delete", rpc_bdev_aio_delete, SPDK_RPC_RUNTIME)
/* [한국어] delete 메서드 등록. */
