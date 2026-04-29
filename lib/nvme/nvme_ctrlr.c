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

int
nvme_ctrlr_set_bpmbl(struct spdk_nvme_ctrlr *ctrlr, uint64_t bpmbl_value)
{
	return nvme_transport_ctrlr_set_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, bpmbl),
					      bpmbl_value);
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

struct intel_log_pages_ctx {
	struct spdk_nvme_intel_log_page_directory log_page_directory;
	struct spdk_nvme_ctrlr *ctrlr;
};

static void
nvme_ctrlr_set_intel_support_log_pages_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct intel_log_pages_ctx *ctx = arg;
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;

	if (!spdk_nvme_cpl_is_error(cpl)) {
		nvme_ctrlr_construct_intel_support_log_page_list(ctrlr, &ctx->log_page_directory);
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES,
			     ctrlr->opts.admin_timeout_ms);
	free(ctx);
}

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

static int
nvme_ctrlr_update_ns_ana_states(const struct spdk_nvme_ana_group_descriptor *desc,
				void *cb_arg)
{
	struct spdk_nvme_ctrlr *ctrlr = cb_arg;
	struct spdk_nvme_ns *ns;
	uint32_t i, nsid;

	for (i = 0; i < desc->num_of_nsid; i++) {
		nsid = desc->nsid[i];
		if (nsid == 0 || nsid > ctrlr->cdata.nn) {
			continue;
		}

		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		assert(ns != NULL);

		ns->ana_group_id = desc->ana_group_id;
		ns->ana_state = desc->ana_state;
	}

	return 0;
}

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

static void
nvme_ctrlr_shutdown_set_cc_done(void *_ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr_detach_ctx *ctx = _ctx;
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to write CC.SHN\n");
		ctx->shutdown_complete = true;
		return;
	}

	if (ctrlr->opts.no_shn_notification) {
		ctx->shutdown_complete = true;
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
	NVME_CTRLR_DEBUGLOG(ctrlr, "RTD3E = %" PRIu32 " us\n", ctrlr->cdata.rtd3e);
	ctx->shutdown_timeout_ms = SPDK_CEIL_DIV(ctrlr->cdata.rtd3e, 1000);
	ctx->shutdown_timeout_ms = spdk_max(ctx->shutdown_timeout_ms, 10000);
	NVME_CTRLR_DEBUGLOG(ctrlr, "shutdown timeout = %" PRIu32 " ms\n", ctx->shutdown_timeout_ms);

	ctx->shutdown_start_tsc = spdk_get_ticks();
	ctx->state = NVME_CTRLR_DETACH_CHECK_CSTS;
}

static void
nvme_ctrlr_shutdown_get_cc_done(void *_ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr_detach_ctx *ctx = _ctx;
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;
	union spdk_nvme_cc_register cc;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CC register\n");
		ctx->shutdown_complete = true;
		return;
	}

	assert(value <= UINT32_MAX);
	cc.raw = (uint32_t)value;

	if (ctrlr->opts.no_shn_notification) {
		NVME_CTRLR_INFOLOG(ctrlr, "Disable SSD without shutdown notification\n");
		if (cc.bits.en == 0) {
			ctx->shutdown_complete = true;
			return;
		}

		cc.bits.en = 0;
	} else {
		cc.bits.shn = SPDK_NVME_SHN_NORMAL;
	}

	rc = nvme_ctrlr_set_cc_async(ctrlr, cc.raw, nvme_ctrlr_shutdown_set_cc_done, ctx);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to write CC.SHN\n");
		ctx->shutdown_complete = true;
	}
}

static void
nvme_ctrlr_shutdown_async(struct spdk_nvme_ctrlr *ctrlr,
			  struct nvme_ctrlr_detach_ctx *ctx)
{
	int rc;

	if (ctrlr->is_removed) {
		ctx->shutdown_complete = true;
		return;
	}

	if (ctrlr->adminq == NULL ||
	    ctrlr->adminq->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE) {
		NVME_CTRLR_INFOLOG(ctrlr, "Adminq is not connected.\n");
		ctx->shutdown_complete = true;
		return;
	}

	ctx->state = NVME_CTRLR_DETACH_SET_CC;
	rc = nvme_ctrlr_get_cc_async(ctrlr, nvme_ctrlr_shutdown_get_cc_done, ctx);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CC register\n");
		ctx->shutdown_complete = true;
	}
}

static void
nvme_ctrlr_shutdown_get_csts_done(void *_ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr_detach_ctx *ctx = _ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctx->ctrlr, "Failed to read the CSTS register\n");
		ctx->shutdown_complete = true;
		return;
	}

	assert(value <= UINT32_MAX);
	ctx->csts.raw = (uint32_t)value;
	ctx->state = NVME_CTRLR_DETACH_GET_CSTS_DONE;
}

static int
nvme_ctrlr_shutdown_poll_async(struct spdk_nvme_ctrlr *ctrlr,
			       struct nvme_ctrlr_detach_ctx *ctx)
{
	union spdk_nvme_csts_register	csts;
	uint32_t			ms_waited;

	switch (ctx->state) {
	case NVME_CTRLR_DETACH_SET_CC:
	case NVME_CTRLR_DETACH_GET_CSTS:
		/* We're still waiting for the register operation to complete */
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		return -EAGAIN;

	case NVME_CTRLR_DETACH_CHECK_CSTS:
		ctx->state = NVME_CTRLR_DETACH_GET_CSTS;
		if (nvme_ctrlr_get_csts_async(ctrlr, nvme_ctrlr_shutdown_get_csts_done, ctx)) {
			NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CSTS register\n");
			return -EIO;
		}
		return -EAGAIN;

	case NVME_CTRLR_DETACH_GET_CSTS_DONE:
		ctx->state = NVME_CTRLR_DETACH_CHECK_CSTS;
		break;

	default:
		assert(0 && "Should never happen");
		return -EINVAL;
	}

	ms_waited = (spdk_get_ticks() - ctx->shutdown_start_tsc) * 1000 / spdk_get_ticks_hz();
	csts.raw = ctx->csts.raw;

	if (csts.bits.shst == SPDK_NVME_SHST_COMPLETE) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "shutdown complete in %u milliseconds\n", ms_waited);
		return 0;
	}

	if (ms_waited < ctx->shutdown_timeout_ms) {
		return -EAGAIN;
	}

	NVME_CTRLR_ERRLOG(ctrlr, "did not shutdown within %u milliseconds\n",
			  ctx->shutdown_timeout_ms);
	if (ctrlr->quirks & NVME_QUIRK_SHST_COMPLETE) {
		NVME_CTRLR_ERRLOG(ctrlr, "likely due to shutdown handling in the VMWare emulated NVMe SSD\n");
	}

	return 0;
}

static inline uint64_t
nvme_ctrlr_get_ready_timeout(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cap.bits.to * 500;
}

static void
nvme_ctrlr_set_cc_en_done(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to set the CC register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1,
			     nvme_ctrlr_get_ready_timeout(ctrlr));
}

static int
nvme_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register	cc;
	int				rc;

	rc = nvme_transport_ctrlr_enable(ctrlr);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "transport ctrlr_enable failed\n");
		return rc;
	}

	cc.raw = ctrlr->process_init_cc.raw;
	if (cc.bits.en != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "called with CC.EN = 1\n");
		return -EINVAL;
	}

	cc.bits.en = 1;
	cc.bits.css = 0;
	cc.bits.shn = 0;
	cc.bits.iosqes = 6; /* SQ entry size == 64 == 2^6 */
	cc.bits.iocqes = 4; /* CQ entry size == 16 == 2^4 */

	/* Page size is 2 ^ (12 + mps). */
	cc.bits.mps = spdk_u32log2(ctrlr->page_size) - 12;

	/*
	 * Since NVMe 1.0, a controller should have at least one bit set in CAP.CSS.
	 * A controller that does not have any bit set in CAP.CSS is not spec compliant.
	 * Try to support such a controller regardless.
	 */
	if (ctrlr->cap.bits.css == 0) {
		NVME_CTRLR_INFOLOG(ctrlr, "Drive reports no command sets supported. Assuming NVM is supported.\n");
		ctrlr->cap.bits.css = SPDK_NVME_CAP_CSS_NVM;
	}

	/*
	 * If the user did not explicitly request a command set, or supplied a value larger than
	 * what can be saved in CC.CSS, use the most reasonable default.
	 */
	if (ctrlr->opts.command_set >= CHAR_BIT) {
		if (ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_IOCS) {
			ctrlr->opts.command_set = SPDK_NVME_CC_CSS_IOCS;
		} else if (ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_NVM) {
			ctrlr->opts.command_set = SPDK_NVME_CC_CSS_NVM;
		} else if (ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_NOIO) {
			/* Technically we should respond with CC_CSS_NOIO in
			 * this case, but we use NVM instead to work around
			 * buggy targets and to match Linux driver behavior.
			 */
			ctrlr->opts.command_set = SPDK_NVME_CC_CSS_NVM;
		} else {
			/* Invalid supported bits detected, falling back to NVM. */
			ctrlr->opts.command_set = SPDK_NVME_CC_CSS_NVM;
		}
	}

	/* Verify that the selected command set is supported by the controller. */
	if (!(ctrlr->cap.bits.css & (1u << ctrlr->opts.command_set))) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Requested I/O command set %u but supported mask is 0x%x\n",
				    ctrlr->opts.command_set, ctrlr->cap.bits.css);
		NVME_CTRLR_DEBUGLOG(ctrlr, "Falling back to NVM. Assuming NVM is supported.\n");
		ctrlr->opts.command_set = SPDK_NVME_CC_CSS_NVM;
	}

	cc.bits.css = ctrlr->opts.command_set;

	switch (ctrlr->opts.arb_mechanism) {
	case SPDK_NVME_CC_AMS_RR:
		break;
	case SPDK_NVME_CC_AMS_WRR:
		if (SPDK_NVME_CAP_AMS_WRR & ctrlr->cap.bits.ams) {
			break;
		}
		return -EINVAL;
	case SPDK_NVME_CC_AMS_VS:
		if (SPDK_NVME_CAP_AMS_VS & ctrlr->cap.bits.ams) {
			break;
		}
		return -EINVAL;
	default:
		return -EINVAL;
	}

	cc.bits.ams = ctrlr->opts.arb_mechanism;
	ctrlr->process_init_cc.raw = cc.raw;

	if (nvme_ctrlr_set_cc_async(ctrlr, cc.raw, nvme_ctrlr_set_cc_en_done, ctrlr)) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_cc() failed\n");
		return -EIO;
	}

	return 0;
}

