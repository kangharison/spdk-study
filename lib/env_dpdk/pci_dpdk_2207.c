/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] DPDK 22.07 ABI 백엔드 (pci_dpdk_2207.c)
 *
 * === 파일의 역할 ===
 * SPDK 의 env_dpdk 추상화 레이어에서, DPDK 22.07 시리즈의 PCI/디바이스 ABI에
 * 맞춘 구체적 구현을 제공한다. 형제 파일 pci_dpdk.c 의 디스패처가 런타임에
 * `rte_version()` 을 보고 22.07 (또는 22.07 호환) 일 때 본 파일의
 * `fn_table_2207` 을 g_dpdk_fn_table 에 바인딩하면, 이후 SPDK 의 모든 PCI
 * 관련 호출이 본 파일의 함수들로 라우팅된다. 본 파일은 22.07 전용 헤더
 * (`22.07/rte_dev.h`, `22.07/rte_bus.h`, `22.07/rte_bus_pci.h` — vendored
 * private headers)를 포함하기 때문에 22.07 의 구조체 레이아웃과 인터럽트
 * API 의 형태에 맞게 동작한다.
 *
 * 핵심은 다음과 같다:
 *   1) 모든 함수가 static 으로 선언되고, 파일 끝의 `fn_table_2207` 구조체에
 *      함수 포인터로 모인다(외부 노출 심볼은 fn_table_2207 하나).
 *   2) 인터럽트 efd 관련 함수들이 `rte_intr_*` (예: rte_intr_fd_get) 라는
 *      "opaque intr_handle" API 를 사용한다. 22.07 부터 `intr_handle` 이
 *      포인터화되어 직접 필드 액세스가 막혔기 때문이다.
 *   3) `pci_driver_register_2207` 가 SPDK `struct spdk_pci_driver` 의
 *      driver_buf[256] 영역을 그대로 `rte_pci_driver` 로 캐스트하여 사용한다.
 *      이를 위해 파일 상단에 SPDK_STATIC_ASSERT 로 buf 가 충분히 크고
 *      offsetof(driver_buf)==0 임을 빌드 타임에 검증한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [SPDK 호출자: lib/env_dpdk/pci.c, lib/env_dpdk/env.c]
 *           │
 *           ▼   (dpdk_pci_*  / dpdk_bus_*  / dpdk_device_* 호출)
 *   [pci_dpdk.c (디스패처) — g_dpdk_fn_table 경유]
 *           │
 *           ▼   (22.07 선택 시)
 *   [본 파일: pci_dpdk_2207.c — fn_table_2207]
 *           │
 *           ▼
 *   [DPDK 22.07 라이브러리: rte_pci_*, rte_intr_*, rte_bus_*]
 *
 * 실행 컨텍스트: 모든 함수는 호스트 유저스페이스. 대부분 SPDK 환경 초기화
 * 또는 PCI/NVMe probe 단계에서 단일 reactor 스레드 컨텍스트에서 호출된다.
 * 인터럽트 efd 함수들은 SPDK가 인터럽트 모드를 옵션으로 활성화한 경우
 * I/O 큐 별 epoll 등록 시점에 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존하는 SPDK 헤더:
 *     · pci_dpdk.h (struct dpdk_fn_table, struct spdk_pci_driver, dpdk_*
 *       선언). fn_table_2207 의 시그니처 계약을 정의.
 *     · spdk/assert.h (SPDK_STATIC_ASSERT, assert).
 * - 의존하는 vendored DPDK 22.07 헤더 (lib/env_dpdk/22.07/):
 *     · rte_dev.h (struct rte_device, devargs).
 *     · rte_bus.h (struct rte_bus, scan_mode 등).
 *     · rte_bus_pci.h (struct rte_pci_device, struct rte_pci_driver,
 *       rte_pci_register, rte_pci_read/write_config).
 * - 의존하는 DPDK 런타임 라이브러리: librte_pci, librte_eal, librte_bus_pci.
 * - 의존하는 모듈: 본 파일을 직접 참조하는 모듈은 없고, 형제 파일
 *   pci_dpdk.c 만이 fn_table_2207 을 extern 으로 가져간다.
 *
 * 데이터 흐름:
 *   SPDK PCI 호출 → pci_dpdk.c 래퍼 → fn_table_2207.<func> →
 *   22.07 의 rte_pci_* 또는 rte_intr_* → 커널(VFIO/uio) 또는 DPDK 내부 캐시.
 *
 * === 주요 함수/구조체 요약 ===
 * - SPDK_STATIC_ASSERT (파일 상단): driver_buf 가 0 오프셋이고 rte_pci_driver
 *   를 담을 만큼 큰지 빌드 타임 검증.
 * - pci_device_get_mem_resource_2207 : BAR 인덱스 검증 후 dev->mem_resource
 *   배열 인덱스 반환.
 * - pci_device_get_name_2207 / get_devargs / get_addr / get_id /
 *   get_numa_node : rte_pci_device 구조체 필드 직접 액세스 thin wrapper.
 * - pci_device_read_config_2207 / write_config : rte_pci_read_config /
 *   rte_pci_write_config 호출 후 반환값을 0/-1 로 정규화 (FreeBSD ABI 분기 포함).
 * - pci_driver_register_2207 : spdk_pci_driver → rte_pci_driver 변환 본체.
 *   id_table 복사, "spdk_<name>" 이름 할당, drv_flags 매핑, probe/remove
 *   콜백 설치 후 rte_pci_register() 호출.
 * - pci_device_*interrupt* / *efd* 군: rte_intr_enable/disable, rte_intr_fd_get,
 *   rte_intr_efd_enable/disable, rte_intr_efds_index_get, rte_intr_cap_multiple
 *   를 디스패치. 22.07 부터 intr_handle 이 opaque pointer 인 점이 22.11과 동일.
 * - bus_probe_2207 / bus_scan_2207 : rte_bus_probe / rte_bus_scan thin wrapper.
 * - device_get_devargs / set_devargs / get_name / scan_allowed : `struct
 *   rte_device` 공통 필드 액세스.
 * - fn_table_2207 (파일 말미): 위 함수 포인터들을 모아 디스패처에 노출.
 */

