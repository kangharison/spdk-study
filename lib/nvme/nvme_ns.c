/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 */

/*
 * [한국어 설명] SPDK NVMe Namespace 관리 구현 (nvme_ns.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 유저스페이스 NVMe 드라이버의 "네임스페이스(Namespace)" 객체 관리를 담당한다.
 * NVMe 스펙에서 네임스페이스는 컨트롤러가 노출하는 *논리 저장 공간 단위*로, 1-based 정수 NSID로
 * 식별되며 자체 LBA 공간·블록 크기·메타데이터·PI(Protection Information)·CSI(Command Set Identifier)
 * 를 가진다. 이 파일이 수행하는 핵심 책임은 다음 4가지다:
 *   1) Identify Namespace (CNS=0x00) admin 명령으로 NS의 raw 메타데이터(spdk_nvme_ns_data, 4KB) 취득
 *   2) Identify NS ID Descriptor List (CNS=0x03)로 NGUID/UUID/CSI 디스크립터 수집
 *   3) IOCS-Specific Identify NS (NVM=CSI 0, ZNS=CSI 2 등 명령 셋별 추가 메타데이터) 취득
 *   4) raw nsdata로부터 SPDK 내부에서 캐시할 cooked 필드(sector_size, md_size, flags, pi_type 등)
 *      를 계산하여 struct spdk_nvme_ns에 채워 넣음 (nvme_ns_set_identify_data)
 * 또한 외부(bdev_nvme 등)에 노출되는 getter 함수들(spdk_nvme_ns_get_*)이 모두 여기 거주하며,
 * NS 객체의 생성(nvme_ns_construct)과 파괴(nvme_ns_destruct) 라이프사이클 진입점도 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 유저스페이스 NVMe 스택의 *컨트롤러 bring-up* 단계에서 호출되는 후반부 모듈이다.
 *
 *   spdk_nvme_probe (lib/nvme/nvme.c)
 *     → nvme_ctrlr_construct (lib/nvme/nvme_ctrlr.c)
 *         → nvme_ctrlr_init 상태머신 진행 (Disable → AQA 설정 → Enable → Identify Ctrl)
 *             → nvme_ctrlr_identify_active_ns (Active NS list, CNS=0x02)
 *                 → 각 NSID에 대해 spdk_nvme_ctrlr_get_ns(ctrlr, nsid) 호출 (RB tree lazy alloc)
 *                     → ★ nvme_ns_construct(ns, id, ctrlr)   ← 본 파일 진입점
 *                         ├─ nvme_ctrlr_identify_ns          (Identify NS, CNS=0x00)
 *                         ├─ nvme_ctrlr_identify_id_desc     (NS ID Descriptor, CNS=0x03)
 *                         └─ nvme_ctrlr_identify_ns_iocs_specific (CSI 별 분기)
 *
 * 실행 컨텍스트: **호스트 유저스페이스 단일 SPDK thread** (spdk_nvme_probe 호출 thread). 모든 admin
 * 명령은 동기 polling(nvme_wait_for_adminq_completion)으로 완료를 기다리며, lock-free가 아니라
 * caller가 single-threaded임을 가정한다. probe 완료 후 attach_cb가 반환되면 사용자 thread가
 * 일반 I/O를 시작할 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존(이 파일이 호출하는 곳):
 *   · nvme_ctrlr_cmd_identify (nvme_ctrlr_cmd.c) — Identify admin 명령 발행기
 *   · nvme_completion_poll_cb / nvme_wait_for_adminq_completion (nvme.c) — 동기 완료 대기
 *   · spdk_nvme_ns_get_format_index — 공개 API(헤더에서 inline 후보)
 *   · spdk_zmalloc / spdk_free (env_dpdk) — hugepage 기반 NUMA-aware 할당 (DMA 호환)
 * - 의존받음(이 파일을 호출하는 곳):
 *   · nvme_ctrlr.c의 ctrlr 초기화 시퀀스 (nvme_ns_construct, nvme_ns_destruct)
 *   · 모든 사용자 코드 (bdev_nvme, examples, perf 등) — spdk_nvme_ns_get_* getter들
 *   · AER NS Attribute Notice 핸들러 — nvme_ns_set_identify_data 재호출 가능 (NS 변화 감지 시)
 * - 공유 자료구조:
 *   · struct spdk_nvme_ns (정의: nvme_internal.h:916) — NS 내부 표현. ctrlr 역참조 + RB tree 노드.
 *     컨트롤러 객체의 ctrlr->ns RB tree 안에 NSID-key로 등록됨.
 *   · struct spdk_nvme_ns_data (스펙 정의: include/spdk/nvme_spec.h) — Identify NS의 raw 4KB 응답.
 *
 * === 주요 함수/구조체 요약 ===
 *  - nvme_ns_construct(): 단일 NS 객체 초기화 진입점. Identify NS → ID Descriptor → IOCS-specific
 *                         3단계 admin 호출을 직렬로 수행하고 cooked 필드를 채운다. 이 함수가
 *                         호출되어야 spdk_nvme_ns_get_*이 의미 있는 값을 반환한다.
 *  - nvme_ns_set_identify_data(): nsdata raw로부터 sector_size·md_size·flags·pi_type·sectors_per_max_io
 *                         등을 계산. 컨트롤러 capability(cdata.oncs/vwc/ctratt) 와 quirk를 함께
 *                         반영. NS Attribute Notice AER 처리 시 재호출되어 변경을 반영한다.
 *  - nvme_ctrlr_identify_ns(): Identify NS (CNS=0x00) admin 명령 한 번 발행 + 동기 대기.
 *                         실패 시 NS 비활성으로 간주하고 nsdata를 0으로 비운다 (호환성 정책).
 *  - nvme_ctrlr_identify_id_desc(): Identify NS ID Descriptor List (CNS=0x03) — NGUID/UUID/CSI
 *                         디스크립터 4KB를 ns->id_desc_list 버퍼에 저장. NVMe 1.3+ 한정.
 *  - spdk_nvme_ns_get_*(): 외부에 노출되는 read-only getter들. 잠금 없는 단순 필드 반환.
 *  - nvme_ns_find_id_desc(): id_desc_list 파싱 — 가변 길이 디스크립터들을 순회하며 type 매칭.
 *  - nvme_ns_destruct(): NS 비활성/해제 시 호출. nsdata·id_desc_list·iocs-specific 영역을 0으로
 *                         비우고 cooked 필드를 모두 reset (재구성 시 잔존 값에 의한 오염 방지).
 *  - struct spdk_nvme_ns: ctrlr 역참조, NSID, 캐시된 IO 파라미터(sector_size 등), nsdata 4KB,
 *                         id_desc_list 4KB, ZNS/NVM IOCS 데이터 포인터, RB tree 노드를 포함.
 *                         정의 위치는 nvme_internal.h:916 — 본 파일은 그 필드들의 *읽기/쓰기 진입점*.
 */

/* [한국어] nvme_internal.h: SPDK NVMe 드라이버의 모든 내부 자료구조와 사적 함수 선언이 모인
 *  중앙 헤더 — struct spdk_nvme_ctrlr/ns/qpair, RB tree 매크로, completion poll 헬퍼,
 *  로깅 매크로(NVME_CTRLR_*LOG), quirk 비트 정의 등이 모두 여기에서 노출된다. */
#include "nvme_internal.h"

/*
 * [한국어]
 * _nvme_ns_get_data - NS의 raw Identify Namespace 데이터(spdk_nvme_ns_data 4KB) 포인터 반환
 *
 * @ns: 대상 NS 객체 (소속 ctrlr 포함, NSID 설정됨)
 * @return: ns->nsdata 의 주소 (NS 객체 내부에 임베드된 4KB nsdata 영역)
 *
 * 이 함수가 왜 필요한가:
 *   - SPDK 내부 코드 곳곳에서 nsdata에 접근할 때 ns->nsdata 직접 표기보다 함수 호출 형태로
 *     일관성을 맞추고, 추후 nsdata 위치/접근 방식 변경 시 한 점에서 수정 가능하도록 추상화.
 *   - 외부 공개용은 spdk_nvme_ns_get_data() (이 파일 후반)이며, 내부 전용은 underscore prefix.
 * 동작:
 *   - 단순 포인터 반환 — &ns->nsdata. inline static이라 호출 오버헤드 0.
 * 실행 컨텍스트:
 *   - 호스트 SPDK thread 어디서든 호출 가능. nsdata 자체는 NS 객체 메모리에 임베드되어 있어
 *     수명은 NS와 동일하므로 별도 동기화 불필요 (NS 객체 자체가 ctrlr_lock으로 보호됨).
 * Caller / Callee:
 *   - Caller: nvme_ns_set_identify_data, nvme_ctrlr_identify_ns, spdk_nvme_ns_get_data, ...
 *   - Callee: 없음 (단순 멤버 접근)
 */
static inline struct spdk_nvme_ns_data *
_nvme_ns_get_data(struct spdk_nvme_ns *ns)
{
	/* [한국어] NS 객체에 임베드된 nsdata 4KB 영역의 주소 반환 — Identify NS 응답 원본을 그대로 보관.
	 *  · nvme_internal.h:968 의 struct spdk_nvme_ns_data nsdata 필드를 가리킴. */
	return &ns->nsdata;
}

/**
 * Update Namespace flags based on Identify Controller
 * and Identify Namespace.  This can be also used for
 * Namespace Attribute Notice events and Namespace
 * operations such as Attach/Detach.
 */
/*
 * [한국어]
 * nvme_ns_set_identify_data - raw Identify NS 응답으로부터 SPDK 내부 cooked 필드 채우기
 *
 * @ns: 대상 NS — ns->nsdata 가 이미 채워져 있어야 하고 ns->ctrlr 도 유효해야 함
 *      (호출 직전에 nvme_ctrlr_identify_ns 가 성공해야 한다는 사전 조건)
 * @return: 없음 (void) — 실패 케이스 없음. 모든 분기는 NS 속성 비트 설정 또는 reset.
 *
 * 이 함수가 왜 필요한가 (동기/배경):
 *   - Identify NS의 raw 4KB는 비트필드 패킹·인덱스 참조(LBAF 배열)·컨트롤러 capability와의 결합
 *     계산이 복잡함. 매 IO 호출마다 raw를 파싱하면 hot path가 무거워지므로, 한 번 cooked 형태로
 *     캐시해 두고 그 후엔 단순 멤버 접근으로 처리하기 위함.
 *   - 또한 NS Attribute Notice AER (Asynchronous Event — NS의 capacity/format/PI 등이 바뀌었음을
 *     알림)을 받으면 nsdata를 다시 읽고 이 함수를 재호출하여 캐시를 갱신해야 한다.
 * 동작 단계:
 *   1) flags 0으로 reset, format_index 산출 (nlbaf < 16 분기 / NVMe 2.0 msb_format)
 *   2) sector_size = 2^(LBAF.lbads), extended_lba_size 초기값으로 sector_size 복사
 *   3) md_size = LBAF.ms (metadata size). FLBAS.extended=1 이면 extended LBA로 간주하여
 *      extended_lba_size에 md_size 합산 + EXTENDED_LBA_SUPPORTED flag set
 *   4) sectors_per_max_io 계산 — MDTS 기반 max_xfer_size를 LBA 크기로 나눔
 *      · NVME_QUIRK_MDTS_EXCLUDE_MD quirk: MDTS가 metadata를 포함하지 않는다고 보고 _no_md 사용
 *   5) sectors_per_stripe — NOIOB(Namespace Optimal IO Boundary) 우선, 없으면 Intel quirk 보정
 *   6) 컨트롤러 ONCS/VWC capability 비트로부터 NS의 지원 기능 flag 도출
 *      (Dataset Mgmt/Compare/Flush/Write Zeroes/Write Uncorrectable/Reservation)
 *   7) PI(Protection Information) — metadata가 있고 DPS.pit 비활성이 아니면 PI 활성 분기로
 *      pi_type 설정. NVMe 2.0 elbas(extended LBA format support)면 nsdata_nvm->elbaf로 pif 결정,
 *      아니면 기본값 16B_GUARD_PI.
 *   8) ns->active = spdk_nvme_ns_is_active(ns) — NCAP 0인지 검사 (비활성 NS 식별)
 * 실행 컨텍스트:
 *   - bring-up 단계 또는 AER 핸들러 단계의 *호스트 SPDK thread*에서만 호출. 동시 호출은 ctrlr_lock
 *     이 외부에서 보장한다고 가정 (이 함수 자체는 잠금 안 잡음).
 * Caller / Callee:
 *   - Caller: nvme_ctrlr_identify_ns (성공 시 마지막에), AER NS Attribute Notice 핸들러
 *   - Callee: spdk_nvme_ns_get_format_index, spdk_nvme_ns_get_max_io_xfer_size,
 *             NVME_CTRLR_DEBUGLOG (로깅), spdk_nvme_ns_is_active
 */