/*
 * [한국어]
 * nvme_ctrlr_state_string - controller 상태머신의 모든 상태(40+)를 사람용 문자열로 변환
 *
 * @state: nvme_ctrlr_state enum 값
 * @return 상태 이름 문자열 (NULL 반환 안 함, 미정의 enum은 "unknown").
 *
 * 사용처: 디버그 로그 (NVME_CTRLR_DEBUGLOG의 "setting state to %s") + 에러 메시지.
 *
 * 상태 그룹 분류:
 *   [INIT 단계]: INIT_DELAY → CONNECT_ADMINQ → WAIT_FOR_CONNECT_ADMINQ → READ_VS → READ_CAP
 *               → CHECK_EN (CC.EN 현재 값 검사)
 *   [DISABLE 단계]: SET_EN_0 → DISABLE_WAIT_FOR_READY_0 → DISABLED
 *   [ENABLE 단계]: ENABLE → ENABLE_WAIT_FOR_READY_1 → RESET_ADMIN_QUEUE
 *   [IDENTIFY 단계]: IDENTIFY → CONFIGURE_AER → SET_KEEP_ALIVE_TIMEOUT → IDENTIFY_IOCS_SPECIFIC
 *                   → GET_ZNS_CMD_EFFECTS_LOG → SET_NUM_QUEUES
 *   [NS DISCOVERY]: IDENTIFY_ACTIVE_NS → IDENTIFY_NS → IDENTIFY_ID_DESCS → IDENTIFY_NS_IOCS_SPECIFIC
 *   [FEATURES 단계]: SET_SUPPORTED_LOG_PAGES → SET_SUPPORTED_INTEL_LOG_PAGES → SET_SUPPORTED_FEATURES
 *                   → SET_HOST_FEATURE → SET_DB_BUF_CFG → SET_HOST_ID
 *   [최종]: TRANSPORT_READY → READY (정상) / ERROR / DISCONNECTED
 *
 * "WAIT_FOR_*" 패턴: 비동기 admin/register 명령을 발행한 후 응답 대기 중인 상태.
 *                    응답 도착 시 callback이 다음 상태로 전이.
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
	case NVME_CTRLR_STATE_READ_VS:
		return "read vs";
	case NVME_CTRLR_STATE_READ_VS_WAIT_FOR_VS:
		return "read vs wait for vs";
	case NVME_CTRLR_STATE_READ_CAP:
		return "read cap";
	case NVME_CTRLR_STATE_READ_CAP_WAIT_FOR_CAP:
		return "read cap wait for cap";
	case NVME_CTRLR_STATE_CHECK_EN:
		return "check en";
	case NVME_CTRLR_STATE_CHECK_EN_WAIT_FOR_CC:
		return "check en wait for cc";
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1:
		return "disable and wait for CSTS.RDY = 1";
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS:
		return "disable and wait for CSTS.RDY = 1 reg";
	case NVME_CTRLR_STATE_SET_EN_0:
		return "set CC.EN = 0";
	case NVME_CTRLR_STATE_SET_EN_0_WAIT_FOR_CC:
		return "set CC.EN = 0 wait for cc";
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0:
		return "disable and wait for CSTS.RDY = 0";
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0_WAIT_FOR_CSTS:
		return "disable and wait for CSTS.RDY = 0 reg";
	case NVME_CTRLR_STATE_DISABLED:
		return "controller is disabled";
	case NVME_CTRLR_STATE_ENABLE:
		return "enable controller by writing CC.EN = 1";
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_CC:
		return "enable controller by writing CC.EN = 1 reg";
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1:
		return "wait for CSTS.RDY = 1";
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS:
		return "wait for CSTS.RDY = 1 reg";
	case NVME_CTRLR_STATE_RESET_ADMIN_QUEUE:
		return "reset admin queue";
	case NVME_CTRLR_STATE_IDENTIFY:
		return "identify controller";
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY:
		return "wait for identify controller";
	case NVME_CTRLR_STATE_CONFIGURE_AER:
		return "configure AER";
	case NVME_CTRLR_STATE_WAIT_FOR_CONFIGURE_AER:
		return "wait for configure aer";
	case NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT:
		return "set keep alive timeout";
	case NVME_CTRLR_STATE_WAIT_FOR_KEEP_ALIVE_TIMEOUT:
		return "wait for set keep alive timeout";
	case NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC:
		return "identify controller iocs specific";
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_IOCS_SPECIFIC:
		return "wait for identify controller iocs specific";
	case NVME_CTRLR_STATE_GET_ZNS_CMD_EFFECTS_LOG:
		return "get zns cmd and effects log page";
	case NVME_CTRLR_STATE_WAIT_FOR_GET_ZNS_CMD_EFFECTS_LOG:
		return "wait for get zns cmd and effects log page";
	case NVME_CTRLR_STATE_SET_NUM_QUEUES:
		return "set number of queues";
	case NVME_CTRLR_STATE_WAIT_FOR_SET_NUM_QUEUES:
		return "wait for set number of queues";
	case NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS:
		return "identify active ns";
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ACTIVE_NS:
		return "wait for identify active ns";
	case NVME_CTRLR_STATE_IDENTIFY_NS:
		return "identify ns";
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS:
		return "wait for identify ns";
	case NVME_CTRLR_STATE_IDENTIFY_ID_DESCS:
		return "identify namespace id descriptors";
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ID_DESCS:
		return "wait for identify namespace id descriptors";
	case NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC:
		return "identify ns iocs specific";
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS_IOCS_SPECIFIC:
		return "wait for identify ns iocs specific";
	case NVME_CTRLR_STATE_SET_SUPPORTED_LOG_PAGES:
		return "set supported log pages";
	case NVME_CTRLR_STATE_SET_SUPPORTED_INTEL_LOG_PAGES:
		return "set supported INTEL log pages";
	case NVME_CTRLR_STATE_WAIT_FOR_SUPPORTED_INTEL_LOG_PAGES:
		return "wait for supported INTEL log pages";
	case NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES:
		return "set supported features";
	case NVME_CTRLR_STATE_SET_HOST_FEATURE:
		return "set host behavior support feature";
	case NVME_CTRLR_STATE_WAIT_FOR_SET_HOST_FEATURE:
		return "wait for set host behavior support feature";
	case NVME_CTRLR_STATE_SET_DB_BUF_CFG:
		return "set doorbell buffer config";
	case NVME_CTRLR_STATE_WAIT_FOR_DB_BUF_CFG:
		return "wait for doorbell buffer config";
	case NVME_CTRLR_STATE_SET_HOST_ID:
		return "set host ID";
	case NVME_CTRLR_STATE_WAIT_FOR_HOST_ID:
		return "wait for set host ID";
	case NVME_CTRLR_STATE_TRANSPORT_READY:
		return "transport ready";
	case NVME_CTRLR_STATE_READY:
		return "ready";
	case NVME_CTRLR_STATE_ERROR:
		return "error";
	case NVME_CTRLR_STATE_DISCONNECTED:
		return "disconnected";
	}
	return "unknown";
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
 * nvme_ctrlr_set_state - 상태 전이 + timeout (디버그 로그 출력)
 */
static void
nvme_ctrlr_set_state(struct spdk_nvme_ctrlr *ctrlr, enum nvme_ctrlr_state state,
		     uint64_t timeout_in_ms)
{
	_nvme_ctrlr_set_state(ctrlr, state, timeout_in_ms, false);
                                  /* [한국어] quiet=false — 모든 전이를 로그로 기록 (bring-up 추적용) */
}

/*
 * [한국어]
 * nvme_ctrlr_set_state_quiet - 상태 전이 + timeout (로그 없음)
 *
 * 사용처: 같은 상태 머무르며 반복 polling 진입 시 (예: WAIT_FOR_READY_1 폴링) — 로그 폭주 방지.
 */
static void
nvme_ctrlr_set_state_quiet(struct spdk_nvme_ctrlr *ctrlr, enum nvme_ctrlr_state state,
			   uint64_t timeout_in_ms)
{
	_nvme_ctrlr_set_state(ctrlr, state, timeout_in_ms, true);
                                  /* [한국어] quiet=true — 같은 상태 반복 진입 시에도 로그 한 번만 (가독성 유지) */
}

/*
 * [한국어]
 * nvme_ctrlr_free_zns_specific_data - ZNS Identify 데이터 해제
 *
 * IDENTIFY_IOCS_SPECIFIC 단계에서 할당된 cdata_zns(ZNS-specific identify controller data) 해제.
 * destruct 시 호출.
 */
static void
nvme_ctrlr_free_zns_specific_data(struct spdk_nvme_ctrlr *ctrlr)
{
	spdk_free(ctrlr->cdata_zns);
                                  /* [한국어] NULL-safe spdk_free — DMA-capable hugepage 메모리 반환 */
	ctrlr->cdata_zns = NULL;
                                  /* [한국어] dangling 방지 */
}

/*
 * [한국어]
 * nvme_ctrlr_free_iocs_specific_data - 모든 IOCS-specific identify 데이터 해제
 *
 * 현재는 ZNS만 지원 — 향후 다른 IOCS(KV 등) 추가 시 여기에 cleanup 추가.
 */
static void
nvme_ctrlr_free_iocs_specific_data(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_ctrlr_free_zns_specific_data(ctrlr);
                                  /* [한국어] ZNS만 위임 — 다른 IOCS 추가 시 여기에 함께 호출 */
}

/*
 * [한국어]
 * nvme_ctrlr_free_doorbell_buffer - shadow doorbell 버퍼 해제
 *
 * NVMe 1.3+ doorbell buffer config 기능이 활성화된 경우 shadow doorbell이 hugepage에 할당됨.
 * destruct 시 해제.
 *
 * Shadow doorbell: 호스트가 매번 MMIO doorbell write 대신 RAM의 shadow value만 갱신하면
 *                  controller가 polling으로 인지 → MMIO 비용 절감 (특히 가상화 환경).
 */