#define ALLOW_INTERNAL_API   /* [한국어] DPDK 가 내부용으로 표시한 API(rte_intr_*, rte_pci_register 일부)를 허용하기 위한 매크로. SPDK가 22.07 의 정식 외부 API만으로는 NVMe 동작에 필요한 기능을 충분히 노출하지 못해 internal symbol 도 사용한다. */
#include <rte_config.h>      /* [한국어] DPDK 빌드 매크로(엔디안, 캐시라인, ABI 옵션) — DPDK 헤더가 의존. */
#include <rte_version.h>     /* [한국어] 버전 매크로. 본 백엔드는 직접 분기에 쓰진 않지만 헤더 호환성을 위해 포함. */
#include "pci_dpdk.h"        /* [한국어] 본 파일이 채워야 하는 인터페이스(struct dpdk_fn_table)와 spdk_pci_driver 정의. */
#include "22.07/rte_dev.h"       /* [한국어] DPDK 22.07 의 struct rte_device 레이아웃 — devargs/numa_node 액세스에 필요. */
#include "22.07/rte_bus.h"       /* [한국어] struct rte_bus / rte_bus_scan/rte_bus_probe / scan_mode 정의. */
#include "22.07/rte_bus_pci.h"   /* [한국어] rte_pci_device, rte_pci_driver, rte_pci_register, rte_pci_read/write_config 등 PCI 핵심 ABI. */
#include "spdk/assert.h"     /* [한국어] SPDK_STATIC_ASSERT (빌드 타임 검증), assert (런타임 디버그 검증). */

/*
 * [한국어]
 * 빌드 타임 검증: spdk_pci_driver 의 첫 256바이트가 driver_buf 여야 한다.
 *
 * 이유: pci_driver_register_2207() 가 driver->driver = (struct rte_pci_driver *)driver
 * 형태로 SPDK 구조체의 헤드 영역을 그대로 rte_pci_driver 로 사용하고,
 * 그 뒤에 SPDK 고유 필드(name, id_table, drv_flags 등)가 이어지도록 설계되어
 * 있다. driver_buf 의 offset 이 0 이 아니면 이 트릭이 깨진다.
 */
SPDK_STATIC_ASSERT(offsetof(struct spdk_pci_driver, driver_buf) == 0, "driver_buf must be first");

/*
 * [한국어]
 * 빌드 타임 검증: driver_buf 가 rte_pci_driver 전체를 담을 만큼 충분히 큰지.
 *
 * DPDK 의 rte_pci_driver 크기는 버전에 따라 늘어날 수 있어, SPDK 가 256B
 * 짜리 buf 를 미리 잡아두고 그 안에 들어가는지 빌드 시 검증한다. 이 검증
 * 은 22.07 의 sizeof(rte_pci_driver) 를 기준으로 통과해야 한다.
 */
SPDK_STATIC_ASSERT(offsetof(struct spdk_pci_driver, driver) >= sizeof(struct rte_pci_driver),
		   "driver_buf not big enough");

