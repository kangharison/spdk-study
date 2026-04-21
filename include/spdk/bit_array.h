/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Bit array data structure
 */

/*
 * [한국어 설명] 가변 길이 비트 배열 공개 API 헤더 (bit_array.h)
 *
 * === 파일의 역할 ===
 * SPDK 내부에서 사용되는 **가변 길이 비트 배열(variable-length bit array)** 자료구조의 공개 API를 정의한다.
 * 각 비트는 0/1 한 가지 상태만 표현하는 플래그이며, 배열 전체는 `num_bits` 크기에 맞춰 동적으로 할당/재할당된다.
 * 구현은 `lib/util/bit_array.c`에 있고, 내부적으로 워드(32/64비트) 배열로 비트를 패킹(packing)하여 저장한다.
 * 이 자료구조는 "어느 인덱스가 설정되어 있는가"를 효율적으로 질의·설정·탐색하기 위한 경량 도구이며,
 * 상위 수준의 할당자(allocator) 시맨틱이 필요할 때는 이 비트 배열을 래핑한 `spdk_bit_pool`(bit_pool.h)을 사용한다.
 *
 * 주요 사용처:
 *   - `lib/thread/`: 등록된 poller/io_channel 인덱스 트래킹
 *   - `lib/blob/`, `lib/lvol/`: Blobstore의 cluster 할당 비트맵
 *   - `lib/ftl/`: FTL의 밴드(band)/청크(chunk) 상태 비트맵
 *   - `lib/nvme/`: CID(Command ID) 할당 비트맵
 *   - `lib/notify/`: 등록된 이벤트 타입 인덱스
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 전반의 **유틸리티 계층(util)** 에 속한다. 실행 컨텍스트는 보통 호스트 유저스페이스 SPDK thread 위이며,
 * 비트 배열 자체는 스레드 안전(thread-safe)이 아니다 — 호출자가 스레드 소유권(affinity)으로 동기화를 보장해야 한다.
 * 호출 체인:
 *   상위 모듈(blob/lvol/ftl/nvme 등)의 할당자/상태머신
 *     → [이 헤더의 spdk_bit_array_* 함수]
 *       → `lib/util/bit_array.c`의 워드 단위 bit 연산 (`__builtin_ffs`, shift/mask)
 * I/O hot-path가 아닌 **메타데이터/제어 경로**에서 주로 호출된다 (클러스터 할당, 초기화, 메타데이터 로드·저장 등).
 *
 * === 타 모듈과의 연결 ===
 * - `spdk/stdinc.h`: `uint32_t`, `bool` 등 C 표준 타입
 * - `spdk/bit_pool.h`: 이 비트 배열을 감싸서 "비트 할당 풀" 시맨틱을 제공 (bit_pool_create_from_array로 승격 가능)
 * - 상위 호출자: 메타데이터가 디스크에 기록·로드될 때 `store_mask`/`load_mask`로 평문 비트마스크 직렬화에 사용
 * 데이터 흐름:
 *   1. `spdk_bit_array_create(N)` → 내부 워드 배열 할당 (모두 0)
 *   2. set/clear로 비트 상태 변경, get으로 질의
 *   3. find_first_set/find_first_clear로 선형 스캔 (워드 단위 + __builtin_ffs로 가속)
 *   4. store_mask/load_mask로 비트맵을 메타데이터 블록에 직렬화·역직렬화
 *   5. `spdk_bit_array_free()` 호출 시 메모리 반환, 포인터 NULL 세팅
 *
 * === 주요 함수/구조체 요약 ===
 * - `struct spdk_bit_array`: 불투명 구조체(opaque) — 외부에서는 포인터로만 다룬다. 실제 레이아웃은 bit_array.c의 private 정의.
 * - `spdk_bit_array_create/free/resize`: 생애주기 관리 (할당/해제/크기 변경)
 * - `spdk_bit_array_get/set/clear`: 단일 비트 질의·설정·해제
 * - `spdk_bit_array_find_first_set/clear`: 선형 탐색 (할당자 스캔 루프의 핵심 primitive)
 * - `spdk_bit_array_count_set/clear`: 집계 (통계·메타데이터 계산용)
 * - `spdk_bit_array_store_mask/load_mask/clear_mask`: 외부 버퍼와의 비트마스크 직렬화 (디스크 메타데이터 영속화)
 *
 * 경계/주의:
 *   - 비트 인덱스는 `uint32_t`로 제한 → 최대 약 4G 비트(512MB 비트맵)
 *   - `capacity()`를 초과하는 인덱스는 get=false, clear=no-op, set=-EINVAL로 처리
 *   - find 함수는 "없음"을 `UINT32_MAX`로 표시 — 반환값 처리 시 반드시 비교 필요
 *   - 동시성: 배열 인스턴스 단위 락 없음. 멀티스레드 접근 시 호출자가 RW 보호를 제공해야 함.
 */

