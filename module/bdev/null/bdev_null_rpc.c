/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 */

/*
 * [한국어 설명] null bdev 모듈의 JSON-RPC 핸들러 (bdev_null_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK의 JSON-RPC 인터페이스를 통해 "null bdev"를 생성/삭제/리사이즈
 * 할 수 있도록 세 개의 RPC 메서드("bdev_null_create", "bdev_null_delete",
 * "bdev_null_resize")를 등록하고 구현한다. null bdev는 백엔드 메모리도
 * 사용하지 않고, 모든 쓰기를 버리고 모든 읽기에 0(미리 할당된 단일
 * read 버퍼 g_null_read_buf)을 반환하는 가상 디스크다. 이는 bdev 레이어
 * 자체의 처리 오버헤드를 측정하기 위한 벤치마크/테스트 용도다.
 * 이 파일은 클라이언트 JSON 입력을 `null_bdev_opts`/`rpc_delete_null`/
 * `rpc_bdev_null_resize`로 디코드한 후, bdev_null.c가 노출하는 API
 * (bdev_null_create / bdev_null_delete / bdev_null_resize)를 호출하고
 * 응답을 송신한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (생성):
 *   [JSON-RPC 클라이언트] → SPDK JSON-RPC 서버
 *     → rpc_bdev_null_create()  ← (이 파일)
 *       → spdk_json_decode_object()
 *       → bdev_null_create()     (bdev_null.c)
 *         → spdk_bdev_register()
 *       → spdk_jsonrpc_*_response()
 * 호출 체인 (삭제, 비동기):
 *   → rpc_bdev_null_delete() → bdev_null_delete()
 *     → spdk_bdev_unregister_by_name() → 비동기 → rpc_bdev_null_delete_cb()
 * 호출 체인 (리사이즈, 동기):
 *   → rpc_bdev_null_resize() → bdev_null_resize()
 *     → spdk_bdev_open_ext + spdk_bdev_notify_blockcnt_change → 응답
 * 실행 컨텍스트: SPDK JSON-RPC 서버 스레드 (보통 마스터 reactor 1개).
 *
 * === 타 모듈과의 연결 ===
 * - 의존: bdev_null.h (옵션 구조체, create/delete/resize 함수, 콜백 타입),
 *   spdk/rpc.h (RPC 등록/응답 송신), spdk/util.h (SPDK_COUNTOF),
 *   spdk/string.h (spdk_strerror), spdk/bdev_module.h (bdev->name 접근),
 *   spdk/log.h (로깅 매크로).
 * - 등록 효과: 파일 끝의 SPDK_RPC_REGISTER가 빌드 시점에 RPC 메서드 테이블에
 *   세 메서드를 추가한다.
 * - 데이터 흐름: 클라이언트 JSON → 디코드 → bdev_null.c API → bdev 코어 →
 *   응답 송신.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_bdev_null_create_decoders[] / rpc_bdev_null_create() : 생성 RPC 처리.
 * - struct rpc_delete_null + 관련 디코더/핸들러/콜백 : 비동기 삭제 처리.
 * - struct rpc_bdev_null_resize + 디코더/핸들러         : 동기 리사이즈 처리.
 * - free_rpc_construct_null / free_rpc_delete_null / free_rpc_bdev_null_resize :
 *   각 구조체의 동적 필드(name)를 해제하는 헬퍼.
 */

#include "spdk/rpc.h"
/* [한국어] SPDK JSON-RPC 서버 API. spdk_jsonrpc_request, spdk_json_val,
 *           spdk_json_decode_object, send_*_response, SPDK_RPC_REGISTER. */
#include "spdk/util.h"
/* [한국어] SPDK_COUNTOF 등 유틸 매크로. 디코더 배열 크기 산출에 사용. */
#include "spdk/string.h"
/* [한국어] spdk_strerror — errno → 사람이 읽는 메시지로 변환. */
#include "spdk/bdev_module.h"
/* [한국어] struct spdk_bdev 정의. bdev->name 필드에 접근하기 위해 필요. */
#include "spdk/log.h"
/* [한국어] SPDK_DEBUGLOG/SPDK_ERRLOG 매크로. 디버그/에러 로그용. */

#include "bdev_null.h"
/* [한국어] 같은 모듈의 내부 헤더. null_bdev_opts, bdev_null_create/delete/resize,
 *           spdk_delete_null_complete 콜백 타입. */

