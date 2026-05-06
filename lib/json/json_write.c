/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK Streaming JSON Writer (json_write.c)
 *
 * === 파일의 역할 ===
 * SPDK가 외부로 JSON을 "한 토큰씩 흘려보내며" 출력할 수 있게 하는 streaming writer 라이브러리.
 * 호출자는 spdk_json_write_begin(write_cb, cb_ctx, flags)로 컨텍스트(spdk_json_write_ctx)를
 * 만들고, spdk_json_write_object_begin/array_begin/named_*/string/uint*/double 등 헬퍼를
 * 순서대로 호출한다. 본 모듈이 자동으로 콤마/콜론/들여쓰기/escape를 삽입하여 RFC 8259 호환
 * 출력을 만든다. 내부적으로 4KB 임시 버퍼(buf[4096])를 두어 잦은 write_cb 호출을 줄이고,
 * 버퍼가 차거나 spdk_json_write_end 시점에만 사용자 콜백을 호출한다.
 *
 * 주요 디자인 포인트:
 *   1) "first_value"/"new_indent" 두 개의 플래그로 콤마/들여쓰기 자동 삽입(begin_value()).
 *   2) FORMATTED 플래그가 켜졌을 때만 줄바꿈/공백 출력 — 그 외에는 compact JSON.
 *   3) 문자열은 UTF-8 입력을 codepoint 단위로 검증·디코드한 뒤 RFC 8259 escape 규칙에 따라
 *      \", \\, 제어문자(\b\f\n\r\t), 0x20 미만/0x7F 이상 비-ASCII는 \uXXXX(또는 surrogate pair)로 출력.
 *   4) 정수 타입별로 별도 함수(write_uint8/16/32/64, write_int32/64) — snprintf %PRI* 사용.
 *   5) "named_*" 패밀리는 (name + ':') + value를 한 번에 — 객체 내부에서 가장 흔한 패턴.
 *   6) 실패 누적: 한 번 실패(failed=true)하면 이후 호출도 모두 실패로 처리 — 중간 에러 검사 부담 제거.
 *      호출자는 spdk_json_write_end 반환값으로 일괄 검사하면 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK JSON-RPC 응답 송신, config save(load_subsystems), bdev/nvme 통계 출력, NVMe-oF
 * subsystem 직렬화 등이 모두 본 모듈을 거쳐 출력 byte stream을 생성한다. 일반 흐름:
 *   RPC 핸들러 → spdk_jsonrpc_begin_result(=spdk_json_write_begin 래퍼) →
 *     spdk_json_write_named_string/uint32/object_begin... →
 *     buf 채워짐 → emit_buf_full → flush_buf → write_cb(소켓 송신) →
 *   spdk_jsonrpc_end_result → spdk_json_write_end → 최종 flush.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/json.h: 공개 writer API와 SPDK_JSON_WRITE_FLAG_FORMATTED 등.
 *   - spdk_internal/utf.h: utf8_valid/utf8_decode_unsafe_*, utf16_encode_surrogate_pair,
 *     from_le16, utf16le_valid — 문자열 escape 시 codepoint 추출.
 *   - libc: snprintf(%PRIu*/%PRId*/%.20e), memcpy, calloc/free, strlen, va_list 처리.
 *   - spdk_vsprintf_alloc(spdk/util.h): named_string_fmt 가족이 사용.
 *   - spdk_uuid_fmt_lower(spdk/uuid.h): write_uuid에서 사용.
 *   - spdk_unlikely(spdk/likely.h): 분기 예측 힌트.
 * 의존받음(피호출):
 *   - lib/jsonrpc/*: spdk_jsonrpc_begin_result/end_result/send_error_response가 본 writer를 감싼다.
 *   - lib/init/subsystem.c: framework save_config가 출력에 사용.
 *   - 모든 SPDK 서브시스템의 *_dump/*_get_info RPC 핸들러.
 * 데이터 흐름: C 자료구조 → spdk_json_write_* 호출 → 4KB buf → write_cb → 사용자 sink.
 * 공유 상태: 없음. 각 writer 컨텍스트는 호출자 스레드 전용 — 동시 사용 금지(외부 직렬화 책임).
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_json_write_ctx: writer 상태 — write_cb, cb_ctx, flags, indent, first_value,
 *     new_indent, failed, buf[4096]/buf_filled.
 *   - spdk_json_write_begin/end/reset: lifecycle.
 *   - emit/emit_buf_full/flush_buf: 4KB 버퍼 관리 + 사용자 콜백 호출.
 *   - emit_fmt/emit_indent: FORMATTED 플래그 켜졌을 때만 동작하는 white-space 출력.
 *   - begin_value: 모든 값 출력 함수가 진입 시 호출하는 helper — 자동 콤마/들여쓰기.
 *   - spdk_json_write_null/bool/uint*/int*/double: primitive 타입 출력.
 *   - spdk_json_write_uint128: 64+64 → 128bit 정수의 십진수 변환(GCC __int128 사용).
 *   - write_codepoint: 단일 codepoint를 RFC 8259 escape 규칙으로 출력(2자/6자/12자 케이스).
 *   - write_string_or_name(_utf16le): UTF-8/UTF-16LE 입력을 codepoint 단위로 처리.
 *   - spdk_json_write_string/_raw/_utf16le_raw/_string_fmt: 문자열 출력 변종.
 *   - spdk_json_write_bytearray: byte를 hex 인코드해 string으로(예: WWN/MAC 등 고유 식별자).
 *   - spdk_json_write_uuid: UUID 표준 표기 string 출력.
 *   - spdk_json_write_array_begin/end/object_begin/end/name/name_raw: 컨테이너/키 출력.
 *   - spdk_json_write_val: 외부 spdk_json_val(보통 파서 출력)을 그대로 재출력 — 라우팅/forwarding 용.
 *   - spdk_json_write_named_*: 객체 내부에서 흔한 (name, value) 쌍 단축 헬퍼.
 */

#include "spdk/json.h"            /* [한국어] 공개 writer API 시그니처 */

#include "spdk_internal/utf.h"    /* [한국어] UTF-8/16 codepoint 추출 헬퍼(escape 인코딩에 필요) */

/*
 * [한국어] streaming writer의 모든 상태를 담은 컨텍스트.
 * spdk_json_write_begin이 calloc으로 할당 → spdk_json_write_end에서 flush 후 free.
 * 호출자가 직접 멤버를 만지면 안 되며(불투명 핸들), 모든 접근은 본 파일의 함수만 수행.
 */
