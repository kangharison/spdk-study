/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] DPDK 22.11 (및 23.x/24.x/25.x 호환) ABI 백엔드 (pci_dpdk_2211.c)
 *
 * === 파일의 역할 ===
 * SPDK 의 env_dpdk 추상화 레이어에서, DPDK 22.11 LTS 와 그 이후 호환되는
 * 시리즈(23.03, 23.07, 23.11, 24.03/07/11, 25.03/07/11, 그리고 in-development
 * 26.03)에 대해 PCI/디바이스 ABI 구현을 제공한다. 22.11 부터 DPDK 가 PCI
 * 드라이버 헤더를 "내부용(bus_pci_driver.h)"과 "공개(rte_bus_pci.h)"로 분리
 * 하면서 SPDK 가 사용하던 일부 심볼이 internal 로 이동했고, intr_handle 도
 * opaque 화되어 직접 필드 액세스가 막혔다. 본 파일은 그 차이를 흡수한다.
 * 디스패처(pci_dpdk.c)는 22.11~25.x 의 검증된 릴리스에 대해 본 파일의
 * `fn_table_2211` 을 g_dpdk_fn_table 로 선택한다.
 *
 * 22.07 백엔드(pci_dpdk_2207.c)와 본 파일의 차이는 사실상 포함하는 헤더만
 * 다르고 함수 본체는 동일하다. SPDK 는 22.11 에서 22.07 와의 차이를 이
 * 두 헤더 세트(`22.07/*.h` vs `22.11/*.h`)로 흡수했으며, 추후 ABI 가 다시
 * 깨지면 새 pci_dpdk_2307.c 를 추가할 예정이다(파일 상단의 STATIC_ASSERT
 * 매크로 트릭이 그 가드 역할을 한다).
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [SPDK 호출자: lib/env_dpdk/pci.c, lib/env_dpdk/env.c]
 *           │
 *           ▼   (dpdk_pci_*  / dpdk_bus_*  / dpdk_device_* 호출)
 *   [pci_dpdk.c (디스패처) — g_dpdk_fn_table 경유]
 *           │
 *           ▼   (22.11~25.x 선택 시)
 *   [본 파일: pci_dpdk_2211.c — fn_table_2211]
 *           │
 *           ▼
 *   [DPDK 22.11+ 라이브러리: rte_pci_*, rte_intr_*, rte_bus_*]
 *
 * 실행 컨텍스트:
 *   · 정적 검증 매크로(SPDK_STATIC_ASSERT)는 빌드 타임에 평가.
 *   · 함수들은 호스트 유저스페이스에서 SPDK env 초기화 또는 PCI/NVMe probe
 *     단계에 호출됨. 인터럽트 efd 함수는 인터럽트 모드 활성화 시 사용.
 *
 * === 타 모듈과의 연결 ===
 * - 의존하는 SPDK 헤더:
 *     · pci_dpdk.h (struct dpdk_fn_table, struct spdk_pci_driver, dpdk_*
 *       선언). fn_table_2211 의 시그니처 계약.
 *     · spdk/assert.h (SPDK_STATIC_ASSERT, assert).
 * - 의존하는 vendored DPDK 22.11 헤더 (lib/env_dpdk/22.11/):
 *     · bus_pci_driver.h (22.11 부터 internal 로 분리된 rte_pci_driver 정의 등).
 *     · bus_driver.h (struct rte_bus 의 internal API — scan_mode 등).
 *     · rte_bus_pci.h (공개 PCI 디바이스 ABI — rte_pci_device, rte_pci_register,
 *       rte_pci_read/write_config).
 * - 의존하는 DPDK 런타임 라이브러리: librte_pci, librte_eal, librte_bus_pci.
 *
 * 데이터 흐름은 22.07 백엔드와 동일하며, 호출하는 함수가 22.11 ABI 의
 * 헤더에서 선언된 것이라는 점만 다르다.
 *
 * === 주요 함수/구조체 요약 ===
 * - SPDK_STATIC_ASSERT (파일 상단): driver_buf 의 offset==0 과 충분한 크기.
 * - 추가 트랩 매크로: rte_pci_mmio_read / rte_pci_mmio_write /
 *   rte_pci_pasid_set_state — DPDK 23.07+ 에서 추가된 API. SPDK 가 아직
 *   사용하지 않으므로, 누군가 잘못 호출하면 정적 어서트로 빌드 실패시켜
 *   "새 pci_dpdk_2307 백엔드를 만들어 달라"는 신호를 준다.
 * - pci_device_get_mem_resource_2211 : BAR 자원 디스크립터 반환 (22.07과 동일).
 * - pci_device_get_name / get_devargs / get_addr / get_id / get_numa_node :
 *   rte_pci_device 의 임베디드 필드 thin accessor.
 * - pci_device_read_config / write_config : DPDK 반환을 0/-1 로 정규화.
 * - pci_driver_register_2211 : SPDK 드라이버 → DPDK 드라이버 변환·등록.
 *   id_table 변환, "spdk_<name>" 할당, drv_flags 매핑, rte_pci_register.
 * - pci_device_*interrupt* / *efd* : rte_intr_* 디스패치 (intr_handle opaque).
 * - bus_probe / bus_scan : rte_bus_probe / rte_bus_scan 호출.
 * - device_get_devargs / set_devargs / get_name / scan_allowed : 공통 베이스
 *   `struct rte_device` 필드 접근.
 * - fn_table_2211 (파일 말미): 위 함수 포인터들의 정적 테이블.
 */

