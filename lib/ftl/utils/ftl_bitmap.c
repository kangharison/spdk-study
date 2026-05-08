/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 비트맵 구현 (ftl_bitmap.c)
 *
 * === 파일의 역할 ===
 * ftl_bitmap.h가 선언한 비트맵 API의 구현. 호출자가 사전 할당한 정렬된 buf를 unsigned long
 * 워드 배열로 보고, 비트 인덱스를 워드 인덱스 + 워드 내 비트 위치로 분해해 비트 조작/검색/카운팅을
 * 수행한다. 검색은 SIMD 같은 외부 의존 없이 GCC builtins(__builtin_ctzl, __builtin_popcountl)와
 * 64비트 워드 단위 점프만으로 충분히 빠르게 동작하도록 작성되었다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/ftl 안에서 사용되는 다양한 트래킹 자료구조의 빌딩 블록.
 * 호출 체인: ftl_band_init / ftl_p2l_map_init → ftl_bitmap_create → 이후 ftl_bitmap_set/clear/get/...
 * 실행 컨텍스트: 호스트 유저스페이스, FTL의 L2P/밴드/P2L 처리 SPDK reactor 스레드.
 *   비트맵 자체는 락-프리가 아니므로 호출자가 thread affinity로 동시 접근을 차단해야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/log.h(SPDK_ERRLOG), spdk/util.h(spdk_divide_round_up, spdk_round_up,
 *   spdk_u32log2, spdk_min), ftl_bitmap.h(공개 인터페이스),
 *   ftl_internal.h(FTL_BLOCK_SIZE = 4 KiB).
 * 의존되는 모듈: ftl_band.c, ftl_internal.h, ftl_l2p.c.
 * 데이터 흐름: 호출자가 정렬 buf 제공 → 이 모듈이 비트 단위 read/write 추상화 → 호출자는 인덱스만 다룸.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct ftl_bitmap: 내부 캡슐화된 핸들 — buf 포인터와 워드 단위 size 보유.
 *   - bitmap_word: unsigned long 별칭(타깃 ABI에 따라 32 또는 64비트).
 *   - FTL_BITMAP_WORD_SHIFT/MASK: 비트 인덱스 → 워드 인덱스/워드 내 위치 분해 매크로.
 *   - locate_bit: 비트 인덱스로부터 워드 포인터와 워드 내 비트 인덱스를 산출하는 static 인라인 헬퍼.
 *   - ftl_bitmap_bits_to_size / bits_to_blocks: 크기 변환.
 *   - ftl_bitmap_create / destroy: 핸들 생성/해제.
 *   - ftl_bitmap_get/set/clear: 단일 비트 조작.
 *   - ftl_bitmap_find_first(value): 범위 내 첫 set/clear 비트 검색 — workhorse.
 *   - ftl_bitmap_find_first_set / clear: find_first의 인자 고정 wrapper.
 *   - ftl_bitmap_count_set: 전체 set 비트 popcount 합산.
 */

#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG — buf 정렬/크기 위반 시 에러 출력에 사용. */
#include "spdk/util.h"
/* [한국어] spdk_divide_round_up, spdk_round_up, spdk_u32log2, spdk_min 등 산술 유틸. */

#include "ftl_bitmap.h"
/* [한국어] 공개 인터페이스 선언 — 구현이 인터페이스와 일치하는지 컴파일러가 검증하도록 포함. */
#include "ftl_internal.h"
/* [한국어] FTL_BLOCK_SIZE(4 KiB) 매크로 — bits_to_blocks 변환에 사용. */

typedef unsigned long bitmap_word;
/* [한국어] 비트맵 워드 타입 별칭 — 64비트 ABI에서는 8바이트, 32비트에서는 4바이트.
 * __builtin_ctzl/popcountl가 unsigned long 시그니처이므로 일치시킨다.
 * SPDK는 64비트 시스템 위주이므로 실질적으로 64비트. */

const size_t ftl_bitmap_buffer_alignment = sizeof(bitmap_word);
/* [한국어] 외부 const 변수 정의 — buf 정렬 요구사항 = sizeof(bitmap_word).
 * 헤더에서 extern 선언되어 호출자도 이 값을 참조해 buf를 할당한다.
 * 워드 단위 read/write가 정렬되어야 캐시라인/UB 회피와 popcountl 효율을 보장. */