/*
 * [한국어]
 * pci_device_get_mem_resource_2207 - PCI BAR 자원 디스크립터 반환.
 *
 * @dev: DPDK 22.07 PCI 디바이스. probe 콜백에서 받은 핸들.
 * @bar: BAR 인덱스 (0..PCI_MAX_RESOURCE-1). NVMe 컨트롤러 레지스터는 BAR0.
 * @return: &dev->mem_resource[bar] (phys_addr/len/addr 포함) 또는 NULL.
 *
 * 동작: bar 가 PCI 표준 상한을 넘으면 assert 후 NULL — 호출자는 BAR 매핑
 * 실패로 처리해야 한다. NVMe 드라이버는 이 결과의 addr 필드(가상 매핑)를
 * MMIO 도어벨/레지스터 액세스에 사용한다.
 *
 * 호출 체인: dpdk_pci_device_get_mem_resource → [본 함수] →
 *            dev->mem_resource 직접 인덱싱(22.07 ABI).
 */
static struct rte_mem_resource *
pci_device_get_mem_resource_2207(struct rte_pci_device *dev, uint32_t bar)
{
	if (bar >= PCI_MAX_RESOURCE) {  /* [한국어] PCI 스펙 상 BAR 는 최대 6개(2개씩 64비트 결합 가능). 범위 초과는 프로그래밍 에러. */
		assert(false);          /* [한국어] 디버그 빌드에서는 즉시 실패시켜 호출자 버그 노출. */
		return NULL;            /* [한국어] 릴리스 빌드 fallback — 호출자에 NULL 반환. */
	}

	return &dev->mem_resource[bar]; /* [한국어] 22.07 의 rte_pci_device 는 mem_resource[PCI_MAX_RESOURCE] 배열을 직접 노출. 포인터 반환만으로 OK. */
}

/*
 * [한국어]
 * pci_device_get_name_2207 - rte_pci_device 의 name 필드(BDF 문자열) 반환.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @return: "0000:81:00.0" 같은 문자열. 라이프타임은 디바이스와 동일.
 *
 * 22.07 ABI 에서는 이름이 정적 char 배열로 디바이스 안에 박혀 있어 직접
 * 액세스 가능. 22.11 도 동일.
 */
static const char *
pci_device_get_name_2207(struct rte_pci_device *rte_dev)
{
	return rte_dev->name;  /* [한국어] 디바이스 구조체 내부 BDF 문자열 직접 반환. */
}

/*
 * [한국어]
 * pci_device_get_devargs_2207 - 디바이스에 연결된 devargs 포인터 반환.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @return: rte_dev->device.devargs 또는 NULL.
 *
 * 22.07 의 rte_pci_device 는 베이스 클래스 `struct rte_device device` 를
 * 임베디드 멤버로 갖고, 그 안에 devargs 가 위치한다.
 */
static struct rte_devargs *
pci_device_get_devargs_2207(struct rte_pci_device *rte_dev)
{
	return rte_dev->device.devargs;  /* [한국어] PCI 디바이스 → 임베디드 베이스 → devargs. */
}

/*
 * [한국어]
 * pci_device_get_addr_2207 - PCI BDF 주소 구조체 포인터 반환.
 *
 * @_dev: PCI 디바이스 핸들.
 * @return: &_dev->addr (domain, bus, devid, function 4튜플).
 *
 * SPDK 측 spdk_pci_addr_parse 와의 변환에 사용.
 */
static struct rte_pci_addr *
pci_device_get_addr_2207(struct rte_pci_device *_dev)
{
	return &_dev->addr;   /* [한국어] 임베디드 addr 필드 주소 반환 — 호출자가 읽기 전용으로 사용. */
}

/*
 * [한국어]
 * pci_device_get_id_2207 - PCI vendor/device/class id 구조체 포인터 반환.
 *
 * @_dev: PCI 디바이스 핸들.
 * @return: &_dev->id. NVMe 매칭(class==0x010802)에 사용.
 */
static struct rte_pci_id *
pci_device_get_id_2207(struct rte_pci_device *_dev)
{
	return &_dev->id;   /* [한국어] 임베디드 id 필드 주소 반환. */
}

/*
 * [한국어]
 * pci_device_get_numa_node_2207 - 디바이스의 NUMA 노드 번호 반환.
 *
 * @_dev: PCI 디바이스 핸들.
 * @return: NUMA 노드 ID (-1 = unknown). SPDK는 디바이스와 같은 노드의
 *          hugepage 메모리 풀에서 DMA 버퍼를 잡아 cross-NUMA 지연을 회피.
 */
