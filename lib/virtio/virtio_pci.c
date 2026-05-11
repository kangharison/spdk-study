/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK virtio PCI 트랜스포트 (virtio_pci.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK virtio 이니시에이터의 **PCI 백엔드**를 구현한다. 즉, virtio 1.x modern
 * 디바이스(QEMU에 의해 vfio/uio로 노출되거나 실제 SR-IOV virtio HW)를 PCIe BAR 매핑된
 * MMIO 레지스터로 직접 제어한다. 다음 책임을 진다:
 *  1) PCI capability list를 따라가며 4개의 virtio capability(common cfg / notify /
 *     ISR / device cfg)를 발견하고 각자가 가리키는 BAR + 오프셋을 주소로 변환,
 *  2) common_cfg 영역의 status/feature/queue_select/queue_desc/queue_avail/queue_used
 *     레지스터를 spdk_mmio_read/write로 조작,
 *  3) 큐 메모리(vring)를 hugepage DMA 메모리(spdk_zmalloc)로 할당하고 IOVA 주소를
 *     queue_desc/avail/used 레지스터에 게시,
 *  4) modern_notify_queue: queue_notify 페이지에 16-bit write로 doorbell 발사,
 *  5) virtio HW가 SIGBUS(예: 디바이스 리무브)를 일으키면 BAR를 anonymous 메모리로
 *     remap해 폴링 루프가 망가지지 않도록 방어.
 * 트랜스포트-비의존 vring/요청 관리는 virtio.c가 담당하며, 여기서는 modern_ops 콜백 테이블만
 * 채운다 (legacy는 미지원 — modern_caps 발견에 실패하면 거절).
 *
 * === 전체 아키텍처에서의 위치 ===
 * bdev_virtio_blk/scsi (모듈)
 *   ↓ virtio_pci_dev_attach / virtio_pci_dev_enumerate
 * [이 파일: virtio_pci.c — DPDK uio/vfio_pci 드라이버 통한 PCI 매핑 + MMIO]
 *   ↓ spdk_pci_device_map_bar (DPDK rte_pci_map_device 위)
 * Linux uio_pci_generic / vfio-pci → 커널 PCI 서브시스템 → PCIe → virtio HW
 *   ↑ (virtio.c가 this file의 modern_ops 콜백 호출)
 * 실행 컨텍스트: SPDK reactor 위, 호스트 유저스페이스. probe는 보통 init thread,
 * MMIO read/write는 큐 owner_thread.
 *
 * === 타 모듈과의 연결 ===
 * - 상위 의존: virtio.c (vring 관리), bdev_virtio (probe 콜백 enum_cb)
 * - 하위 의존: spdk/env.h(spdk_pci_device_*, spdk_zmalloc), spdk/mmio.h(spdk_mmio_read/write_*)
 * - 공유 자료구조:
 *     struct virtio_hw         : 본 파일 사설 — BAR 매핑 + virtio capability 포인터
 *     struct virtio_pci_common_cfg : virtio 1.x 스펙 §4.1.4.3의 64-byte common cfg 레이아웃
 *     g_virtio_hws (전역 TAILQ): 모든 attach된 hw 등록부 — SIGBUS handler가 순회
 *     g_thread_virtio_hw (TLS) : MMIO 진행 중 hw 표시 — SIGBUS 시 어느 hw인지 식별
 * - 데이터 흐름: PCI cap → BAR vaddr → common_cfg/dev_cfg/isr/notify_base 포인터 →
 *               spdk_mmio_* 호출 → PCIe TLP → 디바이스
 *
 * === 주요 함수/구조체 요약 ===
 *  - virtio_pci_dev_enumerate / virtio_pci_dev_attach: 디바이스 enum/특정 PCI 주소 attach
 *  - virtio_pci_dev_probe   : BAR 매핑 + virtio_read_caps 호출 + g_virtio_hws 등록
 *  - virtio_read_caps       : PCI cap list 순회로 common/notify/isr/dev cfg 위치 발견
 *  - modern_setup_queue     : hugepage 할당 후 desc/avail/used IOVA를 common_cfg에 게시
 *  - modern_notify_queue    : queue_notify_addr에 queue_index write — doorbell
 *  - modern_{get,set}_features: device_feature_select/feature 레지스터로 64-bit 피처 R/W
 *  - virtio_pci_dev_sigbus_handler: BAR SIGBUS 시 anonymous 메모리로 remap
 *  - struct virtio_hw       : BAR 6개의 vaddr/len + 4개 capability 포인터 + pci_dev
 */

#include "spdk/stdinc.h"     /* [한국어] 표준 라이브러리 추상화. */

#include "spdk/memory.h"     /* [한국어] VALUE_2MB(hugepage 단위) 등 메모리 매크로. */
#include "spdk/mmio.h"       /* [한국어] spdk_mmio_read/write_{1,2,4} — 컴파일러 reorder 차단 + WC 메모리 대비. */
#include "spdk/string.h"     /* [한국어] spdk_strerror 등 문자열 유틸. */
#include "spdk/env.h"        /* [한국어] spdk_pci_*, spdk_zmalloc, spdk_vtophys — DPDK PCI/메모리 추상화. */

#include "spdk_internal/virtio.h" /* [한국어] virtio 공통 정의. */
#include <linux/virtio_ids.h>     /* [한국어] VIRTIO_ID_BLOCK/SCSI 등 디바이스 ID 매크로 (Linux UAPI). */

/*
 * [한국어]
 * struct virtio_hw - PCI virtio 디바이스 사설 컨텍스트 (virtio_dev->ctx)
 *
 * BAR 매핑 정보와 4개의 virtio capability 포인터를 보관한다. virtio.c가 backend_ops
 * 콜백을 호출할 때마다 dev->ctx를 이 구조체로 캐스팅해 사용. BAR는 modern virtio에서
 * 다중 BAR에 분산될 수 있어 6개를 모두 매핑하고, capability에 명시된 (bar, offset)으로
 * 각 영역의 가상 주소를 계산한다.
 */
struct virtio_hw {
	uint8_t	    use_msix;
	/* [한국어] MSI-X 사용 여부 (PCI cap에서 발견 시 1).
	 * 설정자: virtio_read_caps에서 PCI_CAP_ID_MSIX 발견 시.
	 * 읽는 자: 현재 SPDK는 MSI-X 인터럽트를 안 쓰므로 정보용 (polled-mode). */

	uint32_t    notify_off_multiplier;
	/* [한국어] queue_notify 영역 내 큐별 오프셋 곱셈 인자.
	 * notify_addr = notify_base + queue_notify_off * multiplier (virtio 스펙 §4.1.4.4).
	 * 설정자: virtio_read_caps에서 NOTIFY_CFG capability 뒤에 따라오는 4-byte 필드 read.
	 * 읽는 자: modern_setup_queue에서 큐별 notify_addr 계산. */

	uint8_t     *isr;
	/* [한국어] ISR(인터럽트 상태) 레지스터 매핑 가상 주소 — 8-bit register.
	 * 설정자: virtio_read_caps에서 ISR_CFG capability 발견 시.
	 * 읽는 자: SPDK polled-mode이므로 거의 사용 안 됨, 인터럽트 시 read하면 0으로 클리어.
	 * 동기화: 아토믹 read 1바이트라 별도 락 불필요. */

	uint16_t    *notify_base;
	/* [한국어] queue notify 영역 시작 가상 주소 — 큐별로 16-bit 슬롯이 분포.
	 * 설정자: virtio_read_caps에서 NOTIFY_CFG capability의 BAR+offset.
	 * 읽는 자: modern_setup_queue가 큐별 notify_addr 계산에 사용.
	 * 동기화: 큐별 슬롯이 다르므로 큐 owner_thread 단위 lockless. */

	struct {
		/** Mem-mapped resources from given PCI BAR */
		void        *vaddr;
		/* [한국어] 이 BAR의 매핑된 가상 주소 (NULL이면 매핑 안 됨/존재 안 함).
		 * 설정자: virtio_pci_dev_probe에서 spdk_pci_device_map_bar 결과.
		 * 읽는 자: get_cfg_addr이 capability의 (bar, offset)으로 vaddr+offset 계산.
		 * 동기화: 디바이스 lifetime 동안 불변 — 락 불필요. */

		/** Length of the address space */
		uint32_t    len;
		/* [한국어] BAR 길이 (바이트). capability offset+length가 이를 넘으면 거절.
		 * 설정자: spdk_pci_device_map_bar의 bar_len 출력.
		 * 읽는 자: get_cfg_addr 경계 검사, sigbus handler가 mmap에 사용. */
	} pci_bar[6];
	/* [한국어] PCI 디바이스의 BAR0~BAR5 매핑 슬롯. virtio modern 디바이스는 capability가
	 * 어떤 BAR를 가리키는지 명시하므로, 모두 매핑해두고 cap 발견 시 인덱싱한다.
	 * 동기화: virtio_pci_dev_probe 단일 스레드 init 후 read-only — 락 불필요. */

