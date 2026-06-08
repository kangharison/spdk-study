/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2016 NXP
 */

/*
 * [한국어 설명] DPDK 22.07 vendored 버스 추상화 인터페이스 헤더 (22.07/rte_bus.h)
 *
 * === 파일의 역할 ===
 * DPDK 22.07의 범용 버스(bus) 추상화 계층을 정의한다. rte_bus 구조체와 그 함수 포인터
 * 테이블을 통해 PCI 버스, vdev 버스 등 다양한 버스 유형을 EAL에 동일한 인터페이스로 등록·관리한다.
 * scan/probe/find_device/plug/unplug/dma_map/dma_unmap 콜백으로 버스별 동작을 추상화하며,
 * hot-unplug 및 SIGBUS 에러 처리 핸들러도 버스 단위로 등록한다. SPDK는 PCI 버스 구현을 통해
 * NVMe/RDMA 디바이스를 열거·바인딩한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * [SPDK pci.c] → [DPDK PCI bus (rte_bus 구현)] → [이 헤더의 rte_bus 인터페이스]
 * rte_bus는 EAL 버스 레지스트리(rte_bus_list TAILQ)에 등록된다.
 * rte_eal_init() 호출 시 rte_bus_scan() → rte_bus_probe() 순으로 버스 초기화가 진행된다.
 * pci_dpdk_2207.c가 rte_pci_bus를 통해 이 인터페이스를 간접 사용한다.
 * 실행 컨텍스트: 호스트 유저스페이스, DPDK EAL 초기화 스레드 및 hotplug 이벤트 처리 스레드.
 *
 * === 타 모듈과의 연결 ===
 * - 포함 관계: 이 헤더는 rte_dev.h에 의존(rte_device, rte_dev_cmp_t, rte_dev_iterate_t 등 공유).
 * - pci_dpdk_2207.c(및 22.07/rte_bus_pci.h)가 rte_bus를 PCI 버스로 특수화하여 구현.
 * - SPDK pci.c는 rte_eal_hotplug_add/remove를 통해 PCI 버스에 디바이스를 동적 추가/제거.
 * - sigbus_handler.c는 rte_bus.sigbus_handler 콜백을 통해 VFIO BAR 접근 시 SIGBUS를 처리.
 * - memory.c의 DMA 매핑 경로: spdk_mem_register → rte_bus.dma_map (VFIO IOMMU 매핑).
 * - 데이터 흐름: 버스가 디바이스를 TAILQ에 등록 → probe 시 드라이버가 바인딩 → I/O 가능.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct rte_bus: 버스 추상화 핵심 구조체. scan/probe/dma_map/sigbus_handler 등 15개 콜백.
 * - enum rte_iova_mode: IOVA 매핑 모드 (DC/PA/VA). 메모리 모드 결정에 사용.
 * - rte_bus_register/unregister: EAL 버스 레지스트리에 버스 객체 등록/해제.
 * - rte_bus_scan/probe: 등록된 모든 버스를 순차 스캔 및 드라이버 매칭.
 * - rte_bus_get_iommu_class: 시스템 내 모든 버스의 IOVA 모드 합의값 반환.
 * - RTE_REGISTER_BUS: 컴파일 타임에 버스를 생성자 함수로 자동 등록하는 매크로.
 */

#ifndef _RTE_BUS_H_
#define _RTE_BUS_H_

/**
 * @file
 *
 * DPDK device bus interface
 *
 * This file exposes API and interfaces for bus abstraction
 * over the devices and drivers in EAL.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>        /* [한국어] FILE* 타입 사용 (rte_bus_dump 인자). 표준 I/O. */

#include <rte_log.h>      /* [한국어] DPDK 로깅 시스템. RTE_LOG 매크로로 버스 디버그 메시지 출력. */
#include "rte_dev.h"      /* [한국어] rte_device/rte_driver/rte_dev_cmp_t/rte_dev_iterate_t 정의 포함. */

/** Double linked list of buses */
/* [한국어] rte_bus_list - EAL에 등록된 모든 버스를 연결하는 이중 연결 리스트 헤드 타입.
 * RTE_TAILQ_HEAD 매크로로 생성되며, 실제 전역 리스트는 EAL 내부에서 관리.
 * rte_bus_register()가 이 리스트에 rte_bus를 추가하고, rte_bus_unregister()가 제거.
 * rte_bus_scan/probe/find 함수들이 이 리스트를 순회한다. */
RTE_TAILQ_HEAD(rte_bus_list, rte_bus);