static int
pci_device_get_numa_node_2207(struct rte_pci_device *_dev)
{
	return _dev->device.numa_node;  /* [한국어] 임베디드 베이스 디바이스의 numa_node 필드 — sysfs 의 numa_node 값에서 채워짐. */
}

/*
 * [한국어]
 * pci_device_read_config_2207 - PCI configuration space 읽기.
 *
 * @dev: PCI 디바이스 핸들.
 * @value: 호출자 버퍼 (>=len 바이트).
 * @len: 읽을 바이트 수 (1/2/4 권장).
 * @offset: PCI config space 오프셋 (legacy 256B 또는 extended 4KB).
 * @return: 0 = 정확히 len 바이트 읽음, -1 = 부분 읽기/실패.
 *
 * 22.07 의 rte_pci_read_config 는 "읽은 바이트 수"를 반환하므로 SPDK 는
 * "len 과 같으면 0, 아니면 -1" 로 정규화한다.
 *
 * 호출 체인: dpdk_pci_device_read_config → [본 함수] →
 *            rte_pci_read_config (DPDK 22.07) → VFIO/uio 경유 sysfs config.
 */
static int
pci_device_read_config_2207(struct rte_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	int rc;   /* [한국어] DPDK 가 반환한 "읽은 바이트 수" 또는 음수 errno 보관. */

	rc = rte_pci_read_config(dev, value, len, offset);  /* [한국어] DPDK 의 PCI config 읽기. 내부적으로 sysfs config 또는 VFIO ioctl 사용. */

	return (rc > 0 && (uint32_t) rc == len) ? 0 : -1;   /* [한국어] 정확히 len 바이트면 0, 그 외(부분 읽기, 음수 errno)는 -1 로 정규화 — SPDK 호출자에 단순한 실패 시그널 제공. */
}

/*
 * [한국어]
 * pci_device_write_config_2207 - PCI configuration space 쓰기.
 *
 * @dev: PCI 디바이스.
 * @value: 쓸 데이터 버퍼.
 * @len: 쓸 바이트 수.
 * @offset: 쓸 오프셋.
 * @return: 0 = 성공, -1 = 실패.
 *
 * #ifdef __FreeBSD__ 분기: BSD 의 DPDK 는 이미 0/-1 만 반환하므로 그대로
 * 패스. Linux DPDK 는 "쓴 바이트 수"를 반환하므로 0/-1 로 정규화 필요.
 */
static int
pci_device_write_config_2207(struct rte_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	int rc;   /* [한국어] DPDK 반환값 보관 (Linux: 쓴 바이트 수, BSD: 0/-1). */

	rc = rte_pci_write_config(dev, value, len, offset);   /* [한국어] PCI config 쓰기. 예: Bus Master Enable 비트 셋. */

#ifdef __FreeBSD__
	/* DPDK returns 0 on success and -1 on failure */
	/* [한국어] FreeBSD 빌드의 DPDK는 이미 0/-1 시그널 형식이라 그대로 반환. */
	return rc;
#endif
	return (rc > 0 && (uint32_t) rc == len) ? 0 : -1;   /* [한국어] Linux DPDK 의 "쓴 바이트 수" 를 0/-1 로 정규화. */
}

