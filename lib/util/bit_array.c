/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK bit array 및 bit pool 자료구조 구현 (bit_array.c)
 *
 * === 파일의 역할 ===
 * 동적으로 크기를 조절할 수 있는 비트 배열(spdk_bit_array)과, 그것을 기반으로 하는
 * 비트 풀(spdk_bit_pool)을 구현한다. SPDK 곳곳에서 "다수 객체 중 어떤 것이 사용 중인지"를
 * 추적하는 용도로 사용된다 - 예: NVMe Controller의 IO Queue Pair 슬롯, bdev_io의
 * 핸들 인덱스, lvol 식별자, RPC 콜 ID, blobstore 페이지 사용 비트맵 등. 비트 배열은
 * 단순 set/get/clear/find_first_set/find_first_clear/popcount를 제공하고, 비트 풀은
 * "lowest_free_bit" 캐싱을 통해 빠른 할당/해제를 지원한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK util 라이브러리 → 자료구조 헬퍼.
 * 호출 체인:
 *   사용자 코드(예: lib/nvme, lib/bdev) → spdk_bit_array_*/spdk_bit_pool_* 공개 API
 *     → 본 파일 내부 헬퍼(bit_array_word_count, bit_array_find_first 등)
 *     → spdk_realloc/spdk_free (env_dpdk hugepage allocator)
 * 실행 컨텍스트: 호스트 유저스페이스. 주로 단일 SPDK thread/reactor 컨텍스트에서 호출
 * (보통 자료구조 소유 스레드가 정해져 있음). 락은 사용하지 않으므로 동시 수정은 호출자가 회피.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/bit_array.h, spdk/bit_pool.h(공개 API 선언),
 *   spdk/env.h(spdk_realloc/spdk_free, hugepage 정렬 할당), spdk/likely.h(분기 힌트),
 *   spdk/util.h(spdk_u32log2 등 유틸).
 * - 의존하는 자: lib/nvme/* (qpair 슬롯 추적), lib/bdev/* (bdev_io 핸들), lib/blob/*
 *   (blobstore 페이지/cluster 비트맵), 그 외 다수 컴포넌트.
 * - 데이터 흐름: 사용자가 "num_bits" 크기의 배열 요청 → spdk_realloc로 hugepage에서
 *   할당 → 64비트 워드 배열로 비트 저장 → set/clear/get/find/count 연산.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_bit_array               : 동적 비트 배열 자료구조(Flexible array member).
 * - spdk_bit_pool                : bit_array를 감싸 free 비트 인덱스를 캐싱하는 풀.
 * - spdk_bit_array_create/free   : 생성/해제(hugepage allocator 사용).
 * - spdk_bit_array_resize        : 비트 수 변경(realloc + 새 영역 0초기화 + sentinel 워드 유지).
 * - spdk_bit_array_get/set/clear : 단일 비트 접근.
 * - spdk_bit_array_find_first_set/clear : 시작 인덱스부터 첫 set/clear 비트 검색.
 * - spdk_bit_array_count_set     : popcount 합산.
 * - spdk_bit_array_store/load_mask : 외부 바이트 마스크와의 직렬화/역직렬화.
 * - spdk_bit_pool_allocate_bit/free_bit : 가장 낮은 free 비트 할당/해제(O(1) 또는 O(log n)).
 *
 * === 핵심 트릭: sentinel word ===
 * resize 시 실제 비트 영역 끝에 한 워드(0x2)를 추가로 둔다. 이는 find_first_set이
 * "0 바로 다음에 1"을 찾도록 해 경계 검사를 제거하고, find_first_clear도 마찬가지로
 * 끝에서 무조건 0 비트를 발견하도록 보장한다. 검색 결과가 bit_count 이상이면 호출자가
 * UINT32_MAX(=찾지 못함)로 변환한다.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 헤더 묶음(uint32_t/uint64_t/CHAR_BIT 등). */

#include "spdk/bit_array.h"
/* [한국어] spdk_bit_array 공개 API 선언(spdk_bit_array_create 등). */
#include "spdk/bit_pool.h"
/* [한국어] spdk_bit_pool 공개 API 선언. */
#include "spdk/env.h"
/* [한국어] spdk_realloc/spdk_free(DPDK 기반 hugepage 정렬 할당) 선언. */

#include "spdk/likely.h"
/* [한국어] spdk_likely/spdk_unlikely 분기 힌트 매크로. */
#include "spdk/util.h"
/* [한국어] spdk_u32log2(2의 로그) 등 비트/수학 유틸. */

typedef uint64_t spdk_bit_array_word;
/* [한국어] 비트 배열의 워드 타입(64비트). 64비트는 popcnt/ctz 인스트럭션과 잘 맞고
 * 대형 배열에서도 검색 효율이 좋음. 32비트로 바꾸면 매크로만 갈아끼우면 됨. */
#define SPDK_BIT_ARRAY_WORD_TZCNT(x)	(__builtin_ctzll(x))
/* [한국어] count trailing zeros: x의 최하위(LSB) 0비트 개수. x=0이면 정의되지 않음.
 * find_first_set에서 첫 1비트의 위치를 O(1)에 찾기 위해 사용. */