static void
nvme_ctrlr_free_doorbell_buffer(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->shadow_doorbell) {
                                  /* [한국어] doorbell buffer config 활성화된 경우만 해제 */
		spdk_free(ctrlr->shadow_doorbell);
		ctrlr->shadow_doorbell = NULL;
	}

	if (ctrlr->eventidx) {
		spdk_free(ctrlr->eventidx);
		ctrlr->eventidx = NULL;
	}
}

static void
nvme_ctrlr_set_doorbell_buffer_config_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_WARNLOG(ctrlr, "Doorbell buffer config failed\n");
	} else {
		NVME_CTRLR_INFOLOG(ctrlr, "Doorbell buffer config enabled\n");
	}
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_HOST_ID,
			     ctrlr->opts.admin_timeout_ms);
}

static int
nvme_ctrlr_set_doorbell_buffer_config(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	uint64_t prp1, prp2, len;

	if (!ctrlr->cdata.oacs.dbcs) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_HOST_ID,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_HOST_ID,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	/* only 1 page size for doorbell buffer */
	ctrlr->shadow_doorbell = spdk_zmalloc(ctrlr->page_size, ctrlr->page_size,
					      NULL, SPDK_ENV_LCORE_ID_ANY,
					      SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
	if (ctrlr->shadow_doorbell == NULL) {
		rc = -ENOMEM;
		goto error;
	}

	len = ctrlr->page_size;
	prp1 = spdk_vtophys(ctrlr->shadow_doorbell, &len);
	if (prp1 == SPDK_VTOPHYS_ERROR || len != ctrlr->page_size) {
		rc = -EFAULT;
		goto error;
	}

	ctrlr->eventidx = spdk_zmalloc(ctrlr->page_size, ctrlr->page_size,
				       NULL, SPDK_ENV_LCORE_ID_ANY,
				       SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
	if (ctrlr->eventidx == NULL) {
		rc = -ENOMEM;
		goto error;
	}

	len = ctrlr->page_size;
	prp2 = spdk_vtophys(ctrlr->eventidx, &len);
	if (prp2 == SPDK_VTOPHYS_ERROR || len != ctrlr->page_size) {
		rc = -EFAULT;
		goto error;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_DB_BUF_CFG,
			     ctrlr->opts.admin_timeout_ms);

	rc = nvme_ctrlr_cmd_doorbell_buffer_config(ctrlr, prp1, prp2,
			nvme_ctrlr_set_doorbell_buffer_config_done, ctrlr);
	if (rc != 0) {
		goto error;
	}

	return 0;

error:
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	nvme_ctrlr_free_doorbell_buffer(ctrlr);
	return rc;
}

void
nvme_ctrlr_abort_queued_aborts(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_request	*req, *tmp;
	struct spdk_nvme_cpl	cpl = {};

	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	STAILQ_FOREACH_SAFE(req, &ctrlr->queued_aborts, stailq, tmp) {
		STAILQ_REMOVE_HEAD(&ctrlr->queued_aborts, stailq);
		ctrlr->outstanding_aborts++;

		nvme_complete_request(req->cb_fn, req->cb_arg, req->qpair, req, &cpl);
	}
}

static int
nvme_ctrlr_disconnect(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->is_resetting || ctrlr->is_removed) {
		/*
		 * Controller is already resetting or has been removed. Return
		 *  immediately since there is no need to kick off another
		 *  reset in these cases.
		 */
		return ctrlr->is_resetting ? -EBUSY : -ENXIO;
	}

	ctrlr->is_resetting = true;
	ctrlr->is_failed = false;
	ctrlr->is_disconnecting = true;
	ctrlr->prepare_for_reset = true;

	NVME_CTRLR_NOTICELOG(ctrlr, "resetting controller\n");

	/* Disable keep-alive, it'll be re-enabled as part of the init process */
	ctrlr->keep_alive_interval_ticks = 0;

	/* Abort all of the queued abort requests */
	nvme_ctrlr_abort_queued_aborts(ctrlr);

	nvme_transport_admin_qpair_abort_aers(ctrlr->adminq);

	ctrlr->adminq->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
	nvme_transport_ctrlr_disconnect_qpair(ctrlr, ctrlr->adminq);

	return 0;
}

static void
nvme_ctrlr_disconnect_done(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->is_failed == false);
	ctrlr->is_disconnecting = false;

	/* Doorbell buffer config is invalid during reset */
	nvme_ctrlr_free_doorbell_buffer(ctrlr);

	/* I/O Command Set Specific Identify Controller data is invalidated during reset */
	nvme_ctrlr_free_iocs_specific_data(ctrlr);

	spdk_bit_array_free(&ctrlr->free_io_qids);

	/* Set the state back to DISCONNECTED to cause a full hardware reset. */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISCONNECTED, NVME_TIMEOUT_INFINITE);
}

int
spdk_nvme_ctrlr_disconnect(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	nvme_ctrlr_lock(ctrlr);
	rc = nvme_ctrlr_disconnect(ctrlr);
	nvme_ctrlr_unlock(ctrlr);

	return rc;
}

void
spdk_nvme_ctrlr_reconnect_async(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_ctrlr_lock(ctrlr);

	ctrlr->prepare_for_reset = false;

	/* Set the state back to INIT to cause a full hardware reset. */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT, NVME_TIMEOUT_INFINITE);

	/* Return without releasing ctrlr_lock. ctrlr_lock will be released when
	 * spdk_nvme_ctrlr_reset_poll_async() returns 0.
	 */
}

int
nvme_ctrlr_reinitialize_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	bool async;
	int rc;

	if (nvme_ctrlr_get_current_process(ctrlr) != qpair->active_proc ||
	    spdk_nvme_ctrlr_is_fabrics(ctrlr) || nvme_qpair_is_admin_queue(qpair)) {
		assert(false);
		return -EINVAL;
	}

	/* Force a synchronous connect. */
	async = qpair->async;
	qpair->async = false;
	rc = nvme_transport_ctrlr_connect_qpair(ctrlr, qpair);
	qpair->async = async;

	if (rc != 0) {
		qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
	}

	return rc;
}

/**
 * This function will be called when the controller is being reinitialized.
 * Note: the ctrlr_lock must be held when calling this function.
 */
int
spdk_nvme_ctrlr_reconnect_poll_async(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ns *ns, *tmp_ns;
	struct spdk_nvme_qpair	*qpair;
	int rc = 0, rc_tmp = 0;

	if (nvme_ctrlr_process_init(ctrlr) != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "controller reinitialization failed\n");
		rc = -1;
	}
	if (ctrlr->state != NVME_CTRLR_STATE_READY && rc != -1) {
		return -EAGAIN;
	}

	/*
	 * For non-fabrics controllers, the memory locations of the transport qpair
	 * don't change when the controller is reset. They simply need to be
	 * re-enabled with admin commands to the controller. For fabric
	 * controllers we need to disconnect and reconnect the qpair on its
	 * own thread outside of the context of the reset.
	 */
	if (rc == 0 && !spdk_nvme_ctrlr_is_fabrics(ctrlr)) {
		/* Reinitialize qpairs */
		TAILQ_FOREACH(qpair, &ctrlr->active_io_qpairs, tailq) {
			/* Always clear the qid bit here, even for a foreign qpair. We need
			 * to make sure another process doesn't get the chance to grab that
			 * qid.
			 */
			assert(spdk_bit_array_get(ctrlr->free_io_qids, qpair->id));
			spdk_bit_array_clear(ctrlr->free_io_qids, qpair->id);
			if (nvme_ctrlr_get_current_process(ctrlr) != qpair->active_proc) {
				/*
				 * We cannot reinitialize a foreign qpair. The qpair's owning
				 * process will take care of it. Set failure reason to FAILURE_RESET
				 * to ensure that happens.
				 */
				qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_RESET;
				continue;
			}
			rc_tmp = nvme_ctrlr_reinitialize_io_qpair(ctrlr, qpair);
			if (rc_tmp != 0) {
				rc = rc_tmp;
			}
		}
	}

	/*
	 * Take this opportunity to remove inactive namespaces. During a reset namespace
	 * handles can be invalidated.
	 */
	RB_FOREACH_SAFE(ns, nvme_ns_tree, &ctrlr->ns, tmp_ns) {
		if (!ns->active) {
			RB_REMOVE(nvme_ns_tree, &ctrlr->ns, ns);
			spdk_free(ns);
		}
	}

	if (rc) {
		nvme_ctrlr_fail(ctrlr, false);
	}
	ctrlr->is_resetting = false;

	nvme_ctrlr_unlock(ctrlr);

	if (!ctrlr->cdata.oaes.ns_attribute_notices) {
		/*
		 * If controller doesn't support ns_attribute_notices and
		 * namespace attributes change (e.g. number of namespaces)
		 * we need to update system handling device reset.
		 */
		nvme_io_msg_ctrlr_update(ctrlr);
	}

	return rc;
}

/*
 * For PCIe transport, spdk_nvme_ctrlr_disconnect() will do a Controller Level Reset
 * (Change CC.EN from 1 to 0) as a operation to disconnect the admin qpair.
 * The following two functions are added to do a Controller Level Reset. They have
 * to be called under the nvme controller's lock.
 */
void
nvme_ctrlr_disable(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->is_disconnecting == true);

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_CHECK_EN, NVME_TIMEOUT_INFINITE);
}

int
nvme_ctrlr_disable_poll(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;

	if (nvme_ctrlr_process_init(ctrlr) != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "failed to disable controller\n");
		rc = -1;
	}

	if (ctrlr->state != NVME_CTRLR_STATE_DISABLED && rc != -1) {
		return -EAGAIN;
	}

	return rc;
}

static void
nvme_ctrlr_fail_io_qpairs(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_qpair	*qpair;

	TAILQ_FOREACH(qpair, &ctrlr->active_io_qpairs, tailq) {
		qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
	}
}

