/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 비트맵 자료구조 공개 인터페이스 (ftl_bitmap.h)
 *
 * === 파일의 역할 ===
 * FTL이 다양한 곳에서 사용하는 단순 비트맵(set/clear/get/find_first_set/clear/count)의 공개 API를
 * 정의한다. 사용자가 미리 할당한 버퍼 위에 ftl_bitmap을 만들고 비트 인덱스로 조작하는 방식이다.
 * 주된 사용처는: (1) 밴드 내 valid LBA 트래킹(struct ftl_p2l_map.valid),
 * (2) 청크/슬롯 점유 표시, (3) GC 후보 선정용 valid 카운트 합산. 비트맵 버퍼는 sizeof(unsigned long)
 * (대개 64비트) 정렬되어야 하며, 같은 크기의 배수여야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/ftl 안의 다양한 트래킹 자료구조의 빌딩 블록. 호출 체인:
 *   ftl_band 빌드 → ftl_bitmap_create(...) → 이후 valid map 갱신, 복구 시 P2L 디시리얼라이즈에서 set/clear.
 * 실행 컨텍스트: 호스트 유저스페이스, FTL의 L2P/밴드 처리 스레드. 비트맵 자체는 락-프리가 아니므로
 *   호출자가 동일 비트맵에 동시 접근하지 않도록 spdk_thread 단위 affinity로 보장한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/stdinc.h.
 * 의존되는 모듈: ftl_band.c, ftl_internal.h(ftl_p2l_map.valid), ftl_nv_cache_chunk.c, ftl_l2p.c.
 * 데이터 흐름: 호출자가 hugepage/heap에서 정렬된 buf 할당 → ftl_bitmap_create로 핸들 만듦 →
 *   set/clear/get/count 호출 → ftl_bitmap_destroy로 핸들만 해제(buf는 호출자 책임).
 *
 * === 주요 함수/구조체 요약 ===
 *   - ftl_bitmap_buffer_alignment: 외부 const — buf 정렬 요구사항(=sizeof(unsigned long)).
 *   - ftl_bitmap_bits_to_size(bits): 비트 수 → 바이트 단위 buf 크기 변환.
 *   - ftl_bitmap_bits_to_blocks(bits): 비트 수 → FTL_BLOCK_SIZE(4 KiB) 블록 수 변환.
 *   - ftl_bitmap_create(buf, size): buf 위에 핸들 할당.
 *   - ftl_bitmap_destroy(b): 핸들 해제.
 *   - ftl_bitmap_get/set/clear: 단일 비트 조작.
 *   - ftl_bitmap_find_first_set/clear: 범위 내 첫 set/clear 비트 검색(없으면 UINT64_MAX).
 *   - ftl_bitmap_count_set: 전체 set 비트 수 popcount.
 */

#ifndef FTL_BITMAP_H_
#define FTL_BITMAP_H_
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "spdk/stdinc.h"
/* [한국어] uint64_t/size_t/bool 등 표준 타입 가져오기. */

struct ftl_bitmap;
/* [한국어] forward declaration — 구현 세부는 ftl_bitmap.c에 캡슐화.
 * 외부에서는 포인터로만 다루므로 내부 필드(buf, size) 변경이 자유로움. */

/**
 * @brief The required alignment for buffer used for bitmap
 */
extern const size_t ftl_bitmap_buffer_alignment;
/* [한국어] 외부 const 변수 — 비트맵 버퍼의 필수 정렬 단위(=sizeof(unsigned long), 보통 8바이트).
 * 설정자: ftl_bitmap.c가 정의. 읽는 자: 호출자가 buf 할당 시 정렬 보장에 사용.
 * 값 범위: sizeof(unsigned long) 고정. 동기화: 정의 시점 이후 read-only. */

/**
 * @brief Converts number of bits to bitmap size need to create it
 *
 * @param bits Number of bits
 *
 * @return Size needed to create bitmap which will hold space for specified number of bits
 */