	struct virtio_pci_common_cfg *common_cfg;
	/* [한국어] virtio 1.x common configuration 영역 시작 포인터 (BAR + offset).
	 * 레이아웃(virtio 스펙 §4.1.4.3): device_feature_select/device_feature/guest_feature_select/
	 *   guest_feature/msix_config/num_queues/device_status/config_generation/queue_select/
	 *   queue_size/queue_msix_vector/queue_enable/queue_notify_off/queue_desc/queue_avail/queue_used.
	 * 설정자: virtio_read_caps에서 COMMON_CFG capability의 vaddr.
	 * 읽는 자: modern_* ops 거의 모든 콜백.
	 * 동기화: queue_select와 큐별 레지스터는 atomic하지 않으므로 큐 셋업/티어다운 시
	 *         g_thread_virtio_hw 가드로 단일 스레드 보장. */

	struct spdk_pci_device *pci_dev;
	/* [한국어] DPDK PCI 디바이스 핸들 — spdk_pci_addr/spdk_pci_device_cfg_read 등의 인자.
	 * 설정자: virtio_pci_dev_probe_cb에서 enum_cb 인자로 받은 값.
	 * 읽는 자: virtio_pci_dev_check, free_virtio_hw 등.
	 * 동기화: 디바이스 lifetime 동안 불변. */

	/** Device-specific PCI config space */
	void *dev_cfg;
	/* [한국어] device-specific config 영역 시작 (예: virtio-blk capacity).
	 * 설정자: virtio_read_caps에서 DEVICE_CFG capability의 vaddr.
	 * 읽는 자: modern_read_dev_config / modern_write_dev_config가 offset을 더해 접근.
	 * 동기화: config_generation으로 read 일관성 검증(읽는 동안 백엔드가 갱신해도 재시도). */

	struct virtio_dev *vdev;
	/* [한국어] 부모 virtio_dev 백포인터.
	 * 설정자: virtio_pci_dev_init에서 hw->vdev = vdev로 연결.
	 * 읽는 자: virtio_pci_dev_check가 hw->vdev->name 반환 시. */

	bool is_remapped;
	/* [한국어] SIGBUS 후 anonymous로 remap 됐는지 여부 — 중복 remap 방지.
	 * 설정자: virtio_pci_dev_sigbus_handler.
	 * 읽는 자: 동일 핸들러가 다음 SIGBUS 시 검사. */

	bool is_removing;
	/* [한국어] 디바이스 리무브 진행 중 마커 — 이중 처리 방지.
	 * 설정자: virtio_pci_dev_check.
	 * 읽는 자: virtio_pci_dev_event_process. */

	TAILQ_ENTRY(virtio_hw) tailq;
	/* [한국어] g_virtio_hws 리스트 링크 — sigbus handler가 모든 hw 순회 시 사용.
	 * 동기화: g_hw_mutex 아래에서 INSERT/REMOVE/FOREACH. */
};

/*
 * [한국어]
 * struct virtio_pci_probe_ctx - probe 콜백 인자 묶음
 *
 * spdk_pci_enumerate가 후보 PCI 디바이스를 만날 때마다 virtio_pci_dev_probe_cb를
 * 호출하는데, 이 콜백에 사용자 컨텍스트(enum_cb, device_id)를 전달하기 위한 트램펄린 구조체.
 */
struct virtio_pci_probe_ctx {
	virtio_pci_create_cb enum_cb;
	/* [한국어] 사용자(bdev_virtio)가 등록한 콜백 — virtio_hw가 발견되면 호출되어 virtio_dev 만듦.
	 * 설정자: virtio_pci_dev_enumerate / attach 진입점에서 ctx에 저장.
	 * 읽는 자: virtio_pci_dev_probe가 BAR 매핑 + caps 검증 후 호출. */

	void *enum_ctx;
	/* [한국어] enum_cb로 그대로 전달되는 사용자 컨텍스트 (보통 bdev module ctx).
	 * 동기화: probe 콜백은 spdk_pci_enumerate 직렬 호출 — 락 불필요. */

	uint16_t device_id;
	/* [한국어] 매칭할 virtio device_id (예: VIRTIO_ID_BLOCK=2, VIRTIO_ID_SCSI=8).
	 * 읽는 자: virtio_pci_dev_probe_cb가 PCI device_id를 변환해 비교. */
};

/* [한국어] 전역: 모든 attach된 virtio_hw 리스트. SIGBUS handler가 이를 순회해 BAR remap. */
static TAILQ_HEAD(, virtio_hw) g_virtio_hws = TAILQ_HEAD_INITIALIZER(g_virtio_hws);
/* [한국어] g_virtio_hws 보호 뮤텍스 — INSERT/REMOVE/FOREACH 직렬화. */
static pthread_mutex_t g_hw_mutex = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] TLS — 현재 thread가 MMIO 진행 중인 hw. SIGBUS handler가 이를 보고 BAR remap 대상 식별.
 * 각 modern_* 콜백의 진입에서 set, 종료에서 NULL — async-signal-safe context에서 락 없이 접근. */
__thread struct virtio_hw *g_thread_virtio_hw = NULL;
/* [한국어] SIGBUS handler 진입 직렬화용 atomic flag — 여러 thread가 동시에 SIGBUS 시 한 번만 처리. */
static uint16_t g_signal_lock;
/* [한국어] SIGBUS handler 등록 여부 (한 번만 등록). */
static bool g_sigset = false;

/*
 * Following macros are derived from linux/pci_regs.h, however,
 * we can't simply include that header here, as there is no such
 * file for non-Linux platform.
 */
/* [한국어] PCI 표준 capability list 시작 오프셋 — config space[0x34]가 첫 capability ptr. */
#define PCI_CAPABILITY_LIST	0x34
/* [한국어] vendor-specific capability ID — virtio capability는 모두 이 형식으로 노출. */
#define PCI_CAP_ID_VNDR		0x09
/* [한국어] MSI-X capability ID. */
#define PCI_CAP_ID_MSIX		0x11

/*
 * [한국어]
 * virtio_pci_dev_sigbus_handler - BAR 접근 중 SIGBUS 발생 시 BAR를 anonymous 메모리로 remap
 *
 * @failure_addr: SIGBUS를 일으킨 주소 (현재 미사용 — TLS hw로 식별)
 * @ctx         : SPDK pci 에러 핸들러 컨텍스트 (미사용)
 *
 * 발생 시나리오: vfio-pci로 매핑한 디바이스가 hot-remove되거나 디바이스가 응답 불가 상태가
 * 되면 BAR MMIO read/write가 SIGBUS를 발생시킨다. polling 루프가 SIGBUS로 죽지 않도록
 * 같은 가상 주소(MAP_FIXED)에 익명 메모리를 매핑하고 0xFF로 채워 모든 read가 -1을 반환하게 한다.
 * 후속 MMIO 호출은 정상 진행되며 디바이스 코드는 read 결과 0xFF...로 디바이스 부재를 인지한다.
 *
 * 실행 컨텍스트: 시그널 핸들러 — async-signal-safe 함수만 사용 가능 (mmap, munmap, atomic ops, memset OK).
 *               TLS g_thread_virtio_hw로 어느 hw가 SIGBUS인지 식별 (락 불가).
 *
 * 호출 체인:
 *   커널 SIGBUS → spdk_pci_register_error_handler → [virtio_pci_dev_sigbus_handler]
 */
