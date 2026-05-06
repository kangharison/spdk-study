/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * IDXD specification definitions
 */

/*
 * [한국어 설명] Intel IDXD(DSA/IAA) 와이어 포맷 및 레지스터 정의 (idxd_spec.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 Intel IDXD 패밀리(DSA: Data Streaming Accelerator, IAA: In-memory Analytics
 * Accelerator) 의 **하드웨어 와이어 포맷**을 C 자료형으로 정의한다. 정확히 다음 항목을 다룬다:
 * (1) MMIO BAR/portal/offset 매크로, (2) 디스크립터 flags(IDXD_FLAG_*),
 * (3) DIF/DIX/IAA 옵션 비트마스크, (4) DSA·IAA completion status enum,
 * (5) WQ/디바이스 상태·명령·에러 enum, (6) 64B 디스크립터 구조체(idxd_hw_desc),
 * (7) 32B/64B completion record(dsa_hw_comp_record / iaa_hw_comp_record),
 * (8) IAA AECS 상태블록, (9) GENCAP/WQCAP/GROUPCAP/ENGINECAP/OPCAP 등 디바이스
 * capability/configuration 레지스터 union, (10) GRPCFG/WQCFG 구조체.
 * 이 정의들은 Intel "Data Streaming Accelerator Architecture Specification" 의
 * 비트 레이아웃과 1:1 대응되며 디바이스가 직접 읽고 쓰는 메모리 영역의 골격이다.
 * 코드 측은 이 구조체들에 비트 단위로 값을 채워 portal로 발행하기만 하면 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * idxd_spec.h 는 SPDK IDXD 스택의 **최하위 와이어 포맷 계층**이다.
 *   [응용/accel framework]
 *      ↓ spdk/idxd.h API
 *   [lib/idxd: SPDK 유저스페이스 IDXD 드라이버]
 *      ↓ 본 헤더의 idxd_hw_desc 채워서 portal write
 *   [PCIe → IDXD 디바이스]
 *      ↓ 본 헤더의 dsa_hw_comp_record / iaa_hw_comp_record 메모리에 기록
 * 이 헤더는 코드를 거의 포함하지 않으며, 모든 정의가 디바이스 ABI에 직접 묶여 있어
 * 함부로 수정 금지(필드 순서·비트 너비를 바꾸면 디바이스가 데이터를 잘못 읽음).
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h(uint8/16/32/64_t), spdk/assert.h(SPDK_STATIC_ASSERT — 모든 구조체
 *   크기를 컴파일 타임에 검증해 ABI drift 방지).
 * - 본 헤더에 의존: spdk/idxd.h(spdk_idxd_submit_raw_desc 인자로 idxd_hw_desc 노출),
 *   lib/idxd/idxd.c(디스크립터/completion record 직접 빌드/검사),
 *   module/accel/idxd/(opcode 매핑).
 * - 데이터 흐름: lib/idxd가 idxd_hw_desc.{opcode, flags, src_addr, dst_addr, completion_addr,
 *   xfer_size, op_specific 일부}를 채워 portal에 64B atomic write(ENQCMDS/MOVDIR64B) →
 *   디바이스가 디스크립터 처리 후 completion_addr이 가리키는 dsa_hw_comp_record /
 *   iaa_hw_comp_record에 status·result·bytes_completed 기록 → SW가 status volatile read
 *   로 valid 비트 polling.
 * - 공유 자료구조: idxd_hw_desc(64B aligned), dsa_hw_comp_record(32B), iaa_hw_comp_record(64B),
 *   union idxd_*_register(MMIO BAR0의 GENCAP/WQCAP/CMD/CMDSTS 등 매핑).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct idxd_hw_desc: 64B 정렬 + 정확히 64B 크기. opcode(8b), flags(24b), src/dst/xfer_size,
 *   completion_addr, op_specific(24B union). 모든 IDXD opcode가 같은 64B 구조 공유.
 * - struct dsa_hw_comp_record: 32B 완료 레코드. status(0=미완료, 0x01=PASS, 그 외=오류 코드),
 *   bytes_completed, fault_addr, op_specific 16B union(crc32c_val/dif_*_comp 등).
 * - struct iaa_hw_comp_record: 64B IAA 완료 레코드. status, error_code, output_size,
 *   output_bits, invalid_flags.
 * - struct iaa_aecs: 1568B AECS(Accelerator Engine Context Save) — IAA의 압축/해제 상태
 *   블록(하프만 테이블 등).
 * - union idxd_gencap_register / wqcap / groupcap / enginecap / opcap: MMIO BAR0 capability
 *   레지스터들. block_on_fault, max_xfer_shift, num_wqs, shared_mode 등을 비트필드로 노출.
 * - struct idxd_registers: BAR0의 시작부터 0xE0까지의 레지스터 레이아웃.
 * - struct idxd_grpcfg / union idxd_wqcfg: group 및 WQ 구성 테이블 — config 단계에서
 *   드라이버가 채워 디바이스에 enable 명령 전 적용.
 * - enum dsa_completion_status / iaa_completion_status: status 바이트 값 카탈로그.
 */

#ifndef SPDK_IDXD_SPEC_H
#define SPDK_IDXD_SPEC_H
/* [한국어] 헤더 가드 — 다중 include 시 중복 정의 방지. */

#include "spdk/stdinc.h"
/* [한국어] uint8_t/uint16_t/uint32_t/uint64_t 정수 타입과 기본 표준 라이브러리.
 * 본 헤더의 모든 비트필드는 이 정수 타입에 의존. */
#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT 매크로 — 컴파일 타임 sizeof 검증.
 * 모든 디바이스 ABI 구조체 뒤에 SPDK_STATIC_ASSERT(sizeof(...) == 기대값, "...") 가 따라붙어
 * 컴파일러 패딩/필드 정렬 문제로 ABI가 깨지면 빌드 자체를 막는다. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++에서 include 시 C 링크 규약. 본 헤더는 타입 정의만 있으므로 링크 영향은 없으나,
 * spdk/idxd.h 에서 본 헤더를 가져와 prototype에 사용하므로 일관되게 extern "C". */
#endif

#define IDXD_MMIO_BAR			0
/* [한국어] IDXD 디바이스의 MMIO 레지스터 BAR(Base Address Register) 인덱스.
 * PCI BAR0 = capability/control/command 레지스터 영역(struct idxd_registers 참고).
 * 설정자: lib/idxd 초기화 코드가 spdk_pci_device_map_bar(dev, 0, ...)로 매핑.
 * 읽는 자: 디바이스 enable/cmd 발행 시 BAR0 + offset에 MMIO write/read.
 * 동기화: 단일 control thread만 만지므로 별도 동기화 불필요(probe/init 단계 한정). */
#define IDXD_WQ_BAR			2
/* [한국어] WQ portal 영역 BAR 인덱스. PCI BAR2 = WQ portal(디스크립터 발행용 doorbell).
 * 매핑된 영역에 ENQCMDS(SWQ)/MOVDIR64B(DWQ) 명령으로 64B 디스크립터를 atomic 발행.
 * 설정자: probe 시 매핑.
 * 읽는 자: spdk_idxd_submit_*() 핫패스. */
#define PORTAL_SIZE			0x1000
/* [한국어] WQ 1개당 portal 크기 = 4KB. 4KB 페이지 안에서 어느 64B 슬롯에 쓰든 동일 효과
 * (스펙: portal 페이지는 같은 WQ로 라우팅). 4KB는 page-level VFIO 권한 제어용 단위이기도 하다. */
#define WQ_TOTAL_PORTAL_SIZE		(PORTAL_SIZE * 4)
/* [한국어] 단일 WQ가 사용하는 총 portal 영역 = 4 페이지(16KB).
 * 4종류의 portal(unlimited/dedicated/shared 변종 + 권한 변종)을 위해 4페이지 예약.
 * 설정자: 디바이스 스펙. 읽는 자: portal 주소 산출 로직. */
#define PORTAL_STRIDE			0x40
/* [한국어] portal 안에서 사용하는 stride = 64B = 1 cache line = 디스크립터 크기.
 * 드라이버는 portal 페이지 내부에서 64B 단위로 다른 슬롯을 선택할 수 있어, 동일 WQ에 대한
 * 동시 발행이 cache line ping-pong을 줄이는 데 도움이 된다. */
#define PORTAL_MASK			(PORTAL_SIZE - 1)
/* [한국어] portal 페이지 내부 오프셋 마스크(0xFFF). MMIO 발행 주소 산출 시 페이지 정렬을
 * 보장하기 위해 (addr & ~PORTAL_MASK) 등으로 사용. */
#define WQCFG_SHIFT			5
/* [한국어] WQCFG 테이블의 entry stride 지수 — 1<<5 = 32B per WQ.
 * 즉, WQ N의 WQCFG 시작 = wqcfg_table_base + (N << WQCFG_SHIFT). union idxd_wqcfg가 32B임을 반영. */

#define IDXD_TABLE_OFFSET_MULT		0x100
/* [한국어] BAR0의 offsets 레지스터(union idxd_offsets_register)에 저장된 값에 곱할 단위 = 256B.
 * 예: offsets.grpcfg=0x10 이면 실제 GRPCFG 테이블 위치 = 0x10 * 0x100 = 0x1000 (BAR0 base 기준).
 * 디바이스가 작은 비트수로 큰 offset을 표현하기 위한 압축 표현. */

#define IDXD_CLEAR_CRC_FLAGS		0xFFFFu
/* [한국어] CRC 디스크립터 빌드 시 누적 시드 관련 flag를 모두 해제하기 위한 마스크(하위 16비트).
 * SPDK 라이브러리가 IDXD_FLAG_CRC_READ_CRC_SEED 등을 끄고 새 CRC 계산을 시작할 때 사용. */

#define IDXD_FLAG_FENCE			(1 << 0)
/* [한국어] DSA 디스크립터 flags 비트 0: Fence — 이 디스크립터가 완료되기 전까지 같은 WQ의
 * 후속 디스크립터가 결과를 관찰하지 못하도록 직렬화 강제. RAW(Read-After-Write) 의존성이 있는
 * 연속 디스크립터(예: copy → CRC of copied data)를 batch 안에 넣을 때 유용.
 * 사용자: lib/idxd가 batch 디스크립터 빌드 시 setting. 읽는 자: 디바이스 엔진. */
#define IDXD_FLAG_COMPLETION_ADDR_VALID	(1 << 2)
/* [한국어] 비트 2: CRAV(Completion Record Address Valid) — completion_addr 필드가 유효함을 표시.
 * 0이면 디바이스는 completion record를 쓰지 않고 인터럽트만 발행(거의 사용 안 함).
 * SPDK는 항상 polling이므로 이 비트를 세팅한다. */
#define IDXD_FLAG_REQUEST_COMPLETION	(1 << 3)
/* [한국어] 비트 3: RCR(Request Completion Record) — 완료 시 completion record를 항상 쓰도록 요청.
 * SPDK는 polling 모델이므로 모든 디스크립터에 SET. CRAV와 함께 묶여 있으면 디바이스가 완료
 * 통지를 SW가 읽을 메모리에 남긴다. */
