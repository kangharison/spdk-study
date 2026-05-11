/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] split 가상 bdev의 JSON-RPC 핸들러 (vbdev_split_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK split(파티션) vbdev 모듈의 JSON-RPC 인터페이스를 정의한다.
 * "bdev_split_create" / "bdev_split_delete" 두 메서드를 SPDK RPC 디스패처에 등록하고,
 * JSON 입력을 vbdev_split.c의 핵심 함수(create_vbdev_split / vbdev_split_destruct)로 전달한다.
 * split 모듈은 단일 base bdev를 N개의 동일 크기 sub-bdev로 잘라(spdk_bdev_part 활용) 노출하는
 * 가상 디바이스이며, 본 파일은 그 구성 RPC를 외부로 노출하는 얇은 어댑터 역할을 한다.
 * create RPC는 추가로 응답에 "생성된 sub-bdev 이름 배열"을 인코딩해 클라이언트가 후속 작업에
 * 활용할 수 있게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [클라이언트] → SPDK RPC 디스패처 → rpc_bdev_split_create / rpc_bdev_split_delete (이 파일)
 *   → create_vbdev_split / vbdev_split_destruct (vbdev_split.c)
 *   → spdk_bdev_part_base_construct_ext / spdk_bdev_part_construct (lib/bdev/part.c)
 *   → spdk_bdev_register / unregister.
 * 응답 인코딩 시에는 spdk_bdev_open_ext()로 base bdev 디스크립터를 새로 열어, base bdev에
 * 등록된 part 리스트를 순회하면서 sub-bdev 이름을 JSON 배열로 출력한다(open/close는 같은
 * 컨텍스트 내에서 짝지어 발생).
 *
 * === 타 모듈과의 연결 ===
 * - include: spdk/rpc.h, spdk/util.h, spdk/string.h, vbdev_split.h, spdk/log.h.
 * - 의존: vbdev_split.c(create/destruct), lib/bdev/part.c(spdk_bdev_part_*), lib/bdev(open/close).
 * - 데이터 흐름: 클라이언트 JSON → 본 파일에서 디코딩 → vbdev_split.c → bdev part API
 *   → 등록된 sub-bdev들 → 본 파일이 sub-bdev 이름을 다시 모아 JSON 배열로 응답.
 *
 * === 주요 함수/구조체 요약 ===
 * - `struct rpc_construct_split`     : create RPC 입력 (base_bdev, split_count, split_size_mb).
 * - `struct rpc_delete_split`        : delete RPC 입력 (base_bdev).
 * - `dummy_bdev_event_cb()`          : 응답 빌드용 임시 spdk_bdev_open_ext의 noop 이벤트 콜백.
 * - `rpc_bdev_split_create()`        : create RPC 핸들러 — 분할 생성 후 sub-bdev 이름 배열 응답.
 * - `rpc_bdev_split_delete()`        : delete RPC 핸들러 — 모든 sub-bdev를 제거.
 */

/* [한국어] SPDK_RPC_REGISTER 등 RPC 등록/응답 API. */
#include "spdk/rpc.h"
/* [한국어] SPDK_COUNTOF 등 작은 매크로. */
#include "spdk/util.h"
/* [한국어] spdk_strerror — 음수 errno → 문자열 변환. */
#include "spdk/string.h"

/* [한국어] split 모듈의 공개 헤더 — create_vbdev_split, vbdev_split_destruct,
 *           vbdev_split_get_part_base 프로토타입을 가져온다. */
#include "vbdev_split.h"
/* [한국어] SPDK_ERRLOG/DEBUGLOG. 컴포넌트 이름 "vbdev_split"는 vbdev_split.c에서 등록. */
#include "spdk/log.h"

/* [한국어] bdev_split_create RPC 입력 컨테이너.
 *           JSON: { "base_bdev": "Nvme0n1", "split_count": 4, "split_size_mb": 256 } */
struct rpc_construct_split {
	char *base_bdev;
	/* [한국어] 분할할 base bdev의 이름. base가 아직 등록되지 않았더라도 설정은 등록되며,
	 *           실제 sub-bdev들은 base가 examine 시점에 등록될 때 같이 만들어진다.
	 * 설정자: spdk_json_decode_string()이 strdup으로 채움.
	 * 읽는 자: create_vbdev_split() → vbdev_split_add_config()가 strdup으로 보관.
	 * 값 범위: NULL 가능(디코딩 실패). 성공 시 0이 아닌 길이의 문자열.
	 * 동기화: 단일 RPC 호출 내에서만. */

	uint32_t split_count;
	/* [한국어] 만들 sub-bdev 개수(N). 0이면 에러.
	 * 설정자: spdk_json_decode_uint32. 읽는 자: create_vbdev_split.
	 * 값 범위: 1 이상. base 용량 / split_size 결과로 제한될 수 있음(클램프).
	 * 동기화: 동일. */

	uint64_t split_size_mb;
	/* [한국어] 각 sub-bdev의 크기 (MiB 단위). 0이면 base 용량을 split_count로 균등 분할.
	 * 설정자: spdk_json_decode_uint64 (옵션 — true 플래그). 읽는 자: create_vbdev_split.
	 * 값 범위: 0(자동) 또는 base block size의 배수가 되어야 정확히 떨어진다.
	 * 동기화: 동일. */
};

/* [한국어] bdev_split_create의 JSON 디코더 테이블.
 *           split_size_mb는 4번째 인자 true로 옵션 처리. */
static const struct spdk_json_object_decoder rpc_bdev_split_create_decoders[] = {
	{"base_bdev", offsetof(struct rpc_construct_split, base_bdev), spdk_json_decode_string},
	{"split_count", offsetof(struct rpc_construct_split, split_count), spdk_json_decode_uint32},
	{"split_size_mb", offsetof(struct rpc_construct_split, split_size_mb), spdk_json_decode_uint64, true},
};

/*
 * [한국어]
 * dummy_bdev_event_cb - spdk_bdev_open_ext()가 요구하는 이벤트 콜백의 noop 구현.
 *
 * @type: 이벤트 종류(REMOVE/RESIZE 등).
 * @bdev: 이벤트 대상 bdev.
 * @ctx : open 시 전달한 컨텍스트(여기선 NULL).
 * @return: 없음.
 *
 * spdk_bdev_open_ext()는 base bdev에 hot-remove 등 비동기 이벤트가 발생했을 때
 * 호출할 콜백 등록을 강제한다. 본 RPC는 응답 생성을 위해 일시적으로 base bdev를
 * 열었다가 즉시 닫으므로, 사실상 이벤트가 도달하기 전에 close가 끝난다 — 따라서 noop.
 * 다만 콜백 자체는 NULL이 허용되지 않으므로 빈 함수를 등록한다.
 *
 * 실행 컨텍스트: bdev unregister/resize 등이 일어나는 thread(보통 등록 thread).
 * 호출 체인: lib/bdev → [이 콜백]. 본 RPC 핸들러는 직접 호출하지 않는다.
 */
static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

/*
 * [한국어]
 * rpc_bdev_split_create - "bdev_split_create" RPC 핸들러.
 *
 * @request: RPC 응답 컨텍스트.
 * @params : 입력 JSON 파라미터.
 * @return : void.
 *
 * 동작:
 *   1) JSON 디코딩.
 *   2) create_vbdev_split() 호출 — 설정 등록 + 실제 sub-bdev 생성 시도.
 *   3) base bdev를 일시적으로 open_ext로 열어 part TAILQ를 순회하며 생성된 sub-bdev 이름을
 *      JSON 배열로 응답에 채운다(base가 아직 없으면 빈 배열).
 *   4) 디스크립터 close.
 *
 * 응답 형식: ["base_bdevp0", "base_bdevp1", ...]
 *
 * 실행 컨텍스트: SPDK RPC poller(메인 reactor). bdev open/close가 단일 thread에서 일어나
 * 안전하다.
 *
 * 호출 체인:
 *   RPC dispatcher → [이 함수] → create_vbdev_split() → spdk_bdev_part_construct(...)
 *   응답 빌드 단계: spdk_bdev_open_ext → spdk_bdev_part_get_bdev → spdk_bdev_close.
 */
static void
rpc_bdev_split_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_construct_split req = {};            /* [한국어] 입력 파라미터(0 초기화 — 옵션 필드 안전). */
	struct spdk_json_write_ctx *w;                  /* [한국어] 응답 JSON writer. */
	struct spdk_bdev_desc *base_desc;               /* [한국어] base bdev 디스크립터(임시 open 결과). */
	struct spdk_bdev *base_bdev;                    /* [한국어] base bdev 포인터(desc로부터 추출). */
	int rc;                                         /* [한국어] 반환 코드(0=성공/음수=errno). */

	/* [한국어] JSON → struct 디코딩. 실패 시 INVALID_PARAMS 응답. */
	if (spdk_json_decode_object(params, rpc_bdev_split_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_split_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* [한국어] 핵심 호출 — 설정을 g_split_config에 등록하고, base bdev이 있으면 즉시 sub-bdev 생성.
	 *           base가 아직 없으면 examine 시점에 만들어지며 여기선 0(성공)으로 반환된다. */
	rc = create_vbdev_split(req.base_bdev, req.split_count, req.split_size_mb);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to create %"PRIu32" split bdevs from '%s': %s",
						     req.split_count, req.base_bdev, spdk_strerror(-rc));
		goto out;
	}

	/* [한국어] 응답 인코딩 시작 — sub-bdev 이름을 JSON 배열로 반환. */
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	/* [한국어] base bdev이 등록되어 있다면 일시적으로 열어 part 리스트를 조회.
	 *           write 권한 false — 메타데이터 조회만이므로 read-only로 충분.
	 *           실패 시 = base가 아직 없는 경우 — 빈 배열로 종료. */
	rc = spdk_bdev_open_ext(req.base_bdev, false, dummy_bdev_event_cb, NULL, &base_desc);
	if (rc == 0) {
		struct spdk_bdev_part_base *split_base;          /* [한국어] split이 등록한 part_base. */
		struct bdev_part_tailq *split_base_tailq;        /* [한국어] sub-bdev 리스트 헤드. */
		struct spdk_bdev_part *split_part;               /* [한국어] 순회 변수(각 sub-bdev). */
		struct spdk_bdev *split_bdev;                    /* [한국어] part로부터 추출한 spdk_bdev. */

		/* [한국어] 디스크립터에서 spdk_bdev 포인터 추출. */
		base_bdev = spdk_bdev_desc_get_bdev(base_desc);

		/* [한국어] base bdev에 묶인 split의 part_base를 g_split_config에서 조회. */
		split_base = vbdev_split_get_part_base(base_bdev);

		/* [한국어] create가 성공했으니 part_base는 반드시 존재해야 함. */
		assert(split_base != NULL);

		/* [한국어] part_base에서 sub-bdev들의 TAILQ를 가져와 순회. */
		split_base_tailq = spdk_bdev_part_base_get_tailq(split_base);
		TAILQ_FOREACH(split_part, split_base_tailq, tailq) {
			/* [한국어] 각 part에서 spdk_bdev 추출 후 이름을 JSON 배열에 추가. */
			split_bdev = spdk_bdev_part_get_bdev(split_part);
			spdk_json_write_string(w, spdk_bdev_get_name(split_bdev));
		}

		/* [한국어] 임시 디스크립터 close. unregister와는 별개의 단순 핸들 닫기. */
		spdk_bdev_close(base_desc);
	}

	/* [한국어] 배열 종료 후 응답 전송. */
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

out:
	/* [한국어] base_bdev 문자열 해제. 디코딩 실패 시 NULL일 수 있고 free는 안전. */
	free(req.base_bdev);
}
/* [한국어] "bdev_split_create"를 RPC 디스패처에 등록(런타임). */
SPDK_RPC_REGISTER("bdev_split_create", rpc_bdev_split_create, SPDK_RPC_RUNTIME)