#define FTL_BITMAP_WORD_SHIFT	spdk_u32log2(sizeof(bitmap_word) * 8)
/* [한국어] 비트 인덱스를 워드 인덱스로 변환할 때의 시프트 양 = log2(워드 비트 수).
 *  - sizeof(bitmap_word) * 8 = 64 (64비트 시스템) → log2(64) = 6.
 *  spdk_u32log2는 컴파일 타임 상수 평가가 가능하므로 매크로 비용 없음. */
#define FTL_BITMAP_WORD_MASK	(~(~0UL << FTL_BITMAP_WORD_SHIFT))
/* [한국어] 비트 인덱스의 하위 SHIFT 비트(워드 내 위치)를 추출하는 마스크.
 *  - ~0UL << 6 = 0xFFFFFFFFFFFFFFC0 → ~ 결과 = 0x000000000000003F (= 63 = 64-1).
 *  bit & MASK 연산으로 워드 내 비트 위치(0..63)를 즉시 얻는다. */

/*
 * [한국어]
 * ftl_bitmap_bits_to_size - 표현해야 할 비트 수를 정렬된 buf 바이트 크기로 변환
 *
 * @param bits: 표현할 비트 개수 (호출자가 결정).
 * @return: 정렬을 충족하는 buf 크기(바이트). 호출자는 이 크기만큼 정렬 메모리를 할당해야 한다.
 *
 * 동작 단계:
 *   1) bits가 ftl_bitmap_buffer_alignment 미만이면 최소 정렬 단위만큼 끌어올림 (작은 비트맵도 한 워드 보장).
 *   2) bits/8 올림으로 바이트 단위 변환.
 *   3) ftl_bitmap_buffer_alignment(8B) 단위로 추가 정렬.
 * 실행 컨텍스트: 산술 — 어디서든 호출 가능.
 *
 * 호출 체인:
 *   ftl_band_init / 메타데이터 할당 코드 → [이 함수]
 */
uint64_t
ftl_bitmap_bits_to_size(uint64_t bits)
{
	uint64_t size;
	/* [한국어] 결과 buf 크기(바이트) 누적 변수. */

	if (bits < ftl_bitmap_buffer_alignment) {
		/* [한국어] 비트가 너무 적으면 한 워드도 안 됨 — 최소 워드 1개 크기를 강제하여 후속 산술의 0 division 방지. */
		bits = ftl_bitmap_buffer_alignment;
		/* [한국어] 최소 8(=bitmap_word 바이트 수)로 끌어올림. 의미상 "최소 8비트는 보장". */
	}

	size = spdk_divide_round_up(bits, 8);
	/* [한국어] bits/8 올림 — 비트 수를 바이트 수로 변환. spdk_divide_round_up((bits + 7) / 8). */
	size = spdk_round_up(size, ftl_bitmap_buffer_alignment);
	/* [한국어] 바이트 크기를 8B 정렬 경계로 올림 — 워드 단위 access 안전 보장. */

	return size;
	/* [한국어] 정렬 충족 buf 크기 반환. */
}

/*
 * [한국어]
 * ftl_bitmap_bits_to_blocks - 비트 수를 FTL_BLOCK_SIZE(4 KiB) 블록 수로 변환
 *
 * @param bits: 비트 개수.
 * @return: 4 KiB 블록 단위로 올림한 개수.
 *
 * 동기/배경: 비트맵을 디스크에 영속화할 때 4 KiB 블록 단위 I/O가 필요. 이 함수가 그 변환을 통일.
 *
 * 호출 체인:
 *   메타데이터 영역 크기 결정 코드 → [이 함수]
 */
uint64_t
ftl_bitmap_bits_to_blocks(uint64_t bits)
{
	uint64_t size = ftl_bitmap_bits_to_size(bits);
	/* [한국어] 먼저 정렬 buf 크기(바이트)를 산출. */

	return spdk_divide_round_up(size, FTL_BLOCK_SIZE);
	/* [한국어] 바이트 크기를 4 KiB 블록 수로 올림 — (size + 4095) / 4096. */
}

struct ftl_bitmap {
	bitmap_word *buf;
	/* [한국어] 외부에서 제공된 워드 배열 시작 포인터.
	 * 설정자: ftl_bitmap_create. 읽는 자: 모든 비트 조작 함수.
	 * 동기화: 핸들 생성 후 read-only(가리키는 데이터는 가변). */

