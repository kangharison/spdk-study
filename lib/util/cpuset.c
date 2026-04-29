/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] CPU 셋(set) / 마스크 추상화 (cpuset.c)
 *
 * === 파일의 역할 ===
 * 본 파일은 SPDK 가 reactor 가 동작할 코어 집합을 표현하기 위해 사용하는
 * `struct spdk_cpuset` 의 생성/복사/논리연산/단일 CPU set/get/순회/카운트/
 * 문자열 직렬화/파싱을 구현한다. SPDK 는 시작 시 `--cpumask` 또는
 * SPDK_APP_OPTS 의 reactor_mask 인자로 어느 코어에 reactor 를 띄울지 받는데,
 * 이 입력은 두 가지 형식을 지원한다:
 *   (1) hex bitmask  : "0xff", "0x1010", "ffff,ffff" (Linux 콤마 구분 허용)
 *   (2) CPU list 표기 : "[0,2,4-7]"
 * 본 파일은 그 양쪽을 모두 파싱하고, 내부적으로는 단순 byte 배열(set->cpus)
 * 비트맵으로 저장한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/util 의 leaf 유틸리티이며 thread-agnostic, lockless. 호출 시점은 주로
 * spdk_app_start / spdk_env_init 등 초기화 단계 (단일 스레드 컨텍스트) 이며,
 * 결과는 reactor/thread/poller 배치 결정에 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/cpuset.h(공개 API 와 SPDK_CPUSET_SIZE / struct spdk_cpuset 정의),
 *         spdk/log.h(파싱 에러 로그).
 * - 사용처: lib/event(reactor 시작), lib/thread(per-thread 마스크), env_dpdk
 *           (DPDK EAL --lcores 인자 매핑), RPC 의 thread_get_pollers 등에서
 *           thread 의 cpumask 를 직렬화/역직렬화할 때.
 * - 데이터 흐름: 사용자 입력 문자열 → spdk_cpuset_parse → struct spdk_cpuset
 *               (내부 비트맵) → reactor/thread 의 affinity 결정.
 *               역방향: spdk_cpuset_fmt 로 hex 문자열 출력 (RPC 응답 등).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_cpuset_alloc/free       : 동적 할당/해제 (calloc 기반).
 * - spdk_cpuset_equal/copy       : memcmp/memcpy 의미.
 * - spdk_cpuset_negate/and/or/xor: byte-wise 논리 연산.
 * - spdk_cpuset_zero             : 모두 0.
 * - spdk_cpuset_set_cpu/get_cpu  : 단일 CPU 비트 set/get (cpu/8, cpu%8 인덱싱).
 * - spdk_cpuset_for_each_cpu/count: 모든 set 비트 순회/개수.
 * - spdk_cpuset_fmt              : 비트맵 → 소문자 hex 문자열 (set 내부 캐시 str).
 * - spdk_cpuset_parse            : "[list]" 또는 hex 문자열 → 비트맵.
 * - parse_list/parse_mask (static): 두 입력 형식의 실제 파싱 구현.
 * - hex_value (static)           : 단일 hex 문자 → 0..15 lookup. 공백/잘못된 문자
 *                                  거절을 위해 -1 인코딩 사용 (성공 시 +1 후 -1).
 *
 * struct spdk_cpuset (헤더에 정의):
 *   cpus[]: SPDK_CPUSET_SIZE/8 바이트의 비트맵.
 *   str  : fmt() 결과를 캐시하기 위한 문자열 버퍼.
 */

/* [한국어] struct spdk_cpuset 정의(SPDK_CPUSET_SIZE 등)와 본 파일의 외부 API 선언. */
#include "spdk/cpuset.h"
/* [한국어] SPDK_ERRLOG — parse_list/parse_mask 의 진단 로그용. */
#include "spdk/log.h"

/*
 * [한국어]
 * spdk_cpuset_alloc - heap 에 0 으로 초기화된 spdk_cpuset 구조체를 할당.
 *
 * @return: 성공 시 포인터, 실패 시 NULL.
 *
 * 호출자는 사용 후 spdk_cpuset_free 로 반환해야 한다.
 */
struct spdk_cpuset *
spdk_cpuset_alloc(void)
{
	/* [한국어] calloc — 메모리 0 초기화 → cpus 비트맵이 모두 0 (빈 셋) 으로 시작. */
	return (struct spdk_cpuset *)calloc(1, sizeof(struct spdk_cpuset));
}