/* translate spdk_pci_driver to an rte_pci_driver and register it to dpdk */
/*
 * [한국어]
 * pci_driver_register_2207 - SPDK 드라이버 디스크립터를 22.07 의 rte_pci_driver
 * 로 변환 후 DPDK 에 등록.
 *
 * @driver: SPDK 드라이버 (id_table, name, drv_flags, cb_fn 보유).
 *          driver->driver_buf 의 처음 256B 가 그대로 rte_pci_driver 로 캐스팅
 *          됨(파일 상단 SPDK_STATIC_ASSERT 로 안전성 보장).
 * @probe_fn: 디바이스 probe 콜백 (DPDK가 매칭된 디바이스마다 호출).
 * @remove_fn: 디바이스 remove 콜백 (hot-remove/탈출 시).
 * @return: 0 = 성공, -ENOMEM = 메모리 할당 실패.
 *
 * 동작 단계:
 *   1) id_table 의 (vendor_id==0) sentinel 까지 스캔하여 엔트리 수 카운트.
 *   2) (count+1) 크기의 rte_pci_id 배열 calloc — 마지막 엔트리는 sentinel.
 *   3) 거꾸로 순회하며 SPDK id 필드를 RTE id 필드에 1:1 복사
 *      (class_id, vendor_id, device_id, subvendor → subsystem_vendor_id,
 *       subdevice → subsystem_device_id).
 *   4) "spdk_<driver->name>" 형식 문자열 calloc 후 driver->driver->driver.name
 *      에 설치. 이 이름은 DPDK 로그/에러 메시지에 표시됨.
 *   5) drv_flags 매핑: SPDK_PCI_DRIVER_NEED_MAPPING → RTE_PCI_DRV_NEED_MAPPING
 *      (BAR 자동 매핑 요청), SPDK_PCI_DRIVER_WC_ACTIVATE → RTE_PCI_DRV_WC_ACTIVATE
 *      (Write-Combining 매핑 요청).
 *   6) probe/remove 콜백 설치, rte_pci_register() 호출.
 *
 * 호출 체인:
 *   dpdk_pci_driver_register → [본 함수] → rte_pci_register
 *      → DPDK 가 내부 드라이버 리스트에 추가 → 후속 bus_probe 시 매칭 디바이스
 *        마다 probe_fn 콜백.
 *
 * 실행 컨텍스트: SPDK_PCI_DRIVER_REGISTER 매크로 또는 명시적 등록 단계 (모듈
 * 초기화 시점, 단일 스레드).
 *
 * 누수 주의: 정상 등록 후에는 rte_id_table 과 rte_name 의 소유권이 DPDK 측
 * (rte_pci_driver) 으로 이전된다. 별도 free 는 하지 않으며, 등록 해제 경로
 * 가 호출되면 그때 같이 해제되어야 한다.
 */
static int
pci_driver_register_2207(struct spdk_pci_driver *driver,
			 int (*probe_fn)(struct rte_pci_driver *driver, struct rte_pci_device *device),
			 int (*remove_fn)(struct rte_pci_device *device))

{
	unsigned pci_id_count = 0;       /* [한국어] SPDK id_table 엔트리 수 (sentinel 제외). */
	struct rte_pci_id *rte_id_table; /* [한국어] DPDK 측 ID 테이블 — calloc 으로 새로 할당. */
	char *rte_name;                  /* [한국어] "spdk_<name>" 형식의 DPDK 드라이버 이름. */
	size_t rte_name_len;             /* [한국어] rte_name 버퍼 크기(NUL 포함). */
	uint32_t rte_flags;              /* [한국어] SPDK drv_flags 를 RTE_PCI_DRV_* 로 변환한 값. */

	assert(driver->id_table);        /* [한국어] id_table 은 필수 — NULL 이면 등록 자체가 무의미. */
	while (driver->id_table[pci_id_count].vendor_id) {  /* [한국어] vendor_id==0 인 sentinel 까지 카운트. SPDK 관례상 마지막 엔트리는 0 으로 끝남. */
		pci_id_count++;
	}
	assert(pci_id_count > 0);        /* [한국어] 최소 1개의 매칭 엔트리는 있어야 함. */

	rte_id_table = calloc(pci_id_count + 1, sizeof(*rte_id_table));   /* [한국어] +1 은 DPDK 가 요구하는 sentinel(0-인 마지막 엔트리) 자리. calloc 으로 0-초기화. */
	if (!rte_id_table) {
		return -ENOMEM;          /* [한국어] 메모리 부족 — 호출자에 errno 반환. */
	}

	while (pci_id_count > 0) {       /* [한국어] 거꾸로 순회 — count 변수를 그대로 인덱스로 재활용. */
		struct rte_pci_id *rte_id = &rte_id_table[pci_id_count - 1];        /* [한국어] DPDK 측 슬롯. */
		const struct spdk_pci_id *spdk_id = &driver->id_table[pci_id_count - 1];  /* [한국어] SPDK 측 원본 엔트리. */

		rte_id->class_id = spdk_id->class_id;                  /* [한국어] PCI Class Code (예: NVMe = 0x010802). */
		rte_id->vendor_id = spdk_id->vendor_id;                /* [한국어] 제조사 ID (예: Intel = 0x8086). */
		rte_id->device_id = spdk_id->device_id;                /* [한국어] 디바이스 ID. */
		rte_id->subsystem_vendor_id = spdk_id->subvendor_id;   /* [한국어] 서브시스템 vendor — SPDK 와 DPDK 명명 차이 변환. */
		rte_id->subsystem_device_id = spdk_id->subdevice_id;   /* [한국어] 서브시스템 device — 위와 동일. */
		pci_id_count--;          /* [한국어] 다음 엔트리로 이동. 0 에 도달하면 루프 종료. */
	}

	assert(driver->name);            /* [한국어] 드라이버 이름은 필수 — DPDK 로그에 사용. */
	rte_name_len = strlen(driver->name) + strlen("spdk_") + 1;  /* [한국어] "spdk_<name>" + NUL — 충돌 회피용 prefix. */
	rte_name = calloc(rte_name_len, 1);     /* [한국어] 0-초기화된 문자열 버퍼 — snprintf 가 NUL 보장. */
	if (!rte_name) {
		free(rte_id_table);      /* [한국어] 직전 calloc 누수 방지 — 정상 등록 전이므로 안전하게 free. */
		return -ENOMEM;
	}

	snprintf(rte_name, rte_name_len, "spdk_%s", driver->name);   /* [한국어] "spdk_" + 드라이버 이름. */
	driver->driver->driver.name = rte_name;                      /* [한국어] rte_pci_driver 임베디드 베이스의 name 필드 설정. */
	driver->driver->id_table = rte_id_table;                     /* [한국어] 위에서 변환한 id 테이블 설치 — 소유권은 DPDK 가 잡는다. */

	rte_flags = 0;                   /* [한국어] DPDK drv_flags 누적. */
	if (driver->drv_flags & SPDK_PCI_DRIVER_NEED_MAPPING) {  /* [한국어] BAR 자동 매핑이 필요하다는 SPDK 플래그. */
		rte_flags |= RTE_PCI_DRV_NEED_MAPPING;           /* [한국어] DPDK 가 probe 시 BAR 메모리를 mmap 해 줄 것을 요청. */
	}
	if (driver->drv_flags & SPDK_PCI_DRIVER_WC_ACTIVATE) {   /* [한국어] Write-Combining (PCIe MMIO 성능 최적화) 요청. */
		rte_flags |= RTE_PCI_DRV_WC_ACTIVATE;            /* [한국어] DPDK 가 WC 매핑을 시도하도록 지시. */
	}
	driver->driver->drv_flags = rte_flags;   /* [한국어] 변환된 플래그 설치. */

	driver->driver->probe = probe_fn;        /* [한국어] DPDK 가 매칭 디바이스마다 호출할 probe 함수. */
	driver->driver->remove = remove_fn;      /* [한국어] hot-remove / detach 시 호출되는 정리 함수. */

	rte_pci_register(driver->driver);        /* [한국어] DPDK 의 PCI 드라이버 레지스트리에 추가. 이후 rte_bus_probe() 가 매칭을 진행. */
	return 0;                                /* [한국어] 성공. */
}

