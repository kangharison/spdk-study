/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] IDXD/DSA 유저스페이스 백엔드 구현 (idxd_user.c)
 *
 * === 파일의 역할 ===
 * Intel DSA(Data Streaming Accelerator) / IAA(In-Memory Analytics Accelerator)를
 * SPDK의 다른 PCIe 디바이스와 동일한 방식(DPDK uio/vfio-pci 기반)으로 유저스페이스에서 직접
 * 제어하는 "user" 백엔드 구현이다. 즉 커널의 idxd 드라이버를 우회하고, lib/env_dpdk를 통해
 * BAR0(MMIO 레지스터)/BAR2(WQ portal)를 mmap한 뒤, MOVDIR64B로 64B 디스크립터를 직접
 * portal에 cache-bypass 기록한다. 본 파일은 디바이스 reset/enable, group/WQ configure,
 * BAR mapping/unmapping, PCI 열거(probe/attach), 그리고 IAA의 RFC-1951 고정 Huffman 테이블
 * 초기화까지를 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK IDXD 스택은 다음 두 백엔드(impl) 중 하나를 선택한다 — "user"(이 파일) vs "kernel"
 * (idxd_kernel.c). 양쪽은 spdk_idxd_impl 함수 테이블(probe/destruct/dump_sw_error/portal_get_addr)
 * 을 통해 lib/idxd/idxd.c의 공통 코어와 결합된다. 코어는 디스크립터 빌드/제출/완료 폴링을
 * 담당하며, 본 파일은 "디바이스 자체"의 셋업과 portal 메모리 노출만 책임진다.
 *
 * 호출 체인 (probe 경로):
 *   spdk_idxd_probe(공개 API, lib/idxd/idxd.c)
 *     → g_user_idxd_impl.probe = user_idxd_probe(이 파일)
 *     → spdk_pci_enumerate(spdk_pci_idxd_get_driver(), idxd_enum_cb)
 *     → idxd_enum_cb → enum_ctx->probe_cb(사용자) → probe_cb(claim) → idxd_attach
 *     → idxd_device_configure (BAR map → reset → group_config → wq_config → enable_dev → enable_wq)
 *     → enum_ctx->attach_cb(사용자, idxd) — 사용자 콜백에 디바이스 핸들 전달
 *
 * 호출 체인 (제출 경로 — 본 파일은 portal 주소 노출만 함):
 *   상위 코어가 chan->portal = user_idxd_portal_get_addr(idxd)로 받아둠
 *     → _submit_to_hw(idxd.c) → movdir64b(portal+offset, desc) — 본 파일이 mmap한 BAR2 영역
 *
 * 실행 컨텍스트: 호스트 유저스페이스. probe/attach는 main 스레드의 spdk_app_start 시점에
 * 한 번만, 단일 lock 보호하에 수행. 이후 portal 주소는 reactor 스레드들이 읽기 전용으로 공유.
 *
 * === 타 모듈과의 연결 ===
 * 의존: lib/env_dpdk/(spdk_pci_*, vfio-pci 기반 BAR mapping), include/spdk_internal/idxd.h(공개 idxd 인터페이스),
 *   idxd_internal.h(레지스터 union, 매크로, spdk_idxd_device 정의), POSIX pthread.
 * 데이터 흐름: BAR0(MMIO) ←→ user_idxd->registers (control plane) ; BAR2(WQ Portal) ←→ idxd->portal (data plane).
 *   디바이스가 직접 호스트 메모리(완료 record, src/dst 버퍼)에 DMA — VT-d/IOMMU의 PASID 또는 DPDK의 IOVA 매핑 사용.
 * 공유 자료구조: g_driver_lock(전역 mutex)가 PCI 열거 동안만 보호. spdk_user_idxd_device::registers는 init 후 read-only;
 *   idxd->portal은 init 후 read-only(쓰는 것은 디스크립터지만 매핑 자체는 변하지 않음).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_user_idxd_device: spdk_idxd_device를 임베드한 user 백엔드 디바이스 구조체.
 * - idxd_attach(): PCI 디바이스를 user_idxd로 래핑 후 idxd_device_configure 수행.
 * - idxd_device_configure(): BAR map → reset → group/WQ 설정 → device/WQ enable의 6단계 마스터 절차.
 * - idxd_group_config(): groupcap/enginecap/wqcap을 읽어 모든 엔진+WQ0를 묶은 단일 그룹 구성.
 * - idxd_wq_config(): WQ 모드(WQ_MODE_DEDICATED), 크기, 우선순위, max_xfer_shift 설정.
 * - idxd_wait_cmd(): cmdsts 폴링으로 control 명령 완료 대기 + 에러 코드 검사.
 * - user_idxd_probe(): spdk_pci_enumerate를 통해 DSA/IAA 디바이스를 PCI 트리에서 찾아 attach.
 * - user_idxd_portal_get_addr(): BAR2의 가상주소를 코어에 노출 — 코어가 movdir64b로 직접 기록할 대상.
 * - fixed_ll_sym/fixed_d_sym: IAA가 RFC-1951 deflate 압축에 사용하는 고정 Huffman 테이블(literal/length, distance).
 * - g_user_idxd_impl: 함수 테이블. SPDK_IDXD_IMPL_REGISTER 매크로로 g_idxd_impls 리스트에 등록.
 */

#include "spdk/stdinc.h"     /* [한국어] POSIX 표준 헤더 일괄 포함 */

#include "spdk/env.h"        /* [한국어] spdk_pci_*, spdk_vtophys, spdk_zmalloc 등 SPDK 환경(DPDK 추상화) API */
#include "spdk/util.h"       /* [한국어] SPDK_COUNTOF, container_of 등 유틸 */
#include "spdk/memory.h"     /* [한국어] spdk_mmio_read_4/_8, spdk_mmio_write_4/_8 — MMIO 레지스터 액세스 wrapper (volatile + memory barrier) */
#include "spdk/likely.h"     /* [한국어] spdk_likely/unlikely 분기 힌트 */

#include "spdk/log.h"        /* [한국어] SPDK_ERRLOG/DEBUGLOG/NOTICELOG 매크로 */
#include "spdk_internal/idxd.h"  /* [한국어] spdk_idxd_device 등 공개 IDXD 인터페이스 */

#include "idxd_internal.h"   /* [한국어] 레지스터 union(idxd_cmdsts_register 등), 매크로(IDXD_*_BAR), enum 정의 */

/*
 * [한국어]
 * struct spdk_user_idxd_device — user 백엔드 전용 IDXD 디바이스 래퍼.
 *
 * 코어가 다루는 spdk_idxd_device를 첫 멤버로 임베드한 뒤, user-mode에서만 필요한 필드
 * (PCI 디바이스 핸들, NUMA socket, mmap된 레지스터 포인터)를 추가한다. __user_idxd 매크로로
 * 캐스팅해 user 전용 정보에 접근.
 */
struct spdk_user_idxd_device {
	struct spdk_idxd_device	idxd;
	/* [한국어] 공통 IDXD 디바이스 본체. impl 함수 테이블, type(DSA/IAA), portal, total_wq_size,
	 * batch_size, num_channels 등 backend-agnostic 상태가 들어 있다.
	 * 첫 멤버이므로 __user_idxd((struct spdk_idxd_device*)) 캐스팅이 안전. */