/*
 * [한국어]
 * spdk_cpuset_free - alloc 된 spdk_cpuset 해제.
 *
 * @set: 해제 대상. NULL 도 안전(free(NULL) 보장).
 */
void
spdk_cpuset_free(struct spdk_cpuset *set)
{
	/* [한국어] libc free 위임. NULL 안전. */
	free(set);
}

/*
 * [한국어]
 * spdk_cpuset_equal - 두 cpuset 의 비트맵이 완전히 동일한지 비교.
 *
 * @set1, @set2: 비교 대상 (NULL 불가, assert).
 * @return: true 면 동일.
 *
 * str(캐시) 필드는 비교 대상이 아니며 cpus 비트맵만 본다.
 */
bool
spdk_cpuset_equal(const struct spdk_cpuset *set1, const struct spdk_cpuset *set2)
{
	/* [한국어] 사전조건: 두 셋 모두 valid 포인터여야 함. */
	assert(set1 != NULL);
	assert(set2 != NULL);
	/* [한국어] memcmp == 0 → 모든 바이트 동일. */
	return memcmp(set1->cpus, set2->cpus, sizeof(set2->cpus)) == 0;
}

/*
 * [한국어]
 * spdk_cpuset_copy - src 의 비트맵을 dst 로 복사.
 *
 * 주의: str 캐시는 복사하지 않는다 (dst 는 다음 fmt() 호출에서 새로 생성).
 */
void
spdk_cpuset_copy(struct spdk_cpuset *dst, const struct spdk_cpuset *src)
{
	assert(dst != NULL);
	assert(src != NULL);
	/* [한국어] cpus 비트맵 전체를 byte-wise 복사 — 단일 비트가 아닌 전체 셋. */
	memcpy(&dst->cpus, &src->cpus, sizeof(src->cpus));
}

/*
 * [한국어]
 * spdk_cpuset_negate - 비트맵 전체를 ~ (보수). 의미적으로 "set 의 여집합".
 */
void
spdk_cpuset_negate(struct spdk_cpuset *set)
{
	unsigned int i;
	assert(set != NULL);
	/* [한국어] 각 바이트를 ~ 로 뒤집음 — 모든 비트가 반전된다. */
	for (i = 0; i < sizeof(set->cpus); i++) {
		set->cpus[i] = ~set->cpus[i];
	}
}

/*
 * [한국어]
 * spdk_cpuset_and - 교집합 dst &= src.
 */
void
spdk_cpuset_and(struct spdk_cpuset *dst, const struct spdk_cpuset *src)
{
	unsigned int i;
	assert(dst != NULL);
	assert(src != NULL);
	/* [한국어] AND 연산 — 양쪽 모두 1 인 비트만 1 로 남음. */
	for (i = 0; i < sizeof(src->cpus); i++) {
		dst->cpus[i] &= src->cpus[i];
	}
}

/*
 * [한국어]
 * spdk_cpuset_or - 합집합 dst |= src.
 */
void
spdk_cpuset_or(struct spdk_cpuset *dst, const struct spdk_cpuset *src)
{
	unsigned int i;
	assert(dst != NULL);
	assert(src != NULL);
	/* [한국어] OR 연산 — 어느 쪽이든 1 이면 1. */
	for (i = 0; i < sizeof(src->cpus); i++) {
		dst->cpus[i] |= src->cpus[i];
	}
}

/*
 * [한국어]
 * spdk_cpuset_xor - 대칭차 dst ^= src.
 */
void
spdk_cpuset_xor(struct spdk_cpuset *dst, const struct spdk_cpuset *src)
{
	unsigned int i;
	assert(dst != NULL);
	assert(src != NULL);
	/* [한국어] XOR — 한쪽만 1 인 비트만 1 (양쪽 같으면 0). */
	for (i = 0; i < sizeof(src->cpus); i++) {
		dst->cpus[i] ^= src->cpus[i];
	}
}

/*
 * [한국어]
 * spdk_cpuset_zero - 비트맵 전체를 0 으로 초기화 (빈 셋).
 */