#define IDXD_FLAG_CACHE_CONTROL		(1 << 8)
/* [한국어] 비트 8: Cache Control — 1=결과를 CPU 캐시 우회(non-temporal)하여 memory에 직접 기록,
 * 0=캐시에 기록(기본). SPDK 측 매크로 SPDK_IDXD_FLAG_NONTEMPORAL이 이 비트로 매핑됨. */
#define IDXD_FLAG_DEST_READBACK		(1 << 14)
/* [한국어] 비트 14: Destination Readback — 쓰기 후 destination을 다시 읽어 일관성 강화.
 * 일부 메모리 위계(persistent memory 등)에서 ordering을 안전하게 하기 위해 사용. */
#define IDXD_FLAG_DEST_STEERING_TAG	(1 << 15)
/* [한국어] 비트 15: Destination Steering Tag — DDIO/DSA traffic class 라우팅 힌트를 활성화하여
 * 결과 데이터를 특정 LLC slice/PCIe traffic class로 유도. */
#define IDXD_FLAG_CRC_READ_CRC_SEED	(1 << 16)
/* [한국어] 비트 16: CRC Read CRC Seed — CRC32C 디스크립터에서 seed 필드를 사용하지 않고,
 * 메모리에 저장된 이전 CRC 결과를 읽어 시드로 사용. 큰 데이터를 chunk로 나누어 누적 CRC 계산할 때
 * 사용. */

#define IDXD_DSA_STATUS_DIF_ERROR	0x9
/* [한국어] DSA completion status 0x09 = DIF error. dsa_completion_status::DSA_COMP_DIF_ERR와 동일 값.
 * dif_check/strip 디스크립터에서 PI 검증이 실패했음을 의미. completion record의 dif_chk_* 필드로
 * 어느 블록·어떤 태그가 어긋났는지 식별 가능. */

#define IDXD_DIF_FLAG_INVERT_CRC_RESULT		(1 << 3)
/* [한국어] DIF 디스크립터의 추가 flags(8비트 dif_chk.flags 등) 비트 3 — 결과 CRC를 비트 반전(NVMe DIF
 * Type 1/2/3 변종 호환). */
#define IDXD_DIF_FLAG_INVERT_CRC_SEED		(1 << 2)
/* [한국어] 비트 2 — seed CRC를 비트 반전. 일부 NVMe 구현이 seed를 ~0으로 시작하는 것을 흉내. */
#define IDXD_DIF_FLAG_DIF_BLOCK_SIZE_512	0x0
/* [한국어] 블록 크기 코드 0x0 = 512B (PI 8B 별도). 비트필드 단위로 dif 디스크립터의 flags 하위 2비트에 인코딩. */
#define IDXD_DIF_FLAG_DIF_BLOCK_SIZE_520	0x1
/* [한국어] 블록 크기 코드 0x1 = 520B (= 512B data + 8B PI inline form). T10 PI 일부 형식. */
#define IDXD_DIF_FLAG_DIF_BLOCK_SIZE_4096	0x2
/* [한국어] 블록 크기 코드 0x2 = 4096B (PI 8B 별도). 4K 섹터 NVMe SSD 표준. */
#define IDXD_DIF_FLAG_DIF_BLOCK_SIZE_4104	0x3
/* [한국어] 블록 크기 코드 0x3 = 4104B (= 4096B data + 8B PI inline). NVMe extended LBA 4K+8B. */

#define IDXD_DIF_SOURCE_FLAG_SOURCE_REF_TAG_TYPE	(1 << 7)
/* [한국어] dif_chk/dif_strip 디스크립터의 src_flags 비트 7 — Reference Tag 처리 방식 선택
 * (NVMe Type 1=increment per block / Type 3=fixed). 1이면 Type 3(고정), 0이면 Type 1(증가). */
#define IDXD_DIF_SOURCE_FLAG_REF_TAG_CHECK_DISABLE	(1 << 6)
/* [한국어] 비트 6 — RefTag 검사 비활성화(NVMe Type 0 등). */
#define IDXD_DIF_SOURCE_FLAG_GUARD_CHECK_DISABLE	(1 << 5)
/* [한국어] 비트 5 — Guard(CRC) 검사 비활성화. */
#define IDXD_DIF_SOURCE_FLAG_SOURCE_APP_TAG_TYPE	(1 << 4)
/* [한국어] 비트 4 — Application Tag 처리(고정/증가) 선택. */
#define IDXD_DIF_SOURCE_FLAG_APP_AND_REF_TAG_F_DETECT	(1 << 3)
/* [한국어] 비트 3 — App+Ref Tag 가 모두 0xFF...F 패턴일 때 escape(검사 통과)로 처리(NVMe escape). */
#define IDXD_DIF_SOURCE_FLAG_APP_TAG_F_DETECT		(1 << 2)
/* [한국어] 비트 2 — App Tag 가 0xFF...F일 때 escape. */
#define IDXD_DIF_SOURCE_FLAG_ALL_F_DETECT		(1 << 1)
/* [한국어] 비트 1 — PI 전체가 all-F 패턴일 때 escape. */
#define IDXD_DIF_SOURCE_FLAG_ENABLE_ALL_F_DETECT_ERR	(1)
/* [한국어] 비트 0 — all-F escape 발생 시 error로 보고할지 활성화. */

#define IAA_FLAG_RD_SRC2_AECS		(1 << 16)
/* [한국어] IAA 디스크립터 flags 비트 16 — src2를 AECS(Accelerator Engine Context Save) 구조체로 해석.
 * 압축에서 사전(huffman 테이블 등)을 메모리에 둔 stateful 작업에 사용. */
#define IAA_COMP_FLUSH_OUTPUT		(1 << 1)
/* [한국어] IAA 압축 옵션(compr_flags 비트 1) — 출력 stream에 BFINAL 비트와 sync flush 추가.
 * 청크 단위로 압축 결과를 외부 zlib 디코더가 읽어가도록 한 청크 종료. */
#define IAA_COMP_APPEND_EOB		(1 << 2)
/* [한국어] IAA 압축 옵션 비트 2 — 마지막 블록 뒤에 End-Of-Block 코드 추가. */
#define IAA_COMP_FLAGS			(IAA_COMP_FLUSH_OUTPUT | IAA_COMP_APPEND_EOB)
/* [한국어] SPDK 기본 압축 flag 조합 — 청크별 독립 디코딩 가능 + 명시적 EOB. */
#define IAA_DECOMP_ENABLE		(1 << 0)
/* [한국어] IAA 압축 해제 옵션(decompr_flags 비트 0) — 디코딩 활성. SPDK는 항상 SET. */
#define IAA_DECOMP_FLUSH_OUTPUT		(1 << 1)
/* [한국어] 비트 1 — 출력 버퍼 flush. */
#define IAA_DECOMP_CHECK_FOR_EOB	(1 << 2)
/* [한국어] 비트 2 — 입력 stream에 EOB 마커가 있는지 확인. */
#define IAA_DECOMP_STOP_ON_EOB		(1 << 3)
/* [한국어] 비트 3 — EOB 만나면 즉시 종료(다음 데이터 무시). */
#define IAA_DECOMP_FLAGS		(IAA_DECOMP_ENABLE | \
					IAA_DECOMP_FLUSH_OUTPUT | \
					IAA_DECOMP_CHECK_FOR_EOB | \
					IAA_DECOMP_STOP_ON_EOB)
/* [한국어] SPDK 기본 압축해제 flag 조합 — 활성 + flush + EOB 검사 + EOB에서 정지. */

/*
 * IDXD is a family of devices, DSA and IAA.
 */
/* [한국어] 아래 enum은 DSA·IAA가 dsa_hw_comp_record/iaa_hw_comp_record.status에 기록하는 값의 카탈로그.
 * 0 = "아직 미완료"(volatile polling용 sentinel), 0x01 = 정상 완료, 그 외 = 오류 코드.
 * SPDK 라이브러리는 이 값을 보고 −errno 또는 해당 SPDK 에러 코드로 변환해 cb_fn(status)에 전달. */
enum dsa_completion_status {
	DSA_COMP_NONE			= 0,
	/* [한국어] 0 — 미완료(디바이스가 아직 status 바이트를 쓰지 않음).
	 * 설정자: 디바이스 또는 드라이버 reset(SW 가 0으로 초기화).
	 * 읽는 자: spdk_idxd_process_events()의 polling loop — 0이면 "아직 안 됨" 으로 처리.
	 * 동기화: completion_record.status는 volatile uint8_t — 컴파일러 reorder 방지.
	 * 디바이스 쓰기는 1B atomic이며, polling loop는 캐시 라인 invalidate를 받아 본다. */

	DSA_COMP_SUCCESS		= 1,
	/* [한국어] 0x01 — PASS(정상 완료). 모든 정상 상태의 표준값. cb_fn(arg, 0) 호출.
	 * 설정자: 디바이스. 읽는 자: process_events(). */

	DSA_COMP_SUCCESS_PRED		= 2,
	/* [한국어] 0x02 — Predicate success(분기 디스크립터에서 술어 평가 성공). batch나 conditional
	 * 디스크립터 흐름에서 사용. 일반 SPDK 흐름에서는 거의 등장하지 않음. */

	DSA_COMP_PAGE_FAULT_NOBOF	= 3,
	/* [한국어] 0x03 — Block-on-Fault 미설정 상태에서 페이지 폴트. SVM(Shared Virtual Memory) 사용 시
	 * 디바이스가 호스트 페이지 테이블 walk에 실패. 응용은 페이지 wire 후 재시도 필요. */

	DSA_COMP_PAGE_FAULT_IR		= 4,
	/* [한국어] 0x04 — Page fault during IR(Internal Resume). 부분 처리 중 폴트. fault_addr와
	 * bytes_completed로 상태 복원 후 partial retry 가능. */

	DSA_COMP_BATCH_FAIL		= 5,
	/* [한국어] 0x05 — Batch 디스크립터(BATCH opcode)에서 sub-descriptor 일부 실패.
	 * sub-descriptor 별 status는 별도 batch 결과 영역에서 확인. */

	DSA_COMP_BATCH_PAGE_FAULT	= 6,
	/* [한국어] 0x06 — Batch 처리 중 page fault. */

	DSA_COMP_DR_OFFSET_NOINC	= 7,
	/* [한국어] 0x07 — Delta Record offset이 증가하지 않음(잘못된 delta record). */

	DSA_COMP_DR_OFFSET_ERANGE	= 8,
	/* [한국어] 0x08 — Delta Record offset이 범위 초과. */

	DSA_COMP_DIF_ERR		= 9,
	/* [한국어] 0x09 — DIF check/strip에서 무결성 오류 발견(IDXD_DSA_STATUS_DIF_ERROR).
	 * dsa_hw_comp_record.dif_chk_* 필드로 어느 블록·어떤 태그가 어긋났는지 식별. */

	DSA_COMP_BAD_OPCODE		= 16,
	/* [한국어] 0x10 — 미지원/잘못된 opcode. WQ가 해당 opcode를 허용하지 않거나 디바이스가 모름. */

	DSA_COMP_INVALID_FLAGS		= 17,
	/* [한국어] 0x11 — 디스크립터 flags 조합이 잘못됨(예: CRAV=0인데 RCR=1). */