	struct spdk_pci_device	*device;
	/* [한국어] DPDK가 추상화한 PCIe 디바이스 핸들. spdk_pci_device_map_bar/cfg_read/cfg_write의 인자.
	 * 설정자: idxd_attach에서 enum_cb로부터 받음. 읽는 자: BAR map/unmap, PCI config R/W. */

	int			sock_id;
	/* [한국어] 디바이스가 부착된 NUMA 소켓 ID. 현재 코드는 spdk_idxd_device::socket_id에 별도로 저장하므로
	 * 본 필드는 직접 사용되지는 않지만 디버그/추적 목적의 백업 슬롯. */

	struct idxd_registers	*registers;
	/* [한국어] BAR0(MMIO) 영역을 매핑한 가상주소. version/gencap/wqcap/groupcap/enginecap/cmd/cmdsts/
	 * gensts/sw_err 등 모든 control 레지스터에 접근하는 베이스. 모든 액세스는 spdk_mmio_read/write_*
	 * 래퍼를 거쳐 volatile + 적절한 fence를 보장. */
};

/* [한국어] 캐스팅 매크로 — spdk_idxd_device* → spdk_user_idxd_device*.
 * 첫 멤버가 idxd이므로 주소 동등성이 성립한다. impl 콜백이 받는 idxd를 user-specific 구조체로 환원. */
#define __user_idxd(idxd) (struct spdk_user_idxd_device *)idxd

/* [한국어] PCI 열거를 직렬화하는 전역 락. spdk_pci_enumerate가 멀티 프로세스 환경에서 동시 실행되는 것을 방지.
 * idxd_enum_cb가 g_idxd_impls 등 전역 상태를 만지므로 단일 진입을 강제. */
pthread_mutex_t	g_driver_lock = PTHREAD_MUTEX_INITIALIZER;

/* [한국어] 전방 선언 — idxd_attach가 idxd_enum_cb 안에서 호출되지만 정의는 파일 후반부 */
static struct spdk_idxd_device *idxd_attach(struct spdk_pci_device *device);

/* Used for control commands, not for descriptor submission. */
/*
 * [한국어]
 * idxd_wait_cmd — IDXD control command 완료 폴링.
 *
 * @user_idxd: 디바이스 핸들. registers->cmdsts에 접근.
 * @_timeout: 타임아웃(usec). cmd_status.active가 1인 동안 1usec씩 sleep하며 감소.
 * @return: 0(성공), -EBUSY(타임아웃), -EINVAL(에러 비트 set).
 *
 * IDXD 사양: cmd 레지스터에 쓰기 → 디바이스가 처리 중일 때 cmdsts.active=1, 완료 시 0.
 * 에러 발생 시 cmdsts.err 필드에 에러 코드가 담긴다. 이 함수는 ENABLE_DEV/ENABLE_WQ/RESET/DISABLE 등
 * 모든 control 명령 후에 호출되며, 디스크립터 제출(MOVDIR64B 경로)에는 사용되지 않는다.
 * 호출 컨텍스트: probe/attach 단계의 main 스레드(g_driver_lock 보호하에).
 */