/**
 * IOVA mapping mode.
 *
 * IOVA mapping mode is iommu programming mode of a device.
 * That device (for example: IOMMU backed DMA device) based
 * on rte_iova_mode will generate physical or virtual address.
 *
 */
/*
 * [한국어]
 * enum rte_iova_mode - I/O 가상 주소(IOVA) 매핑 모드
 *
 * NVMe PRP(Physical Region Page)/SGL 주소 필드에 어떤 유형의 주소를 쓰는지 결정.
 * SPDK 환경에서는 vfio-pci 사용 시 VA 모드, UIO 사용 시 PA 모드가 선택된다.
 * rte_bus_get_iommu_class()가 시스템 내 모든 버스/디바이스의 모드를 합의하여 반환.
 */
enum rte_iova_mode {
	RTE_IOVA_DC = 0,
	/* [한국어] Don't care — IOVA 모드 미결정/선호 없음.
	 * 버스에 바인딩된 디바이스가 없거나 특정 모드를 요구하지 않을 때 반환.
	 * rte_bus_get_iommu_class() 합의 결과가 DC이면 EAL이 기본값(보통 VA) 사용. */

	RTE_IOVA_PA = (1 << 0),
	/* [한국어] Physical Address 모드 — DMA 주소로 물리 주소(PA) 사용.
	 * UIO 기반 드라이버(uio_pci_generic, igb_uio) 환경에서 IOMMU 없이 PA로 DMA.
	 * NVMe PRP에 실제 물리 주소를 기록. memory.c의 vtophys가 PA를 반환하는 경로. */

	RTE_IOVA_VA = (1 << 1)
	/* [한국어] Virtual Address 모드 — DMA 주소로 프로세스 가상 주소(VA) 사용.
	 * VFIO(vfio-pci) + IOMMU 환경에서 VA = IOVA로 동작. IOMMU가 VA를 PA로 변환.
	 * SPDK의 기본 권장 모드. memory.c에서 vtophys가 VA를 그대로 IOVA로 반환. */
};

/**
 * Bus specific scan for devices attached on the bus.
 * For each bus object, the scan would be responsible for finding devices and
 * adding them to its private device list.
 *
 * A bus should mandatorily implement this method.
 *
 * @return
 *	0 for successful scan
 *	<0 for unsuccessful scan with error value
 */
/* [한국어] rte_bus_scan_t - 버스 스캔 콜백 함수 타입.
 * 버스에 연결된 디바이스를 탐색하여 버스 내부 디바이스 목록에 추가.
 * PCI 버스의 경우 /sys/bus/pci/devices 를 읽어 BDF 목록을 구성.
 * rte_bus_scan() 호출 시 등록된 모든 버스의 scan 콜백이 순차 호출됨.
 * 반환: 0 성공, 음수 실패. */
typedef int (*rte_bus_scan_t)(void);

/**
 * Implementation specific probe function which is responsible for linking
 * devices on that bus with applicable drivers.
 *
 * This is called while iterating over each registered bus.
 *
 * @return
 *	0 for successful probe
 *	!0 for any error while probing
 */
/* [한국어] rte_bus_probe_t - 버스 probe 콜백 함수 타입.
 * 스캔으로 발견된 디바이스들에 드라이버를 매칭하여 초기화(probe) 수행.
 * PCI 버스의 경우 BDF별로 등록된 PCI 드라이버와 vendor/device ID를 비교하여 매칭.
 * rte_bus_probe() 호출 시 등록된 모든 버스의 probe 콜백이 순차 호출됨.
 * 반환: 0 성공, 비-0 에러. */
typedef int (*rte_bus_probe_t)(void);

/**
 * Device iterator to find a device on a bus.
 *
 * This function returns an rte_device if one of those held by the bus
 * matches the data passed as parameter.
 *
 * If the comparison function returns zero this function should stop iterating
 * over any more devices. To continue a search the device of a previous search
 * can be passed via the start parameter.
 *
 * @param cmp
 *	Comparison function.
 *
 * @param data
 *	Data to compare each device against.
 *
 * @param start
 *	starting point for the iteration
 *
 * @return
 *	The first device matching the data, NULL if none exists.
 */
/* [한국어] rte_bus_find_device_t - 버스 디바이스 탐색 콜백 함수 타입.
 * start: 이전 탐색 결과(NULL이면 처음부터). cmp: 비교 함수. data: 비교 기준 데이터.
 * 반환: 첫 번째 매칭 rte_device, 없으면 NULL.
 * SPDK pci.c의 find_env_devargs, attach_rte_dev 등이 BDF 문자열 기반 탐색에 사용. */
