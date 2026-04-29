/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 */

/* VFIO transport extensions for spdk_nvme_ctrlr */

/*
 * [한국어 설명] vfio-user NVMe 트랜스포트 (nvme_vfio_user.c) — 361 라인
 *
 * === 파일의 역할 ===
 * **vfio-user** 프로토콜 위에 NVMe 컨트롤러를 통신 상대로 두는 SPDK 트랜스포트
 * 구현. vfio-user는 VFIO ioctl 표면을 Unix domain socket 메시지로 외부화한
 * 프로토콜로, 호스트의 SPDK NVMe-oF target(또는 다른 emulator)이 자기 자신을
 * "PCIe 디바이스"처럼 다른 프로세스/VM에 노출할 수 있게 한다. 즉 "물리 PCIe
 * BAR/Config 공간 액세스" 대신 "소켓 RPC로 BAR0/Config을 read/write"한다.
 *
 * 이 파일은 결과적으로 **PCIe 트랜스포트(nvme_pcie.c)의 얇은 변형**이다:
 *   - 컨트롤러 발견(scan): 실제 PCI 열거가 아니라 socket path(traddr) 검사
 *   - 컨트롤러 register read/write: MMIO 대신 vfio-user `BAR_ACCESS` 메시지
 *   - BAR0 매핑: spdk_vfio_user_get_bar_addr (mmap 또는 메시지 기반)
 *   - PCI Config: vfio-user CONFIG region access (busmaster enable 등)
 *   - **qpair / poll group 경로는 PCIe와 100% 공유** — 트랜스포트 vtable이
 *     qpair_submit_request 등은 nvme_pcie_* 함수를 그대로 등록한다.
 *
 * 따라서 doorbell, SQE/CQE 처리, completion polling 등 핫패스는 nvme_pcie.c가
 * 담당하고, 이 파일은 **느린 control plane만** 새로 구현한다.
 *
 * SPDK_CONFIG_VFIO_USER 매크로가 켜져 있을 때만 빌드되며, libvfio-user 의존.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe 드라이버의 트랜스포트 레이어 한 종류. 트랜스포트 vtable
 * (struct spdk_nvme_transport_ops)을 SPDK_NVME_TRANSPORT_REGISTER 매크로로
 * 등록 → spdk_nvme_connect/probe가 trtype=VFIOUSER일 때 이 vtable을 디스패치.
 *
 * 호출 체인 (사용자 → 디바이스):
 *   spdk_nvme_probe(trid={trtype=VFIOUSER, traddr=/path/to/sock_dir}, ...)
 *     → nvme_transport_ctrlr_scan → nvme_vfio_ctrlr_scan [이 파일]
 *       → nvme_ctrlr_probe → nvme_transport_ctrlr_construct
 *         → nvme_vfio_ctrlr_construct [이 파일]
 *           → spdk_vfio_user_setup(socket path) → libvfio-user client 셋업
 *           → nvme_vfio_setup_bar0 → BAR0(doorbell 영역) 매핑
 *           → PCI CMD_REG에 0x404 OR (Bus Master Enable + Interrupt Disable)
 *           → nvme_pcie_ctrlr_construct_admin_qpair (PCIe 코드 재사용)
 *
 * 이후 I/O 핫패스:
 *   spdk_nvme_ctrlr_alloc_io_qpair → nvme_pcie_ctrlr_create_io_qpair
 *   spdk_nvme_ns_cmd_*           → nvme_pcie_qpair_submit_request
 *   spdk_nvme_qpair_process_completions → nvme_pcie_qpair_process_completions
 *
 * 실행 컨텍스트: control plane(이 파일의 함수들)은 사용자 스레드 또는
 *               probe/connect 스레드. data plane은 일반 NVMe와 동일하게
 *               qpair 소유 SPDK thread.
 *
 * === 타 모듈과의 연결 ===
 * 의존(callee):
 *   - spdk/vfio_user_pci.h — spdk_vfio_user_setup/release/get_bar_addr/
 *                            pci_bar_access (libvfio-user 추상화 계층)
 *   - linux/vfio.h — VFIO_PCI_BAR0_REGION_INDEX, VFIO_PCI_CONFIG_REGION_INDEX
 *                    같은 region index 상수 (kernel UAPI 헤더)
 *   - nvme_internal.h — spdk_nvme_ctrlr, nvme_ctrlr_construct/destruct,
 *                       trid/opts, 트랜스포트 vtable 매크로
 *   - nvme_pcie_internal.h — nvme_pcie_ctrlr/qpair, doorbell_base/stride,
 *                             nvme_pcie_qpair_submit_request 등 PCIe 코드 진입점
 * 의존받음(caller):
 *   - lib/nvme/nvme_transport.c — vtable 디스패처가 trtype=VFIOUSER일 때 사용
 *   - lib/nvme/nvme_ctrlr.c — 컨트롤러 라이프사이클이 vtable을 호출
 * 데이터 흐름:
 *   사용자 traddr(socket path) → vfio_user_setup → libvfio-user client →
 *   대상 프로세스(예: NVMe-oF vfio-user target)의 emulated NVMe 컨트롤러.
 *   register R/W는 PCI BAR0 messages, doorbell은 mmap된 BAR0 영역 직접 쓰기.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct nvme_vfio_ctrlr: nvme_pcie_ctrlr를 inheritance(첫 멤버)하면서
 *     vfio_user 디바이스 핸들(dev)과 BAR0 doorbell 베이스를 추가.
 *   - nvme_vfio_ctrlr(): spdk_nvme_ctrlr → nvme_vfio_ctrlr 다운캐스트 (CONTAINEROF).
 *   - nvme_vfio_ctrlr_get/set_reg_4/8(): BAR0 범위의 NVMe 컨트롤러 레지스터를
 *     vfio-user BAR_ACCESS 메시지로 read/write.
 *   - nvme_vfio_ctrlr_set_asq/acq/aqa(): admin SQ/CQ 베이스 주소와 크기 설정
 *     (NVMe spec ASQ/ACQ/AQA 레지스터 — 컨트롤러 enable 직전 필수).
 *   - nvme_vfio_setup_bar0(): BAR0의 doorbell 페이지 mmap.
 *   - nvme_vfio_ctrlr_construct(): 메인 진입점. socket setup → BAR0 매핑 →
 *     PCI busmaster enable → admin qpair 생성.
 *   - nvme_vfio_ctrlr_scan(): probe 시 traddr 존재 검증 후 ctrlr_probe 호출.
 *   - nvme_vfio_ctrlr_enable(): admin qpair의 SQ/CQ DMA 주소를 ASQ/ACQ에 기록.
 *   - vfio_ops: 트랜스포트 vtable. vfio 고유 함수 + nvme_pcie_* 재사용 함수 혼합.
 *   - SPDK_NVME_TRANSPORT_REGISTER(vfio, &vfio_ops): 모듈 init 시 트랜스포트 등록.
 */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 헤더 모음 (stdint, stdio, errno, string, unistd 등). */