/*
 * [한국어]
 * pci_device_enable_interrupt_2207 - 디바이스 인터럽트 활성화.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @return: 0 또는 음수 errno.
 *
 * 22.07 부터 intr_handle 이 opaque pointer 형태이므로 rte_intr_enable() 에
 * 그대로 전달한다(필드 직접 액세스 금지).
 */
static int
pci_device_enable_interrupt_2207(struct rte_pci_device *rte_dev)
{
	return rte_intr_enable(rte_dev->intr_handle);   /* [한국어] DPDK 22.07: intr_handle 은 opaque — VFIO/uio 의 인터럽트 마스크 해제. */
}

/*
 * [한국어]
 * pci_device_disable_interrupt_2207 - 디바이스 인터럽트 비활성화.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @return: 0 또는 음수 errno.
 */
static int
pci_device_disable_interrupt_2207(struct rte_pci_device *rte_dev)
{
	return rte_intr_disable(rte_dev->intr_handle);   /* [한국어] DPDK 의 인터럽트 비활성화 호출 — VFIO/uio 마스크 셋. */
}

/*
 * [한국어]
 * pci_device_get_interrupt_efd_2207 - 디바이스의 (단일) 인터럽트 eventfd 반환.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @return: eventfd (>=0) 또는 음수 errno.
 *
 * SPDK 가 epoll/poller 에 등록해 인터럽트 모드 폴링에 활용.
 */
static int
pci_device_get_interrupt_efd_2207(struct rte_pci_device *rte_dev)
{
	return rte_intr_fd_get(rte_dev->intr_handle);   /* [한국어] 22.07 부터 추가된 opaque accessor. 직접 intr_handle->fd 접근 금지. */
}

/*
 * [한국어]
 * pci_device_create_interrupt_efds_2207 - MSI-X 다중 벡터용 efd 배열 생성.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @count: 생성할 efd 수 (보통 NVMe IO 큐페어 수).
 * @return: 0 또는 음수 errno.
 */
static int
pci_device_create_interrupt_efds_2207(struct rte_pci_device *rte_dev, uint32_t count)
{
	return rte_intr_efd_enable(rte_dev->intr_handle, count);   /* [한국어] DPDK 가 count 개의 eventfd 를 만들어 intr_handle 내부 배열에 저장. */
}