typedef struct rte_device *
(*rte_bus_find_device_t)(const struct rte_device *start, rte_dev_cmp_t cmp,
			 const void *data);

/**
 * Implementation specific probe function which is responsible for linking
 * devices on that bus with applicable drivers.
 *
 * @param dev
 *	Device pointer that was returned by a previous call to find_device.
 *
 * @return
 *	0 on success.
 *	!0 on error.
 */
/* [한국어] rte_bus_plug_t - 단일 디바이스에 대한 probe(드라이버 바인딩) 콜백 타입.
 * dev: find_device로 찾아낸 rte_device 포인터.
 * 반환: 0 성공, 비-0 에러.
 * rte_eal_hotplug_add 경로에서 버스 레벨 plug()가 호출되어 드라이버 probe 수행. */
typedef int (*rte_bus_plug_t)(struct rte_device *dev);

/**
 * Implementation specific remove function which is responsible for unlinking
 * devices on that bus from assigned driver.
 *
 * @param dev
 *	Device pointer that was returned by a previous call to find_device.
 *
 * @return
 *	0 on success.
 *	!0 on error.
 */
/* [한국어] rte_bus_unplug_t - 단일 디바이스의 드라이버 바인딩 해제 콜백 타입.
 * dev: 바인딩 해제할 rte_device 포인터.
 * 반환: 0 성공, 비-0 에러.
 * rte_eal_hotplug_remove 경로에서 호출. SPDK remove_rte_dev → rte_eal_hotplug_remove. */
typedef int (*rte_bus_unplug_t)(struct rte_device *dev);

/**
 * Bus specific parsing function.
 * Validates the syntax used in the textual representation of a device,
 * If the syntax is valid and ``addr`` is not NULL, writes the bus-specific
 * device representation to ``addr``.
 *
 * @param[in] name
 *	device textual description
 *
 * @param[out] addr
 *	device information location address, into which parsed info
 *	should be written. If NULL, nothing should be written, which
 *	is not an error.
 *
 * @return
 *	0 if parsing was successful.
 *	!0 for any error.
 */
/* [한국어] rte_bus_parse_t - 버스 디바이스 이름 파싱 콜백 타입.
 * name: "0000:04:00.0" 형식의 디바이스 텍스트 표현.
 * addr: 파싱 결과를 쓸 버퍼 (NULL이면 유효성 검사만 수행).
 * 반환: 0 성공, 비-0 에러.
 * PCI 버스에서는 BDF(Bus:Device.Function) 형식을 rte_pci_addr 구조체로 변환. */
typedef int (*rte_bus_parse_t)(const char *name, void *addr);

/**
 * Parse bus part of the device arguments.
 *
 * The field name of the struct rte_devargs will be set.
 *
 * @param da
 *	Pointer to the devargs to parse.
 *
 * @return
 *	0 on successful parsing, otherwise rte_errno is set.
 *	-EINVAL: on parsing error.
 *	-ENODEV: if no key matching a device argument is specified.
 *	-E2BIG: device name is too long.
 */
/* [한국어] rte_bus_devargs_parse_t - devargs 파싱 콜백 타입.
 * da: rte_devargs 포인터. 파싱 후 da->name 필드가 설정됨.
 * 반환: 0 성공, -EINVAL(파싱 오류), -ENODEV(키 없음), -E2BIG(이름 길이 초과).
 * rte_eal_hotplug_add의 인자 파싱 과정에서 사용. */
typedef int (*rte_bus_devargs_parse_t)(struct rte_devargs *da);

/**
 * Device level DMA map function.
 * After a successful call, the memory segment will be mapped to the
 * given device.
 *
 * @param dev
 *	Device pointer.
 * @param addr
 *	Virtual address to map.
 * @param iova
 *	IOVA address to map.
 * @param len
 *	Length of the memory segment being mapped.
 *
 * @return
 *	0 if mapping was successful.
 *	Negative value and rte_errno is set otherwise.
 */
/* [한국어] rte_dev_dma_map_t - 버스 레벨 DMA 매핑 콜백 타입.
 * dev: 매핑 대상 디바이스. addr: 가상 주소. iova: I/O 가상 주소. len: 길이.
 * 반환: 0 성공, 음수+rte_errno 실패.
 * PCI 버스에서 VFIO_IOMMU_MAP_DMA ioctl을 수행. memory.c의 spdk_mem_register가 이 경로 사용. */
typedef int (*rte_dev_dma_map_t)(struct rte_device *dev, void *addr,
				  uint64_t iova, size_t len);

