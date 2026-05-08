/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI Task(SCSI command) 헤더 (task.h)
 *
 * === 파일의 역할 ===
 * iSCSI 레벨에서 진행 중인 SCSI 명령을 표현하는 `spdk_iscsi_task` 구조체와 그 인라인
 * 헬퍼들을 선언한다. iSCSI Read/Write/Inquiry 등 모든 SCSI 명령이 이 객체 한 개로 표현되며
 * SPDK의 SCSI 추상화(`spdk_scsi_task`)를 멤버로 포함하여 SCSI 레이어와 iSCSI 레이어를
 * 잇는 어댑터 역할을 한다. R2T (Ready To Transfer, RFC 3720 §10.8) 시퀀싱, Data-Out
 * 분할, DataIn 데이터시퀀스 번호(DataSN) 등 iSCSI 특유의 상태 변수를 모두 보관한다.
 * Task의 수명: 로그인 후 conn에서 SCSI Command PDU 수신 → iscsi_task_get으로 할당 →
 * iscsi_queue_task로 SCSI 레이어에 제출 → 완료 콜백(iscsi_task_cpl) → iscsi_task_put.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   conn에서 SCSI Command PDU 수신 → iscsi.c::iscsi_op_scsi() → iscsi_task_get() →
 *     iscsi_task_associate_pdu() (PDU 참조 카운트 ++)
 *     → iscsi_queue_task() → spdk_scsi_dev_queue_task() → bdev I/O 시작
 *   완료 시: SCSI 레이어 콜백 → iscsi_task_cpl() → 응답 PDU 작성 → iscsi_task_put()
 *     (참조카운트 0이면 iscsi_disassociate_pdu + free).
 * Write에는 R2T 흐름 추가: 호스트에서 Data-Out PDU 분할 도착 → next_expected_r2t_offset
 *   확인 → 데이터 누적 → 모두 받으면 SCSI 제출.
 * 실행 컨텍스트: conn이 바인딩된 SPDK thread (poll group). task는 그 thread에서만 다뤄짐.
 *
 * === 타 모듈과의 연결 ===
 * - iscsi.h: spdk_iscsi_pdu, spdk_iscsi_conn 등 forward 의존.
 * - spdk/scsi.h: spdk_scsi_task 멤버를 통해 SCSI 레이어와 데이터 공유.
 * - conn.h: conn::queued_r2t_tasks/active_r2t_tasks/queued_datain_tasks 큐에 task 매달림.
 * - iscsi_spec.h: iscsi_bhs_scsi_req의 immediate/read_bit 비트 매크로.
 * - util.h: SPDK_CONTAINEROF 매크로 (scsi_task ↔ iscsi_task 변환).
 * 데이터 흐름: PDU header/data → task의 카운터·offset → R2T 발행 또는 SCSI 제출 →
 *   완료 시 응답 PDU 생성 → conn의 write_pdu_list로 송신.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_iscsi_task: SCSI command 1개의 모든 iSCSI 상태 (R2T, DataSN, sub-task 등).
 * - iscsi_task_get(): conn별 task pool에서 할당 (parent로 sub-task도 표현).
 * - iscsi_task_put(): scsi_task ref 카운트 감소.
 * - iscsi_task_associate/disassociate_pdu(): PDU와 task의 양방향 참조 관리.
 * - iscsi_task_is_immediate/is_read(): BHS의 비트 검사 헬퍼.
 * - iscsi_task_from_scsi_task(): 콜백에서 SCSI task → iSCSI task 역변환.
 * - iscsi_task_get_primary(): sub-task → 부모 task 정규화.
 * - iscsi_task_set/get_mobj(): mempool에서 가져온 데이터 버퍼 관리 (R2T용).
 */

#ifndef SPDK_ISCSI_TASK_H
#define SPDK_ISCSI_TASK_H
/* [한국어] 이중 include 방지. */