#define SPDK_BIT_ARRAY_WORD_POPCNT(x)	(__builtin_popcountll(x))
/* [한국어] population count: x에 set된 비트 개수. count_set에서 사용. */
#define SPDK_BIT_ARRAY_WORD_C(x)	((spdk_bit_array_word)(x))
/* [한국어] 워드 타입으로 캐스팅하는 헬퍼 매크로. 32→64비트 승격 시 부호 확장 회피. */
#define SPDK_BIT_ARRAY_WORD_BYTES	sizeof(spdk_bit_array_word)
/* [한국어] 워드 크기(바이트): 8 (64비트). */
#define SPDK_BIT_ARRAY_WORD_BITS	(SPDK_BIT_ARRAY_WORD_BYTES * 8)
/* [한국어] 워드 크기(비트): 64. */
#define SPDK_BIT_ARRAY_WORD_INDEX_SHIFT	spdk_u32log2(SPDK_BIT_ARRAY_WORD_BITS)
/* [한국어] log2(64)=6. bit_index >> 6 = word_index 변환에 사용. 0이 아닌 양의 64
 * 단위 워드 크기에서 컴파일러가 상수 폴딩하면 시프트는 6이 된다. */
#define SPDK_BIT_ARRAY_WORD_INDEX_MASK	((1u << SPDK_BIT_ARRAY_WORD_INDEX_SHIFT) - 1)
/* [한국어] 워드 내 비트 인덱스 마스크(=63). bit_index & 63 = 워드 내 비트 위치. */

struct spdk_bit_array {
	/* [한국어] 동적 비트 배열. C99 flexible array member로 끝에 워드 배열을 꽂는다.
	 * 한 객체로 헤더+데이터를 인접 할당하므로 캐시 효율이 좋고 free도 단순. */
	uint32_t bit_count;
	/* [한국어] 사용자에게 노출되는 논리적 비트 수.
	 * 설정자: spdk_bit_array_resize / 내부 초기화.
	 * 읽는 자: capacity/find/get/set/clear 등 거의 모든 연산.
	 * 값 범위: 0 .. UINT32_MAX-1 (UINT32_MAX는 "찾지 못함" 센티넬로 예약).
	 * 동기화: 호출자가 단일 스레드 액세스 보장. */
	spdk_bit_array_word words[];
	/* [한국어] 비트 데이터를 담는 가변 길이 워드 배열. 마지막 워드 다음에는 sentinel
	 * 워드(0x2)가 한 개 더 할당된다(resize에서 +SPDK_BIT_ARRAY_WORD_BYTES 추가).
	 * 설정자: set/clear/resize/load_mask.
	 * 읽는 자: get/find_first/count/store_mask.
	 * 동기화: 단일 스레드 액세스 가정. */
};

/*
 * [한국어]
 * spdk_bit_array_create - num_bits 크기의 빈 비트 배열을 생성한다.
 *
 * @num_bits: 초기 비트 개수.
 * @return:   할당된 spdk_bit_array 포인터. 실패 시 NULL.
 *
 * 내부적으로 spdk_bit_array_resize(NULL, num_bits)를 호출해 hugepage에 할당한다.
 * 실행 컨텍스트: 단일 스레드. 통상 모듈 초기화 시점.
 */
struct spdk_bit_array *
spdk_bit_array_create(uint32_t num_bits)
{
	struct spdk_bit_array *ba = NULL;
	/* [한국어] resize에 NULL을 전달하면 새로운 배열을 할당하라는 의미. */

	spdk_bit_array_resize(&ba, num_bits);
	/* [한국어] resize가 할당과 sentinel 초기화를 모두 수행. 실패 시 ba는 NULL 그대로. */

	return ba;
	/* [한국어] 성공 시 새 배열 포인터, 실패 시 NULL 반환. */
}

/*
 * [한국어]
 * spdk_bit_array_free - 비트 배열을 해제하고 호출자 포인터를 NULL로 설정.
 *
 * @bap: 비트 배열 포인터의 포인터. NULL 안전.
 * @return: 없음.
 *
 * 호출 후 *bap는 NULL이 되어 use-after-free를 일부 방지한다.
 */
void
spdk_bit_array_free(struct spdk_bit_array **bap)
{
	struct spdk_bit_array *ba;
	/* [한국어] 임시 보관용 포인터. */

	if (!bap) {
		/* [한국어] 호출자가 NULL을 넘기면 아무 것도 하지 않음(NULL-safe). */
		return;
	}

	ba = *bap;
	/* [한국어] 실제 객체 포인터를 임시에 보관. */
	*bap = NULL;
	/* [한국어] 호출자 변수를 먼저 NULL로: 이중 free 방지. */
	spdk_free(ba);
	/* [한국어] hugepage allocator를 통한 해제. ba가 NULL이어도 안전. */
}

/*
 * [한국어]
 * bit_array_word_count - num_bits를 담는 데 필요한 64비트 워드 개수 계산.
 *
 * @num_bits: 비트 개수.
 * @return:   ceil(num_bits / 64).
 */
static inline uint32_t
bit_array_word_count(uint32_t num_bits)
{
	return (num_bits + SPDK_BIT_ARRAY_WORD_BITS - 1) >> SPDK_BIT_ARRAY_WORD_INDEX_SHIFT;
	/* [한국어] (n + 63) >> 6 = ceil(n/64). 정수 산술로 ceil 구현. */
}

/*
 * [한국어]
 * bit_array_word_mask - 워드의 하위 num_bits 비트만 1로 채운 마스크 생성.
 *
 * @num_bits: 0 .. 63 (assert로 강제).
 * @return:   (1 << num_bits) - 1.
 *
 * resize에서 마지막(부분) 워드의 미사용 비트를 0으로 클리어할 때 사용.
 */
static inline spdk_bit_array_word
bit_array_word_mask(uint32_t num_bits)
{
	assert(num_bits < SPDK_BIT_ARRAY_WORD_BITS);
	/* [한국어] num_bits == 64는 정의되지 않은 시프트(UB)를 유발하므로 < 64 보장. */
	return (SPDK_BIT_ARRAY_WORD_C(1) << num_bits) - 1;
	/* [한국어] 하위 num_bits 비트만 1로 set된 워드. */
}