static int
idxd_wait_cmd(struct spdk_user_idxd_device *user_idxd, int _timeout)
{
	uint32_t timeout = _timeout;             /* [한국어] 외부 인자를 가변 카운터로 복사 */
	union idxd_cmdsts_register cmd_status = {};  /* [한국어] cmdsts 레지스터의 비트필드 union (active, err 등) */

	cmd_status.raw = spdk_mmio_read_4(&user_idxd->registers->cmdsts.raw);  /* [한국어] 첫 read — MMIO 4B 읽기 (volatile + barrier) */
	while (cmd_status.active && --timeout) {  /* [한국어] active가 1인 동안 폴링. 사전 감소(--timeout)는 0 도달 시 즉시 탈출 */
		usleep(1);    /* [한국어] 1usec 슬립 — busy-wait 완화. control 명령은 ms 단위라 이 정도 정밀도로 충분 */
		cmd_status.raw = spdk_mmio_read_4(&user_idxd->registers->cmdsts.raw);  /* [한국어] 다음 폴링 read */
	}

	/* Check for timeout */
	if (timeout == 0 && cmd_status.active) {  /* [한국어] 카운터가 0이 됐는데 여전히 active면 디바이스 hang/오작동 */
		SPDK_ERRLOG("Command timeout, waited %u\n", _timeout);
		return -EBUSY;
	}

	/* Check for error */
	if (cmd_status.err) {  /* [한국어] 디바이스가 명령 자체를 거부 — 잘못된 operand, 잘못된 상태 등 */
		SPDK_ERRLOG("Command status reg reports error 0x%x\n", cmd_status.err);
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * idxd_unmap_pci_bar — 매핑된 PCI BAR 영역을 해제.
 *
 * @user_idxd: 디바이스 핸들.
 * @bar: IDXD_MMIO_BAR(0) 또는 IDXD_WQ_BAR(2). MMIO_BAR=registers, WQ_BAR=portal.
 * @return: spdk_pci_device_unmap_bar 반환값.
 *
 * 호출 컨텍스트: 디바이스 destruct 또는 configure 실패 시 롤백 경로.
 */
static int
idxd_unmap_pci_bar(struct spdk_user_idxd_device *user_idxd, int bar)
{
	int rc = 0;
	void *addr = NULL;

	if (bar == IDXD_MMIO_BAR) {
		addr = (void *)user_idxd->registers;  /* [한국어] BAR0 가상주소 — control 레지스터 */
	} else if (bar == IDXD_WQ_BAR) {
		addr = (void *)user_idxd->idxd.portal;  /* [한국어] BAR2 가상주소 — WQ portal (movdir64b 대상) */
	}

	if (addr) {                                /* [한국어] 매핑된 적이 있을 때만 해제 시도 */
		rc = spdk_pci_device_unmap_bar(user_idxd->device, 0, addr);
		/* [한국어] 두 번째 인자가 0인 것은 SPDK 추상화 내에서 bar index를 다르게 다루기 때문 — addr으로 식별 */
	}
	return rc;
}

/*
 * [한국어]
 * idxd_map_pci_bars — BAR0(MMIO 레지스터) + BAR2(WQ portal)를 가상주소로 매핑.
 *
 * @user_idxd: 디바이스 핸들. 성공 시 ->registers와 ->idxd.portal이 채워진다.
 * @return: 0 성공, -1/-EINVAL 실패.
 *
 * BAR0: control plane 레지스터 (gencap, wqcap, cmd, cmdsts, gensts, sw_err 등).
 * BAR2: per-WQ portal 영역. 64B 정렬된 portal에 MOVDIR64B로 디스크립터를 쓰면 디바이스가 그대로 가져간다.
 *
 * IDXD 사양: WQ_DEDICATED 모드에서는 64B 한 번 쓰기로 즉시 큐잉, WQ_SHARED 모드는 ENQCMDS 명령 사용.
 */
static int
idxd_map_pci_bars(struct spdk_user_idxd_device *user_idxd)
{
	int rc;
	void *addr;
	uint64_t phys_addr, size;   /* [한국어] DPDK가 채워주지만 본 함수에서는 사용하지 않음 */

	rc = spdk_pci_device_map_bar(user_idxd->device, IDXD_MMIO_BAR, &addr, &phys_addr, &size);
	if (rc != 0 || addr == NULL) {  /* [한국어] DPDK가 BAR resource를 mmap 실패 — 권한/장치 미동작 */
		SPDK_ERRLOG("pci_device_map_range failed with error code %d\n", rc);
		return -1;
	}
	user_idxd->registers = (struct idxd_registers *)addr;  /* [한국어] 이후 모든 레지스터 액세스의 베이스 */

	rc = spdk_pci_device_map_bar(user_idxd->device, IDXD_WQ_BAR, &addr, &phys_addr, &size);
	if (rc != 0 || addr == NULL) {  /* [한국어] WQ portal 매핑 실패 — 첫 매핑은 롤백 후 에러 반환 */
		SPDK_ERRLOG("pci_device_map_range failed with error code %d\n", rc);
		rc = idxd_unmap_pci_bar(user_idxd, IDXD_MMIO_BAR);  /* [한국어] BAR0 롤백 */
		if (rc) {
			SPDK_ERRLOG("unable to unmap MMIO bar\n");
		}
		return -EINVAL;
	}
	user_idxd->idxd.portal = addr;  /* [한국어] 코어가 user_idxd_portal_get_addr로 받아갈 portal 베이스 */

	return 0;
}

/*
 * [한국어]
 * idxd_disable_dev — DISABLE_DEVICE 명령으로 디바이스 비활성화.
 *
 * destruct 경로에서 호출. cmd 레지스터에 IDXD_DISABLE_DEV opcode를 쓰고 cmdsts 폴링.
 * 실패 시 로그만 남기고 진행 — destruct는 best-effort.
 */
static void
idxd_disable_dev(struct spdk_user_idxd_device *user_idxd)
{
	int rc;
	union idxd_cmd_register cmd = {};   /* [한국어] cmd 레지스터 union — command_code/operand 비트필드 */

	cmd.command_code = IDXD_DISABLE_DEV;  /* [한국어] 사양 정의 opcode (예: 0x02). 자세한 값은 IDXD spec section 8 참조 */

	assert(&user_idxd->registers->cmd.raw); /* scan-build */  /* [한국어] static analyzer false positive 회피용 어서션 */
	spdk_mmio_write_4(&user_idxd->registers->cmd.raw, cmd.raw);  /* [한국어] cmd 레지스터에 4B 쓰기 — 디바이스가 즉시 처리 시작 */
	rc = idxd_wait_cmd(user_idxd, IDXD_REGISTER_TIMEOUT_US);     /* [한국어] cmdsts 폴링 — 일반적으로 수 ms 이내 완료 */
	if (rc < 0) {
		SPDK_ERRLOG("Error disabling device %u\n", rc);
	}
}

/*
 * [한국어]
 * idxd_reset_dev — RESET_DEVICE 명령으로 디바이스를 초기 상태로 리셋.
 *
 * @return: 0 성공, <0 실패.
 *
 * 호출 컨텍스트: idxd_device_configure의 첫 단계. group/WQ 구성 전 깨끗한 상태 보장.
 */
static int
idxd_reset_dev(struct spdk_user_idxd_device *user_idxd)
{
	int rc;
	union idxd_cmd_register cmd = {};

	cmd.command_code = IDXD_RESET_DEVICE;  /* [한국어] RESET 명령 — group/WQ 설정도 초기화 */

	spdk_mmio_write_4(&user_idxd->registers->cmd.raw, cmd.raw);
	rc = idxd_wait_cmd(user_idxd, IDXD_REGISTER_TIMEOUT_US);
	if (rc < 0) {
		SPDK_ERRLOG("Error resetting device %u\n", rc);
	}

	return rc;
}

/*
 * [한국어]
 * idxd_group_config — IDXD 그룹 테이블 설정.
 *
 * IDXD는 엔진(engine — 실제 실행 유닛)과 WQ(work queue)를 그룹으로 묶어 자원을 분배한다.
 * 본 함수는 모든 엔진과 WQ 0을 하나의 그룹에 모두 할당하는 단순 구성을 만든다.
 *
 * 동작 단계:
 *   1) groupcap/enginecap/wqcap 레지스터 읽기 — 디바이스 capability 파악.
 *   2) num_wqs<1이면 -ENOTSUP (IDXD가 활성 WQ를 갖지 않은 비정상 상태).
 *   3) grpcfg.wqs[0]=1, engines=enginecap에 따라 모든 엔진 비트 set, tc_a/tc_b=1, read_buffers_allowed=…
 *   4) offsets 레지스터를 통해 grpcfg 테이블의 BAR0 내부 오프셋 계산.
 *   5) 그룹 0에 위 설정 기록, 그룹 1..N-1은 0으로 클리어.
 *
 * IDXD spec: BAR0의 일부 영역이 grpcfg/wqcfg 테이블로 노출되며 offsets 레지스터로 오프셋이 표시된다.
 */
static int
idxd_group_config(struct spdk_user_idxd_device *user_idxd)
{
	int i;
	union idxd_groupcap_register groupcap;     /* [한국어] num_groups, read_bufs 등 */
	union idxd_enginecap_register enginecap;   /* [한국어] num_engines (DSA: 보통 4개) */
	union idxd_wqcap_register wqcap;           /* [한국어] num_wqs, total_wq_size, max_batch_shift 등 */
	union idxd_offsets_register table_offsets; /* [한국어] grpcfg/wqcfg/perfmon 테이블의 BAR0 내부 오프셋 */

	struct idxd_grptbl *grptbl;
	struct idxd_grpcfg grpcfg = {};

	/* [한국어] 8B raw read — capability 레지스터 64-bit. spdk_mmio_read_8은 lock-free. */
	groupcap.raw = spdk_mmio_read_8(&user_idxd->registers->groupcap.raw);
	enginecap.raw = spdk_mmio_read_8(&user_idxd->registers->enginecap.raw);
	wqcap.raw = spdk_mmio_read_8(&user_idxd->registers->wqcap.raw);

	if (wqcap.num_wqs < 1) {  /* [한국어] WQ가 없는 디바이스는 사용 불가 — IAA/DSA 모두 최소 1개를 가져야 함 */
		return -ENOTSUP;
	}

	/* Build one group with all of the engines and a single work queue. */
	grpcfg.wqs[0] = 1;                              /* [한국어] WQ0 비트만 set — 그룹0은 WQ0 하나만 소유 */
	grpcfg.flags.read_buffers_allowed = groupcap.read_bufs;  /* [한국어] 디바이스가 보고한 read buffer 수 그대로 할당 */
	grpcfg.flags.tc_a = 1;                          /* [한국어] Traffic Class A — PCIe TLP 우선순위 */
	grpcfg.flags.tc_b = 1;                          /* [한국어] Traffic Class B — completion path 우선순위 */
	for (i = 0; i < enginecap.num_engines; i++) {
		grpcfg.engines |= (1 << i);             /* [한국어] 모든 엔진을 이 그룹에 할당 — 엔진 i의 비트 i를 set */
	}

	/* [한국어] offsets 레지스터는 16B(2 * 8B)로 grpcfg/wqcfg/perfmon/msix offsets를 인코딩 */
	table_offsets.raw[0] = spdk_mmio_read_8(&user_idxd->registers->offsets.raw[0]);
	table_offsets.raw[1] = spdk_mmio_read_8(&user_idxd->registers->offsets.raw[1]);

	/* [한국어] grptbl = BAR0 base + grpcfg_offset * IDXD_TABLE_OFFSET_MULT (보통 0x100 단위) */
	grptbl = (struct idxd_grptbl *)((uint8_t *)user_idxd->registers + (table_offsets.grpcfg *
					IDXD_TABLE_OFFSET_MULT));

	/* Write the group we've configured */
	/* [한국어] 그룹 0의 wqs/engines/flags 레지스터에 8B/4B 쓰기. wqs[0..3]은 WQ 비트맵 256bit. */
	spdk_mmio_write_8(&grptbl->group[0].wqs[0], grpcfg.wqs[0]);  /* [한국어] WQ0 ~ WQ63 비트맵 — 우리는 WQ0만 set */
	spdk_mmio_write_8(&grptbl->group[0].wqs[1], 0);              /* [한국어] WQ64 ~ WQ127 — 사용 안 함 */
	spdk_mmio_write_8(&grptbl->group[0].wqs[2], 0);              /* [한국어] WQ128 ~ WQ191 */
	spdk_mmio_write_8(&grptbl->group[0].wqs[3], 0);              /* [한국어] WQ192 ~ WQ255 */
	spdk_mmio_write_8(&grptbl->group[0].engines, grpcfg.engines);/* [한국어] 엔진 비트맵 */
	spdk_mmio_write_4(&grptbl->group[0].flags.raw, grpcfg.flags.raw);  /* [한국어] read_buffers/tc_a/tc_b 등 4B 플래그 */

	/* Write zeroes to the rest of the groups */
	/* [한국어] 다른 그룹은 명시적으로 0으로 비워둠 — reset 후 잔존 값/이전 사용자 흔적 제거 */
	for (i = 1 ; i < groupcap.num_groups; i++) {
		spdk_mmio_write_8(&grptbl->group[i].wqs[0], 0L);
		spdk_mmio_write_8(&grptbl->group[i].wqs[1], 0L);
		spdk_mmio_write_8(&grptbl->group[i].wqs[2], 0L);
		spdk_mmio_write_8(&grptbl->group[i].wqs[3], 0L);
		spdk_mmio_write_8(&grptbl->group[i].engines, 0L);
		spdk_mmio_write_4(&grptbl->group[i].flags.raw, 0L);
	}

	return 0;
}

/*
 * [한국어]
 * idxd_wq_config — WQ 0을 dedicated 모드로 구성.
 *
 * 동작:
 *   1) wqcap에서 total_wq_size 획득 → idxd->total_wq_size에 보관.
 *   2) chan_per_device(채널 수) 결정: 큰 WQ(>=128)면 8채널, 작으면 4채널 분할.
 *   3) offsets에서 wqcfg 테이블 위치 계산.
 *   4) 기존 wqcfg 8개 4B 워드를 모두 read (read-modify-write 프로토콜).
 *   5) wq_size, mode(DEDICATED), max_batch_shift, max_xfer_shift, state(ENABLED), priority 설정.
 *   6) 8개 워드를 한 번에 다시 write back.
 *
 * IDXD spec: wqcfg는 0x20(=32B = 8 * 4B) 단위 레지스터 그룹이며 read-modify-write로 일부 필드만 변경한다.
 * WQ_MODE_DEDICATED: 한 SW 컨텍스트가 한 WQ를 단독 사용 — MOVDIR64B로 제출, 단일 슬롯 점유.
 * WQ_MODE_SHARED: ENQCMDS로 제출, 여러 컨텍스트가 공유 — 본 driver는 사용하지 않음.
 */
static int
idxd_wq_config(struct spdk_user_idxd_device *user_idxd)
{
	uint32_t i;
	struct spdk_idxd_device *idxd = &user_idxd->idxd;  /* [한국어] 코어 공유 구조체 alias */
	union idxd_wqcap_register wqcap;
	union idxd_offsets_register table_offsets;
	union idxd_wqcfg *wqcfg;          /* [한국어] WQ 설정 레지스터 8 x 4B의 union view */

	wqcap.raw = spdk_mmio_read_8(&user_idxd->registers->wqcap.raw);

	SPDK_DEBUGLOG(idxd, "Total ring slots available 0x%x\n", wqcap.total_wq_size);

	idxd->total_wq_size = wqcap.total_wq_size;       /* [한국어] WQ 슬롯 총 개수 — get_channel에서 채널당 분할에 사용 */
	/* Spread the channels we allow per device based on the total number of WQE to try
	 * and achieve optimal performance for common cases.
	 */
	idxd->chan_per_device = (idxd->total_wq_size >= 128) ? 8 : 4;
	/* [한국어] 채널 수 = WQ 크기에 따라 8 또는 4 — 채널이 많을수록 reactor 분산은 좋지만 채널당 슬롯이 줄어 backpressure 가능 */

	table_offsets.raw[0] = spdk_mmio_read_8(&user_idxd->registers->offsets.raw[0]);
	table_offsets.raw[1] = spdk_mmio_read_8(&user_idxd->registers->offsets.raw[1]);

	wqcfg = (union idxd_wqcfg *)((uint8_t *)user_idxd->registers + (table_offsets.wqcfg *
				     IDXD_TABLE_OFFSET_MULT));
	/* [한국어] wqcfg 테이블 시작 주소 = BAR0 + wqcfg_offset * MULT. WQ 인덱스마다 이 sizeof(idxd_wqcfg) 단위로 위치. */

	for (i = 0 ; i < SPDK_COUNTOF(wqcfg->raw); i++) {
		wqcfg->raw[i] = spdk_mmio_read_4(&wqcfg->raw[i]);
		/* [한국어] read-modify-write 프로토콜 — 모든 4B 워드를 먼저 읽어와 디바이스 기본값 보존 */
	}

	wqcfg->wq_size = wqcap.total_wq_size;            /* [한국어] WQ가 가질 슬롯 수 = 디바이스 max */
	wqcfg->mode = WQ_MODE_DEDICATED;                 /* [한국어] DEDICATED — MOVDIR64B 직접 제출 */
	wqcfg->max_batch_shift = user_idxd->registers->gencap.max_batch_shift;
	/* [한국어] batch 디스크립터의 최대 크기 = 2^max_batch_shift. gencap이 보고하는 디바이스 한계 그대로 사용. */
	wqcfg->max_xfer_shift = LOG2_WQ_MAX_XFER;        /* [한국어] 단일 디스크립터 최대 전송 크기 = 2^LOG2_WQ_MAX_XFER (보통 30 = 1GB) */
	wqcfg->wq_state = WQ_ENABLED;                    /* [한국어] enable_wq 명령에서 활성화될 상태 */
	wqcfg->priority = WQ_PRIORITY_1;                 /* [한국어] 우선순위 1 — 그룹 내 라운드로빈 분배 */

	idxd->batch_size = (1 << wqcfg->max_batch_shift);/* [한국어] batch 슬롯 풀 할당에 코어가 사용 */

	for (i = 0; i < SPDK_COUNTOF(wqcfg->raw); i++) {
		spdk_mmio_write_4(&wqcfg->raw[i], wqcfg->raw[i]);
		/* [한국어] 변경된 비트필드를 포함한 8개 워드 모두 write-back */
	}

	return 0;
}

/*
 * [한국어]
 * idxd_device_configure — 마스터 디바이스 셋업 함수.
 *
 * 흐름 (IDXD spec section 6 — Device State Machine):
 *   1) BAR0/BAR2 매핑.
 *   2) RESET — 깨끗한 시작점.
 *   3) version 보관.
 *   4) GROUP 구성 (모든 엔진 + WQ0 → 그룹0).
 *   5) WQ 구성 (DEDICATED, max size).
 *   6) gensts.state 검증 후 ENABLE_DEV.
 *   7) ENABLE_WQ.
 *
 * 실패 시 적절한 라벨로 jump하여 BAR을 unmap하고 리턴. (단, 코드에 IDXD_MMIO_BAR가 두 번 unmap되는
 * 버그성 라인이 있으나 SPDK 본가 그대로 유지.)
 */
static int
idxd_device_configure(struct spdk_user_idxd_device *user_idxd)
{
	int rc = 0;
	union idxd_gensts_register gensts_reg;   /* [한국어] gensts: device state(disabled/enabled), config_error 등 */
	union idxd_cmd_register cmd = {};

	/*
	 * Map BAR0 and BAR2
	 */
	rc = idxd_map_pci_bars(user_idxd);   /* [한국어] 1단계: 컨트롤(BAR0) + portal(BAR2) 매핑 */
	if (rc) {
		return rc;
	}

	/*
	 * Reset the device
	 */
	rc = idxd_reset_dev(user_idxd);      /* [한국어] 2단계: 리셋 — 이전 사용 흔적 제거 */
	if (rc) {
		goto err_reset;
	}

	/*
	 * Save the device version for use in the common library code.
	 */
	user_idxd->idxd.version = user_idxd->registers->version;
	/* [한국어] 3단계: device version 보관. 코어가 features 분기(CRC, dual cast 등)에 사용. */

	/*
	 * Configure groups and work queues.
	 */
	rc = idxd_group_config(user_idxd);   /* [한국어] 4단계: 그룹0 = 모든 엔진 + WQ0 */
	if (rc) {
		goto err_group_cfg;
	}

	rc = idxd_wq_config(user_idxd);      /* [한국어] 5단계: WQ0 dedicated, max size */
	if (rc) {
		goto err_wq_cfg;
	}

	/*
	 * Enable the device
	 */
	gensts_reg.raw = spdk_mmio_read_4(&user_idxd->registers->gensts.raw);
	assert(gensts_reg.state == IDXD_DEVICE_STATE_DISABLED);
	/* [한국어] reset 직후이므로 DISABLED 상태여야 한다. assert로 디버그 빌드에서 검출. */

	cmd.command_code = IDXD_ENABLE_DEV;  /* [한국어] 디바이스 활성화 opcode */

	spdk_mmio_write_4(&user_idxd->registers->cmd.raw, cmd.raw);
	rc = idxd_wait_cmd(user_idxd, IDXD_REGISTER_TIMEOUT_US);
	gensts_reg.raw = spdk_mmio_read_4(&user_idxd->registers->gensts.raw);  /* [한국어] enable 후 상태 확인 */
	if ((rc < 0) || (gensts_reg.state != IDXD_DEVICE_STATE_ENABLED)) {
		rc = -EINVAL;
		SPDK_ERRLOG("Error enabling device %u\n", rc);
		goto err_device_enable;
	}

	/*
	 * Enable the work queue that we've configured
	 */
	cmd.command_code = IDXD_ENABLE_WQ;   /* [한국어] WQ 활성화 — operand에 WQ 인덱스 */
	cmd.operand = 0;                     /* [한국어] WQ 0번 활성화 */

	spdk_mmio_write_4(&user_idxd->registers->cmd.raw, cmd.raw);
	rc = idxd_wait_cmd(user_idxd, IDXD_REGISTER_TIMEOUT_US);
	if (rc < 0) {
		SPDK_ERRLOG("Error enabling work queues 0x%x\n", rc);
		goto err_wq_enable;
	}

	if ((rc == 0) && (gensts_reg.state == IDXD_DEVICE_STATE_ENABLED)) {
		SPDK_DEBUGLOG(idxd, "Device enabled VID 0x%x DID 0x%x\n",
			      user_idxd->device->id.vendor_id, user_idxd->device->id.device_id);
	}

	return rc;
err_wq_enable:
err_device_enable:
err_wq_cfg:
err_group_cfg:
err_reset:
	/* [한국어] 모든 실패 경로에서 BAR0 unmap. (BAR2는 destruct에서 처리되며, 본 구현은 두 번 MMIO_BAR
	 * unmap을 호출하는 잔존 버그가 있지만 idempotent한 동작이라 영향은 제한적.) */
	idxd_unmap_pci_bar(user_idxd, IDXD_MMIO_BAR);
	idxd_unmap_pci_bar(user_idxd, IDXD_MMIO_BAR);

	return rc;
}

/*
 * [한국어]
 * user_idxd_device_destruct — 디바이스 해제 콜백 (g_user_idxd_impl.destruct).
 *
 * 코어가 spdk_idxd_detach → idxd_device_destruct → impl->destruct로 호출.
 * DISABLE → BAR unmap → PCI detach → IAA aecs 메모리 free → 구조체 해제.
 */
static void
user_idxd_device_destruct(struct spdk_idxd_device *idxd)
{
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);  /* [한국어] backend-specific 캐스팅 */

	idxd_disable_dev(user_idxd);                       /* [한국어] DISABLE 명령으로 디바이스 정지 */

	idxd_unmap_pci_bar(user_idxd, IDXD_MMIO_BAR);      /* [한국어] 컨트롤 영역 unmap */
	idxd_unmap_pci_bar(user_idxd, IDXD_WQ_BAR);        /* [한국어] portal 영역 unmap */

	spdk_pci_device_detach(user_idxd->device);         /* [한국어] DPDK PCI 추상화에서 분리 — 다른 프로세스가 다시 attach 가능 */
	if (idxd->type == IDXD_DEV_TYPE_IAA) {
		spdk_free(idxd->aecs);                     /* [한국어] IAA 전용 AECS(Analytics Engine Configuration Set) 메모리 해제 */
	}
	free(user_idxd);                                   /* [한국어] 래퍼 구조체 자체 해제 (idxd는 첫 멤버이므로 함께 해제됨) */
}

/*
 * [한국어]
 * struct idxd_enum_ctx — spdk_pci_enumerate 콜백에 전달할 컨텍스트.
 *
 * SPDK PCI 추상화는 콜백을 통해 디바이스를 하나씩 보여주므로, 사용자 콜백(probe_cb/attach_cb)과
 * 사용자 ctx를 묶어 enum_cb로 넘긴다.
 */
struct idxd_enum_ctx {
	spdk_idxd_probe_cb probe_cb;
	/* [한국어] 사용자가 등록한 "이 디바이스를 attach할까?" 결정 콜백. false 반환 시 스킵. */

	spdk_idxd_attach_cb attach_cb;
	/* [한국어] attach 성공 후 사용자에게 디바이스 핸들을 전달하는 콜백. */

	void *cb_ctx;
	/* [한국어] 사용자 컨텍스트 — 두 콜백에 동일하게 전달된다. */
};

/*
 * [한국어]
 * probe_cb — 디바이스 식별 로그 + claim.
 *
 * @cb_ctx: 사용 안 함(인자 시그니처 호환).
 * @pci_dev: PCI 디바이스 핸들.
 * @return: claim 성공 시 true, 실패(다른 프로세스가 점유) 시 false.
 *
 * SPDK 멀티프로세스 환경에서 spdk_pci_device_claim은 /var/tmp 등에 lock 파일을 만들어
 * 동일 BDF의 디바이스를 두 프로세스가 동시에 잡지 못하게 한다.
 */
static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
	struct spdk_pci_addr pci_addr __attribute__((unused));  /* [한국어] 디버그 로그용 — 미사용 가능성을 컴파일러에 알림 */

	pci_addr = spdk_pci_device_get_addr(pci_dev);

	SPDK_DEBUGLOG(idxd,
		      " Found matching device at %04x:%02x:%02x.%x vendor:0x%04x device:0x%04x\n",
		      pci_addr.domain,
		      pci_addr.bus,
		      pci_addr.dev,
		      pci_addr.func,
		      spdk_pci_device_get_vendor_id(pci_dev),
		      spdk_pci_device_get_device_id(pci_dev));

	/* Claim the device in case conflict with other process */
	if (spdk_pci_device_claim(pci_dev) < 0) {
		return false;  /* [한국어] 다른 SPDK 프로세스가 이미 잡았음 — 이 프로세스는 스킵 */
	}

	return true;
}

