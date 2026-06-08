/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK PCI 디바이스 관리 핵심 (pci.c)
 *
 * === 파일의 역할 ===
 * SPDK의 PCI 디바이스 attach/detach/enumerate/claim/BAR mmap·config space 접근 등 PCI 관련
 * 모든 외부 노출 API(spdk_pci_*)의 구현체이다. DPDK rte_pci 서브시스템 위에 SPDK 자체
 * 디바이스 객체(spdk_pci_device)와 드라이버 객체(spdk_pci_driver)를 두어 라이프사이클을
 * 관리하고, hotplug(udev/uevent) 이벤트와 디바이스 surprise-removal을 처리한다. 또한 동일
 * 디바이스에 대한 다중 프로세스 동시 접근을 막기 위해 /var/tmp 에 lock 파일을 만드는
 * claim 메커니즘과, PCI Configuration Space 표준 영역(VPD/SN/Capabilities 등)에 대한
 * 헬퍼들을 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/env_dpdk의 PCI 어댑터. 호출 체인:
 *   lib/nvme(nvme_pcie_ctrlr_construct) → spdk_pci_device_attach → pci_attach_rte →
 *   rte_eal_hotplug_add → DPDK PCI bus probe → pci_device_init(본 파일) →
 *   driver->cb_fn(상위 모듈 콜백) → vtophys_pci_device_added.
 * 실행 컨텍스트: 대부분 메인 스레드(또는 attach 호출 스레드). hotremove 알람은 DPDK 내부
 * interrupt 스레드에서 발생하므로 g_pci_mutex로 동기화.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: env_internal.h(vtophys_pci_device_added/removed), pci_dpdk.h(dpdk_pci_* 어댑터),
 *         DPDK rte_alarm/rte_devargs/rte_pci, spdk/env.h(공개 spdk_pci_* 시그니처).
 * - 본 파일에 의존: lib/nvme(NVMe attach), lib/vmd, module/bdev/virtio 등 PCI를 다루는 모든 모듈.
 * - 공유 자료구조: g_pci_devices(현재 attach된 모든 디바이스), g_pci_hotplugged_devices(probe 도중),
 *                  g_pci_drivers(등록된 모든 SPDK PCI 드라이버), g_pci_device_providers(추상 attach
 *                  제공자 — 예: "pci", "vmd").
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_pci_driver_register: SPDK 드라이버를 등록 큐에 추가(SPDK_PCI_DRIVER_REGISTER 매크로의 본체).
 * - pci_env_init/_reinit/_fini: DPDK PCI 백엔드와 SPDK 드라이버를 결합.
 * - pci_device_init/_fini: DPDK가 디바이스 발견/제거 시 호출하는 probe/remove 어댑터.
 * - spdk_pci_device_attach: 외부 노출 attach 진입점. driver+enum_cb+BDF로 1개 디바이스 attach.
 * - spdk_pci_enumerate: 등록된 드라이버로 매칭되는 모든 디바이스 enumerate.
 * - spdk_pci_device_map_bar/_unmap_bar: BAR mmap.
 * - spdk_pci_device_cfg_read/_write*: PCI config space 표준/확장 영역 접근.
 * - spdk_pci_device_claim/_unclaim: 다중 프로세스 동시 attach 차단(flock 기반).
 * - struct env_devargs: rte_devargs에 SPDK 별도 timestamp(allowed_at) 부가.
 */

/* [한국어] env_dpdk 내부 선언(vtophys_pci_device_added/removed 등). */
#include "env_internal.h"
/* [한국어] DPDK 버전별 PCI 어댑터 함수 포인터 테이블. dpdk_pci_* 래퍼 호출에 필요. */
#include "pci_dpdk.h"

/* [한국어] rte_eal_alarm_set/cancel — 디바이스 detach 시 1ms 지연 alarm으로 안전 해제. */
#include <rte_alarm.h>
/* [한국어] rte_devargs_parse/insert — PCI 디바이스 인자(BDF + 옵션 문자열) 관리. */
#include <rte_devargs.h>
/* [한국어] rte_pci_device 등 PCI 핵심 타입. */
#include <rte_pci.h>
/* [한국어] SPDK 공개 env API(spdk_pci_* 시그니처). */
#include "spdk/env.h"
/* [한국어] SPDK_ERRLOG/NOTICELOG. */
#include "spdk/log.h"
/* [한국어] spdk_strerror/spdk_strcpy_pad. */
#include "spdk/string.h"
/* [한국어] SPDK 메모리 헬퍼(SHIFT_2MB 등). */
#include "spdk/memory.h"

/* [한국어] Linux sysfs에서 PCI 드라이버 디렉토리. uio/vfio-pci 등의 바인딩 확인에 사용. */
#define SYSFS_PCI_DRIVERS	"/sys/bus/pci/drivers"

/* [한국어] PCI 표준 Configuration Space 크기(256 바이트). PCIe Extended Cap은 0x100~0xFFF 별도. */
#define PCI_CFG_SIZE		256
/* [한국어] PCIe Extended Capability ID 중 Device Serial Number (SN). 0x03 = SN. */
#define PCI_EXT_CAP_ID_SN	0x03

/* DPDK 18.11+ hotplug isn't robust. Multiple apps starting at the same time
 * might cause the internal IPC to misbehave. Just retry in such case.
 */
/* [한국어] DPDK 18.11+에서 hotplug IPC가 동시 다중 프로세스 시 불안정하여
 * -ENOMSG가 산발적으로 반환되는 회피책. 최대 4회 재시도한다. */
#define DPDK_HOTPLUG_RETRY_COUNT 4

/* DPDK alarm/interrupt thread */
/* [한국어] 모든 g_pci_* 큐를 보호하는 글로벌 뮤텍스. DPDK alarm/interrupt 스레드와
 * 사용자 스레드가 동시에 큐를 건드릴 수 있으므로 반드시 필요. */
static pthread_mutex_t g_pci_mutex = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] 현재 SPDK가 추적 중인 모든 attach된 PCI 디바이스 리스트.
 * 설정자: pci_device_init 후 cleanup_pci_devices에서 hotplugged → g_pci_devices로 이동.
 * 읽는 자: spdk_pci_device_attach/enumerate, spdk_pci_for_each_device.
 * 동기화: g_pci_mutex. */
static TAILQ_HEAD(, spdk_pci_device) g_pci_devices = TAILQ_HEAD_INITIALIZER(g_pci_devices);
/* devices hotplugged on a dpdk thread */
/* [한국어] DPDK 스레드 컨텍스트에서 발견되어 아직 SPDK 메인 큐로 이관 전인 임시 큐.
 * 메인 스레드에서 cleanup_pci_devices()가 안전하게 옮긴다(스레드 안전성 보장). */
static TAILQ_HEAD(, spdk_pci_device) g_pci_hotplugged_devices =
	TAILQ_HEAD_INITIALIZER(g_pci_hotplugged_devices);
/* [한국어] 등록된 모든 SPDK PCI 드라이버. SPDK_PCI_DRIVER_REGISTER constructor가 채운다. */
static TAILQ_HEAD(, spdk_pci_driver) g_pci_drivers = TAILQ_HEAD_INITIALIZER(g_pci_drivers);
/* [한국어] PCI 디바이스 attach 제공자(provider). 기본 "pci"(rte) 외에 "vmd" 같은 가상 버스도 등록 가능. */
static TAILQ_HEAD(, spdk_pci_device_provider) g_pci_device_providers =
	TAILQ_HEAD_INITIALIZER(g_pci_device_providers);

/* [한국어] DPDK probe/remove 콜백 시그니처 — 본 파일이 정의하고 pci_dpdk가 등록. */
int pci_device_init(struct rte_pci_driver *driver, struct rte_pci_device *device);
int pci_device_fini(struct rte_pci_device *device);

/*
 * [한국어]
 * struct env_devargs - rte_devargs에 SPDK 측 추가 메타데이터 부가
 *
 * SPDK는 hotplug된 디바이스가 너무 빨리 add/remove 사이클을 도는 것을 방지하기 위해
 * 디바이스별 "최초 허용 시각(allowed_at)"을 따로 기록한다. 본 구조체는 그 매핑 테이블의 원소.
 */
struct env_devargs {
	struct rte_bus	*bus;
	/* [한국어] 디바이스가 속한 DPDK bus 포인터.
	 * 설정자: set_allowed_at(rte_da->bus 복사). 읽는 자: find_env_devargs 비교.
	 * 값 범위: rte_bus_get(pci) 등에서 받은 유효 포인터. 동기화: 단일 init 스레드에서만 수정. */
	char		name[128];
	/* [한국어] 디바이스 이름 문자열(BDF 형식, 예: "0000:01:00.0"). 128 바이트로 고정.
	 * 설정자: spdk_strcpy_pad로 패딩 후 저장. 읽는 자: find_env_devargs(strcmp).
	 * 값 범위: ASCII 문자열. 동기화: 동일. */
	uint64_t	allowed_at;
	/* [한국어] 이 디바이스를 attach 허용해도 좋다고 SPDK가 인정한 시각(TSC tick).
	 * 0 = 아직 한 번도 본 적 없음. 비-0 + delay_init이면 그 시각 이전까지 BLOCKED 유지.
	 * 설정자: set_allowed_at. 읽는 자: scan_pci_bus 정책 결정. */
	TAILQ_ENTRY(env_devargs) link;
	/* [한국어] g_env_devargs 리스트 링크. */
};
/* [한국어] env_devargs 전역 리스트. scan_pci_bus 마다 갱신된다. */
static TAILQ_HEAD(, env_devargs) g_env_devargs = TAILQ_HEAD_INITIALIZER(g_env_devargs);

/*
 * [한국어]
 * find_env_devargs - bus+name으로 env_devargs 노드 검색
 *
 * @bus:  대상 디바이스의 DPDK bus 포인터.
 * @name: BDF 등 디바이스 이름.
 * @return: 매칭 노드 포인터, 없으면 NULL.
 *
 * 선형 검색. 디바이스 수가 보통 수~수십 개이므로 충분히 빠름.
 * 실행 컨텍스트: 메인 스레드(g_env_devargs는 외부 락 없이 사용 — scan_pci_bus 직렬화 가정).
 */
static struct env_devargs *
find_env_devargs(struct rte_bus *bus, const char *name)
{
	struct env_devargs *da;
	/* [한국어] 순회용 임시. */

	TAILQ_FOREACH(da, &g_env_devargs, link) {
		/* [한국어] bus 포인터 동일성 + 이름 문자열 동일성 검사. */
		if (bus == da->bus && !strcmp(name, da->name)) {
			return da;
		}
	}

	return NULL;
	/* [한국어] 매칭 없음. */
}

/*
 * [한국어]
 * map_bar_rte - DPDK 디바이스의 BAR(Base Address Register) 매핑 정보 추출
 *
 * @device:      SPDK PCI 디바이스 객체.
 * @bar:         BAR 인덱스(0~5).
 * @mapped_addr: 출력. BAR이 호스트 가상주소로 mmap된 위치.
 * @phys_addr:   출력. BAR의 PCI 물리주소(호스트 입장의 MMIO 주소).
 * @size:        출력. BAR 크기(바이트).
 * @return:      항상 0.
 *
 * DPDK가 EAL init 단계에서 이미 BAR을 mmap해 두었으므로 본 함수는 그 결과(rte_mem_resource)
 * 만 읽어서 반환한다. SPDK NVMe 드라이버는 이 mapped_addr를 통해 CC/CSTS/doorbell 등에 접근.
 *
 * 호출 체인: spdk_pci_device_map_bar → device->map_bar(=map_bar_rte) → dpdk_pci_device_get_mem_resource.
 */
static int
map_bar_rte(struct spdk_pci_device *device, uint32_t bar,
	    void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	struct rte_mem_resource *res;
	/* [한국어] DPDK가 채워둔 BAR 정보. */

	res = dpdk_pci_device_get_mem_resource(device->dev_handle, bar);
	/* [한국어] 어댑터를 통해 DPDK rte_pci_device의 mem_resource 배열 [bar] 조회. */
	*mapped_addr = res->addr;
	/* [한국어] BAR의 사용자 공간 가상주소 — NVMe 레지스터 접근에 직접 사용. */
	*phys_addr = (uint64_t)res->phys_addr;
	/* [한국어] BAR의 호스트 물리 MMIO 주소(IOVA 계산에 사용될 수 있음). */
	*size = (uint64_t)res->len;
	/* [한국어] BAR 크기. */

	return 0;
}

/*
 * [한국어]
 * unmap_bar_rte - BAR unmap (no-op)
 *
 * DPDK가 BAR mmap을 관리하므로 SPDK 측에서는 명시적 unmap이 필요 없다. detach 시 DPDK가 자동 처리.
 */
static int
unmap_bar_rte(struct spdk_pci_device *device, uint32_t bar, void *addr)
{
	return 0;
	/* [한국어] DPDK 위임 — SPDK는 아무 작업 안 함. */
}

/*
 * [한국어]
 * cfg_read_rte - PCI Configuration Space 읽기 어댑터
 *
 * @dev:   SPDK PCI 디바이스.
 * @value: 출력 버퍼.
 * @len:   읽을 바이트 수(보통 1/2/4).
 * @offset: config space 내 오프셋(0~255 표준, 0x100~ 확장).
 * @return: 성공 시 0, DPDK 오류는 음수.
 *
 * 내부적으로 VFIO ioctl(VFIO_DEVICE_GET_REGION_INFO + pread) 또는 uio_pci_generic의
 * /sys/bus/pci/devices/.../config 파일 접근을 사용한다(DPDK가 선택).
 */
static int
cfg_read_rte(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	/* [한국어] DPDK 어댑터로 위임. */
	return dpdk_pci_device_read_config(dev->dev_handle, value, len, offset);
}

/*
 * [한국어]
 * cfg_write_rte - PCI Configuration Space 쓰기 어댑터
 *
 * 시그니처/내부는 cfg_read_rte와 동일. NVMe Bus Master 활성화, MSI-X enable 비트 설정 등에 사용.
 */
static int
cfg_write_rte(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	/* [한국어] DPDK 어댑터로 위임. */
	return dpdk_pci_device_write_config(dev->dev_handle, value, len, offset);
}

/*
 * [한국어]
 * remove_rte_dev - DPDK PCI 디바이스 hotremove (BDF 기반)
 *
 * @rte_dev: 제거할 디바이스 핸들.
 *
 * BDF 문자열로 rte_eal_hotplug_remove("pci", BDF) 호출. -ENOMSG가 나오면 DPDK IPC 충돌로 보고
 * 최대 DPDK_HOTPLUG_RETRY_COUNT번 재시도. 실행 컨텍스트: rte_eal_alarm 스레드 또는 호출자.
 */
static void
remove_rte_dev(struct rte_pci_device *rte_dev)
{
	char bdf[32];
	/* [한국어] DPDK에 넘길 BDF 문자열("domain:bus:dev.func") 버퍼. */
	int i = 0, rc;
	/* [한국어] i = 재시도 카운터, rc = rte_eal_hotplug_remove 반환값. */

	snprintf(bdf, sizeof(bdf), "%s", dpdk_pci_device_get_name(rte_dev));
	/* [한국어] rte_pci_device의 이름(BDF)을 문자열로 추출 — hotplug_remove는 문자열 인자만 받음. */
	do {
		/* [한국어] "pci" 버스에서 해당 BDF 디바이스를 분리. DPDK가 드라이버 unbind + 자원 해제. */
		rc = rte_eal_hotplug_remove("pci", bdf);
	} while (rc == -ENOMSG && ++i <= DPDK_HOTPLUG_RETRY_COUNT);
	/* [한국어] -ENOMSG = 다중 프로세스 IPC 충돌. 최대 DPDK_HOTPLUG_RETRY_COUNT(4)회 재시도. */
}

