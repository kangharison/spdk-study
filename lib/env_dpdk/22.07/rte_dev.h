/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2014 6WIND S.A.
 */

/*
 * [한국어 설명] DPDK 22.07 vendored 내부 디바이스 공통 인터페이스 헤더 (22.07/rte_dev.h)
 *
 * === 파일의 역할 ===
 * DPDK 22.07 버전의 rte_device / rte_driver / rte_mem_resource 등 범용 디바이스 추상화
 * 구조체와 hotplug, 디바이스 이벤트(add/remove), DMA map/unmap API를 선언한다.
 * SPDK는 이 헤더를 22.07 ABI 전용 vendored 사본으로 lib/env_dpdk/22.07/ 에 보관하여,
 * 시스템에 설치된 DPDK의 실제 버전에 관계없이 pci_dpdk_2207.c 컴파일 시 일관된
 * 구조체 레이아웃을 보장한다. 원본 DPDK와 달리 SPDK 프로젝트가 이 사본을 직접 관리한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pci_dpdk_2207.c 가 이 헤더를 직접 포함하여 rte_device 구조체 필드에 접근한다.
 * 호출 체인: pci_dpdk.c(디스패처) → fn_table_2207 → pci_dpdk_2207.c → 본 헤더 정의 사용.
 * 실행 컨텍스트: 컴파일 타임 전용. 런타임 오버헤드 없음.
 *
 * === 타 모듈과의 연결 ===
 * - 포함 관계: pci_dpdk_2207.c 가 이 헤더를 포함하여 22.07 ABI 의 rte_device 필드에 접근.
 * - 22.07/rte_bus.h, 22.07/rte_bus_pci.h 와 함께 22.07 vendored 헤더 세트를 구성.
 * - 22.11 버전과 비교: 22.11/rte_dev.h 는 일부 deprecated 매크로를 RTE_DEPRECATED 로 처리,
 *   rte_driver_name() 등 accessor 추가 등 소폭 확장되어 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct rte_mem_resource: PCI BAR 자원(phys_addr/len/addr). 가상·물리 주소 쌍.
 * - struct rte_driver: 드라이버 이름/alias 기술 구조체.
 * - struct rte_device: 범용 디바이스(이름/드라이버/버스/NUMA/devargs 포함).
 * - struct rte_dev_iterator: 디바이스 목록 순회 컨텍스트.
 * - rte_eal_hotplug_add/remove: 런타임 PCI 디바이스 추가/제거.
 * - rte_dev_dma_map/unmap: 디바이스 단위 DMA 매핑.
 * - RTE_DEV_FOREACH: 디바이스 문자열 기반 순회 매크로.
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

#include <stdio.h>

#include <rte_config.h>  /* [한국어] DPDK 빌드 시점 매크로. RTE_CACHE_LINE_SIZE 등. */
#include <rte_compat.h>  /* [한국어] __rte_experimental, __rte_internal 등 ABI 가시성 매크로. */
#include <rte_log.h>     /* [한국어] DPDK 로그 서브시스템. 일부 매크로가 로그 레벨에 의존. */

/*
 * [한국어]
 * enum rte_dev_event_type - DPDK 디바이스 이벤트 타입
 *
 * DPDK가 hotplug/remove 감지 시 rte_dev_event_callback_process로 전달하는 이벤트 분류.
 * SPDK의 pci_event.c 는 대신 netlink uevent를 직접 파싱하므로 본 enum을 런타임에 직접
 * 사용하지는 않지만, pci_dpdk_2207.c 의 콜백 시그니처에서 참조된다.
 */
enum rte_dev_event_type {
	RTE_DEV_EVENT_ADD,	/**< device being added */
	/* [한국어] 디바이스 추가 이벤트. hotplug_add 또는 PCI rescan 시 발생. */
	RTE_DEV_EVENT_REMOVE,	/**< device being removed */
	/* [한국어] 디바이스 제거 이벤트. surprise-removal 또는 hotplug_remove 시 발생. */
	RTE_DEV_EVENT_MAX	/**< max value of this enum */
	/* [한국어] 배열 인덱스 상한 sentinel. 실제 이벤트 타입으로는 사용 안 됨. */
};

