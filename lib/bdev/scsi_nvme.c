/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   Copyright (c) 2016 FUJITSU LIMITED, All rights reserved.
 */

/*
 * [한국어 설명] NVMe completion status → SCSI sense key/ASC/ASCQ 변환 테이블 (scsi_nvme.c)
 *
 * === 파일의 역할 ===
 * NVMe controller가 반환한 completion 정보(SCT=Status Code Type, SC=Status Code의 두
 * 8비트 필드)를 SCSI 영역의 4-튜플 (sc=Status, sk=Sense Key, asc=Additional Sense Code,
 * ascq=Additional Sense Code Qualifier)로 1대1 변환하는 거대한 switch/case 테이블 한 개
 * 함수만 정의한 파일. NVMe Spec(Base Spec 1.x/2.x) §5.x의 Status Code 정의와 SPC-4/SBC-3
 * (SCSI Primary/Block Commands) §4.5의 Sense Data 정의를 매핑한다.
 *
 * 본 파일이 존재하는 이유:
 *   1) **iSCSI/SCSI target → NVMe backend 경로**: SPDK의 iSCSI target(lib/iscsi)이나
 *      vhost-scsi target은 host에게 SCSI sense를 반환해야 하지만, 실제 백엔드 bdev가
 *      NVMe인 경우 NVMe 에러 코드만 들고 있다. 이 함수가 그 차이를 메우는 어댑터.
 *   2) **SCSI passthrough가 NVMe 위에 있을 때**: SPDK_BDEV_IO_TYPE_NVME_* 결과를
 *      SCSI initiator가 이해할 수 있게 변환.
 *   3) **공통 진단 경로**: bdev 사용자가 NVMe-specific 에러 코드를 모르더라도 SCSI sense
 *      key 정도는 통상적으로 이해 가능 — 디버깅/로깅용으로도 유용.
 *
 * 본 파일은 코드 수정 없이 한국어 주석만 추가/보강된 학습 사본이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 흐름(전형적인 iSCSI target 시나리오):
 *   iSCSI initiator(외부 호스트) → READ(10) command
 *     → lib/iscsi가 SCSI command를 디코드 → spdk_bdev_readv_blocks (bdev API)
 *       → bdev 모듈(예: bdev_nvme) → NVMe Read cmd → 디바이스 처리 → CQE
 *         → bdev_io_complete()가 bdev_io->internal.error.nvme.{sct,sc} 채움
 *           → iSCSI 완료 콜백이 spdk_scsi_nvme_translate(bdev_io, ...) 호출
 *             → SCSI sense data를 build해서 initiator에게 SCSI Response 전송
 *
 * 실행 컨텍스트: bdev 완료 콜백과 동일한 spdk_thread(reactor)에서 호출. 순수 함수
 * (전역/공유 상태 없음, switch만으로 변환)이므로 동시 호출 안전. 락 불필요.
 *
 * === 타 모듈과의 연결 ===
 *  - **include/spdk/bdev_module.h** — `struct spdk_bdev_io`와 그 안의
 *    `internal.error.nvme.{sct,sc}` 필드 정의. 본 함수는 이 두 값만 입력으로 사용.
 *  - **include/spdk/nvme_spec.h** — `SPDK_NVME_SCT_*`, `SPDK_NVME_SC_*` 상수 정의.
 *    NVMe Base Spec §5.x Figure "Status Code Type Values"와 "Status Code Values"의
 *    enum 매핑. 본 파일은 이 상수들을 case label로 사용.
 *  - **include/spdk/scsi.h / scsi_spec.h** (간접) — `SPDK_SCSI_STATUS_*`,
 *    `SPDK_SCSI_SENSE_*`, `SPDK_SCSI_ASC_*`, `SPDK_SCSI_ASCQ_*` 상수. SPC-4 sense data.
 *    본 함수의 출력 4 포인터에 채워지는 값들의 타입.
 *  - **lib/iscsi**, **lib/scsi**, **module/vhost-scsi 등 호출자** — 본 함수만 호출하면
 *    NVMe 에러를 SCSI 형식으로 자동 변환받을 수 있어 NVMe spec 지식 없이 구현 가능.
 *
 * === 주요 함수/구조체 요약 ===
 *  - spdk_scsi_nvme_translate(bdev_io, sc, sk, asc, ascq):
 *      유일한 함수. bdev_io->internal.error.nvme.{sct,sc}를 읽어 SCT(Status Code Type
 *      4종: GENERIC=0, COMMAND_SPECIFIC=1, MEDIA_ERROR=2, VENDOR_SPECIFIC=7)로 1차 분기,
 *      그 안에서 NVMe SC 값으로 2차 분기해 4 출력 포인터를 채움.
 *      모든 분기는 fallthrough 없이 즉시 break — 디폴트 케이스는 안전하게
 *      CHECK_CONDITION + ILLEGAL_REQUEST + NO_ADDITIONAL_SENSE로 매핑.
 *
 *  변환 표(요약):
 *    SCT=GENERIC, SC=SUCCESS                     → GOOD/NO_SENSE
 *    SCT=GENERIC, SC=INVALID_OPCODE              → CHECK_CONDITION/ILLEGAL_REQUEST/INVALID_OPCODE
 *    SCT=GENERIC, SC=DATA_TRANSFER_ERROR         → CHECK_CONDITION/MEDIUM_ERROR
 *    SCT=GENERIC, SC=ABORTED_POWER_LOSS          → TASK_ABORTED/ABORTED_COMMAND/POWER_LOSS
 *    SCT=GENERIC, SC=RESERVATION_CONFLICT        → RESERVATION_CONFLICT
 *    SCT=COMMAND_SPECIFIC, SC=INVALID_FORMAT     → CHECK_CONDITION/ILLEGAL_REQUEST/FORMAT_FAILED
 *    SCT=COMMAND_SPECIFIC, SC=ATTEMPT_WRITE_RO   → CHECK_CONDITION/DATA_PROTECT/WRITE_PROTECTED
 *    SCT=MEDIA_ERROR,    SC=WRITE_FAULTS         → CHECK_CONDITION/MEDIUM_ERROR/WRITE_FAULT
 *    SCT=MEDIA_ERROR,    SC=UNRECOVERED_READ    → CHECK_CONDITION/MEDIUM_ERROR/UNRECOVERED_READ
 *    SCT=MEDIA_ERROR,    SC=COMPARE_FAILURE      → CHECK_CONDITION/MISCOMPARE
 *    SCT=MEDIA_ERROR,    SC=ACCESS_DENIED        → CHECK_CONDITION/DATA_PROTECT/ACCESS_DENIED
 *    SCT=VENDOR_SPECIFIC / 그 외                  → CHECK_CONDITION/ILLEGAL_REQUEST (안전 디폴트)
 */