/**
 * Device level DMA unmap function.
 * After a successful call, the memory segment will no longer be
 * accessible by the given device.
 *
 * @param dev
 *	Device pointer.
 * @param addr
 *	Virtual address to unmap.
 * @param iova
 *	IOVA address to unmap.
 * @param len
 *	Length of the memory segment being mapped.
 *
 * @return
 *	0 if un-mapping was successful.
 *	Negative value and rte_errno is set otherwise.
 */
/* [한국어] rte_dev_dma_unmap_t - 버스 레벨 DMA 언매핑 콜백 타입.
 * dev: 언매핑 대상 디바이스. addr: 가상 주소. iova: I/O 가상 주소. len: 길이.
 * 반환: 0 성공, 음수+rte_errno 실패.
 * VFIO_IOMMU_UNMAP_DMA ioctl 수행. spdk_mem_unregister 경로에서 호출. */
typedef int (*rte_dev_dma_unmap_t)(struct rte_device *dev, void *addr,
				   uint64_t iova, size_t len);

/**
 * Implement a specific hot-unplug handler, which is responsible for
 * handle the failure when device be hot-unplugged. When the event of
 * hot-unplug be detected, it could call this function to handle
 * the hot-unplug failure and avoid app crash.
 * @param dev
 *	Pointer of the device structure.
 *
 * @return
 *	0 on success.
 *	!0 on error.
 */
/* [한국어] rte_bus_hot_unplug_handler_t - hot-unplug 실패(surprise removal) 처리 콜백 타입.
 * dev: hot-unplug 감지된 rte_device 포인터.
 * 반환: 0 성공, 비-0 에러.
 * SIGBUS 발생 또는 NETLINK REMOVE 이벤트 후 sigbus_handler.c를 통해 호출 가능.
 * 드라이버가 디바이스를 안전하게 분리하고 I/O 요청을 에러 처리하도록 한다. */
typedef int (*rte_bus_hot_unplug_handler_t)(struct rte_device *dev);

/**
 * Implement a specific sigbus handler, which is responsible for handling
 * the sigbus error which is either original memory error, or specific memory
 * error that caused of device be hot-unplugged. When sigbus error be captured,
 * it could call this function to handle sigbus error.
 * @param failure_addr
 *	Pointer of the fault address of the sigbus error.
 *
 * @return
 *	0 for success handle the sigbus for hot-unplug.
 *	1 for not process it, because it is a generic sigbus error.
 *	-1 for failed to handle the sigbus for hot-unplug.
 */
/* [한국어] rte_bus_sigbus_handler_t - SIGBUS 에러 처리 콜백 타입.
 * failure_addr: SIGBUS를 유발한 폴트 주소 (BAR MMIO 주소 등).
 * 반환: 0 hot-unplug SIGBUS 처리 성공, 1 처리하지 않음(일반 메모리 에러), -1 처리 실패.
 * sigbus_handler.c의 sigbus_fault_sighandler가 등록된 모든 버스의 sigbus_handler를 순회 호출.
 * 폴트 주소가 특정 디바이스의 BAR 영역인지 비교하여 해당 디바이스의 hot-unplug 처리 수행. */
typedef int (*rte_bus_sigbus_handler_t)(const void *failure_addr);

/**
 * Bus scan policies
 */
/*
 * [한국어]
 * enum rte_bus_scan_mode - 버스 스캔 정책
 *
 * EAL 인자 --allow / --block 로 설정. SPDK init.c의 build_eal_cmdline()이
 * 사용자가 지정한 디바이스 목록에 따라 ALLOWLIST 또는 BLOCKLIST 모드로 EAL을 구성.
 */
enum rte_bus_scan_mode {
	RTE_BUS_SCAN_UNDEFINED,
	/* [한국어] 스캔 정책 미설정 — EAL 인자가 없거나 초기화 전 상태.
	 * 이 상태에서는 버스가 모든 디바이스를 기본 동작으로 처리. */

	RTE_BUS_SCAN_ALLOWLIST,
	/* [한국어] 허용 목록(allowlist) 모드 — 명시적으로 허용된 디바이스만 probe.
	 * EAL 인자 --allow 0000:04:00.0 으로 설정. SPDK에서 특정 NVMe만 바인딩할 때 사용. */

	RTE_BUS_SCAN_BLOCKLIST,
	/* [한국어] 차단 목록(blocklist) 모드 — 명시적으로 차단된 디바이스는 probe 제외.
	 * EAL 인자 --block 으로 설정. 커널 드라이버를 유지해야 하는 디바이스 제외에 사용. */
};

/**
 * A structure used to configure bus operations.
 */
