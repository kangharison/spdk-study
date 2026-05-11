/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] malloc bdev 모듈의 JSON-RPC 핸들러 (bdev_malloc_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK의 JSON-RPC 인터페이스를 통해 malloc bdev를 생성/삭제할 수
 * 있도록 두 개의 RPC 메서드 핸들러("bdev_malloc_create", "bdev_malloc_delete")를
 * 등록하고 구현한다. 각 핸들러는 클라이언트가 보낸 JSON params를
 * `malloc_bdev_opts` 또는 `rpc_delete_malloc` 구조체로 디코딩한 후,
 * bdev_malloc.c가 노출하는 동기 API(create_malloc_disk) 또는 비동기
 * API(delete_malloc_disk)를 호출한다. 결과는 spdk_jsonrpc_send_*로
 * 클라이언트에 응답한다. RPC 입장에서 본 모듈의 "사용자 정의 컨트롤 플레인"
 * 진입점이 모두 여기에 모여 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (생성):
 *   [JSON-RPC client (rpc.py 등)] → SPDK JSON-RPC 서버
 *     → rpc_bdev_malloc_create()  ← (이 파일)
 *       → spdk_json_decode_object()  (JSON params → malloc_bdev_opts)
 *       → create_malloc_disk()       (bdev_malloc.c)
 *         → spdk_bdev_register()     (lib/bdev)
 *       → spdk_jsonrpc_begin_result() / send_error_response()
 * 호출 체인 (삭제):
 *   → rpc_bdev_malloc_delete()
 *     → delete_malloc_disk()        (bdev_malloc.c)
 *       → spdk_bdev_unregister_by_name() → ... 비동기 ...
 *         → rpc_bdev_malloc_delete_cb() (이 파일에서 등록한 콜백)
 *           → spdk_jsonrpc_send_bool_response() / send_error_response()
 * 실행 컨텍스트: SPDK app의 JSON-RPC 서버 poller가 도는 SPDK thread.
 * 보통 마스터 reactor 코어 위에서 1개 스레드만이 RPC를 처리한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: bdev_malloc.h (옵션 구조체, create/delete 함수 선언),
 *   spdk/rpc.h (SPDK_RPC_REGISTER 매크로, JSON-RPC 서버 인터페이스),
 *   spdk/string.h (spdk_strerror 등 errno→string 변환),
 *   spdk/log.h (SPDK_DEBUGLOG 매크로).
 * - 등록 효과: SPDK_RPC_REGISTER 매크로는 빌드 시점에 RPC 메서드 테이블에
 *   "bdev_malloc_create"/"bdev_malloc_delete"를 등록 (생성자 함수 트릭).
 *   런타임에는 spdk_rpc_set_state(SPDK_RPC_RUNTIME) 이후에만 호출 가능.
 * - 데이터 흐름: 클라이언트 → JSON params → 핸들러 디코드
 *               → create/delete_malloc_disk → bdev 코어 → bdev 트리.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_bdev_malloc_create_decoders[]   : create 메서드의 JSON 필드 매핑 테이블.
 * - rpc_bdev_malloc_create()            : create RPC 핸들러. 동기 처리.
 * - free_rpc_construct_malloc()         : malloc_bdev_opts.name 해제 헬퍼.
 * - struct rpc_delete_malloc            : delete의 입력 (이름 1개).
 * - rpc_bdev_malloc_delete_decoders[]   : delete 메서드의 JSON 필드 매핑.
 * - rpc_bdev_malloc_delete_cb()         : delete의 비동기 완료 콜백 (응답 송신).
 * - rpc_bdev_malloc_delete()            : delete RPC 핸들러 (콜백 등록만 하고 반환).
 */

#include "bdev_malloc.h"
/* [한국어] 같은 모듈의 내부 헤더. malloc_bdev_opts 구조체와
 *           create_malloc_disk()/delete_malloc_disk() 선언, 그리고
 *           spdk_delete_malloc_complete 콜백 타입을 가져온다. */
#include "spdk/rpc.h"
/* [한국어] SPDK JSON-RPC 서버 API. spdk_jsonrpc_request, decode_object,
 *           send_error_response, begin_result, end_result, SPDK_RPC_REGISTER 등.
 *           이 RPC 인프라는 lib/json + lib/jsonrpc + lib/rpc로 구성된다. */
#include "spdk/string.h"
/* [한국어] spdk_strerror() — errno 정수를 사람이 읽는 메시지로 변환.
 *           실패 응답의 message 필드에 넣을 때 사용. */
#include "spdk/log.h"
/* [한국어] SPDK 로깅 매크로 (SPDK_DEBUGLOG, SPDK_ERRLOG). 디버그 빌드에서
 *           "bdev_malloc" 컴포넌트 로그를 켜면 디코드 실패 등이 출력된다. */

/*
 * [한국어]
 * free_rpc_construct_malloc - malloc_bdev_opts 안에서 malloc/strdup으로 잡힌
 *                             동적 할당 필드(name)를 해제하는 정리 헬퍼.
 *
 * @r: 정리할 옵션 구조체. 디코더가 채운 이후 또는 부분 채움 후에 호출 가능.
 * @return: 없음.
 *
 * 동기: rpc_bdev_malloc_create()에서 정상/에러 두 경로 모두에서 동일하게
 * 호출되어 메모리 누수를 막는다. 다른 필드는 모두 스칼라/구조체 by value
 * 라서 별도 해제가 필요 없다.
 *
 * 호출 체인:
 *   rpc_bdev_malloc_create() (성공/cleanup) → [이 함수] → free()
 */
static void
free_rpc_construct_malloc(struct malloc_bdev_opts *r)
{
	free(r->name);
	/* [한국어] r->name은 spdk_json_decode_string이 strdup으로 할당한
	 *           메모리이거나 NULL. free(NULL)은 안전하므로 분기 없이 호출. */
}

/*
 * [한국어] rpc_bdev_malloc_create_decoders
 *           "bdev_malloc_create" RPC의 params(JSON 객체)를 malloc_bdev_opts로
 *           역직렬화하기 위한 필드 디스크립터 배열. 각 엔트리는
 *           {JSON 키, 구조체 내 오프셋, 디코더 함수, optional 플래그}.
 *           true 플래그는 "선택적 필드" — 누락되어도 에러가 아니다.
 *           num_blocks와 block_size만 필수.
 */
static const struct spdk_json_object_decoder rpc_bdev_malloc_create_decoders[] = {
	{"name", offsetof(struct malloc_bdev_opts, name), spdk_json_decode_string, true},
	/* [한국어] 디스크 이름 (선택). 미지정 시 create_malloc_disk()가 "MallocN"으로 자동 생성. */
	{"uuid", offsetof(struct malloc_bdev_opts, uuid), spdk_json_decode_uuid, true},
	/* [한국어] UUID 문자열 → 16바이트 spdk_uuid (선택). 미지정 시 0(자동 할당). */
	{"num_blocks", offsetof(struct malloc_bdev_opts, num_blocks), spdk_json_decode_uint64},
	/* [한국어] 블록 개수 (필수). 누락 시 디코드 실패 → INTERNAL_ERROR 응답. */
	{"block_size", offsetof(struct malloc_bdev_opts, block_size), spdk_json_decode_uint32},
	/* [한국어] 데이터 블록 크기 바이트 (필수). 512의 배수여야 함. */
	{"physical_block_size", offsetof(struct malloc_bdev_opts, physical_block_size), spdk_json_decode_uint32, true},
	/* [한국어] 물리 블록 크기 (선택). 0이면 미지정. */
	{"optimal_io_boundary", offsetof(struct malloc_bdev_opts, optimal_io_boundary), spdk_json_decode_uint32, true},
	/* [한국어] split 힌트(블록 단위, 선택). NVMe NOIOB 유사 개념. */
	{"md_size", offsetof(struct malloc_bdev_opts, md_size), spdk_json_decode_uint32, true},
	/* [한국어] 메타데이터 크기 (선택). 0/8/16/32/64/128만 허용. */
	{"md_interleave", offsetof(struct malloc_bdev_opts, md_interleave), spdk_json_decode_bool, true},
	/* [한국어] 메타데이터 인터리브 여부 (선택, 기본 false). */
	{"dif_type", offsetof(struct malloc_bdev_opts, dif_type), spdk_json_decode_int32, true},
	/* [한국어] DIF 타입 enum (선택). 0=Disable, 1/2/3=Type1/2/3. */
	{"dif_is_head_of_md", offsetof(struct malloc_bdev_opts, dif_is_head_of_md), spdk_json_decode_bool, true},
	/* [한국어] DIF가 메타 헤드에 위치하는지 (선택). NVMe PIL 비트 대응. */
	{"dif_pi_format", offsetof(struct malloc_bdev_opts, dif_pi_format), spdk_json_decode_uint32, true},
	/* [한국어] DIF PI 포맷 enum (선택). 16b/32b/64b CRC 등. */
	{"numa_id", offsetof(struct malloc_bdev_opts, numa_id), spdk_json_decode_int32, true},
	/* [한국어] NUMA 노드 ID (선택). -1(SPDK_ENV_NUMA_ID_ANY)이면 임의 노드. */
};

/*
 * [한국어]
 * rpc_bdev_malloc_create - "bdev_malloc_create" JSON-RPC 메서드 핸들러.
 *
 * @request: SPDK JSON-RPC 서버가 넘겨주는 요청 객체. 응답 송신에 사용.
 * @params : 클라이언트가 보낸 params JSON 값(객체) — 위 디코더 테이블로 파싱.
 * @return : 없음. 모든 결과는 spdk_jsonrpc_send_*로 클라이언트에 직접 응답.
 *
 * 동작 단계:
 *   1) numa_id 기본값을 SPDK_ENV_NUMA_ID_ANY로 설정 (디코더가 못 채워도 안전).
 *   2) JSON params → malloc_bdev_opts 디코딩. 실패하면 INTERNAL_ERROR로 응답.
 *   3) create_malloc_disk()를 동기 호출. 음수 errno면 -errno로 응답.
 *   4) 성공 시 begin_result로 결과 컨텍스트를 열고 bdev 이름 문자열을 응답.
 *   5) goto cleanup으로 흐르는 모든 에러 경로에서도 free_rpc_construct_malloc로
 *      디코더가 strdup한 name을 해제 (메모리 누수 방지).
 *
 * 실행 컨텍스트: SPDK JSON-RPC 서버 스레드 (보통 마스터 reactor).
 * 동기 함수 — 반환 시점에 bdev 등록까지 완료되거나 실패가 통보된다.
 *
 * 호출 체인:
 *   spdk_jsonrpc_server_handle_req() → [이 함수]
 *     → spdk_json_decode_object()
 *     → create_malloc_disk() → spdk_bdev_register()
 *     → spdk_jsonrpc_begin_result()/send_error_response()
 */
static void
rpc_bdev_malloc_create(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct malloc_bdev_opts req = {NULL};
	/* [한국어] 옵션 구조체를 0/NULL로 초기화. name=NULL, uuid=zeros 등.
	 *           디코더가 채우지 못한 선택 필드들의 안전한 기본값이 된다. */
	struct spdk_json_write_ctx *w;
	/* [한국어] 응답 본문(JSON)을 쓰기 위한 라이터 컨텍스트. begin_result로 획득. */
	struct spdk_bdev *bdev;
	/* [한국어] create_malloc_disk()가 채워줄 새 bdev 포인터 (응답에서 이름만 사용). */
	int rc = 0;
	/* [한국어] 반환 코드 누적 변수. 0이면 성공, 음수면 -errno. */

	req.numa_id = SPDK_ENV_NUMA_ID_ANY;
	/* [한국어] numa_id는 선택 필드라 디코더가 못 채울 수도 있다.
	 *           {NULL}만으로는 0으로 초기화되는데 0은 유효한 NUMA 노드 ID이므로,
	 *           "지정 안 함"을 의미하려면 명시적으로 SPDK_ENV_NUMA_ID_ANY(-1)를 둔다. */

	if (spdk_json_decode_object(params, rpc_bdev_malloc_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_malloc_create_decoders),
				    &req)) {
		/* [한국어] params를 위 디코더 표에 따라 req로 역직렬화.
		 *           반환값이 0이 아니면(true) 디코드 실패 — 필수 필드 누락,
		 *           타입 불일치 등. SPDK_COUNTOF는 배열 원소 개수를 컴파일타임에 산출. */
		SPDK_DEBUGLOG(bdev_malloc, "spdk_json_decode_object failed\n");
		/* [한국어] 디버그 빌드에서만 출력되는 로그. 컴포넌트 이름은 파일 끝에서 등록됨. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		/* [한국어] JSON-RPC 표준 에러 코드 -32603(Internal error)로 응답.
		 *           클라이언트는 이 응답을 받으면 RPC가 실패했음을 안다. */
		goto cleanup;
		/* [한국어] cleanup 라벨로 점프해 부분 디코드된 req의 동적 필드(name) 해제. */
	}

	rc = create_malloc_disk(&bdev, &req);
	/* [한국어] 핵심 호출: 디스크를 동기적으로 생성. 내부에서 hugepage 할당,
	 *           DIF 패턴 기록, spdk_bdev_register까지 완료. 실패 시 -errno 반환. */
	if (rc) {
		/* [한국어] 0이 아니면 실패. rc는 음수 errno이므로 -rc로 양수화. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		/* [한국어] errno 그대로를 RPC 에러 코드에 실어 보내고, 메시지로는
		 *           strerror 변환 결과(예: "Out of memory")를 첨부. */
		goto cleanup;
	}

	free_rpc_construct_malloc(&req);
	/* [한국어] 성공 경로: 더 이상 req의 name이 필요 없으므로 즉시 해제.
	 *           아래 응답 송신은 bdev->name(별도 strdup)을 사용하므로 무관. */

	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] 성공 응답 본문 작성 시작. 내부적으로 "result": 키를 열고
	 *           라이터 컨텍스트를 반환. 이후 spdk_json_write_*로 값을 기록. */
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	/* [한국어] 새로 만든 bdev의 이름(문자열 하나)을 result로 기록.
	 *           rpc.py 등 클라이언트는 이 이름을 이후의 다른 RPC에서 식별자로 사용. */
	spdk_jsonrpc_end_result(request, w);
	/* [한국어] 응답 본문 종료 + 실제 송신. begin_result/end_result는 짝을 이룬다. */
	return;
	/* [한국어] 정상 종료. cleanup을 거치지 않으므로 free_rpc_construct_malloc가
	 *           위에서 이미 실행되었음에 유의. */