/*
 * [한국어]
 * detach_rte_cb - rte_eal_alarm 콜백 래퍼: 1ms 지연 후 remove_rte_dev 실행
 *
 * @_dev: rte_pci_device* (void*로 전달). detach 대상 디바이스 핸들.
 *
 * detach_rte()/pci_device_rte_dev_event()가 rte_eal_alarm_set(1, detach_rte_cb, dev)로 등록한다.
 * DPDK는 hotremove 통지를 interrupt 콜백 안에서 실행하므로, 그 안에서 직접
 * rte_eal_hotplug_remove를 부르면 콜백 unregister가 막힌다. 따라서 1ms 지연 alarm으로
 * 콜백을 빠져나간 뒤 별도 컨텍스트에서 제거하는 우회책이다.
 * 실행 컨텍스트: DPDK rte_eal_alarm(interrupt) 스레드.
 *
 * 호출 체인: rte_eal_alarm_set → [detach_rte_cb] → remove_rte_dev → rte_eal_hotplug_remove
 */
static void
detach_rte_cb(void *_dev)
{
	/* [한국어] void* → rte_pci_device*로 해석하여 실제 제거 수행. */
	remove_rte_dev(_dev);
}

/* if it's a physical device we need to deal with DPDK on
 * a different process and we can't just unset one flag
 * here. We also want to stop using any device resources
 * so that the device isn't "in use" by the userspace driver
 * once we detach it. This would allow attaching the device
 * to a different process, or to a kernel driver like nvme.
 */
/*
 * [한국어]
 * detach_rte - 물리 PCI 디바이스를 DPDK에서 분리하는 "pci" provider의 detach_cb
 *
 * @dev: 분리할 SPDK PCI 디바이스 객체.
 *
 * 물리 디바이스는 다른 프로세스의 DPDK와도 자원을 공유하므로, 단순히 플래그만 끄는
 * 것으로는 부족하다. 유저스페이스 드라이버가 디바이스를 "사용 중"으로 잡고 있지 않도록
 * 자원을 모두 놓아야 다른 프로세스나 커널 nvme 드라이버가 다시 attach할 수 있다.
 * 동작 순서:
 *   1) primary 프로세스가 아니면 즉시 반환(자원 소유권은 primary에만 있음).
 *   2) attached=false + pending_removal=true 로 마킹(이후 hotremove 통지가 중복 제거 못 하게).
 *   3) 1ms alarm으로 detach_rte_cb 예약(interrupt 콜백 재진입 회피).
 *   4) 최대 2초간 1ms 간격으로 removed 플래그를 폴링하며 DPDK 제거 완료를 대기.
 *   5) alarm cancel(이미 실행 중이면 끝날 때까지 블록) → 최종 removed 재확인.
 * 실행 컨텍스트: detach 호출 스레드(메인). g_pci_mutex로 alarm 스레드와 동기화.
 *
 * 호출 체인: spdk_pci_device_detach → provider->detach_cb(=detach_rte) → rte_eal_alarm_set/cancel
 */
static void
detach_rte(struct spdk_pci_device *dev)
{
	struct rte_pci_device *rte_dev = dev->dev_handle;
	/* [한국어] DPDK 측 디바이스 핸들 — alarm 콜백 인자로 전달. */
	int i;
	/* [한국어] 2초 폴링 루프 카운터(2000 × 1ms). */
	bool removed;
	/* [한국어] DPDK가 제거를 완료했는지 스냅샷. */

	if (!spdk_process_is_primary()) {
		/* [한국어] secondary 프로세스는 물리 자원 소유권이 없으므로 분리를 수행하지 않음. */
		return;
	}

	pthread_mutex_lock(&g_pci_mutex);
	/* [한국어] attached/pending_removal는 alarm·hotremove 스레드와 공유 → 락 보호. */
	dev->internal.attached = false;
	/* [한국어] 더 이상 SPDK가 이 디바이스를 사용하지 않음을 표시. */
	/* prevent the hotremove notification from removing this device */
	dev->internal.pending_removal = true;
	/* [한국어] pci_device_rte_dev_event의 hotremove 경로가 이 디바이스를 중복 제거하지 못하게 차단. */
	pthread_mutex_unlock(&g_pci_mutex);

	rte_eal_alarm_set(1, detach_rte_cb, rte_dev);
	/* [한국어] 1us(인자 단위는 us) 후 detach_rte_cb 1회 실행 예약 — 현재 컨텍스트를 빠져나간 뒤 제거. */

	/* wait up to 2s for the cb to execute */
	for (i = 2000; i > 0; i--) {
		/* [한국어] 최대 2초(2000 × 1000us) 동안 DPDK 제거 완료를 폴링. */

		spdk_delay_us(1000);
		/* [한국어] 1ms 바쁜 대기 — alarm 콜백이 다른 스레드에서 진행하도록 양보 효과. */
		pthread_mutex_lock(&g_pci_mutex);
		removed = dev->internal.removed;
		/* [한국어] pci_device_fini가 설정하는 removed 플래그를 락 하에 읽음. */
		pthread_mutex_unlock(&g_pci_mutex);

		if (removed) {
			/* [한국어] 제거 완료 — 더 기다릴 필요 없음. */
			break;
		}
	}

	/* besides checking the removed flag, we also need to wait
	 * for the dpdk detach function to unwind, as it's doing some
	 * operations even after calling our detach callback. Simply
	 * cancel the alarm - if it started executing already, this
	 * call will block and wait for it to finish.
	 */
	rte_eal_alarm_cancel(detach_rte_cb, rte_dev);
	/* [한국어] alarm 취소. 이미 실행 중이면 이 호출이 콜백 완료까지 블록 → DPDK detach unwind 보장. */

	/* the device could have been finally removed, so just check
	 * it again.
	 */
	pthread_mutex_lock(&g_pci_mutex);
	removed = dev->internal.removed;
	/* [한국어] alarm 완료 직후 최종 상태 재확인(폴링 중엔 아직 false였을 수 있음). */
	pthread_mutex_unlock(&g_pci_mutex);
	if (!removed) {
		/* [한국어] 2초 내 제거 실패 — 동일 BDF로의 후속 hot-add가 실패할 수 있음을 경고. */
		SPDK_ERRLOG("Timeout waiting for DPDK to remove PCI device %s.\n",
			    dpdk_pci_device_get_name(rte_dev));
		/* If we reach this state, then the device couldn't be removed and most likely
		   a subsequent hot add of a device in the same BDF will fail */
	}
}

/*
 * [한국어]
 * spdk_pci_driver_register - SPDK PCI 드라이버를 등록 리스트에 추가
 *
 * @name:     드라이버 식별 이름(예: "nvme", "ioat"). 이후 spdk_pci_get_driver(name)로 조회.
 * @id_table: 매칭할 vendor/device ID 배열(NULL terminated).
 * @flags:    SPDK_PCI_DRIVER_NEED_MAPPING / _WC_ACTIVATE 등 비트 플래그.
 *
 * SPDK_PCI_DRIVER_REGISTER 매크로가 constructor 컨텍스트에서 호출하며, 보통 main 진입 전에
 * 자동 실행된다. calloc 실패 시 묵묵히 반환(부팅 단계라 ERRLOG도 무의미).
 * 실행 컨텍스트: 프로세스 로드 단계, 단일 스레드.
 *
 * 호출 체인: __attribute__((constructor)) → spdk_pci_driver_register → g_pci_drivers 큐 삽입
 */
void
spdk_pci_driver_register(const char *name, struct spdk_pci_id *id_table, uint32_t flags)
{
	struct spdk_pci_driver *driver;
	/* [한국어] 새로 할당할 드라이버 노드. */

	driver = calloc(1, sizeof(*driver));
	/* [한국어] 0으로 초기화된 드라이버 객체 할당. driver_buf까지 모두 0. */
	if (!driver) {
		/* we can't do any better than bailing atm */
		/* [한국어] 부팅 초기 메모리 부족 — 복구 불가, 그냥 종료. 빌드 단계 OOM은 거의 없음. */
		return;
	}

	driver->name = name;
	/* [한국어] 정적 문자열 리터럴 그대로 보관(복사 없음). */
	driver->id_table = id_table;
	/* [한국어] 정적 배열 포인터 보관. */
	driver->drv_flags = flags;
	/* [한국어] 비트 플래그 저장. */
	driver->driver = (struct rte_pci_driver *)driver->driver_buf;
	/* [한국어] driver_buf 위에 in-place로 rte_pci_driver를 둘 것임을 표시.
	 * 실제 채움은 추후 dpdk_pci_driver_register에서 수행. */
	TAILQ_INSERT_TAIL(&g_pci_drivers, driver, tailq);
	/* [한국어] 전역 드라이버 큐 끝에 추가. 순서는 constructor 실행 순(보통 링크 순). */
}

/*
 * [한국어]
 * spdk_pci_nvme_get_driver - "nvme" SPDK PCI 드라이버 핸들 반환
 *
 * @return: NVMe 드라이버 핸들. NVMe 모듈이 빌드에 포함됐다면 non-NULL.
 *
 * lib/nvme/nvme_pcie.c가 spdk_pci_enumerate 호출 시 사용. spdk_pci_get_driver("nvme")의 별칭.
 */
struct spdk_pci_driver *
spdk_pci_nvme_get_driver(void)
{
	/* [한국어] 이름 "nvme"로 조회. NVMe 모듈이 등록되어 있어야 함. */
	return spdk_pci_get_driver("nvme");
}

/*
 * [한국어]
 * spdk_pci_get_driver - 이름으로 등록된 PCI 드라이버 검색
 *
 * @name:   드라이버 이름(예: "nvme", "ioat", "idxd").
 * @return: 매칭 드라이버 핸들, 없으면 NULL.
 *
 * 본 함수는 한 번 등록된 드라이버를 외부에서 조회하는 진입점이다. 호출 빈도가 낮으므로
 * 선형 검색으로 충분하다(드라이버 수 ≤ 10).
 *
 * 호출 체인: 모듈 → spdk_pci_<module>_get_driver → spdk_pci_get_driver → g_pci_drivers 순회
 */
