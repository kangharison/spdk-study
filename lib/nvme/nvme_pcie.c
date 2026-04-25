/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2017, IBM Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 */

/*
 * NVMe over PCIe transport
 */

/*
 * [한국어 설명] PCIe 트랜스포트 attach/probe/BAR 매핑 구현 (nvme_pcie.c)
 *
 * === 파일의 역할 ===
 * SPDK NVMe 드라이버의 **PCIe 트랜스포트 vtable의 cold-path 구현**. hot-path(submit/complete/
 * doorbell/PRP)는 nvme_pcie_common.c에, 이 파일은 "장치 발견 → BAR 매핑 → 컨트롤러 생성 →
 * 컨트롤러 활성화 → hotplug 감지 → 정리"의 생애 초반·후반 로직을 담당한다.
 *
 * 5개 주요 책임:
 *   1) **Probe/Enumerate**: `nvme_pcie_ctrlr_scan`이 DPDK PCI enumeration으로 NVMe 장치 발견 →
 *      `pcie_nvme_enum_cb`가 각 장치에 대해 attach 여부 판단 → probe_ctx에 컨트롤러 추가.
 *   2) **BAR 매핑**: `nvme_pcie_ctrlr_allocate_bars`가 BAR0를 mmap하여 `pctrlr->regs`에 저장 →
 *      이후 모든 MMIO 레지스터 접근(CAP/CC/CSTS/ASQ/ACQ 등)이 이 포인터 기반으로 이뤄짐.
 *      doorbell_base 계산(BAR0 + 0x1000 + stride) → nvme_pcie_common.c의 sq_tdbl/cq_hdbl 세팅의 근거.
 *   3) **Register MMIO 접근**: 32/64비트 레지스터 R/W의 트랜스포트 vtable 구현. PCIe는 `spdk_mmio_*`
 *      유틸로 직접 BAR에 read/write. `g_thread_mmio_ctrlr` TLS 마커를 접근 전후에 설정 →
 *      SIGBUS 시그널 핸들러가 어느 컨트롤러에서 오류가 났는지 알 수 있도록.
 *   4) **SIGBUS 방어**: PCIe link loss 중 MMIO read 접근 시 SIGBUS 발생 → `nvme_sigbus_fault_sighandler`가
 *      해당 BAR 영역을 anonymous 메모리로 remap하여 0xFF(all-ones = 보통 invalid state)를 채움 →
 *      이후 레지스터 read는 실패 코드를 반환하지만 프로세스는 살아남음. 견고한 hotplug 대응.
 *   5) **CMB/PMR 관리**: `map_cmb/unmap_cmb/reserve_cmb` + PMR 6종. CMB/PMR은 장치 내부 메모리로
 *      SQ/CQ/데이터 버퍼를 배치해 레이턴시 감소 및 내구성 향상에 사용. CMBLOC/CMBSZ/PMRCAP 레지스터
 *      해석 + BAR 매핑.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 **PCIe 트랜스포트 ops 테이블(`pcie_ops`)의 소유자**이자 등록자. 파일 맨 아래의
 * `SPDK_NVME_TRANSPORT_REGISTER(pcie, &pcie_ops)` 매크로가 main() 진입 전에 nvme_transport.c의
 * 전역 TAILQ에 PCIe ops를 삽입 → 이후 상위 레이어(nvme_transport_*)가 이 파일의 함수들을
 * vtable로 호출.
 *
 * 호출 체인 (probe → attach):
 *   [Application]
 *     → spdk_nvme_probe / spdk_nvme_connect
 *     → nvme_transport_ctrlr_scan [nvme_transport.c]
 *         → ops.ctrlr_scan = nvme_pcie_ctrlr_scan [이 파일]
 *             → spdk_pci_enumerate(pcie_nvme_enum_cb)
 *                 → pcie_nvme_enum_cb ★ (각 PCI 장치마다 호출)
 *                     → nvme_ctrlr_probe (검증 + 필터)
 *                         → nvme_transport_ctrlr_construct
 *                             → ops.ctrlr_construct (nvme_pcie.c 내부 헬퍼)
 *                                 → nvme_pcie_ctrlr_allocate_bars ★ (BAR mmap)
 *                                     → spdk_pci_device_map_bar(bar=0)
 *                                     → pctrlr->regs = BAR0 가상 주소
 *                                     → pctrlr->doorbell_base = &regs->doorbell[0]
 *                                 → nvme_ctrlr_process_init (상태머신 진입)
 *                                     → CC/CSTS register 조작 — 이 파일의 set/get_reg_4 호출
 *
 * 호출 체인 (hotplug remove):
 *   [Background kernel event]
 *     → uevent fd readable
 *     → _nvme_pcie_hotplug_monitor (poll_group/reactor가 주기 호출)
 *         → spdk_pci_get_event → SPDK_UEVENT_REMOVE 감지
 *             → _nvme_pcie_event_process
 *                 → nvme_ctrlr_fail + ctrlr->remove_cb
 *
 * === 타 모듈과의 연결 ===
 *   - `nvme_pcie_internal.h`  — struct nvme_pcie_ctrlr 정의 (regs, doorbell_base, cmb/pmr 정보)
 *   - `nvme_pcie_common.c`    — hot-path (submit/complete/doorbell). 이 파일은 cold-path.
 *   - `nvme_transport.c`      — vtable dispatch. 이 파일의 pcie_ops 테이블이 등록 대상.
 *   - `nvme_ctrlr.c`          — 상위 컨트롤러 수명주기 (process_init 상태머신).
 *   - `spdk/env.h` (spdk_pci_*) — DPDK PCI 열거/BAR mmap/hotplug 이벤트 API.
 *   - 전역 `g_thread_mmio_ctrlr` (nvme_pcie_common.c 정의) — 이 파일의 SIGBUS 핸들러가 참조.
 *
 * === 주요 함수/구조체 요약 ===
 *   ★ nvme_pcie_ctrlr_scan        — DPDK enumerate로 장치 탐색 → pcie_nvme_enum_cb 호출
 *   ★ pcie_nvme_enum_cb           — 각 PCI 장치마다 probe 판단 (traddr 필터, enum_ctx 분기)
 *   ★ nvme_pcie_ctrlr_allocate_bars — BAR0 mmap + doorbell_base 계산
 *     nvme_pcie_ctrlr_set/get_reg_4/8 — MMIO register R/W (TLS marker + spdk_mmio 유틸)
 *     nvme_sigbus_fault_sighandler   — PCIe link loss 시 BAR 영역 remap으로 프로세스 보호
 *     _nvme_pcie_event_process + hotplug_monitor — SPDK_UEVENT_REMOVE 감지 + 컨트롤러 fail
 *     nvme_pcie_ctrlr_map_cmb + map_pmr — 장치 내부 메모리 BAR 매핑
 *     nvme_pcie_ctrlr_enable         — CC.EN=1 + shadow_doorbell 설정
 *     nvme_pcie_ctrlr_destruct       — BAR unmap + pctrlr free
 *     ★ pcie_ops 테이블              — 파일 맨 아래, 트랜스포트 vtable 전체 매핑
 *     SPDK_NVME_TRANSPORT_REGISTER(pcie, &pcie_ops) — 전역 TAILQ에 등록
 *
 * 관련 전역:
 *   g_signal_lock      : SIGBUS 핸들러 재진입 방지 atomic flag
 *   g_sigset           : 시그널 핸들러 등록 여부 (중복 등록 방지)
 *   g_hotplug_filter_cb: 사용자 제공 hotplug 필터 (특정 traddr만 허용)
 *
 * 이 파일을 이해하면 SPDK NVMe가 "어떻게 Linux PCIe 장치를 발견하고 유저스페이스에 끌어오는지",
 * 그리고 "link loss나 hotplug 시 어떻게 견고하게 버티는지"가 드러난다.
 */

#include "spdk/stdinc.h"          /* [한국어] 표준 라이브러리 (mmap, memset, errno 등) */
#include "spdk/env.h"             /* [한국어] DPDK 추상화 — spdk_pci_device, spdk_pci_enumerate,
                                   *         spdk_pci_device_map_bar 등 PCI 열거/BAR 매핑 API */
#include "spdk/likely.h"          /* [한국어] spdk_likely/unlikely */
#include "spdk/string.h"          /* [한국어] spdk_strerror */
#include "nvme_internal.h"        /* [한국어] NVMe 내부 타입 — spdk_nvme_ctrlr, probe_ctx, driver 등 */
#include "nvme_pcie_internal.h"   /* [한국어] PCIe 특화 — nvme_pcie_ctrlr 구조체 (regs, devhandle, cmb/pmr 등) */

/*
 * [한국어] nvme_pcie_enum_ctx - probe 중 enumerate 콜백에 전달되는 사용자 컨텍스트.
 *
 * 필드:
 *   - probe_ctx:    사용자 probe 요청 정보 (filter_cb, attach_cb 등)
 *   - pci_addr:     특정 traddr만 probe하려고 할 때 지정된 PCI 주소
 *   - has_pci_addr: 위 필드 유효성 플래그 (false면 전체 enumerate)
 */
struct nvme_pcie_enum_ctx {
	struct spdk_nvme_probe_ctx *probe_ctx;
                                  /* [한국어] 상위 probe 요청 컨텍스트 (콜백 + 필터) */
	struct spdk_pci_addr pci_addr;
                                  /* [한국어] 특정 traddr로 제한된 probe의 대상 주소 */
	bool has_pci_addr;
                                  /* [한국어] true면 pci_addr만 대상, false면 모든 PCI NVMe 장치 */
};

static uint16_t g_signal_lock;
                                  /* [한국어] SIGBUS 핸들러 재진입 방지 락 (atomic CAS로 보호).
                                   *         시그널 핸들러는 async-signal-safe 제약 때문에 일반 mutex 못 씀 → atomic flag. */
static bool g_sigset = false;
                                  /* [한국어] SIGBUS 핸들러가 이미 등록되었는지 (중복 sigaction 호출 방지) */
static spdk_nvme_pcie_hotplug_filter_cb g_hotplug_filter_cb;
                                  /* [한국어] 사용자 지정 hotplug 필터. traddr이 주어졌을 때 allow/deny 판단.
                                   *         NULL이면 모든 장치 허용 (기본). spdk_nvme_pcie_set_hotplug_filter로 설정. */

/*
 * [한국어] ★ nvme_sigbus_fault_sighandler - PCIe link loss 중 MMIO 접근 시 SIGBUS 방어.
 *
 * PCIe 장치가 물리적으로 제거되거나 link down되면 BAR로 매핑된 가상 주소에 MMIO read를 하면
 * SIGBUS 시그널이 발생 → 기본 핸들러는 프로세스 crash. SPDK는 이 핸들러로 crash 대신
 * **BAR 영역을 anonymous 메모리로 remap하여 0xFF로 채움** → 후속 MMIO read는 실패 상태를
 * 반환(all-ones는 보통 invalid state)하지만 프로세스는 살아남고 reset/cleanup 경로로 진행.
 *
 * 재진입 안전성:
 *   - 시그널 핸들러는 async-signal-safe 함수만 호출 가능 — mutex 대신 atomic flag.
 *   - `__atomic_compare_exchange_n`로 획득, 실패 시 조기 리턴 (다른 스레드가 처리 중).
 *
 * 어느 컨트롤러가 SIGBUS 낸지 식별: `g_thread_mmio_ctrlr` TLS 마커 — 호출 직전 세팅, 후 NULL.
 * 시그널은 오류 낸 스레드로 전달되므로 TLS로 정확히 특정 가능.
 *
 * 한 번 remap하면 is_remapped=true로 표시 → 재진입 시 remap 건너뜀 (이미 가상 메모리로 교체됨).
 */