static void
virtio_pci_dev_sigbus_handler(const void *failure_addr, void *ctx)
{
	void *map_address = NULL;  /* [한국어] mmap 결과 임시 저장. */
	uint16_t flag = 0;         /* [한국어] CAS expected 값 (lock free). */
	int i;                     /* [한국어] BAR 인덱스 순회. */

	/* [한국어] 다중 thread가 동시에 SIGBUS면 한 thread만 진입 — CAS로 0→1 시도, 실패 시 즉시 반환. */
	if (!__atomic_compare_exchange_n(&g_signal_lock, &flag, 1, false, __ATOMIC_ACQUIRE,
					 __ATOMIC_RELAXED)) {
		SPDK_DEBUGLOG(virtio_pci, "request g_signal_lock failed\n"); /* [한국어] 다른 thread가 처리 중. */
		return;
	}

	if (g_thread_virtio_hw == NULL || g_thread_virtio_hw->is_remapped) {
		/* [한국어] 어느 hw에서 발생했는지 모르거나 이미 remap된 hw — 처리 불필요. */
		__atomic_store_n(&g_signal_lock, 0, __ATOMIC_RELEASE); /* [한국어] 락 해제. */
		return;
	}

	/* We remap each bar to the same VA to avoid subsequent sigbus error.
	 * Because it is mapped to the same VA, such as hw->common_cfg and so on
	 * do not need to be modified.
	 */
	/* [한국어] 핵심 트릭: MAP_FIXED + 동일 VA로 anonymous mmap → common_cfg 등 캐시된
	 * 포인터가 그대로 유효. 이후 read는 0xFF, write는 무시되어 polling 루프가 살아남는다. */
	for (i = 0; i < 6; ++i) {
		if (g_thread_virtio_hw->pci_bar[i].vaddr == NULL) {
			continue;  /* [한국어] 매핑 안 된 BAR는 스킵. */
		}

		map_address = mmap(g_thread_virtio_hw->pci_bar[i].vaddr,
				   g_thread_virtio_hw->pci_bar[i].len,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		/* [한국어] MAP_FIXED: 동일 VA 강제 (기존 매핑 unmap+replace).
		 * MAP_ANONYMOUS: 파일 백업 X — 디바이스와의 연결 끊김.
		 * 이 호출은 async-signal-safe (Linux mmap은 시그널 안전 시스템 콜). */
		if (map_address == MAP_FAILED) {
			SPDK_ERRLOG("mmap failed\n");  /* [한국어] 가상 주소 공간 오류 — 복구 불가. */
			goto fail;
		}
		memset(map_address, 0xFF, g_thread_virtio_hw->pci_bar[i].len); /* [한국어] read 시 -1을 반환하도록 0xFF로 채움 — 디바이스 부재 시그널. */
	}

	g_thread_virtio_hw->is_remapped = true; /* [한국어] 다음 SIGBUS에서 재처리 방지. */
	__atomic_store_n(&g_signal_lock, 0, __ATOMIC_RELEASE); /* [한국어] 다른 thread가 다른 hw로 SIGBUS 받을 수 있도록 락 해제. */
	return;
fail:
	/* [한국어] mmap 실패 — 이미 remap한 BAR들은 unmap으로 되돌리고 락 해제. */
	for (--i; i >= 0; i--) {
		if (g_thread_virtio_hw->pci_bar[i].vaddr == NULL) {
			continue;
		}

		munmap(g_thread_virtio_hw->pci_bar[i].vaddr, g_thread_virtio_hw->pci_bar[i].len); /* [한국어] anonymous 매핑 해제. */
	}
	__atomic_store_n(&g_signal_lock, 0, __ATOMIC_RELEASE);
}

/*
 * [한국어]
 * virtio_pci_dev_get_by_addr - PCI 주소로 g_virtio_hws에서 hw 검색
 *
 * @traddr: 검색할 PCI 주소 (DBDF)
 * @return: 매칭 hw 또는 NULL
 *
 * 실행 컨텍스트: 어느 thread든 가능 — g_hw_mutex로 직렬화.
 *
 * 호출 체인:
 *   virtio_pci_dev_event_process (UIO remove 이벤트) → [virtio_pci_dev_get_by_addr]
 */
static struct virtio_hw *
virtio_pci_dev_get_by_addr(struct spdk_pci_addr *traddr)
{
	struct virtio_hw *hw;          /* [한국어] 순회 변수. */
	struct spdk_pci_addr addr;     /* [한국어] 비교용 임시 주소. */

	pthread_mutex_lock(&g_hw_mutex); /* [한국어] 리스트 보호. */
	TAILQ_FOREACH(hw, &g_virtio_hws, tailq) {
		addr = spdk_pci_device_get_addr(hw->pci_dev); /* [한국어] hw의 실제 PCI 주소 획득. */
		if (!spdk_pci_addr_compare(&addr, traddr)) {  /* [한국어] 0 = 일치. */
			pthread_mutex_unlock(&g_hw_mutex);
			return hw;
		}
	}
	pthread_mutex_unlock(&g_hw_mutex);

	return NULL;  /* [한국어] 미발견. */
}

/*
 * [한국어]
 * virtio_pci_dev_check - hw가 주어진 device_id 매칭하면 is_removing 마킹 후 이름 반환
 *
 * @hw             : 검사할 hw
 * @device_id_match: 매칭할 virtio device_id (예: VIRTIO_ID_BLOCK)
 * @return: 매칭 시 hw->vdev->name, 미매칭 시 NULL
 *
 * PCI device_id는 transitional(<0x1040)과 modern(>=0x1040)이 다른 매핑 규칙을 사용 — 스펙 §4.1.2.
 *
 * 호출 체인:
 *   virtio_pci_dev_event_process → [virtio_pci_dev_check]
 */
static const char *
virtio_pci_dev_check(struct virtio_hw *hw, uint16_t device_id_match)
{
	uint16_t pci_device_id, device_id;  /* [한국어] PCI 표준 device_id, virtio 의미의 device_id. */

	pci_device_id = spdk_pci_device_get_device_id(hw->pci_dev); /* [한국어] PCI config space의 device_id 필드. */
	if (pci_device_id < 0x1040) {
		/* Transitional devices: use the PCI subsystem device id as
		 * virtio device id, same as legacy driver always did.
		 */
		/* [한국어] transitional(legacy 호환): subsystem device id가 virtio device_id. */
		device_id = spdk_pci_device_get_subdevice_id(hw->pci_dev);
	} else {
		/* Modern devices: simply use PCI device id, but start from 0x1040. */
		/* [한국어] modern: PCI device_id - 0x1040이 virtio device_id (스펙 §4.1.2.1). */
		device_id = pci_device_id - 0x1040;
	}

	if (device_id == device_id_match) {
		hw->is_removing = true;  /* [한국어] 리무브 처리 중 마킹 — 중복 호출 방지. */
		return hw->vdev->name;   /* [한국어] 상위 모듈에 알릴 디바이스 이름. */
	}

	return NULL;
}

/*
 * [한국어]
 * virtio_pci_dev_event_process - PCI 이벤트(UIO remove / VFIO removed) 검사 후 매칭 hw 반환
 *
 * @fd       : UIO event fd (uevent monitor)
 * @device_id: 관심 virtio device_id
 * @return: 리무브된 디바이스 이름 (없으면 NULL)
 *
 * 두 가지 리무브 경로 처리:
 *   1) UIO: spdk_pci_get_event로 uevent fd에서 REMOVE 이벤트 수신
 *   2) VFIO: spdk_pci_device_is_removed로 폴링 — vfio는 이벤트 fd 대신 상태 검사 사용
 *
 * 실행 컨텍스트: bdev_virtio의 리무브 폴러 — SPDK thread context.
 *
 * 호출 체인:
 *   bdev_virtio remove poller → [virtio_pci_dev_event_process]
 */
const char *
virtio_pci_dev_event_process(int fd, uint16_t device_id)
{
	struct spdk_pci_event event;  /* [한국어] uevent 결과. */
	struct virtio_hw *hw, *tmp;   /* [한국어] 순회 변수 + 안전 삭제용 next 포인터. */
	const char *vdev_name;        /* [한국어] 매칭된 디바이스 이름. */

	/* UIO remove handler */
	if (spdk_pci_get_event(fd, &event) > 0) {  /* [한국어] uevent fd에서 이벤트 1건 읽기. >0 = 이벤트 있음. */
		if (event.action == SPDK_UEVENT_REMOVE) {
			hw = virtio_pci_dev_get_by_addr(&event.traddr); /* [한국어] traddr로 hw 식별. */
			if (hw == NULL || hw->is_removing) {
				return NULL;  /* [한국어] 알 수 없음 또는 이미 처리 중. */
			}

			vdev_name = virtio_pci_dev_check(hw, device_id);
			if (vdev_name != NULL) {
				return vdev_name;
			}
		}
	}

	/* VFIO remove handler */
	pthread_mutex_lock(&g_hw_mutex);
	TAILQ_FOREACH_SAFE(hw, &g_virtio_hws, tailq, tmp) { /* [한국어] _SAFE: 순회 중 삭제 가능 변형. */
		if (spdk_pci_device_is_removed(hw->pci_dev) && !hw->is_removing) { /* [한국어] vfio가 보고하는 리무브 상태. */
			vdev_name = virtio_pci_dev_check(hw, device_id);
			if (vdev_name != NULL) {
				pthread_mutex_unlock(&g_hw_mutex);
				return vdev_name;
			}
		}
	}
	pthread_mutex_unlock(&g_hw_mutex);

	return NULL;
}

/*
 * [한국어]
 * check_vq_phys_addr_ok - vring 물리 주소가 32-bit PFN 범위 내인지 검증
 *
 * @vq: 검사할 virtqueue
 * @return: 1 OK, 0 NG (16TB 초과)
 *
 * legacy virtio PCI는 큐 주소를 32-bit PFN(page frame number)으로 게시한다 — PFN×4KB가
 * 한계라 4GB×4KB = 16TB 미만이어야 함. modern은 64-bit이지만 동일 검사 통과 보장 차원에서
 * 호출.
 *
 * 호출 체인:
 *   modern_setup_queue → [check_vq_phys_addr_ok]
 */
static inline int
check_vq_phys_addr_ok(struct virtqueue *vq)
{
	/* Virtio PCI device VIRTIO_PCI_QUEUE_PF register is 32bit,
	 * and only accepts 32 bit page frame number.
	 * Check if the allocated physical memory exceeds 16TB.
	 */
	/* [한국어] (ring_mem + ring_size - 1) 끝주소가 (4KB << 32) = 16TB를 초과하면 PFN 표현 불가. */
	if ((vq->vq_ring_mem + vq->vq_ring_size - 1) >>
	    (VIRTIO_PCI_QUEUE_ADDR_SHIFT + 32)) {
		SPDK_ERRLOG("vring address shouldn't be above 16TB!\n");
		return 0;
	}

	return 1;
}

/*
 * [한국어]
 * free_virtio_hw - virtio_hw 구조체 + 매핑된 BAR 해제
 *
 * @hw: 해제할 hw
 * @return: 없음
 *
 * 모든 매핑된 BAR을 spdk_pci_device_unmap_bar(DPDK 측에서 munmap + 페이지 회수)로 풀고,
 * hw 구조체 free.
 *
 * 호출 체인:
 *   virtio_pci_dev_probe(에러) / modern_destruct_dev → [free_virtio_hw]
 */
static void
free_virtio_hw(struct virtio_hw *hw)
{
	unsigned i;  /* [한국어] BAR 인덱스. */

	for (i = 0; i < 6; ++i) {
		if (hw->pci_bar[i].vaddr == NULL) {
			continue;  /* [한국어] 매핑 안 된 BAR 스킵. */
		}

		spdk_pci_device_unmap_bar(hw->pci_dev, i, hw->pci_bar[i].vaddr); /* [한국어] DPDK 측 BAR 해제. */
	}

	free(hw);  /* [한국어] 구조체 자체 해제. */
}

/*
 * [한국어]
 * pci_dump_json_info - JSON 응답에 PCI virtio 디바이스 정보 추가 (RPC handler 보조)
 *
 * @dev: 대상 virtio_dev
 * @w  : SPDK JSON writer
 *
 * "type" 필드("pci-modern"/"pci-legacy"), "pci_address" 필드(BDF 문자열) 출력.
 *
 * 호출 체인:
 *   virtio_dev_dump_json_info → backend_ops->dump_json_info(=[pci_dump_json_info])
 */
static void
pci_dump_json_info(struct virtio_dev *dev, struct spdk_json_write_ctx *w)
{
	struct virtio_hw *hw = dev->ctx;  /* [한국어] virtio_dev->ctx에 저장된 hw 캐스팅. */
	struct spdk_pci_addr pci_addr = spdk_pci_device_get_addr((struct spdk_pci_device *)hw->pci_dev); /* [한국어] PCI 주소 획득. */
	char addr[32];  /* [한국어] BDF 포맷 문자열 ("0000:00:01.0" 형태). */

	spdk_json_write_name(w, "type");  /* [한국어] "type" key 시작. */
	if (dev->modern) {                /* [한국어] modern flag는 virtio_pci_dev_init에서 1 set. */
		spdk_json_write_string(w, "pci-modern");
	} else {
		spdk_json_write_string(w, "pci-legacy"); /* [한국어] 현재 코드는 legacy 거절하므로 도달 불가하지만 안전망. */
	}

	spdk_pci_addr_fmt(addr, sizeof(addr), &pci_addr); /* [한국어] BDF → 문자열. */
	spdk_json_write_named_string(w, "pci_address", addr);
}

/*
 * [한국어]
 * pci_write_json_config - JSON 응답에 트랜스포트 식별자 출력 (config save 시)
 *
 * @dev: 대상
 * @w  : JSON writer
 *
 * "trtype":"pci", "traddr":"<BDF>" — bdev_virtio가 config dump할 때 사용.
 */
static void
pci_write_json_config(struct virtio_dev *dev, struct spdk_json_write_ctx *w)
{
	struct virtio_hw *hw = dev->ctx;
	struct spdk_pci_addr pci_addr = spdk_pci_device_get_addr(hw->pci_dev);
	char addr[32];

	spdk_pci_addr_fmt(addr, sizeof(addr), &pci_addr);

	spdk_json_write_named_string(w, "trtype", "pci");      /* [한국어] 트랜스포트 종류. */
	spdk_json_write_named_string(w, "traddr", addr);       /* [한국어] PCI 주소. */
}

/*
 * [한국어]
 * io_write64_twopart - 64-bit 값을 32-bit 두 번 write로 분할 기록
 *
 * @val: 64-bit 값
 * @lo : 하위 32-bit MMIO 주소
 * @hi : 상위 32-bit MMIO 주소
 *
 * virtio common_cfg의 queue_desc/avail/used는 64-bit 주소를 32-bit lo/hi 두 레지스터로
 * 분할 노출 — 한 번의 64-bit write가 PCI에서 보장 안 되므로.
 *
 * 호출 체인:
 *   modern_setup_queue / modern_del_queue → [io_write64_twopart]
 */
static inline void
io_write64_twopart(uint64_t val, uint32_t *lo, uint32_t *hi)
{
	spdk_mmio_write_4(lo, val & ((1ULL << 32) - 1)); /* [한국어] 하위 32 비트. */
	spdk_mmio_write_4(hi, val >> 32);                /* [한국어] 상위 32 비트. */
}

/*
 * [한국어]
 * modern_read_dev_config - device-specific config 영역 read (generation 일관성 보장)
 *
 * @dev   : 대상 디바이스
 * @offset: dev_cfg 시작 오프셋
 * @dst   : [out] read 버퍼
 * @length: 바이트 수
 * @return: 0 (실패 경로 없음 — MMIO read는 SIGBUS handler가 처리)
 *
 * virtio 1.x §2.4.1: device가 config를 갱신하는 동안 드라이버가 read 중일 수 있어,
 * config_generation을 read 전후로 비교해 변하지 않을 때까지 재시도. 1바이트씩 read하는
 * 이유는 일부 필드가 word-단위 atomic을 요구하지 않고 generation으로 보호되기 때문.
 *
 * 실행 컨텍스트: 호출 thread. g_thread_virtio_hw로 SIGBUS 핸들러에 hw 전달.
 *
 * 호출 체인:
 *   virtio_dev_read_dev_config → backend_ops->read_dev_cfg(=[modern_read_dev_config])
 */
static int
modern_read_dev_config(struct virtio_dev *dev, size_t offset,
		       void *dst, int length)
{
	struct virtio_hw *hw = dev->ctx;  /* [한국어] hw 컨텍스트. */
	int i;                             /* [한국어] 바이트 인덱스. */
	uint8_t *p;                        /* [한국어] dst 진행 포인터. */
	uint8_t old_gen, new_gen;          /* [한국어] generation 비교용. */

	g_thread_virtio_hw = hw;           /* [한국어] SIGBUS 시 BAR remap 대상 식별 위해 TLS 설정. */
	do {
		old_gen = spdk_mmio_read_1(&hw->common_cfg->config_generation); /* [한국어] read 시작 시점 generation. */

		p = dst;
		for (i = 0;  i < length; i++) {
			*p++ = spdk_mmio_read_1((uint8_t *)hw->dev_cfg + offset + i); /* [한국어] dev_cfg 영역에서 1바이트씩 읽기. */
		}

		new_gen = spdk_mmio_read_1(&hw->common_cfg->config_generation); /* [한국어] read 종료 후 generation. */
	} while (old_gen != new_gen);  /* [한국어] 다르면 중간에 디바이스가 갱신함 — 재시도. */
	g_thread_virtio_hw = NULL;     /* [한국어] TLS 클리어. */

	return 0;
}

/*
 * [한국어]
 * modern_write_dev_config - device-specific config write (단순 1바이트 시퀀스)
 *
 * @dev   : 대상
 * @offset: 오프셋
 * @src   : 데이터
 * @length: 바이트 수
 * @return: 0
 *
 * virtio 스펙상 driver writes는 일관성 검증이 디바이스 책임이므로 단순 시퀀셜 write.
 *
 * 호출 체인:
 *   virtio_dev_write_dev_config → backend_ops->write_dev_cfg(=[modern_write_dev_config])
 */
static int
modern_write_dev_config(struct virtio_dev *dev, size_t offset,
			const void *src, int length)
{
	struct virtio_hw *hw = dev->ctx;
	int i;
	const uint8_t *p = src;

	g_thread_virtio_hw = hw;
	for (i = 0;  i < length; i++) {
		spdk_mmio_write_1(((uint8_t *)hw->dev_cfg) + offset + i, *p++); /* [한국어] 1바이트씩 dev_cfg에 write. */
	}
	g_thread_virtio_hw = NULL;

	return 0;
}

/*
 * [한국어]
 * modern_get_features - 디바이스 지원 피처 64-bit read (lo/hi 분할)
 *
 * @dev: 대상
 * @return: 디바이스 지원 피처 마스크
 *
 * virtio common_cfg의 device_feature는 32-bit이고 select 레지스터로 lo/hi 토글한다 — 스펙 §4.1.4.3.
 * 동작:
 *   1) device_feature_select = 0 → lo 32 비트 read
 *   2) device_feature_select = 1 → hi 32 비트 read
 *
 * 호출 체인:
 *   virtio_negotiate_features → backend_ops->get_features(=[modern_get_features])
 */
static uint64_t
modern_get_features(struct virtio_dev *dev)
{
	struct virtio_hw *hw = dev->ctx;
	uint32_t features_lo, features_hi;  /* [한국어] 하/상위 32 비트. */

	g_thread_virtio_hw = hw;
	spdk_mmio_write_4(&hw->common_cfg->device_feature_select, 0); /* [한국어] 선택 = 하위 윈도. */
	features_lo = spdk_mmio_read_4(&hw->common_cfg->device_feature); /* [한국어] 디바이스가 노출한 lo 32. */

	spdk_mmio_write_4(&hw->common_cfg->device_feature_select, 1); /* [한국어] 선택 = 상위 윈도. */
	features_hi = spdk_mmio_read_4(&hw->common_cfg->device_feature); /* [한국어] hi 32. */
	g_thread_virtio_hw = NULL;

	return ((uint64_t)features_hi << 32) | features_lo;
}

/*
 * [한국어]
 * modern_set_features - 드라이버 협상 피처 64-bit write
 *
 * @dev     : 대상
 * @features: 협상 결과 피처 마스크
 * @return: 0 성공, -EINVAL(VERSION_1 미세트)
 *
 * SPDK virtio 이니시에이터는 modern만 지원 → VIRTIO_F_VERSION_1 비트가 반드시 포함되어야 함.
 * lo/hi 분할 write 후 dev->negotiated_features에 캐싱.
 *
 * 호출 체인:
 *   virtio_negotiate_features → backend_ops->set_features(=[modern_set_features])
 */
static int
modern_set_features(struct virtio_dev *dev, uint64_t features)
{
	struct virtio_hw *hw = dev->ctx;

	if ((features & (1ULL << VIRTIO_F_VERSION_1)) == 0) {
		SPDK_ERRLOG("VIRTIO_F_VERSION_1 feature is not enabled.\n"); /* [한국어] modern 강제 — legacy fallback 거부. */
		return -EINVAL;
	}

	g_thread_virtio_hw = hw;
	spdk_mmio_write_4(&hw->common_cfg->guest_feature_select, 0);            /* [한국어] 하위 윈도. */
	spdk_mmio_write_4(&hw->common_cfg->guest_feature, features & ((1ULL << 32) - 1)); /* [한국어] lo 32 write. */

	spdk_mmio_write_4(&hw->common_cfg->guest_feature_select, 1);            /* [한국어] 상위 윈도. */
	spdk_mmio_write_4(&hw->common_cfg->guest_feature, features >> 32);      /* [한국어] hi 32 write. */
	g_thread_virtio_hw = NULL;

	dev->negotiated_features = features;  /* [한국어] 캐싱 — virtio.c가 EVENT_IDX 등 검사 시 참조. */

	return 0;
}

/*
 * [한국어]
 * modern_destruct_dev - PCI 디바이스 detach + hw 자원 해제
 *
 * @vdev: 대상 디바이스
 *
 * 동작:
 *   1) g_virtio_hws에서 hw 제거 (mutex 보호)
 *   2) BAR 매핑 해제 + hw struct free
 *   3) DPDK PCI 디바이스 detach (드라이버 unbind, fd close)
 *
 * 실행 컨텍스트: detach 경로의 단일 스레드.
 *
 * 호출 체인:
 *   virtio_dev_destruct → backend_ops->destruct_dev(=[modern_destruct_dev])
 */
static void
modern_destruct_dev(struct virtio_dev *vdev)
{
	struct virtio_hw *hw = vdev->ctx;
	struct spdk_pci_device *pci_dev;

	if (hw != NULL) {
		pthread_mutex_lock(&g_hw_mutex);
		TAILQ_REMOVE(&g_virtio_hws, hw, tailq);  /* [한국어] sigbus handler에서 더 이상 hw 안 보이도록 먼저 제거. */
		pthread_mutex_unlock(&g_hw_mutex);
		pci_dev = hw->pci_dev;                    /* [한국어] free 전 pci_dev 백업. */
		free_virtio_hw(hw);                       /* [한국어] BAR unmap + struct free. */
		if (pci_dev) {
			spdk_pci_device_detach(pci_dev);  /* [한국어] DPDK 측 detach (드라이버 unbind). */
		}
	}
}

/*
 * [한국어]
 * modern_get_status - 디바이스 status 비트 read
 *
 * @dev: 대상
 * @return: status (ACK | DRIVER | FEATURES_OK | DRIVER_OK | FAILED 등)
 *
 * 호출 체인:
 *   virtio_dev_get_status / set_status read-back → backend_ops->get_status(=[modern_get_status])
 */
static uint8_t
modern_get_status(struct virtio_dev *dev)
{
	struct virtio_hw *hw = dev->ctx;
	uint8_t ret;

	g_thread_virtio_hw = hw;
	ret = spdk_mmio_read_1(&hw->common_cfg->device_status); /* [한국어] common_cfg.device_status — 8-bit. */
	g_thread_virtio_hw = NULL;

	return ret;
}

/*
 * [한국어]
 * modern_set_status - status 비트 write
 *
 * @dev   : 대상
 * @status: write할 비트 마스크 (RESET=0 또는 누적 비트)
 *
 * 호출 체인:
 *   virtio_dev_set_status → backend_ops->set_status(=[modern_set_status])
 */
static void
modern_set_status(struct virtio_dev *dev, uint8_t status)
{
	struct virtio_hw *hw = dev->ctx;

	g_thread_virtio_hw = hw;
	spdk_mmio_write_1(&hw->common_cfg->device_status, status); /* [한국어] 1바이트 write. RESET=0이면 디바이스 리셋. */
	g_thread_virtio_hw = NULL;
}

/*
 * [한국어]
 * modern_get_queue_size - 큐 크기 query (queue_select → queue_size read)
 *
 * @dev     : 대상
 * @queue_id: 큐 인덱스
 * @return: 디바이스 보고 큐 크기 (0이면 미존재)
 *
 * common_cfg는 queue_select로 큐 컨텍스트를 선택한 뒤 queue_size 등을 조회 — 모든 큐가
 * 한 윈도를 공유하므로 select가 동시성 문제 — g_thread_virtio_hw 가드로 단일 thread 사용.
 *
 * 호출 체인:
 *   virtio_init_queue → backend_ops->get_queue_size(=[modern_get_queue_size])
 */
static uint16_t
modern_get_queue_size(struct virtio_dev *dev, uint16_t queue_id)
{
	struct virtio_hw *hw = dev->ctx;
	uint16_t ret;

	g_thread_virtio_hw = hw;
	spdk_mmio_write_2(&hw->common_cfg->queue_select, queue_id); /* [한국어] 큐 컨텍스트 선택. */
	ret = spdk_mmio_read_2(&hw->common_cfg->queue_size);        /* [한국어] 디바이스가 advertise한 크기. */
	g_thread_virtio_hw = NULL;

	return ret;
}

/*
 * [한국어]
 * modern_setup_queue - vring 메모리 할당 + IOVA 게시 + 큐 활성화
 *
 * @dev: 대상
 * @vq : 셋업할 virtqueue (vq_ring_size, vq_nentries 채워져 있음)
 * @return: 0 성공, -ENOMEM(메모리 또는 16TB 초과), -EFAULT(vtophys 실패)
 *
 * 동작:
 *   1) ring 크기 검증: 단일 hugepage(2MB) 내 — 물리적 연속성 보장
 *   2) spdk_zmalloc(SPDK_MALLOC_DMA): hugepage에서 DMA 가능 메모리 할당
 *   3) spdk_vtophys: VA → IOVA(IOMMU 사용 시) 또는 물리 주소
 *   4) common_cfg에 queue_desc/avail/used IOVA 게시 (lo/hi 분할 write)
 *   5) queue_notify_off read → notify_addr = notify_base + off * multiplier
 *   6) queue_enable = 1 → 디바이스가 이 큐 처리 시작
 *
 * 실행 컨텍스트: 디바이스 init thread 단일.
 *
 * 호출 체인:
 *   virtio_init_queue → backend_ops->setup_queue(=[modern_setup_queue])
 */
static int
modern_setup_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	struct virtio_hw *hw = dev->ctx;
	uint64_t desc_addr, avail_addr, used_addr;  /* [한국어] vring 3개 영역의 IOVA. */
	uint16_t notify_off;                         /* [한국어] queue_notify 영역 내 큐별 오프셋. */
	void *queue_mem;                             /* [한국어] hugepage 할당 결과 VA. */
	uint64_t queue_mem_phys_addr;                /* [한국어] 위 VA의 IOVA. */

	/* To ensure physical address contiguity we make the queue occupy
	 * only a single hugepage (2MB). As of Virtio 1.0, the queue size
	 * always falls within this limit.
	 */
	/* [한국어] hugepage(2MB)는 물리적으로 연속 — 한 큐가 여기 fit하면 IOVA 연속성 보장. */
	if (vq->vq_ring_size > VALUE_2MB) {
		return -ENOMEM;
	}

	queue_mem = spdk_zmalloc(vq->vq_ring_size, VALUE_2MB, NULL,
				 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	/* [한국어] 2MB 정렬 + 0 초기화. SPDK_MALLOC_DMA → DPDK rte_malloc_socket로 DMA-pinned hugepage에서 할당. */
	if (queue_mem == NULL) {
		return -ENOMEM;
	}

	queue_mem_phys_addr = spdk_vtophys(queue_mem, NULL); /* [한국어] VA → IOVA. NULL은 길이 출력 받지 않음. */
	if (queue_mem_phys_addr == SPDK_VTOPHYS_ERROR) {
		spdk_free(queue_mem);
		return -EFAULT;  /* [한국어] hugepage가 매핑 안 된 비정상 경우. */
	}

	vq->vq_ring_mem = queue_mem_phys_addr; /* [한국어] 디바이스에게 보일 IOVA. */
	vq->vq_ring_virt_mem = queue_mem;       /* [한국어] SPDK가 사용할 VA. */

	if (!check_vq_phys_addr_ok(vq)) {       /* [한국어] 16TB 한계 검증. */
		spdk_free(queue_mem);
		return -ENOMEM;
	}

	desc_addr = vq->vq_ring_mem;                                                  /* [한국어] desc 영역 시작. */
	avail_addr = desc_addr + vq->vq_nentries * sizeof(struct vring_desc);         /* [한국어] avail은 desc 바로 뒤. */
	used_addr = (avail_addr + offsetof(struct vring_avail, ring[vq->vq_nentries])
		     + VIRTIO_PCI_VRING_ALIGN - 1) & ~(VIRTIO_PCI_VRING_ALIGN - 1);
	/* [한국어] used는 avail 끝에서 정렬(보통 4) 경계에 align — virtio 스펙 §2.6에서 요구. */

	g_thread_virtio_hw = hw;
	spdk_mmio_write_2(&hw->common_cfg->queue_select, vq->vq_queue_index); /* [한국어] 셋업 대상 큐 선택. */

	io_write64_twopart(desc_addr, &hw->common_cfg->queue_desc_lo,
			   &hw->common_cfg->queue_desc_hi);   /* [한국어] desc 64-bit IOVA 게시. */
	io_write64_twopart(avail_addr, &hw->common_cfg->queue_avail_lo,
			   &hw->common_cfg->queue_avail_hi);  /* [한국어] avail IOVA 게시. */
	io_write64_twopart(used_addr, &hw->common_cfg->queue_used_lo,
			   &hw->common_cfg->queue_used_hi);   /* [한국어] used IOVA 게시. */

	notify_off = spdk_mmio_read_2(&hw->common_cfg->queue_notify_off); /* [한국어] 이 큐의 notify 슬롯 인덱스. */
	vq->notify_addr = (void *)((uint8_t *)hw->notify_base +
				   notify_off * hw->notify_off_multiplier);
	/* [한국어] 실제 doorbell 주소: notify_base + off * multiplier (스펙 §4.1.4.4). */

	spdk_mmio_write_2(&hw->common_cfg->queue_enable, 1); /* [한국어] 큐 활성화 — 디바이스가 이제 desc/avail 폴링 시작. */
	g_thread_virtio_hw = NULL;

	SPDK_DEBUGLOG(virtio_pci, "queue %"PRIu16" addresses:\n", vq->vq_queue_index);
	SPDK_DEBUGLOG(virtio_pci, "\t desc_addr: %" PRIx64 "\n", desc_addr);
	SPDK_DEBUGLOG(virtio_pci, "\t aval_addr: %" PRIx64 "\n", avail_addr);
	SPDK_DEBUGLOG(virtio_pci, "\t used_addr: %" PRIx64 "\n", used_addr);
	SPDK_DEBUGLOG(virtio_pci, "\t notify addr: %p (notify offset: %"PRIu16")\n",
		      vq->notify_addr, notify_off);

	return 0;
}

/*
 * [한국어]
 * modern_del_queue - 큐 비활성화 + IOVA 클리어 + 메모리 해제
 *
 * @dev: 대상
 * @vq : 해제할 큐
 *
 * setup_queue 역순:
 *   1) queue_select로 큐 선택
 *   2) desc/avail/used IOVA를 0으로 → 디바이스가 리퍼런스 못 잡게
 *   3) queue_enable = 0
 *   4) hugepage 해제
 *
 * 호출 체인:
 *   virtio_free_queues → backend_ops->del_queue(=[modern_del_queue])
 */
static void
modern_del_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	struct virtio_hw *hw = dev->ctx;

	g_thread_virtio_hw = hw;
	spdk_mmio_write_2(&hw->common_cfg->queue_select, vq->vq_queue_index); /* [한국어] 대상 큐 선택. */

	io_write64_twopart(0, &hw->common_cfg->queue_desc_lo,
			   &hw->common_cfg->queue_desc_hi);   /* [한국어] desc IOVA = 0 — 디바이스 참조 끊기. */
	io_write64_twopart(0, &hw->common_cfg->queue_avail_lo,
			   &hw->common_cfg->queue_avail_hi);
	io_write64_twopart(0, &hw->common_cfg->queue_used_lo,
			   &hw->common_cfg->queue_used_hi);

	spdk_mmio_write_2(&hw->common_cfg->queue_enable, 0); /* [한국어] 큐 비활성. */
	g_thread_virtio_hw = NULL;

	spdk_free(vq->vq_ring_virt_mem); /* [한국어] hugepage 해제 (DPDK rte_free). */
}