/*
 * [한국어]
 * ftl_bitmap_bits_to_size - 비트 개수를 비트맵 buf 크기(바이트)로 변환
 *
 * @param bits: 표현해야 할 비트 개수.
 * @return: 정렬 요구사항을 충족하는 buf 크기(바이트) — 호출자는 이 크기만큼 정렬된 buf를 할당해야 함.
 *
 * 동작: bits/8 올림 후 ftl_bitmap_buffer_alignment(8B) 단위로 추가 정렬.
 * 실행 컨텍스트: 단순 산술 — 어디서든 호출 가능.
 */
uint64_t ftl_bitmap_bits_to_size(uint64_t bits);

/**
 * @brief Converts number of bits to blocks
 *
 * @param bits Number of bits
 *
 * @return Number of blocks needed to create bitmap which will hold space for specified number of bits
 */
/*
 * [한국어]
 * ftl_bitmap_bits_to_blocks - 비트 개수를 FTL_BLOCK_SIZE(4 KiB) 블록 수로 변환
 *
 * @param bits: 표현해야 할 비트 개수.
 * @return: 4 KiB 블록 단위로 올림한 개수.
 *
 * 동기/배경: 비트맵을 NV cache나 base bdev 위에 영속화할 때 블록 단위 I/O가 필요하므로 이 변환이 쓰인다.
 */
uint64_t ftl_bitmap_bits_to_blocks(uint64_t bits);

/**
 * @brief Creates a bitmap object using a preallocated buffer
 *
 * @param buf The buffer
 * @param size Size of the buffer
 *
 * @return On success - pointer to the allocated bitmap object, otherwise NULL
 */
/*
 * [한국어]
 * ftl_bitmap_create - 사전 할당된 buf 위에 ftl_bitmap 핸들을 생성
 *
 * @param buf: 호출자가 ftl_bitmap_buffer_alignment 정렬로 할당한 메모리 — buf 자체 소유권은 호출자에 유지.
 * @param size: buf 크기(바이트). ftl_bitmap_buffer_alignment의 배수여야 함.
 * @return: 성공 시 ftl_bitmap*, 실패 시 NULL(정렬 위반, 크기 비배수, calloc 실패 등).
 *
 * 동작: 정렬/크기 검증 후 핸들 구조체 calloc, 내부 buf 포인터 저장.
 * 실행 컨텍스트: FTL 디바이스 init mngt 단계, 단일 스레드.
 * 에러 경로: 정렬/크기 위반 시 SPDK_ERRLOG로 메시지 출력 후 NULL 반환.
 */
struct ftl_bitmap *ftl_bitmap_create(void *buf, size_t size);

/**
 * @brief Destroys the bitmap object
 *
 * @param bitmap The bitmap
 */
/*
 * [한국어]
 * ftl_bitmap_destroy - 핸들만 free, 외부 buf는 그대로 유지
 *
 * @param bitmap: ftl_bitmap_create로 만든 핸들. NULL이면 안전하게 무시(free(NULL) 의미).
 */
void ftl_bitmap_destroy(struct ftl_bitmap *bitmap);

/**
 * @brief Gets the value of the specified bit
 *
 * @param bitmap The bitmap
 * @param bit Index of the bit
 *
 * @return True if bit is set, otherwise false
 */
/*
 * [한국어]
 * ftl_bitmap_get - 특정 비트 인덱스의 값 조회
 *
 * @param bitmap: 비트맵 핸들(NULL 불가).
 * @param bit: 0..(size*8-1) 범위 인덱스. 범위 초과는 어서션 실패(디버그)/UB(릴리즈).
 * @return: true = 비트 set, false = clear.
 *
 * 동기화: 비-원자적 — 동시 set/clear와 경쟁하면 결과 미정의. 호출자 측 thread affinity로 보호.
 */
bool ftl_bitmap_get(const struct ftl_bitmap *bitmap, uint64_t bit);