/*
 * [한국어]
 * free_rpc_construct_null - null_bdev_opts.name 해제 헬퍼.
 *
 * @req: 정리할 옵션 구조체.
 * @return: 없음.
 *
 * 호출 체인:
 *   rpc_bdev_null_create() (정상/cleanup) → [이 함수] → free()
 */
static void
free_rpc_construct_null(struct null_bdev_opts *req)
{
	free(req->name);
	/* [한국어] 디코더가 strdup으로 잡은 name 메모리를 해제. NULL 안전. */
}

/*
 * [한국어] rpc_bdev_null_create_decoders
 *           "bdev_null_create" RPC의 params를 null_bdev_opts로 역직렬화하는 표.
 *           {JSON 키, 구조체 오프셋, 디코더, optional 플래그}.
 *           num_blocks/block_size/name이 필수, 나머지는 선택.
 */
static const struct spdk_json_object_decoder rpc_bdev_null_create_decoders[] = {
	{"name", offsetof(struct null_bdev_opts, name), spdk_json_decode_string},
	/* [한국어] 디스크 이름 (필수). null bdev는 자동 명명 로직이 없다. */
	{"uuid", offsetof(struct null_bdev_opts, uuid), spdk_json_decode_uuid, true},
	/* [한국어] UUID (선택). 미지정 시 0 → bdev 코어가 자동 할당. */
	{"num_blocks", offsetof(struct null_bdev_opts, num_blocks), spdk_json_decode_uint64},
	/* [한국어] 블록 개수 (필수). > 0이어야 함 (bdev_null_create에서 검증). */
	{"block_size", offsetof(struct null_bdev_opts, block_size), spdk_json_decode_uint32},
	/* [한국어] 데이터 블록 크기 (필수). 512의 배수여야 함. */
	{"physical_block_size", offsetof(struct null_bdev_opts, physical_block_size), spdk_json_decode_uint32, true},
	/* [한국어] 물리 블록 크기 (선택, 0이면 미지정). */
	{"md_size", offsetof(struct null_bdev_opts, md_size), spdk_json_decode_uint32, true},
	/* [한국어] 메타데이터 크기 (선택). 0/8/16/32/64/128만 허용. */
	{"dif_type", offsetof(struct null_bdev_opts, dif_type), spdk_json_decode_int32, true},
	/* [한국어] DIF 타입 (선택). 0=Disable, 1/2/3=Type1/2/3. */
	{"dif_is_head_of_md", offsetof(struct null_bdev_opts, dif_is_head_of_md), spdk_json_decode_bool, true},
	/* [한국어] DIF가 메타데이터 머리에 있는지 (선택). */
	{"dif_pi_format", offsetof(struct null_bdev_opts, dif_pi_format), spdk_json_decode_uint32, true},
	/* [한국어] DIF PI 포맷 (선택). */
	{"preferred_write_alignment", offsetof(struct null_bdev_opts, preferred_write_alignment), spdk_json_decode_uint32, true},
	/* [한국어] 쓰기 정렬 힌트 (선택). 상위 컨슈머에 대한 권장값. */
	{"preferred_write_granularity", offsetof(struct null_bdev_opts, preferred_write_granularity), spdk_json_decode_uint32, true},
	/* [한국어] 쓰기 단위 힌트 (선택). */
	{"optimal_write_size", offsetof(struct null_bdev_opts, optimal_write_size), spdk_json_decode_uint32, true},
	/* [한국어] 최적 쓰기 크기 힌트 (선택). */
	{"preferred_unmap_alignment", offsetof(struct null_bdev_opts, preferred_unmap_alignment), spdk_json_decode_uint32, true},
	/* [한국어] UNMAP(=TRIM/Deallocate) 정렬 힌트 (선택). */
	{"preferred_unmap_granularity", offsetof(struct null_bdev_opts, preferred_unmap_granularity), spdk_json_decode_uint32, true},
	/* [한국어] UNMAP 단위 힌트 (선택). */
};