struct spdk_json_write_ctx {
	spdk_json_write_cb write_cb;
	/* [한국어] 사용자 sink 콜백 — flush 시 (cb_ctx, data, size)로 호출.
	 * 설정자: spdk_json_write_begin.
	 * 읽는 자: flush_buf.
	 * 반환값: 0=성공, 그 외=실패(컨텍스트가 failed로 마킹됨).
	 * 동기화: 호출자 스레드에서 직렬 실행 — 본 컨텍스트가 멀티스레드 공유 불가하기 때문. */

	void *cb_ctx;
	/* [한국어] write_cb의 첫 인자로 전달되는 사용자 컨텍스트(소켓 fd, 메모리 버퍼 핸들 등).
	 * 본 모듈은 내용을 해석하지 않고 그대로 전달. */

	uint32_t flags;
	/* [한국어] SPDK_JSON_WRITE_FLAG_FORMATTED(들여쓰기/줄바꿈 활성) 같은 옵션 비트마스크.
	 * begin/end 사이에 변경 금지(emit_fmt/emit_indent가 일관된 동작을 가정). */

	uint32_t indent;
	/* [한국어] 현재 들여쓰기 깊이(2 space 단위). object/array_begin에서 ++, end에서 --.
	 * FORMATTED 플래그가 꺼져 있으면 사용되지 않음. */

	bool new_indent;
	/* [한국어] "방금 컨테이너를 열었다"는 표지. true면 다음 값 직전에 줄바꿈+들여쓰기 삽입.
	 * 컨테이너를 열자마자 빈 채로 닫으면(false 유지) 컴팩트하게 "{}"/"[]"로 출력. */

	bool first_value;
	/* [한국어] 현재 컨테이너에서 아직 값이 없으면 true → 콤마 prefix 생략.
	 * 한 번이라도 값을 출력하면 false → 다음 값 앞에 콤마 자동 삽입.
	 * name 출력 시에도 true로 리셋되어 ":"와 value 사이에 콤마가 안 들어감. */

	bool failed;
	/* [한국어] 누적 실패 플래그. 한 번 true가 되면 이후 모든 호출이 즉시 실패.
	 * 호출자가 매 호출마다 에러를 체크하지 않아도 안전 — 종료 시 한 번만 검사. */

	size_t buf_filled;
	/* [한국어] buf[]에 채워진 바이트 수. emit/flush가 갱신.
	 * sizeof(buf)에 도달하면 emit_buf_full → flush_buf 트리거. */

	uint8_t buf[4096];
	/* [한국어] 출력 임시 버퍼. 4KB는 일반 RPC 응답 크기와 소켓 write의 적절한 최소 단위 균형.
	 * flush_buf가 호출되면 [0..buf_filled)를 write_cb로 한 번에 전달 후 buf_filled=0. */
};

/* [한국어] emit_buf_full 전방 선언 — emit() 인라인 함수에서 호출하기 위해 필요. */
static int emit_buf_full(struct spdk_json_write_ctx *w, const void *data, size_t size);

/*
 * [한국어]
 * fail - writer를 실패 상태로 마킹하고 -1 반환.
 *
 * @w: writer 컨텍스트.
 * @return: 항상 -1(호출자 편의 — `return fail(w);` 패턴).
 *
 * 정책: 한 번 실패하면 이후 호출도 자동 실패 — 호출자가 매번 에러 체크 안 해도 됨.
 *   최종 spdk_json_write_end가 누적 실패 여부를 보고한다.
 */
static int
fail(struct spdk_json_write_ctx *w)
{
	w->failed = true;
	return -1;
}

/*
 * [한국어]
 * flush_buf - 내부 4KB 버퍼를 사용자 write_cb로 비움.
 *
 * @w: writer 컨텍스트.
 * @return: 0(성공) / -1(write_cb 실패 — failed 마킹됨).
 *
 * 호출 시점: 버퍼가 가득 차서 emit_buf_full에서 호출 / spdk_json_write_end의 마지막 flush.
 */
static int
flush_buf(struct spdk_json_write_ctx *w)
{
	int rc;

	rc = w->write_cb(w->cb_ctx, w->buf, w->buf_filled); /* [한국어] 사용자 콜백으로 한 번에 전송 */
	if (rc != 0) {
		return fail(w);                                   /* [한국어] 콜백 실패 → 컨텍스트 실패 마킹 */
	}

	w->buf_filled = 0;                                    /* [한국어] 버퍼 재사용 */

	return 0;
}

/*
 * [한국어]
 * spdk_json_write_begin - writer 컨텍스트 생성. 이후 모든 출력 호출의 입구.
 *
 * @write_cb: byte stream sink — 4KB 단위 또는 end 시점에 호출됨.
 * @cb_ctx: write_cb 호출 시 그대로 넘길 사용자 데이터.
 * @flags: SPDK_JSON_WRITE_FLAG_FORMATTED(들여쓰기/줄바꿈) 등.
 * @return: 새 컨텍스트 또는 NULL(OOM).
 *
 * 호출 체인: spdk_jsonrpc_begin_result/사용자 코드 → spdk_json_write_begin.
 */
struct spdk_json_write_ctx *
spdk_json_write_begin(spdk_json_write_cb write_cb, void *cb_ctx, uint32_t flags)
{
	struct spdk_json_write_ctx *w;

	w = calloc(1, sizeof(*w));                  /* [한국어] 0으로 초기화 — buf_filled/failed 등 자동 0 */
	if (w == NULL) {
		return w;                                /* [한국어] OOM */
	}

	w->write_cb = write_cb;
	w->cb_ctx = cb_ctx;
	w->flags = flags;
	w->indent = 0;                               /* [한국어] 최상위는 들여쓰기 0 */
	w->new_indent = false;                       /* [한국어] 아직 컨테이너 안 열림 */
	w->first_value = true;                       /* [한국어] 첫 값 직전 — 콤마 prefix 생략 */
	w->failed = false;
	w->buf_filled = 0;

	return w;
}

/*
 * [한국어]
 * spdk_json_write_end - 컨텍스트 종료, 잔여 버퍼 flush, 컨텍스트 free.
 *
 * @w: writer(NULL 안전 — 0 반환).
 * @return: 0(성공) / -1(누적 실패 또는 마지막 flush 실패).
 *
 * 호출자는 이 시점에서만 누적 결과를 검사하면 충분하다.
 */
int
spdk_json_write_end(struct spdk_json_write_ctx *w)
{
	bool failed;
	int rc;

	if (w == NULL) {
		return 0;                                /* [한국어] NULL 안전성 — begin 실패 후 호출 가능 */
	}

	failed = w->failed;                          /* [한국어] 이전까지 누적 실패 */

	rc = flush_buf(w);                            /* [한국어] 마지막 잔여물 송출 */
	if (rc != 0) {
		failed = true;
	}

	free(w);                                      /* [한국어] 컨텍스트 해제 — w 사용은 여기까지 */

	return failed ? -1 : 0;
}

