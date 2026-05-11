/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/*
 * [한국어 설명] DPDK 메모리/메모존/멤풀/링/타이머 API의 SPDK 어댑터 (env.c)
 *
 * === 파일의 역할 ===
 * SPDK는 자체 메모리/스레드/타이머 추상화(spdk_*)를 외부에 제공하지만, 그 구현은 DPDK EAL의
 * rte_malloc, rte_memzone, rte_mempool, rte_ring, rte_cycles 함수들을 단순 위임하는 형태로
 * 한다. 본 파일은 그 위임 어댑터로서 다음을 모은다:
 *  - DMA-가능한 hugepage 메모리 할당/해제(spdk_malloc / spdk_dma_*).
 *  - 이름 붙은 영구 메모리 영역(spdk_memzone_*).
 *  - 객체 풀(spdk_mempool_*).
 *  - 타이머/지연(spdk_get_ticks, spdk_delay_us, spdk_pause).
 *  - lockless ring(spdk_ring_*).
 *  - CPU 어피니티 해제(spdk_unaffinitize_thread / call_unaffinitized).
 *  - 통계 덤프(spdk_env_dpdk_dump_mem_stats).
 *  - NUMA enforce 토글(mem_enforce_numa).
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/env_dpdk 서브시스템의 핵심 어댑터 파일. SPDK의 모든 모듈(NVMe 드라이버, bdev 코어,
 * thread/reactor, blob, sock 등)이 DMA 메모리·mempool·ring을 본 파일의 spdk_* 함수로 얻는다.
 * 호출 체인: 상위 모듈 → spdk_*(본 파일) → rte_*(DPDK).
 * 실행 컨텍스트: spdk_malloc/free 등은 어느 reactor에서나 호출 가능하며, 내부적으로 DPDK가
 * lockless heap을 제공하므로 별도 SPDK 락은 없다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h, spdk/util.h(spdk_max), spdk/env_dpdk.h, spdk/log.h, spdk/assert.h,
 *         env_internal.h, DPDK rte_malloc/memzone/mempool/cycles/version/eal.
 * - 본 파일에 의존하는 모듈: 사실상 모든 SPDK 라이브러리(lib/nvme의 PRP/SGL DMA 버퍼,
 *   lib/bdev의 bdev_io 풀, lib/thread의 메시지 ring 등).
 * - 데이터 흐름: SPDK 코드는 spdk_dma_zmalloc → 본 파일 → rte_zmalloc_socket →
 *   DPDK heap(hugepage 기반) → 가상주소 반환. 이 가상주소는 추후 vtophys로 IOVA로 변환되어
 *   NVMe DMA 디스크립터(PRP/SGL)에 사용된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_malloc/zmalloc/realloc/free: 정렬·NUMA 고려 일반 할당. flags=0 거부.
 * - spdk_dma_*: SPDK_MALLOC_DMA|SHARE 플래그를 자동으로 붙인 DMA 버전.
 * - spdk_memzone_reserve_aligned: rte_memzone(이름 붙은 영구 영역). IOVA contiguous 옵션.
 * - spdk_mempool_create_ctor: rte_mempool 생성. 캐시 크기를 lcore당 절반 이내로 자동 클램프.
 * - spdk_get_ticks/_hz/spdk_delay_us/spdk_pause: TSC 기반 타이머·지연.
 * - spdk_unaffinitize_thread / spdk_call_unaffinitized: 콜백 동안 CPU 마스크를 풀어 librdmacm
 *   같은 라이브러리가 자기 스레드를 만들 때 사용. TLS 플래그로 멱등성 보장.
 * - spdk_ring_*: rte_ring(SP/SC, MP/SC, MP/MC) 래퍼. 이름은 자동 생성(원자적 카운터).
 * - mem_enforce_numa: g_enforce_numa 토글. fallback 비활성화.
 */

/* [한국어] SPDK 표준 인클루드(stdint, stdio, errno 등). */
#include "spdk/stdinc.h"
/* [한국어] spdk_max 등 공용 유틸 매크로. cache line 정렬에 사용. */
#include "spdk/util.h"
/* [한국어] 본 파일이 구현하는 spdk_env_dpdk_* 공개 API 시그니처. */
#include "spdk/env_dpdk.h"
/* [한국어] SPDK_*LOG 매크로. */
#include "spdk/log.h"
/* [한국어] SPDK_STATIC_ASSERT 매크로. 컴파일 타임 상수 일치 검증에 사용. */
#include "spdk/assert.h"

/* [한국어] env_dpdk 내부 선언(mem_enforce_numa의 시그니처 등 포함). */
#include "env_internal.h"

/* [한국어] DPDK 컴파일 매크로. */
#include <rte_config.h>
/* [한국어] rte_get_timer_cycles, rte_delay_us, rte_pause 등 타이머/지연 API. */
#include <rte_cycles.h>
/* [한국어] rte_malloc_socket, rte_zmalloc_socket, rte_realloc, rte_free, rte_malloc_get_socket_stats. */
#include <rte_malloc.h>
/* [한국어] rte_mempool 생성/검색/get/put. */
#include <rte_mempool.h>
/* [한국어] rte_memzone_reserve_aligned 등 이름 붙은 영구 영역. */
#include <rte_memzone.h>
/* [한국어] DPDK 버전 매크로(혹시 모를 조건부 컴파일 대비). */
#include <rte_version.h>
/* [한국어] rte_eal_process_type(primary/secondary 판정). */
#include <rte_eal.h>

/* [한국어] 현재 스레드가 이미 어피니티 해제되었는지 표시하는 TLS 플래그.
 * spdk_unaffinitize_thread/call_unaffinitized가 멱등성을 가지도록 한다 — 같은 스레드에서
 * 두 번 풀어도 무해하고, call_unaffinitized 진입 시 이미 풀린 상태면 affinity 복원도 생략한다. */
static __thread bool g_is_thread_unaffinitized;
/* [한국어] NUMA 강제 모드 전역 플래그. true면 SOCKET_ID_ANY로의 fallback을 금지하여
 * 요청한 NUMA에서 실패 시 즉시 NULL 반환. mem_enforce_numa()로 set. 단조 증가(끄지 않음). */
