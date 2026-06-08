/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2015 Intel Corporation.
 * Copyright 2013-2014 6WIND S.A.
 */

/*
 * [한국어 설명] DPDK 22.07 vendored PCI 버스 디바이스·드라이버 인터페이스 헤더 (22.07/rte_bus_pci.h)
 *
 * === 파일의 역할 ===
 * DPDK 22.07 버전의 PCI 버스 추상화를 구체화하는 헤더. rte_pci_device(BDF/ID/BAR/인터럽트),
 * rte_pci_driver(probe/remove/dma_map 콜백), rte_pci_bus(디바이스·드라이버 TAILQ) 구조체를 정의.
 * pci_dpdk_2207.c가 이 헤더를 포함하여 rte_pci_device의 개별 필드에 직접 접근한다.
 * 22.11 이후에는 rte_pci_driver가 내부 헤더로 이동하여 이 헤더가 22.07 전용으로 vendored됨.
 * SPDK는 이 구조체 레이아웃이 22.07 ABI와 정확히 일치하는지에 의존하여 동작한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * [SPDK pci.c] → [pci_dpdk_2207.c / pci_dpdk.c fn_table] → [이 헤더의 rte_pci_device 접근]
 * pci_dpdk_2207.c의 dpdk_pci_device_get_addr/id/resource/kdrv 함수들이 이 헤더의 필드 오프셋에 의존.
 * DPDK 22.07 바이너리가 링크될 때 rte_pci_device 레이아웃이 동일해야 필드 접근이 정확하다.
 * 실행 컨텍스트: 호스트 유저스페이스; EAL PCI bus 초기화 + SPDK pci.c probe 경로.
 *
 * === 타 모듈과의 연결 ===
 * - 포함 관계: pci_dpdk_2207.c가 이 헤더를 포함. rte_dev.h/rte_bus.h에 의존.
 * - rte_pci.h(DPDK 공개 헤더)에서 rte_pci_addr/rte_pci_id/PCI_MAX_RESOURCE 등 공유.
 * - SPDK pci.c: map_bar_rte가 dev->mem_resource[bar]로 BAR 주소·길이 접근.
 * - SPDK pci.c: cfg_read_rte/cfg_write_rte가 rte_pci_read/write_config 호출.
 * - SPDK pci.c: remove_rte_dev가 rte_pci_driver의 remove 콜백 호출.
 * - memory.c: DMA 매핑 경로에서 rte_pci_driver의 dma_map/dma_unmap 콜백 경유.
 * - sigbus_handler.c: rte_pci_bus.bus.sigbus_handler를 통해 BAR 폴트 주소 확인.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct rte_pci_device: BDF주소/PCI ID/BAR 배열/인터럽트/드라이버/kdrv/이름을 포함하는 PCI 디바이스 기술자.
 * - struct rte_pci_driver: probe/remove/dma_map 콜백 + id_table + drv_flags로 구성되는 PCI 드라이버.
 * - struct rte_pci_bus: rte_bus 상속 + device_list/driver_list TAILQ 보유.
 * - rte_pci_map_device/unmap_device: BAR mmap()/munmap() 수행. SPDK map_bar_rte가 간접 의존.
 * - rte_pci_read_config/write_config: PCI Config Space R/W. SPDK cfg_read_rte/cfg_write_rte 사용.
 * - RTE_DEV_TO_PCI: rte_device → rte_pci_device container_of 변환 매크로.
 */

#ifndef _RTE_BUS_PCI_H_
#define _RTE_BUS_PCI_H_

/**
 * @file
 * PCI device & driver interface
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>       /* [한국어] FILE* 타입 (rte_pci_dump 인자). 표준 I/O. */
#include <stdlib.h>      /* [한국어] 표준 라이브러리 (size_t, NULL 등). */
#include <limits.h>      /* [한국어] PATH_MAX 등 시스템 한계값 상수. */
#include <errno.h>       /* [한국어] errno 및 표준 에러 코드 (ENOMEM, EINVAL 등). */
#include <stdint.h>      /* [한국어] uint64_t 등 고정 크기 정수 타입 (IOVA 주소, BAR 길이). */
#include <inttypes.h>    /* [한국어] PRIx64 등 printf 포맷 매크로. */

#include <rte_debug.h>      /* [한국어] RTE_VERIFY, rte_panic 등 디버그 assertion 매크로. */
#include <rte_interrupts.h> /* [한국어] rte_intr_handle 타입 정의. PCI MSI-X 인터럽트 핸들 관리. */
#include "rte_dev.h"        /* [한국어] rte_device/rte_driver/rte_mem_resource 기본 구조체. */
#include "rte_bus.h"        /* [한국어] rte_bus 추상화 인터페이스. rte_pci_bus가 이를 상속. */
#include <rte_pci.h>        /* [한국어] rte_pci_addr(BDF), rte_pci_id(vendor/device ID),
                              *          PCI_MAX_RESOURCE, PCI_PRI_STR_SIZE, RTE_PCI_ANY_ID 등. */

/** Pathname of PCI devices directory. */
/* [한국어] rte_pci_get_sysfs_path - Linux sysfs PCI 디바이스 디렉토리 경로 반환.
 * 반환: "/sys/bus/pci/devices" 또는 DPDK_PCI_BUS_SYSFS_PATH 환경변수로 오버라이드된 경로.
 * 동작: PCI 버스 scan() 시 이 경로를 기반으로 BDF 디렉토리를 열거.
 * SPDK init.c의 빌드 인자 경로와 무관하게 DPDK 내부 PCI 스캔에 사용. */
const char *rte_pci_get_sysfs_path(void);

/* Forward declarations */
struct rte_pci_device;  /* [한국어] 순환 참조 방지용 전방 선언. 실제 정의는 아래에 있음. */
struct rte_pci_driver;  /* [한국어] rte_pci_device가 driver 포인터를 멤버로 포함하여 교차 참조. */

/** List of PCI devices */
/* [한국어] rte_pci_device_list - 스캔된 모든 PCI 디바이스를 연결하는 TAILQ 헤드 타입.
 * rte_pci_bus.device_list가 이 타입으로 실제 목록을 보유.
 * FOREACH_DEVICE_ON_PCIBUS 매크로로 순회하여 BDF별 디바이스 처리. */