#include "iscsi/iscsi.h"
/* [한국어] spdk_iscsi_pdu, conn forward 의존. */
#include "spdk/scsi.h"
/* [한국어] spdk_scsi_task, spdk_scsi_task_cpl, spdk_scsi_task_put 등 SCSI 레이어 API. */
#include "spdk/util.h"
/* [한국어] SPDK_CONTAINEROF 매크로 (member→struct 역변환). */

struct spdk_iscsi_task {
	/* [한국어] iSCSI command 1개의 진행 상태 전체. SCSI 레이어와는 .scsi 멤버로 연결. */

	struct spdk_scsi_task	scsi;
	/* [한국어] SCSI 추상 task — 본 구조체의 첫 멤버이므로 SPDK_CONTAINEROF로 역변환 가능.
	 * 설정자: spdk_scsi_task_construct로 초기화 (iscsi_task_get 안에서).
	 * 읽는 자: bdev/SCSI 레이어가 LBA/length/CDB 등을 직접 사용.
	 * 동기화: 단일 SPDK thread에서만 다뤄짐. */

	struct spdk_iscsi_task *parent;
	/* [한국어] sub-task인 경우 부모 task 포인터 (큰 read를 여러 sub-task로 분할 시).
	 * 설정자: iscsi_task_get(parent != NULL) 호출 시 설정.
	 * 읽는 자: iscsi_task_get_primary가 부모 → 자기 식별에 사용.
	 * 값 범위: NULL=primary, 그 외=sub-task의 부모.
	 * 동기화: 부모 task의 subtask_list와 동일 thread에서만 변경. */

	struct spdk_iscsi_conn *conn;
	/* [한국어] 이 task가 속한 iSCSI connection 백포인터.
	 * 설정자: iscsi_task_get에서 conn 인자 그대로 저장.
	 * 읽는 자: 응답 PDU 작성, 완료 콜백.
	 * 값 범위: 유효한 spdk_iscsi_conn 포인터.
	 * 동기화: conn의 thread에 affine. */

	struct spdk_iscsi_pdu *pdu;
	/* [한국어] 이 task의 SCSI Command PDU 포인터 (BHS와 immediate data 보관).
	 * 설정자: iscsi_task_associate_pdu에서 설정 + pdu->ref++.
	 * 읽는 자: iscsi_task_get_bhs로 BHS 접근, R2T·응답 작성 시 BHS 필드 사용.
	 * 값 범위: 활성 task 동안 NULL 아님. disassociate 후 NULL.
	 * 동기화: conn thread. */

	struct spdk_mobj *mobj;
	/* [한국어] R2T로 받을 Data-Out 누적 버퍼 (mempool에서 할당, MAX_RECV_DATA_SEGMENT).
	 * 설정자: iscsi_task_set_mobj (R2T 발행 시).
	 * 읽는 자: Data-Out PDU 도착 시 누적 위치 계산.
	 * 값 범위: NULL 또는 유효 mobj.
	 * 동기화: conn thread. */

	uint64_t start_tsc;
	/* [한국어] 명령 시작 TSC (CPU 사이클) — histogram 측정용 (RTT/latency).
	 * 설정자: iscsi_task_get에서 spdk_get_ticks().
	 * 읽는 자: 완료 시 대기시간 계산. */

	uint32_t outstanding_r2t;
	/* [한국어] 현재 발행된 미완 R2T 수 (Write 요청에서).
	 * 설정자: R2T 발행 시 ++, Data-Out 마지막 수신 시 --.
	 * 값 범위: 0 ≤ x ≤ MaxOutstandingR2T (협상값). */

	uint32_t desired_data_transfer_length;
	/* [한국어] 한 R2T가 요청한 전송 길이 바이트 수 (RFC 3720 §10.8 DesiredDataTransferLength).
	 * 설정자: R2T 작성 시.
	 * 읽는 자: Data-Out 누적 검증, 다음 offset 계산. */

	/* Only valid for Read/Write */
	uint32_t bytes_completed;
	/* [한국어] 지금까지 처리된 데이터 누적 바이트 (Read DataIn 송신/Write Data-Out 수신).
	 * 설정자: 각 sub-task 완료 시 += sub-task length.
	 * 읽는 자: 전체 완료 판정. */