	size_t size;
	/* [한국어] buf의 워드 단위 길이(=바이트크기 / sizeof(bitmap_word)).
	 * 설정자: create. 읽는 자: locate_bit/find_first/count 등이 범위 검사에 사용.
	 * 값 범위: 1 이상. */
};
/* [한국어] 핸들 구조체 — .h에서 forward만 노출되므로 외부에서는 내부 필드 직접 접근 불가. */

/*
 * [한국어]
 * ftl_bitmap_create - 사전 할당된 buf 위에 ftl_bitmap 핸들을 생성
 *
 * @param buf: 호출자가 ftl_bitmap_buffer_alignment(8B) 정렬로 할당한 메모리.
 * @param size: buf 크기(바이트). 8B 배수여야 함.
 * @return: 핸들 포인터(성공) 또는 NULL(정렬/크기 위반, OOM).
 *
 * 동작:
 *   1) buf 시작 주소가 8B 정렬인지 검증.
 *   2) size가 8B 배수인지 검증.
 *   3) 핸들 구조체 calloc.
 *   4) buf 포인터 저장 + size를 워드 단위로 환산해 저장.
 * 실행 컨텍스트: FTL init mngt 단계, 단일 스레드.
 * 에러 경로: 검증 실패 시 SPDK_ERRLOG로 사유 출력 후 NULL 반환 — 호출자는 마운트 실패 처리.
 *
 * 호출 체인:
 *   ftl_band_init / ftl_p2l_map_init → [이 함수]
 */
struct ftl_bitmap *ftl_bitmap_create(void *buf, size_t size)
{
	struct ftl_bitmap *bitmap;
	/* [한국어] 결과 핸들 포인터 — 성공 시 calloc 결과, 실패 시 NULL. */

	if ((uintptr_t)buf % ftl_bitmap_buffer_alignment) {
		/* [한국어] buf 시작 주소 정렬 검사 — uintptr_t로 정수 변환 후 modular 검사.
		 * 정렬되지 않으면 워드 단위 access가 일부 ABI에서 SIGBUS를 일으킬 수 있다. */
		SPDK_ERRLOG("Buffer for bitmap must be aligned to %lu bytes\n",
			    ftl_bitmap_buffer_alignment);
		/* [한국어] 호출자에게 정렬 요구사항을 명확히 알리는 에러 로그. */
		return NULL;
		/* [한국어] 즉시 실패 반환 — 핸들 미할당. */
	}

	if (size % ftl_bitmap_buffer_alignment) {
		/* [한국어] size가 8B 배수가 아니면 워드 배열 길이가 떨어지지 않음. */
		SPDK_ERRLOG("Size of buffer for bitmap must be divisible by %lu bytes\n",
			    ftl_bitmap_buffer_alignment);
		/* [한국어] 사유 로그. */
		return NULL;
		/* [한국어] 즉시 실패. */
	}

	bitmap = calloc(1, sizeof(*bitmap));
	/* [한국어] 핸들 구조체 calloc — 0 초기화된 16바이트 정도(buf 포인터 + size_t).
	 * 외부 buf 자체는 호출자가 이미 보유하므로 여기서 추가 메모리는 핸들만. */
	if (!bitmap) {
		/* [한국어] OOM. */
		return NULL;
		/* [한국어] 호출자가 실패 처리. */
	}

	bitmap->buf = buf;
	/* [한국어] 외부 buf 포인터 저장 — 캐스팅 없이 bitmap_word*로 사용. */
	bitmap->size = size / sizeof(bitmap_word);
	/* [한국어] 바이트 크기를 워드 개수로 환산 저장. 이후 size는 워드 단위로만 의미를 가진다. */

	return bitmap;
	/* [한국어] 사용 가능한 핸들 반환. */
}

/*
 * [한국어]
 * ftl_bitmap_destroy - 핸들 free, 외부 buf는 그대로 유지(호출자 소유권)
 *
 * @param bitmap: ftl_bitmap_create로 만든 핸들. NULL이면 free(NULL)이 안전 무시.
 *
 * 동기/배경: 핸들과 buf의 라이프사이클이 다르다 — buf는 메타데이터 영역에 매핑된 영구 메모리이거나
 *   호출자가 별도 free 책임을 진다. 이 함수는 그저 핸들 구조체만 free.
 */