/*
 * [한국어]
 * struct rte_bus_conf - 버스 동작 설정 구조체
 *
 * 현재는 scan_mode 단일 필드만 포함. rte_bus.conf 멤버로 내장되어 버스별 설정 보유.
 */
struct rte_bus_conf {
	enum rte_bus_scan_mode scan_mode;
	/* [한국어] 스캔 정책 (UNDEFINED/ALLOWLIST/BLOCKLIST).
	 * 설정자: EAL 초기화(rte_eal_init) 중 EAL 인자 파싱 결과로 설정.
	 * 읽는 자: rte_bus_scan()이 디바이스 열거 전에 이 값을 참조하여 필터링.
	 * 값 범위: rte_bus_scan_mode 열거형 3개 값 중 하나.
	 * 동기화: EAL 초기화 완료 후에는 읽기 전용이므로 별도 락 불필요. */
};


/**
 * Get common iommu class of the all the devices on the bus. The bus may
 * check that those devices are attached to iommu driver.
 * If no devices are attached to the bus. The bus may return with don't care
 * (_DC) value.
 * Otherwise, The bus will return appropriate _pa or _va iova mode.
 *
 * @return
 *      enum rte_iova_mode value.
 */
/* [한국어] rte_bus_get_iommu_class_t - 버스에 바인딩된 디바이스들의 IOVA 모드 반환 콜백.
 * 반환: rte_iova_mode (DC/PA/VA).
 * PCI 버스는 VFIO 디바이스가 있으면 VA, UIO만 있으면 PA, 없으면 DC 반환.
 * rte_bus_get_iommu_class()가 모든 버스의 반환값을 합의하여 전체 IOVA 모드 결정. */
typedef enum rte_iova_mode (*rte_bus_get_iommu_class_t)(void);


/**
 * A structure describing a generic bus.
 */
/*
 * [한국어]
 * struct rte_bus - DPDK 범용 버스 추상화 핵심 구조체
 *
 * EAL에 등록된 각 버스(PCI, vdev 등)를 나타내는 객체. 버스별 동작을 함수 포인터 테이블로
 * 표현하며, rte_bus_register()로 전역 rte_bus_list TAILQ에 추가된다.
 * SPDK는 PCI 버스를 통해 NVMe 디바이스를 열거·바인딩·DMA 매핑한다.
 * pci_dpdk_2207.c가 rte_pci_bus(rte_bus의 PCI 확장)를 통해 이 구조체를 간접 참조.
 */
struct rte_bus {
	RTE_TAILQ_ENTRY(rte_bus) next;
	/* [한국어] EAL 전역 버스 목록(rte_bus_list)에 연결되는 TAILQ 링크.
	 * 설정자: rte_bus_register()가 버스를 리스트 꼬리에 삽입할 때 설정.
	 * 읽는 자: rte_bus_scan/probe/find/dump 등이 리스트 순회 시 사용.
	 * 값 범위: 유효한 다음 rte_bus 포인터 또는 리스트 끝(TAILQ_END).
	 * 동기화: EAL 초기화 완료 후에는 불변. 동적 추가 시 내부 뮤텍스 보호. */

	const char *name;
	/* [한국어] 버스 이름 문자열 (예: "pci", "vdev", "ifpga").
	 * 설정자: RTE_REGISTER_BUS 매크로의 생성자 함수에서 RTE_STR(nm)으로 설정.
	 * 읽는 자: rte_bus_find_by_name()이 이름으로 버스 탐색; 로그 메시지 출력.
	 * 값 범위: NULL 불가, 정적 문자열 리터럴.
	 * 동기화: 초기화 후 읽기 전용. */

	rte_bus_scan_t scan;
	/* [한국어] 버스 디바이스 스캔 콜백. EAL 초기화 시 rte_bus_scan()이 호출.
	 * 설정자: 버스 구현체가 정적으로 할당 (예: pci_scan).
	 * 읽는 자: rte_bus_scan()이 모든 버스를 순회하며 각 scan() 호출.
	 * 값 범위: 유효한 함수 포인터 (NULL 불가 — 버스 필수 구현).
	 * 동기화: 초기화 후 불변 포인터. */

	rte_bus_probe_t probe;
	/* [한국어] 버스 드라이버 probe 콜백. scan 후 rte_bus_probe()가 호출.
	 * 설정자: 버스 구현체 정적 할당 (예: pci_probe).
	 * 읽는 자: rte_bus_probe()가 모든 버스 순회.
	 * 값 범위: 유효한 함수 포인터 (필수).
	 * 동기화: 초기화 후 불변. */

