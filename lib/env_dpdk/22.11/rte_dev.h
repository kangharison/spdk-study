/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2014 6WIND S.A.
 */

/*
 * [한국어 설명] DPDK 22.11 vendored 내부 디바이스 공통 인터페이스 헤더 (22.11/rte_dev.h)
 *
 * === 파일의 역할 ===
 * DPDK 22.11 버전의 rte_device / rte_driver / rte_mem_resource 등 범용 디바이스 추상화
 * 구조체와 hotplug, 디바이스 이벤트(add/remove), DMA map/unmap API를 선언한다.
 * 22.07 버전의 22.07/rte_dev.h와 비교하면 rte_device 구조체 필드가 접근자 함수
 * (rte_dev_bus, rte_dev_name 등)로 캡슐화되어 직접 필드 접근이 불가능해진 점이 다르다.
 * pci_dpdk_2211.c가 이 헤더를 포함하여 22.11 ABI의 추상화된 rte_device 인터페이스를 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * [SPDK pci.c] → [pci_dpdk_2211.c / dpdk_fn_table 2211버전] → [이 헤더의 rte_device 접근자]
 * 22.11에서는 rte_pci_driver가 내부 헤더(bus_pci_driver.h)로 이동했으므로 pci_dpdk_2211.c가
 * 22.11/bus_pci_driver.h를 별도 포함하여 rte_pci_driver에 접근한다.
 * rte_device 필드에 직접 접근하는 대신 rte_dev_name/bus/driver 등 접근자 함수를 사용해야 한다.
 * 실행 컨텍스트: 호스트 유저스페이스, DPDK EAL 초기화 + SPDK pci.c probe 경로.
 *
 * === 타 모듈과의 연결 ===
 * - 포함 관계: pci_dpdk_2211.c가 이 헤더를 포함. 22.11/bus_driver.h/bus_pci_driver.h도 이에 의존.
 * - rte_dev.h(22.07) vs rte_dev.h(22.11) 차이: 22.11에서 rte_device 직접 필드 접근 불가.
 * - SPDK pci.c: rte_eal_hotplug_add/remove 호출로 디바이스 동적 추가/제거.
 * - memory.c: rte_dev_dma_map/unmap을 통해 VFIO IOMMU 매핑.
 * - 데이터 흐름: rte_device 접근자 함수 → DPDK 내부 rte_device 구조체 → 버스/드라이버 정보.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct rte_mem_resource: PCI BAR 자원(phys_addr/len/addr). 22.11에서도 레이아웃 동일.
 * - rte_driver_name/rte_dev_bus/rte_dev_name 등: 22.11 신규 접근자 함수 — 필드 직접 접근 대체.
 * - rte_dev_iterator/rte_dev_iterator_init/next: 디바이스 순회 컨텍스트 및 API.
 * - rte_eal_hotplug_add/remove: DPDK PCI 디바이스 런타임 추가/제거.
 * - rte_dev_dma_map/unmap: VFIO 컨테이너에 메모리 영역 DMA 매핑/해제.
 * - RTE_DEV_FOREACH: 조건 기반 디바이스 순회 매크로.
 */

#ifndef _RTE_DEV_H_
#define _RTE_DEV_H_

/**
 * @file
 *
 * RTE PMD Registration Interface
 *
 * This file manages the list of device drivers.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>       /* [한국어] FILE* 타입. rte_bus_dump 등 출력 함수 인자용. */

#include <rte_config.h>  /* [한국어] DPDK 컴파일 설정 — RTE_MAX_LCORE 등 빌드 시 결정된 상수. */
#include <rte_common.h>  /* [한국어] __rte_used, __rte_experimental, RTE_STR 등 공통 매크로. */
#include <rte_compat.h>  /* [한국어] RTE_DEPRECATED 매크로 — 22.11에서 일부 API가 deprecated 표시됨. */
#include <rte_log.h>     /* [한국어] DPDK 로깅 시스템. 버스/드라이버 디버그 메시지용. */

/* [한국어] 전방 선언 — 순환 참조 방지. 22.11에서는 rte_device/rte_driver의 내부 필드가
 * 숨겨지므로 직접 필드 접근 대신 rte_dev_name/rte_dev_bus 등 접근자 함수를 사용해야 한다. */
