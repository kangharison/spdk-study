/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK JSON 토큰 디코더/검색 유틸리티 (json_util.c)
 *
 * === 파일의 역할 ===
 * json_parse.c가 만들어낸 spdk_json_val 토큰 배열을 "C 자료구조로 변환"하거나
 * "객체/배열을 트리처럼 탐색"하기 위한 유틸리티 함수 모음이다. 크게 세 그룹:
 *   (A) 토큰 단위 헬퍼 — spdk_json_val_len(트리 점프 거리), spdk_json_strequal/strdup
 *       (NAME/STRING 비교/복제), json_number_split(수치 토큰 → significand/exponent 분해).
 *   (B) 타입별 number/스트링/uuid/bool 디코더 — spdk_json_number_to_uint8/16/32/64/int32,
 *       spdk_json_decode_uint8/16/32/64/int32/string/bool/uuid. JSON-RPC params를
 *       C struct 필드로 안전하게 옮기는 데 쓰인다(overflow/range 검사 포함).
 *   (C) 객체/배열 디코더 + 검색 — _json_decode_object/spdk_json_decode_object/_relaxed,
 *       spdk_json_decode_array, spdk_json_find/_string/_array, spdk_json_object_first/
 *       _array_first, spdk_json_next, json_skip_object_or_array, spdk_json_free_object.
 * 핵심 디자인은 "decoder 표(spdk_json_object_decoder[])"를 통한 선언적 매핑이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 모든 JSON-RPC 핸들러가 spdk_jsonrpc_decode_object 등을 통해 본 모듈을 호출한다.
 * 일반 흐름:
 *   spdk_json_parse → spdk_json_val[] →
 *   spdk_json_decode_object(values, decoders[], num_decoders, &out_struct)
 *     → 각 key를 검색해 dec->decode_func 호출(spdk_json_decode_string/uint32/...) →
 *     C 필드 채움 → RPC 본문 실행.
 * 본 파일은 모든 SPDK reactor 스레드에서 호출 가능하지만 자체에는 어떠한 스레드/락
 * 객체도 없다(순수 함수). 디버그 로깅은 SPDK_DEBUGLOG(component=json_util)로 출력.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/json.h: 토큰 타입/구조체/디코더 시그니처 등 공개 API.
 *   - spdk_internal/utf.h: 문자열 비교/복제 시 UTF-8 검증은 json_parse.c에서 끝났으므로
 *     여기서는 직접 호출하지 않지만, 향후 확장(예: 케이스 인센서티브 비교)을 위해 포함.
 *   - spdk/log.h: SPDK_DEBUGLOG — 잘못된 키/타입을 디버그 로그로 보고.
 *   - libc: malloc/free/calloc/memcpy/memcmp/memchr.
 *   - spdk_uuid_parse(spdk/uuid.h 구현): UUID 디코드 시 호출.
 * 의존받음(피호출):
 *   - lib/jsonrpc/*: spdk_jsonrpc_decode_object → 본 파일의 decode_object를 그대로 사용.
 *   - bdev/nvme/nvmf 각 RPC 핸들러: spdk_json_decode_uint32 등.
 *   - 설정 파일 로더 lib/init/subsystem.c.
 * 데이터 흐름: spdk_json_val[] (input) → C struct(field) (output).
 * 공유 상태: 없음(decoder 표는 호출자가 자체적으로 정적 정의).
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_json_val_len(): 토큰 1개를 "건너뛰기 위한" 거리 — 객체/배열은 _BEGIN.len + 2(BEGIN+END).
 *   - spdk_json_strequal()/strdup(): NAME/STRING 토큰을 C string과 비교/복제. 임베디드 NUL 거부.
 *   - struct spdk_json_num + json_number_split(): NUMBER 토큰 텍스트를
 *     (negative, significand, exponent)로 분해 — 모든 정수 변환의 공통 1차 통과.
 *   - spdk_json_number_to_uint8/16/32/64/int32(): split 결과를 검증 후 C 정수로.
 *   - _json_decode_object()/spdk_json_decode_object()/_relaxed(): 키-decoder 표 매칭.
 *     relaxed 모드는 모르는 키를 무시(strict 모드는 실패).
 *   - spdk_json_decode_array(): 배열을 C 배열로 — 원소마다 decode_fn 호출.
 *   - spdk_json_decode_<type>(): bool/uint8~64/string/uuid wrapper.
 *   - spdk_json_find()/_string()/_array(): 객체에서 키 검색 — 중복 키는 -EINVAL.
 *   - spdk_json_object_first()/array_first()/next(): 컨테이너 iterator.
 *   - json_skip_object_or_array(): 컨테이너 한 단위를 토큰 단위로 건너뜀(중첩 깊이 추적).
 *   - spdk_json_free_object(): decoder 표 기반 해제 — "INVALID 토큰 디코드"를
 *     자유 자원 해제 시그널로 재사용하는 트릭.
 */

#include "spdk/json.h"          /* [한국어] 공개 토큰/디코더 API */

#include "spdk_internal/utf.h"  /* [한국어] UTF 헬퍼(향후 확장 대비 — 현재 직접 호출 없음) */
#include "spdk/log.h"           /* [한국어] SPDK_DEBUGLOG 매크로 */

#define SPDK_JSON_DEBUG(...) SPDK_DEBUGLOG(json_util, __VA_ARGS__)
/* [한국어] 본 파일 전용 디버그 로그 매크로.
 * 컴포넌트 이름 'json_util'은 파일 끝 SPDK_LOG_REGISTER_COMPONENT()로 등록되며,
 * 사용자가 spdk_log_set_print_level/RPC log_set_flag로 토글 가능하다. */