#define ALLOW_INTERNAL_API   /* [한국어] 22.11 부터 internal 로 옮겨진 일부 API(예: rte_pci_register, intr_handle 의 opaque accessor 일부) 사용을 허용. SPDK가 NVMe 드라이버로서 internal 심볼이 필요. */
#include <rte_config.h>      /* [한국어] DPDK 빌드 매크로. 22.11 헤더가 의존. */
#include <rte_version.h>     /* [한국어] 버전 매크로. 본 백엔드는 직접 분기에 쓰지 않지만 헤더 호환성을 위해 포함. */
#include "pci_dpdk.h"        /* [한국어] 본 파일이 채워야 하는 인터페이스(struct dpdk_fn_table)와 spdk_pci_driver 정의. */
#include "22.11/bus_pci_driver.h"   /* [한국어] 22.11 부터 internal 로 분리된 PCI 드라이버 ABI(rte_pci_driver 내부 필드 등). */
#include "22.11/bus_driver.h"       /* [한국어] 22.11 의 internal bus driver API — scan_mode 등 직접 액세스용. */
#include "22.11/rte_bus_pci.h"      /* [한국어] 22.11 의 공개 PCI 디바이스 ABI — rte_pci_device 레이아웃, rte_pci_read/write_config. */
#include "spdk/assert.h"     /* [한국어] SPDK_STATIC_ASSERT (빌드 타임 검증), assert. */

/*
 * [한국어]
 * 빌드 타임 검증: spdk_pci_driver 의 driver_buf 가 offset 0 에 있어야 한다.
 * 이유는 pci_driver_register_2211() 가 driver->driver_buf 를 그대로
 * rte_pci_driver 로 사용하기 때문이다(파일 시작 부의 SPDK_STATIC_ASSERT 와
 * 동일한 트릭). offset 이 어긋나면 캐스팅이 깨져 PCI 등록이 망가진다.
 */
SPDK_STATIC_ASSERT(offsetof(struct spdk_pci_driver, driver_buf) == 0, "driver_buf must be first");

/*
 * [한국어]
 * 빌드 타임 검증: driver_buf 가 22.11 의 rte_pci_driver 전체를 담을 만큼 큰지.
 * 22.11 에서 rte_pci_driver 가 22.07 보다 커졌을 가능성이 있어 별도 검증.
 */
