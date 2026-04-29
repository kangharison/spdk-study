/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * JSON parsing and encoding
 */

/*
 * [한국어 설명] SPDK zero-copy JSON 파서 + streaming writer 공개 API (json.h) — 약 353 라인
 *
 * === 파일의 역할 ===
 * SPDK 전체에서 공유하는 JSON 처리 라이브러리의 공개 인터페이스를 정의한다.
 * 두 개의 큰 축으로 구성된다:
 *   (1) Zero-copy 파서 — spdk_json_parse(buf, size, vals[], ...)는 입력 버퍼를
 *       복사하지 않고 각 토큰의 시작 위치(start)와 길이(len)만 spdk_json_val
 *       배열에 인덱싱한다. 문자열 디코딩은 옵션(SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE)에
 *       따라 입력 버퍼 자체를 in-place로 수정해서 수행한다.
 *   (2) Streaming writer — spdk_json_write_ctx 컨텍스트를 만들고 임의의 user
 *       콜백(write_cb)으로 JSON을 한 토큰씩 흘려보낸다. begin/end_object,
 *       array, named_* 헬퍼들이 적절히 콤마/콜론을 자동 삽입해 RFC 8259 호환
 *       JSON을 생성한다.
 * 추가로 decode helper 패밀리(spdk_json_decode_object/array/bool/intN/uintN/
 * string/uuid)와 number→정수 변환기, 객체 내부 검색기(spdk_json_find/
 * find_string/find_array, object_first/array_first/next)가 함께 제공된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 JSON-RPC 서버/클라이언트(spdk/jsonrpc.h, spdk/rpc.h), config 파일
 * 처리(lib/init/subsystem.c의 framework load/save config), bdev/nvme 등
 * 각 서브시스템의 RPC 핸들러가 본 헤더의 파서/writer를 사용한다.
 * 호출 흐름:
 *   소켓 수신 byte stream → spdk_jsonrpc_parse_request → spdk_json_parse →
 *   spdk_json_val[] 배열 → spdk_json_decode_object(decoders) → C struct →
 *   RPC 핸들러 → spdk_jsonrpc_begin_result(=spdk_json_write_ctx) →
 *   spdk_json_write_named_* → write_cb → 소켓 송신
 * 즉 SPDK가 "사람·도구와 통신할 때" 거의 모든 경로가 이 파일을 거친다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/stdinc.h(표준 타입 ssize_t/uint*_t/bool/va_list/FILE),
 *             spdk/uuid.h(spdk_uuid 타입 — UUID 디코드/인코드용).
 * 의존(구현): lib/json/json_parse.c, lib/json/json_write.c, lib/json/json_util.c가
 *             본 API들의 실체를 제공. C++ 바인딩이 필요할 수 있어 extern "C" 처리.
 * 데이터 흐름: 입력 byte buffer → spdk_json_val 배열(인덱싱) → 사용자 디코드/검색 →
 *             C 자료구조. 출력 경로는 C 값 → spdk_json_write_ctx → write_cb →
 *             사용자 sink(소켓/파일/메모리).
 * 공유 자료구조: 모든 token이 spdk_json_val의 (start,len,type) 트리플로 표현된다.
 *             writer는 spdk_json_write_ctx(불투명) 내부에 임시 버퍼와 콤마/콜론
 *             상태머신을 보관한다. 호출자는 컨텍스트 외부 상태를 공유하지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 *   - enum spdk_json_val_type: 비트 플래그형 — INVALID/NULL/TRUE/FALSE/NUMBER/STRING/
 *     ARRAY_BEGIN/ARRAY_END/OBJECT_BEGIN/OBJECT_END/NAME. SPDK_JSON_VAL_ANY는
 *     검색용 wildcard로 INVALID와 동일 값(0).
 *   - struct spdk_json_val: 토큰 1개를 표현 — start(원본 buf 내 위치) + len(바이트 또는
 *     하위 토큰 개수) + type. zero-copy의 핵심.
 *   - struct spdk_json_object_decoder: name/offset/decode_func/optional 4-tuple로
 *     C struct 필드와 JSON key를 매핑.
 *   - spdk_json_parse: 입력을 파싱해 spdk_json_val 배열을 채움. 반환값은 토큰 개수
 *     혹은 SPDK_JSON_PARSE_INVALID/_INCOMPLETE/_MAX_DEPTH_EXCEEDED.
 *   - spdk_json_decode_object/_relaxed: 디코더 표 기반 객체→C struct 매핑.
 *   - spdk_json_decode_array: 배열 → C 배열 매핑(stride로 element 간격 지정).
 *   - spdk_json_decode_<type>: bool/uint8/uint16/int32/uint32/uint64/string/uuid 변환기.
 *   - spdk_json_number_to_<type>: NUMBER 토큰의 텍스트를 정수로 변환.
 *   - spdk_json_val_len: 토큰 1개의 "건너뛸 길이" — 객체/배열은 _BEGIN+내용+_END 합산.
 *   - spdk_json_strequal/strdup: 토큰의 문자열 비교/복제.
 *   - spdk_json_write_begin/end/reset: writer lifecycle.
 *   - spdk_json_write_<type>: null/bool/int/uint/double/string/bytearray/uuid 출력.
 *   - spdk_json_write_array_begin/end, object_begin/end, name/name_raw: 컨테이너/키 출력.
 *   - spdk_json_write_named_<type>: name + value 단축형(객체 안에서만 사용).
 *   - spdk_json_find/find_string/find_array: 객체 내 1단계 키 검색.
 *   - spdk_json_object_first/array_first/next: 컨테이너 순회 iterator.
 */

#ifndef SPDK_JSON_H_       /* [한국어] include 가드 — 다중 포함 방지 */
#define SPDK_JSON_H_

#include "spdk/stdinc.h"   /* [한국어] 표준 타입 묶음(ssize_t/uint*_t/size_t/bool/va_list/FILE 등) */
#include "spdk/uuid.h"     /* [한국어] struct spdk_uuid — UUID 디코드/인코드 헬퍼 시그니처에서 사용 */