struct spdk_pci_driver *
spdk_pci_get_driver(const char *name)
{
	struct spdk_pci_driver *driver;

	TAILQ_FOREACH(driver, &g_pci_drivers, tailq) {
		if (strcmp(driver->name, name) == 0) {
			return driver;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * pci_device_rte_dev_event - DPDK uevent(hotplug add/remove) 통지 핸들러
 *
 * @device_name: 이벤트가 발생한 디바이스 이름(BDF 문자열).
 * @event:       RTE_DEV_EVENT_ADD 또는 RTE_DEV_EVENT_REMOVE.
 * @cb_arg:      등록 시 전달한 인자(여기선 NULL).
 *
 * udev로부터의 PCI surprise-removal/insert를 SPDK에 반영한다. _pci_env_init이
 * rte_dev_event_callback_register(NULL, …)로 모든 디바이스에 대해 단 하나 등록한다.
 * REMOVE 이벤트 시: g_pci_devices에서 해당 BDF를 찾아 pending_removal을 세워 추가 attach를
 * 막고, 아직 attach 안 된 디바이스라면(can_detach) 1ms alarm으로 즉시 제거를 예약한다.
 * 이미 attach된 디바이스는 사용자가 명시적으로 detach할 때 제거된다.
 * 실행 컨텍스트: DPDK EAL interrupt 스레드. g_pci_mutex로 사용자 스레드와 동기화.
 *
 * 호출 체인: DPDK uevent → [pci_device_rte_dev_event] → rte_eal_alarm_set(detach_rte_cb)
 */
static void
pci_device_rte_dev_event(const char *device_name,
			 enum rte_dev_event_type event,
			 void *cb_arg)
{
	struct spdk_pci_device *dev;
	/* [한국어] g_pci_devices 순회용. */
	bool can_detach = false;
	/* [한국어] attach되지 않아 즉시 제거 가능한지 여부. */

	switch (event) {
	default:
	case RTE_DEV_EVENT_ADD:
		/* Nothing to do here yet. */
		/* [한국어] hot-add는 명시적 attach 경로로 처리되므로 통지만으로는 할 일 없음. */
		break;
	case RTE_DEV_EVENT_REMOVE:
		pthread_mutex_lock(&g_pci_mutex);
		/* [한국어] g_pci_devices 순회 중 interrupt/사용자 스레드 경쟁 방지. */
		TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
			struct rte_pci_device *rte_dev = dev->dev_handle;
			/* [한국어] BDF 이름 비교를 위한 DPDK 핸들. */

			if (dev->internal.removed) {
				/* DPDK already removed this device, we are still pending
				 * removal of the device from the SPDK device list. Since
				 * DPDK freed the device handle, we must not try to
				 * get its device name.
				 */
				/* [한국어] 이미 DPDK가 핸들을 free한 상태 — get_name 호출 시 UAF 위험이므로 건너뜀. */
				continue;
			}

			if (strcmp(dpdk_pci_device_get_name(rte_dev), device_name)) {
				/* [한국어] 이벤트 대상 BDF와 다른 디바이스는 무시. */
				continue;
			}

			if (!dev->internal.pending_removal) {
				/* [한국어] 아직 제거 예약 안 된 디바이스만 처리(중복 방지). */
				can_detach = !dev->internal.attached;
				/* [한국어] attach 안 됐으면 여기서 바로 제거 가능. attach됐으면 detach 시 제거. */
				/* prevent any further attaches */
				dev->internal.pending_removal = true;
				/* [한국어] 이후 attach 시도를 막아 사라진 디바이스를 잡지 못하게 함. */
				break;
			}
		}
		pthread_mutex_unlock(&g_pci_mutex);

		if (can_detach) {
			/* if device is not attached we can remove it right away.
			 * Otherwise it will be removed at detach.
			 *
			 * Because the user's callback is invoked in eal interrupt
			 * callback, the interrupt callback need to be finished before
			 * it can be unregistered when detaching device. So finish
			 * callback soon and use a deferred removal to detach device
			 * is need. It is a workaround, once the device detaching be
			 * moved into the eal in the future, the deferred removal could
			 * be deleted.
			 */
			/* [한국어] 현재 interrupt 콜백 내부이므로 hotplug_remove를 직접 못 부름
			 * → 1ms alarm으로 지연 제거하여 이 콜백을 먼저 종료시킴. */
			assert(dev != NULL);
			/* [한국어] can_detach가 true면 위 루프가 dev를 찾고 break한 것이므로 non-NULL 보장. */
			rte_eal_alarm_set(1, detach_rte_cb, dev->dev_handle);
			/* [한국어] 지연 제거 예약. */
		}
		break;
	}
}

/*
 * [한국어]
 * cleanup_pci_devices - removed 디바이스 회수 + hotplugged 디바이스를 정식 큐로 이관
 *
 * 두 가지 지연 처리를 한 번에 수행한다:
 *   1) g_pci_devices 중 removed=true인 항목 → vtophys 매핑 해제 후 free.
 *   2) g_pci_hotplugged_devices(DPDK 스레드에서 발견된 임시 큐) → g_pci_devices로 이동 +
 *      vtophys_pci_device_added(IOMMU/DMA 매핑 등록).
 * 이 분리는 DPDK probe가 별도 스레드에서 일어나도 실제 큐 갱신은 메인 스레드에서
 * 안전하게 직렬화하기 위함이다.
 * 실행 컨텍스트: attach/enumerate/detach 진입점에서 메인 스레드가 호출. g_pci_mutex 보호.
 *
 * 호출 체인: spdk_pci_device_attach/enumerate/detach → [cleanup_pci_devices] →
 *           vtophys_pci_device_added/removed
 */
static void
cleanup_pci_devices(void)
{
	struct spdk_pci_device *dev, *tmp;
	/* [한국어] FOREACH_SAFE용: 순회 중 제거/이동을 위해 다음 노드 미리 저장. */

	pthread_mutex_lock(&g_pci_mutex);
	/* [한국어] 두 큐 모두 수정하므로 락 보호. */
	/* cleanup removed devices */
	TAILQ_FOREACH_SAFE(dev, &g_pci_devices, internal.tailq, tmp) {
		if (!dev->internal.removed) {
			/* [한국어] 아직 살아있는 디바이스는 그대로 둠. */
			continue;
		}

		vtophys_pci_device_removed(dev->dev_handle);
		/* [한국어] 이 디바이스의 BAR/DMA 영역을 vtophys 변환 테이블에서 제거. */
		TAILQ_REMOVE(&g_pci_devices, dev, internal.tailq);
		/* [한국어] 추적 리스트에서 분리. */
		free(dev);
		/* [한국어] spdk_pci_device 객체 해제. 이후 dev 접근 금지. */
	}

	/* add newly-attached devices */
	TAILQ_FOREACH_SAFE(dev, &g_pci_hotplugged_devices, internal.tailq, tmp) {
		TAILQ_REMOVE(&g_pci_hotplugged_devices, dev, internal.tailq);
		/* [한국어] 임시 hotplug 큐에서 빼서… */
		TAILQ_INSERT_TAIL(&g_pci_devices, dev, internal.tailq);
		/* [한국어] …정식 추적 큐로 이관. 이제 attach/enumerate가 볼 수 있음. */
		vtophys_pci_device_added(dev->dev_handle);
		/* [한국어] 새 디바이스의 BAR/DMA 영역을 vtophys 테이블에 등록(IOMMU 매핑 포함). */
	}
	pthread_mutex_unlock(&g_pci_mutex);
}

/* [한국어] scan_pci_bus 전방 선언 — _pci_env_init이 정의보다 먼저 호출. */
static int scan_pci_bus(bool delay_init);

/*
 * [한국어]
 * _pci_env_init - PCI 초기 스캔 + 단일 hotremove 콜백 등록
 *
 * pci_env_init / pci_env_reinit이 공통으로 호출하는 헬퍼. SPDK 시작 시점에 이미 버스에
 * 존재하던 디바이스는 2초 이상 안정 상태였다고 가정하여 delay_init=false로 스캔(차단 없이
 * 즉시 allow). 이후 모든 디바이스에 대해 hotremove 통지 콜백을 단 하나 등록한다.
 * 실행 컨텍스트: 메인 스레드, EAL init 직후.
 *
 * 호출 체인: pci_env_init/_reinit → [_pci_env_init] → scan_pci_bus / rte_dev_event_callback_register
 */
static inline void
_pci_env_init(void)
{
	/* We assume devices were present on the bus for more than 2 seconds
	 * before initializing SPDK and there's no need to wait more. We scan
	 * the bus, but we don't block any devices.
	 */
	scan_pci_bus(false);
	/* [한국어] delay_init=false: 초기 디바이스는 지연 없이 즉시 allow. */

	/* Register a single hotremove callback for all devices. */
	if (spdk_process_is_primary()) {
		/* [한국어] hotremove 통지는 primary만 처리(자원 소유권). */
		rte_dev_event_callback_register(NULL, pci_device_rte_dev_event, NULL);
		/* [한국어] device_name=NULL → 모든 디바이스에 대한 글로벌 uevent 콜백 등록. */
	}
}

/*
 * [한국어]
 * pci_env_init - SPDK PCI 환경 초기화 진입점
 *
 * @return: 성공 시 0, 실패 시 음수.
 *
 * spdk_env_dpdk_post_init이 호출한다. 다음 순서로 작업:
 *   1) dpdk_pci_init: DPDK 버전 감지 후 fn_table 결정.
 *   2) g_pci_drivers를 순회하며 각 SPDK 드라이버를 DPDK rte_pci 측에 등록(pci_device_init/fini를
 *      probe/remove 콜백으로 묶음).
 *   3) _pci_env_init: scan_pci_bus + rte_dev_event_callback_register(hotremove).
 *
 * 실행 컨텍스트: 메인 스레드, EAL init 직후 1회.
 *
 * 호출 체인: spdk_env_dpdk_post_init → pci_env_init → dpdk_pci_init → dpdk_pci_driver_register
 */
int
pci_env_init(void)
{
	struct spdk_pci_driver *driver;
	/* [한국어] 드라이버 순회 임시. */
	int rc;
	/* [한국어] dpdk_pci_init 결과. */

	rc = dpdk_pci_init();
	/* [한국어] 런타임 DPDK 버전 감지 및 g_dpdk_fn_table 바인딩. */
	if (rc) {
		return rc;
	}

	TAILQ_FOREACH(driver, &g_pci_drivers, tailq) {
		/* [한국어] 등록된 모든 SPDK 드라이버를 DPDK rte_pci_bus에 결합.
		 * probe_fn = pci_device_init, remove_fn = pci_device_fini. */
		dpdk_pci_driver_register(driver, pci_device_init, pci_device_fini);
	}

	_pci_env_init();
	/* [한국어] 첫 PCI bus 스캔 + hotremove 콜백 등록(primary only). */
	return 0;
}

/*
 * [한국어]
 * pci_env_reinit - secondary 프로세스 재초기화 시 PCI 환경 재설정
 *
 * spdk_env_dpdk_post_init이 secondary로서 다시 진입할 때 호출. 드라이버는 이미
 * pci_env_init에서 한 번 DPDK에 등록됐으므로 재등록은 불필요하고, 버스 재스캔 +
 * hotremove 콜백 등록만 다시 수행한다.
 * 실행 컨텍스트: 메인 스레드.
 *
 * 호출 체인: spdk_env_dpdk_post_init(secondary) → [pci_env_reinit] → _pci_env_init
 */
void
pci_env_reinit(void)
{
	/* There is no need to register pci drivers again, since they were
	 * already pre-registered in pci_env_init.
	 */

	_pci_env_init();
	/* [한국어] 드라이버 재등록 없이 스캔 + hotremove 콜백만 재설정. */
}

/*
 * [한국어]
 * pci_env_fini - PCI 환경 종료: 남은 attach 디바이스 경고 + hotremove 콜백 해제
 *
 * spdk_env_dpdk_post_fini 종료 경로에서 호출. cleanup으로 removed/hotplugged 큐를 정리한 뒤,
 * 종료 시점에 여전히 attach 상태로 남은 디바이스가 있으면(드라이버가 detach를 빠뜨린 것이므로)
 * BDF를 찍어 경고한다. 마지막으로 primary면 hotremove 콜백을 해제한다.
 * 실행 컨텍스트: 메인 스레드, 프로세스 종료 단계.
 *
 * 호출 체인: spdk_env_dpdk_post_fini → [pci_env_fini] → cleanup_pci_devices /
 *           rte_dev_event_callback_unregister
 */
void
pci_env_fini(void)
{
	struct spdk_pci_device *dev;
	/* [한국어] 잔존 디바이스 순회용. */
	char bdf[32];
	/* [한국어] 경고 출력용 BDF 문자열 버퍼. */

	cleanup_pci_devices();
	/* [한국어] 지연된 removed 해제 + hotplugged 이관을 먼저 마무리. */
	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		if (dev->internal.attached) {
			/* [한국어] 종료 시점에 attach 상태 = 드라이버가 detach를 누락 → 누수/리소스 점유 경고. */
			spdk_pci_addr_fmt(bdf, sizeof(bdf), &dev->addr);
			/* [한국어] addr → "domain:bus:dev.func" 문자열로 포맷. */
			SPDK_ERRLOG("Device %s is still attached at shutdown!\n", bdf);
		}
	}

	if (spdk_process_is_primary()) {
		/* [한국어] 등록은 primary만 했으므로 해제도 primary만. */
		rte_dev_event_callback_unregister(NULL, pci_device_rte_dev_event, NULL);
		/* [한국어] _pci_env_init에서 등록한 글로벌 hotremove 콜백 제거. */
	}
}

/*
 * [한국어]
 * pci_device_init - DPDK가 PCI 디바이스를 발견했을 때 호출하는 probe 어댑터
 *
 * @_drv: DPDK 측 rte_pci_driver. SPDK는 spdk_pci_driver와 동일 메모리에 placement했으므로 캐스트 가능.
 * @_dev: 새로 발견된 DPDK PCI 디바이스 핸들.
 * @return: 성공 시 0, 실패 시 음수.
 *
 * 동작:
 *   1) spdk_pci_device 구조체 calloc.
 *   2) DPDK 측 addr/id/numa_node 정보를 SPDK 구조체에 복사.
 *   3) BAR/config 접근 함수 포인터(map_bar_rte 등)를 채움.
 *   4) 드라이버 cb_fn(상위 모듈 콜백)이 있으면 호출 — 상위 모듈이 디바이스를 자기 형식으로 변환.
 *   5) g_pci_hotplugged_devices 큐 끝에 추가(cleanup_pci_devices가 g_pci_devices로 이관).
 *
 * 실행 컨텍스트: DPDK bus probe 시점. 메인 스레드(또는 hotplug interrupt 스레드).
 *
 * 호출 체인: dpdk_bus_probe → rte_pci_probe_one → 본 함수 → driver->cb_fn (예: NVMe attach)
 */
int
pci_device_init(struct rte_pci_driver *_drv,
		struct rte_pci_device *_dev)
{
	struct spdk_pci_driver *driver = (struct spdk_pci_driver *)_drv;
	/* [한국어] SPDK가 driver_buf 위에 rte_pci_driver를 만들었으므로, 거꾸로 spdk_pci_driver*로 캐스트해도 안전. */
	struct spdk_pci_device *dev;
	/* [한국어] 새로 만들 SPDK 디바이스 객체. */
	struct rte_pci_addr *addr;
	/* [한국어] DPDK 측 BDF 주소 구조체. */
	struct rte_pci_id *id;
	/* [한국어] DPDK 측 vendor/device/subsystem ID. */
	int rc;
	/* [한국어] cb_fn 반환값. */

	dev = calloc(1, sizeof(*dev));
	/* [한국어] 0 초기화된 디바이스 객체 할당(모든 internal 플래그 false로 시작). */
	if (dev == NULL) {
		/* [한국어] OOM — DPDK에 probe 실패(-1) 보고. */
		return -1;
	}

	dev->dev_handle = _dev;
	/* [한국어] DPDK 핸들 보관 — 이후 모든 dpdk_pci_* 어댑터 호출의 키. */

	addr = dpdk_pci_device_get_addr(_dev);
	/* [한국어] DPDK에서 BDF 추출. */
	dev->addr.domain = addr->domain;
	/* [한국어] PCI domain(segment). */
	dev->addr.bus = addr->bus;
	/* [한국어] PCI bus 번호. */
	dev->addr.dev = addr->devid;
	/* [한국어] PCI device(slot) 번호. DPDK는 devid 필드명 사용. */
	dev->addr.func = addr->function;
	/* [한국어] PCI function 번호. */

	id = dpdk_pci_device_get_id(_dev);
	/* [한국어] DPDK에서 식별자 추출. */
	dev->id.class_id = id->class_id;
	/* [한국어] PCI class code(예: 0x010802 = NVMe). */
	dev->id.vendor_id = id->vendor_id;
	/* [한국어] 벤더 ID — 드라이버 id_table 매칭에 사용. */
	dev->id.device_id = id->device_id;
	/* [한국어] 디바이스 ID. */
	dev->id.subvendor_id = id->subsystem_vendor_id;
	/* [한국어] 서브시스템 벤더 ID(quirk 매칭 등에 사용). */
	dev->id.subdevice_id = id->subsystem_device_id;
	/* [한국어] 서브시스템 디바이스 ID. */

	dev->numa_id = dpdk_pci_device_get_numa_node(_dev);
	/* [한국어] 디바이스가 붙은 NUMA 노드 — DMA 메모리 NUMA-local 할당 최적화에 사용. */
	dev->type = "pci";
	/* [한국어] provider 타입 문자열. detach 시 "pci" provider의 detach_cb 선택에 사용. */

	dev->map_bar = map_bar_rte;
	/* [한국어] BAR 매핑 함수 포인터를 rte 구현으로 바인딩. */
	dev->unmap_bar = unmap_bar_rte;
	/* [한국어] BAR 해제(no-op) 바인딩. */
	dev->cfg_read = cfg_read_rte;
	/* [한국어] config space read 바인딩. */
	dev->cfg_write = cfg_write_rte;
	/* [한국어] config space write 바인딩. */

	dev->internal.driver = driver;
	/* [한국어] 이 디바이스를 발견한 SPDK 드라이버 기록(enumerate 매칭에 사용). */
	dev->internal.claim_fd = -1;
	/* [한국어] 아직 claim(flock) 안 함 → -1. */

	if (driver->cb_fn != NULL) {
		/* [한국어] attach/enumerate가 임시로 설정한 사용자 콜백이 있으면 즉시 위임. */
		rc = driver->cb_fn(driver->cb_arg, dev);
		/* [한국어] 상위 모듈(예: NVMe)이 디바이스를 자기 형식으로 변환/초기화. */
		if (rc != 0) {
			/* [한국어] 상위 모듈이 거부 → 객체 폐기 후 probe 실패 전파. */
			free(dev);
			return rc;
		}
		dev->internal.attached = true;
		/* [한국어] 콜백 성공 → attach 완료 표시. */
	}

	pthread_mutex_lock(&g_pci_mutex);
	/* [한국어] hotplugged 큐는 DPDK 스레드/메인 스레드 공유 → 락. */
	TAILQ_INSERT_TAIL(&g_pci_hotplugged_devices, dev, internal.tailq);
	/* [한국어] 정식 큐가 아닌 임시 큐에 넣음 → cleanup_pci_devices가 메인 스레드에서 이관. */
	pthread_mutex_unlock(&g_pci_mutex);
	return 0;
	/* [한국어] DPDK에 probe 성공 보고. */
}

/*
 * [한국어]
 * set_allowed_at - 디바이스의 attach 허용 시각(allowed_at)을 기록/갱신
 *
 * @rte_da: 대상 디바이스의 DPDK devargs(bus + name 보유).
 * @tsc:    허용 시각(TSC tick). 0이면 "허용 정보 제거" 의미로 사용.
 *
 * scan_pci_bus가 디바이스를 처음 보거나 정책을 바꿀 때, 그리고 pci_device_fini가 detach 시
 * 이 옵션을 지울 때 호출한다. find_env_devargs로 기존 노드를 찾고, 없으면 새로 만들어
 * g_env_devargs에 넣는다. 새로 만들 때만 bus/name을 복사하고, allowed_at은 항상 덮어쓴다.
 * 실행 컨텍스트: 메인 스레드(scan/attach/fini). g_env_devargs는 init 직렬화 가정으로 무락.
 *
 * 호출 체인: scan_pci_bus / spdk_pci_device_attach / pci_device_fini → [set_allowed_at] →
 *           find_env_devargs / spdk_strcpy_pad
 */
static void
set_allowed_at(struct rte_devargs *rte_da, uint64_t tsc)
{
	struct env_devargs *env_da;
	/* [한국어] 갱신 대상(또는 새로 만들) env_devargs 노드. */

	env_da = find_env_devargs(rte_da->bus, rte_da->name);
	/* [한국어] 이미 추적 중인 디바이스인지 조회. */
	if (env_da == NULL) {
		/* [한국어] 처음 보는 디바이스 → 노드 신규 생성. */
		env_da = calloc(1, sizeof(*env_da));
		if (env_da == NULL) {
			/* [한국어] OOM — 허용 시각 기록 실패. 정책 기본값으로 동작하게 됨. */
			SPDK_ERRLOG("could not set_allowed_at for device %s\n", rte_da->name);
			return;
		}
		env_da->bus = rte_da->bus;
		/* [한국어] 비교 키(bus) 복사. */
		spdk_strcpy_pad(env_da->name, rte_da->name, sizeof(env_da->name), 0);
		/* [한국어] 이름을 128바이트 고정 버퍼에 NUL 패딩 복사(오버런 방지). */
		TAILQ_INSERT_TAIL(&g_env_devargs, env_da, link);
		/* [한국어] 전역 리스트에 등록. */
	}

	env_da->allowed_at = tsc;
	/* [한국어] 허용 시각 갱신(0이면 사실상 "허용 정보 리셋"). */
}

/*
 * [한국어]
 * get_allowed_at - 디바이스의 기록된 허용 시각 조회
 *
 * @rte_da: 대상 디바이스 devargs.
 * @return: 기록된 allowed_at(TSC tick). 기록이 없으면 0.
 *
 * scan_pci_bus가 "이전에 SPDK가 이 디바이스를 본 적이 있는가, 있다면 언제부터 허용인가"를
 * 판단할 때 사용한다. 0 반환은 "처음 보는 디바이스"를 의미.
 * 실행 컨텍스트: 메인 스레드.
 *
 * 호출 체인: scan_pci_bus / spdk_pci_device_attach → [get_allowed_at] → find_env_devargs
 */
static uint64_t
get_allowed_at(struct rte_devargs *rte_da)
{
	struct env_devargs *env_da;
	/* [한국어] 조회 결과 노드. */

	env_da = find_env_devargs(rte_da->bus, rte_da->name);
	/* [한국어] bus+name으로 노드 검색. */
	if (env_da) {
		/* [한국어] 기록 존재 → 저장된 허용 시각 반환. */
		return env_da->allowed_at;
	} else {
		/* [한국어] 기록 없음 → 0(= 처음 보는 디바이스). */
		return 0;
	}
}

/*
 * [한국어]
 * pci_device_fini - DPDK가 PCI 디바이스를 제거할 때 호출하는 remove 어댑터
 *
 * @_dev:   제거 대상 DPDK 디바이스 핸들.
 * @return: 성공 시 0, 아직 attach 중이거나 미발견이면 -EBUSY.
 *
 * pci_env_init에서 각 드라이버의 remove_fn으로 등록된다. g_pci_devices에서 핸들이 일치하는
 * SPDK 디바이스를 찾아, 아직 attach 상태면 -EBUSY로 거부(SPDK 어딘가가 참조 중일 수 있음).
 * 안전하면 allowed_at 옵션을 지우고 removed=true로 마킹만 한다(실제 free는 cleanup_pci_devices).
 * 멀티프로세스 경합(같은 디바이스를 두 프로세스가 동시에 detach)으로 removed가 이미 set돼
 * 있을 수 있으므로 assert하지 않는다(SPDK issue #2456).
 * 실행 컨텍스트: DPDK remove 경로(메인/alarm 스레드). g_pci_mutex 보호.
 *
 * 호출 체인: rte_eal_hotplug_remove → dpdk bus remove → [pci_device_fini]
 */
int
pci_device_fini(struct rte_pci_device *_dev)
{
	struct spdk_pci_device *dev;
	/* [한국어] 핸들 매칭으로 찾을 SPDK 디바이스. */

	pthread_mutex_lock(&g_pci_mutex);
	/* [한국어] g_pci_devices 순회 + removed 마킹 보호. */
	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		if (dev->dev_handle == _dev) {
			/* [한국어] DPDK 핸들이 일치하는 SPDK 디바이스 발견. */
			break;
		}
	}

	if (dev == NULL || dev->internal.attached) {
		/* The device might be still referenced somewhere in SPDK. */
		/* [한국어] 미발견(이미 정리됨) 또는 아직 attach 중 → 제거 거부.
		 * attach 중인 디바이스를 free하면 상위 모듈이 dangling pointer를 잡게 됨. */
		pthread_mutex_unlock(&g_pci_mutex);
		return -EBUSY;
	}

	/* remove our allowed_at option */
	if (dpdk_pci_device_get_devargs(_dev)) {
		/* [한국어] devargs가 있으면 allowed_at=0으로 리셋 → 다음 스캔 때 "처음 보는 디바이스"로 취급. */
		set_allowed_at(dpdk_pci_device_get_devargs(_dev), 0);
	}

	/* It is possible that removed flag was already set when there is a race
	 * between the remove notification for this process, and another process
	 * that is also detaching from this same device (for example, when using
	 * nvme driver in multi-process mode.  So do not assert here.  See
	 * #2456 for additional details.
	 */
	dev->internal.removed = true;
	/* [한국어] 제거 예약 마킹. 실제 큐 제거/free는 cleanup_pci_devices(메인 스레드)가 수행.
	 * 멀티프로세스 경합으로 이미 true일 수 있으므로 assert 없이 단순 set. */
	pthread_mutex_unlock(&g_pci_mutex);
	return 0;

}