#include "spdk/env.h"
/* [한국어] DPDK 환경 추상화 (메모리/PCI). 직접 사용은 적지만 trid/공통 타입 의존. */
#include "spdk/likely.h"
/* [한국어] spdk_likely/unlikely 분기 힌트 매크로. */
#include "spdk/string.h"
/* [한국어] spdk_strerror 등 문자열 헬퍼. */
#include "spdk/vfio_user_pci.h"
/* [한국어] vfio-user 클라이언트 추상화: setup/release, BAR mmap, BAR access. */
#include "nvme_internal.h"
/* [한국어] SPDK NVMe 드라이버 내부 헤더 — spdk_nvme_ctrlr, trid/opts,
 *         트랜스포트 vtable 매크로(SPDK_NVME_TRANSPORT_REGISTER), 로깅 매크로. */
#include "nvme_pcie_internal.h"
/* [한국어] PCIe 트랜스포트 내부 헤더 — nvme_pcie_ctrlr/qpair, doorbell stride,
 *         그리고 vtable에서 재사용할 nvme_pcie_qpair_* / poll_group_* 프로토타입. */

#include <linux/vfio.h>
/* [한국어] Linux UAPI: VFIO_PCI_BAR0_REGION_INDEX(0)/CONFIG_REGION_INDEX(7) 등의
 *         region index 상수. vfio-user 프로토콜은 VFIO와 같은 region 모델 사용. */

#define NVME_MAX_XFER_SIZE		(131072)
/* [한국어] 단일 NVMe I/O의 최대 전송 크기 = 128 KiB. vfio-user 백엔드의 메시지/
 *         DMA 버퍼 한계와 가장 보수적인 스펙 한도를 고려한 고정값.
 *         get_max_xfer_size vtable 콜백이 이 값을 그대로 반환 → 사용자 split 기준. */
#define NVME_MAX_SGES			(1)
/* [한국어] vfio-user 트랜스포트는 SGL을 사용하지 않으므로 SGE 1개로 제한.
 *         실질적으로 PRP 기반 단일 연속 버퍼만 지원. */

/*
 * [한국어]
 * struct nvme_vfio_ctrlr - vfio-user 트랜스포트 전용 컨트롤러 컨텍스트
 *
 * "객체 inheritance" 패턴: 첫 멤버를 nvme_pcie_ctrlr로 두어 PCIe 함수에
 * (struct nvme_pcie_ctrlr *)로 그대로 넘길 수 있게 한다 — qpair/doorbell
 * 핫패스를 PCIe 코드로 100% 재사용하기 위함.
 *
 * 라이프사이클:
 *   nvme_vfio_ctrlr_construct(): calloc + spdk_vfio_user_setup으로 dev 획득
 *   nvme_vfio_ctrlr_destruct():  spdk_vfio_user_release(dev) + free
 */
struct nvme_vfio_ctrlr {
	struct nvme_pcie_ctrlr pctrlr;
	/* [한국어] 부모 PCIe 컨트롤러 컨텍스트(첫 멤버 — 부모 타입으로 캐스팅 가능).
	 *         pctrlr.ctrlr가 일반 spdk_nvme_ctrlr이며, doorbell_base/regs 등
	 *         PCIe 핫패스가 사용하는 필드를 모두 포함. 설정자: construct에서.
	 *         읽는 자: 모든 nvme_pcie_qpair_* 함수가 ctrlr→nvme_pcie_ctrlr 캐스팅으로 접근.
	 *         동기화: 컨트롤러 라이프사이클 동안 거의 불변, qpair별 doorbell 갱신은
	 *         qpair 소유 스레드만 수행하므로 락 불필요. */

	volatile uint32_t *doorbell_base;
	/* [한국어] BAR0 내부의 doorbell 영역(첫 SQ tail doorbell DW부터)을 mmap한
	 *         가상 주소. volatile은 컴파일러가 doorbell write를 제거/재배치하지
	 *         못하게 하기 위함. 설정자: nvme_vfio_setup_bar0()에서 1회 설정.
	 *         읽는 자: pctrlr.doorbell_base에 그대로 복사되어 PCIe 코드에서 사용.
	 *         값 범위: 유효한 mmap된 페이지 주소(NULL 불가, construct 성공 시).
	 *         동기화: 각 qpair는 자기 doorbell DW만 쓰므로 자연스럽게 분리. */

	struct vfio_device *dev;
	/* [한국어] vfio-user 클라이언트 핸들(libvfio-user 추상화). socket 연결,
	 *         BAR/Config region 메타데이터, mmap 영역 추적자.
	 *         설정자: spdk_vfio_user_setup() 반환값을 construct에서 저장.
	 *         읽는 자: 모든 vfio_user_pci_bar_access / get_bar_addr / release 호출.
	 *         값 범위: 유효한 디바이스 포인터(NULL이면 setup 실패).
	 *         동기화: libvfio-user 내부에서 처리 — 동일 디바이스에 대한 동시
	 *         BAR access는 일반적으로 control plane에서만 발생하므로 충돌 거의 없음. */
};

/*
 * [한국어]
 * nvme_vfio_ctrlr - 일반 spdk_nvme_ctrlr → nvme_vfio_ctrlr 다운캐스트
 *
 * @ctrlr: spdk_nvme_ctrlr 베이스 (실제로는 nvme_vfio_ctrlr.pctrlr.ctrlr).
 * @return: 감싸고 있는 nvme_vfio_ctrlr 포인터.
 *
 * 두 단계 캐스팅: ctrlr → nvme_pcie_ctrlr(헬퍼 사용) → nvme_vfio_ctrlr(CONTAINEROF).
 * 모든 vfio 전용 함수가 이 헬퍼로 자기 컨텍스트를 회복한다.
 *
 * 호출 체인: vfio 전용 함수 진입부 → nvme_vfio_ctrlr → struct 멤버 접근.
 */