/*
 * [한국어]
 * spdk_json_write_reset - 컨텍스트를 초기 상태로 되돌림(재사용용).
 *
 * @w: writer.
 *
 * 사용 시나리오: 같은 컨텍스트로 여러 메시지를 연달아 보낼 때 매번 begin/end 대신 reset 사용.
 *   주의: failed=false로 비우므로 이전 실패의 잔재는 사라진다 — 호출자가 reset 전에 검사 책임.
 */
void
spdk_json_write_reset(struct spdk_json_write_ctx *w)
{
	if (w == NULL) {
		return;
	}

	w->buf_filled = 0;
	w->failed = false;
	w->first_value = true;
	w->new_indent = false;
	w->indent = 0;
}

/*
 * [한국어]
 * emit - 임의 바이트열을 내부 버퍼에 추가. 가득 차면 emit_buf_full로 위임.
 *
 * 핫패스: 인라인. 대부분의 호출은 단순 memcpy 한 번으로 끝나도록 설계 — 분기 예측 힌트 사용.
 */
static inline int
emit(struct spdk_json_write_ctx *w, const void *data, size_t size)
{
	size_t buf_remain = sizeof(w->buf) - w->buf_filled;        /* [한국어] 현재 버퍼 잔여 공간 */

	if (spdk_unlikely(size > buf_remain)) {
		/* Not enough space in buffer for the new data. */
		return emit_buf_full(w, data, size);                    /* [한국어] 잔여 < 입력 — 분할 + flush 경로 */
	}

	/* Copy the new data into buf. */
	memcpy(w->buf + w->buf_filled, data, size);                 /* [한국어] 빠른 경로 — 단순 복사 */
	w->buf_filled += size;
	return 0;
}

/*
 * [한국어]
 * emit_buf_full - 버퍼가 부족할 때의 콜드 패스.
 *
 * 동작: 잔여 공간을 메우고 → flush → 남은 데이터를 emit으로 재귀 호출(다음 호출엔 자리가 충분).
 * 큰 입력(예: > 4KB)에 대해서는 자연스럽게 4KB 청크로 쪼개져 write_cb 여러 번 호출된다.
 */
static int
emit_buf_full(struct spdk_json_write_ctx *w, const void *data, size_t size)
{
	size_t buf_remain = sizeof(w->buf) - w->buf_filled;
	int rc;

	assert(size > buf_remain);                                  /* [한국어] 호출자 보장 — 빠른 경로가 아님 */

	/* Copy as much of the new data as possible into the buffer and flush it. */
	memcpy(w->buf + w->buf_filled, data, buf_remain);           /* [한국어] 가능한 만큼 채움 */
	w->buf_filled += buf_remain;

	rc = flush_buf(w);                                           /* [한국어] 가득 찬 버퍼 송출 */
	if (rc != 0) {
		return fail(w);
	}

	/* Recurse to emit the rest of the data. */
	return emit(w, data + buf_remain, size - buf_remain);       /* [한국어] 남은 데이터로 재진입 — 한 번만 더 빠른 경로로 들어가는 게 일반 */
}

/*
 * [한국어]
 * emit_fmt - SPDK_JSON_WRITE_FLAG_FORMATTED가 켜졌을 때만 emit. (개행/공백 출력 전용)
 */
static int
emit_fmt(struct spdk_json_write_ctx *w, const void *data, size_t size)
{
	if (w->flags & SPDK_JSON_WRITE_FLAG_FORMATTED) {
		return emit(w, data, size);                              /* [한국어] pretty mode에서만 토큰 사이 white-space 삽입 */
	}
	return 0;                                                    /* [한국어] compact mode에서는 noop */
}

/*
 * [한국어]
 * emit_indent - 현재 indent 깊이만큼 "  "(2 spaces)를 출력. FORMATTED 시에만.
 */
static int
emit_indent(struct spdk_json_write_ctx *w)
{
	uint32_t i;

	if (w->flags & SPDK_JSON_WRITE_FLAG_FORMATTED) {
		for (i = 0; i < w->indent; i++) {
			if (emit(w, "  ", 2)) { return fail(w); }            /* [한국어] 한 단계당 2 스페이스 */
		}
	}
	return 0;
}

/*
 * [한국어]
 * begin_value - "값을 출력하기 직전에 호출하는" 공용 helper. 콤마/들여쓰기 자동 처리.
 *
 * 동작:
 *   - new_indent(컨테이너 직후): 줄바꿈+들여쓰기 후 끔.
 *   - first_value=false(이전 형제 있음): 콤마 + 줄바꿈 + 들여쓰기.
 *   - first_value=true: 콤마 생략(컨테이너 첫 값 또는 name 다음 value).
 * 종료 시 first_value=false로 마킹 → 같은 컨테이너 내 후속 값은 콤마 prefix 받음.
 *
 * 호출자: 모든 spdk_json_write_<value>* 함수의 시작 부분.
 */
static int
begin_value(struct spdk_json_write_ctx *w)
{
	/* TODO: check for value state */
	if (w->new_indent) {
		if (emit_fmt(w, "\n", 1)) { return fail(w); }            /* [한국어] 컨테이너 막 열린 직후 줄바꿈 */
		if (emit_indent(w)) { return fail(w); }
	}
	if (!w->first_value) {
		if (emit(w, ",", 1)) { return fail(w); }                  /* [한국어] 형제 사이 콤마 */
		if (emit_fmt(w, "\n", 1)) { return fail(w); }
		if (emit_indent(w)) { return fail(w); }
	}
	w->first_value = false;                                      /* [한국어] 이번 값 출력 후엔 형제가 따라올 수 있음 */
	w->new_indent = false;                                       /* [한국어] 컨테이너 첫 값 처리 완료 */
	return 0;
}

/*
 * [한국어]
 * spdk_json_write_val_raw - 이미 형식이 잘 갖춰진 raw JSON 조각을 그대로 emit.
 *
 * 사용처: 다른 시스템에서 받은 JSON snippet을 forwarding하거나, NUMBER 토큰을 소수까지 그대로 보존.
 *   호출자가 "data가 유효한 JSON 값"임을 보장해야 한다(검증 안 함).
 */
int
spdk_json_write_val_raw(struct spdk_json_write_ctx *w, const void *data, size_t len)
{
	if (begin_value(w)) { return fail(w); }   /* [한국어] 콤마/들여쓰기 처리 */
	return emit(w, data, len);                 /* [한국어] 본문 그대로 송출 */
}

/* [한국어] spdk_json_write_null — 리터럴 "null" 출력. */
int
spdk_json_write_null(struct spdk_json_write_ctx *w)
{
	if (begin_value(w)) { return fail(w); }
	return emit(w, "null", 4);
}