/*
 * [한국어]
 * modern_notify_queue - doorbell write (PCI 통지)
 *
 * @dev: 대상
 * @vq : 통지할 큐
 *
 * notify_addr는 setup_queue에서 계산한 큐별 16-bit MMIO 슬롯. 여기에 큐 인덱스를 write하면
 * 디바이스가 새 avail entry가 있다는 신호로 인식 (virtio 스펙 §4.1.5.2).
 *
 * 실행 컨텍스트: 큐 owner_thread (req_flush 경로).
 *
 * 호출 체인:
 *   virtqueue_req_flush → backend_ops->notify_queue(=[modern_notify_queue])
 */
static void
modern_notify_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	g_thread_virtio_hw = dev->ctx;  /* [한국어] SIGBUS 핸들러 가드. */
	spdk_mmio_write_2(vq->notify_addr, vq->vq_queue_index); /* [한국어] doorbell write — 디바이스 깨움. */
	g_thread_virtio_hw = NULL;
}

/*
 * [한국어]
 * modern_ops - virtio modern PCI 트랜스포트 콜백 테이블
 *
 * virtio.c가 backend_ops로 호출하는 모든 콜백을 modern_*에 매핑.
 * legacy_ops는 SPDK가 지원하지 않으므로 별도 테이블 없음.
 */