SPDK_STATIC_ASSERT(offsetof(struct spdk_pci_driver, driver) >= sizeof(struct rte_pci_driver),
		   "driver_buf not big enough");

/* Following API was added in versions later than DPDK 22.11.
 * It is unused right now, if this changes a new pci_dpdk_* should be added.
 */
/*
 * [한국어]
 * 빌드 트랩: 23.07+ 에서 추가된 새 PCI MMIO/PASID API 들을 SPDK 가 잘못
 * 사용하면 빌드 시점에 정적 어서트로 실패시킨다. 메시지로 "새 pci_dpdk_2307
 * 백엔드를 추가해야 한다"고 안내. 본 백엔드는 22.11 호환만 보장하므로,
 * 새 API 가 필요해지는 순간 호환 가정이 무너지기 때문이다.
 *
 * 동작 원리: SPDK_STATIC_ASSERT(false, ...) 는 _Static_assert 로 컴파일 실패.
 * 가변 인자 매크로(...) 와 함께 쓰여, 호출 시 인자에 무관하게 트랩 발동.
 */
#define rte_pci_mmio_read(...) SPDK_STATIC_ASSERT(false, "rte_pci_mmio_read requires new pci_dpdk_2307 compat layer")
#define rte_pci_mmio_write(...) SPDK_STATIC_ASSERT(false, "rte_pci_mmio_write requires new pci_dpdk_2307 compat layer")
#define rte_pci_pasid_set_state(...) SPDK_STATIC_ASSERT(false, "rte_pci_pasid_set_state requires new pci_dpdk_2307 compat layer")

/*
 * [한국어]
 * pci_device_get_mem_resource_2211 - PCI BAR 자원 디스크립터 반환.
 *
 * @dev: 22.11 PCI 디바이스.
 * @bar: BAR 인덱스 (0..PCI_MAX_RESOURCE-1). NVMe 컨트롤러 레지스터는 BAR0.
 * @return: &dev->mem_resource[bar] 또는 NULL(bar 가 범위 초과).
 *
 * 22.07 백엔드와 본체 동일. mem_resource 배열은 22.11 에서도 같은 위치에
 * 유지되어 있다.
 *
 * 호출 체인: dpdk_pci_device_get_mem_resource → [본 함수] → 직접 인덱싱.
 */
static struct rte_mem_resource *
pci_device_get_mem_resource_2211(struct rte_pci_device *dev, uint32_t bar)
{
	if (bar >= PCI_MAX_RESOURCE) {  /* [한국어] PCI 표준 BAR 상한 검사 — 6 초과는 프로그래밍 에러. */
		assert(false);          /* [한국어] 디버그 빌드는 즉시 실패. */
		return NULL;            /* [한국어] 릴리스 빌드 안전 fallback. */
	}

	return &dev->mem_resource[bar]; /* [한국어] 22.11 의 rte_pci_device 도 mem_resource 배열을 직접 노출. */
}

/*
 * [한국어]
 * pci_device_get_name_2211 - 디바이스 이름(BDF 문자열) 반환.
 */
static const char *
pci_device_get_name_2211(struct rte_pci_device *rte_dev)
{
	return rte_dev->name;   /* [한국어] 22.11 의 rte_pci_device 안에 박혀 있는 BDF 문자열 직접 반환. */
}

/*
 * [한국어]
 * pci_device_get_devargs_2211 - PCI 디바이스의 devargs 반환.
 *
 * 22.11 도 22.07 처럼 임베디드 베이스(`device.devargs`)에서 직접 읽는다.
 */
static struct rte_devargs *
pci_device_get_devargs_2211(struct rte_pci_device *rte_dev)
{
	return rte_dev->device.devargs;   /* [한국어] PCI 디바이스 → 임베디드 rte_device → devargs. */
}