struct rte_bus;
struct rte_devargs;
struct rte_device;
struct rte_driver;

/**
 * The device event type.
 */
/*
 * [한국어]
 * enum rte_dev_event_type - 디바이스 이벤트 유형
 *
 * hotplug 콜백(rte_dev_event_cb_fn)과 이벤트 모니터링 스레드가 사용하는 이벤트 분류.
 * SPDK pci_event.c는 NETLINK_KOBJECT_UEVENT 소켓으로 직접 이벤트를 수신하지만,
 * DPDK 내부 hotplug 경로는 이 타입으로 콜백을 전달한다.
 */
enum rte_dev_event_type {
	RTE_DEV_EVENT_ADD,
	/* [한국어] 디바이스 추가 이벤트. rte_eal_hotplug_add 완료 후 등록된 콜백에 통보.
	 * SPDK에서 새 NVMe 디바이스가 런타임 추가될 때 발생. */

	RTE_DEV_EVENT_REMOVE,
	/* [한국어] 디바이스 제거 이벤트. rte_eal_hotplug_remove 또는 surprise removal 감지 시 발생.
	 * PCIe 링크 손실 시 SIGBUS → sigbus_handler → 이 이벤트 경로로 전달. */

	RTE_DEV_EVENT_MAX
	/* [한국어] 열거형 범위 상한값. 배열 크기 결정 또는 유효성 검사에 사용. */
};

/* [한국어] rte_dev_event_cb_fn - 디바이스 이벤트 콜백 함수 타입.
 * device_name: 이벤트 대상 디바이스 이름. event: ADD 또는 REMOVE. cb_arg: 사용자 정의 인자.
 * 설정자: rte_dev_event_callback_register()로 등록.
 * 읽는 자: rte_dev_event_callback_process()가 등록된 모든 콜백을 순회하며 호출.
 * 동기화: DPDK 이벤트 목록 내부 락으로 보호. */
typedef void (*rte_dev_event_cb_fn)(const char *device_name,
					enum rte_dev_event_type event,
					void *cb_arg);

/* Macros to check for invalid function pointers */
/* [한국어] RTE_FUNC_PTR_OR_ERR_RET - 함수 포인터가 NULL이면 retval 반환 (22.11에서 deprecated).
 * 22.07에서 사용되던 패턴이며, 22.11 이후 직접 NULL 체크 후 리턴 방식으로 대체 권장. */
#define RTE_FUNC_PTR_OR_ERR_RET(func, retval) RTE_DEPRECATED(RTE_FUNC_PTR_OR_ERR_RET) \
do { \
	if ((func) == NULL) \
		return retval; \
} while (0)

/* [한국어] RTE_FUNC_PTR_OR_RET - 함수 포인터가 NULL이면 void 리턴 (22.11에서 deprecated).
 * void 반환 함수에서 사용. 22.11 이후 deprecated 처리되어 직접 NULL 체크 권장. */
#define RTE_FUNC_PTR_OR_RET(func) RTE_DEPRECATED(RTE_FUNC_PTR_OR_RET) \
do { \
	if ((func) == NULL) \
		return; \
} while (0)

/**
 * Device policies.
 */
/*
 * [한국어]
 * enum rte_dev_policy - 디바이스 허용/차단 정책
 *
 * rte_devargs 파싱 결과로 설정되며, EAL --allow/--block 인자에 대응.
 * rte_pci_bus.conf.scan_mode와 연동하여 스캔 시 디바이스 필터링에 사용.
 */
enum rte_dev_policy {
	RTE_DEV_ALLOWED,
	/* [한국어] 허용(allowlist) — 이 디바이스는 probe 대상에 포함.
	 * SPDK init.c에서 --allow <BDF> 로 지정된 NVMe 디바이스가 이 정책으로 설정됨. */

	RTE_DEV_BLOCKED,
	/* [한국어] 차단(blocklist) — 이 디바이스는 probe 대상에서 제외.
	 * 커널 드라이버를 유지해야 하거나 SPDK에서 사용하지 않을 디바이스에 설정. */
};

/**
 * A generic memory resource representation.
 */