#ifndef SPDK_BIT_ARRAY_H
#define SPDK_BIT_ARRAY_H

/* [한국어] SPDK 공통 표준 헤더 포함 — uint32_t, bool, size_t 등 기본 타입 의존성. */
#include "spdk/stdinc.h"

#ifdef __cplusplus
/* [한국어] C++ 컴파일 환경에서 C 링키지(name mangling 회피)를 유지하기 위한 extern "C" 블록 시작. */
extern "C" {
#endif

/**
 * Variable-length bit array.
 */
/* [한국어] 가변 길이 비트 배열의 불투명(opaque) 전방 선언.
 *   - 실제 레이아웃은 lib/util/bit_array.c 에서만 정의된다 (워드 배열 + 크기/메타데이터).
 *   - 외부 사용자는 반드시 포인터로만 다루고, 이 헤더의 API 함수들만 이용해야 한다.
 *   - 동기화: 인스턴스 단위 내장 락 없음 — 호출자 책임으로 한 스레드에서만 조작하거나 상위 락으로 보호. */
struct spdk_bit_array;

/**
 * Return the number of bits that a bit array is currently sized to hold.
 *
 * \param ba Bit array to query.
 *
 * \return the number of bits.
 */
/*
 * [한국어]
 * spdk_bit_array_capacity - 비트 배열이 수용 가능한 총 비트 수(크기)를 반환.
 *
 * @ba: 조회 대상 비트 배열 포인터. NULL이 아니어야 하며, `create()` 또는 `resize()`로 초기화된 상태.
 * @return: 배열이 현재 담을 수 있는 비트 개수(= 생성/리사이즈 시 지정한 num_bits).
 *
 * 왜 필요한가: 할당자/상태머신이 경계 검사(bit_index < capacity) 를 위해 현재 크기를 알아야 한다.
 * 동작: 내부적으로 배열 메타데이터에 저장된 크기 필드를 읽기만 한다 (O(1)).
 * 실행 컨텍스트: 아무 SPDK thread에서나 안전 (부수효과 없음). 단, 다른 스레드가 resize 중이면
 *              일관성을 위해 상위 락이 필요.
 * 호출 체인:
 *   할당자/스캔 루프 → [spdk_bit_array_capacity] → 내부 필드 로드
 */
uint32_t spdk_bit_array_capacity(const struct spdk_bit_array *ba);

/**
 * Create a bit array.
 *
 * \param num_bits Number of bits that the bit array is sized to hold.
 *
 * All bits in the array will be cleared.
 *
 * \return a pointer to the new bit array.
 */
/*
 * [한국어]
 * spdk_bit_array_create - 지정된 크기의 새 비트 배열을 할당하고 모든 비트를 0으로 초기화.
 *
 * @num_bits: 배열이 담을 비트 수. 내부적으로는 워드 단위로 올림(ceil)되어 할당된다.
 *            0이면 빈 배열(capacity==0)을 반환할 수도 있으며, 호출자가 이를 처리해야 한다.
 * @return: 성공 시 새 비트 배열 포인터, 실패 시(메모리 부족) NULL.
 *
 * 왜 필요한가: 상위 할당자(blob cluster 비트맵, lvol 인덱스 등)가 초기에 "모두 자유 상태"인
 *              비트맵을 확보해야 한다.
 * 동작:
 *   1. num_bits를 워드(보통 64비트)의 배수로 반올림
 *   2. `malloc` / DPDK 메모리 풀에서 내부 구조체 + 워드 배열 할당
 *   3. `memset(0)`으로 모든 비트 클리어
 * 실행 컨텍스트: 초기화 경로 — 보통 SPDK thread 메인 스레드 또는 모듈 bootstrap 중.
 *               I/O hot-path에서는 호출하지 않는다 (malloc 오버헤드).
 * 에러 경로: malloc 실패 → NULL. 호출자는 반드시 반환값 NULL 검사 필요.
 * 호출 체인:
 *   blob/lvol/ftl 초기화 → [spdk_bit_array_create] → malloc → memset
 */
struct spdk_bit_array *spdk_bit_array_create(uint32_t num_bits);

/**
 * Free a bit array and set the pointer to NULL.
 *
 * \param bap Bit array to free.
 */
/*
 * [한국어]
 * spdk_bit_array_free - 비트 배열 메모리를 해제하고 포인터 변수를 NULL로 초기화.
 *
 * @bap: 비트 배열 포인터의 주소(이중 포인터). 함수 호출 후 `*bap == NULL`이 보장된다.
 *       `*bap`이 이미 NULL이면 no-op (double-free 방지).
 *
 * 왜 필요한가: use-after-free / double-free 방지를 위해 포인터를 명시적으로 무효화.
 *              SPDK의 일반적 관례("free 함수는 포인터의 포인터를 받아 NULL로 세팅")를 따른다.
 * 동작:
 *   1. `*bap == NULL` 이면 즉시 반환
 *   2. 내부 워드 배열과 관리 구조체를 free
 *   3. `*bap = NULL` 세팅
 * 실행 컨텍스트: 종료/해제 경로 — 하위 자원이 더 이상 사용되지 않음을 호출자가 보장해야 한다.
 *               다른 스레드가 여전히 접근 가능한 상태에서 호출하면 UB.
 * 호출 체인:
 *   모듈 finalize / destruct → [spdk_bit_array_free] → free
 */
void spdk_bit_array_free(struct spdk_bit_array **bap);

/**
 * Create or resize a bit array.
 *
 * To create a new bit array, pass a pointer to a spdk_bit_array pointer that is
 * NULL for bap.
 *
 * The bit array will be sized to hold at least num_bits.
 *
 * If num_bits is smaller than the previous size of the bit array,
 * any data beyond the new num_bits size will be cleared.
 *
 * If num_bits is larger than the previous size of the bit array,
 * any data beyond the old num_bits size will be cleared.
 *
 * \param bap Bit array to create/resize.
 * \param num_bits Number of bits that the bit array is sized to hold.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_bit_array_resize - 기존 비트 배열의 크기를 변경하거나, NULL이면 새로 생성.
 *
 * @bap: 비트 배열 포인터의 주소. `*bap == NULL`이면 create와 동일하게 새로 할당.
 *       성공 시 `*bap`이 갱신되며(기존 포인터가 invalidate 될 수 있음), 이후 호출자는
 *       반드시 `*bap`의 새 값을 사용해야 한다 (realloc 시맨틱과 유사).
 * @num_bits: 새 크기(비트 단위). 기존 크기보다 작으면 끝부분 절단 및 초과 영역 클리어.
 *            기존보다 크면 새로 늘어난 영역은 모두 0으로 초기화 (unset).
 * @return: 0 = 성공. 음수 errno(-ENOMEM 등) = 실패. 실패 시 `*bap`은 변경되지 않음(원본 보존).
 *
 * 왜 필요한가: blob cluster 수 확장/축소, 네임스페이스 리사이즈 등 메타데이터가 런타임에
 *              변하는 시나리오를 지원한다.
 * 동작:
 *   1. 새 워드 배열 할당 (또는 기존 재사용)
 *   2. 기존 내용을 복사 (가능한 범위 내에서)
 *   3. 새로 늘어난/잘려 나간 영역을 0으로 초기화
 *   4. 내부 num_bits 필드 업데이트
 * 실행 컨텍스트: 초기화/확장 경로, I/O hot-path 아님. malloc 가능한 상태여야 함.
 * 에러 경로: 할당 실패 시 -ENOMEM 반환, 호출자 원본은 그대로 유지.
 * 호출 체인:
 *   메타데이터 변경 이벤트 → [spdk_bit_array_resize] → 재할당 → memset/copy
 */
int spdk_bit_array_resize(struct spdk_bit_array **bap, uint32_t num_bits);

/**
 * Get the value of a bit from the bit array.
 *
 * If bit_index is beyond the end of the current size of the bit array, this
 * function will return false (i.e. bits beyond the end of the array are implicitly 0).
 *
 * \param ba Bit array to query.
 * \param bit_index The index of a bit to query.
 *
 * \return the value of a bit from the bit array on success, or false on failure.
 */
/*
 * [한국어]
 * spdk_bit_array_get - 특정 비트 인덱스의 값을 조회 (테스트/peek).
 *
 * @ba: 질의 대상 비트 배열 포인터 (const — 수정하지 않음).
 * @bit_index: 조회할 비트의 위치 (0부터 시작). 배열 크기를 초과해도 에러가 아닌 false 반환.
 * @return: 해당 비트가 1이면 true, 0이면 false. 범위 초과도 false (암묵적 0 시맨틱).
 *
 * 왜 필요한가: "이 인덱스가 이미 할당/표시되었나?" 질의는 할당자의 기본 연산이다.
 *              find_first_set 등 내부 스캔 루프 빌딩 블록으로도 사용.
 * 동작:
 *   1. bit_index >= capacity 이면 false 반환 (에러 아님, "out-of-bounds = 0" 규약)
 *   2. word_idx = bit_index / BITS_PER_WORD, bit_off = bit_index % BITS_PER_WORD
 *   3. `(words[word_idx] >> bit_off) & 1` 을 bool로 캐스팅
 * 실행 컨텍스트: 읽기 전용 — 호출자가 쓰기와 동시에 실행하지 않도록 조율하면 thread-safe.
 *               단일 워드 로드이므로 tearing 걱정 없음 (단 멀티 비트 상태 전이 중에는 부정확).
 * 호출 체인:
 *   bit_pool/상위 로직 → [spdk_bit_array_get] → 내부 워드 배열 접근
 */
bool spdk_bit_array_get(const struct spdk_bit_array *ba, uint32_t bit_index);

/**
 * Set (to 1) a bit in the bit array.
 *
 * If bit_index is beyond the end of the bit array, this function will return -EINVAL.
 *
 * \param ba Bit array to set a bit.
 * \param bit_index The index of a bit to set.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_bit_array_set - 특정 비트를 1로 설정 (할당/마킹).
 *
 * @ba: 대상 비트 배열 (수정 가능한 포인터).
 * @bit_index: 설정할 비트 위치. capacity 초과 시 -EINVAL (create/resize 때와 달리
 *             자동 확장하지 않는다 — 상위 계층이 미리 resize를 호출해야 한다).
 * @return: 0 = 성공 (이미 1이어도 성공). -EINVAL = bit_index가 범위 초과.
 *
 * 왜 필요한가: "이 인덱스를 사용 중으로 표시"는 할당자의 필수 연산.
 * 동작:
 *   1. bit_index >= capacity → -EINVAL 리턴
 *   2. word 위치 계산 후 `words[word_idx] |= (1ULL << bit_off)` 로 OR 마킹
 * 실행 컨텍스트: 단일 스레드 전제. 동시 set/clear가 있으면 read-modify-write 경쟁으로
 *               워드 단위 원자성이 깨질 수 있음 — 호출자가 락으로 보호.
 * 에러 경로: 범위 초과 외에는 실패 없음 (할당 없음).
 * 호출 체인:
 *   할당자 → [spdk_bit_array_set] → 워드 OR 연산
 */
int spdk_bit_array_set(struct spdk_bit_array *ba, uint32_t bit_index);

/**
 * Clear (to 0) a bit in the bit array.
 *
 * If bit_index is beyond the end of the bit array, no action is taken. Bits
 * beyond the end of the bit array are implicitly 0.
 *
 * \param ba Bit array to clear a bit.
 * \param bit_index The index of a bit to clear.
 */
/*
 * [한국어]
 * spdk_bit_array_clear - 특정 비트를 0으로 클리어 (해제/반환).
 *
 * @ba: 대상 비트 배열.
 * @bit_index: 클리어할 비트 위치. capacity 초과 시 no-op (에러 아님 — 반환값 없음).
 *
 * 왜 필요한가: "이 인덱스 사용 종료"를 표시하는 해제 연산. set의 짝이다.
 *              set과 달리 범위 초과는 에러 대신 조용히 무시되는데, 이는 "범위 밖은 이미 0"이라는
 *              시맨틱과 일관되기 때문이다.
 * 동작:
 *   1. bit_index >= capacity → 즉시 return (no-op)
 *   2. `words[word_idx] &= ~(1ULL << bit_off)` 로 AND-NOT 마스킹
 * 실행 컨텍스트: set과 마찬가지로 단일 스레드 전제 — read-modify-write 보호 필요.
 * 호출 체인:
 *   할당자 free 경로 → [spdk_bit_array_clear] → 워드 AND-NOT 연산
 */
void spdk_bit_array_clear(struct spdk_bit_array *ba, uint32_t bit_index);

/**
 * Find the index of the first set bit in the array.
 *
 * \param ba The bit array to search.
 * \param start_bit_index The bit index from which to start searching (0 to start
 * from the beginning of the array).
 *
 * \return the index of the first set bit. If no bits are set, returns UINT32_MAX.
 */
/*
 * [한국어]
 * spdk_bit_array_find_first_set - 지정 위치부터 스캔하여 처음으로 1인 비트의 인덱스를 찾는다.
 *
 * @ba: 탐색 대상 비트 배열 (읽기 전용).
 * @start_bit_index: 스캔 시작 위치 (0이면 처음부터). 이 인덱스 자신도 후보에 포함된다.
 * @return: 처음으로 발견된 1-bit의 절대 인덱스. 모두 0이면 `UINT32_MAX` 반환 — 호출자는
 *          반드시 이 값과 비교해 "없음"을 판단해야 한다.
 *
 * 왜 필요한가: "할당된 것들 중 가장 작은 인덱스부터 처리" 패턴(예: 더티 블록 스윕, 반납된
 *              청크 회수 등)에서 핵심 primitive.
 * 동작:
 *   1. start_bit_index가 포함된 워드부터 시작
 *   2. 해당 워드에서 start_bit_index 이상만 유효하도록 하위 비트를 마스크로 제거
 *   3. 해당 워드가 0이 아니면 `__builtin_ctzll`(trailing zeros count)로 워드 내 위치 계산
 *   4. 0이면 다음 워드로 진행 (워드 단위 스킵으로 캐시 친화적 선형 스캔)
 *   5. 모든 워드가 0이면 UINT32_MAX 반환
 * 실행 컨텍스트: 읽기 전용 스캔 — 쓰기와 동시 실행 시에는 스냅샷 일관성이 깨질 수 있어 상위 락 필요.
 * 성능: O(capacity / 64) — 64비트 워드 한 번에 검사, 대개 수 마이크로초.
 * 호출 체인:
 *   상위 스캔 루프(FTL 청크 회수 등) → [spdk_bit_array_find_first_set]
 */
uint32_t spdk_bit_array_find_first_set(const struct spdk_bit_array *ba, uint32_t start_bit_index);

/**
 * Find the index of the first cleared bit in the array.
 *
 * \param ba The bit array to search.
 * \param start_bit_index The bit index from which to start searching (0 to start
 * from the beginning of the array).
 *
 * \return the index of the first cleared bit. If no bits are cleared, returns UINT32_MAX.
 */
/*
 * [한국어]
 * spdk_bit_array_find_first_clear - 지정 위치부터 스캔하여 처음으로 0인 비트의 인덱스를 찾는다.
 *
 * @ba: 탐색 대상 비트 배열.
 * @start_bit_index: 스캔 시작 위치 (0이면 처음부터).
 * @return: 처음으로 발견된 0-bit의 절대 인덱스. 모두 1이면 `UINT32_MAX` (풀 고갈 시그널).
 *
 * 왜 필요한가: 비트맵 기반 할당자의 **핵심 primitive** — "비어있는 첫 번째 슬롯"을 찾는 연산.
 *              find_first_set의 반대 버전이며, `~word`에 `__builtin_ctzll`를 적용해 구현.
 * 동작:
 *   1. start_bit_index가 포함된 워드 로드 → NOT 연산으로 bit-invert
 *   2. start_bit_index 미만 비트는 마스크로 제거 (false-positive 방지)
 *   3. `__builtin_ctzll(~word)` 로 첫 0-bit 위치 계산
 *   4. 해당 워드에 없으면 다음 워드로 — 모든 워드가 ~0이면 UINT32_MAX
 * 실행 컨텍스트: 읽기 전용 스캔. 동시 set/clear 호출 시 스냅샷 일관성 없음.
 * 성능: O(capacity / 64). bit_pool_allocate_bit 내부에서 매 할당마다 호출됨 (hot path).
 * 호출 체인:
 *   spdk_bit_pool_allocate_bit → [spdk_bit_array_find_first_clear] → set
 */
uint32_t spdk_bit_array_find_first_clear(const struct spdk_bit_array *ba, uint32_t start_bit_index);

/**
 * Count the number of set bits in the array.
 *
 * \param ba The bit array to search.
 *
 * \return the number of bits set in the array.
 */
/*
 * [한국어]
 * spdk_bit_array_count_set - 배열 전체에서 1로 설정된 비트 수를 집계.
 *
 * @ba: 집계 대상 비트 배열.
 * @return: 1인 비트의 총 개수 (0 ≤ 값 ≤ capacity).
 *
 * 왜 필요한가: "현재 할당된 클러스터 수", "처리 대기 중인 항목 수" 등 통계/진행률 계산에 사용.
 *              `capacity - count_set == count_clear`가 성립한다.
 * 동작:
 *   1. 모든 워드를 순회하며 `__builtin_popcountll(word)`로 개별 워드의 1-bit 개수 합산
 *   2. O(capacity / 64) 선형 스캔, 워드별 popcount는 하드웨어 명령(예: x86의 POPCNT)으로 가속
 * 실행 컨텍스트: 읽기 전용. 스냅샷 일관성은 상위 락에 의존.
 * 호출 체인:
 *   통계 RPC / 메타데이터 유효성 검사 → [spdk_bit_array_count_set] → popcount 합산
 */
uint32_t spdk_bit_array_count_set(const struct spdk_bit_array *ba);

/**
 * Count the number of cleared bits in the array.
 *
 * \param ba The bit array to search.
 *
 * \return the number of bits cleared in the array.
 */
/*
 * [한국어]
 * spdk_bit_array_count_clear - 배열 전체에서 0으로 클리어된 비트 수를 집계.
 *
 * @ba: 집계 대상 비트 배열.
 * @return: 0인 비트의 총 개수 (= capacity - count_set).
 *
 * 왜 필요한가: "남은 가용 슬롯 수" 질의 — 풀 고갈 임박 판단, 확장(resize) 트리거 등에 사용.
 * 동작: 통상 내부적으로 `capacity - count_set(ba)`로 계산하거나, `__builtin_popcountll(~word)`
 *       합산. 최종 상위 초과 비트는 마스크로 배제해야 정확한 값이 나온다.
 * 실행 컨텍스트: 읽기 전용.
 * 호출 체인:
 *   풀 용량 RPC / 할당자 → [spdk_bit_array_count_clear]
 */
uint32_t spdk_bit_array_count_clear(const struct spdk_bit_array *ba);

/**
 * Store bitmask from bit array.
 *
 * \param ba Bit array.
 * \param mask Destination mask. Mask and bit array capacity must be equal.
 */
/*
 * [한국어]
 * spdk_bit_array_store_mask - 비트 배열 내용을 외부 바이트 버퍼로 **직렬화**.
 *
 * @ba: 원본 비트 배열 (const).
 * @mask: 복사 대상 바이트 버퍼. 호출자가 미리 할당해야 하며, 크기는 최소
 *        `ceil(capacity / 8)` 바이트. 버퍼 크기는 spdk_bit_array_capacity()와 반드시 일치해야 한다.
 *
 * 왜 필요한가: 메타데이터를 디스크에 기록할 때 비트맵을 평문 바이트로 변환 필요.
 *              예: blobstore의 cluster 사용 여부 비트맵을 metadata page에 영속화.
 * 동작:
 *   1. 내부 워드 배열의 바이트 표현을 `mask`로 memcpy
 *   2. 엔디안/워드 크기 변환이 필요하면 내부에서 처리 (플랫폼 간 메타데이터 호환성 유지)
 *   3. 끝부분(워드 경계와 비트 경계 차이)에 대한 처리가 구현에서 이뤄진다
 * 실행 컨텍스트: 읽기 전용 스냅샷 — 저장 중 동시 수정이 있으면 일관성 훼손. 상위 락/정지 상태 필수.
 * 호출 체인:
 *   blobstore sync / 메타데이터 write 경로 → [spdk_bit_array_store_mask] → memcpy
 */
void spdk_bit_array_store_mask(const struct spdk_bit_array *ba, void *mask);

/**
 * Load bitmask to bit array.
 *
 * \param ba Bit array.
 * \param mask Source mask. Mask and bit array capacity must be equal.
 */
/*
 * [한국어]
 * spdk_bit_array_load_mask - 외부 바이트 버퍼 내용을 비트 배열로 **역직렬화**(복원).
 *
 * @ba: 복원 대상 비트 배열. 기존 내용은 덮어쓰여진다. capacity는 mask와 반드시 일치해야 한다.
 * @mask: 입력 바이트 버퍼 (read-only). 보통 디스크에서 읽어온 메타데이터 블록.
 *
 * 왜 필요한가: 부팅 시 영속화된 비트맵을 메모리로 되살리는 경로. store_mask의 역함수.
 * 동작:
 *   1. 기존 워드 배열에 `mask`의 내용을 memcpy
 *   2. 필요 시 엔디안/정렬 보정
 *   3. capacity 기준의 잉여 비트 영역이 있다면 0으로 정규화
 * 실행 컨텍스트: 복원 경로 — 비트 배열 접근자가 아직 동작하지 않는 초기화 단계에서 수행.
 * 호출 체인:
 *   모듈 load / recovery 경로 → 디스크 read → [spdk_bit_array_load_mask]
 */
void spdk_bit_array_load_mask(struct spdk_bit_array *ba, const void *mask);

/**
 * Clear (to 0) bit array bitmask.
 *
 * \param ba Bit array.
 */
/*
 * [한국어]
 * spdk_bit_array_clear_mask - 비트 배열 전체를 0으로 일괄 클리어 (bulk reset).
 *
 * @ba: 대상 비트 배열. capacity는 유지되고, 모든 비트만 0으로 초기화된다.
 *
 * 왜 필요한가: "모든 할당 해제" 또는 "상태 재시작" 시 개별 비트를 일일이 clear하는 것보다
 *              훨씬 빠른 bulk 연산이 필요. 풀 전체 재초기화, recovery abort 경로 등에 사용.
 * 동작: 내부 워드 배열에 `memset(0)` 호출 — O(capacity / 8) 바이트 쓰기.
 * 실행 컨텍스트: 다른 스레드가 참조하지 않는 상태에서 호출해야 함 (상위 락 또는 단일 스레드).
 * 호출 체인:
 *   spdk_bit_pool_free_all_bits → [spdk_bit_array_clear_mask] → memset(0)
 */
void spdk_bit_array_clear_mask(struct spdk_bit_array *ba);

#ifdef __cplusplus
/* [한국어] C++ extern "C" 블록 닫기. */
}
#endif

#endif
/* [한국어] SPDK_BIT_ARRAY_H 헤더 가드 종료. */