static const struct virtio_dev_ops modern_ops = {
	.read_dev_cfg	= modern_read_dev_config,    /* [한국어] dev_cfg 읽기 (generation 검증). */
	.write_dev_cfg	= modern_write_dev_config,    /* [한국어] dev_cfg 쓰기. */
	.get_status	= modern_get_status,          /* [한국어] status 비트 read. */
	.set_status	= modern_set_status,          /* [한국어] status 비트 write. */
	.get_features	= modern_get_features,        /* [한국어] 디바이스 피처 64-bit read. */
	.set_features	= modern_set_features,        /* [한국어] 협상 피처 64-bit write. */
	.destruct_dev	= modern_destruct_dev,        /* [한국어] 자원 해제. */
	.get_queue_size	= modern_get_queue_size,      /* [한국어] 큐 크기 query. */
	.setup_queue	= modern_setup_queue,         /* [한국어] vring 메모리 + IOVA 게시. */
	.del_queue	= modern_del_queue,           /* [한국어] 큐 비활성 + 해제. */
	.notify_queue	= modern_notify_queue,        /* [한국어] doorbell. */
	.dump_json_info = pci_dump_json_info,         /* [한국어] RPC 응답 — 디바이스 정보. */
	.write_json_config = pci_write_json_config,   /* [한국어] config save — trtype/traddr. */
};