int
spdk_nvme_ctrlr_reset(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	nvme_ctrlr_lock(ctrlr);

	rc = nvme_ctrlr_disconnect(ctrlr);
	if (rc == 0) {
		nvme_ctrlr_fail_io_qpairs(ctrlr);
	}

	nvme_ctrlr_unlock(ctrlr);

	if (rc != 0) {
		if (rc == -EBUSY) {
			rc = 0;
		}
		return rc;
	}

	while (1) {
		rc = spdk_nvme_ctrlr_process_admin_completions(ctrlr);
		if (rc == -ENXIO) {
			break;
		}
	}

	spdk_nvme_ctrlr_reconnect_async(ctrlr);

	while (true) {
		rc = spdk_nvme_ctrlr_reconnect_poll_async(ctrlr);
		if (rc != -EAGAIN) {
			break;
		}
	}

	return rc;
}

int
spdk_nvme_ctrlr_reset_subsystem(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cap_register cap;
	int rc = 0;

	cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);
	if (cap.bits.nssrs == 0) {
		NVME_CTRLR_WARNLOG(ctrlr, "subsystem reset is not supported\n");
		return -ENOTSUP;
	}

	NVME_CTRLR_NOTICELOG(ctrlr, "resetting subsystem\n");
	nvme_ctrlr_lock(ctrlr);
	ctrlr->is_resetting = true;
	rc = nvme_ctrlr_set_nssr(ctrlr, SPDK_NVME_NSSR_VALUE);
	ctrlr->is_resetting = false;

	nvme_ctrlr_unlock(ctrlr);
	/*
	 * No more cleanup at this point like in the ctrlr reset. A subsystem reset will cause
	 * a hot remove for PCIe transport. The hot remove handling does all the necessary ctrlr cleanup.
	 */
	return rc;
}

int
spdk_nvme_ctrlr_set_trid(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_transport_id *trid)
{
	int rc = 0;

	nvme_ctrlr_lock(ctrlr);

	if (ctrlr->is_failed == false) {
		rc = -EPERM;
		goto out;
	}

	if (trid->trtype != ctrlr->trid.trtype) {
		rc = -EINVAL;
		goto out;
	}

	if (strncmp(trid->subnqn, ctrlr->trid.subnqn, SPDK_NVMF_NQN_MAX_LEN)) {
		rc = -EINVAL;
		goto out;
	}

	ctrlr->trid = *trid;

out:
	nvme_ctrlr_unlock(ctrlr);
	return rc;
}

void
spdk_nvme_ctrlr_set_remove_cb(struct spdk_nvme_ctrlr *ctrlr,
			      spdk_nvme_remove_cb remove_cb, void *remove_ctx)
{
	if (!spdk_process_is_primary()) {
		return;
	}

	nvme_ctrlr_lock(ctrlr);
	ctrlr->remove_cb = remove_cb;
	ctrlr->cb_ctx = remove_ctx;
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

enum nvme_active_ns_state {
	NVME_ACTIVE_NS_STATE_IDLE,
	NVME_ACTIVE_NS_STATE_PROCESSING,
	NVME_ACTIVE_NS_STATE_DONE,
	NVME_ACTIVE_NS_STATE_ERROR
};

typedef void (*nvme_active_ns_ctx_deleter)(struct nvme_active_ns_ctx *);

struct nvme_active_ns_ctx {
	struct spdk_nvme_ctrlr *ctrlr;
	uint32_t page_count;
	uint32_t next_nsid;
	uint32_t *new_ns_list;
	nvme_active_ns_ctx_deleter deleter;
	struct nvme_completion_poll_status status;

	enum nvme_active_ns_state state;
};

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

static void
nvme_active_ns_ctx_destroy(struct nvme_active_ns_ctx *ctx)
{
	spdk_free(ctx->new_ns_list);
	free(ctx);
}

static int
nvme_ctrlr_destruct_namespace(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns tmp, *ns;

	assert(ctrlr != NULL);

	tmp.id = nsid;
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);
	if (ns == NULL) {
		return -EINVAL;
	}

	nvme_ns_destruct(ns);
	ns->active = false;

	return 0;
}

static int
nvme_ctrlr_construct_namespace(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns *ns;

	if (nsid < 1 || nsid > ctrlr->cdata.nn) {
		return -EINVAL;
	}

	/* Namespaces are constructed on demand, so simply request it. */
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		return -ENOMEM;
	}

	ns->active = true;

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

static void
nvme_ctrlr_identify_active_ns_async_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_active_ns_ctx *ctx = arg;
	uint32_t *new_ns_list = NULL;

	if (ctx->status.timed_out) {
		nvme_active_ns_ctx_destroy(ctx);
		return;
	}

	memcpy(&ctx->status.cpl, cpl, sizeof(*cpl));
	if (spdk_nvme_cpl_is_error(cpl)) {
		ctx->state = NVME_ACTIVE_NS_STATE_ERROR;
		goto out;
	}

	ctx->next_nsid = ctx->new_ns_list[1024 * ctx->page_count - 1];
	if (ctx->next_nsid == 0) {
		ctx->state = NVME_ACTIVE_NS_STATE_DONE;
		goto out;
	}

	ctx->page_count++;
	new_ns_list = spdk_realloc(ctx->new_ns_list,
				   ctx->page_count * sizeof(struct spdk_nvme_ns_list),
				   ctx->ctrlr->page_size);
	if (!new_ns_list) {
		NVME_CTRLR_ERRLOG(ctx->ctrlr, "Failed to reallocate active_ns_list!\n");
		ctx->state = NVME_ACTIVE_NS_STATE_ERROR;
		goto out;
	}

	ctx->new_ns_list = new_ns_list;
	ctx->status.timeout_tsc = spdk_get_ticks() + ctx->ctrlr->opts.admin_timeout_ms * 1000 *
				  spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
	nvme_ctrlr_identify_active_ns_async(ctx);
	return;

out:
	ctx->status.done = true;
	if (ctx->deleter) {
		ctx->deleter(ctx);
	}
}

static void
nvme_ctrlr_identify_active_ns_async(struct nvme_active_ns_ctx *ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;
	uint32_t i;
	int rc;

	if (ctrlr->cdata.nn == 0) {
		ctx->state = NVME_ACTIVE_NS_STATE_DONE;
		goto out;
	}

	assert(ctx->new_ns_list != NULL);

	/*
	 * If controller doesn't support active ns list CNS 0x02 dummy up
	 * an active ns list, i.e. all namespaces report as active
	 */
	if (ctrlr->vs.raw < SPDK_NVME_VERSION(1, 1, 0) || ctrlr->quirks & NVME_QUIRK_IDENTIFY_CNS) {
		uint32_t *new_ns_list;

		/*
		 * Active NS list must always end with zero element.
		 * So, we allocate for cdata.nn+1.
		 */
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
		ctx->new_ns_list[ctrlr->cdata.nn] = 0;
		for (i = 0; i < ctrlr->cdata.nn; i++) {
			ctx->new_ns_list[i] = i + 1;
		}

		ctx->state = NVME_ACTIVE_NS_STATE_DONE;
		goto out;
	}

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
	if (ctx->deleter) {
		ctx->deleter(ctx);
	}
}

static void
_nvme_active_ns_ctx_deleter(struct nvme_active_ns_ctx *ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;
	struct spdk_nvme_ns *ns;

	if (ctx->state == NVME_ACTIVE_NS_STATE_ERROR) {
		nvme_active_ns_ctx_destroy(ctx);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	assert(ctx->state == NVME_ACTIVE_NS_STATE_DONE);

	RB_FOREACH(ns, nvme_ns_tree, &ctrlr->ns) {
		nvme_ns_free_iocs_specific_data(ns);
	}

	nvme_ctrlr_identify_active_ns_swap(ctrlr, ctx->new_ns_list, ctx->page_count * 1024);
	nvme_active_ns_ctx_destroy(ctx);
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_NS, ctrlr->opts.admin_timeout_ms);
}

static void
_nvme_ctrlr_identify_active_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_active_ns_ctx *ctx;

	ctx = nvme_active_ns_ctx_create(ctrlr, _nvme_active_ns_ctx_deleter);
	if (!ctx) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ACTIVE_NS,
			     ctrlr->opts.admin_timeout_ms);
	nvme_ctrlr_identify_active_ns_async(ctx);
}

int
nvme_ctrlr_identify_active_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_active_ns_ctx *ctx;
	int rc;

	ctx = nvme_active_ns_ctx_create(ctrlr, NULL);
	if (!ctx) {
		return -ENOMEM;
	}

	nvme_ctrlr_identify_active_ns_async(ctx);
	if (ctx->state == NVME_ACTIVE_NS_STATE_ERROR) {
		nvme_active_ns_ctx_destroy(ctx);
		return -ENXIO;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, &ctx->status, false);
	if (rc || ctx->state == NVME_ACTIVE_NS_STATE_ERROR) {
		if (!ctx->status.timed_out) {
			nvme_active_ns_ctx_destroy(ctx);
		}

		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_ctrlr_identify_active_ns_async failed: rc=%s\n",
				  spdk_strerror(abs(rc)));
		return -ENXIO;
	}

	assert(ctx->state == NVME_ACTIVE_NS_STATE_DONE);
	nvme_ctrlr_identify_active_ns_swap(ctrlr, ctx->new_ns_list, ctx->page_count * 1024);
	nvme_active_ns_ctx_destroy(ctx);
	return rc;
}

static void
nvme_ctrlr_identify_ns_async_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ns *ns = (struct spdk_nvme_ns *)arg;
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	uint32_t nsid;
	int rc;

	if (nvme_ctrlr_handle_identify_ns_completion(ctrlr, ns, cpl)) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	nvme_ns_set_identify_data(ns);

	/* move on to the next active NS */
	nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, ns->id);
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_ID_DESCS,
				     ctrlr->opts.admin_timeout_ms);
		return;
	}
	ns->ctrlr = ctrlr;
	ns->id = nsid;

	rc = nvme_ctrlr_identify_ns_async(ns);
	if (rc) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}
}