RTE_TAILQ_HEAD(rte_pci_device_list, rte_pci_device);
/** List of PCI drivers */
/* [한국어] rte_pci_driver_list - 등록된 모든 PCI 드라이버를 연결하는 TAILQ 헤드 타입.
 * rte_pci_bus.driver_list가 이 타입으로 실제 목록을 보유.
 * probe 시 driver_list를 순회하며 vendor/device ID로 디바이스와 매칭. */
RTE_TAILQ_HEAD(rte_pci_driver_list, rte_pci_driver);

/* PCI Bus iterators */
/* [한국어] FOREACH_DEVICE_ON_PCIBUS - rte_pci_bus.device_list의 모든 PCI 디바이스를 순회하는 for 루프 매크로.
 * p: rte_pci_device* 루프 변수.
 * SPDK pci.c는 이 매크로보다 dpdk_fn_table의 find_device 경유를 선호하나, DPDK 내부에서 광범위 사용. */
#define FOREACH_DEVICE_ON_PCIBUS(p)	\
		RTE_TAILQ_FOREACH(p, &(rte_pci_bus.device_list), next)

/* [한국어] FOREACH_DRIVER_ON_PCIBUS - rte_pci_bus.driver_list의 모든 PCI 드라이버를 순회하는 for 루프 매크로.
 * p: rte_pci_driver* 루프 변수.
 * probe() 구현에서 드라이버 목록을 순회하며 ID 매칭에 사용. */
#define FOREACH_DRIVER_ON_PCIBUS(p)	\
		RTE_TAILQ_FOREACH(p, &(rte_pci_bus.driver_list), next)

struct rte_devargs;  /* [한국어] devargs 파싱 결과 구조체. 순환 참조 방지 전방 선언. */

/*
 * [한국어]
 * enum rte_pci_kernel_driver - PCI 디바이스에 현재 바인딩된 커널 드라이버 종류
 *
 * DPDK PCI 스캔 시 /sys/bus/pci/devices/<BDF>/driver 심볼릭 링크를 읽어 이 값을 결정.
 * kdrv 값에 따라 VFIO 또는 UIO 경로 중 어느 쪽으로 BAR 매핑 및 DMA를 처리할지 결정.
 * SPDK pci.c의 pci_dpdk_2207.c가 dpdk_pci_device_get_kdrv()로 이 값을 조회.
 */
enum rte_pci_kernel_driver {
	RTE_PCI_KDRV_UNKNOWN = 0,
	/* [한국어] 미확인 드라이버 — UIO 계열이거나 bifurcated(커널+유저 공유) 드라이버.
	 * 스캔 중 driver 링크 대상이 알려진 목록에 없을 때 이 값으로 설정. */

	RTE_PCI_KDRV_IGB_UIO,
	/* [한국어] igb_uio — Intel 제공 커널 UIO 드라이버.
	 * 권장 deprecated 상태; 구형 DPDK 환경 또는 VFIO 미지원 커널에서 사용. */

	RTE_PCI_KDRV_VFIO,
	/* [한국어] vfio-pci — IOMMU 기반 안전한 userspace PCI 접근 드라이버 (권장).
	 * SPDK의 기본 권장 드라이버. IOMMU 그룹 + VFIO 컨테이너로 DMA 격리.
	 * memory.c의 vfio_cfg(VFIO DMA 매핑)가 이 경로와 연동. */

	RTE_PCI_KDRV_UIO_GENERIC,
	/* [한국어] uio_pci_generic — 커널 내장 범용 UIO 드라이버.
	 * IOMMU 없이 물리 주소 직접 DMA. 보안 수준은 낮으나 별도 모듈 빌드 불필요. */

	RTE_PCI_KDRV_NIC_UIO,
	/* [한국어] nic_uio — FreeBSD용 UIO 드라이버. Linux 환경에서는 미사용. */

	RTE_PCI_KDRV_NONE,
	/* [한국어] 커널 드라이버 없음 — 디바이스가 현재 어떤 드라이버에도 바인딩되지 않은 상태.
	 * rte_eal_hotplug_add 직후 probe 전 단계에서 일시적으로 이 값일 수 있음. */

	RTE_PCI_KDRV_NET_UIO,
	/* [한국어] NetUIO — Windows용 UIO 드라이버. Windows DPDK 포팅 전용. */
};

/**
 * A structure describing a PCI device.
 */
/*
 * [한국어]
 * struct rte_pci_device - DPDK PCI 디바이스 기술자
 *
 * PCI 버스 스캔으로 발견된 각 디바이스를 나타내는 핵심 구조체.
 * rte_device를 상속(내장)하여 범용 버스 인터페이스와 통합되며,
 * BDF 주소·PCI ID·BAR 배열·인터럽트 핸들·kdrv 등 디바이스 모든 정보를 보유.
 * pci_dpdk_2207.c가 dpdk_pci_device_get_addr/id/resource/kdrv 함수로 필드에 직접 접근.
 * SPDK pci.c는 map_bar_rte에서 mem_resource[bar]로 BAR mmap 주소·크기를 가져온다.
 */
struct rte_pci_device {
	RTE_TAILQ_ENTRY(rte_pci_device) next;
	/* [한국어] rte_pci_bus.device_list TAILQ에 연결되는 링크 포인터.
	 * 설정자: pci_scan_one()이 새 디바이스를 발견할 때 목록 꼬리에 삽입.
	 * 읽는 자: FOREACH_DEVICE_ON_PCIBUS 매크로, probe/find_device 순회.
	 * 값 범위: 유효한 다음 rte_pci_device 포인터 또는 목록 끝.
	 * 동기화: PCI 버스 내부 목록 락으로 보호. */

	struct rte_device device;
	/* [한국어] 범용 rte_device 기반 구조체 (이름/드라이버/버스/NUMA/devargs 포함).
	 * 설정자: pci_scan_one()이 BDF 이름, bus 포인터 등을 채워 설정.
	 * 읽는 자: RTE_DEV_TO_PCI 매크로로 container_of를 통해 rte_pci_device 복원.
	 *           rte_bus_find_by_device() 등 범용 버스 API가 이 필드로 버스 조회.
	 * 값 범위: 유효한 rte_device. device.driver는 probe 완료 후 설정.
	 * 동기화: probe/remove 과정에서 PCI 버스 내부 락이 device.driver 접근 보호. */

