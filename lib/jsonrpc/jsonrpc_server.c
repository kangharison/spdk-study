/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] JSON-RPC 2.0 서버 코어 엔진 (jsonrpc_server.c)
 *
 * === 파일의 역할 ===
 * JSON-RPC 2.0 사양(https://www.jsonrpc.org/specification)의 서버측 메시지 처리
 * 코어를 구현한다. transport에 무관한(소켓 코드 없음) 부분만 담당:
 *   (1) 수신한 텍스트를 spdk_json_parse로 두 번 파싱(증분 검출 + IN_PLACE 디코딩),
 *   (2) "jsonrpc/method/params/id" 4개 필드를 jsonrpc_request_decoders로 디코딩,
 *   (3) 메서드 디스패치(transport 측 함수 호출) 또는 에러 응답 빌드,
 *   (4) 핸들러가 응답을 작성할 spdk_json_write_ctx 제공 (begin_result/end_result),
 *   (5) bool/error 응답 같은 자주 쓰는 단축 빌더,
 *   (6) RPC trace 로그(g_rpc_log_level/g_rpc_log_file).
 * 실제 socket I/O와 큐 dispatch 로직은 jsonrpc_server_tcp.c에 분리되어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   spdk_jsonrpc_server_poll() (jsonrpc_server_tcp.c) →
 *   jsonrpc_server_conn_recv() →
 *   [이 파일] jsonrpc_parse_request() → parse_single_request() →
 *   [tcp.c] jsonrpc_server_handle_request() → server->handle_request 사용자 콜백 →
 *   (보통 spdk/rpc.c의 spdk_rpc_register_method 등록표 lookup) →
 *   사용자 RPC 핸들러 본체 →
 *   [이 파일] spdk_jsonrpc_begin_result() → JSON writer로 응답 작성 →
 *   spdk_jsonrpc_end_result() → end_response() → [tcp.c] jsonrpc_server_send_response().
 * 실행 컨텍스트: 단일 SPDK app thread(보통 reactor의 default poller). 다만 비동기
 * 핸들러가 다른 thread에서 begin_result/end_result를 호출하는 경우도 있어,
 * end_response → send_response 경로는 thread-safe하게 설계됨.
 *
 * === 타 모듈과의 연결 ===
 * 의존: jsonrpc_internal.h(공용 자료구조), spdk/json.h(파서/writer), spdk/util.h
 * (SPDK_COUNTOF/offsetof), spdk/log.h(SPDK_ERRLOG/DEBUGLOG 등).
 * 반대로 jsonrpc_server_tcp.c가 이 파일의 jsonrpc_parse_request, jsonrpc_free_request,
 * jsonrpc_complete_request, spdk_jsonrpc_send_error_response를 호출.
 * 데이터 흐름: tcp.c의 recv_buf → 본 파일 jsonrpc_parse_request → request->values
 * → 핸들러 → request->send_buf → tcp.c의 send.
 * 공유 자료구조: spdk_jsonrpc_request (recv 사본 + 토큰 배열 + 응답 buf/writer),
 * spdk_jsonrpc_server_conn (큐 등록·해제만 사용, 실제 socket은 tcp.c가 다룸).
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct jsonrpc_request (file-local): 요청 객체의 4개 필드를 decoder로
 *     캡처하기 위한 임시 컨테이너 — spdk_json_object_decoder + offsetof 패턴.
 *   - parse_single_request(): values[0]=object_begin인 단일 요청을 검증/디스패치.
 *   - jsonrpc_parse_request(): 두 번 파싱(incomplete 검출용 + IN_PLACE 디코딩) 후
 *     batch(array)는 미지원으로 거부, 단일 요청은 parse_single_request로 위임.
 *   - jsonrpc_server_write_cb(): writer가 직렬화 byte를 send_buf에 쌓을 때 호출,
 *     필요시 send_buf_size를 2배씩 realloc(상한 32MB).
 *   - begin_response()/end_response()/skip_response(): 응답 객체의 공통 prologue/
 *     epilogue. id가 없으면 notification으로 간주해 응답을 보내지 않음(skip).
 *   - spdk_jsonrpc_begin_result/_end_result/_send_bool_response/_send_error_response*:
 *     사용자 핸들러가 호출하는 응답 빌더 API.
 *   - jsonrpc_log(): RPC trace 로깅(개행 제거 + spdk_log/spdk_flog).
 *   - g_rpc_log_level/_file: 모듈 전역 — spdk_jsonrpc_set_log_level/_file로 설정.
 */

#include "jsonrpc_internal.h"      /* [한국어] 본 라이브러리 내부 자료구조와 함수 시그니처 */

#include "spdk/util.h"             /* [한국어] SPDK_COUNTOF, offsetof, SPDK_CONTAINEROF 등 매크로 */

/* [한국어] 모듈 전역 — RPC trace 로그의 로그 레벨(디스에이블 = 로그 출력 안 함). */
static enum spdk_log_level g_rpc_log_level = SPDK_LOG_DISABLED;

/* [한국어] 모듈 전역 — RPC trace 로그를 별도 파일에 기록하려는 경우의 FILE*.
 * NULL이면 별도 파일 출력 안 함(spdk_log만 사용). */
static FILE *g_rpc_log_file = NULL;

/*
 * [한국어]
 * struct jsonrpc_request (file-local 임시 구조체)
 *
 * spdk_json_decode_object 패턴에서 한 객체의 필드들을 한 번에 캡처하기 위한
 * 컨테이너. 4개 필드는 모두 const struct spdk_json_val * 포인터로, 디코더가
 * 원본 토큰 배열 내부 위치를 그대로 가리키도록 한다(zero-copy in-place).
 * spdk_jsonrpc_request(공용 자료구조)와는 별개 — 이름만 비슷하다.
 */
struct jsonrpc_request {
	const struct spdk_json_val *version;
	/* [한국어] "jsonrpc" 필드 — JSON-RPC 2.0에서 반드시 "2.0" 문자열.
	 * 설정자: capture_val 콜백, 읽는 자: parse_single_request에서 strequal 검사. */

	const struct spdk_json_val *method;
	/* [한국어] "method" 필드 — 호출할 RPC 메서드 이름 문자열. 필수.
	 * jsonrpc_server_handle_request로 전달되는 인자. */

	const struct spdk_json_val *params;
	/* [한국어] "params" 필드 — object 또는 array, 또는 null/없음.
	 * 핸들러가 spdk_json_decode_object 등으로 파라미터를 디코딩하는 데 사용. */

	const struct spdk_json_val *id;
	/* [한국어] "id" 필드 — string/number/null. 없거나 null이면 notification으로 간주. */
};

/*
 * [한국어]
 * spdk_jsonrpc_set_log_level - RPC trace 로그의 로그 레벨 전역 설정
 *
 * @level: SPDK_LOG_DISABLED ~ SPDK_LOG_DEBUG 사이.
 *         DISABLED면 trace 출력 비활성, INFO/DEBUG 등이면 jsonrpc_log()가
 *         spdk_log() 경로로 요청/응답을 출력.
 *
 * 디버깅이나 RPC 호출 추적이 필요할 때 사용자가 호출. 모든 server 인스턴스에
 * 영향을 주는 전역 — 단일 thread에서만 사용한다는 라이브러리 전제 하에 락 없이 안전.
 *
 * 호출 체인: 사용자 코드(예 spdk_app 옵션 처리) → [이 함수] → g_rpc_log_level 갱신.
 */
void
spdk_jsonrpc_set_log_level(enum spdk_log_level level)
{
	assert(level >= SPDK_LOG_DISABLED);  /* [한국어] 인자 범위 하한 검증 */
	assert(level <= SPDK_LOG_DEBUG);     /* [한국어] 인자 범위 상한 검증 */
	g_rpc_log_level = level;             /* [한국어] 전역 변수 갱신 — 이후 호출되는 jsonrpc_log()부터 적용 */
}

/*
 * [한국어]
 * spdk_jsonrpc_set_log_file - RPC trace 로그를 별도 파일에도 출력하도록 등록
 *
 * @file: 사용자가 fopen한 FILE 핸들. NULL이면 파일 출력 비활성.
 *        파일 close 책임은 호출자(이 함수는 핸들 소유권을 가져오지 않음).
 *
 * 호출 체인: 사용자 코드 → [이 함수] → g_rpc_log_file 포인터 갱신.
 */
void
spdk_jsonrpc_set_log_file(FILE *file)
{
	g_rpc_log_file = file;               /* [한국어] 별도 파일 출력 핸들 등록(NULL=비활성) */
}

/*
 * [한국어]
 * remove_newlines - 문자열에서 모든 '\n'을 in-place로 제거
 *
 * @text: NUL 종단 문자열(파괴적 수정 — buf 자체가 변경됨).
 *
 * RPC trace 로그 한 줄을 깔끔히 출력하기 위한 헬퍼. JSON 본문에 개행이 포함되면
 * 로그 한 줄에 여러 줄로 펼쳐져 가독성을 해치므로 압축한다. j(write index) ≤
 * i(read index) 불변식이 유지되어 in-place 안전.
 */
static void
remove_newlines(char *text)
{
	int i = 0, j = 0;                    /* [한국어] i=read, j=write index — j는 항상 i보다 작거나 같음 */

	while (text[i] != '\0') {            /* [한국어] NUL 종단까지 순회 */
		if (text[i] != '\n') {       /* [한국어] 개행이 아닐 때만 복사(개행은 skip) */
			text[j++] = text[i]; /* [한국어] write 후 write index 증가 */
		}
		i++;                         /* [한국어] read index는 항상 증가 */
	}
	text[j] = '\0';                      /* [한국어] 압축된 문자열의 새 끝에 NUL */
}

/*
 * [한국어]
 * jsonrpc_log - 요청/응답 trace 1줄을 spdk_log/file로 출력
 *
 * @buf: trace할 raw JSON 텍스트(파괴적으로 개행이 제거될 수 있음).
 * @prefix: "request: " 또는 "response: " 같은 prefix 문자열.
 *
 * RPC 추적이 활성일 때만 호출되며(g_rpc_log_level != DISABLED 또는 file != NULL),
 * 가독성을 위해 개행을 제거한다. ALLOW_COMMENTS 옵션이 켜진 사용자가 있어,
 * trace 비활성 상태에서는 buf를 건드리지 않는다(원본 보존).
 */
static void
jsonrpc_log(char *buf, const char *prefix)
{
	/* Some custom applications have enabled SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS
	 * to allow comments in JSON RPC objects. To keep backward compatibility of
	 * these applications, remove newlines only if JSON RPC logging is enabled.
	 */
	if (g_rpc_log_level != SPDK_LOG_DISABLED || g_rpc_log_file != NULL) {
		/* [한국어] 어느 쪽이든 trace가 활성 상태일 때만 개행 제거 — 비파괴 fast-path 보존 */
		remove_newlines(buf);
	}

	if (g_rpc_log_level != SPDK_LOG_DISABLED) {
		/* [한국어] SPDK 로그 시스템으로 출력 — 보통 stderr/syslog로 라우팅 */
		spdk_log(g_rpc_log_level, NULL, 0, NULL, "%s%s\n", prefix, buf);
	}

	if (g_rpc_log_file != NULL) {
		/* [한국어] 별도 파일이 등록되어 있으면 추가로 그 파일에도 기록 */
		spdk_flog(g_rpc_log_file, NULL, 0, NULL, "%s%s\n", prefix, buf);
	}
}

/*
 * [한국어]
 * capture_val - spdk_json_object_decoder 콜백 — 토큰 포인터를 그대로 캡처
 *
 * @val: 디코더가 매치한 토큰 — string/number/object_begin 등.
 * @out: struct jsonrpc_request의 해당 필드 주소(offsetof 기반).
 * @return: 항상 0(성공). 디코더 시그니처상 음수 반환은 디코딩 실패.
 *
 * spdk_json_decode_object의 일반적 패턴으로, 필드를 zero-copy로 받아낸 후
 * 후속 검증(타입 검사, strequal 비교)을 호출자에서 수행한다.
 */
static int
capture_val(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;  /* [한국어] out은 (const spdk_json_val**) 위치 */

	*vptr = val;                              /* [한국어] 토큰 포인터를 그대로 저장 */
	return 0;                                 /* [한국어] 항상 성공 */
}

/* [한국어] JSON-RPC 요청 객체의 4개 표준 필드를 디코딩하기 위한 정의표.
 * 각 entry: { 필드명, struct jsonrpc_request 내 offset, 디코더 함수, optional 플래그 }.
 * "method"는 필수(optional 미지정), 나머지는 optional(없어도 OK). */
static const struct spdk_json_object_decoder jsonrpc_request_decoders[] = {
	{"jsonrpc", offsetof(struct jsonrpc_request, version), capture_val, true},
	/* [한국어] "jsonrpc": "2.0" — optional 처리 후 parse_single_request에서 값 검증 */
	{"method", offsetof(struct jsonrpc_request, method), capture_val},
	/* [한국어] "method": string — 필수 */
	{"params", offsetof(struct jsonrpc_request, params), capture_val, true},
	/* [한국어] "params": object/array/null — optional */
	{"id", offsetof(struct jsonrpc_request, id), capture_val, true},
	/* [한국어] "id": string/number/null — optional(없으면 notification) */
};

/*
 * [한국어]
 * parse_single_request - 객체 형태의 단일 JSON-RPC 요청을 검증하고 디스패치
 *
 * @request: jsonrpc_parse_request가 갓 채운 spdk_jsonrpc_request — 빈 응답 buf와
 *           파싱된 values 배열을 가진 상태.
 * @values: 토큰 배열의 시작(values[0]은 OBJECT_BEGIN으로 사전 검증된 상태).
 *
 * 사양 §4 "Request object" 검증 단계:
 *   1) decoder로 jsonrpc/method/params/id 4개 필드 추출(unknown 키는 거절).
 *   2) "jsonrpc"가 있으면 string 타입의 "2.0"이어야 함.
 *   3) "method"는 string 필수.
 *   4) "id"가 있으면 string/number/null 중 하나 — 그 외 타입은 거부.
 *   5) "params"가 있으면 object/array/null. null이면 "파라미터 없음"으로 동등 처리.
 * 모든 검증 통과 시 jsonrpc_server_handle_request(transport-side)로 디스패치,
 * 어느 단계든 실패하면 INVALID_REQUEST 에러 응답.
 *
 * 호출 체인:
 *   jsonrpc_parse_request → [이 함수] → jsonrpc_server_handle_request
 *                                    → 실패 시 jsonrpc_server_handle_error
 */
static void
parse_single_request(struct spdk_jsonrpc_request *request, struct spdk_json_val *values)
{
	struct jsonrpc_request req = {};                /* [한국어] decoder 결과를 받을 임시 컨테이너 (모두 NULL 초기화) */
	const struct spdk_json_val *params = NULL;      /* [한국어] 핸들러로 전달할 params(없거나 null이면 NULL 유지) */

	if (spdk_json_decode_object(values, jsonrpc_request_decoders,
				    SPDK_COUNTOF(jsonrpc_request_decoders),
				    &req)) {
		/* [한국어] 디코딩 실패: unknown key, missing required("method"), 잘못된 타입 등.
		 * 단일 분기로 INVALID_REQUEST 처리. */
		goto invalid;
	}

	if (req.version && (req.version->type != SPDK_JSON_VAL_STRING ||
			    !spdk_json_strequal(req.version, "2.0"))) {
		/* [한국어] "jsonrpc" 필드가 있는데 string "2.0"이 아니면 사양 위반. */
		goto invalid;
	}

	if (!req.method || req.method->type != SPDK_JSON_VAL_STRING) {
		/* [한국어] "method" 누락이거나 string이 아님 — 사양 §4에서 method는 필수 string. */
		goto invalid;
	}

	if (req.id) {
		/* [한국어] id가 존재하면 사양 §4상 string/number/null만 허용.
		 * (사양은 fractional number 비권장이지만 SPDK는 허용.) */
		if (req.id->type == SPDK_JSON_VAL_STRING ||
		    req.id->type == SPDK_JSON_VAL_NUMBER ||
		    req.id->type == SPDK_JSON_VAL_NULL) {
			request->id = req.id;            /* [한국어] 응답 작성 시 echo할 id 토큰 보관 */
		} else  {
			/* [한국어] object/array 등 타입은 거절 */
			goto invalid;
		}
	}
	/* [한국어] req.id가 NULL인 경우는 notification(사양 §4.1) — request->id가 NULL로 남고
	 * end_response 단계에서 응답 송신을 skip(skip_response). */

	if (req.params) {
		/* null json value is as if there were no parameters */
		if (req.params->type != SPDK_JSON_VAL_NULL) {
			/* [한국어] null이면 "파라미터 없음"과 동등 — 핸들러에 NULL 전달.
			 * 그 외에는 object/array만 허용(사양 §4.2). */
			if (req.params->type != SPDK_JSON_VAL_ARRAY_BEGIN &&
			    req.params->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
				goto invalid;
			}
			params = req.params;             /* [한국어] 핸들러에 전달할 params 토큰 — 핸들러는 spdk_json_decode_object 등으로 해석 */
		}
	}

	/* [한국어] 모든 검증 통과 — transport-side 디스패치 함수 호출.
	 * 정의는 jsonrpc_server_tcp.c → 사용자가 server_listen 시 등록한 handle_request 콜백으로 진입. */
	jsonrpc_server_handle_request(request, req.method, params);
	return;

invalid:
	/* [한국어] 어느 검증이든 실패하면 표준 에러 코드 -32600(invalid request)로 응답 송신. */
	jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_INVALID_REQUEST);
}

