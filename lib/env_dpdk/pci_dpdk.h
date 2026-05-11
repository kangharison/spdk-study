/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] DPDK 버전 추상화 레이어 헤더 (pci_dpdk.h)
 *
 * === 파일의 역할 ===
 * SPDK는 여러 DPDK 버전(21.11, 22.x, 23.x, 24.x …)에서 빌드 가능해야 하는데,
 * DPDK는 마이너 버전 사이에서도 rte_pci_device 등 내부 구조체의 멤버 레이아웃과
 * 함수 시그니처를 자주 바꾼다. 본 헤더는 그 차이를 흡수하는 "함수 포인터 테이블
 * (dpdk_fn_table)"과, 그 테이블을 통해 실제 호출을 위임하는 dpdk_pci_* 래퍼들을
 * 선언한다. 또한 spdk_pci_driver 구조체를 정의하여 SPDK 측 PCI 드라이버 식별/콜백을
 * DPDK 드라이버 핸들과 결합한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/env_dpdk 서브시스템에서 PCI 디바이스 접근 경로의 가장 아래쪽 어댑터 계층이다.
 * 호출 체인: SPDK 사용자 코드(예: lib/nvme의 nvme_pcie_ctrlr_construct) →
 * spdk_pci_device_*(pci.c) → dpdk_pci_device_*(본 헤더) → dpdk_fn_table.* (특정 DPDK 버전용 .c)
 * 실행 컨텍스트: 모두 호스트 유저스페이스, 보통 메인 스레드 또는 디바이스 attach 스레드에서 호출.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/env.h(spdk_pci_id, spdk_pci_enum_cb 등의 공개 타입), DPDK rte_pci/rte_dev API.
 * - 본 헤더에 의존: lib/env_dpdk/pci.c, lib/env_dpdk/pci_dpdk_2207.c, pci_dpdk_2211.c 등 버전별
 *   구현 파일과 SPDK PCI 드라이버 등록 코드(nvme/virtio/ioat 등).
 * - 데이터 흐름: SPDK PCI 드라이버 객체(spdk_pci_driver)가 dpdk_pci_driver_register를 통해
 *   DPDK 측 드라이버 등록 테이블에 들어가며, 이후 디바이스 probe 이벤트 발생 시 함수 포인터
 *   테이블(dpdk_fn_table)을 거쳐 BAR/cap 정보가 SPDK 드라이버에게 전달된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_pci_driver: SPDK 측 PCI 드라이버 핸들. driver_buf에 DPDK 측 rte_pci_driver를
 *   in-place로 만든다(버전별 크기 차이 흡수용 256바이트 버퍼).
 * - struct dpdk_fn_table: 모든 DPDK PCI 호출의 함수 포인터 모음. dpdk_pci_init이 버전을
 *   감지하여 적절한 테이블을 활성화한다.
 * - dpdk_pci_init: 런타임 DPDK 버전 감지 후 fn_table을 결정.
 * - dpdk_pci_device_*: BAR/devargs/주소/ID/config space/interrupt를 DPDK 디바이스에서 읽고 쓴다.
 * - dpdk_bus_scan/probe: rte_bus 단의 스캔/프로브 트리거.
 */

#ifndef SPDK_PCI_DPDK_H
#define SPDK_PCI_DPDK_H

/* [한국어] SPDK 공개 env API. spdk_pci_id, spdk_pci_enum_cb 등 본 헤더의 타입에 필요. */
#include "spdk/env.h"

/*
 * [한국어]
 * struct spdk_pci_driver - SPDK 측 PCI 드라이버 핸들
 *
 * SPDK NVMe/virtio/ioat 등 각 모듈이 자신의 PCI 드라이버를 표현할 때 사용하는 구조체.
 * driver 멤버는 DPDK 버전마다 크기가 다르므로 driver_buf(고정 256바이트)에 in-place로
 * 만들어 ABI 의존을 차단한다.
 */
struct spdk_pci_driver {
	uint8_t				driver_buf[256];
	/* [한국어] DPDK rte_pci_driver의 실체를 담을 정적 버퍼.
	 * 설정자: dpdk_pci_driver_register가 이 버퍼 위에 placement-init 형태로 rte_pci_driver를 만든다.
	 * 읽는 자: DPDK PCI bus가 probe 시 driver 포인터를 통해 접근. 본 버퍼를 직접 읽어선 안 됨.
	 * 값 범위: 256 바이트. DPDK 버전 업그레이드에 따른 크기 증가에 대비한 여유분.
	 * 동기화: 등록은 init 단계 단일 스레드에서만 일어나므로 별도 락 불필요. */