/*
 * [한국어]
 * get_cfg_addr - virtio capability의 (bar, offset)을 가상 주소로 변환
 *
 * @hw : 대상
 * @cap: 발견된 PCI capability
 * @return: 매핑된 가상 주소 또는 NULL(에러)
 *
 * 검증:
 *   - bar가 0..5 범위
 *   - offset+length 정수 오버플로 없음
 *   - offset+length가 BAR 길이 내
 *   - BAR가 매핑되어 있음
 *
 * 호출 체인:
 *   virtio_read_caps → [get_cfg_addr]
 */
static void *
get_cfg_addr(struct virtio_hw *hw, struct virtio_pci_cap *cap)
{
	uint8_t  bar    = cap->bar;       /* [한국어] capability가 가리키는 BAR 번호. */
	uint32_t length = cap->length;     /* [한국어] capability 영역 길이. */
	uint32_t offset = cap->offset;     /* [한국어] BAR 내 시작 오프셋. */

	if (bar > 5) {
		SPDK_ERRLOG("invalid bar: %"PRIu8"\n", bar);  /* [한국어] PCI BAR는 0..5. */
		return NULL;
	}

	if (offset + length < offset) { /* [한국어] uint32 wrap-around 검사 — 악성/손상 capability 방어. */
		SPDK_ERRLOG("offset(%"PRIu32") + length(%"PRIu32") overflows\n",
			    offset, length);
		return NULL;
	}

	if (offset + length > hw->pci_bar[bar].len) { /* [한국어] BAR 범위 밖 접근 방지. */
		SPDK_ERRLOG("invalid cap: overflows bar space: %"PRIu32" > %"PRIu32"\n",
			    offset + length, hw->pci_bar[bar].len);
		return NULL;
	}

	if (hw->pci_bar[bar].vaddr == NULL) { /* [한국어] BAR 매핑 실패 — capability 사용 불가. */
		SPDK_ERRLOG("bar %"PRIu8" base addr is NULL\n", bar);
		return NULL;
	}

	return hw->pci_bar[bar].vaddr + offset; /* [한국어] vaddr + offset = 영역 가상 주소. */
}