	rte_bus_find_device_t find_device;
	/* [한국어] 버스 내 특정 디바이스 탐색 콜백.
	 * 설정자: 버스 구현체 정적 할당.
	 * 읽는 자: rte_eal_hotplug_add/remove 경로 및 SPDK pci.c의 디바이스 탐색.
	 * 값 범위: 유효한 함수 포인터.
	 * 동기화: 버스 내부 디바이스 리스트에 대한 접근은 버스 구현 내 락으로 보호. */

	rte_bus_plug_t plug;
	/* [한국어] 단일 디바이스 plug(probe) 콜백. hot-add 경로에서 사용.
	 * 설정자: 버스 구현체 정적 할당.
	 * 읽는 자: rte_eal_hotplug_add → rte_bus_plug_t.
	 * 값 범위: NULL 가능 (hot-plug 미지원 버스).
	 * 동기화: 버스 구현 내부에서 처리. */

	rte_bus_unplug_t unplug;
	/* [한국어] 단일 디바이스 unplug(드라이버 분리) 콜백. hot-remove 경로에서 사용.
	 * 설정자: 버스 구현체 정적 할당.
	 * 읽는 자: rte_eal_hotplug_remove → rte_bus_unplug_t.
	 * SPDK pci.c: remove_rte_dev → rte_eal_hotplug_remove → unplug.
	 * 값 범위: NULL 가능. 동기화: 버스 구현 내부 처리. */

	rte_bus_parse_t parse;
	/* [한국어] 디바이스 이름 문자열 파싱 콜백.
	 * 설정자: 버스 구현체 정적 할당.
	 * 읽는 자: devargs 파싱, BDF 문자열 → rte_pci_addr 변환.
	 * 값 범위: NULL 가능. */

	rte_bus_devargs_parse_t devargs_parse;
	/* [한국어] devargs 전체 파싱 콜백. rte_devargs 구조체의 name 필드 설정.
	 * 설정자: 버스 구현체 정적 할당.
	 * 읽는 자: rte_eal_hotplug_add 내부 devargs 파싱 경로.
	 * 값 범위: NULL 가능. */

	rte_dev_dma_map_t dma_map;
	/* [한국어] 버스 레벨 DMA 매핑 콜백. VFIO 기반 PCI 버스에서 필수.
	 * 설정자: PCI 버스 구현체가 pci_dma_map 함수를 등록.
	 * 읽는 자: memory.c의 spdk_mem_register → rte_dev_dma_map → 이 콜백.
	 *          새 hugepage/메모리 세그먼트 등록 시 모든 VFIO 디바이스에 DMA 매핑 추가.
	 * 값 범위: VFIO 미사용 버스에서는 NULL.
	 * 동기화: VFIO 컨테이너 fd는 g_vfio.mutex로 보호. */

	rte_dev_dma_unmap_t dma_unmap;
	/* [한국어] 버스 레벨 DMA 언매핑 콜백. DMA 등록 해제 시 사용.
	 * 설정자: PCI 버스 구현체가 pci_dma_unmap 함수를 등록.
	 * 읽는 자: spdk_mem_unregister → rte_dev_dma_unmap → 이 콜백.
	 * 값 범위: VFIO 미사용 버스에서는 NULL.
	 * 동기화: dma_map과 동일. */

	struct rte_bus_conf conf;
	/* [한국어] 버스별 설정 (scan_mode: ALLOWLIST/BLOCKLIST/UNDEFINED).
	 * 설정자: EAL 인자 파싱(rte_eal_init) 시 --allow/--block 옵션에 따라 설정.
	 * 읽는 자: scan() 콜백이 디바이스 열거 전 필터링 기준으로 참조.
	 * 값 범위: rte_bus_scan_mode 중 하나.
	 * 동기화: EAL 초기화 완료 후 불변. */

	rte_bus_get_iommu_class_t get_iommu_class;
	/* [한국어] 버스 IOVA 모드 반환 콜백. PA/VA/DC 중 하나 반환.
	 * 설정자: 버스 구현체 정적 할당.
	 * 읽는 자: rte_bus_get_iommu_class()가 전체 시스템 IOVA 모드 합의에 사용.
	 *          SPDK init.c에서 VFIO/UIO 감지 후 메모리 매핑 방식 결정에 활용.
	 * 값 범위: NULL 가능. */

	rte_dev_iterate_t dev_iterate;
	/* [한국어] 버스 디바이스 이터레이터 콜백.
	 * 설정자: 버스 구현체 정적 할당.
	 * 읽는 자: rte_dev_iterator_next()가 이 콜백을 통해 다음 디바이스 탐색.
	 * 값 범위: NULL 가능. */

