/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 전용 메모리 풀 공개 인터페이스 (ftl_mempool.h)
 *
 * === 파일의 역할 ===
 * FTL 메타데이터/DMA 버퍼용 고성능 메모리 풀의 공개 API. 두 가지 모드를 지원한다:
 *   1) DMA 풀(ftl_mempool_create) — 내부에서 hugepage DMA 메모리를 할당해 NVMe DMA에 즉시 사용 가능.
 *   2) 외부 버퍼 풀(ftl_mempool_create_ext) — 호출자가 mmap한 영구 메모리 위에 풀을 얹어,
 *      슈퍼블록 영구 객체와 동일 베이스에서 ftl_df_obj_id로 영속 식별 가능. 초기에는 uninitialized
 *      상태(claim/release_df만 가능)이며 ftl_mempool_initialize_ext 호출 시 일반 get/put 모드로 전환.
 *
 * === 전체 아키텍처에서의 위치 ===
 * FTL의 거의 모든 동적 메모리(요청 객체, 메타데이터 페이지, 매핑 페이지 등) 풀의 빌딩 블록.
 * 호출 체인: ftl_core_init → ftl_mempool_create(요청용) /
 *   ftl_md_init → 외부 메모리 mmap → ftl_mempool_create_ext → ftl_mempool_claim_df로 기존 객체 등록 →
 *   ftl_mempool_initialize_ext로 일반 모드 전환 → 이후 get/put.
 * 실행 컨텍스트: SPDK reactor 스레드. 풀 자체는 lockless가 아니므로 한 풀은 한 스레드에서만 사용한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/stdinc.h, ftl_df.h(ftl_df_obj_id 타입).
 * 의존되는 모듈: ftl_mempool.c(구현), ftl_md.c, ftl_band.c, ftl_io.c 등 거의 모든 FTL 메인 코드.
 * 데이터 흐름: 풀이 관리하는 객체들은 호출자 코드 사이에서 빈번히 get/put 됨.
 *   외부 버퍼 모드에서는 ftl_df_obj_id로 영속화되어 다음 부팅에서 같은 객체를 재발견 가능.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct ftl_mempool: 불투명 핸들(외부 노출 X).
 *   - ftl_mempool_create(count, size, alignment, socket_id): DMA 풀 생성(NUMA 지원).
 *   - ftl_mempool_destroy: DMA 풀 해제.
 *   - ftl_mempool_get/put: 풀에서 객체 획득/반환.
 *   - ftl_mempool_create_ext(buffer, count, size, alignment): 외부 버퍼 위 풀 생성(uninit 상태).
 *   - ftl_mempool_destroy_ext: 외부 버퍼 풀 해제(buffer는 호출자 책임).
 *   - ftl_mempool_initialize_ext: uninit → init 전환 — 이후 get/put 가능, claim/release_df는 disallow.
 *   - ftl_mempool_get_df_obj_id(mpool, ptr): 객체 포인터 → 영속 ID 변환.
 *   - ftl_mempool_get_df_ptr(mpool, id): ID → 객체 포인터 변환.
 *   - ftl_mempool_claim_df / release_df: uninit 모드에서 영속 ID로 객체 점유/반환.
 *   - ftl_mempool_get_df_obj_index: 객체의 풀 내 인덱스 산출.
 */

#ifndef FTL_MEMPOOL_H
#define FTL_MEMPOOL_H
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "spdk/stdinc.h"
/* [한국어] size_t/uint*_t 등 표준 타입. */

#include "ftl_df.h"
/* [한국어] ftl_df_obj_id 타입 — durable-format 영속 ID 표현. */

/* TODO: Consider porting this mempool to general SPDK utils */
/* [한국어] 이 mempool은 FTL 전용으로 시작했으나, 일반 SPDK 유틸로 승격 검토 중이라는 원본 TODO. */

/**
 * @brief Creates and initializes custom FTL memory pool using DMA kind memory
 *
 * @param count Count of element in the memory pool
 * @param size Size of elements in the memory pool
 * @param alignment Memory alignment of element in the memory pool
 * @param socket_id It is the socket identifier in the case of NUMA. The value
 * can be *SOCKET_ID_ANY* if there is no NUMA constraint for the reserved zone.
 *
 * @return Pointer to the memory pool
 */
/*
 * [한국어]
 * ftl_mempool_create - DMA 메모리 기반 풀 생성 + 즉시 초기화
 *
 * @param count: 풀 안에 만들 요소 개수.
 * @param size: 한 요소의 크기(바이트).
 * @param alignment: 요소 정렬(바이트). NVMe DMA 요구사항(보통 4 KiB)에 맞춰 호출자가 결정.
 * @param socket_id: NUMA 소켓 ID(NUMA 강제 시) 또는 SOCKET_ID_ANY(-1, 무관).
 * @return: 풀 핸들 또는 NULL(OOM).
 *
 * 동기/배경: 내부에서 spdk_dma_zmalloc 또는 hugepage 풀에서 count*size 만큼 DMA 친화 메모리를 할당.
 *   결과는 즉시 초기화 상태이므로 get/put을 바로 사용 가능.
 * 실행 컨텍스트: 디바이스 init mngt 단계, 단일 스레드.
 */