/*
 * [한국어]
 * rpc_bdev_null_create - "bdev_null_create" JSON-RPC 메서드 핸들러.
 *
 * @request: RPC 요청 객체. 응답 송신에 사용.
 * @params : 클라이언트가 보낸 JSON params.
 * @return : 없음. 응답은 spdk_jsonrpc_*_response()로 직접 송신.
 *
 * 동작:
 *   1) JSON params → null_bdev_opts 디코딩.
 *   2) bdev_null_create() 동기 호출 — 옵션 검증 + spdk_bdev_register.
 *   3) 성공 시 result로 bdev 이름을 응답, 실패 시 에러 응답.
 *   4) 성공/실패 모두 free_rpc_construct_null로 동적 필드 해제.
 *
 * 실행 컨텍스트: SPDK JSON-RPC 서버 스레드.
 *
 * 호출 체인:
 *   spdk_jsonrpc_server_handle_req() → [이 함수]
 *     → spdk_json_decode_object()
 *     → bdev_null_create() → spdk_bdev_register()
 *     → spdk_jsonrpc_begin_result() / send_error_response()
 */
static void
rpc_bdev_null_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct null_bdev_opts req = {};
	/* [한국어] 0/NULL 일괄 초기화. 모든 선택 필드의 안전한 기본값이 됨. */
	struct spdk_json_write_ctx *w;
	/* [한국어] 응답 본문 작성용 라이터 컨텍스트. */
	struct spdk_bdev *bdev;
	/* [한국어] bdev_null_create()가 채울 새 bdev 포인터. 응답에서 이름만 사용. */
	int rc = 0;
	/* [한국어] 반환 코드 누적 변수. */

	if (spdk_json_decode_object(params, rpc_bdev_null_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_null_create_decoders),
				    &req)) {
		/* [한국어] JSON params를 위 디코더 표대로 req로 역직렬화. 실패 시 if 진입. */
		SPDK_DEBUGLOG(bdev_null, "spdk_json_decode_object failed\n");
		/* [한국어] 디버그 로그. 컴포넌트 이름은 bdev_null.c 끝에서 등록. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		/* [한국어] 표준 JSON-RPC INTERNAL_ERROR(-32603)로 응답. */
		goto cleanup;
	}

	rc = bdev_null_create(&bdev, &req);
	/* [한국어] 핵심 호출: null bdev 생성. md_size 검증, block_size 정렬 확인,
	 *           DIF 설정 검증, spdk_bdev_register까지 동기 수행. */
	if (rc) {
		/* [한국어] 음수 errno 반환 시 에러 응답. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] 성공 응답 본문 시작. */
	spdk_json_write_string(w, bdev->name);
	/* [한국어] 새 bdev의 이름을 result로 기록. 클라이언트가 이후 참조용 식별자로 사용. */
	spdk_jsonrpc_end_result(request, w);
	/* [한국어] 응답 송신 종료. */
	free_rpc_construct_null(&req);
	/* [한국어] 정상 경로의 정리. req.name(strdup) 해제. */
	return;

cleanup:
	free_rpc_construct_null(&req);
	/* [한국어] 에러 경로의 공통 정리. req.name이 NULL일 수 있으나 free(NULL)은 안전. */
}
SPDK_RPC_REGISTER("bdev_null_create", rpc_bdev_null_create, SPDK_RPC_RUNTIME)
/* [한국어] "bdev_null_create"를 RUNTIME 상태에서만 받아들이도록 등록. */

struct rpc_delete_null {
	/* [한국어] "bdev_null_delete" RPC 입력 구조체. 단일 필드 name만 가진다.
	 * 생성/소멸: rpc_bdev_null_delete()의 스택 변수로 매 요청마다 생성/소멸.
	 * 사용 흐름: spdk_json_decode_object → bdev_null_delete → free_rpc_delete_null. */
	char *name;
	/* [한국어] 삭제할 bdev의 이름 문자열(strdup 소유).
	 * 설정자: spdk_json_decode_string (decoder 표를 통해 호출). JSON params의
	 *         "name" 키 값을 strdup으로 복사해 채운다.
	 * 읽는 자: bdev_null_delete()가 이름을 받아 spdk_bdev_unregister_by_name에 전달.
	 * 값 범위: 유효한 bdev 이름(NUL 종단 문자열)이거나, 디코더 실패 시 NULL.
	 *          빈 문자열은 unregister_by_name이 -ENODEV로 거절.
	 * 동기화: 핸들러 호출 스택에서만 존재(요청별 독립). 다른 스레드와 공유 없음. */
};

/*
 * [한국어]
 * free_rpc_delete_null - rpc_delete_null.name 해제 헬퍼.
 *
 * @req: 정리할 구조체. name=NULL이어도 안전.
 * @return: 없음.
 */