static void
nvme_sigbus_fault_sighandler(const void *failure_addr, void *ctx)
{
	void *map_address;
                                  /* [한국어] mmap 반환 주소 (remap 성공 여부 확인) */
	uint16_t flag = 0;
                                  /* [한국어] CAS expected 값 — 0이면 누구도 처리 중 아님 */

	if (!__atomic_compare_exchange_n(&g_signal_lock, &flag, 1, false, __ATOMIC_ACQUIRE,
					 __ATOMIC_RELAXED)) {
                                  /* [한국어] CAS: g_signal_lock이 0이면 1로 atomic 교체.
                                   *         실패 = 다른 스레드가 이미 SIGBUS 처리 중 → 중복 remap 방지 위해 리턴 */
		SPDK_DEBUGLOG(nvme, "request g_signal_lock failed\n");
		return;
	}

	if (g_thread_mmio_ctrlr == NULL) {
                                  /* [한국어] TLS 마커 미세팅 — MMIO 중이 아닌 경로에서 SIGBUS가 온 것 → 이 핸들러가 처리할 것 아님 */
		return;
	}

	if (!g_thread_mmio_ctrlr->is_remapped) {
                                  /* [한국어] 이 컨트롤러의 BAR가 아직 remap 안 됨 — 지금 실행 */
		map_address = mmap((void *)g_thread_mmio_ctrlr->regs, g_thread_mmio_ctrlr->regs_size,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                                  /* [한국어] ★ 기존 BAR 매핑 영역을 anonymous 메모리로 덮어씀 (MAP_FIXED 강제):
                                   *         - MAP_FIXED: 기존 매핑을 atomic 교체 (MUSL/glibc 보장)
                                   *         - MAP_ANONYMOUS: BAR가 아닌 일반 RAM 페이지
                                   *         이후 이 주소로의 read/write는 SIGBUS 없이 일반 메모리 접근됨 */
		if (map_address == MAP_FAILED) {
                                  /* [한국어] remap 실패 — 드문 경우 (메모리 고갈 등) — 정리 포기 */
			SPDK_ERRLOG("mmap failed\n");
			__atomic_store_n(&g_signal_lock, 0, __ATOMIC_RELEASE);
			return;
		}
		memset(map_address, 0xFF, sizeof(struct spdk_nvme_registers));
                                  /* [한국어] 0xFF로 채움 — NVMe 레지스터가 all-ones일 때 장치 invalid state로 해석됨
                                   *         (CAP=~0, CSTS=~0 등) → 상위 코드가 link loss 감지해 reset 유도 */
		g_thread_mmio_ctrlr->regs = (volatile struct spdk_nvme_registers *)map_address;
                                  /* [한국어] 이제 레지스터 포인터는 anonymous 메모리 가리킴 */
		g_thread_mmio_ctrlr->is_remapped = true;
                                  /* [한국어] remap 완료 표시 — 재진입 시 중복 remap 방지 */
	}
	__atomic_store_n(&g_signal_lock, 0, __ATOMIC_RELEASE);
                                  /* [한국어] 락 해제 (release barrier로 이전 쓰기가 먼저 observed) */
}

/*
 * [한국어] _nvme_pcie_event_process - PCI hotplug 이벤트 하나 처리 (ADD/REMOVE).
 *
 * @event: spdk_pci_event — kernel uevent에서 파싱된 PCI 이벤트
 * @cb_ctx: probe_ctx의 사용자 컨텍스트
 *
 * ADD 이벤트: primary 프로세스에서만 처리 (multi-process 중복 방지)
 *   - filter_cb 통과하면 spdk_pci_device_allow로 enumerate 허용 목록에 추가
 *   - 실제 attach는 다음 enumerate 사이클의 pcie_nvme_enum_cb가 수행
 *
 * REMOVE 이벤트: 모든 프로세스에서 처리
 *   - traddr로 컨트롤러 조회
 *   - ctrlr_fail(true)로 컨트롤러를 failed 상태로 마킹 → 이후 모든 I/O 거부
 *   - ctrlr->remove_cb 사용자 콜백 호출 (애플리케이션이 정리 시작)
 */
static void
_nvme_pcie_event_process(struct spdk_pci_event *event, void *cb_ctx)
{
	struct spdk_nvme_transport_id trid;
                                  /* [한국어] traddr 포맷해서 ctrlr 조회용 trid 구성 */
	struct spdk_nvme_ctrlr *ctrlr;
                                  /* [한국어] 이벤트 대상 컨트롤러 */

	if (event->action == SPDK_UEVENT_ADD) {
                                  /* [한국어] 장치 추가 이벤트 */
		if (spdk_process_is_primary()) {
                                  /* [한국어] primary만 처리 — secondary가 독립적으로 추가하면 중복 */
			if (g_hotplug_filter_cb == NULL || g_hotplug_filter_cb(&event->traddr)) {
                                  /* [한국어] 필터 통과 또는 필터 없음 */
				/* The enumerate interface implement the add operation */
				spdk_pci_device_allow(&event->traddr);
                                  /* [한국어] DPDK에 이 주소 allow 등록 — 다음 spdk_pci_enumerate가 이 장치를 발견 */
			}
		}
	} else if (event->action == SPDK_UEVENT_REMOVE) {
                                  /* [한국어] 장치 제거 이벤트 */
		memset(&trid, 0, sizeof(trid));
		spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
                                  /* [한국어] trtype=PCIE 설정 */

		if (spdk_pci_addr_fmt(trid.traddr, sizeof(trid.traddr), &event->traddr) < 0) {
                                  /* [한국어] PCI 주소를 "0000:81:00.0" 같은 문자열로 포맷 */
			SPDK_ERRLOG("Failed to format pci address\n");
			return;
		}

		ctrlr = nvme_get_ctrlr_by_trid_unsafe(&trid, NULL);
                                  /* [한국어] trid로 등록된 컨트롤러 찾음 — unsafe = 락 없이 (이미 보호된 컨텍스트 가정) */
		if (ctrlr == NULL) {
                                  /* [한국어] 등록 안 된 장치 — 무시 */
			return;
		}
		NVME_CTRLR_DEBUGLOG(ctrlr, "remove\n");

		nvme_ctrlr_lock(ctrlr);
		nvme_ctrlr_fail(ctrlr, true);
                                  /* [한국어] 컨트롤러를 failed 상태로 마킹 (hot_remove=true) — 이후 모든 I/O가 -ENXIO 반환 */
		nvme_ctrlr_unlock(ctrlr);

		/* get the user app to clean up and stop I/O */
		if (ctrlr->remove_cb) {
                                  /* [한국어] 사용자가 remove 콜백 등록했으면 호출 — 애플리케이션이 cleanup 시작 */
			nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
                                  /* [한국어] lock 해제 후 콜백 호출 (콜백이 drivers API 다시 부를 수 있으므로 lock 재귀 회피) */
			ctrlr->remove_cb(ctrlr->cb_ctx, ctrlr);
			nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
                                  /* [한국어] lock 재획득 */
		}
	}
}

/*
 * [한국어] _nvme_pcie_hotplug_monitor - hotplug 이벤트 큐 드레인 + 물리 제거 감지.
 *
 * 두 경로:
 *   1) hotplug_fd가 있으면 (kernel uevent netlink 등) 읽어서 이벤트 파싱 + _nvme_pcie_event_process 호출
 *   2) 등록된 PCIe 컨트롤러 모두 순회 → spdk_pci_device_is_removed 체크 → 물리 제거됐으면 정리 시작
 *
 * 호출자: poll_group/reactor가 주기 호출 (hotplug 지연 감지는 poll 빈도에 의존).
 * 반환값: 0 = 아무것도 제거 안 됨, 1 = 최소 하나 제거됨 (호출자가 후속 re-enumerate 판단에 사용).
 */
static int
_nvme_pcie_hotplug_monitor(struct spdk_nvme_probe_ctx *probe_ctx)
{
	struct spdk_nvme_ctrlr *ctrlr, *tmp;
                                  /* [한국어] FOREACH_SAFE 반복자 (ctrlr 제거 중에도 순회 안전) */
	struct spdk_pci_event event;
	int rc = 0;

	if (g_spdk_nvme_driver->hotplug_fd >= 0) {
                                  /* [한국어] hotplug 모니터링 활성화된 경우만 — driver init 시 netlink 소켓 생성 */
		while (spdk_pci_get_event(g_spdk_nvme_driver->hotplug_fd, &event) > 0) {
                                  /* [한국어] non-blocking으로 큐에 쌓인 이벤트 모두 드레인 */
			_nvme_pcie_event_process(&event, probe_ctx->cb_ctx);
		}
	}

	/* Initiate removal of physically hotremoved PCI controllers. Even after
	 * they're hotremoved from the system, SPDK might still report them via RPC.
	 */
                                  /* [한국어] 일부 환경에서는 uevent 감지가 불안정 — 별도로 모든 컨트롤러의 존재 확인.
                                   *         spdk_pci_device_is_removed는 sysfs 상태 직접 체크. */
	TAILQ_FOREACH_SAFE(ctrlr, &g_spdk_nvme_driver->shared_attached_ctrlrs, tailq, tmp) {
		bool do_remove = false;
		struct nvme_pcie_ctrlr *pctrlr;

		if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
                                  /* [한국어] PCIe가 아닌 컨트롤러(RDMA/TCP/VFIOUSER)는 이 모니터 대상 아님 */
			continue;
		}

		pctrlr = nvme_pcie_ctrlr(ctrlr);
		if (spdk_pci_device_is_removed(pctrlr->devhandle)) {
                                  /* [한국어] 커널이 보고하는 PCI 상태 확인 — 제거된 상태면 do_remove 플래그 세팅 */
			do_remove = true;
			rc = 1;
                                  /* [한국어] 반환값 1 = "최소 하나 제거됨" — 호출자가 후속 처리 */
		}

		if (do_remove) {
                                  /* [한국어] _nvme_pcie_event_process의 REMOVE 경로와 동일 정리 */
			nvme_ctrlr_lock(ctrlr);
			nvme_ctrlr_fail(ctrlr, true);
			nvme_ctrlr_unlock(ctrlr);
			if (ctrlr->remove_cb) {
				nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
				ctrlr->remove_cb(ctrlr->cb_ctx, ctrlr);
				nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
			}
		}
	}
	return rc;
}

/*
 * [한국어] nvme_pcie_reg_addr - offset 레지스터의 MMIO 가상 주소 계산.
 *
 * pctrlr->regs는 BAR0 매핑 기준 시작 주소 (allocate_bars에서 세팅).
 * volatile 반환: 컴파일러가 읽기 순서 재배치하지 않도록 (MMIO 필수).
 */
static volatile void *
nvme_pcie_reg_addr(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	return (volatile void *)((uintptr_t)pctrlr->regs + offset);
                                  /* [한국어] uintptr_t 경유로 포인터 산술 + volatile cast */
}

/*
 * [한국어] nvme_pcie_ctrlr_get_registers - BAR 매핑 시작 주소 반환 (사용자 직접 접근용).
 *
 * `spdk_nvme_ctrlr_get_registers` 공개 API의 PCIe 구현. 사용자가 volatile 포인터로 직접
 * 레지스터 read 가능. 주의: PCIe link가 살아있어야 안전 (dead 상태면 SIGBUS).
 */
static volatile struct spdk_nvme_registers *
nvme_pcie_ctrlr_get_registers(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	return pctrlr->regs;
                                  /* [한국어] BAR0 가상 주소 직접 노출 */
}

/*
 * [한국어] ★ nvme_pcie_ctrlr_set_reg_4 - 32비트 레지스터 MMIO write.
 *
 * 트랜스포트 vtable의 ctrlr_set_reg_4 구현 (nvme_transport.c가 dispatch).
 *
 * TLS 마커 패턴:
 *   g_thread_mmio_ctrlr = pctrlr  ← SIGBUS 핸들러가 어느 컨트롤러인지 알도록 표시
 *   spdk_mmio_write_4(...)         ← 실제 MMIO write (link loss면 여기서 SIGBUS)
 *   g_thread_mmio_ctrlr = NULL    ← 마커 해제 (다른 코드가 SIGBUS 내더라도 엉뚱한 컨트롤러로 오인하지 않도록)
 */
static int
nvme_pcie_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
                                  /* [한국어] 레지스터 구조체 범위 초과 방지 — 버그 조기 포착 */
	g_thread_mmio_ctrlr = pctrlr;
                                  /* [한국어] SIGBUS 시 어느 컨트롤러인지 식별 위한 TLS 마커 세팅 */
	spdk_mmio_write_4(nvme_pcie_reg_addr(ctrlr, offset), value);
                                  /* [한국어] ★ BAR의 offset 위치에 32비트 write (내부적으로 volatile store + memory barrier) */
	g_thread_mmio_ctrlr = NULL;
                                  /* [한국어] 마커 해제 */
	return 0;
}

/*
 * [한국어] nvme_pcie_ctrlr_set_reg_8 - 64비트 레지스터 write (ASQ/ACQ 등).
 */
static int
nvme_pcie_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	g_thread_mmio_ctrlr = pctrlr;
	spdk_mmio_write_8(nvme_pcie_reg_addr(ctrlr, offset), value);
                                  /* [한국어] 64비트 MMIO write — x86은 단일 store, 32비트 플랫폼은 2×32 분할될 수 있음 */
	g_thread_mmio_ctrlr = NULL;
	return 0;
}