	rte_bus_hot_unplug_handler_t hot_unplug_handler;
	/* [한국어] PCIe surprise removal 감지 후 호출되는 hot-unplug 처리 콜백.
	 * 설정자: PCI 버스 구현체가 pci_hot_unplug_handler를 등록.
	 * 읽는 자: sigbus_handler.c의 sigbus_fault_sighandler → SPDK 에러 핸들러 → 이 콜백.
	 *          디바이스가 PCIe 링크 손실로 갑자기 사라질 때 드라이버 정리 수행.
	 * 값 범위: NULL 가능.
	 * 동기화: SIGBUS 시그널 핸들러에서 호출되므로 async-signal-safe 함수만 사용해야 함. */

	rte_bus_sigbus_handler_t sigbus_handler;
	/* [한국어] SIGBUS 폴트 주소가 이 버스의 디바이스 BAR인지 확인하는 콜백.
	 * 설정자: PCI 버스 구현체가 pci_sigbus_handler를 등록.
	 * 읽는 자: sigbus_handler.c의 sigbus_fault_sighandler가 등록된 모든 버스 순회.
	 * 반환: 0(처리됨), 1(무관), -1(처리 실패).
	 * SPDK sigbus_handler.c: 사용자 등록 에러 핸들러 → rte_bus.sigbus_handler 순으로 호출.
	 * 동기화: SIGBUS 핸들러 컨텍스트 — async-signal-safe 제약. */
};

/**
 * Register a Bus handler.
 *
 * @param bus
 *   A pointer to a rte_bus structure describing the bus
 *   to be registered.
 */
/* [한국어] rte_bus_register - EAL 버스 레지스트리에 버스 객체 등록.
 * bus: 등록할 rte_bus 구조체 포인터.
 * 동작: rte_bus_list TAILQ 꼬리에 추가. RTE_REGISTER_BUS 매크로의 생성자 함수에서 호출.
 * 호출 시점: 프로그램 시작 시 __constructor__ 우선도 BUS로 자동 호출. */
void rte_bus_register(struct rte_bus *bus);

/**
 * Unregister a Bus handler.
 *
 * @param bus
 *   A pointer to a rte_bus structure describing the bus
 *   to be unregistered.
 */
/* [한국어] rte_bus_unregister - EAL 버스 레지스트리에서 버스 객체 제거.
 * bus: 제거할 rte_bus 구조체 포인터.
 * 동작: rte_bus_list TAILQ에서 해당 버스 노드 제거.
 * 호출 시점: 모듈 언로드 또는 EAL cleanup 시. */
void rte_bus_unregister(struct rte_bus *bus);

/**
 * Scan all the buses.
 *
 * @return
 *   0 in case of success in scanning all buses
 *  !0 in case of failure to scan
 */
/* [한국어] rte_bus_scan - 등록된 모든 버스의 scan() 콜백 순차 호출.
 * 반환: 0 전체 성공, 비-0 중 하나라도 실패.
 * 동작: /sys/bus/pci/devices 등을 읽어 버스별 디바이스 목록 구성.
 * 호출 체인: rte_eal_init → [rte_bus_scan] → 각 버스의 scan() → 디바이스 발견. */
int rte_bus_scan(void);

/**
 * For each device on the buses, perform a driver 'match' and call the
 * driver-specific probe for device initialization.
 *
 * @return
 *	 0 for successful match/probe
 *	!0 otherwise
 */
/* [한국어] rte_bus_probe - 등록된 모든 버스의 probe() 콜백 순차 호출.
 * 반환: 0 전체 성공, 비-0 실패.
 * 동작: scan으로 발견된 디바이스와 등록된 드라이버(vendor/device ID)를 매칭하여 probe.
 * 호출 체인: rte_eal_init → rte_bus_scan → [rte_bus_probe] → 드라이버 probe() → NVMe 초기화. */
int rte_bus_probe(void);

/**
 * Dump information of all the buses registered with EAL.
 *
 * @param f
 *	 A valid and open output stream handle
 */
/* [한국어] rte_bus_dump - 등록된 모든 버스 정보를 파일 스트림에 출력.
 * f: 출력 대상 파일 스트림 (예: stderr, 로그 파일).
 * 디버깅/진단 목적으로 각 버스 이름과 설정을 출력. */
void rte_bus_dump(FILE *f);

/**
 * Bus comparison function.
 *
 * @param bus
 *	Bus under test.
 *
 * @param data
 *	Data to compare against.
 *
 * @return
 *	0 if the bus matches the data.
 *	!0 if the bus does not match.
 *	<0 if ordering is possible and the bus is lower than the data.
 *	>0 if ordering is possible and the bus is greater than the data.
 */