#ifdef __cplusplus
extern "C" {               /* [한국어] C++ 컴파일러에서도 본 헤더를 그대로 쓰도록 C 링키지 강제 */
#endif

/*
 * [한국어] JSON 토큰 타입 비트 플래그.
 *
 * 비트 플래그로 정의된 이유: spdk_json_find()류 검색 함수가 "특정 타입(들)만
 * 매치"하도록 OR 결합한 마스크를 받기 때문. 예) (SPDK_JSON_VAL_STRING |
 * SPDK_JSON_VAL_NUMBER)를 넘기면 둘 중 하나에 해당하는 키만 매치된다.
 *
 * INVALID(=0)과 SPDK_JSON_VAL_ANY가 같은 값을 갖는 트릭: 검색 시 "타입 무관"을
 * 표현할 때 0(=INVALID 비트가 켜진 상태)을 wildcard로 재해석한다.
 *
 * 컨테이너(객체/배열)는 _BEGIN과 _END 두 토큰으로 표현되어, 파서가 출력하는
 * spdk_json_val 배열에서 한 컨테이너의 자식들은 [_BEGIN][자식들...][_ END] 형태로
 * 연속해서 나타난다. NAME은 객체의 키(따옴표 문자열) 토큰에 부여된다 — 동일한
 * 문자열 토큰이라도 객체 키 위치라면 STRING이 아니라 NAME 타입을 받는다.
 */
enum spdk_json_val_type {
	SPDK_JSON_VAL_INVALID = 0,
	/* [한국어] 무효/미초기화 토큰. 파서 내부 sentinel 또는 검색 wildcard로 재사용된다.
	 * 설정자: 파서가 배열을 0으로 초기화한 상태 또는 명시적으로 검색 호출자가 ANY로 사용.
	 * 읽는 자: spdk_json_find()가 타입 마스크와 비교, 매치되면 모든 타입 허용.
	 * 값 범위: 정수 0. 다른 비트 플래그와 겹치지 않도록 0번 비트는 의도적으로 비워둠. */
#define SPDK_JSON_VAL_ANY SPDK_JSON_VAL_INVALID
	/* [한국어] 검색용 wildcard — INVALID와 같은 0 값을 갖지만, 의미를 명시하기 위한 별칭.
	 * 사용 예: spdk_json_find(obj, "key", &k, &v, SPDK_JSON_VAL_ANY) — 타입 무관 매치. */
	SPDK_JSON_VAL_NULL = 1U << 1,
	/* [한국어] JSON null 리터럴. start는 'n'을 가리키고 len=4(="null").
	 * 설정자: 파서가 'null' 키워드를 만나면 부여.
	 * 읽는 자: 디코더가 null 처리(보통 포인터를 NULL로) 또는 RPC에서 명시적 null로 전달. */
	SPDK_JSON_VAL_TRUE = 1U << 2,
	/* [한국어] JSON true 리터럴. start는 't', len=4. spdk_json_decode_bool이 *out=true로 변환. */
	SPDK_JSON_VAL_FALSE = 1U << 3,
	/* [한국어] JSON false 리터럴. start는 'f', len=5. spdk_json_decode_bool이 *out=false로 변환. */
	SPDK_JSON_VAL_NUMBER = 1U << 4,
	/* [한국어] JSON number(정수/소수/지수). start/len은 원본 텍스트 영역을 가리킨다.
	 * 숫자 변환은 spdk_json_number_to_uint8/16/uint32/uint64/int32 등으로 별도 수행.
	 * (파서 단계에서는 텍스트 그대로 두어 정밀도 손실/타입 결정을 호출자에게 위임) */
	SPDK_JSON_VAL_STRING = 1U << 5,
	/* [한국어] JSON 문자열(따옴표 제외 본문). DECODE_IN_PLACE 플래그가 켜졌으면 escape가
	 * 풀린 UTF-8로 in-place 디코드되어 있고 len도 디코드 후 길이.
	 * 설정자: 파서. 읽는 자: spdk_json_decode_string/strdup/strequal. */
	SPDK_JSON_VAL_ARRAY_BEGIN = 1U << 6,
	/* [한국어] JSON 배열 시작 '['. start는 '['를 가리키고, len은 _BEGIN 다음에 오는
	 * 토큰들의 개수(중첩 컨테이너 포함, _END는 미포함). 즉 _BEGIN + len + 1 위치에 _END가 있다.
	 * 이 규칙으로 컨테이너 전체를 O(1)에 건너뛸 수 있다(spdk_json_val_len 참고). */
	SPDK_JSON_VAL_ARRAY_END = 1U << 7,
	/* [한국어] JSON 배열 종료 ']'. start는 ']', len=1. 보통 호출자는 직접 참조하지 않고
	 * iterator(spdk_json_next)나 길이 계산에서만 사용. */
	SPDK_JSON_VAL_OBJECT_BEGIN = 1U << 8,
	/* [한국어] JSON 객체 시작 '{'. ARRAY_BEGIN과 같은 규칙(len = 자식 토큰 수, _END 미포함).
	 * 객체 자식은 NAME, value, NAME, value, ... 패턴으로 짝수 개. */
	SPDK_JSON_VAL_OBJECT_END = 1U << 9,
	/* [한국어] JSON 객체 종료 '}'. start='}', len=1. */
	SPDK_JSON_VAL_NAME = 1U << 10,
	/* [한국어] 객체의 키 문자열. 의미는 STRING과 같지만 위치(키 슬롯)가 달라 구분된다.
	 * spdk_json_decode_object()가 이 NAME 토큰을 보고 디코더 표의 name과 strequal로 매치. */
};

/*
 * [한국어] 파싱된 JSON 토큰 1개를 표현하는 zero-copy descriptor.
 *
 * 파서는 입력 byte buffer를 절대 복사하지 않고, 각 토큰의 (시작 포인터, 길이,
 * 타입) 트리플만 spdk_json_val 배열에 채운다. 따라서 입력 버퍼는 파싱이 끝난
 * 뒤에도 spdk_json_val을 사용하는 동안 유효해야 한다(buffer lifetime 의존).
 *
 * 컨테이너(_BEGIN) 토큰의 len은 "건너뛸 자식 토큰 수"로 재해석된다 — bytes가
 * 아니라 인덱스 거리. 이 덕분에 한 컨테이너 전체를 한 번에 스킵하거나 짝이 되는
 * _END를 O(1)에 찾을 수 있다.
 */
struct spdk_json_val {
	/**
	 * Pointer to the location of the value within the parsed JSON input.
	 *
	 * For SPDK_JSON_VAL_STRING and SPDK_JSON_VAL_NAME,
	 *  this points to the beginning of the decoded UTF-8 string without quotes.
	 *
	 * For SPDK_JSON_VAL_NUMBER, this points to the beginning of the number as represented in
	 *  the original JSON (text representation, not converted to a numeric value).
	 *
	 * For JSON objects and arrays, this points to their beginning and has a type
	 *  set to SPDK_JSON_VAL_OBJECT_BEGIN or SPDK_JSON_VAL_ARRAY_BEGIN respectively.
	 */
	void *start;
	/* [한국어] 토큰의 원본 버퍼 내 시작 위치(파서가 입력 buffer 내부를 가리키는 raw 포인터).
	 * 설정자: spdk_json_parse(). 토큰 종류에 따라 가리키는 대상이 다르다 —
	 *   - STRING/NAME: 따옴표를 제외한 본문 첫 바이트. DECODE_IN_PLACE 플래그가 켜져 있으면
	 *     escape가 풀린 UTF-8 버퍼 시작. 꺼져 있으면 raw JSON 그대로(escape 포함).
	 *   - NUMBER: 숫자 텍스트 첫 바이트('-' 또는 숫자). 변환 전 원시 텍스트.
	 *   - 컨테이너 _BEGIN: '['/'{' 문자 위치.
	 *   - NULL/TRUE/FALSE: 키워드 첫 글자.
	 * 읽는 자: 디코더, 검색기, writer 등 모든 소비자.
	 * 값 범위: 입력 buffer 내부 임의 위치(NULL 가능 — INVALID 토큰).
	 * 동기화: 파싱 결과는 단일 스레드에서만 다뤄진다(파서/디코더가 모두 동기 함수). */

	/**
	 * Length of value.
	 *
	 * For SPDK_JSON_VAL_STRING, SPDK_JSON_VAL_NUMBER, and SPDK_JSON_VAL_NAME,
	 *  this is the length in bytes of the value starting at \ref start.
	 *
	 * For SPDK_JSON_VAL_ARRAY_BEGIN and SPDK_JSON_VAL_OBJECT_BEGIN,
	 *  this is the number of values contained within the array or object (including
	 *  nested objects and arrays, but not including the _END value). The array or object _END
	 *  value can be found by advancing len values from the _BEGIN value.
	 */
	uint32_t len;
	/* [한국어] 토큰 길이 — 단위는 토큰 종류에 따라 달라진다(중요!).
	 *   - STRING/NAME/NUMBER: 바이트 수. start[0..len-1]이 실제 본문.
	 *   - 컨테이너 _BEGIN: "이 컨테이너에 포함된 토큰의 개수"(_END는 제외, 중첩 컨테이너의
	 *     자식 토큰까지 모두 포함). 즉 _BEGIN 인덱스 + len + 1 위치에 짝이 되는 _END가 있다.
	 *   - 컨테이너 _END/NULL/TRUE/FALSE: 키워드 길이(예: TRUE=4, FALSE=5, NULL=4, ARRAY_END=1).
	 * 설정자: 파서. 읽는 자: spdk_json_val_len(컨테이너 스킵), 디코더, 검색기. */

	/**
	 * Type of value.
	 */
	enum spdk_json_val_type type;
	/* [한국어] 토큰 타입. enum spdk_json_val_type 중 정확히 한 비트가 켜진다(= 단일 값).
	 * INVALID(0)는 파서가 미사용 슬롯에 둘 수 있는 sentinel 값이기도 하다.
	 * 설정자: 파서. 읽는 자: 모든 소비자. */
};

/**
 * Invalid JSON syntax.
 */
#define SPDK_JSON_PARSE_INVALID			-1
/* [한국어] spdk_json_parse() 반환 에러 — 입력이 JSON 문법에 맞지 않음(예: 잘못된 토큰).
 * RPC 서버에서 이 코드를 받으면 JSON-RPC error -32700(parse error)를 응답한다. */

/**
 * JSON was valid up to the end of the current buffer, but did not represent a complete JSON value.
 */
#define SPDK_JSON_PARSE_INCOMPLETE		-2
/* [한국어] spdk_json_parse() 반환 — 지금까지는 valid이지만 끝까지 다 못 받았음.
 * 스트리밍 입력(소켓)에서 자주 발생: 더 받아서 buffer를 키우고 재시도하는 신호. */

#define SPDK_JSON_PARSE_MAX_DEPTH_EXCEEDED	-3
/* [한국어] 중첩 깊이 한계 초과. 파서 내부 재귀(또는 명시적 stack) depth 한계를 넘김.
 * 악의적으로 깊은 객체로 stack 폭발을 일으키는 입력을 거부하는 안전 장치. */

/**
 * Decode JSON strings and names in place (modify the input buffer).
 */
#define SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE	0x000000001
/* [한국어] spdk_json_parse() 플래그 — STRING/NAME 토큰의 escape를 입력 buffer 내부에서
 * 풀어버림(\n → 0x0A 등). 이후 spdk_json_val.start는 escape가 풀린 UTF-8 본문을 가리킨다.
 * 입력 buffer가 read-only이거나 원본을 보존하고 싶다면 이 플래그를 켜면 안 된다. */

/**
 * Allow parsing of comments.
 *
 * Comments are not allowed by the JSON RFC, so this is not enabled by default.
 */
#define SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS	0x000000002
/* [한국어] spdk_json_parse() 플래그 — '//', '/* ... *​/' 주석 허용. RFC 8259는 주석을
 * 금지하지만, 사용자가 손으로 작성하는 SPDK config 파일(JSON5스러움)을 위해 옵션 제공. */

/*
 * Parse JSON data.
 *
 * \param data Raw JSON data; must be encoded in UTF-8.
 * Note that the data may be modified to perform in-place string decoding.
 *
 * \param size Size of data in bytes.
 *
 * \param end If non-NULL, this will be filled a pointer to the byte just beyond the end
 * of the valid JSON.
 *
 * \return Number of values parsed, or negative on failure:
 * SPDK_JSON_PARSE_INVALID if the provided data was not valid JSON, or
 * SPDK_JSON_PARSE_INCOMPLETE if the provided data was not a complete JSON value.
 */
/*
 * [한국어]
 * spdk_json_parse - zero-copy JSON 파서 진입점.
 *
 * @json:       UTF-8 JSON byte buffer. DECODE_IN_PLACE 플래그가 켜져 있으면 본 함수가
 *              입력을 in-place로 수정한다(escape 디코딩).
 * @size:       json 버퍼의 바이트 길이.
 * @values:     결과 토큰을 받을 spdk_json_val 배열(호출자 소유). NULL이면 "필요 토큰 수
 *              계산만"하는 dry-run 모드로 동작 — 반환값으로 토큰 수를 받아 적절한 크기로
 *              재할당 후 다시 호출하는 패턴이 가능.
 * @num_values: values 배열의 capacity(슬롯 수). 부족하면 SPDK_JSON_PARSE_INVALID 또는
 *              부분 결과 + 음수 코드를 반환.
 * @end:        성공 시 (비-NULL이면) 마지막으로 소비한 바이트 다음 위치를 채운다.
 *              스트리밍 파서가 다음 메시지의 시작점을 알기 위해 사용.
 * @flags:      SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE / _ALLOW_COMMENTS의 OR 조합.
 * @return:     성공 시 채워진 토큰 개수(_END 포함). 실패 시 SPDK_JSON_PARSE_INVALID,
 *              _INCOMPLETE, _MAX_DEPTH_EXCEEDED 중 하나.
 *
 * RPC 서버는 이 함수를 소켓 수신 buffer에 반복 호출 — _INCOMPLETE면 더 받아서 재시도,
 * 성공하면 해당 메시지를 처리하고 end 포인터를 기준으로 buffer를 컴팩션.
 *
 * 호출 체인:
 *   spdk_jsonrpc_parse_request → spdk_json_parse → 파서 내부 토크나이저
 */
ssize_t spdk_json_parse(void *json, size_t size, struct spdk_json_val *values, size_t num_values,
			void **end, uint32_t flags);

/*
 * [한국어] 단일 JSON 값을 C 자료형으로 변환하는 디코더 콜백 타입.
 * @val: 디코딩 대상 토큰(보통 spdk_json_object_decoder 표에서 매칭된 value 토큰).
 * @out: 변환 결과를 쓸 메모리(spdk_json_decode_object()가 base + offset으로 계산해 전달).
 * @return: 0 성공, 음수 실패. 실패 시 spdk_json_decode_object 전체가 -1로 단락된다.
 * 사용 예: spdk_json_decode_uint32, spdk_json_decode_string, 사용자 정의 enum 디코더 등. */
typedef int (*spdk_json_decode_fn)(const struct spdk_json_val *val, void *out);

/*
 * [한국어] JSON 객체 → C struct 매핑 표 한 행.
 *
 * 사용 패턴:
 *   static const struct spdk_json_object_decoder my_decoders[] = {
 *     {"name",   offsetof(struct my, name),   spdk_json_decode_string, false},
 *     {"size",   offsetof(struct my, size),   spdk_json_decode_uint64, false},
 *     {"opt",    offsetof(struct my, opt),    spdk_json_decode_bool,   true},
 *   };
 *   spdk_json_decode_object(values, my_decoders, SPDK_COUNTOF(my_decoders), &my);
 */
struct spdk_json_object_decoder {
	const char *name;
	/* [한국어] 매핑할 JSON 객체 키 이름(NUL-종결 C 문자열).
	 * 설정자: RPC 핸들러가 정적 표를 작성할 때.
	 * 읽는 자: spdk_json_decode_object()가 객체의 NAME 토큰들과 strequal로 비교.
	 * 값 범위: 표 lifetime 동안 유효한 정적/장기 lifetime 문자열(보통 string literal). */

	size_t offset;
	/* [한국어] out 구조체 시작점에서의 필드 오프셋(보통 offsetof 매크로 사용).
	 * 디코더는 (char *)out + offset 위치에 결과를 쓴다.
	 * 설정자: 표 작성자. 읽는 자: spdk_json_decode_object 내부. */

	spdk_json_decode_fn decode_func;
	/* [한국어] 이 키의 값을 C 표현으로 변환할 디코더(spdk_json_decode_<type> 또는 사용자 정의).
	 * 설정자: 표 작성자. 읽는 자: spdk_json_decode_object가 매칭된 value 토큰과 함께 호출.
	 * NULL은 허용되지 않음(디코더 없는 매핑은 의미 없음). */

	bool optional;
	/* [한국어] true면 입력에 키가 없어도 성공으로 간주, false면 누락 시 -EINVAL.
	 * RPC 핸들러가 "필수/선택 파라미터"를 표현하는 표준 방법.
	 * 설정자: 표 작성자. 읽는 자: spdk_json_decode_object의 누락 검사. */
};

/*
 * [한국어]
 * spdk_json_decode_object - JSON 객체를 디코더 표에 따라 C struct로 변환(strict).
 *
 * @values:       OBJECT_BEGIN 토큰을 가리키는 spdk_json_val 포인터(보통 spdk_json_parse 결과).
 * @decoders:     키-필드 매핑 표(상기 spdk_json_object_decoder 배열).
 * @num_decoders: 표의 행 수(SPDK_COUNTOF(decoders)).
 * @out:          결과를 받을 C struct base 포인터(필드는 base + decoder.offset에 쓰임).
 * @return:       0 성공, -1 실패. 실패 케이스: (1) 표에 없는 키가 객체에 존재(strict — RPC
 *                파라미터 검증에서 typo 잡기 위해), (2) optional=false 키 누락,
 *                (3) 어떤 decode_func가 음수 반환, (4) values가 OBJECT_BEGIN 아님.
 *
 * RPC 핸들러의 기본 진입 패턴: 받은 params(JSON 객체)를 이 함수로 핸들러 로컬 struct에 풀고,
 * 검증/처리 후 응답을 작성. 호출 컨텍스트는 RPC 처리 스레드(보통 SPDK app thread).
 *
 * 호출 체인: rpc_handler → spdk_json_decode_object → 각 decode_func
 */
int spdk_json_decode_object(const struct spdk_json_val *values,
			    const struct spdk_json_object_decoder *decoders, size_t num_decoders, void *out);

/*
 * [한국어]
 * spdk_json_decode_object_relaxed - decode_object의 relaxed 버전.
 *
 * @values/@decoders/@num_decoders/@out: spdk_json_decode_object와 동일.
 * @return: 0 성공, -1 실패. strict 버전과의 차이는 표에 없는 키가 있어도 무시(에러 아님).
 *
 * 사용 예: 미래 호환성을 위해 클라이언트가 새 필드를 추가해도 서버가 거부하지 않게 하고
 * 싶을 때, 또는 일부 메타데이터를 무시하고 알려진 키만 추출하고 싶을 때.
 */
int spdk_json_decode_object_relaxed(const struct spdk_json_val *values,
				    const struct spdk_json_object_decoder *decoders, size_t num_decoders, void *out);

/**
 * Decode a JSON array.
 *
 * \param values List of values to decode.
 * \param decode_func Function to use to decode each individual value.
 * \param out Buffer to store decoded value(s).  If `stride` != 0, this buffer is advanced `stride`
 *            bytes for each decoded value.
 * \param out_size Number of decoded values.
 * \param max_size Maximum number of array elements to decode.
 * \param stride Number of bytes to advance `out`.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_json_decode_array - JSON 배열의 각 원소를 동일 디코더로 풀어 C 배열로 변환.
 *
 * @values:      ARRAY_BEGIN 토큰.
 * @decode_func: 원소 1개를 디코드하는 콜백(예: spdk_json_decode_string).
 * @out:         첫 원소 출력 위치. stride만큼 전진하며 각 원소를 쓴다.
 * @max_size:    out 버퍼가 받을 수 있는 최대 원소 수(buffer overflow 방지).
 * @out_size:    실제 디코드된 원소 수를 채워 반환.
 * @stride:      out을 다음 원소 위치로 이동시킬 바이트 수(보통 sizeof(원소 타입)).
 * @return:      0 성공, -1 실패(원소 수 초과, decode_func 실패 등).
 *
 * RPC 핸들러에서 string array, uint32 array 등을 받을 때 표준 패턴.
 */
int spdk_json_decode_array(const struct spdk_json_val *values, spdk_json_decode_fn decode_func,
			   void *out, size_t max_size, size_t *out_size, size_t stride);

/*
 * [한국어] 단일 토큰 → C 자료형 디코더 패밀리. 모두 spdk_json_decode_fn 시그니처를 따른다.
 * @val: 디코드할 토큰. @out: 결과를 쓸 메모리(타입에 맞는 크기). @return: 0 성공, -1 실패.
 * 주의: out의 타입은 함수마다 다르다 — uint8용은 uint8_t*, string용은 char**(strdup 후 채움)
 * 등. spdk_json_decode_object의 decoder 표에 그대로 사용한다.
 */
int spdk_json_decode_bool(const struct spdk_json_val *val, void *out);
/* [한국어] TRUE/FALSE 토큰을 *bool에 저장. 다른 타입이면 -1. */
int spdk_json_decode_uint8(const struct spdk_json_val *val, void *out);
/* [한국어] NUMBER 토큰을 uint8_t에 저장. 0~255 범위 초과 시 -1. */
int spdk_json_decode_uint16(const struct spdk_json_val *val, void *out);
/* [한국어] NUMBER 토큰을 uint16_t에 저장. 범위 초과/타입 불일치 시 -1. */
int spdk_json_decode_int32(const struct spdk_json_val *val, void *out);
/* [한국어] NUMBER 토큰을 int32_t에 저장(음수 허용). 범위 초과 시 -1. */
int spdk_json_decode_uint32(const struct spdk_json_val *val, void *out);
/* [한국어] NUMBER 토큰을 uint32_t에 저장. 범위 초과/음수 시 -1. */
int spdk_json_decode_uint64(const struct spdk_json_val *val, void *out);
/* [한국어] NUMBER 토큰을 uint64_t에 저장. 범위 초과/음수 시 -1. */
int spdk_json_decode_string(const struct spdk_json_val *val, void *out);
/* [한국어] STRING 토큰을 strdup해서 *(char **)out에 저장. 호출자가 free() 책임.
 * spdk_json_free_object()가 자동으로 free 해주는 메모리 짝(소유권 이전)이다. */
int spdk_json_decode_uuid(const struct spdk_json_val *val, void *out);
/* [한국어] STRING 토큰("xxxxxxxx-xxxx-...")을 spdk_uuid 바이너리 표현으로 디코드. */

/*
 * [한국어]
 * spdk_json_free_object - decode_object로 strdup된 문자열 등 동적 메모리를 일괄 해제.
 *
 * @decoders:     디코드에 사용했던 표(메모리 소유권을 가진 디코더 식별용).
 * @num_decoders: 표 크기.
 * @obj:          spdk_json_decode_object의 out으로 전달했던 base 포인터.
 *
 * spdk_json_decode_string 같은 디코더가 strdup한 메모리를 모두 해제한다. RPC 핸들러
 * 종료 시 누수 없이 정리하는 표준 짝. obj 자체는 해제하지 않는다(스택/외부 소유).
 */
void spdk_json_free_object(const struct spdk_json_object_decoder *decoders, size_t num_decoders,
			   void *obj);

/**
 * Get length of a value in number of values.
 *
 * This can be used to skip over a value while interpreting parse results.
 *
 * For SPDK_JSON_VAL_ARRAY_BEGIN and SPDK_JSON_VAL_OBJECT_BEGIN,
 *  this returns the number of values contained within this value, plus the _BEGIN and _END values.
 *
 * For all other values, this returns 1.
 */
/*
 * [한국어]
 * spdk_json_val_len - 토큰 1개를 "건너뛸 때" 이동해야 하는 토큰 수 계산.
 *
 * @val:    대상 토큰.
 * @return: 단순 토큰은 1, 컨테이너(_BEGIN)는 (자식 수 + 2) — _BEGIN과 _END까지 포함.
 *          호출자는 cur_val += spdk_json_val_len(cur_val)로 다음 sibling 토큰으로 이동.
 *
 * 사용 예: 객체를 직접 순회하며 모르는 키는 통째로 건너뛸 때.
 */
size_t spdk_json_val_len(const struct spdk_json_val *val);

/**
 * Compare JSON string with null terminated C string.
 *
 * \return true if strings are equal or false if not
 */
/*
 * [한국어]
 * spdk_json_strequal - JSON STRING/NAME 토큰과 NUL-종결 C 문자열을 비교.
 *
 * @val: STRING 또는 NAME 타입 토큰. 다른 타입이면 무조건 false.
 * @str: 비교 대상 C 문자열.
 * @return: 길이와 바이트가 모두 같으면 true, 아니면 false.
 *
 * spdk_json_decode_object 내부에서 객체 키와 디코더 표 name을 매치할 때 사용.
 */
bool spdk_json_strequal(const struct spdk_json_val *val, const char *str);

/**
 * Equivalent of strdup() for JSON string values.
 *
 * If val is not representable as a C string (contains embedded '\0' characters),
 * returns NULL.
 *
 * Caller is responsible for passing the result to free() when it is no longer needed.
 */
/*
 * [한국어]
 * spdk_json_strdup - STRING/NAME 토큰을 새 malloc된 NUL-종결 C 문자열로 복제.
 *
 * @val:    STRING 또는 NAME 토큰.
 * @return: malloc된 NUL-종결 문자열 또는 NULL(메모리 부족, 본문에 임베디드 \0 포함, 타입 오류).
 *          호출자가 free() 호출 책임.
 */
char *spdk_json_strdup(const struct spdk_json_val *val);

/*
 * [한국어] NUMBER 토큰의 텍스트 표현을 정수형으로 안전 변환하는 함수 패밀리.
 * 모두 @val: NUMBER 토큰, @num: 결과 출력. @return 0 성공, 음수(-EINVAL/-ERANGE) 실패.
 * spdk_json_decode_<type>이 내부적으로 이 변환기를 호출.
 */
int spdk_json_number_to_uint8(const struct spdk_json_val *val, uint8_t *num);
/* [한국어] NUMBER 토큰 → uint8_t. 0~255 외 또는 부동소수점이면 실패. */
int spdk_json_number_to_uint16(const struct spdk_json_val *val, uint16_t *num);
/* [한국어] NUMBER 토큰 → uint16_t. 0~65535 외 또는 부동소수점이면 실패. */
int spdk_json_number_to_int32(const struct spdk_json_val *val, int32_t *num);
/* [한국어] NUMBER 토큰 → int32_t. INT32 범위 외 또는 부동소수점이면 실패. */
int spdk_json_number_to_uint32(const struct spdk_json_val *val, uint32_t *num);
/* [한국어] NUMBER 토큰 → uint32_t. 음수/범위 외/부동소수점이면 실패. */
int spdk_json_number_to_uint64(const struct spdk_json_val *val, uint64_t *num);
/* [한국어] NUMBER 토큰 → uint64_t. 음수/범위 외/부동소수점이면 실패. */

/*
 * [한국어] JSON streaming writer 컨텍스트 — 불투명 타입.
 * lib/json/json_write.c에 정의되며 내부에 출력 버퍼, 콤마/콜론 자동 삽입을 위한
 * 상태머신(직전 토큰 종류, 객체/배열 nesting depth, 키 다음 콜론 대기 여부 등),
 * 사용자 write_cb와 cb_ctx 핸들을 보관한다.
 */
struct spdk_json_write_ctx;

#define SPDK_JSON_WRITE_FLAG_FORMATTED	0x00000001
/* [한국어] spdk_json_write_begin() 플래그 — 사람이 읽기 좋게 들여쓰기 + 줄바꿈 삽입.
 * 디폴트(0)는 compact mode(공백 없음, 네트워크 효율). config 파일 저장 시 켠다. */

/*
 * [한국어] writer가 토큰을 만들 때마다 호출되는 사용자 sink 콜백.
 * @cb_ctx: spdk_json_write_begin에 넘긴 context(사용자 정의 — 보통 file/sock fd 또는 buffer).
 * @data:   직렬화된 JSON 텍스트 청크(연속 호출하면 이어붙여 한 메시지).
 * @size:   data의 바이트 수.
 * @return: 0 성공, 음수면 writer가 모든 후속 호출을 단락(에러 전파).
 */
typedef int (*spdk_json_write_cb)(void *cb_ctx, const void *data, size_t size);

/*
 * [한국어]
 * spdk_json_write_begin - JSON streaming writer 컨텍스트 생성.
 *
 * @write_cb: 출력 sink 콜백(예: spdk_jsonrpc 내부 소켓 송신 함수).
 * @cb_ctx:   콜백에 매번 전달될 컨텍스트.
 * @flags:    SPDK_JSON_WRITE_FLAG_FORMATTED 등.
 * @return:   새 컨텍스트 또는 NULL(메모리 부족).
 *
 * 라이프사이클: begin → write_* 시퀀스 → end. 중간 실패해도 end로 자원 해제.
 */
struct spdk_json_write_ctx *spdk_json_write_begin(spdk_json_write_cb write_cb, void *cb_ctx,
		uint32_t flags);

/*
 * [한국어]
 * spdk_json_write_end - writer flush + 컨텍스트 해제.
 * @w: spdk_json_write_begin 반환값.
 * @return: 0 성공, 음수 실패(콜백이 음수 반환했거나 객체/배열 짝이 안 맞음).
 */
int spdk_json_write_end(struct spdk_json_write_ctx *w);

/*
 * [한국어]
 * spdk_json_write_reset - 컨텍스트 내부 버퍼/nesting 상태를 초기화(재사용).
 * @w: writer. RPC 응답 1건 작성 후 다음 응답 작성을 위해 호출되기도 한다.
 */
void spdk_json_write_reset(struct spdk_json_write_ctx *w);

/* [한국어] writer에 단일 값 토큰을 출력. 모두 @return 0 성공, 음수 실패.
 * 객체 내부에서는 호출 직전에 spdk_json_write_name()이 선행되어야 한다(아니면 배열/최상위). */
int spdk_json_write_null(struct spdk_json_write_ctx *w);
/* [한국어] JSON null 출력. */
int spdk_json_write_bool(struct spdk_json_write_ctx *w, bool val);
/* [한국어] true/false 출력. */
int spdk_json_write_uint8(struct spdk_json_write_ctx *w, uint8_t val);
/* [한국어] 0~255 정수 출력. */
int spdk_json_write_uint16(struct spdk_json_write_ctx *w, uint16_t val);
/* [한국어] 0~65535 정수 출력. */
int spdk_json_write_int32(struct spdk_json_write_ctx *w, int32_t val);
/* [한국어] 부호 있는 32비트 정수 출력. */
int spdk_json_write_uint32(struct spdk_json_write_ctx *w, uint32_t val);
/* [한국어] 부호 없는 32비트 정수 출력. */
int spdk_json_write_int64(struct spdk_json_write_ctx *w, int64_t val);
/* [한국어] 부호 있는 64비트 정수 출력. JSON 스펙은 정수 정밀도를 정의하지 않지만,
 * SPDK는 int64까지 정확히 직렬화한다(클라이언트가 BigInt 처리해야 할 수 있음). */
int spdk_json_write_uint64(struct spdk_json_write_ctx *w, uint64_t val);
/* [한국어] 부호 없는 64비트 정수 출력. */
int spdk_json_write_uint128(struct spdk_json_write_ctx *w, uint64_t low_val, uint64_t high_val);
/* [한국어] 128비트 정수를 두 uint64(low/high)로 받아 큰 10진수 문자열로 출력.
 * NVMe 통계 같은 매우 큰 카운터를 손실 없이 직렬화. */
int spdk_json_write_double(struct spdk_json_write_ctx *w, double val);
/* [한국어] 부동소수점 숫자 출력. NaN/Inf는 RFC가 금지 — 호출자가 사전에 거른다. */
int spdk_json_write_string(struct spdk_json_write_ctx *w, const char *val);
/* [한국어] NUL-종결 C 문자열을 따옴표 + escape 처리하여 JSON STRING으로 출력. */
int spdk_json_write_string_raw(struct spdk_json_write_ctx *w, const char *val, size_t len);
/* [한국어] 길이 명시 버전. embedded \0를 허용하는 임의 byte 시퀀스 직렬화에 사용. */
int spdk_json_write_bytearray(struct spdk_json_write_ctx *w, const void *val, size_t len);
/* [한국어] 임의 byte 배열을 hex 문자열("AABBCC...")로 출력. binary blob의 표준 직렬화. */
int spdk_json_write_uuid(struct spdk_json_write_ctx *w, const struct spdk_uuid *uuid);
/* [한국어] UUID를 표준 형식("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx")으로 출력. */

/**
 * Write null-terminated UTF-16LE string.
 *
 * \param w JSON write context.
 * \param val UTF-16LE string; must be null terminated.
 * \return 0 on success or negative on failure.
 */
/*
 * [한국어]
 * spdk_json_write_string_utf16le - UTF-16LE 문자열을 UTF-8 JSON STRING으로 변환 출력.
 * Windows/SPDK NVMe identify 페이지 등 UTF-16LE 원본을 직렬화할 때 사용. NUL 종결 필수.
 */
int spdk_json_write_string_utf16le(struct spdk_json_write_ctx *w, const uint16_t *val);

/**
 * Write UTF-16LE string.
 *
 * \param w JSON write context.
 * \param val UTF-16LE string; may contain embedded null characters.
 * \param len Length of val in 16-bit code units (i.e. size of string in bytes divided by 2).
 * \return 0 on success or negative on failure.
 */
/*
 * [한국어]
 * spdk_json_write_string_utf16le_raw - 길이 명시 UTF-16LE → UTF-8 JSON STRING 변환.
 * @len: 16비트 코드 유닛 수(바이트 수가 아님!).
 */
int spdk_json_write_string_utf16le_raw(struct spdk_json_write_ctx *w, const uint16_t *val,
				       size_t len);

/*
 * [한국어]
 * spdk_json_write_string_fmt - printf 스타일 포맷팅 후 결과를 JSON STRING으로 출력.
 * @w: writer. @fmt: printf 포맷. ...: 인자들.
 * GCC __format__ attribute로 컴파일 시 포맷 검증 활성화.
 */
int spdk_json_write_string_fmt(struct spdk_json_write_ctx *w, const char *fmt,
			       ...) __attribute__((__format__(__printf__, 2, 3)));

/*
 * [한국어]
 * spdk_json_write_string_fmt_v - vprintf 스타일(va_list 받는 버전).
 * 가변 인자 wrapper에서 사용.
 */
int spdk_json_write_string_fmt_v(struct spdk_json_write_ctx *w, const char *fmt, va_list args);

/*
 * [한국어] 컨테이너 시작/끝 출력. begin/end 짝이 맞아야 spdk_json_write_end()가 성공.
 * writer 내부에 nesting 스택을 유지하며, 콤마/콜론 자동 삽입의 근거가 된다.
 */
int spdk_json_write_array_begin(struct spdk_json_write_ctx *w);
/* [한국어] '[' 출력. 객체 키 다음이면 콜론 선행, 배열/최상위면 필요시 콤마 선행. */
int spdk_json_write_array_end(struct spdk_json_write_ctx *w);
/* [한국어] ']' 출력. 짝이 안 맞으면 음수 반환. */
int spdk_json_write_object_begin(struct spdk_json_write_ctx *w);
/* [한국어] '{' 출력. */
int spdk_json_write_object_end(struct spdk_json_write_ctx *w);
/* [한국어] '}' 출력. */
int spdk_json_write_name(struct spdk_json_write_ctx *w, const char *name);
/* [한국어] 객체 키 출력 — "name": 형태. 직후에 반드시 한 개의 값(write_*) 호출이 와야 한다.
 * NUL-종결 C 문자열 입력. */
int spdk_json_write_name_raw(struct spdk_json_write_ctx *w, const char *name, size_t len);
/* [한국어] 길이 명시 버전. embedded \0 또는 비-NUL-종결 입력에 사용. */

/*
 * [한국어]
 * spdk_json_write_val - 이미 파싱된 spdk_json_val 한 개(컨테이너 포함)를 그대로 다시 출력.
 *
 * @w: writer. @val: 출력할 토큰(컨테이너면 자식 토큰 전체를 재귀적으로 출력).
 * @return: 0 성공.
 *
 * RPC pass-through(클라이언트가 보낸 임의 객체를 응답에 그대로 포함)나 cached JSON 재사용에 유용.
 */
int spdk_json_write_val(struct spdk_json_write_ctx *w, const struct spdk_json_val *val);

/*
 * Append bytes directly to the output stream without validation.
 *
 * Can be used to write values with specific encodings that differ from the JSON writer output.
 */
/*
 * [한국어]
 * spdk_json_write_val_raw - 임의 바이트를 출력 스트림에 그대로 부착(검증 없음).
 *
 * @w: writer. @data/@len: 부착할 바이트.
 *
 * 위험: 호출자가 JSON 문법을 깨뜨리면 결과가 무효. 매우 드문 케이스(예: 사전 직렬화된
 * JSON 청크 삽입)에서만 사용.
 */
int spdk_json_write_val_raw(struct spdk_json_write_ctx *w, const void *data, size_t len);

/* Utility functions */
/*
 * [한국어] "named" 단축형 — 객체 안에서 spdk_json_write_name() + spdk_json_write_<type>()를
 * 한 번에 수행. RPC 응답을 객체로 만들 때 가장 자주 쓰이는 형태.
 * 모두 @return 0 성공, 음수 실패. 호출자는 spdk_json_write_object_begin() 다음에 사용.
 */
int spdk_json_write_named_null(struct spdk_json_write_ctx *w, const char *name);
/* [한국어] "name": null 출력. */
int spdk_json_write_named_bool(struct spdk_json_write_ctx *w, const char *name, bool val);
/* [한국어] "name": true/false 출력. */
int spdk_json_write_named_uint8(struct spdk_json_write_ctx *w, const char *name, uint8_t val);
/* [한국어] "name": 0~255 출력. */
int spdk_json_write_named_uint16(struct spdk_json_write_ctx *w, const char *name, uint16_t val);
/* [한국어] "name": 0~65535 출력. */
int spdk_json_write_named_int32(struct spdk_json_write_ctx *w, const char *name, int32_t val);
/* [한국어] "name": int32 출력. */
int spdk_json_write_named_uint32(struct spdk_json_write_ctx *w, const char *name, uint32_t val);
/* [한국어] "name": uint32 출력. */
int spdk_json_write_named_int64(struct spdk_json_write_ctx *w, const char *name, int64_t val);
/* [한국어] "name": int64 출력. */
int spdk_json_write_named_uint64(struct spdk_json_write_ctx *w, const char *name, uint64_t val);
/* [한국어] "name": uint64 출력. */
int spdk_json_write_named_uint128(struct spdk_json_write_ctx *w, const char *name,
				  uint64_t low_val, uint64_t high_val);
/* [한국어] "name": uint128 출력(low+high 두 uint64로 입력). */
int spdk_json_write_named_double(struct spdk_json_write_ctx *w, const char *name, double val);
/* [한국어] "name": double 출력. */

int spdk_json_write_named_string(struct spdk_json_write_ctx *w, const char *name, const char *val);
/* [한국어] "name": "val" 출력(NUL-종결 문자열). 가장 흔한 RPC 응답 패턴. */
int spdk_json_write_named_string_fmt(struct spdk_json_write_ctx *w, const char *name,
				     const char *fmt, ...) __attribute__((__format__(__printf__, 3, 4)));
/* [한국어] "name": printf-formatted 문자열. 컴파일 타임 포맷 검증. */
int spdk_json_write_named_string_fmt_v(struct spdk_json_write_ctx *w, const char *name,
				       const char *fmt, va_list args);
/* [한국어] vprintf 변종. */
int spdk_json_write_named_bytearray(struct spdk_json_write_ctx *w, const char *name,
				    const void *val, size_t len);
/* [한국어] "name": "AABBCC..." 형태로 바이트 배열을 hex 문자열로 출력. */
int spdk_json_write_named_uuid(struct spdk_json_write_ctx *w, const char *name,
			       const struct spdk_uuid *uuid);
/* [한국어] "name": "<uuid>" 출력. */

int spdk_json_write_named_array_begin(struct spdk_json_write_ctx *w, const char *name);
/* [한국어] "name": [ 출력 — 이후 원소 출력 후 spdk_json_write_array_end() 호출 필요. */
int spdk_json_write_named_object_begin(struct spdk_json_write_ctx *w, const char *name);
/* [한국어] "name": { 출력 — 이후 키/값 출력 후 spdk_json_write_object_end() 호출 필요. */

/**
 * Return JSON value associated with key \c key_name. Subobjects won't be searched.
 *
 * \param object JSON object to be examined
 * \param key_name name of the key
 * \param key optional, will be set with found key
 * \param val optional, will be set with value of the key
 * \param type search for specific value type. Pass SPDK_JSON_VAL_ANY to match any type.
 * \return 0 if found or negative error code:
 * -EINVAL - json object is invalid
 * -ENOENT - key not found
 * -EDOM - key exists but value type mismatch.
 * -EPROTOTYPE - json not enclosed in {}.
 */
/*
 * [한국어]
 * spdk_json_find - JSON 객체 1단계에서 특정 키/타입을 검색.
 *
 * @object:   OBJECT_BEGIN 토큰. 1단계만 검색(중첩 객체로 들어가지 않음).
 * @key_name: 찾을 키 이름.
 * @key:      (선택) 매치된 NAME 토큰 포인터를 받음. NULL이면 무시.
 * @val:      (선택) 매치된 value 토큰 포인터를 받음. NULL이면 무시.
 * @type:     기대하는 value 타입. SPDK_JSON_VAL_ANY면 타입 무관.
 * @return:   0 성공, -EINVAL/-ENOENT/-EDOM/-EPROTOTYPE 실패.
 *
 * decode_object 표를 만들기 어려운 동적 케이스(키 이름이 런타임 결정 등)에 사용.
 */
int spdk_json_find(struct spdk_json_val *object, const char *key_name, struct spdk_json_val **key,
		   struct spdk_json_val **val, enum spdk_json_val_type type);

/**
 * The same as calling \c spdk_json_find() function with \c type set to \c SPDK_JSON_VAL_STRING
 *
 * \param object JSON object to be examined
 * \param key_name name of the key
 * \param key optional, will be set with found key
 * \param val optional, will be set with value of the key
 * \return See \c spdk_json_find
 */
/*
 * [한국어]
 * spdk_json_find_string - spdk_json_find의 type=STRING 단축형.
 * 매치된 value가 STRING이 아니면 -EDOM.
 */
int spdk_json_find_string(struct spdk_json_val *object, const char *key_name,
			  struct spdk_json_val **key, struct spdk_json_val **val);

/**
 * The same as calling \c spdk_json_key() function with \c type set to \c SPDK_JSON_VAL_ARRAY_BEGIN
 *
 * \param object JSON object to be examined
 * \param key_name name of the key
 * \param key optional, will be set with found key
 * \param value optional, will be set with key value
 * \return See \c spdk_json_find
 */
/*
 * [한국어]
 * spdk_json_find_array - spdk_json_find의 type=ARRAY_BEGIN 단축형.
 * 매치된 value가 배열이 아니면 -EDOM.
 */
int spdk_json_find_array(struct spdk_json_val *object, const char *key_name,
			 struct spdk_json_val **key, struct spdk_json_val **value);

/**
 * Return first JSON value in given JSON object.
 *
 * \param object pointer to JSON object begin
 * \return Pointer to first object or NULL if object is empty or is not an JSON object
 */
/*
 * [한국어]
 * spdk_json_object_first - 객체의 첫 NAME 토큰 반환(iterator 시작).
 *
 * @object: OBJECT_BEGIN 토큰.
 * @return: 첫 키 NAME 토큰 또는 NULL(빈 객체/객체 아님).
 *
 * 사용 패턴: for (k = spdk_json_object_first(o); k; k = spdk_json_next(k)) { ... }
 */
struct spdk_json_val *spdk_json_object_first(struct spdk_json_val *object);

/**
 * Return first JSON value in array.
 *
 * \param array_begin pointer to JSON array begin
 * \return Pointer to first JSON value or NULL if array is empty or is not an JSON array.
 */
/*
 * [한국어]
 * spdk_json_array_first - 배열의 첫 원소 토큰 반환(iterator 시작).
 *
 * @array_begin: ARRAY_BEGIN 토큰.
 * @return:      첫 원소 토큰 또는 NULL(빈 배열/배열 아님).
 *
 * 사용 패턴: for (v = spdk_json_array_first(a); v; v = spdk_json_next(v)) { ... }
 */
struct spdk_json_val *spdk_json_array_first(struct spdk_json_val *array_begin);

/**
 * Advance to the next JSON value in JSON object or array.
 *
 * \warning if \c pos is not JSON key or JSON array element behaviour is undefined.
 *
 * \param pos pointer to JSON key if iterating over JSON object or array element
 * \return next JSON value or NULL if there is no more objects or array elements
 */
/*
 * [한국어]
 * spdk_json_next - 같은 컨테이너의 다음 sibling 토큰으로 이동.
 *
 * @pos:    현재 위치 — 객체면 NAME 토큰, 배열이면 원소 토큰이어야 한다.
 * @return: 다음 NAME 또는 원소 토큰. 컨테이너 끝이면 NULL.
 *
 * 내부적으로 spdk_json_val_len()을 활용해 중첩 컨테이너를 통째로 건너뛴다.
 */
struct spdk_json_val *spdk_json_next(struct spdk_json_val *pos);

#ifdef __cplusplus
}                          /* [한국어] extern "C" 종결 */
#endif

#endif                     /* [한국어] SPDK_JSON_H_ — include 가드 종결 */