/*
 * [한국어]
 * spdk_json_val_len - 토큰 하나를 "건너뛰는 거리"를 토큰 단위로 반환.
 *
 * @val: 토큰 포인터(NULL 허용 — 0 반환).
 * @return: 건너뛸 토큰 수. 컨테이너(_BEGIN)는 자식 + _END 포함, 그 외 토큰은 1.
 *
 * 동기: spdk_json_parse가 _OBJECT_BEGIN/_ARRAY_BEGIN의 len 필드에 "내부 자식 토큰 개수"를
 *       기록해 두므로, BEGIN+자식+END = (len + 2)개를 한 번에 건너뛸 수 있다 — O(1) 트리 점프.
 *       호출자(decode_array/decode_object)는 이 함수로 "다음 형제 토큰" 위치를 계산한다.
 */
size_t
spdk_json_val_len(const struct spdk_json_val *val)
{
	if (val == NULL) {
		return 0;                          /* [한국어] NULL 입력 안전성 — 호출자 if 분기 절약 */
	}

	if (val->type == SPDK_JSON_VAL_ARRAY_BEGIN || val->type == SPDK_JSON_VAL_OBJECT_BEGIN) {
		return val->len + 2;               /* [한국어] _BEGIN(1) + 자식(len) + _END(1) */
	}

	return 1;                              /* [한국어] 단일 값 토큰 또는 단일 NAME — 1개로 카운트 */
}

/*
 * [한국어]
 * spdk_json_strequal - STRING/NAME 토큰을 NUL-종결 C string과 정확 비교.
 *
 * @val: 비교할 토큰. 문자열/이름이 아니면 false.
 * @str: 비교 대상 NUL-종결 문자열.
 * @return: 길이와 내용이 모두 같으면 true.
 *
 * 동기: 토큰의 start는 zero-copy로 입력 버퍼를 가리키므로 NUL-종결이 아니다 → 길이 기반 비교.
 * 호출 체인: _json_decode_object(name 비교), spdk_json_find(키 검색) 등에서 사용.
 */
bool
spdk_json_strequal(const struct spdk_json_val *val, const char *str)
{
	size_t len;

	if (val->type != SPDK_JSON_VAL_STRING && val->type != SPDK_JSON_VAL_NAME) {
		return false;                          /* [한국어] 문자열 토큰 아니면 매치 불가 */
	}

	len = strlen(str);
	if (val->len != len) {
		return false;                          /* [한국어] 길이 prefilter — memcmp 비용 절약 */
	}

	return memcmp(val->start, str, len) == 0;  /* [한국어] 본문 바이트 비교(escape 디코드 후 raw 바이트 동일성) */
}

/*
 * [한국어]
 * spdk_json_strdup - STRING/NAME 토큰을 NUL-종결 C string으로 복제(malloc).
 *
 * @val: 복제할 토큰. 문자열 아니면 NULL.
 * @return: malloc 된 NUL-종결 문자열, 또는 NULL(타입 불일치 / 임베디드 NUL / 메모리 부족).
 *
 * 임베디드 NUL 거부 이유: C 코드에서 "%s"/strlen 등 NUL-기반 API와 함께 쓸 때
 *   silently truncated되는 보안/버그를 막기 위함. 호출자는 NULL 반환 시 -ENOMEM이 아니라
 *   잘못된 입력 가능성도 함께 고려해야 한다.
 * 호출 체인: spdk_json_decode_string → spdk_json_strdup → 사용자 char*.
 */
char *
spdk_json_strdup(const struct spdk_json_val *val)
{
	size_t len;
	char *s;

	if (val->type != SPDK_JSON_VAL_STRING && val->type != SPDK_JSON_VAL_NAME) {
		return NULL;
	}

	len = val->len;

	if (memchr(val->start, '\0', len)) {
		/* String contains embedded NUL, so it is not a valid C string. */
		return NULL;                              /* [한국어] 보안: NUL이 박힌 문자열은 C 문자열로 사용 불가 */
	}

	s = malloc(len + 1);                          /* [한국어] +1: NUL 종결자 자리 */
	if (s == NULL) {
		return s;                                  /* [한국어] OOM */
	}

	memcpy(s, val->start, len);                    /* [한국어] 본문 복사 */
	s[len] = '\0';                                  /* [한국어] NUL 종결 */

	return s;
}

/*
 * [한국어] 수치 토큰을 분해 저장하는 내부 표현.
 * 임의 정밀도 부동소수 라이브러리를 끌어들이지 않고도 정수 변환에 충분한 정보를 보존한다.
 */
struct spdk_json_num {
	bool negative;
	/* [한국어] 부호. 값 자체가 0이라도 '-0' 텍스트면 true가 들어올 수 있음(역할: 정수 변환에서 거부 판단).
	 * 설정자: json_number_split의 첫 글자 '-' 처리.
	 * 읽는 자: spdk_json_number_to_*에서 음수 허용 여부 판단. */

	uint64_t significand;
	/* [한국어] 가수부(0~UINT64_MAX). 정수 부분과 소수부의 모든 디짓을 누적해 보관.
	 * 예) 12.345 → significand=12345, exponent=-3.
	 * 설정자: json_number_split 메인 루프(*pval = new_val).
	 * 읽는 자: 각 type별 변환기에서 UINT8/16/32/64_MAX와 비교 후 캐스팅.
	 * 동기화: 호출자 스택 변수만 사용 — 락 불필요. */

	int64_t exponent;
	/* [한국어] 10의 지수부(부호 있음). 'e±N' 표기와 소수 자릿수를 합산해 정규화.
	 * 예) 1.5e2 → significand=15, exponent=2-1=1.
	 * 설정자: json_number_split 후처리(±exponent_u64 - frac_digits + significand 정규화 루프).
	 * 읽는 자: 정수 변환기에서 0인지 검사(0이 아니면 -ERANGE — 정수 컨버터는 정확한 정수만 허용). */
};