void
spdk_cpuset_zero(struct spdk_cpuset *set)
{
	assert(set != NULL);
	/* [한국어] memset 0 — 모든 CPU 비트가 미선택 상태로. */
	memset(set->cpus, 0, sizeof(set->cpus));
}

/*
 * [한국어]
 * spdk_cpuset_set_cpu - 단일 CPU 비트의 켜고 끄기.
 *
 * @set:   대상 cpuset.
 * @cpu:   CPU 인덱스 (0 .. SPDK_CPUSET_SIZE-1). 범위 초과 시 assert 실패.
 * @state: true 면 set, false 면 clear.
 *
 * 비트 위치 = bit `cpu % 8` of byte `cpu / 8`. byte-단위 비트맵 인코딩.
 */
void
spdk_cpuset_set_cpu(struct spdk_cpuset *set, uint32_t cpu, bool state)
{
	assert(set != NULL);
	/* [한국어] 비트맵 크기를 비트 단위로 환산 — 범위 검증. */
	assert(cpu < sizeof(set->cpus) * 8);
	if (state) {
		/* [한국어] 해당 비트만 1 로 OR-set. */
		set->cpus[cpu / 8] |= (1U << (cpu % 8));
	} else {
		/* [한국어] 해당 비트만 0 으로 AND-clear. */
		set->cpus[cpu / 8] &= ~(1U << (cpu % 8));
	}
}

/*
 * [한국어]
 * spdk_cpuset_get_cpu - 단일 CPU 비트의 set 여부 조회.
 *
 * @return: true 면 set, false 면 clear.
 */
bool
spdk_cpuset_get_cpu(const struct spdk_cpuset *set, uint32_t cpu)
{
	assert(set != NULL);
	assert(cpu < sizeof(set->cpus) * 8);
	/* [한국어] 해당 byte 를 cpu%8 만큼 우측 시프트 후 LSB 만 추출 → 0/1. */
	return (set->cpus[cpu / 8] >> (cpu % 8)) & 1U;
}

/*
 * [한국어]
 * spdk_cpuset_for_each_cpu - set 의 모든 set-비트 CPU 인덱스에 대해 fn 호출.
 *
 * @set: 대상 cpuset.
 * @fn:  콜백. (ctx, cpu_index) 형식으로 호출됨.
 * @ctx: 콜백에 그대로 전달되는 사용자 컨텍스트.
 *
 * 동시성: set 은 호출 중 변경되어서는 안 된다 (호출자가 보장).
 */
void
spdk_cpuset_for_each_cpu(const struct spdk_cpuset *set,
			 void (*fn)(void *ctx, uint32_t cpu), void *ctx)
{
	/* [한국어] 현재 byte 의 비트맵 스냅샷. */
	uint8_t n;
	unsigned int i, j;
	/* [한국어] 외곽 루프: byte 단위 (= 8 CPU 그룹). */
	for (i = 0; i < sizeof(set->cpus); i++) {
		n = set->cpus[i];
		/* [한국어] 내부 루프: 1 byte 안의 8 비트를 LSB → MSB 순으로 검사. */
		for (j = 0; j < 8; j++) {
			/* [한국어] j 번째 비트가 set 이면 콜백 호출 — CPU 인덱스 = i*8+j. */
			if (n & (1 << j)) {
				fn(ctx, i * 8 + j);
			}
		}
	}
}

/*
 * [한국어]
 * count_fn (static) - spdk_cpuset_count 가 사용하는 콜백. set 비트마다 +1.
 */
static void
count_fn(void *ctx, uint32_t cpu)
{
	/* [한국어] ctx 는 uint32_t* count 누산기. */
	uint32_t *count = ctx;

	(*count)++;
}

/*
 * [한국어]
 * spdk_cpuset_count - set 된 CPU 비트의 개수 반환 (popcount).
 *
 * 구현은 count_fn 을 통해 단순 누적. SPDK 의 cpuset 은 비트맵이 작아
 * (수십~수백 비트) 별도 popcount 최적화가 불필요하다.
 */
uint32_t
spdk_cpuset_count(const struct spdk_cpuset *set)
{
	uint32_t count = 0;

	/* [한국어] 모든 set-비트에 대해 count_fn 호출 → 결과 누적. */
	spdk_cpuset_for_each_cpu(set, count_fn, &count);
	return count;
}