/*
 * [한국어]
 * struct rte_mem_resource - PCI BAR(Base Address Register) 자원 기술자
 *
 * PCI 디바이스의 각 BAR 영역(메모리 또는 I/O)의 물리·가상 주소와 크기를 기록.
 * rte_pci_device.mem_resource[PCI_MAX_RESOURCE] 배열의 요소 타입.
 * SPDK pci.c map_bar_rte가 mem_resource[bar].addr과 len으로 BAR mmap 결과를 읽는다.
 * 22.11에서도 이 구조체의 레이아웃은 22.07과 동일하게 유지된다.
 */
struct rte_mem_resource {
	uint64_t phys_addr;
	/* [한국어] BAR 물리 주소 (PCI Config Space의 BAR 레지스터 값).
	 * 설정자: pci_scan_one()이 sysfs resource 파일에서 물리 주소를 읽어 설정.
	 * 읽는 자: UIO 드라이버가 mmap offset 계산에 사용. VFIO 환경에서는 직접 미사용.
	 * 값 범위: 64비트 물리 주소. BAR가 없거나 미지원이면 0.
	 * 동기화: 스캔 완료 후 불변. */

	uint64_t len;
	/* [한국어] BAR 영역 크기 (바이트 단위).
	 * 설정자: pci_scan_one()이 sysfs resource 파일에서 크기를 읽어 설정.
	 * 읽는 자: SPDK pci.c map_bar_rte에서 mmap 길이 인자로 전달; BAR 범위 유효성 확인.
	 * 값 범위: 양수 BAR 크기; BAR 미사용 시 0.
	 * 동기화: 불변. */

	void *addr;
	/* [한국어] BAR mmap 후 유저스페이스 가상 주소.
	 * 설정자: rte_pci_map_device()의 VFIO/UIO mmap 성공 후 설정. 실패 또는 미매핑 시 NULL.
	 * 읽는 자: SPDK pci.c map_bar_rte → map_bar_rte_fn이 이 주소를 반환.
	 *           NVMe 드라이버가 BAR0 addr로 CC/CSTS/도어벨 레지스터 MMIO 접근.
	 * 값 범위: mmap 성공 시 유효한 가상 주소; NULL = 미매핑.
	 * 동기화: probe 완료 후 단일 reactor가 전담하므로 별도 락 불필요. */
};

/*
 * [한국어] 22.11 ABI 변경: rte_device 구조체 내부 필드가 은닉(opaque)되어
 * 아래 접근자 함수들을 통해서만 접근 가능. pci_dpdk_2211.c는 22.07처럼 직접
 * dev->driver, dev->name 등에 접근하는 대신 이 함수들을 사용해야 한다.
 */

/**
 * Retrieve a driver name.
 *
 * @param driver
 *   A pointer to a driver structure.
 * @return
 *   A pointer to the driver name string.
 */
/* [한국어] rte_driver_name - rte_driver 포인터에서 드라이버 이름 문자열 반환.
 * 22.11에서 rte_driver 구조체 직접 접근 불가로 이 함수 사용 필수.
 * 반환: 드라이버 이름 C 문자열 포인터 (예: "nvme"). */
const char *
rte_driver_name(const struct rte_driver *driver);

/**
 * Retrieve a device bus.
 *
 * @param dev
 *   A pointer to a device structure.
 * @return
 *   A pointer to this device bus.
 */
/* [한국어] rte_dev_bus - rte_device에서 소속 버스(rte_bus*) 반환.
 * 22.07에서의 dev->bus 직접 접근을 이 함수로 대체.
 * SPDK pci_dpdk_2211.c가 DMA 매핑 경로에서 버스 포인터를 얻을 때 사용. */
const struct rte_bus *
rte_dev_bus(const struct rte_device *dev);

/**
 * Retrieve bus specific information for a device.
 *
 * @param dev
 *   A pointer to a device structure.
 * @return
 *   A string describing this device or NULL if none is available.
 */
/* [한국어] rte_dev_bus_info - 디바이스의 버스 특화 정보 문자열 반환.
 * 예: PCI 버스에서는 "0000:04:00.0" 형식의 BDF 문자열. 없으면 NULL. */
const char *
rte_dev_bus_info(const struct rte_device *dev);

/**
 * Retrieve a device arguments.
 *
 * @param dev
 *   A pointer to a device structure.
 * @return
 *   A pointer to this device devargs.
 */