/*
 * [한국어]
 * json_number_split - NUMBER 토큰의 텍스트를 (negative, significand, exponent)로 분해.
 *
 * @val: 분해할 NUMBER 토큰.
 * @num: out — 분해 결과(호출자가 스택에 자리 할당).
 * @return: 0(성공), -EINVAL(타입 불일치/빈 문자열), -ERANGE(significand/exponent 오버플로).
 *
 * 동작 단계:
 *   1) 부호 처리 — '-'면 negative=true, iter 진행.
 *   2) state machine(INT/FRAC/EXP)으로 한 글자씩 읽으며 누적기(*pval)를 10진수로 누적.
 *      소수 자릿수는 frac_digits로 따로 셈.
 *   3) 지수부를 부호와 함께 int64_t로 변환 후 -frac_digits 보정(= 10^exponent의 최종 지수).
 *   4) 지수가 음수면 significand의 trailing zero를 가능한 한 제거(정확도 손실 없는 정규화),
 *      양수면 significand에 곱할 수 있는 만큼 곱해 exponent를 0에 가깝게 만든다.
 * 호출 체인: spdk_json_number_to_uint8/16/32/64/int32 → json_number_split.
 *
 * 주의: 본 함수는 부동소수로 변환하지 않는다 — 정확한 정수 표현 가능 여부를 정수 변환기가 판정.
 */
static int
json_number_split(const struct spdk_json_val *val, struct spdk_json_num *num)
{
	const char *iter;                            /* [한국어] 입력 스캔 포인터 */
	size_t remaining;                             /* [한국어] 남은 바이트 수 */
	uint64_t *pval;                               /* [한국어] 현재 누적할 대상(int 부분/지수 부분 토글) */
	uint64_t frac_digits = 0;                     /* [한국어] 소수 자릿수 카운트 — 마지막에 exponent에서 차감 */
	uint64_t exponent_u64 = 0;                    /* [한국어] 지수부 절대값 누적기(나중에 부호 적용) */
	bool exponent_negative = false;               /* [한국어] 지수 부호 */
	enum {
		NUM_STATE_INT,                            /* [한국어] '.' 이전 — 정수부 누적 */
		NUM_STATE_FRAC,                           /* [한국어] '.' 이후 — 소수부 누적(frac_digits 증가) */
		NUM_STATE_EXP,                            /* [한국어] 'e'/'E' 이후 — 지수부 누적(pval 전환) */
	} state;

	memset(num, 0, sizeof(*num));                  /* [한국어] 출력 구조체 초기화(필드 일관성) */

	if (val->type != SPDK_JSON_VAL_NUMBER) {
		return -EINVAL;                            /* [한국어] 호출자 실수 방어 */
	}

	remaining = val->len;
	if (remaining == 0) {
		return -EINVAL;                            /* [한국어] 빈 NUMBER 토큰은 발생할 수 없지만 방어 */
	}

	iter = val->start;
	if (*iter == '-') {
		num->negative = true;                      /* [한국어] 부호 기록 */
		iter++;
		remaining--;
	}

	state = NUM_STATE_INT;                          /* [한국어] 부호 다음은 항상 정수부 시작 */
	pval = &num->significand;                       /* [한국어] 누적기 — 정수/소수 둘 다 같은 significand에 누적 */
	while (remaining--) {
		char c = *iter++;

		if (c == '.') {
			state = NUM_STATE_FRAC;                  /* [한국어] 정수→소수 전환(누적 대상은 동일 — significand에 그대로 붙임) */
		} else if (c == 'e' || c == 'E') {
			state = NUM_STATE_EXP;                   /* [한국어] 지수부 시작 */
			pval = &exponent_u64;                    /* [한국어] 누적 대상 전환 */
		} else if (c == '-') {
			assert(state == NUM_STATE_EXP);          /* [한국어] 부호는 지수부에서만 등장(parser가 보장) */
			exponent_negative = true;
		} else if (c == '+') {
			assert(state == NUM_STATE_EXP);
			/* exp_negative = false; */ /* already false by default */ /* [한국어] '+'는 의미상 noop */
		} else {
			uint64_t new_val;

			assert(c >= '0' && c <= '9');            /* [한국어] parser DFA가 다른 글자는 통과시키지 않음 */
			new_val = *pval * 10 + c - '0';          /* [한국어] 10진수 자릿수 누적 */
			if (new_val < *pval) {
				return -ERANGE;                       /* [한국어] uint64 오버플로 — 토큰이 너무 큼 */
			}

			if (state == NUM_STATE_FRAC) {
				frac_digits++;                        /* [한국어] 소수부 자릿수 카운트 — exponent 보정에 사용 */
			}

			*pval = new_val;
		}
	}

	if (exponent_negative) {
		if (exponent_u64 > 9223372036854775808ULL) { /* abs(INT64_MIN) */
			return -ERANGE;                           /* [한국어] int64로 표현 불가능한 음수 지수 */
		}
		num->exponent = (int64_t) - exponent_u64;
	} else {
		if (exponent_u64 > INT64_MAX) {
			return -ERANGE;                           /* [한국어] 양수 지수 오버플로 */
		}
		num->exponent = exponent_u64;
	}
	num->exponent -= frac_digits;                     /* [한국어] 소수부를 정수처럼 모았으므로 그만큼 지수에서 차감(자리수 회복) */

	/* Apply as much of the exponent as possible without overflow or truncation */
	if (num->exponent < 0) {
		while (num->exponent && num->significand >= 10 && num->significand % 10 == 0) {
			num->significand /= 10;                   /* [한국어] trailing zero를 제거하면서 exponent를 0에 접근 — 손실 없는 정규화 */
			num->exponent++;
		}
	} else { /* positive exponent */
		while (num->exponent) {
			uint64_t new_val = num->significand * 10;

			if (new_val < num->significand) {
				break;                                /* [한국어] 곱셈 오버플로 — 더 이상 정규화 불가, exponent 일부 잔존 */
			}

			num->significand = new_val;
			num->exponent--;
		}
	}

	return 0;                                         /* [한국어] 정상 분해 완료 — 정확 정수 여부는 호출자가 판단 */
}