/*
 * [한국어]
 * spdk_bit_array_resize - 비트 배열의 크기를 num_bits로 변경(생성 포함).
 *
 * @bap:       기존 비트 배열 포인터의 포인터. NULL이면 새로 할당.
 * @num_bits:  새로운 비트 수(0 .. UINT32_MAX-1).
 * @return:    0 성공, -EINVAL bap NULL이거나 num_bits==UINT32_MAX, -ENOMEM 할당 실패.
 *
 * 동작:
 *  1) num_bits를 워드 단위 크기로 환산하고 sentinel 워드(+8B)를 추가한 크기로 realloc.
 *  2) sentinel 워드를 0x2로 설정(LSB=0, bit1=1) → find_first_set/clear가
 *     끝에서 안전하게 종료될 수 있도록.
 *  3) 확장된 영역(있다면)은 0으로 초기화, 축소된 경우 마지막(부분) 워드의
 *     남은 비트를 마스크로 클리어.
 *  4) bit_count 갱신, *bap 갱신.
 *
 * 호출 체인: spdk_bit_array_create → [resize], 또는 사용자가 직접 호출.
 * 실행 컨텍스트: 단일 스레드. realloc 동안 다른 스레드의 접근 금지.
 */
int
spdk_bit_array_resize(struct spdk_bit_array **bap, uint32_t num_bits)
{
	struct spdk_bit_array *new_ba;
	/* [한국어] realloc 결과 새 객체. */
	uint32_t old_word_count, new_word_count;
	/* [한국어] 변경 전/후 워드 개수. */
	size_t new_size;
	/* [한국어] realloc에 전달할 총 바이트 크기(헤더 + words + sentinel). */

	/*
	 * Max number of bits allowed is UINT32_MAX - 1, because we use UINT32_MAX to denote
	 * when a set or cleared bit cannot be found.
	 */
	if (!bap || num_bits == UINT32_MAX) {
		/* [한국어] UINT32_MAX는 "찾지 못함" 센티넬로 예약 → 비트 수로 허용 안 함. */
		return -EINVAL;
	}

	new_word_count = bit_array_word_count(num_bits);
	/* [한국어] num_bits를 담는 데 필요한 64비트 워드 수 계산. */
	new_size = offsetof(struct spdk_bit_array, words) + new_word_count * SPDK_BIT_ARRAY_WORD_BYTES;
	/* [한국어] 헤더(bit_count) + 워드 영역의 총 크기. */

	/*
	 * Always keep one extra word with a 0 and a 1 past the actual required size so that the
	 * find_first functions can just keep going until they match.
	 */
	new_size += SPDK_BIT_ARRAY_WORD_BYTES;
	/* [한국어] sentinel 워드(8B)를 위한 추가 공간. find_first_*의 종료 조건 단순화 트릭. */

	new_ba = (struct spdk_bit_array *)spdk_realloc(*bap, new_size, 64);
	/* [한국어] hugepage 정렬(=64B) realloc. 기존 데이터는 보존되며 부족하면 새 영역 추가. */
	if (!new_ba) {
		/* [한국어] 메모리 부족 - 호출자에게 -ENOMEM. */
		return -ENOMEM;
	}

	/*
	 * Set up special extra word (see above comment about find_first_clear).
	 *
	 * This is set to 0b10 so that find_first_clear will find a 0 at the very first
	 * bit past the end of the buffer, and find_first_set will find a 1 at the next bit
	 * past that.
	 */
	new_ba->words[new_word_count] = 0x2;
	/* [한국어] sentinel = 0b10. 워드 인덱스 new_word_count가 영역 외 sentinel 위치이며,
	 * 비트 0(=0)을 find_first_clear가 찾고, 비트 1(=1)을 find_first_set이 찾는다. */

	if (*bap == NULL) {
		/* [한국어] 새로 할당된 경우: bit_count는 미초기화 메모리이므로 0으로 초기화 필요. */
		old_word_count = 0;
		new_ba->bit_count = 0;
	} else {
		/* [한국어] 기존 객체를 확장/축소: bit_count 필드는 보존되어 있음. */
		old_word_count = bit_array_word_count(new_ba->bit_count);
	}

	if (new_word_count > old_word_count) {
		/* Zero out new entries */
		memset(&new_ba->words[old_word_count], 0,
		       (new_word_count - old_word_count) * SPDK_BIT_ARRAY_WORD_BYTES);
		/* [한국어] 확장 영역을 0으로 채움 - 새 비트들은 모두 clear 상태로 시작. */
	} else if (new_word_count == old_word_count && num_bits < new_ba->bit_count) {
		/* Make sure any existing partial last word is cleared beyond the new num_bits. */
		uint32_t last_word_bits;
		spdk_bit_array_word mask;

		last_word_bits = num_bits & SPDK_BIT_ARRAY_WORD_INDEX_MASK;
		/* [한국어] 새 마지막 워드에서 유효한 비트 개수(0..63). */
		mask = bit_array_word_mask(last_word_bits);
		/* [한국어] 유효 비트만 1인 마스크 생성. */
		new_ba->words[old_word_count - 1] &= mask;
		/* [한국어] 마지막 워드의 미사용 상위 비트를 클리어. count_set 등에서 잘못 카운트되는 것 방지. */
	}

	new_ba->bit_count = num_bits;
	/* [한국어] 새로운 논리 비트 수 기록. */
	*bap = new_ba;
	/* [한국어] 호출자 변수에 갱신된 객체 포인터 반영. */
	return 0;
}