/*
 * [한국어]
 * spdk_cpuset_fmt - cpuset 의 비트맵을 소문자 hex 문자열로 직렬화.
 *
 * @set:    대상. set->str 에 결과가 저장되며, 같은 포인터를 반환.
 * @return: NUL 종료 hex 문자열 (예: 비트0,1 set → "3").
 *
 * 주의: 결과는 set->str 내부 버퍼를 가리키므로 set 가 변경/해제되면 무효.
 *       lcore_max(가장 큰 set CPU) 에 해당하는 byte 부터 시작해 0 byte 까지
 *       내려오며 8비트 = 2 hex digit 로 출력. 첫 byte 의 상위 4비트가 0 이면
 *       leading zero 를 생략 (hex 표기 자연스러움).
 */
const char *
spdk_cpuset_fmt(struct spdk_cpuset *set)
{
	/* [한국어] lcore_max: set 된 CPU 인덱스 중 최댓값. lcore: 순회 변수. */
	uint32_t lcore, lcore_max = 0;
	int val, i, n;
	char *ptr;
	/* [한국어] 0..15 → '0'..'f' 매핑 — nibble 출력용. */
	static const char *hex = "0123456789abcdef";

	assert(set != NULL);

	/* [한국어] 가장 높은 set CPU 비트 찾기 — 결과 길이를 정하기 위해 필요. */
	for (lcore = 0; lcore < sizeof(set->cpus) * 8; lcore++) {
		if (spdk_cpuset_get_cpu(set, lcore)) {
			lcore_max = lcore;
		}
	}

	/* [한국어] 출력 시작 포인터를 set->str 버퍼 헤드로 설정. */
	ptr = set->str;
	/* [한국어] 출력 시작 byte 인덱스 = lcore_max / 8 (가장 높은 byte). */
	n = lcore_max / 8;
	val = set->cpus[n];

	/* Store first number only if it is not leading zero */
	/* [한국어] 첫 byte 의 상위 nibble 이 0 이면 leading zero 제거 — hex 자연 표기. */
	if ((val & 0xf0) != 0) {
		*(ptr++) = hex[(val & 0xf0) >> 4];
	}
	/* [한국어] 첫 byte 의 하위 nibble 은 항상 출력 (0 이라도 의미가 있음). */
	*(ptr++) = hex[val & 0x0f];

	/* [한국어] 나머지 byte 들은 큰 인덱스 → 작은 인덱스 순으로 (MSB-first) 2 글자씩 출력. */
	for (i = n - 1; i >= 0; i--) {
		val = set->cpus[i];
		/* [한국어] 상위 nibble. */
		*(ptr++) = hex[(val & 0xf0) >> 4];
		/* [한국어] 하위 nibble. */
		*(ptr++) = hex[val & 0x0f];
	}
	/* [한국어] C 문자열 종료. */
	*ptr = '\0';

	return set->str;
}

/*
 * [한국어]
 * hex_value (static) - 단일 hex 문자 → 0..15 변환. 무효 문자는 -1.
 *
 * 구현: 256 바이트 lookup table. 미정의 슬롯은 0(=공백, 무효 문자) 이고,
 * 정의된 슬롯에는 (값 + 1) 이 저장된다. 반환 시 -1 하여 무효 문자는 -1,
 * 정의된 문자는 0..15 가 되도록 한다 (C99 designated initializer 트릭).
 */
static int
hex_value(uint8_t c)
{
#define V(x, y) [x] = y + 1
	/* [한국어] '0'-'9', 'A'-'F', 'a'-'f' 만 (값+1) 로 채우고 나머지는 0. */
	static const int8_t val[256] = {
		V('0', 0), V('1', 1), V('2', 2), V('3', 3), V('4', 4),
		V('5', 5), V('6', 6), V('7', 7), V('8', 8), V('9', 9),
		V('A', 0xA), V('B', 0xB), V('C', 0xC), V('D', 0xD), V('E', 0xE), V('F', 0xF),
		V('a', 0xA), V('b', 0xB), V('c', 0xC), V('d', 0xD), V('e', 0xE), V('f', 0xF),
	};
#undef V

	/* [한국어] 무효 문자: 0 - 1 = -1. 유효 문자: (값+1) - 1 = 값. */
	return val[c] - 1;
}