static inline struct nvme_vfio_ctrlr *
nvme_vfio_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	/* [한국어] PCIe 헬퍼로 먼저 부모 컨텍스트 회복. */

	return SPDK_CONTAINEROF(pctrlr, struct nvme_vfio_ctrlr, pctrlr);
	/* [한국어] CONTAINEROF: pctrlr 주소에서 멤버 오프셋만큼 빼서 둘러싸는
	 *         nvme_vfio_ctrlr 주소를 계산. C 표준 트릭(offsetof 기반). */
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_get_registers - NVMe 컨트롤러 레지스터 영역 포인터 반환
 *
 * @ctrlr: 대상 컨트롤러
 * @return: BAR0를 mmap한 가상 주소 (struct spdk_nvme_registers 형태로 캐스팅)
 *
 * BAR0는 컨트롤러 레지스터(CAP/VS/CC/CSTS/AQA/ASQ/ACQ + doorbell)를 담는
 * 단일 영역. PCIe 트랜스포트와 동일하게 사용자 코드가 직접 읽는 경로(예:
 * spdk_nvme_ctrlr_get_regs_*)에서 호출된다. 단, vfio-user의 BAR0가 mmap
 * 가능한 경우에만 의미가 있고, 메시지 기반 접근만 가능하면 NULL일 수 있음
 * (이때 사용자는 set/get_reg_4/8 경로를 사용해야 함).
 */
static volatile struct spdk_nvme_registers *
nvme_vfio_ctrlr_get_registers(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);
	/* [한국어] vfio 컨텍스트 회복. */

	return vctrlr->pctrlr.regs;
	/* [한국어] PCIe 코드와 공유: pctrlr.regs는 BAR0의 가상 주소(mmap된 경우). */
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_set_reg_4 - BAR0 안의 32-bit 컨트롤러 레지스터에 쓰기
 *
 * @ctrlr:  대상 컨트롤러
 * @offset: BAR0 내부 오프셋 (0..sizeof(spdk_nvme_registers)-4)
 * @value:  기록할 32-bit 값 (호스트 엔디언, 트랜스포트가 변환 처리)
 * @return: 0 성공, 음수 실패 (vfio-user 메시지 실패)
 *
 * PCIe MMIO 대신 vfio-user `BAR_ACCESS` 메시지로 BAR0를 write한다. 컨트롤
 * 평면(CC.EN=1, IOSQES/IOCQES 설정 등)에서만 호출되는 저빈도 경로.
 *
 * 호출 체인:
 *   nvme_ctrlr_set_cc / nvme_ctrlr_set_aqa 등 → vtable.ctrlr_set_reg_4
 *     → 이 함수 → spdk_vfio_user_pci_bar_access(write=true) → libvfio-user
 *     → 상대 프로세스의 BAR0 핸들러
 */
static int
nvme_vfio_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);
	/* [한국어] vfio 컨텍스트 회복. */

	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	/* [한국어] 4바이트 access이므로 마지막 유효 오프셋은 size-4. 디버그 가드. */
	NVME_CTRLR_LOG2(DEBUG, nvme_vfio, ctrlr, "offset 0x%x, value 0x%x\n", offset, value);
	/* [한국어] 디버그 로그 — nvme_vfio 컴포넌트로 분류되어 SPDK 로그 필터로 제어 가능. */

	return spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_BAR0_REGION_INDEX,
					     offset, 4, &value, true);
	/* [한국어] vfio-user BAR0 region에 4바이트 write. 마지막 인자 true=write.
	 *         &value는 송신 버퍼 — 동기 호출이므로 value의 스택 수명 안전. */
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_set_reg_8 - BAR0 안의 64-bit 컨트롤러 레지스터에 쓰기
 *
 * @ctrlr/@offset/@value/@return: set_reg_4와 동일하나 8바이트.
 *
 * NVMe spec의 64-bit 레지스터(ASQ/ACQ 등) 설정에 사용. vfio-user 백엔드가
 * 64-bit access를 지원해야 한다(libvfio-user의 region descriptor에 명시).
 */
static int
nvme_vfio_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);
	/* [한국어] vfio 컨텍스트 회복. */

	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	/* [한국어] 8바이트 access ⇒ 마지막 유효 오프셋은 size-8. */
	NVME_CTRLR_LOG2(DEBUG, nvme_vfio, ctrlr, "offset 0x%x, value 0x%"PRIx64"\n", offset, value);
	/* [한국어] 디버그 로그 (PRIx64로 64-bit 16진 출력). */

	return spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_BAR0_REGION_INDEX,
					     offset, 8, &value, true);
	/* [한국어] BAR0에 8바이트 write. */
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_get_reg_4 - BAR0 안의 32-bit 컨트롤러 레지스터 읽기
 *
 * @ctrlr/@offset: 위와 동일
 * @value: 결과를 받을 출력 포인터
 * @return: 0 성공, 음수 실패 (실패 시 *value는 변경되지 않음을 가정)
 *
 * CSTS.RDY 폴링, CAP/VS 식별 등에 호출. 동기 호출이라 비싸지만 control plane
 * 한정.
 */
static int
nvme_vfio_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);
	/* [한국어] vfio 컨텍스트 회복. */
	int ret;
	/* [한국어] vfio bar_access의 반환값 보관. */

	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	/* [한국어] 4바이트 access 경계 가드. */

	ret = spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_BAR0_REGION_INDEX,
					    offset, 4, value, false);
	/* [한국어] BAR0 4바이트 read (write=false). 결과는 *value에 채워짐. */
	if (ret != 0) {
		/* [한국어] vfio-user 메시지 실패 — socket 단절 또는 region 오류. */
		NVME_CTRLR_ERRLOG(ctrlr, "offset %x\n", offset);
		/* [한국어] 어느 오프셋에서 실패했는지 에러 로그. */
		return ret;
	}

	NVME_CTRLR_LOG2(DEBUG, nvme_vfio, ctrlr, "offset 0x%x, value 0x%x\n", offset, *value);
	/* [한국어] 성공 디버그 로그 — 읽은 값을 기록. */

	return 0;
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_get_reg_8 - BAR0 안의 64-bit 컨트롤러 레지스터 읽기
 *
 * get_reg_4의 8바이트 버전. CAP 같은 64-bit 레지스터 검사에 사용.
 */