/*
 * [한국어]
 * spdk_bit_array_capacity - 현재 비트 배열의 비트 개수 반환.
 */
uint32_t
spdk_bit_array_capacity(const struct spdk_bit_array *ba)
{
	return ba->bit_count;
	/* [한국어] 단순 필드 반환. */
}

/*
 * [한국어]
 * bit_array_get_word - bit_index를 (워드 인덱스, 워드 내 비트 인덱스)로 분해하고 범위 검사.
 *
 * @ba:              비트 배열.
 * @bit_index:       0 .. bit_count-1.
 * @word_index:      [out] bit_index / 64.
 * @word_bit_index:  [out] bit_index % 64.
 * @return:          0 성공, -EINVAL 범위 초과.
 */
static inline int
bit_array_get_word(const struct spdk_bit_array *ba, uint32_t bit_index,
		   uint32_t *word_index, uint32_t *word_bit_index)
{
	if (spdk_unlikely(bit_index >= ba->bit_count)) {
		/* [한국어] 범위 외 접근 - 호출자에 -EINVAL 반환. spdk_unlikely는 정상 경로 최적화 힌트. */
		return -EINVAL;
	}

	*word_index = bit_index >> SPDK_BIT_ARRAY_WORD_INDEX_SHIFT;
	/* [한국어] bit_index / 64 (시프트로 빠르게). */
	*word_bit_index = bit_index & SPDK_BIT_ARRAY_WORD_INDEX_MASK;
	/* [한국어] bit_index % 64 (& 63). */

	return 0;
}

/*
 * [한국어]
 * spdk_bit_array_get - bit_index 위치의 비트 값 반환.
 *
 * @ba:        비트 배열.
 * @bit_index: 조회할 비트 인덱스.
 * @return:    true=set, false=clear 또는 범위 외.
 *
 * 주의: 범위 외 인덱스도 false로 반환(에러 구분 없음). 호출자가 범위를 알고 있어야 함.
 */
bool
spdk_bit_array_get(const struct spdk_bit_array *ba, uint32_t bit_index)
{
	uint32_t word_index, word_bit_index;
	/* [한국어] 워드 분해 결과 변수. */

	if (bit_array_get_word(ba, bit_index, &word_index, &word_bit_index)) {
		/* [한국어] 범위 외 접근은 false 반환 - 패딩/sentinel은 0으로 가정. */
		return false;
	}

	return (ba->words[word_index] >> word_bit_index) & 1U;
	/* [한국어] 해당 워드를 비트 위치만큼 오른쪽 시프트 후 LSB 추출 - 0 또는 1. */
}

/*
 * [한국어]
 * spdk_bit_array_set - bit_index 위치의 비트를 1로 설정.
 *
 * @return: 0 성공, -EINVAL 범위 외.
 */
int
spdk_bit_array_set(struct spdk_bit_array *ba, uint32_t bit_index)
{
	uint32_t word_index, word_bit_index;
	/* [한국어] 워드 분해 결과. */

	if (bit_array_get_word(ba, bit_index, &word_index, &word_bit_index)) {
		/* [한국어] 범위 외는 명시적 에러 반환(set은 의미 있는 실패이므로 clear와 다른 동작). */
		return -EINVAL;
	}

	ba->words[word_index] |= (SPDK_BIT_ARRAY_WORD_C(1) << word_bit_index);
	/* [한국어] 해당 비트를 OR로 set. 다른 비트는 영향 없음. */
	return 0;
}

/*
 * [한국어]
 * spdk_bit_array_clear - bit_index 위치의 비트를 0으로 클리어.
 *
 * 범위 외 인덱스는 no-op(에러 아님) - "끝 너머는 이미 0"으로 간주하는 단순 의미론.
 */
void
spdk_bit_array_clear(struct spdk_bit_array *ba, uint32_t bit_index)
{
	uint32_t word_index, word_bit_index;
	/* [한국어] 워드 분해 결과. */

	if (bit_array_get_word(ba, bit_index, &word_index, &word_bit_index)) {
		/*
		 * Clearing past the end of the bit array is a no-op, since bit past the end
		 * are implicitly 0.
		 */
		/* [한국어] 범위 외 clear는 무시 - 의미상 안전한 no-op. */
		return;
	}

	ba->words[word_index] &= ~(SPDK_BIT_ARRAY_WORD_C(1) << word_bit_index);
	/* [한국어] 해당 비트만 0으로(AND with NOT mask), 다른 비트는 보존. */
}

/*
 * [한국어]
 * bit_array_find_first - start_bit_index부터 첫 번째 set/clear 비트 위치를 찾는다.
 *
 * @ba:                비트 배열.
 * @start_bit_index:   탐색 시작 인덱스.
 * @xor_mask:          0(=set 검색) 또는 ~0(=clear 검색)을 워드와 XOR해 set 검색으로 통일.
 * @return:            찾은 비트 인덱스. 못 찾으면 ba->bit_count 이상의 값(센티넬 인덱스).
 *
 * 워드 단위로 점프하며 ctz 인스트럭션으로 첫 1비트를 O(1)에 찾는다.
 * sentinel 워드가 항상 0/1 비트를 모두 포함하므로 종료 검사가 필요 없다.
 */