/* [한국어] 디바이스 이벤트 콜백 함수 타입. device_name은 BDF 형식, event는 위 enum, cb_arg는 사용자 컨텍스트. */
typedef void (*rte_dev_event_cb_fn)(const char *device_name,
					enum rte_dev_event_type event,
					void *cb_arg);

/* Macros to check for invalid function pointers */
/* [한국어] NULL 함수 포인터 조기 검사 매크로 — 오류 경로에서 retval을 반환. */
#define RTE_FUNC_PTR_OR_ERR_RET(func, retval) do { \
	if ((func) == NULL) \
		return retval; \
} while (0)

/* [한국어] NULL 함수 포인터 조기 검사 매크로 — 오류 경로에서 void 반환. */
#define RTE_FUNC_PTR_OR_RET(func) do { \
	if ((func) == NULL) \
		return; \
} while (0)

/*
 * [한국어]
 * enum rte_dev_policy - EAL 화이트리스트/블랙리스트 정책
 *
 * EAL 커맨드라인의 --allow / --block 과 대응. SPDK init.c 가 build_eal_cmdline에서
 * 사용자가 지정한 디바이스를 ALLOWED/BLOCKED로 변환하여 rte_devargs에 삽입한다.
 */
enum rte_dev_policy {
	RTE_DEV_ALLOWED,	/* [한국어] 화이트리스트(probe 허용). --allow 인자로 지정된 디바이스. */
	RTE_DEV_BLOCKED,	/* [한국어] 블랙리스트(probe 금지). --block 인자로 지정된 디바이스. */
};

/*
 * [한국어]
 * struct rte_mem_resource - PCI BAR(Base Address Register) 자원 기술자
 *
 * DPDK가 PCI 디바이스의 BAR mmap 결과를 저장하는 기본 단위. pci_dpdk_2207.c의
 * pci_device_get_mem_resource_2207이 이 구조체의 포인터를 반환한다.
 */
struct rte_mem_resource {
	uint64_t phys_addr;
	/* [한국어] BAR의 호스트 물리 MMIO 주소. 0이면 자원 없음(BAR 비사용 또는 비매핑).
	 * 설정자: DPDK PCI 버스 scan 시 /sys/bus/pci/devices/.../resource 파일에서 읽음.
	 * 읽는 자: SPDK vtophys 등록 시 DMA 주소 힌트로 사용.
	 * 값 범위: 0 또는 유효한 물리 주소(일반적으로 1GB 이상 메모리 맵 I/O 공간).
	 * 동기화: attach 후 read-only. */
	uint64_t len;
	/* [한국어] BAR 크기(바이트). 예: NVMe BAR0은 보통 16KB 이상.
	 * 설정자: DPDK scan.  읽는 자: SPDK BAR 경계 검사, IOMMU map 크기 인자.
	 * 값 범위: 0(BAR 없음) 또는 page-aligned 양수.  동기화: read-only. */
	void *addr;
	/* [한국어] BAR이 mmap된 호스트 가상주소. NULL이면 아직 매핑 안 됨.
	 * 설정자: DPDK가 EAL init 시 mmap(또는 uio_pci_generic /dev/uioN).
	 * 읽는 자: SPDK NVMe 드라이버가 CC/CSTS/doorbell MMIO 레지스터 접근에 사용.
	 * 값 범위: NULL 또는 page-aligned 가상주소.  동기화: attach 후 read-only. */
};

/*
 * [한국어]
 * struct rte_driver - DPDK 드라이버 메타데이터
 *
 * 등록된 모든 DPDK 드라이버(PMD)가 전역 드라이버 리스트에 연결된다.
 * pci_dpdk_2207.c 의 pci_driver_register_2207은 rte_pci_driver 안의 driver 필드에
 * spdk_<name> 문자열을 채운다.
 */
