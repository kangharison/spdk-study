/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK Zero-copy JSON 파서 구현 (json_parse.c)
 *
 * === 파일의 역할 ===
 * 외부 입력(소켓 byte stream / 설정 파일 / RPC 요청)으로 들어온 JSON 텍스트를
 * "복사 없이" 토큰화하여 spdk_json_val 배열로 변환하는 모듈이다. 파서의 핵심
 * 출력은 (start, len, type) 트리플의 배열이며, 각 토큰은 입력 버퍼 안의 바이트
 * 위치를 그대로 가리킨다 — 즉 별도의 토큰 버퍼/문자열 복제를 두지 않는다.
 * 객체/배열은 _BEGIN과 _END 두 개의 가상 토큰으로 표현되며, _BEGIN 토큰의 len
 * 필드에는 "그 컨테이너 안에 들어 있는 자식 토큰의 총 개수"가 저장되어 트리
 * 순회를 O(1)로 만든다. 문자열의 경우 (옵션 SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE를
 * 켜면) escape 시퀀스를 입력 버퍼 안에서 해독하면서 in-place로 압축한다 —
 * 입력 버퍼 자체가 수정되는 부수효과가 있으므로 호출자는 원본 보존 여부를
 * 미리 판단해야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 JSON-RPC 서버(lib/jsonrpc), config 파서(lib/init), bdev/nvmf 등 모든
 * 서브시스템의 RPC 핸들러가 이 파일의 spdk_json_parse()를 거쳐 입력을 받는다.
 * 일반적인 호출 체인:
 *   소켓/파일 read → byte buffer → spdk_json_parse(buf, size, vals[], ...)
 *     → spdk_json_val[]
 *     → spdk_json_decode_object(decoders, ...) (json_util.c에 위치)
 *     → C struct 필드 채움
 *     → RPC 핸들러 본문 실행
 * 실행 컨텍스트는 호출자 스레드(보통 RPC 서버를 처리하는 SPDK reactor 스레드)이며
 * 본 파일 자체는 어떠한 SPDK 락/스레드 API도 호출하지 않는다(순수 함수 모듈).
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/json.h: spdk_json_val/spdk_json_val_type/SPDK_JSON_PARSE_* 상수 정의.
 *   - spdk_internal/utf.h: utf8_valid/utf8_encode_unsafe/utf8_codepoint_len/
 *     utf16_valid_surrogate_high/_low, utf16_decode_surrogate_pair 등 — RFC 8259가
 *     요구하는 UTF-8/UTF-16 surrogate 처리에 사용.
 *   - libc: assert, memcmp, memmove, memchr, calloc/free 미사용(파서 자체는 stack
 *     기반 — 동적할당 없음. depth 64까지의 컨테이너 스택을 자동변수로 잡음).
 * 의존받음(피호출): 본 파일은 외부 헤더의 구조체 spdk_json_val 배열을 채운다.
 *   호출자는 채워진 배열을 lib/json/json_util.c의 디코더 함수들에 넘겨 C
 *   자료구조로 변환한다. 그 외에 lib/jsonrpc/jsonrpc_server*가 직접 토큰
 *   배열을 순회하기도 한다.
 * 데이터 흐름: byte buffer 입력 → 토큰 인덱스 → 호출자 디코드/검색.
 * 공유 상태: 없음. 모든 상태(depth/containers/state)는 spdk_json_parse 함수
 *   프레임 안에 자동 변수로만 존재.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_json_parse(): 공개 진입점. State machine으로 한 바이트씩 읽으며 토큰화.
 *   - json_decode_string(): "..." 본문 스캔 + escape/UTF-8 검증 + (옵션) in-place 디코드.
 *   - json_decode_string_escape(): 백슬래시 escape 디스패치(\b\f\n\r\t\\\"\/와 \uXXXX).
 *   - json_decode_string_escape_unicode(): \uXXXX 1~2개를 codepoint로 → UTF-8 인코딩.
 *   - json_decode_string_escape_twochar(): 2글자 escape를 단일 바이트로 매핑.
 *   - json_valid_number(): RFC 8259 number 문법(-?int(.frac)?(eE±exp)?) 검증 — goto 기반 DFA.
 *   - json_valid_comment(): // 또는 /∗ ... ∗/ 주석 스킵(기본 비활성, FLAG_ALLOW_COMMENTS).
 *   - match_literal()/g_json_literals[]: true/false/null 리터럴 매칭. 첫 글자의 비트 3,4를
 *     index로 쓰는 트릭으로 분기 비용을 제거.
 *   - SPDK_JSON_MAX_NESTING_DEPTH(=64): {/[ 중첩 한계 — 스택 오버플로/악성 입력 방어.
 *   - state machine: STATE_VALUE / _NAME / _NAME_SEPARATOR / _VALUE_SEPARATOR / _END.
 */

#include "spdk/json.h"          /* [한국어] 공개 API 선언 — spdk_json_val/spdk_json_val_type/SPDK_JSON_PARSE_* */

#include "spdk_internal/utf.h"  /* [한국어] UTF-8/UTF-16 검증·인코딩 헬퍼(파서 내부에서만 사용) */

#define SPDK_JSON_MAX_NESTING_DEPTH	64
/* [한국어] JSON 객체/배열의 최대 중첩 깊이.
 * 의도: (1) 자동 배열 containers[]/con_value[]의 크기를 컴파일 타임에 고정,
 *        (2) 무한 중첩으로 스택을 폭파시키려는 악성 입력 방어,
 *        (3) 일반적인 RPC payload 깊이로 충분(64는 사실상 무제한).
 * 초과 시 spdk_json_parse는 SPDK_JSON_PARSE_MAX_DEPTH_EXCEEDED를 반환한다. */

/*
 * [한국어]
 * hex_value - 단일 ASCII 문자를 16진수 값(0~15)로 변환. '0'~'9','A'~'F','a'~'f' 외 입력은 -1을 반환.
 *
 * @c: 16진수 문자로 추정되는 1바이트(소문자/대문자 혼용 허용).
 * @return: 0~15 정수(유효 hex), 또는 -1(무효 문자).
 *
 * 동기: \uXXXX 디코드에서 4자리 hex를 빠르게 변환해야 하는데, 분기·subtract
 *       조합 대신 256개 짜리 lookup table 한 번으로 처리해 분기 예측 부담을 없앤다.
 * 트릭: V() 매크로가 [x] = y + 1로 저장 → 0이 "유효 0"이 아니라 "엔트리 미설정"을
 *       의미하도록 만들어, 마지막에 -1을 빼서 "엔트리 미설정 → -1" + "유효 hex → 본래 값"이
 *       동시에 처리되게 한다. C99 designated initializer로 256바이트 테이블이 .rodata에 박힘.
 * 호출 체인: json_decode_string_escape_unicode → hex_value (4번 호출).
 */
static int
hex_value(uint8_t c)
{
#define V(x, y) [x] = y + 1                       /* [한국어] designated initializer 매크로 — 0(미설정) ↔ y+1(유효 매핑) 구분 */
	static const int8_t val[256] = {              /* [한국어] 256바이트 정적 테이블 — 모든 바이트 입력에 안전 */
		V('0', 0), V('1', 1), V('2', 2), V('3', 3), V('4', 4),                         /* [한국어] 0~4 매핑 */
		V('5', 5), V('6', 6), V('7', 7), V('8', 8), V('9', 9),                         /* [한국어] 5~9 매핑 */
		V('A', 0xA), V('B', 0xB), V('C', 0xC), V('D', 0xD), V('E', 0xE), V('F', 0xF), /* [한국어] 대문자 hex */
		V('a', 0xA), V('b', 0xB), V('c', 0xC), V('d', 0xD), V('e', 0xE), V('f', 0xF), /* [한국어] 소문자 hex */
	};
#undef V                                          /* [한국어] 매크로 누수 방지 */

	return val[c] - 1;                            /* [한국어] 미설정(0) → -1, 유효 → 원래 값. 한 번의 산술로 분기 제거 */
}

/*
 * [한국어]
 * json_decode_string_escape_unicode - \uXXXX(또는 \uXXXX\uYYYY surrogate pair) 한 묶음을 UTF-8로 변환.
 *
 * @strp: in/out 입력 포인터의 포인터. 호출 시 백슬래시('\\')를 가리키고, 성공 시 escape 시퀀스 다음 바이트로 전진.
 * @buf_end: 입력 버퍼의 한 칸 뒤(end sentinel). 모든 경계 체크에 사용.
 * @out: in-place 디코드 출력 위치. NULL이면 길이만 계산(SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE 미지정 시).
 * @return: 출력에 쓴 바이트 수(>0), 또는 SPDK_JSON_PARSE_INVALID/_INCOMPLETE.
 *
 * 동기: JSON spec(RFC 8259)이 BMP 외 codepoint는 UTF-16 surrogate pair(\uD800~\uDBFF \uDC00~\uDFFF)로
 *       표기하도록 강제한다. 따라서 첫 \uXXXX가 high surrogate이면 다음 \uYYYY(low)까지 같이 읽어
 *       단일 codepoint로 합친 뒤 UTF-8(최대 4바이트)로 인코딩해야 한다.
 * In-place 안전성: \uXXXX는 6바이트(surrogate pair는 12바이트)인데 UTF-8 최대 인코딩은 4바이트이므로
 *       항상 입력보다 짧거나 같다 — 같은 버퍼에 덮어써도 미래 바이트를 침범하지 않음.
 * 호출 체인: json_decode_string_escape → json_decode_string_escape_unicode → utf16_*/utf8_encode_unsafe.
 */
static int
json_decode_string_escape_unicode(uint8_t **strp, uint8_t *buf_end, uint8_t *out)
{
	uint8_t *str = *strp;             /* [한국어] 로컬 작업 포인터(원본 *strp는 성공 시에만 갱신) */
	int v0, v1, v2, v3;               /* [한국어] hex 4자리 값(상위→하위 순으로 v3..v0) */
	uint32_t val;                     /* [한국어] 누적된 16비트 codepoint(또는 surrogate 값) */
	uint32_t surrogate_high = 0;      /* [한국어] 직전에 high surrogate를 봤으면 그 값을 저장(0이면 미상태) */
	int rc;                           /* [한국어] utf8_encode_unsafe/utf8_codepoint_len 반환 */
decode:
	/* \uXXXX */
	assert(buf_end > str);            /* [한국어] 호출자가 보장: 진입 시 최소 1바이트 남음 */

	if (*str++ != '\\') { return SPDK_JSON_PARSE_INVALID; }   /* [한국어] '\\'로 시작해야 escape — 아니면 즉시 무효 */
	if (buf_end == str) { return SPDK_JSON_PARSE_INCOMPLETE; } /* [한국어] 다음 바이트('u') 부족 → 더 받아야 함 */

	if (*str++ != 'u') { return SPDK_JSON_PARSE_INVALID; }     /* [한국어] '\u' 형태만 허용(\x 등 다른 hex escape는 JSON 비표준) */
	if (buf_end == str) { return SPDK_JSON_PARSE_INCOMPLETE; } /* [한국어] hex 4자리 시작 전에 끊김 */

	if ((v3 = hex_value(*str++)) < 0) { return SPDK_JSON_PARSE_INVALID; } /* [한국어] 1번째 hex(가장 높은 nibble) */
	if (buf_end == str) { return SPDK_JSON_PARSE_INCOMPLETE; }

	if ((v2 = hex_value(*str++)) < 0) { return SPDK_JSON_PARSE_INVALID; } /* [한국어] 2번째 hex */
	if (buf_end == str) { return SPDK_JSON_PARSE_INCOMPLETE; }

	if ((v1 = hex_value(*str++)) < 0) { return SPDK_JSON_PARSE_INVALID; } /* [한국어] 3번째 hex */
	if (buf_end == str) { return SPDK_JSON_PARSE_INCOMPLETE; }

	if ((v0 = hex_value(*str++)) < 0) { return SPDK_JSON_PARSE_INVALID; } /* [한국어] 4번째(가장 낮은) hex */
	if (buf_end == str) { return SPDK_JSON_PARSE_INCOMPLETE; }            /* [한국어] surrogate 후속을 확인하기 위한 lookahead */

	val = v0 | (v1 << 4) | (v2 << 8) | (v3 << 12);  /* [한국어] big-endian으로 16비트 정수 조립(JSON \uXXXX는 항상 빅엔디안 표기) */

	if (surrogate_high) {
		/* We already parsed the high surrogate, so this should be the low part. */
		if (!utf16_valid_surrogate_low(val)) {  /* [한국어] high 다음에는 반드시 0xDC00~0xDFFF 범위의 low여야 함 */
			return SPDK_JSON_PARSE_INVALID;
		}

		/* Convert UTF-16 surrogate pair into codepoint and fall through to utf8_encode. */
		val = utf16_decode_surrogate_pair(surrogate_high, val); /* [한국어] (high,low) → 0x10000~0x10FFFF 범위 codepoint */
	} else if (utf16_valid_surrogate_high(val)) {
		surrogate_high = val;                                   /* [한국어] high를 보관, low를 받기 위해 루프백 */

		/*
		 * We parsed a \uXXXX sequence that decoded to the first half of a
		 *  UTF-16 surrogate pair, so it must be immediately followed by another
		 *  \uXXXX escape.
		 *
		 * Loop around to get the low half of the surrogate pair.
		 */
		if (buf_end == str) { return SPDK_JSON_PARSE_INCOMPLETE; } /* [한국어] low \uXXXX를 시작할 바이트가 없음 */
		goto decode;                                               /* [한국어] decode 라벨로 점프해 두 번째 \uXXXX를 다시 파싱 */
	} else if (utf16_valid_surrogate_low(val)) {
		/*
		 * We found the second half of surrogate pair without the first half;
		 *  this is an invalid encoding.
		 */
		return SPDK_JSON_PARSE_INVALID;       /* [한국어] high 없는 low 단독은 RFC상 잘못된 시퀀스 */
	}

	/*
	 * Convert Unicode escape (or surrogate pair) to UTF-8 in place.
	 *
	 * This is safe (will not write beyond the buffer) because the \uXXXX sequence is 6 bytes
	 *  (or 12 bytes for surrogate pairs), and the longest possible UTF-8 encoding of a
	 *  single codepoint is 4 bytes.
	 */
	if (out) {
		rc = utf8_encode_unsafe(out, val);  /* [한국어] 실제 UTF-8 바이트열을 out 위치에 기록 */
	} else {
		rc = utf8_codepoint_len(val);       /* [한국어] 길이 계산 모드(in-place 비활성) — 디코드 길이만 리턴 */
	}
	if (rc < 0) {
		return SPDK_JSON_PARSE_INVALID;     /* [한국어] codepoint가 0x10FFFF 초과 등 UTF-8 인코드 불가 */
	}

	*strp = str; /* update input pointer */ /* [한국어] 성공 시에만 호출자 포인터 갱신(부분 실패 시 원본 유지) */
	return rc; /* return number of bytes decoded */ /* [한국어] 출력 바이트 수(상위 디코더가 out 진행에 사용) */
}

/*
 * [한국어]
 * json_decode_string_escape_twochar - 2글자 단순 escape(\b\f\n\r\t\\\"\/)를 단일 바이트로 매핑.
 *
 * @strp: in/out 입력 포인터의 포인터(현재 '\\'를 가리킴). 성공 시 2바이트 전진.
 * @buf_end: 입력 종료 위치(end sentinel).
 * @out: in-place 디코드 시 결과 1바이트를 기록할 위치, 또는 NULL(길이만 계산).
 * @return: 1(생성 바이트 수) 또는 SPDK_JSON_PARSE_INCOMPLETE/_INVALID.
 *
 * 동기: \uXXXX보다 흔한 일반 escape를 lookup 한 번으로 빠르게 처리하기 위한 분리 함수.
 *       json_decode_string_escape에서 먼저 호출되며, 실패하면 \uXXXX 디코더로 fallback.
 * 보안: escapes[]에 등록되지 않은 두 번째 바이트(0)는 무효로 거부 — 임의 escape 허용 금지.
 * 호출 체인: json_decode_string_escape → json_decode_string_escape_twochar.
 */
static int
json_decode_string_escape_twochar(uint8_t **strp, uint8_t *buf_end, uint8_t *out)
{
	static const uint8_t escapes[256] = {        /* [한국어] 256바이트 lookup — 입력 byte → 디코드 byte */
		['b'] = '\b',                            /* [한국어] backspace */
		['f'] = '\f',                            /* [한국어] form feed */
		['n'] = '\n',                            /* [한국어] line feed */
		['r'] = '\r',                            /* [한국어] carriage return */
		['t'] = '\t',                            /* [한국어] horizontal tab */
		['/'] = '/',                             /* [한국어] slash — JSON spec이 허용한 옵셔널 escape */
		['"'] = '"',                             /* [한국어] 따옴표 */
		['\\'] = '\\',                           /* [한국어] 백슬래시 자체 */
	};
	uint8_t *str = *strp;                        /* [한국어] 로컬 작업 포인터 */
	uint8_t c;                                   /* [한국어] 매핑 결과 바이트(0이면 미매핑) */

	assert(buf_end > str);                       /* [한국어] 호출자 보장 — 최소 1바이트 존재 */
	if (buf_end - str < 2) {
		return SPDK_JSON_PARSE_INCOMPLETE;       /* [한국어] '\\' 다음 1바이트가 부족 → 추가 입력 필요 */
	}

	assert(str[0] == '\\');                      /* [한국어] 호출자가 escape 시작('\\')을 보장 */

	c = escapes[str[1]];                         /* [한국어] '\\' 다음 바이트로 lookup */
	if (c) {
		if (out) {
			*out = c;                            /* [한국어] in-place 디코드 모드면 디코드 바이트 기록 */
		}
		*strp += 2; /* consumed two bytes */     /* [한국어] '\\X' 두 바이트 소비 */
		return 1; /* produced one byte */        /* [한국어] 출력은 항상 1바이트 */
	}

	return SPDK_JSON_PARSE_INVALID;              /* [한국어] 등록되지 않은 escape는 spec 위반 */
}

/*
 * Decode JSON string backslash escape.
 * \param strp pointer to pointer to first character of escape (the backslash).
 *  *strp is also advanced to indicate how much input was consumed.
 *
 * \return Number of bytes appended to out
 */
/*
 * [한국어]
 * json_decode_string_escape - 임의의 백슬래시 escape를 디스패치(2글자 우선, 실패 시 \uXXXX).
 *
 * @strp/@buf_end/@out: 위 두 헬퍼와 동일.
 * @return: out에 쓴 바이트 수(>0), 또는 SPDK_JSON_PARSE_INVALID/_INCOMPLETE.
 *
 * 호출자(json_decode_string)가 '\\'를 만나면 진입한다. 흔한 케이스(\n,\t 등)를 빠르게 처리하고,
 *   매핑되지 않은 escape인 경우에만 \uXXXX 분기로 떨어진다(\u 외에는 결국 unicode 함수도 무효 반환).
 */
static int
json_decode_string_escape(uint8_t **strp, uint8_t *buf_end, uint8_t *out)
{
	int rc;

	rc = json_decode_string_escape_twochar(strp, buf_end, out); /* [한국어] 빠른 경로: 2글자 escape 시도 */
	if (rc > 0) {
		return rc;                                              /* [한국어] 2글자 매핑 성공 → 즉시 반환 */
	}

	return json_decode_string_escape_unicode(strp, buf_end, out); /* [한국어] fallback: \uXXXX 시도(실패 시 그대로 에러 전파) */
}

/*
 * Decode JSON string in place.
 *
 * \param str_start Pointer to the beginning of the string (the opening " character).
 *
 * \return Number of bytes in decoded string (beginning from start).
 */
/*
 * [한국어]
 * json_decode_string - "..." 문자열 토큰을 스캔하고(옵션상) in-place 디코드.
 *
 * @str_start: 입력 버퍼 안 여는 따옴표('"')를 가리키는 포인터.
 * @buf_end: 입력 버퍼의 한 칸 뒤(end sentinel).
 * @str_end: out — 닫는 따옴표 다음 위치(다음 토큰 스캔 시작점). 에러 시 마지막으로 본 위치.
 * @flags: SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE 비트 사용. 켜져 있으면 escape를 풀어 입력 버퍼 in-place 작성.
 * @return: 디코드 후 문자열의 바이트 길이(>=0) 또는 SPDK_JSON_PARSE_INVALID/_INCOMPLETE.
 *
 * 동작: 따옴표 안의 바이트를 한 글자씩 스캔하며,
 *   1) 닫는 '"'를 만나면 종료.
 *   2) '\\'면 escape 디코더 호출 → out 위치에 디코드 결과 기록.
 *   3) 0x00~0x1F는 제어문자 — RFC상 반드시 escape 해야 하므로 raw 등장은 무효.
 *   4) 그 외에는 utf8_valid로 1~4바이트 UTF-8 시퀀스를 검증 후 그대로 통과.
 * In-place 시멘틱: out은 항상 str보다 작거나 같은 위치를 유지(escape는 항상 입력보다 같거나 짧음).
 *   `flags`가 꺼져 있으면 출력 위치를 갱신만 하고 실제로 쓰진 않음(memmove 생략).
 * 호출 체인: spdk_json_parse(STATE_VALUE/STATE_NAME에서 '"' 분기) → json_decode_string.
 */
static int
json_decode_string(uint8_t *str_start, uint8_t *buf_end, uint8_t **str_end, uint32_t flags)
{
	uint8_t *str = str_start;                        /* [한국어] 입력 진행 포인터(곧 시작 따옴표 '"' 위로 이동) */
	uint8_t *out = str_start + 1; /* Decode string in place (skip the initial quote) */ /* [한국어] 출력은 시작 따옴표 다음 칸부터 — 토큰 길이는 (out - (start+1))로 계산 */
	int rc;                                          /* [한국어] escape/utf8_valid 반환값 임시 변수 */

	if (buf_end - str_start < 2) {
		/*
		 * Shortest valid string (the empty string) is two bytes (""),
		 *  so this can't possibly be valid
		 */
		*str_end = str;                              /* [한국어] 호출자에게 마지막 본 위치 보고 */
		return SPDK_JSON_PARSE_INCOMPLETE;           /* [한국어] 스트리밍 입력 — 더 받으면 완성 가능 */
	}

	if (*str++ != '"') {
		*str_end = str;                              /* [한국어] 시작이 따옴표가 아니면 즉시 무효 */
		return SPDK_JSON_PARSE_INVALID;
	}

	while (str < buf_end) {                          /* [한국어] 본문 한 글자씩 스캔 */
		if (str[0] == '"') {
			/*
			 * End of string.
			 * Update str_end to point at next input byte and return output length.
			 */
			*str_end = str + 1;                       /* [한국어] 닫는 '"' 다음으로 진행 — 호출자가 이 위치부터 다음 토큰 스캔 */
			return out - str_start - 1;               /* [한국어] 디코드된 본문 길이(시작 따옴표 1바이트 제외) */
		} else if (str[0] == '\\') {
			rc = json_decode_string_escape(&str, buf_end,
						       flags & SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE ? out : NULL); /* [한국어] in-place 모드면 out에 직접 작성, 아니면 길이만 계산 */
			assert(rc != 0);                          /* [한국어] escape 디코더는 0을 반환하지 않음(>0 또는 음수 에러) */
			if (rc < 0) {
				*str_end = str;                       /* [한국어] 디코드 실패 — 호출자가 이 위치 기준으로 에러 처리 */
				return rc;
			}
			out += rc;                                /* [한국어] 출력 길이만 누적(in-place 비활성 시 실제 메모리는 변경되지 않음) */
		} else if (str[0] <= 0x1f) {
			/* control characters must be escaped */
			*str_end = str;
			return SPDK_JSON_PARSE_INVALID;           /* [한국어] RFC 8259: 0x00~0x1F는 raw 금지 → escape 강제 */
		} else {
			rc = utf8_valid(str, buf_end);            /* [한국어] 1~4바이트 UTF-8 시퀀스 검증. 0=truncated, <0=invalid, >0=length */
			if (rc == 0) {
				*str_end = str;
				return SPDK_JSON_PARSE_INCOMPLETE;    /* [한국어] 다중 바이트 UTF-8 중간에 버퍼 끝 — 더 받아야 함 */
			} else if (rc < 0) {
				*str_end = str;
				return SPDK_JSON_PARSE_INVALID;       /* [한국어] overlong/illegal 시퀀스 등 RFC 위반 */
			}

			if (out && out != str && (flags & SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE)) {
				memmove(out, str, rc);                /* [한국어] in-place 디코드 시 escape로 인해 out과 str이 어긋났다면 본문을 앞으로 당김 */
			}
			out += rc;                                /* [한국어] 출력 진행 */
			str += rc;                                /* [한국어] 입력 진행 */
		}
	}

	/* If execution gets here, we ran out of buffer. */
	*str_end = str;                                   /* [한국어] 닫는 따옴표를 못 찾고 EOF — 더 받아야 함 */
	return SPDK_JSON_PARSE_INCOMPLETE;
}

/*
 * [한국어]
 * json_valid_number - JSON 숫자 토큰 문법(`-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?`)을 검증하면서 길이를 측정.
 *
 * @start: 숫자가 시작하는 위치(첫 디짓 또는 부호).
 * @buf_end: 입력 버퍼 종료 위치.
 * @return: 숫자 토큰의 바이트 길이(>0), SPDK_JSON_PARSE_INCOMPLETE(EOF로 끝남) 또는 SPDK_JSON_PARSE_INVALID.
 *
 * 동기: JSON spec(RFC 8259 §6)의 숫자 문법은 leading-zero 금지 등 비정규 정규식이라
 *       단순한 strtod 호출로는 제대로 검증되지 않는다. 따라서 명시적 DFA(상태기계)로 구현했다.
 *       각 라벨(num_int_digits, num_frac_first_digit, num_exp_sign, ...)이 DFA의 한 상태이다.
 *       goto는 컴파일러가 직접 점프 테이블/branchless 코드로 풀 수 있어 분기 비용을 최소화한다.
 * 종료 처리: 유효 종료 상태(done_valid)에서 buf_end에 도달하면 길이를 그대로 리턴.
 *   무효 종료 상태(done_invalid)에서 buf_end에 도달했으면 INCOMPLETE — 이어서 더 받으면 가능할 수 있음.
 *   확정적으로 무효한 문자가 끼었으면 INVALID.
 * 호출 체인: spdk_json_parse(STATE_VALUE에서 [-0-9] 분기) → json_valid_number.
 */
static int
json_valid_number(uint8_t *start, uint8_t *buf_end)
{
	uint8_t *p = start;                  /* [한국어] 작업 포인터 — DFA 진행 */
	uint8_t c;                            /* [한국어] 현재 검사 중인 바이트 */

	if (p >= buf_end) { return -1; }     /* [한국어] 빈 입력은 NUMBER가 될 수 없음 */

	c = *p++;
	if (c >= '1' && c <= '9') { goto num_int_digits; } /* [한국어] [1-9]로 시작 → 정수 자릿수 누적 상태 */
	if (c == '0') { goto num_frac_or_exp; }            /* [한국어] '0'은 단독 또는 0.x/0e 만 허용 — leading zero 금지 */
	if (c == '-') { goto num_int_first_digit; }        /* [한국어] 음수 부호 → 다음에 정수 첫 자릿수 필요 */
	p--;                                                /* [한국어] 위 어디에도 안 맞으면 시작조차 무효 — 입력 한 칸 되돌림 */
	goto done_invalid;

num_int_first_digit:                        /* [한국어] '-' 다음 — 반드시 [0-9] 한 글자 필요 */
	if (spdk_likely(p != buf_end)) {
		c = *p++;
		if (c == '0') { goto num_frac_or_exp; }            /* [한국어] -0 — 더 이상 정수 자리는 못 옴 */
		if (c >= '1' && c <= '9') { goto num_int_digits; } /* [한국어] -1~9 — 정수 누적 시작 */
		p--;                                               /* [한국어] 무효 글자 만남 — 한 칸 되돌리고 invalid */
	}
	goto done_invalid;                                     /* [한국어] EOF여도 부호만 있고 숫자 없음 → invalid(=INCOMPLETE 후속 처리) */

num_int_digits:                              /* [한국어] 정수 자릿수 0개 이상 누적 가능 상태 */
	if (spdk_likely(p != buf_end)) {
		c = *p++;
		if (c >= '0' && c <= '9') { goto num_int_digits; }   /* [한국어] 자릿수 계속 — self loop */
		if (c == '.') { goto num_frac_first_digit; }         /* [한국어] '.' 만나면 소수 부분 필수 */
		if (c == 'e' || c == 'E') { goto num_exp_sign; }     /* [한국어] 지수 부분 시작 */
		p--;                                                  /* [한국어] 숫자 종료 → 한 칸 되돌리고 valid */
	}
	goto done_valid;                                          /* [한국어] EOF는 정수 단독으로도 valid */

num_frac_or_exp:                              /* [한국어] '0' 또는 '-0' 직후 — 정수 자릿수 추가 금지(leading zero 차단) */
	if (spdk_likely(p != buf_end)) {
		c = *p++;
		if (c == '.') { goto num_frac_first_digit; }
		if (c == 'e' || c == 'E') { goto num_exp_sign; }
		p--;                                                  /* [한국어] 그 외 글자는 숫자 아님 → 한 칸 되돌림 */
	}
	goto done_valid;                                          /* [한국어] '0' 단독도 RFC상 합법 */

num_frac_first_digit:                         /* [한국어] '.' 직후 — 적어도 한 자릿수 필수 */
	if (spdk_likely(p != buf_end)) {
		c = *p++;
		if (c >= '0' && c <= '9') { goto num_frac_digits; }
		p--;
	}
	goto done_invalid;                                         /* [한국어] '.'으로 끝나면 incomplete 또는 invalid */

num_frac_digits:                              /* [한국어] 소수 자릿수 누적 가능 — self loop */
	if (spdk_likely(p != buf_end)) {
		c = *p++;
		if (c >= '0' && c <= '9') { goto num_frac_digits; }
		if (c == 'e' || c == 'E') { goto num_exp_sign; }
		p--;
	}
	goto done_valid;

num_exp_sign:                                 /* [한국어] 'e'/'E' 직후 — 부호 또는 숫자 가능 */
	if (spdk_likely(p != buf_end)) {
		c = *p++;
		if (c >= '0' && c <= '9') { goto num_exp_digits; }
		if (c == '-' || c == '+') { goto num_exp_first_digit; } /* [한국어] 부호는 옵셔널 */
		p--;
	}
	goto done_invalid;

num_exp_first_digit:                          /* [한국어] 지수 부호 다음 — 적어도 한 자릿수 필수 */
	if (spdk_likely(p != buf_end)) {
		c = *p++;
		if (c >= '0' && c <= '9') { goto num_exp_digits; }
		p--;
	}
	goto done_invalid;

num_exp_digits:                               /* [한국어] 지수 자릿수 누적 — self loop */
	if (spdk_likely(p != buf_end)) {
		c = *p++;
		if (c >= '0' && c <= '9') { goto num_exp_digits; }
		p--;
	}
	goto done_valid;

done_valid:
	/* Valid end state */
	return p - start;                          /* [한국어] 토큰 길이 = 진행한 바이트 수 */

done_invalid:
	/* Invalid end state */
	if (p == buf_end) {
		/* Hit the end of the buffer - the stream is incomplete. */
		return SPDK_JSON_PARSE_INCOMPLETE;     /* [한국어] 스트림 추가 입력으로 완성 가능성 → INCOMPLETE */
	}

	/* Found an invalid character in an invalid end state */
	return SPDK_JSON_PARSE_INVALID;            /* [한국어] 확실히 비정규 — 거부 */
}

/*
 * [한국어]
 * json_valid_comment - SPDK 확장 — `//` 한줄 주석 또는 `/∗ ... ∗/` 다중행 주석을 인식하고 길이 반환.
 *
 * @start: '/'를 가리키는 포인터.
 * @buf_end: 입력 종료.
 * @return: 주석 전체 바이트 수(>0) 또는 SPDK_JSON_PARSE_INVALID/_INCOMPLETE.
 *
 * 동기: 표준 JSON은 주석을 허용하지 않지만 SPDK config 파일에서는 가독성을 위해 허용한다.
 *       호출자(spdk_json_parse)는 SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS가 켜졌을 때만 진입한다.
 *       파싱 결과 토큰에는 주석이 포함되지 않으며, 파서는 단순히 길이만큼 입력을 스킵한다.
 * 호출 체인: spdk_json_parse(STATE_VALUE에서 '/' 분기, ALLOW_COMMENTS 시) → json_valid_comment.
 */
static int
json_valid_comment(const uint8_t *start, const uint8_t *buf_end)
{
	const uint8_t *p = start;        /* [한국어] 작업 포인터 */
	bool multiline;                  /* [한국어] true=/∗∗/ , false=// 형식 */

	assert(buf_end > p);             /* [한국어] 호출자 보장: 최소 1바이트 */
	if (buf_end - p < 2) {
		return SPDK_JSON_PARSE_INCOMPLETE;  /* [한국어] '/' 다음 바이트가 없으면 추가 입력 필요 */
	}

	if (p[0] != '/') {
		return SPDK_JSON_PARSE_INVALID;  /* [한국어] 호출자가 '/'를 보장하지만 방어적으로 한 번 더 확인 */
	}
	if (p[1] == '*') {
		multiline = true;            /* [한국어] /∗ 시작 — ∗/까지 스캔 */
	} else if (p[1] == '/') {
		multiline = false;           /* [한국어] // 시작 — 줄바꿈까지 스캔 */
	} else {
		return SPDK_JSON_PARSE_INVALID;  /* [한국어] '/' 단독은 JSON 토큰 아님 */
	}
	p += 2;                          /* [한국어] '/'와 '*'/'/' 두 글자 소비 */

	if (multiline) {
		while (p != buf_end - 1) {                 /* [한국어] 두 글자 lookahead 가능한 동안 진행 */
			if (p[0] == '*' && p[1] == '/') {
				/* Include the terminating star and slash in the comment */
				return p - start + 2;              /* [한국어] '*' '/'를 포함한 총 길이를 반환 */
			}
			p++;
		}
	} else {
		while (p != buf_end) {
			if (*p == '\r' || *p == '\n') {
				/* Do not include the line terminator in the comment */
				return p - start;                  /* [한국어] 라인 종료 문자는 주석에 포함하지 않고 다음 토큰으로 넘김 */
			}
			p++;
		}
	}

	return SPDK_JSON_PARSE_INCOMPLETE; /* [한국어] 주석 종료 표지를 못 찾고 EOF — 추가 입력 필요 */
}

/*
 * [한국어] JSON 리터럴(true/false/null) 정의 테이블 엔트리.
 * 한 엔트리는 (토큰 타입, 문자열 길이, 문자열 본문)을 표현한다.
 * 메인 파서가 첫 글자만 보고 후보를 결정한 뒤, 본 구조체의 str/len으로 본문을 검증한다.
 */
struct json_literal {
	enum spdk_json_val_type type;
	/* [한국어] 매칭 성공 시 토큰에 부여할 타입(SPDK_JSON_VAL_TRUE/_FALSE/_NULL).
	 * 설정자: g_json_literals 정적 초기화에서만.
	 * 읽는 자: spdk_json_parse가 토큰 타입을 결정할 때 사용.
	 * 동기화: 정적 read-only 테이블이므로 락 불필요. */

	uint32_t len;
	/* [한국어] 리터럴 문자열의 바이트 길이("true"=4, "false"=5, "null"=4).
	 * memcmp 호출 길이로 사용. */

	uint8_t str[8];
	/* [한국어] 리터럴 본문(NUL 종결 보장 위해 여유 8바이트).
	 * 8로 잡은 이유는 캐시 라인 정렬 + 가장 긴 "false"(5) + NUL 여유. */
};

/*
 * JSON only defines 3 possible literals; they can be uniquely identified by bits
 *  3 and 4 of the first character:
 *   'f' = 0b11[00]110
 *   'n' = 0b11[01]110
 *   't' = 0b11[10]100
 * These two bits can be used as an index into the g_json_literals array.
 */
/* [한국어] (c >> 3) & 3 으로 인덱싱하면 'f'→0, 'n'→1, 't'→2가 나오므로
 * 분기 한 번 없이 후보 리터럴을 O(1)로 선택할 수 있다. 4번째 빈 엔트리(인덱스 3)는
 * 위 트릭이 의도하지 않은 값을 만들었을 때(예: 다른 글자가 들어왔을 때) 안전하게 빈 슬롯에
 * 떨어지도록 두는 패딩이다 — 메인 파서가 case 't'/'f'/'n' 외에는 진입 자체를 안 하므로
 * 실질적으로는 도달 불가. */
static const struct json_literal g_json_literals[] = {
	{SPDK_JSON_VAL_FALSE, 5, "false"},  /* [한국어] index 0: 'f' */
	{SPDK_JSON_VAL_NULL,  4, "null"},   /* [한국어] index 1: 'n' */
	{SPDK_JSON_VAL_TRUE,  4, "true"},   /* [한국어] index 2: 't' */
	{}                                   /* [한국어] index 3: 빈 패딩(도달 시 len=0이 되어 자연스레 invalid) */
};

/*
 * [한국어]
 * match_literal - 입력 위치 [start, end) 가 literal[len]과 일치하는지 검증.
 *
 * @start: 매칭 시작 위치(파서가 't'/'f'/'n'을 본 위치).
 * @end: 입력 버퍼 종료.
 * @literal: 비교 대상(예: "true").
 * @len: literal의 길이.
 * @return: len(=토큰 길이, 매칭 성공) / SPDK_JSON_PARSE_INCOMPLETE / SPDK_JSON_PARSE_INVALID.
 *
 * 호출 체인: spdk_json_parse(case 't'/'f'/'n') → match_literal.
 */
static int
match_literal(const uint8_t *start, const uint8_t *end, const uint8_t *literal, size_t len)
{
	assert(end >= start);                              /* [한국어] 음수 길이 방어 */
	if ((size_t)(end - start) < len) {
		return SPDK_JSON_PARSE_INCOMPLETE;             /* [한국어] 리터럴 본문이 다 안 들어옴 — 추가 입력 가능성 */
	}

	if (memcmp(start, literal, len) != 0) {
		return SPDK_JSON_PARSE_INVALID;                /* [한국어] 길이는 충분하나 내용 불일치(예: "tru!e") */
	}

	return len;                                         /* [한국어] 정상 매치 — 토큰 길이를 그대로 반환 */
}

/*
 * [한국어]
 * spdk_json_parse - SPDK JSON 파서의 공개 진입점. byte buffer를 spdk_json_val 배열로 토큰화.
 *
 * @json: 파싱할 입력 버퍼 시작. (옵션) FLAG_DECODE_IN_PLACE 시 in-place로 escape 디코드되어 변경됨.
 * @size: 입력 버퍼 크기(바이트).
 * @values: 출력 토큰 배열. NULL 허용(이 경우 토큰 개수만 반환 — "dry-run" 사용처).
 * @num_values: values 배열 용량. 부족하면 토큰은 채우지 않지만 cur_value는 계속 증가하므로
 *              호출자는 반환값으로 필요한 용량을 알 수 있다(2-pass 사용 패턴).
 * @end: out — 파싱이 멈춘 위치. 정상 종료 시 마지막 whitespace까지의 끝 위치, 에러 시 에러 위치.
 * @flags: SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE / FLAG_ALLOW_COMMENTS 비트마스크.
 * @return: 채워진(또는 채워질) 토큰 개수, 또는 SPDK_JSON_PARSE_INVALID/_INCOMPLETE/_MAX_DEPTH_EXCEEDED.
 *
 * 동작: 단일 패스 state machine. 한 글자씩 읽으며 STATE_VALUE/STATE_NAME/_NAME_SEPARATOR/
 *       _VALUE_SEPARATOR/_END 사이를 천이한다. 객체/배열 진입 시 containers[depth]에 type을,
 *       con_value[depth]에 _BEGIN 토큰의 인덱스를 기록 → 매칭되는 _END에서 _BEGIN의 len을
 *       (자식 토큰 개수)로 패치해 트리 구조를 1차원 배열로 표현한다.
 *
 * In-place 부수효과: FLAG_DECODE_IN_PLACE이 켜지면 입력 버퍼의 escape가 풀려서 원본이 손상된다.
 *   호출자는 토큰의 start/len이 가리키는 데이터가 "디코드된 본문"임을 알아야 한다.
 *
 * 메모리: 동적 할당 없음. depth-limit 64로 자동변수 배열만 사용 → 재진입/취소 안전.
 *   본 함수는 호출자 스레드에서 동기 실행되며 SPDK 스레드 API를 사용하지 않는다.
 *
 * 호출 체인:
 *   상위: spdk_jsonrpc_parse_request, spdk_app_json_config_load, spdk_subsystem_init_from_json_config 등.
 *   하위: json_decode_string, json_valid_number, json_valid_comment, match_literal.
 */
ssize_t
spdk_json_parse(void *json, size_t size, struct spdk_json_val *values, size_t num_values,
		void **end, uint32_t flags)
{
	uint8_t *json_end = json + size;                                   /* [한국어] 입력 버퍼 sentinel */
	enum spdk_json_val_type containers[SPDK_JSON_MAX_NESTING_DEPTH];   /* [한국어] 현재 중첩된 컨테이너의 타입 스택(_OBJECT_BEGIN/_ARRAY_BEGIN) */
	size_t con_value[SPDK_JSON_MAX_NESTING_DEPTH];                     /* [한국어] 각 깊이의 _BEGIN 토큰이 values[] 내 어느 인덱스인지 — _END에서 len 패치할 때 필요 */
	enum spdk_json_val_type con_type = SPDK_JSON_VAL_INVALID;          /* [한국어] 가장 안쪽 컨테이너 타입 캐시(콤마 처리에서 사용) */
	bool trailing_comma = false;                                        /* [한국어] 마지막 토큰이 콤마였는지 — 닫기 전 콤마 금지 검출 */
	size_t depth = 0; /* index into containers */                       /* [한국어] 컨테이너 스택 깊이 */
	size_t cur_value = 0; /* index into values */                       /* [한국어] 다음에 채울 token 슬롯 인덱스 */
	size_t con_start_value;                                              /* [한국어] _END 처리 시 _BEGIN 토큰 인덱스 임시 변수 */
	uint8_t *data = json;                                                /* [한국어] 입력 진행 포인터 */
	uint8_t *new_data;                                                   /* [한국어] json_decode_string의 out 인자(다음 위치) */
	int rc = 0;                                                          /* [한국어] 하위 함수 반환값 임시 */
	const struct json_literal *lit;                                      /* [한국어] true/false/null 매칭에 사용할 literal 엔트리 포인터 */
	enum {
		STATE_VALUE, /* initial state */            /* [한국어] 값(또는 컨테이너 시작)을 기다리는 상태 */
		STATE_VALUE_SEPARATOR, /* value separator (comma) */ /* [한국어] 값 직후 — 콤마 또는 컨테이너 닫기 기대 */
		STATE_NAME, /* "name": value */             /* [한국어] 객체 안에서 키(이름) 문자열 기대 */
		STATE_NAME_SEPARATOR, /* colon */           /* [한국어] 키 직후 — ':' 기대 */
		STATE_END, /* parsed the complete value, so only whitespace is valid */ /* [한국어] 최상위 값 1개 끝 — whitespace만 허용 */
	} state = STATE_VALUE;                                               /* [한국어] 입력 시작은 항상 값(또는 컨테이너) 기대 상태 */

/* [한국어] 새 토큰을 values[]에 기록하는 헬퍼 매크로.
 *   - values가 NULL이거나 cur_value가 num_values를 넘었으면 기록은 생략하지만 cur_value는
 *     계속 증가시켜, 호출자가 "필요한 토큰 수"를 반환값으로 알 수 있게 한다(2-pass 패턴).
 *   - len은 byte 길이(컨테이너의 경우 문자 길이)이며, 컨테이너 _BEGIN의 경우 _END 처리 시
 *     자식 개수로 덮어쓴다. */
#define ADD_VALUE(t, val_start_ptr, val_end_ptr) \
	if (values && cur_value < num_values) { \
		values[cur_value].type = t; \
		values[cur_value].start = val_start_ptr; \
		values[cur_value].len = val_end_ptr - val_start_ptr; \
	} \
	cur_value++

	while (data < json_end) {                /* [한국어] 메인 토크나이저 루프 — 입력 끝까지 또는 STATE_END에 도달할 때까지 */
		uint8_t c = *data;                    /* [한국어] 1바이트 lookahead */

		switch (c) {
		case ' ':
		case '\t':
		case '\r':
		case '\n':
			/* Whitespace is allowed between any tokens. */
			data++;                           /* [한국어] 토큰 사이 whitespace는 무시 */
			break;

		case 't':
		case 'f':
		case 'n':
			/* true, false, or null */
			if (state != STATE_VALUE) { goto done_invalid; } /* [한국어] 리터럴은 값 자리에서만 허용 */
			lit = &g_json_literals[(c >> 3) & 3]; /* See comment above g_json_literals[] */ /* [한국어] 첫 글자 비트 트릭으로 후보 선택 */
			assert(lit->str[0] == c);             /* [한국어] 비트 트릭 검증: 선택된 엔트리의 첫 글자 일치 */
			rc = match_literal(data, json_end, lit->str, lit->len); /* [한국어] 본문 전체 비교 */
			if (rc < 0) { goto done_rc; }                            /* [한국어] INCOMPLETE/INVALID 전파 */
			ADD_VALUE(lit->type, data, data + rc);                   /* [한국어] 토큰 추가(_TRUE/_FALSE/_NULL) */
			data += rc;                                              /* [한국어] 입력 진행 */
			state = depth ? STATE_VALUE_SEPARATOR : STATE_END;       /* [한국어] 컨테이너 안: 콤마 기대 / 최상위: 종료 */
			trailing_comma = false;                                  /* [한국어] 콤마 다음 닫기 금지 플래그 해제 */
			break;

		case '"':
			if (state != STATE_VALUE && state != STATE_NAME) { goto done_invalid; } /* [한국어] 문자열은 값 또는 객체 키 자리에서만 */
			rc = json_decode_string(data, json_end, &new_data, flags); /* [한국어] 문자열 본문 스캔(+옵션 in-place 디코드) */
			if (rc < 0) {
				data = new_data;                                       /* [한국어] 에러 위치 보고용 */
				goto done_rc;
			}
			/*
			 * Start is data + 1 to skip initial quote.
			 * Length is data + rc - 1 to skip both quotes.
			 */
			ADD_VALUE(state == STATE_VALUE ? SPDK_JSON_VAL_STRING : SPDK_JSON_VAL_NAME,
				  data + 1, data + rc - 1);                            /* [한국어] 시작/끝 따옴표 제외한 본문만 토큰 범위로 기록 */
			data = new_data;                                           /* [한국어] 닫는 따옴표 다음으로 진행 */
			if (state == STATE_NAME) {
				state = STATE_NAME_SEPARATOR;                          /* [한국어] 키 다음에는 ':' 기대 */
			} else {
				state = depth ? STATE_VALUE_SEPARATOR : STATE_END;     /* [한국어] 값이었다면 콤마/종료 상태로 천이 */
			}
			trailing_comma = false;
			break;

		case '-':
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			if (state != STATE_VALUE) { goto done_invalid; }   /* [한국어] 숫자도 값 자리에서만 */
			rc = json_valid_number(data, json_end);            /* [한국어] DFA로 숫자 길이 측정 */
			if (rc < 0) { goto done_rc; }
			ADD_VALUE(SPDK_JSON_VAL_NUMBER, data, data + rc);  /* [한국어] 텍스트 그대로 토큰화 — 정수 변환은 디코더에서 수행 */
			data += rc;
			state = depth ? STATE_VALUE_SEPARATOR : STATE_END;
			trailing_comma = false;
			break;

		case '{':
		case '[':
			if (state != STATE_VALUE) { goto done_invalid; }   /* [한국어] 컨테이너 시작도 값 자리만 */
			if (depth == SPDK_JSON_MAX_NESTING_DEPTH) {
				rc = SPDK_JSON_PARSE_MAX_DEPTH_EXCEEDED;       /* [한국어] depth-bomb 방어 */
				goto done_rc;
			}
			if (c == '{') {
				con_type = SPDK_JSON_VAL_OBJECT_BEGIN;
				state = STATE_NAME;                            /* [한국어] 객체 안 첫 토큰은 키 또는 닫기 */
			} else {
				con_type = SPDK_JSON_VAL_ARRAY_BEGIN;
				state = STATE_VALUE;                            /* [한국어] 배열 안 첫 토큰은 값 또는 닫기 */
			}
			con_value[depth] = cur_value;                       /* [한국어] _BEGIN 토큰의 위치를 기억 — 닫을 때 len 패치할 곳 */
			containers[depth++] = con_type;                     /* [한국어] 컨테이너 스택에 push */
			ADD_VALUE(con_type, data, data + 1);                /* [한국어] _BEGIN 토큰 1바이트 길이로 일단 기록 */
			data++;
			trailing_comma = false;
			break;

		case '}':
		case ']':
			if (trailing_comma) { goto done_invalid; }          /* [한국어] {... ,} 처럼 콤마 후 즉시 닫기 금지 */
			if (depth == 0) { goto done_invalid; }              /* [한국어] 매칭되는 _BEGIN 없는 닫기 */
			con_type = containers[--depth];                     /* [한국어] 스택 pop — 이 닫기에 대응되는 _BEGIN의 타입 복원 */
			con_start_value = con_value[depth];                 /* [한국어] _BEGIN 토큰 인덱스 복원 */
			if (values && con_start_value < num_values) {
				values[con_start_value].len = cur_value - con_start_value - 1;
				/* [한국어] _BEGIN의 len 필드를 "내부 자식 토큰 개수"로 패치.
				 *   cur_value: _END를 추가하기 직전이므로 _BEGIN+자식들의 끝 인덱스.
				 *   -1: _BEGIN 자신 제외. _END는 아직 추가 안 했으므로 빠짐. */
			}
			if (c == '}') {
				if (state != STATE_NAME && state != STATE_VALUE_SEPARATOR) {
					goto done_invalid;                          /* [한국어] '}'은 빈 객체 직후(STATE_NAME) 또는 마지막 값 직후(STATE_VALUE_SEPARATOR)만 허용 */
				}
				if (con_type != SPDK_JSON_VAL_OBJECT_BEGIN) {
					goto done_invalid;                          /* [한국어] [...} 같은 짝 불일치 */
				}
				ADD_VALUE(SPDK_JSON_VAL_OBJECT_END, data, data + 1);
			} else {
				if (state != STATE_VALUE && state != STATE_VALUE_SEPARATOR) {
					goto done_invalid;                          /* [한국어] ']'은 빈 배열 직후(STATE_VALUE) 또는 마지막 값 직후만 */
				}
				if (con_type != SPDK_JSON_VAL_ARRAY_BEGIN) {
					goto done_invalid;
				}
				ADD_VALUE(SPDK_JSON_VAL_ARRAY_END, data, data + 1);
			}
			con_type = depth == 0 ? SPDK_JSON_VAL_INVALID : containers[depth - 1]; /* [한국어] 한 단계 위 컨테이너 캐시 갱신 */
			data++;
			state = depth ? STATE_VALUE_SEPARATOR : STATE_END;
			trailing_comma = false;
			break;

		case ',':
			if (state != STATE_VALUE_SEPARATOR) { goto done_invalid; } /* [한국어] 콤마는 값 직후만 허용 */
			data++;
			assert(con_type == SPDK_JSON_VAL_ARRAY_BEGIN ||
			       con_type == SPDK_JSON_VAL_OBJECT_BEGIN);              /* [한국어] depth>0이 보장되어야 콤마가 합법 */
			state = con_type == SPDK_JSON_VAL_ARRAY_BEGIN ? STATE_VALUE : STATE_NAME; /* [한국어] 다음 토큰: 배열이면 값, 객체면 키 */
			trailing_comma = true;                                        /* [한국어] 다음에 닫기 토큰이 오면 무효 */
			break;

		case ':':
			if (state != STATE_NAME_SEPARATOR) { goto done_invalid; } /* [한국어] 콜론은 키 직후만 */
			data++;
			state = STATE_VALUE;                                       /* [한국어] 콜론 다음은 값 */
			break;

		case '/':
			if (!(flags & SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS)) {
				goto done_invalid;                                     /* [한국어] 표준 JSON에는 주석 없음 — 옵션 미설정 시 거부 */
			}
			rc = json_valid_comment(data, json_end);
			if (rc < 0) { goto done_rc; }
			/* Skip over comment */
			data += rc;                                                 /* [한국어] 주석은 토큰화하지 않고 단순 스킵 */
			break;

		default:
			goto done_invalid;                                          /* [한국어] 어떤 case에도 해당 없는 바이트 — 무효 */
		}

		if (state == STATE_END) {
			break;                                                       /* [한국어] 최상위 단일 값 파싱 완료 — 더 토큰화하지 않고 trailing whitespace 처리로 */
		}
	}

	if (state == STATE_END) {
		/* Skip trailing whitespace */
		while (data < json_end) {
			uint8_t c = *data;

			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
				data++;                                /* [한국어] 최상위 값 종료 후 trailing whitespace는 그냥 스킵 */
			} else {
				break;                                  /* [한국어] non-whitespace가 등장해도 거부하지 않음 — 호출자가 *end로 위치를 받아 후속 처리 가능(스트리밍) */
			}
		}

		/*
		 * These asserts are just for sanity checking - they are guaranteed by the allowed
		 *  state transitions.
		 */
		assert(depth == 0);                  /* [한국어] STATE_END 도달 시 모든 컨테이너가 닫힘 */
		assert(trailing_comma == false);     /* [한국어] STATE_END에서는 trailing comma 없음 */
		assert(data <= json_end);
		if (end) {
			*end = data;                      /* [한국어] 호출자에게 다음 입력 시작 위치 보고 */
		}
		return cur_value;                     /* [한국어] 파싱된(또는 파싱되었을) 토큰 개수 */
	}

	/* Invalid end state - ran out of data */
	rc = SPDK_JSON_PARSE_INCOMPLETE;          /* [한국어] state machine이 최종 상태 아닌 채로 EOF — 추가 입력 가능성 */

done_rc:
	assert(rc < 0);                            /* [한국어] 음수 에러 코드만 여기로 진입 */
	if (end) {
		*end = data;                           /* [한국어] 에러 위치 보고 */
	}
	return rc;

done_invalid:
	rc = SPDK_JSON_PARSE_INVALID;              /* [한국어] state machine이 명시적으로 거부한 케이스 */
	goto done_rc;
}