/*
 * [한국어]
 * spdk_pci_device_detach - 외부 노출 디바이스 detach API
 *
 * @dev: 분리할 SPDK PCI 디바이스(반드시 attach 상태여야 함).
 *
 * 사용자/상위 모듈이 디바이스 사용을 마쳤을 때 호출. claim(flock)이 걸려 있으면 먼저
 * 해제하고, dev->type("pci"/"vmd" 등)에 맞는 provider를 찾아 그 detach_cb를 호출한다.
 * "pci" provider의 경우 detach_rte가 DPDK hotremove를 수행한다. 마지막에 cleanup_pci_devices로
 * 실제 큐 제거/free를 마무리한다.
 * 실행 컨텍스트: 메인 스레드(사용자 컨텍스트).
 *
 * 호출 체인: 사용자/NVMe → [spdk_pci_device_detach] → provider->detach_cb → cleanup_pci_devices
 */
void
spdk_pci_device_detach(struct spdk_pci_device *dev)
{
	struct spdk_pci_device_provider *provider;
	/* [한국어] dev->type에 매칭되는 attach 제공자. */

	assert(dev->internal.attached);
	/* [한국어] attach 안 된 디바이스 detach는 호출자 버그. */

	if (dev->internal.claim_fd >= 0) {
		/* [한국어] 다중 프로세스 lock 파일이 잡혀 있으면 먼저 해제(flock + unlink). */
		spdk_pci_device_unclaim(dev);
	}

	TAILQ_FOREACH(provider, &g_pci_device_providers, tailq) {
		if (strcmp(dev->type, provider->name) == 0) {
			/* [한국어] 타입 문자열이 일치하는 provider 발견("pci" → g_pci_rte_provider). */
			break;
		}
	}

	assert(provider != NULL);
	/* [한국어] dev->type은 항상 등록된 provider 중 하나여야 함. */
	dev->internal.attached = false;
	/* [한국어] 더 이상 attach 아님으로 표시(detach_cb 진입 전 상태 정리). */
	provider->detach_cb(dev);
	/* [한국어] provider별 실제 분리 수행(pci → detach_rte → DPDK hotremove). */

	cleanup_pci_devices();
	/* [한국어] removed 마킹된 디바이스를 큐에서 빼고 free. */
}

/*
 * [한국어]
 * scan_pci_bus - PCI 버스를 스캔하고 디바이스별 allow/block 정책 결정
 *
 * @delay_init: true면 새로 발견된 디바이스를 2초간 BLOCKED로 두었다가 허용(지연 init).
 * @return:     성공 0, devargs 할당/파싱 실패 -1.
 *
 * DPDK 버스를 다시 스캔하여 모든 PCI 디바이스를 순회한다. 각 디바이스의 rte_devargs가 없으면
 * "pci:<BDF>" 문자열로 생성/insert하고, allowed_at 기록에 따라 정책을 조정한다:
 *  - 이전에 SPDK가 본 디바이스(allowed_at 존재): 허용 시각이 지났고 BLOCKED면 ALLOWED로 전환.
 *  - 처음 보는 디바이스: delay_init이면 BLOCKED + (now+2초) 허용 예약, 아니면 즉시 ALLOWED.
 * 지연 init은 부팅 직후 막 꽂힌 디바이스가 불안정한 add/remove를 반복하는 것을 막기 위함.
 * 실행 컨텍스트: 메인 스레드. enumerate/init 경로에서 호출.
 *
 * 호출 체인: _pci_env_init / spdk_pci_enumerate → [scan_pci_bus] → dpdk_bus_scan /
 *           rte_devargs_parse / set_allowed_at
 */
static int
scan_pci_bus(bool delay_init)
{
	struct rte_dev_iterator it;
	/* [한국어] RTE_DEV_FOREACH 순회 상태. */
	struct rte_device *rte_dev;
	/* [한국어] 현재 순회 중인 DPDK 디바이스. */
	uint64_t now;
	/* [한국어] 스캔 시작 시각(TSC) — 허용 정책 비교 기준. */

	dpdk_bus_scan();
	/* [한국어] DPDK가 sysfs를 다시 읽어 버스의 현재 디바이스 목록을 갱신. */
	now = spdk_get_ticks();
	/* [한국어] 현재 TSC tick 캡처. */

	if (!TAILQ_FIRST(&g_pci_drivers)) {
		/* [한국어] 등록된 SPDK 드라이버가 하나도 없으면 정책을 건드릴 이유 없음 → 조기 반환. */
		return 0;
	}

	RTE_DEV_FOREACH(rte_dev, "bus=pci", &it) {
		/* [한국어] "bus=pci" 필터로 모든 PCI 디바이스 순회. */
		struct rte_devargs *da;
		/* [한국어] 현재 디바이스의 정책/인자 구조체. */

		da = dpdk_device_get_devargs(rte_dev);
		/* [한국어] 기존 devargs 조회. */
		if (!da) {
			/* [한국어] 정책 정보가 아직 없는 디바이스 → 새 devargs 생성. */
			char devargs_str[128];
			/* [한국어] "pci:<BDF>" 형식 인자 문자열. */

			/* the device was never blocked or allowed */
			da = calloc(1, sizeof(*da));
			if (!da) {
				/* [한국어] OOM → 스캔 실패. */
				return -1;
			}

			snprintf(devargs_str, sizeof(devargs_str), "pci:%s", dpdk_device_get_name(rte_dev));
			/* [한국어] "pci:0000:01:00.0" 같은 인자 문자열 구성. */
			if (rte_devargs_parse(da, devargs_str) != 0) {
				/* [한국어] 파싱 실패 → 방금 할당한 da 해제 후 실패. */
				free(da);
				return -1;
			}

			rte_devargs_insert(&da);
			/* [한국어] DPDK 글로벌 devargs 리스트에 등록(이후 DPDK가 소유). */
			dpdk_device_set_devargs(rte_dev, da);
			/* [한국어] 디바이스에 devargs 연결. */
		}

		if (get_allowed_at(da)) {
			/* [한국어] SPDK가 이전에 본 디바이스 → 허용 시각 기록 존재. */
			uint64_t allowed_at = get_allowed_at(da);
			/* [한국어] 기록된 허용 시각. */

			/* this device was seen by spdk before... */
			if (da->policy == RTE_DEV_BLOCKED && allowed_at <= now) {
				/* [한국어] 지연 차단 시간이 지났으면 이제 허용으로 승격. */
				da->policy = RTE_DEV_ALLOWED;
			}
		} else if ((dpdk_device_scan_allowed(rte_dev) && da->policy == RTE_DEV_ALLOWED) ||
			   da->policy != RTE_DEV_BLOCKED) {
			/* override the policy only if not permanently blocked */
			/* [한국어] 사용자가 영구 차단(allowlist 제외)하지 않은 디바이스만 정책 조정. */

			if (delay_init) {
				/* [한국어] 지연 init: 일단 BLOCKED 후 2초 뒤 허용 → 불안정 hotplug 진정. */
				da->policy = RTE_DEV_BLOCKED;
				set_allowed_at(da, now + 2 * spdk_get_ticks_hz());
				/* [한국어] 허용 시각 = 현재 + 2초(ticks_hz = 1초당 TSC tick). */
			} else {
				/* [한국어] 즉시 init: 바로 허용(부팅 시 이미 안정적인 디바이스 가정). */
				da->policy = RTE_DEV_ALLOWED;
				set_allowed_at(da, now);
				/* [한국어] 허용 시각 = 지금. */
			}
		}
	}

	return 0;
	/* [한국어] 모든 디바이스 정책 갱신 완료. */
}

/*
 * [한국어]
 * pci_attach_rte - "pci" provider의 attach_cb: BDF로 DPDK hotplug add 수행
 *
 * @addr:   attach할 디바이스의 BDF.
 * @return: 성공 0, 실패 시 음수 errno.
 *
 * spdk_pci_device_attach가 provider 순회 중 "pci" provider를 통해 호출한다.
 * rte_eal_hotplug_add("pci", BDF, "")로 DPDK에 디바이스를 붙이며, -ENOMSG(IPC 충돌)는
 * 최대 4회 재시도한다. 재시도 후 -EEXIST가 나오면 이전 요청이 실제로는 성공한 것이므로 0으로 정규화.
 * 실행 컨텍스트: 메인 스레드(attach 호출 스레드).
 *
 * 호출 체인: spdk_pci_device_attach → provider->attach_cb(=pci_attach_rte) → rte_eal_hotplug_add
 */
static int
pci_attach_rte(const struct spdk_pci_addr *addr)
{
	char bdf[32];
	/* [한국어] hotplug_add에 넘길 BDF 문자열. */
	int rc, i = 0;
	/* [한국어] rc = 반환값, i = 재시도 카운터. */

	spdk_pci_addr_fmt(bdf, sizeof(bdf), addr);
	/* [한국어] addr 구조체 → "domain:bus:dev.func" 문자열로 변환. */

	do {
		/* [한국어] "pci" 버스에 BDF 디바이스를 옵션 없이("") attach 시도. */
		rc = rte_eal_hotplug_add("pci", bdf, "");
	} while (rc == -ENOMSG && ++i <= DPDK_HOTPLUG_RETRY_COUNT);
	/* [한국어] -ENOMSG = 다중 프로세스 IPC 충돌, 최대 4회 재시도. */

	if (i > 1 && rc == -EEXIST) {
		/* Even though the previous request timed out, the device
		 * was attached successfully.
		 */
		/* [한국어] 재시도가 있었고 -EEXIST면, 앞선 요청이 타임아웃 후 실제로는 성공했다는 의미 → 성공 처리. */
		rc = 0;
	}

	return rc;
	/* [한국어] attach 결과 반환. */
}

/* [한국어] 기본 "pci" attach 제공자 인스턴스. attach=DPDK hotplug, detach=detach_rte. */
static struct spdk_pci_device_provider g_pci_rte_provider = {
	.name = "pci",
	/* [한국어] provider 식별 이름. dev->type="pci"와 매칭. */
	.attach_cb = pci_attach_rte,
	/* [한국어] BDF로 DPDK hotplug add. */
	.detach_cb = detach_rte,
	/* [한국어] DPDK hotplug remove(지연 alarm 기반). */
};

/* [한국어] constructor 시점에 g_pci_rte_provider를 g_pci_device_providers에 등록하는 매크로. */
SPDK_PCI_REGISTER_DEVICE_PROVIDER(pci, &g_pci_rte_provider);