static bool g_enforce_numa;

/* [한국어] DPDK SOCKET_ID_ANY와 SPDK SPDK_ENV_NUMA_ID_ANY가 동일한 정수값을 갖는지 컴파일 타임 검사.
 * 이 두 매크로는 본 파일에서 자주 상호 변환되므로 상수 mismatch가 발생하면 즉시 빌드 실패시킨다. */
SPDK_STATIC_ASSERT(SOCKET_ID_ANY == SPDK_ENV_NUMA_ID_ANY, "SOCKET_ID_ANY mismatch");

/*
 * [한국어]
 * spdk_malloc - hugepage 백엔드 일반 메모리 할당
 *
 * @size:    바이트 단위 크기.
 * @align:   정렬 요구. 캐시라인보다 작으면 캐시라인으로 올림.
 * @unused:  과거 IOVA 반환용 인자(현재는 사용 금지). NULL이어야 함.
 * @numa_id: 선호 NUMA. SPDK_ENV_NUMA_ID_ANY면 임의.
 * @flags:   SPDK_MALLOC_DMA, SPDK_MALLOC_SHARE 등 비트 플래그(0이면 거부).
 * @return:  할당된 가상주소, 실패 시 NULL.
 *
 * NUMA 우선 할당 후 g_enforce_numa가 false면 SOCKET_ID_ANY로 fallback한다.
 * DPDK rte_malloc은 hugepage 풀에서 cache-line aligned 할당을 제공하며, 결과 주소는
 * 곧 vtophys로 IOVA 변환이 가능하므로 이후 NVMe DMA 디스크립터에 그대로 넣을 수 있다.
 * 실행 컨텍스트: 어느 reactor에서나 호출 가능. DPDK heap이 lockless이므로 SPDK 락 불필요.
 *
 * 호출 체인: SPDK 모듈 → spdk_malloc → rte_malloc_socket
 */
void *
spdk_malloc(size_t size, size_t align, uint64_t *unused, int numa_id, uint32_t flags)
{
	void *buf;
	/* [한국어] 결과 포인터. */

	if (flags == 0 || unused != NULL) {
		/* [한국어] 새 API에서는 flags 필수, unused는 반드시 NULL. 어기면 즉시 NULL 반환하여
		 * 잘못된 사용을 빨리 드러낸다(보통 호출자 측 마이그레이션 실수). */
		return NULL;
	}

	align = spdk_max(align, RTE_CACHE_LINE_SIZE);
	/* [한국어] 캐시라인 미만 정렬은 의미 없음 — false sharing/IOMMU 정렬 요구를 피하려고
	 * 항상 캐시라인 이상으로 올림. */
	buf = rte_malloc_socket(NULL, size, align, numa_id);
	/* [한국어] DPDK heap에서 지정 NUMA로 할당. NULL 이름은 익명 할당. */
	if (buf == NULL && !g_enforce_numa && numa_id != SOCKET_ID_ANY) {
		/* [한국어] 1차 실패이고 NUMA 강제 모드가 아니며 요청이 특정 NUMA였다면 ANY로 fallback. */
		buf = rte_malloc_socket(NULL, size, align, SOCKET_ID_ANY);
	}
	return buf;
}

/*
 * [한국어]
 * spdk_zmalloc - 0으로 초기화되는 hugepage 할당
 *
 * spdk_malloc과 동일하나 rte_zmalloc_socket을 사용하여 결과 버퍼를 0으로 채운다.
 * NVMe Identify·SQE·CQE 등 헤더의 unused 필드를 안전하게 0으로 두기 위해 자주 쓰임.
 */
void *
spdk_zmalloc(size_t size, size_t align, uint64_t *unused, int numa_id, uint32_t flags)
{
	void *buf;
	/* [한국어] 결과 포인터. */

	if (flags == 0 || unused != NULL) {
		/* [한국어] spdk_malloc과 동일한 사전조건 검사. */
		return NULL;
	}

	align = spdk_max(align, RTE_CACHE_LINE_SIZE);
	/* [한국어] 캐시라인 정렬 강제. */
	buf = rte_zmalloc_socket(NULL, size, align, numa_id);
	/* [한국어] 0-초기화 NUMA 할당. */
	if (buf == NULL && !g_enforce_numa && numa_id != SOCKET_ID_ANY) {
		/* [한국어] NUMA fallback 동일 로직. */
		buf = rte_zmalloc_socket(NULL, size, align, SOCKET_ID_ANY);
	}
	return buf;
}

/*
 * [한국어]
 * spdk_realloc - 할당 크기 변경
 *
 * @buf:   기존 포인터(NULL이면 새 할당).
 * @size:  새 크기.
 * @align: 정렬 요구(캐시라인 미만이면 올림).
 *
 * rte_realloc은 가능하면 in-place로 확장하고 그렇지 못하면 새 영역 할당 후 복사하여
 * 기존 영역을 free한다. 따라서 호출자는 반환된 새 포인터를 사용해야 한다.
 */
void *
spdk_realloc(void *buf, size_t size, size_t align)
{
	align = spdk_max(align, RTE_CACHE_LINE_SIZE);
	/* [한국어] 정렬 보정. */
	return rte_realloc(buf, size, align);
	/* [한국어] DPDK realloc 위임. NUMA는 기존 buf의 소속 NUMA를 따름. */
}

/*
 * [한국어]
 * spdk_free - spdk_malloc/zmalloc 등으로 받은 버퍼 해제
 *
 * NULL 안전(rte_free 자체가 NULL을 무시).
 */
void
spdk_free(void *buf)
{
	rte_free(buf);
	/* [한국어] DPDK heap 반환. */
}

/*
 * [한국어]
 * spdk_dma_malloc_socket - DMA-가능 메모리 할당(NUMA 지정)
 *
 * SPDK_MALLOC_DMA|SHARE 플래그를 자동으로 더한 spdk_malloc 래퍼. 결과는 vtophys 변환 가능하며
 * NVMe PRP/SGL에 그대로 사용할 수 있다.
 */