cleanup:
	free_rpc_construct_malloc(&req);
	/* [한국어] 모든 에러 경로의 공통 정리. name이 NULL이어도 free(NULL)은 안전. */
}
SPDK_RPC_REGISTER("bdev_malloc_create", rpc_bdev_malloc_create, SPDK_RPC_RUNTIME)
/* [한국어] 컴파일 시점에 RPC 메서드 테이블에 등록하는 매크로. 정적 생성자 트릭으로
 *           main() 진입 전에 호출되어 메서드를 추가한다.
 *           SPDK_RPC_RUNTIME = framework가 RUNTIME 상태(앱 부팅 완료)로 진입한
 *           이후에만 이 RPC를 받아들이도록 제한. STARTUP에는 사용 불가. */

struct rpc_delete_malloc {
	/* [한국어] "bdev_malloc_delete" RPC의 입력만을 담는 작은 구조체 (필드 1개). */
	char *name;
	/* [한국어] 삭제할 bdev 이름 (필수).
	 * 설정자: spdk_json_decode_string (strdup 할당).
	 * 읽는 자: rpc_bdev_malloc_delete()가 delete_malloc_disk에 그대로 전달.
	 * 동기화: 핸들러 스택 위 한 인스턴스만 존재하므로 락 불필요. */
};