	DSA_COMP_NOZERO_RESERVE		= 18,
	/* [한국어] 0x12 — 디스크립터의 reserved 필드가 0이 아님. */

	DSA_COMP_XFER_ERANGE		= 19,
	/* [한국어] 0x13 — xfer_size 가 max_xfer_size 한계 초과. */

	DSA_COMP_DESC_CNT_ERANGE	= 20,
	/* [한국어] 0x14 — Batch 디스크립터의 desc_count가 max_batch_size 초과. */

	DSA_COMP_DR_ERANGE		= 21,
	/* [한국어] 0x15 — Delta record 크기 범위 오류. */

	DSA_COMP_OVERLAP_BUFFERS	= 22,
	/* [한국어] 0x16 — src와 dst 버퍼가 겹침(memmove 가 아닌 한 디바이스가 거부). */

	DSA_COMP_DCAST_ERR		= 23,
	/* [한국어] 0x17 — Dualcast: dst1과 dst2의 4K alignment가 일치하지 않음. */

	DSA_COMP_DESCLIST_ALIGN		= 24,
	/* [한국어] 0x18 — Batch desc_list_addr이 정렬되지 않음. */

	DSA_COMP_INT_HANDLE_INVAL	= 25,
	/* [한국어] 0x19 — 잘못된 interrupt handle. polling 모델에서는 거의 발생 안 함. */

	DSA_COMP_CRA_XLAT		= 26,
	/* [한국어] 0x1A — Completion Record Address translation 실패(SVM/PASID 문제). */

	DSA_COMP_CRA_ALIGN		= 27,
	/* [한국어] 0x1B — Completion record가 32B 정렬되지 않음. SPDK는 항상 정렬 보장. */

	DSA_COMP_ADDR_ALIGN		= 28,
	/* [한국어] 0x1C — src/dst 주소 alignment 위반(opcode별 요구사항 다름). */

	DSA_COMP_PRIV_BAD		= 29,
	/* [한국어] 0x1D — privilege 비트 부정합(WQ 권한과 디스크립터 priv 불일치). */

	DSA_COMP_TRAFFIC_CLASS_CONF	= 30,
	/* [한국어] 0x1E — Traffic class 구성 오류(group config). */

	DSA_COMP_PFAULT_RDBA		= 31,
	/* [한국어] 0x1F — Page fault on read-back address. */

	DSA_COMP_HW_ERR1		= 32,
	/* [한국어] 0x20 — 하드웨어 내부 오류 1. 디바이스 reset이 필요할 수 있음. */

	DSA_COMP_HW_ERR_DRB		= 33,
	/* [한국어] 0x21 — Drain readback hardware error. */

	DSA_COMP_TRANSLATION_FAIL	= 34,
	/* [한국어] 0x22 — IOMMU/PASID translation 실패. */
};

enum iaa_completion_status {
	IAA_COMP_NONE			= 0,
	/* [한국어] 0 — 미완료. iaa_hw_comp_record.status volatile polling sentinel. */
	IAA_COMP_SUCCESS		= 1,
	/* [한국어] 0x01 — IAA 정상 완료. */
	IAA_COMP_PAGE_FAULT_IR		= 4,
	/* [한국어] 0x04 — Internal resume page fault(DSA와 동일 의미). */
	IAA_COMP_OUTBUF_OVERFLOW	= 5,
	/* [한국어] 0x05 — 출력 버퍼 부족(압축 결과가 max_dst_size 초과). 응용은 더 큰 버퍼로 재시도. */
	IAA_COMP_BAD_OPCODE		= 16,
	/* [한국어] 0x10 — 미지원 opcode. */
	IAA_COMP_INVALID_FLAGS		= 17,
	/* [한국어] 0x11 — 잘못된 flags 조합(IAA_COMP_FLAGS 등). */
	IAA_COMP_NOZERO_RESERVE		= 18,
	/* [한국어] 0x12 — reserved 필드 비-zero. */
	IAA_COMP_INVALID_SIZE		= 19,
	/* [한국어] 0x13 — src1_size/xfer_size 범위 오류. */
	IAA_COMP_OVERLAP_BUFFERS	= 22,
	/* [한국어] 0x16 — 입력/출력 버퍼 overlap. */
	IAA_COMP_INT_HANDLE_INVAL	= 25,
	/* [한국어] 0x19 — 잘못된 interrupt handle. */
	IAA_COMP_CRA_XLAT		= 32,
	/* [한국어] 0x20 — Completion record address translation 실패. */
	IAA_COMP_CRA_ALIGN		= 33,
	/* [한국어] 0x21 — Completion record alignment 위반. */
	IAA_COMP_ADDR_ALIGN		= 34,
	/* [한국어] 0x22 — 입출력 주소 alignment 위반. */
	IAA_COMP_PRIV_BAD		= 35,
	/* [한국어] 0x23 — privilege 비트 부정합. */
	IAA_COMP_TRAFFIC_CLASS_CONF	= 36,
	/* [한국어] 0x24 — traffic class 구성 오류. */
	IAA_COMP_PFAULT_RDBA		= 37,
	/* [한국어] 0x25 — Page fault on read-back address. */
	IAA_COMP_HW_ERR1		= 38,
	/* [한국어] 0x26 — 하드웨어 내부 오류. */
	IAA_COMP_TRANSLATION_FAIL	= 39,
	/* [한국어] 0x27 — IOMMU translation 실패. */
	IAA_COMP_PRS_TIMEOUT		= 40,
	/* [한국어] 0x28 — Page Request Service timeout (SVM). */
	IAA_COMP_WATCHDOG		= 41,
	/* [한국어] 0x29 — Watchdog timeout. 압축 해제가 비정상적으로 오래 걸리면 디바이스가 abort. */
	IAA_COMP_INVALID_COMP_FLAG	= 48,
	/* [한국어] 0x30 — 잘못된 IAA compression flag. */
	IAA_COMP_INVALID_FILTER_FLAG	= 49,
	/* [한국어] 0x31 — 잘못된 filter flag(SCAN/EXTRACT 등 분석 opcode). */
	IAA_COMP_INVALID_NUM_ELEMS	= 50,
	/* [한국어] 0x32 — num_inputs 잘못. */
};

enum idxd_wq_state {
	WQ_DISABLED	= 0,
	/* [한국어] WQCFG.wq_state = 0 — WQ 비활성. 디스크립터 발행하면 거부. */
	WQ_ENABLED	= 1,
	/* [한국어] = 1 — WQ 활성. 발행 가능. */
};

enum idxd_wq_flag {
	WQ_FLAG_DEDICATED	= 0,
	/* [한국어] WQ가 Dedicated WQ(DWQ) 여부 플래그(bof/dedicated 필드의 의미가 컨텍스트별 다름).
	 * DWQ는 한 시점에 한 process만 발행 가능(MOVDIR64B 사용). */
	WQ_FLAG_BOF		= 1,
	/* [한국어] BOF(Block On Fault) — 페이지 폴트 시 디바이스가 SVM 페이지 fault 해결을 기다리도록 함.
	 * 0이면 폴트 시 즉시 DSA_COMP_PAGE_FAULT_NOBOF 반환. */
};

enum idxd_wq_type {
	WQT_NONE	= 0,
	/* [한국어] WQ 사용 용도 미설정. */
	WQT_KERNEL	= 1,
	/* [한국어] 커널 전용 WQ — 커널 내 idxd 드라이버가 직접 사용. */
	WQT_USER	= 2,
	/* [한국어] 유저스페이스 WQ — SPDK가 PF passthrough(VFIO)/idxd char dev 통해 직접 발행. */
	WQT_MDEV	= 3,
	/* [한국어] mdev(VFIO mediated device) WQ — 가상화: 게스트 VM에 WQ 슬라이스 할당. */
};

enum idxd_dev_state {
	IDXD_DEVICE_STATE_DISABLED	= 0,
	/* [한국어] 디바이스 disable 상태. enable 명령 전. */
	IDXD_DEVICE_STATE_ENABLED	= 1,
	/* [한국어] enable 완료, I/O 발행 가능. */
	IDXD_DEVICE_STATE_DRAIN		= 2,
	/* [한국어] drain 중 — in-flight 디스크립터 처리 후 disable로 진입. */
	IDXD_DEVICE_STATE_HALT		= 3,
	/* [한국어] halt — SW err 또는 치명적 오류로 정지. cmd로 reset 필요. */
};

enum idxd_device_reset_type {
	IDXD_DEVICE_RESET_SOFTWARE	= 0,
	/* [한국어] SW reset (CMD 레지스터로 trigger). */
	IDXD_DEVICE_RESET_FLR		= 1,
	/* [한국어] PCI Function Level Reset. 디바이스 전체 상태 클리어. */
	IDXD_DEVICE_RESET_WARM		= 2,
	/* [한국어] Warm reset (구성 일부 보존). */
	IDXD_DEVICE_RESET_COLD		= 3,
	/* [한국어] Cold reset (전체 초기화). */
};

enum idxd_cmds {
	IDXD_ENABLE_DEV		= 1,
	/* [한국어] 디바이스 활성화 명령. CMD 레지스터에 command_code=1로 발행. */
	IDXD_DISABLE_DEV	= 2,
	/* [한국어] 디바이스 비활성화. */
	IDXD_DRAIN_ALL		= 3,
	/* [한국어] 모든 in-flight 디스크립터 drain(끝까지 처리). */
	IDXD_ABORT_ALL		= 4,
	/* [한국어] 모든 in-flight 디스크립터 abort(즉시 중단, 미완료 상태로 둠). */
	IDXD_RESET_DEVICE	= 5,
	/* [한국어] 디바이스 reset. */
	IDXD_ENABLE_WQ		= 6,
	/* [한국어] 특정 WQ 활성화. operand 필드에 WQ index. */
	IDXD_DISABLE_WQ		= 7,
	/* [한국어] 특정 WQ 비활성화. */
	IDXD_DRAIN_WQ		= 8,
	/* [한국어] 특정 WQ drain. */
	IDXD_ABORT_WQ		= 9,
	/* [한국어] 특정 WQ abort. */
	IDXD_RESET_WQ		= 10,
	/* [한국어] 특정 WQ reset(상태 클리어). */
};