void *
spdk_dma_malloc_socket(size_t size, size_t align, uint64_t *unused, int numa_id)
{
	return spdk_malloc(size, align, unused, numa_id, (SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE));
	/* [한국어] DMA + SHARE(다른 프로세스 매핑 허용)를 강제. */
}

/*
 * [한국어]
 * spdk_dma_zmalloc_socket - 0-초기화 DMA 메모리 할당(NUMA 지정)
 */
void *
spdk_dma_zmalloc_socket(size_t size, size_t align, uint64_t *unused, int numa_id)
{
	return spdk_zmalloc(size, align, unused, numa_id, (SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE));
	/* [한국어] DMA + SHARE 자동 적용. NVMe Identify 명령용 4KB 버퍼 등에 자주 쓰임. */
}

/*
 * [한국어]
 * spdk_dma_malloc - DMA-가능 메모리 할당(NUMA 미지정)
 */
void *
spdk_dma_malloc(size_t size, size_t align, uint64_t *unused)
{
	return spdk_dma_malloc_socket(size, align, unused, SPDK_ENV_NUMA_ID_ANY);
	/* [한국어] NUMA ANY로 위임 — DPDK가 적당한 노드에서 할당. */
}

/*
 * [한국어]
 * spdk_dma_zmalloc - 0-초기화 DMA 메모리 할당(NUMA 미지정)
 */
void *
spdk_dma_zmalloc(size_t size, size_t align, uint64_t *unused)
{
	return spdk_dma_zmalloc_socket(size, align, unused, SPDK_ENV_NUMA_ID_ANY);
	/* [한국어] 위와 동일하나 0-초기화. */
}

/*
 * [한국어]
 * spdk_dma_realloc - DMA 메모리 realloc
 *
 * @unused: 반드시 NULL.
 *
 * SPDK_MALLOC_DMA 플래그가 자동으로 유지되도록 spdk_realloc 대신 본 별도 함수가 존재.
 * unused 호환 검사 후 일반 rte_realloc을 호출.
 */
void *
spdk_dma_realloc(void *buf, size_t size, size_t align, uint64_t *unused)
{
	if (unused != NULL) {
		/* [한국어] 새 API: 반드시 NULL이어야 함. */
		return NULL;
	}
	align = spdk_max(align, RTE_CACHE_LINE_SIZE);
	/* [한국어] 캐시라인 정렬 강제. */
	return rte_realloc(buf, size, align);
	/* [한국어] 기존 hugepage heap 안에서 realloc(NUMA 정보는 기존 영역으로부터 추론). */
}

/*
 * [한국어]
 * spdk_dma_free - DMA 메모리 해제
 *
 * spdk_free와 동일하지만 dma 짝 API의 일관성을 위해 별도 노출.
 */
void
spdk_dma_free(void *buf)
{
	spdk_free(buf);
	/* [한국어] 단순 위임. */
}

/*
 * [한국어]
 * spdk_memzone_reserve_aligned - 이름 붙은 영구 메모리 영역 예약
 *
 * @name:    영역 이름(NULL 불가, 프로세스 내 유일).
 * @len:     크기.
 * @numa_id: 선호 NUMA.
 * @flags:   SPDK_MEMZONE_NO_IOVA_CONTIG 등.
 * @align:   정렬 요구.
 * @return:  영역 시작 가상주소(성공) 또는 NULL.
 *
 * rte_memzone은 EAL 영역 안의 hugepage 풀에서 "이름 붙은" 메모리를 예약하며, 한 번 만들어진
 * 영역은 spdk_memzone_lookup으로 다시 찾을 수 있다(DPDK primary/secondary 모드에서 공유 가능).
 * 기본적으로 IOVA contiguous(연속 IOVA)를 요청하지만 NO_IOVA_CONTIG 플래그로 해제 가능하다.
 * 성공 시 0으로 초기화한 후 반환한다(rte_memzone은 zero-fill을 자동 보장하지 않음).
 *
 * 호출 체인: SPDK 모듈 → spdk_memzone_reserve_aligned → rte_memzone_reserve_aligned
 */
void *
spdk_memzone_reserve_aligned(const char *name, size_t len, int numa_id,
			     unsigned flags, unsigned align)
{
	const struct rte_memzone *mz;
	/* [한국어] DPDK 측 핸들. */
	unsigned dpdk_flags = 0;
	/* [한국어] DPDK rte_memzone 플래그로 변환된 값. */

	if ((flags & SPDK_MEMZONE_NO_IOVA_CONTIG) == 0) {
		/* [한국어] 별도 옵트아웃이 없으면 IOVA contiguous 강제 — NVMe DMA 시 단일 PRP로 처리 가능. */
		dpdk_flags |= RTE_MEMZONE_IOVA_CONTIG;
	}

	if (numa_id == SPDK_ENV_NUMA_ID_ANY) {
		/* [한국어] SPDK 상수 → DPDK 상수 변환(둘 다 같은 값임은 위 STATIC_ASSERT로 보장). */
		numa_id = SOCKET_ID_ANY;
	}

	mz = rte_memzone_reserve_aligned(name, len, numa_id, dpdk_flags, align);
	/* [한국어] DPDK가 이름·NUMA·플래그·정렬을 만족하는 hugepage 청크를 찾아 예약. */
	if (mz == NULL && !g_enforce_numa && numa_id != SOCKET_ID_ANY) {
		/* [한국어] NUMA 강제 모드가 아니면 SOCKET_ID_ANY로 fallback. */
		mz = rte_memzone_reserve_aligned(name, len, SOCKET_ID_ANY, dpdk_flags, align);
	}

	if (mz != NULL) {
		memset(mz->addr, 0, len);
		/* [한국어] 명시적 0-초기화 — rte_memzone은 자동 zero가 아니므로 SPDK가 보장. */
		return mz->addr;
		/* [한국어] 사용자에게는 가상주소만 노출(rte_memzone 핸들은 캡슐화). */
	} else {
		return NULL;
	}
}

/*
 * [한국어]
 * spdk_memzone_reserve - 캐시라인 정렬 기본값으로 reserve_aligned 호출
 *
 * SPDK 대다수 코드에서 사용하는 기본 진입점. 정렬을 명시할 필요가 없을 때 사용.
 */