void
ftl_bitmap_destroy(struct ftl_bitmap *bitmap)
{
	free(bitmap);
	/* [한국어] 핸들 구조체만 해제. buf는 호출자 책임. */
}

/*
 * [한국어]
 * locate_bit - 비트 인덱스를 워드 포인터와 워드 내 비트 인덱스로 분해
 *
 * @param bitmap: 비트맵 핸들.
 * @param bit: 0..(size*워드비트수 - 1) 범위 비트 인덱스.
 * @param word_out: [out] 해당 비트가 속한 워드의 포인터.
 * @param word_bit_idx_out: [out] 워드 내 0..63 비트 위치.
 *
 * 동기/배경: get/set/clear 모두 같은 분해 로직이 필요해 추출. 인라인이라 호출 비용 없음.
 * 동작: bit >> SHIFT로 워드 인덱스, bit & MASK로 워드 내 비트 위치 산출.
 * 어서션: 워드 인덱스가 size 범위 내인지 확인.
 */
static inline void
locate_bit(const struct ftl_bitmap *bitmap, uint64_t bit,
	   bitmap_word **word_out, uint8_t *word_bit_idx_out)
{
	size_t word_idx = bit >> FTL_BITMAP_WORD_SHIFT;
	/* [한국어] 비트 인덱스를 6비트(64비트 시스템) 우시프트해 워드 인덱스 산출.
	 * 예: bit=130 → 130>>6 = 2 (3번째 워드). */

	assert(word_idx < bitmap->size);
	/* [한국어] 범위 검증 — 디버그 빌드에서 잘못된 인덱스 즉시 잡음.
	 * 릴리즈에서는 어서션 사라지므로 호출자 측 인덱스 검증이 필요. */

	*word_bit_idx_out = bit & FTL_BITMAP_WORD_MASK;
	/* [한국어] 워드 내 비트 위치 산출 — 하위 6비트만 추출 (0..63).
	 * 예: bit=130 → 130 & 0x3F = 2 (해당 워드의 2번째 비트). */
	*word_out = &bitmap->buf[word_idx];
	/* [한국어] 해당 워드의 포인터를 호출자에게 반환. 호출자는 이 포인터로 read-modify-write 수행. */
}

/*
 * [한국어]
 * ftl_bitmap_get - 특정 비트 인덱스의 값(0/1) 조회
 *
 * @param bitmap: 비트맵 핸들.
 * @param bit: 인덱스(0..size*워드비트-1).
 * @return: true = set, false = clear.
 *
 * 동작: locate_bit로 워드/위치 분해 → (1UL << pos) 마스크로 AND.
 * 동기화: 비-원자적 — 동시 set/clear와 경쟁 시 결과 미정의.
 */
bool
ftl_bitmap_get(const struct ftl_bitmap *bitmap, uint64_t bit)
{
	bitmap_word *word;
	/* [한국어] locate_bit가 채워줄 워드 포인터. */
	uint8_t word_bit_idx;
	/* [한국어] 워드 내 비트 위치. */

	locate_bit(bitmap, bit, &word, &word_bit_idx);
	/* [한국어] 인덱스 분해 — 어서션도 이 안에서 수행. */

	return *word & (1UL << word_bit_idx);
	/* [한국어] 해당 비트 마스크와 AND 결과를 bool로 변환. 0이면 false, 그 외엔 true. */
}

/*
 * [한국어]
 * ftl_bitmap_set - 특정 비트 인덱스를 1로 설정
 *
 * @param bitmap: 비트맵 핸들.
 * @param bit: 인덱스.
 *
 * 동작: 마스크 OR. 비-원자적.
 */
void
ftl_bitmap_set(struct ftl_bitmap *bitmap, uint64_t bit)
{
	bitmap_word *word;
	/* [한국어] 워드 포인터 out-param. */
	uint8_t word_bit_idx;
	/* [한국어] 워드 내 위치 out-param. */

	locate_bit(bitmap, bit, &word, &word_bit_idx);
	/* [한국어] 인덱스 분해. */

	*word |= (1UL << word_bit_idx);
	/* [한국어] 해당 비트 set — read-modify-write. 동기화 불필요한 단일 스레드 가정. */
}

/*
 * [한국어]
 * ftl_bitmap_clear - 특정 비트 인덱스를 0으로 설정
 *
 * @param bitmap: 비트맵 핸들.
 * @param bit: 인덱스.
 */