enum idxd_cmdsts_err {
	IDXD_CMDSTS_SUCCESS		= 0,
	/* [한국어] CMD 성공. CMDSTS.err = 0. */
	IDXD_CMDSTS_INVAL_CMD		= 1,
	/* [한국어] 잘못된 명령 코드. */
	IDXD_CMDSTS_INVAL_WQIDX		= 2,
	/* [한국어] 잘못된 WQ index. */
	IDXD_CMDSTS_HW_ERR		= 3,
	/* [한국어] 하드웨어 오류. */
	IDXD_CMDSTS_ERR_DEV_ENABLED	= 16,
	/* [한국어] enable 시도했으나 이미 활성화됨. */
	IDXD_CMDSTS_ERR_CONFIG		= 17,
	/* [한국어] 디바이스 config(GENCFG/GRPCFG/WQCFG) 부정합. */
	IDXD_CMDSTS_ERR_BUSMASTER_EN	= 18,
	/* [한국어] PCI Bus Master Enable이 꺼져 있음 — config space 설정 필요. */
	IDXD_CMDSTS_ERR_PASID_INVAL	= 19,
	/* [한국어] 잘못된 PASID. */
	IDXD_CMDSTS_ERR_WQ_SIZE_ERANGE	= 20,
	/* [한국어] WQCFG.wq_size 한계 초과(WQCAP.total_wq_size 초과). */
	IDXD_CMDSTS_ERR_GRP_CONFIG	= 21,
	/* [한국어] Group config 오류 1. */
	IDXD_CMDSTS_ERR_GRP_CONFIG2	= 22,
	/* [한국어] Group config 오류 2. */
	IDXD_CMDSTS_ERR_GRP_CONFIG3	= 23,
	/* [한국어] Group config 오류 3. */
	IDXD_CMDSTS_ERR_GRP_CONFIG4	= 24,
	/* [한국어] Group config 오류 4. */
	IDXD_CMDSTS_ERR_DEV_NOTEN	= 32,
	/* [한국어] WQ enable 시 디바이스가 enable 상태가 아님. */
	IDXD_CMDSTS_ERR_WQ_ENABLED	= 33,
	/* [한국어] WQ enable 시도했으나 이미 활성화됨. */
	IDXD_CMDSTS_ERR_WQ_SIZE		= 34,
	/* [한국어] WQ size 부적합. */
	IDXD_CMDSTS_ERR_WQ_PRIOR	= 35,
	/* [한국어] WQ priority 부정합. */
	IDXD_CMDSTS_ERR_WQ_MODE		= 36,
	/* [한국어] WQ mode(shared/dedicated) 가 디바이스 capability와 불일치. */
	IDXD_CMDSTS_ERR_BOF_EN		= 37,
	/* [한국어] BOF(Block-on-Fault) 활성화 오류. */
	IDXD_CMDSTS_ERR_PASID_EN	= 38,
	/* [한국어] PASID 활성화 오류. */
	IDXD_CMDSTS_ERR_MAX_BATCH_SIZE	= 39,
	/* [한국어] max_batch_shift 한계 초과. */
	IDXD_CMDSTS_ERR_MAX_XFER_SIZE	= 40,
	/* [한국어] max_xfer_shift 한계 초과. */
	IDXD_CMDSTS_ERR_DIS_DEV_EN	= 49,
	/* [한국어] WQ 활성화 상태에서 disable device 시도. */
	IDXD_CMDSTS_ERR_DEV_NOT_EN	= 50,
	/* [한국어] disable device 시도했으나 이미 disable 상태. */
	IDXD_CMDSTS_ERR_INVAL_INT_IDX	= 65,
	/* [한국어] 잘못된 interrupt index. */
	IDXD_CMDSTS_ERR_NO_HANDLE	= 66,
	/* [한국어] interrupt handle 없음. */
};

enum idxd_wq_hw_state {
	IDXD_WQ_DEV_DISABLED	= 0,
	/* [한국어] HW 입장 WQ disabled. */
	IDXD_WQ_DEV_ENABLED	= 1,
	/* [한국어] HW 입장 WQ enabled. */
	IDXD_WQ_DEV_BUSY	= 2,
	/* [한국어] WQ가 enable→disable 전이 중(drain 진행). */
};

/*
 * [한국어] struct idxd_hw_desc - IDXD 64B 디스크립터 (DSA/IAA 공통).
 * 디바이스가 portal로 받은 64B를 그대로 읽어 처리. 모든 opcode가 같은 64B 레이아웃을 공유하며,
 * opcode별로 src/dst/xfer_size/op_specific 필드의 의미가 달라진다.
 *
 * 설정자: lib/idxd 가 spdk_idxd_submit_*() 안에서 채움.
 * 읽는 자: IDXD 디바이스 엔진(MMIO portal에 64B atomic write 후 즉시 디바이스가 읽음).
 * 동기화: ENQCMDS/MOVDIR64B는 64B atomic write를 보장(cache-line 분할 없음).
 *         발행 후 디스크립터 메모리는 디바이스 사용 중이므로 응용이 수정 금지(완료 후 재사용 가능).
 * 정렬: 64B aligned (디바이스 요구).
 */
struct idxd_hw_desc {
	uint32_t	pasid: 20;
	/* [한국어] PASID(Process Address Space ID) — SVM(Shared Virtual Memory) 사용 시 디바이스가
	 * 사용자 가상주소를 IOMMU 통해 해석할 때 필요. 0..(2^20-1) 범위.
	 * 설정자: 드라이버. 읽는 자: 디바이스. SVM 미사용 시 0이면 디바이스가 디폴트 PASID 사용. */
	uint32_t	rsvd: 11;
	/* [한국어] 예약 비트(11). 반드시 0이어야 함 — 비-zero면 DSA_COMP_NOZERO_RESERVE. */
	uint32_t	priv: 1;
	/* [한국어] privilege 비트 — 1이면 디바이스가 privileged 권한으로 메모리 접근(커널 메모리 등).
	 * WQCFG.priv 와 일치해야 함. 사용자 모드 SPDK는 0. */
	uint32_t	flags: 24;
	/* [한국어] DSA flags 비트마스크 — IDXD_FLAG_FENCE/CRAV/RCR/CACHE_CONTROL/... 조합.
	 * 24비트 너비 — flags 비트 위치는 0..23 까지 정의. */
	uint32_t	opcode: 8;
	/* [한국어] 8비트 opcode — IDXD_OPCODE_NOP(0x00), DRAIN(0x02), MEMMOVE(0x03), FILL(0x04),
	 * COMPARE(0x09), CRCGEN(0x10), COPY_CRC(0x11), DIF_CHECK(0x12), DIF_INSERT(0x13),
	 * DIF_STRIP(0x14), DIF_UPDATE(0x15), DIX_GEN(0x17), CACHE_FLUSH(0x18), BATCH(0x01),
	 * IAA: COMPRESS(0x43), DECOMPRESS(0x42), CRC64(0x44), SCAN/EXTRACT/SELECT/RLE_BURST/...
	 * 디바이스가 capability(opcap)에서 지원 여부를 광고. */

	uint64_t	completion_addr;
	/* [한국어] 완료 레코드의 IOVA(물리/매핑) 주소. 디바이스가 완료 시 dsa_hw_comp_record(32B) 또는
	 * iaa_hw_comp_record(64B) 를 이 주소에 기록.
	 * 설정자: lib/idxd가 채널 완료 record array의 슬롯 주소를 spdk_vtophys로 변환해 채움.
	 * 읽는 자: 디바이스. 정렬 요구: 32B(완료 레코드 정렬과 일치). */

	union {
		uint64_t	src_addr;
		/* [한국어] 표준 src 주소(MEMMOVE/COMPARE/CRC/DIF 의 source). */
		uint64_t	src1_addr;
		/* [한국어] IAA의 src1(압축/해제 입력 1번). */
		uint64_t	readback_addr;
		/* [한국어] DRAIN_READBACK 변종에서 readback 주소. */
		uint64_t	pattern;
		/* [한국어] FILL opcode에서 8B 반복 패턴 값(주소 자리에 패턴 그 자체 저장).
		 * spdk_idxd_submit_fill의 fill_pattern 인자가 여기로 들어감. */
		uint64_t	desc_list_addr;
		/* [한국어] BATCH opcode에서 sub-descriptor 배열의 주소. */
	};
	/* [한국어] 위 union 의 의미는 opcode 별로 결정 — 디바이스가 opcode를 보고 어느 멤버로 해석할지 결정. */

	union {
		uint64_t	dst_addr;
		/* [한국어] 표준 dst 주소(MEMMOVE/FILL/COPY_CRC 등). */
		uint64_t	readback_addr2;
		/* [한국어] DRAIN_READBACK 변종 두 번째 주소. */
		uint64_t	src2_addr;
		/* [한국어] COMPARE의 두 번째 source / IAA의 src2(보통 AECS 또는 보조 데이터). */
		uint64_t	comp_pattern;
		/* [한국어] COMPARE_PATTERN opcode에서 비교 대상 패턴 8B. */
	};

	union {
		uint32_t	src1_size;
		/* [한국어] IAA src1 입력 크기. */
		uint32_t	xfer_size;
		/* [한국어] 표준 전송 크기(바이트). MEMMOVE/COPY_CRC/FILL 등.
		 * max는 1<<gencap.max_xfer_shift (기본 2^28=256MB 또는 그 이상). */
		uint32_t	desc_count;
		/* [한국어] BATCH opcode에서 sub-descriptor 개수.
		 * max는 1<<gencap.max_batch_shift. */
	};

	uint16_t	int_handle;
	/* [한국어] interrupt handle — completion interrupt 사용 시 어느 MSI-X 벡터인지 식별.
	 * polling 모델에서는 사용 안 함(0). */

	union {
		uint16_t	rsvd1;
		/* [한국어] 일반 opcode 예약 영역. 0이어야 함. */
		uint16_t	compr_flags;
		/* [한국어] IAA COMPRESS opcode 전용 flags(IAA_COMP_FLUSH_OUTPUT|IAA_COMP_APPEND_EOB 등). */
		uint16_t	decompr_flags;
		/* [한국어] IAA DECOMPRESS opcode 전용 flags(IAA_DECOMP_*). */
	};

	/* [한국어] 아래 24B union — opcode-specific 영역. opcode마다 다른 sub-구조체로 해석. */
	union {
		struct {
			uint64_t	src2_addr;
			/* [한국어] IAA 보조 src2 주소(예: AECS 위치). */
			uint32_t	max_dst_size;
			/* [한국어] IAA 출력 버퍼 한계(압축 시 dst 가용 크기, overflow 검증용). */
			uint32_t	src2_size;
			/* [한국어] src2 크기. */
			uint32_t	filter_flags;
			/* [한국어] IAA 분석 opcode(SCAN/EXTRACT/SELECT 등) 의 필터 옵션. */
			uint32_t	num_inputs;
			/* [한국어] 분석 opcode의 입력 element 수. */
		} iaa;
		/* [한국어] IAA 분석/압축 opcode가 사용하는 op_specific 레이아웃. */

		uint8_t		expected_res;
		/* [한국어] COMPARE_PATTERN 결과의 expected value(스캔 정합 여부 평가용). */

		struct {
			uint64_t	addr;
			/* [한국어] Delta record(변경분 기록) 위치 주소. */
			uint32_t	max_size;
			/* [한국어] delta record 출력 버퍼 한계. */
		} delta;
		/* [한국어] CREATE_DELTA_RECORD / APPLY_DELTA opcode 전용 영역. */

		uint32_t	delta_rec_size;
		/* [한국어] APPLY_DELTA opcode에서 입력 delta record 크기. */

		uint64_t	dest2;
		/* [한국어] DUALCAST opcode의 두 번째 destination 주소(dst1은 위 union의 dst_addr).
		 * 4KB 정렬 필요. */

		struct {
			uint32_t	seed;
			/* [한국어] CRC32C 시드 값. spdk_idxd_submit_crc32c의 seed 인자가 여기로. */
			uint32_t	rsvd;
			/* [한국어] 예약. 0. */
			uint64_t	addr;
			/* [한국어] CRC seed를 메모리에서 읽어올 경우(CRC_READ_CRC_SEED flag set) 의 주소. */
		} crc32c;
		/* [한국어] CRCGEN/COPY_CRC opcode 전용 영역. */