/* [한국어] rte_dev_devargs - rte_device에서 devargs(디바이스 초기화 인자) 구조체 반환.
 * 22.07의 dev->devargs 직접 접근 대체. devargs에는 디바이스별 파라미터 키-값이 포함. */
const struct rte_devargs *
rte_dev_devargs(const struct rte_device *dev);

/**
 * Retrieve a device driver.
 *
 * @param dev
 *   A pointer to a device structure.
 * @return
 *   A pointer to this device driver.
 */
/* [한국어] rte_dev_driver - rte_device에서 probe한 드라이버 포인터 반환.
 * 22.07의 dev->driver 직접 접근 대체. probe 전에는 NULL 반환. */
const struct rte_driver *
rte_dev_driver(const struct rte_device *dev);

/**
 * Retrieve a device name.
 *
 * @param dev
 *   A pointer to a device structure.
 * @return
 *   A pointer to this device name.
 */
/* [한국어] rte_dev_name - rte_device에서 디바이스 이름 문자열 반환.
 * 22.07의 dev->name 직접 접근 대체. PCI 버스에서는 "0000:04:00.0" 형식 BDF 문자열.
 * SPDK pci_dpdk_2211.c의 dpdk_pci_device_get_addr/id 등에서 이름 기반 탐색에 사용. */
const char *
rte_dev_name(const struct rte_device *dev);

/**
 * Retrieve a device numa node.
 *
 * @param dev
 *   A pointer to a device structure.
 * @return
 *   A pointer to this device numa node.
 */
/* [한국어] rte_dev_numa_node - rte_device의 NUMA 노드 번호 반환.
 * 22.07의 dev->numa_node 직접 접근 대체.
 * SPDK memory 할당 시 디바이스와 같은 NUMA 소켓의 메모리를 선호하는 결정에 사용. */
int
rte_dev_numa_node(const struct rte_device *dev);

/*
 * Internal identifier length
 * Sufficiently large to allow for UUID or PCI address
 */
/* [한국어] RTE_DEV_NAME_MAX_LEN - 디바이스 이름 문자열 최대 길이 (64바이트).
 * UUID(36자) 또는 PCI BDF 주소(12자)를 모두 수용할 수 있는 크기.
 * rte_pci_device.name 배열의 크기 기준(PCI_PRI_STR_SIZE+1이 이 값보다 작음). */
#define RTE_DEV_NAME_MAX_LEN 64

/**
 * Query status of a device.
 *
 * @param dev
 *   Generic device pointer.
 * @return
 *   (int)true if already probed successfully, 0 otherwise.
 */
/* [한국어] rte_dev_is_probed - 디바이스 probe 완료 여부 조회.
 * dev: 확인할 rte_device 포인터.
 * 반환: non-0(probe 완료), 0(probe 미완료 또는 NULL).
 * 22.11에서는 내부적으로 rte_dev_driver(dev) != NULL 로 구현. */
int rte_dev_is_probed(const struct rte_device *dev);

/**
 * Hotplug add a given device to a specific bus.
 *
 * In multi-process, it will request other processes to add the same device.
 * A failure, in any process, will rollback the action
 *
 * @param busname
 *   The bus name the device is added to.
 * @param devname
 *   The device name. Based on this device name, eal will identify a driver
 *   capable of handling it and pass it to the driver probing function.
 * @param drvargs
 *   Device arguments to be passed to the driver.
 * @return
 *   0 on success, negative on error.
 */
/* [한국어] rte_eal_hotplug_add - 지정 버스에 BDF 디바이스를 runtime add.
 * busname: "pci" 등 버스 이름. devname: "0000:04:00.0" 형식 BDF. drvargs: 드라이버 인자 문자열.
 * 반환: 0 성공, 음수 에러. 다중 프로세스 환경에서는 IPC로 전파, 실패 시 롤백.
 * SPDK pci.c pci_attach_rte가 이 함수를 호출하여 런타임에 새 NVMe 디바이스를 바인딩.
 * 호출 체인: spdk_pci_device_attach → pci_attach_rte → [rte_eal_hotplug_add]. */
int rte_eal_hotplug_add(const char *busname, const char *devname,
			const char *drvargs);

/**
 * Add matching devices.
 *
 * In multi-process, it will request other processes to add the same device.
 * A failure, in any process, will rollback the action
 *
 * @param devargs
 *   Device arguments including bus, class and driver properties.
 * @return
 *   0 on success, negative on error.
 */