/*
 * [한국어]
 * spdk_pci_device_attach - 외부 노출 단일 디바이스 attach API
 *
 * @driver:      대상 드라이버(spdk_pci_*_get_driver()로 얻음).
 * @enum_cb:     디바이스가 성공적으로 attach되었을 때 호출될 사용자 콜백.
 * @enum_ctx:    enum_cb의 첫 인자(사용자 컨텍스트).
 * @pci_address: 대상 디바이스의 BDF(domain:bus:dev:func).
 * @return:      성공 시 0, 디바이스 미발견/이미 사용 중이면 -1.
 *
 * 동작:
 *   1) cleanup_pci_devices로 미반영 hotremove 정리.
 *   2) g_pci_devices에서 BDF 일치 + 동일 driver인 기존 디바이스 검색 → 있으면 즉시 enum_cb.
 *   3) 없으면 driver->cb_fn/cb_arg를 임시 설정하고 provider->attach_cb로 DPDK hotplug add 시도.
 *      provider 순회로 "pci"/"vmd" 등 여러 백엔드를 시도.
 *   4) cleanup_pci_devices로 새로 attach된 디바이스를 정식 큐로 이관.
 *
 * 실행 컨텍스트: 메인 스레드(또는 attach 호출 스레드). cb_fn은 동일 스레드에서 호출.
 *
 * 호출 체인: 사용자/NVMe → spdk_pci_device_attach → pci_attach_rte → rte_eal_hotplug_add →
 *           dpdk bus probe → pci_device_init → driver->cb_fn(=enum_cb)
 */
int
spdk_pci_device_attach(struct spdk_pci_driver *driver,
		       spdk_pci_enum_cb enum_cb,
		       void *enum_ctx, struct spdk_pci_addr *pci_address)
{
	struct spdk_pci_device *dev;
	/* [한국어] BDF 매칭으로 찾을(또는 새로 attach될) 디바이스. */
	struct spdk_pci_device_provider *provider;
	/* [한국어] 시도할 attach 제공자(pci/vmd 등). */
	struct rte_pci_device *rte_dev;
	/* [한국어] allowlist 갱신용 DPDK 핸들. */
	struct rte_devargs *da;
	/* [한국어] allowlist 정책 조정용 devargs. */
	int rc;
	/* [한국어] 단계별 반환값. */

	cleanup_pci_devices();
	/* [한국어] 지연된 removed/hotplugged 처리를 먼저 반영해 최신 상태로 검색. */

	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		if (spdk_pci_addr_compare(&dev->addr, pci_address) == 0) {
			/* [한국어] 요청 BDF와 동일한 기존 디바이스 발견. */
			break;
		}
	}

	if (dev != NULL && dev->internal.driver == driver) {
		/* [한국어] 이미 같은 드라이버로 발견된 디바이스 → 새 hotplug 없이 즉시 콜백 경로. */
		pthread_mutex_lock(&g_pci_mutex);
		if (dev->internal.attached || dev->internal.pending_removal) {
			/* [한국어] 이미 attach됐거나 제거 진행 중이면 attach 불가. */
			pthread_mutex_unlock(&g_pci_mutex);
			return -1;
		}

		rc = enum_cb(enum_ctx, dev);
		/* [한국어] 상위 모듈 콜백 직접 호출(이미 발견된 디바이스이므로 probe 불필요). */
		if (rc == 0) {
			/* [한국어] 콜백 성공 → attach 완료 표시. */
			dev->internal.attached = true;
		}
		pthread_mutex_unlock(&g_pci_mutex);
		return rc;
	}

	driver->cb_fn = enum_cb;
	/* [한국어] pci_device_init이 probe 직후 호출할 수 있게 콜백을 드라이버에 임시 게시. */
	driver->cb_arg = enum_ctx;
	/* [한국어] 콜백 컨텍스트도 함께 게시. */

	rc = -ENODEV;
	/* [한국어] provider가 하나도 성공 못 했을 때 기본값. */
	TAILQ_FOREACH(provider, &g_pci_device_providers, tailq) {
		/* [한국어] 등록된 모든 provider("pci","vmd"…)를 순서대로 시도. */
		rc = provider->attach_cb(pci_address);
		if (rc == 0) {
			/* [한국어] 어느 provider가 성공하면 그것으로 확정. */
			break;
		}
	}

	driver->cb_arg = NULL;
	/* [한국어] 임시 게시한 콜백 컨텍스트 제거(다른 attach 경로 오염 방지). */
	driver->cb_fn = NULL;
	/* [한국어] 임시 콜백 제거. */

	cleanup_pci_devices();
	/* [한국어] probe로 hotplugged 큐에 들어간 새 디바이스를 정식 큐로 이관. */

	if (rc != 0) {
		/* [한국어] 모든 provider 실패 → attach 실패. */
		return -1;
	}

	/* explicit attach ignores the allowlist, so if we blocked this
	 * device before let's enable it now - just for clarity.
	 */
	/* [한국어] 명시적 attach는 allowlist를 무시하므로, 이전에 BLOCKED였다면 명확성을 위해 ALLOWED로 갱신. */
	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		if (spdk_pci_addr_compare(&dev->addr, pci_address) == 0) {
			/* [한국어] 방금 attach된 디바이스를 다시 찾음. */
			break;
		}
	}
	assert(dev != NULL);
	/* [한국어] attach 성공했으므로 반드시 큐에 존재. */

	rte_dev = dev->dev_handle;
	/* [한국어] DPDK 핸들 확보. */
	if (rte_dev != NULL) {
		da = dpdk_pci_device_get_devargs(rte_dev);
		/* [한국어] 정책 조정 대상 devargs 조회. */
		if (da && get_allowed_at(da)) {
			/* [한국어] SPDK가 이전에 본(allowed_at 기록 있는) 디바이스면 정책 명시적 갱신. */
			set_allowed_at(da, spdk_get_ticks());
			/* [한국어] 허용 시각을 지금으로. */
			da->policy = RTE_DEV_ALLOWED;
			/* [한국어] 정책을 ALLOWED로 확정. */
		}
	}

	return 0;
	/* [한국어] attach 성공. */
}

/* Note: You can call spdk_pci_enumerate from more than one thread
 *       simultaneously safely, but you cannot call spdk_pci_enumerate
 *       and rte_eal_pci_probe simultaneously.
 */
/*
 * [한국어]
 * spdk_pci_enumerate - 드라이버에 매칭되는 모든 PCI 디바이스를 발견·attach
 *
 * @driver:   매칭 기준 SPDK 드라이버(id_table로 vendor/device 필터).
 * @enum_cb:  각 매칭 디바이스마다 호출될 사용자 콜백.
 * @enum_ctx: enum_cb의 첫 인자.
 * @return:   성공 0, 실패 -1(콜백이 음수 반환 또는 스캔/probe 실패).
 *
 * lib/nvme 등이 "사용 가능한 모든 NVMe를 한 번에 잡기" 위해 호출한다. 2단계로 동작:
 *  1) 이미 g_pci_devices에 있고 아직 attach 안 된 매칭 디바이스를 즉시 enum_cb로 처리.
 *  2) scan_pci_bus(true)로 새 디바이스를 발견(지연 init) + dpdk_bus_probe로 probe 트리거 →
 *     pci_device_init가 driver->cb_fn(=enum_cb)을 호출.
 * 여러 스레드에서 동시 호출은 안전하나, DPDK probe와 동시 실행은 불가.
 * 실행 컨텍스트: 메인 스레드. g_pci_mutex로 1단계 순회 보호.
 *
 * 호출 체인: lib/nvme(nvme_pcie_ctrlr_scan) → [spdk_pci_enumerate] → scan_pci_bus / dpdk_bus_probe
 */
int
spdk_pci_enumerate(struct spdk_pci_driver *driver,
		   spdk_pci_enum_cb enum_cb,
		   void *enum_ctx)
{
	struct spdk_pci_device *dev;
	/* [한국어] 순회 대상 디바이스. */
	int rc;
	/* [한국어] 콜백/probe 반환값. */

	cleanup_pci_devices();
	/* [한국어] 지연 처리 반영 후 최신 큐로 순회. */

	pthread_mutex_lock(&g_pci_mutex);
	/* [한국어] 1단계 순회 + attached 마킹 보호. */
	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		if (dev->internal.attached ||
		    dev->internal.driver != driver ||
		    dev->internal.pending_removal) {
			/* [한국어] 이미 attach됐거나, 다른 드라이버 소속이거나, 제거 중인 디바이스는 건너뜀. */
			continue;
		}

		rc = enum_cb(enum_ctx, dev);
		/* [한국어] 매칭 디바이스에 사용자 콜백 적용. */
		if (rc == 0) {
			/* [한국어] 성공 → attach 표시. */
			dev->internal.attached = true;
		} else if (rc < 0) {
			/* [한국어] 콜백이 치명 오류(음수) → 즉시 실패 반환(양수는 "skip" 의미로 계속). */
			pthread_mutex_unlock(&g_pci_mutex);
			return -1;
		}
	}
	pthread_mutex_unlock(&g_pci_mutex);

	if (scan_pci_bus(true) != 0) {
		/* [한국어] 2단계: 새 디바이스 발견(지연 init). 실패 시 enumerate 실패. */
		return -1;
	}

	driver->cb_fn = enum_cb;
	/* [한국어] probe 도중 pci_device_init이 호출할 콜백 임시 게시. */
	driver->cb_arg = enum_ctx;
	/* [한국어] 콜백 컨텍스트 게시. */

	if (dpdk_bus_probe() != 0) {
		/* [한국어] DPDK probe 트리거 → 새로 ALLOWED된 디바이스에 대해 pci_device_init 실행.
		 * 실패 시 임시 콜백을 정리하고 실패 반환. */
		driver->cb_arg = NULL;
		driver->cb_fn = NULL;
		return -1;
	}

	driver->cb_arg = NULL;
	/* [한국어] probe 종료 후 임시 콜백 정리. */
	driver->cb_fn = NULL;

	cleanup_pci_devices();
	/* [한국어] probe로 hotplugged된 디바이스를 정식 큐로 이관. */
	return 0;
	/* [한국어] enumerate 성공. */
}

/*
 * [한국어]
 * spdk_pci_for_each_device - 추적 중인 모든 PCI 디바이스에 콜백 적용
 *
 * @ctx: fn에 전달할 사용자 컨텍스트.
 * @fn:  각 디바이스마다 호출될 함수.
 *
 * 디버그/통계 수집 등에서 현재 g_pci_devices의 전체 디바이스를 안전하게 순회한다.
 * FOREACH_SAFE를 써서 fn이 디바이스를 제거하더라도 순회가 깨지지 않는다.
 * 실행 컨텍스트: 임의 스레드(g_pci_mutex로 보호). fn은 락 보유 상태에서 호출되므로
 * fn 내부에서 attach/detach/enumerate를 호출하면 데드락 주의.
 *
 * 호출 체인: 사용자 → [spdk_pci_for_each_device] → fn(ctx, dev)
 */
void
spdk_pci_for_each_device(void *ctx, void (*fn)(void *ctx, struct spdk_pci_device *dev))
{
	struct spdk_pci_device *dev, *tmp;
	/* [한국어] SAFE 순회용 현재/다음 노드. */

	pthread_mutex_lock(&g_pci_mutex);
	/* [한국어] 순회 중 큐 변경으로부터 보호. */
	TAILQ_FOREACH_SAFE(dev, &g_pci_devices, internal.tailq, tmp) {
		/* [한국어] fn이 dev를 제거해도 tmp로 다음을 안전하게 잇는다. */
		fn(ctx, dev);
	}
	pthread_mutex_unlock(&g_pci_mutex);
}

/*
 * [한국어]
 * spdk_pci_device_map_bar - BAR을 사용자 공간에 매핑하고 필요 시 IOMMU에 등록
 *
 * @dev:         대상 디바이스.
 * @bar:         BAR 인덱스(0~5). NVMe는 보통 BAR0에 컨트롤러 레지스터.
 * @mapped_addr: 출력. BAR이 매핑된 호스트 가상주소(MMIO 접근에 직접 사용).
 * @phys_addr:   출력. IOVA(IOMMU 모드에 따라 VA 또는 물리주소).
 * @size:        출력. BAR 크기.
 * @return:      성공 0, IOMMU 매핑 실패 -EFAULT, 그 외 음수.
 *
 * dev->map_bar(=map_bar_rte)로 DPDK가 미리 mmap한 BAR 정보를 얻은 뒤, VFIO+IOMMU가 켜져 있으면
 * 그 BAR 영역을 IOMMU에도 매핑한다. IOVA 모드가 VA면 가상주소를 IOVA로(DPDK와 일치),
 * PA 모드면 물리주소를 IOVA로 사용한다. 이는 디바이스가 P2P DMA 등으로 이 BAR을 IOVA로
 * 참조할 수 있게 하기 위함이다.
 * 실행 컨텍스트: 메인 스레드(드라이버 init). 호출 체인: NVMe ctrlr_construct → [본 함수] →
 *   dev->map_bar / vtophys_iommu_map_dma_bar.
 */
int
spdk_pci_device_map_bar(struct spdk_pci_device *dev, uint32_t bar,
			void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	int rc;
	/* [한국어] map_bar / iommu_map 반환값. */

	rc = dev->map_bar(dev, bar, mapped_addr, phys_addr, size);
	/* [한국어] DPDK가 mmap한 BAR의 가상주소/물리주소/크기 획득. */
	if (rc) {
		/* [한국어] BAR 정보 조회 실패 → 그대로 전파. */
		return rc;
	}

#if VFIO_ENABLED
	/* Automatically map the BAR to the IOMMU */
	if (!spdk_iommu_is_enabled()) {
		/* [한국어] IOMMU 비활성(예: uio_pci_generic, no-IOMMU) → IOMMU 매핑 불필요. */
		return 0;
	}

	if (rte_eal_iova_mode() == RTE_IOVA_VA) {
		/* We'll use the virtual address as the iova to match DPDK. */
		/* [한국어] IOVA=VA 모드: 가상주소 자체를 IOVA로 사용(DPDK 정책과 일치). */
		rc = vtophys_iommu_map_dma_bar((uint64_t)(*mapped_addr), (uint64_t) * mapped_addr, *size);
		/* [한국어] IOMMU에 (VA=IOVA) 매핑 등록. */
		if (rc) {
			/* [한국어] 매핑 실패 → BAR unmap 후 EFAULT. */
			dev->unmap_bar(dev, bar, *mapped_addr);
			return -EFAULT;
		}

		*phys_addr = (uint64_t)(*mapped_addr);
		/* [한국어] IOVA가 곧 VA이므로 phys_addr(=IOVA)를 가상주소로 덮어씀. */
	} else {
		/* We'll use the physical address as the iova to match DPDK. */
		/* [한국어] IOVA=PA 모드: 물리주소를 IOVA로 사용. */
		rc = vtophys_iommu_map_dma_bar((uint64_t)(*mapped_addr), *phys_addr, *size);
		/* [한국어] IOMMU에 (VA → PA=IOVA) 매핑 등록. */
		if (rc) {
			/* [한국어] 매핑 실패 → BAR unmap 후 EFAULT. */
			dev->unmap_bar(dev, bar, *mapped_addr);
			return -EFAULT;
		}
	}
#endif
	return rc;
	/* [한국어] 성공(0). */
}

/*
 * [한국어]
 * spdk_pci_device_unmap_bar - BAR의 IOMMU 매핑 해제 후 BAR unmap
 *
 * @dev:  대상 디바이스.
 * @bar:  BAR 인덱스.
 * @addr: spdk_pci_device_map_bar가 반환했던 매핑 가상주소.
 * @return: 성공 0, IOMMU unmap 실패 -EFAULT.
 *
 * map_bar의 역연산. VFIO+IOMMU가 켜져 있으면 먼저 IOMMU 매핑을 풀고, 그 다음 dev->unmap_bar
 * (rte는 no-op)를 호출한다.
 * 실행 컨텍스트: 메인 스레드(드라이버 fini).
 */
