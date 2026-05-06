/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * iSCSI specification definitions
 */

/*
 * [한국어 설명] iSCSI 프로토콜 와이어 포맷 정의 (iscsi_spec.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 RFC 7143(구 RFC 3720) 의 iSCSI 프로토콜에서 사용되는 PDU(Protocol
 * Data Unit) 와이어 포맷을 C 구조체로 1:1 매핑하여 정의하는 헤더이다.
 * iSCSI 는 SCSI 명령(CDB)을 TCP 연결 위로 캡슐화하여 IP 네트워크를 통해
 * 블록 스토리지를 노출하는 프로토콜이며, SPDK 의 iSCSI target (lib/iscsi)
 * 구현은 이 헤더의 구조체 레이아웃에 직접 캐스팅하여 송수신 버퍼를 해석한다.
 * BHS(Basic Header Segment, 48B 고정), AHS(Additional Header Segment, 가변),
 * Header Digest(4B optional CRC32C), Data Segment, Pad, Data Digest(4B optional)
 * 의 PDU 레이아웃 중 본 헤더는 BHS 부분과 일부 매크로/플래그 정의를 담당한다.
 * 또한 Login negotiation 의 status class/detail, Reject 사유 코드, SNACK 타입
 * 등 프로토콜 enum 도 함께 정의한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK iSCSI target 의 PDU 처리 파이프라인은 다음과 같이 흐른다:
 *   socket(read) → BHS 48B 수신 → AHS 길이만큼 추가 수신 → Header Digest 검증
 *   → Data Segment + Pad + Data Digest 수신 → opcode 디스패치 → SCSI 레이어
 *   → bdev I/O 발행 → 완료 시 SCSI Response/Data-In PDU 송신.
 * 이 헤더에 정의된 구조체는 위 단계 중 "BHS 디코딩" 과 "응답 PDU 생성" 양쪽
 * 모두에서 사용된다. 호스트 컨텍스트는 SPDK reactor 스레드(특정 코어 고정)
 * 에서 polled-mode 로 동작하므로, 본 헤더의 구조체들은 SPDK thread 내부에서
 * lockless 하게 다뤄진다(연결 단위 affinity).
 * 이 파일은 와이어 포맷만 다루므로 어떤 .c 파일도 호출하지 않으며, 반대로
 * lib/iscsi/*.c 와 module/event/subsystems/iscsi/* 가 본 헤더를 include 한다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/iscsi/iscsi.c, conn.c, login.c, task.c, tgt_node.c 가 PDU 파싱과
 *   응답 생성을 위해 본 헤더를 직접 사용한다.
 * - spdk/scsi.h (SCSI CDB/Sense 정의) 와 함께 쓰여 SCSI 레이어로 디스패치된다.
 * - spdk/sock.h 가 제공하는 비동기 소켓 I/O 위에서 PDU 단위 송수신이 일어난다.
 * - spdk/assert.h 의 SPDK_STATIC_ASSERT 로 BHS 길이 48B 를 컴파일 타임에 검증.
 * 데이터 흐름: 클라이언트(initiator) TCP → 본 구조체로 캐스팅 후 필드 추출 →
 * SCSI CDB 추출 → bdev I/O → 완료 → 응답 PDU 작성 → TCP 송신.
 * 공유 자료구조: 한 PDU 의 ITT(Initiator Task Tag) 는 명령 전 라이프사이클
 * 동안 task lookup 의 키로 쓰여 spdk_iscsi_task 와 페어링된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct iscsi_bhs               : 모든 PDU 의 공통 48B 기본 헤더 베이스 구조체
 * - struct iscsi_bhs_login_req/rsp : 로그인 요청/응답 (Security/Operational/FullFeature 단계 협상)
 * - struct iscsi_bhs_scsi_req/resp : SCSI 명령(16B CDB 임베드)/응답
 * - struct iscsi_bhs_data_in/out   : 솔리시티드/언솔리시티드 데이터 전송
 * - struct iscsi_bhs_r2t           : Ready-To-Transfer (target 이 initiator 에게 데이터 송신 허가)
 * - struct iscsi_bhs_nop_in/out    : 핑/keepalive
 * - struct iscsi_bhs_async         : target 발 비동기 메시지 (LU 변경, 로그아웃 요청 등)
 * - struct iscsi_bhs_reject        : 프로토콜 위반 PDU 거부
 * - struct iscsi_ahs               : Additional Header Segment (확장 CDB, Bidi expected length)
 * - enum iscsi_op                  : 모든 PDU opcode (initiator 0x00-0x1f / target 0x20-0x3f)
 * - ISCSI_BHS_LOGIN_GET_*BIT/CSG/NSG: 로그인 PDU 의 T/C 비트, CSG/NSG 단계 추출 매크로
 */

#ifndef SPDK_ISCSI_SPEC_H
#define SPDK_ISCSI_SPEC_H

#include "spdk/stdinc.h"
/* [한국어] 표준 C 헤더 묶음 (stdint.h, stddef.h, stdbool.h 등).
 * iSCSI BHS 가 정확한 폭의 정수형(uint8_t/uint16_t/uint32_t/uint64_t)을
 * 요구하므로 stdint.h 에서 제공하는 fixed-width 타입이 필수적이다. */

#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT 매크로 제공.
 * BHS 가 정확히 48 바이트(ISCSI_BHS_LEN) 임을 컴파일 타임에 검증하기 위해
 * 사용한다. 컴파일러가 패딩을 잘못 넣으면 빌드가 실패하도록 강제한다. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 컴파일러가 본 헤더를 include 할 때 C 링크 규칙을 적용하도록
 * extern "C" 블록을 연다. SPDK 는 C 라이브러리이지만 C++ 사용자도 호출 가능. */
#endif

#define ISCSI_BHS_LEN 48
/* [한국어] iSCSI Basic Header Segment 의 고정 길이 (RFC 7143 §11.1).
 * 모든 PDU 는 정확히 48B 의 BHS 로 시작한다. 수신 단계에서 우선 48B 만
 * 읽어 opcode 와 AHS/Data 길이 필드를 확인한 뒤 후속 데이터 길이를 결정한다. */

#define ISCSI_DIGEST_LEN 4
/* [한국어] Header/Data Digest 의 길이 (CRC32C, 4B). RFC 7143 §11.1 / §10.6.
 * Login negotiation 의 HeaderDigest=CRC32C, DataDigest=CRC32C 가 합의되면
 * 각각 BHS+AHS 뒤, Data Segment+Pad 뒤에 4B CRC32C 가 붙는다. */

#define ISCSI_ALIGNMENT 4
/* [한국어] iSCSI 의 모든 길이 필드는 4B 정렬이 강제된다 (RFC 7143 §11.1).
 * Data Segment 가 4B 의 배수가 아니면 Pad 바이트를 0 으로 채워 정렬한다. */

