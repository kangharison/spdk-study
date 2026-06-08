/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK 메모리 매핑 및 vtophys(가상→IOVA) 변환 핵심 (memory.c)
 *
 * === 파일의 역할 ===
 * SPDK가 NVMe DMA에 사용할 hugepage 가상주소를 IOVA(또는 물리주소)로 변환하기 위한
 * 페이지 단위 룩업 트리 인프라를 구현한다. 핵심은 256TB 가상주소 공간을 1GB 단위로 색인하는
 * 2단계 트리(map_256tb → map_1gb2mb)와, 필요 시 2MB → 4KB로 떨어지는 3단계 fallback
 * (map_1gb4kb → map_2mb4kb)이다. 이 트리는 모든 spdk_mem_map(범용 vtophys 룩업 객체)이
 * 공유하는 인덱싱 스킴이며, 본 파일은:
 *  - spdk_mem_map 생성/해제 API(spdk_mem_map_alloc/free).
 *  - 가상주소→IOVA 변환 핵심 함수(spdk_vtophys, spdk_mem_map_translate).
 *  - DPDK mem_event 콜백(메모리 영역 추가/삭제 알림) 처리.
 *  - VFIO IOMMU 기반 DMA map/unmap(/dev/vfio/<group>의 container_dma_map ioctl).
 *  - hugepage 미사용(--no-huge) 모드 지원.
 * 를 모두 담는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/env_dpdk의 가장 깊은 부분으로, NVMe 드라이버(lib/nvme)가 PRP/SGL 디스크립터를 만들 때
 * 매번 spdk_vtophys()로 가상주소→IOVA 변환을 수행한다. SPDK의 hot path 함수 중 하나이며
 * 룩업 성능을 위해 2단계 직접 인덱싱(O(1))을 사용한다.
 * 호출 체인:
 *   spdk_dma_malloc → DPDK rte_malloc_socket → hugepage 매핑 → DPDK mem_event 콜백 →
 *   memory_hotplug_cb(본 파일) → spdk_mem_register → g_mem_reg_map 트리 갱신 →
 *   이후 spdk_vtophys 룩업이 O(1)에 성공.
 * 실행 컨텍스트: 메인 스레드(메모리 등록) + 모든 reactor(vtophys 룩업). 룩업 자체는 lockless
 * (트리 노드 등록 후 변경 없음 가정).
 *
 * === 타 모듈과의 연결 ===
 * - 의존: env_internal.h(SHIFT_256TB/1GB), pci_dpdk.h(BAR vtophys 지원), DPDK rte_memory/
 *         rte_eal_memconfig/rte_dev/rte_pci, Linux vfio.h(IOMMU map), spdk/memory.h(SHIFT_2MB/4KB).
 * - 본 파일에 의존: lib/nvme(PRP 생성), lib/bdev(DMA 버퍼), lib/sock(가속용 zerocopy 등).
 * - 데이터 흐름: DPDK가 hugepage를 alloc/free할 때마다 RTE_MEM_EVENT_ALLOC/_FREE를 본 파일이 잡고
 *   SPDK 측 트리에 반영. NVMe 드라이버는 이후 spdk_vtophys()로 IOVA를 얻어 NVMe PRP entry에 채움.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct map_256tb / map_1gb2mb / map_1gb4kb / map_2mb4kb: 다단계 룩업 트리.
 * - struct spdk_mem_map: 일반화된 가상주소→임의 값 매핑 객체(vtophys, dma_set 등에 활용).
 * - struct spdk_vfio_dma_map / vfio_cfg: VFIO IOMMU container_dma_map 매핑을 트래킹.
 * - mem_map_translate: 트리 룩업의 핵심 fast path.
 * - mem_map_init/_fini: 트리 초기화/해제.
 * - vtophys_init/_fini: 부팅 시 hugepage 매핑 등록 + IOMMU 초기 매핑.
 * - memory_hotplug_cb: DPDK 메모리 이벤트 콜백(hugepage 동적 추가/삭제).
 * - vtophys_iommu_map_dma / _unmap_dma: VFIO_IOMMU_MAP_DMA ioctl 래퍼.
 * - spdk_vtophys: 외부 노출 핫패스 변환 함수.
 * - mem_disable_huge_pages / mem_disable_vtophys: 런타임 정책 토글.
 */

/* [한국어] SPDK 표준 인클루드. errno, calloc, pthread 등에 필요. */
#include "spdk/stdinc.h"

/* [한국어] env_dpdk 내부 선언. SHIFT_256TB/1GB와 mem_disable_* 토글 선언. */
#include "env_internal.h"
/* [한국어] BAR 영역도 vtophys에 등록하기 위한 DPDK 어댑터. */
#include "pci_dpdk.h"

/* [한국어] DPDK 컴파일 매크로. */
#include <rte_config.h>
/* [한국어] rte_mem_event_callback_register, rte_memseg_walk, rte_mem_virt2iova 등 메모리 API. */
#include <rte_memory.h>
/* [한국어] DPDK 내부 메모리 구성 락 RTE_LCORE_LOCK 매크로 등(인접 룩업 동기화 필요 시 사용). */
#include <rte_eal_memconfig.h>
/* [한국어] rte_dev_*. 일부 DPDK 버전에서 필요. */
#include <rte_dev.h>
/* [한국어] rte_pci_device — BAR vtophys 등록 함수에서 사용. */
#include <rte_pci.h>

/* [한국어] SPDK 내부 assert 매크로(SPDK_UNREACHABLE 등). */
#include "spdk_internal/assert.h"

/* [한국어] SPDK_STATIC_ASSERT. */
#include "spdk/assert.h"
/* [한국어] spdk_likely/spdk_unlikely. hot path 룩업에서 분기 예측 힌트 부여. */
#include "spdk/likely.h"
/* [한국어] TAILQ_* 매크로. */
#include "spdk/queue.h"
/* [한국어] spdk_min, container_of. */
#include "spdk/util.h"
/* [한국어] SHIFT_2MB/4KB, VALUE_2MB/4KB, MASK_2MB/4KB. SPDK 페이지 크기 표준 매크로. */
#include "spdk/memory.h"
/* [한국어] 외부 EAL 협업 API 선언. */
#include "spdk/env_dpdk.h"
/* [한국어] SPDK_*LOG. */
#include "spdk/log.h"

#ifdef __linux__
/* [한국어] Linux 커널 버전 매크로(LINUX_VERSION_CODE) — VFIO는 3.6+ 필요. */
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
/* [한국어] VFIO 인터페이스 정의: VFIO_IOMMU_MAP_DMA, VFIO_IOMMU_TYPE1 등. */
#include <linux/vfio.h>
/* [한국어] DPDK VFIO 헬퍼(rte_vfio_get_container_fd, rte_vfio_is_enabled). */
#include <rte_vfio.h>

/*
 * [한국어]
 * struct spdk_vfio_dma_map - VFIO IOMMU container에 등록된 단일 DMA 매핑
 *
 * SPDK가 VFIO_IOMMU_MAP_DMA ioctl을 호출할 때 사용한 인자(map)를 그대로 보관하여 추후 unmap
 * 시 동일 파라미터로 VFIO_IOMMU_UNMAP_DMA를 실행할 수 있게 한다.
 */
struct spdk_vfio_dma_map {
	struct vfio_iommu_type1_dma_map map;
	/* [한국어] VFIO ioctl 인자 구조체(iova, vaddr, size, flags 등).
	 * 설정자: vtophys_iommu_map_dma. 읽는 자: vtophys_iommu_unmap_dma.
	 * 값 범위: VFIO 커널 ABI 정의 그대로. 동기화: g_vfio.mutex로 큐와 함께 보호. */
	TAILQ_ENTRY(spdk_vfio_dma_map) tailq;
	/* [한국어] g_vfio.maps 큐 링크. */
};

/*
 * [한국어]
 * struct vfio_cfg - VFIO IOMMU container 전역 상태
 *
 * SPDK는 모든 DMA 가능 PCI 디바이스를 단일 VFIO container에 묶어 IOMMU 매핑을 공유한다.
 * 본 구조체는 그 container의 fd와 현재 등록된 매핑 리스트를 들고 있다.
 */
struct vfio_cfg {
	int fd;
	/* [한국어] /dev/vfio/vfio container 파일 디스크립터. -1이면 비활성.
	 * 설정자: vfio_enable(rte_vfio_container_create 등에서 받음).
	 * 읽는 자: vtophys_iommu_map/unmap_dma의 ioctl 호출. 동기화: g_vfio.mutex. */
	bool enabled;
	/* [한국어] VFIO 모드 활성 여부. enabled=false면 IOVA를 물리주소로 직접 사용. */
	bool noiommu_enabled;
	/* [한국어] VFIO no-IOMMU 모드(IOMMU 비활성 환경에서 root 권한으로 사용)인지 여부. */
	unsigned device_ref;
	/* [한국어] container에 attach된 PCI 디바이스 수. 0→비-0 또는 비-0→0 전이 시 매핑 일괄 등록/해제. */
	TAILQ_HEAD(, spdk_vfio_dma_map) maps;
	/* [한국어] 현재 등록된 모든 DMA 매핑 큐. 디바이스 추가 시 일괄 반영하기 위해 보관. */
	pthread_mutex_t mutex;
	/* [한국어] 본 구조체와 maps 큐를 보호. */
};

/* [한국어] 전역 VFIO 상태. 정적 초기화로 fd=-1(미사용) 상태로 시작. */
static struct vfio_cfg g_vfio = {
	.fd = -1,
	.enabled = false,
	.noiommu_enabled = false,
	.device_ref = 0,
	.maps = TAILQ_HEAD_INITIALIZER(g_vfio.maps),
	.mutex = PTHREAD_MUTEX_INITIALIZER
};
#endif
#endif

#if DEBUG
/* [한국어] 디버그 빌드에서만 SPDK_ERRLOG로 동작. 릴리즈에서는 no-op. */
#define DEBUG_PRINT(...) SPDK_ERRLOG(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#endif

/* [한국어] 변환 실패 sentinel — 어떤 페이지에도 매핑되지 않음. */
#define ADDR_INVALID		((uint64_t)-1)

/* [한국어] 가상주소를 2MB 페이지 번호(VFN)로 변환. SHIFT_2MB=21. */
#define VFN_2MB(vaddr)		((vaddr) >> SHIFT_2MB)
/* [한국어] 가상주소를 4KB 페이지 번호로 변환. SHIFT_4KB=12. */
#define VFN_4KB(vaddr)		((vaddr) >> SHIFT_4KB)

/* [한국어] 2MB VFN을 4KB VFN으로 확장(좌시프트). 2MB→4KB는 9비트 확장. */
#define FN_2MB_TO_4KB(fn)	((fn) << (SHIFT_2MB - SHIFT_4KB))
/* [한국어] 4KB VFN을 2MB VFN으로 축소(우시프트). */
#define FN_4KB_TO_2MB(fn)	((fn) >> (SHIFT_2MB - SHIFT_4KB))

/* [한국어] 2MB VFN의 상위 비트로 256TB 트리 인덱스 계산. 1GB 단위로 청크. */
#define MAP_256TB_IDX(vfn_2mb)	((vfn_2mb) >> (SHIFT_1GB - SHIFT_2MB))
/* [한국어] 2MB VFN의 하위 비트로 1GB 청크 내 인덱스(0~511). */
#define MAP_1GB_IDX(vfn_2mb)	((vfn_2mb) & ((1ULL << (SHIFT_1GB - SHIFT_2MB)) - 1))
/* [한국어] 4KB VFN의 하위 비트로 2MB 청크 내 인덱스(0~511). */
#define MAP_2MB_IDX(vfn_4kb)	((vfn_4kb) & ((1ULL << (SHIFT_2MB - SHIFT_4KB)) - 1))

/* [한국어] 256TB 루트 트리 엔트리 수(=2^18=262144). 1GB 청크 단위. */
#define MAP_256TB_SIZE		(1ULL << (SHIFT_256TB - SHIFT_1GB))
/* [한국어] 1GB 청크 내 2MB 엔트리 수(=512). */
#define MAP_1GB_SIZE		(1ULL << (SHIFT_1GB - SHIFT_2MB))
/* [한국어] 2MB 청크 내 4KB 엔트리 수(=512). */
#define MAP_2MB_SIZE		(1ULL << (SHIFT_2MB - SHIFT_4KB))

/* [한국어] 인덱스 3개로부터 가상주소를 역산. 페이지 정렬 결과만 의미 있음. */
#define ADDR_FROM_IDX(idx_256tb, idx_1gb, idx_2mb) \
	(((idx_256tb) << SHIFT_1GB) | ((idx_1gb) << SHIFT_2MB) | ((idx_2mb) << SHIFT_4KB))

/* Page is registered */
/* [한국어] 등록 맵의 translation 값에서 "이 2MB 페이지가 SPDK에 등록되어 있음"을 표시하는 비트(62). */
#define REG_MAP_REGISTERED	(1ULL << 62)

/* A notification region barrier. The 2MB translation entry that's marked
 * with this flag must be unregistered separately. This allows contiguous
 * regions to be unregistered in the same chunks they were registered.
 */
/* [한국어] "여기서 새 등록 영역이 시작됨" 표시(비트 63). 두 영역이 인접해도 unregister는
 * 등록 시 단위 그대로 분리해야 하므로 시작 경계를 트리에 새겨둔다. */
#define REG_MAP_NOTIFY_START	(1ULL << 63)

/* 4KB vtophys mapping */
/* [한국어] vtophys translation 값에서 "이 항목은 4KB 페이지 매핑이다"를 표시(비트 63).
 * 일반적으로는 2MB 페이지지만 일부(--no-huge 또는 4KB hugepage 미사용 등)는 4KB로만 매핑. */
#define VTOPHYS_4KB		(1ULL << 63)
/* [한국어] vtophys 값에서 플래그 비트를 제거하고 IOVA만 추출. */
#define VTOPHYS_ADDR(paddr)	((paddr) & ~VTOPHYS_4KB)

/* Third-level map for 4KB translations */
/*
 * [한국어]
 * struct map_2mb4kb - 4KB 단위 변환을 위한 3단계 맵
 *
 * 2MB 청크 하나에 대해 4KB 단위 IOVA(또는 사용자 정의 값) 512개를 평면 배열로 보관.
 */
struct map_2mb4kb {
	uint64_t translation_4kb[MAP_2MB_SIZE];
	/* [한국어] 4KB 페이지별 변환 값 배열.
	 * 설정자: spdk_mem_map_set_translation. 읽는 자: mem_map_translate fast path.
	 * 값 범위: IOVA 또는 default_translation(미등록). 동기화: spdk_mem_map.mutex. */
};

/* Second-level map table indexed by bits [21..29] of the virtual address.
 * Each entry contains the address translation for a 2MB page or an error
 * for entries that haven't been retrieved yet.
 */
/*
 * [한국어]
 * struct map_1gb2mb - 2MB 페이지 단위 변환을 위한 중간 단계 맵
 *
 * 1GB 청크 하나에 대해 2MB 페이지 512개의 변환 값을 보관.
 */
struct map_1gb2mb {
	uint64_t translation_2mb[MAP_1GB_SIZE];
	/* [한국어] 2MB 페이지별 변환 값 배열.
	 * 설정자: spdk_mem_map_set_translation. 읽는 자: mem_map_translate. */
};

/* Second-level map containing 4KB translations. */
/*
 * [한국어]
 * struct map_1gb4kb - 4KB 폴백을 위한 2MB 청크 포인터 배열
 *
 * 1GB 안에 4KB 매핑이 필요한 2MB 청크가 있을 때, 해당 슬롯에 map_2mb4kb 노드를 할당.
 */
struct map_1gb4kb {
	struct map_2mb4kb *map[MAP_1GB_SIZE];
	/* [한국어] 2MB 청크별 4KB 룩업 노드 포인터.
	 * 설정자: 4KB 변환이 처음 필요할 때 calloc으로 노드 할당. NULL이면 4KB 매핑 없음.
	 * 읽는 자: mem_map_translate. 동기화: spdk_mem_map.mutex로 노드 할당 시 보호. */
};

/* Top-level map table indexed by bits [30..47] of the virtual address.
 * Each entry points to a second-level map table or NULL.
 */
/*
 * [한국어]
 * struct map_256tb - 최상위 루트 맵
 *
 * 가상주소 비트 [30..47]을 인덱스로 사용하여 1GB 청크당 두 종류의 2단계 노드(2MB/4KB)를 가짐.
 */
struct map_256tb {
	struct {
		struct map_1gb2mb	*map_1gb2mb;
		/* [한국어] 2MB 매핑 노드 포인터(없으면 NULL). */
		struct map_1gb4kb	*map_1gb4kb;
		/* [한국어] 4KB 매핑 노드 포인터(필요 시에만 할당). */
	} map[MAP_256TB_SIZE];
	/* [한국어] 1GB 청크별 룩업 슬롯. 총 262144개(소형 포인터 배열, 약 4MB). */
};

/* Page-granularity memory address translation */
/*
 * [한국어]
 * struct spdk_mem_map - 일반화된 페이지 단위 가상주소→값 매핑 객체
 *
 * SPDK는 vtophys 외에도 다양한 attribute(예: NUMA, dma_unsafe 영역 표시 등)를 페이지 단위로
 * 트래킹할 때 이 구조체를 사용한다. 본 파일의 함수들은 모두 본 구조체에 대해 일반화되어 있다.
 */
struct spdk_mem_map {
	struct map_256tb map_256tb;
	/* [한국어] 핵심 룩업 트리. 인라인 임베드되어 추가 캐시 미스 없이 접근. */
	pthread_mutex_t mutex;
	/* [한국어] 매핑 set/clear 시 트리 노드 동적 할당을 보호. 룩업은 lock-free. */
	uint64_t default_translation;
	/* [한국어] 등록되지 않은 페이지에 대한 기본 반환값(예: ADDR_INVALID). */
	struct spdk_mem_map_ops ops;
	/* [한국어] 매핑 변경 시 호출될 사용자 콜백(notify_cb 등). */
	void *cb_ctx;
	/* [한국어] ops 콜백에 전달될 사용자 컨텍스트. */
	TAILQ_ENTRY(spdk_mem_map) tailq;
	/* [한국어] g_spdk_mem_maps 전역 큐 링크 — 새 등록 발생 시 모든 맵에 통지. */
};

/* Registrations map. The 64 bit translations are bit fields with the
 * following layout (starting with the low bits):
 *    0 - 61 : reserved
 *   62 - 63 : flags
 */
/* [한국어] SPDK 영역 등록 상태를 추적하는 마스터 맵. 다른 모든 spdk_mem_map에 대해
 * "어느 가상 영역이 SPDK 등록 영역인가"를 판단하는 단일 진실의 출처. 비트 62=REGISTERED,
 * 비트 63=NOTIFY_START. */
static struct spdk_mem_map *g_mem_reg_map;
/* [한국어] 모든 spdk_mem_map 인스턴스의 전역 큐. mem_event 발생 시 모두에게 통지. */
static TAILQ_HEAD(spdk_mem_map_head, spdk_mem_map) g_spdk_mem_maps =
	TAILQ_HEAD_INITIALIZER(g_spdk_mem_maps);
/* [한국어] g_spdk_mem_maps 큐 보호 뮤텍스. 매핑 등록/해제 시 사용. */
static pthread_mutex_t g_spdk_mem_map_mutex = PTHREAD_MUTEX_INITIALIZER;

/* [한국어] 레거시 메모리 모드. DPDK가 동적 hugepage 추가/제거를 하지 않음. */
static bool g_legacy_mem;
/* [한국어] hugepage 사용 여부(--no-huge로 false). */
static bool g_huge_pages = true;
/* [한국어] vtophys 활성 여부(--no-pci 등으로 false). false면 PCI/DMA 없음을 가정. */
static bool g_vtophys = true;

/*
 * [한국어]
 * mem_map_translate - 다단계 트리에서 가상주소→매핑 값 변환 (fast path)
 *
 * @map:       spdk_mem_map 인스턴스(vtophys 맵일 수도, 사용자 맵일 수도).
 * @vaddr:     변환할 호스트 가상주소.
 * @page_size: 출력. 변환된 페이지 크기(VALUE_2MB 또는 VALUE_4KB).
 * @return:    매핑 값. 등록되지 않은 페이지면 map->default_translation.
 *
 * 룩업 단계:
 *   1) 가상주소를 SHIFT_2MB로 시프트해 2MB VFN 산출 → 256TB 인덱스/1GB 인덱스 분리.
 *   2) map_256tb[idx].map_1gb2mb가 non-NULL이고 translation_2mb[idx_1gb]가 기본값이 아니면 hit.
 *   3) 그렇지 않으면 4KB 폴백 트리(map_1gb4kb → map_2mb4kb) 탐색.
 *   4) 둘 다 실패면 default_translation 반환.
 *
 * 본 함수는 NVMe PRP 생성 등 hot path에서 매 I/O마다 호출되므로 spdk_likely로 분기 예측 유도.
 * 트리 노드는 등록 후 변경 없이 유지되므로 lock-free 가능(쓰기 측은 mutex 보호).
 *
 * 호출 체인: spdk_vtophys / spdk_mem_map_translate → mem_map_translate
 */