int
spdk_pci_device_unmap_bar(struct spdk_pci_device *dev, uint32_t bar, void *addr)
{
#if VFIO_ENABLED
	int rc;
	/* [한국어] iommu_unmap 반환값. */

	if (spdk_iommu_is_enabled()) {
		/* [한국어] IOMMU가 켜져 있으면 map_bar에서 등록한 IOVA 매핑을 해제. */
		rc = vtophys_iommu_unmap_dma_bar((uint64_t)addr);
		if (rc) {
			/* [한국어] IOMMU unmap 실패 → EFAULT. */
			return -EFAULT;
		}
	}
#endif

	return dev->unmap_bar(dev, bar, addr);
	/* [한국어] BAR unmap(rte 구현은 DPDK 위임 no-op). */
}

/*
 * [한국어]
 * spdk_pci_device_enable_interrupt - 디바이스 인터럽트(레거시/단일) 활성화 어댑터
 *
 * @dev: 대상 디바이스.
 * @return: DPDK 반환값(성공 0).
 *
 * polled-mode가 기본이지만, interrupt 모드 poll group(예: 저전력 대기)을 쓸 때 디바이스
 * 인터럽트를 켠다. 단순히 DPDK 어댑터로 위임. 실행 컨텍스트: 메인 스레드.
 */
int
spdk_pci_device_enable_interrupt(struct spdk_pci_device *dev)
{
	/* [한국어] DPDK rte_pci 인터럽트 enable로 위임. */
	return dpdk_pci_device_enable_interrupt(dev->dev_handle);
}

/*
 * [한국어]
 * spdk_pci_device_disable_interrupt - 디바이스 인터럽트 비활성화 어댑터
 *
 * @dev: 대상 디바이스.
 * @return: DPDK 반환값.
 *
 * enable_interrupt의 역연산. 실행 컨텍스트: 메인 스레드.
 */
int
spdk_pci_device_disable_interrupt(struct spdk_pci_device *dev)
{
	/* [한국어] DPDK 인터럽트 disable로 위임. */
	return dpdk_pci_device_disable_interrupt(dev->dev_handle);
}

/*
 * [한국어]
 * spdk_pci_device_get_interrupt_efd - 디바이스의 인터럽트 eventfd 반환
 *
 * @dev: 대상 디바이스.
 * @return: eventfd 파일 디스크립터(epoll에 등록해 인터럽트 대기에 사용).
 *
 * interrupt 모드에서 reactor가 이 efd를 epoll로 감시하여 디바이스 인터럽트를 대기한다.
 * 실행 컨텍스트: 메인 스레드(설정 시점).
 */
int
spdk_pci_device_get_interrupt_efd(struct spdk_pci_device *dev)
{
	/* [한국어] DPDK가 디바이스에 바인딩한 인터럽트 eventfd 반환. */
	return dpdk_pci_device_get_interrupt_efd(dev->dev_handle);
}

/*
 * [한국어]
 * spdk_pci_device_enable_interrupts - MSI-X 다중 인터럽트 벡터 활성화
 *
 * @dev:       대상 디바이스(VFIO 바인딩 + MSI-X capability 필요).
 * @efd_count: 생성할 eventfd 개수 = 사용할 MSI-X 벡터 수.
 * @return:    성공 0, 인자 오류 -EINVAL, MSI-X 미지원 -ENOTSUP, 그 외 DPDK 오류.
 *
 * NVMe I/O qpair마다 별도 인터럽트 벡터를 쓰는 interrupt 모드에서, 여러 MSI-X 벡터를 한 번에
 * 활성화한다. 단계: efd_count 검증 → MSI-X capability 확인 → eventfd 배열 생성 →
 * 각 efd를 각 인터럽트 벡터에 바인딩(enable). 바인딩 실패 시 만든 eventfd를 정리(롤백).
 * MSI-X는 PCIe capability로, 디바이스당 다수의 인터럽트 벡터를 메시지 기반으로 제공한다.
 * 실행 컨텍스트: 메인 스레드(qpair 생성 시).
 *
 * 호출 체인: NVMe interrupt 모드 설정 → [본 함수] → dpdk_pci_device_create_interrupt_efds /
 *           dpdk_pci_device_enable_interrupt
 */
int
spdk_pci_device_enable_interrupts(struct spdk_pci_device *dev, uint32_t efd_count)
{
	struct rte_pci_device *rte_dev = dev->dev_handle;
	/* [한국어] DPDK 핸들. */
	int rc;
	/* [한국어] 단계별 반환값. */

	if (efd_count == 0) {
		/* [한국어] 0개 벡터 요청은 무의미 → 인자 오류. */
		SPDK_ERRLOG("Invalid efd_count (%u)\n", efd_count);
		return -EINVAL;
	}

	/* Detect if device has MSI-X capability */
	if (dpdk_pci_device_interrupt_cap_multi(rte_dev) != 1) {
		/* [한국어] 다중 인터럽트(MSI-X) capability가 없으면 지원 불가. */
		SPDK_ERRLOG("VFIO MSI-X capability not present for device %s\n",
			    dpdk_pci_device_get_name(rte_dev));
		return -ENOTSUP;
	}

	/* Create event file descriptors */
	rc = dpdk_pci_device_create_interrupt_efds(rte_dev, efd_count);
	/* [한국어] efd_count개의 eventfd 생성(각 벡터당 1개). */
	if (rc) {
		/* [한국어] eventfd 생성 실패 → 전파. */
		SPDK_ERRLOG("Can't setup eventfd (%u)\n", efd_count);
		return rc;
	}

	/* Bind each event fd to each interrupt vector */
	rc = dpdk_pci_device_enable_interrupt(rte_dev);
	/* [한국어] 각 eventfd를 각 MSI-X 벡터에 바인딩하고 인터럽트 enable. */
	if (rc) {
		/* [한국어] 바인딩 실패 → 방금 만든 eventfd 정리(롤백) 후 전파. */
		SPDK_ERRLOG("Failed to enable interrupt for PCI device %s\n",
			    dpdk_pci_device_get_name(rte_dev));
		dpdk_pci_device_delete_interrupt_efds(rte_dev);
		return rc;
	}

	return 0;
	/* [한국어] MSI-X 벡터 활성화 성공. */
}

/*
 * [한국어]
 * spdk_pci_device_disable_interrupts - MSI-X 다중 인터럽트 비활성화 + eventfd 정리
 *
 * @dev: 대상 디바이스.
 * @return: 성공 0, DPDK disable 실패 시 음수.
 *
 * enable_interrupts의 역연산: 인터럽트를 끄고 생성했던 eventfd 배열을 삭제한다.
 * 실행 컨텍스트: 메인 스레드(qpair 해제 시).
 */
int
spdk_pci_device_disable_interrupts(struct spdk_pci_device *dev)
{
	struct rte_pci_device *rte_dev = dev->dev_handle;
	/* [한국어] DPDK 핸들. */
	int rc;
	/* [한국어] disable 반환값. */

	rc = dpdk_pci_device_disable_interrupt(rte_dev);
	/* [한국어] MSI-X 벡터 바인딩 해제 + 인터럽트 disable. */
	if (rc) {
		/* [한국어] disable 실패 → eventfd 정리는 생략하고 오류 전파. */
		SPDK_ERRLOG("Failed to disable interrupt for PCI device %s\n",
			    dpdk_pci_device_get_name(rte_dev));
		return rc;
	}

	dpdk_pci_device_delete_interrupt_efds(rte_dev);
	/* [한국어] enable 시 만든 eventfd 배열 삭제. */

	return 0;
	/* [한국어] 비활성화 성공. */
}

/*
 * [한국어]
 * spdk_pci_device_get_interrupt_efd_by_index - 인덱스로 특정 인터럽트 벡터의 eventfd 반환
 *
 * @dev:   대상 디바이스.
 * @index: 인터럽트 벡터 인덱스(0 = 기본/단일 벡터, 1+ = MSI-X 추가 벡터).
 * @return: 해당 벡터의 eventfd.
 *
 * SPDK 측 index 0은 기본 인터럽트 efd, 1부터는 DPDK의 efd 배열 인덱스 0부터로 매핑된다
 * (오프셋 1 차이). qpair마다 다른 벡터를 epoll로 감시할 때 사용.
 * 실행 컨텍스트: 메인 스레드(설정 시점).
 */
int
spdk_pci_device_get_interrupt_efd_by_index(struct spdk_pci_device *dev, uint32_t index)
{
	if (index == 0) {
		/* [한국어] index 0 = 기본 단일 인터럽트 efd. */
		return dpdk_pci_device_get_interrupt_efd(dev->dev_handle);
	} else {
		/* Note: The interrupt vector offset starts from 1, and in DPDK these
		 * are mapped to efd index 0 onwards.
		 */
		/* [한국어] SPDK index 1 → DPDK efd 배열 0 (오프셋 -1로 변환). */
		return dpdk_pci_device_get_interrupt_efd_by_index(dev->dev_handle, index - 1);
	}
}

/*
 * [한국어]
 * spdk_pci_device_get_domain - 디바이스의 PCI domain(segment) 번호 반환
 *
 * @dev: 대상 디바이스.
 * @return: PCI domain 번호(보통 0, 다중 segment 시스템에서 0 초과).
 *
 * BDF 중 domain 필드 접근자. 실행 컨텍스트: 임의 스레드(읽기 전용).
 */
uint32_t
spdk_pci_device_get_domain(struct spdk_pci_device *dev)
{
	/* [한국어] pci_device_init에서 채운 domain 반환. */
	return dev->addr.domain;
}

/*
 * [한국어]
 * spdk_pci_device_get_bus - 디바이스의 PCI bus 번호 반환
 *
 * @dev: 대상 디바이스.
 * @return: PCI bus 번호(0~255).
 *
 * 실행 컨텍스트: 임의 스레드(읽기 전용).
 */
uint8_t
spdk_pci_device_get_bus(struct spdk_pci_device *dev)
{
	/* [한국어] BDF의 bus 필드 반환. */
	return dev->addr.bus;
}

/*
 * [한국어]
 * spdk_pci_device_get_dev - 디바이스의 PCI device(slot) 번호 반환
 *
 * @dev: 대상 디바이스.
 * @return: PCI device 번호(0~31).
 *
 * 실행 컨텍스트: 임의 스레드(읽기 전용).
 */
uint8_t
spdk_pci_device_get_dev(struct spdk_pci_device *dev)
{
	/* [한국어] BDF의 device 필드 반환. */
	return dev->addr.dev;
}

/*
 * [한국어]
 * spdk_pci_device_get_func - 디바이스의 PCI function 번호 반환
 *
 * @dev: 대상 디바이스.
 * @return: PCI function 번호(0~7).
 *
 * 실행 컨텍스트: 임의 스레드(읽기 전용).
 */
uint8_t
spdk_pci_device_get_func(struct spdk_pci_device *dev)
{
	/* [한국어] BDF의 function 필드 반환. */
	return dev->addr.func;
}

/*
 * [한국어]
 * spdk_pci_device_get_vendor_id - 디바이스의 PCI 벤더 ID 반환
 *
 * @dev: 대상 디바이스.
 * @return: 16비트 벤더 ID(예: 0x8086 = Intel).
 *
 * 실행 컨텍스트: 임의 스레드(읽기 전용).
 */
uint16_t
spdk_pci_device_get_vendor_id(struct spdk_pci_device *dev)
{
	/* [한국어] id.vendor_id 반환. */
	return dev->id.vendor_id;
}

/*
 * [한국어]
 * spdk_pci_device_get_device_id - 디바이스의 PCI 디바이스 ID 반환
 *
 * @dev: 대상 디바이스.
 * @return: 16비트 디바이스 ID.
 *
 * 실행 컨텍스트: 임의 스레드(읽기 전용).
 */
uint16_t
spdk_pci_device_get_device_id(struct spdk_pci_device *dev)
{
	/* [한국어] id.device_id 반환. */
	return dev->id.device_id;
}

/*
 * [한국어]
 * spdk_pci_device_get_subvendor_id - 디바이스의 서브시스템 벤더 ID 반환
 *
 * @dev: 대상 디바이스.
 * @return: 16비트 subsystem vendor ID(quirk 매칭 등에 사용).
 *
 * 실행 컨텍스트: 임의 스레드(읽기 전용).
 */
uint16_t
spdk_pci_device_get_subvendor_id(struct spdk_pci_device *dev)
{
	/* [한국어] id.subvendor_id 반환. */
	return dev->id.subvendor_id;
}

/*
 * [한국어]
 * spdk_pci_device_get_subdevice_id - 디바이스의 서브시스템 디바이스 ID 반환
 *
 * @dev: 대상 디바이스.
 * @return: 16비트 subsystem device ID.
 *
 * 실행 컨텍스트: 임의 스레드(읽기 전용).
 */
uint16_t
spdk_pci_device_get_subdevice_id(struct spdk_pci_device *dev)
{
	/* [한국어] id.subdevice_id 반환. */
	return dev->id.subdevice_id;
}

/*
 * [한국어]
 * spdk_pci_device_get_id - 디바이스의 전체 식별자 구조체 반환
 *
 * @dev: 대상 디바이스.
 * @return: spdk_pci_id 값 복사본(class/vendor/device/subvendor/subdevice).
 *
 * 개별 접근자 대신 한 번에 전체 ID를 받을 때 사용(quirk 테이블 매칭 등).
 * 실행 컨텍스트: 임의 스레드(읽기 전용).
 */
struct spdk_pci_id
spdk_pci_device_get_id(struct spdk_pci_device *dev)
{
	/* [한국어] id 구조체 값 자체를 반환(얕은 복사). */
	return dev->id;
}

/*
 * [한국어]
 * spdk_pci_device_get_numa_id - 디바이스가 붙은 NUMA 노드 ID 반환
 *
 * @dev: 대상 디바이스.
 * @return: NUMA 노드 번호(-1이면 미지정).
 *
 * DMA 버퍼를 디바이스와 같은 NUMA 노드에 할당해 cross-socket 트래픽을 줄이는 데 사용.
 * 실행 컨텍스트: 임의 스레드(읽기 전용).
 */
int
spdk_pci_device_get_numa_id(struct spdk_pci_device *dev)
{
	/* [한국어] pci_device_init이 채운 numa_id 반환. */
	return dev->numa_id;
}

/*
 * [한국어]
 * spdk_pci_device_cfg_read - PCI config space 임의 길이 읽기 공개 API
 *
 * @dev:    대상 디바이스.
 * @value:  출력 버퍼.
 * @len:    읽을 바이트 수.
 * @offset: config space 오프셋.
 * @return: 성공 0, 실패 음수.
 *
 * dev->cfg_read(=cfg_read_rte)로 위임하는 얇은 래퍼. cfg_read8/16/32가 이를 호출한다.
 * 실행 컨텍스트: 메인 스레드(드라이버 init/control).
 */
int
spdk_pci_device_cfg_read(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	/* [한국어] 함수 포인터로 DPDK config read 어댑터 호출. */
	return dev->cfg_read(dev, value, len, offset);
}

/*
 * [한국어]
 * spdk_pci_device_cfg_write - PCI config space 임의 길이 쓰기 공개 API
 *
 * @dev:    대상 디바이스.
 * @value:  쓸 데이터 버퍼.
 * @len:    쓸 바이트 수.
 * @offset: config space 오프셋.
 * @return: 성공 0, 실패 음수.
 *
 * dev->cfg_write(=cfg_write_rte)로 위임. Bus Master Enable, MSI-X enable 비트 설정 등에 사용.
 * 실행 컨텍스트: 메인 스레드.
 */
int
spdk_pci_device_cfg_write(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	/* [한국어] 함수 포인터로 DPDK config write 어댑터 호출. */
	return dev->cfg_write(dev, value, len, offset);
}