void *
spdk_memzone_reserve(const char *name, size_t len, int numa_id, unsigned flags)
{
	return spdk_memzone_reserve_aligned(name, len, numa_id, flags,
					    RTE_CACHE_LINE_SIZE);
	/* [한국어] 정렬은 캐시라인 크기 — false sharing 방지. */
}

/*
 * [한국어]
 * spdk_memzone_lookup - 이름으로 이미 만들어진 memzone의 가상주소 검색
 *
 * @name: 찾을 영역 이름.
 * @return: 가상주소(있음) 또는 NULL(없음).
 *
 * primary가 만들어둔 memzone을 secondary가 같은 이름으로 찾아 attach할 때 자주 사용.
 */
void *
spdk_memzone_lookup(const char *name)
{
	const struct rte_memzone *mz = rte_memzone_lookup(name);
	/* [한국어] DPDK 등록 테이블에서 이름 검색. */

	if (mz != NULL) {
		return mz->addr;
		/* [한국어] 가상주소만 노출. */
	} else {
		return NULL;
	}
}

/*
 * [한국어]
 * spdk_memzone_free - 이름으로 memzone 해제
 *
 * @return: 성공 0, 없거나 실패 시 -1.
 */
int
spdk_memzone_free(const char *name)
{
	const struct rte_memzone *mz = rte_memzone_lookup(name);
	/* [한국어] 핸들 획득. */

	if (mz != NULL) {
		return rte_memzone_free(mz);
		/* [한국어] DPDK가 reference를 추적하여 마지막 참조 해제 시 hugepage 반환. */
	}

	return -1;
	/* [한국어] 이름이 없으면 -1. */
}

/*
 * [한국어]
 * spdk_memzone_dump - 모든 memzone 정보를 파일에 덤프(디버그)
 */
void
spdk_memzone_dump(FILE *f)
{
	rte_memzone_dump(f);
	/* [한국어] DPDK가 헤더 + 각 항목을 텍스트로 출력. */
}

/*
 * [한국어]
 * spdk_mempool_create_ctor - 사용자 정의 객체 생성자(constructor) 포함 mempool 생성
 *
 * @name:         풀 이름.
 * @count:        총 원소 수.
 * @ele_size:     원소 크기.
 * @cache_size:   per-lcore 캐시 크기(자동 클램프됨).
 * @numa_id:      선호 NUMA.
 * @obj_init:     각 원소 1회 호출되는 초기화 콜백(NULL 허용).
 * @obj_init_arg: 콜백 인자.
 * @return:       성공 시 spdk_mempool 포인터, 실패 NULL.
 *
 * rte_mempool은 lockless ring 기반 객체 풀이며, per-lcore 캐시로 hot path에서 atomic 없는
 * 빠른 get/put을 제공한다. 본 함수는 캐시 크기를 (count/2)/lcore 이하로 자동 클램프하여
 * 캐시가 풀 절반 이상을 잠식하지 않도록 한다.
 *
 * 호출 체인: lib/bdev/lib/nvme 등 풀 생성 코드 → spdk_mempool_create_ctor → rte_mempool_create
 */
struct spdk_mempool *
spdk_mempool_create_ctor(const char *name, size_t count,
			 size_t ele_size, size_t cache_size, int numa_id,
			 spdk_mempool_obj_cb_t *obj_init, void *obj_init_arg)
{
	struct rte_mempool *mp;
	/* [한국어] DPDK 측 풀 핸들. */
	size_t tmp;
	/* [한국어] 캐시 클램프 계산용 임시. */

	if (numa_id == SPDK_ENV_NUMA_ID_ANY) {
		/* [한국어] SPDK ANY → DPDK ANY 변환. */
		numa_id = SOCKET_ID_ANY;
	}

	/* No more than half of all elements can be in cache */
	tmp = (count / 2) / rte_lcore_count();
	/* [한국어] (전체 원소의 절반)을 lcore 수로 나눈 값이 lcore당 캐시 상한.
	 * 이렇게 하면 모든 lcore의 캐시 합이 풀 절반을 넘지 않도록 보장 → starvation 방지. */
	if (cache_size > tmp) {
		cache_size = tmp;
		/* [한국어] 사용자가 너무 큰 값을 주면 자동 축소. */
	}

	if (cache_size > RTE_MEMPOOL_CACHE_MAX_SIZE) {
		cache_size = RTE_MEMPOOL_CACHE_MAX_SIZE;
		/* [한국어] DPDK가 정한 절대 상한도 적용. */
	}

	mp = rte_mempool_create(name, count, ele_size, cache_size,
				0, NULL, NULL, (rte_mempool_obj_cb_t *)obj_init, obj_init_arg,
				numa_id, 0);
	/* [한국어] private_data_size=0, mp_init=NULL/arg=NULL(풀 자체 초기화 불필요),
	 * obj_init=사용자 콜백(각 원소를 1회 초기화), flags=0(기본 SP/SC 캐시 등). */
	if (mp == NULL && !g_enforce_numa && numa_id != SOCKET_ID_ANY) {
		/* [한국어] NUMA fallback. */
		mp = rte_mempool_create(name, count, ele_size, cache_size,
					0, NULL, NULL, (rte_mempool_obj_cb_t *)obj_init, obj_init_arg,
					SOCKET_ID_ANY, 0);
	}

	return (struct spdk_mempool *)mp;
	/* [한국어] DPDK 핸들을 SPDK 추상 핸들로 캐스팅 — 호출자는 내부를 모른 채 사용. */
}


/*
 * [한국어]
 * spdk_mempool_create - 객체 초기화 콜백이 없는 단순 mempool 생성
 *
 * 단순히 _ctor에 NULL 콜백을 전달하는 래퍼.
 */
struct spdk_mempool *
spdk_mempool_create(const char *name, size_t count,
		    size_t ele_size, size_t cache_size, int numa_id)
{
	return spdk_mempool_create_ctor(name, count, ele_size, cache_size, numa_id,
					NULL, NULL);
	/* [한국어] obj_init=NULL → 원소를 그냥 zero·미초기화 상태로 둠. */
}