/*
 * [한국어]
 * parse_list (static) - "[a,b-c,d]" 형태의 CPU list 표기 파싱.
 *
 * @mask:   '[' 로 시작하는 입력 문자열.
 * @set:    출력. 파싱된 비트맵.
 * @return: 0 성공, -1 실패 (잘못된 문자/순서/범위).
 *
 * 문법: '[' (item (',' item)*)? ']'
 *   item = N | N '-' M (N <= M, 모두 0 .. SPDK_CPUSET_SIZE-1)
 * 공백(blank) 은 토큰 사이에서만 허용. 빈 list "[]" 는 set 이 0 인 채 0 반환.
 */
static int
parse_list(const char *mask, struct spdk_cpuset *set)
{
	/* [한국어] strtoul 가 다음 문자 위치를 반환할 포인터. */
	char *end;
	const char *ptr = mask;
	uint32_t lcore;
	/* [한국어] 현재 진행 중인 range 의 [lcore_min, lcore_max]. UINT32_MAX 는 미설정. */
	uint32_t lcore_min, lcore_max;

	/* [한국어] 파싱 시작 시 모든 비트 0 으로 초기화. */
	spdk_cpuset_zero(set);
	lcore_min = UINT32_MAX;

	/* [한국어] 첫 문자 '[' 를 건너뜀. 이미 호출자가 검사했음. */
	ptr++;
	end = (char *)ptr;
	do {
		/* [한국어] 토큰 앞쪽 공백 건너뛰기. */
		while (isblank(*ptr)) {
			ptr++;
		}
		/* [한국어] 잘못된 위치의 종결/구분 기호 검출 — 비어있는 item 거절. */
		if (*ptr == '\0' || *ptr == ']' || *ptr == '-' || *ptr == ',') {
			goto invalid_character;
		}

		/* [한국어] 10진수 숫자 파싱. errno 를 0 으로 초기화 후 strtoul 호출. */
		errno = 0;
		lcore = strtoul(ptr, &end, 10);
		if (errno) {
			SPDK_ERRLOG("Conversion of core mask in '%s' failed\n", mask);
			return -1;
		}

		/* [한국어] 코어 번호가 비트맵 크기를 초과하면 거절. */
		if (lcore >= sizeof(set->cpus) * 8) {
			SPDK_ERRLOG("Core number %" PRIu32 " is out of range in '%s'\n", lcore, mask);
			return -1;
		}

		/* [한국어] 숫자 뒤쪽 공백 건너뜀 (예: "0  -  2"). */
		while (isblank(*end)) {
			end++;
		}

		if (*end == '-') {
			/* [한국어] range 의 시작 — 다음 숫자가 끝점이 됨. */
			lcore_min = lcore;
		} else if (*end == ',' || *end == ']') {
			/* [한국어] item 종결 — 단독 숫자 또는 range 의 끝점. */
			lcore_max = lcore;
			/* [한국어] range 시작이 없었다면 단독 숫자 → min=lcore. */
			if (lcore_min == UINT32_MAX) {
				lcore_min = lcore;
			}
			/* [한국어] 역순 range 거절. */
			if (lcore_min > lcore_max) {
				SPDK_ERRLOG("Invalid range of CPUs (%" PRIu32 " > %" PRIu32 ")\n",
					    lcore_min, lcore_max);
				return -1;
			}
			/* [한국어] [min, max] 모든 비트 set. */
			for (lcore = lcore_min; lcore <= lcore_max; lcore++) {
				spdk_cpuset_set_cpu(set, lcore, true);
			}
			/* [한국어] range 사용 완료 → 다음 item 을 위해 미설정 상태로 리셋. */
			lcore_min = UINT32_MAX;
		} else {
			/* [한국어] 그 외 문자 — 잘못된 위치. */
			goto invalid_character;
		}

		/* [한국어] 다음 item 시작 위치 = 종결자(','/']') 의 다음 문자. */
		ptr = end + 1;

	} while (*end != ']');

	return 0;

invalid_character:
	if (*end == '\0') {
		/* [한국어] 끝까지 ']' 가 없으면 형식 오류 메시지. */
		SPDK_ERRLOG("Unexpected end of core list '%s'\n", mask);
	} else {
		/* [한국어] 어느 문자에서 실패했는지 알려줌. */
		SPDK_ERRLOG("Parsing of core list '%s' failed on character '%c'\n", mask, *end);
	}
	return -1;
}

