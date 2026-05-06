/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK Notify 서브시스템의 JSON-RPC 핸들러 (notify_rpc.c)
 *
 * === 파일의 역할 ===
 * SPDK Notify 라이브러리(lib/notify/notify.c)가 내부적으로 보유한 알림 타입
 * 카탈로그와 환형 이벤트 버퍼를 외부 관리자(예: spdk_rpc.py 클라이언트, OpenStack
 * cinder driver, K8s csi-driver 등)가 JSON-RPC 인터페이스로 조회할 수 있도록
 * 두 개의 RPC 엔드포인트를 등록한다.
 *   1) `notify_get_types`         — 등록된 알림 타입 이름 배열을 반환.
 *   2) `notify_get_notifications` — id/max 필터로 환형 버퍼의 이벤트들을 반환.
 *
 * 두 RPC 모두 SPDK_RPC_RUNTIME 단계에서만 유효(데몬 부팅 완료 후). 실제 데이터
 * 평면 hot-path 와는 무관한 컨트롤/관찰 평면 핸들러이므로 성능보다는 정확한
 * JSON 직렬화와 입력 검증이 중요하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 외부 RPC client (Python 등)
 *   ↓ TCP/UNIX socket 으로 JSON 메시지
 * SPDK lib/jsonrpc/ (요청 디스패처)
 *   ↓ method 이름으로 디스패치 ("notify_get_types" / "notify_get_notifications")
 * 본 파일의 핸들러 (rpc_notify_get_types / rpc_notify_get_notifications)
 *   ↓ spdk_notify_foreach_type / spdk_notify_foreach_event 호출
 * lib/notify/notify.c (글로벌 락 + 환형 버퍼)
 *   ↓ 결과를 콜백으로 한 건씩 전달
 * 본 파일의 콜백 (notify_get_types_cb / notify_get_notifications_cb)
 *   ↓ spdk_json_write_* API 로 응답 스트림에 직렬화
 * 다시 RPC client 로 응답 전송
 *
 * 실행 컨텍스트는 일반적으로 SPDK 메인(main) reactor의 RPC 처리 thread.
 *
 * === 타 모듈과의 연결 ===
 *  - lib/notify/notify.c    : 실제 데이터 보관 + foreach API 제공자.
 *  - lib/jsonrpc/           : 요청 파싱/응답 직렬화 인프라
 *                             (spdk_jsonrpc_request, spdk_json_write_ctx).
 *  - lib/rpc/               : SPDK_RPC_REGISTER 매크로로 메서드 등록.
 *  - include/spdk/notify.h  : 콜백 시그니처와 spdk_notify_event 정의.
 *  - include/spdk/jsonrpc.h : 에러 코드, response begin/end API.
 *
 * 데이터 흐름은 단방향(read-only): 외부 → SPDK 의 상태를 조회만 한다.
 * 발행(publish)은 별도 코드 경로(예: bdev_register)에서 일어나며 본 파일은 관여
 * 하지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - notify_get_types_cb        : foreach_type 콜백, 타입 이름을 JSON 문자열로 출력.
 *  - rpc_notify_get_types       : `notify_get_types` 메서드 핸들러 — JSON 배열 반환.
 *  - struct rpc_notify_get_notifications: 요청 파라미터(id/max)와 응답 ctx 묶음.
 *  - rpc_notify_get_notifications_decoders: id/max JSON 디코더 테이블.
 *  - notify_get_notifications_cb: foreach_event 콜백, 이벤트 1건을 JSON object 로 출력.
 *  - rpc_notify_get_notifications: `notify_get_notifications` 메서드 핸들러.
 *  - SPDK_LOG_REGISTER_COMPONENT(notify_rpc): 본 파일 전용 디버그 로그 컴포넌트 등록.
 */


#include "spdk/rpc.h"
/* [한국어] SPDK JSON-RPC 서버 측 API. SPDK_RPC_REGISTER 매크로와
 * spdk_jsonrpc_request 핸들 자료형을 정의. */
#include "spdk/string.h"
/* [한국어] spdk_strerror 등 errno 변환 유틸. 잘못된 파라미터에 EINVAL 메시지를
 * 사람이 읽기 좋은 형태로 응답에 담을 때 사용. */
#include "spdk/notify.h"
/* [한국어] 본 RPC가 위임하는 라이브러리의 공개 API
 * (spdk_notify_foreach_type/event, spdk_notify_event 구조체 등). */
#include "spdk/env.h"
/* [한국어] 환경 추상화 헤더. 직접 호출은 없으나, 다른 SPDK 헤더 의존성 일관성을
 * 위해 포함. */
#include "spdk/util.h"
/* [한국어] SPDK_COUNTOF — 디코더 테이블 크기 산출에 사용. */