/*
 * [한국어]
 * spdk_json_number_to_uint8 - NUMBER 토큰을 정확한 uint8로 변환.
 * @val: NUMBER 토큰. @num: out — uint8 결과.
 * @return: 0 / -EINVAL(타입) / -ERANGE(부호/소수/오버플로).
 *
 * 정확성 정책: 정수 컨버터는 "딱 떨어지는 정수"만 허용한다 — exponent != 0이거나
 *   negative이면 -ERANGE. JSON-RPC params에 1.5나 -1을 uint로 넣는 실수를 즉시 차단.
 */
int
spdk_json_number_to_uint8(const struct spdk_json_val *val, uint8_t *num)
{
	struct spdk_json_num split_num;
	int rc;

	rc = json_number_split(val, &split_num);          /* [한국어] 1차 분해 */
	if (rc) {
		return rc;
	}

	if (split_num.exponent || split_num.negative) {
		return -ERANGE;                                /* [한국어] uint은 양의 정수만 */
	}

	if (split_num.significand > UINT8_MAX) {
		return -ERANGE;                                /* [한국어] 8비트 범위 초과 */
	}
	*num = (uint8_t)split_num.significand;
	return 0;
}

/*
 * [한국어] spdk_json_number_to_uint16 — uint16 변환. uint8와 동일한 정책. */
int
spdk_json_number_to_uint16(const struct spdk_json_val *val, uint16_t *num)
{
	struct spdk_json_num split_num;
	int rc;

	rc = json_number_split(val, &split_num);
	if (rc) {
		return rc;
	}

	if (split_num.exponent || split_num.negative) {
		return -ERANGE;
	}

	if (split_num.significand > UINT16_MAX) {
		return -ERANGE;
	}
	*num = (uint16_t)split_num.significand;          /* [한국어] 16비트 캐스팅 */
	return 0;
}

/*
 * [한국어] spdk_json_number_to_int32 — 부호 있는 32비트 변환.
 *
 * 음수 처리: significand > abs(INT32_MIN)=2^31 이면 -ERANGE. INT32_MIN 자체는 표현 가능하므로
 *   `2147483648`까지 허용 → -INT32_MIN가 int32에 안 들어가는 표준 함정을 (int64) 캐스팅으로 회피. */
int
spdk_json_number_to_int32(const struct spdk_json_val *val, int32_t *num)
{
	struct spdk_json_num split_num;
	int rc;

	rc = json_number_split(val, &split_num);
	if (rc) {
		return rc;
	}

	if (split_num.exponent) {
		return -ERANGE;                              /* [한국어] 정수만 허용 */
	}

	if (split_num.negative) {
		if (split_num.significand > 2147483648) { /* abs(INT32_MIN) */
			return -ERANGE;
		}
		*num = (int32_t) - (int64_t)split_num.significand; /* [한국어] int64를 거쳐 부호 적용 — UB 회피 */
		return 0;
	}

	/* positive */
	if (split_num.significand > INT32_MAX) {
		return -ERANGE;
	}
	*num = (int32_t)split_num.significand;
	return 0;
}

/* [한국어] spdk_json_number_to_uint32 — uint8/16과 동일한 패턴. */
int
spdk_json_number_to_uint32(const struct spdk_json_val *val, uint32_t *num)
{
	struct spdk_json_num split_num;
	int rc;

	rc = json_number_split(val, &split_num);
	if (rc) {
		return rc;
	}

	if (split_num.exponent || split_num.negative) {
		return -ERANGE;
	}

	if (split_num.significand > UINT32_MAX) {
		return -ERANGE;
	}
	*num = (uint32_t)split_num.significand;
	return 0;
}

/* [한국어] spdk_json_number_to_uint64 — significand가 곧 uint64 결과(정확한 정수일 때).
 * 64비트는 사이즈 체크 불필요(significand가 uint64이므로 자체 보장). */
int
spdk_json_number_to_uint64(const struct spdk_json_val *val, uint64_t *num)
{
	struct spdk_json_num split_num;
	int rc;

	rc = json_number_split(val, &split_num);
	if (rc) {
		return rc;
	}

	if (split_num.exponent || split_num.negative) {
		return -ERANGE;
	}

	*num = split_num.significand;
	return 0;
}

/*
 * [한국어]
 * _json_decode_object - 객체 토큰을 decoder 표 기반으로 C struct에 매핑(strict/relaxed 공통 본체).
 *
 * @values: _OBJECT_BEGIN 토큰을 가리키는 포인터.
 * @decoders: (name, offset, decode_func, optional) 4-tuple 배열.
 * @num_decoders: 배열 크기.
 * @out: 결과 C struct 포인터(offset은 out에서의 byte offset).
 * @relaxed: true면 모르는 키를 무시(json-rpc 호환), false면 실패.
 * @return: 0(성공) / -1(실패: 타입/키/디코드/필수 누락 등).
 *
 * 동작:
 *   1) seen[]을 calloc해 각 디코더가 매치되었는지 추적(중복 키/누락 검출).
 *   2) 객체 안의 (NAME, VALUE) 쌍을 순회 — i는 객체 시작에서의 토큰 오프셋.
 *      한 쌍의 다음 위치 = 1(NAME) + spdk_json_val_len(v)(VALUE 트리 전체 길이).
 *   3) 각 NAME을 decoder.name과 비교, 일치하면 dec->decode_func(v, field) 호출.
 *   4) relaxed=false에서 매치 안 된 키 / 필수 필드 누락은 invalid 플래그.
 *
 * 호출 체인: spdk_json_decode_object/_relaxed → _json_decode_object → 사용자 decode_func.
 *
 * 메모리: seen[] 한 번 calloc/free. 호출 빈도는 RPC 한 번당 1~수회로 적음.
 */