/** support version - RFC3720(10.12.4) */
#define ISCSI_VERSION 0x00
/* [한국어] iSCSI 프로토콜 버전 (현재 0x00 만 정의). Login PDU 의
 * version_max/version_min/version_act 필드 비교에 사용된다. 미래 확장을 위해
 * 필드는 8 비트로 두지만 현재 스펙은 0 만 유효 — 다른 값은 거부 대상. */

#define ISCSI_ALIGN(SIZE) \
	(((SIZE) + (ISCSI_ALIGNMENT - 1)) & ~(ISCSI_ALIGNMENT - 1))
/* [한국어] 임의의 SIZE 를 ISCSI_ALIGNMENT(4B) 의 배수로 올림(round-up).
 * Data Segment 길이 그대로 + 4B align 한 차이만큼 Pad 바이트가 필요하다.
 * 비트 트릭: (x + 3) & ~3 → 2 의 거듭제곱 정렬에 표준 패턴. */

/** for authentication key (non encoded 1024bytes) RFC3720(5.1/11.1.4) */
#define ISCSI_TEXT_MAX_VAL_LEN 8192
/* [한국어] CHAP 등 인증 키-값의 최대 길이(인코딩 전 raw 8KB).
 * Login/Text PDU 의 key=value 페어 중 인증 부분에서 base64/hex 인코딩 후
 * 더 길어질 수 있어 8192 로 잡아둠. 일반 단순 값은 ISCSI_TEXT_MAX_SIMPLE_VAL_LEN 사용. */

/**
 * RFC 3720 5.1
 * If not otherwise specified, the maximum length of a simple-value
 * (not its encoded representation) is 255 bytes, not including the delimiter
 * (comma or zero byte).
 */
#define ISCSI_TEXT_MAX_SIMPLE_VAL_LEN 255
/* [한국어] 일반(simple) 키-값에서 값의 최대 길이 (RFC 7143 §6.2).
 * key=value\0 형태로 인코딩되며, 콤마(,)/널(0) 종결자는 포함하지 않는다.
 * 예: "MaxRecvDataSegmentLength=8192" 의 우측 값. */

#define ISCSI_TEXT_MAX_KEY_LEN 63
/* [한국어] 키 이름의 최대 길이 (RFC 7143 §6.2). 표준 키 명칭(HeaderDigest,
 * MaxRecvDataSegmentLength 등)은 이 한도 안에 들어온다. */

enum iscsi_op {
	/* Initiator opcodes */
	/* [한국어] === Initiator → Target 방향 PDU opcode 그룹 (0x00-0x1f) ===
	 * 최상위 비트가 0 인 opcode 는 initiator 가 보내는 PDU 임을 의미.
	 * 설정자: lib/iscsi 송신 경로(initiator 라이브러리) 또는 PDU 수신 후
	 *         파싱 시 BHS 의 opcode 비트 6개에서 추출.
	 * 읽는 자: PDU 디스패치 핸들러 테이블의 인덱스로 사용. */
	ISCSI_OP_NOPOUT         = 0x00,
	/* [한국어] NOP-Out PDU. initiator 가 보내는 ping/keepalive 또는
	 * StatSN ack 운반자. RFC 7143 §11.18. ttt=0xffffffff 면 단순 ping,
	 * 그 외에는 target 의 NOP-In 에 대한 응답. */
	ISCSI_OP_SCSI           = 0x01,
	/* [한국어] SCSI Command PDU. 16B CDB 임베드. RFC 7143 §11.2.
	 * read_bit/write_bit/expected_data_xfer_len 으로 데이터 방향과 양 결정. */
	ISCSI_OP_TASK           = 0x02,
	/* [한국어] SCSI Task Management Function Request. RFC 7143 §11.5.
	 * AbortTask, AbortTaskSet, LUN Reset 등을 요청. iscsi_task_func 참조. */
	ISCSI_OP_LOGIN          = 0x03,
	/* [한국어] Login Request. RFC 7143 §11.12. CSG/NSG 비트로 단계 협상
	 * (Security → Operational → FullFeature). key=value 텍스트가 데이터에. */
	ISCSI_OP_TEXT           = 0x04,
	/* [한국어] Text Request. RFC 7143 §11.10. SendTargets= 등으로 디스커버리
	 * 또는 풀피처 단계에서 파라미터 추가 협상. */
	ISCSI_OP_SCSI_DATAOUT   = 0x05,
	/* [한국어] SCSI Data-Out PDU. WRITE 명령에 대한 데이터 전송.
	 * Unsolicited(R2T 없이 첫 ImmediateData/FirstBurst) 또는 Solicited(R2T 후). */
	ISCSI_OP_LOGOUT         = 0x06,
	/* [한국어] Logout Request. 세션/연결 종료. reason 코드에 따라
	 * 정상 close, connection recovery, recovery 의 의미가 달라진다. */
	ISCSI_OP_SNACK          = 0x10,
	/* [한국어] SNACK Request — Selective Negative Acknowledge. RFC 7143 §11.16.
	 * Status/Data/R2T 재전송 요청. ErrorRecoveryLevel >=1 에서 사용. */
	ISCSI_OP_VENDOR_1C      = 0x1c,
	/* [한국어] Vendor specific opcode (initiator 측 reserved 영역). */
	ISCSI_OP_VENDOR_1D      = 0x1d,
	/* [한국어] Vendor specific opcode. */
	ISCSI_OP_VENDOR_1E      = 0x1e,
	/* [한국어] Vendor specific opcode. */

	/* Target opcodes */
	/* [한국어] === Target → Initiator 방향 PDU opcode 그룹 (0x20-0x3f) ===
	 * 최상위 비트(0x20)가 1 인 opcode = target 발신.
	 * SPDK iSCSI target 송신 경로에서 BHS 채울 때 이 값들을 사용. */
	ISCSI_OP_NOPIN          = 0x20,
	/* [한국어] NOP-In. target 발 ping 또는 ExpCmdSN 갱신 운반자.
	 * itt=0xffffffff 면 일방적 알림으로 ack 불필요. */
	ISCSI_OP_SCSI_RSP       = 0x21,
	/* [한국어] SCSI Response. SCSI status 와 sense data, residual count 운반. */
	ISCSI_OP_TASK_RSP       = 0x22,
	/* [한국어] SCSI Task Management Function Response. */
	ISCSI_OP_LOGIN_RSP      = 0x23,
	/* [한국어] Login Response. status_class/status_detail 로 결과 통보. */
	ISCSI_OP_TEXT_RSP       = 0x24,
	/* [한국어] Text Response. 협상 응답 또는 SendTargets 결과. */
	ISCSI_OP_SCSI_DATAIN    = 0x25,
	/* [한국어] SCSI Data-In PDU. READ 명령의 데이터 + (옵션) phase-collapsed status. */
	ISCSI_OP_LOGOUT_RSP     = 0x26,
	/* [한국어] Logout Response. response 필드로 success/cleanup 결과. */
	ISCSI_OP_R2T            = 0x31,
	/* [한국어] Ready To Transfer. target 이 initiator 에게 WRITE 데이터 일부를
	 * 보내라고 허가. buffer_offset/desired_xfer_len 으로 윈도우 지정. */
	ISCSI_OP_ASYNC          = 0x32,
	/* [한국어] Asynchronous Message. target 이 자발적으로 보내는 알림
	 * (LU 변경, 강제 로그아웃, 협상 리셋 등). RFC 7143 §11.9. */
	ISCSI_OP_VENDOR_3C      = 0x3c,
	/* [한국어] Vendor specific opcode (target 측). */
	ISCSI_OP_VENDOR_3D      = 0x3d,
	/* [한국어] Vendor specific opcode. */
	ISCSI_OP_VENDOR_3E      = 0x3e,
	/* [한국어] Vendor specific opcode. */
	ISCSI_OP_REJECT         = 0x3f,
	/* [한국어] Reject. target 이 받은 PDU 가 프로토콜 위반/디지스트 에러
	 * 등으로 처리 불가일 때 원본 BHS 를 첨부해 거부. */
};