/* [한국어] spdk_json_write_bool — true/false 리터럴. */
int
spdk_json_write_bool(struct spdk_json_write_ctx *w, bool val)
{
	if (begin_value(w)) { return fail(w); }
	if (val) {
		return emit(w, "true", 4);
	} else {
		return emit(w, "false", 5);
	}
}

/*
 * [한국어]
 * spdk_json_write_uint8 - uint8 정수를 십진수로 출력.
 *
 * 구현: 32바이트 스택 버퍼에 snprintf로 변환 후 emit.
 *   sizeof(buf)=32는 가장 긴 64비트 정수 표기(20자) + sign + NUL을 충분히 커버.
 *   snprintf 결과가 truncate되었으면 fail — 정상 입력에서는 발생하지 않지만 방어적.
 *   PRIu8 등 inttypes.h 매크로로 64bit/32bit 호스트에서 모두 정확한 포맷.
 */
int
spdk_json_write_uint8(struct spdk_json_write_ctx *w, uint8_t val)
{
	char buf[32];
	int count;

	if (begin_value(w)) { return fail(w); }
	count = snprintf(buf, sizeof(buf), "%" PRIu8, val);                            /* [한국어] 8비트 unsigned 십진수 */
	if (count <= 0 || (size_t)count >= sizeof(buf)) { return fail(w); }            /* [한국어] truncate/error 방어 */
	return emit(w, buf, count);
}

/* [한국어] spdk_json_write_uint16 — uint8과 동일 패턴, 16비트. */
int
spdk_json_write_uint16(struct spdk_json_write_ctx *w, uint16_t val)
{
	char buf[32];
	int count;

	if (begin_value(w)) { return fail(w); }
	count = snprintf(buf, sizeof(buf), "%" PRIu16, val);
	if (count <= 0 || (size_t)count >= sizeof(buf)) { return fail(w); }
	return emit(w, buf, count);
}

/* [한국어] spdk_json_write_int32 — 부호 있는 32비트 정수 출력. */
int
spdk_json_write_int32(struct spdk_json_write_ctx *w, int32_t val)
{
	char buf[32];
	int count;

	if (begin_value(w)) { return fail(w); }
	count = snprintf(buf, sizeof(buf), "%" PRId32, val);                           /* [한국어] PRId32: 부호 있는 32비트 */
	if (count <= 0 || (size_t)count >= sizeof(buf)) { return fail(w); }
	return emit(w, buf, count);
}

/* [한국어] spdk_json_write_uint32 — 부호 없는 32비트. */
int
spdk_json_write_uint32(struct spdk_json_write_ctx *w, uint32_t val)
{
	char buf[32];
	int count;

	if (begin_value(w)) { return fail(w); }
	count = snprintf(buf, sizeof(buf), "%" PRIu32, val);
	if (count <= 0 || (size_t)count >= sizeof(buf)) { return fail(w); }
	return emit(w, buf, count);
}

/* [한국어] spdk_json_write_int64 — 부호 있는 64비트. */
int
spdk_json_write_int64(struct spdk_json_write_ctx *w, int64_t val)
{
	char buf[32];
	int count;

	if (begin_value(w)) { return fail(w); }
	count = snprintf(buf, sizeof(buf), "%" PRId64, val);
	if (count <= 0 || (size_t)count >= sizeof(buf)) { return fail(w); }
	return emit(w, buf, count);
}

/* [한국어] spdk_json_write_uint64 — 부호 없는 64비트. */
int
spdk_json_write_uint64(struct spdk_json_write_ctx *w, uint64_t val)
{
	char buf[32];
	int count;

	if (begin_value(w)) { return fail(w); }
	count = snprintf(buf, sizeof(buf), "%" PRIu64, val);
	if (count <= 0 || (size_t)count >= sizeof(buf)) { return fail(w); }
	return emit(w, buf, count);
}

/*
 * [한국어]
 * spdk_json_write_uint128 - 128비트 정수를 십진수로 출력. low_val + (high_val << 64).
 *
 * 동기: NVMe 스펙에는 128비트 카운터(예: Total NVM Capacity, Power-On Hours 일부 SMART 필드)가
 *       존재. libc의 printf는 %llu(=64비트)까지만 보장하므로 직접 십진 변환을 구현.
 * 알고리즘: GCC 확장 unsigned __int128로 high<<64 + low를 만들고, 10^10 단위로 나누어
 *   10자리씩 buf에 prepend(앞쪽으로 쌓음). 마지막 세그먼트는 0-패딩 없이.
 *   buf[128]은 최대 39자리(2^128 = ~3.4e38) + NUL을 충분히 커버.
 */
int
spdk_json_write_uint128(struct spdk_json_write_ctx *w, uint64_t low_val, uint64_t high_val)
{
	char buf[128] = {'\0'};                              /* [한국어] 누적 결과 — '\0'로 초기화해 prepend 안전 */
	uint64_t low = low_val, high = high_val;             /* [한국어] 로컬 복사(컴파일러가 인자 변경 자유) */
	int count = 0;

	if (begin_value(w)) { return fail(w); }

	if (high != 0) {
		char temp_buf[128] = {'\0'};
		uint64_t seg;
		unsigned __int128 total = (unsigned __int128)low +
					  ((unsigned __int128)high << 64); /* [한국어] 128비트 합성 — GCC 확장 */

		while (total) {
			seg = total % 10000000000;                   /* [한국어] 10자리 segment 추출 */
			total = total / 10000000000;
			if (total) {
				count = snprintf(temp_buf, 128, "%010" PRIu64 "%s", seg, buf); /* [한국어] 중간 segment는 10자리 0-패딩 */
			} else {
				count = snprintf(temp_buf, 128, "%" PRIu64 "%s", seg, buf);    /* [한국어] 가장 높은 segment는 leading zero 없음 */
			}

			if (count <= 0 || (size_t)count >= sizeof(temp_buf)) {
				return fail(w);
			}

			snprintf(buf, 128, "%s", temp_buf);          /* [한국어] temp_buf의 누적 결과를 buf로 다시 복사(다음 prepend 준비) */
		}
	} else {
		count = snprintf(buf, sizeof(buf), "%" PRIu64, low); /* [한국어] high=0이면 단순 64비트 출력 */

		if (count <= 0 || (size_t)count >= sizeof(buf)) { return fail(w); }
	}

	return emit(w, buf, count);                          /* [한국어] 최종 십진수 문자열 송출 */
}

/* [한국어] spdk_json_write_named_uint128 — 객체 내부 (name, uint128) 한 번에. */
int
spdk_json_write_named_uint128(struct spdk_json_write_ctx *w, const char *name,
			      uint64_t low_val, uint64_t high_val)
{
	int rc = spdk_json_write_name(w, name);              /* [한국어] "name": prefix */

	return rc ? rc : spdk_json_write_uint128(w, low_val, high_val);
}