	struct rte_pci_addr addr;
	/* [한국어] PCI 위치 주소 — domain:bus:devid.function (BDF).
	 * 설정자: pci_scan_one()이 /sys/bus/pci/devices/<BDF> 이름을 파싱하여 설정.
	 * 읽는 자: pci_dpdk_2207.c의 dpdk_pci_device_get_addr; SPDK pci.c의 주소 비교.
	 * 값 범위: 0000:00:00.0 ~ ffff:ff:1f.7 범위의 유효한 BDF.
	 * 동기화: 스캔 완료 후 불변. */

	struct rte_pci_id id;
	/* [한국어] PCI 식별자 — class_id/vendor_id/device_id/subsystem_vendor_id/subsystem_device_id.
	 * 설정자: pci_scan_one()이 sysfs의 vendor/device/class 파일에서 읽어 설정.
	 * 읽는 자: probe() 시 드라이버 id_table과 비교하여 매칭; dpdk_pci_device_get_id 호출자.
	 * 값 범위: 각 필드는 16비트 또는 24비트. RTE_PCI_ANY_ID(0xffff)는 와일드카드.
	 * 동기화: 스캔 완료 후 불변. */

	struct rte_mem_resource mem_resource[PCI_MAX_RESOURCE];
	/* [한국어] PCI BAR(Base Address Register) 자원 배열. PCI_MAX_RESOURCE(6) 개.
	 * 설정자: pci_uio_map_resource 또는 pci_vfio_map_resource가 BAR mmap 후 addr 설정.
	 * 읽는 자: SPDK pci.c map_bar_rte → mem_resource[bar].addr/len으로 BAR 가상 주소·크기 획득.
	 *           NVMe 드라이버는 BAR0(NVMe CC/CSTS/doorbell 레지스터)에 접근.
	 * 값 범위: phys_addr=PCI BAR 물리 주소, len=BAR 크기, addr=mmap 후 가상 주소(NULL=미매핑).
	 * 동기화: probe 완료 후 addr 접근은 단일 스레드(reactor) 전담. */

	struct rte_intr_handle *intr_handle;
	/* [한국어] PCI 인터럽트 핸들 (MSI/MSI-X 포함).
	 * 설정자: pci_vfio_enable_intr 또는 pci_uio_alloc_resource가 할당 후 설정.
	 * 읽는 자: rte_intr_enable/disable 계열 함수; NVMe 완료 인터럽트 처리(폴드 모드에서는 미사용).
	 * 값 범위: 유효한 rte_intr_handle 포인터 또는 NULL(인터럽트 미사용).
	 * 동기화: rte_intr_handle 내부 락으로 보호. */

	struct rte_pci_driver *driver;
	/* [한국어] 이 디바이스를 probe한 PCI 드라이버 포인터.
	 * 설정자: probe() 성공 시 드라이버 포인터 설정; remove() 후 NULL로 초기화.
	 * 읽는 자: rte_eal_hotplug_remove 경로에서 remove 콜백 호출; 드라이버 존재 확인.
	 * 값 범위: 유효한 rte_pci_driver 포인터 또는 NULL(probe 전/후).
	 * 동기화: PCI 버스 내부 락으로 설정/초기화 보호. */

	uint16_t max_vfs;
	/* [한국어] SR-IOV(Single Root I/O Virtualization) 가상 함수 최대 수.
	 * 설정자: pci_scan_one()이 sysfs의 sriov_numvfs를 읽어 설정.
	 * 읽는 자: SR-IOV 드라이버가 VF(Virtual Function) 생성 수 결정에 사용.
	 * 값 범위: 0(SR-IOV 미지원 또는 비활성), 1 이상(VF 허용 수).
	 * 동기화: 스캔 완료 후 불변. */

	enum rte_pci_kernel_driver kdrv;
	/* [한국어] 현재 바인딩된 커널 드라이버 종류 (VFIO/UIO_GENERIC/IGB_UIO 등).
	 * 설정자: pci_scan_one()이 /sys/bus/pci/devices/<BDF>/driver 링크 대상을 분석.
	 * 읽는 자: pci_dpdk_2207.c dpdk_pci_device_get_kdrv; SPDK pci.c가 VFIO/UIO 경로 결정.
	 * 값 범위: rte_pci_kernel_driver 열거형 값 중 하나.
	 * 동기화: 스캔 완료 후 probe 전까지는 불변. hotplug 후 재스캔 시 갱신 가능. */

	char name[PCI_PRI_STR_SIZE+1];
	/* [한국어] PCI 위치 ASCII 문자열 (예: "0000:04:00.0\0").
	 * 설정자: pci_scan_one()이 snprintf(PCI_PRI_FMT)로 BDF 주소를 문자열로 채움.
	 * 읽는 자: SPDK pci.c의 로그 출력; rte_device.name(이 배열의 주소)으로 디바이스 이름 노출.
	 * 값 범위: "DDDD:BB:DD.F" 형식 12자 + NULL.
	 * 동기화: 스캔 완료 후 불변. */

	struct rte_intr_handle *vfio_req_intr_handle;
	/* [한국어] VFIO 요청 인터럽트(REQ) 핸들 — PCIe FLR(Function Level Reset) 요청 처리용.
	 * 설정자: VFIO probe 시 VFIO_PCI_REQ_IRQ_INDEX 인터럽트 설정 후 할당.
	 * 읽는 자: PCIe surprise removal/FLR 이벤트 감지 시 sigbus_handler 경로와 연동.
	 * 값 범위: 유효한 rte_intr_handle 포인터 또는 NULL(VFIO 미사용 또는 REQ IRQ 미지원).
	 * 동기화: rte_intr_handle 내부 락으로 보호. */
};

/**
 * @internal
 * Helper macro for drivers that need to convert to struct rte_pci_device.
 */