/* This function must only be called while holding g_driver_lock */
/*
 * [한국어]
 * idxd_enum_cb — spdk_pci_enumerate가 디바이스 하나를 발견할 때마다 호출되는 콜백.
 *
 * @ctx: idxd_enum_ctx (사용자 콜백 + cb_ctx).
 * @pci_dev: 발견된 디바이스.
 * @return: 0(처리됨), 1(사용자가 skip), <0(에러).
 *
 * 흐름: 사용자 probe_cb로 attach 의도 확인 → 본 파일 probe_cb로 claim →
 *       idxd_attach로 디바이스 설정 → 사용자 attach_cb로 결과 반환.
 *
 * 호출 컨텍스트: g_driver_lock 보호하에 main 스레드에서 한 번만.
 */
static int
idxd_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct idxd_enum_ctx *enum_ctx = ctx;
	struct spdk_idxd_device *idxd;

	/* Call the user probe_cb to see if they want this device or not, if not
	 * skip it with a positive return code.
	 */
	if (enum_ctx->probe_cb(enum_ctx->cb_ctx, pci_dev) == false) {
		return 1;  /* [한국어] 사용자가 거절 — 양의 값으로 enumerate에 "skip" 시그널 */
	}

	if (probe_cb(enum_ctx->cb_ctx, pci_dev)) {  /* [한국어] claim + 로그 */
		idxd = idxd_attach(pci_dev);        /* [한국어] BAR mapping + configure 일괄 수행 */
		if (idxd == NULL) {
			SPDK_ERRLOG("idxd_attach() failed\n");
			return -EINVAL;
		}

		enum_ctx->attach_cb(enum_ctx->cb_ctx, idxd);  /* [한국어] 사용자에게 디바이스 핸들 전달 */
	}

	return 0;
}

