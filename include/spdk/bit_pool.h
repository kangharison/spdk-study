/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Bit pool data structure
 */

/*
 * [한국어 설명] 비트 기반 슬롯 할당 풀 공개 API 헤더 (bit_pool.h)
 *
 * === 파일의 역할 ===
 * `spdk_bit_array`(bit_array.h)를 내부적으로 감싸서 **비트-단위 슬롯 할당 풀**(allocator) 시맨틱을 제공한다.
 * 각 비트는 "인덱스 슬롯의 할당 여부"(1=할당됨, 0=자유)를 나타내며, API는 free-bit 탐색·할당·반환 연산을
 * 제공한다. 즉, 원시 비트 조작 API였던 bit_array 위에 **할당자(allocator) 계층**을 얹은 것이다.
 *
 * 구현은 `lib/util/bit_pool.c`에 위치하며, 내부적으로 다음 두 자료구조를 가진다:
 *   1. 내부 `spdk_bit_array` (할당 상태 비트맵 — 비트가 1이면 할당됨)
 *   2. 할당된 비트 개수 카운터 (count_allocated 같은 집계용)
 * 일반적으로 다음과 같이 활용한다:
 *   - NVMe CID(Command Identifier) 풀
 *   - bdev_io 식별자 풀
 *   - lvol 인덱스 슬롯
 *   - 기타 "N개 고정 크기 슬롯 중 하나를 빌려오고 반납하는" 모든 상황
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK **유틸리티 계층(util)** 의 일부이며, 실행 컨텍스트는 기본적으로 호출자의 스레드 소유권을 따른다.
 * I/O hot-path에서 사용될 수 있으나, 모든 API가 thread-safe가 아니라는 점에 주의 — 멀티스레드 환경에서는
 * 호출자가 상위 락을 제공하거나 단일 SPDK thread에 affinity를 부여해야 한다.
 *
 * 호출 체인(전형적 사용):
 *   상위 할당자(CID/세션 ID 관리)
 *     → spdk_bit_pool_allocate_bit() → 내부적으로 spdk_bit_array_find_first_clear + spdk_bit_array_set
 *   사용 종료 시
 *     → spdk_bit_pool_free_bit() → 내부적으로 spdk_bit_array_clear + 카운터 감소
 *
 * === 타 모듈과의 연결 ===
 * - `spdk/stdinc.h`: 기본 타입 의존
 * - `spdk/bit_array.h`: 내부적으로 래핑하는 하위 자료구조
 * - `spdk_bit_pool_create_from_array()`: 외부에서 미리 구성한 상태 비트맵(예: 복원된 메타데이터)을 그대로 풀 초기 상태로 승격시키는 경로
 * - `spdk_bit_pool_store_mask/load_mask`: 메타데이터 영속화 (내부 비트배열의 store/load에 위임)
 *
 * 데이터 흐름:
 *   1. create(N) 또는 create_from_array(existing) → 내부 비트맵 준비
 *   2. allocate_bit() → 자유 비트 찾아 set, 인덱스 반환 (풀 고갈 시 UINT32_MAX)
 *   3. free_bit(idx) → 해당 비트 clear, 카운터 감소
 *   4. store_mask/load_mask → 디스크 ↔ 메모리 비트맵 동기화
 *   5. free() → 내부 비트맵 포함 전체 해제
 *
 * === 주요 함수/구조체 요약 ===
 * - `struct spdk_bit_pool`: 불투명 구조체. 내부는 bit_pool.c에서 정의 (bit_array + free_count 카운터).
 * - `spdk_bit_pool_create(N)`: 모든 비트가 자유 상태인 새 풀 생성
 * - `spdk_bit_pool_create_from_array(arr)`: 기존 비트 배열을 인수받아 풀로 승격 (소유권 이전)
 * - `spdk_bit_pool_allocate_bit`: **★ 할당 핵심 API** — free 비트 하나를 선점하고 인덱스 반환
 * - `spdk_bit_pool_set_bit_allocated`: 특정 인덱스를 명시적으로 할당 상태로 세팅 (외부에서 이미 정해진 슬롯 선점 시)
 * - `spdk_bit_pool_free_bit`: 인덱스를 자유 상태로 반환. 이미 자유인 상태 반납 시 assert 발생
 * - `spdk_bit_pool_count_allocated / count_free`: 통계
 * - `spdk_bit_pool_store_mask / load_mask / free_all_bits`: 영속화 및 bulk 초기화
 *
 * 경계/주의:
 *   - `allocate_bit`가 UINT32_MAX를 반환하면 풀 고갈 — 호출자가 반드시 특수값 검사 필요
 *   - `free_bit`로 이미 자유 상태인 비트를 반납하면 디버그 빌드에서 `assert` 실패 (프로덕션 빌드는 UB로 풀 카운터 오염 가능)
 *   - 스레드 안전성 내장 없음 — 할당/반환이 여러 스레드에서 동시 호출되면 경쟁 조건 발생
 */