	struct rte_pci_driver		*driver;
	/* [한국어] 위 driver_buf를 (struct rte_pci_driver *)로 캐스팅한 포인터.
	 * 설정자: dpdk_pci_driver_register.
	 * 읽는 자: DPDK PCI 등록 해제 시. SPDK 코드는 직접 dereference하지 않음.
	 * 값 범위: 본 구조체 driver_buf의 시작 주소.
	 * 동기화: 한 번 설정된 후 변경되지 않으므로 락 불필요. */

	const char                      *name;
	/* [한국어] 드라이버 이름 문자열(예: "spdk_nvme"). 디버그 로그 식별자로 사용.
	 * 설정자: 모듈 정적 초기화 시점.
	 * 읽는 자: rte_pci_driver.driver.name으로 복사되어 DPDK 로그에서 표시.
	 * 값 범위: 정적 문자열 리터럴. NULL 불가.
	 * 동기화: 읽기 전용. */

	const struct spdk_pci_id	*id_table;
	/* [한국어] 이 드라이버가 매칭할 vendor/device ID 테이블 (NULL terminated).
	 * 설정자: 정적 초기화 (예: nvme_pci_driver_id_table).
	 * 읽는 자: 디바이스 probe 시 rte_pci_match가 사용.
	 * 값 범위: 0/0 sentinel로 끝나는 spdk_pci_id 배열.
	 * 동기화: 읽기 전용 정적 데이터. */

	uint32_t			drv_flags;
	/* [한국어] 드라이버 플래그(SPDK_PCI_DRIVER_NEED_MAPPING, _WC_ACTIVATE 등).
	 * 설정자: 정적 초기화.
	 * 읽는 자: rte_pci_driver.drv_flags로 변환되어 DPDK PCI bus가 매핑 정책 결정에 사용.
	 * 값 범위: 비트 필드. 각 비트는 spdk/env.h에서 정의.
	 * 동기화: 읽기 전용. */

	spdk_pci_enum_cb		cb_fn;
	/* [한국어] 디바이스가 attach될 때 호출될 SPDK 측 콜백.
	 * 설정자: 모듈 등록 시 (예: spdk_pci_nvme_device_attach).
	 * 읽는 자: probe 이벤트 발생 시 호출.
	 * 값 범위: 유효한 함수 포인터 또는 NULL(구식 attach 방식).
	 * 동기화: 콜백 자체는 디바이스 probe 컨텍스트(보통 메인 스레드)에서 직렬 실행. */

	void				*cb_arg;
	/* [한국어] cb_fn 호출 시 첫 인자로 전달될 사용자 컨텍스트.
	 * 설정자: 모듈 등록 시.
	 * 읽는 자: cb_fn 본체.
	 * 값 범위: 임의 포인터(보통 모듈 내부 컨텍스트).
	 * 동기화: cb_fn과 동일 컨텍스트에서만 사용. */

	TAILQ_ENTRY(spdk_pci_driver)	tailq;
	/* [한국어] 등록된 모든 SPDK PCI 드라이버를 잇는 큐 링크.
	 * 설정자: dpdk_pci_driver_register가 g_pci_drivers TAILQ에 삽입.
	 * 읽는 자: 종료 시 정리 루프.
	 * 값 범위: TAILQ_HEAD/INSERT_TAIL이 관리.
	 * 동기화: 등록은 init 단계 단일 스레드. 런타임 변경 없음. */
};

/* [한국어] DPDK 측 핵심 자료구조의 전방 선언. 헤더 의존을 줄여 컴파일 캡슐화 강화. */
struct rte_pci_device;
struct rte_pci_driver;
struct rte_device;

/*
 * [한국어]
 * struct dpdk_fn_table - DPDK 버전 추상화를 위한 함수 포인터 테이블
 *
 * SPDK는 빌드 시점이 아닌 런타임에 사용 중인 DPDK 버전에 맞는 구현 .c (예: pci_dpdk_2211.c)
 * 를 자동 선택한다. 그 선택을 단일 디스패치 포인트로 만드는 것이 본 구조체이다.
 * 모든 SPDK PCI 호출은 g_dpdk_fn_table을 거쳐 실제 DPDK 함수에 도달한다.
 */