/*
 * [한국어]
 * spdk_pci_device_cfg_read8 - config space에서 1바이트 읽기
 *
 * @dev:    대상 디바이스.
 * @value:  출력(uint8).
 * @offset: config space 오프셋.
 * @return: 성공 0, 실패 음수.
 *
 * len=1로 고정한 cfg_read 래퍼. 실행 컨텍스트: 메인 스레드.
 */
int
spdk_pci_device_cfg_read8(struct spdk_pci_device *dev, uint8_t *value, uint32_t offset)
{
	/* [한국어] 길이 1바이트로 config read 위임. */
	return spdk_pci_device_cfg_read(dev, value, 1, offset);
}

/*
 * [한국어]
 * spdk_pci_device_cfg_write8 - config space에 1바이트 쓰기
 *
 * @dev:    대상 디바이스.
 * @value:  쓸 값(uint8, 값 전달).
 * @offset: config space 오프셋.
 * @return: 성공 0, 실패 음수.
 *
 * 값 전달 인자의 주소를 넘겨 len=1로 cfg_write 위임. 실행 컨텍스트: 메인 스레드.
 */
int
spdk_pci_device_cfg_write8(struct spdk_pci_device *dev, uint8_t value, uint32_t offset)
{
	/* [한국어] 지역 변수 value의 주소를 넘겨 1바이트 write. */
	return spdk_pci_device_cfg_write(dev, &value, 1, offset);
}

/*
 * [한국어]
 * spdk_pci_device_cfg_read16 - config space에서 2바이트 읽기
 *
 * @dev:    대상 디바이스.
 * @value:  출력(uint16).
 * @offset: config space 오프셋.
 * @return: 성공 0, 실패 음수.
 *
 * len=2로 고정한 cfg_read 래퍼. 실행 컨텍스트: 메인 스레드.
 */
int
spdk_pci_device_cfg_read16(struct spdk_pci_device *dev, uint16_t *value, uint32_t offset)
{
	/* [한국어] 길이 2바이트로 config read 위임. */
	return spdk_pci_device_cfg_read(dev, value, 2, offset);
}

/*
 * [한국어]
 * spdk_pci_device_cfg_write16 - config space에 2바이트 쓰기
 *
 * @dev:    대상 디바이스.
 * @value:  쓸 값(uint16, 값 전달).
 * @offset: config space 오프셋.
 * @return: 성공 0, 실패 음수.
 *
 * len=2로 cfg_write 위임. 실행 컨텍스트: 메인 스레드.
 */
int
spdk_pci_device_cfg_write16(struct spdk_pci_device *dev, uint16_t value, uint32_t offset)
{
	/* [한국어] 지역 변수 주소를 넘겨 2바이트 write. */
	return spdk_pci_device_cfg_write(dev, &value, 2, offset);
}

/*
 * [한국어]
 * spdk_pci_device_cfg_read32 - config space에서 4바이트 읽기
 *
 * @dev:    대상 디바이스.
 * @value:  출력(uint32).
 * @offset: config space 오프셋.
 * @return: 성공 0, 실패 음수.
 *
 * len=4로 고정한 cfg_read 래퍼. SN capability 탐색 등에 사용. 실행 컨텍스트: 메인 스레드.
 */
int
spdk_pci_device_cfg_read32(struct spdk_pci_device *dev, uint32_t *value, uint32_t offset)
{
	/* [한국어] 길이 4바이트로 config read 위임. */
	return spdk_pci_device_cfg_read(dev, value, 4, offset);
}

/*
 * [한국어]
 * spdk_pci_device_cfg_write32 - config space에 4바이트 쓰기
 *
 * @dev:    대상 디바이스.
 * @value:  쓸 값(uint32, 값 전달).
 * @offset: config space 오프셋.
 * @return: 성공 0, 실패 음수.
 *
 * len=4로 cfg_write 위임. 실행 컨텍스트: 메인 스레드.
 */
int
spdk_pci_device_cfg_write32(struct spdk_pci_device *dev, uint32_t value, uint32_t offset)
{
	/* [한국어] 지역 변수 주소를 넘겨 4바이트 write. */
	return spdk_pci_device_cfg_write(dev, &value, 4, offset);
}

/*
 * [한국어]
 * spdk_pci_device_get_serial_number - PCIe Extended Capability에서 Device Serial Number 추출
 *
 * @dev: 대상 디바이스.
 * @sn:  출력 버퍼(최소 17바이트 — 16 hex + NUL).
 * @len: sn 버퍼 크기.
 * @return: 성공 0, SN capability 미존재/오류/버퍼 부족 시 -1.
 *
 * PCIe Extended Configuration Space(0x100~0xFFF)의 capability 연결 리스트를 따라가며
 * ID가 PCI_EXT_CAP_ID_SN(0x03)인 항목을 찾는다. 각 ext cap 헤더는 [15:0]=cap ID,
 * [31:20]=다음 cap 오프셋 형식이다. SN을 찾으면 헤더 다음 8바이트(64비트 시리얼)를 두 번의
 * 32비트 read로 읽어 "%08x%08x"(상위 후 하위 워드)로 문자열화한다.
 * 실행 컨텍스트: 메인 스레드. 호출자: 디바이스 식별/로깅 모듈.
 *
 * 호출 체인: 상위 모듈 → [본 함수] → spdk_pci_device_cfg_read32 (반복)
 */
int
spdk_pci_device_get_serial_number(struct spdk_pci_device *dev, char *sn, size_t len)
{
	int err;
	/* [한국어] 각 config read의 오류 코드. */
	uint32_t pos, header = 0;
	/* [한국어] pos = 현재 capability 오프셋, header = 그 위치의 cap 헤더 dword. */
	uint32_t i, buf[2];
	/* [한국어] i = SN 워드 인덱스, buf = 64비트 시리얼의 두 32비트 워드. */

	if (len < 17) {
		/* [한국어] 16 hex 문자 + NUL = 17 미만이면 담을 수 없음. */
		return -1;
	}

	err = spdk_pci_device_cfg_read32(dev, &header, PCI_CFG_SIZE);
	/* [한국어] 확장 cap 리스트 시작(0x100)에서 첫 헤더 읽기. */
	if (err || !header) {
		/* [한국어] read 실패 또는 헤더가 0(확장 cap 없음) → SN 없음. */
		return -1;
	}

	pos = PCI_CFG_SIZE;
	/* [한국어] 현재 탐색 위치 = 0x100. */
	while (1) {
		/* [한국어] capability 연결 리스트를 끝까지 순회. */
		if ((header & 0x0000ffff) == PCI_EXT_CAP_ID_SN) {
			/* [한국어] 헤더 하위 16비트(cap ID)가 SN(0x03)이면 발견. */
			if (pos) {
				/* skip the header */
				pos += 4;
				/* [한국어] 4바이트 헤더를 건너뛰어 SN 데이터 시작으로 이동. */
				for (i = 0; i < 2; i++) {
					/* [한국어] 64비트 시리얼을 32비트 두 번에 나눠 읽기. */
					err = spdk_pci_device_cfg_read32(dev, &buf[i], pos + 4 * i);
					if (err) {
						/* [한국어] 시리얼 워드 read 실패 → 실패. */
						return -1;
					}
				}
				snprintf(sn, len, "%08x%08x", buf[1], buf[0]);
				/* [한국어] 상위 워드(buf[1]) + 하위 워드(buf[0]) 순으로 16자리 hex 문자열 생성. */
				return 0;
				/* [한국어] SN 추출 성공. */
			}
		}
		pos = (header >> 20) & 0xffc;
		/* [한국어] 헤더 [31:20] = 다음 cap 오프셋(4바이트 정렬 → &0xffc). */
		/* 0 if no other items exist */
		if (pos < PCI_CFG_SIZE) {
			/* [한국어] 다음 오프셋이 0x100 미만(=0 또는 표준 영역)이면 리스트 종료 → SN 없음. */
			return -1;
		}
		err = spdk_pci_device_cfg_read32(dev, &header, pos);
		/* [한국어] 다음 capability 헤더 읽기. */
		if (err) {
			/* [한국어] read 실패 → 실패. */
			return -1;
		}
	}
	return -1;
	/* [한국어] (도달 불가) 안전망. */
}

/*
 * [한국어]
 * spdk_pci_device_get_addr - 디바이스의 BDF 주소 구조체 반환
 *
 * @dev: 대상 디바이스.
 * @return: spdk_pci_addr 값 복사본(domain/bus/dev/func).
 *
 * 실행 컨텍스트: 임의 스레드(읽기 전용).
 */
struct spdk_pci_addr
spdk_pci_device_get_addr(struct spdk_pci_device *dev)
{
	/* [한국어] addr 구조체 값 자체를 반환(얕은 복사). */
	return dev->addr;
}

/*
 * [한국어]
 * spdk_pci_device_is_removed - 디바이스가 제거 진행 중인지 조회
 *
 * @dev: 대상 디바이스.
 * @return: true면 제거 예약됨(pending_removal), false면 정상.
 *
 * surprise-removal 후 상위 모듈이 I/O 발행 전에 디바이스 생존을 확인할 때 사용.
 * 실행 컨텍스트: 임의 스레드(단일 bool 읽기, 락 없이 best-effort).
 */
bool
spdk_pci_device_is_removed(struct spdk_pci_device *dev)
{
	/* [한국어] hotremove 통지/detach가 세운 pending_removal 플래그 반환. */
	return dev->internal.pending_removal;
}

/*
 * [한국어]
 * spdk_pci_addr_compare - 두 PCI 주소를 사전식(domain→bus→dev→func)으로 비교
 *
 * @a1: 첫 번째 BDF.
 * @a2: 두 번째 BDF.
 * @return: a1>a2면 1, a1<a2면 -1, 동일하면 0.
 *
 * attach/enumerate에서 디바이스를 BDF로 식별/정렬할 때 사용. 우선순위가 높은 필드부터
 * 차례로 비교하여 첫 차이에서 결정한다.
 * 실행 컨텍스트: 임의 스레드(순수 비교, 상태 없음).
 *
 * 호출 체인: spdk_pci_device_attach 등 → [spdk_pci_addr_compare]
 */
int
spdk_pci_addr_compare(const struct spdk_pci_addr *a1, const struct spdk_pci_addr *a2)
{
	if (a1->domain > a2->domain) {
		/* [한국어] domain 우선 비교. */
		return 1;
	} else if (a1->domain < a2->domain) {
		return -1;
	} else if (a1->bus > a2->bus) {
		/* [한국어] domain 동일 → bus 비교. */
		return 1;
	} else if (a1->bus < a2->bus) {
		return -1;
	} else if (a1->dev > a2->dev) {
		/* [한국어] bus 동일 → device 비교. */
		return 1;
	} else if (a1->dev < a2->dev) {
		return -1;
	} else if (a1->func > a2->func) {
		/* [한국어] device 동일 → function 비교. */
		return 1;
	} else if (a1->func < a2->func) {
		return -1;
	}

	return 0;
	/* [한국어] 모든 필드 동일 → 같은 디바이스. */
}

#ifdef __linux__
/*
 * [한국어]
 * spdk_pci_device_claim - 다중 프로세스 동시 attach 차단(flock 기반 claim)
 *
 * @dev: claim할 디바이스.
 * @return: 성공 0, 이미 다른 프로세스가 점유 중 -EACCES, 그 외 -errno.
 *
 * SPDK는 여러 프로세스가 같은 NVMe를 동시에 잡으면 위험하므로, /var/tmp에 BDF별 lock 파일을
 * 만들고 advisory write-lock(F_SETLK)을 건다. 락에 성공하면 파일 본문에 자기 PID를 기록해두어,
 * 다른 프로세스가 락 실패 시 누가 잡았는지 알 수 있게 한다. 파일은 mmap으로 PID를 공유한다.
 * 락을 유지하려면 fd를 열어둔 채로 두며(close하면 락 해제), unclaim에서 닫는다.
 * 실행 컨텍스트: 메인 스레드(attach 시). Linux 전용(다른 OS는 stub).
 *
 * 호출 체인: 드라이버 → [spdk_pci_device_claim] → open/ftruncate/mmap/fcntl(F_SETLK)
 */
int
spdk_pci_device_claim(struct spdk_pci_device *dev)
{
	int dev_fd;
	/* [한국어] lock 파일 디스크립터(락 유지를 위해 계속 open 상태로 둠). */
	char dev_name[64];
	/* [한국어] lock 파일 경로(BDF 포함). */
	int pid;
	/* [한국어] 락 실패 시 읽어들인 점유 프로세스 PID(진단용). */
	void *dev_map;
	/* [한국어] PID를 공유하기 위해 lock 파일을 mmap한 주소. */
	struct flock pcidev_lock = {
		/* [한국어] fcntl advisory lock 명세. */
		.l_type = F_WRLCK,
		/* [한국어] write 락(배타적) — 한 프로세스만 점유 가능. */
		.l_whence = SEEK_SET,
		/* [한국어] 오프셋 기준 = 파일 시작. */
		.l_start = 0,
		/* [한국어] 락 시작 오프셋 0. */
		.l_len = 0,
		/* [한국어] len 0 = 파일 전체(EOF까지) 잠금. */
	};

	snprintf(dev_name, sizeof(dev_name), "/var/tmp/spdk_pci_lock_%04x:%02x:%02x.%x",
		 dev->addr.domain, dev->addr.bus, dev->addr.dev, dev->addr.func);
	/* [한국어] BDF별 고유 lock 파일 경로 생성 → 디바이스 단위 배타성 보장. */

	dev_fd = open(dev_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	/* [한국어] lock 파일 열기/생성(소유자 RW 권한). 시스템콜 open. */
	if (dev_fd == -1) {
		/* [한국어] 파일 열기 실패(권한/경로) → errno 반환. */
		SPDK_ERRLOG("could not open %s\n", dev_name);
		return -errno;
	}

	if (ftruncate(dev_fd, sizeof(int)) != 0) {
		/* [한국어] 파일 크기를 int(PID 저장용)만큼 확보. 실패 시 fd 닫고 반환. */
		SPDK_ERRLOG("could not truncate %s\n", dev_name);
		close(dev_fd);
		return -errno;
	}

	dev_map = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
		       MAP_SHARED, dev_fd, 0);
	/* [한국어] PID 슬롯을 MAP_SHARED로 매핑 → 모든 프로세스가 같은 PID 값을 본다. */
	if (dev_map == MAP_FAILED) {
		/* [한국어] mmap 실패 → fd 닫고 반환. */
		SPDK_ERRLOG("could not mmap dev %s (%d)\n", dev_name, errno);
		close(dev_fd);
		return -errno;
	}

	if (fcntl(dev_fd, F_SETLK, &pcidev_lock) != 0) {
		/* [한국어] 배타적 write 락 시도. 실패 = 다른 프로세스가 이미 점유. */
		pid = *(int *)dev_map;
		/* [한국어] 공유 매핑에서 점유 프로세스 PID 읽어 진단 로그에 표시. */
		SPDK_ERRLOG("Cannot create lock on device %s, probably"
			    " process %d has claimed it\n", dev_name, pid);
		munmap(dev_map, sizeof(int));
		/* [한국어] 매핑 해제. */
		close(dev_fd);
		/* [한국어] fd 닫기(락을 못 잡았으므로 유지 불필요). */
		/* F_SETLK returns unspecified errnos, normalize them */
		return -EACCES;
		/* [한국어] F_SETLK errno가 일정치 않아 -EACCES로 정규화. */
	}

	*(int *)dev_map = (int)getpid();
	/* [한국어] 락 성공 → 공유 슬롯에 자기 PID 기록(다른 프로세스 진단용). */
	munmap(dev_map, sizeof(int));
	/* [한국어] PID는 파일에 영속됐으므로 매핑은 해제(락은 fd가 유지). */
	dev->internal.claim_fd = dev_fd;
	/* [한국어] 락 유지를 위해 fd를 디바이스에 보관(unclaim/detach에서 close). */
	/* Keep dev_fd open to maintain the lock. */
	return 0;
	/* [한국어] claim 성공. */
}