/* [한국어] RTE_DEV_TO_PCI - rte_device 포인터를 rte_pci_device 포인터로 변환하는 container_of 매크로.
 * ptr: rte_device* (rte_pci_device.device 필드의 주소).
 * 동작: container_of(ptr, struct rte_pci_device, device) 수행.
 * SPDK pci.c와 pci_dpdk_2207.c에서 rte_bus 콜백(probe/remove 등)에서 rte_device를 rte_pci_device로 복원. */
#define RTE_DEV_TO_PCI(ptr) container_of(ptr, struct rte_pci_device, device)

/* [한국어] RTE_DEV_TO_PCI_CONST - const rte_device* → const rte_pci_device* 변환.
 * 읽기 전용 컨텍스트(cfg_read 등)에서 const 안전성을 유지한 채 변환. */
#define RTE_DEV_TO_PCI_CONST(ptr) \
	container_of(ptr, const struct rte_pci_device, device)

/* [한국어] RTE_ETH_DEV_TO_PCI - rte_eth_dev → rte_pci_device 변환 매크로.
 * eth_dev->device(rte_device*)를 거쳐 RTE_DEV_TO_PCI를 적용. NIC 드라이버용; SPDK는 직접 미사용. */
#define RTE_ETH_DEV_TO_PCI(eth_dev)	RTE_DEV_TO_PCI((eth_dev)->device)

#ifdef __cplusplus
/** C++ macro used to help building up tables of device IDs */
/* [한국어] RTE_PCI_DEVICE (C++ 버전) - PCI 드라이버의 id_table 초기화용 매크로.
 * vend: vendor_id (예: 0x8086 Intel), dev: device_id (예: 0x0953 NVMe SSD).
 * class_id=ANY, subsystem_vendor/device=ANY 로 폭넓은 매칭.
 * rte_pci_driver.id_table[]에 엔트리를 생성하여 probe 시 이 ID에 매칭되는 디바이스와 연결. */
#define RTE_PCI_DEVICE(vend, dev) \
	RTE_CLASS_ANY_ID,         \
	(vend),                   \
	(dev),                    \
	RTE_PCI_ANY_ID,           \
	RTE_PCI_ANY_ID
#else
/** Macro used to help building up tables of device IDs */
/* [한국어] RTE_PCI_DEVICE (C 버전) - PCI 드라이버의 id_table 구조체 지시자 초기화.
 * vend: vendor_id, dev: device_id. class_id=ANY_ID, subsystem=ANY_ID로 넓은 매칭.
 * SPDK NVMe 드라이버가 0x8086 등 vendor + device_id로 NVMe 디바이스를 등록할 때 사용. */
#define RTE_PCI_DEVICE(vend, dev)          \
	.class_id = RTE_CLASS_ANY_ID,      \
	.vendor_id = (vend),               \
	.device_id = (dev),                \
	.subsystem_vendor_id = RTE_PCI_ANY_ID, \
	.subsystem_device_id = RTE_PCI_ANY_ID
#endif

/**
 * Initialisation function for the driver called during PCI probing.
 */
/* [한국어] rte_pci_probe_t - PCI 드라이버 probe 함수 타입.
 * 첫 번째 인자: 드라이버 포인터(드라이버 전역 설정 접근용).
 * 두 번째 인자: probe 대상 rte_pci_device 포인터.
 * 반환: 0 성공, 음수 실패.
 * 동작: BAR mmap, 링 초기화, DMA 메모리 할당 등 디바이스 초기화 수행.
 * SPDK NVMe 드라이버의 probe 함수가 이 타입으로 rte_pci_driver.probe에 등록됨. */
typedef int (rte_pci_probe_t)(struct rte_pci_driver *, struct rte_pci_device *);

/**
 * Uninitialisation function for the driver called during hotplugging.
 */
/* [한국어] rte_pci_remove_t - PCI 드라이버 remove 함수 타입. hot-unplug 또는 spdk_nvme_detach 시 호출.
 * dev: 분리할 rte_pci_device 포인터.
 * 반환: 0 성공, 음수 실패.
 * 동작: I/O 큐 제거, BAR unmap, 드라이버 내부 상태 정리.
 * SPDK NVMe 드라이버의 remove 함수가 rte_pci_driver.remove에 등록됨. */
typedef int (rte_pci_remove_t)(struct rte_pci_device *);

/**
 * Driver-specific DMA mapping. After a successful call the device
 * will be able to read/write from/to this segment.
 *
 * @param dev
 *   Pointer to the PCI device.
 * @param addr
 *   Starting virtual address of memory to be mapped.
 * @param iova
 *   Starting IOVA address of memory to be mapped.
 * @param len
 *   Length of memory segment being mapped.
 * @return
 *   - 0 On success.
 *   - Negative value and rte_errno is set otherwise.
 */
/* [한국어] pci_dma_map_t - PCI 드라이버 레벨 DMA 매핑 함수 타입.
 * dev: 매핑 대상 rte_pci_device. addr: 가상 주소. iova: IOVA 주소. len: 길이.
 * 반환: 0 성공, 음수+rte_errno 실패.
 * 동작: VFIO 디바이스의 경우 VFIO_IOMMU_MAP_DMA ioctl 수행.
 * memory.c가 hugepage 등록 시 이 경로를 통해 모든 probe된 디바이스에 DMA 매핑 추가. */
typedef int (pci_dma_map_t)(struct rte_pci_device *dev, void *addr,
			    uint64_t iova, size_t len);

/**
 * Driver-specific DMA un-mapping. After a successful call the device
 * will not be able to read/write from/to this segment.
 *
 * @param dev
 *   Pointer to the PCI device.
 * @param addr
 *   Starting virtual address of memory to be unmapped.
 * @param iova
 *   Starting IOVA address of memory to be unmapped.
 * @param len
 *   Length of memory segment being unmapped.
 * @return
 *   - 0 On success.
 *   - Negative value and rte_errno is set otherwise.
 */
/* [한국어] pci_dma_unmap_t - PCI 드라이버 레벨 DMA 언매핑 함수 타입.
 * dev: 언매핑 대상 rte_pci_device. addr: 가상 주소. iova: IOVA 주소. len: 길이.
 * 반환: 0 성공, 음수+rte_errno 실패.
 * 동작: VFIO_IOMMU_UNMAP_DMA ioctl 수행. 해당 영역을 사용하는 I/O 완료 후에만 호출. */