	uint32_t data_out_cnt;
	/* [한국어] 이 task에 도착한 Data-Out PDU 카운트 (디버깅·통계). */

	/*
	 * Tracks the current offset of large read or write io.
	 */
	uint32_t current_data_offset;
	/* [한국어] 현재까지 송수신된 데이터 오프셋 (Read DataIn 분할 송신, Write 누적 수신 모두). */

	/*
	 * next_expected_r2t_offset is used when we receive
	 * the DataOUT PDU.
	 */
	uint32_t next_expected_r2t_offset;
	/* [한국어] 다음에 받을 Data-Out PDU의 buffer offset 기대값.
	 * Data-Out이 순서대로 도착하지 않으면 RFC 3720 위반으로 판정·재요청. */

	/*
	 * Tracks the length of the R2T that is in progress.
	 * Used to check that an R2T burst does not exceed
	 *  MaxBurstLength.
	 */
	uint32_t current_r2t_length;
	/* [한국어] 진행 중인 R2T burst의 누적 데이터 길이.
	 * MaxBurstLength 초과 검증에 사용. */

	/*
	 * next_r2t_offset is used when we are sending the
	 * R2T packet to keep track of next offset of r2t.
	 */
	uint32_t next_r2t_offset;
	/* [한국어] 다음에 발행할 R2T의 BufferOffset 값. Write가 분할 R2T를 사용할 때 증가. */

	uint32_t R2TSN;
	/* [한국어] R2T Sequence Number (RFC 3720 §10.8.1) — task 내에서 단조 증가. */

	uint32_t r2t_datasn; /* record next datasn for a r2tsn */
	/* [한국어] 현재 R2T 시퀀스에 대해 기대하는 다음 Data-Out DataSN. */

	uint32_t acked_r2tsn; /* next r2tsn to be acked */
	/* [한국어] 다음에 ACK 받기를 기다리는 R2TSN — SNACK/재전송 처리. */

	uint32_t datain_datasn;
	/* [한국어] Read 응답으로 송신한 DataIn의 DataSN 카운터. */

	uint32_t acked_data_sn; /* next expected datain datasn */
	/* [한국어] 호스트가 다음에 ACK해주길 기대하는 datain DataSN. */

	uint32_t ttt;
	/* [한국어] Target Transfer Tag (RFC 3720 §10.8) — R2T/Data-Out 매핑 식별자. */

	bool is_r2t_active;
	/* [한국어] 현재 R2T 시퀀스가 진행 중인지. 송신 중복 방지 플래그. */

	uint32_t tag;
	/* [한국어] iSCSI Initiator Task Tag (ITT, RFC 3720 §3.1) — initiator가 부여한 ID.
	 * 응답 PDU에서 그대로 echo되어 호스트가 자신의 명령을 매칭. */

	/**
	 * Record the lun id just in case the lun is invalid,
	 * which will happen when hot removing the lun.
	 */
	int lun_id;
	/* [한국어] LUN id (정수, 0..N-1). LUN이 hot-remove되어도 응답 작성을 위해 별도 저장.
	 * 설정자: iscsi_task_get/queue_task 중 SCSI BHS의 LUN 필드 파싱 결과.
	 * 읽는 자: completion 콜백에서 응답 PDU 작성. */

	struct spdk_poller *mgmt_poller;
	/* [한국어] Task Management Function의 retry/timeout 처리용 poller (예: ABORT TASK SET).
	 * 미사용 시 NULL. */

	TAILQ_ENTRY(spdk_iscsi_task) link;
	/* [한국어] conn의 task 큐(queued_r2t_tasks/active_r2t_tasks/queued_datain_tasks 등)
	 * 중 하나에 매달리는 링크. 큐 종류는 task 진행 단계에 따라 다름. */