void
nvme_ns_set_identify_data(struct spdk_nvme_ns *ns)
{
	/* [한국어] raw nsdata 포인터 — Identify NS 응답 4KB 원본. lbaf[] 배열, flbas, dps, oncs 등 비트필드 포함. */
	struct spdk_nvme_ns_data	*nsdata;
	/* [한국어] NVM Command Set Specific Identify NS 데이터 (NVMe 2.0 elbas=1 일 때만 유효).
	 *  elbaf[] 배열로 PI format(pif: 16/32/64-bit Guard) 정보를 제공. */
	struct spdk_nvme_nvm_ns_data	*nsdata_nvm;
	/* [한국어] 현재 활성 LBA Format의 인덱스 (0..63). nsdata->lbaf[format_index] 로 sector size·md size 조회. */
	uint32_t			format_index;

	/* [한국어] 임베디드된 nsdata 영역 주소 획득 (단순 &ns->nsdata). */
	nsdata = _nvme_ns_get_data(ns);
	/* [한국어] NVM IOCS-specific 데이터 포인터 캐시. nvme_ctrlr_identify_ns_nvm_specific 가 별도로 할당해 둠.
	 *  elbas=0 인 컨트롤러에선 NULL일 수 있음. */
	nsdata_nvm = ns->nsdata_nvm;

	/* [한국어] 모든 NS feature flag를 0으로 reset — 아래 분기들이 비트 OR로 다시 설정.
	 *  AER 재호출 시에도 깨끗한 상태에서 다시 계산하기 위함. */
	ns->flags = 0x0000;
	/* [한국어] FLBAS의 format 비트필드로부터 사용 중인 LBA Format 인덱스 도출.
	 *  · nlbaf < 16: 4비트 (legacy)   · nlbaf >= 16 (NVMe 2.0): msb_format(4비트) << 4 | format(4비트). */
	format_index = spdk_nvme_ns_get_format_index(nsdata);

	/* [한국어] sector_size = 2^(LBAF.lbads). 일반적으로 lbads=9(512B) 또는 12(4096B). */
	ns->sector_size = 1 << nsdata->lbaf[format_index].lbads;
	/* [한국어] extended_lba_size 초기값으로 sector_size 복사. extended LBA 분기에서 md_size 합산될 수 있음. */
	ns->extended_lba_size = ns->sector_size;

	/* [한국어] metadata size(바이트). PI 비활성이면 0. PI 활성 + Type1/2/3 이면 일반적으로 8 (Guard 2 + AppTag 2 + RefTag 4). */
	ns->md_size = nsdata->lbaf[format_index].ms;
	/* [한국어] FLBAS.extended=1: metadata가 LBA 데이터 말미에 inline으로 붙는 모드.
	 *  · 0 이면 분리형(Separate metadata pointer를 SQE의 MPTR/cdw14-15 로 따로 전달). */
	if (nsdata->flbas.extended) {
		/* [한국어] flag에 EXTENDED_LBA 지원 표시 — 사용자가 buffer 크기 계산 시 sector_size+md_size 사용해야 함을 통지. */
		ns->flags |= SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED;
		/* [한국어] extended LBA 모드의 실제 블록 크기 = sector_size + md_size (예: 512+8=520, 4096+64=4160). */
		ns->extended_lba_size += ns->md_size;
	}

	/* [한국어] 단일 IO로 전송 가능한 최대 섹터 수 — MDTS 기반 max_xfer_size 를 extended_lba_size 로 나눔.
	 *  · 호출자가 이 값보다 큰 IO를 요청하면 SPDK가 child request로 split. */
	ns->sectors_per_max_io = spdk_nvme_ns_get_max_io_xfer_size(ns) / ns->extended_lba_size;
	/* [한국어] metadata 제외 단일 IO 최대 섹터 — separate metadata 모드 또는 MDTS_EXCLUDE_MD quirk에서 사용. */
	ns->sectors_per_max_io_no_md = spdk_nvme_ns_get_max_io_xfer_size(ns) / ns->sector_size;
	/* [한국어] NVME_QUIRK_MDTS_EXCLUDE_MD: 일부 컨트롤러는 MDTS에 metadata 크기를 포함하지 않는다고 보고함.
	 *  이 경우 sector_size 기준으로 계산해야 실제 전송 한계를 초과하지 않음. */
	if (ns->ctrlr->quirks & NVME_QUIRK_MDTS_EXCLUDE_MD) {
		/* [한국어] MDTS quirk 적용 — sectors_per_max_io 를 _no_md 값으로 덮어씀. */
		ns->sectors_per_max_io = ns->sectors_per_max_io_no_md;
	}

	/* [한국어] NOIOB(Namespace Optimal IO Boundary) — 0 이 아니면 권장 stripe 경계 (블록 단위).
	 *  사용자가 NOIOB 경계를 가로지르는 IO를 피하면 일부 SSD에서 성능 향상. */
	if (nsdata->noiob) {
		/* [한국어] NOIOB 값 그대로 stripe 크기로 사용. */
		ns->sectors_per_stripe = nsdata->noiob;
		/* [한국어] DEBUG 로그 — NS별 권장 stripe 경계 기록 (운영 시엔 출력 안 됨). */
		NVME_CTRLR_DEBUGLOG(ns->ctrlr, "ns %u optimal IO boundary %" PRIu32 " blocks\n", ns->id,
				    ns->sectors_per_stripe);
	/* [한국어] Intel-specific striping quirk — 일부 Intel SSD가 cdata.vs[3] 에 stripe 힌트를 vendor specific 영역에 둠.
	 *  vs[3]==0 이면 quirk 비활성. NOIOB가 표준이라 보통 이 분기는 legacy 드라이브 전용. */
	} else if (ns->ctrlr->quirks & NVME_INTEL_QUIRK_STRIPING &&
		   ns->ctrlr->cdata.vs[3] != 0) {
		/* [한국어] stripe = 2^vs[3] * min_page_size / sector_size — 페이지 단위를 LBA 단위로 환산. */
		ns->sectors_per_stripe = (1ULL << ns->ctrlr->cdata.vs[3]) * ns->ctrlr->min_page_size /
					 ns->sector_size;
		/* [한국어] DEBUG 로그 — Intel quirk로 도출된 stripe 크기 기록. */
		NVME_CTRLR_DEBUGLOG(ns->ctrlr, "ns %u stripe size quirk %" PRIu32 " blocks\n", ns->id,
				    ns->sectors_per_stripe);
	} else {
		/* [한국어] 둘 다 해당 없음 — stripe 0 (제약 없음). */
		ns->sectors_per_stripe = 0;
	}

	/* [한국어] ONCS.nvmdsmsv: Dataset Management(SPDK_NVME_OPC_DATASET_MANAGEMENT) 지원 — TRIM/Deallocate 가능. */
	if (ns->ctrlr->cdata.oncs.nvmdsmsv) {
		/* [한국어] DEALLOCATE flag set — 사용자가 spdk_nvme_ns_cmd_dataset_management 호출 가능함을 표시. */
		ns->flags |= SPDK_NVME_NS_DEALLOCATE_SUPPORTED;
	}

	/* [한국어] ONCS.nvmcmps: Compare 명령 지원 — 데이터 일치 검증용 read-modify 패턴. */
	if (ns->ctrlr->cdata.oncs.nvmcmps) {
		/* [한국어] COMPARE flag set. */
		ns->flags |= SPDK_NVME_NS_COMPARE_SUPPORTED;
	}

	/* [한국어] VWC.present: Volatile Write Cache 가 컨트롤러에 존재 → Flush 명령 의미 있음.
	 *  · VWC 없으면 Flush는 no-op이지만 SPDK는 flag로 명시적으로 노출. */
	if (ns->ctrlr->cdata.vwc.present) {
		/* [한국어] FLUSH flag set — bdev_nvme의 Flush 우회 결정에 사용. */
		ns->flags |= SPDK_NVME_NS_FLUSH_SUPPORTED;
	}

	/* [한국어] ONCS.nvmwzsv: Write Zeroes 명령 지원 — payload 없이 LBA range를 0으로 채우는 효율적 명령. */
	if (ns->ctrlr->cdata.oncs.nvmwzsv) {
		/* [한국어] WRITE_ZEROES flag set. */
		ns->flags |= SPDK_NVME_NS_WRITE_ZEROES_SUPPORTED;
	}

	/* [한국어] ONCS.nvmwusv: Write Uncorrectable 명령 지원 — 의도적으로 LBA를 unrecoverable error 상태로 표시. */
	if (ns->ctrlr->cdata.oncs.nvmwusv) {
		/* [한국어] WRITE_UNCORRECTABLE flag set — 주로 시뮬레이션/테스트 용도. */
		ns->flags |= SPDK_NVME_NS_WRITE_UNCORRECTABLE_SUPPORTED;
	}

	/* [한국어] NSRESCAP: Reservation Capability — Persistent Reservation 명령 지원 여부의 비트 마스크.
	 *  · raw != 0 이면 어떤 형태로든 reservation 지원. */
	if (nsdata->nsrescap.raw) {
		/* [한국어] RESERVATION flag set — multi-host 시나리오에서 NS lock 사용 가능 표시. */
		ns->flags |= SPDK_NVME_NS_RESERVATION_SUPPORTED;
	}

	/* [한국어] PI 기본값 disable로 초기화 — 아래 조건 만족 시에만 유효 type으로 변경. */
	ns->pi_type = SPDK_NVME_FMT_NVM_PROTECTION_DISABLE;
	/* [한국어] PI 활성 조건: metadata가 있고(LBAF.ms!=0) DPS.pit가 0이 아님(Type 1/2/3).
	 *  · DPS(Data Protection Settings) 의 pit 필드가 PI Type을 직접 인코딩. */
	if (nsdata->lbaf[format_index].ms && nsdata->dps.pit) {
		/* [한국어] DPS_PI flag set — 사용자가 PI 메타데이터를 직접 제공/검증해야 함을 표시. */
		ns->flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;
		/* [한국어] PI Type 캐시 — 1/2/3 (각각 Logical Block 매 영역, Logical Block, RefTag 자동증가 등의 의미). */
		ns->pi_type = nsdata->dps.pit;
		/* [한국어] elbas 분기: NVMe 2.0의 Extended LBA Format Support — 16/32/64-bit Guard 선택 가능.
		 *  nsdata_nvm 가 NULL 이거나 elbas=0 이면 안전한 기본값(16B Guard)으로 fallback. */
		if (nsdata_nvm != NULL && ns->ctrlr->cdata.ctratt.bits.elbas) {
			/* We may have nsdata_nvm for other purposes but
			 * the elbaf array is only valid when elbas is 1.
			 */
			/* [한국어] elbaf[format_index].pif 로 PI Format 결정 — 16B/32B/64B Guard 중 하나. */
			ns->pi_format = nsdata_nvm->elbaf[format_index].pif;
		} else {
			/* [한국어] 호환성 기본값 — 전통 16-bit CRC Guard. NVMe 1.x 모든 드라이브가 지원. */
			ns->pi_format = SPDK_NVME_16B_GUARD_PI;
		}
	}

	/* [한국어] 활성 여부 판정 — NCAP(Namespace Capacity) 가 0이 아니면 active.
	 *  · Identify NS는 비활성 NSID에 대해 zero-filled 응답을 돌려주므로 NCAP 검사가 active 판단의 표준. */
	ns->active = spdk_nvme_ns_is_active(ns);
}