		struct {
			uint8_t		src_flags;
			/* [한국어] DIF_CHECK src flags(IDXD_DIF_SOURCE_FLAG_*). RefTag 타입, 검사 disable 등. */
			uint8_t		rsvd1;
			/* [한국어] 예약. 0. */
			uint8_t		flags;
			/* [한국어] DIF_CHECK 일반 flags(블록 크기 코드 + INVERT_CRC_*). */
			uint8_t		rsvd2[5];
			/* [한국어] 예약 5B. 0. */
			uint32_t	ref_tag_seed;
			/* [한국어] Reference Tag 시작값(NVMe Type 1: LBA, Type 3: 고정). */
			uint16_t	app_tag_mask;
			/* [한국어] Application Tag 비교 시 마스크(0xFFFF=전체 비교, 0x0000=무시). */
			uint16_t	app_tag_seed;
			/* [한국어] Application Tag 기대값. */
		} dif_chk;
		/* [한국어] DIF_CHECK opcode 전용. */

		struct {
			uint8_t		rsvd1;
			/* [한국어] 예약. 0. */
			uint8_t		dest_flag;
			/* [한국어] DIF_INSERT destination flags — 생성된 PI의 RefTag 타입 등. */
			uint8_t		flags;
			/* [한국어] 일반 flags(블록 크기 등). */
			uint8_t		rsvd2[13];
			/* [한국어] 예약 13B. 0. */
			uint32_t	ref_tag_seed;
			/* [한국어] 삽입할 RefTag 시작값. */
			uint16_t	app_tag_mask;
			/* [한국어] 삽입할 AppTag 마스크. */
			uint16_t	app_tag_seed;
			/* [한국어] 삽입할 AppTag 값. */
		} dif_ins;
		/* [한국어] DIF_INSERT opcode 전용. */

		struct {
			uint8_t		src_flags;
			/* [한국어] 입력 측 DIF flags. */
			uint8_t		dest_flags;
			/* [한국어] 출력 측 DIF flags. */
			uint8_t		flags;
			/* [한국어] 일반 flags. */
			uint8_t		rsvd[5];
			/* [한국어] 예약 5B. 0. */
			uint32_t	src_ref_tag_seed;
			/* [한국어] src RefTag 시작값(검증). */
			uint16_t	src_app_tag_mask;
			/* [한국어] src AppTag 마스크. */
			uint16_t	src_app_tag_seed;
			/* [한국어] src AppTag 기대값. */
			uint32_t	dest_ref_tag_seed;
			/* [한국어] dst RefTag 시작값(생성). */
			uint16_t	dest_app_tag_mask;
			/* [한국어] dst AppTag 마스크. */
			uint16_t	dest_app_tag_seed;
			/* [한국어] dst AppTag 시드. */
		} dif_upd;
		/* [한국어] DIF_UPDATE opcode 전용 — src의 PI 검증과 동시에 dst의 PI 갱신. */

		struct {
			uint8_t		src_flags;
			/* [한국어] DIF_STRIP src flags. */
			uint8_t		rsvd1;
			/* [한국어] 예약. 0. */
			uint8_t		flags;
			/* [한국어] 일반 flags(블록 크기 등). */
			uint8_t		rsvd2[5];
			/* [한국어] 예약 5B. 0. */
			uint32_t	ref_tag_seed;
			/* [한국어] 검증할 RefTag 시작값. */
			uint16_t	app_tag_mask;
			/* [한국어] 검증할 AppTag 마스크. */
			uint16_t	app_tag_seed;
			/* [한국어] 검증할 AppTag 기대값. */
		} dif_strip;
		/* [한국어] DIF_STRIP opcode 전용. */

		struct {
			uint8_t		rsvd1;
			/* [한국어] 예약. 0. */
			uint8_t		dest_flags;
			/* [한국어] DIX 생성 시 dst PI 형식 옵션. */
			uint8_t		flags;
			/* [한국어] 일반 flags. */
			uint8_t		rsvd2[13];
			/* [한국어] 예약 13B. 0. */
			uint32_t	ref_tag_seed;
			/* [한국어] 생성할 RefTag 시작값. */
			uint16_t	app_tag_mask;
			/* [한국어] 생성할 AppTag 마스크. */
			uint16_t	app_tag_seed;
			/* [한국어] 생성할 AppTag 시드. */
		} dix_gen;
		/* [한국어] DIX_GEN opcode 전용 — 데이터와 분리된 mdiov 메타 버퍼에 PI 작성. */

		uint8_t		op_specific[24];
		/* [한국어] 위에서 정의되지 않은 opcode/실험적 opcode를 위한 raw 24B 영역.
		 * spdk_idxd_submit_raw_desc 사용 시 응용이 직접 채움. */
	};
} __attribute((aligned(64)));
/* [한국어] 64B 정렬 — 디스크립터를 portal에 ENQCMDS/MOVDIR64B로 발행할 때 cache-line 단위 atomic
 * write가 필수. 정렬되지 않으면 split write가 발생해 디바이스가 잘못된 데이터를 읽음. */
SPDK_STATIC_ASSERT(sizeof(struct idxd_hw_desc) == 64, "size mismatch");
/* [한국어] 컴파일 타임 검증 — 64B가 아니면 빌드 실패. ABI drift 방지. */

/*
 * [한국어] struct dsa_hw_comp_record - DSA 32B 완료 레코드.
 * 디바이스가 idxd_hw_desc.completion_addr 가 가리키는 주소에 작성. 응용은 status 비트를 polling
 * 으로 watch하여 완료 검출.
 *
 * 설정자: IDXD 디바이스(메모리 쓰기). 응용/드라이버는 status를 0으로 reset하여 재사용.
 * 읽는 자: spdk_idxd_process_events()의 polling loop.
 * 동기화: status는 volatile uint8_t로 선언되어 컴파일러가 read를 reorder/제거하지 않도록 함.
 *         x86은 strong memory model이라 디바이스 쓰기가 cache-coherent하게 SW polling에 보임.
 */
struct dsa_hw_comp_record {
	volatile uint8_t	status;
	/* [한국어] 완료 상태 코드 — 0=미완료, 0x01=DSA_COMP_SUCCESS, 그 외=오류 코드.
	 * volatile 키워드로 polling read 보장.
	 * 설정자: 디바이스(완료 시 1B atomic write).
	 * 읽는 자: process_events() loop.
	 * 동기화: 디바이스 write → cache coherency → SW read. */

	union {
		uint8_t		result;
		/* [한국어] opcode별 raw 결과 1B (COMPARE의 일치/불일치 등). */
		uint8_t		dif_status;
		/* [한국어] DIF opcode에서 어느 태그(Guard/AppTag/RefTag)가 어긋났는지 비트별 표시. */
	};

	uint16_t		rsvd;
	/* [한국어] 예약. 0. */

	uint32_t		bytes_completed;
	/* [한국어] 디바이스가 부분 완료/실패 시 처리한 바이트 수. 페이지 폴트 부분 처리 등에서 사용.
	 * COMPARE에서는 첫 mismatch 위치 오프셋. */

	uint64_t		fault_addr;
	/* [한국어] page fault 발생 시 폴트 주소(IOVA). 응용/드라이버가 페이지 wire 후 재시도할 때 참고. */

	union {
		uint32_t	delta_rec_size;
		/* [한국어] CREATE_DELTA_RECORD opcode 결과 — 생성된 delta 크기. */

		uint32_t	crc32c_val;
		/* [한국어] CRCGEN/COPY_CRC opcode 결과 CRC32-C 값. spdk_idxd_submit_crc32c의 *crc_dst로 복사. */

		struct {
			uint32_t	dif_chk_ref_tag;
			/* [한국어] DIF_CHECK 실패 시 어긋난 RefTag 값. */
			uint16_t	dif_chk_app_tag_mask;
			/* [한국어] DIF_CHECK 실패 시 사용한 AppTag mask. */
			uint16_t	dif_chk_app_tag;
			/* [한국어] DIF_CHECK 실패 시 어긋난 AppTag 값. */
		};
		/* [한국어] DIF_CHECK opcode 결과 영역. */

		struct {
			uint64_t	rsvd;
			/* [한국어] 예약. 0. */
			uint32_t	ref_tag;
			/* [한국어] DIF_INSERT가 마지막으로 생성한 RefTag. */
			uint16_t	app_tag_mask;
			/* [한국어] AppTag mask. */
			uint16_t	app_tag;
			/* [한국어] 마지막 AppTag. */
		} dif_ins_comp;
		/* [한국어] DIF_INSERT 결과. */

		struct {
			uint32_t	src_ref_tag;
			/* [한국어] DIF_UPDATE: src에서 읽은 RefTag. */
			uint16_t	src_app_tag_mask;
			/* [한국어] src AppTag mask. */
			uint16_t	src_app_tag;
			/* [한국어] src AppTag. */
			uint32_t	dest_ref_tag;
			/* [한국어] dst에 쓴 RefTag. */
			uint16_t	dest_app_tag_mask;
			/* [한국어] dst AppTag mask. */
			uint16_t	dest_app_tag;
			/* [한국어] dst AppTag. */
		} dif_upd_comp;
		/* [한국어] DIF_UPDATE 결과 — src 검증 + dst 생성 양쪽 정보. */

		struct {
			uint64_t	rsvd;
			/* [한국어] 예약. 0. */
			uint32_t	dix_gen_ref_tag;
			/* [한국어] DIX_GEN: 생성한 마지막 RefTag. */
			uint16_t	dix_gen_app_tag_mask;
			/* [한국어] AppTag mask. */
			uint16_t	dix_gen_app_tag;
			/* [한국어] AppTag. */
		} dix_gen_comp;
		/* [한국어] DIX_GEN 결과. */

		uint8_t		op_specific[16];
		/* [한국어] 위에서 정의되지 않은 opcode/실험적 opcode 결과를 위한 raw 16B. */
	};
};
SPDK_STATIC_ASSERT(sizeof(struct dsa_hw_comp_record) == 32, "size mismatch");
/* [한국어] 컴파일 타임 검증 — 32B 정확. */

/*
 * [한국어] struct iaa_hw_comp_record - IAA 64B 완료 레코드.
 * IAA는 압축/해제 결과 추가 정보(output_size, output_bits 등)를 위해 64B 사용.
 * DSA와 별도의 구조체로 정의된 이유.
 *
 * 설정자: IAA 디바이스. 읽는 자: process_events().
 * 동기화: status volatile read (DSA와 동일). 정렬: 64B(완료 record array 슬롯 단위).
 */