/*
 * [한국어] ★ nvme_pcie_ctrlr_get_reg_4 - 32비트 레지스터 MMIO read.
 *
 * 반환값:
 *   0 = 성공
 *   -1 = 읽은 값이 all-ones (~value == 0) → SIGBUS 핸들러가 remap한 invalid state 또는
 *        실제 링크 loss. 상위가 이 값을 감지해 reset 경로로 진입.
 *
 * all-ones 체크가 핵심 link-loss 감지 포인트:
 *   - 정상 NVMe 레지스터는 all-ones인 경우가 없음 (예: CAP, VS, CC 모두 유효 비트 패턴)
 *   - link dead이거나 SIGBUS 후 remap된 영역이면 모든 비트가 1 → -1 반환
 *   - 매우 보수적이지만 확실한 감지 방법
 */
static int
nvme_pcie_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	assert(value != NULL);
	g_thread_mmio_ctrlr = pctrlr;
                                  /* [한국어] SIGBUS 대비 TLS 마커 */
	*value = spdk_mmio_read_4(nvme_pcie_reg_addr(ctrlr, offset));
                                  /* [한국어] ★ BAR에서 32비트 read (volatile load) */
	g_thread_mmio_ctrlr = NULL;
	if (~(*value) == 0) {
                                  /* [한국어] all-ones 감지 = 링크 무효 상태 추정 */
		return -1;
	}

	return 0;
}

/*
 * [한국어] nvme_pcie_ctrlr_get_reg_8 - 64비트 레지스터 read (CAP 등).
 */
static int
nvme_pcie_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	assert(value != NULL);
	g_thread_mmio_ctrlr = pctrlr;
	*value = spdk_mmio_read_8(nvme_pcie_reg_addr(ctrlr, offset));
	g_thread_mmio_ctrlr = NULL;
	if (~(*value) == 0) {
                                  /* [한국어] 64비트 all-ones 감지 */
		return -1;
	}

	return 0;
}

/*
 * [한국어] ★ NVMe 스펙 레지스터 전용 wrappers (ASQ/ACQ/AQA/CMB*/PMR*)
 *
 * 아래 함수들은 모두 set/get_reg_4/8을 호출하는 얇은 wrapper.
 * offsetof로 각 레지스터의 BAR offset 계산 → MMIO R/W.
 *
 * 주요 레지스터 (NVMe Base Spec Section 3):
 *   ASQ    (0x28): Admin SQ Base Address (64b) — admin 큐 물리 주소
 *   ACQ    (0x30): Admin CQ Base Address (64b)
 *   AQA    (0x24): Admin Queue Attributes (32b) — admin SQ/CQ 크기
 *   CMBLOC (0x38): Controller Memory Buffer Location — CMB가 BAR 어느 위치인지
 *   CMBSZ  (0x3C): CMB Size — 크기와 지원 기능(WDS/RDS 등)
 *   PMRCAP (0xE00): Persistent Memory Region Capabilities
 *   PMRCTL (0xE04): PMR Control — PMR enable
 *   PMRSTS (0xE08): PMR Status — PMR 준비 상태
 *   PMRMSCL/MSCU (0xE14/E18): PMR Memory Space Control Lower/Upper — CBA 주소 32b 분할
 */
static int
nvme_pcie_ctrlr_set_asq(struct nvme_pcie_ctrlr *pctrlr, uint64_t value)
{
	return nvme_pcie_ctrlr_set_reg_8(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, asq),
					 value);
                                  /* [한국어] Admin SQ Base Address 64b write — ctrlr reset 초기화 단계에서 호출 */
}

static int
nvme_pcie_ctrlr_set_acq(struct nvme_pcie_ctrlr *pctrlr, uint64_t value)
{
	return nvme_pcie_ctrlr_set_reg_8(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, acq),
					 value);
                                  /* [한국어] Admin CQ Base Address 64b write */
}

static int
nvme_pcie_ctrlr_set_aqa(struct nvme_pcie_ctrlr *pctrlr, const union spdk_nvme_aqa_register *aqa)
{
	return nvme_pcie_ctrlr_set_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, aqa.raw),
					 aqa->raw);
                                  /* [한국어] AQA 32b write — ASQS(SQ 크기)와 ACQS(CQ 크기) 비트필드 포함 */
}

static int
nvme_pcie_ctrlr_get_cmbloc(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_cmbloc_register *cmbloc)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, cmbloc.raw),
					 &cmbloc->raw);
                                  /* [한국어] CMBLOC read — BIR(BAR 번호)과 OFST(BAR 내 offset) 획득 */
}

static int
nvme_pcie_ctrlr_get_cmbsz(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_cmbsz_register *cmbsz)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, cmbsz.raw),
					 &cmbsz->raw);
                                  /* [한국어] CMBSZ read — SQS/CQS/LISTS/RDS/WDS 지원 비트 + 크기 단위(SZU)와 개수(SZ) */
}

static int
nvme_pcie_ctrlr_get_pmrcap(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_pmrcap_register *pmrcap)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, pmrcap.raw),
					 &pmrcap->raw);
                                  /* [한국어] PMRCAP read — PMR 지원 기능 플래그 (RDS/WDS/BIR 등) */
}

static int
nvme_pcie_ctrlr_set_pmrctl(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_pmrctl_register *pmrctl)
{
	return nvme_pcie_ctrlr_set_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, pmrctl.raw),
					 pmrctl->raw);
                                  /* [한국어] PMRCTL write — PMR Enable 비트 세팅 */
}

static int
nvme_pcie_ctrlr_get_pmrctl(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_pmrctl_register *pmrctl)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, pmrctl.raw),
					 &pmrctl->raw);
                                  /* [한국어] PMRCTL read — 현재 enable 상태 확인 */
}

static int
nvme_pcie_ctrlr_get_pmrsts(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_pmrsts_register *pmrsts)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, pmrsts.raw),
					 &pmrsts->raw);
                                  /* [한국어] PMRSTS read — CBAI/NRDY/HSTS 등 상태 비트 */
}

static int
nvme_pcie_ctrlr_set_pmrmscl(struct nvme_pcie_ctrlr *pctrlr, uint32_t value)
{
	return nvme_pcie_ctrlr_set_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, pmrmscl.raw),
					 value);
                                  /* [한국어] PMRMSCL write — Controller Base Address(CBA) 하위 32비트 */
}

static int
nvme_pcie_ctrlr_set_pmrmscu(struct nvme_pcie_ctrlr *pctrlr, uint32_t value)
{
	return nvme_pcie_ctrlr_set_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, pmrmscu),
					 value);
                                  /* [한국어] PMRMSCU write — CBA 상위 32비트 (PMR 전체 주소는 MSCL + MSCU) */
}

/*
 * [한국어] nvme_pcie_ctrlr_get_max_xfer_size - 한 I/O의 최대 전송 크기 (PRP 배열 크기 기반).
 *
 * 계산 근거 (영문 주석 설명):
 *   - 명령이 PRP 2개를 초과하면 PRP list 사용: prp1은 SQE embedded, 나머지는 list.
 *   - list 최대 엔트리 수 = NVME_MAX_PRP_LIST_ENTRIES (nvme_pcie_internal.h 정의)
 *   - 총 페이지 수 = NVME_MAX_PRP_LIST_ENTRIES (prp1 포함, 첫 페이지가 4KB 미정렬일 수 있어
 *     공식적으로 +1 하지 않음 — 보수적 추정).
 *
 * 주의: 실제 장치 MDTS(identify)가 이보다 작으면 상위 레이어가 추가 제한.
 *       이 함수는 "PCIe 트랜스포트의 이론적 최대"만 제공.
 */
static  uint32_t
nvme_pcie_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	/*
	 * For commands requiring more than 2 PRP entries, one PRP will be
	 *  embedded in the command (prp1), and the rest of the PRP entries
	 *  will be in a list pointed to by the command (prp2).  The number
	 *  of PRP entries in the list is defined by
	 *  NVME_MAX_PRP_LIST_ENTRIES.
	 *
	 *  Note that the max xfer size is not (MAX_ENTRIES + 1) * page_size
	 *  because the first PRP entry may not be aligned on a 4KiB
	 *  boundary.
	 */
	return NVME_MAX_PRP_LIST_ENTRIES * ctrlr->page_size;
                                  /* [한국어] 리스트 엔트리 수 × 페이지 크기 (page_size = CAP.MPSMIN 기반, 보통 4KB) */
}

/*
 * [한국어] nvme_pcie_ctrlr_get_max_sges - 한 명령의 최대 SGE 개수.
 *
 * NVME_MAX_SGL_DESCRIPTORS 상수 (nvme_pcie_internal.h 정의) — tracker 구조체 내 SGL 배열 크기로 결정.
 * 현재 SPDK는 단일 SGL segment만 지원하므로 이 값이 실질적 한계.
 */
static uint16_t
nvme_pcie_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	return NVME_MAX_SGL_DESCRIPTORS;
                                  /* [한국어] tracker.u.sgl[] 배열 크기와 동일 */
}

/*
 * [한국어] ★ nvme_pcie_ctrlr_map_cmb - CMB(Controller Memory Buffer) BAR 매핑 (내부 SQ 용).
 *
 * 컨트롤러 construct 단계에서 한 번 호출. SQ를 CMB에 배치하여 PCIe round-trip 감소
 * (nvme_pcie_common.c의 qpair_construct가 opts.use_cmb_sqs 확인 후 이 영역에 SQ 할당).
 *
 * NVMe 스펙 분석 과정:
 *   1) CAP.CMBS 비트 → CMB 기능 지원 여부
 *   2) CMBSZ 레지스터 → 크기 단위(SZU)와 개수(SZ) + 지원 기능(SQS/CQS/WDS/RDS 등)
 *      - unit_size = 2^(12 + 4*SZU) → 4KB/64KB/1MB/16MB/256MB/4GB/64GB 중 하나
 *      - 실제 크기 = unit_size × SZ
 *   3) CMBLOC 레지스터 → BIR(어느 BAR) + OFST(BAR 내 offset)
 *   4) spdk_pci_device_map_bar → 해당 BAR를 가상 주소로 mmap
 *   5) 유효성 검증: BAR 범위 내 + SQS 지원 확인
 *
 * 실패 경로(exit 레이블): CMB 비활성화 + use_cmb_sqs=false로 SQ를 호스트 메모리로 fallback.
 */
static void
nvme_pcie_ctrlr_map_cmb(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc;
	void *addr = NULL;
                                  /* [한국어] BAR mmap 결과 가상 주소 */
	uint32_t bir;
                                  /* [한국어] Base Indicator Register — 어느 BAR(0/2/3/4/5)인지 */
	union spdk_nvme_cmbsz_register cmbsz;
	union spdk_nvme_cmbloc_register cmbloc;
                                  /* [한국어] CMB Size / Location 레지스터 union */
	uint64_t size, unit_size, offset, bar_size = 0, bar_phys_addr = 0;

	if (!pctrlr->regs->cap.bits.cmbs) {
                                  /* [한국어] CAP.CMBS=0 → CMB 미지원 장치 */
		goto exit;
	}

	if (nvme_pcie_ctrlr_get_cmbsz(pctrlr, &cmbsz) ||
	    nvme_pcie_ctrlr_get_cmbloc(pctrlr, &cmbloc)) {
                                  /* [한국어] 레지스터 read 실패 (link 문제 등) */
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "get registers failed\n");
		goto exit;
	}

	if (!cmbsz.bits.sz) {
                                  /* [한국어] CMB 크기 0 — 실질적으로 비활성 */
		goto exit;
	}

	bir = cmbloc.bits.bir;
	/* Values 0 2 3 4 5 are valid for BAR */
                                  /* [한국어] BAR 1은 유효하지 않음 (보통 32/64b BAR의 상위 절반) */
	if (bir > 5 || bir == 1) {
		goto exit;
	}

	/* unit size for 4KB/64KB/1MB/16MB/256MB/4GB/64GB */
	unit_size = (uint64_t)1 << (12 + 4 * cmbsz.bits.szu);
                                  /* [한국어] SZU 0→4KB(2^12), 1→64KB(2^16), 2→1MB, ... — NVMe 스펙 CMBSZ 정의 */
	/* controller memory buffer size in Bytes */
	size = unit_size * cmbsz.bits.sz;
                                  /* [한국어] SZ 필드 = unit 개수 */
	/* controller memory buffer offset from BAR in Bytes */
	offset = unit_size * cmbloc.bits.ofst;
                                  /* [한국어] BAR 내 시작 offset (unit 단위 × OFST 필드) */

	rc = spdk_pci_device_map_bar(pctrlr->devhandle, bir, &addr,
				     &bar_phys_addr, &bar_size);
                                  /* [한국어] DPDK로 BAR mmap — addr=가상, bar_phys_addr=물리(DMA용), bar_size=BAR 전체 크기 */
	if ((rc != 0) || addr == NULL) {
                                  /* [한국어] 매핑 실패 — 권한/MEM_LIMIT 등 */
		goto exit;
	}

	if (offset > bar_size) {
                                  /* [한국어] offset이 BAR 크기 초과 (레지스터 값 이상) */
		goto exit;
	}

	if (size > bar_size - offset) {
                                  /* [한국어] CMB 영역이 BAR 경계 넘어감 */
		goto exit;
	}

	pctrlr->cmb.bar_va = addr;
                                  /* [한국어] CMB 가상 주소 저장 — alloc_cmb가 bump 할당에 사용 */
	pctrlr->cmb.bar_pa = bar_phys_addr;
                                  /* [한국어] DMA용 물리 주소 */
	pctrlr->cmb.size = size;
	pctrlr->cmb.current_offset = offset;
                                  /* [한국어] bump allocator 시작 위치 */

	if (!cmbsz.bits.sqs) {
                                  /* [한국어] SQS(Submission Queue Support) 비트 미설정 — SQ를 CMB에 못 둠 */
		pctrlr->ctrlr.opts.use_cmb_sqs = false;
	}

	return;