static int
nvme_ctrlr_identify_ns_async(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	struct spdk_nvme_ns_data *nsdata;

	nsdata = &ns->nsdata;

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS,
			     ctrlr->opts.admin_timeout_ms);
	return nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS, 0, ns->id, 0,
				       nsdata, sizeof(*nsdata),
				       nvme_ctrlr_identify_ns_async_done, ns);
}

static int
nvme_ctrlr_identify_namespaces(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t nsid;
	struct spdk_nvme_ns *ns;
	int rc;

	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		/* No active NS, move on to the next state */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_ID_DESCS,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	ns->ctrlr = ctrlr;
	ns->id = nsid;

	rc = nvme_ctrlr_identify_ns_async(ns);
	if (rc) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}

	return rc;
}

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

static void
nvme_ctrlr_process_async_event_finish(struct spdk_nvme_ctrlr_aer_completion *async_event)
{
	struct spdk_nvme_ctrlr_process	*active_proc;

	active_proc = nvme_ctrlr_get_current_process(async_event->ctrlr);
	if (active_proc && active_proc->aer_cb_fn) {
		active_proc->aer_cb_fn(active_proc->aer_cb_arg, &async_event->cpl);
	}

	spdk_free(async_event);
}

static void
nvme_ctrlr_update_namespaces(struct spdk_nvme_ctrlr_aer_completion *async_event)
{
	struct spdk_nvme_ctrlr *ctrlr = async_event->ctrlr;
	uint32_t nsid, i;
	struct spdk_nvme_ns *ns;

	/* Log page is not used, go over all active namespaces.
	 * Either the log page overflowed or disable_read_changed_ns_list_log_page is used. */
	if (async_event->log_page.changed_ns_list == NULL) {
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
		     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
			ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
			nvme_ns_construct(ns, nsid, ctrlr);
		}

		return;
	}

	/* Iterate over NSID from the log page. */
	for (i = 0; i < SPDK_NVME_MAX_CHANGED_NAMESPACES; i++) {
		nsid = async_event->log_page.changed_ns_list[i];

		/* End of the list */
		if (nsid == 0) {
			break;
		}

		/* Log page contains NSID for namespaces that were marked
		 * as inactive, no need to identify them. */
		if (!spdk_nvme_ctrlr_is_active_ns(ctrlr, nsid)) {
			continue;
		}

		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		nvme_ns_construct(ns, nsid, ctrlr);
	}

	free(async_event->log_page.changed_ns_list);
}

static int
nvme_ctrlr_clear_changed_ns_log(struct spdk_nvme_ctrlr_aer_completion *async_event)
{
	struct spdk_nvme_ctrlr			*ctrlr = async_event->ctrlr;
	struct nvme_completion_poll_status	*status;
	int		rc = -ENOMEM;
	uint32_t	*changed_ns_list;
	size_t		changed_ns_list_length = SPDK_NVME_MAX_CHANGED_NAMESPACES * sizeof(uint32_t);

	if (ctrlr->opts.disable_read_changed_ns_list_log_page) {
		return 0;
	}

	changed_ns_list = calloc(1, changed_ns_list_length);
	if (!changed_ns_list) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate buffer for getting changed ns log.\n");
		goto out;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		goto out;
	}

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr,
					      SPDK_NVME_LOG_CHANGED_NS_LIST,
					      SPDK_NVME_GLOBAL_NS_TAG,
					      changed_ns_list, changed_ns_list_length, 0,
					      nvme_completion_poll_cb, status);
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_cmd_get_log_page() failed: rc=%d\n", rc);
		free(status);
		goto out;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for spdk_nvme_ctrlr_cmd_get_log_page failed: rc=%s\n",
				  spdk_strerror(abs(rc)));
		goto out;
	}

	/* only check the case of overflow. */
	if (changed_ns_list[0] == UINT32_MAX) {
		NVME_CTRLR_WARNLOG(ctrlr, "changed ns log overflowed.\n");
		goto out;
	}

	async_event->log_page.changed_ns_list = changed_ns_list;
	return 0;

out:
	free(changed_ns_list);
	return rc;
}

static void
nvme_ctrlr_process_async_event(struct spdk_nvme_ctrlr_aer_completion *async_event)
{
	struct spdk_nvme_ctrlr *ctrlr = async_event->ctrlr;
	struct spdk_nvme_cpl *cpl = &async_event->cpl;
	union spdk_nvme_async_event_completion event;
	int rc;

	event.raw = cpl->cdw0;

	if ((event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) &&
	    (event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED)) {
		nvme_ctrlr_clear_changed_ns_log(async_event);

		rc = nvme_ctrlr_identify_active_ns(ctrlr);
		if (rc) {
			return;
		}
		nvme_ctrlr_update_namespaces(async_event);
		nvme_io_msg_ctrlr_update(ctrlr);
	}

	if ((event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) &&
	    (event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_ANA_CHANGE)) {
		if (!ctrlr->opts.disable_read_ana_log_page) {
			rc = nvme_ctrlr_update_ana_log_page(ctrlr);
			if (rc) {
				return;
			}
			nvme_ctrlr_parse_ana_log_page(ctrlr, nvme_ctrlr_update_ns_ana_states,
						      ctrlr);
		}
	}

	nvme_ctrlr_process_async_event_finish(async_event);
}

static void
nvme_ctrlr_queue_async_event(struct spdk_nvme_ctrlr *ctrlr,
			     const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr_aer_completion *async_event;
	struct spdk_nvme_ctrlr_process *proc;

	/* Add async event to each process objects event list */
	TAILQ_FOREACH(proc, &ctrlr->active_procs, tailq) {
		/* Must be shared memory so other processes can access */
		async_event = spdk_zmalloc(sizeof(*async_event), 0, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
		if (!async_event) {
			NVME_CTRLR_ERRLOG(ctrlr, "Alloc nvme event failed, ignore the event\n");
			return;
		}
		async_event->ctrlr = ctrlr;
		async_event->cpl = *cpl;

		STAILQ_INSERT_TAIL(&proc->async_events, async_event, link);
	}
}

static void
nvme_ctrlr_complete_queued_async_events(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_aer_completion *async_event, *async_event_tmp;
	struct spdk_nvme_ctrlr_process *active_proc;

	active_proc = nvme_ctrlr_get_current_process(ctrlr);

	STAILQ_FOREACH_SAFE(async_event, &active_proc->async_events, link, async_event_tmp) {
		STAILQ_REMOVE(&active_proc->async_events, async_event,
			      spdk_nvme_ctrlr_aer_completion, link);
		nvme_ctrlr_process_async_event(async_event);
	}
}

static void
nvme_ctrlr_async_event_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_async_event_request	*aer = arg;
	struct spdk_nvme_ctrlr		*ctrlr = aer->ctrlr;

	if (cpl->status.sct == SPDK_NVME_SCT_GENERIC &&
	    cpl->status.sc == SPDK_NVME_SC_ABORTED_SQ_DELETION) {
		/*
		 *  This is simulated when controller is being shut down, to
		 *  effectively abort outstanding asynchronous event requests
		 *  and make sure all memory is freed.  Do not repost the
		 *  request in this case.
		 */
		return;
	}

	if (cpl->status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC &&
	    cpl->status.sc == SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED) {
		/*
		 *  SPDK will only send as many AERs as the device says it supports,
		 *  so this status code indicates an out-of-spec device.  Do not repost
		 *  the request in this case.
		 */
		NVME_CTRLR_ERRLOG(ctrlr, "Controller appears out-of-spec for asynchronous event request\n"
				  "handling.  Do not repost this AER.\n");
		return;
	}

	/* Add the events to the list */
	nvme_ctrlr_queue_async_event(ctrlr, cpl);

	/* If the ctrlr was removed or in the destruct state, we should not send aer again */
	if (ctrlr->is_removed || ctrlr->is_destructed) {
		return;
	}

	/*
	 * Repost another asynchronous event request to replace the one
	 *  that just completed.
	 */
	if (nvme_ctrlr_construct_and_submit_aer(ctrlr, aer)) {
		/*
		 * We can't do anything to recover from a failure here,
		 * so just print a warning message and leave the AER unsubmitted.
		 */
		NVME_CTRLR_ERRLOG(ctrlr, "resubmitting AER failed!\n");
	}
}

static int
nvme_ctrlr_construct_and_submit_aer(struct spdk_nvme_ctrlr *ctrlr,
				    struct nvme_async_event_request *aer)
{
	struct nvme_request *req;

	aer->ctrlr = ctrlr;
	req = nvme_allocate_request_null(ctrlr->adminq, nvme_ctrlr_async_event_cb, aer);
	aer->req = req;
	if (req == NULL) {
		return -1;
	}

	req->cmd.opc = SPDK_NVME_OPC_ASYNC_EVENT_REQUEST;
	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static void
nvme_ctrlr_configure_aer_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_async_event_request		*aer;
	int					rc;
	uint32_t				i;
	struct spdk_nvme_ctrlr *ctrlr =	(struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_NOTICELOG(ctrlr, "nvme_ctrlr_configure_aer failed!\n");
		ctrlr->num_aers = 0;
	} else {
		/* aerl is a zero-based value, so we need to add 1 here. */
		ctrlr->num_aers = spdk_min(NVME_MAX_ASYNC_EVENTS, (ctrlr->cdata.aerl + 1));
	}

	for (i = 0; i < ctrlr->num_aers; i++) {
		aer = &ctrlr->aer[i];
		rc = nvme_ctrlr_construct_and_submit_aer(ctrlr, aer);
		if (rc) {
			NVME_CTRLR_ERRLOG(ctrlr, "nvme_ctrlr_construct_and_submit_aer failed!\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
			return;
		}
	}
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT, ctrlr->opts.admin_timeout_ms);
}