/*
 * [한국어]
 * spdk_json_write_double - double을 과학표기(%.20e)로 출력.
 *
 * 정밀도: 20자리 가수 — IEEE 754 binary64의 약 15.95 decimal digit를 모두 보존하기에 충분 + 마진.
 *   호출자는 NaN/Inf가 들어오면 JSON 표준에서 표현 불가 — snprintf가 "nan"/"inf" 문자열을 만들고
 *   호출자가 보낸 결과가 표준 JSON으로 파싱되지 않을 수 있음. SPDK 코드 경로에서는 NaN/Inf를
 *   상위 레이어에서 걸러야 한다.
 */
int
spdk_json_write_double(struct spdk_json_write_ctx *w, double val)
{
	char buf[32];
	int count;

	if (begin_value(w)) { return fail(w); }
	count = snprintf(buf, sizeof(buf), "%.20e", val);   /* [한국어] 과학 표기, 가수 20자리 */
	if (count <= 0 || (size_t)count >= sizeof(buf)) { return fail(w); }
	return emit(w, buf, count);
}

/*
 * [한국어]
 * write_hex_2 - 1바이트(uint8)를 대문자 hex 2글자로 변환해 dest[0..1]에 기록.
 * 사용처: \uXXXX의 각 자리, byte array hex 인코딩.
 */
static void
write_hex_2(void *dest, uint8_t val)
{
	char *p = dest;
	char hex[] = "0123456789ABCDEF";

	p[0] = hex[val >> 4];                /* [한국어] 상위 nibble */
	p[1] = hex[val & 0xf];               /* [한국어] 하위 nibble */
}

/*
 * [한국어]
 * write_hex_4 - 2바이트(uint16)를 hex 4글자로 변환(big-endian 표기). \uXXXX 작성에 사용.
 */
static void
write_hex_4(void *dest, uint16_t val)
{
	write_hex_2(dest, (uint8_t)(val >> 8));                  /* [한국어] 상위 바이트 → 앞 두 글자 */
	write_hex_2((char *)dest + 2, (uint8_t)(val & 0xff));    /* [한국어] 하위 바이트 → 뒤 두 글자 */
}

/*
 * [한국어]
 * write_codepoint - 단일 Unicode codepoint를 RFC 8259 escape 규칙에 맞게 출력.
 *
 * 케이스:
 *   1) 짧은 escape 매핑(\b/\f/\n/\r/\t/\"/\\) — 2바이트 출력.
 *   2) 0x20~0x7E ASCII printable — raw 1바이트.
 *   3) BMP(<0x10000) 그 외 — \uXXXX 6바이트.
 *   4) BMP 외(emoji 등 0x10000~0x10FFFF) — UTF-16 surrogate pair \uHHHH\uLLLL 12바이트.
 * 0x7F(DEL)는 spec상 control이 아니지만 SPDK는 보수적으로 \uXXXX로 인코드(spec 비준수가 아님 — 옵션).
 *
 * 호출 체인: write_string_or_name(_utf16le) → write_codepoint → emit.
 */
static inline int
write_codepoint(struct spdk_json_write_ctx *w, uint32_t codepoint)
{
	static const uint8_t escapes[] = {
		['\b'] = 'b',                             /* [한국어] backspace → \b */
		['\f'] = 'f',                             /* [한국어] form feed → \f */
		['\n'] = 'n',
		['\r'] = 'r',
		['\t'] = 't',
		['"'] = '"',                              /* [한국어] 따옴표 → \" */
		['\\'] = '\\',                            /* [한국어] 백슬래시 → \\ */
		/*
		 * Forward slash (/) is intentionally not converted to an escape
		 *  (it is valid unescaped).
		 */
		/* [한국어] '/'는 escape 가능하지만 의무는 아니다 — 가독성을 위해 raw 출력. */
	};
	uint16_t high, low;
	char out[13];                                  /* [한국어] 최대 12자(\uHHHH\uLLLL) + 마진 */
	size_t out_len;

	if (codepoint < sizeof(escapes) && escapes[codepoint]) {
		out[0] = '\\';
		out[1] = escapes[codepoint];
		out_len = 2;                               /* [한국어] 짧은 escape */
	} else if (codepoint >= 0x20 && codepoint < 0x7F) {
		/*
		 * Encode plain ASCII directly (except 0x7F, since it is really
		 *  a control character, despite the JSON spec not considering it one).
		 */
		out[0] = (uint8_t)codepoint;
		out_len = 1;                               /* [한국어] printable ASCII는 raw 1바이트 */
	} else if (codepoint < 0x10000) {
		out[0] = '\\';
		out[1] = 'u';
		write_hex_4(&out[2], (uint16_t)codepoint); /* [한국어] BMP는 단일 \uXXXX */
		out_len = 6;
	} else {
		utf16_encode_surrogate_pair(codepoint, &high, &low); /* [한국어] BMP 외 — surrogate pair로 분해 */
		out[0] = '\\';
		out[1] = 'u';
		write_hex_4(&out[2], high);
		out[6] = '\\';
		out[7] = 'u';
		write_hex_4(&out[8], low);
		out_len = 12;
	}

	return emit(w, out, out_len);
}

/*
 * [한국어]
 * write_string_or_name - UTF-8 입력을 codepoint별로 검증·escape하여 큰따옴표로 감싸 출력.
 *
 * @val/@len: UTF-8 byte 배열과 길이.
 * 동작: 1) 시작 따옴표 → 2) utf8_valid로 다음 codepoint의 byte 길이(1~4) 측정 →
 *       3) 길이별 utf8_decode_unsafe_N으로 codepoint 추출 → 4) write_codepoint로 escape 출력.
 *       유효하지 않은 UTF-8(overlong/illegal byte 등)이면 fail.
 *
 * 호출 체인: spdk_json_write_string_raw, spdk_json_write_name_raw → write_string_or_name.
 */
static int
write_string_or_name(struct spdk_json_write_ctx *w, const char *val, size_t len)
{
	const uint8_t *p = val;
	const uint8_t *end = val + len;

	if (emit(w, "\"", 1)) { return fail(w); }   /* [한국어] 시작 따옴표 */

	while (p != end) {
		int codepoint_len;
		uint32_t codepoint;

		codepoint_len = utf8_valid(p, end);     /* [한국어] 1~4(유효) / 0(truncated) / -1(invalid) */
		switch (codepoint_len) {
		case 1:
			codepoint = utf8_decode_unsafe_1(p); /* [한국어] ASCII 단일 바이트 */
			break;
		case 2:
			codepoint = utf8_decode_unsafe_2(p); /* [한국어] 2바이트 시퀀스 */
			break;
		case 3:
			codepoint = utf8_decode_unsafe_3(p);
			break;
		case 4:
			codepoint = utf8_decode_unsafe_4(p);
			break;
		default:
			return fail(w);                      /* [한국어] 0/-1: 무효한 UTF-8 — 호출자 책임으로 거부 */
		}

		if (write_codepoint(w, codepoint)) { return fail(w); }
		p += codepoint_len;                      /* [한국어] 다음 codepoint로 진행 */
	}

	return emit(w, "\"", 1);                     /* [한국어] 닫는 따옴표 */
}