static int
nvme_vfio_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);
	/* [한국어] vfio 컨텍스트 회복. */
	int ret;
	/* [한국어] vfio bar_access 반환값. */

	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	/* [한국어] 8바이트 access 경계 가드. */

	ret = spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_BAR0_REGION_INDEX,
					    offset, 8, value, false);
	/* [한국어] BAR0 8바이트 read. */
	if (ret != 0) {
		/* [한국어] 실패 분기 — 오프셋 로깅 후 반환. */
		NVME_CTRLR_ERRLOG(ctrlr, "offset %x\n", offset);
		return ret;
	}

	NVME_CTRLR_LOG2(DEBUG, nvme_vfio, ctrlr, "offset 0x%x, value 0x%"PRIx64"\n", offset, *value);
	/* [한국어] 성공 디버그 로그. */

	return 0;
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_set_asq - Admin Submission Queue Base Address 레지스터 설정
 *
 * @ctrlr: 대상 컨트롤러
 * @value: admin SQ의 호스트 물리 주소(DMA-able)
 * @return: 0 성공, 음수 실패
 *
 * NVMe spec: ASQ는 64-bit, 컨트롤러 enable(CC.EN=1) 직전에 설정해야 함.
 * BAR0의 ASQ 오프셋에 64-bit write로 채운다.
 *
 * 호출 체인: nvme_vfio_ctrlr_enable → 이 함수 → set_reg_8 → vfio bar access.
 */
static int
nvme_vfio_ctrlr_set_asq(struct spdk_nvme_ctrlr *ctrlr, uint64_t value)
{
	/* [한국어] spdk_nvme_registers 구조체에서 asq 멤버의 오프셋을 컴파일 타임에
	 *         계산하여 BAR0 절대 오프셋으로 사용. NVMe spec 4.6 §3.1.10. */
	return nvme_vfio_ctrlr_set_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, asq),
					 value);
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_set_acq - Admin Completion Queue Base Address 레지스터 설정
 *
 * ASQ와 한 쌍. admin CQ의 DMA 주소를 64-bit로 설정.
 */
static int
nvme_vfio_ctrlr_set_acq(struct spdk_nvme_ctrlr *ctrlr, uint64_t value)
{
	/* [한국어] acq 오프셋(NVMe spec 4.6 §3.1.11)에 64-bit write. */
	return nvme_vfio_ctrlr_set_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, acq),
					 value);
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_set_aqa - Admin Queue Attributes 레지스터 설정
 *
 * @ctrlr: 대상 컨트롤러
 * @aqa:   AQA 레지스터 union (asqs/acqs 비트필드, 둘 다 0-based size)
 * @return: 0 성공, 음수 실패
 *
 * AQA.ASQS = admin SQ 엔트리 수 - 1, AQA.ACQS = admin CQ 엔트리 수 - 1.
 * ASQ/ACQ 설정 후, CC.EN=1 전에 마지막으로 채워야 하는 32-bit 레지스터.
 */
static int
nvme_vfio_ctrlr_set_aqa(struct spdk_nvme_ctrlr *ctrlr, const union spdk_nvme_aqa_register *aqa)
{
	/* [한국어] aqa.raw 오프셋(NVMe spec 4.6 §3.1.9)에 32-bit write. */
	return nvme_vfio_ctrlr_set_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, aqa.raw),
					 aqa->raw);
}

/*
 * [한국어]
 * nvme_vfio_setup_bar0 - vfio-user 디바이스의 BAR0(doorbell 영역) 매핑
 *
 * @vctrlr: vfio 컨트롤러 컨텍스트
 * @return: 0 성공, -EINVAL 매핑 실패
 *
 * BAR0의 0x1000 오프셋(NVMe spec상 doorbell 영역 시작)부터 0x1000 바이트를
 * 매핑한다. doorbell은 핫패스에서 매번 vfio 메시지로 보낼 수 없으므로 mmap
 * 가능한 영역으로 노출시켜 호스트 메모리 write가 곧 doorbell write가 되도록
 * 설계됨.
 *
 * 호출 체인: nvme_vfio_ctrlr_construct → 이 함수 → spdk_vfio_user_get_bar_addr.
 */
static int
nvme_vfio_setup_bar0(struct nvme_vfio_ctrlr *vctrlr)
{
	void *doorbell;
	/* [한국어] mmap된 doorbell 영역의 시작 주소(가상). */

	doorbell = spdk_vfio_user_get_bar_addr(vctrlr->dev, 0, 0x1000, 0x1000);
	/* [한국어] BAR index 0 의 0x1000 오프셋(NVMe doorbell base) ~ +0x1000 바이트.
	 *         vfio_user 백엔드가 mmap을 지원하면 mmap된 가상 주소, 아니면 NULL.
	 *         핫패스 doorbell write가 효율적으로 동작하려면 mmap 필수. */
	if (!doorbell) {
		/* [한국어] 매핑 실패: control plane만으로는 NVMe I/O가 불가능하므로 abort. */
		return -EINVAL;
	}

	vctrlr->doorbell_base = (volatile uint32_t *)doorbell;
	/* [한국어] uint32_t* 캐스팅 — 각 doorbell DW가 32-bit. volatile로 컴파일러가
	 *         스토어를 합치거나 제거하지 못하게 한다. */
	return 0;
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_construct - vfio-user 컨트롤러 라이프사이클 시작
 *
 * @trid:     트랜스포트 식별자. trtype=VFIOUSER, traddr=대상 socket 디렉토리.
 *            "{traddr}/cntrl" 파일이 vfio-user 컨트롤 소켓의 정해진 경로.
 * @opts:     spdk_nvme_ctrlr_opts (admin_queue_size, use_cmb_sqs 등).
 * @devhandle: PCIe와 호환된 vtable 시그니처 — vfio-user에서는 미사용.
 * @return:    spdk_nvme_ctrlr 포인터 또는 NULL(실패).
 *
 * 메인 진입점. 다음 순서로 컨트롤러를 부팅한다:
 *   1) "{traddr}/cntrl" 경로 존재 확인 (vfio-user target이 아직 떠있는지)
 *   2) nvme_vfio_ctrlr 구조체 calloc
 *   3) spdk_vfio_user_setup() — Unix domain socket 연결, region 메타 협상
 *   4) BAR0 매핑(doorbell 영역) — nvme_vfio_setup_bar0
 *   5) opts 복사, admin_queue_size 최소값 보장, use_cmb_sqs 강제 off
 *      (vfio-user는 컨트롤러 메모리 버퍼 SQ 미지원)
 *   6) nvme_ctrlr_construct — 공통 컨트롤러 초기화 (락, 풀 등)
 *   7) PCI Config의 CMD 레지스터에 0x404 OR — Bus Master Enable(bit 2) +
 *      Interrupt Disable(bit 10). vfio-user는 polling이라 INTx 불필요.
 *   8) CAP 레지스터 읽어 doorbell stride 계산
 *      stride_u32 = 2^dstrd (NVMe spec: doorbell 거리 = 2^(2+dstrd) 바이트
 *      = 4 * 2^dstrd 바이트 = 2^dstrd DW)
 *   9) nvme_pcie_ctrlr_construct_admin_qpair — admin SQ/CQ 메모리 + PRP 풀
 *   10) nvme_ctrlr_add_process(0) — primary process(=현재 프로세스) 등록
 *
 * 실패 시 exit 라벨로 점프해 vfio dev release + struct free.
 *
 * 실행 컨텍스트: 사용자 스레드(probe 호출 스레드).
 *
 * 호출 체인:
 *   spdk_nvme_probe → nvme_transport_ctrlr_construct(VFIOUSER vtable)
 *     → nvme_vfio_ctrlr_construct → 위 10단계 → spdk_nvme_ctrlr 반환
 */