/*
 * [한국어]
 * pci_device_get_addr_2211 - PCI BDF 주소 반환.
 */
static struct rte_pci_addr *
pci_device_get_addr_2211(struct rte_pci_device *_dev)
{
	return &_dev->addr;   /* [한국어] domain/bus/devid/function 4튜플 주소 반환. */
}

/*
 * [한국어]
 * pci_device_get_id_2211 - PCI vendor/device/class id 반환.
 */
static struct rte_pci_id *
pci_device_get_id_2211(struct rte_pci_device *_dev)
{
	return &_dev->id;   /* [한국어] NVMe class match (0x010802) 등에 사용. */
}

/*
 * [한국어]
 * pci_device_get_numa_node_2211 - NUMA 노드 번호 반환.
 */
static int
pci_device_get_numa_node_2211(struct rte_pci_device *_dev)
{
	return _dev->device.numa_node;   /* [한국어] sysfs numa_node 에서 채워진 베이스 디바이스 필드. */
}

/*
 * [한국어]
 * pci_device_read_config_2211 - PCI configuration space 읽기 (22.11 ABI).
 *
 * @dev: 디바이스. @value: 호출자 버퍼. @len: 1/2/4. @offset: config 오프셋.
 * @return: 0 = 성공(정확히 len 읽음), -1 = 실패/부분.
 *
 * 22.07 와 동일하게 rte_pci_read_config 의 "읽은 바이트 수" 반환을 0/-1 로
 * 정규화. ABI 시그니처는 22.07 와 같다.
 */
static int
pci_device_read_config_2211(struct rte_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	int rc;   /* [한국어] DPDK 반환값 (읽은 바이트 수 또는 음수 errno). */

	rc = rte_pci_read_config(dev, value, len, offset);   /* [한국어] DPDK 22.11 의 PCI config 읽기 — VFIO/uio 경유. */

	return (rc > 0 && (uint32_t) rc == len) ? 0 : -1;   /* [한국어] 정확히 len 바이트면 0, 아니면 -1 로 정규화. */
}

/*
 * [한국어]
 * pci_device_write_config_2211 - PCI configuration space 쓰기 (22.11 ABI).
 *
 * #ifdef __FreeBSD__ 분기는 BSD DPDK 가 이미 0/-1 만 반환하기 때문.
 */
static int
pci_device_write_config_2211(struct rte_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	int rc;   /* [한국어] Linux: 쓴 바이트 수, BSD: 0/-1. */

	rc = rte_pci_write_config(dev, value, len, offset);   /* [한국어] PCI config 쓰기. 예: BME, MSE 비트 설정. */

#ifdef __FreeBSD__
	/* DPDK returns 0 on success and -1 on failure */
	/* [한국어] BSD DPDK 는 이미 0/-1 형식 — 그대로 반환. */
	return rc;
#endif
	return (rc > 0 && (uint32_t) rc == len) ? 0 : -1;   /* [한국어] Linux DPDK 의 "쓴 바이트 수"를 0/-1 로 정규화. */
}

/* translate spdk_pci_driver to an rte_pci_driver and register it to dpdk */
/*
 * [한국어]
 * pci_driver_register_2211 - SPDK 드라이버를 22.11 의 rte_pci_driver 로
 * 변환하여 등록.
 *
 * 본체 코드는 22.07 백엔드의 pci_driver_register_2207() 와 완전히 동일하다.
 * 차이는 단지 컴파일 시 포함하는 헤더 세트(22.11/bus_pci_driver.h)에서 오는
 * 구조체 레이아웃 일치. 22.11 부터 일부 필드가 internal 헤더로 이동했지만
 * SPDK 가 쓰는 필드명(probe, remove, drv_flags, id_table, driver.name)은
 * 동일하게 유지되어 있다.
 *
 * @driver: SPDK 드라이버. driver_buf 첫 256B 가 rte_pci_driver 본체.
 * @probe_fn: 디바이스 probe 콜백.
 * @remove_fn: hot-remove 콜백.
 * @return: 0 = 성공, -ENOMEM = 메모리 할당 실패.
 *
 * 호출 체인:
 *   dpdk_pci_driver_register → [본 함수] → rte_pci_register
 *      → DPDK 가 내부 드라이버 리스트에 추가.
 *
 * 단계는 2207 백엔드와 동일:
 *   1) id_table 카운트 → 2) calloc 으로 rte_id_table 생성 →
 *   3) SPDK id 필드 → RTE id 필드 1:1 복사 →
 *   4) "spdk_<name>" 이름 calloc/snprintf →
 *   5) drv_flags(NEED_MAPPING/WC_ACTIVATE) 매핑 →
 *   6) probe/remove 콜백 설치 + rte_pci_register.
 */