static void
free_rpc_delete_null(struct rpc_delete_null *req)
{
	free(req->name);
	/* [한국어] strdup으로 잡힌 name 해제. */
}

static const struct spdk_json_object_decoder rpc_bdev_null_delete_decoders[] = {
	/* [한국어] delete 메서드의 JSON 디코더 표. name 1개 필수. */
	{"name", offsetof(struct rpc_delete_null, name), spdk_json_decode_string},
	/* [한국어] optional 플래그 없음 → 필수. */
};

/*
 * [한국어]
 * rpc_bdev_null_delete_cb - bdev_null_delete()의 비동기 완료 콜백.
 *
 * @cb_arg   : delete 호출 시 넘긴 spdk_jsonrpc_request*.
 * @bdeverrno: 0=성공, 음수=-errno.
 * @return   : 없음.
 *
 * 동기: bdev 코어가 unregister + bdev_null_destruct를 끝낸 시점에 호출되어
 *       이 시점에 클라이언트에 응답을 보낸다.
 *
 * 실행 컨텍스트: bdev 코어가 unregister를 완료한 SPDK thread.
 *
 * 호출 체인:
 *   bdev_null_delete() → spdk_bdev_unregister_by_name() → ... 비동기 ...
 *     → bdev_null_destruct() → [이 함수] → spdk_jsonrpc_send_*_response()
 */
static void
rpc_bdev_null_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	/* [한국어] 비동기 단계를 넘어 RPC 요청 핸들을 보존하기 위해 cb_arg에 실음. */

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
		/* [한국어] 성공 — true 단일 값으로 응답. */
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
		/* [한국어] 실패 — errno 그대로 + strerror 메시지로 응답. */
	}
}

/*
 * [한국어]
 * rpc_bdev_null_delete - "bdev_null_delete" RPC 핸들러.
 *
 * @request: 클라이언트의 요청. 콜백 cb_arg로 그대로 전달.
 * @params : {"name": "Null0"} 형태의 JSON.
 * @return : 없음. 결과는 비동기 콜백 또는 즉시 에러 응답.
 *
 * 동작:
 *   1) JSON 디코딩 → req.name. 실패 시 INTERNAL_ERROR 응답 후 cleanup.
 *   2) bdev_null_delete()에 콜백 등록 — 즉시 반환. 실제 삭제는 비동기.
 *   3) (성공 경로) free_rpc_delete_null로 req.name 해제 후 return.
 *      bdev_null_delete가 내부적으로 이름을 더 이상 참조하지 않거나
 *      자체적으로 다른 곳에 저장하므로 여기서 해제해도 안전.
 *
 * 실행 컨텍스트: SPDK JSON-RPC 서버 스레드.
 *
 * 호출 체인:
 *   spdk_jsonrpc_server_handle_req() → [이 함수]
 *     → spdk_json_decode_object()
 *     → bdev_null_delete() → 비동기 → rpc_bdev_null_delete_cb()
 */
static void
rpc_bdev_null_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_delete_null req = {NULL};
	/* [한국어] name=NULL로 초기화. */

	if (spdk_json_decode_object(params, rpc_bdev_null_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_null_delete_decoders),
				    &req)) {
		/* [한국어] JSON params 파싱. 필수 name이 없거나 타입 불일치 시 실패. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev_null_delete(req.name, rpc_bdev_null_delete_cb, request);
	/* [한국어] 비동기 삭제 시작. 즉시 반환되며, 결과는 콜백을 통해 클라이언트로. */

	free_rpc_delete_null(&req);
	/* [한국어] 정상 경로 정리. */

	return;

cleanup:
	free_rpc_delete_null(&req);
	/* [한국어] 에러 경로 정리. */
}
SPDK_RPC_REGISTER("bdev_null_delete", rpc_bdev_null_delete, SPDK_RPC_RUNTIME)
/* [한국어] delete 메서드 등록. */