static struct spdk_nvme_ctrlr *
	nvme_vfio_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
			  const struct spdk_nvme_ctrlr_opts *opts,
			  void *devhandle)
{
	struct nvme_vfio_ctrlr *vctrlr;
	/* [한국어] 새로 만들 vfio 컨트롤러 컨텍스트. */
	struct nvme_pcie_ctrlr *pctrlr;
	/* [한국어] vctrlr->pctrlr로의 단축 포인터 (가독성). */
	uint16_t cmd_reg;
	/* [한국어] PCI Config Space의 CMD 레지스터(2바이트). 0x404 비트 OR 후 다시 write. */
	union spdk_nvme_cap_register cap;
	/* [한국어] CAP 레지스터(64-bit) — doorbell stride 등 컨트롤러 capability. */
	int ret;
	/* [한국어] 단계별 반환 코드 누적. */
	char ctrlr_path[PATH_MAX];
	/* [한국어] "{traddr}/cntrl" 경로 버퍼. vfio-user target이 listen 중인 socket 파일. */

	snprintf(ctrlr_path, sizeof(ctrlr_path), "%s/cntrl", trid->traddr);
	/* [한국어] 트랜스포트 식별자의 traddr(디렉토리)에 "/cntrl" 부착. SPDK NVMe-oF
	 *         vfio-user target이 사용하는 약속된 컨트롤 소켓 이름. */
	ret = access(ctrlr_path, F_OK);
	/* [한국어] 파일 존재 여부 확인 — socket이 listen 중이라면 path가 존재. */
	if (ret != 0) {
		/* [한국어] 존재하지 않으면 target이 아직 안 떴거나 traddr 오기재. */
		SPDK_ERRLOG("Access path %s failed\n", ctrlr_path);
		return NULL;
	}

	vctrlr = calloc(1, sizeof(*vctrlr));
	/* [한국어] 컨트롤러 컨텍스트 0으로 초기화 할당. SPDK는 일반적으로 hugepage가
	 *         아닌 일반 heap 사용 — 컨트롤러 객체는 DMA 대상이 아니므로 OK. */
	if (!vctrlr) {
		/* [한국어] 메모리 부족 — 매우 드문 경우. 호출자(probe)가 처리. */
		return NULL;
	}

	vctrlr->dev = spdk_vfio_user_setup(ctrlr_path);
	/* [한국어] vfio-user 클라이언트 셋업: socket 연결, region 정보 교환,
	 *         DMA region 등록. 실패 시 NULL. */
	if (!vctrlr->dev) {
		/* [한국어] 셋업 실패 — vctrlr만 free하고 종료(아직 dev 없음). */
		SPDK_ERRLOG("Error to setup vfio device\n");
		free(vctrlr);
		return NULL;
	}

	ret = nvme_vfio_setup_bar0(vctrlr);
	/* [한국어] BAR0(doorbell 영역) mmap. */
	if (ret != 0) {
		/* [한국어] 매핑 실패 — exit 라벨로 가서 dev release + free. */
		SPDK_ERRLOG("Error to get device BAR0\n");
		goto exit;
	}

	pctrlr = &vctrlr->pctrlr;
	/* [한국어] 부모 PCIe 컨텍스트 단축 포인터. 이제부터 PCIe 코드와 공유되는
	 *         필드를 채운다. */
	pctrlr->doorbell_base = vctrlr->doorbell_base;
	/* [한국어] mmap한 doorbell 베이스를 PCIe 코드가 보는 자리에도 복사 — 핫패스
	 *         qpair_submit_request가 이 포인터로 SQ tail doorbell을 친다. */
	pctrlr->ctrlr.is_removed = false;
	/* [한국어] 컨트롤러 hot-removal 플래그. 정상 새 컨트롤러는 false. */
	pctrlr->ctrlr.opts = *opts;
	/* [한국어] 사용자 옵션 복사 (큐 크기, no_shn_notification 등). */
	pctrlr->ctrlr.trid = *trid;
	/* [한국어] 트랜스포트 ID 복사 — 추후 식별/로그/RPC에 사용. */
	pctrlr->ctrlr.opts.use_cmb_sqs = false;
	/* [한국어] vfio-user는 컨트롤러 메모리 버퍼 SQ를 지원하지 않으므로 강제 off.
	 *         설사 사용자가 켜달라고 해도 무시. */
	pctrlr->ctrlr.opts.admin_queue_size = spdk_max(pctrlr->ctrlr.opts.admin_queue_size,
					      NVME_PCIE_MIN_ADMIN_QUEUE_SIZE);
	/* [한국어] admin queue 크기 최소치 보장 (NVMe spec 권장 + SPDK 내부 상수).
	 *         사용자가 너무 작은 값을 줘도 안전한 크기로 끌어올림. */

	ret = nvme_ctrlr_construct(&pctrlr->ctrlr);
	/* [한국어] 공통 컨트롤러 초기화: 락, 상태, 풀, 콜백 슬롯 등. */
	if (ret != 0) {
		/* [한국어] 실패 — dev release + free 경로로. */
		goto exit;
	}

	/* Enable PCI busmaster and disable INTx */
	ret = spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2,
					    &cmd_reg, false);
	/* [한국어] PCI Config Space 오프셋 4 (CMD 레지스터, 2바이트) 읽기.
	 *         CONFIG_REGION_INDEX는 VFIO 표준의 PCI Config region. */
	if (ret != 0) {
		/* [한국어] 읽기 실패 — 컨트롤러 구성 롤백 후 exit. */
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "Read PCI CMD REG failed\n");
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		goto exit;
	}
	cmd_reg |= 0x404;
	/* [한국어] 0x404 = bit 2 (Bus Master Enable) | bit 10 (Interrupt Disable).
	 *         BME가 켜져야 디바이스가 호스트 메모리에 DMA 가능, INTx Disable로
	 *         polling-mode SPDK가 인터럽트 잡음 방지. */
	ret = spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2,
					    &cmd_reg, true);
	/* [한국어] 수정한 CMD 값을 다시 write. */
	if (ret != 0) {
		/* [한국어] write 실패 — 동일하게 롤백 후 exit. */
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "Write PCI CMD REG failed\n");
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		goto exit;
	}

	if (nvme_ctrlr_get_cap(&pctrlr->ctrlr, &cap)) {
		/* [한국어] CAP 레지스터(64-bit) 읽기. 내부적으로 vtable.get_reg_8 → 본 파일
		 *         get_reg_8 호출. CAP에는 doorbell stride(dstrd), MQES, CSS 등이 포함. */
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "get_cap() failed\n");
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		goto exit;
	}

	/* Doorbell stride is 2 ^ (dstrd + 2),
	 * but we want multiples of 4, so drop the + 2 */
	pctrlr->doorbell_stride_u32 = 1 << cap.bits.dstrd;
	/* [한국어] NVMe spec: 두 doorbell 간 거리 = 2^(2+DSTRD) 바이트. 4바이트(=1 DW)
	 *         단위로 표현하면 2^DSTRD DW. PCIe 코드가 이 값으로 doorbell_base에서
	 *         qpair별 doorbell DW 인덱스를 계산. */

	ret = nvme_pcie_ctrlr_construct_admin_qpair(&pctrlr->ctrlr, pctrlr->ctrlr.opts.admin_queue_size);
	/* [한국어] admin SQ/CQ 메모리 할당, PRP 풀, request 풀 초기화 — PCIe 코드 재사용. */
	if (ret != 0) {
		/* [한국어] admin qpair 구성 실패 — 컨트롤러 destruct 후 exit. */
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		goto exit;
	}

	/* Construct the primary process properties */
	ret = nvme_ctrlr_add_process(&pctrlr->ctrlr, 0);
	/* [한국어] SPDK는 멀티프로세스 공유 컨트롤러를 지원 — 여기선 primary(=처음 만든)
	 *         프로세스로 자기 자신을 등록. devhandle=0(미사용). */
	if (ret != 0) {
		/* [한국어] 등록 실패 — destruct 후 exit. */
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		goto exit;
	}

	return &pctrlr->ctrlr;
	/* [한국어] 성공: 사용자에게 spdk_nvme_ctrlr 핸들 반환. probe가 이를 콜백으로
	 *         사용자에게 전달. */