/*
 * [한국어]
 * parse_mask (static) - "ff,ff" / "0xff" / "ffffffff" 형태의 hex bitmask 파싱.
 *
 * @mask:   입력 문자열. 선택적으로 "0x"/"0X" 접두사, 콤마 구분 허용.
 * @set:    출력 비트맵.
 * @len:    공백 제거 후 mask 의 사용 가능 길이.
 * @return: 0 성공, -1 실패.
 *
 * 동작: 문자열의 마지막 문자(가장 낮은 자리수)부터 거꾸로 순회하며
 *       각 hex digit 를 4 비트로 풀어 cpu 비트맵에 세팅. Linux 의 /sys/...
 *       cpumask 가 8자리마다 콤마를 넣는 형식과 호환된다.
 */
static int
parse_mask(const char *mask, struct spdk_cpuset *set, size_t len)
{
	int i, j;
	char c;
	int val;
	/* [한국어] 현재까지 처리한 비트 인덱스 — 0 부터 시작해서 4 씩 증가. */
	uint32_t lcore = 0;

	/* [한국어] "0x"/"0X" 접두사 제거 — hex 임을 명시한 흔한 표기. */
	if (mask[0] == '0' && (mask[1] == 'x' || mask[1] == 'X')) {
		mask += 2;
		len -= 2;
	}

	/* [한국어] 결과 비트맵 초기화. */
	spdk_cpuset_zero(set);
	/* [한국어] 가장 낮은 자리수(=문자열 끝)부터 4비트씩 처리.
	 * 인덱스 i 는 mask 의 문자 위치 (큰 → 작은). */
	for (i = len - 1; i >= 0; i--) {
		c = mask[i];
		if (c == ',') {
			/* Linux puts comma delimiters in its cpumasks, just skip them. */
			/* [한국어] Linux 콤마 구분자(/sys/.../cpumask) 호환 — 스킵. */
			continue;
		}
		/* [한국어] hex 문자 → 0..15. 잘못된 문자는 -1. */
		val = hex_value(c);
		if (val < 0) {
			/* Invalid character */
			SPDK_ERRLOG("Invalid character in core mask '%s' (%c)\n", mask, c);
			return -1;
		}
		/* [한국어] 한 hex digit = 4비트. 비트 0(LSB) → 비트 3(MSB) 순으로
		 *  cpu 인덱스에 매핑. SPDK_CPUSET_SIZE 초과 비트는 무시. */
		for (j = 0; j < 4 && lcore < SPDK_CPUSET_SIZE; j++, lcore++) {
			if ((1 << j) & val) {
				spdk_cpuset_set_cpu(set, lcore, true);
			}
		}
	}

	return 0;
}

/*
 * [한국어]
 * spdk_cpuset_parse - 사용자 입력 문자열 → spdk_cpuset 비트맵 파싱.
 *
 * @set:    출력. 파싱된 결과로 채워짐.
 * @mask:   입력. "[a,b-c]" list 표기 또는 hex bitmask.
 * @return: 0 성공, -1 실패 (NULL 인자/빈 문자열/형식 오류).
 *
 * 호출 시점: spdk_app_start 의 reactor mask 파싱, RPC 의 thread cpumask 변경 등.
 */
int
spdk_cpuset_parse(struct spdk_cpuset *set, const char *mask)
{
	int ret;
	size_t len;

	/* [한국어] NULL 인자 거절. */
	if (mask == NULL || set == NULL) {
		return -1;
	}

	/* [한국어] 앞쪽 공백 트리밍. */
	while (isblank(*mask)) {
		mask++;
	}

	/* [한국어] 뒷쪽 공백 트리밍 (len 만 줄임 — 원본 문자열은 변경하지 않음). */
	len = strlen(mask);
	while (len > 0 && isblank(mask[len - 1])) {
		len--;
	}

	/* [한국어] 공백만 남는 경우 거절. */
	if (len == 0) {
		return -1;
	}

	/* [한국어] '[' 시작 → list 표기, 그 외 → hex bitmask 로 분기. */
	if (mask[0] == '[') {
		ret = parse_list(mask, set);
	} else {
		ret = parse_mask(mask, set, len);
	}

	return ret;
}