/*
 * [한국어]
 * jsonrpc_server_write_cb - spdk_json_write_ctx의 write callback (응답 직렬화 sink)
 *
 * @cb_ctx: spdk_json_write_begin 호출 시 등록한 컨텍스트 — 여기서는 spdk_jsonrpc_request*.
 * @data: writer가 직렬화한 byte 청크.
 * @size: byte 수.
 * @return: 0 성공, -1 실패(send buf 한도 초과 또는 realloc 실패).
 *
 * spdk_json_write_named_string 등 모든 writer 호출은 결국 이 콜백으로 byte를 흘려보낸다.
 * 잔여 공간이 부족하면 send_buf_size를 2배씩 늘려 capacity를 확보 — 단, 32MB 상한을
 * 넘으면 거부(잘못된 핸들러나 비정상적으로 큰 응답 차단). 두 단계로 나뉘어 있다:
 *   (1) capacity check 루프(필요 크기 계산),
 *   (2) realloc(필요 시) → memcpy → send_len 갱신.
 *
 * 호출 컨텍스트: spdk_json_write_* 함수의 내부 — 보통 사용자 RPC 핸들러 thread.
 */
static int
jsonrpc_server_write_cb(void *cb_ctx, const void *data, size_t size)
{
	struct spdk_jsonrpc_request *request = cb_ctx;  /* [한국어] cb_ctx에서 request 복원 */
	size_t new_size = request->send_buf_size;       /* [한국어] 새 buf 크기 후보 (필요 시 2배씩 키움) */

	while (new_size - request->send_len < size) {
		/* [한국어] 잔여 공간 < 추가될 byte 수면 더 큰 capacity 필요 */
		if (new_size >= SPDK_JSONRPC_SEND_BUF_SIZE_MAX) {
			/* [한국어] 32MB 한도 초과 — 응답 자체를 작성할 수 없으므로 -1 반환 */
			SPDK_ERRLOG("Send buf exceeded maximum size (%zu)\n",
				    (size_t)SPDK_JSONRPC_SEND_BUF_SIZE_MAX);
			return -1;
		}

		new_size *= 2;                          /* [한국어] capacity 2배로 grow — amortized O(1) append 보장 */
	}

	if (new_size != request->send_buf_size) {
		/* [한국어] capacity 변경 필요 — realloc 시도 */
		uint8_t *new_buf;

		/* Add extra byte for the null terminator. */
		new_buf = realloc(request->send_buf, new_size + 1);  /* [한국어] +1: 종단 NUL 자리(jsonrpc_server_conn_send에서 활용) */
		if (new_buf == NULL) {
			/* [한국어] OOM — request의 기존 send_buf는 그대로 두고 -1 반환(호출자가 cleanup). */
			SPDK_ERRLOG("Resizing send_buf failed (current size %zu, new size %zu)\n",
				    request->send_buf_size, new_size);
			return -1;
		}

		request->send_buf = new_buf;            /* [한국어] 확장된 buf 포인터로 갱신 */
		request->send_buf_size = new_size;      /* [한국어] capacity 갱신 */
	}

	memcpy(request->send_buf + request->send_len, data, size);  /* [한국어] writer 청크를 send_buf 끝에 추가 */
	request->send_len += size;                  /* [한국어] 유효 byte 수 갱신 */

	return 0;                                   /* [한국어] 성공 — writer가 다음 청크를 보낼 수 있음 */
}