/**
 * @brief Sets the specified bit
 *
 * @param bitmap The bitmap
 * @param bit Index of the bit
 */
/*
 * [한국어]
 * ftl_bitmap_set - 특정 비트 인덱스를 1로 설정
 *
 * @param bitmap: 비트맵 핸들.
 * @param bit: 인덱스(범위 초과 시 어서션 실패).
 *
 * 동기화: 비-원자적 — 다른 스레드의 동시 set/clear/get과 경쟁하면 안전하지 않음. */
void ftl_bitmap_set(struct ftl_bitmap *bitmap, uint64_t bit);

/**
 * @brief Clears the specified bit
 *
 * @param bitmap The bitmap
 * @param bit Index of the bit
 */
/*
 * [한국어]
 * ftl_bitmap_clear - 특정 비트 인덱스를 0으로 설정
 *
 * @param bitmap: 비트맵 핸들.
 * @param bit: 인덱스.
 *
 * 동기화: 비-원자적. */
void ftl_bitmap_clear(struct ftl_bitmap *bitmap, uint64_t bit);

/**
 * @brief Finds the first set bit
 *
 * @param bitmap The bitmap
 * @param start_bit Index of the bit from which to begin searching
 * @param end_bit Index of the bit up to which to search
 *
 * @return Index of the first set bit or UINT64_MAX if none found
 */
/*
 * [한국어]
 * ftl_bitmap_find_first_set - [start_bit, end_bit] 범위 내 첫 set 비트 인덱스 검색
 *
 * @param bitmap: 비트맵 핸들.
 * @param start_bit: 검색 시작 인덱스(포함).
 * @param end_bit: 검색 종료 인덱스(포함).
 * @return: 첫 set 비트 인덱스 또는 UINT64_MAX(없음).
 *
 * 동작: 64비트 워드 단위로 점프하면서 비-zero 워드를 찾으면 __builtin_ctzl로 첫 1비트 위치 추출 — O(워드수).
 */
uint64_t ftl_bitmap_find_first_set(struct ftl_bitmap *bitmap, uint64_t start_bit, uint64_t end_bit);

/**
 * @brief Finds the first clear bit
 *
 * @param bitmap The bitmap
 * @param start_bit Index of the bit from which to begin searching
 * @param end_bit Index of the bit up to which to search
 *
 * @return Index of the first clear bit or UINT64_MAX if none found
 */
/*
 * [한국어]
 * ftl_bitmap_find_first_clear - 범위 내 첫 clear(0) 비트 인덱스 검색
 *
 * @param bitmap: 비트맵 핸들.
 * @param start_bit/end_bit: 검색 범위(포함).
 * @return: 첫 clear 비트 인덱스 또는 UINT64_MAX.
 *
 * 동작: ftl_bitmap_find_first의 ~ 반전 트릭 — 워드를 ~0UL로 XOR 후 첫 1비트 검색.
 */
uint64_t ftl_bitmap_find_first_clear(struct ftl_bitmap *bitmap, uint64_t start_bit,
				     uint64_t end_bit);

/**
 * @brief Iterates over and counts set bits
 *
 * @param bitmap The bitmap
 *
 * @return Count of sets bits
 */
/*
 * [한국어]
 * ftl_bitmap_count_set - 비트맵 전체에서 set 비트 개수 popcount
 *
 * @param bitmap: 비트맵 핸들.
 * @return: 1로 set된 비트 개수.
 *
 * 동작: 64비트 워드별로 __builtin_popcountl로 비트 수 계산 후 합산 — O(워드수).
 * 사용 예: ftl_p2l_map의 valid 비트맵에서 GC 후보 선정 시 한 밴드의 valid LBA 수를 빠르게 산출.
 */
uint64_t ftl_bitmap_count_set(struct ftl_bitmap *bitmap);

#endif /* FTL_BITMAP_H_ */
/* [한국어] 헤더 가드 종료. */