#include "spdk/log.h"
/* [한국어] SPDK_DEBUGLOG/SPDK_LOG_REGISTER_COMPONENT — 디버깅 로그. */

/*
 * [한국어]
 * notify_get_types_cb - 등록된 알림 타입 1건을 JSON 배열 항목으로 출력하는 콜백.
 *
 * @type: 순회 중인 spdk_notify_type 노드(불투명).
 * @ctx:  spdk_json_write_ctx* 형태로 캐스팅된 응답 직렬화 컨텍스트.
 * @return: 항상 0(중단 없음 — 모든 타입을 출력).
 *
 * 호출자(spdk_notify_foreach_type)가 g_events_lock 보유 상태에서 호출하므로,
 * 이 함수 내부에서 spdk_notify_* 를 다시 호출하면 deadlock. 단순 JSON write 만
 * 수행한다.
 *
 * 호출 체인:
 *   rpc_notify_get_types → spdk_notify_foreach_type → [notify_get_types_cb]
 *     → spdk_json_write_string
 */
static int
notify_get_types_cb(const struct spdk_notify_type *type, void *ctx)
{
	spdk_json_write_string((struct spdk_json_write_ctx *)ctx, spdk_notify_type_get_name(type));
	/* [한국어] 응답 JSON 배열에 타입 이름을 한 항목으로 추가.
	 * spdk_notify_type_get_name으로 이름 포인터를 얻고,
	 * spdk_json_write_string으로 그것을 quote/escape 처리하여 출력 스트림에 기록. */
	return 0;
	/* [한국어] 항상 0을 반환하여 모든 타입을 끝까지 순회. */
}

/*
 * [한국어]
 * rpc_notify_get_types - JSON-RPC 메서드 `notify_get_types` 핸들러.
 *
 * 입력 스키마: 파라미터 없음. params 가 NULL 이 아니면 INVALID_PARAMS 에러.
 * 출력 스키마: 문자열 배열. 예) ["bdev_register", "bdev_unregister", ...]
 *
 * @request: SPDK 측 RPC 요청 핸들 (응답 송신용).
 * @params:  파싱된 JSON 파라미터 트리. 본 메서드는 인자 없음을 강제.
 *
 * 동작:
 *   1) params 가 NULL 이 아니면 즉시 에러 응답.
 *   2) spdk_jsonrpc_begin_result 로 응답 stream 시작.
 *   3) JSON 배열 열기 → spdk_notify_foreach_type 으로 콜백 dispatch
 *      (각 타입 이름이 string 으로 추가됨) → 배열 닫기.
 *   4) spdk_jsonrpc_end_result 로 송신 완료.
 *
 * 호출 컨텍스트: SPDK RPC 서버 thread. 핸들러는 동기적으로 응답을 마무리.
 *
 * 호출 체인:
 *   spdk_jsonrpc_server (디스패처) → [rpc_notify_get_types]
 *     → spdk_notify_foreach_type → notify_get_types_cb → spdk_json_write_string
 */
static void
rpc_notify_get_types(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	/* [한국어] JSON 응답 스트림 컨텍스트. begin/end 사이에서 write_* 가 누적. */

	if (params != NULL) {
		/* [한국어] 본 메서드는 파라미터를 받지 않는다. 호환성 차원에서 빈 객체
		 * 라도 거부 — 클라이언트의 잘못된 사용을 즉시 알린다. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "No parameters required");
		/* [한국어] 표준 JSON-RPC 에러 코드(-32602)와 사람이 읽을 수 있는
		 * 메시지를 응답하고 즉시 반환. */
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] 응답 객체의 "result" 필드를 쓸 수 있는 스트림을 시작. 호출 후에는
	 * write_* 함수들로 본문을 채운 뒤 spdk_jsonrpc_end_result 로 마무리해야 한다. */
	spdk_json_write_array_begin(w);
	/* [한국어] 결과 최상위는 배열. JSON: [ ... ] */
	spdk_notify_foreach_type(notify_get_types_cb, w);
	/* [한국어] 등록된 모든 타입에 대해 콜백을 호출, 이름을 배열에 누적.
	 * w 를 ctx 로 전달하여 콜백이 응답 스트림에 직접 쓸 수 있게 한다. */
	spdk_json_write_array_end(w);
	/* [한국어] 배열 닫기. */

	spdk_jsonrpc_end_result(request, w);
	/* [한국어] 응답 송신 완료. 내부적으로 framing 후 RPC 클라이언트로 전송. */
}
SPDK_RPC_REGISTER("notify_get_types", rpc_notify_get_types, SPDK_RPC_RUNTIME)
/* [한국어] 메서드 이름 "notify_get_types" 와 핸들러를 등록.
 * SPDK_RPC_RUNTIME 은 "데몬 부팅 완료 후에만 호출 가능"이라는 의미로,
 * 초기화 단계의 RPC(SPDK_RPC_STARTUP)와 구분된다. 매크로는 컨스트럭터 어트리뷰트로
 * 자동 호출되어 RPC 디스패처에 등록된다. */