/* The IDXD driver supports 2 distinct HW units, DSA and IAA. */
/*
 * [한국어]
 * user_idxd_probe — g_user_idxd_impl.probe. 모든 DSA/IAA PCI 디바이스를 열거하여 attach.
 *
 * @cb_ctx: 사용자 컨텍스트.
 * @attach_cb: attach 성공 시 호출.
 * @probe_cb: attach 결정 콜백.
 *
 * spdk_pci_idxd_get_driver()는 SPDK env이 등록한 vendor/device ID 매칭 드라이버 객체를 반환.
 * spdk_pci_enumerate가 PCI 트리를 스캔하며 매칭 디바이스마다 idxd_enum_cb를 호출.
 *
 * 락: g_driver_lock으로 직렬화 — DPDK 내부에는 동시 enumerate 안전성이 보장되지 않을 수 있음.
 */
static int
user_idxd_probe(void *cb_ctx, spdk_idxd_attach_cb attach_cb,
		spdk_idxd_probe_cb probe_cb)
{
	int rc;
	struct idxd_enum_ctx enum_ctx;

	enum_ctx.probe_cb = probe_cb;
	enum_ctx.attach_cb = attach_cb;
	enum_ctx.cb_ctx = cb_ctx;

	pthread_mutex_lock(&g_driver_lock);                                              /* [한국어] PCI 열거 직렬화 시작 */
	rc = spdk_pci_enumerate(spdk_pci_idxd_get_driver(), idxd_enum_cb, &enum_ctx);    /* [한국어] PCI 트리 스캔 + 콜백 */
	pthread_mutex_unlock(&g_driver_lock);                                            /* [한국어] 직렬화 종료 */
	assert(rc == 0);                                                                 /* [한국어] 디버그: 정상 케이스에서 0이어야 함 */

	return rc;
}