static inline uint32_t
bit_array_find_first(const struct spdk_bit_array *ba, uint32_t start_bit_index,
		     spdk_bit_array_word xor_mask)
{
	uint32_t word_index, first_word_bit_index;
	/* [한국어] 시작 위치를 워드/비트로 분해. */
	spdk_bit_array_word word, first_word_mask;
	/* [한국어] 현재 검사 중인 워드와 첫 워드 마스크. */
	const spdk_bit_array_word *words, *cur_word;
	/* [한국어] 워드 배열 시작 포인터와 현재 포인터. */

	if (spdk_unlikely(start_bit_index >= ba->bit_count)) {
		/* [한국어] 시작이 이미 끝을 넘으면 즉시 bit_count 반환(=찾지 못함). */
		return ba->bit_count;
	}

	word_index = start_bit_index >> SPDK_BIT_ARRAY_WORD_INDEX_SHIFT;
	/* [한국어] 시작 워드 인덱스. */
	words = ba->words;
	/* [한국어] 워드 배열 시작 주소 보관(나중에 인덱스 계산용). */
	cur_word = &words[word_index];
	/* [한국어] 검사 시작 워드 포인터. */

	/*
	 * Special case for first word: skip start_bit_index % SPDK_BIT_ARRAY_WORD_BITS bits
	 * within the first word.
	 */
	first_word_bit_index = start_bit_index & SPDK_BIT_ARRAY_WORD_INDEX_MASK;
	/* [한국어] 첫 워드 내에서 건너뛸 비트 수(start_bit_index % 64). */
	first_word_mask = bit_array_word_mask(first_word_bit_index);
	/* [한국어] 건너뛸 비트만큼 1로 채운 마스크. */

	word = (*cur_word ^ xor_mask) & ~first_word_mask;
	/* [한국어] xor_mask로 set/clear 검색을 통일(set 검색이 됨), 그리고 시작 이전의 비트는
	 * ~mask로 클리어해 무시. word가 0이면 이 워드에 후보 비트가 없다는 뜻. */

	/*
	 * spdk_bit_array_resize() guarantees that an extra word with a 1 and a 0 will always be
	 * at the end of the words[] array, so just keep going until a word matches.
	 */
	while (word == 0) {
		/* [한국어] 워드에 후보가 없으면 다음 워드 검사. sentinel 덕분에 무한루프 안 됨. */
		word = *++cur_word ^ xor_mask;
		/* [한국어] 다음 워드를 xor_mask로 변환해 다시 0인지 검사. */
	}

	return ((uintptr_t)cur_word - (uintptr_t)words) * 8 + SPDK_BIT_ARRAY_WORD_TZCNT(word);
	/* [한국어] (워드 시작주소 차이 × 8) = 그 워드까지의 비트 수, + ctz(word) = 첫 1비트의 위치.
	 * 워드는 8바이트이므로 (포인터차이/8)*64 = (포인터차이)*8 (비트 단위). */
}


/*
 * [한국어]
 * spdk_bit_array_find_first_set - start_bit_index부터 첫 set(=1) 비트 인덱스를 찾는다.
 *
 * @return: 찾으면 그 인덱스, 못 찾으면 UINT32_MAX.
 */
uint32_t
spdk_bit_array_find_first_set(const struct spdk_bit_array *ba, uint32_t start_bit_index)
{
	uint32_t bit_index;
	/* [한국어] 검색 결과 임시 보관. */

	bit_index = bit_array_find_first(ba, start_bit_index, 0);
	/* [한국어] xor_mask=0이면 set 검색(워드를 그대로 검사). */

	/*
	 * If we ran off the end of the array and found the 1 bit in the extra word,
	 * return UINT32_MAX to indicate no actual 1 bits were found.
	 */
	if (bit_index >= ba->bit_count) {
		/* [한국어] sentinel에서 발견된 가짜 1 비트 → 사용자 관점에서는 못 찾음. */
		bit_index = UINT32_MAX;
	}

	return bit_index;
}

/*
 * [한국어]
 * spdk_bit_array_find_first_clear - start_bit_index부터 첫 clear(=0) 비트 인덱스를 찾는다.
 *
 * @return: 찾으면 그 인덱스, 못 찾으면 UINT32_MAX.
 */
uint32_t
spdk_bit_array_find_first_clear(const struct spdk_bit_array *ba, uint32_t start_bit_index)
{
	uint32_t bit_index;
	/* [한국어] 검색 결과 임시 보관. */

	bit_index = bit_array_find_first(ba, start_bit_index, SPDK_BIT_ARRAY_WORD_C(-1));
	/* [한국어] xor_mask=~0이면 워드 비트를 반전해 clear 검색을 set 검색으로 통일. */

	/*
	 * If we ran off the end of the array and found the 0 bit in the extra word,
	 * return UINT32_MAX to indicate no actual 0 bits were found.
	 */
	if (bit_index >= ba->bit_count) {
		/* [한국어] sentinel에서 발견된 가짜 0 비트 → 사용자 관점에서 못 찾음. */
		bit_index = UINT32_MAX;
	}

	return bit_index;
}

/*
 * [한국어]
 * spdk_bit_array_count_set - 전체 배열에서 set 비트 개수를 popcount로 합산.
 *
 * @return: set 비트 수.
 */
uint32_t
spdk_bit_array_count_set(const struct spdk_bit_array *ba)
{
	const spdk_bit_array_word *cur_word = ba->words;
	/* [한국어] 워드 배열 시작. */
	uint32_t word_count = bit_array_word_count(ba->bit_count);
	/* [한국어] 처리할 워드 개수. */
	uint32_t set_count = 0;
	/* [한국어] 누적 set 비트 카운터. */

	while (word_count--) {
		/*
		 * No special treatment is needed for the last (potentially partial) word, since
		 * spdk_bit_array_resize() makes sure the bits past bit_count are cleared.
		 */
		set_count += SPDK_BIT_ARRAY_WORD_POPCNT(*cur_word++);
		/* [한국어] popcount 인스트럭션(__builtin_popcountll → x86 popcnt/ARM cnt)으로 O(1) 합산.
		 * 마지막 부분 워드의 미사용 비트는 resize에서 0으로 보장되므로 별도 마스킹 불필요. */
	}

	return set_count;
}