/*
 * [한국어]
 * write_string_or_name_utf16le - UTF-16LE 입력을 codepoint별로 escape하여 출력.
 *
 * 사용처: NVMe 같은 일부 디바이스의 식별 문자열은 UTF-16LE — 변환 없이 직접 입력 가능.
 * 동작: utf16le_valid로 1(BMP) / 2(surrogate pair) word 길이 측정 후 codepoint 추출.
 */
static int
write_string_or_name_utf16le(struct spdk_json_write_ctx *w, const uint16_t *val, size_t len)
{
	const uint16_t *p = val;
	const uint16_t *end = val + len;

	if (emit(w, "\"", 1)) { return fail(w); }

	while (p != end) {
		int codepoint_len;
		uint32_t codepoint;

		codepoint_len = utf16le_valid(p, end);
		switch (codepoint_len) {
		case 1:
			codepoint = from_le16(&p[0]);                                            /* [한국어] BMP 단일 word(LE 변환) */
			break;
		case 2:
			codepoint = utf16_decode_surrogate_pair(from_le16(&p[0]), from_le16(&p[1])); /* [한국어] surrogate pair → 0x10000~0x10FFFF */
			break;
		default:
			return fail(w);
		}

		if (write_codepoint(w, codepoint)) { return fail(w); }
		p += codepoint_len;
	}

	return emit(w, "\"", 1);
}

/*
 * [한국어]
 * spdk_json_write_string_raw - 길이 명시 UTF-8 문자열 출력.
 * 사용처: NUL 종결자가 없는 buffer/slice. 임베디드 NUL을 포함한 데이터는  으로 escape.
 */
int
spdk_json_write_string_raw(struct spdk_json_write_ctx *w, const char *val, size_t len)
{
	if (begin_value(w)) { return fail(w); }
	return write_string_or_name(w, val, len);
}

/*
 * [한국어]
 * spdk_json_write_string - NUL-종결 C string 출력. strlen으로 길이 자동 측정 후 _raw에 위임.
 */
int
spdk_json_write_string(struct spdk_json_write_ctx *w, const char *val)
{
	return spdk_json_write_string_raw(w, val, strlen(val));
}

/*
 * [한국어]
 * spdk_json_write_string_utf16le_raw - UTF-16LE 입력(길이 명시)을 string으로 출력.
 */
int
spdk_json_write_string_utf16le_raw(struct spdk_json_write_ctx *w, const uint16_t *val, size_t len)
{
	if (begin_value(w)) { return fail(w); }
	return write_string_or_name_utf16le(w, val, len);
}

/*
 * [한국어]
 * spdk_json_write_string_utf16le - 0-종결 UTF-16LE 입력 — 길이를 직접 셈.
 */
int
spdk_json_write_string_utf16le(struct spdk_json_write_ctx *w, const uint16_t *val)
{
	const uint16_t *p;
	size_t len;

	for (len = 0, p = val; *p; p++) {        /* [한국어] uint16_t 단위 strlen 직접 구현 — strlen은 byte 단위라 사용 불가 */
		len++;
	}

	return spdk_json_write_string_utf16le_raw(w, val, len);
}

/*
 * [한국어]
 * spdk_json_write_string_fmt - printf 스타일 문자열 출력.
 *
 * 동기: 디버그/에러 메시지 등에서 즉시 포맷팅이 필요한 경우.
 * 구현: va_list로 분리해서 _v 버전에 위임.
 */
int
spdk_json_write_string_fmt(struct spdk_json_write_ctx *w, const char *fmt, ...)
{
	va_list args;
	int rc;

	va_start(args, fmt);
	rc = spdk_json_write_string_fmt_v(w, fmt, args);
	va_end(args);

	return rc;
}

/*
 * [한국어]
 * spdk_json_write_string_fmt_v - 위 함수의 va_list 버전. spdk_vsprintf_alloc으로 임시 buffer 할당 → 정리.
 * spdk_vsprintf_alloc은 길이를 미리 모르는 포맷 결과를 동적 할당으로 안전하게 생성.
 */
int
spdk_json_write_string_fmt_v(struct spdk_json_write_ctx *w, const char *fmt, va_list args)
{
	char *s;
	int rc;

	s = spdk_vsprintf_alloc(fmt, args);
	if (s == NULL) {
		return -1;                            /* [한국어] OOM */
	}

	rc = spdk_json_write_string(w, s);
	free(s);
	return rc;
}

/*
 * [한국어]
 * spdk_json_write_bytearray - byte 배열을 hex 문자열로 인코드해 string으로 출력.
 *
 * 사용처: WWN/MAC/NVMe NQN의 raw byte 등 binary 식별자.
 * 형식: 각 바이트를 대문자 2자리 hex로 — len byte → 2*len 자 hex 문자열.
 * 메모리: malloc 한 번 — 큰 입력이면 임시 메모리 부담이 있지만, 일반 식별자 길이는 16~256 byte 수준.
 */
int
spdk_json_write_bytearray(struct spdk_json_write_ctx *w, const void *val, size_t len)
{
	const uint8_t *v = val;
	size_t i;
	char *s;
	int rc;

	s = malloc(2 * len + 1);                  /* [한국어] hex 2자 × len + NUL */
	if (s == NULL) {
		return -1;
	}

	for (i = 0; i < len; ++i) {
		write_hex_2(&s[2 * i], *v++);          /* [한국어] 한 byte → hex 2자리 */
	}
	s[2 * len] = '\0';

	rc = spdk_json_write_string(w, s);
	free(s);
	return rc;
}

/*
 * [한국어]
 * spdk_json_write_uuid - struct spdk_uuid를 표준 UUID 텍스트로 변환해 string 출력.
 * 형식: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (소문자).
 */
int
spdk_json_write_uuid(struct spdk_json_write_ctx *w, const struct spdk_uuid *uuid)
{
	char str[SPDK_UUID_STRING_LEN];           /* [한국어] 36자 + NUL = 37 — 매크로로 정의 */

	spdk_uuid_fmt_lower(str, sizeof(str), uuid); /* [한국어] util/uuid.c — RFC 4122 표준 표기 변환 */

	return spdk_json_write_string(w, str);
}