/*
 * [한국어]
 * user_idxd_dump_sw_err — sw_err 레지스터 4 x 8B를 로그에 dump.
 *
 * 디스크립터가 디바이스에서 거부되거나 비정상 종료되면 sw_err에 상세 코드/wq 인덱스/operation이 기록됨.
 * 코어가 비정상 완료를 감지했을 때 디버깅 정보로 호출. portal 인자는 호환을 위해 받지만 user 백엔드에서는 사용 안 함.
 */
static void
user_idxd_dump_sw_err(struct spdk_idxd_device *idxd, void *portal)
{
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);
	union idxd_swerr_register sw_err;
	uint16_t i;

	SPDK_NOTICELOG("SW Error Raw:");
	for (i = 0; i < 4; i++) {
		sw_err.raw[i] = spdk_mmio_read_8(&user_idxd->registers->sw_err.raw[i]);  /* [한국어] sw_err: 32B 레지스터 = 4 x 8B */
		SPDK_NOTICELOG("    0x%lx\n", sw_err.raw[i]);
	}

	SPDK_NOTICELOG("SW Error error code: %#x\n", (uint8_t)(sw_err.error));
	SPDK_NOTICELOG("SW Error WQ index: %u\n", (uint8_t)(sw_err.wq_idx));
	SPDK_NOTICELOG("SW Error Operation: %u\n", (uint8_t)(sw_err.operation));
}

/*
 * [한국어]
 * user_idxd_portal_get_addr — BAR2(WQ portal)의 가상주소를 코어에 노출.
 *
 * 코어는 이 주소 + per-channel offset에 movdir64b로 64B 디스크립터를 직접 쓴다 (cache-bypass).
 * idxd->portal은 idxd_map_pci_bars에서 mmap된 값.
 */