/*
 * [한국어]
 * spdk_mempool_get_name - mempool 이름 문자열 반환(디버그용)
 */
char *
spdk_mempool_get_name(struct spdk_mempool *mp)
{
	return ((struct rte_mempool *)mp)->name;
	/* [한국어] DPDK 핸들로 캐스팅 후 .name 멤버 반환. */
}

/*
 * [한국어]
 * spdk_mempool_free - mempool 해제
 */
void
spdk_mempool_free(struct spdk_mempool *mp)
{
	rte_mempool_free((struct rte_mempool *)mp);
	/* [한국어] DPDK가 ring·메모리 chunk를 모두 반환. NULL 안전. */
}

/*
 * [한국어]
 * spdk_mempool_get - 풀에서 객체 1개 꺼내기
 *
 * @return: 성공 시 객체 포인터, 비어있으면 NULL.
 *
 * lockless per-lcore 캐시 → 공통 ring 순서로 시도. polled-mode 핫패스에서 매우 자주 호출.
 */
void *
spdk_mempool_get(struct spdk_mempool *mp)
{
	void *ele = NULL;
	/* [한국어] 출력 변수 초기화 — rte_mempool_get가 실패 시 ele를 건드리지 않을 가능성 대비. */
	int rc;
	/* [한국어] 0=성공, -ENOENT=빔. */

	rc = rte_mempool_get((struct rte_mempool *)mp, &ele);
	if (rc != 0) {
		/* [한국어] 풀이 비었으면 NULL 반환 — SPDK에서는 명시적 NULL 시그널을 사용. */
		return NULL;
	}
	return ele;
}

/*
 * [한국어]
 * spdk_mempool_get_bulk - 풀에서 count개 객체 한 번에 꺼내기
 *
 * @return: 0 성공, 음수 실패(요청 수만큼 못 꺼내면 모두 반환되지 않음 — atomic 동작).
 */
int
spdk_mempool_get_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count)
{
	return rte_mempool_get_bulk((struct rte_mempool *)mp, ele_arr, count);
	/* [한국어] 캐시 활용 + ring 단일 호출 → 호출당 비용 분할상환. */
}

/*
 * [한국어]
 * spdk_mempool_put - 풀에 객체 1개 반환
 */
void
spdk_mempool_put(struct spdk_mempool *mp, void *ele)
{
	rte_mempool_put((struct rte_mempool *)mp, ele);
	/* [한국어] 캐시가 차면 일부를 ring으로 flush. */
}

/*
 * [한국어]
 * spdk_mempool_put_bulk - 풀에 count개 객체 한 번에 반환
 */
void
spdk_mempool_put_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count)
{
	rte_mempool_put_bulk((struct rte_mempool *)mp, ele_arr, count);
	/* [한국어] bulk get의 짝. */
}

/*
 * [한국어]
 * spdk_mempool_count - 현재 풀에 남은 객체 수(추정)
 *
 * 정확값이 아닐 수 있음(per-lcore 캐시·동시 접근 때문에 근사치). 상태 모니터링용.
 */
size_t
spdk_mempool_count(const struct spdk_mempool *pool)
{
	return rte_mempool_avail_count((struct rte_mempool *)pool);
	/* [한국어] DPDK가 ring·캐시 카운터를 합산하여 반환. */
}

/*
 * [한국어]
 * spdk_mempool_obj_iter - 풀의 모든 객체에 대해 콜백 호출
 *
 * @return: 순회된 객체 수.
 *
 * 풀 정리·통계 수집·일괄 재초기화 등에 사용. polling 핫패스에서 호출 금지.
 */
uint32_t
spdk_mempool_obj_iter(struct spdk_mempool *mp, spdk_mempool_obj_cb_t obj_cb,
		      void *obj_cb_arg)
{
	return rte_mempool_obj_iter((struct rte_mempool *)mp, (rte_mempool_obj_cb_t *)obj_cb,
				    obj_cb_arg);
	/* [한국어] DPDK가 모든 청크의 모든 원소를 순차 방문. */
}

/*
 * [한국어]
 * struct env_mempool_mem_iter_ctx - mem_iter 콜백 어댑터 컨텍스트
 *
 * SPDK는 (mp, arg, addr, iova, len, idx) 시그니처를 사용하지만 DPDK는 (mp, opaque, memhdr, idx)
 * 시그니처를 사용한다. 어댑터에서 memhdr을 풀어 SPDK 시그니처로 재호출하기 위한 컨텍스트이다.
 */
struct env_mempool_mem_iter_ctx {
	spdk_mempool_mem_cb_t *user_cb;
	/* [한국어] 사용자 콜백 함수 포인터.
	 * 설정자: spdk_mempool_mem_iter 진입 시 스택에 만들어 채움.
	 * 읽는 자: mempool_mem_iter_remap.
	 * 값 범위: 비-NULL 함수 포인터.
	 * 동기화: 스택 변수, 호출 동안만 유효 — 별도 락 불필요. */
	void *user_arg;
	/* [한국어] 사용자 콜백 인자.
	 * 설정자/읽는 자/동기화: user_cb와 동일. */
};

/*
 * [한국어]
 * mempool_mem_iter_remap - DPDK→SPDK 시그니처 어댑터
 *
 * @mp:      DPDK 풀 핸들.
 * @opaque:  env_mempool_mem_iter_ctx 포인터.
 * @memhdr:  현재 메모리 청크 헤더(addr/iova/len 포함).
 * @mem_idx: 청크 인덱스.
 *
 * memhdr에서 SPDK 콜백이 기대하는 인자를 풀어내 user_cb를 호출.
 *
 * 호출 체인: spdk_mempool_mem_iter → rte_mempool_mem_iter(DPDK) → mempool_mem_iter_remap → user_cb
 */