/* [한국어] rte_dev_probe - devargs 통합 문자열로 디바이스 probe 트리거.
 * devargs: "pci:0000:04:00.0,key=val" 형식의 버스+이름+인자 통합 문자열.
 * rte_eal_hotplug_add보다 상위 레벨. SPDK는 직접 사용 안 함(rte_eal_hotplug_add 경로 선호). */
int rte_dev_probe(const char *devargs);

/**
 * Hotplug remove a given device from a specific bus.
 *
 * In multi-process, it will request other processes to remove the same device.
 * A failure, in any process, will rollback the action
 *
 * @param busname
 *   The bus name the device is removed from.
 * @param devname
 *   The device name being removed.
 * @return
 *   0 on success, negative on error.
 */
/* [한국어] rte_eal_hotplug_remove - 지정 버스에서 BDF 디바이스 runtime remove.
 * busname: "pci". devname: "0000:04:00.0" 형식 BDF.
 * 반환: 0 성공, 음수 에러. -ENOMSG 반환 시 IPC 충돌(SPDK pci.c는 4회까지 재시도).
 * 호출 체인: remove_rte_dev → [rte_eal_hotplug_remove] → PCI bus unplug. */
int rte_eal_hotplug_remove(const char *busname, const char *devname);

/**
 * Remove one device.
 *
 * In multi-process, it will request other processes to remove the same device.
 * A failure, in any process, will rollback the action
 *
 * @param dev
 *   Data structure of the device to remove.
 * @return
 *   0 on success, negative on error.
 */
/* [한국어] rte_dev_remove - rte_device 포인터 직접 기반 remove.
 * dev: 제거할 rte_device 포인터. BDF 문자열 불필요.
 * SPDK는 직접 사용 안 함(remove_rte_dev는 BDF 문자열 기반 rte_eal_hotplug_remove 사용). */
int rte_dev_remove(struct rte_device *dev);

/**
 * Device comparison function.
 *
 * This type of function is used to compare an rte_device with arbitrary
 * data.
 *
 * @param dev
 *   Device handle.
 *
 * @param data
 *   Data to compare against. The type of this parameter is determined by
 *   the kind of comparison performed by the function.
 *
 * @return
 *   0 if the device matches the data.
 *   !0 if the device does not match.
 *   <0 if ordering is possible and the device is lower than the data.
 *   >0 if ordering is possible and the device is greater than the data.
 */
/* [한국어] rte_dev_cmp_t - 디바이스 비교 콜백 타입.
 * rte_bus_find_device_t 등에서 탐색 기준 함수로 사용.
 * 반환: 0 일치, 비-0 불일치. 음수/양수로 정렬 순서 표현 가능. */
typedef int (*rte_dev_cmp_t)(const struct rte_device *dev, const void *data);

/* [한국어] RTE_PMD_EXPORT_NAME_ARRAY - PMD 이름 배열 변수 이름 생성 헬퍼.
 * n##idx[] 형식으로 인덱싱된 배열 이름을 생성. RTE_PMD_EXPORT_NAME에서 내부 사용. */
#define RTE_PMD_EXPORT_NAME_ARRAY(n, idx) n##idx[]

/* [한국어] RTE_PMD_EXPORT_NAME - 드라이버 이름을 __attribute__((used)) 정적 변수로 ELF 심볼에 기록.
 * 동적 로딩 시 런타임 링커가 드라이버 이름을 감지하여 초기화 순서 조정에 사용. */
#define RTE_PMD_EXPORT_NAME(name, idx) \
static const char RTE_PMD_EXPORT_NAME_ARRAY(this_pmd_name, idx) \
__rte_used = RTE_STR(name)

/* [한국어] DRV_EXP_TAG - 드라이버 확장 태그 이름 생성 헬퍼.
 * __<name>_<tag> 형식의 심볼 이름 생성. pci_tbl_export/param_string_export 등 태그에 사용. */
#define DRV_EXP_TAG(name, tag) __##name##_##tag

/* [한국어] RTE_PMD_REGISTER_PCI_TABLE - PCI ID 테이블 이름을 심볼에 기록.
 * 동적 로딩 시 PMD가 지원하는 디바이스 목록 광고. dpdk-devbind.py 등 도구가 참조. */
