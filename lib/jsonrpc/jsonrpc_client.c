/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] JSON-RPC client 코어 (jsonrpc_client.c)
 *
 * === 파일의 역할 ===
 * JSON-RPC 2.0 클라이언트의 transport-agnostic 코어. 두 가지 책임:
 *   (1) 응답 파싱 (jsonrpc_parse_response): client->recv_buf의 누적 byte를
 *       spdk_json_parse(IN_PLACE)로 파싱하고, jsonrpc/id/result/error 4개 필드를
 *       jsonrpc_response_decoders로 디코딩하여 client->resp에 보관.
 *   (2) 요청 직렬화 헬퍼 (spdk_jsonrpc_begin_request/_end_request):
 *       사용자가 spdk_json_write_*로 method/params를 채울 spdk_json_write_ctx 제공.
 * 실제 socket I/O와 연결 관리는 jsonrpc_client_tcp.c가 담당.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 흐름 (요청 송신):
 *   사용자 → spdk_jsonrpc_client_create_request() (tcp.c) →
 *   spdk_jsonrpc_begin_request() [이 파일] → spdk_json_write_* 로 본문 작성 →
 *   spdk_jsonrpc_end_request() [이 파일] → spdk_jsonrpc_client_send_request() (tcp.c) →
 *   poll에서 send().
 * 호출 흐름 (응답 수신):
 *   poll → recv → jsonrpc_client_recv() (tcp.c) →
 *   jsonrpc_parse_response() [이 파일] → client->resp 채움 →
 *   spdk_jsonrpc_client_get_response() (tcp.c) → 사용자.
 * 실행 컨텍스트: 단일 thread (rpc.py 같은 CLI는 main thread, SPDK 내부 도구는
 * 호출자 thread). spdk_jsonrpc_client는 thread-safe하지 않다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/util.h(SPDK_COUNTOF/offsetof), jsonrpc_internal.h(client 자료구조).
 * 데이터 흐름:
 *   client->recv_buf(누적 raw JSON) → spdk_json_parse → resp->values(토큰) →
 *   spdk_json_decode_object → resp->jsonrpc(version/id/result/error 토큰 포인터들).
 *   요청 측: 사용자 spdk_json_write_* → jsonrpc_client_write_cb → request->send_buf.
 * 공유 자료구조: spdk_jsonrpc_client (recv 상태 + resp 슬롯 1개),
 *               spdk_jsonrpc_client_request (send buf + cursor),
 *               spdk_jsonrpc_client_response_internal (외부 노출 + buf 소유권).
 *
 * === 주요 함수/구조체 요약 ===
 *   - capture_version/_id/_any: spdk_json_object_decoder 콜백 — 토큰 캡처 + 타입 검증.
 *   - jsonrpc_response_decoders[]: 응답 객체의 4개 표준 필드 매핑 표.
 *   - jsonrpc_parse_response(): recv_buf 파싱 → client->resp 객체 생성.
 *   - jsonrpc_client_write_cb(): 요청 직렬화 sink — send_buf 자동 확장.
 *   - spdk_jsonrpc_begin_request(): 요청 객체 prologue ({"jsonrpc","id","method",) 작성.
 *   - spdk_jsonrpc_end_request(): epilogue ('}', '\n') 작성, send_buf 마감.
 */

#include "spdk/util.h"             /* [한국어] SPDK_COUNTOF, offsetof 매크로 */
#include "jsonrpc_internal.h"      /* [한국어] client 자료구조 (recv_buf, resp, request 등) */

/*
 * [한국어]
 * capture_version - "jsonrpc" 필드 디코더 콜백: "2.0"인지 검증 + 캡처
 *
 * @val: 디코더가 매치한 토큰 — string 타입이어야 함.
 * @out: const spdk_json_val** (응답 객체의 version 필드 슬롯).
 * @return: 0 성공, SPDK_JSON_PARSE_INVALID 실패(다른 버전 문자열).
 *
 * 사양 §5: 응답의 jsonrpc는 반드시 "2.0". 클라이언트 측에서도 엄격히 검증한다.
 */
static int
capture_version(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;

	if (spdk_json_strequal(val, "2.0") != true) {
		/* [한국어] "2.0"이 아니면 디코딩 실패 — 사양 위반 응답. */
		return SPDK_JSON_PARSE_INVALID;
	}

	*vptr = val;  /* [한국어] 검증 통과 — 토큰 포인터 그대로 저장 */
	return 0;
}

/*
 * [한국어]
 * capture_id - "id" 필드 디코더 콜백: string 또는 number만 허용
 *
 * @val: 매치된 토큰.
 * @out: id 슬롯.
 * @return: 0 성공, -EINVAL 실패.
 *
 * 사양 §5: id는 요청의 id를 echo. SPDK 클라이언트는 string/number만 받음
 * (서버는 null도 허용하지만 클라이언트는 자기가 보낸 정수 id가 와야 정상).
 */
static int
capture_id(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;

	if (val->type != SPDK_JSON_VAL_STRING && val->type != SPDK_JSON_VAL_NUMBER) {
		/* [한국어] string/number 외 타입은 거절 (예: object/array). */
		return -EINVAL;
	}

	*vptr = val;
	return 0;
}

/*
 * [한국어]
 * capture_any - 타입 검증 없이 토큰 그대로 캡처 (result/error용)
 *
 * @val: 매치된 토큰 (모든 타입 허용 — result는 핸들러가 임의 형태로 만들 수 있음).
 * @out: 슬롯.
 * @return: 항상 0.
 */
static int
capture_any(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;

	*vptr = val;  /* [한국어] 타입 검증 없이 그대로 저장 — 사용자가 직접 해석 */
	return 0;
}

/* [한국어] JSON-RPC 응답 객체의 4개 필드 디코더 표.
 * "jsonrpc"는 필수(optional 미지정), 나머지는 optional.
 * 사양 §5: 응답은 반드시 result XOR error 중 하나만 포함하지만, 디코더 단에서는
 * 둘 다 optional로 두고 사용자가 사후 검사한다(result==NULL이면 error 확인). */
static const struct spdk_json_object_decoder jsonrpc_response_decoders[] = {
	{"jsonrpc", offsetof(struct spdk_jsonrpc_client_response, version), capture_version},
	/* [한국어] "jsonrpc": "2.0" — 필수, 값 검증 포함 */
	{"id", offsetof(struct spdk_jsonrpc_client_response, id), capture_id, true},
	/* [한국어] "id": string/number — optional, 타입 검증 */
	{"result", offsetof(struct spdk_jsonrpc_client_response, result), capture_any, true},
	/* [한국어] "result": 모든 타입 — optional, 사용자 파싱 */
	{"error", offsetof(struct spdk_jsonrpc_client_response, error), capture_any, true},
	/* [한국어] "error": object — optional, 사용자 파싱 */
};

/*
 * [한국어]
 * jsonrpc_parse_response - client->recv_buf의 누적 데이터에서 응답 1건 파싱
 *
 * @client: 응답을 보관할 client 인스턴스.
 * @return: 1 = 응답 1건 파싱 성공(client->resp 채워짐, ready=true),
 *          0 = INCOMPLETE(더 받아야 함, recv 루프 계속),
 *          -EINVAL = 파싱 에러(연결 종료 권장),
 *          -ENOSPC = client->resp 슬롯이 이미 점유됨(이전 응답 미회수),
 *          -errno = OOM 등.
 *
 * 동작 단계:
 *   1) 1차 spdk_json_parse(NULL): 토큰 개수 확인 + INCOMPLETE 검출.
 *      INCOMPLETE면 0 반환 — 다음 recv 대기.
 *   2) calloc으로 response_internal 생성 (flexible array values 포함).
 *   3) client->resp 슬롯이 이미 점유돼 있으면 -ENOSPC (사용자가 get_response 안 함).
 *   4) recv_buf 소유권을 r->buf로 이전 — client는 다음 응답을 위해 새 버퍼 alloc.
 *   5) 2차 spdk_json_parse(IN_PLACE): r->buf 내부에 NUL 박으며 토큰 디코딩.
 *   6) 최상위 OBJECT_BEGIN 검증 + jsonrpc_response_decoders로 4개 필드 추출.
 *   7) ready=true로 마킹 — 다음 get_response()가 회수 가능.
 *
 * 호출 체인:
 *   jsonrpc_client_recv() (tcp.c) → [이 함수] → 사용자 spdk_jsonrpc_client_get_response().
 */
int
jsonrpc_parse_response(struct spdk_jsonrpc_client *client)
{
	struct spdk_jsonrpc_client_response_internal *r;
	ssize_t rc;
	size_t buf_len;
	size_t values_cnt;
	void *end = NULL;


	/* Check to see if we have received a full JSON value. */
	rc = spdk_json_parse(client->recv_buf, client->recv_offset, NULL, 0, &end, 0);
	/* [한국어] 1차 파싱 — 토큰 배열 NULL이라 카운트 + 완성도 체크. recv_buf 비파괴. */
	if (rc == SPDK_JSON_PARSE_INCOMPLETE) {
		/* [한국어] JSON이 끝나지 않음 — 다음 recv를 기다린다. */
		return 0;
	}

	SPDK_DEBUGLOG(rpc_client, "JSON string is :\n%s\n", client->recv_buf);
	/* [한국어] 디버그 로그 — rpc_client 컴포넌트 활성 시만 출력. */
	if (rc < 0 || rc > SPDK_JSONRPC_CLIENT_MAX_VALUES) {
		SPDK_ERRLOG("JSON parse error (rc: %zd)\n", rc);
		/*
		 * Can't recover from parse error (no guaranteed resync point in streaming JSON).
		 * Return an error to indicate that the connection should be closed.
		 */
		/* [한국어] invalid JSON 또는 토큰 한도(8192) 초과 — 스트리밍에서 복구 불가. */
		return -EINVAL;
	}

	values_cnt = rc;

	r = calloc(1, sizeof(*r) + sizeof(struct spdk_json_val) * (values_cnt + 1));
	/* [한국어] flexible array 포함 alloc — values_cnt+1: 마지막 sentinel 자리.
	 * calloc으로 zero-init — ready=false, buf=NULL 등. */
	if (!r) {
		return -errno;  /* [한국어] OOM */
	}

	if (client->resp) {
		/* [한국어] 사용자가 이전 응답을 get_response로 가져가지 않음 — 슬롯 점유.
		 * 새로 만든 r은 폐기 — 사용자 코드 버그. */
		free(r);
		return -ENOSPC;
	}

	client->resp = r;  /* [한국어] 슬롯 등록 */

	r->buf = client->recv_buf;     /* [한국어] recv_buf 소유권 이전 — values 토큰들이 이 buf 내부를 가리킴 */
	buf_len = client->recv_offset; /* [한국어] 누적된 byte 길이 — 2차 파싱 입력 */
	r->values_cnt = values_cnt;    /* [한국어] 토큰 개수 보관 */

	client->recv_buf_size = 0;     /* [한국어] client는 다음 응답에서 새 buf alloc하므로 capacity 0으로 */
	client->recv_offset = 0;       /* [한국어] 누적 위치 리셋 */
	client->recv_buf = NULL;       /* [한국어] 더 이상 client가 소유하지 않음 */

	/* Decode a second time now that there is a full JSON value available. */
	rc = spdk_json_parse(r->buf, buf_len, r->values, values_cnt, &end,
			     SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);
	/* [한국어] 2차 파싱 — IN_PLACE: string 토큰 끝에 NUL을 박음(zero-copy). */
	if (rc != (ssize_t)values_cnt) {
		/* [한국어] 1차/2차 토큰 개수 불일치 — 매우 드문 IN_PLACE 디코딩 이상. */
		SPDK_ERRLOG("JSON parse error on second pass (rc: %zd, expected: %zu)\n", rc, values_cnt);
		goto err;
	}

	assert(end != NULL);  /* [한국어] 2차 파싱 성공이면 end는 set */

	if (r->values[0].type != SPDK_JSON_VAL_OBJECT_BEGIN) {
		/* [한국어] JSON-RPC 응답은 반드시 객체 — 그 외(array/string/number)는 사양 위반. */
		SPDK_ERRLOG("top-level JSON value was not object\n");
		goto err;
	}

	if (spdk_json_decode_object(r->values, jsonrpc_response_decoders,
				    SPDK_COUNTOF(jsonrpc_response_decoders), &r->jsonrpc)) {
		/* [한국어] 4개 필드 디코딩 실패 — 필수 jsonrpc 누락, 잘못된 타입, unknown 키 등. */
		goto err;
	}

	r->ready = 1;  /* [한국어] 사용자에게 회수 가능한 상태로 마킹 */
	return 1;

err:
	client->resp = NULL;  /* [한국어] 슬롯 해제 — 다음 recv가 새 응답을 받을 수 있음 */
	spdk_jsonrpc_client_free_response(&r->jsonrpc);
	/* [한국어] 외부 노출 jsonrpc 필드를 통해 SPDK_CONTAINEROF로 r 자체 free + r->buf free. */
	return -EINVAL;
}

/*
 * [한국어]
 * jsonrpc_client_write_cb - client request 직렬화 sink (spdk_json_write_ctx 콜백)
 *
 * @cb_ctx: spdk_jsonrpc_client_request*.
 * @data: writer가 직렬화한 byte.
 * @size: byte 수.
 * @return: 0 성공, -ENOSPC(32MB 한도 초과), -ENOMEM(realloc 실패).
 *
 * 요청 직렬화 시 모든 spdk_json_write_*가 결국 이 콜백을 통해 send_buf에 byte를 쌓는다.
 * server 측 jsonrpc_server_write_cb와 동일한 capacity-doubling 전략 — 단,
 * 종단 NUL을 위한 +1 byte를 alloc하지 않는다(클라이언트는 NUL 종단 trace를 안 함).
 *
 * 호출 컨텍스트: spdk_jsonrpc_begin_request → 사용자 write 호출 →
 * spdk_jsonrpc_end_request 사이에서 호출됨 — 단일 thread.
 */
static int
jsonrpc_client_write_cb(void *cb_ctx, const void *data, size_t size)
{
	struct spdk_jsonrpc_client_request *request = cb_ctx;
	size_t new_size = request->send_buf_size;

	while (new_size - request->send_len < size) {
		if (new_size >= SPDK_JSONRPC_SEND_BUF_SIZE_MAX) {
			/* [한국어] 32MB 한도 — 비정상적으로 큰 요청 차단. */
			SPDK_ERRLOG("Send buf exceeded maximum size (%zu)\n",
				    (size_t)SPDK_JSONRPC_SEND_BUF_SIZE_MAX);
			return -ENOSPC;
		}

		new_size *= 2;  /* [한국어] capacity 2배 — amortized O(1) append */
	}

	if (new_size != request->send_buf_size) {
		uint8_t *new_buf;

		new_buf = realloc(request->send_buf, new_size);
		/* [한국어] 서버와 달리 +1 안 함 — 클라이언트는 send 시 NUL 종단을 사용하지 않음. */
		if (new_buf == NULL) {
			SPDK_ERRLOG("Resizing send_buf failed (current size %zu, new size %zu)\n",
				    request->send_buf_size, new_size);
			return -ENOMEM;
		}

		request->send_buf = new_buf;
		request->send_buf_size = new_size;
	}

	memcpy(request->send_buf + request->send_len, data, size);
	request->send_len += size;

	return 0;
}

/*
 * [한국어]
 * spdk_jsonrpc_begin_request - 클라이언트 요청 객체 prologue 작성 (공개 API)
 *
 * @request: 사용자가 client_create_request로 받은 빈 요청.
 * @id: 요청 식별자(>=0이면 포함, <0이면 notification으로 id 생략).
 * @method: 호출할 RPC 메서드 이름. NULL이면 method 필드 생략(흔치 않음 — chained
 *          빌드 시 사용자가 별도로 채울 수 있도록 한 escape hatch).
 * @return: writer 컨텍스트 — 사용자가 params 등을 이어 쓰는 데 사용. NULL=실패.
 *
 * 사용 패턴:
 *   req = spdk_jsonrpc_client_create_request();
 *   w = spdk_jsonrpc_begin_request(req, id, "bdev_get_bdevs");
 *   spdk_json_write_named_object_begin(w, "params"); ... ; spdk_json_write_object_end(w);
 *   spdk_jsonrpc_end_request(req, w);
 *   spdk_jsonrpc_client_send_request(client, req);
 *
 * 호출 체인: 사용자 → [이 함수] → spdk_json_write_begin (writer 생성) →
 * "jsonrpc"/"id"/"method" 필드 작성.
 */
struct spdk_json_write_ctx *
spdk_jsonrpc_begin_request(struct spdk_jsonrpc_client_request *request, int32_t id,
			   const char *method)
{
	struct spdk_json_write_ctx *w;

	w = spdk_json_write_begin(jsonrpc_client_write_cb, request, 0);
	/* [한국어] writer 컨텍스트 생성 — sink는 jsonrpc_client_write_cb로 등록. */
	if (w == NULL) {
		return NULL;  /* [한국어] OOM 등 */
	}

	spdk_json_write_object_begin(w);                     /* [한국어] '{' */
	spdk_json_write_named_string(w, "jsonrpc", "2.0");   /* [한국어] "jsonrpc":"2.0" */

	if (id >= 0) {
		/* [한국어] 정수 id를 명시 — 서버는 응답에서 같은 id를 echo. */
		spdk_json_write_named_int32(w, "id", id);
	}
	/* [한국어] id<0이면 id 필드 생략 — 사양 §4.1 notification으로 간주됨(서버는 응답 안 함). */

	if (method) {
		/* [한국어] method 이름 작성 — 일반적인 사용 경로. */
		spdk_json_write_named_string(w, "method", method);
	}
	/* [한국어] method=NULL은 거의 사용 안 함 — 사용자가 별도로 method를 추가하는 advanced 케이스. */

	return w;  /* [한국어] 사용자가 params 이어 쓰기에 사용 */
}

/*
 * [한국어]
 * spdk_jsonrpc_end_request - 요청 객체 epilogue + 메시지 구분자 (공개 API)
 *
 * @request: 진행 중 요청.
 * @w: begin_request가 반환한 writer.
 *
 * '}' 닫기 → writer 종료 → '\n' 추가. send_buf는 이 호출 후 socket으로 송신 가능.
 *
 * 호출 체인: 사용자 → [이 함수] → 사용자가 spdk_jsonrpc_client_send_request로 client에 등록.
 */
void
spdk_jsonrpc_end_request(struct spdk_jsonrpc_client_request *request, struct spdk_json_write_ctx *w)
{
	assert(w != NULL);

	spdk_json_write_object_end(w);   /* [한국어] '}' — 요청 객체 닫기 */
	spdk_json_write_end(w);          /* [한국어] writer 정리 */
	jsonrpc_client_write_cb(request, "\n", 1);
	/* [한국어] 메시지 boundary 힌트 newline — 서버 측 streaming 파서가 incomplete 검출에 도움. */
}

/* [한국어] SPDK_LOG_REGISTER_COMPONENT: "rpc_client" 로그 컴포넌트 등록.
 * SPDK_DEBUGLOG(rpc_client, ...)가 활성화될 때만 출력. */
SPDK_LOG_REGISTER_COMPONENT(rpc_client)