/*
 * [한국어]
 * jsonrpc_parse_request - recv 버퍼의 한 chunk에서 JSON-RPC 요청 1건을 파싱·디스패치
 *
 * @conn: 이 요청이 도착한 server-side 연결.
 * @json: recv_buf 내 시작 포인터(전 호출에서 소비 안 한 잔여를 포함할 수 있음).
 * @size: 그 시작점부터 사용 가능한 byte 수.
 * @return: 소비한 byte 수(>0) — caller(server_tcp.c)는 이 만큼 offset 이동 후 재호출.
 *          0 = INCOMPLETE(더 받아야 함, caller는 다음 recv 대기).
 *          -1 = 치명적 에러(메모리 부족, parse error 등) — connection 종료해야 함.
 *
 * 동작 단계:
 *   1) spdk_json_parse(NULL, 0, NULL)로 토큰 개수만 미리 파악(non-IN_PLACE).
 *      INCOMPLETE면 0 반환 — recv 버퍼에 더 채워질 때까지 대기.
 *   2) request 구조체 calloc, outstanding_queue에 등록 (queue_lock 보호).
 *   3) recv_buffer에 raw JSON 사본 생성(NUL 종단 포함) → trace 로깅.
 *   4) values 배열 malloc — 토큰 개수에 맞춰.
 *   5) send_buf 초기 크기로 malloc, response writer 생성.
 *   6) parse error/한도 초과면 PARSE_ERROR 응답 후 -1 반환(스트리밍 JSON에서는
 *      복구 동기화점이 없어 connection을 닫아야 안전).
 *   7) 두 번째 파싱은 IN_PLACE — spdk_json_val의 string/number 토큰이 recv_buffer의
 *      특정 byte 범위를 가리키도록 in-place로 NUL을 박는다. zero-copy 디코딩 핵심.
 *   8) 최상위 토큰이 OBJECT_BEGIN이면 단일 요청, ARRAY_BEGIN이면 batch(미지원),
 *      그 외는 INVALID_REQUEST.
 *
 * 호출 체인:
 *   jsonrpc_server_conn_recv() → [이 함수] → parse_single_request() →
 *                                        → jsonrpc_server_handle_request()
 *                                        → 또는 jsonrpc_server_handle_error()
 */