#define RTE_PMD_REGISTER_PCI_TABLE(name, table) \
static const char DRV_EXP_TAG(name, pci_tbl_export)[] __rte_used = \
RTE_STR(table)

/* [한국어] RTE_PMD_REGISTER_PARAM_STRING - 드라이버 파라미터 문자열을 심볼에 기록.
 * devargs 파싱 시 허용 키 목록 광고. 사용자가 잘못된 파라미터를 전달하면 경고 출력. */
#define RTE_PMD_REGISTER_PARAM_STRING(name, str) \
static const char DRV_EXP_TAG(name, param_string_export)[] \
__rte_used = str

/**
 * Advertise the list of kernel modules required to run this driver
 *
 * This string lists the kernel modules required for the devices
 * associated to a PMD. The format of each line of the string is:
 * "<device-pattern> <kmod-expression>".
 *
 * The possible formats for the device pattern are:
 *   "*"                     all devices supported by this driver
 *   "pci:*"                 all PCI devices supported by this driver
 *   "pci:v8086:d*:sv*:sd*"  all PCI devices supported by this driver
 *                           whose vendor id is 0x8086.
 *
 * The format of the kernel modules list is a parenthesized expression
 * containing logical-and (&) and logical-or (|).
 *
 * The device pattern and the kmod expression are separated by a space.
 *
 * Example:
 * - "* igb_uio | uio_pci_generic | vfio"
 */
/* [한국어] RTE_PMD_REGISTER_KMOD_DEP - 드라이버가 필요로 하는 커널 모듈 목록을 ELF 심볼에 기록.
 * dpdk-devbind.py 같은 사용자 도구가 이 심볼을 읽어 필요한 커널 모듈을 자동 로드.
 * SPDK 환경에서는 "vfio-pci | uio_pci_generic" 형태로 등록. */
#define RTE_PMD_REGISTER_KMOD_DEP(name, str) \
static const char DRV_EXP_TAG(name, kmod_dep_export)[] \
__rte_used = str

/**
 * Iteration context.
 *
 * This context carries over the current iteration state.
 */
/*
 * [한국어]
 * struct rte_dev_iterator - 디바이스 목록 순회 컨텍스트 (22.11)
 *
 * RTE_DEV_FOREACH 매크로와 rte_dev_iterator_init/next를 통해 버스·클래스 조건에
 * 맞는 디바이스를 순차 방문할 때 사용하는 상태 구조체. 22.07과 동일한 레이아웃.
 * 힙 할당 없이 스택 변수로 사용 가능. 순회 도중 포기해도 안전.
 */
struct rte_dev_iterator {
	const char *dev_str;
	/* [한국어] 전체 디바이스 서술 문자열 (예: "pci:0000:04:00.0,vfio").
	 * 설정자: rte_dev_iterator_init()이 호출자 제공 str을 저장.
	 * 읽는 자: rte_dev_iterator_next()가 다음 디바이스 탐색에 사용.
	 * 값 범위: NULL 불가; devargs 형식 준수.
	 * 동기화: 단일 스레드 전용. */

	const char *bus_str;
	/* [한국어] dev_str 중 버스 관련 부분 (예: "pci:0000:04:00.0").
	 * 설정자: rte_dev_iterator_init()이 dev_str 파싱 후 설정.
	 * 읽는 자: 버스 레이어 find_device가 이 값으로 디바이스 식별.
	 * 동기화: dev_str 수명에 종속. */

	const char *cls_str;
	/* [한국어] dev_str 중 클래스 관련 부분 (예: "eth_dev").
	 * 설정자: rte_dev_iterator_init()이 클래스 접두사 파싱 후 설정.
	 * 값 범위: 클래스 없는 순수 PCI 디바이스에서는 NULL. */

	struct rte_bus *bus;
	/* [한국어] 현재 순회 중인 버스 핸들.
	 * 설정자: rte_dev_iterator_init()이 bus_str로 버스 조회 후 설정.
	 * 읽는 자: rte_dev_iterator_next()가 해당 버스의 find_device 호출.
	 * 값 범위: 매칭 버스 없으면 NULL. */

	struct rte_class *cls;
	/* [한국어] 현재 순회 중인 클래스 핸들. 클래스 없는 순회에서는 NULL. */