static int
_json_decode_object(const struct spdk_json_val *values,
		    const struct spdk_json_object_decoder *decoders, size_t num_decoders, void *out, bool relaxed)
{
	uint32_t i;                          /* [한국어] 객체 내부 토큰 인덱스 진행 */
	bool invalid = false;                 /* [한국어] 누적 에러 플래그(early return 대신 모두 채우고 마지막에 보고) */
	size_t decidx;                        /* [한국어] decoder 인덱스 */
	bool *seen;                           /* [한국어] 각 decoder가 한 번이라도 매치되었는지 — 중복/필수 검출 */

	if (values == NULL || values->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
		return -1;                        /* [한국어] 객체가 아니면 즉시 실패 */
	}

	seen = calloc(sizeof(bool), num_decoders); /* [한국어] 0으로 초기화된 시각 배열 */
	if (seen == NULL) {
		return -1;                        /* [한국어] OOM */
	}

	for (i = 0; i < values->len;) {       /* [한국어] values->len = 객체 내부 자식 토큰 수(parser가 _BEGIN.len에 기록) */
		const struct spdk_json_val *name = &values[i + 1];   /* [한국어] +1: _BEGIN 다음 첫 NAME */
		const struct spdk_json_val *v = &values[i + 2];      /* [한국어] NAME 다음이 VALUE */
		bool found = false;                                   /* [한국어] 이 (NAME,VALUE)가 어떤 decoder에 매치되었는지 */

		for (decidx = 0; decidx < num_decoders; decidx++) {
			const struct spdk_json_object_decoder *dec = &decoders[decidx];
			if (spdk_json_strequal(name, dec->name)) {
				void *field = (void *)((uintptr_t)out + dec->offset); /* [한국어] out 기준 offset에 위치한 멤버 주소 */

				found = true;

				if (seen[decidx]) {
					/* duplicate field name */
					invalid = true;                                    /* [한국어] 같은 키 두 번 등장 — JSON 자체가 모호 */
					SPDK_JSON_DEBUG("Duplicate key '%s'\n", dec->name);
				} else {
					seen[decidx] = true;
					if (dec->decode_func(v, field)) {
						invalid = true;
						SPDK_JSON_DEBUG("Decoder failed to decode key '%s'\n", dec->name);
						/* keep going to fill out any other valid keys */
						/* [한국어] 부분 실패라도 다른 필드는 채워서 디버깅 메시지 풍부화 */
					}
				}
				break;                                                  /* [한국어] 매칭 1번이면 다음 (NAME,VALUE)로 */
			}
		}

		if (!relaxed && !found) {
			invalid = true;
			SPDK_JSON_DEBUG("Decoder not found for key '%.*s'\n", name->len, (char *)name->start);
			/* [한국어] strict 모드: 모르는 키는 거부(스키마 일치 보장). relaxed: 무시. */
		}

		i += 1 + spdk_json_val_len(v);   /* [한국어] 다음 (NAME,VALUE) 쌍으로 점프 — VALUE가 객체/배열이면 그 안 전체를 건너뜀 */
	}

	for (decidx = 0; decidx < num_decoders; decidx++) {
		if (!decoders[decidx].optional && !seen[decidx]) {
			/* required field is missing */
			invalid = true;
			break;                         /* [한국어] 첫 번째 누락만 보고하면 충분 */
		}
	}

	free(seen);
	return invalid ? -1 : 0;
}

/*
 * [한국어]
 * spdk_json_free_object - decoder 표를 그대로 사용해 obj 안의 동적 자원을 해제.
 *
 * @decoders/@num_decoders: 디코드 시 사용한 표.
 * @obj: 해제할 C struct 포인터.
 *
 * 트릭: type=SPDK_JSON_VAL_INVALID인 가짜 토큰을 만들어 각 decode_func에 다시 넘긴다.
 *   spdk_json_decode_string 등은 invalid 토큰을 받으면 "기존 *s를 free하고 NULL 대입"하도록
 *   설계되어 있어, 별도 "free_func" 표를 만들지 않고도 자원 해제를 할 수 있다.
 *   호출자는 디코드 실패/객체 폐기 시점에 본 함수를 호출.
 */
void
spdk_json_free_object(const struct spdk_json_object_decoder *decoders, size_t num_decoders,
		      void *obj)
{
	struct spdk_json_val invalid_val = {
		.start = "",
		.len = 0,
		.type = SPDK_JSON_VAL_INVALID    /* [한국어] decoder가 'free 시그널'로 인식하는 magic value */
	};
	size_t decidx;

	for (decidx = 0; decidx < num_decoders; decidx++) {
		const struct spdk_json_object_decoder *dec = &decoders[decidx];
		void *field = (void *)((uintptr_t)obj + dec->offset);

		/* decoding an invalid value will free the
		 * previous memory without allocating it again.
		 */
		dec->decode_func(&invalid_val, field);
	}
}


/* [한국어] spdk_json_decode_object — strict 모드 wrapper(모르는 키 거부). */
int
spdk_json_decode_object(const struct spdk_json_val *values,
			const struct spdk_json_object_decoder *decoders, size_t num_decoders, void *out)
{
	return _json_decode_object(values, decoders, num_decoders, out, false);
}

/* [한국어] spdk_json_decode_object_relaxed — relaxed 모드 wrapper(모르는 키 허용/무시).
 * 사용처: 향후 RPC 확장으로 클라이언트가 새 키를 보내도 옛 서버가 깨지지 않게 하는 호환성. */
int
spdk_json_decode_object_relaxed(const struct spdk_json_val *values,
				const struct spdk_json_object_decoder *decoders, size_t num_decoders, void *out)
{
	return _json_decode_object(values, decoders, num_decoders, out, true);
}

/*
 * [한국어]
 * spdk_json_decode_array - 배열 토큰을 C 배열로 디코드. 원소 1개당 decode_func 1번 호출.
 *
 * @values: _ARRAY_BEGIN 토큰.
 * @decode_func: 원소 1개를 변환하는 콜백(예: spdk_json_decode_uint32, spdk_json_decode_string).
 * @out: 출력 C 배열의 시작 포인터.
 * @max_size: 출력 배열 용량(원소 개수). 입력이 더 많으면 -1.
 * @out_size: out — 실제 디코드한 원소 개수.
 * @stride: 원소 간 byte 간격(sizeof(원소 타입)). 호출자가 "구조체 배열의 한 멤버"를 원소로
 *          쓰는 경우에도 stride에 sizeof(struct)를 주면 동작.
 * @return: 0(성공) / -1(타입 불일치 / 용량 초과 / 원소 디코드 실패).
 *
 * 호출 체인: spdk_json_decode_object → 사용자 정의 decode_func → spdk_json_decode_array.
 */