static inline uint64_t
mem_map_translate(const struct spdk_mem_map *map, uint64_t vaddr, int *page_size)
{
	const struct map_1gb2mb *map_1gb2mb;
	/* [한국어] 2MB 룩업 노드 포인터 임시. */
	const struct map_1gb4kb *map_1gb4kb;
	/* [한국어] 4KB 룩업 1단계 노드 포인터. */
	const struct map_2mb4kb *map_2mb4kb;
	/* [한국어] 4KB 룩업 2단계 노드 포인터. */
	uint64_t translation, vfn_4kb, vfn_2mb, idx_2mb, idx_1gb, idx_256tb;
	/* [한국어] 변환 결과 + 다단계 인덱스 임시 변수들. */

	/* [한국어] vaddr → 2MB VFN(가상 페이지 번호). 256TB 트리 인덱싱의 기본 단위. */
	vfn_2mb = VFN_2MB(vaddr);
	/* [한국어] 2MB VFN의 상위 비트 → 256TB 루트 트리 인덱스(1GB 청크 선택). */
	idx_256tb = MAP_256TB_IDX(vfn_2mb);
	/* [한국어] 2MB VFN의 하위 9비트 → 1GB 청크 내 2MB 슬롯 인덱스(0~511). */
	idx_1gb = MAP_1GB_IDX(vfn_2mb);

	/* Check the 2MB map first */
	/* [한국어] 1단계: 해당 1GB 청크의 2MB 룩업 노드 포인터를 꺼낸다. */
	map_1gb2mb = map->map_256tb.map[idx_256tb].map_1gb2mb;
	/* [한국어] 노드가 존재하는 경우가 대다수(hugepage 매핑) → likely 힌트. */
	if (spdk_likely(map_1gb2mb != NULL)) {
		/* [한국어] 2MB 슬롯에서 변환 값을 읽는다. */
		translation = map_1gb2mb->translation_2mb[idx_1gb];
		/* [한국어] default가 아니면 실제 등록된 2MB 매핑 → hit. */
		if (spdk_likely(translation != map->default_translation)) {
			/* [한국어] 호출자에게 페이지 크기 2MB를 알린다. */
			*page_size = VALUE_2MB;
			/* [한국어] 2MB 변환 값 반환(IOVA 등). */
			return translation;
		}
	}

	/* There's no 2MB translation for this address, check the 4KB map */
	/* [한국어] 2MB miss → 4KB 폴백 트리 1단계 노드를 본다(--no-huge 등). */
	map_1gb4kb = map->map_256tb.map[idx_256tb].map_1gb4kb;
	if (spdk_likely(map_1gb4kb != NULL)) {
		/* [한국어] 1GB 청크 내 해당 2MB 청크의 4KB 룩업 노드를 꺼낸다. */
		map_2mb4kb = map_1gb4kb->map[idx_1gb];
		if (spdk_likely(map_2mb4kb != NULL)) {
			/* [한국어] 4KB VFN과 2MB 청크 내 인덱스(0~511)를 계산. */
			vfn_4kb = VFN_4KB(vaddr);
			idx_2mb = MAP_2MB_IDX(vfn_4kb);
			/* [한국어] 호출자에게 페이지 크기 4KB를 알린다. */
			*page_size = VALUE_4KB;

			/* [한국어] 4KB 슬롯의 변환 값 반환. */
			return map_2mb4kb->translation_4kb[idx_2mb];
		}
	}

	/* [한국어] 2MB/4KB 모두 miss → 기본값(미등록 sentinel) 반환. */
	*page_size = VALUE_2MB;
	return map->default_translation;
}

/*
 * [한국어]
 * mem_map_is_4kb_mapping - 주어진 가상주소가 4KB 단위로 매핑되었는지 판정
 *
 * @map:   조회 대상 spdk_mem_map.
 * @vaddr: 검사할 가상주소.
 * @return: 4KB 매핑이면 true, 2MB(또는 미등록)면 false.
 *
 * mem_map_translate를 호출하되 변환 값은 버리고 page_size 출력만 본다.
 * spdk_mem_unregister 경로에서 2MB 영역이 사실 여러 4KB 등록 조각으로 이루어졌는지
 * 판단하여 unregister 알림을 페이지별로 쪼갤지 결정하는 데 쓰인다.
 *
 * 실행 컨텍스트: 메인 스레드(등록/해제). 호출 체인: mem_unregister_page → mem_map_is_4kb_mapping → mem_map_translate
 */
static bool
mem_map_is_4kb_mapping(struct spdk_mem_map *map, uint64_t vaddr)
{
	/* [한국어] mem_map_translate가 채워줄 페이지 크기 출력 변수. */
	int page_size;

	/* [한국어] 변환 값 자체는 필요 없고 page_size만 얻기 위해 호출. */
	mem_map_translate(map, vaddr, &page_size);
	/* [한국어] page_size==VALUE_4KB이면 이 주소는 4KB 매핑. */
	return page_size == VALUE_4KB;
}

/*
 * [한국어]
 * mem_map_walk_region - [vaddr, vaddr+size) 구간을 페이지 단위로 분해해 콜백 호출
 *
 * @map:      대상 매핑.
 * @vaddr:    구간 시작 가상주소(4KB 정렬).
 * @size:     구간 길이.
 * @callback: 각 페이지 청크마다 호출될 함수(set/unregister/check 등).
 * @ctx:      콜백에 전달할 사용자 컨텍스트.
 * @return:   0 성공. 콜백이 0이 아닌 값을 반환하면 즉시 그 값으로 중단.
 *
 * 구간을 세 부분으로 나눠 순회한다: (1) 앞쪽 2MB 정렬 전까지의 4KB 꼬리,
 * (2) 가운데 2MB 정렬 본문, (3) 뒤쪽 남은 4KB 꼬리. 이렇게 하면 hugepage 정렬
 * 영역은 2MB 단위로, 비정렬 가장자리는 4KB 단위로 처리해 트리 노드 낭비를 줄인다.
 * 매핑 set/clear/register/unregister의 공통 순회 엔진이다.
 *
 * 실행 컨텍스트: 메인 스레드(매핑 변경). 호출 체인: spdk_mem_(un)register/set_translation → mem_map_walk_region → callback
 */
static int
mem_map_walk_region(struct spdk_mem_map *map, uint64_t vaddr, size_t size,
		    int (*callback)(struct spdk_mem_map *map, uint64_t addr, size_t sz, void *ctx),
		    void *ctx)
{
	/* [한국어] 현재 위치의 4KB/2MB VFN 임시. */
	uint64_t vfn_4kb, vfn_2mb;
	/* [한국어] 각 단계 순회 종료 VFN. */
	uint64_t vfn_4kb_end, vfn_2mb_end;
	/* [한국어] 콜백 반환값(0이 아니면 중단). */
	int rc;

	/* === 1단계: 앞쪽 2MB 정렬 경계 전까지 4KB 단위로 처리 === */
	/* [한국어] 시작점의 4KB VFN. */
	vfn_4kb = VFN_4KB(vaddr);
	/* [한국어] 종료점 = 다음 2MB 경계의 4KB VFN과 구간 끝 중 더 작은 쪽.
	 * vaddr+MASK_2MB로 올림 후 2MB VFN→4KB VFN 변환하여 2MB 정렬 지점을 구함. */
	vfn_4kb_end = spdk_min(FN_2MB_TO_4KB(VFN_2MB(vaddr + MASK_2MB)), VFN_4KB(vaddr + size));
	while (vfn_4kb < vfn_4kb_end) {
		/* [한국어] 이 4KB 페이지에 대해 콜백 실행. */
		rc = callback(map, vaddr, VALUE_4KB, ctx);
		if (rc != 0) {
			/* [한국어] 콜백 실패 시 즉시 전파(부분 처리 상태로 반환). */
			return rc;
		}
		/* [한국어] 다음 4KB 페이지로 전진. */
		vaddr += VALUE_4KB;
		size -= VALUE_4KB;
		vfn_4kb++;
	}

	/* === 2단계: 2MB 정렬 본문을 2MB 단위로 처리 === */
	/* [한국어] 현재(2MB 정렬된) 위치의 2MB VFN과 종료 VFN. */
	vfn_2mb = VFN_2MB(vaddr);
	vfn_2mb_end = VFN_2MB(vaddr + size);
	while (vfn_2mb < vfn_2mb_end) {
		/* [한국어] 2MB 페이지(=hugepage) 단위 콜백. */
		rc = callback(map, vaddr, VALUE_2MB, ctx);
		if (rc != 0) {
			return rc;
		}
		/* [한국어] 다음 2MB 페이지로 전진. */
		vaddr += VALUE_2MB;
		size -= VALUE_2MB;
		vfn_2mb++;
	}

	/* === 3단계: 끝에 남은 2MB 미만 꼬리를 다시 4KB 단위로 처리 === */
	vfn_4kb = VFN_4KB(vaddr);
	vfn_4kb_end = VFN_4KB(vaddr + size);
	while (vfn_4kb < vfn_4kb_end) {
		/* [한국어] 잔여 4KB 페이지 콜백. */
		rc = callback(map, vaddr, VALUE_4KB, ctx);
		if (rc != 0) {
			return rc;
		}
		/* [한국어] 다음 4KB 페이지로 전진. */
		vaddr += VALUE_4KB;
		size -= VALUE_4KB;
		vfn_4kb++;
	}

	/* [한국어] 전 구간 콜백 성공. */
	return 0;
}

/*
 * [한국어]
 * mem_reg_map_next_region - 등록 맵에서 addr 이후의 다음 "등록 시작 영역"을 찾는다
 *
 * @addr:   탐색 시작 가상주소(0이면 처음부터).
 * @return: REG_MAP_NOTIFY_START 비트가 켜진 다음 영역의 시작 가상주소, 없으면 ADDR_INVALID.
 *
 * g_mem_reg_map 트리를 256TB→1GB(→필요 시 2MB) 인덱스 순으로 선형 스캔하며 등록
 * 영역의 시작 경계를 찾는다. 비어 있는 1GB 청크는 통째로 건너뛰어(goto next_256tb)
 * 262144개 슬롯 스캔을 효율화한다. 새 spdk_mem_map이 추가될 때 기존 등록 영역을
 * 모두 다시 통지하기 위한 이터레이터로 mem_map_notify_walk가 반복 호출한다.
 *
 * 실행 컨텍스트: 메인 스레드(g_mem_reg_map->mutex 보유 하에). 호출 체인: mem_map_notify_walk → mem_reg_map_next_region → mem_map_translate
 */
static uint64_t
mem_reg_map_next_region(uint64_t addr)
{
	/* [한국어] 3단계 트리 인덱스 임시. */
	uint64_t idx_256tb, idx_1gb, idx_2mb;
	/* [한국어] reg=등록 플래그 값, vfn=시작 주소의 페이지 번호. */
	uint64_t reg, vfn_2mb, vfn_4kb;
	/* [한국어] mem_map_translate가 채우는 페이지 크기. */
	int page_size;

	/* [한국어] 시작 addr를 트리 인덱스 3종으로 분해. */
	vfn_2mb = VFN_2MB(addr);
	vfn_4kb = VFN_4KB(addr);
	idx_256tb = MAP_256TB_IDX(vfn_2mb);
	idx_1gb = MAP_1GB_IDX(vfn_2mb);
	idx_2mb = MAP_2MB_IDX(vfn_4kb);
	/* [한국어] 루트 인덱스부터 끝까지 1GB 청크 순회. */
	for (; idx_256tb < MAP_256TB_SIZE; idx_256tb++) {
		/* [한국어] 이 1GB 청크에 2MB/4KB 노드가 둘 다 없으면 등록이 전혀 없음 → 통째로 스킵. */
		if (!g_mem_reg_map->map_256tb.map[idx_256tb].map_1gb2mb &&
		    !g_mem_reg_map->map_256tb.map[idx_256tb].map_1gb4kb) {
			goto next_256tb;
		}

		/* [한국어] 1GB 청크 내 2MB 슬롯을 순회. */
		for (; idx_1gb < MAP_1GB_SIZE; idx_1gb++) {
			/* [한국어] 인덱스로부터 가상주소 역산. */
			addr = ADDR_FROM_IDX(idx_256tb, idx_1gb, idx_2mb);
			/* [한국어] 이 주소의 등록 플래그/페이지 크기 조회. */
			reg = mem_map_translate(g_mem_reg_map, addr, &page_size);

			/* [한국어] NOTIFY_START 비트 = 새 등록 영역의 시작 → 이 주소 반환. */
			if (reg & REG_MAP_NOTIFY_START) {
				/* [한국어] START면 반드시 REGISTERED여야 함(불변식). */
				assert(reg & REG_MAP_REGISTERED);
				return addr;
			}

			/* [한국어] 이 2MB 청크가 4KB로 매핑되어 있으면 4KB 단위로 더 자세히 스캔. */
			if (page_size == VALUE_4KB) {
				for (; idx_2mb < MAP_2MB_SIZE; idx_2mb++) {
					/* [한국어] 4KB 인덱스까지 포함해 주소 역산. */
					addr = ADDR_FROM_IDX(idx_256tb, idx_1gb, idx_2mb);
					reg = mem_map_translate(g_mem_reg_map, addr, &page_size);

					/* [한국어] 4KB 단위에서도 START 경계 발견 시 반환. */
					if (reg & REG_MAP_NOTIFY_START) {
						assert(reg & REG_MAP_REGISTERED);
						return addr;
					}
				}
			}

			/* [한국어] 다음 2MB 슬롯부터는 4KB 인덱스를 0으로 리셋. */
			idx_2mb = 0;
		}
next_256tb:
		/* [한국어] 다음 1GB 청크로 넘어가면 1GB 인덱스를 0부터 다시. */
		idx_1gb = 0;
	}

	/* [한국어] 더 이상 등록 시작 영역이 없음. */
	return ADDR_INVALID;
}

/*
 * Walk the currently registered memory via the main memory registration map
 * and call the new map's notify callback for each virtually contiguous region.
 */
/*
 * [한국어]
 * mem_map_notify_walk - 현재 등록된 모든 메모리 영역에 대해 새 맵의 콜백을 일괄 통지
 *
 * @map:    통지를 받을 spdk_mem_map(방금 alloc되었거나 free되는 중).
 * @action: SPDK_MEM_MAP_NOTIFY_REGISTER 또는 _UNREGISTER.
 * @return: 0 성공. REGISTER 중 콜백이 실패하면 그 rc(이미 통지한 영역은 롤백).
 *
 * 새 맵이 생기면 그 시점에 이미 등록된 hugepage 영역들을 모르므로, g_mem_reg_map을
 * 기준으로 가상연속 영역마다 notify_cb를 호출해 "따라잡게" 한다(vtophys_notify가 IOVA를
 * 채우는 식). REGISTER 도중 실패하면 이미 통지한 부분에 UNREGISTER를 역으로 보내 원자성을
 * 흉내낸다(err_unregister). 순회 동안 g_mem_reg_map->mutex를 잡아 새 등록이 끼어들지 못하게 한다.
 *
 * 실행 컨텍스트: 메인 스레드. 호출 체인: spdk_mem_map_alloc/free → mem_map_notify_walk → mem_reg_map_next_region / notify_cb
 */
static int
mem_map_notify_walk(struct spdk_mem_map *map, enum spdk_mem_map_notify_action action)
{
	/* [한국어] 현재 영역 주소, 실패 지점 주소, 영역 길이. */
	uint64_t addr, fail_addr, size;
	/* [한국어] notify_cb 반환값. */
	int rc;

	/* [한국어] 등록 맵이 아직 없으면(초기화 전) 통지 불가. */
	if (!g_mem_reg_map) {
		return -EINVAL;
	}

	/* Hold the memory registration map mutex so no new registrations can be added while we are looping. */
	/* [한국어] 순회 중 새 등록이 트리를 바꾸지 못하도록 등록 맵 뮤텍스 보유. */
	pthread_mutex_lock(&g_mem_reg_map->mutex);
	/* [한국어] 0부터 시작해 등록 영역을 차례로 순회. */
	for (addr = mem_reg_map_next_region(0);
	     addr != ADDR_INVALID;
	     addr = mem_reg_map_next_region(addr)) {
		/* [한국어] are_contiguous 기반으로 가상연속 길이를 최대치로 측정. */
		size = UINT64_MAX;
		spdk_mem_map_translate(g_mem_reg_map, addr, &size);
		/* [한국어] 새 맵의 콜백에 이 영역의 (가상)주소/길이를 통지. */
		rc = map->ops.notify_cb(map->cb_ctx, map, action,
					(void *)addr, size);
		/* Don't bother handling unregister failures. It can't be any worse */
		/* [한국어] REGISTER 실패만 롤백 대상(UNREGISTER 실패는 무시). */
		if (rc != 0 && action == SPDK_MEM_MAP_NOTIFY_REGISTER) {
			goto err_unregister;
		}
		/* [한국어] 측정된 영역 길이만큼 전진해 다음 영역 탐색 기준점 설정. */
		addr += size;
	}

	/* [한국어] 정상 종료 → 락 해제 후 성공. */
	pthread_mutex_unlock(&g_mem_reg_map->mutex);
	return 0;

err_unregister:
	/* [한국어] 실패가 발생한 주소를 경계로 기록. */
	fail_addr = addr;
	/* [한국어] 처음부터 fail_addr 직전까지 이미 통지한 영역에 UNREGISTER를 역통지. */
	for (addr = mem_reg_map_next_region(0);
	     addr != ADDR_INVALID && addr != fail_addr;
	     addr = mem_reg_map_next_region(addr)) {
		/* [한국어] 다시 영역 길이를 측정. */
		size = UINT64_MAX;
		spdk_mem_map_translate(g_mem_reg_map, addr, &size);
		/* [한국어] 앞서 REGISTER한 것을 취소(반환값 무시 — 더 나빠질 게 없음). */
		map->ops.notify_cb(map->cb_ctx, map,
				   SPDK_MEM_MAP_NOTIFY_UNREGISTER,
				   (void *)addr, size);
		addr += size;
	}

	/* [한국어] 락 해제 후 원래 실패 코드 반환. */
	pthread_mutex_unlock(&g_mem_reg_map->mutex);
	return rc;
}

/*
 * [한국어]
 * mem_map_free - 룩업 트리의 모든 동적 노드와 맵 자체를 해제
 *
 * @map: 해제할 spdk_mem_map.
 *
 * 256TB 루트 배열의 각 슬롯에서 2MB 노드(map_1gb2mb)를 free하고, 4KB 폴백 노드
 * (map_1gb4kb)가 있으면 그 안의 512개 map_2mb4kb까지 모두 free한 뒤 맵 구조체를 해제한다.
 * spdk_mem_map_free의 후반부(통지/큐 제거 이후) 실제 메모리 반환 단계.
 *
 * 실행 컨텍스트: 메인 스레드. 호출 체인: spdk_mem_map_alloc(실패 경로)/spdk_mem_map_free → mem_map_free → free
 */
static void
mem_map_free(struct spdk_mem_map *map)
{
	/* [한국어] 4KB 폴백 1단계 노드 임시. */
	struct map_1gb4kb *map_1gb4kb;
	/* [한국어] 루트/4KB 슬롯 순회 인덱스. */
	size_t i, j;

	/* [한국어] 262144개 루트 슬롯 전부 순회. */
	for (i = 0; i < SPDK_COUNTOF(map->map_256tb.map); i++) {
		/* [한국어] 2MB 노드 해제(NULL이면 free는 no-op). */
		free(map->map_256tb.map[i].map_1gb2mb);
		/* [한국어] 4KB 폴백 노드 포인터 꺼내기. */
		map_1gb4kb = map->map_256tb.map[i].map_1gb4kb;
		if (map_1gb4kb == NULL) {
			/* [한국어] 4KB 매핑이 없는 슬롯은 건너뜀. */
			continue;
		}
		/* [한국어] 4KB 노드 내부의 512개 2MB-청크 노드를 각각 해제. */
		for (j = 0; j < SPDK_COUNTOF(map_1gb4kb->map); j++) {
			free(map_1gb4kb->map[j]);
		}
		/* [한국어] 4KB 1단계 노드 자체 해제. */
		free(map_1gb4kb);
	}
	/* [한국어] 트리 보호용 뮤텍스 파괴. */
	pthread_mutex_destroy(&map->mutex);
	/* [한국어] 맵 구조체 해제. */
	free(map);
}

/*
 * [한국어]
 * spdk_mem_map_alloc - 새 페이지 단위 매핑 객체 할당
 *
 * @default_translation: 등록되지 않은 페이지의 기본 반환값.
 * @ops:                 매핑 변경 시 호출될 콜백 모음(notify_cb 등). NULL 허용.
 * @cb_ctx:              ops 콜백 호출 시 컨텍스트.
 * @return:              새 spdk_mem_map* 또는 NULL(메모리 부족).
 *
 * ops->notify_cb가 있으면 이미 등록된 모든 SPDK 메모리 영역에 대해 즉시
 * SPDK_MEM_MAP_NOTIFY_REGISTER 알림을 전송하여 새 맵이 기존 영역을 따라잡게 한다.
 * 그 후 g_spdk_mem_maps 큐에 추가되어 추후 이벤트 알림 대상이 된다.
 *
 * 실행 컨텍스트: 메인 스레드(매핑 alloc 시점). 내부적으로 g_spdk_mem_map_mutex 보유.
 */