static void
mempool_mem_iter_remap(struct rte_mempool *mp, void *opaque, struct rte_mempool_memhdr *memhdr,
		       unsigned mem_idx)
{
	struct env_mempool_mem_iter_ctx *ctx = opaque;
	/* [한국어] opaque를 어댑터 컨텍스트로 캐스팅. */

	ctx->user_cb((struct spdk_mempool *)mp, ctx->user_arg, memhdr->addr, memhdr->iova, memhdr->len,
		     mem_idx);
	/* [한국어] DPDK 측 핸들을 SPDK 추상 핸들로 캐스팅하고 청크 정보(addr/iova/len)와 인덱스 전달. */
}

/*
 * [한국어]
 * spdk_mempool_mem_iter - mempool의 각 메모리 청크에 대해 콜백 실행
 *
 * @return: 순회된 청크 수.
 *
 * NVMe DMA 등록 시점에 풀의 모든 청크를 IOMMU에 등록해야 할 때 사용.
 */
uint32_t
spdk_mempool_mem_iter(struct spdk_mempool *mp, spdk_mempool_mem_cb_t mem_cb,
		      void *mem_cb_arg)
{
	struct env_mempool_mem_iter_ctx ctx = {
		.user_cb = mem_cb,
		.user_arg = mem_cb_arg
	};
	/* [한국어] 어댑터 컨텍스트를 스택에 구성 — 본 함수 반환 전까지만 유효. */

	return rte_mempool_mem_iter((struct rte_mempool *)mp, mempool_mem_iter_remap, &ctx);
	/* [한국어] DPDK 순회기에 어댑터를 위임. */
}

/*
 * [한국어]
 * spdk_mempool_lookup - 이름으로 mempool 검색
 */
struct spdk_mempool *
spdk_mempool_lookup(const char *name)
{
	return (struct spdk_mempool *)rte_mempool_lookup(name);
	/* [한국어] DPDK 등록 테이블에서 이름 검색. NULL이면 없음. */
}

/*
 * [한국어]
 * spdk_process_is_primary - 현재 프로세스가 DPDK primary인지 확인
 *
 * @return: primary면 true, secondary면 false.
 *
 * DPDK는 메모리·디바이스 소유권을 primary 1개와 secondary N개로 나눈다. 자원 생성/해제는
 * 보통 primary만 수행해야 하므로 SPDK 코드가 본 함수로 분기한다.
 */
bool
spdk_process_is_primary(void)
{
	return (rte_eal_process_type() == RTE_PROC_PRIMARY);
	/* [한국어] DPDK가 EAL init 시 결정한 프로세스 종류 비교. */
}

/*
 * [한국어]
 * spdk_get_ticks - TSC 기반 타이머 카운터 읽기
 *
 * @return: 단조 증가하는 64비트 카운터.
 *
 * polled-mode SPDK는 wall-clock 대신 TSC(Time Stamp Counter)를 사용해 마이크로초 단위
 * 타임아웃·통계를 측정한다. rte_get_timer_cycles는 적절한 invariant TSC source를 활용.
 */
uint64_t
spdk_get_ticks(void)
{
	return rte_get_timer_cycles();
	/* [한국어] DPDK가 cpu별 보정·sync를 마친 TSC 값 반환. */
}

/*
 * [한국어]
 * spdk_get_ticks_hz - 타이머 주파수 (Hz)
 *
 * @return: spdk_get_ticks의 1초당 증가량.
 */
uint64_t
spdk_get_ticks_hz(void)
{
	return rte_get_timer_hz();
	/* [한국어] 보통 CPU base clock(예: 2.4 GHz → 2_400_000_000). */
}

/*
 * [한국어]
 * spdk_delay_us - 지정 마이크로초만큼 busy-loop 지연
 *
 * @us: 지연 시간(마이크로초).
 *
 * polled-mode 컨텍스트에서는 sleep 호출이 reactor 정지를 의미하므로, 매우 짧은 지연은
 * busy-loop이 더 합리적이다. rte_delay_us는 TSC 폴링 기반.
 * 실행 컨텍스트: 극히 짧은 지연(수~수백 us)에만 사용 권장.
 */
void
spdk_delay_us(unsigned int us)
{
	rte_delay_us(us);
	/* [한국어] DPDK가 TSC를 spin하면서 us 경과를 기다림. */
}

/*
 * [한국어]
 * spdk_pause - CPU pause 인스트럭션 1회 실행
 *
 * spinlock·polling 루프 내부에서 호출하여 hyper-thread 형제에게 자원을 양보.
 * x86은 PAUSE, ARM은 YIELD/wfe로 구현됨.
 */
void
spdk_pause(void)
{
	rte_pause();
	/* [한국어] inline asm pause/yield. 비용은 매우 낮음. */
}

/*
 * [한국어]
 * spdk_unaffinitize_thread - 현재 스레드의 CPU 어피니티를 모든 코어로 풀기
 *
 * librdmacm 등 일부 라이브러리는 자기 보조 스레드를 만들 때 호출 스레드의 어피니티를
 * 상속한다. SPDK reactor 스레드는 1코어에 핀되어 있으므로 그 보조 스레드도 같은 코어에
 * 핀되면 reactor와 경합한다. 이를 막기 위해 라이브러리 호출 직전 어피니티를 풀어 모든
 * 코어로 만든다.
 *
 * TLS 플래그(g_is_thread_unaffinitized)로 멱등성 확보 — 이미 풀린 스레드는 즉시 반환.
 * 실행 컨텍스트: reactor 스레드. 호출 후에는 다시 reactor 코어로 핀하지 않음에 유의
 * (call_unaffinitized 형태가 더 안전).
 */
void
spdk_unaffinitize_thread(void)
{
	rte_cpuset_t new_cpuset;
	/* [한국어] sched_setaffinity에 넘길 마스크. */
	long num_cores, i;
	/* [한국어] 시스템 전체 코어 수 및 순회 변수. */

	if (g_is_thread_unaffinitized) {
		/* [한국어] 이미 풀려있으면 아무 일도 하지 않음. */
		return;
	}

	CPU_ZERO(&new_cpuset);
	/* [한국어] 마스크 초기화. */

	num_cores = sysconf(_SC_NPROCESSORS_CONF);
	/* [한국어] 시스템 설정상의 CPU 개수(online 여부 무관). */

	/* Create a mask containing all CPUs */
	for (i = 0; i < num_cores; i++) {
		CPU_SET(i, &new_cpuset);
		/* [한국어] 0..num_cores-1까지 모두 set — 결과적으로 어느 코어에서나 실행 가능한 상태. */
	}

	rte_thread_set_affinity(&new_cpuset);
	/* [한국어] DPDK 래퍼가 sched_setaffinity 호출. 본 스레드 한정. */
	g_is_thread_unaffinitized = true;
	/* [한국어] TLS 플래그를 세워 다음 호출 시 빠르게 short-circuit. */
}