int
jsonrpc_parse_request(struct spdk_jsonrpc_server_conn *conn, const void *json, size_t size)
{
	struct spdk_jsonrpc_request *request;
	ssize_t rc;
	size_t len;
	void *end = NULL;                       /* [한국어] spdk_json_parse가 갱신 — 파싱이 끝난 byte 위치 */

	/* Check to see if we have received a full JSON value. It is safe to cast away const
	 * as we don't decode in place. */
	rc = spdk_json_parse((void *)json, size, NULL, 0, &end, 0);
	/* [한국어] 1차 파싱: 토큰 배열 NULL — 토큰 개수 카운트와 incomplete 검출 용도.
	 * IN_PLACE 플래그 없으므로 buf 비파괴 — const 벗기기는 안전. */
	if (rc == SPDK_JSON_PARSE_INCOMPLETE) {
		/* [한국어] JSON이 끝나지 않음 — caller는 이 chunk를 그대로 두고 다음 recv 대기. */
		return 0;
	}

	request = calloc(1, sizeof(*request));
	if (request == NULL) {
		/* [한국어] OOM: 요청 객체 자체를 만들 수 없음 — connection 종료가 안전. */
		SPDK_DEBUGLOG(rpc, "Out of memory allocating request\n");
		return -1;
	}

	pthread_spin_lock(&conn->queue_lock);
	/* [한국어] outstanding_queue 조작 보호 — send_response 등 다른 thread와 경쟁 가능. */
	conn->outstanding_requests++;           /* [한국어] in-flight 카운트 증가 (poll loop의 종료 조건과 연동) */
	STAILQ_INSERT_TAIL(&conn->outstanding_queue, request, link);
	/* [한국어] 처리 중 큐의 tail에 등록 — 응답이 완성되면 send_queue로 옮겨진다. */
	pthread_spin_unlock(&conn->queue_lock);

	request->conn = conn;                   /* [한국어] 응답 송신 시점에 어느 conn으로 보낼지 알기 위한 역참조 */

	len = end - json;                       /* [한국어] 1차 파싱이 소비한 byte 수 — caller 반환값 */
	request->recv_buffer = malloc(len + 1); /* [한국어] +1: NUL 종단 자리 */
	if (request->recv_buffer == NULL) {
		SPDK_ERRLOG("Failed to allocate buffer to copy request (%zu bytes)\n", len + 1);
		jsonrpc_free_request(request);  /* [한국어] outstanding_queue에서 제거하고 calloc된 request free */
		return -1;
	}

	memcpy(request->recv_buffer, json, len);
	request->recv_buffer[len] = '\0';       /* [한국어] in-place 디코딩이 string 경계를 NUL로 박을 것 — 시작 시 종단 보장 */

	jsonrpc_log(request->recv_buffer, "request: ");
	/* [한국어] trace 로그 출력 — g_rpc_log_level/file 설정 시. 호출 후 buf의 개행이 제거될 수 있음. */

	if (rc > 0 && rc <= SPDK_JSONRPC_MAX_VALUES) {
		/* [한국어] 정상적인 토큰 개수 범위 — values 배열 할당.
		 * rc <= 0 또는 한도 초과는 아래에서 별도 처리. */
		request->values_cnt = rc;
		request->values = malloc(request->values_cnt * sizeof(request->values[0]));
		if (request->values == NULL) {
			SPDK_ERRLOG("Failed to allocate buffer for JSON values (%zu bytes)\n",
				    request->values_cnt * sizeof(request->values[0]));
			jsonrpc_free_request(request);
			return -1;
		}
	}

	request->send_offset = 0;               /* [한국어] tcp send 루프의 부분 송신 cursor 초기화 */
	request->send_len = 0;                  /* [한국어] 응답 직렬화 byte 수 0에서 시작 */
	request->send_buf_size = SPDK_JSONRPC_SEND_BUF_SIZE_INIT;  /* [한국어] 32KB 초기 capacity */
	/* Add extra byte for the null terminator. */
	request->send_buf = malloc(request->send_buf_size + 1);    /* [한국어] +1: 송신 시점 종단 NUL */
	if (request->send_buf == NULL) {
		SPDK_ERRLOG("Failed to allocate send_buf (%zu bytes)\n", request->send_buf_size);
		jsonrpc_free_request(request);
		return -1;
	}

	request->response = spdk_json_write_begin(jsonrpc_server_write_cb, request, 0);
	/* [한국어] writer 컨텍스트 생성 — write_cb로 jsonrpc_server_write_cb 등록.
	 * 이후 핸들러가 spdk_json_write_*를 호출하면 결국 send_buf에 byte가 누적됨. */
	if (request->response == NULL) {
		SPDK_ERRLOG("Failed to allocate response JSON write context.\n");
		jsonrpc_free_request(request);
		return -1;
	}

	if (rc <= 0 || rc > SPDK_JSONRPC_MAX_VALUES) {
		/* [한국어] 1차 파싱이 INVALID(rc<0) 또는 토큰 한도 초과(rc>1024)인 경우.
		 * 여기까지 와서 처리하는 이유: 응답을 보내려면 request 구조와 writer가 필요하므로
		 * 위 setup을 먼저 해두고 에러 응답을 작성한다. */
		SPDK_DEBUGLOG(rpc, "JSON parse error\n");
		jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_PARSE_ERROR);

		/*
		 * Can't recover from parse error (no guaranteed resync point in streaming JSON).
		 * Return an error to indicate that the connection should be closed.
		 */
		/* [한국어] 스트리밍 JSON에서는 어디부터 다음 메시지인지 동기화점이 없으므로 conn 종료. */
		return -1;
	}

	/* Decode a second time now that there is a full JSON value available. */
	rc = spdk_json_parse(request->recv_buffer, size, request->values, request->values_cnt, &end,
			     SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);
	/* [한국어] 2차 파싱: IN_PLACE 모드 — string 토큰의 끝 자리에 NUL을 박아 zero-copy 추출.
	 * 이로 인해 recv_buffer는 더 이상 원본 raw JSON이 아니지만, 토큰 포인터들은 유효. */
	if (rc < 0 || rc > SPDK_JSONRPC_MAX_VALUES) {
		SPDK_DEBUGLOG(rpc, "JSON parse error on second pass\n");
		/* [한국어] 1차에서는 통과했지만 2차에서 실패 — IN_PLACE 디코딩 특이 케이스(드뭄). */
		jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_PARSE_ERROR);
		return -1;
	}

	assert(end != NULL);                    /* [한국어] 2차 파싱이 성공했으면 end는 반드시 set */

	if (request->values[0].type == SPDK_JSON_VAL_OBJECT_BEGIN) {
		/* [한국어] 정상 케이스 — 단일 요청 객체. */
		parse_single_request(request, request->values);
	} else if (request->values[0].type == SPDK_JSON_VAL_ARRAY_BEGIN) {
		/* [한국어] 사양 §6 batch 요청은 SPDK가 미지원 — 거부.
		 * (rpc.py도 batch를 보내지 않음.) */
		SPDK_DEBUGLOG(rpc, "Got batch array (not currently supported)\n");
		jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	} else {
		/* [한국어] 최상위가 string/number/bool 등인 경우 — 사양 위반. */
		SPDK_DEBUGLOG(rpc, "top-level JSON value was not array or object\n");
		jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	}

	return len;                             /* [한국어] caller에 소비한 byte 수 알림 — recv_buf에서 이만큼 압축됨 */
}

