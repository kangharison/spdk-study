/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation. All rights reserved.
 *   Copyright (c) 2024 Samsung Electronics Co., Ltd. All rights reserved.
 */

/**
 * \file
 * NVMe specification definitions
 */

/*
 * [한국어 설명] NVMe Base/Fabrics/ZNS/FDP 스펙 와이어 포맷 단일 소스 (nvme_spec.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 NVM Express(NVMe) Base Specification 1.x/2.x, NVMe over Fabrics(NVMe-oF) 1.x,
 * NVMe ZNS Command Set Specification, NVMe Command Set Specification(NVM CS), NVMe FDP
 * (Flexible Data Placement, TP4146) 등 NVMe 표준이 정의한 **모든 와이어 포맷**(컨트롤러
 * 레지스터 비트필드, SQE/CQE, Identify 데이터 구조체, Log Page, Get/Set Feature CDW
 * 인코딩, Status Code, Sanitize/Format/Firmware/Boot Partition 명령 페이로드)을 호스트
 * C 타입으로 1:1 매핑한다. 이 파일은 코드 로직을 갖지 않고 모든 멤버는 packed bit-field
 * 또는 fixed-width integer로 구성되며, 모든 구조체는 끝에 `SPDK_STATIC_ASSERT(sizeof) ==`
 * 검증을 두어 호스트 컴파일러의 패딩이 스펙 바이트 크기와 일치하지 않으면 빌드 자체가
 * 실패하도록 한다. SPDK 전체에서 NVMe 표준 ABI의 단일 진실 소스(single source of truth)
 * 이며, 새 NVMe TP(Technical Proposal)가 표준에 편입될 때마다 이 파일이 가장 먼저
 * 수정된다. NVMe Base 외의 확장은 이 파일이 아닌 별도 헤더(nvme_zns.h, nvme_intel.h,
 * nvme_ocssd_spec.h 등)가 추가 API를 노출하지만, 와이어 포맷 자체는 본 파일에 통합되어
 * 있다(예: ZNS Identify Namespace는 spdk_nvme_zns_ns_data로 본 파일에 정의되어 있고
 * 사용자 API 함수는 nvme_zns.h에 따로 노출).
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe 스택의 "타입 정의 뿌리"이다. 호출 체인 관점에서:
 *   사용자 코드(spdk_nvme_ns_cmd_read/write/...) →
 *   lib/nvme/nvme_ns_cmd.c (SQE 옵코드/CDW 채움) →
 *   lib/nvme/nvme_qpair.c (request → 본 파일의 struct spdk_nvme_cmd 슬롯) →
 *   lib/nvme/nvme_pcie_common.c::nvme_pcie_qpair_submit_request (PRP/SGL 빌드,
 *      doorbell write — 본 파일의 spdk_nvme_sgl_descriptor 사용) →
 *   PCIe MMIO + DMA(SQ/CQ 링은 hugepage, 컨트롤러 레지스터는 BAR0 매핑) →
 *   NVMe SSD 펌웨어가 동일 바이트 레이아웃 해석 →
 *   완료는 spdk_nvme_cpl 16B(SQHD/SQID/CID/Status)로 호스트 메모리 CQ 링에 기록 →
 *   spdk_nvme_qpair_process_completions()가 phase bit 토글로 새 CQE 검출 → cb_fn 호출.
 * 컨트롤러 초기화는 lib/nvme/nvme_ctrlr.c가 BAR0를 매핑한 뒤 본 파일의
 * struct spdk_nvme_registers 멤버를 spdk_mmio_read/write*로 직접 접근한다(CAP→AQA→ASQ→
 * ACQ→CC.EN→CSTS.RDY 폴링→Set Features→Identify Controller). NVMe-oF의 경우 물리 PCIe
 * 대신 RDMA/TCP/FC 트랜스포트가 같은 SQE/CQE 바이트 레이아웃을 wire로 전달하므로 이
 * 헤더는 호스트/타겟 양쪽 모두에서 동일하게 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h(uint8_t/16/32/64_t 고정폭 정수, 와이어 포맷에 필수),
 *        spdk/assert.h(SPDK_STATIC_ASSERT — 거의 모든 구조체 끝에 sizeof 검증).
 * 의존되는 곳:
 *   · lib/nvme/* (driver 전체) — qpair, ctrlr, ns, pcie, tcp, rdma, fabric, zns, opal,
 *     security, ns_cmd, transport 등 거의 모든 .c 파일이 이 헤더의 타입을 직접 사용.
 *   · lib/nvmf/* (NVMe-oF target) — 호스트 측 SQE/CQE를 그대로 받아 처리하므로
 *     동일 타입을 호스트와 공유. ctrlr.c, subsystem.c, ctrlr_bdev.c, ctrlr_discovery.c,
 *     transport.c, fabrics.c가 사용.
 *   · module/bdev/nvme/* — bdev_nvme.c가 read/write를 spdk_nvme_cmd로 변환,
 *     ZNS/Reservation/AER 처리 모두 이 헤더 의존.
 *   · include/spdk/nvme.h — 많은 인라인 함수(spdk_nvme_cpl_get_status_string 등)가
 *     본 파일의 enum/struct를 참조.
 *   · examples/nvme/identify, app/spdk_nvme_identify, app/spdk_nvme_perf — 진단/벤치
 *     앱이 본 파일의 모든 Identify/Log Page 구조체를 출력.
 * 데이터 흐름 관점:
 *   - 호스트 메모리(SQ ring) ↔ DMA ↔ 컨트롤러 처리 ↔ DMA ↔ 호스트 메모리(CQ ring,
 *     데이터 버퍼). SQE/CQE/SGL/PRP는 모두 본 헤더 타입이 그대로 wire에 직렬화된다.
 *   - 호스트 메모리(struct spdk_nvme_ctrlr_data 4096B) ← Identify Admin 명령 ← 컨트롤러
 *     펌웨어. 이 데이터는 attach 시 lib/nvme/nvme_ctrlr.c가 1회 캐시한다.
 *   - 컨트롤러 레지스터(BAR0) ↔ MMIO load/store ↔ 호스트 (struct spdk_nvme_registers).
 * 공유 자료구조: 본 헤더가 정의하는 모든 타입은 SPDK 전역에서 "공유 ABI"이며, 호스트
 *   주소 공간에서는 보통 hugepage(DMA pinned) 또는 BAR0 매핑된 메모리에 위치한다.
 *
 * === 주요 함수/구조체 요약 ===
 * 함수 없음 — 모두 타입/상수 정의이다. 다음이 가장 빈번히 참조되는 핵심:
 *   1) Controller Registers (struct spdk_nvme_registers, BAR0 0x00~0xFFF):
 *      CAP(8B, capabilities)/VS(4B, version)/INTMS·INTMC(4B, interrupt mask)/
 *      CC(4B, controller config)/CSTS(4B, status)/NSSR(4B, subsystem reset)/
 *      AQA(4B, admin queue attributes)/ASQ·ACQ(8B, admin queue base addr)/
 *      CMB·BPx·PMR 시리즈(메모리 버퍼/부트 파티션/지속 메모리 영역) +
 *      Doorbell (offset 0x1000부터 SQ/CQ tail/head). 모두 packed union의 raw + bits.
 *   2) I/O 경로 — struct spdk_nvme_cmd(64B SQE) / spdk_nvme_cpl(16B CQE) /
 *      spdk_nvme_status(2B 상태 비트필드) / spdk_nvme_sgl_descriptor(16B SGL),
 *      enum spdk_nvme_psdt_value(PRP/SGL/메타데이터 SGL 선택), spdk_nvme_data_transfer
 *      (Host→Controller, Controller→Host, 양방향 등 4가지).
 *   3) Opcode enum — spdk_nvme_admin_opcode(0x00~0x7F admin), spdk_nvme_nvm_opcode
 *      (NVM CS I/O), spdk_nvme_zns_opcode(ZNS), spdk_nvme_directive_type 등.
 *   4) Status Code — enum spdk_nvme_status_code_type(SCT 0~7), generic/command-specific/
 *      media/path status code(SC 0x00~0xFF). status string 변환은 nvme.h의 인라인.
 *   5) Identify — struct spdk_nvme_ctrlr_data(4096B, CNS=0x01) / spdk_nvme_ns_data
 *      (4096B, CNS=0x00) / spdk_nvme_zns_ctrlr_data / spdk_nvme_zns_ns_data /
 *      spdk_nvme_nvm_ctrlr_data / spdk_nvme_ns_id_desc(NS Identification descriptor) /
 *      spdk_nvme_primary_ctrl_capabilities / spdk_nvme_secondary_ctrl_list 등 4096B 페이지.
 *   6) Get/Set Feature CDW — union spdk_nvme_feat_*(arbitration, power_management,
 *      temperature_threshold, error_recovery, volatile_write_cache, number_of_queues,
 *      interrupt_coalescing, async_event_configuration, host_mem_buffer, keep_alive_timer,
 *      host_controlled_thermal_management, ...) 모두 32비트 CDW11 비트필드.
 *   7) Log Page — struct spdk_nvme_error_information_entry(64B) / health_information_page
 *      (512B SMART) / firmware_page(512B) / cmds_and_effect_log_page(4096B) /
 *      telemetry_log_page_hdr(512B) / sanitize_status_log_page(512B) /
 *      ana_page(ANA multipath) / fdp_*(FDP 4가지 로그) / reservation_notification_log.
 *   8) ZNS — enum spdk_nvme_zns_zone_type/state/zra_report_opts/zone_send_action/
 *      zone_receive_action, struct spdk_nvme_zns_zone_desc(64B), spdk_nvme_zns_zone_report.
 *   9) Reservation — enum spdk_nvme_reservation_type(7가지), struct
 *      spdk_nvme_reservation_acquire_data/register_data/key_data/status_data/
 *      registered_ctrlr_data, reservation_release_action, notification log type.
 *  10) Format/Firmware/Sanitize — struct spdk_nvme_format(4B CDW10),
 *      spdk_nvme_fw_commit(4B CDW10), spdk_nvme_sanitize(4B CDW10), enum
 *      spdk_sanitize_action / spdk_nvme_fw_commit_action / spdk_nvme_secure_erase_setting.
 *  11) Async Event — enum spdk_nvme_async_event_type/info_*(error/smart/notice/nvm),
 *      union spdk_nvme_async_event_completion(CDW0 4B). AER는 Admin SQ에 미리 적재되어
 *      이벤트 발생 시 컨트롤러가 CQE로 통지(인터럽트 회피의 polled-mode 모델).
 *  12) Boot Partition — union spdk_nvme_bpinfo_register/bprsel_register, enum
 *      spdk_nvme_brs_value(boot read status). NVMe Base 1.4 §3.7.
 *  13) FDP — TP4146 정의 모두(struct spdk_nvme_fdp_event/event_desc/ruh_descriptor/
 *      cfg_descriptor/ruhu_descriptor/stats_log_page/events_log_page).
 *  14) Directive — enum spdk_nvme_directive_type(Identify=0/Streams=1) +
 *      Streams send/receive operation, struct spdk_nvme_ns_streams_data/_status.
 *
 * 본 한국어 주석 정비는 NVMe Base 2.0/Fabrics 1.1/ZNS 1.1/FDP TP4146까지의 와이어
 * 포맷에 대해 4섹션 상단 블록 + 핵심 I/O 경로 구조체의 필드 멀티라인 주석을 제공한다.
 * 5131줄에 달하는 모든 enum 값/필드/매크로의 완전 주석화는 후속 세션에서 점진적으로
 * 확장하며, 현 시점에서는 (a) 상단 4섹션 블록, (b) 컨트롤러 레지스터 그룹, (c) SGL
 * 디스크립터, (d) SQE/CQE/Status, (e) Opcode/Feature/Status code 핵심 enum,
 * (f) 핵심 Identify 구조체에 우선 집중한다.
 */

#ifndef SPDK_NVME_SPEC_H          /* [한국어] include 가드 */
#define SPDK_NVME_SPEC_H

#include "spdk/stdinc.h"          /* [한국어] 표준 정수 타입 */

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/assert.h"          /* [한국어] SPDK_STATIC_ASSERT — 구조체 크기 스펙 일치 검증용 */

/**
 * Use to mark a command to apply to all namespaces, or to retrieve global
 *  log pages.
 */
#define SPDK_NVME_GLOBAL_NS_TAG		((uint32_t)0xFFFFFFFF)
                                  /* [한국어] NSID=0xFFFFFFFF 특수값 — "모든 네임스페이스 대상" 의미
                                   *  - Identify/Get Log Page 등 명령에서 사용
                                   *  - NVMe 스펙 §6.1 — 개별 네임스페이스가 아닌 global 정보 요청 시 전달 */

#define SPDK_NVME_MAX_IO_QUEUES		(65535)
                                  /* [한국어] NVMe 스펙상 허용되는 I/O 큐의 최대 수 (QID 0은 admin, 1~65535 범위) */

#define SPDK_NVME_QUEUE_MIN_ENTRIES		(2)
                                  /* [한국어] 큐 깊이 최소값 — 엔트리 2개 미만은 스펙 위반 */

#define SPDK_NVME_ADMIN_QUEUE_MIN_ENTRIES	SPDK_NVME_QUEUE_MIN_ENTRIES
                                  /* [한국어] admin 큐 최소 깊이 */
#define SPDK_NVME_ADMIN_QUEUE_MAX_ENTRIES	4096
                                  /* [한국어] admin 큐 최대 깊이 (스펙 상한) */

/* Controllers with quirk NVME_QUIRK_MINIMUM_ADMIN_QUEUE_SIZE must have
 * admin queue size entries that are an even multiple of this number.
 */
#define SPDK_NVME_ADMIN_QUEUE_QUIRK_ENTRIES_MULTIPLE	64
                                  /* [한국어] 특정 벤더 컨트롤러가 요구하는 admin 큐 크기 단위
                                   *  - SPDK는 quirk 플래그로 감지하여 해당 컨트롤러에 대해 큐 크기를 64의 배수로 강제 */

#define SPDK_NVME_IO_QUEUE_MIN_ENTRIES		SPDK_NVME_QUEUE_MIN_ENTRIES
                                  /* [한국어] I/O 큐 최소 깊이 */
#define SPDK_NVME_IO_QUEUE_MAX_ENTRIES		65536
                                  /* [한국어] I/O 큐 최대 깊이 (스펙상 16비트 인덱스 전 범위) */

/**
 * Indicates the maximum number of range sets that may be specified
 *  in the dataset management command.
 */
#define SPDK_NVME_DATASET_MANAGEMENT_MAX_RANGES	256
                                  /* [한국어] Dataset Management(TRIM/Discard) 명령 하나가 다룰 수 있는 범위 세트 최대 개수
                                   *  - SPDK bdev unmap 경로에서 큰 TRIM을 이 값으로 분할 */

/**
 * Maximum number of blocks that may be specified in a single dataset management range.
 */
#define SPDK_NVME_DATASET_MANAGEMENT_RANGE_MAX_BLOCKS	0xFFFFFFFFu
                                  /* [한국어] DSM 단일 범위의 최대 블록 수 (32비트 전 범위) */

/**
 * Maximum number of entries in the log page of Changed Namespace List.
 */
#define SPDK_NVME_MAX_CHANGED_NAMESPACES 1024
                                  /* [한국어] Changed Namespace List 로그 페이지의 최대 엔트리 수 (스펙) */

#define SPDK_NVME_DOORBELL_REGISTER_SIZE 4
                                  /* [한국어] 각 doorbell 레지스터 크기 = 4B (DSTRD=0 기준)
                                   *  - 실제 stride는 CAP.DSTRD로 확장 가능 (8B/16B...) */

/* [한국어] === Controller Capabilities (CAP) — BAR0 offset 0x00, 8B 64-bit RO ===
 * NVMe Base 2.0 §3.1.3.1. 컨트롤러 attach 시 lib/nvme/nvme_ctrlr.c가 가장 먼저 읽는다.
 * 이 값으로 (1) 큐 최대 깊이, (2) 페이지 크기 범위, (3) 명령 셋 지원, (4) timeout,
 * (5) doorbell stride, (6) CMB/PMR/Boot Partition 지원 여부를 알아내 컨트롤러 초기화
 * 시퀀스(CC.MPS·CC.CSS 결정, AQA 설정 등)를 분기한다. 64비트 단위로 한 번에 읽어야 하며
 * (스펙상 partial read 미정의), SPDK는 spdk_mmio_read_8()로 읽는다. */
union spdk_nvme_cap_register {
	uint64_t	raw;
	/* [한국어] CAP 64비트 통째 — endian 변환 후 멤버별 접근하거나, raw로 한 번에 읽어
	 * spdk_mmio 영역의 partial-read 비결정성을 피한다. */
	struct {
		/** maximum queue entries supported */
		uint32_t mqes		: 16;
		/* [한국어] CAP.MQES — 큐 한 개의 최대 엔트리 수(0's based, 실제 값 = mqes+1).
		 * lib/nvme/nvme_ctrlr.c가 SQ/CQ 깊이를 min(요청, mqes+1)로 클램프한다. */

		/** contiguous queues required */
		uint32_t cqr		: 1;
		/* [한국어] CAP.CQR=1이면 큐의 물리 메모리가 연속이어야 한다. SPDK는 hugepage로
		 * 큐를 할당하므로 일반적으로 자동 만족. =0이면 PRP list로 분산 가능. */

		/** arbitration mechanism supported */
		uint32_t ams		: 2;
		/* [한국어] CAP.AMS — round-robin 외에 weighted RR(bit0)/vendor(bit1) 지원 여부.
		 * CC.AMS 설정 시 이 비트로 지원 여부 검증. */

		uint32_t reserved1	: 5;
		/* [한국어] 예약(0). 미래 확장. */

		/** timeout */
		uint32_t to		: 8;
		/* [한국어] CAP.TO — CSTS.RDY 토글까지의 worst-case 지연(500ms 단위). 호스트는
		 * CC.EN=1 후 to*500ms 동안 CSTS.RDY를 폴링한 뒤 timeout 처리. */

		/** doorbell stride */
		uint32_t dstrd		: 4;
		/* [한국어] CAP.DSTRD — doorbell 간 stride = 2^(2+dstrd) bytes. 0이면 4B(default),
		 * 일부 컨트롤러는 cache-line align(0xF)으로 false-sharing 회피. SPDK는
		 * (1 << (2 + dstrd))로 SQ_n_TDBL/CQ_n_HDBL 오프셋 계산. */

		/** NVM subsystem reset supported */
		uint32_t nssrs		: 1;
		/* [한국어] CAP.NSSRS — NSSR(0x20)에 SPDK_NVME_NSSR_VALUE(0x4E564D65='NVMe')를
		 * 쓰면 서브시스템 전체 리셋. =0이면 NSSR write가 무시된다. */

		/** command sets supported */
		uint32_t css		: 8;
		/* [한국어] CAP.CSS — 비트마스크. bit0=NVM CS, bit6=I/O CS(ZNS/KV 등 다중 CS),
		 * bit7=Admin Only. CC.CSS 설정 시 여기 set bit로만 가능. */

		/** boot partition support */
		uint32_t bps		: 1;
		/* [한국어] CAP.BPS=1이면 Boot Partition 영역과 BPx 레지스터 사용 가능
		 * (NVMe Base 1.4 §3.7). UEFI 펌웨어 부트 영역. */

		uint32_t reserved2	: 2;
		/* [한국어] 예약(0). */

		/** memory page size minimum */
		uint32_t mpsmin		: 4;
		/* [한국어] CAP.MPSMIN — 호스트 메모리 페이지 크기 최소값. 실제 = 2^(12+mpsmin) bytes.
		 * 0이면 4KB. CC.MPS는 mpsmin~mpsmax 범위 내에서만 설정 가능. */

		/** memory page size maximum */
		uint32_t mpsmax		: 4;
		/* [한국어] CAP.MPSMAX — 호스트 메모리 페이지 크기 최대값. 실제 = 2^(12+mpsmax). */

		/** persistent memory region supported */
		uint32_t pmrs		: 1;
		/* [한국어] CAP.PMRS=1이면 PMR(Persistent Memory Region) 사용 가능 — PMRCAP/
		 * PMRCTL/PMRSTS/PMRMSCL/PMRMSCU/PMREBS/PMRSWTP 레지스터 활성. */

		/** controller memory buffer supported */
		uint32_t cmbs		: 1;
		/* [한국어] CAP.CMBS=1이면 CMB(Controller Memory Buffer) 사용 가능 — 컨트롤러
		 * 자체 RAM을 호스트가 PCIe BAR로 매핑해 SQ/CQ/PRP/data로 활용 가능. lib/nvme/
		 * nvme_pcie.c::nvme_pcie_ctrlr_alloc_cmb_io_buffer가 사용. */

		/** NVM subsystem shutdown supported */
		uint32_t nsss		: 1;
		/* [한국어] CAP.NSSS=1(NVMe 2.0)이면 NSSD(NVM Subsystem Shutdown) 명령 지원. */

		/** controller ready with media support */
		uint32_t crwms		: 1;
		/* [한국어] CAP.CRWMS — Controller Ready With Media Support. =1이면 CC.EN=1 후
		 * media 준비까지 기다려서 RDY 보고 가능(NVMe 2.0). */

		/** controller ready independent of media support */
		uint32_t crims		: 1;
		/* [한국어] CAP.CRIMS — media 준비 이전에도 CSTS.RDY=1 가능. CC.CRIME=1과 짝.
		 * 부팅 시간을 단축하지만 첫 I/O는 NVMe error로 reject 될 수 있음. */

		uint32_t reserved3	: 3;
		/* [한국어] 예약(0). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_cap_register) == 8, "Incorrect size");
/* [한국어] CAP는 정확히 8B여야 한다 — 컴파일러 패딩 발생 시 BAR0 매핑이 어긋난다. */

/**
 * I/O Command Set Selected
 *
 * Only a single command set is defined as of NVMe 1.3 (NVM). Later, it became
 * possible to disable I/O Command Sets, that is, configuring it to only use the
 * Admin Command Set. With 1.4c and Namespace Types, additional I/O Command Sets
 * are available.
 */
/* [한국어] === CC.CSS — Controller Configuration의 I/O Command Set Selected 필드 ===
 * NVMe 1.3 이전엔 NVM CS만 존재해 CSS=0이 사실상 유일했다. NVMe 1.4c에서 Namespace
 * Type/I/O Command Set Combination을 도입하면서 CSS=6(One or more I/O CS, 즉 ZNS/KV/NVM
 * 혼합)이 추가되었고, CSS=7(No I/O)은 Admin Only 컨트롤러(예: Discovery, MI). */
enum spdk_nvme_cc_css {
	SPDK_NVME_CC_CSS_NVM		= 0x0,	/**< NVM command set */
	/* [한국어] 0x0 — NVM Command Set만 사용. 가장 일반적인 모드.
	 * I/O 큐에서 read/write/flush 등 NVM CS opcode만 발행 가능. */
	SPDK_NVME_CC_CSS_IOCS		= 0x6,	/**< One or more I/O command sets */
	/* [한국어] 0x6 — Multi-Command-Set 모드. 네임스페이스마다 CSI(Command Set Identifier)
	 * 로 NVM/ZNS/KV 중 선택. lib/nvme가 ZNS namespace를 사용하려면 이 값으로 enable. */
	SPDK_NVME_CC_CSS_NOIO		= 0x7,	/**< No I/O, only admin */
	/* [한국어] 0x7 — I/O 큐 없이 Admin만 동작. Discovery 컨트롤러 / NVMe-MI 전용. */
};

#define SPDK_NVME_CAP_CSS_NVM (1u << SPDK_NVME_CC_CSS_NVM) /**< NVM command set supported */
/* [한국어] CAP.CSS bit0 — NVM CS 지원 여부. CC.CSS=0 설정 시 이 비트가 1이어야 함. */
#define SPDK_NVME_CAP_CSS_IOCS (1u << SPDK_NVME_CC_CSS_IOCS) /**< One or more I/O Command sets supported */
/* [한국어] CAP.CSS bit6 — Multi I/O CS 지원 여부. ZNS/KV 사용 전 필수 검증. */
#define SPDK_NVME_CAP_CSS_NOIO (1u << SPDK_NVME_CC_CSS_NOIO) /**< No I/O, only admin */
/* [한국어] CAP.CSS bit7 — Admin Only 컨트롤러 지원 여부. Discovery/MI 컨트롤러는 1. */

/* [한국어] === Controller Configuration (CC) — BAR0 offset 0x14, 4B RW ===
 * 호스트가 컨트롤러에 "동작 시작/정지" 신호를 주는 메인 레지스터. NVMe Base 2.0 §3.1.3.5.
 * 초기화 시퀀스: CC.EN=0 → CSTS.RDY=0 대기 → AQA/ASQ/ACQ 설정 → CC.CSS/MPS/AMS 설정 →
 * CC.IOSQES=6/IOCQES=4(SQE=64B/CQE=16B 인코딩) → CC.EN=1 → CSTS.RDY=1 폴링.
 * 종료 시: CC.SHN=01b/10b 설정 → CSTS.SHST=10b 대기 후 PCI off. */
union spdk_nvme_cc_register {
	uint32_t	raw;
	/* [한국어] CC 32비트 통째 — 일반적으로 raw 단위로 RMW 후 spdk_mmio_write_4. */
	struct {
		/** enable */
		uint32_t en		: 1;
		/* [한국어] CC.EN — 0→1로 토글하면 컨트롤러가 admin 큐 활성화 시작.
		 * CSTS.RDY가 1이 될 때까지 CAP.TO*500ms 폴링. 1→0이면 reset(데이터 손실 위험). */

		uint32_t reserved1	: 3;
		/* [한국어] 예약(0). */

		/** i/o command set selected */
		uint32_t css		: 3;
		/* [한국어] CC.CSS — 위 enum 참조. CC.EN=0 상태에서만 변경 가능. */

		/** memory page size */
		uint32_t mps		: 4;
		/* [한국어] CC.MPS — 호스트 메모리 페이지 크기 = 2^(12+mps). PRP1/PRP2 정렬과
		 * 데이터 transfer chunk 단위에 영향. CAP.MPSMIN~MAX 범위 내에서만 설정. */

		/** arbitration mechanism selected */
		uint32_t ams		: 3;
		/* [한국어] CC.AMS — 0=Round Robin(default), 1=Weighted RR with Urgent class,
		 * 7=Vendor specific. CAP.AMS에서 지원 여부 확인 후 설정. */

		/** shutdown notification */
		uint32_t shn		: 2;
		/* [한국어] CC.SHN — 0=No shutdown, 1=Normal(데이터 flush 후 종료),
		 * 2=Abrupt(즉시 종료, 데이터 손실 가능). CSTS.SHST로 진행 상태 확인. */

		/** i/o submission queue entry size */
		uint32_t iosqes		: 4;
		/* [한국어] CC.IOSQES — SQE 크기 = 2^iosqes bytes. NVM CS는 64B이므로 6. */

		/** i/o completion queue entry size */
		uint32_t iocqes		: 4;
		/* [한국어] CC.IOCQES — CQE 크기 = 2^iocqes bytes. NVM CS는 16B이므로 4. */

		/** controller ready independent of media enable */
		uint32_t crime		: 1;
		/* [한국어] CC.CRIME — CAP.CRIMS=1일 때만 의미. 1이면 media 미준비여도 RDY=1.
		 * 부팅 시간 단축용; 첫 read는 status code Namespace Not Ready로 떨어질 수 있음. */

		uint32_t reserved2	: 7;
		/* [한국어] 예약(0). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_cc_register) == 4, "Incorrect size");
/* [한국어] CC는 정확히 4B(32비트). */

/* [한국어] === Shutdown Notification 값 (CC.SHN) ===
 * NVMe Base 2.0 §3.1.3.5 Table 24. */
enum spdk_nvme_shn_value {
	SPDK_NVME_SHN_NORMAL		= 0x1,
	/* [한국어] 0x1 — Normal shutdown. 컨트롤러는 미완료 명령을 처리하고 캐시를
	 * media에 flush한다. 호스트는 CSTS.SHST=2(complete)까지 대기 후 PCI off. */
	SPDK_NVME_SHN_ABRUPT		= 0x2,
	/* [한국어] 0x2 — Abrupt shutdown. 즉시 종료. 캐시되지 않은 데이터가 손실 가능.
	 * 비상 종료(전원 손실 직전 등)에만 사용. */
};

/* [한국어] === Controller Status (CSTS) — BAR0 offset 0x1C, 4B RO ===
 * 컨트롤러의 현재 상태. CC 변경의 결과 확인용. NVMe Base 2.0 §3.1.3.6. */
union spdk_nvme_csts_register {
	uint32_t	raw;
	/* [한국어] CSTS 32비트 raw — polled-mode 컨트롤러 상태 폴링에 자주 읽힌다. */
	struct {
		/** ready */
		uint32_t rdy		: 1;
		/* [한국어] CSTS.RDY — CC.EN과 함께 변동. 1이면 admin 큐 처리 가능. 호스트는
		 * CC.EN=1 후 CAP.TO*500ms 동안 이 비트가 1이 되길 폴링. */

		/** controller fatal status */
		uint32_t cfs		: 1;
		/* [한국어] CSTS.CFS — 1이면 컨트롤러 내부 fatal error. 모든 I/O 정지, 리셋 필수. */

		/** shutdown status */
		uint32_t shst		: 2;
		/* [한국어] CSTS.SHST — 0=Normal, 1=Shutdown 진행 중, 2=Shutdown 완료. */

		/** NVM subsystem reset occurred */
		uint32_t nssro		: 1;
		/* [한국어] CSTS.NSSRO — 1이면 NSSR(NVM Subsystem Reset)이 발생했었음. RWC. */

		/** Processing paused */
		uint32_t pp		: 1;
		/* [한국어] CSTS.PP — 1이면 명령 처리 일시 정지(thermal/power 이슈 등). */

		uint32_t reserved1	: 26;
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_csts_register) == 4, "Incorrect size");

/* [한국어] === Shutdown Status (CSTS.SHST) === NVMe Base 2.0 §3.1.3.6 Table 25. */
enum spdk_nvme_shst_value {
	SPDK_NVME_SHST_NORMAL		= 0x0,
	/* [한국어] 0x0 — 정상 동작 중. shutdown 진행 안 함. */
	SPDK_NVME_SHST_OCCURRING	= 0x1,
	/* [한국어] 0x1 — shutdown 처리 중. flush 완료 대기. */
	SPDK_NVME_SHST_COMPLETE		= 0x2,
	/* [한국어] 0x2 — shutdown 완료. 호스트가 PCI off 가능. */
};

/* [한국어] === Admin Queue Attributes (AQA) — BAR0 offset 0x24, 4B RW ===
 * Admin SQ/CQ의 큐 깊이를 설정. NVMe Base 2.0 §3.1.3.7. CC.EN=0 상태에서만 변경. */
union spdk_nvme_aqa_register {
	uint32_t	raw;
	struct {
		/** admin submission queue size */
		uint32_t asqs		: 12;
		/* [한국어] AQA.ASQS — Admin SQ 깊이(0's based, 실제 값 = asqs+1). 최대 4096(0xFFF+1).
		 * lib/nvme/nvme_ctrlr.c::nvme_ctrlr_construct_admin_qpair에서 설정. */

		uint32_t reserved1	: 4;
		/* [한국어] 예약(0). */

		/** admin completion queue size */
		uint32_t acqs		: 12;
		/* [한국어] AQA.ACQS — Admin CQ 깊이(0's based). 일반적으로 ASQS와 같게 설정. */

		uint32_t reserved2	: 4;
		/* [한국어] 예약(0). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_aqa_register) == 4, "Incorrect size");
/* [한국어] AQA는 정확히 4B. */

/* [한국어] === Version (VS) — BAR0 offset 0x08, 4B RO ===
 * 컨트롤러가 구현한 NVMe 스펙 버전. NVMe Base 2.0 §3.1.3.2. 호스트는 이 값으로
 * 1.x vs 2.x 호환성 분기(ANA, ZNS, FDP 등 신규 기능 활성화 여부 결정)를 한다. */
union spdk_nvme_vs_register {
	uint32_t	raw;
	struct {
		/** indicates the tertiary version */
		uint32_t ter		: 8;
		/* [한국어] VS.TER — 3차 버전(예: 1.4.1의 .1). 패치 레벨. */
		/** indicates the minor version */
		uint32_t mnr		: 8;
		/* [한국어] VS.MNR — minor 버전(예: 1.4의 4). */
		/** indicates the major version */
		uint32_t mjr		: 16;
		/* [한국어] VS.MJR — major 버전(예: 1.4의 1). 16비트는 미래 확장 여유. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_vs_register) == 4, "Incorrect size");

/** Generate raw version in the same format as \ref spdk_nvme_vs_register for comparison. */
/* [한국어] SPDK_NVME_VERSION(mjr,mnr,ter) — VS 레지스터 raw 값과 직접 비교 가능한
 * 32비트 인코딩 생성. 예: SPDK_NVME_VERSION(1,4,0) == 0x00010400. lib/nvme 코드에서
 * "if (vs.raw >= SPDK_NVME_VERSION(1,4,0)) { /* 1.4 features */ }" 패턴으로 사용. */
#define SPDK_NVME_VERSION(mjr, mnr, ter) \
	(((uint32_t)(mjr) << 16) | \
	((uint32_t)(mnr) << 8) | \
	(uint32_t)(ter))

/* Test that the shifts are correct */
SPDK_STATIC_ASSERT(SPDK_NVME_VERSION(1, 0, 0) == 0x00010000, "version macro error");
SPDK_STATIC_ASSERT(SPDK_NVME_VERSION(1, 2, 1) == 0x00010201, "version macro error");

/* [한국어] === Controller Memory Buffer Location (CMBLOC) — offset 0x38, 4B RO ===
 * CMB는 컨트롤러가 자체 RAM을 PCIe BAR에 노출해 호스트가 SQ/CQ/PRP list/data를 지연 없이
 * 접근하게 하는 기능. CAP.CMBS=1일 때만 유효. NVMe Base 2.0 §3.1.3.10. */
union spdk_nvme_cmbloc_register {
	uint32_t	raw;
	struct {
		/** indicator of BAR which contains controller memory buffer(CMB) */
		uint32_t bir		: 3;
		/* [한국어] CMBLOC.BIR — CMB가 매핑된 PCIe BAR 인덱스(0~5). lib/nvme/nvme_pcie.c가
		 * 이 BAR를 mmap해서 CMB IO buffer로 사용한다. */
		uint32_t reserved1	: 9;
		/* [한국어] 예약(0). */
		/** offset of CMB in multiples of the size unit */
		uint32_t ofst		: 20;
		/* [한국어] CMBLOC.OFST — BAR 내 CMB 시작 오프셋 = ofst * (CMBSZ.szu 크기 단위).
		 * 즉 실제 BAR 주소 = BAR base + ofst * 4KB(szu=0 기준). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_cmbloc_register) == 4, "Incorrect size");

/* [한국어] === Controller Memory Buffer Size (CMBSZ) — offset 0x3C, 4B RO ===
 * CMB 크기와 어떤 종류의 데이터를 CMB에 둘 수 있는지(SQ/CQ/PRP list/read data/write data). */
union spdk_nvme_cmbsz_register {
	uint32_t	raw;
	struct {
		/** support submission queues in CMB */
		uint32_t sqs		: 1;
		/* [한국어] CMBSZ.SQS — 1이면 CMB에 SQ 배치 가능(I/O latency 감소). */
		/** support completion queues in CMB */
		uint32_t cqs		: 1;
		/* [한국어] CMBSZ.CQS — 1이면 CMB에 CQ 배치 가능(드물게 지원). */
		/** support PRP and SGLs lists in CMB */
		uint32_t lists		: 1;
		/* [한국어] CMBSZ.LISTS — 1이면 PRP list/SGL을 CMB에 둬서 컨트롤러가 fetch 시
		 * 호스트 메모리 trip을 절약. */
		/** support read data and metadata in CMB */
		uint32_t rds		: 1;
		/* [한국어] CMBSZ.RDS — 1이면 read 결과 데이터/메타데이터를 CMB에 직접 받기 가능. */
		/** support write data and metadata in CMB */
		uint32_t wds		: 1;
		/* [한국어] CMBSZ.WDS — 1이면 write 데이터를 CMB에 두고 컨트롤러가 PRP/SGL로
		 * CMB 주소를 가리켜 zero-copy. */
		uint32_t reserved1	: 3;
		/* [한국어] 예약(0). */
		/** indicates the granularity of the size unit */
		uint32_t szu		: 4;
		/* [한국어] CMBSZ.SZU — size unit. 0=4KB, 1=64KB, 2=1MB, ..., 6=4GB. */
		/** size of CMB in multiples of the size unit */
		uint32_t sz		: 20;
		/* [한국어] CMBSZ.SZ — CMB 크기 = sz * (szu 단위). 0이면 CMB 비활성. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_cmbsz_register) == 4, "Incorrect size");

/* [한국어] === CMB Memory Space Control (CMBMSC) — offset 0x50, 8B RW (NVMe 1.4+) ===
 * CMB를 호스트가 임의 PCIe 주소 공간에 매핑할 수 있게 하는 controller memory space.
 * NVMe Base 2.0 §3.1.3.16. */
union spdk_nvme_cmbmsc_register {
	uint64_t	raw;
	struct {
		/** capability registers enabled */
		uint64_t cre		: 1;
		/* [한국어] CMBMSC.CRE — capability register access enable. 1이면 CMB 영역 첫 부분에
		 * 컨트롤러가 노출하는 capability registers(추가 제어용)를 참조 가능. */

		/** controller memory space enable */
		uint64_t cmse		: 1;
		/* [한국어] CMBMSC.CMSE — controller memory space enable. 1이면 컨트롤러가 호스트의
		 * 일반 시스템 메모리처럼 CMB를 PCIe target 주소(CBA)로 응답. zero-copy 전제. */

		uint64_t reserved	: 10;
		/* [한국어] 예약(0). */

		/** controller base address */
		uint64_t cba		: 52;
		/* [한국어] CMBMSC.CBA — controller base address. (cba << 12)이 CMB의 PCIe target
		 * 주소. 호스트는 이 값을 SQE PRP/SGL에 직접 적어 컨트롤러가 자기 RAM에 DMA. */
	} bits;

};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_cmbmsc_register) == 8, "Incorrect size");

/* [한국어] === CMB Status (CMBSTS) — offset 0x58, 4B RO === */
union spdk_nvme_cmbsts_register {
	uint32_t	raw;
	struct {
		/** controller base address invalid */
		uint32_t cbai		: 1;
		/* [한국어] CMBSTS.CBAI — 1이면 CMBMSC.CBA가 잘못 설정됨(BAR 범위 밖 등). */

		uint32_t reserved	: 31;
		/* [한국어] 예약(0). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_cmbsts_register) == 4, "Incorrect size");

/* [한국어] === CMB Elasticity Buffer Size (CMBEBS) — offset 0x5C, 4B RO ===
 * NVMe 2.0에서 CMB write가 즉시 media에 가지 않고 elasticity 버퍼에 누적되는 경우의 크기. */
union spdk_nvme_cmbebs_register {
	uint32_t	raw;
	struct {
		/** CMB Elasticity Buffer Size Units */
		uint32_t cmbszu		: 4;
		/* [한국어] CMBEBS.CMBSZU — 단위(0=Bytes, 1=KiB, 2=MiB, 3=GiB). */
		/** CMB Read Bypass Behavior */
		uint32_t cmbrbb		: 1;
		/* [한국어] CMBEBS.CMBRBB — read가 elasticity buffer를 우회하는지 여부. */

		uint32_t reserved	: 3;
		/* [한국어] 예약(0). */
		/** CMB elasticity buffer size base */
		uint32_t cmbwbz		: 24;
		/* [한국어] CMBEBS.CMBWBZ — 실제 elasticity buffer 크기 = cmbwbz * (cmbszu 단위). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_cmbebs_register) == 4, "Incorrect size");

/* [한국어] === CMB Sustained Write Throughput (CMBSWTP) — offset 0x60, 4B RO ===
 * CMB로의 sustained write 처리량 보장값. */
union spdk_nvme_cmbswtp_register {
	uint32_t	raw;
	struct {
		/** CMB Sustained Write Throughput Units */
		uint32_t cmbswtu	: 4;
		/* [한국어] CMBSWTP.CMBSWTU — 단위(0=B/s, 1=KiB/s, 2=MiB/s, 3=GiB/s). */

		uint32_t reserved	: 4;
		/* [한국어] 예약(0). */
		/** CMB Sustained Write Throughput */
		uint32_t cmbswtv	: 24;
		/* [한국어] CMBSWTP.CMBSWTV — 처리량 = cmbswtv * (cmbswtu 단위). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_cmbswtp_register) == 4, "Incorrect size");

/* [한국어] === Controller Ready Timeouts (CRTO) — offset 0x68, 4B RO (NVMe 2.0) ===
 * CAP.TO를 대체하는 16비트 타임아웃(부팅 시간이 길어짐에 따라 8비트로 부족). */
union spdk_nvme_crto_register {
	uint32_t	raw;
	struct {
		/** Controller Ready With Media Timeout */
		uint32_t crwmt	: 16;
		/* [한국어] CRTO.CRWMT — CAP.CRWMS=1이고 CC.CRIME=0일 때의 RDY 대기 시간(100ms 단위). */
		/** Controller Ready Independent of Media Timeout */
		uint32_t crimt	: 16;
		/* [한국어] CRTO.CRIMT — CC.CRIME=1일 때의 RDY 대기 시간(100ms 단위, 보통 더 짧다). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_crto_register) == 4, "Incorrect size");


/* [한국어] === Persistent Memory Region Capabilities (PMRCAP) — offset 0xE00, 4B RO ===
 * PMR은 컨트롤러가 노출하는 비휘발성 메모리 영역(예: 컨트롤러 측 NVDIMM/MRAM)으로,
 * 호스트는 PCIe BAR를 통해 byte-addressable로 접근 가능. CMB의 비휘발 버전.
 * NVMe Base 2.0 §3.1.4. */
union spdk_nvme_pmrcap_register {
	uint32_t	raw;
	struct {
		uint32_t reserved1	: 3;
		/* [한국어] 예약(0). */

		/** read data support */
		uint32_t rds		: 1;
		/* [한국어] PMRCAP.RDS — 1이면 PMR 영역 read 가능. */

		/** write data support */
		uint32_t wds		: 1;
		/* [한국어] PMRCAP.WDS — 1이면 PMR 영역 write 가능. */

		/** base indicator register */
		uint32_t bir		: 3;
		/* [한국어] PMRCAP.BIR — PMR이 매핑된 PCIe BAR 인덱스. */

		/**
		 * persistent memory region time units
		 * 00b: 500 milliseconds
		 * 01b: minutes
		 */
		uint32_t pmrtu		: 2;
		/* [한국어] PMRCAP.PMRTU — PMRTO 단위(00=500ms, 01=분). 부팅 시 PMR ready
		 * 대기 시간 계산용. */

		/** persistent memory region write barrier mechanisms */
		uint32_t pmrwbm		: 4;
		/* [한국어] PMRCAP.PMRWBM — write durability 보장 메커니즘(컨트롤러가 power-fail
		 * 시 데이터 안정 보장하는 방식). */

		uint32_t reserved2	: 2;
		/* [한국어] 예약(0). */

		/** persistent memory region timeout */
		uint32_t pmrto		: 8;
		/* [한국어] PMRCAP.PMRTO — PMRCTL.EN=1 후 ready까지 timeout(pmrtu 단위). */

		/** controller memory space supported */
		uint32_t cmss		: 1;
		/* [한국어] PMRCAP.CMSS — CMB와 같은 controller memory space 매핑 가능 여부. */

		uint32_t reserved3	: 7;
		/* [한국어] 예약(0). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_pmrcap_register) == 4, "Incorrect size");

/* [한국어] === PMR Control (PMRCTL) — offset 0xE04, 4B RW === */
union spdk_nvme_pmrctl_register {
	uint32_t	raw;
	struct {
		/** enable */
		uint32_t en		: 1;
		/* [한국어] PMRCTL.EN — 1로 토글하면 PMR 활성화 시작. PMRSTS.NRDY=0 폴링 대기. */

		uint32_t reserved	: 31;
		/* [한국어] 예약(0). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_pmrctl_register) == 4, "Incorrect size");

/* [한국어] === PMR Status (PMRSTS) — offset 0xE08, 4B RO === */
union spdk_nvme_pmrsts_register {
	uint32_t	raw;
	struct {
		/** err */
		uint32_t err		: 8;
		/* [한국어] PMRSTS.ERR — PMR 진입 시 에러 코드. */

		/** not ready */
		uint32_t nrdy		: 1;
		/* [한국어] PMRSTS.NRDY — 1이면 PMR 미준비. PMRCTL.EN=1 후 timeout 동안 0 폴링. */

		/**
		 * health status
		 * 000b: Normal Operation
		 * 001b: Restore Error
		 * 010b: Read Only
		 * 011b: Unreliable
		 */
		uint32_t hsts		: 3;
		/* [한국어] PMRSTS.HSTS — 0=Normal, 1=Restore Error(전원 복구 실패),
		 * 2=Read Only(쓰기 불가), 3=Unreliable(데이터 신뢰 불가). */

		/** controller base address invalid */
		uint32_t cbai		: 1;
		/* [한국어] PMRSTS.CBAI — PMRMSCL/PMRMSCU CBA 잘못 설정 시 1. */

		uint32_t reserved	: 19;
		/* [한국어] 예약(0). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_pmrsts_register) == 4, "Incorrect size");

/* [한국어] === PMR Elasticity Buffer Size (PMREBS) — offset 0xE0C, 4B RO === */
union spdk_nvme_pmrebs_register {
	uint32_t	raw;
	struct {
		/**
		 * pmr elasticity buffer size units
		 * 0h: Bytes
		 * 1h: 1 KiB
		 * 2h: 1 MiB
		 * 3h: 1 GiB
		 */
		uint32_t pmrszu		: 4;
		/* [한국어] PMREBS.PMRSZU — 단위(0=B, 1=KiB, 2=MiB, 3=GiB). */

		/** read bypass behavior */
		uint32_t rbb		: 1;
		/* [한국어] PMREBS.RBB — read가 elasticity buffer를 우회하는지 여부. */

		uint32_t reserved	: 3;
		/* [한국어] 예약(0). */

		/** pmr elasticity buffer size base */
		uint32_t pmrwbz		: 24;
		/* [한국어] PMREBS.PMRWBZ — 실제 크기 = pmrwbz * (pmrszu 단위). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_pmrebs_register) == 4, "Incorrect size");

/* [한국어] === PMR Sustained Write Throughput (PMRSWTP) — offset 0xE10, 4B RO === */
union spdk_nvme_pmrswtp_register {
	uint32_t	raw;
	struct {
		/**
		 * pmr sustained write throughput units
		 * 0h: Bytes per second
		 * 1h: 1 KiB / s
		 * 2h: 1 MiB / s
		 * 3h: 1 GiB / s
		 */
		uint32_t pmrswtu	: 4;
		/* [한국어] PMRSWTP.PMRSWTU — 단위(0=B/s, 1=KiB/s, 2=MiB/s, 3=GiB/s). */

		uint32_t reserved	: 4;
		/* [한국어] 예약(0). */

		/** pmr sustained write throughput */
		uint32_t pmrswtv	: 24;
		/* [한국어] PMRSWTP.PMRSWTV — 처리량 = pmrswtv * (pmrswtu 단위). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_pmrswtp_register) == 4, "Incorrect size");

/* [한국어] === PMR Memory Space Control Lower (PMRMSCL) — offset 0xE14, 4B RW === */
union spdk_nvme_pmrmscl_register {
	uint32_t	raw;
	struct {
		uint32_t reserved1	: 1;
		/* [한국어] 예약(0). */

		/** controller memory space enable */
		uint32_t cmse		: 1;
		/* [한국어] PMRMSCL.CMSE — controller memory space enable(1이면 PMRMSCU/L의 CBA로
		 * PCIe target 주소 노출). */

		uint32_t reserved2	: 10;
		/* [한국어] 예약(0). */

		/** controller base address */
		uint32_t cba		: 20;
		/* [한국어] PMRMSCL.CBA — controller base address 하위 32비트(PMRMSCU가 상위).
		 * 실제 base = ((PMRMSCU<<32) | (cba<<12)). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_pmrmscl_register) == 4, "Incorrect size");

/* [한국어] === Boot Partition Information (BPINFO) — offset 0x40, 4B RO ===
 * NVMe Base 1.4 §3.7. UEFI 펌웨어가 OS 부팅 전에 NVMe로부터 boot image를 읽기 위한 기능.
 * CAP.BPS=1일 때만 유효. */
/** Boot partition information */
union spdk_nvme_bpinfo_register	{
	uint32_t	raw;
	struct {
		/** Boot partition size in 128KB multiples */
		uint32_t bpsz		: 15;
		/* [한국어] BPINFO.BPSZ — boot partition 크기 = bpsz * 128KB. 0이면 BP 미지원. */

		uint32_t reserved1	: 9;
		/* [한국어] 예약(0). */

		/**
		 * Boot read status
		 * 00b: No Boot Partition read operation requested
		 * 01b: Boot Partition read in progress
		 * 10b: Boot Partition read completed successfully
		 * 11b: Error completing Boot Partition read
		 */
		uint32_t brs		: 2;
		/* [한국어] BPINFO.BRS — boot read 진행 상태. enum spdk_nvme_brs_value 참조. */

		uint32_t reserved2	: 5;
		/* [한국어] 예약(0). */

		/** Active Boot Partition ID */
		uint32_t abpid		: 1;
		/* [한국어] BPINFO.ABPID — 현재 활성 부팅 파티션 ID(0 또는 1, A/B 더블 버퍼). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_bpinfo_register) == 4, "Incorrect size");

/** Boot read status values */
/* [한국어] === Boot Read Status (BPINFO.BRS) === NVMe Base 1.4 §3.7. */
enum spdk_nvme_brs_value {
	SPDK_NVME_BRS_NO_READ		= 0x0,
	/* [한국어] 0x0 — 아직 boot read 요청 없음. */
	SPDK_NVME_BRS_READ_IN_PROGRESS	= 0x1,
	/* [한국어] 0x1 — boot read 진행 중. 호스트는 폴링 대기. */
	SPDK_NVME_BRS_READ_SUCCESS	= 0x2,
	/* [한국어] 0x2 — boot read 성공. BPMBL이 가리키는 호스트 메모리에 데이터 도착. */
	SPDK_NVME_BRS_READ_ERROR	= 0x3,
	/* [한국어] 0x3 — boot read 실패(부팅 이미지 손상/통신 오류). */
};

/* [한국어] === Boot Partition Read Select (BPRSEL) — offset 0x44, 4B RW ===
 * BPID, 읽을 오프셋과 크기를 설정 후 컨트롤러가 BPMBL이 가리키는 메모리로 DMA. */
/** Boot partition read select */
union spdk_nvme_bprsel_register {
	uint32_t	raw;
	struct {
		/** Boot partition read size in multiples of 4KB */
		uint32_t bprsz		: 10;
		/* [한국어] BPRSEL.BPRSZ — read 크기 = bprsz * 4KB. */

		/** Boot partition read offset in multiples of 4KB */
		uint32_t bprof		: 20;
		/* [한국어] BPRSEL.BPROF — boot partition 내 시작 오프셋 = bprof * 4KB. */

		uint32_t reserved	: 1;
		/* [한국어] 예약(0). */

		/** Boot Partition Identifier */
		uint32_t bpid		: 1;
		/* [한국어] BPRSEL.BPID — 어느 BP를 읽을지(0=BP0, 1=BP1, A/B 더블 버퍼). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_bprsel_register) == 4, "Incorrect size");

/** Value to write to NSSR to indicate a NVM subsystem reset ("NVMe") */
/* [한국어] NSSR 매직 값 — 'NVMe' ASCII = 0x4E 0x56 0x4D 0x65. NSSR(offset 0x20)에 이 값을
 * write하면 CAP.NSSRS=1인 컨트롤러가 NVM subsystem 전체 리셋 수행. 다른 값은 무시.
 * lib/nvme/nvme_ctrlr.c::nvme_ctrlr_subsystem_reset()이 사용. */
#define SPDK_NVME_NSSR_VALUE	0x4E564D65

/* [한국어] === Controller Registers (BAR0) — 메모리 매핑 레지스터 전체 레이아웃 ===
 * NVMe Base 2.0 §3.1.3. 컨트롤러의 모든 제어 레지스터가 PCIe BAR0에 메모리 매핑되어 있고
 * lib/nvme/nvme_pcie.c::nvme_pcie_ctrlr_attach()가 이 영역을 mmap하여 struct
 * spdk_nvme_registers 포인터로 캐스트한다. 이후 모든 레지스터 read/write는
 * spdk_mmio_read_4/8(), spdk_mmio_write_4/8()로 수행 — 이는 일반 load/store가 아니라
 * 컴파일러 reorder를 막고 strong ordering을 보장하는 mmio 전용 매크로이다.
 * 메모리 레이아웃은 NVMe 스펙이 고정한 오프셋과 정확히 일치해야 하므로 이 구조체 끝에
 * SPDK_STATIC_ASSERT(offsetof) 검증이 줄지어 있다(아래 참조).
 *
 * 주요 오프셋:
 *   0x00 CAP, 0x08 VS, 0x0C INTMS, 0x10 INTMC, 0x14 CC, 0x1C CSTS, 0x20 NSSR,
 *   0x24 AQA, 0x28 ASQ, 0x30 ACQ, 0x38 CMBLOC, 0x3C CMBSZ, 0x40 BPINFO,
 *   0x44 BPRSEL, 0x48 BPMBL, 0x50 CMBMSC, 0x58 CMBSTS, 0x5C CMBEBS, 0x60 CMBSWTP,
 *   0x64 NSSD, 0x68 CRTO, 0xE00 PMRCAP, 0xE04 PMRCTL, 0xE08 PMRSTS, 0xE0C PMREBS,
 *   0xE10 PMRSWTP, 0xE14 PMRMSCL, 0xE18 PMRMSCU, 0x1000+ Doorbells.
 * Doorbell은 별도의 동적 stride(CAP.DSTRD)로 계산되므로 본 struct 마지막에 배치된다. */
struct spdk_nvme_registers {
	/** controller capabilities */
	union spdk_nvme_cap_register	cap;
	/* [한국어] CAP @ 0x00 — 8B RO. 부팅 시 가장 먼저 읽히는 capability bitmap. */

	/** version of NVMe specification */
	union spdk_nvme_vs_register	vs;
	/* [한국어] VS @ 0x08 — 4B RO. NVMe 스펙 버전(major.minor.tertiary). */
	uint32_t			intms; /* interrupt mask set */
	/* [한국어] INTMS @ 0x0C — 4B RW1S. bit n에 1을 쓰면 vector n 인터럽트 마스크. SPDK는
	 * polled-mode이므로 일반적으로 사용하지 않으나, MSI-X 인터럽트 모드 폴백 시 사용. */
	uint32_t			intmc; /* interrupt mask clear */
	/* [한국어] INTMC @ 0x10 — 4B RW1C. bit n에 1을 쓰면 마스크 해제. */

	/** controller configuration */
	union spdk_nvme_cc_register	cc;
	/* [한국어] CC @ 0x14 — 4B RW. 호스트가 컨트롤러 동작을 제어하는 메인 레지스터. */

	uint32_t			reserved1;
	/* [한국어] 예약 @ 0x18 — 4B. 미래 확장 슬롯. */
	union spdk_nvme_csts_register	csts; /* controller status */
	/* [한국어] CSTS @ 0x1C — 4B RO. CC 변경의 결과(RDY/CFS/SHST 등) 폴링 대상. */
	uint32_t			nssr; /* NVM subsystem reset */
	/* [한국어] NSSR @ 0x20 — 4B WO. SPDK_NVME_NSSR_VALUE 쓰면 NVM subsystem reset. */

	/** admin queue attributes */
	union spdk_nvme_aqa_register	aqa;
	/* [한국어] AQA @ 0x24 — 4B RW. Admin SQ/CQ 깊이 설정. */

	uint64_t			asq; /* admin submission queue base addr */
	/* [한국어] ASQ @ 0x28 — 8B RW. Admin SQ의 PCIe target address(보통 호스트 hugepage
	 * 물리 주소). lib/nvme/nvme_pcie_common.c가 hugepage SQ 링 할당 후 이 값에 기록. */
	uint64_t			acq; /* admin completion queue base addr */
	/* [한국어] ACQ @ 0x30 — 8B RW. Admin CQ의 PCIe target address. */
	/** controller memory buffer location */
	union spdk_nvme_cmbloc_register	cmbloc;
	/* [한국어] CMBLOC @ 0x38 — 4B RO. CAP.CMBS=1일 때만 유효. */
	/** controller memory buffer size */
	union spdk_nvme_cmbsz_register	cmbsz;
	/* [한국어] CMBSZ @ 0x3C — 4B RO. CMB 크기와 지원되는 데이터 종류. */

	/** boot partition information */
	union spdk_nvme_bpinfo_register	bpinfo;

	/** boot partition read select */
	union spdk_nvme_bprsel_register	bprsel;

	/** boot partition memory buffer location (must be 4KB aligned) */
	uint64_t			bpmbl;

	/** controller memory buffer memory space control */
	union spdk_nvme_cmbmsc_register	cmbmsc;

	/** controller memory buffer status */
	union spdk_nvme_cmbsts_register	cmbsts;
	/* [한국어] CMBSTS @ 0x58 — 4B RO. CMB CBA 유효성 등 상태. */

	/** controller memory buffer elasticity buffer size */
	union spdk_nvme_cmbebs_register	cmbebs;
	/* [한국어] CMBEBS @ 0x5C — 4B RO. */

	/** controller memory buffer sustained write throughput */
	union spdk_nvme_cmbswtp_register cmbswtp;
	/* [한국어] CMBSWTP @ 0x60 — 4B RO. */

	/** NVM subsystem shutdown */
	uint32_t			nssd;
	/* [한국어] NSSD @ 0x64 — 4B WO (NVMe 2.0). 서브시스템 전체 graceful shutdown 명령. */

	/** controller ready timeouts */
	union spdk_nvme_crto_register	crto;
	/* [한국어] CRTO @ 0x68 — 4B RO (NVMe 2.0). CAP.TO 16비트 확장. */

	uint32_t			reserved2[0x365];
	/* [한국어] 예약 영역 @ 0x6C ~ 0xDFC. 미래 확장 영역. 0x365 dword = 0xD94 bytes. */

	/** persistent memory region capabilities */
	union spdk_nvme_pmrcap_register	pmrcap;
	/* [한국어] PMRCAP @ 0xE00 — 4B RO. PMR 영역의 capability bitmap. */

	/** persistent memory region control */
	union spdk_nvme_pmrctl_register	pmrctl;
	/* [한국어] PMRCTL @ 0xE04 — 4B RW. PMR enable/disable. */

	/** persistent memory region status */
	union spdk_nvme_pmrsts_register	pmrsts;
	/* [한국어] PMRSTS @ 0xE08 — 4B RO. PMR ready/health 상태. */

	/** persistent memory region elasticity buffer size */
	union spdk_nvme_pmrebs_register	pmrebs;
	/* [한국어] PMREBS @ 0xE0C — 4B RO. */

	/** persistent memory region sustained write throughput */
	union spdk_nvme_pmrswtp_register	pmrswtp;
	/* [한국어] PMRSWTP @ 0xE10 — 4B RO. */

	/** persistent memory region memory space control lower */
	union spdk_nvme_pmrmscl_register	pmrmscl;
	/* [한국어] PMRMSCL @ 0xE14 — 4B RW. PMR base address 하위 32비트. */

	uint32_t			pmrmscu; /* persistent memory region memory space control upper */
	/* [한국어] PMRMSCU @ 0xE18 — 4B RW. PMR base address 상위 32비트. */

	uint32_t			reserved3[0x79];
	/* [한국어] 예약 영역 @ 0xE1C ~ 0xFFC. 0x79 dword = 0x1E4 bytes. 0x1000부터 doorbell. */

	/* [한국어] === Doorbell 영역 @ 0x1000 부터 ===
	 * Doorbell은 호스트가 SQ에 새 SQE를 enqueue 한 후 컨트롤러에게 "처리하라"고 알리는
	 * 가장 hot한 핵심 메모리 매핑이다. CAP.DSTRD에 따라 stride가 4B/8B/.../64B로 가변이며,
	 * SQ_n_TDBL과 CQ_n_HDBL이 페어로 배치된다 (n=0이 admin, 1~N이 I/O queue). 본 구조체는
	 * 1쌍만 정적 할당해 두고, 실제 접근은 (uintptr_t)doorbell + n*(2*stride) 형태로
	 * 동적으로 인덱싱한다(lib/nvme/nvme_pcie.c::nvme_pcie_qpair_construct).
	 * SPDK polled-mode I/O 핫패스: SQE 작성 → smp_wmb → spdk_mmio_write_4(sq_tdbl, new_tail). */
	struct {
		uint32_t	sq_tdbl;	/* submission queue tail doorbell */
		/* [한국어] SQ tail doorbell — 호스트가 새 SQE 위치(tail+1)를 컨트롤러에 알림.
		 * 새 명령 발행 시 한 번 write되며, 이게 NVMe I/O 경로의 가장 비싼 단일 동작
		 * (PCIe MMIO write, ~수백 ns). SPDK는 batching으로 doorbell write를 줄여 throughput 향상. */
		uint32_t	cq_hdbl;	/* completion queue head doorbell */
		/* [한국어] CQ head doorbell — 호스트가 CQE를 처리한 위치(head+1)를 컨트롤러에 알림.
		 * spdk_nvme_qpair_process_completions()가 N개의 CQE를 일괄 수확한 뒤 한 번만 write. */
	} doorbell[1];
	/* [한국어] flexible array(N=1) — 실제 doorbell 개수는 (1+I/O 큐 수). 인덱싱 시
	 * sizeof(doorbell[0]) 대신 (4 << CAP.DSTRD)*2를 stride로 사용해야 한다. */
};

/* NVMe controller register space offsets */
SPDK_STATIC_ASSERT(0x00 == offsetof(struct spdk_nvme_registers, cap),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0x08 == offsetof(struct spdk_nvme_registers, vs), "Incorrect register offset");
SPDK_STATIC_ASSERT(0x0C == offsetof(struct spdk_nvme_registers, intms),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0x10 == offsetof(struct spdk_nvme_registers, intmc),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0x14 == offsetof(struct spdk_nvme_registers, cc), "Incorrect register offset");
SPDK_STATIC_ASSERT(0x1C == offsetof(struct spdk_nvme_registers, csts), "Incorrect register offset");
SPDK_STATIC_ASSERT(0x20 == offsetof(struct spdk_nvme_registers, nssr), "Incorrect register offset");
SPDK_STATIC_ASSERT(0x24 == offsetof(struct spdk_nvme_registers, aqa), "Incorrect register offset");
SPDK_STATIC_ASSERT(0x28 == offsetof(struct spdk_nvme_registers, asq), "Incorrect register offset");
SPDK_STATIC_ASSERT(0x30 == offsetof(struct spdk_nvme_registers, acq), "Incorrect register offset");
SPDK_STATIC_ASSERT(0x38 == offsetof(struct spdk_nvme_registers, cmbloc),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0x3C == offsetof(struct spdk_nvme_registers, cmbsz),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0x40 == offsetof(struct spdk_nvme_registers, bpinfo),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0x44 == offsetof(struct spdk_nvme_registers, bprsel),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0x48 == offsetof(struct spdk_nvme_registers, bpmbl),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0x50 == offsetof(struct spdk_nvme_registers, cmbmsc),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0x58 == offsetof(struct spdk_nvme_registers, cmbsts),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0x5C == offsetof(struct spdk_nvme_registers, cmbebs),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0x60 == offsetof(struct spdk_nvme_registers, cmbswtp),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0x64 == offsetof(struct spdk_nvme_registers, nssd),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0x68 == offsetof(struct spdk_nvme_registers, crto),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0xE00 == offsetof(struct spdk_nvme_registers, pmrcap),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0xE04 == offsetof(struct spdk_nvme_registers, pmrctl),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0xE08 == offsetof(struct spdk_nvme_registers, pmrsts),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0xE0C == offsetof(struct spdk_nvme_registers, pmrebs),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0xE10 == offsetof(struct spdk_nvme_registers, pmrswtp),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0xE14 == offsetof(struct spdk_nvme_registers, pmrmscl),
		   "Incorrect register offset");
SPDK_STATIC_ASSERT(0xE18 == offsetof(struct spdk_nvme_registers, pmrmscu),
		   "Incorrect register offset");

/* [한국어] ===== SGL (Scatter/Gather List) 디스크립터 =====
 *  - NVMe 1.1+ 선택적 기능, NVMe-oF에서는 필수
 *  - PRP와 달리 임의 오프셋·임의 길이의 메모리 조각을 자유롭게 연결 가능
 *  - 각 디스크립터는 16B, hot-path에서 자주 등장 */
enum spdk_nvme_sgl_descriptor_type {
	SPDK_NVME_SGL_TYPE_DATA_BLOCK		= 0x0,
                                  /* [한국어] 일반 데이터 블록 (호스트 메모리 내 버퍼) */
	SPDK_NVME_SGL_TYPE_BIT_BUCKET		= 0x1,
                                  /* [한국어] "비트 버킷" — read 시 특정 구간 폐기 (데이터 필터링) */
	SPDK_NVME_SGL_TYPE_SEGMENT		= 0x2,
                                  /* [한국어] 다음 SGL 세그먼트 포인터 (체이닝). 뒤에 계속 디스크립터가 따라옴 */
	SPDK_NVME_SGL_TYPE_LAST_SEGMENT		= 0x3,
                                  /* [한국어] 마지막 세그먼트 포인터 — 이후 추가 체이닝 없음 */
	SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK	= 0x4,
                                  /* [한국어] 키(rkey) 포함 데이터 블록 — RDMA 트랜스포트에서 원격 메모리 접근용 */
	SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK	= 0x5,
                                  /* [한국어] 트랜스포트 특정 데이터 블록 (TCP SGL in-capsule 등) */
	/* 0x6 - 0xE reserved */
	SPDK_NVME_SGL_TYPE_VENDOR_SPECIFIC	= 0xF
                                  /* [한국어] 벤더 확장용 */
};

enum spdk_nvme_sgl_descriptor_subtype {
	SPDK_NVME_SGL_SUBTYPE_ADDRESS		= 0x0,
                                  /* [한국어] 주소 기반 — address 필드가 버스 주소 */
	SPDK_NVME_SGL_SUBTYPE_OFFSET		= 0x1,
                                  /* [한국어] 오프셋 기반 — 기준 주소에 대한 상대 오프셋 */
	SPDK_NVME_SGL_SUBTYPE_TRANSPORT		= 0xa,
                                  /* [한국어] 트랜스포트 특정 의미 — 예: NVMe/TCP의 in-capsule data */
};

#pragma pack(push, 1)
                                  /* [한국어] 1바이트 정렬 강제 — 스펙상 16B 정확히 맞추기 위해 패딩 금지 */
struct spdk_nvme_sgl_descriptor {
	uint64_t address;
                                  /* [한국어] 버스/오프셋/키 중 하나로 해석되는 64비트 필드
                                   *  - subtype에 따라 의미 변경
                                   *  - SUBTYPE_ADDRESS: IOMMU 버스 주소
                                   *  - SUBTYPE_OFFSET: 기준 주소로부터의 오프셋
                                   *  - keyed 디스크립터에서는 하위 32비트가 rkey로 해석되는 변형도 있음 */
	union {
		struct {
			uint8_t reserved[7];
			uint8_t subtype	: 4;
                                  /* [한국어] 하위 4비트: subtype (enum spdk_nvme_sgl_descriptor_subtype) */
			uint8_t type	: 4;
                                  /* [한국어] 상위 4비트: type (enum spdk_nvme_sgl_descriptor_type)
                                   *  - generic 뷰: type/subtype만 빠르게 확인할 때 사용 */
		} generic;

		struct {
			uint32_t length;
                                  /* [한국어] 이 세그먼트의 데이터 길이 (바이트).
                                   *  - DATA_BLOCK에서는 블록 크기
                                   *  - SEGMENT/LAST_SEGMENT에서는 다음 세그먼트 내 디스크립터 배열 크기 */
			uint8_t reserved[3];
			uint8_t subtype	: 4;
			uint8_t type	: 4;
		} unkeyed;
                                  /* [한국어] keyed가 아닌 일반 경우 — PCIe 트랜스포트에서 사용 */

		struct {
			uint64_t length		: 24;
                                  /* [한국어] 24비트 길이 (최대 16MB 세그먼트) */
			uint64_t key		: 32;
                                  /* [한국어] 32비트 키 — RDMA rkey. 원격 메모리 영역 식별 */
			uint64_t subtype	: 4;
			uint64_t type		: 4;
		} keyed;
                                  /* [한국어] RDMA 트랜스포트용 — 원격 호스트가 보낸 rkey/주소로 DMA 가능한 영역 지정 */
	};
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_sgl_descriptor) == 16, "Incorrect size");
                                  /* [한국어] 정확히 16B — 스펙 SGL 디스크립터 크기 */
#pragma pack(pop)

enum spdk_nvme_psdt_value {
                                  /* [한국어] PSDT (PRP or SGL for Data Transfer) — SQE의 psdt 필드에 저장
                                   *  - 데이터/메타데이터 포인터를 PRP로 쓸지 SGL로 쓸지 지정 */
	SPDK_NVME_PSDT_PRP		= 0x0,
                                  /* [한국어] 데이터·메타 모두 PRP 사용 (PCIe 기본) */
	SPDK_NVME_PSDT_SGL_MPTR_CONTIG	= 0x1,
                                  /* [한국어] 데이터는 SGL, 메타는 연속 버퍼 포인터(mptr) */
	SPDK_NVME_PSDT_SGL_MPTR_SGL	= 0x2,
                                  /* [한국어] 데이터·메타 모두 SGL (메타는 data SGL 바로 앞 인접 SGL) */
	SPDK_NVME_PSDT_RESERVED		= 0x3
};

/**
 * Submission queue priority values for Create I/O Submission Queue Command.
 *
 * Only valid for weighted round robin arbitration method.
 */
enum spdk_nvme_qprio {
	SPDK_NVME_QPRIO_URGENT		= 0x0,
	SPDK_NVME_QPRIO_HIGH		= 0x1,
	SPDK_NVME_QPRIO_MEDIUM		= 0x2,
	SPDK_NVME_QPRIO_LOW		= 0x3
};

#define SPDK_NVME_CREATE_IO_SQ_QPRIO_MASK	0x3

/**
 * Optional Arbitration Mechanism Supported by the controller.
 *
 * Two bits for CAP.AMS (18:17) field are set to '1' when the controller supports.
 * There is no bit for AMS_RR where all controllers support and set to 0x0 by default.
 */
enum spdk_nvme_cap_ams {
	SPDK_NVME_CAP_AMS_WRR		= 0x1,	/**< weighted round robin */
	SPDK_NVME_CAP_AMS_VS		= 0x2,	/**< vendor specific */
};

/**
 * Arbitration Mechanism Selected to the controller.
 *
 * Value 0x2 to 0x6 is reserved.
 */
enum spdk_nvme_cc_ams {
	SPDK_NVME_CC_AMS_RR		= 0x0,	/**< default round robin */
	SPDK_NVME_CC_AMS_WRR		= 0x1,	/**< weighted round robin */
	SPDK_NVME_CC_AMS_VS		= 0x7,	/**< vendor specific */
};

/**
 * Fused Operation
 */
enum spdk_nvme_cmd_fuse {
	SPDK_NVME_CMD_FUSE_NONE		= 0x0,	/**< normal operation */
	SPDK_NVME_CMD_FUSE_FIRST	= 0x1,	/**< fused operation, first command */
	SPDK_NVME_CMD_FUSE_SECOND	= 0x2,	/**< fused operation, second command */
	SPDK_NVME_CMD_FUSE_MASK		= 0x3,  /**< fused operation flags mask */
};

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_ARBITRATION
 */
/* [한국어] === union spdk_nvme_feat_arbitration === Set/Get Features FID=0x01 의 CDW11 인코딩.
 * NVMe Base 2.0 §5.27.1.2. CC.AMS=0x1(WRR with Urgent priority)을 활성화했을 때만 의미가 있다.
 * 컨트롤러는 한 round에서 SQ priority별로 burst 크기와 가중치를 곱한 만큼 명령을 fetch한다.
 * 설정자 = 호스트(Set Features)/읽는 자 = 호스트(Get Features) 또는 컨트롤러 내부 arbiter.
 * 동기화 = controller 단위 immutable 동안 변경 불가; 변경 시 in-flight 명령에 즉시 영향. */
union spdk_nvme_feat_arbitration {
	uint32_t raw;                              /* [한국어] 32비트 raw 뷰 — CDW11에 그대로 기재 가능. */
	struct {
		/** Arbitration Burst */
		uint32_t ab : 3;
		/* [한국어] [2:0] AB(Arbitration Burst) — 한 라운드에서 fetch할 최대 명령 수 = 2^AB.
		 * AB=7(SPDK_NVME_ARBITRATION_BURST_UNLIMITED)이면 무제한. 보통 AB=0(=1) ~ 6(=64). */

		uint32_t reserved : 5;
		/* [한국어] [7:3] reserved — NVMe 스펙 정의 영역 외. 0으로 기재. */

		/** Low Priority Weight */
		uint32_t lpw : 8;
		/* [한국어] [15:8] LPW — Low Priority SQ에 한 라운드에 부여할 가중치(0=1, 255=256).
		 * Urgent → High → Medium → Low 순으로 fetch. */

		/** Medium Priority Weight */
		uint32_t mpw : 8;
		/* [한국어] [23:16] MPW — Medium Priority 가중치. */

		/** High Priority Weight */
		uint32_t hpw : 8;
		/* [한국어] [31:24] HPW — High Priority 가중치. Urgent SQ는 가중치 없이 우선 처리. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_arbitration) == 4, "Incorrect size");

#define SPDK_NVME_ARBITRATION_BURST_UNLIMITED	0x7
/* [한국어] AB=0x7 — 한 round의 burst 무제한. CC.AMS=WRR + Urgent SQ가 없을 때 RR과 동등. */

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_POWER_MANAGEMENT
 */
/* [한국어] === union spdk_nvme_feat_power_management === FID=0x02 의 CDW11 인코딩.
 * NVMe Base 2.0 §5.27.1.3. 컨트롤러의 현재 active power state(PS)를 RW로 설정.
 * 설정자 = 호스트 / 읽는 자 = 컨트롤러 power manager.
 * 값 범위 = PS는 [0..NPSS](Identify Controller npss). 동기화 = 변경 시 즉시 새 PS로 전이; entry/exit
 * latency는 power_state.enlat/exlat로 사전 측정. */
union spdk_nvme_feat_power_management {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Power State */
		uint32_t ps : 5;
		/* [한국어] [4:0] PS — Power State 인덱스 0~31. struct spdk_nvme_power_state psd[32]
		 * 의 인덱스. 낮은 인덱스 = 고성능/고전력, 높은 인덱스 = 저전력/저성능. */

		/** Workload Hint */
		uint32_t wh : 3;
		/* [한국어] [7:5] WH(Workload Hint) — 0=No workload, 1=Extended idle, 2=Heavy I/O.
		 * 컨트롤러가 latency vs power 트레이드오프 조정에 사용. */

		uint32_t reserved : 24;
		/* [한국어] [31:8] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_power_management) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_LBA_RANGE_TYPE
 */
/* [한국어] === union spdk_nvme_feat_lba_range_type === FID=0x03 의 CDW11.
 * NVMe Base 2.0 §5.27.1.4. NS 내부를 type별 partition(file system data/RAID/cache/...)으로 분류.
 * CDW11에 num만 기재; 실제 range 데이터(64B*num)는 별도 PRP/SGL 페이로드로 H2C 전달. */
union spdk_nvme_feat_lba_range_type {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Number of LBA Ranges */
		uint32_t num : 6;
		/* [한국어] [5:0] NUM — 데이터 페이로드에 포함된 LBA range 개수 - 1 (0-based, 최대 64). */

		uint32_t reserved : 26;
		/* [한국어] [31:6] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_lba_range_type) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD
 */
/* [한국어] === union spdk_nvme_feat_temperature_threshold === FID=0x04 의 CDW11.
 * NVMe Base 2.0 §5.27.1.5. 컨트롤러/센서별 over/under temperature threshold 설정. 임계 도달 시
 * AER(type=SMART, info=TEMPERATURE_THRESHOLD)로 호스트 통지. 8개 센서(temp_sensor[0..7])와
 * composite(=0)에 대해 각각 over/under 두 종류씩 = 18개 임계를 별개로 set 가능. */
union spdk_nvme_feat_temperature_threshold {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Temperature Threshold */
		uint32_t tmpth : 16;
		/* [한국어] [15:0] TMPTH — Threshold 온도 (Kelvin). NVMe는 모든 온도를 K 단위로 표기.
		 * 273=0°C, 373=100°C 등. */

		/** Threshold Temperature Select */
		uint32_t tmpsel : 4;
		/* [한국어] [19:16] TMPSEL — 어느 센서의 임계인지: 0=composite, 1~8=temp_sensor[0..7],
		 * 0xF=ALL (모든 센서에 동일 임계 적용). */

		/** Threshold Type Select */
		uint32_t thsel : 2;
		/* [한국어] [21:20] THSEL — 0=Over Temperature(이 값을 초과하면 AER), 1=Under
		 * Temperature(미만이면 AER). */

		uint32_t reserved : 10;
		/* [한국어] [31:22] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_temperature_threshold) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_ERROR_RECOVERY
 */
/* [한국어] === union spdk_nvme_feat_error_recovery === FID=0x05 의 CDW11.
 * NVMe Base 2.0 §5.27.1.6. 미디어 read 에러 시 컨트롤러가 재시도에 쓰는 시간 한도와
 * deallocated/unwritten LBA에 대한 read 에러 보고 활성화 여부를 제어. */
union spdk_nvme_feat_error_recovery {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Time Limited Error Recovery */
		uint32_t tler : 16;
		/* [한국어] [15:0] TLER — 100ms 단위 재시도 타임아웃. 0 = no limit (가능한 모든 ECC/
		 * read retry 사용 → 응답 지연↑). 짧을수록 빠른 실패가 가능해 RT/RAID 쓰기 우선 환경에 유리. */

		/** Deallocated or Unwritten Logical Block Error Enable */
		uint32_t dulbe : 1;
		/* [한국어] [16] DULBE — deallocated/unwritten LBA에 대한 read 시 에러 반환 여부.
		 * 1이면 SC=DEALLOCATED_OR_UNWRITTEN_BLOCK(media error 0x87) 반환. NS data
		 * dlfeat.read_value가 0x00/0xFF로 정의됐을 때만 의미. */

		uint32_t reserved : 15;
		/* [한국어] [31:17] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_error_recovery) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE
 */
/* [한국어] === union spdk_nvme_feat_volatile_write_cache === FID=0x06 의 CDW11.
 * NVMe Base 2.0 §5.27.1.7. 컨트롤러의 휘발성 write cache 활성/비활성. 끄면 모든 write가
 * media까지 도달한 뒤에만 완료(latency↑/안전↑). bdev_nvme의 'enable_write_cache'와 1:1 매핑.
 * 컨트롤러가 vwc.present=1일 때만 이 FID 의미가 있다(존재 여부 = Identify ctrlr.vwc 참조). */
union spdk_nvme_feat_volatile_write_cache {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Volatile Write Cache Enable */
		uint32_t wce : 1;
		/* [한국어] [0] WCE — 1이면 휘발성 write cache 활성. Flush(opcode 0x00)으로 강제
		 * sync 가능. 0이면 모든 write가 implicit FUA처럼 동작. */

		uint32_t reserved : 31;
		/* [한국어] [31:1] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_volatile_write_cache) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_NUMBER_OF_QUEUES
 */
/* [한국어] === union spdk_nvme_feat_number_of_queues === FID=0x07 의 CDW11.
 * NVMe Base 2.0 §5.27.1.8. 호스트가 원하는 I/O SQ/CQ 개수를 컨트롤러에 통지; 컨트롤러는
 * CQE.CDW0에 실제 할당량을 회신(요청보다 적을 수 있음). lib/nvme/nvme_ctrlr.c 가 controller
 * init 시 한 번 발행해 max I/O qpair 수 결정. CC.EN=1 이후에는 변경 불가. */
union spdk_nvme_feat_number_of_queues {
	uint32_t raw;                              /* [한국어] CDW11 raw. CQE 회신도 동일 인코딩. */
	struct {
		/** Number of I/O Submission Queues Requested */
		uint32_t nsqr : 16;
		/* [한국어] [15:0] NSQR — 요청 SQ 수 - 1 (0-based). 예: 0=1개, 0xFFFF=65536개. */

		/** Number of I/O Completion Queues Requested */
		uint32_t ncqr : 16;
		/* [한국어] [31:16] NCQR — 요청 CQ 수 - 1. 보통 NSQR=NCQR (1:1 mapping). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_number_of_queues) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_INTERRUPT_COALESCING
 */
/* [한국어] === union spdk_nvme_feat_interrupt_coalescing === FID=0x08 의 CDW11.
 * NVMe Base 2.0 §5.27.1.9. CQ 인터럽트 합치기 — N개의 CQE가 쌓이거나 시간이 경과하면 한 번에
 * 인터럽트 발생. polled-mode SPDK는 인터럽트를 사용하지 않으므로 이 설정의 영향 없음. */
union spdk_nvme_feat_interrupt_coalescing {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Aggregation Threshold */
		uint32_t thr : 8;
		/* [한국어] [7:0] THR — 누적 CQE 수 - 1 (0=1개, 255=256개). */

		/** Aggregation time */
		uint32_t time : 8;
		/* [한국어] [15:8] TIME — 100us 단위 타임아웃. 0 = 시간 기반 합치기 비활성. */

		uint32_t reserved : 16;
		/* [한국어] [31:16] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_interrupt_coalescing) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION
 */
/* [한국어] === union spdk_nvme_feat_interrupt_vector_configuration === FID=0x09 의 CDW11.
 * NVMe Base 2.0 §5.27.1.10. 특정 MSI-X/single-MSI 인터럽트 벡터에 대해 coalescing 사용 여부 토글.
 * polled-mode에서는 사용 X. 인터럽트 모드 드라이버에서만 의미. */
union spdk_nvme_feat_interrupt_vector_configuration {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Interrupt Vector */
		uint32_t iv : 16;
		/* [한국어] [15:0] IV — 대상 MSI-X 벡터 인덱스(0=admin CQ, 1+=I/O CQ). */

		/** Coalescing Disable */
		uint32_t cd : 1;
		/* [한국어] [16] CD — 1이면 이 벡터에 대해 coalescing 비활성(즉시 인터럽트). */

		uint32_t reserved : 15;
		/* [한국어] [31:17] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_interrupt_vector_configuration) == 4,
		   "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_WRITE_ATOMICITY
 */
/* [한국어] === union spdk_nvme_feat_write_atomicity === FID=0x0A 의 CDW11.
 * NVMe Base 2.0 §5.27.1.11. AWUN(Atomic Write Unit Normal) 사용 여부. AWUN을 무시하면
 * 컨트롤러는 단일 LBA만 atomic 보장(=1 LBA write). */
union spdk_nvme_feat_write_atomicity {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Disable Normal */
		uint32_t dn : 1;
		/* [한국어] [0] DN — 1이면 AWUN/NAWUN 무시(atomic 보장 단위 = 1 LBA). 0이면
		 * Identify ctrlr.awun 단위로 atomic 보장. */

		uint32_t reserved : 31;
		/* [한국어] [31:1] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_write_atomicity) == 4, "Incorrect size");

/* [한국어] === union spdk_nvme_critical_warning_state === SMART log critical_warning 필드(1B)
 * 와 AER configuration의 crit_warn 필드 양쪽에서 공유. 각 비트가 SET이면 해당 종류의 critical
 * 상태 활성/AER 발생 허용 의미. NVMe Base 2.0 §5.16.1.3 (Health log) / §5.27.1.12 (AER cfg). */
union spdk_nvme_critical_warning_state {
	uint8_t		raw;                       /* [한국어] 8비트 raw — SMART critical_warning 필드와 비트 호환. */

	struct {
		uint8_t	available_spare		: 1;
		/* [한국어] [0] available_spare — 사용 가능한 spare 영역이 임계 미만(NVM 마모 임박). */
		uint8_t	temperature		: 1;
		/* [한국어] [1] temperature — composite/sensor 온도가 over/under 임계 도달. */
		uint8_t	device_reliability	: 1;
		/* [한국어] [2] device_reliability — 미디어 신뢰성 저하(read-only 전환 임박). */
		uint8_t	read_only		: 1;
		/* [한국어] [3] read_only — 컨트롤러가 read-only로 전환됨(쓰기 불가). */
		uint8_t	volatile_memory_backup	: 1;
		/* [한국어] [4] volatile_memory_backup — 휘발성 메모리 백업 장치(슈퍼커패시터 등) 실패. */
		uint8_t	reserved		: 3;
		/* [한국어] [7:5] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_critical_warning_state) == 1, "Incorrect size");

/**
 * Data used by Set Features / Get Features \ref SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION
 */
/* [한국어] === union spdk_nvme_feat_async_event_configuration === FID=0x0B 의 CDW11.
 * NVMe Base 2.0 §5.27.1.12. 어떤 종류의 비동기 이벤트를 컨트롤러가 호스트에 통지(AER 명령
 * 완료를 통해)할지 비트 마스크로 토글. SPDK는 lib/nvme/nvme_ctrlr.c 에서 init 시 SMART/NS attr
 * /FW activation/telemetry/ANA change/discovery 모두 enable 후 AER을 미리 적재. */
union spdk_nvme_feat_async_event_configuration {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		union spdk_nvme_critical_warning_state crit_warn;
		/* [한국어] [7:0] crit_warn — SMART/Health critical 비트 마스크(spare/temperature/
		 * device_reliability/read_only/volatile_memory_backup). 1이면 해당 사유로 AER 허용. */
		uint8_t ns_attr_notice		: 1;
		/* [한국어] [8] NS Attribute Notice — NS create/delete/format 등 변경 시 AER 통지. */
		uint8_t fw_activation_notice	: 1;
		/* [한국어] [9] FW Activation Notice — Firmware Commit 시작 시 AER. */
		uint8_t telemetry_log_notice	: 1;
		/* [한국어] [10] Telemetry Log Notice — controller-initiated telemetry 갱신 시 AER. */
		uint8_t ana_change_notice	: 1;
		/* [한국어] [11] ANA Change Notice — NVMe-oF multipath ANA state 변경 시 AER (LID=0x0C). */
		uint8_t reserved1		: 4;
		/* [한국어] [15:12] reserved. */
		uint16_t reserved2		: 15;
		/* [한국어] [30:16] reserved. */
		/** Discovery log change (refer to the NVMe over Fabrics specification) */
		uint16_t discovery_log_change_notice	: 1;
		/* [한국어] [31] Discovery Log Change — NVMe-oF Discovery 컨트롤러에서 등록 변경 시 AER. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_async_event_configuration) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION
 */
/* [한국어] === union spdk_nvme_feat_autonomous_power_state_transition === FID=0x0C CDW11.
 * NVMe Base 2.0 §5.27.1.13. APST 활성화 시 컨트롤러는 idle time이 임계를 넘으면 자동으로
 * 저전력 PS로 전이. 데이터 페이로드(struct spdk_nvme_apst_table 256B)에 PS별 idle 임계 기재. */
union spdk_nvme_feat_autonomous_power_state_transition {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Autonomous Power State Transition Enable */
		uint32_t apste : 1;
		/* [한국어] [0] APSTE — 1=APST 활성, 0=비활성. Identify ctrlr.apsta.supported=1 필요. */

		uint32_t reserved : 31;
		/* [한국어] [31:1] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_autonomous_power_state_transition) == 4,
		   "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_HOST_MEM_BUFFER
 */
/* [한국어] === union spdk_nvme_feat_host_mem_buffer === FID=0x0D CDW11.
 * NVMe Base 2.0 §5.27.1.14. HMB(Host Memory Buffer) — DRAMless SSD가 호스트 RAM을 빌려
 * FTL 매핑 캐시로 사용. 호스트는 hugepage 영역의 chunked DMA descriptor list(HMB Descriptor
 * Entry)를 데이터 페이로드로 전달. SPDK는 spdk_dma_zmalloc 으로 hugepage 할당 후 EHM=1로 활성. */
union spdk_nvme_feat_host_mem_buffer {
	uint32_t raw;                              /* [한국어] CDW11 raw. CDW12=size in 4KiB units, CDW13/14=descriptor 주소. */
	struct {
		/** Enable Host Memory */
		uint32_t ehm : 1;
		/* [한국어] [0] EHM — 1=HMB 활성, 0=비활성. Identify ctrlr.hmpre>0 일 때만 의미. */

		/** Memory Return */
		uint32_t mr : 1;
		/* [한국어] [1] MR(Memory Return) — 1이면 reset 후 컨트롤러가 이전 HMB 영역의 내용을
		 * 재사용 가능(휘발성 mem이 보존됐다는 호스트 약속). 0=폐기. */

		uint32_t reserved : 30;
		/* [한국어] [31:2] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_host_mem_buffer) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_KEEP_ALIVE_TIMER
 */
/* [한국어] === union spdk_nvme_feat_keep_alive_timer === FID=0x0F CDW11.
 * NVMe Base 2.0 §5.27.1.15. NVMe-oF에서 호스트가 dead한지 컨트롤러가 검출하기 위한 타임아웃.
 * 호스트는 KATO 내에 admin Keep Alive(0x18) 명령을 발행해야 함. SPDK NVMe-oF target은
 * lib/nvmf/ctrlr.c::nvmf_ctrlr_keep_alive_poll 에서 만료 검사. */
union spdk_nvme_feat_keep_alive_timer {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Keep Alive Timeout */
		uint32_t kato : 32;
		/* [한국어] [31:0] KATO — Keep Alive Timeout (ms). 0=비활성. controller가 kas 단위
		 * 의 granularity로 round 처리(Identify ctrlr.kas, 100ms 단위). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_keep_alive_timer) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_HOST_CONTROLLED_THERMAL_MANAGEMENT
 */
/* [한국어] === union spdk_nvme_feat_host_controlled_thermal_management === FID=0x10 CDW11.
 * NVMe Base 2.0 §5.27.1.16. HCTM — 호스트가 컨트롤러에 두 단계 thermal management 임계 지정.
 * Identify ctrlr.hctma.supported=1 일 때만 활성. */
union spdk_nvme_feat_host_controlled_thermal_management {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Thermal Management Temperature 2 */
		uint32_t tmt2 : 16;
		/* [한국어] [15:0] TMT2 — 더 적극적인 thermal throttling 임계(K). TMT1보다 높은 온도. */

		/** Thermal Management Temperature 1 */
		uint32_t tmt1 : 16;
		/* [한국어] [31:16] TMT1 — 가벼운 thermal management 임계(K). TMT1≤TMT2 이어야 함. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_host_controlled_thermal_management) == 4,
		   "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_NON_OPERATIONAL_POWER_STATE_CONFIG
 */
/* [한국어] === union spdk_nvme_feat_non_operational_power_state_config === FID=0x11 CDW11.
 * NVMe Base 2.0 §5.27.1.17. APST가 non-operational PS(I/O 처리 불가, 하지만 RTD3보다 빠른 wake)
 * 까지 활용할지 토글. */
union spdk_nvme_feat_non_operational_power_state_config {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Non-Operational Power State Permissive Mode Enable */
		uint32_t noppme : 1;
		/* [한국어] [0] NOPPME — 1이면 APST가 non-operational PS도 사용 허용. host가
		 * 받아들일 수 있는 wake-up latency 한도 내에서. */

		uint32_t reserved : 31;
		/* [한국어] [31:1] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_non_operational_power_state_config) == 4,
		   "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_SOFTWARE_PROGRESS_MARKER
 */
/* [한국어] === union spdk_nvme_feat_software_progress_marker === FID=0x80 CDW11.
 * NVMe Base 2.0 §5.27.1.18. Pre-boot SW가 OS 부팅 전 자기 자신의 progress count를 기록 →
 * OS는 이 값으로 hang 발생 여부 진단. Set 시 0 이외 값은 무시(컨트롤러 내부 카운터 증가). */
union spdk_nvme_feat_software_progress_marker {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Pre-boot Software Load Count */
		uint32_t pbslc : 8;
		/* [한국어] [7:0] PBSLC — 컨트롤러가 카운트한 pre-boot SW 로드 횟수. Get으로 read,
		 * Set value=0으로 reset (다른 값 무시). */

		uint32_t reserved : 24;
		/* [한국어] [31:8] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_software_progress_marker) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_HOST_IDENTIFIER
 */
/* [한국어] === union spdk_nvme_feat_host_identifier === FID=0x81 CDW11.
 * NVMe Base 2.0 §5.27.1.19. host_id (8B 또는 16B) 등록. reservation/multipath 식별에 필수.
 * 데이터 페이로드(8B/16B)는 별도 PRP/SGL로 H2C 전달. */
union spdk_nvme_feat_host_identifier {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/** Enable Extended Host Identifier */
		uint32_t exhid : 1;
		/* [한국어] [0] EXHID — 1이면 16B Extended Host Identifier 사용, 0이면 8B Host Id. */

		uint32_t reserved : 31;
		/* [한국어] [31:1] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_host_identifier) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_HOST_RESERVE_MASK
 */
/* [한국어] === union spdk_nvme_feat_reservation_notification_mask === FID=0x82 CDW11.
 * NVMe Base 2.0 §8.19.6. NS-별 reservation 관련 AER을 어떤 종류만 받을지 비트 마스크.
 * 1=mask(통지 안 받음), 0=allow(AER 받음). NSID 별로 별도 설정. */
union spdk_nvme_feat_reservation_notification_mask {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		uint32_t reserved1 : 1;
		/* [한국어] [0] reserved. */
		/* Mask Registration Preempted Notification */
		uint32_t regpre    : 1;
		/* [한국어] [1] regpre — 1이면 Registration Preempted 통지 차단. */
		/* Mask Reservation Released Notification */
		uint32_t resrel    : 1;
		/* [한국어] [2] resrel — 1이면 Reservation Released 통지 차단. */
		/* Mask Reservation Preempted Notification */
		uint32_t respre    : 1;
		/* [한국어] [3] respre — 1이면 Reservation Preempted 통지 차단. */
		uint32_t reserved2 : 28;
		/* [한국어] [31:4] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_reservation_notification_mask) == 4,
		   "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_HOST_RESERVE_PERSIST
 */
/* [한국어] === union spdk_nvme_feat_reservation_persistence === FID=0x83 CDW11.
 * NVMe Base 2.0 §8.19.7. NS-별 reservation 정보를 power cycle을 넘어 보존할지 토글.
 * Identify ctrlr.rescap.ptpls=1 일 때만 의미. */
union spdk_nvme_feat_reservation_persistence {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/* Persist Through Power Loss */
		uint32_t ptpl      : 1;
		/* [한국어] [0] PTPL — 1이면 power cycle 후에도 reservation 유지. 0이면 reset 시 모두 초기화. */
		uint32_t reserved  : 31;
		/* [한국어] [31:1] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_reservation_persistence) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_FDP
 */
/* [한국어] === union spdk_nvme_feat_fdp_cdw11 === FID=0x1D CDW11 (FDP enable).
 * NVMe TP4146 (Flexible Data Placement). Endurance Group 단위로 FDP를 활성화. CDW11에 대상
 * Endurance Group ID 지정, CDW12에 enable 여부와 사용할 FDP 구성 인덱스 기재. */
union spdk_nvme_feat_fdp_cdw11 {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/* Endurance Group Identifier */
		uint32_t endgid   : 16;
		/* [한국어] [15:0] ENDGID — 대상 Endurance Group ID(1-base). FDP는 NS가 아닌 EG 단위 활성. */
		uint32_t reserved : 16;
		/* [한국어] [31:16] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_fdp_cdw11) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_FDP
 */
/* [한국어] === union spdk_nvme_feat_fdp_cdw12 === FID=0x1D CDW12 (FDP enable). */
union spdk_nvme_feat_fdp_cdw12 {
	uint32_t raw;                              /* [한국어] CDW12 raw. */
	struct {
		/* Flexible Data Placement Enable */
		uint32_t fdpe      : 1;
		/* [한국어] [0] FDPE — 1=FDP 활성, 0=비활성. enable 시 컨트롤러는 FDP 구성에 따라
		 * RUH(Reclaim Unit Handle)별 격리된 write 영역 운용. */
		uint32_t reserved1 : 7;
		/* [한국어] [7:1] reserved. */
		/* Flexible Data Placement Configuration Index */
		uint32_t fdpci     : 8;
		/* [한국어] [15:8] FDPCI — 어떤 FDP 구성(RUH 수, RUH 타입 등)을 사용할지. FDP
		 * Configurations log page(LID=0x20) cfg_descriptor 배열의 인덱스. */
		uint32_t reserved2 : 16;
		/* [한국어] [31:16] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_fdp_cdw12) == 4, "Incorrect size");

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_FDP_EVENTS
 */
/* [한국어] === union spdk_nvme_feat_fdp_events_cdw11 === FID=0x1E CDW11 (FDP events). */
union spdk_nvme_feat_fdp_events_cdw11 {
	uint32_t raw;                              /* [한국어] CDW11 raw. */
	struct {
		/* Placement Handle associated with RUH */
		uint32_t phndl    : 16;
		/* [한국어] [15:0] PHNDL — Placement Handle (RUH 인덱스). 어떤 RUH의 event 종류를
		 * 토글할지 지정. */
		/* Number of FDP event types in data buffer */
		uint32_t noet     : 8;
		/* [한국어] [23:16] NOET — 데이터 페이로드의 fdp_event_desc 엔트리 수. 각 desc는
		 * fdp_etype + fdpeta(enabled bit)로 enable/disable. */
		uint32_t reserved : 8;
		/* [한국어] [31:24] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_fdp_events_cdw11) == 4, "Incorrect size");

/**
 * Data used by Set Feature \ref SPDK_NVME_FEAT_FDP_EVENTS
 */
/* [한국어] === union spdk_nvme_feat_fdp_events_cdw12 === FID=0x1E CDW12 (FDP events). */
union spdk_nvme_feat_fdp_events_cdw12 {
	uint32_t raw;                              /* [한국어] CDW12 raw. */
	struct {
		/* FDP Event Enable */
		uint32_t fdpee     : 1;
		/* [한국어] [0] FDPEE — 1=enable, 0=disable. NOET 개의 event_desc 모두에 적용. */
		uint32_t reserved1 : 31;
		/* [한국어] [31:1] reserved. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_feat_fdp_events_cdw12) == 4, "Incorrect size");

/* [한국어] === union spdk_nvme_cmd_cdw10 === SQE의 CDW10 (32비트)을 opcode별로 다르게 해석.
 * NVMe Base 2.0 §5 각 명령 절. SPDK lib/nvme/nvme_ctrlr_cmd.c가 admin 명령 빌드 시 이 union을
 * 통해 비트 필드 단위로 채운다(raw 직접 setting도 가능). 같은 4B 영역에 여러 struct가 overlay. */
union spdk_nvme_cmd_cdw10 {
	uint32_t raw;                              /* [한국어] CDW10 raw 32비트. */
	struct {
		/* Controller or Namespace Structure */
		uint32_t cns       : 8;
		/* [한국어] [7:0] CNS — 어떤 구조체를 받을지 (enum spdk_nvme_identify_cns). */
		uint32_t reserved  : 8;
		/* [한국어] [15:8] reserved. */
		/* Controller Identifier */
		uint32_t cntid     : 16;
		/* [한국어] [31:16] CNTID — 일부 CNS(0x12 controller list, 0x13 NS attached ctrlrs)
		 * 에서 starting controller ID. */
	} identify;                                /* [한국어] opcode=0x06(IDENTIFY) 시 인코딩. */

	struct {
		/* Log Page Identifier */
		uint32_t lid       : 8;
		/* [한국어] [7:0] LID — Log Page ID (enum spdk_nvme_log_page). */
		/* Log Specific Field */
		uint32_t lsp       : 7;
		/* [한국어] [14:8] LSP — Log별 specific param (Telemetry create=1 등). */
		/* Retain Asynchronous Event */
		uint32_t rae       : 1;
		/* [한국어] [15] RAE — 1이면 회수해도 AER 마스크 유지(반복 회수 가능). 0=clear. */
		/* Number of Dwords Lower */
		uint32_t numdl     : 16;
		/* [한국어] [31:16] NUMDL — 회수할 데이터의 dword 수 - 1, 하위 16비트. CDW11.NUMDU와 합쳐 32비트. */
	} get_log_page;                            /* [한국어] opcode=0x02 시. */

	struct {
		/* Submission Queue Identifier */
		uint32_t sqid      : 16;
		/* [한국어] [15:0] SQID — abort 대상이 들어 있는 SQ. */
		/* Command Identifier */
		uint32_t cid       : 16;
		/* [한국어] [31:16] CID — 대상 명령의 CID. */
	} abort;                                   /* [한국어] opcode=0x08 시. */

	struct {
		/* NVMe Security Specific Field */
		uint32_t nssf      : 8;
		/* [한국어] [7:0] NSSF — NVMe Security Specific Field. */
		/* SP Specific 0 */
		uint32_t spsp0     : 8;
		/* [한국어] [15:8] SPSP0 — Security Protocol Specific 0. TCG Opal에서 ComID. */
		/* SP Specific 1 */
		uint32_t spsp1     : 8;
		/* [한국어] [23:16] SPSP1 — SP specific 1. */
		/* Security Protocol */
		uint32_t secp      : 8;
		/* [한국어] [31:24] SECP — Security Protocol. 0x01=TCG, 0x02=TCG Storage 등. */
	} sec_send_recv;                           /* [한국어] opcode=0x81/0x82 시. */

	struct {
		/* Queue Identifier */
		uint32_t qid       : 16;
		/* [한국어] [15:0] QID — 생성할 SQ/CQ ID. */
		/* Queue Size */
		uint32_t qsize     : 16;
		/* [한국어] [31:16] QSIZE — 큐 항목 수 - 1 (0-based). MQES 한도 이내. */
	} create_io_q;                             /* [한국어] opcode=0x01/0x05 시. */

	struct {
		/* Queue Identifier */
		uint32_t qid       : 16;
		/* [한국어] [15:0] QID — 삭제 대상 SQ/CQ ID. */
		uint32_t reserved  : 16;
		/* [한국어] [31:16] reserved. */
	} delete_io_q;                             /* [한국어] opcode=0x00/0x04 시. */

	struct {
		/* Feature Identifier */
		uint32_t fid       : 8;
		/* [한국어] [7:0] FID — Feature ID (enum spdk_nvme_feat). */
		/* Select */
		uint32_t sel       : 3;
		/* [한국어] [10:8] SEL — 0=current, 1=default, 2=saved, 3=supported capability. */
		uint32_t reserved  : 21;
		/* [한국어] [31:11] reserved. */
	} get_features;                            /* [한국어] opcode=0x0a 시. */

	struct {
		/* Feature Identifier */
		uint32_t fid       : 8;
		/* [한국어] [7:0] FID — 설정 대상 Feature ID. */
		uint32_t reserved  : 23;
		/* [한국어] [30:8] reserved. */
		/* Save */
		uint32_t sv        : 1;
		/* [한국어] [31] SV — 1이면 power cycle을 넘어 persistent 저장. */
	} set_features;                            /* [한국어] opcode=0x09 시. */

	struct {
		/* Select */
		uint32_t sel      : 4;
		/* [한국어] [3:0] SEL — 0=Attach, 1=Detach (enum spdk_nvme_ns_attach_type). */
		uint32_t reserved : 28;
		/* [한국어] [31:4] reserved. */
	} ns_attach;                               /* [한국어] opcode=0x15 시. */

	struct {
		/* Select */
		uint32_t sel      : 4;
		/* [한국어] [3:0] SEL — 0=Create, 1=Delete (enum spdk_nvme_ns_management_type). */
		uint32_t reserved : 28;
		/* [한국어] [31:4] reserved. */
	} ns_manage;                               /* [한국어] opcode=0x0d 시. */

	struct {
		/* Number of Ranges */
		uint32_t nr       : 8;
		/* [한국어] [7:0] NR — DSM range 수 - 1 (0-based, 최대 256). */
		uint32_t reserved : 24;
		/* [한국어] [31:8] reserved. */
	} dsm;                                     /* [한국어] opcode=0x09(I/O DSM) 시. */

	struct {
		/* Reservation Register Action */
		uint32_t rrega     : 3;
		/* [한국어] [2:0] RREGA — Register/Unregister/Replace key (enum spdk_nvme_reservation_register_action). */
		/* Ignore Existing Key */
		uint32_t iekey     : 1;
		/* [한국어] [3] IEKEY — 1이면 crkey 검증 생략. */
		uint32_t reserved  : 26;
		/* [한국어] [29:4] reserved. */
		/* Change Persist Through Power Loss State */
		uint32_t cptpl     : 2;
		/* [한국어] [31:30] CPTPL — power-loss persistence 변경 (enum spdk_nvme_reservation_register_cptpl). */
	} resv_register;                           /* [한국어] opcode=0x0d(I/O Reservation Register) 시. */

	struct {
		/* Reservation Release Action */
		uint32_t rrela     : 3;
		/* [한국어] [2:0] RRELA — Release/Clear (enum spdk_nvme_reservation_release_action). */
		/* Ignore Existing Key */
		uint32_t iekey     : 1;
		/* [한국어] [3] IEKEY — 키 검증 생략. */
		uint32_t reserved1 : 4;
		/* [한국어] [7:4] reserved. */
		/* Reservation Type */
		uint32_t rtype     : 8;
		/* [한국어] [15:8] RTYPE — Write Excl/Excl Access 등 (enum spdk_nvme_reservation_type). */
		uint32_t reserved2 : 16;
		/* [한국어] [31:16] reserved. */
	} resv_release;                            /* [한국어] opcode=0x15 시. */

	struct {
		/* Reservation Acquire Action */
		uint32_t racqa     : 3;
		/* [한국어] [2:0] RACQA — Acquire/Preempt/Preempt+Abort (enum spdk_nvme_reservation_acquire_action). */
		/* Ignore Existing Key */
		uint32_t iekey     : 1;
		/* [한국어] [3] IEKEY — 키 검증 생략. */
		uint32_t reserved1 : 4;
		/* [한국어] [7:4] reserved. */
		/* Reservation Type */
		uint32_t rtype     : 8;
		/* [한국어] [15:8] RTYPE — reservation 타입. */
		uint32_t reserved2 : 16;
		/* [한국어] [31:16] reserved. */
	} resv_acquire;                            /* [한국어] opcode=0x11 시. */

	struct {
		/* Management Operation */
		uint32_t mo        : 8;
		/* [한국어] [7:0] MO — I/O Management 동작(FDP RUHS/RUHU 등). */
		uint32_t reserved  : 8;
		/* [한국어] [15:8] reserved. */
		/* Management Operation Specific */
		uint32_t mos       : 16;
		/* [한국어] [31:16] MOS — MO별 specific 파라미터. */
	} mgmt_send_recv;                          /* [한국어] opcode=0x12/0x1D 시. */
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_cmd_cdw10) == 4, "Incorrect size");

/* [한국어] === union spdk_nvme_cmd_cdw11 === SQE의 CDW11 (32비트). opcode별 인코딩.
 * NVMe Base 2.0 §5. CDW10과 마찬가지로 같은 4B에 여러 struct가 overlay.
 * 모든 feat_* union이 이 union 안에 포함되어 있어 spdk_nvme_ctrlr_set_feature 가 단일 cdw11을
 * 직접 union 멤버로 캐스팅해 채울 수 있도록 설계. */
union spdk_nvme_cmd_cdw11 {
	uint32_t raw;                              /* [한국어] CDW11 raw. */

	struct {
		/* NVM Set Identifier */
		uint32_t nvmsetid  : 16;
		/* [한국어] [15:0] NVMSETID — Identify의 일부 CNS에서 NVM Set 필터링. */
		uint32_t reserved  : 8;
		/* [한국어] [23:16] reserved. */
		/* Command Set Identifier */
		uint32_t csi       : 8;
		/* [한국어] [31:24] CSI — 0=NVM, 1=KV, 2=ZNS. CNS=0x05/0x06/0x1c 등 IOCS 한정 CNS에서 의미. */
	} identify;                                /* [한국어] IDENTIFY. */

	struct {
		/* Physically Contiguous */
		uint32_t pc       : 1;
		/* [한국어] [0] PC — 1이면 SQ 메모리가 물리적으로 연속(PRP1=base). 0이면 PRP list. */
		/* Queue Priority */
		uint32_t qprio    : 2;
		/* [한국어] [2:1] QPRIO — Urgent(0)/High(1)/Med(2)/Low(3). CC.AMS=WRR일 때만 의미. */
		uint32_t reserved : 13;
		/* [한국어] [15:3] reserved. */
		/* Completion Queue Identifier */
		uint32_t cqid     : 16;
		/* [한국어] [31:16] CQID — 이 SQ가 완료 통지할 CQ ID. CQ가 먼저 생성되어 있어야 함. */
	} create_io_sq;                            /* [한국어] CREATE_IO_SQ(0x01). */

	struct {
		/* Physically Contiguous */
		uint32_t pc       : 1;
		/* [한국어] [0] PC — CQ 메모리 물리 연속 여부. */
		/* Interrupts Enabled */
		uint32_t ien      : 1;
		/* [한국어] [1] IEN — 1=인터럽트 활성, 0=비활성. SPDK polled-mode는 IEN=0. */
		uint32_t reserved : 14;
		/* [한국어] [15:2] reserved. */
		/* Interrupt Vector */
		uint32_t iv       : 16;
		/* [한국어] [31:16] IV — MSI-X 벡터 인덱스. IEN=0이면 무시. */
	} create_io_cq;                            /* [한국어] CREATE_IO_CQ(0x05). */

	struct {
		/* Directive Operation */
		uint32_t doper    : 8;
		/* [한국어] [7:0] DOPER — Directive Send/Recv operation (enum spdk_nvme_streams_directive_*_operation). */
		/* Directive Type */
		uint32_t dtype    : 8;
		/* [한국어] [15:8] DTYPE — 1=Identify, 2=Streams, 3=Data Placement (FDP). */
		/* Directive Specific */
		uint32_t dspec    : 16;
		/* [한국어] [31:16] DSPEC — operation별 specific 파라미터. */
	} directive;                               /* [한국어] DIRECTIVE_SEND/RECV(0x19/0x1a). */

	struct {
		/* Number of Dwords */
		uint32_t numdu    : 16;
		/* [한국어] [15:0] NUMDU — Get Log Page 회수 dword 수의 상위 16비트. CDW10.NUMDL과 합쳐 32비트. */
		/* Log Specific Identifier */
		uint32_t lsid     : 16;
		/* [한국어] [31:16] LSID — Log별 specific identifier(예: ANA group ID). */
	} get_log_page;                            /* [한국어] GET_LOG_PAGE(0x02). */

	struct {
		/* Extended Data Structure */
		uint32_t eds      : 1;
		/* [한국어] [0] EDS — 1이면 Extended reservation status data structure 사용. */
		uint32_t reserved : 31;
		/* [한국어] [31:1] reserved. */
	} resv_report;                             /* [한국어] RESERVATION_REPORT(0x0e). */

	union spdk_nvme_feat_arbitration feat_arbitration;
	/* [한국어] FID=0x01 Set/Get Features의 cdw11. union 안의 union — bits.ab/lpw/mpw/hpw로 직접 접근. */
	union spdk_nvme_feat_power_management feat_power_management;
	/* [한국어] FID=0x02. */
	union spdk_nvme_feat_lba_range_type feat_lba_range_type;
	/* [한국어] FID=0x03. */
	union spdk_nvme_feat_temperature_threshold feat_temp_threshold;
	/* [한국어] FID=0x04. */
	union spdk_nvme_feat_error_recovery feat_error_recovery;
	/* [한국어] FID=0x05. */
	union spdk_nvme_feat_volatile_write_cache feat_volatile_write_cache;
	/* [한국어] FID=0x06. */
	union spdk_nvme_feat_number_of_queues feat_num_of_queues;
	/* [한국어] FID=0x07 — controller init 시 SPDK가 발행. */
	union spdk_nvme_feat_interrupt_coalescing feat_interrupt_coalescing;
	/* [한국어] FID=0x08. */
	union spdk_nvme_feat_interrupt_vector_configuration feat_interrupt_vector_configuration;
	/* [한국어] FID=0x09. */
	union spdk_nvme_feat_write_atomicity feat_write_atomicity;
	/* [한국어] FID=0x0a. */
	union spdk_nvme_feat_async_event_configuration feat_async_event_cfg;
	/* [한국어] FID=0x0b — SPDK가 init 시 모든 AER 종류 enable. */
	union spdk_nvme_feat_keep_alive_timer feat_keep_alive_timer;
	/* [한국어] FID=0x0f — NVMe-oF KATO. */
	union spdk_nvme_feat_host_identifier feat_host_identifier;
	/* [한국어] FID=0x81. */
	union spdk_nvme_feat_reservation_notification_mask feat_rsv_notification_mask;
	/* [한국어] FID=0x82. */
	union spdk_nvme_feat_reservation_persistence feat_rsv_persistence;
	/* [한국어] FID=0x83. */
	union spdk_nvme_feat_fdp_cdw11 feat_fdp_cdw11;
	/* [한국어] FID=0x1d — FDP enable. */
	union spdk_nvme_feat_fdp_events_cdw11 feat_fdp_events_cdw11;
	/* [한국어] FID=0x1e — FDP events. */

	struct {
		/* Attribute – Integral Dataset for Read */
		uint32_t idr      : 1;
		/* [한국어] [0] IDR — 이 dataset을 read 위주로 다룰 것이라는 hint. */
		/* Attribute – Integral Dataset for Write */
		uint32_t idw      : 1;
		/* [한국어] [1] IDW — write 위주 hint. */
		/* Attribute – Deallocate */
		uint32_t ad       : 1;
		/* [한국어] [2] AD — TRIM/Discard. 컨트롤러가 LBA range를 dealloc 처리. */
		uint32_t reserved : 29;
		/* [한국어] [31:3] reserved. */
	} dsm;                                     /* [한국어] DATASET_MANAGEMENT(0x09 I/O). */
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_cmd_cdw11) == 4, "Incorrect size");

/* [한국어] === union spdk_nvme_cmd_cdw12 === SQE의 CDW12 (32비트). I/O 명령에서 NLB+제어 비트.
 * NVMe NVM CS 1.0 §3. write/read 류는 CDW12 구조가 동일(spdk는 union을 통해 일관 접근).
 * 상위 16비트(bit 16-31)가 호스트 io_flags의 CDW12_MASK와 1:1 매핑되어 SPDK가 변환 비용 0. */
union spdk_nvme_cmd_cdw12 {
	uint32_t raw;                              /* [한국어] CDW12 raw. */

	struct {
		/* Number of Ranges */
		uint32_t nlb       : 16;
		/* [한국어] [15:0] NLB — Number of Logical Blocks - 1 (0-based). 한 명령에 NLB+1개 LBA. */
		uint32_t reserved  : 4;
		/* [한국어] [19:16] reserved. */
		/* Directive Type */
		uint32_t dtype     : 4;
		/* [한국어] [23:20] DTYPE — Directive Type. 0x1=Identify, 0x2=Streams, 0x3=Data Placement(FDP). */
		/* Storage Tag Check */
		uint32_t stc       : 1;
		/* [한국어] [24] STC — Storage Tag Check (NVMe 2.0 64B Guard PI). */
		uint32_t reserved2 : 1;
		/* [한국어] [25] reserved (단, ZNS Append에서는 PIREMAP). */
		/* Protection Information Check */
		uint32_t prchk     : 3;
		/* [한국어] [28:26] PRCHK — bit26=REFTAG, bit27=APPTAG, bit28=GUARD 검증. */
		/* Protection Information Action */
		uint32_t pract     : 1;
		/* [한국어] [29] PRACT — 1이면 컨트롤러가 PI 자동 생성/검증. */
		/* Force Unit Access */
		uint32_t fua       : 1;
		/* [한국어] [30] FUA — write 미디어 도달 보장 / read는 cache 우회. */
		/* Limited Retry */
		uint32_t lr        : 1;
		/* [한국어] [31] LR — 에러 시 retry 제한. 빠른 실패 우선. */
	} write;                                   /* [한국어] WRITE(0x01)/READ(0x02)/COMPARE(0x05). */

	struct {
		/* Number of Ranges */
		uint32_t nr        : 8;
		/* [한국어] [7:0] NR — Source Range 수 - 1 (Copy 명령). */
		/* Descriptor Format */
		uint32_t df        : 4;
		/* [한국어] [11:8] DF — Source Range descriptor format (0/1/2/3 = 32B/40B 등). */
		/* Protection Information Field Read */
		uint32_t prinfor   : 4;
		/* [한국어] [15:12] PRINFOR — read(source) 측 PI 필드. */
		uint32_t reserved  : 4;
		/* [한국어] [19:16] reserved. */
		/* Directive Type */
		uint32_t dtype     : 4;
		/* [한국어] [23:20] DTYPE — destination directive type. */
		/* Storage Tag Check Write */
		uint32_t stcw      : 1;
		/* [한국어] [24] STCW — write side storage tag check. */
		uint32_t reserved2 : 1;
		/* [한국어] [25] reserved. */
		/* Protection Information Field Write */
		uint32_t prinfow   : 4;
		/* [한국어] [29:26] PRINFOW — write(destination) PI 필드. */
		/* Force Unit Access */
		uint32_t fua       : 1;
		/* [한국어] [30] FUA. */
		/* Limited Retry */
		uint32_t lr        : 1;
		/* [한국어] [31] LR. */
	} copy;                                    /* [한국어] COPY(0x19) — 컨트롤러 내부 LBA 복사. */

	struct {
		/* Number of Logical Blocks */
		uint32_t nlb       : 16;
		/* [한국어] [15:0] NLB — 0으로 채울 LBA 수 - 1. */
		uint32_t reserved  : 8;
		/* [한국어] [23:16] reserved. */
		/* Storage Tag Check */
		uint32_t stc       : 1;
		/* [한국어] [24] STC. */
		/* Deallocate */
		uint32_t deac      : 1;
		/* [한국어] [25] DEAC — Write Zeroes에서 1이면 dealloc까지 수행(메모리 해제). */
		/* Protection Information Check */
		uint32_t prchk     : 3;
		/* [한국어] [28:26] PRCHK. */
		/* Protection Information Action */
		uint32_t pract     : 1;
		/* [한국어] [29] PRACT. */
		/* Force Unit Access */
		uint32_t fua       : 1;
		/* [한국어] [30] FUA. */
		/* Limited Retry */
		uint32_t lr        : 1;
		/* [한국어] [31] LR. */
	} write_zeroes;                            /* [한국어] WRITE_ZEROES(0x08). */

	union spdk_nvme_feat_fdp_cdw12 feat_fdp_cdw12;
	/* [한국어] FID=0x1d Set Features의 cdw12. */
	union spdk_nvme_feat_fdp_events_cdw12 feat_fdp_events_cdw12;
	/* [한국어] FID=0x1e Set Features의 cdw12. */
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_cmd_cdw12) == 4, "Incorrect size");

/* [한국어] === union spdk_nvme_cmd_cdw13 === SQE의 CDW13 (32비트). NVM CS write의 directive
 * specific 등 일부 명령에서 사용. 대부분의 read는 CDW13=0. */
union spdk_nvme_cmd_cdw13 {
	uint32_t raw;                              /* [한국어] CDW13 raw. */

	struct {
		/* Dataset Management */
		uint32_t dsm       : 8;
		/* [한국어] [7:0] DSM — Dataset Management hint(IDR/IDW/AD/sequential write/seq read 등 8비트 마스크). */
		uint32_t reserved  : 8;
		/* [한국어] [15:8] reserved. */
		/* Directive Specific */
		uint32_t dspec     : 16;
		/* [한국어] [31:16] DSPEC — Streams stream id 또는 FDP placement handle ID 등. */
	} write;                                   /* [한국어] WRITE(0x01) 시. */
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_cmd_cdw13) == 4, "Incorrect size");

/*
 * [한국어] ★ I/O 경로 제출 측 핵심 구조체 ★
 * struct spdk_nvme_cmd - Submission Queue Entry (SQE)
 *
 * NVMe 스펙 §4.2. 고정 64바이트. 16개의 32비트 dword(CDW0~CDW15)로 구성.
 * 호스트는 이 구조체를 SQ 링 버퍼의 sq_tail 위치에 기록한 뒤 doorbell을
 * ring하여 장치에 제출 통지. 장치는 DMA로 SQE를 읽고 해당 명령을 수행.
 *
 * Dword 배치:
 *   CDW0    : opc(8) + fuse(2) + rsvd(4) + psdt(2) + cid(16)
 *   CDW1    : nsid (네임스페이스 ID)
 *   CDW2-3  : 예약
 *   CDW4-5  : mptr (메타데이터 포인터, 64bit)
 *   CDW6-9  : dptr — PRP(prp1 + prp2) 또는 SGL1 디스크립터
 *   CDW10-15: 명령별 파라미터(opcode에 따라 해석)
 *
 * 호스트 드라이버는 opcode 설정 후 CDWn 필드를 opcode 규격에 맞게 채운다.
 * 예) READ: nsid=X, dptr=PRP/SGL, cdw10/11=SLBA(64bit), cdw12=NLB(16bit)+제어 플래그
 */
struct spdk_nvme_cmd {
	/* dword 0 */
	uint16_t opc	:  8;	/* opcode */
                                  /* [한국어] 8비트 opcode — admin(0x00~)이나 I/O(0x00~) 코드 지정. enum spdk_nvme_opc / spdk_nvme_nvm_opcode 참조
                                   *  - 예: READ=0x02, WRITE=0x01, FLUSH=0x00, WRITE_ZEROES=0x08 */
	uint16_t fuse	:  2;	/* fused operation */
                                  /* [한국어] Fused 명령 지정 — 두 SQE를 원자적으로 실행하도록 큐잉
                                   *  - 0=normal, 1=fused first, 2=fused second, 3=reserved
                                   *  - COMPARE+WRITE atomic 구현에 사용 */
	uint16_t rsvd1	:  4;     /* [한국어] 예약 4비트 */
	uint16_t psdt	:  2;     /* [한국어] PSDT — PRP/SGL 중 어느 포맷으로 dptr·mptr을 해석할지 (enum spdk_nvme_psdt_value 참조) */
	uint16_t cid;		/* command identifier */
                                  /* [한국어] 16비트 command ID — 호스트가 SQE 제출 시 할당
                                   *  - CQE의 cid와 매치해 어느 요청의 완료인지 복원
                                   *  - 범위: 0 ~ (num_entries-1) 큐 내 unique 필요 */

	/* dword 1 */
	uint32_t nsid;		/* namespace identifier */
                                  /* [한국어] 대상 네임스페이스 ID — 일반 I/O에서는 해당 NS 번호, 전역 명령은 0xFFFFFFFF(SPDK_NVME_GLOBAL_NS_TAG) */

	/* dword 2-3 */
	uint32_t rsvd2;           /* [한국어] 예약 */
	uint32_t rsvd3;           /* [한국어] 예약 */

	/* dword 4-5 */
	uint64_t mptr;		/* metadata pointer */
                                  /* [한국어] 메타데이터(PI Guard/AppTag/RefTag 포함 영역) 버스 주소
                                   *  - PSDT=PRP이면 호스트 메모리 연속 버퍼 포인터
                                   *  - PSDT=SGL_MPTR_SGL이면 이 필드가 SGL 디스크립터 시작 주소로 해석
                                   *  - 메타데이터를 쓰지 않으면 0 */

	/* dword 6-9: data pointer */
	union {
		struct {
			uint64_t prp1;		/* prp entry 1 */
                                  /* [한국어] 첫 PRP 엔트리 (임의 오프셋 허용)
                                   *  - 단일 4KB 미만 전송은 prp1만으로 충분
                                   *  - 그 이상은 prp1 + prp2로 2페이지, 혹은 prp2가 PRP 리스트 포인터 역할 */
			uint64_t prp2;		/* prp entry 2 */
                                  /* [한국어] 두 번째 PRP 엔트리 또는 PRP 리스트 포인터
                                   *  - 전송이 2 페이지면: prp2 = 두 번째 페이지 주소
                                   *  - 3페이지 이상: prp2 = 나머지 PRP를 담은 4KB 정렬된 리스트 배열 주소 (tracker의 u.prp[])
                                   *  - 모든 후속 PRP는 4KB 경계 정렬 필수 */
		} prp;
                                  /* [한국어] PRP(Physical Region Page) 모드 — PSDT=0 */

		struct spdk_nvme_sgl_descriptor sgl1;
                                  /* [한국어] SGL 모드 — PSDT=1/2 시 이 필드로 해석
                                   *  - 단일 디스크립터 또는 세그먼트 체이닝 가능
                                   *  - NVMe-oF TCP/RDMA 경로가 주로 사용 */
	} dptr;
                                  /* [한국어] Data Pointer (dword 6-9, 16바이트). psdt 값에 따라 prp 또는 sgl1로 해석 */

	/* command-specific */
	union {
		uint32_t cdw10;
		union spdk_nvme_cmd_cdw10 cdw10_bits;
                                  /* [한국어] cdw10 — opcode별 의미 상이. READ/WRITE에서는 SLBA의 하위 32비트 */
	};
	/* command-specific */
	union {
		uint32_t cdw11;
		union spdk_nvme_cmd_cdw11 cdw11_bits;
                                  /* [한국어] cdw11 — READ/WRITE에서는 SLBA 상위 32비트 (총 64bit LBA) */
	};
	/* command-specific */
	union {
		uint32_t cdw12;
		union spdk_nvme_cmd_cdw12 cdw12_bits;
                                  /* [한국어] cdw12 — READ/WRITE에서는 하위 16비트 NLB(Number of Logical Blocks, 0-based) + 상위 비트에 제어 플래그(FUA, LR, PI 등) */
	};
	/* command-specific */
	union {
		uint32_t cdw13;
		union spdk_nvme_cmd_cdw13 cdw13_bits;
                                  /* [한국어] cdw13 — DSM(Dataset Management) hint 비트, directive 등 */
	};
	/* dword 14-15 */
	uint32_t cdw14;		/* command-specific */
                                  /* [한국어] cdw14 — I/O PI 명령 시 EILBRT/RefTag, 기타 옵션 */
	uint32_t cdw15;		/* command-specific */
                                  /* [한국어] cdw15 — I/O PI 명령 시 ELBAT/ELBATM, 기타 옵션 */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_cmd) == 64, "Incorrect size");
                                  /* [한국어] 64B 고정 — 스펙 §4.2. 어긋나면 SQ 링 버퍼 인덱싱이 깨짐 */

/*
 * [한국어] ★ I/O 경로 완료 측 상태 필드 ★
 * struct spdk_nvme_status - CQE 내부 상태 비트필드 (2바이트)
 *
 * NVMe 스펙 §4.6.1. CQE의 dword 3 상위 16비트에 배치된다.
 * phase 비트(p)는 완료 폴링의 핵심 — 장치가 CQ에 새 엔트리를 쓸 때마다
 * phase를 토글해서 호스트가 "새 CQE" 여부를 판별하게 함.
 */
struct spdk_nvme_status {
	uint16_t p	:  1;	/* phase tag */
                                  /* [한국어] Phase Tag — 장치가 CQ 한 바퀴 돌 때마다 뒤집힘
                                   *  - 호스트는 자신이 기대하는 phase(qpair의 flags.phase)와 일치해야 "유효 CQE"로 판정
                                   *  - 이 메커니즘이 CQ에 대한 lockless "새 엔트리 감지"의 근간 */
	uint16_t sc	:  8;	/* status code */
                                  /* [한국어] 상태 코드 (SC) — 성공/에러 세부 분류
                                   *  - 0 = Successful Completion
                                   *  - nonzero = sct(타입)과 조합해 enum spdk_nvme_status_code / spdk_nvme_generic_command_status_code 등 매핑 */
	uint16_t sct	:  3;	/* status code type */
                                  /* [한국어] SCT — 상태 코드 타입
                                   *  - 0=Generic, 1=Command Specific, 2=Media/DI, 3=Path, 7=Vendor Specific
                                   *  - (SCT, SC) 쌍으로 고유한 상태 해석 */
	uint16_t crd	:  2;   /* command retry delay */
                                  /* [한국어] Command Retry Delay — 실패 시 재시도까지 기다릴 시간(힌트)
                                   *  - 컨트롤러가 CRT 레지스터로 주기 정의, 이 필드는 그 중 어느 슬롯을 쓸지 */
	uint16_t m	:  1;	/* more */
                                  /* [한국어] More — 같은 커맨드에 대해 추가 정보(예: 에러 로그)가 있음을 표시 */
	uint16_t dnr	:  1;	/* do not retry */
                                  /* [한국어] DNR — 1이면 호스트가 재시도해도 성공 가능성 없음 → 즉시 에러 반환 */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_status) == 2, "Incorrect size");
                                  /* [한국어] 2B 고정 — CQE의 dword 3 상위 절반 */

/**
 * Completion queue entry
 */
/*
 * [한국어] ★ I/O 경로 완료 측 핵심 구조체 ★
 * struct spdk_nvme_cpl - Completion Queue Entry (CQE)
 *
 * NVMe 스펙 §4.6. 고정 16바이트. 장치가 요청 처리 후 DMA로 호스트 CQ에 씀.
 *
 * 드라이버의 완료 폴링 흐름:
 *   1) cq[cq_head] 위치의 CQE 읽기 (volatile read)
 *   2) CQE의 status.p와 qpair의 기대 phase 비교 → 일치 시 유효
 *   3) CQE의 cid로 tracker 배열 색인 → 원 nvme_request 복원
 *   4) status 코드 평가 후 완료 콜백 호출
 *   5) cq_head 증가, wrap 시 expected phase 토글
 *   6) (일정 수 처리 후) CQ head doorbell ring
 */
struct spdk_nvme_cpl {
	/* dword 0 */
	uint32_t		cdw0;	/* command-specific */
                                  /* [한국어] 명령별 반환 데이터 (예: GET_FEATURES 현재값, READ의 zero-copy 확장 정보 등)
                                   *  - 일반 READ/WRITE는 0 */

	/* dword 1 */
	uint32_t		cdw1;	/* command-specific */
                                  /* [한국어] 명령별 반환 확장 dword */

	/* dword 2 */
	uint16_t		sqhd;	/* submission queue head pointer */
                                  /* [한국어] 장치가 현재까지 소비한 SQ head 포인터
                                   *  - 호스트는 이 값으로 SQ 내 사용 가능 공간(= num_entries - (sq_tail - sqhd) mod num_entries)을 계산 */
	uint16_t		sqid;	/* submission queue identifier */
                                  /* [한국어] 이 CQE가 속한 SQ의 ID — 여러 SQ가 하나의 CQ를 공유할 때 식별 */

	/* dword 3 */
	uint16_t		cid;	/* command identifier */
                                  /* [한국어] 원 SQE의 cid 복사 — 이 값으로 tracker 배열 색인 → 호스트 요청 복원 */
	union {
		uint16_t                status_raw;
                                  /* [한국어] raw 16비트 뷰 — 전체 비트 마스크 비교 시 사용 */
		struct spdk_nvme_status	status;
                                  /* [한국어] 비트필드 뷰 — p/sc/sct/crd/m/dnr 개별 필드 접근 */
	};
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_cpl) == 16, "Incorrect size");
                                  /* [한국어] 16B 고정 — 스펙 §4.6. CQ 링 엔트리 크기 */

/**
 * Dataset Management range
 */
struct spdk_nvme_dsm_range {
	union {
		struct {
			uint32_t af		: 4; /**< access frequency */
			uint32_t al		: 2; /**< access latency */
			uint32_t reserved0	: 2;

			uint32_t sr		: 1; /**< sequential read range */
			uint32_t sw		: 1; /**< sequential write range */
			uint32_t wp		: 1; /**< write prepare */
			uint32_t reserved1	: 13;

			uint32_t access_size	: 8; /**< command access size */
		} bits;

		uint32_t raw;
	} attributes;

	uint32_t length;
	uint64_t starting_lba;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_dsm_range) == 16, "Incorrect size");

/**
 * Simple Copy Command source range
 */
struct spdk_nvme_scc_source_range {
	uint64_t reserved0;
	uint64_t slba;
	uint16_t nlb;
	uint16_t reserved18;
	uint32_t reserved20;
	uint32_t eilbrt;
	uint16_t elbat;
	uint16_t elbatm;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_scc_source_range) == 32, "Incorrect size");

/**
 * Status code types
 */
/* [한국어] === Status Code Type (SCT) — CQE의 status.sct 3비트 필드 ===
 * NVMe Base 2.0 §4.6.1.2.1. (SCT, SC) 쌍이 한 상태를 고유하게 결정한다. SPDK는
 * lib/nvme/nvme_qpair.c의 spdk_nvme_qpair_print_completion()이 sct/sc를 디코딩해 사람용
 * 문자열로 변환한다. */
enum spdk_nvme_status_code_type {
	SPDK_NVME_SCT_GENERIC		= 0x0,
	/* [한국어] 0x0 — Generic 카테고리. opcode/필드/SGL/SQ 자체에 대한 일반 오류
	 * (Invalid Field, Invalid Opcode, Aborted by Request 등). enum
	 * spdk_nvme_generic_command_status_code 참조. */
	SPDK_NVME_SCT_COMMAND_SPECIFIC	= 0x1,
	/* [한국어] 0x1 — Command Specific. 특정 admin/I/O 명령에서만 발생하는 오류
	 * (Invalid Queue Identifier, Invalid Firmware Slot 등). enum
	 * spdk_nvme_command_specific_status_code 참조. */
	SPDK_NVME_SCT_MEDIA_ERROR	= 0x2,
	/* [한국어] 0x2 — Media/DI(Data Integrity) Error. 미디어 자체의 손상, PI(Protection
	 * Information) 검증 실패, Compare 실패 등. enum spdk_nvme_media_error_status_code 참조. */
	SPDK_NVME_SCT_PATH		= 0x3,
	/* [한국어] 0x3 — Path-related (NVMe 1.4+). ANA 경로 손실, 비대칭 접근 비활성, host
	 * path error 등. NVMe-oF/multipath 시나리오에서 자주 등장. */
	/* 0x4-0x6 - reserved */
	SPDK_NVME_SCT_VENDOR_SPECIFIC	= 0x7,
	/* [한국어] 0x7 — 벤더 확장. SC 의미는 컨트롤러 벤더가 정의. */
};

/**
 * Generic command status codes
 */
/* [한국어] === enum spdk_nvme_generic_command_status_code === SCT=0x0(Generic) 일 때 SC.
 * NVMe Base 2.0 §4.2.3.1 Table 4-12. opcode 무관하게 모든 명령에서 발생 가능한 일반적 오류.
 * 0x00~0x7F=admin/I/O 공통, 0x80~0xBF=I/O CS 특화 (NVM CS 일부 SC만 generic으로 분류).
 * SPDK는 lib/nvme/nvme_qpair.c::nvme_qpair_print_completion에서 사람이 읽는 문자열로 변환. */
enum spdk_nvme_generic_command_status_code {
	SPDK_NVME_SC_SUCCESS				= 0x00,
	/* [한국어] 0x00 — 명령 성공. spdk_nvme_cpl_is_success() 매크로의 기준. */
	SPDK_NVME_SC_INVALID_OPCODE			= 0x01,
	/* [한국어] 0x01 — opcode 미지원/불가. admin SQ에 I/O opcode를 보냈거나 그 반대도 포함. */
	SPDK_NVME_SC_INVALID_FIELD			= 0x02,
	/* [한국어] 0x02 — CDW의 어떤 필드가 invalid (reserved 값/지원 안 함/범위 초과). 가장 흔한 오류. */
	SPDK_NVME_SC_COMMAND_ID_CONFLICT		= 0x03,
	/* [한국어] 0x03 — 이미 사용 중인 CID로 새 명령 제출. SPDK qpair tracker가 검출. */
	SPDK_NVME_SC_DATA_TRANSFER_ERROR		= 0x04,
	/* [한국어] 0x04 — DMA 전송 실패(PRP/SGL 무효 주소, PCIe AER 등). */
	SPDK_NVME_SC_ABORTED_POWER_LOSS			= 0x05,
	/* [한국어] 0x05 — 전원 손실로 abort. 보통 sudden shutdown 후 미디어 도달 못한 명령. */
	SPDK_NVME_SC_INTERNAL_DEVICE_ERROR		= 0x06,
	/* [한국어] 0x06 — 컨트롤러 내부 오류. controller fatal 직전 신호일 수 있음. */
	SPDK_NVME_SC_ABORTED_BY_REQUEST			= 0x07,
	/* [한국어] 0x07 — host의 Abort(0x08) 명령으로 취소됨. */
	SPDK_NVME_SC_ABORTED_SQ_DELETION		= 0x08,
	/* [한국어] 0x08 — SQ 삭제로 인한 강제 abort. spdk_nvme_cpl_is_aborted_sq_deletion 매크로. */
	SPDK_NVME_SC_ABORTED_FAILED_FUSED		= 0x09,
	/* [한국어] 0x09 — Fused pair 중 한 쪽 실패로 같이 abort. */
	SPDK_NVME_SC_ABORTED_MISSING_FUSED		= 0x0a,
	/* [한국어] 0x0a — Fused pair에서 짝이 없음(FUSE_FIRST 다음에 FUSE_SECOND가 없거나 vice versa). */
	SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT	= 0x0b,
	/* [한국어] 0x0b — NSID가 attach되지 않았거나 LBA format이 잘못됨. */
	SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR		= 0x0c,
	/* [한국어] 0x0c — 잘못된 명령 순서(예: CC.EN=1 전에 NVMe-oF Connect 등). */
	SPDK_NVME_SC_INVALID_SGL_SEG_DESCRIPTOR		= 0x0d,
	/* [한국어] 0x0d — SGL segment descriptor 무효. */
	SPDK_NVME_SC_INVALID_NUM_SGL_DESCIRPTORS	= 0x0e,
	/* [한국어] 0x0e — SGL descriptor 수가 데이터 길이와 맞지 않음. */
	SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID		= 0x0f,
	/* [한국어] 0x0f — Data SGL 총 길이가 명령 데이터 길이와 불일치. */
	SPDK_NVME_SC_METADATA_SGL_LENGTH_INVALID	= 0x10,
	/* [한국어] 0x10 — Metadata SGL 길이 불일치. */
	SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID	= 0x11,
	/* [한국어] 0x11 — SGL descriptor type이 컨트롤러가 지원하지 않는 값. */
	SPDK_NVME_SC_INVALID_CONTROLLER_MEM_BUF		= 0x12,
	/* [한국어] 0x12 — CMB(Controller Memory Buffer) 영역에 invalid 접근. */
	SPDK_NVME_SC_INVALID_PRP_OFFSET			= 0x13,
	/* [한국어] 0x13 — PRP offset이 페이지 정렬 등 제약 위반. */
	SPDK_NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED		= 0x14,
	/* [한국어] 0x14 — Write 길이가 AWUN을 넘김(atomic 보장 불가). */
	SPDK_NVME_SC_OPERATION_DENIED			= 0x15,
	/* [한국어] 0x15 — 권한 부족으로 거부. */
	SPDK_NVME_SC_INVALID_SGL_OFFSET			= 0x16,
	/* [한국어] 0x16 — SGL offset 무효. */
	/* 0x17 - reserved */
	SPDK_NVME_SC_HOSTID_INCONSISTENT_FORMAT		= 0x18,
	/* [한국어] 0x18 — 호스트가 8B/16B Host ID를 일관되지 않게 사용(중간에 EXHID 변경 등). */
	SPDK_NVME_SC_KEEP_ALIVE_EXPIRED			= 0x19,
	/* [한국어] 0x19 — Keep Alive 타임아웃 만료. NVMe-oF 호스트 dead 검출. */
	SPDK_NVME_SC_KEEP_ALIVE_INVALID			= 0x1a,
	/* [한국어] 0x1a — Keep Alive 명령 자체가 invalid (KATO=0인데 발행 등). */
	SPDK_NVME_SC_ABORTED_PREEMPT			= 0x1b,
	/* [한국어] 0x1b — Reservation Preempt + Abort로 강제 취소. */
	SPDK_NVME_SC_SANITIZE_FAILED			= 0x1c,
	/* [한국어] 0x1c — Sanitize 실패. Sanitize Status log(LID=0x81) 확인. */
	SPDK_NVME_SC_SANITIZE_IN_PROGRESS		= 0x1d,
	/* [한국어] 0x1d — Sanitize 진행 중에 sanitize-impacted 명령 발행. */
	SPDK_NVME_SC_SGL_DATA_BLOCK_GRANULARITY_INVALID	= 0x1e,
	/* [한국어] 0x1e — SGL Data Block의 granularity가 컨트롤러 요구 사항 위반. */
	SPDK_NVME_SC_COMMAND_INVALID_IN_CMB		= 0x1f,
	/* [한국어] 0x1f — CMB SQ에서 허용되지 않는 명령 발행. */
	SPDK_NVME_SC_COMMAND_NAMESPACE_IS_PROTECTED	= 0x20,
	/* [한국어] 0x20 — NS write protection 활성화 상태에서 write. */
	SPDK_NVME_SC_COMMAND_INTERRUPTED		= 0x21,
	/* [한국어] 0x21 — 컨트롤러가 명령 실행 중 인터럽트(controller reset 등). */
	SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR	= 0x22,
	/* [한국어] 0x22 — Transport-level transient error(Fabrics RDMA/TCP). retry 가능. */
	SPDK_NVME_SC_COMMAND_PROHIBITED_BY_LOCKDOWN	= 0x23,
	/* [한국어] 0x23 — Command/Feature Lockdown으로 차단(LID=0x14 참조). */
	SPDK_NVME_SC_ADMIN_COMMAND_MEDIA_NOT_READY	= 0x24,
	/* [한국어] 0x24 — Admin 명령이 media ready 신호 전 발행. */

	SPDK_NVME_SC_FDP_DISABLED			= 0x29,
	/* [한국어] 0x29 — FDP가 비활성 상태에서 FDP 관련 명령 발행. */
	SPDK_NVME_SC_INVALID_PLACEMENT_HANDLE_LIST	= 0x2A,
	/* [한국어] 0x2A — FDP placement handle list 무효. */

	SPDK_NVME_SC_LBA_OUT_OF_RANGE			= 0x80,
	/* [한국어] 0x80 — read/write 시 SLBA+NLB가 NS의 nsze를 넘김. NVM CS specific generic. */
	SPDK_NVME_SC_CAPACITY_EXCEEDED			= 0x81,
	/* [한국어] 0x81 — thin-provisioned NS의 실제 capacity 초과(write). */
	SPDK_NVME_SC_NAMESPACE_NOT_READY		= 0x82,
	/* [한국어] 0x82 — NS가 아직 ready 상태 아님(format 진행 중 등). DNR(Do Not Retry) 비트
	 * 함께 확인 — DNR=0이면 retry 가능. */
	SPDK_NVME_SC_RESERVATION_CONFLICT               = 0x83,
	/* [한국어] 0x83 — reservation 충돌(다른 호스트가 exclusive를 잡고 있음). */
	SPDK_NVME_SC_FORMAT_IN_PROGRESS                 = 0x84,
	/* [한국어] 0x84 — Format NVM 진행 중에 NS-impacting 명령 발행. */
	SPDK_NVME_SC_INVALID_VALUE_SIZE			= 0x85,
	/* [한국어] 0x85 — KV(Key-Value) value 길이 무효. */
	SPDK_NVME_SC_INVALID_KEY_SIZE			= 0x86,
	/* [한국어] 0x86 — KV key 길이 무효. */
	SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST		= 0x87,
	/* [한국어] 0x87 — KV Retrieve/Delete 시 키 없음. */
	SPDK_NVME_SC_UNRECOVERED_ERROR			= 0x88,
	/* [한국어] 0x88 — KV unrecovered error. */
	SPDK_NVME_SC_KEY_EXISTS				= 0x89,
	/* [한국어] 0x89 — KV Store 시 EXIST 옵션이지만 key가 이미 있음(또는 vice versa). */
};

/**
 * Command specific status codes
 */
/* [한국어] === enum spdk_nvme_command_specific_status_code === SCT=0x1(Command Specific) SC.
 * NVMe Base 2.0 §4.2.3.2 Tables 4-13/4-14. 특정 admin/I/O 명령에서만 발생하는 오류.
 * 0x00~0x7F=admin specific, 0xB8~0xBF=ZNS 등 I/O CS specific. */
enum spdk_nvme_command_specific_status_code {
	SPDK_NVME_SC_COMPLETION_QUEUE_INVALID		= 0x00,
	/* [한국어] 0x00 — CREATE_IO_SQ가 참조한 CQID가 유효한 CQ가 아님. */
	SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER		= 0x01,
	/* [한국어] 0x01 — DELETE_IO_SQ/CQ에 잘못된 QID. */
	SPDK_NVME_SC_INVALID_QUEUE_SIZE			= 0x02,
	/* [한국어] 0x02 — CREATE_IO_SQ/CQ의 QSIZE가 컨트롤러 한도(MQES) 초과. */
	SPDK_NVME_SC_ABORT_COMMAND_LIMIT_EXCEEDED	= 0x03,
	/* [한국어] 0x03 — Abort 명령이 컨트롤러 ACL(abort command limit) 초과. */
	/* 0x04 - reserved */
	SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED = 0x05,
	/* [한국어] 0x05 — AER 적재 수가 AERL+1 초과. */
	SPDK_NVME_SC_INVALID_FIRMWARE_SLOT		= 0x06,
	/* [한국어] 0x06 — Firmware Commit 시 잘못된 slot. */
	SPDK_NVME_SC_INVALID_FIRMWARE_IMAGE		= 0x07,
	/* [한국어] 0x07 — 다운로드된 firmware image 무효. */
	SPDK_NVME_SC_INVALID_INTERRUPT_VECTOR		= 0x08,
	/* [한국어] 0x08 — CREATE_IO_CQ의 IV가 invalid (>=MSI-X 벡터 수). */
	SPDK_NVME_SC_INVALID_LOG_PAGE			= 0x09,
	/* [한국어] 0x09 — Get Log Page LID 미지원. */
	SPDK_NVME_SC_INVALID_FORMAT			= 0x0a,
	/* [한국어] 0x0a — Format NVM의 LBAF/MSET/PI/PIL/SES 무효. */
	SPDK_NVME_SC_FIRMWARE_REQ_CONVENTIONAL_RESET    = 0x0b,
	/* [한국어] 0x0b — Firmware Commit 후 controller-level reset(CC.EN cycle) 필요. */
	SPDK_NVME_SC_INVALID_QUEUE_DELETION             = 0x0c,
	/* [한국어] 0x0c — DELETE_IO_CQ 발행 시 아직 사용 중인 SQ가 있음(SQ를 먼저 지워야 함). */
	SPDK_NVME_SC_FEATURE_ID_NOT_SAVEABLE            = 0x0d,
	/* [한국어] 0x0d — Set Features SAVE=1인데 해당 FID는 saveable 아님. */
	SPDK_NVME_SC_FEATURE_NOT_CHANGEABLE             = 0x0e,
	/* [한국어] 0x0e — Set Features 대상 FID가 read-only. */
	SPDK_NVME_SC_FEATURE_NOT_NAMESPACE_SPECIFIC     = 0x0f,
	/* [한국어] 0x0f — NS-specific인 줄 알고 NSID를 줬으나 해당 FID는 controller-scope. */
	SPDK_NVME_SC_FIRMWARE_REQ_NVM_RESET             = 0x10,
	/* [한국어] 0x10 — Firmware activation에 NVM Subsystem reset 필요. */
	SPDK_NVME_SC_FIRMWARE_REQ_RESET                 = 0x11,
	/* [한국어] 0x11 — Firmware activation에 controller reset 필요(임의 reset OK). */
	SPDK_NVME_SC_FIRMWARE_REQ_MAX_TIME_VIOLATION    = 0x12,
	/* [한국어] 0x12 — Firmware activation 최대 시간(MTFA) 초과 위험. */
	SPDK_NVME_SC_FIRMWARE_ACTIVATION_PROHIBITED     = 0x13,
	/* [한국어] 0x13 — Firmware activation 금지(slot RO 등). */
	SPDK_NVME_SC_OVERLAPPING_RANGE                  = 0x14,
	/* [한국어] 0x14 — DSM/Format LBA range가 서로 겹침. */
	SPDK_NVME_SC_NAMESPACE_INSUFFICIENT_CAPACITY    = 0x15,
	/* [한국어] 0x15 — NS Management Create 시 NVM 용량 부족. */
	SPDK_NVME_SC_NAMESPACE_ID_UNAVAILABLE           = 0x16,
	/* [한국어] 0x16 — NS ID가 컨트롤러 nn 한도 초과/이미 존재. */
	/* 0x17 - reserved */
	SPDK_NVME_SC_NAMESPACE_ALREADY_ATTACHED         = 0x18,
	/* [한국어] 0x18 — NS Attachment Attach인데 이미 attached. */
	SPDK_NVME_SC_NAMESPACE_IS_PRIVATE               = 0x19,
	/* [한국어] 0x19 — private NS를 다른 controller에 attach 시도. */
	SPDK_NVME_SC_NAMESPACE_NOT_ATTACHED             = 0x1a,
	/* [한국어] 0x1a — Detach 대상이 attach 안 됨. */
	SPDK_NVME_SC_THINPROVISIONING_NOT_SUPPORTED     = 0x1b,
	/* [한국어] 0x1b — NS Create thin provisioning 요청했으나 미지원. */
	SPDK_NVME_SC_CONTROLLER_LIST_INVALID            = 0x1c,
	/* [한국어] 0x1c — NS Attachment의 controller list 형식 invalid. */
	SPDK_NVME_SC_DEVICE_SELF_TEST_IN_PROGRESS	= 0x1d,
	/* [한국어] 0x1d — Device Self-Test 진행 중에 새 self-test/conflicting 명령. */
	SPDK_NVME_SC_BOOT_PARTITION_WRITE_PROHIBITED	= 0x1e,
	/* [한국어] 0x1e — Boot Partition write가 잠금 상태. */
	SPDK_NVME_SC_INVALID_CTRLR_ID			= 0x1f,
	/* [한국어] 0x1f — Virtualization Management의 controller ID invalid. */
	SPDK_NVME_SC_INVALID_SECONDARY_CTRLR_STATE	= 0x20,
	/* [한국어] 0x20 — secondary controller가 Online이 아님(SR-IOV). */
	SPDK_NVME_SC_INVALID_NUM_CTRLR_RESOURCES	= 0x21,
	/* [한국어] 0x21 — VQ/VI flexible resource 수 invalid. */
	SPDK_NVME_SC_INVALID_RESOURCE_ID		= 0x22,
	/* [한국어] 0x22 — Virtualization resource ID 무효. */
	SPDK_NVME_SC_SANITIZE_PROHIBITED		= 0x23,
	/* [한국어] 0x23 — Sanitize FW 정책상 차단. */
	SPDK_NVME_SC_ANA_GROUP_IDENTIFIER_INVALID	= 0x24,
	/* [한국어] 0x24 — ANA group ID invalid. */
	SPDK_NVME_SC_ANA_ATTACH_FAILED			= 0x25,
	/* [한국어] 0x25 — ANA group attach 실패. */
	SPDK_NVME_SC_INSUFFICIENT_CAPACITY		= 0x26,
	/* [한국어] 0x26 — NVM subsystem 전체 용량 부족. */
	SPDK_NVME_SC_NAMESPACE_ATTACH_LIMIT_EXCEEDED	= 0x27,
	/* [한국어] 0x27 — NS attach 한도(maxcna) 초과. */
	SPDK_NVME_SC_PROHIBIT_CMD_EXEC_NOT_SUPPORTED	= 0x28,
	/* [한국어] 0x28 — Command/Feature lockdown 미지원. */
	SPDK_NVME_SC_IOCS_NOT_SUPPORTED			= 0x29,
	/* [한국어] 0x29 — 요청 I/O Command Set 미지원. */
	SPDK_NVME_SC_IOCS_NOT_ENABLED			= 0x2a,
	/* [한국어] 0x2a — IOCS profile 미활성. */
	SPDK_NVME_SC_IOCS_COMBINATION_REJECTED		= 0x2b,
	/* [한국어] 0x2b — IOCS 조합 거부(NVM+ZNS 동시 등). */
	SPDK_NVME_SC_INVALID_IOCS			= 0x2c,
	/* [한국어] 0x2c — CSI(Command Set Identifier) 무효. */
	SPDK_NVME_SC_IDENTIFIER_UNAVAILABLE		= 0x2d,
	/* [한국어] 0x2d — Identify 시 요청 식별자 사용 불가. */

	SPDK_NVME_SC_STREAM_RESOURCE_ALLOCATION_FAILED	= 0x7f,
	/* [한국어] 0x7f — Streams Directive 자원 할당 실패. */
	SPDK_NVME_SC_CONFLICTING_ATTRIBUTES		= 0x80,
	/* [한국어] 0x80 — DSM 등 명령의 attribute 조합 충돌(IDR+IDW+AD 동시 등). */
	SPDK_NVME_SC_INVALID_PROTECTION_INFO		= 0x81,
	/* [한국어] 0x81 — PRACT/PRCHK 비트 조합이 NS의 PI type과 불일치. */
	SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE	= 0x82,
	/* [한국어] 0x82 — read-only LBA range에 write 시도. */
	SPDK_NVME_SC_CMD_SIZE_LIMIT_SIZE_EXCEEDED	= 0x83,
	/* [한국어] 0x83 — 명령 크기(MDTS) 초과. */

	SPDK_NVME_SC_ZONED_BOUNDARY_ERROR		= 0xb8,
	/* [한국어] 0xb8 — ZNS read/write가 zone 경계를 가로지름. */
	SPDK_NVME_SC_ZONE_IS_FULL			= 0xb9,
	/* [한국어] 0xb9 — ZNS write가 Full zone 대상. Reset 후 재사용해야 함. */
	SPDK_NVME_SC_ZONE_IS_READ_ONLY			= 0xba,
	/* [한국어] 0xba — Read-Only zone에 write. */
	SPDK_NVME_SC_ZONE_IS_OFFLINE			= 0xbb,
	/* [한국어] 0xbb — Offline zone 접근. */
	SPDK_NVME_SC_ZONE_INVALID_WRITE			= 0xbc,
	/* [한국어] 0xbc — Sequential Write Required zone에 write pointer 위치가 아닌 LBA write. */
	SPDK_NVME_SC_TOO_MANY_ACTIVE_ZONES		= 0xbd,
	/* [한국어] 0xbd — 컨트롤러 active zones 한도 초과. */
	SPDK_NVME_SC_TOO_MANY_OPEN_ZONES		= 0xbe,
	/* [한국어] 0xbe — open zones 한도 초과 — 일부 zone을 close 후 retry. */
	SPDK_NVME_SC_INVALID_ZONE_STATE_TRANSITION	= 0xbf,
	/* [한국어] 0xbf — Zone Mgmt Send에서 잘못된 상태 전이(Empty → Finish 등). */
};

/**
 * Media error status codes
 */
/* [한국어] === enum spdk_nvme_media_error_status_code === SCT=0x2(Media/DI) SC.
 * NVMe Base 2.0 §4.2.3.3 Table 4-15. 미디어 자체 에러 또는 PI(Protection Information) 검증 실패.
 * spdk_nvme_cpl_is_pi_error 매크로가 GUARD/APPTAG/REFTAG 세 종류를 묶어 검출. */
enum spdk_nvme_media_error_status_code {
	SPDK_NVME_SC_WRITE_FAULTS			= 0x80,
	/* [한국어] 0x80 — write가 미디어에 도달하지 못함(NAND program failure). */
	SPDK_NVME_SC_UNRECOVERED_READ_ERROR		= 0x81,
	/* [한국어] 0x81 — read가 ECC로도 복구 불가. media corruption. */
	SPDK_NVME_SC_GUARD_CHECK_ERROR			= 0x82,
	/* [한국어] 0x82 — PI Guard(CRC-16) 검증 실패. PRCHK_GUARD 활성화 필요. */
	SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR	= 0x83,
	/* [한국어] 0x83 — PI App Tag 검증 실패. PRCHK_APPTAG. */
	SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR		= 0x84,
	/* [한국어] 0x84 — PI Ref Tag 검증 실패. PRCHK_REFTAG. */
	SPDK_NVME_SC_COMPARE_FAILURE			= 0x85,
	/* [한국어] 0x85 — Compare 명령에서 호스트 데이터 ≠ 미디어 데이터. */
	SPDK_NVME_SC_ACCESS_DENIED			= 0x86,
	/* [한국어] 0x86 — TCG Opal 등 보안으로 read/write 차단됨. */
	SPDK_NVME_SC_DEALLOCATED_OR_UNWRITTEN_BLOCK     = 0x87,
	/* [한국어] 0x87 — DULBE=1 상태에서 dealloc/unwritten LBA read. NS dlfeat 참조. */
	SPDK_NVME_SC_END_TO_END_STORAGE_TAG_CHECK_ERROR	= 0x88,
	/* [한국어] 0x88 — NVMe 2.0 64B Guard 등 Storage Tag 검증 실패. */
};

/**
 * Path related status codes
 */
/* [한국어] === enum spdk_nvme_path_status_code === SCT=0x3(Path) SC.
 * NVMe Base 2.0 §4.2.3.4 Table 4-16. NVMe-oF 또는 multi-controller 환경에서 경로 자체 문제.
 * spdk_nvme_cpl_is_path_error/is_ana_error 매크로로 분기 — bdev_nvme multipath가 retry/
 * fail-over 결정에 사용. */
enum spdk_nvme_path_status_code {
	SPDK_NVME_SC_INTERNAL_PATH_ERROR		= 0x00,
	/* [한국어] 0x00 — 컨트롤러 내부 경로 오류. retry 가능성 있음. */
	SPDK_NVME_SC_ASYMMETRIC_ACCESS_PERSISTENT_LOSS	= 0x01,
	/* [한국어] 0x01 — ANA Persistent Loss. 호스트는 path 영구 제거. */
	SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE	= 0x02,
	/* [한국어] 0x02 — ANA Inaccessible. 일시적 접근 불가, retry로 다른 path. */
	SPDK_NVME_SC_ASYMMETRIC_ACCESS_TRANSITION	= 0x03,
	/* [한국어] 0x03 — ANA 상태 전이 중. retry 권장. */

	SPDK_NVME_SC_CONTROLLER_PATH_ERROR		= 0x60,
	/* [한국어] 0x60 — 컨트롤러 측 path error. */

	SPDK_NVME_SC_HOST_PATH_ERROR			= 0x70,
	/* [한국어] 0x70 — Host 측 path error (transport disconnect 등). bdev_nvme reconnector
	 * 가 이 SC를 받으면 NVMe-oF transport reset 시도. */
	SPDK_NVME_SC_ABORTED_BY_HOST			= 0x71,
	/* [한국어] 0x71 — host가 명시적으로 abort. */
};

#define SPDK_NVME_MAX_OPC 0xff
/* [한국어] NVMe opcode 8비트 폭의 최대값 — opcode supported bitmap의 크기 결정. */

/**
 * Admin opcodes
 */
/* [한국어] === Admin Command Opcode (CDW0.OPC) ===
 * NVMe Base 2.0 §5. Admin SQ(QID=0)에서만 발행 가능. opcode의 하위 2비트는 데이터 방향
 * (spdk_nvme_data_transfer)을 인코딩한다 — bit0=H2C, bit1=C2H. 따라서 IDENTIFY(0x06)
 * 처럼 bit1=1인 opcode는 컨트롤러→호스트 데이터 전송이 있고, FW_DOWNLOAD(0x11)처럼
 * bit0=1이면 호스트→컨트롤러. 0x80 이상은 NVM Command Set 관리 명령(Format/Sanitize 등). */
enum spdk_nvme_admin_opcode {
	SPDK_NVME_OPC_DELETE_IO_SQ			= 0x00,
	/* [한국어] 0x00 — Delete I/O Submission Queue. CDW10[QID]가 삭제할 SQ ID. 큐를 짝짓는
	 * 모든 미완료 요청은 abort 처리되며 호스트는 Delete IO CQ 발행 전에 SQ를 먼저 삭제. */
	SPDK_NVME_OPC_CREATE_IO_SQ			= 0x01,
	/* [한국어] 0x01 — Create I/O Submission Queue. CDW10[QID]+CDW10[QSIZE]+CDW11[CQID,
	 * QPRIO,PC,IV]+PRP1(SQ 메모리 주소). lib/nvme/nvme_pcie_common.c가 io qpair 생성 시 발행. */
	SPDK_NVME_OPC_GET_LOG_PAGE			= 0x02,
	/* [한국어] 0x02 — Get Log Page. CDW10[LID,LSP,RAE]+CDW10[NUMDL]+CDW11[NUMDU,LSI]+
	 * CDW12-13[LPOL/LPOU 64비트 오프셋]+CDW14[UUID]. SMART/Error/FW slot/Telemetry 등 모두
	 * 이 명령으로 수확. enum spdk_nvme_log_page 참조. 데이터 방향 = C2H. */
	/* 0x03 - reserved */
	SPDK_NVME_OPC_DELETE_IO_CQ			= 0x04,
	/* [한국어] 0x04 — Delete I/O Completion Queue. 해당 CQ를 사용하는 SQ가 모두 삭제된
	 * 후에만 발행 가능. 순서: DELETE_IO_SQ → DELETE_IO_CQ. */
	SPDK_NVME_OPC_CREATE_IO_CQ			= 0x05,
	/* [한국어] 0x05 — Create I/O Completion Queue. 순서: CREATE_IO_CQ → CREATE_IO_SQ.
	 * CDW11[IEN,IV] 인터럽트 활성/벡터 인덱스 설정(SPDK polled-mode는 IEN=0). */
	SPDK_NVME_OPC_IDENTIFY				= 0x06,
	/* [한국어] 0x06 — Identify. CDW10[CNS]+CDW10[CNTID]+CDW11[NVMSETID,CNS_SPECIFIC]+
	 * CDW14[UUID]. CNS=0x01(Identify Controller, 4096B), CNS=0x00(Active NS), CNS=0x02
	 * (NS list), CNS=0x05(I/O CS specific NS), CNS=0x06(I/O CS specific Controller, ZNS) 등.
	 * 데이터 방향 = C2H. 컨트롤러/네임스페이스 capability 발견의 핵심. */
	/* 0x07 - reserved */
	SPDK_NVME_OPC_ABORT				= 0x08,
	/* [한국어] 0x08 — Abort. CDW10[SQID,CID]로 대상 명령 식별. 컨트롤러는 best-effort로
	 * 취소 시도; CQE.CDW0[bit0]=1이면 abort 실패(이미 처리 중). */
	SPDK_NVME_OPC_SET_FEATURES			= 0x09,
	/* [한국어] 0x09 — Set Features. CDW10[FID,SAVE]+CDW11~CDW13(feature별 인코딩, union
	 * spdk_nvme_feat_* 참조). enum spdk_nvme_feat 참조. */
	SPDK_NVME_OPC_GET_FEATURES			= 0x0a,
	/* [한국어] 0x0a — Get Features. CDW10[FID,SEL]; SEL=0(current), 1(default), 2(saved),
	 * 3(supported capability). 결과는 CQE.CDW0와 옵션 데이터 페이로드(C2H). */
	/* 0x0b - reserved */
	SPDK_NVME_OPC_ASYNC_EVENT_REQUEST		= 0x0c,
	/* [한국어] 0x0c — Asynchronous Event Request. 호스트가 미리 N개(<=AERL+1)를 admin SQ에
	 * 적재. 컨트롤러는 임의 시점 이벤트(ANA 변경, SMART critical, NS attribute 변경 등)
	 * 발생 시 그 SQE를 완료시켜 통지(CQE.CDW0에 type/info/log_page). polled-mode 인터럽트
	 * 회피의 핵심 패턴. lib/nvme/nvme_ctrlr.c::nvme_ctrlr_construct_aer 참조. */
	SPDK_NVME_OPC_NS_MANAGEMENT			= 0x0d,
	/* [한국어] 0x0d — Namespace Management (NVMe 1.2+). CDW10[SEL]=0(Create)/1(Delete).
	 * Create는 호스트 메모리(4096B)에 ns_data를 채워 PRP/SGL로 전달. */
	/* 0x0e-0x0f - reserved */
	SPDK_NVME_OPC_FIRMWARE_COMMIT			= 0x10,
	/* [한국어] 0x10 — Firmware Commit. CDW10[FS,CA]로 firmware slot과 commit action 지정
	 * (struct spdk_nvme_fw_commit). */
	SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD		= 0x11,
	/* [한국어] 0x11 — Firmware Image Download. CDW10[NUMD]+CDW11[OFST] 전송 단위 분할.
	 * 데이터 방향 = H2C. 큰 펌웨어를 여러 번 나눠서 송신 후 COMMIT으로 적용. */

	SPDK_NVME_OPC_DEVICE_SELF_TEST			= 0x14,
	/* [한국어] 0x14 — Device Self-Test. CDW10[STC]=1(short)/2(extended). 결과는 Get Log
	 * Page LID=0x06으로 회수. */
	SPDK_NVME_OPC_NS_ATTACHMENT			= 0x15,
	/* [한국어] 0x15 — Namespace Attachment. CDW10[SEL]=0(Attach)/1(Detach). H2C로
	 * 컨트롤러 ID 리스트 전달. multi-controller subsystem용. */

	SPDK_NVME_OPC_KEEP_ALIVE			= 0x18,
	/* [한국어] 0x18 — Keep Alive. NVMe-oF에서 호스트가 주기적으로 발행해 컨트롤러가
	 * 호스트 생존 확인. KATO(Keep Alive Timeout) Feature로 주기 설정. */
	SPDK_NVME_OPC_DIRECTIVE_SEND			= 0x19,
	/* [한국어] 0x19 — Directive Send. Streams 등 directive 활성화/구성. */
	SPDK_NVME_OPC_DIRECTIVE_RECEIVE			= 0x1a,
	/* [한국어] 0x1a — Directive Receive. directive 상태 조회. */

	SPDK_NVME_OPC_VIRTUALIZATION_MANAGEMENT		= 0x1c,
	/* [한국어] 0x1c — Virtualization Management. SR-IOV/secondary controller resource 할당. */
	SPDK_NVME_OPC_NVME_MI_SEND			= 0x1d,
	/* [한국어] 0x1d — NVMe-MI Send. NVMe Management Interface 메시지 in-band 전달. */
	SPDK_NVME_OPC_NVME_MI_RECEIVE			= 0x1e,
	/* [한국어] 0x1e — NVMe-MI Receive. */

	SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG		= 0x7c,
	/* [한국어] 0x7c — Doorbell Buffer Config (가상화 환경). 호스트가 doorbell shadow 영역과
	 * EventIdx 영역을 컨트롤러에 등록해 hypervisor에 의한 트랩 비용 절감. */

	SPDK_NVME_OPC_FORMAT_NVM			= 0x80,
	/* [한국어] 0x80 — Format NVM. CDW10[LBAF,MSET,PI,PIL,SES] (struct spdk_nvme_format).
	 * LBA 포맷/PI 타입/secure erase 옵션 변경. 모든 데이터 파괴. */
	SPDK_NVME_OPC_SECURITY_SEND			= 0x81,
	/* [한국어] 0x81 — Security Send. CDW10[SECP=protocol,SPSP]. SECP=0x01이면 TCG Opal
	 * 토큰 스트림 전달(opal_spec.h). 데이터 방향 = H2C. */
	SPDK_NVME_OPC_SECURITY_RECEIVE			= 0x82,
	/* [한국어] 0x82 — Security Receive. SECP=0x01 + Level 0 Discovery로 디바이스 보안
	 * capability 회수 등. 데이터 방향 = C2H. */

	SPDK_NVME_OPC_SANITIZE				= 0x84,
	/* [한국어] 0x84 — Sanitize. struct spdk_nvme_sanitize (CDW10) — Block Erase/
	 * Crypto Erase/Overwrite. 진행 상태는 Get Log Page LID=0x81 (sanitize_status_log_page). */

	SPDK_NVME_OPC_GET_LBA_STATUS			= 0x86,
	/* [한국어] 0x86 — Get LBA Status (NVMe 1.4+). 특정 LBA range의 미디어 손상 여부 조회. */
	SPDK_NVME_OPC_VENDOR_SPECIFIC_START		= 0xC0,
	/* [한국어] 0xC0 — 0xC0~0xFF 영역은 vendor-specific. Intel, Samsung 등이 자체 진단/
	 * 펌웨어 명령에 사용 (예: nvme_intel.h가 이 영역의 LID/FID를 정의). */
};

/**
 * NVM command set opcodes
 */
/* [한국어] === NVM Command Set I/O Opcode (CDW0.OPC) ===
 * NVMe NVM Command Set Specification §3. I/O SQ(QID>=1)에서 발행. opcode 하위 2비트가
 * 데이터 방향 인코딩. */
enum spdk_nvme_nvm_opcode {
	SPDK_NVME_OPC_FLUSH				= 0x00,
	/* [한국어] 0x00 — Flush. 컨트롤러의 휘발성 캐시를 media에 동기화. NSID=0xFFFFFFFF로
	 * 발행 시 모든 NS flush(컨트롤러 지원 시). 데이터 전송 없음. */
	SPDK_NVME_OPC_WRITE				= 0x01,
	/* [한국어] 0x01 — Write. CDW10/11=SLBA(64), CDW12[NLB,FUA,LR,PRINFO,DTYPE], CDW13
	 * [DSM,DSPEC]. 데이터 방향=H2C. lib/nvme/nvme_ns_cmd.c::spdk_nvme_ns_cmd_write가 발행. */
	SPDK_NVME_OPC_READ				= 0x02,
	/* [한국어] 0x02 — Read. CDW10/11=SLBA(64), CDW12[NLB,FUA,LR,PRINFO]. 데이터=C2H. */
	/* 0x03 - reserved */
	SPDK_NVME_OPC_WRITE_UNCORRECTABLE		= 0x04,
	/* [한국어] 0x04 — Write Uncorrectable. 지정 LBA를 의도적으로 "uncorrectable"로 표시
	 * (RAID 시뮬레이션/테스트). */
	SPDK_NVME_OPC_COMPARE				= 0x05,
	/* [한국어] 0x05 — Compare. 호스트 버퍼와 미디어 데이터를 컨트롤러가 비교; 불일치 시
	 * status code COMPARE_FAILURE(0x85, SCT=Media). FUSE_FIRST와 짝지어
	 * Compare-and-Write atomic 구현. */
	/* 0x06-0x07 - reserved */
	SPDK_NVME_OPC_WRITE_ZEROES			= 0x08,
	/* [한국어] 0x08 — Write Zeroes. 데이터 전송 없이 컨트롤러가 지정 LBA range를 0으로
	 * 채움 (실제로는 thin-provision/dealloc 처리). bdev unmap의 fallback 또는 zero-fill. */
	SPDK_NVME_OPC_DATASET_MANAGEMENT		= 0x09,
	/* [한국어] 0x09 — Dataset Management (TRIM/Discard). CDW10[NR=range count-1]+CDW11
	 * [Attribute=AD/IDR/IDW]+PRP1=struct spdk_nvme_dsm_range[N]. SSD에 LBA 영역 dealloc
	 * 알려서 GC/wear-leveling 효율화. */

	SPDK_NVME_OPC_VERIFY				= 0x0c,
	/* [한국어] 0x0c — Verify (NVMe 1.4+). 호스트 데이터 전송 없이 컨트롤러가 LBA range의
	 * media integrity 검증(읽어서 PI 검사). */
	SPDK_NVME_OPC_RESERVATION_REGISTER		= 0x0d,
	/* [한국어] 0x0d — Reservation Register. host_id를 NS reservation table에 등록/등록 해제. */
	SPDK_NVME_OPC_RESERVATION_REPORT		= 0x0e,
	/* [한국어] 0x0e — Reservation Report. 현재 NS의 모든 등록자/소유자 정보 회수(C2H). */

	SPDK_NVME_OPC_RESERVATION_ACQUIRE		= 0x11,
	/* [한국어] 0x11 — Reservation Acquire. 등록된 host가 reservation 획득(쓰기 독점 등). */
	SPDK_NVME_OPC_IO_MANAGEMENT_RECEIVE		= 0x12,
	/* [한국어] 0x12 — I/O Management Receive (FDP TP4146). FDP RUH 상태 조회. */
	SPDK_NVME_OPC_RESERVATION_RELEASE		= 0x15,
	/* [한국어] 0x15 — Reservation Release. reservation 해제 또는 모든 등록자 clear. */

	SPDK_NVME_OPC_COPY				= 0x19,
	/* [한국어] 0x19 — Copy (NVMe 2.0). 호스트 데이터 미경유, 컨트롤러 내부에서 source LBA
	 * range를 dest LBA range로 복사(struct spdk_nvme_scc_source_range[] PRP). */
	SPDK_NVME_OPC_IO_MANAGEMENT_SEND		= 0x1D,
	/* [한국어] 0x1D — I/O Management Send (FDP). RUH update. */
};

/**
 * Zoned Namespace command set opcodes
 *
 * In addition to the opcodes of the NVM command set, the Zoned Namespace
 * command set supports the following opcodes.
 */
/* [한국어] === ZNS Command Set 추가 opcode (NVM CS opcode와 별개로 추가) ===
 * NVMe ZNS Command Set Specification §3.4. nvme_zns.h에서 사용자 API로 노출. */
enum spdk_nvme_zns_opcode {
	SPDK_NVME_OPC_ZONE_MGMT_SEND			= 0x79,
	/* [한국어] 0x79 — Zone Management Send. CDW13[ZSA] = Open/Close/Finish/Reset/Offline/
	 * Set Descriptor Extension. zone state machine 전이 제어. */
	SPDK_NVME_OPC_ZONE_MGMT_RECV			= 0x7a,
	/* [한국어] 0x7a — Zone Management Receive. CDW13[ZRA]=0(Report)/1(Extended Report).
	 * 페이로드(C2H)는 spdk_nvme_zns_zone_report + zone_desc 배열. */
	SPDK_NVME_OPC_ZONE_APPEND			= 0x7d,
	/* [한국어] 0x7d — Zone Append. ZSLBA(zone start)만 지정; 컨트롤러가 실제 write LBA
	 * 결정 후 CQE.CDW0/1로 회신. write pointer 추적 불필요한 multi-producer. */
};

/**
 * Data transfer (bits 1:0) of an NVMe opcode.
 *
 * \sa spdk_nvme_opc_get_data_transfer
 */
/* [한국어] === Data Transfer 방향 (opcode 하위 2비트) === NVMe Base 2.0 §5. opcode 인코딩
 * 규칙으로 bit0=H2C 존재, bit1=C2H 존재를 표시. SPDK는 spdk_nvme_opc_get_data_transfer()
 * 인라인으로 추출해 PRP/SGL 빌드 시 read/write 방향을 결정. */
enum spdk_nvme_data_transfer {
	/** Opcode does not transfer data */
	SPDK_NVME_DATA_NONE				= 0,
	/* [한국어] 0 — 데이터 전송 없음 (Flush, Verify, Abort 등). */
	/** Opcode transfers data from host to controller (e.g. Write) */
	SPDK_NVME_DATA_HOST_TO_CONTROLLER		= 1,
	/* [한국어] 1 — H2C (Write, FW Download, Security Send, DSM 등). */
	/** Opcode transfers data from controller to host (e.g. Read) */
	SPDK_NVME_DATA_CONTROLLER_TO_HOST		= 2,
	/* [한국어] 2 — C2H (Read, Identify, Get Log Page, Security Receive 등). */
	/** Opcode transfers data both directions */
	SPDK_NVME_DATA_BIDIRECTIONAL			= 3
	/* [한국어] 3 — 양방향 (특수 명령 일부). */
};

/**
 * Extract the Data Transfer bits from an NVMe opcode.
 *
 * This determines whether a command requires a data buffer and
 * which direction (host to controller or controller to host) it is
 * transferred.
 */
static inline enum spdk_nvme_data_transfer spdk_nvme_opc_get_data_transfer(uint8_t opc)
{
	return (enum spdk_nvme_data_transfer)(opc & 3);
}

static inline uint32_t
spdk_nvme_bytes_to_numd(uint32_t len)
{
	return (len >> 2) - 1;
}

#pragma pack(push, 1)
struct spdk_nvme_host_behavior {
	uint8_t acre;
	uint8_t etdas;
	uint8_t lbafee;
	uint8_t reserved[509];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_host_behavior) == 512, "Incorrect size");
#pragma pack(pop)

/**
 * Supported FDP event descriptor
 */
/* [한국어] === struct spdk_nvme_fdp_event_desc === FDP TP4146 §5.7. FID=0x1E feat_fdp_events
 * 의 데이터 페이로드 단일 entry(2B). 호스트가 어떤 FDP event 종류를 enable/disable할지 지정.
 * SPDK에서는 module/bdev/nvme 가 FDP feature 활성 시 사용. */
struct spdk_nvme_fdp_event_desc {
	/* FDP Event type */
	uint8_t fdp_etype;
	/* [한국어] [byte 0] fdp_etype — event 종류 (enum spdk_nvme_fdp_event_type). 0x0~0x3=host event,
	 * 0x80~0x81=controller event. */

	/* FDP event type attributes */
	union {
		/* [한국어] [byte 1] FDPETA — FDP Event Type Attributes (1B). */
		uint8_t raw;
		struct {
			/*  FDP event enabled */
			uint8_t fdp_ee   : 1;
			/* [한국어] [0] fdp_ee — 1=이 event 종류 활성, 0=비활성. */
			uint8_t reserved : 7;
			/* [한국어] [7:1] reserved. */
		} bits;
	} fdpeta;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fdp_event_desc) == 2, "Incorrect size");

/**
 * Reclaim unit handle status descriptor
 */
/* [한국어] === struct spdk_nvme_fdp_ruhs_desc === FDP TP4146. I/O Mgmt Receive(opcode 0x12)
 * MO=RUHS(0x01) 응답의 한 RUH 상태 entry(32B). 호스트는 ruamw(미디어 write 잔여량)를 보고 같은
 * placement handle의 RUH 교체 시점을 결정. */
struct spdk_nvme_fdp_ruhs_desc {
	/* Placement Identifier */
	uint16_t pid;
	/* [한국어] [bytes 0-1] PID — Placement Identifier. write 명령의 dspec과 매칭. */

	/* Reclaim Unit Handle Identifier */
	uint16_t ruhid;
	/* [한국어] [bytes 2-3] RUHID — 실제 RUH ID. */

	/* Estimated Active Reclaim Unit Time Remaining */
	uint32_t earutr;
	/* [한국어] [bytes 4-7] EARUTR — Estimated Active Reclaim Unit Time Remaining (sec). RUH가
	 * active로 남아 있을 추정 시간. 호스트는 이 값으로 "RUH 교체 직전 finalize" 정책 결정. */

	/* Reclaim Unit Available Media Writes */
	uint64_t ruamw;
	/* [한국어] [bytes 8-15] RUAMW — Reclaim Unit이 받을 수 있는 추가 media write LBA 수. */

	uint8_t reserved[16];
	/* [한국어] [bytes 16-31] reserved. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fdp_ruhs_desc) == 32, "Incorrect size");

/**
 * Reclaim unit handle status
 */
/* [한국어] === struct spdk_nvme_fdp_ruhs === RUHS 응답 헤더(16B) + 가변 길이 desc 배열.
 * I/O Mgmt Receive(opc=0x12) MO=0x01의 데이터 페이로드 시작. */
struct spdk_nvme_fdp_ruhs {
	uint8_t reserved[14];
	/* [한국어] [bytes 0-13] reserved. */

	/* Number of Reclaim Unit Handle Status Descriptors */
	uint16_t nruhsd;
	/* [한국어] [bytes 14-15] NRUHSD — 뒤따르는 ruhs_desc 엔트리 수. */

	struct spdk_nvme_fdp_ruhs_desc ruhs_desc[];
	/* [한국어] flexible array — NRUHSD 개의 RUH descriptor. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fdp_ruhs) == 16, "Incorrect size");

/**
 * Management operation to perform for IO management receive
 */
enum spdk_nvme_fdp_mgmt_recv_mo {
	SPDK_NVME_FDP_IO_MGMT_RECV_NA		= 0x00,
	SPDK_NVME_FDP_IO_MGMT_RECV_RUHS		= 0x01,
	/* 0x02-0xFE - reserved */
	SPDK_NVME_FDP_IO_MGMT_RECV_VS		= 0xFF,
};

/**
 * Management operation to perform for IO management send
 */
enum spdk_nvme_fdp_mgmt_send_mo {
	SPDK_NVME_FDP_IO_MGMT_SEND_NA		= 0x00,
	SPDK_NVME_FDP_IO_MGMT_SEND_RUHU		= 0x01,
	/* 0x02-0xFE - reserved */
	SPDK_NVME_FDP_IO_MGMT_SEND_VS		= 0xFF,
};

/* [한국어] === Feature Identifier (FID) — Get/Set Features (CDW10[FID]) ===
 * NVMe Base 2.0 §5.27/§5.15 Tables. 컨트롤러의 동작 모드/임계값/큐 구성을 RW로 제어.
 * 각 FID마다 CDW11(~CDW13)의 비트 인코딩이 다르며 union spdk_nvme_feat_* 가 정의한다.
 * Set: opcode=0x09, CDW10[FID,SAVE]+CDW11~13. Get: opcode=0x0A, CDW10[FID,SEL]. */
enum spdk_nvme_feat {
	/* 0x00 - reserved */

	/** cdw11 layout defined by \ref spdk_nvme_feat_arbitration */
	SPDK_NVME_FEAT_ARBITRATION				= 0x01,
	/* [한국어] 0x01 — Arbitration. WRR(weighted round robin) 가중치 LPW/MPW/HPW와
	 * Arbitration Burst(AB) 설정. CC.AMS=1일 때만 의미. */
	/** cdw11 layout defined by \ref spdk_nvme_feat_power_management */
	SPDK_NVME_FEAT_POWER_MANAGEMENT				= 0x02,
	/* [한국어] 0x02 — Power Management. PS(power state) 0~31 + WH(workload hint). */
	/** cdw11 layout defined by \ref spdk_nvme_feat_lba_range_type */
	SPDK_NVME_FEAT_LBA_RANGE_TYPE				= 0x03,
	/* [한국어] 0x03 — LBA Range Type. NS 내 partition별 type 분류 (data/cache/swap 등). */
	/** cdw11 layout defined by \ref spdk_nvme_feat_temperature_threshold */
	SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD			= 0x04,
	/* [한국어] 0x04 — Temperature Threshold. THSEL=0(over)/1(under) + TMPSEL(센서 인덱스)
	 * + TMPTH(임계 온도 켈빈). 초과/미만 시 AER 발생 가능. */
	/** cdw11 layout defined by \ref spdk_nvme_feat_error_recovery */
	SPDK_NVME_FEAT_ERROR_RECOVERY				= 0x05,
	/* [한국어] 0x05 — Error Recovery. TLER(Time Limited Error Recovery, 100ms 단위) +
	 * DULBE(Deallocated/Unwritten Logical Block Error Enable). */
	/** cdw11 layout defined by \ref spdk_nvme_feat_volatile_write_cache */
	SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE			= 0x06,
	/* [한국어] 0x06 — Volatile Write Cache. WCE(write cache enable) 1비트. 끄면 모든 write가
	 * media 도달 후에만 완료(latency↑, 안전↑). bdev_nvme의 'enable_caching' 옵션과 매핑. */
	/** cdw11 layout defined by \ref spdk_nvme_feat_number_of_queues */
	SPDK_NVME_FEAT_NUMBER_OF_QUEUES				= 0x07,
	/* [한국어] 0x07 — Number of Queues. CDW11=요청 SQ/CQ 수(0-based), CQE.CDW0=실제 할당.
	 * lib/nvme/nvme_ctrlr.c가 init 시 발행해 max I/O qpair 수 결정. */
	/** cdw11 layout defined by \ref spdk_nvme_feat_interrupt_coalescing */
	SPDK_NVME_FEAT_INTERRUPT_COALESCING			= 0x08,
	/* [한국어] 0x08 — Interrupt Coalescing. THR(threshold)+TIME(100us 단위). polled-mode
	 * 에서는 의미 없음. */
	/** cdw11 layout defined by \ref spdk_nvme_feat_interrupt_vector_configuration */
	SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION		= 0x09,
	/* [한국어] 0x09 — Interrupt Vector Config. 벡터별 CD(Coalescing Disable). */
	/** cdw11 layout defined by \ref spdk_nvme_feat_write_atomicity */
	SPDK_NVME_FEAT_WRITE_ATOMICITY				= 0x0A,
	/* [한국어] 0x0A — Write Atomicity Normal. DN(Disable Normal) AWUN/AWUPF 사용 여부. */
	/** cdw11 layout defined by \ref spdk_nvme_feat_async_event_configuration */
	SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION		= 0x0B,
	/* [한국어] 0x0B — Async Event Config. 어떤 종류의 AER을 허용할지(SMART critical/
	 * NS attribute changed/FW activation/telemetry 등) 비트마스크. */
	/** cdw11 layout defined by \ref spdk_nvme_feat_autonomous_power_state_transition */
	SPDK_NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION	= 0x0C,
	/* [한국어] 0x0C — Autonomous Power State Transition. APSTE 활성화 시 컨트롤러가
	 * idle 시간에 따라 자동 power state 전환. */
	/** cdw11 layout defined by \ref spdk_nvme_feat_host_mem_buffer */
	SPDK_NVME_FEAT_HOST_MEM_BUFFER				= 0x0D,
	/* [한국어] 0x0D — Host Memory Buffer (HMB). 호스트가 컨트롤러에 DMA-able 메모리를
	 * 할당해줘 DRAMless SSD가 FTL 매핑 캐시로 사용. SPDK는 hugepage 영역을 EHM=1로 등록. */
	SPDK_NVME_FEAT_TIMESTAMP				= 0x0E,
	/* [한국어] 0x0E — Timestamp. 호스트가 ms 단위 timestamp를 컨트롤러에 동기화 — error
	 * log entry 의 timestamp 필드에 사용. */
	/** cdw11 layout defined by \ref spdk_nvme_feat_keep_alive_timer */
	SPDK_NVME_FEAT_KEEP_ALIVE_TIMER				= 0x0F,
	/* [한국어] 0x0F — Keep Alive Timer. KATO(ms). NVMe-oF에서 호스트 dead 검출 타임아웃. */
	/** cdw11 layout defined by \ref spdk_nvme_feat_host_controlled_thermal_management */
	SPDK_NVME_FEAT_HOST_CONTROLLED_THERMAL_MANAGEMENT	= 0x10,
	/* [한국어] 0x10 — HCTM. TMT1(non-critical)/TMT2(critical) 임계 온도. */
	/** cdw11 layout defined by \ref spdk_nvme_feat_non_operational_power_state_config */
	SPDK_NVME_FEAT_NON_OPERATIONAL_POWER_STATE_CONFIG	= 0x11,
	/* [한국어] 0x11 — Non-Op Power State Config. NOPPME 활성화 시 non-operational PS도
	 * APST 전환에 포함. */

	SPDK_NVME_FEAT_READ_RECOVERY_LEVEL_CONFIG		= 0x12,
	SPDK_NVME_FEAT_PREDICTABLE_LATENCY_MODE_CONFIG		= 0x13,
	SPDK_NVME_FEAT_PREDICTABLE_LATENCY_MODE_WINDOW		= 0x14,
	SPDK_NVME_FEAT_LBA_STATUS_INFORMATION_ATTRIBUTES	= 0x15,
	/** data buffer layout  defined by \ref spdk_nvme_host_behavior */
	SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT			= 0x16,
	SPDK_NVME_FEAT_SANITIZE_CONFIG				= 0x17,
	SPDK_NVME_FEAT_ENDURANCE_GROUP_EVENT			= 0x18,
	SPDK_NVME_FEAT_IO_COMMAND_SET_PROFILE			= 0x19,
	SPDK_NVME_FEAT_SPINUP_CONTROL				= 0x1A,
	/* 0x1B-0x1C - reserved */

	/**
	 * cdw11 layout defined by \ref spdk_nvme_feat_fdp_cdw11
	 * cdw12 layout defined by \ref spdk_nvme_feat_fdp_cdw12
	 */
	SPDK_NVME_FEAT_FDP					= 0x1D,

	/**
	 * cdw11 layout defined by \ref spdk_nvme_feat_fdp_events_cdw11
	 * cdw12 layout defined by \ref spdk_nvme_feat_fdp_events_cdw12
	 * data layout defined by \ref spdk_nvme_fdp_event_desc
	 */
	SPDK_NVME_FEAT_FDP_EVENTS				= 0x1E,

	/* 0x1F-0x77 - reserved */
	/* 0x78-0x7C - NVMe-MI features */
	SPDK_NVME_FEAT_ENHANCED_CONTROLLER_METADATA		= 0x7D,
	SPDK_NVME_FEAT_CONTROLLER_METADATA			= 0x7E,
	SPDK_NVME_FEAT_NAMESPACE_METADATA			= 0x7F,

	/** cdw11 layout defined by \ref spdk_nvme_feat_software_progress_marker */
	SPDK_NVME_FEAT_SOFTWARE_PROGRESS_MARKER			= 0x80,

	/** cdw11 layout defined by \ref spdk_nvme_feat_host_identifier */
	SPDK_NVME_FEAT_HOST_IDENTIFIER				= 0x81,
	/** cdw11 layout defined by \ref spdk_nvme_feat_reservation_notification_mask */
	SPDK_NVME_FEAT_HOST_RESERVE_MASK			= 0x82,
	/** cdw11 layout defined by \ref spdk_nvme_feat_reservation_persistence */
	SPDK_NVME_FEAT_HOST_RESERVE_PERSIST			= 0x83,
	SPDK_NVME_FEAT_NAMESPACE_WRITE_PROTECTION_CONFIG	= 0x84,

	/* 0x85-0xBF - command set specific (reserved) */

	/* 0xC0-0xFF - vendor specific */
};

/** Bit set of attributes for DATASET MANAGEMENT commands. */
enum spdk_nvme_dsm_attribute {
	SPDK_NVME_DSM_ATTR_INTEGRAL_READ		= 0x1,
	SPDK_NVME_DSM_ATTR_INTEGRAL_WRITE		= 0x2,
	SPDK_NVME_DSM_ATTR_DEALLOCATE			= 0x4,
};

struct spdk_nvme_power_state {
	uint16_t mp;				/* bits 15:00: maximum power */

	uint8_t reserved1;

	uint8_t mps		: 1;		/* bit 24: max power scale */
	uint8_t nops		: 1;		/* bit 25: non-operational state */
	uint8_t reserved2	: 6;

	uint32_t enlat;				/* bits 63:32: entry latency in microseconds */
	uint32_t exlat;				/* bits 95:64: exit latency in microseconds */

	uint8_t rrt		: 5;		/* bits 100:96: relative read throughput */
	uint8_t reserved3	: 3;

	uint8_t rrl		: 5;		/* bits 108:104: relative read latency */
	uint8_t reserved4	: 3;

	uint8_t rwt		: 5;		/* bits 116:112: relative write throughput */
	uint8_t reserved5	: 3;

	uint8_t rwl		: 5;		/* bits 124:120: relative write latency */
	uint8_t reserved6	: 3;

	uint16_t idlp;				/* bits 143:128: idle power */

	uint8_t reserved7	: 6;
	uint8_t ips		: 2;		/* bits 151:150: idle power scale */

	uint8_t reserved8;

	uint16_t actp;				/* bits 175:160: active power */

	uint8_t apw		: 3;		/* bits 178:176: active power workload */
	uint8_t reserved9	: 3;
	uint8_t aps		: 2;		/* bits 183:182: active power scale */

	uint8_t reserved10[9];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_power_state) == 32, "Incorrect size");

/** Identify command CNS value */
/* [한국어] === Identify(0x06) CNS — Controller or Namespace Structure 선택자 ===
 * NVMe Base 2.0 §5.17. Identify 명령 CDW10 하위 8비트로 어떤 4096B 데이터 구조체를
 * 받을지 결정. lib/nvme/nvme_ctrlr.c가 attach 시 IDENTIFY_CTRLR(0x01) → ACTIVE_NS_LIST
 * (0x02) → NS_IOCS(0x05)/NS(0x00) → NS_ID_DESCRIPTOR_LIST(0x03) 순으로 발행해 컨트롤러/
 * 네임스페이스 식별 정보를 캐시한다. CSI(Command Set Identifier)별 변형은 IOCS suffix를
 * 가진 CNS(0x05/0x06/0x07/0x1a/0x1b)를 사용. */
enum spdk_nvme_identify_cns {
	/** Identify namespace indicated in CDW1.NSID */
	SPDK_NVME_IDENTIFY_NS				= 0x00,
	/* [한국어] 0x00 — Identify Namespace (NVM CS 한정). 결과는 struct spdk_nvme_ns_data
	 * (4096B). NSID는 CDW1로 지정. */

	/** Identify controller */
	SPDK_NVME_IDENTIFY_CTRLR			= 0x01,
	/* [한국어] 0x01 — Identify Controller. 결과는 struct spdk_nvme_ctrlr_data (4096B).
	 * VID/SSVID/SN/MN/FR/RAB/IEEE/CMIC/MDTS/CNTLID/VER/RTD3R/RTD3E/OAES/CTRATT/RRLS/...
	 * 모든 컨트롤러 capability 한 번에 회수. */

	/** List active NSIDs greater than CDW1.NSID */
	SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST		= 0x02,
	/* [한국어] 0x02 — Active Namespace List. 결과는 uint32_t[1024]. CDW1.NSID보다 큰
	 * NSID들을 오름차순으로 반환(0으로 종료). */

	/** List namespace identification descriptors */
	SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST	= 0x03,
	/* [한국어] 0x03 — Namespace Identification Descriptor list. EUI64/NGUID/UUID/CSI 등
	 * 영구 식별자 4096B. struct spdk_nvme_ns_id_desc 가변 길이 배열. */

	/** Identify namespace indicated in CDW1.NSID, specific to CDW11.CSI */
	SPDK_NVME_IDENTIFY_NS_IOCS			= 0x05,
	/* [한국어] 0x05 — I/O CS specific Identify Namespace. CDW11.CSI=0x02면 ZNS namespace
	 * (struct spdk_nvme_zns_ns_data 4096B), CSI=0x00이면 NVM CS의 추가 식별. */

	/** Identify controller, specific to CDW11.CSI */
	SPDK_NVME_IDENTIFY_CTRLR_IOCS			= 0x06,
	/* [한국어] 0x06 — I/O CS specific Identify Controller. CSI=0x02면 spdk_nvme_zns_ctrlr_data
	 * (ZASL 등 ZNS controller 한도). nvme_zns.h::spdk_nvme_zns_ctrlr_get_data가 이 캐시 반환. */

	/** List active NSIDs greater than CDW1.NSID, specific to CDW11.CSI */
	SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST_IOCS		= 0x07,
	/* [한국어] 0x07 — CSI 한정 Active NS list. CSI별 NSID 필터. */

	/** I/O Command Set Independent Identify Namespace */
	SPDK_NVME_IDENTIFY_NS_IOCS_INDEPENDENT		= 0x08,
	/* [한국어] 0x08 — CS-independent NS identify (NVMe 2.0). LBSTM/NSFEAT/NMIC 등 CS와
	 * 무관한 공통 속성. */

	/** List allocated NSIDs greater than CDW1.NSID */
	SPDK_NVME_IDENTIFY_ALLOCATED_NS_LIST		= 0x10,
	/* [한국어] 0x10 — Allocated(생성됐지만 attach 안 된 것 포함) NS list. NS Management 후
	 * 사용. */

	/** Identify namespace if CDW1.NSID is allocated */
	SPDK_NVME_IDENTIFY_NS_ALLOCATED			= 0x11,
	/* [한국어] 0x11 — allocated NS identify. NS_MANAGEMENT(Create) 직후 사용. */

	/** Get list of controllers starting at CDW10.CNTID that are attached to CDW1.NSID */
	SPDK_NVME_IDENTIFY_NS_ATTACHED_CTRLR_LIST	= 0x12,
	/* [한국어] 0x12 — 특정 NSID에 attach된 controller ID list. multi-controller subsystem용. */

	/** Get list of controllers starting at CDW10.CNTID */
	SPDK_NVME_IDENTIFY_CTRLR_LIST			= 0x13,
	/* [한국어] 0x13 — subsystem 내 모든 controller ID list. */

	/** Get primary controller capabilities structure */
	SPDK_NVME_IDENTIFY_PRIMARY_CTRLR_CAP		= 0x14,
	/* [한국어] 0x14 — Primary Controller Capabilities (struct spdk_nvme_primary_ctrl_capabilities,
	 * 4096B). SR-IOV PF의 VQ/VI 자원 풀 정보. */

	/** Get secondary controller list */
	SPDK_NVME_IDENTIFY_SECONDARY_CTRLR_LIST		= 0x15,
	/* [한국어] 0x15 — Secondary Controller List (VF 컨트롤러 32B 엔트리 배열). */

	/** Get UUID List */
	SPDK_NVME_IDENTIFY_UUID_LIST			= 0x17,
	/* [한국어] 0x17 — Vendor-specific UUID list. */

	/** List allocated NSIDs greater than CDW1.NSID, specific to CDW11.CSI */
	SPDK_NVME_IDENTIFY_ALLOCATED_NS_LIST_IOCS	= 0x1a,
	/* [한국어] 0x1a — CSI-filtered Allocated NS list. */

	/** Identify namespace if CDW1.NSID is allocated, specific to CDWD11.CSI */
	SPDK_NVME_IDENTIFY_NS_ALLOCATED_IOCS		= 0x1b,
	/* [한국어] 0x1b — CSI-filtered allocated NS identify. */

	/** Identify I/O Command Sets */
	SPDK_NVME_IDENTIFY_IOCS				= 0x1c,
	/* [한국어] 0x1c — I/O Command Sets supported (struct spdk_nvme_iocs_vector[]).
	 * NSID 별로 어떤 CSI 조합이 enable 가능한지 반환. CC.CSS=0x6(IOCS 모드)에서 사용. */
};

/** NVMe over Fabrics controller model */
enum spdk_nvmf_ctrlr_model {
	/** NVM subsystem uses dynamic controller model */
	SPDK_NVMF_CTRLR_MODEL_DYNAMIC			= 0,

	/** NVM subsystem uses static controller model */
	SPDK_NVMF_CTRLR_MODEL_STATIC			= 1,
};

#define SPDK_NVME_CTRLR_SN_LEN	20
#define SPDK_NVME_CTRLR_MN_LEN	40
#define SPDK_NVME_CTRLR_FR_LEN	8
#define SPDK_NVME_CTRLR_MEGCAP_LEN 16

/** Identify Controller data sgls.supported values */
enum spdk_nvme_sgls_supported {
	/** SGLs are not supported */
	SPDK_NVME_SGLS_NOT_SUPPORTED			= 0,

	/** SGLs are supported with no alignment or granularity requirement. */
	SPDK_NVME_SGLS_SUPPORTED			= 1,

	/** SGLs are supported with a DWORD alignment and granularity requirement. */
	SPDK_NVME_SGLS_SUPPORTED_DWORD_ALIGNED		= 2,
};

/** Identify Controller data vwc.flush_broadcast values */
enum spdk_nvme_flush_broadcast {
	/** Support for NSID=FFFFFFFFh with Flush is not indicated. */
	SPDK_NVME_FLUSH_BROADCAST_NOT_INDICATED		= 0,

	/* 01b: Reserved */

	/** Flush does not support NSID set to FFFFFFFFh. */
	SPDK_NVME_FLUSH_BROADCAST_NOT_SUPPORTED		= 2,

	/** Flush supports NSID set to FFFFFFFFh. */
	SPDK_NVME_FLUSH_BROADCAST_SUPPORTED		= 3
};

#define SPDK_NVME_MAXDNA_FIELD_SIZE 16
#define SPDK_NVME_NQN_FIELD_SIZE 256

/** Identify Controller data NVMe over Fabrics-specific fields */
struct spdk_nvme_cdata_nvmf_specific {
	/** I/O queue command capsule supported size (16-byte units) */
	uint32_t	ioccsz;

	/** I/O queue response capsule supported size (16-byte units) */
	uint32_t	iorcsz;

	/** In-capsule data offset (16-byte units) */
	uint16_t	icdoff;

	/** Controller attributes */
	struct {
		/** Controller model: \ref spdk_nvmf_ctrlr_model */
		uint8_t	ctrlr_model : 1;
		uint8_t reserved : 7;
	} ctrattr;

	/** Maximum SGL block descriptors (0 = no limit) */
	uint8_t		msdbd;

	/** Optional fabric commands supported */
	struct {
		/** Support disconnect command and individual I/O queue deletion */
		uint16_t disconnect : 1;
		uint16_t reserved : 15;
	} ofcs;

	uint8_t		reserved[242];
};

/** Identify Controller data SGL support */
struct spdk_nvme_cdata_sgls {
	uint32_t	supported : 2;
	uint32_t	keyed_sgl : 1;
	uint32_t	reserved1 : 13;
	uint32_t	bit_bucket_descriptor : 1;
	uint32_t	metadata_pointer : 1;
	uint32_t	oversized_sgl : 1;
	uint32_t	metadata_address : 1;
	uint32_t	sgl_offset : 1;
	uint32_t	transport_sgl : 1;
	uint32_t	reserved2 : 10;
};

/** Identify Controller data Optional NVM Command Support */
struct spdk_nvme_cdata_oncs {
	union {
		uint16_t	raw;
		struct {
			/** Compare Command Support */
			uint16_t	nvmcmps: 1;

			/** Write Uncorrectable Support Variants */
			uint16_t	nvmwusv: 1;

			/** Dataset Management Support Variants */
			uint16_t	nvmdsmsv: 1;

			/** Write Zeroes Support Variants */
			uint16_t	nvmwzsv: 1;

			/** Save and Select Feature Support */
			uint16_t	ssfs: 1;

			/** Reservations Support */
			uint16_t	reservs: 1;

			/** Timestamp Support */
			uint16_t	tss: 1;

			/** Verify Support */
			uint16_t	nvmvfys: 1;

			/** Copy Support */
			uint16_t	nvmcpys: 1;

			uint16_t	rsvd : 7;
		};

		/** Old bit names are deprecated and will be removed in 26.05 release */
		struct {
			uint16_t	compare : 1;
			uint16_t	write_unc : 1;
			uint16_t	dsm: 1;
			uint16_t	write_zeroes: 1;
			uint16_t	set_features_save: 1;
			uint16_t	reservations: 1;
			uint16_t	timestamp: 1;
			uint16_t	verify: 1;
			uint16_t	copy: 1;
			uint16_t	reserved9: 7;
		};
	};
};

struct spdk_nvme_cdata_oacs {
	union {
		struct {
			/** Security Send Receive Supported */
			uint16_t	ssrs  : 1;

			/** Format NVM Supported */
			uint16_t	fnvms    : 1;

			/** Firmware Download Supported */
			uint16_t	fwds  : 1;

			/** Namespace Management Supported */
			uint16_t	nms  : 1;

			/** Device Self-test Supported */
			uint16_t	dsts : 1;

			/** Directives Supported */
			uint16_t	dirs : 1;

			/** Supports NVMe-MI */
			uint16_t	nsrs : 1;

			/** Virtualization Management Supported */
			uint16_t	vms : 1;

			/** Doorbell Buffer Config Supported */
			uint16_t	dbcs : 1;

			/** Get LBA Status Supported */
			uint16_t	glss : 1;

			/** Command and Feature Lockdown Supported */
			uint16_t	cfls : 1;

			/** Host Managed Live Migration Support */
			uint16_t	hmlms : 1;

			uint16_t	rsvd : 4;
		};

		/** Old bit names are deprecated and will be removed in 26.05 release */
		struct {
			uint16_t	security  : 1;
			uint16_t	format    : 1;
			uint16_t	firmware  : 1;
			uint16_t	ns_manage  : 1;
			uint16_t	device_self_test : 1;
			uint16_t	directives : 1;
			uint16_t	nvme_mi : 1;
			uint16_t	virtualization_management : 1;
			uint16_t	doorbell_buffer_config : 1;
			uint16_t	get_lba_status : 1;
			uint16_t	command_feature_lockdown : 1;
			uint16_t	oacs_rsvd : 5;
		};
	};
};

struct spdk_nvme_cdata_fuses {
	union {
		uint16_t	raw;
		struct {
			/** Fused Compare and Write Supported */
			uint16_t	fcws: 1;

			uint16_t	rsvd: 15;
		};

		/** Old bit names are deprecated and will be removed in 26.05 release */
		struct {
			uint16_t	compare_and_write : 1;
			uint16_t	reserved : 15;
		};
	};
};

struct spdk_nvme_cdata_oaes {
	uint32_t	reserved1 : 8;

	/* Supports sending Namespace Attribute Notices. */
	uint32_t	ns_attribute_notices : 1;

	/* Supports sending Firmware Activation Notices. */
	uint32_t	fw_activation_notices : 1;

	uint32_t	reserved2 : 1;

	/* Supports Asymmetric Namespace Access Change Notices. */
	uint32_t	ana_change_notices : 1;

	/* Supports Predictable Latency Event Aggregate Log Change Notices. */
	uint32_t	pleal_change_notices : 1;

	/* Supports LBA Status Information Alert Notices. */
	uint32_t	lba_sia_notices : 1;

	/* Supports Endurance Group Event Aggregate Log Page Change Notices. */
	uint32_t	egealp_change_notices : 1;

	/* Supports Normal NVM Subsystem Shutdown event. */
	uint32_t	nnvm_sse : 1;

	uint32_t	reserved3 : 11;

	/* Supports Zone Descriptor Change Notices (refer to the ZNS Command Set specification) */
	uint32_t	zdes_change_notices : 1;

	uint32_t	reserved4 : 3;

	/* Supports Discovery log change notices (refer to the NVMe over Fabrics specification) */
	uint32_t	discovery_log_change_notices : 1;
};

union spdk_nvme_cdata_ctratt {
	uint32_t	raw;
	struct {
		/* Supports 128-bit host identifier */
		uint32_t	host_id_exhid_supported: 1;

		/* Supports non-operational power state permissive mode */
		uint32_t	non_operational_power_state_permissive_mode: 1;

		/* Supports NVM sets */
		uint32_t	nvm_sets: 1;

		/* Supports read recovery levels */
		uint32_t	read_recovery_levels: 1;

		/* Supports endurance groups */
		uint32_t	endurance_groups: 1;

		/* Supports predictable latency mode */
		uint32_t	predictable_latency_mode: 1;

		/* Supports traffic based keep alive */
		uint32_t	tbkas: 1;

		/* Supports reporting of namespace granularity */
		uint32_t	namespace_granularity: 1;

		/* Supports SQ associations */
		uint32_t	sq_associations: 1;

		/* Supports reporting of UUID list */
		uint32_t	uuid_list: 1;

		/* NVM subsystem supports multiple domains */
		uint32_t	mds: 1;

		/* Supports fixed capacity management */
		uint32_t	fixed_capacity_management: 1;

		/* Supports variable capacity management */
		uint32_t	variable_capacity_management: 1;

		/* Supports delete endurance group operation */
		uint32_t	delete_endurance_group: 1;

		/* Supports delete NVM set */
		uint32_t	delete_nvm_set: 1;

		/* Supports I/O command set specific extended PI formats */
		uint32_t	elbas: 1;

		uint32_t	reserved1: 3;

		/* Supports flexible data placement */
		uint32_t	fdps: 1;

		uint32_t	reserved2: 12;
	} bits;
};

#pragma pack(push, 1)
/* [한국어] ★★★ struct spdk_nvme_ctrlr_data — Identify Controller (CNS=0x01) 4096B 페이로드 ★★★
 * NVMe Base 2.0 §5.17.2 Identify Controller Data Structure. Admin Identify(0x06, CNS=0x01)
 * 명령의 응답으로 호스트가 받는 4096바이트 구조체. lib/nvme/nvme_ctrlr.c::nvme_ctrlr_identify
 * 가 attach 시 수확해 controller 객체에 캐싱; 이후 모든 capability check(예: vwc.present,
 * oncs.dsm, sgls.supported)이 이 캐시에서 read.
 *
 * 메모리 레이아웃 (4096B = 4 KiB):
 *  ┌────────────────────────────────────────────────────────┐
 *  │ bytes 0-255:    controller capabilities and features   │
 *  │ bytes 256-511:  admin command set attributes (OACS,FRMW)│
 *  │ bytes 512-703:  NVM command set attributes (SQES,CQES) │
 *  │ bytes 704-1791: I/O Command Set 공통 / NVMe-oF 등      │
 *  │ bytes 1792-1919: NVMe-oF specific                       │
 *  │ bytes 2048-3071: power state descriptor[32] (32B*32)   │
 *  │ bytes 3072-4095: vendor specific                        │
 *  └────────────────────────────────────────────────────────┘
 *
 * 설정자 = 컨트롤러(읽기 전용 immutable). 읽는 자 = 호스트 NVMe 드라이버(SPDK lib/nvme).
 * 동기화 = Identify 한 번 회수 후 attach 동안 read-only 캐시; FW activation/NS attach 시
 * 다시 회수해 갱신 가능.
 *
 * 필드 그룹별 의미는 아래 인라인 주석 참조. */
struct spdk_nvme_ctrlr_data {
	/* bytes 0-255: controller capabilities and features */
	/* [한국어] === bytes 0-255: 컨트롤러 식별/공통 capability (PCI 정보, multi-path, RTD3 등) === */

	/** pci vendor id */
	uint16_t		vid;
	/* [한국어] [bytes 0-1] VID — PCI Vendor ID. PCI SIG 등록값 (Intel=0x8086, Samsung=0x144D 등).
	 * 설정자 = 컨트롤러 ROM. 읽는 자 = SPDK lspci 출력/디버그 로그. */

	/** pci subsystem vendor id */
	uint16_t		ssvid;
	/* [한국어] [bytes 2-3] SSVID — PCI Subsystem Vendor ID. OEM이 ODM에게 부여한 별도 ID. */

	/** serial number */
	int8_t			sn[SPDK_NVME_CTRLR_SN_LEN];
	/* [한국어] [bytes 4-23] SN — ASCII 시리얼 번호(공백 패딩, 비-널 종료). 길이 20.
	 * SPDK는 bdev 이름 생성/RPC 출력에 활용. */

	/** model number */
	int8_t			mn[SPDK_NVME_CTRLR_MN_LEN];
	/* [한국어] [bytes 24-63] MN — ASCII 모델명 40바이트(공백 패딩). */

	/** firmware revision */
	uint8_t			fr[SPDK_NVME_CTRLR_FR_LEN];
	/* [한국어] [bytes 64-71] FR — 8자 ASCII 펌웨어 리비전. Firmware Commit 후 갱신될 수 있음. */

	/** recommended arbitration burst */
	uint8_t			rab;
	/* [한국어] [byte 72] RAB — 권장 Arbitration Burst (2^RAB 명령). FID=0x01 set 시 참고치. */

	/** ieee oui identifier */
	uint8_t			ieee[3];
	/* [한국어] [bytes 73-75] IEEE OUI — IEEE 등록 24비트 OUI. NGUID/EUI64의 vendor prefix와 매칭. */

	/** controller multi-path I/O and namespace sharing capabilities */
	struct {
		/* [한국어] CMIC — Controller Multi-path I/O and Namespace Sharing Capabilities (1B).
		 * NVMe Base 2.0 §5.17.2.1.4. multi-port/multi-ctrlr NVM subsystem 위상 표시. */
		union {
			struct {
				/* Multiple Ports */
				uint8_t mports : 1;
				/* [한국어] [0] mports — 1=NVM subsystem이 ≥2 PCIe/Fabrics port 보유. */

				/* Multiple Controllers */
				uint8_t mctrs: 1;
				/* [한국어] [1] mctrs — 1=NVM subsystem에 ≥2 controller 존재(SR-IOV 또는 dual-port). */

				/* Function Type */
				uint8_t ft : 1;
				/* [한국어] [2] ft — 0=PF(Physical Function), 1=VF(Virtual Function, SR-IOV). */

				/* Asymmetric Namespace Access Reporting Support */
				uint8_t anars : 1;
				/* [한국어] [3] anars — 1=ANA 보고 지원. NVMe-oF multipath의 핵심 (LID=0x0C 회수 가능). */
				uint8_t rsvd : 4;
				/* [한국어] [7:4] reserved. */
			};

			/** Old bit names are deprecated and will be removed in 26.05 release */
			struct {
				uint8_t multi_port	: 1;
				/* [한국어] mports의 deprecated alias. */
				uint8_t multi_ctrlr	: 1;
				/* [한국어] mctrs의 deprecated alias. */
				uint8_t sr_iov		: 1;
				/* [한국어] ft의 deprecated alias. */
				uint8_t ana_reporting	: 1;
				/* [한국어] anars의 deprecated alias. */
				uint8_t reserved	: 4;
				/* [한국어] reserved. */
			};
		};
	} cmic;

	/** maximum data transfer size */
	uint8_t			mdts;
	/* [한국어] [byte 77] MDTS — 단일 명령의 최대 전송 크기. 실제 = 2^MDTS * CAP.MPSMIN.
	 * 0=무제한. SPDK lib/nvme/nvme_ns_cmd.c가 큰 I/O를 자동 분할(splitting)하는 기준.
	 * MPSMIN은 4KB가 일반적이므로 MDTS=5는 128KB. */

	/** controller id */
	uint16_t		cntlid;
	/* [한국어] [bytes 78-79] CNTLID — NVM Subsystem 안에서 이 컨트롤러의 고유 ID. NVMe-oF
	 * Connect 시 host가 명시 가능; SPDK는 캐시해 reservation/multipath에 사용. */

	/** version */
	union spdk_nvme_vs_register	ver;
	/* [한국어] [bytes 80-83] VER — VS register 사본 (Major.Minor.Tertiary). PCIe BAR과 동일. */

	/** RTD3 resume latency */
	uint32_t		rtd3r;
	/* [한국어] [bytes 84-87] RTD3R — RTD3(Runtime D3 cold) Resume Latency (us). 컨트롤러가
	 * D3cold→D0 복귀에 걸리는 최대 시간. APST 정책 결정에 사용. */

	/** RTD3 entry latency */
	uint32_t		rtd3e;
	/* [한국어] [bytes 88-91] RTD3E — RTD3 Entry Latency (us). D0→D3cold 진입 시간. */

	/** optional asynchronous events supported */
	struct spdk_nvme_cdata_oaes oaes;
	/* [한국어] [bytes 92-95] OAES — Optional AER 지원 비트맵. 컨트롤러가 어떤 종류의 AER을
	 * 발행할 수 있는지 (NS attribute notice, FW activation, telemetry, ANA change 등).
	 * SPDK가 init 시 feat_async_event_cfg로 enable 시 컨트롤러가 OAES와 AND. */

	/** controller attributes */
	union spdk_nvme_cdata_ctratt ctratt;
	/* [한국어] [bytes 96-99] CTRATT — Controller Attributes 비트맵. host_id 128bit 지원,
	 * NVM Sets 지원, Read Recovery Levels, Endurance Groups, Predictable Latency,
	 * Traffic Based Keep Alive, Namespace Granularity, SQ Associations, UUID List 등. */

	/** Read Recovery Levels Supported */
	uint16_t		rrls;
	/* [한국어] [bytes 100-101] RRLS — Read Recovery Level 지원 비트맵 (각 비트 = level 0~15
	 * 지원 여부). FID=0x12 Read Recovery Level Config로 선택. */

	uint8_t			reserved_102[9];
	/* [한국어] [bytes 102-110] reserved. */

	/** Controller Type */
	uint8_t			cntrltype;
	/* [한국어] [byte 111] CNTRLTYPE — 1=I/O, 2=Discovery, 3=Administrative
	 * (enum spdk_nvme_ctrlr_type). NVMe-oF에서 host가 controller 종류 식별. */

	/** FRU globally unique identifier */
	uint8_t			fguid[16];
	/* [한국어] [bytes 112-127] FGUID — Field Replaceable Unit GUID (128bit). hot-plug 시
	 * 같은 FRU 식별. 0=미지원. */

	/** Command Retry Delay Time 1, 2 and 3 */
	uint16_t		crdt[3];
	/* [한국어] [bytes 128-133] CRDT[1,2,3] — Command Retry Delay Time (100ms 단위). CQE.status
	 * 의 CRD 필드(2비트)가 1~3 가리키면 호스트는 해당 시간 대기 후 retry. */

	uint8_t			reserved_134[119];
	/* [한국어] [bytes 134-252] reserved. */

	/** NVM Subsystem Report */
	struct {
		/* [한국어] NVMSR — NVM Subsystem Report (1B). NVMe Base 2.0 §5.17.2.1.16. */
		/* NVM Subsystem part of NVMe storage device */
		uint8_t		nvmesd : 1;
		/* [한국어] [0] nvmesd — 1=이 NVM subsystem이 NVMe Storage Device의 일부. */

		/* NVM Subsystem part of NVMe enclosure */
		uint8_t		nvmee : 1;
		/* [한국어] [1] nvmee — 1=NVMe Enclosure(JBOF/storage shelf) 내부. */

		uint8_t		nvmsr_rsvd : 6;
		/* [한국어] [7:2] reserved. */
	} nvmsr;

	/** VPD Write Cycle Information */
	struct {
		/* [한국어] VWCI — VPD Write Cycle Information. NVMe-MI VPD 영역의 write 한도 추적. */
		/* VPD write cycles remaining */
		uint8_t		vwcr : 7;
		/* [한국어] [6:0] vwcr — 남은 VPD write cycle(0=거의 다 씀). */

		/* VPD write cycles remaining valid */
		uint8_t		vwcrv : 1;
		/* [한국어] [7] vwcrv — 1=vwcr 필드가 유효. 0이면 무시. */
	} vwci;

	/** Management Endpoint Capabilities */
	struct {
		/* [한국어] MEC — Management Endpoint Capabilities. NVMe-MI 관리 채널 지원. */
		/* SMBus/I2C Port management endpoint */
		uint8_t		smbusme : 1;
		/* [한국어] [0] smbusme — 1=SMBus/I2C 통한 NVMe-MI 지원. */

		/* PCIe port management endpoint */
		uint8_t		pcieme : 1;
		/* [한국어] [1] pcieme — 1=PCIe VDM(Vendor Defined Message) 통한 NVMe-MI 지원. */

		uint8_t		mec_rsvd : 6;
		/* [한국어] [7:2] reserved. */
	} mec;

	/* bytes 256-511: admin command set attributes */
	/* [한국어] === bytes 256-511: Admin Command Set Attributes — admin opcode 지원/한도/firmware
	 * /sanitize/ANA capability 등 === */

	/** optional admin command support */
	struct spdk_nvme_cdata_oacs oacs;
	/* [한국어] [bytes 256-257] OACS — Optional Admin Command Support 비트맵.
	 * Security Send/Recv, Format NVM, Firmware Commit/Download, NS Management, Self-Test,
	 * Directives, Virtualization Mgmt, Doorbell Buffer Config, Get LBA Status, Lockdown 등.
	 * SPDK lib/nvme/nvme_ctrlr.c가 명령 발행 전 oacs 비트로 capability 확인. */

	/** abort command limit */
	uint8_t			acl;
	/* [한국어] [byte 258] ACL — 동시에 발행 가능한 Abort 명령 수 - 1. 보통 3(=4개). */

	/** asynchronous event request limit */
	uint8_t			aerl;
	/* [한국어] [byte 259] AERL — 동시에 미완료 상태로 큐에 적재 가능한 AER 수 - 1.
	 * SPDK init 시 AERL+1 개의 AER을 admin SQ에 미리 채워 컨트롤러 이벤트 통지를 대기. */

	/** firmware updates */
	struct {
		/* [한국어] FRMW — Firmware Updates Support (1B). */
		/* first slot is read-only */
		uint8_t		slot1_ro  : 1;
		/* [한국어] [0] slot1_ro — slot1이 read-only(공장 펌웨어). */

		/* number of firmware slots */
		uint8_t		num_slots : 3;
		/* [한국어] [3:1] num_slots — 지원 firmware slot 수(1~7). Firmware Slot Info log(LID=0x03)와 매핑. */

		/* support activation without reset */
		uint8_t		activation_without_reset : 1;
		/* [한국어] [4] activation_without_reset — 1=Firmware Commit CA=3 사용 가능
		 * (controller reset 없이 즉시 활성화). */

		/* Support multiple update detection */
		uint8_t		multiple_update_detection : 1;
		/* [한국어] [5] multiple_update_detection — 1=동일 slot에 다중 update 시도 검출 가능. */

		uint8_t		frmw_rsvd : 2;
		/* [한국어] [7:6] reserved. */
	} frmw;

	/** log page attributes */
	struct {
		/* [한국어] LPA — Log Page Attributes (1B). NVMe Base 2.0 §5.17.2.1.21. */
		union {
			struct {
				/* SMART Support */
				uint8_t		smarts : 1;
				/* [한국어] [0] smarts — SMART/Health (LID=0x02) NS-specific 지원. */
				/* Commands Supported and Effects Support */
				uint8_t		cses : 1;
				/* [한국어] [1] cses — Commands Supported and Effects log (LID=0x05) 지원. */
				/* Log Page Extended Data Support */
				uint8_t		lpeds: 1;
				/* [한국어] [2] lpeds — Get Log Page에서 32bit NUMD 지원 (>4GB log). */
				/* Telemetry Support */
				uint8_t		ts : 1;
				/* [한국어] [3] ts — Telemetry log (LID=0x07/0x08) 지원. */
				/* Persistent Event Support */
				uint8_t		pes : 1;
				/* [한국어] [4] pes — Persistent Event Log (LID=0x0D) 지원. */
				/* Miscellaneous Log Page Support */
				uint8_t		mlps : 1;
				/* [한국어] [5] mlps — Misc log 지원. */
				/* Data Area 4 Support */
				uint8_t		da4s : 1;
				/* [한국어] [6] da4s — Telemetry Data Area 4 지원. */
				uint8_t		rsvd : 1;
				/* [한국어] [7] reserved. */
			};
			/** Old bit names are deprecated and will be removed in 26.05 release */
			struct {
				uint8_t		ns_smart : 1;
				/* [한국어] smarts의 deprecated alias. */
				uint8_t		celp : 1;
				/* [한국어] cses의 deprecated alias. */
				uint8_t		edlp: 1;
				/* [한국어] lpeds의 deprecated alias. */
				uint8_t		telemetry : 1;
				/* [한국어] ts의 deprecated alias. */
				uint8_t		pelp : 1;
				/* [한국어] pes의 deprecated alias. */
				uint8_t		lplp : 1;
				/* [한국어] mlps의 deprecated alias. */
				uint8_t		da4_telemetry : 1;
				/* [한국어] da4s의 deprecated alias. */
				uint8_t		lpa_rsvd : 1;
				/* [한국어] reserved. */
			};
		};
	} lpa;

	/** error log page entries */
	uint8_t			elpe;
	/* [한국어] [byte 262] ELPE — Error Log Page entries 수 - 1 (0-base). 컨트롤러가 보관하는
	 * 최근 에러 로그 엔트리 수. */

	/** number of power states supported */
	uint8_t			npss;
	/* [한국어] [byte 263] NPSS — Number of Power States supported - 1. psd[0..NPSS] 유효. */

	/** admin vendor specific command configuration */
	struct {
		/* [한국어] AVSCC — Admin Vendor Specific Command Config. */
		/* admin vendor specific commands use disk format */
		uint8_t		spec_format : 1;
		/* [한국어] [0] spec_format — vendor specific admin 명령이 NVMe-규격 dword 포맷 사용 여부. */

		uint8_t		avscc_rsvd  : 7;
		/* [한국어] [7:1] reserved. */
	} avscc;

	/** autonomous power state transition attributes */
	struct {
		/* [한국어] APSTA — Autonomous Power State Transition Attributes. */
		/** controller supports autonomous power state transitions */
		uint8_t		supported  : 1;
		/* [한국어] [0] supported — 1=APST 지원 (FID=0x0c 활성 가능). */

		uint8_t		apsta_rsvd : 7;
		/* [한국어] [7:1] reserved. */
	} apsta;

	/** warning composite temperature threshold */
	uint16_t		wctemp;
	/* [한국어] [bytes 266-267] WCTEMP — Warning Composite Temperature Threshold (K).
	 * Composite 온도가 이 값을 초과하면 SMART critical_warning.temperature=1. */

	/** critical composite temperature threshold */
	uint16_t		cctemp;
	/* [한국어] [bytes 268-269] CCTEMP — Critical Composite Temp (K). 초과 시 컨트롤러는
	 * thermal shutdown 수행 가능. wctemp ≤ cctemp. */

	/** maximum time for firmware activation */
	uint16_t		mtfa;
	/* [한국어] [bytes 270-271] MTFA — Maximum Time for Firmware Activation (100ms 단위).
	 * Firmware Commit이 이 시간을 넘으면 status FIRMWARE_REQ_MAX_TIME_VIOLATION. */

	/** host memory buffer preferred size */
	uint32_t		hmpre;
	/* [한국어] [bytes 272-275] HMPRE — HMB 권장 크기 (4KB 단위). 0=HMB 미지원. */

	/** host memory buffer minimum size */
	uint32_t		hmmin;
	/* [한국어] [bytes 276-279] HMMIN — HMB 최소 크기 (4KB 단위). 호스트가 HMB 활성 시 ≥hmmin 보장. */

	/** total NVM capacity */
	uint64_t		tnvmcap[2];
	/* [한국어] [bytes 280-295] TNVMCAP — Total NVM Capacity (128bit, bytes). NVM Set/Endurance
	 * Group 지원 컨트롤러만 nonzero. */

	/** unallocated NVM capacity */
	uint64_t		unvmcap[2];
	/* [한국어] [bytes 296-311] UNVMCAP — Unallocated NVM Capacity (128bit, bytes). 미할당 영역
	 * (NS Management Create로 사용 가능한 free 용량). */

	/** replay protected memory block support */
	struct {
		/* [한국어] RPMBS — Replay Protected Memory Block Support. eMMC RPMB 유사 보안 영역. */
		uint8_t		num_rpmb_units	: 3;
		/* [한국어] [2:0] num_rpmb_units — RPMB target 수. */
		uint8_t		auth_method	: 3;
		/* [한국어] [5:3] auth_method — 0=HMAC SHA-256. */
		uint8_t		reserved1	: 2;
		/* [한국어] [7:6] reserved. */

		uint8_t		reserved2;
		/* [한국어] [byte 313] reserved. */

		uint8_t		total_size;
		/* [한국어] [byte 314] total_size — RPMB 영역 크기 (128KB 단위 - 1). */
		uint8_t		access_size;
		/* [한국어] [byte 315] access_size — 한 번 RPMB read/write 최대 크기 (512B 단위 - 1). */
	} rpmbs;

	/** extended device self-test time (in minutes) */
	uint16_t		edstt;
	/* [한국어] [bytes 316-317] EDSTT — Extended Device Self-Test Time (minutes). 예상 시간. */

	/** device self-test options */
	union {
		/* [한국어] DSTO — Device Self-Test Options. */
		uint8_t	raw;
		struct {
			/** Device supports only one device self-test operation at a time */
			uint8_t	one_only : 1;
			/* [한국어] [0] one_only — 1=한 번에 하나의 self-test만 가능(NS-별 동시 실행 불가). */

			uint8_t	reserved : 7;
			/* [한국어] [7:1] reserved. */
		} bits;
	} dsto;

	/**
	 * Firmware update granularity
	 *
	 * 4KB units
	 * 0x00 = no information provided
	 * 0xFF = no restriction
	 */
	uint8_t			fwug;
	/* [한국어] [byte 319] FWUG — Firmware Update Granularity (4KB 단위). Firmware Image
	 * Download의 chunk 크기 정렬 요구. */

	/**
	 * Keep Alive Support
	 *
	 * Granularity of keep alive timer in 100 ms units
	 * 0 = keep alive not supported
	 */
	uint16_t		kas;
	/* [한국어] [bytes 320-321] KAS — Keep Alive Support granularity (100ms 단위). 0=keep alive
	 * 미지원(NVMe PCIe). NVMe-oF 컨트롤러는 보통 nonzero. */

	/** Host controlled thermal management attributes */
	union {
		/* [한국어] HCTMA — Host Controlled Thermal Management Attributes. */
		uint16_t		raw;
		struct {
			uint16_t	supported : 1;
			/* [한국어] [0] supported — 1=HCTM 지원 (FID=0x10 사용 가능). */
			uint16_t	reserved : 15;
			/* [한국어] [15:1] reserved. */
		} bits;
	} hctma;

	/** Minimum thermal management temperature */
	uint16_t		mntmt;
	/* [한국어] [bytes 324-325] MNTMT — Minimum Thermal Management Temp (K). HCTMA TMT1/TMT2
	 * 의 하한. */

	/** Maximum thermal management temperature */
	uint16_t		mxtmt;
	/* [한국어] [bytes 326-327] MXTMT — Maximum Thermal Mgmt Temp (K). HCTMA의 상한. */

	/** Sanitize capabilities */
	union {
		/* [한국어] SANICAP — Sanitize Capabilities. */
		uint32_t	raw;
		struct {
			uint32_t	crypto_erase : 1;
			/* [한국어] [0] crypto_erase — Crypto Erase(media key 폐기) 지원. */
			uint32_t	block_erase : 1;
			/* [한국어] [1] block_erase — Block Erase(NAND 모든 블록 erase) 지원. */
			uint32_t	overwrite : 1;
			/* [한국어] [2] overwrite — Overwrite(패턴으로 덮어쓰기) 지원. */
			uint32_t	reserved : 29;
			/* [한국어] [31:3] reserved. */
		} bits;
	} sanicap;

	/** Host memory buffer minimum descriptor entry size */
	uint32_t		hmminds;
	/* [한국어] [bytes 332-335] HMMINDS — HMB Descriptor Entry Size minimum (4KB 단위).
	 * 0=요구사항 없음. */

	/** Host memory maximum descriptor entries */
	uint16_t		hmmaxd;
	/* [한국어] [bytes 336-337] HMMAXD — HMB Descriptor 최대 entry 수. */

	/** NVM set identifier maximum */
	uint16_t		nsetidmax;
	/* [한국어] [bytes 338-339] NSETIDMAX — NVM Set ID 최대값. */

	/** Endurance group identifier maximum */
	uint16_t		endgidmax;
	/* [한국어] [bytes 340-341] ENDGIDMAX — Endurance Group ID 최대값. FDP는 EG 단위로 활성. */

	/** ANA transition time */
	uint8_t			anatt;
	/* [한국어] [byte 342] ANATT — ANA Transition Time (sec). ANA Change 상태 최대 지속 시간. */

	/* bytes 343: Asymmetric namespace access capabilities */
	struct {
		/* [한국어] ANACAP — ANA Capabilities (1B). NVMe Base 2.0 §5.17.2.1.31.
		 * 컨트롤러가 보고 가능한 ANA state 종류와 ANA group ID 동작 특성. */
		uint8_t		ana_optimized_state : 1;
		/* [한국어] [0] — Optimized state 보고 가능. */
		uint8_t		ana_non_optimized_state : 1;
		/* [한국어] [1] — Non-Optimized state 보고. */
		uint8_t		ana_inaccessible_state : 1;
		/* [한국어] [2] — Inaccessible state 보고. */
		uint8_t		ana_persistent_loss_state : 1;
		/* [한국어] [3] — Persistent Loss state 보고. */
		uint8_t		ana_change_state : 1;
		/* [한국어] [4] — Change state 보고. */
		uint8_t		reserved : 1;
		/* [한국어] [5] reserved. */
		uint8_t		no_change_anagrpid : 1;
		/* [한국어] [6] no_change_anagrpid — 1=NS attach 시 ANA group 변경 불가. */
		uint8_t		non_zero_anagrpid : 1;
		/* [한국어] [7] non_zero_anagrpid — 1=ANAGRPID는 항상 nonzero (0=disabled NS 표시 불가). */
	} anacap;

	/* bytes 344-347: ANA group identifier maximum */
	uint32_t		anagrpmax;
	/* [한국어] [bytes 344-347] ANAGRPMAX — ANA Group Identifier 최대값. */
	/* bytes 348-351: number of ANA group identifiers */
	uint32_t		nanagrpid;
	/* [한국어] [bytes 348-351] NANAGRPID — 현재 사용 중인 ANA group 수. */

	/* bytes 352-355: persistent event log size */
	uint32_t		pels;
	/* [한국어] [bytes 352-355] PELS — Persistent Event Log Size (64KB 단위). LID=0x0D 회수 가능 크기. */

	/* Domain identifier that contains this controller */
	uint16_t		domain_identifier;
	/* [한국어] [bytes 356-357] Domain ID — 이 컨트롤러가 속한 NVM Domain. NVMe 2.0 multi-domain. */

	uint8_t			reserved3[10];
	/* [한국어] [bytes 358-367] reserved. */

	/* Maximum capacity of a single endurance group */
	uint8_t			megcap[SPDK_NVME_CTRLR_MEGCAP_LEN];
	/* [한국어] [bytes 368-383] MEGCAP — Endurance Group 한 개 최대 용량 (128bit, bytes). */

	/* bytes 384-511 */
	uint8_t			reserved384[128];
	/* [한국어] [bytes 384-511] reserved. */

	/* bytes 512-703: nvm command set attributes */
	/* [한국어] === bytes 512-703: NVM Command Set Attributes — I/O 명령 큐 항목 크기/지원 명령
	 * /VWC/원자성/PI 등 === */

	/** submission queue entry size */
	struct {
		/* [한국어] SQES — SQ Entry Size (1B, min/max log2). NVMe-oF에서만 의미. PCIe는 64B 고정. */
		uint8_t		min : 4;
		/* [한국어] [3:0] min — 최소 SQE 크기 = 2^min 바이트. PCIe=6 (64B). */
		uint8_t		max : 4;
		/* [한국어] [7:4] max — 최대 SQE 크기 = 2^max 바이트. */
	} sqes;

	/** completion queue entry size */
	struct {
		/* [한국어] CQES — CQ Entry Size (1B). PCIe는 16B 고정. */
		uint8_t		min : 4;
		/* [한국어] [3:0] min — 최소 = 2^min. PCIe=4 (16B). */
		uint8_t		max : 4;
		/* [한국어] [7:4] max — 최대 = 2^max. */
	} cqes;

	uint16_t		maxcmd;
	/* [한국어] [bytes 514-515] MAXCMD — NVMe-oF에서 한 번에 outstanding 가능한 명령 수.
	 * fabrics 한도. PCIe는 0 (의미 없음). */

	/** number of namespaces */
	uint32_t		nn;
	/* [한국어] [bytes 516-519] NN — 컨트롤러가 지원하는 NSID 최대값. CNS=0x02 Active NS list로
	 * 실제 attach된 NS 회수. */

	/** optional nvm command support */
	struct spdk_nvme_cdata_oncs oncs;
	/* [한국어] [bytes 520-521] ONCS — Optional NVM Command Support 비트맵. Compare, Write
	 * Uncorrectable, Write Zeroes, DSM, Reservation, Verify, Copy, Timestamp 등.
	 * SPDK lib/nvme/nvme_ns_cmd.c 의 spdk_nvme_ns_cmd_*가 이 비트맵 확인 후 dispatch. */

	/** fused operation support */
	struct spdk_nvme_cdata_fuses fuses;
	/* [한국어] [bytes 522-523] FUSES — Fused 명령 조합 지원 비트맵. Compare-and-Write 등. */

	/** format nvm attributes */
	struct {
		/* [한국어] FNA — Format NVM Attributes (1B). */
		uint8_t		format_all_ns: 1;
		/* [한국어] [0] format_all_ns — 1=Format이 모든 NS에 동시 적용(NS-별 불가). */
		uint8_t		erase_all_ns: 1;
		/* [한국어] [1] erase_all_ns — 1=Secure Erase가 모든 NS에 동시 적용. */
		uint8_t		crypto_erase_supported: 1;
		/* [한국어] [2] crypto_erase_supported — Format 명령에서 Crypto Erase(SES=2) 지원. */
		uint8_t		reserved: 5;
		/* [한국어] [7:3] reserved. */
	} fna;

	/** volatile write cache */
	struct {
		/* [한국어] VWC — Volatile Write Cache (1B). */
		uint8_t		present : 1;
		/* [한국어] [0] present — 1=휘발성 write cache 존재. FID=0x06 WCE 의미. */
		uint8_t		flush_broadcast : 2;
		/* [한국어] [2:1] flush_broadcast — 0=NSID-specific만, 1=NSID=0xFFFFFFFF로 broadcast 가능. */
		uint8_t		reserved : 5;
		/* [한국어] [7:3] reserved. */
	} vwc;

	/** atomic write unit normal */
	uint16_t		awun;
	/* [한국어] [bytes 526-527] AWUN — Atomic Write Unit Normal - 1 (LBA 단위). 정상 운영 시
	 * atomic 보장 단위. */

	/** atomic write unit power fail */
	uint16_t		awupf;
	/* [한국어] [bytes 528-529] AWUPF — Atomic Write Unit Power Fail - 1 (LBA). 전원 손실 시
	 * 보장 단위 (≤AWUN). */

	/** NVM vendor specific command configuration */
	uint8_t			nvscc;
	/* [한국어] [byte 530] NVSCC — NVM Vendor Specific Command Configuration. */

	/** Namespace Write Protection Capabilities */
	uint8_t			nwpc;
	/* [한국어] [byte 531] NWPC — Namespace Write Protection Capabilities. */

	/** atomic compare & write unit */
	uint16_t		acwu;
	/* [한국어] [bytes 532-533] ACWU — Atomic Compare & Write Unit - 1. Compare-and-Write 단일
	 * atomic 보장 LBA 수. */

	/** optional copy formats supported */
	struct {
		/* [한국어] OCFS — Optional Copy Formats Supported. */
		uint16_t	copy_format0 : 1;
		/* [한국어] [0] copy_format0 — Copy 명령 format 0(32B source range desc) 지원. */
		uint16_t	reserved1: 15;
		/* [한국어] [15:1] reserved. */
	} ocfs;


	struct spdk_nvme_cdata_sgls sgls;
	/* [한국어] [bytes 536-539] SGLS — SGL Support 비트맵. supported(2bit), keyed_sgl,
	 * bit_bucket_descriptor, metadata_pointer, oversized SGL, MPTR_SGL, address as offset,
	 * transport_data_block 등. NVMe-oF는 SGL 필수. PCIe는 PRP 또는 SGL. SPDK는 PCIe에서도
	 * SGL.supported 확인 후 SGL build (libnvme nvme_pcie_qpair.c). */

	/* maximum number of allowed namespaces */
	uint32_t		mnan;
	/* [한국어] [bytes 540-543] MNAN — 컨트롤러에 attach 가능한 NS 최대 수. */

	/* maximum domain namespace attachments */
	uint8_t			maxdna[SPDK_NVME_MAXDNA_FIELD_SIZE];
	/* [한국어] [bytes 544-559] MAXDNA — Domain별 NS attach 한도 (128bit). */

	/* maximum I/O controller namespace attachments */
	uint32_t		maxcna;
	/* [한국어] [bytes 560-563] MAXCNA — I/O controller가 attach 가능한 NS 최대 수. */

	uint8_t			reserved4[204];
	/* [한국어] [bytes 564-767] reserved. */

	uint8_t			subnqn[SPDK_NVME_NQN_FIELD_SIZE];
	/* [한국어] [bytes 768-1023] SUBNQN — NVM Subsystem NVMe Qualified Name (256B). NVMe-oF에서
	 * 호스트 Connect 시 매칭. PCIe는 보통 nqn.2014-08.org.nvmexpress 형식. */

	uint8_t			reserved5[768];
	/* [한국어] [bytes 1024-1791] reserved. */

	struct spdk_nvme_cdata_nvmf_specific nvmf_specific;
	/* [한국어] [bytes 1792-2047] NVMe-oF specific (256B): IOCCSZ(I/O Cmd Capsule Size),
	 * IORCSZ(I/O Resp Capsule Size), ICDOFF(In-Capsule Data Offset), CTRATT, MSDBD 등.
	 * NVMe-oF transport별 capsule 크기 협상에 필요. */

	/* bytes 2048-3071: power state descriptors */
	struct spdk_nvme_power_state	psd[32];
	/* [한국어] [bytes 2048-3071] PSD[32] — 32개 power state descriptor (32B씩). 각 PS의 max
	 * power, idle/active power, entry/exit latency, relative read/write throughput/latency.
	 * APST(FID=0x0c) 정책 결정과 FID=0x02 set value의 인덱스로 사용. NPSS+1개만 유효. */

	/* bytes 3072-4095: vendor specific */
	uint8_t			vs[1024];
	/* [한국어] [bytes 3072-4095] VS — vendor specific 영역(1024B). nvme_intel.h 등 vendor
	 * 헤더가 이 영역에 자체 구조체 매핑. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ctrlr_data) == 4096, "Incorrect size");
#pragma pack(pop)

struct spdk_nvme_zns_ctrlr_data {
	/** zone append size limit */
	uint8_t			zasl;

	uint8_t			reserved1[4095];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_zns_ctrlr_data) == 4096, "Incorrect size");

#pragma pack(push, 1)
struct spdk_nvme_primary_ctrl_capabilities {
	/**  controller id */
	uint16_t		cntlid;
	/**  port identifier */
	uint16_t		portid;
	/**  controller resource types */
	struct {
		uint8_t vq_supported	: 1;
		uint8_t vi_supported	: 1;
		uint8_t reserved	: 6;
	} crt;
	uint8_t			reserved[27];
	/** total number of VQ flexible resources */
	uint32_t		vqfrt;
	/** total number of VQ flexible resources assigned to secondary controllers */
	uint32_t		vqrfa;
	/** total number of VQ flexible resources allocated to primary controller */
	uint16_t		vqrfap;
	/** total number of VQ Private resources for the primary controller */
	uint16_t		vqprt;
	/** max number of VQ flexible Resources that may be assigned to a secondary controller */
	uint16_t		vqfrsm;
	/** preferred granularity of assigning and removing VQ Flexible Resources */
	uint16_t		vqgran;
	uint8_t			reserved1[16];
	/** total number of VI flexible resources for the primary and its secondary controllers */
	uint32_t		vifrt;
	/** total number of VI flexible resources assigned to the secondary controllers */
	uint32_t		virfa;
	/** total number of VI flexible resources currently allocated to the primary controller */
	uint16_t		virfap;
	/** total number of VI private resources for the primary controller */
	uint16_t		viprt;
	/** max number of VI flexible resources that may be assigned to a secondary controller */
	uint16_t		vifrsm;
	/** preferred granularity of assigning and removing VI flexible resources */
	uint16_t		vigran;
	uint8_t			reserved2[4016];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_primary_ctrl_capabilities) == 4096, "Incorrect size");

struct spdk_nvme_secondary_ctrl_entry {
	/** controller identifier of the secondary controller */
	uint16_t		scid;
	/** controller identifier of the associated primary controller */
	uint16_t		pcid;
	/** indicates the state of the secondary controller */
	struct {
		uint8_t is_online	: 1;
		uint8_t reserved	: 7;
	} scs;
	uint8_t	reserved[3];
	/** VF number if the secondary controller is an SR-IOV VF */
	uint16_t		vfn;
	/** number of VQ flexible resources assigned to the indicated secondary controller */
	uint16_t		nvq;
	/** number of VI flexible resources assigned to the indicated secondary controller */
	uint16_t		nvi;
	uint8_t			reserved1[18];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_secondary_ctrl_entry) == 32, "Incorrect size");

struct spdk_nvme_secondary_ctrl_list {
	/** number of Secondary controller entries in the list */
	uint8_t					number;
	uint8_t					reserved[31];
	struct spdk_nvme_secondary_ctrl_entry	entries[127];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_secondary_ctrl_list) == 4096, "Incorrect size");
#pragma pack(pop)

struct spdk_nvme_rescap {
	union {
		struct {
			/** Persist Through Power Loss Support */
			uint8_t ptpls	: 1;
			/** Write Exclusive Support */
			uint8_t wes	: 1;
			/** Exclusive Access Support */
			uint8_t eas	: 1;
			/** Write Exclusive – Registrants Only Support */
			uint8_t weros	: 1;
			/** Exclusive Access – Registrants Only Support */
			uint8_t	earos	: 1;
			/** Write Exclusive – All Registrants Support */
			uint8_t	wears	: 1;
			/** Exclusive Access – All Registrants Support */
			uint8_t	eaars	: 1;
			/** Ignore Existing Key Support */
			uint8_t	ieks	: 1;
		};

		/** Old bit naming is deprecated and will be removed in 26.05 release */
		struct {
			uint8_t	persist : 1;
			uint8_t	write_exclusive : 1;
			uint8_t	exclusive_access : 1;
			uint8_t	write_exclusive_reg_only : 1;
			uint8_t	exclusive_access_reg_only : 1;
			uint8_t	write_exclusive_all_reg : 1;
			uint8_t	exclusive_access_all_reg : 1;
			uint8_t	ignore_existing_key : 1;
		};
	};
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_rescap) == 1, "Incorrect size");

struct spdk_nvme_fpi {
	union {
		struct {
			/** Remaining Format NVM */
			uint8_t rfnvm	: 7;
			/** Format Progress Indicator Support */
			uint8_t fpis	: 1;
		};

		/** Old bit naming is deprecated and will be removed in 26.05 release */
		struct {
			uint8_t percentage_remaining	: 7;
			uint8_t	fpi_supported		: 1;
		};
	};
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fpi) == 1, "Incorrect size");

struct spdk_nvme_nmic {
	union {
		struct {
			/** Shared Namespace */
			uint8_t shrns	: 1;
			/** Dispersed Namespace */
			uint8_t disns	: 1;
			/** Reserved */
			uint8_t	rsvd : 6;
		};

		/** Old bit naming is deprecated and will be removed in 26.05 release */
		struct {
			uint8_t	can_share : 1;
			uint8_t	reserved : 7;
		};
	};
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_nmic) == 1, "Incorrect size");

struct spdk_nvme_nsattr {
	union {
		struct {
			/** Currently Write Protected */
			uint8_t cwp	: 1;
			/** Reserved */
			uint8_t rsvd	: 7;
		};

		/** Old bit naming is deprecated and will be removed in 26.05 release */
		struct {
			uint8_t	write_protected	: 1;
			uint8_t	reserved	: 7;
		};
	};
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_nsattr) == 1, "Incorrect size");

/* [한국어] ★★★ struct spdk_nvme_ns_data — Identify Namespace (CNS=0x00) 4096B 페이로드 ★★★
 * NVMe Base 2.0 §5.17.2.2 (NVM CS) Identify Namespace Data Structure. Admin Identify(0x06,
 * CNS=0x00) 명령의 응답으로 NSID-당 받는 4096바이트. lib/nvme/nvme_ns.c::nvme_ns_construct
 * 가 attach 시 NSID마다 회수해 spdk_nvme_ns 객체에 캐싱; sector_size, num_sectors, md_size,
 * pi_type, dlfeat 등이 모두 이 구조체에서 유도.
 *
 * 설정자 = 컨트롤러 (read-only after Identify).
 * 읽는 자 = 호스트 NVMe 드라이버 (lib/nvme), bdev_nvme.
 * 동기화 = NS 변경 AER(NS attribute changed) 후 다시 회수.
 *
 * 핵심 의존: flbas.format → lbaf[format].lbads/ms → sector_size/md_size 계산. */
struct spdk_nvme_ns_data {
	/** namespace size */
	uint64_t		nsze;
	/* [한국어] [bytes 0-7] NSZE — Namespace Size (LBA 단위). 논리적 크기. read/write 가능 LBA 범위 = [0, nsze). */

	/** namespace capacity */
	uint64_t		ncap;
	/* [한국어] [bytes 8-15] NCAP — Namespace Capacity (LBA). 실제로 할당된 미디어 용량.
	 * thin-provisioned NS는 ncap < nsze 가능. */

	/** namespace utilization */
	uint64_t		nuse;
	/* [한국어] [bytes 16-23] NUSE — Namespace Utilization (LBA). 현재 사용 중인 LBA 수. */

	/** namespace features */
	struct {
		/* [한국어] NSFEAT — Namespace Features (1B). */
		/** thin provisioning */
		uint8_t		thin_prov : 1;
		/* [한국어] [0] thin_prov — 1=thin provisioning 지원 (nsze>ncap 가능). */

		/** NAWUN, NAWUPF, and NACWU are defined for this namespace */
		uint8_t		ns_atomic_write_unit : 1;
		/* [한국어] [1] ns_atomic_write_unit — 1=NS-별 nawun/nawupf/nacwu 유효. */

		/** Supports Deallocated or Unwritten LBA error for this namespace */
		uint8_t		dealloc_or_unwritten_error : 1;
		/* [한국어] [2] — DULBE error 보고 지원 (read 시 SC=DEALLOCATED_OR_UNWRITTEN_BLOCK). */

		/** Non-zero NGUID and EUI64 for namespace are never reused */
		uint8_t		guid_never_reused : 1;
		/* [한국어] [3] guid_never_reused — 1=NGUID/EUI64가 NS 재사용 시에도 같은 값 안 씀(고유). */

		/** Optimal Performance field */
		uint8_t		optperf : 1;
		/* [한국어] [4] optperf — 1=npwg/npwa/npdg/npda/nows 필드가 의미 있음. */

		uint8_t		reserved1 : 3;
		/* [한국어] [7:5] reserved. */
	} nsfeat;

	/** number of lba formats */
	uint8_t			nlbaf;
	/* [한국어] [byte 25] NLBAF — 지원 LBA Format 수 - 1 (0-base). lbaf[0..nlbaf] 유효. */

	/** formatted lba size */
	struct {
		/* [한국어] FLBAS — Formatted LBA Size (1B). 현재 활성 LBA format 인덱스. */
		/** LSB for Format index */
		uint8_t		format     : 4;
		/* [한국어] [3:0] format — 활성 lbaf 인덱스 LSB. SPDK ns->sector_size = 1<<lbaf[format].lbads. */
		uint8_t		extended   : 1;
		/* [한국어] [4] extended — 1=metadata가 LBA 데이터에 inline (size = lba+ms). 0=별도 mptr. */
		/** MSB for Format index, to be ignored if nlbaf <= 16 */
		uint8_t		msb_format : 2;
		/* [한국어] [6:5] msb_format — nlbaf>16일 때 format MSB. */
		uint8_t		reserved2  : 1;
		/* [한국어] [7] reserved. */
	} flbas;

	/** metadata capabilities */
	struct {
		/* [한국어] MC — Metadata Capabilities (1B). */
		/** metadata can be transferred as part of data prp list */
		uint8_t		extended  : 1;
		/* [한국어] [0] extended — 1=metadata inline 가능 (FLBAS.extended=1 활성). */

		/** metadata can be transferred with separate metadata pointer */
		uint8_t		pointer   : 1;
		/* [한국어] [1] pointer — 1=별도 MPTR로 metadata 전송 가능. */

		/** reserved */
		uint8_t		reserved3 : 6;
		/* [한국어] [7:2] reserved. */
	} mc;

	/** end-to-end data protection capabilities */
	struct {
		/* [한국어] DPC — Data Protection Capabilities (1B). 지원 PI type. */
		/** protection information type 1 */
		uint8_t		pit1     : 1;
		/* [한국어] [0] pit1 — Type 1 PI 지원. RefTag=초기 LBA + i. */

		/** protection information type 2 */
		uint8_t		pit2     : 1;
		/* [한국어] [1] pit2 — Type 2 PI 지원. RefTag 임의 (호스트가 지정). */

		/** protection information type 3 */
		uint8_t		pit3     : 1;
		/* [한국어] [2] pit3 — Type 3 PI 지원. RefTag 검증 안 함. */

		/** first eight bytes of metadata */
		uint8_t		md_start : 1;
		/* [한국어] [3] md_start — 1=PI를 metadata 시작 8B에 위치 가능. */

		/** last eight bytes of metadata */
		uint8_t		md_end   : 1;
		/* [한국어] [4] md_end — 1=PI를 metadata 끝 8B에 위치 가능. */
	} dpc;

	/** end-to-end data protection type settings */
	struct {
		/* [한국어] DPS — Data Protection Type Settings (1B). 현재 활성 PI 설정. */
		/** protection information type */
		uint8_t		pit       : 3;
		/* [한국어] [2:0] pit — 0=비활성, 1=Type1, 2=Type2, 3=Type3. */

		/** 1 == protection info transferred at start of metadata */
		/** 0 == protection info transferred at end of metadata */
		uint8_t		md_start  : 1;
		/* [한국어] [3] md_start — 1=PI는 metadata 시작, 0=끝. */

		uint8_t		reserved4 : 4;
		/* [한국어] [7:4] reserved. */
	} dps;

	/** namespace multi-path I/O and namespace sharing capabilities */
	struct spdk_nvme_nmic nmic;
	/* [한국어] [byte 30] NMIC — NS Multi-path/Sharing (struct nmic). 1B. shrns(공유)/disns(분산). */

	/** reservation capabilities */
	union {
		/* [한국어] NSRESCAP — NS Reservation Capabilities (struct rescap; 1B). */
		struct spdk_nvme_rescap rescap;
		/* [한국어] PTPLS/Write Excl/Excl Access/등 비트별 지원 여부. */
		uint8_t raw;
		/* [한국어] raw 1B 뷰. */
	} nsrescap;
	/** format progress indicator */
	struct spdk_nvme_fpi fpi;
	/* [한국어] [byte 32] FPI — Format Progress Indicator (struct fpi; 1B). rfnvm(남은 %)+fpis(지원 여부). */

	/** deallocate logical features */
	union {
		/* [한국어] DLFEAT — Deallocate Logical Block Features (1B). NVMe Base 2.0 §5.17.2.2.5. */
		uint8_t		raw;
		struct {
			/**
			 * Value read from deallocated blocks
			 *
			 * 000b = not reported
			 * 001b = all bytes 0x00
			 * 010b = all bytes 0xFF
			 *
			 * \ref spdk_nvme_dealloc_logical_block_read_value
			 */
			uint8_t	read_value : 3;
			/* [한국어] [2:0] read_value — dealloc된 LBA read 시 반환 값(enum spdk_nvme_dealloc_logical_block_read_value). */

			/** Supports Deallocate bit in Write Zeroes */
			uint8_t	write_zero_deallocate : 1;
			/* [한국어] [3] — Write Zeroes의 DEAC=1 옵션 지원. */

			/**
			 * Guard field behavior for deallocated logical blocks
			 * 0: contains 0xFFFF
			 * 1: contains CRC for read value
			 */
			uint8_t	guard_value : 1;
			/* [한국어] [4] guard_value — dealloc LBA의 PI Guard 필드 동작. */

			uint8_t	reserved : 3;
			/* [한국어] [7:5] reserved. */
		} bits;
	} dlfeat;

	/** namespace atomic write unit normal */
	uint16_t		nawun;
	/* [한국어] [bytes 34-35] NAWUN — NS Atomic Write Unit Normal - 1 (LBA). NS-별 atomic 단위.
	 * nsfeat.ns_atomic_write_unit=1일 때 의미. */

	/** namespace atomic write unit power fail */
	uint16_t		nawupf;
	/* [한국어] [bytes 36-37] NAWUPF — NS Atomic Write Unit Power Fail - 1. ≤NAWUN. */

	/** namespace atomic compare & write unit */
	uint16_t		nacwu;
	/* [한국어] [bytes 38-39] NACWU — NS Atomic Compare & Write Unit - 1. */

	/** namespace atomic boundary size normal */
	uint16_t		nabsn;
	/* [한국어] [bytes 40-41] NABSN — NS Atomic Boundary Size Normal - 1. atomic write가 가로지르면 안 되는 경계 크기. */

	/** namespace atomic boundary offset */
	uint16_t		nabo;
	/* [한국어] [bytes 42-43] NABO — NS Atomic Boundary Offset. 첫 boundary가 LBA 0에서 떨어진 거리. */

	/** namespace atomic boundary size power fail */
	uint16_t		nabspf;
	/* [한국어] [bytes 44-45] NABSPF — NS Atomic Boundary Size Power Fail - 1. */

	/** namespace optimal I/O boundary in logical blocks */
	uint16_t		noiob;
	/* [한국어] [bytes 46-47] NOIOB — NS Optimal I/O Boundary (LBA). I/O 정렬 추천 경계.
	 * SPDK bdev_nvme의 optimal_io_boundary와 매핑. */

	/** NVM capacity */
	uint64_t		nvmcap[2];
	/* [한국어] [bytes 48-63] NVMCAP — NVM Capacity (128bit, bytes). NS의 실제 미디어 용량(byte). */

	/** Namespace Preferred Write Granularity */
	uint16_t		npwg;
	/* [한국어] [bytes 64-65] NPWG — NS Preferred Write Granularity - 1 (LBA). nsfeat.optperf=1일 때. */

	/** Namespace Preferred Write Alignment */
	uint16_t                npwa;
	/* [한국어] [bytes 66-67] NPWA — NS Preferred Write Alignment - 1. */

	/** Namespace Preferred Deallocate Granularity */
	uint16_t                npdg;
	/* [한국어] [bytes 68-69] NPDG — NS Preferred Dealloc Granularity. */

	/** Namespace Preferred Deallocate Alignment */
	uint16_t                npda;
	/* [한국어] [bytes 70-71] NPDA — NS Preferred Dealloc Alignment. */

	/** Namespace Optimal Write Size */
	uint16_t                nows;
	/* [한국어] [bytes 72-73] NOWS — NS Optimal Write Size - 1. */

	/** Maximum Single Source Range Length */
	uint16_t                mssrl;
	/* [한국어] [bytes 74-75] MSSRL — Copy 명령 source range 최대 길이 (LBA). */

	/** Maximum Copy Length */
	uint32_t                mcl;
	/* [한국어] [bytes 76-79] MCL — Copy 명령 총 LBA 한도. */

	/** Maximum Source Range Count */
	uint8_t	                msrc;
	/* [한국어] [byte 80] MSRC — Copy source range 최대 수 - 1 (0-base). */

	uint8_t			reserved81[11];
	/* [한국어] [bytes 81-91] reserved. */

	/** ANA group identifier */
	uint32_t		anagrpid;
	/* [한국어] [bytes 92-95] ANAGRPID — ANA Group ID. NVMe-oF multipath에서 같은 ANA group의
	 * 모든 NS는 동일 path state. */

	uint8_t			reserved96[3];
	/* [한국어] [bytes 96-98] reserved. */

	/** namespace attributes */
	struct spdk_nvme_nsattr nsattr;
	/* [한국어] [byte 99] NSATTR — NS Attributes (struct nsattr; 1B). cwp(write protected). */

	/** NVM Set Identifier */
	uint16_t		nvmsetid;
	/* [한국어] [bytes 100-101] NVMSETID — 이 NS가 속한 NVM Set ID. 0=set 미사용. */

	/** Endurance group identifier */
	uint16_t		endgid;
	/* [한국어] [bytes 102-103] ENDGID — 이 NS가 속한 Endurance Group. FDP 활성 시 EG 단위 RUH 운용. */

	/** namespace globally unique identifier */
	uint8_t			nguid[16];
	/* [한국어] [bytes 104-119] NGUID — NS Globally Unique ID (128bit). RFC 4122 UUID와 호환.
	 * SPDK가 bdev UUID 우선순위로 사용 (NGUID > EUI64 > UUID descriptor). */

	/** IEEE extended unique identifier */
	uint64_t		eui64;
	/* [한국어] [bytes 120-127] EUI64 — IEEE Extended Unique ID (64bit). 0=미사용. */

	/** lba format support */
	struct {
		/* [한국어] LBAF[64] — LBA Format Descriptor 배열. NVMe Base 2.0 §5.17.2.2.27.
		 * flbas.format이 가리키는 entry가 현재 활성. */
		/** metadata size */
		uint32_t	ms	  : 16;
		/* [한국어] [15:0] MS — metadata 바이트 수 (per LBA). 0=metadata 없음. PI Type1/2/3에서
		 * MS≥8 필요. */

		/** lba data size */
		uint32_t	lbads	  : 8;
		/* [한국어] [23:16] LBADS — LBA data size = 2^lbads 바이트. 일반적으로 9(=512B) 또는 12(=4KB). */

		/** relative performance */
		uint32_t	rp	  : 2;
		/* [한국어] [25:24] RP — Relative Performance: 0=Best, 1=Better, 2=Good, 3=Degraded.
		 * 호스트가 여러 LBA format 중 선택 시 참조. */

		uint32_t	reserved6 : 6;
		/* [한국어] [31:26] reserved. */
	} lbaf[64];

	uint8_t			vendor_specific[3712];
	/* [한국어] [bytes 384-4095] vendor specific 영역(3712B). nvme_intel.h 등에서 사용. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ns_data) == 4096, "Incorrect size");

enum spdk_nvme_pi_format {
	SPDK_NVME_16B_GUARD_PI	= 0,
	SPDK_NVME_32B_GUARD_PI	= 1,
	SPDK_NVME_64B_GUARD_PI	= 2,
};

struct spdk_nvme_nvm_ns_data {
	/** logical block storage tag mask */
	uint64_t		lbstm;

	/** protection information capabilities */
	struct {
		/** 16b guard protection information storage tag support */
		uint8_t		_16bpists	: 1;

		/** 16b guard protection information storage tag mask */
		uint8_t		_16bpistm	: 1;

		/** storage tag check read support */
		uint8_t		stcrs		: 1;

		uint8_t		reserved	: 5;
	} pic;

	uint8_t			reserved[3];

	struct {
		/** storage tag size */
		uint32_t	sts		: 7;

		/** protection information format */
		uint32_t	pif		: 2;

		uint32_t	reserved	: 23;
	} elbaf[64];

	uint8_t			reserved2[3828];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_nvm_ns_data) == 4096, "Incorrect size");

struct spdk_nvme_nvm_ctrlr_data {
	/* verify size limit */
	uint8_t			vsl;

	/* write zeroes size limit */
	uint8_t			wzsl;

	/* write uncorrectable size limit */
	uint8_t			wusl;

	/* dataset management ranges limit */
	uint8_t			dmrl;

	/* dataset management range size limit */
	uint32_t		dmrsl;

	/* dataset management size limit */
	uint64_t		dmsl;

	uint8_t			rsvd16[4080];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_nvm_ctrlr_data) == 4096, "Incorrect size");

struct spdk_nvme_zns_ns_data {
	/** zone operation characteristics */
	struct {
		uint16_t	variable_zone_capacity : 1;
		uint16_t	zone_active_excursions : 1;
		uint16_t	reserved0 : 14;
	} zoc;

	/** optional zoned command support */
	struct {
		uint16_t	read_across_zone_boundaries : 1;
		uint16_t	reserved0 : 15;
	} ozcs;

	/** maximum active resources */
	uint32_t		mar;

	/** maximum open resources */
	uint32_t		mor;

	/** reset recommended limit */
	uint32_t		rrl;

	/** finish recommended limit */
	uint32_t		frl;

	/** reset recommended limit 1 */
	uint32_t		rrl1;

	/** reset recommended limit 2 */
	uint32_t		rrl2;

	/** reset recommended limit 3 */
	uint32_t		rrl3;

	/** finish recommended limit 1 */
	uint32_t		frl1;

	/** finish recommended limit 2 */
	uint32_t		frl2;

	/** finish recommended limit 3 */
	uint32_t		frl3;

	uint8_t			reserved44[2772];

	/** zns lba format extension support */
	struct {
		/** zone size */
		uint64_t	zsze;

		/** zone descriptor extension size */
		uint64_t	zdes : 8;

		uint64_t	reserved15 : 56;
	} lbafe[64];

	uint8_t			vendor_specific[256];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_zns_ns_data) == 4096, "Incorrect size");

/** Identify – I/O Command Set Independent Identify Namespace Data Structure (CNS 08h) */
struct spdk_nvme_ns_iocs_independent_data {
	/** Common Namespace Features */
	struct {
		uint8_t reserved1	: 3;
		/** UID Reuse */
		uint8_t uidreuse	: 1;
		/** Rotational Media */
		uint8_t rmedia		: 1;
		/** Volatile Write Cache Not Present */
		uint8_t vwcnp		: 1;
		uint8_t reserved2	: 2;
	} nsfeat;

	/** Namespace Multi-path I/O and Namespace Sharing Capabilities */
	struct spdk_nvme_nmic nmic;

	/** Reservation Capabilities */
	struct spdk_nvme_rescap rescap;

	/** Format Progress Indicator */
	struct spdk_nvme_fpi fpi;

	/** ANA Group Identifier */
	uint32_t anagrpid;

	/** Namespace Attributes */
	struct spdk_nvme_nsattr nsattr;

	uint8_t reserved1;

	/** NVM Set Identifier */
	uint16_t nvmsetid;

	/** Endurance Group Identifier */
	uint16_t endgid;

	/** Namespace Status */
	struct {
		/** Namespace Ready */
		uint8_t nrdy		: 1;
		/** I/O Impacted */
		uint8_t ioi		: 2;
		uint8_t reserved	: 5;
	} nstat;

	/** Key Per I/O Status */
	struct {
		/** Key Per I/O Enabled in Namespace */
		uint8_t kpioens		: 1;
		/** Key Per I/O Supported in Namespace */
		uint8_t kpiosns		: 1;
		uint8_t reserved	: 6;
	} kpios;

	/** Maximum Key Tag */
	uint16_t maxkt;

	uint16_t reserved2;

	/** Reachability Group Identifier */
	uint32_t rgrpid;

	uint8_t reserved3[4072];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ns_iocs_independent_data) == 4096, "Incorrect size");

/**
 * IO command set vector for IDENTIFY_IOCS
 */
struct spdk_nvme_iocs_vector {
	uint8_t	nvm  : 1;
	uint8_t	kv   : 1;
	uint8_t	zns  : 1;
	uint8_t	rsvd : 5;
	uint8_t	rsvd2[7];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_iocs_vector) == 8, "Incorrect size");

/**
 * Deallocated logical block features - read value
 */
enum spdk_nvme_dealloc_logical_block_read_value {
	/** Not reported */
	SPDK_NVME_DEALLOC_NOT_REPORTED	= 0,

	/** Deallocated blocks read 0x00 */
	SPDK_NVME_DEALLOC_READ_00	= 1,

	/** Deallocated blocks read 0xFF */
	SPDK_NVME_DEALLOC_READ_FF	= 2,
};

/**
 * Reservation Type Encoding
 */
/* [한국어] === Reservation Type === NVMe Base 2.0 §8.19.1.
 * Acquire 시 RTYPE으로 지정. SCSI Persistent Reservation과 1:1 호환되어 dual-stack 클러스터
 * 환경에서 호스트가 NS를 단일 owner에 lock하거나 multi-host 일관성을 강제. */
enum spdk_nvme_reservation_type {
	/* 0x00 - reserved */

	/* Write Exclusive Reservation */
	SPDK_NVME_RESERVE_WRITE_EXCLUSIVE		= 0x1,
	/* [한국어] 0x1 — Write Exclusive. owner만 write 가능. read는 모든 host. */

	/* Exclusive Access Reservation */
	SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS		= 0x2,
	/* [한국어] 0x2 — Exclusive Access. owner만 read/write. 다른 host는 status code
	 * Reservation Conflict로 reject. */

	/* Write Exclusive - Registrants Only Reservation */
	SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY	= 0x3,
	/* [한국어] 0x3 — Write Exclusive - Registrants Only. write는 등록된 host만, read는
	 * 모든 host. */

	/* Exclusive Access - Registrants Only Reservation */
	SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_REG_ONLY	= 0x4,
	/* [한국어] 0x4 — Exclusive Access - Registrants Only. read/write 모두 등록된 host만. */

	/* Write Exclusive - All Registrants Reservation */
	SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS	= 0x5,
	/* [한국어] 0x5 — Write Exclusive - All Registrants. 등록된 모든 host가 write 가능
	 * (소유자 역할 분산). */

	/* Exclusive Access - All Registrants Reservation */
	SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS	= 0x6,
	/* [한국어] 0x6 — Exclusive Access - All Registrants. 등록된 모든 host가 read/write. */

	/* 0x7-0xFF - Reserved */
};

struct spdk_nvme_reservation_acquire_data {
	/** current reservation key */
	uint64_t		crkey;
	/** preempt reservation key */
	uint64_t		prkey;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_reservation_acquire_data) == 16, "Incorrect size");

/**
 * Reservation Acquire action
 */
/* [한국어] === Reservation Acquire (0x11) action ===
 * NVMe Base 2.0 §8.19.4. host가 NS reservation을 잡거나 다른 host의 reservation을 빼앗기. */
enum spdk_nvme_reservation_acquire_action {
	SPDK_NVME_RESERVE_ACQUIRE		= 0x0,
	/* [한국어] 0x0 — Acquire. crkey와 prkey=0으로 reservation 획득(이미 등록된 host만). */
	SPDK_NVME_RESERVE_PREEMPT		= 0x1,
	/* [한국어] 0x1 — Preempt. prkey가 가진 reservation을 빼앗기 + 모든 host의 등록 해제.
	 * 페일오버에 사용. */
	SPDK_NVME_RESERVE_PREEMPT_ABORT		= 0x2,
	/* [한국어] 0x2 — Preempt + Abort. preempt에 더해 진행 중 I/O 즉시 abort. */
};

#pragma pack(push, 1)
struct spdk_nvme_reservation_status_data {
	/** reservation action generation counter */
	uint32_t		gen;
	/** reservation type */
	uint8_t			rtype;
	/** number of registered controllers */
	uint16_t		regctl;
	uint16_t		reserved1;
	/** persist through power loss state */
	uint8_t			ptpls;
	uint8_t			reserved[14];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_reservation_status_data) == 24, "Incorrect size");

struct spdk_nvme_reservation_status_extended_data {
	struct spdk_nvme_reservation_status_data	data;
	uint8_t						reserved[40];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_reservation_status_extended_data) == 64,
		   "Incorrect size");

struct spdk_nvme_registered_ctrlr_data {
	/** controller id */
	uint16_t		cntlid;
	/** reservation status */
	struct {
		uint8_t		status    : 1;
		uint8_t		reserved1 : 7;
	} rcsts;
	uint8_t			reserved2[5];
	/** 64-bit host identifier */
	uint64_t		hostid;
	/** reservation key */
	uint64_t		rkey;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_registered_ctrlr_data) == 24, "Incorrect size");

struct spdk_nvme_registered_ctrlr_extended_data {
	/** controller id */
	uint16_t		cntlid;
	/** reservation status */
	struct {
		uint8_t		status    : 1;
		uint8_t		reserved1 : 7;
	} rcsts;
	uint8_t			reserved2[5];
	/** reservation key */
	uint64_t		rkey;
	/** 128-bit host identifier */
	uint8_t			hostid[16];
	uint8_t			reserved3[32];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_registered_ctrlr_extended_data) == 64, "Incorrect size");
#pragma pack(pop)

/**
 * Change persist through power loss state for
 *  Reservation Register command
 */
enum spdk_nvme_reservation_register_cptpl {
	SPDK_NVME_RESERVE_PTPL_NO_CHANGES		= 0x0,
	SPDK_NVME_RESERVE_PTPL_CLEAR_POWER_ON		= 0x2,
	SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS	= 0x3,
};

/**
 * Registration action for Reservation Register command
 */
/* [한국어] === Reservation Register (0x0d) action ===
 * NVMe Base 2.0 §8.19.3. host_id를 NS의 등록자 목록에 추가/제거/키 교체. reservation
 * acquire 전에 register가 선행되어야 함. */
enum spdk_nvme_reservation_register_action {
	SPDK_NVME_RESERVE_REGISTER_KEY		= 0x0,
	/* [한국어] 0x0 — 새 nrkey로 등록. 이미 등록된 host는 status code reservation conflict. */
	SPDK_NVME_RESERVE_UNREGISTER_KEY	= 0x1,
	/* [한국어] 0x1 — crkey 일치 시 등록 해제. */
	SPDK_NVME_RESERVE_REPLACE_KEY		= 0x2,
	/* [한국어] 0x2 — crkey → nrkey로 키 교체. host_id는 유지. */
};

struct spdk_nvme_reservation_register_data {
	/** current reservation key */
	uint64_t		crkey;
	/** new reservation key */
	uint64_t		nrkey;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_reservation_register_data) == 16, "Incorrect size");

struct spdk_nvme_reservation_key_data {
	/** current reservation key */
	uint64_t		crkey;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_reservation_key_data) == 8, "Incorrect size");

/**
 * Reservation Release action
 */
/* [한국어] === Reservation Release (0x15) action ===
 * NVMe Base 2.0 §8.19.5. */
enum spdk_nvme_reservation_release_action {
	SPDK_NVME_RESERVE_RELEASE		= 0x0,
	/* [한국어] 0x0 — Release. 자신의 reservation 해제(등록은 유지). */
	SPDK_NVME_RESERVE_CLEAR			= 0x1,
	/* [한국어] 0x1 — Clear. 모든 등록 해제 + reservation clear (기존 owner만 가능). */
};

/**
 * Reservation notification log page type
 */
enum spdk_nvme_reservation_notification_log_page_type {
	SPDK_NVME_RESERVATION_LOG_PAGE_EMPTY	= 0x0,
	SPDK_NVME_REGISTRATION_PREEMPTED	= 0x1,
	SPDK_NVME_RESERVATION_RELEASED		= 0x2,
	SPDK_NVME_RESERVATION_PREEMPTED		= 0x3,
};

/**
 * Reservation notification log
 */
struct spdk_nvme_reservation_notification_log {
	/** 64-bit incrementing reservation notification log page count */
	uint64_t	log_page_count;
	/** Reservation notification log page type */
	uint8_t		type;
	/** Number of additional available reservation notification log pages */
	uint8_t		num_avail_log_pages;
	uint8_t		reserved[2];
	uint32_t	nsid;
	uint8_t		reserved1[48];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_reservation_notification_log) == 64, "Incorrect size");

/* Mask Registration Preempted Notification */
#define SPDK_NVME_REGISTRATION_PREEMPTED_MASK	(1U << 1)
/* Mask Reservation Released Notification */
#define SPDK_NVME_RESERVATION_RELEASED_MASK	(1U << 2)
/* Mask Reservation Preempted Notification */
#define SPDK_NVME_RESERVATION_PREEMPTED_MASK	(1U << 3)

/**
 * Log page identifiers for SPDK_NVME_OPC_GET_LOG_PAGE
 */
/* [한국어] === Log Page Identifier (LID) — Get Log Page (CDW10[LID]) ===
 * NVMe Base 2.0 §5.16. 컨트롤러 통계/상태/이벤트를 풀(pull) 방식으로 회수. AER가 통지하면
 * 호스트가 해당 LID로 Get Log Page를 발행해 상세 데이터 수확. SPDK는 spdk_nvme_ctrlr_cmd_
 * get_log_page() 공개 API로 모든 LID 조회 가능. */
enum spdk_nvme_log_page {
	/** Supported log pages (optional) */
	SPDK_NVME_LOG_SUPPORTED_LOG_PAGES	= 0x00,
	/* [한국어] 0x00 — Supported Log Pages (NVMe 2.0). 1024B. struct
	 * spdk_nvme_supported_log_pages — LID별 지원 여부 비트맵. */

	/** Error information (mandatory) - \ref spdk_nvme_error_information_entry */
	SPDK_NVME_LOG_ERROR			= 0x01,
	/* [한국어] 0x01 — Error Information (필수). 64B 엔트리 배열, 컨트롤러가 보관한 최근
	 * 에러 N개. struct spdk_nvme_error_information_entry — error count, SQID, CID,
	 * status, parameter error location, LBA, NSID, vendor specific. */

	/** SMART / health information (mandatory) - \ref spdk_nvme_health_information_page */
	SPDK_NVME_LOG_HEALTH_INFORMATION	= 0x02,
	/* [한국어] 0x02 — SMART/Health (필수). 512B. struct spdk_nvme_health_information_page —
	 * critical_warning, temperature, available_spare, percentage_used, data_units_r/w,
	 * host_r/w_commands, controller_busy_time, power_cycles, power_on_hours, unsafe_shutdowns,
	 * media_errors, num_error_info_log_entries. nvme-cli/spdk_nvme_perf 진단의 핵심. */

	/** Firmware slot information (mandatory) - \ref spdk_nvme_firmware_page */
	SPDK_NVME_LOG_FIRMWARE_SLOT		= 0x03,
	/* [한국어] 0x03 — Firmware Slot (필수). 512B. AFI(active firmware info) +
	 * 7개 firmware revision string. */

	/** Changed namespace list (optional) */
	SPDK_NVME_LOG_CHANGED_NS_LIST	= 0x04,
	/* [한국어] 0x04 — Changed NS list. 4096B(uint32_t[1024]). NS attribute 변경 AER 후
	 * 어떤 NS가 바뀌었는지 회수. */

	/** Command effects log (optional) */
	SPDK_NVME_LOG_COMMAND_EFFECTS_LOG	= 0x05,
	/* [한국어] 0x05 — Commands Supported and Effects. 4096B. 각 admin/I/O opcode가 NS/
	 * Controller capability에 미치는 영향(CSUPP, LBCC, NCC, NIC, CCC). NVMe-oF passthrough
	 * 안전성 판단에 사용. */

	/** Device self test (optional) */
	SPDK_NVME_LOG_DEVICE_SELF_TEST	= 0x06,
	/* [한국어] 0x06 — Device Self-Test 결과. */

	/** Host initiated telemetry log (optional) */
	SPDK_NVME_LOG_TELEMETRY_HOST_INITIATED	= 0x07,
	/* [한국어] 0x07 — Host-initiated Telemetry. 호스트가 cdw10[LSP].bit0=1로 새 capture
	 * 트리거 후 회수. struct spdk_nvme_telemetry_log_page_hdr 헤더 + 가변 데이터 영역
	 * (3 data area). 펌웨어 디버그 덤프. */

	/** Controller initiated telemetry log (optional) */
	SPDK_NVME_LOG_TELEMETRY_CTRLR_INITIATED	= 0x08,
	/* [한국어] 0x08 — Controller-initiated Telemetry. 컨트롤러가 critical event 발생 시
	 * AER로 통지 후 호스트가 회수. */

	/** Endurance group Information (optional) */
	SPDK_NVME_LOG_ENDURANCE_GROUP_INFORMATION	= 0x09,
	/* [한국어] 0x09 — Endurance Group Info. */

	/** Predictable latency per NVM set (optional) */
	SPDK_NVME_LOG_PREDICATBLE_LATENCY	= 0x0A,
	/* [한국어] 0x0A — Predictable Latency Mode (PLM) per NVM set. */

	/** Predictable latency event aggregate (optional) */
	SPDK_NVME_LOG_PREDICTABLE_LATENCY_EVENT	= 0x0B,
	/* [한국어] 0x0B — PLM 이벤트 집계. */

	/** Asymmetric namespace access log (optional) */
	SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS = 0x0C,
	/* [한국어] 0x0C — ANA Log Page. struct spdk_nvme_ana_page + ANA Group Descriptor
	 * 배열. 각 NSID의 ANA state(Optimized/Non-Optimized/Inaccessible/Persistent Loss/
	 * Change). NVMe-oF multipath의 핵심. */

	/** Persistent event log (optional) */
	SPDK_NVME_LOG_PERSISTENT_EVENT_LOG = 0x0D,
	/* [한국어] 0x0D — Persistent Event Log. 전원 사이클을 넘어 보존되는 이벤트 기록. */

	/* 0x0E NVM command set specific */

	/** Endurance group event aggregate (optional) */
	SPDK_NVME_LOG_ENDURANCE_GROUP_EVENT = 0x0F,
	/* [한국어] 0x0F — Endurance Group Event. */

	/** Media unit status (optional) */
	SPDK_NVME_LOG_MEDIA_UNIT_STATUS = 0x10,
	/* [한국어] 0x10 — Media Unit Status. */

	/** Supported capacity configuration list (optional) */
	SPDK_NVME_LOG_CAPACITY_CONFIGURATION_LIST	= 0x11,
	/* [한국어] 0x11 — Capacity Configuration List. */

	/** Feature identifiers supported and effects (optional) */
	SPDK_NVME_LOG_FEATURE_IDS_EFFECTS	= 0x12,
	/* [한국어] 0x12 — Feature Identifiers Supported and Effects. struct
	 * spdk_nvme_feature_ids_effects_log_page (1024B). 각 FID 지원/저장 가능/CC 영향. */

	/** NVMe-MI commands supported and effects (optional) */
	SPDK_NVME_LOG_NVME_MI_COMMANDS_EFFECTS	= 0x13,
	/* [한국어] 0x13 — NVMe-MI 명령 effects. */

	/** Command and feature lockdown (optional) */
	SPDK_NVME_LOG_COMMAND_FEATURE_LOCKDOWN	= 0x14,
	/* [한국어] 0x14 — Command/Feature Lockdown. */

	/** Boot partition (optional) */
	SPDK_NVME_LOG_BOOT_PARTITION	= 0x15,
	/* [한국어] 0x15 — Boot Partition log. */

	/** Rotational media information (optional) */
	SPDK_NVME_LOG_ROTATIONAL_MEDIA_INFORMATION	= 0x16,
	/* [한국어] 0x16 — Rotational Media (HDD)에 대한 NVMe HDD 표준 정보. */

	/* 0x17-0x1f - reserved */

	/** FDP configurations (optional) */
	SPDK_NVME_LOG_FDP_CONFIGURATIONS	= 0x20,
	/* [한국어] 0x20 — FDP Configurations (TP4146). struct spdk_nvme_fdp_cfg_log_page +
	 * cfg_descriptor 배열. */

	/** Reclaim unit handle usage (optional) */
	SPDK_NVME_LOG_RECLAIM_UNIT_HANDLE_USAGE	= 0x21,
	/* [한국어] 0x21 — FDP RUH usage. struct spdk_nvme_fdp_ruhu_log_page. */

	/** FDP statistics (optional) */
	SPDK_NVME_LOG_FDP_STATISTICS	= 0x22,
	/* [한국어] 0x22 — FDP statistics. struct spdk_nvme_fdp_stats_log_page. */

	/** FDP events (optional) */
	SPDK_NVME_LOG_FDP_EVENTS	= 0x23,
	/* [한국어] 0x23 — FDP events log. struct spdk_nvme_fdp_events_log_page. */

	/* 0x24-0x6f - reserved */

	/** Discovery(refer to the NVMe over Fabrics specification) */
	SPDK_NVME_LOG_DISCOVERY		= 0x70,
	/* [한국어] 0x70 — NVMe-oF Discovery Log Page. NVMe-oF Discovery 컨트롤러에서 listen
	 * 가능한 모든 NQN/transport/주소 회수. */

	/* 0x71-0x7f - reserved for NVMe over Fabrics */

	/** Reservation notification (optional) */
	SPDK_NVME_LOG_RESERVATION_NOTIFICATION	= 0x80,
	/* [한국어] 0x80 — Reservation Notification. struct spdk_nvme_reservation_notification_log
	 * (64B). reservation 관련 AER 후 회수. */

	/** Sanitize status (optional) */
	SPDK_NVME_LOG_SANITIZE_STATUS = 0x81,
	/* [한국어] 0x81 — Sanitize Status. struct spdk_nvme_sanitize_status_log_page (512B).
	 * 진행률(SPROG), 상태(SSTAT), 작업 ID(SCDW10), 추정 완료 시간 등. */

	/* 0x82-0xBE - I/O command set specific */

	/** Changed zone list (refer to Zoned Namespace command set) */
	SPDK_NVME_LOG_CHANGED_ZONE_LIST = 0xBF,
	/* [한국어] 0xBF — ZNS Changed Zone List. zone state 변경 AER 후 회수. */

	/* 0xC0-0xFF - vendor specific */
	SPDK_NVME_LOG_VENDOR_SPECIFIC_START	= 0xc0,
	/* [한국어] 0xC0 — vendor-specific 영역 시작. nvme_intel.h 등에서 정의. */
	SPDK_NVME_LOG_VENDOR_SPECIFIC_END	= 0xff,
	/* [한국어] 0xFF — vendor-specific 영역 끝. */
};

#define spdk_nvme_log_page_is_vendor_specific(lid) ((lid) >= SPDK_NVME_LOG_VENDOR_SPECIFIC_START)

struct spdk_nvme_supported_log_pages {
	/* Log Page Identifier Supported 0-255 */
	struct {
		/* LID Supported - 0 */
		uint32_t lsupp		: 1;
		/* Index Offset Supported - 1 */
		uint32_t ios		: 1;
		/* Reserved - 2:15 */
		uint32_t reserved	: 14;
		/* LID Specific Parameter - 16:31 */
		uint32_t lidsp		: 16;
	} lids[256];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_supported_log_pages) == 1024, "Incorrect size");

/* Feature Identifiers Effects Log Page */
struct spdk_nvme_feature_ids_effects_log_page {
	/* Feature Identifier Supported 0-255 */
	struct {
		/* FID Supported - 0 */
		uint32_t fsupp		: 1;
		/* User Data Content Change - 1 */
		uint32_t udcc		: 1;
		/* Namespace Capability Change - 2 */
		uint32_t ncc		: 1;
		/* Namespace Inventory Change - 3 */
		uint32_t nic		: 1;
		/* Controller Capability Change - 4 */
		uint32_t ccc		: 1;
		/* Reserved - 5:18 */
		uint32_t reserved	: 14;
		/* UUID Selection Supported - 19 */
		uint32_t uss		: 1;

		/* FID scope (FSP) - 20:31 */
		/* FID scope - Namespace Scope - 0 */
		uint32_t nscpe		: 1;
		/* FID scope - Controller Scope - 1 */
		uint32_t cscpe		: 1;
		/* FID scope - NVM Set Scope - 2 */
		uint32_t nsetscpe	: 1;
		/* FID scope - Endurance Group Scope - 3 */
		uint32_t egscpe		: 1;
		/* FID scope - Domain Scope - 4 */
		uint32_t dscpe		: 1;
		/* FID scope - NVM Subsystem Scope - 5 */
		uint32_t nsscpe		: 1;
		/* FID scope - Controller Data Queue - 6 */
		uint32_t cdqscpe	: 1;
		/* FID scope - Reserved - 7:11 */
		uint32_t fsp_reserved	: 5;
	} fis[256];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_feature_ids_effects_log_page) == 1024, "Incorrect size");

/**
 * Error information log page (\ref SPDK_NVME_LOG_ERROR)
 */
struct spdk_nvme_error_information_entry {
	uint64_t		error_count;
	uint16_t		sqid;
	uint16_t		cid;
	struct spdk_nvme_status	status;
	uint16_t		error_location;
	uint64_t		lba;
	uint32_t		nsid;
	uint8_t			vendor_specific;
	uint8_t			trtype;
	uint8_t			reserved30[2];
	uint64_t		command_specific;
	uint16_t		trtype_specific;
	uint8_t			reserved42[22];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_error_information_entry) == 64, "Incorrect size");

/**
 * SMART / health information page (\ref SPDK_NVME_LOG_HEALTH_INFORMATION)
 */
#pragma pack(push, 1)
struct spdk_nvme_health_information_page {
	union spdk_nvme_critical_warning_state	critical_warning;

	uint16_t		temperature;
	uint8_t			available_spare;
	uint8_t			available_spare_threshold;
	uint8_t			percentage_used;

	uint8_t			reserved[26];

	/*
	 * Note that the following are 128-bit values, but are
	 *  defined as an array of 2 64-bit values.
	 */
	/* Data Units Read is always in 512-byte units. */
	uint64_t		data_units_read[2];
	/* Data Units Written is always in 512-byte units. */
	uint64_t		data_units_written[2];
	/* For NVM command set, this includes Compare commands. */
	uint64_t		host_read_commands[2];
	uint64_t		host_write_commands[2];
	/* Controller Busy Time is reported in minutes. */
	uint64_t		controller_busy_time[2];
	uint64_t		power_cycles[2];
	uint64_t		power_on_hours[2];
	uint64_t		unsafe_shutdowns[2];
	uint64_t		media_errors[2];
	uint64_t		num_error_info_log_entries[2];
	/* Controller temperature related. */
	uint32_t		warning_temp_time;
	uint32_t		critical_temp_time;
	uint16_t		temp_sensor[8];

	uint8_t			reserved2[296];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_health_information_page) == 512, "Incorrect size");
#pragma pack(pop)

/* Commands Supported and Effects Data Structure */
struct spdk_nvme_cmds_and_effect_entry {
	/** Command Supported */
	uint16_t csupp : 1;

	/** Logic Block Content Change  */
	uint16_t lbcc  : 1;

	/** Namespace Capability Change */
	uint16_t ncc   : 1;

	/** Namespace Inventory Change */
	uint16_t nic   : 1;

	/** Controller Capability Change */
	uint16_t ccc   : 1;

	uint16_t reserved1 : 11;

	/* Command Submission and Execution recommendation
	 * 000 - No command submission or execution restriction
	 * 001 - Submitted when there is no outstanding command to same NS
	 * 010 - Submitted when there is no outstanding command to any NS
	 * others - Reserved
	 * \ref command_submission_and_execution in section 5.14.1.5 NVMe Revision 1.3
	 */
	uint16_t cse : 3;

	/** UUID Selection Supported */
	uint16_t uss : 1;

	/** Command Scope bits (CSP) */

	/** Namespace Scope */
	uint16_t nscpe		: 1;

	/** Controller Scope */
	uint16_t cscpe		: 1;

	/** NVM Set Scope */
	uint16_t nsetscpe	: 1;

	/** Endurance Group Scope */
	uint16_t egscpe		: 1;

	/** Domain Scope */
	uint16_t dscpe		: 1;

	/** NVM Subsystem Scope */
	uint16_t nsscpe		: 1;

	uint16_t csp_reserved	: 6;
};

/* Commands Supported and Effects Log Page */
struct spdk_nvme_cmds_and_effect_log_page {
	/** Commands Supported and Effects Data Structure for the Admin Commands */
	struct spdk_nvme_cmds_and_effect_entry admin_cmds_supported[256];

	/** Commands Supported and Effects Data Structure for the IO Commands */
	struct spdk_nvme_cmds_and_effect_entry io_cmds_supported[256];

	uint8_t reserved0[2048];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_cmds_and_effect_log_page) == 4096, "Incorrect size");

/*
 * Get Log Page – Telemetry Host/Controller Initiated Log (Log Identifiers 07h/08h)
 */
/* [한국어] === struct spdk_nvme_telemetry_log_page_hdr === LID=0x07/0x08 Telemetry log 헤더(512B).
 * NVMe Base 2.0 §5.16.1.13. 펌웨어 디버그 덤프(controller internal logs/state) 회수. 헤더 뒤에
 * data area 1/2/3가 가변 길이로 이어지며, 각 area의 last block 포인터로 끝 위치를 알 수 있다.
 * Host-initiated(LID=0x07): 호스트가 LSP.bit0=1로 새 capture 트리거 후 회수.
 * Controller-initiated(LID=0x08): 컨트롤러가 critical event 발생 시 AER로 통지 후 호스트가 회수. */
struct spdk_nvme_telemetry_log_page_hdr {
	/* Log page identifier */
	uint8_t    lpi;
	/* [한국어] [byte 0] LPI — Log Page ID 사본 (0x07 또는 0x08). */
	uint8_t    rsvd[4];
	/* [한국어] [bytes 1-4] reserved. */
	uint8_t    ieee_oui[3];
	/* [한국어] [bytes 5-7] IEEE OUI — 컨트롤러 vendor의 OUI. parser가 vendor 별 데이터 해석 분기. */
	/* Data area 1 last block */
	uint16_t   dalb1;
	/* [한국어] [bytes 8-9] DALB1 — Data Area 1의 last block 인덱스(512B 단위). 핵심 정보(보통). */
	/* Data area 2 last block */
	uint16_t   dalb2;
	/* [한국어] [bytes 10-11] DALB2 — Data Area 2 last block (확장 진단). */
	/* Data area 3 last block */
	uint16_t   dalb3;
	/* [한국어] [bytes 12-13] DALB3 — Data Area 3 last block (벤더 specific). */
	uint8_t    rsvd1[368];
	/* [한국어] [bytes 14-381] reserved. */
	/* Controller initiated data avail */
	uint8_t    ctrlr_avail;
	/* [한국어] [byte 382] — Controller-initiated telemetry 데이터 가용 여부 (0=없음, 1=있음). */
	/* Controller initiated telemetry data generation */
	uint8_t    ctrlr_gen;
	/* [한국어] [byte 383] — Controller-initiated 데이터 generation 번호. 새 capture마다 증가. */
	/* Reason identifier */
	uint8_t    rsnident[128];
	/* [한국어] [bytes 384-511] RSNIDENT — capture 발생 사유 식별자(vendor 정의). */
	uint8_t    telemetry_datablock[0];
	/* [한국어] flexible end — 헤더 직후 data area 1/2/3가 연속해 위치. 호스트는 dalb1/2/3로 size 계산. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_telemetry_log_page_hdr) == 512, "Incorrect size");

/**
 * Sanitize Status Type
 */
enum spdk_nvme_sanitize_status_type {
	SPDK_NVME_NEVER_BEEN_SANITIZED		= 0x0,
	SPDK_NVME_RECENT_SANITIZE_SUCCESSFUL	= 0x1,
	SPDK_NVME_SANITIZE_IN_PROGRESS		= 0x2,
	SPDK_NVME_SANITIZE_FAILED		= 0x3,
};

/**
 * Sanitize status sstat field
 */
struct spdk_nvme_sanitize_status_sstat {
	uint16_t status			: 3;
	uint16_t complete_pass		: 5;
	uint16_t global_data_erase	: 1;
	uint16_t reserved		: 7;
};

/**
 * Sanitize log page
 */
struct spdk_nvme_sanitize_status_log_page {
	/* Sanitize progress */
	uint16_t				sprog;
	/* Sanitize status */
	struct spdk_nvme_sanitize_status_sstat	sstat;
	/* CDW10 of sanitize command */
	uint32_t				scdw10;
	/* Estimated overwrite time in seconds */
	uint32_t				et_overwrite;
	/* Estimated block erase time in seconds */
	uint32_t				et_block_erase;
	/* Estimated crypto erase time in seconds */
	uint32_t				et_crypto_erase;
	uint8_t					reserved[492];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_sanitize_status_log_page) == 512, "Incorrect size");

/**
 * Asynchronous Event Type
 */
/* [한국어] === Async Event Type (CQE.CDW0[2:0]) ===
 * NVMe Base 2.0 §5.2.1. AER 명령(0x0c)의 완료 CDW0에서 type/info/log_page_id를 추출.
 * type으로 카테고리 구분 후 info로 세부 사유 식별, log_page_identifier로 어떤 LID를
 * Get Log Page로 회수해야 하는지 안내. */
enum spdk_nvme_async_event_type {
	/* Error Status */
	SPDK_NVME_ASYNC_EVENT_TYPE_ERROR	= 0x0,
	/* [한국어] 0x0 — 컨트롤러 내부 오류(persistent/transient/diagnostic). 호스트는 보통
	 * Error Information log(LID=0x01)를 회수해 자세한 정보 확인. */
	/* SMART/Health Status */
	SPDK_NVME_ASYNC_EVENT_TYPE_SMART	= 0x1,
	/* [한국어] 0x1 — SMART/Health 임계 초과(온도/spare/reliability). LID=0x02 회수. */
	/* Notice */
	SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE	= 0x2,
	/* [한국어] 0x2 — Notice 카테고리(NS attribute 변경, FW activation, ANA change,
	 * Discovery log 변경). LID는 info에 따라 0x04/0x0C/0x70 등. */
	/* 0x3 - 0x5 Reserved */

	/* I/O Command Set Specific Status */
	SPDK_NVME_ASYNC_EVENT_TYPE_IO		= 0x6,
	/* [한국어] 0x6 — I/O CS 특화 이벤트(Reservation log available, Sanitize 완료,
	 * Zone Descriptor 변경). LID=0x80(Reservation), 0x81(Sanitize), 0xBF(ZNS) 등. */
	/* Vendor Specific */
	SPDK_NVME_ASYNC_EVENT_TYPE_VENDOR	= 0x7,
	/* [한국어] 0x7 — vendor 정의 이벤트. */
};

/**
 * Asynchronous Event Information for Error Status
 */
enum spdk_nvme_async_event_info_error {
	/* Write to Invalid Doorbell Register */
	SPDK_NVME_ASYNC_EVENT_WRITE_INVALID_DB		= 0x0,
	/* [한국어] 0x0 — 호스트가 잘못된 doorbell 주소에 write — 보통 SPDK 버그(잘못된 stride
	 * 계산 등) 시 발생. */
	/* Invalid Doorbell Register Write Value */
	SPDK_NVME_ASYNC_EVENT_INVALID_DB_WRITE		= 0x1,
	/* [한국어] 0x1 — doorbell에 큐 깊이 초과/0 등 잘못된 값 write. */
	/* Diagnostic Failure */
	SPDK_NVME_ASYNC_EVENT_DIAGNOSTIC_FAILURE	= 0x2,
	/* [한국어] 0x2 — Device Self-Test 실패. */
	/* Persistent Internal Error */
	SPDK_NVME_ASYNC_EVENT_PERSISTENT_INTERNAL	= 0x3,
	/* [한국어] 0x3 — 영구 내부 오류 — 컨트롤러 reset 필요. */
	/* Transient Internal Error */
	SPDK_NVME_ASYNC_EVENT_TRANSIENT_INTERNAL	= 0x4,
	/* [한국어] 0x4 — 일시적 내부 오류 — retry로 복구 가능. */
	/* Firmware Image Load Error */
	SPDK_NVME_ASYNC_EVENT_FW_IMAGE_LOAD		= 0x5,
	/* [한국어] 0x5 — 펌웨어 이미지 로딩 실패. */

	/* 0x6 - 0xFF Reserved */
};

/**
 * Asynchronous Event Information for SMART/Health Status
 */
enum spdk_nvme_async_event_info_smart {
	/* NVM Subsystem Reliability */
	SPDK_NVME_ASYNC_EVENT_SUBSYSTEM_RELIABILITY	= 0x0,
	/* [한국어] 0x0 — 신뢰성 저하(media wearout 임박 등). */
	/* Temperature Threshold */
	SPDK_NVME_ASYNC_EVENT_TEMPERATURE_THRESHOLD	= 0x1,
	/* [한국어] 0x1 — 온도 임계 초과/미만 — Set Features TEMPERATURE_THRESHOLD(0x04) 임계. */
	/* Spare Below Threshold */
	SPDK_NVME_ASYNC_EVENT_SPARE_BELOW_THRESHOLD	= 0x2,
	/* [한국어] 0x2 — Available Spare 임계 미만. */

	/* 0x3 - 0xFF Reserved */
};

/**
 * Asynchronous Event Information for Notice
 */
enum spdk_nvme_async_event_info_notice {
	/* Namespace Attribute Changed */
	SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED		= 0x0,
	/* [한국어] 0x0 — NS attribute 변경(NS create/delete, capacity 변경 등). LID=0x04
	 * Changed NS list 회수. */
	/* Firmware Activation Starting */
	SPDK_NVME_ASYNC_EVENT_FW_ACTIVATION_START	= 0x1,
	/* [한국어] 0x1 — Firmware activation 시작 — 호스트는 곧 컨트롤러 reset 가능성 대비. */
	/* Telemetry Log Changed */
	SPDK_NVME_ASYNC_EVENT_TELEMETRY_LOG_CHANGED	= 0x2,
	/* [한국어] 0x2 — controller-initiated telemetry log 갱신. LID=0x08 회수. */
	/* Asymmetric Namespace Access Change */
	SPDK_NVME_ASYNC_EVENT_ANA_CHANGE		= 0x3,
	/* [한국어] 0x3 — ANA state 변경. LID=0x0C 회수해 path 재계산. NVMe-oF multipath 핵심. */

	/* 0x4 - 0xEF Reserved */

	/** Discovery log change event(refer to the NVMe over Fabrics specification) */
	SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE	= 0xF0,
	/* [한국어] 0xF0 — Discovery 컨트롤러의 등록 정보 변경. LID=0x70 회수. */

	/* 0xF1 - 0xFF Reserved */
};

/**
 * Asynchronous Event Information for NVM Command Set Specific Status
 */
enum spdk_nvme_async_event_info_nvm_command_set {
	/* Reservation Log Page Available */
	SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL	= 0x0,
	/* [한국어] 0x0 — Reservation 관련 이벤트. LID=0x80 회수. */
	/* Sanitize Operation Completed */
	SPDK_NVME_ASYNC_EVENT_SANITIZE_COMPLETED	= 0x1,
	/* [한국어] 0x1 — Sanitize 완료. LID=0x81로 결과 확인. */

	/* 0x2 - 0xFF Reserved */
};

/**
 * Asynchronous Event Request Completion
 */
/* [한국어] === AER Completion CDW0 인코딩 ===
 * AER 명령의 CQE.CDW0에 type+info+log_page_id가 packed로 담긴다. SPDK는 lib/nvme/nvme_ctrlr.c
 * ::nvme_ctrlr_async_event_cb에서 이 union으로 디코딩 → 사용자 등록 콜백 호출 → 필요 시
 * Get Log Page로 후속 데이터 회수. */
union spdk_nvme_async_event_completion {
	uint32_t raw;
	struct {
		uint32_t async_event_type	: 3;
		/* [한국어] [2:0] type — enum spdk_nvme_async_event_type. */
		uint32_t reserved1		: 5;
		uint32_t async_event_info	: 8;
		/* [한국어] [15:8] info — type별 enum spdk_nvme_async_event_info_*. */
		uint32_t log_page_identifier	: 8;
		/* [한국어] [23:16] LID — 호스트가 후속 Get Log Page에 사용할 ID. */
		uint32_t reserved2		: 8;
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_async_event_completion) == 4, "Incorrect size");

/**
 * Firmware slot information page (\ref SPDK_NVME_LOG_FIRMWARE_SLOT)
 */
struct spdk_nvme_firmware_page {
	struct {
		uint8_t	active_slot	: 3; /**< Slot for current FW */
		uint8_t	reserved3	: 1;
		uint8_t	next_reset_slot	: 3; /**< Slot that will be active at next controller reset */
		uint8_t	reserved7	: 1;
	} afi;

	uint8_t			reserved[7];
	uint8_t			revision[7][8]; /** Revisions for 7 slots (ASCII strings) */
	uint8_t			reserved2[448];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_firmware_page) == 512, "Incorrect size");

/**
 * Asymmetric Namespace Access page (\ref SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS)
 */
struct spdk_nvme_ana_page {
	uint64_t change_count;
	uint16_t num_ana_group_desc;
	uint8_t reserved[6];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ana_page) == 16, "Incorrect size");

/* Asymmetric namespace access state */
/* [한국어] === ANA(Asymmetric Namespace Access) State === NVMe Base 1.4+ §8.20.
 * NVMe-oF multipath의 핵심. (controller, ANA group)별 상태로 호스트가 어느 path를 우선
 * 사용할지 결정. NVMe-oF에서 host는 ANA log(LID=0x0C)와 ANA Change AER로 상태 추적. */
enum spdk_nvme_ana_state {
	SPDK_NVME_ANA_OPTIMIZED_STATE		= 0x1,
	/* [한국어] 0x1 — Optimized. 최적 경로. 호스트는 이 path 우선 사용. */
	SPDK_NVME_ANA_NON_OPTIMIZED_STATE	= 0x2,
	/* [한국어] 0x2 — Non-Optimized. 사용 가능하나 latency/throughput 저하 가능. fail-over
	 * 백업 경로. */
	SPDK_NVME_ANA_INACCESSIBLE_STATE	= 0x3,
	/* [한국어] 0x3 — Inaccessible. 일시적으로 접근 불가(상태 전이 중 등). retry로 대체. */
	SPDK_NVME_ANA_PERSISTENT_LOSS_STATE	= 0x4,
	/* [한국어] 0x4 — Persistent Loss. 영구 접근 불가(컨트롤러 down 등). 호스트가 path 제거. */
	SPDK_NVME_ANA_CHANGE_STATE		= 0xF,
	/* [한국어] 0xF — Change. ANA group 상태가 바뀌는 중. 호스트는 곧 새 상태로 갱신될 것을
	 * 기대하고 retry 또는 path 재계산. */
};

/* ANA group descriptor */
struct spdk_nvme_ana_group_descriptor {
	uint32_t ana_group_id;
	uint32_t num_of_nsid;
	uint64_t change_count;

	uint8_t ana_state : 4;
	uint8_t reserved0 : 4;

	uint8_t reserved1[15];

	uint32_t nsid[];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ana_group_descriptor) == 32, "Incorrect size");

/* Reclaim unit handle type */
enum spdk_nvme_fdp_ruh_type {
	/* 0x0 Reserved */

	/* Reclaim unit handle type initially isolated */
	SPDK_NVME_FDP_RUHT_INITIALLY_ISOLATED		= 0x1,
	/* Reclaim unit handle type persistently isolated */
	SPDK_NVME_FDP_RUHT_PERSISTENTLY_ISOLATED	= 0x2,

	/* 0x3 - 0xBF Reserved */

	/* 0xC0 - 0xFF Vendor specific */
};

/* Reclaim unit handle descriptor */
/* [한국어] === struct spdk_nvme_fdp_ruh_descriptor === FDP TP4146. cfg_descriptor 끝의 RUH
 * 종류 entry 배열 element(4B). */
struct spdk_nvme_fdp_ruh_descriptor {
	/* Reclaim unit handle type */
	uint8_t ruht;
	/* [한국어] [byte 0] RUHT — Reclaim Unit Handle Type. 0x1=Initially Isolated,
	 * 0x2=Persistently Isolated (enum spdk_nvme_fdp_ruh_type). */
	uint8_t reserved[3];
	/* [한국어] [bytes 1-3] reserved. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fdp_ruh_descriptor) == 4, "Incorrect size");

/* FDP configuration descriptor */
/* [한국어] === struct spdk_nvme_fdp_cfg_descriptor === FDP TP4146. FDP Configurations log page
 * (LID=0x20)의 한 구성 entry. 컨트롤러는 여러 가능한 FDP 구성(FDPCI 인덱스로 선택)을 노출하고
 * 호스트는 FID=0x1d set 시 그 중 하나 선택. */
struct spdk_nvme_fdp_cfg_descriptor {
	/* Descriptor size */
	uint16_t ds;
	/* [한국어] [bytes 0-1] DS — descriptor 총 길이(64 + len(ruh_desc[])*4). */

	/* FDP attributes */
	union {
		/* [한국어] FDPA — FDP Attributes (1B). */
		uint8_t raw;
		struct {
			/* Reclaim group identifier format */
			uint8_t rgif	: 4;
			/* [한국어] [3:0] RGIF — Reclaim Group ID 비트 폭(0=group 미사용). */
			/* FDP volatile write cache */
			uint8_t fdpvwc	: 1;
			/* [한국어] [4] fdpvwc — 1=FDP에 휘발성 write cache 영향 있음. */
			uint8_t rsvd1	: 2;
			/* [한국어] [6:5] reserved. */
			/* FDP configuration valid */
			uint8_t fdpcv	: 1;
			/* [한국어] [7] fdpcv — 1=이 구성이 FID=0x1d로 선택 가능. */
		} bits;
	} fdpa;

	/* Vendor specific size */
	uint8_t vss;
	/* [한국어] [byte 3] VSS — vendor specific 영역 크기. */
	/* Number of reclaim groups */
	uint32_t nrg;
	/* [한국어] [bytes 4-7] NRG — Reclaim Group 수. */
	/* Number of reclaim unit handles */
	uint16_t nruh;
	/* [한국어] [bytes 8-9] NRUH — RUH 수 (이후 ruh_desc[] 길이). */
	/* Max placement identifiers */
	uint16_t maxpids;
	/* [한국어] [bytes 10-11] MAXPIDS — 최대 placement identifier 수. */
	/* Number of namespaces supported */
	uint32_t nns;
	/* [한국어] [bytes 12-15] NNS — 이 구성이 지원하는 NS 수. */
	/* Reclaim unit nominal size */
	uint64_t runs;
	/* [한국어] [bytes 16-23] RUNS — Reclaim Unit Nominal Size (bytes). NAND erase block 단위. */
	/* Estimated reclaim unit time limit */
	uint32_t erutl;
	/* [한국어] [bytes 24-27] ERUTL — Estimated Reclaim Unit Time Limit (sec). */
	uint8_t rsvd28[36];
	/* [한국어] [bytes 28-63] reserved. */
	struct spdk_nvme_fdp_ruh_descriptor ruh_desc[];
	/* [한국어] flexible array — NRUH 개의 RUH descriptor (4B씩). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fdp_cfg_descriptor) == 64, "Incorrect size");

/* FDP configurations log page (\ref SPDK_NVME_LOG_FDP_CONFIGURATIONS) */
/* [한국어] === struct spdk_nvme_fdp_cfg_log_page === LID=0x20. Get Log Page로 회수해
 * 컨트롤러가 노출하는 FDP 구성 목록 확인. */
struct spdk_nvme_fdp_cfg_log_page {
	/* Number of FDP configurations */
	uint16_t ncfg;
	/* [한국어] [bytes 0-1] NCFG — cfg_desc[] 엔트리 수. */
	/* Version of log page */
	uint8_t version;
	/* [한국어] [byte 2] version — log page format 버전. */
	uint8_t reserved1;
	/* [한국어] [byte 3] reserved. */
	/* Size of this log page in bytes */
	uint32_t size;
	/* [한국어] [bytes 4-7] size — log page 총 길이(byte). */
	uint8_t reserved2[8];
	/* [한국어] [bytes 8-15] reserved. */
	struct spdk_nvme_fdp_cfg_descriptor cfg_desc[];
	/* [한국어] flexible array — NCFG 개의 cfg_descriptor. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fdp_cfg_log_page) == 16, "Incorrect size");

/* Reclaim unit handle attributes */
enum spdk_nvme_fdp_ruh_attributes {
	/* Not used by a namespace */
	SPDK_NVME_FDP_RUHA_UNUSED		= 0x0,
	/* Use a specific reclaim unit handle */
	SPDK_NVME_FDP_RUHA_HOST_SPECIFIED	= 0x1,
	/* Use the only default reclaim unit handle  */
	SPDK_NVME_FDP_RUHA_CTRLR_SPECIFIED	= 0x2,

	/* 0x3 - 0xFF Reserved */
};

/* Reclaim unit handle usage descriptor */
/* [한국어] === struct spdk_nvme_fdp_ruhu_descriptor === RUH usage log entry(8B). 각 RUH가
 * 어떤 attribute(unused/host-specified/ctrlr-specified)로 활용되는지 보고. */
struct spdk_nvme_fdp_ruhu_descriptor {
	/* Reclaim unit handle attributes */
	uint8_t ruha;
	/* [한국어] [byte 0] RUHA — enum spdk_nvme_fdp_ruh_attributes (0=Unused, 1=Host Specified,
	 * 2=Controller Specified). */
	uint8_t reserved[7];
	/* [한국어] [bytes 1-7] reserved. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fdp_ruhu_descriptor) == 8, "Incorrect size");

/* Reclaim unit handle usage log page (\ref SPDK_NVME_LOG_RECLAIM_UNIT_HANDLE_USAGE) */
/* [한국어] === struct spdk_nvme_fdp_ruhu_log_page === LID=0x21. Get Log Page로 RUH 사용 현황 확인. */
struct spdk_nvme_fdp_ruhu_log_page {
	/* Number of Reclaim Unit Handles */
	uint16_t nruh;
	/* [한국어] [bytes 0-1] NRUH — ruhu_desc[] 엔트리 수. */
	uint8_t reserved[6];
	/* [한국어] [bytes 2-7] reserved. */
	struct spdk_nvme_fdp_ruhu_descriptor ruhu_desc[];
	/* [한국어] flexible array. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fdp_ruhu_log_page) == 8, "Incorrect size");

/* FDP statistics log page (\ref SPDK_NVME_LOG_FDP_STATISTICS) */
/* [한국어] === struct spdk_nvme_fdp_stats_log_page === LID=0x22. FDP write amplification 추적
 * 통계. host write 대비 media write/erase 비율로 FDP 효과 측정. */
struct spdk_nvme_fdp_stats_log_page {
	/* Host bytes with metadata written */
	uint64_t hbmw[2];
	/* [한국어] [bytes 0-15] HBMW — Host가 보낸 write 바이트 수(metadata 포함, 128bit). */
	/* Media bytes with metadata written */
	uint64_t mbmw[2];
	/* [한국어] [bytes 16-31] MBMW — 실제 media에 write된 바이트 수(GC 포함, 128bit). MBMW/HBMW = WAF. */
	/* Media bytes erased */
	uint64_t mbe[2];
	/* [한국어] [bytes 32-47] MBE — Media에서 erase된 바이트 수(NAND erase). */
	uint8_t rsvd48[16];
	/* [한국어] [bytes 48-63] reserved. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fdp_stats_log_page) == 64, "Incorrect size");

/* FDP report event types (cdw10 log specific parameter) */
enum spdk_nvme_fdp_report_event_type {
	/* Report FDP controller events */
	SPDK_NVME_FDP_REPORT_CTRL_EVENTS	= 0x0,
	/* Report FDP host events */
	SPDK_NVME_FDP_REPORT_HOST_EVENTS	= 0x1,
};

/* FDP event type */
enum spdk_nvme_fdp_event_type {
	/* FDP host events */
	/* Reclaim unit not fully written to capacity */
	SPDK_NVME_FDP_EVENT_RU_NOT_WRITTEN_CAPACITY	= 0x0,
	/* Reclaim unit time limit exceeded */
	SPDK_NVME_FDP_EVENT_RU_TIME_LIMIT_EXCEEDED	= 0x1,
	/* Controller reset modified reclaim unit handles */
	SPDK_NVME_FDP_EVENT_CTRLR_RESET_MODIFY_RUH	= 0x2,
	/* Invalid placement identifier */
	SPDK_NVME_FDP_EVENT_INVALID_PLACEMENT_ID	= 0x3,

	/* 0x4 - 0x6F Reserved */

	/* 0x70 - 0x7F Vendor specific */

	/* FDP controller events */
	/* Media reallocated */
	SPDK_NVME_FDP_EVENT_MEDIA_REALLOCATED		= 0x80,
	/* Implicitly modified reclaim unit handle */
	SPDK_NVME_FDP_EVENT_IMPLICIT_MODIFIED_RUH	= 0x81,

	/* 0x82 - 0xEF Reserved */

	/* 0xF0 - 0xFF Vendor specific */
};

/* Media reallocated */
/* [한국어] === struct spdk_nvme_fdp_event_media_reallocated === Media Reallocated 이벤트 specific
 * 데이터(16B). FDP event.event_type_specific[]에 들어감. NAND wear-leveling으로 LBA가 새 미디어
 * 위치로 이동했을 때 호스트에 통지. */
#pragma pack(push, 1)
struct spdk_nvme_fdp_event_media_reallocated {
	/* Specific event flags */
	union {
		/* [한국어] SEF — Specific Event Flags (1B). */
		uint8_t raw;
		struct {
			/* LBA valid */
			uint8_t lbav		: 1;
			/* [한국어] [0] lbav — 1=lba 필드 유효. 0이면 ranges 미상. */
			uint8_t reserved	: 7;
			/* [한국어] [7:1] reserved. */
		} bits;
	} sef;

	uint8_t reserved1;
	/* [한국어] [byte 1] reserved. */
	/* Number of LBAs moved */
	uint16_t nlbam;
	/* [한국어] [bytes 2-3] NLBAM — 이동된 LBA 수. */
	/* Logical block address */
	uint64_t lba;
	/* [한국어] [bytes 4-11] LBA — 이동된 영역 시작 LBA. */
	uint8_t reserved2[4];
	/* [한국어] [bytes 12-15] reserved. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fdp_event_media_reallocated) == 16, "Incorrect size");

/* FDP event */
/* [한국어] === struct spdk_nvme_fdp_event === FDP events log(LID=0x23)의 한 entry(64B).
 * 컨트롤러가 발생한 이벤트(host event=RU not full, time exceeded, invalid pid 등 / ctrlr event=
 * Media Reallocated, Implicit RUH Modified)를 호스트에 보고. */
struct spdk_nvme_fdp_event {
	/* Event type */
	uint8_t etype;
	/* [한국어] [byte 0] ETYPE — enum spdk_nvme_fdp_event_type. */

	/* FDP event flags */
	union {
		/* [한국어] FDPEF — FDP Event Flags (1B). */
		uint8_t raw;
		struct {
			/* Placement identifier valid */
			uint8_t piv		: 1;
			/* [한국어] [0] piv — 1=pid 필드 유효. */
			/* NSID valid */
			uint8_t nsidv		: 1;
			/* [한국어] [1] nsidv — 1=nsid 유효. */
			/* Location valid */
			uint8_t lv		: 1;
			/* [한국어] [2] lv — 1=rgid/ruhid 유효. */
			uint8_t reserved	: 5;
			/* [한국어] [7:3] reserved. */
		} bits;
	} fdpef;

	/* Placement identifier */
	uint16_t pid;
	/* [한국어] [bytes 2-3] PID — 이벤트와 연관된 placement ID. */
	/* Event timestamp */
	uint64_t timestamp;
	/* [한국어] [bytes 4-11] timestamp — FID=0x0E Timestamp 단위(ms). */
	/* Namespace identifier */
	uint32_t nsid;
	/* [한국어] [bytes 12-15] NSID — 이벤트 발생 NS. */
	/* Event type specific */
	uint64_t event_type_specific[2];
	/* [한국어] [bytes 16-31] event_type_specific — etype별 16B specific 데이터. Media
	 * Reallocated인 경우 fdp_event_media_reallocated 구조체. */
	/* Reclaim group identifier */
	uint16_t rgid;
	/* [한국어] [bytes 32-33] RGID — Reclaim Group ID. */
	/* Reclaim unit handle identifier */
	uint16_t ruhid;
	/* [한국어] [bytes 34-35] RUHID. */
	uint8_t reserved[4];
	/* [한국어] [bytes 36-39] reserved. */
	uint8_t vs[24];
	/* [한국어] [bytes 40-63] VS — vendor specific. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fdp_event) == 64, "Incorrect size");
#pragma pack(pop)

/* FDP events log page (\ref SPDK_NVME_LOG_FDP_EVENTS) */
/* [한국어] === struct spdk_nvme_fdp_events_log_page === LID=0x23 Get Log Page 응답. */
struct spdk_nvme_fdp_events_log_page {
	/* Number of FDP events */
	uint32_t nevents;
	/* [한국어] [bytes 0-3] NEVENTS — event[] 배열의 entry 수. */
	uint8_t reserved[60];
	/* [한국어] [bytes 4-63] reserved. */
	struct spdk_nvme_fdp_event event[];
	/* [한국어] flexible array — NEVENTS 개의 fdp_event(64B씩). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fdp_events_log_page) == 64, "Incorrect size");

/**
 * Namespace attachment Type Encoding
 */
enum spdk_nvme_ns_attach_type {
	/* Controller attach */
	SPDK_NVME_NS_CTRLR_ATTACH	= 0x0,

	/* Controller detach */
	SPDK_NVME_NS_CTRLR_DETACH	= 0x1,

	/* 0x2-0xF - Reserved */
};

/**
 * Namespace management Type Encoding
 */
enum spdk_nvme_ns_management_type {
	/* Create */
	SPDK_NVME_NS_MANAGEMENT_CREATE	= 0x0,

	/* Delete */
	SPDK_NVME_NS_MANAGEMENT_DELETE	= 0x1,

	/* 0x2-0xF - Reserved */
};

struct spdk_nvme_ns_list {
	uint32_t ns_list[1024];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ns_list) == 4096, "Incorrect size");

/**
 * Namespace identification descriptor type
 *
 * \sa spdk_nvme_ns_id_desc
 */
enum spdk_nvme_nidt {
	/** IEEE Extended Unique Identifier */
	SPDK_NVME_NIDT_EUI64		= 0x01,

	/** Namespace GUID */
	SPDK_NVME_NIDT_NGUID		= 0x02,

	/** Namespace UUID */
	SPDK_NVME_NIDT_UUID		= 0x03,

	/** Namespace Command Set Identifier */
	SPDK_NVME_NIDT_CSI		= 0x04,
};

struct spdk_nvme_ns_id_desc {
	/** Namespace identifier type */
	uint8_t nidt;

	/** Namespace identifier length (length of nid field) */
	uint8_t nidl;

	uint8_t reserved2;
	uint8_t reserved3;

	/** Namespace identifier */
	uint8_t nid[];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ns_id_desc) == 4, "Incorrect size");

struct spdk_nvme_ctrlr_list {
	uint16_t ctrlr_count;
	uint16_t ctrlr_list[2047];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ctrlr_list) == 4096, "Incorrect size");

/* [한국어] === Command Set Identifier (CSI) ===
 * NVMe Base 2.0 §3.1.5. CC.CSS=0x6에서 namespace당 1개. NS Identification descriptor의
 * nidt=4(CSI)로 노출. */
enum spdk_nvme_csi {
	SPDK_NVME_CSI_NVM	= 0x0,
	/* [한국어] 0x0 — NVM Command Set (read/write/flush 등 표준). */
	SPDK_NVME_CSI_KV	= 0x1,
	/* [한국어] 0x1 — Key-Value Command Set (NVMe 2.0). */
	SPDK_NVME_CSI_ZNS	= 0x2,
	/* [한국어] 0x2 — Zoned Namespace Command Set. nvme_zns.h 사용자 API 활성. */
};

/* [한국어] === Format NVM (0x80) - SES (Secure Erase Setting) ===
 * struct spdk_nvme_format.ses 필드. NVMe Base 2.0 §5.14. */
enum spdk_nvme_secure_erase_setting {
	SPDK_NVME_FMT_NVM_SES_NO_SECURE_ERASE	= 0x0,
	/* [한국어] 0x0 — secure erase 없이 단순 포맷(메타만 변경). */
	SPDK_NVME_FMT_NVM_SES_USER_DATA_ERASE	= 0x1,
	/* [한국어] 0x1 — User Data Erase. 모든 LBA를 복구 불가능한 패턴으로 덮어씀. */
	SPDK_NVME_FMT_NVM_SES_CRYPTO_ERASE	= 0x2,
	/* [한국어] 0x2 — Cryptographic Erase. 미디어 암호화 키 폐기 — 즉시 모든 데이터 복구
	 * 불가. 가장 빠르지만 SED 컨트롤러만 지원. */
};

/* [한국어] === PI(Protection Information) location in metadata === */
enum spdk_nvme_pi_location {
	SPDK_NVME_FMT_NVM_PROTECTION_AT_TAIL	= 0x0,
	/* [한국어] 0x0 — 메타데이터의 뒤(tail)에 PI 8B. NVMe 표준 위치. */
	SPDK_NVME_FMT_NVM_PROTECTION_AT_HEAD	= 0x1,
	/* [한국어] 0x1 — 메타데이터의 앞(head)에 PI 8B. SCSI T10 DIF 호환. */
};

/* [한국어] === PI Type === T10 DIF/DIX type. */
enum spdk_nvme_pi_type {
	SPDK_NVME_FMT_NVM_PROTECTION_DISABLE		= 0x0,
	/* [한국어] 0x0 — PI 비활성. 메타만 사용. */
	SPDK_NVME_FMT_NVM_PROTECTION_TYPE1		= 0x1,
	/* [한국어] 0x1 — PI Type 1. RefTag 매 블록마다 +1. AppTag 0xFFFF는 escape. */
	SPDK_NVME_FMT_NVM_PROTECTION_TYPE2		= 0x2,
	/* [한국어] 0x2 — PI Type 2. RefTag 호스트 자유 설정. */
	SPDK_NVME_FMT_NVM_PROTECTION_TYPE3		= 0x3,
	/* [한국어] 0x3 — PI Type 3. RefTag/AppTag 모두 호스트 자유 설정. */
};

/* [한국어] === Metadata transfer === */
enum spdk_nvme_metadata_setting {
	SPDK_NVME_FMT_NVM_METADATA_TRANSFER_AS_BUFFER	= 0x0,
	/* [한국어] 0x0 — 메타를 별도 버퍼로 전송(MPTR 사용). DIX 모드. */
	SPDK_NVME_FMT_NVM_METADATA_TRANSFER_AS_LBA	= 0x1,
	/* [한국어] 0x1 — 메타를 데이터 LBA에 인터리브 전송(extended LBA). DIF 모드. */
};

/* Format - Command Dword 10 */
/* [한국어] === Format NVM (0x80) CDW10 인코딩 ===
 * NVMe Base 2.0 §5.14. 모든 LBA 데이터가 파괴되므로 신중. NS Management로 NS를
 * detach 후 발행하는 것이 안전. */
struct spdk_nvme_format {
	/* LBA format lower (LSB 4 bits of format index), also called lbafl in 2.0 spec */
	uint32_t	lbaf		: 4;
	/* [한국어] LBAF (lower 4 bits) — Identify NS의 lbaf[] 배열 인덱스. 어떤 LBA size +
	 * MS 조합으로 포맷할지. lbafu와 합쳐 6비트 인덱스. */
	/* Metadata settings, also called mset in 2.0 spec */
	uint32_t	ms		: 1;
	/* [한국어] MS — 메타데이터 전송 모드 (0=별도 버퍼, 1=extended LBA). */
	/* Protection information */
	uint32_t	pi		: 3;
	/* [한국어] PI — enum spdk_nvme_pi_type (0=disable, 1/2/3=Type). */
	/* Protection information location */
	uint32_t	pil		: 1;
	/* [한국어] PIL — 메타 안 PI 위치 (0=tail, 1=head). */
	/* Secure erase settings */
	uint32_t	ses		: 3;
	/* [한국어] SES — enum spdk_nvme_secure_erase_setting. */
	/* LBA format upper (MSB 2 bits of format index) */
	uint32_t	lbafu		: 2;
	/* [한국어] LBAFU (upper 2 bits) — NVMe 2.0에서 lbaf 인덱스가 4→6비트로 확장. */
	uint32_t	reserved	: 18;
	/* [한국어] 예약(0). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_format) == 4, "Incorrect size");

/* [한국어] === T10 DIF Protection Information (8B) === Read/Write 시 메타데이터 영역에
 * 포함되어 컨트롤러가 검증. NVMe NVM CS §5.2. */
struct spdk_nvme_protection_info {
	uint16_t	guard;
	/* [한국어] Guard — LBA 데이터의 16비트 CRC (CRC-16 T10). Write 시 호스트가 계산해
	 * 채우거나 PRACT=1로 컨트롤러에게 위임. */
	uint16_t	app_tag;
	/* [한국어] Application Tag — 호스트 정의 16비트 태그. 보통 application/file 식별. */
	uint32_t	ref_tag;
	/* [한국어] Reference Tag — 32비트, Type 1에서는 LBA 하위 32비트와 일치 강제,
	 * Type 3에서는 호스트 자유. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_protection_info) == 8, "Incorrect size");

/* Data structures for sanitize command */
/* Sanitize - Command Dword 10 */
/* [한국어] === Sanitize (0x84) CDW10 인코딩 ===
 * NVMe Base 2.0 §5.24. 모든 LBA + 컨트롤러 캐시를 안전하게 폐기. 진행 중에는 일반 I/O
 * 차단(SC=Sanitize In Progress 0x1d). 진행률은 LID=0x81로 폴링. */
struct spdk_nvme_sanitize {
	/* Sanitize Action (SANACT) */
	uint32_t sanact	: 3;
	/* [한국어] SANACT — enum spdk_sanitize_action. */
	/* Allow Unrestricted Sanitize Exit (AUSE) */
	uint32_t ause	: 1;
	/* [한국어] AUSE — 1이면 sanitize 중단 시에도 컨트롤러 사용 가능(보안 약화). */
	/* Overwrite Pass Count (OWPASS) */
	uint32_t owpass	: 4;
	/* [한국어] OWPASS — Overwrite 액션의 pass 수(0=16, 1~15=실제). */
	/* Overwrite Invert Pattern Between Passes */
	uint32_t oipbp	: 1;
	/* [한국어] OIPBP — 1이면 각 pass마다 패턴을 반전. */
	/* No Deallocate after sanitize (NDAS) */
	uint32_t ndas	: 1;
	/* [한국어] NDAS — 1이면 sanitize 후 LBA dealloc 안 함(블록 그대로 유지). */
	/* reserved */
	uint32_t reserved	: 22;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_sanitize) == 4, "Incorrect size");

/* Sanitize Action */
enum spdk_sanitize_action {
	/* Exit Failure Mode */
	SPDK_NVME_SANITIZE_EXIT_FAILURE_MODE	= 0x1,
	/* [한국어] 0x1 — sanitize 실패 후 failure mode 탈출. */
	/* Start a Block Erase sanitize operation */
	SPDK_NVME_SANITIZE_BLOCK_ERASE		= 0x2,
	/* [한국어] 0x2 — Block Erase. 모든 NAND 블록 erase. */
	/* Start an Overwrite sanitize operation */
	SPDK_NVME_SANITIZE_OVERWRITE		= 0x3,
	/* [한국어] 0x3 — Overwrite. 호스트가 지정 패턴(CDW11)을 N-pass로 덮어씀. */
	/* Start a Crypto Erase sanitize operation */
	SPDK_NVME_SANITIZE_CRYPTO_ERASE		= 0x4,
	/* [한국어] 0x4 — Crypto Erase. 암호화 키 폐기 — 즉시 모든 데이터 복구 불가. */
};

/** Parameters for SPDK_NVME_OPC_FIRMWARE_COMMIT cdw10: commit action */
enum spdk_nvme_fw_commit_action {
	/**
	 * Downloaded image replaces the image specified by
	 * the Firmware Slot field. This image is not activated.
	 */
	SPDK_NVME_FW_COMMIT_REPLACE_IMG			= 0x0,
	/**
	 * Downloaded image replaces the image specified by
	 * the Firmware Slot field. This image is activated at the next reset.
	 */
	SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG	= 0x1,
	/**
	 * The image specified by the Firmware Slot field is
	 * activated at the next reset.
	 */
	SPDK_NVME_FW_COMMIT_ENABLE_IMG			= 0x2,
	/**
	 * The image specified by the Firmware Slot field is
	 * requested to be activated immediately without reset.
	 */
	SPDK_NVME_FW_COMMIT_RUN_IMG			= 0x3,
	/**
	 * Downloaded image replaces the Boot Partition specified by
	 * the Boot Partition ID field.
	 */
	SPDK_NVME_FW_COMMIT_REPLACE_BOOT_PARTITION	= 0x6,
	/**
	 * Mark the Boot Partition specified in the BPID field as Active
	 * and update BPINFO.ABPID.
	 */
	SPDK_NVME_FW_COMMIT_ACTIVATE_BOOT_PARTITION	= 0x7,
};

/** Parameters for SPDK_NVME_OPC_FIRMWARE_COMMIT cdw10 */
struct spdk_nvme_fw_commit {
	/**
	 * Firmware Slot. Specifies the firmware slot that shall be used for the
	 * Commit Action. The controller shall choose the firmware slot (slot 1 - 7)
	 * to use for the operation if the value specified is 0h.
	 */
	uint32_t	fs		: 3;
	/**
	 * Commit Action. Specifies the action that is taken on the image downloaded
	 * with the Firmware Image Download command or on a previously downloaded and
	 * placed image.
	 */
	uint32_t	ca		: 3;
	uint32_t	reserved	: 25;
	/**
	 * Boot Partition ID. Specifies the boot partition that shall be used for the
	 * Commit Action.
	 */
	uint32_t	bpid		: 1;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_fw_commit) == 4, "Incorrect size");

/* ZNS Zone Send Action (ZSA) cdw13 */
enum spdk_nvme_zns_zone_send_action {
	SPDK_NVME_ZONE_CLOSE	= 0x1,
	SPDK_NVME_ZONE_FINISH	= 0x2,
	SPDK_NVME_ZONE_OPEN	= 0x3,
	SPDK_NVME_ZONE_RESET	= 0x4,
	SPDK_NVME_ZONE_OFFLINE	= 0x5,
	SPDK_NVME_ZONE_SET_ZDE	= 0x10,
};

/* ZNS Zone Receive Action (ZRA) cdw13 */
enum spdk_nvme_zns_zone_receive_action {
	SPDK_NVME_ZONE_REPORT		= 0x0,
	SPDK_NVME_ZONE_EXTENDED_REPORT	= 0x1,
};

enum spdk_nvme_zns_zra_report_opts {
	SPDK_NVME_ZRA_LIST_ALL	= 0x0,
	SPDK_NVME_ZRA_LIST_ZSE	= 0x1,
	SPDK_NVME_ZRA_LIST_ZSIO	= 0x2,
	SPDK_NVME_ZRA_LIST_ZSEO	= 0x3,
	SPDK_NVME_ZRA_LIST_ZSC	= 0x4,
	SPDK_NVME_ZRA_LIST_ZSF	= 0x5,
	SPDK_NVME_ZRA_LIST_ZSRO	= 0x6,
	SPDK_NVME_ZRA_LIST_ZSO	= 0x7,
};

/* [한국어] === ZNS Zone Type === ZNS Spec §3.4.2.1. */
enum spdk_nvme_zns_zone_type {
	SPDK_NVME_ZONE_TYPE_SEQWR = 0x2,
	/* [한국어] 0x2 — Sequential Write Required. ZNS 스펙 1.x에서 정의된 유일한 zone type.
	 * write는 반드시 write pointer 위치에 sequential. random은 reset 후만 가능. */
};

/* [한국어] === ZNS Zone State Machine === ZNS Spec §3.4.2.4.
 * 전이: EMPTY → (write) → IO/EO → (Close) → CLOSED → (Open) → IO/EO →
 *       (Finish) → FULL → (Reset) → EMPTY. RO/OFFLINE은 미디어 손상 시 진입.
 * 호스트는 Zone Management Send/Receive로 명시적 전이 또는 write로 자동 전이를 트리거. */
enum spdk_nvme_zns_zone_state {
	SPDK_NVME_ZONE_STATE_EMPTY	= 0x1,
	/* [한국어] 0x1 — EMPTY. WP=ZSLBA, write 가능. reset 직후 또는 신규 zone. */
	SPDK_NVME_ZONE_STATE_IOPEN	= 0x2,
	/* [한국어] 0x2 — Implicitly Opened. write 한 번 발생 후 자동 진입. MOR 한도 영향. */
	SPDK_NVME_ZONE_STATE_EOPEN	= 0x3,
	/* [한국어] 0x3 — Explicitly Opened. Open 명령으로 진입. host 의도 표현. MOR/MAR 한도. */
	SPDK_NVME_ZONE_STATE_CLOSED	= 0x4,
	/* [한국어] 0x4 — CLOSED. Close 명령 또는 컨트롤러 자동 진입. WP는 보존, 자원 해제. */
	SPDK_NVME_ZONE_STATE_RONLY	= 0xD,
	/* [한국어] 0xD — Read Only. write 차단(미디어 손상 등 영구 상태). */
	SPDK_NVME_ZONE_STATE_FULL	= 0xE,
	/* [한국어] 0xE — FULL. WP=ZSLBA+ZCAP. write 불가. read만 가능. Reset으로 EMPTY 복귀. */
	SPDK_NVME_ZONE_STATE_OFFLINE	= 0xF,
	/* [한국어] 0xF — OFFLINE. 미디어 결함으로 read 불가. ZCAP=0. */
};

/* [한국어] === ZNS Zone Descriptor (64B) === Zone Management Receive 응답 페이로드의
 * 핵심. 각 zone마다 1개. nvme_zns.h의 report_zones / ext_report_zones API가 이 배열을
 * 호스트 메모리로 회수해 사용자에게 노출한다. */
struct spdk_nvme_zns_zone_desc {
	/** Zone Type */
	uint8_t zt		: 4;
	/* [한국어] zt — enum spdk_nvme_zns_zone_type (현재 SEQWR=0x2만 정의). */

	uint8_t rsvd0		: 4;
	/* [한국어] 예약(0). */

	uint8_t rsvd1		: 4;
	/* [한국어] 예약(0). */

	/** Zone State */
	uint8_t zs		: 4;
	/* [한국어] zs — enum spdk_nvme_zns_zone_state. EMPTY/IOPEN/EOPEN/CLOSED/RONLY/FULL/
	 * OFFLINE 중 하나. */

	/**
	 * Zone Attributes
	 */
	union {
		uint8_t raw;

		struct {
			/** Zone Finished by controller */
			uint8_t zfc: 1;
			/* [한국어] ZFC — 1이면 컨트롤러가 자체적으로 zone을 FULL로 전이. */

			/** Finish Zone Recommended */
			uint8_t fzr: 1;
			/* [한국어] FZR — 컨트롤러가 호스트에 finish 권고(GC 효율 향상 등). */

			/** Reset Zone Recommended */
			uint8_t rzr: 1;
			/* [한국어] RZR — 컨트롤러가 호스트에 reset 권고. */

			uint8_t rsvd3 : 4;

			/** Zone Descriptor Extension Valid */
			uint8_t zdev: 1;
			/* [한국어] ZDEV — 1이면 zone descriptor extension(zone-local 메타) 유효.
			 * Set Zone Desc Extension 명령으로 호스트가 부착. ext_report_zones로 회수. */
		} bits;
	} za;

	uint8_t reserved[5];

	/** Zone Capacity (in number of LBAs) */
	uint64_t zcap;
	/* [한국어] zcap — 이 zone이 실제로 쓸 수 있는 LBA 수. zone_size보다 작을 수 있음
	 * (NAND 페이지 정렬 등으로 인한 패딩). */

	/** Zone Start LBA */
	uint64_t zslba;
	/* [한국어] zslba — zone의 첫 LBA. zone_size로 정렬됨. */

	/** Write Pointer (LBA) */
	uint64_t wp;
	/* [한국어] wp — 다음 write가 일어날 LBA. EMPTY=zslba, FULL=zslba+zcap. */

	uint8_t reserved32[32];
	/* [한국어] 예약(0). 후속 ZNS 스펙 확장용. ext report 시 이 영역 뒤에 zone descriptor
	 * extension 가변 길이가 따라옴(ext_size는 ZNS Identify Namespace의 zdes 값). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_zns_zone_desc) == 64, "Incorrect size");

/* [한국어] === ZNS Zone Management Receive 응답 헤더 (64B) === 후행 descs[]는 가변 길이
 * zone descriptor 배열. nr_zones는 partial_report=0면 namespace 전체, =1면 본 응답 페이로드
 * 안의 개수. */
struct spdk_nvme_zns_zone_report {
	uint64_t nr_zones;
	/* [한국어] nr_zones — partial_report=0이면 namespace 전체 zone 수, =1이면 본 페이로드의
	 * descs[] 개수. nvme_zns.h::spdk_nvme_zns_report_zones의 partial_report 인자가 결정. */
	uint8_t reserved8[56];
	struct spdk_nvme_zns_zone_desc descs[];
	/* [한국어] descs[] — 가변 길이 zone descriptor 배열. ext report인 경우 각 desc 뒤에
	 * extension 데이터가 패딩. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_zns_zone_report) == 64, "Incorrect size");

/* Directives field */
/* [한국어] === Directive Type === NVMe Base 1.3+ §8.7.
 * Directive Send/Receive(0x19/0x1a) 명령의 CDW11[DTYPE]과 write 명령 CDW12[DTYPE]에 사용.
 * write에 dtype을 부착하면 컨트롤러가 hint 또는 NS 내 classification을 적용. */
enum spdk_nvme_directive_type {
	SPDK_NVME_DIRECTIVE_TYPE_IDENTIFY = 0x0,
	/* [한국어] 0x0 — Identify directive. 지원되는 directive 종류 + enabled 상태 조회. */
	SPDK_NVME_DIRECTIVE_TYPE_STREAMS = 0x1,
	/* [한국어] 0x1 — Streams directive. write에 stream id(dspec)를 부착해 SSD가 같은
	 * stream의 데이터를 NAND block 단위로 그룹화 → GC 효율 향상. (NVMe 1.3) */
	SPDK_NVME_DIRECTIVE_TYPE_DATA_PLACEMENT = 0x2,
	/* [한국어] 0x2 — Data Placement directive. FDP(TP4146)의 Reclaim Unit Handle(RUH) 지정.
	 * write가 어느 RUH에 속할지 결정해 NAND wear/locality 제어. */
};

enum spdk_nvme_identify_directive_send_operation {
	SPDK_NVME_IDENTIFY_DIRECTIVE_SEND_ENABLED = 0x1,
};

enum spdk_nvme_identify_directive_receive_operation {
	SPDK_NVME_IDENTIFY_DIRECTIVE_RECEIVE_RETURN_PARAM = 0x1,
};

struct spdk_nvme_ns_identify_directive_param {
	struct {
		/* set to 1b to indicate that the Identify Directive is supported */
		uint8_t identify	: 1;
		/* set to 1b if the Streams Directive is supported */
		uint8_t streams		: 1;
		/* set to 1b if the Data Placement Directive is supported */
		uint8_t data_pd		: 1;
		uint8_t reserved1	: 5;
		uint8_t reserved2[31];
	} directives_supported;
	struct {
		/* set to 1b to indicate that the Identify Directive is enabled */
		uint8_t identify	: 1;
		/* set to 1b if the Streams Directive is enabled */
		uint8_t streams		: 1;
		/* set to 1b if the Data Placement Directive is enabled */
		uint8_t data_pd		: 1;
		uint8_t reserved1	: 5;
		uint8_t reserved2[31];
	} directives_enabled;
	struct {
		/**
		 * cleared to 0b as the host is not able to change the state of
		 * Identify Directive
		 */
		uint8_t identify	: 1;
		/**
		 * cleared to 0b to indicate that the Streams Directive state
		 * is not preserved across ctrl reset
		 */
		uint8_t streams		: 1;
		/**
		 * set to 1b if the Data Placement Directive is supported to
		 * indicate that the host specified Data Placement Directive
		 * state is preserved across ctrl reset
		 */
		uint8_t data_pd		: 1;
		uint8_t reserved1	: 5;
		uint8_t reserved2[31];
	} directives_persistence;

	uint32_t reserved[1000];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ns_identify_directive_param) == 4096, "Incorrect size");

/* [한국어] === enum spdk_nvme_streams_directive_receive_operation === Directive Receive(0x1a)
 * 의 DOPER 필드. NVMe 1.3 Streams. CDW10[DOPER]+CDW11[DTYPE=2]. */
enum spdk_nvme_streams_directive_receive_operation {
	SPDK_NVME_STREAMS_DIRECTIVE_RECEIVE_RETURN_PARAM = 0x1,
	/* [한국어] 0x1 — Return Parameters. struct spdk_nvme_ns_streams_data(32B) 회수. NS의 stream
	 * 한도, 사용 가능 수, write size 등. */
	SPDK_NVME_STREAMS_DIRECTIVE_RECEIVE_GET_STATUS = 0x2,
	/* [한국어] 0x2 — Get Status. struct spdk_nvme_ns_streams_status(131072B) 회수. 현재 open
	 * 된 stream ID 목록. */
	SPDK_NVME_STREAMS_DIRECTIVE_RECEIVE_ALLOCATE_RESOURCE = 0x3,
	/* [한국어] 0x3 — Allocate Resource. CDW12[NSR]만큼 stream resource 예약. */
};

/* [한국어] === enum spdk_nvme_streams_directive_send_operation === Directive Send(0x19) DOPER. */
enum spdk_nvme_streams_directive_send_operation {
	SPDK_NVME_STREAMS_DIRECTIVE_SEND_RELEASE_ID = 0x1,
	/* [한국어] 0x1 — Release Stream ID. CDW12[stream_id]에 해당하는 ID 해제. */
	SPDK_NVME_STREAMS_DIRECTIVE_SEND_RELEASE_RESOURCE = 0x2,
	/* [한국어] 0x2 — Release Resource. NS의 모든 stream resource 해제. */
};

/* [한국어] === struct spdk_nvme_ns_streams_data === Streams Directive Receive Return Parameters
 * (DOPER=0x1) 응답 32B. NS-별 streams capability. SPDK는 io_flags=STREAMS_DIRECTIVE 사용 전에
 * 이 데이터로 가용 stream 수 확인. */
struct spdk_nvme_ns_streams_data {
	/* MAX Streams Limit */
	uint16_t msl;
	/* [한국어] [bytes 0-1] MSL — 컨트롤러 전체 stream 최대 수. */
	/* NVM Subsystem Streams Available */
	uint16_t nssa;
	/* [한국어] [bytes 2-3] NSSA — 사용 가능한 subsystem-wide stream 수. */
	/* NVM Subsystem Streams Open */
	uint16_t nsso;
	/* [한국어] [bytes 4-5] NSSO — 현재 open된 subsystem stream 수. */
	/* NVM Subsystem Stream Capability */
	struct {
		/* [한국어] NSSC — Stream Capability (1B). */
		/* Stream ID may be shared by multiple host IDs if set to 1. */
		uint8_t ssid		: 1;
		/* [한국어] [0] ssid — 1=stream ID가 여러 host_id 간 공유 가능. */
		uint8_t reserved	: 7;
		/* [한국어] [7:1] reserved. */
	} nssc;
	uint8_t reserved1[9];
	/* [한국어] [bytes 7-15] reserved. */
	/* Namespace Specific Fields
	 * Stream Write Size */
	uint32_t sws;
	/* [한국어] [bytes 16-19] SWS — Stream Write Size (LBA). 한 stream의 권장 write 단위. */
	/* Stream Granularity Size */
	uint16_t sgs;
	/* [한국어] [bytes 20-21] SGS — Stream Granularity (SWS 배수). erase block 정렬 등. */
	/* Namespace and Host Identifier Specific Fields
	 * Namespace Streams Allocated */
	uint16_t nsa;
	/* [한국어] [bytes 22-23] NSA — 이 NS+host에 할당된 stream 수. */
	/* Namespace Streams Open */
	uint16_t nso;
	/* [한국어] [bytes 24-25] NSO — 현재 open된 NS+host stream 수. */
	uint8_t reserved2[6];
	/* [한국어] [bytes 26-31] reserved. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ns_streams_data) == 32, "Incorrect size");

/* [한국어] === struct spdk_nvme_ns_streams_status === Streams Get Status(DOPER=0x2) 응답.
 * 131072B (=128 KiB). 현재 NS에 open된 stream의 ID 목록 회수. */
struct spdk_nvme_ns_streams_status {
	/* Open Stream Count, this field specifies the number of streams that are currently open */
	uint16_t open_streams_count;
	/* [한국어] [bytes 0-1] open_streams_count — 유효 stream_id[] 엔트리 수. */

	/* Stream Identifier, this field specifies the open stream identifier */
	uint16_t stream_id[65535];
	/* [한국어] [bytes 2-131071] stream_id[N] — 각 open stream의 ID(uint16, 1-base). 호스트는
	 * write 명령의 cdw13.dspec에 이 ID를 넣어 같은 stream으로 routing. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ns_streams_status) == 131072, "Incorrect size");

enum spdk_nvme_ctrlr_type {
	/* 0x00 - reserved */

	/* I/O Controller */
	SPDK_NVME_CTRLR_IO		= 0x1,

	/* Discovery Controller */
	SPDK_NVME_CTRLR_DISCOVERY	= 0x2,

	/* Administrative Controller */
	SPDK_NVME_CTRLR_ADMINISTRATIVE	= 0x3,

	/* 0x4-0xFF - Reserved */
};

#define spdk_nvme_cpl_is_error(cpl)			\
	((cpl)->status.sc != SPDK_NVME_SC_SUCCESS ||	\
	 (cpl)->status.sct != SPDK_NVME_SCT_GENERIC)

#define spdk_nvme_cpl_is_success(cpl)	(!spdk_nvme_cpl_is_error(cpl))

#define spdk_nvme_cpl_is_pi_error(cpl)						\
	((cpl)->status.sct == SPDK_NVME_SCT_MEDIA_ERROR &&			\
	 ((cpl)->status.sc == SPDK_NVME_SC_GUARD_CHECK_ERROR ||			\
	  (cpl)->status.sc == SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR ||	\
	  (cpl)->status.sc == SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR))

#define spdk_nvme_cpl_is_abort_success(cpl)	\
	(spdk_nvme_cpl_is_success(cpl) && !((cpl)->cdw0 & 1U))

#define spdk_nvme_cpl_is_path_error(cpl)	\
	((cpl)->status.sct == SPDK_NVME_SCT_PATH)

#define spdk_nvme_cpl_is_ana_error(cpl)						\
	((cpl)->status.sct == SPDK_NVME_SCT_PATH &&				\
	 ((cpl)->status.sc == SPDK_NVME_SC_ASYMMETRIC_ACCESS_PERSISTENT_LOSS ||	\
	  (cpl)->status.sc == SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE ||	\
	  (cpl)->status.sc == SPDK_NVME_SC_ASYMMETRIC_ACCESS_TRANSITION))

#define spdk_nvme_cpl_is_aborted_sq_deletion(cpl)		\
	((cpl)->status.sct == SPDK_NVME_SCT_GENERIC &&		\
	 (cpl)->status.sc == SPDK_NVME_SC_ABORTED_SQ_DELETION)

#define spdk_nvme_cpl_is_aborted_by_request(cpl)            \
	((cpl)->status.sct == SPDK_NVME_SCT_GENERIC &&          \
	 (cpl)->status.sc == SPDK_NVME_SC_ABORTED_BY_REQUEST)

/* [한국어] === SPDK_NVME_IO_FLAGS — read/write/append API의 io_flags 인자 비트마스크 ===
 * spdk_nvme_ns_cmd_read/write/append/compare/appendv 등이 모두 io_flags를 받아 SQE의
 * 적절한 위치(CDW0[fuse] / CDW12[PRACT/PRCHK*/FUA/LR/DTYPE])로 분배. 사용자는 본 헤더의
 * 매크로를 OR로 조합해 한 번에 전달. 비트 배치는 의도적으로 CDW12 상위 16비트와 1:1
 * 매핑하여 변환 비용을 0으로 한다(SPDK_NVME_IO_FLAGS_CDW12_MASK). */

/** Set fused operation */
#define SPDK_NVME_IO_FLAGS_FUSE_FIRST (SPDK_NVME_CMD_FUSE_FIRST << 0)
/* [한국어] FUSE_FIRST — 두 SQE를 fused pair로 묶어 atomic 실행. Compare-and-Write 시
 * COMPARE에 FUSE_FIRST, WRITE에 FUSE_SECOND. CDW0의 fuse 필드(bit 8-9)에 들어감. */
#define SPDK_NVME_IO_FLAGS_FUSE_SECOND (SPDK_NVME_CMD_FUSE_SECOND << 0)
/* [한국어] FUSE_SECOND — fused pair의 두 번째 명령. */
#define SPDK_NVME_IO_FLAGS_FUSE_MASK (SPDK_NVME_CMD_FUSE_MASK << 0)
/* [한국어] FUSE_MASK — fuse 비트 추출 마스크. */

/* Bits 20-31 of SPDK_NVME_IO_FLAGS map directly to their associated bits in
 * cdw12 for NVMe IO commands
 */
/** For enabling directive types on write-oriented commands */
#define SPDK_NVME_IO_FLAGS_DIRECTIVE(dtype) (dtype << 20)
/* [한국어] DIRECTIVE — write 명령의 dtype 4비트(bit 20-23). Streams/Data Placement 등.
 * dspec(stream id)는 cdw13에 별도. */
#define SPDK_NVME_IO_FLAGS_STREAMS_DIRECTIVE \
	SPDK_NVME_IO_FLAGS_DIRECTIVE(SPDK_NVME_DIRECTIVE_TYPE_STREAMS)
/* [한국어] STREAMS_DIRECTIVE — Streams directive 활성화. NVMe 1.3 Streams. */
#define SPDK_NVME_IO_FLAGS_DATA_PLACEMENT_DIRECTIVE \
	SPDK_NVME_IO_FLAGS_DIRECTIVE(SPDK_NVME_DIRECTIVE_TYPE_DATA_PLACEMENT)
/* [한국어] DATA_PLACEMENT_DIRECTIVE — FDP(TP4146) Data Placement directive. RUH 지정. */
/** Zone append specific, determines the contents of the reference tag written to the media */
#define SPDK_NVME_IO_FLAGS_ZONE_APPEND_PIREMAP (1U << 25)
/* [한국어] PIREMAP (bit25) — Zone Append 시 컨트롤러가 ref_tag을 actual LBA로 자동 재기입.
 * Type1 PI에서 호스트가 미리 ref_tag을 모를 때(append이므로) 필수. */
/** Enable protection information checking of the Logical Block Reference Tag field */
#define SPDK_NVME_IO_FLAGS_PRCHK_REFTAG (1U << 26)
/* [한국어] PRCHK_REFTAG (bit26) — RefTag 검증 활성화. CDW12 bit 26. */
/** Enable protection information checking of the Application Tag field */
#define SPDK_NVME_IO_FLAGS_PRCHK_APPTAG (1U << 27)
/* [한국어] PRCHK_APPTAG (bit27) — AppTag 검증 활성화. */
/** Enable protection information checking of the Guard field */
#define SPDK_NVME_IO_FLAGS_PRCHK_GUARD (1U << 28)
/* [한국어] PRCHK_GUARD (bit28) — Guard(CRC-16) 검증 활성화. */
/** The protection information is stripped or inserted when set this bit */
#define SPDK_NVME_IO_FLAGS_PRACT (1U << 29)
/* [한국어] PRACT (bit29) — Protection Information Action. write 시 컨트롤러가 PI 자동
 * 생성/삽입(호스트 PI 미경유), read 시 자동 검증/제거. SPDK accel framework가 사용. */
#define SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS (1U << 30)
/* [한국어] FUA (bit30) — Force Unit Access. write 완료 전에 미디어까지 도달 보장
 * (write cache 우회). read 시에는 cache가 아닌 media에서 직접 읽기. */
#define SPDK_NVME_IO_FLAGS_LIMITED_RETRY (1U << 31)
/* [한국어] LR (bit31) — Limited Retry. 컨트롤러가 에러 시 재시도 횟수 제한. 빠른 실패
 * 우선 시 사용. */

/** Mask of valid io flags mask */
#define SPDK_NVME_IO_FLAGS_VALID_MASK 0xFFFF0003
/* [한국어] VALID_MASK — 사용자가 지정 가능한 io_flags 비트 = bit0-1(fuse) + bit16-31. */
#define SPDK_NVME_IO_FLAGS_CDW12_MASK 0xFFFF0000
/* [한국어] CDW12_MASK — CDW12 상위 16비트(bit 16-31)와 1:1 매핑되는 영역. SPDK는
 * (io_flags & CDW12_MASK)을 그대로 cdw12에 OR해서 비트 분배 비용 0. */
#define SPDK_NVME_IO_FLAGS_PRCHK_MASK 0x1C000000
/* [한국어] PRCHK_MASK — bit26-28 (REFTAG/APPTAG/GUARD) 검증 비트 묶음. */

/** Identify command buffer response size */
#define SPDK_NVME_IDENTIFY_BUFLEN 4096
/* [한국어] Identify 응답 페이로드 크기 — 모든 CNS 변형이 정확히 4096B(NVMe 스펙 강제). */

#ifdef __cplusplus
}
#endif

#endif