exit:
	pctrlr->ctrlr.opts.use_cmb_sqs = false;
                                  /* [한국어] 어떤 이유로든 실패 시 CMB SQ 사용 비활성화 (호스트 메모리 fallback) */
	return;
}

/*
 * [한국어] nvme_pcie_ctrlr_unmap_cmb - CMB BAR 언맵 (destruct 경로).
 *
 * 사용자가 map_io_cmb를 통해 mem_register했었으면 먼저 unregister → BAR unmap.
 */
static int
nvme_pcie_ctrlr_unmap_cmb(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc = 0;
	union spdk_nvme_cmbloc_register cmbloc;
	void *addr = pctrlr->cmb.bar_va;

	if (addr) {
                                  /* [한국어] 매핑되어 있으면만 */
		if (pctrlr->cmb.mem_register_addr) {
                                  /* [한국어] 사용자 데이터용 mem_register했으면 먼저 해제 */
			spdk_mem_unregister(pctrlr->cmb.mem_register_addr, pctrlr->cmb.mem_register_size);
		}

		if (nvme_pcie_ctrlr_get_cmbloc(pctrlr, &cmbloc)) {
                                  /* [한국어] unmap 시 BIR 재조회 필요 — 링크 문제로 실패 가능 */
			NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "get_cmbloc() failed\n");
			return -EIO;
		}
		rc = spdk_pci_device_unmap_bar(pctrlr->devhandle, cmbloc.bits.bir, addr);
                                  /* [한국어] DPDK BAR unmap */
	}
	return rc;
}

/*
 * [한국어] nvme_pcie_ctrlr_reserve_cmb - CMB 사용 예약 (use_cmb_sqs와 충돌 검사).
 *
 * 공개 API `spdk_nvme_ctrlr_reserve_cmb`의 PCIe 구현. 사용자가 CMB를 데이터 버퍼로 쓸 때
 * 먼저 호출해 "이제부터 내가 쓸 테니 SQ로 쓰지 마라" 선언.
 */
static int
nvme_pcie_ctrlr_reserve_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	if (pctrlr->cmb.bar_va == NULL) {
                                  /* [한국어] CMB 미매핑 (장치 미지원) */
		NVME_CTRLR_DEBUGLOG(ctrlr, "CMB not available\n");
		return -ENOTSUP;
	}

	if (ctrlr->opts.use_cmb_sqs) {
                                  /* [한국어] 이미 SQ 용도로 쓰고 있음 — 충돌 */
		NVME_CTRLR_ERRLOG(ctrlr, "CMB is already in use for submission queues.\n");
		return -ENOTSUP;
	}

	return 0;
                                  /* [한국어] OK — 사용자가 이후 map_io_cmb로 데이터 버퍼 매핑 가능 */
}

/*
 * [한국어] nvme_pcie_ctrlr_map_io_cmb - 사용자가 CMB를 데이터 버퍼로 쓰도록 공개 매핑.
 *
 * 호출자: spdk_nvme_ctrlr_map_cmb 공개 API.
 * 반환값: CMB 가상 주소 (사용자가 직접 write/read) + *size에 크기.
 *
 * 제약:
 *   - WDS(Write Data Support) 또는 RDS(Read Data Support) 최소 하나 지원
 *   - CMB 크기 ≥ 4MiB (2MB 정렬 여유 포함)
 *   - 2MB 경계로 정렬 → spdk_mem_register로 DPDK 힙에 등록 (DMA 가능)
 *
 * 반환된 주소는 **2MB-aligned hugepage 상응** 영역이므로 장치 DMA와 호환.
 */
static void *
nvme_pcie_ctrlr_map_io_cmb(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	union spdk_nvme_cmbsz_register cmbsz;
	union spdk_nvme_cmbloc_register cmbloc;
	uint64_t mem_register_start, mem_register_end;
                                  /* [한국어] 2MB 정렬된 등록 범위 */
	int rc;

	if (pctrlr->cmb.mem_register_addr != NULL) {
                                  /* [한국어] 이미 매핑됨 — 같은 결과 반환 (idempotent) */
		*size = pctrlr->cmb.mem_register_size;
		return pctrlr->cmb.mem_register_addr;
	}

	*size = 0;
                                  /* [한국어] 실패 시 *size=0 기본값 */

	if (pctrlr->cmb.bar_va == NULL) {
                                  /* [한국어] map_cmb가 성공하지 못했음 */
		NVME_CTRLR_DEBUGLOG(ctrlr, "CMB not available\n");
		return NULL;
	}

	if (ctrlr->opts.use_cmb_sqs) {
                                  /* [한국어] SQ 용도로 이미 사용 중 — reserve_cmb로 먼저 해제해야 함 */
		NVME_CTRLR_ERRLOG(ctrlr, "CMB is already in use for submission queues.\n");
		return NULL;
	}

	if (nvme_pcie_ctrlr_get_cmbsz(pctrlr, &cmbsz) ||
	    nvme_pcie_ctrlr_get_cmbloc(pctrlr, &cmbloc)) {
		NVME_CTRLR_ERRLOG(ctrlr, "get registers failed\n");
		return NULL;
	}

	/* If only SQS is supported */
	if (!(cmbsz.bits.wds || cmbsz.bits.rds)) {
                                  /* [한국어] Write/Read Data Support 모두 없음 → 데이터 버퍼로 못 씀 */
		return NULL;
	}

	/* If CMB is less than 4MiB in size then abort CMB mapping */
	if (pctrlr->cmb.size < (1ULL << 22)) {
                                  /* [한국어] 4MiB 미만이면 2MB 정렬 후 실효 용량 너무 작음 → 포기 */
		return NULL;
	}

	mem_register_start = _2MB_PAGE((uintptr_t)pctrlr->cmb.bar_va + pctrlr->cmb.current_offset +
				       VALUE_2MB - 1);
                                  /* [한국어] 시작 주소를 2MB 경계로 올림 ((addr+2MB-1) & ~(2MB-1)) */
	mem_register_end = _2MB_PAGE((uintptr_t)pctrlr->cmb.bar_va + pctrlr->cmb.current_offset +
				     pctrlr->cmb.size);
                                  /* [한국어] 끝 주소는 2MB 경계로 내림 */

	rc = spdk_mem_register((void *)mem_register_start, mem_register_end - mem_register_start);
                                  /* [한국어] ★ DPDK 힙 맵에 등록 — 이후 spdk_vtophys가 이 영역의 물리 주소 조회 가능
                                   *         이로써 사용자가 이 영역을 DMA 버퍼로 사용 가능 */
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_mem_register() failed\n");
		return NULL;
	}

	pctrlr->cmb.mem_register_addr = (void *)mem_register_start;
	pctrlr->cmb.mem_register_size = mem_register_end - mem_register_start;

	*size = pctrlr->cmb.mem_register_size;
	return pctrlr->cmb.mem_register_addr;
                                  /* [한국어] 2MB-aligned 가상 주소 반환 */
}

/*
 * [한국어] nvme_pcie_ctrlr_unmap_io_cmb - map_io_cmb의 짝 해제.
 *
 * spdk_mem_unregister로 DPDK 힙에서 제거 → 이후 이 영역으로의 vtophys는 실패.
 */
static int
nvme_pcie_ctrlr_unmap_io_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	int rc;

	if (pctrlr->cmb.mem_register_addr == NULL) {
                                  /* [한국어] 매핑 안 되어 있으면 idempotent 성공 */
		return 0;
	}

	rc = spdk_mem_unregister(pctrlr->cmb.mem_register_addr, pctrlr->cmb.mem_register_size);

	if (rc == 0) {
                                  /* [한국어] 성공 시 필드 클리어 — 재매핑 가능 상태 */
		pctrlr->cmb.mem_register_addr = NULL;
		pctrlr->cmb.mem_register_size = 0;
	}

	return rc;
}

/*
 * [한국어] ★ nvme_pcie_ctrlr_map_pmr - PMR(Persistent Memory Region) BAR 매핑.
 *
 * PMR은 NVMe 1.4+에서 추가된 **비휘발성** 장치 메모리 — 전원 차단 후에도 내용 보존.
 * 주로 Optane 기반 SSD가 지원. CMB와 달리 CBA(Controller Base Address) 설정 필요.
 *
 * 시퀀스:
 *   1) CAP.PMRS 확인
 *   2) PMRCAP → BIR(BAR 2~5) 획득
 *   3) spdk_pci_device_map_bar → BAR mmap
 *   4) CMSS(Controller Memory Space Supported) 플래그 있으면:
 *      a) PMRMSCU (상위 32b)와 PMRMSCL (하위 32b + CMSE bit)로 CBA 세팅
 *      b) PMRSTS 읽어 CBAI(CBA Invalid) 비트 확인 — 0이면 호스트 주소로 PMR 참조 가능
 *   5) pctrlr->pmr 정보 저장
 */
static void
nvme_pcie_ctrlr_map_pmr(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc;
	void *addr = NULL;
	uint32_t bir;
	union spdk_nvme_pmrcap_register pmrcap;
	uint64_t bar_size = 0, bar_phys_addr = 0;

	if (!pctrlr->regs->cap.bits.pmrs) {
                                  /* [한국어] CAP.PMRS=0 — PMR 미지원 장치 */
		return;
	}

	if (nvme_pcie_ctrlr_get_pmrcap(pctrlr, &pmrcap)) {
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "get registers failed\n");
		return;
	}

	bir = pmrcap.bits.bir;
	/* Values 2 3 4 5 are valid for BAR */
                                  /* [한국어] PMR은 BAR 2~5만 허용 (BAR0는 레지스터, BAR1은 상위 절반) */
	if (bir > 5 || bir < 2) {
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "invalid base indicator register value\n");
		return;
	}

	rc = spdk_pci_device_map_bar(pctrlr->devhandle, bir, &addr, &bar_phys_addr, &bar_size);
                                  /* [한국어] DPDK BAR mmap */
	if ((rc != 0) || addr == NULL) {
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "could not map the bar %d\n", bir);
		return;
	}

	if (pmrcap.bits.cmss) {
                                  /* [한국어] Controller Memory Space Supported — 호스트 주소 기반 PMR 접근 기능 */
		uint32_t pmrmscl, pmrmscu, cmse = 1;
                                  /* [한국어] cmse = Controller Memory Space Enable 비트 */
		union spdk_nvme_pmrsts_register pmrsts;

		/* Enable Controller Memory Space */
		pmrmscl = (uint32_t)((bar_phys_addr & 0xFFFFF000ULL) | (cmse << 1));
                                  /* [한국어] PMRMSCL 하위 32b 구성:
                                   *         - [31:12]: CBA(Controller Base Address) 하위 — 4KB 정렬
                                   *         - [1]: CMSE = 1 (enable)
                                   *         - [0]: reserved */
		pmrmscu = (uint32_t)((bar_phys_addr >> 32ULL) & 0xFFFFFFFFULL);
                                  /* [한국어] PMRMSCU 상위 32b — CBA 상위 주소 */

		if (nvme_pcie_ctrlr_set_pmrmscu(pctrlr, pmrmscu)) {
                                  /* [한국어] 상위 먼저 write (장치가 원자적으로 인식하도록 순서 중요) */
			NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "set_pmrmscu() failed\n");
			spdk_pci_device_unmap_bar(pctrlr->devhandle, bir, addr);
			return;
		}

		if (nvme_pcie_ctrlr_set_pmrmscl(pctrlr, pmrmscl)) {
                                  /* [한국어] 하위 write (CMSE=1 포함 — 장치가 CBA 확정 및 활성화) */
			NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "set_pmrmscl() failed\n");
			spdk_pci_device_unmap_bar(pctrlr->devhandle, bir, addr);
			return;
		}

		if (nvme_pcie_ctrlr_get_pmrsts(pctrlr, &pmrsts)) {
			NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "get pmrsts failed\n");
			spdk_pci_device_unmap_bar(pctrlr->devhandle, bir, addr);
			return;
		}

		if (pmrsts.bits.cbai) {
                                  /* [한국어] CBAI(Controller Base Address Invalid) 세트 — CBA 설정 실패 */
			NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "Controller Memory Space Enable Failure\n");
			NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "CBA Invalid - Host Addresses cannot reference PMR\n");
		} else {
                                  /* [한국어] 성공 — 이제 호스트 메모리 주소로 PMR 참조 가능 */
			NVME_CTRLR_DEBUGLOG(&pctrlr->ctrlr, "Controller Memory Space Enable Success\n");
			NVME_CTRLR_DEBUGLOG(&pctrlr->ctrlr, "Host Addresses can reference PMR\n");
		}
	}

	pctrlr->pmr.bar_va = addr;
	pctrlr->pmr.bar_pa = bar_phys_addr;
	pctrlr->pmr.size = pctrlr->ctrlr.pmr_size = bar_size;
                                  /* [한국어] ctrlr->pmr_size도 동시 설정 — 공개 API(spdk_nvme_ctrlr_get_pmr_size)가 참조 */
}