typedef int (pci_dma_unmap_t)(struct rte_pci_device *dev, void *addr,
			      uint64_t iova, size_t len);

/**
 * A structure describing a PCI driver.
 */
/*
 * [한국���]
 * struct rte_pci_driver - DPDK PCI 드라이버 기술자
 *
 * rte_pci_bus.driver_list에 등록된 각 PCI 드라이버를 나타내는 구조체.
 * probe/remove/dma_map/dma_unmap 콜백 + id_table로 드라이버가 지원하는 디바이스 정의.
 * SPDK NVMe 드라이버가 이 구조체를 통해 NVMe PCI 디바이스와 바인딩된다.
 * pci_dpdk_2207.c의 dpdk_pci_driver_register()가 이 구조체를 rte_pci_bus에 등록.
 * 주의: 22.11 이후 이 구조체는 내부 헤더로 이동 — 이 헤더는 22.07 전용.
 */
struct rte_pci_driver {
	RTE_TAILQ_ENTRY(rte_pci_driver) next;
	/* [한국어] rte_pci_bus.driver_list TAILQ 링크.
	 * 설정자: rte_pci_register()가 드라이버를 목록 꼬리에 삽입할 때 설정.
	 * 읽는 자: FOREACH_DRIVER_ON_PCIBUS가 probe 시 드라이버 목록을 순회.
	 * 동기화: PCI 버스 내부 목록 락으로 보호. */

	struct rte_driver driver;
	/* [한국어] 범용 rte_driver 기반 구조체 (name/alias 포함).
	 * 설정자: RTE_PMD_REGISTER_PCI 매크로가 생성자 함수에서 driver.name을 설정.
	 * 읽는 자: 로그 출력, 드라이버 이름 기반 탐색.
	 * 동기화: 초기화 완료 후 불변. */

	struct rte_pci_bus *bus;
	/* [한국어] 이 드라이버가 등록된 PCI 버스 참조 포인터.
	 * 설정자: rte_pci_register()가 등록 시 &rte_pci_bus를 저장.
	 * 읽는 자: 드라이버 내부에서 버스 수준 API 호출에 사용.
	 * 동기화: 등록 후 불변. */

	rte_pci_probe_t *probe;
	/* [한국어] 디바이스 초기화(probe) 콜백 함수 포인터.
	 * 설정자: 드라이버 구현체가 정적 초기화 시 설정 (예: nvme_pci_probe).
	 * 읽는 자: rte_bus_probe() → PCI probe → plug() → 이 콜백 호출.
	 * 값 범위: 유효한 함수 포인터 (필수).
	 * 동기화: 불변. */

	rte_pci_remove_t *remove;
	/* [한국어] 디바이스 분리(remove) 콜백 함수 포인터.
	 * 설정자: 드라이버 구현체가 정적 초기화 시 설정 (예: nvme_pci_remove).
	 * 읽는 자: rte_eal_hotplug_remove → unplug() → 이 콜백 호출.
	 * 값 범위: NULL 가능 (hot-remove 미지원 드라이버).
	 * 동기화: 불변. */

	pci_dma_map_t *dma_map;
	/* [한국어] 드라이버 레벨 DMA 매핑 콜백. memory.c의 메모리 등록 경로에서 사용.
	 * 설정자: VFIO 기반 드라이버가 pci_dma_map 함수를 등록.
	 * 읽는 자: rte_bus.dma_map → rte_pci_driver.dma_map 순으로 호출.
	 * 값 범위: VFIO 미사용 드라이버에서는 NULL.
	 * 동기화: 불변 포인터; 실제 VFIO ioctl은 g_vfio.mutex 보호. */

	pci_dma_unmap_t *dma_unmap;
	/* [한국어] 드라이버 레벨 DMA 언매핑 콜백.
	 * 설정자: VFIO 기반 드라이버가 pci_dma_unmap 함수를 등록.
	 * 읽는 자: rte_bus.dma_unmap → rte_pci_driver.dma_unmap 순으로 호출.
	 * 값 범위: dma_map과 동일하게 NULL 가능.
	 * 동기화: dma_map과 동일. */

	const struct rte_pci_id *id_table;
	/* [한국어] 드라이버가 지원하는 PCI ID 목록 배열 (NULL 엔트리로 끝남).
	 * 설정자: 드라이버 구현체가 정적 배열로 초기화 (예: nvme_pci_id_table[]).
	 * 읽는 자: probe() 시 디바이스의 rte_pci_id와 비교하여 드라이버 매칭 결정.
	 * 값 범위: 하나 이상의 rte_pci_id 엔트리 + {0,0,0,0,0} 종료 엔트리 배열.
	 * 동기화: 불변 정적 배열. */

	uint32_t drv_flags;
	/* [한국어] 드라이버 동작 플래그 비트 조합 (RTE_PCI_DRV_* 상수 OR).
	 * 설정자: 드라이버 구현체가 정적 초기화 시 설정.
	 * 읽는 자: probe() 시 BAR 매핑 필요 여부(NEED_MAPPING), WC 활성화 여부 등 결정.
	 * 값 범위: RTE_PCI_DRV_NEED_MAPPING(0x1)/WC_ACTIVATE(0x2)/INTR_LSC(0x8)/
	 *           INTR_RMV(0x10)/KEEP_MAPPED_RES(0x20)/NEED_IOVA_AS_VA(0x40) 조합.
	 * 동기화: 초기화 후 불변. */
};

/**
 * Structure describing the PCI bus
 */
/*
 * [한국어]
 * struct rte_pci_bus - DPDK PCI 버스 구현 구조체
 *
 * rte_bus를 상속(내장)하여 PCI 버스 고유의 디바이스·드라이버 목록을 추가로 보유.
 * 전역 변수 rte_pci_bus(DPDK 내부)가 이 구조체의 단일 인스턴스이며 EAL에 자동 등록됨.
 * FOREACH_DEVICE/DRIVER_ON_PCIBUS 매크로가 이 구조체의 두 TAILQ를 직접 참조.
 */