struct ftl_mempool *ftl_mempool_create(size_t count, size_t size,
				       size_t alignment, int socket_id);

/**
 * @brief Destroys the FTL memory pool

 * @param mpool The memory pool to be destroyed
 */
/*
 * [한국어]
 * ftl_mempool_destroy - DMA 풀 해제 — 내부 DMA 메모리도 함께 free
 *
 * @param mpool: ftl_mempool_create로 만든 풀 핸들.
 */
void ftl_mempool_destroy(struct ftl_mempool *mpool);

/**
 * @brief Gets (allocates) an element from the memory pool
 *
 * Allowed only for the initialized memory pool.
 *
 * @param mpool The memory pool
 *
 * @return Element from memory pool. If memory pool empty it returns NULL.
 */
/*
 * [한국어]
 * ftl_mempool_get - 풀에서 한 요소를 꺼냄(할당)
 *
 * @param mpool: 초기화된 풀 핸들.
 * @return: 요소 포인터 또는 NULL(풀 비어있음 — back pressure 신호).
 *
 * 동기화: 풀은 lockless가 아니므로 한 풀은 한 스레드에서만 호출. SPDK reactor affinity로 보장.
 * 호출자 의무: 사용 후 반드시 ftl_mempool_put으로 반환.
 */
void *ftl_mempool_get(struct ftl_mempool *mpool);

/**
 * @brief Puts (releases) the element to the memory pool
 *
 * Allowed only for the initialized memory pool.
 *
 * @param mpool The memory pool
 * @param element The element to be released
 */
/*
 * [한국어]
 * ftl_mempool_put - 요소를 풀로 반환
 *
 * @param mpool: 초기화된 풀 핸들.
 * @param element: 반환할 요소(이 풀에서 get된 것이어야 함).
 */
void ftl_mempool_put(struct ftl_mempool *mpool, void *element);

/**
 * @brief Creates custom FTL memory pool using memory allocated externally
 *
 * The pool is uninitialized.
 * The uninitialized pool is accessible via ftl_mempool_claim_df() and
 * ftl_mempool_release_df() APIs. The pool's free buffer list is initialized
 * to contain only elements that were not claimed using ftl_mempool_claim_df()
 * after the call to ftl_mempool_initialize_ext.
 * See ftl_mempool_initialize_ext().
 *
 * @param buffer Externally allocated underlying memory buffer
 * @param count Count of element in the memory pool
 * @param size Size of elements in the memory pool
 * @param alignment Memory alignment of element in the memory pool
 *
 * @return Pointer to the memory pool
 */
/*
 * [한국어]
 * ftl_mempool_create_ext - 외부에서 할당된 buffer 위에 uninit 풀 생성
 *
 * @param buffer: 호출자가 mmap 등으로 확보한 underlying memory.
 * @param count/size/alignment: 풀 요소 개수/크기/정렬.
 * @return: 풀 핸들(uninit 상태).
 *
 * 동기/배경: 슈퍼블록 영구 객체와 같은 베이스 주소를 공유하면서 ftl_df_obj_id로 영속 식별이 필요한
 *   메타데이터(예: L2P 캐시 페이지 디스크립터)를 위해 마련. 풀이 uninit인 동안에는 claim/release_df로
 *   "이미 사용 중인" 요소를 영속 ID로 등록해야 한다.
 * 라이프사이클: create_ext → 영구 데이터에 기록된 ID들을 모두 claim_df → initialize_ext →
 *   이후 일반 get/put 사용 가능. claim된 요소는 in-use 상태이므로 다시 사용자가 put하기 전까지 풀에 없음.
 */
struct ftl_mempool *ftl_mempool_create_ext(void *buffer, size_t count, size_t size,
		size_t alignment);

/**
 * @brief Destroys the FTL memory pool w/ externally allocated underlying mem buf
 *
 * The external buf is not being freed.
 *
 * @param mpool The memory pool to be destroyed
 */
/*
 * [한국어]
 * ftl_mempool_destroy_ext - 외부 buf 풀 해제(buffer는 호출자 책임)
 *
 * @param mpool: create_ext로 만든 풀 핸들.
 *
 * 동작: 핸들/내부 free list만 free. underlying buffer는 호출자가 munmap 등으로 별도 해제.
 */
void ftl_mempool_destroy_ext(struct ftl_mempool *mpool);

/**
 * @brief Initialize the FTL memory pool w/ externally allocated mem buf.
 *
 * The pool is initialized to contain only elements that were not claimed.
 * All claimed elements are considered to be in use and will be returned
 * to the pool via ftl_mempool_put() after initialization.
 * After the pool is initialized, it is only accessible via
 * ftl_mempool_get() and ftl_mempool_put() APIs.
 *
 * This function should only be called on an uninitialized pool (ie. created via ftl_mempool_create_ext).
 * Any attempt to initialize an already initialized pool (whether after calling ftl_mempool_create, or
 * calling ftl_mempool_initialize_ext twice) will result in an assert.
 *
 * Depending on the memory pool being initialized or not, the use of the
 * following APIs is as follows:
 * API					uninitialized pool		initialized pool
 * ftl_mempool_get()			disallowed			allowed
 * ftl_mempool_put()			disallowed			allowed
 * ftl_mempool_claim_df()		allowed				disallowed
 * ftl_mempool_release_df()		allowed				disallowed
 *
 * @param mpool The memory pool
 */