/*
 * [한국어]
 * nvme_ctrlr_identify_ns - Identify Namespace (CNS=0x00) admin 명령 발행 + 동기 대기
 *
 * @ns: 대상 NS — ns->ctrlr 와 ns->id (NSID) 가 설정되어 있어야 함. nsdata 영역은 이 함수가 채움.
 * @return: 0 성공 (NS 비활성으로 판명되어도 0 반환 — 비활성도 정상 시나리오), 음수 errno 실패
 *           · -ENOMEM: status tracker 할당 실패
 *           · nvme_ctrlr_cmd_identify 의 즉시 반환 에러 (qpair 미준비 등)
 *
 * 이 함수가 왜 필요한가:
 *   - NVMe 컨트롤러로부터 NS의 4KB raw 메타데이터를 가져오는 *유일한 진입점*. 이 데이터 없이는
 *     LBA 크기·총 용량·PI 등 어떤 IO 파라미터도 결정 불가.
 *   - 비동기 admin 인터페이스(nvme_ctrlr_cmd_identify)를 동기 대기로 감싸는 wrapper 역할.
 *     bring-up 단계는 직렬 시퀀스이므로 동기가 자연스러움.
 * 동작:
 *   1) status tracker 할당 (nvme_completion_poll_status — 완료 통지·결과 보존용)
 *   2) Identify NS admin command 발행 (CNS=0x00, NSID=ns->id, CSI=0)
 *      · payload는 ns->nsdata (4KB), spec 정의된 spdk_nvme_ns_data 레이아웃
 *   3) nvme_wait_for_adminq_completion 으로 admin completion 동기 대기 (polling)
 *   4) 실패 시 (예: 비활성 NS 또는 일시적 오류): nsdata를 0으로 지우고 0 리턴 (호환성 정책)
 *   5) 성공 시 nvme_ns_set_identify_data 로 cooked 필드 채움
 * 실행 컨텍스트:
 *   - bring-up 시점의 호스트 SPDK thread (caller가 ctrlr_lock을 잡고 진입한 상태)
 *   - 동기 대기 중에 admin qpair polling이 일어나므로 그 사이 다른 admin 발행은 차단됨
 * Caller / Callee:
 *   - Caller: nvme_ns_construct (NS 객체 초기화의 1단계)
 *   - Callee: nvme_ctrlr_cmd_identify (admin 명령 발행), nvme_completion_poll_cb (완료 콜백),
 *             nvme_wait_for_adminq_completion (동기 대기), nvme_ns_destruct (실패 시 정리),
 *             nvme_ns_set_identify_data (성공 시 cooked 변환)
 *
 * 호출 체인:
 *   nvme_ctrlr_init → nvme_ns_construct → [nvme_ctrlr_identify_ns]
 *     → nvme_ctrlr_cmd_identify → admin SQ enqueue → SSD → CQE
 *     → nvme_wait_for_adminq_completion → status->done
 *     → nvme_ns_set_identify_data
 */
static int
nvme_ctrlr_identify_ns(struct spdk_nvme_ns *ns)
{
	/* [한국어] admin 완료를 polling 방식으로 동기 대기하기 위한 추적자.
	 *  · status->done(완료 플래그) + status->cpl(원본 CQE 사본)을 보관. */
	struct nvme_completion_poll_status	*status;
	/* [한국어] Identify NS payload 버퍼 — NS 객체에 임베드된 4KB nsdata. SSD가 여기에 DMA로 직접 write. */
	struct spdk_nvme_ns_data		*nsdata;
	/* [한국어] return code — 0 성공, 음수 errno. */
	int					rc;

	/* [한국어] status tracker 할당 — 일반 calloc 사용 (admin completion만 추적, DMA payload 아님).
	 *  · 1 sizeof zero-fill — done=false, cpl=0 으로 시작. */
	status = calloc(1, sizeof(*status));
	/* [한국어] 메모리 부족 — bring-up 초기 단계라 이 시점에 ENOMEM은 시스템 거의 불가능 상태. */
	if (!status) {
		/* [한국어] 컨트롤러 식별 prefix 포함 ERR 로그 — 어느 ctrlr에서 발생인지 즉시 식별. */
		NVME_CTRLR_ERRLOG(ns->ctrlr, "Failed to allocate status tracker\n");
		/* [한국어] errno 표준값으로 실패 알림 — caller(nvme_ns_construct)는 NS 구성 중단. */
		return -ENOMEM;
	}

	/* [한국어] nsdata payload 주소 획득 (단순 &ns->nsdata). 이 버퍼에 SSD가 4KB DMA write 수행. */
	nsdata = _nvme_ns_get_data(ns);
	/* [한국어] Identify admin 명령 발행:
	 *   · CNS = SPDK_NVME_IDENTIFY_NS (0x00) — NS 단위 identify
	 *   · cntid = 0 (no controller filter)
	 *   · nsid = ns->id — 대상 namespace
	 *   · csi = 0 (NVM command set 기본 — IOCS-specific은 별도 함수에서)
	 *   · 응답 buffer = nsdata, 크기 = sizeof(spdk_nvme_ns_data) = 4096
	 *   · 완료 시 nvme_completion_poll_cb 가 status->done 을 set 하고 status->cpl 에 CQE 복사. */
	rc = nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS, 0, ns->id, 0,
				     nsdata, sizeof(*nsdata),
				     nvme_completion_poll_cb, status);
	/* [한국어] 명령 발행 자체가 실패 — admin SQ가 가득 찼거나 ctrlr 상태 이상.
	 *  · 이 경우 status는 사용되지 않았으므로 free 후 즉시 errno 전파. */
	if (rc != 0) {
		/* [한국어] 사용 안 한 status tracker 해제 — leak 방지. */
		free(status);
		/* [한국어] caller에 원인 errno 그대로 전달. */
		return rc;
	}

	/* [한국어] admin queue를 polling하여 status->done 이 true가 될 때까지 대기.
	 *  · 두 번째 인자 true: SPDK_DEBUGLOG로 fail 메시지 출력 옵션. */
	rc = nvme_wait_for_adminq_completion(ns->ctrlr, status, true);
	/* [한국어] 완료 자체는 받았으나 NVMe SC 에러(Status Code != 0) — 비활성 NS 가능성 큼.
	 *  · 비활성 NS는 spec상 zero-filled 응답이지만 일부 드라이브는 Invalid Field 에러로 응답하기도 함.
	 *  · 어느 쪽이든 SPDK는 "이 NS는 사용 불가" 처리하고 NS 구성을 0(성공)으로 종료시켜
	 *    상위가 NS list 순회를 계속할 수 있게 한다 — 호환성 우선 정책. */
	if (rc) {
		/* This can occur if the namespace is not active. Simply zero the
		 * namespace data and continue. */
		/* [한국어] 일반 ERR 로그(컨트롤러 prefix 없음) — strerror로 errno 디코딩. */
		SPDK_ERRLOG("wait for nvme_ctrlr_cmd_identify failed: rc=%s\n", spdk_strerror(abs(rc)));
		/* [한국어] NS 객체의 모든 캐시 필드와 nsdata 4KB 영역을 0으로 리셋 — 잔존 데이터로 인한 오염 방지. */
		nvme_ns_destruct(ns);
		/* [한국어] caller에는 0 (성공) 반환 — bring-up 시퀀스가 다음 NSID로 진행되도록.
		 *  · ns->active = false 가 destruct로 자동 설정되므로 추후 IO 차단됨. */
		return 0;
	}

	/* [한국어] nsdata raw → cooked 필드 변환 (sector_size, md_size, flags, pi_type 등 캐시).
	 *  · 이 호출 후에야 spdk_nvme_ns_get_* getter들이 의미 있는 값 반환. */
	nvme_ns_set_identify_data(ns);
	/* [한국어] status tracker leak 발생 — calloc된 status를 free하지 않음.
	 *  · 주의: 원본 SPDK 코드의 동작이며, nvme_wait_for_adminq_completion 성공 경로에서
	 *    status를 호출자가 free하는 것이 SPDK 규약. (free 책임은 caller인 본 함수가 가진다는 가정인데
	 *    여기선 누락 — 잠재적 leak. 단, bring-up 시 1회만 호출되어 영향 미미.) */
	return 0;
}

/*
 * [한국어]
 * nvme_ctrlr_identify_ns_zns_specific - ZNS 명령 셋 전용 Identify NS (CNS=0x05) 발행
 *
 * @ns: 대상 NS — ns->csi 가 SPDK_NVME_CSI_ZNS 로 이미 결정되어 있어야 함 (ID Descriptor에서 도출)
 * @return: 0 성공 (ns->nsdata_zns 에 ZNS 전용 데이터 저장됨), 음수 errno 실패
 *
 * 이 함수가 왜 필요한가:
 *   - ZNS(Zoned Namespace, NVMe TP-4053)는 NVM 명령 셋 위에 zone 구조를 추가한 새 명령 셋.
 *     기본 Identify NS(CNS=0x00) 외에 zone size, max active zones, zoc(Zone Open Cap) 등
 *     ZNS-specific 메타데이터를 별도로 가져와야 ZNS IO를 발행할 수 있음.
 *   - SPDK는 이 데이터를 별도 4KB 버퍼에 저장하고 ns->nsdata_zns 포인터로 연결.
 * 동작:
 *   1) 기존 nsdata_zns 가 있으면 해제 (재호출 안전성)
 *   2) DMA 호환 4KB 버퍼 할당 (spdk_zmalloc — hugepage 기반, NUMA-aware, 64B align)
 *   3) Identify NS IOCS-Specific (CNS=0x05) admin 발행 — CSI=ZNS 로 ZNS 데이터 요청
 *   4) 동기 대기 → 성공 시 ns->nsdata_zns 에 포인터 저장
 * 실행 컨텍스트: bring-up 시점 호스트 SPDK thread (caller가 ctrlr_lock 보유)
 * Caller / Callee:
 *   - Caller: nvme_ctrlr_identify_ns_iocs_specific (CSI 분기)
 *   - Callee: nvme_ns_free_zns_specific_data (기존 정리), spdk_zmalloc (hugepage 할당),
 *             nvme_ctrlr_cmd_identify, nvme_wait_for_adminq_completion
 */