	struct rte_device *device;
	/* [한국어] 현재 순회 위치 — 직전 next()가 반환한 rte_device.
	 * 설정자: rte_dev_iterator_next()가 다음 디바이스 찾은 후 갱신.
	 * 값 범위: 유효한 포인터 또는 순회 종료 시 NULL.
	 * 동기화: 단일 스레드 사용 전제. hotplug 동시 발생 시 dangling 위험. */

	void *class_device;
	/* [한국어] 클래스 레이어가 내부 상태 추적에 사용하는 불투명 포인터.
	 * 설정자: 클래스 iterate 콜백이 설정.
	 * 값 범위: 클래스 없는 순회에서는 NULL. */
};

/**
 * Device iteration function.
 *
 * Find the next device matching properties passed in parameters.
 * The function takes an additional ``start`` parameter, that is
 * used as starting context when relevant.
 *
 * The function returns the current element in the iteration.
 * This return value will potentially be used as a start parameter
 * in subsequent calls to the function.
 *
 * The additional iterator parameter is only there if a specific
 * implementation needs additional context. It must not be modified by
 * the iteration function itself.
 *
 * @param start
 *   Starting iteration context.
 *
 * @param devstr
 *   Device description string.
 *
 * @param it
 *   Device iterator.
 *
 * @return
 *   The address of the current element matching the device description
 *   string.
 */
/* [한국어] rte_dev_iterate_t - 버스/클래스 레이어 디바이스 순회 콜백 타입.
 * start: 이전 반환값(첫 호출 시 NULL). devstr: 조건 문자열. it: 이터레이터 컨텍스트.
 * 반환: 다음 매칭 디바이스 포인터; 없으면 NULL. */
typedef void *(*rte_dev_iterate_t)(const void *start,
				   const char *devstr,
				   const struct rte_dev_iterator *it);

/**
 * Initializes a device iterator.
 *
 * This iterator allows accessing a list of devices matching a criteria.
 * The device matching is made among all buses and classes currently registered,
 * filtered by the device description given as parameter.
 *
 * This function will not allocate any memory. It is safe to stop the
 * iteration at any moment and let the iterator go out of context.
 *
 * @param it
 *   Device iterator handle.
 *
 * @param str
 *   Device description string.
 *
 * @return
 *   0 on successful initialization.
 *   <0 on error.
 */
/* [한국어] rte_dev_iterator_init - 디바이스 이터레이터 초기화.
 * it: 초기화할 이터레이터 (스택 변수 가능). str: 디바이스 서술 문자열.
 * 반환: 0 성공, 음수 실패. 힙 할당 없음. 언제든 포기 가능.
 * 호출 체인: RTE_DEV_FOREACH 매크로 → [rte_dev_iterator_init] → 버스/클래스 레지스트리 조회. */
__rte_experimental
int
rte_dev_iterator_init(struct rte_dev_iterator *it, const char *str);

/**
 * Iterates on a device iterator.
 *
 * Generates a new rte_device handle corresponding to the next element
 * in the list described in comprehension by the iterator.
 *
 * The next object is returned, and the iterator is updated.
 *
 * @param it
 *   Device iterator handle.
 *
 * @return
 *   An rte_device handle if found.
 *   NULL if an error occurred (rte_errno is set).
 *   NULL if no device could be found (rte_errno is not set).
 */
/* [한국어] rte_dev_iterator_next - 이터레이터에서 다음 디바이스 반환.
 * it: init/이전 next로 갱신된 이터레이터. it->device를 다음 매칭 디바이스로 갱신.
 * 반환: 다음 rte_device; 에러 시 NULL+rte_errno; 순회 끝이면 NULL+rte_errno=0.
 * 호출 체인: RTE_DEV_FOREACH / 직접 호출 → [rte_dev_iterator_next] → 버스 find_device. */
__rte_experimental
struct rte_device *
rte_dev_iterator_next(struct rte_dev_iterator *it);

/* [한국어] RTE_DEV_FOREACH - 디바이스 서술 문자열에 매칭하는 모든 rte_device 순회 매크로.
 * dev: rte_device* 루프 변수. devstr: 디바이스 조건 문자열. it: rte_dev_iterator 포인터.
 * 내부: init 후 next 반복 호출. dev가 NULL이 될 때 종료. */