/*
 * [한국어]
 * spdk_pci_device_unclaim - claim 해제: lock fd 닫고 lock 파일 제거
 *
 * @dev: unclaim할 디바이스(claim_fd >= 0이어야 의미 있음).
 *
 * claim의 역연산. fd를 닫으면 advisory 락이 자동 해제되고, lock 파일도 unlink한다.
 * 실행 컨텍스트: 메인 스레드(detach/명시적 unclaim 시). Linux 전용.
 *
 * 호출 체인: spdk_pci_device_detach / 사용자 → [spdk_pci_device_unclaim] → close/unlink
 */
void
spdk_pci_device_unclaim(struct spdk_pci_device *dev)
{
	char dev_name[64];
	/* [한국어] unlink할 lock 파일 경로. */

	snprintf(dev_name, sizeof(dev_name), "/var/tmp/spdk_pci_lock_%04x:%02x:%02x.%x",
		 dev->addr.domain, dev->addr.bus, dev->addr.dev, dev->addr.func);
	/* [한국어] claim과 동일한 규칙으로 경로 재구성. */

	close(dev->internal.claim_fd);
	/* [한국어] fd 닫기 → advisory write 락 자동 해제. */
	dev->internal.claim_fd = -1;
	/* [한국어] claim 안 함 상태로 표시. */
	unlink(dev_name);
	/* [한국어] lock 파일 삭제(다음 claim이 깨끗한 상태에서 시작). */
}
#else /* !__linux__ */
/*
 * [한국어]
 * spdk_pci_device_claim - (비-Linux) claim 스텁
 *
 * @dev: 무시됨.
 * @return: 항상 0(성공).
 *
 * Linux 외 플랫폼에는 /var/tmp flock 기반 claim이 아직 구현되지 않음(TODO).
 * 실행 컨텍스트: 메인 스레드.
 */
int
spdk_pci_device_claim(struct spdk_pci_device *dev)
{
	/* TODO */
	/* [한국어] 비-Linux: 미구현, 항상 성공 처리. */
	return 0;
}

/*
 * [한국어]
 * spdk_pci_device_unclaim - (비-Linux) unclaim 스텁
 *
 * @dev: 무시됨.
 *
 * Linux 외 플랫폼 미구현(TODO). 실행 컨텍스트: 메인 스레드.
 */
void
spdk_pci_device_unclaim(struct spdk_pci_device *dev)
{
	/* TODO */
	/* [한국어] 비-Linux: 미구현 no-op. */
}
#endif /* __linux__ */

/*
 * [한국어]
 * spdk_pci_addr_parse - "BDF" 문자열을 spdk_pci_addr로 파싱(여러 형식 허용)
 *
 * @addr: 출력 BDF 구조체.
 * @bdf:  파싱할 문자열. 다양한 축약 형식 허용:
 *        "dddd:bb:dd.f"(full), "bb:dd.f"(domain 생략), "bb:dd"(domain·func 생략) 등.
 * @return: 성공 0, NULL 인자/형식 불일치/범위 초과 시 -EINVAL.
 *
 * 사용자/RPC가 준 BDF 문자열을 정규 구조체로 변환한다. ':' 와 '.' 구분자를 모두 받아들이고,
 * 생략된 domain/func은 0으로 채운다. 마지막에 bus≤0xFF, dev≤0x1F, func≤7 범위를 검증한다.
 * 실행 컨텍스트: 임의 스레드(순수 함수).
 *
 * 호출 체인: RPC/CLI → [spdk_pci_addr_parse] → sscanf
 */
int
spdk_pci_addr_parse(struct spdk_pci_addr *addr, const char *bdf)
{
	unsigned domain, bus, dev, func;
	/* [한국어] sscanf로 추출할 임시 필드들(생략 형식은 0으로 보정). */

	if (addr == NULL || bdf == NULL) {
		/* [한국어] NULL 인자 방어. */
		return -EINVAL;
	}

	if ((sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &dev, &func) == 4) ||
	    (sscanf(bdf, "%x.%x.%x.%x", &domain, &bus, &dev, &func) == 4)) {
		/* Matched a full address - all variables are initialized */
		/* [한국어] full BDF(domain:bus:dev.func) 매칭 — 모든 필드 채워짐. */
	} else if (sscanf(bdf, "%x:%x:%x", &domain, &bus, &dev) == 3) {
		/* [한국어] func 생략 형식 → func=0. */
		func = 0;
	} else if ((sscanf(bdf, "%x:%x.%x", &bus, &dev, &func) == 3) ||
		   (sscanf(bdf, "%x.%x.%x", &bus, &dev, &func) == 3)) {
		/* [한국어] domain 생략 형식 → domain=0. */
		domain = 0;
	} else if ((sscanf(bdf, "%x:%x", &bus, &dev) == 2) ||
		   (sscanf(bdf, "%x.%x", &bus, &dev) == 2)) {
		/* [한국어] domain·func 모두 생략 → 둘 다 0. */
		domain = 0;
		func = 0;
	} else {
		/* [한국어] 어떤 형식과도 불일치 → 잘못된 입력. */
		return -EINVAL;
	}

	if (bus > 0xFF || dev > 0x1F || func > 7) {
		/* [한국어] PCI 스펙 범위 검증: bus 8비트, dev 5비트, func 3비트. */
		return -EINVAL;
	}

	addr->domain = domain;
	/* [한국어] 검증된 domain 저장. */
	addr->bus = bus;
	/* [한국어] bus 저장. */
	addr->dev = dev;
	/* [한국어] device 저장. */
	addr->func = func;
	/* [한국어] function 저장. */

	return 0;
	/* [한국어] 파싱 성공. */
}

/*
 * [한국어]
 * spdk_pci_addr_fmt - spdk_pci_addr를 정규 "dddd:bb:dd.f" 문자열로 포맷
 *
 * @bdf:  출력 버퍼.
 * @sz:   버퍼 크기.
 * @addr: 포맷할 BDF 구조체.
 * @return: 성공 0, 버퍼 부족/오류 -1.
 *
 * addr_parse의 역연산. domain은 4자리, bus/dev는 2자리 hex로 zero-pad한 표준 표기를 만든다.
 * 실행 컨텍스트: 임의 스레드(순수 함수).
 *
 * 호출 체인: 로깅/attach 등 → [spdk_pci_addr_fmt] → snprintf
 */
int
spdk_pci_addr_fmt(char *bdf, size_t sz, const struct spdk_pci_addr *addr)
{
	int rc;
	/* [한국어] snprintf가 쓴(또는 썼을) 문자 수. */

	rc = snprintf(bdf, sz, "%04x:%02x:%02x.%x",
		      addr->domain, addr->bus,
		      addr->dev, addr->func);
	/* [한국어] 표준 BDF 표기(domain 4자리, bus/dev 2자리 zero-pad). */

	if (rc > 0 && (size_t)rc < sz) {
		/* [한국어] 잘림 없이 정상 기록됨 → 성공. */
		return 0;
	}

	return -1;
	/* [한국어] 버퍼 부족 또는 오류. */
}

/*
 * [한국어]
 * spdk_pci_hook_device - 외부에서 만든 가짜/가상 PCI 디바이스를 SPDK에 등록(hook)
 *
 * @drv: 이 디바이스를 소유할 드라이버.
 * @dev: 호출자가 직접 map_bar/cfg_read 등 함수 포인터를 채운 디바이스 객체.
 * @return: 성공 0, 드라이버 콜백 거부 시 -ECANCELED.
 *
 * DPDK probe를 거치지 않고(예: 시뮬레이터, vfio-user, 사용자 정의 백엔드) 디바이스를 SPDK
 * 추적 리스트에 직접 끼워 넣는 진입점이다. 호출자는 미리 map_bar/unmap_bar/cfg_read/cfg_write
 * 4개 함수 포인터를 채워야 한다(assert로 강제). 드라이버 콜백이 있으면 호출해 상위 모듈이
 * 디바이스를 인식하게 하고, attach 표시 후 g_pci_devices에 추가한다.
 * 실행 컨텍스트: 메인 스레드.
 *
 * 호출 체인: 사용자 백엔드 → [spdk_pci_hook_device] → drv->cb_fn → g_pci_devices 삽입
 */
int
spdk_pci_hook_device(struct spdk_pci_driver *drv, struct spdk_pci_device *dev)
{
	int rc;
	/* [한국어] 드라이버 콜백 반환값. */

	assert(dev->map_bar != NULL);
	/* [한국어] 호출자가 BAR 매핑 구현을 제공했는지 강제. */
	assert(dev->unmap_bar != NULL);
	/* [한국어] BAR unmap 구현 강제. */
	assert(dev->cfg_read != NULL);
	/* [한국어] config read 구현 강제. */
	assert(dev->cfg_write != NULL);
	/* [한국어] config write 구현 강제. */
	dev->internal.driver = drv;
	/* [한국어] 소유 드라이버 기록(enumerate 매칭 등에 사용). */

	if (drv->cb_fn != NULL) {
		/* [한국어] 드라이버에 임시 게시된 콜백이 있으면 상위 모듈에 디바이스 알림. */
		rc = drv->cb_fn(drv->cb_arg, dev);
		if (rc != 0) {
			/* [한국어] 상위 모듈 거부 → hook 취소. 호출자가 객체를 회수. */
			return -ECANCELED;
		}

		dev->internal.attached = true;
		/* [한국어] 콜백 성공 → attach 표시. */
	}

	TAILQ_INSERT_TAIL(&g_pci_devices, dev, internal.tailq);
	/* [한국어] 정식 추적 리스트에 직접 추가(hotplugged 큐 경유 없음). */

	return 0;
	/* [한국어] hook 성공. */
}

/*
 * [한국어]
 * spdk_pci_unhook_device - hook으로 등록했던 디바이스를 추적 리스트에서 제거
 *
 * @dev: 제거할 디바이스(반드시 detach 상태 — attached=false).
 *
 * spdk_pci_hook_device의 역연산. 객체 메모리 해제는 호출자 책임(SPDK는 free하지 않음).
 * 실행 컨텍스트: 메인 스레드.
 *
 * 호출 체인: 사용자 백엔드 → [spdk_pci_unhook_device] → g_pci_devices 제거
 */
void
spdk_pci_unhook_device(struct spdk_pci_device *dev)
{
	assert(!dev->internal.attached);
	/* [한국어] 아직 attach 중인 디바이스를 unhook하면 상위 모듈이 dangling 참조 → 금지. */
	TAILQ_REMOVE(&g_pci_devices, dev, internal.tailq);
	/* [한국어] 추적 리스트에서 분리(객체 free는 호출자가 수행). */
}

/*
 * [한국어]
 * spdk_pci_register_device_provider - 새 attach 제공자(provider)를 등록
 *
 * @provider: name/attach_cb/detach_cb를 채운 provider 객체(정적 수명이어야 함).
 *
 * 기본 "pci"(rte) 외에 "vmd" 같은 가상 버스를 통해 디바이스를 attach/detach하는 백엔드를
 * 추가한다. spdk_pci_device_attach가 provider 리스트를 순회하며 시도하므로, 등록 순서가
 * 시도 순서가 된다. SPDK_PCI_REGISTER_DEVICE_PROVIDER 매크로가 constructor에서 호출.
 * 실행 컨텍스트: constructor 또는 메인 스레드.
 *
 * 호출 체인: SPDK_PCI_REGISTER_DEVICE_PROVIDER → [본 함수] → g_pci_device_providers 삽입
 */
void
spdk_pci_register_device_provider(struct spdk_pci_device_provider *provider)
{
	/* [한국어] provider 리스트 끝에 추가(시도 우선순위 = 등록 순). */
	TAILQ_INSERT_TAIL(&g_pci_device_providers, provider, tailq);
}

/*
 * [한국어]
 * spdk_pci_device_get_type - 디바이스의 provider 타입 문자열 반환
 *
 * @dev: 대상 디바이스.
 * @return: "pci"/"vmd" 등 타입 문자열.
 *
 * detach 시 어떤 provider의 detach_cb를 쓸지 결정하는 키. 실행 컨텍스트: 임의 스레드(읽기 전용).
 */
const char *
spdk_pci_device_get_type(const struct spdk_pci_device *dev)
{
	/* [한국어] pci_device_init/hook이 설정한 type 문자열 반환. */
	return dev->type;
}

/*
 * [한국어]
 * spdk_pci_device_allow - 특정 BDF를 DPDK allowlist에 추가(차단 해제)
 *
 * @pci_addr: 허용할 디바이스 BDF.
 * @return: 성공 0, 할당/파싱/insert 실패 시 음수 errno.
 *
 * 사용자가 명시적으로 "이 디바이스는 SPDK가 잡아도 된다"고 지정할 때 사용. "pci:<BDF>" 인자를
 * 만들어 rte_devargs로 파싱하고 policy=RTE_DEV_ALLOWED로 설정한 뒤 DPDK 글로벌 리스트에 insert한다.
 * 동일 주소 devargs가 이미 있으면 DPDK가 덮어쓰므로 중복 체크 불필요하며, insert 후 메모리는
 * DPDK가 관리한다(SPDK가 추적할 필요 없음).
 * 실행 컨텍스트: 메인 스레드(설정 단계).
 *
 * 호출 체인: 사용자/RPC → [spdk_pci_device_allow] → rte_devargs_parse/insert
 */
int
spdk_pci_device_allow(struct spdk_pci_addr *pci_addr)
{
	struct rte_devargs *da;
	/* [한국어] 새로 만들 DPDK devargs(allowlist 항목). */
	char devargs_str[128];
	/* [한국어] "pci:<BDF>" 인자 문자열. */

	da = calloc(1, sizeof(*da));
	/* [한국어] devargs 할당(성공 시 DPDK가 소유권 인수). */
	if (da == NULL) {
		/* [한국어] OOM. */
		SPDK_ERRLOG("could not allocate rte_devargs\n");
		return -ENOMEM;
	}

	snprintf(devargs_str, sizeof(devargs_str), "pci:%04x:%02x:%02x.%x",
		 pci_addr->domain, pci_addr->bus, pci_addr->dev, pci_addr->func);
	/* [한국어] BDF를 "pci:dddd:bb:dd.f" 형식 인자로 구성. */
	if (rte_devargs_parse(da, devargs_str) != 0) {
		/* [한국어] 파싱 실패 → da 해제 후 EINVAL. */
		SPDK_ERRLOG("rte_devargs_parse() failed on '%s'\n", devargs_str);
		free(da);
		return -EINVAL;
	}
	da->policy = RTE_DEV_ALLOWED;
	/* [한국어] 정책을 명시적 허용으로 설정 → 스캔/probe 시 이 디바이스를 잡을 수 있게 됨. */
	/* Note: if a devargs already exists for this device address, it just gets
	 * overridden.  So we do not need to check if the devargs already exists.
	 * DPDK will take care of memory management for the devargs structure after
	 * it has been inserted, so there's nothing SPDK needs to track.
	 */
	if (rte_devargs_insert(&da) != 0) {
		/* [한국어] 글로벌 리스트 insert 실패 → da 해제 후 EINVAL. */
		SPDK_ERRLOG("rte_devargs_insert() failed on '%s'\n", devargs_str);
		free(da);
		return -EINVAL;
	}

	return 0;
	/* [한국어] allowlist 등록 성공. */
}