static int
nvme_ctrlr_identify_ns_zns_specific(struct spdk_nvme_ns *ns)
{
	/* [한국어] admin 완료 추적자. */
	struct nvme_completion_poll_status *status;
	/* [한국어] ctrlr 포인터 캐시 — ns->ctrlr 반복 deref 회피용 가독성 변수. */
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	/* [한국어] ZNS 전용 identify 데이터 버퍼 — 4KB DMA 호환 메모리.
	 *  · spdk_nvme_zns_ns_data: zone size, mar(Max Active Resources), mor 등 포함. */
	struct spdk_nvme_zns_ns_data *nsdata_zns;
	/* [한국어] return code. */
	int rc;

	/* [한국어] 재호출 안전성 — 이전에 할당된 ZNS 데이터가 있으면 먼저 해제.
	 *  · AER NS Attribute Notice로 재호출되는 케이스 대비. */
	nvme_ns_free_zns_specific_data(ns);

	/* [한국어] DMA 호환 4KB 버퍼 할당:
	 *   · size = sizeof(spdk_nvme_zns_ns_data) (4096B)
	 *   · align = 64 (cacheline align — 호스트 캐시·DMA 정렬)
	 *   · NUMA = ANY (any NUMA node 허용)
	 *   · MALLOC_SHARE = multi-process 공유 가능 hugepage (SPDK secondary process도 접근). */
	nsdata_zns = spdk_zmalloc(sizeof(*nsdata_zns), 64, NULL, SPDK_ENV_NUMA_ID_ANY,
				  SPDK_MALLOC_SHARE);
	/* [한국어] hugepage 풀 고갈 — 시스템 hugepage 부족 또는 SPDK env 미초기화. */
	if (!nsdata_zns) {
		/* [한국어] errno 전파 — caller가 NS의 ZNS 활성화를 포기. */
		return -ENOMEM;
	}

	/* [한국어] status tracker는 일반 RAM (DMA 대상 아님) — calloc로 충분. */
	status = calloc(1, sizeof(*status));
	if (!status) {
		/* [한국어] ERR 로그 with ctrlr context. */
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		/* [한국어] 이미 할당된 ZNS 버퍼 leak 방지 — spdk_free로 hugepage 풀에 반환. */
		spdk_free(nsdata_zns);
		return -ENOMEM;
	}

	/* [한국어] Identify NS IOCS-Specific:
	 *   · CNS = SPDK_NVME_IDENTIFY_NS_IOCS (0x05) — IOCS specific NS data
	 *   · csi = ns->csi (= SPDK_NVME_CSI_ZNS) — ZNS 명령 셋 식별자 명시
	 *   · 응답을 nsdata_zns에 4KB DMA. */
	rc = nvme_ctrlr_cmd_identify(ctrlr, SPDK_NVME_IDENTIFY_NS_IOCS, 0, ns->id, ns->csi,
				     nsdata_zns, sizeof(*nsdata_zns),
				     nvme_completion_poll_cb, status);
	/* [한국어] 발행 실패 — 양쪽 자원 모두 정리. */
	if (rc != 0) {
		/* [한국어] ZNS payload 버퍼 반환 (hugepage). */
		spdk_free(nsdata_zns);
		/* [한국어] status tracker 반환 (heap). */
		free(status);
		/* [한국어] errno 전파. */
		return rc;
	}

	/* [한국어] 동기 polling 대기. */
	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	/* [한국어] SC 에러 등 — ZNS specific data 취득 실패. NS 자체는 살아 있을 수 있음. */
	if (rc) {
		/* [한국어] ERR 로그 — strerror 변환. */
		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_ctrlr_cmd_identify failed: %s\n", spdk_strerror(abs(rc)));
		/* [한국어] ZNS 버퍼 해제 (status leak — 위 nvme_ctrlr_identify_ns 와 동일 패턴). */
		spdk_free(nsdata_zns);
		/* [한국어] -ENXIO: 디바이스 응답 자체가 비정상 — caller 측 ZNS 비활성화. */
		return -ENXIO;
	}

	/* [한국어] 성공 — ZNS 데이터를 NS 객체에 연결. ZNS IO는 이 포인터를 통해 zone size 등 참조. */
	ns->nsdata_zns = nsdata_zns;
	return 0;
}

/*
 * [한국어]
 * nvme_ctrlr_identify_ns_nvm_specific - NVM 명령 셋 IOCS-specific Identify NS (CNS=0x05) 발행
 *
 * @ns: 대상 NS — ns->csi == SPDK_NVME_CSI_NVM 이고 컨트롤러의 ctratt.elbas=1 인 경우만 의미 있음
 * @return: 0 성공 (ns->nsdata_nvm 채워짐), 음수 errno 실패
 *
 * 이 함수가 왜 필요한가:
 *   - NVMe 2.0의 ELBAS(Extended LBA Format Support) — LBA Format을 16/32/64-bit Guard 등 다양한
 *     PI Format 으로 확장한 기능. NVM 명령 셋에서도 IOCS-specific identify로 elbaf[] 배열을
 *     별도로 가져와야 PI Format(pif)을 정확히 알 수 있음.
 *   - 이 데이터가 없으면 nvme_ns_set_identify_data 가 pi_format을 기본값(16B Guard)으로 fallback.
 * 동작: ZNS variant와 동일한 패턴 (할당 → identify 발행 → 동기 대기 → 포인터 저장)
 * 실행 컨텍스트: bring-up 시점 호스트 SPDK thread
 * Caller / Callee:
 *   - Caller: nvme_ctrlr_identify_ns_iocs_specific (CSI=NVM 분기 + elbas=1 조건)
 *   - Callee: ZNS variant와 동일
 *
 * 주의: 함수 시작에서 nvme_ns_free_zns_specific_data() 호출은 이름과 달리 ZNS 데이터 해제이며
 *  본 함수의 NVM specific data와는 무관. 의도는 "이 NS는 NVM CSI로 결정되었으므로 이전에 잘못
 *  ZNS로 잡혀 있던 데이터가 있으면 청소" — 컨트롤러 reset/재구성 시퀀스 안전성. (혹은 코드의 약간의
 *  copy-paste 흔적일 수 있으나 결과적으로 안전 동작.)
 */
static int
nvme_ctrlr_identify_ns_nvm_specific(struct spdk_nvme_ns *ns)
{
	/* [한국어] admin 완료 추적자. */
	struct nvme_completion_poll_status *status;
	/* [한국어] ctrlr 캐시. */
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	/* [한국어] NVM IOCS-specific 데이터 4KB 버퍼 — elbaf[64] 배열 등 포함. */
	struct spdk_nvme_nvm_ns_data *nsdata_nvm;
	/* [한국어] return code. */
	int rc;

	/* [한국어] 이전 ZNS 잔존 데이터 정리 (CSI 변경 시 안전 — 위 주의 사항 참조). */
	nvme_ns_free_zns_specific_data(ns);

	/* [한국어] 4KB DMA 호환 hugepage 할당 — ZNS variant와 동일 옵션. */
	nsdata_nvm = spdk_zmalloc(sizeof(*nsdata_nvm), 64, NULL, SPDK_ENV_NUMA_ID_ANY,
				  SPDK_MALLOC_SHARE);
	/* [한국어] 할당 실패 — hugepage 부족. */
	if (!nsdata_nvm) {
		return -ENOMEM;
	}

	/* [한국어] status tracker (일반 heap). */
	status = calloc(1, sizeof(*status));
	if (!status) {
		/* [한국어] ERR 로그 with ctrlr prefix. */
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		/* [한국어] 할당된 NVM 버퍼 leak 방지. */
		spdk_free(nsdata_nvm);
		return -ENOMEM;
	}

	/* [한국어] Identify NS IOCS-Specific (CNS=0x05), CSI=NVM(=0):
	 *   · 응답으로 elbaf[] 배열(각 LBA Format의 PI Format), DPS 확장 정보 등 4KB 수신. */
	rc = nvme_ctrlr_cmd_identify(ctrlr, SPDK_NVME_IDENTIFY_NS_IOCS, 0, ns->id, ns->csi,
				     nsdata_nvm, sizeof(*nsdata_nvm),
				     nvme_completion_poll_cb, status);
	/* [한국어] 발행 실패 — 양쪽 정리. */
	if (rc != 0) {
		spdk_free(nsdata_nvm);
		free(status);
		return rc;
	}

	/* [한국어] 완료 polling. */
	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	/* [한국어] 완료는 받았으나 SC 에러 — NVM specific data 미지원이거나 일시 오류. */
	if (rc) {
		/* [한국어] ERR 로그 — strerror로 errno 디코딩. */
		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_ctrlr_cmd_identify failed: rc=%s\n",
				  spdk_strerror(abs(rc)));
		/* [한국어] NVM 버퍼 해제 (status leak — 동일 패턴). */
		spdk_free(nsdata_nvm);
		/* [한국어] -ENXIO: 디바이스 응답 비정상. */
		return -ENXIO;
	}

	/* [한국어] 성공 — NVM IOCS 데이터를 NS 객체에 연결. nvme_ns_set_identify_data 가 이후 elbaf 참조 가능. */
	ns->nsdata_nvm = nsdata_nvm;
	return 0;
}

/*
 * [한국어]
 * nvme_ctrlr_identify_ns_iocs_specific - CSI 별로 IOCS-specific Identify NS 분기 발행
 *
 * @ns: 대상 NS — ns->csi 가 ID Descriptor에서 도출되어 결정된 상태 (ZNS or NVM)
 * @return: 0 성공, 음수 errno 실패. -EINVAL 은 도달 불가 (assert로 강제 종료).
 *
 * 이 함수가 왜 필요한가:
 *   - 명령 셋(CSI)별로 IOCS-specific identify의 의미가 다르므로 단일 dispatch 진입점이 필요.
 *   - nvme_ns_has_supported_iocs_specific_data() 가 true 인 NS만 이 함수가 호출되므로,
 *     여기서 미지원 CSI는 assert로 잡힘 — 호출 가드와 분기 로직의 invariant 보호.
 * 동작:
 *   - ZNS: 무조건 ZNS specific identify 발행
 *   - NVM: ctratt.elbas=1 이어야만 NVM specific identify 발행 (그렇지 않으면 fallthrough → assert)
 *   - 기타 CSI: assert(0) — 도달하면 사전 가드(nvme_ns_has_supported_iocs_specific_data) 위반
 * 실행 컨텍스트: bring-up 시점 호스트 SPDK thread
 * Caller / Callee:
 *   - Caller: nvme_ns_construct (NS 객체 초기화의 3단계, ID Descriptor 단계 이후)
 *   - Callee: nvme_ctrlr_identify_ns_zns_specific, nvme_ctrlr_identify_ns_nvm_specific
 */
static int
nvme_ctrlr_identify_ns_iocs_specific(struct spdk_nvme_ns *ns)
{
	/* [한국어] CSI 분기 — Identify NS ID Descriptor List에서 도출된 명령 셋 식별자. */
	switch (ns->csi) {
	/* [한국어] Zoned Namespace (NVMe TP-4053). */
	case SPDK_NVME_CSI_ZNS:
		/* [한국어] ZNS variant로 위임. */
		return nvme_ctrlr_identify_ns_zns_specific(ns);
	/* [한국어] 표준 NVM 명령 셋 (read/write/flush 등). */
	case SPDK_NVME_CSI_NVM:
		/* [한국어] ELBAS — Extended LBA Format Support (NVMe 2.0 controller attribute).
		 *  · 1 이면 elbaf 배열이 유효 → NVM specific identify 의미 있음.
		 *  · 0 이면 NVM-specific data 없음 → fallthrough 로 assert(0) 도달 시도되나,
		 *    호출 가드(nvme_ns_has_supported_iocs_specific_data)가 elbas=0 인 NVM CSI를 미리
		 *    걸러내므로 실제로는 도달 안 함. */
		if (ns->ctrlr->cdata.ctratt.bits.elbas) {
			/* [한국어] NVM IOCS-specific identify 발행. */
			return nvme_ctrlr_identify_ns_nvm_specific(ns);
		}
	/* fallthrough */
	/* [한국어] 정의되지 않은 CSI 또는 NVM이면서 elbas=0 — invariant 위반.
	 *  · DEBUG 빌드에선 assert로 즉시 종료, RELEASE 빌드에선 -EINVAL 반환으로 fallback. */
	default:
		/*
		 * This switch must handle all cases for which
		 * nvme_ns_has_supported_iocs_specific_data() returns true,
		 * other cases should never happen.
		 */
		/* [한국어] 강제 abort — 사전 가드 함수와의 동기화가 깨졌음을 즉시 노출.
		 *  · NDEBUG 빌드에선 nop이 되어 아래 -EINVAL 반환 경로로 진행. */
		assert(0);
	}

	/* [한국어] DEBUG 빌드 외 fallback — 잘못된 CSI 였음을 errno로 보고. */
	return -EINVAL;
}