static int
pci_driver_register_2211(struct spdk_pci_driver *driver,
			 int (*probe_fn)(struct rte_pci_driver *driver, struct rte_pci_device *device),
			 int (*remove_fn)(struct rte_pci_device *device))

{
	unsigned pci_id_count = 0;       /* [한국어] SPDK id_table 엔트리 수 (sentinel 제외). */
	struct rte_pci_id *rte_id_table; /* [한국어] DPDK 측 ID 테이블 — 새로 calloc. */
	char *rte_name;                  /* [한국어] "spdk_<name>" 형식의 DPDK 드라이버 이름. */
	size_t rte_name_len;             /* [한국어] rte_name 버퍼 크기 (NUL 포함). */
	uint32_t rte_flags;              /* [한국어] SPDK drv_flags → RTE_PCI_DRV_* 변환 결과. */

	assert(driver->id_table);        /* [한국어] id_table 은 필수 — NULL 이면 등록 의미 없음. */
	while (driver->id_table[pci_id_count].vendor_id) {   /* [한국어] vendor_id==0 sentinel 까지 카운트. */
		pci_id_count++;
	}
	assert(pci_id_count > 0);        /* [한국어] 최소 1개 매칭 엔트리 필수. */

	rte_id_table = calloc(pci_id_count + 1, sizeof(*rte_id_table));   /* [한국어] +1: DPDK 가 요구하는 0-sentinel 자리. calloc 으로 0-초기화. */
	if (!rte_id_table) {
		return -ENOMEM;          /* [한국어] 할당 실패 → 호출자에 errno 전달. */
	}

	while (pci_id_count > 0) {       /* [한국어] 거꾸로 순회 — 인덱스 변수 재활용. */
		struct rte_pci_id *rte_id = &rte_id_table[pci_id_count - 1];        /* [한국어] DPDK 측 슬롯. */
		const struct spdk_pci_id *spdk_id = &driver->id_table[pci_id_count - 1];  /* [한국어] SPDK 원본 엔트리. */

		rte_id->class_id = spdk_id->class_id;                  /* [한국어] PCI Class Code (NVMe = 0x010802). */
		rte_id->vendor_id = spdk_id->vendor_id;                /* [한국어] 제조사 ID. */
		rte_id->device_id = spdk_id->device_id;                /* [한국어] 디바이스 ID. */
		rte_id->subsystem_vendor_id = spdk_id->subvendor_id;   /* [한국어] subsystem vendor — 명명 변환. */
		rte_id->subsystem_device_id = spdk_id->subdevice_id;   /* [한국어] subsystem device — 명명 변환. */
		pci_id_count--;          /* [한국어] 다음 엔트리로. */
	}

	assert(driver->name);            /* [한국어] 드라이버 이름은 필수. */
	rte_name_len = strlen(driver->name) + strlen("spdk_") + 1;   /* [한국어] "spdk_<name>" + NUL. SPDK 가 등록한 드라이버임을 prefix 로 표기. */
	rte_name = calloc(rte_name_len, 1);   /* [한국어] 0-초기화 문자열 버퍼. */
	if (!rte_name) {
		free(rte_id_table);      /* [한국어] 직전 calloc 누수 방지 — 등록 전이므로 안전. */
		return -ENOMEM;
	}

	snprintf(rte_name, rte_name_len, "spdk_%s", driver->name);   /* [한국어] "spdk_" prefix + 드라이버 이름. */
	driver->driver->driver.name = rte_name;                      /* [한국어] rte_pci_driver 임베디드 베이스의 name 설정 (DPDK 로그에 표시됨). */
	driver->driver->id_table = rte_id_table;                     /* [한국어] 위에서 변환한 id_table 설치. 소유권은 DPDK 측이 잡는다. */

	rte_flags = 0;                   /* [한국어] DPDK drv_flags 누적용. */
	if (driver->drv_flags & SPDK_PCI_DRIVER_NEED_MAPPING) {  /* [한국어] BAR 자동 매핑 요청 플래그. */
		rte_flags |= RTE_PCI_DRV_NEED_MAPPING;           /* [한국어] DPDK 가 probe 시 BAR 메모리를 mmap 하도록 지시. */
	}
	if (driver->drv_flags & SPDK_PCI_DRIVER_WC_ACTIVATE) {   /* [한국어] Write-Combining 매핑 요청 플래그. */
		rte_flags |= RTE_PCI_DRV_WC_ACTIVATE;            /* [한국어] DPDK 가 가능하면 WC 매핑 시도 — MMIO 쓰기 burst 최적화. */
	}
	driver->driver->drv_flags = rte_flags;   /* [한국어] 변환된 플래그 설치. */

	driver->driver->probe = probe_fn;        /* [한국어] DPDK probe 콜백 설치. */
	driver->driver->remove = remove_fn;      /* [한국어] hot-remove/detach 콜백 설치. */

	rte_pci_register(driver->driver);        /* [한국어] DPDK PCI 드라이버 레지스트리 추가 — 후속 rte_bus_probe() 가 매칭/probe 진행. */
	return 0;                                /* [한국어] 성공. */
}