/*
 * [한국어]
 * ftl_mempool_initialize_ext - uninit ext 풀을 init 모드로 전환
 *
 * @param mpool: create_ext로 만든 uninit 풀.
 *
 * 동기/배경: 영속 데이터에 기록된 ID들을 모두 claim한 후, 나머지 요소들을 free list에 넣어 일반 사용 가능 상태로 만든다.
 *   호출 후에는 claim/release_df는 disallow되고 get/put만 허용. 두 번 호출 시 어서션 실패.
 */
void ftl_mempool_initialize_ext(struct ftl_mempool *mpool);

/**
 * @brief Return a df object id for a given pool element.
 *
 * @param mpool			The memory pool
 * @param df_obj_ptr		Pointer to the pool element
 *
 * @return df object id
 */
/*
 * [한국어]
 * ftl_mempool_get_df_obj_id - 풀 요소 포인터를 영속 ID(df_obj_id)로 변환
 *
 * @param mpool: 풀 핸들. @param df_obj_ptr: 풀에서 get한 요소 포인터.
 * @return: 영속 ID — 슈퍼블록 등에 기록 가능.
 *
 * 동기/배경: 다음 부팅에서 동일 객체를 식별하려면 hugepage 가상 주소가 아닌 베이스 오프셋이 필요.
 */
ftl_df_obj_id ftl_mempool_get_df_obj_id(struct ftl_mempool *mpool, void *df_obj_ptr);

/**
 * @brief Return an element pointer for a given df object id.
 *
 * @param mpool			The memory pool
 * @param df_obj_id		Df object id of a pool element
 *
 * @return Element ptr
 */
/*
 * [한국어]
 * ftl_mempool_get_df_ptr - 영속 ID를 풀 요소 포인터로 환원
 *
 * @param mpool: 풀 핸들. @param df_obj_id: 슈퍼블록에서 읽은 영속 ID.
 * @return: 인메모리 요소 포인터.
 */
void *ftl_mempool_get_df_ptr(struct ftl_mempool *mpool, ftl_df_obj_id df_obj_id);

/**
 * @brief Claim an element for use.
 *
 * Allowed only for the uninitialized memory pool.
 *
 * @param mpool			The memory pool
 * @param df_obj_id		Df object id of a pool element to claim
 *
 * @return Element ptr
 */
/*
 * [한국어]
 * ftl_mempool_claim_df - uninit 풀에서 영속 ID로 요소를 점유 표시
 *
 * @param mpool: uninit 풀 핸들.
 * @param df_obj_id: 영속 ID — 이전 부팅에서 슈퍼블록에 기록되었던 ID.
 * @return: 해당 요소의 인메모리 포인터.
 *
 * 동기/배경: 부팅 직후, 이전에 사용 중이었던 요소들을 풀에 "이미 점유 중"으로 표시.
 *   이후 initialize_ext 호출 시 점유되지 않은 요소들만 free list로 들어간다.
 */
void *ftl_mempool_claim_df(struct ftl_mempool *mpool, ftl_df_obj_id df_obj_id);

/**
 * @brief Release an element to the pool.
 *
 * Allowed only for the uninitialized memory pool.
 *
 * @param mpool			The memory pool
 * @param df_obj_id		Df object id of a pool element to claim
 */
/*
 * [한국어]
 * ftl_mempool_release_df - uninit 풀에서 점유 표시를 해제
 *
 * @param mpool: uninit 풀.
 * @param df_obj_id: 영속 ID.
 *
 * 사용 예: claim_df로 점유했다가 부팅 시점에 더 이상 유효하지 않은 객체로 판명되면 release.
 */
void ftl_mempool_release_df(struct ftl_mempool *mpool, ftl_df_obj_id df_obj_id);

/**
 * @brief Return an index for a given element in the memory pool.
 *
 * @param mpool			The memory pool
 * @param df_obj_ptr		Element from df memory pool. The pointer may be offset from the beginning of the element.
 *
 * @return Index (offset / element_size) of the element parameter from the beginning of the pool
 */
/*
 * [한국어]
 * ftl_mempool_get_df_obj_index - 요소의 풀 내 0-based 인덱스 산출
 *
 * @param mpool: 풀 핸들.
 * @param df_obj_ptr: 풀 요소(시작 주소 또는 요소 내 오프셋 가능).
 * @return: (포인터 - 베이스) / element_size — 0..count-1.
 *
 * 사용 예: 디버깅이나 통계, 외부 인덱싱 매핑.
 */
size_t ftl_mempool_get_df_obj_index(struct ftl_mempool *mpool, void *df_obj_ptr);
#endif /* FTL_MEMPOOL_H */
/* [한국어] 헤더 가드 종료. */