struct spdk_mem_map *
spdk_mem_map_alloc(uint64_t default_translation, const struct spdk_mem_map_ops *ops, void *cb_ctx)
{
	/* [한국어] 새로 만들 맵 포인터. */
	struct spdk_mem_map *map;
	/* [한국어] 초기 통지 결과 코드. */
	int rc;

	/* [한국어] 맵 구조체를 0으로 초기화하며 할당(트리 슬롯 전부 NULL). */
	map = calloc(1, sizeof(*map));
	if (map == NULL) {
		return NULL;
	}

	/* [한국어] 트리 노드 동적 할당을 보호할 뮤텍스 초기화. */
	if (pthread_mutex_init(&map->mutex, NULL)) {
		/* [한국어] 실패 시 방금 할당한 구조체 정리. */
		free(map);
		return NULL;
	}

	/* [한국어] 미등록 페이지 기본 반환값 저장. */
	map->default_translation = default_translation;
	/* [한국어] 콜백 컨텍스트 저장. */
	map->cb_ctx = cb_ctx;
	/* [한국어] ops 모음이 주어졌으면 값 복사(notify_cb/are_contiguous). */
	if (ops) {
		map->ops = *ops;
	}

	/* [한국어] notify_cb가 있으면 기존 등록 영역을 따라잡기 위한 초기 통지 수행. */
	if (ops && ops->notify_cb) {
		/* [한국어] 전역 맵 큐를 보호(통지 + 큐 삽입 원자화). */
		pthread_mutex_lock(&g_spdk_mem_map_mutex);
		/* [한국어] 현재 등록된 모든 영역에 대해 REGISTER 콜백을 일괄 호출. */
		rc = mem_map_notify_walk(map, SPDK_MEM_MAP_NOTIFY_REGISTER);
		if (rc != 0) {
			/* [한국어] 초기 통지 실패 → 큐에 넣지 않고 정리 후 NULL. */
			pthread_mutex_unlock(&g_spdk_mem_map_mutex);
			DEBUG_PRINT("Initial mem_map notify failed\n");
			mem_map_free(map);
			return NULL;
		}
		/* [한국어] 이후 mem_event를 받을 수 있도록 전역 큐에 등록. */
		TAILQ_INSERT_TAIL(&g_spdk_mem_maps, map, tailq);
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	}

	/* [한국어] 완성된 맵 반환. */
	return map;
}

/*
 * [한국어]
 * spdk_mem_map_free - 매핑 객체 해제(이중 free 안전)
 *
 * @pmap: 해제할 spdk_mem_map 포인터의 포인터. NULL 또는 *pmap==NULL이면 무시.
 *
 * notify_cb가 있는 맵이라면 등록된 모든 영역에 대해 UNREGISTER 알림 전송 후 전역 큐에서 제거.
 * 마지막에 *pmap=NULL로 설정하여 호출자가 dangling pointer를 갖지 않게 한다.
 */
void
spdk_mem_map_free(struct spdk_mem_map **pmap)
{
	/* [한국어] *pmap에서 꺼낸 실제 맵 포인터. */
	struct spdk_mem_map *map;

	/* [한국어] 포인터의 포인터가 NULL이면 아무것도 안 함(방어). */
	if (!pmap) {
		return;
	}

	/* [한국어] 역참조해 실제 맵을 얻는다. */
	map = *pmap;

	/* [한국어] 이미 해제됐거나 NULL이면 무시(이중 free 안전). */
	if (!map) {
		return;
	}

	/* [한국어] notify_cb가 있던 맵이면 등록 해제 통지 + 전역 큐 제거. */
	if (map->ops.notify_cb) {
		pthread_mutex_lock(&g_spdk_mem_map_mutex);
		/* [한국어] 등록된 영역 전부에 UNREGISTER 통지(IOMMU unmap 등). */
		mem_map_notify_walk(map, SPDK_MEM_MAP_NOTIFY_UNREGISTER);
		/* [한국어] 이후 이벤트를 받지 않도록 큐에서 제거. */
		TAILQ_REMOVE(&g_spdk_mem_maps, map, tailq);
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	}

	/* [한국어] 트리 노드 + 구조체 실제 해제. */
	mem_map_free(map);
	/* [한국어] 호출자 포인터를 NULL로 만들어 dangling 방지. */
	*pmap = NULL;
}

/*
 * [한국어]
 * mem_check_region_unregistered - 구간이 전부 미등록 상태인지 검증하는 walk 콜백
 *
 * @map:   g_mem_reg_map(등록 마스터 맵).
 * @vaddr: 검사할 청크 시작.
 * @len:   청크 길이.
 * @ctx:   미사용.
 * @return: 전부 미등록이면 0, 일부라도 REGISTERED면 -EBUSY.
 *
 * mem_map_walk_region이 페이지 단위로 호출. 새 등록/예약 전에 충돌을 방지한다.
 * 호출 체인: spdk_mem_register/spdk_mem_reserve → mem_map_walk_region → mem_check_region_unregistered
 */
static int
mem_check_region_unregistered(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	/* [한국어] reg=등록 플래그, curlen=현재 변환 단위 길이. */
	uint64_t reg, curlen;

	/* [한국어] 청크 전체를 변환 단위로 쪼개며 검사. */
	while (len > 0) {
		/* [한국어] are_contiguous로 측정 가능한 만큼 길이 상한 설정. */
		curlen = len;
		/* [한국어] 이 부분의 등록 플래그 조회(연속 길이도 curlen에 반환). */
		reg = spdk_mem_map_translate(map, vaddr, &curlen);
		/* [한국어] 이미 등록된 영역과 겹치면 충돌 → -EBUSY. */
		if (reg & REG_MAP_REGISTERED) {
			return -EBUSY;
		}

		/* [한국어] 측정된 길이만큼 전진. */
		vaddr += curlen;
		len -= curlen;
	}

	/* [한국어] 전 구간 미등록 확인. */
	return 0;
}

/*
 * [한국어]
 * mem_check_region_registered - 구간이 전부 등록 상태인지 검증하는 walk 콜백
 *
 * @map:   g_mem_reg_map.
 * @vaddr: 검사할 청크 시작.
 * @len:   청크 길이.
 * @ctx:   미사용.
 * @return: 전부 등록이면 0, 일부라도 미등록이면 -EINVAL.
 *
 * unregister 전에 대상 구간이 실제로 등록되어 있는지 확인한다.
 * 호출 체인: spdk_mem_unregister → mem_map_walk_region → mem_check_region_registered
 */
static int
mem_check_region_registered(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	/* [한국어] reg=등록 플래그, curlen=현재 변환 단위 길이. */
	uint64_t reg, curlen;

	/* [한국어] 청크 전체를 검사. */
	while (len > 0) {
		curlen = len;
		/* [한국어] 등록 플래그 조회. */
		reg = spdk_mem_map_translate(map, vaddr, &curlen);
		/* [한국어] 미등록 부분이 있으면 해제 불가 → -EINVAL. */
		if (!(reg & REG_MAP_REGISTERED)) {
			return -EINVAL;
		}

		/* [한국어] 다음 단위로 전진. */
		vaddr += curlen;
		len -= curlen;
	}

	/* [한국어] 전 구간 등록 확인. */
	return 0;
}

/*
 * [한국어]
 * mem_register_page - 한 페이지 청크를 등록 맵에 REGISTERED로 표시하는 walk 콜백
 *
 * @map:   g_mem_reg_map.
 * @vaddr: 등록할 청크 시작.
 * @len:   청크 길이.
 * @ctx:   int* 페이지 카운터(영역의 첫 페이지를 구분하기 위함).
 * @return: spdk_mem_map_set_translation 결과(0 또는 -ENOMEM).
 *
 * 영역의 첫 페이지(*page==0)에는 REGISTERED|NOTIFY_START를, 나머지에는 REGISTERED만
 * 새긴다. NOTIFY_START는 unregister 시 등록 단위 경계를 보존하는 데 쓰인다.
 * 호출 체인: spdk_mem_register → mem_map_walk_region → mem_register_page
 */
static int
mem_register_page(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	/* [한국어] 영역 내 페이지 순번 카운터(첫 페이지=0). */
	int *page = ctx;

	/* [한국어] 첫 페이지면 NOTIFY_START도 함께, 그 외엔 REGISTERED만 설정. 후위증가로 다음 호출에선 비-0. */
	return spdk_mem_map_set_translation(map, vaddr, len, (*page)++ == 0 ?
					    REG_MAP_REGISTERED | REG_MAP_NOTIFY_START :
					    REG_MAP_REGISTERED);
}

/*
 * [한국어]
 * spdk_mem_register - SPDK가 인식할 메모리 영역 등록
 *
 * @vaddr: 등록할 영역의 시작 가상주소. 4KB 정렬 필수, 256TB 범위 내.
 * @len:   영역 길이. 4KB 배수.
 * @return: 성공 시 0, 이미 등록되어 충돌이면 -EBUSY, 인자 오류면 -EINVAL.
 *
 * 동작:
 *   1) 정렬·범위 검증.
 *   2) g_mem_reg_map 트리에서 이 영역이 미등록 상태인지 walk-check.
 *   3) walk하면서 첫 페이지에 REG_MAP_NOTIFY_START | REGISTERED, 나머지는 REGISTERED 표시.
 *   4) g_spdk_mem_maps의 모든 매핑에 NOTIFY_REGISTER 콜백 통지.
 *
 * 보통 hugepage 영역은 DPDK가 자동으로 등록하지만, 사용자가 자체 mmap한 영역(예: BAR,
 * P2P 메모리)을 SPDK vtophys에 노출하고 싶을 때 직접 호출한다.
 *
 * 실행 컨텍스트: 메인 스레드. g_spdk_mem_map_mutex 보유.
 */
int
spdk_mem_register(void *vaddr, size_t len)
{
	/* [한국어] 통지 대상 맵 순회 임시. */
	struct spdk_mem_map *map;
	/* [한국어] rc=결과 코드, page=영역 내 페이지 순번 카운터(첫 페이지 식별). */
	int rc, page = 0;

	/* [한국어] 가상주소가 256TB 범위를 벗어나면 트리 인덱싱 불가 → 거부. */
	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", vaddr);
		return -EINVAL;
	}

	/* [한국어] vaddr/len이 4KB 정렬이 아니면 페이지 단위 매핑 불가 → 거부. */
	if (((uintptr_t)vaddr & MASK_4KB) || (len & MASK_4KB)) {
		DEBUG_PRINT("invalid %s parameters, vaddr=%p len=%ju\n",
			    __func__, vaddr, len);
		return -EINVAL;
	}

	/* [한국어] 길이 0은 등록할 게 없음 → 성공 처리. */
	if (len == 0) {
		return 0;
	}

	/* [한국어] 전역 맵 큐/등록 맵을 보호. */
	pthread_mutex_lock(&g_spdk_mem_map_mutex);
	/* [한국어] 먼저 이 영역이 미등록 상태인지 검증(중복 등록 방지). */
	rc = mem_map_walk_region(g_mem_reg_map, (uint64_t)vaddr, len,
				 mem_check_region_unregistered, NULL);
	if (rc != 0) {
		/* [한국어] 이미 일부 등록되어 있으면 -EBUSY 등 그대로 반환. */
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return rc;
	}

	/* [한국어] 등록 맵에 REGISTERED 비트(첫 페이지엔 NOTIFY_START)를 새긴다. */
	rc = mem_map_walk_region(g_mem_reg_map, (uint64_t)vaddr, len,
				 mem_register_page, &page);
	if (rc != 0) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return rc;
	}

	/* [한국어] 등록된 모든 spdk_mem_map(vtophys/numa 등)에 REGISTER 통지. */
	TAILQ_FOREACH(map, &g_spdk_mem_maps, tailq) {
		/* [한국어] 각 맵의 notify_cb가 이 영역에 대해 IOVA/소켓ID 등을 채운다. */
		rc = map->ops.notify_cb(map->cb_ctx, map, SPDK_MEM_MAP_NOTIFY_REGISTER, vaddr, len);
		if (rc != 0) {
			/* [한국어] 개별 맵 실패는 로그만 남기고 계속(전체 실패로 보지 않음). */
			DEBUG_PRINT("failed to register vaddr %p to map %p, rc = %d\n", vaddr, map, rc);
		}
	}

	pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	return 0;
}

/*
 * [한국어]
 * mem_unregister_page - 한 페이지 청크를 등록 해제하며 등록 단위별로 UNREGISTER 통지를 누적하는 walk 콜백
 *
 * @map:   g_mem_reg_map(첫 진입 시). 내부에서 g_spdk_mem_maps도 순회.
 * @vaddr: 해제할 청크 시작.
 * @len:   청크 길이(VALUE_2MB 또는 VALUE_4KB).
 * @ctx:   struct iovec* — 현재까지 모은 "하나의 등록 영역" 경계를 누적 보관.
 * @return: 0 성공, 콜백 실패 시 그 rc.
 *
 * 핵심 아이디어: 등록은 NOTIFY_START 경계 단위로 이뤄졌으므로 해제 통지도 그 단위로
 * 다시 합쳐 보내야 한다. ctx의 iovec에 연속 페이지를 누적하다가, 새 NOTIFY_START를
 * 만나면 직전까지 모은 영역을 모든 맵에 역순으로 UNREGISTER 통지하고 새 영역을 시작한다.
 * 2MB 영역이 4KB 매핑이면(여러 작은 등록 조각일 수 있어) 4KB 단위로 재귀 분해한다.
 *
 * 실행 컨텍스트: 메인 스레드(g_spdk_mem_map_mutex 보유). 호출 체인: spdk_mem_unregister → mem_map_walk_region → mem_unregister_page (재귀)
 */
static int
mem_unregister_page(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	/* [한국어] 누적 중인 등록 영역 경계(iov_base=시작, iov_len=길이). */
	struct iovec *region = ctx;
	/* [한국어] off=2MB→4KB 분해 오프셋, reg=등록 플래그. */
	uint64_t off, reg;
	/* [한국어] 콜백 반환값. */
	int rc;

	/* We've already checked that the whole region we're trying to unregister was actually
	 * registered at this point.  But if we're trying to unregister a 2MB region that uses 4KB
	 * translations, we need to check each 4KB page individually, because that 2MB region could
	 * consist of multiple smaller registrations, so we might need to send multiple
	 * notifications.
	 */
	/* [한국어] 2MB 청크인데 실제로는 4KB 매핑이면 → 여러 작은 등록일 수 있어 4KB로 쪼개 재귀. */
	if (len > VALUE_4KB && mem_map_is_4kb_mapping(map, vaddr)) {
		/* [한국어] 4KB 매핑이라면 이 분기로 들어온 len은 반드시 2MB. */
		assert(len == VALUE_2MB);
		/* [한국어] 2MB를 512개 4KB 페이지로 나눠 각각 재귀 해제. */
		for (off = 0; off < len; off += VALUE_4KB) {
			rc = mem_unregister_page(map, vaddr + off, VALUE_4KB, ctx);
			if (rc != 0) {
				return rc;
			}
		}
		/* Set translation for the whole 2MB page to free the 4KB map */
		/* [한국어] 2MB 전체를 0으로 설정해 4KB 폴백 노드를 정리(2MB 슬롯 복귀). */
		return spdk_mem_map_set_translation(map, vaddr, len, 0);
	}

	/* [한국어] 이 페이지의 등록 플래그를 읽고 곧바로 0으로 클리어. */
	reg = spdk_mem_map_translate(map, vaddr, NULL);
	spdk_mem_map_set_translation(map, vaddr, len, 0);
	/* [한국어] 이미 누적된 영역이 있고 이 페이지가 새 등록의 시작(NOTIFY_START)이면 → 직전 영역을 먼저 통지. */
	if (region->iov_len > 0 && (reg & REG_MAP_NOTIFY_START)) {
		/* [한국어] 모든 맵에 역순으로 UNREGISTER 통지(등록 시 정순의 역). */
		TAILQ_FOREACH_REVERSE(map, &g_spdk_mem_maps, spdk_mem_map_head, tailq) {
			rc = map->ops.notify_cb(map->cb_ctx, map, SPDK_MEM_MAP_NOTIFY_UNREGISTER,
						region->iov_base, region->iov_len);
			if (rc != 0) {
				/* [한국어] 해제 통지 실패는 로그만(롤백 불가). */
				DEBUG_PRINT("failed to unregister vaddr %p from map %p, rc = %d\n",
					    region->iov_base, map, rc);
			}
		}

		/* [한국어] 새 등록 영역을 이 페이지부터 다시 누적 시작. */
		region->iov_base = (void *)vaddr;
		region->iov_len = len;
	} else {
		/* [한국어] 같은 등록 영역의 연속 페이지 → 길이만 늘려 누적. */
		region->iov_len += len;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_mem_unregister - 등록된 메모리 영역 해제
 *
 * @vaddr: 해제할 영역 시작 가상주소.
 * @len:   영역 길이.
 * @return: 성공 시 0, 영역 경계가 등록 단위와 불일치하면 -ERANGE, 부분 영역이 미등록이면 -EINVAL.
 *
 * 등록 시 단위 그대로만 해제할 수 있다(REG_MAP_NOTIFY_START가 새겨진 페이지부터). 일부만
 * 해제하려는 시도는 -ERANGE로 거부 — 다중 매핑 일관성 보장을 위함.
 *
 * 실행 컨텍스트: 메인 스레드.
 */
int
spdk_mem_unregister(void *vaddr, size_t len)
{
	/* [한국어] 통지 대상 맵 순회 임시. */
	struct spdk_mem_map *map;
	/* [한국어] mem_unregister_page가 누적할 영역 경계 누산기. */
	struct iovec region;
	/* [한국어] 결과 코드. */
	int rc;
	/* [한국어] reg=시작 페이지 플래그, newreg=구간 직후 페이지 플래그. */
	uint64_t reg, newreg;

	/* [한국어] 256TB 범위 검증. */
	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", vaddr);
		return -EINVAL;
	}

	/* [한국어] 4KB 정렬 검증. */
	if (((uintptr_t)vaddr & MASK_4KB) || (len & MASK_4KB)) {
		DEBUG_PRINT("invalid %s parameters, vaddr=%p len=%ju\n",
			    __func__, vaddr, len);
		return -EINVAL;
	}

	pthread_mutex_lock(&g_spdk_mem_map_mutex);

	/* The first page must be a start of a region. Also check if it's
	 * registered to make sure we don't return -ERANGE for non-registered
	 * regions.
	 */
	/* [한국어] 시작 페이지가 등록되어 있으면서 NOTIFY_START가 아니면 → 등록 영역 중간을
	 * 잘라내려는 시도이므로 -ERANGE. (미등록이면 통과시켜 후속 check가 -EINVAL 반환.) */
	reg = spdk_mem_map_translate(g_mem_reg_map, (uint64_t)vaddr, NULL);
	if ((reg & REG_MAP_REGISTERED) && (reg & REG_MAP_NOTIFY_START) == 0) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return -ERANGE;
	}

	/* [한국어] 해제 대상 전 구간이 실제로 등록되어 있는지 검증. */
	rc = mem_map_walk_region(g_mem_reg_map, (uint64_t)vaddr, len,
				 mem_check_region_registered, NULL);
	if (rc != 0) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return rc;
	}

	/* [한국어] 구간 바로 다음 페이지의 플래그 조회. */
	newreg = spdk_mem_map_translate(g_mem_reg_map, (uint64_t)vaddr + len, NULL);
	/* If the next page is registered, it must be a start of a region as well,
	 * otherwise we'd be unregistering only a part of a region.
	 */
	/* [한국어] 다음 페이지가 등록되어 있는데 NOTIFY_START가 아니면 → 이 해제가 한 등록 영역의
	 * 앞부분만 잘라내는 셈이므로 -ERANGE로 거부(등록 단위 보존 불변식). */
	if ((newreg & REG_MAP_NOTIFY_START) == 0 && (newreg & REG_MAP_REGISTERED)) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return -ERANGE;
	}

	/* [한국어] 누산기 초기화(아직 모은 영역 없음). */
	region.iov_base = vaddr;
	region.iov_len = 0;
	/* [한국어] 등록 맵을 클리어하며 등록 단위별로 UNREGISTER 통지 누적. */
	rc = mem_map_walk_region(g_mem_reg_map, (uint64_t)vaddr, len,
				 mem_unregister_page, &region);
	if (rc != 0) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return rc;
	}

	/* [한국어] 마지막으로 모은 영역이 남아 있으면(walk 끝에서 아직 통지 안 됨) 마무리 통지. */
	if (region.iov_len > 0) {
		TAILQ_FOREACH_REVERSE(map, &g_spdk_mem_maps, spdk_mem_map_head, tailq) {
			rc = map->ops.notify_cb(map->cb_ctx, map, SPDK_MEM_MAP_NOTIFY_UNREGISTER,
						region.iov_base, region.iov_len);
			if (rc != 0) {
				DEBUG_PRINT("failed to unregister vaddr %p from map %p, rc = %d\n",
					    region.iov_base, map, rc);
			}
		}
	}

	pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	return 0;
}

/*
 * [한국어]
 * spdk_mem_reserve - 영역을 "예약"하여 다른 등록과 충돌하지 않게 표시
 *
 * @vaddr: 예약할 영역 시작.
 * @len:   영역 길이.
 * @return: 성공 시 0, 이미 어떤 부분이 등록되어 충돌이면 -EBUSY.
 *
 * 실제 변환 값은 default_translation으로 두지만 트리 노드는 할당하여 해당 범위에 다른
 * 등록이 들어오지 못하게 한다. 예: P2P DMA용 BAR 영역 예약, 외부 라이브러리가 쓸 mmap 영역 등.
 */