/*
 * [한국어] nvme_pcie_ctrlr_unmap_pmr - PMR BAR 언맵 (map의 짝).
 *
 * CMSS 지원이면 PMRMSCU/PMRMSCL을 0으로 리셋 (CBA 해제) 후 BAR unmap.
 */
static int
nvme_pcie_ctrlr_unmap_pmr(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc = 0;
	union spdk_nvme_pmrcap_register pmrcap;
	void *addr = pctrlr->pmr.bar_va;

	if (addr == NULL) {
                                  /* [한국어] 매핑 안 되어 있으면 idempotent 0 */
		return rc;
	}

	if (pctrlr->pmr.mem_register_addr) {
                                  /* [한국어] 사용자 데이터 등록 되어 있었으면 해제 */
		spdk_mem_unregister(pctrlr->pmr.mem_register_addr, pctrlr->pmr.mem_register_size);
	}

	if (nvme_pcie_ctrlr_get_pmrcap(pctrlr, &pmrcap)) {
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "get_pmrcap() failed\n");
		return -EIO;
	}

	if (pmrcap.bits.cmss) {
                                  /* [한국어] CMSS 지원 PMR이면 CBA 리셋 */
		if (nvme_pcie_ctrlr_set_pmrmscu(pctrlr, 0)) {
			NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "set_pmrmscu() failed\n");
		}

		if (nvme_pcie_ctrlr_set_pmrmscl(pctrlr, 0)) {
                                  /* [한국어] CMSE=0 비트도 함께 클리어 — PMR 비활성화 */
			NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "set_pmrmscl() failed\n");
		}
	}

	rc = spdk_pci_device_unmap_bar(pctrlr->devhandle, pmrcap.bits.bir, addr);

	return rc;
}

/*
 * [한국어] ★ nvme_pcie_ctrlr_config_pmr - PMR enable/disable 상태 전이 (timeout 대기 포함).
 *
 * PMRCTL.EN 비트를 write한 후 PMRSTS.NRDY가 원하는 값이 될 때까지 busy-wait.
 * PMRCAP.PMRTO + PMRTU가 제공하는 timeout 범위 내에서 대기.
 *
 * PMRTU 의미:
 *   0: PMRTO 단위 = 500ms
 *   1: PMRTU 단위 = 60000ms (1분)
 * → PMR은 비휘발성 매체라 활성화에 시간이 걸릴 수 있음 (예: 전원 ramp, 초기화 등).
 *
 * Busy-wait 루프: PMRSTS.NRDY가 (enable && NRDY==1)일 때 여전히 대기 중 → 루프 계속.
 */
static int
nvme_pcie_ctrlr_config_pmr(struct spdk_nvme_ctrlr *ctrlr, bool enable)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	union spdk_nvme_pmrcap_register pmrcap;
	union spdk_nvme_pmrctl_register pmrctl;
	union spdk_nvme_pmrsts_register pmrsts;
	uint8_t pmrto, pmrtu;
                                  /* [한국어] PMR Time-Out value + Time Unit */
	uint64_t timeout_in_ms, ticks_per_ms, timeout_in_ticks, now_ticks;

	if (!pctrlr->regs->cap.bits.pmrs) {
		NVME_CTRLR_ERRLOG(ctrlr, "PMR is not supported by the controller\n");
		return -ENOTSUP;
	}

	if (nvme_pcie_ctrlr_get_pmrcap(pctrlr, &pmrcap)) {
		NVME_CTRLR_ERRLOG(ctrlr, "get registers failed\n");
		return -EIO;
	}

	pmrto = pmrcap.bits.pmrto;
                                  /* [한국어] timeout 숫자 */
	pmrtu = pmrcap.bits.pmrtu;
                                  /* [한국어] 시간 단위 플래그 (0=500ms, 1=1분) */

	if (pmrtu > 1) {
                                  /* [한국어] 스펙상 0~1만 유효 */
		NVME_CTRLR_ERRLOG(ctrlr, "PMR Time Units Invalid\n");
		return -EINVAL;
	}

	ticks_per_ms = spdk_get_ticks_hz() / 1000;
                                  /* [한국어] ms당 tick 수 */
	timeout_in_ms = pmrto * (pmrtu ? (60 * 1000) : 500);
                                  /* [한국어] pmrto × 단위 = 총 대기 ms */
	timeout_in_ticks = timeout_in_ms * ticks_per_ms;

	if (nvme_pcie_ctrlr_get_pmrctl(pctrlr, &pmrctl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "get pmrctl failed\n");
		return -EIO;
	}

	if (enable && pmrctl.bits.en != 0) {
                                  /* [한국어] enable 요청인데 이미 활성화됨 */
		NVME_CTRLR_ERRLOG(ctrlr, "PMR is already enabled\n");
		return -EINVAL;
	} else if (!enable && pmrctl.bits.en != 1) {
                                  /* [한국어] disable 요청인데 이미 비활성화됨 */
		NVME_CTRLR_ERRLOG(ctrlr, "PMR is already disabled\n");
		return -EINVAL;
	}

	pmrctl.bits.en = enable;
                                  /* [한국어] EN 비트 업데이트 */

	if (nvme_pcie_ctrlr_set_pmrctl(pctrlr, &pmrctl)) {
                                  /* [한국어] PMRCTL write — 장치가 전원/메모리 시퀀스 시작 */
		NVME_CTRLR_ERRLOG(ctrlr, "set pmrctl failed\n");
		return -EIO;
	}

	now_ticks =  spdk_get_ticks();
                                  /* [한국어] timeout 기준 시작 tick */

	do {
		if (nvme_pcie_ctrlr_get_pmrsts(pctrlr, &pmrsts)) {
			NVME_CTRLR_ERRLOG(ctrlr, "get pmrsts failed\n");
			return -EIO;
		}

		if (pmrsts.bits.nrdy == enable &&
		    spdk_get_ticks() > now_ticks + timeout_in_ticks) {
                                  /* [한국어] 여전히 not-ready인데 timeout 경과 */
			NVME_CTRLR_ERRLOG(ctrlr, "PMR Enable - Timed Out\n");
			return -ETIMEDOUT;
		}
	} while (pmrsts.bits.nrdy == enable);
                                  /* [한국어] NRDY != enable이 되면 (ready 상태 도달) 루프 종료 */

	NVME_CTRLR_DEBUGLOG(ctrlr, "PMR %s\n", enable ? "Enabled" : "Disabled");

	return 0;
}

/*
 * [한국어] nvme_pcie_ctrlr_enable_pmr / disable_pmr - config_pmr의 얇은 wrapper.
 */
static int
nvme_pcie_ctrlr_enable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	return nvme_pcie_ctrlr_config_pmr(ctrlr, true);
                                  /* [한국어] enable=true */
}

static int
nvme_pcie_ctrlr_disable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	return nvme_pcie_ctrlr_config_pmr(ctrlr, false);
                                  /* [한국어] enable=false */
}

/*
 * [한국어] nvme_pcie_ctrlr_map_io_pmr - 사용자가 PMR을 데이터 버퍼로 쓰도록 공개 매핑.
 *
 * CMB의 map_io_cmb와 유사 — WDS/RDS 최소 하나 지원, 4MiB 이상, 2MB 정렬 후 DPDK 등록.
 * 차이: PMR은 BAR offset 없음 (bar_va 전체 영역 사용), current_offset 필드 없음.
 */
static void *
nvme_pcie_ctrlr_map_io_pmr(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	union spdk_nvme_pmrcap_register pmrcap;
	uint64_t mem_register_start, mem_register_end;
	int rc;

	if (!pctrlr->regs->cap.bits.pmrs) {
		NVME_CTRLR_ERRLOG(ctrlr, "PMR is not supported by the controller\n");
		return NULL;
	}

	if (pctrlr->pmr.mem_register_addr != NULL) {
                                  /* [한국어] 이미 매핑됨 — idempotent 반환 */
		*size = pctrlr->pmr.mem_register_size;
		return pctrlr->pmr.mem_register_addr;
	}

	*size = 0;

	if (pctrlr->pmr.bar_va == NULL) {
                                  /* [한국어] map_pmr 실패 */
		NVME_CTRLR_DEBUGLOG(ctrlr, "PMR not available\n");
		return NULL;
	}

	if (nvme_pcie_ctrlr_get_pmrcap(pctrlr, &pmrcap)) {
		NVME_CTRLR_ERRLOG(ctrlr, "get registers failed\n");
		return NULL;
	}

	/* Check if WDS / RDS is supported */
	if (!(pmrcap.bits.wds || pmrcap.bits.rds)) {
                                  /* [한국어] 데이터 R/W 기능 모두 없으면 포기 */
		return NULL;
	}

	/* If PMR is less than 4MiB in size then abort PMR mapping */
	if (pctrlr->pmr.size < (1ULL << 22)) {
                                  /* [한국어] 4MiB 미만 시 2MB 정렬 후 실효 용량 부족 */
		return NULL;
	}

	mem_register_start = _2MB_PAGE((uintptr_t)pctrlr->pmr.bar_va + VALUE_2MB - 1);
                                  /* [한국어] 2MB 경계로 올림 */
	mem_register_end = _2MB_PAGE((uintptr_t)pctrlr->pmr.bar_va + pctrlr->pmr.size);
                                  /* [한국어] 2MB 경계로 내림 */

	rc = spdk_mem_register((void *)mem_register_start, mem_register_end - mem_register_start);
                                  /* [한국어] DPDK 힙 등록 — 이후 vtophys로 DMA 가능 */
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_mem_register() failed\n");
		return NULL;
	}

	pctrlr->pmr.mem_register_addr = (void *)mem_register_start;
	pctrlr->pmr.mem_register_size = mem_register_end - mem_register_start;

	*size = pctrlr->pmr.mem_register_size;
	return pctrlr->pmr.mem_register_addr;
}

/*
 * [한국어] nvme_pcie_ctrlr_unmap_io_pmr - map_io_pmr의 짝 해제.
 *
 * 매핑되지 않은 상태면 -ENXIO 반환 (CMB의 unmap은 0 반환 — 차이점).
 */
static int
nvme_pcie_ctrlr_unmap_io_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	int rc;

	if (pctrlr->pmr.mem_register_addr == NULL) {
                                  /* [한국어] 매핑 안 되어 있음 — 호출 오류로 간주 (CMB와 달리 -ENXIO 반환) */
		return -ENXIO;
	}

	rc = spdk_mem_unregister(pctrlr->pmr.mem_register_addr, pctrlr->pmr.mem_register_size);

	if (rc == 0) {
                                  /* [한국어] 성공 시 필드 클리어 */
		pctrlr->pmr.mem_register_addr = NULL;
		pctrlr->pmr.mem_register_size = 0;
	}

	return rc;
}