/*
 * [한국어]
 * virtio_read_caps - PCI capability list를 따라가며 virtio 4종 capability 발견
 *
 * @hw: 대상 hw
 * @return: 0 성공, 음수 errno
 *
 * PCI 표준: config space[0x34]가 첫 capability ptr. 이를 따라 next 체인을 순회.
 * 각 capability의 cap_vndr이 0x09(VENDOR)이면 virtio capability이고 cfg_type이
 * COMMON_CFG/NOTIFY_CFG/DEVICE_CFG/ISR_CFG 중 하나. 4가지 모두 발견해야 modern.
 * MSI-X capability는 0x11이며 발견 시 use_msix = 1만 마킹 (인터럽트 모드 미사용).
 *
 * 실행 컨텍스트: probe 단일 thread.
 *
 * 호출 체인:
 *   virtio_pci_dev_probe → [virtio_read_caps] → spdk_pci_device_cfg_read / get_cfg_addr
 */
static int
virtio_read_caps(struct virtio_hw *hw)
{
	uint8_t pos;                /* [한국어] capability 체인 진행 위치 (config space offset). */
	struct virtio_pci_cap cap;  /* [한국어] 현재 읽은 cap. */
	int ret;                    /* [한국어] cfg_read 결과. */

	ret = spdk_pci_device_cfg_read(hw->pci_dev, &pos, 1, PCI_CAPABILITY_LIST); /* [한국어] 첫 cap ptr (1바이트). */
	if (ret < 0) {
		SPDK_DEBUGLOG(virtio_pci, "failed to read pci capability list\n");
		return ret;
	}

	while (pos) {  /* [한국어] 0이면 체인 끝. */
		ret = spdk_pci_device_cfg_read(hw->pci_dev, &cap, sizeof(cap), pos); /* [한국어] cap 구조 전체 read. */
		if (ret < 0) {
			SPDK_ERRLOG("failed to read pci cap at pos: %"PRIx8"\n", pos);
			break;
		}

		if (cap.cap_vndr == PCI_CAP_ID_MSIX) { /* [한국어] MSI-X 발견 — 정보용 마킹만. */
			hw->use_msix = 1;
		}

		if (cap.cap_vndr != PCI_CAP_ID_VNDR) { /* [한국어] vendor cap이 아니면 virtio cap도 아님 — 스킵. */
			SPDK_DEBUGLOG(virtio_pci,
				      "[%2"PRIx8"] skipping non VNDR cap id: %02"PRIx8"\n",
				      pos, cap.cap_vndr);
			goto next;
		}

		SPDK_DEBUGLOG(virtio_pci,
			      "[%2"PRIx8"] cfg type: %"PRIu8", bar: %"PRIu8", offset: %04"PRIx32", len: %"PRIu32"\n",
			      pos, cap.cfg_type, cap.bar, cap.offset, cap.length);

		switch (cap.cfg_type) {  /* [한국어] virtio cap의 4종 분기 (스펙 §4.1.4). */
		case VIRTIO_PCI_CAP_COMMON_CFG:
			hw->common_cfg = get_cfg_addr(hw, &cap); /* [한국어] common configuration. */
			break;
		case VIRTIO_PCI_CAP_NOTIFY_CFG:
			spdk_pci_device_cfg_read(hw->pci_dev, &hw->notify_off_multiplier,
						 4, pos + sizeof(cap)); /* [한국어] cap 뒤 4바이트가 multiplier. */
			hw->notify_base = get_cfg_addr(hw, &cap);   /* [한국어] notify 영역 시작. */
			break;
		case VIRTIO_PCI_CAP_DEVICE_CFG:
			hw->dev_cfg = get_cfg_addr(hw, &cap);  /* [한국어] device-specific config. */
			break;
		case VIRTIO_PCI_CAP_ISR_CFG:
			hw->isr = get_cfg_addr(hw, &cap);      /* [한국어] ISR(인터럽트 상태) 레지스터. */
			break;
		}

next:
		pos = cap.cap_next; /* [한국어] 다음 cap으로 진행. */
	}

	if (hw->common_cfg == NULL || hw->notify_base == NULL ||
	    hw->dev_cfg == NULL    || hw->isr == NULL) {
		/* [한국어] 4종 모두 발견 못하면 modern 아님 — legacy로 추정 (SPDK 미지원). */
		SPDK_DEBUGLOG(virtio_pci, "no modern virtio pci device found.\n");
		if (ret < 0) {
			return ret;
		} else {
			return -EINVAL;
		}
	}

	SPDK_DEBUGLOG(virtio_pci, "found modern virtio pci device.\n");

	SPDK_DEBUGLOG(virtio_pci, "common cfg mapped at: %p\n", hw->common_cfg);
	SPDK_DEBUGLOG(virtio_pci, "device cfg mapped at: %p\n", hw->dev_cfg);
	SPDK_DEBUGLOG(virtio_pci, "isr cfg mapped at: %p\n", hw->isr);
	SPDK_DEBUGLOG(virtio_pci, "notify base: %p, notify off multiplier: %u\n",
		      hw->notify_base, hw->notify_off_multiplier);

	return 0;
}

/*
 * [한국어]
 * virtio_pci_dev_probe - 단일 PCI 디바이스 probe (BAR 매핑 + caps + enum_cb 호출)
 *
 * @pci_dev: 후보 PCI 디바이스
 * @ctx    : enum_cb + 사용자 컨텍스트
 * @return: 0 성공, -1 실패 (스킵)
 *
 * 동작:
 *   1) BDF 문자열 변환 (디버그용)
 *   2) virtio_hw calloc
 *   3) BAR 0~5 모두 spdk_pci_device_map_bar로 매핑
 *   4) virtio_read_caps로 4종 cap 검증 (실패 = legacy → 거절)
 *   5) ctx->enum_cb 호출 → 상위 모듈이 virtio_dev 만들고 hw->vdev 연결
 *   6) g_sigset이 false면 SIGBUS 핸들러 등록 (한 번만)
 *   7) g_virtio_hws에 hw 추가
 *
 * 실행 컨텍스트: spdk_pci_enumerate 직렬 — 단일 thread.
 *
 * 호출 체인:
 *   virtio_pci_dev_enumerate / attach → virtio_pci_dev_probe_cb → [virtio_pci_dev_probe]
 */