/*
 * [한국어]
 * spdk_call_unaffinitized - 콜백을 어피니티 해제 상태에서 실행 후 복원
 *
 * @cb:  실행할 콜백.
 * @arg: 콜백 인자.
 * @return: cb의 반환값.
 *
 * cb 호출 동안만 어피니티를 풀고, 반환 직후 원래 어피니티를 복원한다. 따라서 reactor의 코어 핀
 * 상태가 보존된다. 이미 unaffinitized 상태였다면 그냥 cb를 호출(복원/저장 생략).
 */
void *
spdk_call_unaffinitized(void *cb(void *arg), void *arg)
{
	rte_cpuset_t orig_cpuset;
	/* [한국어] cb 실행 전 원래 어피니티 백업. */
	void *ret;
	/* [한국어] cb의 반환값. */

	if (cb == NULL) {
		/* [한국어] 방어적 검사 — 잘못된 사용 즉시 NULL 반환. */
		return NULL;
	}

	if (g_is_thread_unaffinitized) {
		/* [한국어] 이미 풀려있으면 단순 호출 — 복원할 원본이 없으므로 백업 불필요. */
		ret = cb(arg);
	} else {
		rte_thread_get_affinity(&orig_cpuset);
		/* [한국어] 현재 어피니티 백업. */
		spdk_unaffinitize_thread();
		/* [한국어] 어피니티 풀기 + g_is_thread_unaffinitized=true. */

		ret = cb(arg);
		/* [한국어] 사용자 콜백 실행 — 이 동안 cb가 만든 자식 스레드는 모든 코어 어피니티를 상속. */

		rte_thread_set_affinity(&orig_cpuset);
		/* [한국어] reactor 코어 핀을 복원 — SPDK polled-mode 보존. */
		g_is_thread_unaffinitized = false;
		/* [한국어] TLS 플래그 원복. */
	}

	return ret;
}

/*
 * [한국어]
 * spdk_ring_create - lockless ring 생성
 *
 * @type:    SP_SC / MP_SC / MP_MC.
 * @count:   링 깊이(원소 수).
 * @numa_id: 선호 NUMA.
 * @return:  성공 시 spdk_ring 핸들, 실패 NULL.
 *
 * SPDK reactor 간 메시지 전달, bdev_io 큐 등에 사용되는 핵심 자료구조.
 * 이름은 "ring_<원자카운터>_<pid>" 형식으로 자동 생성하므로 호출자가 충돌 걱정을 안 해도 된다.
 *
 * 호출 체인: lib/thread/lib/bdev → spdk_ring_create → rte_ring_create
 */
struct spdk_ring *
spdk_ring_create(enum spdk_ring_type type, size_t count, int numa_id)
{
	char ring_name[64];
	/* [한국어] 자동 생성 이름 버퍼. */
	static uint32_t ring_num = 0;
	/* [한국어] 프로세스 내 ring 순번. atomic FETCH_ADD로 증가. */
	unsigned flags = RING_F_EXACT_SZ;
	/* [한국어] EXACT_SZ: count를 정확히 그 크기로 만들도록 DPDK에 요청
	 * (default는 가장 가까운 2의 거듭제곱으로 올림). */
	struct rte_ring *ring;
	/* [한국어] DPDK 측 핸들. */

	switch (type) {
	case SPDK_RING_TYPE_SP_SC:
		/* [한국어] Single Producer / Single Consumer — 가장 빠르지만 1:1 통신 한정. */
		flags |= RING_F_SP_ENQ | RING_F_SC_DEQ;
		break;
	case SPDK_RING_TYPE_MP_SC:
		/* [한국어] Multi Producer / Single Consumer — fan-in. enqueue는 atomic CAS, dequeue는 단일. */
		flags |= RING_F_SC_DEQ;
		break;
	case SPDK_RING_TYPE_MP_MC:
		/* [한국어] Multi Producer / Multi Consumer — 양쪽 모두 atomic. 가장 일반적이지만 느림. */
		flags |= 0;
		break;
	default:
		/* [한국어] 알 수 없는 타입 → 방어적 NULL 반환. */
		return NULL;
	}

	snprintf(ring_name, sizeof(ring_name), "ring_%u_%d",
		 __atomic_fetch_add(&ring_num, 1, __ATOMIC_RELAXED), getpid());
	/* [한국어] 이름 자동 생성. atomic_fetch_add로 카운터를 증가시켜 멀티스레드 동시 생성에도 안전.
	 * RELAXED 메모리 순서면 충분(이름 충돌 회피만 보장하면 됨). pid를 섞어 secondary 프로세스와도 분리. */

	ring = rte_ring_create(ring_name, count, numa_id, flags);
	/* [한국어] DPDK가 hugepage에서 ring buffer 할당 + 등록. */
	if (ring == NULL && !g_enforce_numa && numa_id != SOCKET_ID_ANY) {
		/* [한국어] NUMA fallback. */
		ring = rte_ring_create(ring_name, count, SOCKET_ID_ANY, flags);
	}
	return (struct spdk_ring *)ring;
	/* [한국어] DPDK 핸들을 SPDK 추상 핸들로 캐스팅. */
}

/*
 * [한국어]
 * spdk_ring_free - ring 해제
 */
void
spdk_ring_free(struct spdk_ring *ring)
{
	rte_ring_free((struct rte_ring *)ring);
	/* [한국어] DPDK가 ring 메모리·등록 해제. */
}

/*
 * [한국어]
 * spdk_ring_count - ring에 들어있는 객체 수(근사)
 */