/*
 * [한국어]
 * spdk_jsonrpc_get_conn - 요청에 연결된 conn 핸들 반환(공개 API)
 *
 * @request: 진행 중 RPC 요청.
 * @return: 이 요청이 도착한 server-side 연결 핸들. 핸들러가 close_cb를 등록하거나
 *          연결 종료 시점을 추적하고 싶을 때 사용.
 *
 * 호출 체인: 사용자 RPC 핸들러 → [이 함수] → request->conn 반환.
 */
struct spdk_jsonrpc_server_conn *
spdk_jsonrpc_get_conn(struct spdk_jsonrpc_request *request)
{
	return request->conn;                   /* [한국어] 단순 getter — request 생성 시 저장된 conn 포인터 반환 */
}

/* Never return NULL */
/*
 * [한국어]
 * begin_response - 응답 객체의 공통 prologue 작성("jsonrpc":"2.0", "id":...)
 *
 * @request: 진행 중 RPC 요청.
 * @return: 핸들러가 result/error 본문을 이어 쓸 수 있는 spdk_json_write_ctx.
 *          NULL을 반환하지 않는다(주석 명시) — request->response는 parse 단계에서 보장됨.
 *
 * JSON-RPC 2.0 사양 §5: 응답은 jsonrpc/id 필드를 항상 포함, result 또는 error
 * 중 하나만 포함. id는 요청의 id를 그대로 echo, 없거나 invalid면 null.
 *
 * 호출 체인:
 *   spdk_jsonrpc_begin_result(),
 *   spdk_jsonrpc_send_error_response(),
 *   spdk_jsonrpc_send_error_response_fmt() →
 *   [이 함수] → 핸들러가 result/error body 추가.
 */