#include "spdk/bdev_module.h"
/* [한국어] struct spdk_bdev_io 정의와 internal.error union — NVMe 에러 정보의 출처. */

#include "spdk/nvme_spec.h"
/* [한국어] NVMe Base Spec의 SCT/SC 상수 enum (SPDK_NVME_SCT_*, SPDK_NVME_SC_*). switch label로 사용. */

/*
 * [한국어]
 * spdk_scsi_nvme_translate - NVMe completion status를 SCSI sense data 4-튜플로 변환.
 *
 * @bdev_io: 완료된 bdev_io. internal.error.nvme.{sct,sc} 필드만 사용 — 호출자는 반드시
 *           이 두 필드가 NVMe 모듈에 의해 의미 있는 값으로 채워진 상태에서 호출해야 한다.
 *           (성공한 I/O라도 SCT=GENERIC, SC=SUCCESS로 호출 가능 → GOOD/NO_SENSE 반환)
 * @sc:      [out] SCSI Status Code (8비트). 예: SPDK_SCSI_STATUS_GOOD(0x00),
 *           SPDK_SCSI_STATUS_CHECK_CONDITION(0x02), SPDK_SCSI_STATUS_RESERVATION_CONFLICT(0x18) 등.
 *           SPC-4 §5.3 "Status Codes" 정의.
 * @sk:      [out] SCSI Sense Key (4비트, 보통 8비트로 보관). NO_SENSE/MEDIUM_ERROR/HARDWARE_ERROR/
 *           ILLEGAL_REQUEST/UNIT_ATTENTION/DATA_PROTECT/ABORTED_COMMAND 등. SPC-4 §4.5.6.
 *           CHECK_CONDITION 응답일 때만 의미가 있음.
 * @asc:     [out] Additional Sense Code (8비트). SPC-4 §4.5.6 + Annex F에 정의된 수백 개의 코드 중 하나.
 * @ascq:    [out] Additional Sense Code Qualifier (8비트). asc의 세부 분류. (asc, ascq) 쌍이 SCSI
 *           오류의 정확한 의미를 결정한다. 예: (0x29, 0x00) = "POWER ON, RESET, OR BUS DEVICE RESET".
 *
 * 본 함수는 4 포인터에 한 번씩만 쓰고 반환하는 순수 함수. 락/원자 연산 없음.
 * 호출 컨텍스트: bdev 완료 콜백 동일 thread (reactor). 대개 iSCSI/vhost-scsi target의 완료 경로.
 *
 * 호출 체인:
 *   bdev_io 완료 콜백 (예: iscsi_bdev_io_completion) → [이 함수] → SCSI Response/Sense Data build
 */