struct rte_pci_bus {
	struct rte_bus bus;
	/* [한국어] 범용 rte_bus 기반 구조체 — 버스 이름/scan/probe/dma_map/sigbus_handler 등 포함.
	 * 설정자: PCI 버스 구현체가 정적 초기화 시 모든 콜백 포인터 설정.
	 * 읽는 자: rte_bus_register/find/scan/probe 등 범용 버스 API가 이 필드를 통해 접근.
	 * RTE_REGISTER_BUS(pci, rte_pci_bus.bus) 매크로로 EAL 버스 레지스트리에 등록.
	 * 동기화: 초기화 후 콜백 포인터는 불변. */

	struct rte_pci_device_list device_list;
	/* [한국어] 스캔으로 발견된 rte_pci_device 객체의 TAILQ.
	 * 설정자: pci_scan_one()이 새 디바이스를 꼬리에 TAILQ_INSERT_TAIL.
	 * 읽는 자: FOREACH_DEVICE_ON_PCIBUS / pci_probe / pci_find_device.
	 * 값 범위: 0개 이상의 rte_pci_device 노드.
	 * 동기화: PCI 버스 내부 락(pthread_mutex)으로 삽입/제거 시 보호. */

	struct rte_pci_driver_list driver_list;
	/* [한국어] rte_pci_register()로 등록된 rte_pci_driver 객체의 TAILQ.
	 * 설정자: rte_pci_register()가 드라이버를 TAILQ_INSERT_TAIL.
	 * 읽는 자: FOREACH_DRIVER_ON_PCIBUS / probe 시 ID 매칭.
	 * 값 범위: 0개 이상의 rte_pci_driver 노드.
	 * 동기화: 드라이버 등록은 EAL 초기화 전 생성자 함수에서 일어나므로 대부분 락 불필요.
	 *          hot-register 시에는 버스 내부 락 사용. */
};

/** Device needs PCI BAR mapping (done with either IGB_UIO or VFIO) */
/* [한국어] RTE_PCI_DRV_NEED_MAPPING - probe 시 BAR 자원 자동 mmap 요청 플래그.
 * 이 비트가 설정된 드라이버는 EAL probe 경로에서 rte_pci_map_device()를 자동 호출.
 * SPDK NVMe 드라이버는 이 플래그를 설정하여 BAR0(NVMe CC/CSTS/doorbell)를 mmap. */
#define RTE_PCI_DRV_NEED_MAPPING 0x0001
/** Device needs PCI BAR mapping with enabled write combining (wc) */
/* [한국어] RTE_PCI_DRV_WC_ACTIVATE - BAR mmap 시 Write Combining 활성화.
 * PCIe 쓰기 요청을 버퍼링하여 도어벨 레지스터 쓰기 성능 향상.
 * NVMe 도어벨(SQ tail/CQ head 갱신)에 유효. SPDK는 선택적 설정. */
#define RTE_PCI_DRV_WC_ACTIVATE 0x0002
/** Device already probed can be probed again to check for new ports. */
/* [한국어] RTE_PCI_DRV_PROBE_AGAIN - 이미 probe된 디바이스도 재probe 허용.
 * 동적 포트 추가를 지원하는 NIC 드라이버용. SPDK NVMe에서는 일반적으로 미사용. */
#define RTE_PCI_DRV_PROBE_AGAIN 0x0004
/** Device driver supports link state interrupt */
/* [한국어] RTE_PCI_DRV_INTR_LSC - 링크 상태 변경 인터럽트(LSC) 지원.
 * NIC 드라이버의 링크 UP/DOWN 감지용. NVMe에서는 해당 없음. */
#define RTE_PCI_DRV_INTR_LSC	0x0008
/** Device driver supports device removal interrupt */
/* [한국어] RTE_PCI_DRV_INTR_RMV - 디바이스 제거 인터럽트(RMV) 지원.
 * PCIe surprise removal 감지 시 인터럽트 기반 통보. sigbus_handler와 연동 가능. */
#define RTE_PCI_DRV_INTR_RMV 0x0010
/** Device driver needs to keep mapped resources if unsupported dev detected */
/* [한국어] RTE_PCI_DRV_KEEP_MAPPED_RES - 미지원 디바이스 감지 시 BAR 매핑 유지.
 * probe 실패 시에도 mmap을 해제하지 않고 재시도를 허용. 일부 NIC 드라이버 전용. */
#define RTE_PCI_DRV_KEEP_MAPPED_RES 0x0020
/** Device driver needs IOVA as VA and cannot work with IOVA as PA */
/* [한국어] RTE_PCI_DRV_NEED_IOVA_AS_VA - IOVA=VA 모드 필수 드라이버 표시.
 * 이 플래그가 있는 드라이버는 PA 모드(UIO) 환경에서 probe 거부됨.
 * VFIO를 필수로 요구하는 드라이버가 설정. SPDK NVMe는 보통 PA/VA 모두 지원. */
#define RTE_PCI_DRV_NEED_IOVA_AS_VA 0x0040

/**
 * Map the PCI device resources in user space virtual memory address
 *
 * Note that driver should not call this function when flag
 * RTE_PCI_DRV_NEED_MAPPING is set, as EAL will do that for
 * you when it's on.
 *
 * @param dev
 *   A pointer to a rte_pci_device structure describing the device
 *   to use
 *
 * @return
 *   0 on success, negative on error and positive if no driver
 *   is found for the device.
 */
/* [한국어] rte_pci_map_device - PCI BAR 자원을 유저스페이스 가상 주소에 mmap.
 * dev: BAR를 매핑할 rte_pci_device 포인터.
 * 반환: 0 성공, 음수 에러, 양수(드라이버 없음).
 * 동작: VFIO/UIO 경로에 따라 /dev/vfio/<group> 또는 /dev/uio<n>의 BAR 영역을 mmap.
 *       결과는 dev->mem_resource[i].addr에 저장.
 * SPDK pci.c: map_bar_rte가 dpdk_fn_table.map_bar()를 통해 이 함수를 간접 호출. */
int rte_pci_map_device(struct rte_pci_device *dev);

/**
 * Unmap this device
 *
 * @param dev
 *   A pointer to a rte_pci_device structure describing the device
 *   to use
 */