static struct spdk_json_write_ctx *
begin_response(struct spdk_jsonrpc_request *request)
{
	struct spdk_json_write_ctx *w = request->response;  /* [한국어] parse 시 등록된 writer 컨텍스트 */

	/* The assertion below ensures that no response data has been written yet.
	 * Otherwise, it would result in malformed JSON.
	 */
	assert(request->send_len == 0);
	/* [한국어] 응답 시작 시점에는 send_buf가 비어 있어야 함 — 누군가 미리 쓰면 JSON 깨짐.
	 * (예외: send_error_response는 호출 전에 jsonrpc_reset_response로 강제 0으로 만든다.) */

	spdk_json_write_object_begin(w);                /* [한국어] '{' 작성 */
	spdk_json_write_named_string(w, "jsonrpc", "2.0");  /* [한국어] "jsonrpc":"2.0" — 사양 필수 */

	spdk_json_write_name(w, "id");                  /* [한국어] "id": 작성 — 값은 다음 if에서 */
	if (request->id) {
		/* [한국어] 요청의 id 토큰을 그대로 출력(zero-copy echo). */
		spdk_json_write_val(w, request->id);
	} else {
		/* [한국어] 요청에 id가 없었거나 캡처되지 않음 — null로 응답(사양 §5).
		 * (단, notification = id 없음 케이스는 begin_response 자체가 호출되기 전에
		 *  end_response가 skip_response를 선택하지만, 에러 응답 시에는 항상 호출.) */
		spdk_json_write_null(w);
	}

	return w;                                       /* [한국어] 핸들러가 result/error 본문을 이어 쓸 writer 반환 */
}

/*
 * [한국어]
 * skip_response - 응답 송신을 생략(notification 처리)
 *
 * @request: 진행 중 RPC 요청.
 *
 * JSON-RPC 2.0 사양 §4.1: id 없는 notification에는 응답하지 않는다.
 * begin_response 직전에 호출되면 send_buf가 비어 있고 writer만 종료시키면 됨.
 * begin_result/end_result 후에 알게 되었다면 send_len을 0으로 리셋하여 누적된
 * 부분 출력을 폐기 후 빈 응답을 send_response에 넘긴다 — tcp 측에서 send_len==0
 * 응답은 그냥 free되며 byte는 나가지 않는다(send 루프 if check).
 *
 * 호출 체인:
 *   spdk_jsonrpc_end_result() → [이 함수] (id 없는 경우) → jsonrpc_server_send_response().
 */
static void
skip_response(struct spdk_jsonrpc_request *request)
{
	request->send_len = 0;                          /* [한국어] 누적된 출력 폐기 — 빈 응답으로 만듦 */
	spdk_json_write_end(request->response);         /* [한국어] writer 정리(메모리 해제) */
	request->response = NULL;                       /* [한국어] 사후 조건: response writer 종료 마킹 */
	jsonrpc_server_send_response(request);          /* [한국어] queue 이동 후 tcp 측이 free_request — byte는 나가지 않음 */
}

/*
 * [한국어]
 * end_response - 응답 객체의 공통 epilogue 작성 후 송신 큐에 등록
 *
 * @request: 진행 중 RPC 요청 — begin_response 후 핸들러가 result/error 본문을 채운 상태.
 *
 * 객체 닫기('}'), writer 종료, 메시지 구분자 '\n' 추가, send_queue 이동 순서로 마무리.
 * '\n'은 streaming 환경에서 메시지 boundary 힌트로 동작(JSON 자체 종결자는 아니지만 가독성/디버깅용).
 *
 * 호출 체인:
 *   spdk_jsonrpc_end_result() (id 있는 경우),
 *   spdk_jsonrpc_send_error_response(),
 *   spdk_jsonrpc_send_error_response_fmt() →
 *   [이 함수] → jsonrpc_server_send_response() (queue 이동, 이후 tcp send 루프).
 */