/*
 * [한국어]
 * struct rpc_notify_get_notifications - notify_get_notifications 요청의 파싱
 * 결과 + 응답 직렬화 컨텍스트를 묶어 콜백에 전달하기 위한 보조 구조체.
 */
struct rpc_notify_get_notifications {
	uint64_t id;
	/* [한국어] 클라이언트가 마지막으로 본 이벤트 ID 다음 위치. 0 이면 처음부터.
	 * 설정자: spdk_json_decode_object 가 입력 JSON 의 "id" 필드를 파싱해 채움.
	 *          (필드 없으면 기본값 0 유지.)
	 * 읽는 자: spdk_notify_foreach_event 의 start_idx 로 전달.
	 * 값 범위: 0..UINT64_MAX. notify.c의 윈도우 보정 로직이 너무 오래된 값은
	 *  자동으로 (head - MAX)로 끌어올린다.
	 * 동기화: 단일 RPC 호출 내에서만 사용되므로 별도 락 불요. */

	uint64_t max;
	/* [한국어] 한 번의 응답으로 가져올 최대 이벤트 수. 기본 UINT64_MAX(무제한).
	 * 설정자: spdk_json_decode_object — 입력에 "max" 가 있으면 갱신.
	 * 읽는 자: spdk_notify_foreach_event 의 max 로 전달.
	 * 값 범위: 0(아무것도 안 가져옴) ~ UINT64_MAX.
	 * 동기화: 단일 RPC 호출 범위. */

	struct spdk_json_write_ctx *w;
	/* [한국어] 응답 직렬화에 쓸 JSON 스트림 컨텍스트 포인터.
	 * 설정자: rpc_notify_get_notifications 가 spdk_jsonrpc_begin_result 의
	 *          반환값으로 채움.
	 * 읽는 자: notify_get_notifications_cb 가 각 이벤트를 객체로 직렬화할 때 사용.
	 * 값 범위: NULL 불가(비0 검증은 호출자 책임).
	 * 동기화: 단일 RPC 호출 범위. */
};

/* [한국어] JSON 객체 디코더 테이블. spdk_json_decode_object 가 이 배열을 사용해
 * 입력 JSON 의 키를 구조체 필드 오프셋으로 매핑한다.
 * 각 엔트리: { 키 이름, 오프셋, 디코더 함수, optional 여부 }.
 * 두 필드 모두 optional(true) — 둘 다 생략하면 처음부터 무제한 fetch. */
static const struct spdk_json_object_decoder rpc_notify_get_notifications_decoders[] = {
	{"id", offsetof(struct rpc_notify_get_notifications, id), spdk_json_decode_uint64, true},
	/* [한국어] "id" 키 → struct.id (uint64), 선택적. */
	{"max", offsetof(struct rpc_notify_get_notifications, max), spdk_json_decode_uint64, true},
	/* [한국어] "max" 키 → struct.max (uint64), 선택적. */
};


/*
 * [한국어]
 * notify_get_notifications_cb - 이벤트 1건을 JSON object 로 직렬화하는 콜백.
 *
 * @id:  이벤트의 글로벌 단조 증가 ID (notify.c 의 g_events_head 에서 부여).
 * @ev:  이벤트 페이로드(고정 길이 type/ctx 문자열). NUL-padded.
 * @ctx: 사용자 컨텍스트 — 본 파일에서는 rpc_notify_get_notifications* 로 캐스팅.
 * @return: 항상 0(중단하지 않음).
 *
 * JSON 출력 형식 (한 항목당):
 *   {
 *     "type": "<event type 문자열>",
 *     "ctx":  "<event 컨텍스트 문자열>",
 *     "id":   <uint64 이벤트 ID>
 *   }
 *
 * 호출 체인:
 *   rpc_notify_get_notifications → spdk_notify_foreach_event
 *     → [notify_get_notifications_cb] → spdk_json_write_*
 */