/*
 * [한국어]
 * spdk_bit_array_count_clear - clear 비트 개수 = 총 비트 - set 비트.
 */
uint32_t
spdk_bit_array_count_clear(const struct spdk_bit_array *ba)
{
	return ba->bit_count - spdk_bit_array_count_set(ba);
	/* [한국어] 두 번 순회 회피를 위해 set 카운트 한 번 + 뺄셈 한 번. */
}

/*
 * [한국어]
 * spdk_bit_array_store_mask - 비트 배열을 외부 바이트 마스크로 직렬화.
 *
 * @ba:   비트 배열.
 * @mask: 출력 버퍼(최소 ceil(num_bits/8) 바이트). 호출자 소유.
 *
 * RPC/JSON에서 CPU 마스크 등을 외부에 노출할 때 사용.
 */
void
spdk_bit_array_store_mask(const struct spdk_bit_array *ba, void *mask)
{
	uint32_t size, i;
	/* [한국어] size: 통째로 복사할 바이트 수, i: 잔여 비트 인덱스. */
	uint32_t num_bits = spdk_bit_array_capacity(ba);
	/* [한국어] 비트 수 캐시. */

	size = num_bits / CHAR_BIT;
	/* [한국어] 8비트 단위로 떨어지는 바이트 수. */
	memcpy(mask, ba->words, size);
	/* [한국어] 워드 배열 메모리를 그대로 마스크로 복사(little-endian 메모리 표현 유지). */

	for (i = 0; i < num_bits % CHAR_BIT; i++) {
		/* [한국어] 8비트 단위로 떨어지지 않는 잔여 비트들을 한 비트씩 처리. */
		if (spdk_bit_array_get(ba, i + size * CHAR_BIT)) {
			((uint8_t *)mask)[size] |= (1U << i);
			/* [한국어] set이면 해당 비트만 OR로 1로 설정. */
		} else {
			((uint8_t *)mask)[size] &= ~(1U << i);
			/* [한국어] clear이면 해당 비트만 0으로(주변 잔여비트 보존). */
		}
	}
}

/*
 * [한국어]
 * spdk_bit_array_load_mask - 외부 바이트 마스크를 비트 배열에 역직렬화 적재.
 */
void
spdk_bit_array_load_mask(struct spdk_bit_array *ba, const void *mask)
{
	uint32_t size, i;
	/* [한국어] size: 바이트 단위 복사 크기, i: 잔여 비트. */
	uint32_t num_bits = spdk_bit_array_capacity(ba);
	/* [한국어] 비트 수 캐시. */

	size = num_bits / CHAR_BIT;
	/* [한국어] 8비트 떨어지는 바이트 수. */
	memcpy(ba->words, mask, size);
	/* [한국어] 마스크를 워드 배열에 그대로 적재. */

	for (i = 0; i < num_bits % CHAR_BIT; i++) {
		/* [한국어] 잔여 비트들을 한 비트씩 set/clear. */
		if (((uint8_t *)mask)[size] & (1U << i)) {
			spdk_bit_array_set(ba, i + size * CHAR_BIT);
			/* [한국어] 마스크에 1이면 set. */
		} else {
			spdk_bit_array_clear(ba, i + size * CHAR_BIT);
			/* [한국어] 0이면 clear. */
		}
	}
}

/*
 * [한국어]
 * spdk_bit_array_clear_mask - 모든 비트를 clear로 초기화.
 */
void
spdk_bit_array_clear_mask(struct spdk_bit_array *ba)
{
	uint32_t size, i;
	uint32_t num_bits = spdk_bit_array_capacity(ba);
	/* [한국어] 비트 수 캐시. */

	size = num_bits / CHAR_BIT;
	/* [한국어] 8비트 떨어지는 바이트 수. */
	memset(ba->words, 0, size);
	/* [한국어] 워드 영역을 통째로 0으로. */

	for (i = 0; i < num_bits % CHAR_BIT; i++) {
		/* [한국어] 잔여 비트들을 한 비트씩 clear(부분 워드 다른 비트 보존). */
		spdk_bit_array_clear(ba, i + size * CHAR_BIT);
	}
}

/*
 * [한국어] spdk_bit_pool: 비트 배열을 감싸 빠른 할당/해제를 지원하는 풀.
 * lowest_free_bit과 free_count 캐시를 유지해 매 호출마다 find_first_clear 전체 순회를
 * 회피한다. 사용처: NVMe qpair ID 할당, bdev_io 핸들 할당, blob ID 할당 등 "유한한
 * ID 풀에서 가장 작은 free 인덱스 할당" 패턴.
 */
struct spdk_bit_pool {
	struct spdk_bit_array	*array;
	/* [한국어] 풀의 비트 저장소(1=allocated, 0=free).
	 * 설정자: bit_pool_create/resize/load_mask, 그리고 allocate/free_bit가 set/clear.
	 * 읽는 자: is_allocated, count_*, store_mask 등.
	 * 동기화: 호출자가 단일 스레드 보장. */
	uint32_t		lowest_free_bit;
	/* [한국어] 가장 낮은 free 비트의 인덱스(없으면 UINT32_MAX).
	 * 설정자: allocate/free_bit, resize, load_mask가 갱신.
	 * 읽는 자: allocate_bit가 우선 후보로 사용.
	 * 값 범위: 0 .. capacity-1, 또는 UINT32_MAX. */
	uint32_t		free_count;
	/* [한국어] 현재 free 비트 개수.
	 * 설정자: allocate(--), free_bit(++), resize/load_mask가 재계산.
	 * 읽는 자: count_free, count_allocated. */
};