static int
virtio_pci_dev_probe(struct spdk_pci_device *pci_dev, struct virtio_pci_probe_ctx *ctx)
{
	struct virtio_hw *hw;        /* [한국어] 새 hw 컨텍스트. */
	uint8_t *bar_vaddr;          /* [한국어] BAR 매핑 결과 VA. */
	uint64_t bar_paddr, bar_len; /* [한국어] BAR 물리 주소(미사용)와 길이. */
	int rc;
	unsigned i;
	char bdf[32];                /* [한국어] BDF 문자열. */
	struct spdk_pci_addr addr;   /* [한국어] PCI 주소. */

	addr = spdk_pci_device_get_addr(pci_dev);
	rc = spdk_pci_addr_fmt(bdf, sizeof(bdf), &addr);
	if (rc != 0) {
		SPDK_ERRLOG("Ignoring a device with non-parseable PCI address\n");
		return -1;
	}

	hw = calloc(1, sizeof(*hw)); /* [한국어] 모든 BAR vaddr를 NULL로 초기화. */
	if (hw == NULL) {
		SPDK_ERRLOG("%s: calloc failed\n", bdf);
		return -1;
	}

	hw->pci_dev = pci_dev;

	for (i = 0; i < 6; ++i) {
		rc = spdk_pci_device_map_bar(pci_dev, i, (void *) &bar_vaddr, &bar_paddr,
					     &bar_len);
		/* [한국어] DPDK rte_pci_map_device 위에서 mmap된 BAR 영역 — 가상 주소 + 길이 출력. */
		if (rc != 0) {
			SPDK_ERRLOG("%s: failed to memmap PCI BAR %u\n", bdf, i);
			free_virtio_hw(hw);
			return -1;
		}

		hw->pci_bar[i].vaddr = bar_vaddr;  /* [한국어] BAR 슬롯에 저장. */
		hw->pci_bar[i].len = bar_len;
	}

	/* Virtio PCI caps exist only on modern PCI devices.
	 * Legacy devices are not supported.
	 */
	/* [한국어] modern cap 4종 발견 못하면 거절 — SPDK는 legacy 미지원. */
	if (virtio_read_caps(hw) != 0) {
		SPDK_NOTICELOG("Ignoring legacy PCI device at %s\n", bdf);
		free_virtio_hw(hw);
		return -1;
	}

	rc = ctx->enum_cb((struct virtio_pci_ctx *)hw, ctx->enum_ctx);
	/* [한국어] 상위 모듈에게 hw 통보 — 모듈이 virtio_pci_dev_init으로 virtio_dev 연결. */
	if (rc != 0) {
		free_virtio_hw(hw);
		return rc;
	}

	if (g_sigset != true) {
		spdk_pci_register_error_handler(virtio_pci_dev_sigbus_handler,
						NULL);
		/* [한국어] SIGBUS 핸들러 등록 — BAR remap을 위해. 첫 디바이스에서만 한 번. */
		g_sigset = true;
	}

	pthread_mutex_lock(&g_hw_mutex);
	TAILQ_INSERT_TAIL(&g_virtio_hws, hw, tailq);  /* [한국어] 전역 리스트에 등록 — sigbus handler가 볼 수 있게. */
	pthread_mutex_unlock(&g_hw_mutex);

	return 0;
}

/*
 * [한국어]
 * virtio_pci_dev_probe_cb - spdk_pci_enumerate 콜백 (PCI ID 매칭 → probe)
 *
 * @probe_ctx: virtio_pci_probe_ctx*
 * @pci_dev  : 후보 디바이스
 * @return: 0=accept, 1=skip(다른 device_id), 그 외 = probe 결과
 *
 * PCI device_id 0x1000~0x107F가 virtio 영역 (Red Hat 할당). transitional/modern 변환은
 * virtio_pci_dev_check와 동일.
 *
 * 호출 체인:
 *   spdk_pci_enumerate (DPDK) → [virtio_pci_dev_probe_cb] → virtio_pci_dev_probe
 */
static int
virtio_pci_dev_probe_cb(void *probe_ctx, struct spdk_pci_device *pci_dev)
{
	struct virtio_pci_probe_ctx *ctx = probe_ctx;
	uint16_t pci_device_id = spdk_pci_device_get_device_id(pci_dev); /* [한국어] PCI device_id read. */
	uint16_t device_id;                                              /* [한국어] virtio 의미 변환. */

	if (pci_device_id < 0x1000 || pci_device_id > 0x107f) {
		SPDK_ERRLOG("Probe device is not a virtio device\n"); /* [한국어] 이 driver의 PCI ID 테이블에서 들어왔어야 함. */
		return 1;
	}

	if (pci_device_id < 0x1040) {
		/* Transitional devices: use the PCI subsystem device id as
		 * virtio device id, same as legacy driver always did.
		 */
		device_id = spdk_pci_device_get_subdevice_id(pci_dev);
	} else {
		/* Modern devices: simply use PCI device id, but start from 0x1040. */
		device_id = pci_device_id - 0x1040;
	}

	if (device_id != ctx->device_id) {
		return 1;  /* [한국어] 관심 device_id와 다름 — 스킵. */
	}

	return virtio_pci_dev_probe(pci_dev, ctx);  /* [한국어] 매칭 → 본격 probe. */
}

/*
 * [한국어]
 * virtio_pci_dev_enumerate - 시스템의 모든 PCI에서 device_id 매칭 디바이스 enum
 *
 * @enum_cb       : 매칭 디바이스마다 호출할 콜백
 * @enum_ctx      : 콜백 컨텍스트
 * @pci_device_id : 관심 virtio device_id (예: VIRTIO_ID_BLOCK)
 * @return: spdk_pci_enumerate 결과
 *
 * secondary process(예: DPDK secondary)는 미지원 — primary만.
 *
 * 호출 체인:
 *   bdev_virtio init → [virtio_pci_dev_enumerate] → spdk_pci_enumerate → virtio_pci_dev_probe_cb
 */
int
virtio_pci_dev_enumerate(virtio_pci_create_cb enum_cb, void *enum_ctx,
			 uint16_t pci_device_id)
{
	struct virtio_pci_probe_ctx ctx;  /* [한국어] 트램펄린 컨텍스트. */

	if (!spdk_process_is_primary()) {  /* [한국어] DPDK primary 프로세스만 PCI map 가능. */
		SPDK_WARNLOG("virtio_pci secondary process support is not implemented yet.\n");
		return 0;
	}

	ctx.enum_cb = enum_cb;
	ctx.enum_ctx = enum_ctx;
	ctx.device_id = pci_device_id;

	return spdk_pci_enumerate(spdk_pci_virtio_get_driver(),
				  virtio_pci_dev_probe_cb, &ctx);
	/* [한국어] DPDK가 등록된 virtio PCI ID들 순회 — 매칭마다 probe_cb 호출. */
}

/*
 * [한국어]
 * virtio_pci_dev_attach - 특정 PCI 주소의 단일 디바이스 attach
 *
 * @enum_cb    : 콜백
 * @enum_ctx   : 콜백 컨텍스트
 * @device_id  : 매칭 virtio device_id
 * @pci_address: 정확한 BDF
 * @return: spdk_pci_device_attach 결과
 *
 * RPC bdev_virtio_attach 등에서 사용 — 정확한 BDF로 한 디바이스만 attach.
 *
 * 호출 체인:
 *   RPC handler → [virtio_pci_dev_attach] → spdk_pci_device_attach → virtio_pci_dev_probe_cb
 */
int
virtio_pci_dev_attach(virtio_pci_create_cb enum_cb, void *enum_ctx,
		      uint16_t device_id, struct spdk_pci_addr *pci_address)
{
	struct virtio_pci_probe_ctx ctx;

	if (!spdk_process_is_primary()) {
		SPDK_WARNLOG("virtio_pci secondary process support is not implemented yet.\n");
		return 0;
	}

	ctx.enum_cb = enum_cb;
	ctx.enum_ctx = enum_ctx;
	ctx.device_id = device_id;

	return spdk_pci_device_attach(spdk_pci_virtio_get_driver(),
				      virtio_pci_dev_probe_cb, &ctx, pci_address);
}

/*
 * [한국어]
 * virtio_pci_dev_init - virtio_dev에 PCI 백엔드 ops 바인딩 (enum_cb 내부에서 호출)
 *
 * @vdev   : 호출자(상위 모듈)가 할당한 virtio_dev
 * @name   : 디바이스 이름
 * @pci_ctx: virtio_pci_dev_probe가 enum_cb에 넘긴 hw (실제 struct virtio_hw*)
 * @return: 0 성공, virtio_dev_construct 결과
 *
 * 동작:
 *   1) virtio_dev_construct로 vdev 기본 초기화 (mutex, name, ops=modern_ops, ctx=hw)
 *   2) is_hw=1 (vtophys 사용 표시), modern=1 (legacy 아님) flag set
 *   3) hw->vdev = vdev로 백포인터 연결 (sigbus handler에서 hw->vdev->name 사용)
 *
 * 실행 컨텍스트: enum_cb의 호출 흐름 — 단일 thread.
 *
 * 호출 체인:
 *   bdev_virtio enum_cb → [virtio_pci_dev_init] → virtio_dev_construct
 */
int
virtio_pci_dev_init(struct virtio_dev *vdev, const char *name,
		    struct virtio_pci_ctx *pci_ctx)
{
	int rc;
	struct virtio_hw *hw = (struct virtio_hw *)pci_ctx;  /* [한국어] enum_cb에 넘긴 hw 캐스팅. */

	rc = virtio_dev_construct(vdev, name, &modern_ops, pci_ctx);
	if (rc != 0) {
		return rc;
	}

	vdev->is_hw = 1;    /* [한국어] HW 트랜스포트 → add_iovs에서 spdk_vtophys 사용. */
	vdev->modern = 1;   /* [한국어] modern virtio 1.x. */
	hw->vdev = vdev;    /* [한국어] 백포인터 — sigbus handler가 이름 회수 시 사용. */

	return 0;
}

/* [한국어] SPDK 로그 컴포넌트 등록 — `--logflag virtio_pci`로 활성화. */
SPDK_LOG_REGISTER_COMPONENT(virtio_pci)