/*
 * [한국어]
 * pci_device_delete_interrupt_efds_2207 - create 의 짝. 모든 efd 해제.
 *
 * @rte_dev: PCI 디바이스 핸들.
 *
 * 반환은 void. detach 경로에서 호출.
 */
static void
pci_device_delete_interrupt_efds_2207(struct rte_pci_device *rte_dev)
{
	return rte_intr_efd_disable(rte_dev->intr_handle);   /* [한국어] efd 자원 해제. void 함수의 return 은 단지 호출 흐름 종료 표현. */
}

/*
 * [한국어]
 * pci_device_get_interrupt_efd_by_index_2207 - 인덱스로 efd 조회.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @index: efd 배열 인덱스.
 * @return: eventfd 또는 음수 errno.
 */
static int
pci_device_get_interrupt_efd_by_index_2207(struct rte_pci_device *rte_dev, uint32_t index)
{
	return rte_intr_efds_index_get(rte_dev->intr_handle, index);   /* [한국어] 22.07 의 opaque accessor — 큐페어별 eventfd 조회. */
}

/*
 * [한국어]
 * pci_device_interrupt_cap_multi_2207 - MSI-X 다중 벡터 지원 여부.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @return: 1 = 다중 지원, 0 = 단일만 지원.
 */
static int
pci_device_interrupt_cap_multi_2207(struct rte_pci_device *rte_dev)
{
	return rte_intr_cap_multiple(rte_dev->intr_handle);   /* [한국어] DPDK 가 디바이스 capability 를 점검하여 0/1 반환. */
}

/*
 * [한국어]
 * bus_probe_2207 - rte_bus_probe() thin wrapper.
 *
 * 등록된 모든 버스(PCI, VDEV, AUX 등)의 디바이스에 대해 매칭된 드라이버
 * probe 콜백을 호출. SPDK env 초기화 마지막 단계에서 1회 실행된다.
 *
 * @return: 0 = 모든 probe 성공, 음수 = 실패.
 */
static int
bus_probe_2207(void)
{
	return rte_bus_probe();   /* [한국어] DPDK 22.07 버스 probe — 모든 디바이스 드라이버에 probe 디스패치. */
}

/*
 * [한국어]
 * bus_scan_2207 - rte_bus_scan() thin wrapper.
 *
 * 시스템상의 디바이스 enumerate (PCI sysfs, VDEV 인자 파싱 등). 매칭/probe
 * 는 별도의 bus_probe 단계에서.
 */
static void
bus_scan_2207(void)
{
	rte_bus_scan();   /* [한국어] DPDK 가 등록된 버스의 scan 콜백을 모두 실행 — 디바이스 리스트 채우기. */
}

/*
 * [한국어]
 * device_get_devargs_2207 - 공통 베이스 rte_device 의 devargs 반환.
 *
 * @dev: PCI/VDEV/AUX 등 공통 베이스.
 * @return: dev->devargs 또는 NULL.
 */
static struct rte_devargs *
device_get_devargs_2207(struct rte_device *dev)
{
	return dev->devargs;   /* [한국어] EAL 인자 파싱 시 dev 에 연결된 옵션 문자열. */
}

/*
 * [한국어]
 * device_set_devargs_2207 - 공통 베이스 디바이스에 devargs 설치.
 *
 * @dev: 대상 디바이스.
 * @devargs: 설치할 인자 (호출자가 동적 생성한 경우 소유권 이전).
 *
 * SPDK 가 디바이스를 동적으로 추가할 때 옵션을 주입.
 */
static void
device_set_devargs_2207(struct rte_device *dev, struct rte_devargs *devargs)
{
	dev->devargs = devargs;   /* [한국어] 단순 포인터 대입 — 22.07 ABI 가 직접 필드 액세스를 허용. */
}

/*
 * [한국어]
 * device_get_name_2207 - 공통 베이스 디바이스 이름 반환.
 *
 * @dev: 대상 디바이스.
 * @return: dev->name (라이프타임은 dev 와 동일).
 */
static const char *
device_get_name_2207(struct rte_device *dev)
{
	return dev->name;   /* [한국어] BDF/VDEV 이름 등 — 디바이스 종류에 따라 형식이 다름. */
}

/*
 * [한국어]
 * device_scan_allowed_2207 - 디바이스가 속한 버스가 ALLOWLIST 모드인지 판정.
 *
 * @dev: 대상 디바이스.
 * @return: true = ALLOWLIST(명시 허용만), false = BLOCKLIST(기본 허용).
 *
 * SPDK 가 -a/-b EAL 인자에 따라 동작 모드를 가르치는 데 활용.
 */