struct rte_driver {
	RTE_TAILQ_ENTRY(rte_driver) next;
	/* [한국어] 전역 드라이버 TAILQ 링크. EAL이 드라이버 목록 순회 시 사용.
	 * 동기화: EAL init 단계에서만 삽입되므로 이후 read-only. */
	const char *name;
	/* [한국어] 드라이버 식별 이름(예: "spdk_nvme"). 로그 및 드라이버 lookup에 사용.
	 * 설정자: pci_driver_register_2207이 "spdk_<원래이름>" 동적 할당 문자열로 설정.
	 * 읽는 자: DPDK 디버그 출력, rte_driver_find_by_name. 동기화: 등록 후 read-only. */
	const char *alias;
	/* [한국어] 드라이버 별칭. SPDK는 보통 NULL로 설정. DPDK 커맨드라인에서 별칭으로도 매칭. */
};

/*
 * Internal identifier length
 * Sufficiently large to allow for UUID or PCI address
 */
/* [한국어] 디바이스 이름 최대 길이. BDF("0000:FF:1F.7")는 12자, UUID는 36자이므로 64로 충분. */
#define RTE_DEV_NAME_MAX_LEN 64

/*
 * [한국어]
 * struct rte_device - DPDK 범용 디바이스 공통 베이스 구조체
 *
 * PCI/AUXILIARY/VDEV 등 모든 디바이스 타입이 공유하는 최소 정보. rte_pci_device,
 * rte_vdev_device 등이 이를 첫 멤버로 임베드하여 container_of 패턴으로 접근한다.
 * pci_dpdk_2207.c 의 device_get_devargs / device_get_name / device_scan_allowed 함수가
 * 이 구조체 필드를 직접 접근한다(22.07에서는 opaque가 아님).
 */
struct rte_device {
	RTE_TAILQ_ENTRY(rte_device) next;
	/* [한국어] 버스별 디바이스 TAILQ 링크. rte_bus.devices 리스트 연결용.
	 * 동기화: scan 단계 이후 read-only. */
	const char *name;
	/* [한국어] 디바이스 이름 문자열(BDF 형식 "0000:01:00.0" 또는 vdev 이름).
	 * 설정자: DPDK scan 시 BDF 또는 devargs 이름 복사. 읽는 자: pci.c의 attach 매칭, 로그.
	 * 동기화: 등록 후 read-only. */
	const struct rte_driver *driver;
	/* [한국어] probe 성공 후 연결된 드라이버 포인터. probe 전에는 NULL.
	 * 설정자: DPDK bus probe 성공 시. 읽는 자: rte_dev_is_probed(비-NULL이면 true).
	 * 동기화: probe 후 read-only. */
	const struct rte_bus *bus;
	/* [한국어] 이 디바이스가 속한 버스(PCI/AUXILIARY/VDEV 등).
	 * 설정자: scan 시 버스가 자기 장치를 등록할 때. 읽는 자: pci_dpdk_2207의 bus 비교.
	 * 동기화: scan 후 read-only. */
	int numa_node;
	/* [한국어] 이 디바이스가 전기적으로 연결된 NUMA 노드 ID(-1이면 unknown).
	 * 설정자: DPDK가 /sys/bus/pci/devices/.../numa_node 파일에서 읽어 저장.
	 * 읽는 자: dpdk_pci_device_get_numa_node → rte_device 캐스팅 후 접근.
	 * 동기화: 초기화 후 read-only. */
	struct rte_devargs *devargs;
	/* [한국어] 이 디바이스에 대한 EAL 커맨드라인 인자(--allow=BDF,key=val 파싱 결과).
	 * 설정자: DPDK가 EAL init 시 devargs 파싱 후 저장. SPDK는 hotplug 시 교체 가능.
	 * 읽는 자: dpdk_device_get_devargs / set_devargs / scan_allowed.
	 * 동기화: pci.c의 g_pci_mutex로 보호(hotplug 교체 시). */
};

/**
 * Query status of a device.
 *
 * @param dev
 *   Generic device pointer.
 * @return
 *   (int)true if already probed successfully, 0 otherwise.
 */
/* [한국어] rte_dev_is_probed - 디바이스 probe 완료 여부 조회.
 * driver 포인터가 NULL이 아니면 probe 성공. 반환: non-0(완료), 0(미완). */
int rte_dev_is_probed(const struct rte_device *dev);