/*
 * [한국어]
 * spdk_bit_pool_create - num_bits 크기의 빈 비트 풀(전부 free) 생성.
 *
 * @return: 성공 시 풀 포인터, 실패 시 NULL.
 *
 * 내부 비트 배열을 만들고 lowest_free_bit=0, free_count=num_bits로 초기화.
 */
struct spdk_bit_pool *
spdk_bit_pool_create(uint32_t num_bits)
{
	struct spdk_bit_pool *pool = NULL;
	struct spdk_bit_array *array;
	/* [한국어] 내부 비트 배열을 임시로 가지고 있다가 풀에 소유권 이전. */

	array = spdk_bit_array_create(num_bits);
	/* [한국어] hugepage에서 비트 배열 할당. */
	if (array == NULL) {
		return NULL;
	}

	pool = calloc(1, sizeof(*pool));
	/* [한국어] 풀 헤더는 일반 힙(작고 자주 변경되지 않음)에서 0초기화 할당. */
	if (pool == NULL) {
		spdk_bit_array_free(&array);
		/* [한국어] 풀 할당 실패 시 비트 배열을 되돌려 누수 방지. */
		return NULL;
	}

	pool->array = array;
	/* [한국어] 풀이 비트 배열 소유권 인수. */
	pool->lowest_free_bit = 0;
	/* [한국어] 비트 배열은 모두 0으로 초기화되어 있으니 가장 낮은 free 비트는 0. */
	pool->free_count = num_bits;
	/* [한국어] 모든 비트가 free 상태. */

	return pool;
}

/*
 * [한국어]
 * spdk_bit_pool_create_from_array - 이미 있는 비트 배열을 풀로 감싸 생성.
 *
 * 비트 배열의 소유권은 풀로 이전된다. 풀이 free될 때 함께 해제됨.
 */
struct spdk_bit_pool *
spdk_bit_pool_create_from_array(struct spdk_bit_array *array)
{
	struct spdk_bit_pool *pool = NULL;

	pool = calloc(1, sizeof(*pool));
	if (pool == NULL) {
		/* [한국어] 풀 헤더 할당 실패 - array는 호출자가 계속 소유. */
		return NULL;
	}

	pool->array = array;
	/* [한국어] 비트 배열 소유권 인수. */
	pool->lowest_free_bit = spdk_bit_array_find_first_clear(array, 0);
	/* [한국어] 기존 배열 상태에 맞게 캐시 초기화: 첫 free 비트 검색. */
	pool->free_count = spdk_bit_array_count_clear(array);
	/* [한국어] 기존 free 비트 수 계산. */

	return pool;
}

/*
 * [한국어]
 * spdk_bit_pool_free - 풀과 내부 비트 배열을 해제하고 호출자 포인터를 NULL로.
 */
void
spdk_bit_pool_free(struct spdk_bit_pool **ppool)
{
	struct spdk_bit_pool *pool;

	if (!ppool) {
		/* [한국어] NULL-safe. */
		return;
	}

	pool = *ppool;
	*ppool = NULL;
	/* [한국어] 호출자 변수를 먼저 NULL로(이중 free 방지). */
	if (pool != NULL) {
		spdk_bit_array_free(&pool->array);
		/* [한국어] 비트 배열 해제. */
		free(pool);
		/* [한국어] 풀 헤더 해제(일반 힙). */
	}
}

/*
 * [한국어]
 * spdk_bit_pool_resize - 풀의 비트 수 변경.
 *
 * 내부 비트 배열 resize 후 lowest_free_bit/free_count 캐시 재계산.
 */
int
spdk_bit_pool_resize(struct spdk_bit_pool **ppool, uint32_t num_bits)
{
	struct spdk_bit_pool *pool;
	int rc;

	assert(ppool != NULL);
	/* [한국어] resize는 NULL ppool을 허용하지 않음(호출자 계약). */

	pool = *ppool;
	rc = spdk_bit_array_resize(&pool->array, num_bits);
	/* [한국어] 내부 비트 배열 크기 변경. */
	if (rc) {
		return rc;
	}

	pool->lowest_free_bit = spdk_bit_array_find_first_clear(pool->array, 0);
	/* [한국어] 캐시 갱신: 새로운 첫 free 비트. */
	pool->free_count = spdk_bit_array_count_clear(pool->array);
	/* [한국어] 캐시 갱신: 새로운 free 비트 수. */

	return 0;
}

/*
 * [한국어]
 * spdk_bit_pool_capacity - 풀의 총 비트 수(=비트 배열 capacity).
 */
uint32_t
spdk_bit_pool_capacity(const struct spdk_bit_pool *pool)
{
	return spdk_bit_array_capacity(pool->array);
}

/*
 * [한국어]
 * spdk_bit_pool_is_allocated - bit_index가 할당 상태인지 조회.
 */
bool
spdk_bit_pool_is_allocated(const struct spdk_bit_pool *pool, uint32_t bit_index)
{
	return spdk_bit_array_get(pool->array, bit_index);
	/* [한국어] 1=allocated, 0=free 또는 범위 외. */
}

/*
 * [한국어]
 * spdk_bit_pool_allocate_bit - 가장 낮은 free 비트를 allocate하고 그 인덱스를 반환.
 *
 * @return: 할당된 인덱스. 풀이 가득 차면 UINT32_MAX.
 *
 * lowest_free_bit 캐시 덕분에 첫 후보 결정은 O(1), 그 이후 다음 free 비트 검색은
 * find_first_clear에 의해 평균 O(워드 수). 호출자는 이 인덱스를 자기 객체와 1:1 매핑.
 */