/*
 * [한국어]
 * nvme_ctrlr_identify_id_desc - Identify NS ID Descriptor List (CNS=0x03) admin 발행
 *
 * @ns: 대상 NS — ns->id_desc_list 4KB 버퍼에 응답을 받는다
 * @return: 0 성공 (또는 NVMe < 1.3 으로 skip), 음수 errno 실패
 *
 * 이 함수가 왜 필요한가:
 *   - NSID는 컨트롤러 reset/포맷 후 재할당될 수 있어 "영구적 식별자"가 아님. NGUID/EUI64/UUID 같은
 *     globally unique 식별자가 NS의 영속 ID 역할을 함. 또한 CSI(Command Set Identifier) 디스크립터로
 *     해당 NS가 NVM/ZNS/KV 중 어느 명령 셋에 속하는지 결정.
 *   - 이 데이터 없이는 multi-path / 영구 매핑 / IOCS-specific identify 분기 모두 불가.
 * 동작:
 *   1) id_desc_list 버퍼 0 초기화 (재호출 시 잔존 데이터 제거)
 *   2) 호환성 가드:
 *      · NVMe 버전 < 1.3 이고 IOCS capability 없음 → ID Descriptor 자체가 spec 미지원이므로 skip
 *      · NVME_QUIRK_IDENTIFY_CNS quirk: 일부 드라이브가 알 수 없는 CNS에 hang을 일으킴 → skip
 *   3) Identify NS ID Descriptor List (CNS=0x03) admin 발행 — 응답은 4KB 가변 길이 디스크립터 리스트
 *   4) 동기 대기 → 실패해도 fatal 아님 (WARN만, NS는 NSID로만 식별 가능한 모드로 계속)
 *   5) nvme_ns_set_id_desc_list_data 호출 → CSI 추출 (NVM/ZNS 분기에 사용)
 * 실행 컨텍스트: bring-up 시점 호스트 SPDK thread
 * Caller / Callee:
 *   - Caller: nvme_ns_construct (NS 객체 초기화의 2단계 — Identify NS 성공 후)
 *   - Callee: nvme_ctrlr_cmd_identify, nvme_wait_for_adminq_completion, nvme_ns_set_id_desc_list_data
 */