static int
nvme_ctrlr_configure_aer(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_feat_async_event_configuration	config;
	int						rc;

	config.raw = 0;

	if (spdk_nvme_ctrlr_is_discovery(ctrlr)) {
		config.bits.discovery_log_change_notice = 1;
	} else {
		config.bits.crit_warn.bits.available_spare = 1;
		config.bits.crit_warn.bits.temperature = 1;
		config.bits.crit_warn.bits.device_reliability = 1;
		config.bits.crit_warn.bits.read_only = 1;
		config.bits.crit_warn.bits.volatile_memory_backup = 1;

		if (ctrlr->vs.raw >= SPDK_NVME_VERSION(1, 2, 0)) {
			if (ctrlr->cdata.oaes.ns_attribute_notices) {
				config.bits.ns_attr_notice = 1;
			}
			if (ctrlr->cdata.oaes.fw_activation_notices) {
				config.bits.fw_activation_notice = 1;
			}
			if (ctrlr->cdata.oaes.ana_change_notices) {
				config.bits.ana_change_notice = 1;
			}
		}
		if (ctrlr->vs.raw >= SPDK_NVME_VERSION(1, 3, 0) && ctrlr->cdata.lpa.ts) {
			config.bits.telemetry_log_notice = 1;
		}
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_CONFIGURE_AER,
			     ctrlr->opts.admin_timeout_ms);

	rc = nvme_ctrlr_cmd_set_async_event_config(ctrlr, config,
			nvme_ctrlr_configure_aer_done,
			ctrlr);
	if (rc != 0) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return rc;
	}

	return 0;
}

struct spdk_nvme_ctrlr_process *
nvme_ctrlr_get_process(struct spdk_nvme_ctrlr *ctrlr, pid_t pid)
{
	struct spdk_nvme_ctrlr_process	*active_proc;

	TAILQ_FOREACH(active_proc, &ctrlr->active_procs, tailq) {
		if (active_proc->pid == pid) {
			return active_proc;
		}
	}

	return NULL;
}

struct spdk_nvme_ctrlr_process *
nvme_ctrlr_get_current_process(struct spdk_nvme_ctrlr *ctrlr)
{
	return nvme_ctrlr_get_process(ctrlr, getpid());
}

/**
 * This function will be called when a process is using the controller.
 *  1. For the primary process, it is called when constructing the controller.
 *  2. For the secondary process, it is called at probing the controller.
 * Note: will check whether the process is already added for the same process.
 */
int
nvme_ctrlr_add_process(struct spdk_nvme_ctrlr *ctrlr, void *devhandle)
{
	struct spdk_nvme_ctrlr_process	*ctrlr_proc;
	pid_t				pid = getpid();

	/* Check whether the process is already added or not */
	if (nvme_ctrlr_get_process(ctrlr, pid)) {
		return 0;
	}

	/* Initialize the per process properties for this ctrlr */
	ctrlr_proc = spdk_zmalloc(sizeof(struct spdk_nvme_ctrlr_process),
				  64, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	if (ctrlr_proc == NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "failed to allocate memory to track the process props\n");

		return -1;
	}

	ctrlr_proc->is_primary = spdk_process_is_primary();
	ctrlr_proc->pid = pid;
	STAILQ_INIT(&ctrlr_proc->active_reqs);
	ctrlr_proc->devhandle = devhandle;
	ctrlr_proc->ref = 0;
	TAILQ_INIT(&ctrlr_proc->allocated_io_qpairs);
	STAILQ_INIT(&ctrlr_proc->async_events);

	TAILQ_INSERT_TAIL(&ctrlr->active_procs, ctrlr_proc, tailq);

	return 0;
}

/**
 * This function will be called when the process detaches the controller.
 * Note: the ctrlr_lock must be held when calling this function.
 */
static void
nvme_ctrlr_remove_process(struct spdk_nvme_ctrlr *ctrlr,
			  struct spdk_nvme_ctrlr_process *proc)
{
	struct spdk_nvme_qpair	*qpair, *tmp_qpair;

	assert(STAILQ_EMPTY(&proc->active_reqs));

	TAILQ_FOREACH_SAFE(qpair, &proc->allocated_io_qpairs, per_process_tailq, tmp_qpair) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}

	TAILQ_REMOVE(&ctrlr->active_procs, proc, tailq);

	if (ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		spdk_pci_device_detach(proc->devhandle);
	}

	spdk_free(proc);
}

/**
 * This function will be called when the process exited unexpectedly
 *  in order to free any incomplete nvme request, allocated IO qpairs
 *  and allocated memory.
 * Note: the ctrlr_lock must be held when calling this function.
 */
static void
nvme_ctrlr_cleanup_process(struct spdk_nvme_ctrlr_process *proc)
{
	struct nvme_request	*req, *tmp_req;
	struct spdk_nvme_qpair	*qpair, *tmp_qpair;
	struct spdk_nvme_ctrlr_aer_completion *event;

	STAILQ_FOREACH_SAFE(req, &proc->active_reqs, stailq, tmp_req) {
		STAILQ_REMOVE(&proc->active_reqs, req, nvme_request, stailq);

		assert(req->pid == proc->pid);
		nvme_cleanup_user_req(req);
		nvme_free_request(req);
	}

	/* Remove async event from each process objects event list */
	while (!STAILQ_EMPTY(&proc->async_events)) {
		event = STAILQ_FIRST(&proc->async_events);
		STAILQ_REMOVE_HEAD(&proc->async_events, link);
		spdk_free(event);
	}

	TAILQ_FOREACH_SAFE(qpair, &proc->allocated_io_qpairs, per_process_tailq, tmp_qpair) {
		TAILQ_REMOVE(&proc->allocated_io_qpairs, qpair, per_process_tailq);

		/*
		 * The process may have been killed while some qpairs were in their
		 *  completion context.  Clear that flag here to allow these IO
		 *  qpairs to be deleted.
		 */
		qpair->in_completion_context = 0;

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

void
nvme_ctrlr_proc_get_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc;

	nvme_ctrlr_lock(ctrlr);

	nvme_ctrlr_remove_inactive_proc(ctrlr);

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		active_proc->ref++;
	}

	nvme_ctrlr_unlock(ctrlr);
}

void
nvme_ctrlr_proc_put_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc;
	int				proc_count;

	nvme_ctrlr_lock(ctrlr);

	proc_count = nvme_ctrlr_remove_inactive_proc(ctrlr);

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		active_proc->ref--;
		assert(active_proc->ref >= 0);

		/*
		 * The last active process will be removed at the end of
		 * the destruction of the controller.
		 */
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

static void
nvme_ctrlr_process_init_vs_done(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the VS register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	assert(value <= UINT32_MAX);
	ctrlr->vs.raw = (uint32_t)value;
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READ_CAP, NVME_TIMEOUT_INFINITE);
}

static void
nvme_ctrlr_process_init_cap_done(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CAP register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	ctrlr->cap.raw = value;
	nvme_ctrlr_init_cap(ctrlr);
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_CHECK_EN, NVME_TIMEOUT_INFINITE);
}

static void
nvme_ctrlr_process_init_check_en(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	enum nvme_ctrlr_state state;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CC register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	assert(value <= UINT32_MAX);
	ctrlr->process_init_cc.raw = (uint32_t)value;

	if (ctrlr->process_init_cc.bits.en) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "CC.EN = 1\n");
		state = NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1;
	} else {
		state = NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0;
	}

	nvme_ctrlr_set_state(ctrlr, state, nvme_ctrlr_get_ready_timeout(ctrlr));
}

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
	if (ctrlr->quirks & NVME_QUIRK_DELAY_BEFORE_CHK_RDY) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Applying quirk: delay 2.5 seconds before reading registers\n");
		ctrlr->sleep_timeout_tsc = spdk_get_ticks() + (2500 * spdk_get_ticks_hz() / 1000);
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0,
			     nvme_ctrlr_get_ready_timeout(ctrlr));
}

static void
nvme_ctrlr_process_init_set_en_0_read_cc(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	union spdk_nvme_cc_register cc;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CC register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	assert(value <= UINT32_MAX);
	cc.raw = (uint32_t)value;
	cc.bits.en = 0;
	ctrlr->process_init_cc.raw = cc.raw;

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_EN_0_WAIT_FOR_CC,
			     nvme_ctrlr_get_ready_timeout(ctrlr));

	rc = nvme_ctrlr_set_cc_async(ctrlr, cc.raw, nvme_ctrlr_process_init_set_en_0, ctrlr);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_cc() failed\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}
}

static void
nvme_ctrlr_process_init_wait_for_ready_1(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	union spdk_nvme_csts_register csts;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* While a device is resetting, it may be unable to service MMIO reads
		 * temporarily. Allow for this case.
		 */
		if (!ctrlr->is_failed && ctrlr->state_timeout_tsc != NVME_TIMEOUT_INFINITE) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Failed to read the CSTS register\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1,
					     NVME_TIMEOUT_KEEP_EXISTING);
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
	} else {
		NVME_CTRLR_DEBUGLOG(ctrlr, "CC.EN = 1 && CSTS.RDY = 0 - waiting for reset to complete\n");
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1,
					   NVME_TIMEOUT_KEEP_EXISTING);
	}
}

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
	} else {
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0,
					   NVME_TIMEOUT_KEEP_EXISTING);
	}
}

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
		/*
		 * The controller has been enabled.
		 *  Perform the rest of initialization serially.
		 */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_RESET_ADMIN_QUEUE,
				     ctrlr->opts.admin_timeout_ms);
	} else {
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1,
					   NVME_TIMEOUT_KEEP_EXISTING);
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

static void
nvme_keep_alive_completion(void *cb_ctx, const struct spdk_nvme_cpl *cpl)
{
	/* Do nothing */
}

/*
 * Check if we need to send a Keep Alive command.
 * Caller must hold ctrlr->ctrlr_lock.
 */
static int
nvme_ctrlr_keep_alive(struct spdk_nvme_ctrlr *ctrlr)
{
	uint64_t now;
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	int rc = 0;

	now = spdk_get_ticks();
	if (now < ctrlr->next_keep_alive_tick) {
		return rc;
	}

	req = nvme_allocate_request_null(ctrlr->adminq, nvme_keep_alive_completion, NULL);
	if (req == NULL) {
		return rc;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KEEP_ALIVE;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Submitting Keep Alive failed\n");
		rc = -ENXIO;
	}

	ctrlr->next_keep_alive_tick = now + ctrlr->keep_alive_interval_ticks;
	return rc;
}