/*
 * [한국어] ★★ nvme_pcie_ctrlr_allocate_bars - BAR0 mmap + doorbell_base 계산 + CMB/PMR 매핑 ★★
 *
 * PCIe 컨트롤러 초기화의 **핵심 단계**. 이 함수가 성공해야 이후 모든 MMIO 레지스터 접근이 가능.
 *
 * 작업 내용:
 *   1) BAR0를 mmap → `pctrlr->regs` (NVMe 레지스터 영역 = 4KB + 큐 개수 × 8B × doorbell_stride)
 *      - spdk_pci_device_map_bar: DPDK VFIO/UIO 경유 mmap
 *      - addr=가상주소, phys_addr=물리주소(DMA용 — BAR는 직접 DMA 안 하지만 일관성), size=BAR0 크기
 *   2) `regs_size` 저장 — SIGBUS 핸들러가 remap 영역 크기 결정에 사용
 *   3) ★ `doorbell_base` 계산 — regs 구조체의 doorbell[0].sq_tdbl 주소 → 이후
 *      qpair_construct가 `(2*qid+0)*stride` 오프셋을 더해 각 SQ/CQ doorbell 위치 산출
 *   4) CMB/PMR 매핑 시도 (선택적 기능)
 *
 * 호출자: nvme_pcie_ctrlr_construct. 실패 시 컨트롤러 생성 포기.
 */
static int
nvme_pcie_ctrlr_allocate_bars(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc;
	void *addr = NULL;
                                  /* [한국어] BAR0 가상 주소 */
	uint64_t phys_addr = 0, size = 0;
                                  /* [한국어] BAR0 물리 주소 + 크기 */

	rc = spdk_pci_device_map_bar(pctrlr->devhandle, 0, &addr,
				     &phys_addr, &size);
                                  /* [한국어] ★ BAR0 mmap — NVMe 레지스터 전체 영역 획득
                                   *         bar=0은 NVMe 스펙상 mandatory BAR (CAP/VS/CC/CSTS/AQA/ASQ/ACQ/doorbell) */

	if ((addr == NULL) || (rc != 0)) {
                                  /* [한국어] 매핑 실패 — VFIO 권한 문제, 다른 프로세스 점유 등 */
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "nvme_pcicfg_map_bar failed with rc %d or bar %p\n",
				  rc, addr);
		return -1;
	}

	pctrlr->regs = (volatile struct spdk_nvme_registers *)addr;
                                  /* [한국어] ★ 모든 MMIO 접근의 기점 — volatile cast로 컴파일러 최적화 방지 */
	pctrlr->regs_size = size;
                                  /* [한국어] SIGBUS 핸들러가 remap할 때 사용 */
	pctrlr->doorbell_base = (volatile uint32_t *)&pctrlr->regs->doorbell[0].sq_tdbl;
                                  /* [한국어] ★★ doorbell 배열 시작 주소 — nvme_pcie_common.c의 qpair_construct가
                                   *         pctrlr->doorbell_base + (2*qid+0)*stride로 각 SQ tail doorbell 계산
                                   *         NVMe 스펙: doorbell은 BAR0 offset 0x1000부터, 큐당 2개(SQT+CQH) */
	nvme_pcie_ctrlr_map_cmb(pctrlr);
                                  /* [한국어] CMB 매핑 (선택적, 실패해도 계속 진행) */
	nvme_pcie_ctrlr_map_pmr(pctrlr);
                                  /* [한국어] PMR 매핑 (선택적) */

	return 0;
}

/*
 * [한국어] nvme_pcie_ctrlr_free_bars - allocate_bars의 역순 해제 (destruct 경로).
 *
 * 해제 순서 (역순):
 *   1) PMR unmap (cmss 있으면 PMRMSCU/L=0로 리셋)
 *   2) CMB unmap
 *   3) BAR0 unmap (primary 프로세스만 — secondary는 참조만, unmap은 primary 책임)
 *
 * is_removed=true (장치 물리 제거됨)이면 이미 MMIO 불가 → 해제 스킵 (DPDK가 내부 정리).
 */
static int
nvme_pcie_ctrlr_free_bars(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc = 0;
	void *addr = (void *)pctrlr->regs;

	if (pctrlr->ctrlr.is_removed) {
                                  /* [한국어] 물리 제거된 장치 — unmap 시도 자체가 SIGBUS 유발 가능 */
		return rc;
	}

	rc = nvme_pcie_ctrlr_unmap_pmr(pctrlr);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "nvme_ctrlr_unmap_pmr failed with error code %d\n", rc);
		return -1;
	}

	rc = nvme_pcie_ctrlr_unmap_cmb(pctrlr);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "nvme_ctrlr_unmap_cmb failed with error code %d\n", rc);
		return -1;
	}

	if (addr && spdk_process_is_primary()) {
		/* NOTE: addr may have been remapped here. We're relying on DPDK to call
		 * munmap internally.
		 */
                                  /* [한국어] BAR0 unmap — primary만 수행.
                                   *         SIGBUS 후 remap됐다면 anonymous 메모리일 수 있지만 DPDK가 처리 */
		rc = spdk_pci_device_unmap_bar(pctrlr->devhandle, 0, addr);
	}
	return rc;
}

/* This function must only be called while holding g_spdk_nvme_driver->lock */
/*
 * [한국어] ★★ pcie_nvme_enum_cb - DPDK PCI enumerate가 각 NVMe 장치마다 호출하는 콜백 ★★
 *
 * 호출자: nvme_pcie_ctrlr_scan → spdk_pci_enumerate/attach → 이 콜백.
 *
 * 반환값:
 *   0 = 이 장치 attach 성공
 *   1 = skip (traddr 불일치 등)
 *   -1 = 에러 (enumerate 중단)
 *
 * 분기:
 *   1) Secondary 프로세스:
 *      - primary가 이미 construct한 ctrlr이 있어야 함 (없으면 에러)
 *      - interrupt 모드는 secondary 지원 안 함
 *      - nvme_ctrlr_add_process로 자기 프로세스 컨텍스트 추가
 *   2) Primary 프로세스:
 *      - traddr 제한 있으면 일치 검사 (아니면 skip=1)
 *      - nvme_ctrlr_probe로 probe_cb 호출 → 사용자가 attach 결정
 */
static int
pcie_nvme_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct spdk_nvme_transport_id trid = {};
	struct nvme_pcie_enum_ctx *enum_ctx = ctx;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_pci_addr pci_addr, _pci_addr;

	pci_addr = spdk_pci_device_get_addr(pci_dev);
                                  /* [한국어] DPDK가 관리하는 PCI 주소 획득 (BDF 형식) */

	spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
	spdk_pci_addr_fmt(trid.traddr, sizeof(trid.traddr), &pci_addr);
                                  /* [한국어] "0000:81:00.0" 같은 문자열로 포맷해 trid.traddr에 저장 */

	if (spdk_pci_addr_parse(&_pci_addr, trid.traddr)) {
                                  /* [한국어] 자기 포맷한 문자열을 다시 파싱 — 내부 일관성 검증 (보통 성공) */
		SPDK_ERRLOG("spdk_pci_addr_parse failed; likely internal environment layer issue.\n");
		assert(false);
		return -1;
	}

	ctrlr = nvme_get_ctrlr_by_trid_unsafe(&trid, NULL);
                                  /* [한국어] 이 trid로 이미 등록된 ctrlr 조회 (보통 primary에서만 유효) */
	if (!spdk_process_is_primary()) {
                                  /* [한국어] ★ Secondary 프로세스 경로 — primary가 이미 만든 ctrlr에 자기 컨텍스트만 추가 */
		if (!ctrlr) {
			SPDK_ERRLOG("Controller must be constructed in the primary process first.\n");
			return -1;
		}

		if (ctrlr->opts.enable_interrupts) {
                                  /* [한국어] secondary는 interrupt 모드 미지원 (MSI-X vector 프로세스별 라우팅 어려움) */
			NVME_CTRLR_ERRLOG(ctrlr, "Secondary processes are not supported in interrupt mode.\n");
			return -1;
		}

		return nvme_ctrlr_add_process(ctrlr, pci_dev);
                                  /* [한국어] 이 프로세스의 컨텍스트(pid, devhandle 등)를 ctrlr->active_procs에 추가 */
	}

	/* check whether user passes the pci_addr */
	if (enum_ctx->has_pci_addr &&
	    (spdk_pci_addr_compare(&pci_addr, &enum_ctx->pci_addr) != 0)) {
                                  /* [한국어] 사용자가 특정 traddr 지정 + 일치하지 않으면 skip(1) */
		return 1;
	}

	return nvme_ctrlr_probe(&trid, enum_ctx->probe_ctx, pci_dev);
                                  /* [한국어] ★ probe 본체 — 사용자 probe_cb 호출 + construct + attach 체인 */
}

/*
 * [한국어] nvme_pci_ctrlr_scan_attached - primary 프로세스 전용 hotplug 모니터.
 *
 * 트랜스포트 vtable의 ctrlr_scan_attached 구현. `_nvme_pcie_hotplug_monitor` 위임.
 * secondary는 no-op (primary가 감지).
 */
static int
nvme_pci_ctrlr_scan_attached(struct spdk_nvme_probe_ctx *probe_ctx)
{
	/* Only the primary process can monitor hotplug. */
	if (spdk_process_is_primary()) {
		return _nvme_pcie_hotplug_monitor(probe_ctx);
                                  /* [한국어] hotplug 이벤트 드레인 + is_removed 체크 */
	}
	return 0;
}

/*
 * [한국어] ★ nvme_pcie_ctrlr_scan - 트랜스포트 vtable의 ctrlr_scan 구현 (probe 진입점) ★
 *
 * @direct_connect: true면 traddr 정확히 지정된 attach, false면 전체 enumerate.
 *
 * 분기:
 *   1) traddr 있음 → enum_ctx.has_pci_addr=true, traddr 파싱
 *   2) scan_attached 먼저 호출 → hotplug remove 이벤트 우선 처리 (#3205 회피)
 *   3) traddr 없음: spdk_pci_enumerate — 모든 NVMe 장치 순회 (pcie_nvme_enum_cb 호출)
 *      traddr 있음: spdk_pci_device_attach — 특정 장치만 attach
 */
static int
nvme_pcie_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx,
		     bool direct_connect)
{
	struct nvme_pcie_enum_ctx enum_ctx = {};

	enum_ctx.probe_ctx = probe_ctx;

	if (strlen(probe_ctx->trid.traddr) != 0) {
                                  /* [한국어] traddr 지정됨 — 특정 장치만 attach */
		if (spdk_pci_addr_parse(&enum_ctx.pci_addr, probe_ctx->trid.traddr)) {
                                  /* [한국어] traddr 파싱 실패 — 잘못된 형식 */
			return -1;
		}
		enum_ctx.has_pci_addr = true;
	}

	/* Only the primary process can monitor hotplug. */
	if (nvme_pci_ctrlr_scan_attached(probe_ctx) > 0) {
		/* Some removal events were received. Return immediately, avoiding
		 * an spdk_pci_enumerate() which could trigger issue #3205. */
                                  /* [한국어] hotplug remove가 발생한 경우 spdk_pci_enumerate 건너뜀.
                                   *         이슈 #3205: 장치 제거 직후 enumerate가 crash 유발 가능 */
		return 0;
	}

	if (enum_ctx.has_pci_addr == false) {
                                  /* [한국어] 전체 enumerate */
		return spdk_pci_enumerate(spdk_pci_nvme_get_driver(),
					  pcie_nvme_enum_cb, &enum_ctx);
                                  /* [한국어] DPDK가 NVMe 드라이버에 등록된 모든 장치 순회 → 각 장치마다 enum_cb 호출 */
	} else {
                                  /* [한국어] 특정 traddr attach */
		return spdk_pci_device_attach(spdk_pci_nvme_get_driver(),
					      pcie_nvme_enum_cb, &enum_ctx, &enum_ctx.pci_addr);
                                  /* [한국어] 지정된 pci_addr 장치 한 개만 attach */
	}
}