struct dpdk_fn_table {
	struct rte_mem_resource *(*pci_device_get_mem_resource)(struct rte_pci_device *dev, uint32_t bar);
	/* [한국어] PCI 디바이스의 BAR(Base Address Register, 0~5) 매핑 정보를 반환.
	 * 설정자: 버전별 구현 파일에서 dpdk_pci_init 시 채워짐.
	 * 읽는 자: SPDK NVMe 등 BAR mmap 후 MMIO doorbell·CC·CSTS 레지스터 접근.
	 * 값 범위: rte_mem_resource{ phys_addr, len, addr } 또는 NULL(BAR 미사용).
	 * 동기화: 디바이스 attach 후 정적이므로 read-only 사용. */

	const char *(*pci_device_get_name)(struct rte_pci_device *);
	/* [한국어] DPDK PCI 디바이스의 이름 문자열을 반환(BDF 형식 등). */

	struct rte_devargs *(*pci_device_get_devargs)(struct rte_pci_device *);
	/* [한국어] EAL 명령행에서 전달된 디바이스별 인자(예: --allow=0000:01:00.0,opt=val) 접근. */

	struct rte_pci_addr *(*pci_device_get_addr)(struct rte_pci_device *);
	/* [한국어] PCI 디바이스의 BDF 주소(domain:bus:device:function) 반환. */

	struct rte_pci_id *(*pci_device_get_id)(struct rte_pci_device *);
	/* [한국어] vendor/device/subvendor/subdevice/class ID 반환. */

	int (*pci_device_get_numa_node)(struct rte_pci_device *_dev);
	/* [한국어] 디바이스가 부착된 NUMA 노드 ID 반환. SPDK가 동일 NUMA에 큐쌍/버퍼를 배치하기 위함. */

	int (*pci_device_read_config)(struct rte_pci_device *dev, void *value, uint32_t len,
				      uint32_t offset);
	/* [한국어] PCI Configuration Space에서 len 바이트를 offset부터 읽어 value에 저장.
	 * 사용 예: NVMe Capabilities, MSI-X 테이블 위치 조회 등. */
	int (*pci_device_write_config)(struct rte_pci_device *dev, void *value, uint32_t len,
				       uint32_t offset);
	/* [한국어] PCI Configuration Space에 len 바이트를 offset에 기록.
	 * 사용 예: Bus Master 활성화, MSI-X enable 비트 설정. */

	int (*pci_driver_register)(struct spdk_pci_driver *driver,
				   int (*probe_fn)(struct rte_pci_driver *driver, struct rte_pci_device *device),
				   int (*remove_fn)(struct rte_pci_device *device));
	/* [한국어] SPDK 드라이버를 DPDK 측에 등록. probe_fn/remove_fn은 모두 SPDK가 정의하는
	 * 어댑터 콜백으로, DPDK 시그니처를 만족하면서 내부적으로 spdk_pci_driver.cb_fn을 호출한다. */

	int (*pci_device_enable_interrupt)(struct rte_pci_device *rte_dev);
	/* [한국어] 디바이스 인터럽트 활성화(VFIO eventfd 또는 UIO 인터럽트 fd 준비).
	 * SPDK는 polled-mode가 기본이지만, NVMe admin queue나 hot-plug 이벤트 등에서 사용. */
	int (*pci_device_disable_interrupt)(struct rte_pci_device *rte_dev);
	/* [한국어] 위와 반대. detach 시 호출. */
	int (*pci_device_get_interrupt_efd)(struct rte_pci_device *rte_dev);
	/* [한국어] 단일 인터럽트의 eventfd 파일디스크립터 반환. epoll/select에 등록 가능. */
	int (*pci_device_create_interrupt_efds)(struct rte_pci_device *rte_dev, uint32_t count);
	/* [한국어] MSI-X처럼 다중 벡터를 사용하는 디바이스에 대해 count개의 eventfd 생성. */
	void (*pci_device_delete_interrupt_efds)(struct rte_pci_device *rte_dev);
	/* [한국어] 위 create의 짝. detach 시 모든 eventfd close. */
	int (*pci_device_get_interrupt_efd_by_index)(struct rte_pci_device *rte_dev, uint32_t index);
	/* [한국어] 다중 벡터 eventfd 중 index번째 fd 반환. */
	int (*pci_device_interrupt_cap_multi)(struct rte_pci_device *rte_dev);
	/* [한국어] 디바이스가 다중 벡터 인터럽트를 지원하는지 검사(MSI-X capability 확인). */

	void (*bus_scan)(void);
	/* [한국어] PCI bus 스캔 트리거. /sys/bus/pci/devices를 읽어 rte_pci_device 객체를 만든다. */
	int (*bus_probe)(void);
	/* [한국어] 스캔된 디바이스에 대해 등록된 드라이버를 매칭하고 probe_fn 호출. */