int
spdk_mem_reserve(void *vaddr, size_t len)
{
	/* [한국어] 모든 맵에 예약 표시를 전파하기 위한 순회 임시. */
	struct spdk_mem_map *map;
	/* [한국어] 충돌 검사 결과. */
	int rc;

	/* [한국어] 256TB 범위 검증. */
	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", vaddr);
		return -EINVAL;
	}

	/* [한국어] 4KB 정렬 검증. */
	if (((uintptr_t)vaddr & MASK_4KB) || (len & MASK_4KB)) {
		DEBUG_PRINT("invalid %s parameters, vaddr=%p len=%ju\n",
			    __func__, vaddr, len);
		return -EINVAL;
	}

	/* [한국어] 길이 0이면 예약할 게 없음. */
	if (len == 0) {
		return 0;
	}

	pthread_mutex_lock(&g_spdk_mem_map_mutex);

	/* Check if any part of this range is already registered */
	/* [한국어] 이미 등록된 영역과 겹치면 예약 불가(-EBUSY). */
	rc = mem_map_walk_region(g_mem_reg_map, (uint64_t)vaddr, len,
				 mem_check_region_unregistered, NULL);
	if (rc != 0) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return rc;
	}

	/* Simply set the translation to the memory map's default. This allocates the space in the
	 * map but does not provide a valid translation. */
	/* [한국어] 등록 맵에 default 값을 설정 → 트리 노드는 할당되지만 유효 변환은 없음.
	 * 이렇게 해당 범위 슬롯을 차지해 다른 등록이 끼어들지 못하게 "예약"한다. */
	spdk_mem_map_set_translation(g_mem_reg_map, (uint64_t)vaddr, len,
				     g_mem_reg_map->default_translation);

	/* [한국어] 다른 모든 맵에도 같은 범위를 default로 채워 노드 일관성 확보. */
	TAILQ_FOREACH(map, &g_spdk_mem_maps, tailq) {
		spdk_mem_map_set_translation(map, (uint64_t)vaddr, len, map->default_translation);
	}

	pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	return 0;
}

/*
 * [한국어]
 * mem_map_get_map_1gb2mb - 2MB 룩업 노드(map_1gb2mb)를 얻거나(필요 시) 새로 할당
 *
 * @map:     대상 매핑.
 * @vfn_2mb: 2MB 가상 페이지 번호.
 * @alloc:   노드가 없을 때 새로 할당할지 여부.
 * @return:  노드 포인터, 또는 (alloc=false이고 없음) / 할당 실패 시 NULL.
 *
 * 256TB 루트에서 해당 1GB 슬롯의 2MB 노드를 찾는다. 없고 alloc이면 mutex를 잡고
 * double-checked locking으로 새 노드를 malloc해 모든 엔트리를 default로 초기화한다.
 * 한 번 등록되면 노드는 free 전까지 위치가 변하지 않아 lock-free 룩업이 가능하다.
 *
 * 호출 체인: mem_map_set_2mb_translation / mem_map_set_4kb_translation → mem_map_get_map_1gb2mb
 */
static struct map_1gb2mb *
mem_map_get_map_1gb2mb(struct spdk_mem_map *map, uint64_t vfn_2mb, bool alloc)
{
	/* [한국어] 결과 노드 포인터. */
	struct map_1gb2mb *map_1gb2mb;
	/* [한국어] 256TB 루트 인덱스. */
	uint64_t idx_256tb = MAP_256TB_IDX(vfn_2mb);
	/* [한국어] 새 노드 초기화 루프 인덱스. */
	size_t i;

	/* [한국어] 인덱스가 루트 배열 범위를 넘으면 잘못된 주소 → NULL. */
	if (spdk_unlikely(idx_256tb >= SPDK_COUNTOF(map->map_256tb.map))) {
		return NULL;
	}

	/* [한국어] 현재 슬롯의 2MB 노드 포인터(없으면 NULL). */
	map_1gb2mb = map->map_256tb.map[idx_256tb].map_1gb2mb;
	/* [한국어] 노드가 없고 할당 요청이면 새로 만든다. */
	if (!map_1gb2mb && alloc) {
		/* [한국어] 동시 할당 경쟁 방지를 위해 맵 뮤텍스 획득. */
		pthread_mutex_lock(&map->mutex);

		/* Recheck to make sure nobody else got the mutex first. */
		/* [한국어] 락 획득 전 다른 스레드가 이미 할당했을 수 있어 재확인(double-checked locking). */
		map_1gb2mb = map->map_256tb.map[idx_256tb].map_1gb2mb;
		if (!map_1gb2mb) {
			/* [한국어] 512개 2MB 변환 배열(4KB) 할당. */
			map_1gb2mb = malloc(sizeof(struct map_1gb2mb));
			if (map_1gb2mb) {
				/* initialize all entries to default translation */
				/* [한국어] 모든 슬롯을 미등록 기본값으로 채움. */
				for (i = 0; i < SPDK_COUNTOF(map_1gb2mb->translation_2mb); i++) {
					map_1gb2mb->translation_2mb[i] = map->default_translation;
				}
				/* [한국어] 마지막에 트리에 게시 — 이 시점부터 lock-free 룩업이 본다. */
				map->map_256tb.map[idx_256tb].map_1gb2mb = map_1gb2mb;
			}
		}

		pthread_mutex_unlock(&map->mutex);

		/* [한국어] 여전히 NULL이면 malloc 실패. */
		if (!map_1gb2mb) {
			DEBUG_PRINT("allocation failed\n");
			return NULL;
		}
	}

	/* [한국어] 노드(또는 alloc=false이고 미존재 시 NULL) 반환. */
	return map_1gb2mb;
}

/*
 * [한국어]
 * mem_map_get_map_1gb4kb - 4KB 폴백 1단계 노드(map_1gb4kb)를 얻거나(필요 시) 할당
 *
 * @map:     대상 매핑.
 * @vfn_4kb: 4KB 가상 페이지 번호.
 * @alloc:   없을 때 할당 여부.
 * @return:  노드 포인터 또는 NULL.
 *
 * 4KB 변환이 처음 필요할 때 1GB당 하나의 map_1gb4kb(2MB 청크 포인터 512개)를 calloc한다.
 * double-checked locking으로 동시 할당을 방지한다.
 *
 * 호출 체인: mem_map_get_map_2mb4kb → mem_map_get_map_1gb4kb
 */
static struct map_1gb4kb *
mem_map_get_map_1gb4kb(struct spdk_mem_map *map, uint64_t vfn_4kb, bool alloc)
{
	/* [한국어] 결과 노드 포인터. */
	struct map_1gb4kb *map_1gb4kb;
	/* [한국어] 4KB VFN을 2MB VFN으로 축소해 루트 인덱스 계산. */
	uint64_t vfn_2mb, idx_256tb;

	vfn_2mb = FN_4KB_TO_2MB(vfn_4kb);
	idx_256tb = MAP_256TB_IDX(vfn_2mb);
	/* [한국어] 루트 범위 초과 검증. */
	if (idx_256tb >= SPDK_COUNTOF(map->map_256tb.map)) {
		return NULL;
	}

	/* [한국어] 현재 4KB 1단계 노드 포인터. */
	map_1gb4kb = map->map_256tb.map[idx_256tb].map_1gb4kb;
	if (map_1gb4kb == NULL && alloc) {
		/* [한국어] 할당 경쟁 방지 락. */
		pthread_mutex_lock(&map->mutex);
		/* Recheck to make sure nobody else got the mutex first. */
		/* [한국어] 재확인 후 0초기화된 포인터 배열(512개) 할당. */
		map_1gb4kb = map->map_256tb.map[idx_256tb].map_1gb4kb;
		if (map_1gb4kb == NULL) {
			map_1gb4kb = calloc(1, sizeof(*map_1gb4kb));

		}
		/* [한국어] 트리에 게시(NULL일 수도 있으나 그대로 저장 — 다음 시도에서 재할당). */
		map->map_256tb.map[idx_256tb].map_1gb4kb = map_1gb4kb;
		pthread_mutex_unlock(&map->mutex);
	}

	return map_1gb4kb;
}

/*
 * [한국어]
 * mem_map_get_map_2mb4kb - 4KB 변환 배열 노드(map_2mb4kb)를 얻거나(필요 시) 할당
 *
 * @map:     대상 매핑.
 * @vfn_4kb: 4KB 가상 페이지 번호.
 * @alloc:   없을 때 할당 여부.
 * @return:  노드 포인터 또는 NULL.
 *
 * 1GB→2MB 청크 슬롯에 512개 4KB 변환 배열을 할당한다. 새로 만들 때, 기존에 이 2MB 청크가
 * 가지고 있던 2MB 변환 값을 512개 4KB 슬롯에 그대로 복사해 의미를 보존한다.
 *
 * 호출 체인: mem_map_set_4kb_translation → mem_map_get_map_2mb4kb → mem_map_get_map_1gb4kb
 */
static struct map_2mb4kb *
mem_map_get_map_2mb4kb(struct spdk_mem_map *map, uint64_t vfn_4kb, bool alloc)
{
	/* [한국어] 결과 4KB 배열 노드. */
	struct map_2mb4kb *map_2mb4kb;
	/* [한국어] 상위 4KB 1단계 노드. */
	struct map_1gb4kb *map_1gb4kb;
	/* [한국어] 2MB VFN, 1GB 내 인덱스, 기존 2MB 변환 값. */
	uint64_t vfn_2mb, idx_1gb, translation;
	/* [한국어] mem_map_translate 출력 페이지 크기. */
	int page_size;
	/* [한국어] 초기화 루프 인덱스. */
	size_t i;

	/* [한국어] 먼저 4KB 1단계 노드 확보. */
	map_1gb4kb = mem_map_get_map_1gb4kb(map, vfn_4kb, alloc);
	if (map_1gb4kb == NULL) {
		return NULL;
	}

	/* [한국어] 1GB 청크 내 2MB 슬롯 인덱스 계산. */
	vfn_2mb = FN_4KB_TO_2MB(vfn_4kb);
	idx_1gb = MAP_1GB_IDX(vfn_2mb);
	/* [한국어] 해당 2MB 청크의 4KB 배열 노드 포인터. */
	map_2mb4kb = map_1gb4kb->map[idx_1gb];
	if (map_2mb4kb == NULL && alloc) {
		pthread_mutex_lock(&map->mutex);
		/* Recheck to make sure nobody else got the mutex first. */
		/* [한국어] 재확인 후 512개 4KB 변환 배열 할당. */
		map_2mb4kb = map_1gb4kb->map[idx_1gb];
		if (map_2mb4kb == NULL) {
			map_2mb4kb = malloc(sizeof(*map_2mb4kb));
			if (map_2mb4kb != NULL) {
				/* Fill the 4kb map with the 2mb translation, if it had any */
				/* [한국어] 이 2MB 청크의 기존 변환 값을 읽어(있다면) 모든 4KB 슬롯에 복사 → 분할 전후 의미 보존. */
				translation = mem_map_translate(map, vfn_4kb << SHIFT_4KB,
								&page_size);
				for (i = 0; i < SPDK_COUNTOF(map_2mb4kb->translation_4kb); i++) {
					map_2mb4kb->translation_4kb[i] = translation;
				}
				/* [한국어] 트리에 게시. */
				map_1gb4kb->map[idx_1gb] = map_2mb4kb;
			}
		}
		pthread_mutex_unlock(&map->mutex);
	}

	return map_2mb4kb;
}

/*
 * [한국어]
 * mem_map_set_4kb_translation - 4KB 페이지 하나에 변환 값 설정
 *
 * @map:         대상 매핑.
 * @vaddr:       4KB 정렬 가상주소.
 * @translation: 새 변환 값(IOVA 등).
 * @return:      0 성공, 노드 할당 실패 시 -ENOMEM.
 *
 * 4KB 배열 노드를 확보(alloc=true)해 해당 슬롯에 값을 쓴 뒤, 같은 2MB 슬롯은 default로
 * 돌려 "이 2MB 영역은 4KB 매핑을 쓴다"는 신호를 트리에 남긴다. mem_map_translate가 2MB
 * miss → 4KB 폴백으로 흐르게 하는 핵심 규칙이다.
 *
 * 호출 체인: mem_map_set_page_translation → mem_map_set_4kb_translation
 */
static int
mem_map_set_4kb_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t translation)
{
	/* [한국어] 4KB 배열 노드. */
	struct map_2mb4kb *map_2mb4kb;
	/* [한국어] 2MB 노드(default로 되돌릴 대상). */
	struct map_1gb2mb *map_1gb2mb;
	/* [한국어] 4KB/2MB VFN. */
	uint64_t vfn_4kb, vfn_2mb;
	/* [한국어] 각 단계 인덱스. */
	uint64_t idx_2mb, idx_1gb;

	/* [한국어] 4KB VFN 계산 후 배열 노드 확보(없으면 할당). */
	vfn_4kb = VFN_4KB(vaddr);
	map_2mb4kb = mem_map_get_map_2mb4kb(map, vfn_4kb, true);
	if (!map_2mb4kb) {
		DEBUG_PRINT("could not get %p map\n", (void *)vaddr);
		return -ENOMEM;
	}

	/* [한국어] 2MB 청크 내 4KB 인덱스에 변환 값 기록. */
	idx_2mb = MAP_2MB_IDX(vfn_4kb);
	map_2mb4kb->translation_4kb[idx_2mb] = translation;

	/* Set 2MB map to the default translation to indicate this region has 4KB mapping */
	/* [한국어] 같은 2MB 슬롯을 default로 되돌려 룩업이 4KB 폴백을 타게 한다. */
	vfn_2mb = FN_4KB_TO_2MB(vfn_4kb);
	map_1gb2mb = mem_map_get_map_1gb2mb(map, vfn_2mb, false);
	if (map_1gb2mb != NULL) {
		idx_1gb = MAP_1GB_IDX(vfn_2mb);
		map_1gb2mb->translation_2mb[idx_1gb] = map->default_translation;
	}

	return 0;
}

/*
 * [한국어]
 * mem_map_set_2mb_translation - 2MB 페이지 하나에 변환 값 설정
 *
 * @map:         대상 매핑.
 * @vaddr:       2MB 정렬 가상주소.
 * @translation: 새 변환 값.
 * @return:      0 성공, -ENOMEM.
 *
 * 2MB 노드를 확보해 슬롯에 값을 쓴다. 추가로 이 2MB 청크에 이미 4KB 배열 노드가 있으면
 * 그 512개 슬롯도 같은 값으로 채워 두 표현의 일관성을 유지한다(특히 default 설정 시
 * 4KB 매핑 표시 의미와 충돌하지 않도록).
 *
 * 호출 체인: mem_map_set_page_translation → mem_map_set_2mb_translation
 */
static int
mem_map_set_2mb_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t translation)
{
	/* [한국어] 보조 4KB 배열 노드(있을 경우 동기화 대상). */
	struct map_2mb4kb *map_2mb4kb;
	/* [한국어] 2MB 노드. */
	struct map_1gb2mb *map_1gb2mb;
	/* [한국어] i=4KB 슬롯 인덱스, vfn/idx=2MB 인덱스. */
	uint64_t i, vfn_2mb, idx_1gb;

	/* [한국어] 2MB VFN 계산 후 노드 확보(없으면 할당). */
	vfn_2mb = VFN_2MB(vaddr);
	map_1gb2mb = mem_map_get_map_1gb2mb(map, vfn_2mb, true);
	if (!map_1gb2mb) {
		DEBUG_PRINT("could not get %p map\n", (void *)vaddr);
		return -ENOMEM;
	}

	/* [한국어] 2MB 슬롯에 변환 값 기록. */
	idx_1gb = MAP_1GB_IDX(vfn_2mb);
	map_1gb2mb->translation_2mb[idx_1gb] = translation;

	/* Set up 4KB translations too in case this region later uses 4KB mapping or we're
	 * setting the default translation (which is also used to indicate a 4KB mapping).
	 */
	/* [한국어] 이 2MB 청크에 4KB 배열 노드가 이미 있으면 512개 슬롯을 같은 값으로 동기화(할당은 안 함). */
	map_2mb4kb = mem_map_get_map_2mb4kb(map, FN_2MB_TO_4KB(vfn_2mb), false);
	if (map_2mb4kb != NULL) {
		for (i = 0; i < SPDK_COUNTOF(map_2mb4kb->translation_4kb); i++) {
			map_2mb4kb->translation_4kb[i] = translation;
		}
	}

	return 0;
}

/*
 * [한국어]
 * mem_map_set_page_translation - 페이지 크기에 따라 2MB/4KB 변환 설정을 디스패치하는 walk 콜백
 *
 * @map:         대상 매핑.
 * @vaddr:       페이지 정렬 가상주소.
 * @page_size:   VALUE_4KB 또는 VALUE_2MB.
 * @translation: 변환 값(void* 형태로 전달되나 실제로는 uint64_t).
 * @return:      하위 set 함수 결과(0/-ENOMEM/-EINVAL).
 *
 * mem_map_walk_region이 각 페이지 청크마다 호출. 호출 체인: spdk_mem_map_set_translation → mem_map_walk_region → mem_map_set_page_translation
 */
static int
mem_map_set_page_translation(struct spdk_mem_map *map, uint64_t vaddr, size_t page_size,
			     void *translation)
{
	/* [한국어] walk가 알려준 페이지 크기로 분기. */
	switch (page_size) {
	case VALUE_4KB:
		/* [한국어] 4KB 경로. */
		return mem_map_set_4kb_translation(map, vaddr, (uint64_t)translation);
	case VALUE_2MB:
		/* [한국어] 2MB 경로. */
		return mem_map_set_2mb_translation(map, vaddr, (uint64_t)translation);
	default:
		/* [한국어] walk는 두 크기만 넘기므로 도달 불가(방어적 assert). */
		assert(0 && "should never happan");
		return -EINVAL;
	}
}

/*
 * [한국어]
 * spdk_mem_map_set_translation - 페이지 단위 변환 값 설정 (외부 API)
 *
 * @map:   대상 spdk_mem_map.
 * @vaddr: 페이지 정렬된 가상주소(2MB 또는 4KB 정렬).
 * @size:  영역 크기(2MB 또는 4KB 배수).
 * @translation: 새 매핑 값(예: IOVA, NUMA ID, 임의 사용자 값).
 * @return: 성공 시 0, 인자 오류면 -EINVAL.
 *
 * 페이지 크기에 따라 2MB 또는 4KB 변환을 호출. SPDK는 hugepage에는 2MB, 일반 페이지에는
 * 4KB 분기를 자동 선택한다.
 */
int
spdk_mem_map_set_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size,
			     uint64_t translation)
{
	/* [한국어] 256TB 범위 검증. */
	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %" PRIu64 "\n", vaddr);
		return -EINVAL;
	}

	/* [한국어] 4KB 정렬 검증. */
	if (((uintptr_t)vaddr & MASK_4KB) || (size & MASK_4KB)) {
		DEBUG_PRINT("invalid %s parameters, vaddr=%" PRIu64 " len=%" PRIu64 "\n",
			    __func__, vaddr, size);
		return -EINVAL;
	}

	/* [한국어] 구간을 페이지 단위로 순회하며 페이지별 set 콜백 적용. */
	return mem_map_walk_region(map, vaddr, size, mem_map_set_page_translation,
				   (void *)translation);
}

/*
 * [한국어]
 * spdk_mem_map_clear_translation - 페이지 매핑 제거(default로 되돌림)
 *
 * @map:   대상 매핑.
 * @vaddr: 가상주소.
 * @size:  영역 크기.
 * @return: spdk_mem_map_set_translation에 default_translation을 넘긴 결과.
 */
int
spdk_mem_map_clear_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size)
{
	/* [한국어] 매핑을 default_translation으로 되돌리는 것 = 사실상 매핑 제거. */
	return spdk_mem_map_set_translation(map, vaddr, size, map->default_translation);
}

/*
 * [한국어]
 * spdk_mem_map_translate - 가상주소를 변환하고 연속 매핑 길이까지 측정 (외부 API)
 *
 * @map:   대상 매핑(vtophys/numa 등).
 * @vaddr: 변환할 가상주소(페이지 정렬일 필요는 없음 — 오프셋 포함 가능).
 * @size:  입출력. 입력=원하는 최대 길이, 출력=가상·물리 연속인 실제 길이. NULL 허용.
 * @return: 변환 값(IOVA 등) 또는 default_translation.
 *
 * mem_map_translate로 시작 페이지 변환을 얻고, 그 페이지의 남은 길이(오프셋 보정)를 계산한다.
 * size가 주어지고 ops->are_contiguous 콜백이 있으면, 다음 페이지들이 물리적으로 연속인 한
 * 길이를 누적해 단일 PRP/SGL 엔트리로 묶을 수 있는 최대 길이를 반환한다. NVMe 드라이버가
 * DMA 디스크립터 개수를 줄이기 위해 이 길이 정보를 활용한다.
 *
 * 실행 컨텍스트: 모든 reactor(hot path). 트리는 읽기 전용이므로 lock-free.
 * 호출 체인: spdk_vtophys / spdk_mem_get_numa_id / 기타 → spdk_mem_map_translate → mem_map_translate
 */
