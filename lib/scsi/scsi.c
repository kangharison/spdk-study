/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SCSI 서브시스템 진입점, LUN ID 포맷 변환, opcode 문자열 (scsi.c)
 *
 * === 파일의 역할 ===
 * SPDK SCSI 서브시스템의 라이브러리 레벨 초기화/정리(spdk_scsi_init/fini)와
 * SCSI 명령 처리에 필요한 보조 기능을 제공한다. 구체적으로:
 *   1) SPDK trace 프레임워크에 SCSI 관련 trace point(SCSI_TASK_START/DONE) 등록.
 *   2) SCSI Logical Unit Number의 정수 표현(int)과 8바이트 LUN 포맷(SAM-3) 사이 변환.
 *   3) SBC opcode → 사람이 읽을 문자열 매핑(디버그 로그용).
 *   4) SPDK_LOG_REGISTER_COMPONENT(scsi)로 "scsi" 로그 컴포넌트 등록.
 * 본 파일에는 SCSI 명령 처리 루프나 PR 로직이 포함되지 않으며, 순전히 부가 인프라이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * spdk_subsystem_init() 시점에 spdk_scsi_init()이 호출되며, 이는 현재 no-op이지만
 * SPDK_TRACE_REGISTER_FN으로 등록된 scsi_trace()는 spdk_trace_init() 시점에 한번 실행되어
 * trace 슬롯을 미리 잡아둔다. lun_id 변환 함수는 iSCSI/vhost-scsi 프론트엔드가
 * 8바이트 LUN(SAM-3 5.1 — peripheral device addressing 등)을 정수 인덱스로 디코드할 때 사용.
 * 실행 컨텍스트: 초기화 함수는 메인 스레드, 변환 함수는 임의 SPDK 스레드.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: scsi_internal.h, spdk/util.h, spdk/trace.h, spdk/log.h.
 * - 데이터 흐름: 8B LUN(와이어) ↔ int (내부 인덱스); SBC opcode 바이트 → 문자열(로그).
 * - 공유: trace 등록은 전역 trace 테이블에 영향. SPDK_LOG_REGISTER_COMPONENT는 전역 로그 등록.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_scsi_init/fini             : 현재 no-op이지만 향후 hook용 자리.
 * - scsi_trace                      : 'd'/'t' 심볼로 SCSI 추적 객체 등록.
 * - spdk_scsi_lun_id_int_to_fmt     : int → 8B LUN (SAM-3 addressing method 0x00/0x01).
 * - spdk_scsi_lun_id_fmt_to_int     : 8B LUN → int (역변환).
 * - scsi_sbc_opcode_strings[]       : SBC opcode → 문자열 룩업 테이블.
 * - spdk_scsi_sbc_opcode_string     : 위 테이블 선형 검색 후 문자열 반환.
 */

#include "scsi_internal.h"
/* [한국어] SCSI 내부 정의 — spdk_scsi_*, TRACE_SCSI_TASK_* 매크로 등 사용. */
#include "spdk/util.h"
/* [한국어] SPDK_COUNTOF 매크로(배열 원소 수 컴파일타임 계산) 등 사용. */

/*
 * [한국어]
 * spdk_scsi_init - SCSI 서브시스템 초기화 진입점
 *
 * @return: 0 (현재 항상 성공). 향후 동적 초기화가 추가되면 음수 errno를 반환할 수 있다.
 *
 * SPDK 서브시스템 디스패처가 부팅 단계에서 호출한다. 현재는 추가 작업이 필요 없어 no-op.
 * 실행 컨텍스트: 메인 스레드(서브시스템 초기화 단계).
 *
 * 호출 체인:
 *   spdk_subsystem_init → "scsi" 서브시스템 → [spdk_scsi_init]
 */
int
spdk_scsi_init(void)
{
	return 0;
	/* [한국어] 별도 초기화 동작 없음 — 단순 성공 반환. */
}

/*
 * [한국어]
 * spdk_scsi_fini - SCSI 서브시스템 정리 진입점
 *
 * SPDK 서브시스템 디스패처의 종료 시점에 호출. 현재 정리할 전역 자원이 없어 no-op.
 *
 * 호출 체인:
 *   spdk_subsystem_fini → "scsi" 서브시스템 → [spdk_scsi_fini]
 */
void
spdk_scsi_fini(void)
{
	/* [한국어] 향후 전역 자원이 생기면 여기서 해제. */
}

