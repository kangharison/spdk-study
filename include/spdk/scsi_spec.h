/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * SCSI specification definitions
 */

/*
 * [한국어 설명] SCSI 와이어 포맷 정의 헤더 (scsi_spec.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 SCSI 표준(SAM-5/SPC-4/SBC-3/MMC-6/SSC-4) 의 와이어 레벨 상수와 패킷 구조체를 모아둔 "스펙 미러"이다.
 * CDB opcode, status/sense/ASC/ASCQ 코드, INQUIRY/VPD 페이지 레이아웃, Persistent Reservation in/out 파라미터,
 * UNMAP/WRITE_SAME 비트, Transport ID 포맷 등을 SCSI 스펙 그대로 C enum/struct로 표현한다.
 * 본 파일은 어떠한 동작도 하지 않으며, 미들레이어가 "스펙대로" CDB를 디코드/응답을 인코드하는 데 쓰는
 * 단일 진실 공급원(Single Source of Truth)이다.
 * 모든 비트필드/배열 크기는 SCSI 스펙 표(Table)에 1:1 대응되어 있다 — 임의 수정 금지.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK SCSI 미들레이어(lib/scsi)와 트랜스포트(lib/iscsi, lib/vhost)에서 본 헤더를 포함하여 사용한다.
 *   [트랜스포트가 받은 raw CDB 바이트] -> [본 헤더의 enum/struct로 디코드] -> [bdev API 호출] ->
 *   [완료 후 본 헤더의 응답 구조체로 인코드] -> [트랜스포트가 PDU로 송신]
 * 호출 체인이라기보다 데이터 형식 정의이며, 컴파일 타임에만 의미를 갖는다(런타임 코드 0줄).
 * 백엔드가 NVMe인 경우 lib/bdev/scsi_nvme.c가 NVMe completion -> SCSI sense 변환 시 본 헤더의 sense_key/asc/ascq
 * 상수를 참조한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존(아래로): spdk/stdinc.h (uint8_t/uint16_t 등), spdk/assert.h (SPDK_STATIC_ASSERT — 구조체 사이즈 컴파일 타임 검증).
 * - 의존(위로 - 본 헤더를 사용하는 측):
 *   * lib/scsi/scsi_bdev.c - CDB 디코드 메인 로직 (모든 SBC opcode enum 사용)
 *   * lib/scsi/lun.c - LUN 단위 명령 라우팅
 *   * lib/scsi/task.c - sense data 빌드 (sense key/asc/ascq enum)
 *   * lib/scsi/port.c - INQUIRY VPD 페이지 인코드 (vpd struct, designator desc)
 *   * lib/scsi/scsi_pr.c - Persistent Reservation in/out 파싱 (pr_in/out 구조체)
 *   * lib/iscsi/iscsi.c - SCSI status 코드 변환
 *   * lib/bdev/scsi_nvme.c - NVMe SC/SCT -> SCSI (sk, asc, ascq) 매핑
 * 데이터 흐름: 본 헤더는 형식만 제공 — 실제 데이터 흐름은 위 모듈에서 발생.
 *
 * === 주요 함수/구조체 요약 ===
 * 본 헤더는 함수가 없고 모두 enum/struct/매크로다. 핵심 그룹:
 * - opcode enum: spdk_spc_opcode (SPC-4: INQUIRY, REPORT_LUNS, PR_IN/OUT, MODE_SENSE 등),
 *                spdk_sbc_opcode (SBC-3: READ_*/WRITE_*/UNMAP/WRITE_SAME/COMPARE_AND_WRITE/SYNC_CACHE 등),
 *                spdk_mmc_opcode (광매체), spdk_ssc_opcode (테이프).
 * - 상태/오류 enum: spdk_scsi_status (GOOD/CHECK_COND/BUSY/RES_CONFLICT/TASK_SET_FULL),
 *                  spdk_scsi_sense (10여 개 sense key), spdk_scsi_asc/ascq (sense 보조 코드).
 * - VPD enum: spdk_spc_vpd (페이지 0x00/0x80/0x83/0x86/0xB0/0xB1/0xB2),
 *             SPDK_SPC_VPD_IDENTIFIER_TYPE_* (NAA/EUI64/SCSI Name 등 designator type).
 * - 구조체: spdk_scsi_cdb_inquiry/inquiry_data, spdk_scsi_vpd_page/desig_desc/tgt_port_desc/port_desc,
 *           spdk_scsi_iscsi_transport_id, spdk_scsi_unmap_bdesc,
 *           PR 패밀리(pr_in_read_*, pr_in_full_status_desc, pr_out_param_list, reg_and_move_param_list).
 * - PR enum: spdk_scsi_pr_in_action_code, spdk_scsi_pr_scope_code, spdk_scsi_pr_type_code,
 *            spdk_scsi_pr_out_service_action_code (6+8 = 총 14 코드).
 */

#ifndef SPDK_SCSI_SPEC_H
#define SPDK_SCSI_SPEC_H

/* [한국어] uint8_t/uint16_t/uint32_t/uint64_t 등 고정 폭 정수 타입을 도입.
 *         와이어 포맷을 정확히 정의하려면 정수 폭이 명확해야 하므로 필수. */
#include "spdk/stdinc.h"

/* [한국어] SPDK_STATIC_ASSERT 매크로 도입.
 *         CDB/응답 구조체의 sizeof를 컴파일 타임에 스펙 표(Table)와 비교 검증하기 위해 필요.
 *         예: spdk_scsi_cdb_inquiry는 정확히 6바이트여야 함. */
#include "spdk/assert.h"