/* [한국어] rte_eal_hotplug_add - 지정 버스에 BDF 디바이스를 runtime add.
 * SPDK pci.c의 pci_attach_rte가 "pci" + BDF + "" 인자로 호출.
 * 다중 프로세스 환경에서는 IPC로 전파 — 실패 시 롤백. 반환: 0 성공, 음수 에러. */
int rte_eal_hotplug_add(const char *busname, const char *devname,
			const char *drvargs);

/* [한국어] rte_dev_probe - devargs 통합 문자열로 디바이스 probe 트리거.
 * rte_eal_hotplug_add보다 상위 레벨. SPDK는 직접 사용 안 함. */
int rte_dev_probe(const char *devargs);

/* [한국어] rte_eal_hotplug_remove - 지정 버스에서 BDF 디바이스 runtime remove.
 * SPDK pci.c의 remove_rte_dev가 호출. -ENOMSG 시 IPC 충돌(4회 재시도).
 * 반환: 0 성공, 음수 에러. */
int rte_eal_hotplug_remove(const char *busname, const char *devname);

/* [한국어] rte_dev_remove - rte_device 포인터 직접 기반 remove.
 * BDF 문자열 불필요. SPDK는 직접 사용 안 함(remove_rte_dev는 문자열 경로 사용). */
int rte_dev_remove(struct rte_device *dev);

/* [한국어] rte_dev_cmp_t - 디바이스 비교 콜백 타입.
 * 버스 find_device 구현에서 사용. 반환: 0이면 일치, 비-0이면 불일치/순서. */
typedef int (*rte_dev_cmp_t)(const struct rte_device *dev, const void *data);

/* [한국어] PMD 드라이버 이름 등록 보조 매크로 — .so 심볼 테이블에 드라이버 이름 삽입. */
#define RTE_PMD_EXPORT_NAME_ARRAY(n, idx) n##idx[]

/* [한국어] 드라이버 이름을 __attribute__((used)) 정적 변수로 심볼 테이블에 기록. */
#define RTE_PMD_EXPORT_NAME(name, idx) \
static const char RTE_PMD_EXPORT_NAME_ARRAY(this_pmd_name, idx) \
__rte_used = RTE_STR(name)

/* [한국어] 드라이버 확장 태그 이름 생성 헬퍼 — pci_tbl_export / param_string_export 등에 사용. */
#define DRV_EXP_TAG(name, tag) __##name##_##tag

/* [한국어] PCI ID 테이블 이름을 심볼에 기록. 동적 로딩 시 PMD가 지원하는 디바이스 목록 광고. */
#define RTE_PMD_REGISTER_PCI_TABLE(name, table) \
static const char DRV_EXP_TAG(name, pci_tbl_export)[] __rte_used = \
RTE_STR(table)

/* [한국어] 드라이버 파라미터 문자열을 심볼에 기록. devargs 파싱 시 허용 키 목록 광고. */
#define RTE_PMD_REGISTER_PARAM_STRING(name, str) \
static const char DRV_EXP_TAG(name, param_string_export)[] \
__rte_used = str

/*
 * [한국어]
 * struct rte_dev_iterator - 디바이스 목록 순회 컨텍스트
 *
 * RTE_DEV_FOREACH 매크로와 rte_dev_iterator_init/next를 통해 버스/클래스 조건에
 * 맞는 디바이스를 순차 방문할 때 사용. SPDK pci.c는 이보다 직접적인 TAILQ 순회를
 * 사용하지만, 일부 DPDK API가 내부적으로 이 구조체를 참조한다.
 */

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
 * dpdk-devbind.py 같은 사용자 도구가 이 심볼을 읽어 자동으로 필요한 커널 모듈(vfio-pci,
 * uio_pci_generic 등)을 로드할 수 있도록 한다. SPDK 환경에서는 vfio-pci 또는 uio_pci_generic이
 * 필요하며, 이 매크로로 등록된 의존성이 올바르지 않으면 디바이스 바인딩에 실패한다. */
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
 * struct rte_dev_iterator - 디바이스 목록 순회 컨텍스트
 *
 * RTE_DEV_FOREACH 매크로 및 rte_dev_iterator_init / rte_dev_iterator_next API를 통해
 * 버스·클래스 조건에 맞는 rte_device 목록을 순차 방문할 때 사용하는 상태 구조체.
 * SPDK pci.c는 PCI 버스 직접 TAILQ 순회를 주로 사용하지만, DPDK 내부 API
 * (rte_eal_hotplug_add/remove 등)가 이 구조체를 간접적으로 참조한다.
 * 힙 할당 없이 스택 변수로 사용 가능하며, 순회 도중에도 안전하게 포기할 수 있다.
 */
