/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] passthru 가상 bdev의 JSON-RPC 핸들러 구현 (vbdev_passthru_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK passthru(투과형) 가상 bdev 모듈에 대한 JSON-RPC 진입점을 정의한다.
 * SPDK의 RPC 서버(`lib/rpc`)는 외부 클라이언트(예: scripts/rpc.py)가 보내는 JSON 요청을
 * 받아 등록된 핸들러 함수로 디스패치하는데, 본 파일은 "bdev_passthru_create"와
 * "bdev_passthru_delete" 두 가지 메서드의 디코더와 콜백을 등록한다.
 * passthru 모듈 자체의 핵심 로직(bdev_passthru_create_disk / bdev_passthru_delete_disk)은
 * 동일 디렉토리의 vbdev_passthru.c에 있고, 본 파일은 단순한 "JSON ↔ C struct" 어댑터 역할만
 * 수행한다. 따라서 본 파일을 수정하지 않고도 RPC 시그니처(필드 이름/타입)만으로 외부
 * 인터페이스가 결정된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [외부 클라이언트] → spdk_jsonrpc_server (lib/jsonrpc) → SPDK RPC 디스패처 (lib/rpc)
 *   → rpc_bdev_passthru_create / rpc_bdev_passthru_delete (이 파일)
 *   → bdev_passthru_create_disk / bdev_passthru_delete_disk (vbdev_passthru.c)
 *   → spdk_bdev_register / spdk_bdev_unregister (lib/bdev)
 * 실행 컨텍스트는 SPDK 메인 reactor의 RPC poller — 즉 `g_init_thread`(또는 RPC를
 * 처리하도록 지정된 thread)에서 실행된다. RPC 핸들러는 polled-mode 환경의 비동기 콜백이며,
 * 블로킹 I/O를 직접 발행하지 않고 bdev_register/unregister API를 호출한다(이 API는 자체적으로
 * 비동기 완료 콜백을 통해 결과를 보고).
 *
 * === 타 모듈과의 연결 ===
 * - include: spdk/rpc.h(JSON-RPC 등록 매크로), spdk/util.h(SPDK_COUNTOF), spdk/string.h
 *   (spdk_strerror), spdk/log.h(로깅), vbdev_passthru.h(create_disk/delete_disk 선언).
 * - 의존: lib/jsonrpc, lib/rpc(메서드 등록 테이블), lib/bdev(스핀-락-프리 bdev 등록 경로),
 *   그리고 같은 디렉토리의 vbdev_passthru.c(실제 vbdev 구현).
 * - 데이터 흐름: 클라이언트가 보낸 JSON 객체 → spdk_json_decode_object()로 본 파일의
 *   `rpc_bdev_passthru_create` 구조체에 디코딩 → 실제 디스크 생성 함수 호출 → 결과
 *   문자열(생성된 bdev 이름) 또는 에러 코드를 JSON으로 다시 인코딩하여 반환.
 *
 * === 주요 함수/구조체 요약 ===
 * - `struct rpc_bdev_passthru_create`     : create RPC 입력 파라미터를 담는 임시 구조체.
 * - `struct rpc_bdev_passthru_delete`     : delete RPC 입력 파라미터를 담는 임시 구조체.
 * - `rpc_bdev_passthru_create_decoders[]` : JSON 필드명 ↔ 구조체 오프셋 매핑(create).
 * - `rpc_bdev_passthru_delete_decoders[]` : JSON 필드명 ↔ 구조체 오프셋 매핑(delete).
 * - `rpc_bdev_passthru_create()`          : create RPC의 디코드/실행/응답 핸들러.
 * - `rpc_bdev_passthru_delete()`          : delete RPC의 디코드/실행 핸들러.
 * - `rpc_bdev_passthru_delete_cb()`       : delete의 비동기 완료 콜백 — bdev_unregister가
 *                                            끝난 뒤 클라이언트에게 응답을 보낸다.
 */

/* [한국어] passthru 모듈의 공개 헤더 — bdev_passthru_create_disk()/bdev_passthru_delete_disk()
 *           프로토타입을 가져온다. 이 두 함수가 본 파일의 RPC 핸들러로부터 호출되는 핵심 진입점. */
#include "vbdev_passthru.h"
/* [한국어] SPDK_RPC_REGISTER, spdk_jsonrpc_send_*, spdk_jsonrpc_begin_result 등 RPC API 선언. */
#include "spdk/rpc.h"
/* [한국어] SPDK_COUNTOF(배열 원소 개수) 매크로 등 작은 유틸. 디코더 테이블 크기 계산에 사용. */
#include "spdk/util.h"
/* [한국어] spdk_strerror() — POSIX errno를 사람이 읽을 수 있는 문자열로 변환(에러 응답에 사용). */
#include "spdk/string.h"
/* [한국어] SPDK_DEBUGLOG/SPDK_ERRLOG 매크로. 디버그 로그 컴포넌트는 vbdev_passthru.c에서
 *           SPDK_LOG_REGISTER_COMPONENT로 등록된 "vbdev_passthru" 이름을 공유한다. */
#include "spdk/log.h"

/* Structure to hold the parameters for this RPC method. */
/* [한국어] bdev_passthru_create RPC가 받는 JSON 객체를 디코딩할 임시 컨테이너.
 *           JSON 입력 예시:
 *             { "base_bdev_name": "Nvme0n1", "name": "PT0", "uuid": "..." }
 *           디코더 테이블이 각 필드를 본 구조체의 멤버에 직접 채운다. */
struct rpc_bdev_passthru_create {
	char *base_bdev_name;
	/* [한국어] 위에 stack 할 base bdev의 이름 (예: "Nvme0n1").
	 * 설정자: spdk_json_decode_string()이 strdup으로 새 버퍼를 할당해 채움.
	 * 읽는 자: bdev_passthru_create_disk()에 전달되어 spdk_bdev_get_by_name()으로 base bdev 조회.
	 * 값 범위: NULL 가능(디코딩 실패 시) — 실패 경로에서 free()는 NULL-safe.
	 * 동기화: 단일 RPC 핸들러 호출 내에서만 사용되므로 별도 락 없음. */

	char *name;
	/* [한국어] 새로 생성될 passthru vbdev의 이름. SPDK 전역에서 유일해야 한다.
	 * 설정자: spdk_json_decode_string()이 할당.
	 * 읽는 자: bdev_passthru_create_disk()가 spdk_bdev_register()의 bdev->name으로 설정.
	 * 값 범위: NULL 가능 — 실패 시 free_rpc_bdev_passthru_create에서 정리.
	 * 동기화: 한 RPC 호출 내에서만 사용. */

	struct spdk_uuid uuid;
	/* [한국어] 새 vbdev에 부여할 UUID. 선택 입력(true 플래그 디코더) — 미지정 시 0으로 채워짐.
	 * 설정자: spdk_json_decode_uuid()가 문자열을 16바이트 바이너리로 변환해 채움.
	 * 읽는 자: bdev_passthru_create_disk()가 spdk_bdev_alloc_bdev_uuid()로 처리.
	 * 값 범위: 16 바이트 바이너리 UUID; 0이면 랜덤 UUID를 생성하라는 신호.
	 * 동기화: 한 RPC 호출 내에서만 사용. */
};

/* Free the allocated memory resource after the RPC handling. */
/*
 * [한국어]
 * free_rpc_bdev_passthru_create - rpc_bdev_passthru_create 구조체 내부의 strdup'd 문자열을 해제한다.
 *
 * @r: 정리할 RPC 입력 구조체 포인터(스택 변수 주소). NULL 아니어야 함.
 * @return: 없음.
 *
 * spdk_json_decode_string()은 입력 JSON 문자열을 strdup으로 복제해 둔다. 이를 해제하지 않으면
 * RPC 호출마다 메모리가 누수된다. 본 함수는 성공/실패 양쪽 경로에서 cleanup: 라벨로부터 호출된다.
 * NULL pointer는 free() 표준상 안전하므로 디코딩 실패 시(필드가 채워지기 전 fail)에도 안전.
 *
 * 호출 체인: rpc_bdev_passthru_create()의 cleanup: → free_rpc_bdev_passthru_create()
 *           실행 컨텍스트: SPDK RPC poller (메인 reactor의 init thread).
 */
static void
free_rpc_bdev_passthru_create(struct rpc_bdev_passthru_create *r)
{
	free(r->base_bdev_name);  /* [한국어] base_bdev_name 디코더가 strdup한 버퍼 해제. NULL이면 noop. */
	free(r->name);            /* [한국어] name 디코더가 strdup한 버퍼 해제. */
}

/* Structure to decode the input parameters for this RPC method. */
/* [한국어] JSON 객체의 각 필드를 어떻게 C 구조체에 채울지 지정하는 디코더 테이블.
 *           각 항목은 {"키 이름", 구조체 내 오프셋, 디코더 함수, [선택 플래그(true=옵션)]} 형태.
 *           spdk_json_decode_object()가 이 테이블을 사용해 자동 매핑한다. */
static const struct spdk_json_object_decoder rpc_bdev_passthru_create_decoders[] = {
	/* [한국어] "base_bdev_name" 필드를 문자열로 디코드 — 필수 (선택 플래그 없음). */
	{"base_bdev_name", offsetof(struct rpc_bdev_passthru_create, base_bdev_name), spdk_json_decode_string},
	/* [한국어] "name" 필드를 문자열로 디코드 — 필수. */
	{"name", offsetof(struct rpc_bdev_passthru_create, name), spdk_json_decode_string},
	/* [한국어] "uuid" 필드를 spdk_uuid로 디코드 — 4번째 인자 true는 "옵션" 의미.
	 *           미지정 시 구조체 초기화 값(0)이 그대로 유지됨. */
	{"uuid", offsetof(struct rpc_bdev_passthru_create, uuid), spdk_json_decode_uuid, true},
};

/* Decode the parameters for this RPC method and properly construct the passthru
 * device. Error status returned in the failed cases.
 */
/*
 * [한국어]
 * rpc_bdev_passthru_create - "bdev_passthru_create" JSON-RPC 메서드의 핸들러.
 *
 * @request: SPDK RPC 디스패처가 전달하는 RPC 요청 컨텍스트(응답 인코딩에 사용).
 * @params : 디코딩되지 않은 JSON 파라미터 객체(클라이언트가 보낸 raw JSON 트리).
 * @return : void — 결과는 spdk_jsonrpc_send_* 함수로 클라이언트에게 직접 전송.
 *
 * 동작 순서:
 *   1) 디코더 테이블로 JSON → struct rpc_bdev_passthru_create 변환.
 *   2) 실패 시 INTERNAL_ERROR 응답 후 cleanup.
 *   3) 성공 시 bdev_passthru_create_disk() 호출(여기서 base bdev 열고 vbdev 등록).
 *   4) create_disk 실패 시 errno 기반 에러 응답.
 *   5) 성공 시 새로 만들어진 vbdev 이름을 JSON 문자열로 응답.
 *   6) 항상 cleanup: 라벨에서 임시 문자열을 해제.
 *
 * 실행 컨텍스트: SPDK RPC poller(메인 reactor) — 모든 bdev 등록 작업이 단일 thread에서 일어나
 * 별도 락 없이 안전하다(SPDK bdev 코어가 thread-affinity 모델을 강제).
 *
 * 호출 체인: SPDK RPC dispatcher → [이 함수] → bdev_passthru_create_disk() (vbdev_passthru.c)
 *                                              → spdk_bdev_open_ext() / spdk_bdev_register()
 */
static void
rpc_bdev_passthru_create(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_bdev_passthru_create req = {NULL};   /* [한국어] 입력 파라미터 임시 저장소. 모든 포인터 필드를 NULL로 초기화. */
	struct spdk_json_write_ctx *w;                  /* [한국어] 성공 응답 JSON을 인코딩할 writer 핸들. */
	int rc;                                         /* [한국어] create_disk 반환 코드(성공 0 / 실패 -errno). */

	/* [한국어] JSON 객체를 디코더 테이블에 따라 req에 채운다. 0이 성공. */
	if (spdk_json_decode_object(params, rpc_bdev_passthru_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_passthru_create_decoders),
				    &req)) {
		/* [한국어] 디코딩 실패 — 디버그 로그 한 줄 남기고 INTERNAL_ERROR 코드(=-32603) 응답. */
		SPDK_DEBUGLOG(vbdev_passthru, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;  /* [한국어] 부분적으로 채워졌을 수 있는 문자열 해제 후 종료. */
	}

	/* [한국어] 핵심 호출 — vbdev_passthru.c의 create_disk가 base bdev를 열고 새 vbdev를 등록.
	 *           동기 호출처럼 보이지만 내부에서 spdk_bdev_open_ext + spdk_bdev_register만 수행하므로
	 *           블로킹 I/O는 일어나지 않는다(메모리 할당과 등록만 처리). */
	rc = bdev_passthru_create_disk(req.base_bdev_name, req.name, &req.uuid);
	if (rc != 0) {
		/* [한국어] 등록 실패 — POSIX errno(음수)를 사람이 읽는 문자열로 변환해 응답. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	/* [한국어] 성공 응답 빌드 시작 — JSON writer는 RPC 디스패처에 등록된 응답 버퍼에 직접 인코딩. */
	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] 결과는 단일 문자열(생성된 vbdev 이름). 클라이언트는 이 이름으로 후속 RPC를 호출할 수 있다. */
	spdk_json_write_string(w, req.name);
	/* [한국어] 응답 마무리 — TCP/Unix socket 통해 즉시 전송된다. */
	spdk_jsonrpc_end_result(request, w);

cleanup:
	/* [한국어] 성공/실패 무관하게 strdup된 입력 문자열 해제. */
	free_rpc_bdev_passthru_create(&req);
}
/* [한국어] SPDK RPC 메서드 등록 — 매크로가 자동 호출되는 생성자(__attribute__((constructor)))로
 *           "bdev_passthru_create"라는 이름을 디스패처 테이블에 추가한다.
 *           SPDK_RPC_RUNTIME = 런타임(서버 가동 후) 호출 가능 상태에서만 활성. */
SPDK_RPC_REGISTER("bdev_passthru_create", rpc_bdev_passthru_create, SPDK_RPC_RUNTIME)

/* [한국어] bdev_passthru_delete RPC가 받는 입력. 단일 필드 "name"만 받는다. */
struct rpc_bdev_passthru_delete {
	char *name;
	/* [한국어] 삭제할 passthru vbdev의 이름.
	 * 설정자: spdk_json_decode_string()이 strdup으로 채움.
	 * 읽는 자: bdev_passthru_delete_disk()가 spdk_bdev_unregister_by_name() 호출 시 사용.
	 * 값 범위: NULL 가능(디코딩 실패 시). 실패 경로에서 free 안전.
	 * 동기화: 단일 RPC 호출 내에서만 사용. */
};

/*
 * [한국어]
 * free_rpc_bdev_passthru_delete - delete RPC의 strdup'd 문자열을 해제.
 *
 * @req: 정리할 RPC 입력 구조체.
 * @return: 없음.
 *
 * cleanup 라벨에서 호출. 호출 체인: rpc_bdev_passthru_delete → free_rpc_bdev_passthru_delete.
 * 실행 컨텍스트: RPC poller(메인 reactor).
 */
static void
free_rpc_bdev_passthru_delete(struct rpc_bdev_passthru_delete *req)
{
	free(req->name);  /* [한국어] name 필드의 strdup된 버퍼 해제. NULL이면 noop. */
}

/* [한국어] delete RPC 디코더 테이블 — 단일 필수 필드 "name". */
static const struct spdk_json_object_decoder rpc_bdev_passthru_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_passthru_delete, name), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_passthru_delete_cb - bdev_unregister 완료를 RPC 클라이언트에 통지하는 비동기 콜백.
 *
 * @cb_arg   : delete RPC 호출 시 전달된 spdk_jsonrpc_request 포인터(컨텍스트로 보존).
 * @bdeverrno: 0이면 성공, 음수 errno면 실패.
 * @return   : 없음.
 *
 * SPDK bdev unregister는 비동기 — 모든 채널이 닫히고 모듈 destruct가 끝나야 콜백이 호출된다.
 * 따라서 delete RPC는 본 콜백에서 비로소 응답을 회신한다(클라이언트는 그 동안 대기).
 *
 * 호출 체인: bdev_passthru_delete_disk → ... → spdk_bdev_unregister 완료 → [이 콜백]
 * 실행 컨텍스트: bdev unregister가 완료되는 thread — 일반적으로 등록을 수행한 동일 reactor.
 */