size_t
spdk_ring_count(struct spdk_ring *ring)
{
	return rte_ring_count((struct rte_ring *)ring);
	/* [한국어] head/tail 차이로 계산 — 동시 접근 중에는 약간 오차 가능. */
}

/*
 * [한국어]
 * spdk_ring_enqueue - ring에 count개를 한꺼번에 enqueue (전부 또는 전부 X)
 *
 * @objs:       enqueue할 객체 포인터 배열.
 * @count:      개수.
 * @free_space: (선택) 호출 후 남은 빈 자리 수 출력.
 * @return:     성공적으로 enqueue된 개수(보통 count 또는 0).
 */
size_t
spdk_ring_enqueue(struct spdk_ring *ring, void **objs, size_t count,
		  size_t *free_space)
{
	return rte_ring_enqueue_bulk((struct rte_ring *)ring, objs, count,
				     (unsigned int *)free_space);
	/* [한국어] bulk 모드: 자리가 모자라면 0 반환(부분 enqueue 안 함) — atomic 시맨틱 보장. */
}

/*
 * [한국어]
 * spdk_ring_dequeue - ring에서 최대 count개 dequeue (있는 만큼)
 *
 * @return: 실제 꺼낸 개수.
 */
size_t
spdk_ring_dequeue(struct spdk_ring *ring, void **objs, size_t count)
{
	return rte_ring_dequeue_burst((struct rte_ring *)ring, objs, count, NULL);
	/* [한국어] burst 모드: 가능한 만큼만 꺼낸다(부분 dequeue 허용) — polled 핫패스에 적합. */
}

/*
 * [한국어]
 * spdk_env_dpdk_dump_mem_stats - 모든 DPDK 메모리 통계를 파일로 덤프
 *
 * RPC 또는 SIGUSR로 호출되는 디버그 헬퍼. polling 핫패스에서 호출 금지(파일 I/O).
 */
void
spdk_env_dpdk_dump_mem_stats(FILE *file)
{
	fprintf(file, "DPDK memory size %" PRIu64 "\n", rte_eal_get_physmem_size());
	/* [한국어] 전체 hugepage 풀 크기. */
	fprintf(file, "DPDK memory layout\n");
	rte_dump_physmem_layout(file);
	/* [한국어] hugepage 청크별 물리/가상 주소 매핑. */
	fprintf(file, "DPDK memzones.\n");
	rte_memzone_dump(file);
	/* [한국어] 모든 memzone 목록. */
	fprintf(file, "DPDK mempools.\n");
	rte_mempool_list_dump(file);
	/* [한국어] 모든 mempool 목록과 사용량. */
	fprintf(file, "DPDK malloc stats.\n");
	rte_malloc_dump_stats(file, NULL);
	/* [한국어] heap 통계. */
	fprintf(file, "DPDK malloc heaps.\n");
	rte_malloc_dump_heaps(file);
	/* [한국어] 노드별 heap 상세. */
}

/*
 * [한국어]
 * spdk_env_dpdk_get_mem_stats - 특정 NUMA의 hugepage heap 통계 구조체 채우기
 *
 * @stats:   출력 구조체. NULL 거부.
 * @numa_id: NUMA 노드 ID.
 * @return:  0 성공, -EINVAL/음수 실패.
 *
 * 텔레메트리/RPC에서 한 노드 단위 통계를 수집할 때 사용.
 */
int
spdk_env_dpdk_get_mem_stats(struct spdk_env_dpdk_mem_stats *stats, uint32_t numa_id)
{
	struct rte_malloc_socket_stats socket_stats = {};
	/* [한국어] DPDK 측 통계 구조체. 0으로 초기화하여 미사용 필드 노이즈 제거. */
	int rc;
	/* [한국어] DPDK 반환 코드. */

	if (stats == NULL) {
		/* [한국어] 출력 인자 NULL 거부. */
		return -EINVAL;
	}

	rc = rte_malloc_get_socket_stats(numa_id, &socket_stats);
	/* [한국어] 노드별 heap 통계 조회. 노드가 비활성이면 음수 반환. */
	if (rc != 0) {
		return rc;
		/* [한국어] DPDK 에러 그대로 전파. */
	}

	stats->heap_totalsz_bytes = socket_stats.heap_totalsz_bytes;
	/* [한국어] 노드 heap 총 크기. */
	stats->heap_freesz_bytes = socket_stats.heap_freesz_bytes;
	/* [한국어] 현재 자유 크기. */
	stats->greatest_free_size = socket_stats.greatest_free_size;
	/* [한국어] 가장 큰 단일 자유 chunk 크기 — 큰 할당이 가능한지 가늠. */
	stats->heap_allocsz_bytes = socket_stats.heap_allocsz_bytes;
	/* [한국어] 현재 할당된 크기. */
	stats->free_count = socket_stats.free_count;
	/* [한국어] 자유 chunk 수. */
	stats->alloc_count = socket_stats.alloc_count;
	/* [한국어] 할당된 chunk 수. */

	return 0;
}

/*
 * [한국어]
 * spdk_get_tid - 현재 스레드의 OS TID 반환
 *
 * @return: rte_sys_gettid 결과(Linux의 SYS_gettid). 로그에 스레드 식별자 표시 등에 사용.
 */
int
spdk_get_tid(void)
{
	return rte_sys_gettid();
	/* [한국어] Linux: gettid syscall 1회. cache되지 않을 수 있음. */
}

/*
 * [한국어]
 * mem_enforce_numa - NUMA 강제 모드 활성화 (env_internal.h 선언과 일치)
 *
 * SPDK CLI의 --strict-numa 같은 옵션이 주어진 경우 호출되어 g_enforce_numa를 true로 둔다.
 * 한 번 켜지면 끄지 않으며, 이후 본 파일의 모든 NUMA fallback 분기가 비활성화된다.
 *
 * 호출 체인: spdk_env_dpdk_post_init(옵션 파싱) → mem_enforce_numa
 */
void
mem_enforce_numa(void)
{
	g_enforce_numa = true;
	/* [한국어] 단순 토글. 이후 spdk_malloc/zmalloc/memzone/mempool/ring의 NUMA fallback 차단. */
}