void
ftl_bitmap_clear(struct ftl_bitmap *bitmap, uint64_t bit)
{
	bitmap_word *word;
	/* [한국어] 워드 포인터 out-param. */
	uint8_t word_bit_idx;
	/* [한국어] 워드 내 위치 out-param. */

	locate_bit(bitmap, bit, &word, &word_bit_idx);
	/* [한국어] 인덱스 분해. */

	*word &= ~(1UL << word_bit_idx);
	/* [한국어] 해당 비트 clear — 마스크 NOT을 AND. */
}

/*
 * [한국어]
 * ftl_bitmap_find_first - [start_bit, end_bit] 범위 내 첫 set(value=true) 또는 clear(value=false) 비트 찾기
 *
 * @param bitmap: 비트맵 핸들.
 * @param start_bit: 검색 시작(포함).
 * @param end_bit: 검색 종료(포함).
 * @param value: true=첫 set 비트, false=첫 clear 비트.
 * @return: 찾은 비트 인덱스, 없으면 UINT64_MAX.
 *
 * 동기/배경: GC 후보 선정, 빈 슬롯 찾기 등에서 자주 호출되므로 워드 단위 점프로 최적화.
 * 동작:
 *   1) skip 결정: value=true면 0(원본 그대로), value=false면 ~0UL(반전 후 set 비트 찾기).
 *   2) start_bit이 속한 워드부터 검사: 시작 비트 이전을 마스킹으로 무시.
 *   3) 없으면 다음 워드들 순회 — end_bit까지.
 *   4) 첫 비-zero 워드의 첫 set 비트 위치를 __builtin_ctzl로 추출.
 *   5) 결과가 end_bit 초과면 UINT64_MAX 반환 (워드 단위 검사라 발생 가능).
 *
 * 호출 체인:
 *   ftl_bitmap_find_first_set / ftl_bitmap_find_first_clear → [이 함수]
 */
static uint64_t
ftl_bitmap_find_first(struct ftl_bitmap *bitmap, uint64_t start_bit,
		      uint64_t end_bit, bool value)
{
	bitmap_word skip = (value ? 0 : ~0UL);
	/* [한국어] XOR 마스크 — value=true면 0(원본 그대로), false면 ~0UL(전체 비트 반전 후 set 검색).
	 * 이 트릭으로 set/clear 검색 로직을 한 함수로 통합. */
	bitmap_word word;
	/* [한국어] 현재 검사 중인 워드 임시값. */
	size_t i, end;
	/* [한국어] i = 현재 워드 인덱스, end = 마지막 워드 인덱스(배타). */
	uint64_t ret;
	/* [한국어] 결과 비트 인덱스(반환 후보). */

	assert(start_bit <= end_bit);
	/* [한국어] start ≤ end 검증 — 잘못된 호출 즉시 잡음. */

	i = start_bit >> FTL_BITMAP_WORD_SHIFT;
	/* [한국어] 시작 비트가 속한 워드 인덱스 산출. */
	assert(i < bitmap->size);
	/* [한국어] 워드 인덱스 범위 검증. */

	word = (bitmap->buf[i] ^ skip) & (~0UL << (start_bit & FTL_BITMAP_WORD_MASK));
	/* [한국어] 첫 워드 처리:
	 *  - bitmap->buf[i] ^ skip: value=false 검색 시 비트 반전(0→1, 1→0)으로 set 비트 찾기 통일.
	 *  - ~0UL << (start_bit & MASK): 시작 비트 이전(워드 내) 비트들을 0으로 마스킹 — 검색 범위 제한.
	 *  - 두 결과 AND: 검색 시작 위치 이후의 set 비트만 남음. */
	if (word != 0) {
		/* [한국어] 시작 워드에 후보 비트가 있으면 즉시 found 라벨로 점프. */
		goto found;
	}

	end = spdk_min((end_bit >> FTL_BITMAP_WORD_SHIFT) + 1, bitmap->size);
	/* [한국어] 마지막 워드 인덱스(배타) — end_bit 워드 + 1, 그러나 bitmap->size를 넘지 않도록 클램프. */
	for (i = i + 1; i < end; i++) {
		/* [한국어] 다음 워드부터 끝까지 순차 검사. */
		word = bitmap->buf[i] ^ skip;
		/* [한국어] value=false 검색 시 비트 반전. set 검색은 그대로. */
		if (word != 0) {
			/* [한국어] 비-zero 워드 발견 — 첫 set 비트가 이 안에 있음. */
			goto found;
		}
	}

	return UINT64_MAX;
	/* [한국어] 모든 워드 검사해도 못 찾음 — 호출자 약속한 sentinel 반환. */
found:
	/* [한국어] 비-zero 워드 발견 시 점프 라벨 — i와 word가 채워진 상태. */
	ret = (i << FTL_BITMAP_WORD_SHIFT) + __builtin_ctzl(word);
	/* [한국어] 비트 인덱스 합성:
	 *  - i << SHIFT: 워드 인덱스에 워드비트수를 곱한 값(=워드 시작 비트 인덱스).
	 *  - __builtin_ctzl(word): GCC 내장 함수로 word의 최하위 1비트 위치(0..63) 반환.
	 *  두 값을 합쳐 전체 비트 인덱스 산출. */
	if (ret > end_bit) {
		/* [한국어] 워드 단위 검사라 end_bit를 살짝 넘는 비트가 잡힐 수 있음 — 그 경우 못 찾은 것으로 처리. */
		return UINT64_MAX;
	}
	return ret;
	/* [한국어] 유효 범위 내 첫 set 비트 인덱스 반환. */
}