/* [한국어] rte_pci_unmap_device - PCI BAR mmap 해제.
 * dev: BAR 매핑을 해제할 rte_pci_device 포인터.
 * 동작: dev->mem_resource[i].addr에 대해 munmap 수행 후 addr=NULL 설정.
 * SPDK pci.c: unmap_bar_rte가 dpdk_fn_table.unmap_bar()를 통해 간접 호출. */
void rte_pci_unmap_device(struct rte_pci_device *dev);

/**
 * Dump the content of the PCI bus.
 *
 * @param f
 *   A pointer to a file for output
 */
/* [한국어] rte_pci_dump - PCI 버스의 모든 디바이스 정보를 출력.
 * f: 출력 파일 스트림.
 * 동작: FOREACH_DEVICE_ON_PCIBUS로 순회하며 BDF/ID/kdrv/BAR 정보 출력.
 * 진단/디버깅 목적으로 사용. */
void rte_pci_dump(FILE *f);

/**
 * Find device's extended PCI capability.
 *
 *  @param dev
 *    A pointer to rte_pci_device structure.
 *
 *  @param cap
 *    Extended capability to be found, which can be any from
 *    RTE_PCI_EXT_CAP_ID_*, defined in librte_pci.
 *
 *  @return
 *  > 0: The offset of the next matching extended capability structure
 *       within the device's PCI configuration space.
 *  < 0: An error in PCI config space read.
 *  = 0: Device does not support it.
 */
/* [한국어] rte_pci_find_ext_capability - PCI 확장 Capability 구조체의 Config Space 오프셋 탐색.
 * dev: 탐색 대상 rte_pci_device. cap: RTE_PCI_EXT_CAP_ID_* 확장 Capability ID.
 * 반환: > 0 해당 Capability 구조체의 Config Space 오프셋, = 0 미지원, < 0 Config Space 읽기 에러.
 * 동작: PCI Express Config Space의 확장 Capability 연결 리스트(오프셋 0x100~)를 순회.
 * SPDK에서 ARI(Alternative Routing ID), SRIOV 등 고급 PCIe 기능 탐색에 사용 가능. */
__rte_experimental
off_t rte_pci_find_ext_capability(struct rte_pci_device *dev, uint32_t cap);

/**
 * Enables/Disables Bus Master for device's PCI command register.
 *
 *  @param dev
 *    A pointer to rte_pci_device structure.
 *  @param enable
 *    Enable or disable Bus Master.
 *
 *  @return
 *  0 on success, -1 on error in PCI config space read/write.
 */
/* [한국어] rte_pci_set_bus_master - PCI Command 레지스터의 Bus Master(BM) 비트 설정/해제.
 * dev: 대상 rte_pci_device. enable: true=BM 활성화, false=BM 비활성화.
 * 반환: 0 성공, -1 Config Space 읽기/쓰기 에러.
 * 동작: PCI Config Space 오프셋 0x04(Command 레지스터)의 비트 2(Bus Master Enable)를 조작.
 * BM 비트가 설정되어야 디바이스가 DMA(메모리 읽기/쓰기)를 시작할 수 있음. NVMe 초기화 필수 단계. */
__rte_experimental
int rte_pci_set_bus_master(struct rte_pci_device *dev, bool enable);

/**
 * Register a PCI driver.
 *
 * @param driver
 *   A pointer to a rte_pci_driver structure describing the driver
 *   to be registered.
 */
/* [한국어] rte_pci_register - rte_pci_bus.driver_list에 PCI 드라이버 등록.
 * driver: 등록할 rte_pci_driver 포인터.
 * 동작: driver_list 꼬리에 TAILQ_INSERT_TAIL 수행 및 driver->bus = &rte_pci_bus 설정.
 * RTE_PMD_REGISTER_PCI 매크로의 생성자 함수에서 호출됨.
 * pci_dpdk_2207.c의 dpdk_pci_driver_register()가 이 함수를 wrapping하여 SPDK 드라이버 등록. */
void rte_pci_register(struct rte_pci_driver *driver);

/** Helper for PCI device registration from driver (eth, crypto) instance */
/* [한국어] RTE_PMD_REGISTER_PCI - 컴파일 타임에 PCI 드라이버를 자동 등록하는 매크로.
 * nm: 드라이버 이름 심볼. pci_drv: rte_pci_driver 구조체 변수.
 * 동작: RTE_INIT(일반 생성자 우선도) 함수를 생성하여 프로그램 시작 시 rte_pci_register 호출.
 * RTE_PMD_EXPORT_NAME으로 드라이버 이름을 ELF 심볼에도 기록. */