/*
 * [한국어]
 * scsi_trace - SPDK trace 프레임워크에 SCSI 관련 trace point를 등록
 *
 * SPDK_TRACE_REGISTER_FN(scsi_trace, ...)로 등록된 후 spdk_trace_init() 시점에 호출된다.
 * 두 종류의 trace point를 등록한다:
 *   - SCSI_TASK_START: scsi_lun.c가 task 실행 직전에 기록.
 *   - SCSI_TASK_DONE : scsi_lun.c가 task 완료 직후에 기록.
 * 또한 OWNER_TYPE_SCSI_DEV/OBJECT_SCSI_TASK 심볼을 'd'/'t' 단축 문자로 등록 — trace 시각화 도구에서 사용.
 *
 * 호출 체인:
 *   spdk_trace_init → 등록된 SPDK_TRACE_REGISTER_FN 모두 호출 → [scsi_trace]
 */
static void
scsi_trace(void)
{
	spdk_trace_register_owner_type(OWNER_TYPE_SCSI_DEV, 'd');
	/* [한국어] trace owner 타입 'SCSI dev'을 단문자 'd'로 등록 — 시각화 시 owner 컬럼 표시. */
	spdk_trace_register_object(OBJECT_SCSI_TASK, 't');
	/* [한국어] trace 객체 'SCSI task'를 단문자 't'로 등록. */
	spdk_trace_register_description("SCSI_TASK_DONE", TRACE_SCSI_TASK_DONE,
					OWNER_TYPE_SCSI_DEV, OBJECT_SCSI_TASK, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	/* [한국어] task 완료 이벤트의 디스플레이 이름/소유자/객체/인자 타입 등록. INT 인자 1개. */
	spdk_trace_register_description("SCSI_TASK_START", TRACE_SCSI_TASK_START,
					OWNER_TYPE_SCSI_DEV, OBJECT_SCSI_TASK, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	/* [한국어] task 시작 이벤트(보통 task->length 인자)도 동일 형식으로 등록. */
}
SPDK_TRACE_REGISTER_FN(scsi_trace, "scsi", TRACE_GROUP_SCSI)
/* [한국어] 정적 등록 매크로 — 컴포넌트명 "scsi", trace 그룹 SCSI로 위 scsi_trace 함수 등록. */

/*
 * [한국어]
 * spdk_scsi_lun_id_int_to_fmt - 정수 LUN id를 SAM-3 8바이트 LUN 포맷으로 변환
 *
 * @lun_id: 정수 LUN 인덱스(예: 0, 1, 2...).
 * @return: 8바이트 LUN 값(big-endian 의미를 가진 64비트 정수, 상위 비트가 method 코드).
 *
 * SAM-3 4.9.4 LUN addressing methods:
 *   - method 0x00 (Peripheral device addressing): LUN < 256, 상위 2비트 00, [55:48]에 8비트 LUN.
 *   - method 0x01 (Flat space addressing)       : LUN < 16384, 상위 2비트 01, [61:48]에 14비트 LUN.
 *   - 그 외(>=16384) 는 본 구현에서 미지원 → 0.
 * 인코딩은 64비트 정수 형태로 반환되며, 와이어로 나갈 때 to_be64로 빅엔디안 변환된다.
 *
 * 호출 체인:
 *   iSCSI/vhost-scsi REPORT LUNS 응답 작성 등 → [spdk_scsi_lun_id_int_to_fmt]
 */
uint64_t
spdk_scsi_lun_id_int_to_fmt(int lun_id)
{
	uint64_t fmt_lun, method;
	/* [한국어] fmt_lun: 결과; method: SAM-3 addressing method 코드(0x00/0x01). */

	if (lun_id < 0x0100) {
		/* below 256 */
		/* [한국어] 256 미만이면 method 0x00 (peripheral device addressing). */
		method = 0x00U;
		fmt_lun = (method & 0x03U) << 62;
		/* [한국어] 상위 2비트(63:62)에 method 코드 배치. */
		fmt_lun |= ((uint64_t)lun_id & 0x00ffU) << 48;
		/* [한국어] [55:48]에 8비트 LUN 값. 나머지 비트는 0. */
	} else if (lun_id < 0x4000) {
		/* below 16384 */
		/* [한국어] 256~16383: flat addressing(method 0x01). */
		method = 0x01U;
		fmt_lun = (method & 0x03U) << 62;
		/* [한국어] 상위 2비트에 method 0x01. */
		fmt_lun |= ((uint64_t)lun_id & 0x3fffU) << 48;
		/* [한국어] [61:48]에 14비트 LUN 값. */
	} else {
		/* XXX */
		/* [한국어] 16384 이상은 본 구현 범위 밖 — 0 반환(미지원 표시). */
		fmt_lun = 0;
	}

	return fmt_lun;
	/* [한국어] 호출자는 보통 to_be64()로 와이어에 쓴다. */
}

/*
 * [한국어]
 * spdk_scsi_lun_id_fmt_to_int - 8바이트 LUN 포맷을 정수 LUN id로 디코드
 *
 * @fmt_lun: 64비트 LUN 표현(host order — 와이어에서 from_be64 후 전달).
 * @return:  정수 LUN id (지원되지 않는 method면 0xFFFF).
 *
 * spdk_scsi_lun_id_int_to_fmt의 역. method 비트로 분기하여 LUN 영역을 추출한다.
 *
 * 호출 체인:
 *   iSCSI/vhost-scsi 명령 디코더(LUN 필드 파싱) → [spdk_scsi_lun_id_fmt_to_int]
 */
int
spdk_scsi_lun_id_fmt_to_int(uint64_t fmt_lun)
{
	uint64_t method;
	/* [한국어] addressing method 비트(63:62) 추출 결과. */
	int lun_i;
	/* [한국어] 디코드된 정수 LUN. */

	method = (fmt_lun >> 62) & 0x03U;
	/* [한국어] 상위 2비트만 마스크 — SAM-3 addressing method 코드. */
	fmt_lun = fmt_lun >> 48;
	/* [한국어] 의미 비트가 [61:48]에 있으니 16비트 우측 시프트로 정렬. */
	if (method == 0x00U) {
		lun_i = (int)(fmt_lun & 0x00ffU);
		/* [한국어] peripheral 방식: 하위 8비트가 LUN. */
	} else if (method == 0x01U) {
		lun_i = (int)(fmt_lun & 0x3fffU);
		/* [한국어] flat 방식: 하위 14비트가 LUN. */
	} else {
		lun_i = 0xffffU;
		/* [한국어] 미지원 method — 잘못된 LUN을 알리는 sentinel. */
	}
	return lun_i;
}

/*
 * [한국어]
 * struct scsi_sbc_opcode_string - SBC opcode 1바이트와 사람이 읽는 문자열의 매핑
 */
struct scsi_sbc_opcode_string {
	enum spdk_sbc_opcode opc;
	/* [한국어] SBC(SCSI Block Commands) opcode 값 (1바이트, scsi_spec.h enum 정의).
	 * 설정자: 정적 배열 초기화 시점에 고정.
	 * 읽는 자: spdk_scsi_sbc_opcode_string의 선형 검색.
	 * 값 범위: SPDK_SBC_* 열거자 (예: 0x28 READ_10, 0x2A WRITE_10).
	 * 동기화: 정적 const 데이터이므로 동기화 불필요. */
	const char *str;
	/* [한국어] 해당 opcode를 사람이 읽는 형태로 표현한 ASCII 문자열 리터럴.
	 * 설정자: 배열 초기화 시점에 고정.
	 * 읽는 자: 디버그 로그 출력 코드.
	 * 값 범위: 정적 문자열 리터럴 포인터(NULL 불가).
	 * 동기화: 정적 const 데이터, lock 불필요. */
};

/* [한국어] SBC opcode → 문자열 매핑 정적 테이블 — 컴파일 타임에 고정.
 *          spdk_scsi_sbc_opcode_string()이 선형 검색. NVMe와 달리 SCSI는 opcode 수가
 *          작아 선형 검색이 충분하다. 동기화 불필요(read-only). */
static const struct scsi_sbc_opcode_string scsi_sbc_opcode_strings[] = {
	{ SPDK_SBC_COMPARE_AND_WRITE, "COMPARE AND WRITE" },
	{ SPDK_SBC_FORMAT_UNIT, "FORMAT UNIT" },
	{ SPDK_SBC_GET_LBA_STATUS, "GET LBA STATUS" },
	{ SPDK_SBC_ORWRITE_16, "ORWRITE 16" },
	{ SPDK_SBC_PRE_FETCH_10, "PRE FETCH 10" },
	{ SPDK_SBC_PRE_FETCH_16, "PRE FETCH 16" },
	{ SPDK_SBC_READ_6, "READ 6" },
	{ SPDK_SBC_READ_10, "READ 10" },
	{ SPDK_SBC_READ_12, "READ 12" },
	{ SPDK_SBC_READ_16, "READ 16" },
	{ SPDK_SBC_READ_ATTRIBUTE, "READ ATTRIBUTE" },
	{ SPDK_SBC_READ_BUFFER, "READ BUFFER" },
	{ SPDK_SBC_READ_CAPACITY_10, "READ CAPACITY 10" },
	{ SPDK_SBC_READ_DEFECT_DATA_10, "READ DEFECT DATA 10" },
	{ SPDK_SBC_READ_DEFECT_DATA_12, "READ DEFECT DATA 12" },
	{ SPDK_SBC_READ_LONG_10, "READ LONG 10" },
	{ SPDK_SBC_REASSIGN_BLOCKS, "REASSIGN BLOCKS" },
	{ SPDK_SBC_SANITIZE, "SANITIZE" },
	{ SPDK_SBC_START_STOP_UNIT, "START STOP UNIT" },
	{ SPDK_SBC_SYNCHRONIZE_CACHE_10, "SYNCHRONIZE CACHE 10" },
	{ SPDK_SBC_SYNCHRONIZE_CACHE_16, "SYNCHRONIZE CACHE 16" },
	{ SPDK_SBC_UNMAP, "UNMAP" },
	{ SPDK_SBC_VERIFY_10, "VERIFY 10" },
	{ SPDK_SBC_VERIFY_12, "VERIFY 12" },
	{ SPDK_SBC_VERIFY_16, "VERIFY 16" },
	{ SPDK_SBC_WRITE_6, "WRITE 6" },
	{ SPDK_SBC_WRITE_10, "WRITE 10" },
	{ SPDK_SBC_WRITE_12, "WRITE 12" },
	{ SPDK_SBC_WRITE_16, "WRITE 16" },
	{ SPDK_SBC_WRITE_AND_VERIFY_10, "WRITE AND VERIFY 10" },
	{ SPDK_SBC_WRITE_AND_VERIFY_12, "WRITE AND VERIFY 12" },
	{ SPDK_SBC_WRITE_AND_VERIFY_16, "WRITE AND VERIFY 16" },
	{ SPDK_SBC_WRITE_LONG_10, "WRITE LONG 10" },
	{ SPDK_SBC_WRITE_SAME_10, "WRITE SAME 10" },
	{ SPDK_SBC_WRITE_SAME_16, "WRITE SAME 16" },
	{ SPDK_SBC_XDREAD_10, "XDREAD 10" },
	{ SPDK_SBC_XDWRITE_10, "XDWRITE 10" },
	{ SPDK_SBC_XDWRITEREAD_10, "XDWRITEREAD 10" },
	{ SPDK_SBC_XPWRITE_10, "XPWRITE 10" }
};

/*
 * [한국어]
 * spdk_scsi_sbc_opcode_string - SBC opcode를 사람이 읽는 문자열로 변환
 *
 * @opcode: CDB[0] (SBC opcode 1바이트).
 * @sa:     service action (가변 길이 CDB의 보조 식별자). 현재 본 구현에서는 미사용.
 * @return: 매칭되는 문자열, 없으면 "UNKNOWN".
 *
 * 디버그/에러 로그가 SCSI 명령을 사람이 알아볼 수 있게 출력할 때 사용.
 * 가변 길이 CDB의 service action 분기는 향후 추가 예정(FIXME 주석 참고).
 *
 * 호출 체인:
 *   bdev_scsi_execute / 디버그 로그 → [spdk_scsi_sbc_opcode_string]
 */
const char *
spdk_scsi_sbc_opcode_string(uint8_t opcode, uint16_t sa)
{
	uint8_t i;
	/* [한국어] 테이블 순회 인덱스. */

	/* FIXME: sa is unsupported currently, support variable length CDBs if necessary */
	for (i = 0; i < SPDK_COUNTOF(scsi_sbc_opcode_strings); i++) {
		/* [한국어] 정적 테이블 길이만큼 선형 검색 — opcode 수가 적어 효율 충분. */
		if (scsi_sbc_opcode_strings[i].opc == opcode) {
			return scsi_sbc_opcode_strings[i].str;
			/* [한국어] 첫 매칭 즉시 반환. */
		}
	}

	return "UNKNOWN";
	/* [한국어] 테이블에 없는 opcode는 "UNKNOWN" 으로 표시 — 호출자 측 로그 가독성 유지. */
}

SPDK_LOG_REGISTER_COMPONENT(scsi)
/* [한국어] "scsi" 로그 컴포넌트 등록 매크로 — SPDK_DEBUGLOG(scsi, ...) 사용 가능.
 *          런타임에 "rpc.py log_set_flag scsi"로 활성화 가능. */