exit:
	/* [한국어] 공통 실패 정리 라벨: vfio dev 닫고 컨텍스트 free. nvme_ctrlr_construct
	 *         이전에 들어온 실패 경로에서는 nvme_ctrlr_destruct가 이미 호출되었거나
	 *         호출이 불필요한 상태. */
	spdk_vfio_user_release(vctrlr->dev);
	/* [한국어] vfio-user socket close + region unmap. */
	free(vctrlr);
	/* [한국어] 컨텍스트 메모리 해제. */
	return NULL;
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_scan - vfio-user 컨트롤러 발견(probe) 단계
 *
 * @probe_ctx:      probe 컨텍스트 (사용자가 spdk_nvme_probe로 시작; trid 포함).
 * @direct_connect: 직접 connect 호출인지 여부 (사용 안함, 인터페이스 호환용).
 * @return: 0 성공, 음수 실패.
 *
 * PCIe scan은 sysfs PCI 디바이스 열거를 하지만, vfio-user에서는 traddr이 곧
 * 단일 컨트롤러 socket 디렉토리 경로이므로 "그 경로가 존재하는지"만 확인하고
 * 즉시 nvme_ctrlr_probe로 넘긴다(=구체적 컨트롤러 생성 트리거).
 *
 * 호출 체인:
 *   spdk_nvme_probe → nvme_transport_ctrlr_scan(VFIOUSER vtable)
 *     → nvme_vfio_ctrlr_scan → nvme_ctrlr_probe → ... → nvme_vfio_ctrlr_construct
 */
