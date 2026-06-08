/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK NVMe Controller 라이프사이클 구현 (nvme_ctrlr.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK NVMe 드라이버의 *컨트롤러 단위 상태머신*을 담당하는 가장 큰 모듈(5,600+ 라인)이다.
 * 핵심 책임:
 *   1) **Controller bring-up 상태머신** (`nvme_ctrlr_process_init`):
 *      INIT → DISABLE → ENABLE → CHECK_EN_DELAY → IDENTIFY → CONFIGURE_AER → IDENTIFY_NS
 *      → IDENTIFY_ID_DESC → IDENTIFY_IOCS_SPECIFIC → CONFIGURE_NUMBER_OF_QUEUES
 *      → CONFIGURE_HOST_ID → READY 의 비동기 상태 진행. 각 상태는 admin 명령 1개 또는 register
 *      access 1개에 해당하며, 완료 콜백이 다음 상태로 전이.
 *   2) **NVMe register access** (CC/CSTS/CAP/VS) — controller enable/disable, capability 검사
 *      모두 BAR0의 8/16/32/64-bit MMIO read/write로 수행. transport ops를 통해 추상화.
 *   3) **Identify Controller / Active NS list / IOCS-specific Controller**:
 *      cdata, primary_data, secondary_data 등 controller 메타데이터 취득.
 *   4) **Namespace tree 관리**: ctrlr->ns RB tree에 active NS를 lazy-insert + iteration.
 *   5) **AER (Async Event Request)**: nvme_ctrlr_construct_and_submit_aer — AER을 항상 outstanding
 *      유지하여 device-initiated event(NS Attribute Notice, Error Log, Firmware Activation 등) 수신.
 *   6) **Reset 시퀀스**: spdk_nvme_ctrlr_reset / disconnect 처리. controller fatal 또는 사용자
 *      명시 reset 시 SQ/CQ destroy → controller disable → 재 enable → re-identify 의 시퀀스.
 *   7) **Reference counting**: multi-process attach 시 process별 ref 추적.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   spdk_nvme_probe (lib/nvme/nvme.c)
 *     → nvme_transport_ctrlr_construct → ctrlr 객체 alloc + transport bring-up
 *     → init_ctrlrs 리스트로 이동
 *     → spdk_nvme_probe_poll_async → nvme_ctrlr_poll_internal:
 *         → ★ nvme_ctrlr_process_init (이 파일의 핵심 — 상태머신 진행)
 *             ├─ register access (CC.EN=0/1, CSTS.RDY 폴링, AQA 설정)
 *             ├─ Identify Controller → cdata 채움
 *             ├─ nvme_ctrlr_identify_active_ns_async → Active NS list (CNS=0x02)
 *             ├─ NS RB tree에 NSID 추가
 *             ├─ 각 NS에 대해 nvme_ns_construct (lib/nvme/nvme_ns.c)
 *             ├─ AER outstanding 등록
 *             └─ READY 상태
 *
 * 실행 컨텍스트: **호스트 유저스페이스 SPDK thread**. `nvme_ctrlr_process_init`은 polling 패턴이라
 * 사용자가 spdk_nvme_probe_poll_async 호출 시마다 한 단계씩 진행. 각 단계는 비동기 admin 명령을
 * 발행하고 완료 콜백에서 다음 상태로 전이.
 *
 * === 타 모듈과의 연결 ===
 * - 의존(이 파일이 호출하는 곳):
 *   · nvme_transport.c (register access, qpair connect/disconnect)
 *   · nvme_ctrlr_cmd.c (Identify, Get/Set Features, Format 등 admin 명령 빌더)
 *   · nvme_ns.c (nvme_ns_construct/destruct, NS RB tree)
 *   · nvme_qpair.c (admin qpair 처리)
 *   · nvme.c (nvme_completion_poll_cb, robust mutex, attached_ctrlrs 리스트)
 * - 의존받음:
 *   · nvme.c의 probe/connect/detach 진입점
 *   · 모든 사용자 코드 (spdk_nvme_ctrlr_get_*, spdk_nvme_ctrlr_alloc_io_qpair, ...)
 *   · NVMe-oF target (controller 객체를 backend로 사용)
 * - 공유 자료구조:
 *   · struct spdk_nvme_ctrlr (정의: nvme_internal.h:1497) — 본 파일이 이 객체의 거의 모든
 *     필드를 다룬다. ns RB tree(이 파일이 RB_GENERATE_STATIC), adminq, active_io_qpairs,
 *     state, cdata, opts, trid, transport ops vtable 등.
 *
 * === 주요 함수/구조체 요약 ===
 *  - nvme_ctrlr_process_init: 상태머신 진행 — 가장 핵심 함수. 사용자 폴링마다 호출됨.
 *  - nvme_ctrlr_identify (변형들): Identify Controller / Active NS list 등 발행.
 *  - nvme_ctrlr_identify_active_ns_async / nvme_ctrlr_destruct_namespaces:
 *      NS RB tree에 active NSID 채움 / 정리.
 *  - spdk_nvme_ctrlr_get_ns: 외부 공개 — RB_FIND + lazy alloc 패턴 (NSID로 NS 객체 조회).
 *  - spdk_nvme_ctrlr_reset / reset_async: controller reset 동기/비동기 변형.
 *  - nvme_ctrlr_construct_and_submit_aer: AER 항상 outstanding 유지.
 *  - nvme_ctrlr_proc_get_ref / put_ref: multi-process ref counting.
 *  - nvme_ns_cmp / RB_GENERATE_STATIC: NS tree의 비교 함수 + RB tree 코드 generation.
 *  - nvme_ctrlr_get_*_async 매크로: BAR register access 추상화.
 */

/* [한국어] stdinc.h: SPDK 공통 표준 라이브러리 (string.h, stdint.h, errno.h, assert.h 등 일괄 include). */
#include "spdk/stdinc.h"

/* [한국어] nvme_internal.h: NVMe 사적 자료구조 — struct spdk_nvme_ctrlr 등. */
#include "nvme_internal.h"
/* [한국어] nvme_io_msg.h: 외부 thread → controller 메시지 채널. */
#include "nvme_io_msg.h"

/* [한국어] env.h: DPDK 추상화. */
#include "spdk/env.h"
/* [한국어] string.h: SPDK 문자열 헬퍼. */
#include "spdk/string.h"
/* [한국어] endian.h: 바이트 오더 변환 (NVMe register는 little-endian, MMIO 정렬 유틸). */
#include "spdk/endian.h"

/* [한국어] forward declaration — Active NS 비동기 컨텍스트 (이 파일 내부에서 정의됨). */
struct nvme_active_ns_ctx;

/* [한국어] static forward declarations — 함수들이 서로 호출하는데 정의 순서가 다양하므로 미리 선언. */
/* [한국어] AER을 새로 발행 (이전 AER 완료 후 항상 한 개를 outstanding 유지하기 위함). */
static int nvme_ctrlr_construct_and_submit_aer(struct spdk_nvme_ctrlr *ctrlr,
		struct nvme_async_event_request *aer);
/* [한국어] Active NS list (CNS=0x02) 비동기 발행 진입점. */
static void nvme_ctrlr_identify_active_ns_async(struct nvme_active_ns_ctx *ctx);
/* [한국어] Identify NS (CNS=0x00) 비동기 — 이 파일 내부 buffer 채워짐 후 nvme_ns.c와 협력. */
static int nvme_ctrlr_identify_ns_async(struct spdk_nvme_ns *ns);
/* [한국어] IOCS-specific Identify NS 비동기 (ZNS/NVM 분기). */
static int nvme_ctrlr_identify_ns_iocs_specific_async(struct spdk_nvme_ns *ns);
/* [한국어] NS ID Descriptor List 비동기. */
static int nvme_ctrlr_identify_id_desc_async(struct spdk_nvme_ns *ns);
/* [한국어] CAP register로부터 controller 가용성 비트 추출 (timeout, page size 한도 등). */
static void nvme_ctrlr_init_cap(struct spdk_nvme_ctrlr *ctrlr);
/* [한국어] state machine 다음 상태로 전이 + timeout 시각 갱신. */
static void nvme_ctrlr_set_state(struct spdk_nvme_ctrlr *ctrlr, enum nvme_ctrlr_state state,
				 uint64_t timeout_in_ms);

/*
 * [한국어]
 * nvme_ns_cmp - NS RB tree의 비교 함수 (NSID 기준 오름차순 정렬)
 *
 * @ns1, @ns2: 비교할 두 NS 객체
 * @return: -1 (ns1 < ns2), 0 (equal), 1 (ns1 > ns2)
 *
 * RB_GENERATE_STATIC 매크로가 이 함수를 사용하여 RB tree의 균형 유지·검색·삽입 코드를 생성.
 * NSID는 1-based unsigned 32-bit이므로 단순 비교만으로 정렬 가능.
 */
static int
nvme_ns_cmp(struct spdk_nvme_ns *ns1, struct spdk_nvme_ns *ns2)
{
	/* [한국어] ns1.id < ns2.id — 앞에 위치. */
	if (ns1->id < ns2->id) {
		return -1;
	/* [한국어] ns1.id > ns2.id — 뒤에 위치. */
	} else if (ns1->id > ns2->id) {
		return 1;
	} else {
		/* [한국어] 같은 NSID — RB tree에선 중복 키 발생 안 해야 함 (RB_INSERT가 중복이면 기존 객체 반환). */
		return 0;
	}
}

/* [한국어] RB tree 코드 생성 — `nvme_ns_tree` 라는 이름의 tree에 대해 insert/find/remove 등 함수 자동 생성.
 *  · STATIC: 이 파일 내부에서만 사용 (외부 노출 X).
 *  · key 비교는 nvme_ns_cmp가 담당. */
RB_GENERATE_STATIC(nvme_ns_tree, spdk_nvme_ns, node, nvme_ns_cmp);

/*
 * [한국어]
 * nvme_ctrlr_get_reg_async - NVMe BAR register 비동기 read 매크로 (sz = 4 또는 8)
 *
 * Token-paste 매크로 — sz 값에 따라 nvme_transport_ctrlr_get_reg_4_async 또는 _8_async로 dispatch.
 * 각 register는 spec 정의된 offset이 spdk_nvme_registers 구조체에 그대로 매핑되어 있으므로
 * offsetof()로 BAR0 내 위치 산출.
 *
 * 사용 예: nvme_ctrlr_get_reg_async(ctrlr, csts, 4, cb_fn, cb_arg)
 *           → nvme_transport_ctrlr_get_reg_4_async(ctrlr, offsetof(..., csts), cb_fn, cb_arg)
 */
#define nvme_ctrlr_get_reg_async(ctrlr, reg, sz, cb_fn, cb_arg) \
	nvme_transport_ctrlr_get_reg_ ## sz ## _async(ctrlr, \
		offsetof(struct spdk_nvme_registers, reg), cb_fn, cb_arg)

/* [한국어] BAR register 비동기 write 매크로 — get과 동일 패턴, val 인자 추가. */
#define nvme_ctrlr_set_reg_async(ctrlr, reg, sz, val, cb_fn, cb_arg) \
	nvme_transport_ctrlr_set_reg_ ## sz ## _async(ctrlr, \
		offsetof(struct spdk_nvme_registers, reg), val, cb_fn, cb_arg)

/* [한국어] CC(Controller Configuration, offset 0x14) 4-byte read — controller enable/page size/AMS 등 설정값 조회. */
#define nvme_ctrlr_get_cc_async(ctrlr, cb_fn, cb_arg) \
	nvme_ctrlr_get_reg_async(ctrlr, cc, 4, cb_fn, cb_arg)

/* [한국어] CSTS(Controller Status, offset 0x1C) 4-byte read — RDY/CFS/SHST 등 상태 비트 조회.
 *  · CSTS.RDY는 enable/disable 시퀀스 polling의 핵심. */
#define nvme_ctrlr_get_csts_async(ctrlr, cb_fn, cb_arg) \
	nvme_ctrlr_get_reg_async(ctrlr, csts, 4, cb_fn, cb_arg)

/* [한국어] CAP(Controller Capabilities, offset 0x00) 8-byte read — MQES, TO, CSS, MPSMIN/MAX 등 가용성. */
#define nvme_ctrlr_get_cap_async(ctrlr, cb_fn, cb_arg) \
	nvme_ctrlr_get_reg_async(ctrlr, cap, 8, cb_fn, cb_arg)

/* [한국어] VS(Version, offset 0x08) 4-byte read — 컨트롤러가 지원하는 NVMe major/minor/tertiary 버전. */
#define nvme_ctrlr_get_vs_async(ctrlr, cb_fn, cb_arg) \
	nvme_ctrlr_get_reg_async(ctrlr, vs, 4, cb_fn, cb_arg)

/* [한국어] CC 4-byte write — controller enable(EN=1)/disable(EN=0)·shutdown 트리거에 사용. */
#define nvme_ctrlr_set_cc_async(ctrlr, value, cb_fn, cb_arg) \
	nvme_ctrlr_set_reg_async(ctrlr, cc, 4, value, cb_fn, cb_arg)

/*
 * [한국어]
 * nvme_ctrlr_get_cc - CC(Controller Configuration) register 동기 read
 *
 * @ctrlr: 대상 controller
 * @cc: out — CC 값 4-byte 저장
 * @return: 0 성공, 음수 errno (transport read 실패)
 *
 * CC 비트필드:
 *   - EN (bit 0): 1 = enable, 0 = disable
 *   - CSS (bit 4-6): Command Set Selected (NVM/Discovery 등)
 *   - MPS (bit 7-10): Memory Page Size (2^(12+MPS))
 *   - AMS (bit 11-13): Arbitration Mechanism Selected
 *   - SHN (bit 14-15): Shutdown Notification (00=No, 01=Normal, 10=Abrupt)
 *   - IOSQES/IOCQES (bit 16-19, 20-23): IO SQ/CQ Entry Size (log2)
 */
static int
nvme_ctrlr_get_cc(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_cc_register *cc)
{
	/* [한국어] transport ops로 BAR0+offsetof(cc.raw) 위치에서 4-byte MMIO read. */
	return nvme_transport_ctrlr_get_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, cc.raw),
					      &cc->raw);
}

/*
 * [한국어]
 * nvme_ctrlr_get_csts - CSTS(Controller Status) register 동기 read
 *
 * CSTS 비트필드:
 *   - RDY (bit 0): 1 = ready (CC.EN과 sync), 0 = not ready
 *   - CFS (bit 1): Controller Fatal Status — fatal error 발생 신호
 *   - SHST (bit 2-3): Shutdown Status (00=Normal, 01=in progress, 10=complete)
 *   - NSSRO/PP (bit 4, 5): NVM Subsystem Reset Occurred / Processing Paused
 */
static int
nvme_ctrlr_get_csts(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_csts_register *csts)
{
	/* [한국어] CSTS 4-byte MMIO read. */
	return nvme_transport_ctrlr_get_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, csts.raw),
					      &csts->raw);
}

/*
 * [한국어]
 * nvme_ctrlr_get_cap - CAP(Controller Capabilities) register 동기 read
 *
 * CAP는 8-byte register — controller가 호스트에 노출하는 가용성 정보:
 *   - MQES (bit 0-15): Maximum Queue Entries Supported (0-based)
 *   - CQR (bit 16): Contiguous Queues Required
 *   - AMS (bit 17-18): Arbitration Mechanism Supported
 *   - TO (bit 24-31): Timeout — controller ready 도달까지의 최대 시간 (500ms 단위)
 *   - DSTRD (bit 32-35): Doorbell Stride — doorbell register 간격
 *   - NSSRS (bit 36): NVM Subsystem Reset Supported
 *   - CSS (bit 37-44): Command Sets Supported
 *   - MPSMIN/MPSMAX (bit 48-51, 52-55): Min/Max Memory Page Size
 */
int
nvme_ctrlr_get_cap(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_cap_register *cap)
{
	/* [한국어] CAP 8-byte read — 단일 PCIe TLP로 atomic 가능 (CAP는 read-only). */
	return nvme_transport_ctrlr_get_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, cap.raw),
					      &cap->raw);
}

/*
 * [한국어]
 * nvme_ctrlr_get_vs - VS(Version) register 동기 read
 *
 * VS 4-byte: bit 0-7 tertiary, 8-15 minor, 16-31 major. 예: 0x00010300 = 1.3.0.
 */
int
nvme_ctrlr_get_vs(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_vs_register *vs)
{
	/* [한국어] VS 4-byte read. */
	return nvme_transport_ctrlr_get_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, vs.raw),
					      &vs->raw);
}

/*
 * [한국어]
 * nvme_ctrlr_get_cmbsz - CMBSZ(Controller Memory Buffer Size) register read
 *
 * CMB(Controller Memory Buffer)는 controller가 호스트에 노출하는 자체 메모리 (BAR4 등). SQ를 CMB에
 * 두면 doorbell 1번으로 SQ entry write까지 처리되어 latency 감소 (use_cmb_sqs 옵션).
 */
int
nvme_ctrlr_get_cmbsz(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_cmbsz_register *cmbsz)
{
	/* [한국어] CMBSZ 4-byte — CMB 크기·기능 비트. */
	return nvme_transport_ctrlr_get_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, cmbsz.raw),
					      &cmbsz->raw);
}

/*
 * [한국어]
 * nvme_ctrlr_get_pmrcap - PMRCAP(Persistent Memory Region Capabilities) register read
 *
 * PMR은 controller에 부착된 persistent memory를 host가 직접 접근할 수 있게 한 기능 (NVMe 1.4).
 */
int
nvme_ctrlr_get_pmrcap(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_pmrcap_register *pmrcap)
{
	/* [한국어] PMRCAP 4-byte read — PMR 지원 여부, 크기 등. */
	return nvme_transport_ctrlr_get_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, pmrcap.raw),
					      &pmrcap->raw);
}

/*
 * [한국어]
 * nvme_ctrlr_get_bpinfo - BPINFO(Boot Partition Info) register read
 *
 * Boot Partition은 BIOS/firmware가 부팅 코드를 로드할 수 있게 NVMe SSD가 제공하는 영역.
 */
int
nvme_ctrlr_get_bpinfo(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_bpinfo_register *bpinfo)
{
	/* [한국어] BPINFO 4-byte read — boot partition 사이즈·상태 비트. */
	return nvme_transport_ctrlr_get_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, bpinfo.raw),
					      &bpinfo->raw);
}

/*
 * [한국어]
 * nvme_ctrlr_set_bprsel - BPRSEL(Boot Partition Read Select) register write
 *
 * Boot partition을 호스트가 read하기 위해 select하는 register.
 */
int
nvme_ctrlr_set_bprsel(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_bprsel_register *bprsel)
{
	/* [한국어] BPRSEL 4-byte write — read 시작 LBA 등 지정. */
	return nvme_transport_ctrlr_set_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, bprsel.raw),
					      bprsel->raw);
}

/*
 * [한국어]
 * nvme_ctrlr_set_bpmbl - BPMBL(Boot Partition Memory Buffer Location, 8 bytes) BAR0 레지스터 write
 *
 * @ctrlr: 대상 컨트롤러
 * @bpmbl_value: physical/IOVA 주소 — Boot Partition 데이터를 host 메모리 어디에 DMA 할지
 * @return 0 성공, 음수 errno (트랜스포트 write 실패)
 *
 * NVMe 1.4 §3.1.20: BPMBL 은 BP read 시 데이터가 작성될 host buffer 의 base 주소.
 * 호스트가 hugepage DMA 가능 영역의 IOVA 를 PRP-style 로 등록 후 BPRSEL.BPID 와 함께 BP read 트리거.
 *
 * 호출 체인:
 *   spdk_nvme_ctrlr_read_boot_partition_start → [본 함수] → transport set_reg_8
 *   → PCIe MMIO write 8B (BAR0 offset 0x40) / Fabrics property set
 */
int
nvme_ctrlr_set_bpmbl(struct spdk_nvme_ctrlr *ctrlr, uint64_t bpmbl_value)
{
	return nvme_transport_ctrlr_set_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, bpmbl),
					      bpmbl_value);
	                                  /* [한국어] BPMBL offset (=0x40) 에 8B write — DMA 대상 주소 등록. */
}

/*
 * [한국어]
 * nvme_ctrlr_set_nssr - NSSR(NVM Subsystem Reset) register write
 *
 * NSSR(offset 0x20)에 매직 값 0x4E564D65 ("NVMe") 쓰면 controller 자체가 NVM Subsystem reset 수행.
 * 이는 CC.EN 토글보다 더 강력한 reset (전체 subsystem 재초기화).
 */
static int
nvme_ctrlr_set_nssr(struct spdk_nvme_ctrlr *ctrlr, uint32_t nssr_value)
{
	/* [한국어] NSSR 4-byte write — value는 보통 0x4E564D65 ("NVMe" magic). */
	return nvme_transport_ctrlr_set_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, nssr),
					      nssr_value);
}

/*
 * [한국어]
 * nvme_ctrlr_multi_iocs_enabled - controller가 multi-IOCS 모드로 동작 중인지
 *
 * @return: true — IOCS specific NS data 의미 있음, NS construct 시 IOCS-specific identify 발행 필요
 *
 * 두 조건 모두 만족해야 함:
 *   1) CAP.CSS에 IOCS 비트 set (controller가 ZNS/KV 등 명령 셋 지원)
 *   2) CC.CSS = SPDK_NVME_CC_CSS_IOCS (현재 IOCS 명령 셋으로 활성화 — opts.command_set이 결정)
 *
 * Caller: nvme_ns_construct (IOCS-specific identify 발행 가드).
 */
bool
nvme_ctrlr_multi_iocs_enabled(struct spdk_nvme_ctrlr *ctrlr)
{
	/* [한국어] 두 조건 AND — CAP.CSS bit + opts.command_set 일치. */
	return ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_IOCS &&
	       ctrlr->opts.command_set == SPDK_NVME_CC_CSS_IOCS;
}

/* When the field in spdk_nvme_ctrlr_opts are changed and you change this function, please
 * also update the nvme_ctrl_opts_init function in nvme_ctrlr.c
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_default_ctrlr_opts - 컨트롤러 attach 옵션 기본값으로 채움 (공개 API)
 *
 * @opts: [out] 옵션 구조체. caller 가 alloc 후 전달.
 * @opts_size: opts 의 실제 크기 (ABI 호환용 — SPDK 가 옵션 확장해도 구버전 호환).
 *
 * 동작: SET_FIELD 매크로로 ABI-safe 필드별 기본값 설정. 매크로는 offsetof + sizeof 검사로
 * opts 크기 범위를 벗어나는 필드는 skip — 사용자가 sizeof(struct spdk_nvme_ctrlr_opts) 보다
 * 작은 버전을 전달해도 안전.
 *
 * 주요 기본값:
 *   - num_io_queues = DEFAULT_MAX_IO_QUEUES (1024) — 호스트가 원하는 최대 I/O qpair 수
 *   - arb_mechanism = SPDK_NVME_CC_AMS_RR (Round Robin)
 *   - keep_alive_timeout_ms = 0 (PCIe), 10초 (Fabrics) — Fabrics 는 의무
 *   - admin_timeout_ms = NVME_MAX_ADMIN_TIMEOUT_IN_SECS × 1000 (30초)
 *
 * 사용 패턴:
 *   struct spdk_nvme_ctrlr_opts opts;
 *   spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, sizeof(opts));
 *   opts.num_io_queues = 8;  // 사용자 override
 *   spdk_nvme_probe(..., &opts);
 *
 * 동기화: opts 는 사용자 스택/heap 메모리라 잠금 불필요.
 */
void
spdk_nvme_ctrlr_get_default_ctrlr_opts(struct spdk_nvme_ctrlr_opts *opts, size_t opts_size)
{
	assert(opts);

	opts->opts_size = opts_size;

#define FIELD_OK(field) \
	offsetof(struct spdk_nvme_ctrlr_opts, field) + sizeof(opts->field) <= opts_size

#define SET_FIELD(field, value) \
	if (offsetof(struct spdk_nvme_ctrlr_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = value; \
	} \

	SET_FIELD(num_io_queues, DEFAULT_MAX_IO_QUEUES);
	SET_FIELD(use_cmb_sqs, false);
	SET_FIELD(no_shn_notification, false);
	SET_FIELD(enable_interrupts, false);
	SET_FIELD(arb_mechanism, SPDK_NVME_CC_AMS_RR);
	SET_FIELD(arbitration_burst, 0);
	SET_FIELD(low_priority_weight, 0);
	SET_FIELD(medium_priority_weight, 0);
	SET_FIELD(high_priority_weight, 0);
	SET_FIELD(keep_alive_timeout_ms, MIN_KEEP_ALIVE_TIMEOUT_IN_MS);
	SET_FIELD(transport_retry_count, SPDK_NVME_DEFAULT_RETRY_COUNT);
	SET_FIELD(io_queue_size, DEFAULT_IO_QUEUE_SIZE);

	if (nvme_driver_init() == 0) {
		if (FIELD_OK(hostnqn)) {
			nvme_get_default_hostnqn(opts->hostnqn, sizeof(opts->hostnqn));
		}

		if (FIELD_OK(extended_host_id)) {
			memcpy(opts->extended_host_id, &g_spdk_nvme_driver->default_extended_host_id,
			       sizeof(opts->extended_host_id));
		}

	}

	SET_FIELD(io_queue_requests, DEFAULT_IO_QUEUE_REQUESTS);

	if (FIELD_OK(src_addr)) {
		memset(opts->src_addr, 0, sizeof(opts->src_addr));
	}

	if (FIELD_OK(src_svcid)) {
		memset(opts->src_svcid, 0, sizeof(opts->src_svcid));
	}

	if (FIELD_OK(host_id)) {
		memset(opts->host_id, 0, sizeof(opts->host_id));
	}

	SET_FIELD(command_set, CHAR_BIT);
	SET_FIELD(admin_timeout_ms, NVME_MAX_ADMIN_TIMEOUT_IN_SECS * 1000);
	SET_FIELD(header_digest, false);
	SET_FIELD(data_digest, false);
	SET_FIELD(disable_error_logging, false);
	SET_FIELD(transport_ack_timeout, SPDK_NVME_DEFAULT_TRANSPORT_ACK_TIMEOUT);
	SET_FIELD(admin_queue_size, DEFAULT_ADMIN_QUEUE_SIZE);
	SET_FIELD(fabrics_connect_timeout_us, NVME_FABRIC_CONNECT_COMMAND_TIMEOUT);
	SET_FIELD(disable_read_ana_log_page, false);
	SET_FIELD(disable_read_changed_ns_list_log_page, false);
	SET_FIELD(tls_psk, NULL);
	SET_FIELD(dhchap_key, NULL);
	SET_FIELD(dhchap_ctrlr_key, NULL);
	SET_FIELD(dhchap_digests,
		  SPDK_BIT(SPDK_NVMF_DHCHAP_HASH_SHA256) |
		  SPDK_BIT(SPDK_NVMF_DHCHAP_HASH_SHA384) |
		  SPDK_BIT(SPDK_NVMF_DHCHAP_HASH_SHA512));
	SET_FIELD(dhchap_dhgroups,
		  SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_NULL) |
		  SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_2048) |
		  SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_3072) |
		  SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_4096) |
		  SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_6144) |
		  SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_8192));
#undef FIELD_OK
#undef SET_FIELD
}

const struct spdk_nvme_ctrlr_opts *
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_opts - controller의 현재 opts 포인터 반환 (사용자 노출 API)
 *
 * @return ctrlr->opts에 대한 포인터.
 *
 * 주의: 사용자가 직접 수정 가능. 일부 필드(io_queue_size 등)는 process_init에서 정규화된 값.
 *       hostnqn 등 일부 필드 변경 시 다음 reset부터 적용. 즉시 적용 안 됨.
 */
struct spdk_nvme_ctrlr_opts *
spdk_nvme_ctrlr_get_opts(struct spdk_nvme_ctrlr *ctrlr)
{
	return &ctrlr->opts;
                                  /* [한국어] 단순 필드 포인터 반환 — 락 없음 (read는 안전, write는 사용자 책임) */
}

/**
 * This function will be called when the process allocates the IO qpair.
 * Note: the ctrlr_lock must be held when calling this function.
 */
/*
 * [한국어]
 * nvme_ctrlr_proc_add_io_qpair - alloc된 IO qpair를 현 프로세스의 process 객체에 등록
 *
 * @qpair: 등록할 qpair (이미 alloc + transport create_io_qpair 완료된 상태)
 *
 * Multi-process 안전성: 같은 controller에 여러 프로세스가 attach된 경우, 각 프로세스는 자신의
 * spdk_nvme_ctrlr_process 객체에 자기가 만든 qpair만 등록. 다른 프로세스의 qpair는 추적 X.
 *
 * Caller invariant: ctrlr_lock 보유 상태 (active_procs 리스트 race 방지).
 *
 * 동작:
 *   nvme_ctrlr_get_current_process(ctrlr) — 현 PID의 process 객체 검색.
 *   있으면 qpair를 allocated_io_qpairs에 추가 + qpair->active_proc back-pointer 설정.
 *   없으면 noop (공유 컨트롤러 attach 못 한 상태 — 이 프로세스는 qpair 생성 안 함).
 */
static void
nvme_ctrlr_proc_add_io_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr_process	*active_proc;
	struct spdk_nvme_ctrlr		*ctrlr = qpair->ctrlr;

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
                                  /* [한국어] 현 PID의 process 객체 검색 — active_procs TAILQ에서 PID 매칭 */
	if (active_proc) {
		TAILQ_INSERT_TAIL(&active_proc->allocated_io_qpairs, qpair, per_process_tailq);
                                  /* [한국어] process별 qpair 리스트에 추가 — 프로세스 종료 시 일괄 정리에 사용 */
		qpair->active_proc = active_proc;
                                  /* [한국어] back-pointer — qpair에서 자기 process 즉시 회수 가능 */
	}
}

/**
 * This function will be called when the process frees the IO qpair.
 * Note: the ctrlr_lock must be held when calling this function.
 */
/*
 * [한국어]
 * nvme_ctrlr_proc_remove_io_qpair - 현 프로세스의 process 객체에서 qpair 제거
 *
 * IO qpair를 free할 때 호출. 해당 process의 allocated_io_qpairs 리스트에서 검색 후 제거.
 * 다른 process가 같은 ctrlr의 다른 qpair를 들고 있어도 이 호출은 영향 없음.
 *
 * Caller invariant: ctrlr_lock 보유 상태.
 */
static void
nvme_ctrlr_proc_remove_io_qpair(struct spdk_nvme_qpair *qpair)
{
	/* [한국어] 현 프로세스의 process 객체. */
	struct spdk_nvme_ctrlr_process	*active_proc;
	/* [한국어] qpair 소속 ctrlr. */
	struct spdk_nvme_ctrlr		*ctrlr = qpair->ctrlr;
	/* [한국어] 순회 변수. */
	struct spdk_nvme_qpair          *active_qpair, *tmp_qpair;

	/* [한국어] PID 일치하는 active_procs 항목 검색. */
	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	/* [한국어] 다른 프로세스 소속 qpair — 처리 권한 없음. */
	if (!active_proc) {
		return;
	}

	/* [한국어] allocated_io_qpairs 순회 — 일치 qpair 발견 시 제거. _SAFE는 순회 중 제거 안전. */
	TAILQ_FOREACH_SAFE(active_qpair, &active_proc->allocated_io_qpairs,
			   per_process_tailq, tmp_qpair) {
		/* [한국어] 포인터 일치 검사. */
		if (active_qpair == qpair) {
			/* [한국어] 리스트에서 제거 — qpair 객체 자체 free는 caller 책임. */
			TAILQ_REMOVE(&active_proc->allocated_io_qpairs,
				     active_qpair, per_process_tailq);

			/* [한국어] 한 번 발견하면 종료 — 같은 qpair 중복 등록 안 됨. */
			break;
		}
	}
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_default_io_qpair_opts - IO qpair opts 기본값 채우기 (사용자 노출 API)
 *
 * @ctrlr:     기본값 출처 controller (io_queue_size 등 ctrlr->opts에서 상속)
 * @opts:      [out] 채울 opts 구조체
 * @opts_size: 사용자가 알고 있는 opts 크기 (ABI 호환)
 *
 * 동작:
 *   memset 0으로 초기화 → opts_size 저장 → FIELD_OK + SET_FIELD 매크로로 ABI 안전 필드별 채움.
 *
 * 기본값 정책:
 *   qprio = URGENT (가장 높은 우선순위, RR 모드 호환)
 *   io_queue_size/requests = ctrlr 기본값 상속 (set_num_queues에서 협상된 크기)
 *   delay_cmd_submit = false (즉시 doorbell — interrupt 모드와 충돌)
 *   sq/cq vaddr/paddr/buffer_size = 0 (트랜스포트가 자동 할당)
 *   create_only = false (alloc + connect 한 번에)
 *   async_mode = false (동기 connect 폴링)
 *   disable_pcie_sgl_merge = false (PCIe SGL merge 최적화 활성)
 *
 * 사용처: 사용자가 spdk_nvme_ctrlr_alloc_io_qpair 호출 전 opts 준비 시 가장 먼저 호출 → 일부만 덮어쓰기.
 */
void
spdk_nvme_ctrlr_get_default_io_qpair_opts(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_io_qpair_opts *opts,
		size_t opts_size)
{
	assert(ctrlr);

	assert(opts);

	memset(opts, 0, opts_size);
                                  /* [한국어] 0 초기화 — 모든 포인터/플래그 NULL/false 시작 */
	opts->opts_size = opts_size;
                                  /* [한국어] 사용자가 알고 있는 크기 보존 — 이후 ABI 호환 검사에 사용 */

#define FIELD_OK(field) \
	offsetof(struct spdk_nvme_io_qpair_opts, field) + sizeof(opts->field) <= opts_size
                                  /* [한국어] ABI 호환 검사 — 필드가 사용자 opts_size 안에 들어가는지 */

#define SET_FIELD(field, value) \
        if (FIELD_OK(field)) { \
                opts->field = value; \
        } \
                                  /* [한국어] FIELD_OK 통과 시만 값 설정 — 구버전 사용자는 새 필드 무시 */

	SET_FIELD(qprio, SPDK_NVME_QPRIO_URGENT);
                                  /* [한국어] 우선순위 URGENT — RR(Round Robin) 모드에서 유일한 유효값 */
	SET_FIELD(io_queue_size, ctrlr->opts.io_queue_size);
                                  /* [한국어] ctrlr 기본 큐 크기 상속 — set_num_queues에서 정규화된 값 */
	SET_FIELD(io_queue_requests, ctrlr->opts.io_queue_requests);
                                  /* [한국어] outstanding request 풀 크기 (>= io_queue_size) */
	SET_FIELD(delay_cmd_submit, false);
                                  /* [한국어] 즉시 doorbell write — true면 batching 후 한꺼번에 (interrupt 모드와 비호환) */
	SET_FIELD(sq.vaddr, NULL);
	SET_FIELD(sq.paddr, 0);
	SET_FIELD(sq.buffer_size, 0);
                                  /* [한국어] SQ 버퍼 — NULL이면 트랜스포트가 hugepage 자동 할당, 사용자 제공 시 CMB 등 활용 가능 */
	SET_FIELD(cq.vaddr, NULL);
	SET_FIELD(cq.paddr, 0);
	SET_FIELD(cq.buffer_size, 0);
                                  /* [한국어] CQ 버퍼 — 일반적으로 NULL (자동 할당). CMB CQ는 거의 안 씀. */
	SET_FIELD(create_only, false);
                                  /* [한국어] alloc + connect 한 번에. true면 alloc만 하고 사용자가 connect 별도 호출 */
	SET_FIELD(async_mode, false);
                                  /* [한국어] 동기 connect 폴링. true면 비동기 — process_completions로 진행 */
	SET_FIELD(disable_pcie_sgl_merge, false);
                                  /* [한국어] PCIe SGL merge 최적화 — 인접 SGE 병합. true로 비활성하면 일부 디버깅 시나리오에 유용 */

#undef FIELD_OK
#undef SET_FIELD
}

/*
 * [한국어]
 * nvme_ctrlr_io_qpair_opts_copy - 사용자 opts → 라이브러리 opts ABI 안전 복사 (내부 헬퍼)
 *
 * @dst:           [out] 라이브러리가 사용할 정상화된 opts (이미 기본값 채워진 상태)
 * @src:           사용자 제공 opts
 * @opts_size_src: 사용자가 알고 있는 opts 크기
 *
 * 동작: get_default_io_qpair_opts와 동일한 FIELD_OK 패턴 — opts_size_src 안에 들어가는 필드만 dst에 복사.
 *       마지막에 dst->opts_size = opts_size_src로 갱신 (이후 다른 함수가 이 크기로 검사 가능).
 *
 * SPDK_STATIC_ASSERT(sizeof == 80): 새 필드 추가 시 컴파일 타임 가드. 누군가 필드 추가하면
 *                                    이 assert가 깨지므로 SET_FIELD 추가 + 80을 새 크기로 갱신해야 함.
 */
static void
nvme_ctrlr_io_qpair_opts_copy(struct spdk_nvme_io_qpair_opts *dst,
			      const struct spdk_nvme_io_qpair_opts *src, size_t opts_size_src)
{
	if (!opts_size_src) {
                                  /* [한국어] opts_size 0은 의미 없음 — 사용자가 정상 구조체를 줘야 함 */
		SPDK_ERRLOG("opts_size_src should not be zero value\n");
		assert(false);
	}

#define FIELD_OK(field) \
        offsetof(struct spdk_nvme_io_qpair_opts, field) + sizeof(src->field) <= opts_size_src

#define SET_FIELD(field) \
        if (FIELD_OK(field)) { \
                dst->field = src->field; \
        } \

	SET_FIELD(qprio);
	SET_FIELD(io_queue_size);
	SET_FIELD(io_queue_requests);
	SET_FIELD(delay_cmd_submit);
	SET_FIELD(sq.vaddr);
	SET_FIELD(sq.paddr);
	SET_FIELD(sq.buffer_size);
	SET_FIELD(cq.vaddr);
	SET_FIELD(cq.paddr);
	SET_FIELD(cq.buffer_size);
	SET_FIELD(create_only);
	SET_FIELD(async_mode);
	SET_FIELD(disable_pcie_sgl_merge);
                                  /* [한국어] 13개 필드 모두 — get_default와 같은 순서 (가독성) */

	dst->opts_size = opts_size_src;
                                  /* [한국어] dst의 size를 사용자 size로 갱신 — 일관성 유지 */

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_io_qpair_opts) == 80, "Incorrect size");
                                  /* [한국어] 컴파일 타임 가드 — 누군가 필드 추가하면 이 assert가 깨짐.
                                   *  깨진 사람이 SET_FIELD 추가 + 80을 새 크기로 갱신해야 함. */

#undef FIELD_OK
#undef SET_FIELD
}

/*
 * [한국어]
 * nvme_ctrlr_create_io_qpair - IO qpair 객체 생성 (alloc_io_qpair의 내부 헬퍼)
 *
 * @ctrlr: 대상 controller
 * @opts:  qpair 옵션 (이미 정규화됨)
 * @return 새 qpair 포인터 또는 NULL.
 *
 * 호출 컨텍스트: spdk_nvme_ctrlr_alloc_io_qpair 내부에서만.
 *
 * 동작 5단계 (lock 보호):
 *   [1] qprio 비트 검증 — SPDK_NVME_CREATE_IO_SQ_QPRIO_MASK 외 비트 있으면 거부.
 *   [2] CC.AMS = RR(Round Robin)이면 qprio는 URGENT(0)만 유효 — RR은 우선순위 무시.
 *       AMS=WRR(Weighted RR) 또는 vendor-specific일 때만 다른 qprio 의미 있음.
 *   [3] qid 할당 — spdk_nvme_ctrlr_alloc_qid가 free_io_qids bitmap에서 다음 빈 비트 검색.
 *   [4] 트랜스포트별 create_io_qpair 호출 — SQ/CQ 메모리 할당, struct spdk_nvme_qpair 객체 생성.
 *   [5] active_io_qpairs 리스트에 추가 + process별 등록 (nvme_ctrlr_proc_add_io_qpair).
 *
 * 실패 시 qid 반환(spdk_nvme_ctrlr_free_qid)으로 leak 방지.
 *
 * 주의: 이 시점엔 qpair가 alloc만 됨. connect는 별도 호출 필요 (alloc_io_qpair의 다음 단계).
 */
static struct spdk_nvme_qpair *
nvme_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
			   const struct spdk_nvme_io_qpair_opts *opts)
{
	int32_t					qid;
	struct spdk_nvme_qpair			*qpair;
	union spdk_nvme_cc_register		cc;

	if (!ctrlr) {
		return NULL;
	}

	nvme_ctrlr_lock(ctrlr);
                                  /* [한국어] qid bitmap 조작 + active_io_qpairs 변경 race 방지 */
	cc.raw = ctrlr->process_init_cc.raw;
                                  /* [한국어] process_init에서 마지막으로 쓴 CC 값 캐시 — AMS 비트 검사용
                                   *  (실제 BAR을 매번 read 안 해도 됨, lock 보호 하 일관성 보장) */

	if (opts->qprio & ~SPDK_NVME_CREATE_IO_SQ_QPRIO_MASK) {
                                  /* [한국어] qprio는 2-bit (URGENT=0/HIGH=1/MEDIUM=2/LOW=3) — 그 외 비트는 invalid */
		nvme_ctrlr_unlock(ctrlr);
		return NULL;
	}

	/*
	 * Only value SPDK_NVME_QPRIO_URGENT(0) is valid for the
	 * default round robin arbitration method.
	 */
	if ((cc.bits.ams == SPDK_NVME_CC_AMS_RR) && (opts->qprio != SPDK_NVME_QPRIO_URGENT)) {
                                  /* [한국어] AMS=RR — 모든 큐가 동일 가중치, qprio 의미 없음. URGENT(0)만 허용 (스펙 강제). */
		NVME_CTRLR_ERRLOG(ctrlr, "invalid queue priority for default round robin arbitration method\n");
		nvme_ctrlr_unlock(ctrlr);
		return NULL;
	}

	qid = spdk_nvme_ctrlr_alloc_qid(ctrlr);
                                  /* [한국어] free_io_qids bitmap에서 다음 빈 비트 검색 후 set + 반환.
                                   *  qid는 1-based (admin은 qid=0 예약). */
	if (qid < 0) {
		nvme_ctrlr_unlock(ctrlr);
		return NULL;
                                  /* [한국어] 모든 IO qpair 슬롯 사용 중 — set_num_queues에서 협상된 max 도달 */
	}

	qpair = nvme_transport_ctrlr_create_io_qpair(ctrlr, qid, opts);
                                  /* [한국어] 트랜스포트별 qpair 객체 생성 — PCIe는 SQ/CQ DMA 메모리 할당 + tracker pool,
                                   *  RDMA는 ibv_create_qp + ibv_create_cq, TCP는 socket 준비. 아직 connect 안 함. */
	if (qpair == NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "nvme_transport_ctrlr_create_io_qpair() failed\n");
		spdk_nvme_ctrlr_free_qid(ctrlr, qid);
                                  /* [한국어] qpair create 실패 — qid 반환으로 leak 방지 */
		nvme_ctrlr_unlock(ctrlr);
		return NULL;
	}

	TAILQ_INSERT_TAIL(&ctrlr->active_io_qpairs, qpair, tailq);
                                  /* [한국어] ctrlr가 추적하는 모든 IO qpair 리스트에 추가 — destruct 시 강제 정리 대상 */

	nvme_ctrlr_proc_add_io_qpair(qpair);
                                  /* [한국어] 현 프로세스의 process 객체에 등록 — multi-process 안전성 */

	nvme_ctrlr_unlock(ctrlr);

	return qpair;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_connect_io_qpair - 이미 alloc된 IO qpair를 controller에 connect (외부 공개 API)
 *
 * @ctrlr: 대상 controller
 * @qpair: 연결할 qpair (alloc create_only 옵션으로 만들어진 경우 등)
 * @return: 0 성공, -EISCONN (이미 연결됨), 음수 errno (transport 실패)
 *
 * Connect 의미: NVMe-oF 의 Fabrics Connect 명령 발행, 또는 PCIe면 Create I/O SQ/CQ admin 발행.
 *
 * 동작:
 *   1) qpair state == DISCONNECTED 가 아니면 EISCONN 반환 (이미 연결됨 또는 dead)
 *   2) ctrlr_lock 보호 하에 transport ops connect 호출
 *   3) NVME_QUIRK_DELAY_AFTER_QUEUE_ALLOC: 일부 드라이브가 qpair create 후 즉시 IO 받으면 hang —
 *      100us 대기로 안정화.
 */
int
spdk_nvme_ctrlr_connect_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	/* [한국어] return code. */
	int rc;

	/* [한국어] qpair state 가드 — DISCONNECTED 만 connect 가능. */
	if (nvme_qpair_get_state(qpair) != NVME_QPAIR_DISCONNECTED) {
		return -EISCONN;
	}

	/* [한국어] ctrlr_lock 획득 — qpair 리스트 조작과 transport connect 명령의 atomicity. */
	nvme_ctrlr_lock(ctrlr);
	/* [한국어] transport별 connect 위임 — PCIe = Create I/O SQ/CQ admin, Fabrics = Connect 명령. */
	rc = nvme_transport_ctrlr_connect_qpair(ctrlr, qpair);
	/* [한국어] lock 해제. */
	nvme_ctrlr_unlock(ctrlr);

	/* [한국어] quirk 적용 — 일부 드라이브의 race 회피 100us delay. */
	if (ctrlr->quirks & NVME_QUIRK_DELAY_AFTER_QUEUE_ALLOC) {
		/* [한국어] busy-wait 100us — 짧은 시간이라 nanosleep 대신 spin. */
		spdk_delay_us(100);
	}

	/* [한국어] connect 결과 그대로 caller에게. */
	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_disconnect_io_qpair - IO qpair disconnect (free 전 단계)
 *
 * @qpair: disconnect할 qpair
 *
 * 단순 wrapper — ctrlr_lock 보호 하에 transport ops disconnect 호출.
 * Disconnect 후 qpair 객체는 free 가능 상태가 됨.
 */
void
spdk_nvme_ctrlr_disconnect_io_qpair(struct spdk_nvme_qpair *qpair)
{
	/* [한국어] qpair 소속 ctrlr 캐시. */
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;

	/* [한국어] lock 보호 하에 disconnect — transport별 처리 (Delete IO SQ/CQ admin 등). */
	nvme_ctrlr_lock(ctrlr);
	nvme_transport_ctrlr_disconnect_qpair(ctrlr, qpair);
	nvme_ctrlr_unlock(ctrlr);
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_admin_qp_fd - admin qpair의 epoll-able fd 반환 (interrupt mode 지원)
 *
 * @ctrlr: 대상 controller
 * @opts: epoll handler 옵션 출력
 * @return: fd 또는 음수 errno
 *
 * Interrupt-driven completion notification 지원하는 transport(VFIO-USER 등)에서 사용 가능.
 * 일반 polling 모드에선 -ENOSYS.
 */
int
spdk_nvme_ctrlr_get_admin_qp_fd(struct spdk_nvme_ctrlr *ctrlr,
				struct spdk_event_handler_opts *opts)
{
	/* [한국어] admin qpair에 위임 — qpair 단위로 fd 노출. */
	return spdk_nvme_qpair_get_fd(ctrlr->adminq, opts);
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_alloc_io_qpair - ★ IO qpair 할당 + 자동 connect (사용자 노출 API)
 *
 * @ctrlr:     대상 controller (READY 상태 필수)
 * @user_opts: 사용자 opts (NULL이면 기본값만)
 * @opts_size: 사용자가 알고 있는 opts 크기
 * @return     새 qpair 또는 NULL.
 *
 * 사용자가 IO를 시작하기 위해 가장 먼저 호출하는 API. 일반 사용 패턴:
 *   spdk_nvme_io_qpair_opts opts;
 *   spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
 *   opts.qprio = HIGH;  // 일부 필드만 덮어쓰기
 *   qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
 *   spdk_nvme_ns_cmd_read(ns, qpair, ...);
 *
 * 동작 8단계 (lock 보호):
 *   [1] state != READY 이면 거부 (resetting/initializing 중에는 free_io_qids bitmap 없음)
 *   [2] 기본 opts 채움 + 사용자 opts ABI-safe overlay
 *   [3] 사용자가 sq.vaddr/cq.vaddr 제공 시 buffer_size가 io_queue_size 충분한지 검증
 *   [4] interrupt + delay_cmd_submit 충돌 검증 (둘 다 켜면 의미 없음)
 *   [5] nvme_ctrlr_create_io_qpair — qid 할당 + transport create + active_io_qpairs 추가
 *   [6] create_only=true면 alloc만 하고 return (사용자가 connect 별도 호출)
 *   [7] spdk_nvme_ctrlr_connect_io_qpair — 자동 connect
 *   [8] connect 실패 시 cleanup 4단계 — proc_remove + active_io_qpairs remove + qid bitmap 반환 + transport delete
 */
struct spdk_nvme_qpair *
spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
			       const struct spdk_nvme_io_qpair_opts *user_opts,
			       size_t opts_size)
{

	struct spdk_nvme_qpair		*qpair = NULL;
	struct spdk_nvme_io_qpair_opts	opts;
	int				rc;

	nvme_ctrlr_lock(ctrlr);

	if (spdk_unlikely(ctrlr->state != NVME_CTRLR_STATE_READY)) {
		/* When controller is resetting or initializing, free_io_qids is deleted or not created yet.
		 * We can't create IO qpair in that case */
                                  /* [한국어] reset 중 또는 초기화 중 — qid bitmap이 없거나 무효화됨. 사용자는 reset 완료까지 대기 필요. */
		goto unlock;
	}

	/*
	 * Get the default options, then overwrite them with the user-provided options
	 * up to opts_size.
	 *
	 * This allows for extensions of the opts structure without breaking
	 * ABI compatibility.
	 */
	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
                                  /* [한국어] 기본값으로 채움 — 라이브러리가 알고 있는 모든 필드 */
	if (user_opts) {
		nvme_ctrlr_io_qpair_opts_copy(&opts, user_opts, spdk_min(opts.opts_size, opts_size));
                                  /* [한국어] 사용자 값으로 overlay — opts_size 작은 쪽 기준으로 ABI 안전 */

		/* If user passes buffers, make sure they're big enough for the requested queue size */
		if (opts.sq.vaddr) {
                                  /* [한국어] 사용자 SQ 버퍼 제공 (예: CMB 활용) — 충분히 큰지 검증 */
			if (opts.sq.buffer_size < (opts.io_queue_size * sizeof(struct spdk_nvme_cmd))) {
                                  /* [한국어] SQ entry 64B × queue_size 가 buffer_size보다 크면 overflow → 거부 */
				NVME_CTRLR_ERRLOG(ctrlr, "sq buffer size %" PRIx64 " is too small for sq size %zx\n",
						  opts.sq.buffer_size, (opts.io_queue_size * sizeof(struct spdk_nvme_cmd)));
				goto unlock;
			}
		}
		if (opts.cq.vaddr) {
                                  /* [한국어] 사용자 CQ 버퍼 — 마찬가지 검증 (CQ entry는 16B) */
			if (opts.cq.buffer_size < (opts.io_queue_size * sizeof(struct spdk_nvme_cpl))) {
				NVME_CTRLR_ERRLOG(ctrlr, "cq buffer size %" PRIx64 " is too small for cq size %zx\n",
						  opts.cq.buffer_size, (opts.io_queue_size * sizeof(struct spdk_nvme_cpl)));
				goto unlock;
			}
		}
	}

	if (ctrlr->opts.enable_interrupts && opts.delay_cmd_submit) {
                                  /* [한국어] interrupt 모드 + delay_cmd_submit 충돌 — interrupt는 즉시 doorbell 필요, batching과 의미 충돌 */
		NVME_CTRLR_ERRLOG(ctrlr, "delay command submit cannot work with interrupts\n");
		goto unlock;
	}

	qpair = nvme_ctrlr_create_io_qpair(ctrlr, &opts);
                                  /* [한국어] qid 할당 + transport create — 위에서 정의한 헬퍼 */

	if (qpair == NULL || opts.create_only == true) {
                                  /* [한국어] create 실패(NULL) 또는 사용자가 자동 connect 거부 — 여기서 종료.
                                   *  create_only면 사용자가 spdk_nvme_ctrlr_connect_io_qpair 별도 호출. */
		goto unlock;
	}

	rc = spdk_nvme_ctrlr_connect_io_qpair(ctrlr, qpair);
                                  /* [한국어] 자동 connect — Fabrics Connect 명령 또는 PCIe Create IO SQ/CQ admin */
	if (rc != 0) {
                                  /* [한국어] connect 실패 — 4단계 cleanup으로 모든 자원 회수 */
		NVME_CTRLR_ERRLOG(ctrlr, "nvme_transport_ctrlr_connect_io_qpair() failed\n");
		nvme_ctrlr_proc_remove_io_qpair(qpair);
                                  /* [한국어] 1) process 객체에서 제거 */
		TAILQ_REMOVE(&ctrlr->active_io_qpairs, qpair, tailq);
                                  /* [한국어] 2) ctrlr active 리스트에서 제거 */
		spdk_bit_array_set(ctrlr->free_io_qids, qpair->id);
                                  /* [한국어] 3) qid bitmap에서 다시 free 마킹 (set bit = available, clear bit = in-use) */
		nvme_transport_ctrlr_delete_io_qpair(ctrlr, qpair);
                                  /* [한국어] 4) transport별 qpair 객체 free (메모리 + 큐 자원 반환) */
		qpair = NULL;
		goto unlock;
	}

unlock:
	nvme_ctrlr_unlock(ctrlr);

	return qpair;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_reconnect_io_qpair - 끊긴 IO qpair 재연결 (사용자 노출 API)
 *
 * @qpair: disconnect된 qpair (transport 에러 등으로 끊김)
 * @return 0 성공, -ENODEV(controller 제거됨), -EAGAIN(아직 reset 중 또는 disconnect 진행 중),
 *         -ENXIO(controller failed 또는 destroying 상태).
 *
 * 사용처: NVMe-oF에서 link 끊김 후 재연결 시도, fabric reset 후 회복 등.
 *
 * 동작 (lock 보호):
 *   상태 검증 4단계 → 트랜스포트 connect 호출.
 *   각 음수 반환은 사용자가 다른 처리 (재시도/폐기/대기) 결정에 사용.
 */
int
spdk_nvme_ctrlr_reconnect_io_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr;
	enum nvme_qpair_state qpair_state;
	int rc;

	assert(qpair != NULL);
	assert(nvme_qpair_is_admin_queue(qpair) == false);
                                  /* [한국어] admin qpair는 reconnect API 대상 아님 (controller reset이 처리) */
	assert(qpair->ctrlr != NULL);

	ctrlr = qpair->ctrlr;
	nvme_ctrlr_lock(ctrlr);
	qpair_state = nvme_qpair_get_state(qpair);

	if (ctrlr->is_removed) {
		rc = -ENODEV;
		goto out;
                                  /* [한국어] hot-removal 등으로 device 자체가 사라짐 — 재연결 영구 불가능 */
	}

	if (ctrlr->is_resetting || qpair_state == NVME_QPAIR_DISCONNECTING) {
		rc = -EAGAIN;
		goto out;
                                  /* [한국어] reset 진행 중 또는 disconnect 진행 중 — 잠시 후 재시도 */
	}

	if (ctrlr->is_failed || qpair_state == NVME_QPAIR_DESTROYING) {
		rc = -ENXIO;
		goto out;
                                  /* [한국어] controller fail 상태 또는 qpair destroy 진행 중 — 회복 불가 */
	}

	if (qpair_state != NVME_QPAIR_DISCONNECTED) {
		rc = 0;
		goto out;
                                  /* [한국어] 이미 connected 또는 connecting — 사용자에게 OK 신호 (idempotent) */
	}

	rc = nvme_transport_ctrlr_connect_qpair(ctrlr, qpair);
                                  /* [한국어] 트랜스포트 reconnect — Fabrics는 새 Connect 명령, PCIe는 Create IO SQ/CQ 재발행 */
	if (rc) {
		rc = -EAGAIN;
                                  /* [한국어] 일시적 실패 — 사용자 재시도 가능 신호로 -EAGAIN 정규화 */
		goto out;
	}

out:
	nvme_ctrlr_unlock(ctrlr);
	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_admin_qp_failure_reason - admin qpair의 실패 사유 조회 (사용자 노출 API)
 *
 * @return spdk_nvme_qp_failure_reason enum (NONE/REMOVED/ABORTED 등).
 *
 * 사용처: process_admin_completions가 -EIO 반환 시 원인 진단.
 */
spdk_nvme_qp_failure_reason
spdk_nvme_ctrlr_get_admin_qp_failure_reason(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->adminq->transport_failure_reason;
                                  /* [한국어] 단순 필드 반환 — admin qpair에 트랜스포트가 마지막 실패 사유 저장 */
}

/*
 * This internal function will attempt to take the controller
 * lock before calling disconnect on a controller qpair.
 * Functions already holding the controller lock should
 * call nvme_transport_ctrlr_disconnect_qpair directly.
 */
/*
 * [한국어]
 * nvme_ctrlr_disconnect_qpair - lock 자동 획득 후 transport disconnect (내부 wrapper)
 *
 * @qpair: disconnect할 qpair
 *
 * vs nvme_transport_ctrlr_disconnect_qpair: 이 함수는 lock 자동 획득. 이미 lock 보유한 호출자는
 * transport 함수를 직접 호출 (lock 재진입 회피 — 비록 recursive lock이긴 해도).
 *
 * 사용처: lock 잡지 않은 외부에서 안전하게 disconnect 호출 시.
 */
void
nvme_ctrlr_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;

	assert(ctrlr != NULL);
	nvme_ctrlr_lock(ctrlr);
	nvme_transport_ctrlr_disconnect_qpair(ctrlr, qpair);
                                  /* [한국어] 트랜스포트별 disconnect — Delete IO SQ/CQ admin (PCIe) 또는 Fabric Disconnect */
	nvme_ctrlr_unlock(ctrlr);
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_free_io_qpair - ★ IO qpair free + 자동 disconnect (사용자 노출 API)
 *
 * @qpair: free할 qpair (NULL 허용 — noop)
 * @return 0 (항상 0 — 에러는 로그만, free 자체는 실패해도 사용자에게 알릴 의미 없음).
 *
 * 사용 시점: 사용자가 더 이상 이 qpair로 IO 안 보낼 때. process_completions 안에서 자기 자신 free 가능.
 *
 * 동작 7단계:
 *   [1] NULL 가드 — noop.
 *   [2] in_completion_context 검사 — 사용자가 process_completions cb_fn 안에서 free 호출 시
 *       즉시 free 못 함 (cb_fn이 qpair 사용 중). delete_after_completion_context=1 마킹 후 return →
 *       process_completions가 callback 끝나고 이 플래그 보고 free 진행.
 *   [3] transport disconnect 호출.
 *   [4] async qpair는 disconnect가 즉시 완료 안 될 수 있음 → DISCONNECTED 상태까지 process_completions 폴링.
 *       (poll_group_remove가 DISCONNECTED 상태 요구하므로 leak 방지).
 *   [5] DISCONNECTED 도달 검증. 실패 시 로그만 + 0 반환 (다른 프로세스 정리 케이스 대비).
 *   [6] poll_group 등록되어 있고 같은 프로세스가 만든 qpair면 poll_group_remove.
 *       (다른 프로세스가 만든 qpair는 그 프로세스의 poll_group에 등록됨 → 우리가 만질 수 없음)
 *   [7] DESTROYING 상태로 set + (같은 프로세스 qpair면) queued_reqs abort + 4단계 cleanup
 *       (proc_remove + active_io_qpairs remove + qid bitmap 반환 + transport delete).
 *
 * Multi-process 안전성: foreign qpair (다른 프로세스가 만든 것)는 abort/poll_group_remove 스킵 —
 *                      callback도 그 프로세스 컨텍스트라 우리가 호출하면 안 됨.
 */
int
spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr;
	int rc;

	if (qpair == NULL) {
		return 0;
                                  /* [한국어] NULL 안전 — 사용자가 alloc 실패한 NULL을 그대로 free 호출해도 안전 */
	}

	ctrlr = qpair->ctrlr;

	if (qpair->in_completion_context) {
		/*
		 * There are many cases where it is convenient to delete an io qpair in the context
		 *  of that qpair's completion routine.  To handle this properly, set a flag here
		 *  so that the completion routine will perform an actual delete after the context
		 *  unwinds.
		 */
		qpair->delete_after_completion_context = 1;
                                  /* [한국어] ★ in-completion 자기 free 패턴 — 즉시 free 못 함 (cb_fn이 qpair 사용 중).
                                   *  process_completions가 callback 끝나고 이 플래그 검사 → 실제 free 진행. */
		return 0;
	}

	nvme_transport_ctrlr_disconnect_qpair(ctrlr, qpair);
                                  /* [한국어] 트랜스포트 disconnect 시작 — sync 또는 async (트랜스포트별) */

	/* For async qpairs, the disconnect may not complete immediately. Poll until the qpair
	 * reaches the DISCONNECTED state to ensure the poll group can be removed without error.
	 * This prevents resource leaks when spdk_nvme_poll_group_remove() checks the qpair state.
	 */
	while (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
		spdk_nvme_qpair_process_completions(qpair, 0);
                                  /* [한국어] async disconnect 진행 폴링 — DISCONNECTING → DISCONNECTED 전이 대기 */
	}

	if (nvme_qpair_get_state(qpair) != NVME_QPAIR_DISCONNECTED) {
                                  /* [한국어] DISCONNECTED 도달 못 함 — 다른 프로세스가 정리 중이거나 비정상 상태 */
		NVME_CTRLR_ERRLOG(ctrlr, "qpair is not in DISCONNECTED state: state=%d\n",
				  nvme_qpair_get_state(qpair));
		return 0;
                                  /* [한국어] 0 반환 — 사용자에게 에러 알릴 의미 없음 (이미 정리됨) */
	}

	if (qpair->poll_group && (qpair->active_proc == nvme_ctrlr_get_current_process(ctrlr))) {
                                  /* [한국어] poll_group 등록 + 같은 프로세스가 만든 qpair만 — foreign qpair는 다른 프로세스의 poll_group 소유 */
		rc = spdk_nvme_poll_group_remove(qpair->poll_group->group, qpair);
		if (rc != 0) {
                                  /* [한국어] poll_group_remove 실패 — 매우 드물지만 로그만 (free는 계속) */
			NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_poll_group_remove() failed: rc=%s\n",
					  spdk_strerror(abs(rc)));
			return 0;
		}
	}

	/* Do not retry. */
	nvme_qpair_set_state(qpair, NVME_QPAIR_DESTROYING);
                                  /* [한국어] DESTROYING 마킹 — 이후 어떤 작업도 거부 (재시도 무의미) */

	/* In the multi-process case, a process may call this function on a foreign
	 * I/O qpair (i.e. one that this process did not create) when that qpairs process
	 * exits unexpectedly.  In that case, we must not try to abort any reqs associated
	 * with that qpair, since the callbacks will also be foreign to this process.
	 */
	if (qpair->active_proc == nvme_ctrlr_get_current_process(ctrlr)) {
                                  /* [한국어] 우리 프로세스가 만든 qpair만 abort — foreign callback 호출 금지 (UAF 위험) */
		nvme_qpair_abort_all_queued_reqs(qpair);
                                  /* [한국어] queued_reqs 모두 cancel + cb_fn(SCT_GENERIC, SC_ABORTED_SQ_DELETION) 호출 */
	}

	nvme_ctrlr_lock(ctrlr);

	nvme_ctrlr_proc_remove_io_qpair(qpair);
                                  /* [한국어] 1) process 객체에서 제거 */

	TAILQ_REMOVE(&ctrlr->active_io_qpairs, qpair, tailq);
                                  /* [한국어] 2) ctrlr active 리스트에서 제거 */
	spdk_nvme_ctrlr_free_qid(ctrlr, qpair->id);
                                  /* [한국어] 3) qid bitmap에 set bit (다시 free 마킹) */

	nvme_transport_ctrlr_delete_io_qpair(ctrlr, qpair);
                                  /* [한국어] 4) transport별 qpair 객체 free (DMA 메모리 + 큐 자원 반환) */
	nvme_ctrlr_unlock(ctrlr);
	return 0;
}

static void
nvme_ctrlr_construct_intel_support_log_page_list(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_intel_log_page_directory *log_page_directory)
{
	if (log_page_directory == NULL) {
		return;
	}

	assert(ctrlr->cdata.vid == SPDK_PCI_VID_INTEL);

	ctrlr->log_page_supported[SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY] = true;

	if (log_page_directory->read_latency_log_len ||
	    (ctrlr->quirks & NVME_INTEL_QUIRK_READ_LATENCY)) {
		ctrlr->log_page_supported[SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY] = true;
	}
	if (log_page_directory->write_latency_log_len ||
	    (ctrlr->quirks & NVME_INTEL_QUIRK_WRITE_LATENCY)) {
		ctrlr->log_page_supported[SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY] = true;
	}
	if (log_page_directory->temperature_statistics_log_len) {
		ctrlr->log_page_supported[SPDK_NVME_INTEL_LOG_TEMPERATURE] = true;
	}
	if (log_page_directory->smart_log_len) {
		ctrlr->log_page_supported[SPDK_NVME_INTEL_LOG_SMART] = true;
	}
	if (log_page_directory->marketing_description_log_len) {
		ctrlr->log_page_supported[SPDK_NVME_INTEL_MARKETING_DESCRIPTION] = true;
	}
}

/*
 * [한국어] struct intel_log_pages_ctx - Intel log page directory get 비동기 컨텍스트
 *
 * nvme_ctrlr_set_intel_support_log_pages 가 Intel vendor-specific log page directory(0xC0)
 * read 발행 시 heap 할당하여 cb_arg 로 전달. 완료 콜백 done 에서 사용 후 free.
 */
struct intel_log_pages_ctx {
	struct spdk_nvme_intel_log_page_directory log_page_directory;
	/* [한국어] Intel log page directory 응답 버퍼 (DMA 대상).
	 *  - 설정자: 디바이스가 DMA 로 채움 (admin get_log_page 완료 시점).
	 *  - 읽는 자: nvme_ctrlr_construct_intel_support_log_page_list — 각 length 필드 검사 후
	 *    해당 log page 의 log_page_supported[] 비트 set.
	 *  - 값 범위: 각 log_len 필드는 해당 Intel log page 의 실제 크기 (0=미지원).
	 *  - 동기화: 단일 admin completion thread 만 접근 — 별도 락 불요. */

	struct spdk_nvme_ctrlr *ctrlr;
	/* [한국어] 콜백에서 사용할 ctrlr 역참조.
	 *  - 설정자: nvme_ctrlr_set_intel_support_log_pages 에서 set.
	 *  - 읽는 자: done 콜백 — log_page_supported[] 비트맵 변경, 상태 전이 트리거.
	 *  - 값 범위: 유효 ctrlr 포인터 (NULL 불가).
	 *  - 동기화: ctx 수명 동안 ctrlr 가 destruct 되지 않음을 보장 (reset/destruct 가 ctx 정리 대기). */
};

/*
 * [한국어]
 * nvme_ctrlr_set_intel_support_log_pages_done - Intel log page directory get 완료 콜백
 *
 * @arg: intel_log_pages_ctx — log_page_directory 응답 + ctrlr 참조
 * @cpl: 명령 완료 정보
 *
 * 동작:
 *   - 성공이면 directory 분석하여 log_page_supported[] 비트맵의 Intel 항목 set.
 *   - 실패해도 진행 — Intel log page 는 옵션 (스펙 외 벤더 확장).
 *   - 다음 상태 SET_SUPPORTED_FEATURES 로 전이.
 *   - ctx free.
 *
 * 호출 체인:
 *   nvme_ctrlr_set_intel_support_log_pages (admin 발행) → admin 완료 → [본 콜백]
 */
static void
nvme_ctrlr_set_intel_support_log_pages_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct intel_log_pages_ctx *ctx = arg;
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;

	if (!spdk_nvme_cpl_is_error(cpl)) {
		nvme_ctrlr_construct_intel_support_log_page_list(ctrlr, &ctx->log_page_directory);
		                                  /* [한국어] directory 응답에서 사용 가능한 log page 비트 set. */
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES,
			     ctrlr->opts.admin_timeout_ms);
	                                  /* [한국어] Intel log page 실패해도 다음 단계 진행. */
	free(ctx);                    /* [한국어] heap 할당 ctx 해제 — directory buffer 포함. */
}

/*
 * [한국어]
 * nvme_ctrlr_set_intel_support_log_pages - Intel 벤더 log page 지원 여부 비동기 조회
 *
 * @ctrlr: 대상 컨트롤러 (Identify 에서 VID=Intel 확인됨)
 * @return 0 (실패해도 0 — 다음 상태로 진행 보장)
 *
 * Intel SSD 들이 노출하는 벤더-specific log page들(Read/Write Latency, Marketing
 * Description, SMART Attributes 등) 의 지원 여부를 controller 의 log page directory(0xC0)
 * 를 read 하여 확인하고 log_page_supported[] 비트맵에 반영.
 *
 * 비동기 모델: get_log_page admin 발행 → 완료 콜백이 directory 분석 + 다음 상태 전이.
 *
 * 호출 체인 (process_init 상태머신):
 *   nvme_ctrlr_process_init (SET_SUPPORTED_INTEL_LOG_PAGES) → [본 함수]
 *     → spdk_nvme_ctrlr_cmd_get_log_page (Intel directory)
 *     → nvme_ctrlr_set_intel_support_log_pages_done
 *     → 다음 상태 SET_SUPPORTED_FEATURES
 */
static int
nvme_ctrlr_set_intel_support_log_pages(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	struct intel_log_pages_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	ctx->ctrlr = ctrlr;

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY,
					      SPDK_NVME_GLOBAL_NS_TAG, &ctx->log_page_directory,
					      sizeof(struct spdk_nvme_intel_log_page_directory),
					      0, nvme_ctrlr_set_intel_support_log_pages_done, ctx);
	if (rc != 0) {
		free(ctx);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_SUPPORTED_INTEL_LOG_PAGES,
			     ctrlr->opts.admin_timeout_ms);

	return 0;
}

/*
 * [한국어]
 * nvme_ctrlr_alloc_ana_log_page - ANA(Asymmetric Namespace Access) log page buffer 할당/재할당
 *
 * @ctrlr: 대상 컨트롤러 (NVMe-oF multipath 대상)
 * @return 0 성공, -ENXIO / -ENOMEM 실패
 *
 * ANA(NVMe spec §8.20): 같은 subsystem 의 여러 컨트롤러가 동일 namespace 를 공유할 때, 각
 * 컨트롤러를 통한 path 가 "optimized / non-optimized / inaccessible" 등 상태를 가지므로,
 * 호스트가 multipath 결정에 사용. ANA log page (LID=0x0C) 로 이 상태를 조회.
 *
 * log page 크기는 동적: 헤더 + (NANAGRPID × 그룹 디스크립터) + (active_ns_count × NSID 배열).
 * 활성 NS 수가 변경되면(NS Attribute Change AER) 버퍼도 재할당 필요. 본 함수가 이 동작을 캡슐화.
 *
 * 동작:
 *   1) 필요 크기 계산.
 *   2) 기존 버퍼보다 크면 realloc (기존 NULL 이면 calloc).
 *   3) copied_ana_desc(파싱 임시 버퍼) 도 동일 크기로 재할당.
 *   4) ctrlr->ana_log_page_size 업데이트.
 *
 * 호출 체인:
 *   nvme_ctrlr_update_ana_log_page → [본 함수] → realloc/calloc
 */
static int
nvme_ctrlr_alloc_ana_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t ana_log_page_size;

	ana_log_page_size = sizeof(struct spdk_nvme_ana_page) + ctrlr->cdata.nanagrpid *
			    sizeof(struct spdk_nvme_ana_group_descriptor) + ctrlr->active_ns_count *
			    sizeof(uint32_t);

	/* Number of active namespaces may have changed.
	 * Check if ANA log page fits into existing buffer.
	 */
	if (ana_log_page_size > ctrlr->ana_log_page_size) {
		void *new_buffer;

		if (ctrlr->ana_log_page) {
			new_buffer = realloc(ctrlr->ana_log_page, ana_log_page_size);
		} else {
			new_buffer = calloc(1, ana_log_page_size);
		}

		if (!new_buffer) {
			NVME_CTRLR_ERRLOG(ctrlr, "could not allocate ANA log page buffer, size %u\n",
					  ana_log_page_size);
			return -ENXIO;
		}

		ctrlr->ana_log_page = new_buffer;
		if (ctrlr->copied_ana_desc) {
			new_buffer = realloc(ctrlr->copied_ana_desc, ana_log_page_size);
		} else {
			new_buffer = calloc(1, ana_log_page_size);
		}

		if (!new_buffer) {
			NVME_CTRLR_ERRLOG(ctrlr, "could not allocate a buffer to parse ANA descriptor, size %u\n",
					  ana_log_page_size);
			return -ENOMEM;
		}

		ctrlr->copied_ana_desc = new_buffer;
		ctrlr->ana_log_page_size = ana_log_page_size;
	}

	return 0;
}

/*
 * [한국어]
 * nvme_ctrlr_update_ana_log_page - ANA log page 동기 read + 내부 캐시 갱신
 *
 * @ctrlr: 대상 컨트롤러
 * @return 0 성공, 음수 errno
 *
 * 동작:
 *   1) buffer 크기 보장 (alloc_ana_log_page).
 *   2) Get Log Page (LID=0x0C, NSID=0xFFFFFFFF global) admin 명령 발행.
 *   3) 동기 polling 완료 대기.
 *   4) ctrlr->ana_log_page 에 raw 데이터 저장 — 이후 parse_ana_log_page 가 사용.
 *
 * 호출 시점:
 *   - Controller bring-up 후 1회 (initial state)
 *   - ANA Change AER 수신 시 (path state 변경)
 *   - 사용자 수동 호출 (drift 감지)
 */
static int
nvme_ctrlr_update_ana_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_completion_poll_status *status;
	int rc;

	rc = nvme_ctrlr_alloc_ana_log_page(ctrlr);
	if (rc != 0) {
		return rc;
	}

	status = calloc(1, sizeof(*status));
	if (status == NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS,
					      SPDK_NVME_GLOBAL_NS_TAG, ctrlr->ana_log_page,
					      ctrlr->ana_log_page_size, 0,
					      nvme_completion_poll_cb, status);
	if (rc != 0) {
		free(status);
		return rc;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for spdk_nvme_ctrlr_cmd_get_log_page failed: rc=%s\n",
				  spdk_strerror(abs(rc)));
	}

	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_update_ns_ana_states - ANA group descriptor 한 개를 NS 객체들에 반영
 *
 * @desc: ANA group descriptor (한 그룹의 ana_state + 포함된 NSID 리스트)
 * @cb_arg: spdk_nvme_ctrlr * (parse 호출자가 전달)
 * @return 0 (parse 계속 진행)
 *
 * parse_ana_log_page 의 per-descriptor 콜백으로 등록되어 호출됨. 한 group descriptor 의
 * ana_group_id / ana_state 를 그 그룹에 속한 모든 NS 객체에 복사한다.
 *
 * 동작:
 *   - 각 NSID 에 대해 spdk_nvme_ctrlr_get_ns (lazy alloc) 로 NS 객체 확보.
 *   - ns->ana_group_id, ns->ana_state 갱신.
 *
 * NSID 검증: 0 또는 NN 초과는 skip — 잘못된 log page 데이터로부터 보호.
 *
 * 호출 체인:
 *   nvme_ctrlr_parse_ana_log_page (각 그룹 디스크립터 순회) → [본 함수]
 */
static int
nvme_ctrlr_update_ns_ana_states(const struct spdk_nvme_ana_group_descriptor *desc,
				void *cb_arg)
{
	struct spdk_nvme_ctrlr *ctrlr = cb_arg;
	                                  /* [한국어] cb_arg 를 ctrlr 로 캐스팅 — 등록 시 self 전달 규약. */
	struct spdk_nvme_ns *ns;
	uint32_t i, nsid;

	for (i = 0; i < desc->num_of_nsid; i++) {
	                                  /* [한국어] 이 그룹에 속한 NSID 개수만큼 순회. */
		nsid = desc->nsid[i];
		if (nsid == 0 || nsid > ctrlr->cdata.nn) {
		                                  /* [한국어] 비정상 NSID — 무시하고 다음으로. (firmware bug 방어) */
			continue;
		}

		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		                                  /* [한국어] NS 객체 lazy alloc 또는 기존 발견. */
		assert(ns != NULL);       /* [한국어] NSID 범위 check 후라 NULL 비정상. */

		ns->ana_group_id = desc->ana_group_id;
		                                  /* [한국어] 그룹 식별자 — multipath 정책 키. */
		ns->ana_state = desc->ana_state;
		                                  /* [한국어] OPTIMIZED/NON_OPTIMIZED/INACCESSIBLE/PERSISTENT_LOSS/CHANGE. */
	}

	return 0;                     /* [한국어] 0 = parse 계속 (모든 그룹 순회). */
}

/*
 * [한국어]
 * nvme_ctrlr_parse_ana_log_page - ANA log page raw 데이터를 순회하며 사용자 콜백 호출 (내부 API)
 *
 * @ctrlr: ANA log page 가 캐시된 컨트롤러
 * @cb_fn: 각 그룹 디스크립터마다 호출될 콜백 (return < 0 면 순회 중단)
 * @cb_arg: 콜백 컨텍스트
 * @return 0 성공, 음수 errno
 *
 * 동작:
 *   1) ana_log_page 헤더 분석 (num_ana_group_desc)
 *   2) 각 그룹 디스크립터를 copied_ana_desc 로 복사 (raw 가 packed 라 직접 접근 위험 → align 보장)
 *   3) cb_fn(desc, cb_arg) 호출 — 사용자가 NS 상태 갱신 등 수행
 *   4) desc->num_of_nsid 만큼 다음 디스크립터 위치로 전진
 *
 * 사용처: 사용자가 ANA 상태 변화를 인지하고 multipath 정책을 동적으로 조정할 때.
 * SPDK 내부에서 nvme_ctrlr_update_ns_ana_states 가 등록됨.
 */
int
nvme_ctrlr_parse_ana_log_page(struct spdk_nvme_ctrlr *ctrlr,
			      spdk_nvme_parse_ana_log_page_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_ana_group_descriptor *copied_desc;
	uint8_t *orig_desc;
	uint32_t i, desc_size, copy_len;
	int rc = 0;

	if (ctrlr->ana_log_page == NULL) {
		return -EINVAL;
	}

	copied_desc = ctrlr->copied_ana_desc;

	orig_desc = (uint8_t *)ctrlr->ana_log_page + sizeof(struct spdk_nvme_ana_page);
	copy_len = ctrlr->ana_log_page_size - sizeof(struct spdk_nvme_ana_page);

	for (i = 0; i < ctrlr->ana_log_page->num_ana_group_desc; i++) {
		memcpy(copied_desc, orig_desc, copy_len);

		rc = cb_fn(copied_desc, cb_arg);
		if (rc != 0) {
			break;
		}

		desc_size = sizeof(struct spdk_nvme_ana_group_descriptor) +
			    copied_desc->num_of_nsid * sizeof(uint32_t);
		orig_desc += desc_size;
		copy_len -= desc_size;
	}

	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_set_supported_log_pages - 컨트롤러가 지원하는 Log Page 목록 결정
 *
 * @ctrlr: 대상 NVMe 컨트롤러 구조체
 * @return: 성공 시 0, ANA log page 업데이트 실패 시 음수
 *
 * 동기/배경:
 *   사용자가 spdk_nvme_ctrlr_get_log_page() 같은 API 로 특정 log page를 조회하려고 할 때,
 *   NVMe 컨트롤러가 *실제로 지원* 하는지 사전에 알아야 무용한 admin 명령이 실패하지 않는다.
 *   이 함수는 controller bring-up 상태머신 의 한 단계로, identify ctrlr 결과(ctrlr->cdata) 의
 *   비트 필드들을 확인해 log_page_supported[] 비트맵을 구성한다.
 *
 * 동작 단계:
 *   1) 비트맵 0으로 초기화
 *   2) NVMe 스펙 mandatory log page 3종 무조건 활성화 (Error, Health, Firmware Slot)
 *   3) Identify Ctrlr 의 LPA(Log Page Attributes).cses 비트 확인 → Command Effects Log
 *   4) CMIC.anars (ANA reporting support) → ANA log page + (옵션 아니면) 즉시 read+parse
 *   5) ctratt.fdps (Flexible Data Placement) → FDP 관련 4종 log page
 *   6) Vendor=Intel + PCIe + quirk 미지정 → Intel vendor log pages 단계로 진행
 *      그 외 → SET_SUPPORTED_FEATURES 단계로 직접 진행
 *
 * 실행 컨텍스트:
 *   nvme_ctrlr_process_init() state machine 안에서 호출되는 admin worker.
 *   호출 직전 상태: NVMe_CTRLR_STATE_SET_SUPPORTED_LOG_PAGES.
 *   동기 호출이지만 ANA log page 가 발생하면 그 안에서 admin queue polling 으로 동기 대기.
 *
 * 호출 체인:
 *   nvme_ctrlr_process_init → [이 함수] → nvme_ctrlr_set_state(다음 상태)
 */
static int
nvme_ctrlr_set_supported_log_pages(struct spdk_nvme_ctrlr *ctrlr)
{
	int	rc = 0;                                                 /* [한국어] 반환 코드 — ANA log page 업데이트가 실패할 수 있어 추적 */

	/* [한국어] log_page_supported 비트맵을 모두 0으로 초기화 — 이후 mandatory + 옵션 비트들을 켬 */
	memset(ctrlr->log_page_supported, 0, sizeof(ctrlr->log_page_supported));
	/* Mandatory pages */
	/* [한국어] NVMe 스펙 1.x Section 5.16 — 모든 컨트롤러가 의무적으로 지원해야 하는 3종 log page */
	ctrlr->log_page_supported[SPDK_NVME_LOG_ERROR] = true;          /* [한국어] Error Information (LID=01h) — 펌웨어가 보고하는 에러 큐 */
	ctrlr->log_page_supported[SPDK_NVME_LOG_HEALTH_INFORMATION] = true; /* [한국어] SMART/Health (LID=02h) — 온도, 마모도, capacity 등 */
	ctrlr->log_page_supported[SPDK_NVME_LOG_FIRMWARE_SLOT] = true;  /* [한국어] Firmware Slot Info (LID=03h) — 슬롯별 펌웨어 버전 */
	/* [한국어] Identify Ctrlr LPA(Log Page Attributes).cses 비트 — Command Effects 지원 여부 */
	if (ctrlr->cdata.lpa.cses) {
		ctrlr->log_page_supported[SPDK_NVME_LOG_COMMAND_EFFECTS_LOG] = true; /* [한국어] LID=05h — admin 명령별 부수 효과(데이터 변경/제출 큐 영향 등) */
	}

	/* [한국어] CMIC(Controller Multi-path & I/O Sharing).anars 비트 — ANA(Asymmetric Namespace Access) reporting 지원 여부 */
	if (ctrlr->cdata.cmic.anars) {
		ctrlr->log_page_supported[SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS] = true; /* [한국어] LID=0Ch — multi-path 환경에서 namespace 별 path 상태 */
		/* [한국어] 사용자가 ANA 자동 read를 비활성화하지 않았다면 즉시 ANA log를 받아와 ns 상태에 반영 */
		if (!ctrlr->opts.disable_read_ana_log_page) {
			rc = nvme_ctrlr_update_ana_log_page(ctrlr); /* [한국어] admin GET_LOG_PAGE 동기 호출 + 결과를 ctrlr->ana_log_page 에 저장 */
			if (rc == 0) {
				/* [한국어] 받아온 ANA log page를 파싱해 각 ns 의 ana_state(OPTIMIZED/NON-OPTIMIZED/INACCESSIBLE/PERSISTENT_LOSS/CHANGE)를 갱신 */
				nvme_ctrlr_parse_ana_log_page(ctrlr, nvme_ctrlr_update_ns_ana_states,
							      ctrlr);
			}
		}
	}

	/* [한국어] CTRATT(Controller Attributes).fdps — Flexible Data Placement (NVMe 2.0+) 지원 여부 */
	if (ctrlr->cdata.ctratt.bits.fdps) {
		/* [한국어] FDP 관련 4종 log page 모두 활성화 — 데이터 배치 정책, 통계, 이벤트 추적용 */
		ctrlr->log_page_supported[SPDK_NVME_LOG_FDP_CONFIGURATIONS] = true;        /* [한국어] FDP 구성 (RUH/RUH 그룹) */
		ctrlr->log_page_supported[SPDK_NVME_LOG_RECLAIM_UNIT_HANDLE_USAGE] = true; /* [한국어] RUH 사용량 통계 */
		ctrlr->log_page_supported[SPDK_NVME_LOG_FDP_STATISTICS] = true;            /* [한국어] FDP 동작 통계 */
		ctrlr->log_page_supported[SPDK_NVME_LOG_FDP_EVENTS] = true;                /* [한국어] FDP 이벤트 (media wear-out 등) */
	}

	/* [한국어] Intel 벤더 + PCIe 트랜스포트 + Intel 전용 quirk 비활성 → Intel vendor log pages 단계로 분기
	 *   (NVMe-oF 트랜스포트에서는 vendor log 가 의미 없으므로 PCIe 만 해당) */
	if (ctrlr->cdata.vid == SPDK_PCI_VID_INTEL &&
	    ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE &&
	    !(ctrlr->quirks & NVME_INTEL_QUIRK_NO_LOG_PAGES)) {
		/* [한국어] 다음 상태: Intel 전용 log page 지원 비트맵 설정 (LATENCY_TRACKING 등) */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_INTEL_LOG_PAGES,
				     ctrlr->opts.admin_timeout_ms);

	} else {
		/* [한국어] Intel 비대상 → log page 단계 종료, feature 지원 비트맵 단계로 직행 */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES,
				     ctrlr->opts.admin_timeout_ms);

	}

	return rc;                                                       /* [한국어] ANA log page 동기 read 가 실패했다면 음수, 그 외 0 */
}

/*
 * [한국어]
 * nvme_ctrlr_set_intel_supported_features - Intel 벤더 전용 feature 비트맵 활성화
 *
 * @ctrlr: 대상 NVMe 컨트롤러 (Intel 벤더로 검증된 상태)
 *
 * 동기/배경:
 *   NVMe Set/Get Features 명령의 Feature Identifier 공간 중 C0h~FFh 범위는 vendor-specific.
 *   Intel SSD 들은 이 영역에 7개의 자체 feature(MAX_LBA, NATIVE_MAX_LBA, POWER_GOVERNOR,
 *   SMBUS_ADDRESS, LED_PATTERN, RESET_TIMED_WORKLOAD_COUNTERS, LATENCY_TRACKING)을 정의함.
 *   호출자(set_supported_features)가 vid==INTEL 검증 후 이 함수를 부르므로 unconditional 활성화.
 *
 * 동작: feature_supported 비트맵의 7개 Intel 항목을 true 로 마킹 (실제 동작은 사용자가
 *       Set/Get Features 호출 시 이 비트맵을 보고 반환할 뿐, 컨트롤러에 추가 명령 안 보냄).
 *
 * 호출 체인:
 *   nvme_ctrlr_set_supported_features → [이 함수] (vid==INTEL 시만)
 */
static void
nvme_ctrlr_set_intel_supported_features(struct spdk_nvme_ctrlr *ctrlr)
{
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_MAX_LBA] = true;                        /* [한국어] FID=C1h — 사용자 가시 LBA 최대값 (Intel quirk) */
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_NATIVE_MAX_LBA] = true;                 /* [한국어] FID=C2h — 디바이스 native 최대 LBA (provisioning 전) */
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_POWER_GOVERNOR_SETTING] = true;         /* [한국어] FID=C6h — Power state governor (성능/전력 trade-off) */
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_SMBUS_ADDRESS] = true;                  /* [한국어] FID=C8h — SMBus 주소 (BMC 통신용) */
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_LED_PATTERN] = true;                    /* [한국어] FID=C7h — LED indicator 패턴 (식별/locate 용) */
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_RESET_TIMED_WORKLOAD_COUNTERS] = true;  /* [한국어] FID=D5h — 시간 기반 워크로드 카운터 리셋 */
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING] = true;               /* [한국어] FID=E2h — Intel SSD latency histogram (워크로드 분석용) */
}

/*
 * [한국어]
 * nvme_ctrlr_set_arbitration_feature - Arbitration(큐 우선순위 가중치) 설정 admin 명령 발행
 *
 * @ctrlr: 대상 NVMe 컨트롤러
 *
 * 동기/배경:
 *   NVMe 컨트롤러는 여러 IO Submission Queue 를 라운드 로빈 또는 가중치 기반으로 처리.
 *   Arbitration Feature(FID=01h) 는 두 가지 파라미터를 설정:
 *     • Arbitration Burst(AB): 한 번에 fetch 할 SQE 수 (2^AB), 0=금지
 *     • WRR(Weighted Round Robin) 가중치: HPW/MPW/LPW (high/medium/low priority weight)
 *   parallelink 같은 응용은 burst 를 키워 throughput, 가중치로 QoS 를 조정 가능.
 *
 * 동작 단계:
 *   1) opts.arbitration_burst==0 → no-op (기본값 유지)
 *   2) >7 검증 (3-bit 필드) — 잘못된 값이면 warning 출력 후 return
 *   3) completion poll status 동적 할당 (admin 명령 동기 대기용)
 *   4) cdw11 비트필드 빌드:
 *        bits[2:0]   = AB (burst exponent)
 *        bits[15:8]  = LPW (low priority weight)  — WRR 지원 시
 *        bits[23:16] = MPW (medium priority weight)
 *        bits[31:24] = HPW (high priority weight)
 *   5) Set Features (FID=01h) admin 명령 발행 — 비동기 콜백으로 nvme_completion_poll_cb
 *   6) nvme_wait_for_adminq_completion 으로 동기 대기 (admin queue polling)
 *   7) 실패 시 ERRLOG 만 출력하고 다음 단계로 진행 (치명적이지 않음)
 *
 * 실행 컨텍스트:
 *   nvme_ctrlr_set_supported_features 끝부분에서 동기 호출.
 *   admin queue 를 polling 으로 돌려야 하므로 controller bring-up 단계 또는 idle 한 시점에만 호출.
 */
static void
nvme_ctrlr_set_arbitration_feature(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t cdw11;                                                    /* [한국어] Set Features 의 Command DWord 11 — AB + WRR weights 인코딩 */
	struct nvme_completion_poll_status *status;                        /* [한국어] 비동기 콜백이 결과를 채울 동기 폴링 핸들 */
	int rc;                                                            /* [한국어] wait_for_adminq_completion 반환값 (음수=에러) */

	/* [한국어] 사용자가 arbitration_burst 를 설정 안 했으면(0) 기본값 유지 — 명령 안 보냄 */
	if (ctrlr->opts.arbitration_burst == 0) {
		return;
	}

	/* [한국어] AB 필드는 NVMe 스펙상 3 bit (0~7) — 8 이상은 잘못된 값이므로 거부 */
	if (ctrlr->opts.arbitration_burst > 7) {
		NVME_CTRLR_WARNLOG(ctrlr, "Valid arbitration burst values is from 0-7\n");
		return;
	}

	/* [한국어] poll status 객체 할당 — nvme_completion_poll_cb 가 done/cpl 채우면
	 * nvme_wait_for_adminq_completion 이 그걸 보고 동기 대기를 끝냄 */
	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return;                                                    /* [한국어] OOM — feature 설정 포기, 디바이스 동작은 계속 (기본값) */
	}

	cdw11 = ctrlr->opts.arbitration_burst;                             /* [한국어] AB(Arbitration Burst) 를 cdw11 의 bits[2:0] 에 배치 */

	/* [한국어] Identify Ctrlr 에서 WRR(Weighted Round Robin) 을 지원한다고 보고했을 때만 가중치 반영
	 * — 미지원 컨트롤러에 WRR 비트를 보내면 invalid field 에러 */
	if (spdk_nvme_ctrlr_get_flags(ctrlr) & SPDK_NVME_CTRLR_WRR_SUPPORTED) {
		cdw11 |= (uint32_t)ctrlr->opts.low_priority_weight << 8;       /* [한국어] LPW: bits[15:8] — low priority queue 슬롯 가중치 */
		cdw11 |= (uint32_t)ctrlr->opts.medium_priority_weight << 16;   /* [한국어] MPW: bits[23:16] — medium priority */
		cdw11 |= (uint32_t)ctrlr->opts.high_priority_weight << 24;     /* [한국어] HPW: bits[31:24] — high priority */
	}

	/* [한국어] Admin Set Features 명령 발행 — FID=01h(SPDK_NVME_FEAT_ARBITRATION) + cdw11
	 * 콜백 nvme_completion_poll_cb 는 status->done=true 와 cpl 을 채움, 동기 대기는 wait 함수가 함 */
	if (spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_FEAT_ARBITRATION,
					    cdw11, 0, NULL, 0,
					    nvme_completion_poll_cb, status) < 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Set arbitration feature failed\n");
		free(status);                                              /* [한국어] 명령 자체가 큐에 못 들어갔으므로 status 즉시 해제 */
		return;
	}

	/* [한국어] admin queue polling 으로 status->done==true 까지 대기 (true=완료 후 status free)
	 * 내부적으로 spdk_nvme_qpair_process_completions(ctrlr->adminq) 반복 호출 */
	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (rc) {
		/* [한국어] 실패 시 ERRLOG만 출력 — 컨트롤러 bring-up은 계속 진행 (치명적 아님) */
		NVME_CTRLR_ERRLOG(ctrlr, "wait for spdk_nvme_ctrlr_cmd_set_feature failed: rc=%s\n",
				  spdk_strerror(abs(rc)));
	}
}

/*
 * [한국어]
 * nvme_ctrlr_set_supported_features - 컨트롤러 feature 지원 비트맵 + Arbitration 설정
 *
 * @ctrlr: 대상 NVMe 컨트롤러
 *
 * 동기/배경:
 *   NVMe Set/Get Features 명령(opcode 09h/0Ah)의 FID(Feature Identifier) 공간 중,
 *   어느 FID 가 실제로 의미를 갖는지 컨트롤러마다 다름. SPDK 는 Identify Ctrlr 결과의
 *   비트필드(vwc/apsta/hmpre 등)를 보고 *지원 가능* 한 FID 를 미리 비트맵화해서
 *   사용자가 spdk_nvme_ctrlr_set_feature() 호출 시 사전 검증에 사용한다.
 *
 * 동작 단계:
 *   1) feature_supported[] 비트맵 0으로 초기화
 *   2) NVMe 스펙 mandatory 9종 무조건 활성화 (Arbitration, Power Mgmt, ...)
 *   3) 옵션 feature 3종 — Identify Ctrlr 비트로 분기:
 *        vwc.present=1 → Volatile Write Cache (FID=06h)
 *        apsta.supported=1 → Autonomous Power State Transition (FID=0Ch)
 *        hmpre>0 → Host Memory Buffer (FID=0Dh, NVMe 1.2+)
 *   4) Intel 벤더면 nvme_ctrlr_set_intel_supported_features 추가 호출 (Intel FID 7종)
 *   5) nvme_ctrlr_set_arbitration_feature — 사용자 opts.arbitration_burst 가 설정됐으면
 *      실제 admin Set Features 명령 발행 (지원 비트맵 설정과 다른 작업)
 *
 * 호출 체인:
 *   nvme_ctrlr_process_init → [이 함수] → set_arbitration_feature(동기 admin)
 *                                      → set_intel_supported_features(Intel 시)
 */
static void
nvme_ctrlr_set_supported_features(struct spdk_nvme_ctrlr *ctrlr)
{
	memset(ctrlr->feature_supported, 0, sizeof(ctrlr->feature_supported)); /* [한국어] 비트맵 전체 초기화 후 mandatory + 옵션 비트만 켬 */
	/* Mandatory features */
	/* [한국어] NVMe 스펙 1.x Section 5.21.1.x — 모든 컨트롤러가 의무적으로 지원하는 9종 feature */
	ctrlr->feature_supported[SPDK_NVME_FEAT_ARBITRATION] = true;                       /* [한국어] FID=01h — 큐 우선순위 가중치 (Arbitration Burst + WRR) */
	ctrlr->feature_supported[SPDK_NVME_FEAT_POWER_MANAGEMENT] = true;                  /* [한국어] FID=02h — Power State (PS0~PS31) 선택 */
	ctrlr->feature_supported[SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD] = true;             /* [한국어] FID=04h — 온도 경고 임계값 */
	ctrlr->feature_supported[SPDK_NVME_FEAT_ERROR_RECOVERY] = true;                    /* [한국어] FID=05h — TLER (Time-Limited Error Recovery) */
	ctrlr->feature_supported[SPDK_NVME_FEAT_NUMBER_OF_QUEUES] = true;                  /* [한국어] FID=07h — IO SQ/CQ 개수 협상 */
	ctrlr->feature_supported[SPDK_NVME_FEAT_INTERRUPT_COALESCING] = true;              /* [한국어] FID=08h — 인터럽트 합치기 (threshold + time) */
	ctrlr->feature_supported[SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION] = true;    /* [한국어] FID=09h — MSI-X 벡터별 coalescing 설정 */
	ctrlr->feature_supported[SPDK_NVME_FEAT_WRITE_ATOMICITY] = true;                   /* [한국어] FID=0Ah — write atomicity 보장 단위 (DUN/AWUN) */
	ctrlr->feature_supported[SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION] = true;         /* [한국어] FID=0Bh — AER 가 보고할 이벤트 종류 마스크 */
	/* Optional features */
	/* [한국어] Volatile Write Cache(FID=06h) — 컨트롤러가 휘발성 캐시를 가질 때만 의미 (vwc.present 비트) */
	if (ctrlr->cdata.vwc.present) {
		ctrlr->feature_supported[SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE] = true;      /* [한국어] flush 명령으로 캐시 비울 수 있는지 결정 */
	}
	/* [한국어] APST(Autonomous Power State Transition, FID=0Ch) — idle 시 자동 저전력 전환 */
	if (ctrlr->cdata.apsta.supported) {
		ctrlr->feature_supported[SPDK_NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION] = true;
	}
	/* [한국어] HMB(Host Memory Buffer, FID=0Dh) — 컨트롤러가 호스트 RAM 일부를 working memory로 빌려씀 (DRAM-less SSD 용)
	 * hmpre(Host Memory Buffer Preferred Size) > 0 이면 컨트롤러가 원함 */
	if (ctrlr->cdata.hmpre) {
		ctrlr->feature_supported[SPDK_NVME_FEAT_HOST_MEM_BUFFER] = true;
	}
	/* [한국어] Intel 벤더 SSD 면 추가 7종 feature 활성화 (vendor-specific FID C0h~FFh 영역) */
	if (ctrlr->cdata.vid == SPDK_PCI_VID_INTEL) {
		nvme_ctrlr_set_intel_supported_features(ctrlr);
	}

	/* [한국어] 비트맵 설정과 별개로 — 사용자가 opts.arbitration_burst 를 설정했다면
	 * 실제 admin Set Features 명령으로 컨트롤러에 반영. 위 비트맵은 *어떤 FID가 가능한가* 를 추적하고
	 * 이 함수는 *실제 명령 발행* 으로 동작이 다름 */
	nvme_ctrlr_set_arbitration_feature(ctrlr);
}

/*
 * [한국어]
 * nvme_ctrlr_set_host_feature_done - Set Features (Host Behavior Support) 비동기 완료 콜백
 *
 * @arg: cb_arg 로 전달된 spdk_nvme_ctrlr* (set_host_feature 가 등록)
 * @cpl: NVMe CQE — sc/sct 로 성공/실패 판단
 *
 * 동기/배경:
 *   nvme_ctrlr_set_host_feature 가 비동기로 발행한 admin Set Features (FID=16h, Host Behavior
 *   Support) 의 완료 콜백. host->lbafee=1 (Extended LBA Format Enabled) 같은 호스트 동작 힌트를
 *   컨트롤러에 알린 결과를 처리한다.
 *
 * 동작:
 *   1) 사전에 spdk_dma_zmalloc 으로 잡아둔 host buffer (tmp_ptr) 해제 — 이 시점이면 컨트롤러가
 *      DMA 로 다 읽어갔으므로 안전
 *   2) CQE error 검사 — 실패면 ctrlr 상태를 ERROR 로 강제 (controller bring-up 중단)
 *   3) 성공이면 feature_supported 비트맵에 HOST_BEHAVIOR_SUPPORT 마킹
 *   4) 다음 상태 SET_DB_BUF_CFG (Doorbell Buffer Config — shadow doorbell 설정) 로 전이
 *
 * 실행 컨텍스트:
 *   admin queue completion polling 시 호출. process_init state machine 의 비동기 step.
 *
 * 호출 체인:
 *   admin completion poll → nvme_completion_poll_cb 가 아닌 직접 등록된 콜백 [이 함수]
 *                       → nvme_ctrlr_set_state(SET_DB_BUF_CFG)
 */
static void
nvme_ctrlr_set_host_feature_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;      /* [한국어] cb_arg 캐스팅 — Set Features 발행 시 self 포인터를 cb_arg 로 전달 */

	/* [한국어] Set Features (with data buffer) 의 host buffer 해제
	 * 컨트롤러가 DMA로 읽고 CQE 가 도착했으니 더 이상 메모리 유지 불필요 */
	spdk_free(ctrlr->tmp_ptr);
	ctrlr->tmp_ptr = NULL;                                              /* [한국어] dangling 방지 */

	/* [한국어] CQE 의 status.sct(Status Code Type) + status.sc(Status Code) 로 에러 판단
	 * 실패 시 admin error log 출력 후 controller 상태를 ERROR 로 강제 — bring-up 중단 */
	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Set host behavior support feature failed: SC %x SCT %x\n",
				  cpl->status.sc, cpl->status.sct);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);  /* [한국어] 에러 상태는 timeout 무한 — 사용자가 reset 결정해야 회복 */
		return;
	}

	/* [한국어] 성공 — feature_supported 비트맵에 FID=16h(HOST_BEHAVIOR_SUPPORT) 마킹
	 * 이후 사용자가 Get Features 로 이 FID 조회 시 사전 검증 통과 */
	ctrlr->feature_supported[SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT] = true;

	/* [한국어] 다음 단계: Doorbell Buffer Config (FID=7Eh, NVMe 1.3+ shadow doorbell)
	 * VM virtio-nvme 같은 환경에서 MMIO doorbell write를 host 메모리에 mirror 해 cost 절감 */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_DB_BUF_CFG,
			     ctrlr->opts.admin_timeout_ms);
}

/* We do not want to do add synchronous operation anymore.
 * We set the Host Behavior Support feature asynchronousin in different states.
 */
/*
 * [한국어]
 * nvme_ctrlr_set_host_feature - Host Behavior Support feature 비동기 발행 (lbafee=1)
 *
 * @ctrlr: 대상 NVMe 컨트롤러
 * @return: 0 성공(비동기 발행 OK), 음수 = 실패 (state ERROR 로 전이됨)
 *
 * 동기/배경:
 *   NVMe 1.4 + Set Features FID=16h (Host Behavior Support) 의 lbafee 비트는
 *   "호스트가 LBA Format Extension(64-bit RefTag 등) 을 이해하고 처리할 수 있다" 는 신호.
 *   ctratt.bits.elbas (Extended LBA Format Support) 가 1인 컨트롤러에 한해 보낸다.
 *
 *   영문 주석 의도: 과거에는 동기 호출이었으나, 다른 상태들과 일관되게 *비동기 + state machine* 로 통일.
 *
 * 동작 단계:
 *   1) elbas 미지원 컨트롤러 → 이 단계 skip, SET_DB_BUF_CFG 로 직접 전이 후 return 0
 *   2) DMA-able 4KB 정렬 buffer 동적 할당 (host_behavior 구조체 담을 자리, OOM 시 error path)
 *   3) state 를 WAIT_FOR_SET_HOST_FEATURE 로 마킹 (process_init poller 가 완료 콜백 대기)
 *   4) host->lbafee = 1 설정 (컨트롤러에게 "확장 LBA 포맷 OK" 통지)
 *   5) spdk_nvme_ctrlr_cmd_set_feature 비동기 호출 — 콜백 = nvme_ctrlr_set_host_feature_done
 *   6) 발행 실패 시 error label → 메모리 free + state ERROR
 *
 * 실행 컨텍스트:
 *   nvme_ctrlr_process_init state machine 의 SET_HOST_FEATURE 단계.
 *   비동기 발행이므로 즉시 return, 완료는 done 콜백이 처리.
 */
static int
nvme_ctrlr_set_host_feature(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_host_behavior *host;                              /* [한국어] DMA buffer 캐스팅용 — Set Features 의 data 페이로드 */
	int rc;                                                            /* [한국어] 반환 코드 */

	/* [한국어] CTRATT(Controller Attributes).elbas — Extended LBA Format Support 미지원이면
	 * Host Behavior 도 보낼 필요 없이 다음 단계로 진행 (NVMe < 1.4 컨트롤러 호환) */
	if (!ctrlr->cdata.ctratt.bits.elbas) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_DB_BUF_CFG,
				     ctrlr->opts.admin_timeout_ms);
		return 0;                                                  /* [한국어] no-op 성공 */
	}

	/* [한국어] DMA buffer 할당 — 4KB 정렬 (PRP 호환), zmalloc 으로 0 초기화
	 * tmp_ptr 에 보관하다가 done 콜백이 free 함 (수명 = 명령 in-flight 동안) */
	ctrlr->tmp_ptr = spdk_dma_zmalloc(sizeof(struct spdk_nvme_host_behavior), 4096, NULL);
	if (!ctrlr->tmp_ptr) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate host behavior support data\n");
		rc = -ENOMEM;
		goto error;                                                /* [한국어] OOM — error label 로 점프 (state ERROR) */
	}

	/* [한국어] 상태 전이 — process_init poller 가 admin completion 을 기다림
	 * 이 시점부터 done 콜백이 실행되기 전까지 다른 state 로 진행 안 됨 */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_SET_HOST_FEATURE,
			     ctrlr->opts.admin_timeout_ms);

	host = ctrlr->tmp_ptr;                                             /* [한국어] DMA buffer를 spec 구조체 포인터로 alias */

	host->lbafee = 1;                                                  /* [한국어] LBA Format Extension Enable — 64-bit metadata/RefTag 처리 가능 신호 */

	/* [한국어] Admin Set Features 비동기 발행
	 * cdw11=0 / cdw12=0 / data buffer + size = host_behavior / 콜백 = done */
	rc = spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT,
					     0, 0, host, sizeof(struct spdk_nvme_host_behavior),
					     nvme_ctrlr_set_host_feature_done, ctrlr);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Set host behavior support feature failed: %d\n", rc);
		goto error;                                                /* [한국어] admin SQ 가득 참 등 발행 실패 — error path */
	}

	return 0;                                                          /* [한국어] 비동기 발행 성공 — 완료는 done 콜백이 처리 */

error:
	/* [한국어] 에러 경로: 할당된 buffer free + state ERROR 마킹 */
	spdk_free(ctrlr->tmp_ptr);
	ctrlr->tmp_ptr = NULL;                                             /* [한국어] dangling 방지 */

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE); /* [한국어] timeout 무한 = 회복 불가, 사용자 reset 필요 */
	return rc;                                                         /* [한국어] -ENOMEM 또는 set_feature 의 음수 반환 */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_is_failed - 컨트롤러가 fail 상태인지 단순 조회 (public API)
 *
 * @ctrlr: 조회 대상 컨트롤러
 * @return: true = is_failed 플래그 set, false = 정상
 *
 * 동기/배경:
 *   사용자가 I/O 발행 전, 또는 reset 결정 전 컨트롤러가 정상 상태인지 빠르게 확인하는 용도.
 *   nvme_ctrlr_fail() 이 호출되면 is_failed 플래그가 set 되며, 회복은 reset 만 가능.
 *
 * 동작: 단일 플래그 read — 락 불필요 (atomic read 의미, 정확성 비요구).
 */
bool
spdk_nvme_ctrlr_is_failed(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->is_failed;                                           /* [한국어] 플래그 read — 정확성보다 가시성 우선이라 락 없이 OK */
}

/*
 * [한국어]
 * nvme_ctrlr_fail - 내부용 fail 진입점 (호출자가 ctrlr lock 보유 가정)
 *
 * @ctrlr: fail 처리할 컨트롤러
 * @hot_remove: true 면 PCIe surprise removal (디바이스가 물리적으로 사라짐)
 *
 * 동기/배경:
 *   컨트롤러가 회복 불가 에러 (펌웨어 패닉, MMIO 응답 없음, hot remove 등) 상태로 진입했을 때
 *   in-flight IO 모두를 즉시 실패시키지 않고, *플래그만 set + admin qpair disconnect* 한다.
 *   in-flight IO 의 실제 실패 보고는 process_completions 가 다음 polling 사이클에 처리.
 *   이 lazy 처리 패턴은 lock 보유 시간을 최소화 + 사용자 callback 컨텍스트 안전 보장 목적.
 *
 * 동작 단계:
 *   1) hot_remove → is_removed 플래그 set (PCIe ENOENT 시 transport 가 검사)
 *   2) 이미 is_failed → 중복 보고 무시 (idempotent)
 *   3) 이미 is_disconnecting → reset 진행 중이라 별도 fail 처리 불필요
 *   4) is_failed = true 마킹 + state = ERROR(timeout 무한)
 *   5) admin qpair 강제 disconnect (transport 레이어 — PCIe BAR 해제, RDMA QP 파괴 등)
 *   6) ERRLOG 출력 — 사용자 진단용
 *
 * 실행 컨텍스트:
 *   ★ 호출자가 이미 ctrlr lock 을 잡고 있어야 함 (transport callback 등 내부 경로용).
 *   외부 사용자는 lock wrapper 인 spdk_nvme_ctrlr_fail() 을 호출.
 *
 * 호출 체인:
 *   PCIe pcie_ctrlr_construct 실패 / transport 에러 / spdk_nvme_ctrlr_fail wrapper → [이 함수]
 *                       → nvme_transport_ctrlr_disconnect_qpair(adminq)
 */
void
nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr, bool hot_remove)
{
	/*
	 * Set the flag here and leave the work failure of qpairs to
	 * spdk_nvme_qpair_process_completions().
	 */
	/* [한국어] hot_remove 플래그 — PCIe 디바이스가 surprise removal 된 경우 (사용자 unplug 등)
	 * transport layer 의 ENOENT 에러를 영구적인 디바이스 부재로 해석하게 함 */
	if (hot_remove) {
		ctrlr->is_removed = true;
	}

	/* [한국어] 이미 failed 상태면 추가 작업 없음 — 여러 경로에서 중복 호출 가능하므로 idempotent */
	if (ctrlr->is_failed) {
		NVME_CTRLR_NOTICELOG(ctrlr, "already in failed state\n");
		return;
	}

	/* [한국어] reset 진행 중(is_disconnecting=true) 이면 reset 흐름이 alone 처리 — fail 별도 시작 X */
	if (ctrlr->is_disconnecting) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "already disconnecting\n");
		return;
	}

	ctrlr->is_failed = true;                                           /* [한국어] 플래그 set — 이후 IO 발행자가 검사하여 EIO 반환 */
	/* [한국어] state = ERROR + timeout 무한 — process_init 가 진행 중이라면 그 자리에서 정지 */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	/* [한국어] admin qpair 강제 disconnect — 더 이상 admin 명령을 디바이스에 보내지 않게
	 * PCIe면 BAR doorbell write 차단, RDMA면 QP 파괴 등 transport-specific cleanup */
	nvme_transport_ctrlr_disconnect_qpair(ctrlr, ctrlr->adminq);
	NVME_CTRLR_ERRLOG(ctrlr, "in failed state.\n");                    /* [한국어] 사용자 진단용 — dmesg/syslog 에 명확한 신호 */
}

/**
 * This public API function will try to take the controller lock.
 * Any private functions being called from a thread already holding
 * the ctrlr lock should call nvme_ctrlr_fail directly.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_fail - 외부 사용자용 fail 진입점 (lock wrapper)
 *
 * @ctrlr: fail 마킹할 컨트롤러
 *
 * 동기/배경:
 *   영문 주석대로 — 외부 사용자(bdev_nvme, 애플리케이션 등) 가 컨트롤러를 강제 실패시킬 때 호출.
 *   ctrlr lock 을 자동 획득/해제 한다. lock 을 이미 보유한 내부 경로(transport callback 등) 는
 *   nvme_ctrlr_fail 을 직접 호출해야 deadlock 회피.
 *
 * 동작: lock 획득 → nvme_ctrlr_fail(hot_remove=false) → unlock
 */
void
spdk_nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_ctrlr_lock(ctrlr);                                            /* [한국어] ctrlr->ctrlr_lock 획득 — multi-thread 안전성 */
	nvme_ctrlr_fail(ctrlr, false);                                     /* [한국어] hot_remove=false 외부 호출은 보통 논리적 fail (디바이스는 살아있을 수 있음) */
	nvme_ctrlr_unlock(ctrlr);                                          /* [한국어] lock 해제 */
}

/*
 * [한국어]
 * nvme_ctrlr_shutdown_set_cc_done - shutdown 시퀀스에서 CC 레지스터 쓰기 완료 콜백
 *
 * @_ctx:  nvme_ctrlr_detach_ctx (shutdown 진행 상태를 담은 컨텍스트)
 * @value: 방금 쓴 CC 레지스터 값 (참고용 — shutdown 본 콜백은 사용 안 함)
 * @cpl:   admin write 명령의 NVMe completion (성공/실패 sct/sc 포함)
 *
 * 동기/배경:
 *   shutdown chain 의 4단계 비동기 콜백 체인 중 1번째 완료 시점에 호출된다.
 *   체인 흐름:
 *     nvme_ctrlr_shutdown_async() → CC read 발행
 *       → nvme_ctrlr_shutdown_get_cc_done() (CC read 완료 → SHN/EN 비트 갱신 후 CC write 발행)
 *         → nvme_ctrlr_shutdown_set_cc_done() (CC write 완료 → CSTS 폴링 준비) ← 본 함수
 *           → nvme_ctrlr_shutdown_poll_async() (CSTS read 반복 → SHST=Complete 대기)
 *             → nvme_ctrlr_shutdown_get_csts_done() (CSTS read 완료 → 다시 poll 평가)
 *
 *   NVMe Spec 1.x §7.6.2 Shutdown Processing:
 *     CC.SHN(Shutdown Notification) = 01b (Normal) or 10b (Abrupt) 쓰기 후
 *     컨트롤러가 메타데이터/캐시를 안전하게 NVM에 flush 하고 CSTS.SHST = 10b(Complete)로 표기.
 *   즉 이 함수는 "SHN 비트가 디바이스에 기록되었다"는 사실만 보장하며, 실제 shutdown 완료는
 *   다음 단계(CSTS 폴링)에서 확인한다.
 *
 * 동작 단계:
 *   [1] CPL 에러 검사 — write 자체 실패 시 즉시 shutdown_complete=true 로 종료(정리는 caller 가 수행).
 *   [2] no_shn_notification 옵션 분기 — 사용자가 SHN 통지를 끈 경우(테스트/특정 quirk),
 *       SHN 대신 EN=0 만 쓰는 경로이므로 CSTS 폴링 불필요 → 즉시 완료 마킹.
 *   [3] RTD3E(NVMe Spec §5.15.2.2 Identify Controller, RTD3 Entry Latency) 기반 shutdown timeout 계산.
 *       RTD3E 단위는 µs 이므로 ms 로 변환 (CEIL_DIV 1000), 최소 10초(10000ms) 강제.
 *   [4] shutdown_start_tsc 기록 + state = CHECK_CSTS — poll_async 가 다음 진입 시 CSTS read 시작.
 *
 * 실행 컨텍스트:
 *   admin qpair completion 처리 스레드(보통 management thread). spdk_nvme_qpair_process_completions()
 *   콜 스택에서 동기적으로 호출됨. ctrlr lock 은 caller(nvme_ctrlr_shutdown_poll_async 와 동일 컨텍스트)
 *   가 보유하고 있다고 가정 — 본 콜백은 별도 lock 획득 안 함.
 *
 * 호출 체인:
 *   nvme_ctrlr_set_cc_async (admin write 발행) → admin completion → 본 콜백
 */
static void
nvme_ctrlr_shutdown_set_cc_done(void *_ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr_detach_ctx *ctx = _ctx;
	/* [한국어] void* → 실제 detach 컨텍스트로 캐스팅 — async helper 의 cb_arg 타입 규약. */
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;
	/* [한국어] 로깅·옵션 조회용으로 ctrlr 포인터 추출 (편의 변수). */

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* [한국어] CPL 의 sct/sc 필드 검사 — admin write 가 실패했는지(SC ≠ Success). */
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to write CC.SHN\n");
		/* [한국어] 사용자/운영자 진단용 — CC 레지스터 write 실패는 디바이스 상태 이상 신호. */
		ctx->shutdown_complete = true;
		/* [한국어] 더 진행해도 의미 없음 → 즉시 완료 마킹하여 상위 poll loop 종료 유도.
		 *         caller 가 이후 detach 정리 경로로 진입한다. */
		return;
	}

	if (ctrlr->opts.no_shn_notification) {
		/* [한국어] 사용자 옵션: SHN 통지 없이 단순히 CC.EN=0 만으로 디바이스 비활성화.
		 *         (일부 가상 디바이스/테스트 환경에서 SHN 처리가 부정확한 경우 우회.)
		 *         이 경로에서는 get_cc_done 이 EN=0 을 직접 썼으므로 CSTS 폴링이 불필요. */
		ctx->shutdown_complete = true;
		/* [한국어] EN=0 write 가 성공했으므로 shutdown 절차는 끝. */
		return;
	}

	/*
	 * The NVMe specification defines RTD3E to be the time between
	 *  setting SHN = 1 until the controller will set SHST = 10b.
	 * If the device doesn't report RTD3 entry latency, or if it
	 *  reports RTD3 entry latency less than 10 seconds, pick
	 *  10 seconds as a reasonable amount of time to
	 *  wait before proceeding.
	 */
	/* [한국어] NVMe Spec §5.15.2.2 Identify Controller — RTD3E(Runtime D3 Entry Latency, µs)는
	 *         호스트가 SHN=1 을 쓴 시점부터 컨트롤러가 SHST=10b(Complete) 로 표기할 때까지의
	 *         최대 예상 시간이다. 0 또는 너무 작은 값을 보고하는 디바이스는 보수적으로 10s 사용. */
	NVME_CTRLR_DEBUGLOG(ctrlr, "RTD3E = %" PRIu32 " us\n", ctrlr->cdata.rtd3e);
	/* [한국어] 디바이스가 광고한 RTD3E(µs) 디버그 출력 — 진단/튜닝 참고. */
	ctx->shutdown_timeout_ms = SPDK_CEIL_DIV(ctrlr->cdata.rtd3e, 1000);
	/* [한국어] µs → ms 올림 변환. CEIL_DIV 사용 이유: rtd3e 가 1000 미만이면 0 ms 가 되어 timeout 즉시 만료
	 *         하는 버그를 방지하기 위함. */
	ctx->shutdown_timeout_ms = spdk_max(ctx->shutdown_timeout_ms, 10000);
	/* [한국어] 최소 10초(10000ms) 보장 — 디바이스 보고치가 비현실적으로 작은 경우 대비. */
	NVME_CTRLR_DEBUGLOG(ctrlr, "shutdown timeout = %" PRIu32 " ms\n", ctx->shutdown_timeout_ms);
	/* [한국어] 최종 적용된 timeout 값을 로깅 — 이후 poll_async 가 ms_waited 와 비교. */

	ctx->shutdown_start_tsc = spdk_get_ticks();
	/* [한국어] 폴링 시작 기준 시각(TSC tick) 저장. poll_async 에서
	 *         ms_waited = (now - start) * 1000 / hz 로 경과 시간 계산. */
	ctx->state = NVME_CTRLR_DETACH_CHECK_CSTS;
	/* [한국어] 상태 머신 전이: SET_CC → CHECK_CSTS.
	 *         다음 poll_async 호출 시 CSTS read 를 발행하는 분기로 진입. */
}

/*
 * [한국어]
 * nvme_ctrlr_shutdown_get_cc_done - shutdown chain 의 CC 레지스터 read 완료 콜백 (RMW 의 R 단계)
 *
 * @_ctx:  nvme_ctrlr_detach_ctx
 * @value: 방금 읽은 CC 레지스터 32-bit 값 (uint64_t 컨테이너에 담겨 전달됨)
 * @cpl:   admin Get Property/MMIO read 결과 CPL
 *
 * 동기/배경:
 *   shutdown chain 의 두 번째 단계. CC 레지스터를 RMW(Read-Modify-Write) 하기 위해
 *   먼저 read 가 필요하다. 이유:
 *     1) CC 의 다른 비트 (CSS, MPS, IOSQES, IOCQES, AMS 등)를 보존해야 한다.
 *        SHN 만 1 로 세팅하고 나머지를 0 으로 덮어쓰면 컨트롤러 동작이 망가진다.
 *     2) no_shn_notification 옵션 분기 시 EN 비트 현재 값(이미 0인지)도 확인해야 한다.
 *
 *   NVMe Spec 1.x §3.1.5 Controller Configuration (CC):
 *     - bit 4 (EN):    Enable — 1 = 컨트롤러 활성, 0 = 비활성/리셋
 *     - bits 14:13(SHN): Shutdown Notification — 00=No, 01=Normal, 10=Abrupt
 *     - 기타: CSS, MPS, AMS, IOSQES, IOCQES (보존 대상)
 *
 * 동작 단계:
 *   [1] CPL 에러면 shutdown_complete=true 로 즉시 종료.
 *   [2] value(64-bit MMIO read 컨테이너) 의 하위 32-bit 를 cc 로 캐스팅 + assert 로 상위 0 확인.
 *   [3] no_shn_notification 분기:
 *         - EN==0 이면 이미 비활성화 상태 → 추가 작업 없이 완료.
 *         - EN==1 이면 EN=0 으로 RMW (controller reset 효과).
 *       기본 분기:
 *         - SHN = SPDK_NVME_SHN_NORMAL (=01b) — 정상 shutdown 통지.
 *   [4] nvme_ctrlr_set_cc_async 로 write 발행. 실패 시 즉시 종료 마킹.
 *
 * 실행 컨텍스트:
 *   admin completion 처리 컨텍스트(get_cc_async 의 콜백). 본 함수는 set_cc_done 처럼
 *   별도 lock 을 잡지 않고 ctx 만 갱신.
 *
 * 호출 체인:
 *   nvme_ctrlr_get_cc_async (read 발행) → admin completion → 본 콜백
 *     → nvme_ctrlr_set_cc_async (write 발행, cb=set_cc_done)
 */
static void
nvme_ctrlr_shutdown_get_cc_done(void *_ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr_detach_ctx *ctx = _ctx;
	/* [한국어] cb_arg 복원 — chain 전반에서 ctx 가 진행 상태 보관. */
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;
	/* [한국어] 편의 변수 — async helper 호출과 로깅에 사용. */
	union spdk_nvme_cc_register cc;
	/* [한국어] CC 레지스터 비트필드 union — raw 32bit 와 .bits.{en, shn, css, ...} 양방향 접근. */
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* [한국어] read 자체가 실패했는지 확인 — 실패면 RMW 진행 불가. */
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CC register\n");
		ctx->shutdown_complete = true;
		/* [한국어] 폴링 루프 빠져나가도록 마킹 — caller 가 후속 정리. */
		return;
	}

	assert(value <= UINT32_MAX);
	/* [한국어] CC 는 32-bit 레지스터인데 async helper 는 64-bit 컨테이너를 사용 →
	 *         상위 32bit 가 0 임을 디버그 빌드에서 확인. */
	cc.raw = (uint32_t)value;
	/* [한국어] 32bit 값 그대로 union 에 적재 → bits 필드를 통해 EN/SHN/CSS 등 추출 가능. */

	if (ctrlr->opts.no_shn_notification) {
		/* [한국어] 옵션 분기: SHN 통지 생략 모드. */
		NVME_CTRLR_INFOLOG(ctrlr, "Disable SSD without shutdown notification\n");
		/* [한국어] 운영자에게 알리는 INFO 레벨 메시지 — 비표준 종료 경로 사용 중. */
		if (cc.bits.en == 0) {
			/* [한국어] EN 비트(NVMe §3.1.5)가 이미 0 — 컨트롤러가 이미 비활성 상태.
			 *         추가 write 불필요 → 즉시 완료 마킹. */
			ctx->shutdown_complete = true;
			return;
		}

		cc.bits.en = 0;
		/* [한국어] EN 비트만 1→0 으로 클리어. 다른 비트는 보존(RMW).
		 *         이는 controller reset 의 시작 신호 — 디바이스가 in-flight I/O 정리. */
	} else {
		cc.bits.shn = SPDK_NVME_SHN_NORMAL;
		/* [한국어] SHN(Shutdown Notification) = 01b (Normal Shutdown).
		 *         NVMe Spec §3.1.5: Normal 은 컨트롤러가 모든 outstanding 명령을 완료하고
		 *         캐시·메타데이터를 NVM 에 안전하게 flush 하도록 요청. (Abrupt=10b 와 대비.) */
	}

	rc = nvme_ctrlr_set_cc_async(ctrlr, cc.raw, nvme_ctrlr_shutdown_set_cc_done, ctx);
	/* [한국어] RMW 의 W 단계: 수정된 cc.raw 를 admin Set Property/MMIO write 로 발행.
	 *         완료 시 set_cc_done 콜백이 호출되어 chain 다음 단계(CSTS 폴링 준비)로 진입. */
	if (rc != 0) {
		/* [한국어] write 발행 자체가 실패 (admin queue full, allocation 실패 등). */
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to write CC.SHN\n");
		ctx->shutdown_complete = true;
		/* [한국어] 폴링 루프 종료 신호. */
	}
}

/*
 * [한국어]
 * nvme_ctrlr_shutdown_async - controller shutdown 비동기 시퀀스의 진입점
 *
 * @ctrlr: shutdown 시킬 컨트롤러
 * @ctx:   shutdown 진행 상태를 담을 detach 컨텍스트 (caller 가 stack/heap 으로 할당)
 *
 * 동기/배경:
 *   spdk_nvme_detach_async / spdk_nvme_ctrlr_disable / 정상 종료 경로에서 호출되어
 *   NVMe Spec §7.6.2 의 "Shutdown Processing" 시퀀스를 시작한다. 본 함수는 첫 번째 admin
 *   register read 를 발행하기만 하고 즉시 return — 실제 shutdown 진행은
 *   poll_async / 콜백 chain 에서 비동기로 진행된다.
 *
 *   중요: 본 함수가 return 한 시점에 shutdown 은 *시작* 된 것이지 완료된 것이 아니다.
 *         caller 는 ctx->shutdown_complete 가 true 가 될 때까지 nvme_ctrlr_shutdown_poll_async()
 *         를 반복 호출해야 한다 (또는 동기 wrapper 인 nvme_ctrlr_shutdown_poll_blocking).
 *
 * 동작 단계:
 *   [1] hot-removal 검사 — 디바이스가 PCIe 핫 언플러그된 경우, MMIO 가 모두 invalid 한 상태.
 *       shutdown 명령 시도해도 의미 없으므로 즉시 완료 마킹.
 *   [2] adminq 상태 검사 — adminq 가 NULL 이거나 transport 가 failure 상태면 admin 명령
 *       발행 불가 → 즉시 완료 마킹 (graceful fail).
 *   [3] state = SET_CC 로 초기화 (chain 의 첫 번째 단계 — get_cc_async 응답 대기).
 *   [4] get_cc 발행 → 응답 시 shutdown_get_cc_done 호출되어 RMW 진행.
 *
 * 실행 컨텍스트:
 *   detach API 호출자 스레드. ctrlr lock 은 caller(nvme_ctrlr_destruct/detach 경로) 에서
 *   이미 획득했다고 가정 — 본 함수는 별도 lock 작업 없음.
 *
 * 호출 체인:
 *   spdk_nvme_detach_async / nvme_ctrlr_destruct_async →
 *     nvme_ctrlr_shutdown_async (본 함수) →
 *       nvme_ctrlr_get_cc_async →
 *         (admin completion) nvme_ctrlr_shutdown_get_cc_done →
 *           nvme_ctrlr_set_cc_async →
 *             (admin completion) nvme_ctrlr_shutdown_set_cc_done →
 *               (poll loop) nvme_ctrlr_shutdown_poll_async ↔ nvme_ctrlr_shutdown_get_csts_done
 */
static void
nvme_ctrlr_shutdown_async(struct spdk_nvme_ctrlr *ctrlr,
			  struct nvme_ctrlr_detach_ctx *ctx)
{
	int rc;

	if (ctrlr->is_removed) {
		/* [한국어] 디바이스가 PCIe 슬롯에서 물리적으로 제거됨(hotplug remove)
		 *         또는 transport 가 영구 실패로 표기 — MMIO/admin 모두 무의미. */
		ctx->shutdown_complete = true;
		/* [한국어] shutdown 시도 skip 하고 즉시 완료 처리 — caller 는 cleanup 으로 진행. */
		return;
	}

	if (ctrlr->adminq == NULL ||
	    ctrlr->adminq->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE) {
		/* [한국어] adminq 가 아예 생성 안 됐거나(초기화 실패) transport 레벨 실패 마킹된 경우.
		 *         (transport_failure_reason 값:
		 *           SPDK_NVME_QPAIR_FAILURE_NONE = 정상,
		 *           NVME_QPAIR_FAILURE_LOCAL/REMOTE/UNKNOWN = 각각 호스트/타깃/원인불명 실패) */
		NVME_CTRLR_INFOLOG(ctrlr, "Adminq is not connected.\n");
		/* [한국어] INFO 레벨 — 정상 종료 시퀀스 중에도 발생 가능 (이미 disconnect 된 reset 후 등). */
		ctx->shutdown_complete = true;
		/* [한국어] admin 명령 발행 불가 → graceful 한 폴링 종료. */
		return;
	}

	ctx->state = NVME_CTRLR_DETACH_SET_CC;
	/* [한국어] state machine 초기 상태 = SET_CC (이름은 "CC 쓰기 대기" 의미).
	 *         poll_async 의 switch 에서 NVME_CTRLR_DETACH_SET_CC 분기는 "register op 진행 중"
	 *         으로 해석되어 -EAGAIN 으로 polling 계속. */
	rc = nvme_ctrlr_get_cc_async(ctrlr, nvme_ctrlr_shutdown_get_cc_done, ctx);
	/* [한국어] CC read 비동기 발행 — completion 시 get_cc_done 콜백이 RMW 의 R→M→W 단계 진행. */
	if (rc != 0) {
		/* [한국어] admin 큐 자원 부족·할당 실패 — chain 시작 자체가 실패. */
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CC register\n");
		ctx->shutdown_complete = true;
		/* [한국어] 폴링 루프 종료 신호 → caller 가 cleanup. */
	}
}

/*
 * [한국어]
 * nvme_ctrlr_shutdown_get_csts_done - shutdown chain 의 CSTS read 완료 콜백
 *
 * @_ctx:  nvme_ctrlr_detach_ctx
 * @value: 방금 읽은 CSTS(Controller Status) 32-bit 값
 * @cpl:   admin/MMIO read 결과 CPL
 *
 * 동기/배경:
 *   shutdown chain 의 마지막 폴링 단계. SHN write 후 컨트롤러가 SHST(Shutdown Status)
 *   필드를 SPDK_NVME_SHST_COMPLETE(=10b) 로 갱신할 때까지 CSTS 를 주기적으로 read 한다.
 *   본 함수는 한 번의 CSTS read 가 끝났을 때 호출되며, 결과를 ctx->csts 에 저장하고
 *   상태 머신을 GET_CSTS_DONE 으로 전이시킨다. 실제 SHST 비트 검사는
 *   다음 poll_async 호출에서 수행된다 (read 콜백과 평가 로직 분리 패턴).
 *
 *   NVMe Spec 1.x §3.1.6 Controller Status (CSTS):
 *     - bit 0    (RDY):      Ready
 *     - bit 1    (CFS):      Controller Fatal Status
 *     - bits 3:2 (SHST):     Shutdown Status — 00=Normal, 01=Occurring, 10=Complete
 *     - bit 4    (NSSRO):    NVM Subsystem Reset Occurred
 *     - bit 5    (PP):       Processing Paused
 *
 * 동작 단계:
 *   [1] CPL 에러 검사 — read 실패면 즉시 완료 마킹 후 종료.
 *   [2] 32-bit assert + raw 저장 (CC 와 동일한 패턴).
 *   [3] state = GET_CSTS_DONE — poll_async 가 다음 진입 시 CSTS 평가 분기로 진입.
 *
 * 실행 컨텍스트:
 *   admin completion 처리 컨텍스트. lock 추가 획득 없음.
 *
 * 호출 체인:
 *   nvme_ctrlr_shutdown_poll_async (CHECK_CSTS 분기) →
 *     nvme_ctrlr_get_csts_async →
 *       (admin completion) 본 콜백 →
 *         (다음 poll_async 호출) GET_CSTS_DONE 분기에서 SHST 평가
 */
static void
nvme_ctrlr_shutdown_get_csts_done(void *_ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr_detach_ctx *ctx = _ctx;
	/* [한국어] cb_arg 복원 — chain 내 다른 콜백과 동일 패턴. */

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* [한국어] CSTS read 실패 — admin queue/transport 문제 가능성. */
		NVME_CTRLR_ERRLOG(ctx->ctrlr, "Failed to read the CSTS register\n");
		ctx->shutdown_complete = true;
		/* [한국어] 더 이상 폴링 의미 없음 → 종료 마킹. */
		return;
	}

	assert(value <= UINT32_MAX);
	/* [한국어] CSTS 도 32-bit 레지스터 — 64-bit 컨테이너의 상위는 0 이어야 함. */
	ctx->csts.raw = (uint32_t)value;
	/* [한국어] raw 저장 → poll_async 가 다음 호출 시 csts.bits.shst 등을 평가. */
	ctx->state = NVME_CTRLR_DETACH_GET_CSTS_DONE;
	/* [한국어] 상태 전이: GET_CSTS(read 발행 후 대기) → GET_CSTS_DONE.
	 *         poll_async switch 에서 GET_CSTS_DONE 분기는 SHST 평가 로직으로 fall-through. */
}

/*
 * [한국어]
 * nvme_ctrlr_shutdown_poll_async - shutdown 진행 상태를 한 번 평가/진행 (반복 호출 대상)
 *
 * @ctrlr: shutdown 중인 컨트롤러
 * @ctx:   shutdown_async 가 초기화한 detach 컨텍스트
 * @return:
 *   - 0       : shutdown 완료 (성공 또는 timeout 으로 강제 종료) — caller 가 cleanup 진행 가능
 *   - -EAGAIN : 아직 진행 중 — caller 는 일정 간격 후 다시 호출해야 함
 *   - -EIO    : CSTS read 발행 실패 (admin 자원 문제)
 *   - -EINVAL : 알 수 없는 state (defensive)
 *
 * 동기/배경:
 *   shutdown chain 의 CSTS 폴링 루프 본체. shutdown_async 가 SET_CC →(콜백)→ CHECK_CSTS 까지
 *   상태를 진전시킨 후, caller 가 본 함수를 -EAGAIN 이 아닐 때까지 반복 호출한다.
 *
 *   본 함수의 역할은 두 가지로 명확히 분리된다:
 *     (A) state machine 진행: 직전 CSTS 결과가 도착했는지 확인하고, 필요시 다음 read 발행.
 *     (B) timeout 평가: ms_waited 와 shutdown_timeout_ms 비교.
 *
 * 동작 단계 (state 별):
 *   - SET_CC / GET_CSTS:
 *       이전 register operation 의 완료를 기다리는 중. process_completions 호출하여
 *       admin CQ 폴링 → -EAGAIN 반환. 콜백이 호출되면 state 가 다른 값으로 전이됨.
 *   - CHECK_CSTS:
 *       state = GET_CSTS 로 변경 후 새로운 CSTS read 발행.
 *       (shutdown_async 직후 set_cc_done 콜백이 CHECK_CSTS 로 세팅한 경우 진입.)
 *   - GET_CSTS_DONE:
 *       방금 도착한 csts 결과를 break 후 평가 단계로 fall-through.
 *       state = CHECK_CSTS 로 되돌려서 timeout 미달 시 다음 read 발행 가능.
 *   - default: assert (불가능한 상태).
 *
 *   평가 단계 (GET_CSTS_DONE 통과 후):
 *     [1] ms_waited 계산 — TSC tick 차이를 ms 로 변환.
 *     [2] csts.bits.shst == SHST_COMPLETE(=10b) 면 정상 종료 → return 0.
 *     [3] timeout 미만이면 -EAGAIN 으로 폴링 계속 (다음 호출에서 CHECK_CSTS 분기 진입).
 *     [4] timeout 초과면 ERRLOG + return 0 (강제 종료 — caller 가 후속 정리 진행).
 *
 * 실행 컨텍스트:
 *   detach 폴링 컨텍스트(보통 management thread). admin queue process_completions 호출하므로
 *   adminq 에 대한 single-thread 접근 불변량 유지가 필요 (호출자가 보장).
 *
 * 호출 체인:
 *   spdk_nvme_detach_async / spdk_nvme_detach_poll →
 *     nvme_ctrlr_shutdown_poll_async (본 함수) →
 *       (CHECK_CSTS) nvme_ctrlr_get_csts_async →
 *         (completion) nvme_ctrlr_shutdown_get_csts_done →
 *           (다음 호출) state=GET_CSTS_DONE 진입 → SHST 평가
 */
static int
nvme_ctrlr_shutdown_poll_async(struct spdk_nvme_ctrlr *ctrlr,
			       struct nvme_ctrlr_detach_ctx *ctx)
{
	union spdk_nvme_csts_register	csts;
	/* [한국어] CSTS 비트필드 union — .bits.shst, .bits.rdy 등으로 디코드. */
	uint32_t			ms_waited;
	/* [한국어] shutdown_start_tsc 부터 현재까지 경과 ms — timeout 비교용. */

	switch (ctx->state) {
	case NVME_CTRLR_DETACH_SET_CC:
	case NVME_CTRLR_DETACH_GET_CSTS:
		/* We're still waiting for the register operation to complete */
		/* [한국어] 두 상태 모두 "register 비동기 op 발행 후 콜백 대기 중" 의미.
		 *         - SET_CC:   shutdown_async 시작 직후 ~ get_cc_done 까지 / get_cc_done 후
		 *                     set_cc_async 발행 후 set_cc_done 까지 (이 사이 구간 모두 SET_CC).
		 *         - GET_CSTS: get_csts_async 발행 후 get_csts_done 까지. */
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		/* [한국어] admin CQ 를 한 번 폴링 — pending CPL 이 있으면 해당 콜백 실행 →
		 *         state 가 GET_CSTS_DONE / CHECK_CSTS 등으로 전이될 수 있음.
		 *         max_completions=0 → 모든 가용 completion 처리. */
		return -EAGAIN;
		/* [한국어] 아직 진행 중 — caller 는 다시 호출해야 함. */

	case NVME_CTRLR_DETACH_CHECK_CSTS:
		/* [한국어] "이제 새 CSTS read 를 발행할 차례" 상태.
		 *         set_cc_done 직후 또는 GET_CSTS_DONE 평가에서 timeout 미달 시 진입. */
		ctx->state = NVME_CTRLR_DETACH_GET_CSTS;
		/* [한국어] 즉시 GET_CSTS 로 전이 — 콜백 도착 전까지 SET_CC/GET_CSTS 분기에서 폴링. */
		if (nvme_ctrlr_get_csts_async(ctrlr, nvme_ctrlr_shutdown_get_csts_done, ctx)) {
			/* [한국어] CSTS read 비동기 발행 실패 — admin queue 자원 부족 등.
			 *         이 경우 state 는 GET_CSTS 로 남지만 콜백 호출이 없으므로 리턴값으로 종료 신호. */
			NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CSTS register\n");
			return -EIO;
		}
		return -EAGAIN;
		/* [한국어] read 발행 성공 — 콜백 도착까지 폴링 계속. */

	case NVME_CTRLR_DETACH_GET_CSTS_DONE:
		/* [한국어] CSTS read 콜백이 raw 값을 ctx->csts 에 저장한 직후 진입.
		 *         이번 호출에서 SHST 평가 후, timeout 미달이면 다시 read 발행 가능하도록
		 *         CHECK_CSTS 로 reset. */
		ctx->state = NVME_CTRLR_DETACH_CHECK_CSTS;
		break;
		/* [한국어] switch 탈출 → 아래 평가 로직으로 fall-through. */

	default:
		assert(0 && "Should never happen");
		/* [한국어] state 머신 외 값 — 메모리 손상 또는 enum 변경 미반영 의심. */
		return -EINVAL;
	}

	ms_waited = (spdk_get_ticks() - ctx->shutdown_start_tsc) * 1000 / spdk_get_ticks_hz();
	/* [한국어] 경과 시간 ms 계산: tick_diff × 1000 / ticks_per_sec.
	 *         start_tsc 는 set_cc_done 에서 SHN write 성공 직후 기록됨. */
	csts.raw = ctx->csts.raw;
	/* [한국어] 콜백이 저장한 마지막 CSTS raw 값을 union 에 적재 — bits 필드 디코드용. */

	if (csts.bits.shst == SPDK_NVME_SHST_COMPLETE) {
		/* [한국어] SHST(Shutdown Status, NVMe §3.1.6 bits 3:2) = 10b (Complete).
		 *         컨트롤러가 메타/캐시 flush 완료 → 안전하게 disconnect 가능. */
		NVME_CTRLR_DEBUGLOG(ctrlr, "shutdown complete in %u milliseconds\n", ms_waited);
		/* [한국어] 디버그 로깅 — 실제 소요 ms 기록 (RTD3E 와 비교 가능). */
		return 0;
		/* [한국어] 정상 종료 — caller 는 더 이상 폴링하지 않음. */
	}

	if (ms_waited < ctx->shutdown_timeout_ms) {
		/* [한국어] 아직 RTD3E 기반 timeout 안 지남 — 폴링 계속.
		 *         다음 호출에서 CHECK_CSTS 분기로 진입해 새 CSTS read 발행. */
		return -EAGAIN;
	}

	NVME_CTRLR_ERRLOG(ctrlr, "did not shutdown within %u milliseconds\n",
			  ctx->shutdown_timeout_ms);
	/* [한국어] timeout 초과 — 디바이스가 SHST=Complete 보고 안 함.
	 *         이 경우 강제로 진행 (return 0). caller 가 hot-remove 또는 fail 경로 처리. */
	if (ctrlr->quirks & NVME_QUIRK_SHST_COMPLETE) {
		/* [한국어] 알려진 quirk: VMware 가상 NVMe SSD 는 SHST=Complete 표기를 누락하는 경우가 있음.
		 *         operator 가 진단 시 혼란 줄이기 위한 힌트 메시지. */
		NVME_CTRLR_ERRLOG(ctrlr, "likely due to shutdown handling in the VMWare emulated NVMe SSD\n");
	}

	return 0;
	/* [한국어] timeout 이지만 caller 가 cleanup 으로 진행하도록 0 반환 (성공으로 간주). */
}

/*
 * [한국어]
 * nvme_ctrlr_get_ready_timeout - CAP.TO 필드로부터 enable/disable ready timeout(ms) 계산
 *
 * @ctrlr: 컨트롤러
 * @return: ready timeout (ms) — CSTS.RDY 비트가 기대 값으로 바뀌기까지 호스트가 기다려야 할 최대 시간
 *
 * 동기/배경:
 *   NVMe Spec 1.x §3.1.1 Controller Capabilities (CAP) — bits 31:24 (TO, Timeout):
 *     "Worst case time that host shall wait for CSTS.RDY to transition from 0 to 1
 *      after CC.EN transitions from 0 to 1, or from 1 to 0 after CC.EN 1→0."
 *     단위는 500ms 이다 (즉 CAP.TO * 500ms).
 *
 *   이 timeout 은 enable_async / disable_async chain 에서 ENABLE_WAIT_FOR_READY_1 /
 *   DISABLE_WAIT_FOR_READY_0 상태의 state_timeout_tsc 로 사용된다 — 초과 시 process_init 에서
 *   에러 처리.
 *
 * 동작: ctrlr->cap (READ_CAP 단계에서 한 번 읽어 캐시) 의 TO 비트필드에 500 곱셈만 수행.
 * 실행 컨텍스트: 모든 컨텍스트 (단순 산술, lock 불필요).
 * 호출처: nvme_ctrlr_set_cc_en_done, disable 경로 등.
 */
static inline uint64_t
nvme_ctrlr_get_ready_timeout(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cap.bits.to * 500;
	/* [한국어] CAP.TO (단위 500ms) → ms 환산. 예: TO=20 → 10000ms = 10초. */
}

/*
 * [한국어]
 * nvme_ctrlr_set_cc_en_done - CC.EN=1 write 완료 콜백 (enable 시퀀스 단계 전이)
 *
 * @ctx:   spdk_nvme_ctrlr* (cb_arg 로 ctrlr 자체를 전달)
 * @value: 방금 쓴 CC 값 (참고용)
 * @cpl:   write 결과
 *
 * 동기/배경:
 *   nvme_ctrlr_enable() 가 발행한 "CC.EN=1, CC.CSS/MPS/IOSQES/... 설정" admin write 의 완료
 *   시점에 호출된다. 이 시점에 디바이스는 SQ/CQ 메모리, 어드민 큐, 컨트롤러 내부 상태 등을
 *   초기화 시작 — 호스트는 CSTS.RDY 가 0→1 로 바뀔 때까지 기다려야 한다 (NVMe Spec §3.1.5
 *   "Initialization Sequence").
 *
 * 동작 단계:
 *   [1] CPL 에러면 state = ERROR (회복 불가) 로 전이하고 종료. INFINITE timeout 으로 setting —
 *       reset/cleanup 경로가 처리할 때까지 그대로 머무름.
 *   [2] 정상이면 state = ENABLE_WAIT_FOR_READY_1 로 전이. timeout 은 CAP.TO * 500ms (NVMe 스펙치).
 *       이후 process_init 의 다음 iteration 에서 CSTS read 를 발행해 RDY=1 확인.
 *
 * 실행 컨텍스트: admin completion 콜백.
 * 호출 체인:
 *   nvme_ctrlr_enable → nvme_ctrlr_set_cc_async (CC write 발행) →
 *     (admin completion) 본 콜백 → nvme_ctrlr_set_state(ENABLE_WAIT_FOR_READY_1) →
 *       (process_init 다음 iter) nvme_ctrlr_process_init_wait_for_ready_1
 */
static void
nvme_ctrlr_set_cc_en_done(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	/* [한국어] cb_arg 는 enable() 에서 ctrlr 자체로 설정 — 따라서 단순 캐스팅. */

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* [한국어] CC write 실패 — 디바이스 자체에 문제가 있어 enable 진행 불가. */
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to set the CC register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		/* [한국어] 상태머신을 ERROR 로 고정 — INFINITE 는 timeout 검사 비활성화 sentinel.
		 *         외부 reset 경로가 명시적으로 회복할 때까지 그대로. */
		return;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1,
			     nvme_ctrlr_get_ready_timeout(ctrlr));
	/* [한국어] 정상 경로: 다음 상태 = WAIT_FOR_READY_1 (CSTS.RDY=1 대기).
	 *         timeout 은 CAP.TO * 500ms — NVMe 가 보장하는 최대 ready 전이 시간. */
}

/*
 * [한국어]
 * nvme_ctrlr_enable - 컨트롤러 enable: CC 레지스터를 spec 에 맞게 구성하고 EN=1 write 발행
 *
 * @ctrlr: enable 할 컨트롤러 (이미 disabled / RDY=0 상태여야 함)
 * @return:
 *   - 0       : CC write 발행 성공 (실제 enable 완료는 set_cc_en_done 콜백 + RDY=1 대기)
 *   - -EINVAL : CC.EN 이 이미 1 이거나 arb_mechanism 이 디바이스 capability 와 불일치
 *   - -EIO    : nvme_ctrlr_set_cc_async 발행 실패
 *   - 기타    : nvme_transport_ctrlr_enable 의 transport-level 실패 코드
 *
 * 동기/배경:
 *   process_init 상태머신에서 NVME_CTRLR_STATE_ENABLE 진입 시 한 번 호출되는 함수.
 *   NVMe Spec §7.6.1 Initialization 의 "host shall configure CC and write CC.EN to 1" 절차를
 *   구현한다. 본 함수가 return 한 시점에는 CC write 가 발행만 된 상태 — 실제 활성화 완료는
 *   set_cc_en_done 콜백이 RDY=1 대기로 전이시킨 뒤 process_init 의 다음 iteration 에서 검증.
 *
 *   설정하는 CC 비트(NVMe Spec §3.1.5):
 *     EN=1            : 컨트롤러 활성화
 *     CSS             : Command Set Select (NVM / I/O Command Sets / Admin-only)
 *     SHN=0           : Shutdown Notification 클리어 (이전 shutdown 흔적 제거)
 *     IOSQES=6        : SQ entry size = 64 byte (2^6)
 *     IOCQES=4        : CQ entry size = 16 byte (2^4)
 *     MPS             : Memory Page Size (host page size) — 2^(12+mps)
 *     AMS             : Arbitration Mechanism Select (RR/WRR/Vendor-specific)
 *
 * 동작 단계:
 *   [1] transport-specific enable hook 호출 (PCIe: BAR 검증, Fabrics: 추가 properties 등).
 *   [2] process_init_cc 캐시(직전 read_cc 단계에서 저장)를 base 로 RMW 시작.
 *       만약 EN 이 이미 1 이면 호출 컨텍스트 버그 → -EINVAL.
 *   [3] EN=1, SHN=0, IOSQES/IOCQES/MPS 표준값 세팅.
 *   [4] CSS 결정:
 *         - 디바이스가 CAP.CSS=0 이면(spec 위반) NVM 으로 가정.
 *         - opts.command_set 이 sentinel(>=CHAR_BIT) 이면 IOCS > NVM > NOIO 우선순위로 자동 선택.
 *         - opts.command_set 이 CAP.CSS 에 포함 안 되면 NVM 으로 fallback.
 *   [5] AMS 검증: WRR/VS 옵션은 CAP.AMS 에 해당 비트가 있어야 사용 가능. 없으면 -EINVAL.
 *   [6] 최종 cc.raw 를 process_init_cc 에 백업 후 set_cc_async 발행 (cb=set_cc_en_done).
 *
 * 실행 컨텍스트:
 *   process_init 폴링 컨텍스트(보통 management thread). ctrlr lock 보유 가정.
 *
 * 호출 체인:
 *   nvme_ctrlr_process_init →
 *     nvme_ctrlr_enable (본 함수) →
 *       nvme_transport_ctrlr_enable (transport hook) +
 *       nvme_ctrlr_set_cc_async (CC write 발행) →
 *         (admin completion) nvme_ctrlr_set_cc_en_done →
 *           state = ENABLE_WAIT_FOR_READY_1
 */
static int
nvme_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register	cc;
	/* [한국어] CC 의 비트필드 union — 단계적으로 비트 세팅 후 raw 를 admin write 에 사용. */
	int				rc;

	rc = nvme_transport_ctrlr_enable(ctrlr);
	/* [한국어] transport 별 enable 훅 — PCIe 는 BAR/MMIO 검증, Fabrics 는 추가 property write 등.
	 *         실패 시 enable 자체 중단. */
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "transport ctrlr_enable failed\n");
		return rc;
	}

	cc.raw = ctrlr->process_init_cc.raw;
	/* [한국어] 직전 READ_CC 단계에서 디바이스로부터 읽어 캐시한 CC 값을 RMW base 로 사용.
	 *         이는 디바이스가 강제하는 reserved 비트들을 보존하기 위함. */
	if (cc.bits.en != 0) {
		/* [한국어] 호출 전제 조건 위반 — 호출자는 EN=0 인 disabled 상태에서 진입해야 함.
		 *         (CHECK_EN → SET_EN_0 → DISABLE_WAIT_FOR_READY_0 → DISABLED → ENABLE 순서) */
		NVME_CTRLR_ERRLOG(ctrlr, "called with CC.EN = 1\n");
		return -EINVAL;
	}

	cc.bits.en = 1;
	/* [한국어] EN(Enable, NVMe §3.1.5 bit 0)=1 — 본 write 가 디바이스 활성화 트리거. */
	cc.bits.css = 0;
	/* [한국어] CSS 임시 0 — 아래 [4] 단계에서 적절한 command set 결정 후 재설정. */
	cc.bits.shn = 0;
	/* [한국어] SHN(Shutdown Notification)=0 — 이전 shutdown 잔여 비트 클리어. */
	cc.bits.iosqes = 6; /* SQ entry size == 64 == 2^6 */
	/* [한국어] IOSQES(I/O SQ Entry Size, log2 byte)=6 → 64 byte. NVMe 표준 SQE 크기. */
	cc.bits.iocqes = 4; /* CQ entry size == 16 == 2^4 */
	/* [한국어] IOCQES(I/O CQ Entry Size, log2 byte)=4 → 16 byte. NVMe 표준 CQE 크기. */

	/* Page size is 2 ^ (12 + mps). */
	cc.bits.mps = spdk_u32log2(ctrlr->page_size) - 12;
	/* [한국어] MPS(Memory Page Size) 인코딩: log2(page_size) - 12.
	 *         예: page_size=4KB(2^12) → mps=0, 2MB hugepage(2^21) → mps=9.
	 *         디바이스가 이 값으로 PRP entry alignment 와 DMA buffer 단위를 결정. */

	/*
	 * Since NVMe 1.0, a controller should have at least one bit set in CAP.CSS.
	 * A controller that does not have any bit set in CAP.CSS is not spec compliant.
	 * Try to support such a controller regardless.
	 */
	if (ctrlr->cap.bits.css == 0) {
		/* [한국어] CAP.CSS(NVMe §3.1.1 bits 44:37) 가 0 인 디바이스는 spec 위반.
		 *         호환성 위해 NVM 만 지원한다고 가정하고 진행. */
		NVME_CTRLR_INFOLOG(ctrlr, "Drive reports no command sets supported. Assuming NVM is supported.\n");
		ctrlr->cap.bits.css = SPDK_NVME_CAP_CSS_NVM;
		/* [한국어] 캐시된 cap 값을 직접 수정 — 이후 검증 단계에서도 이 값 사용. */
	}

	/*
	 * If the user did not explicitly request a command set, or supplied a value larger than
	 * what can be saved in CC.CSS, use the most reasonable default.
	 */
	if (ctrlr->opts.command_set >= CHAR_BIT) {
		/* [한국어] opts.command_set 의 sentinel 값(CHAR_BIT=8 이상) — 사용자가 명시 안 함.
		 *         디바이스 capability 기반으로 자동 선택. */
		if (ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_IOCS) {
			/* [한국어] 1순위: I/O Command Sets (NVMe 1.4+ 의 ZNS, KV 등 다중 command set). */
			ctrlr->opts.command_set = SPDK_NVME_CC_CSS_IOCS;
		} else if (ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_NVM) {
			/* [한국어] 2순위: 표준 NVM (전통적 블록 스토리지). */
			ctrlr->opts.command_set = SPDK_NVME_CC_CSS_NVM;
		} else if (ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_NOIO) {
			/* Technically we should respond with CC_CSS_NOIO in
			 * this case, but we use NVM instead to work around
			 * buggy targets and to match Linux driver behavior.
			 */
			/* [한국어] 3순위: Admin Only (I/O 명령 미지원). spec 상 CC_CSS_NOIO(=111b) 가 정답이지만
			 *         일부 buggy 디바이스 호환성 + Linux 커널 드라이버 동작 모방을 위해 NVM 사용. */
			ctrlr->opts.command_set = SPDK_NVME_CC_CSS_NVM;
		} else {
			/* Invalid supported bits detected, falling back to NVM. */
			/* [한국어] CAP.CSS 에 알 수 없는 비트만 set — 안전하게 NVM 으로 fallback. */
			ctrlr->opts.command_set = SPDK_NVME_CC_CSS_NVM;
		}
	}

	/* Verify that the selected command set is supported by the controller. */
	if (!(ctrlr->cap.bits.css & (1u << ctrlr->opts.command_set))) {
		/* [한국어] 사용자가 명시한(또는 위에서 선택한) command_set 이 CAP.CSS 비트마스크에 없음.
		 *         (CC.CSS 는 인덱스, CAP.CSS 는 비트마스크 — `1u << command_set` 으로 변환 비교.) */
		NVME_CTRLR_DEBUGLOG(ctrlr, "Requested I/O command set %u but supported mask is 0x%x\n",
				    ctrlr->opts.command_set, ctrlr->cap.bits.css);
		NVME_CTRLR_DEBUGLOG(ctrlr, "Falling back to NVM. Assuming NVM is supported.\n");
		ctrlr->opts.command_set = SPDK_NVME_CC_CSS_NVM;
		/* [한국어] 안전한 fallback — 대부분 디바이스가 NVM 을 지원하므로. */
	}

	cc.bits.css = ctrlr->opts.command_set;
	/* [한국어] 최종 결정된 command set 인덱스를 CC.CSS 에 반영. */

	switch (ctrlr->opts.arb_mechanism) {
	case SPDK_NVME_CC_AMS_RR:
		/* [한국어] Round Robin — 모든 디바이스가 필수로 지원 (NVMe §4.11). 검증 불필요. */
		break;
	case SPDK_NVME_CC_AMS_WRR:
		if (SPDK_NVME_CAP_AMS_WRR & ctrlr->cap.bits.ams) {
			/* [한국어] Weighted Round Robin with Urgent Priority — CAP.AMS 비트 확인 필요. */
			break;
		}
		return -EINVAL;
		/* [한국어] 사용자가 WRR 요청했지만 디바이스 미지원 → enable 거부. */
	case SPDK_NVME_CC_AMS_VS:
		if (SPDK_NVME_CAP_AMS_VS & ctrlr->cap.bits.ams) {
			/* [한국어] Vendor Specific arbitration — CAP.AMS bit 확인. */
			break;
		}
		return -EINVAL;
	default:
		return -EINVAL;
		/* [한국어] 알 수 없는 arbitration 옵션 — 사용자 입력 오류. */
	}

	cc.bits.ams = ctrlr->opts.arb_mechanism;
	/* [한국어] 검증 통과한 AMS 값을 CC 에 적용. */
	ctrlr->process_init_cc.raw = cc.raw;
	/* [한국어] enable 시 사용한 최종 CC 값을 캐시에 백업 — 이후 reset/disable 시 RMW base 로 재사용. */

	if (nvme_ctrlr_set_cc_async(ctrlr, cc.raw, nvme_ctrlr_set_cc_en_done, ctrlr)) {
		/* [한국어] CC 에 EN=1 + 모든 설정 한 번에 write — admin/MMIO 비동기 발행.
		 *         완료 시 set_cc_en_done 콜백이 ENABLE_WAIT_FOR_READY_1 상태로 전이. */
		NVME_CTRLR_ERRLOG(ctrlr, "set_cc() failed\n");
		return -EIO;
	}

	return 0;
	/* [한국어] write 발행 성공 — caller(process_init) 는 다음 iteration 에서 콜백 결과 반영된
	 *         새 state 로 dispatch. */
}

/*
 * [한국어]
 * nvme_ctrlr_state_string - controller 상태머신의 모든 상태(40+)를 사람용 문자열로 변환
 *
 * @state:  nvme_ctrlr_state enum 값
 * @return: 상태 이름 문자열 (NULL 반환 안 함, 미정의 enum은 "unknown" — defensive)
 *
 * 동기/배경:
 *   nvme_ctrlr 의 상태머신(nvme_ctrlr_process_init() 가 dispatch) 은 40+ 상태를 가지며,
 *   각 상태가 의미하는 admin 명령/register 동작이 다르다. 디버깅·운영 시 enum 정수만으로는
 *   추적이 불가능하므로 모든 set_state 가 본 함수를 통해 사람-읽기 좋은 문자열로 변환해
 *   DEBUGLOG 에 출력한다 ("setting state to %s").
 *
 * 사용처:
 *   - _nvme_ctrlr_set_state: state 전이 시 디버그 로그.
 *   - nvme_ctrlr_process_init 의 timeout/error 로깅.
 *
 * 상태 그룹 분류 (process_init 진행 순서):
 *   [INIT 단계]:    INIT_DELAY → CONNECT_ADMINQ → WAIT_FOR_CONNECT_ADMINQ → READ_VS → READ_CAP
 *                  → CHECK_EN (CC.EN 현재 값 검사)
 *   [DISABLE 단계]: SET_EN_0 → DISABLE_WAIT_FOR_READY_0 → DISABLED
 *                  (CC.EN=1 이었으면 controller reset 부터 시작)
 *   [ENABLE 단계]:  ENABLE → ENABLE_WAIT_FOR_READY_1 → RESET_ADMIN_QUEUE
 *                  (CC.EN=1 write + CSTS.RDY=1 대기)
 *   [IDENTIFY 단계]:IDENTIFY → CONFIGURE_AER → SET_KEEP_ALIVE_TIMEOUT → IDENTIFY_IOCS_SPECIFIC
 *                  → GET_ZNS_CMD_EFFECTS_LOG → SET_NUM_QUEUES
 *   [NS DISCOVERY]: IDENTIFY_ACTIVE_NS → IDENTIFY_NS → IDENTIFY_ID_DESCS → IDENTIFY_NS_IOCS_SPECIFIC
 *   [FEATURES 단계]:SET_SUPPORTED_LOG_PAGES → SET_SUPPORTED_INTEL_LOG_PAGES → SET_SUPPORTED_FEATURES
 *                  → SET_HOST_FEATURE → SET_DB_BUF_CFG → SET_HOST_ID
 *   [최종]:         TRANSPORT_READY → READY (정상) / ERROR / DISCONNECTED
 *
 * "WAIT_FOR_*" 패턴:
 *   비동기 admin/register 명령을 발행한 후 응답 대기 중인 상태. process_init 매 호출마다
 *   admin queue 를 폴링하여 콜백 트리거 → 콜백이 다음 상태로 set_state. timeout 초과 시
 *   process_init 이 ERROR 상태로 강제 전이.
 *
 * 실행 컨텍스트: 모든 컨텍스트 (read-only, lock 불필요).
 */
static const char *
nvme_ctrlr_state_string(enum nvme_ctrlr_state state)
{
	switch (state) {
	case NVME_CTRLR_STATE_INIT_DELAY:
		return "delay init";
                                  /* [한국어] 일부 트랜스포트(VFIO 등)가 초기화 전 짧은 delay 필요 시 진입 */
	case NVME_CTRLR_STATE_CONNECT_ADMINQ:
		return "connect adminq";
                                  /* [한국어] admin qpair connect 시작 — Fabrics면 Connect 명령, PCIe는 SQ/CQ 등록 */
	case NVME_CTRLR_STATE_WAIT_FOR_CONNECT_ADMINQ:
		return "wait for connect adminq";
                                  /* [한국어] adminq connect 비동기 응답 대기 (특히 Fabrics RDMA/TCP 핸드셰이크) */
	case NVME_CTRLR_STATE_READ_VS:
		return "read vs";
                                  /* [한국어] VS(Version, NVMe §3.1.2) 레지스터 read 발행 — 디바이스 spec 버전 식별 */
	case NVME_CTRLR_STATE_READ_VS_WAIT_FOR_VS:
		return "read vs wait for vs";
                                  /* [한국어] VS read 응답 대기 */
	case NVME_CTRLR_STATE_READ_CAP:
		return "read cap";
                                  /* [한국어] CAP(Controller Capabilities, §3.1.1) read — MQES/TO/CSS 등 capability 캐시 */
	case NVME_CTRLR_STATE_READ_CAP_WAIT_FOR_CAP:
		return "read cap wait for cap";
                                  /* [한국어] CAP read 응답 대기 */
	case NVME_CTRLR_STATE_CHECK_EN:
		return "check en";
                                  /* [한국어] CC.EN 현재 값 read — 1이면 controller reset부터 시작, 0이면 바로 ENABLE */
	case NVME_CTRLR_STATE_CHECK_EN_WAIT_FOR_CC:
		return "check en wait for cc";
                                  /* [한국어] CC read 응답 대기 — 콜백이 EN 비트 검사 후 다음 상태 결정 */
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1:
		return "disable and wait for CSTS.RDY = 1";
                                  /* [한국어] reset 전 RDY=1 확인 단계 — CC.EN=1 이지만 RDY=0 이면 wait (NVMe §7.3) */
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS:
		return "disable and wait for CSTS.RDY = 1 reg";
                                  /* [한국어] CSTS read 응답 대기 (RDY=1 폴링용) */
	case NVME_CTRLR_STATE_SET_EN_0:
		return "set CC.EN = 0";
                                  /* [한국어] CC.EN=0 write 발행 — controller reset 트리거 (in-flight I/O 전부 abort) */
	case NVME_CTRLR_STATE_SET_EN_0_WAIT_FOR_CC:
		return "set CC.EN = 0 wait for cc";
                                  /* [한국어] EN=0 write 응답 대기 */
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0:
		return "disable and wait for CSTS.RDY = 0";
                                  /* [한국어] EN=0 write 후 RDY가 1→0으로 떨어지길 대기 (NVMe §7.3 controller reset 완료 신호) */
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0_WAIT_FOR_CSTS:
		return "disable and wait for CSTS.RDY = 0 reg";
                                  /* [한국어] CSTS read 응답 대기 (RDY=0 폴링용) */
	case NVME_CTRLR_STATE_DISABLED:
		return "controller is disabled";
                                  /* [한국어] reset 완료 — 이제 ENABLE 단계로 진행 가능 */
	case NVME_CTRLR_STATE_ENABLE:
		return "enable controller by writing CC.EN = 1";
                                  /* [한국어] nvme_ctrlr_enable() 진입 — CC 전체 설정 + EN=1 write 발행 */
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_CC:
		return "enable controller by writing CC.EN = 1 reg";
                                  /* [한국어] CC write 응답 대기 — set_cc_en_done 콜백 트리거 */
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1:
		return "wait for CSTS.RDY = 1";
                                  /* [한국어] EN=1 write 후 디바이스가 RDY=1 보고할 때까지 대기 (CAP.TO 시간 안에) */
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS:
		return "wait for CSTS.RDY = 1 reg";
                                  /* [한국어] CSTS read 응답 대기 (RDY=1 폴링용) */
	case NVME_CTRLR_STATE_RESET_ADMIN_QUEUE:
		return "reset admin queue";
                                  /* [한국어] enable 직후 admin queue 재초기화 — outstanding 명령 제거, 인덱스 리셋 */
	case NVME_CTRLR_STATE_IDENTIFY:
		return "identify controller";
                                  /* [한국어] Identify Controller (CNS=01h, NVMe §5.15) admin 명령 발행 — cdata 채움 */
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY:
		return "wait for identify controller";
                                  /* [한국어] Identify CPL 대기 */
	case NVME_CTRLR_STATE_CONFIGURE_AER:
		return "configure AER";
                                  /* [한국어] Set Features (FID=0Bh, AER) — 비동기 이벤트 마스크 설정 */
	case NVME_CTRLR_STATE_WAIT_FOR_CONFIGURE_AER:
		return "wait for configure aer";
                                  /* [한국어] AER 설정 CPL 대기 */
	case NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT:
		return "set keep alive timeout";
                                  /* [한국어] Set Features (FID=0Fh) — Fabrics 의 keep-alive heartbeat 주기 설정 */
	case NVME_CTRLR_STATE_WAIT_FOR_KEEP_ALIVE_TIMEOUT:
		return "wait for set keep alive timeout";
                                  /* [한국어] keep-alive 설정 CPL 대기 */
	case NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC:
		return "identify controller iocs specific";
                                  /* [한국어] Identify (CNS=06h) — I/O Command Set 별 specific 데이터 (예: ZNS controller data) */
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_IOCS_SPECIFIC:
		return "wait for identify controller iocs specific";
                                  /* [한국어] IOCS specific identify CPL 대기 */
	case NVME_CTRLR_STATE_GET_ZNS_CMD_EFFECTS_LOG:
		return "get zns cmd and effects log page";
                                  /* [한국어] Get Log Page (LID=05h, ZNS) — Zoned Namespace 명령별 effects bitmap */
	case NVME_CTRLR_STATE_WAIT_FOR_GET_ZNS_CMD_EFFECTS_LOG:
		return "wait for get zns cmd and effects log page";
                                  /* [한국어] ZNS log CPL 대기 */
	case NVME_CTRLR_STATE_SET_NUM_QUEUES:
		return "set number of queues";
                                  /* [한국어] Set Features (FID=07h, Number of Queues) — 호스트가 요청하는 IOQ 개수 협상 */
	case NVME_CTRLR_STATE_WAIT_FOR_SET_NUM_QUEUES:
		return "wait for set number of queues";
                                  /* [한국어] num_queues CPL 대기 — 응답값으로 실제 할당된 IOSQ/IOCQ 개수 결정 */
	case NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS:
		return "identify active ns";
                                  /* [한국어] Identify (CNS=02h) — 활성 namespace ID 리스트 가져오기 */
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ACTIVE_NS:
		return "wait for identify active ns";
                                  /* [한국어] active NS list CPL 대기 */
	case NVME_CTRLR_STATE_IDENTIFY_NS:
		return "identify ns";
                                  /* [한국어] Identify (CNS=00h) per-NS — LBA size, capacity, format 정보 */
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS:
		return "wait for identify ns";
                                  /* [한국어] per-NS identify CPL 대기 */
	case NVME_CTRLR_STATE_IDENTIFY_ID_DESCS:
		return "identify namespace id descriptors";
                                  /* [한국어] Identify (CNS=03h) — NSID descriptor (UUID, EUI64, NGUID) */
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ID_DESCS:
		return "wait for identify namespace id descriptors";
                                  /* [한국어] ID descriptors CPL 대기 */
	case NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC:
		return "identify ns iocs specific";
                                  /* [한국어] Identify (CNS=05h) per-NS — ZNS 등 command set 별 NS 데이터 */
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS_IOCS_SPECIFIC:
		return "wait for identify ns iocs specific";
                                  /* [한국어] per-NS IOCS specific CPL 대기 */
	case NVME_CTRLR_STATE_SET_SUPPORTED_LOG_PAGES:
		return "set supported log pages";
                                  /* [한국어] supported log pages 마스크 빌드 — 동기 단계 (CPL 대기 없음) */
	case NVME_CTRLR_STATE_SET_SUPPORTED_INTEL_LOG_PAGES:
		return "set supported INTEL log pages";
                                  /* [한국어] Intel vendor-specific log page 검사 (smart, latency 등) */
	case NVME_CTRLR_STATE_WAIT_FOR_SUPPORTED_INTEL_LOG_PAGES:
		return "wait for supported INTEL log pages";
                                  /* [한국어] Intel log page 응답 대기 */
	case NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES:
		return "set supported features";
                                  /* [한국어] Get Features 시도해 디바이스가 지원하는 feature ID 마스크 빌드 */
	case NVME_CTRLR_STATE_SET_HOST_FEATURE:
		return "set host behavior support feature";
                                  /* [한국어] Set Features (FID=16h, Host Behavior Support) — ACRE/LBAFEE 등 */
	case NVME_CTRLR_STATE_WAIT_FOR_SET_HOST_FEATURE:
		return "wait for set host behavior support feature";
                                  /* [한국어] host behavior CPL 대기 */
	case NVME_CTRLR_STATE_SET_DB_BUF_CFG:
		return "set doorbell buffer config";
                                  /* [한국어] Doorbell Buffer Config admin 명령 — shadow doorbell 활성화 (NVMe 1.3+) */
	case NVME_CTRLR_STATE_WAIT_FOR_DB_BUF_CFG:
		return "wait for doorbell buffer config";
                                  /* [한국어] doorbell buffer config CPL 대기 */
	case NVME_CTRLR_STATE_SET_HOST_ID:
		return "set host ID";
                                  /* [한국어] Set Features (FID=81h) — multi-host fabric 환경 호스트 식별자 */
	case NVME_CTRLR_STATE_WAIT_FOR_HOST_ID:
		return "wait for set host ID";
                                  /* [한국어] host ID CPL 대기 */
	case NVME_CTRLR_STATE_TRANSPORT_READY:
		return "transport ready";
                                  /* [한국어] 모든 admin 초기화 완료 — transport-specific ready 훅 호출 단계 */
	case NVME_CTRLR_STATE_READY:
		return "ready";
                                  /* [한국어] 정상 운영 상태 — I/O qpair 할당/제출 가능. process_init 종료 */
	case NVME_CTRLR_STATE_ERROR:
		return "error";
                                  /* [한국어] 회복 불가 에러 — 외부 reset 호출까지 대기, 모든 신규 I/O 거부 */
	case NVME_CTRLR_STATE_DISCONNECTED:
		return "disconnected";
                                  /* [한국어] transport 레벨 disconnect (Fabrics 핸드셰이크 실패 등) */
	}
	return "unknown";
                                  /* [한국어] enum 외 값 (메모리 손상 / enum 추가 후 case 누락) — defensive */
};

/*
 * [한국어]
 * _nvme_ctrlr_set_state - controller 상태머신 상태 전이 + timeout 설정 (내부 구현)
 *
 * @ctrlr:         대상 controller
 * @state:         새 상태
 * @timeout_in_ms: 다음 상태까지 허용 시간(ms). 특수 값:
 *                  - NVME_TIMEOUT_KEEP_EXISTING: 기존 timeout 유지 (재진입 케이스)
 *                  - NVME_TIMEOUT_INFINITE: 무한 대기 (fast-path 비-critical 단계)
 *                  - 그 외: ms → tick 변환 후 절대 시각 저장
 * @quiet:         true면 디버그 로그 출력 안 함 (반복 진입 시 로그 폭주 방지)
 *
 * 동작:
 *   [1] state 즉시 갱신.
 *   [2] KEEP_EXISTING이면 timeout_tsc 그대로 두고 return.
 *   [3] INFINITE이면 inf 라벨로 jump → state_timeout_tsc = NVME_TIMEOUT_INFINITE.
 *   [4] 일반 ms 값이면 overflow 검사 (ticks_per_ms 곱셈 + now_ticks 덧셈) →
 *       state_timeout_tsc = now + (ms × hz / 1000).
 *
 * Overflow 방어: ms × ticks_per_ms 또는 결과 + now_ticks가 UINT64_MAX 초과 시 INFINITE로 fall-through.
 *               48-bit ticks/sec 가정하면 ms는 최대 2^16 ms ≈ 65초까지 안전 (실제 사용은 훨씬 작음).
 *
 * 사용처: process_init의 모든 상태 전이 + reset/shutdown 시퀀스.
 */
static void
_nvme_ctrlr_set_state(struct spdk_nvme_ctrlr *ctrlr, enum nvme_ctrlr_state state,
		      uint64_t timeout_in_ms, bool quiet)
{
	uint64_t ticks_per_ms, timeout_in_ticks, now_ticks;

	ctrlr->state = state;
                                  /* [한국어] 상태 즉시 갱신 — 다음 process_init 진입 시 이 값으로 dispatch */
	if (timeout_in_ms == NVME_TIMEOUT_KEEP_EXISTING) {
                                  /* [한국어] 기존 timeout 유지 — WAIT_FOR_* 상태 진입 시 사용 (이미 timeout 설정됨) */
		if (!quiet) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "setting state to %s (keeping existing timeout)\n",
					    nvme_ctrlr_state_string(ctrlr->state));
		}
		return;
	}

	if (timeout_in_ms == NVME_TIMEOUT_INFINITE) {
		goto inf;
                                  /* [한국어] 명시적 무한 — fast-path 단계나 안 critical한 register access */
	}

	ticks_per_ms = spdk_get_ticks_hz() / 1000;
                                  /* [한국어] TSC tick → ms 변환 계수 (TSC 주파수 / 1000) */
	if (timeout_in_ms > UINT64_MAX / ticks_per_ms) {
                                  /* [한국어] 곱셈 overflow 검사 — ms × ticks_per_ms가 UINT64_MAX 초과 가능성 */
		NVME_CTRLR_ERRLOG(ctrlr,
				  "Specified timeout would cause integer overflow. Defaulting to no timeout.\n");
		goto inf;
                                  /* [한국어] overflow 위험 — INFINITE로 fall-through 안전 처리 */
	}

	now_ticks = spdk_get_ticks();
	timeout_in_ticks = timeout_in_ms * ticks_per_ms;
                                  /* [한국어] timeout을 tick 단위로 변환 */
	if (timeout_in_ticks > UINT64_MAX - now_ticks) {
                                  /* [한국어] 덧셈 overflow 검사 — now + ticks가 UINT64_MAX 넘는지 */
		NVME_CTRLR_ERRLOG(ctrlr,
				  "Specified timeout would cause integer overflow. Defaulting to no timeout.\n");
		goto inf;
	}

	ctrlr->state_timeout_tsc = timeout_in_ticks + now_ticks;
                                  /* [한국어] 절대 timeout 시각 저장 — process_init이 매 호출마다 spdk_get_ticks()와 비교 */
	if (!quiet) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "setting state to %s (timeout %" PRIu64 " ms)\n",
				    nvme_ctrlr_state_string(ctrlr->state), timeout_in_ms);
	}
	return;
inf:
	if (!quiet) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "setting state to %s (no timeout)\n",
				    nvme_ctrlr_state_string(ctrlr->state));
	}
	ctrlr->state_timeout_tsc = NVME_TIMEOUT_INFINITE;
                                  /* [한국어] sentinel 값 — process_init이 timeout 검사 skip */
}

/*
 * [한국어]
 * nvme_ctrlr_set_state - 상태 전이 + timeout 갱신 (DEBUGLOG 동반)
 *
 * @ctrlr        : 상태 전이 대상 컨트롤러. 호출자가 ctrlr_lock 보유 가정.
 * @state        : 전이할 새 상태(nvme_ctrlr_state enum, 40+ 상태 — DELAY/CONNECT_ADMINQ/
 *                 READ_VS/CHECK_EN/SET_EN_0/DISABLE_WAIT_FOR_READY_0/ENABLE/IDENTIFY/...
 *                 /TRANSPORT_READY/READY/ERROR/DISCONNECTED).
 * @timeout_in_ms: 새 상태에서 머물 수 있는 최대 시간(ms). 특수값 두 개:
 *                 - NVME_TIMEOUT_INFINITE  : 무한 대기(timeout 검사 비활성화).
 *                 - NVME_TIMEOUT_KEEP_EXISTING: 기존 state_timeout_tsc 유지(같은 폴링
 *                   루프를 돌면서 상태만 바꿀 때, timer 가 누적되지 않게).
 * @return: 없음.
 *
 * 동기/배경:
 *   nvme_ctrlr 의 진입(bring-up)·종료(disconnect)·재초기화(reset) 흐름은 모두
 *   nvme_ctrlr_process_init() 폴링 함수가 dispatch 하는 거대한 상태머신으로 모델링된다.
 *   본 함수는 그 상태 전이 시 단 하나의 진입점이며, "상태 + timeout" 한 쌍을 원자적으로
 *   갱신한다. 매 전이마다 DEBUGLOG 를 남겨 bring-up 흐름을 사후 추적할 수 있다.
 *
 * 동작 단계:
 *   [1] _nvme_ctrlr_set_state(... quiet=false) 로 위임.
 *   [2] 헬퍼는 ctrlr->state 즉시 갱신 + state_timeout_tsc 절대 시각 계산
 *       (now_ticks + ms * ticks_per_ms, overflow 시 INFINITE 로 fallback).
 *   [3] DEBUGLOG 에 "setting state to <name> (timeout N ms / no timeout)" 출력.
 *
 * 실행 컨텍스트:
 *   - process_init 폴링 컨텍스트(보통 management thread).
 *   - admin completion 콜백(예: set_cc_en_done, identify_cb 등).
 *   - 호출자는 항상 ctrlr_lock 보유 가정.
 *
 * 호출 체인:
 *   <상태머신 dispatch / admin completion 콜백> →
 *     [nvme_ctrlr_set_state] → _nvme_ctrlr_set_state → ctrlr->state, state_timeout_tsc 갱신
 */
static void
nvme_ctrlr_set_state(struct spdk_nvme_ctrlr *ctrlr, enum nvme_ctrlr_state state,
		     uint64_t timeout_in_ms)
{
	_nvme_ctrlr_set_state(ctrlr, state, timeout_in_ms, false);
	/* [한국어] quiet=false — 본 헬퍼는 "한 번만 진입하는" 정상 전이용으로 매 전이마다 DEBUGLOG.
	 *         bring-up·reset 시퀀스 추적에 필수 — 어떤 admin write/read 가 다음 상태를 트리거했는지
	 *         log 만 보고도 재구성할 수 있어야 함. */
}

/*
 * [한국어]
 * nvme_ctrlr_set_state_quiet - 상태 전이 + timeout 갱신 (로그 없는 조용한 버전)
 *
 * @ctrlr        : 상태 전이 대상 컨트롤러. ctrlr_lock 보유 가정.
 * @state        : 전이할 새 상태. 보통 set_state 와 동일한 상태로 재진입할 때 사용
 *                 (예: NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1 polling 자체 재진입).
 * @timeout_in_ms: 새 timeout(ms). KEEP_EXISTING/INFINITE 동일 의미.
 * @return: 없음.
 *
 * 동기/배경:
 *   상태머신 일부 단계는 admin write 응답이 늦거나, register polling 이 한 번에 끝나지
 *   않아 같은 상태로 수십~수백 번 재진입한다. 이때 set_state 처럼 매번 DEBUGLOG 를 남기면
 *   로그 파일이 폭주하고 진짜 전이가 묻혀 디버깅 가독성이 망가진다. quiet 버전은
 *   "상태/timeout 은 갱신하되 DEBUGLOG 는 남기지 않는다" 를 위한 변형이다.
 *
 *   대표 사용처:
 *     - WAIT_FOR_READY_1 / WAIT_FOR_READY_0 의 CSTS polling 재진입 (폴링 한 번이 끝나지
 *       않았으면 같은 상태 그대로 + timeout 만 KEEP_EXISTING 로 갱신).
 *     - admin completion 직전·직후의 "상태는 동일, 카운터만 갱신" 헬퍼들.
 *
 * 동작 단계:
 *   [1] _nvme_ctrlr_set_state(... quiet=true) 로 위임.
 *   [2] 헬퍼가 ctrlr->state, state_timeout_tsc 갱신.
 *   [3] DEBUGLOG 출력은 skip — 로그 폭주 방지.
 *
 * 실행 컨텍스트: process_init 폴링 컨텍스트, 또는 admin completion 콜백. ctrlr_lock 보유.
 *
 * 호출 체인:
 *   <상태머신 polling 헬퍼> → [nvme_ctrlr_set_state_quiet] → _nvme_ctrlr_set_state → 상태 갱신
 */
static void
nvme_ctrlr_set_state_quiet(struct spdk_nvme_ctrlr *ctrlr, enum nvme_ctrlr_state state,
			   uint64_t timeout_in_ms)
{
	_nvme_ctrlr_set_state(ctrlr, state, timeout_in_ms, true);
	/* [한국어] quiet=true — 같은 상태 반복 재진입 시에도 DEBUGLOG 출력 skip.
	 *         WAIT_FOR_* 폴링 재진입처럼 초당 수백 번 set_state 가 호출되는 경로에서
	 *         로그 폭주를 방지하고, "처음 진입한 set_state" 한 번만 보이게 하기 위함. */
}

/*
 * [한국어]
 * nvme_ctrlr_free_zns_specific_data - ZNS Identify Controller IOCS-specific 데이터 해제
 *
 * @ctrlr: 대상 컨트롤러. ctrlr_lock 보유 가정.
 * @return: 없음.
 *
 * 동기/배경:
 *   NVMe 1.4+ 부터 도입된 I/O Command Sets(IOCS) 메커니즘은 컨트롤러가 동시에 여러
 *   command set(NVM, ZNS, KV ...) 을 지원할 수 있게 한다. 각 command set 마다 별도의
 *   "Identify Controller IOCS-specific" 데이터(CSI=각 set 의 코드, NS=ctrlr) 가 있어,
 *   ZNS 의 경우 zone size, max active/open zones 등 zone-related capability 가 담긴다.
 *
 *   이 IOCS-specific 데이터는 process_init 의 IDENTIFY_IOCS_SPECIFIC 단계에서 디바이스로부터
 *   read 되어 ctrlr->cdata_zns 에 캐시된다. 컨트롤러 reset/disconnect 시점에는 이 캐시를
 *   "stale" 로 간주하고 폐기 — reset 후 디바이스가 capability 를 바꿨을 가능성이 있어
 *   다시 IDENTIFY_IOCS_SPECIFIC 을 거쳐 새로 받아야 한다.
 *
 * 동작 단계:
 *   [1] spdk_free(cdata_zns) — DPDK hugepage 메모리 반환 (NULL-safe).
 *   [2] 포인터 = NULL 로 dangling 방지.
 *
 * 실행 컨텍스트:
 *   - destruct 경로 (spdk_nvme_ctrlr_destruct 하위).
 *   - reset 경로 (nvme_ctrlr_disconnect_done 하위, IOCS-specific data invalidation).
 *   ctrlr_lock 보유.
 *
 * 호출 체인:
 *   nvme_ctrlr_disconnect_done / spdk_nvme_ctrlr_destruct →
 *     nvme_ctrlr_free_iocs_specific_data → [본 함수] → spdk_free
 */
static void
nvme_ctrlr_free_zns_specific_data(struct spdk_nvme_ctrlr *ctrlr)
{
	spdk_free(ctrlr->cdata_zns);
	/* [한국어] DPDK hugepage 영역에서 할당된 ZNS Identify Controller IOCS-specific
	 *         캐시 해제. spdk_free 는 NULL 인자 안전 — 미할당 상태에서도 호출 가능. */
	ctrlr->cdata_zns = NULL;
	/* [한국어] dangling 포인터 방지. 다음 IDENTIFY_IOCS_SPECIFIC 단계에서 재할당될 때까지
	 *         cdata_zns == NULL 가 "ZNS capability 미캐시" 의 sentinel 로 동작. */
}

/*
 * [한국어]
 * nvme_ctrlr_free_iocs_specific_data - 모든 IOCS-specific Identify 데이터 해제 (dispatcher)
 *
 * @ctrlr: 대상 컨트롤러. ctrlr_lock 보유 가정.
 * @return: 없음.
 *
 * 동기/배경:
 *   NVMe 1.4+ I/O Command Sets(IOCS) 는 NVM/ZNS/KV 등 여러 command set 을 동시 지원.
 *   각 set 마다 IOCS-specific Identify Controller 데이터를 별도 캐시한다.
 *   본 함수는 그 모든 IOCS 캐시를 한 번에 정리하는 dispatcher — 현재 SPDK 가 지원하는
 *   IOCS 가 ZNS 뿐이지만, 향후 KV 등 추가 시 여기에 free 호출 한 줄을 더하는 식으로
 *   확장한다.
 *
 *   호출 시점:
 *     - reset (disconnect_done): 캐시된 IOCS 데이터는 reset 시 stale 로 간주.
 *       reset 후 다시 IDENTIFY_IOCS_SPECIFIC 단계를 거쳐 새로 받음.
 *     - destruct: 컨트롤러 자체가 해제되므로 모든 IOCS 캐시도 free.
 *
 * 동작 단계:
 *   [1] ZNS 전용 캐시 free 위임 (nvme_ctrlr_free_zns_specific_data).
 *   [2] (향후) KV 등 다른 IOCS 가 추가되면 여기에 free 호출 추가.
 *
 * 실행 컨텍스트: destruct / reset 경로. ctrlr_lock 보유.
 *
 * 호출 체인:
 *   nvme_ctrlr_disconnect_done / spdk_nvme_ctrlr_destruct →
 *     [본 함수] → nvme_ctrlr_free_zns_specific_data → spdk_free
 */
static void
nvme_ctrlr_free_iocs_specific_data(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_ctrlr_free_zns_specific_data(ctrlr);
	/* [한국어] 현재 SPDK 가 캐시하는 IOCS-specific 데이터는 ZNS 뿐이므로 ZNS free 만 위임.
	 *         향후 KV/Computational Storage 등 다른 IOCS 가 추가되면, 그에 대응하는
	 *         free_<set>_specific_data 를 여기 한 줄씩 추가하는 패턴. */
}

/*
 * [한국어]
 * nvme_ctrlr_free_doorbell_buffer - Shadow Doorbell / EventIdx 버퍼 해제 (DBBUF spec)
 *
 * @ctrlr: 대상 컨트롤러. ctrlr_lock 보유 가정.
 * @return: 없음.
 *
 * 동기/배경:
 *   NVMe 1.3 §5.7 (Doorbell Buffer Config, DBBUF) 는 가상화 환경(특히 PCIe pass-through 가
 *   아닌 emulation) 에서 매 SQ tail / CQ head doorbell write 가 VM-exit 을 유발해
 *   매우 비싸다는 문제를 해결하기 위해 도입. 호스트가 doorbell 값을 RAM 의 두 페이지
 *   ("Shadow Doorbell Buffer" 와 "EventIdx Buffer") 에 미러링하고, 컨트롤러는 이 두 RAM
 *   영역을 polling 함으로써 MMIO write 자체를 줄이거나 생략할 수 있다.
 *
 *   - shadow_doorbell : 호스트가 갱신하는 SQ/CQ doorbell 값 ("이게 진짜 새 값") — 한 페이지.
 *   - eventidx        : 컨트롤러가 polling 시 사용하는 "마지막으로 본 doorbell 값" — 한 페이지.
 *                       호스트가 shadow != eventidx 가 되면 그제서야 진짜 MMIO write 한 번
 *                       (race-free 한 wake-up).
 *
 *   두 페이지 모두 DMA-capable hugepage 에서 할당 (spdk_zmalloc DMA|SHARE) 되며,
 *   doorbell_buffer_config admin 명령으로 PRP1/PRP2 = 두 페이지 물리주소를 디바이스에 등록.
 *
 *   호출 시점:
 *     - reset (disconnect_done): reset 후 디바이스 컨텍스트가 날아가므로 doorbell buffer 등록도
 *       무효 → 캐시 free 후 reset 완료 시 다시 SET_DB_BUF_CFG 단계에서 재등록.
 *     - destruct: 컨트롤러 정리 시 hugepage 반환.
 *
 * 동작 단계:
 *   [1] shadow_doorbell != NULL 이면 hugepage free + NULL 화.
 *   [2] eventidx != NULL 이면 hugepage free + NULL 화.
 *   (DBBUF 미사용 컨트롤러나 PCIe 외 transport 는 둘 다 NULL — no-op.)
 *
 * 실행 컨텍스트:
 *   - destruct 경로.
 *   - disconnect_done (reset 시 invalidate).
 *   ctrlr_lock 보유.
 *
 * 호출 체인:
 *   nvme_ctrlr_disconnect_done / nvme_ctrlr_destruct_async / set_doorbell_buffer_config error →
 *     [본 함수] → spdk_free × 2
 */
static void
nvme_ctrlr_free_doorbell_buffer(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->shadow_doorbell) {
		/* [한국어] DBBUF 가 활성화되어 shadow_doorbell 페이지가 할당된 경우만 해제.
		 *         NVMe 1.3 미만 / Fabrics / OACS.dbcs=0 디바이스에서는 NULL 인 상태로
		 *         이 분기 자체를 skip. */
		spdk_free(ctrlr->shadow_doorbell);
		/* [한국어] DPDK hugepage 영역(SPDK_MALLOC_DMA|SHARE) 에서 할당된 1 page 반환. */
		ctrlr->shadow_doorbell = NULL;
		/* [한국어] dangling 방지 — NULL 화 후 다음 SET_DB_BUF_CFG 단계에서 재할당. */
	}

	if (ctrlr->eventidx) {
		/* [한국어] EventIdx 페이지(컨트롤러가 polling 시 비교에 쓰는 "마지막 본 값")가
		 *         별도로 할당돼 있으면 함께 해제. shadow_doorbell 과 짝이 되어 항상
		 *         두 페이지가 같이 살거나 같이 죽음. */
		spdk_free(ctrlr->eventidx);
		/* [한국어] hugepage 1 page 반환. */
		ctrlr->eventidx = NULL;
		/* [한국어] dangling 방지. */
	}
}

/*
 * [한국어]
 * nvme_ctrlr_set_doorbell_buffer_config_done - Doorbell Buffer Config admin 응답 콜백
 *
 * @arg: cb_arg = 본 발행 시 등록된 ctrlr 포인터.
 * @cpl: admin CQE — 성공/실패 상태 코드 포함.
 * @return: 없음.
 *
 * 동기/배경:
 *   NVMe 1.3 §5.7 Doorbell Buffer Config 명령(opcode 0x7C)이 비동기 완료되면 호출.
 *   디바이스가 shadow_doorbell + eventidx 페이지의 PRP1/PRP2 를 등록하고 ACK 한 시점.
 *   이후로는 SQ/CQ doorbell write 가 RAM 페이지를 거쳐 polling 될 수 있음.
 *
 * 동작 단계:
 *   [1] CPL 에러면 WARN 로그만 — DBBUF 는 best-effort 최적화이므로 실패해도 컨트롤러는
 *       계속 사용 가능 (전통적 MMIO doorbell 로 fallback). reset 으로 가지 않음.
 *   [2] 성공이면 INFO 로그.
 *   [3] 어느 경우든 다음 상태 = SET_HOST_ID 로 전이 (process_init 진행 계속).
 *
 * 실행 컨텍스트: admin completion 콜백. ctrlr_lock 보유 (admin 처리 폴링 컨텍스트).
 *
 * 호출 체인:
 *   nvme_ctrlr_set_doorbell_buffer_config (admin 발행) →
 *     nvme_ctrlr_cmd_doorbell_buffer_config →
 *       (admin completion) [본 함수] →
 *         set_state(NVME_CTRLR_STATE_SET_HOST_ID)
 */
static void
nvme_ctrlr_set_doorbell_buffer_config_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;
	/* [한국어] cb_arg 로 등록된 ctrlr 복원 — admin 발행 시 본 함수와 ctrlr 페어로 등록. */

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* [한국어] DBBUF 등록 실패 — best-effort 기능이므로 전체 init 실패시키지 않고
		 *         WARN 만 남기고 진행. 디바이스는 일반 MMIO doorbell write 로 동작. */
		NVME_CTRLR_WARNLOG(ctrlr, "Doorbell buffer config failed\n");
	} else {
		/* [한국어] 성공 — 이후 SQ/CQ doorbell update 시 RAM 페이지 동기화 path 유효화. */
		NVME_CTRLR_INFOLOG(ctrlr, "Doorbell buffer config enabled\n");
	}
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_HOST_ID,
			     ctrlr->opts.admin_timeout_ms);
	/* [한국어] 성공/실패 무관: 다음 상태 = SET_HOST_ID 로 전이.
	 *         process_init 의 다음 iteration 이 set_host_id admin 발행. */
}

/*
 * [한국어]
 * nvme_ctrlr_set_doorbell_buffer_config - Shadow Doorbell 버퍼 두 페이지 할당 + 등록 admin 발행
 *
 * @ctrlr : 대상 컨트롤러. ctrlr_lock 보유 가정.
 * @return:
 *   - 0       : DBBUF 미지원/스킵 또는 admin 발행 성공 (실제 활성화는 *_done 콜백 시점).
 *   - -ENOMEM : hugepage 할당 실패.
 *   - -EFAULT : virt→phys 변환 실패 또는 페이지 경계 mismatch.
 *   - 기타    : nvme_ctrlr_cmd_doorbell_buffer_config 의 admin queue submission 에러.
 *
 * 동기/배경:
 *   process_init 상태머신의 NVME_CTRLR_STATE_SET_DB_BUF_CFG 진입 시 호출.
 *   NVMe 1.3 §5.7 Doorbell Buffer Config 명령으로 호스트가 두 hugepage 의 물리 주소
 *   (shadow_doorbell, eventidx) 를 디바이스에 알려준다. 디바이스는 이 RAM 영역을 polling
 *   하여 MMIO doorbell write 를 줄이거나 완전히 생략 가능 (특히 가상화 환경의 VM-exit
 *   비용 절감).
 *
 *   조건: OACS.dbcs(=Optional Admin Command Support, Doorbell Buffer Config bit) 가 1 이고
 *         transport 가 PCIe 인 경우만. Fabrics(RDMA/TCP) 는 doorbell 자체가 transport 메시지에
 *         피기백되므로 의미 없음.
 *
 * 동작 단계:
 *   [1] OACS.dbcs == 0 → DBBUF 미지원. 다음 상태 = SET_HOST_ID 로 skip.
 *   [2] transport != PCIe → DBBUF 의미 없음. 동일하게 skip.
 *   [3] shadow_doorbell hugepage 1 page 할당 (DMA|SHARE).
 *   [4] virt→phys (PRP1) 추출, 페이지 경계 체크 (vtophys 가 page_size 보다 작은 contiguous
 *       범위 반환할 가능성 — 페이지 단위 보장 검증).
 *   [5] eventidx hugepage 1 page 할당 + PRP2 추출 + 검증.
 *   [6] 상태 = WAIT_FOR_DB_BUF_CFG 로 전이 후 doorbell_buffer_config admin 발행
 *       (cb=set_doorbell_buffer_config_done).
 *   [7] 에러 시 ERROR 상태로 전이 + 부분 할당된 페이지 정리.
 *
 * 실행 컨텍스트: process_init 폴링 컨텍스트. ctrlr_lock 보유.
 *
 * 호출 체인:
 *   nvme_ctrlr_process_init (state=SET_DB_BUF_CFG) →
 *     [본 함수] →
 *       nvme_ctrlr_cmd_doorbell_buffer_config (admin 발행) →
 *         (completion) nvme_ctrlr_set_doorbell_buffer_config_done →
 *           set_state(SET_HOST_ID)
 */
static int
nvme_ctrlr_set_doorbell_buffer_config(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	/* [한국어] 반환 코드 — 0=성공/skip, 음수=오류. error 라벨에서 free 후 반환. */
	uint64_t prp1, prp2, len;
	/* [한국어] prp1/prp2 = shadow_doorbell/eventidx 의 물리주소(NVMe DBBUF 명령의 PRP 슬롯).
	 *         len = vtophys 에서 contiguous 범위 검사용. */

	if (!ctrlr->cdata.oacs.dbcs) {
		/* [한국어] OACS.DBCS(NVMe §5.21 Identify Controller, Optional Admin Command Support
		 *         bit "Doorbell Buffer Config Supported")=0 이면 디바이스 미지원.
		 *         조용히 skip 하고 다음 init 단계로 진행. */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_HOST_ID,
				     ctrlr->opts.admin_timeout_ms);
		/* [한국어] 다음 상태 = SET_HOST_ID — DBBUF skip 시에도 init flow 깨지지 않게. */
		return 0;
	}

	if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		/* [한국어] DBBUF 는 PCIe MMIO doorbell 비용 절감용 — Fabrics(RDMA/TCP/FC) 에선
		 *         doorbell 이 transport 메시지로 전달되므로 본 최적화가 무의미.
		 *         transport != PCIe 면 skip. */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_HOST_ID,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	/* only 1 page size for doorbell buffer */
	ctrlr->shadow_doorbell = spdk_zmalloc(ctrlr->page_size, ctrlr->page_size,
					      NULL, SPDK_ENV_LCORE_ID_ANY,
					      SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
	/* [한국어] Shadow Doorbell 버퍼 1 page 를 hugepage 에서 할당.
	 *         - size=page_size, align=page_size : NVMe DBBUF 가 PRP entry 로 받으므로
	 *           반드시 page-aligned 여야 함.
	 *         - SPDK_MALLOC_DMA  : 디바이스가 DMA 로 polling 하므로 IOMMU/물리연속 보장 필요.
	 *         - SPDK_MALLOC_SHARE: multi-process 시 secondary process 도 매핑 가능.
	 *         - LCORE_ANY        : 특정 NUMA node 강제 안 함. */
	if (ctrlr->shadow_doorbell == NULL) {
		/* [한국어] hugepage 부족 — DPDK 메모리 풀 고갈 또는 page 단편화. */
		rc = -ENOMEM;
		goto error;
	}

	len = ctrlr->page_size;
	/* [한국어] vtophys 에 input/output: in=찾고 싶은 contiguous 길이, out=실제 contiguous 길이. */
	prp1 = spdk_vtophys(ctrlr->shadow_doorbell, &len);
	/* [한국어] 가상주소 → 물리(또는 IOVA) 변환. DPDK hugepage 에서 받은 페이지는 보통 page
	 *         경계로 contiguous 보장되지만, 안전을 위해 직접 검증. */
	if (prp1 == SPDK_VTOPHYS_ERROR || len != ctrlr->page_size) {
		/* [한국어] vtophys 실패(IOMMU 미설정 등) 또는 contiguous 길이가 한 페이지 미만 —
		 *         page-aligned PRP 보장 안 되므로 실패 처리. */
		rc = -EFAULT;
		goto error;
	}

	ctrlr->eventidx = spdk_zmalloc(ctrlr->page_size, ctrlr->page_size,
				       NULL, SPDK_ENV_LCORE_ID_ANY,
				       SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
	/* [한국어] EventIdx 버퍼 1 page 를 동일 조건으로 할당. shadow_doorbell 과 짝이 되어
	 *         항상 둘 다 활성. */
	if (ctrlr->eventidx == NULL) {
		rc = -ENOMEM;
		goto error;
	}

	len = ctrlr->page_size;
	prp2 = spdk_vtophys(ctrlr->eventidx, &len);
	/* [한국어] EventIdx 의 물리주소 추출 — DBBUF 명령의 PRP2 로 사용. */
	if (prp2 == SPDK_VTOPHYS_ERROR || len != ctrlr->page_size) {
		rc = -EFAULT;
		goto error;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_DB_BUF_CFG,
			     ctrlr->opts.admin_timeout_ms);
	/* [한국어] admin 발행 직전 상태 전이 — 이후 *_done 콜백이 SET_HOST_ID 로 전이.
	 *         발행 전에 미리 WAIT 상태로 둬야 race 없이 timeout 추적 가능. */

	rc = nvme_ctrlr_cmd_doorbell_buffer_config(ctrlr, prp1, prp2,
			nvme_ctrlr_set_doorbell_buffer_config_done, ctrlr);
	/* [한국어] NVMe Spec §5.7 Doorbell Buffer Config (opcode 0x7C) admin 발행.
	 *         PRP1=Shadow Doorbell phys, PRP2=EventIdx phys. 응답은 *_done 콜백. */
	if (rc != 0) {
		/* [한국어] admin queue submission 자체 실패 — admin 슬롯 고갈/qpair 비정상 등. */
		goto error;
	}

	return 0;
	/* [한국어] 발행 성공. 실제 활성화는 콜백 시점. */

error:
	/* [한국어] 모든 실패 경로의 공통 cleanup. ERROR 상태로 영구 전이 후 부분 할당된 페이지 정리. */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	/* [한국어] DBBUF 자체는 best-effort 지만, 메모리 할당/vtophys 실패는 시스템 레벨 문제 —
	 *         ERROR 로 전이해 외부 reset 으로 복구. INFINITE = timeout 검사 disable. */
	nvme_ctrlr_free_doorbell_buffer(ctrlr);
	/* [한국어] 부분 성공한 페이지(shadow 할당, eventidx 실패 케이스 등) 회수. NULL-safe. */
	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_abort_queued_aborts - reset 시점에 admin queue 에 미발행 대기 중인 Abort 요청 정리
 *
 * @ctrlr: 대상 컨트롤러. ctrlr_lock 보유 가정.
 * @return: 없음.
 *
 * 동기/배경:
 *   NVMe Spec §5.1 (Admin Command Set), Abort 명령(opcode 0x08)은 진행 중인 다른 명령을
 *   취소 요청한다. 그러나 디바이스가 동시에 처리 가능한 abort 수에는 제한이 있어
 *   (Identify Controller cdata.acl + 1, "Abort Command Limit"), SPDK 는 이 한도를 넘는
 *   abort 요청을 admin queue 에 직접 발행하지 않고 ctrlr->queued_aborts STAILQ 에
 *   대기시킨다 (outstanding_aborts < acl 가 되면 하나씩 발행).
 *
 *   reset (disconnect) 시점에는 admin queue 가 폐기되므로 이 대기 큐도 비워야 한다 —
 *   대기 중인 abort 들은 "발행 자체를 못 했으니" 디바이스에는 영향이 없지만, 호출자에게
 *   "취소됨(SC_ABORTED_SQ_DELETION)" 을 통보해야 호출자의 cb_fn 이 정리된다.
 *
 *   상태 코드 선택: NVMe spec 의 SC=SQ Deletion (Generic SCT) — "큐가 사라져 명령이
 *   처리되지 못했다" 의 표준 코드. 실제로 SQ deletion 이 일어나지 않더라도 reset 시
 *   호스트가 동일 의미로 사용하는 관행.
 *
 * 동작 단계:
 *   [1] 모든 cb 에 전달할 공통 CPL 을 zero 초기화 후 SC/SCT 세팅.
 *   [2] STAILQ 를 SAFE 순회하면서:
 *       - HEAD 에서 빼내고 outstanding_aborts++ (다음 [3] 의 nvme_complete_request 가 abort
 *         완료 콜백으로 outstanding_aborts-- 하므로 여기서 ++ 해 균형 유지).
 *       - nvme_complete_request 로 호출자 cb_fn 호출 → 호출자 입장에서는 "abort 가
 *         실패(SQ Deletion)" 로 보임.
 *
 * 실행 컨텍스트:
 *   - nvme_ctrlr_disconnect 시작 시 (reset chain 의 첫 정리 step).
 *   - destruct 시 (남아있는 큐 비우기).
 *   ctrlr_lock 보유 — STAILQ 접근 동기화 책임은 caller.
 *
 * 호출 체인:
 *   nvme_ctrlr_disconnect (reset 시작) →
 *     [본 함수] →
 *       nvme_complete_request × N (cb 통보) → outstanding_aborts 카운터 일관 유지
 */
void
nvme_ctrlr_abort_queued_aborts(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_request	*req, *tmp;
	/* [한국어] STAILQ_FOREACH_SAFE 용 — req=현재, tmp=다음. SAFE 매크로는 현재 노드를
	 *         REMOVE 해도 다음 노드를 잃지 않게 미리 백업. */
	struct spdk_nvme_cpl	cpl = {};
	/* [한국어] 모든 취소 통보에 공통으로 사용할 가짜 CQE. zero 초기화로 status 외 필드 0. */

	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	/* [한국어] Status Code = "Aborted - Submission Queue Deletion" (NVMe §4.6.1.2.1, Generic SC).
	 *         "큐가 삭제되어 명령이 폐기됨" — reset/disconnect 시 호스트 측에서 사용하는 표준 코드. */
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	/* [한국어] Status Code Type = Generic Command Status (NVMe §4.6.1, type=0). */

	STAILQ_FOREACH_SAFE(req, &ctrlr->queued_aborts, stailq, tmp) {
		/* [한국어] 대기 중인 abort 요청 리스트 순회. SAFE 형 — 본문에서 req 를 큐에서
		 *         빼내(REMOVE_HEAD)고 free 해도 tmp 가 살아있어 다음 iteration 안전. */
		STAILQ_REMOVE_HEAD(&ctrlr->queued_aborts, stailq);
		/* [한국어] HEAD 에서 빼내기 — req 는 항상 현재 HEAD 이므로 REMOVE_HEAD 가 안전·O(1). */
		ctrlr->outstanding_aborts++;
		/* [한국어] 카운터 균형 — 아래 nvme_complete_request 안에서 abort completion 콜백이
		 *         outstanding_aborts-- 를 수행하므로, 여기서 미리 ++ 해야 0 으로 수렴.
		 *         (정상 발행 경로에서는 발행 시 ++, 완료 시 --. 본 경로는 발행 단계를
		 *         건너뛰고 곧장 완료 시뮬레이션이라 ++ 를 직접 해줘야 함.) */

		nvme_complete_request(req->cb_fn, req->cb_arg, req->qpair, req, &cpl);
		/* [한국어] 호출자 cb 에 "Aborted - SQ Deletion" CQE 로 통보.
		 *         req 는 mempool 로 반환됨. 호출자는 abort 가 거부된 것으로 인식하고
		 *         자체 정리. */
	}
}

/*
 * [한국어]
 * nvme_ctrlr_disconnect - 컨트롤러 disconnect 시작점 (reset chain 진입의 첫 단계)
 *
 * @ctrlr: disconnect 대상 컨트롤러. 호출자가 ctrlr_lock 보유 가정.
 * @return:
 *   - 0      : disconnect 시퀀스 시작 성공. 호출자는 이후 reset_poll 로 완료 폴링.
 *   - -EBUSY : 이미 reset 중 (is_resetting=true) — 중복 reset 거부.
 *   - -ENXIO : 컨트롤러가 hot-removed 됨 (is_removed=true) — 더 이상 reset 의미 없음.
 *
 * 동기/배경:
 *   NVMe Spec §7.3 Reset Processing 의 "Controller Reset" 흐름 중 disconnect 단계.
 *   사용자가 spdk_nvme_ctrlr_reset / spdk_nvme_ctrlr_disconnect 를 호출했거나,
 *   I/O 에러/AER fatal 등으로 컨트롤러가 자동 reset 되어야 할 때 진입.
 *
 *   reset 은 "disconnect → (사용자 폴링) → reconnect" 의 비동기 2-step:
 *     1) disconnect : 본 함수 — admin/io qpair 끊기, 진행 중 명령 모두 abort, hardware
 *                     레벨에서 controller 와의 connection 단절.
 *     2) reconnect  : spdk_nvme_ctrlr_reconnect_async (state=INIT 으로 되돌림) →
 *                     reset_poll_async 가 process_init 을 다시 돌려 활성 상태로 복원.
 *
 * 동작 단계:
 *   [1] 이미 reset 중(is_resetting) 또는 removed(is_removed) 이면 즉시 return — 중복 방지.
 *   [2] 플래그 set:
 *       - is_resetting=true   : 다른 reset 진입 차단.
 *       - is_failed=false     : reset 시작 시 failed 플래그 클리어 (재시도 의미).
 *       - is_disconnecting=true: disconnect_done 까지의 transient 상태.
 *       - prepare_for_reset=true: I/O qpair 들이 새 명령 발행 멈추도록 신호.
 *   [3] keep_alive interval=0 — keep-alive admin 명령 재발행 중단 (reset 후 재초기화 시 복구).
 *   [4] queued_aborts 큐의 모든 대기 abort 를 SC=SQ_DELETION 으로 통보 후 폐기.
 *   [5] 진행 중 AER(Asynchronous Event Request) 들을 transport hook 으로 abort.
 *   [6] adminq 의 transport_failure_reason = LOCAL — "호스트 측이 끊는다" 표시.
 *   [7] transport-specific disconnect_qpair(adminq) — PCIe 면 SQ/CQ delete + admin SQ disable,
 *       Fabrics 면 transport connection 종료.
 *
 *   이후 사용자(또는 polling 루프)가 reset 를 진행하면 disconnect_done → DISCONNECTED 상태
 *   → reconnect_async(INIT) → process_init 이 활성화 시퀀스 재실행.
 *
 * 실행 컨텍스트:
 *   - 사용자 호출 (spdk_nvme_ctrlr_disconnect / spdk_nvme_ctrlr_reset 의 일부).
 *   - 자동 reset 경로 (예: AER fatal 또는 transport timeout 시 management thread).
 *   ctrlr_lock 보유.
 *
 * 호출 체인:
 *   spdk_nvme_ctrlr_disconnect / spdk_nvme_ctrlr_reset →
 *     [nvme_ctrlr_disconnect] →
 *       nvme_ctrlr_abort_queued_aborts (대기 abort 정리) +
 *       nvme_transport_admin_qpair_abort_aers (진행 중 AER abort) +
 *       nvme_transport_ctrlr_disconnect_qpair (transport 레벨 disconnect)
 *     ... → (poll) nvme_ctrlr_disconnect_done → state=DISCONNECTED →
 *     spdk_nvme_ctrlr_reconnect_async → state=INIT → process_init 재실행
 */
static int
nvme_ctrlr_disconnect(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->is_resetting || ctrlr->is_removed) {
		/*
		 * Controller is already resetting or has been removed. Return
		 *  immediately since there is no need to kick off another
		 *  reset in these cases.
		 */
		/* [한국어] 두 sentinel 의미:
		 *   - is_resetting : 이미 다른 호출자가 reset 진행 중 → 중복 reset 시작 안 됨.
		 *     EBUSY 반환으로 caller 에게 "잠시 후 다시" 신호.
		 *   - is_removed   : PCIe surprise removal 이나 명시적 remove 후 — controller
		 *     자체가 의미 없음. ENXIO 로 영구 실패 신호. */
		return ctrlr->is_resetting ? -EBUSY : -ENXIO;
	}

	ctrlr->is_resetting = true;
	/* [한국어] reset 진입 마커 — 다른 reset/disconnect 를 EBUSY 로 거부. disconnect_done 후
	 *         이어지는 reconnect_async 가 process_init 마지막에 false 로 클리어. */
	ctrlr->is_failed = false;
	/* [한국어] reset 자체가 회복 시도이므로 failed 플래그 클리어. 이전 fatal 표시 제거. */
	ctrlr->is_disconnecting = true;
	/* [한국어] disconnect 진행 중 transient 상태 — disconnect_done 에서 false 로 토글.
	 *         외부에서 "지금이 disconnect-only 상태인가" 를 확인하는 용도. */
	ctrlr->prepare_for_reset = true;
	/* [한국어] I/O qpair poll 들이 본 플래그를 보고 "더 이상 새 I/O 발행 금지" 모드 진입.
	 *         이미 in-flight 인 I/O 는 transport disconnect 에서 abort 됨. */

	NVME_CTRLR_NOTICELOG(ctrlr, "resetting controller\n");
	/* [한국어] 운영 가시성 — reset 시작 시점을 NOTICE 레벨로 명시 (운영 로그에서 잘 보이게). */

	/* Disable keep-alive, it'll be re-enabled as part of the init process */
	ctrlr->keep_alive_interval_ticks = 0;
	/* [한국어] Keep-Alive admin 자동 재발행 disable — disconnect 중에는 admin queue 자체가
	 *         사라지므로 발행 시도가 무의미. process_init 의 SET_KEEP_ALIVE_TIMEOUT 단계에서
	 *         재설정됨. */

	/* Abort all of the queued abort requests */
	nvme_ctrlr_abort_queued_aborts(ctrlr);
	/* [한국어] 발행 대기 중이던 abort 요청들을 SC=SQ_DELETION 으로 호출자에게 통보 후 폐기.
	 *         admin queue 가 곧 disconnect 되므로 "발행될 일이 없음". */

	nvme_transport_admin_qpair_abort_aers(ctrlr->adminq);
	/* [한국어] 진행 중인 AER(Asynchronous Event Request, NVMe §5.2 Spec) 들을 transport hook
	 *         으로 abort. AER 은 디바이스가 비동기 이벤트(temperature, error, NS change 등)를
	 *         호스트에 통지할 때 쓰는 long-poll admin 명령. reset 시 모두 회수. */

	ctrlr->adminq->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
	/* [한국어] qpair 가 죽은 이유를 "LOCAL"(호스트 측에서 끊음) 로 명시 — transport
	 *         disconnect 가 in-flight 명령을 fail-completion 처리할 때 이 값을 status 로
	 *         사용. 디바이스 fault 와 호스트 reset 을 구분하는 단서. */
	nvme_transport_ctrlr_disconnect_qpair(ctrlr, ctrlr->adminq);
	/* [한국어] transport-specific 실제 disconnect — PCIe: admin SQ/CQ delete + EN=0 까지
	 *         밀어 controller stop, Fabrics: transport connection close (TCP socket
	 *         close / RDMA QP destroy). 이 호출 후 adminq 는 사용 불가 상태. */

	return 0;
	/* [한국어] disconnect 시퀀스 시작 성공. 실제 완료(disconnect_done) 는 reset_poll_async
	 *         또는 process_init 의 polling 진행에서 감지·호출됨. */
}

/*
 * [한국어]
 * nvme_ctrlr_disconnect_done - disconnect 완료 처리: 캐시 invalidate + 상태=DISCONNECTED
 *
 * @ctrlr: disconnect 완료된 컨트롤러. ctrlr_lock 보유 가정.
 * @return: 없음.
 *
 * 동기/배경:
 *   nvme_ctrlr_disconnect 가 시작한 disconnect 시퀀스의 종결 함수. transport-level
 *   disconnect_qpair 가 실제로 끝났음(I/O 모두 abort, qpair 자원 회수)이 process_init 의
 *   폴링 또는 reset_poll_async 에서 확인되면 호출된다.
 *
 *   이 시점에 컨트롤러는 "전기적으로는 살아 있을 수 있지만 SPDK 측 컨텍스트는 모두
 *   stale" 인 상태. 따라서 reset 후 디바이스가 다시 줄 수 있는 모든 정보(IOCS-specific
 *   identify, doorbell 등록 정보, free I/O queue ID bitmap)를 폐기하고, 상태머신을
 *   DISCONNECTED 로 되돌려 reconnect 진입을 기다린다.
 *
 * 동작 단계:
 *   [1] is_failed == false 검증 (assert) — disconnect_done 은 정상 disconnect 종료의
 *       "정리" 함수이므로, fatal failure 와는 다른 경로.
 *   [2] is_disconnecting=false — disconnect 시퀀스 종료 신호.
 *   [3] Doorbell Buffer Config 캐시 free — reset 후 재등록 필요.
 *   [4] IOCS-specific identify 데이터(ZNS 등) 캐시 free — reset 후 재read 필요.
 *   [5] free_io_qids bitmap free — I/O queue ID 풀 무효화 (reset 후 재할당).
 *   [6] state=DISCONNECTED + INFINITE timeout — reconnect_async 가 INIT 으로 전이시킬 때까지
 *       대기.
 *
 * 실행 컨텍스트:
 *   - process_init / reset_poll_async polling 컨텍스트.
 *   - ctrlr_lock 보유.
 *
 * 호출 체인:
 *   nvme_ctrlr_disconnect (시작) → ... transport disconnect 진행 ... →
 *     [본 함수] → state=DISCONNECTED →
 *       (호출자가 폴링 루프에서 감지) →
 *         spdk_nvme_ctrlr_reconnect_async → state=INIT →
 *           nvme_ctrlr_process_init 재실행 (CONNECT_ADMINQ → ... → READY)
 */
static void
nvme_ctrlr_disconnect_done(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->is_failed == false);
	/* [한국어] 본 함수는 "정상 disconnect 종료" 경로. is_failed=true 이면 fatal failure
	 *         경로(별도 정리)를 타야 하므로 디버그 빌드에서 trip. */
	ctrlr->is_disconnecting = false;
	/* [한국어] disconnect transient 상태 종료. 이 토글 후 외부에서 "disconnect 끝났음" 인지. */

	/* Doorbell buffer config is invalid during reset */
	nvme_ctrlr_free_doorbell_buffer(ctrlr);
	/* [한국어] reset 시 디바이스 컨텍스트가 날아가므로 등록한 shadow_doorbell/eventidx PRP 도
	 *         디바이스 측에선 잊혀진 상태. 호스트 캐시도 같이 free 해 stale 사용 방지.
	 *         재초기화 시 SET_DB_BUF_CFG 단계에서 새로 할당·등록. */

	/* I/O Command Set Specific Identify Controller data is invalidated during reset */
	nvme_ctrlr_free_iocs_specific_data(ctrlr);
	/* [한국어] reset 후 디바이스가 ZNS/IOCS 관련 capability 를 변경했을 가능성 — stale 캐시
	 *         사용 방지. 재초기화 시 IDENTIFY_IOCS_SPECIFIC 단계에서 다시 read. */

	spdk_bit_array_free(&ctrlr->free_io_qids);
	/* [한국어] I/O queue ID 풀(bit array) 해제. reset 후 디바이스의 max I/O queues 가
	 *         바뀌었을 가능성을 고려해 풀 자체를 재생성 — SET_NUM_QUEUES 단계에서 새로 할당. */

	/* Set the state back to DISCONNECTED to cause a full hardware reset. */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISCONNECTED, NVME_TIMEOUT_INFINITE);
	/* [한국어] 상태=DISCONNECTED. INFINITE timeout 으로 외부 reconnect 진입까지 무한 대기.
	 *         이 상태에서 process_init 이 호출돼도 dispatch 가 no-op (state 가 곧
	 *         reconnect_async 에 의해 INIT 으로 바뀔 것을 기대). */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_disconnect - 외부 사용자용 disconnect 진입 API (locking wrapper)
 *
 * @ctrlr: 사용자가 spdk_nvme_probe / spdk_nvme_attach 등으로 얻은 컨트롤러 핸들.
 * @return:
 *   - 0      : disconnect 시퀀스 시작 성공. 사용자는 spdk_nvme_ctrlr_reconnect_poll_async
 *              로 완료 폴링 후 spdk_nvme_ctrlr_reconnect_async 로 재연결.
 *   - -EBUSY : 이미 reset/disconnect 진행 중 — 잠시 후 재시도.
 *   - -ENXIO : 컨트롤러 hot-removed.
 *
 * 동기/배경:
 *   SPDK 공개 API (include/spdk/nvme.h 에 선언) 의 진입점.
 *   사용자(예: bdev_nvme 모듈)가 hot-plug 이벤트 처리, 스토리지 fault recovery, 또는
 *   사용자 명시적 reset 시 호출. 내부 nvme_ctrlr_disconnect 와 동일한 일을 하지만
 *   외부 호출이므로 ctrlr_lock 을 자체적으로 acquire/release.
 *
 *   대비: nvme_ctrlr_disconnect 는 internal — 호출자가 이미 lock 보유 가정.
 *
 * 동작 단계:
 *   [1] ctrlr_lock acquire — 다른 admin 처리/reset 진입과 직렬화.
 *   [2] nvme_ctrlr_disconnect 위임.
 *   [3] ctrlr_lock release — 본 함수 return 후 사용자는 lock 안 잡고 다음 단계 호출 가능.
 *
 * 실행 컨텍스트:
 *   - 사용자 thread (보통 management thread).
 *   - lock 미보유 상태에서 호출.
 *
 * 호출 체인:
 *   <사용자 코드(bdev_nvme reset_poll, etc.)> →
 *     [spdk_nvme_ctrlr_disconnect] (lock acquire) →
 *       nvme_ctrlr_disconnect → (transport disconnect, abort 정리, ...)
 *     (lock release)
 *   ... 사용자가 reconnect_async 로 ctrlr 활성 복원 ...
 */
int
spdk_nvme_ctrlr_disconnect(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;
	/* [한국어] 내부 disconnect 의 반환 코드를 lock release 후에도 보존하기 위한 임시 변수. */

	nvme_ctrlr_lock(ctrlr);
	/* [한국어] 외부 API 진입 — ctrlr_lock 획득. 동시에 호출되는 admin completion poller
	 *         /reset 진입 등과 직렬화 (lock 은 pthread_mutex / 단일 process 에선 흔히 spin). */
	rc = nvme_ctrlr_disconnect(ctrlr);
	/* [한국어] 실제 disconnect 로직 위임. 호출자(본 함수)가 lock 을 보유하므로
	 *         nvme_ctrlr_disconnect 의 사전조건 만족. */
	nvme_ctrlr_unlock(ctrlr);
	/* [한국어] 사용자 코드로 return 하기 전 lock 해제. */

	return rc;
	/* [한국어] -EBUSY/-ENXIO/0 그대로 전달. 사용자는 EBUSY 면 재시도, ENXIO 면 detach. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_reconnect_async - 컨트롤러 재초기화 시작 (reset chain 의 후반 진입점)
 *
 * @ctrlr: 이미 disconnect 단계를 마친 컨트롤러 (state=DISCONNECTED 또는 INIT 직전).
 * @return: 없음 (비동기 — 진행 상황은 reconnect_poll_async 로 확인).
 *
 * 동기/배경:
 *   reset chain 은 두 단계로 나뉜다:
 *     1) disconnect 단계: spdk_nvme_ctrlr_disconnect → nvme_ctrlr_disconnect →
 *        (poll) nvme_ctrlr_disconnect_done → state=DISCONNECTED.
 *     2) reconnect 단계: 본 함수 → state=INIT → 사용자 폴링(spdk_nvme_ctrlr_reconnect_poll_async)
 *        이 nvme_ctrlr_process_init 을 반복 호출 → READY.
 *
 *   "reconnect_async" 는 이 두 번째 단계의 시작점이다. 함수 자체는 짧지만 의미가 큼:
 *   상태머신을 INIT 으로 강제 되돌림으로써 process_init 이 처음부터(CONNECT_ADMINQ →
 *   READ_VS → READ_CAP → CHECK_EN → ...) 다시 진행해 컨트롤러를 활성 상태로 끌어올린다.
 *
 *   특이점 - lock 보유 정책:
 *   본 함수는 lock 을 "acquire 만 하고 unlock 하지 않은 채 return". 의도적인 설계 —
 *   reconnect 진행 중에는 다른 admin/reset 진입을 차단해야 하기 때문. lock 은 사용자가
 *   spdk_nvme_ctrlr_reconnect_poll_async 를 반복 호출하다가 0(완료) 을 받으면 그쪽에서
 *   unlock 한다. 즉 "reconnect_async 는 lock 잡기, reconnect_poll_async 는 lock 풀기" 의
 *   비대칭 lock 페어링.
 *
 * 동작 단계:
 *   [1] ctrlr_lock acquire (이후 unlock 안 함 — reconnect 진행 동안 보유 유지).
 *   [2] prepare_for_reset=false — disconnect 진입 시 set 한 "I/O 발행 차단" 신호 해제.
 *       이제 process_init 이 admin 명령을 재발행할 수 있게 됨.
 *   [3] state=INIT + INFINITE timeout — process_init 이 CONNECT_ADMINQ 부터 다시 시작.
 *   [4] 의도적으로 unlock 안 하고 return.
 *
 * 실행 컨텍스트:
 *   - 사용자 thread.
 *   - 진입 시 lock 미보유, 종료 시 lock 보유 (caller 가 보유 상태로 계속 진행).
 *
 * 호출 체인:
 *   ... spdk_nvme_ctrlr_disconnect → disconnect_done → state=DISCONNECTED ...
 *   <사용자 코드> →
 *     [spdk_nvme_ctrlr_reconnect_async] (lock 획득) →
 *       set_state(INIT) → (return, lock 보유)
 *   <사용자 폴링 루프> →
 *     spdk_nvme_ctrlr_reconnect_poll_async → nvme_ctrlr_process_init → ... → READY
 *     (마지막에 lock release)
 */
void
spdk_nvme_ctrlr_reconnect_async(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_ctrlr_lock(ctrlr);
	/* [한국어] reconnect 시퀀스 진입 — lock 획득. 이 lock 은 본 함수에서 unlock 하지 않고,
	 *         사용자가 이후 reconnect_poll_async 를 호출해 process_init 이 READY 에 도달
	 *         (또는 ERROR 로 실패) 한 시점에서 unlock 됨. */

	ctrlr->prepare_for_reset = false;
	/* [한국어] disconnect 진입 시 set 했던 "I/O qpair 들이 새 명령 발행 금지" 플래그 해제.
	 *         이제 process_init 의 admin 발행과 후속 I/O qpair reinitialize 가 정상 동작. */

	/* Set the state back to INIT to cause a full hardware reset. */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT, NVME_TIMEOUT_INFINITE);
	/* [한국어] 상태머신을 INIT 으로 reset — process_init 이 다음 호출에서 init flow 처음
	 *         (CONNECT_ADMINQ → READ_VS → READ_CAP → CHECK_EN → SET_EN_0 → ENABLE → ...
	 *          → IDENTIFY → SET_NUM_QUEUES → SET_DB_BUF_CFG → SET_HOST_ID →
	 *          TRANSPORT_READY → READY) 부터 재진행하게 만든다.
	 *         INFINITE timeout — process_init 자체가 단계별 timeout 을 다시 세팅하므로
	 *         초기값은 무한으로 두고 시작. */

	/* Return without releasing ctrlr_lock. ctrlr_lock will be released when
	 * spdk_nvme_ctrlr_reset_poll_async() returns 0.
	 */
	/* [한국어] 명시적 의도: lock 보유한 채 return. reconnect 진행이 끝날 때까지 (poll 함수가
	 *         0=READY 또는 영구 실패를 보고할 때까지) 다른 reset/admin 진입을 차단하기 위함.
	 *         사용자가 lock 을 풀지 않으면 deadlock — API 사용 계약 상 반드시
	 *         spdk_nvme_ctrlr_reconnect_poll_async 를 폴링해야 함. */
}

/*
 * [한국어]
 * nvme_ctrlr_reinitialize_io_qpair - reset 후 끊긴 IO qpair 를 다시 활성화 (PCIe 전용)
 *
 * @ctrlr: 대상 controller. reset 시퀀스를 마치고 admin 통신은 복구된 상태여야 한다.
 * @qpair: 재초기화할 IO qpair. 호출자(reconnect_poll_async)가 ctrlr->active_io_qpairs
 *         리스트를 순회하며 본 함수를 호출한다. 현재 프로세스 소유이고 PCIe 전송이며
 *         IO qpair(=admin 아님) 인 경우에만 진입한다.
 * @return: 0 = 재연결 성공 / 음수 = transport_failure_reason 가 LOCAL 로 마킹되고 caller
 *          가 호출 결과를 누적한다.
 *
 * 동기/배경:
 *   PCIe transport 의 경우 reset(CC.EN 1→0→1) 후에도 SQ/CQ 의 메모리 위치(PRP base 등)는
 *   유지되며, 단지 controller 측 큐 상태가 초기화되었으므로 admin 명령(Create IO SQ/CQ)
 *   으로 다시 활성화하면 된다. Fabrics(NVMe-oF) 는 별도 thread 에서 disconnect/reconnect
 *   시퀀스를 거치므로 본 함수의 단순 admin 재구성 경로를 사용하지 않는다.
 *
 * 동작 단계:
 *   1) 호출 컨텍스트 검증 (현재 프로세스 소유, non-fabrics, non-admin) — 위반 시 assert.
 *   2) qpair->async 플래그를 임시로 false 로 강제하여 synchronous connect 강제.
 *      reconnect_poll_async 는 lock 보유 상태에서 호출되므로 비동기 분기가 lock 을
 *      넘겨받아 호출하면 안 된다 (deadlock 위험). 끝난 후 원래 값 복원.
 *   3) nvme_transport_ctrlr_connect_qpair() 호출 — 내부적으로 Create IO CQ/SQ admin
 *      명령을 발행하고 admin 응답을 기다림.
 *   4) 실패 시 transport_failure_reason 을 LOCAL 로 마킹 — bdev 레이어가 추후
 *      qpair_process_completions() 에서 이를 보고 IO 를 fail 처리.
 *
 * 실행 컨텍스트: ctrlr_lock 보유 상태. controller 를 소유한 single-thread 에서 호출.
 *
 * 호출 체인:
 *   spdk_nvme_ctrlr_reset → spdk_nvme_ctrlr_reconnect_poll_async →
 *     [nvme_ctrlr_reinitialize_io_qpair] → nvme_transport_ctrlr_connect_qpair →
 *     (PCIe) admin Create IO SQ/CQ
 */
int
nvme_ctrlr_reinitialize_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	bool async;        /* [한국어] 원래 비동기 모드 플래그 백업용 — 함수 끝에서 복원. */
	int rc;            /* [한국어] connect_qpair 반환값. 0=성공/음수=실패. */

	/* [한국어] 사전 조건 검증:
	 *   (1) 현재 프로세스 == qpair 소유 프로세스 — 다른 프로세스의 qpair 를 건드리지 않음.
	 *   (2) PCIe (non-fabrics) — fabrics 는 별도 reconnect 경로 사용.
	 *   (3) admin queue 가 아님 — admin 은 reset 시퀀스에서 이미 처리됨.
	 *   하나라도 위반되면 호출자 버그이므로 assert 후 -EINVAL 반환. */
	if (nvme_ctrlr_get_current_process(ctrlr) != qpair->active_proc ||
	    spdk_nvme_ctrlr_is_fabrics(ctrlr) || nvme_qpair_is_admin_queue(qpair)) {
		assert(false);                  /* [한국어] 디버그 빌드에서 즉시 abort — 호출자 버그를 빠르게 노출. */
		return -EINVAL;                 /* [한국어] 릴리즈 빌드에서는 -EINVAL 로 caller 가 처리하도록. */
	}

	/* Force a synchronous connect. */
	/* [한국어] 동기 connect 강제. reconnect_poll_async 자체가 polling 형태이므로 본 함수에서
	 *         별도의 비동기 콜백 큐잉을 만들면 lock 추적이 복잡해진다. lock 보유 상태에서
	 *         admin 응답까지 대기하는 것이 안전. */
	async = qpair->async;                   /* [한국어] 원래 모드 백업. */
	qpair->async = false;                   /* [한국어] 동기 모드 강제. nvme_transport_ctrlr_connect_qpair 가
	                                         *         이 플래그를 보고 응답까지 기다림. */
	rc = nvme_transport_ctrlr_connect_qpair(ctrlr, qpair);
	                                        /* [한국어] PCIe 전송 핸들러 호출 — Create IO CQ → Create IO SQ
	                                         *         두 admin 명령을 순차 발행하고 완료 대기. */
	qpair->async = async;                   /* [한국어] 원래 비동기 플래그 복원. 사용자가 비동기로 IO 발행해 왔다면
	                                         *         그 모드 그대로 유지. */

	if (rc != 0) {
		qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
		                                /* [한국어] 재연결 실패 — qpair 를 LOCAL 실패로 마킹.
		                                 *         이후 qpair_process_completions() 가 이 플래그를 확인하고
		                                 *         outstanding IO 를 모두 -EIO 로 완료시킴. */
	}

	return rc;                              /* [한국어] caller(reconnect_poll_async) 가 누적하여
	                                         *         하나라도 실패하면 ctrlr 전체를 fail 처리. */
}

/**
 * This function will be called when the controller is being reinitialized.
 * Note: the ctrlr_lock must be held when calling this function.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_reconnect_poll_async - reset 후 reconnect 진행을 폴링 (사용자 노출 API)
 *
 * @ctrlr: 사전에 spdk_nvme_ctrlr_reconnect_async() 가 호출되어 ctrlr_lock 을 보유한 상태인
 *         controller. 이 함수가 0 또는 영구 실패를 반환할 때 비로소 lock 을 해제한다.
 * @return: 0  = reconnect 완료 (READY 상태 진입, lock 해제됨)
 *          -EAGAIN = 아직 진행 중. 사용자는 다시 호출해야 함.
 *          -1  = 영구 실패. ctrlr 는 fail 처리되었고 lock 도 해제됨.
 *
 * 동기/배경:
 *   reset 시퀀스의 마지막 단계. spdk_nvme_ctrlr_reconnect_async() 가 state 를
 *   NVME_CTRLR_STATE_INIT 로 되돌려 놓으면, 본 함수가 nvme_ctrlr_process_init() 를
 *   여러 번 호출하여 INIT → … → READY 까지 점진적으로 진행한다. 한 번의 호출에서는
 *   몇 단계만 진행되므로 polling 형태로 재호출이 필요하다.
 *
 * 동작 단계:
 *   1) nvme_ctrlr_process_init() 호출 — state machine 한 단계 진행. 실패 시 rc=-1.
 *   2) 아직 READY 가 아니면 -EAGAIN 반환 (lock 보유 유지) — 사용자는 재호출 필요.
 *   3) READY 도달 시(또는 영구 실패 시) IO qpair 일괄 재초기화:
 *      - PCIe: 각 qpair 의 transport_ctrlr_connect_qpair 호출 (admin Create IO SQ/CQ).
 *        free_io_qids 비트맵에서 해당 qid 클리어 — 다른 프로세스가 가로채지 못하도록.
 *      - Fabrics: 본 함수에서는 처리하지 않음. 각 qpair 의 owning thread 가 별도로 처리.
 *   4) inactive namespace 제거 — reset 중 ns 핸들이 무효화되었을 수 있음.
 *   5) 실패 시 nvme_ctrlr_fail(false) — 영구 실패 마킹, hot-remove 콜백 발화 안 함.
 *   6) is_resetting=false 로 reset 진행 종료 마킹.
 *   7) **ctrlr_lock 해제** — 이 시점부터 다른 reset/admin 진입 가능.
 *   8) ns_attribute_notices 미지원 컨트롤러는 nvme_io_msg_ctrlr_update() 로 ns 변동 알림.
 *
 * 실행 컨텍스트: ctrlr_lock 보유 상태로 진입. 0 또는 -1 반환 시점에 lock 해제.
 *               호출자(spdk_nvme_ctrlr_reset / 사용자 reconnect 폴링 루프) 의 단일 thread.
 *
 * 호출 체인:
 *   spdk_nvme_ctrlr_reset → reconnect_poll_async (반복 호출) →
 *     [reconnect_poll_async] → nvme_ctrlr_process_init → ... → READY
 *                          ↘ (PCIe) reinitialize_io_qpair 일괄 →
 *                          ↘ inactive ns RB-tree 정리 →
 *                          ↘ unlock
 */
int
spdk_nvme_ctrlr_reconnect_poll_async(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ns *ns, *tmp_ns;       /* [한국어] inactive ns 제거 시 RB-tree 안전 순회용 커서/임시. */
	struct spdk_nvme_qpair	*qpair;          /* [한국어] active_io_qpairs 순회 커서. */
	int rc = 0, rc_tmp = 0;                 /* [한국어] rc=최종 결과(영구 실패 -1), rc_tmp=qpair 재초기화 임시. */

	/* [한국어] state machine 한 단계 진행. 내부적으로 현재 state 에 해당하는 핸들러를 1회 실행하고
	 *         다음 state 로 전이시킨다. polling 모델이므로 한 번 호출에 INIT→READY 까지 가는 것은 아니며,
	 *         사용자가 재호출하면서 점진 진행. */
	if (nvme_ctrlr_process_init(ctrlr) != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "controller reinitialization failed\n");
		rc = -1;                        /* [한국어] 영구 실패 마킹 — 아래에서 nvme_ctrlr_fail() 호출. */
	}
	/* [한국어] 아직 READY 도달 전이면 -EAGAIN 반환. lock 은 그대로 유지하여 다른 reset 진입 차단.
	 *         단, rc==-1 (영구 실패) 인 경우는 EAGAIN 반환하지 않고 아래 cleanup 진행. */
	if (ctrlr->state != NVME_CTRLR_STATE_READY && rc != -1) {
		return -EAGAIN;                 /* [한국어] 사용자 재호출 필요. lock 보유 유지. */
	}

	/*
	 * For non-fabrics controllers, the memory locations of the transport qpair
	 * don't change when the controller is reset. They simply need to be
	 * re-enabled with admin commands to the controller. For fabric
	 * controllers we need to disconnect and reconnect the qpair on its
	 * own thread outside of the context of the reset.
	 */
	/* [한국어] PCIe 전송에서만 본 위치에서 IO qpair 재초기화 수행. Fabrics 는 각 qpair 의 owning
	 *         thread 가 spdk_nvme_ctrlr_reconnect_io_qpair() 로 별도 처리. */
	if (rc == 0 && !spdk_nvme_ctrlr_is_fabrics(ctrlr)) {
		/* Reinitialize qpairs */
		TAILQ_FOREACH(qpair, &ctrlr->active_io_qpairs, tailq) {
			/* Always clear the qid bit here, even for a foreign qpair. We need
			 * to make sure another process doesn't get the chance to grab that
			 * qid.
			 */
			/* [한국어] free_io_qids 비트맵에서 이 qid 비트를 0 으로 — 즉 "사용 중" 으로 마킹.
			 *         multi-process 에서 secondary process 가 같은 qid 를 새로 할당받지 못하게 막음.
			 *         qpair 가 다른 프로세스 소유여도 마찬가지로 마킹해야 안전. */
			assert(spdk_bit_array_get(ctrlr->free_io_qids, qpair->id));
			                        /* [한국어] reset 직전 reset 가 비트를 set(=free) 으로 만들었으므로
			                         *         지금은 1 이어야 함. assert 로 invariant 검증. */
			spdk_bit_array_clear(ctrlr->free_io_qids, qpair->id);
			                        /* [한국어] 비트 클리어 = "사용 중". 위 assert 와 짝. */
			if (nvme_ctrlr_get_current_process(ctrlr) != qpair->active_proc) {
				/*
				 * We cannot reinitialize a foreign qpair. The qpair's owning
				 * process will take care of it. Set failure reason to FAILURE_RESET
				 * to ensure that happens.
				 */
				/* [한국어] 다른 프로세스 소유 qpair — 본 프로세스에서 admin 재발행 불가.
				 *         FAILURE_RESET 로 마킹해두면 owning 프로세스가 IO 발행 시 이 플래그를 감지하고
				 *         자체적으로 reinitialize 해야 함을 알게 됨. */
				qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_RESET;
				continue;       /* [한국어] foreign qpair 는 건드리지 않고 다음으로. */
			}
			rc_tmp = nvme_ctrlr_reinitialize_io_qpair(ctrlr, qpair);
			                        /* [한국어] 본 프로세스 소유 qpair 재초기화 — admin Create IO CQ/SQ. */
			if (rc_tmp != 0) {
				rc = rc_tmp;    /* [한국어] 하나라도 실패하면 최종 rc 에 누적. 아래에서 ctrlr_fail. */
			}
		}
	}

	/*
	 * Take this opportunity to remove inactive namespaces. During a reset namespace
	 * handles can be invalidated.
	 */
	/* [한국어] reset 중에 ns 가 detach 되었을 수 있음. RB-tree 안전 순회로 inactive ns 제거. */
	RB_FOREACH_SAFE(ns, nvme_ns_tree, &ctrlr->ns, tmp_ns) {
		if (!ns->active) {
			RB_REMOVE(nvme_ns_tree, &ctrlr->ns, ns);
			                        /* [한국어] RB-tree 에서 ns 노드 제거. */
			spdk_free(ns);          /* [한국어] hugepage 할당된 ns 구조체 해제. */
		}
	}

	if (rc) {
		nvme_ctrlr_fail(ctrlr, false);
		                                /* [한국어] reset 영구 실패 — ctrlr 를 fail 상태로 마킹.
		                                 *         두 번째 인자 false = hot_remove 콜백 발화 안 함
		                                 *         (실제 디바이스 제거가 아니라 reset 실패이므로). */
	}
	ctrlr->is_resetting = false;            /* [한국어] reset 진행 플래그 해제 — 이후 다른 reset 진입 허용. */

	nvme_ctrlr_unlock(ctrlr);               /* [한국어] reconnect_async 에서 보유했던 lock 을 여기서 해제.
	                                         *         계약상 reconnect_poll_async 가 0 또는 -1 반환 시 unlock. */

	if (!ctrlr->cdata.oaes.ns_attribute_notices) {
		/*
		 * If controller doesn't support ns_attribute_notices and
		 * namespace attributes change (e.g. number of namespaces)
		 * we need to update system handling device reset.
		 */
		/* [한국어] OAES.ns_attribute_notices=0 인 컨트롤러는 AER 로 ns 변동을 알리지 않음.
		 *         따라서 reset 후 ns 수가 달라졌을 수도 있으므로, io_msg 시스템에 강제로
		 *         ctrlr 업데이트 신호를 보내 외부(bdev_nvme 등) 가 ns 재스캔하도록. */
		nvme_io_msg_ctrlr_update(ctrlr);
	}

	return rc;                              /* [한국어] 0=성공, -1=영구 실패. -EAGAIN 은 위에서 이미 처리. */
}

/*
 * For PCIe transport, spdk_nvme_ctrlr_disconnect() will do a Controller Level Reset
 * (Change CC.EN from 1 to 0) as a operation to disconnect the admin qpair.
 * The following two functions are added to do a Controller Level Reset. They have
 * to be called under the nvme controller's lock.
 */
/*
 * [한국어]
 * nvme_ctrlr_disable - controller 를 disable 상태(CC.EN=0)로 진입시키는 state machine 트리거
 *
 * @ctrlr: disable 시퀀스를 시작할 controller. is_disconnecting 플래그가 이미 true 여야 함.
 *
 * 동기/배경:
 *   PCIe transport 에서 spdk_nvme_ctrlr_disconnect() 가 호출되면 admin qpair 를 끊기 위해
 *   "Controller Level Reset" (CC.EN 1→0) 을 발행해야 한다. 본 함수는 그 첫 단추로,
 *   state 를 CHECK_EN 으로 설정하여 이후 polling 에서 단계별 진행이 일어나도록 한다.
 *   NVMe 스펙 §3.1.5 (Controller Configuration register, CC) / §7.3.2 Controller Level Reset.
 *
 * 동작 단계:
 *   1) is_disconnecting==true 검증 (호출자 invariant).
 *   2) state 를 NVME_CTRLR_STATE_CHECK_EN 으로 설정 — process_init 의 다음 polling 시
 *      CHECK_EN 핸들러가 실행되어 CSTS.RDY 와 CC.EN 을 읽고 적절한 disable 시퀀스로 분기.
 *   3) timeout=INFINITE — 단계별 timeout 은 process_init 핸들러가 자체 설정.
 *
 * 실행 컨텍스트: ctrlr_lock 보유 상태로 호출되어야 함.
 *
 * 호출 체인:
 *   spdk_nvme_ctrlr_disconnect → [nvme_ctrlr_disable] →
 *     (이후 사용자가 nvme_ctrlr_disable_poll 폴링) →
 *     CHECK_EN → SET_EN_0 → DISABLE_WAIT_FOR_READY_0 → DISABLED
 */
void
nvme_ctrlr_disable(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->is_disconnecting == true);
	                                        /* [한국어] disconnect 진행 마킹이 선행되어야 함 — 그래야 admin
	                                         *         경쟁 진입을 차단하고 안전하게 disable 가능. */

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_CHECK_EN, NVME_TIMEOUT_INFINITE);
	                                        /* [한국어] 상태 머신을 CHECK_EN 으로 진입. process_init 핸들러가
	                                         *         CSTS/CC 를 읽어 disable 절차(CC.EN 0 write → CSTS.RDY=0
	                                         *         대기) 를 단계별로 진행. INFINITE timeout 은 각 단계가
	                                         *         자체 timeout 을 설정하므로 초기값은 무한. */
}

/*
 * [한국어]
 * nvme_ctrlr_disable_poll - controller disable 진행을 폴링 (state machine 진척)
 *
 * @ctrlr: 사전에 nvme_ctrlr_disable() 이 호출된 controller. ctrlr_lock 보유 상태.
 * @return: 0 = DISABLED 도달 (성공)
 *          -EAGAIN = 진행 중. 사용자는 재호출 필요.
 *          -1 = 영구 실패 (process_init 에러).
 *
 * 동기/배경:
 *   nvme_ctrlr_disable() 이 state 를 CHECK_EN 으로 설정한 후, 본 함수는 process_init 을
 *   반복 호출하여 DISABLED 까지 진행한다. polling 모델이므로 caller 는 -EAGAIN 동안
 *   계속 호출해야 한다.
 *
 * 실행 컨텍스트: ctrlr_lock 보유 상태. caller 의 단일 thread.
 *
 * 호출 체인:
 *   spdk_nvme_ctrlr_disconnect → nvme_ctrlr_disable → (loop) [nvme_ctrlr_disable_poll]
 *     → nvme_ctrlr_process_init → CC.EN=0 MMIO write → CSTS.RDY=0 대기 → DISABLED
 */
int
nvme_ctrlr_disable_poll(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;                             /* [한국어] 최종 결과. 0=성공, -1=영구 실패. */

	/* [한국어] state machine 한 단계 진행. CHECK_EN → SET_EN_0 → WAIT_FOR_READY_0 → DISABLED 순. */
	if (nvme_ctrlr_process_init(ctrlr) != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "failed to disable controller\n");
		rc = -1;                        /* [한국어] 영구 실패 — caller 가 ctrlr_fail() 등 후처리 필요. */
	}

	/* [한국어] 아직 DISABLED 도달 전이고 영구 실패도 아니면 -EAGAIN — 재호출 요청. */
	if (ctrlr->state != NVME_CTRLR_STATE_DISABLED && rc != -1) {
		return -EAGAIN;
	}

	return rc;                              /* [한국어] 0 또는 -1. caller 가 후속 처리 분기. */
}

/*
 * [한국어]
 * nvme_ctrlr_fail_io_qpairs - 모든 활성 IO qpair 에 LOCAL 실패 플래그 일괄 설정
 *
 * @ctrlr: 대상 controller. active_io_qpairs 리스트가 보호 대상.
 *
 * 동기/배경:
 *   reset 시퀀스 진입 직후 호출되어, 진행 중인 IO 가 어차피 무효화될 것임을 qpair 측에
 *   미리 알려둔다. 이후 사용자 thread 가 qpair_process_completions() 호출 시 이 플래그를
 *   감지하고 outstanding IO 를 모두 -EIO 로 완료시켜 caller 의 재시도 로직이 동작하도록 함.
 *
 * 실행 컨텍스트: ctrlr_lock 보유 상태. spdk_nvme_ctrlr_reset() 의 disconnect 직후 호출.
 *               각 qpair 는 자기 owning thread 에서 별도로 polling 되므로 본 함수는 단지
 *               플래그만 set 하는 lockless 작업.
 *
 * 호출 체인:
 *   spdk_nvme_ctrlr_reset → [nvme_ctrlr_fail_io_qpairs]
 *     → (각 qpair 의 owning thread 가 spdk_nvme_qpair_process_completions 호출 시 감지)
 */
static void
nvme_ctrlr_fail_io_qpairs(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_qpair	*qpair;          /* [한국어] active_io_qpairs 순회 커서. */

	TAILQ_FOREACH(qpair, &ctrlr->active_io_qpairs, tailq) {
		qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
		                                /* [한국어] LOCAL 실패 플래그 set — 호스트 측 reset 으로 인한 IO 실패.
		                                 *         단일 store 이며 owning thread 측은 polling 시 이 값을
		                                 *         읽기만 하므로 별도 atomic/barrier 불필요 (eventual visibility 충분). */
	}
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_reset - 사용자 노출 동기 reset API (전체 reset 사이클 구동)
 *
 * @ctrlr: reset 대상 controller. 어떤 상태(failed/healthy/already-resetting) 이든 진입 가능.
 * @return: 0 = reset 성공, READY 복귀
 *          음수 = reset 실패. 대부분 ctrlr 가 fail 처리됨.
 *          (-EBUSY 는 "이미 reset 중" 인 경우인데, 본 함수는 이를 0 으로 변환하여 idempotent 처리)
 *
 * 동기/배경:
 *   비동기(reconnect_async/poll) API 의 동기 wrapper. 호출 즉시 disconnect → admin queue 비움
 *   → reconnect → READY 까지 한 번의 호출에서 끝낸다. 다만 사용자 thread 가 본 함수에 갇히는
 *   동안 admin polling 을 본 함수가 직접 수행하므로, 호출 thread 는 ctrlr 의 admin 소유 thread
 *   여야 한다 (또는 그에 준하는 single-threaded 모델). NVMe 스펙 §7.3 Reset Processing.
 *
 * 동작 단계 (전체 reset 사이클):
 *   1) lock 획득 → nvme_ctrlr_disconnect() 호출 → IO qpair 일괄 LOCAL 실패 마킹.
 *      disconnect 가 -EBUSY 반환 = 이미 reset 진행 중 → idempotent 0 반환.
 *   2) lock 해제 (disconnect 가 admin disable 시퀀스를 polling 으로 진행하므로 일단 unlock).
 *   3) admin queue 비우기 — process_admin_completions 가 -ENXIO 반환 (qpair 끊김) 까지 루프.
 *      이 동안 outstanding admin 명령들이 모두 abort 콜백으로 정리됨.
 *   4) spdk_nvme_ctrlr_reconnect_async() — lock 재획득, state=INIT 으로 reset.
 *   5) spdk_nvme_ctrlr_reconnect_poll_async() 를 -EAGAIN 동안 반복 호출 →
 *      INIT → ENABLE → IDENTIFY → SET_FEATURES → CONFIGURE_AER → ... → READY.
 *      poll_async 가 0 또는 -1 반환 시 lock 해제됨.
 *
 * 실행 컨텍스트: 사용자 thread (admin polling 권한 보유 thread). blocking — 수십~수백 ms 소요 가능.
 *               polled-mode 이므로 본 함수가 CPU 를 점유 (busy-wait).
 *
 * 호출 체인:
 *   user → [spdk_nvme_ctrlr_reset] → nvme_ctrlr_disconnect (CC.EN=1→0)
 *                                  → spdk_nvme_ctrlr_process_admin_completions (drain)
 *                                  → spdk_nvme_ctrlr_reconnect_async (state=INIT)
 *                                  → spdk_nvme_ctrlr_reconnect_poll_async (loop)
 *                                  → READY
 */
int
spdk_nvme_ctrlr_reset(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;                                 /* [한국어] 단계별 반환값. 최종 0=성공, 음수=실패. */

	nvme_ctrlr_lock(ctrlr);                 /* [한국어] disconnect 시작은 lock 보유 필수 — admin 경쟁 차단. */

	rc = nvme_ctrlr_disconnect(ctrlr);      /* [한국어] CC.EN=1→0 transition 시퀀스 시작.
	                                         *         성공=0, 이미 reset 진행 중=-EBUSY, 기타=음수. */
	if (rc == 0) {
		nvme_ctrlr_fail_io_qpairs(ctrlr);
		                                /* [한국어] disconnect 진입 직후 모든 IO qpair 에 LOCAL 실패 마킹.
		                                 *         outstanding IO 가 owning thread 에서 -EIO 로 정리되도록. */
	}

	nvme_ctrlr_unlock(ctrlr);               /* [한국어] disconnect 후 admin polling 단계는 lock 없이 진행
	                                         *         (admin 큐는 본 함수가 단독 polling 하므로 lockless). */

	if (rc != 0) {
		if (rc == -EBUSY) {
			rc = 0;                 /* [한국어] 이미 reset 진행 중 → idempotent 처리. 사용자는 성공으로 간주. */
		}
		return rc;                      /* [한국어] EBUSY 외의 실패는 그대로 반환. ctrlr 는 fail 처리되었을 수 있음. */
	}

	/* [한국어] admin queue drain 루프. disconnect 직후 admin qpair 가 끊기면서
	 *         outstanding admin 명령들을 abort 콜백으로 완료시켜야 함.
	 *         process_admin_completions 가 -ENXIO 반환 = qpair 가 끊겨 더 처리할 것 없음. */
	while (1) {
		rc = spdk_nvme_ctrlr_process_admin_completions(ctrlr);
		                                /* [한국어] admin CQ 폴링. polled-mode — busy-wait 형태이지만
		                                 *         disconnect 후 abort 콜백들이 빠르게 처리되므로 짧음. */
		if (rc == -ENXIO) {
			break;                  /* [한국어] qpair 끊김 — drain 완료. 다음 단계(reconnect) 로. */
		}
		                                /* [한국어] 아직 처리할 completion 이 있으면(rc>=0) 계속 루프. */
	}

	spdk_nvme_ctrlr_reconnect_async(ctrlr);
	                                        /* [한국어] reset 후반부 진입 — lock 재획득, state=INIT 으로 되돌림.
	                                         *         이후 polling 으로 INIT→READY 진행. lock 은 poll_async 가 해제. */

	while (true) {
		rc = spdk_nvme_ctrlr_reconnect_poll_async(ctrlr);
		                                /* [한국어] state machine 한 단계 진행. -EAGAIN=계속, 0=READY 도달, -1=영구 실패. */
		if (rc != -EAGAIN) {
			break;                  /* [한국어] 0 또는 -1 — 어느 쪽이든 lock 은 이미 해제됨. */
		}
		                                /* [한국어] -EAGAIN — 다음 단계 진행을 위해 다시 호출. */
	}

	return rc;                              /* [한국어] 0=성공(READY 복귀), -1=영구 실패. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_reset_subsystem - NVMe Subsystem Reset (NSSR) 발행
 *
 * @ctrlr: reset 대상. CAP.NSSRS=1 (Subsystem Reset 지원) 이어야 함.
 * @return: 0 = NSSR 레지스터 write 성공
 *          -ENOTSUP = 컨트롤러가 NSSR 미지원
 *          기타 음수 = MMIO write 실패
 *
 * 동기/배경:
 *   NVMe 스펙 §7.3.1 Subsystem Reset. NSSR 레지스터에 매직 값(0x4E564D65 = "NVMe") 을 write
 *   하면 subsystem 전체가 reset 된다. CC.EN=0 으로 단일 controller 만 reset 하는 것과 달리,
 *   subsystem 의 모든 controller/namespace/PCIe link 가 영향받는다.
 *
 * CC.EN reset vs NSSR 차이:
 *   - CC.EN reset (spdk_nvme_ctrlr_reset): 단일 controller 의 큐/state 만 초기화. PCIe link 유지.
 *   - NSSR: subsystem 전체 reset. PCIe transport 에서는 link down/up 발생 → host 측에서는
 *     hot-remove 이벤트로 처리된다. 따라서 본 함수는 reset 시퀀스를 직접 구동하지 않고,
 *     단지 NSSR write 만 하고 hot-remove handler 에 모든 cleanup 을 위임한다.
 *
 * 동작 단계:
 *   1) CAP.NSSRS 확인 — 미지원 시 -ENOTSUP.
 *   2) lock 획득, is_resetting=true.
 *   3) nvme_ctrlr_set_nssr(SPDK_NVME_NSSR_VALUE) — MMIO 로 NSSR 레지스터에 매직 값 write.
 *   4) is_resetting=false, lock 해제.
 *   5) 추가 cleanup 없음 — PCIe hot-remove 가 link down 을 감지하고 ctrlr destroy 수행.
 *
 * 실행 컨텍스트: 사용자 thread. lock 보유 후 MMIO 1회 write 만 하므로 매우 짧음.
 *
 * 호출 체인:
 *   user → [spdk_nvme_ctrlr_reset_subsystem] → nvme_ctrlr_set_nssr → MMIO write
 *        → (subsystem reset 발생) → (hot-remove 콜백 트리거) → ctrlr destruct
 */
int
spdk_nvme_ctrlr_reset_subsystem(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cap_register cap;       /* [한국어] CAP 레지스터 캐시 — NSSRS 비트 확인용. */
	int rc = 0;                             /* [한국어] 최종 반환값. */

	cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);
	                                        /* [한국어] CAP (Controller Capabilities) 레지스터 읽기.
	                                         *         init 시 캐시된 값 사용 — MMIO 재접근 회피. */
	if (cap.bits.nssrs == 0) {
		NVME_CTRLR_WARNLOG(ctrlr, "subsystem reset is not supported\n");
		return -ENOTSUP;                /* [한국어] NSSRS=0 — NSSR 미지원. CC.EN reset 으로 폴백 권장. */
	}

	NVME_CTRLR_NOTICELOG(ctrlr, "resetting subsystem\n");
	nvme_ctrlr_lock(ctrlr);                 /* [한국어] is_resetting 플래그 보호 + 동시 reset 차단. */
	ctrlr->is_resetting = true;             /* [한국어] reset 진행 마킹 — 동시 reset 진입 차단. */
	rc = nvme_ctrlr_set_nssr(ctrlr, SPDK_NVME_NSSR_VALUE);
	                                        /* [한국어] NSSR 레지스터에 매직 값 0x4E564D65 ("NVMe", little-endian) write.
	                                         *         이 매직 값만이 reset 트리거 — 다른 값은 무시된다 (스펙 §3.1.7). */
	ctrlr->is_resetting = false;            /* [한국어] write 자체는 즉시 끝나므로 플래그 즉시 해제.
	                                         *         실제 reset 효과는 비동기로 PCIe link down 형태로 나타남. */

	nvme_ctrlr_unlock(ctrlr);
	/*
	 * No more cleanup at this point like in the ctrlr reset. A subsystem reset will cause
	 * a hot remove for PCIe transport. The hot remove handling does all the necessary ctrlr cleanup.
	 */
	/* [한국어] 추가 cleanup 없음. 이유:
	 *   PCIe transport 에서 NSSR 은 link down 을 유발하고, 이는 PCIe hot-remove 이벤트로
	 *   감지되어 별도 핸들러(ctrlr->remove_cb 또는 내부 destruct 경로) 가 모든 정리를 수행함.
	 *   따라서 본 함수는 단지 트리거만 발사하고 즉시 반환. */
	return rc;                              /* [한국어] 0 = NSSR write 성공 (reset 효과는 비동기). */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_set_trid - controller 의 transport id 변경 (ANA failover 등에서 사용)
 *
 * @ctrlr: 변경 대상 controller. **반드시 is_failed==true 여야 함** (= reset 실패 후 재시도 직전).
 * @trid: 새 transport id. 같은 trtype/subnqn 을 가져야 하고, 보통 traddr/trsvcid 만 바뀐다
 *        (예: NVMe-oF 의 다른 ANA path 로 failover).
 * @return: 0 = 성공
 *          -EPERM = ctrlr 가 failed 상태가 아님 (정상 동작 중인 ctrlr 의 trid 변경 금지)
 *          -EINVAL = trtype 또는 subnqn 불일치
 *
 * 동기/배경:
 *   NVMe-oF (특히 multipath/ANA) 에서 한 path 가 끊겼을 때, 사용자(또는 nvme bdev 모듈) 가
 *   다른 path 의 traddr/trsvcid 로 trid 를 갈아끼우고 다시 reset 을 시도하는 시나리오를 위한 API.
 *   같은 controller 객체를 재사용하므로 application 은 동일한 spdk_nvme_ctrlr 핸들로 계속 동작 가능.
 *
 * 동작 단계:
 *   1) lock 획득.
 *   2) is_failed 검증 — 정상 동작 중에는 변경 금지 (활성 IO 와 conflict 위험).
 *   3) trtype 동일성 검증 — 같은 transport 종류여야 함 (RDMA→TCP 같은 변경 불가).
 *   4) subnqn 동일성 검증 — 같은 subsystem 내 path 변경만 허용.
 *   5) ctrlr->trid 를 새 값으로 덮어쓰기.
 *   6) lock 해제.
 *
 * 실행 컨텍스트: 사용자 thread (보통 bdev_nvme 의 reset 감독 thread).
 *               변경 후 사용자는 spdk_nvme_ctrlr_reset() 으로 새 trid 로 reconnect 시도.
 *
 * 호출 체인:
 *   bdev_nvme failover handler → [spdk_nvme_ctrlr_set_trid] → ctrlr->trid 갱신
 *     → spdk_nvme_ctrlr_reset → 새 trid 로 reconnect
 */
int
spdk_nvme_ctrlr_set_trid(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_transport_id *trid)
{
	int rc = 0;                             /* [한국어] 반환값. */

	nvme_ctrlr_lock(ctrlr);                 /* [한국어] trid 는 reset 시퀀스에서 사용되므로 lock 보호 필수. */

	if (ctrlr->is_failed == false) {
		rc = -EPERM;                    /* [한국어] 정상 동작 중에는 변경 금지 — 활성 IO 와 path 충돌 방지. */
		goto out;
	}

	if (trid->trtype != ctrlr->trid.trtype) {
		rc = -EINVAL;                   /* [한국어] transport 종류 변경 금지 (예: RDMA→TCP 불가). */
		goto out;
	}

	if (strncmp(trid->subnqn, ctrlr->trid.subnqn, SPDK_NVMF_NQN_MAX_LEN)) {
		rc = -EINVAL;                   /* [한국어] 다른 subsystem 으로 옮기는 것 금지. */
		goto out;
	}

	ctrlr->trid = *trid;                    /* [한국어] traddr/trsvcid 등을 새 값으로 갱신.
	                                         *         struct 전체 복사 — strncpy 같은 string 처리 불필요. */

out:
	nvme_ctrlr_unlock(ctrlr);
	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_set_remove_cb - PCIe hot-remove 콜백 등록 (primary process 전용)
 *
 * @ctrlr: 콜백을 설정할 controller.
 * @remove_cb: 디바이스가 제거되었을 때 호출될 콜백. 시그니처: void cb(void *ctx, struct spdk_nvme_ctrlr *).
 * @remove_ctx: 콜백 호출 시 첫 인자로 전달될 사용자 컨텍스트.
 *
 * 동기/배경:
 *   NVMe SSD 가 PCIe hot-remove 되거나 NSSR 후 link down 되면, SPDK 가 이를 감지하여
 *   사용자가 등록한 remove_cb 를 호출한다. 사용자(보통 bdev_nvme) 는 이 콜백에서 ctrlr
 *   참조를 정리하고 spdk_nvme_detach() 를 호출하여 자원을 회수한다.
 *
 * 제약:
 *   - **primary process 만 호출 허용**: secondary process 는 ctrlr 객체를 공유하지만
 *     hot-remove 이벤트는 primary 가 단독으로 처리하기 때문. secondary 가 호출하면 무시.
 *
 * 실행 컨텍스트: 사용자 thread (등록 시점은 init 직후가 일반적).
 *               콜백 자체는 SPDK 내부의 device removal 감지 thread (PCIe 모듈) 에서 호출됨.
 *
 * 호출 체인:
 *   user/bdev_nvme → [spdk_nvme_ctrlr_set_remove_cb] (등록만)
 *   (이후 PCIe remove 감지 시) PCIe transport → ctrlr->remove_cb(cb_ctx, ctrlr)
 */
void
spdk_nvme_ctrlr_set_remove_cb(struct spdk_nvme_ctrlr *ctrlr,
			      spdk_nvme_remove_cb remove_cb, void *remove_ctx)
{
	if (!spdk_process_is_primary()) {
		return;                         /* [한국어] secondary process 는 hot-remove 핸들링 권한 없음 — silently 무시. */
	}

	nvme_ctrlr_lock(ctrlr);                 /* [한국어] remove_cb 는 다른 thread(remove 감지) 에서 읽히므로 lock 보호. */
	ctrlr->remove_cb = remove_cb;           /* [한국어] 콜백 함수 포인터 저장. NULL 가능 (해제 의미). */
	ctrlr->cb_ctx = remove_ctx;             /* [한국어] 콜백 첫 인자로 전달될 컨텍스트. 사용자 객체 ptr 등. */
	nvme_ctrlr_unlock(ctrlr);
}

int
spdk_nvme_ctrlr_set_keys(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ctrlr_key_opts *opts)
{
	nvme_ctrlr_lock(ctrlr);
	if (SPDK_GET_FIELD(opts, dhchap_key, ctrlr->opts.dhchap_key) == NULL &&
	    SPDK_GET_FIELD(opts, dhchap_ctrlr_key, ctrlr->opts.dhchap_ctrlr_key) != NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "DH-HMAC-CHAP controller key requires host key to be set\n");
		nvme_ctrlr_unlock(ctrlr);
		return -EINVAL;
	}

	ctrlr->opts.dhchap_key =
		SPDK_GET_FIELD(opts, dhchap_key, ctrlr->opts.dhchap_key);
	ctrlr->opts.dhchap_ctrlr_key =
		SPDK_GET_FIELD(opts, dhchap_ctrlr_key, ctrlr->opts.dhchap_ctrlr_key);
	nvme_ctrlr_unlock(ctrlr);

	return 0;
}

static void
nvme_ctrlr_identify_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "nvme_identify_controller failed!\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	/*
	 * Use MDTS to ensure our default max_xfer_size doesn't exceed what the
	 *  controller supports.
	 */
	ctrlr->max_xfer_size = nvme_transport_ctrlr_get_max_xfer_size(ctrlr);
	NVME_CTRLR_DEBUGLOG(ctrlr, "transport max_xfer_size %u\n", ctrlr->max_xfer_size);
	if (ctrlr->cdata.mdts > 0) {
		ctrlr->max_xfer_size = spdk_min(ctrlr->max_xfer_size,
						ctrlr->min_page_size * (1 << ctrlr->cdata.mdts));
		NVME_CTRLR_DEBUGLOG(ctrlr, "MDTS max_xfer_size %u\n", ctrlr->max_xfer_size);
	}

	NVME_CTRLR_DEBUGLOG(ctrlr, "CNTLID 0x%04" PRIx16 "\n", ctrlr->cdata.cntlid);
	if (ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		ctrlr->cntlid = ctrlr->cdata.cntlid;
	} else {
		/*
		 * Fabrics controllers should already have CNTLID from the Connect command.
		 *
		 * If CNTLID from Connect doesn't match CNTLID in the Identify Controller data,
		 * trust the one from Connect.
		 */
		if (ctrlr->cntlid != ctrlr->cdata.cntlid) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Identify CNTLID 0x%04" PRIx16 " != Connect CNTLID 0x%04" PRIx16 "\n",
					    ctrlr->cdata.cntlid, ctrlr->cntlid);
		}
	}

	if (ctrlr->cdata.sgls.supported && !(ctrlr->quirks & NVME_QUIRK_NOT_USE_SGL)) {
		assert(ctrlr->cdata.sgls.supported != 0x3);
		ctrlr->flags |= SPDK_NVME_CTRLR_SGL_SUPPORTED;
		if (ctrlr->cdata.sgls.supported == 0x2) {
			ctrlr->flags |= SPDK_NVME_CTRLR_SGL_REQUIRES_DWORD_ALIGNMENT;
		}

		ctrlr->max_sges = nvme_transport_ctrlr_get_max_sges(ctrlr);
		NVME_CTRLR_DEBUGLOG(ctrlr, "transport max_sges %u\n", ctrlr->max_sges);
	}

	if (ctrlr->cdata.sgls.metadata_address && !(ctrlr->quirks & NVME_QUIRK_NOT_USE_SGL)) {
		ctrlr->flags |= SPDK_NVME_CTRLR_MPTR_SGL_SUPPORTED;
	}

	if (ctrlr->cdata.oacs.ssrs && !(ctrlr->quirks & NVME_QUIRK_OACS_SECURITY)) {
		ctrlr->flags |= SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED;
	}

	if (ctrlr->cdata.oacs.dirs) {
		ctrlr->flags |= SPDK_NVME_CTRLR_DIRECTIVES_SUPPORTED;
	}

	NVME_CTRLR_DEBUGLOG(ctrlr, "fuses compare and write: %d\n",
			    ctrlr->cdata.fuses.fcws);
	if (ctrlr->cdata.fuses.fcws) {
		ctrlr->flags |= SPDK_NVME_CTRLR_COMPARE_AND_WRITE_SUPPORTED;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_CONFIGURE_AER,
			     ctrlr->opts.admin_timeout_ms);
}

static int
nvme_ctrlr_identify(struct spdk_nvme_ctrlr *ctrlr)
{
	int	rc;

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY,
			     ctrlr->opts.admin_timeout_ms);

	rc = nvme_ctrlr_cmd_identify(ctrlr, SPDK_NVME_IDENTIFY_CTRLR, 0, 0, 0,
				     &ctrlr->cdata, sizeof(ctrlr->cdata),
				     nvme_ctrlr_identify_done, ctrlr);
	if (rc != 0) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return rc;
	}

	return 0;
}

static void
nvme_ctrlr_get_zns_cmd_and_effects_log_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_cmds_and_effect_log_page *log_page;
	struct spdk_nvme_ctrlr *ctrlr = arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "nvme_ctrlr_get_zns_cmd_and_effects_log failed!\n");
		spdk_free(ctrlr->tmp_ptr);
		ctrlr->tmp_ptr = NULL;
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	log_page = ctrlr->tmp_ptr;

	if (log_page->io_cmds_supported[SPDK_NVME_OPC_ZONE_APPEND].csupp) {
		ctrlr->flags |= SPDK_NVME_CTRLR_ZONE_APPEND_SUPPORTED;
	}
	spdk_free(ctrlr->tmp_ptr);
	ctrlr->tmp_ptr = NULL;

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_NUM_QUEUES, ctrlr->opts.admin_timeout_ms);
}

static int
nvme_ctrlr_get_zns_cmd_and_effects_log(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	assert(!ctrlr->tmp_ptr);
	ctrlr->tmp_ptr = spdk_zmalloc(sizeof(struct spdk_nvme_cmds_and_effect_log_page), 64, NULL,
				      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE | SPDK_MALLOC_DMA);
	if (!ctrlr->tmp_ptr) {
		rc = -ENOMEM;
		goto error;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_GET_ZNS_CMD_EFFECTS_LOG,
			     ctrlr->opts.admin_timeout_ms);

	rc = spdk_nvme_ctrlr_cmd_get_log_page_ext(ctrlr, SPDK_NVME_LOG_COMMAND_EFFECTS_LOG,
			0, ctrlr->tmp_ptr, sizeof(struct spdk_nvme_cmds_and_effect_log_page),
			0, 0, 0, SPDK_NVME_CSI_ZNS << 24,
			nvme_ctrlr_get_zns_cmd_and_effects_log_done, ctrlr);
	if (rc != 0) {
		goto error;
	}

	return 0;

error:
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	spdk_free(ctrlr->tmp_ptr);
	ctrlr->tmp_ptr = NULL;
	return rc;
}

static void
nvme_ctrlr_identify_zns_specific_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* no need to print an error, the controller simply does not support ZNS */
		nvme_ctrlr_free_zns_specific_data(ctrlr);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_NUM_QUEUES,
				     ctrlr->opts.admin_timeout_ms);
		return;
	}

	/* A zero zasl value means use mdts */
	if (ctrlr->cdata_zns->zasl) {
		uint32_t max_append = ctrlr->min_page_size * (1 << ctrlr->cdata_zns->zasl);
		ctrlr->max_zone_append_size = spdk_min(ctrlr->max_xfer_size, max_append);
	} else {
		ctrlr->max_zone_append_size = ctrlr->max_xfer_size;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_GET_ZNS_CMD_EFFECTS_LOG,
			     ctrlr->opts.admin_timeout_ms);
}

/**
 * This function will try to fetch the I/O Command Specific Controller data structure for
 * each I/O Command Set supported by SPDK.
 *
 * If an I/O Command Set is not supported by the controller, "Invalid Field in Command"
 * will be returned. Since we are fetching in a exploratively way, getting an error back
 * from the controller should not be treated as fatal.
 *
 * I/O Command Sets not supported by SPDK will be skipped (e.g. Key Value Command Set).
 *
 * I/O Command Sets without a IOCS specific data structure (i.e. a zero-filled IOCS specific
 * data structure) will be skipped (e.g. NVM Command Set, Key Value Command Set).
 */
static int
nvme_ctrlr_identify_iocs_specific(struct spdk_nvme_ctrlr *ctrlr)
{
	int	rc;

	if (!nvme_ctrlr_multi_iocs_enabled(ctrlr)) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_NUM_QUEUES,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	/*
	 * Since SPDK currently only needs to fetch a single Command Set, keep the code here,
	 * instead of creating multiple NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC substates,
	 * which would require additional functions and complexity for no good reason.
	 */
	assert(!ctrlr->cdata_zns);
	ctrlr->cdata_zns = spdk_zmalloc(sizeof(*ctrlr->cdata_zns), 64, NULL, SPDK_ENV_NUMA_ID_ANY,
					SPDK_MALLOC_SHARE | SPDK_MALLOC_DMA);
	if (!ctrlr->cdata_zns) {
		rc = -ENOMEM;
		goto error;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_IOCS_SPECIFIC,
			     ctrlr->opts.admin_timeout_ms);

	rc = nvme_ctrlr_cmd_identify(ctrlr, SPDK_NVME_IDENTIFY_CTRLR_IOCS, 0, 0, SPDK_NVME_CSI_ZNS,
				     ctrlr->cdata_zns, sizeof(*ctrlr->cdata_zns),
				     nvme_ctrlr_identify_zns_specific_done, ctrlr);
	if (rc != 0) {
		goto error;
	}

	return 0;

error:
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	nvme_ctrlr_free_zns_specific_data(ctrlr);
	return rc;
}

/*
 * [한국어] enum nvme_active_ns_state - Active Namespace List 비동기 페이지 페치 진행 상태
 *
 * Identify (CNS=0x02) 는 NSID 1024개 한 페이지씩 반환 — 컨트롤러의 활성 NS 가 1024개를
 * 넘으면 next_nsid 를 갱신하며 여러 페이지를 순차 페치한다. 이 상태머신은 그 순차 진행을 관리.
 */
enum nvme_active_ns_state {
	NVME_ACTIVE_NS_STATE_IDLE,
	/* [한국어] 초기 상태 — 아직 첫 페이지 admin 발행 전.
	 *  - 설정자: nvme_active_ns_ctx_create (calloc 결과로 자동 IDLE=0).
	 *  - 읽는 자: 첫 페이지 발행 직전. */

	NVME_ACTIVE_NS_STATE_PROCESSING,
	/* [한국어] 현재 페이지 admin 진행 중 (응답 대기).
	 *  - 설정자: nvme_ctrlr_identify_active_ns_async — admin 발행 직후.
	 *  - 읽는 자: deleter 가 본 상태에서는 호출 안 됨 (race 방지 가드). */

	NVME_ACTIVE_NS_STATE_DONE,
	/* [한국어] 모든 페이지 페치 완료 — new_ns_list 사용 가능.
	 *  - 설정자: identify_active_ns_async_done — 마지막 페이지에서 0 sentinel 발견 또는
	 *    next_nsid 가 NN 초과 도달 시.
	 *  - 읽는 자: deleter — swap 진행. */

	NVME_ACTIVE_NS_STATE_ERROR
	/* [한국어] admin 실패 또는 alloc 실패 — caller 가 정리 후 ERROR 상태로 컨트롤러 전이.
	 *  - 설정자: identify_active_ns_async_done (admin 에러), 또는 ctx_create 실패 시 직접 set. */
};

typedef void (*nvme_active_ns_ctx_deleter)(struct nvme_active_ns_ctx *);
/* [한국어] active NS ctx 의 비동기 deleter 콜백 타입.
 *  - async 페치가 끝나면 (DONE/ERROR) 호출됨. 동기 페치는 deleter=NULL 로 caller 가 직접 destroy. */

/*
 * [한국어] struct nvme_active_ns_ctx - Active NS List 비동기 페치의 진행 컨텍스트
 *
 * Identify (CNS=0x02) 명령은 한 번에 NSID 1024개의 페이지(4KB) 만 반환하므로, NN 이 큰
 * 컨트롤러에서는 next_nsid 를 증가시키며 여러 admin 명령을 직렬로 발행해야 한다.
 * 본 ctx 가 그 진행 상태를 보관 — page_count, next_nsid, status 등.
 */
struct nvme_active_ns_ctx {
	struct spdk_nvme_ctrlr *ctrlr;
	/* [한국어] 대상 컨트롤러 역참조.
	 *  - 설정자: create 시 set, 이후 불변.
	 *  - 읽는 자: deleter / done 콜백 — 다음 admin 발행, ns 트리 swap. */

	uint32_t page_count;
	/* [한국어] 지금까지 페치한 페이지 수 (=발행한 admin 명령 수).
	 *  - 설정자: identify_active_ns_async — 페이지 완료 후 ++.
	 *  - 읽는 자: deleter — new_ns_list 의 최대 슬롯 수 계산 (page_count × 1024). */

	uint32_t next_nsid;
	/* [한국어] 다음 페이지 페치 시작 NSID.
	 *  - 설정자: done 콜백 — 마지막 페이지의 마지막 NSID + 1 로 갱신.
	 *  - 읽는 자: 다음 admin 발행 — Identify cdw1 (NSID) 필드. */

	uint32_t *new_ns_list;
	/* [한국어] 누적 NSID 리스트 buffer — 페이지별 응답이 이어 붙음. 0 sentinel 로 종료.
	 *  - 설정자: create 시 1페이지(4KB) alloc, 이후 페이지 추가 시 realloc.
	 *  - 읽는 자: deleter (swap 대상). 컨트롤러 ns RB tree 와 swap 후 free.
	 *  - 동기화: ctx 수명 동안 ctx 소유자(단일 admin completion thread) 만 접근. */

	nvme_active_ns_ctx_deleter deleter;
	/* [한국어] 페치 종료 시 호출할 cleanup 콜백.
	 *  - 설정자: create 시 caller 가 지정 (async: _nvme_active_ns_ctx_deleter, 동기: NULL).
	 *  - 읽는 자: identify_active_ns_async_done — DONE/ERROR 도달 시 호출. */

	struct nvme_completion_poll_status status;
	/* [한국어] 동기 페치(deleter=NULL) 의 admin 완료 polling 상태.
	 *  - 설정자: nvme_completion_poll_cb 가 done=true, cpl 채움.
	 *  - 읽는 자: nvme_wait_for_adminq_completion (동기 경로). */

	enum nvme_active_ns_state state;
	/* [한국어] 현재 페치 진행 상태머신.
	 *  - 설정자: ctx_create=IDLE, async=PROCESSING, done 콜백=DONE/ERROR.
	 *  - 읽는 자: deleter — DONE/ERROR 분기. 사용자 정의 deleter 가 이 값을 보고 정리 결정. */
};

/*
 * [한국어]
 * nvme_active_ns_ctx_create - Active NS List 페치 컨텍스트 할당 + 첫 페이지 buffer 준비
 *
 * @ctrlr: 대상 컨트롤러
 * @deleter: 비동기 종료 콜백 (동기 모드는 NULL)
 * @return ctx 포인터, OOM 시 NULL
 *
 * 동작:
 *   - ctx 자체 calloc — state=IDLE 자동 초기화.
 *   - new_ns_list 4KB(spdk_nvme_ns_list 크기) zmalloc — DPDK hugepage shared (multi-process 공유)
 *   - page_count=1 — 첫 페이지 buffer 확보됨을 표시.
 *
 * hugepage 사용 이유: Identify 응답이 DMA 로 들어오므로 IOVA 매핑 가능한 메모리 필요.
 */
static struct nvme_active_ns_ctx *
nvme_active_ns_ctx_create(struct spdk_nvme_ctrlr *ctrlr, nvme_active_ns_ctx_deleter deleter)
{
	struct nvme_active_ns_ctx *ctx;
	uint32_t *new_ns_list = NULL;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate nvme_active_ns_ctx!\n");
		return NULL;
	}

	new_ns_list = spdk_zmalloc(sizeof(struct spdk_nvme_ns_list), ctrlr->page_size,
				   NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_SHARE);
	if (!new_ns_list) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate active_ns_list!\n");
		free(ctx);
		return NULL;
	}

	ctx->page_count = 1;
	ctx->new_ns_list = new_ns_list;
	ctx->ctrlr = ctrlr;
	ctx->deleter = deleter;

	return ctx;
}

/*
 * [한국어]
 * nvme_active_ns_ctx_destroy - active NS ctx 해제 (deleter 또는 동기 caller 가 호출)
 *
 * @ctx: 해제 대상 컨텍스트
 *
 * new_ns_list 는 hugepage 영역이라 spdk_free 사용, ctx 자체는 일반 heap 이라 free.
 */
static void
nvme_active_ns_ctx_destroy(struct nvme_active_ns_ctx *ctx)
{
	spdk_free(ctx->new_ns_list);  /* [한국어] hugepage 영역 해제 — DPDK rte_free. */
	free(ctx);                    /* [한국어] heap calloc 결과 — 일반 free. */
}

/*
 * [한국어]
 * nvme_ctrlr_destruct_namespace - 특정 NSID 의 NS 객체를 inactive 로 전환 (정리)
 *
 * @ctrlr: 대상 컨트롤러
 * @nsid: 정리할 NSID
 * @return 0 성공, -EINVAL (NSID 가 RB tree 에 없음)
 *
 * 호출 시점: identify_active_ns 결과가 "이 NSID 는 더 이상 활성 아님" 으로 판정될 때 (NS Attr
 * Change AER 처리). RB tree 에서 노드 자체는 유지하되 active=false 로 표시 — 추후 재활성화 가능성 대비.
 *
 * 동작: ns_destruct → nsdata/id_desc/IOCS 데이터 모두 0 으로 clear → active=false set.
 */
static int
nvme_ctrlr_destruct_namespace(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns tmp, *ns;

	assert(ctrlr != NULL);

	tmp.id = nsid;
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);
	                                  /* [한국어] RB tree 검색 — 존재하지 않으면 정리 불가 (이미 destroyed). */
	if (ns == NULL) {
		return -EINVAL;
	}

	nvme_ns_destruct(ns);         /* [한국어] nsdata/id_desc/iocs-specific 모두 0 클리어. */
	ns->active = false;           /* [한국어] active 비트 false — 사용자 get_first/next_active_ns 가 skip. */

	return 0;
}

/*
 * [한국어]
 * nvme_ctrlr_construct_namespace - 특정 NSID 의 NS 객체를 active 로 마킹 (lazy alloc)
 *
 * @ctrlr: 대상 컨트롤러
 * @nsid: 활성화할 NSID
 * @return 0 성공, -EINVAL (NSID 범위 밖), -ENOMEM (alloc 실패)
 *
 * 호출 시점: identify_active_ns 결과로부터 active NSID 가 발견될 때마다 호출.
 * spdk_nvme_ctrlr_get_ns 의 lazy alloc 패턴을 활용 — 객체 미존재면 RB tree 에 추가.
 *
 * 주의: 이 함수는 ns->active 만 set. 실제 nsdata 채우기는 별도 identify_ns(CNS=0x00) 발행 필요.
 */
static int
nvme_ctrlr_construct_namespace(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns *ns;

	if (nsid < 1 || nsid > ctrlr->cdata.nn) {
	                                  /* [한국어] NSID 범위 검사 — 0 (invalid) 또는 NN 초과. */
		return -EINVAL;
	}

	/* Namespaces are constructed on demand, so simply request it. */
	/* [한국어] lazy alloc 패턴 — get_ns 가 RB_FIND 후 미발견 시 alloc + insert 수행. */
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		return -ENOMEM;       /* [한국어] OOM — alloc 실패. */
	}

	ns->active = true;            /* [한국어] active 비트 set — get_first/next_active_ns 에서 발견됨. */

	return 0;
}

/* Returns true if the identify flow should be terminated, false otherwise. */
static bool
nvme_ctrlr_handle_identify_ns_completion(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns,
		const struct spdk_nvme_cpl *cpl)
{
	/* A namespace becoming inactive during NVMe controller initialization should not be
	 * considered a fatal error leading to controller state machine failure. */
	if (spdk_nvme_cpl_is_error(cpl)) {
		if (cpl->status.sct == SPDK_NVME_SCT_GENERIC &&
		    (cpl->status.sc == SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT ||
		     cpl->status.sc == SPDK_NVME_SC_INVALID_FIELD)) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Destructing namespace due to identify completion error\n");
			nvme_ctrlr_destruct_namespace(ctrlr, ns->id);
		} else {
			return true;
		}
	}

	return false;
}

static void
nvme_ctrlr_identify_active_ns_swap(struct spdk_nvme_ctrlr *ctrlr, uint32_t *new_ns_list,
				   size_t max_entries)
{
	uint32_t active_ns_count = 0;
	size_t i;
	uint32_t nsid;
	struct spdk_nvme_ns *ns, *tmp_ns;
	int rc;

	/* First, remove namespaces that no longer exist */
	RB_FOREACH_SAFE(ns, nvme_ns_tree, &ctrlr->ns, tmp_ns) {
		nsid = new_ns_list[0];
		active_ns_count = 0;
		while (nsid != 0) {
			if (nsid == ns->id) {
				break;
			}

			nsid = new_ns_list[active_ns_count++];
		}

		if (nsid != ns->id) {
			/* Did not find this namespace id in the new list. */
			NVME_CTRLR_DEBUGLOG(ctrlr, "Namespace %u was removed\n", ns->id);
			nvme_ctrlr_destruct_namespace(ctrlr, ns->id);
		}
	}

	/* Next, add new namespaces */
	active_ns_count = 0;
	for (i = 0; i < max_entries; i++) {
		nsid = new_ns_list[active_ns_count];

		if (nsid == 0) {
			break;
		}

		/* If the namespace already exists, this will not construct it a second time. */
		rc = nvme_ctrlr_construct_namespace(ctrlr, nsid);
		if (rc != 0) {
			/* We can't easily handle a failure here. But just move on. */
			assert(false);
			NVME_CTRLR_DEBUGLOG(ctrlr, "Failed to allocate a namespace object.\n");
			continue;
		}

		active_ns_count++;
	}

	ctrlr->active_ns_count = active_ns_count;
}

/*
 * [한국어]
 * nvme_ctrlr_identify_active_ns_async_done -
 *   CNS 0x02 (Active NS List) Identify admin 명령의 완료 콜백.
 *
 * @arg: nvme_active_ns_ctx — identify chain 상태 컨테이너 (active_ns_async 가
 *       발행 시점에 등록).
 * @cpl: NVMe Completion Queue Entry — 명령 결과 (status + cdw0/1).
 * @return: void (콜백)
 *
 * 동작 (Active NS List paging):
 *  NVMe 1.1+ 의 Identify CNS=0x02 는 한 번에 최대 1024개 NSID 만 반환한다.
 *  네임스페이스 수가 더 많으면 마지막 NSID 를 다음 호출의 시작 NSID 로
 *  넘겨 다음 1024개를 받는다 — "페이징" 패턴. 본 콜백이 그 페이징 루프의 핵심.
 *  단계:
 *   1. timed_out 이면 즉시 ctx 정리 (메모리 누수 방지). 이후 콜백 재진입 차단.
 *   2. cpl 을 ctx->status.cpl 에 보관 (호출자 분석 가능). 에러면 ERROR 상태.
 *   3. 직전 페이지의 마지막 슬롯 (`[1024 * page_count - 1]`) 을 read →
 *      0 이면 "더 없음" 으로 페이징 종료 (DONE 상태). 비제로면 next_nsid.
 *   4. 더 받아야 하면 page_count++ 하고 buffer realloc (페이지 1개 = 1024
 *      uint32_t = 4KiB) — 컨트롤러 page_size 정렬 필수.
 *   5. timeout_tsc 재계산 후 다음 페이지 발행 (재귀적으로 active_ns_async 호출).
 *
 * 실행 컨텍스트: admin 큐 완료를 폴링하는 reactor 스레드.
 *   (spdk_nvme_ctrlr_process_admin_completions 가 트리거.)
 * 에러 경로: ERROR/DONE 모두 `out:` 으로 점프해 ctx->deleter (보통
 *   `_nvme_active_ns_ctx_deleter`) 호출 — 자원 정리·상태 전이 한 곳에서.
 *
 * 호출 체인:
 *   nvme_ctrlr_cmd_identify(CNS=0x02) 완료 → [본 콜백]
 *     → (계속) nvme_ctrlr_identify_active_ns_async (다음 페이지)
 *     → (종료) ctx->deleter → _nvme_active_ns_ctx_deleter
 *       → nvme_ctrlr_identify_active_ns_swap (실제 ns 트리 교체)
 */
static void
nvme_ctrlr_identify_active_ns_async_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	/* [한국어] arg 캐스팅 — admin cmd 발행 시 ctx 를 cb_arg 로 등록했으므로 동일 객체. */
	struct nvme_active_ns_ctx *ctx = arg;
	uint32_t *new_ns_list = NULL;
	/* [한국어] realloc 결과 임시 보관 — 실패해도 ctx->new_ns_list 는 유지(누수 방지). */

	/* [한국어] timed_out 검사 우선 — 이미 호출자 측에서 timeout 처리됐을 수 있다.
	 * 이 경우 ctx 의 유효성 보장이 깨졌으므로 즉시 destroy 후 반환.
	 * (admin completion polling 이 늦게 도착한 경우 흔히 발생.) */
	if (ctx->status.timed_out) {
		nvme_active_ns_ctx_destroy(ctx);
		return;
	}

	/* [한국어] cpl 사본 보관 — 호출자가 status.cpl 로 결과 코드 확인 가능. */
	memcpy(&ctx->status.cpl, cpl, sizeof(*cpl));
	/* [한국어] NVMe spec status 검사 — SC|SCT 가 0 이 아니면 에러. */
	if (spdk_nvme_cpl_is_error(cpl)) {
		ctx->state = NVME_ACTIVE_NS_STATE_ERROR;
		goto out;
	}

	/* [한국어] 직전 페이지의 마지막 슬롯 = 다음 페이지의 starting NSID.
	 * 1024 * page_count - 1 = 직전 페이지 끝 인덱스 (0-based).
	 * NVMe 1.1+ 스펙: CNS 0x02 의 응답 마지막 NSID 가 0 = "더 없음", 비제로 = 다음 page 시작. */
	ctx->next_nsid = ctx->new_ns_list[1024 * ctx->page_count - 1];
	if (ctx->next_nsid == 0) {
		/* [한국어] 페이징 완료 — 모든 active NS 수집 끝. */
		ctx->state = NVME_ACTIVE_NS_STATE_DONE;
		goto out;
	}

	/* [한국어] 추가 페이지 필요 — buffer 확장. */
	ctx->page_count++;
	/* [한국어] page_size 정렬 spdk_realloc — DMA 매핑 호환을 위해 컨트롤러 MPS 사용.
	 * 실패 시 기존 buffer 유지 + ERROR 전이 (NULL 덮어쓰지 않음). */
	new_ns_list = spdk_realloc(ctx->new_ns_list,
				   ctx->page_count * sizeof(struct spdk_nvme_ns_list),
				   ctx->ctrlr->page_size);
	if (!new_ns_list) {
		NVME_CTRLR_ERRLOG(ctx->ctrlr, "Failed to reallocate active_ns_list!\n");
		ctx->state = NVME_ACTIVE_NS_STATE_ERROR;
		goto out;
	}

	ctx->new_ns_list = new_ns_list;
	/* [한국어] 다음 페이지용 timeout_tsc 재설정 — admin_timeout_ms 단위.
	 * ms → us → ticks 변환: ms * 1000 * (ticks/sec) / (us/sec=1e6) = ms * ticks_hz / 1000. */
	ctx->status.timeout_tsc = spdk_get_ticks() + ctx->ctrlr->opts.admin_timeout_ms * 1000 *
				  spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
	/* [한국어] 재귀적으로 다음 페이지 발행 — 본 콜백이 다시 호출될 것임. */
	nvme_ctrlr_identify_active_ns_async(ctx);
	return;

out:
	/* [한국어] DONE 또는 ERROR 도달 시 공통 정리. */
	ctx->status.done = true;
	/* [한국어] deleter 가 등록돼 있으면 (비동기 경로의 경우) 호출 — 자원 정리 +
	 * 상태 전이 (_nvme_active_ns_ctx_deleter 가 swap·destroy·set_state 수행).
	 * 동기 경로 (nvme_ctrlr_identify_active_ns) 는 deleter=NULL 로 호출하므로
	 * 호출자가 직접 정리. */
	if (ctx->deleter) {
		ctx->deleter(ctx);
	}
}

/*
 * [한국어]
 * nvme_ctrlr_identify_active_ns_async -
 *   Active NS List (CNS 0x02) Identify 발행. 또는 컨트롤러가 미지원이면
 *   "전체 NS 가 active" 라고 가정한 가짜 목록을 채워 동기 완료시킨다.
 *
 * @ctx: identify chain 상태 컨테이너. 호출자가 nvme_active_ns_ctx_create 로
 *       만들어 전달. ctx->ctrlr / ctx->new_ns_list / ctx->next_nsid 사용.
 * @return: void (비동기) — 결과는 콜백 또는 deleter 로.
 *
 * 동작:
 *   1. cdata.nn == 0 (네임스페이스 없음) → DONE 직행.
 *   2. 컨트롤러 버전 < NVMe 1.1 또는 NVME_QUIRK_IDENTIFY_CNS quirk 가 있으면
 *      CNS 0x02 미지원으로 간주 → "1..nn 전체가 active" 라는 가짜 목록 생성
 *      (마지막 슬롯에 0 sentinel) 후 DONE.
 *   3. 그 외 → CNS 0x02 admin Identify 발행, 완료는 active_ns_async_done 콜백.
 *
 * 페이징과의 관계: 본 함수는 페이징 루프의 "한 페이지 발행" 단위. done 콜백이
 * next_nsid 갱신 후 본 함수를 다시 호출 — 페이지가 모두 끝날 때까지 반복.
 *
 * 실행 컨텍스트: reactor 스레드 (admin 발행). state machine 전이는
 *   PROCESSING ↔ DONE/ERROR.
 *
 * 호출 체인:
 *   _nvme_ctrlr_identify_active_ns / nvme_ctrlr_identify_active_ns / done callback
 *     → [본 함수]
 *     → nvme_ctrlr_cmd_identify(CNS=0x02)
 *     → (콜백) nvme_ctrlr_identify_active_ns_async_done
 */
static void
nvme_ctrlr_identify_active_ns_async(struct nvme_active_ns_ctx *ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;
	uint32_t i;
	int rc;

	/* [한국어] 네임스페이스 0개 → 발행할 것 없음. cdata.nn 은 직전 Identify
	 * Controller 결과의 NN 필드 (네임스페이스 총 개수). */
	if (ctrlr->cdata.nn == 0) {
		ctx->state = NVME_ACTIVE_NS_STATE_DONE;
		goto out;
	}

	/* [한국어] new_ns_list 는 ctx_create 가 1 페이지로 미리 할당해두므로 항상 비-NULL. */
	assert(ctx->new_ns_list != NULL);

	/*
	 * If controller doesn't support active ns list CNS 0x02 dummy up
	 * an active ns list, i.e. all namespaces report as active
	 */
	/* [한국어] NVMe 1.0 또는 quirk → CNS 0x02 미지원. 1..nn 을 그대로 active 목록으로
	 * 채워준다 (전체 namespace 가 active 라고 가정). */
	if (ctrlr->vs.raw < SPDK_NVME_VERSION(1, 1, 0) || ctrlr->quirks & NVME_QUIRK_IDENTIFY_CNS) {
		uint32_t *new_ns_list;

		/*
		 * Active NS list must always end with zero element.
		 * So, we allocate for cdata.nn+1.
		 */
		/* [한국어] 0 sentinel 자리까지 포함해 nn+1 개 슬롯. 페이지 단위로 올림.
		 * sizeof(ns_list)/sizeof(uint32_t) = 1024 — 페이지당 1024 NSID 슬롯. */
		ctx->page_count = spdk_divide_round_up(ctrlr->cdata.nn + 1,
						       sizeof(struct spdk_nvme_ns_list) / sizeof(new_ns_list[0]));
		new_ns_list = spdk_realloc(ctx->new_ns_list,
					   ctx->page_count * sizeof(struct spdk_nvme_ns_list),
					   ctx->ctrlr->page_size);
		if (!new_ns_list) {
			NVME_CTRLR_ERRLOG(ctrlr, "Failed to reallocate active_ns_list!\n");
			ctx->state = NVME_ACTIVE_NS_STATE_ERROR;
			goto out;
		}

		ctx->new_ns_list = new_ns_list;
		/* [한국어] nn 위치에 0 sentinel — done 콜백이 페이징 종료 판정에 쓰는 규약. */
		ctx->new_ns_list[ctrlr->cdata.nn] = 0;
		/* [한국어] NSID 는 1-based — 0..nn-1 인덱스에 1..nn 값을 채움. */
		for (i = 0; i < ctrlr->cdata.nn; i++) {
			ctx->new_ns_list[i] = i + 1;
		}

		ctx->state = NVME_ACTIVE_NS_STATE_DONE;
		goto out;
	}

	/* [한국어] 정상 경로 — CNS 0x02 admin Identify 발행.
	 * - nsid=next_nsid: 페이징의 시작 NSID (첫 호출은 0, 이후엔 직전 page 의 last + 1 의미).
	 * - 목적지 버퍼: new_ns_list 의 (page_count-1) 번째 페이지 슬롯 — page_count 가 항상 ≥1. */
	ctx->state = NVME_ACTIVE_NS_STATE_PROCESSING;
	rc = nvme_ctrlr_cmd_identify(ctrlr, SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST, 0, ctx->next_nsid, 0,
				     &ctx->new_ns_list[1024 * (ctx->page_count - 1)], sizeof(struct spdk_nvme_ns_list),
				     nvme_ctrlr_identify_active_ns_async_done, ctx);
	if (rc != 0) {
		ctx->state = NVME_ACTIVE_NS_STATE_ERROR;
		goto out;
	}

	return;

out:
	/* [한국어] DONE 또는 ERROR 도달 시 deleter 호출 (등록돼 있을 때만).
	 * deleter 없는 동기 경로는 호출자가 직접 정리. */
	if (ctx->deleter) {
		ctx->deleter(ctx);
	}
}

/*
 * [한국어]
 * _nvme_active_ns_ctx_deleter -
 *   active NS list 수집이 끝났을 때 호출되는 deleter 콜백.
 *   ERROR/DONE 양쪽 분기 처리 + ns 트리 교체 + 다음 state 전이.
 *
 * @ctx: identify chain 상태 컨테이너.
 *
 * 동작:
 *   1. ERROR 면: ctx 해제 + CTRLR_STATE_ERROR 전이 → init state machine 중단.
 *   2. DONE 이면:
 *      a. 기존 NS 트리의 각 ns 에 대해 iocs-specific 데이터 해제 (다음 단계에서
 *         새로 받을 것이므로 stale 제거).
 *      b. active_ns_swap 으로 ctrlr->ns 트리를 새 목록으로 교체.
 *      c. ctx 해제.
 *      d. CTRLR_STATE_IDENTIFY_NS 로 전이 — 다음 단계는 ns 1개씩 Identify NS.
 *
 * 비동기 경로 전용: ctx_create 에 deleter=본 함수로 등록한 경우만 호출.
 * 동기 경로는 nvme_ctrlr_identify_active_ns 가 직접 swap·destroy 수행.
 *
 * 실행 컨텍스트: reactor 스레드 (admin completion 폴링 콜백 안에서).
 *
 * 호출 체인:
 *   nvme_ctrlr_identify_active_ns_async_done → ctx->deleter → [본 함수]
 *     → nvme_ctrlr_identify_active_ns_swap (트리 교체)
 *     → nvme_ctrlr_set_state(IDENTIFY_NS) — 다음 단계로 전이
 */
static void
_nvme_active_ns_ctx_deleter(struct nvme_active_ns_ctx *ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;
	struct spdk_nvme_ns *ns;

	/* [한국어] ERROR 면 정리 후 state machine 중단 — 이후 controller 사용 불가. */
	if (ctx->state == NVME_ACTIVE_NS_STATE_ERROR) {
		nvme_active_ns_ctx_destroy(ctx);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	/* [한국어] state machine 불변식 — async 콜백은 DONE/ERROR 둘 중 하나만 도달. */
	assert(ctx->state == NVME_ACTIVE_NS_STATE_DONE);

	/* [한국어] 기존 ns 트리의 모든 ns 의 iocs-specific data 해제 — 다음 IDENTIFY_NS
	 * 단계에서 새로 받을 것이므로 stale 데이터를 미리 비운다. RB_FOREACH 는 RB-tree
	 * in-order traversal (NSID 오름차순). */
	RB_FOREACH(ns, nvme_ns_tree, &ctrlr->ns) {
		nvme_ns_free_iocs_specific_data(ns);
	}

	/* [한국어] 새 active NS 목록을 ctrlr->ns 트리에 반영 — 추가된 ns insert, 제거된 ns
	 * delete. page_count * 1024 = 새 목록의 최대 슬롯 수 (마지막 0 sentinel 포함).
	 * swap 함수 안에서 active_ns_count 도 갱신. */
	nvme_ctrlr_identify_active_ns_swap(ctrlr, ctx->new_ns_list, ctx->page_count * 1024);
	nvme_active_ns_ctx_destroy(ctx);
	/* [한국어] 다음 단계: 각 active NS 에 대해 Identify Namespace (CNS 0x00) 발행.
	 * 시작은 nvme_ctrlr_identify_namespaces 가 처리. */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_NS, ctrlr->opts.admin_timeout_ms);
}

/*
 * [한국어]
 * _nvme_ctrlr_identify_active_ns -
 *   state machine 의 IDENTIFY_ACTIVE_NS 진입점.
 *
 * @ctrlr: 대상 컨트롤러.
 *
 * 동작: ctx 객체 생성 → deleter 등록 → state 를 WAIT_FOR_IDENTIFY_ACTIVE_NS
 * 로 전이 (timeout 감시 활성화) → 첫 페이지 발행. 이후 모든 후속 처리는
 * async 콜백/deleter 체인이 이어 받는다.
 *
 * 실행 컨텍스트: state machine 처리 루프 (process_init).
 *
 * 호출 체인:
 *   nvme_ctrlr_process_init (state IDENTIFY_ACTIVE_NS)
 *     → [본 함수]
 *     → nvme_ctrlr_identify_active_ns_async (첫 페이지)
 *     → (async 완료 chain)
 *     → _nvme_active_ns_ctx_deleter → state IDENTIFY_NS
 */
static void
_nvme_ctrlr_identify_active_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_active_ns_ctx *ctx;

	/* [한국어] deleter 콜백 등록 — 비동기 종료 시 자동 swap/state 전이 보장. */
	ctx = nvme_active_ns_ctx_create(ctrlr, _nvme_active_ns_ctx_deleter);
	if (!ctx) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	/* [한국어] WAIT_FOR_IDENTIFY_ACTIVE_NS 로 전이 — admin_timeout_ms 안에 끝나야 함. */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ACTIVE_NS,
			     ctrlr->opts.admin_timeout_ms);
	/* [한국어] 첫 페이지 발행 — 후속은 콜백 chain. */
	nvme_ctrlr_identify_active_ns_async(ctx);
}

/*
 * [한국어]
 * nvme_ctrlr_identify_active_ns -
 *   Active NS list 수집의 동기 버전. 결과를 ctrlr->ns 트리에 즉시 swap.
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 0 성공. -ENOMEM (ctx 할당 실패) / -ENXIO (admin 명령 실패 또는 timeout).
 *
 * 차이점 (vs _nvme_ctrlr_identify_active_ns):
 *   - 동기: 본 함수는 호출 즉시 nvme_wait_for_adminq_completion 으로 polling 완료까지 block.
 *   - deleter=NULL: 호출자가 직접 swap·destroy 수행.
 *   - state machine 전이 없음: spdk_nvme_ctrlr_reset 등 reset 경로에서 호출됨.
 *
 * 동기 폴링 패턴: nvme_wait_for_adminq_completion 이 timeout 또는 status.done=true
 * 까지 admin completion 을 폴링한다. async 콜백 chain 이 status.done 을 세팅.
 *
 * 실행 컨텍스트: reset / NS rescan 등 동기 호출 경로 (reactor 스레드).
 *
 * 호출 체인:
 *   spdk_nvme_ctrlr_reset / 외부 → [본 함수]
 *     → nvme_ctrlr_identify_active_ns_async (first page)
 *     → nvme_wait_for_adminq_completion (block until done)
 *     → nvme_ctrlr_identify_active_ns_swap (swap directly)
 */
int
nvme_ctrlr_identify_active_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_active_ns_ctx *ctx;
	int rc;

	/* [한국어] deleter 없이 ctx 생성 — 동기 경로에서는 본 함수가 직접 정리. */
	ctx = nvme_active_ns_ctx_create(ctrlr, NULL);
	if (!ctx) {
		return -ENOMEM;
	}

	/* [한국어] 첫 페이지 발행 — 컨트롤러가 NVMe 1.0 / quirk 면 즉시 DONE
	 * (admin 명령 없이 가짜 목록 채움), 그 외에는 admin 명령이 in-flight 로. */
	nvme_ctrlr_identify_active_ns_async(ctx);
	if (ctx->state == NVME_ACTIVE_NS_STATE_ERROR) {
		nvme_active_ns_ctx_destroy(ctx);
		return -ENXIO;
	}

	/* [한국어] admin completion 폴링 — status.done 또는 timeout 까지 block.
	 * 두 번째 인자 false = "command 자체의 status 검사는 호출자가 수행".
	 * 페이징이 여러 번 발생해도 마지막 콜백이 status.done=true 로 세팅하면 빠져나옴. */
	rc = nvme_wait_for_adminq_completion(ctrlr, &ctx->status, false);
	if (rc || ctx->state == NVME_ACTIVE_NS_STATE_ERROR) {
		/* [한국어] timed_out 이면 done 콜백이 ctx 를 이미 destroy 했을 수 있음 — 중복 destroy 금지. */
		if (!ctx->status.timed_out) {
			nvme_active_ns_ctx_destroy(ctx);
		}

		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_ctrlr_identify_active_ns_async failed: rc=%s\n",
				  spdk_strerror(abs(rc)));
		return -ENXIO;
	}

	/* [한국어] 성공 — swap·destroy 직접 수행 (deleter 없는 동기 경로의 책임). */
	assert(ctx->state == NVME_ACTIVE_NS_STATE_DONE);
	nvme_ctrlr_identify_active_ns_swap(ctrlr, ctx->new_ns_list, ctx->page_count * 1024);
	nvme_active_ns_ctx_destroy(ctx);
	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_identify_ns_async_done -
 *   Identify Namespace (CNS 0x00) 완료 콜백. 한 ns 가 끝나면 다음 active ns 로 진행.
 *
 * @arg: 처리 중이던 spdk_nvme_ns. admin 발행 시 cb_arg 로 등록한 동일 객체.
 * @cpl: 완료 entry.
 *
 * 동작 (per-ns identify chain):
 *   1. handle_identify_ns_completion 으로 cpl 검사 + nsdata 후처리 (ABORT 등 분기).
 *      실패면 controller ERROR 전이.
 *   2. nvme_ns_set_identify_data 로 nsdata 의 필드를 ns 객체에 캐싱 (sector_size,
 *      sectors, md_size, pi_type 등).
 *   3. get_next_active_ns 로 다음 active NSID 조회. NULL 이면 모든 active ns 의
 *      Identify Namespace 가 끝났다는 의미 → CTRLR_STATE_IDENTIFY_ID_DESCS 로 전이.
 *   4. 다음 ns 가 있으면 nvme_ctrlr_identify_ns_async 재호출 — 다음 콜백이 본 함수.
 *
 * 결과적으로 본 함수는 "active NS 1개씩 순차 처리" 의 루프 본체 역할.
 *
 * 실행 컨텍스트: reactor 스레드 (admin completion).
 *
 * 호출 체인:
 *   nvme_ctrlr_cmd_identify(CNS=0x00) 완료 → [본 콜백]
 *     → (next ns 있음) nvme_ctrlr_identify_ns_async → 다시 본 콜백
 *     → (전부 끝) state IDENTIFY_ID_DESCS → nvme_ctrlr_identify_id_desc_async
 */
static void
nvme_ctrlr_identify_ns_async_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ns *ns = (struct spdk_nvme_ns *)arg;
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	uint32_t nsid;
	int rc;

	/* [한국어] cpl 검사 + nsdata 후처리 (lbaf/pi 등 도출). 실패면 controller 자체를 ERROR. */
	if (nvme_ctrlr_handle_identify_ns_completion(ctrlr, ns, cpl)) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	/* [한국어] nsdata → ns 의 hot 필드들로 복사 (sector_size, extended_lba_size,
	 * md_size, pi_type 등). bdev/io path 가 매번 nsdata 를 파싱하지 않게. */
	nvme_ns_set_identify_data(ns);

	/* move on to the next active NS */
	/* [한국어] active NS 트리에서 다음 NSID 조회 — 0 = 더 없음. */
	nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, ns->id);
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		/* [한국어] 모든 active NS 의 CNS=0x00 완료 — 다음 단계 IDENTIFY_ID_DESCS.
		 * (NS ID descriptor list CNS=0x03 — NSID 의 NGUID/EUI64/UUID 조회) */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_ID_DESCS,
				     ctrlr->opts.admin_timeout_ms);
		return;
	}
	/* [한국어] 다음 ns 객체의 ctrlr/id 필드 재설정 (방어적 코딩 — get_ns 가 반환한
	 * 객체는 이미 ctrlr/id 가 채워져 있어야 하지만 swap 직후 초기 상태 보장). */
	ns->ctrlr = ctrlr;
	ns->id = nsid;

	/* [한국어] 다음 ns 의 Identify NS 발행 — 콜백이 본 함수로 재진입. */
	rc = nvme_ctrlr_identify_ns_async(ns);
	if (rc) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}
}

/*
 * [한국어]
 * nvme_ctrlr_identify_ns_async -
 *   주어진 ns 1개에 대해 Identify Namespace (CNS 0x00) admin 발행.
 *
 * @ns: 대상 ns. ns->id (NSID), ns->ctrlr (소속 컨트롤러), ns->nsdata
 *      (결과 저장 버퍼 — 4096바이트) 사용.
 * @return: 0 = 발행 성공 (비동기), 음수 = -errno.
 *
 * NVMe spec: CNS 0x00 = Identify Namespace, NSID 지정, 데이터 4096B.
 * 결과 nsdata 에는 NSZE/NCAP/NUSE/NSFEAT/LBAF[16]/DPS/PI 등 ns 메타데이터.
 *
 * state 전이: 호출 시점에 WAIT_FOR_IDENTIFY_NS 로 전환 — admin_timeout_ms 안에
 *   콜백이 도달해야 함. timeout 시 state machine 이 ERROR.
 *
 * 호출 체인:
 *   nvme_ctrlr_identify_namespaces (첫 ns)
 *   또는 nvme_ctrlr_identify_ns_async_done (다음 ns)
 *     → [본 함수]
 *     → nvme_ctrlr_cmd_identify(CNS=0x00)
 *     → (콜백) nvme_ctrlr_identify_ns_async_done
 */
static int
nvme_ctrlr_identify_ns_async(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	struct spdk_nvme_ns_data *nsdata;

	/* [한국어] nsdata 는 ns 내부 4KiB 버퍼 — admin 응답을 직접 받음. */
	nsdata = &ns->nsdata;

	/* [한국어] state 전이 — 콜백이 도착하기 전까지 WAIT_FOR_IDENTIFY_NS.
	 * admin_timeout_ms 가 지나면 process_init 가 timeout 처리. */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS,
			     ctrlr->opts.admin_timeout_ms);
	/* [한국어] CNS=0x00 발행 — NSID=ns->id, CSI=0 (NVM Command Set), CNTID 무시. */
	return nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS, 0, ns->id, 0,
				       nsdata, sizeof(*nsdata),
				       nvme_ctrlr_identify_ns_async_done, ns);
}

/*
 * [한국어]
 * nvme_ctrlr_identify_namespaces -
 *   state machine 의 IDENTIFY_NS 진입점. 첫 active NS 에 대해 Identify NS 발행.
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 0 = 발행 성공 또는 active NS 없음. 음수 = 발행 실패.
 *
 * 동작:
 *   1. 첫 active NSID 조회 (active_ns 트리에서 최소값).
 *   2. ns 가 없으면 (= active NS 0개) → IDENTIFY_ID_DESCS 로 건너뜀.
 *   3. ns 가 있으면 → ctrlr/id 설정 후 identify_ns_async 호출.
 *      이후 done 콜백 chain 이 모든 active ns 를 순회.
 *
 * 호출 체인:
 *   nvme_ctrlr_process_init (state IDENTIFY_NS)
 *     → [본 함수]
 *     → nvme_ctrlr_identify_ns_async (첫 ns)
 *     → (async chain) 모든 ns 완료 → state IDENTIFY_ID_DESCS
 */
static int
nvme_ctrlr_identify_namespaces(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t nsid;
	struct spdk_nvme_ns *ns;
	int rc;

	/* [한국어] active NS 트리의 최소 NSID. 0 = 없음. */
	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		/* No active NS, move on to the next state */
		/* [한국어] active NS 0개 — IDENTIFY_NS 단계 건너뛰고 IDENTIFY_ID_DESCS 로. */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_ID_DESCS,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	/* [한국어] 첫 ns 의 ctrlr/id 세팅 후 async chain 시작. */
	ns->ctrlr = ctrlr;
	ns->id = nsid;

	rc = nvme_ctrlr_identify_ns_async(ns);
	if (rc) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}

	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_identify_namespaces_iocs_specific_next -
 *   I/O Command Set Specific Identify Namespace (CNS 0x05 ZNS / 0x08 NVM-specific)
 *   를 active NS 마다 순회 발행. ZNS·NVM 분기는 ns->csi 기반.
 *
 * @ctrlr: 대상 컨트롤러.
 * @prev_nsid: 직전 처리한 NSID. 0 이면 첫 호출 (= first active ns 부터 시작).
 * @return: 0 = 발행 성공 또는 처리 완료, 음수 = 발행 실패.
 *
 * 동작:
 *   1. prev_nsid == 0 → get_first_active_ns, 그 외 → get_next_active_ns(prev_nsid).
 *   2. 더 없으면 IDENTIFY_NS_IOCS_SPECIFIC 단계 종료 → SET_SUPPORTED_INTEL_LOG_PAGES 로.
 *   3. ns->csi 에 따라 zns_specific_async 또는 nvm_specific_async 발행.
 *   4. 각 콜백 (zns/nvm_specific_async_done) 이 본 함수를 prev_nsid=현재 ns->id 로 재호출.
 *
 * iterator 패턴 — 외부 chain 의 다음 단계로 넘어가기 전에 모든 active ns 의
 * I/O command set specific data 를 수집한다.
 *
 * 호출 체인:
 *   nvme_ctrlr_identify_namespaces_iocs_specific (chain 시작, prev_nsid=0)
 *   또는 zns/nvm_specific_async_done (다음 ns)
 *     → [본 함수]
 *     → ns->csi 분기 → identify_ns_zns_specific_async / identify_ns_nvm_specific_async
 *     → (async chain) 또는 다음 state 전이
 */
static int
nvme_ctrlr_identify_namespaces_iocs_specific_next(struct spdk_nvme_ctrlr *ctrlr, uint32_t prev_nsid)
{
	uint32_t nsid;
	struct spdk_nvme_ns *ns;
	int rc;

	if (!prev_nsid) {
		nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	} else {
		/* move on to the next active NS */
		nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, prev_nsid);
	}

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		/* No first/next active NS, move on to the next state */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_LOG_PAGES,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	/* loop until we find a ns which has (supported) iocs specific data */
	while (!nvme_ns_has_supported_iocs_specific_data(ns)) {
		nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, ns->id);
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			/* no namespace with (supported) iocs specific data found */
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_LOG_PAGES,
					     ctrlr->opts.admin_timeout_ms);
			return 0;
		}
	}

	rc = nvme_ctrlr_identify_ns_iocs_specific_async(ns);
	if (rc) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}

	return rc;
}

static void
nvme_ctrlr_identify_ns_zns_specific_async_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ns *ns = (struct spdk_nvme_ns *)arg;
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;

	if (nvme_ctrlr_handle_identify_ns_completion(ctrlr, ns, cpl)) {
		nvme_ns_free_zns_specific_data(ns);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	nvme_ctrlr_identify_namespaces_iocs_specific_next(ctrlr, ns->id);
}

static int
nvme_ctrlr_identify_ns_zns_specific_async(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	int rc;

	assert(!ns->nsdata_zns);
	ns->nsdata_zns = spdk_zmalloc(sizeof(*ns->nsdata_zns), 64, NULL, SPDK_ENV_NUMA_ID_ANY,
				      SPDK_MALLOC_SHARE);
	if (!ns->nsdata_zns) {
		return -ENOMEM;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS_IOCS_SPECIFIC,
			     ctrlr->opts.admin_timeout_ms);
	rc = nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS_IOCS, 0, ns->id, ns->csi,
				     ns->nsdata_zns, sizeof(*ns->nsdata_zns),
				     nvme_ctrlr_identify_ns_zns_specific_async_done, ns);
	if (rc) {
		nvme_ns_free_zns_specific_data(ns);
	}

	return rc;
}

static void
nvme_ctrlr_identify_ns_nvm_specific_async_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ns *ns = (struct spdk_nvme_ns *)arg;
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;

	if (nvme_ctrlr_handle_identify_ns_completion(ctrlr, ns, cpl)) {
		nvme_ns_free_nvm_specific_data(ns);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	nvme_ctrlr_identify_namespaces_iocs_specific_next(ctrlr, ns->id);
}

static int
nvme_ctrlr_identify_ns_nvm_specific_async(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	int rc;

	assert(!ns->nsdata_nvm);
	ns->nsdata_nvm = spdk_zmalloc(sizeof(*ns->nsdata_nvm), 64, NULL, SPDK_ENV_NUMA_ID_ANY,
				      SPDK_MALLOC_SHARE);
	if (!ns->nsdata_nvm) {
		return -ENOMEM;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS_IOCS_SPECIFIC,
			     ctrlr->opts.admin_timeout_ms);
	rc = nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS_IOCS, 0, ns->id, ns->csi,
				     ns->nsdata_nvm, sizeof(*ns->nsdata_nvm),
				     nvme_ctrlr_identify_ns_nvm_specific_async_done, ns);
	if (rc) {
		nvme_ns_free_nvm_specific_data(ns);
	}

	return rc;
}

static int
nvme_ctrlr_identify_ns_iocs_specific_async(struct spdk_nvme_ns *ns)
{
	switch (ns->csi) {
	case SPDK_NVME_CSI_ZNS:
		return nvme_ctrlr_identify_ns_zns_specific_async(ns);
	case SPDK_NVME_CSI_NVM:
		if (ns->ctrlr->cdata.ctratt.bits.elbas) {
			return nvme_ctrlr_identify_ns_nvm_specific_async(ns);
		}
	/* fallthrough */
	default:
		/*
		 * This switch must handle all cases for which
		 * nvme_ns_has_supported_iocs_specific_data() returns true,
		 * other cases should never happen.
		 */
		assert(0);
	}

	return -EINVAL;
}

static int
nvme_ctrlr_identify_namespaces_iocs_specific(struct spdk_nvme_ctrlr *ctrlr)
{
	if (!nvme_ctrlr_multi_iocs_enabled(ctrlr)) {
		/* Multi IOCS not supported/enabled, move on to the next state */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_LOG_PAGES,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	return nvme_ctrlr_identify_namespaces_iocs_specific_next(ctrlr, 0);
}

static void
nvme_ctrlr_identify_id_desc_async_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ns *ns = (struct spdk_nvme_ns *)arg;
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	uint32_t nsid;
	int rc;

	if (nvme_ctrlr_handle_identify_ns_completion(ctrlr, ns, cpl)) {
		/*
		 * Many controllers claim to be compatible with NVMe 1.3, however,
		 * they do not implement NS ID Desc List. Therefore, instead of setting
		 * the state to NVME_CTRLR_STATE_ERROR, silently ignore the completion
		 * error and move on to the next state.
		 *
		 * The proper way is to create a new quirk for controllers that violate
		 * the NVMe 1.3 spec by not supporting NS ID Desc List.
		 * (Re-using the NVME_QUIRK_IDENTIFY_CNS quirk is not possible, since
		 * it is too generic and was added in order to handle controllers that
		 * violate the NVMe 1.1 spec by not supporting ACTIVE LIST).
		 */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC,
				     ctrlr->opts.admin_timeout_ms);
		return;
	}

	nvme_ns_set_id_desc_list_data(ns);

	/* move on to the next active NS */
	nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, ns->id);
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC,
				     ctrlr->opts.admin_timeout_ms);
		return;
	}

	rc = nvme_ctrlr_identify_id_desc_async(ns);
	if (rc) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}
}

static int
nvme_ctrlr_identify_id_desc_async(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;

	memset(ns->id_desc_list, 0, sizeof(ns->id_desc_list));

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ID_DESCS,
			     ctrlr->opts.admin_timeout_ms);
	return nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST,
				       0, ns->id, 0, ns->id_desc_list, sizeof(ns->id_desc_list),
				       nvme_ctrlr_identify_id_desc_async_done, ns);
}

static int
nvme_ctrlr_identify_id_desc_namespaces(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t nsid;
	struct spdk_nvme_ns *ns;
	int rc;

	if ((ctrlr->vs.raw < SPDK_NVME_VERSION(1, 3, 0) &&
	     !(ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_IOCS)) ||
	    (ctrlr->quirks & NVME_QUIRK_IDENTIFY_CNS)) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Version < 1.3; not attempting to retrieve NS ID Descriptor List\n");
		/* NS ID Desc List not supported, move on to the next state */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		/* No active NS, move on to the next state */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	rc = nvme_ctrlr_identify_id_desc_async(ns);
	if (rc) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}

	return rc;
}

static void
nvme_ctrlr_update_nvmf_ioccsz(struct spdk_nvme_ctrlr *ctrlr)
{
	if (spdk_nvme_ctrlr_is_fabrics(ctrlr)) {
		if (ctrlr->cdata.nvmf_specific.ioccsz < 4) {
			NVME_CTRLR_ERRLOG(ctrlr, "Incorrect IOCCSZ %u, the minimum value should be 4\n",
					  ctrlr->cdata.nvmf_specific.ioccsz);
			ctrlr->cdata.nvmf_specific.ioccsz = 4;
			assert(0);
		}
		ctrlr->ioccsz_bytes = ctrlr->cdata.nvmf_specific.ioccsz * 16 - sizeof(struct spdk_nvme_cmd);
		ctrlr->icdoff = ctrlr->cdata.nvmf_specific.icdoff;
	}
}

static void
nvme_ctrlr_set_num_queues_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	uint32_t cq_allocated, sq_allocated, min_allocated, i;
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Set Features - Number of Queues failed!\n");
		ctrlr->opts.num_io_queues = 0;
	} else {
		/*
		 * Data in cdw0 is 0-based.
		 * Lower 16-bits indicate number of submission queues allocated.
		 * Upper 16-bits indicate number of completion queues allocated.
		 */
		sq_allocated = (cpl->cdw0 & 0xFFFF) + 1;
		cq_allocated = (cpl->cdw0 >> 16) + 1;

		/*
		 * For 1:1 queue mapping, set number of allocated queues to be minimum of
		 * submission and completion queues.
		 */
		min_allocated = spdk_min(sq_allocated, cq_allocated);

		/* Set number of queues to be minimum of requested and actually allocated. */
		ctrlr->opts.num_io_queues = spdk_min(min_allocated, ctrlr->opts.num_io_queues);

		if (ctrlr->opts.enable_interrupts) {
			if (ctrlr->quirks & NVME_QUIRK_MSIX_VECTOR_COUNT) {
				/* This controller does not allocate enough vectors for
				 * each IO queue plus the admin queue. So decrement the number
				 * of IO queues we will allow by one.
				 */
				ctrlr->opts.num_io_queues--;
			}
			ctrlr->opts.num_io_queues = spdk_min(MAX_IO_QUEUES_WITH_INTERRUPTS,
							     ctrlr->opts.num_io_queues);
			if (nvme_transport_ctrlr_enable_interrupts(ctrlr) < 0) {
				NVME_CTRLR_ERRLOG(ctrlr, "Failed to enable interrupts!\n");
				ctrlr->opts.enable_interrupts = false;
			}
		}
	}

	ctrlr->free_io_qids = spdk_bit_array_create(ctrlr->opts.num_io_queues + 1);
	if (ctrlr->free_io_qids == NULL) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	/* Initialize list of free I/O queue IDs. QID 0 is the admin queue (implicitly allocated). */
	for (i = 1; i <= ctrlr->opts.num_io_queues; i++) {
		spdk_nvme_ctrlr_free_qid(ctrlr, i);
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS,
			     ctrlr->opts.admin_timeout_ms);
}

static int
nvme_ctrlr_set_num_queues(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	if (ctrlr->opts.num_io_queues > SPDK_NVME_MAX_IO_QUEUES) {
		NVME_CTRLR_NOTICELOG(ctrlr, "Limiting requested num_io_queues %u to max %d\n",
				     ctrlr->opts.num_io_queues, SPDK_NVME_MAX_IO_QUEUES);
		ctrlr->opts.num_io_queues = SPDK_NVME_MAX_IO_QUEUES;
	} else if (ctrlr->opts.num_io_queues < 1) {
		NVME_CTRLR_NOTICELOG(ctrlr, "Requested num_io_queues 0, increasing to 1\n");
		ctrlr->opts.num_io_queues = 1;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_SET_NUM_QUEUES,
			     ctrlr->opts.admin_timeout_ms);

	rc = nvme_ctrlr_cmd_set_num_queues(ctrlr, ctrlr->opts.num_io_queues,
					   nvme_ctrlr_set_num_queues_done, ctrlr);
	if (rc != 0) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return rc;
	}

	return 0;
}

static void
nvme_ctrlr_set_keep_alive_timeout_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	uint32_t keep_alive_interval_us;
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		if ((cpl->status.sct == SPDK_NVME_SCT_GENERIC) &&
		    (cpl->status.sc == SPDK_NVME_SC_INVALID_FIELD)) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Keep alive timeout Get Feature is not supported\n");
		} else {
			NVME_CTRLR_ERRLOG(ctrlr, "Keep alive timeout Get Feature failed: SC %x SCT %x\n",
					  cpl->status.sc, cpl->status.sct);
			ctrlr->opts.keep_alive_timeout_ms = 0;
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
			return;
		}
	} else {
		if (ctrlr->opts.keep_alive_timeout_ms != cpl->cdw0) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Controller adjusted keep alive timeout to %u ms\n",
					    cpl->cdw0);
		}

		ctrlr->opts.keep_alive_timeout_ms = cpl->cdw0;
	}

	if (ctrlr->opts.keep_alive_timeout_ms == 0) {
		ctrlr->keep_alive_interval_ticks = 0;
	} else {
		keep_alive_interval_us = ctrlr->opts.keep_alive_timeout_ms * 1000 / 2;

		NVME_CTRLR_DEBUGLOG(ctrlr, "Sending keep alive every %u us\n", keep_alive_interval_us);

		ctrlr->keep_alive_interval_ticks = (keep_alive_interval_us * spdk_get_ticks_hz()) /
						   UINT64_C(1000000);

		/* Schedule the first Keep Alive to be sent as soon as possible. */
		ctrlr->next_keep_alive_tick = spdk_get_ticks();
	}

	if (spdk_nvme_ctrlr_is_discovery(ctrlr)) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READY, NVME_TIMEOUT_INFINITE);
	} else {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC,
				     ctrlr->opts.admin_timeout_ms);
	}
}

static int
nvme_ctrlr_set_keep_alive_timeout(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	if (ctrlr->opts.keep_alive_timeout_ms == 0) {
		if (spdk_nvme_ctrlr_is_discovery(ctrlr)) {
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READY, NVME_TIMEOUT_INFINITE);
		} else {
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC,
					     ctrlr->opts.admin_timeout_ms);
		}
		return 0;
	}

	/* Note: Discovery controller identify data does not populate KAS according to spec. */
	if (!spdk_nvme_ctrlr_is_discovery(ctrlr) && ctrlr->cdata.kas == 0) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Controller KAS is 0 - not enabling Keep Alive\n");
		ctrlr->opts.keep_alive_timeout_ms = 0;
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_KEEP_ALIVE_TIMEOUT,
			     ctrlr->opts.admin_timeout_ms);

	/* Retrieve actual keep alive timeout, since the controller may have adjusted it. */
	rc = spdk_nvme_ctrlr_cmd_get_feature(ctrlr, SPDK_NVME_FEAT_KEEP_ALIVE_TIMER, 0, NULL, 0,
					     nvme_ctrlr_set_keep_alive_timeout_done, ctrlr);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Keep alive timeout Get Feature failed: %d\n", rc);
		ctrlr->opts.keep_alive_timeout_ms = 0;
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return rc;
	}

	return 0;
}

static void
nvme_ctrlr_set_host_id_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/*
		 * Treat Set Features - Host ID failure as non-fatal, since the Host ID feature
		 * is optional.
		 */
		NVME_CTRLR_WARNLOG(ctrlr, "Set Features - Host ID failed: SC 0x%x SCT 0x%x\n",
				   cpl->status.sc, cpl->status.sct);
	} else {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Set Features - Host ID was successful\n");
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_TRANSPORT_READY, ctrlr->opts.admin_timeout_ms);
}

static int
nvme_ctrlr_set_host_id(struct spdk_nvme_ctrlr *ctrlr)
{
	uint8_t *host_id;
	uint32_t host_id_size;
	int rc;

	if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		/*
		 * NVMe-oF sends the host ID during Connect and doesn't allow
		 * Set Features - Host Identifier after Connect, so we don't need to do anything here.
		 */
		NVME_CTRLR_DEBUGLOG(ctrlr, "NVMe-oF transport - not sending Set Features - Host ID\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_TRANSPORT_READY, ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	if (ctrlr->cdata.ctratt.bits.host_id_exhid_supported) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Using 128-bit extended host identifier\n");
		host_id = ctrlr->opts.extended_host_id;
		host_id_size = sizeof(ctrlr->opts.extended_host_id);
	} else {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Using 64-bit host identifier\n");
		host_id = ctrlr->opts.host_id;
		host_id_size = sizeof(ctrlr->opts.host_id);
	}

	/* If the user specified an all-zeroes host identifier, don't send the command. */
	if (spdk_mem_all_zero(host_id, host_id_size)) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "User did not specify host ID - not sending Set Features - Host ID\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_TRANSPORT_READY, ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	SPDK_LOGDUMP(nvme, "host_id", host_id, host_id_size);

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_HOST_ID,
			     ctrlr->opts.admin_timeout_ms);

	rc = nvme_ctrlr_cmd_set_host_id(ctrlr, host_id, host_id_size, nvme_ctrlr_set_host_id_done, ctrlr);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Set Features - Host ID failed: %d\n", rc);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * nvme_ctrlr_process_async_event_finish - AER 후처리(ns 갱신 등) 완료 후 사용자 콜백 호출 + 메모리 해제
 *
 * @async_event: 처리 끝난 AER completion 래퍼. 각 process 의 async_events 큐에서 빠져나온 상태.
 *
 * 동기/배경:
 *   process_async_event() 의 종착점. NS_ATTR_CHANGED 라면 ns 재구성, ANA_CHANGE 라면 ANA log
 *   재읽기 등의 SPDK 내부 후처리가 끝난 뒤, 사용자가 spdk_nvme_ctrlr_register_aer_callback() 로
 *   등록해 둔 콜백을 호출하여 응용에도 알림. 마지막에 shared hugepage 로 할당된 wrapper 해제.
 *
 * 실행 컨텍스트: spdk_nvme_ctrlr_process_admin_completions() 의 호출 흐름.
 *               다른 thread 에서 호출되지 않으므로 lock 불필요.
 *
 * 호출 체인:
 *   process_admin_completions → complete_queued_async_events → process_async_event
 *     → [process_async_event_finish] → user aer_cb_fn(aer_cb_arg, cpl) → spdk_free
 */
static void
nvme_ctrlr_process_async_event_finish(struct spdk_nvme_ctrlr_aer_completion *async_event)
{
	struct spdk_nvme_ctrlr_process	*active_proc;
	                                        /* [한국어] 현재 프로세스의 ctrlr_process 객체 — 사용자 aer_cb 가 여기 등록됨. */

	active_proc = nvme_ctrlr_get_current_process(async_event->ctrlr);
	                                        /* [한국어] getpid() 기반으로 현재 프로세스의 process 엔트리 검색. */
	if (active_proc && active_proc->aer_cb_fn) {
		active_proc->aer_cb_fn(active_proc->aer_cb_arg, &async_event->cpl);
		                                /* [한국어] 사용자 콜백 호출 — bdev_nvme 등이 ns hot-add/remove 알림 수신.
		                                 *         CPL 포인터를 넘겨 응용이 async_event_type/info 비트를 직접 해석하게 함. */
	}

	spdk_free(async_event);                 /* [한국어] queue_async_event 에서 spdk_zmalloc(SHARE) 으로 할당된 wrapper 해제.
	                                         *         shared hugepage 풀에 반환되어 다른 process 도 재사용 가능. */
}

/*
 * [한국어]
 * nvme_ctrlr_update_namespaces - NS_ATTR_CHANGED AER 후 변경된 namespace 들을 재 identify
 *
 * @async_event: AER completion wrapper. log_page.changed_ns_list 가 채워져 있으면 그 목록의
 *               NSID 만, NULL 이면 모든 active NS 를 재 identify.
 *
 * 동기/배경:
 *   NVMe 스펙 §5.21.1.4 Namespace Attribute Changed (Notice). NS 가 attach/detach/포맷/리사이즈
 *   되면 컨트롤러가 AER 로 알리고, host 는 변경된 NSID 의 nsdata (NSZE/NCAP/NUSE/LBAF 등) 를
 *   다시 읽어서 ns 객체를 갱신해야 한다. NSID 별로 정확히 무엇이 바뀌었는지는 nvme_ns_construct
 *   에서 Identify NS 를 발행하여 새 nsdata 로 덮어씀으로써 처리.
 *
 * 두 경로:
 *   (A) changed_ns_list==NULL: log page 미지원이거나 overflow 또는 사용자 옵션으로 비활성화된
 *       경우. 모든 active NS 를 일괄 재구성 (보수적이지만 안전).
 *   (B) changed_ns_list!=NULL: log 가 알려준 변경된 NSID 만 재구성. 효율적.
 *
 * 종료 조건 (changed_ns_list 의 종단):
 *   - 0 NSID = 리스트 끝.
 *   - UINT32_MAX (clear_changed_ns_log 에서 미리 차단됨) 는 overflow 의미였다면 (A) 경로로 처리.
 *
 * 실행 컨텍스트: process_admin_completions → process_async_event 흐름.
 *               동일 thread 라 lock 불필요. nvme_ns_construct 가 내부적으로 동기 admin 발행.
 *
 * 호출 체인:
 *   process_async_event → [update_namespaces] → spdk_nvme_ctrlr_get_ns × N → nvme_ns_construct
 *                                            → free(changed_ns_list)
 */
static void
nvme_ctrlr_update_namespaces(struct spdk_nvme_ctrlr_aer_completion *async_event)
{
	struct spdk_nvme_ctrlr *ctrlr = async_event->ctrlr;
	                                        /* [한국어] AER 발생한 ctrlr — wrapper 가 보관해 둔 참조. */
	uint32_t nsid, i;                       /* [한국어] nsid=루프 변수 (1-based NSID), i=리스트 인덱스. */
	struct spdk_nvme_ns *ns;                /* [한국어] 현재 갱신 대상 ns 객체. */

	/* Log page is not used, go over all active namespaces.
	 * Either the log page overflowed or disable_read_changed_ns_list_log_page is used. */
	/* [한국어] 경로 (A): log page 정보가 없으면 보수적으로 모든 active NS 를 재 identify. */
	if (async_event->log_page.changed_ns_list == NULL) {
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
		     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
			                        /* [한국어] active_ns_list bit_array 순회 — 1=active 인 NSID 만 꺼냄. */
			ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
			                        /* [한국어] RB-tree 에서 ns 객체 가져옴 (없으면 lazy alloc). */
			nvme_ns_construct(ns, nsid, ctrlr);
			                        /* [한국어] Identify NS 동기 발행 → nsdata 갱신. NS 가 detach 되었으면
			                         *         내부에서 ns->is_active=false 로 마킹. */
		}

		return;                         /* [한국어] (A) 경로 완료 — changed_ns_list 가 없으니 free 도 불필요. */
	}

	/* Iterate over NSID from the log page. */
	/* [한국어] 경로 (B): log page 가 알려준 변경 NSID 들만 처리. SPDK_NVME_MAX_CHANGED_NAMESPACES 만큼만
	 *         로그 페이지 크기가 잡혀 있으므로 그 한도까지만 순회. */
	for (i = 0; i < SPDK_NVME_MAX_CHANGED_NAMESPACES; i++) {
		nsid = async_event->log_page.changed_ns_list[i];
		                                /* [한국어] 4-byte NSID 하나씩 읽기. 컨트롤러가 channged 한 NSID 들을
		                                 *         오름차순으로 채워준다 (스펙 §5.16.1.5 Changed NS List Log Page). */

		/* End of the list */
		if (nsid == 0) {
			break;
			                        /* [한국어] 0=종단 마커. NSID 0 은 NVMe 에서 invalid 로 정의되므로
			                         *         "리스트 끝" 의미로 사용된다. */
		}

		/* Log page contains NSID for namespaces that were marked
		 * as inactive, no need to identify them. */
		/* [한국어] 비활성 NSID 는 log 에 포함될 수 있지만 (detach 직후) Identify NS 가 무의미.
		 *         active_ns_list 비트맵에 없는 NSID 는 skip. */
		if (!spdk_nvme_ctrlr_is_active_ns(ctrlr, nsid)) {
			continue;
		}

		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		                                /* [한국어] active 인 ns 객체 가져옴. */
		nvme_ns_construct(ns, nsid, ctrlr);
		                                /* [한국어] Identify NS 발행 → nsdata 갱신. */
	}

	free(async_event->log_page.changed_ns_list);
	                                        /* [한국어] clear_changed_ns_log 에서 calloc 으로 할당된 버퍼 해제.
	                                         *         spdk_free 가 아닌 일반 free — DMA 버퍼가 아니라 파싱 결과 보관용. */
}

/*
 * [한국어]
 * nvme_ctrlr_clear_changed_ns_log - Changed Namespace List log page 를 read-and-clear
 *
 * @async_event: AER wrapper. 성공 시 log_page.changed_ns_list 에 결과 버퍼 포인터 저장.
 * @return: 0 = 성공 또는 사용자가 옵션으로 비활성화함 (둘 다 OK)
 *          -ENOMEM = 버퍼 할당 실패
 *          음수 = get_log_page 실패 (transport/admin 오류)
 *
 * 동기/배경:
 *   NVMe 스펙 §5.16.1.5 Changed Namespace List (LID=0x04). 이 log page 는 마지막으로 읽힌
 *   이후 변경된 NSID 들의 리스트를 반환하며, **읽는 동작 자체가 컨트롤러 측 큐를 비우는
 *   read-and-clear** 동작이다. 따라서 한 번 읽어두지 않으면 다음 AER 가 발생하지 않는다
 *   (스펙 §5.21.1.4 NS_ATTR_CHANGED 는 log page 가 비어있어야 다시 발생).
 *
 *   리스트가 4096 바이트 (1024 NSID) 를 초과하면 첫 entry 가 0xFFFFFFFF (UINT32_MAX) 로
 *   set 되어 overflow 신호를 보내며, 이 경우 host 는 모든 NS 를 재스캔해야 한다.
 *
 * 동작 단계:
 *   1) opts.disable_read_changed_ns_list_log_page 옵션이 set 이면 noop 으로 0 반환.
 *      (사용자가 별도 경로로 ns 변경 추적 시 사용)
 *   2) calloc 으로 4096B (1024 entries × 4B) 버퍼 할당.
 *   3) Get Log Page 동기 발행 — LID=CHANGED_NS_LIST, NSID=GLOBAL (0xFFFFFFFF).
 *   4) 첫 entry 가 UINT32_MAX 이면 overflow → out 분기로 버퍼 해제 + 음수 반환 (caller=update_namespaces 가
 *      log_page.changed_ns_list==NULL 경로로 모든 NS 재스캔).
 *   5) 정상 시 wrapper 에 버퍼 포인터 저장 후 0 반환 — 호출자가 free 책임.
 *
 * 실행 컨텍스트: process_async_event 흐름. 동기 admin 발행 (poll 으로 완료 대기).
 *
 * 호출 체인:
 *   process_async_event (NS_ATTR_CHANGED) → [clear_changed_ns_log]
 *     → spdk_nvme_ctrlr_cmd_get_log_page → nvme_wait_for_adminq_completion
 *     → async_event->log_page.changed_ns_list = changed_ns_list (성공 경로)
 *   상위에서 update_namespaces 가 이 리스트를 사용하여 NSID 별 재 identify.
 */
static int
nvme_ctrlr_clear_changed_ns_log(struct spdk_nvme_ctrlr_aer_completion *async_event)
{
	struct spdk_nvme_ctrlr			*ctrlr = async_event->ctrlr;
	                                        /* [한국어] AER wrapper 에 보관된 ctrlr 참조. */
	struct nvme_completion_poll_status	*status;
	                                        /* [한국어] 동기 polling 용 status 객체 — done/cpl 보관. */
	int		rc = -ENOMEM;           /* [한국어] 기본값 ENOMEM — 첫 calloc 실패 시 그대로 반환. */
	uint32_t	*changed_ns_list;       /* [한국어] log page 결과 버퍼 — 1024개 NSID 배열. */
	size_t		changed_ns_list_length = SPDK_NVME_MAX_CHANGED_NAMESPACES * sizeof(uint32_t);
	                                        /* [한국어] 버퍼 크기 = 1024 × 4 = 4096B (NVMe spec 정의). */

	if (ctrlr->opts.disable_read_changed_ns_list_log_page) {
		return 0;
		                                /* [한국어] 사용자 옵션으로 비활성화 — log 안 읽음. AER 재트리거가
		                                 *         안 되는 부작용 있지만 일부 버그 있는 컨트롤러 우회용. */
	}

	changed_ns_list = calloc(1, changed_ns_list_length);
	                                        /* [한국어] zero-initialized 4KiB 버퍼 — DMA 가 아닌 일반 메모리.
	                                         *         get_log_page 는 transport 가 zero-copy 또는 bounce buffer 처리. */
	if (!changed_ns_list) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate buffer for getting changed ns log.\n");
		goto out;                       /* [한국어] rc 는 ENOMEM 그대로. */
	}

	status = calloc(1, sizeof(*status));
	                                        /* [한국어] poll status 객체 — done flag + cpl 사본 보관용. */
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		goto out;                       /* [한국어] changed_ns_list 는 out 분기에서 free. */
	}

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr,
					      SPDK_NVME_LOG_CHANGED_NS_LIST,
					      /* [한국어] LID=0x04 = Changed NS List (read-and-clear semantics). */
					      SPDK_NVME_GLOBAL_NS_TAG,
					      /* [한국어] NSID=0xFFFFFFFF = global — 모든 NS 의 변경 사항을 모음. */
					      changed_ns_list, changed_ns_list_length, 0,
					      /* [한국어] payload 버퍼 + 길이 + offset(0=처음부터). */
					      nvme_completion_poll_cb, status);
	                                        /* [한국어] 완료 콜백 + status. nvme_wait_for_adminq_completion 이
	                                         *         status->done 을 polling 하여 동기 대기. */
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_cmd_get_log_page() failed: rc=%d\n", rc);
		free(status);                   /* [한국어] 발행 자체가 실패 — caller 가 콜백을 호출하지 않을 것이므로
		                                 *         status 를 본 함수에서 직접 free. */
		goto out;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	                                        /* [한국어] admin completion polling — third arg true = 완료 후 status free.
	                                         *         성공 시 changed_ns_list 가 컨트롤러 응답으로 채워져 있음. */
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for spdk_nvme_ctrlr_cmd_get_log_page failed: rc=%s\n",
				  spdk_strerror(abs(rc)));
		goto out;                       /* [한국어] 컨트롤러 측 오류 — 버퍼 해제 후 반환. */
	}

	/* only check the case of overflow. */
	/* [한국어] overflow 검사 — 첫 entry==UINT32_MAX 이면 변경된 NS 가 1024개 초과여서 리스트가 잘림.
	 *         이 경우 caller(update_namespaces) 가 log==NULL 경로로 모든 NS 재스캔하도록 본 함수는
	 *         실패로 처리하여 wrapper 의 changed_ns_list 는 NULL 로 유지. */
	if (changed_ns_list[0] == UINT32_MAX) {
		NVME_CTRLR_WARNLOG(ctrlr, "changed ns log overflowed.\n");
		goto out;                       /* [한국어] rc 는 그대로 (마지막 wait 가 0 반환했으므로 0).
		                                 *         caller 는 list==NULL 로 보고 (A) 경로 진입. */
	}

	async_event->log_page.changed_ns_list = changed_ns_list;
	                                        /* [한국어] 성공 — wrapper 에 버퍼 포인터 저장. update_namespaces 가
	                                         *         이걸 사용하여 NSID 별 재 identify 후 free. */
	return 0;

out:
	free(changed_ns_list);                  /* [한국어] 실패 경로 일괄 cleanup. NULL free 는 안전 (libc 보장). */
	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_process_async_event - 큐에 쌓인 AER completion 한 건을 디코드하여 SPDK 내부 후처리 수행
 *
 * @async_event: complete_queued_async_events 가 STAILQ 에서 꺼낸 wrapper. cpl 안에 AER 결과 보관.
 *
 * 동기/배경:
 *   NVMe 스펙 §5.21 Asynchronous Event Request — 컨트롤러가 host 에 비동기 알림(SMART critical,
 *   NS 변경, ANA 변경, firmware activation, telemetry 등)을 보내는 메커니즘. AER completion 의
 *   CDW0 에 (async_event_type, async_event_info, log_page_id) 가 들어있고, host 는 그에 맞는
 *   log page 를 읽어 상세 정보를 얻는다.
 *
 *   본 함수는 SPDK 가 자체적으로 처리해야 하는 두 종류 이벤트 (NS_ATTR_CHANGED, ANA_CHANGE) 를
 *   디코드하여 ns 객체 / ANA 상태를 갱신한다. 그 외 이벤트는 사용자 콜백(aer_cb_fn) 에 전달.
 *
 * 처리하는 이벤트 타입:
 *   1) NOTICE / NS_ATTR_CHANGED:
 *      · Changed NS List log read-and-clear → 변경된 NSID 리스트 획득.
 *      · identify_active_ns 로 active NS 비트맵 재로드.
 *      · 변경된 NSID 들 nvme_ns_construct 로 nsdata 갱신.
 *      · nvme_io_msg_ctrlr_update 로 외부(bdev_nvme 등) 에 reset 신호.
 *   2) NOTICE / ANA_CHANGE (NVMe-oF multipath):
 *      · 사용자 옵션이 disable 이면 skip.
 *      · ANA log page 재읽기 → 각 NS 의 ANA state(optimized/non-optimized/inaccessible 등) 갱신.
 *
 * 그 외 이벤트 (SMART, error, telemetry 등):
 *   · 본 함수에서 별도 처리 없이 finish 로 진행 → 사용자 콜백에서 응용이 직접 처리.
 *
 * 실행 컨텍스트: process_admin_completions → complete_queued_async_events.
 *               동기 admin 발행을 포함 (clear_changed_ns_log, identify_active_ns, update_ana_log_page).
 *
 * 호출 체인:
 *   complete_queued_async_events → [process_async_event]
 *     → (NS_ATTR_CHANGED) clear_changed_ns_log + identify_active_ns + update_namespaces + io_msg_update
 *     → (ANA_CHANGE) update_ana_log_page + parse_ana_log_page
 *     → process_async_event_finish (사용자 콜백 + free)
 *
 * 에러 경로:
 *   · identify_active_ns 또는 update_ana_log_page 실패 시 finish 호출 없이 return —
 *     wrapper 가 leak 될 가능성 있음 (현재 SPDK 동작 그대로 보존, 코드 수정 금지).
 */
static void
nvme_ctrlr_process_async_event(struct spdk_nvme_ctrlr_aer_completion *async_event)
{
	struct spdk_nvme_ctrlr *ctrlr = async_event->ctrlr;
	                                        /* [한국어] AER 발생한 ctrlr — wrapper 가 보관해 둔 참조. */
	struct spdk_nvme_cpl *cpl = &async_event->cpl;
	                                        /* [한국어] AER completion 사본. CDW0 에 event_type/info/log_page_id 인코딩. */
	union spdk_nvme_async_event_completion event;
	                                        /* [한국어] CDW0 비트필드 디코드용 union. event.bits.* 로 접근. */
	int rc;

	event.raw = cpl->cdw0;
	                                        /* [한국어] CDW0 raw 32bit 를 union 에 적재 — 이후 비트필드 read.
	                                         *         layout: [7:0]=event_type, [15:8]=event_info, [23:16]=log_page_id. */

	/* [한국어] 분기 1: NS_ATTR_CHANGED — Namespace 의 attach/detach/format/resize 가 발생.
	 *         async_event_type=NOTICE(0x2), async_event_info=NS_ATTR_CHANGED(0x0). */
	if ((event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) &&
	    (event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED)) {
		nvme_ctrlr_clear_changed_ns_log(async_event);
		                                /* [한국어] Changed NS List log read-and-clear. 성공 시 wrapper 에
		                                 *         changed_ns_list 버퍼 attach. 실패해도 진행 — update_namespaces 가
		                                 *         (A) 경로(전체 재스캔) 로 fallback. */

		rc = nvme_ctrlr_identify_active_ns(ctrlr);
		                                /* [한국어] active NS 비트맵 재로드 — Identify Active NS List (CNS=0x02)
		                                 *         발행. 새로 attach 된 NSID 도 비트맵에 반영. */
		if (rc) {
			return;                 /* [한국어] active_ns 식별 실패 — wrapper free 안 됨 (의도적; 재시도 여지). */
		}
		nvme_ctrlr_update_namespaces(async_event);
		                                /* [한국어] 변경된 NS 들의 nsdata 갱신 (Identify NS 발행 × N). */
		nvme_io_msg_ctrlr_update(ctrlr);
		                                /* [한국어] 외부 모듈(bdev_nvme, NVMe-oF target 등) 에 ctrlr 갱신 통지 —
		                                 *         별도 thread 에서 ns 재스캔 트리거. */
	}

	/* [한국어] 분기 2: ANA_CHANGE — Asymmetric Namespace Access 상태 변경 (NVMe-oF multipath).
	 *         primary path 가 inaccessible 로 바뀌면 host 가 다른 path 로 IO 라우팅. */
	if ((event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) &&
	    (event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_ANA_CHANGE)) {
		if (!ctrlr->opts.disable_read_ana_log_page) {
			                        /* [한국어] 사용자가 ANA log 읽기를 비활성화하지 않은 경우만 갱신. */
			rc = nvme_ctrlr_update_ana_log_page(ctrlr);
			                        /* [한국어] ANA log page (LID=0x0C) 동기 read — 각 ANAGRPID 별 state 정보. */
			if (rc) {
				return;         /* [한국어] log read 실패 — finish 호출 없이 종료 (재시도 여지). */
			}
			nvme_ctrlr_parse_ana_log_page(ctrlr, nvme_ctrlr_update_ns_ana_states,
						      ctrlr);
			                        /* [한국어] 파싱 후 각 NS 의 ana_state 필드 갱신 (콜백 패턴). */
		}
	}

	nvme_ctrlr_process_async_event_finish(async_event);
	                                        /* [한국어] 사용자 콜백 호출 + wrapper free.
	                                         *         그 외 이벤트(SMART, error, telemetry 등) 는 위 분기를 모두 거치지 않고
	                                         *         바로 여기로 와서 사용자 콜백에 전달. */
}

/*
 * [한국어]
 * nvme_ctrlr_queue_async_event - AER completion 을 모든 attached process 의 큐에 fan-out 복제
 *
 * @ctrlr: AER 가 발생한 controller.
 * @cpl: AER completion. CDW0 에 event 정보, status 에 SC/SCT.
 *
 * 동기/배경:
 *   SPDK 는 multi-process 모델을 지원 — 한 NVMe controller 를 primary + secondary 여러 프로세스가
 *   공유 가능하다. AER 는 컨트롤러가 host 에 보내는 단일 알림이지만, 각 프로세스가 자신의 사용자
 *   콜백(aer_cb_fn) 을 가지므로 모든 프로세스가 알림을 받아야 한다. 따라서 본 함수는 cpl 을
 *   shared hugepage 에 복제하여 active_procs 리스트의 모든 process 큐에 push 한다.
 *
 *   각 process 는 자기 thread 에서 process_admin_completions() 호출 시
 *   complete_queued_async_events() 로 자기 큐의 wrapper 들을 처리한다. SPDK_MALLOC_SHARE 로
 *   할당된 shared 메모리이므로 secondary process 도 같은 가상 주소로 접근 가능.
 *
 * 동작 단계:
 *   1) active_procs 순회 (ctrlr_lock 보유 상태로 호출되어야 안전).
 *   2) 각 proc 마다 spdk_zmalloc(SHARE) 으로 wrapper 새로 할당.
 *   3) ctrlr 와 cpl 복사.
 *   4) STAILQ_INSERT_TAIL — proc 별 FIFO 순서로 처리.
 *
 * 실행 컨텍스트: nvme_ctrlr_async_event_cb (admin CQ 폴링 흐름) 에서 호출.
 *               호출자가 lock 보유 상태.
 *
 * 호출 체인:
 *   admin completion polling → nvme_ctrlr_async_event_cb → [queue_async_event]
 *     → 각 proc 의 STAILQ insert
 *   각 process 측: process_admin_completions → complete_queued_async_events → process_async_event
 *
 * 에러 경로:
 *   · spdk_zmalloc 실패 시 ERRLOG 후 return — 일부 proc 에 이미 들어간 wrapper 는 그대로 두며
 *     이후 제거됨. AER 1건 분실 (재시도 안 함).
 */
static void
nvme_ctrlr_queue_async_event(struct spdk_nvme_ctrlr *ctrlr,
			     const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr_aer_completion *async_event;
	                                        /* [한국어] proc 별 wrapper. shared hugepage 에 할당. */
	struct spdk_nvme_ctrlr_process *proc;
	                                        /* [한국어] active_procs 순회 커서. */

	/* Add async event to each process objects event list */
	TAILQ_FOREACH(proc, &ctrlr->active_procs, tailq) {
		                                /* [한국어] ctrlr 를 attach 한 모든 process 순회 — primary + secondaries. */
		/* Must be shared memory so other processes can access */
		async_event = spdk_zmalloc(sizeof(*async_event), 0, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
		                                /* [한국어] zero-initialized + shared hugepage. SHARE 플래그로 secondary 가
		                                 *         같은 가상 주소로 접근 가능. align 0=기본, NUMA ANY=어디든 OK. */
		if (!async_event) {
			NVME_CTRLR_ERRLOG(ctrlr, "Alloc nvme event failed, ignore the event\n");
			return;                 /* [한국어] hugepage 부족 — 이번 AER 는 일부 proc 에만 전달되거나 분실.
			                         *         이미 큐에 넣은 wrapper 는 정상 처리됨. */
		}
		async_event->ctrlr = ctrlr;     /* [한국어] 후속 처리 시 ctrlr 역참조용. */
		async_event->cpl = *cpl;        /* [한국어] cpl 구조체 전체 복사 — wrapper 가 자체 사본 보관. */

		STAILQ_INSERT_TAIL(&proc->async_events, async_event, link);
		                                /* [한국어] FIFO tail insert — 각 proc 가 자기 thread 에서 head 부터
		                                 *         순차 처리. lock 은 caller 가 보유 (ctrlr_lock). */
	}
}

/*
 * [한국어]
 * nvme_ctrlr_complete_queued_async_events - 현재 process 의 AER 큐에 쌓인 이벤트들을 일괄 처리
 *
 * @ctrlr: 처리 대상 controller.
 *
 * 동기/배경:
 *   queue_async_event 가 fan-out 으로 모든 process 큐에 wrapper 를 넣어두면, 각 process 는
 *   자기 process_admin_completions() 호출 시 본 함수를 통해 자기 큐를 비운다. STAILQ 안전 순회
 *   매크로로 처리 도중 wrapper 가 free 되어도 안전하게 다음 항목으로 진행.
 *
 *   per-process 큐 분리의 의미:
 *     · 사용자 콜백(aer_cb_fn) 은 process 별로 다를 수 있음.
 *     · primary 가 늦게 polling 해도 secondary 는 자기 큐만 보면 되므로 latency 격리.
 *
 * 실행 컨텍스트: process_admin_completions 흐름. 현재 process 의 자기 thread.
 *               ctrlr_lock 은 caller(process_admin_completions) 가 보유.
 *
 * 호출 체인:
 *   process_admin_completions → [complete_queued_async_events]
 *     → process_async_event × N (큐의 모든 wrapper 처리)
 */
static void
nvme_ctrlr_complete_queued_async_events(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_aer_completion *async_event, *async_event_tmp;
	                                        /* [한국어] async_event=현재 처리 중, _tmp=다음 노드 안전 보존. */
	struct spdk_nvme_ctrlr_process *active_proc;
	                                        /* [한국어] 현재 process 의 ctrlr_process 객체. */

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	                                        /* [한국어] getpid() 로 현재 process 의 entry 검색. */

	STAILQ_FOREACH_SAFE(async_event, &active_proc->async_events, link, async_event_tmp) {
		                                /* [한국어] 안전 순회 — link 필드를 따라가되 _tmp 에 다음 포인터 미리 저장.
		                                 *         process_async_event 가 wrapper 를 free 해도 _tmp 는 유효. */
		STAILQ_REMOVE(&active_proc->async_events, async_event,
			      spdk_nvme_ctrlr_aer_completion, link);
		                                /* [한국어] 큐에서 wrapper 분리 — 이후 process_async_event 는 큐와 무관하게
		                                 *         이 wrapper 를 처리하고 free. STAILQ_REMOVE 는 O(n) 검색이지만
		                                 *         FOREACH_SAFE 와 함께 쓰면 흔히 사용되는 안전 패턴. */
		nvme_ctrlr_process_async_event(async_event);
		                                /* [한국어] AER 디코드 + 후처리 + 사용자 콜백 + free. */
	}
}

/*
 * [한국어]
 * nvme_ctrlr_async_event_cb - AER 한 건 완료 시 호출되는 admin completion 콜백 (★ AER 자동 재발행 패턴)
 *
 * @arg: nvme_async_event_request 포인터 (ctrlr->aer[i] 슬롯 중 하나).
 * @cpl: AER completion. CDW0 에 event_type/info/log_page_id, status 에 SC/SCT.
 *
 * 동기/배경:
 *   ★ NVMe AER 의 핵심 패턴 ★
 *   AER 는 컨트롤러가 host 에 알림을 보내는 통신로이다. host 가 AER 명령을 admin queue 에 발행하고
 *   "보류" 상태로 두면, 컨트롤러는 알릴 이벤트가 생길 때 그 명령을 완료시켜 알림을 전달한다.
 *   따라서 host 는 항상 N개의 AER 를 발행해 두어야 하며, 한 건이 완료될 때마다 즉시 다른 AER 를
 *   "재발행" 하여 슬롯을 보충해야 끊김 없는 통지가 가능하다.
 *
 *   본 콜백이 그 재발행을 담당:
 *     1) cpl 디코드 → queue_async_event 로 모든 proc 큐에 fan-out.
 *     2) shutdown/제거 상황이 아니면 즉시 같은 aer 슬롯에 새 AER 를 발행.
 *
 * 처리하는 특수 status:
 *   (A) GENERIC / ABORTED_SQ_DELETION: shutdown 시뮬레이션. 컨트롤러가 admin SQ 를 삭제하면서
 *       모든 outstanding AER 를 abort. 메모리 누수 방지를 위해 SPDK 가 인위적으로 만들기도 함.
 *       → 재발행 금지 (ctrlr 가 종료 중). 큐에도 넣지 않음.
 *   (B) COMMAND_SPECIFIC / AER_LIMIT_EXCEEDED: 컨트롤러가 AERL 보다 더 많이 받았다고 거부.
 *       SPDK 는 cdata.aerl+1 만큼만 보내므로 이 코드는 spec 위반 컨트롤러 신호.
 *       → 재발행 금지 (무한 루프 방지). 큐에도 넣지 않음.
 *   (C) 그 외 (정상 또는 다른 에러): event 큐에 push + 같은 aer 슬롯 재발행.
 *
 * 실행 컨텍스트: spdk_nvme_qpair_process_completions(ctrlr->adminq) 흐름. 사용자 thread (admin polling).
 *               ctrlr_lock 보유 상태 (process_admin_completions 가 잡은 lock).
 *
 * 호출 체인:
 *   admin CQ polling → qpair completion → [async_event_cb]
 *     → queue_async_event (fan-out to all procs)
 *     → construct_and_submit_aer (재발행)
 *
 * 에러 경로:
 *   · 재발행 실패 시 ERRLOG 만 — AER 슬롯 1개 영구 손실. 다음 reset 까지는 그 슬롯 비활성.
 */
static void
nvme_ctrlr_async_event_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_async_event_request	*aer = arg;
	                                        /* [한국어] 완료된 AER 슬롯 — ctrlr->aer[] 배열의 한 entry.
	                                         *         재발행 시 같은 슬롯을 재사용 (req 만 새로 할당). */
	struct spdk_nvme_ctrlr		*ctrlr = aer->ctrlr;
	                                        /* [한국어] AER 가 속한 ctrlr — construct_and_submit_aer 가 채워둠. */

	/* [한국어] 케이스 (A): shutdown 시 SQ deletion 으로 인한 abort.
	 *         GENERIC=0x0, ABORTED_SQ_DELETION=0x08 (NVMe 스펙 §4.6.1.2.1). */
	if (cpl->status.sct == SPDK_NVME_SCT_GENERIC &&
	    cpl->status.sc == SPDK_NVME_SC_ABORTED_SQ_DELETION) {
		/*
		 *  This is simulated when controller is being shut down, to
		 *  effectively abort outstanding asynchronous event requests
		 *  and make sure all memory is freed.  Do not repost the
		 *  request in this case.
		 */
		return;                         /* [한국어] 재발행 금지 — ctrlr 가 종료 중이므로 새 AER 가 의미 없음.
		                                 *         큐에도 안 넣음 (가짜 이벤트). */
	}

	/* [한국어] 케이스 (B): AER 한도 초과. SPDK 는 한도(aerl+1) 내에서만 발행하므로
	 *         이 코드는 컨트롤러가 잘못 보고하는 것 — out-of-spec.
	 *         COMMAND_SPECIFIC=0x1, AER_LIMIT_EXCEEDED=0x05. */
	if (cpl->status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC &&
	    cpl->status.sc == SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED) {
		/*
		 *  SPDK will only send as many AERs as the device says it supports,
		 *  so this status code indicates an out-of-spec device.  Do not repost
		 *  the request in this case.
		 */
		NVME_CTRLR_ERRLOG(ctrlr, "Controller appears out-of-spec for asynchronous event request\n"
				  "handling.  Do not repost this AER.\n");
		return;                         /* [한국어] 재발행 금지 — 무한 루프 (즉시 거부 → 즉시 재발행 → ...) 방지. */
	}

	/* Add the events to the list */
	nvme_ctrlr_queue_async_event(ctrlr, cpl);
	                                        /* [한국어] 정상 이벤트 — 모든 active_procs 큐에 fan-out 복제. */

	/* If the ctrlr was removed or in the destruct state, we should not send aer again */
	/* [한국어] 재발행 직전 ctrlr 상태 점검 — hot-remove 또는 destruct 진행 중이면
	 *         새 AER 발행이 admin SQ 에 push 되었다가 즉시 abort 될 뿐이므로 skip. */
	if (ctrlr->is_removed || ctrlr->is_destructed) {
		return;
	}

	/*
	 * Repost another asynchronous event request to replace the one
	 *  that just completed.
	 */
	/* [한국어] ★ 재발행 — 같은 aer 슬롯에 새 nvme_request 와 함께 새 AER 명령을 admin SQ 에 push.
	 *         이로써 N개 AER 슬롯이 항상 outstanding 상태로 유지되어 다음 이벤트를 즉시 받을 준비. */
	if (nvme_ctrlr_construct_and_submit_aer(ctrlr, aer)) {
		/*
		 * We can't do anything to recover from a failure here,
		 * so just print a warning message and leave the AER unsubmitted.
		 */
		NVME_CTRLR_ERRLOG(ctrlr, "resubmitting AER failed!\n");
		                                /* [한국어] 발행 실패 — 슬롯 영구 손실. 복구 경로 없음 (코드 그대로).
		                                 *         다음 reset 시 configure_aer 가 다시 N개 발행하면서 회복됨. */
	}
}

/*
 * [한국어]
 * nvme_ctrlr_construct_and_submit_aer - admin queue 에 AER 한 건 발행 (재발행/초기 발행 공통)
 *
 * @ctrlr: 발행 대상 controller.
 * @aer: 사용할 aer 슬롯 (ctrlr->aer[i]). req 포인터가 새 nvme_request 로 갱신됨.
 * @return: 0 = submit 성공
 *          -1 = 요청 객체 할당 실패
 *          기타 = transport submit 실패
 *
 * 동기/배경:
 *   AER 명령(opcode 0x0C) 을 발행한다. 페이로드 없음 (null buffer), 컨트롤러는 이 명령을 보류
 *   상태로 들고 있다가 알릴 이벤트가 생기면 그 명령을 완료시킴으로써 알림을 전달한다.
 *
 *   호출 시점:
 *     1) configure_aer_done — 초기에 N개 AER 발행 (controller bring-up 마지막 단계).
 *     2) async_event_cb — 한 건 완료 후 자동 재발행.
 *
 * 동작 단계:
 *   1) aer->ctrlr 채움 (콜백에서 ctrlr 역참조용).
 *   2) nvme_allocate_request_null — payload 없는 nvme_request 객체 할당. 콜백/cb_arg=aer.
 *   3) opcode = ASYNC_EVENT_REQUEST (0x0C).
 *   4) admin SQ 에 submit — transport 가 SQE 를 doorbell 까지 처리.
 *
 * 실행 컨텍스트: configure_aer_done 또는 async_event_cb 흐름. 사용자 thread.
 *               ctrlr_lock 보유 상태 (caller invariant).
 *
 * 호출 체인:
 *   configure_aer_done / async_event_cb → [construct_and_submit_aer]
 *     → nvme_allocate_request_null → nvme_ctrlr_submit_admin_request → admin SQ doorbell
 */
static int
nvme_ctrlr_construct_and_submit_aer(struct spdk_nvme_ctrlr *ctrlr,
				    struct nvme_async_event_request *aer)
{
	struct nvme_request *req;               /* [한국어] admin SQ 에 push 할 요청 객체. */

	aer->ctrlr = ctrlr;                     /* [한국어] 콜백에서 ctrlr 역참조용 — 슬롯 재사용 시도 매번 set. */
	req = nvme_allocate_request_null(ctrlr->adminq, nvme_ctrlr_async_event_cb, aer);
	                                        /* [한국어] payload 없는 admin request 할당. cb_arg=aer 슬롯이므로
	                                         *         완료 콜백에서 같은 슬롯에 재발행 가능. */
	aer->req = req;                         /* [한국어] 슬롯에 req 포인터 보관 — debug/cleanup 용. */
	if (req == NULL) {
		return -1;                      /* [한국어] mempool 고갈 — 재발행 실패. caller 가 ERRLOG. */
	}

	req->cmd.opc = SPDK_NVME_OPC_ASYNC_EVENT_REQUEST;
	                                        /* [한국어] NVMe admin opcode 0x0C — Asynchronous Event Request.
	                                         *         CDW1-15 모두 0 (사용 안 함). 컨트롤러가 이벤트 발생 시 cdw0 채워서 완료. */
	return nvme_ctrlr_submit_admin_request(ctrlr, req);
	                                        /* [한국어] admin SQ 에 SQE push + doorbell write. transport 별 핸들러 호출. */
}

/*
 * [한국어]
 * nvme_ctrlr_configure_aer_done - Set Features (AER Config) 완료 콜백 → 초기 AER N개 발행 + 다음 state
 *
 * @arg: ctrlr 포인터 (configure_aer 가 cb_arg 로 전달).
 * @cpl: Set Features completion. 성공/실패 여부 판단용.
 *
 * 동기/배경:
 *   process_init state machine 의 한 단계. configure_aer 가 발행한 Set Features (FID=0x0B,
 *   Async Event Configuration) 가 완료되면 본 콜백이 호출된다. 여기서 슬롯 N개에 대해 AER
 *   초기 발행을 일괄 수행하고, 다음 state(SET_KEEP_ALIVE_TIMEOUT) 로 전이한다.
 *
 *   슬롯 개수 결정:
 *     · Identify Controller cdata.aerl (Async Event Request Limit) 가 컨트롤러 지원 한도 — 0-based
 *       이므로 +1 필요.
 *     · NVME_MAX_ASYNC_EVENTS (SPDK 컴파일 시 상수) 가 host 측 한도.
 *     · min(NVME_MAX_ASYNC_EVENTS, aerl+1) 만큼 발행.
 *
 *   Set Features 자체가 실패해도 num_aers=0 으로 두고 진행 — AER 비활성화로 운용 가능.
 *
 * 실행 컨텍스트: process_admin_completions 흐름의 admin completion 콜백.
 *               ctrlr_lock 보유 상태 (caller invariant).
 *
 * 호출 체인:
 *   configure_aer → Set Features command → admin completion → [configure_aer_done]
 *     → construct_and_submit_aer × num_aers
 *     → set_state(SET_KEEP_ALIVE_TIMEOUT)
 *
 * 에러 경로:
 *   · cpl error: num_aers=0, 발행 루프 skip, 다음 state 로 진행 (AER 없는 모드).
 *   · construct_and_submit_aer 실패 (mempool 고갈): state=ERROR 로 전이, 즉시 return.
 */
static void
nvme_ctrlr_configure_aer_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_async_event_request		*aer;
	                                        /* [한국어] 발행 루프에서 사용할 슬롯 포인터 — ctrlr->aer[i]. */
	int					rc;
	uint32_t				i;
	struct spdk_nvme_ctrlr *ctrlr =	(struct spdk_nvme_ctrlr *)arg;
	                                        /* [한국어] cb_arg 로 전달된 ctrlr 캐스트. */

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_NOTICELOG(ctrlr, "nvme_ctrlr_configure_aer failed!\n");
		ctrlr->num_aers = 0;
		                                /* [한국어] Set Features 실패 — AER 발행 없이 진행. 일부 컨트롤러는
		                                 *         이 feature 를 미지원 → 그래도 ctrlr 는 정상 동작 가능. */
	} else {
		/* aerl is a zero-based value, so we need to add 1 here. */
		ctrlr->num_aers = spdk_min(NVME_MAX_ASYNC_EVENTS, (ctrlr->cdata.aerl + 1));
		                                /* [한국어] 슬롯 개수 = min(host 한도, 컨트롤러 한도+1).
		                                 *         · cdata.aerl: Identify Controller 의 AERL — 0-based 이므로 +1.
		                                 *         · NVME_MAX_ASYNC_EVENTS: SPDK 빌드 시 상수 (보통 8).
		                                 *         · ctrlr->aer[NVME_MAX_ASYNC_EVENTS] 배열 크기 안에서 사용. */
	}

	for (i = 0; i < ctrlr->num_aers; i++) {
		aer = &ctrlr->aer[i];           /* [한국어] i번째 슬롯. */
		rc = nvme_ctrlr_construct_and_submit_aer(ctrlr, aer);
		                                /* [한국어] 각 슬롯에 AER 명령 1건씩 발행 — N개 outstanding 으로 유지.
		                                 *         완료 콜백은 모두 nvme_ctrlr_async_event_cb 로 동일. */
		if (rc) {
			NVME_CTRLR_ERRLOG(ctrlr, "nvme_ctrlr_construct_and_submit_aer failed!\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
			                        /* [한국어] 한 슬롯이라도 발행 실패 = mempool/transport 문제 — ERROR 로 전이.
			                         *         caller(process_init) 가 reset 또는 fail 처리. */
			return;
		}
	}
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT, ctrlr->opts.admin_timeout_ms);
	                                        /* [한국어] AER 발행 완료 — 다음 state 로 전이. timeout=admin_timeout_ms
	                                         *         (다음 단계 admin command 응답 대기 한도). */
}

/*
 * [한국어]
 * nvme_ctrlr_configure_aer - process_init 단계: Set Features (AER Config) 발행 진입점
 *
 * @ctrlr: bring-up 중인 controller. process_init state machine 이 호출.
 * @return: 0 = Set Features 발행 성공 (콜백 대기 상태)
 *          음수 = 발행 실패 (transport/mempool 오류). state=ERROR 마킹됨.
 *
 * 동기/배경:
 *   NVMe 스펙 §5.27.1.8 Async Event Configuration (FID=0x0B). host 가 어떤 알림을 받을지
 *   비트마스크로 지정. 미지정 비트의 이벤트는 컨트롤러가 alert 안 함.
 *
 * 설정하는 알림 종류:
 *   Discovery controller (NVMe-oF):
 *     · discovery_log_change_notice: discovery log 변경 알림.
 *   IO controller:
 *     · crit_warn.* (5비트): SMART critical warning (스페어, 온도, 신뢰성, RO, 휘발성 메모리 백업).
 *     · NVMe 1.2+: ns_attr_notice, fw_activation_notice, ana_change_notice (oaes 비트로 지원 확인).
 *     · NVMe 1.3+: telemetry_log_notice (lpa.ts 비트로 지원 확인).
 *
 * 동작 단계:
 *   1) config 비트 설정 (위 정책).
 *   2) state=WAIT_FOR_CONFIGURE_AER, timeout=admin_timeout_ms.
 *   3) Set Features 발행 — 완료 시 configure_aer_done 콜백.
 *   4) 발행 실패 시 state=ERROR 로 전이.
 *
 * 실행 컨텍스트: process_init state machine. ctrlr_lock 보유.
 *
 * 호출 체인:
 *   process_init (state=CONFIGURE_AER) → [configure_aer]
 *     → nvme_ctrlr_cmd_set_async_event_config → admin SQ submit
 *     → (완료) configure_aer_done → 초기 AER N개 발행
 */
static int
nvme_ctrlr_configure_aer(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_feat_async_event_configuration	config;
	                                        /* [한국어] Set Features 의 CDW11 비트필드 union — bit 단위 알림 마스크. */
	int						rc;

	config.raw = 0;                         /* [한국어] 모든 비트 0 으로 시작 — 명시 set 한 알림만 받음. */

	/* [한국어] Discovery vs IO 컨트롤러 분기. */
	if (spdk_nvme_ctrlr_is_discovery(ctrlr)) {
		config.bits.discovery_log_change_notice = 1;
		                                /* [한국어] Discovery controller 전용 — discovery log page 변경 시 알림.
		                                 *         host 가 새 NVMe-oF target 발견 또는 기존 target 변경을 즉시 인지. */
	} else {
		/* [한국어] IO controller — SMART critical warning 5종 모두 enable. */
		config.bits.crit_warn.bits.available_spare = 1;
		                                /* [한국어] available spare 가 임계치 이하로 떨어지면 알림. */
		config.bits.crit_warn.bits.temperature = 1;
		                                /* [한국어] composite temperature 가 임계 범위를 벗어나면 알림. */
		config.bits.crit_warn.bits.device_reliability = 1;
		                                /* [한국어] media degradation 으로 신뢰성 저하 시 알림. */
		config.bits.crit_warn.bits.read_only = 1;
		                                /* [한국어] media 가 read-only 모드로 전환됨 — write 더 이상 불가. */
		config.bits.crit_warn.bits.volatile_memory_backup = 1;
		                                /* [한국어] PLP(Power Loss Protection) 휘발성 메모리 백업 장치 실패. */

		/* [한국어] NVMe 1.2+ 추가 OAES 알림 — Identify Controller oaes 비트로 컨트롤러 지원 확인. */
		if (ctrlr->vs.raw >= SPDK_NVME_VERSION(1, 2, 0)) {
			if (ctrlr->cdata.oaes.ns_attribute_notices) {
				config.bits.ns_attr_notice = 1;
				                /* [한국어] NS attach/detach/format/resize 시 알림. clear_changed_ns_log 가
				                 *         후속 처리에서 사용. */
			}
			if (ctrlr->cdata.oaes.fw_activation_notices) {
				config.bits.fw_activation_notice = 1;
				                /* [한국어] firmware activation 발생 시 알림 — 후속 reset 트리거 가능. */
			}
			if (ctrlr->cdata.oaes.ana_change_notices) {
				config.bits.ana_change_notice = 1;
				                /* [한국어] NVMe-oF multipath 의 ANA 상태 변경 알림 — failover 트리거. */
			}
		}
		/* [한국어] NVMe 1.3+ telemetry log notice — lpa.ts (telemetry log support) 비트 확인. */
		if (ctrlr->vs.raw >= SPDK_NVME_VERSION(1, 3, 0) && ctrlr->cdata.lpa.ts) {
			config.bits.telemetry_log_notice = 1;
			                        /* [한국어] telemetry log 가 갱신될 때 알림 — host 가 디버깅 정보 수집 가능. */
		}
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_CONFIGURE_AER,
			     ctrlr->opts.admin_timeout_ms);
	                                        /* [한국어] state 를 대기 상태로 — Set Features 응답까지 다른 진행 중단.
	                                         *         timeout 만료 시 process_init 이 ERROR 로 처리. */

	rc = nvme_ctrlr_cmd_set_async_event_config(ctrlr, config,
			nvme_ctrlr_configure_aer_done,
			ctrlr);
	                                        /* [한국어] Set Features 명령 발행 (FID=0x0B). cb=configure_aer_done,
	                                         *         cb_arg=ctrlr. nvme_ctrlr_cmd.c 내에서 admin SQ submit 까지. */
	if (rc != 0) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		                                /* [한국어] 발행 자체가 실패 — admin queue 문제 또는 mempool 고갈.
		                                 *         ERROR state 로 전이하여 caller(process_init) 가 reset 결정. */
		return rc;
	}

	return 0;                               /* [한국어] 발행 성공 — 콜백 대기. process_init 은 다음 polling 에서
	                                         *         WAIT_FOR_CONFIGURE_AER state 처리(노옵)하다가 콜백 발화 시 다음 state. */
}

/*
 * [한국어]
 * nvme_ctrlr_get_process - 주어진 PID 의 active process 객체를 반환.
 *
 * @ctrlr: 대상 컨트롤러.
 * @pid: 찾을 PID.
 * @return: 일치하는 spdk_nvme_ctrlr_process*, 없으면 NULL.
 *
 * SPDK multi-process 모델: 하나의 NVMe 컨트롤러를 DPDK primary + secondary
 * 프로세스 여러 개가 공유 가능. 각 프로세스마다 자신의 active_reqs, IO qpair
 * 목록, AER 큐, ref count 등 per-process 상태를 보관 — `struct
 * spdk_nvme_ctrlr_process` 가 그 단위. ctrlr->active_procs TAILQ 가 모든
 * 활성 프로세스의 리스트.
 *
 * 동기화: 호출 측이 nvme_ctrlr_lock 보유 가정 (TAILQ 순회 중 다른 프로세스가
 * add/remove 하면 corruption).
 *
 * 실행 컨텍스트: ctrlr_lock 안에서 호출. O(n) 선형 탐색 (n=프로세스 수 — 보통 1~2).
 *
 * 호출 체인:
 *   nvme_ctrlr_get_current_process / add_process(dup 검사) / proc_get_ref / put_ref
 *     → [본 함수]
 */
struct spdk_nvme_ctrlr_process *
nvme_ctrlr_get_process(struct spdk_nvme_ctrlr *ctrlr, pid_t pid)
{
	struct spdk_nvme_ctrlr_process	*active_proc;

	/* [한국어] active_procs TAILQ 순회 — 보통 1~2 entries 라 O(n) 충분. */
	TAILQ_FOREACH(active_proc, &ctrlr->active_procs, tailq) {
		if (active_proc->pid == pid) {
			return active_proc;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * nvme_ctrlr_get_current_process - 현재 프로세스(getpid()) 의 ctrlr_process 객체 반환.
 *
 * 편의 함수 — 가장 흔한 패턴인 "이 프로세스 자신의 per-process state" 조회.
 * 호출자 측에서 ctrlr_lock 보유 가정.
 */
struct spdk_nvme_ctrlr_process *
nvme_ctrlr_get_current_process(struct spdk_nvme_ctrlr *ctrlr)
{
	return nvme_ctrlr_get_process(ctrlr, getpid());
}

/*
 * [한국어]
 * nvme_ctrlr_add_process -
 *   현재 프로세스를 컨트롤러의 active_procs 에 등록. multi-process 환경에서
 *   각 프로세스가 컨트롤러를 attach 할 때 호출된다.
 *
 * @ctrlr: 대상 컨트롤러.
 * @devhandle: PCIe transport 의 device handle (`spdk_pci_device*`) — secondary
 *             process 에서 detach 시 해제하려면 보관 필요.
 * @return: 0 성공 (또는 이미 등록), -1 메모리 할당 실패.
 *
 * 동작:
 *   1. Primary 의 경우: controller construct 시점에 호출 (probe_internal 경로).
 *   2. Secondary 의 경우: probe 시 attach 단계에서 호출.
 *   3. 동일 PID 가 이미 등록돼 있으면 멱등 (no-op).
 *   4. 새 ctrlr_process 객체를 SPDK_MALLOC_SHARE 로 할당 — DPDK 공유 메모리
 *      (hugepage)에 두어 secondary process 가 같은 객체 보고 가능.
 *   5. per-process 필드 초기화: is_primary, pid, active_reqs (큐 깊이 추적),
 *      devhandle, ref=0, allocated_io_qpairs, async_events 큐.
 *
 * 동기화: 호출 측에서 ctrlr_lock 보유 가정.
 *
 * 호출 체인:
 *   probe / attach 경로 → [본 함수] → spdk_zmalloc + TAILQ_INSERT_TAIL
 */
int
nvme_ctrlr_add_process(struct spdk_nvme_ctrlr *ctrlr, void *devhandle)
{
	struct spdk_nvme_ctrlr_process	*ctrlr_proc;
	pid_t				pid = getpid();

	/* Check whether the process is already added or not */
	/* [한국어] 멱등성 — 같은 PID 의 등록 시도가 중복 호출이면 no-op. */
	if (nvme_ctrlr_get_process(ctrlr, pid)) {
		return 0;
	}

	/* Initialize the per process properties for this ctrlr */
	/* [한국어] SPDK_MALLOC_SHARE — DPDK 공유 메모리 (hugepage) 풀에서 할당.
	 * 이래야 secondary process 가 자기 가상주소 공간에서 같은 물리 페이지 접근 가능.
	 * 64B alignment 는 캐시라인 정렬 + SPDK 관례. */
	ctrlr_proc = spdk_zmalloc(sizeof(struct spdk_nvme_ctrlr_process),
				  64, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	if (ctrlr_proc == NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "failed to allocate memory to track the process props\n");

		return -1;
	}

	/* [한국어] per-process 필드 초기화. */
	ctrlr_proc->is_primary = spdk_process_is_primary();  /* DPDK primary 여부 */
	ctrlr_proc->pid = pid;
	STAILQ_INIT(&ctrlr_proc->active_reqs);    /* 발행 중인 admin/io request 추적 */
	ctrlr_proc->devhandle = devhandle;        /* PCIe detach 시 사용 */
	ctrlr_proc->ref = 0;                      /* 이 프로세스의 ref count 시작 */
	TAILQ_INIT(&ctrlr_proc->allocated_io_qpairs);  /* 이 프로세스가 할당한 IO qpair */
	STAILQ_INIT(&ctrlr_proc->async_events);   /* 이 프로세스용 AER 큐 */

	/* [한국어] 컨트롤러의 active_procs TAILQ 끝에 추가. ctrlr_lock 으로 보호. */
	TAILQ_INSERT_TAIL(&ctrlr->active_procs, ctrlr_proc, tailq);

	return 0;
}

/*
 * [한국어]
 * nvme_ctrlr_remove_process -
 *   정상 detach 경로 — 프로세스가 명시적으로 controller 를 떠날 때 호출.
 *
 * @ctrlr: 대상 컨트롤러.
 * @proc: 제거할 ctrlr_process (자기 자신 또는 inactive 정리 대상).
 *
 * 동작 순서:
 *   1. active_reqs 비어 있어야 함 (정상 detach 전제 — admin/io 완료).
 *   2. 이 프로세스가 할당한 모든 IO qpair 를 해제 (spdk_nvme_ctrlr_free_io_qpair).
 *   3. active_procs TAILQ 에서 제거.
 *   4. PCIe transport 면 spdk_pci_device_detach 로 devhandle 해제 (DPDK
 *      PCI 리소스 반환).
 *   5. proc 자체 free (SPDK_MALLOC_SHARE 였으므로 hugepage 반환).
 *
 * 동기화: 호출 측이 ctrlr_lock 보유 가정 (다른 프로세스의 ref/list 조작과 race 방지).
 *
 * 호출 체인:
 *   nvme_ctrlr_proc_put_ref (ref=0 도달 + last 아님)
 *     → [본 함수]
 */
static void
nvme_ctrlr_remove_process(struct spdk_nvme_ctrlr *ctrlr,
			  struct spdk_nvme_ctrlr_process *proc)
{
	struct spdk_nvme_qpair	*qpair, *tmp_qpair;

	/* [한국어] 정상 detach 전제 — 발행 중인 request 없음. ungraceful 종료는
	 * nvme_ctrlr_cleanup_process 가 처리. */
	assert(STAILQ_EMPTY(&proc->active_reqs));

	/* [한국어] 이 프로세스가 보유한 모든 IO qpair 해제. _SAFE 매크로로
	 * iterator 무효화 회피 (free 가 qpair 를 free 하므로 다음 노드 미리 캐싱). */
	TAILQ_FOREACH_SAFE(qpair, &proc->allocated_io_qpairs, per_process_tailq, tmp_qpair) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}

	TAILQ_REMOVE(&ctrlr->active_procs, proc, tailq);

	/* [한국어] PCIe transport 한정 — DPDK 의 PCI 디바이스 핸들 detach. Fabrics
	 * (RDMA/TCP) 는 devhandle=NULL 이므로 해당 없음. */
	if (ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		spdk_pci_device_detach(proc->devhandle);
	}

	/* [한국어] proc 자체 해제 (SPDK_MALLOC_SHARE 로 잡혔으므로 hugepage 반환). */
	spdk_free(proc);
}

/*
 * [한국어]
 * nvme_ctrlr_cleanup_process -
 *   비정상 종료 (kill -9, SEGV 등) 한 프로세스의 자원 강제 정리.
 *
 * @proc: 비정상 종료된 프로세스의 ctrlr_process 객체.
 *
 * 동작:
 *   1. 발행 중이던 모든 request 강제 free — 정상 detach 와 달리 대기 없이 즉시 정리.
 *   2. 미수신 AER 이벤트 free.
 *   3. 할당된 모든 IO qpair 해제 — in_completion_context 강제 0 으로 reset (프로세스가
 *      completion 처리 중에 죽었을 수 있으므로). no_deletion_notification_needed=1
 *      로 컨트롤러에 Delete IO Q admin 명령 skip (이미 죽은 프로세스 위해 발행 무의미).
 *   4. proc 자체 free.
 *
 * normal vs cleanup 비교:
 *   - remove_process: active_reqs 비어 있음 가정 (정상 detach).
 *   - cleanup_process: active_reqs 강제 정리 (kill 등으로 in-flight 남음).
 *
 * 동기화: 호출 측이 ctrlr_lock 보유 가정. 보통 remove_inactive_proc 가
 * `kill(pid, 0) == -1 && errno == ESRCH` 로 죽은 프로세스 감지 후 호출.
 *
 * 호출 체인:
 *   nvme_ctrlr_remove_inactive_proc (PID 존재 확인 실패)
 *     → [본 함수]
 */
static void
nvme_ctrlr_cleanup_process(struct spdk_nvme_ctrlr_process *proc)
{
	struct nvme_request	*req, *tmp_req;
	struct spdk_nvme_qpair	*qpair, *tmp_qpair;
	struct spdk_nvme_ctrlr_aer_completion *event;

	/* [한국어] 발행 중이던 request 들 강제 정리 — 콜백 호출 없이 그냥 free.
	 * 프로세스가 죽었으므로 콜백 대상이 없다. */
	STAILQ_FOREACH_SAFE(req, &proc->active_reqs, stailq, tmp_req) {
		STAILQ_REMOVE(&proc->active_reqs, req, nvme_request, stailq);

		assert(req->pid == proc->pid);
		nvme_cleanup_user_req(req);   /* 사용자 버퍼 unmap 등 */
		nvme_free_request(req);
	}

	/* Remove async event from each process objects event list */
	/* [한국어] 이 프로세스가 받지 못한 AER 이벤트 큐 정리. */
	while (!STAILQ_EMPTY(&proc->async_events)) {
		event = STAILQ_FIRST(&proc->async_events);
		STAILQ_REMOVE_HEAD(&proc->async_events, link);
		spdk_free(event);
	}

	/* [한국어] 이 프로세스가 할당한 IO qpair 들 정리. */
	TAILQ_FOREACH_SAFE(qpair, &proc->allocated_io_qpairs, per_process_tailq, tmp_qpair) {
		TAILQ_REMOVE(&proc->allocated_io_qpairs, qpair, per_process_tailq);

		/*
		 * The process may have been killed while some qpairs were in their
		 *  completion context.  Clear that flag here to allow these IO
		 *  qpairs to be deleted.
		 */
		/* [한국어] 죽은 시점에 completion 폴링 중이었다면 in_completion_context=1
		 * 로 남아 있어 정상 free 가 거부됨 — 여기서 강제 reset. */
		qpair->in_completion_context = 0;

		/* [한국어] no_deletion_notification_needed=1 → Delete IOSQ/IOCQ admin
		 * 명령 발행 skip. 이미 죽은 프로세스를 위해 컨트롤러에 알릴 필요 없음
		 * (성능 + 죽은 프로세스의 qpair 가 어차피 사용 불가). */
		qpair->no_deletion_notification_needed = 1;

		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}

	spdk_free(proc);
}

/**
 * This function will be called when destructing the controller.
 *  1. There is no more admin request on this controller.
 *  2. Clean up any left resource allocation when its associated process is gone.
 */
void
nvme_ctrlr_free_processes(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc, *tmp;

	/* Free all the processes' properties and make sure no pending admin IOs */
	TAILQ_FOREACH_SAFE(active_proc, &ctrlr->active_procs, tailq, tmp) {
		TAILQ_REMOVE(&ctrlr->active_procs, active_proc, tailq);

		assert(STAILQ_EMPTY(&active_proc->active_reqs));

		spdk_free(active_proc);
	}
}

/**
 * This function will be called when any other process attaches or
 *  detaches the controller in order to cleanup those unexpectedly
 *  terminated processes.
 * Note: the ctrlr_lock must be held when calling this function.
 */
static int
nvme_ctrlr_remove_inactive_proc(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc, *tmp;
	int				active_proc_count = 0;

	TAILQ_FOREACH_SAFE(active_proc, &ctrlr->active_procs, tailq, tmp) {
		if ((kill(active_proc->pid, 0) == -1) && (errno == ESRCH)) {
			NVME_CTRLR_ERRLOG(ctrlr, "process %d terminated unexpected\n", active_proc->pid);

			TAILQ_REMOVE(&ctrlr->active_procs, active_proc, tailq);

			nvme_ctrlr_cleanup_process(active_proc);
		} else {
			active_proc_count++;
		}
	}

	return active_proc_count;
}

/*
 * [한국어]
 * nvme_ctrlr_proc_get_ref -
 *   현재 프로세스의 ref count 증가. multi-process 환경의 reference counting 진입.
 *
 * @ctrlr: 대상 컨트롤러.
 *
 * 동작:
 *   1. ctrlr_lock 보유.
 *   2. remove_inactive_proc 로 죽은 프로세스 정리 (housekeeping).
 *   3. 현재 프로세스의 ctrlr_process 찾아 ref++.
 *   4. ctrlr_lock 해제.
 *
 * 용도: bdev_nvme 등 상위 모듈이 컨트롤러를 사용하는 동안 ref 를 잡아 두어,
 * 다른 프로세스가 detach 시도해도 자원이 일찍 회수되지 않게 한다.
 * `nvme_ctrlr_proc_put_ref` 와 대칭 (acquire/release 패턴).
 *
 * 호출 체인:
 *   bdev_nvme 등 → [본 함수] → ref++
 */
void
nvme_ctrlr_proc_get_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc;

	nvme_ctrlr_lock(ctrlr);

	/* [한국어] 죽은 프로세스 정리 — get/put_ref 마다 호출되어 lazy cleanup. */
	nvme_ctrlr_remove_inactive_proc(ctrlr);

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		active_proc->ref++;
	}

	nvme_ctrlr_unlock(ctrlr);
}

/*
 * [한국어]
 * nvme_ctrlr_proc_put_ref -
 *   현재 프로세스의 ref count 감소. 0 도달 시 (단, 마지막 프로세스가 아니면)
 *   process 객체 제거.
 *
 * @ctrlr: 대상 컨트롤러.
 *
 * 동작:
 *   1. lock 보유.
 *   2. 죽은 프로세스 정리 + 살아있는 프로세스 수 카운트.
 *   3. 현재 프로세스의 ref-- + assert(ref >= 0) — under-flow 방지.
 *   4. ref == 0 이고 proc_count != 1 (= 마지막 아님) 이면 remove_process 호출.
 *      마지막 프로세스의 process 객체는 ctrlr destruct 시 일괄 처리.
 *   5. lock 해제.
 *
 * "마지막은 남겨둠" 이유: 마지막 프로세스가 ref=0 으로 detach 하는 순간 컨트롤러
 * 자체를 free 해야 하는데, 그 흐름은 spdk_nvme_detach 의 상위에서 처리.
 *
 * 호출 체인:
 *   bdev_nvme 등 detach 시 → [본 함수] → ref-- → (last 아니면) remove_process
 */
void
nvme_ctrlr_proc_put_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc;
	int				proc_count;

	nvme_ctrlr_lock(ctrlr);

	/* [한국어] 죽은 프로세스 정리 — proc_count 는 정리 후 살아있는 프로세스 수. */
	proc_count = nvme_ctrlr_remove_inactive_proc(ctrlr);

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		active_proc->ref--;
		assert(active_proc->ref >= 0);

		/*
		 * The last active process will be removed at the end of
		 * the destruction of the controller.
		 */
		/* [한국어] ref=0 도달 + 마지막 프로세스가 아닌 경우만 즉시 remove.
		 * 마지막은 ctrlr destruct path 가 nvme_ctrlr_free_processes 로 처리. */
		if (active_proc->ref == 0 && proc_count != 1) {
			nvme_ctrlr_remove_process(ctrlr, active_proc);
		}
	}

	nvme_ctrlr_unlock(ctrlr);
}

int
nvme_ctrlr_get_ref_count(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc;
	int				ref = 0;

	nvme_ctrlr_lock(ctrlr);

	nvme_ctrlr_remove_inactive_proc(ctrlr);

	TAILQ_FOREACH(active_proc, &ctrlr->active_procs, tailq) {
		ref += active_proc->ref;
	}

	nvme_ctrlr_unlock(ctrlr);

	return ref;
}

/**
 *  Get the PCI device handle which is only visible to its associated process.
 */
struct spdk_pci_device *
nvme_ctrlr_proc_get_devhandle(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc;
	struct spdk_pci_device		*devhandle = NULL;

	nvme_ctrlr_lock(ctrlr);

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		devhandle = active_proc->devhandle;
	}

	nvme_ctrlr_unlock(ctrlr);

	return devhandle;
}

/*
 * [한국어]
 * nvme_ctrlr_process_init_vs_done - VS 레지스터 비동기 읽기 완료 콜백 (process_init 첫 단계)
 *
 * @ctx: ctrlr 포인터 (callback context).
 * @value: VS 레지스터 raw 값 (32-bit). MMIO 또는 fabric property get 결과.
 * @cpl: completion. fabric 의 경우 admin command 응답.
 *
 * 동기/배경:
 *   process_init state machine 의 READ_VS 단계 완료. VS = Version 레지스터 (offset 0x08).
 *   여기서 NVMe 1.0/1.1/1.2/1.3/2.0 등 컨트롤러 spec 버전을 알아내며, 이후 단계의 feature
 *   분기(예: ANA 지원, FLBAS 확장 등) 가 이 값을 참조한다.
 *
 *   NVMe 스펙 §3.1.2 VS register:
 *     · bits[31:16] = MJR (Major)
 *     · bits[15:8]  = MNR (Minor)
 *     · bits[7:0]   = TER (Tertiary)
 *
 * 다음 state: READ_CAP (CAP 레지스터 읽기).
 */
static void
nvme_ctrlr_process_init_vs_done(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;    /* [한국어] callback context = ctrlr. */

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the VS register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		                                /* [한국어] register read 실패는 fatal — ERROR state 로 전이. */
		return;
	}

	assert(value <= UINT32_MAX);            /* [한국어] VS 는 32-bit 레지스터 — uint64_t 컨테이너에서 상위 32-bit 는 0 이어야 함. */
	ctrlr->vs.raw = (uint32_t)value;        /* [한국어] 캐시에 저장 — 이후 spdk_nvme_ctrlr_get_regs_vs() 등에서 사용. */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READ_CAP, NVME_TIMEOUT_INFINITE);
	                                        /* [한국어] 다음 단계: CAP 레지스터 읽기. INFINITE = 다음 state 에서 자체 timeout 설정. */
}

/*
 * [한국어]
 * nvme_ctrlr_process_init_cap_done - CAP 레지스터 비동기 읽기 완료 콜백
 *
 * @value: CAP 레지스터 raw 값 (64-bit). 다양한 컨트롤러 capability 정보 인코딩.
 *
 * 동기/배경:
 *   NVMe 스펙 §3.1.4 CAP register (offset 0x00, 64-bit). 이 콜백은 CAP 값을 캐시하고
 *   nvme_ctrlr_init_cap() 으로 ctrlr 의 파생 필드들 (max_io_queues, min/max page size, MQES,
 *   timeout 단위 등) 을 계산한 뒤 CHECK_EN 단계로 진입.
 *
 *   주요 CAP 비트 (init_cap 에서 사용):
 *     · MQES[15:0]: Maximum Queue Entries Supported.
 *     · TO[31:24]: Timeout (500ms 단위) — controller ready 대기 시간.
 *     · DSTRD[35:32]: Doorbell Stride.
 *     · NSSRS[36]: NVMe Subsystem Reset 지원.
 *     · CSS[44:37]: Command Sets Supported (NVM/Discovery/IO command set).
 *     · MPSMIN/MPSMAX[51:48]/[55:52]: Memory Page Size 한도.
 *
 * 다음 state: CHECK_EN (CC.EN 현재 값 확인).
 */
static void
nvme_ctrlr_process_init_cap_done(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CAP register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	ctrlr->cap.raw = value;                 /* [한국어] CAP 64-bit raw 캐시. value 자체가 64-bit 이므로 전 비트 보존. */
	nvme_ctrlr_init_cap(ctrlr);             /* [한국어] CAP 파생 필드 계산 — ready_timeout_in_ms, min/max_page_size 등. */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_CHECK_EN, NVME_TIMEOUT_INFINITE);
	                                        /* [한국어] 다음 단계: CC.EN 비트 확인 → enable/disable 시퀀스 분기. */
}

/*
 * [한국어]
 * nvme_ctrlr_process_init_check_en - CC.EN 비트 확인 후 disable 시퀀스 분기
 *
 * @value: CC 레지스터 raw 값 (32-bit).
 *
 * 동기/배경:
 *   CC.EN (Controller Configuration, bit 0) 의 현재 값에 따라 분기:
 *     · CC.EN==1: 이미 enable 상태 (이전 host 가 그렇게 두고 종료) — 먼저 CSTS.RDY=1 까지
 *       대기한 뒤 CC.EN=0 write 로 disable 진행 (스펙: enable 후에만 정상 disable 가능).
 *     · CC.EN==0: 이미 disable 상태이지만 CSTS.RDY=0 도 확인하여 진짜 disable 인지 검증.
 *
 *   이렇게 하는 이유: SPDK 가 매 attach 시 항상 fresh enable 시퀀스를 거치기 위함. 이전 상태가
 *   어떻든 깨끗한 disable→enable 한 사이클 후 IO queue 등 자원을 새로 만든다.
 *
 * 다음 state: DISABLE_WAIT_FOR_READY_1 또는 DISABLE_WAIT_FOR_READY_0.
 */
static void
nvme_ctrlr_process_init_check_en(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	enum nvme_ctrlr_state state;            /* [한국어] CC.EN 분기 결과 state. */

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CC register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	assert(value <= UINT32_MAX);
	ctrlr->process_init_cc.raw = (uint32_t)value;
	                                        /* [한국어] CC 캐시 — 이후 set_en_0_read_cc 등에서 다른 비트(CSS/MPS/AMS) 보존하며 EN 만 토글. */

	if (ctrlr->process_init_cc.bits.en) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "CC.EN = 1\n");
		state = NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1;
		                                /* [한국어] enable 상태 — 정상 disable 위해 CSTS.RDY=1 먼저 확인. */
	} else {
		state = NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0;
		                                /* [한국어] disable 상태 — CSTS.RDY=0 확인하여 일관성 검증. */
	}

	nvme_ctrlr_set_state(ctrlr, state, nvme_ctrlr_get_ready_timeout(ctrlr));
	                                        /* [한국어] timeout = CAP.TO (500ms 단위) 변환값 — 컨트롤러가 advertise 한 reset 시간. */
}

/*
 * [한국어]
 * nvme_ctrlr_process_init_set_en_0 - CC.EN=0 write 완료 콜백 (★ 컨트롤러 disable 핵심 단계)
 *
 * @value: CC write 후 응답값 (사용 안 함 — write 완료 신호용).
 *
 * 동기/배경:
 *   CC.EN=0 으로 controller disable 발동. NVMe 스펙 §3.1.5/§7.3.2 에 따라 host 가 CC.EN 을
 *   1→0 으로 write 한 후, 컨트롤러는 모든 outstanding command 를 abort 하고 CSTS.RDY 를 0 으로
 *   transition. host 는 그 transition 을 polling 으로 확인해야 함.
 *
 *   PCIe 디바이스 quirk:
 *     일부 SSD 는 CC.EN=0 직후 짧은 시간 동안 PCI config space 또는 BAR access 가 unstable.
 *     NVME_QUIRK_DELAY_BEFORE_CHK_RDY 가 set 되어 있으면 2.5 초 sleep 적용. SPDK 는 polled-mode
 *     이므로 sleep() 호출 대신 sleep_timeout_tsc 를 미래 tick 으로 set 하여 process_init 이
 *     그 시점까지 노옵으로 대기.
 *
 * 다음 state: DISABLE_WAIT_FOR_READY_0 (CSTS.RDY=0 polling).
 */
static void
nvme_ctrlr_process_init_set_en_0(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to write the CC register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	/*
	 * Wait 2.5 seconds before accessing PCI registers.
	 * Not using sleep() to avoid blocking other controller's initialization.
	 */
	/* [한국어] PCIe 호환성 quirk — 일부 SSD 는 CC.EN=0 직후 register access 불안정.
	 *         polled-mode 라 실제 sleep() 호출하면 다른 ctrlr 의 init 까지 막히므로
	 *         sleep_timeout_tsc 만 set 해두고 process_init 이 그 시점까지 노옵으로 통과. */
	if (ctrlr->quirks & NVME_QUIRK_DELAY_BEFORE_CHK_RDY) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Applying quirk: delay 2.5 seconds before reading registers\n");
		ctrlr->sleep_timeout_tsc = spdk_get_ticks() + (2500 * spdk_get_ticks_hz() / 1000);
		                                /* [한국어] now + 2500ms (in ticks). 2500*hz/1000 = 2.5s 분량 tick. */
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0,
			     nvme_ctrlr_get_ready_timeout(ctrlr));
	                                        /* [한국어] CSTS.RDY=0 으로 transition 될 때까지 polling. */
}

/*
 * [한국어]
 * nvme_ctrlr_process_init_set_en_0_read_cc - CC read → EN bit 만 0 으로 mask → 다시 write
 *
 * @value: CC read 결과 (현재 CC 값).
 *
 * 동기/배경:
 *   "EN 비트만 0 으로 토글" 을 정확히 하기 위한 read-modify-write 패턴. CC 에는 EN 외에도
 *   CSS (Command Set Selected), MPS (Memory Page Size), AMS (Arbitration Mechanism), SHN
 *   (Shutdown Notification), IOSQES/IOCQES (queue entry size) 등 다른 비트들이 있고,
 *   이 비트들을 보존해야 한다 (init_cap 에서 enable 시 다시 설정하지만, disable 단계에서
 *   임의로 0 을 만들면 컨트롤러가 비정상 동작할 수 있음).
 *
 * 동작 단계:
 *   1) cpl error → ERROR state.
 *   2) value 캐스트 후 cc.raw 에 적재.
 *   3) cc.bits.en = 0 으로 mask.
 *   4) state=SET_EN_0_WAIT_FOR_CC (write 완료 대기).
 *   5) set_cc_async 로 CC write 발행 — 완료 시 set_en_0 콜백.
 *
 * 다음 state: SET_EN_0_WAIT_FOR_CC → set_en_0 콜백 → DISABLE_WAIT_FOR_READY_0.
 */
static void
nvme_ctrlr_process_init_set_en_0_read_cc(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	union spdk_nvme_cc_register cc;         /* [한국어] CC 비트필드 union — bits.en 등 named access. */
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CC register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	assert(value <= UINT32_MAX);
	cc.raw = (uint32_t)value;               /* [한국어] read 결과를 union 에 적재. 다른 비트는 그대로 유지. */
	cc.bits.en = 0;                         /* [한국어] EN 비트(0번) 만 0 으로 mask — 다른 비트 보존 (read-modify-write 핵심). */
	ctrlr->process_init_cc.raw = cc.raw;    /* [한국어] 캐시 갱신 — 이후 enable 시퀀스에서 이 값 기반으로 다시 EN=1. */

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_EN_0_WAIT_FOR_CC,
			     nvme_ctrlr_get_ready_timeout(ctrlr));
	                                        /* [한국어] CC write 완료 대기 state. timeout = CAP.TO 변환값. */

	rc = nvme_ctrlr_set_cc_async(ctrlr, cc.raw, nvme_ctrlr_process_init_set_en_0, ctrlr);
	                                        /* [한국어] CC write 비동기 발행. PCIe 는 MMIO write, fabric 은 property set
	                                         *         admin command. 완료 시 set_en_0 콜백 → CSTS.RDY=0 polling 진입. */
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_cc() failed\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}
}

/*
 * [한국어]
 * nvme_ctrlr_process_init_wait_for_ready_1 - CSTS.RDY=1 polling 콜백 (CC.EN=1 후 ready 도달 대기)
 *
 * @value: CSTS 레지스터 raw 값 (32-bit).
 *
 * 동기/배경:
 *   "이미 enable 되어 있던 컨트롤러" 를 정상 disable 하기 전에 CSTS.RDY=1 인지 확인하는 단계.
 *   CSTS.RDY=1 = 컨트롤러가 admin queue 처리 가능 상태 → 이때만 정상적으로 CC.EN=0 disable
 *   가능. CSTS.CFS=1 (Controller Fatal Status) 면 컨트롤러가 fatal error 상태 — 이 경우도
 *   disable 진행 (어차피 reset 으로 회복 시도).
 *
 * MMIO read 실패 처리 (resilience):
 *   reset 진행 중인 디바이스는 일시적으로 MMIO read 가 실패할 수 있다. is_failed==false 이고
 *   timeout 미만이면 재시도(같은 state 로 복귀, KEEP_EXISTING timeout). 그 외에는 ERROR.
 *
 * 분기:
 *   · CSTS.RDY=1 또는 CFS=1 → SET_EN_0 단계로 진입 (CC read-modify-write 시작).
 *   · 둘 다 0 → 같은 state 재귀 (quiet, 로그 폭주 방지) 하여 다음 polling 에서 다시 read.
 */
static void
nvme_ctrlr_process_init_wait_for_ready_1(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	union spdk_nvme_csts_register csts;     /* [한국어] CSTS 비트필드 — bits.rdy, bits.cfs 등. */

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* While a device is resetting, it may be unable to service MMIO reads
		 * temporarily. Allow for this case.
		 */
		/* [한국어] reset 중 일시적 MMIO 실패는 재시도 — failed 가 아니고 timeout 미만이면 같은 state 로 retry. */
		if (!ctrlr->is_failed && ctrlr->state_timeout_tsc != NVME_TIMEOUT_INFINITE) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Failed to read the CSTS register\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1,
					     NVME_TIMEOUT_KEEP_EXISTING);
			                        /* [한국어] KEEP_EXISTING = 기존 timeout_tsc 유지 — 누적 시간 추적 위해 reset 안 함. */
		} else {
			NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CSTS register\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		}

		return;
	}

	assert(value <= UINT32_MAX);
	csts.raw = (uint32_t)value;
	if (csts.bits.rdy == 1 || csts.bits.cfs == 1) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_EN_0,
				     nvme_ctrlr_get_ready_timeout(ctrlr));
		                                /* [한국어] RDY=1(정상) 또는 CFS=1(fatal). 어느 쪽이든 disable 시퀀스 진입. */
	} else {
		NVME_CTRLR_DEBUGLOG(ctrlr, "CC.EN = 1 && CSTS.RDY = 0 - waiting for reset to complete\n");
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1,
					   NVME_TIMEOUT_KEEP_EXISTING);
		                                /* [한국어] 아직 ready=0 → 같은 state 로 재귀. quiet 으로 같은 메시지 로그 폭주 방지. */
	}
}

/*
 * [한국어]
 * nvme_ctrlr_process_init_wait_for_ready_0 - CSTS.RDY=0 polling 콜백 (CC.EN=0 후 disable 완료 대기)
 *
 * @value: CSTS 레지스터 raw 값.
 *
 * 동기/배경:
 *   CC.EN=0 write 후 컨트롤러가 disable 완료 되면 CSTS.RDY 도 0 으로 바뀌어야 한다. 이 transition
 *   까지 polling. RDY=0 도달 시 DISABLED state 로 진입 (이후 enable 또는 reset 의 종착).
 *
 *   실패 시 처리는 wait_for_ready_1 과 동일 (transient MMIO 실패는 재시도).
 *
 * 분기:
 *   · CSTS.RDY=0 → DISABLED state. 이로써 disable 완료.
 *   · RDY=1 → 같은 state 재귀 (quiet).
 */
static void
nvme_ctrlr_process_init_wait_for_ready_0(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	union spdk_nvme_csts_register csts;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* While a device is resetting, it may be unable to service MMIO reads
		 * temporarily. Allow for this case.
		 */
		if (!ctrlr->is_failed && ctrlr->state_timeout_tsc != NVME_TIMEOUT_INFINITE) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Failed to read the CSTS register\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0,
					     NVME_TIMEOUT_KEEP_EXISTING);
		} else {
			NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CSTS register\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		}

		return;
	}

	assert(value <= UINT32_MAX);
	csts.raw = (uint32_t)value;
	if (csts.bits.rdy == 0) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "CC.EN = 0 && CSTS.RDY = 0\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLED,
				     nvme_ctrlr_get_ready_timeout(ctrlr));
		                                /* [한국어] disable 완료 — DISABLED state 진입.
		                                 *         이후 사용자/process_init 이 enable 시퀀스 또는 reset 종료 결정. */
	} else {
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0,
					   NVME_TIMEOUT_KEEP_EXISTING);
		                                /* [한국어] 아직 ready=1 — 다음 polling 에서 다시 CSTS read. */
	}
}

/*
 * [한국어]
 * nvme_ctrlr_process_init_enable_wait_for_ready_1 - CC.EN=1 write 후 CSTS.RDY=1 도달 대기 콜백
 *
 * @value: CSTS 레지스터 raw 값.
 *
 * 동기/배경:
 *   nvme_ctrlr_enable() 이 ASQ/ACQ/AQA/CC.EN=1 까지 다 setup 한 후, 컨트롤러가 admin queue
 *   처리 준비가 끝나면 CSTS.RDY 를 1로 set 한다. 그 transition 을 polling. RDY=1 도달 시
 *   RESET_ADMIN_QUEUE state 로 진입 — 이후 Identify Controller 등 본격적인 admin command 시작.
 *
 *   "controller is ready" 라는 로그가 정상 bring-up 의 핵심 마일스톤.
 *
 * 분기:
 *   · CSTS.RDY=1 → RESET_ADMIN_QUEUE state. 이후 Identify, Set Features 등 일련의 init 진행.
 *   · RDY=0 → 같은 state 재귀 (quiet).
 */
static void
nvme_ctrlr_process_init_enable_wait_for_ready_1(void *ctx, uint64_t value,
		const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	union spdk_nvme_csts_register csts;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* While a device is resetting, it may be unable to service MMIO reads
		 * temporarily. Allow for this case.
		 */
		if (!ctrlr->is_failed && ctrlr->state_timeout_tsc != NVME_TIMEOUT_INFINITE) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Failed to read the CSTS register\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1,
					     NVME_TIMEOUT_KEEP_EXISTING);
		} else {
			NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CSTS register\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		}

		return;
	}

	assert(value <= UINT32_MAX);
	csts.raw = value;
	if (csts.bits.rdy == 1) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "CC.EN = 1 && CSTS.RDY = 1 - controller is ready\n");
		                                /* [한국어] ★ bring-up 의 핵심 마일스톤 — admin queue 사용 가능. */
		/*
		 * The controller has been enabled.
		 *  Perform the rest of initialization serially.
		 */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_RESET_ADMIN_QUEUE,
				     ctrlr->opts.admin_timeout_ms);
		                                /* [한국어] 다음 단계: admin queue reset 및 Identify Controller 등 본격 init.
		                                 *         timeout 단위가 ready_timeout(CAP.TO) 에서 admin_timeout_ms 로 변경 —
		                                 *         이제부터는 admin command 응답 시간 기준. */
	} else {
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1,
					   NVME_TIMEOUT_KEEP_EXISTING);
		                                /* [한국어] 아직 ready=0 — 다음 polling 에서 다시 CSTS read. quiet 으로 로그 폭주 방지. */
	}
}

/**
 * This function will be called repeatedly during initialization until the controller is ready.
 */
/*
 * [한국어]
 * nvme_ctrlr_process_init - ★★★ NVMe controller bring-up 상태머신 driver ★★★
 *
 * @ctrlr: 진행 대상 controller
 * @return 0 (정상 진행 또는 READY 도달), 음수 errno (치명적 실패).
 *
 * 이 함수는 사용자 reactor 루프에서 spdk_nvme_probe_poll_async를 호출할 때마다
 * (정확히는 nvme_ctrlr_poll_internal 경유로) 호출되어 ctrlr->state 머신을 한 단계씩 진행시킨다.
 * READY 도달까지 수십~수백 번 호출됨.
 *
 * 동작 패턴:
 *   [Stage A] sleep_timeout_tsc 검사 — 일부 quirky device는 reset 후 짧은 delay 필요.
 *             tsc <= sleep_timeout_tsc면 즉시 return 0 (FSM 진입 안 함).
 *   [Stage B] switch(state)로 dispatch — 각 case는 두 종류:
 *             - "동기 단계" (예: SET_EN_0): 작업 즉시 수행 + nvme_ctrlr_set_state로 다음 상태 전이
 *             - "비동기 단계" (예: READ_VS): 비동기 register/admin 명령 발행 + WAIT_FOR_* 상태 전이
 *               → 응답 도착 시 callback이 다음 상태로 전이
 *   [Stage C] WAIT_FOR_* 상태들은 timeout 검사:
 *             spdk_get_ticks() > state_timeout_tsc면 ERROR 상태로 전이.
 *
 * 전체 시퀀스 (정상 경로):
 *   INIT_DELAY (옵션, quirky device)
 *     → CONNECT_ADMINQ → WAIT_FOR_CONNECT_ADMINQ
 *     → READ_VS → WAIT_FOR_VS (NVMe spec 버전 확인)
 *     → READ_CAP → WAIT_FOR_CAP (controller capability 확인)
 *     → CHECK_EN → WAIT_FOR_CC (현재 CC.EN 상태 확인)
 *     ─ if CC.EN==1 ─→ DISABLE_WAIT_FOR_READY_1 → SET_EN_0 → DISABLE_WAIT_FOR_READY_0
 *     ─ if CC.EN==0 ─→ DISABLED (직접 enable로 진행)
 *     → DISABLED → ENABLE (CC.EN=1, 100us delay 포함)
 *     → ENABLE_WAIT_FOR_READY_1 (CSTS.RDY=1까지 폴링)
 *     → RESET_ADMIN_QUEUE
 *     → IDENTIFY → WAIT_FOR_IDENTIFY (Identify Controller, CNS=0x01)
 *     → CONFIGURE_AER (Async Event Request 설정)
 *     → SET_KEEP_ALIVE_TIMEOUT (NVMe-oF에서 host alive 통보)
 *     → IDENTIFY_IOCS_SPECIFIC (ZNS 등 IOCS-specific identify, NVMe 2.0)
 *     → GET_ZNS_CMD_EFFECTS_LOG (ZNS 사용 시 effects log)
 *     → SET_NUM_QUEUES (Set Features, FID=0x07로 IO qpair 수 협상)
 *     → IDENTIFY_ACTIVE_NS (Active NS list, CNS=0x02)
 *     → IDENTIFY_NS, IDENTIFY_ID_DESCS, IDENTIFY_NS_IOCS_SPECIFIC (각 NS에 대해 반복)
 *     → SET_SUPPORTED_LOG_PAGES, SET_SUPPORTED_INTEL_LOG_PAGES, SET_SUPPORTED_FEATURES
 *     → SET_HOST_FEATURE (Host Behavior Support feature)
 *     → SET_DB_BUF_CFG (NVMe 1.3 doorbell buffer config)
 *     → SET_HOST_ID (Set Features, FID=0x81로 Host Identifier)
 *     → TRANSPORT_READY (트랜스포트별 추가 ready 콜백)
 *     → READY (사용자에게 attach_cb 호출)
 *
 * Reset 경로: 사용자가 spdk_nvme_ctrlr_reset 호출 시 ctrlr->state를 INIT으로 되돌림 → 이 함수가 같은
 *            시퀀스를 다시 진행. Identify는 캐시되므로 짧음.
 *
 * Error 경로: 어느 단계든 치명적 실패 시 ERROR 상태로 전이 → nvme_ctrlr_poll_internal이 감지 후
 *            failed_ctxs로 이동 + destruct_async.
 *
 * 호출자: nvme_ctrlr_poll_internal (lib/nvme/nvme.c) — probe_poll_async가 init_ctrlrs 순회 시 호출.
 */
int
nvme_ctrlr_process_init(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t ready_timeout_in_ms;
	uint64_t ticks;
	int rc = 0;

	ticks = spdk_get_ticks();
                                  /* [한국어] 현재 tick — sleep_timeout_tsc 검사와 timeout 계산에 사용 */

	/*
	 * May need to avoid accessing any register on the target controller
	 * for a while. Return early without touching the FSM.
	 * Check sleep_timeout_tsc > 0 for unit test.
	 */
	if ((ctrlr->sleep_timeout_tsc > 0) &&
	    (ticks <= ctrlr->sleep_timeout_tsc)) {
                                  /* [한국어] Quirky device용 delay — reset 직후 register access 회피.
                                   *  unit test에서 sleep_timeout_tsc=0으로 끄는 케이스 대비 > 0 검사 추가. */
		return 0;
	}
	ctrlr->sleep_timeout_tsc = 0;
                                  /* [한국어] sleep 종료 — 명시적으로 0으로 클리어 */

	ready_timeout_in_ms = nvme_ctrlr_get_ready_timeout(ctrlr);
                                  /* [한국어] CAP.TO 기반 ready timeout 계산 — controller spec이 정의한 ready 도달 한도 */

	/*
	 * Check if the current initialization step is done or has timed out.
	 */
	switch (ctrlr->state) {
                                  /* [한국어] 거대한 dispatch — 40+ case의 상태머신.
                                   *  각 case는 동기 또는 비동기 작업 + 다음 상태 전이 처리. */
	case NVME_CTRLR_STATE_INIT_DELAY:
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT, ready_timeout_in_ms);
		if (ctrlr->quirks & NVME_QUIRK_DELAY_BEFORE_INIT) {
			/*
			 * Controller may need some delay before it's enabled.
			 *
			 * This is a workaround for an issue where the PCIe-attached NVMe controller
			 * is not ready after VFIO reset. We delay the initialization rather than the
			 * enabling itself, because this is required only for the very first enabling
			 * - directly after a VFIO reset.
			 */
			NVME_CTRLR_DEBUGLOG(ctrlr, "Adding 2 second delay before initializing the controller\n");
			ctrlr->sleep_timeout_tsc = ticks + (2000 * spdk_get_ticks_hz() / 1000);
		}
		break;

	case NVME_CTRLR_STATE_DISCONNECTED:
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT, NVME_TIMEOUT_INFINITE);
		break;

	case NVME_CTRLR_STATE_CONNECT_ADMINQ: /* synonymous with NVME_CTRLR_STATE_INIT and NVME_CTRLR_STATE_DISCONNECTED */
		rc = nvme_transport_ctrlr_connect_qpair(ctrlr, ctrlr->adminq);
		if (rc == 0) {
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_CONNECT_ADMINQ,
					     NVME_TIMEOUT_INFINITE);
		} else {
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		}
		break;

	case NVME_CTRLR_STATE_WAIT_FOR_CONNECT_ADMINQ:
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);

		switch (nvme_qpair_get_state(ctrlr->adminq)) {
		case NVME_QPAIR_CONNECTING:
			if (ctrlr->is_failed) {
				nvme_transport_ctrlr_disconnect_qpair(ctrlr, ctrlr->adminq);
				break;
			}

			break;
		case NVME_QPAIR_CONNECTED:
			nvme_qpair_set_state(ctrlr->adminq, NVME_QPAIR_ENABLED);
		/* Fall through */
		case NVME_QPAIR_ENABLED:
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READ_VS,
					     NVME_TIMEOUT_INFINITE);
			/* Abort any queued requests that were sent while the adminq was connecting
			 * to avoid stalling the init process during a reset, as requests don't get
			 * resubmitted while the controller is resetting and subsequent commands
			 * would get queued too.
			 */
			nvme_qpair_abort_queued_reqs(ctrlr->adminq);
			break;
		case NVME_QPAIR_DISCONNECTING:
			assert(ctrlr->adminq->async == true);
			break;
		case NVME_QPAIR_DISCONNECTED:
		/* fallthrough */
		default:
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
			break;
		}

		break;

	case NVME_CTRLR_STATE_READ_VS:
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READ_VS_WAIT_FOR_VS, NVME_TIMEOUT_INFINITE);
		rc = nvme_ctrlr_get_vs_async(ctrlr, nvme_ctrlr_process_init_vs_done, ctrlr);
		break;

	case NVME_CTRLR_STATE_READ_CAP:
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READ_CAP_WAIT_FOR_CAP, NVME_TIMEOUT_INFINITE);
		rc = nvme_ctrlr_get_cap_async(ctrlr, nvme_ctrlr_process_init_cap_done, ctrlr);
		break;

	case NVME_CTRLR_STATE_CHECK_EN:
		/* Begin the hardware initialization by making sure the controller is disabled. */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_CHECK_EN_WAIT_FOR_CC, ready_timeout_in_ms);
		rc = nvme_ctrlr_get_cc_async(ctrlr, nvme_ctrlr_process_init_check_en, ctrlr);
		break;

	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1:
		/*
		 * Controller is currently enabled. We need to disable it to cause a reset.
		 *
		 * If CC.EN = 1 && CSTS.RDY = 0, the controller is in the process of becoming ready.
		 *  Wait for the ready bit to be 1 before disabling the controller.
		 */
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS,
					   NVME_TIMEOUT_KEEP_EXISTING);
		rc = nvme_ctrlr_get_csts_async(ctrlr, nvme_ctrlr_process_init_wait_for_ready_1, ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_EN_0:
		NVME_CTRLR_DEBUGLOG(ctrlr, "Setting CC.EN = 0\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_EN_0_WAIT_FOR_CC, ready_timeout_in_ms);
		rc = nvme_ctrlr_get_cc_async(ctrlr, nvme_ctrlr_process_init_set_en_0_read_cc, ctrlr);
		break;

	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0:
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0_WAIT_FOR_CSTS,
					   NVME_TIMEOUT_KEEP_EXISTING);
		rc = nvme_ctrlr_get_csts_async(ctrlr, nvme_ctrlr_process_init_wait_for_ready_0, ctrlr);
		break;

	case NVME_CTRLR_STATE_DISABLED:
		if (ctrlr->is_disconnecting) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Ctrlr was disabled.\n");
		} else {
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE, ready_timeout_in_ms);

			/*
			 * Delay 100us before setting CC.EN = 1.  Some NVMe SSDs miss CC.EN getting
			 *  set to 1 if it is too soon after CSTS.RDY is reported as 0.
			 */
			spdk_delay_us(100);
		}
		break;

	case NVME_CTRLR_STATE_ENABLE:
		NVME_CTRLR_DEBUGLOG(ctrlr, "Setting CC.EN = 1\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_CC, ready_timeout_in_ms);
		rc = nvme_ctrlr_enable(ctrlr);
		if (rc) {
			NVME_CTRLR_ERRLOG(ctrlr, "Ctrlr enable failed with error: %d", rc);
		}
		return rc;

	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1:
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS,
					   NVME_TIMEOUT_KEEP_EXISTING);
		rc = nvme_ctrlr_get_csts_async(ctrlr, nvme_ctrlr_process_init_enable_wait_for_ready_1,
					       ctrlr);
		break;

	case NVME_CTRLR_STATE_RESET_ADMIN_QUEUE:
		nvme_transport_qpair_reset(ctrlr->adminq);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY, NVME_TIMEOUT_INFINITE);
		break;

	case NVME_CTRLR_STATE_IDENTIFY:
		rc = nvme_ctrlr_identify(ctrlr);
		break;

	case NVME_CTRLR_STATE_CONFIGURE_AER:
		rc = nvme_ctrlr_configure_aer(ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT:
		rc = nvme_ctrlr_set_keep_alive_timeout(ctrlr);
		break;

	case NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC:
		rc = nvme_ctrlr_identify_iocs_specific(ctrlr);
		break;

	case NVME_CTRLR_STATE_GET_ZNS_CMD_EFFECTS_LOG:
		rc = nvme_ctrlr_get_zns_cmd_and_effects_log(ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_NUM_QUEUES:
		nvme_ctrlr_update_nvmf_ioccsz(ctrlr);
		rc = nvme_ctrlr_set_num_queues(ctrlr);
		break;

	case NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS:
		_nvme_ctrlr_identify_active_ns(ctrlr);
		break;

	case NVME_CTRLR_STATE_IDENTIFY_NS:
		rc = nvme_ctrlr_identify_namespaces(ctrlr);
		break;

	case NVME_CTRLR_STATE_IDENTIFY_ID_DESCS:
		rc = nvme_ctrlr_identify_id_desc_namespaces(ctrlr);
		break;

	case NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC:
		rc = nvme_ctrlr_identify_namespaces_iocs_specific(ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_SUPPORTED_LOG_PAGES:
		rc = nvme_ctrlr_set_supported_log_pages(ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_SUPPORTED_INTEL_LOG_PAGES:
		rc = nvme_ctrlr_set_intel_support_log_pages(ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES:
		nvme_ctrlr_set_supported_features(ctrlr);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_HOST_FEATURE,
				     ctrlr->opts.admin_timeout_ms);
		break;

	case NVME_CTRLR_STATE_SET_HOST_FEATURE:
		rc = nvme_ctrlr_set_host_feature(ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_DB_BUF_CFG:
		rc = nvme_ctrlr_set_doorbell_buffer_config(ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_HOST_ID:
		rc = nvme_ctrlr_set_host_id(ctrlr);
		break;

	case NVME_CTRLR_STATE_TRANSPORT_READY:
		rc = nvme_transport_ctrlr_ready(ctrlr);
		if (rc) {
			NVME_CTRLR_ERRLOG(ctrlr, "Transport controller ready step failed: rc %d\n", rc);
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		} else {
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READY, NVME_TIMEOUT_INFINITE);
		}
		break;

	case NVME_CTRLR_STATE_READY:
		NVME_CTRLR_DEBUGLOG(ctrlr, "Ctrlr already in ready state\n");
		return 0;

	case NVME_CTRLR_STATE_ERROR:
		NVME_CTRLR_ERRLOG(ctrlr, "Ctrlr is in error state\n");
		return -1;

	case NVME_CTRLR_STATE_READ_VS_WAIT_FOR_VS:
	case NVME_CTRLR_STATE_READ_CAP_WAIT_FOR_CAP:
	case NVME_CTRLR_STATE_CHECK_EN_WAIT_FOR_CC:
	case NVME_CTRLR_STATE_SET_EN_0_WAIT_FOR_CC:
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS:
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0_WAIT_FOR_CSTS:
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_CC:
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS:
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY:
	case NVME_CTRLR_STATE_WAIT_FOR_CONFIGURE_AER:
	case NVME_CTRLR_STATE_WAIT_FOR_KEEP_ALIVE_TIMEOUT:
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_IOCS_SPECIFIC:
	case NVME_CTRLR_STATE_WAIT_FOR_GET_ZNS_CMD_EFFECTS_LOG:
	case NVME_CTRLR_STATE_WAIT_FOR_SET_NUM_QUEUES:
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ACTIVE_NS:
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS:
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ID_DESCS:
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS_IOCS_SPECIFIC:
	case NVME_CTRLR_STATE_WAIT_FOR_SUPPORTED_INTEL_LOG_PAGES:
	case NVME_CTRLR_STATE_WAIT_FOR_SET_HOST_FEATURE:
	case NVME_CTRLR_STATE_WAIT_FOR_DB_BUF_CFG:
	case NVME_CTRLR_STATE_WAIT_FOR_HOST_ID:
		/*
		 * nvme_ctrlr_process_init() may be called from the completion context
		 * for the admin qpair. Avoid recursive calls for this case.
		 */
		if (!ctrlr->adminq->in_completion_context) {
			spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		}
		break;

	default:
		assert(0);
		return -1;
	}

	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "Ctrlr operation failed with error: %d, ctrlr state: %d (%s)\n",
				  rc, ctrlr->state, nvme_ctrlr_state_string(ctrlr->state));
	}

	/* Note: we use the ticks captured when we entered this function.
	 * This covers environments where the SPDK process gets swapped out after
	 * we tried to advance the state but before we check the timeout here.
	 * It is not normal for this to happen, but harmless to handle it in this
	 * way.
	 */
	if (ctrlr->state_timeout_tsc != NVME_TIMEOUT_INFINITE &&
	    ticks > ctrlr->state_timeout_tsc) {
		NVME_CTRLR_ERRLOG(ctrlr, "Initialization timed out in state %d (%s)\n",
				  ctrlr->state, nvme_ctrlr_state_string(ctrlr->state));
		return -1;
	}

	return rc;
}

/*
 * [한국어]
 * nvme_robust_mutex_init_recursive_shared - 재귀 호출 가능 + multi-process robust mutex 초기화
 *
 * @mtx: 초기화할 pthread_mutex_t (hugepage 공유 영역)
 * @return 0 성공, -1 실패.
 *
 * "RECURSIVE" 의미: 같은 thread가 같은 mutex를 여러 번 lock 가능 — depth 카운터로 관리.
 *                  같은 thread의 nested 함수 호출이 같은 락을 다시 잡을 때 deadlock 회피.
 *
 * Linux: PROCESS_SHARED + ROBUST + RECURSIVE 3종 속성 — multi-process crash-safe + 재귀 가능.
 * FreeBSD: ROBUST/PSHARED 미지원 — RECURSIVE만 적용.
 *
 * vs nvme_robust_mutex_init_shared (nvme.c): driver 전역 lock은 재귀 안 필요 (단순 lock/unlock),
 * controller lock은 재귀 가능 (admin command 발행 시 내부 함수가 다시 lock 획득).
 */
int
nvme_robust_mutex_init_recursive_shared(pthread_mutex_t *mtx)
{
	pthread_mutexattr_t attr;
	int rc = 0;

	if (pthread_mutexattr_init(&attr)) {
		return -1;
	}
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) ||
                                  /* [한국어] RECURSIVE: 같은 thread의 nested lock 허용 (depth 카운터) */
#ifndef __FreeBSD__
	    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) ||
                                  /* [한국어] ROBUST: holder 사망 시 EOWNERDEAD 알림 — multi-process crash 안전 */
	    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) ||
                                  /* [한국어] PROCESS_SHARED: hugepage 공유 영역에 두면 다른 프로세스도 인식 */
#endif
	    pthread_mutex_init(mtx, &attr)) {
		rc = -1;
	}
	pthread_mutexattr_destroy(&attr);
                                  /* [한국어] attr는 임시 — init 후 즉시 destroy */
	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_construct - controller 객체 생성 + 초기 상태 설정
 *
 * @ctrlr: 트랜스포트가 이미 trid/opts/quirks 채워서 전달한 ctrlr 포인터
 * @return 0 성공, 음수 errno (mutex init 실패).
 *
 * 호출 컨텍스트: nvme_transport_ctrlr_construct 내부 (트랜스포트별 BAR 매핑 후) — 일반 init 흐름.
 *
 * 동작:
 *   [1] 초기 상태 설정:
 *       - PCIe: INIT_DELAY (quirky device 대응)
 *       - Fabrics: INIT (즉시 시작 — 트랜스포트 connect 자체가 이미 들어옴)
 *   [2] admin_queue_size 검증/정규화:
 *       - 스펙 max(4096) 초과 시 max로 클램프
 *       - 일부 device의 quirk: multiple 단위로 round-up
 *       - 스펙 min(2) 미만 시 min으로 클램프
 *   [3] 플래그 0 초기화 + free_io_qids NULL (process_init이 set_num_queues에서 채움)
 *   [4] active_io_qpairs/queued_aborts 빈 리스트로 초기화
 *   [5] ANA log page 미할당 마킹 (read 시 lazy alloc)
 *   [6] ★ ctrlr_lock = recursive + shared + robust mutex (multi-process crash-safe)
 *   [7] active_procs/register_operations/ns 빈 컨테이너 초기화
 */
int
nvme_ctrlr_construct(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	if (ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT_DELAY, NVME_TIMEOUT_INFINITE);
                                  /* [한국어] PCIe — quirky device 대응을 위한 INIT_DELAY 진입 (실제 delay는 quirk bit에 의해 결정) */
	} else {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT, NVME_TIMEOUT_INFINITE);
                                  /* [한국어] Fabrics — 별도 delay 없이 INIT 시작 (트랜스포트 connect가 이미 됨) */
	}

	if (ctrlr->opts.admin_queue_size > SPDK_NVME_ADMIN_QUEUE_MAX_ENTRIES) {
                                  /* [한국어] NVMe 스펙 admin queue 최대 항목 수 (4096) 초과 — 강제 클램프 */
		NVME_CTRLR_ERRLOG(ctrlr, "admin_queue_size %u exceeds max defined by NVMe spec, use max value\n",
				  ctrlr->opts.admin_queue_size);
		ctrlr->opts.admin_queue_size = SPDK_NVME_ADMIN_QUEUE_MAX_ENTRIES;
	}

	if (ctrlr->quirks & NVME_QUIRK_MINIMUM_ADMIN_QUEUE_SIZE &&
	    (ctrlr->opts.admin_queue_size % SPDK_NVME_ADMIN_QUEUE_QUIRK_ENTRIES_MULTIPLE) != 0) {
                                  /* [한국어] 일부 device 버그 — admin queue 크기가 N의 배수여야 함.
                                   *  배수 아니면 SPDK_ALIGN_CEIL로 다음 배수로 round-up. */
		NVME_CTRLR_ERRLOG(ctrlr,
				  "admin_queue_size %u is invalid for this NVMe device, adjust to next multiple\n",
				  ctrlr->opts.admin_queue_size);
		ctrlr->opts.admin_queue_size = SPDK_ALIGN_CEIL(ctrlr->opts.admin_queue_size,
					       SPDK_NVME_ADMIN_QUEUE_QUIRK_ENTRIES_MULTIPLE);
	}

	if (ctrlr->opts.admin_queue_size < SPDK_NVME_ADMIN_QUEUE_MIN_ENTRIES) {
                                  /* [한국어] 스펙 min (2) 미만 — 클램프 (admin은 SQ+CQ 1개 + ASYNC EVENT 1개 최소 필요) */
		NVME_CTRLR_ERRLOG(ctrlr,
				  "admin_queue_size %u is less than minimum defined by NVMe spec, use min value\n",
				  ctrlr->opts.admin_queue_size);
		ctrlr->opts.admin_queue_size = SPDK_NVME_ADMIN_QUEUE_MIN_ENTRIES;
	}

	ctrlr->flags = 0;
                                  /* [한국어] 플래그 0 — process_init이 진행하면서 WRR_SUPPORTED 등 채움 */
	ctrlr->free_io_qids = NULL;
                                  /* [한국어] IO qid bitmap 미할당 — set_num_queues 단계에서 spdk_bit_array_create로 alloc */
	ctrlr->is_resetting = false;
	ctrlr->is_failed = false;
	ctrlr->is_destructed = false;
                                  /* [한국어] 상태 플래그 3종 0 클리어 */

	TAILQ_INIT(&ctrlr->active_io_qpairs);
                                  /* [한국어] 빈 active IO qpair 리스트 — alloc_io_qpair 시 추가 */
	STAILQ_INIT(&ctrlr->queued_aborts);
	ctrlr->outstanding_aborts = 0;
                                  /* [한국어] abort 큐잉 메커니즘 초기화 (동시 발행 가능 abort 수 제한 있음) */

	ctrlr->ana_log_page = NULL;
	ctrlr->ana_log_page_size = 0;
                                  /* [한국어] ANA(Asymmetric Namespace Access) log — Fabrics multipath용. lazy alloc. */

	rc = nvme_robust_mutex_init_recursive_shared(&ctrlr->ctrlr_lock);
                                  /* [한국어] ★ 컨트롤러 단위 lock 초기화 — recursive + shared + robust */
	if (rc != 0) {
		return rc;
                                  /* [한국어] mutex init 실패 — 매우 드물지만 즉시 전파 */
	}

	TAILQ_INIT(&ctrlr->active_procs);
                                  /* [한국어] multi-process attach 추적 리스트 — process별 ref/cb 등 보관 */
	STAILQ_INIT(&ctrlr->register_operations);
                                  /* [한국어] async register R/W의 가짜 완료 큐 (multi-process 위해, nvme_qpair.c와 짝) */

	RB_INIT(&ctrlr->ns);
                                  /* [한국어] NS RB tree 빈 상태 초기화 — IDENTIFY_ACTIVE_NS에서 채워짐 */

	return rc;
}

static void
nvme_ctrlr_init_cap(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->cap.bits.ams & SPDK_NVME_CAP_AMS_WRR) {
		ctrlr->flags |= SPDK_NVME_CTRLR_WRR_SUPPORTED;
	}

	ctrlr->min_page_size = 1u << (12 + ctrlr->cap.bits.mpsmin);

	/* For now, always select page_size == min_page_size. */
	ctrlr->page_size = ctrlr->min_page_size;

	ctrlr->opts.io_queue_size = spdk_max(ctrlr->opts.io_queue_size, SPDK_NVME_IO_QUEUE_MIN_ENTRIES);
	ctrlr->opts.io_queue_size = spdk_min(ctrlr->opts.io_queue_size, MAX_IO_QUEUE_ENTRIES);
	if (ctrlr->quirks & NVME_QUIRK_MINIMUM_IO_QUEUE_SIZE &&
	    ctrlr->opts.io_queue_size == DEFAULT_IO_QUEUE_SIZE) {
		/* If the user specifically set an IO queue size different than the
		 * default, use that value.  Otherwise overwrite with the quirked value.
		 * This allows this quirk to be overridden when necessary.
		 * However, cap.mqes still needs to be respected.
		 */
		ctrlr->opts.io_queue_size = DEFAULT_IO_QUEUE_SIZE_FOR_QUIRK;
	}
	ctrlr->opts.io_queue_size = spdk_min(ctrlr->opts.io_queue_size, ctrlr->cap.bits.mqes + 1u);

	ctrlr->opts.io_queue_requests = spdk_max(ctrlr->opts.io_queue_requests, ctrlr->opts.io_queue_size);
}

/*
 * [한국어]
 * nvme_ctrlr_destruct_finish - 컨트롤러 destruct 마지막 단계 (mutex destroy + processes free)
 *
 * @ctrlr: 정리할 controller
 *
 * 호출 컨텍스트: nvme_ctrlr_destruct_poll_async가 shutdown 완료 + ns/qpair cleanup 후 마지막에 호출.
 *
 * 동작:
 *   [1] ctrlr_lock의 lock_depth 검증 — > 0이면 누군가 unlock 안 한 상태. 디버그 빌드 abort.
 *   [2] pthread_mutex_destroy로 robust mutex 자체 해제. 실패는 거의 없으나 assert.
 *   [3] nvme_ctrlr_free_processes로 active_procs 리스트의 모든 process 컨텍스트 free.
 *
 * 이후 ctrlr 구조체 자체는 트랜스포트 layer가 free (BAR unmap 등 함께).
 */
void
nvme_ctrlr_destruct_finish(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	if (ctrlr->lock_depth > 0) {
                                  /* [한국어] 누군가 lock 보유 중 destruct 진입 — 코드 버그 */
		NVME_CTRLR_ERRLOG(ctrlr, "lock currently held (depth=%d)!\n", ctrlr->lock_depth);
		assert(false);
	}

	rc = pthread_mutex_destroy(&ctrlr->ctrlr_lock);
                                  /* [한국어] 재귀 robust mutex 해제 — kernel futex 자원 반환 */
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "could not destroy ctrlr_lock: %s\n", spdk_strerror(rc));
		assert(false);
	}

	nvme_ctrlr_free_processes(ctrlr);
                                  /* [한국어] active_procs 리스트 정리 — 각 process별 컨텍스트(ref count, cb 등) 모두 free */
}

/*
 * [한국어]
 * nvme_ctrlr_destruct_async - 컨트롤러 비동기 destruct 시작
 *
 * @ctrlr: 정리 대상
 * @ctx:   destruct context (poll_async가 진행 추적)
 *
 * 호출 컨텍스트: nvme_ctrlr_detach_async (last-ref) 또는 nvme_ctrlr_poll_internal (init 실패).
 *
 * 동작 단계:
 *   [1] is_destructed=true 마킹 — 이후 새 attach 거부 (ctrlr_probe가 EBUSY 반환)
 *   [2] admin completion 한 번 폴링 — 이미 inflight 응답 흡수
 *   [3] queued aborts 모두 abort + admin AER abort (controller가 보낸 AER 응답 대기 취소)
 *   [4] 모든 active_io_qpairs free (사용자가 free 안 한 qpair도 강제 정리)
 *   [5] doorbell buffer + IOCS-specific 데이터 free
 *   [6] nvme_ctrlr_shutdown_async — controller shutdown 시퀀스 시작 (CC.SHN 비트 + CSTS.SHST 폴링)
 *
 * 후속: nvme_ctrlr_destruct_poll_async가 shutdown 완료까지 폴링.
 */
void
nvme_ctrlr_destruct_async(struct spdk_nvme_ctrlr *ctrlr,
			  struct nvme_ctrlr_detach_ctx *ctx)
{
	struct spdk_nvme_qpair *qpair, *tmp;

	NVME_CTRLR_DEBUGLOG(ctrlr, "Prepare to destruct SSD\n");

	ctrlr->prepare_for_reset = false;
                                  /* [한국어] reset 준비 플래그 클리어 — destruct는 reset 아님 */
	ctrlr->is_destructed = true;
                                  /* [한국어] ★ destruct 마킹 — ctrlr_probe가 이 플래그 검사 후 EBUSY로 새 attach 거부 */

	spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
                                  /* [한국어] 마지막 admin completion 흡수 — 진행 중 callback이 있으면 호출 */

	nvme_ctrlr_abort_queued_aborts(ctrlr);
                                  /* [한국어] 큐잉된 abort 명령 모두 cancel — 더 이상 처리 안 함 */
	nvme_transport_admin_qpair_abort_aers(ctrlr->adminq);
                                  /* [한국어] outstanding AER abort — controller가 보낼 AER 응답 더 안 받음 */

	TAILQ_FOREACH_SAFE(qpair, &ctrlr->active_io_qpairs, tailq, tmp) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
                                  /* [한국어] 사용자가 free 안 한 IO qpair 강제 정리 — leak 방지 */
	}

	nvme_ctrlr_free_doorbell_buffer(ctrlr);
                                  /* [한국어] shadow doorbell hugepage 반환 (NVMe 1.3+) */
	nvme_ctrlr_free_iocs_specific_data(ctrlr);
                                  /* [한국어] ZNS 등 IOCS-specific identify 데이터 반환 */

	nvme_ctrlr_shutdown_async(ctrlr, ctx);
                                  /* [한국어] controller shutdown 시작 — CC.SHN=01(Normal) 설정 + CSTS.SHST=10 도달까지 폴링.
                                   *  완료는 destruct_poll_async에서 검사. */
}

int
nvme_ctrlr_destruct_poll_async(struct spdk_nvme_ctrlr *ctrlr,
			       struct nvme_ctrlr_detach_ctx *ctx)
{
	struct spdk_nvme_ns *ns, *tmp_ns;
	int rc = 0;

	if (!ctx->shutdown_complete) {
		rc = nvme_ctrlr_shutdown_poll_async(ctrlr, ctx);
		if (rc == -EAGAIN) {
			return -EAGAIN;
		}
		/* Destruct ctrlr forcefully for any other error. */
	}

	if (ctx->cb_fn) {
		ctx->cb_fn(ctrlr);
	}

	nvme_transport_ctrlr_disconnect_qpair(ctrlr, ctrlr->adminq);

	RB_FOREACH_SAFE(ns, nvme_ns_tree, &ctrlr->ns, tmp_ns) {
		nvme_ctrlr_destruct_namespace(ctrlr, ns->id);
		RB_REMOVE(nvme_ns_tree, &ctrlr->ns, ns);
		spdk_free(ns);
	}

	ctrlr->active_ns_count = 0;

	spdk_bit_array_free(&ctrlr->free_io_qids);

	free(ctrlr->ana_log_page);
	free(ctrlr->copied_ana_desc);
	ctrlr->ana_log_page = NULL;
	ctrlr->copied_ana_desc = NULL;
	ctrlr->ana_log_page_size = 0;

	nvme_transport_ctrlr_destruct(ctrlr);

	return rc;
}

void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_ctrlr_detach_ctx ctx = { .ctrlr = ctrlr };
	int rc;

	nvme_ctrlr_destruct_async(ctrlr, &ctx);

	while (1) {
		rc = nvme_ctrlr_destruct_poll_async(ctrlr, &ctx);
		if (rc != -EAGAIN) {
			break;
		}
		nvme_delay(1000);
	}
}

int
nvme_ctrlr_submit_admin_request(struct spdk_nvme_ctrlr *ctrlr,
				struct nvme_request *req)
{
	return nvme_qpair_submit_request(ctrlr->adminq, req);
}

/*
 * [한국어]
 * nvme_keep_alive_completion - Keep Alive admin command 완료 콜백 (no-op)
 *
 * @cb_ctx: 사용 안 함 (NULL).
 * @cpl: completion. 검사 안 함.
 *
 * 동기/배경:
 *   Keep Alive 는 단순히 "host 가 살아있다" 신호이므로 완료 시 후속 동작 불필요. 단, request
 *   객체 자체는 SPDK request 처리 흐름에서 자동 free 되므로 콜백은 비워둬도 누수 없음.
 *
 *   Keep Alive 가 timeout 되면 컨트롤러가 자체적으로 fabric 연결을 종료시키며, host 측은
 *   admin polling 시 -ENXIO 또는 SCT/SC 에러로 감지 → reset 트리거.
 *
 * 실행 컨텍스트: admin completion polling 흐름.
 */
static void
nvme_keep_alive_completion(void *cb_ctx, const struct spdk_nvme_cpl *cpl)
{
	/* Do nothing */
	/* [한국어] keep-alive 완료 후 후속 동작 없음. SPDK 의 request 메모리 관리는 콜백 전후로 자동. */
}

/*
 * Check if we need to send a Keep Alive command.
 * Caller must hold ctrlr->ctrlr_lock.
 */
/*
 * [한국어]
 * nvme_ctrlr_keep_alive - Keep Alive admin command 주기적 발행 (interval 만료 시에만)
 *
 * @ctrlr: keep-alive 대상 controller. opts.keep_alive_timeout_ms > 0 으로 활성화된 상태.
 * @return: 0 = 발행 성공 또는 아직 interval 미만 (no-op)
 *          -ENXIO = admin queue 발행 실패 (qpair 끊김 등)
 *
 * 동기/배경:
 *   NVMe 스펙 §5.21.2 (1.x) / §5.27 (2.x) Keep Alive Command (opcode 0x18). NVMe-oF 에서
 *   필수이며, host 가 timeout 안에 keep-alive 를 발행하지 않으면 컨트롤러가 fabric 연결을
 *   종료한다. PCIe 에서는 보통 사용 안 함 (link 자체가 살아있는지 확인 가능).
 *
 *   본 함수는 admin polling 마다 호출되지만 next_keep_alive_tick 이전이면 즉시 return —
 *   실제 발행은 keep_alive_interval_ticks 주기 (보통 timeout 의 1/2) 마다.
 *
 * 동작 단계:
 *   1) 현재 tick 이 next_keep_alive_tick 미만이면 즉시 0 반환 (아직 발행 시점 아님).
 *   2) admin request 할당. 실패 시 0 반환 (다음 polling 에서 재시도).
 *   3) opcode = KEEP_ALIVE (0x18), CDW 모두 0.
 *   4) admin SQ 에 submit.
 *   5) next_keep_alive_tick 갱신 (다음 발행 시점).
 *
 * 실행 컨텍스트: process_admin_completions 진입부. ctrlr_lock 보유 (caller invariant).
 *
 * 호출 체인:
 *   process_admin_completions → [keep_alive] → nvme_ctrlr_submit_admin_request (SQ push)
 *
 * 에러 경로:
 *   · req 할당 실패: 0 반환 (silent skip — 다음 polling 에서 자동 재시도).
 *   · submit 실패: -ENXIO (caller 가 process_admin_completions 에서 -ENXIO 반환 → reset 트리거).
 */
static int
nvme_ctrlr_keep_alive(struct spdk_nvme_ctrlr *ctrlr)
{
	uint64_t now;                           /* [한국어] 현재 TSC tick — interval 비교용. */
	struct nvme_request *req;               /* [한국어] keep-alive admin request. */
	struct spdk_nvme_cmd *cmd;              /* [한국어] req->cmd 의 alias — opcode 설정용. */
	int rc = 0;

	now = spdk_get_ticks();                 /* [한국어] 현재 TSC 값 read — 비싼 시스템콜 아닌 lfence+rdtsc. */
	if (now < ctrlr->next_keep_alive_tick) {
		return rc;                      /* [한국어] interval 미달 — 다음 polling 에서 다시 시도. */
	}

	req = nvme_allocate_request_null(ctrlr->adminq, nvme_keep_alive_completion, NULL);
	                                        /* [한국어] payload 없는 admin request. cb=no-op, cb_arg=NULL. */
	if (req == NULL) {
		return rc;                      /* [한국어] mempool 고갈 — silent skip. 다음 polling 에서 재시도. */
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KEEP_ALIVE;    /* [한국어] opcode 0x18 — admin Keep Alive command. */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	                                        /* [한국어] admin SQ 에 SQE push + doorbell write. */
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Submitting Keep Alive failed\n");
		rc = -ENXIO;                    /* [한국어] -ENXIO = qpair 끊김 신호. caller 가 reset 결정. */
	}

	ctrlr->next_keep_alive_tick = now + ctrlr->keep_alive_interval_ticks;
	                                        /* [한국어] 다음 발행 시점 = 지금 + interval. interval 은 보통 timeout/2. */
	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_is_nssr_supported - 이 controller 에서 Subsystem Reset (NSSR) 가능 여부 (PCIe + CAP.NSSRS=1)
 *
 * @ctrlr: 검사 대상.
 * @return: true = NSSR 가능 / false = 미지원 또는 fabric
 *
 * 동기/배경:
 *   NSSR 은 NSSR 레지스터에 매직 값 write 로 발동되는데, SPDK 는 이를 동기 MMIO 로 처리한다.
 *   fabric (RDMA/TCP) 에서는 register access 가 원격 RPC 형태이므로 deadlock 위험이 있어
 *   PCIe transport 로 한정. 또한 컨트롤러가 CAP.NSSRS=1 로 NSSR 지원을 advertise 해야 함.
 *
 * 실행 컨텍스트: 사용자 thread. lock 불필요 (cap, trid 는 attach 후 immutable).
 */
bool
spdk_nvme_ctrlr_is_nssr_supported(struct spdk_nvme_ctrlr *ctrlr)
{
	/* NSSR is done via write to the NVMe register.
	 * SPDK is handling it synchronously, so in nvmf connected to another nvmf
	 * it might cause delays and possible deadlocks.
	 * Limit NSSR to be done only for PCIe transport.
	 */
	/* [한국어] 두 조건 AND:
	 *   · CAP.NSSRS=1 — 컨트롤러가 NSSR 지원 (스펙 §3.1.4 CAP.NSSRS).
	 *   · trtype==PCIe — 동기 MMIO 가 안전한 transport 만 허용. */
	return ctrlr->cap.bits.nssrs == 1 && ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_process_admin_completions - admin queue 완료 처리 (★ poller 메인 루프 진입점)
 *
 * @ctrlr: 처리 대상 controller. 호출 thread 는 admin polling 권한을 가져야 함.
 * @return: ≥0 = 처리한 completion 수 (admin + io_msg + AER)
 *          음수 = 에러 (-ENXIO = qpair 끊김 → reset/disconnect_done 트리거)
 *
 * 동기/배경:
 *   ★ NVMe admin queue 의 main poller — 사용자(보통 bdev_nvme reset poller, 또는 SPDK reactor
 *   에 등록된 poller) 가 주기적으로 호출하여 admin queue 의 모든 작업을 처리한다.
 *
 *   처리하는 일 4가지 (한 호출에서 모두):
 *     1) Keep Alive 발행 (interval 만료 시).
 *     2) IO message channel 처리 — 외부 thread 가 보낸 admin 요청 발행 (예: bdev_nvme attach_ns).
 *     3) admin CQ polling — outstanding admin command 완료 처리. 콜백 호출 (identify_done,
 *        configure_aer_done, async_event_cb 등 모든 admin 완료가 여기로).
 *     4) AER 큐 비우기 — 이번 polling 사이클에서 적재된 AER wrapper 처리.
 *
 *   reset 시퀀스에서의 특수 처리:
 *     rc==-ENXIO && is_disconnecting → nvme_ctrlr_disconnect_done() 호출. 이는 disconnect
 *     state machine 이 admin qpair 가 완전히 끊김을 인지하는 신호.
 *
 * 동작 단계:
 *   1) ctrlr_lock 획득.
 *   2) keep_alive_interval_ticks 가 set 되어 있으면 keep-alive 발행 시도.
 *   3) io_msg_process — 외부 thread 가 큐잉한 admin 요청들을 admin SQ 로 발행. 처리 수 누적.
 *   4) qpair_process_completions(adminq) — admin CQ 폴링. 각 completion 의 콜백 호출.
 *   5) AER 큐 비우기 (per-process).
 *   6) -ENXIO 이고 disconnect 진행 중이면 disconnect_done() 호출.
 *   7) lock 해제.
 *   8) 처리 수 합산 후 반환.
 *
 * 실행 컨텍스트: 사용자 thread (보통 reactor poller). 한 호출에서 처리 시간이 N us 수준.
 *               polled-mode — interrupt 미사용. busy-wait 형태로 CPU 점유.
 *
 * 호출 체인:
 *   reactor poller / spdk_nvme_ctrlr_reset → [process_admin_completions]
 *     → keep_alive (필요 시) → io_msg_process → qpair_process_completions(adminq)
 *     → 각 admin completion 콜백 → complete_queued_async_events → (조건부) disconnect_done
 *
 * 에러 경로:
 *   · keep-alive submit 실패: -ENXIO 반환 (lock 해제 후).
 *   · io_msg_process 음수: 그 코드 반환.
 *   · qpair_process_completions -ENXIO + is_disconnecting: disconnect_done() 호출 후 -ENXIO 반환.
 */
int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	int32_t num_completions;                /* [한국어] 처리된 completion 누적 수 (반환값). */
	int32_t rc;                             /* [한국어] 각 단계 결과. 음수=에러, 양수=처리 수. */
	struct spdk_nvme_ctrlr_process	*active_proc;
	                                        /* [한국어] 현재 process 의 ctrlr_process — AER 큐 처리 시 사용. */

	nvme_ctrlr_lock(ctrlr);                 /* [한국어] admin queue 동시 진입 차단 — 다른 thread 의 reset/admin 발행과 직렬화. */

	/* [한국어] 단계 1: Keep Alive (NVMe-oF 만 의미 있음). interval_ticks==0 이면 비활성. */
	if (ctrlr->keep_alive_interval_ticks) {
		rc = nvme_ctrlr_keep_alive(ctrlr);
		if (rc) {
			nvme_ctrlr_unlock(ctrlr);
			return rc;              /* [한국어] keep-alive submit 실패 — 즉시 반환 (caller 는 reset 결정). */
		}
	}

	/* [한국어] 단계 2: 외부 thread 가 보낸 admin 요청 발행 (bdev_nvme attach_ns 등). */
	rc = nvme_io_msg_process(ctrlr);
	if (rc < 0) {
		nvme_ctrlr_unlock(ctrlr);
		return rc;                      /* [한국어] io_msg 처리 중 치명적 오류. */
	}
	num_completions = rc;                   /* [한국어] io_msg 처리 수를 누적 시작. */

	/* [한국어] 단계 3: ★ admin CQ polling — 모든 완료 콜백 (identify_done, async_event_cb 등) 호출. */
	rc = spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	                                        /* [한국어] 0 = unlimited (모든 outstanding completion 처리). */

	/* Each process has an async list, complete the ones for this process object */
	/* [한국어] 단계 4: 이번 사이클에서 적재된 AER 들을 사용자 콜백까지 전달. */
	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		nvme_ctrlr_complete_queued_async_events(ctrlr);
	}

	/* [한국어] disconnect 진행 중에 admin qpair 가 끊겼다 (-ENXIO) 면 disconnect_done 트리거 — state machine 한 단계 진행. */
	if (rc == -ENXIO && ctrlr->is_disconnecting) {
		nvme_ctrlr_disconnect_done(ctrlr);
	}

	nvme_ctrlr_unlock(ctrlr);

	/* [한국어] 최종 반환값 결정:
	 *   · qpair polling 음수면 그 코드를 반환.
	 *   · 양수면 io_msg 처리 수와 합산. */
	if (rc < 0) {
		num_completions = rc;
	} else {
		num_completions += rc;
	}

	return num_completions;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_data - Identify Controller 응답 데이터(4KB raw) 포인터 반환 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return ctrlr->cdata 의 const 포인터 — 4KB struct spdk_nvme_ctrlr_data (Identify Ctrlr 응답 raw).
 *
 * NVMe Spec §5.15.2.2 Identify Controller 응답 구조 그대로:
 *   - vid, ssvid: PCIe vendor/subsystem vendor ID
 *   - mn, sn, fr: model name, serial number, firmware revision
 *   - mdts: maximum data transfer size
 *   - cntlid, ver: controller ID, NVMe version
 *   - oacs, oncs, fuses: 지원 admin/IO/fused 명령 비트맵
 *   - nn: number of namespaces (최대 NSID)
 *   - vwc, awun, awupf: write cache, atomic write unit
 *   - nvme-oF 추가 필드: ioccsz, iorcsz, icdoff, ctratt 등
 *
 * 사용처: bdev_nvme 가 SSD 모델/serial/펌웨어 표시, MDTS 기반 split 계산, 능력 비트 검사.
 *
 * 동기화: read-only, cdata 는 controller init 후 변경 없음 — 잠금 불필요.
 */
const struct spdk_nvme_ctrlr_data *
spdk_nvme_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr)
{
	return &ctrlr->cdata;         /* [한국어] 4KB raw Identify Controller 응답 — read-only. */
}

union spdk_nvme_csts_register spdk_nvme_ctrlr_get_regs_csts(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_csts_register csts;

	if (nvme_ctrlr_get_csts(ctrlr, &csts)) {
		csts.raw = SPDK_NVME_INVALID_REGISTER_VALUE;
	}
	return csts;
}

union spdk_nvme_cc_register spdk_nvme_ctrlr_get_regs_cc(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register cc;

	if (nvme_ctrlr_get_cc(ctrlr, &cc)) {
		cc.raw = SPDK_NVME_INVALID_REGISTER_VALUE;
	}
	return cc;
}

union spdk_nvme_cap_register spdk_nvme_ctrlr_get_regs_cap(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cap;
}

union spdk_nvme_vs_register spdk_nvme_ctrlr_get_regs_vs(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->vs;
}

union spdk_nvme_cmbsz_register spdk_nvme_ctrlr_get_regs_cmbsz(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cmbsz_register cmbsz;

	if (nvme_ctrlr_get_cmbsz(ctrlr, &cmbsz)) {
		cmbsz.raw = 0;
	}

	return cmbsz;
}

union spdk_nvme_pmrcap_register spdk_nvme_ctrlr_get_regs_pmrcap(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_pmrcap_register pmrcap;

	if (nvme_ctrlr_get_pmrcap(ctrlr, &pmrcap)) {
		pmrcap.raw = 0;
	}

	return pmrcap;
}

union spdk_nvme_bpinfo_register spdk_nvme_ctrlr_get_regs_bpinfo(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_bpinfo_register bpinfo;

	if (nvme_ctrlr_get_bpinfo(ctrlr, &bpinfo)) {
		bpinfo.raw = 0;
	}

	return bpinfo;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_pmrsz - PMR(Persistent Memory Region) 크기(byte) 반환 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return ctrlr->pmr_size — PMRCAP.PMRSZ 기반. PMR 미지원 / 미활성 컨트롤러는 0.
 *
 * PMR: NVMe 1.4+ — 컨트롤러가 BAR 영역에 노출하는 영구 메모리(non-volatile DIMM 등 backend).
 * 호스트가 memcpy 로 직접 R/W 가능 (PCIe 64-bit BAR), 컨트롤러 reset 후에도 데이터 유지.
 * 사용자가 PMR 영역을 사용할지 결정하기 위한 capability 조회.
 */
uint64_t
spdk_nvme_ctrlr_get_pmrsz(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->pmr_size;       /* [한국어] PMR 크기 직접 반환 — 미지원이면 0. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_num_ns - Identify Controller의 NN(Number of Namespaces) 필드 반환 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return ctrlr->cdata.nn — 컨트롤러가 노출하는 최대 NSID (1-based 최대치).
 *
 * 주의: NN 은 *최대 NSID 값* 으로 실제 활성 NS 개수와 다를 수 있음 — NSID 가 sparse 하게 할당
 * 가능 (예: NN=10 이지만 NSID 1, 3, 7 만 active). 활성 NS 순회는 get_first/next_active_ns 사용.
 */
uint32_t
spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cdata.nn;       /* [한국어] Identify Controller의 NN 필드 — 최대 NSID 값. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_is_active_ns - 특정 NSID 가 현재 active 상태인지 판정 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @nsid: 검사할 NSID (1-based)
 * @return true = active, false = 비활성 or 미존재
 *
 * Active 의 의미: Identify Active Namespace List (CNS=0x02) 에 포함되어 있고 NS 객체가
 * 정상 construct 되어 ns->active=true 인 상태. NS Attribute Notice AER 후 동적으로 변경 가능.
 *
 * 사용처: 사용자가 NSID 입력 후 "유효한가?" 확인. bdev_nvme 가 attach 시 NS 정렬.
 *
 * 동작: ns RB tree 에서 NSID 검색 → ns->active 비트 반환. 미발견은 false (한 번도 alloc 안 된 NSID).
 */
bool
spdk_nvme_ctrlr_is_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns tmp, *ns;  /* [한국어] RB_FIND key 객체 + 검색 결과 */

	tmp.id = nsid;                /* [한국어] comparator 가 id 만 보므로 다른 필드 불필요 */
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);
	                                  /* [한국어] O(log n) 검색 — NSID 기반 정렬된 RB tree. */

	if (ns != NULL) {
		return ns->active;    /* [한국어] NS 객체 존재 — active 비트 반환. */
	}

	return false;                 /* [한국어] NS 객체 없음 = 한 번도 alloc 안 됨 (lazy alloc 미발생) → 비활성으로 간주. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_first_active_ns - 가장 작은 NSID 의 active NS 반환 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return active NS 의 NSID (1-based), 활성 NS 없으면 0
 *
 * 사용자 순회 패턴:
 *   nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
 *   while (nsid != 0) {
 *     // ... use nsid ...
 *     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid);
 *   }
 *
 * RB tree 의 in-order traversal 이라 NSID 오름차순으로 방문 — sparse NSID 환경에서도 안전.
 *
 * 동작: RB_MIN 으로 최소 NSID 노드 시작 → ns->active==true 까지 RB_NEXT 진행.
 */
uint32_t
spdk_nvme_ctrlr_get_first_active_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ns *ns;      /* [한국어] 순회 중 현재 위치 */

	ns = RB_MIN(nvme_ns_tree, &ctrlr->ns);
	                                  /* [한국어] RB tree 좌측 최소 — 가장 작은 NSID 의 NS. */
	if (ns == NULL) {
		return 0;             /* [한국어] NS 가 하나도 alloc 되지 않은 컨트롤러. */
	}

	while (ns != NULL) {
		if (ns->active) {
			return ns->id;
			                                  /* [한국어] 첫 active NS 발견 — NSID 반환. */
		}

		ns = RB_NEXT(nvme_ns_tree, &ctrlr->ns, ns);
		                                  /* [한국어] in-order 다음 노드 — NSID 오름차순. */
	}

	return 0;                     /* [한국어] 끝까지 active 발견 못 함 — 0 (= end-of-list). */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_next_active_ns - prev_nsid 다음의 active NS 반환 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @prev_nsid: 이전에 반환받은 NSID (반드시 RB tree 에 존재해야 함)
 * @return prev_nsid 다음의 active NSID, 더 없으면 0
 *
 * get_first_active_ns 와 짝을 이루는 순회 helper. prev_nsid 노드 자체는 skip 하고 그 다음부터
 * active 검색. 사용자가 NSID 를 직접 +1 하지 않는 이유: NSID 가 sparse 일 수 있어 RB tree 의
 * 실제 next 노드를 따라가야 정확함.
 */
uint32_t
spdk_nvme_ctrlr_get_next_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t prev_nsid)
{
	struct spdk_nvme_ns tmp, *ns; /* [한국어] key 객체 + 순회 변수 */

	tmp.id = prev_nsid;           /* [한국어] 현재 위치 검색 key */
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);
	if (ns == NULL) {
		return 0;             /* [한국어] prev_nsid 가 tree 에 없음 — 잘못된 호출 시퀀스. */
	}

	ns = RB_NEXT(nvme_ns_tree, &ctrlr->ns, ns);
	                                  /* [한국어] 한 칸 전진 — prev_nsid 자체는 skip. */
	while (ns != NULL) {
		if (ns->active) {
			return ns->id;
		}

		ns = RB_NEXT(nvme_ns_tree, &ctrlr->ns, ns);
	}

	return 0;                     /* [한국어] 더 이상 active NS 없음 — 0 (= end). */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_ns - NSID로 NS 객체 조회 (Lazy allocation 패턴 핵심)
 *
 * @ctrlr: 대상 controller
 * @nsid: 조회할 NSID (1-based, 1..ctrlr->cdata.nn 범위 내)
 * @return: NS 포인터 또는 NULL (NSID 범위 밖 또는 OOM)
 *
 * 이 함수는 SPDK NVMe 객체 모델의 *가장 자주 호출되는 진입점* 중 하나 — 사용자가 NSID로 NS 객체를
 * 얻을 때 항상 이 함수를 통과한다.
 *
 * Lazy allocation 패턴:
 *   - 첫 호출 시: NS 객체가 RB tree에 없으면 zmalloc + RB_INSERT (lazy create)
 *   - 두 번째 이후: RB tree에서 즉시 발견 (O(log n))
 *   - 비활성 NSID도 일단 객체는 생성됨 (ns->active=false 인 채로 보관 — 추후 활성화 가능성 대비)
 *
 * 동기화: ctrlr_lock으로 RB tree 조작 + lazy alloc 보호 (multi-thread 안전).
 *
 * 동작 단계:
 *   1) NSID 범위 검사 — 1 <= nsid <= ctrlr->cdata.nn (Identify Controller의 NN 필드)
 *   2) ctrlr_lock 획득
 *   3) RB_FIND로 검색 (key는 tmp.id = nsid)
 *   4) 없으면:
 *      - hugepage 64B aligned NS 객체 alloc (DMA 호환 + multi-process 공유 가능)
 *      - id, ctrlr 역참조 설정
 *      - RB_INSERT
 *   5) lock 해제
 *
 * 주의: 반환된 NS 객체가 *active*임을 보장하지 않음. 사용자는 spdk_nvme_ns_is_active()로 확인 필요.
 * 또 nvme_ns_construct가 호출되어 nsdata가 채워졌는지도 별개 — bring-up 단계에서 자동 호출됨.
 */
struct spdk_nvme_ns *
spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	/* [한국어] RB_FIND key 객체 — id 필드만 사용. 스택 할당이라 함수 종료 시 자동 해제. */
	struct spdk_nvme_ns tmp;
	/* [한국어] 결과 NS — RB tree에서 발견 또는 새로 alloc. */
	struct spdk_nvme_ns *ns;

	/* [한국어] NSID 범위 검사. NN(Number of Namespaces, Identify Controller cdata.nn)이 최대 NSID.
	 *  · NSID 0 = broadcast/invalid, NN 초과 = controller가 노출하지 않는 영역.
	 *  · cdata.nn은 0이 가능 (NS가 하나도 없는 controller). */
	if (nsid < 1 || nsid > ctrlr->cdata.nn) {
		return NULL;
	}

	/* [한국어] ctrlr_lock 획득 — RB tree 조작과 lazy alloc의 atomicity 확보. */
	nvme_ctrlr_lock(ctrlr);

	/* [한국어] RB_FIND key — comparator(nvme_ns_cmp)는 id 필드만 본다. */
	tmp.id = nsid;
	/* [한국어] RB tree 검색 — O(log n). 이미 있으면 그 객체 포인터 반환. */
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);

	/* [한국어] 미발견 — lazy alloc 진행. */
	if (ns == NULL) {
		/* [한국어] DMA 호환 NS 객체 할당:
		 *   · sizeof(spdk_nvme_ns): nsdata 4KB + id_desc_list 4KB + 메타 등 약 9KB
		 *   · align 64: cacheline 정렬 (false sharing 회피, DMA 정렬)
		 *   · NUMA ANY: 어느 NUMA 노드든 OK
		 *   · MALLOC_SHARE: hugepage shared (multi-process secondary도 같은 가상 주소로 접근 가능). */
		ns = spdk_zmalloc(sizeof(struct spdk_nvme_ns), 64, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
		/* [한국어] hugepage 부족 — lock 해제 후 NULL 반환. */
		if (ns == NULL) {
			nvme_ctrlr_unlock(ctrlr);
			return NULL;
		}

		/* [한국어] DEBUG 로그 — 새 NS가 RB tree에 추가됨을 기록. */
		NVME_CTRLR_DEBUGLOG(ctrlr, "Namespace %u was added\n", nsid);
		/* [한국어] NSID 저장 — RB tree key로 사용. */
		ns->id = nsid;
		/* [한국어] ctrlr 역참조 — IO 발행 시 transport / max_xfer 등 참조 경로. */
		ns->ctrlr = ctrlr;
		/* [한국어] RB tree 삽입 — O(log n). 같은 NSID 중복은 발생 불가 (caller가 이미 RB_FIND로 확인). */
		RB_INSERT(nvme_ns_tree, &ctrlr->ns, ns);
	}

	/* [한국어] lock 해제. */
	nvme_ctrlr_unlock(ctrlr);

	/* [한국어] NS 객체 (기존 또는 신규) 반환. caller는 이 객체로 추가 작업(IO, getter 등) 진행. */
	return ns;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_pci_device - PCIe 컨트롤러의 DPDK PCI device 핸들 반환 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return spdk_pci_device 포인터, PCIe 가 아니거나 NULL ctrlr 면 NULL
 *
 * SPDK 가 DPDK rte_pci_device 를 spdk_pci_device 로 래핑한 객체. 사용자가 PCIe BAR 직접 매핑,
 * MSI-X 설정 등 lower-level 접근이 필요할 때 사용 (희소 사용처). 일반 사용자는 호출 불요.
 *
 * NVMe-oF 컨트롤러(RDMA/TCP/FC) 는 PCI 디바이스가 없으므로 NULL 반환.
 */
struct spdk_pci_device *
spdk_nvme_ctrlr_get_pci_device(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr == NULL) {
		return NULL;          /* [한국어] NULL 인자 방어 — 일부 사용자가 ctrlr=NULL 로 호출. */
	}

	if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		return NULL;          /* [한국어] Fabrics 트랜스포트는 PCI 디바이스 없음. */
	}

	return nvme_ctrlr_proc_get_devhandle(ctrlr);
	                                  /* [한국어] multi-process: 현재 프로세스의 ctrlr_process->devhandle 반환. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_numa_id - 컨트롤러가 attach 된 NUMA 노드 ID 반환 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return NUMA node id (0~N-1) 또는 SPDK_ENV_NUMA_ID_ANY (불명)
 *
 * NUMA-aware 스케줄링에 사용: SPDK 사용자가 컨트롤러 가까운 코어에 reactor 를 배치해
 * cross-socket memory 접근을 회피하고 IO 지연을 최소화한다.
 *
 * id_valid 비트로 unset 상태 구분 — NUMA 노드 0 도 유효 값이므로 단순히 0 = "미설정" 으로
 * 해석하면 안 됨.
 *
 * PCIe: /sys/bus/pci/devices/<bdf>/numa_node 에서 결정.
 * NVMe-oF: 일반적으로 ID_ANY (네트워크 트랜스포트는 NUMA 친화도 의미가 다름).
 */
int32_t
spdk_nvme_ctrlr_get_numa_id(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->numa.id_valid) {
		return ctrlr->numa.id;
		                                  /* [한국어] valid 플래그 set 시에만 실제 ID 반환. */
	} else {
		return SPDK_ENV_NUMA_ID_ANY;
		                                  /* [한국어] 미설정 sentinel — caller 는 어느 NUMA 든 OK 로 해석. */
	}
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_id - 컨트롤러 식별자(CNTLID, 16-bit) 반환 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return cdata.cntlid — NVMe-oF subsystem 내 controller 식별자. PCIe 에서는 일반적으로 0.
 *
 * NVMe-oF: 하나의 subsystem 에 여러 controller (각각 다른 host 의 view) 가 있을 수 있어
 * cntlid 로 구분. discovery log page 등에 노출됨.
 */
uint16_t
spdk_nvme_ctrlr_get_id(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cntlid;         /* [한국어] Identify Controller 응답의 CNTLID 필드 (NVMe spec §5.15.2.2). */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_max_xfer_size - 단일 NVMe 명령으로 전송 가능한 최대 바이트 수 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return ctrlr->max_xfer_size — MDTS(Maximum Data Transfer Size, Identify Controller) 기반.
 *
 * bdev_nvme 가 split 결정에 사용. MDTS=0 (제한 없음) 컨트롤러도 SPDK 내부 PRP/SGL 자료구조
 * 한도 안에서 최대 크기가 결정됨. max_xfer_size 초과 사용자 IO 는 SPDK 가 자동 split.
 *
 * 계산:
 *   - MDTS != 0: 2^MDTS × CAP.MPSMIN (페이지 크기)
 *   - MDTS == 0: SPDK 내부 한도 (보통 1MB ~ 수 MB)
 *   - Fabrics: transport-specific 추가 한도 적용 (RDMA max_inline_data 등)
 */
uint32_t
spdk_nvme_ctrlr_get_max_xfer_size(const struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->max_xfer_size;  /* [한국어] nvme_ctrlr_init_cap / transport_set_xfer 에서 계산된 캐시값. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_max_sges - 단일 명령에서 사용 가능한 최대 SGE(Scatter-Gather Element) 수 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return SGL 지원: ctrlr->max_sges (트랜스포트 한도). 미지원: UINT16_MAX (즉 사실상 무한).
 *
 * bdev_nvme 가 사용자 IO 의 SGL 길이가 이 한도를 넘으면 split 또는 bounce buffer 로 처리.
 *
 * UINT16_MAX 반환 이유: SGL 미지원 컨트롤러는 PRP 만 사용 — PRP 의 페이지 한도가 별도로 적용되므로
 * SGE 한도 자체는 무관. 사용자가 "PRP 모드면 무한"으로 해석하도록 sentinel 제공.
 */
uint16_t
spdk_nvme_ctrlr_get_max_sges(const struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->flags & SPDK_NVME_CTRLR_SGL_SUPPORTED) {
	                                  /* [한국어] SGL_SUPPORTED 플래그: Identify Controller 의 SGLS 필드 기반. */
		return ctrlr->max_sges;
		                                  /* [한국어] 트랜스포트별 SGE 한도 (PCIe: 일반적으로 큰 값, RDMA: ibv 한도). */
	} else {
		return UINT16_MAX;    /* [한국어] SGL 미지원 → SGE 한도 무관 sentinel. */
	}
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_register_aer_callback - 사용자 AER 콜백 등록 (per-process)
 *
 * @ctrlr: 등록 대상 controller. 이미 attach 된 상태여야 함.
 * @aer_cb_fn: AER 발생 시 호출될 사용자 콜백. 시그니처: void cb(void *ctx, const struct spdk_nvme_cpl *).
 *             cpl 의 cdw0 안에 event_type/info/log_page_id 인코딩되어 있음.
 * @aer_cb_arg: 콜백 첫 인자로 전달될 사용자 컨텍스트.
 *
 * 동기/배경:
 *   AER 메커니즘은 SPDK 내부에서 N개 슬롯으로 자동 발행/재발행되며 (configure_aer + async_event_cb),
 *   각 이벤트는 nvme_ctrlr_process_async_event() 가 디코드한다. 그 처리의 마지막 단계
 *   (nvme_ctrlr_process_async_event_finish) 에서 본 함수가 등록한 사용자 콜백이 호출된다.
 *
 *   콜백은 process 별로 분리되어 있으므로 (active_proc->aer_cb_fn) primary 와 secondary 가 각자
 *   다른 콜백을 등록할 수 있다. multi-process 환경에서 같은 ctrlr 를 공유해도 process 별로
 *   알림 처리가 격리됨.
 *
 *   사용자(보통 bdev_nvme) 가 이 콜백에서 처리하는 일:
 *     · NS 변경 알림 → bdev 재스캔 트리거.
 *     · firmware activation → reset 스케줄링.
 *     · SMART critical → 사용자 모니터링 시스템에 보고.
 *
 * 동작 단계:
 *   1) lock 획득.
 *   2) getpid() 로 현재 프로세스의 ctrlr_process entry 검색.
 *   3) 있으면 aer_cb_fn / aer_cb_arg 갱신.
 *   4) lock 해제.
 *
 * 실행 컨텍스트: 사용자 thread. 보통 attach 직후 또는 RPC 핸들러에서 호출.
 *               콜백 자체는 admin polling thread (process_admin_completions) 에서 호출됨.
 *
 * 호출 체인:
 *   user/bdev_nvme → [register_aer_callback] (등록만)
 *   (이후 AER 발생 시) async_event_cb → queue → process_async_event_finish → user aer_cb_fn
 */
void
spdk_nvme_ctrlr_register_aer_callback(struct spdk_nvme_ctrlr *ctrlr,
				      spdk_nvme_aer_cb aer_cb_fn,
				      void *aer_cb_arg)
{
	struct spdk_nvme_ctrlr_process *active_proc;
	                                        /* [한국어] 현재 프로세스의 ctrlr_process 엔트리 — per-process 콜백 보관소. */

	nvme_ctrlr_lock(ctrlr);                 /* [한국어] active_procs 리스트 안전 순회용 lock. */

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	                                        /* [한국어] getpid() 로 현재 프로세스 엔트리 검색. */
	if (active_proc) {
		active_proc->aer_cb_fn = aer_cb_fn;
		                                /* [한국어] 콜백 함수 포인터 저장. NULL 가능 — 등록 해제 의미. */
		active_proc->aer_cb_arg = aer_cb_arg;
		                                /* [한국어] 콜백에 전달될 컨텍스트 — bdev_nvme 객체 ptr 등. */
	}

	nvme_ctrlr_unlock(ctrlr);
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_disable_read_changed_ns_list_log_page - Changed NS List log 자동 read 비활성화
 *
 * @ctrlr: 옵션 변경 대상 controller.
 *
 * 동기/배경:
 *   기본 동작에서 SPDK 는 NS_ATTR_CHANGED AER 발생 시 Changed NS List log page (LID=0x04) 를
 *   자동으로 read-and-clear 하여 변경된 NSID 목록을 얻는다. 일부 사용자(특히 NVMe-oF target
 *   middleware) 는 자체 경로로 NS 변경을 추적하므로 이 자동 동작을 비활성화하고 싶을 수 있다.
 *
 *   이 함수가 호출되면 nvme_ctrlr_clear_changed_ns_log() 가 즉시 0 반환하여 noop 으로 동작.
 *   대신 nvme_ctrlr_update_namespaces() 는 changed_ns_list==NULL 경로(전체 NS 재스캔) 로 진입.
 *
 *   주의: 이 함수는 lock 없이 단일 bool 쓰기. plain store 이며 다음 AER 처리 시 read.
 *
 * 실행 컨텍스트: 사용자 thread. attach 직후 1회 호출이 일반적.
 *
 * 호출 체인:
 *   user → [disable_read_changed_ns_list_log_page] → opts.disable_read_changed_ns_list_log_page = true
 *   (이후 AER 처리 시) clear_changed_ns_log → 노옵 0 반환
 */
void
spdk_nvme_ctrlr_disable_read_changed_ns_list_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	ctrlr->opts.disable_read_changed_ns_list_log_page = true;
	                                        /* [한국어] opts 구조체의 bool 플래그 set. lock 없이 단일 store —
	                                         *         AER 처리 thread 가 read 하기 전에만 set 되면 OK (eventual visibility). */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_register_timeout_callback - IO/admin command timeout 콜백 등록 (per-process)
 *
 * @ctrlr: 등록 대상 controller.
 * @timeout_io_us: IO command timeout (마이크로초). 0=비활성.
 * @timeout_admin_us: admin command timeout (마이크로초). 0=비활성.
 * @cb_fn: timeout 발생 시 호출될 콜백. 시그니처: void cb(void *cb_arg, struct spdk_nvme_ctrlr *,
 *         struct spdk_nvme_qpair *, uint16_t cid).
 * @cb_arg: 콜백에 전달될 컨텍스트.
 *
 * 동기/배경:
 *   SPDK 는 폴링 모드에서 oustanding command 의 발행 시각(tick) 을 기록하고, qpair 폴링 시
 *   매 명령마다 (now - submit_tick) 가 timeout 을 초과하는지 검사한다. 초과하면 본 함수로
 *   등록된 콜백이 호출되며, caller(보통 bdev_nvme) 는 reset/abort 결정을 내린다.
 *
 *   us → ticks 변환: spdk_get_ticks_hz() (TSC 주파수) × us / 1e6.
 *
 *   keep-alive 와의 정합성:
 *     keep_alive_timeout 이 io/admin timeout 보다 크면 "keep-alive 가 끊긴 후에야 IO timeout"
 *     이 감지되어 reset 지연 발생 가능 → 경고 로그.
 *
 * 동작 단계:
 *   1) lock 획득.
 *   2) keep-alive vs timeout 비교 → 부정합 시 경고.
 *   3) us → ticks 변환 후 active_proc 에 저장.
 *   4) ctrlr->timeout_enabled = true (전역 플래그) — qpair 폴링이 이 플래그 보고 timeout 검사.
 *   5) lock 해제.
 *
 * 실행 컨텍스트: 사용자 thread. 콜백은 qpair_process_completions 에서 호출됨.
 *
 * 호출 체인:
 *   bdev_nvme attach → [register_timeout_callback] (등록만)
 *   (이후 IO/admin 발행 시) submit_tick 기록 → polling 시 (now-submit_tick > timeout_ticks) →
 *     timeout_cb_fn(cb_arg, ctrlr, qpair, cid)
 */
void
spdk_nvme_ctrlr_register_timeout_callback(struct spdk_nvme_ctrlr *ctrlr,
		uint64_t timeout_io_us, uint64_t timeout_admin_us,
		spdk_nvme_timeout_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_ctrlr_process	*active_proc;
	                                        /* [한국어] per-process timeout 콜백 보관소. */

	nvme_ctrlr_lock(ctrlr);                 /* [한국어] active_procs / timeout_enabled 보호. */

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		/* [한국어] keep-alive interval 이 IO/admin timeout 보다 크면 정합성 경고.
		 *         이 경우 fabric 끊김을 keep-alive 가 감지하기 전에 IO timeout 이 먼저 트리거되어
		 *         불필요한 reset 이 발생할 수 있음. */
		if (ctrlr->opts.keep_alive_timeout_ms * SPDK_MSEC_TO_USEC > timeout_io_us) {
			NVME_CTRLR_WARNLOG(ctrlr,
					   "opts.keep_alive_timeout_ms %u should be less than timeout_io_us %lu\n",
					   ctrlr->opts.keep_alive_timeout_ms, timeout_io_us);
		}

		if (ctrlr->opts.keep_alive_timeout_ms * SPDK_MSEC_TO_USEC > timeout_admin_us) {
			NVME_CTRLR_WARNLOG(ctrlr,
					   "opts.keep_alive_timeout_ms %u should be less than timeout_admin_us %lu\n",
					   ctrlr->opts.keep_alive_timeout_ms, timeout_admin_us);
		}

		active_proc->timeout_io_ticks = timeout_io_us * spdk_get_ticks_hz() / 1000000ULL;
		                                /* [한국어] us → ticks 변환. spdk_get_ticks_hz() = TSC 주파수 (예: 3.3GHz).
		                                 *         qpair 폴링 시 (now_tick - submit_tick) > timeout_ticks 비교. */
		active_proc->timeout_admin_ticks = timeout_admin_us * spdk_get_ticks_hz() / 1000000ULL;
		                                /* [한국어] admin command 별도 timeout — 보통 IO 보다 길게 설정. */
		active_proc->timeout_cb_fn = cb_fn;
		                                /* [한국어] timeout 콜백 — bdev_nvme 가 reset 또는 abort 결정. */
		active_proc->timeout_cb_arg = cb_arg;
	}

	ctrlr->timeout_enabled = true;          /* [한국어] 전역 플래그 — qpair 폴링이 이 플래그 set 일 때만 timeout 검사
	                                         *         수행 (오버헤드 회피). 한 번 켜지면 유지. */

	nvme_ctrlr_unlock(ctrlr);
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_is_log_page_supported - 특정 log page ID 지원 여부 조회 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @log_page: 검사할 log page ID (0x00~0xFF)
 * @return true=지원, false=미지원
 *
 * Identify Controller / Get Features 응답을 분석하여 nvme_ctrlr_set_supported_log_pages 에서
 * log_page_supported[256] 비트맵을 채워둠. SMART(0x02), Error(0x01), Firmware Slot(0x03),
 * Changed NS List(0x04), Commands Supported(0x05) 등 NVMe 표준 + Intel 벤더 log page 모두 포함.
 *
 * 사용처: nvme-cli 류 도구가 사용자에게 "이 log page 사용 가능?" 표시. bdev_nvme 가 SMART 폴링
 * 활성화 여부 결정.
 *
 * 동기화: read-only — log_page_supported[] 는 init 이후 변경 없음 (락 불필요).
 */
bool
spdk_nvme_ctrlr_is_log_page_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page)
{
	/* No bounds check necessary, since log_page is uint8_t and log_page_supported has 256 entries */
	/* [한국어] uint8_t 범위 = 0~255 = 배열 크기와 정확히 일치 — 별도 bounds check 불요. */
	SPDK_STATIC_ASSERT(sizeof(ctrlr->log_page_supported) == 256, "log_page_supported size mismatch");
	                                  /* [한국어] 컴파일 타임 assert — 배열 크기 변경 시 즉시 빌드 에러. */
	return ctrlr->log_page_supported[log_page];
	                                  /* [한국어] 비트맵 단순 lookup — bool 배열. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_is_feature_supported - 특정 Feature ID(FID) 지원 여부 조회 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @feature_code: 검사할 Feature ID (0x00~0xFF — NVMe spec §5.21)
 * @return true=지원, false=미지원
 *
 * Get Features admin 명령으로 모든 FID 를 한 번씩 probe 한 결과를 feature_supported[256] 비트맵에
 * 저장 (nvme_ctrlr_set_supported_features). SET=Arbitration, Power Mgmt, Temperature Threshold,
 * Volatile Write Cache, Number of Queues 등.
 *
 * 사용처: 사용자가 spdk_nvme_ctrlr_cmd_set_feature 호출 전 사전 검증.
 *
 * 동기화: read-only — feature_supported[] 는 init 이후 변경 없음.
 */
bool
spdk_nvme_ctrlr_is_feature_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature_code)
{
	/* No bounds check necessary, since feature_code is uint8_t and feature_supported has 256 entries */
	/* [한국어] uint8_t 범위 = 배열 크기와 정확히 일치. */
	SPDK_STATIC_ASSERT(sizeof(ctrlr->feature_supported) == 256, "feature_supported size mismatch");
	return ctrlr->feature_supported[feature_code];
	                                  /* [한국어] 비트맵 lookup — bool 배열. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_attach_ns -
 *   Namespace Attachment (opcode 0x15, SEL=0x00) admin 동기 발행. 기존 namespace 를
 *   특정 컨트롤러(들)에 binding. attach 완료 후 active_ns 재 수집 + 새 ns 객체 construct.
 *
 * @ctrlr: 대상 컨트롤러 (= attach 명령을 발행하는 컨트롤러; payload 의 list 와 별개).
 * @nsid: attach 할 namespace ID. 0 금지.
 * @payload: spdk_nvme_ctrlr_list — 이 namespace 를 attach 할 컨트롤러들의 CNTLID 리스트.
 * @return: 0 성공, 음수 errno.
 *
 * NVMe Namespace Management 모델: namespace 는 "생성 (create_ns)" → "attach" 두 단계로 분리.
 * 한 namespace 가 여러 컨트롤러에 attach 될 수 있음 (multi-controller subsystem, ANA 등 활용).
 * attach 후에는 해당 컨트롤러의 active_ns 트리에 추가되고 호스트는 일반 IO 가능.
 *
 * 단계:
 *   1. nsid==0 차단 (Namespace Attachment 의 unsupported NSID).
 *   2. status tracker 할당 후 attach_ns admin (opcode 0x15, SEL=0x00 Controller Attach) 발행.
 *   3. 동기 polling 완료 대기 (true = command status 검사).
 *   4. nvme_ctrlr_identify_active_ns 호출 — active NS list (CNS 0x02) 재 수집해 트리 갱신.
 *   5. 새 ns 객체 get_ns 로 찾은 뒤 nvme_ns_construct 로 nsdata 등 메타데이터 초기화.
 *
 * 호출 체인:
 *   사용자/RPC nvme_attach_ns → [본 함수]
 *     → nvme_ctrlr_cmd_attach_ns (opcode 0x15, SEL=0x00)
 *     → nvme_wait_for_adminq_completion
 *     → nvme_ctrlr_identify_active_ns (트리 재구성)
 *     → nvme_ns_construct (ns 메타데이터 채움)
 */
int
spdk_nvme_ctrlr_attach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			  struct spdk_nvme_ctrlr_list *payload)
{
	struct nvme_completion_poll_status	*status;
	struct spdk_nvme_ns			*ns;
	int					res;

	/* [한국어] NSID 0 은 attach 의미 없음 — 컨트롤러 자체 또는 broadcast 의미라 거부. */
	if (nsid == 0) {
		return -EINVAL;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	/* [한국어] Namespace Attachment admin 발행 — payload 가 attach 대상 컨트롤러 CNTLID 리스트. */
	res = nvme_ctrlr_cmd_attach_ns(ctrlr, nsid, payload,
				       nvme_completion_poll_cb, status);
	if (res) {
		free(status);
		return res;
	}

	res = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (res) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for spdk_nvme_ctrlr_attach_ns failed: rc=%s\n",
				  spdk_strerror(abs(res)));
		return res;
	}

	/* [한국어] attach 후 active NS 목록이 바뀌었으므로 동기적으로 재 수집 (트리 갱신). */
	res = nvme_ctrlr_identify_active_ns(ctrlr);
	if (res) {
		return res;
	}

	/* [한국어] 새로 추가된 ns 객체를 찾아 nsdata/필드 초기화 — IO 발행 전제 준비. */
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_get_ns failed!\n");
		return -ENXIO;
	}

	return nvme_ns_construct(ns, nsid, ctrlr);
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_detach_ns -
 *   Namespace Attachment (opcode 0x15, SEL=0x01) admin 동기 발행. 기존 binding 해제.
 *
 * @ctrlr: 대상 컨트롤러.
 * @nsid: detach 할 NSID.
 * @payload: detach 할 컨트롤러들의 CNTLID 리스트.
 * @return: 0 성공, 음수 errno.
 *
 * attach 와 대칭. detach 후에도 namespace 자체는 존재 (subsystem 보유) — delete_ns 와
 * 구분. detach 된 컨트롤러에서는 더 이상 IO 발행 불가.
 *
 * detach 후 active_ns 재 수집 — 트리에서 해당 nsid 가 빠짐 (또는 다른 controller 에 여전히
 * attach 돼 있으면 보존되지만 본 컨트롤러 트리에서는 제거).
 *
 * 호출 체인:
 *   사용자/RPC → [본 함수]
 *     → nvme_ctrlr_cmd_detach_ns (opcode 0x15, SEL=0x01)
 *     → nvme_ctrlr_identify_active_ns (트리 갱신)
 */
int
spdk_nvme_ctrlr_detach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			  struct spdk_nvme_ctrlr_list *payload)
{
	struct nvme_completion_poll_status	*status;
	int					res;

	if (nsid == 0) {
		return -EINVAL;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	/* [한국어] Namespace Attachment admin (SEL=0x01) — 1=Detach 의미. */
	res = nvme_ctrlr_cmd_detach_ns(ctrlr, nsid, payload,
				       nvme_completion_poll_cb, status);
	if (res) {
		free(status);
		return res;
	}

	res = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (res) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for spdk_nvme_ctrlr_detach_ns failed: rc=%s\n",
				  spdk_strerror(abs(res)));
		return res;
	}

	/* [한국어] detach 후 active NS 재 수집 — 본 컨트롤러 트리에서 해당 nsid 제거됨. */
	return nvme_ctrlr_identify_active_ns(ctrlr);
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_create_ns -
 *   Namespace Management (opcode 0x0D, SEL=0x00) admin 동기 발행. subsystem 에
 *   새 namespace 를 생성. attach 와 분리 — 생성된 ns 는 어떤 컨트롤러에도 binding 안 됨.
 *
 * @ctrlr: 명령 발행 컨트롤러.
 * @payload: 새 ns 의 spdk_nvme_ns_data (size, capacity, FLBAS, DPS 등 모든 메타데이터).
 * @return: 생성된 NSID (>0) — admin 응답의 cdw0 에 담겨 옴. 실패 시 0.
 *
 * 반환 타입이 uint32_t (errno 아님) 이유: NVMe spec 이 NSID 를 cdw0 로 돌려주는 패턴.
 * 0 = 실패, 양수 = 생성된 NSID. 호출자는 이 NSID 로 spdk_nvme_ctrlr_attach_ns 호출.
 *
 * 단계:
 *   1. status tracker 할당.
 *   2. cmd_create_ns 발행 (opcode 0x0D, SEL=0x00 = Create).
 *   3. 비동기 polling — nvme_wait_for_adminq_completion(false) = command status 검사 안 함
 *      (호출자가 직접 status->cpl.cdw0 으로 NSID 받음).
 *   4. 응답의 cdw0 = 생성된 NSID. timeout 분기로 free 정책.
 *   5. NSID 반환 (또는 0).
 *
 * 호출 체인:
 *   사용자/RPC nvme_create_ns → [본 함수]
 *     → nvme_ctrlr_cmd_create_ns (opcode 0x0D, SEL=0x00)
 *     → cdw0 = NSID
 */
uint32_t
spdk_nvme_ctrlr_create_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns_data *payload)
{
	struct nvme_completion_poll_status	*status;
	int					res;
	uint32_t				nsid;

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return 0;
	}

	/* [한국어] Namespace Management create. payload 가 새 ns 의 모든 메타데이터. */
	res = nvme_ctrlr_cmd_create_ns(ctrlr, payload, nvme_completion_poll_cb, status);
	if (res) {
		free(status);
		return 0;
	}

	/* [한국어] false = command status 검사 안 함 (호출자가 cdw0 직접 사용).
	 * cdw0 = 생성된 NSID, 실패 시 0. */
	res = nvme_wait_for_adminq_completion(ctrlr, status, false);
	nsid = status->cpl.cdw0;
	if (!status->timed_out) {
		free(status);
	}

	if (res) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for spdk_nvme_ctrlr_create_ns failed: rc=%s\n",
				  spdk_strerror(abs(res)));
		return 0;
	}

	/* [한국어] 성공 시 nsid 는 항상 양수 (NVMe spec). */
	assert(nsid > 0);

	/* Return the namespace ID that was created */
	return nsid;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_delete_ns -
 *   Namespace Management (opcode 0x0D, SEL=0x01) admin 동기 발행. subsystem 의
 *   namespace 를 영구 삭제. delete 전에 모든 컨트롤러에서 detach 되어 있어야 함.
 *
 * @ctrlr: 명령 발행 컨트롤러.
 * @nsid: 삭제할 NSID. 0 금지. 0xFFFFFFFF = 모든 namespace 삭제 (구현 지원 시).
 * @return: 0 성공, 음수 errno.
 *
 * create_ns 의 역. delete 후 NSID 는 재 사용 가능 (다음 create_ns 가 같은 NSID 반환 가능).
 * detach_ns 와의 차이: detach 는 binding 만 해제 (ns 자체는 보존), delete 는 ns 영구 삭제.
 *
 * 호출 체인:
 *   사용자/RPC → [본 함수]
 *     → nvme_ctrlr_cmd_delete_ns (opcode 0x0D, SEL=0x01)
 *     → nvme_ctrlr_identify_active_ns (트리 갱신)
 */
int
spdk_nvme_ctrlr_delete_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct nvme_completion_poll_status	*status;
	int					res;

	if (nsid == 0) {
		return -EINVAL;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	/* [한국어] Namespace Management delete (SEL=0x01). */
	res = nvme_ctrlr_cmd_delete_ns(ctrlr, nsid, nvme_completion_poll_cb, status);
	if (res) {
		free(status);
		return res;
	}

	res = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (res) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for spdk_nvme_ctrlr_delete_ns failed: rc=%s\n",
				  spdk_strerror(abs(res)));
		return res;
	}

	/* [한국어] delete 후 active NS 재 수집 — 삭제된 nsid 가 트리에서 빠짐. */
	return nvme_ctrlr_identify_active_ns(ctrlr);
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_format -
 *   Format NVM (opcode 0x80) admin 명령 동기 발행 + 완료 후 컨트롤러 reset.
 *
 * @ctrlr: 대상 컨트롤러.
 * @nsid: format 대상 NSID. 0xFFFFFFFF = 모든 namespace 일괄 (FNA bit 0 지원 시).
 *        구체적 NSID 가능 여부는 FNA(Format NVM Attributes, cdata.fna) 비트 결정.
 * @format: spdk_nvme_format 구조체 — lbaf(LBA Format index), ms(Metadata Settings),
 *          pi(Protection Information type), pil(PI Location), ses(Secure Erase Settings).
 * @return: 0 성공, 음수 errno.
 *
 * 동작:
 *   1. status tracker (nvme_completion_poll_status) 동적 할당 — admin 완료 polling 용.
 *   2. nvme_ctrlr_cmd_format 로 admin 명령 발행.
 *   3. nvme_wait_for_adminq_completion 으로 polling 완료 대기 (timeout 시 free 책임 분기).
 *   4. 성공 시 spdk_nvme_ctrlr_reset 호출 — format 은 namespace identify 결과를 무효화하므로
 *      reset 후 다시 IDENTIFY chain 으로 namespace 메타데이터 재수집 필요.
 *
 * 주의: format 은 데이터 파괴적 명령. SES (Secure Erase Settings) 가 0=None 이면 metadata
 * 영역만 다시 쓰고 사용자 데이터 보존, 1=User Data Erase, 2=Cryptographic Erase.
 *
 * 실행 컨텍스트: 호스트 응용 (CLI 도구). 동기 — admin 완료까지 block.
 *
 * 호출 체인:
 *   사용자/RPC → [본 함수]
 *     → nvme_ctrlr_cmd_format (opcode 0x80)
 *     → nvme_wait_for_adminq_completion
 *     → spdk_nvme_ctrlr_reset (format 후 명시적 reset 필수)
 */
int
spdk_nvme_ctrlr_format(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
		       struct spdk_nvme_format *format)
{
	struct nvme_completion_poll_status	*status;
	int					res;

	/* [한국어] status tracker — 동기 polling 의 결과/타이밍 추적용. heap 에 두는 이유:
	 * timeout 시 콜백이 늦게 도착해도 dangling 회피 — 스택이면 timeout 후 함수 반환 시
	 * 스택 corruption 위험. nvme_wait_for_adminq_completion 이 timeout 시 status 보존 권한
	 * 을 갖는다 (timed_out=true 면 호출자가 free 하지 않음). */
	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	/* [한국어] Format NVM admin 발행 — 비동기. completion 콜백은 nvme_completion_poll_cb
	 * 가 status->done=true 로 세팅. */
	res = nvme_ctrlr_cmd_format(ctrlr, nsid, format, nvme_completion_poll_cb,
				    status);
	if (res) {
		free(status);
		return res;
	}

	/* [한국어] true 인자 = "command status 검사 위임" (호출자는 res 만 확인).
	 * format 은 일반적으로 오래 걸리므로 admin_timeout_ms 충분히 커야 함. */
	res = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (res) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for spdk_nvme_ctrlr_format failed: rc=%s\n",
				  spdk_strerror(abs(res)));
		return res;
	}

	/* [한국어] format 성공 후 reset — namespace 메타데이터 (LBA size, metadata size, PI
	 * 등) 가 바뀌었을 수 있으므로 init state machine 을 다시 돌려 ns 트리 재구성. */
	return spdk_nvme_ctrlr_reset(ctrlr);
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_update_firmware -
 *   Firmware Image Download (opcode 0x11) 를 페이지 단위로 반복 + Firmware Commit
 *   (opcode 0x10) 발행 후 컨트롤러 reset. NVMe spec §5.13 (Download) / §5.14 (Commit).
 *
 * @ctrlr: 대상 컨트롤러.
 * @payload: 새 firmware 이미지의 host 메모리 버퍼.
 * @size: 이미지 크기. 4-byte multiple 필수 (NVMe spec 규정).
 * @slot: commit 할 firmware slot 번호 (1..FRMW.NOFS). 0 = 컨트롤러 선택.
 * @commit_action: REPLACE_IMG (downloaded → slot, 활성화 X) 또는
 *                  REPLACE_AND_ENABLE_IMG (downloaded → slot + 즉시 활성).
 * @completion_status: out — commit 의 spdk_nvme_status (SCT/SC). 호출자가 추가 검사 가능.
 * @return: 0 성공, 음수 errno (-EINVAL, -1, -ENOMEM, fw_commit 의 res 등).
 *
 * 전체 흐름:
 *   1. 입력 검증: completion_status NULL 금지, size % 4 == 0, commit_action 화이트리스트.
 *   2. 단일 status tracker 재사용으로 모든 admin 명령 polling.
 *   3. Firmware Download 페이지 단위 (min_page_size 청크):
 *      while (남음 > 0):
 *        - 전송 크기 = min(남음, min_page_size)
 *        - status zero-init → nvme_ctrlr_cmd_fw_image_download → wait
 *        - 실패 시 timeout 분기 (timed_out 이면 status 보존 정책)
 *        - p / offset / size_remaining 갱신
 *   4. Firmware Commit (opcode 0x10): fw_commit { fs=slot, ca=commit_action } admin 발행.
 *   5. commit 응답의 status 를 completion_status 에 복사.
 *   6. NVM_RESET (commit 후 reset 요구) 외의 모든 에러 → 음수 반환.
 *      - CONVENTIONAL_RESET 요구는 NOTICELOG 후 음수 반환 (호출자가 직접 PCIe reset).
 *      - NVM_RESET 요구만 정상 경로로 진행 → spdk_nvme_ctrlr_reset 호출.
 *
 * 페이지 청킹 이유: 일부 컨트롤러는 한 번에 작은 단위만 받을 수 있어 ctrlr->min_page_size
 * (보통 4KiB) 로 잘라 보낸다. NVMe spec 은 max FW transfer size 별도 정의 없음.
 *
 * 호출 체인:
 *   사용자/RPC fw_update → [본 함수]
 *     → loop: nvme_ctrlr_cmd_fw_image_download (opcode 0x11, NUMD/OFST 필드)
 *     → nvme_ctrlr_cmd_fw_commit (opcode 0x10, FS/CA dword)
 *     → spdk_nvme_ctrlr_reset (필요 시)
 */
int
spdk_nvme_ctrlr_update_firmware(struct spdk_nvme_ctrlr *ctrlr, void *payload, uint32_t size,
				int slot, enum spdk_nvme_fw_commit_action commit_action, struct spdk_nvme_status *completion_status)
{
	struct spdk_nvme_fw_commit		fw_commit;
	struct nvme_completion_poll_status	*status;
	int					res;
	unsigned int				size_remaining;
	unsigned int				offset;
	unsigned int				transfer;
	uint8_t					*p;

	/* [한국어] 입력 검증 — completion_status out 포인터 필수. */
	if (!completion_status) {
		return -EINVAL;
	}
	memset(completion_status, 0, sizeof(struct spdk_nvme_status));
	/* [한국어] NVMe spec 규정: firmware download 크기는 dword (4B) 정렬 필수.
	 * NUMD 필드가 dword 단위로 길이를 인코딩하기 때문. */
	if (size % 4) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_update_firmware invalid size!\n");
		return -1;
	}

	/* Current support only for SPDK_NVME_FW_COMMIT_REPLACE_IMG
	 * and SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG
	 */
	/* [한국어] commit_action 화이트리스트 — ACTIVATE_NO_RESET 등 다른 action 미지원.
	 * (활성 즉시 reset 필요한 동작만 본 API 가 처리, 그 외는 호출자가 직접 구성.) */
	if ((commit_action != SPDK_NVME_FW_COMMIT_REPLACE_IMG) &&
	    (commit_action != SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG)) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_update_firmware invalid command!\n");
		return -1;
	}

	/* [한국어] status tracker — 본 함수의 모든 admin 명령에서 재사용 (memset 으로 매번
	 * 초기화). format 과 같은 heap 할당 이유. */
	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	/* Firmware download */
	/* [한국어] 페이지 단위 download 루프 — payload 를 ctrlr->min_page_size (보통 4KiB)
	 * 씩 잘라 NVMe 컨트롤러에 전달. 컨트롤러는 OFST 로 누적 위치 추적. */
	size_remaining = size;
	offset = 0;
	p = payload;

	while (size_remaining > 0) {
		/* [한국어] 한 번에 보낼 양 = min(남음, 페이지 크기). */
		transfer = spdk_min(size_remaining, ctrlr->min_page_size);

		memset(status, 0, sizeof(*status));
		/* [한국어] Firmware Image Download admin (opcode 0x11):
		 *  - NUMD: transfer 의 dword 수
		 *  - OFST: 누적 offset (dword)
		 *  - 데이터 포인터: p */
		res = nvme_ctrlr_cmd_fw_image_download(ctrlr, transfer, offset, p,
						       nvme_completion_poll_cb,
						       status);
		if (res) {
			free(status);
			return res;
		}

		/* [한국어] false = "command status 검사 안 함" (호출자가 직접 res 로 판단).
		 * 본 함수는 res 만 보면 충분. */
		res = nvme_wait_for_adminq_completion(ctrlr, status, false);
		if (res) {
			/* [한국어] timeout 시 status 보존 (콜백이 늦게 도착할 수 있음). */
			if (!status->timed_out) {
				free(status);
			}

			NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_fw_image_download failed: rc=%s\n",
					  spdk_strerror(abs(res)));
			return res;
		}

		/* [한국어] 다음 페이지로 이동. */
		p += transfer;
		offset += transfer;
		size_remaining -= transfer;
	}

	/* Firmware commit */
	/* [한국어] Firmware Commit (opcode 0x10) — 다운로드된 이미지를 slot 에 영속 저장 또는 활성화.
	 *  - fs (Firmware Slot, dword 10 비트 2:0): 어느 slot 에 commit
	 *  - ca (Commit Action, dword 10 비트 5:3): REPLACE_IMG / REPLACE_AND_ENABLE / ACTIVATE_NO_RESET 등
	 *  - bpid (Boot Partition ID, dword 10 비트 31): 0 (= boot partition 아님) */
	memset(&fw_commit, 0, sizeof(struct spdk_nvme_fw_commit));
	fw_commit.fs = slot;
	fw_commit.ca = commit_action;

	memset(status, 0, sizeof(*status));
	res = nvme_ctrlr_cmd_fw_commit(ctrlr, &fw_commit, nvme_completion_poll_cb,
				       status);
	if (res) {
		free(status);
		return res;
	}

	/* [한국어] commit 완료 대기 + 응답 status 를 호출자에게 전달 (호출자가 SCT/SC 직접 검사). */
	res = nvme_wait_for_adminq_completion(ctrlr, status, false);
	memcpy(completion_status, &status->cpl.status, sizeof(struct spdk_nvme_status));
	if (!status->timed_out) {
		free(status);
	}

	if (res) {
		/* [한국어] commit 의 일부 정상 응답은 "reset 요청" 으로 분류된다 (NVMe spec):
		 *  - FIRMWARE_REQ_NVM_RESET (NVM Subsystem Reset 필요): 본 함수가 마지막에 reset 호출.
		 *  - FIRMWARE_REQ_CONVENTIONAL_RESET (PCIe conventional reset 필요): 호스트가 PCIe
		 *    레벨에서 처리해야 하므로 본 함수는 NOTICELOG 후 음수 반환 (호출자 책임).
		 *  - 그 외 에러: 일반 실패 처리. */
		if (completion_status->sct != SPDK_NVME_SCT_COMMAND_SPECIFIC ||
		    completion_status->sc != SPDK_NVME_SC_FIRMWARE_REQ_NVM_RESET) {
			if (completion_status->sct == SPDK_NVME_SCT_COMMAND_SPECIFIC  &&
			    completion_status->sc == SPDK_NVME_SC_FIRMWARE_REQ_CONVENTIONAL_RESET) {
				NVME_CTRLR_NOTICELOG(ctrlr,
						     "firmware activation requires conventional reset to be performed. !\n");
			} else {
				NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_ctrlr_cmd_fw_commit failed: rc=%s\n",
						  spdk_strerror(abs(res)));
			}

			return res;
		}
	}

	/* [한국어] NVM_RESET 요청 또는 정상 commit → 컨트롤러 reset 으로 새 firmware 활성. */
	return spdk_nvme_ctrlr_reset(ctrlr);
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_reserve_cmb -
 *   Controller Memory Buffer (CMB) 의 사용 권한 확보 + 사이즈 반환.
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 양수 = CMB 크기(바이트), -ENOTSUP = CMB 미지원 또는 R/W disable.
 *
 * CMB (NVMe 1.2+): NVMe 컨트롤러의 BAR 일부를 호스트가 메모리로 read/write 가능한 영역으로
 * 노출. 용도: PRP/SQE/CQE 자체를 컨트롤러 내부 메모리에 두면 PCIe 왕복 1회 감소 (호스트
 * 메모리 → BAR doorbell write → 컨트롤러가 호스트 메모리 fetch 대신, 모든 게 BAR 내).
 *
 * 단계:
 *   1. CMBSZ 레지스터 read — RDS/WDS 비트로 R/W 지원 여부 확인. 둘 중 하나라도 0 이면 미지원.
 *   2. CMB 크기 계산: sz * (0x1000 << (szu * 4))
 *      - sz: SZU 단위 수.
 *      - szu: Size Unit (0=4KB, 1=64KB, 2=1MB, 3=16MB, 4=256MB, 5=4GB).
 *   3. transport ops 의 reserve_cmb 호출 — PCIe 의 경우 BAR 매핑 권한 확보.
 *   4. 성공 시 크기 반환.
 *
 * 동기화: ctrlr_lock 으로 reserve 와 다른 transport 조작 직렬화.
 *
 * 호출 체인:
 *   사용자 → [본 함수] → nvme_transport_ctrlr_reserve_cmb (transport-specific)
 */
int
spdk_nvme_ctrlr_reserve_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc, size;
	union spdk_nvme_cmbsz_register cmbsz;

	/* [한국어] CMBSZ (Controller Memory Buffer Size, offset 0x38) read. */
	cmbsz = spdk_nvme_ctrlr_get_regs_cmbsz(ctrlr);

	/* [한국어] RDS (Read Data Support) / WDS (Write Data Support) — 둘 다 1 이어야 IO data 용으로
	 * 사용 가능. 하나만 지원하면 양방향 IO 발행 어렵다. */
	if (cmbsz.bits.rds == 0 || cmbsz.bits.wds == 0) {
		return -ENOTSUP;
	}

	/* [한국어] 크기 = sz * (4KB << szu*4).
	 * szu=0 → 4KB, szu=1 → 64KB, szu=2 → 1MB, ... */
	size = cmbsz.bits.sz * (0x1000 << (cmbsz.bits.szu * 4));

	nvme_ctrlr_lock(ctrlr);
	/* [한국어] transport 측 reserve — PCIe 면 BAR mapping 권한 확보 + 다른 사용자와 배제. */
	rc = nvme_transport_ctrlr_reserve_cmb(ctrlr);
	nvme_ctrlr_unlock(ctrlr);

	if (rc < 0) {
		return rc;
	}

	return size;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_map_cmb - CMB BAR 영역을 호스트 가상주소 공간에 mmap.
 *
 * @ctrlr: 대상 컨트롤러.
 * @size: out — 매핑된 영역 크기.
 * @return: 호스트 가상주소 (성공) 또는 NULL.
 *
 * reserve_cmb 후 호출 — 사용자가 이 가상주소로 read/write 하면 BAR MMIO 발생.
 * 호스트 입장에서는 일반 메모리처럼 보이지만 실제로는 PCIe write/read 트랜잭션.
 *
 * 호출 체인: 사용자 → [본 함수] → nvme_transport_ctrlr_map_cmb (PCIe BAR mmap)
 */
void *
spdk_nvme_ctrlr_map_cmb(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	void *buf;

	nvme_ctrlr_lock(ctrlr);
	buf = nvme_transport_ctrlr_map_cmb(ctrlr, size);
	nvme_ctrlr_unlock(ctrlr);

	return buf;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_unmap_cmb - CMB 매핑 해제 (map 의 역).
 *
 * 호출자는 unmap 후 이전 가상주소 사용 금지 (segfault).
 */
void
spdk_nvme_ctrlr_unmap_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_ctrlr_lock(ctrlr);
	nvme_transport_ctrlr_unmap_cmb(ctrlr);
	nvme_ctrlr_unlock(ctrlr);
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_enable_pmr - Persistent Memory Region 활성화.
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 0 성공, 음수 errno.
 *
 * PMR (NVMe 1.4+): 컨트롤러가 제공하는 persistent (전원 차단 시에도 보존되는) 메모리 영역.
 * battery-backed 또는 NVRAM. 호스트가 가상주소로 mmap 해 사용 — 쓰기가 즉시 영속.
 * 용도: high-performance journaling, lockless metadata 등.
 *
 * 호출 체인: 사용자 → [본 함수] → nvme_transport_ctrlr_enable_pmr
 *   (PMRCTL.EN bit set + PMRSTS polling)
 */
int
spdk_nvme_ctrlr_enable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	nvme_ctrlr_lock(ctrlr);
	rc = nvme_transport_ctrlr_enable_pmr(ctrlr);
	nvme_ctrlr_unlock(ctrlr);

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_disable_pmr - PMR 비활성화 (enable 의 역).
 *
 * 호출자는 disable 후 PMR 매핑 가상주소 사용 금지.
 */
int
spdk_nvme_ctrlr_disable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	nvme_ctrlr_lock(ctrlr);
	rc = nvme_transport_ctrlr_disable_pmr(ctrlr);
	nvme_ctrlr_unlock(ctrlr);

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_map_pmr - PMR 영역 mmap.
 *
 * @ctrlr: 대상 컨트롤러 (enable_pmr 후).
 * @size: out — 매핑된 크기.
 * @return: 호스트 가상주소 또는 NULL.
 *
 * PMR 은 일반 메모리처럼 보이지만 write 가 즉시 persistent. fence/flush 도 PMR 컨트롤러가
 * 적절히 처리.
 */
void *
spdk_nvme_ctrlr_map_pmr(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	void *buf;

	nvme_ctrlr_lock(ctrlr);
	buf = nvme_transport_ctrlr_map_pmr(ctrlr, size);
	nvme_ctrlr_unlock(ctrlr);

	return buf;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_unmap_pmr - PMR 매핑 해제.
 *
 * @return: 0 성공, 음수 errno.
 */
int
spdk_nvme_ctrlr_unmap_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	nvme_ctrlr_lock(ctrlr);
	rc = nvme_transport_ctrlr_unmap_pmr(ctrlr);
	nvme_ctrlr_unlock(ctrlr);

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_read_boot_partition_start -
 *   Boot Partition 비동기 READ 트리거. BPMBL (Boot Partition Memory Buffer Location)
 *   레지스터에 physical address 등록 + BPRSEL (Boot Partition Read Select) 에
 *   bpid/bprof/bprsz 기록하면 컨트롤러가 DMA 로 호스트 메모리에 boot partition 복사.
 *
 * @ctrlr: 대상 컨트롤러.
 * @payload: 호스트 메모리 버퍼 (DMA 대상). 물리적으로 연속이어야 함.
 * @bprsz: Boot Partition Read Size (4KiB 단위 수).
 * @bprof: Boot Partition Read Offset (4KiB 단위).
 * @bpid: Boot Partition ID (0 또는 1, 컨트롤러 지원 시).
 * @return: 0 = 트리거 성공 (read는 background 진행), 음수 errno.
 *
 * NVMe Boot Partition (NVMe spec §3.1.5.2): 컨트롤러가 두 개의 boot partition (BPID 0/1)
 * 을 제공해 호스트가 부팅 firmware 를 직접 BAR-mapped DMA 로 read 가능. 인지 후에는
 * BPINFO.BRS 레지스터를 polling 해 READ_SUCCESS 까지 기다린다 (read_boot_partition_poll).
 *
 * 동작:
 *   1. CAP.BPS=0 이면 boot partition 미지원 → -ENOTSUP.
 *   2. BPINFO 레지스터로 BRS (Boot Read Status) 검사 — IN_PROGRESS 면 -EALREADY.
 *   3. payload 의 physical address 변환 (spdk_vtophys) — 4KiB * bprsz 바이트가 physically contiguous 인지 검증.
 *   4. BPMBL (offset 0x30) 에 physical address write — 컨트롤러가 DMA 목적지로 사용.
 *   5. BPRSEL (offset 0x28) 에 {bpid, bprof, bprsz} write — 이 write 가 read trigger.
 *
 * 동기화: ctrlr_lock 으로 BPMBL/BPRSEL write sequence 보호 — 다른 read 트리거와 race 방지.
 *
 * 호출 체인:
 *   사용자/CLI → [본 함수] (트리거) → 호스트가 polling → read_boot_partition_poll
 */
int
spdk_nvme_ctrlr_read_boot_partition_start(struct spdk_nvme_ctrlr *ctrlr, void *payload,
		uint32_t bprsz, uint32_t bprof, uint32_t bpid)
{
	union spdk_nvme_bprsel_register bprsel;
	union spdk_nvme_bpinfo_register bpinfo;
	uint64_t bpmbl, bpmb_size;

	/* [한국어] CAP.BPS (Boot Partition Support, bit 45) — boot partition 기능 가용성. */
	if (ctrlr->cap.bits.bps == 0) {
		return -ENOTSUP;
	}

	/* [한국어] BPINFO (Boot Partition Information) 읽어 현재 read status 확인.
	 * BRS 필드: 0=NO_READ, 1=READ_IN_PROGRESS, 2=READ_SUCCESS, 3=READ_ERROR. */
	if (nvme_ctrlr_get_bpinfo(ctrlr, &bpinfo)) {
		NVME_CTRLR_ERRLOG(ctrlr, "get bpinfo failed\n");
		return -EIO;
	}

	/* [한국어] 이미 진행 중이면 중복 트리거 금지 — 컨트롤러는 한 번에 하나만 처리. */
	if (bpinfo.bits.brs == SPDK_NVME_BRS_READ_IN_PROGRESS) {
		NVME_CTRLR_ERRLOG(ctrlr, "Boot Partition read already initiated\n");
		return -EALREADY;
	}

	nvme_ctrlr_lock(ctrlr);

	/* [한국어] payload 의 virtual → physical 주소 변환. DPDK hugepage 환경에서 동작.
	 * bpmb_size 는 in/out — 호출 시 요청한 크기, 반환 시 실제 연속된 크기. */
	bpmb_size = bprsz * 4096;
	bpmbl = spdk_vtophys(payload, &bpmb_size);
	if (bpmbl == SPDK_VTOPHYS_ERROR) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_vtophys of bpmbl failed\n");
		nvme_ctrlr_unlock(ctrlr);
		return -EFAULT;
	}

	/* [한국어] 반환된 연속 크기가 요청 크기와 같지 않으면 = 중간에 물리 페이지가 끊김.
	 * BPMBL 은 단일 base address 만 받으므로 PRP 같은 list 가 불가 — physically contiguous 필수. */
	if (bpmb_size != bprsz * 4096) {
		NVME_CTRLR_ERRLOG(ctrlr, "Boot Partition buffer is not physically contiguous\n");
		nvme_ctrlr_unlock(ctrlr);
		return -EFAULT;
	}

	/* [한국어] BPMBL 레지스터 write — 컨트롤러가 이 PA 로 DMA. */
	if (nvme_ctrlr_set_bpmbl(ctrlr, bpmbl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_bpmbl() failed\n");
		nvme_ctrlr_unlock(ctrlr);
		return -EIO;
	}

	/* [한국어] BPRSEL 비트필드 채우기 — 어느 partition 의 어디서부터 얼마나 read. */
	bprsel.bits.bpid = bpid;
	bprsel.bits.bprof = bprof;
	bprsel.bits.bprsz = bprsz;

	/* [한국어] BPRSEL write = read trigger. write 자체가 DMA 시작 신호. */
	if (nvme_ctrlr_set_bprsel(ctrlr, &bprsel)) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_bprsel() failed\n");
		nvme_ctrlr_unlock(ctrlr);
		return -EIO;
	}

	nvme_ctrlr_unlock(ctrlr);
	return 0;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_read_boot_partition_poll -
 *   BPINFO.BRS 상태 폴링 — read_boot_partition_start 후 완료 여부 확인.
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 0 = read 완료, -EAGAIN = 아직 진행 중 (다시 호출), -EIO/EINVAL = 에러.
 *
 * 호출자는 0 또는 -EAGAIN 이외의 음수가 나올 때까지 주기적으로 폴링한다.
 * payload 버퍼는 0 반환 후에 안전하게 read 가능.
 *
 * BRS 상태 4가지:
 *   - NO_READ (0): 미시작 → 호출자 실수, -EINVAL.
 *   - READ_IN_PROGRESS (1): 진행 중 → -EAGAIN (계속 폴링).
 *   - READ_ERROR (2): 컨트롤러 에러 → -EIO.
 *   - READ_SUCCESS (3): 완료 → 0 (payload 사용 가능).
 *
 * 호출 체인:
 *   사용자 polling loop → [본 함수] (until rc != -EAGAIN)
 */
int
spdk_nvme_ctrlr_read_boot_partition_poll(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	union spdk_nvme_bpinfo_register bpinfo;

	/* [한국어] BPINFO 매번 fresh read — BAR0 MMIO. */
	if (nvme_ctrlr_get_bpinfo(ctrlr, &bpinfo)) {
		NVME_CTRLR_ERRLOG(ctrlr, "get bpinfo failed\n");
		return -EIO;
	}

	switch (bpinfo.bits.brs) {
	case SPDK_NVME_BRS_NO_READ:
		/* [한국어] start 호출 안 함 — 호출 순서 오류. */
		NVME_CTRLR_ERRLOG(ctrlr, "Boot Partition read not initiated\n");
		rc = -EINVAL;
		break;
	case SPDK_NVME_BRS_READ_IN_PROGRESS:
		/* [한국어] 아직 DMA 중 — caller 가 다시 폴링하라는 의미로 -EAGAIN. */
		NVME_CTRLR_DEBUGLOG(ctrlr, "Boot Partition read in progress\n");
		rc = -EAGAIN;
		break;
	case SPDK_NVME_BRS_READ_ERROR:
		/* [한국어] 컨트롤러가 DMA 중 에러 검출. payload 내용 신뢰 불가. */
		NVME_CTRLR_ERRLOG(ctrlr, "Error completing Boot Partition read\n");
		rc = -EIO;
		break;
	case SPDK_NVME_BRS_READ_SUCCESS:
		/* [한국어] 완료 — payload 사용 가능. */
		NVME_CTRLR_INFOLOG(ctrlr, "Boot Partition read completed successfully\n");
		break;
	default:
		/* [한국어] spec 외 값 — 컨트롤러 firmware bug 가능. */
		NVME_CTRLR_ERRLOG(ctrlr, "Invalid Boot Partition read status\n");
		rc = -EINVAL;
	}

	return rc;
}

/*
 * [한국어]
 * nvme_write_boot_partition_cb -
 *   write_boot_partition 의 4단계 state machine 콜백. 한 단계 admin 완료마다 호출되어
 *   다음 단계로 전이.
 *
 * @arg: ctrlr (start 호출 시 self 등록).
 * @cpl: 직전 admin 완료 entry.
 *
 * State machine (bp_ws):
 *   DOWNLOADING ──────────> DOWNLOADED ──────────> REPLACE ──────────> ACTIVATE ──> 사용자 콜백
 *      ↑    ↓                    ↓                     ↓                  ↓
 *      └────┘ (다음 page)        ↓                     ↓                  ↓
 *   page 단위 fw_image_download   ↓                     ↓                  ↓
 *                       fw_commit(REPLACE_BOOT_PARTITION)                  ↓
 *                                             fw_commit(ACTIVATE_BOOT_PARTITION)
 *
 * 동작:
 *   1. 에러 검사: cpl 이 에러면 즉시 사용자 콜백 호출 후 종료.
 *   2. DOWNLOADING:
 *      - fw_payload/offset/size_remaining 갱신.
 *      - 다음 chunk size = min(남음, min_page_size).
 *      - 다음 fw_image_download 발행.
 *      - 마지막 chunk (transfer_size < min_page_size) 면 DOWNLOADED 로 전이 예약.
 *   3. DOWNLOADED: fw_commit (CA=REPLACE_BOOT_PARTITION) 발행 → REPLACE 전이.
 *   4. REPLACE: fw_commit (CA=ACTIVATE_BOOT_PARTITION) 발행 → ACTIVATE 전이.
 *   5. ACTIVATE: 사용자 콜백 호출 (성공).
 *   6. 알 수 없는 state: 에러 콜백.
 *
 * 비동기 chain — 사용자는 콜백 1회만 받음 (성공 또는 첫 실패 시점). 중간 단계는 내부 처리.
 *
 * 실행 컨텍스트: admin completion polling 콜백 (reactor 스레드).
 *
 * 호출 체인:
 *   nvme_ctrlr_cmd_fw_image_download / fw_commit 완료
 *     → [본 함수]
 *     → 다음 단계 admin 발행 또는 사용자 콜백
 */
static void
nvme_write_boot_partition_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	int res;
	struct spdk_nvme_ctrlr *ctrlr = arg;
	struct spdk_nvme_fw_commit fw_commit;
	/* [한국어] 내부 에러 시 사용자에게 전달할 합성 cpl — INTERNAL_DEVICE_ERROR. */
	struct spdk_nvme_cpl err_cpl =
	{.status = {.sct = SPDK_NVME_SCT_GENERIC, .sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR }};

	/* [한국어] cpl 에러는 어느 단계든 즉시 사용자 콜백 + 종료. */
	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Write Boot Partition failed\n");
		ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, cpl);
		return;
	}

	if (ctrlr->bp_ws == SPDK_NVME_BP_WS_DOWNLOADING) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Boot Partition Downloading at Offset %d Success\n", ctrlr->fw_offset);
		/* [한국어] 직전 chunk 가 성공했으므로 cursor 전진. */
		ctrlr->fw_payload = (uint8_t *)ctrlr->fw_payload + ctrlr->fw_transfer_size;
		ctrlr->fw_offset += ctrlr->fw_transfer_size;
		ctrlr->fw_size_remaining -= ctrlr->fw_transfer_size;
		/* [한국어] 다음 chunk — 남은 양이 page 크기보다 작으면 마지막 chunk. */
		ctrlr->fw_transfer_size = spdk_min(ctrlr->fw_size_remaining, ctrlr->min_page_size);
		res = nvme_ctrlr_cmd_fw_image_download(ctrlr, ctrlr->fw_transfer_size, ctrlr->fw_offset,
						       ctrlr->fw_payload, nvme_write_boot_partition_cb, ctrlr);
		if (res) {
			NVME_CTRLR_ERRLOG(ctrlr, "nvme_ctrlr_cmd_fw_image_download failed!\n");
			ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, &err_cpl);
			return;
		}

		/* [한국어] 이 chunk 가 마지막 (page 크기 미만)이면 다음 콜백에서 DOWNLOADED state 처리. */
		if (ctrlr->fw_transfer_size < ctrlr->min_page_size) {
			ctrlr->bp_ws = SPDK_NVME_BP_WS_DOWNLOADED;
		}
	} else if (ctrlr->bp_ws == SPDK_NVME_BP_WS_DOWNLOADED) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Boot Partition Download Success\n");
		/* [한국어] download 끝 → REPLACE_BOOT_PARTITION commit 발행. */
		memset(&fw_commit, 0, sizeof(struct spdk_nvme_fw_commit));
		fw_commit.bpid = ctrlr->bpid;
		fw_commit.ca = SPDK_NVME_FW_COMMIT_REPLACE_BOOT_PARTITION;
		res = nvme_ctrlr_cmd_fw_commit(ctrlr, &fw_commit,
					       nvme_write_boot_partition_cb, ctrlr);
		if (res) {
			NVME_CTRLR_ERRLOG(ctrlr, "nvme_ctrlr_cmd_fw_commit failed!\n");
			NVME_CTRLR_ERRLOG(ctrlr, "commit action: %d\n", fw_commit.ca);
			ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, &err_cpl);
			return;
		}

		ctrlr->bp_ws = SPDK_NVME_BP_WS_REPLACE;
	} else if (ctrlr->bp_ws == SPDK_NVME_BP_WS_REPLACE) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Boot Partition Replacement Success\n");
		/* [한국어] REPLACE 끝 → ACTIVATE_BOOT_PARTITION commit 발행 (새 partition 활성). */
		memset(&fw_commit, 0, sizeof(struct spdk_nvme_fw_commit));
		fw_commit.bpid = ctrlr->bpid;
		fw_commit.ca = SPDK_NVME_FW_COMMIT_ACTIVATE_BOOT_PARTITION;
		res = nvme_ctrlr_cmd_fw_commit(ctrlr, &fw_commit,
					       nvme_write_boot_partition_cb, ctrlr);
		if (res) {
			NVME_CTRLR_ERRLOG(ctrlr, "nvme_ctrlr_cmd_fw_commit failed!\n");
			NVME_CTRLR_ERRLOG(ctrlr, "commit action: %d\n", fw_commit.ca);
			ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, &err_cpl);
			return;
		}

		ctrlr->bp_ws = SPDK_NVME_BP_WS_ACTIVATE;
	} else if (ctrlr->bp_ws == SPDK_NVME_BP_WS_ACTIVATE) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Boot Partition Activation Success\n");
		/* [한국어] 마지막 단계 — 사용자에게 성공 cpl 전달. */
		ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, cpl);
	} else {
		/* [한국어] bp_ws 가 예상 범위 밖 — 동시 호출 등 잘못된 사용. */
		NVME_CTRLR_ERRLOG(ctrlr, "Invalid Boot Partition write state\n");
		ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, &err_cpl);
		return;
	}
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_write_boot_partition -
 *   Boot Partition WRITE 비동기 발행. download → commit(REPLACE) → commit(ACTIVATE)
 *   3단계가 콜백 chain 으로 자동 진행.
 *
 * @ctrlr: 대상 컨트롤러.
 * @payload: 새 boot partition 이미지의 host buffer.
 * @size: 이미지 크기.
 * @bpid: 대상 Boot Partition ID (0 or 1).
 * @cb_fn: 최종 완료 (성공/실패) 콜백.
 * @cb_arg: 콜백 인자.
 * @return: 0 = 첫 download admin 발행 성공, 음수 = 즉시 실패.
 *
 * 동작:
 *   1. CAP.BPS=0 면 boot partition 미지원 → -ENOTSUP.
 *   2. ctrlr 의 bp_* state 필드 초기화 (DOWNLOADING + 진행 변수들).
 *   3. 첫 fw_image_download (min_page_size 또는 전체) 발행 — 콜백 chain 이 이어 받음.
 *
 * 비동기 모델: 호출자는 즉시 반환받고, 실제 진행은 nvme_write_boot_partition_cb 가
 *   상태 변수 (bp_ws, fw_offset, fw_size_remaining) 보고 단계 진행. 사용자 콜백은
 *   최종 1회만 호출 (성공 시 ACTIVATE 끝, 실패 시 발생 시점).
 *
 * 호출 체인:
 *   사용자 → [본 함수] (첫 download)
 *     → nvme_write_boot_partition_cb (DOWNLOADING → DOWNLOADED → REPLACE → ACTIVATE)
 *     → 사용자 cb_fn (1회)
 */
int
spdk_nvme_ctrlr_write_boot_partition(struct spdk_nvme_ctrlr *ctrlr,
				     void *payload, uint32_t size, uint32_t bpid,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	int res;

	if (ctrlr->cap.bits.bps == 0) {
		return -ENOTSUP;
	}

	/* [한국어] state machine 초기 상태 + per-write 진행 변수 세팅.
	 * 이 필드들은 ctrlr 에 상주 — 동시 write 금지 (state 가 단일이라). */
	ctrlr->bp_ws = SPDK_NVME_BP_WS_DOWNLOADING;
	ctrlr->bpid = bpid;
	ctrlr->bp_write_cb_fn = cb_fn;
	ctrlr->bp_write_cb_arg = cb_arg;
	ctrlr->fw_offset = 0;
	ctrlr->fw_size_remaining = size;
	ctrlr->fw_payload = payload;
	/* [한국어] 첫 chunk 크기 — 보통 min_page_size, 이미지가 그보다 작으면 size. */
	ctrlr->fw_transfer_size = spdk_min(ctrlr->fw_size_remaining, ctrlr->min_page_size);

	/* [한국어] 첫 download admin 발행 — 이후는 콜백 체인. */
	res = nvme_ctrlr_cmd_fw_image_download(ctrlr, ctrlr->fw_transfer_size, ctrlr->fw_offset,
					       ctrlr->fw_payload, nvme_write_boot_partition_cb, ctrlr);

	return res;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_is_discovery - NVMe-oF Discovery Controller 여부 판정 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return true = discovery controller, false = 일반 IO controller
 *
 * NVMe-oF Discovery Subsystem: subnqn 이 표준 NQN "nqn.2014-08.org.nvmexpress.discovery"
 * 으로 시작하는 특수 컨트롤러. 사용자가 connect 후 Discovery Log Page (0x70) 만 read 가능하며
 * 일반 IO 명령은 거부됨. SPDK 가 NVMe-oF target 의 가용 subsystem 목록을 조회할 때 사용.
 *
 * 동작: subnqn 의 앞쪽 길이가 표준 discovery NQN 과 일치하는지 prefix 비교.
 * PCIe 컨트롤러는 subnqn 이 빈 문자열이라 항상 false.
 */
bool
spdk_nvme_ctrlr_is_discovery(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr);                /* [한국어] NULL 인자 방어 — debug 빌드 즉시 abort. */

	return !strncmp(ctrlr->trid.subnqn, SPDK_NVMF_DISCOVERY_NQN,
			strlen(SPDK_NVMF_DISCOVERY_NQN));
	                                  /* [한국어] subnqn prefix 매치 — strncmp == 0 면 discovery NQN 으로 시작.
	                                   *         일반 IO subsystem 은 사용자 정의 NQN ("nqn.<date>.<...>") 사용. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_is_fabrics - NVMe-oF Fabrics 컨트롤러 여부 판정 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return true = Fabrics(RDMA/TCP/FC), false = PCIe
 *
 * 트랜스포트 종류로 분기: PCIe 는 BAR MMIO 직접 접근, Fabrics 는 Property Get/Set 명령 경유.
 * 호출자(bdev_nvme, 진단 코드 등)가 트랜스포트별 분기 처리 시 사용.
 *
 * 동작: trid.trtype 을 spdk_nvme_trtype_is_fabrics 헬퍼로 분류.
 *   - PCIe(=0): false
 *   - RDMA(=1), FC(=2), TCP(=3), VFIOUSER(=4): true (VFIOUSER 도 NVMe-oF 처럼 동작)
 *   - CUSTOM: 트랜스포트 등록 시 명시
 */
bool
spdk_nvme_ctrlr_is_fabrics(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr);

	return spdk_nvme_trtype_is_fabrics(ctrlr->trid.trtype);
	                                  /* [한국어] trtype 분류 함수 호출 — PCIe vs 그 외 분기. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_security_receive -
 *   Security Receive (opcode 0x82) admin 명령 동기 발행. SPC-4 / TCG Opal SED 의
 *   "device → host" 보안 데이터 전송 (challenge, response 등).
 *
 * @ctrlr: 대상 컨트롤러.
 * @secp: Security Protocol (SPC-4 §7.7). 0x01=TCG, 0x02=SPDM, 0xEF=IEEE 1667 등.
 * @spsp: Security Protocol Specific (16-bit). 프로토콜별 의미 (Opal 의 ComID 등).
 * @nssf: NVMe Security Specific Field (NVMe 1.4+). 0 = unused.
 * @payload: 응답 데이터 받을 host buffer. NVMe 컨트롤러가 DMA write.
 * @size: 버퍼 크기 (bytes).
 * @return: 0 성공 + payload 채워짐, 음수 errno.
 *
 * Security Send/Receive 는 SPC-4 의 SECURITY PROTOCOL IN/OUT 의 NVMe 매핑.
 * Opal SED 의 unlock challenge-response, TCG Storage 명령 등 모든 보안 기능이
 * 이 두 admin 으로 처리됨 (별도 NVMe-specific 명령 없음).
 *
 * 호출 체인:
 *   사용자/Opal 라이브러리 → [본 함수]
 *     → spdk_nvme_ctrlr_cmd_security_receive (opcode 0x82, dword 10 = SECP|SPSP|NSSF, dword 11 = size)
 *     → nvme_wait_for_adminq_completion (동기 polling)
 */
int
spdk_nvme_ctrlr_security_receive(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
				 uint16_t spsp, uint8_t nssf, void *payload, size_t size)
{
	struct nvme_completion_poll_status	*status;
	int					res;

	/* [한국어] status tracker heap 할당 — format / firmware 와 동일 패턴. */
	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	/* [한국어] Security Receive 발행 — 비동기. cmd 빌더가 SECP/SPSP/NSSF 를 dword 10/11 에 패킹. */
	res = spdk_nvme_ctrlr_cmd_security_receive(ctrlr, secp, spsp, nssf, payload, size,
			nvme_completion_poll_cb, status);
	if (res) {
		free(status);
		return res;
	}

	/* [한국어] true = command status 검사 위임 — status->cpl.status 의 SCT/SC 가 0 이어야 성공. */
	res = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (res) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for spdk_nvme_ctrlr_cmd_security_receive failed: rc=%s\n",
				  spdk_strerror(abs(res)));
	}

	return res;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_security_send -
 *   Security Send (opcode 0x81) admin 명령 동기 발행. "host → device" 보안 데이터 전송.
 *
 * @ctrlr: 대상 컨트롤러.
 * @secp: Security Protocol (security_receive 와 동일 의미).
 * @spsp: Security Protocol Specific.
 * @nssf: NVMe Security Specific Field.
 * @payload: 전송할 데이터 (host buffer). NVMe 컨트롤러가 DMA read.
 * @size: 데이터 크기.
 * @return: 0 성공, 음수 errno.
 *
 * Security Send 의 페어 = Security Receive. Opal SED 의 명령 시퀀스는 보통
 * Send (command) → Receive (response) 의 RPC 패턴.
 *
 * receive 와의 유일한 차이: DMA 방향 (host→device). nvme_ctrlr_cmd_security_send
 * 가 PRP/SGL 설정을 SLBA(Send) opcode 에 맞게 구성.
 *
 * 호출 체인:
 *   사용자/Opal 라이브러리 → [본 함수]
 *     → spdk_nvme_ctrlr_cmd_security_send (opcode 0x81)
 *     → nvme_wait_for_adminq_completion
 */
int
spdk_nvme_ctrlr_security_send(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
			      uint16_t spsp, uint8_t nssf, void *payload, size_t size)
{
	struct nvme_completion_poll_status	*status;
	int					res;

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	/* [한국어] Security Send 발행 — host→device DMA. */
	res = spdk_nvme_ctrlr_cmd_security_send(ctrlr, secp, spsp, nssf, payload, size,
						nvme_completion_poll_cb,
						status);
	if (res) {
		free(status);
		return res;
	}

	res = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (res) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_cmd_security_send failed: rc=%s\n",
				  spdk_strerror(abs(res)));
	}

	return res;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_flags - 컨트롤러의 SPDK 내부 capability flag 비트마스크 반환 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return ctrlr->flags 의 직접 사본 (enum spdk_nvme_ctrlr_flags 비트들 — COMPARE_AND_WRITE_SUPPORTED,
 *  SGL_SUPPORTED, SECURITY_SEND_RECV_SUPPORTED, DIRECTIVES_SUPPORTED 등 NVMe 확장 기능 지원 여부).
 *
 * 이 flag 비트들은 nvme_ctrlr_process_init 진행 중 Identify Controller / Get Features
 * 응답을 분석하여 SPDK 가 채워둔다. 사용자(예: bdev_nvme)가 "이 컨트롤러가 SGL 사용 가능한가?"
 * 같은 분기 판단 시 호출.
 *
 * 동기화: read-only, ctrlr_lock 불필요 (READY 이후엔 변경 없음).
 *
 * 호출 체인:
 *   bdev_nvme / 사용자 → [본 함수] → ctrlr->flags (단순 read)
 */
uint64_t
spdk_nvme_ctrlr_get_flags(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->flags;
                                  /* [한국어] flags 비트마스크 직접 반환 — 잠금 없는 단순 read. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_transport_id - 컨트롤러의 트랜스포트 식별자(trid) 포인터 반환 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return &ctrlr->trid (struct spdk_nvme_transport_id) — trtype/traddr/trsvcid/subnqn 등.
 *
 * 사용자가 컨트롤러의 주소 정보를 원할 때 호출. 반환 포인터는 ctrlr 내부 메모리를 가리키므로
 * ctrlr 수명 동안만 유효 (detach 후 사용 금지).
 *
 * PCIe: trid.trtype=PCIE, traddr="0000:01:00.0" (BDF 문자열)
 * NVMe-oF: trid.trtype=RDMA/TCP/FC, traddr="192.168.1.10", trsvcid="4420", subnqn=...
 *
 * 호출 체인:
 *   RPC handler / bdev_nvme 진단 → [본 함수] → 사용자 표시/로깅
 */
const struct spdk_nvme_transport_id *
spdk_nvme_ctrlr_get_transport_id(struct spdk_nvme_ctrlr *ctrlr)
{
	return &ctrlr->trid;          /* [한국어] trid 포인터 그대로 노출 — 외부에서 read-only로 사용 */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_alloc_qid - I/O queue ID 하나를 free_io_qids 비트맵에서 할당 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @return 할당된 qid (1 ~ opts.num_io_queues), 가용 qid 없으면 -1
 *
 * NVMe 스펙: qid 0 = admin queue (예약), 1~num_io_queues = I/O queue.
 * free_io_qids 는 ctrlr_construct 에서 bit_array_create(opts.num_io_queues + 1) 로 생성되어
 * qid 1..N 비트가 1(=free)로 초기화됨. find_first_set(.. ,1) 로 첫 1 비트(=가용 qid) 검색.
 *
 * 동기화: ctrlr_lock (multi-process / multi-thread 동시 alloc 방지 — 동일 qid 중복 할당 금지).
 *
 * 호출 체인:
 *   spdk_nvme_ctrlr_alloc_io_qpair → [본 함수] → qid 결정
 *     → nvme_transport_ctrlr_create_io_qpair (해당 qid 로 SQ/CQ 생성)
 */
int32_t
spdk_nvme_ctrlr_alloc_qid(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t qid;                 /* [한국어] 할당될 qid 임시 저장 */

	assert(ctrlr->free_io_qids);  /* [한국어] free_io_qids 비트맵이 ctrlr_construct 에서 생성되었어야 함 */
	nvme_ctrlr_lock(ctrlr);       /* [한국어] 비트맵 변경 직렬화 — robust mutex */
	qid = spdk_bit_array_find_first_set(ctrlr->free_io_qids, 1);
	                                  /* [한국어] qid 1 부터 검색 (qid 0 은 admin 으로 예약) — 첫 1 비트 위치 반환.
	                                   *         가용 없으면 UINT32_MAX 반환. */
	if (qid > ctrlr->opts.num_io_queues) {
	                                  /* [한국어] num_io_queues 초과 = 가용 qid 없음 (UINT32_MAX 또는 옵션 초과). */
		NVME_CTRLR_ERRLOG(ctrlr, "No free I/O queue IDs\n");
		nvme_ctrlr_unlock(ctrlr);
		return -1;
	}

	spdk_bit_array_clear(ctrlr->free_io_qids, qid);
	                                  /* [한국어] 비트 0 으로 — "사용 중" 표시. 이후 free_qid 가 다시 1 로. */
	nvme_ctrlr_unlock(ctrlr);
	return qid;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_free_qid - I/O queue ID 를 free_io_qids 비트맵으로 반납 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @qid: 반납할 queue ID (1 ~ opts.num_io_queues)
 *
 * spdk_nvme_ctrlr_free_io_qpair 등에서 qpair 자원 해제 시 호출. 이후 같은 qid 가 다른
 * alloc 요청에 재사용된다.
 *
 * 안전 처리: free_io_qids 가 NULL 일 수 있음 (컨트롤러 destruct 진행 중) — 그 경우 무시.
 *
 * 동기화: ctrlr_lock — alloc 과 짝.
 */
void
spdk_nvme_ctrlr_free_qid(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid)
{
	assert(qid <= ctrlr->opts.num_io_queues);
	                                  /* [한국어] 잘못된 qid 반납 방지 — 디버그 빌드 assert. */

	nvme_ctrlr_lock(ctrlr);       /* [한국어] 비트맵 변경 직렬화 */

	if (spdk_likely(ctrlr->free_io_qids)) {
	                                  /* [한국어] destruct 중이면 free_io_qids 가 NULL 가능 — race 안전. */
		spdk_bit_array_set(ctrlr->free_io_qids, qid);
		                                  /* [한국어] 비트 1 = free 표시. 이후 alloc_qid 에서 재발견. */
	}

	nvme_ctrlr_unlock(ctrlr);
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_memory_domains - 트랜스포트가 지원하는 memory domain 목록 조회 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @domains: [out] domain 포인터 배열
 * @array_size: 배열 크기 (입력)
 * @return 실제 도메인 수 (배열 크기 초과 시 array_size 까지만 채움). 음수면 에러.
 *
 * Memory domain: SPDK 추상화 — DMA 가능한 메모리의 종류 식별 (RDMA PD, GPU memory 등).
 * bdev_nvme 가 "이 컨트롤러가 GPU 메모리로 직접 DMA 가능한가?" 같은 판단에 사용.
 *
 * 동작: 단순 트랜스포트 vtable 디스패치 — 트랜스포트별 구현이 도메인 enumeration 수행.
 *  - PCIe: 일반 시스템 메모리 도메인만 (보통 1개)
 *  - RDMA: ibv_pd 별로 1개씩
 *  - TCP: 시스템 메모리만
 *
 * 호출 체인:
 *   bdev_nvme_get_memory_domains → [본 함수] → nvme_transport_ctrlr_get_memory_domains
 */
int
spdk_nvme_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_memory_domain **domains, int array_size)
{
	return nvme_transport_ctrlr_get_memory_domains(ctrlr, domains, array_size);
	                                  /* [한국어] 트랜스포트별 vtable 호출 — PCIe/RDMA/TCP 각자 enumeration. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_authenticate - 컨트롤러 admin qpair 에 대한 DH-HMAC-CHAP 인증 시작 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러
 * @cb_fn: 인증 완료 콜백
 * @cb_ctx: 콜백 컨텍스트
 * @return 0 = 인증 시작 성공 (이후 비동기 진행), 음수 errno
 *
 * NVMe-oF (NVMe 2.0) in-band authentication: 연결된 qpair 위에서 challenge-response 인증을
 * 수행하여 host/controller 의 정당성 검증. PCIe 에서는 일반적으로 무의미 (인증 불필요).
 *
 * 동작:
 *   - admin qpair (ctrlr->adminq) 에 대해 spdk_nvme_qpair_authenticate 위임
 *   - 내부에서 auth state machine 시작 (NEGOTIATE → AWAIT_CHALLENGE → ... → DONE)
 *   - 진행은 spdk_nvme_qpair_process_completions 폴링 시 한 단계씩
 *   - 완료 시 cb_fn 호출 (성공: status=0, 실패: status=음수 errno)
 *
 * 사용처: NVMe-oF discovery 후 첫 connect 시 controller 가 auth required 응답하면 호출.
 *
 * 호출 체인:
 *   사용자 / bdev_nvme → [본 함수] → spdk_nvme_qpair_authenticate(adminq, ...)
 *     → nvme_fabric_qpair_authenticate_async
 */
int
spdk_nvme_ctrlr_authenticate(struct spdk_nvme_ctrlr *ctrlr,
			     spdk_nvme_authenticate_cb cb_fn, void *cb_ctx)
{
	return spdk_nvme_qpair_authenticate(ctrlr->adminq, cb_fn, cb_ctx);
	                                  /* [한국어] admin qpair (qid=0) 에 인증 위임 — 인증 자체는 qpair-level 동작.
	                                   *         I/O qpair 도 별도 인증 필요한 경우 spdk_nvme_qpair_authenticate 직접 호출. */
}