enum iscsi_task_func {
	/* [한국어] SCSI Task Management Function 코드 (RFC 7143 §11.5).
	 * iscsi_bhs_task_req 의 flags 하위 7비트(ISCSI_TASK_FUNCTION_MASK)로 운반. */
	ISCSI_TASK_FUNC_ABORT_TASK = 1,
	/* [한국어] 단일 태스크 abort. ref_task_tag 가 가리키는 ITT 만 취소. */
	ISCSI_TASK_FUNC_ABORT_TASK_SET = 2,
	/* [한국어] 해당 LUN 의 태스크 셋 전체 abort. */
	ISCSI_TASK_FUNC_CLEAR_ACA = 3,
	/* [한국어] ACA(Auto Contingent Allegiance) 상태 클리어. */
	ISCSI_TASK_FUNC_CLEAR_TASK_SET = 4,
	/* [한국어] LUN 의 모든 태스크 정리. */
	ISCSI_TASK_FUNC_LOGICAL_UNIT_RESET = 5,
	/* [한국어] 단일 LU 리셋. */
	ISCSI_TASK_FUNC_TARGET_WARM_RESET = 6,
	/* [한국어] 타깃 전체 warm reset (세션 유지하며 LU 리셋). */
	ISCSI_TASK_FUNC_TARGET_COLD_RESET = 7,
	/* [한국어] 타깃 전체 cold reset (모든 세션/연결 강제 종료). */
	ISCSI_TASK_FUNC_TASK_REASSIGN = 8,
	/* [한국어] 태스크 재할당 (ErrorRecoveryLevel=2 의 connection recovery). */
};

enum iscsi_task_func_resp {
	/* [한국어] Task Management Response 의 response 필드 코드.
	 * iscsi_bhs_task_resp.response 에 직접 들어간다. RFC 7143 §11.6. */
	ISCSI_TASK_FUNC_RESP_COMPLETE = 0,
	/* [한국어] 함수 정상 완료. */
	ISCSI_TASK_FUNC_RESP_TASK_NOT_EXIST = 1,
	/* [한국어] ref_task_tag 로 지정한 태스크가 존재하지 않음. */
	ISCSI_TASK_FUNC_RESP_LUN_NOT_EXIST = 2,
	/* [한국어] 지정한 LUN 미존재. */
	ISCSI_TASK_FUNC_RESP_TASK_STILL_ALLEGIANT = 3,
	/* [한국어] 태스크가 아직 다른 연결에 attached. */
	ISCSI_TASK_FUNC_RESP_REASSIGNMENT_NOT_SUPPORTED = 4,
	/* [한국어] Task Reassign 미지원 (ErrorRecoveryLevel < 2 등). */
	ISCSI_TASK_FUNC_RESP_FUNC_NOT_SUPPORTED = 5,
	/* [한국어] 해당 TM 함수 자체 미지원. */
	ISCSI_TASK_FUNC_RESP_AUTHORIZATION_FAILED = 6,
	/* [한국어] 권한 없음 (예: 다른 initiator 의 태스크에 abort 시도). */
	ISCSI_TASK_FUNC_REJECTED = 255
	/* [한국어] 일반적으로 거부됨. (255 는 SPDK 확장; 표준은 reserved.) */
};

struct iscsi_bhs {
	/* [한국어] 모든 PDU 에 공통으로 적용되는 Basic Header Segment 의 일반화
	 * 형태. 특정 opcode 별 BHS(아래 iscsi_bhs_login_req 등)는 같은 48B 영역을
	 * 다른 의미로 reinterpret 한다. SPDK 코드는 일단 이 일반 구조체로 BHS 를
	 * 받아서 opcode 를 본 뒤 알맞는 하위 구조체로 캐스팅한다.
	 * 동기화: PDU 단위로 한 연결의 단일 reactor 스레드만 다루므로 락 불필요. */
	uint8_t opcode		: 6;
	/* [한국어] PDU opcode 6비트. enum iscsi_op 값과 1:1 매핑.
	 * 설정자: PDU 송신자가 채움. 읽는 자: 수신자 PDU dispatcher.
	 * 값 범위: 0x00-0x3f. 비트필드 순서는 little-endian 전제(SPDK 지원 ABI).
	 * 동기화: PDU 단위 immutable — 디코딩 후 변경 금지. */

	uint8_t immediate	: 1;
	/* [한국어] Immediate(I) 비트. 1 이면 CmdSN 큐를 무시하고 즉시 처리해야 함
	 * (Login, Logout, NOP-Out, Task Management 등에서 사용). RFC 7143 §11.1. */

	uint8_t reserved	: 1;
	/* [한국어] 예약 비트. 송신 시 0, 수신 시 무시. */

	uint8_t flags;
	/* [한국어] PDU 타입별 플래그 바이트. 의미는 opcode 마다 상이.
	 * SCSI Command 에서는 ATTR/R/W/F 비트, Login 에서는 T/C/CSG/NSG, 등. */

	uint8_t rsv[2];
	/* [한국어] 일부 PDU 에서는 status/version 등으로 reinterpret 됨.
	 * 일반화 BHS 에서는 reserved 로 표기. */

	uint8_t total_ahs_len;
	/* [한국어] AHS(Additional Header Segment) 의 총 길이를 4B 단위로 표현.
	 * 실제 바이트 수 = total_ahs_len * 4. 대부분 PDU 는 0.
	 * 읽는 자: PDU 수신기가 BHS 뒤에 더 읽을 바이트 양을 결정할 때 사용. */

	uint8_t data_segment_len[3];
	/* [한국어] Data Segment 의 길이(빅엔디안 24비트, 0..16M-1 바이트).
	 * 실제 바이트 수 = network-byte-order 24bit 정수.
	 * 4B 정렬을 위해 이 값을 ISCSI_ALIGN 한 길이만큼 Pad 가 추가된다. */