	TAILQ_HEAD(subtask_list, spdk_iscsi_task) subtask_list;
	/* [한국어] 이 (primary) task에서 분할된 sub-task들의 헤드. parent 사용 시 채워짐. */
	TAILQ_ENTRY(spdk_iscsi_task) subtask_link;
	/* [한국어] 자신이 sub-task인 경우 parent의 subtask_list에 매달리는 링크. */
	bool is_queued; /* is queued in scsi layer for handling */
	/* [한국어] SCSI 레이어 처리 큐에 진입한 상태인지 표시 (재제출 방지). */
};

/*
 * [한국어]
 * iscsi_task_put - task 참조 감소 (실제 free는 SCSI 레이어가 ref 0일 때 수행).
 *
 * @task: 대상 task.
 *
 * spdk_scsi_task_put이 ref-- 하고 ref==0이면 free 콜백을 호출 (iscsi 레이어가 등록한
 * cleanup이 거기서 disassociate_pdu 등을 수행).
 */
static inline void
iscsi_task_put(struct spdk_iscsi_task *task)
{
	spdk_scsi_task_put(&task->scsi);
	/* [한국어] SCSI 레이어 ref 감소. ref==0일 때 free callback이 호출됨. */
}

/*
 * [한국어]
 * iscsi_task_get_pdu - 연관된 SCSI Command PDU 반환.
 */
static inline struct spdk_iscsi_pdu *
iscsi_task_get_pdu(struct spdk_iscsi_task *task)
{
	return task->pdu;
	/* [한국어] 단순 필드 접근 — 인라인 함수로 명시적 API 제공. */
}

/*
 * [한국어]
 * iscsi_task_set_pdu - 연관 PDU 포인터 설정 (참조 카운트 변경 없음).
 */
static inline void
iscsi_task_set_pdu(struct spdk_iscsi_task *task, struct spdk_iscsi_pdu *pdu)
{
	task->pdu = pdu;
	/* [한국어] 포인터 대입만. ref++/-- 는 associate/disassociate가 수행. */
}

/*
 * [한국어]
 * iscsi_task_get_bhs - 연관 PDU의 Basic Header Segment 포인터 반환.
 *
 * BHS는 RFC 3720 §10.1 정의 48바이트 고정 헤더. opcode·tag·LBA 등을 모두 포함.
 */
static inline struct iscsi_bhs *
iscsi_task_get_bhs(struct spdk_iscsi_task *task)
{
	return &iscsi_task_get_pdu(task)->bhs;
	/* [한국어] PDU 가져와 그 안의 bhs 멤버 주소 반환. */
}

/*
 * [한국어]
 * iscsi_task_associate_pdu - task와 PDU의 양방향 참조 설정 (PDU ref++).
 *
 * task->pdu = pdu, pdu->ref++. 같은 PDU가 여러 sub-task에 연관될 수 있어 ref 카운트 사용.
 */
static inline void
iscsi_task_associate_pdu(struct spdk_iscsi_task *task, struct spdk_iscsi_pdu *pdu)
{
	iscsi_task_set_pdu(task, pdu);
	/* [한국어] task→pdu 포인터. */
	pdu->ref++;
	/* [한국어] PDU 참조 카운트 증가. iscsi_put_pdu가 0일 때만 실제 해제. */
}

/*
 * [한국어]
 * iscsi_task_disassociate_pdu - task와 PDU 연관 해제 (PDU ref--).
 *
 * 연관된 PDU가 있을 때만 iscsi_put_pdu (내부에서 ref--)을 호출하여 누수 방지.
 */
static inline void
iscsi_task_disassociate_pdu(struct spdk_iscsi_task *task)
{
	if (iscsi_task_get_pdu(task)) {
		/* [한국어] 연관 PDU가 있을 때만 처리. */
		iscsi_put_pdu(iscsi_task_get_pdu(task));
		/* [한국어] PDU ref 감소 (필요 시 mempool 반환). */
		iscsi_task_set_pdu(task, NULL);
		/* [한국어] task의 PDU 포인터 무효화. */
	}
}