static void
rpc_bdev_passthru_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;  /* [한국어] 보존된 RPC 요청 컨텍스트 복원. */

	if (bdeverrno == 0) {
		/* [한국어] 성공 — 단순 boolean true 응답. 클라이언트는 이를 OK로 해석. */
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		/* [한국어] 실패 — errno를 그대로 RPC 에러 코드로, 메시지는 strerror로 변환. */
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

/*
 * [한국어]
 * rpc_bdev_passthru_delete - "bdev_passthru_delete" RPC 핸들러.
 *
 * @request: RPC 컨텍스트(응답에 사용).
 * @params : 입력 JSON 파라미터.
 * @return : void.
 *
 * 디코드 후 bdev_passthru_delete_disk()를 호출해 unregister를 시작한다. 응답은 콜백
 * (rpc_bdev_passthru_delete_cb)에서 비동기로 회신되므로, 본 함수는 cleanup 후 즉시 반환.
 *
 * 호출 체인: RPC dispatcher → [이 함수] → bdev_passthru_delete_disk() (vbdev_passthru.c)
 *                                          → spdk_bdev_unregister_by_name() → ... → 콜백.
 * 실행 컨텍스트: SPDK RPC poller(메인 reactor).
 */
static void
rpc_bdev_passthru_delete(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_bdev_passthru_delete req = {NULL};  /* [한국어] 입력 파라미터 임시 저장소. */

	if (spdk_json_decode_object(params, rpc_bdev_passthru_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_passthru_delete_decoders),
				    &req)) {
		/* [한국어] JSON 디코딩 실패 → 즉시 에러 응답. 콜백 경로 안 탐. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] 비동기 unregister 시작. 완료 시 rpc_bdev_passthru_delete_cb가 request로 응답.
	 *           request 포인터를 cb_arg로 전달하여 컨텍스트를 콜백까지 보존. */
	bdev_passthru_delete_disk(req.name, rpc_bdev_passthru_delete_cb, request);

cleanup:
	/* [한국어] 입력 임시 문자열 해제 — req.name은 delete_disk 내부에서 strdup으로 별도 보관됨. */
	free_rpc_bdev_passthru_delete(&req);
}
/* [한국어] "bdev_passthru_delete" RPC를 디스패처에 등록 (런타임 활성). */
SPDK_RPC_REGISTER("bdev_passthru_delete", rpc_bdev_passthru_delete, SPDK_RPC_RUNTIME)