	uint64_t lun;
	/* [한국어] Logical Unit Number (8B SAM-3 LUN format).
	 * 첫 16비트는 method-specific addressing, 나머지는 device-specific.
	 * 일부 PDU(NOP-Out, Login 등)에서는 reserved/ttt 와 reinterpret. */

	uint32_t itt;
	/* [한국어] Initiator Task Tag. initiator 가 명령을 식별하기 위해 부여한 32비트.
	 * target 은 응답 PDU 에 그대로 echo back 하여 task 매칭에 쓴다.
	 * 0xffffffff 는 "no task" 를 의미 (NOP-In, Async 등). */

	uint32_t ttt;
	/* [한국어] Target Transfer Tag. target 이 R2T/Data-In 에서 부여한 태그.
	 * initiator 는 후속 Data-Out 에서 동일 ttt 를 echo. 0xffffffff = none. */

	uint32_t stat_sn;
	/* [한국어] Status Sequence Number. target 이 status PDU 마다 1 증가시키는
	 * 단조 증가 카운터. initiator 는 ExpStatSN 으로 ack. 손실 검출에 쓰임. */

	uint32_t exp_stat_sn;
	/* [한국어] Expected StatSN — initiator 가 다음에 받을 것으로 기대하는 stat_sn.
	 * 이를 통해 target 은 어디까지 ack 됐는지 확인하고 재전송 버퍼를 비울 수 있다. */

	uint32_t max_stat_sn;
	/* [한국어] 일반 BHS 슬롯 명칭. 실제 PDU 별로 max_cmd_sn / data_sn / 등으로
	 * 다른 의미를 가진다. SCSI Response 에서는 max_cmd_sn 의 자리.  */

	uint8_t res3[12];
	/* [한국어] PDU 타입별 가변 영역 12B. PDU 구체화 BHS(아래)에서 의미를 가진다. */
};
SPDK_STATIC_ASSERT(sizeof(struct iscsi_bhs) == ISCSI_BHS_LEN, "ISCSI_BHS_LEN mismatch");
/* [한국어] BHS 가 정확히 48B 임을 컴파일 타임에 검증.
 * 컴파일러 패딩이 끼어들거나 비트필드 처리가 다른 ABI 면 빌드 실패. */

struct iscsi_bhs_async {
	/* [한국어] Async Message PDU (opcode 0x32). target 이 자발적으로 보내는
	 * 비요청 알림. 예: 강제 로그아웃 요청, 협상 reset, 드라이브 결함 알림.
	 * RFC 7143 §11.9. ITT 는 항상 0xffffffff. */
	uint8_t opcode		: 6;	/* opcode = 0x32 */
	/* [한국어] 항상 0x32 (ISCSI_OP_ASYNC). */
	uint8_t reserved	: 2;
	/* [한국어] Reserved 2 비트. */
	uint8_t flags;
	/* [한국어] 최상위 F 비트(0x80) 항상 1. 나머지 reserved. */
	uint8_t res[2];
	/* [한국어] Reserved. */

	uint8_t total_ahs_len;
	/* [한국어] AHS 길이 (4B 단위). 보통 0. */
	uint8_t data_segment_len[3];
	/* [한국어] Sense data 등 디테일 페이로드 길이. */

	uint64_t lun;
	/* [한국어] 영향을 받는 LUN. async_event 가 LU 단위일 때만 의미가짐. */
	uint32_t ffffffff;
	/* [한국어] ITT 자리 — Async 는 task 와 무관하므로 0xffffffff 고정. */
	uint32_t res3;
	/* [한국어] Reserved. */
	uint32_t stat_sn;
	/* [한국어] target 의 다음 status sequence number. */
	uint32_t exp_cmd_sn;
	/* [한국어] target 이 기대하는 다음 CmdSN (initiator → target). */
	uint32_t max_cmd_sn;
	/* [한국어] initiator 가 보낼 수 있는 최대 CmdSN (윈도우 상한). */
	uint8_t async_event;
	/* [한국어] Async event 코드 (0=SCSI, 1=request logout, 2=connection drop,
	 * 3=session drop, 4=request renegotiation, 5=vendor) — RFC 7143 §11.9.1. */
	uint8_t async_vcode;
	/* [한국어] Vendor 전용 sub-event 코드 (async_event=5 일 때만 의미). */
	uint16_t param1;
	/* [한국어] event-specific 파라미터 1 (예: connection drop 시 CID). */
	uint16_t param2;
	/* [한국어] event-specific 파라미터 2 (예: Time2Wait). */
	uint16_t param3;
	/* [한국어] event-specific 파라미터 3 (예: Time2Retain). */
	uint8_t res4[4];
	/* [한국어] Reserved. */
};

struct iscsi_bhs_login_req {
	/* [한국어] Login Request PDU (opcode 0x03). 세션/연결 수립 시
	 * Security → Operational → FullFeature 단계 협상을 위해 사용.
	 * 데이터 세그먼트는 key=value\0 텍스트 시퀀스. RFC 7143 §11.12. */
	uint8_t opcode		: 6;	/* opcode = 0x03 */
	uint8_t immediate	: 1;
	/* [한국어] Login PDU 는 항상 immediate=1. CmdSN 큐 우회. */
	uint8_t reserved	: 1;
	uint8_t flags;
	/* [한국어] T/C/CSG/NSG 비트:
	 *   bit7(T=Transit): 다음 단계로 이행 요청
	 *   bit6(C=Continue): 키-값이 다음 PDU 에 이어짐
	 *   bit3-2(CSG): Current Stage (0=Sec,1=Oper,3=FullFeat)
	 *   bit1-0(NSG): Next Stage (T=1 일 때 의미). */
	uint8_t version_max;
	/* [한국어] initiator 가 지원하는 최대 iSCSI 버전 (현재 0x00). */
	uint8_t version_min;
	/* [한국어] initiator 가 지원하는 최소 iSCSI 버전. */
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint8_t isid[6];
	/* [한국어] Initiator Session ID (6B). 동일 initiator/target 쌍의 다중 세션
	 * 구분에 쓰이며 (T,A,B,C,D) 형식. RFC 7143 §11.12.5. */
	uint16_t tsih;
	/* [한국어] Target Session Identifying Handle. New session 시 0,
	 * 후속 connection 추가 시 target 이 첫 응답에서 부여한 값을 사용. */
	uint32_t itt;
	/* [한국어] Initiator Task Tag — 이 Login PDU 식별자. */
	uint16_t cid;
	/* [한국어] Connection ID. 한 세션 내 다중 connection(MC/S) 구분. */
	uint16_t res2;
	uint32_t cmd_sn;
	/* [한국어] CmdSN. Login 에서는 SerialZero(원하는 시작값)를 운반. */
	uint32_t exp_stat_sn;
	/* [한국어] Expected StatSN. 첫 Login 은 0. */
	uint8_t res3[16];
};