bool
spdk_nvme_ctrlr_is_nssr_supported(struct spdk_nvme_ctrlr *ctrlr)
{
	/* NSSR is done via write to the NVMe register.
	 * SPDK is handling it synchronously, so in nvmf connected to another nvmf
	 * it might cause delays and possible deadlocks.
	 * Limit NSSR to be done only for PCIe transport.
	 */
	return ctrlr->cap.bits.nssrs == 1 && ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE;
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	int32_t num_completions;
	int32_t rc;
	struct spdk_nvme_ctrlr_process	*active_proc;

	nvme_ctrlr_lock(ctrlr);

	if (ctrlr->keep_alive_interval_ticks) {
		rc = nvme_ctrlr_keep_alive(ctrlr);
		if (rc) {
			nvme_ctrlr_unlock(ctrlr);
			return rc;
		}
	}

	rc = nvme_io_msg_process(ctrlr);
	if (rc < 0) {
		nvme_ctrlr_unlock(ctrlr);
		return rc;
	}
	num_completions = rc;

	rc = spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);

	/* Each process has an async list, complete the ones for this process object */
	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		nvme_ctrlr_complete_queued_async_events(ctrlr);
	}

	if (rc == -ENXIO && ctrlr->is_disconnecting) {
		nvme_ctrlr_disconnect_done(ctrlr);
	}

	nvme_ctrlr_unlock(ctrlr);

	if (rc < 0) {
		num_completions = rc;
	} else {
		num_completions += rc;
	}

	return num_completions;
}

const struct spdk_nvme_ctrlr_data *
spdk_nvme_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr)
{
	return &ctrlr->cdata;
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

uint64_t
spdk_nvme_ctrlr_get_pmrsz(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->pmr_size;
}

uint32_t
spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cdata.nn;
}

bool
spdk_nvme_ctrlr_is_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns tmp, *ns;

	tmp.id = nsid;
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);

	if (ns != NULL) {
		return ns->active;
	}

	return false;
}

uint32_t
spdk_nvme_ctrlr_get_first_active_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ns *ns;

	ns = RB_MIN(nvme_ns_tree, &ctrlr->ns);
	if (ns == NULL) {
		return 0;
	}

	while (ns != NULL) {
		if (ns->active) {
			return ns->id;
		}

		ns = RB_NEXT(nvme_ns_tree, &ctrlr->ns, ns);
	}

	return 0;
}

uint32_t
spdk_nvme_ctrlr_get_next_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t prev_nsid)
{
	struct spdk_nvme_ns tmp, *ns;

	tmp.id = prev_nsid;
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);
	if (ns == NULL) {
		return 0;
	}

	ns = RB_NEXT(nvme_ns_tree, &ctrlr->ns, ns);
	while (ns != NULL) {
		if (ns->active) {
			return ns->id;
		}

		ns = RB_NEXT(nvme_ns_tree, &ctrlr->ns, ns);
	}

	return 0;
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

struct spdk_pci_device *
spdk_nvme_ctrlr_get_pci_device(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr == NULL) {
		return NULL;
	}

	if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		return NULL;
	}

	return nvme_ctrlr_proc_get_devhandle(ctrlr);
}

int32_t
spdk_nvme_ctrlr_get_numa_id(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->numa.id_valid) {
		return ctrlr->numa.id;
	} else {
		return SPDK_ENV_NUMA_ID_ANY;
	}
}

uint16_t
spdk_nvme_ctrlr_get_id(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cntlid;
}

uint32_t
spdk_nvme_ctrlr_get_max_xfer_size(const struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->max_xfer_size;
}

uint16_t
spdk_nvme_ctrlr_get_max_sges(const struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->flags & SPDK_NVME_CTRLR_SGL_SUPPORTED) {
		return ctrlr->max_sges;
	} else {
		return UINT16_MAX;
	}
}

void
spdk_nvme_ctrlr_register_aer_callback(struct spdk_nvme_ctrlr *ctrlr,
				      spdk_nvme_aer_cb aer_cb_fn,
				      void *aer_cb_arg)
{
	struct spdk_nvme_ctrlr_process *active_proc;

	nvme_ctrlr_lock(ctrlr);

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		active_proc->aer_cb_fn = aer_cb_fn;
		active_proc->aer_cb_arg = aer_cb_arg;
	}

	nvme_ctrlr_unlock(ctrlr);
}

void
spdk_nvme_ctrlr_disable_read_changed_ns_list_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	ctrlr->opts.disable_read_changed_ns_list_log_page = true;
}

void
spdk_nvme_ctrlr_register_timeout_callback(struct spdk_nvme_ctrlr *ctrlr,
		uint64_t timeout_io_us, uint64_t timeout_admin_us,
		spdk_nvme_timeout_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_ctrlr_process	*active_proc;

	nvme_ctrlr_lock(ctrlr);

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
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
		active_proc->timeout_admin_ticks = timeout_admin_us * spdk_get_ticks_hz() / 1000000ULL;
		active_proc->timeout_cb_fn = cb_fn;
		active_proc->timeout_cb_arg = cb_arg;
	}

	ctrlr->timeout_enabled = true;

	nvme_ctrlr_unlock(ctrlr);
}

bool
spdk_nvme_ctrlr_is_log_page_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page)
{
	/* No bounds check necessary, since log_page is uint8_t and log_page_supported has 256 entries */
	SPDK_STATIC_ASSERT(sizeof(ctrlr->log_page_supported) == 256, "log_page_supported size mismatch");
	return ctrlr->log_page_supported[log_page];
}

bool
spdk_nvme_ctrlr_is_feature_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature_code)
{
	/* No bounds check necessary, since feature_code is uint8_t and feature_supported has 256 entries */
	SPDK_STATIC_ASSERT(sizeof(ctrlr->feature_supported) == 256, "feature_supported size mismatch");
	return ctrlr->feature_supported[feature_code];
}

int
spdk_nvme_ctrlr_attach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			  struct spdk_nvme_ctrlr_list *payload)
{
	struct nvme_completion_poll_status	*status;
	struct spdk_nvme_ns			*ns;
	int					res;

	if (nsid == 0) {
		return -EINVAL;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

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

	res = nvme_ctrlr_identify_active_ns(ctrlr);
	if (res) {
		return res;
	}

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_get_ns failed!\n");
		return -ENXIO;
	}

	return nvme_ns_construct(ns, nsid, ctrlr);
}

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

	return nvme_ctrlr_identify_active_ns(ctrlr);
}

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

	res = nvme_ctrlr_cmd_create_ns(ctrlr, payload, nvme_completion_poll_cb, status);
	if (res) {
		free(status);
		return 0;
	}

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

	assert(nsid > 0);

	/* Return the namespace ID that was created */
	return nsid;
}

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

	return nvme_ctrlr_identify_active_ns(ctrlr);
}

int
spdk_nvme_ctrlr_format(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
		       struct spdk_nvme_format *format)
{
	struct nvme_completion_poll_status	*status;
	int					res;

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	res = nvme_ctrlr_cmd_format(ctrlr, nsid, format, nvme_completion_poll_cb,
				    status);
	if (res) {
		free(status);
		return res;
	}

	res = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (res) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for spdk_nvme_ctrlr_format failed: rc=%s\n",
				  spdk_strerror(abs(res)));
		return res;
	}

	return spdk_nvme_ctrlr_reset(ctrlr);
}

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

	if (!completion_status) {
		return -EINVAL;
	}
	memset(completion_status, 0, sizeof(struct spdk_nvme_status));
	if (size % 4) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_update_firmware invalid size!\n");
		return -1;
	}

	/* Current support only for SPDK_NVME_FW_COMMIT_REPLACE_IMG
	 * and SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG
	 */
	if ((commit_action != SPDK_NVME_FW_COMMIT_REPLACE_IMG) &&
	    (commit_action != SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG)) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_update_firmware invalid command!\n");
		return -1;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	/* Firmware download */
	size_remaining = size;
	offset = 0;
	p = payload;

	while (size_remaining > 0) {
		transfer = spdk_min(size_remaining, ctrlr->min_page_size);

		memset(status, 0, sizeof(*status));
		res = nvme_ctrlr_cmd_fw_image_download(ctrlr, transfer, offset, p,
						       nvme_completion_poll_cb,
						       status);
		if (res) {
			free(status);
			return res;
		}

		res = nvme_wait_for_adminq_completion(ctrlr, status, false);
		if (res) {
			if (!status->timed_out) {
				free(status);
			}

			NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_fw_image_download failed: rc=%s\n",
					  spdk_strerror(abs(res)));
			return res;
		}

		p += transfer;
		offset += transfer;
		size_remaining -= transfer;
	}

	/* Firmware commit */
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

	res = nvme_wait_for_adminq_completion(ctrlr, status, false);
	memcpy(completion_status, &status->cpl.status, sizeof(struct spdk_nvme_status));
	if (!status->timed_out) {
		free(status);
	}

	if (res) {
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

	return spdk_nvme_ctrlr_reset(ctrlr);
}

int
spdk_nvme_ctrlr_reserve_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc, size;
	union spdk_nvme_cmbsz_register cmbsz;

	cmbsz = spdk_nvme_ctrlr_get_regs_cmbsz(ctrlr);

	if (cmbsz.bits.rds == 0 || cmbsz.bits.wds == 0) {
		return -ENOTSUP;
	}

	size = cmbsz.bits.sz * (0x1000 << (cmbsz.bits.szu * 4));

	nvme_ctrlr_lock(ctrlr);
	rc = nvme_transport_ctrlr_reserve_cmb(ctrlr);
	nvme_ctrlr_unlock(ctrlr);

	if (rc < 0) {
		return rc;
	}

	return size;
}

void *
spdk_nvme_ctrlr_map_cmb(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	void *buf;

	nvme_ctrlr_lock(ctrlr);
	buf = nvme_transport_ctrlr_map_cmb(ctrlr, size);
	nvme_ctrlr_unlock(ctrlr);

	return buf;
}

void
spdk_nvme_ctrlr_unmap_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_ctrlr_lock(ctrlr);
	nvme_transport_ctrlr_unmap_cmb(ctrlr);
	nvme_ctrlr_unlock(ctrlr);
}