#ifndef SPDK_BIT_POOL_H
#define SPDK_BIT_POOL_H

/* [한국어] SPDK 표준 헤더 포함 — uint32_t, bool 등 기본 타입. */
#include "spdk/stdinc.h"

#ifdef __cplusplus
/* [한국어] C++ 환경에서 C 링키지를 유지하기 위한 extern "C" 블록 시작. */
extern "C" {
#endif

/* [한국어] 비트 풀의 불투명 전방 선언. 실제 정의는 lib/util/bit_pool.c 내부에서만 가능.
 *   - 외부 사용자는 포인터로만 다루고 본 헤더의 API 함수들로 조작.
 *   - 내부적으로 `spdk_bit_array` + 할당 카운터로 구성된다. */
struct spdk_bit_pool;

/* [한국어] 하위 자료구조인 비트 배열의 전방 선언. `create_from_array()`에 전달하기 위해 필요.
 *   - 실제 정의는 spdk/bit_array.h에서 확인. 풀 생성 이후에는 소유권이 풀로 이전되므로
 *     호출자는 더 이상 해당 bit_array를 직접 해제·조작해서는 안 된다. */
struct spdk_bit_array;

/**
 * Return the number of bits that a bit pool is currently sized to hold.
 *
 * \param pool Bit pool to query.
 *
 * \return the number of bits.
 */
/*
 * [한국어]
 * spdk_bit_pool_capacity - 풀의 전체 슬롯 수(비트 용량)를 반환.
 *
 * @pool: 조회 대상 비트 풀 포인터.
 * @return: 풀이 담을 수 있는 최대 비트(슬롯) 수. 내부 bit_array의 capacity와 동일.
 *
 * 왜 필요한가: 호출자가 "인덱스 유효 범위"를 확인하거나 "전체 용량 대비 할당률"을 계산할 때 사용.
 * 동작: 내부 `spdk_bit_array_capacity()` 호출로 위임 (O(1)).
 * 실행 컨텍스트: 읽기 전용, 부수효과 없음.
 * 호출 체인:
 *   상위 통계/검증 로직 → [spdk_bit_pool_capacity] → spdk_bit_array_capacity
 */
uint32_t spdk_bit_pool_capacity(const struct spdk_bit_pool *pool);

/**
 * Create a bit pool.
 *
 * All bits in the pool will be available for allocation.
 *
 * \param num_bits Number of bits that the bit pool is sized to hold.
 *
 * \return a pointer to the new bit pool.
 */
/*
 * [한국어]
 * spdk_bit_pool_create - 모든 비트가 자유 상태인 새 비트 풀을 생성.
 *
 * @num_bits: 풀이 담을 슬롯(비트) 수. 0이면 경계 케이스 — 대부분의 API가 즉시 UINT32_MAX 반환.
 * @return: 새 풀 포인터, 메모리 부족 시 NULL. 호출자는 반드시 NULL 검사 필요.
 *
 * 왜 필요한가: 새 할당자 인스턴스를 만들 때 기본 진입점. 예: NVMe qpair당 CID 풀.
 * 동작:
 *   1. 내부적으로 `spdk_bit_array_create(num_bits)` 호출 (모든 비트가 0=자유)
 *   2. 풀 메타데이터 구조체 할당, 카운터 초기화 (allocated=0, free=num_bits)
 *   3. bit_array와 풀 구조체를 연결
 * 에러 경로: malloc 실패 → 내부 bit_array 할당 롤백 후 NULL 반환.
 * 실행 컨텍스트: 초기화 경로. I/O hot-path 아님.
 * 호출 체인:
 *   qpair init / lvol attach / bdev_io 풀 구성 → [spdk_bit_pool_create]
 */
struct spdk_bit_pool *spdk_bit_pool_create(uint32_t num_bits);

/**
 * Create a bit pool from an existing spdk_bit_array.
 *
 * The starting state of the bit pool will be specified by the state
 * of the specified spdk_bit_array.
 *
 * The new spdk_bit_pool will consume the spdk_bit_array and assumes
 * responsibility for freeing it.  The caller should not use the
 * spdk_bit_array after this function returns.
 *
 * \param array spdk_bit_array representing the starting state of the new bit pool.
 *
 * \return a pointer to the new bit pool, NULL if one could not be created (in which
 *         case the caller maintains responsibility for the spdk_bit_array)
 */
/*
 * [한국어]
 * spdk_bit_pool_create_from_array - 기존 비트 배열을 풀로 **승격**(consume)하여 초기 상태로 사용.
 *
 * @array: 초기 상태를 담은 비트 배열. 성공 시 풀이 소유권을 가져가고, 호출자는 더 이상 이 포인터를
 *         직접 free/접근해서는 안 된다 (double-free 위험).
 * @return: 성공 시 새 풀 포인터. **실패 시(NULL)에는 호출자가 여전히 array의 소유권을 가짐** — 이
 *          비대칭성이 중요: 호출자는 반환값을 보고 array를 free할지 결정해야 한다.
 *
 * 왜 필요한가: 메타데이터를 디스크에서 복원했을 때, 이미 채워진 비트 배열(할당된 슬롯들 표시 포함)을
 *              그대로 풀의 초기 상태로 승격시키는 경로. 예: blobstore 부팅 후 cluster 사용 맵을
 *              메모리에 로드한 뒤 할당자로 전환.
 * 동작:
 *   1. 새 풀 구조체 할당 (실패 시 NULL 반환 — array 소유권 호출자 유지)
 *   2. 풀 내부 array 포인터를 인자 `array`로 세팅 (소유권 이전)
 *   3. `spdk_bit_array_count_set(array)` 등으로 allocated 카운터 초기화
 * 에러 경로: 풀 메타데이터 malloc 실패 시 NULL 반환, array는 건드리지 않음.
 * 실행 컨텍스트: 복원/초기화 경로.
 * 호출 체인:
 *   recovery / metadata load → [spdk_bit_pool_create_from_array]
 */
struct spdk_bit_pool *spdk_bit_pool_create_from_array(struct spdk_bit_array *array);

/**
 * Free a bit pool and set the pointer to NULL.
 *
 * \param pool Bit pool to free.
 */
/*
 * [한국어]
 * spdk_bit_pool_free - 풀과 내부 비트 배열을 모두 해제하고 포인터를 NULL로 설정.
 *
 * @pool: 풀 포인터의 주소(이중 포인터). 호출 후 `*pool == NULL`이 보장된다.
 *        `*pool == NULL`이면 no-op (안전하게 재호출 가능).
 *
 * 왜 필요한가: 풀 인스턴스 종료 시 내부 bit_array까지 한 번에 해제해 누수를 방지.
 *              SPDK의 관례(포인터 더블 포인터 → NULL 세팅)를 따른다.
 * 동작:
 *   1. `*pool == NULL` 즉시 반환
 *   2. 내부 `spdk_bit_array_free()` 호출로 비트 배열 해제
 *   3. 풀 구조체 free, `*pool = NULL`
 * 주의: 아직 할당된 슬롯이 남아있어도 해제된다 — 상위 계층은 모든 슬롯이 반납되었는지 사전에 보장해야 한다.
 * 실행 컨텍스트: 종료/모듈 해체 경로.
 * 호출 체인:
 *   모듈 finalize → [spdk_bit_pool_free] → spdk_bit_array_free
 */
void spdk_bit_pool_free(struct spdk_bit_pool **pool);

/**
 * Create or resize a bit pool.
 *
 * To create a new bit pool, pass a pointer to a spdk_bit_pool pointer that is
 * NULL.
 *
 * The bit pool will be sized to hold at least num_bits.
 *
 * If num_bits is larger than the previous size of the bit pool,
 * the new bits will all be available for future allocations.
 *
 * \param pool Bit pool to create/resize.
 * \param num_bits Number of bits that the bit pool is sized to hold.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_bit_pool_resize - 풀 용량을 변경 (또는 NULL이면 새로 생성).
 *
 * @pool: 풀 포인터의 주소. `*pool == NULL`이면 내부적으로 create 경로와 동일.
 *        성공 시 `*pool`이 갱신될 수 있음 (내부 bit_array가 재할당되므로 호출자의 원래 포인터가 바뀜).
 * @num_bits: 새 용량. 기존보다 크면 늘어난 영역은 모두 자유 상태(0)로 시작.
 *            기존보다 작으면 절단된 영역에서 할당된 비트가 있었더라도 사용자에게 알려지지 않고 손실됨 —
 *            상위 계층이 사전에 반납을 보장해야 함.
 * @return: 0 = 성공. 음수 errno = 실패 (예: -ENOMEM). 실패 시 `*pool`은 변경되지 않음.
 *
 * 왜 필요한가: 런타임 스케일링 — blobstore cluster 수 확장, lvol 리사이즈 등.
 * 동작:
 *   1. 내부 `spdk_bit_array_resize()` 호출로 비트 배열 재할당
 *   2. 새로 늘어난 영역은 0으로 초기화됨 (spdk_bit_array_resize 시맨틱)
 *   3. 할당 카운터는 유지 (자유 슬롯 수만 늘어남)
 * 에러 경로: bit_array 재할당 실패 시 원본 풀과 bit_array 모두 보존.
 * 실행 컨텍스트: 초기화/확장 경로. 진행 중인 할당/반환과 동시 수행 불가.
 * 호출 체인:
 *   확장 이벤트 → [spdk_bit_pool_resize] → spdk_bit_array_resize
 */
int spdk_bit_pool_resize(struct spdk_bit_pool **pool, uint32_t num_bits);

/**
 * Return whether the specified bit has been allocated from the bit pool.
 *
 * If bit_index is beyond the end of the current size of the bit pool, this
 * function will return false (i.e. bits beyond the end of the pool cannot be allocated).
 *
 * \param pool Bit pool to query.
 * \param bit_index The index of a bit to query.
 *
 * \return true if the bit has been allocated, false otherwise
 */
/*
 * [한국어]
 * spdk_bit_pool_is_allocated - 특정 슬롯이 현재 할당 상태인지 질의.
 *
 * @pool: 조회 대상 풀.
 * @bit_index: 질의할 슬롯 인덱스. 범위 초과 시 false (할당 가능 영역이 아니므로).
 * @return: true = 이미 할당됨(사용 중), false = 자유 상태 또는 범위 밖.
 *
 * 왜 필요한가: 디버깅·검증 — "이 CID가 정말 활성 상태인가?", "이 슬롯에 작업을 배정해도 되는가?"
 * 동작: 내부 `spdk_bit_array_get()` 호출로 위임. 비트가 1이면 할당됨.
 * 실행 컨텍스트: 읽기 전용, 동시 수정과 함께 호출 시 스냅샷 부정확 가능.
 * 호출 체인:
 *   디버그/검증 루틴 → [spdk_bit_pool_is_allocated] → spdk_bit_array_get
 */
bool spdk_bit_pool_is_allocated(const struct spdk_bit_pool *pool, uint32_t bit_index);

/**
 * Allocate a bit from the bit pool.
 *
 * \param pool Bit pool to allocate a bit from
 *
 * \return index of the allocated bit, UINT32_MAX if no free bits exist
 */
/*
 * [한국어]
 * spdk_bit_pool_allocate_bit - 자유 상태인 슬롯 하나를 찾아 할당 표시하고 인덱스를 반환. (★ 핵심 API)
 *
 * @pool: 할당을 요청할 대상 풀.
 * @return: 할당된 슬롯의 인덱스. 풀 고갈 시 `UINT32_MAX` — 호출자는 반드시 이 값을 검사해
 *          "풀 고갈" 분기를 처리해야 한다.
 *
 * 왜 필요한가: 비트맵 기반 슬롯 풀의 대표 연산. "빈자리 찾기 + 점유"를 원자적(단일 스레드 기준)으로 수행.
 * 동작:
 *   1. `spdk_bit_array_find_first_clear(array, 0)`으로 첫 자유 비트 탐색
 *   2. `UINT32_MAX`이면 즉시 그대로 반환 (할당 실패)
 *   3. 아니면 `spdk_bit_array_set(array, idx)`로 점유 표시, 할당 카운터 증가
 *   4. idx 반환
 * 성능: O(capacity / 64) worst case이지만, 내부적으로 "가장 최근 할당 직후"부터 검색을 시작하는
 *       최적화를 채택할 수도 있다 (구현 디테일). 어쨌든 hot path에서 자주 호출됨.
 * 실행 컨텍스트: **비 thread-safe** — 여러 스레드에서 동시에 호출하면 같은 비트가 두 번 할당될 수 있음.
 *               반드시 풀을 소유한 단일 SPDK thread에서 호출하거나, 상위 락으로 직렬화해야 한다.
 * 에러 경로: 풀 고갈 → UINT32_MAX. 호출자는 backoff/재시도 또는 풀 확장 결정을 내린다.
 * 호출 체인:
 *   CID 할당 / slot 예약 → [spdk_bit_pool_allocate_bit] → find_first_clear + set
 */
uint32_t spdk_bit_pool_allocate_bit(struct spdk_bit_pool *pool);

/**
 * Set the specified bit as allocated in the bit pool.
 *
 * \param pool Bit pool to set the bit in
 * \param bit_index Index of the bit to set
 *
 * \return 0 if the bit is set successfully, -EBUSY if the bit was already set,
 * -EINVAL if bit_index is out of range.
 */
/*
 * [한국어]
 * spdk_bit_pool_set_bit_allocated - **특정 인덱스**를 명시적으로 할당 상태로 세팅 (수동 예약).
 *
 * @pool: 대상 풀.
 * @bit_index: 할당할 슬롯 인덱스. 범위 초과 시 -EINVAL.
 * @return: 0 = 성공 (비트를 0→1로 변경하고 카운터 증가).
 *          -EBUSY = 이미 할당된 비트 (이중 점유 방지 — 카운터 오염 방지용 가드).
 *          -EINVAL = bit_index >= capacity.
 *
 * 왜 필요한가: "이미 정해진 ID/슬롯"을 예약해야 하는 경우 — 예: 프로토콜이 특정 CID를 지정했거나,
 *              부트 시 특정 슬롯을 예비로 잡아두고 시작해야 할 때. allocate_bit과 달리 "아무 자유
 *              슬롯" 이 아니라 "지정한 슬롯"을 선점한다.
 * 동작:
 *   1. bit_index >= capacity → -EINVAL
 *   2. `spdk_bit_array_get(array, idx)`가 true → -EBUSY
 *   3. 그 외 `spdk_bit_array_set(array, idx)` 수행 + 카운터 증가 → 0
 * 실행 컨텍스트: 비 thread-safe — allocate_bit과 동일한 동기화 요구.
 * 호출 체인:
 *   복원/특수 예약 → [spdk_bit_pool_set_bit_allocated] → spdk_bit_array_get/set
 */
int spdk_bit_pool_set_bit_allocated(struct spdk_bit_pool *pool, uint32_t bit_index);

/**
 * Free a bit back to the bit pool.
 *
 * Callers must not try to free a bit that has not been allocated, otherwise the
 * pool may become corrupted without notification.  Freeing a bit that has not
 * been allocated will result in an assert in debug builds.
 *
 * \param pool Bit pool to place the freed bit
 * \param bit_index The index of a bit to free.
 */
/*
 * [한국어]
 * spdk_bit_pool_free_bit - 이전에 할당된 슬롯을 자유 상태로 반환 (allocate의 짝).
 *
 * @pool: 대상 풀.
 * @bit_index: 반환할 슬롯 인덱스. 반드시 현재 "할당 상태"여야 한다 — 자유 상태인 비트를 반납하면
 *             **디버그 빌드에서 `assert` 실패**, 릴리스 빌드에서는 카운터 오염으로 풀이 조용히 부패.
 *             범위 초과 시 동작은 구현에 따라 assert 혹은 no-op.
 *
 * 왜 필요한가: allocate_bit으로 받은 인덱스의 사용이 끝났을 때 풀에 돌려주는 정상 경로.
 * 동작:
 *   1. (디버그) bit_index 범위 및 현재 할당 상태 검사 → 위반 시 assert
 *   2. `spdk_bit_array_clear(array, idx)`로 비트 0 세팅
 *   3. 할당 카운터 감소, 자유 카운터 증가
 * 반환값 없음: 에러는 assert로만 전달. 호출자는 반드시 "정말 할당된 인덱스인가"를 이중 점검해야 한다.
 * 실행 컨텍스트: 비 thread-safe — 다른 allocate/free와 동시 호출 금지.
 * 호출 체인:
 *   CID 완료 처리 / 슬롯 해제 → [spdk_bit_pool_free_bit] → spdk_bit_array_clear
 */
void spdk_bit_pool_free_bit(struct spdk_bit_pool *pool, uint32_t bit_index);

/**
 * Count the number of bits allocated from the pool.
 *
 * \param pool The bit pool to count.
 *
 * \return the number of bits allocated from the pool.
 */
/*
 * [한국어]
 * spdk_bit_pool_count_allocated - 현재 할당 상태인 슬롯 수를 반환.
 *
 * @pool: 집계 대상 풀.
 * @return: 1로 표시된 비트 개수 (= 사용 중인 슬롯 수).
 *
 * 왜 필요한가: 풀 사용률 모니터링, 자원 고갈 경고, 메트릭 노출 등.
 * 동작: 보통 풀 내부 카운터를 O(1)로 반환 (매번 선형 스캔하지 않음). 초기화 이후 allocate/free
 *       호출마다 동기 갱신된 값이다. 구현에 따라 `spdk_bit_array_count_set()`으로 폴백.
 * 실행 컨텍스트: 읽기 전용.
 * 호출 체인:
 *   RPC / telemetry → [spdk_bit_pool_count_allocated]
 */
uint32_t spdk_bit_pool_count_allocated(const struct spdk_bit_pool *pool);

/**
 * Count the number of free bits in the pool.
 *
 * \param pool The bit pool to count.
 *
 * \return the number of free bits in the pool.
 */
/*
 * [한국어]
 * spdk_bit_pool_count_free - 현재 자유 상태인 슬롯 수를 반환 (할당 가능 용량).
 *
 * @pool: 집계 대상 풀.
 * @return: 0으로 표시된 비트 개수 (= 할당 가능한 여유 슬롯 수). `capacity - count_allocated`와 동일.
 *
 * 왜 필요한가: "지금 할당 요청 N개를 받을 수 있는가?" 판단 — 상위 계층이 backpressure 정책,
 *              throttling, 풀 확장 트리거를 결정할 때 사용.
 * 동작: 내부 카운터 또는 `capacity - count_allocated` 계산으로 O(1) 반환.
 * 실행 컨텍스트: 읽기 전용.
 * 호출 체인:
 *   스케줄러 / admission control → [spdk_bit_pool_count_free]
 */
uint32_t spdk_bit_pool_count_free(const struct spdk_bit_pool *pool);

/**
 * Store bitmask from bit pool.
 *
 * \param pool Bit pool.
 * \param mask Destination mask. Mask and bit array pool must be equal.
 */
/*
 * [한국어]
 * spdk_bit_pool_store_mask - 풀의 할당 상태를 외부 버퍼로 **직렬화** (영속화용).
 *
 * @pool: 원본 풀.
 * @mask: 출력 바이트 버퍼. 호출자가 `ceil(capacity/8)` 바이트를 미리 할당해야 함.
 *
 * 왜 필요한가: 디스크/메타데이터 블록에 풀 상태를 기록. 부팅 시 load_mask로 복원 가능.
 * 동작: 내부 `spdk_bit_array_store_mask(pool->array, mask)` 호출로 위임.
 * 실행 컨텍스트: 읽기 전용 — 기록 중 동시 allocate/free 호출 금지 (일관성 훼손 방지).
 * 호출 체인:
 *   sync/snapshot → [spdk_bit_pool_store_mask] → spdk_bit_array_store_mask
 */
void spdk_bit_pool_store_mask(const struct spdk_bit_pool *pool, void *mask);

/**
 * Load bitmask to bit pool.
 *
 * \param pool Bit pool.
 * \param mask Source mask. Mask and bit array pool must be equal.
 */
/*
 * [한국어]
 * spdk_bit_pool_load_mask - 외부 버퍼의 비트맵을 풀의 할당 상태로 **복원**.
 *
 * @pool: 복원 대상 풀. 기존 상태는 덮어씌워진다.
 * @mask: 입력 바이트 버퍼 (디스크에서 읽은 메타데이터 등). 크기는 pool capacity와 호환되어야 함.
 *
 * 왜 필요한가: 부팅·recovery 시 이전 세션의 풀 상태를 복원. store_mask의 역함수.
 * 동작:
 *   1. 내부 `spdk_bit_array_load_mask(pool->array, mask)` 호출
 *   2. 풀의 allocated/free 카운터를 새 내용 기준으로 재계산 (popcount 기반)
 * 실행 컨텍스트: 초기화/복원 경로 — 다른 allocate/free 호출과 동시에 실행되지 않아야 함.
 * 호출 체인:
 *   recovery / metadata load → [spdk_bit_pool_load_mask] → spdk_bit_array_load_mask
 */
void spdk_bit_pool_load_mask(struct spdk_bit_pool *pool, const void *mask);

/**
 * Free all bits back into the bit pool.
 *
 * \param pool Bit pool.
 */
/*
 * [한국어]
 * spdk_bit_pool_free_all_bits - 풀 전체를 **초기 상태(모두 자유)** 로 한 번에 리셋 (bulk).
 *
 * @pool: 리셋 대상 풀. 모든 슬롯이 자유 상태가 되고, 할당 카운터는 0으로 초기화된다.
 *
 * 왜 필요한가: 개별 free_bit을 N번 호출하는 것보다 훨씬 빠른 bulk reset. 풀 재사용, recovery abort,
 *              테스트 초기화 등에 사용.
 * 동작:
 *   1. `spdk_bit_array_clear_mask(pool->array)`로 전체 비트 0 세팅 (memset)
 *   2. 카운터 초기화: allocated=0, free=capacity
 * 주의: 외부 코드가 현재 할당된 인덱스를 참조하고 있더라도 리셋된다 — 상위 계층이 "아무도 더 이상
 *       참조하지 않음"을 보장해야 한다.
 * 실행 컨텍스트: 비 thread-safe. 다른 allocate/free와 동시 호출 금지.
 * 호출 체인:
 *   모듈 reset / 테스트 시나리오 → [spdk_bit_pool_free_all_bits] → spdk_bit_array_clear_mask
 */
void spdk_bit_pool_free_all_bits(struct spdk_bit_pool *pool);

#ifdef __cplusplus
/* [한국어] C++ extern "C" 블록 닫기. */
}
#endif

#endif
/* [한국어] SPDK_BIT_POOL_H 헤더 가드 종료. */