static int
notify_get_notifications_cb(uint64_t id, const struct spdk_notify_event *ev, void *ctx)
{
	struct rpc_notify_get_notifications *req = ctx;
	/* [한국어] 콜백 컨텍스트를 우리가 정의한 보조 구조체로 환원. */

	spdk_json_write_object_begin(req->w);
	/* [한국어] 한 이벤트를 표현하는 JSON 객체 시작 ('{'). */
	spdk_json_write_named_string(req->w, "type", ev->type);
	/* [한국어] "type": "<문자열>" 출력. NUL-padding 된 고정 필드도 NUL 까지만
	 * 직렬화 되므로 안전. */
	spdk_json_write_named_string(req->w, "ctx", ev->ctx);
	/* [한국어] "ctx": "<문자열>" 출력. */
	spdk_json_write_named_uint64(req->w, "id", id);
	/* [한국어] "id": <숫자> 출력. 클라이언트가 다음 polling 시 이 값+1 을
	 * 사용해 incremental fetch 를 이어갈 수 있다. */
	spdk_json_write_object_end(req->w);
	/* [한국어] 객체 종료 ('}'). */
	return 0;
	/* [한국어] 모든 항목을 끝까지 처리하도록 0 반환. */
}

/*
 * [한국어]
 * rpc_notify_get_notifications - JSON-RPC 메서드 `notify_get_notifications` 핸들러.
 *
 * 입력 스키마(모두 선택적, 객체):
 *   { "id":  <uint64, 마지막으로 본 이벤트 다음 위치, 기본 0>,
 *     "max": <uint64, 응답 한도, 기본 UINT64_MAX> }
 * 출력 스키마: 위 콜백이 만든 객체들의 배열.
 *
 * @request: 응답을 보낼 RPC 요청 핸들.
 * @params:  파싱된 JSON 파라미터 트리(NULL 가능).
 *
 * 동작:
 *   1) 기본값 (id=0, max=UINT64_MAX) 으로 req 초기화.
 *   2) params 가 있으면 디코더 테이블로 파싱. 실패 시 INVALID_PARAMS 에러 응답.
 *   3) 응답 stream 시작 → 배열 열기.
 *   4) spdk_notify_foreach_event 호출 — 윈도우 보정 + 콜백 dispatch.
 *   5) 배열 닫고 응답 송신.
 *
 * 호출 체인:
 *   spdk_jsonrpc_server (디스패처) → [rpc_notify_get_notifications]
 *     → spdk_notify_foreach_event → notify_get_notifications_cb
 */
static void
rpc_notify_get_notifications(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_notify_get_notifications req = {0, UINT64_MAX};
	/* [한국어] 입력 기본값: id=0(처음부터), max=UINT64_MAX(무제한).
	 * 사용자가 둘 다 생략하면 환형 윈도우(최근 1024개) 전체를 반환. */

	if (params &&
	    spdk_json_decode_object(params, rpc_notify_get_notifications_decoders,
				    SPDK_COUNTOF(rpc_notify_get_notifications_decoders), &req)) {
		/* [한국어] params 가 있고, 디코드 시 0 이외(에러)이 반환되면 invalid 처리.
		 * SPDK_COUNTOF 는 컴파일타임 배열 크기 매크로. */
		SPDK_DEBUGLOG(notify_rpc, "spdk_json_decode_object failed\n");
		/* [한국어] 디버그 로그(파싱 실패 원인 추적). */

		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(EINVAL));
		/* [한국어] 표준 INVALID_PARAMS(-32602) + errno 메시지로 에러 응답. */
		return;
	}


	req.w = spdk_jsonrpc_begin_result(request);
	/* [한국어] result 스트림 시작. req 구조체의 w 필드에 저장하여 콜백에서 접근. */

	spdk_json_write_array_begin(req.w);
	/* [한국어] 응답은 이벤트 객체들의 배열 — '['. */
	spdk_notify_foreach_event(req.id, req.max, notify_get_notifications_cb, &req);
	/* [한국어] notify 라이브러리에 위임. (req.id, req.max) 윈도우 안의 이벤트들이
	 * 콜백을 통해 배열에 한 객체씩 추가된다. ctx 로 req 전체를 넘겨주어 콜백이
	 * req.w 에 직접 쓸 수 있도록 한다. */
	spdk_json_write_array_end(req.w);
	/* [한국어] 배열 닫기 — ']'. */

	spdk_jsonrpc_end_result(request, req.w);
	/* [한국어] 응답 송신. */
}
SPDK_RPC_REGISTER("notify_get_notifications", rpc_notify_get_notifications, SPDK_RPC_RUNTIME)
/* [한국어] 메서드 등록. SPDK_RPC_RUNTIME — 데몬 부팅 후 호출 가능. */

SPDK_LOG_REGISTER_COMPONENT(notify_rpc)
/* [한국어] 본 파일 전용 디버그 로그 컴포넌트 "notify_rpc" 등록. SPDK_DEBUGLOG
 * 매크로의 첫 인자(category)로 사용되며, 실행 시 spdk 옵션 -L notify_rpc 또는
 * RPC log_set_print_level 등으로 출력 활성화 가능. */