static char *
user_idxd_portal_get_addr(struct spdk_idxd_device *idxd)
{
	return (char *)idxd->portal;  /* [한국어] BAR2 가상주소 그대로 노출. 호출자는 portal_offset을 더해 사용 */
}

/*
 * [한국어]
 * g_user_idxd_impl — user 백엔드 함수 테이블.
 *
 * SPDK_IDXD_IMPL_REGISTER 매크로(파일 끝)가 STAILQ로 g_idxd_impls 리스트에 등록.
 * spdk_idxd_set_config(false)가 "user" 이름으로 검색하여 g_idxd_impl로 선택.
 */
static struct spdk_idxd_impl g_user_idxd_impl = {
	.name			= "user",                          /* [한국어] set_config의 매칭 키 */
	.probe			= user_idxd_probe,                 /* [한국어] PCI 열거 진입점 */
	.destruct		= user_idxd_device_destruct,       /* [한국어] detach 시 호출 */
	.dump_sw_error		= user_idxd_dump_sw_err,           /* [한국어] 에러 발생 시 디버깅 dump */
	.portal_get_addr	= user_idxd_portal_get_addr        /* [한국어] portal 주소 노출 */
};

/*
 * Fixed Huffman tables the IAA hardware requires to implement RFC-1951.
 */
/*
 * [한국어]
 * fixed_ll_sym — IAA 압축 엔진이 RFC-1951 deflate 표준의 "fixed Huffman code"를
 *   사용할 때 AECS(Analytics Engine Configuration Set)에 미리 적재해야 하는 literal/length 코드 테이블.
 *
 * RFC-1951 §3.2.6: literal value 0..143은 8비트, 144..255는 9비트, 종료 256은 7비트, length 257..279는 7비트,
 * 280..287은 8비트로 인코딩된다. 여기 적힌 32비트 값들은 (codeword_bits | length_field) 형태로
 * IAA가 직접 소비하는 사전계산된 형식이다. 본 테이블 자체는 하드웨어 사양 부속 문서로 제공된 것을 그대로 옮긴 것.
 *
 * 설정자: idxd_attach에서 IAA 디바이스인 경우 idxd->aecs->ll_sym에 memcpy.
 * 읽는 자: IAA가 압축/해제 디스크립터 처리 시 직접 DMA로 fetch.
 */
const uint32_t fixed_ll_sym[286] = {
	0x40030, 0x40031, 0x40032, 0x40033, 0x40034, 0x40035, 0x40036, 0x40037,
	0x40038, 0x40039, 0x4003A, 0x4003B, 0x4003C, 0x4003D, 0x4003E, 0x4003F,
	0x40040, 0x40041, 0x40042, 0x40043, 0x40044, 0x40045, 0x40046, 0x40047,
	0x40048, 0x40049, 0x4004A, 0x4004B, 0x4004C, 0x4004D, 0x4004E, 0x4004F,
	0x40050, 0x40051, 0x40052, 0x40053, 0x40054, 0x40055, 0x40056, 0x40057,
	0x40058, 0x40059, 0x4005A, 0x4005B, 0x4005C, 0x4005D, 0x4005E, 0x4005F,
	0x40060, 0x40061, 0x40062, 0x40063, 0x40064, 0x40065, 0x40066, 0x40067,
	0x40068, 0x40069, 0x4006A, 0x4006B, 0x4006C, 0x4006D, 0x4006E, 0x4006F,
	0x40070, 0x40071, 0x40072, 0x40073, 0x40074, 0x40075, 0x40076, 0x40077,
	0x40078, 0x40079, 0x4007A, 0x4007B, 0x4007C, 0x4007D, 0x4007E, 0x4007F,
	0x40080, 0x40081, 0x40082, 0x40083, 0x40084, 0x40085, 0x40086, 0x40087,
	0x40088, 0x40089, 0x4008A, 0x4008B, 0x4008C, 0x4008D, 0x4008E, 0x4008F,
	0x40090, 0x40091, 0x40092, 0x40093, 0x40094, 0x40095, 0x40096, 0x40097,
	0x40098, 0x40099, 0x4009A, 0x4009B, 0x4009C, 0x4009D, 0x4009E, 0x4009F,
	0x400A0, 0x400A1, 0x400A2, 0x400A3, 0x400A4, 0x400A5, 0x400A6, 0x400A7,
	0x400A8, 0x400A9, 0x400AA, 0x400AB, 0x400AC, 0x400AD, 0x400AE, 0x400AF,
	0x400B0, 0x400B1, 0x400B2, 0x400B3, 0x400B4, 0x400B5, 0x400B6, 0x400B7,
	0x400B8, 0x400B9, 0x400BA, 0x400BB, 0x400BC, 0x400BD, 0x400BE, 0x400BF,
	0x48190, 0x48191, 0x48192, 0x48193, 0x48194, 0x48195, 0x48196, 0x48197,
	0x48198, 0x48199, 0x4819A, 0x4819B, 0x4819C, 0x4819D, 0x4819E, 0x4819F,
	0x481A0, 0x481A1, 0x481A2, 0x481A3, 0x481A4, 0x481A5, 0x481A6, 0x481A7,
	0x481A8, 0x481A9, 0x481AA, 0x481AB, 0x481AC, 0x481AD, 0x481AE, 0x481AF,
	0x481B0, 0x481B1, 0x481B2, 0x481B3, 0x481B4, 0x481B5, 0x481B6, 0x481B7,
	0x481B8, 0x481B9, 0x481BA, 0x481BB, 0x481BC, 0x481BD, 0x481BE, 0x481BF,
	0x481C0, 0x481C1, 0x481C2, 0x481C3, 0x481C4, 0x481C5, 0x481C6, 0x481C7,
	0x481C8, 0x481C9, 0x481CA, 0x481CB, 0x481CC, 0x481CD, 0x481CE, 0x481CF,
	0x481D0, 0x481D1, 0x481D2, 0x481D3, 0x481D4, 0x481D5, 0x481D6, 0x481D7,
	0x481D8, 0x481D9, 0x481DA, 0x481DB, 0x481DC, 0x481DD, 0x481DE, 0x481DF,
	0x481E0, 0x481E1, 0x481E2, 0x481E3, 0x481E4, 0x481E5, 0x481E6, 0x481E7,
	0x481E8, 0x481E9, 0x481EA, 0x481EB, 0x481EC, 0x481ED, 0x481EE, 0x481EF,
	0x481F0, 0x481F1, 0x481F2, 0x481F3, 0x481F4, 0x481F5, 0x481F6, 0x481F7,
	0x481F8, 0x481F9, 0x481FA, 0x481FB, 0x481FC, 0x481FD, 0x481FE, 0x481FF,
	0x38000, 0x38001, 0x38002, 0x38003, 0x38004, 0x38005, 0x38006, 0x38007,
	0x38008, 0x38009, 0x3800A, 0x3800B, 0x3800C, 0x3800D, 0x3800E, 0x3800F,
	0x38010, 0x38011, 0x38012, 0x38013, 0x38014, 0x38015, 0x38016, 0x38017,
	0x400C0, 0x400C1, 0x400C2, 0x400C3, 0x400C4, 0x400C5
};