#define RTE_DEV_FOREACH(dev, devstr, it) \
	for (rte_dev_iterator_init(it, devstr), \
	     dev = rte_dev_iterator_next(it); \
	     dev != NULL; \
	     dev = rte_dev_iterator_next(it))

/**
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice
 *
 * It registers the callback for the specific device.
 * Multiple callbacks can be registered at the same time.
 *
 * @param device_name
 *  The device name, that is the param name of the struct rte_device,
 *  null value means for all devices.
 * @param cb_fn
 *  callback address.
 * @param cb_arg
 *  address of parameter for callback.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
/* [한국어] rte_dev_event_callback_register - hotplug 이벤트 콜백 등록.
 * device_name: 대상 디바이스 이름; NULL이면 모든 디바이스. cb_fn: 콜백. cb_arg: 사용자 인자.
 * 반환: 0 성공, 음수 실패. 동기화: DPDK 이벤트 목록 내부 락으로 스레드 안전 보장. */
__rte_experimental
int
rte_dev_event_callback_register(const char *device_name,
				rte_dev_event_cb_fn cb_fn,
				void *cb_arg);

/**
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice
 *
 * It unregisters the callback according to the specified device.
 *
 * @param device_name
 *  The device name, that is the param name of the struct rte_device,
 *  null value means for all devices and their callbacks.
 * @param cb_fn
 *  callback address.
 * @param cb_arg
 *  address of parameter for callback, (void *)-1 means to remove all
 *  registered which has the same callback address.
 *
 * @return
 *  - On success, return the number of callback entities removed.
 *  - On failure, a negative value.
 */
/* [한국어] rte_dev_event_callback_unregister - 등록된 hotplug 이벤트 콜백 해제.
 * cb_arg가 (void*)-1이면 동일 cb_fn의 모든 콜백 일괄 제거.
 * 반환: 제거된 콜백 수(성공 >=0), 음수 에러. */
__rte_experimental
int
rte_dev_event_callback_unregister(const char *device_name,
				  rte_dev_event_cb_fn cb_fn,
				  void *cb_arg);

/**
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice
 *
 * Executes all the user application registered callbacks for
 * the specific device.
 *
 * @param device_name
 *  The device name.
 * @param event
 *  the device event type.
 */
/* [한국어] rte_dev_event_callback_process - 지정 디바이스에 등록된 모든 콜백 즉시 실행.
 * pci_event.c의 udev 이벤트 감지 후 DPDK 레이어로 전파할 때 간접 사용. */
__rte_experimental
void
rte_dev_event_callback_process(const char *device_name,
			       enum rte_dev_event_type event);

/**
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice
 *
 * Start the device event monitoring.
 *
 * @return
 *   - On success, zero.
 *   - On failure, a negative value.
 */
__rte_experimental
int
rte_dev_event_monitor_start(void);

/**
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice
 *
 * Stop the device event monitoring.
 *
 * @return
 *   - On success, zero.
 *   - On failure, a negative value.
 */
__rte_experimental
int
rte_dev_event_monitor_stop(void);

/**
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice
 *
 * Enable hotplug handling for devices.
 *
 * @return
 *   - On success, zero.
 *   - On failure, a negative value.
 */
__rte_experimental
int
rte_dev_hotplug_handle_enable(void);

/**
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice
 *
 * Disable hotplug handling for devices.
 *
 * @return
 *   - On success, zero.
 *   - On failure, a negative value.
 */
__rte_experimental
int
rte_dev_hotplug_handle_disable(void);

/**
 * Device level DMA map function.
 * After a successful call, the memory segment will be mapped to the
 * given device.
 *
 * @note: Memory must be registered in advance using rte_extmem_* APIs.
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
__rte_experimental
int
rte_dev_dma_map(struct rte_device *dev, void *addr, uint64_t iova, size_t len);

/**
 * Device level DMA unmap function.
 * After a successful call, the memory segment will no longer be
 * accessible by the given device.
 *
 * @note: Memory must be registered in advance using rte_extmem_* APIs.
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
__rte_experimental
int
rte_dev_dma_unmap(struct rte_device *dev, void *addr, uint64_t iova,
		  size_t len);

#ifdef __cplusplus
}
#endif

#endif /* _RTE_DEV_H_ */