uint32_t
spdk_bit_pool_allocate_bit(struct spdk_bit_pool *pool)
{
	uint32_t bit_index = pool->lowest_free_bit;
	/* [한국어] 캐시된 후보를 그대로 사용 - 검색 비용 0. */

	if (bit_index == UINT32_MAX) {
		/* [한국어] 풀이 가득 찬 상태 - 호출자에 실패 통지. */
		return UINT32_MAX;
	}

	spdk_bit_array_set(pool->array, bit_index);
	/* [한국어] 해당 비트를 allocated로 표시. */
	pool->lowest_free_bit = spdk_bit_array_find_first_clear(pool->array, bit_index);
	/* [한국어] 다음 free 비트 캐시 갱신(현재 위치 이후 검색). */
	pool->free_count--;
	/* [한국어] free 비트 수 1 감소. */
	return bit_index;
}

/*
 * [한국어]
 * spdk_bit_pool_set_bit_allocated - 특정 bit_index를 명시적으로 allocate.
 *
 * @return: 0 성공, -EBUSY 이미 할당됨, 또는 array_set 에러 그대로.
 *
 * 사용처: 외부 ID(예: persistent metadata에서 복원)를 풀에 등록할 때.
 */
int
spdk_bit_pool_set_bit_allocated(struct spdk_bit_pool *pool, uint32_t bit_index)
{
	int rc;

	if (spdk_bit_array_get(pool->array, bit_index)) {
		/* [한국어] 이미 1이면 다른 곳에서 점유 중 - EBUSY. */
		return -EBUSY;
	}

	rc = spdk_bit_array_set(pool->array, bit_index);
	if (rc != 0) {
		/* [한국어] 범위 외 등 set 실패는 그대로 전파. */
		return rc;
	}

	if (pool->lowest_free_bit == bit_index) {
		/* [한국어] 캐시된 후보를 막 점유했다면 다음 free 비트로 갱신. */
		pool->lowest_free_bit = spdk_bit_array_find_first_clear(pool->array, bit_index);
	}
	pool->free_count--;
	/* [한국어] free 카운트 감소. */

	return 0;
}

/*
 * [한국어]
 * spdk_bit_pool_free_bit - 점유된 비트를 해제(반납).
 *
 * 사전 조건: 해당 비트는 반드시 allocated 상태여야 함(assert).
 * 해제 후 lowest_free_bit이 더 작아질 수 있으면 갱신.
 */
void
spdk_bit_pool_free_bit(struct spdk_bit_pool *pool, uint32_t bit_index)
{
	assert(spdk_bit_array_get(pool->array, bit_index) == true);
	/* [한국어] double-free 검출: 이미 free된 비트를 free하면 풀 카운트 손상. */

	spdk_bit_array_clear(pool->array, bit_index);
	/* [한국어] 비트 클리어 = free 상태로. */
	if (pool->lowest_free_bit > bit_index) {
		/* [한국어] 더 낮은 free 후보가 생겼으면 캐시 업데이트(이번 인덱스가 새 후보). */
		pool->lowest_free_bit = bit_index;
	}
	pool->free_count++;
	/* [한국어] free 비트 수 증가. */
}

/*
 * [한국어]
 * spdk_bit_pool_count_allocated - 할당된 비트 개수 = 총 - free.
 */
uint32_t
spdk_bit_pool_count_allocated(const struct spdk_bit_pool *pool)
{
	return spdk_bit_array_capacity(pool->array) - pool->free_count;
}

/*
 * [한국어]
 * spdk_bit_pool_count_free - free 비트 개수(캐시값 직접 반환).
 */
uint32_t
spdk_bit_pool_count_free(const struct spdk_bit_pool *pool)
{
	return pool->free_count;
}

/*
 * [한국어]
 * spdk_bit_pool_store_mask - 풀의 점유 상태를 외부 마스크로 직렬화(persistent storage 등).
 */
void
spdk_bit_pool_store_mask(const struct spdk_bit_pool *pool, void *mask)
{
	spdk_bit_array_store_mask(pool->array, mask);
	/* [한국어] 단순 위임. */
}

/*
 * [한국어]
 * spdk_bit_pool_load_mask - 외부 마스크로부터 풀 상태 복원 후 캐시 재계산.
 */
void
spdk_bit_pool_load_mask(struct spdk_bit_pool *pool, const void *mask)
{
	spdk_bit_array_load_mask(pool->array, mask);
	/* [한국어] 비트 배열 적재. */
	pool->lowest_free_bit = spdk_bit_array_find_first_clear(pool->array, 0);
	/* [한국어] 캐시 재계산: 첫 free 비트. */
	pool->free_count = spdk_bit_array_count_clear(pool->array);
	/* [한국어] 캐시 재계산: free 비트 수. */
}

/*
 * [한국어]
 * spdk_bit_pool_free_all_bits - 풀의 모든 비트를 free 상태로 일괄 초기화.
 */
void
spdk_bit_pool_free_all_bits(struct spdk_bit_pool *pool)
{
	spdk_bit_array_clear_mask(pool->array);
	/* [한국어] 비트 배열 전체 0. */
	pool->lowest_free_bit = 0;
	/* [한국어] 모든 비트 free → 가장 낮은 free 인덱스는 0. */
	pool->free_count = spdk_bit_array_capacity(pool->array);
	/* [한국어] free 비트 수 = 총 비트 수. */
}