struct iscsi_bhs_login_rsp {
	/* [한국어] Login Response PDU (opcode 0x23). target 이 Login 요청에 대한
	 * 결과를 status_class/status_detail 로 통보. RFC 7143 §11.13. */
	uint8_t opcode		: 6;	/* opcode = 0x23 */
	uint8_t reserved	: 2;
	uint8_t flags;
	/* [한국어] T/C/CSG/NSG 비트 — 단계 협상 결과를 반영. */
	uint8_t version_max;
	/* [한국어] target 의 최대 지원 버전. */
	uint8_t version_act;
	/* [한국어] 실제 활성화된 버전 (초기 이후 변경 없음). */
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint8_t isid[6];
	/* [한국어] initiator 가 보낸 ISID 를 echo. */
	uint16_t tsih;
	/* [한국어] target 이 새 세션에 부여한 TSIH. 후속 connection 에서 사용. */
	uint32_t itt;
	/* [한국어] 요청 PDU 의 ITT echo. */
	uint32_t res2;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint8_t status_class;
	/* [한국어] 결과 분류:
	 *   0=Success, 1=Redirection, 2=Initiator Error, 3=Target Error.
	 *   ISCSI_CLASS_* 매크로 참조. */
	uint8_t status_detail;
	/* [한국어] status_class 별 세부 코드. ISCSI_LOGIN_* 매크로 참조. */
	uint8_t res3[10];
};

struct iscsi_bhs_logout_req {
	/* [한국어] Logout Request PDU (opcode 0x06). 세션/연결을 정상 종료하거나
	 * connection recovery 를 요청. RFC 7143 §11.14. */
	uint8_t opcode		: 6;	/* opcode = 0x06 */
	uint8_t immediate	: 1;
	uint8_t reserved	: 1;
	uint8_t reason		: 7;
	/* [한국어] 종료 사유:
	 *   0=close session, 1=close connection, 2=remove for recovery. */
	uint8_t reason_1	: 1;
	/* [한국어] reason 의 reserved 최상위 비트. */
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint8_t res2[8];
	uint32_t itt;
	uint16_t cid;
	/* [한국어] 종료 대상 Connection ID (reason=1,2 시 의미). */
	uint16_t res3;
	uint32_t cmd_sn;
	uint32_t exp_stat_sn;
	uint8_t res4[16];
};

struct iscsi_bhs_logout_resp {
	/* [한국어] Logout Response PDU (opcode 0x26). target 이 자원 정리를 마치고
	 * Time2Wait/Time2Retain 을 통보. RFC 7143 §11.15. */
	uint8_t opcode		: 6;	/* opcode = 0x26 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t response;
	/* [한국어] 결과 코드: 0=success, 1=CID not found, 2=recovery not supported,
	 * 3=cleanup failed. */
	uint8_t res;
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint8_t res2[8];
	uint32_t itt;
	uint32_t res3;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint32_t res4;
	uint16_t time_2_wait;
	/* [한국어] initiator 가 새 connection 을 시도하기 전 대기할 최소 시간(초). */
	uint16_t time_2_retain;
	/* [한국어] target 이 task 상태를 보존할 최대 시간. recovery 윈도우. */
	uint32_t res5;
};

struct iscsi_bhs_nop_in {
	/* [한국어] NOP-In PDU (opcode 0x20). target 발 핑/keepalive,
	 * StatSN 흐름 진행 또는 ExpCmdSN/MaxCmdSN 갱신용 캐리어.
	 * itt=0xffffffff & ttt!=0xffffffff 이면 응답 NOP-Out 요구. RFC 7143 §11.19. */
	uint8_t opcode		: 6;	/* opcode = 0x20 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint8_t res3[12];
};

struct iscsi_bhs_nop_out {
	/* [한국어] NOP-Out PDU (opcode 0x00). initiator 발 핑/응답.
	 * 1) ttt=0xffffffff: 일반 ping (ack 운반)
	 * 2) ttt!=0xffffffff: target 의 NOP-In 에 대한 응답 echo. */
	uint8_t opcode		: 6;	/* opcode = 0x00 */
	uint8_t immediate	: 1;
	uint8_t reserved	: 1;
	uint8_t flags;
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	/* [한국어] target 이 NOP-In 에 부여했던 ttt 를 echo. */
	uint32_t cmd_sn;
	uint32_t exp_stat_sn;
	uint8_t res4[16];
};