struct iaa_hw_comp_record {
	volatile uint8_t	status;
	/* [한국어] IAA 완료 상태 — iaa_completion_status enum 값. 0=미완료. */
	uint8_t			error_code;
	/* [한국어] 추가 에러 코드(opcode-specific). status가 오류일 때 세부 분석용. */
	uint16_t		rsvd;
	/* [한국어] 예약. 0. */
	uint32_t		bytes_completed;
	/* [한국어] 입력에서 처리한 바이트 수(부분 진행 시). */
	uint64_t		fault_addr;
	/* [한국어] page fault 시 폴트 주소. */
	uint32_t		invalid_flags;
	/* [한국어] flags 검증 실패 시 잘못된 비트 위치 표시. */
	uint32_t		rsvd2;
	/* [한국어] 예약. 0. */
	uint32_t		output_size;
	/* [한국어] 출력 크기(압축: 결과 압축 데이터 크기, 해제: 산출된 평문 크기).
	 * spdk_idxd_submit_compress의 *output_size로 복사됨. */
	uint8_t			output_bits;
	/* [한국어] 출력의 마지막 바이트에서 유효한 bit 수(bit-stream 정렬). */
	uint8_t			rsvd3;
	/* [한국어] 예약. 0. */
	uint16_t		rsvd4;
	/* [한국어] 예약. 0. */
	uint64_t		rsvd5[4];
	/* [한국어] 예약 32B. 0. 64B 패딩 채움. */
};
SPDK_STATIC_ASSERT(sizeof(struct iaa_hw_comp_record) == 64, "size mismatch");
/* [한국어] 64B 검증. */

/*
 * [한국어] struct iaa_aecs - IAA AECS(Accelerator Engine Context Save) 1568B 구조체.
 * IAA 압축/해제의 stateful 정보(허프만 테이블, accumulator 등)를 메모리에 저장.
 * 디바이스가 IAA_FLAG_RD_SRC2_AECS flag로 src2를 AECS로 해석하면 이 구조체를 읽어 상태 복원.
 *
 * 설정자: 응용 또는 IAA 디바이스(누적 결과 갱신). 읽는 자: IAA 디바이스.
 * 동기화: 한 디스크립터 처리 중에는 디바이스 점유 — 응용 수정 금지.
 * 정렬: 32B 권장.
 */
struct iaa_aecs {
	uint32_t crc;
	/* [한국어] 누적 CRC32 값. 청크 단위 압축에서 이전 청크 CRC. */
	uint32_t xor_checksum;
	/* [한국어] 누적 XOR 체크섬(데이터 무결성 검증용). */
	uint32_t rsvd[5];
	/* [한국어] 예약 20B. 0. */
	uint32_t num_output_accum_bits;
	/* [한국어] output_accum 배열에 누적된 비트 수(bit-stream 진행 상태). */
	uint8_t output_accum[256];
	/* [한국어] 출력 bit-stream 누적 버퍼(연속 압축 시 마지막 미완 바이트 보존). */
	uint32_t ll_sym[286];
	/* [한국어] DEFLATE Literal/Length 허프만 코드 테이블(286 심볼). */
	uint32_t rsvd1;
	/* [한국어] 예약 4B. 0. */
	uint32_t rsvd3;
	/* [한국어] 예약 4B. 0. */
	uint32_t d_sym[30];
	/* [한국어] DEFLATE Distance 허프만 코드 테이블(30 심볼). */
	uint32_t pad[2];
	/* [한국어] 패딩 8B — 1568B 정렬. */
};
SPDK_STATIC_ASSERT(sizeof(struct iaa_aecs) == 1568, "size mismatch");
/* [한국어] 1568B 검증 — 4 + 4 + 20 + 4 + 256 + 286*4 + 4 + 4 + 30*4 + 8 = 1568. */

/*
 * [한국어] union idxd_gencap_register - General Capability 8B 레지스터(BAR0).
 * 디바이스가 지원하는 일반 기능을 광고. probe 단계에서 한 번 읽어 드라이버 동작 결정.
 *
 * 설정자: HW(read-only). 읽는 자: 드라이버 init. 동기화: 1회 읽기, 동시성 없음.
 */
union idxd_gencap_register {
	struct {
		uint64_t block_on_fault: 1;
		/* [한국어] 비트 0 — Block-on-Fault 지원(SVM 페이지 폴트 시 디바이스가 wait). */
		uint64_t overlap_copy: 1;
		/* [한국어] 비트 1 — src/dst overlap copy 지원. */
		uint64_t cache_control_mem: 1;
		/* [한국어] 비트 2 — Cache Control(non-temporal) memory write 지원. */
		uint64_t cache_control_cache: 1;
		/* [한국어] 비트 3 — Cache Control to cache 지원. */
		uint64_t command_cap: 1;
		/* [한국어] 비트 4 — CMDCAP 레지스터 존재 여부. */
		uint64_t rsvd: 3;
		/* [한국어] 예약 비트 3개. */
		uint64_t dest_readback: 1;
		/* [한국어] 비트 8 — Destination Readback 지원. */
		uint64_t drain_readback: 1;
		/* [한국어] 비트 9 — Drain Readback 지원. */
		uint64_t rsvd2: 6;
		/* [한국어] 예약 6비트. */
		uint64_t max_xfer_shift: 5;
		/* [한국어] 비트 16-20 — max transfer size shift. 1<<max_xfer_shift = 단일 디스크립터 최대 byte 수. */
		uint64_t max_batch_shift: 4;
		/* [한국어] 비트 21-24 — max batch size shift. 1<<max_batch_shift = batch 디스크립터 sub-desc 수. */
		uint64_t max_ims_mult: 6;
		/* [한국어] 비트 25-30 — Interrupt Message Storage 크기 multiplier. */
		uint64_t config_support: 1;
		/* [한국어] 비트 31 — software-driven configuration 지원(GRPCFG/WQCFG 쓰기 가능). */
		uint64_t rsvd3: 32;
		/* [한국어] 예약 32비트. */
	};
	uint64_t raw;
	/* [한국어] 비트필드 전체를 64비트로 한 번에 읽기 위한 raw view. MMIO load는 64비트 단위로 수행 권장. */
};
SPDK_STATIC_ASSERT(sizeof(union idxd_gencap_register) == 8, "size mismatch");

/*
 * [한국어] union idxd_wqcap_register - WQ Capability 8B 레지스터.
 * WQ 관련 디바이스 능력 광고.
 */
union idxd_wqcap_register {
	struct {
		uint64_t total_wq_size: 16;
		/* [한국어] 비트 0-15 — 모든 WQ 합산 size 한도(디스크립터 슬롯 수). 드라이버가 WQ 분배 시 한계. */
		uint64_t num_wqs: 8;
		/* [한국어] 비트 16-23 — 디바이스가 가진 WQ 개수. */
		uint64_t wqcfg_size: 4;
		/* [한국어] 비트 24-27 — WQCFG entry 크기 코드. */
		uint64_t rsvd: 20;
		/* [한국어] 예약. */
		uint64_t shared_mode: 1;
		/* [한국어] 비트 48 — Shared WQ(SWQ) 모드 지원. ENQCMDS 명령으로 다중 process가 안전하게 발행. */
		uint64_t dedicated_mode: 1;
		/* [한국어] 비트 49 — Dedicated WQ(DWQ) 모드 지원. MOVDIR64B로 한 process 전용. */
		uint64_t ats_support: 1;
		/* [한국어] 비트 50 — Address Translation Service(PCIe ATS) 지원 — IOMMU TLB 활용. */
		uint64_t priority: 1;
		/* [한국어] 비트 51 — WQ priority 지원. */
		uint64_t occupancy: 1;
		/* [한국어] 비트 52 — WQ occupancy 측정 지원. */
		uint64_t occupancy_int: 1;
		/* [한국어] 비트 53 — occupancy 임계 도달 인터럽트 지원. */
		uint64_t rsvd1: 10;
		/* [한국어] 예약. */
	};
	uint64_t raw;
	/* [한국어] raw 64비트 view. */
};
SPDK_STATIC_ASSERT(sizeof(union idxd_wqcap_register) == 8, "size mismatch");

/*
 * [한국어] union idxd_groupcap_register - Group Capability 8B 레지스터.
 * Group(WQ + Engine 묶음) 관련 능력.
 */
union idxd_groupcap_register {
	struct {
		uint64_t num_groups: 8;
		/* [한국어] 비트 0-7 — 디바이스의 group 개수. */
		uint64_t read_bufs: 8;
		/* [한국어] 비트 8-15 — group이 사용 가능한 read buffer 풀 크기. */
		uint64_t read_bufs_ctrl: 1;
		/* [한국어] 비트 16 — read buffer per-group 제어 지원. */
		uint64_t read_bus_limit: 1;
		/* [한국어] 비트 17 — read bus bandwidth limit 지원. */
		uint64_t rsvd: 46;
		/* [한국어] 예약. */
	};
	uint64_t raw;
	/* [한국어] raw view. */
};
SPDK_STATIC_ASSERT(sizeof(union idxd_groupcap_register) == 8, "size mismatch");

/*
 * [한국어] union idxd_enginecap_register - Engine Capability 8B 레지스터.
 */
union idxd_enginecap_register {
	struct {
		uint64_t num_engines: 8;
		/* [한국어] 비트 0-7 — 디바이스의 engine 개수(병렬 처리 단위). 보통 4. */
		uint64_t rsvd: 56;
		/* [한국어] 예약. */
	};
	uint64_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_enginecap_register) == 8, "size mismatch");

/*
 * [한국어] struct idxd_opcap_register - Operation Capability 32B(256-bit) bitmap.
 * 각 비트가 opcode 0..255에 대응. 비트가 1이면 디바이스가 해당 opcode 지원.
 * 예: DSA → MEMMOVE(0x03), CRCGEN(0x10), DIF_*(0x12-0x15) 비트 set.
 *      IAA → COMPRESS(0x43), DECOMPRESS(0x42), CRC64(0x44), SCAN/EXTRACT/... set.
 *
 * 설정자: HW(read-only). 읽는 자: 드라이버가 디바이스 종류(DSA/IAA) 판별, opcode 지원 검사.
 */
struct idxd_opcap_register {
	uint64_t raw[4];
	/* [한국어] 256비트를 64비트 4개로 표현. raw[0] = opcode 0-63, raw[1] = 64-127, ... .
	 * 응용/드라이버가 (raw[op/64] >> (op%64)) & 1 로 검사. */
};
SPDK_STATIC_ASSERT(sizeof(struct idxd_opcap_register) == 32, "size mismatch");

/*
 * [한국어] union idxd_offsets_register - 보조 테이블 offset 16B 레지스터.
 * GRPCFG/WQCFG/MSIX_PERM/IMS/PERFMON 테이블이 BAR0 상에서 어디에 위치하는지 표시.
 * 실제 offset = 필드값 * IDXD_TABLE_OFFSET_MULT(0x100).
 */
union idxd_offsets_register {
	struct {
		uint64_t grpcfg: 16;
		/* [한국어] 비트 0-15 — GRPCFG 테이블 offset(units of 256B). */
		uint64_t wqcfg: 16;
		/* [한국어] 비트 16-31 — WQCFG 테이블 offset. */
		uint64_t msix_perm: 16;
		/* [한국어] 비트 32-47 — MSI-X permission 테이블 offset. */
		uint64_t ims: 16;
		/* [한국어] 비트 48-63 — IMS(Interrupt Message Storage) 테이블 offset. */
		uint64_t perfmon: 16;
		/* [한국어] 두 번째 64비트 — PERFMON 영역 offset. */
		uint64_t rsvd: 48;
		/* [한국어] 예약. */
	};
	uint64_t raw[2];
	/* [한국어] 16B raw view (2 x uint64). */
};
SPDK_STATIC_ASSERT(sizeof(union idxd_offsets_register) == 16, "size mismatch");