#define RTE_PMD_REGISTER_PCI(nm, pci_drv) \
RTE_INIT(pciinitfn_ ##nm) \
{\
	(pci_drv).driver.name = RTE_STR(nm);\
	rte_pci_register(&pci_drv); \
} \
RTE_PMD_EXPORT_NAME(nm, __COUNTER__)

/**
 * Unregister a PCI driver.
 *
 * @param driver
 *   A pointer to a rte_pci_driver structure describing the driver
 *   to be unregistered.
 */
/* [한국어] rte_pci_unregister - rte_pci_bus.driver_list에서 PCI 드라이버 제거.
 * driver: 제거할 rte_pci_driver 포인터.
 * 동작: driver_list에서 TAILQ_REMOVE 수행.
 * 모듈 언로드 또는 동적 드라이버 해제 시 호출. SPDK 정상 종료 시에는 일반적으로 미호출. */
void rte_pci_unregister(struct rte_pci_driver *driver);

/**
 * Read PCI config space.
 *
 * @param device
 *   A pointer to a rte_pci_device structure describing the device
 *   to use
 * @param buf
 *   A data buffer where the bytes should be read into
 * @param len
 *   The length of the data buffer.
 * @param offset
 *   The offset into PCI config space
 * @return
 *  Number of bytes read on success, negative on error.
 */
/* [한국어] rte_pci_read_config - PCI Config Space 읽기.
 * device: 읽기 대상 rte_pci_device. buf: 읽은 데이터 버퍼. len: 읽을 바이트 수. offset: Config Space 오프셋.
 * 반환: 읽은 바이트 수(성공), 음수(에러).
 * 동작: VFIO의 경우 VFIO_PCI_CONFIG_REGION으로 pread; UIO의 경우 /proc/bus/pci/<BDF> 파일 사용.
 * SPDK pci.c: cfg_read_rte → dpdk_fn_table.cfg_read → rte_pci_read_config 호출.
 * 호출 체인: spdk_pci_device_cfg_read → cfg_read_rte → [rte_pci_read_config]. */
int rte_pci_read_config(const struct rte_pci_device *device,
		void *buf, size_t len, off_t offset);

/**
 * Write PCI config space.
 *
 * @param device
 *   A pointer to a rte_pci_device structure describing the device
 *   to use
 * @param buf
 *   A data buffer containing the bytes should be written
 * @param len
 *   The length of the data buffer.
 * @param offset
 *   The offset into PCI config space
 */
/* [한국어] rte_pci_write_config - PCI Config Space 쓰기.
 * device: 쓰기 대상 rte_pci_device. buf: 쓸 데이터 버퍼. len: 바이트 수. offset: Config Space 오프셋.
 * 반환: 쓴 바이트 수(성공), 음수(에러).
 * 동작: rte_pci_read_config와 동일한 경로; pwrite 또는 /proc 파일 쓰기.
 * SPDK pci.c: cfg_write_rte → dpdk_fn_table.cfg_write → rte_pci_write_config 호출. */
int rte_pci_write_config(const struct rte_pci_device *device,
		const void *buf, size_t len, off_t offset);

/**
 * A structure used to access io resources for a pci device.
 * rte_pci_ioport is arch, os, driver specific, and should not be used outside
 * of pci ioport api.
 */
/*
 * [한국어]
 * struct rte_pci_ioport - PCI I/O Port 접근 컨텍스트
 *
 * PCI 레거시 I/O Port(x86 in/out 명령) 또는 memory-mapped I/O에 대한 접근 컨텍스트.
 * NVMe BAR0는 MMIO이므로 SPDK NVMe 드라이버에서는 이 구조체를 사용하지 않는다.
 * 레거시 I/O Port가 필요한 일부 구형 PCI 디바이스 드라이버용.
 */
struct rte_pci_ioport {
	struct rte_pci_device *dev;
	/* [한국어] 이 ioport 객체가 속한 rte_pci_device 역참조 포인터.
	 * 설정자: rte_pci_ioport_map()이 dev를 저장.
	 * 읽는 자: rte_pci_ioport_read/write가 dev의 BAR 정보 조회.
	 * 동기화: ioport 사용 기간 동안 dev는 유효해야 함. */

	uint64_t base;
	/* [한국어] I/O Port 기준 주소 (또는 MMIO mmap 주소).
	 * 설정자: rte_pci_ioport_map()이 BAR에서 추출하여 설정.
	 * 읽는 자: rte_pci_ioport_read/write가 base+offset으로 실제 주소 계산.
	 * 값 범위: x86 I/O Port(0~0xFFFF) 또는 mmap 후 가상 주소. */

	uint64_t len;
	/* [한국어] I/O Port 영역 길이 — memory-mapped 포트에서만 채워짐.
	 * 설정자: rte_pci_ioport_map()이 BAR 크기에서 추출. I/O Port 모드에서는 미사용.
	 * 값 범위: MMIO 모드에서 BAR 크기, I/O Port 모드에서 0. */
};

/**
 * Initialize a rte_pci_ioport object for a pci device io resource.
 *
 * This object is then used to gain access to those io resources (see below).
 *
 * @param dev
 *   A pointer to a rte_pci_device structure describing the device
 *   to use.
 * @param bar
 *   Index of the io pci resource we want to access.
 * @param p
 *   The rte_pci_ioport object to be initialized.
 * @return
 *  0 on success, negative on error.
 */
/* [한국어] rte_pci_ioport_map - PCI BAR I/O 자원을 rte_pci_ioport 객체로 초기화.
 * dev: 대상 rte_pci_device. bar: 접근할 BAR 인덱스 (0~5). p: 초기화할 rte_pci_ioport 포인터.
 * 반환: 0 성공, 음수 에러.
 * SPDK NVMe는 BAR0 MMIO를 mem_resource[0].addr로 직접 접근하므로 이 함수를 미사용. */
int rte_pci_ioport_map(struct rte_pci_device *dev, int bar,
		struct rte_pci_ioport *p);

/**
 * Release any resources used in a rte_pci_ioport object.
 *
 * @param p
 *   The rte_pci_ioport object to be uninitialized.
 * @return
 *  0 on success, negative on error.
 */
/* [한국어] rte_pci_ioport_unmap - rte_pci_ioport 자원 해제.
 * p: 해제할 rte_pci_ioport 포인터.
 * 반환: 0 성공, 음수 에러. */
int rte_pci_ioport_unmap(struct rte_pci_ioport *p);

/**
 * Read from a io pci resource.
 *
 * @param p
 *   The rte_pci_ioport object from which we want to read.
 * @param data
 *   A data buffer where the bytes should be read into
 * @param len
 *   The length of the data buffer.
 * @param offset
 *   The offset into the pci io resource.
 */
/* [한국어] rte_pci_ioport_read - PCI I/O 자원에서 읽기.
 * p: ioport 객체. data: 읽기 버퍼. len: 읽을 크기. offset: I/O 자원 내 오프셋.
 * 동작: x86의 경우 inb/inw/inl 또는 mmap 주소 읽기. */
void rte_pci_ioport_read(struct rte_pci_ioport *p,
		void *data, size_t len, off_t offset);

/**
 * Write to a io pci resource.
 *
 * @param p
 *   The rte_pci_ioport object to which we want to write.
 * @param data
 *   A data buffer where the bytes should be read into
 * @param len
 *   The length of the data buffer.
 * @param offset
 *   The offset into the pci io resource.
 */
/* [한국어] rte_pci_ioport_write - PCI I/O 자원에 쓰기.
 * p: ioport 객체. data: 쓸 데이터. len: 쓸 크기. offset: I/O 자원 내 오프셋.
 * 동작: x86의 경우 outb/outw/outl 또는 mmap 주소 쓰기. */
void rte_pci_ioport_write(struct rte_pci_ioport *p,
		const void *data, size_t len, off_t offset);

#ifdef __cplusplus
}
#endif

#endif /* _RTE_BUS_PCI_H_ */