struct rpc_bdev_null_resize {
	/* [한국어] "bdev_null_resize" RPC 입력 구조체. 대상 이름과 새 크기 두 필드.
	 * 생성/소멸: rpc_bdev_null_resize()의 스택 변수.
	 * 사용 흐름: spdk_json_decode_object → bdev_null_resize → free_rpc_bdev_null_resize. */
	char *name;
	/* [한국어] 리사이즈 대상 bdev의 이름 문자열(strdup 소유).
	 * 설정자: spdk_json_decode_string(decoder 표). 클라이언트 JSON의 "name"을 복사.
	 * 읽는 자: bdev_null_resize()가 spdk_bdev_open_ext의 인자로 전달.
	 * 값 범위: 유효한 null bdev 이름. 디코더 실패 시 NULL.
	 *          다른 모듈의 bdev 이름이면 bdev_null_resize 내부에서 -EINVAL.
	 * 동기화: 요청별 스택 변수 — 다른 RPC 핸들러와 공유 없음. */
	uint64_t new_size;
	/* [한국어] 새 디스크 크기 (단위: MiB; bdev_null_resize의 두 번째 인자 시그니처).
	 * 설정자: spdk_json_decode_uint64(decoder 표).
	 * 읽는 자: bdev_null_resize()가 내부에서 *1024*1024 후 blocklen으로 나눠
	 *         새 blockcnt를 계산하고 spdk_bdev_notify_blockcnt_change에 전달.
	 * 값 범위: 현재 디스크 크기(MiB) 이상이어야 함. 작으면 -EINVAL(축소 거부).
	 *          0이면 디스크 용량 0 → 별도 검증 없이 거의 무의미 처리.
	 *          uint64_t 표현 한도 내(블록 수 환산 시 오버플로 주의는 호출자 몫).
	 * 동기화: 요청별 스택 변수. */
};

static const struct spdk_json_object_decoder rpc_bdev_null_resize_decoders[] = {
	/* [한국어] resize 메서드 디코더. 두 필드 모두 필수. */
	{"name", offsetof(struct rpc_bdev_null_resize, name), spdk_json_decode_string},
	/* [한국어] 대상 이름 (필수). */
	{"new_size", offsetof(struct rpc_bdev_null_resize, new_size), spdk_json_decode_uint64}
	/* [한국어] 새 크기 MiB (필수). */
};

/*
 * [한국어]
 * free_rpc_bdev_null_resize - rpc_bdev_null_resize.name 해제 헬퍼.
 *
 * @req: 정리할 구조체.
 * @return: 없음.
 */
static void
free_rpc_bdev_null_resize(struct rpc_bdev_null_resize *req)
{
	free(req->name);
	/* [한국어] strdup으로 잡힌 name 해제. */
}

/*
 * [한국어]
 * rpc_bdev_null_resize - "bdev_null_resize" RPC 핸들러 (동기).
 *
 * @request: RPC 요청 객체.
 * @params : {"name": "Null0", "new_size": 1024} 형태.
 * @return : 없음. 동기적으로 응답 송신.
 *
 * 동작:
 *   1) JSON 디코딩.
 *   2) bdev_null_resize() 동기 호출.
 *      - 내부에서 spdk_bdev_open_ext, blockcnt 변경 통보, close.
 *   3) 성공 시 true 응답, 실패 시 에러 응답.
 *
 * 제약: 새 크기는 현재보다 크거나 같아야 한다 (축소 불가).
 *
 * 실행 컨텍스트: SPDK JSON-RPC 서버 스레드.
 *
 * 호출 체인:
 *   spdk_jsonrpc_server_handle_req() → [이 함수]
 *     → spdk_json_decode_object()
 *     → bdev_null_resize() → spdk_bdev_open_ext/notify_blockcnt_change/close
 */
static void
rpc_bdev_null_resize(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_null_resize req = {};
	/* [한국어] 0/NULL 일괄 초기화. */
	int rc;
	/* [한국어] resize 결과 코드. */

	if (spdk_json_decode_object(params, rpc_bdev_null_resize_decoders,
				    SPDK_COUNTOF(rpc_bdev_null_resize_decoders),
				    &req)) {
		/* [한국어] JSON params 디코드. 필수 필드 누락 시 실패. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_null_resize(req.name, req.new_size);
	/* [한국어] 동기 리사이즈. 대상이 없거나 다른 모듈의 bdev면 -EINVAL,
	 *           새 크기가 더 작으면 -EINVAL. */
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	/* [한국어] 성공 — true 응답. */
cleanup:
	free_rpc_bdev_null_resize(&req);
	/* [한국어] 모든 경로의 공통 정리. */
}
SPDK_RPC_REGISTER("bdev_null_resize", rpc_bdev_null_resize, SPDK_RPC_RUNTIME)
/* [한국어] resize 메서드 등록. RUNTIME 상태에서만 호출 가능. */