/*
 * [한국어]
 * iscsi_task_is_immediate - 이 task의 SCSI Request가 immediate(헤더 immediate bit) 인지.
 *
 * RFC 3720 §10.3.1: immediate=1이면 ExpCmdSN ordering 우회. fast-path 결정에 사용.
 * @return: 1=immediate, 0=일반.
 */
static inline int
iscsi_task_is_immediate(struct spdk_iscsi_task *task)
{
	struct iscsi_bhs_scsi_req *scsi_req;
	/* [한국어] BHS를 SCSI Command 형식 view로 캐스팅. */

	scsi_req = (struct iscsi_bhs_scsi_req *)iscsi_task_get_bhs(task);
	/* [한국어] BHS 첫 바이트의 immediate bit 위치는 SCSI Command 구조에서 정의. */
	return (scsi_req->immediate == 1);
	/* [한국어] bitfield 값 비교 (1 또는 0). */
}

/*
 * [한국어]
 * iscsi_task_is_read - 이 task가 Read 명령(Read bit set)인지.
 *
 * RFC 3720 §10.3.1: read_bit=1이면 target → initiator 데이터 흐름. R2T 불필요.
 */
static inline int
iscsi_task_is_read(struct spdk_iscsi_task *task)
{
	struct iscsi_bhs_scsi_req *scsi_req;
	/* [한국어] BHS view. */

	scsi_req = (struct iscsi_bhs_scsi_req *)iscsi_task_get_bhs(task);
	return (scsi_req->read_bit == 1);
	/* [한국어] read_bit 검사. */
}

struct spdk_iscsi_task *iscsi_task_get(struct spdk_iscsi_conn *conn,
				       struct spdk_iscsi_task *parent,
				       spdk_scsi_task_cpl cpl_fn);
/* [한국어] task pool에서 task 1개 할당.
 * @parent: NULL=primary task, 非NULL=sub-task로 만들고 parent의 subtask_list에 매닮.
 * @cpl_fn: SCSI 레이어가 완료 시 호출할 콜백 (예: iscsi_task_cpl). */

/*
 * [한국어]
 * iscsi_task_from_scsi_task - SCSI task → iSCSI task 역변환.
 *
 * SCSI 레이어 완료 콜백은 spdk_scsi_task* 만 받으므로 본 헬퍼로 외부 spdk_iscsi_task로
 * 복원한다 (SPDK_CONTAINEROF는 member → struct 변환).
 */
static inline struct spdk_iscsi_task *
iscsi_task_from_scsi_task(struct spdk_scsi_task *task)
{
	return SPDK_CONTAINEROF(task, struct spdk_iscsi_task, scsi);
	/* [한국어] member 주소에서 외부 struct 시작 주소 계산 (linux container_of 동등). */
}

/*
 * [한국어]
 * iscsi_task_get_primary - sub-task인 경우 부모, 아니면 자기 자신을 반환.
 *
 * Read/Write 분할 시 sub-task 완료 콜백에서 primary task 단위로 상태를 갱신하기 위해 사용.
 */
static inline struct spdk_iscsi_task *
iscsi_task_get_primary(struct spdk_iscsi_task *task)
{
	if (task->parent) {
		/* [한국어] sub-task인 경우. */
		return task->parent;
	} else {
		/* [한국어] primary task 자기 자신. */
		return task;
	}
}

/*
 * [한국어]
 * iscsi_task_set_mobj - R2T 누적용 mempool 버퍼(mobj) 연결.
 */
static inline void
iscsi_task_set_mobj(struct spdk_iscsi_task *task, struct spdk_mobj *mobj)
{
	task->mobj = mobj;
	/* [한국어] 단순 대입. mobj 자체의 mp/buf/data_len은 mempool이 관리. */
}

/*
 * [한국어]
 * iscsi_task_get_mobj - 연결된 mobj 반환.
 */
static inline struct spdk_mobj *
iscsi_task_get_mobj(struct spdk_iscsi_task *task)
{
	return task->mobj;
	/* [한국어] 필드 접근. */
}

#endif /* SPDK_ISCSI_TASK_H */
/* [한국어] include guard 종료. */