/*
 * [한국어]
 * spdk_json_write_array_begin - "[" 출력 + 새 컨테이너 상태로 진입.
 *
 * 상태 변경:
 *   - first_value=true: 컨테이너 안 첫 값은 콤마 prefix 없음.
 *   - new_indent=true: 첫 값 직전 줄바꿈+들여쓰기 트리거.
 *   - indent++: 들여쓰기 한 단계 증가.
 */
int
spdk_json_write_array_begin(struct spdk_json_write_ctx *w)
{
	if (begin_value(w)) { return fail(w); }   /* [한국어] 배열도 하나의 "값" — 콤마/들여쓰기 처리 */
	w->first_value = true;
	w->new_indent = true;
	w->indent++;
	if (emit(w, "[", 1)) { return fail(w); }
	return 0;
}

/*
 * [한국어]
 * spdk_json_write_array_end - "]" 출력. indent--, 컨텐츠가 있었다면 줄바꿈 후 indent.
 *
 * new_indent가 true(빈 배열)이면 줄바꿈 생략 → "[]" 컴팩트 출력.
 */
int
spdk_json_write_array_end(struct spdk_json_write_ctx *w)
{
	w->first_value = false;                   /* [한국어] 닫기는 "값을 출력한 효과" — 다음 형제 앞에 콤마가 와야 함 */
	if (w->indent == 0) { return fail(w); }   /* [한국어] 매칭되는 _begin 없는 _end */
	w->indent--;
	if (!w->new_indent) {
		if (emit_fmt(w, "\n", 1)) { return fail(w); } /* [한국어] 빈 배열이 아니면 마지막 원소 다음 줄바꿈 + 닫기 들여쓰기 */
		if (emit_indent(w)) { return fail(w); }
	}
	w->new_indent = false;
	return emit(w, "]", 1);
}

/*
 * [한국어]
 * spdk_json_write_object_begin - "{" 출력 + 객체 컨텍스트 진입. array_begin과 동일 구조.
 */
int
spdk_json_write_object_begin(struct spdk_json_write_ctx *w)
{
	if (begin_value(w)) { return fail(w); }
	w->first_value = true;
	w->new_indent = true;
	w->indent++;
	if (emit(w, "{", 1)) { return fail(w); }
	return 0;
}

/*
 * [한국어]
 * spdk_json_write_object_end - "}" 출력 + 객체 닫기. array_end와 동일 구조.
 */
int
spdk_json_write_object_end(struct spdk_json_write_ctx *w)
{
	w->first_value = false;
	if (w->indent == 0) { return fail(w); }
	w->indent--;
	if (!w->new_indent) {
		if (emit_fmt(w, "\n", 1)) { return fail(w); }
		if (emit_indent(w)) { return fail(w); }
	}
	w->new_indent = false;
	return emit(w, "}", 1);
}

/*
 * [한국어]
 * spdk_json_write_name_raw - 객체 키(name + ":") 출력.
 *
 * 동작: begin_value(콤마 등) → 문자열 escape로 "name" 출력 → ":" → (FORMATTED 시) " ".
 *   key 다음에는 first_value=true로 다시 세팅 — 그래야 이어지는 value 앞에 콤마가 안 들어간다.
 *
 * 호출 체인: spdk_json_write_named_<type> → spdk_json_write_name → spdk_json_write_name_raw.
 */
int
spdk_json_write_name_raw(struct spdk_json_write_ctx *w, const char *name, size_t len)
{
	/* TODO: check that container is an object */
	if (begin_value(w)) { return fail(w); }                  /* [한국어] 객체 안 새 항목 — 이전 (key,value)와 사이 콤마 처리 */
	if (write_string_or_name(w, name, len)) { return fail(w); } /* [한국어] key를 escape 처리해 따옴표로 감쌈 */
	w->first_value = true;                                    /* [한국어] 다음 value 앞에 콤마 금지 */
	if (emit(w, ":", 1)) { return fail(w); }
	return emit_fmt(w, " ", 1);                                /* [한국어] FORMATTED면 ": " 스타일, 아니면 ":" */
}

/* [한국어] spdk_json_write_name — NUL-종결 C string wrapper. strlen 후 _raw 위임. */
int
spdk_json_write_name(struct spdk_json_write_ctx *w, const char *name)
{
	return spdk_json_write_name_raw(w, name, strlen(name));
}

/*
 * [한국어]
 * spdk_json_write_val - 외부 spdk_json_val 토큰(트리)을 그대로 writer로 재출력.
 *
 * 사용처: 클라이언트가 보낸 JSON snippet을 그대로 forward(예: bdev_get_bdevs 같은 RPC가
 *   이전 단계에서 받은 옵션 객체를 응답에 포함시키는 패턴), 또는 파서 결과를 정규화/pretty-print.
 *
 * 동작: 토큰 type별 분기 — primitive는 단순 위임, 컨테이너는 자식들을 순회하며 재귀.
 *   컨테이너 자식 순회 시 하위 컨테이너는 _BEGIN.len + 2(BEGIN+END)만큼 점프.
 */
int
spdk_json_write_val(struct spdk_json_write_ctx *w, const struct spdk_json_val *val)
{
	size_t num_values, i;

	switch (val->type) {
	case SPDK_JSON_VAL_NUMBER:
		return spdk_json_write_val_raw(w, val->start, val->len);     /* [한국어] 숫자 텍스트 그대로(소수 정밀도 보존) */

	case SPDK_JSON_VAL_STRING:
		return spdk_json_write_string_raw(w, val->start, val->len);  /* [한국어] escape 재인코딩 거쳐 출력 */

	case SPDK_JSON_VAL_NAME:
		return spdk_json_write_name_raw(w, val->start, val->len);    /* [한국어] 객체 키 출력 */

	case SPDK_JSON_VAL_TRUE:
		return spdk_json_write_bool(w, true);

	case SPDK_JSON_VAL_FALSE:
		return spdk_json_write_bool(w, false);

	case SPDK_JSON_VAL_NULL:
		return spdk_json_write_null(w);

	case SPDK_JSON_VAL_ARRAY_BEGIN:
	case SPDK_JSON_VAL_OBJECT_BEGIN:
		num_values = val[0].len;                                      /* [한국어] _BEGIN의 len = 자식 토큰 수 */

		if (val[0].type == SPDK_JSON_VAL_OBJECT_BEGIN) {
			if (spdk_json_write_object_begin(w)) {
				return fail(w);
			}
		} else {
			if (spdk_json_write_array_begin(w)) {
				return fail(w);
			}
		}

		/* Loop up to and including the _END value */
		for (i = 0; i < num_values + 1;) {                           /* [한국어] +1: 마지막 _END까지 포함 */
			if (spdk_json_write_val(w, &val[i + 1])) {
				return fail(w);                                       /* [한국어] 자식 재귀 호출 */
			}
			if (val[i + 1].type == SPDK_JSON_VAL_ARRAY_BEGIN ||
			    val[i + 1].type == SPDK_JSON_VAL_OBJECT_BEGIN) {
				i += val[i + 1].len + 2;                              /* [한국어] 하위 컨테이너 통째 점프(BEGIN+자식+END) */
			} else {
				i++;                                                   /* [한국어] 단일 토큰 1개 진행 */
			}
		}
		return 0;

	case SPDK_JSON_VAL_ARRAY_END:
		return spdk_json_write_array_end(w);

	case SPDK_JSON_VAL_OBJECT_END:
		return spdk_json_write_object_end(w);

	case SPDK_JSON_VAL_INVALID:
		/* Handle INVALID to make the compiler happy (and catch other unhandled types) */
		return fail(w);                                                /* [한국어] INVALID 토큰은 출력 불가 — 호출자 에러 */
	}

	return fail(w);                                                    /* [한국어] enum 외 값(있을 수 없음) 방어 */
}