/* [한국어] rte_bus_cmp_t - 버스 비교 콜백 타입.
 * bus: 비교 대상 버스. data: 비교 기준 데이터 (예: 버스 이름 문자열).
 * 반환: 0 일치, 비-0 불일치, 음수/양수로 정렬 순서 표현 가능.
 * rte_bus_find()의 탐색 기준 콜백으로 사용. */
typedef int (*rte_bus_cmp_t)(const struct rte_bus *bus, const void *data);

/**
 * Bus iterator to find a particular bus.
 *
 * This function compares each registered bus to find one that matches
 * the data passed as parameter.
 *
 * If the comparison function returns zero this function will stop iterating
 * over any more buses. To continue a search the bus of a previous search can
 * be passed via the start parameter.
 *
 * @param start
 *	Starting point for the iteration.
 *
 * @param cmp
 *	Comparison function.
 *
 * @param data
 *	 Data to pass to comparison function.
 *
 * @return
 *	 A pointer to a rte_bus structure or NULL in case no bus matches
 */
/* [한국어] rte_bus_find - 조건에 맞는 버스를 rte_bus_list에서 탐색.
 * start: 이전 탐색 결과 또는 NULL(처음부터). cmp: 비교 콜백. data: 비교 기준.
 * 반환: 첫 번째 매칭 rte_bus 포인터, 없으면 NULL.
 * rte_bus_find_by_name/by_device가 내부적으로 이 함수를 이름/디바이스 기준 cmp와 함께 사용. */
struct rte_bus *rte_bus_find(const struct rte_bus *start, rte_bus_cmp_t cmp,
			     const void *data);

/**
 * Find the registered bus for a particular device.
 */
/* [한국어] rte_bus_find_by_device - 특정 rte_device가 속한 버스를 반환.
 * dev: 버스를 찾을 rte_device 포인터.
 * 반환: dev->bus와 같은 rte_bus, 없으면 NULL.
 * SPDK pci.c에서 디바이스의 버스 레벨 DMA 매핑 함수를 구하는 데 사용. */
struct rte_bus *rte_bus_find_by_device(const struct rte_device *dev);

/**
 * Find the registered bus for a given name.
 */
/* [한국어] rte_bus_find_by_name - 이름으로 버스를 탐색하여 반환.
 * busname: "pci", "vdev" 등 버스 이름 문자열.
 * 반환: 해당 이름의 rte_bus, 없으면 NULL.
 * rte_eal_hotplug_add("pci", ...) 등에서 버스 이름으로 버스 객체 조회. */
struct rte_bus *rte_bus_find_by_name(const char *busname);


/**
 * Get the common iommu class of devices bound on to buses available in the
 * system. RTE_IOVA_DC means that no preference has been expressed.
 *
 * @return
 *     enum rte_iova_mode value.
 */
/* [한국어] rte_bus_get_iommu_class - 시스템 전체 버스의 IOVA 모드 합의값 반환.
 * 반환: PA(물리 주소), VA(가상 주소), DC(무관심) 중 합의된 값.
 * 동작: 모든 버스의 get_iommu_class() 콜백을 호출하여 PA/VA가 충돌하면 경고 출력.
 * SPDK init.c에서 EAL 초기화 후 VFIO/UIO 결정 확인에 사용. */
enum rte_iova_mode rte_bus_get_iommu_class(void);

/**
 * Helper for Bus registration.
 * The constructor has higher priority than PMD constructors.
 */
/* [한국어] RTE_REGISTER_BUS - 컴파일 타임에 버스를 EAL에 자동 등록하는 매크로.
 * nm: 버스 이름 심볼 (예: pci, vdev). bus: 등록할 rte_bus 구조체 변수.
 * 동작: RTE_INIT_PRIO(BUS 우선도)로 생성자 함수를 생성하여 프로그램 시작 시 자동 호출.
 * 버스 우선도가 PMD 생성자보다 높아서 드라이버 등록 전에 버스가 먼저 준비된다.
 * PCI 버스 구현체가 이 매크로로 rte_pci_bus를 전역 레지스트리에 등록. */
#define RTE_REGISTER_BUS(nm, bus) \
RTE_INIT_PRIO(businitfn_ ##nm, BUS) \
{\
	(bus).name = RTE_STR(nm);\
	rte_bus_register(&bus); \
}

#ifdef __cplusplus
}
#endif

#endif /* _RTE_BUS_H */