inline uint64_t
spdk_mem_map_translate(const struct spdk_mem_map *map, uint64_t vaddr, uint64_t *size)
{
	/* [한국어] 현재까지 확인된 연속 길이. */
	uint64_t cur_size;
	/* [한국어] 이전 페이지 변환 값(연속성 비교 기준). */
	uint64_t prev_translation;
	/* [한국어] 시작 페이지 변환 값(최종 반환값). */
	uint64_t orig_translation;
	/* [한국어] 현재 페이지 변환 값. */
	uint64_t curr_translation;
	/* [한국어] mem_map_translate가 채우는 페이지 크기. */
	int page_size;

	/* [한국어] 256TB 범위 밖이면 기본값 반환(잘못된 주소). */
	if (spdk_unlikely(vaddr & ~MASK_256TB)) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", (void *)vaddr);
		return map->default_translation;
	}

	/* [한국어] 시작 페이지 변환 + 페이지 크기 조회. */
	curr_translation = mem_map_translate(map, vaddr, &page_size);
	/* [한국어] 시작 페이지에서 vaddr 오프셋 이후로 남은 길이(4KB/2MB 페이지 내 잔여). */
	cur_size = page_size - (page_size == VALUE_4KB ? _4KB_OFFSET(vaddr) : _2MB_OFFSET(vaddr));
	/* [한국어] size를 안 보거나, 연속성 콜백이 없거나, 미등록이면 한 페이지만 반환. */
	if (size == NULL || map->ops.are_contiguous == NULL ||
	    curr_translation == map->default_translation) {
		if (size != NULL) {
			/* [한국어] 요청 길이와 현재 페이지 잔여 중 작은 쪽으로 클램프. */
			*size = spdk_min(*size, cur_size);
		}
		return curr_translation;
	}

	/* [한국어] 연속 길이 누적 시작: 시작 변환 값을 기준/원본으로 보관. */
	prev_translation = orig_translation = curr_translation;
	vaddr += cur_size;
	/* [한국어] 요청 길이를 채울 때까지 다음 페이지들을 검사. */
	while (cur_size < *size) {
		/* [한국어] 다음 페이지 변환 값 조회. */
		curr_translation = mem_map_translate(map, vaddr, &page_size);
		/* [한국어] 이전 페이지와 물리적으로 연속이 아니면 누적 중단. */
		if (!map->ops.are_contiguous(prev_translation, curr_translation)) {
			break;
		}

		/* [한국어] 연속이면 길이/주소 전진하고 기준 갱신. */
		cur_size += page_size;
		vaddr += page_size;
		prev_translation = curr_translation;
	}

	/* [한국어] 요청 길이와 누적 연속 길이 중 작은 쪽 반환. */
	*size = spdk_min(*size, cur_size);
	/* [한국어] 시작 페이지의 변환 값(IOVA) 반환. */
	return orig_translation;
}

/*
 * [한국어]
 * memory_hotplug_cb - DPDK 메모리 이벤트(hugepage 동적 추가/삭제) 콜백
 *
 * @event_type: RTE_MEM_EVENT_ALLOC 또는 _FREE.
 * @addr:       추가/삭제된 영역 시작 가상주소.
 * @len:        영역 길이.
 * @arg:        미사용(등록 시 NULL).
 *
 * DPDK가 hugepage를 동적으로 alloc/free할 때마다 호출되어 SPDK 측 등록 트리를 동기화한다.
 * ALLOC이면 spdk_mem_register로 등록(→ vtophys 룩업 가능), FREE면 spdk_mem_unregister.
 * 외부에서 DPDK를 따로 초기화한 경우(--match-allocations 미보장), 등록 단위와 해제 단위가
 * 달라 RDMA MR 같은 곳에서 문제가 생길 수 있어 RTE_MEMSEG_FLAG_DO_NOT_FREE로 세그먼트를 고정한다.
 *
 * 실행 컨텍스트: DPDK가 메모리 변경 시 호출(보통 메인 스레드/할당 경로).
 * 호출 체인: DPDK rte_mem_event → memory_hotplug_cb → spdk_mem_register/unregister
 */
static void
memory_hotplug_cb(enum rte_mem_event event_type,
		  const void *addr, size_t len, void *arg)
{
	/* [한국어] 새 hugepage 영역이 할당된 이벤트. */
	if (event_type == RTE_MEM_EVENT_ALLOC) {
		/* [한국어] SPDK 등록 트리에 추가 → 이후 vtophys 룩업 가능. */
		spdk_mem_register((void *)addr, len);

		/* [한국어] SPDK가 DPDK를 직접 초기화한 경우(외부 임베드 아님)면 추가 처리 불필요. */
		if (!spdk_env_dpdk_external_init()) {
			return;
		}

		/* When the user initialized DPDK separately, we can't
		 * be sure that --match-allocations RTE flag was specified.
		 * Without this flag, DPDK can free memory in different units
		 * than it was allocated. It doesn't work with things like RDMA MRs.
		 *
		 * For such cases, we mark segments so they aren't freed.
		 */
		/* [한국어] 외부 DPDK 초기화 시 할당/해제 단위 불일치를 막기 위해 각 세그먼트를 freeze. */
		while (len > 0) {
			/* [한국어] 현재 주소를 포함하는 DPDK 메모리 세그먼트. */
			struct rte_memseg *seg;

			/* [한국어] 가상주소 → 세그먼트 디스크립터 조회. */
			seg = rte_mem_virt2memseg(addr, NULL);
			assert(seg != NULL);
			/* [한국어] DO_NOT_FREE 플래그로 DPDK가 이 세그먼트를 임의 해제하지 못하게 고정. */
			seg->flags |= RTE_MEMSEG_FLAG_DO_NOT_FREE;
			/* [한국어] hugepage 크기만큼 다음 세그먼트로 전진. */
			addr = (void *)((uintptr_t)addr + seg->hugepage_sz);
			len -= seg->hugepage_sz;
		}
	} else if (event_type == RTE_MEM_EVENT_FREE) {
		/* [한국어] hugepage 해제 이벤트 → SPDK 트리에서 제거(IOMMU unmap 포함). */
		spdk_mem_unregister((void *)addr, len);
	}
}

/*
 * [한국어]
 * memory_iter_cb - 부팅 시 기존 DPDK 메모리 세그먼트를 등록하는 walk 콜백
 *
 * @msl: 세그먼트가 속한 메모리 세그먼트 리스트(미사용).
 * @ms:  현재 세그먼트.
 * @len: 연속 세그먼트 길이.
 * @arg: 미사용.
 * @return: spdk_mem_register 결과(0 또는 음수 errno).
 *
 * mem_map_init에서 rte_memseg_contig_walk로 호출되어, 콜백 등록 이전에 이미 존재하던
 * 모든 hugepage 영역을 SPDK 트리에 채운다(이후엔 memory_hotplug_cb가 증분 처리).
 * 호출 체인: mem_map_init → rte_memseg_contig_walk → memory_iter_cb → spdk_mem_register
 */
static int
memory_iter_cb(const struct rte_memseg_list *msl,
	       const struct rte_memseg *ms, size_t len, void *arg)
{
	/* [한국어] 세그먼트의 가상주소/길이를 SPDK에 등록. */
	return spdk_mem_register(ms->addr, len);
}

/* [한국어] memory_hotplug_cb 콜백이 DPDK에 등록되었는지 추적(중복 unregister 방지). */
static bool g_mem_event_cb_registered = false;

/*
 * [한국어]
 * mem_map_mem_event_callback_register - DPDK 메모리 이벤트 콜백 등록
 *
 * @return: 0 성공, DPDK 등록 실패 시 그 rc.
 *
 * rte_mem_event_callback_register로 "spdk" 이름의 hotplug 콜백을 등록한다.
 * 성공 시 g_mem_event_cb_registered=true로 표시해 fini 때 중복 해제를 막는다.
 * 호출 체인: mem_map_init → mem_map_mem_event_callback_register
 */
static int
mem_map_mem_event_callback_register(void)
{
	/* [한국어] DPDK 등록 결과. */
	int rc;

	/* [한국어] hugepage 동적 추가/삭제 알림을 받도록 콜백 등록. */
	rc = rte_mem_event_callback_register("spdk", memory_hotplug_cb, NULL);
	if (rc != 0) {
		return rc;
	}

	/* [한국어] 등록 성공 표시. */
	g_mem_event_cb_registered = true;
	return 0;
}

/*
 * [한국어]
 * mem_map_mem_event_callback_unregister - DPDK 메모리 이벤트 콜백 해제(멱등)
 *
 * 등록된 적이 있을 때만 rte_mem_event_callback_unregister를 호출하고 플래그를 내린다.
 * 호출 체인: mem_map_fini / mem_map_init(에러 경로) → mem_map_mem_event_callback_unregister
 */
static void
mem_map_mem_event_callback_unregister(void)
{
	/* [한국어] 등록되어 있을 때만 해제(이중 해제 방지). */
	if (g_mem_event_cb_registered) {
		g_mem_event_cb_registered = false;
		rte_mem_event_callback_unregister("spdk", NULL);
	}
}

/*
 * [한국어]
 * mem_reg_map_check_contiguous - 등록 맵 전용 are_contiguous 콜백(등록 영역 경계 판정)
 *
 * @addr1: 이전 페이지의 등록 플래그 값.
 * @addr2: 다음 페이지의 등록 플래그 값.
 * @return: 두 페이지를 같은 등록 영역으로 묶을 수 있으면 1, 아니면 0.
 *
 * 일반 vtophys와 달리 등록 맵에서 "연속"의 의미는 "같은 등록 단위에 속함"이다.
 * 다음 페이지가 미등록이거나 새 등록의 시작(NOTIFY_START)이면 경계로 보고 0을 반환한다.
 * spdk_mem_map_translate가 등록 영역의 길이를 측정할 때 이 콜백을 사용한다.
 *
 * 호출 체인: spdk_mem_map_translate(g_mem_reg_map) → are_contiguous → mem_reg_map_check_contiguous
 */
static int
mem_reg_map_check_contiguous(uint64_t addr1, uint64_t addr2)
{
	/* [한국어] 이전 페이지는 항상 등록 상태여야 함(불변식). */
	assert(addr1 & REG_MAP_REGISTERED);
	/* [한국어] 다음 페이지가 미등록이면 연속 종료. */
	if (!(addr2 & REG_MAP_REGISTERED)) {
		return 0;
	}

	/* addr2 is the start of a new registration */
	/* [한국어] 다음 페이지가 새 등록의 시작이면 경계 → 0, 아니면 같은 영역 → 1. */
	return !(addr2 & REG_MAP_NOTIFY_START);
}

/*
 * [한국어]
 * mem_map_init - 메모리 등록 인프라 초기화(g_mem_reg_map + 이벤트 콜백 + 기존 세그먼트 등록)
 *
 * @legacy_mem: DPDK legacy 메모리 모드 여부(동적 hugepage 추가/제거 없음).
 * @return:     0 성공, 실패 시 음수 errno(중간 단계 롤백).
 *
 * (1) 마스터 등록 맵 g_mem_reg_map을 alloc하고, (2) legacy가 아니면 hotplug 콜백을 등록,
 * (3) 이미 존재하는 모든 DPDK 메모리 세그먼트를 walk로 등록한다. SPDK 부팅 시 env 초기화
 * 단계에서 한 번 호출된다. 실패 시 콜백 해제 → 맵 해제 순으로 정리한다.
 *
 * 실행 컨텍스트: 메인 스레드(초기화). 호출 체인: spdk_env_dpdk_post_init/mem_init → mem_map_init
 */
int
mem_map_init(bool legacy_mem)
{
	/* [한국어] 등록 맵 ops: notify는 없고 연속성 판정만 등록 전용 콜백 사용. */
	const struct spdk_mem_map_ops reg_map_ops = {
		.notify_cb = NULL,
		.are_contiguous = mem_reg_map_check_contiguous,
	};
	/* [한국어] 단계별 결과 코드. */
	int rc;

	/* [한국어] legacy 모드 전역 플래그 저장. */
	g_legacy_mem = legacy_mem;

	/* [한국어] 마스터 등록 맵 생성(default=0). */
	g_mem_reg_map = spdk_mem_map_alloc(0, &reg_map_ops, NULL);
	if (g_mem_reg_map == NULL) {
		DEBUG_PRINT("memory registration map allocation failed\n");
		return -ENOMEM;
	}

	/* [한국어] legacy가 아니면 동적 hugepage 이벤트 콜백 등록. */
	if (!g_legacy_mem) {
		/**
		 * To prevent DPDK complaining, only register the callback when
		 * we are not in legacy mem mode.
		 */
		/* [한국어] legacy 모드에서는 DPDK가 이벤트를 안 내므로 등록하지 않는다. */
		rc = mem_map_mem_event_callback_register();
		if (rc != 0) {
			DEBUG_PRINT("memory event callback registration failed, rc = %d\n", rc);
			goto err_free_reg_map;
		}
	}

	/*
	 * Walk all DPDK memory segments and register them
	 * with the main memory map
	 */
	/* [한국어] 콜백 등록 이전에 이미 존재하던 모든 hugepage 세그먼트를 등록. */
	rc = rte_memseg_contig_walk(memory_iter_cb, NULL);
	if (rc != 0) {
		DEBUG_PRINT("memory segments walking failed, rc = %d\n", rc);
		goto err_unregister_mem_cb;
	}

	/* [한국어] 초기화 성공. */
	return 0;

err_unregister_mem_cb:
	/* [한국어] 세그먼트 walk 실패 시 등록했던 콜백 해제. */
	mem_map_mem_event_callback_unregister();
err_free_reg_map:
	/* [한국어] 마스터 등록 맵 해제 후 실패 코드 반환. */
	spdk_mem_map_free(&g_mem_reg_map);
	return rc;
}

/*
 * [한국어]
 * mem_map_fini - 메모리 등록 인프라 정리(콜백 해제 + 마스터 맵 해제)
 *
 * env 종료 시 호출. hotplug 콜백을 먼저 떼어 더 이상 트리가 변경되지 않게 한 뒤 맵을 해제.
 * 호출 체인: env 종료 → mem_map_fini
 */
void
mem_map_fini(void)
{
	/* [한국어] DPDK 메모리 이벤트 콜백 해제. */
	mem_map_mem_event_callback_unregister();
	/* [한국어] 마스터 등록 맵 해제. */
	spdk_mem_map_free(&g_mem_reg_map);
}

/*
 * [한국어]
 * spdk_iommu_is_enabled - 시스템에서 IOMMU(VFIO)가 활성 상태인지 질의 (외부 API)
 *
 * @return: VFIO가 켜져 있고 no-IOMMU 모드가 아니면 true, 아니면 false.
 *
 * VFIO 미지원 빌드에서는 항상 false. NVMe/RDMA 드라이버가 IOVA 전략(VA 직접 사용 vs 물리주소)을
 * 결정할 때 참조한다. 호출 체인: pci_device_vtophys / vtophys_notify 등 → spdk_iommu_is_enabled
 */
bool
spdk_iommu_is_enabled(void)
{
#if VFIO_ENABLED
	/* [한국어] enabled이면서 no-IOMMU가 아닐 때만 진짜 IOMMU가 동작. */
	return g_vfio.enabled && !g_vfio.noiommu_enabled;
#else
	/* [한국어] VFIO 미지원 빌드. */
	return false;
#endif
}

/*
 * [한국어]
 * struct spdk_vtophys_pci_device - vtophys가 BAR 변환에 참조할 PCI 디바이스 트래킹 노드
 *
 * SPDK가 attach한 PCI 디바이스를 큐에 모아, vtophys 요청이 hugepage가 아닌 BAR 주소일 때
 * 각 디바이스의 BAR 리소스 범위를 검색하는 데 쓴다.
 */
struct spdk_vtophys_pci_device {
	struct rte_pci_device *pci_device;
	/* [한국어] DPDK PCI 디바이스 핸들(BAR phys_addr/addr/len 조회용).
	 * 설정자: vtophys_pci_device_added. 읽는 자: vtophys_get_paddr_pci.
	 * 값 범위: 유효한 rte_pci_device 포인터. 동기화: g_vtophys_pci_devices_mutex. */
	TAILQ_ENTRY(spdk_vtophys_pci_device) tailq;
	/* [한국어] g_vtophys_pci_devices 큐 링크.
	 * 설정자/읽는 자: add/removed/get_paddr_pci. 동기화: g_vtophys_pci_devices_mutex. */
};

/* [한국어] g_vtophys_pci_devices 큐 보호 뮤텍스. */
static pthread_mutex_t g_vtophys_pci_devices_mutex = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] SPDK가 attach한 PCI 디바이스 큐(BAR vtophys 검색 대상). */
static TAILQ_HEAD(, spdk_vtophys_pci_device) g_vtophys_pci_devices =
	TAILQ_HEAD_INITIALIZER(g_vtophys_pci_devices);

/* [한국어] 가상주소 → IOVA(또는 물리주소) 변환 맵. NVMe PRP 생성의 핵심. no-pci면 NULL. */
static struct spdk_mem_map *g_vtophys_map;
/* [한국어] IOVA별 DMA 매핑 refcount 맵. 같은 IOVA에 대한 중복 map/unmap을 참조계수로 관리. */
static struct spdk_mem_map *g_phys_ref_map;
/* [한국어] 가상주소 → NUMA socket id 맵. hugepage일 때만 생성. */
static struct spdk_mem_map *g_numa_map;

#if VFIO_ENABLED
/*
 * [한국어]
 * _vfio_iommu_map_dma - VFIO container에 단일 DMA 매핑을 추가(ioctl)하고 큐에 기록 (락 없는 내부)
 *
 * @vaddr: 매핑할 호스트 가상주소.
 * @iova:  디바이스가 보게 될 IOVA.
 * @size:  매핑 길이.
 * @return: 0 성공(또는 디바이스 미연결로 보류), -ENOMEM.
 *
 * spdk_vfio_dma_map 노드를 만들어 VFIO ioctl 인자를 채운다. 아직 container에 디바이스가
 * 하나도 없으면(device_ref==0) 실제 ioctl을 미루고 노드만 큐에 넣어, 첫 디바이스 hotplug
 * 시점에 일괄 매핑한다(VFIO는 IOMMU 그룹 1개 이상이 있어야 매핑 가능). 호출자는 g_vfio.mutex 보유.
 *
 * 호출 체인: vtophys_iommu_map_dma / vtophys_iommu_map_dma_bar → _vfio_iommu_map_dma → ioctl(VFIO_IOMMU_MAP_DMA)
 */
static int
_vfio_iommu_map_dma(uint64_t vaddr, uint64_t iova, uint64_t size)
{
	/* [한국어] 새 매핑 트래킹 노드. */
	struct spdk_vfio_dma_map *dma_map;
	/* [한국어] ioctl 반환값. */
	int ret;

	/* [한국어] 매핑 노드 할당(나중 unmap을 위해 인자 보존). */
	dma_map = calloc(1, sizeof(*dma_map));
	if (dma_map == NULL) {
		return -ENOMEM;
	}

	/* [한국어] VFIO ABI: argsz=구조체 크기(버전 협상). */
	dma_map->map.argsz = sizeof(dma_map->map);
	/* [한국어] 읽기/쓰기 모두 허용하는 DMA 매핑. */
	dma_map->map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	/* [한국어] 호스트 가상주소. */
	dma_map->map.vaddr = vaddr;
	/* [한국어] 디바이스가 사용할 IOVA. */
	dma_map->map.iova = iova;
	/* [한국어] 매핑 크기. */
	dma_map->map.size = size;

	/* [한국어] container에 디바이스가 아직 없으면 ioctl을 미루고 노드만 기록. */
	if (g_vfio.device_ref == 0) {
		/* VFIO requires at least one device (IOMMU group) to be added to
		 * a VFIO container before it is possible to perform any IOMMU
		 * operations on that container. This memory will be mapped once
		 * the first device (IOMMU group) is hotplugged.
		 *
		 * Since the vfio container is managed internally by DPDK, it is
		 * also possible that some device is already in that container, but
		 * it's not managed by SPDK -  e.g. an NIC attached internally
		 * inside DPDK. We could map the memory straight away in such
		 * scenario, but there's no need to do it. DPDK devices clearly
		 * don't need our mappings and hence we defer the mapping
		 * unconditionally until the first SPDK-managed device is
		 * hotplugged.
		 */
		/* [한국어] 첫 SPDK 디바이스 hotplug 시 vtophys_pci_device_added가 일괄 매핑한다. */
		goto out_insert;
	}

	/* [한국어] 커널 VFIO IOMMU에 DMA 매핑 요청(IOVA↔vaddr 페이지 테이블 설정). */
	ret = ioctl(g_vfio.fd, VFIO_IOMMU_MAP_DMA, &dma_map->map);
	if (ret) {
		/* There are cases the vfio container doesn't have IOMMU group, it's safe for this case */
		/* [한국어] IOMMU 그룹이 없는 경우 등은 무시 가능 — 노티스만 남김. */
		SPDK_NOTICELOG("Cannot set up DMA mapping, error %d, ignored\n", errno);
	}

out_insert:
	/* [한국어] 추후 unmap을 위해 매핑 노드를 큐에 보존. */
	TAILQ_INSERT_TAIL(&g_vfio.maps, dma_map, tailq);
	return 0;
}


/*
 * [한국어]
 * vtophys_iommu_map_dma - IOVA 참조계수를 관리하며 DMA 매핑을 추가 (iova=pa 경로)
 *
 * @vaddr/@iova/@size: 매핑 파라미터.
 * @return: 0 성공, 실패 시 음수.
 *
 * 같은 IOVA가 이미 매핑되어 있으면(refcount>0) 실제 ioctl 없이 참조계수만 +1한다.
 * 처음 매핑이면 g_vfio.mutex를 잡고 _vfio_iommu_map_dma로 ioctl 후 refcount를 1로 설정.
 * 여러 가상 영역이 동일 물리주소를 가리킬 수 있어 중복 매핑/해제를 막는다.
 *
 * 호출 체인: vtophys_map_pagemap / vtophys_notify → vtophys_iommu_map_dma
 */