struct rte_dev_iterator {
	const char *dev_str;
	/* [한국어] 전체 디바이스 서술 문자열 (예: "pci:0000:04:00.0,vfio").
	 * 설정자: rte_dev_iterator_init()이 호출자 제공 str을 그대로 저장.
	 * 읽는 자: rte_dev_iterator_next()가 다음 디바이스를 찾는 데 사용.
	 * 값 범위: NULL 불가; devargs 형식 "bus:name[,key=val...]"을 따른다.
	 * 동기화: 단일 스레드 전용 — 복수 스레드가 같은 이터레이터를 공유하면 안 된다. */

	const char *bus_str;
	/* [한국어] dev_str 중 버스 관련 부분만 추출한 포인터 (예: "pci:0000:04:00.0").
	 * 설정자: rte_dev_iterator_init()이 dev_str을 파싱하여 설정.
	 * 읽는 자: 버스 레이어의 find_device 콜백이 이 값으로 디바이스를 식별.
	 * 값 범위: 버스 접두사가 없으면 NULL 가능.
	 * 동기화: 부모 dev_str 수명에 종속 — dev_str이 해제되면 무효. */

	const char *cls_str;
	/* [한국어] dev_str 중 클래스 관련 부분만 추출한 포인터 (예: "eth_dev").
	 * 설정자: rte_dev_iterator_init()이 클래스 접두사를 파싱하여 설정.
	 * 읽는 자: 클래스 레이어의 iterate 콜백이 이 값으로 클래스 디바이스를 필터링.
	 * 값 범위: 클래스가 없는 순수 PCI 디바이스에서는 NULL.
	 * 동기화: bus_str과 동일한 수명 규칙 적용. */

	struct rte_bus *bus;
	/* [한국어] 현재 순회 중인 버스 핸들 (예: PCI 버스 등록 객체).
	 * 설정자: rte_dev_iterator_init()이 bus_str로 버스를 조회하여 설정.
	 * 읽는 자: rte_dev_iterator_next()가 해당 버스의 find_device를 호출.
	 * 값 범위: 유효한 rte_bus 포인터; 매칭 버스 없으면 NULL (순회 불가).
	 * 동기화: DPDK EAL 버스 레지스트리는 초기화 이후 변경되지 않으므로 락 불필요. */

	struct rte_class *cls;
	/* [한국어] 현재 순회 중인 클래스 핸들 (예: eth, vdev 등).
	 * 설정자: rte_dev_iterator_init()이 cls_str로 클래스 레지스트리를 조회하여 설정.
	 * 읽는 자: rte_dev_iterator_next()가 클래스 레이어 iterate를 통해 클래스 디바이스를 방문.
	 * 값 범위: 클래스 없는 순수 버스 순회 시 NULL.
	 * 동기화: 클래스 레지스트리도 초기화 이후 불변. */

	struct rte_device *device;
	/* [한국어] 현재 순회 위치 — 직전 next() 호출이 반환한 rte_device 포인터.
	 * 설정자: rte_dev_iterator_next()가 다음 디바이스를 찾은 후 갱신.
	 * 읽는 자: RTE_DEV_FOREACH 루프 변수, 또는 next() 시작 컨텍스트로 재사용.
	 * 값 범위: 유효한 rte_device 포인터; 순회 종료 시 NULL.
	 * 동기화: 단일 스레드 사용 전제. 다른 스레드가 동시에 hotplug로 디바이스를 제거하면
	 *          이 포인터가 dangling pointer가 될 수 있어 주의 필요. */