static int
nvme_ctrlr_identify_id_desc(struct spdk_nvme_ns *ns)
{
	/* [한국어] admin 완료 추적자. */
	struct nvme_completion_poll_status      *status;
	/* [한국어] return code. */
	int                                     rc;

	/* [한국어] id_desc_list 4KB 버퍼 0 초기화 — 이전 NS 또는 재구성 잔존 데이터 제거.
	 *  · 빈 list (nidl=0)는 nvme_ns_find_id_desc 가 즉시 NULL 반환하여 안전. */
	memset(ns->id_desc_list, 0, sizeof(ns->id_desc_list));

	/* [한국어] 호환성 가드 — 두 조건 중 하나라도 해당하면 ID Descriptor 명령 자체를 보내지 않음:
	 *  (a) NVMe 버전 < 1.3.0 *그리고* CAP.CSS에 IOCS 비트 없음 — spec상 CNS=0x03 미정의 시기
	 *  (b) NVME_QUIRK_IDENTIFY_CNS — 알 수 없는 CNS에 controller hang 일으키는 드라이브 회피.
	 *  이런 경우 ID Descriptor list는 빈 상태로 두고 0 (성공) 반환. */
	if ((ns->ctrlr->vs.raw < SPDK_NVME_VERSION(1, 3, 0) &&
	     !(ns->ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_IOCS)) ||
	    (ns->ctrlr->quirks & NVME_QUIRK_IDENTIFY_CNS)) {
		/* [한국어] DEBUG 로그 — skip 이유 기록 (운영 시엔 출력 안 됨). */
		NVME_CTRLR_DEBUGLOG(ns->ctrlr, "Version < 1.3; not attempting to retrieve NS ID Descriptor List\n");
		/* [한국어] 0 (성공) 반환 — bring-up 계속 진행. */
		return 0;
	}

	/* [한국어] status tracker 할당. */
	status = calloc(1, sizeof(*status));
	if (!status) {
		/* [한국어] ERR 로그 with ctrlr context. */
		NVME_CTRLR_ERRLOG(ns->ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	/* [한국어] DEBUG 로그 — ID Descriptor 시도 시작. */
	NVME_CTRLR_DEBUGLOG(ns->ctrlr, "Attempting to retrieve NS ID Descriptor List\n");
	/* [한국어] Identify NS ID Descriptor List (CNS=0x03) 발행:
	 *   · cntid=0 (no filter), nsid=ns->id, csi=0
	 *   · 응답 = 가변 길이 디스크립터들이 packed 된 4KB 영역 (NIDT/NIDL/NID 형식). */
	rc = nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST, 0, ns->id,
				     0, ns->id_desc_list, sizeof(ns->id_desc_list),
				     nvme_completion_poll_cb, status);
	/* [한국어] 발행 자체가 실패 — qpair 미준비 등. status는 사용 안 됨. */
	if (rc < 0) {
		free(status);
		return rc;
	}

	/* [한국어] 동기 polling. */
	rc = nvme_wait_for_adminq_completion(ns->ctrlr, status, true);
	/* [한국어] SC 에러 — 일부 드라이브가 ID Descriptor 미지원이거나 일시 오류.
	 *  · ID Descriptor 부재는 fatal 아님. NS는 NSID 단독 식별으로 계속 동작 가능.
	 *  · WARN 로그 + 버퍼 0 reset 후 진행. */
	if (rc) {
		/* [한국어] WARN 로그 — ID Descriptor 미취득 알림 (NSID 식별으로 fallback 가능 시그널). */
		NVME_CTRLR_WARNLOG(ns->ctrlr, "Failed to retrieve NS ID Descriptor List\n");
		/* [한국어] 부분적으로 채워졌을 수 있는 응답을 0으로 비워서 파서 안전성 확보. */
		memset(ns->id_desc_list, 0, sizeof(ns->id_desc_list));
	}

	/* [한국어] CSI 추출 + 캐시 — id_desc_list에서 CSI descriptor 찾아 ns->csi에 저장.
	 *  · 디스크립터가 비어있으면 기본 SPDK_NVME_CSI_NVM 으로 fallback. */
	nvme_ns_set_id_desc_list_data(ns);
	/* [한국어] rc 그대로 전파 — 실패한 경우라도 caller(nvme_ns_construct)는 별도 정책으로 처리. */
	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_id - NS의 NSID 반환 (1-based)
 *
 * @ns: 대상 NS
 * @return: NSID — SQE의 cmd.nsid 필드에 그대로 사용 가능. 0이면 미할당 NS.
 *
 * 외부 공개 getter — bdev_nvme 등 사용자가 IO 발행 시 cmd.nsid 채우는 데 사용.
 * 잠금 없음 (read-only, NS 객체 lifetime 내 불변).
 */
uint32_t
spdk_nvme_ns_get_id(struct spdk_nvme_ns *ns)
{
	/* [한국어] NSID 단순 반환 — 1-based. RB tree key로도 사용됨. */
	return ns->id;
}

/*
 * [한국어]
 * spdk_nvme_ns_is_active - NS가 활성 상태인지 (즉 IO 가능한지) 판정
 *
 * @ns: 대상 NS
 * @return: true 활성, false 비활성 (NSID 0 또는 NCAP 0)
 *
 * 활성 판정 기준 (NVMe spec):
 *   1) NSID != 0 — NSID 0은 broadcast 또는 미할당
 *   2) NCAP != 0 — Identify NS 응답이 zero-fill이면 비활성. NCAP은 active NS에서 반드시 non-zero.
 *
 * Caller: nvme_ns_set_identify_data (ns->active 캐시), 외부 사용자 (IO 전 가드).
 */
bool
spdk_nvme_ns_is_active(struct spdk_nvme_ns *ns)
{
	/* [한국어] nsdata 포인터 — NCAP 검사를 위해. NULL 초기화는 컴파일러 미사용 경고 회피. */
	const struct spdk_nvme_ns_data *nsdata = NULL;

	/*
	 * According to the spec, valid NS has non-zero id.
	 */
	/* [한국어] NSID 0 차단 — spec상 broadcast(0xFFFFFFFF) 또는 unallocated 의미. */
	if (ns->id == 0) {
		return false;
	}

	/* [한국어] nsdata 포인터 획득. */
	nsdata = _nvme_ns_get_data(ns);

	/*
	 * According to the spec, Identify Namespace will return a zero-filled structure for
	 *  inactive namespace IDs.
	 * Check NCAP since it must be nonzero for an active namespace.
	 */
	/* [한국어] NCAP(Namespace Capacity, 8B LBA 단위) — active NS에서 반드시 non-zero.
	 *  · 비활성 NS는 zero-fill 응답이라 ncap=0. NSZE 대신 NCAP을 보는 이유는 thin-provisioned
	 *    NS의 경우 NSZE는 0 가능성이 있으나 NCAP은 capacity의 진짜 지표. */
	return nsdata->ncap != 0;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_ctrlr - NS의 소속 컨트롤러 역참조
 *
 * @ns: 대상 NS
 * @return: NS가 속한 spdk_nvme_ctrlr 포인터 (절대 NULL 아님 — NS 객체 자체가 ctrlr가 알당해 줌)
 *
 * IO 발행 시 ctrlr context (transport, MDTS, doorbell)을 같이 들고 가야 할 때 사용.
 */
struct spdk_nvme_ctrlr *
spdk_nvme_ns_get_ctrlr(struct spdk_nvme_ns *ns)
{
	/* [한국어] 단순 역참조 — ns 생성 시 nvme_ns_construct가 설정함. */
	return ns->ctrlr;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_max_io_xfer_size - 단일 IO 최대 전송 byte 크기
 *
 * @return: 컨트롤러의 max_xfer_size — MDTS 기반 계산값 (보통 64KB~1MB).
 *
 * 사용자가 이 값보다 큰 buffer를 1 IO로 보내면 SPDK 또는 SSD가 split. 미리 알면 split 회피 가능.
 */
uint32_t
spdk_nvme_ns_get_max_io_xfer_size(struct spdk_nvme_ns *ns)
{
	/* [한국어] ctrlr 단위 캐시값 — bring-up에서 cdata.mdts 와 min_page_size로 산출. */
	return ns->ctrlr->max_xfer_size;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_sector_size - LBA 데이터 블록 크기 (metadata 제외)
 *
 * @return: 일반적으로 512 또는 4096 byte.
 *
 * extended LBA 모드에서도 이 함수는 metadata를 제외한 *데이터* 크기만 반환.
 * extended_lba_size를 원하면 spdk_nvme_ns_get_extended_sector_size 사용.
 */
uint32_t
spdk_nvme_ns_get_sector_size(struct spdk_nvme_ns *ns)
{
	/* [한국어] cooked 캐시 필드 — nvme_ns_set_identify_data 가 설정. */
	return ns->sector_size;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_extended_sector_size - LBA 블록 크기 (extended LBA 시 metadata 포함)
 *
 * @return: 비-extended 모드: sector_size와 동일.
 *          extended 모드: sector_size + md_size (예: 512+8=520, 4096+64=4160)
 *
 * extended LBA NS의 buffer 크기 계산 시 반드시 이 함수의 값을 사용해야 함.
 */
uint32_t
spdk_nvme_ns_get_extended_sector_size(struct spdk_nvme_ns *ns)
{
	/* [한국어] FLBAS.extended=1 분기에서 md_size가 더해진 값. */
	return ns->extended_lba_size;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_num_sectors - NS의 총 LBA 수 (NSZE)
 *
 * @return: NSZE — Namespace Size in LBAs. 총 capacity는 num_sectors * sector_size.
 */
uint64_t
spdk_nvme_ns_get_num_sectors(struct spdk_nvme_ns *ns)
{
	/* [한국어] nsdata->nsze raw 값 — 8B LBA count. */
	return _nvme_ns_get_data(ns)->nsze;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_size - NS의 총 byte 용량
 *
 * @return: NSZE * sector_size (extended가 아닌 데이터 byte 수).
 *
 * 주의: extended LBA에서 metadata도 포함한 raw 매체 크기를 원하면 별도 계산 필요.
 */
uint64_t
spdk_nvme_ns_get_size(struct spdk_nvme_ns *ns)
{
	/* [한국어] num_sectors × sector_size — 사용자에게 노출되는 NS의 데이터 용량. */
	return spdk_nvme_ns_get_num_sectors(ns) * spdk_nvme_ns_get_sector_size(ns);
}

/*
 * [한국어]
 * spdk_nvme_ns_get_flags - NS feature flag bitmap 반환
 *
 * @return: SPDK_NVME_NS_*_SUPPORTED 비트들의 OR (FLUSH/COMPARE/DSM/WRITE_ZEROES/PI 등).
 *
 * 사용자는 이 flag를 보고 어떤 명령을 보낼지 결정.
 */
uint32_t
spdk_nvme_ns_get_flags(struct spdk_nvme_ns *ns)
{
	/* [한국어] cooked flag bitmap — nvme_ns_set_identify_data 가 ONCS/VWC 등에서 도출. */
	return ns->flags;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_pi_type - NS의 PI Type 반환
 *
 * @return: SPDK_NVME_FMT_NVM_PROTECTION_DISABLE | TYPE1 | TYPE2 | TYPE3
 *
 * Type 1/2/3은 RefTag 동작이 다름 — Type 1은 LBA 기반 자동 증가, Type 2/3은 호스트 명시 등.
 */
enum spdk_nvme_pi_type
spdk_nvme_ns_get_pi_type(struct spdk_nvme_ns *ns) {
	/* [한국어] cooked pi_type — DPS.pit 비트필드에서 추출됨. */
	return ns->pi_type;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_pi_format - NS의 PI Format 반환 (NVMe 2.0 ELBAS)
 *
 * @return: 16/32/64-bit Guard 중 하나. ELBAS 미지원 컨트롤러에선 16B Guard 기본값.
 */
enum spdk_nvme_pi_format
spdk_nvme_ns_get_pi_format(struct spdk_nvme_ns *ns) {
	/* [한국어] elbaf[].pif에서 도출된 pi_format. */
	return ns->pi_format;
}

/*
 * [한국어]
 * spdk_nvme_ns_supports_extended_lba - extended LBA 모드인지 (metadata가 inline인지)
 *
 * @return: true = inline metadata (FLBAS.extended=1). false = separate metadata pointer 모드.
 */
bool
spdk_nvme_ns_supports_extended_lba(struct spdk_nvme_ns *ns)
{
	/* [한국어] EXTENDED_LBA flag 비트 검사 — inline 여부 판정. */
	return (ns->flags & SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED) ? true : false;
}

/*
 * [한국어]
 * spdk_nvme_ns_supports_write_uncorrectable - Write Uncorrectable 명령 지원 여부
 */
bool
spdk_nvme_ns_supports_write_uncorrectable(struct spdk_nvme_ns *ns)
{
	/* [한국어] WRITE_UNCORRECTABLE flag 비트 검사 — ONCS.nvmwusv 에서 유래. */
	return (ns->flags & SPDK_NVME_NS_WRITE_UNCORRECTABLE_SUPPORTED) ? true : false;
}

/*
 * [한국어]
 * spdk_nvme_ns_supports_compare - Compare 명령 지원 여부
 */
bool
spdk_nvme_ns_supports_compare(struct spdk_nvme_ns *ns)
{
	/* [한국어] COMPARE flag 비트 검사 — ONCS.nvmcmps 에서 유래. */
	return (ns->flags & SPDK_NVME_NS_COMPARE_SUPPORTED) ? true : false;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_md_size - per-LBA metadata 크기 (byte)
 *
 * @return: PI 비활성이면 0. PI Type 1/2/3 활성이면 일반적으로 8 (Guard 2 + AppTag 2 + RefTag 4).
 */
uint32_t
spdk_nvme_ns_get_md_size(struct spdk_nvme_ns *ns)
{
	/* [한국어] cooked md_size — LBAF.ms 에서 도출. */
	return ns->md_size;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_format_index - 사용 중인 LBA Format의 인덱스 도출
 *
 * @nsdata: Identify NS raw 데이터
 * @return: 0~63 사이의 LBAF 인덱스 (nsdata->lbaf[index] 로 sector size·md size 조회 가능)
 *
 * 인코딩:
 *   - nlbaf < 16 (legacy): FLBAS.format 4비트만 사용
 *   - nlbaf >= 16 (NVMe 2.0): msb_format(상위 4비트) << 4 | format(하위 4비트) — 6비트 인덱스
 *
 * Caller: nvme_ns_set_identify_data, public 사용자.
 */
uint32_t
spdk_nvme_ns_get_format_index(const struct spdk_nvme_ns_data *nsdata)
{
	/* [한국어] legacy 인코딩 분기 — 16개 미만의 LBAF만 가진 컨트롤러. */
	if (nsdata->nlbaf < 16) {
		/* [한국어] format 4비트만 의미 있음. */
		return nsdata->flbas.format;
	} else {
		/* [한국어] NVMe 2.0 확장 인코딩 — msb_format를 상위 비트로 결합하여 0~63 범위 표현. */
		return ((nsdata->flbas.msb_format << 4) + nsdata->flbas.format);
	}
}

/*
 * [한국어]
 * spdk_nvme_ns_get_data - raw Identify NS 응답 4KB의 const 포인터 반환
 *
 * 외부 공개 — 사용자가 raw nsdata에 접근하고 싶을 때 사용 (read-only).
 */
const struct spdk_nvme_ns_data *
spdk_nvme_ns_get_data(struct spdk_nvme_ns *ns)
{
	/* [한국어] 내부 _nvme_ns_get_data 위임 — const-correct 외부 진입점. */
	return _nvme_ns_get_data(ns);
}

/*
 * [한국어]
 * spdk_nvme_nvm_ns_get_data - NVM IOCS-specific Identify NS 데이터 반환
 *
 * @return: NVMe 2.0 ELBAS 컨트롤러에서만 non-NULL. 그렇지 않으면 NULL.
 */
const struct spdk_nvme_nvm_ns_data *
spdk_nvme_nvm_ns_get_data(struct spdk_nvme_ns *ns)
{
	/* [한국어] elbas=1 인 경우만 nvme_ctrlr_identify_ns_nvm_specific 가 채워둠. 아니면 NULL. */
	return ns->nsdata_nvm;
}

/* We have to use the typedef in the function declaration to appease astyle. */
/* [한국어] astyle 자동 포매터 회피용 typedef — enum 이름이 너무 길어서 함수 선언 줄바꿈 시
 *  astyle이 깨뜨리는 문제를 피하려는 예방책. 기능적 의미는 없고 가독성/포맷팅용. */
typedef enum spdk_nvme_dealloc_logical_block_read_value
spdk_nvme_dealloc_logical_block_read_value_t;

/*
 * [한국어]
 * spdk_nvme_ns_get_dealloc_logical_block_read_value - Deallocate된 LBA를 read 시 어떤 값이 반환되는지
 *
 * @ns: 대상 NS
 * @return: SPDK_NVME_DEALLOC_NOT_REPORTED | DEALLOC_READ_00 | DEALLOC_READ_FF
 *
 * 이 정보가 왜 필요한가:
 *   - TRIM/Deallocate 후 해당 LBA를 read하면 SSD 별로 동작이 다름:
 *     · 어떤 SSD: 0x00 으로 채워진 데이터 반환 (SPDK_NVME_DEALLOC_READ_00)
 *     · 어떤 SSD: 0xFF 로 채워진 데이터 반환 (SPDK_NVME_DEALLOC_READ_FF)
 *     · 어떤 SSD: 정의되지 않음 (이전 데이터 또는 garbage 가능)
 *   - 사용자(예: filesystem)는 이 동작에 따라 데이터 무결성 가정을 달리해야 함.
 *
 * 동작:
 *   - NVME_QUIRK_READ_ZERO_AFTER_DEALLOCATE quirk: 일부 드라이브가 DLFEAT 보고를 잘못해도
 *     실제로는 0을 반환함을 알고 있음 → 강제로 READ_00 반환
 *   - 그 외: nsdata->dlfeat.bits.read_value 그대로 반환
 */
spdk_nvme_dealloc_logical_block_read_value_t
spdk_nvme_ns_get_dealloc_logical_block_read_value(
	struct spdk_nvme_ns *ns)
{
	/* [한국어] ctrlr 캐시 — quirks 비트 검사용. */
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	/* [한국어] raw nsdata — DLFEAT 비트필드 읽기 위함. */
	const struct spdk_nvme_ns_data *data = spdk_nvme_ns_get_data(ns);

	/* [한국어] quirk 우선 — DLFEAT 보고를 못 믿는 드라이브는 강제로 READ_00 보고. */
	if (ctrlr->quirks & NVME_QUIRK_READ_ZERO_AFTER_DEALLOCATE) {
		/* [한국어] 0x00 read 보장 — filesystem TRIM 의미론과 정합. */
		return SPDK_NVME_DEALLOC_READ_00;
	} else {
		/* [한국어] DLFEAT.read_value 그대로 — 컨트롤러 보고 신뢰. */
		return data->dlfeat.bits.read_value;
	}
}

/*
 * [한국어]
 * spdk_nvme_ns_get_optimal_io_boundary - 권장 stripe 경계 (LBA 단위)
 *
 * @return: NOIOB 값 또는 Intel quirk로 도출된 stripe 크기. 없으면 0.
 *
 * 사용자가 이 경계를 가로지르는 IO를 피하면 일부 SSD에서 성능 향상.
 */
uint32_t
spdk_nvme_ns_get_optimal_io_boundary(struct spdk_nvme_ns *ns)
{
	/* [한국어] cooked sectors_per_stripe — NOIOB 또는 Intel quirk 분기에서 설정됨. */
	return ns->sectors_per_stripe;
}

/*
 * [한국어]
 * nvme_ns_find_id_desc - id_desc_list 4KB 버퍼에서 특정 type의 디스크립터 검색
 *
 * @ns: 대상 NS — ns->id_desc_list 가 채워져 있어야 의미 있음
 * @type: SPDK_NVME_NIDT_NGUID(0x01) | EUI64(0x02) | UUID(0x03) | CSI(0x04)
 * @length: out — 발견된 디스크립터의 NID 길이 (NIDL 값) 저장
 * @return: NID 데이터의 const 포인터 (디스크립터 내부), 또는 NULL (end of list / 못 찾음)
 *
 * 디스크립터 포맷 (NVMe spec):
 *   ┌─────┬─────┬─────┬─────┬───────────────┐
 *   │ NIDT│ NIDL│ Rsv2│ Rsv3│ NID (NIDL byte)│
 *   └─────┴─────┴─────┴─────┴───────────────┘
 *      1B    1B    1B    1B    가변 길이
 *   - NIDT(Namespace Identifier Type): 0x01=EUI64, 0x02=NGUID, 0x03=UUID, 0x04=CSI
 *   - NIDL(Namespace Identifier Length): NID 영역 길이 (header 4B 제외)
 *   - NIDL=0 → end of list 시그널
 *
 * 동작:
 *   - offset 0 부터 시작하여 디스크립터 헤더(4B) + NID 영역을 따라 순회
 *   - 매 iteration에서 NIDL=0 (end), 또는 type 일치 시 즉시 반환
 *   - 경계 검사: offset + NIDL + 4 > 4KB 면 invalid descriptor → NULL 반환
 *
 * Caller: spdk_nvme_ns_get_nguid, spdk_nvme_ns_get_uuid, nvme_ns_get_csi
 */
static const void *
nvme_ns_find_id_desc(const struct spdk_nvme_ns *ns, enum spdk_nvme_nidt type, size_t *length)
{
	/* [한국어] 현재 검사 중인 디스크립터 포인터 — id_desc_list[offset] 위치를 spec 구조체로 캐스팅. */
	const struct spdk_nvme_ns_id_desc *desc;
	/* [한국어] 4KB 버퍼 내 현재 offset (byte). */
	size_t offset;

	/* [한국어] 처음부터 순회. */
	offset = 0;
	/* [한국어] 헤더 4B만큼이라도 안전히 읽을 공간이 남아 있을 때까지 순회. */
	while (offset + 4 < sizeof(ns->id_desc_list)) {
		/* [한국어] 현 offset 위치를 디스크립터 구조체로 해석 — packed binary 파싱. */
		desc = (const struct spdk_nvme_ns_id_desc *)&ns->id_desc_list[offset];

		/* [한국어] NIDL=0 → end of list (sentinel). 더 이상 디스크립터 없음. */
		if (desc->nidl == 0) {
			/* End of list */
			return NULL;
		}

		/*
		 * Check if this descriptor fits within the list.
		 * 4 is the fixed-size descriptor header (not counted in NIDL).
		 */
		/* [한국어] 경계 검사 — NIDL + 헤더 4B가 4KB 버퍼를 벗어나면 invalid (firmware bug 가능성).
		 *  · 안전을 위해 NULL 반환하고 종료 — out-of-bound read 방지. */
		if (offset + desc->nidl + 4 > sizeof(ns->id_desc_list)) {
			/* Descriptor longer than remaining space in list (invalid) */
			return NULL;
		}

		/* [한국어] type 일치 — out 길이 저장 후 NID payload 첫 byte 주소 반환. */
		if (desc->nidt == type) {
			/* [한국어] NIDL을 호출자에게 반환 — caller가 이 길이로 NID memcpy/검증. */
			*length = desc->nidl;
			/* [한국어] NID 영역 시작 — 디스크립터 헤더 다음의 가변 길이 데이터. */
			return &desc->nid[0];
		}

		/* [한국어] 다음 디스크립터로 진행 — 현재 디스크립터의 헤더 4B + NID NIDL B 만큼 offset 증가. */
		offset += 4 + desc->nidl;
	}

	/* [한국어] 4KB 끝까지 못 찾음. */
	return NULL;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_nguid - NS의 NGUID(Namespace Globally Unique Identifier) 반환
 *
 * @ns: 대상 NS
 * @return: 16-byte NGUID 포인터 (id_desc_list 내부), 못 찾거나 길이 잘못이면 NULL
 *
 * NGUID는 globally unique 16-byte 식별자 — multi-path 환경에서 같은 NS인지 식별, 영속 매핑에 사용.
 */
const uint8_t *
spdk_nvme_ns_get_nguid(const struct spdk_nvme_ns *ns)
{
	/* [한국어] 검색 결과 NGUID 포인터. */
	const uint8_t *nguid;
	/* [한국어] 디스크립터에서 보고된 길이. */
	size_t size;

	/* [한국어] NGUID type(0x02) 검색. */
	nguid = nvme_ns_find_id_desc(ns, SPDK_NVME_NIDT_NGUID, &size);
	/* [한국어] 길이 무결성 검사 — NGUID는 spec상 정확히 16B(spdk_nvme_ns_data.nguid 크기)여야 함.
	 *  · 다르면 firmware bug — WARN 로그 후 NULL 반환. */
	if (nguid && size != SPDK_SIZEOF_MEMBER(struct spdk_nvme_ns_data, nguid)) {
		NVME_CTRLR_WARNLOG(ns->ctrlr,
				   "Invalid NIDT_NGUID descriptor length reported: %zu (expected: %zu)\n",
				   size, SPDK_SIZEOF_MEMBER(struct spdk_nvme_ns_data, nguid));
		return NULL;
	}

	/* [한국어] 정상 NGUID 또는 NULL (못 찾음) 그대로 반환. */
	return nguid;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_uuid - NS의 UUID(RFC 4122) 반환
 *
 * @ns: 대상 NS
 * @return: spdk_uuid 포인터 (16B), 못 찾거나 길이 오류면 NULL
 *
 * UUID는 또 다른 globally unique 식별자 — NGUID와 병행 가능. 일부 드라이브는 UUID만, 일부는 NGUID만 보고.
 */
const struct spdk_uuid *
spdk_nvme_ns_get_uuid(const struct spdk_nvme_ns *ns)
{
	/* [한국어] 검색 결과 UUID 포인터. */
	const struct spdk_uuid *uuid;
	/* [한국어] 디스크립터 보고 길이. */
	size_t uuid_size;

	/* [한국어] UUID type(0x03) 검색. */
	uuid = nvme_ns_find_id_desc(ns, SPDK_NVME_NIDT_UUID, &uuid_size);
	/* [한국어] 길이 검증 — sizeof(spdk_uuid) = 16B 와 일치해야 함. */
	if (uuid && uuid_size != sizeof(*uuid)) {
		NVME_CTRLR_WARNLOG(ns->ctrlr, "Invalid NIDT_UUID descriptor length reported: %zu (expected: %zu)\n",
				   uuid_size, sizeof(*uuid));
		return NULL;
	}

	/* [한국어] 정상 UUID 또는 NULL 반환. */
	return uuid;
}

/*
 * [한국어]
 * nvme_ns_get_csi - id_desc_list에서 CSI(Command Set Identifier) 추출
 *
 * @ns: 대상 NS
 * @return: SPDK_NVME_CSI_NVM(0) | ZNS(2) | KV(1) — 발견 못하면 NVM 기본값
 *
 * CSI는 1-byte 식별자로 NS가 어떤 명령 셋에 속하는지 결정 — IOCS-specific identify 분기에 핵심.
 *
 * 동작:
 *   - id_desc_list에서 NIDT_CSI(0x04) 디스크립터 검색
 *   - 길이가 1B 가 아니면 invalid → NVM fallback + WARN
 *   - 디스크립터 자체가 없으면:
 *     · 컨트롤러가 IOCS 지원이라면 (CAP.CSS.iocs=1) 보고가 누락된 것이므로 WARN
 *     · IOCS 미지원이면 NVM이 기본 — silent fallback
 */
static enum spdk_nvme_csi
nvme_ns_get_csi(const struct spdk_nvme_ns *ns) {
	/* [한국어] CSI 포인터 (1-byte). */
	const uint8_t *csi;
	/* [한국어] 디스크립터 보고 길이. */
	size_t csi_size;

	/* [한국어] CSI type(0x04) 검색. */
	csi = nvme_ns_find_id_desc(ns, SPDK_NVME_NIDT_CSI, &csi_size);
	/* [한국어] 길이 무결성 — CSI는 정확히 1B 여야 함. */
	if (csi && csi_size != sizeof(*csi))
	{
		/* [한국어] WARN 로그 + 안전한 NVM fallback. */
		NVME_CTRLR_WARNLOG(ns->ctrlr, "Invalid NIDT_CSI descriptor length reported: %zu (expected: %zu)\n",
				   csi_size, sizeof(*csi));
		return SPDK_NVME_CSI_NVM;
	}
	/* [한국어] CSI 디스크립터 자체가 부재. */
	if (!csi)
	{
		/* [한국어] CAP.CSS.iocs=1 컨트롤러는 CSI 보고가 의무 — 누락이면 firmware bug. WARN 출력. */
		if (ns->ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_IOCS) {
			NVME_CTRLR_WARNLOG(ns->ctrlr, "CSI not reported for NSID: %" PRIu32 "\n", ns->id);
		}
		/* [한국어] 어느 경우든 NVM 기본값으로 안전하게 진행. */
		return SPDK_NVME_CSI_NVM;
	}

	/* [한국어] 정상 — CSI 1-byte 값 dereference하여 enum으로 반환. */
	return *csi;
}

/*
 * [한국어]
 * nvme_ns_set_id_desc_list_data - id_desc_list에서 도출 가능한 cooked 필드 채우기
 *
 * 현재는 CSI 1개만 추출 — 향후 확장 시 NGUID/UUID 캐싱도 여기에 추가 가능.
 *
 * Caller: nvme_ctrlr_identify_id_desc (ID Descriptor 응답 수신 직후)
 */
void
nvme_ns_set_id_desc_list_data(struct spdk_nvme_ns *ns)
{
	/* [한국어] CSI 추출하여 ns->csi 캐시 — 이후 nvme_ctrlr_identify_ns_iocs_specific 분기에 사용. */
	ns->csi = nvme_ns_get_csi(ns);
}

/*
 * [한국어]
 * spdk_nvme_ns_get_csi - NS의 CSI 반환 (외부 공개 getter)
 *
 * @return: NVM/ZNS/KV 등.
 */
enum spdk_nvme_csi
spdk_nvme_ns_get_csi(const struct spdk_nvme_ns *ns) {
	/* [한국어] cooked csi — nvme_ns_set_id_desc_list_data 가 채움. */
	return ns->csi;
}

/*
 * [한국어]
 * nvme_ns_free_zns_specific_data - ZNS IOCS-specific 4KB 버퍼 해제
 *
 * @ns: 대상 NS
 *
 * 동작:
 *   - NSID가 0이면 미할당 NS — 아무것도 안 함 (defensive)
 *   - nsdata_zns 포인터가 non-NULL이면 spdk_free로 hugepage 풀에 반환 + 포인터 NULL clear
 *
 * Caller: nvme_ns_free_iocs_specific_data, nvme_ctrlr_identify_ns_zns_specific (재호출 시 정리),
 *         nvme_ctrlr_identify_ns_nvm_specific (CSI 변경 안전성 — 약간의 의도치 않은 호출).
 */
void
nvme_ns_free_zns_specific_data(struct spdk_nvme_ns *ns)
{
	/* [한국어] NSID 0 = 미할당 — 사실상 no-op으로 안전하게 빠져나감. */
	if (!ns->id) {
		return;
	}

	/* [한국어] ZNS 데이터가 할당된 경우만 해제. */
	if (ns->nsdata_zns) {
		/* [한국어] hugepage 풀로 4KB 반환. */
		spdk_free(ns->nsdata_zns);
		/* [한국어] dangling pointer 방지 — NULL clear. 다음 nvme_ns_set_identify_data가 NULL 체크 의존. */
		ns->nsdata_zns = NULL;
	}
}

/*
 * [한국어]
 * nvme_ns_free_nvm_specific_data - NVM IOCS-specific 4KB 버퍼 해제
 *
 * 위 ZNS variant와 동일 패턴 — 대상만 nsdata_nvm.
 */
void
nvme_ns_free_nvm_specific_data(struct spdk_nvme_ns *ns)
{
	/* [한국어] NSID 0 가드. */
	if (!ns->id) {
		return;
	}

	/* [한국어] NVM 데이터 할당된 경우만 해제. */
	if (ns->nsdata_nvm) {
		spdk_free(ns->nsdata_nvm);
		ns->nsdata_nvm = NULL;
	}
}

/*
 * [한국어]
 * nvme_ns_free_iocs_specific_data - 모든 IOCS-specific 데이터 일괄 해제
 *
 * NS destruct 시 또는 컨트롤러 reset 시 사용 — ZNS/NVM 둘 다 정리.
 */
void
nvme_ns_free_iocs_specific_data(struct spdk_nvme_ns *ns)
{
	/* [한국어] ZNS 해제. */
	nvme_ns_free_zns_specific_data(ns);
	/* [한국어] NVM 해제. */
	nvme_ns_free_nvm_specific_data(ns);
}

/*
 * [한국어]
 * nvme_ns_has_supported_iocs_specific_data - IOCS-specific identify 발행 필요 여부 가드
 *
 * @ns: 대상 NS — ns->csi 가 결정되어 있어야 함
 * @return: true → nvme_ctrlr_identify_ns_iocs_specific 호출 의미 있음
 *
 * 분기:
 *   - NVM CSI: ctratt.elbas=1 (NVMe 2.0 ELBAS) 인 경우만 IOCS-specific data 의미 있음
 *   - ZNS CSI: 항상 IOCS-specific data 필요 (zone size 등)
 *   - 그 외: 미지원 CSI — WARN 후 false
 *
 * 이 함수가 nvme_ns_construct에서 가드로 사용되어 불필요한 admin 명령 발행 회피.
 */
bool
nvme_ns_has_supported_iocs_specific_data(struct spdk_nvme_ns *ns)
{
	/* [한국어] CSI 분기. */
	switch (ns->csi) {
	/* [한국어] 표준 NVM 명령 셋. */
	case SPDK_NVME_CSI_NVM:
		/* [한국어] ELBAS=1 → 확장 LBA Format 정보 있음 → identify_ns_nvm_specific 의미 있음. */
		if (ns->ctrlr->cdata.ctratt.bits.elbas) {
			return true;
		}

		/* [한국어] ELBAS=0 → NVM-specific 데이터 의미 없음. */
		return false;
	/* [한국어] ZNS — 항상 zone size, max active zones 등이 필요하므로 무조건 true. */
	case SPDK_NVME_CSI_ZNS:
		return true;
	/* [한국어] 기타 CSI (KV 등 SPDK 미지원) — WARN 출력 후 false. */
	default:
		NVME_CTRLR_WARNLOG(ns->ctrlr, "Unsupported CSI: %u for NSID: %u\n", ns->csi, ns->id);
		return false;
	}
}

/*
 * [한국어]
 * spdk_nvme_ns_get_ana_group_id - NS의 ANA 그룹 ID 반환 (NVMe-oF multipath)
 *
 * @return: ANA Group ID (0이면 ANA 미지원 컨트롤러)
 *
 * ANA(Asymmetric Namespace Access)는 NVMe-oF 환경에서 path별 NS 가용성을 그룹 단위로 관리.
 */
uint32_t
spdk_nvme_ns_get_ana_group_id(const struct spdk_nvme_ns *ns)
{
	/* [한국어] cooked ana_group_id — ANA log page 처리 시 nvme_ctrlr가 채워둠. */
	return ns->ana_group_id;
}

/*
 * [한국어]
 * spdk_nvme_ns_get_ana_state - NS의 현재 ANA 상태 반환
 *
 * @return: OPTIMIZED | NON_OPTIMIZED | INACCESSIBLE | PERSISTENT_LOSS | CHANGE
 *
 * Multi-path 사용자가 어떤 path로 IO를 보낼지 결정하는 데 사용 (OPTIMIZED 우선).
 */
enum spdk_nvme_ana_state
spdk_nvme_ns_get_ana_state(const struct spdk_nvme_ns *ns) {
	/* [한국어] cooked ana_state — ANA log 응답으로 갱신됨. */
	return ns->ana_state;
}

/*
 * [한국어]
 * nvme_ns_construct - NS 객체의 라이프사이클 진입점 (Identify 시퀀스 전체 수행)
 *
 * @ns: 대상 NS 객체 — caller(spdk_nvme_ctrlr_get_ns)가 zmalloc/RB_INSERT 까지 처리하고 넘김
 * @id: NSID (1-based) — assert로 0이 아님이 보장됨
 * @ctrlr: 소속 컨트롤러
 * @return: 0 성공 (NS가 비활성으로 판명되어도 0), 음수 errno 실패
 *
 * 이 함수가 왜 필요한가:
 *   - NS 객체를 단순 alloc하는 것만으론 사용할 수 없음. NVMe controller에 admin 명령들을 보내
 *     실제 NS 메타데이터를 가져와야 비로소 IO 발행 가능. 이 함수가 그 시퀀스의 단일 진입점.
 *   - Identify NS → ID Descriptor → IOCS-specific 의 3단계 admin 호출을 직렬로 묶음.
 *
 * 동작 순서:
 *   1) ctrlr 역참조 + NSID 저장 + ANA 상태 OPTIMIZED 기본값으로 초기화
 *      (ANA 실제 상태는 추후 ANA log page 읽을 때 덮어써짐)
 *   2) Identify NS (CNS=0x00) — 실패면 즉시 errno 전파
 *   3) 비활성 NS 검사 — 활성이 아니면 ID Descriptor 단계 skip하고 0 반환
 *      (비활성 NS는 메타데이터가 의미 없으므로 추가 admin 호출 낭비)
 *   4) Identify NS ID Descriptor List (CNS=0x03) — 실패면 errno 전파 (CSI 결정 불가하면 진행 불가)
 *   5) 컨트롤러가 multi-IOCS 지원 *그리고* 이 NS가 IOCS-specific data를 필요로 하면
 *      Identify NS IOCS-Specific (CNS=0x05) 호출
 *
 * 실행 컨텍스트: bring-up 시점 호스트 SPDK thread (caller가 ctrlr_lock 보유 가정)
 * Caller / Callee:
 *   - Caller: nvme_ctrlr_identify_active_ns (active NS list 순회 시 각 NSID마다 호출),
 *             AER NS Attribute Notice 핸들러 (NS 변경 시 재호출)
 *   - Callee: nvme_ctrlr_identify_ns, nvme_ctrlr_identify_id_desc,
 *             nvme_ctrlr_identify_ns_iocs_specific, spdk_nvme_ns_is_active,
 *             nvme_ctrlr_multi_iocs_enabled, nvme_ns_has_supported_iocs_specific_data
 *
 * 호출 체인:
 *   nvme_ctrlr_init → nvme_ctrlr_identify_active_ns
 *     → for each active NSID: spdk_nvme_ctrlr_get_ns (RB tree alloc)
 *       → [nvme_ns_construct]
 *         ├─ nvme_ctrlr_identify_ns         (CNS=0x00)
 *         ├─ nvme_ctrlr_identify_id_desc    (CNS=0x03, NVMe 1.3+)
 *         └─ nvme_ctrlr_identify_ns_iocs_specific (CNS=0x05, IOCS 지원시)
 */
int
nvme_ns_construct(struct spdk_nvme_ns *ns, uint32_t id,
		  struct spdk_nvme_ctrlr *ctrlr)
{
	/* [한국어] return code. */
	int	rc;

	/* [한국어] DEBUG 빌드 invariant — NSID는 1-based이므로 0은 절대 도달 불가.
	 *  · spdk_nvme_ctrlr_get_ns가 nsid<1 을 거른 후 호출되므로 여기 도달 시 그것의 위반. */
	assert(id > 0);

	/* [한국어] 역참조 설정 — IO 시 ctrlr.transport / max_xfer_size 등을 참조하기 위함. */
	ns->ctrlr = ctrlr;
	/* [한국어] NSID 저장 — RB tree key, SQE의 cmd.nsid 등에 사용. */
	ns->id = id;
	/* This will be overwritten when reading ANA log page. */
	/* [한국어] ANA 상태 안전한 기본값 — OPTIMIZED. ANA 미지원 컨트롤러에서도 이 값으로 IO 진행 가능.
	 *  · nvme_ctrlr가 ANA log를 읽으면 그때 실제 상태로 덮어쓰므로 임시값 의미. */
	ns->ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;

	/* [한국어] 1단계 — Identify NS 발행. 이 호출이 nsdata 4KB를 채우고 cooked 필드 설정. */
	rc = nvme_ctrlr_identify_ns(ns);
	/* [한국어] 발행/응답 자체가 실패 — bring-up 단계의 fatal로 전파. */
	if (rc != 0) {
		return rc;
	}

	/* skip Identify NS ID Descriptor List for inactive NS */
	/* [한국어] 활성 검사 — Identify NS가 SC 에러로 비활성 판정되었을 수 있음.
	 *  · 비활성이면 추가 admin 호출 낭비를 피하고 0(성공)으로 즉시 종료.
	 *  · NS 객체는 RB tree에 남지만 ns->active=false 이고 ns->csi는 NVM 기본값 유지. */
	if (!spdk_nvme_ns_is_active(ns)) {
		return 0;
	}

	/* [한국어] 2단계 — ID Descriptor List 발행. NVMe < 1.3 이면 내부에서 skip 처리됨. */
	rc = nvme_ctrlr_identify_id_desc(ns);
	/* [한국어] 발행 자체가 실패 — fatal. CSI 결정 불가 의미. */
	if (rc != 0) {
		return rc;
	}

	/* [한국어] 3단계 가드 — 두 조건 모두 만족 시에만 IOCS-specific identify 발행:
	 *  (a) 컨트롤러가 multi-IOCS 지원 (cdata.ctratt에서 결정)
	 *  (b) 이 NS가 IOCS-specific data 필요 (CSI별 분기) */
	if (nvme_ctrlr_multi_iocs_enabled(ctrlr) &&
	    nvme_ns_has_supported_iocs_specific_data(ns)) {
		/* [한국어] 3단계 — IOCS-specific identify (ZNS/NVM 분기). */
		rc = nvme_ctrlr_identify_ns_iocs_specific(ns);
		/* [한국어] 실패 시 fatal — IOCS-specific data 없이는 ZNS NS의 zone size 등 결정 불가. */
		if (rc != 0) {
			return rc;
		}
	}

	/* [한국어] 모든 단계 성공 — NS는 이제 IO 발행 가능 상태. */
	return 0;
}

/*
 * [한국어]
 * nvme_ns_destruct - NS 객체의 모든 캐시 데이터 reset (객체 메모리 자체는 caller가 해제)
 *
 * @ns: 대상 NS
 *
 * 이 함수가 왜 필요한가:
 *   - NS 비활성화 (Identify NS 실패), AER NS Attribute Notice로 NS 재구성 필요, 컨트롤러 reset 등의
 *     상황에서 이전 캐시 데이터로 인한 오염을 방지하기 위함.
 *   - NS 객체 메모리 자체는 RB tree에 남아 있고 caller(주로 spdk_nvme_ctrlr_destruct)가 free함.
 *
 * 동작:
 *   - NSID 0이면 미할당 — no-op
 *   - nsdata 4KB 0 채우기 (NCAP=0 이 되어 spdk_nvme_ns_is_active 가 false 반환)
 *   - id_desc_list 4KB 0 채우기
 *   - IOCS-specific (ZNS/NVM) 데이터 해제
 *   - 모든 cooked 필드 reset (sector_size 등 0, csi는 NVM 기본값으로)
 *
 * 주의: ns->ctrlr 와 ns->id는 그대로 유지 — RB tree key와 역참조는 NS 객체가 살아있는 동안 유효해야 함.
 *
 * Caller: nvme_ctrlr_identify_ns (실패 시), spdk_nvme_ctrlr_destruct, AER 핸들러.
 */
void
nvme_ns_destruct(struct spdk_nvme_ns *ns)
{
	/* [한국어] nsdata 포인터 — memset 대상. */
	struct spdk_nvme_ns_data *nsdata;

	/* [한국어] NSID 0 가드 — 미할당 NS는 처리할 게 없음. */
	if (!ns->id) {
		return;
	}

	/* [한국어] nsdata 영역 주소 획득. */
	nsdata = _nvme_ns_get_data(ns);
	/* [한국어] nsdata 4KB 전체 0 reset — 비활성 NS의 spec 동작과 동일하게 만들어 NCAP=0 보장.
	 *  · 이후 spdk_nvme_ns_is_active 가 false 반환하여 사용자 IO 차단. */
	memset(nsdata, 0, sizeof(*nsdata));
	/* [한국어] id_desc_list 4KB 전체 0 reset — nvme_ns_find_id_desc 가 즉시 NULL 반환하게 됨. */
	memset(ns->id_desc_list, 0, sizeof(ns->id_desc_list));
	/* [한국어] ZNS/NVM IOCS-specific 4KB 버퍼들 해제 (hugepage 풀에 반환) + 포인터 NULL clear. */
	nvme_ns_free_iocs_specific_data(ns);
	/* [한국어] cooked sector_size 0 — getter가 0 반환하여 사용자가 NS 사용 불가 인지. */
	ns->sector_size = 0;
	/* [한국어] cooked extended_lba_size 0. */
	ns->extended_lba_size = 0;
	/* [한국어] cooked md_size 0 — PI 비활성 의미. */
	ns->md_size = 0;
	/* [한국어] PI Type disable. */
	ns->pi_type = 0;
	/* [한국어] sectors_per_max_io 0 — split 계산 시 0 division 위험하지만, IO 차단 상태이므로 도달 안 함. */
	ns->sectors_per_max_io = 0;
	/* [한국어] sectors_per_max_io_no_md 0. */
	ns->sectors_per_max_io_no_md = 0;
	/* [한국어] stripe 경계 0 (제약 없음). */
	ns->sectors_per_stripe = 0;
	/* [한국어] feature flag 모두 clear — NS가 어떤 명령도 지원하지 않는 상태로. */
	ns->flags = 0;
	/* [한국어] CSI 기본값 NVM으로 reset — 추후 재구성 시 안전한 시작점. */
	ns->csi = SPDK_NVME_CSI_NVM;
}