static int
nvme_vfio_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx,
		     bool direct_connect)
{
	int ret;
	/* [한국어] access() 결과 보관. */

	if (probe_ctx->trid.trtype != SPDK_NVME_TRANSPORT_VFIOUSER) {
		/* [한국어] vtable 디스패치 단계에서 trtype 체크가 있지만 이중 가드.
		 *         다른 trtype이 잘못 들어오면 즉시 거부. */
		SPDK_ERRLOG("Can only use SPDK_NVME_TRANSPORT_VFIOUSER");
		return -EINVAL;
	}

	ret = access(probe_ctx->trid.traddr, F_OK);
	/* [한국어] traddr(socket 디렉토리) 존재 확인. construct는 "/cntrl"까지 본다. */
	if (ret != 0) {
		/* [한국어] 디렉토리/소켓 없음 — vfio-user target 미기동 또는 오타. */
		SPDK_ERRLOG("Error to access file %s\n", probe_ctx->trid.traddr);
		return ret;
	}
	SPDK_DEBUGLOG(nvme_vfio, "Scan controller : %s\n", probe_ctx->trid.traddr);
	/* [한국어] 발견 디버그 로그 — nvme_vfio 컴포넌트로 분류. */

	return nvme_ctrlr_probe(&probe_ctx->trid, probe_ctx, NULL);
	/* [한국어] 공통 probe 진입점: 콜백 호출(probe_cb→사용자 OK시 attach_cb) 후
	 *         nvme_transport_ctrlr_construct → nvme_vfio_ctrlr_construct. */
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_enable - admin queue 베이스 주소/크기를 컨트롤러 레지스터에 기록
 *
 * @ctrlr: 대상 컨트롤러
 * @return: 0 성공, -EIO 레지스터 write 실패
 *
 * NVMe spec 컨트롤러 enable 시퀀스: ASQ → ACQ → AQA → CC.EN=1.
 * 이 함수는 ASQ/ACQ/AQA 3개를 채워주며, CC.EN 토글은 상위 nvme_ctrlr 코드가
 * vtable.set_reg_4(CC offset, ...)로 수행한다.
 *
 * vadminq는 nvme_pcie_ctrlr_construct_admin_qpair에서 만든 admin qpair —
 * cmd_bus_addr/cpl_bus_addr/num_entries를 그대로 가져다 쓴다.
 *
 * 호출 체인: nvme_ctrlr_enable → vtable.ctrlr_enable → 이 함수.
 */
static int
nvme_vfio_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_qpair *vadminq = nvme_pcie_qpair(ctrlr->adminq);
	/* [한국어] admin qpair를 PCIe 형으로 다운캐스트 (vfio-user는 PCIe qpair를 재사용). */
	union spdk_nvme_aqa_register aqa;
	/* [한국어] AQA 레지스터 union. .bits.{asqs,acqs}로 비트필드 접근. */

	if (nvme_vfio_ctrlr_set_asq(ctrlr, vadminq->cmd_bus_addr)) {
		/* [한국어] admin SQ 메모리의 DMA(bus) 주소를 ASQ 레지스터에 write.
		 *         vfio-user의 IOMMU/DMA region 매핑이 호스트 PA를 그대로 쓰도록 설정됨. */
		NVME_CTRLR_ERRLOG(ctrlr, "set_asq() failed\n");
		return -EIO;
	}

	if (nvme_vfio_ctrlr_set_acq(ctrlr, vadminq->cpl_bus_addr)) {
		/* [한국어] admin CQ 메모리의 DMA 주소를 ACQ 레지스터에 write. */
		NVME_CTRLR_ERRLOG(ctrlr, "set_acq() failed\n");
		return -EIO;
	}

	aqa.raw = 0;
	/* [한국어] union 전체를 0으로 초기화 — 미사용 비트 보존. */
	/* acqs and asqs are 0-based. */
	aqa.bits.acqs = nvme_pcie_qpair(ctrlr->adminq)->num_entries - 1;
	/* [한국어] admin CQ 엔트리 수 - 1 (NVMe spec 0-based). */
	aqa.bits.asqs = nvme_pcie_qpair(ctrlr->adminq)->num_entries - 1;
	/* [한국어] admin SQ 엔트리 수 - 1. SPDK는 SQ/CQ 동일 크기 사용. */

	if (nvme_vfio_ctrlr_set_aqa(ctrlr, &aqa)) {
		/* [한국어] AQA 레지스터에 32-bit write — admin queue 크기 알림. */
		NVME_CTRLR_ERRLOG(ctrlr, "set_aqa() failed\n");
		return -EIO;
	}

	return 0;
	/* [한국어] 성공: 상위 코드가 이어서 CC.EN=1을 set. */
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_destruct - vfio-user 컨트롤러 종료/해제
 *
 * @ctrlr: 대상 컨트롤러
 * @return: 0 (현재 구현은 항상 성공)
 *
 * construct의 역순:
 *   1) admin qpair 메모리 해제 (PCIe 코드 재사용)
 *   2) 공통 컨트롤러 destruct 마무리
 *   3) vfio dev release (socket 닫기, region unmap)
 *   4) 컨텍스트 free
 */
static int
nvme_vfio_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);
	/* [한국어] vfio 컨텍스트 회복(나중에 dev 닫고 free하기 위함). */

	if (ctrlr->adminq) {
		/* [한국어] admin qpair가 만들어졌으면(보통 그렇다) 해제. */
		nvme_pcie_qpair_destroy(ctrlr->adminq);
	}

	nvme_ctrlr_destruct_finish(ctrlr);
	/* [한국어] 공통 컨트롤러 종료 마무리(잔여 콜백 정리, 풀 해제 등). */

	spdk_vfio_user_release(vctrlr->dev);
	/* [한국어] vfio-user 디바이스 해제: socket close + 매핑 해제. */
	free(vctrlr);
	/* [한국어] 컨텍스트 메모리 해제. */

	return 0;
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_get_max_xfer_size - 단일 I/O 최대 전송 크기 보고
 *
 * @ctrlr: (미사용 — vfio-user는 디바이스별 협상 없이 고정값 사용)
 * @return: NVME_MAX_XFER_SIZE (=128 KiB)
 *
 * 사용자/상위 모듈이 spdk_nvme_ns_cmd_*를 부를 때 split 기준으로 사용.
 * 더 큰 I/O는 빌더에서 자동 분할된다.
 */
static  uint32_t
nvme_vfio_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	return NVME_MAX_XFER_SIZE;
	/* [한국어] 128 KiB 고정. */
}

/*
 * [한국어]
 * nvme_vfio_ctrlr_get_max_sges - 단일 I/O 최대 SGE 수 보고
 *
 * @ctrlr: (미사용)
 * @return: NVME_MAX_SGES (=1)
 *
 * vfio-user는 SGL을 쓰지 않으므로 사실상 단일 연속 버퍼만 지원 → 1.
 */
static uint16_t
nvme_vfio_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	return NVME_MAX_SGES;
	/* [한국어] 1 고정. */
}

