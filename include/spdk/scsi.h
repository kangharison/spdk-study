/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * SCSI to bdev translation layer
 */

/*
 * [한국어 설명] SPDK SCSI 미들레이어 공개 API 헤더 (scsi.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK SCSI 미들레이어가 외부(주로 iSCSI target / vhost-scsi target)에 노출하는 공개 API를 정의한다.
 * SCSI 명령(CDB, Command Descriptor Block)을 받아 디코드한 뒤 SPDK bdev 계층의 spdk_bdev_io로 변환·발행하고,
 * 완료 시 NVMe 등 백엔드의 결과를 SCSI status/sense data로 다시 매핑하여 전달하는 게이트웨이 역할을 한다.
 * 즉 "SCSI(상위) <-> bdev(하위)" 사이의 번역 계층이며, 본 헤더는 그 입구(spdk_scsi_dev/lun/task/port API)를 제공한다.
 * 트랜스포트 비종속이므로 iSCSI/vhost-scsi 모두 동일한 dev/lun/task 추상화를 공유한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택에서 본 미들레이어는 다음 구간에 위치한다:
 *   [iSCSI/vhost-scsi target (lib/iscsi, lib/vhost)]
 *     -> [SCSI 미들레이어 (lib/scsi, 본 헤더)]
 *       -> [bdev 계층 (lib/bdev, spdk_bdev_io)]
 *         -> [bdev 모듈: NVMe / AIO / lvol / malloc 등]
 *           -> [실제 디바이스 또는 파일/메모리]
 * 호출 체인 측면에서 상위 트랜스포트 코드는 본 헤더의 spdk_scsi_dev_queue_task() / spdk_scsi_dev_queue_mgmt_task()
 * 로 진입하고, SCSI 미들레이어는 bdev API(spdk_bdev_read_blocks, spdk_bdev_unmap 등)를 호출한다.
 * 모든 task는 LUN의 spdk_thread(=특정 reactor)에서 단일 스레드로 처리되며 cross-thread 접근은 spdk_thread_send_msg()로
 * 직렬화된다 (lockless 설계의 근거).
 *
 * === 타 모듈과의 연결 ===
 * - 의존(아래로): spdk/bdev.h (블록 I/O 발행), spdk/queue.h (TAILQ 매크로),
 *   spdk_internal/trace_defs.h (성능 추적용 trace 포인트 정의),
 *   lib/bdev/scsi_nvme.c (백엔드가 NVMe인 경우 NVMe completion -> SCSI sense 변환).
 * - 의존(위로 - 본 헤더를 사용하는 측): lib/iscsi/* (iSCSI target), lib/vhost/vhost_scsi.c (vhost-scsi target).
 * 데이터 흐름: 트랜스포트 PDU/IO 요청 -> spdk_scsi_task 생성 -> spdk_scsi_dev_queue_task ->
 * lib/scsi/scsi_bdev.c가 CDB를 디코드해 spdk_bdev_io 발행 -> bdev 콜백에서 spdk_scsi_task의 status/sense 채움 ->
 * task->cpl_fn() 호출 -> 트랜스포트가 결과 PDU를 클라이언트로 전송.
 * 핵심 공유 자료구조: spdk_scsi_task (CDB/iov/sense의 컨테이너), spdk_scsi_lun (bdev claim 보유),
 * spdk_scsi_port (transport ID).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_scsi_init/fini: 미들레이어 전역 초기화·해제 (subsystem init 시 1회 호출).
 * - spdk_scsi_dev_construct[_ext]: 이름과 bdev 리스트로 SCSI device + LUN 묶음 생성. iSCSI target node당 1개.
 * - spdk_scsi_dev_add_lun[_ext]: 이미 존재하는 dev에 LUN을 추가/제거 (hot add 지원).
 * - spdk_scsi_dev_queue_task / queue_mgmt_task: 일반 I/O / task management(abort, reset) 진입점.
 * - spdk_scsi_task_construct / put / set_status / build_sense_data: task 생명주기·결과 코드 조작.
 * - spdk_scsi_port_create / port_set_iscsi_transport_id: initiator/target port와 그 transport ID 설정.
 * - spdk_scsi_lun_open / close: bdev descriptor 보유한 LUN handle 획득(I/O 채널 할당과 짝).
 * - 핵심 구조체 spdk_scsi_task: CDB, iov, sense_data, status, ref count, bdev_io 포인터를 묶는다.
 */

#ifndef SPDK_SCSI_H
#define SPDK_SCSI_H

/* [한국어] stdinc.h: SPDK 표준 인클루드 묶음 (stdint, stddef, stdbool 등) — POSIX/플랫폼 타입을 일괄 노출. */
#include "spdk/stdinc.h"

/* [한국어] bdev.h: 본 헤더가 spdk_bdev_io_wait_entry 등 bdev 타입을 직접 임베드하므로 필수.
 *         SCSI -> bdev 호출 경로의 핵심 의존성이다. */
#include "spdk/bdev.h"
/* [한국어] queue.h: TAILQ_ENTRY 매크로를 사용해 spdk_scsi_task를 LUN별 pending 큐에 연결하기 위해 포함.
 *         BSD-style intrusive list 매크로 (sys/queue.h 호환). */
#include "spdk/queue.h"

/* [한국어] trace_defs.h: SPDK 트레이싱 프레임워크가 사용하는 trace point ID 정의.
 *         성능 분석을 위해 SCSI task 진입/완료 등의 이벤트를 lockless ring으로 기록하기 위함. */
#include "spdk_internal/trace_defs.h"