static void
end_response(struct spdk_jsonrpc_request *request)
{
	spdk_json_write_object_end(request->response);  /* [한국어] '}' — 응답 객체 닫기 */
	spdk_json_write_end(request->response);         /* [한국어] writer 종료(인덴트/스택 정리, 메모리 해제) */
	request->response = NULL;                       /* [한국어] 사후 조건: response 사용 종료 표시 */

	jsonrpc_server_write_cb(request, "\n", 1);      /* [한국어] 메시지 끝에 newline 추가 — 디버깅/사람 가독성 */
	jsonrpc_server_send_response(request);          /* [한국어] outstanding→send 큐 이동, tcp poll 루프가 송신 */
}

/*
 * [한국어]
 * jsonrpc_free_request - 요청 객체와 모든 동적 메모리 해제
 *
 * @request: 해제 대상. NULL이면 no-op(방어적 처리).
 *
 * 정상 경로: send_response 후 tcp send 루프가 jsonrpc_complete_request → 이 함수 호출.
 * 비정상 경로: parse 단계 OOM, conn 강제 종료 시 cleanup으로도 호출.
 *
 * outstanding_queue에 들어 있다면 STAILQ에서 제거 + outstanding_requests 감소 —
 * queue_lock(spinlock) 보호. conn==NULL이면 (연결이 이미 닫힘) 큐 작업은 skip하고
 * 메모리만 해제. recv_buffer/values/send_buf/request 자체 모두 free.
 *
 * 호출 컨텍스트: server poll thread만(헤더 명시). cross-thread free는 use-after-free 위험.
 */
void
jsonrpc_free_request(struct spdk_jsonrpc_request *request)
{
	struct spdk_jsonrpc_request *req;
	struct spdk_jsonrpc_server_conn *conn;

	if (!request) {
		/* [한국어] NULL 입력 방어 — server_tcp.c가 send_request==NULL일 때도 호출하기 때문 */
		return;
	}

	/* We must send or skip response explicitly */
	assert(request->response == NULL);
	/* [한국어] response writer가 NULL이라는 것은 end/skip이 호출되어 응답이 마감됐음을 보장.
	 * 그렇지 않다면 핸들러가 응답을 보내지 않은 상태로 free하는 버그. */

	conn = request->conn;
	if (conn != NULL) {
		/* [한국어] 정상 경로 — conn이 살아 있으면 큐에서 빼야 함. */
		pthread_spin_lock(&conn->queue_lock);
		conn->outstanding_requests--;            /* [한국어] in-flight 카운트 감소 */
		STAILQ_FOREACH(req, &conn->outstanding_queue, link) {
			if (req == request) {
				/* [한국어] outstanding_queue에 아직 있다면 거기서 제거.
				 * send_response가 이미 send_queue로 옮겼다면 여기서 못 찾음 — 그래도 정상. */
				STAILQ_REMOVE(&conn->outstanding_queue,
					      req, spdk_jsonrpc_request, link);
				break;
			}
		}
		pthread_spin_unlock(&conn->queue_lock);
	}
	/* [한국어] conn==NULL인 경우는 연결이 이미 닫혀 큐 자체가 무효 — 메모리만 free. */
	free(request->recv_buffer);                      /* [한국어] raw JSON 사본 해제 (NULL이어도 free 안전) */
	free(request->values);                           /* [한국어] 토큰 배열 해제 */
	free(request->send_buf);                         /* [한국어] 송신 버퍼 해제 */
	free(request);                                   /* [한국어] 요청 구조체 자체 해제 */
}

/*
 * [한국어]
 * jsonrpc_complete_request - 응답 송신 완료 후의 마무리
 *
 * @request: 응답이 전부 socket으로 나간 후의 요청.
 *
 * trace 로그 기록 후 free_request로 정리. 호출자는 jsonrpc_server_conn_send().
 */
void
jsonrpc_complete_request(struct spdk_jsonrpc_request *request)
{
	jsonrpc_log(request->send_buf, "response: ");
	/* [한국어] 송신된 응답을 trace 로그에 기록 — g_rpc_log_level/file 설정 시. */

	jsonrpc_free_request(request);
	/* [한국어] outstanding_queue에서 빠진 상태이지만 실제 free는 여기서 — buf들도 함께 해제. */
}

/*
 * [한국어]
 * spdk_jsonrpc_begin_result - 핸들러가 정상 result 본문을 작성하기 시작 (공개 API)
 *
 * @request: 디스패치된 요청.
 * @return: result 값을 이어 쓸 spdk_json_write_ctx.
 *
 * 사용 패턴: w = begin_result; spdk_json_write_*; spdk_jsonrpc_end_result(request, w);
 *
 * 호출 체인: 사용자 핸들러 → [이 함수] → begin_response (jsonrpc/id 작성) →
 * 핸들러가 result body 작성 → spdk_jsonrpc_end_result.
 */
struct spdk_json_write_ctx *
spdk_jsonrpc_begin_result(struct spdk_jsonrpc_request *request)
{
	struct spdk_json_write_ctx *w = begin_response(request);
	/* [한국어] {"jsonrpc":"2.0", "id":..., 까지 작성된 상태 */

	spdk_json_write_name(w, "result");      /* [한국어] "result": 키만 작성 — 값은 핸들러가 이어 씀 */
	return w;
}

/*
 * [한국어]
 * spdk_jsonrpc_end_result - 핸들러가 result 작성 완료 후 호출(공개 API)
 *
 * @request: 디스패치된 요청.
 * @w: begin_result가 반환한 writer (검증용).
 *
 * id가 NULL이거나 SPDK_JSON_VAL_NULL이면 notification으로 간주하여 응답을 skip.
 * 그렇지 않으면 정상적으로 객체를 닫고 송신 큐로 이동.
 *
 * 호출 체인: 사용자 핸들러 → [이 함수] → end_response/skip_response →
 *           jsonrpc_server_send_response.
 */