/*
 * [한국어] union idxd_gencfg_register - General Configuration 4B 레지스터.
 * 드라이버가 일반 동작 옵션을 설정.
 *
 * 설정자: 드라이버(write). 읽는 자: HW. 동기화: enable 전 한 번에 설정.
 */
union idxd_gencfg_register {
	struct {
		uint8_t		global_read_buf_limit;
		/* [한국어] 8비트 — global read buffer 한계(group capability와 함께 사용). */
		uint8_t		reserved0 : 4;
		/* [한국어] 예약 4비트. */
		uint8_t		user_mode_int_enabled : 1;
		/* [한국어] 비트 12 — user-mode interrupt 활성화. SPDK polling 모델에서는 0. */
		uint8_t		reserved1 : 3;
		/* [한국어] 예약 3비트. */
		uint16_t	reserved2;
		/* [한국어] 예약 16비트. */
	};
	uint32_t raw;
	/* [한국어] raw 32비트 view. */
};
SPDK_STATIC_ASSERT(sizeof(union idxd_gencfg_register) == 4, "size mismatch");

/*
 * [한국어] union idxd_genctrl_register - General Control 4B 레지스터.
 * 인터럽트 활성/비활성 토글.
 */
union idxd_genctrl_register {
	struct {
		uint32_t	sw_err_int_enable : 1;
		/* [한국어] 비트 0 — SW error 발생 시 인터럽트 발행 활성화. */
		uint32_t	halt_state_int_enable : 1;
		/* [한국어] 비트 1 — Halt 상태 진입 시 인터럽트. */
		uint32_t	reserved : 30;
		/* [한국어] 예약. */
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_genctrl_register) == 4, "size mismatch");

/*
 * [한국어] union idxd_gensts_register - General Status 4B 레지스터.
 * 디바이스 현재 상태(state, reset_type) 표시.
 *
 * 설정자: HW. 읽는 자: 드라이버 모니터링.
 */
union idxd_gensts_register {
	struct {
		uint32_t state: 2;
		/* [한국어] 비트 0-1 — idxd_dev_state 값(DISABLED/ENABLED/DRAIN/HALT). */
		uint32_t reset_type: 2;
		/* [한국어] 비트 2-3 — idxd_device_reset_type 값. */
		uint32_t rsvd: 28;
		/* [한국어] 예약. */
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_gensts_register) == 4, "size mismatch");

/*
 * [한국어] union idxd_intcause_register - Interrupt Cause 4B 레지스터.
 * 인터럽트가 발생한 이유 비트별 표시(write-1-to-clear 유형).
 */
union idxd_intcause_register {
	struct {
		uint32_t	software_err : 1;
		/* [한국어] 비트 0 — SW error(잘못된 디스크립터 등). SWERR 레지스터에 상세. */
		uint32_t	command_completion : 1;
		/* [한국어] 비트 1 — CMD 레지스터의 명령 완료 통지. */
		uint32_t	wq_occupancy_below_limit : 1;
		/* [한국어] 비트 2 — WQ 점유율이 임계 이하로 떨어짐. */
		uint32_t	perfmon_counter_overflow : 1;
		/* [한국어] 비트 3 — PERFMON counter overflow. */
		uint32_t	halt_state : 1;
		/* [한국어] 비트 4 — 디바이스가 HALT 상태로 진입. */
		uint32_t	reserved : 26;
		/* [한국어] 예약. */
		uint32_t	int_handles_revoked : 1;
		/* [한국어] 비트 31 — interrupt handle 회수됨(IMS 변경 등). */
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_intcause_register) == 4, "size mismatch");

/*
 * [한국어] union idxd_cmd_register - Command 4B 레지스터(드라이버가 CMD 발행).
 * MMIO write로 명령 코드 + operand + 인터럽트 요청 비트를 한 번에 설정.
 *
 * 설정자: 드라이버. 읽는 자: HW. 동기화: 한 번에 1 명령 — CMDSTS.active로 진행 중 확인.
 */
union idxd_cmd_register {
	struct {
		uint32_t	operand : 20;
		/* [한국어] 비트 0-19 — 명령 operand. ENABLE_WQ의 WQ index 등. */
		uint32_t	command_code : 5;
		/* [한국어] 비트 20-24 — idxd_cmds enum 값(1=ENABLE_DEV, 6=ENABLE_WQ 등). */
		uint32_t	reserved : 6;
		/* [한국어] 예약 6비트. */
		uint32_t	request_completion_interrupt : 1;
		/* [한국어] 비트 31 — 명령 완료 시 인터럽트 발행 요청. polling 모델은 0. */
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_cmd_register) == 4, "size mismatch");

/*
 * [한국어] union idxd_cmdsts_register - Command Status 4B 레지스터.
 * 가장 최근 발행된 CMD의 결과/진행 상태.
 *
 * 설정자: HW(쓰기). 읽는 자: 드라이버(폴링).
 */
union idxd_cmdsts_register {
	struct {
		uint32_t err : 8;
		/* [한국어] 비트 0-7 — idxd_cmdsts_err 값(0=성공, 그 외=오류). */
		uint32_t result : 16;
		/* [한국어] 비트 8-23 — 명령 결과 데이터(예: 할당된 interrupt handle 번호). */
		uint32_t rsvd: 7;
		/* [한국어] 예약. */
		uint32_t active: 1;
		/* [한국어] 비트 31 — 1이면 명령 진행 중. 드라이버는 이 비트가 0이 될 때까지 polling. */
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_cmdsts_register) == 4, "size mismatch");

/*
 * [한국어] union idxd_cmdcap_register - Command Capability 4B 레지스터.
 * 디바이스가 지원하는 CMD 종류 비트마스크. 드라이버가 명령 발행 전 지원 여부 확인.
 */
union idxd_cmdcap_register {
	struct {
		uint32_t	reserved0 : 1;
		/* [한국어] 예약. */
		uint32_t	enable_device : 1;
		/* [한국어] ENABLE_DEV 지원. */
		uint32_t	disable_device : 1;
		/* [한국어] DISABLE_DEV 지원. */
		uint32_t	drain_all : 1;
		/* [한국어] DRAIN_ALL 지원. */
		uint32_t	abort_all : 1;
		/* [한국어] ABORT_ALL 지원. */
		uint32_t	reset_device : 1;
		/* [한국어] RESET_DEVICE 지원. */
		uint32_t	enable_wq : 1;
		/* [한국어] ENABLE_WQ 지원. */
		uint32_t	disable_wq : 1;
		/* [한국어] DISABLE_WQ 지원. */
		uint32_t	drain_wq : 1;
		/* [한국어] DRAIN_WQ 지원. */
		uint32_t	abort_wq : 1;
		/* [한국어] ABORT_WQ 지원. */
		uint32_t	reset_wq : 1;
		/* [한국어] RESET_WQ 지원. */
		uint32_t	drain_pasid : 1;
		/* [한국어] DRAIN_PASID 지원(특정 PASID drain). */
		uint32_t	abort_pasid : 1;
		/* [한국어] ABORT_PASID 지원. */
		uint32_t	request_int_handle : 1;
		/* [한국어] interrupt handle 요청 명령 지원. */
		uint32_t	release_int_handle : 1;
		/* [한국어] interrupt handle 해제 명령 지원. */
		uint32_t	reserved1 : 17;
		/* [한국어] 예약. */
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_cmdcap_register) == 4, "size mismatch");

/*
 * [한국어] union idxd_swerr_register - Software Error 32B 레지스터(4 x uint64).
 * SW error 인터럽트 발생 시 어떤 디스크립터/WQ/PASID에서 오류가 났는지 상세 정보 기록.
 *
 * 설정자: HW. 읽는 자: 드라이버 error handler. 동기화: write-1-to-clear (valid 비트).
 */
union idxd_swerr_register {
	struct {
		uint64_t valid: 1;
		/* [한국어] 비트 0 — 1이면 본 레지스터 내용이 유효(이전 SW error 캡처). */
		uint64_t overflow: 1;
		/* [한국어] 비트 1 — valid 클리어 전에 새 SW error 발생(덮어씀). */
		uint64_t desc_valid: 1;
		/* [한국어] 비트 2 — descriptor 정보가 유효. */
		uint64_t wq_idx_valid: 1;
		/* [한국어] 비트 3 — wq_idx 필드 유효. */
		uint64_t batch: 1;
		/* [한국어] 비트 4 — batch 디스크립터 처리 중 오류. */
		uint64_t fault_rw: 1;
		/* [한국어] 비트 5 — page fault read(0)/write(1) 구분. */
		uint64_t priv: 1;
		/* [한국어] 비트 6 — privileged 모드 디스크립터에서 오류. */
		uint64_t rsvd: 1;
		/* [한국어] 예약. */
		uint64_t error: 8;
		/* [한국어] 비트 8-15 — error code(dsa_completion_status 와 동일 카탈로그). */
		uint64_t wq_idx: 8;
		/* [한국어] 비트 16-23 — 오류 발생 WQ index. */
		uint64_t rsvd2: 8;
		/* [한국어] 예약. */
		uint64_t operation: 8;
		/* [한국어] 비트 32-39 — 오류 발생 opcode. */
		uint64_t pasid: 20;
		/* [한국어] 비트 40-59 — 오류 발생 PASID. */
		uint64_t rsvd3: 4;
		/* [한국어] 예약. */
		uint64_t batch_idx: 16;
		/* [한국어] 두 번째 64비트 시작 — batch 안에서 어느 sub-descriptor에서 오류. */
		uint64_t rsvd4: 16;
		/* [한국어] 예약. */
		uint64_t invalid_flags: 32;
		/* [한국어] 잘못된 flags 비트마스크. */
		uint64_t fault_addr;
		/* [한국어] 세 번째 64비트 — page fault address. */
		uint64_t rsvd5;
		/* [한국어] 네 번째 64비트 — 예약. */
	};
	uint64_t raw[4];
	/* [한국어] 32B raw view. */
};
SPDK_STATIC_ASSERT(sizeof(union idxd_swerr_register) == 32, "size mismatch");

/*
 * [한국어] struct idxd_registers - BAR0(MMIO control register) 레이아웃 0x00~0xE0.
 * 드라이버는 BAR0를 매핑한 후 본 구조체로 캐스팅하여 필드 접근(MMIO read/write).
 *
 * 설정자: 드라이버(쓰기 가능 필드만) / HW(read-only 필드).
 * 동기화: control plane은 단일 thread에서 처리. I/O hot path는 BAR0 안 보고 BAR2 portal만 사용.
 * 정렬: 8B 정렬(MMIO load/store 정렬 요구).
 */