static int
vtophys_iommu_map_dma(uint64_t vaddr, uint64_t iova, uint64_t size)
{
	/* [한국어] 이 IOVA의 현재 참조계수. */
	uint64_t refcount;
	/* [한국어] 매핑 결과. */
	int ret;

	/* [한국어] phys_ref 맵에서 IOVA의 현재 참조계수 조회. */
	refcount = spdk_mem_map_translate(g_phys_ref_map, iova, NULL);
	assert(refcount < UINT64_MAX);
	/* [한국어] 이미 매핑되어 있으면 ioctl 없이 참조계수만 증가. */
	if (refcount > 0) {
		spdk_mem_map_set_translation(g_phys_ref_map, iova, size, refcount + 1);
		return 0;
	}

	/* [한국어] 첫 매핑 → VFIO 매핑 큐/ioctl 보호 락. */
	pthread_mutex_lock(&g_vfio.mutex);
	ret = _vfio_iommu_map_dma(vaddr, iova, size);
	pthread_mutex_unlock(&g_vfio.mutex);
	if (ret) {
		return ret;
	}

	/* [한국어] 매핑 성공 → 참조계수 1로 설정. */
	spdk_mem_map_set_translation(g_phys_ref_map, iova, size, refcount + 1);
	return 0;
}

/*
 * [한국어]
 * vtophys_iommu_map_dma_bar - BAR 영역을 IOMMU에 DMA 매핑 (참조계수 없이)
 *
 * @vaddr/@iova/@size: 매핑 파라미터.
 * @return: _vfio_iommu_map_dma 결과.
 *
 * P2P/BAR 매핑은 일반 메모리와 달리 참조계수 공유 대상이 아니므로 곧장 ioctl한다.
 * 호출 체인: lib/nvme CMB/P2P 등록 등 → vtophys_iommu_map_dma_bar → _vfio_iommu_map_dma
 */
int
vtophys_iommu_map_dma_bar(uint64_t vaddr, uint64_t iova, uint64_t size)
{
	/* [한국어] 매핑 결과. */
	int ret;

	/* [한국어] VFIO 매핑 큐/ioctl 보호. */
	pthread_mutex_lock(&g_vfio.mutex);
	ret = _vfio_iommu_map_dma(vaddr, iova, size);
	pthread_mutex_unlock(&g_vfio.mutex);

	return ret;
}

/*
 * [한국어]
 * _vfio_iommu_unmap_dma - VFIO container에서 단일 DMA 매핑 제거(ioctl) 후 노드 해제 (락 없는 내부)
 *
 * @dma_map: 제거할 매핑 노드.
 * @return:  항상 0.
 *
 * device_ref==0이면(디바이스 detach로 커널이 이미 매핑을 떼어냄) ioctl 없이 노드만 제거한다.
 * 그렇지 않으면 VFIO_IOMMU_UNMAP_DMA ioctl로 IOMMU 페이지 테이블에서 매핑을 제거한다.
 * 호출자는 g_vfio.mutex 보유. 호출 체인: vtophys_iommu_unmap_dma(_bar) → _vfio_iommu_unmap_dma → ioctl
 */
static int
_vfio_iommu_unmap_dma(struct spdk_vfio_dma_map *dma_map)
{
	/* [한국어] VFIO unmap ioctl 인자(0초기화). */
	struct vfio_iommu_type1_dma_unmap unmap = {};
	/* [한국어] ioctl 반환값. */
	int ret;

	/* [한국어] 디바이스가 없으면 커널 매핑도 이미 없음 → 참조만 제거. */
	if (g_vfio.device_ref == 0) {
		/* Memory is not mapped anymore, just remove it's references */
		goto out_remove;
	}

	/* [한국어] ABI 버전(argsz)/플래그/대상 IOVA/크기 설정. */
	unmap.argsz = sizeof(unmap);
	unmap.flags = 0;
	unmap.iova = dma_map->map.iova;
	unmap.size = dma_map->map.size;
	/* [한국어] 커널 IOMMU에서 매핑 해제 요청. */
	ret = ioctl(g_vfio.fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
	if (ret) {
		/* [한국어] 해제 실패는 노티스만(이미 사라진 매핑일 수 있음). */
		SPDK_NOTICELOG("Cannot clear DMA mapping, error %d, ignored\n", errno);
	}

out_remove:
	/* [한국어] 큐에서 노드 제거 후 해제. */
	TAILQ_REMOVE(&g_vfio.maps, dma_map, tailq);
	free(dma_map);
	return 0;
}

/*
 * [한국어]
 * vtophys_iommu_unmap_dma - IOVA 참조계수를 줄이며 DMA 매핑 해제 (iova=pa 경로)
 *
 * @iova: 해제 대상 IOVA.
 * @size: 매핑 크기.
 * @return: 0 성공, 매핑을 못 찾으면 -ENXIO.
 *
 * IOVA로 매핑 노드를 찾고 참조계수를 1 감소시킨다. 남은 참조가 있으면(refcount>1) 실제
 * 해제를 미룬다. 마지막 참조일 때만 커널 unmap을 수행한다. vtophys_iommu_map_dma의 짝.
 * 호출 체인: vtophys_unmap_iommu_paddr / vtophys_notify → vtophys_iommu_unmap_dma
 */
static int
vtophys_iommu_unmap_dma(uint64_t iova, uint64_t size)
{
	/* [한국어] 검색된 매핑 노드. */
	struct spdk_vfio_dma_map *dma_map;
	/* [한국어] 현재 참조계수. */
	uint64_t refcount;
	/* [한국어] 해제 결과. */
	int ret;

	/* [한국어] 매핑 큐 보호 락. */
	pthread_mutex_lock(&g_vfio.mutex);
	/* [한국어] IOVA가 일치하는 매핑 노드 선형 검색. */
	TAILQ_FOREACH(dma_map, &g_vfio.maps, tailq) {
		if (dma_map->map.iova == iova) {
			break;
		}
	}

	/* [한국어] 못 찾으면 매핑되지 않은 IOVA → -ENXIO. */
	if (dma_map == NULL) {
		DEBUG_PRINT("Cannot clear DMA mapping for IOVA %"PRIx64" - it's not mapped\n", iova);
		pthread_mutex_unlock(&g_vfio.mutex);
		return -ENXIO;
	}

	/* [한국어] 참조계수 조회 후 1 감소. */
	refcount = spdk_mem_map_translate(g_phys_ref_map, iova, NULL);
	assert(refcount < UINT64_MAX);
	if (refcount > 0) {
		spdk_mem_map_set_translation(g_phys_ref_map, iova, size, refcount - 1);
	}

	/* We still have outstanding references, don't clear it. */
	/* [한국어] 아직 다른 참조가 남았으면 실제 해제 보류. */
	if (refcount > 1) {
		pthread_mutex_unlock(&g_vfio.mutex);
		return 0;
	}

	/** don't support partial or multiple-page unmap for now */
	/* [한국어] 부분/다중 페이지 해제는 미지원 — 등록 시와 동일 크기여야 함. */
	assert(dma_map->map.size == size);

	/* [한국어] 마지막 참조 → 실제 커널 unmap 수행. */
	ret = _vfio_iommu_unmap_dma(dma_map);
	pthread_mutex_unlock(&g_vfio.mutex);

	return ret;
}

/*
 * [한국어]
 * vtophys_iommu_unmap_dma_bar - BAR DMA 매핑을 가상주소로 찾아 해제 (참조계수 없이)
 *
 * @vaddr: 해제 대상 호스트 가상주소.
 * @return: 0 성공, 못 찾으면 -ENXIO.
 *
 * vtophys_iommu_map_dma_bar의 짝. IOVA가 아니라 vaddr로 노드를 찾는다(BAR는 IOVA==VA가 아닐 수 있음).
 * 호출 체인: lib/nvme CMB/P2P 해제 → vtophys_iommu_unmap_dma_bar → _vfio_iommu_unmap_dma
 */
int
vtophys_iommu_unmap_dma_bar(uint64_t vaddr)
{
	/* [한국어] 검색된 매핑 노드. */
	struct spdk_vfio_dma_map *dma_map;
	/* [한국어] 해제 결과. */
	int ret;

	/* [한국어] 매핑 큐 보호 락. */
	pthread_mutex_lock(&g_vfio.mutex);
	/* [한국어] vaddr이 일치하는 매핑 노드 검색. */
	TAILQ_FOREACH(dma_map, &g_vfio.maps, tailq) {
		if (dma_map->map.vaddr == vaddr) {
			break;
		}
	}

	/* [한국어] 못 찾으면 -ENXIO. */
	if (dma_map == NULL) {
		DEBUG_PRINT("Cannot clear DMA mapping for address %"PRIx64" - it's not mapped\n", vaddr);
		pthread_mutex_unlock(&g_vfio.mutex);
		return -ENXIO;
	}

	/* [한국어] 매핑 제거. */
	ret = _vfio_iommu_unmap_dma(dma_map);
	pthread_mutex_unlock(&g_vfio.mutex);
	return ret;
}
#endif

/*
 * [한국어]
 * vtophys_get_paddr_memseg - DPDK 메모리 세그먼트에서 가상주소의 물리주소(IOVA) 조회
 *
 * @vaddr: 변환할 가상주소.
 * @len:   출력(NULL 허용). 세그먼트 끝까지 남은 길이.
 * @return: IOVA 또는 SPDK_VTOPHYS_ERROR.
 *
 * rte_mem_virt2memseg로 세그먼트를 찾아 그 시작 IOVA에 vaddr 오프셋을 더한다. hugepage로
 * 관리되는 주소의 가장 빠른 변환 경로. 호출 체인: vtophys_map_memseg / vtophys_notify → vtophys_get_paddr_memseg
 */
static uint64_t
vtophys_get_paddr_memseg(uint64_t vaddr, size_t *len)
{
	/* [한국어] paddr=세그먼트 IOVA, offset=세그먼트 내 오프셋. */
	uintptr_t paddr, offset;
	/* [한국어] DPDK 메모리 세그먼트. */
	struct rte_memseg *seg;

	/* [한국어] 가상주소를 포함하는 세그먼트 조회. */
	seg = rte_mem_virt2memseg((void *)(uintptr_t)vaddr, NULL);
	if (seg != NULL) {
		/* [한국어] 세그먼트의 시작 IOVA. */
		paddr = seg->iova;
		/* [한국어] IOVA가 유효하지 않으면(pagemap 미할당 등) 에러. */
		if (paddr == RTE_BAD_IOVA) {
			return SPDK_VTOPHYS_ERROR;
		}
		/* [한국어] 세그먼트 시작으로부터 vaddr 오프셋. */
		offset = vaddr - (uintptr_t)seg->addr;
		if (len != NULL) {
			/* [한국어] 세그먼트 끝까지 남은 길이 반환. */
			assert(seg->len > offset);
			*len = seg->len - offset;
		}
		/* [한국어] 시작 IOVA + 오프셋 = 정확한 물리주소. */
		paddr += offset;
		return paddr;
	}

	/* [한국어] DPDK가 관리하지 않는 주소. */
	return SPDK_VTOPHYS_ERROR;
}

/* Try to get the paddr from /proc/self/pagemap */
/*
 * [한국어]
 * vtophys_get_paddr_pagemap - /proc/self/pagemap을 통해 물리주소 조회(iova=pa 폴백)
 *
 * @vaddr: 변환할 가상주소(페이지 정렬).
 * @return: 물리주소 또는 SPDK_VTOPHYS_ERROR.
 *
 * DPDK 세그먼트가 아닌 사용자 mmap 영역의 물리주소를 rte_mem_virt2iova(pagemap 기반)로 얻는다.
 * 백킹 페이지가 아직 없어 실패하면 페이지를 한 번 touch(원자 read)해 fault를 유발한 뒤 재시도한다.
 * 호출 체인: vtophys_map_pagemap → vtophys_get_paddr_pagemap
 */
static uint64_t
vtophys_get_paddr_pagemap(uint64_t vaddr)
{
	/* [한국어] 변환된 물리주소. */
	uintptr_t paddr;

	/* Silence static analyzers */
	/* [한국어] vaddr==0은 정상 변환 대상이 아님(정적 분석기 경고 억제 겸). */
	assert(vaddr != 0);
	/* [한국어] pagemap 기반 물리주소 조회. */
	paddr = rte_mem_virt2iova((void *)vaddr);
	if (paddr == RTE_BAD_IOVA) {
		/*
		 * The vaddr may be valid but doesn't have a backing page
		 * assigned yet.  Touch the page to ensure a backing page
		 * gets assigned, then try to translate again.
		 */
		/* [한국어] 백킹 페이지 미할당일 수 있어 원자 read로 page fault 유발 후 재시도. */
		rte_atomic64_read((rte_atomic64_t *)vaddr);
		paddr = rte_mem_virt2iova((void *)vaddr);
	}
	/* [한국어] 재시도도 실패하면 변환 불가. */
	if (paddr == RTE_BAD_IOVA) {
		/* Unable to get to the physical address. */
		return SPDK_VTOPHYS_ERROR;
	}

	return paddr;
}

/*
 * [한국어]
 * pci_device_vtophys - 한 PCI 디바이스의 BAR 리소스에서 가상주소→물리주소 변환
 *
 * @dev:   PCI 디바이스.
 * @vaddr: 변환할 가상주소(BAR 매핑 영역으로 추정).
 * @len:   변환할 길이(BAR 범위 안에 완전히 들어가야 함).
 * @return: 물리주소(또는 IOMMU+VA 모드면 vaddr 그대로), 못 찾으면 SPDK_VTOPHYS_ERROR.
 *
 * 디바이스의 모든 BAR 리소스를 순회하며 vaddr이 어느 BAR 범위에 들어가는지 확인한다.
 * IOMMU가 켜져 있고 IOVA==VA면 BAR가 매핑 시 자동 등록되므로 vaddr을 그대로 반환한다.
 * 호출 체인: vtophys_get_paddr_pci → pci_device_vtophys
 */
static uint64_t
pci_device_vtophys(struct rte_pci_device *dev, uint64_t vaddr, size_t len)
{
	/* [한국어] BAR 메모리 리소스 디스크립터. */
	struct rte_mem_resource *res;
	/* [한국어] 계산된 물리주소. */
	uint64_t paddr;
	/* [한국어] BAR 인덱스. */
	unsigned r;

	/* [한국어] 디바이스의 모든 BAR(리소스) 순회. */
	for (r = 0; r < PCI_MAX_RESOURCE; r++) {
		/* [한국어] r번 BAR의 메모리 리소스 정보 조회. */
		res = dpdk_pci_device_get_mem_resource(dev, r);

		/* [한국어] 물리주소가 없거나 vaddr/len이 이 BAR 범위에 완전히 들어가지 않으면 스킵. */
		if (res->phys_addr == 0 || vaddr < (uint64_t)res->addr ||
		    (vaddr + len) >= (uint64_t)res->addr + res->len) {
			continue;
		}

#if VFIO_ENABLED
		/* [한국어] IOMMU+IOVA==VA 모드면 BAR가 매핑 시 자동 등록됨 → vaddr 그대로 사용. */
		if (spdk_iommu_is_enabled() && rte_eal_iova_mode() == RTE_IOVA_VA) {
			/*
			 * The IOMMU is on and we're using IOVA == VA. The BAR was
			 * automatically registered when it was mapped, so just return
			 * the virtual address here.
			 */
			return vaddr;
		}
#endif
		/* [한국어] BAR 물리 시작주소 + (vaddr - BAR 가상 시작주소) = 물리주소. */
		paddr = res->phys_addr + (vaddr - (uint64_t)res->addr);
		return paddr;
	}

	/* [한국어] 이 디바이스의 어느 BAR에도 속하지 않음. */
	return SPDK_VTOPHYS_ERROR;
}

/* Try to get the paddr from pci devices */
/*
 * [한국어]
 * vtophys_get_paddr_pci - 등록된 모든 PCI 디바이스에서 BAR 물리주소 검색
 *
 * @vaddr: 변환할 가상주소.
 * @len:   길이.
 * @return: 물리주소, 어느 디바이스 BAR에도 없으면 SPDK_VTOPHYS_ERROR.
 *
 * g_vtophys_pci_devices 큐를 순회하며 각 디바이스에 pci_device_vtophys를 시도한다.
 * 호출 체인: vtophys_map_pci / vtophys_notify → vtophys_get_paddr_pci → pci_device_vtophys
 */
static uint64_t
vtophys_get_paddr_pci(uint64_t vaddr, size_t len)
{
	/* [한국어] 디바이스 트래킹 노드 순회 임시. */
	struct spdk_vtophys_pci_device *vtophys_dev;
	/* [한국어] 변환된 물리주소. */
	uintptr_t paddr;
	/* [한국어] 현재 디바이스 핸들. */
	struct rte_pci_device	*dev;

	/* [한국어] 디바이스 큐 보호 락. */
	pthread_mutex_lock(&g_vtophys_pci_devices_mutex);
	/* [한국어] 등록된 디바이스를 차례로 시도. */
	TAILQ_FOREACH(vtophys_dev, &g_vtophys_pci_devices, tailq) {
		dev = vtophys_dev->pci_device;
		/* [한국어] 이 디바이스 BAR에서 변환 시도. */
		paddr = pci_device_vtophys(dev, vaddr, len);
		if (paddr != SPDK_VTOPHYS_ERROR) {
			/* [한국어] 찾으면 락 해제 후 반환. */
			pthread_mutex_unlock(&g_vtophys_pci_devices_mutex);
			return paddr;
		}
	}
	pthread_mutex_unlock(&g_vtophys_pci_devices_mutex);

	/* [한국어] 어느 디바이스에도 없음. */
	return SPDK_VTOPHYS_ERROR;
}

#if VFIO_ENABLED
/*
 * [한국어]
 * vtophys_unmap_pci - BAR 영역 vtophys 매핑을 제거하는 walk 콜백
 *
 * @map/@vaddr/@len/@ctx: walk 표준 파라미터.
 * @return: 0 성공, BAR 변환 실패 시 -EFAULT.
 *
 * 먼저 vaddr이 PCI BAR인지 확인(아니면 -EFAULT)한 뒤 vtophys 트리에서 변환을 클리어한다.
 * 호출 체인: vtophys_notify(UNREGISTER) → vtophys_walk_region → vtophys_unmap_pci
 */
static int
vtophys_unmap_pci(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	/* [한국어] 검증용 물리주소. */
	uint64_t paddr;

	/* [한국어] 이 주소가 정말 PCI BAR인지 확인. */
	paddr = vtophys_get_paddr_pci((uint64_t)vaddr, len);
	if (paddr == SPDK_VTOPHYS_ERROR) {
		DEBUG_PRINT("could not get phys addr for 0x%" PRIx64 "\n", vaddr);
		return -EFAULT;
	}

	/* [한국어] vtophys 트리에서 이 영역 변환 제거. */
	return spdk_mem_map_clear_translation(map, vaddr, len);
}

/*
 * [한국어]
 * vtophys_unmap_iommu_paddr - iova=pa 모드에서 페이지별로 IOMMU 매핑 해제하는 walk 콜백
 *
 * @map/@vaddr/@len/@ctx: walk 표준 파라미터.
 * @return: 0 성공, 변환/해제 실패 시 -EFAULT.
 *
 * 현재 페이지의 물리주소를 spdk_vtophys로 얻어 vtophys_iommu_unmap_dma로 IOMMU에서 떼어낸다.
 * iova=pa에서는 영역이 물리적으로 비연속일 수 있어 페이지 단위로 해제한다.
 * 호출 체인: vtophys_notify(UNREGISTER, iova=pa) → vtophys_walk_region → vtophys_unmap_iommu_paddr
 */
static int
vtophys_unmap_iommu_paddr(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	/* [한국어] 이 페이지의 물리주소. */
	uint64_t paddr;
	/* [한국어] 해제 결과. */
	int rc;

	/* [한국어] vtophys로 물리주소 조회. */
	paddr = spdk_vtophys((void *)vaddr, NULL);
	if (paddr == SPDK_VTOPHYS_ERROR) {
		DEBUG_PRINT("could not get phys addr for 0x%" PRIx64 "\n", vaddr);
		return -EFAULT;
	}

	/* [한국어] 해당 물리주소(IOVA)의 IOMMU 매핑 해제. */
	rc = vtophys_iommu_unmap_dma(paddr, len);
	if (rc) {
		DEBUG_PRINT("Failed to iommu unmap paddr 0x%" PRIx64 "\n", paddr);
		return -EFAULT;
	}

	return 0;
}
#endif

/*
 * [한국어]
 * vtophys_unmap_page - vtophys 트리에서 한 페이지 변환을 제거하는 walk 콜백
 *
 * @map/@vaddr/@len/@ctx: walk 표준 파라미터.
 * @return: spdk_mem_map_clear_translation 결과.
 *
 * 호출 체인: vtophys_notify(UNREGISTER) → vtophys_walk_region → vtophys_unmap_page
 */
static int
vtophys_unmap_page(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	/* [한국어] vtophys 매핑을 default(ERROR)로 되돌려 제거. */
	return spdk_mem_map_clear_translation(map, vaddr, len);
}

/*
 * [한국어]
 * vtophys_set_translation - vtophys 트리에 변환 값을 쓰되 4KB 매핑이면 플래그 비트 설정
 *
 * @map/@vaddr/@len: 매핑 대상.
 * @paddr: 물리주소(IOVA).
 * @return: spdk_mem_map_set_translation 결과.
 *
 * len이 4KB면 VTOPHYS_4KB 비트(63)를 paddr에 OR해 "이 항목은 4KB 매핑"임을 표시한다.
 * spdk_vtophys 읽기 측에서 이 비트로 페이지 크기(마스크)를 구분한다.
 * 호출 체인: vtophys_map_* → vtophys_set_translation → spdk_mem_map_set_translation
 */
static int
vtophys_set_translation(struct spdk_mem_map *map, uint64_t vaddr, size_t len, uint64_t paddr)
{
	/* [한국어] 4KB 매핑이면 상위 플래그 비트를 켠다(물리주소 상위 비트는 0이라고 가정). */
	if (len == VALUE_4KB) {
		assert(!(paddr & VTOPHYS_4KB));
		paddr |= VTOPHYS_4KB;
	}

	/* [한국어] (플래그 포함) 변환 값 기록. */
	return spdk_mem_map_set_translation(map, vaddr, len, paddr);
}

/*
 * [한국어]
 * vtophys_map_pci - BAR 영역의 물리주소를 vtophys 트리에 등록하는 walk 콜백
 *
 * @map/@vaddr/@len/@ctx: walk 표준 파라미터.
 * @return: 0 성공, BAR 변환 실패 시 -EFAULT.
 *
 * 호출 체인: vtophys_notify(REGISTER, BAR) → vtophys_walk_region → vtophys_map_pci
 */
static int
vtophys_map_pci(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	/* [한국어] BAR 물리주소. */
	uint64_t paddr;

	/* [한국어] PCI BAR에서 물리주소 조회. */
	paddr = vtophys_get_paddr_pci(vaddr, len);
	if (paddr == SPDK_VTOPHYS_ERROR) {
		DEBUG_PRINT("could not get phys addr for 0x%"PRIx64"\n", vaddr);
		return -EFAULT;
	}

	/* [한국어] vtophys 트리에 등록. */
	return vtophys_set_translation(map, vaddr, len, paddr);
}

#if VFIO_ENABLED
/*
 * [한국어]
 * vtophys_map_vaddr - IOVA==VA 모드에서 vaddr 자체를 IOVA로 등록하는 walk 콜백
 *
 * @map/@vaddr/@len/@ctx: walk 표준 파라미터.
 * @return: vtophys_set_translation 결과.
 *
 * IOMMU+IOVA==VA에서는 IOVA가 가상주소와 동일하므로 변환 값으로 vaddr을 그대로 쓴다.
 * 호출 체인: vtophys_notify(REGISTER, IOMMU+VA) → vtophys_walk_region → vtophys_map_vaddr
 */
static int
vtophys_map_vaddr(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	/* [한국어] IOVA==VA: 변환 값 = vaddr. */
	return vtophys_set_translation(map, vaddr, len, vaddr);
}
#endif

/*
 * [한국어]
 * vtophys_map_pagemap - pagemap 물리주소를 vtophys 트리에 등록하는 walk 콜백(iova=pa)
 *
 * @map/@vaddr/@len/@ctx: walk 표준 파라미터.
 * @return: 0 성공, 4KB 페이지/비정렬/변환 실패 시 -EINVAL/-EFAULT.
 *
 * iova=pa 모드에서는 4KB 페이지가 swap되거나 zero page일 위험이 있어 hugepage(2MB 이상)만
 * 신뢰한다. 물리주소를 얻어 2MB 정렬을 확인하고, IOMMU가 켜져 있으면 물리주소로 IOMMU 매핑까지 한다.
 * 호출 체인: vtophys_notify(REGISTER, iova=pa) → vtophys_walk_region → vtophys_map_pagemap
 */
static int
vtophys_map_pagemap(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	/* [한국어] pagemap 물리주소. */
	uint64_t paddr;

	/* In iova=pa mode we can only reliably map hugepages, because we cannot guarantee that a
	 * 4KB page is pinned and isn't swapped or doesn't point to a zero page (which is likely if
	 * the memory was just mmap()-ed and hasn't been written yet).  To be totally safe, we'd
	 * have to check /proc/kpageflags, but checking the length and paddr's alignment should be
	 * enough to catch most cases.
	 */
	/* [한국어] iova=pa에서는 4KB 페이지를 신뢰할 수 없어(swap/zero page) hugepage만 허용. */
	if (len < VALUE_2MB) {
		DEBUG_PRINT("page size 4KB is unsupported in iova=pa mode\n");
		return -EINVAL;
	}

	/* [한국어] pagemap에서 물리주소 조회. */
	paddr = vtophys_get_paddr_pagemap(vaddr);
	if (paddr == SPDK_VTOPHYS_ERROR) {
		DEBUG_PRINT("could not get phys addr for 0x%"PRIx64"\n", vaddr);
		return -EFAULT;
	}

	/* [한국어] hugepage 물리주소는 2MB 정렬이어야 함(아니면 비정상). */
	if (paddr & MASK_2MB) {
		DEBUG_PRINT("invalid paddr 0x%" PRIx64 " - must be 2MB aligned\n", paddr);
		return -EINVAL;
	}
#if VFIO_ENABLED
	/* If the IOMMU is on, but DPDK is using iova-mode=pa, we want to register this memory
	 * with the IOMMU using the physical address to match. */
	/* [한국어] IOMMU+iova=pa면 물리주소를 IOVA로 사용해 IOMMU 매핑도 등록(IOVA==물리주소 일치). */
	if (spdk_iommu_is_enabled()) {
		int rc = vtophys_iommu_map_dma(vaddr, paddr, len);
		if (rc) {
			DEBUG_PRINT("Unable to assign vaddr 0x%" PRIx64" to paddr 0x%" PRIx64 "\n",
				    vaddr, paddr);
			return -EFAULT;
		}
	}
#endif
	/* [한국어] vtophys 트리에 물리주소 등록. */
	return vtophys_set_translation(map, vaddr, len, paddr);
}

/*
 * [한국어]
 * vtophys_map_memseg - DPDK 세그먼트 물리주소를 vtophys 트리에 등록하는 walk 콜백
 *
 * @map/@vaddr/@len/@ctx: walk 표준 파라미터.
 * @return: 0 성공, 변환 실패/세그먼트 불연속 시 -EFAULT.
 *
 * DPDK가 관리하는 hugepage 영역의 물리주소를 세그먼트에서 얻어 등록한다. iova=pa면 한
 * 세그먼트 안에 요청 길이가 다 들어가는지(seglen>=len) 확인해 물리 연속성을 보장한다.
 * 호출 체인: vtophys_notify(REGISTER, DPDK 관리) → vtophys_walk_region → vtophys_map_memseg
 */
static int
vtophys_map_memseg(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	/* [한국어] 세그먼트 물리주소. */
	uint64_t paddr;
	/* [한국어] 세그먼트 끝까지 남은 길이. */
	size_t seglen;

	/* [한국어] 세그먼트에서 물리주소와 잔여 길이 조회. */
	paddr = vtophys_get_paddr_memseg(vaddr, &seglen);
	if (paddr == SPDK_VTOPHYS_ERROR) {
		DEBUG_PRINT("could not get phys addr for 0x%"PRIx64"\n", vaddr);
		return -EFAULT;
	}

	/* [한국어] iova=pa인데 요청 길이가 세그먼트 경계를 넘으면 물리 불연속 → 거부. */
	if (rte_eal_iova_mode() == RTE_IOVA_PA && seglen < len) {
		DEBUG_PRINT("unexpected paddr=0x%"PRIx64" len=%zu for vaddr=0x%"PRIx64", "
			    "wanted=%zu\n", paddr, seglen, vaddr, len);
		return -EFAULT;
	}

	/* [한국어] vtophys 트리에 등록. */
	return vtophys_set_translation(map, vaddr, len, paddr);
}

/*
 * [한국어]
 * vtophys_walk_region - vtophys 콜백 시그니처에 맞춘 mem_map_walk_region 래퍼
 *
 * @map/@vaddr/@len: 순회 대상.
 * @map_page:        각 페이지에 적용할 vtophys map/unmap 콜백.
 * @return:          mem_map_walk_region 결과.
 *
 * vtophys_notify가 ctx 없이 페이지별 콜백을 적용할 때 사용하는 간단한 어댑터.
 * 호출 체인: vtophys_notify → vtophys_walk_region → mem_map_walk_region
 */
static int
vtophys_walk_region(struct spdk_mem_map *map, void *vaddr, size_t len,
		    int (*map_page)(struct spdk_mem_map *map, uint64_t vaddr, size_t sz, void *ctx))
{
	/* [한국어] ctx=NULL로 페이지 단위 순회 실행. */
	return mem_map_walk_region(map, (uint64_t)vaddr, len, map_page, NULL);
}

/*
 * [한국어]
 * vtophys_notify - g_vtophys_map의 등록/해제 통지 콜백(메모리 종류별 변환 전략 선택)
 *
 * @cb_ctx: 미사용.
 * @map:    g_vtophys_map.
 * @action: REGISTER 또는 UNREGISTER.
 * @vaddr:  영역 시작(4KB 정렬, 256TB 내).
 * @len:    영역 길이.
 * @return: 0 성공, 실패 시 음수 errno.
 *
 * spdk_mem_register/unregister가 모든 맵에 통지할 때 vtophys 맵용으로 호출된다. 주소 종류에
 * 따라 다른 변환 경로를 선택한다: (1) DPDK 세그먼트(memseg), (2) PCI BAR, (3) IOMMU+IOVA==VA
 * (vaddr==IOVA), (4) iova=pa pagemap. 등록 시엔 필요하면 IOMMU 매핑까지 수행하고, 해제 시엔
 * 역으로 IOMMU unmap 후 트리 변환을 클리어한다.
 *
 * 실행 컨텍스트: 메인 스레드(등록/해제 경로). 호출 체인: spdk_mem_register/unregister → notify_cb → vtophys_notify
 */
static int
vtophys_notify(void *cb_ctx, struct spdk_mem_map *map,
	       enum spdk_mem_map_notify_action action,
	       void *vaddr, size_t len)
{
	/* [한국어] 결과 코드. */
	int rc = 0;
	/* [한국어] DPDK 세그먼트 물리주소(판별용). */
	uint64_t paddr;

	/* [한국어] 256TB 범위 검증. */
	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", vaddr);
		return -EINVAL;
	}

	/* [한국어] 4KB 정렬 검증. */
	if (((uintptr_t)vaddr & MASK_4KB) || (len & MASK_4KB)) {
		DEBUG_PRINT("invalid parameters, vaddr=%p len=%ju\n",
			    vaddr, len);
		return -EINVAL;
	}

	/* Get the physical address from the DPDK memsegs */
	/* [한국어] 먼저 DPDK 세그먼트 물리주소를 시도 — 성공/실패로 메모리 종류를 1차 판별. */
	paddr = vtophys_get_paddr_memseg((uint64_t)vaddr, NULL);

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		/* [한국어] DPDK 세그먼트가 아닌 주소(BAR/사용자 mmap 등). */
		if (paddr == SPDK_VTOPHYS_ERROR) {
			/* This is not an address that DPDK is managing. */

			/* Check if this is a PCI BAR. They need special handling */
			/* [한국어] PCI BAR이면 BAR 전용 등록 경로로. */
			paddr = vtophys_get_paddr_pci((uint64_t)vaddr, len);
			if (paddr != SPDK_VTOPHYS_ERROR) {
				return vtophys_walk_region(map, vaddr, len, vtophys_map_pci);
			}
#if VFIO_ENABLED
			/* [한국어] 현재 DPDK IOVA 모드 조회. */
			enum rte_iova_mode iova_mode;

			iova_mode = rte_eal_iova_mode();

			/* [한국어] IOMMU+IOVA==VA: vaddr을 IOVA로 IOMMU 매핑 후 vaddr 변환 등록. */
			if (spdk_iommu_is_enabled() && iova_mode == RTE_IOVA_VA) {
				/* We'll use the virtual address as the iova to match DPDK. */
				paddr = (uint64_t)vaddr;
				rc = vtophys_iommu_map_dma((uint64_t)vaddr, paddr, len);
				if (rc) {
					return -EFAULT;
				}

				rc = vtophys_walk_region(map, vaddr, len, vtophys_map_vaddr);
				if (rc != 0) {
					return rc;
				}
			} else
#endif
			{
				/* [한국어] 그 외(iova=pa) → pagemap 물리주소로 등록. */
				rc = vtophys_walk_region(map, vaddr, len, vtophys_map_pagemap);
				if (rc != 0) {
					return rc;
				}
			}
		} else {
			/* This is an address managed by DPDK. Just setup the translations. */
			/* [한국어] DPDK 관리 hugepage → 세그먼트 물리주소로 등록. */
			rc = vtophys_walk_region(map, vaddr, len, vtophys_map_memseg);
			if (rc != 0) {
				return rc;
			}
		}

		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
#if VFIO_ENABLED
		/* [한국어] DPDK 세그먼트가 아닌 주소면 IOMMU 해제가 필요할 수 있음. */
		if (paddr == SPDK_VTOPHYS_ERROR) {
			/*
			 * This is not an address that DPDK is managing.
			 */

			/* Check if this is a PCI BAR. They need special handling */
			/* [한국어] PCI BAR이면 BAR 전용 해제 경로로. */
			paddr = vtophys_get_paddr_pci((uint64_t)vaddr, len);
			if (paddr != SPDK_VTOPHYS_ERROR) {
				return vtophys_walk_region(map, vaddr, len, vtophys_unmap_pci);
			}

			/* If vfio is enabled,
			 * we need to unmap the range from the IOMMU
			 */
			/* [한국어] IOMMU가 켜져 있으면 IOMMU에서도 매핑을 떼어내야 함. */
			if (spdk_iommu_is_enabled()) {
				/* [한국어] 측정용 길이/주소/IOVA 모드. */
				uint64_t buffer_len = len;
				uint8_t *va = vaddr;
				enum rte_iova_mode iova_mode;

				iova_mode = rte_eal_iova_mode();
				/*
				 * In virtual address mode, the region is contiguous and can be done in
				 * one unmap.
				 */
				/* [한국어] IOVA==VA면 영역이 가상연속이라 한 번에 unmap. */
				if (iova_mode == RTE_IOVA_VA) {
					/* [한국어] 등록 시 vaddr==IOVA였으므로 변환 결과가 vaddr/len과 일치해야 함. */
					paddr = spdk_vtophys(va, &buffer_len);
					if (buffer_len != len || paddr != (uintptr_t)va) {
						DEBUG_PRINT("Unmapping %p with length %lu failed because "
							    "translation had address 0x%" PRIx64 " and length %lu\n",
							    va, len, paddr, buffer_len);
						return -EINVAL;
					}
					/* [한국어] IOMMU에서 단일 unmap. */
					rc = vtophys_iommu_unmap_dma(paddr, len);
					if (rc) {
						DEBUG_PRINT("Failed to iommu unmap paddr 0x%" PRIx64 "\n", paddr);
						return -EFAULT;
					}
				} else if (iova_mode == RTE_IOVA_PA) {
					/* [한국어] iova=pa면 물리 불연속 가능 → 페이지별 IOMMU unmap. */
					rc = vtophys_walk_region(map, vaddr, len,
								 vtophys_unmap_iommu_paddr);
					if (rc != 0) {
						return rc;
					}
				}
			}
		}
#endif
		/* [한국어] 마지막으로 vtophys 트리에서 변환을 클리어. */
		rc = vtophys_walk_region(map, vaddr, len, vtophys_unmap_page);
		break;
	default:
		/* [한국어] REGISTER/UNREGISTER 외 액션은 없음(방어). */
		SPDK_UNREACHABLE();
	}

	return rc;
}