/*
 * [한국어] ★★ nvme_pcie_ctrlr_construct - PCIe 컨트롤러 객체 생성 + BAR 매핑 + admin qpair 초기화 ★★
 *
 * 트랜스포트 vtable의 ctrlr_construct 구현. pcie_nvme_enum_cb → nvme_ctrlr_probe가 호출.
 *
 * 전체 시퀀스:
 *   [1] spdk_pci_device_claim — 이 프로세스가 PCI 장치 소유권 획득
 *   [2] nvme_pcie_ctrlr 구조체 할당 (SHARE hugepage — multi-process)
 *   [3] trid/opts/quirks/NUMA 필드 초기화
 *   [4] nvme_ctrlr_construct — 공통 ctrlr 초기화
 *   [5] ★ nvme_pcie_ctrlr_allocate_bars — BAR0 mmap + doorbell_base 계산 + CMB/PMR
 *   [6] PCI Config 레지스터 CMD에 0x404 OR:
 *       - 0x004: Bus Master Enable (DMA 허용)
 *       - 0x400: Interrupt Disable (INTx off — SPDK는 polled 또는 MSI-X)
 *   [7] CAP 레지스터 read → doorbell_stride_u32 계산
 *       (NVMe 스펙: doorbell stride = 2^(DSTRD+2) bytes; 여기서는 dword 단위로 2^DSTRD 저장)
 *   [8] ★ nvme_pcie_ctrlr_construct_admin_qpair — admin qpair(qid=0) SQ/CQ 할당
 *       (nvme_pcie_common.c 구현)
 *   [9] nvme_ctrlr_add_process — 현재 프로세스 컨텍스트 등록
 *   [10] 첫 ctrlr면 SIGBUS 핸들러 등록 (g_sigset 플래그)
 */
static struct spdk_nvme_ctrlr *
	nvme_pcie_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
			  const struct spdk_nvme_ctrlr_opts *opts,
			  void *devhandle)
{
	struct spdk_pci_device *pci_dev = devhandle;
                                  /* [한국어] DPDK가 전달한 PCI 장치 핸들 */
	struct nvme_pcie_ctrlr *pctrlr;
	union spdk_nvme_cap_register cap;
	uint16_t cmd_reg;
                                  /* [한국어] PCI Config Space의 Command Register (offset 0x04) */
	int rc;
	struct spdk_pci_id pci_id;

	rc = spdk_pci_device_claim(pci_dev);
                                  /* [한국어] DPDK에 이 장치를 우리가 쓰겠다고 claim — 다른 프로세스가 동시 사용 못 하게 */
	if (rc < 0) {
		SPDK_ERRLOG("could not claim device %s (%s)\n",
			    trid->traddr, spdk_strerror(-rc));
		return NULL;
	}

	pctrlr = spdk_zmalloc(sizeof(struct nvme_pcie_ctrlr), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
                                  /* [한국어] 64B align, SHARE hugepage (primary/secondary 공유 가능) */
	if (pctrlr == NULL) {
		spdk_pci_device_unclaim(pci_dev);
                                  /* [한국어] 할당 실패 시 claim 되돌림 */
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	pctrlr->is_remapped = false;
                                  /* [한국어] BAR 아직 SIGBUS-remap 안 됨 */
	pctrlr->ctrlr.is_removed = false;
                                  /* [한국어] 장치 제거 플래그 초기값 */
	pctrlr->devhandle = devhandle;
                                  /* [한국어] PCI 장치 핸들 저장 — 이후 BAR mmap/unmap 등에서 사용 */
	pctrlr->ctrlr.opts = *opts;
                                  /* [한국어] 사용자 옵션 복사 (io_queue_size, qprio 등) */
	pctrlr->ctrlr.trid = *trid;
                                  /* [한국어] trid 복사 (traddr 등) */
	pctrlr->ctrlr.opts.admin_queue_size = spdk_max(pctrlr->ctrlr.opts.admin_queue_size,
					      NVME_PCIE_MIN_ADMIN_QUEUE_SIZE);
                                  /* [한국어] admin 큐 크기에 최소값 강제 (너무 작으면 AER 등 부족) */
	pci_id = spdk_pci_device_get_id(pci_dev);
	pctrlr->ctrlr.quirks = nvme_get_quirks(&pci_id);
                                  /* [한국어] 벤더/장치 ID별 quirk 플래그 조회 (NVME_QUIRK_NO_SGL_FOR_DSM 등) */
	if (pci_dev->numa_id != SPDK_ENV_NUMA_ID_ANY) {
                                  /* [한국어] NUMA 정보가 있으면 ctrlr에도 저장 — I/O qpair 할당 시 NUMA 친화적 배치 */
		pctrlr->ctrlr.numa.id_valid = 1;
		pctrlr->ctrlr.numa.id = pci_dev->numa_id;
	}

	rc = nvme_ctrlr_construct(&pctrlr->ctrlr);
                                  /* [한국어] 공통 ctrlr 초기화 — state machine 초기화, 기본 리스트 INIT 등 */
	if (rc != 0) {
		spdk_pci_device_unclaim(pci_dev);
		spdk_free(pctrlr);
		return NULL;
	}

	rc = nvme_pcie_ctrlr_allocate_bars(pctrlr);
                                  /* [한국어] ★ BAR0 mmap + doorbell_base + CMB/PMR */
	if (rc != 0) {
		spdk_pci_device_unclaim(pci_dev);
		spdk_free(pctrlr);
		return NULL;
	}

	/* Enable PCI busmaster and disable INTx */
	spdk_pci_device_cfg_read16(pci_dev, &cmd_reg, 4);
                                  /* [한국어] PCI Config offset 0x04 (Command Register) 16b read */
	cmd_reg |= 0x404;
                                  /* [한국어] 비트 OR:
                                   *         0x004 = Bus Master Enable (장치가 DMA 수행 허용 — 필수)
                                   *         0x400 = Interrupt Disable (INTx 비활성화 — SPDK는 polled 또는 MSI-X 사용) */
	spdk_pci_device_cfg_write16(pci_dev, cmd_reg, 4);
                                  /* [한국어] Command Register write-back */

	if (nvme_ctrlr_get_cap(&pctrlr->ctrlr, &cap)) {
                                  /* [한국어] CAP 레지스터 read (64b) — 장치 기능 플래그 */
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "get_cap() failed\n");
		spdk_pci_device_unclaim(pci_dev);
		spdk_free(pctrlr);
		return NULL;
	}

	/* Doorbell stride is 2 ^ (dstrd + 2),
	 * but we want multiples of 4, so drop the + 2 */
                                  /* [한국어] ★ doorbell stride 계산:
                                   *         NVMe spec: 실제 바이트 stride = 2^(DSTRD+2) — 최소 4B, 최대 2^63 B
                                   *         여기선 dword 단위로 저장 → 2^DSTRD
                                   *         qpair_construct가 이 값으로 각 SQ/CQ doorbell 위치 산출 */
	pctrlr->doorbell_stride_u32 = 1 << cap.bits.dstrd;

	rc = nvme_pcie_ctrlr_construct_admin_qpair(&pctrlr->ctrlr, pctrlr->ctrlr.opts.admin_queue_size);
                                  /* [한국어] ★ admin qpair(qid=0) 생성 — nvme_pcie_common.c */
	if (rc != 0) {
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
                                  /* [한국어] 실패 시 공통 destruct 경로로 (BAR unmap 포함) */
		return NULL;
	}

	/* Construct the primary process properties */
	rc = nvme_ctrlr_add_process(&pctrlr->ctrlr, pci_dev);
                                  /* [한국어] 현재 (primary) 프로세스 컨텍스트를 ctrlr에 등록 */
	if (rc != 0) {
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		return NULL;
	}

	if (g_sigset != true) {
                                  /* [한국어] 프로세스 전체에서 최초 ctrlr construct 시만 SIGBUS 핸들러 등록 */
		spdk_pci_register_error_handler(nvme_sigbus_fault_sighandler,
						NULL);
                                  /* [한국어] DPDK에 SIGBUS 시 호출할 핸들러 등록 (PCIe error 경로) */
		g_sigset = true;
                                  /* [한국어] 이후 중복 등록 방지 */
	}

	return &pctrlr->ctrlr;
                                  /* [한국어] 공통 spdk_nvme_ctrlr 포인터 반환 — 상위 probe_cb에 전달됨 */
}

/*
 * [한국어] ★ nvme_pcie_ctrlr_enable - 컨트롤러 CC.EN=1 직전 preparation (ASQ/ACQ/AQA 세팅).
 *
 * 트랜스포트 vtable의 ctrlr_enable 구현. nvme_ctrlr_process_init 상태머신의 ENABLE 단계 호출.
 *
 * 3가지 레지스터 초기화 (NVMe spec Section 3):
 *   1) ASQ = admin SQ 물리 주소 — 장치가 여기서 admin SQE fetch
 *   2) ACQ = admin CQ 물리 주소 — 장치가 여기에 admin CQE 기록
 *   3) AQA = admin 큐 크기 (ASQS, ACQS 모두 0-based)
 *
 * 이후 상위(nvme_ctrlr_process_init)가 CC.EN=1을 쓰면 장치가 admin 큐 fetch 시작 → CSTS.RDY=1 대기.
 *
 * 주의: 이 함수는 CC.EN=1 자체는 안 씀 (그건 nvme_ctrlr.c의 공통 로직이 처리).
 */
static int
nvme_pcie_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_pcie_qpair *padminq = nvme_pcie_qpair(ctrlr->adminq);
                                  /* [한국어] admin qpair의 PCIe 특화 정보 (cmd/cpl 물리 주소) */
	union spdk_nvme_aqa_register aqa;

	if (nvme_pcie_ctrlr_set_asq(pctrlr, padminq->cmd_bus_addr)) {
                                  /* [한국어] ASQ = admin SQ 물리 주소 (qpair_construct에서 계산됨) */
		NVME_CTRLR_ERRLOG(ctrlr, "set_asq() failed\n");
		return -EIO;
	}

	if (nvme_pcie_ctrlr_set_acq(pctrlr, padminq->cpl_bus_addr)) {
                                  /* [한국어] ACQ = admin CQ 물리 주소 */
		NVME_CTRLR_ERRLOG(ctrlr, "set_acq() failed\n");
		return -EIO;
	}

	aqa.raw = 0;
	/* acqs and asqs are 0-based. */
	aqa.bits.acqs = nvme_pcie_qpair(ctrlr->adminq)->num_entries - 1;
                                  /* [한국어] Admin CQ Size (0-based) — 예: num_entries=32면 ACQS=31 */
	aqa.bits.asqs = nvme_pcie_qpair(ctrlr->adminq)->num_entries - 1;
                                  /* [한국어] Admin SQ Size (0-based) */

	if (nvme_pcie_ctrlr_set_aqa(pctrlr, &aqa)) {
                                  /* [한국어] AQA write — 장치가 admin 큐 크기 인식 */
		NVME_CTRLR_ERRLOG(ctrlr, "set_aqa() failed\n");
		return -EIO;
	}

	return 0;
                                  /* [한국어] 이후 상위가 CC.EN=1 → CSTS.RDY=1 대기 시퀀스 수행 */
}

/*
 * [한국어] nvme_pcie_ctrlr_destruct - 트랜스포트 vtable의 ctrlr_destruct 구현.
 *
 * 해제 순서 (역순으로 construct 뒤집기):
 *   1) admin qpair 해제 (nvme_pcie_common.c의 qpair_destroy)
 *   2) 공통 ctrlr destruct finish (namespace 해제 등)
 *   3) BAR unmap (CMB/PMR 포함)
 *   4) interrupt 활성화된 경우 비활성화
 *   5) PCI device 언클레임 + detach (DPDK 내부 정리)
 *   6) pctrlr 구조체 free
 */
static int
nvme_pcie_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct spdk_pci_device *devhandle = nvme_ctrlr_proc_get_devhandle(ctrlr);
                                  /* [한국어] 현재 프로세스의 devhandle (multi-process에서 프로세스별) */

	if (ctrlr->adminq) {
		nvme_pcie_qpair_destroy(ctrlr->adminq);
                                  /* [한국어] admin qpair 해제 — outstanding AER abort 포함 */
	}

	nvme_ctrlr_destruct_finish(ctrlr);
                                  /* [한국어] 공통 cleanup — namespace 해제, process_list 정리 */

	nvme_pcie_ctrlr_free_bars(pctrlr);
                                  /* [한국어] BAR + CMB + PMR unmap */

	if (devhandle) {
		if (ctrlr->opts.enable_interrupts) {
                                  /* [한국어] interrupt 모드였으면 VFIO MSI-X 해제 */
			spdk_pci_device_disable_interrupts(devhandle);
		}
		spdk_pci_device_unclaim(devhandle);
                                  /* [한국어] claim 해제 */
		spdk_pci_device_detach(devhandle);
                                  /* [한국어] DPDK 내부 장치 리스트에서 제거 */
	}

	spdk_free(pctrlr);
                                  /* [한국어] 컨트롤러 구조체 자체 해제 (SHARE hugepage) */

	return 0;
}