/*
 * [한국어]
 * pci_device_enable_interrupt_2211 - 디바이스 인터럽트 활성화.
 *
 * 22.11 의 intr_handle 도 opaque 이므로 직접 필드 액세스 없이
 * rte_intr_enable() 에 그대로 전달한다. 22.07 백엔드와 본체 동일.
 */
static int
pci_device_enable_interrupt_2211(struct rte_pci_device *rte_dev)
{
	return rte_intr_enable(rte_dev->intr_handle);   /* [한국어] DPDK 의 인터럽트 마스크 해제 — VFIO/uio 의 EFD 활성화 포함. */
}

/*
 * [한국어]
 * pci_device_disable_interrupt_2211 - 디바이스 인터럽트 비활성화.
 */
static int
pci_device_disable_interrupt_2211(struct rte_pci_device *rte_dev)
{
	return rte_intr_disable(rte_dev->intr_handle);   /* [한국어] DPDK 측 인터럽트 마스크 셋 — 인터럽트 모드 해제. */
}

/*
 * [한국어]
 * pci_device_get_interrupt_efd_2211 - 단일 인터럽트 eventfd 반환.
 */
static int
pci_device_get_interrupt_efd_2211(struct rte_pci_device *rte_dev)
{
	return rte_intr_fd_get(rte_dev->intr_handle);   /* [한국어] opaque accessor — 22.11 도 22.07 와 동일하게 함수 호출만 허용. */
}

/*
 * [한국어]
 * pci_device_create_interrupt_efds_2211 - count 개의 efd 생성 (MSI-X 다중).
 */
static int
pci_device_create_interrupt_efds_2211(struct rte_pci_device *rte_dev, uint32_t count)
{
	return rte_intr_efd_enable(rte_dev->intr_handle, count);   /* [한국어] DPDK 가 count 개 eventfd 를 만들어 intr_handle 내부 배열에 저장. */
}