/*
 * [한국어]
 * numa_notify - g_numa_map의 등록/해제 통지 콜백(가상주소→NUMA socket id 기록)
 *
 * @cb_ctx: 미사용.
 * @map:    g_numa_map.
 * @action: REGISTER/UNREGISTER.
 * @vaddr:  영역 시작.
 * @len:    영역 길이.
 * @return: 항상 0(세그먼트를 못 찾아도 실패로 보지 않음).
 *
 * DPDK 세그먼트의 socket_id를 NUMA 맵에 기록해 spdk_mem_get_numa_id가 O(1)에 답하게 한다.
 * vhost/vfio-user 등 비-DPDK 등록 경로에서는 세그먼트가 없을 수 있는데, 이때 0을 반환해야
 * 상위 spdk_mem_register가 실패하지 않는다(해당 메모리는 NUMA_ID_ANY로 처리).
 *
 * 호출 체인: spdk_mem_register/unregister → notify_cb → numa_notify
 */
static int
numa_notify(void *cb_ctx, struct spdk_mem_map *map,
	    enum spdk_mem_map_notify_action action,
	    void *vaddr, size_t len)
{
	/* [한국어] socket_id를 얻을 DPDK 세그먼트. */
	struct rte_memseg *seg;

	/* We always return 0 from here, even if we aren't able to get a
	 * memseg for the address. This can happen in non-DPDK memory
	 * registration paths, for example vhost or vfio-user. That is OK,
	 * spdk_mem_get_numa_id() just returns SPDK_ENV_NUMA_ID_ANY for
	 * that kind of memory. If we return an error here, the
	 * spdk_mem_register() from vhost or vfio-user would fail which is
	 * not what we want.
	 */
	/* [한국어] 세그먼트를 못 찾으면(비-DPDK 메모리) NUMA 기록 없이 성공 처리. */
	seg = rte_mem_virt2memseg(vaddr, NULL);
	if (seg == NULL) {
		return 0;
	}

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		/* [한국어] 이 영역의 NUMA socket id를 맵에 기록. */
		spdk_mem_map_set_translation(map, (uint64_t)vaddr, len, seg->socket_id);
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		/* [한국어] 해제 시 NUMA 기록 제거. */
		spdk_mem_map_clear_translation(map, (uint64_t)vaddr, len);
		break;
	default:
		break;
	}

	return 0;
}

/*
 * [한국어]
 * vtophys_check_contiguous_entries - vtophys 맵 전용 are_contiguous 콜백(물리 연속성 판정)
 *
 * @paddr1: 앞 페이지의 vtophys 변환 값(플래그 포함).
 * @paddr2: 뒤 페이지의 vtophys 변환 값.
 * @return: 두 페이지가 물리적으로 연속이면 1, 아니면 0.
 *
 * 가상주소 공간에서 인접한 두 페이지(4KB 또는 2MB)에 대해 호출된다. 물리주소가 정확히
 * 페이지 크기만큼 떨어져 있어야 물리 연속이며, 이때만 단일 PRP/SGL 엔트리로 묶을 수 있다.
 * 호출 체인: spdk_mem_map_translate(g_vtophys_map) → are_contiguous → vtophys_check_contiguous_entries
 */
static int
vtophys_check_contiguous_entries(uint64_t paddr1, uint64_t paddr2)
{
	/* [한국어] 앞 페이지의 4KB 플래그로 페이지 크기 결정. */
	uint64_t page_size = (paddr1 & VTOPHYS_4KB) ? VALUE_4KB : VALUE_2MB;

	/* This function is always called with paddrs for two subsequent
	 * 4KB/2MB chunks in virtual address space, so those chunks will be only
	 * physically contiguous if the physical addresses are 4KB/2MB apart
	 * from each other as well.
	 */
	/* [한국어] 물리주소 차이가 페이지 크기와 같으면 물리 연속. */
	return (paddr2 - paddr1 == page_size);
}

#if VFIO_ENABLED

/*
 * [한국어]
 * vfio_enabled - 시스템에 vfio_pci 드라이버가 활성화되어 있는지 질의
 *
 * @return: vfio_pci가 사용 가능하면 true.
 * 호출 체인: vtophys_iommu_init → vfio_enabled → rte_vfio_is_enabled
 */
static bool
vfio_enabled(void)
{
	/* [한국어] DPDK에 vfio_pci 가용 여부 질의. */
	return rte_vfio_is_enabled("vfio_pci");
}

/* Check if IOMMU is enabled on the system */
/*
 * [한국어]
 * has_iommu_groups - 시스템에 IOMMU 그룹이 하나라도 있는지 확인
 *
 * @return: /sys/kernel/iommu_groups에 ./, ../ 외 항목이 있으면 true.
 *
 * IOMMU가 켜진 시스템은 이 디렉토리에 그룹 디렉토리가 생긴다. ./와 ../를 빼기 위해 3개까지만
 * 세고 2개 초과면 IOMMU 활성으로 본다. 호출 체인: vtophys_iommu_init → has_iommu_groups
 */
static bool
has_iommu_groups(void)
{
	/* [한국어] 디렉토리 엔트리 카운터. */
	int count = 0;
	/* [한국어] IOMMU 그룹 디렉토리 열기. */
	DIR *dir = opendir("/sys/kernel/iommu_groups");

	/* [한국어] 못 열면 IOMMU 미지원으로 간주. */
	if (dir == NULL) {
		return false;
	}

	/* [한국어] 최대 3개 엔트리까지만 세면 충분(>2 판정용). */
	while (count < 3 && readdir(dir) != NULL) {
		count++;
	}

	closedir(dir);
	/* there will always be ./ and ../ entries */
	/* [한국어] ./, ../ 2개를 넘으면 실제 IOMMU 그룹 존재. */
	return count > 2;
}

/*
 * [한국어]
 * vfio_noiommu_enabled - VFIO no-IOMMU 모드 활성 여부 질의
 *
 * @return: no-IOMMU 모드면 true.
 * 호출 체인: vtophys_iommu_init → vfio_noiommu_enabled → rte_vfio_noiommu_is_enabled
 */
