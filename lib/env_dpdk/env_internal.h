/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK env_dpdk 서브시스템의 내부 전용 헤더 (env_internal.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 lib/env_dpdk/ 디렉토리 내부의 .c 파일들끼리만 공유하는 선언을 모아둔 파일이다.
 * SPDK는 자체적인 메모리/PCI/스레드 추상화 API(spdk_*)를 외부에 제공하지만, 그 구현은 DPDK
 * (Data Plane Development Kit)의 EAL(Environment Abstraction Layer)에 위임한다.
 * 이 헤더는 외부에 노출하지 않는 초기화/종료 루틴(pci_env_init, mem_map_init, vtophys_init 등),
 * 가상↔IOVA 주소 변환을 위한 vtophys 등록/해제 함수, hugepage·NUMA·vtophys 정책을 비활성화하는
 * 내부 토글, 그리고 256 TB 가상주소 공간을 4KB/2MB 페이지 단위 트리로 관리하기 위한 비트 시프트
 * 매크로(SHIFT_256TB, SHIFT_1GB)를 정의한다. 또한 SPDK가 요구하는 DPDK 최소 버전(21.11)을
 * 컴파일 타임에 강제한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 전체 I/O 스택의 가장 아래쪽에 위치하는 환경 추상화 계층(lib/env_dpdk)에 속하며,
 * spdk_env_init() → DPDK rte_eal_init() → pci_env_init()/mem_map_init()/vtophys_init() 호출
 * 체인의 핵심 진입점들을 선언한다. 즉, SPDK 애플리케이션이 부팅될 때 가장 먼저 실행되는 경로
 * (애플리케이션 → spdk_app_start → spdk_env_init → init.c → 본 헤더의 함수들)에서 사용된다.
 * 실행 컨텍스트는 호스트 유저스페이스의 main 스레드(또는 primary process의 EAL 초기화 단계)이며,
 * polled-mode I/O가 시작되기 전 단일 스레드 환경에서 호출된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존하는 모듈: DPDK (rte_eal, rte_config, rte_version), spdk/env.h(공개 API), spdk/stdinc.h.
 * - 본 헤더에 의존하는 파일: lib/env_dpdk/init.c, lib/env_dpdk/memory.c, lib/env_dpdk/pci.c,
 *   lib/env_dpdk/env.c, lib/env_dpdk/threads.c 등.
 * - 데이터 흐름: SPDK NVMe 드라이버(lib/nvme)와 bdev 코어(lib/bdev)는 DMA 버퍼를 spdk_dma_*alloc로
 *   할당하며, 그 결과 가상주소는 vtophys 모듈을 통해 IOVA(IOMMU 사용 시) 또는 물리주소로
 *   변환된다. 본 헤더의 vtophys_iommu_map_dma_bar/_unmap_dma_bar는 PCIe BAR(MMIO) 영역을
 *   IOMMU에 매핑하여 NVMe 컨트롤러가 호스트 메모리에 DMA할 수 있게 한다.
 * - 공유 핵심 자료구조: rte_pci_device(DPDK가 정의), spdk_pci_driver(SPDK 측 PCI 드라이버 핸들).
 *
 * === 주요 함수/구조체 요약 ===
 * - pci_env_init/_fini/_reinit: SPDK 측 PCI 드라이버 등록/해제. rte_pci_register와 연동.
 * - mem_map_init/_fini: 256 TB 가상주소 공간을 1 GB 페이지 단위로 색인하는 2단계 트리 초기화.
 * - vtophys_init/_fini: 가상주소→IOVA 변환 캐시 초기화. NVMe DMA 경로의 핵심.
 * - vtophys_iommu_map_dma_bar/_unmap_dma_bar: PCIe BAR을 IOMMU에 매핑/해제.
 * - vtophys_pci_device_added/_removed: DMA 가능 PCI 디바이스의 refcount 관리.
 * - mem_disable_huge_pages/mem_disable_vtophys/mem_enforce_numa: CLI 옵션에 따른 정책 토글.
 * - SHIFT_256TB/MASK_256TB/SHIFT_1GB/VALUE_1GB/MASK_1GB: 가상주소 인덱스 계산용 비트 매크로.
 */

#ifndef SPDK_ENV_INTERNAL_H
#define SPDK_ENV_INTERNAL_H

/* [한국어] SPDK 표준 인클루드 묶음 (stdint, stdio, string 등을 포함).
 * 본 헤더가 size_t/uint64_t/bool 같은 표준 타입을 직접 사용하므로 필요. */
#include "spdk/stdinc.h"

/* [한국어] SPDK 공개 env API 헤더. spdk_pci_*, spdk_dma_* 등 외부 노출 타입과
 * 본 헤더에서 다루는 PCI 디바이스/드라이버 추상화의 정의가 들어있어 필수. */
#include "spdk/env.h"

/* [한국어] DPDK 컴파일 시점 매크로 모음 (RTE_MAX_LCORE, RTE_CACHE_LINE_SIZE 등).
 * 본 헤더가 내부적으로 DPDK 자료구조를 노출하기 때문에 가장 먼저 포함해야 한다. */
#include <rte_config.h>
/* [한국어] DPDK 버전 매크로 (RTE_VERSION, RTE_VERSION_NUM). 아래 #if 검사에 사용. */
#include <rte_version.h>
/* [한국어] DPDK EAL 초기화/종료 API. rte_eal_init, rte_eal_process_type 등이 정의됨. */
#include <rte_eal.h>

/* [한국어] DPDK 21.11 미만 버전은 SPDK가 사용하는 일부 API(rte_get_main_lcore,
 * 새로운 rte_pci 인터페이스 등)를 제공하지 않으므로 컴파일 자체를 거부한다.
 * 컴파일 타임에 명확한 진단을 띄워 ABI 불일치 런타임 오류를 방지하기 위함. */
#if RTE_VERSION < RTE_VERSION_NUM(21, 11, 0, 0)
#error RTE_VERSION is too old! Minimum 21.11 is required.
#endif

/* x86-64 and ARM userspace virtual addresses use only the low 48 bits [0..47],
 * which is enough to cover 256 TB.
 */
/* [한국어] x86-64/ARM64 유저스페이스 가상주소는 보통 하위 48비트만 사용한다(canonical form).
 * 따라서 SPDK는 256 TB(=2^48) 공간을 1 GB 페이지로 색인하는 다단계 테이블을 사용하며,
 * 그 시프트 폭이 48이다. 가상주소→IOVA 트리의 최상위 인덱스를 뽑는 데 사용된다. */
#define SHIFT_256TB	48 /* (1 << 48) == 256 TB */
/* [한국어] 가상주소에서 256 TB 범위 내부의 오프셋을 추출할 때 사용하는 마스크. */
#define MASK_256TB	((1ULL << SHIFT_256TB) - 1)

/* [한국어] 1 GB 페이지의 시프트 폭. SPDK는 vtophys 매핑 트리에서 1 GB 단위 청크로 인덱싱하여
 * 메모리 사용량과 룩업 비용의 균형을 맞춘다(2 MB 페이지 256개 = 512 MB이 아닌 1 GB 단위). */
#define SHIFT_1GB	30 /* (1 << 30) == 1 GB */
/* [한국어] 1 GB의 정수값. 청크 경계 정렬·할당 크기 계산에 사용. */
#define VALUE_1GB	(1ULL << SHIFT_1GB)
/* [한국어] 가상주소에서 1 GB 청크 내부 오프셋을 추출하는 마스크. */
#define MASK_1GB	((1ULL << SHIFT_1GB) - 1)

/*
 * [한국어]
 * pci_env_init - SPDK 측 PCI 드라이버 등록 테이블 초기화
 *
 * @return: 성공 시 0, 실패 시 음수 errno.
 *
 * spdk_env_init() 직후 한 번 호출되어, SPDK가 선언한 모든 spdk_pci_driver 항목을
 * DPDK rte_pci 서브시스템에 등록한다. 이후 디바이스 probe 시 매칭이 가능해진다.
 * 실행 컨텍스트: 메인 스레드, EAL init 직후. 재진입 불가.
 *
 * 호출 체인: spdk_env_dpdk_post_init → pci_env_init → rte_pci_register
 */
int pci_env_init(void);

/*
 * [한국어]
 * pci_env_reinit - 자식 프로세스(secondary)에서 PCI 환경 재등록
 *
 * primary→secondary 분기 시 secondary 측에서 SPDK PCI 드라이버를 다시 연결할 때 사용.
 * 실행 컨텍스트: secondary 프로세스의 초기화 단계.
 *
 * 호출 체인: spdk_env_dpdk_post_init(secondary) → pci_env_reinit
 */
void pci_env_reinit(void);

/*
 * [한국어]
 * pci_env_fini - PCI 환경 정리
 *
 * spdk_env_fini 시 호출되어, 등록된 SPDK PCI 드라이버를 해제하고 내부 자료구조를 정리한다.
 * 실행 컨텍스트: 메인 스레드, 모든 polling이 멈춘 후.
 */
void pci_env_fini(void);

/*
 * [한국어]
 * mem_map_init - vtophys 가상주소 트리 초기화
 *
 * @legacy_mem: 레거시 메모리 모드 여부. true면 DPDK가 페이지를 동적으로 추가/제거하지 않고
 *              초기화 시 한 번 등록된 hugepage만 사용한다(낮은 오버헤드, 동적 hotplug 불가).
 * @return: 성공 시 0, 실패 시 음수 errno.
 *
 * 256 TB 가상주소 공간을 1 GB 청크 단위로 인덱싱하기 위한 2단계 트리(루트 256 K 엔트리 ×
 * 1 GB 청크 1024 엔트리)를 할당한다. 이후 모든 spdk_dma_malloc 결과는 이 트리에 등록되어
 * NVMe DMA 시 빠른 vtophys 변환이 가능하다.
 * 실행 컨텍스트: 메인 스레드.
 *
 * 호출 체인: spdk_env_dpdk_post_init → mem_map_init → rte_mem_event_callback_register
 */
int mem_map_init(bool legacy_mem);

/*
 * [한국어]
 * mem_map_fini - vtophys 가상주소 트리 해제
 *
 * 모든 매핑 엔트리를 해제하고 내부 트리 메모리를 free한다.
 * 실행 컨텍스트: spdk_env_fini 종료 단계.
 */
void mem_map_fini(void);

/*
 * [한국어]
 * vtophys_init - 가상→IOVA(또는 물리) 변환 모듈 초기화
 *
 * @return: 성공 시 0, 실패 시 음수 errno.
 *
 * vtophys 매핑 룩업 캐시를 초기화하고, IOMMU 사용 가능 여부를 판별한다.
 * IOMMU(VFIO) 모드에서는 IOVA가 가상주소와 일치하도록 매핑되며,
 * 비-IOMMU(uio) 모드에서는 hugepage의 실제 물리주소를 반환한다.
 *
 * 호출 체인: spdk_env_dpdk_post_init → vtophys_init
 */
int vtophys_init(void);

/*
 * [한국어]
 * vtophys_fini - vtophys 변환 모듈 정리
 *
 * 모든 IOVA 매핑을 해제하고 내부 자료구조를 free한다.
 */
void vtophys_fini(void);

/*
 * [한국어]
 * vtophys_iommu_map_dma_bar - PCIe BAR(MMIO 영역)을 IOMMU에 매핑
 *
 * @vaddr: BAR을 mmap한 호스트 가상주소.
 * @iova: 디바이스가 사용할 IOVA(보통 vaddr와 동일하게 1:1 매핑).
 * @size: BAR 크기(바이트).
 * @return: 성공 시 0, 실패 시 음수 errno.
 *
 * 일부 NVMe 컨트롤러는 자기 BAR을 통해 다른 디바이스나 호스트 메모리에 DMA를 시도할 수
 * 있는데(P2P 등), 그러려면 BAR도 IOMMU 도메인 안에 등록되어 있어야 한다.
 * 본 함수는 VFIO container_dma_map ioctl로 그 매핑을 추가한다.
 * 실행 컨텍스트: 메인 또는 디바이스 attach 스레드. 내부 락 사용.
 */
int vtophys_iommu_map_dma_bar(uint64_t vaddr, uint64_t iova, uint64_t size);

/*
 * [한국어]
 * vtophys_iommu_unmap_dma_bar - PCIe BAR의 IOMMU 매핑 해제
 *
 * @vaddr: 이전 매핑 시 사용한 가상주소(키 역할).
 * @return: 성공 시 0, 실패 시 음수 errno.
 *
 * 디바이스 detach 시 호출되어 VFIO container_dma_unmap을 수행한다.
 */
int vtophys_iommu_unmap_dma_bar(uint64_t vaddr);

/* [한국어] DPDK rte_pci_device의 전방 선언. 본 헤더 사용자가 헤더 구조 전체를
 * 알 필요가 없도록 포인터 인자만 받는 형태로 추상화. */
struct rte_pci_device;

/**
 * Report a DMA-capable PCI device to the vtophys translation code.
 * Increases the refcount of active DMA-capable devices managed by SPDK.
 * This must be called after a `rte_pci_device` is created.
 */
/*
 * [한국어]
 * vtophys_pci_device_added - DMA 가능 PCI 디바이스를 vtophys에 등록
 *
 * @pci_device: 새로 attach된 DPDK PCI 디바이스 핸들.
 *
 * SPDK가 관리하는 활성 DMA 디바이스 수(refcount)를 1 증가시킨다. 이 카운터가 0보다 크면
 * 이후 hugepage 매핑·해제 이벤트마다 IOMMU에도 동일하게 반영해야 함을 의미한다.
 * 실행 컨텍스트: PCI probe 콜백 내부(메인 스레드).
 *
 * 호출 체인: spdk_pci_device_attach → 모듈별 probe_fn → vtophys_pci_device_added
 */
void vtophys_pci_device_added(struct rte_pci_device *pci_device);

/**
 * Report the removal of a DMA-capable PCI device to the vtophys translation code.
 * Decreases the refcount of active DMA-capable devices managed by SPDK.
 * This must be called before a `rte_pci_device` is destroyed.
 */
/*
 * [한국어]
 * vtophys_pci_device_removed - DMA 가능 PCI 디바이스의 vtophys 등록 해제
 *
 * @pci_device: detach 직전인 DPDK PCI 디바이스 핸들.
 *
 * 활성 DMA 디바이스 refcount를 1 감소시킨다. 0이 되면 이후 IOMMU 매핑 트래킹을 생략할 수
 * 있어 fast path 비용이 줄어든다.
 *
 * 호출 체인: spdk_pci_device_detach → vtophys_pci_device_removed → rte_pci_remove
 */
void vtophys_pci_device_removed(struct rte_pci_device *pci_device);

/**
 * Disable huge page usage based on SPDK command line option --no-huge.
 */
/*
 * [한국어]
 * mem_disable_huge_pages - hugepage 사용 비활성화 토글
 *
 * SPDK CLI의 --no-huge 옵션이 주어진 경우 호출된다. 이 모드에서는 4KB 일반 페이지로
 * 동작하므로 vtophys 매핑이 페이지 단위로 매우 많아지고 성능이 떨어지지만, hugepage가
 * 없는 환경(예: 일부 컨테이너)에서도 SPDK를 구동할 수 있다.
 */
void mem_disable_huge_pages(void);

/**
 * Disable vtophys map based on no_pci environment option
 */
/*
 * [한국어]
 * mem_disable_vtophys - vtophys 매핑 자체 비활성화
 *
 * --no-pci 옵션 등 PCI/DMA 디바이스를 전혀 사용하지 않는 시나리오에서 호출되어,
 * 비싼 vtophys 트리 등록 작업을 생략한다(예: bdev_malloc 단독 시 메모리 절약).
 */
void mem_disable_vtophys(void);

/**
 * Enforce socket ID allocations.
 */
/*
 * [한국어]
 * mem_enforce_numa - NUMA 강제 할당 모드 켜기
 *
 * 일반적으로 SPDK는 요청된 NUMA 노드에서 할당이 실패하면 SOCKET_ID_ANY로 fallback하지만,
 * 본 함수가 호출되면 fallback을 금지하고 지정 NUMA에서 실패 시 그대로 NULL을 반환한다.
 * 멀티 NUMA 시스템에서 latency를 엄격히 관리해야 할 때 사용.
 */
void mem_enforce_numa(void);

#endif