static bool
device_scan_allowed_2207(struct rte_device *dev)
{
	return dev->bus->conf.scan_mode == RTE_BUS_SCAN_ALLOWLIST;   /* [한국어] 22.07: bus 구조체의 conf.scan_mode 를 직접 비교. ALLOWLIST 모드는 명시된 디바이스만 사용. */
}

/*
 * [한국어]
 * fn_table_2207 - 위에서 정의한 22.07 백엔드 구현들을 모은 함수 포인터 테이블.
 *
 * 설정자: 본 파일에서 정적 초기화 (빌드 타임 고정).
 * 읽는 자: 형제 파일 pci_dpdk.c 의 dpdk_pci_init() 가 g_dpdk_fn_table 에
 *          이 구조체의 주소를 대입(런타임에 22.07 이 선택된 경우).
 *          그 후 모든 dpdk_pci_*/dpdk_bus_*/dpdk_device_* 래퍼가 이 테이블의
 *          함수들을 호출.
 * 값 범위: const-like 정적 초기화 — 런타임 변경 없음.
 * 동기화: 읽기 전용으로 다뤄지므로 락 불필요.
 *
 * 인터페이스 계약은 pci_dpdk.h 의 struct dpdk_fn_table 정의에 따른다.
 * 22.11 백엔드(fn_table_2211)와 시그니처가 일치하므로 디스패처는 어느
 * 백엔드를 선택했는지 모르고도 동일한 호출을 수행할 수 있다.
 */
struct dpdk_fn_table fn_table_2207 = {
	.pci_device_get_mem_resource	= pci_device_get_mem_resource_2207,    /* [한국어] BAR 자원 디스크립터 반환. */
	.pci_device_get_name		= pci_device_get_name_2207,            /* [한국어] BDF 문자열. */
	.pci_device_get_devargs		= pci_device_get_devargs_2207,         /* [한국어] PCI 디바이스 devargs. */
	.pci_device_get_addr		= pci_device_get_addr_2207,            /* [한국어] BDF 주소 구조체. */
	.pci_device_get_id		= pci_device_get_id_2207,              /* [한국어] vendor/device/class id. */
	.pci_device_get_numa_node	= pci_device_get_numa_node_2207,       /* [한국어] NUMA 노드 ID. */
	.pci_device_read_config		= pci_device_read_config_2207,         /* [한국어] PCI config 읽기. */
	.pci_device_write_config	= pci_device_write_config_2207,        /* [한국어] PCI config 쓰기. */
	.pci_driver_register		= pci_driver_register_2207,            /* [한국어] SPDK 드라이버 → DPDK 드라이버 변환·등록. */
	.pci_device_enable_interrupt	= pci_device_enable_interrupt_2207,    /* [한국어] 인터럽트 활성화. */
	.pci_device_disable_interrupt	= pci_device_disable_interrupt_2207,   /* [한국어] 인터럽트 비활성화. */
	.pci_device_get_interrupt_efd	= pci_device_get_interrupt_efd_2207,   /* [한국어] 단일 인터럽트 efd. */
	.pci_device_create_interrupt_efds = pci_device_create_interrupt_efds_2207,  /* [한국어] 다중 efd 생성. */
	.pci_device_delete_interrupt_efds = pci_device_delete_interrupt_efds_2207,  /* [한국어] efd 해제. */
	.pci_device_get_interrupt_efd_by_index = pci_device_get_interrupt_efd_by_index_2207,  /* [한국어] 인덱스 기반 efd 조회. */
	.pci_device_interrupt_cap_multi	= pci_device_interrupt_cap_multi_2207, /* [한국어] MSI-X 다중 벡터 지원 여부. */
	.bus_scan			= bus_scan_2207,                       /* [한국어] DPDK 버스 스캔. */
	.bus_probe			= bus_probe_2207,                      /* [한국어] DPDK 버스 probe. */
	.device_get_devargs		= device_get_devargs_2207,             /* [한국어] 공통 베이스 디바이스 devargs. */
	.device_set_devargs		= device_set_devargs_2207,             /* [한국어] 공통 베이스 디바이스에 devargs 설치. */
	.device_get_name		= device_get_name_2207,                /* [한국어] 공통 베이스 디바이스 이름. */
	.device_scan_allowed		= device_scan_allowed_2207,            /* [한국어] 버스가 ALLOWLIST 모드인지 판정. */
};