int
spdk_json_decode_array(const struct spdk_json_val *values, spdk_json_decode_fn decode_func,
		       void *out, size_t max_size, size_t *out_size, size_t stride)
{
	uint32_t i;                              /* [한국어] 입력 토큰 인덱스 */
	char *field;                             /* [한국어] 출력 포인터(byte 단위 진행을 위해 char*) */

	if (values == NULL || values->type != SPDK_JSON_VAL_ARRAY_BEGIN) {
		return -1;                            /* [한국어] 배열이 아니면 거부 */
	}

	*out_size = 0;
	field = out;
	for (i = 0; i < values->len;) {           /* [한국어] 배열 _BEGIN의 len = 자식 토큰 개수 */
		const struct spdk_json_val *v = &values[i + 1]; /* [한국어] +1: _BEGIN 건너뛰기 */

		if (*out_size == max_size) {
			return -1;                        /* [한국어] 용량 초과 — 호출자에게 truncation 알림 */
		}

		if (decode_func(v, field)) {
			return -1;                        /* [한국어] 한 원소라도 실패하면 전체 실패 */
		}

		i += spdk_json_val_len(v);            /* [한국어] 다음 원소로 점프(중첩 컨테이너 한 단위 통째로 건너뜀) */
		field += stride;                       /* [한국어] 출력 포인터 stride만큼 진행 */
		(*out_size)++;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_json_decode_bool - TRUE/FALSE 토큰을 C bool로.
 * @val/@out: 토큰/포인터(bool *). @return: 0/-1.
 */
int
spdk_json_decode_bool(const struct spdk_json_val *val, void *out)
{
	bool *f = out;

	if (val->type != SPDK_JSON_VAL_TRUE && val->type != SPDK_JSON_VAL_FALSE) {
		return -1;                            /* [한국어] 다른 타입이면 거부(0/1로 자동변환 금지) */
	}

	*f = val->type == SPDK_JSON_VAL_TRUE;
	return 0;
}

/* [한국어] spdk_json_decode_uint8 — number_to_uint8 wrapper. void*는 spdk_json_decode_fn 시그니처 통일을 위함. */
int
spdk_json_decode_uint8(const struct spdk_json_val *val, void *out)
{
	uint8_t *i = out;

	return spdk_json_number_to_uint8(val, i);
}

/* [한국어] spdk_json_decode_uint16 — number_to_uint16 wrapper. */
int
spdk_json_decode_uint16(const struct spdk_json_val *val, void *out)
{
	uint16_t *i = out;

	return spdk_json_number_to_uint16(val, i);
}

/* [한국어] spdk_json_decode_int32 — number_to_int32 wrapper. */
int
spdk_json_decode_int32(const struct spdk_json_val *val, void *out)
{
	int32_t *i = out;

	return spdk_json_number_to_int32(val, i);
}

/* [한국어] spdk_json_decode_uint32 — number_to_uint32 wrapper. */
int
spdk_json_decode_uint32(const struct spdk_json_val *val, void *out)
{
	uint32_t *i = out;

	return spdk_json_number_to_uint32(val, i);
}

/* [한국어] spdk_json_decode_uint64 — number_to_uint64 wrapper. */
int
spdk_json_decode_uint64(const struct spdk_json_val *val, void *out)
{
	uint64_t *i = out;

	return spdk_json_number_to_uint64(val, i);
}

/*
 * [한국어]
 * spdk_json_decode_string - STRING/NAME 토큰을 malloc 된 NUL-종결 char*로.
 *
 * @val: 토큰. INVALID 타입이면 strdup이 NULL을 반환해 *s를 NULL로 두고 -1을 반환 →
 *       spdk_json_free_object의 "free 시그널" 패턴과 자연스럽게 결합한다(기존 *s만 free되고 끝).
 * @out: char ** — 호출자의 char* 변수 주소.
 * @return: 0/-1.
 *
 * 핵심 시멘틱: 매번 호출 시 free(*s) → 즉, 같은 필드를 여러 번 디코드해도 메모리 누수 없음.
 */
int
spdk_json_decode_string(const struct spdk_json_val *val, void *out)
{
	char **s = out;

	free(*s);                                  /* [한국어] 이전 값 해제 — 재디코드/free 호환 */

	*s = spdk_json_strdup(val);                /* [한국어] INVALID 토큰이면 NULL */

	if (*s) {
		return 0;
	} else {
		return -1;
	}
}

/*
 * [한국어]
 * spdk_json_decode_uuid - UUID 문자열 토큰을 spdk_uuid 바이너리로.
 *
 * @val: STRING 토큰("xxxxxxxx-xxxx-..." 형태).
 * @out: struct spdk_uuid *.
 *
 * 호출 흐름: 일단 char*로 dup → spdk_uuid_parse → 임시 char* free.
 * 호출 체인: 사용자 decoder 표 → spdk_json_decode_uuid → spdk_uuid_parse(lib/util/uuid.c).
 */
int
spdk_json_decode_uuid(const struct spdk_json_val *val, void *out)
{
	struct spdk_uuid *uuid = out;
	char *str = NULL;
	int rc;

	rc = spdk_json_decode_string(val, &str);   /* [한국어] 문자열 복제 — 임베디드 NUL 거부 포함 */
	if (rc != 0) {
		return rc;
	}

	rc = spdk_uuid_parse(uuid, str);            /* [한국어] UUID 텍스트 파싱(SPDK util) */
	free(str);

	return rc == 0 ? 0 : -1;                    /* [한국어] 파싱 결과를 -1로 normalize */
}

/*
 * [한국어]
 * json_first - 컨테이너의 "첫 자식 토큰"을 반환(빈 컨테이너면 NULL).
 *
 * @object: _OBJECT_BEGIN 또는 _ARRAY_BEGIN 토큰 포인터.
 * @type: 허용할 컨테이너 비트마스크(둘 중 하나/둘 다).
 * @return: 첫 자식 토큰, 또는 NULL(타입 불일치/빈 컨테이너).
 */
static struct spdk_json_val *
json_first(struct spdk_json_val *object, enum spdk_json_val_type type)
{
	/* 'object' must be JSON object or array. 'type' might be combination of these two. */
	assert((type & (SPDK_JSON_VAL_ARRAY_BEGIN | SPDK_JSON_VAL_OBJECT_BEGIN)) != 0); /* [한국어] 호출자 인자 검증 */

	assert(object != NULL);

	if ((object->type & type) == 0) {
		return NULL;                              /* [한국어] 컨테이너 타입 불일치 */
	}

	object++;                                      /* [한국어] _BEGIN 다음 토큰이 첫 자식 */
	if (object->len == 0) {
		return NULL;                              /* [한국어] 빈 본문(이 위치는 실제로 _END일 수도) */
	}

	return object;
}

/*
 * [한국어]
 * json_value - NAME 토큰에서 짝이 되는 VALUE 토큰을 반환(JSON에서는 NAME 바로 다음).
 */
static struct spdk_json_val *
json_value(struct spdk_json_val *key)
{
	return key->type == SPDK_JSON_VAL_NAME ? key + 1 : NULL; /* [한국어] NAME이 아니면 짝이 없음 */
}

/*
 * [한국어]
 * spdk_json_find - 객체에서 특정 이름의 키를 검색하고 (선택) 타입 검증.
 *
 * @object: _OBJECT_BEGIN 토큰 포인터.
 * @key_name: 찾을 키 이름(C 문자열).
 * @key: out — 매치된 NAME 토큰(NULL 허용).
 * @val: out — 매치된 VALUE 토큰(NULL 허용).
 * @type: 허용할 VALUE 타입 비트마스크. SPDK_JSON_VAL_ANY(=0)면 타입 무관.
 * @return: 0(찾음), -EPROTOTYPE(객체 아님), -EINVAL(중복 키), -EDOM(타입 불일치), -ENOENT(없음).
 *
 * 동작: spdk_json_next로 객체를 순회 → NAME만 골라 strequal 비교 → 중복 검출 후 타입 체크.
 *   spdk_json_decode_object와 달리 "단일 키 1회 lookup"에 최적화.
 */
int
spdk_json_find(struct spdk_json_val *object, const char *key_name, struct spdk_json_val **key,
	       struct spdk_json_val **val, enum spdk_json_val_type type)
{
	struct spdk_json_val *_key = NULL;       /* [한국어] 누적 매치 결과(중복 검출용) */
	struct spdk_json_val *_val = NULL;
	struct spdk_json_val *it_first, *it;     /* [한국어] iterator */

	assert(object != NULL);

	it_first = json_first(object, SPDK_JSON_VAL_OBJECT_BEGIN);
	if (!it_first) {
		SPDK_JSON_DEBUG("Not enclosed in {}\n");
		return -EPROTOTYPE;                   /* [한국어] 객체가 아님 — POSIX 에러 코드로 매핑 */
	}

	for (it = it_first;
	     it != NULL;
	     it = spdk_json_next(it)) {
		if (it->type != SPDK_JSON_VAL_NAME) {
			continue;                          /* [한국어] NAME 외 토큰은 건너뜀(이론상 발생 안 함, 방어적) */
		}

		if (spdk_json_strequal(it, key_name) != true) {
			continue;
		}

		if (_key) {
			SPDK_JSON_DEBUG("Duplicate key '%s'", key_name);
			return -EINVAL;                    /* [한국어] 같은 키 두 번 — 모호 */
		}

		_key = it;
		_val = json_value(_key);

		if (type != SPDK_JSON_VAL_ANY && (_val->type & type) == 0) {
			SPDK_JSON_DEBUG("key '%s' type is %#x but expected one of %#x\n", key_name, _val->type, type);
			return -EDOM;                      /* [한국어] 타입 mismatch */
		}
	}

	if (key) {
		*key = _key;                            /* [한국어] 호출자가 NULL 넘기면 출력 생략 */
	}

	if (val) {
		*val = _val;
	}

	return _val ? 0 : -ENOENT;                 /* [한국어] 끝까지 못 찾았으면 ENOENT */
}

/* [한국어] spdk_json_find_string — STRING 타입 키 검색 wrapper. */
int
spdk_json_find_string(struct spdk_json_val *object, const char *key_name,
		      struct spdk_json_val **key, struct spdk_json_val **val)
{
	return spdk_json_find(object, key_name, key, val, SPDK_JSON_VAL_STRING);
}

/* [한국어] spdk_json_find_array — 배열 타입 키 검색 wrapper. */
int
spdk_json_find_array(struct spdk_json_val *object, const char *key_name,
		     struct spdk_json_val **key, struct spdk_json_val **val)
{
	return spdk_json_find(object, key_name, key, val, SPDK_JSON_VAL_ARRAY_BEGIN);
}

/*
 * [한국어]
 * spdk_json_object_first - 객체의 첫 키(NAME) 토큰을 반환. 빈 객체면 NULL.
 *
 * 사용 예: for (it = spdk_json_object_first(obj); it; it = spdk_json_next(it)) { ... }
 */
struct spdk_json_val *
spdk_json_object_first(struct spdk_json_val *object)
{
	struct spdk_json_val *first = json_first(object, SPDK_JSON_VAL_OBJECT_BEGIN);

	/* Empty object? */
	return first && first->type != SPDK_JSON_VAL_OBJECT_END ? first : NULL; /* [한국어] {} 즉 _END 직접 도달 = 빈 객체 */
}

/*
 * [한국어]
 * spdk_json_array_first - 배열의 첫 원소 토큰을 반환. 빈 배열이면 NULL.
 */
struct spdk_json_val *
spdk_json_array_first(struct spdk_json_val *array_begin)
{
	struct spdk_json_val *first = json_first(array_begin, SPDK_JSON_VAL_ARRAY_BEGIN);

	/* Empty array? */
	return first && first->type != SPDK_JSON_VAL_ARRAY_END ? first : NULL;
}

/*
 * [한국어]
 * json_skip_object_or_array - 컨테이너 _BEGIN을 받아 매칭되는 _END "다음" 위치를 반환.
 *
 * @val: _OBJECT_BEGIN 또는 _ARRAY_BEGIN 토큰.
 * @return: _END 바로 다음의 토큰 포인터(즉 컨테이너 다음 형제). 매칭 실패 시 NULL.
 *
 * 알고리즘: 동일 타입이 등장하면 lvl++, 짝 _END면 lvl--. lvl이 0이 되거나 INVALID에 도달하면 종료.
 *   parser가 만든 spdk_json_val 배열은 마지막에 type=INVALID 토큰을 두지 않으므로 호출자는 항상
 *   잘 형성된 컨테이너를 넘긴다고 가정 — 그래도 방어적으로 INVALID 검사.
 */
static struct spdk_json_val *
json_skip_object_or_array(struct spdk_json_val *val)
{
	unsigned lvl;                                /* [한국어] 중첩 레벨 카운터 */
	enum spdk_json_val_type end_type;            /* [한국어] 매칭 _END 타입 */
	struct spdk_json_val *it;

	if (val->type == SPDK_JSON_VAL_OBJECT_BEGIN) {
		end_type = SPDK_JSON_VAL_OBJECT_END;
	} else if (val->type == SPDK_JSON_VAL_ARRAY_BEGIN) {
		end_type = SPDK_JSON_VAL_ARRAY_END;
	} else {
		SPDK_JSON_DEBUG("Expected JSON object (%#x) or array (%#x) but got %#x\n",
				SPDK_JSON_VAL_OBJECT_BEGIN, SPDK_JSON_VAL_ARRAY_BEGIN, val->type);
		return NULL;                              /* [한국어] 컨테이너가 아니면 거부 */
	}

	lvl = 1;                                      /* [한국어] _BEGIN 자기 자신 1단계 진입 */
	for (it = val + 1; it->type != SPDK_JSON_VAL_INVALID && lvl != 0; it++) {
		if (it->type == val->type) {
			lvl++;                                /* [한국어] 동일 타입 _BEGIN — 중첩 증가 */
		} else if (it->type == end_type) {
			lvl--;                                /* [한국어] 매칭 _END — 중첩 감소 */
		}
	}

	/* if lvl != 0 we have invalid JSON object */
	if (lvl != 0) {
		SPDK_JSON_DEBUG("Can't find end of object (type: %#x): lvl (%u) != 0)\n", val->type, lvl);
		it = NULL;                                /* [한국어] 잘못 형성됨 */
	}

	return it;                                    /* [한국어] _END 바로 다음 — for의 it++로 자연 스킵됨 */
}

/*
 * [한국어]
 * spdk_json_next - 토큰 하나(또는 NAME-VALUE 쌍)에서 같은 컨테이너 안 다음 형제로 이동.
 *
 * @it: 현재 토큰. 객체 안이면 NAME, 배열 안이면 값 토큰을 가리키는 게 일반적.
 * @return: 다음 형제 토큰, 또는 NULL(컨테이너 끝/INVALID).
 *
 * 동작:
 *   - NAME이면 짝 VALUE를 찾고, 그 VALUE 너머로 점프(재귀).
 *   - 단순 값 타입은 +1.
 *   - 컨테이너 _BEGIN이면 json_skip_object_or_array로 한 번에 점프.
 *   - _END/INVALID이면 NULL.
 */
struct spdk_json_val *
spdk_json_next(struct spdk_json_val *it)
{
	struct spdk_json_val *val, *next;

	switch (it->type) {
	case SPDK_JSON_VAL_NAME:
		val = json_value(it);                  /* [한국어] NAME → VALUE */
		next = spdk_json_next(val);            /* [한국어] VALUE 너머로 점프(재귀) */
		break;

	/* We are in the middle of an array - get to next entry */
	case SPDK_JSON_VAL_NULL:
	case SPDK_JSON_VAL_TRUE:
	case SPDK_JSON_VAL_FALSE:
	case SPDK_JSON_VAL_NUMBER:
	case SPDK_JSON_VAL_STRING:
		val = it + 1;                          /* [한국어] 단순 값 — 1 토큰만 차지 */
		return val;

	case SPDK_JSON_VAL_ARRAY_BEGIN:
	case SPDK_JSON_VAL_OBJECT_BEGIN:
		next = json_skip_object_or_array(it);  /* [한국어] 컨테이너 통째로 건너뜀 */
		break;

	/* Can't go to the next object if started from the end of array or object */
	case SPDK_JSON_VAL_ARRAY_END:
	case SPDK_JSON_VAL_OBJECT_END:
	case SPDK_JSON_VAL_INVALID:
		return NULL;                           /* [한국어] 끝/무효는 다음 형제가 없음 */
	default:
		assert(false);                          /* [한국어] 위에서 모든 enum 값 처리 완료 — 도달 시 버그 */
		return NULL;

	}

	/* EOF ? */
	if (next == NULL) {
		return NULL;
	}

	switch (next->type) {
	case SPDK_JSON_VAL_ARRAY_END:
	case SPDK_JSON_VAL_OBJECT_END:
	case SPDK_JSON_VAL_INVALID:
		return NULL;                            /* [한국어] 다음 토큰이 컨테이너 끝이면 형제 없음 */
	default:
		/* Next value */
		return next;
	}
}

SPDK_LOG_REGISTER_COMPONENT(json_util)
/* [한국어] json_util 컴포넌트를 SPDK 로그 시스템에 등록 — SPDK_DEBUGLOG 매크로의 토글 키.
 * 사용자: spdk_log_set_flag("json_util") 또는 RPC `log_set_flag`로 디버그 출력 활성화. */