/*
 * [한국어]
 * free_rpc_delete_malloc - rpc_delete_malloc.name 해제 헬퍼.
 *
 * @r: 정리할 구조체. NULL 필드는 free(NULL)로 안전.
 * @return: 없음.
 *
 * 호출 체인:
 *   rpc_bdev_malloc_delete() (정상/에러) → [이 함수] → free()
 */
static void
free_rpc_delete_malloc(struct rpc_delete_malloc *r)
{
	free(r->name);
	/* [한국어] strdup으로 잡힌 이름 문자열 해제. */
}

static const struct spdk_json_object_decoder rpc_bdev_malloc_delete_decoders[] = {
	/* [한국어] delete 메서드의 JSON 디코더 표. name 1개만 필수. */
	{"name", offsetof(struct rpc_delete_malloc, name), spdk_json_decode_string},
	/* [한국어] 4번째 인자(optional 플래그)가 없으므로 false → 필수 필드. */
};

/*
 * [한국어]
 * rpc_bdev_malloc_delete_cb - delete_malloc_disk()의 비동기 완료 콜백.
 *
 * @cb_arg   : delete_malloc_disk 호출 시 넘긴 spdk_jsonrpc_request*.
 * @bdeverrno: 0=성공, 음수=-errno (예: -ENODEV, -EBUSY).
 * @return   : 없음.
 *
 * 동기: bdev 코어가 unregister + bdev_malloc_destruct를 모두 마친 후 호출되며,
 *       이 시점에 클라이언트에 응답을 보내야 한다. 콜백에서 직접 RPC 응답을
 *       작성하는 SPDK 비동기 RPC의 전형적 패턴.
 *
 * 실행 컨텍스트: bdev 코어가 unregister를 완료한 SPDK thread.
 *                보통 RPC 처리 스레드와 동일하지만 보장은 아니다.
 *                spdk_jsonrpc_send_*는 thread-safe하게 만들어져 있다.
 *
 * 호출 체인:
 *   delete_malloc_disk() → spdk_bdev_unregister_by_name() → ... 비동기 ...
 *     → bdev_malloc_destruct() → [이 함수]
 *     → spdk_jsonrpc_send_bool_response() / send_error_response()
 */