void
spdk_jsonrpc_end_result(struct spdk_jsonrpc_request *request, struct spdk_json_write_ctx *w)
{
	assert(w != NULL);                      /* [한국어] writer 인자 검증 */
	assert(w == request->response);         /* [한국어] 사용자 begin_result가 반환한 writer와 동일해야 함 */

	/* If there was no ID in request we skip response. */
	if (request->id && request->id->type != SPDK_JSON_VAL_NULL) {
		/* [한국어] 정상 응답 송신 경로 — id가 string/number 등 valid value. */
		end_response(request);
	} else {
		/* [한국어] notification 또는 id=null — 사양상 응답 송신 금지. send_buf 폐기. */
		skip_response(request);
	}
}

/*
 * [한국어]
 * spdk_jsonrpc_send_bool_response - 가장 흔한 응답 패턴 단축 빌더 (공개 API)
 *
 * @request: 디스패치된 요청.
 * @value: true/false.
 *
 * "result": true/false 형태 응답을 한 호출로 완성. 다수의 RPC가 단순 ack용으로 사용.
 */
void
spdk_jsonrpc_send_bool_response(struct spdk_jsonrpc_request *request, bool value)
{
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request); /* [한국어] {"jsonrpc"... "id"... "result": 까지 */
	assert(w != NULL);
	spdk_json_write_bool(w, value);         /* [한국어] true/false 값 작성 */
	spdk_jsonrpc_end_result(request, w);    /* [한국어] '}' 닫고 송신 큐로 */
}

/*
 * [한국어]
 * jsonrpc_reset_response - 누적된 부분 출력을 폐기하고 빈 buf 상태로 리셋
 *
 * @request: 진행 중 요청.
 *
 * 핸들러가 result를 작성하다가 중간에 에러를 발견해 error 응답으로 전환할 때 사용.
 * spdk_json_write_reset은 writer의 인덴트/객체 스택을 초기 상태로 되돌리고,
 * send_len=0은 누적된 byte 출력을 무효화한다(다음 begin_response의 assert 통과).
 */
static void
jsonrpc_reset_response(struct spdk_jsonrpc_request *request)
{
	spdk_json_write_reset(request->response);  /* [한국어] writer 내부 상태 초기화 */
	request->send_len = 0; /* to skip all previous data previously written by jsonrpc_server_write_cb */
	/* [한국어] send_buf의 capacity/포인터는 유지 — 위 byte만 무효화. */
}

/*
 * [한국어]
 * spdk_jsonrpc_send_error_response - 표준 에러 응답 송신 (공개 API)
 *
 * @request: 진행 중 요청.
 * @error_code: SPDK_JSONRPC_ERROR_* 또는 사용자 정의 코드(범위 -32768~-32000 예약).
 * @msg: 사용자에게 노출되는 평문 메시지.
 *
 * 사양 §5.1: error 응답은 {jsonrpc, id, "error":{"code":..., "message":...}} 구조.
 * 이전에 result를 일부 썼을 가능성이 있어 jsonrpc_reset_response로 buf를 비운 후 작성.
 *
 * 호출 체인: 사용자 핸들러 또는 jsonrpc_server_handle_error → [이 함수] → end_response.
 */
void
spdk_jsonrpc_send_error_response(struct spdk_jsonrpc_request *request,
				 int error_code, const char *msg)
{
	struct spdk_json_write_ctx *w;

	jsonrpc_reset_response(request);        /* [한국어] 이전에 부분 출력이 있었어도 폐기 */

	w = begin_response(request);            /* [한국어] {"jsonrpc","id" 까지 작성 */

	spdk_json_write_named_object_begin(w, "error");  /* [한국어] "error":{ */
	spdk_json_write_named_int32(w, "code", error_code);  /* [한국어] "code": -32xxx */
	spdk_json_write_named_string(w, "message", msg);     /* [한국어] "message":"..." */
	spdk_json_write_object_end(w);          /* [한국어] error 객체 닫기 */

	end_response(request);                  /* [한국어] 응답 전체 객체 닫고 송신 큐로 */
}

/*
 * [한국어]
 * spdk_jsonrpc_send_error_response_fmt - printf 스타일 에러 메시지 (공개 API)
 *
 * @request: 진행 중 요청.
 * @error_code: 에러 코드.
 * @fmt: printf 형식 문자열, 가변 인자.
 *
 * 디바이스 이름/숫자 등 동적 정보를 메시지에 포함하고 싶을 때 사용 — 내부적으로
 * spdk_json_write_named_string_fmt_v가 한 번에 형식화 + JSON 이스케이프 수행.
 */
void
spdk_jsonrpc_send_error_response_fmt(struct spdk_jsonrpc_request *request,
				     int error_code, const char *fmt, ...)
{
	struct spdk_json_write_ctx *w;
	va_list args;

	jsonrpc_reset_response(request);        /* [한국어] 부분 출력 폐기 */

	w = begin_response(request);            /* [한국어] prologue */

	spdk_json_write_named_object_begin(w, "error");
	spdk_json_write_named_int32(w, "code", error_code);
	va_start(args, fmt);                    /* [한국어] 가변 인자 시작 */
	spdk_json_write_named_string_fmt_v(w, "message", fmt, args);  /* [한국어] vprintf-스타일 + JSON escape */
	va_end(args);                           /* [한국어] 가변 인자 종료 */
	spdk_json_write_object_end(w);          /* [한국어] error 객체 닫기 */

	end_response(request);                  /* [한국어] 응답 마감 + 송신 큐 */
}

/* [한국어] SPDK_LOG_REGISTER_COMPONENT: 로그 컴포넌트 "rpc"를 등록.
 * SPDK_DEBUGLOG(rpc, ...)가 사용자가 --logflag rpc로 활성화한 경우만 출력되도록 한다.
 * 컴파일 타임에 정적 등록 — 런타임 cost 없음. */
SPDK_LOG_REGISTER_COMPONENT(rpc)