/*
 * [한국어] nvme_pcie_ctrlr_enable_interrupts - VFIO MSI-X 활성화 (optional).
 *
 * ctrlr->opts.enable_interrupts=true인 경우 ctrlr_ready 단계 후 호출.
 * DPDK VFIO eventfd-based MSI-X 벡터를 num_io_queues+1 개 (admin 1개 + I/O N개) 활성화.
 */
static int
nvme_pcie_ctrlr_enable_interrupts(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_pci_device *devhandle = nvme_ctrlr_proc_get_devhandle(ctrlr);
	int rc;

	assert(devhandle != NULL);
	rc = spdk_pci_device_enable_interrupts(devhandle, ctrlr->opts.num_io_queues);
                                  /* [한국어] VFIO_DEVICE_SET_IRQS ioctl 내부 호출로 MSI-X 벡터 N개 활성화.
                                   *         각 벡터는 per-qpair eventfd (qpair_get_fd가 반환) */
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "enable_interrupts() failed\n");
		return -EIO;
	}

	return 0;
}

/*
 * [한국어] nvme_pcie_qpair_iterate_requests - outstanding tracker 순회 (트랜스포트 vtable).
 *
 * nvme_transport_qpair_iterate_requests가 위임. 각 tracker의 req에 iter_fn 콜백 적용.
 * timeout 체크, error injection 조사, 디버그 dump 등에 사용.
 *
 * iter_fn이 non-zero 반환하면 순회 중단.
 */
static int
nvme_pcie_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
				 int (*iter_fn)(struct nvme_request *req, void *arg),
				 void *arg)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker *tr, *tmp;
                                  /* [한국어] FOREACH_SAFE용 — iter_fn 콜백이 tr을 제거해도 안전 */
	int rc;

	assert(iter_fn != NULL);

	TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, tmp) {
                                  /* [한국어] outstanding_tr 리스트 순회 (제출되었으나 미완료 tracker) */
		assert(tr->req != NULL);

		rc = iter_fn(tr->req, arg);
                                  /* [한국어] 사용자 콜백 호출 */
		if (rc != 0) {
                                  /* [한국어] non-zero 반환 시 순회 조기 종료 */
			return rc;
		}
	}

	return 0;
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_pcie_set_hotplug_filter - hotplug 필터 콜백 설정.
 *
 * 애플리케이션이 "어떤 traddr 장치만 hotplug ADD 허용할지" 제어 가능.
 * NULL 세팅(기본)이면 모든 NVMe PCIe 장치 허용.
 *
 * 사용처: 특정 BDF 장치만 감시하거나, 화이트리스트 기반 hotplug 정책 구현 시.
 */
void
spdk_nvme_pcie_set_hotplug_filter(spdk_nvme_pcie_hotplug_filter_cb filter_cb)
{
	g_hotplug_filter_cb = filter_cb;
                                  /* [한국어] 전역 콜백 포인터 갱신 — _nvme_pcie_event_process의 ADD 경로에서 참조 */
}

/*
 * [한국어] ★ nvme_pci_driver_id - DPDK PCI 드라이버 매칭 테이블.
 *
 * DPDK가 시스템의 모든 PCI 장치를 열거하면서 이 테이블과 매칭.
 * class_id=SPDK_PCI_CLASS_NVME (0x010802) — NVMe 표준 class code.
 * vendor/device/sub* 모두 ANY — 모든 벤더의 NVMe 장치 수용.
 * 마지막 sentinel {.vendor_id=0}로 테이블 끝 표시.
 */
static struct spdk_pci_id nvme_pci_driver_id[] = {
	{
		.class_id = SPDK_PCI_CLASS_NVME,
                                  /* [한국어] NVMe class code — PCI spec상 mass storage class + NVM Express subclass */
		.vendor_id = SPDK_PCI_ANY_ID,
		.device_id = SPDK_PCI_ANY_ID,
		.subvendor_id = SPDK_PCI_ANY_ID,
		.subdevice_id = SPDK_PCI_ANY_ID,
                                  /* [한국어] 모든 벤더 장치 허용 — NVMe class만 매칭되면 OK */
	},
	{ .vendor_id = 0, /* sentinel */ },
                                  /* [한국어] 테이블 종결자 */
};

/*
 * [한국어] ★ SPDK_PCI_DRIVER_REGISTER - DPDK에 NVMe PCI 드라이버 등록 매크로.
 *
 * constructor 함수로 전개되어 main() 전에 자동 실행 → DPDK에 "nvme" 이름으로 드라이버 등록.
 * spdk_pci_nvme_get_driver()가 이후 이 객체를 반환 → scan()에서 enumerate 시 사용.
 *
 * 플래그:
 *   NEED_MAPPING: BAR를 자동으로 mmap해두기 (SPDK는 어차피 BAR0 사용)
 *   WC_ACTIVATE:  Write Combining 활성화 — doorbell write 배칭으로 성능 향상
 *                 (인접 MMIO write들이 PCIe TLP로 합쳐져 전송됨)
 */
SPDK_PCI_DRIVER_REGISTER(nvme, nvme_pci_driver_id,
			 SPDK_PCI_DRIVER_NEED_MAPPING | SPDK_PCI_DRIVER_WC_ACTIVATE);

/*
 * [한국어] ★★★ pcie_ops - PCIe 트랜스포트의 공식 vtable.
 *
 * 이 구조체가 이 파일의 "퍼블릭 인터페이스"이자 nvme_transport.c의 vtable 디스패치 타겟.
 * 파일 맨 아래 SPDK_NVME_TRANSPORT_REGISTER 매크로로 전역 TAILQ에 등록.
 *
 * 구성 (기능별 그룹):
 *   name/type                         : 식별자
 *   ctrlr_construct/scan/destruct     : 수명주기
 *   ctrlr_enable/enable_interrupts    : 활성화
 *   ctrlr_get_registers/set_reg_*     : 레지스터 R/W
 *   ctrlr_get_max_xfer_size/sges      : 전송 제약
 *   ctrlr_reserve/map/unmap_cmb/pmr   : 장치 내부 메모리
 *   ctrlr_create/delete/connect/disconnect_qpair : qpair 관리
 *   qpair_abort_reqs/reset/submit_request/process_completions/
 *     iterate_requests/get_fd         : ★ hot-path (nvme_pcie_common.c)
 *   admin_qpair_abort_aers             : AER 정리
 *   poll_group_*                      : poll group 관리 (nvme_pcie_common.c)
 *
 * 대부분 이 파일(cold-path) 또는 nvme_pcie_common.c(hot-path)에 정의되어 있음.
 */
const struct spdk_nvme_transport_ops pcie_ops = {
	.name = "PCIE",
                                  /* [한국어] 문자열 식별자 — nvme_get_transport("PCIE")로 조회 */
	.type = SPDK_NVME_TRANSPORT_PCIE,
                                  /* [한국어] enum 식별자 */
	.ctrlr_construct = nvme_pcie_ctrlr_construct,
	.ctrlr_scan = nvme_pcie_ctrlr_scan,
	.ctrlr_scan_attached = nvme_pci_ctrlr_scan_attached,
	.ctrlr_destruct = nvme_pcie_ctrlr_destruct,
	.ctrlr_enable = nvme_pcie_ctrlr_enable,
	.ctrlr_enable_interrupts = nvme_pcie_ctrlr_enable_interrupts,
                                  /* [한국어] 수명주기 6종 (이 파일) */

	.ctrlr_get_registers = nvme_pcie_ctrlr_get_registers,
	.ctrlr_set_reg_4 = nvme_pcie_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_pcie_ctrlr_set_reg_8,
	.ctrlr_get_reg_4 = nvme_pcie_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_pcie_ctrlr_get_reg_8,
                                  /* [한국어] 레지스터 R/W 5종 (이 파일, MMIO 직접) */

	.ctrlr_get_max_xfer_size = nvme_pcie_ctrlr_get_max_xfer_size,
	.ctrlr_get_max_sges = nvme_pcie_ctrlr_get_max_sges,
                                  /* [한국어] 전송 제약 2종 */

	.ctrlr_reserve_cmb = nvme_pcie_ctrlr_reserve_cmb,
	.ctrlr_map_cmb = nvme_pcie_ctrlr_map_io_cmb,
	.ctrlr_unmap_cmb = nvme_pcie_ctrlr_unmap_io_cmb,
                                  /* [한국어] CMB 3종 (PCIe 전용 기능) */

	.ctrlr_enable_pmr = nvme_pcie_ctrlr_enable_pmr,
	.ctrlr_disable_pmr = nvme_pcie_ctrlr_disable_pmr,
	.ctrlr_map_pmr = nvme_pcie_ctrlr_map_io_pmr,
	.ctrlr_unmap_pmr = nvme_pcie_ctrlr_unmap_io_pmr,
                                  /* [한국어] PMR 4종 (NVMe 1.4+ 비휘발성 메모리 영역) */

	.ctrlr_create_io_qpair = nvme_pcie_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_pcie_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_pcie_ctrlr_connect_qpair,
	.ctrlr_disconnect_qpair = nvme_pcie_ctrlr_disconnect_qpair,
                                  /* [한국어] qpair 수명주기 4종 (nvme_pcie_common.c — Create/Delete IO SQ/CQ admin 체인) */

	.qpair_abort_reqs = nvme_pcie_qpair_abort_reqs,
	.qpair_reset = nvme_pcie_qpair_reset,
	.qpair_submit_request = nvme_pcie_qpair_submit_request,
                                  /* [한국어] ★★★ I/O hot-path 제출 진입점 — nvme_transport_qpair_submit_request가 여기로 dispatch */
	.qpair_process_completions = nvme_pcie_qpair_process_completions,
                                  /* [한국어] ★★★ I/O hot-path 완료 폴링 — phase bit polling의 실체 */
	.qpair_iterate_requests = nvme_pcie_qpair_iterate_requests,
                                  /* [한국어] 이 파일 정의 (위) */
	.qpair_get_fd = nvme_pcie_qpair_get_fd,
                                  /* [한국어] interrupt-mode용 eventfd (nvme_pcie_common.c) */
	.admin_qpair_abort_aers = nvme_pcie_admin_qpair_abort_aers,
                                  /* [한국어] AER 전용 abort (nvme_pcie_common.c) */

	.poll_group_create = nvme_pcie_poll_group_create,
	.poll_group_connect_qpair = nvme_pcie_poll_group_connect_qpair,
	.poll_group_disconnect_qpair = nvme_pcie_poll_group_disconnect_qpair,
	.poll_group_add = nvme_pcie_poll_group_add,
	.poll_group_remove = nvme_pcie_poll_group_remove,
	.poll_group_process_completions = nvme_pcie_poll_group_process_completions,
	.poll_group_check_disconnected_qpairs = nvme_pcie_poll_group_check_disconnected_qpairs,
	.poll_group_destroy = nvme_pcie_poll_group_destroy,
	.poll_group_get_stats = nvme_pcie_poll_group_get_stats,
	.poll_group_free_stats = nvme_pcie_poll_group_free_stats
                                  /* [한국어] poll group 10종 (nvme_pcie_common.c — 순차 순회 기반) */
};

/*
 * [한국어] ★★★ SPDK_NVME_TRANSPORT_REGISTER(pcie, &pcie_ops) ★★★
 *
 * 이 매크로가 이 파일 전체의 "존재 이유"를 확정한다.
 * __attribute__((constructor)) 함수로 전개되어 main() 진입 전에 자동 실행:
 *   → spdk_nvme_transport_register(&pcie_ops)
 *   → nvme_transport.c의 g_spdk_nvme_transports TAILQ에 PCIe ops 삽입
 *
 * 이 등록 이후부터 상위 레이어가 nvme_get_transport("PCIE")로 조회 가능해지고,
 * nvme_transport_qpair_submit_request 등의 vtable dispatch가 이 파일의 함수들로 연결됨.
 *
 * 등록 시점: 프로세스 로드 시 (libc ctor) — 어떤 spdk_nvme_* 호출보다 먼저.
 */
SPDK_NVME_TRANSPORT_REGISTER(pcie, &pcie_ops);