/*
 * [한국어]
 * vfio_ops - vfio-user 트랜스포트의 vtable
 *
 * SPDK NVMe 드라이버는 트랜스포트별로 spdk_nvme_transport_ops를 등록하고,
 * lib/nvme/nvme_transport.c의 디스패처가 trtype에 맞춰 적절한 함수를 호출한다.
 *
 * 이 vtable의 흥미로운 점은 **control plane 함수만 vfio 전용으로 새로 작성**
 * 하고, **data plane(qpair/poll_group)은 모두 nvme_pcie_* 함수를 그대로 재사용**
 * 한다는 점이다. 이는 nvme_vfio_ctrlr 구조체 첫 멤버를 nvme_pcie_ctrlr로 두어
 * 메모리 레이아웃이 호환되도록 설계했기 때문에 가능하다.
 *
 * 항목별 분류:
 *   [vfio 고유]   ctrlr_construct/scan/destruct/enable, get_registers,
 *                 set/get_reg_4/8, get_max_xfer_size, get_max_sges
 *   [PCIe 재사용] ctrlr_*_io_qpair, ctrlr_*_qpair, admin_qpair_abort_aers,
 *                 qpair_reset/abort_reqs/submit_request/process_completions,
 *                 poll_group_*  (사실상 모든 핫패스)
 */
const struct spdk_nvme_transport_ops vfio_ops = {
	.name = "VFIOUSER",
	/* [한국어] 트랜스포트 이름(로그/RPC에 노출). */
	.type = SPDK_NVME_TRANSPORT_VFIOUSER,
	/* [한국어] enum 식별자 — trid.trtype과 매칭된다. */
	.ctrlr_construct = nvme_vfio_ctrlr_construct,
	/* [한국어] 컨트롤러 생성. */
	.ctrlr_scan = nvme_vfio_ctrlr_scan,
	/* [한국어] probe 시 컨트롤러 발견. */
	.ctrlr_destruct = nvme_vfio_ctrlr_destruct,
	/* [한국어] 컨트롤러 해제. */
	.ctrlr_enable = nvme_vfio_ctrlr_enable,
	/* [한국어] ASQ/ACQ/AQA 설정. CC.EN=1은 상위 코드. */

	.ctrlr_get_registers = nvme_vfio_ctrlr_get_registers,
	/* [한국어] BAR0 매핑 포인터 반환(가능한 경우). */
	.ctrlr_set_reg_4 = nvme_vfio_ctrlr_set_reg_4,
	/* [한국어] 32-bit 컨트롤러 레지스터 write (vfio bar access). */
	.ctrlr_set_reg_8 = nvme_vfio_ctrlr_set_reg_8,
	/* [한국어] 64-bit 컨트롤러 레지스터 write. */
	.ctrlr_get_reg_4 = nvme_vfio_ctrlr_get_reg_4,
	/* [한국어] 32-bit 컨트롤러 레지스터 read. */
	.ctrlr_get_reg_8 = nvme_vfio_ctrlr_get_reg_8,
	/* [한국어] 64-bit 컨트롤러 레지스터 read (CAP 등). */

	.ctrlr_get_max_xfer_size = nvme_vfio_ctrlr_get_max_xfer_size,
	/* [한국어] 128 KiB 보고. */
	.ctrlr_get_max_sges = nvme_vfio_ctrlr_get_max_sges,
	/* [한국어] 1 보고 (SGL 미사용). */

	.ctrlr_create_io_qpair = nvme_pcie_ctrlr_create_io_qpair,
	/* [한국어] I/O qpair 생성 — PCIe 코드 그대로 사용. */
	.ctrlr_delete_io_qpair = nvme_pcie_ctrlr_delete_io_qpair,
	/* [한국어] I/O qpair 삭제. */
	.ctrlr_connect_qpair = nvme_pcie_ctrlr_connect_qpair,
	/* [한국어] qpair 컨트롤러에 연결 (Create SQ/CQ admin command 발행). */
	.ctrlr_disconnect_qpair = nvme_pcie_ctrlr_disconnect_qpair,
	/* [한국어] qpair 연결 해제 (Delete SQ/CQ admin command). */
	.admin_qpair_abort_aers = nvme_pcie_admin_qpair_abort_aers,
	/* [한국어] AER(Async Event Request)을 abort하는 admin 경로. */

	.qpair_reset = nvme_pcie_qpair_reset,
	/* [한국어] qpair 내부 상태 리셋 (실패 복구 등). */
	.qpair_abort_reqs = nvme_pcie_qpair_abort_reqs,
	/* [한국어] in-flight 요청들을 즉시 실패로 완료시킴 (큐 셧다운 시). */
	.qpair_submit_request = nvme_pcie_qpair_submit_request,
	/* [한국어] ★ 핫패스 ★ — SQE 기록 + doorbell write. mmap된 BAR0 doorbell을 직접 친다. */
	.qpair_process_completions = nvme_pcie_qpair_process_completions,
	/* [한국어] ★ 핫패스 ★ — CQE phase bit 폴링, 사용자 콜백 디스패치, doorbell 진행. */

	.poll_group_create = nvme_pcie_poll_group_create,
	/* [한국어] 여러 qpair를 하나의 poll group으로 묶어 통합 폴링 — PCIe 재사용. */
	.poll_group_connect_qpair = nvme_pcie_poll_group_connect_qpair,
	/* [한국어] qpair를 poll group에 연결. */
	.poll_group_disconnect_qpair = nvme_pcie_poll_group_disconnect_qpair,
	/* [한국어] qpair를 poll group에서 분리. */
	.poll_group_add = nvme_pcie_poll_group_add,
	/* [한국어] qpair 추가. */
	.poll_group_remove = nvme_pcie_poll_group_remove,
	/* [한국어] qpair 제거. */
	.poll_group_process_completions = nvme_pcie_poll_group_process_completions,
	/* [한국어] poll group 내 모든 qpair의 CQE를 통합 처리. */
	.poll_group_destroy = nvme_pcie_poll_group_destroy,
	/* [한국어] poll group 해제. */
	.poll_group_get_stats = nvme_pcie_poll_group_get_stats,
	/* [한국어] 통계 수집 (RPC/관측용). */
	.poll_group_free_stats = nvme_pcie_poll_group_free_stats
	/* [한국어] 통계 해제. */
};

SPDK_NVME_TRANSPORT_REGISTER(vfio, &vfio_ops);
/* [한국어] 정적 초기화자(__attribute__((constructor)))로 SPDK 시작 시 vtable 등록.
 *         이후 spdk_nvme_probe(trtype=VFIOUSER) 호출 시 위 vtable이 디스패치된다. */

SPDK_LOG_REGISTER_COMPONENT(nvme_vfio)
/* [한국어] 로그 컴포넌트 등록 — SPDK_DEBUGLOG(nvme_vfio, ...) 호출이
 *         런타임 로그 필터(`-L nvme_vfio`)로 on/off 된다. */