void
spdk_scsi_nvme_translate(const struct spdk_bdev_io *bdev_io, int *sc, int *sk,
			 int *asc, int *ascq)
{
	int nvme_sct = bdev_io->internal.error.nvme.sct;
	/* [한국어] NVMe Status Code Type (3비트, NVMe Spec §4.6.1 Figure "Completion Queue Entry").
	 *  주요 값: 0=GENERIC, 1=COMMAND_SPECIFIC, 2=MEDIA_ERROR, 3=PATH_RELATED, 7=VENDOR_SPECIFIC. */
	int nvme_sc = bdev_io->internal.error.nvme.sc;
	/* [한국어] NVMe Status Code (8비트). SCT 안에서의 세부 에러 코드. SCT 값에 따라 enum 의미가 달라짐. */

	switch (nvme_sct) {
	/* [한국어] 1차 분기 — Status Code Type별로 case 그룹을 나눔. NVMe spec이 SCT/SC 두 단계로
	 *  분류해 둔 구조를 그대로 따라 번역 표를 작성. */
	case SPDK_NVME_SCT_GENERIC:
		/* [한국어] SCT=0x0 GENERIC — Admin/I/O 명령 모두에서 발생할 수 있는 일반 에러군.
		 *  대다수의 흔한 에러(SUCCESS 포함)가 여기에 속함. */
		switch (nvme_sc) {
		/* [한국어] 2차 분기 — GENERIC 내에서 구체적 SC 값별로 SCSI 매핑. */
		case SPDK_NVME_SC_SUCCESS:
			/* [한국어] 0x00: 명령 정상 완료. SCSI에서는 GOOD(0x00) status + sense 없음. */
			*sc   = SPDK_SCSI_STATUS_GOOD;
			/* [한국어] SCSI Status: GOOD — SCSI Response의 status 필드. */
			*sk   = SPDK_SCSI_SENSE_NO_SENSE;
			/* [한국어] sense data 자체가 없음을 의미하는 sense key. */
			*asc  = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
			/* [한국어] 추가 sense code 없음. */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] 추가 qualifier도 없음 — 정상 응답의 표준 4-튜플. */
			break;
		case SPDK_NVME_SC_INVALID_OPCODE:
			/* [한국어] 0x01: 디바이스가 모르는 opcode를 받음. SCSI에서는 잘못된 CDB로 분류. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] CHECK_CONDITION(0x02) — 자세한 내용은 sense data로 전달. */
			*sk   = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
			/* [한국어] ILLEGAL_REQUEST(0x05) — initiator 측 명령 오류군. */
			*asc  = SPDK_SCSI_ASC_INVALID_COMMAND_OPERATION_CODE;
			/* [한국어] (0x20) "INVALID COMMAND OPERATION CODE" — opcode 자체가 정의되지 않음. */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] qualifier 없음. */
			break;
		case SPDK_NVME_SC_INVALID_FIELD:
			/* [한국어] 0x02: 명령의 필드 값이 잘못됨. SCSI는 INVALID FIELD IN CDB로 매핑. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense를 함께 전달. */
			*sk   = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
			/* [한국어] 명령 형식 오류. */
			*asc  = SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB;
			/* [한국어] (0x24) "INVALID FIELD IN CDB" — initiator의 CDB 필드 오류. */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] 추가 정보 없음. */
			break;
		case SPDK_NVME_SC_DATA_TRANSFER_ERROR:
		case SPDK_NVME_SC_CAPACITY_EXCEEDED:
			/* [한국어] 0x04 데이터 전송 실패 또는 0x05 용량 초과 — 모두 매체 관련 일반 에러로 묶음.
			 *  SCSI에서는 MEDIUM_ERROR로 통일 처리. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_MEDIUM_ERROR;
			/* [한국어] (0x03) MEDIUM_ERROR — 매체/저장 영역 오류군. */
			*asc  = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
			/* [한국어] 세부 분류는 일반화 — 디바이스가 더 자세한 정보를 주지 않음. */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] qualifier 없음. */
			break;
		case SPDK_NVME_SC_ABORTED_POWER_LOSS:
			/* [한국어] 0x06: 전원 손실 임박으로 명령 abort. SCSI는 task aborted + warning power loss. */
			*sc   = SPDK_SCSI_STATUS_TASK_ABORTED;
			/* [한국어] (0x40) TASK_ABORTED — 명령 자체가 취소됨. */
			*sk   = SPDK_SCSI_SENSE_ABORTED_COMMAND;
			/* [한국어] (0x0B) ABORTED_COMMAND — 디바이스가 명령을 abort. */
			*asc  = SPDK_SCSI_ASC_WARNING;
			/* [한국어] (0x0B) WARNING — 일반 경고 그룹. */
			*ascq = SPDK_SCSI_ASCQ_POWER_LOSS_EXPECTED;
			/* [한국어] (0x08) POWER LOSS EXPECTED — 전원 손실 예상이 원인임을 명시. */
			break;
		case SPDK_NVME_SC_INTERNAL_DEVICE_ERROR:
			/* [한국어] 0x06: 디바이스 내부 오류. SCSI HARDWARE_ERROR + INTERNAL TARGET FAILURE. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense로 자세한 정보 전달. */
			*sk   = SPDK_SCSI_SENSE_HARDWARE_ERROR;
			/* [한국어] (0x04) HARDWARE_ERROR — 디바이스 측 하드웨어 결함. */
			*asc  = SPDK_SCSI_ASC_INTERNAL_TARGET_FAILURE;
			/* [한국어] (0x44) INTERNAL TARGET FAILURE — 타깃 내부 장애. */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] 추가 정보 없음 — 디바이스가 세부 원인을 노출하지 않음. */
			break;
		case SPDK_NVME_SC_ABORTED_BY_REQUEST:
		case SPDK_NVME_SC_ABORTED_SQ_DELETION:
		case SPDK_NVME_SC_ABORTED_FAILED_FUSED:
		case SPDK_NVME_SC_ABORTED_MISSING_FUSED:
			/* [한국어] 0x07~0x0A: abort 요청, SQ 삭제, fused 명령 실패/누락 등 다양한 abort 원인.
			 *  SCSI에서는 모두 task aborted + aborted command로 통일. */
			*sc   = SPDK_SCSI_STATUS_TASK_ABORTED;
			/* [한국어] 명령이 취소됨. */
			*sk   = SPDK_SCSI_SENSE_ABORTED_COMMAND;
			/* [한국어] sense key: 명령 abort. */
			*asc  = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
			/* [한국어] 세부 원인은 일반화. */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] qualifier 없음. */
			break;
		case SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT:
			/* [한국어] 0x0B: 잘못된 NSID 또는 포맷 불일치. SCSI에서는 ILLEGAL_REQUEST + ACCESS DENIED
			 *  + INVALID LU IDENTIFIER (LUN 식별 오류로 매핑). */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
			/* [한국어] 명령 형식 오류군. */
			*asc  = SPDK_SCSI_ASC_ACCESS_DENIED;
			/* [한국어] (0x20) ACCESS DENIED — 잘못된 LUN/namespace. */
			*ascq = SPDK_SCSI_ASCQ_INVALID_LU_IDENTIFIER;
			/* [한국어] (0x09) "INVALID LU IDENTIFIER" — LU(LUN) 식별자가 무효. */
			break;
		case SPDK_NVME_SC_LBA_OUT_OF_RANGE:
			/* [한국어] 0x80: 요청 LBA가 namespace 범위 밖. SCSI는 표준 LBA OOR 매핑. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
			/* [한국어] 명령 오류. */
			*asc  = SPDK_SCSI_ASC_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
			/* [한국어] (0x21) "LOGICAL BLOCK ADDRESS OUT OF RANGE". */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] 세부 qualifier 없음. */
			break;
		case SPDK_NVME_SC_NAMESPACE_NOT_READY:
			/* [한국어] 0x82: namespace가 아직 준비되지 않음(예: 포맷 진행 중). SCSI는 NOT READY. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_NOT_READY;
			/* [한국어] (0x02) NOT_READY — 디바이스/LU 미준비. */
			*asc  = SPDK_SCSI_ASC_LOGICAL_UNIT_NOT_READY;
			/* [한국어] (0x04) "LOGICAL UNIT NOT READY". */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] 사유 미보고. */
			break;
		case SPDK_NVME_SC_RESERVATION_CONFLICT:
			/* [한국어] 0x83: NVMe Reservation에 의해 거부됨. SCSI에는 전용 status가 있음. */
			*sc   = SPDK_SCSI_STATUS_RESERVATION_CONFLICT;
			/* [한국어] (0x18) RESERVATION_CONFLICT — sense data 없이 status만으로 의미 전달. */
			*sk   = SPDK_SCSI_SENSE_NO_SENSE;
			/* [한국어] sense data 자체가 없음. */
			*asc  = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
			/* [한국어] 추가 정보 없음. */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] qualifier 없음. */
			break;
		case SPDK_NVME_SC_COMMAND_ID_CONFLICT:
		case SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR:
		case SPDK_NVME_SC_INVALID_SGL_SEG_DESCRIPTOR:
		case SPDK_NVME_SC_INVALID_NUM_SGL_DESCIRPTORS:
		case SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID:
		case SPDK_NVME_SC_METADATA_SGL_LENGTH_INVALID:
		case SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID:
		case SPDK_NVME_SC_INVALID_CONTROLLER_MEM_BUF:
		case SPDK_NVME_SC_INVALID_PRP_OFFSET:
		case SPDK_NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED:
		case SPDK_NVME_SC_INVALID_SGL_OFFSET:
		case SPDK_NVME_SC_HOSTID_INCONSISTENT_FORMAT:
		case SPDK_NVME_SC_KEEP_ALIVE_EXPIRED:
		case SPDK_NVME_SC_KEEP_ALIVE_INVALID:
		case SPDK_NVME_SC_FORMAT_IN_PROGRESS:
		default:
			/* [한국어] 위에서 매핑하지 않은 모든 GENERIC SC들 + 알 수 없는 미래 코드.
			 *  대부분은 호스트가 잘못 보낸 명령(SGL/PRP 오류, command id 충돌, keep-alive 등)이거나
			 *  진행 중 상태. SCSI에서는 안전하게 ILLEGAL_REQUEST로 통합 — 자세한 분류 어려움. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
			/* [한국어] 안전 디폴트 — 명령 측 오류로 분류. */
			*asc  = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
			/* [한국어] 세부 코드 없음. */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] 사유 미보고. */
			break;
		}
		break;
		/* [한국어] SCT=GENERIC 그룹 종료. */
	case SPDK_NVME_SCT_COMMAND_SPECIFIC:
		/* [한국어] SCT=0x1 COMMAND_SPECIFIC — 명령에 따라 의미가 달라지는 SC 그룹.
		 *  주로 admin 명령(Create IO Q, Format, Firmware) 관련 + 일부 I/O 명령. */
		switch (nvme_sc) {
		/* [한국어] 2차 분기 — COMMAND_SPECIFIC 내 구체적 SC. */
		case SPDK_NVME_SC_COMPLETION_QUEUE_INVALID:
		case SPDK_NVME_SC_ABORT_COMMAND_LIMIT_EXCEEDED:
			/* [한국어] CQ 무효 / abort 한도 초과 — admin 명령 형식 오류로 일반화. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
			/* [한국어] 명령 오류군. */
			*asc  = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
			/* [한국어] 세부 코드 없음. */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] qualifier 없음. */
			break;
		case SPDK_NVME_SC_INVALID_FORMAT:
			/* [한국어] Format NVM 명령 실패 — SCSI Format Unit 실패에 대응. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
			/* [한국어] 명령 오류로 분류. */
			*asc  = SPDK_SCSI_ASC_FORMAT_COMMAND_FAILED;
			/* [한국어] (0x31) "FORMAT COMMAND FAILED" — Format Unit 실패. */
			*ascq = SPDK_SCSI_ASCQ_FORMAT_COMMAND_FAILED;
			/* [한국어] (0x01) qualifier도 동일 의미 — SPC 표준 매핑. */
			break;
		case SPDK_NVME_SC_CONFLICTING_ATTRIBUTES:
			/* [한국어] dataset management 등에서 attribute 충돌. SCSI는 INVALID FIELD IN CDB. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
			/* [한국어] 명령 오류. */
			*asc  = SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB;
			/* [한국어] (0x24) attribute 충돌은 결국 CDB 필드 오류로 매핑. */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] qualifier 없음. */
			break;
		case SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE:
			/* [한국어] read-only 영역에 write 시도. SCSI는 DATA_PROTECT + WRITE_PROTECTED. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_DATA_PROTECT;
			/* [한국어] (0x07) DATA_PROTECT — 데이터 보호 정책 위반군. */
			*asc  = SPDK_SCSI_ASC_WRITE_PROTECTED;
			/* [한국어] (0x27) "WRITE PROTECTED". */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] qualifier 없음. */
			break;
		case SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER:
		case SPDK_NVME_SC_INVALID_QUEUE_SIZE:
		case SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED:
		case SPDK_NVME_SC_INVALID_FIRMWARE_SLOT:
		case SPDK_NVME_SC_INVALID_FIRMWARE_IMAGE:
		case SPDK_NVME_SC_INVALID_INTERRUPT_VECTOR:
		case SPDK_NVME_SC_INVALID_LOG_PAGE:
		case SPDK_NVME_SC_FIRMWARE_REQ_CONVENTIONAL_RESET:
		case SPDK_NVME_SC_INVALID_QUEUE_DELETION:
		case SPDK_NVME_SC_FEATURE_ID_NOT_SAVEABLE:
		case SPDK_NVME_SC_FEATURE_NOT_CHANGEABLE:
		case SPDK_NVME_SC_FEATURE_NOT_NAMESPACE_SPECIFIC:
		case SPDK_NVME_SC_FIRMWARE_REQ_NVM_RESET:
		case SPDK_NVME_SC_FIRMWARE_REQ_RESET:
		case SPDK_NVME_SC_FIRMWARE_REQ_MAX_TIME_VIOLATION:
		case SPDK_NVME_SC_FIRMWARE_ACTIVATION_PROHIBITED:
		case SPDK_NVME_SC_OVERLAPPING_RANGE:
		case SPDK_NVME_SC_NAMESPACE_INSUFFICIENT_CAPACITY:
		case SPDK_NVME_SC_NAMESPACE_ID_UNAVAILABLE:
		case SPDK_NVME_SC_NAMESPACE_ALREADY_ATTACHED:
		case SPDK_NVME_SC_NAMESPACE_IS_PRIVATE:
		case SPDK_NVME_SC_NAMESPACE_NOT_ATTACHED:
		case SPDK_NVME_SC_THINPROVISIONING_NOT_SUPPORTED:
		case SPDK_NVME_SC_CONTROLLER_LIST_INVALID:
		case SPDK_NVME_SC_INVALID_PROTECTION_INFO:
		default:
			/* [한국어] 위에서 매핑하지 않은 admin/명령 특화 오류 + 미래 코드.
			 *  대부분 admin 명령(Create Q, Get Log Page, Firmware, Namespace Mgmt 등) 관련이라
			 *  SCSI initiator는 거의 보지 못함 — 안전 디폴트로 ILLEGAL_REQUEST. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
			/* [한국어] 안전 디폴트. */
			*asc  = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
			/* [한국어] 세부 코드 없음. */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] qualifier 없음. */
			break;
		}
		break;
		/* [한국어] SCT=COMMAND_SPECIFIC 그룹 종료. */
	case SPDK_NVME_SCT_MEDIA_ERROR:
		/* [한국어] SCT=0x2 MEDIA_ERROR — 매체/데이터 보호(End-to-end DIF/PI) 관련 에러군.
		 *  대부분 read 또는 PI 검증 실패. */
		switch (nvme_sc) {
		/* [한국어] 2차 분기 — MEDIA_ERROR 내 구체적 SC. */
		case SPDK_NVME_SC_WRITE_FAULTS:
			/* [한국어] write 시 매체 오류 — SCSI WRITE FAULT 매핑. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_MEDIUM_ERROR;
			/* [한국어] 매체 오류군. */
			*asc  = SPDK_SCSI_ASC_PERIPHERAL_DEVICE_WRITE_FAULT;
			/* [한국어] (0x03) "PERIPHERAL DEVICE WRITE FAULT". */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] qualifier 없음. */
			break;
		case SPDK_NVME_SC_UNRECOVERED_READ_ERROR:
			/* [한국어] read 복구 불가능 매체 오류 (URE) — uncorrectable. SCSI 표준 매핑. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_MEDIUM_ERROR;
			/* [한국어] 매체 오류. */
			*asc  = SPDK_SCSI_ASC_UNRECOVERED_READ_ERROR;
			/* [한국어] (0x11) "UNRECOVERED READ ERROR" — RAID 레벨 복구 trigger 가능. */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] qualifier 없음. */
			break;
		case SPDK_NVME_SC_GUARD_CHECK_ERROR:
			/* [한국어] PI Guard(CRC16/T10 DIF guard tag) 검증 실패. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_MEDIUM_ERROR;
			/* [한국어] 매체 오류군 (PI 손상도 매체 손상으로 간주). */
			*asc  = SPDK_SCSI_ASC_LOGICAL_BLOCK_GUARD_CHECK_FAILED;
			/* [한국어] (0x10) "LOGICAL BLOCK GUARD CHECK FAILED". */
			*ascq = SPDK_SCSI_ASCQ_LOGICAL_BLOCK_GUARD_CHECK_FAILED;
			/* [한국어] (0x01) 동일 의미 qualifier — guard 실패 명확화. */
			break;
		case SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR:
			/* [한국어] PI Application Tag 검증 실패. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_MEDIUM_ERROR;
			/* [한국어] 매체 오류군. */
			*asc  = SPDK_SCSI_ASC_LOGICAL_BLOCK_APP_TAG_CHECK_FAILED;
			/* [한국어] (0x10) ASC + 다른 ASCQ로 app tag 실패 구분. */
			*ascq = SPDK_SCSI_ASCQ_LOGICAL_BLOCK_APP_TAG_CHECK_FAILED;
			/* [한국어] (0x02) "LOGICAL BLOCK APPLICATION TAG CHECK FAILED". */
			break;
		case SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR:
			/* [한국어] PI Reference Tag(보통 LBA 하위 32비트와 매칭) 검증 실패. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_MEDIUM_ERROR;
			/* [한국어] 매체 오류군. */
			*asc  = SPDK_SCSI_ASC_LOGICAL_BLOCK_REF_TAG_CHECK_FAILED;
			/* [한국어] ASC + ref tag qualifier로 어떤 PI 필드가 실패했는지 정확히 통신. */
			*ascq = SPDK_SCSI_ASCQ_LOGICAL_BLOCK_REF_TAG_CHECK_FAILED;
			/* [한국어] (0x03) "LOGICAL BLOCK REFERENCE TAG CHECK FAILED". */
			break;
		case SPDK_NVME_SC_COMPARE_FAILURE:
			/* [한국어] Compare 명령 또는 Compare-and-Write의 비교 실패. SCSI MISCOMPARE로 매핑. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_MISCOMPARE;
			/* [한국어] (0x0E) MISCOMPARE — verify/compare 결과 불일치. */
			*asc  = SPDK_SCSI_ASC_MISCOMPARE_DURING_VERIFY_OPERATION;
			/* [한국어] (0x1D) "MISCOMPARE DURING VERIFY OPERATION". */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] qualifier 없음. */
			break;
		case SPDK_NVME_SC_ACCESS_DENIED:
			/* [한국어] 매체 보호 정책에 의해 거부 (예: namespace lock). SCSI ACCESS DENIED. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_DATA_PROTECT;
			/* [한국어] (0x07) DATA_PROTECT — 데이터 보호 정책. */
			*asc  = SPDK_SCSI_ASC_ACCESS_DENIED;
			/* [한국어] (0x20) ACCESS_DENIED. */
			*ascq = SPDK_SCSI_ASCQ_NO_ACCESS_RIGHTS;
			/* [한국어] (0x02) "NO ACCESS RIGHTS" — 권한 부족. */
			break;
		case SPDK_NVME_SC_DEALLOCATED_OR_UNWRITTEN_BLOCK:
		default:
			/* [한국어] deallocated/unwritten 블록 read (TRIM 영역 read) + 그 외 매체 에러.
			 *  SCSI에서는 deallocated read에 표준 매핑이 모호하므로 ILLEGAL_REQUEST 디폴트. */
			*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
			/* [한국어] sense 전달. */
			*sk   = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
			/* [한국어] 안전 디폴트. */
			*asc  = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
			/* [한국어] 세부 코드 없음. */
			*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
			/* [한국어] qualifier 없음. */
			break;
		}
		break;
		/* [한국어] SCT=MEDIA_ERROR 그룹 종료. */
	case SPDK_NVME_SCT_VENDOR_SPECIFIC:
	default:
		/* [한국어] SCT=0x7 VENDOR_SPECIFIC + 알 수 없는 SCT(향후 PATH_RELATED 등) 모두 안전 디폴트.
		 *  SCSI initiator가 NVMe 벤더 코드를 해석할 방법이 없으므로 일반 명령 오류로 보고. */
		*sc   = SPDK_SCSI_STATUS_CHECK_CONDITION;
		/* [한국어] sense 전달. */
		*sk   = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
		/* [한국어] 안전 디폴트. */
		*asc  = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
		/* [한국어] 세부 코드 없음. */
		*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
		/* [한국어] qualifier 없음. */
		break;
	}
}