/*
 * [한국어]
 * ftl_bitmap_find_first_set - 범위 내 첫 set(1) 비트 인덱스 반환
 *
 * @param bitmap/start_bit/end_bit: 비트맵과 검색 범위.
 * @return: 첫 set 비트 인덱스 또는 UINT64_MAX.
 *
 * 구현은 ftl_bitmap_find_first(value=true) 호출 wrapper.
 */
uint64_t
ftl_bitmap_find_first_set(struct ftl_bitmap *bitmap, uint64_t start_bit, uint64_t end_bit)
{
	return ftl_bitmap_find_first(bitmap, start_bit, end_bit, true);
	/* [한국어] value=true → set 비트 검색. */
}

/*
 * [한국어]
 * ftl_bitmap_find_first_clear - 범위 내 첫 clear(0) 비트 인덱스 반환
 *
 * @param bitmap/start_bit/end_bit: 비트맵과 범위.
 * @return: 첫 clear 비트 인덱스 또는 UINT64_MAX.
 *
 * 구현은 ftl_bitmap_find_first(value=false) — 내부적으로 비트 반전 트릭 사용.
 */
uint64_t
ftl_bitmap_find_first_clear(struct ftl_bitmap *bitmap, uint64_t start_bit,
			    uint64_t end_bit)
{
	return ftl_bitmap_find_first(bitmap, start_bit, end_bit, false);
	/* [한국어] value=false → clear 비트 검색(반전 후 set 검색과 동일). */
}

/*
 * [한국어]
 * ftl_bitmap_count_set - 비트맵 전체 set 비트 수 popcount 합산
 *
 * @param bitmap: 비트맵 핸들.
 * @return: 1로 set된 비트의 총 개수.
 *
 * 동기/배경: GC 후보 선정 시 한 밴드의 valid LBA 수가 곧 valid 비트맵의 set 개수.
 * 동작: 모든 워드를 순회하며 __builtin_popcountl로 워드별 1비트 개수 합산. O(워드수).
 *
 * 호출 체인:
 *   ftl_band_validity / GC 통계 코드 → [이 함수]
 */
uint64_t
ftl_bitmap_count_set(struct ftl_bitmap *bitmap)
{
	size_t i;
	/* [한국어] 워드 인덱스 루프 변수. */
	bitmap_word *word = bitmap->buf;
	/* [한국어] 순회용 포인터 — 매 반복마다 증가시켜 buf[i] 인덱싱과 동등하게 동작. */
	uint64_t count = 0;
	/* [한국어] 누적 카운터. */

	for (i = 0; i < bitmap->size; i++, word++) {
		/* [한국어] 모든 워드(0..size-1) 순회. word 포인터 증가는 컴파일러 최적화 도우미. */
		count += __builtin_popcountl(*word);
		/* [한국어] GCC 내장 popcount — 한 워드의 1비트 수를 한 번에 반환(보통 SSE4.2 popcnt 명령으로 컴파일).
		 * O(1)에 가까운 단일 명령이라 워드당 비용이 매우 낮다. */
	}

	return count;
	/* [한국어] 합산 결과 반환. */
}