static bool
vfio_noiommu_enabled(void)
{
	/* [한국어] DPDK에 no-IOMMU 모드 여부 질의. */
	return rte_vfio_noiommu_is_enabled();
}

/*
 * [한국어]
 * vtophys_iommu_init - VFIO container fd를 발견하고 g_vfio 상태를 초기화
 *
 * vfio_pci가 활성이고 IOMMU 그룹(또는 no-IOMMU 모드)이 있을 때만 진행한다. DPDK가 내부에서
 * 만든 /dev/vfio/vfio container의 fd를 /proc/self/fd 심볼릭 링크들을 스캔해 찾아낸다.
 * 성공 시 g_vfio.fd/enabled를 채워 이후 IOMMU map/unmap이 가능해진다.
 *
 * 실행 컨텍스트: 메인 스레드(vtophys_init 내). 호출 체인: vtophys_init → vtophys_iommu_init
 */
static void
vtophys_iommu_init(void)
{
	/* [한국어] /proc/self/fd/<n> 경로 버퍼. */
	char proc_fd_path[PATH_MAX + 1];
	/* [한국어] readlink 결과(심볼릭 링크가 가리키는 실제 경로). */
	char link_path[PATH_MAX + 1];
	/* [한국어] 찾고자 하는 VFIO container 경로. */
	const char vfio_path[] = "/dev/vfio/vfio";
	/* [한국어] /proc/self/fd 디렉토리 핸들. */
	DIR *dir;
	/* [한국어] 디렉토리 엔트리. */
	struct dirent *d;

	/* [한국어] vfio_pci 미활성이면 IOMMU 사용 불가. */
	if (!vfio_enabled()) {
		return;
	}

	/* [한국어] no-IOMMU 모드면 표시, 아니면 IOMMU 그룹 존재를 확인(없으면 중단). */
	if (vfio_noiommu_enabled()) {
		g_vfio.noiommu_enabled = true;
	} else if (!has_iommu_groups()) {
		return;
	}

	/* [한국어] 현재 프로세스의 열린 fd 목록을 스캔. */
	dir = opendir("/proc/self/fd");
	if (!dir) {
		DEBUG_PRINT("Failed to open /proc/self/fd (%d)\n", errno);
		return;
	}

	/* [한국어] 각 fd 엔트리를 검사. */
	while ((d = readdir(dir)) != NULL) {
		/* [한국어] 심볼릭 링크가 아닌 항목(., ..)은 스킵. */
		if (d->d_type != DT_LNK) {
			continue;
		}

		/* [한국어] /proc/self/fd/<n> 경로 구성. */
		snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/self/fd/%s", d->d_name);
		/* [한국어] 링크가 가리키는 경로를 읽고 길이가 vfio_path와 다르면 스킵. */
		if (readlink(proc_fd_path, link_path, sizeof(link_path)) != (sizeof(vfio_path) - 1)) {
			continue;
		}

		/* [한국어] 경로가 /dev/vfio/vfio와 일치하면 이 fd가 container fd. */
		if (memcmp(link_path, vfio_path, sizeof(vfio_path) - 1) == 0) {
			/* [한국어] fd 번호(디렉토리 엔트리 이름)를 파싱해 저장. */
			sscanf(d->d_name, "%d", &g_vfio.fd);
			break;
		}
	}

	closedir(dir);

	/* [한국어] container fd를 못 찾았으면 IOMMU 비활성. */
	if (g_vfio.fd < 0) {
		DEBUG_PRINT("Failed to discover DPDK VFIO container fd.\n");
		return;
	}

	/* [한국어] VFIO 사용 가능 표시. */
	g_vfio.enabled = true;

	return;
}

#endif

/*
 * [한국어]
 * vtophys_pci_device_added - SPDK가 PCI 디바이스를 attach할 때 호출(BAR 트래킹 + 보류 매핑 적용)
 *
 * @pci_device: attach된 DPDK PCI 디바이스.
 *
 * 디바이스를 g_vtophys_pci_devices 큐에 추가해 BAR vtophys 검색 대상에 넣는다. VFIO에서는
 * 이 디바이스가 첫 SPDK 디바이스(device_ref 0→1)이면, container에 IOMMU 그룹이 비로소
 * 들어왔으므로 그동안 보류했던 모든 DMA 매핑을 이제 실제 ioctl로 일괄 적용한다.
 *
 * 실행 컨텍스트: 메인 스레드(PCI attach). 호출 체인: lib/nvme PCIe ctrlr_construct → spdk_pci_device_attach → vtophys_pci_device_added
 */
void
vtophys_pci_device_added(struct rte_pci_device *pci_device)
{
	/* [한국어] 새 트래킹 노드. */
	struct spdk_vtophys_pci_device *vtophys_dev;

	/* [한국어] 디바이스 큐 보호 락. */
	pthread_mutex_lock(&g_vtophys_pci_devices_mutex);

	/* [한국어] 디바이스 트래킹 노드 할당 후 큐에 추가. */
	vtophys_dev = calloc(1, sizeof(*vtophys_dev));
	if (vtophys_dev) {
		vtophys_dev->pci_device = pci_device;
		TAILQ_INSERT_TAIL(&g_vtophys_pci_devices, vtophys_dev, tailq);
	} else {
		/* [한국어] 할당 실패는 로그만(BAR vtophys 검색에서 이 디바이스가 빠질 뿐). */
		DEBUG_PRINT("Memory allocation error\n");
	}
	pthread_mutex_unlock(&g_vtophys_pci_devices_mutex);

#if VFIO_ENABLED
	/* [한국어] 보류 매핑 일괄 적용용 임시. */
	struct spdk_vfio_dma_map *dma_map;
	int ret;

	/* [한국어] VFIO 비활성이면 IOMMU 매핑 처리 불필요. */
	if (!g_vfio.enabled) {
		return;
	}

	/* [한국어] container 디바이스 참조계수 증가(보호 락). */
	pthread_mutex_lock(&g_vfio.mutex);
	g_vfio.device_ref++;
	/* [한국어] 이미 다른 디바이스가 있었다면 보류 매핑은 이미 적용됨 → 종료. */
	if (g_vfio.device_ref > 1) {
		pthread_mutex_unlock(&g_vfio.mutex);
		return;
	}

	/* This is the first SPDK device using DPDK vfio. This means that the first
	 * IOMMU group might have been just been added to the DPDK vfio container.
	 * From this point it is certain that the memory can be mapped now.
	 */
	/* [한국어] 첫 디바이스 → 그간 큐에만 쌓아둔 보류 매핑을 모두 실제 ioctl로 적용. */
	TAILQ_FOREACH(dma_map, &g_vfio.maps, tailq) {
		ret = ioctl(g_vfio.fd, VFIO_IOMMU_MAP_DMA, &dma_map->map);
		if (ret) {
			DEBUG_PRINT("Cannot update DMA mapping, error %d\n", errno);
			break;
		}
	}
	pthread_mutex_unlock(&g_vfio.mutex);
#endif
}

/*
 * [한국어]
 * vtophys_pci_device_removed - SPDK가 PCI 디바이스를 detach할 때 호출(트래킹 제거 + 매핑 정리)
 *
 * @pci_device: detach되는 DPDK PCI 디바이스.
 *
 * g_vtophys_pci_devices 큐에서 노드를 제거한다. VFIO에서는 device_ref를 감소시켜 마지막 SPDK
 * 디바이스(1→0)이면, 나중에 어떤 외부 요인과 무관하게 쉽게 재매핑할 수 있도록 모든 DMA 매핑을
 * 수동으로 IOMMU에서 떼어낸다(노드는 큐에 남겨 두어 재추가 시 재적용).
 *
 * 실행 컨텍스트: 메인 스레드(PCI detach). 호출 체인: spdk_pci_device_detach → vtophys_pci_device_removed
 */
void
vtophys_pci_device_removed(struct rte_pci_device *pci_device)
{
	/* [한국어] 제거 대상 트래킹 노드. */
	struct spdk_vtophys_pci_device *vtophys_dev;

	/* [한국어] 디바이스 큐 보호 락. */
	pthread_mutex_lock(&g_vtophys_pci_devices_mutex);
	/* [한국어] 일치하는 디바이스 노드를 찾아 큐에서 제거/해제. */
	TAILQ_FOREACH(vtophys_dev, &g_vtophys_pci_devices, tailq) {
		if (vtophys_dev->pci_device == pci_device) {
			TAILQ_REMOVE(&g_vtophys_pci_devices, vtophys_dev, tailq);
			free(vtophys_dev);
			break;
		}
	}
	pthread_mutex_unlock(&g_vtophys_pci_devices_mutex);

#if VFIO_ENABLED
	/* [한국어] 매핑 해제용 임시. */
	struct spdk_vfio_dma_map *dma_map;
	int ret;

	/* [한국어] VFIO 비활성이면 처리 불필요. */
	if (!g_vfio.enabled) {
		return;
	}

	/* [한국어] 참조계수 감소(보호 락). */
	pthread_mutex_lock(&g_vfio.mutex);
	assert(g_vfio.device_ref > 0);
	g_vfio.device_ref--;
	/* [한국어] 아직 다른 디바이스가 남았으면 매핑 유지 → 종료. */
	if (g_vfio.device_ref > 0) {
		pthread_mutex_unlock(&g_vfio.mutex);
		return;
	}

	/* This is the last SPDK device using DPDK vfio. If DPDK doesn't have
	 * any additional devices using it's vfio container, all the mappings
	 * will be automatically removed by the Linux vfio driver. We unmap
	 * the memory manually to be able to easily re-map it later regardless
	 * of other, external factors.
	 */
	/* [한국어] 마지막 디바이스 → 모든 매핑을 수동으로 IOMMU에서 해제(노드는 큐에 보존). */
	TAILQ_FOREACH(dma_map, &g_vfio.maps, tailq) {
		/* [한국어] 각 매핑에 대한 unmap ioctl 인자 구성. */
		struct vfio_iommu_type1_dma_unmap unmap = {};
		unmap.argsz = sizeof(unmap);
		unmap.flags = 0;
		unmap.iova = dma_map->map.iova;
		unmap.size = dma_map->map.size;
		/* [한국어] 커널 IOMMU에서 매핑 제거. */
		ret = ioctl(g_vfio.fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
		if (ret) {
			DEBUG_PRINT("Cannot unmap DMA memory, error %d\n", errno);
			break;
		}
	}
	pthread_mutex_unlock(&g_vfio.mutex);
#endif
}

/*
 * [한국어]
 * vtophys_init - vtophys/phys_ref/numa 맵을 생성하고 IOMMU를 초기화
 *
 * @return: 0 성공, 맵 할당 실패 시 -ENOMEM(부분 생성분 롤백).
 *
 * SPDK env 초기화 단계에서 호출. (1) IOMMU container fd 발견, (2) g_phys_ref_map(IOVA 참조계수),
 * (3) hugepage 사용 시 g_numa_map, (4) vtophys 활성(no-pci 아님) 시 g_vtophys_map을 만든다.
 * 각 맵의 ops(notify_cb/are_contiguous)에 따라 등록 시 자동으로 IOVA/소켓ID가 채워진다.
 *
 * 실행 컨텍스트: 메인 스레드(초기화). 호출 체인: spdk_env 초기화 → vtophys_init
 */
int
vtophys_init(void)
{
	/* [한국어] vtophys 맵 ops: 등록 시 IOVA 채움 + 물리 연속성 판정. */
	const struct spdk_mem_map_ops vtophys_map_ops = {
		.notify_cb = vtophys_notify,
		.are_contiguous = vtophys_check_contiguous_entries,
	};

	/* [한국어] phys_ref 맵 ops: 단순 카운터 저장소(콜백 불필요). */
	const struct spdk_mem_map_ops phys_ref_map_ops = {
		.notify_cb = NULL,
		.are_contiguous = NULL,
	};

	/* [한국어] numa 맵 ops: 등록 시 socket id 기록. */
	const struct spdk_mem_map_ops numa_map_ops = {
		.notify_cb = numa_notify,
		.are_contiguous = NULL,
	};

#if VFIO_ENABLED
	/* [한국어] IOMMU container fd 발견 및 g_vfio 초기화(맵 생성 전에 수행). */
	vtophys_iommu_init();
#endif

	/* [한국어] IOVA 참조계수 맵 생성(default=0). */
	g_phys_ref_map = spdk_mem_map_alloc(0, &phys_ref_map_ops, NULL);
	if (g_phys_ref_map == NULL) {
		DEBUG_PRINT("phys_ref map allocation failed.\n");
		return -ENOMEM;
	}

	/* [한국어] hugepage를 쓸 때만 NUMA 맵 생성(default=ANY). */
	if (g_huge_pages) {
		g_numa_map = spdk_mem_map_alloc(SPDK_ENV_NUMA_ID_ANY, &numa_map_ops, NULL);
		if (g_numa_map == NULL) {
			DEBUG_PRINT("numa map allocation failed.\n");
			/* [한국어] 실패 시 앞서 만든 phys_ref 맵 롤백. */
			spdk_mem_map_free(&g_phys_ref_map);
			return -ENOMEM;
		}
	}

	/* [한국어] vtophys 활성(no-pci 아님)일 때만 vtophys 맵 생성. 등록은 alloc 시점에
	 * notify_cb로 기존 영역에 대해 즉시 수행되어 IOVA가 채워진다. */
	if (g_vtophys) {
		g_vtophys_map = spdk_mem_map_alloc(SPDK_VTOPHYS_ERROR, &vtophys_map_ops, NULL);
		if (g_vtophys_map == NULL) {
			DEBUG_PRINT("vtophys map allocation failed\n");
			/* [한국어] 실패 시 numa/phys_ref 맵 롤백. */
			spdk_mem_map_free(&g_numa_map);
			spdk_mem_map_free(&g_phys_ref_map);
			return -ENOMEM;
		}
	}

	return 0;
}

/*
 * [한국어]
 * vtophys_fini - vtophys/numa/phys_ref 맵을 모두 해제
 *
 * env 종료 시 호출. 각 맵 해제 시 notify_cb가 있는 맵(vtophys/numa)은 등록된 영역에 대해
 * UNREGISTER 통지가 발생(IOMMU unmap 등). 호출 체인: spdk_env 종료 → vtophys_fini
 */
void
vtophys_fini(void)
{
	/* [한국어] vtophys 맵 해제(IOMMU unmap 유발). */
	spdk_mem_map_free(&g_vtophys_map);
	/* [한국어] NUMA 맵 해제. */
	spdk_mem_map_free(&g_numa_map);
	/* [한국어] 참조계수 맵 해제. */
	spdk_mem_map_free(&g_phys_ref_map);
}

/*
 * [한국어]
 * spdk_vtophys - 가상주소를 IOVA(또는 물리주소)로 변환 (외부 핫패스 API)
 *
 * @buf:  변환할 호스트 가상주소(페이지 내 임의 오프셋 허용).
 * @size: 입출력(NULL 허용). 입력=원하는 길이, 출력=물리 연속 길이.
 * @return: IOVA + 페이지 내 오프셋, 또는 SPDK_VTOPHYS_ERROR.
 *
 * NVMe 드라이버가 PRP/SGL 엔트리를 만들 때 매 I/O마다 호출하는 가장 뜨거운 변환 함수.
 * g_vtophys_map에서 페이지 변환을 얻고, 4KB/2MB 플래그로 페이지 내 오프셋을 더해 최종 IOVA를
 * 만든다. 룩업은 lock-free(트리 노드는 등록 후 불변). no-pci 환경이면 맵이 없어 ERROR.
 *
 * 실행 컨텍스트: 모든 reactor(I/O 발행 스레드). 호출 체인: lib/nvme prp_list_append/build_request → spdk_vtophys → spdk_mem_map_translate
 */
uint64_t
spdk_vtophys(const void *buf, uint64_t *size)
{
	/* [한국어] vaddr=입력 가상주소, paddr=룩업 결과(플래그 포함), mask=오프셋 마스크. */
	uint64_t vaddr, paddr, mask;

	/* vtophys map do not get created in no-pci env */
	/* [한국어] no-pci면 vtophys 맵이 없어 변환 불가. */
	if (g_vtophys_map == NULL) {
		return SPDK_VTOPHYS_ERROR;
	}

	/* [한국어] 포인터를 정수 주소로. */
	vaddr = (uint64_t)buf;
	/* [한국어] 페이지 단위 변환 + 연속 길이 측정. */
	paddr = spdk_mem_map_translate(g_vtophys_map, vaddr, size);
	/* [한국어] 미등록/오류면 그대로 ERROR 반환. */
	if (paddr == SPDK_VTOPHYS_ERROR) {
		return SPDK_VTOPHYS_ERROR;
	}

	/* [한국어] 4KB 매핑이면 4KB 마스크, 아니면 2MB 마스크로 페이지 내 오프셋 추출. */
	mask = (paddr & VTOPHYS_4KB) ? MASK_4KB : MASK_2MB;
	/* [한국어] 플래그 제거한 IOVA에 페이지 내 오프셋을 더해 최종 IOVA 반환. */
	return VTOPHYS_ADDR(paddr) + (vaddr & mask);
}

/*
 * [한국어]
 * spdk_mem_get_numa_id - 가상주소가 속한 NUMA socket id 조회 (외부 API)
 *
 * @buf:  질의할 가상주소.
 * @size: 입출력(NULL 허용). 같은 socket id가 연속되는 길이.
 * @return: NUMA socket id, NUMA 맵이 없거나 비-DPDK 메모리면 SPDK_ENV_NUMA_ID_ANY.
 *
 * NUMA-aware 버퍼 배치/스레드 affinity 결정에 사용. 호출 체인: 상위 NUMA 최적화 코드 → spdk_mem_get_numa_id → spdk_mem_map_translate
 */
int32_t
spdk_mem_get_numa_id(const void *buf, uint64_t *size)
{
	/* [한국어] hugepage 미사용 등으로 NUMA 맵이 없으면 ANY. */
	if (!g_numa_map) {
		return SPDK_ENV_NUMA_ID_ANY;
	}

	/* [한국어] NUMA 맵에서 socket id 조회. */
	return spdk_mem_map_translate(g_numa_map, (uint64_t)buf, size);
}

/*
 * [한국어]
 * spdk_mem_get_fd_and_offset - 가상주소가 속한 hugepage 백킹 파일의 fd와 오프셋 조회 (외부 API)
 *
 * @vaddr:  질의할 가상주소(DPDK 세그먼트여야 함).
 * @offset: 출력. 백킹 파일 내 오프셋.
 * @return: 백킹 파일 fd(>=0), 세그먼트 없음 -ENOENT, DPDK 조회 실패 시 음수.
 *
 * vhost/vfio-user 등이 게스트/외부 프로세스와 hugepage 메모리를 공유(fd 전달)할 때 사용한다.
 * 호출 체인: lib/vhost / vfio-user → spdk_mem_get_fd_and_offset → rte_memseg_get_fd*
 */
int
spdk_mem_get_fd_and_offset(void *vaddr, uint64_t *offset)
{
	/* [한국어] 대상 DPDK 세그먼트. */
	struct rte_memseg *seg;
	/* [한국어] ret=오프셋 조회 결과, fd=백킹 파일 디스크립터. */
	int ret, fd;

	/* [한국어] 가상주소를 포함하는 세그먼트 조회. */
	seg = rte_mem_virt2memseg(vaddr, NULL);
	if (!seg) {
		SPDK_ERRLOG("memory %p doesn't exist\n", vaddr);
		return -ENOENT;
	}

	/* [한국어] 세그먼트의 hugepage 백킹 파일 fd 조회(thread-unsafe — 단일 스레드 가정). */
	fd = rte_memseg_get_fd_thread_unsafe(seg);
	if (fd < 0) {
		return fd;
	}

	/* [한국어] 백킹 파일 내 이 세그먼트의 오프셋 조회. */
	ret = rte_memseg_get_fd_offset_thread_unsafe(seg, offset);
	if (ret < 0) {
		return ret;
	}

	/* [한국어] fd 반환(offset은 출력 인자로 전달됨). */
	return fd;
}

/*
 * [한국어]
 * mem_disable_huge_pages - hugepage 사용을 비활성화하는 런타임 정책 토글 (외부 API)
 *
 * --no-huge 환경에서 호출. g_huge_pages=false로 두면 vtophys_init이 NUMA 맵을 만들지 않고
 * 4KB 페이지 기반 매핑만 사용한다. 반드시 vtophys_init 이전에 호출되어야 한다.
 * 호출 체인: env 옵션 파싱 → mem_disable_huge_pages
 */
void
mem_disable_huge_pages(void)
{
	/* [한국어] hugepage 비활성 플래그 설정. */
	g_huge_pages = false;
}

/*
 * [한국어]
 * mem_disable_vtophys - vtophys(물리주소 변환)를 비활성화하는 런타임 정책 토글 (외부 API)
 *
 * --no-pci 등 PCI/DMA가 없는 환경에서 호출. g_vtophys=false면 vtophys_init이 g_vtophys_map을
 * 만들지 않아 spdk_vtophys가 항상 ERROR를 반환한다. vtophys_init 이전에 호출되어야 한다.
 * 호출 체인: env 옵션 파싱 → mem_disable_vtophys
 */
void
mem_disable_vtophys(void)
{
	/* [한국어] vtophys 비활성 플래그 설정. */
	g_vtophys = false;
}