	void *class_device;
	/* [한국어] 클래스 레이어가 추가 컨텍스트로 사용하는 불투명 포인터 (예: 클래스 디바이스 핸들).
	 * 설정자: 클래스 iterate 콜백이 내부 상태 추적을 위해 직접 설정.
	 * 읽는 자: 같은 클래스 iterate 콜백의 다음 호출에서 start 파라미터로 사용.
	 * 값 범위: 클래스 레이어 구현마다 다름. 클래스 없는 순회에서는 NULL.
	 * 동기화: class_device의 수명은 해당 클래스 레이어 내부 규칙을 따름. */
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
/* [한국어] rte_dev_iterate_t - 버스/클래스 레이어가 구현하는 디바이스 순회 콜백 함수 타입.
 * start: 이전 호출 반환값을 재사용하는 시작 컨텍스트 (첫 호출 시 NULL).
 * devstr: 디바이스 조건 문자열.
 * it: 현재 이터레이터 컨텍스트 (수정 금지).
 * 반환: 다음 매칭 디바이스 포인터; 더 없으면 NULL. */
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
/* [한국어] rte_dev_iterator_init - 디바이스 이터레이터를 초기화.
 * it: 초기화할 이터레이터 핸들 (스택 변수 가능).
 * str: "bus:name[,key=val...]" 형식의 디바이스 서술 문자열.
 * 반환: 0 성공, 음수 실패.
 * 동작: 힙 메모리를 할당하지 않으므로 언제든 순회를 포기해도 안전.
 * 호출 체인: 호출자(예: RTE_DEV_FOREACH 매크로) → [rte_dev_iterator_init] → 버스/클래스 레지스트리 조회. */
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
/* [한국어] rte_dev_iterator_next - 이터레이터에서 다음 디바이스를 반환.
 * it: 이전 init 또는 next로 갱신된 이터레이터 핸들.
 * 반환: 다음 rte_device 포인터; 에러 시 NULL+rte_errno 설정; 순회 끝이면 NULL+rte_errno 0.
 * 동작: it->device를 다음 매칭 디바이스로 갱신하고 반환. 버스/클래스 레이어 iterate 콜백 호출.
 * 호출 체인: 호출자(RTE_DEV_FOREACH 또는 직접 호출) → [rte_dev_iterator_next]
 *              → 버스 find_device → 클래스 iterate. */
__rte_experimental
struct rte_device *
rte_dev_iterator_next(struct rte_dev_iterator *it);

/* [한국어] RTE_DEV_FOREACH - 디바이스 서술 문자열 devstr에 매칭하는 모든 rte_device를 순회하는 for 루프 매크로.
 * dev: 루프 변수 (rte_device *); devstr: 디바이스 조건 문자열; it: rte_dev_iterator 포인터.
 * 내부 동작: 루프 init에서 rte_dev_iterator_init 호출, 각 반복에서 rte_dev_iterator_next 호출.
 * SPDK pci.c는 이 매크로보다 버스 직접 TAILQ를 선호하나, DPDK 일부 유틸이 내부적으로 사용. */
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
/* [한국어] rte_dev_event_callback_register - 특정 디바이스의 hotplug 이벤트 콜백 등록.
 * device_name: 콜백 대상 디바이스 이름; NULL이면 모든 디바이스에 적용.
 * cb_fn: 이벤트 발생 시 호출될 rte_dev_event_cb_fn 콜백.
 * cb_arg: 콜백에 전달될 사용자 정의 인자.
 * 반환: 0 성공, 음수 실패.
 * SPDK에서: pci_event.c의 NETLINK 기반 이벤트 처리와 별개 경로이나, 내부 DPDK 이벤트 전파에 사용. */
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
 * device_name: 대상 디바이스 이름; NULL이면 모든 디바이스.
 * cb_fn: 해제할 콜백 함수 포인터.
 * cb_arg: (void *)-1이면 같은 cb_fn을 가진 모든 콜백 일괄 제거; 특정 값이면 해당 콜백만 제거.
 * 반환: 제거된 콜백 엔트리 수(성공 시 >= 0), 음수 에러.
 * 동기화: DPDK 내부 이벤트 목록 락을 통해 스레드 안전 보장. */
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
/* [한국어] rte_dev_event_callback_process - 지정 디바이스에 등록된 모든 콜백을 즉시 실행.
 * device_name: 이벤트 대상 디바이스 이름.
 * event: 발생한 이벤트 유형 (RTE_DEV_EVENT_ADD / _REMOVE).
 * 반환값 없음. 내부적으로 등록된 콜백 목록을 순회하며 cb_fn(device_name, event, cb_arg)를 호출.
 * SPDK에서: pci_event.c 의 NETLINK 이벤트 루프가 udev 이벤트를 감지한 후 DPDK 레이어에 전파할 때 간접 사용. */
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
/* [한국어] rte_dev_event_monitor_start - DPDK 내부 디바이스 이벤트 모니터링 스레드 시작.
 * 반환: 0 성공, 음수 실패.
 * 동작: DPDK EAL이 관리하는 이벤트 폴링 스레드를 활성화. SPDK는 자체 NETLINK 소켓(pci_event.c)을
 * 사용하므로 이 함수를 직접 호출하지 않을 수 있으나, DPDK init 경로에서 내부적으로 호출될 수 있음. */
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
/* [한국어] rte_dev_event_monitor_stop - DPDK 내부 디바이스 이벤트 모니터링 스레드 중지.
 * 반환: 0 성공, 음수 실패.
 * 동작: rte_dev_event_monitor_start로 시작된 모니터링 스레드를 종료.
 * SPDK 종료 시 spdk_env_fini → rte_eal_cleanup 경로에서 내부적으로 호출될 수 있음. */
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
/* [한국어] rte_dev_hotplug_handle_enable - DPDK 레이어 hotplug 처리 활성화.
 * 반환: 0 성공, 음수 실패.
 * 동작: 이벤트 모니터링 스레드가 감지한 이벤트를 실제로 처리(콜백 호출)하도록 허용.
 * 비활성 상태에서는 이벤트가 큐에 쌓이지만 처리는 되지 않는다.
 * SPDK init.c의 spdk_env_dpdk_post_init에서 필요 시 호출. */
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
/* [한국어] rte_dev_hotplug_handle_disable - DPDK 레이어 hotplug 처리 비활성화.
 * 반환: 0 성공, 음수 실패.
 * 동작: 이벤트 모니터링은 계속하되, 실제 hotplug 처리(probe/remove 콜백)는 중단.
 * 종료 시퀀스나 크리티컬 구간에서 hotplug 이벤트가 드라이버 구조체를 건드리지 못하도록 보호. */
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
/* [한국어] rte_dev_dma_map - 특정 디바이스에 메모리 영역을 DMA 매핑.
 * dev: 매핑 대상 rte_device (VFIO 컨테이너와 연결된 PCI 디바이스).
 * addr: 매핑할 가상 주소 (rte_extmem_* API로 등록된 영역이어야 함).
 * iova: 디바이스가 사용할 I/O 가상 주소 (NVMe PRP/SGL에 쓰이는 DMA 주소).
 * len: 매핑 길이.
 * 반환: 0 성공, 음수+rte_errno 실패.
 * 동작: VFIO 컨테이너에 VFIO_IOMMU_MAP_DMA ioctl을 수행. memory.c의 vtophys 매핑과 일관성 유지 필요.
 * 호출 체인: SPDK memory.c spdk_mem_register → rte_dev_dma_map (VFIO 모드에서). */
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
/* [한국어] rte_dev_dma_unmap - 디바이스에서 DMA 매핑 해제.
 * dev: 언매핑 대상 rte_device.
 * addr: 해제할 가상 주소.
 * iova: 해제할 IOVA 주소.
 * len: 해제 길이.
 * 반환: 0 성공, 음수+rte_errno 실패.
 * 동작: VFIO_IOMMU_UNMAP_DMA ioctl 수행. 매핑 해제 후 디바이스는 해당 주소에 DMA 불가.
 * NVMe 드라이버가 해당 메모리를 사용하는 I/O가 완료된 후에만 호출해야 함.
 * 호출 체인: SPDK memory.c spdk_mem_unregister → rte_dev_dma_unmap (VFIO 모드에서). */
__rte_experimental
int
rte_dev_dma_unmap(struct rte_device *dev, void *addr, uint64_t iova,
		  size_t len);

#ifdef __cplusplus
}
#endif

#endif /* _RTE_DEV_H_ */