/*
 * [한국어]
 * pci_device_delete_interrupt_efds_2211 - 모든 efd 해제. detach 경로에서 호출.
 */
static void
pci_device_delete_interrupt_efds_2211(struct rte_pci_device *rte_dev)
{
	return rte_intr_efd_disable(rte_dev->intr_handle);   /* [한국어] efd 자원 해제. void 함수의 return 은 흐름 종료 표현. */
}

/*
 * [한국어]
 * pci_device_get_interrupt_efd_by_index_2211 - 인덱스 기반 efd 조회.
 */
static int
pci_device_get_interrupt_efd_by_index_2211(struct rte_pci_device *rte_dev, uint32_t index)
{
	return rte_intr_efds_index_get(rte_dev->intr_handle, index);   /* [한국어] 큐페어별 eventfd 조회. */
}

/*
 * [한국어]
 * pci_device_interrupt_cap_multi_2211 - MSI-X 다중 벡터 capability 조회.
 */
static int
pci_device_interrupt_cap_multi_2211(struct rte_pci_device *rte_dev)
{
	return rte_intr_cap_multiple(rte_dev->intr_handle);   /* [한국어] DPDK 가 디바이스 capability 를 점검 — 0/1 반환. */
}

/*
 * [한국어]
 * bus_probe_2211 - rte_bus_probe() thin wrapper. 22.07 와 동일한 본체.
 *
 * env 초기화 마지막 단계에서 1회 호출되어, 등록된 모든 드라이버의 probe
 * 콜백이 매칭된 디바이스마다 호출되도록 트리거한다.
 */
static int
bus_probe_2211(void)
{
	return rte_bus_probe();   /* [한국어] DPDK 22.11 의 버스 probe — 디바이스/드라이버 매칭 후 probe_fn 호출. */
}

/*
 * [한국어]
 * bus_scan_2211 - rte_bus_scan() thin wrapper.
 */
static void
bus_scan_2211(void)
{
	rte_bus_scan();   /* [한국어] DPDK 가 등록된 버스의 scan 콜백을 모두 실행 — 디바이스 enumerate. */
}

/*
 * [한국어]
 * device_get_devargs_2211 - 공통 베이스 rte_device 의 devargs 반환.
 */
static struct rte_devargs *
device_get_devargs_2211(struct rte_device *dev)
{
	return dev->devargs;   /* [한국어] EAL 인자 파싱으로 채워진 dev->devargs 직접 반환. */
}

/*
 * [한국어]
 * device_set_devargs_2211 - 공통 베이스 rte_device 에 devargs 설치.
 */
static void
device_set_devargs_2211(struct rte_device *dev, struct rte_devargs *devargs)
{
	dev->devargs = devargs;   /* [한국어] 단순 포인터 대입 — 22.11 에서도 베이스 디바이스 필드는 직접 액세스 가능. */
}

/*
 * [한국어]
 * device_get_name_2211 - 공통 베이스 디바이스 이름 반환.
 */
static const char *
device_get_name_2211(struct rte_device *dev)
{
	return dev->name;   /* [한국어] BDF/VDEV 이름 등 — 디바이스 종류에 따라 형식이 다름. */
}

/*
 * [한국어]
 * device_scan_allowed_2211 - 디바이스가 속한 버스의 scan 모드가 ALLOWLIST 인지 판정.
 *
 * 22.11 부터 bus 의 conf 구조체에 직접 액세스하려면 internal API
 * (bus_driver.h) 가 필요해서 ALLOW_INTERNAL_API 매크로가 켜져 있어야 한다.
 */
static bool
device_scan_allowed_2211(struct rte_device *dev)
{
	return dev->bus->conf.scan_mode == RTE_BUS_SCAN_ALLOWLIST;   /* [한국어] 22.11 internal API: dev → bus → conf.scan_mode 비교. ALLOWLIST 면 명시 디바이스만 사용. */
}