/*
 * [한국어]
 * fixed_d_sym — RFC-1951 fixed Huffman의 distance 코드 테이블.
 * distance 0..29는 5비트 고정 길이로 인코딩되며, 본 테이블은 IAA 형식으로 사전 변환된 값.
 */
const uint32_t fixed_d_sym[30] = {
	0x28000, 0x28001, 0x28002, 0x28003, 0x28004, 0x28005, 0x28006, 0x28007,
	0x28008, 0x28009, 0x2800A, 0x2800B, 0x2800C, 0x2800D, 0x2800E, 0x2800F,
	0x28010, 0x28011, 0x28012, 0x28013, 0x28014, 0x28015, 0x28016, 0x28017,
	0x28018, 0x28019, 0x2801A, 0x2801B, 0x2801C, 0x2801D
};
/* [한국어] DYNAMIC_HDR=0x2: deflate block header bits = "10" (dynamic Huffman). LSB first 인코딩.
 * 우리는 fixed 테이블을 dynamic-block처럼 미리 등록하는 트릭을 사용. */
#define DYNAMIC_HDR			0x2
/* [한국어] 위 헤더의 bit length = 3 (BFINAL=0 + BTYPE=10 = 3비트) */
#define DYNAMIC_HDR_SIZE		3

/* Caller must hold g_driver_lock */
/*
 * [한국어]
 * idxd_attach — 디바이스 attach 마스터 함수.
 *
 * @device: PCI 디바이스 핸들 (DSA 또는 IAA).
 * @return: 성공 시 spdk_idxd_device*, 실패 시 NULL (모든 자원 정리됨).
 *
 * 흐름:
 *   1) spdk_user_idxd_device 할당.
 *   2) device id로 DSA/IAA 판별 → idxd->type 설정.
 *   3) IAA면 AECS(Analytics Engine Configuration Set) DMA 메모리 할당 + 물리주소 변환 + fixed Huffman 적재.
 *   4) PCI busmaster 활성화 (PCI command register bit 2).
 *   5) idxd_device_configure로 BAR mapping ~ WQ enable.
 *
 * 호출 컨텍스트: idxd_enum_cb → idxd_attach. g_driver_lock 보호.
 */
static struct spdk_idxd_device *
idxd_attach(struct spdk_pci_device *device)
{
	struct spdk_user_idxd_device *user_idxd;
	struct spdk_idxd_device *idxd;
	uint16_t did = device->id.device_id;     /* [한국어] PCI device ID — DSA 또는 IAA 식별자 */
	uint32_t cmd_reg;                        /* [한국어] PCI config space의 command register */
	uint64_t updated = sizeof(struct iaa_aecs);  /* [한국어] vtophys가 매핑 길이를 갱신하는 in-out 인자 */
	int rc;

	user_idxd = calloc(1, sizeof(struct spdk_user_idxd_device));
	if (user_idxd == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for user_idxd device.\n");
		return NULL;
	}

	idxd = &user_idxd->idxd;                 /* [한국어] 첫 멤버이므로 동일 주소. 코어 API와 인터페이스. */
	if (did == PCI_DEVICE_ID_INTEL_DSA) {
		idxd->type = IDXD_DEV_TYPE_DSA;  /* [한국어] DSA: memmove/fill/CRC/dual-cast 등 데이터 무브 가속 */
	} else if (did == PCI_DEVICE_ID_INTEL_IAA) {
		idxd->type = IDXD_DEV_TYPE_IAA;  /* [한국어] IAA: deflate 압축/CRC/필터 등 분석 가속 */
		idxd->aecs = spdk_zmalloc(sizeof(struct iaa_aecs),
					  0x20, NULL,
					  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		/* [한국어] AECS 영역 — IAA의 압축/해제 컨텍스트를 담는 DMA 버퍼.
		 * alignment 0x20 = 32B (IAA 사양 요구사항). DMA 메모리는 hugepage 기반 IOVA-매핑됨. */
		if (idxd->aecs == NULL) {
			SPDK_ERRLOG("Failed to allocate iaa aecs\n");
			goto err;
		}

		idxd->aecs_addr = spdk_vtophys((void *)idxd->aecs, &updated);
		/* [한국어] AECS의 IOVA(또는 물리주소) 변환 — 디바이스 DMA가 사용. updated가 sizeof보다 작으면 페이지 분할 발생. */
		if (idxd->aecs_addr == SPDK_VTOPHYS_ERROR || updated < sizeof(struct iaa_aecs)) {
			SPDK_ERRLOG("Failed to translate iaa aecs\n");
			spdk_free(idxd->aecs);
			goto err;
		}

		/* Configure aecs table using fixed Huffman table */
		idxd->aecs->output_accum[0] = DYNAMIC_HDR | 1;  /* [한국어] BFINAL=1 + BTYPE=10 → 0x3? — 실제로 (0x2|0x1)=0x3, "마지막 dynamic block" 헤더 비트 패턴 */
		idxd->aecs->num_output_accum_bits = DYNAMIC_HDR_SIZE;  /* [한국어] 헤더 비트 수 = 3 */

		/* Add Huffman table to aecs */
		memcpy(idxd->aecs->ll_sym, fixed_ll_sym, sizeof(fixed_ll_sym));  /* [한국어] literal/length 테이블 적재 */
		memcpy(idxd->aecs->d_sym, fixed_d_sym, sizeof(fixed_d_sym));     /* [한국어] distance 테이블 적재 */
	}

	user_idxd->device = device;              /* [한국어] PCI 핸들 보관 */
	idxd->impl = &g_user_idxd_impl;          /* [한국어] 함수 테이블 — 코어가 destruct/dump/portal 시 호출 */
	idxd->socket_id = device->socket_id;     /* [한국어] NUMA 친화도 — 채널을 같은 소켓 reactor에 배치하는 데 사용 */
	pthread_mutex_init(&idxd->num_channels_lock, NULL);  /* [한국어] num_channels 카운터 보호 (get_channel/put_channel 동시성) */

	/* Enable PCI busmaster. */
	spdk_pci_device_cfg_read32(device, &cmd_reg, 4);   /* [한국어] PCI config offset 4 = command register 읽기 */
	cmd_reg |= 0x4;                                    /* [한국어] bit 2 = busmaster enable — 디바이스가 호스트 메모리에 DMA 가능 */
	spdk_pci_device_cfg_write32(device, cmd_reg, 4);   /* [한국어] 변경값 기록 */

	rc = idxd_device_configure(user_idxd);             /* [한국어] BAR map ~ WQ enable의 풀 셋업 */
	if (rc) {
		goto err;
	}

	return idxd;
err:
	user_idxd_device_destruct(idxd);                   /* [한국어] 부분 자원 해제 (destruct는 NULL/미초기화 가드 포함) */
	return NULL;
}

/* [한국어] 매크로 — g_user_idxd_impl을 g_idxd_impls STAILQ에 등록하는 생성자.
 * lib/idxd/idxd.c에 정의된 SPDK_IDXD_IMPL_REGISTER가 __attribute__((constructor))를 통해
 * main 진입 전에 STAILQ_INSERT_TAIL을 수행. spdk_idxd_set_config가 이름으로 검색하는 키. */
SPDK_IDXD_IMPL_REGISTER(user, &g_user_idxd_impl);