#ifdef __cplusplus
/* [한국어] C++ 컴파일러에서 본 헤더 포함 시 C 링키지 강제 (name mangling 방지). */
extern "C" {
#endif

/*
 * [한국어]
 * spdk_scsi_group_code - CDB opcode 상위 3비트로 표현되는 명령 그룹(길이) 코드.
 *
 * SPC-4 §4.3.5 Operation code의 group code 필드(상위 3비트)는 CDB 길이를 결정한다:
 *   group 0(0x00) -> 6 byte CDB, group 1(0x20)/2(0x40) -> 10 byte, group 4(0x80) -> 16 byte, group 5(0xa0) -> 12 byte.
 * 미들레이어가 raw 바이트를 받아 (cdb[0] & 0xe0) 으로 그룹을 판별하고 (cdb[0] & 0x1f) 로 opcode를 얻는다.
 */
enum spdk_scsi_group_code {
	SPDK_SCSI_6BYTE_CMD = 0x00,
	/* [한국어] 6-byte CDB 그룹 (예: TEST UNIT READY 0x00, READ_6 0x08, WRITE_6 0x0a, INQUIRY 0x12).
	 *         가장 오래된 형식이며 LBA가 21비트로 제한되어 신규 명령에서는 잘 사용되지 않음. */
	SPDK_SCSI_10BYTE_CMD = 0x20,
	/* [한국어] 10-byte CDB 그룹 (예: READ_10 0x28, WRITE_10 0x2a, READ_CAPACITY_10 0x25, MODE_SENSE_10 0x5a).
	 *         가장 흔한 일반 I/O CDB 길이. LBA 32비트, transfer len 16비트. */
	SPDK_SCSI_10BYTE_CMD2 = 0x40,
	/* [한국어] 또 다른 10-byte 그룹 (예: WRITE_SAME 0x41, UNMAP 0x42, PERSISTENT_RESERVE_IN/OUT 0x5e/0x5f).
	 *         비교적 새로운 10-byte 명령들이 모여 있음. */
	SPDK_SCSI_16BYTE_CMD = 0x80,
	/* [한국어] 16-byte CDB 그룹 (예: READ_16 0x88, WRITE_16 0x8a, COMPARE_AND_WRITE 0x89,
	 *         WRITE_SAME_16 0x93, SAI_READ_CAPACITY_16 service action 0x9e).
	 *         LBA 64비트 — 대용량 SSD 표준 경로. */
	SPDK_SCSI_12BYTE_CMD = 0xa0,
	/* [한국어] 12-byte CDB 그룹 (예: READ_12 0xa8, WRITE_12 0xaa, REPORT_LUNS 0xa0).
	 *         LBA 32비트, transfer len 32비트로 16-byte 대비 짧음. */
};

/* [한국어] CDB[0]의 상위 3비트를 추출하는 마스크 (그룹 코드 분리용).
 *         예: cdb[0] & 0xe0 == SPDK_SCSI_16BYTE_CMD 이면 16-byte 명령. */
#define SPDK_SCSI_GROUP_MASK	0xe0
/* [한국어] CDB[0]의 하위 5비트(opcode 본체) 마스크.
 *         그룹 코드 분리 후 같은 그룹 내에서 명령을 구분하는 용도. */
#define SPDK_SCSI_OPCODE_MASK	0x1f

/*
 * [한국어]
 * spdk_scsi_status - SCSI Status 코드 (SAM-5 Table-58).
 *
 * 1바이트 status 필드의 정의이며, iSCSI Response PDU의 Status 필드 또는 vhost-scsi descriptor의 status에 그대로 들어간다.
 * 미들레이어는 spdk_scsi_task_set_status() 로 본 enum 값을 task->status에 저장하고, 트랜스포트가 PDU 인코딩 시 사용.
 */
enum spdk_scsi_status {
	SPDK_SCSI_STATUS_GOOD = 0x00,
	/* [한국어] 정상 완료. 가장 흔한 케이스. sense data 없음. */
	SPDK_SCSI_STATUS_CHECK_CONDITION = 0x02,
	/* [한국어] 오류 발생 — sense data로 (sense_key, ASC, ASCQ) 를 함께 보고해야 함.
	 *         디코드 오류, 매체 오류, hot-remove, UNIT ATTENTION 등 모든 에러의 표준 운반 수단. */
	SPDK_SCSI_STATUS_CONDITION_MET = 0x04,
	/* [한국어] PRE-FETCH 등 특수 명령에서 조건이 충족됨. SBC에서 특정 명령에만 사용. */
	SPDK_SCSI_STATUS_BUSY = 0x08,
	/* [한국어] 지금은 처리 불가, 잠시 후 재시도 권고. 큐 일시 포화 등에서 반환. */
	SPDK_SCSI_STATUS_INTERMEDIATE = 0x10,
	/* [한국어] Linked command 시퀀스의 중간 단계 GOOD (deprecated, SAM-5에서 obsolete). */
	SPDK_SCSI_STATUS_INTERMEDIATE_CONDITION_MET = 0x14,
	/* [한국어] Linked command 중간 단계 + condition met (deprecated). */
	SPDK_SCSI_STATUS_RESERVATION_CONFLICT = 0x18,
	/* [한국어] Persistent Reservation에 의해 접근이 차단됨.
	 *         예: Write Exclusive 보유 중인 LUN에 다른 initiator가 WRITE 발행 시. */
	SPDK_SCSI_STATUS_Obsolete = 0x22,
	/* [한국어] 0x22 — SAM-5에서 obsolete된 코드 (구 COMMAND TERMINATED). 호환성을 위해 보존. */
	SPDK_SCSI_STATUS_TASK_SET_FULL = 0x28,
	/* [한국어] 큐가 가득 차서 task를 더 받을 수 없음. NVMe로 치면 SQ overflow와 유사. */
	SPDK_SCSI_STATUS_ACA_ACTIVE = 0x30,
	/* [한국어] Auto Contingent Allegiance 활성 — ACA가 풀릴 때까지 일반 명령 거부. */
	SPDK_SCSI_STATUS_TASK_ABORTED = 0x40,
	/* [한국어] task가 abort됨 (ABORT_TASK / LUN_RESET 등으로 인해).
	 *         spdk_scsi_task_process_abort() 가 본 status를 설정. */
};

/*
 * [한국어]
 * spdk_scsi_sense - SCSI Sense Key (SPC-4 Table-30).
 *
 * CHECK CONDITION status가 반환될 때 sense data의 4-bit field에 채워지는 분류 코드.
 * (sense_key, ASC, ASCQ) 3종이 묶여 호스트에 정확한 오류 원인을 전달한다.
 * 스펙상 0x00~0x0f 범위의 4비트 값으로, 본 enum은 그 중 사용되는 14개를 정의한다.
 */
enum spdk_scsi_sense {
	SPDK_SCSI_SENSE_NO_SENSE = 0x00,
	/* [한국어] 오류 정보 없음 (REQUEST SENSE에 응답하나 보고할 사항이 없을 때). */
	SPDK_SCSI_SENSE_RECOVERED_ERROR = 0x01,
	/* [한국어] 명령은 완료되었으나 내부적으로 회복 절차(retry/ECC)가 있었음. 통계 보고용. */
	SPDK_SCSI_SENSE_NOT_READY = 0x02,
	/* [한국어] 매체/디바이스가 아직 준비되지 않음. 흔한 ASC 조합: 0x04/0x01 (LUN becoming ready). */
	SPDK_SCSI_SENSE_MEDIUM_ERROR = 0x03,
	/* [한국어] 매체 자체 오류 (read error 등). NVMe Media Error 매핑. */
	SPDK_SCSI_SENSE_HARDWARE_ERROR = 0x04,
	/* [한국어] 디바이스 하드웨어 오류 (controller failure 등). */
	SPDK_SCSI_SENSE_ILLEGAL_REQUEST = 0x05,
	/* [한국어] CDB 자체가 잘못됨 — 디코드 단계에서 가장 자주 등장. ASC: 0x20/0x24/0x21 등. */
	SPDK_SCSI_SENSE_UNIT_ATTENTION = 0x06,
	/* [한국어] 디바이스 상태 변화 알림 (power-on, reset, capacity change, mode parameter change).
	 *         초도 INQUIRY 후 한 번만 보고되는 sticky 조건. */
	SPDK_SCSI_SENSE_DATA_PROTECT = 0x07,
	/* [한국어] 쓰기 보호 또는 PR로 인해 차단됨. ASC 0x27 (WRITE PROTECTED) 등. */
	SPDK_SCSI_SENSE_BLANK_CHECK = 0x08,
	/* [한국어] (테이프) blank 영역 도달. SBC에서는 거의 사용 안 됨. */
	SPDK_SCSI_SENSE_VENDOR_SPECIFIC = 0x09,
	/* [한국어] 벤더 전용 의미. SPDK는 사용하지 않음. */
	SPDK_SCSI_SENSE_COPY_ABORTED = 0x0a,
	/* [한국어] EXTENDED COPY 명령이 도중에 중단됨. */
	SPDK_SCSI_SENSE_ABORTED_COMMAND = 0x0b,
	/* [한국어] 내부 사정으로 명령이 강제 종료됨. retry 가능. */
	SPDK_SCSI_SENSE_VOLUME_OVERFLOW = 0x0d,
	/* [한국어] (테이프) 볼륨 끝 초과. SBC에서는 미사용. */
	SPDK_SCSI_SENSE_MISCOMPARE = 0x0e,
	/* [한국어] VERIFY/COMPARE_AND_WRITE의 비교 실패. ASC 0x1d. */
};

/*
 * [한국어]
 * spdk_scsi_asc - Additional Sense Code (SPC-4 Annex D, 1바이트).
 *
 * sense key와 함께 정확한 오류 원인을 식별. 동일한 0x10 등이 여러 의미로 매핑되는 것은
 * ASCQ(qualifier)로 더 세분화되기 때문이다 (예: 0x10/0x01=guard, 0x10/0x02=app tag, 0x10/0x03=ref tag).
 * 본 enum은 SPDK가 실제로 보고하는 ASC 값들만 발췌.
 */
enum spdk_scsi_asc {
	SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE = 0x00,
	/* [한국어] 추가 정보 없음. NO_SENSE/RECOVERED_ERROR와 짝. */
	SPDK_SCSI_ASC_PERIPHERAL_DEVICE_WRITE_FAULT = 0x03,
	/* [한국어] write 도중 디바이스 fault. HARDWARE_ERROR sense key와 짝. */
	SPDK_SCSI_ASC_LOGICAL_UNIT_NOT_READY = 0x04,
	/* [한국어] LUN 준비 안 됨. NOT_READY sense key와 짝, ASCQ로 단계 구분(0x01 becoming, 0x02 manual intervention). */
	SPDK_SCSI_ASC_WARNING = 0x0b,
	/* [한국어] 일반 경고 (S.M.A.R.T threshold 등). */
	SPDK_SCSI_ASC_LOGICAL_BLOCK_GUARD_CHECK_FAILED = 0x10,
	/* [한국어] T10 PI Guard(CRC) 검증 실패. ASCQ 0x01. */
	SPDK_SCSI_ASC_LOGICAL_BLOCK_APP_TAG_CHECK_FAILED = 0x10,
	/* [한국어] T10 PI Application Tag 검증 실패. 같은 ASC 0x10 + ASCQ 0x02. */
	SPDK_SCSI_ASC_LOGICAL_BLOCK_REF_TAG_CHECK_FAILED = 0x10,
	/* [한국어] T10 PI Reference Tag 검증 실패. ASC 0x10 + ASCQ 0x03. */
	SPDK_SCSI_ASC_UNRECOVERED_READ_ERROR = 0x11,
	/* [한국어] 회복 불가 read 오류. MEDIUM_ERROR + 0x11. NVMe "Unrecovered Read Error" 매핑. */
	SPDK_SCSI_ASC_MISCOMPARE_DURING_VERIFY_OPERATION = 0x1d,
	/* [한국어] VERIFY/CAW miscompare. MISCOMPARE sense key + 0x1d. */
	SPDK_SCSI_ASC_INVALID_COMMAND_OPERATION_CODE = 0x20,
	/* [한국어] CDB[0]의 opcode가 미지원. ILLEGAL_REQUEST + 0x20. 가장 흔한 디코드 실패. */
	SPDK_SCSI_ASC_ACCESS_DENIED = 0x20,
	/* [한국어] 동일 ASC 0x20을 ACCESS DENIED 의미로도 사용. ASCQ로 구분. */
	SPDK_SCSI_ASC_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE = 0x21,
	/* [한국어] LBA 또는 LBA+length가 capacity를 초과. ILLEGAL_REQUEST + 0x21. */
	SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB = 0x24,
	/* [한국어] CDB 내부 필드가 잘못됨 (예약 비트 사용, 길이 0, 잘못된 service action 등). 매우 hot. */
	SPDK_SCSI_ASC_LOGICAL_UNIT_NOT_SUPPORTED = 0x25,
	/* [한국어] LUN이 존재하지 않음. process_null_lun 경로의 응답. */
	SPDK_SCSI_ASC_WRITE_PROTECTED = 0x27,
	/* [한국어] 쓰기 보호된 매체. DATA_PROTECT + 0x27. */
	SPDK_SCSI_ASC_CAPACITY_DATA_HAS_CHANGED = 0x2a,
	/* [한국어] 용량 변화 알림 (lvol resize 등). UNIT_ATTENTION + 0x2a + ASCQ 0x09. */
	SPDK_SCSI_ASC_FORMAT_COMMAND_FAILED = 0x31,
	/* [한국어] FORMAT UNIT 실패. */
	SPDK_SCSI_ASC_SAVING_PARAMETERS_NOT_SUPPORTED = 0x39,
	/* [한국어] MODE_SELECT의 SP(save) 비트 불가. ILLEGAL_REQUEST + 0x39. */
	SPDK_SCSI_ASC_INTERNAL_TARGET_FAILURE = 0x44,
	/* [한국어] target 내부 오류. HARDWARE_ERROR + 0x44. NVMe Internal Error 매핑. */
};

/*
 * [한국어]
 * spdk_scsi_ascq - Additional Sense Code Qualifier (1바이트).
 *
 * ASC에 종속된 보조 식별자. 같은 ASC 값에서도 ASCQ에 따라 다른 의미를 가지므로
 * 본 enum의 동일 정수값이 여러 이름으로 정의되는 경우가 있다 (위 ASC와 동일한 패턴).
 * 미들레이어는 build_sense_data 시 (sk, asc, ascq) 트리플로 모두 채운다.
 */
enum spdk_scsi_ascq {
	SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE = 0x00,
	/* [한국어] 원인 불명/추가 정보 없음. */
	SPDK_SCSI_ASCQ_BECOMING_READY = 0x01,
	/* [한국어] 디바이스가 ready로 전환 중. NOT_READY/0x04 와 짝. */
	SPDK_SCSI_ASCQ_FORMAT_COMMAND_FAILED = 0x01,
	/* [한국어] FORMAT 실패의 보조 코드 (ASC 0x31과 짝). */
	SPDK_SCSI_ASCQ_LOGICAL_BLOCK_GUARD_CHECK_FAILED = 0x01,
	/* [한국어] PI Guard 실패 보조 (ASC 0x10). */
	SPDK_SCSI_ASCQ_LOGICAL_BLOCK_APP_TAG_CHECK_FAILED = 0x02,
	/* [한국어] PI App Tag 실패 보조 (ASC 0x10). */
	SPDK_SCSI_ASCQ_NO_ACCESS_RIGHTS = 0x02,
	/* [한국어] 접근 권한 없음 (ASC 0x20 ACCESS_DENIED). */
	SPDK_SCSI_ASCQ_LOGICAL_BLOCK_REF_TAG_CHECK_FAILED = 0x03,
	/* [한국어] PI Ref Tag 실패 보조 (ASC 0x10). */
	SPDK_SCSI_ASCQ_POWER_LOSS_EXPECTED = 0x08,
	/* [한국어] 전원 손실 임박 경고. */
	SPDK_SCSI_ASCQ_INVALID_LU_IDENTIFIER = 0x09,
	/* [한국어] 잘못된 LU 식별자. */
	SPDK_SCSI_ASCQ_CAPACITY_DATA_HAS_CHANGED = 0x09,
	/* [한국어] 용량 변경 알림 보조 (ASC 0x2a UNIT_ATTENTION). */
};

/*
 * [한국어]
 * spdk_spc_opcode - SPC(Primary Commands) 계열 CDB opcode (SPC-4 §6).
 *
 * 디바이스 종류와 무관한 공통 명령들 — INQUIRY, REPORT_LUNS, MODE_SENSE/SELECT, PERSISTENT_RESERVE 등.
 * 모든 LUN(disk/tape/dvd 모두)이 이 명령을 인식해야 한다. 미들레이어는 (cdb[0] == SPDK_SPC_*) 분기로 라우팅.
 */
enum spdk_spc_opcode {
	/* SPC3 related */
	SPDK_SPC_ACCESS_CONTROL_IN = 0x86,
	/* [한국어] Access Control 정보 조회 (SCSI ACL). SPDK 미사용. */
	SPDK_SPC_ACCESS_CONTROL_OUT = 0x87,
	/* [한국어] Access Control 정보 설정. SPDK 미사용. */
	SPDK_SPC_EXTENDED_COPY = 0x83,
	/* [한국어] 두 LUN 사이의 데이터 복사(SCSI XCOPY). SPDK 미구현. */
	SPDK_SPC_INQUIRY = 0x12,
	/* [한국어] 디바이스 정보(vendor/product/version/VPD) 조회. EVPD=0이면 standard inquiry,
	 *         EVPD=1이면 page_code 별 VPD 데이터. hot path (드라이버 init 시 발행). */
	SPDK_SPC_LOG_SELECT = 0x4c,
	/* [한국어] log page 파라미터 설정. SPDK는 일부만 지원. */
	SPDK_SPC_LOG_SENSE = 0x4d,
	/* [한국어] log page 조회 (Informational Exceptions, Read/Write Error counter 등). */
	SPDK_SPC_MODE_SELECT_6 = 0x15,
	/* [한국어] 6-byte mode parameter 설정. */
	SPDK_SPC_MODE_SELECT_10 = 0x55,
	/* [한국어] 10-byte mode parameter 설정. */
	SPDK_SPC_MODE_SENSE_6 = 0x1a,
	/* [한국어] 6-byte mode page 조회 (Caching, Control, Read-Write Error Recovery 등).
	 *         초기화 시 OS가 자주 호출 — hot path. */
	SPDK_SPC_MODE_SENSE_10 = 0x5a,
	/* [한국어] 10-byte mode page 조회. 더 큰 응답을 받을 때 사용. */
	SPDK_SPC_PERSISTENT_RESERVE_IN = 0x5e,
	/* [한국어] PR 정보 조회 (READ_KEYS / READ_RESERVATION / REPORT_CAPABILITIES / READ_FULL_STATUS).
	 *         service action으로 4개 분기. lib/scsi/scsi_pr.c 참조. */
	SPDK_SPC_PERSISTENT_RESERVE_OUT = 0x5f,
	/* [한국어] PR 등록/예약/해제/선점 (REGISTER, RESERVE, RELEASE, CLEAR, PREEMPT, REG_AND_MOVE 등 7개 SA).
	 *         클러스터 환경에서 fencing/failover의 핵심. */
	SPDK_SPC_PREVENT_ALLOW_MEDIUM_REMOVAL = 0x1e,
	/* [한국어] 매체 제거 잠금. 광/테이프 외 SBC에서는 거의 미사용. */
	SPDK_SPC_READ_ATTRIBUTE = 0x8c,
	/* [한국어] 디바이스 attribute 조회. */
	SPDK_SPC_READ_BUFFER = 0x3c,
	/* [한국어] 진단/펌웨어 다운로드용 버퍼 read. */
	SPDK_SPC_RECEIVE_COPY_RESULTS = 0x84,
	/* [한국어] EXTENDED_COPY 결과 수신. */
	SPDK_SPC_RECEIVE_DIAGNOSTIC_RESULTS = 0x1c,
	/* [한국어] 진단 결과 수신. */
	SPDK_SPC_REPORT_LUNS = 0xa0,
	/* [한국어] target에 존재하는 LUN 목록 조회. 항상 LUN0에서 응답해야 함 (process_null_lun도 처리).
	 *         OS 부팅 시 가장 먼저 발행되는 명령 중 하나 — hot path. */
	SPDK_SPC_REQUEST_SENSE = 0x03,
	/* [한국어] 가장 최근 sense data 조회 (autosense 시 거의 미사용). */
	SPDK_SPC_SEND_DIAGNOSTIC = 0x1d,
	/* [한국어] 디바이스 self-test 트리거. */
	SPDK_SPC_TEST_UNIT_READY = 0x00,
	/* [한국어] LUN ready 여부 polling. 데이터 없음 (전송 길이 0). 매우 hot 한 헬스체크. */
	SPDK_SPC_WRITE_ATTRIBUTE = 0x8d,
	/* [한국어] attribute 쓰기. */
	SPDK_SPC_WRITE_BUFFER = 0x3b,
	/* [한국어] 펌웨어 다운로드/진단용 버퍼 write. */

	SPDK_SPC_SERVICE_ACTION_IN_12 = 0xab,
	/* [한국어] 12-byte CDB의 service action 디스패치 (READ_MEDIA_SERIAL_NUMBER 등). */
	SPDK_SPC_SERVICE_ACTION_OUT_12 = 0xa9,
	/* [한국어] 12-byte service action OUT. */
	SPDK_SPC_SERVICE_ACTION_IN_16 = 0x9e,
	/* [한국어] 16-byte service action IN. SAI_READ_CAPACITY_16(0x10), GET_LBA_STATUS(0x12) 등이 여기로 들어옴.
	 *         (cdb[1] & 0x1f)으로 service action 분기. */
	SPDK_SPC_SERVICE_ACTION_OUT_16 = 0x9f,
	/* [한국어] 16-byte service action OUT. WRITE_LONG_16(SAO 0x11) 등. */

	SPDK_SPC_VARIABLE_LENGTH = 0x7f,
	/* [한국해] 32-byte 가변 CDB (READ_32/WRITE_32 등). T10 PI 강화 명령에 사용. */

	SPDK_SPC_MO_CHANGE_ALIASES = 0x0b,
	/* [한국어] MAINTENANCE_OUT의 service action (0xa4 + 0x0b). alias 변경. */
	SPDK_SPC_MO_SET_DEVICE_IDENTIFIER = 0x06,
	/* [한국어] MAINTENANCE_OUT — device identifier 설정. */
	SPDK_SPC_MO_SET_PRIORITY = 0x0e,
	/* [한국어] MAINTENANCE_OUT — task priority 설정. */
	SPDK_SPC_MO_SET_TARGET_PORT_GROUPS = 0x0a,
	/* [한국어] MAINTENANCE_OUT — TPG (ALUA) 설정. multipathing 정책 변경. */
	SPDK_SPC_MO_SET_TIMESTAMP = 0x0f,
	/* [한국어] MAINTENANCE_OUT — 디바이스 타임스탬프 설정. */
	SPDK_SPC_MI_REPORT_ALIASES = 0x0b,
	/* [한국어] MAINTENANCE_IN — alias 보고. */
	SPDK_SPC_MI_REPORT_DEVICE_IDENTIFIER = 0x05,
	/* [한국어] MAINTENANCE_IN — device identifier 보고. */
	SPDK_SPC_MI_REPORT_PRIORITY = 0x0e,
	/* [한국어] MAINTENANCE_IN — priority 보고. */
	SPDK_SPC_MI_REPORT_SUPPORTED_OPERATION_CODES = 0x0c,
	/* [한국어] MAINTENANCE_IN — 지원 opcode 목록 보고. SPC-4 §6.30. */
	SPDK_SPC_MI_REPORT_SUPPORTED_TASK_MANAGEMENT_FUNCTIONS = 0x0d,
	/* [한국어] MAINTENANCE_IN — 지원 TMF 목록 보고. */
	SPDK_SPC_MI_REPORT_TARGET_PORT_GROUPS = 0x0a,
	/* [한국어] MAINTENANCE_IN — TPG 보고 (RTPG, ALUA multipath용). */
	SPDK_SPC_MI_REPORT_TIMESTAMP = 0x0f,
	/* [한국어] MAINTENANCE_IN — 타임스탬프 보고. */

	/* SPC2 related (Obsolete) */
	SPDK_SPC2_RELEASE_6 = 0x17,
	/* [한국어] SPC-2 RESERVE/RELEASE는 SPC-3 PR로 대체되었으나, 호환을 위해 보존.
	 *         CRH(Compatible Reservation Handing) 비트가 켜진 PR에서 변환 처리. */
	SPDK_SPC2_RELEASE_10 = 0x57,
	/* [한국어] 10-byte RELEASE (obsolete). */
	SPDK_SPC2_RESERVE_6 = 0x16,
	/* [한국어] SPC-2 RESERVE 6-byte (obsolete). */
	SPDK_SPC2_RESERVE_10 = 0x56,
	/* [한국어] SPC-2 RESERVE 10-byte (obsolete). */
};

/*
 * [한국어]
 * spdk_scc_opcode - SCC(SCSI Controller Commands) opcode.
 *
 * RAID/array controller 명령 묶음. SPDK는 SCC 자체는 거의 사용하지 않으나, MAINTENANCE IN/OUT은
 * ALUA(SPC) 처리를 위해 본 opcode 라우터로 들어온다.
 */
enum spdk_scc_opcode {
	SPDK_SCC_MAINTENANCE_IN = 0xa3,
	/* [한국어] MAINTENANCE_IN. cdb[1] service action에 따라 SPC_MI_REPORT_* 로 디스패치.
	 *         RTPG(0x0a)가 가장 자주 호출됨 — 멀티패스 드라이버가 active path 결정 시 사용. */
	SPDK_SCC_MAINTENANCE_OUT = 0xa4,
	/* [한국어] MAINTENANCE_OUT. SPC_MO_SET_TARGET_PORT_GROUPS(0x0a) 등 ALUA 상태 변경에 사용. */
};

/*
 * [한국어]
 * spdk_sbc_opcode - SBC(Block Commands) 계열 CDB opcode (SBC-3 §5).
 *
 * 블록 디바이스 (디스크/SSD) 전용 명령. SPDK 미들레이어의 hot path 대부분이 본 enum 값에 매핑된다.
 * NVMe 매핑:
 *   READ_*   <-> NVMe Read (0x02)
 *   WRITE_*  <-> NVMe Write (0x01)
 *   UNMAP    <-> NVMe Dataset Management (0x09, deallocate)
 *   WRITE_SAME(unmap=1) <-> NVMe Write Zeroes (0x08) 또는 Dataset Management
 *   COMPARE_AND_WRITE <-> NVMe Compare + Write (atomic은 미보장)
 *   SYNCHRONIZE_CACHE_* <-> NVMe Flush (0x00)
 *   READ_CAPACITY_*    <-> NVMe Identify Namespace 의 NSZE 필드
 */
enum spdk_sbc_opcode {
	SPDK_SBC_COMPARE_AND_WRITE = 0x89,
	/* [한국어] CAW (atomic compare-and-write). buffer = compare(N블록) + write(N블록), 길이 2N블록.
	 *         VMware VMFS의 atomic test-and-set에 사용. lib/scsi가 read+compare+write 시퀀스로 에뮬레이션. */
	SPDK_SBC_FORMAT_UNIT = 0x04,
	/* [한국어] FORMAT UNIT. SSD에서는 보통 미지원 또는 noop. */
	SPDK_SBC_GET_LBA_STATUS = 0x0012009e,
	/* [한국어] LBA의 mapped/deallocated 상태 조회. 32-bit 인코딩: (0x9e 16-byte SAI)<<16 | (0x12 SA).
	 *         lib/scsi의 디스패처가 본 합성 상수로 분기 — 일반 enum 값과 다른 패턴이므로 주의. */
	SPDK_SBC_ORWRITE_16 = 0x8b,
	/* [한국어] OR WRITE — 기존 데이터와 OR 연산 후 쓰기. SPDK 미구현. */
	SPDK_SBC_PRE_FETCH_10 = 0x34,
	/* [한국어] LBA 범위를 캐시에 미리 적재. SSD에서는 효과 미미. */
	SPDK_SBC_PRE_FETCH_16 = 0x90,
	/* [한국어] 16-byte PRE_FETCH (64-bit LBA). */
	SPDK_SBC_READ_6 = 0x08,
	/* [한국어] READ_6: 21-bit LBA, 8-bit 블록수. legacy 용. */
	SPDK_SBC_READ_10 = 0x28,
	/* [한국어] READ_10: 32-bit LBA, 16-bit 블록수. 가장 흔한 read CDB. NVMe Read 매핑. */
	SPDK_SBC_READ_12 = 0xa8,
	/* [한국어] READ_12: 32-bit LBA, 32-bit 블록수. 큰 transfer 시 사용. */
	SPDK_SBC_READ_16 = 0x88,
	/* [한국어] READ_16: 64-bit LBA, 32-bit 블록수. 2TB 초과 디스크 표준. NVMe Read 매핑. */
	SPDK_SBC_READ_ATTRIBUTE = 0x8c,
	/* [한국어] medium attribute 읽기 (SSC 위주). */
	SPDK_SBC_READ_BUFFER = 0x3c,
	/* [한국어] 진단/펌웨어 영역 read. */
	SPDK_SBC_READ_CAPACITY_10 = 0x25,
	/* [한국어] LUN 용량(LBA 마지막, block size) 조회 — 32-bit LBA. 8B 응답.
	 *         OS 파티션 인식 시 발행되는 매우 hot한 명령. */
	SPDK_SBC_READ_DEFECT_DATA_10 = 0x37,
	/* [한국어] 결함 블록 목록 조회 (HDD 용). SSD에서는 빈 응답. */
	SPDK_SBC_READ_DEFECT_DATA_12 = 0xb7,
	/* [한국어] 12-byte READ_DEFECT_DATA. */
	SPDK_SBC_READ_LONG_10 = 0x3e,
	/* [한국어] 데이터 + ECC 영역까지 read (HDD 진단용). */
	SPDK_SBC_REASSIGN_BLOCKS = 0x07,
	/* [한국어] bad block 재할당. SSD 자체 wear leveling이 처리하므로 미사용. */
	SPDK_SBC_SANITIZE = 0x48,
	/* [한국어] crypto erase / overwrite / block erase. NVMe Sanitize에 매핑. */
	SPDK_SBC_START_STOP_UNIT = 0x1b,
	/* [한국어] LUN start/stop/eject. Start bit(1<<0)로 spin up. */
	SPDK_SBC_SYNCHRONIZE_CACHE_10 = 0x35,
	/* [한국어] write cache flush. NVMe Flush 매핑. fsync()/fdatasync()가 발행. hot path. */
	SPDK_SBC_SYNCHRONIZE_CACHE_16 = 0x91,
	/* [한국어] 16-byte SYNC_CACHE (64-bit LBA). */
	SPDK_SBC_UNMAP = 0x42,
	/* [한국어] thin provisioning deallocate. NVMe Dataset Management 매핑.
	 *         VPD 0xB2 (Logical Block Provisioning)의 LBPU 비트 = 1일 때만 지원 보고. */
	SPDK_SBC_VERIFY_10 = 0x2f,
	/* [한국어] read 후 비교만(데이터 전송 안 함) 또는 호스트 데이터와 비교 (BYTCHK 비트 의존). */
	SPDK_SBC_VERIFY_12 = 0xaf,
	/* [한국어] 12-byte VERIFY. */
	SPDK_SBC_VERIFY_16 = 0x8f,
	/* [한국어] 16-byte VERIFY. */
	SPDK_SBC_WRITE_6 = 0x0a,
	/* [한국어] WRITE_6: 21-bit LBA. legacy. */
	SPDK_SBC_WRITE_10 = 0x2a,
	/* [한국어] WRITE_10: 32-bit LBA. 가장 흔한 write. NVMe Write 매핑. hot path. */
	SPDK_SBC_WRITE_12 = 0xaa,
	/* [한국어] WRITE_12: 32-bit transfer length. */
	SPDK_SBC_WRITE_16 = 0x8a,
	/* [한국어] WRITE_16: 64-bit LBA. 2TB+ 표준. */
	SPDK_SBC_WRITE_AND_VERIFY_10 = 0x2e,
	/* [한국어] WRITE 후 자동 VERIFY. lib/scsi가 두 단계로 분해해 처리. */
	SPDK_SBC_WRITE_AND_VERIFY_12 = 0xae,
	/* [한국어] 12-byte WRITE_AND_VERIFY. */
	SPDK_SBC_WRITE_AND_VERIFY_16 = 0x8e,
	/* [한국어] 16-byte WRITE_AND_VERIFY. */
	SPDK_SBC_WRITE_LONG_10 = 0x3f,
	/* [한국어] 데이터 + ECC를 직접 쓰기 (HDD 진단용). */
	SPDK_SBC_WRITE_SAME_10 = 0x41,
	/* [한국어] 단일 블록 패턴을 다수 LBA에 반복 write. UNMAP=1 이면 deallocate(write-zeroes 매핑).
	 *         VPD 0xB2 LBPWS10 비트로 지원 광고. NVMe Write Zeroes / Dataset Mgmt 매핑. */
	SPDK_SBC_WRITE_SAME_16 = 0x93,
	/* [한국어] 16-byte WRITE_SAME. UNMAP 비트로 sparse provisioning 핵심. NDMP/lvol에서 사용. */
	SPDK_SBC_XDREAD_10 = 0x52,
	/* [한국어] XOR read (RAID 보조). */
	SPDK_SBC_XDWRITE_10 = 0x50,
	/* [한국어] XOR write. */
	SPDK_SBC_XDWRITEREAD_10 = 0x53,
	/* [한국어] XOR write+read. */
	SPDK_SBC_XPWRITE_10 = 0x51,
	/* [한국어] XOR parity write. */

	SPDK_SBC_SAI_READ_CAPACITY_16 = 0x10,
	/* [한국어] SERVICE_ACTION_IN_16(0x9e)의 SA=0x10. 64-bit LBA capacity 조회. 32B 응답.
	 *         block size, PI type, LBPME(thin), LBPRZ(zeroed-on-deallocate) 등 풍부한 정보. */
	SPDK_SBC_SAI_READ_LONG_16 = 0x11,
	/* [한국어] 16-byte READ_LONG service action. */
	SPDK_SBC_SAO_WRITE_LONG_16 = 0x11,
	/* [한국어] SERVICE_ACTION_OUT_16(0x9f)의 SA=0x11. 16-byte WRITE_LONG. */

	SPDK_SBC_VL_READ_32 = 0x0009,
	/* [한국어] VARIABLE_LENGTH(0x7f)의 service action 0x0009. 32-byte CDB READ.
	 *         T10 PI 검증을 위한 추가 필드(예상 ref tag/app tag/guard) 보유. */
	SPDK_SBC_VL_VERIFY_32 = 0x000a,
	/* [한국어] 32-byte VERIFY (PI 검증 강화). */
	SPDK_SBC_VL_WRITE_32 = 0x000b,
	/* [한국어] 32-byte WRITE (PI). */
	SPDK_SBC_VL_WRITE_AND_VERIFY_32 = 0x000c,
	/* [한국어] 32-byte WRITE_AND_VERIFY. */
	SPDK_SBC_VL_WRITE_SAME_32 = 0x000d,
	/* [한국어] 32-byte WRITE_SAME (PI). */
	SPDK_SBC_VL_XDREAD_32 = 0x0003,
	/* [한국어] 32-byte XOR read. */
	SPDK_SBC_VL_XDWRITE_32 = 0x0004,
	/* [한국어] 32-byte XOR write. */
	SPDK_SBC_VL_XDWRITEREAD_32 = 0x0007,
	/* [한국어] 32-byte XOR write+read. */
	SPDK_SBC_VL_XPWRITE_32 = 0x0006,
	/* [한국어] 32-byte XOR parity write. */
};

/* [한국어] START_STOP_UNIT(0x1b) CDB의 START 비트 (cdb[4] 의 비트 0).
 *         1=spin up/start, 0=stop. SPDK는 noop으로 GOOD 응답하나 비트 검사를 위해 정의. */
#define SPDK_SBC_START_STOP_UNIT_START_BIT (1 << 0)

/*
 * [한국어]
 * spdk_mmc_opcode - MMC(Multimedia Commands) opcode (CD/DVD/Blu-ray).
 *
 * 광 매체 드라이브용 명령. SPDK는 일반적으로 광매체 emulation을 하지 않으나, 호환을 위해 enum 정의만 보유한다.
 * cdb[0]은 SBC와 일부 겹치는 값이 있으나, peripheral device type이 0x05(DVD)이면 본 enum으로 해석.
 */
enum spdk_mmc_opcode {
	/* MMC6 */
	SPDK_MMC_READ_DISC_STRUCTURE = 0xad,
	/* [한국어] disc 물리 구조 읽기 (Blu-ray 등). */

	/* MMC4 */
	SPDK_MMC_BLANK = 0xa1,
	/* [한국어] CD-RW blank. */
	SPDK_MMC_CLOSE_TRACK_SESSION = 0x5b,
	/* [한국어] track/session 종료. */
	SPDK_MMC_ERASE_10 = 0x2c,
	/* [한국어] 광매체 erase. */
	SPDK_MMC_FORMAT_UNIT = 0x04,
	/* [한국어] 광매체 포맷. */
	SPDK_MMC_GET_CONFIGURATION = 0x46,
	/* [한국어] 광 드라이브 feature 조회. */
	SPDK_MMC_GET_EVENT_STATUS_NOTIFICATION = 0x4a,
	/* [한국어] 미디어 변경 등 이벤트 polling. */
	SPDK_MMC_GET_PERFORMANCE = 0xac,
	/* [한국어] 성능 통계 조회. */
	SPDK_MMC_INQUIRY = 0x12,
	/* [한국어] INQUIRY (SPC와 동일 코드). */
	SPDK_MMC_LOAD_UNLOAD_MEDIUM = 0xa6,
	/* [한국어] 미디어 로드/언로드. */
	SPDK_MMC_MECHANISM_STATUS = 0xbd,
	/* [한국어] 메카닉 상태 조회. */
	SPDK_MMC_MODE_SELECT_10 = 0x55,
	/* [한국어] mode parameter 설정 (10-byte). */
	SPDK_MMC_MODE_SENSE_10 = 0x5a,
	/* [한국어] mode parameter 조회. */
	SPDK_MMC_PAUSE_RESUME = 0x4b,
	/* [한국어] 오디오 재생 일시정지/재개. */
	SPDK_MMC_PLAY_AUDIO_10 = 0x45,
	/* [한국어] 오디오 재생 (10-byte). */
	SPDK_MMC_PLAY_AUDIO_12 = 0xa5,
	/* [한국어] 오디오 재생 (12-byte). */
	SPDK_MMC_PLAY_AUDIO_MSF = 0x47,
	/* [한국어] MSF(분/초/프레임) 단위 재생. */
	SPDK_MMC_PREVENT_ALLOW_MEDIUM_REMOVAL = 0x1e,
	/* [한국어] 트레이 잠금/해제. */
	SPDK_MMC_READ_10 = 0x28,
	/* [한국어] data sector READ_10. */
	SPDK_MMC_READ_12 = 0xa8,
	/* [한국어] data sector READ_12. */
	SPDK_MMC_READ_BUFFER = 0x3c,
	/* [한국어] 진단 버퍼 읽기. */
	SPDK_MMC_READ_BUFFER_CAPACITY = 0x5c,
	/* [한국어] write 버퍼 잔량 조회. */
	SPDK_MMC_READ_CAPACITY = 0x25,
	/* [한국어] 광매체 capacity. */
	SPDK_MMC_READ_CD = 0xbe,
	/* [한국어] CD raw read (sub-channel 포함). */
	SPDK_MMC_READ_CD_MSF = 0xb9,
	/* [한국어] CD raw read MSF 형식. */
	SPDK_MMC_READ_DISC_INFORMATION = 0x51,
	/* [한국어] disc 정보 (rec/multi-session 등). */
	SPDK_MMC_READ_DVD_STRUCTURE = 0xad,
	/* [한국어] DVD 구조 (READ_DISC_STRUCTURE alias). */
	SPDK_MMC_READ_FORMAT_CAPACITIES = 0x23,
	/* [한국어] 가능한 포맷 용량 목록. */
	SPDK_MMC_READ_SUB_CHANNEL = 0x42,
	/* [한국어] CD sub-channel data. */
	SPDK_MMC_READ_TOC_PMA_ATIP = 0x43,
	/* [한국어] TOC/PMA/ATIP 정보. */
	SPDK_MMC_READ_TRACK_INFORMATION = 0x52,
	/* [한국어] 트랙 단위 정보. */
	SPDK_MMC_REPAIR_TRACK = 0x58,
	/* [한국어] 트랙 복구. */
	SPDK_MMC_REPORT_KEY = 0xa4,
	/* [한국어] CSS 인증 key 보고 (DVD copy protect). */
	SPDK_MMC_REQUEST_SENSE = 0x03,
	/* [한국어] sense data 요청 (SPC와 공통). */
	SPDK_MMC_RESERVE_TRACK = 0x53,
	/* [한국어] 추후 record를 위한 트랙 예약. */
	SPDK_MMC_SCAN = 0xba,
	/* [한국어] 빠른 탐색. */
	SPDK_MMC_SEEK_10 = 0x2b,
	/* [한국어] 헤드 이동. */
	SPDK_MMC_SEND_CUE_SHEET = 0x5d,
	/* [한국어] CD recording용 cue sheet 전송. */
	SPDK_MMC_SEND_DVD_STRUCTURE = 0xbf,
	/* [한국어] DVD 구조 전송. */
	SPDK_MMC_SEND_KEY = 0xa3,
	/* [한국어] CSS 인증 key 전송. */
	SPDK_MMC_SEND_OPC_INFORMATION = 0x54,
	/* [한국어] OPC(Optimum Power Calibration) 정보 전송. */
	SPDK_MMC_SET_CD_SPEED = 0xbb,
	/* [한국어] 회전 속도 설정. */
	SPDK_MMC_SET_READ_AHEAD = 0xa7,
	/* [한국어] read-ahead 영역 지정. */
	SPDK_MMC_SET_STREAMING = 0xb6,
	/* [한국어] 스트리밍 모드 설정. */
	SPDK_MMC_START_STOP_UNIT = 0x1b,
	/* [한국어] 트레이 eject/load (1<<1=LoEj). */
	SPDK_MMC_STOP_PLAY_SCAN = 0x4e,
	/* [한국어] 재생/스캔 중지. */
	SPDK_MMC_SYNCHRONIZE_CACHE = 0x35,
	/* [한국어] write cache flush. */
	SPDK_MMC_TEST_UNIT_READY = 0x00,
	/* [한국어] ready polling. */
	SPDK_MMC_VERIFY_10 = 0x2f,
	/* [한국어] write 후 verify. */
	SPDK_MMC_WRITE_10 = 0xa2,
	/* [한국어] data sector WRITE_10. opcode가 SBC와 다름(0xa2 vs 0x2a) — peripheral type 분기 필요. */
	SPDK_MMC_WRITE_12 = 0xaa,
	/* [한국어] data sector WRITE_12. */
	SPDK_MMC_WRITE_AND_VERIFY_10 = 0x2e,
	/* [한국어] write+verify. */
	SPDK_MMC_WRITE_BUFFER = 0x3b,
	/* [한국어] 펌웨어/진단용 버퍼 write. */
};

/*
 * [한국어]
 * spdk_ssc_opcode - SSC(Stream Commands) opcode (테이프 디바이스).
 *
 * 테이프 시퀀셜 디바이스용. SPDK가 직접 테이프를 다루지는 않으나, peripheral type 0x01 디바이스에 대해
 * INQUIRY 응답이나 디버그 출력에서 본 enum이 사용된다.
 */
enum spdk_ssc_opcode {
	SPDK_SSC_ERASE_6 = 0x19,
	/* [한국어] 테이프 erase. */
	SPDK_SSC_FORMAT_MEDIUM = 0x04,
	/* [한국어] 테이프 포맷. */
	SPDK_SSC_LOAD_UNLOAD = 0x1b,
	/* [한국어] 테이프 로드/언로드. */
	SPDK_SSC_LOCATE_10 = 0x2b,
	/* [한국어] 특정 위치로 이동 (10-byte). */
	SPDK_SSC_LOCATE_16 = 0x92,
	/* [한국어] 16-byte LOCATE. */
	SPDK_SSC_MOVE_MEDIUM_ATTACHED = 0xa7,
	/* [한국어] auto-loader 매체 이동. */
	SPDK_SSC_READ_6 = 0x08,
	/* [한국어] 테이프 READ_6. */
	SPDK_SSC_READ_BLOCK_LIMITS = 0x05,
	/* [한국어] 블록 크기 제한 조회. */
	SPDK_SSC_READ_ELEMENT_STATUS_ATTACHED = 0xb4,
	/* [한국어] auto-loader element 상태. */
	SPDK_SSC_READ_POSITION = 0x34,
	/* [한국어] 현재 위치 조회. */
	SPDK_SSC_READ_REVERSE_6 = 0x0f,
	/* [한국어] 역방향 read. */
	SPDK_SSC_RECOVER_BUFFERED_DATA = 0x14,
	/* [한국어] 버퍼 잔존 데이터 회복. */
	SPDK_SSC_REPORT_DENSITY_SUPPORT = 0x44,
	/* [한국어] 지원 밀도 보고. */
	SPDK_SSC_REWIND = 0x01,
	/* [한국어] 테이프 되감기. */
	SPDK_SSC_SET_CAPACITY = 0x0b,
	/* [한국어] 가용 용량 설정. */
	SPDK_SSC_SPACE_6 = 0x11,
	/* [한국어] 블록/파일마크 단위 이동 (6-byte). */
	SPDK_SSC_SPACE_16 = 0x91,
	/* [한국어] 16-byte SPACE. */
	SPDK_SSC_VERIFY_6 = 0x13,
	/* [한국어] 테이프 verify. */
	SPDK_SSC_WRITE_6 = 0x0a,
	/* [한국어] 테이프 WRITE_6. */
	SPDK_SSC_WRITE_FILEMARKS_6 = 0x10,
	/* [한국어] file mark 기록 (테이프 분할자). */
};

/*
 * [한국어]
 * spdk_spc_vpd - INQUIRY VPD(Vital Product Data) page code (SPC-4 §7.8).
 *
 * INQUIRY 명령에 EVPD=1 + page_code=값 으로 요청 시 반환되는 페이지의 식별자.
 * 호스트(특히 multipath/scsi-mid)가 디바이스를 식별하고 기능을 광고받는 표준 채널.
 */
enum spdk_spc_vpd {
	SPDK_SPC_VPD_DEVICE_IDENTIFICATION = 0x83,
	/* [한국어] 디바이스 식별 페이지. NAA / EUI64 / SCSI Name 등 designator 리스트. multipath 식별의 핵심.
	 *         NVMe NGUID/EUI64를 NAA 또는 EUI64 designator로 매핑하여 보고 (lib/bdev/scsi_nvme.c). */
	SPDK_SPC_VPD_EXTENDED_INQUIRY_DATA = 0x86,
	/* [한국어] 확장 inquiry. PI/SIMPSUP/HEADSUP/PRIOR_SUP 등 부가 기능 비트맵. spdk_scsi_vpd_ext_inquiry 구조체. */
	SPDK_SPC_VPD_MANAGEMENT_NETWORK_ADDRESSES = 0x85,
	/* [한국어] 관리 네트워크 주소 (storage controller IP 등). */
	SPDK_SPC_VPD_MODE_PAGE_POLICY = 0x87,
	/* [한국어] 각 mode page의 정책(shared/per_target_port/per_initiator). */
	SPDK_SPC_VPD_SCSI_PORTS = 0x88,
	/* [한국어] target의 SCSI port 목록 + 각 port의 transport ID. */
	SPDK_SPC_VPD_SOFTWARE_INTERFACE_IDENTIFICATION = 0x84,
	/* [한국어] 소프트웨어 인터페이스 식별 (펌웨어 GUID 등). */
	SPDK_SPC_VPD_SUPPORTED_VPD_PAGES = 0x00,
	/* [한국어] 본 디바이스가 지원하는 VPD page 목록. 호스트가 가장 먼저 발행. */
	SPDK_SPC_VPD_UNIT_SERIAL_NUMBER = 0x80,
	/* [한국어] 시리얼 번호 (ASCII). multipath 보조 식별자. */
	SPDK_SPC_VPD_BLOCK_LIMITS = 0xb0,
	/* [한국어] WRITE_SAME 최대 블록, UNMAP descriptor 최대, optimal transfer length 등 SBC 한도.
	 *         커널/fio 등이 본 페이지로 large I/O 분할 정책 결정. */
	SPDK_SPC_VPD_BLOCK_DEV_CHARS = 0xb1,
	/* [한국어] medium rotation rate(0=non-rotating SSD, 1=non-rotating reserved, 1024+=rpm), nominal form factor. */
	SPDK_SPC_VPD_BLOCK_THIN_PROVISION = 0xb2,
	/* [한국어] thin provisioning 광고. LBPU(UNMAP 지원), LBPWS/LBPWS10(WRITE_SAME unmap 지원),
	 *         LBPRZ(deallocate 후 0 read 보장), provisioning type. */
};

/*
 * [한국어]
 * spdk_spc_peripheral_qualifier - INQUIRY 응답의 peripheral qualifier 필드 (3비트, SPC-4 Table-138).
 *
 * 디바이스 연결 상태를 표현. spdk_scsi_cdb_inquiry_data::peripheral_qualifier에 저장.
 */
enum spdk_spc_peripheral_qualifier {
	SPDK_SPC_PERIPHERAL_QUALIFIER_CONNECTED = 0,
	/* [한국어] 디바이스가 연결되어 있고 사용 가능. 정상 LUN. */
	SPDK_SPC_PERIPHERAL_QUALIFIER_NOT_CONNECTED = 1,
	/* [한국어] 디바이스 타입은 지원하나 현재 연결되어 있지 않음. */
	SPDK_SPC_PERIPHERAL_QUALIFIER_NOT_CAPABLE = 3,
	/* [한국어] 해당 LUN을 지원하지 않음. process_null_lun에서 일부 케이스에 보고. */
};

/*
 * [한국어]
 * 익명 enum - 다양한 SPC 정수 상수 모음.
 *
 * peripheral device type, SPC version, transport protocol identifier, code set, association,
 * designator type 등 INQUIRY/VPD 응답 작성에 필요한 상수들이 한 익명 enum 으로 묶여 있다.
 * 익명이라 별도 타입은 없으나 정수 리터럴 대신 의미 있는 이름을 부여하기 위함.
 */
enum {
	SPDK_SPC_PERIPHERAL_DEVICE_TYPE_DISK = 0x00,
	/* [한국어] Direct access block device (disk/SSD). 가장 흔한 SPDK 타겟. */
	SPDK_SPC_PERIPHERAL_DEVICE_TYPE_TAPE = 0x01,
	/* [한국어] Sequential access (테이프). SSC. */
	SPDK_SPC_PERIPHERAL_DEVICE_TYPE_DVD = 0x05,
	/* [한국어] CD/DVD/BD. MMC. */
	SPDK_SPC_PERIPHERAL_DEVICE_TYPE_CHANGER = 0x08,
	/* [한국어] Medium changer (auto-loader). */

	SPDK_SPC_VERSION_NONE = 0x00,
	/* [한국어] SPC 버전 미보고 (신규 디바이스가 아님). */
	SPDK_SPC_VERSION_SPC = 0x03,
	/* [한국어] SPC-1 (1997). */
	SPDK_SPC_VERSION_SPC2 = 0x04,
	/* [한국어] SPC-2 (2001). */
	SPDK_SPC_VERSION_SPC3 = 0x05,
	/* [한국어] SPC-3 (2005). PR 기능 도입. */
	SPDK_SPC_VERSION_SPC4 = 0x06,
	/* [한국어] SPC-4 (2014). SPDK가 광고하는 기본 버전. */

	SPDK_SPC_PROTOCOL_IDENTIFIER_FC = 0x00,
	/* [한국어] Fibre Channel transport. */
	SPDK_SPC_PROTOCOL_IDENTIFIER_PSCSI = 0x01,
	/* [한국어] Parallel SCSI (legacy). */
	SPDK_SPC_PROTOCOL_IDENTIFIER_SSA = 0x02,
	/* [한국어] Serial Storage Architecture. */
	SPDK_SPC_PROTOCOL_IDENTIFIER_IEEE1394 = 0x03,
	/* [한국어] FireWire (SBP-2). */
	SPDK_SPC_PROTOCOL_IDENTIFIER_RDMA = 0x04,
	/* [한국어] SRP (SCSI RDMA Protocol) over InfiniBand. */
	SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI = 0x05,
	/* [한국어] iSCSI over TCP. SPDK iSCSI target이 보고하는 값. */
	SPDK_SPC_PROTOCOL_IDENTIFIER_SAS = 0x06,
	/* [한국어] Serial Attached SCSI. */
	SPDK_SPC_PROTOCOL_IDENTIFIER_ADT = 0x07,
	/* [한국어] Automation/Drive Interface (테이프 자동화). */
	SPDK_SPC_PROTOCOL_IDENTIFIER_ATA = 0x08,
	/* [한국어] ATA (SATA). */

	SPDK_SPC_VPD_CODE_SET_BINARY = 0x01,
	/* [한국어] designator가 raw binary (NAA/EUI64). */
	SPDK_SPC_VPD_CODE_SET_ASCII = 0x02,
	/* [한국어] ASCII (T10 vendor ID 등). */
	SPDK_SPC_VPD_CODE_SET_UTF8 = 0x03,
	/* [한국어] UTF-8 (SCSI Name iSCSI). */

	SPDK_SPC_VPD_ASSOCIATION_LOGICAL_UNIT = 0x00,
	/* [한국어] 본 designator는 LUN과 연관 (가장 일반적). */
	SPDK_SPC_VPD_ASSOCIATION_TARGET_PORT = 0x01,
	/* [한국어] target port와 연관. */
	SPDK_SPC_VPD_ASSOCIATION_TARGET_DEVICE = 0x02,
	/* [한국어] target device 전체와 연관. */

	SPDK_SPC_VPD_IDENTIFIER_TYPE_VENDOR_SPECIFIC = 0x00,
	/* [한국어] 벤더 의존 (해석 자유). */
	SPDK_SPC_VPD_IDENTIFIER_TYPE_T10_VENDOR_ID = 0x01,
	/* [한국어] T10 vendor ID + product ID + serial 형식 (ASCII). */
	SPDK_SPC_VPD_IDENTIFIER_TYPE_EUI64 = 0x02,
	/* [한국어] IEEE EUI-64. NVMe namespace의 EUI64를 그대로 매핑 가능. */
	SPDK_SPC_VPD_IDENTIFIER_TYPE_NAA = 0x03,
	/* [한국어] IEEE NAA. multipath OS가 가장 선호하는 형식 (8/16바이트).
	 *         NVMe NGUID(16B)와 호환되는 NAA Format 6 형태로 보고 가능. */
	SPDK_SPC_VPD_IDENTIFIER_TYPE_RELATIVE_TARGET_PORT = 0x04,
	/* [한국어] target port 상대 식별자. */
	SPDK_SPC_VPD_IDENTIFIER_TYPE_TARGET_PORT_GROUP = 0x05,
	/* [한국어] TPG ID (ALUA). */
	SPDK_SPC_VPD_IDENTIFIER_TYPE_LOGICAL_UNIT_GROUP = 0x06,
	/* [한국어] LU 그룹 ID. */
	SPDK_SPC_VPD_IDENTIFIER_TYPE_MD5_LOGICAL_UNIT = 0x07,
	/* [한국어] LU의 MD5 해시. */
	SPDK_SPC_VPD_IDENTIFIER_TYPE_SCSI_NAME = 0x08,
	/* [한국어] SCSI name string (UTF-8). iSCSI IQN을 그대로 designator로 사용 가능. */
};

/*
 * [한국어]
 * spdk_scsi_cdb_inquiry - INQUIRY(0x12) 6-byte CDB의 와이어 레이아웃 (SPC-4 §6.4).
 *
 * 호스트가 INQUIRY를 발행하면 본 구조체에 1:1 매핑되는 6바이트가 도착한다.
 * 미들레이어는 evpd / page_code / alloc_len을 읽어 standard inquiry 또는 VPD page를 결정.
 */
struct spdk_scsi_cdb_inquiry {
	uint8_t opcode;
	/* [한국어] 항상 0x12 (SPDK_SPC_INQUIRY). 디스패처가 본 값으로 명령 식별. */
	uint8_t evpd;
	/* [한국어] EVPD 비트 (bit 0). 0=standard inquiry, 1=VPD page 요청.
	 *         설정자: 호스트(initiator). 읽는 자: 미들레이어 디코더. */
	uint8_t page_code;
	/* [한국어] EVPD=1일 때 요청 VPD page 코드 (spdk_spc_vpd 값). EVPD=0이면 무시(0이어야 함). */
	uint8_t alloc_len[2];
	/* [한국어] allocation length (Big-Endian uint16). 호스트 버퍼 크기 — 응답을 이 길이로 절단해야 함.
	 *         미들레이어는 from_be16(alloc_len)으로 읽고, 응답이 크면 truncate. */
	uint8_t control;
	/* [한국어] CONTROL 바이트 (NACA/Linked 비트 등). SPDK는 NACA만 검사하고 나머지는 무시. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_scsi_cdb_inquiry) == 6, "incorrect CDB size");
/* [한국어] 컴파일 타임 검증: CDB 크기가 정확히 6바이트(SPC-4 Table-141). 패딩이 끼면 컴파일 실패. */

/*
 * [한국어]
 * spdk_scsi_cdb_inquiry_data - Standard INQUIRY 응답 데이터 헤더 (SPC-4 §6.4.2 Table-142).
 *
 * EVPD=0인 INQUIRY 응답의 첫 96바이트(=고정 96B + 가변 desc[]) 구조.
 * 미들레이어는 LUN의 bdev/SCSI 설정을 읽어 본 구조체를 채워 호스트에 반환.
 * 비트필드 순서가 little-endian C 컴파일러 기준으로 wire와 매칭된다 (peripheral_device_type 5비트가 lower).
 */
struct spdk_scsi_cdb_inquiry_data {
	uint8_t peripheral_device_type : 5;
	/* [한국어] 디바이스 타입 (SPDK_SPC_PERIPHERAL_DEVICE_TYPE_DISK 등). 하위 5비트. */
	uint8_t peripheral_qualifier : 3;
	/* [한국어] 연결 상태 (spdk_spc_peripheral_qualifier 값). 상위 3비트. */
	uint8_t rmb;
	/* [한국어] Removable Media Bit (bit 7). SPDK 디스크는 0. */
	uint8_t version;
	/* [한국어] 지원 SPC 버전 (SPDK_SPC_VERSION_SPC4=0x06). */
	uint8_t response;
	/* [한국어] response data format (4 bit) + HISUP/NORMACA. SPC-4: format=2 고정. */
	uint8_t add_len;
	/* [한국어] additional length: 본 바이트 이후의 바이트 수 (총 length - 5). 호스트 파서의 길이 검증용. */
	uint8_t flags;
	/* [한국어] SCCS/ACC/TPGS/3PC/PROTECT 비트 모음 (byte 5).
	 *         TPGS는 ALUA 지원, PROTECT는 T10 PI 지원 광고. */
	uint8_t flags2;
	/* [한국어] ENCSERV/VS/MULTIP/MCHNGR (byte 6). MULTIP=다중 port 지원 광고. */
	uint8_t flags3;
	/* [한국어] BQUE/CMDQUE/VS/RELADR (byte 7). CMDQUE=명령 큐잉 지원 (항상 1로 광고). */
	uint8_t t10_vendor_id[8];
	/* [한국어] T10 vendor ID 8B ASCII (예: "INTEL   "). 부족 시 공백으로 패딩. */
	uint8_t product_id[16];
	/* [한국어] product ID 16B ASCII (예: "SPDK bdev       "). */
	uint8_t product_rev[4];
	/* [한국어] product revision 4B ASCII (예: "0001"). */
	uint8_t vendor[20];
	/* [한국어] vendor-specific 영역 20B. SPDK는 보통 공백 또는 시리얼 일부를 채움. */
	uint8_t ius;
	/* [한국어] IUS/QAS/CLOCKING (byte 56). SAS 관련 비트, SPDK는 0. */
	uint8_t reserved;
	/* [한국어] 예약 바이트 — 0으로 채움. */
	uint8_t desc[];
	/* [한국어] version descriptor 배열 (가변 길이, 각 2B). 지원 표준의 배열 — SPC-4/SBC-3 등. */
};

/*
 * [한국어]
 * spdk_scsi_vpd_page - VPD page 응답 공통 헤더 (SPC-4 §7.8 Table-461).
 *
 * 모든 VPD page는 4바이트 헤더(타입/페이지코드/길이) + 가변 params[]로 구성.
 * 미들레이어는 page_code별로 params 영역을 다르게 채운다.
 */
struct spdk_scsi_vpd_page {
	uint8_t peripheral_device_type : 5;
	/* [한국어] 디바이스 타입 (inquiry data와 동일). */
	uint8_t peripheral_qualifier : 3;
	/* [한국어] 연결 상태. */
	uint8_t page_code;
	/* [한국어] 본 응답이 어떤 VPD page인지 (spdk_spc_vpd 값과 일치). */
	uint8_t alloc_len[2];
	/* [한국어] 본 헤더 이후 params[] 의 바이트 수 (Big-Endian uint16).
	 *         호스트 파서가 정확히 이 길이만큼 읽도록 보장. */
	uint8_t params[];
	/* [한국어] page-specific 페이로드 (designator 리스트, block limits 필드 등). */
};

/* [한국어] 아래 SPDK_SCSI_VEXT_* 매크로들은 VPD 0x86(Extended Inquiry)의 비트 플래그 정의이다.
 *         spdk_scsi_vpd_ext_inquiry::check / sup / sup2 등 각 1바이트 필드의 비트 비트마스크. */

/* [한국어] check 필드 - REF_CHK 비트: T10 PI Reference Tag 검증 지원. */
#define SPDK_SCSI_VEXT_REF_CHK		0x01
/* [한국어] check 필드 - APP_CHK 비트: T10 PI Application Tag 검증 지원. */
#define SPDK_SCSI_VEXT_APP_CHK		0x02
/* [한국어] check 필드 - GRD_CHK 비트: T10 PI Guard CRC 검증 지원. */
#define SPDK_SCSI_VEXT_GRD_CHK		0x04
/* [한국어] sup 필드 - SIMPSUP: Simple task attribute 지원. */
#define SPDK_SCSI_VEXT_SIMPSUP		0x01
/* [한국어] sup 필드 - ORDSUP: Ordered task attribute 지원. */
#define SPDK_SCSI_VEXT_ORDSUP		0x02
/* [한국어] sup 필드 - HEADSUP: Head of Queue task attribute 지원. */
#define SPDK_SCSI_VEXT_HEADSUP		0x04
/* [한국어] sup 필드 - PRIOR_SUP: priority 큐잉 지원. */
#define SPDK_SCSI_VEXT_PRIOR_SUP	0x08
/* [한국어] sup 필드 - GROUP_SUP: grouping 함수 지원. */
#define SPDK_SCSI_VEXT_GROUP_SUP	0x10
/* [한국어] sup 필드 - UASK_SUP: UNIT ATTENTION condition queuing 지원. */
#define SPDK_SCSI_VEXT_UASK_SUP		0x20
/* [한국어] sup2 필드 - V_SUP: volatile cache 지원 광고. */
#define SPDK_SCSI_VEXT_V_SUP		0x01
/* [한국어] sup2 필드 - NV_SUP: non-volatile cache 지원 광고. */
#define SPDK_SCSI_VEXT_NV_SUP		0x02
/* [한국어] sup2 필드 - CRD_SUP: capacity-reservation descriptor 지원. */
#define SPDK_SCSI_VEXT_CRD_SUP		0x04
/* [한국어] sup2 필드 - WU_SUP: write uncorrectable 지원. */
#define SPDK_SCSI_VEXT_WU_SUP		0x08

/*
 * [한국어]
 * spdk_scsi_vpd_ext_inquiry - VPD 0x86 Extended Inquiry Data 응답 (SPC-4 §7.8.6 Table-468).
 *
 * 64바이트 고정 길이 응답으로, T10 PI/큐잉/캐시 등 부가 기능 비트맵을 광고한다.
 */
struct spdk_scsi_vpd_ext_inquiry {
	uint8_t peripheral;
	/* [한국어] peripheral_device_type(5b) + peripheral_qualifier(3b)를 합친 1바이트. */
	uint8_t page_code;
	/* [한국어] 0x86 고정 (EXTENDED_INQUIRY_DATA). */
	uint8_t alloc_len[2];
	/* [한국어] 이후 데이터 길이 (Big-Endian uint16, 보통 60). */
	uint8_t check;
	/* [한국어] PI 검증 비트맵 (REF_CHK/APP_CHK/GRD_CHK + ACTIVATE_MICROCODE 등). */
	uint8_t sup;
	/* [한국어] task attribute 지원 비트맵 (SIMPSUP/ORDSUP/HEADSUP/PRIOR_SUP/GROUP_SUP/UASK_SUP). */
	uint8_t sup2;
	/* [한국어] 캐시 등 추가 지원 비트 (V_SUP/NV_SUP/CRD_SUP/WU_SUP). */
	uint8_t luiclr;
	/* [한국어] LUN i_t nexus loss/reset 시 클리어 동작. */
	uint8_t cbcs;
	/* [한국어] referrals capable, R_SUP 등. */
	uint8_t micro_dl;
	/* [한국어] microcode download 단위. */
	uint8_t reserved[54];
	/* [한국어] 나머지 예약 영역 — 0으로 채움. 총 길이 64바이트 보장. */
};

/* [한국어] designator descriptor의 PIV(Protocol Identifier Valid) 비트 (byte 1, bit 7).
 *         1이면 protocol_id 필드 유효 — target_port와 연관된 designator는 PIV=1이 일반적. */
#define SPDK_SPC_VPD_DESIG_PIV	0x80

/* designation descriptor */
/*
 * [한국어]
 * spdk_scsi_desig_desc - VPD 0x83 Device Identification 페이지의 단일 designator descriptor.
 *
 * 한 LUN/port에 여러 designator가 붙을 수 있어 가변 배열로 나열된다.
 * 호스트 multipath 드라이버는 이들 중 NAA/EUI64를 우선해 LUN의 unique ID로 사용한다.
 */
struct spdk_scsi_desig_desc {
	uint8_t code_set	: 4;
	/* [한국어] designator 인코딩 (BINARY=1, ASCII=2, UTF8=3). 하위 4비트. */
	uint8_t protocol_id	: 4;
	/* [한국어] piv=1일 때 의미: 이 designator가 어느 transport와 연관되는지 (ISCSI=5 등). */
	uint8_t type		: 4;
	/* [한국어] designator type (SPDK_SPC_VPD_IDENTIFIER_TYPE_*). NAA=3, EUI64=2, SCSI_NAME=8. */
	uint8_t association	: 2;
	/* [한국어] 본 designator가 LU/port/device 중 무엇을 식별하는지. */
	uint8_t reserved0	: 1;
	/* [한국어] 예약 비트. */
	uint8_t piv		: 1;
	/* [한국어] Protocol Identifier Valid. 1이면 protocol_id 필드 의미 있음. */
	uint8_t reserved1;
	/* [한국어] 예약 바이트 — 0으로 채움. */
	uint8_t	len;
	/* [한국어] 뒤따르는 desig[] 의 바이트 수. NAA-6=8, NAA-Type6=16 등. */
	uint8_t desig[];
	/* [한국어] 실제 식별자 바이트열 (binary 또는 ASCII). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_scsi_desig_desc) == 4, "Invalid size");
/* [한국어] 헤더 정확히 4바이트 (SPC-4 Table-462). */

/* mode page policy descriptor */
/*
 * [한국어]
 * spdk_scsi_mpage_policy_desc - VPD 0x87 Mode Page Policy의 단일 entry.
 *
 * 각 mode page가 shared인지 per-target-port인지 등을 광고. 호스트가 mode page 변경의 영향을 추정하는 데 사용.
 */
struct spdk_scsi_mpage_policy_desc {
	uint8_t page_code;
	/* [한국어] 대상 mode page code (Caching=0x08, Control=0x0a 등). */
	uint8_t sub_page_code;
	/* [한국어] subpage 코드 (있으면). */
	uint8_t policy;
	/* [한국어] 정책 비트필드. shared=0x00, per_target_port=0x01, per_initiator=0x03 등. */
	uint8_t reserved;
	/* [한국어] 예약. */
};

/* target port descriptor */
/*
 * [한국어]
 * spdk_scsi_tgt_port_desc - VPD 0x88 SCSI Ports의 target port designator entry.
 */
struct spdk_scsi_tgt_port_desc {
	uint8_t code_set;
	/* [한국어] designator 인코딩 (UTF8=3 — iSCSI IQN). */
	uint8_t desig_type;
	/* [한국어] designator type (보통 SCSI_NAME=8). */
	uint8_t reserved;
	/* [한국어] 예약. */
	uint8_t	len;
	/* [한국어] designator[] 바이트 수. */
	uint8_t designator[];
	/* [한국어] target port 식별자 (예: "iqn.2016-06.io.spdk:tgt0,t,0x1"). */
};

/* SCSI port designation descriptor */
/*
 * [한국어]
 * spdk_scsi_port_desc - VPD 0x88 SCSI Ports의 한 port 전체 entry.
 *
 * relative target port id + initiator port + target port 정보가 한 묶음으로 나열된다.
 * 모든 16-bit 필드는 Big-Endian.
 */
struct spdk_scsi_port_desc {
	uint16_t reserved;
	/* [한국어] 예약. 0으로 채움. */
	uint16_t rel_port_id;
	/* [한국어] relative target port identifier (Big-Endian). dev 안에서 port를 식별. */
	uint16_t reserved2;
	/* [한국어] 예약. */
	uint16_t init_port_len;
	/* [한국어] initiator port designator 길이. */
	uint16_t init_port_id;
	/* [한국어] initiator port id (간략화 — 실제로는 가변길이 string이나 wire에서는 길이 필드와 연동). */
	uint16_t reserved3;
	/* [한국어] 예약. */
	uint16_t tgt_desc_len;
	/* [한국어] 뒤따르는 tgt_desc[] (target port descriptor 묶음) 의 바이트 수. */
	uint8_t tgt_desc[];
	/* [한국어] spdk_scsi_tgt_port_desc 들의 가변 배열. */
};

/* iSCSI initiator port TransportID header */
/*
 * [한국어]
 * spdk_scsi_iscsi_transport_id - SPC-4 Table-258 iSCSI Transport ID (4B 헤더 + 가변 name).
 *
 * Persistent Reservation 응답이나 SPECIFY_INITIATOR_PORTS 파라미터에서 initiator를 식별할 때 사용.
 * format 필드:
 *   0=World Wide Unique Initiator Port Identifier ("iqn.xxx,i,0x<isid>")
 *   1=iSCSI Initiator Device TransportID Format ("iqn.xxx")
 */
struct spdk_scsi_iscsi_transport_id {
	uint8_t protocol_id : 4;
	/* [한국어] 항상 SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI(0x05). 하위 4비트. */
	uint8_t reserved1   : 2;
	/* [한국어] 예약. */
	uint8_t format      : 2;
	/* [한국어] iSCSI Transport ID format (0 또는 1). 상위 2비트. */
	uint8_t reserved2;
	/* [한국어] 예약. */
	uint16_t additional_len;
	/* [한국어] name[] 의 바이트 수 (Big-Endian uint16). 호스트가 정확히 이 길이만큼 파싱. */
	uint8_t name[];
	/* [한국어] iSCSI 이름 (UTF-8 ASCII), 형식은 위 format 비트에 따름. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_scsi_iscsi_transport_id) == 4, "Incorrect size");
/* [한국어] 헤더 정확히 4바이트. */

/* SCSI UNMAP block descriptor */
/*
 * [한국어]
 * spdk_scsi_unmap_bdesc - UNMAP(0x42) 명령의 single block descriptor (SBC-3 §5.28 Table-91, 16바이트).
 *
 * UNMAP 파라미터 데이터는 8B 헤더 + N개의 본 descriptor로 구성. 미들레이어는 본 구조체 배열을 디코드해
 * 각 (lba, block_count) 영역을 spdk_bdev_unmap_blocks()로 NVMe Dataset Management(deallocate)에 매핑.
 */
struct spdk_scsi_unmap_bdesc {
	/* UNMAP LOGICAL BLOCK ADDRESS */
	uint64_t lba;
	/* [한국어] deallocate 시작 LBA (Big-Endian, 8B). 호스트가 from_be64로 읽어 그대로 전달. */

	/* NUMBER OF LOGICAL BLOCKS */
	uint32_t block_count;
	/* [한국어] deallocate할 블록 수 (Big-Endian uint32). 0이면 noop. */

	/* RESERVED */
	uint32_t reserved;
	/* [한국어] 예약 4바이트 — 0이어야 함. 합계 16바이트. */
};

/* SCSI Persistent Reserve In action codes */
/*
 * [한국어]
 * spdk_scsi_pr_in_action_code - PERSISTENT_RESERVE_IN(0x5e) CDB의 service action 코드 (SPC-4 §6.13).
 *
 * cdb[1] 의 하위 5비트로 4가지 조회 서비스를 디스패치.
 */
enum spdk_scsi_pr_in_action_code {
	/* Read all registered reservation keys */
	SPDK_SCSI_PR_IN_READ_KEYS		= 0x00,
	/* [한국어] 현재 등록된 모든 reservation key 목록 조회.
	 *         응답: pr_generation + key 배열. lib/scsi/scsi_pr.c에서 처리. */
	/* Read current persistent reservations */
	SPDK_SCSI_PR_IN_READ_RESERVATION	= 0x01,
	/* [한국어] 활성 reservation 1개의 정보 조회 (없으면 빈 응답). */
	/* Return capabilities information */
	SPDK_SCSI_PR_IN_REPORT_CAPABILITIES	= 0x02,
	/* [한국어] PR 기능 광고: PTPL(persist through power loss), CRH(SPC-2 호환), 지원 type 마스크 등.
	 *         응답: spdk_scsi_pr_in_report_capabilities_data 구조체 (8B). */
	/* Read all registrations and persistent reservations */
	SPDK_SCSI_PR_IN_READ_FULL_STATUS	= 0x03,
	/* [한국어] 모든 registrant + reservation을 한 번에 조회 (각 entry는 transport ID 포함, 가변 길이). */
	/* 0x04h - 0x1fh Reserved */
};

/*
 * [한국어]
 * spdk_scsi_pr_scope_code - reservation의 적용 범위 코드 (SPC-4 §6.14 Table-219).
 *
 * 현재는 LU_SCOPE만 사용됨 — element scope는 deprecated.
 */
enum spdk_scsi_pr_scope_code {
	/* Persistent reservation applies to full logical unit */
	SPDK_SCSI_PR_LU_SCOPE			= 0x00,
	/* [한국어] reservation이 LUN 전체에 적용. SPDK가 보고/허용하는 유일한 scope. */
};

/* SCSI Persistent Reservation type codes */
/*
 * [한국어]
 * spdk_scsi_pr_type_code - reservation 정책 type 코드 (SPC-4 §6.14 Table-219).
 *
 * 6가지 정책으로, write/read 권한과 등록자 우대 여부에 따라 분기.
 *   - Write Exclusive: 보유자만 write. 다른 모두는 read만 가능.
 *   - Exclusive Access: 보유자만 read+write. 다른 모두는 모든 접근 거부.
 *   - * - Registrants Only: 비보유자도 등록자이면 read 또는 read+write 가능.
 *   - * - All Registrants: 모든 등록자가 보유자처럼 동작 (no single holder).
 */
enum spdk_scsi_pr_type_code {
	/* Write Exclusive */
	SPDK_SCSI_PR_WRITE_EXCLUSIVE		= 0x01,
	/* [한국어] 보유자만 write 가능, 비보유자는 read 가능. 가장 흔한 cluster fencing 정책. */
	/* Exclusive Access */
	SPDK_SCSI_PR_EXCLUSIVE_ACCESS		= 0x03,
	/* [한국어] 보유자만 read+write 가능, 비보유자는 모든 access 거부. 강한 격리. */
	/* Write Exclusive - Registrants Only */
	SPDK_SCSI_PR_WRITE_EXCLUSIVE_REGS_ONLY	= 0x05,
	/* [한국어] 보유자만 write, 비등록자는 read도 거부. 등록자는 read 가능. */
	/* Exclusive Access - Registrants Only */
	SPDK_SCSI_PR_EXCLUSIVE_ACCESS_REGS_ONLY	= 0x06,
	/* [한국어] 보유자만 read+write, 비등록자는 모두 거부. 등록자는 read 가능. */
	/* Write Exclusive - All Registrants */
	SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS	= 0x07,
	/* [한국어] 모든 등록자가 write 가능, 비등록자는 read만. 단일 보유자 없음 (multi-writer cluster). */
	/* Exclusive Access - All Registrants */
	SPDK_SCSI_PR_EXCLUSIVE_ACCESS_ALL_REGS	= 0x08,
	/* [한국어] 모든 등록자가 read+write, 비등록자는 모두 거부. */
};

/* SCSI Persistent Reserve In header for
 * Read Keys, Read Reservation, Read Full Status
 */
/*
 * [한국어]
 * spdk_scsi_pr_in_read_header - PR_IN(READ_KEYS/READ_RESERVATION/READ_FULL_STATUS) 응답 공통 헤더 (8B).
 *
 * 응답의 가장 앞 8바이트로, 현재 generation과 뒤따르는 데이터 길이를 알린다.
 */
struct spdk_scsi_pr_in_read_header {
	/* persistent reservation generation */
	uint32_t pr_generation;
	/* [한국어] PR 상태가 변할 때마다 증가하는 카운터(Big-Endian uint32).
	 *         호스트가 PREEMPT 등 비교 수단으로 사용. */
	uint32_t additional_len;
	/* [한국어] 본 헤더 이후 데이터의 바이트 수 (Big-Endian uint32). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_scsi_pr_in_read_header) == 8, "Incorrect size");
/* [한국어] 8바이트 보장 (SPC-4 Table-209). */

/* SCSI Persistent Reserve In read keys data */
/*
 * [한국어]
 * spdk_scsi_pr_in_read_keys_data - READ_KEYS service action(0x00) 응답 본문.
 */
struct spdk_scsi_pr_in_read_keys_data {
	struct spdk_scsi_pr_in_read_header header;
	/* [한국어] 공통 헤더. additional_len = 8 * (등록 key 개수). */
	/* reservation key list */
	uint64_t rkeys[];
	/* [한국어] 등록된 모든 key의 가변 배열 (각 8B Big-Endian). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_scsi_pr_in_read_keys_data) == 8, "Incorrect size");

/* SCSI Persistent Reserve In read reservations data */
/*
 * [한국어]
 * spdk_scsi_pr_in_read_reservations_data - READ_RESERVATION service action(0x01) 응답 (총 24B).
 *
 * 활성 reservation이 있으면 additional_len=0x10, 없으면 0으로 보고하여 24B/8B 응답이 된다.
 */
struct spdk_scsi_pr_in_read_reservations_data {
	/* Fixed 0x10 with reservation and 0 for no reservation */
	struct spdk_scsi_pr_in_read_header header;
	/* [한국어] 헤더의 additional_len: 보유 시 0x10, 미보유 시 0. */
	/* reservation key */
	uint64_t rkey;
	/* [한국어] 활성 reservation의 key (Big-Endian). 미보유 시 의미 없음. */
	uint32_t obsolete1;
	/* [한국어] 구 SPC 호환 영역 — obsolete. 0으로 채움. */
	uint8_t reserved;
	/* [한국어] 예약. */
	uint8_t type  : 4;
	/* [한국어] reservation type (spdk_scsi_pr_type_code 값). */
	uint8_t scope : 4;
	/* [한국어] reservation scope (LU_SCOPE=0). */
	uint16_t obsolete2;
	/* [한국어] obsolete. 0으로 채움. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_scsi_pr_in_read_reservations_data) == 24, "Incorrect size");

/* SCSI Persistent Reserve In report capabilities data */
/*
 * [한국어]
 * spdk_scsi_pr_in_report_capabilities_data - REPORT_CAPABILITIES service action(0x02) 응답 (8B).
 *
 * SPDK가 어떤 PR 기능을 지원하는지 광고. SPC-4 Table-217 1:1 미러.
 * 비트맵은 little-endian C 컴파일러에서 wire 순서와 매치되도록 정렬되어 있다.
 */
struct spdk_scsi_pr_in_report_capabilities_data {
	/* Fixed value 0x8 */
	uint16_t length;
	/* [한국어] 본 응답 전체 크기 8 (Big-Endian uint16). */

	/* Persist through power loss capable */
	uint8_t ptpl_c    : 1;
	/* [한국어] 전원 손실에도 PR 상태 유지 가능 여부 광고 (capability). */
	uint8_t reserved1 : 1;
	/* [한국어] 예약. */
	/* All target ports capable */
	uint8_t atp_c     : 1;
	/* [한국어] ALL_TG_PT 비트 지원 여부. */
	/* Specify initiator port capable */
	uint8_t sip_c     : 1;
	/* [한국어] SPEC_I_PT(specify initiator ports) 지원. */
	/* Compatible reservation handing bit to indicate
	 * SPC-2 reserve/release is supported
	 */
	uint8_t crh       : 1;
	/* [한국어] 1이면 SPC-2 RESERVE/RELEASE도 함께 처리(호환 모드). */
	uint8_t reserved2 : 3;
	/* [한국어] 예약. */
	/* Persist through power loss activated */
	uint8_t ptpl_a    : 1;
	/* [한국어] PTPL 현재 활성 여부 (호스트가 활성화한 경우). */
	uint8_t reserved3 : 6;
	/* [한국어] 예약. */
	/* Type mask valid */
	uint8_t tmv       : 1;
	/* [한국어] 1이면 뒤따르는 type mask(wr_ex/ex_ac 등) 유효. */

	/* Type mask format */
	uint8_t reserved4 : 1;
	/* [한국어] 예약 (type 0x00). */
	/* Write Exclusive */
	uint8_t wr_ex     : 1;
	/* [한국어] WRITE_EXCLUSIVE(0x01) 지원. */
	uint8_t reserved5 : 1;
	/* [한국어] 예약. */
	/* Exclusive Access */
	uint8_t ex_ac     : 1;
	/* [한국어] EXCLUSIVE_ACCESS(0x03) 지원. */
	uint8_t reserved6 : 1;
	/* [한국어] 예약. */
	/* Write Exclusive - Registrants Only */
	uint8_t wr_ex_ro  : 1;
	/* [한국어] WR_EX_RO(0x05) 지원. */
	/* Exclusive Access - Registrants Only */
	uint8_t ex_ac_ro  : 1;
	/* [한국어] EX_AC_RO(0x06) 지원. */
	/* Write Exclusive - All Registrants */
	uint8_t wr_ex_ar  : 1;
	/* [한국어] WR_EX_AR(0x07) 지원. */
	/* Exclusive Access - All Registrants */
	uint8_t ex_ac_ar  : 1;
	/* [한국어] EX_AC_AR(0x08) 지원. */
	uint8_t reserved7 : 7;
	/* [한국어] 예약. */

	uint8_t reserved8[2];
	/* [한국어] 예약 2바이트 — 0으로 채움. 총 8바이트 보장. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_scsi_pr_in_report_capabilities_data) == 8, "Incorrect size");

/* SCSI Persistent Reserve In full status descriptor */
/*
 * [한국어]
 * spdk_scsi_pr_in_full_status_desc - READ_FULL_STATUS 응답의 단일 entry (24B 헤더 + transport_id).
 *
 * 각 등록자(initiator port + key) 별 하나씩 나열. SPC-4 Table-216.
 */
struct spdk_scsi_pr_in_full_status_desc {
	/* Reservation key */
	uint64_t rkey;
	/* [한국어] 이 등록자의 reservation key (Big-Endian). */
	uint8_t reserved1[4];
	/* [한국어] 예약 4바이트. */

	/* 0 - Registrant only
	 * 1 - Registrant and reservation holder
	 */
	uint8_t r_holder  : 1;
	/* [한국어] 이 entry가 reservation 보유자이기도 하면 1. (All Registrants type에서는 모두 1). */
	/* All target ports */
	uint8_t all_tg_pt : 1;
	/* [한국어] all target ports 영향 적용 여부. */
	uint8_t reserved2 : 6;
	/* [한국어] 예약 비트들. */

	/* Reservation type */
	uint8_t type      : 4;
	/* [한국어] reservation type (보유자 entry에만 의미). */
	/* Set to LU_SCOPE */
	uint8_t scope     : 4;
	/* [한국어] scope (LU_SCOPE=0). */

	uint8_t reserved3[4];
	/* [한국어] 예약 4바이트. */
	uint16_t relative_target_port_id;
	/* [한국어] 이 등록자가 등록한 target port의 상대 ID (Big-Endian). */
	/* Size of TransportID */
	uint32_t desc_len;
	/* [한국어] transport_id[] 의 바이트 수 (Big-Endian uint32). */

	uint8_t transport_id[];
	/* [한국어] initiator의 transport ID 가변 영역 (예: spdk_scsi_iscsi_transport_id 직렬화 결과). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_scsi_pr_in_full_status_desc) == 24, "Incorrect size");
/* [한국어] 헤더 정확히 24바이트. */

/* SCSI Persistent Reserve In full status data */
/*
 * [한국어]
 * spdk_scsi_pr_in_full_status_data - READ_FULL_STATUS service action(0x03) 응답 컨테이너.
 */
struct spdk_scsi_pr_in_full_status_data {
	struct spdk_scsi_pr_in_read_header header;
	/* [한국어] 공통 헤더 (pr_generation + 뒤이은 desc_list 길이). */
	/* Full status descriptors */
	struct spdk_scsi_pr_in_full_status_desc desc_list[];
	/* [한국어] 모든 등록자의 entry 가변 배열 (각 entry 자체도 가변 — desc_len에 따라 stride 계산). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_scsi_pr_in_full_status_data) == 8, "Incorrect size");
/* [한국어] 헤더 8바이트만 정적 보장. */

/* SCSI Persistent Reserve Out service action codes */
/*
 * [한국어]
 * spdk_scsi_pr_out_service_action_code - PERSISTENT_RESERVE_OUT(0x5f) CDB의 service action 코드 (SPC-4 §6.14).
 *
 * cdb[1] 의 하위 5비트로 8가지 PR 변경 동작을 디스패치. 클러스터 fencing 절차의 핵심.
 */
enum spdk_scsi_pr_out_service_action_code {
	/* Register/unregister a reservation key */
	SPDK_SCSI_PR_OUT_REGISTER		= 0x00,
	/* [한국어] reservation key 등록 또는 해제. rkey=0 이면 등록, sa_rkey=0이면 해제.
	 *         초기 fencing 단계에서 모든 노드가 발행. */
	/* Create a persistent reservation */
	SPDK_SCSI_PR_OUT_RESERVE		= 0x01,
	/* [한국어] 등록된 key로 reservation 생성 (type 지정). */
	/* Release a persistent reservation */
	SPDK_SCSI_PR_OUT_RELEASE		= 0x02,
	/* [한국어] 보유 중인 reservation 해제. */
	/* Clear all reservation keys and persistent reservations */
	SPDK_SCSI_PR_OUT_CLEAR			= 0x03,
	/* [한국어] 모든 등록과 reservation을 일괄 삭제 (관리자 reset 용). */
	/* Preempt persistent reservations and/or remove registrants */
	SPDK_SCSI_PR_OUT_PREEMPT		= 0x04,
	/* [한국어] 다른 노드의 reservation을 강제로 빼앗음. failover 시 새 마스터가 발행 — 클러스터 fencing의 핵심. */
	/* Preempt persistent reservations and or remove registrants
	 * and abort all tasks for all preempted I_T nexuses
	 */
	SPDK_SCSI_PR_OUT_PREEMPT_AND_ABORT	= 0x05,
	/* [한국어] PREEMPT + 빼앗긴 노드의 모든 in-flight task를 abort. split-brain 방지에 필수. */
	/* Register/unregister a reservation key based on the ignore bit */
	SPDK_SCSI_PR_OUT_REG_AND_IGNORE_KEY	= 0x06,
	/* [한국어] 기존 key 검증 없이 강제 등록. 노드 재부팅 후 stale 등록 무시 시. */
	/* Register a reservation key for another I_T nexus
	 * and move a persistent reservation to that I_T nexus
	 */
	SPDK_SCSI_PR_OUT_REG_AND_MOVE		= 0x07,
	/* [한국어] 다른 I_T nexus(다른 initiator)에 reservation을 이전. 별도 파라미터 list 사용. */
	/* 0x08 - 0x1f Reserved */
};

/* SCSI Persistent Reserve Out parameter list */
/*
 * [한국어]
 * spdk_scsi_pr_out_param_list - PR_OUT 명령의 파라미터 데이터 (24B 헤더 + 가변).
 *
 * REGISTER/RESERVE/RELEASE/CLEAR/PREEMPT 등 대부분의 service action에 사용. SPC-4 Table-220.
 */
struct spdk_scsi_pr_out_param_list {
	/* Reservation key */
	uint64_t rkey;
	/* [한국어] 호출자가 보유한 현재 key (등록 검증용, Big-Endian). */
	/* Service action reservation key */
	uint64_t sa_rkey;
	/* [한국어] action마다 의미 다름:
	 *         REGISTER 시 새 key, PREEMPT 시 빼앗을 대상 key, RELEASE/CLEAR는 보통 0. */
	uint8_t obsolete1[4];
	/* [한국어] 구 SPC scope-specific address. obsolete — 0으로 채움. */

	/* Active persist through power loss */
	uint8_t aptpl     : 1;
	/* [한국어] 1이면 PR 상태를 비휘발 저장. lib/scsi/scsi_pr_persist.c가 파일에 저장. */
	uint8_t reserved1 : 1;
	/* [한국어] 예약. */
	/* All target ports */
	uint8_t all_tg_pt : 1;
	/* [한국어] 1이면 target의 모든 port에 등록 적용. */
	/* Specify initiator ports */
	uint8_t spec_i_pt : 1;
	/* [한국어] 1이면 param_data[] 에 명시된 initiator들에 대해서만 적용. */
	uint8_t reserved2 : 4;
	/* [한국어] 예약. */

	uint8_t reserved3;
	/* [한국어] 예약 바이트. */
	uint16_t obsolete2;
	/* [한국어] obsolete. */

	uint8_t param_data[];
	/* [한국어] spec_i_pt=1 시 transport ID list 등 추가 파라미터 (가변). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_scsi_pr_out_param_list) == 24, "Incorrect size");
/* [한국어] 헤더 24바이트 보장. */

/*
 * [한국어]
 * spdk_scsi_pr_out_reg_and_move_param_list - REG_AND_MOVE service action(0x07) 전용 파라미터 list (SPC-4 Table-225).
 *
 * 다른 I_T nexus로 reservation을 이전할 때 사용. 일반 param_list와는 레이아웃이 다르다(unreg 비트, target_port_id 포함).
 */
struct spdk_scsi_pr_out_reg_and_move_param_list {
	/* Reservation key */
	uint64_t rkey;
	/* [한국어] 호출자의 현재 key (검증용). */
	/* Service action reservation key */
	uint64_t sa_rkey;
	/* [한국어] 이전 후 새 보유자가 사용할 key. */
	uint8_t reserved1;
	/* [한국어] 예약. */

	/* Active persist through power loss */
	uint8_t aptpl     : 1;
	/* [한국어] 비휘발 저장 활성화. */
	/* Unregister */
	uint8_t unreg     : 1;
	/* [한국어] 1이면 호출자의 등록은 해제하면서 reservation만 이전. */
	uint8_t reserved2 : 6;
	/* [한국어] 예약. */

	uint16_t relative_target_port_id;
	/* [한국어] 새 보유자의 target port 상대 ID (Big-Endian). */
	/* TransportID parameter data length */
	uint32_t transport_id_len;
	/* [한국어] transport_id[] 의 바이트 수. */
	uint8_t transport_id[];
	/* [한국어] 새 보유자가 될 initiator의 transport ID. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_scsi_pr_out_reg_and_move_param_list) == 24, "Incorrect size");
/* [한국어] 헤더 24바이트 보장. */

/*
 * SPC-4
 * Table-258 SECURITY PROTOCOL field in SECURITY PROTOCOL IN command
 */
/* [한국어] SECURITY PROTOCOL 필드 - INFO(0x00): 지원 protocol 목록 조회용 메타.
 *         SECURITY PROTOCOL IN(0xa2)/OUT(0xb5) 명령의 cdb[1] 에 들어감. SPDK 미지원이지만 상수만 정의. */
#define SPDK_SCSI_SECP_INFO	0x00
/* [한국어] SECURITY PROTOCOL 필드 - TCG(0x01): TCG Storage spec (Opal SED 등). */
#define SPDK_SCSI_SECP_TCG	0x01

/* [한국어] VPD 0xB2 Logical Block Provisioning의 LBPU 비트:
 *         UNMAP(0x42) 명령 지원 광고. 1이면 미들레이어가 UNMAP 응답을 처리. */
#define SPDK_SCSI_UNMAP_LBPU			1 << 7
/* [한국어] VPD 0xB2 LBPWS 비트: WRITE_SAME_16(0x93)에 UNMAP 비트 지원 광고. */
#define SPDK_SCSI_UNMAP_LBPWS			1 << 6
/* [한국어] VPD 0xB2 LBPWS10 비트: WRITE_SAME_10(0x41)에 UNMAP 비트 지원 광고. */
#define SPDK_SCSI_UNMAP_LBPWS10			1 << 5

/* [한국어] VPD 0xB2 provisioning type 필드 - 0x00: full provisioning (thick).
 *         deallocate 미지원, 모든 LBA가 항상 매핑된 상태. */
#define SPDK_SCSI_UNMAP_FULL_PROVISIONING	0x00
/* [한국어] provisioning type - 0x01: resource provisioning. */
#define SPDK_SCSI_UNMAP_RESOURCE_PROVISIONING	0x01
/* [한국어] provisioning type - 0x02: thin provisioning. lvol/sparse bdev에서 사용. */
#define SPDK_SCSI_UNMAP_THIN_PROVISIONING	0x02

#ifdef __cplusplus
}
#endif

#endif /* SPDK_SCSI_SPEC_H */