	struct rte_devargs *(*device_get_devargs)(struct rte_device *dev);
	/* [한국어] 일반 rte_device 단의 devargs 접근. PCI 외 다른 bus(예: vdev)에도 적용. */
	void (*device_set_devargs)(struct rte_device *dev, struct rte_devargs *devargs);
	/* [한국어] devargs 교체. hotplug 시 동적으로 인자 전달이 필요한 경우 사용. */
	const char *(*device_get_name)(struct rte_device *dev);
	/* [한국어] rte_device의 이름 문자열. */
	bool (*device_scan_allowed)(struct rte_device *dev);
	/* [한국어] EAL 화이트리스트/블랙리스트에 따라 본 디바이스가 스캔 허용 대상인지 판정. */
};

/*
 * [한국어]
 * dpdk_pci_init - DPDK 버전을 감지하여 함수 포인터 테이블 활성화
 *
 * @return: 성공 시 0, 실패 시 음수 errno.
 *
 * 빌드된 DPDK의 RTE_VER_YEAR/MONTH 매크로를 보고 g_dpdk_fn_table을 적절한 버전별
 * 구현으로 설정한다. 이후 모든 dpdk_pci_* 래퍼는 이 테이블을 거쳐 동작한다.
 * 실행 컨텍스트: pci_env_init 내부에서 1회 호출.
 */
int dpdk_pci_init(void);

/*
 * [한국어]
 * dpdk_pci_device_* / dpdk_device_* / dpdk_bus_* (래퍼 함수들)
 *
 * 각 래퍼는 단순히 g_dpdk_fn_table.<해당 멤버>를 호출하는 1줄짜리 thunk이며,
 * SPDK 호출자가 dpdk_fn_table을 직접 참조하지 않도록 캡슐화한다.
 * 시그니처와 의미는 위 dpdk_fn_table 멤버 주석을 동일하게 따른다.
 */
struct rte_mem_resource *dpdk_pci_device_get_mem_resource(struct rte_pci_device *dev, uint32_t bar);
/* [한국어] vaddr 가상범위 [vaddr, vaddr+len)을 PCI BAR DMA 매핑에 대해 IOVA로 변환.
 * 일부 DPDK 버전은 rte_pci_device 내부에 vtophys를 직접 노출하지 않으므로 별도 래퍼로 노출. */
uint64_t dpdk_pci_device_vtophys(struct rte_pci_device *dev, uint64_t vaddr, size_t len);
const char *dpdk_pci_device_get_name(struct rte_pci_device *);
struct rte_devargs *dpdk_pci_device_get_devargs(struct rte_pci_device *);
struct rte_pci_addr *dpdk_pci_device_get_addr(struct rte_pci_device *);
struct rte_pci_id *dpdk_pci_device_get_id(struct rte_pci_device *);
int dpdk_pci_device_get_numa_node(struct rte_pci_device *_dev);
int dpdk_pci_device_read_config(struct rte_pci_device *dev, void *value, uint32_t len,
				uint32_t offset);
int dpdk_pci_device_write_config(struct rte_pci_device *dev, void *value, uint32_t len,
				 uint32_t offset);
int dpdk_pci_driver_register(struct spdk_pci_driver *driver,
			     int (*probe_fn)(struct rte_pci_driver *driver, struct rte_pci_device *device),
			     int (*remove_fn)(struct rte_pci_device *device));
int dpdk_pci_device_enable_interrupt(struct rte_pci_device *rte_dev);
int dpdk_pci_device_disable_interrupt(struct rte_pci_device *rte_dev);
int dpdk_pci_device_get_interrupt_efd(struct rte_pci_device *rte_dev);
int dpdk_pci_device_create_interrupt_efds(struct rte_pci_device *rte_dev, uint32_t count);
void dpdk_pci_device_delete_interrupt_efds(struct rte_pci_device *rte_dev);
int dpdk_pci_device_get_interrupt_efd_by_index(struct rte_pci_device *rte_dev, uint32_t index);
int dpdk_pci_device_interrupt_cap_multi(struct rte_pci_device *rte_dev);
void dpdk_bus_scan(void);
int dpdk_bus_probe(void);
struct rte_devargs *dpdk_device_get_devargs(struct rte_device *dev);
void dpdk_device_set_devargs(struct rte_device *dev, struct rte_devargs *devargs);
const char *dpdk_device_get_name(struct rte_device *dev);
bool dpdk_device_scan_allowed(struct rte_device *dev);

#endif /* ifndef SPDK_PCI_DPDK_H */