struct iscsi_bhs_r2t {
	/* [한국어] Ready To Transfer PDU (opcode 0x31). target 이 initiator 에게
	 * WRITE 데이터의 일부 윈도우를 전송하라고 허가. Solicited Data-Out 의 트리거.
	 * RFC 7143 §11.8. */
	uint8_t opcode		: 6;	/* opcode = 0x31 */
	uint8_t reserved	: 2;
	uint8_t flags;
	/* [한국어] F bit 항상 1. */
	uint8_t rsv[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	/* [한국어] R2T 자체는 데이터 세그먼트 0. */
	uint64_t lun;
	uint32_t itt;
	/* [한국어] 대상 SCSI Command 의 ITT. */
	uint32_t ttt;
	/* [한국어] target 부여 — 후속 Data-Out 이 동일 ttt 사용. */
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint32_t r2t_sn;
	/* [한국어] 같은 명령에 대한 R2T 시퀀스 번호 (0,1,2,...). */
	uint32_t buffer_offset;
	/* [한국어] WRITE 버퍼의 시작 오프셋. */
	uint32_t desired_xfer_len;
	/* [한국어] 이 R2T 윈도우에서 전송 받기 원하는 바이트 수. */
};

struct iscsi_bhs_reject {
	/* [한국어] Reject PDU (opcode 0x3f). 받은 PDU 가 디지스트 에러/프로토콜
	 * 위반/지원 안 됨 등으로 처리 불가일 때 원본 BHS 를 데이터 세그먼트에 첨부.
	 * RFC 7143 §11.17. */
	uint8_t opcode		: 6;	/* opcode = 0x3f */
	uint8_t reserved	: 2;
	uint8_t flags;
	/* [한국어] F bit 항상 1. */
	uint8_t reason;
	/* [한국어] Reject 사유. ISCSI_REASON_* 매크로 참조. */
	uint8_t res;
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint8_t res2[8];
	uint32_t ffffffff;
	/* [한국어] ITT 자리 — Reject 는 task 와 무관하므로 0xffffffff. */
	uint32_t res3;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint32_t data_sn;
	/* [한국어] DataSNACK 거부 시 관련 DataSN 또는 R2TSN. */
	uint8_t res4[8];
};

struct iscsi_bhs_scsi_req {
	/* [한국어] SCSI Command PDU (opcode 0x01). 16B SCSI CDB 임베드.
	 * read_bit/write_bit 로 데이터 방향, expected_data_xfer_len 으로 데이터 양.
	 * RFC 7143 §11.2. */
	uint8_t opcode		: 6;	/* opcode = 0x01 */
	uint8_t immediate	: 1;
	uint8_t reserved	: 1;
	uint8_t attribute	: 3;
	/* [한국어] Task Attribute (0=Untagged, 1=Simple, 2=Ordered, 3=Head of Queue,
	 * 4=ACA). SCSI Architecture Model 의 큐 정렬 속성. */
	uint8_t reserved2	: 2;
	uint8_t write_bit	: 1;
	/* [한국어] W=1: 명령이 Data-Out 데이터를 동반(WRITE). */
	uint8_t read_bit	: 1;
	/* [한국어] R=1: 명령이 Data-In 데이터를 기대(READ). 양쪽 1 = bidirectional. */
	uint8_t final_bit	: 1;
	/* [한국어] F=1: 이 PDU 가 명령의 마지막 PDU. ImmediateData 가 있으면 0 가능. */
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	/* [한국어] ImmediateData 의 길이 (bytes). */
	uint64_t lun;
	uint32_t itt;
	uint32_t expected_data_xfer_len;
	/* [한국어] 명령이 전송할 총 바이트 수 (READ 의 경우 Data-In 합계,
	 * WRITE 의 경우 Data-Out 합계). residual 계산에 쓰임. */
	uint32_t cmd_sn;
	uint32_t exp_stat_sn;
	uint8_t cdb[16];
	/* [한국어] SCSI Command Descriptor Block. 6/10/12/16B CDB 가 들어가며
	 * 부족한 자리는 0 패딩. >16B CDB 는 AHS 의 Extended CDB 로 운반. */
};

struct iscsi_bhs_scsi_resp {
	/* [한국어] SCSI Response PDU (opcode 0x21). SCSI status, sense data,
	 * residual count 운반. RFC 7143 §11.4. */
	uint8_t opcode		: 6;	/* opcode = 0x21 */
	uint8_t reserved	: 2;
	uint8_t flags;
	/* [한국어] o=overflow, u=underflow, O/U=bidi versions, S=phase-collapsed. */
	uint8_t response;
	/* [한국어] iSCSI 서비스 응답 (0=Command Completed at Target, 1=Target Failure). */
	uint8_t status;
	/* [한국어] SCSI status (0=GOOD, 0x02=CHECK_CONDITION, 0x08=BUSY, ...). */
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	/* [한국어] Sense data + (옵션) bidi residual response data 크기. */
	uint8_t res4[8];
	uint32_t itt;
	uint32_t snacktag;
	/* [한국어] 만약 SNACK 응답으로 재전송 중이면 원래 SNACK 의 식별자. */
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint32_t exp_data_sn;
	/* [한국어] 이 명령에서 보낸 Data-In PDU 개수 (initiator 의 ack 검증용). */
	uint32_t bi_read_res_cnt;
	/* [한국어] Bi-directional read residual count. */
	uint32_t res_cnt;
	/* [한국어] Residual count: under/overflow 시 누락/초과 바이트. */
};

struct iscsi_bhs_data_in {
	/* [한국어] SCSI Data-In PDU (opcode 0x05). READ 응답 데이터 운반.
	 * S=1 phase-collapsed 면 status 도 함께 전달 — 별도 SCSI Response 생략 가능.
	 * RFC 7143 §11.7. */
	uint8_t opcode		: 6;	/* opcode = 0x05 */
	uint8_t reserved	: 2;
	uint8_t flags;
	/* [한국어] A=acknowledge, O=overflow, U=underflow, S=phase-collapsed status. */
	uint8_t res;
	uint8_t status;
	/* [한국어] phase-collapsed (S=1) 시 SCSI status. 그 외 reserved. */
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint32_t data_sn;
	/* [한국어] 같은 명령 안에서 Data-In PDU 의 시퀀스 번호 (0..). */
	uint32_t buffer_offset;
	/* [한국어] 이 데이터의 시작 오프셋 (initiator 버퍼 기준). */
	uint32_t res_cnt;
	/* [한국어] phase-collapsed 시 residual count. */
};

struct iscsi_bhs_data_out {
	/* [한국어] SCSI Data-Out PDU (opcode 0x05 → 표 상에서는 0x05/Initiator).
	 * 헤더의 opcode = 0x05 로 표기되어 있으나, initiator 의 Data-Out 은 opcode
	 * 0x05 (ISCSI_OP_SCSI_DATAOUT). 이 코드 표기 0x25 는 historical typo —
	 * 실제 송신자 코드는 ISCSI_OP_SCSI_DATAOUT(0x05) 사용. */
	uint8_t opcode		: 6;	/* opcode = 0x25 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	/* [한국어] R2T 의 ttt 를 echo (solicited) 또는 0xffffffff (unsolicited). */
	uint32_t res3;
	uint32_t exp_stat_sn;
	uint32_t res4;
	uint32_t data_sn;
	/* [한국어] 같은 명령 / 같은 R2T 안에서의 Data-Out 시퀀스 번호. */
	uint32_t buffer_offset;
	/* [한국어] 이 데이터의 시작 오프셋. R2T 의 buffer_offset+범위 안. */
	uint32_t res5;
};

struct iscsi_bhs_snack_req {
	/* [한국어] SNACK Request PDU (opcode 0x10). Status/Data/R2T 재전송 요청.
	 * ErrorRecoveryLevel >= 1 에서만 사용. RFC 7143 §11.16. */
	uint8_t opcode		: 6;	/* opcode = 0x10 */
	uint8_t reserved	: 2;
	uint8_t flags;
	/* [한국어] 하위 4비트 = SNACK 타입 (ISCSI_FLAG_SNACK_TYPE_*). */
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	uint32_t res5;
	uint32_t exp_stat_sn;
	uint8_t res6[8];
	uint32_t beg_run;
	/* [한국어] 재전송 윈도우 시작 (StatSN 또는 DataSN/R2TSN). */
	uint32_t run_len;
	/* [한국어] 윈도우 길이. 0 이면 begRun 부터 끝까지 전부. */
};

struct iscsi_bhs_task_req {
	/* [한국어] Task Management Function Request PDU (opcode 0x02).
	 * Abort/Reset 류 명령 전달. RFC 7143 §11.5. */
	uint8_t opcode		: 6;	/* opcode = 0x02 */
	uint8_t immediate	: 1;
	/* [한국어] TM 은 보통 immediate=1 — CmdSN 큐 우회로 즉시 실행. */
	uint8_t reserved	: 1;
	uint8_t flags;
	/* [한국어] 하위 7비트(ISCSI_TASK_FUNCTION_MASK) = enum iscsi_task_func. */
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ref_task_tag;
	/* [한국어] AbortTask 등에서 대상 태스크의 ITT. 0xffffffff = N/A. */
	uint32_t cmd_sn;
	uint32_t exp_stat_sn;
	uint32_t ref_cmd_sn;
	/* [한국어] 대상 태스크의 CmdSN — TM 윈도우 검증에 사용. */
	uint32_t exp_data_sn;
	/* [한국어] Task Reassign 시 마지막으로 받은 Data-In SN. */
	uint8_t res5[8];
};

struct iscsi_bhs_task_resp {
	/* [한국어] Task Management Function Response PDU (opcode 0x22). */
	uint8_t opcode		: 6;	/* opcode = 0x22 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t response;
	/* [한국어] enum iscsi_task_func_resp 값. */
	uint8_t res;
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint8_t res2[8];
	uint32_t itt;
	uint32_t res3;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint8_t res4[12];
};

struct iscsi_bhs_text_req {
	/* [한국어] Text Request PDU (opcode 0x04). 디스커버리 세션의 SendTargets=
	 * 또는 풀피처 단계의 추가 파라미터 협상. RFC 7143 §11.10. */
	uint8_t opcode		: 6;	/* opcode = 0x04 */
	uint8_t immediate	: 1;
	uint8_t reserved	: 1;
	uint8_t flags;
	/* [한국어] F=final, C=continue (장문 텍스트 분할). */
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	/* [한국어] key=value\0 텍스트 합계 길이. */
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	/* [한국어] 첫 PDU 는 0xffffffff. continue 시 target 이 부여한 ttt echo. */
	uint32_t cmd_sn;
	uint32_t exp_stat_sn;
	uint8_t res3[16];
};

struct iscsi_bhs_text_resp {
	/* [한국어] Text Response PDU (opcode 0x24). RFC 7143 §11.11. */
	uint8_t opcode		: 6;	/* opcode = 0x24 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	/* [한국어] continue 응답이면 target 이 다음 라운드용 ttt 를 부여. */
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint8_t res4[12];
};

/* generic flags */
#define ISCSI_FLAG_FINAL			0x80
/* [한국어] BHS flags 의 F(final) 비트. 다중 PDU 시퀀스의 마지막을 표시. */

/* login flags */
#define ISCSI_LOGIN_TRANSIT			0x80
/* [한국어] T 비트. 협상 단계 이행 요청 (CSG → NSG). */
#define ISCSI_LOGIN_CONTINUE			0x40
/* [한국어] C 비트. 키-값 페이로드가 다음 PDU 로 이어짐. */
#define ISCSI_LOGIN_CURRENT_STAGE_MASK		0x0c
/* [한국어] CSG 비트필드 마스크 (bit3-2). */
#define ISCSI_LOGIN_CURRENT_STAGE_0		0x04
/* [한국어] CSG=1 (Operational stage 표기 — 0x04 = Op). 주의: 코드 0/1/3 만 유효. */
#define ISCSI_LOGIN_CURRENT_STAGE_1		0x08
/* [한국어] CSG=2 (Login op 단계의 alternate value). */
#define ISCSI_LOGIN_CURRENT_STAGE_3		0x0c
/* [한국어] CSG=3 (FullFeature stage). */
#define ISCSI_LOGIN_NEXT_STAGE_MASK		0x03
/* [한국어] NSG 비트필드 마스크 (bit1-0). */
#define ISCSI_LOGIN_NEXT_STAGE_0		0x01
/* [한국어] NSG=1 (Operational). */
#define ISCSI_LOGIN_NEXT_STAGE_1		0x02
/* [한국어] NSG=2. */
#define ISCSI_LOGIN_NEXT_STAGE_3		0x03
/* [한국어] NSG=3 (FullFeature). */

/* text flags */
#define ISCSI_TEXT_CONTINUE			0x40
/* [한국어] Text PDU 의 C(continue) 비트 — 키-값이 다음 PDU 로 분할됨. */

/* datain flags */
#define ISCSI_DATAIN_ACKNOWLEDGE		0x40
/* [한국어] A 비트. initiator 가 SNACK 으로 ack 해야 함을 표시. */
#define ISCSI_DATAIN_OVERFLOW			0x04
/* [한국어] O 비트. expected_data_xfer_len 보다 더 많은 데이터 발생. */
#define ISCSI_DATAIN_UNDERFLOW			0x02
/* [한국어] U 비트. 더 적은 데이터로 종료. residual count 참조. */
#define ISCSI_DATAIN_STATUS			0x01
/* [한국어] S 비트. phase-collapsed — 이 Data-In 에 SCSI status 동봉. */

/* SCSI resp flags */
#define ISCSI_SCSI_BIDI_OVERFLOW		0x10
/* [한국어] Bidi (양방향) read 측 overflow. */
#define ISCSI_SCSI_BIDI_UNDERFLOW		0x08
/* [한국어] Bidi read 측 underflow. */
#define ISCSI_SCSI_OVERFLOW			0x04
/* [한국어] write/단방향 overflow. */
#define ISCSI_SCSI_UNDERFLOW			0x02
/* [한국어] write/단방향 underflow. */

/* SCSI task flags */
#define ISCSI_TASK_FUNCTION_MASK		0x7f
/* [한국어] flags 바이트의 하위 7비트 = enum iscsi_task_func 값. */

/* Reason for Reject */
#define ISCSI_REASON_RESERVED			0x1
/* [한국어] Reserved 사유. (스펙상 reserved 영역의 PDU 거부 — 거의 안 쓰임.) */
#define ISCSI_REASON_DATA_DIGEST_ERROR		0x2
/* [한국어] DataDigest CRC32C 불일치 — 데이터 손상 감지. */
#define ISCSI_REASON_DATA_SNACK_REJECT		0x3
/* [한국어] SNACK 으로 요청된 재전송이 더 이상 가능하지 않음 (버퍼 비움 등). */
#define ISCSI_REASON_PROTOCOL_ERROR		0x4
/* [한국어] 일반 프로토콜 위반 (필드 부정합, 시퀀스 위반 등). */
#define ISCSI_REASON_CMD_NOT_SUPPORTED		0x5
/* [한국어] 받은 opcode 미지원. */
#define ISCSI_REASON_IMM_CMD_REJECT		0x6
/* [한국어] Immediate 명령 한도 초과 (target 이 동시에 처리 가능한 양 초과). */
#define ISCSI_REASON_TASK_IN_PROGRESS		0x7
/* [한국어] 동일 ITT 가 이미 진행 중. */
#define ISCSI_REASON_INVALID_SNACK		0x8
/* [한국어] SNACK 파라미터 부정합. */
#define ISCSI_REASON_INVALID_PDU_FIELD		0x9
/* [한국어] PDU 필드값 부정합 (LUN 형식 등). */
#define ISCSI_REASON_LONG_OPERATION_REJECT	0xa
/* [한국어] 장기 동작 거부 (자원 부족 등). */
#define ISCSI_REASON_NEGOTIATION_RESET		0xb
/* [한국어] 협상 reset 요구 (FullFeature 후 협상 시도 등). */
#define ISCSI_REASON_WAIT_FOR_RESET		0xc
/* [한국어] reset 진행 중 — 이후 다시 시도하라. */

#define ISCSI_FLAG_SNACK_TYPE_DATA		0
/* [한국어] SNACK type=0: Data/R2T 재전송 요청. */
#define ISCSI_FLAG_SNACK_TYPE_R2T		0
/* [한국어] type 0 의 alternate alias (Data/R2T 통합 SNACK). */
#define ISCSI_FLAG_SNACK_TYPE_STATUS		1
/* [한국어] type=1: Status (SCSI Response/Task Resp 등) 재전송 요청. */
#define ISCSI_FLAG_SNACK_TYPE_DATA_ACK		2
/* [한국어] type=2: Data-In ack — A 비트로 요청된 ack 응답. */
#define ISCSI_FLAG_SNACK_TYPE_RDATA		3
/* [한국어] type=3: R-Data SNACK (initiator 가 캐시한 데이터 재사용). */
#define ISCSI_FLAG_SNACK_TYPE_MASK		0x0F	/* 4 bits */
/* [한국어] flags 의 하위 4비트가 SNACK 타입 (위 0~3 값). */

struct iscsi_ahs {
	/* [한국어] Additional Header Segment. BHS 뒤, Header Digest 앞에 위치하는
	 * 가변 길이 확장 헤더. Extended CDB(>16B), Bidirectional Expected Read-Data
	 * Length 등에 사용. RFC 7143 §11.1. */
	/* 0-3 */
	uint8_t ahs_len[2];
	/* [한국어] 이 AHS 의 specific 필드 길이(빅엔디안 16비트). 헤더(4B) 미포함.
	 * 전체 AHS 크기 = ahs_len + 4(타입+specific1) 를 4B 로 정렬. */
	uint8_t ahs_type;
	/* [한국어] AHS 종류:
	 *   1 = Extended CDB (16B 초과 CDB)
	 *   2 = Bidirectional Expected Read-Data Length. */
	uint8_t ahs_specific1;
	/* [한국어] 종류별 specific 1B (대개 reserved). */
	/* 4-x */
	uint8_t ahs_specific2[];
	/* [한국어] 가변 길이 specific payload (flexible array). */
};

#define ISCSI_BHS_LOGIN_GET_TBIT(X) (!!(X & ISCSI_LOGIN_TRANSIT))
/* [한국어] Login flags 바이트에서 T 비트 추출 (0/1).
 * !! 트릭으로 비트마스크 결과를 0/1 로 정규화. */
#define ISCSI_BHS_LOGIN_GET_CBIT(X) (!!(X & ISCSI_LOGIN_CONTINUE))
/* [한국어] Login flags 의 C 비트 추출. */
#define ISCSI_BHS_LOGIN_GET_CSG(X) ((X & ISCSI_LOGIN_CURRENT_STAGE_MASK) >> 2)
/* [한국어] Current Stage 정수 추출 (0,1,3 중 하나). bit3-2 → 우측 시프트 2. */
#define ISCSI_BHS_LOGIN_GET_NSG(X) (X & ISCSI_LOGIN_NEXT_STAGE_MASK)
/* [한국어] Next Stage 정수 추출 (bit1-0). */

#define ISCSI_CLASS_SUCCESS			0x00
/* [한국어] Login Response status_class 0 — 성공. */
#define ISCSI_CLASS_REDIRECT			0x01
/* [한국어] status_class 1 — 다른 주소로 리디렉션. */
#define ISCSI_CLASS_INITIATOR_ERROR		0x02
/* [한국어] status_class 2 — initiator 측 에러 (인증 실패 등). */
#define ISCSI_CLASS_TARGET_ERROR		0x03
/* [한국어] status_class 3 — target 측 에러 (자원 부족 등). */

/* Class (Success) detailed info: 0 */
#define ISCSI_LOGIN_ACCEPT			0x00
/* [한국어] class=0 의 detail 0 — 정상 수락. */

/* Class (Redirection) detailed info: 1 */
#define ISCSI_LOGIN_TARGET_TEMPORARILY_MOVED	0x01
/* [한국어] class=1 의 detail — target 이 일시적으로 다른 주소로 이동.
 * Login Response 데이터에 TargetAddress= 키가 포함되어야 한다. */
#define ISCSI_LOGIN_TARGET_PERMANENTLY_MOVED	0x02
/* [한국어] target 이 영구 이동. initiator 는 디스커버리 캐시 갱신 필요. */

/* Class (Initiator Error) detailed info: 2 */
#define ISCSI_LOGIN_INITIATOR_ERROR		0x00
/* [한국어] 일반 initiator 에러. */
#define ISCSI_LOGIN_AUTHENT_FAIL		0x01
/* [한국어] 인증 실패 (CHAP 응답 불일치 등). */
#define ISCSI_LOGIN_AUTHORIZATION_FAIL		0x02
/* [한국어] 인가 실패 (인증은 OK 지만 LUN 접근 권한 없음). */
#define ISCSI_LOGIN_TARGET_NOT_FOUND		0x03
/* [한국어] TargetName 으로 지정된 타깃이 존재하지 않음. */
#define ISCSI_LOGIN_TARGET_REMOVED		0x04
/* [한국어] 타깃이 제거됨. */
#define ISCSI_LOGIN_UNSUPPORTED_VERSION		0x05
/* [한국어] version_min/max 협상 실패. */
#define ISCSI_LOGIN_TOO_MANY_CONNECTIONS	0x06
/* [한국어] MaxConnections 초과. */
#define ISCSI_LOGIN_MISSING_PARMS		0x07
/* [한국어] 필수 협상 키 누락. */
#define ISCSI_LOGIN_CONN_ADD_FAIL		0x08
/* [한국어] 기존 세션에 connection 추가 실패. */
#define ISCSI_LOGIN_NOT_SUPPORTED_SESSION_TYPE	0x09
/* [한국어] SessionType 미지원 (Discovery/Normal). */
#define ISCSI_LOGIN_NO_SESSION			0x0a
/* [한국어] connection 추가 시도했으나 가리키는 세션 없음 (TSIH 무효). */
#define ISCSI_LOGIN_INVALID_LOGIN_REQUEST	0x0b
/* [한국어] PDU 자체 부정합. */

/* Class (Target Error) detailed info: 3 */
#define ISCSI_LOGIN_STATUS_TARGET_ERROR		0x00
/* [한국어] 일반 target 에러. */
#define ISCSI_LOGIN_STATUS_SERVICE_UNAVAILABLE	0x01
/* [한국어] 서비스 일시 불가 (재시작 중 등). */
#define ISCSI_LOGIN_STATUS_NO_RESOURCES		0x02
/* [한국어] 메모리/세션 자원 부족. */

#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 닫기. */
#endif

#endif /* SPDK_ISCSI_SPEC_H */
/* [한국어] include guard 종료. */