struct idxd_registers {
	uint32_t			version;
	/* [한국어] 0x00: 디바이스 버전(읽기 전용). */
	uint32_t			reserved0;
	/* [한국어] 0x04: 예약. */
	uint64_t			reserved1;
	/* [한국어] 0x08: 예약. */
	union idxd_gencap_register	gencap;
	/* [한국어] 0x10: GENCAP 8B(읽기 전용). */
	uint64_t			reserved2;
	/* [한국어] 0x18: 예약. */
	union idxd_wqcap_register	wqcap;
	/* [한국어] 0x20: WQCAP 8B(읽기 전용). */
	uint64_t			reserved3;
	/* [한국어] 0x28: 예약. */
	union idxd_groupcap_register	groupcap;
	/* [한국어] 0x30: GROUPCAP 8B. */
	union idxd_enginecap_register	enginecap;
	/* [한국어] 0x38: ENGINECAP 8B. */
	struct idxd_opcap_register	opcap;
	/* [한국어] 0x40: OPCAP 32B opcode bitmap. */
	union idxd_offsets_register	offsets;
	/* [한국어] 0x60: 보조 테이블 offset 16B. */
	uint64_t			reserved4[2];
	/* [한국어] 0x70: 예약 16B. */
	union idxd_gencfg_register	gencfg;
	/* [한국어] 0x80: GENCFG 4B(쓰기). */
	uint32_t			reserved5;
	/* [한국어] 0x84: 예약. */
	union idxd_genctrl_register	genctrl;
	/* [한국어] 0x88: GENCTRL 4B(쓰기). */
	uint32_t			reserved6;
	/* [한국어] 0x8C: 예약. */
	union idxd_gensts_register	gensts;
	/* [한국어] 0x90: GENSTS 4B(읽기). */
	uint32_t			reserved7;
	/* [한국어] 0x94: 예약. */
	union idxd_intcause_register	intcause;
	/* [한국어] 0x98: INTCAUSE 4B(W1C). */
	uint32_t			reserved8;
	/* [한국어] 0x9C: 예약. */
	union idxd_cmd_register		cmd;
	/* [한국어] 0xA0: CMD 4B(쓰기 — 명령 발행). */
	uint32_t			reserved9;
	/* [한국어] 0xA4: 예약. */
	union idxd_cmdsts_register	cmdsts;
	/* [한국어] 0xA8: CMDSTS 4B(읽기 — 명령 진행 상태/결과). */
	uint32_t			reserved10;
	/* [한국어] 0xAC: 예약. */
	union idxd_cmdcap_register	cmdcap;
	/* [한국어] 0xB0: CMDCAP 4B(읽기). */
	uint32_t			reserved11;
	/* [한국어] 0xB4: 예약. */
	uint64_t			reserved12;
	/* [한국어] 0xB8: 예약. */
	union idxd_swerr_register	sw_err;
	/* [한국어] 0xC0~0xDF: SW Error 정보 32B. */
};
SPDK_STATIC_ASSERT(sizeof(struct idxd_registers) == 0xE0, "size mismatch");
/* [한국어] 224B 검증 — 4+4+8+8+8+8+8+8+32+16+16+4+4+4+4+4+4+4+4+4+4+4+4+4+4+8+32 = 0xE0. */

/*
 * [한국어] union idxd_group_flags - Group Configuration의 flags 4B 필드.
 * Group은 WQ 집합 + Engine 집합을 묶는 단위 — 같은 group 안의 WQ는 group의 engine pool로 처리.
 */
union idxd_group_flags {
	struct {
		uint32_t tc_a : 3;
		/* [한국어] 비트 0-2 — Traffic Class A(PCIe TC). I/O 트래픽 우선순위 제어. */
		uint32_t tc_b : 3;
		/* [한국어] 비트 3-5 — Traffic Class B. */
		uint32_t reserved0 : 1;
		/* [한국어] 예약. */
		uint32_t global_read_buffer_limit: 1;
		/* [한국어] 비트 7 — global read buffer limit 사용. */
		uint32_t read_buffers_reserved : 8;
		/* [한국어] 비트 8-15 — group이 예약하는 read buffer 수. */
		uint32_t reserved1: 4;
		/* [한국어] 예약. */
		uint32_t read_buffers_allowed : 8;
		/* [한국어] 비트 20-27 — group이 사용 가능한 read buffer 한계. */
		uint32_t reserved2 : 4;
		/* [한국어] 예약. */
	};
	uint32_t raw;
	/* [한국어] raw view. */
};
SPDK_STATIC_ASSERT(sizeof(union idxd_group_flags) == 4, "size mismatch");

/*
 * [한국어] struct idxd_grpcfg - Group Configuration 64B(=8 x uint64) entry.
 * GRPCFG 테이블의 한 group entry. 드라이버가 enable 전에 채워서 디바이스에 반영.
 *
 * 설정자: 드라이버. 읽는 자: HW(enable 시 적용).
 * 동기화: enable 전 한 번 설정 — runtime 변경 안 함(또는 disable 후 재설정).
 */
struct idxd_grpcfg {
	uint64_t wqs[4];
	/* [한국어] 256-bit bitmap — 이 group에 속하는 WQ index 비트(WQ 0..255).
	 * 4개 uint64로 분할(bit i = WQ i 가 이 group 소속). */

	uint64_t engines;
	/* [한국어] 64-bit bitmap — 이 group에 속하는 engine index. */

	union idxd_group_flags flags;
	/* [한국어] Group flags(traffic class, read buffer 정책). */

	/* This is not part of the definition, but in practice the stride in the table
	 * is 64 bytes. */
	/* [한국어] 아래 두 필드는 스펙 자체에 정의된 건 아니지만, 실제 GRPCFG 테이블의 stride가 64B 이므로
	 * 패딩으로 둔다. (테이블에 entry를 64B 간격으로 배치하기 위함.) */
	uint32_t reserved0;
	/* [한국어] 패딩 4B. */
	uint64_t reserved1[2];
	/* [한국어] 패딩 16B. 합계 4+8+8+4+8+8+4+16=60... → 64B 정렬 패딩 완성용. */
};
SPDK_STATIC_ASSERT(sizeof(struct idxd_grpcfg) == 64, "size mismatch");

/*
 * [한국어] struct idxd_grptbl - Group Configuration Table.
 * GRPCFG 테이블의 시작점. 실제로는 group 수만큼 idxd_grpcfg가 배열로 이어진다(가변 길이).
 * group[1] 는 C에서 가변 배열 형태(flexible array idiom 보다 단순한 trick).
 *
 * 설정자/읽는 자: 드라이버 — 디바이스 enable 전 GRPCFG 테이블 위치 산출:
 *   GRPCFG 테이블 base = BAR0 + offsets.grpcfg * IDXD_TABLE_OFFSET_MULT.
 */
struct idxd_grptbl {
	struct idxd_grpcfg group[1];
	/* [한국어] group[0] = group 0 의 config, group[1..N-1] = 추가 group(num_groups까지).
	 * 실제 사용 시 [num_groups] 크기로 캐스팅. */
};

/*
 * [한국어] union idxd_wqcfg - WQ Configuration 32B entry(8 x uint32).
 * 각 WQ에 대해 size, mode, priority, PASID, occupancy 한계, 상태 등을 모두 담는 큰 entry.
 * WQCFG 테이블 base = BAR0 + offsets.wqcfg * IDXD_TABLE_OFFSET_MULT, stride = 1<<WQCFG_SHIFT (32B).
 *
 * 설정자: 드라이버(enable 전 설정 + 일부 runtime). 읽는 자: HW.
 * 동기화: WQ enable/disable 사이에 변경. enable 상태에서는 일부 read-only.
 */
union idxd_wqcfg {
	struct {
		uint16_t wq_size;
		/* [한국어] uint32[0] 하위 — WQ가 보유할 디스크립터 슬롯 수. 합계는 wqcap.total_wq_size 이하. */
		uint16_t rsvd;
		/* [한국어] 예약. */
		uint16_t wq_thresh;
		/* [한국어] uint32[1] 하위 — occupancy 임계(SWQ에서 backpressure 트리거). */
		uint16_t rsvd1;
		/* [한국어] 예약. */
		uint32_t mode: 1;
		/* [한국어] uint32[2] 비트 0 — 0=Shared(SWQ), 1=Dedicated(DWQ). 발행 명령(ENQCMDS vs MOVDIR64B) 결정. */
		uint32_t bof: 1;
		/* [한국어] 비트 1 — Block-on-Fault. SVM 사용 시 페이지 폴트 시 wait 여부. */
		uint32_t wq_ats_disable: 1;
		/* [한국어] 비트 2 — PCIe ATS(Address Translation Service) 비활성화. */
		uint32_t rsvd2: 1;
		/* [한국어] 예약. */
		uint32_t priority: 4;
		/* [한국어] 비트 4-7 — WQ priority(0=lowest, 15=highest). */
		uint32_t pasid: 20;
		/* [한국어] 비트 8-27 — DWQ에서 사용할 default PASID. */
		uint32_t pasid_en: 1;
		/* [한국어] 비트 28 — PASID 사용 활성화. */
		uint32_t priv: 1;
		/* [한국어] 비트 29 — privileged 모드 활성화. */
		uint32_t rsvd3: 2;
		/* [한국어] 예약. */
		uint32_t max_xfer_shift: 5;
		/* [한국어] uint32[3] 비트 0-4 — WQ별 max transfer shift(WQ 단위로 디바이스 한계보다 작게 가능). */
		uint32_t max_batch_shift: 4;
		/* [한국어] 비트 5-8 — WQ별 max batch shift. */
		uint32_t rsvd4: 23;
		/* [한국어] 예약. */
		uint16_t occupancy_inth;
		/* [한국어] uint32[4] 하위 — occupancy interrupt handle. */
		uint16_t occupancy_table_sel: 1;
		/* [한국어] 비트 0 — occupancy 테이블 선택. */
		uint16_t rsvd5: 15;
		/* [한국어] 예약. */
		uint16_t occupancy_limit;
		/* [한국어] uint32[5] 하위 — occupancy 한계. */
		uint16_t occupancy_int_en: 1;
		/* [한국어] 비트 0 — occupancy 인터럽트 활성화. */
		uint16_t rsvd6: 15;
		/* [한국어] 예약. */
		uint16_t occupancy;
		/* [한국어] uint32[6] 하위 — 현재 WQ 점유율(읽기 전용, HW가 갱신). */
		uint16_t occupancy_int: 1;
		/* [한국어] 비트 0 — occupancy 인터럽트 발생 비트(write-to-clear). */
		uint16_t rsvd7: 12;
		/* [한국어] 예약. */
		uint16_t mode_support: 1;
		/* [한국어] 비트 13 — 이 WQ가 두 모드(SWQ/DWQ)를 모두 지원하는지(읽기 전용). */
		uint16_t wq_state: 2;
		/* [한국어] 비트 14-15 — idxd_wq_state(0=DISABLED, 1=ENABLED). 읽기 전용. */
		uint32_t rsvd8;
		/* [한국어] uint32[7] — 예약 32비트. */
	};
	uint32_t raw[8];
	/* [한국어] 32B raw view (8 x uint32). MMIO write 단위로 접근 가능. */
};
SPDK_STATIC_ASSERT(sizeof(union idxd_wqcfg) == 32, "size mismatch");
/* [한국어] 32B 검증 — WQCFG_SHIFT(=5)와 일치. */

#ifdef __cplusplus
}
/* [한국어] extern "C" 종료. */
#endif

#endif /* SPDK_IDXD_SPEC_H */
/* [한국어] SPDK_IDXD_SPEC_H 헤더 가드 종료. */