int
spdk_nvme_ctrlr_enable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	nvme_ctrlr_lock(ctrlr);
	rc = nvme_transport_ctrlr_enable_pmr(ctrlr);
	nvme_ctrlr_unlock(ctrlr);

	return rc;
}

int
spdk_nvme_ctrlr_disable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	nvme_ctrlr_lock(ctrlr);
	rc = nvme_transport_ctrlr_disable_pmr(ctrlr);
	nvme_ctrlr_unlock(ctrlr);

	return rc;
}

void *
spdk_nvme_ctrlr_map_pmr(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	void *buf;

	nvme_ctrlr_lock(ctrlr);
	buf = nvme_transport_ctrlr_map_pmr(ctrlr, size);
	nvme_ctrlr_unlock(ctrlr);

	return buf;
}

int
spdk_nvme_ctrlr_unmap_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	nvme_ctrlr_lock(ctrlr);
	rc = nvme_transport_ctrlr_unmap_pmr(ctrlr);
	nvme_ctrlr_unlock(ctrlr);

	return rc;
}

int
spdk_nvme_ctrlr_read_boot_partition_start(struct spdk_nvme_ctrlr *ctrlr, void *payload,
		uint32_t bprsz, uint32_t bprof, uint32_t bpid)
{
	union spdk_nvme_bprsel_register bprsel;
	union spdk_nvme_bpinfo_register bpinfo;
	uint64_t bpmbl, bpmb_size;

	if (ctrlr->cap.bits.bps == 0) {
		return -ENOTSUP;
	}

	if (nvme_ctrlr_get_bpinfo(ctrlr, &bpinfo)) {
		NVME_CTRLR_ERRLOG(ctrlr, "get bpinfo failed\n");
		return -EIO;
	}

	if (bpinfo.bits.brs == SPDK_NVME_BRS_READ_IN_PROGRESS) {
		NVME_CTRLR_ERRLOG(ctrlr, "Boot Partition read already initiated\n");
		return -EALREADY;
	}

	nvme_ctrlr_lock(ctrlr);

	bpmb_size = bprsz * 4096;
	bpmbl = spdk_vtophys(payload, &bpmb_size);
	if (bpmbl == SPDK_VTOPHYS_ERROR) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_vtophys of bpmbl failed\n");
		nvme_ctrlr_unlock(ctrlr);
		return -EFAULT;
	}

	if (bpmb_size != bprsz * 4096) {
		NVME_CTRLR_ERRLOG(ctrlr, "Boot Partition buffer is not physically contiguous\n");
		nvme_ctrlr_unlock(ctrlr);
		return -EFAULT;
	}

	if (nvme_ctrlr_set_bpmbl(ctrlr, bpmbl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_bpmbl() failed\n");
		nvme_ctrlr_unlock(ctrlr);
		return -EIO;
	}

	bprsel.bits.bpid = bpid;
	bprsel.bits.bprof = bprof;
	bprsel.bits.bprsz = bprsz;

	if (nvme_ctrlr_set_bprsel(ctrlr, &bprsel)) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_bprsel() failed\n");
		nvme_ctrlr_unlock(ctrlr);
		return -EIO;
	}

	nvme_ctrlr_unlock(ctrlr);
	return 0;
}

int
spdk_nvme_ctrlr_read_boot_partition_poll(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	union spdk_nvme_bpinfo_register bpinfo;

	if (nvme_ctrlr_get_bpinfo(ctrlr, &bpinfo)) {
		NVME_CTRLR_ERRLOG(ctrlr, "get bpinfo failed\n");
		return -EIO;
	}

	switch (bpinfo.bits.brs) {
	case SPDK_NVME_BRS_NO_READ:
		NVME_CTRLR_ERRLOG(ctrlr, "Boot Partition read not initiated\n");
		rc = -EINVAL;
		break;
	case SPDK_NVME_BRS_READ_IN_PROGRESS:
		NVME_CTRLR_DEBUGLOG(ctrlr, "Boot Partition read in progress\n");
		rc = -EAGAIN;
		break;
	case SPDK_NVME_BRS_READ_ERROR:
		NVME_CTRLR_ERRLOG(ctrlr, "Error completing Boot Partition read\n");
		rc = -EIO;
		break;
	case SPDK_NVME_BRS_READ_SUCCESS:
		NVME_CTRLR_INFOLOG(ctrlr, "Boot Partition read completed successfully\n");
		break;
	default:
		NVME_CTRLR_ERRLOG(ctrlr, "Invalid Boot Partition read status\n");
		rc = -EINVAL;
	}

	return rc;
}

static void
nvme_write_boot_partition_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	int res;
	struct spdk_nvme_ctrlr *ctrlr = arg;
	struct spdk_nvme_fw_commit fw_commit;
	struct spdk_nvme_cpl err_cpl =
	{.status = {.sct = SPDK_NVME_SCT_GENERIC, .sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR }};

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Write Boot Partition failed\n");
		ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, cpl);
		return;
	}

	if (ctrlr->bp_ws == SPDK_NVME_BP_WS_DOWNLOADING) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Boot Partition Downloading at Offset %d Success\n", ctrlr->fw_offset);
		ctrlr->fw_payload = (uint8_t *)ctrlr->fw_payload + ctrlr->fw_transfer_size;
		ctrlr->fw_offset += ctrlr->fw_transfer_size;
		ctrlr->fw_size_remaining -= ctrlr->fw_transfer_size;
		ctrlr->fw_transfer_size = spdk_min(ctrlr->fw_size_remaining, ctrlr->min_page_size);
		res = nvme_ctrlr_cmd_fw_image_download(ctrlr, ctrlr->fw_transfer_size, ctrlr->fw_offset,
						       ctrlr->fw_payload, nvme_write_boot_partition_cb, ctrlr);
		if (res) {
			NVME_CTRLR_ERRLOG(ctrlr, "nvme_ctrlr_cmd_fw_image_download failed!\n");
			ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, &err_cpl);
			return;
		}

		if (ctrlr->fw_transfer_size < ctrlr->min_page_size) {
			ctrlr->bp_ws = SPDK_NVME_BP_WS_DOWNLOADED;
		}
	} else if (ctrlr->bp_ws == SPDK_NVME_BP_WS_DOWNLOADED) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Boot Partition Download Success\n");
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
		ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, cpl);
	} else {
		NVME_CTRLR_ERRLOG(ctrlr, "Invalid Boot Partition write state\n");
		ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, &err_cpl);
		return;
	}
}

int
spdk_nvme_ctrlr_write_boot_partition(struct spdk_nvme_ctrlr *ctrlr,
				     void *payload, uint32_t size, uint32_t bpid,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	int res;

	if (ctrlr->cap.bits.bps == 0) {
		return -ENOTSUP;
	}

	ctrlr->bp_ws = SPDK_NVME_BP_WS_DOWNLOADING;
	ctrlr->bpid = bpid;
	ctrlr->bp_write_cb_fn = cb_fn;
	ctrlr->bp_write_cb_arg = cb_arg;
	ctrlr->fw_offset = 0;
	ctrlr->fw_size_remaining = size;
	ctrlr->fw_payload = payload;
	ctrlr->fw_transfer_size = spdk_min(ctrlr->fw_size_remaining, ctrlr->min_page_size);

	res = nvme_ctrlr_cmd_fw_image_download(ctrlr, ctrlr->fw_transfer_size, ctrlr->fw_offset,
					       ctrlr->fw_payload, nvme_write_boot_partition_cb, ctrlr);

	return res;
}

bool
spdk_nvme_ctrlr_is_discovery(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr);

	return !strncmp(ctrlr->trid.subnqn, SPDK_NVMF_DISCOVERY_NQN,
			strlen(SPDK_NVMF_DISCOVERY_NQN));
}

bool
spdk_nvme_ctrlr_is_fabrics(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr);

	return spdk_nvme_trtype_is_fabrics(ctrlr->trid.trtype);
}

int
spdk_nvme_ctrlr_security_receive(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
				 uint16_t spsp, uint8_t nssf, void *payload, size_t size)
{
	struct nvme_completion_poll_status	*status;
	int					res;

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	res = spdk_nvme_ctrlr_cmd_security_receive(ctrlr, secp, spsp, nssf, payload, size,
			nvme_completion_poll_cb, status);
	if (res) {
		free(status);
		return res;
	}

	res = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (res) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for spdk_nvme_ctrlr_cmd_security_receive failed: rc=%s\n",
				  spdk_strerror(abs(res)));
	}

	return res;
}

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

uint64_t
spdk_nvme_ctrlr_get_flags(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->flags;
}

const struct spdk_nvme_transport_id *
spdk_nvme_ctrlr_get_transport_id(struct spdk_nvme_ctrlr *ctrlr)
{
	return &ctrlr->trid;
}

int32_t
spdk_nvme_ctrlr_alloc_qid(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t qid;

	assert(ctrlr->free_io_qids);
	nvme_ctrlr_lock(ctrlr);
	qid = spdk_bit_array_find_first_set(ctrlr->free_io_qids, 1);
	if (qid > ctrlr->opts.num_io_queues) {
		NVME_CTRLR_ERRLOG(ctrlr, "No free I/O queue IDs\n");
		nvme_ctrlr_unlock(ctrlr);
		return -1;
	}

	spdk_bit_array_clear(ctrlr->free_io_qids, qid);
	nvme_ctrlr_unlock(ctrlr);
	return qid;
}

void
spdk_nvme_ctrlr_free_qid(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid)
{
	assert(qid <= ctrlr->opts.num_io_queues);

	nvme_ctrlr_lock(ctrlr);

	if (spdk_likely(ctrlr->free_io_qids)) {
		spdk_bit_array_set(ctrlr->free_io_qids, qid);
	}

	nvme_ctrlr_unlock(ctrlr);
}

int
spdk_nvme_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_memory_domain **domains, int array_size)
{
	return nvme_transport_ctrlr_get_memory_domains(ctrlr, domains, array_size);
}

int
spdk_nvme_ctrlr_authenticate(struct spdk_nvme_ctrlr *ctrlr,
			     spdk_nvme_authenticate_cb cb_fn, void *cb_ctx)
{
	return spdk_nvme_qpair_authenticate(ctrlr->adminq, cb_fn, cb_ctx);
}