/*
 * [한국어]
 * fn_table_2211 - 22.11/23.x/24.x/25.x 호환 백엔드의 함수 포인터 테이블.
 *
 * 설정자: 본 파일에서 정적 초기화.
 * 읽는 자: pci_dpdk.c 의 dpdk_pci_init() 가 g_dpdk_fn_table 에 이 구조체의
 *          주소를 대입(런타임에 22.11 호환 버전이 선택된 경우).
 *          그 후 모든 dpdk_pci_*/dpdk_bus_*/dpdk_device_* 래퍼가 이 테이블의
 *          함수들을 호출.
 * 값 범위: 정적 초기화 — 런타임 변경 없음.
 * 동기화: 읽기 전용으로 다뤄지므로 락 불필요.
 *
 * 시그니처는 fn_table_2207 과 완전히 동일(struct dpdk_fn_table 의 모든 슬롯).
 * 디스패처는 어느 쪽이 활성인지 모르고도 동일하게 호출 가능.
 */
struct dpdk_fn_table fn_table_2211 = {
	.pci_device_get_mem_resource	= pci_device_get_mem_resource_2211,    /* [한국어] BAR 자원 디스크립터. */
	.pci_device_get_name		= pci_device_get_name_2211,            /* [한국어] BDF 문자열. */
	.pci_device_get_devargs		= pci_device_get_devargs_2211,         /* [한국어] PCI 디바이스 devargs. */
	.pci_device_get_addr		= pci_device_get_addr_2211,            /* [한국어] BDF 주소 구조체. */
	.pci_device_get_id		= pci_device_get_id_2211,              /* [한국어] vendor/device/class id. */
	.pci_device_get_numa_node	= pci_device_get_numa_node_2211,       /* [한국어] NUMA 노드 ID. */
	.pci_device_read_config		= pci_device_read_config_2211,         /* [한국어] PCI config 읽기. */
	.pci_device_write_config	= pci_device_write_config_2211,        /* [한국어] PCI config 쓰기. */
	.pci_driver_register		= pci_driver_register_2211,            /* [한국어] SPDK→DPDK 드라이버 변환·등록. */
	.pci_device_enable_interrupt	= pci_device_enable_interrupt_2211,    /* [한국어] 인터럽트 활성화. */
	.pci_device_disable_interrupt	= pci_device_disable_interrupt_2211,   /* [한국어] 인터럽트 비활성화. */
	.pci_device_get_interrupt_efd	= pci_device_get_interrupt_efd_2211,   /* [한국어] 단일 인터럽트 efd. */
	.pci_device_create_interrupt_efds = pci_device_create_interrupt_efds_2211,  /* [한국어] 다중 efd 생성. */
	.pci_device_delete_interrupt_efds = pci_device_delete_interrupt_efds_2211,  /* [한국어] efd 해제. */
	.pci_device_get_interrupt_efd_by_index = pci_device_get_interrupt_efd_by_index_2211,  /* [한국어] 인덱스 기반 efd 조회. */
	.pci_device_interrupt_cap_multi	= pci_device_interrupt_cap_multi_2211, /* [한국어] MSI-X 다중 벡터 지원 여부. */
	.bus_scan			= bus_scan_2211,                       /* [한국어] DPDK 버스 스캔. */
	.bus_probe			= bus_probe_2211,                      /* [한국어] DPDK 버스 probe. */
	.device_get_devargs		= device_get_devargs_2211,             /* [한국어] 공통 베이스 디바이스 devargs. */
	.device_set_devargs		= device_set_devargs_2211,             /* [한국어] 공통 베이스 디바이스에 devargs 설치. */
	.device_get_name		= device_get_name_2211,                /* [한국어] 공통 베이스 디바이스 이름. */
	.device_scan_allowed		= device_scan_allowed_2211,            /* [한국어] 버스가 ALLOWLIST 모드인지 판정. */
};