static void
rpc_bdev_malloc_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	/* [한국어] cb_arg에 RPC 요청 핸들이 인코딩되어 있음. 캐스팅해서 사용. */

	if (bdeverrno == 0) {
		/* [한국어] 성공 — 단순 true 응답으로 충분. */
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		/* [한국어] 실패 — 에러 코드와 메시지로 응답. */
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

/*
 * [한국어]
 * rpc_bdev_malloc_delete - "bdev_malloc_delete" JSON-RPC 메서드 핸들러.
 *
 * @request: 클라이언트의 RPC 요청. delete 콜백의 cb_arg로 그대로 전달된다.
 * @params : params JSON. {"name": "Malloc0"} 형태가 와야 함.
 * @return : 없음. 결과는 비동기 콜백 또는 즉시 에러 응답으로 통지.
 *
 * 동작 단계:
 *   1) JSON 디코딩 → req.name. 실패 시 즉시 INTERNAL_ERROR.
 *   2) delete_malloc_disk(name, cb, request) 호출 — 비동기. 즉시 반환됨.
 *   3) cleanup에서 req.name(strdup 메모리)을 해제.
 *      delete_malloc_disk가 이름을 더 이상 보지 않는 시점이 보장되므로 안전.
 *
 * 실행 컨텍스트: SPDK JSON-RPC 서버 스레드.
 *
 * 호출 체인:
 *   spdk_jsonrpc_server_handle_req() → [이 함수]
 *     → spdk_json_decode_object()
 *     → delete_malloc_disk() → ... 비동기 ... → rpc_bdev_malloc_delete_cb()
 */
static void
rpc_bdev_malloc_delete(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_delete_malloc req = {NULL};
	/* [한국어] name=NULL로 초기화. 디코더 실패 시에도 free(NULL) 안전. */

	if (spdk_json_decode_object(params, rpc_bdev_malloc_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_malloc_delete_decoders),
				    &req)) {
		/* [한국어] JSON params 파싱. 실패 시 0 아닌 값 반환 → if 진입. */
		SPDK_DEBUGLOG(bdev_malloc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		/* [한국어] 클라이언트에 표준 INTERNAL_ERROR 응답. */
		goto cleanup;
	}

	delete_malloc_disk(req.name, rpc_bdev_malloc_delete_cb, request);
	/* [한국어] 비동기 삭제 시작. cb_fn에 위 콜백, cb_arg에 request 전달.
	 *           실제 삭제는 모든 채널이 닫힐 때까지 진행 — 이 함수는 즉시 반환된다.
	 *           대상이 존재하지 않거나 다른 즉시 에러 시 delete_malloc_disk 내부에서
	 *           cb_fn(cb_arg, -errno)를 동기 호출하므로 응답은 항상 보장된다. */

cleanup:
	free_rpc_delete_malloc(&req);
	/* [한국어] req.name 해제. delete_malloc_disk는 내부에서 이미 spdk_bdev 트리의
	 *           name을 키로 lookup을 끝냈거나, 비동기 경로에서는 내부 strdup을
	 *           사용하므로 여기서 해제해도 안전하다. */
}
SPDK_RPC_REGISTER("bdev_malloc_delete", rpc_bdev_malloc_delete, SPDK_RPC_RUNTIME)
/* [한국어] delete 메서드 등록. RUNTIME 상태에서만 호출 허용. */