/* [한국어] bdev_split_delete RPC 입력 — 단일 필드 base_bdev. */
struct rpc_delete_split {
	char *base_bdev;
	/* [한국어] 삭제 대상 base bdev 이름. 이 base 위에 만들어진 모든 sub-bdev이 함께 제거된다.
	 * 설정자: spdk_json_decode_string. 읽는 자: vbdev_split_destruct.
	 * 값 범위: NULL 가능.
	 * 동기화: 단일 RPC 호출 내에서만. */
};

/* [한국어] delete RPC 디코더 — 단일 필수 필드. */
static const struct spdk_json_object_decoder rpc_bdev_split_delete_decoders[] = {
	{"base_bdev", offsetof(struct rpc_delete_split, base_bdev), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_split_delete - "bdev_split_delete" RPC 핸들러.
 *
 * @request: RPC 컨텍스트.
 * @params : 입력 JSON.
 * @return : void.
 *
 * vbdev_split_destruct()는 동기적으로 g_split_config에서 설정을 찾아 part_base를 hotremove
 * (sub-bdev들을 unregister)한 뒤 free한다. unregister 자체는 비동기지만, RPC 응답은 곧바로
 * boolean true로 회신하는 단순 정책이다(개별 unregister 완료를 대기하지 않음 — split의
 * destruct 콜백이 base_free를 통해 정리).
 *
 * 호출 체인: RPC dispatcher → [이 함수] → vbdev_split_destruct() (vbdev_split.c)
 *                                          → spdk_bdev_part_base_hotremove → unregister 체인.
 * 실행 컨텍스트: SPDK RPC poller.
 */
static void
rpc_bdev_split_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_delete_split req = {};   /* [한국어] 입력 임시 저장소. */
	int rc;                             /* [한국어] destruct 반환 코드. */

	/* [한국어] JSON 디코딩 실패 → 즉시 INVALID_PARAMS 응답. */
	if (spdk_json_decode_object(params, rpc_bdev_split_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_split_delete_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* [한국어] split 설정 제거 시작. -ENOENT면 등록되지 않은 이름 → 클라이언트 입력 오류로 간주. */
	rc = vbdev_split_destruct(req.base_bdev);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
		goto out;
	}

	/* [한국어] 성공 응답(boolean true). 실제 unregister 완료는 비동기지만 RPC 결과는 즉시 반환. */
	spdk_jsonrpc_send_bool_response(request, true);
out:
	/* [한국어] strdup된 base_bdev 문자열 해제. */
	free(req.base_bdev);
}
/* [한국어] "bdev_split_delete" RPC 등록(런타임 활성). */
SPDK_RPC_REGISTER("bdev_split_delete", rpc_bdev_split_delete, SPDK_RPC_RUNTIME)