/*
 * [한국어]
 * spdk_json_write_named_* 패밀리 — 객체 내부에서 (name, value) 쌍을 한 번에.
 *
 * 모든 함수가 동일한 구조: spdk_json_write_name(name) → 실패 시 즉시 반환,
 *   성공 시 대응하는 value writer 호출. 호출자 코드를 줄이고 자동 콤마/들여쓰기 처리에 일관성을 줌.
 *
 * 호출 빈도가 매우 높은 hot path — RPC 응답 생성, config dump, 통계 출력 등 모든 곳에서 사용.
 */

/* [한국어] named_null — "name": null */
int
spdk_json_write_named_null(struct spdk_json_write_ctx *w, const char *name)
{
	int rc = spdk_json_write_name(w, name);
	return rc ? rc : spdk_json_write_null(w);
}

/* [한국어] named_bool — "name": true/false */
int
spdk_json_write_named_bool(struct spdk_json_write_ctx *w, const char *name, bool val)
{
	int rc = spdk_json_write_name(w, name);

	return rc ? rc : spdk_json_write_bool(w, val);
}

/* [한국어] named_uint8/16/32/64 + int32/64 + double — 정수/실수 한 번에. */
int
spdk_json_write_named_uint8(struct spdk_json_write_ctx *w, const char *name, uint8_t val)
{
	int rc = spdk_json_write_name(w, name);

	return rc ? rc : spdk_json_write_uint8(w, val);
}

int
spdk_json_write_named_uint16(struct spdk_json_write_ctx *w, const char *name, uint16_t val)
{
	int rc = spdk_json_write_name(w, name);

	return rc ? rc : spdk_json_write_uint16(w, val);
}

int
spdk_json_write_named_int32(struct spdk_json_write_ctx *w, const char *name, int32_t val)
{
	int rc = spdk_json_write_name(w, name);

	return rc ? rc : spdk_json_write_int32(w, val);
}

int
spdk_json_write_named_uint32(struct spdk_json_write_ctx *w, const char *name, uint32_t val)
{
	int rc = spdk_json_write_name(w, name);

	return rc ? rc : spdk_json_write_uint32(w, val);
}

int
spdk_json_write_named_int64(struct spdk_json_write_ctx *w, const char *name, int64_t val)
{
	int rc = spdk_json_write_name(w, name);

	return rc ? rc : spdk_json_write_int64(w, val);
}

int
spdk_json_write_named_uint64(struct spdk_json_write_ctx *w, const char *name, uint64_t val)
{
	int rc = spdk_json_write_name(w, name);

	return rc ? rc : spdk_json_write_uint64(w, val);
}

int
spdk_json_write_named_double(struct spdk_json_write_ctx *w, const char *name, double val)
{
	int rc = spdk_json_write_name(w, name);

	return rc ? rc : spdk_json_write_double(w, val);
}

/* [한국어] named_string — "name": "<val>" (NUL-종결 문자열). */
int
spdk_json_write_named_string(struct spdk_json_write_ctx *w, const char *name, const char *val)
{
	int rc = spdk_json_write_name(w, name);

	return rc ? rc : spdk_json_write_string(w, val);
}

/*
 * [한국어]
 * spdk_json_write_named_string_fmt - "name": "<printf 결과>" 한 번에.
 * 사용처: 사람 읽는 메시지/디버그 로그성 필드.
 */
int
spdk_json_write_named_string_fmt(struct spdk_json_write_ctx *w, const char *name,
				 const char *fmt, ...)
{
	va_list args;
	int rc;

	va_start(args, fmt);
	rc = spdk_json_write_named_string_fmt_v(w, name, fmt, args);
	va_end(args);

	return rc;
}

/* [한국어] _v 버전 — va_list로 분리된 본체. name 출력 후 vsprintf_alloc으로 임시 string 생성 → 정리. */
int
spdk_json_write_named_string_fmt_v(struct spdk_json_write_ctx *w, const char *name,
				   const char *fmt, va_list args)
{
	char *s;
	int rc;

	rc = spdk_json_write_name(w, name);
	if (rc) {
		return rc;
	}

	s = spdk_vsprintf_alloc(fmt, args);

	if (s == NULL) {
		return -1;                                /* [한국어] OOM */
	}

	rc = spdk_json_write_string(w, s);
	free(s);
	return rc;
}

/* [한국어] named_bytearray — "name": "<hex>" — binary 식별자(WWN/MAC/NQN raw)에. */
int
spdk_json_write_named_bytearray(struct spdk_json_write_ctx *w, const char *name, const void *val,
				size_t len)
{
	int rc = spdk_json_write_name(w, name);

	return rc ? rc : spdk_json_write_bytearray(w, val, len);
}

/* [한국어] named_array_begin — "name": [ — 호출 후 사용자가 array 컨텐츠 출력. */
int
spdk_json_write_named_array_begin(struct spdk_json_write_ctx *w, const char *name)
{
	int rc = spdk_json_write_name(w, name);

	return rc ? rc : spdk_json_write_array_begin(w);
}

/* [한국어] named_object_begin — "name": { */
int
spdk_json_write_named_object_begin(struct spdk_json_write_ctx *w, const char *name)
{
	int rc = spdk_json_write_name(w, name);

	return rc ? rc : spdk_json_write_object_begin(w);
}

/* [한국어] named_uuid — "name": "xxxxxxxx-..." UUID 표준 표기. */
int
spdk_json_write_named_uuid(struct spdk_json_write_ctx *w, const char *name,
			   const struct spdk_uuid *uuid)
{
	int rc = spdk_json_write_name(w, name);

	return rc ? rc : spdk_json_write_uuid(w, uuid);
}