#ifdef __cplusplus
/* [한국어] C++ 컴파일러에서 본 헤더를 포함할 때 C 링키지를 강제 — name mangling 방지. */
extern "C" {
#endif

/* Defines for SPDK tracing framework */
/* [한국어] 시스템 전체에서 동시에 존재할 수 있는 SCSI device의 최대 개수.
 *         trace 슬롯/통계 배열 크기 산정에 사용되며, iSCSI target node 수의 상한과도 연동됨. */
#define SPDK_SCSI_MAX_DEVS			1024
/* [한국어] 한 SCSI device가 가질 수 있는 port(initiator/target port)의 최대 개수.
 *         iSCSI에서는 보통 portal group 수만큼 target port를 갖는다. */
#define SPDK_SCSI_DEV_MAX_PORTS			4
/* [한국어] SCSI device 이름의 최대 길이(바이트). iSCSI target name(IQN) 등을 담기 위한 상한. */
#define SPDK_SCSI_DEV_MAX_NAME			255

/* [한국어] SCSI port 이름의 최대 길이. INQUIRY VPD 0x88(SCSI Ports)에 보고되는 값의 상한. */
#define SPDK_SCSI_PORT_MAX_NAME_LENGTH		255
/* [한국어] Transport ID 문자열의 최대 길이. SPC-4 Table-258 (iSCSI/SAS/FC 등 transport별 ID 포맷)을 담는 버퍼 크기. */
#define SPDK_SCSI_MAX_TRANSPORT_ID_LENGTH	255

/*
 * [한국어]
 * spdk_scsi_data_dir - SCSI task의 데이터 전송 방향을 표현하는 enum.
 *
 * SAM(SCSI Architecture Model)의 transfer direction 개념과 매핑된다.
 * 트랜스포트(iSCSI Data-In/Data-Out PDU, vhost-scsi descriptor)에서 받은 방향 비트를
 * 미들레이어 공통 표현으로 통일하기 위해 사용된다. spdk_scsi_task::dxfer_dir에 저장.
 */
enum spdk_scsi_data_dir {
	SPDK_SCSI_DIR_NONE = 0,
	/* [한국어] 데이터 전송이 없는 명령 (TEST UNIT READY, START STOP UNIT 등).
	 *         CDB만 전달되고 데이터 페이즈가 생략된다. */
	SPDK_SCSI_DIR_TO_DEV = 1,
	/* [한국어] Initiator -> Target 방향. 즉 WRITE 계열 (WRITE_6/10/12/16, WRITE_SAME, MODE_SELECT 등).
	 *         호스트가 보낸 데이터를 디바이스가 수신하여 저장한다. */
	SPDK_SCSI_DIR_FROM_DEV = 2,
	/* [한국어] Target -> Initiator 방향. 즉 READ 계열 (READ_6/10/12/16, INQUIRY, MODE_SENSE, REPORT_LUNS 등).
	 *         디바이스가 데이터를 호스트에 반환한다. */
};

/*
 * [한국어]
 * spdk_scsi_task_func - Task Management Function(TMF) 코드 enum.
 *
 * SAM-5의 task management function 정의를 그대로 추적한다. iSCSI에서는 SCSI Task Management
 * Function Request PDU의 function 필드로 전달되며, vhost-scsi에서는 별도 채널로 전달된다.
 * spdk_scsi_dev_queue_mgmt_task()에 입력되어 본 enum 값에 따라 abort/reset 경로가 분기한다.
 */
enum spdk_scsi_task_func {
	SPDK_SCSI_TASK_FUNC_ABORT_TASK = 0,
	/* [한국어] 특정 한 task만 중단. abort_id로 지정된 ITT(initiator task tag)와 일치하는 task만 cancel.
	 *         hot path에서 자주 발생하지는 않으나(타임아웃 등), 정확한 task 식별이 중요하다. */
	SPDK_SCSI_TASK_FUNC_ABORT_TASK_SET,
	/* [한국어] 해당 LUN에서 이 initiator가 발행한 모든 task를 중단. ACA 환경에서 사용. */
	SPDK_SCSI_TASK_FUNC_CLEAR_TASK_SET,
	/* [한국어] 해당 LUN의 모든 task(어느 initiator가 발행했든)를 중단. */
	SPDK_SCSI_TASK_FUNC_LUN_RESET,
	/* [한국어] LUN 단위 reset: 진행 중 task abort + UNIT ATTENTION 발생. iSCSI initiator의 lun_reset 요청에 응답. */
	SPDK_SCSI_TASK_FUNC_TARGET_RESET,
	/* [한국어] Target 전체(모든 LUN) reset. iSCSI session 단위의 가장 무거운 회복 절차. */
};

enum spdk_scsi_task_func {
	SPDK_SCSI_TASK_FUNC_ABORT_TASK = 0,
	SPDK_SCSI_TASK_FUNC_ABORT_TASK_SET,
	SPDK_SCSI_TASK_FUNC_CLEAR_TASK_SET,
	SPDK_SCSI_TASK_FUNC_LUN_RESET,
	SPDK_SCSI_TASK_FUNC_TARGET_RESET,
};

/*
 * SAM does not define the value for these service responses.  Each transport
 *  (i.e. SAS, FC, iSCSI) will map these value to transport-specific codes,
 *  and may add their own.
 */
/*
 * [한국어]
 * spdk_scsi_task_mgmt_resp - TMF 응답 코드 enum.
 *
 * SAM 자체는 구체적인 정수 값을 정하지 않으며, 각 트랜스포트(SAS, FC, iSCSI)가 자체 코드로 매핑한다.
 * SPDK는 미들레이어 공통 표현으로 본 enum을 정의해두고, 트랜스포트 어댑터(lib/iscsi)가
 * iSCSI Task Management Function Response PDU의 response 필드 등 전송 단위 코드로 다시 변환한다.
 * spdk_scsi_task::response 에 저장된다.
 */
enum spdk_scsi_task_mgmt_resp {
	SPDK_SCSI_TASK_MGMT_RESP_COMPLETE,
	/* [한국어] TMF 처리가 완료되었다는 일반적 성공 응답 (드라이버 내부 디폴트). */
	SPDK_SCSI_TASK_MGMT_RESP_SUCCESS,
	/* [한국어] 요청한 동작이 실제로 성공적으로 수행됨 (예: target task가 실제로 abort됨). */
	SPDK_SCSI_TASK_MGMT_RESP_REJECT,
	/* [한국어] 요청 자체가 거부됨 (잘못된 파라미터, 권한 없음 등). */
	SPDK_SCSI_TASK_MGMT_RESP_INVALID_LUN,
	/* [한국어] 지정한 LUN이 존재하지 않거나 활성화 상태가 아님. iSCSI에서 LU NOT EXISTS 매핑. */
	SPDK_SCSI_TASK_MGMT_RESP_TARGET_FAILURE,
	/* [한국어] 타겟 측 내부 오류로 TMF를 수행할 수 없음 (검사용 — 정상 경로에서는 거의 발생하지 않음). */
	SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED
	/* [한국어] 해당 TMF 코드를 지원하지 않음 (SPDK가 구현하지 않은 function). */
};

/* [한국어] 전방 선언: spdk_scsi_task 의 콜백 typedef가 자기 자신을 가리키므로 먼저 선언해 둔다. */
struct spdk_scsi_task;
/* [한국어] task 완료 콜백 시그니처. CDB 처리가 끝난 뒤 미들레이어가 호출하며,
 *         실제 구현은 iSCSI/vhost 트랜스포트 측이 등록 (예: iscsi_task_cpl).
 *         호출 컨텍스트: LUN의 spdk_thread (즉 task가 처리된 그 reactor). */
typedef void (*spdk_scsi_task_cpl)(struct spdk_scsi_task *task);
/* [한국어] task 메모리 해제 콜백. ref가 0에 도달했을 때 호출되어 트랜스포트가 task 자체를 free한다.
 *         완료 콜백(cpl_fn)과 분리된 이유: cpl_fn은 결과 보고만 담당, 실제 free는 ref count 기반으로 별도. */
typedef void (*spdk_scsi_task_free)(struct spdk_scsi_task *task);

/*
 * [한국어]
 * spdk_scsi_task - SPDK SCSI 미들레이어가 다루는 단일 task(요청)의 상태/페이로드 컨테이너.
 *
 * 트랜스포트(iSCSI/vhost)가 새 명령 PDU를 받으면 본 구조체를 할당해 채운 뒤
 * spdk_scsi_dev_queue_task() 또는 queue_mgmt_task()로 미들레이어에 넘긴다.
 * 미들레이어는 CDB를 디코드하고 spdk_bdev_io를 발행하며, 결과를 본 구조체의 status/sense_data/iov에
 * 채운 뒤 cpl_fn 콜백으로 보고한다. 완료 후 ref가 0이 되면 free_fn 콜백으로 메모리 해제.
 * 한 task는 항상 단일 spdk_thread(LUN 소유 thread)에서만 다뤄지므로 lockless이다.
 */
struct spdk_scsi_task {
	uint8_t				status;
	/* [한국어] SCSI status 코드 (SAM-4 Table-58: GOOD=0x00, CHECK_CONDITION=0x02, BUSY=0x08,
	 *         RESERVATION_CONFLICT=0x18, TASK_SET_FULL=0x28 등 — scsi_spec.h::spdk_scsi_status 참조).
	 *         설정자: 미들레이어 처리 코드(특히 lib/scsi/scsi_bdev.c)와 spdk_scsi_task_set_status().
	 *         읽는 자: 트랜스포트의 cpl_fn 구현 — 이 값으로 응답 PDU의 status 필드를 채운다. */
	uint8_t				function; /* task mgmt function */
	/* [한국어] Task Management Function 코드 (spdk_scsi_task_func 값 중 하나).
	 *         설정자: 트랜스포트가 TMF PDU 수신 시 spdk_scsi_dev_queue_mgmt_task 호출 전에 채움.
	 *         읽는 자: 미들레이어가 본 값에 따라 abort/reset 분기 처리. 일반 I/O task에서는 사용되지 않음. */
	uint8_t				response; /* task mgmt response */
	/* [한국어] TMF 응답 코드 (spdk_scsi_task_mgmt_resp 값 중 하나).
	 *         설정자: 미들레이어가 TMF 처리 완료 후 채움.
	 *         읽는 자: 트랜스포트의 cpl_fn 구현 — TMF 응답 PDU의 response 필드를 구성. */

	struct spdk_scsi_lun		*lun;
	/* [한국어] 이 task가 향하는 Logical Unit. dev:lun_id 쌍을 트랜스포트가 결정하여 채워둔다.
	 *         설정자: spdk_scsi_dev_queue_task 진입 시 LUN ID로 lookup하여 채워짐.
	 *         읽는 자: 미들레이어가 lun에 결합된 bdev_desc/io_channel로 spdk_bdev_io 발행.
	 *         값 범위: 유효한 LUN 포인터 또는 NULL(잘못된 LUN — process_null_lun 경로 진입). */
	struct spdk_scsi_port		*target_port;
	/* [한국어] 이 task를 수신한 target port (서버 측 endpoint 식별자, REPORT_LUNS/RTPG에 사용).
	 *         설정자: 트랜스포트가 task 생성 시 자기 portal에 매칭되는 port를 채움.
	 *         읽는 자: PERSISTENT_RESERVE 처리, INQUIRY VPD 0x88(SCSI Ports) 응답 생성. */
	struct spdk_scsi_port		*initiator_port;
	/* [한국어] 이 task를 발행한 initiator port (클라이언트 측 식별자, IQN+ISID 포함).
	 *         설정자: iSCSI session 수립 시 결정된 initiator port 포인터를 트랜스포트가 채움.
	 *         읽는 자: PR(persistent reservation) 권한 검사, abort/clear task set 시 같은 initiator 필터링. */

	spdk_scsi_task_cpl		cpl_fn;
	/* [한국어] 완료 콜백 함수 포인터. status/sense/iov가 채워진 뒤 미들레이어가 본 콜백을 호출한다.
	 *         설정자: spdk_scsi_task_construct() 호출 시 트랜스포트 측 함수가 등록.
	 *         읽는 자: 미들레이어 (모든 완료 경로의 끝). 실행 컨텍스트: LUN의 spdk_thread. */
	spdk_scsi_task_free		free_fn;
	/* [한국어] task 메모리 해제 콜백. 트랜스포트가 task pool에 반환하거나 free()하는 동작을 캡슐화.
	 *         설정자: spdk_scsi_task_construct()에서 등록.
	 *         읽는 자: spdk_scsi_task_put()이 ref 감소 후 0이면 호출. */

	uint32_t ref;
	/* [한국어] 참조 카운트. 한 task가 여러 비동기 단계(write_zeroes split, compare-and-write 두 페이즈 등)에
	 *         걸쳐 있을 때 안전하게 free 시점을 보장하기 위해 사용.
	 *         설정자: spdk_scsi_task_construct()=1로 초기화, task_get/put()이 +-1.
	 *         동기화: 본 task는 단일 spdk_thread에서만 다뤄지므로 atomic 불필요(SPDK lockless 원칙). */
	uint32_t transfer_len;
	/* [한국어] CDB가 요청한 전송 길이(바이트). READ_*은 LBA 수×블록크기, WRITE_*도 동일.
	 *         설정자: lib/scsi/scsi_bdev.c가 CDB 디코드 후 채움.
	 *         읽는 자: 버퍼 할당, allocation length 비교 (data_transferred ≤ transfer_len). */
	uint32_t dxfer_dir;
	/* [한국어] 데이터 전송 방향 (spdk_scsi_data_dir 값). NONE/TO_DEV/FROM_DEV.
	 *         설정자: 트랜스포트가 PDU 헤더로부터 결정하여 채움.
	 *         읽는 자: 미들레이어의 디코드 로직 — read/write 경로 분기. */
	uint32_t length;
	/* [한국어] 트랜스포트 단계에서 결정된 PDU 페이로드 길이(바이트). transfer_len과 다를 수 있음
	 *         (예: iSCSI MaxRecvDataSegmentLength로 분할된 경우). 디버깅/가드 검증 용도. */

	/**
	 * Amount of data actually transferred.  Can be less than requested
	 *  transfer_len - i.e. SCSI INQUIRY.
	 */
	uint32_t data_transferred;
	/* [한국어] 실제로 전송된 데이터의 바이트 수. transfer_len과 같지 않을 수 있다.
	 *         예: INQUIRY는 alloc_len만큼만 채우고 나머지는 truncate; READ_CAPACITY 8/16바이트.
	 *         설정자: 미들레이어 응답 생성 코드.
	 *         읽는 자: 트랜스포트가 PDU의 Data-In length를 채울 때 사용. */

	uint64_t offset;
	/* [한국어] 데이터 페이로드 내부의 현재 오프셋 또는 LBA 기반 오프셋(바이트).
	 *         iSCSI Data-Out PDU의 BufferOffset, 또는 split된 bdev_io 처리 시 누적 위치 추적용. */

	uint8_t *cdb;
	/* [한국어] Command Descriptor Block 포인터(가변 길이: 6/10/12/16 바이트).
	 *         설정자: 트랜스포트가 PDU 내부 또는 별도 버퍼에서 가리키도록 설정.
	 *         읽는 자: 미들레이어 디코더 — cdb[0]이 opcode, 이후는 SBC/SPC/SSC 스펙별 필드. */

	/**
	 * \internal
	 * Size of internal buffer or zero when iov.iov_base is not internally managed.
	 */
	uint32_t alloc_len;
	/* [한국어] 미들레이어가 자체 할당한 내부 응답 버퍼(iov.iov_base)의 크기.
	 *         값 범위: 0이면 외부에서 제공한 버퍼라 미들레이어가 free하지 않음.
	 *         사용처: INQUIRY/MODE_SENSE/REPORT_LUNS 등 응답을 미들레이어가 생성하는 경우 자체 할당 후 alloc_len 기록. */
	/**
	 * \internal
	 * iov is internal buffer. Use iovs to access elements of IO.
	 */
	struct iovec iov;
	/* [한국어] 단일 버퍼를 표현하는 내장 iovec(iov_base + iov_len). 보통 iovcnt==1 케이스에 사용.
	 *         외부 iovs[]가 따로 설정될 때는 보조용. */
	struct iovec caw_iov; /* used for compare and write */
	/* [한국어] COMPARE AND WRITE(0x89) 처리 시 비교용 버퍼를 보관하는 별도 iovec.
	 *         스펙상 buffer_size = 2 * num_blocks (앞=비교, 뒤=쓰기). 분리 보관해 디코드를 단순화. */
	struct iovec *iovs;
	/* [한국어] scatter-gather 형태의 데이터 버퍼 배열. iSCSI multi-PDU나 vhost 다중 segment에서 사용.
	 *         설정자: 트랜스포트가 PDU 수신 시 segment별로 채움.
	 *         읽는 자: spdk_bdev_*v API에 그대로 전달되어 zero-copy DMA가 가능. */
	uint16_t iovcnt;
	/* [한국어] iovs[] 의 유효 엔트리 수. 0이면 데이터 없는 명령. */

	uint8_t sense_data[32];
	/* [한국어] CHECK CONDITION 시 호스트로 반환되는 fixed/descriptor format sense data 버퍼(최대 32B).
	 *         설정자: spdk_scsi_task_build_sense_data()/set_status() 가 (sk, asc, ascq)로 채움.
	 *         읽는 자: 트랜스포트의 cpl_fn — Status PDU의 sense data 필드로 복사.
	 *         크기 32: SPC-4 fixed format(18B) + descriptor format용 여유. */
	size_t sense_data_len;
	/* [한국어] sense_data에 실제로 채워진 길이. 0이면 sense 없음(GOOD status). */

	void *bdev_io;
	/* [한국어] 본 task가 발행한 spdk_bdev_io 포인터(불투명 형). bdev 완료 콜백 -> SCSI 매핑에서 참조.
	 *         설정자: lib/scsi/scsi_bdev.c가 spdk_bdev_*() 호출 후 반환된 bdev_io를 저장.
	 *         읽는 자: 완료/abort 처리 시 추적. */

	TAILQ_ENTRY(spdk_scsi_task) scsi_link;
	/* [한국어] LUN의 pending task 큐 또는 abort 큐에 본 task를 연결하기 위한 BSD-style intrusive list 노드.
	 *         사용처: spdk_scsi_dev_has_pending_tasks() 순회, abort_task_set 시 일치 task 검색. */

	uint32_t abort_id;
	/* [한국어] ABORT_TASK TMF 처리 시 대상 task의 식별자(보통 ITT, initiator task tag).
	 *         설정자: 트랜스포트가 TMF task 생성 시 abort 대상의 ID를 채움.
	 *         읽는 자: 미들레이어가 pending 큐에서 동일 ID의 task를 찾아 abort. */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	/* [한국어] bdev_io 풀이 고갈되어 spdk_bdev_*() 호출이 -ENOMEM을 반환했을 때 재시도를 큐잉하는 노드.
	 *         spdk_bdev_queue_io_wait()로 등록되며, bdev_io 가용 시 콜백이 호출되어 재시도. */
};

/* [한국어] 불투명(opaque) 전방 선언: 본 헤더는 인터페이스만 노출하고, 구체 정의는 lib/scsi 내부(scsi_internal.h)에 둠.
 *         이를 통해 사용자가 내부 필드에 의존하지 못하도록 ABI를 안정화한다. */
struct spdk_scsi_port;     /* [한국어] initiator/target 양 끝의 SCSI port (transport ID + name + index). */
struct spdk_scsi_dev;      /* [한국어] SCSI device(가상 target node) — 이름과 LUN 리스트, port 리스트를 보유. */
struct spdk_scsi_lun;      /* [한국어] Logical Unit — 정확히 1개의 bdev claim과 I/O 채널 보유. */
struct spdk_scsi_lun_desc; /* [한국어] LUN open descriptor — 사용자 측 핸들. open/close 시 lifetime 관리. */

/* [한국어] LUN hot-remove 콜백 시그니처. 백엔드 bdev가 사라질 때 미들레이어가 등록된 사용자 콜백을 호출.
 *         예: bdev_nvme의 hotplug 제거 시 iSCSI session에 unit attention을 알리는 데 사용. */
typedef void (*spdk_scsi_lun_remove_cb_t)(struct spdk_scsi_lun *, void *);
/* [한국어] device destruct 비동기 완료 콜백. spdk_scsi_dev_destruct()는 모든 LUN의 io_channel 해제까지
 *         spdk_thread 메시지 왕복이 필요하므로 비동기이며, 끝났을 때 본 콜백으로 결과(rc)를 알린다. */
typedef void (*spdk_scsi_dev_destruct_cb_t)(void *cb_arg, int rc);

/*
 * [한국어]
 * spdk_scsi_init - SPDK SCSI 미들레이어를 초기화한다.
 *
 * @return: 성공 시 0, 실패 시 -1. 호출자는 -1이면 spdk_app_start 단계에서 실패 처리.
 *
 * subsystem 부트 시퀀스에서 1회 호출되어 trace point 등록 등 전역 상태를 준비한다.
 * iSCSI/vhost-scsi target subsystem이 본 함수를 의존하므로 그보다 먼저 실행되어야 한다.
 * 실행 컨텍스트: app init 스레드(보통 reactor 0). 멀티스레드 진입은 가정하지 않음.
 *
 * 호출 체인:
 *   spdk_app_start -> subsystem init (scsi) -> [spdk_scsi_init]
 */
int spdk_scsi_init(void);

/*
 * [한국어]
 * spdk_scsi_fini - SPDK SCSI 미들레이어 전역 상태를 해제한다.
 *
 * subsystem fini 단계에서 호출되며, 등록된 trace point 정리와 리소스 해제를 수행한다.
 * 모든 spdk_scsi_dev/lun이 사전에 destruct되어 있어야 한다(역순 셧다운 원칙).
 * 실행 컨텍스트: app shutdown 스레드.
 *
 * 호출 체인:
 *   spdk_app_stop -> subsystem fini (scsi) -> [spdk_scsi_fini]
 */
void spdk_scsi_fini(void);

/*
 * [한국어]
 * spdk_scsi_lun_get_id - 주어진 LUN의 정수형 LUN ID를 반환한다.
 *
 * @param lun: 조회 대상 LUN. 유효한 포인터여야 한다(NULL 비허용).
 * @return: 0 이상의 LUN ID. 트랜스포트가 REPORT_LUNS 응답이나 디버그 로그에 사용.
 *
 * LUN ID는 spdk_scsi_dev_construct/add_lun 시 사용자가 부여한 lun_id_list 의 값이다.
 * 호출 컨텍스트: 어느 spdk_thread에서든 안전(read-only 필드 접근).
 */
int spdk_scsi_lun_get_id(const struct spdk_scsi_lun *lun);

/*
 * [한국어]
 * spdk_scsi_lun_get_bdev_name - LUN이 claim한 bdev의 이름 문자열을 반환한다.
 *
 * @param lun: 조회 대상 LUN.
 * @return: NUL-terminated bdev 이름. 수명은 LUN의 lifetime과 동일.
 *
 * 디버그/RPC dump 용도로 주로 사용된다 (예: scsi_get_devices RPC 응답).
 */
const char *spdk_scsi_lun_get_bdev_name(const struct spdk_scsi_lun *lun);

/*
 * [한국어]
 * spdk_scsi_lun_get_dev - LUN이 속한 상위 SCSI device를 반환한다.
 *
 * @param lun: 조회 대상 LUN.
 * @return: dev 포인터 (const). LUN이 free되기 전까지 유효.
 *
 * LUN -> dev 역참조가 필요한 경우(이름 출력, port 조회 등)에 사용.
 */
const struct spdk_scsi_dev *spdk_scsi_lun_get_dev(const struct spdk_scsi_lun *lun);

/*
 * [한국어]
 * spdk_scsi_lun_is_removing - LUN이 hot-remove 진행 중인지 확인한다.
 *
 * @param lun: 조회 대상 LUN.
 * @return: hot-remove가 시작된 상태이면 true, 아니면 false.
 *
 * 새로운 task를 발행하기 전에 호출하여, true면 트랜스포트는 즉시 CHECK_CONDITION + UNIT_ATTENTION 응답을 보낸다.
 * bdev hot-unplug 처리 도중 in-flight task의 race를 막는 가드 역할.
 */
bool spdk_scsi_lun_is_removing(const struct spdk_scsi_lun *lun);

/*
 * [한국어]
 * spdk_scsi_dev_get_name - SCSI device 이름 문자열을 반환한다.
 *
 * @param dev: 조회 대상 device.
 * @return: 성공 시 NUL-terminated 이름, 실패(NULL dev 등) 시 NULL.
 *
 * 보통 iSCSI target node IQN 또는 vhost-scsi controller 이름이 그대로 사용된다.
 * 길이 한도: SPDK_SCSI_DEV_MAX_NAME(255).
 */
const char *spdk_scsi_dev_get_name(const struct spdk_scsi_dev *dev);

/*
 * [한국어]
 * spdk_scsi_dev_get_id - SCSI device의 정수 ID를 반환한다.
 *
 * @param dev: 조회 대상 device.
 * @return: 0 ~ SPDK_SCSI_MAX_DEVS-1 범위의 ID.
 *
 * 미들레이어 내부 device 배열의 인덱스로 사용되며, trace event/debug 용도로 노출된다.
 */
int spdk_scsi_dev_get_id(const struct spdk_scsi_dev *dev);

/*
 * [한국어]
 * spdk_scsi_dev_get_lun - 주어진 device 안에서 lun_id에 해당하는 LUN을 찾는다.
 *
 * @param dev: 검색 대상 device.
 * @param lun_id: 조회할 LUN ID(정수형, dev_construct 시 부여한 값).
 * @return: 일치하는 LUN 포인터, 없으면 NULL.
 *
 * REPORT_LUNS 외 명령에서 CDB의 LUN 필드를 LUN 객체로 매핑할 때 사용.
 * 호출 빈도: hot path (모든 SCSI command 진입 시).
 */
struct spdk_scsi_lun *spdk_scsi_dev_get_lun(struct spdk_scsi_dev *dev, int lun_id);

/*
 * [한국어]
 * spdk_scsi_dev_get_first_lun - device의 LUN 리스트에서 첫 번째 LUN을 반환한다.
 *
 * @param dev: 순회 대상 device.
 * @return: 첫 LUN 포인터, LUN이 하나도 없으면 NULL.
 *
 * for-each 패턴 진입점: 보통 get_first_lun + get_next_lun 조합으로 모든 LUN 순회.
 * REPORT_LUNS 응답 구성, RPC dump 등에 사용.
 */
struct spdk_scsi_lun *spdk_scsi_dev_get_first_lun(struct spdk_scsi_dev *dev);

/*
 * [한국어]
 * spdk_scsi_dev_get_next_lun - 이전 LUN의 다음 LUN을 반환한다.
 *
 * @param prev_lun: 이전 LUN 포인터(NULL 비허용).
 * @return: 다음 LUN, 마지막이면 NULL.
 *
 * 리스트 순회용. 내부 자료구조(TAILQ)의 next 포인터를 따른다 — 순회 중 LUN 추가/삭제는 안전하지 않으므로
 * 호출자는 동일 spdk_thread에서 read-only로 사용해야 한다.
 */
struct spdk_scsi_lun *spdk_scsi_dev_get_next_lun(struct spdk_scsi_lun *prev_lun);

/*
 * [한국어]
 * spdk_scsi_dev_has_pending_tasks - device에 진행 중 task가 있는지 확인한다.
 *
 * @param dev: 조회 대상 device.
 * @param initiator_port: 특정 initiator만 검사하면 그 port, 모든 initiator를 검사하려면 NULL.
 * @return: 한 개 이상의 pending task가 있으면 true.
 *
 * shutdown/hot-remove/세션 종료 처리 중 inflight I/O가 모두 drain되었는지 polling하는 용도.
 * iSCSI target이 graceful logout 처리 시 본 함수를 반복 호출하며 모두 비워질 때까지 대기.
 */
bool spdk_scsi_dev_has_pending_tasks(const struct spdk_scsi_dev *dev,
				     const struct spdk_scsi_port *initiator_port);

/*
 * [한국어]
 * spdk_scsi_dev_destruct - SCSI device를 비동기적으로 해제한다.
 *
 * @param dev: 해제 대상 device.
 * @param cb_fn: 해제 완료 시 호출될 콜백 (rc=0이면 성공).
 * @param cb_arg: cb_fn에 전달될 사용자 컨텍스트.
 *
 * 동작: 모든 LUN을 차례로 destruct하고, 각 LUN의 io_channel을 그것이 할당된 spdk_thread에서 해제하기 위해
 * spdk_thread_send_msg로 메시지 왕복을 수행한다. 이 때문에 동기 반환이 불가능하며 비동기 콜백 모델을 채택.
 * 호출자: iSCSI target node 삭제, vhost controller 제거, app shutdown 경로.
 *
 * 호출 체인:
 *   iscsi_tgt_node_destruct -> spdk_scsi_dev_destruct -> (LUN별 io_channel 해제 메시지) -> cb_fn
 */
void spdk_scsi_dev_destruct(struct spdk_scsi_dev *dev,
			    spdk_scsi_dev_destruct_cb_t cb_fn, void *cb_arg);

/*
 * [한국어]
 * spdk_scsi_dev_queue_mgmt_task - Task Management Function task를 미들레이어에 enqueue한다.
 *
 * @param dev: 대상 SCSI device.
 * @param task: TMF task. 호출 전에 spdk_scsi_task_construct()로 구성되고 task->function에 TMF 코드가 채워져야 함.
 *
 * 미들레이어는 task->function 값에 따라 abort_task / abort_task_set / clear_task_set / lun_reset / target_reset
 * 분기를 수행하고, task->response에 결과를 기록한 뒤 task->cpl_fn을 호출한다.
 * 실행 컨텍스트: 트랜스포트 polling thread (iSCSI poll group의 reactor).
 * 동기성: 일부 분기(LUN/target reset)는 LUN의 thread로 메시지 왕복이 필요하므로 비동기 완료가 일반적.
 *
 * 호출 체인:
 *   iscsi_pdu_hdr_op_task -> spdk_scsi_task_construct -> spdk_scsi_dev_queue_mgmt_task -> ...
 */
void spdk_scsi_dev_queue_mgmt_task(struct spdk_scsi_dev *dev, struct spdk_scsi_task *task);

/*
 * [한국어]
 * spdk_scsi_dev_queue_task - 일반 SCSI task(I/O command)를 미들레이어에 enqueue한다.
 *
 * @param dev: 대상 SCSI device.
 * @param task: spdk_scsi_task_construct()로 구성된 task. cdb/iov/transfer_len/dxfer_dir이 모두 설정되어 있어야 함.
 *
 * 본 함수는 모든 read/write/inquiry 등 비-TMF 명령의 단일 진입점이다.
 * 동작: LUN lookup -> hot-remove 검사 -> CDB opcode 디코드 -> spdk_bdev_io 발행 -> 비동기 완료 -> cpl_fn 콜백.
 * 호출 빈도: 매우 hot path. 모든 SCSI READ/WRITE에 대해 호출됨.
 * 실행 컨텍스트: 트랜스포트 polling thread 진입 후 LUN owner thread로 메시지 디스패치.
 *
 * 호출 체인:
 *   iscsi_pdu_payload_op_scsi -> [spdk_scsi_dev_queue_task] -> scsi_lun_execute_task -> bdev API
 */
void spdk_scsi_dev_queue_task(struct spdk_scsi_dev *dev, struct spdk_scsi_task *task);

/*
 * [한국어]
 * spdk_scsi_dev_add_port - SCSI device에 새 port를 추가한다.
 *
 * @param dev: 대상 device.
 * @param id: 새 port의 식별자(64-bit). 트랜스포트별 portal/연결 식별자로 매핑.
 * @param name: port 이름 (NUL-terminated). 길이 ≤ SPDK_SCSI_PORT_MAX_NAME_LENGTH.
 * @return: 0 성공, -1 실패(중복 ID, 슬롯 부족 등).
 *
 * iSCSI target node 생성 시 portal group마다 1회 호출되어 target port를 등록한다.
 * dev당 최대 SPDK_SCSI_DEV_MAX_PORTS(4) 개.
 */
int spdk_scsi_dev_add_port(struct spdk_scsi_dev *dev, uint64_t id, const char *name);

/*
 * [한국어]
 * spdk_scsi_dev_delete_port - device에서 지정 port를 제거한다.
 *
 * @param dev: 대상 device.
 * @param id: 제거할 port의 ID.
 * @return: 0 성공, -1 실패(존재하지 않는 ID).
 *
 * portal group 삭제 또는 target node 변경 시 호출.
 */
int spdk_scsi_dev_delete_port(struct spdk_scsi_dev *dev, uint64_t id);

/*
 * [한국어]
 * spdk_scsi_dev_find_port_by_id - ID로 device의 port를 검색한다.
 *
 * @param dev: 검색 대상 device.
 * @param id: port ID.
 * @return: 일치하는 port 포인터, 없으면 NULL.
 *
 * 새 task가 도착했을 때 어떤 target port에 도착했는지 매핑하는 데 사용.
 */
struct spdk_scsi_port *spdk_scsi_dev_find_port_by_id(struct spdk_scsi_dev *dev, uint64_t id);

/*
 * [한국어]
 * spdk_scsi_dev_allocate_io_channels - device의 모든 LUN에 대해 I/O channel을 할당한다.
 *
 * @param dev: 대상 device.
 * @return: 0 성공, -1 실패(어느 LUN이든 채널 할당 실패).
 *
 * 본 함수는 LUN별 spdk_bdev_get_io_channel()을 호출하여 현재 spdk_thread에서 사용할 채널을 확보한다.
 * iSCSI/vhost connection이 처음 SCSI command를 내기 전에 호출 — bdev I/O는 채널이 없으면 발행 불가.
 */
int spdk_scsi_dev_allocate_io_channels(struct spdk_scsi_dev *dev);

/*
 * [한국어]
 * spdk_scsi_dev_free_io_channels - device의 모든 LUN의 I/O channel을 해제한다.
 *
 * @param dev: 대상 device.
 *
 * connection 종료/disconnect 시 호출. allocate와 짝을 이루어야 하며, 동일 spdk_thread에서 호출되어야 안전.
 */
void spdk_scsi_dev_free_io_channels(struct spdk_scsi_dev *dev);

/*
 * [한국어]
 * spdk_scsi_dev_construct - 이름과 bdev/LUN ID 리스트로 SCSI device 객체를 생성한다.
 *
 * @param name: 새 device 이름 (NUL-terminated, 길이 ≤ SPDK_SCSI_DEV_MAX_NAME).
 * @param bdev_name_list: 각 LUN에 attach할 bdev 이름 배열. bdev_name_list[i] 는 lun_id_list[i] 의 백엔드.
 * @param lun_id_list: 각 LUN의 ID 배열. 호출자가 메모리를 관리한다(생성 후 free 가능).
 * @param num_luns: bdev_name_list / lun_id_list 의 엔트리 수.
 * @param protocol_id: INQUIRY 응답에 보고될 SPC protocol identifier (예: ISCSI=0x05, SAS=0x06).
 *                    scsi_spec.h::SPDK_SPC_PROTOCOL_IDENTIFIER_* 참고.
 * @param hotremove_cb: 임의 LUN의 백엔드 bdev가 사라질 때 1회 호출되는 콜백.
 * @param hotremove_ctx: hotremove_cb의 사용자 컨텍스트.
 * @return: 생성된 device 포인터, 실패 시 NULL.
 *
 * 동작 단계: 이름/슬롯 확보 -> 각 (bdev, LUN id) 쌍에 대해 spdk_scsi_dev_add_lun 반복 호출 -> dev 자료구조 등록.
 * resize 콜백 미지원 버전이며, 신규 코드는 _ext 변형 사용을 권장.
 *
 * 호출 체인:
 *   iSCSI target_node 생성 RPC -> [spdk_scsi_dev_construct] -> spdk_scsi_dev_add_lun(N회)
 */
struct spdk_scsi_dev *spdk_scsi_dev_construct(const char *name,
		const char *bdev_name_list[],
		int *lun_id_list,
		int num_luns,
		uint8_t protocol_id,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx);

/*
 * [한국어]
 * spdk_scsi_dev_construct_ext - construct의 확장 버전. resize 콜백을 추가로 받는다.
 *
 * @param name: device 이름.
 * @param bdev_name_list: LUN별 bdev 이름 배열.
 * @param lun_id_list: LUN ID 배열.
 * @param num_luns: 엔트리 수.
 * @param protocol_id: SPC protocol identifier.
 * @param resize_cb: LUN의 백엔드 bdev 용량이 변경되었을 때(동적 lvol 확장 등) 호출되는 콜백.
 *                   상위 트랜스포트는 이 콜백을 받아 host에 CAPACITY DATA HAS CHANGED unit attention 발행.
 * @param resize_ctx: resize_cb의 사용자 컨텍스트.
 * @param hotremove_cb: LUN hot-removal 콜백.
 * @param hotremove_ctx: hotremove_cb의 사용자 컨텍스트.
 * @return: 생성된 device 포인터, 실패 시 NULL.
 *
 * lvol resize / dynamic capacity 시나리오를 지원하기 위해 추가된 변형.
 */
struct spdk_scsi_dev *spdk_scsi_dev_construct_ext(const char *name,
		const char *bdev_name_list[],
		int *lun_id_list,
		int num_luns,
		uint8_t protocol_id,
		void (*resize_cb)(const struct spdk_scsi_lun *, void *),
		void *resize_ctx,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx);

/*
 * [한국어]
 * spdk_scsi_dev_delete_lun - device에서 특정 LUN을 제거한다.
 *
 * @param dev: 대상 device.
 * @param lun: 제거할 LUN.
 *
 * 동작: lun을 dev의 LUN 배열에서 unlink하고, in-flight task drain 후 lun 자체를 destruct.
 * dev 자체는 살아있으며, 마지막 LUN이 제거되어도 dev는 존재한다.
 * 사용처: 동적 LUN 제거 RPC.
 */
void spdk_scsi_dev_delete_lun(struct spdk_scsi_dev *dev, struct spdk_scsi_lun *lun);

/*
 * [한국어]
 * spdk_scsi_dev_add_lun - 기존 device에 새 LUN을 추가한다 (resize 콜백 없음).
 *
 * @param dev: 대상 device.
 * @param bdev_name: LUN의 백엔드로 사용할 bdev 이름.
 * @param lun_id: 새 LUN의 ID. dev 안에서 유일해야 하며 0 이상.
 * @param hotremove_cb: bdev가 hot-remove될 때 호출되는 콜백.
 * @param hotremove_ctx: 콜백 컨텍스트.
 * @return: 0 성공, 음수 errno 실패(중복 ID, bdev 미존재, claim 실패 등).
 *
 * 내부에서 spdk_bdev_open_ext + spdk_scsi_lun_construct 등을 호출해 bdev claim과 LUN 객체 생성을 일괄 처리.
 */
int spdk_scsi_dev_add_lun(struct spdk_scsi_dev *dev, const char *bdev_name, int lun_id,
			  void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
			  void *hotremove_ctx);

/*
 * [한국어]
 * spdk_scsi_dev_add_lun_ext - add_lun의 확장: resize 콜백을 추가로 받는다.
 *
 * @param dev: 대상 device.
 * @param bdev_name: 백엔드 bdev 이름.
 * @param lun_id: LUN ID.
 * @param resize_cb: bdev 용량 변경 시 호출(상위 트랜스포트는 capacity-changed UA 발행 트리거).
 * @param resize_ctx: resize_cb 컨텍스트.
 * @param hotremove_cb: hot-remove 콜백.
 * @param hotremove_ctx: 콜백 컨텍스트.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 동적 lvol resize 등 신규 시나리오에서는 본 _ext 버전 사용을 권장.
 */
int spdk_scsi_dev_add_lun_ext(struct spdk_scsi_dev *dev, const char *bdev_name, int lun_id,
			      void (*resize_cb)(const struct spdk_scsi_lun *, void *),
			      void *resize_ctx,
			      void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
			      void *hotremove_ctx);

/*
 * [한국어]
 * spdk_scsi_port_create - 새 SCSI port 객체를 동적으로 생성한다.
 *
 * @param id: port의 64-bit 식별자(트랜스포트 결정).
 * @param index: dev 내부에서 port를 식별하는 인덱스 (0 ~ SPDK_SCSI_DEV_MAX_PORTS-1).
 * @param name: port 이름 (NUL-terminated).
 * @return: 생성된 port 포인터, 실패 시 NULL.
 *
 * dev_add_port와 달리 dev에 attach되지 않는 자유 port를 만든다 — initiator port 생성에 주로 쓰인다.
 * 해제는 spdk_scsi_port_free()로 한다.
 */
struct spdk_scsi_port *spdk_scsi_port_create(uint64_t id, uint16_t index, const char *name);

/*
 * [한국어]
 * spdk_scsi_port_free - port 객체를 해제하고 포인터를 NULL로 설정한다.
 *
 * @param pport: port 포인터의 포인터 (이중 포인터). 함수 내부에서 *pport = NULL로 마무리하여
 *              double-free를 방지한다.
 *
 * port_create와 짝. dev에 attach된 port는 dev_destruct이 자동으로 정리하므로 호출자가 직접 free할 필요 없음.
 */
void spdk_scsi_port_free(struct spdk_scsi_port **pport);

/*
 * [한국어]
 * spdk_scsi_port_get_name - port 이름 문자열을 반환한다.
 *
 * @param port: 조회 대상 port.
 * @return: NUL-terminated 이름 (port lifetime 동안 유효).
 *
 * INQUIRY VPD 0x88(SCSI Ports) 응답 구성, RPC dump, 디버그 로그 등에 사용.
 */
const char *spdk_scsi_port_get_name(const struct spdk_scsi_port *port);

/*
 * [한국어]
 * spdk_scsi_task_construct - 비할당된 spdk_scsi_task 메모리를 초기화하고 콜백을 등록한다.
 *
 * @param task: 호출자가 미리 할당한 task 메모리(zeroed 권장).
 * @param cpl_fn: 완료 시 호출되는 콜백 (트랜스포트별 결과 보고 함수).
 * @param free_fn: ref가 0이 되어 해제될 때 호출되는 콜백.
 *
 * 동작: ref=1, 기본 status=GOOD, sense_data_len=0 등으로 초기화하고 콜백을 저장한다.
 * 호출자는 본 함수 호출 후 cdb/iov/transfer_len/dxfer_dir 등 명령별 필드를 채워야 한다.
 *
 * 호출 체인:
 *   iSCSI: iscsi_task_get -> [spdk_scsi_task_construct] -> CDB 채움 -> spdk_scsi_dev_queue_task
 */
void spdk_scsi_task_construct(struct spdk_scsi_task *task,
			      spdk_scsi_task_cpl cpl_fn,
			      spdk_scsi_task_free free_fn);

/*
 * [한국어]
 * spdk_scsi_task_put - task의 참조 카운트를 1 감소시키고, 0이 되면 free_fn을 호출한다.
 *
 * @param task: 대상 task. 호출자가 더 이상 참조하지 않을 때 호출.
 *
 * 비대칭 race에 대비한 ref count 패턴. 한 task가 split되어 여러 bdev_io 완료를 기다릴 때
 * 각 단계마다 get/put 쌍으로 안전하게 관리한다.
 * 실행 컨텍스트: task의 LUN owner thread (즉 cpl_fn이 호출되는 스레드와 동일).
 */
void spdk_scsi_task_put(struct spdk_scsi_task *task);

/*
 * [한국어]
 * spdk_scsi_task_set_data - task의 내부 iov에 외부 버퍼를 연결한다 (소유권은 호출자).
 *
 * @param task: 대상 task.
 * @param data: 데이터 버퍼 시작 포인터.
 * @param len: 버퍼 길이(바이트).
 *
 * 동작: task->iov.iov_base = data, iov_len = len, iovs = &task->iov, iovcnt = 1, alloc_len = 0 (외부 소유 표시).
 * 외부에서 미리 할당한 응답 페이로드를 task에 부착할 때 사용. 미들레이어는 이 버퍼를 free하지 않는다.
 */
void spdk_scsi_task_set_data(struct spdk_scsi_task *task, void *data, uint32_t len);

/*
 * [한국어]
 * spdk_scsi_task_scatter_data - 단일 src 버퍼를 task의 iovs[]에 산포 복사한다.
 *
 * @param task: 대상 task (iovs/iovcnt가 미리 설정되어 있어야 함 — 트랜스포트가 수신 segment를 준비한 상태).
 * @param src: 소스 버퍼.
 * @param len: 소스 길이.
 * @return: 실제로 기록된 총 바이트 수, 실패 시 -1 (iovs 부족 등).
 *
 * 사용처: 미들레이어가 INQUIRY 등 응답을 단일 평면 버퍼로 만든 뒤 트랜스포트가 요구하는 sg 목록으로 흩뿌릴 때.
 */
int spdk_scsi_task_scatter_data(struct spdk_scsi_task *task, const void *src, size_t len);

/*
 * [한국어]
 * spdk_scsi_task_gather_data - task의 iovs[]를 단일 평면 버퍼로 복사·반환한다.
 *
 * @param task: 대상 task (iovs/iovcnt가 채워진 write 데이터 등).
 * @param len: out 파라미터 — 반환 버퍼 총 길이가 기록됨.
 * @return: malloc된 평면 버퍼 (호출자가 free 책임), 실패 시 NULL.
 *
 * 사용처: WRITE 데이터처럼 트랜스포트가 sg로 받아둔 페이로드를 미들레이어가 단일 버퍼로 처리해야 할 때
 * (예: COMPARE 검증, 응답 파싱). free 누수 주의.
 */
void *spdk_scsi_task_gather_data(struct spdk_scsi_task *task, int *len);

/*
 * [한국어]
 * spdk_scsi_task_build_sense_data - task->sense_data에 fixed-format sense를 구성한다.
 *
 * @param task: 대상 task.
 * @param sk: Sense Key (SPDK_SCSI_SENSE_*; 4-bit field). 예: ILLEGAL_REQUEST=0x05, NOT_READY=0x02.
 * @param asc: Additional Sense Code (1바이트). 예: INVALID_FIELD_IN_CDB=0x24.
 * @param ascq: ASC Qualifier (1바이트). asc와 합쳐 구체 오류를 식별.
 *
 * 동작: SPC-4 §4.5의 fixed-format response code 0x70 헤더 + valid bit + sense key + ASC/ASCQ를 채우고
 * sense_data_len을 18로 설정. 별도로 status를 변경하지는 않음(보통 set_status가 함께 호출됨).
 *
 * 자주 사용되는 (sk, asc, ascq) 조합:
 *   (5, 0x20, 0x00) Invalid command operation code
 *   (5, 0x24, 0x00) Invalid field in CDB
 *   (5, 0x21, 0x00) LBA out of range
 *   (2, 0x04, 0x01) Logical unit becoming ready
 *   (6, 0x29, 0x00) Power on, reset, or bus device reset (UA)
 */
void spdk_scsi_task_build_sense_data(struct spdk_scsi_task *task, int sk, int asc,
				     int ascq);

/*
 * [한국어]
 * spdk_scsi_task_set_status - task의 SCSI status를 설정하고, CHECK CONDITION이면 sense까지 채운다.
 *
 * @param task: 대상 task.
 * @param sc: SCSI status 코드 (spdk_scsi_status). GOOD=0x00, CHECK_CONDITION=0x02, BUSY=0x08 등.
 * @param sk: sense key (sc가 CHECK_CONDITION인 경우만 의미).
 * @param asc: ASC.
 * @param ascq: ASCQ.
 *
 * 가장 자주 사용되는 응답 빌더. CHECK_CONDITION 외 케이스에서 sk/asc/ascq는 무시된다.
 * 디코드 단계에서 잘못된 CDB 검출 등 모든 에러 경로의 종착점.
 */
void spdk_scsi_task_set_status(struct spdk_scsi_task *task, int sc, int sk, int asc,
			       int ascq);

/*
 * [한국어]
 * spdk_scsi_task_copy_status - 한 task의 status/sense를 다른 task로 복사한다.
 *
 * @param dst: 복사 대상 task (status가 덮어씌워짐).
 * @param src: 복사 원본 task.
 *
 * 사용처: WRITE_AND_VERIFY 같이 두 단계로 나누어진 명령에서 첫 단계(write)의 결과를 두 번째 단계(verify)로
 * 전달하거나, 합성 task의 결과를 부모 task에 보고할 때.
 */
void spdk_scsi_task_copy_status(struct spdk_scsi_task *dst, struct spdk_scsi_task *src);

/*
 * [한국어]
 * spdk_scsi_task_process_null_lun - 존재하지 않는 LUN에 도착한 task의 처리 헬퍼.
 *
 * @param task: 대상 task.
 *
 * 동작: REPORT_LUNS/INQUIRY 일부는 LUN0 아닌 곳에서도 응답해야 하는 SAM 규칙을 만족시키기 위해
 * task->cdb를 검사해 허용 여부를 결정하고, 그 외에는 (sk=0x05 ILLEGAL_REQUEST, asc=0x25 LU NOT SUPPORTED)
 * sense를 채워 즉시 완료 처리한다.
 *
 * 호출 체인:
 *   spdk_scsi_dev_queue_task -> LUN lookup 실패 -> [spdk_scsi_task_process_null_lun] -> task->cpl_fn
 */
void spdk_scsi_task_process_null_lun(struct spdk_scsi_task *task);

/*
 * [한국어]
 * spdk_scsi_task_process_abort - abort된 task에 대한 정리 헬퍼.
 *
 * @param task: abort 처리될 task.
 *
 * 동작: status=TASK_ABORTED(0x40)와 적절한 sense를 채우고 cpl_fn을 호출.
 * TMF ABORT_TASK / ABORT_TASK_SET / LUN_RESET 처리 경로에서 in-flight task들에 대해 호출됨.
 */
void spdk_scsi_task_process_abort(struct spdk_scsi_task *task);

/*
 * [한국어]
 * spdk_scsi_lun_open - LUN을 사용자(트랜스포트 connection)에 대해 open하고 descriptor를 반환한다.
 *
 * @param lun: open 대상 LUN.
 * @param hotremove_cb: bdev hot-remove 시 사용자 측 정리 절차를 수행하는 콜백.
 *                     이 콜백은 (a) 상위 계층(예: iSCSI) 의 모든 outstanding task를 완료시키고,
 *                     (b) 할당된 io_channel을 free하고, (c) lun을 close해야 한다.
 * @param hotremove_ctx: hotremove_cb의 사용자 컨텍스트.
 * @param desc: out 파라미터 — 성공 시 사용자에게 반환되는 LUN descriptor.
 * @return: 성공 시 0, 실패 시 양의 errno (LUN이 hot-removing 등).
 *
 * iSCSI session/connection이 새로 LUN을 사용하기 시작할 때 호출되어 lifetime token(desc)을 얻는다.
 * 한 LUN에 대해 여러 connection이 각자의 desc를 가질 수 있다.
 */
int spdk_scsi_lun_open(struct spdk_scsi_lun *lun, spdk_scsi_lun_remove_cb_t hotremove_cb,
		       void *hotremove_ctx, struct spdk_scsi_lun_desc **desc);

/*
 * [한국어]
 * spdk_scsi_lun_close - LUN descriptor를 닫는다 (open과 짝).
 *
 * @param desc: 닫을 descriptor. 호출 후 desc는 무효.
 *
 * 사용자가 더 이상 LUN을 참조하지 않을 때 호출. 마지막 desc가 close되고 hot-remove가 진행 중이라면
 * 미들레이어가 LUN destruct 절차를 마무리한다.
 */
void spdk_scsi_lun_close(struct spdk_scsi_lun_desc *desc);

/*
 * [한국어]
 * spdk_scsi_lun_allocate_io_channel - 현재 spdk_thread에서 LUN에 사용할 bdev I/O channel을 할당한다.
 *
 * @param desc: 대상 LUN descriptor.
 * @return: 0 성공, -1 실패.
 *
 * spdk_bdev_get_io_channel 의 wrapper. 실행 컨텍스트: 채널을 사용할 spdk_thread에서 직접 호출되어야 함.
 * cross-thread 호출 시 spdk_thread_send_msg로 dispatch 필요.
 */
int spdk_scsi_lun_allocate_io_channel(struct spdk_scsi_lun_desc *desc);

/*
 * [한국어]
 * spdk_scsi_lun_free_io_channel - LUN의 I/O channel을 해제한다.
 *
 * @param desc: 대상 descriptor.
 *
 * allocate와 짝. 채널을 할당한 동일 spdk_thread에서 호출해야 한다.
 */
void spdk_scsi_lun_free_io_channel(struct spdk_scsi_lun_desc *desc);

/*
 * [한국어]
 * spdk_scsi_lun_get_dif_ctx - 주어진 task의 DIF(Data Integrity Field) context를 초기화한다.
 *
 * @param lun: 대상 LUN.
 * @param task: 페이로드를 담은 task (CDB로 LBA/transfer length 추출).
 * @param dif_ctx: out 파라미터 — 성공 시 초기화된 DIF 컨텍스트가 채워짐.
 * @return: true 성공, false 실패(DIF 미지원 또는 잘못된 CDB).
 *
 * Type 1/2/3 DIF (T10 PI: Guard 16b + App Tag 16b + Ref Tag 32b)를 호스트-target 간에 검증할 때
 * 트랜스포트(iSCSI digest, NVMe-oF)가 본 컨텍스트로 PI 계산/검사를 수행한다.
 */
bool spdk_scsi_lun_get_dif_ctx(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task,
			       struct spdk_dif_ctx *dif_ctx);

/*
 * [한국어]
 * spdk_scsi_port_set_iscsi_transport_id - initiator port에 iSCSI Transport ID를 설정한다.
 *
 * @param port: 대상 initiator port.
 * @param iscsi_name: initiator 이름(IQN, NUL-terminated).
 * @param isid: iSCSI Session ID(48-bit, RFC 7143).
 *
 * SPC-4 Table-258 iSCSI Transport ID 포맷(format=0x40, "iqn.xxx,i,0xISID")으로 직렬화하여 port에 저장.
 * Persistent Reservation (READ_KEYS, READ_FULL_STATUS) 응답에서 본 transport ID가 사용된다.
 */
void spdk_scsi_port_set_iscsi_transport_id(struct spdk_scsi_port *port,
		char *iscsi_name, uint64_t isid);

/*
 * [한국어]
 * spdk_scsi_lun_id_int_to_fmt - 정수 LUN ID를 SAM-5 LUN 64-bit 포맷으로 변환한다.
 *
 * @param lun_id: 정수형 LUN ID (0 ~ 16383 정도).
 * @return: 64-bit LUN format 값 (Big-Endian으로 host에 전송될 수 있도록 인코딩).
 *
 * SAM-5 §4.6 LUN field 구조: 8B 중 첫 2B가 address method + bus identifier + single-level LUN.
 * REPORT_LUNS 응답이나 RTPG 응답 작성 시 사용.
 */
uint64_t spdk_scsi_lun_id_int_to_fmt(int lun_id);

/*
 * [한국어]
 * spdk_scsi_lun_id_fmt_to_int - SAM-5 LUN 포맷을 정수 LUN ID로 역변환한다.
 *
 * @param fmt_lun: 64-bit LUN format 값.
 * @return: 정수 LUN ID, 잘못된 format이면 -1.
 *
 * iSCSI BHS의 LUN 필드(8B)를 미들레이어 내부 정수 ID로 변환할 때 사용.
 */
int spdk_scsi_lun_id_fmt_to_int(uint64_t fmt_lun);

/*
 * [한국어]
 * spdk_scsi_sbc_opcode_string - SBC opcode + service action을 사람이 읽을 수 있는 문자열로 변환.
 *
 * @param opcode: SCSI operation code (CDB[0]).
 * @param sa: service action 코드 (12/16바이트 CDB에서 사용; 미사용 시 0).
 * @return: opcode를 설명하는 정적 문자열 (예: "READ(10)", "WRITE_SAME(16)").
 *
 * 디버그 로그/trace 출력에 사용. hot path는 아니며 실패 진단용.
 */
const char *spdk_scsi_sbc_opcode_string(uint8_t opcode, uint16_t sa);
#ifdef __cplusplus
}
#endif

#endif /* SPDK_SCSI_H */
