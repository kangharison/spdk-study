/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI Task 생명주기 관리 (task.c)
 *
 * === 파일의 역할 ===
 * SPDK iSCSI 타깃의 "task" 객체(struct spdk_iscsi_task) 생성·해제 두 함수를 담는다.
 * iSCSI 측 task는 SCSI Command PDU 1건(또는 그것의 Read 분할 sub-task)에 대응하며,
 * 내부적으로 spdk_scsi_task를 임베드하여 SPDK SCSI 레이어가 인지 가능한 형태로 다룬다.
 * iscsi_task_get()이 mempool에서 객체를 꺼내 conn에 연결하고, iscsi_task_free()는
 * SCSI 레이어의 ref-count가 0이 되어 호출되는 콜백으로 mempool에 반납한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (생성):
 *   conn polling → PDU 수신 → SCSI Command 처리 (iscsi.c) →
 *     iscsi_task_get(conn, parent=NULL, cpl_fn=iscsi_task_cpl) →
 *     spdk_scsi_dev_queue_task → bdev I/O 제출.
 * 호출 체인 (해제):
 *   bdev 완료 콜백 → SCSI 레이어가 응답 빌드 + ref-- → 0 도달 시 task->free 호출 →
 *     iscsi_task_free → mempool 반환.
 * Read 응답이 MaxRecvDataSegmentLength로 분할되는 경우:
 *   parent task 1개 + 다수 child sub-task가 같은 dxfer_dir/CDB/lun을 공유하며 생성/완료된다.
 * 실행 컨텍스트: 모든 task 연산은 conn이 바인딩된 단일 SPDK thread(poll_group)에서 실행되어
 *   lockless. data_in_cnt/pending_task_cnt 같은 카운터도 그 스레드에서만 갱신.
 *
 * === 타 모듈과의 연결 ===
 * - iscsi/conn.h: task->conn 백포인터, conn->pending_task_cnt/data_in_cnt 카운터 갱신.
 * - iscsi/task.h: spdk_iscsi_task 정의, iscsi_task_from_scsi_task() 변환 매크로.
 * - spdk/scsi.h: spdk_scsi_task_construct/put — SCSI 레이어 ref-count 관리.
 * - spdk/histogram_data.h: 타깃 단위 latency 히스토그램.
 * - g_iscsi.task_pool: mempool에서 task 객체 풀링 (DPDK rte_mempool 기반, lockless).
 * - iscsi/tgt_node.h: target->histogram에 latency tally.
 *
 * === 주요 함수/구조체 요약 ===
 * - iscsi_task_get(conn, parent, cpl_fn): 빈 task를 mempool에서 꺼내 초기화 + ref 증가 (parent).
 * - iscsi_task_free(scsi_task): SCSI 레이어가 호출하는 정리 콜백(latency 기록, 카운터 감소, mempool 반환).
 * - 핵심 카운터:
 *   conn->pending_task_cnt: 진행 중 task 수 (destruct 시 0 대기 조건).
 *   conn->data_in_cnt: Read 분할 sub-task 미완료 수 (전송 backpressure 판단).
 *   parent->scsi.ref: 부모 task의 SCSI ref-count — child가 ++/--로 부모 수명 연장.
 */

#include "spdk/env.h"
/* [한국어] SPDK 환경 — spdk_get_ticks(TSC 카운터)와 mempool API에 의존. */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG 등 로그 매크로 정의. */
#include "iscsi/conn.h"
/* [한국어] spdk_iscsi_conn 정의 — task->conn 멤버 접근에 필요. */
#include "iscsi/task.h"
/* [한국어] spdk_iscsi_task 정의 + iscsi_task_from_scsi_task / get_mobj / disassociate_pdu 헬퍼. */
#include "iscsi/tgt_node.h"
/* [한국어] spdk_iscsi_tgt_node 정의 — task->conn->target->histogram 접근에 필요. */

/*
 * [한국어]
 * iscsi_task_free - spdk_scsi_task의 free 콜백 (ref-count 0 도달 시 호출).
 *
 * @scsi_task: SCSI 레이어가 알고 있는 임베드된 spdk_scsi_task 포인터.
 *             컨테이너_of 패턴(iscsi_task_from_scsi_task)으로 spdk_iscsi_task 복원.
 *
 * 동기/배경: spdk_scsi_task_construct() 호출 시 free 콜백으로 본 함수를 등록한다.
 * SCSI 레이어가 모든 ref를 풀어 0이 되면 본 함수를 호출하여 자원을 반환한다.
 *
 * 절차:
 *  1) start_tsc로부터의 경과 시간을 target histogram에 tally (latency 측정).
 *  2) parent가 있으면 (Read sub-task 등):
 *     - 자신이 Read 방향이면 conn->data_in_cnt 감소.
 *     - parent의 SCSI ref 감소(spdk_scsi_task_put) — parent 자체 free 가능 시점에 도달.
 *  3) Memory object(DataOut payload 버퍼)가 부착되어 있으면 datapool에 반환.
 *  4) 연관된 PDU와 disassociate (PDU 생명주기 분리).
 *  5) conn->pending_task_cnt 감소.
 *  6) g_iscsi.task_pool(mempool)에 객체 반환.
 *
 * 실행 컨텍스트: conn->thread (단일 SPDK thread). lockless 조건.
 * 호출 체인:
 *   bdev 완료 → ... → spdk_scsi_task_put → ref==0 시 free_fn(=iscsi_task_free).
 */
static void
iscsi_task_free(struct spdk_scsi_task *scsi_task)
{
	uint64_t tsc_diff;
	/* [한국어] 시작 TSC 대비 현재까지의 경과 사이클 — latency 측정용. */
	struct spdk_iscsi_task *task = iscsi_task_from_scsi_task(scsi_task);
	/* [한국어] container_of 매크로 — 임베드된 scsi_task로부터 부모 iscsi_task 복원. */

	if (task->conn->target->histogram) {
		/* [한국어] target에 histogram이 활성화되어 있으면 latency 누적. */
		tsc_diff = spdk_get_ticks() - task->start_tsc;
		/* [한국어] 현재 TSC - 시작 TSC = 처리 시간 (사이클). */
		spdk_histogram_data_tally(task->conn->target->histogram, tsc_diff);
		/* [한국어] 히스토그램 버킷에 1 가산 (lock-free, per-CPU 가정). */
	}

	if (task->parent) {
		/* [한국어] 이 task는 다른 task의 child(분할 sub-task)일 때만 parent != NULL. */
		if (task->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV) {
			/* [한국어] FROM_DEV = device → host 즉 Read 방향.
			 * 분할된 Read sub-task의 완료이므로 conn의 진행 중 DataIn 카운터 감소. */
			assert(task->conn->data_in_cnt > 0);
			/* [한국어] 카운터 정합성 검사 — 음수가 되면 버그. */
			task->conn->data_in_cnt--;
		}

		spdk_scsi_task_put(&task->parent->scsi);
		/* [한국어] parent의 SCSI ref-- — 모든 child가 끝나면 parent도 free 진입. */
		task->parent = NULL;
		/* [한국어] 백포인터 끊기 — 이중 put 방지. */
	}

	if (iscsi_task_get_mobj(task)) {
		/* [한국어] DataOut 같은 incoming payload용으로 datapool에서 빌린 mobj가 있으면 반환. */
		iscsi_datapool_put(iscsi_task_get_mobj(task));
	}

	iscsi_task_disassociate_pdu(task);
	/* [한국어] 연관된 PDU와의 양방향 링크 끊기 — PDU도 별도 수명을 갖는다. */
	assert(task->conn->pending_task_cnt > 0);
	/* [한국어] conn에 attach된 task 카운터 정합성 검사. */
	task->conn->pending_task_cnt--;
	/* [한국어] 진행 중 task 수 감소 — 0이 되면 conn destruct 가능. */
	spdk_mempool_put(g_iscsi.task_pool, (void *)task);
	/* [한국어] DPDK mempool에 객체 반환 (lockless ring). 이후 다음 task_get에서 재사용. */
}

/*
 * [한국어]
 * iscsi_task_get - 새 iSCSI task 객체 할당 + 초기화.
 *
 * @conn: 이 task가 속한 연결 (필수).
 * @parent: NULL이면 최상위 task, 비-NULL이면 분할 sub-task (parent 속성 상속).
 * @cpl_fn: SCSI 레이어가 task 완료 시 호출할 콜백 (예: iscsi_task_cpl, iscsi_task_mgmt_cpl).
 * @return: 초기화된 task 포인터. mempool 고갈 시 abort() (회복 불가능 가정).
 *
 * 동기/배경: SCSI Command PDU 처리 또는 그 분할 처리 진입점. 객체는 g_iscsi.task_pool
 * (DPDK rte_mempool, lockless)에서 꺼낸다 — malloc 비용 회피.
 *
 * 절차:
 *  1) mempool에서 객체 한 개 획득.
 *  2) 객체 zero-init + start_tsc 기록.
 *  3) conn 연결, conn->pending_task_cnt++.
 *  4) spdk_scsi_task_construct로 SCSI 레이어에 등록 (cpl/free 콜백 지정).
 *  5) parent가 있으면:
 *     - parent SCSI ref++ (자기 자신이 살아있는 동안 parent도 살아있어야 함).
 *     - tag/lun_id/dxfer_dir/transfer_len/lun/cdb/initiator_port/target_port 상속.
 *     - Read 방향이면 conn->data_in_cnt++.
 *
 * 실행 컨텍스트: conn->thread (단일 SPDK thread).
 * 호출 체인:
 *   PDU 수신 처리 (iscsi.c::iscsi_op_scsi 등) → iscsi_task_get →
 *   (분할 처리 시) iscsi_task_get(parent=...) → 모두 끝나면 iscsi_task_free 체인.
 */
struct spdk_iscsi_task *
iscsi_task_get(struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *parent,
	       spdk_scsi_task_cpl cpl_fn)
{
	struct spdk_iscsi_task *task;
	/* [한국어] mempool에서 가져올 객체 임시 포인터. */

	task = spdk_mempool_get(g_iscsi.task_pool);
	/* [한국어] DPDK rte_mempool에서 lockless로 객체 획득.
	 * 풀은 spdk_iscsi_init 단계에서 (DEFAULT_TASK_POOL_SIZE 등) 미리 채워져 있음. */
	if (!task) {
		/* [한국어] 풀 고갈 — 정상적인 사이징에서는 발생하지 않는다고 가정.
		 * SPDK iSCSI는 회복 시도 대신 abort()로 즉시 종료 (디버그 용이성 우선). */
		SPDK_ERRLOG("Unable to get task\n");
		abort();
	}

	assert(conn != NULL);
	/* [한국어] conn 미지정은 호출 오용. */
	memset(task, 0, sizeof(*task));
	/* [한국어] mempool에서 재사용된 메모리는 이전 잔여 데이터가 남아 있을 수 있어 0-초기화 필수. */
	task->start_tsc = spdk_get_ticks();
	/* [한국어] latency 측정 기준 TSC — free 시 차이를 histogram에 기록. */
	task->conn = conn;
	/* [한국어] 백포인터 — 모든 후속 처리에서 conn 컨텍스트 접근. */
	assert(conn->pending_task_cnt < UINT32_MAX);
	/* [한국어] 카운터 오버플로우 방어 (실질적으로 도달 불가). */
	conn->pending_task_cnt++;
	/* [한국어] 진행 중 task 수 증가 — destruct 차단 조건. */
	spdk_scsi_task_construct(&task->scsi,
				 cpl_fn,
				 iscsi_task_free);
	/* [한국어] SCSI 레이어 task 초기화 — ref=1로 시작.
	 * cpl_fn: 모든 자식이 끝나고 응답 준비된 시점 콜백 (iSCSI 측 응답 PDU 작성).
	 * free_fn: ref==0 도달 시 콜백 (위 iscsi_task_free). */
	if (parent) {
		/* [한국어] 분할 sub-task 또는 mgmt-나누기 등 parent가 있는 경우. */
		parent->scsi.ref++;
		/* [한국어] 부모 ref++ — 자식이 살아있는 동안 부모 메모리 보존.
		 * ref 감소는 자식의 iscsi_task_free에서 spdk_scsi_task_put으로 수행. */
		task->parent = parent;
		/* [한국어] 부모 백포인터. */
		task->tag = parent->tag;
		/* [한국어] iSCSI ITT (Initiator Task Tag) 상속 — 같은 명령의 sub-task. */
		task->lun_id = parent->lun_id;
		/* [한국어] 동일 LUN. */
		task->scsi.dxfer_dir = parent->scsi.dxfer_dir;
		/* [한국어] 데이터 방향 상속 (FROM_DEV=Read, TO_DEV=Write, NONE). */
		task->scsi.transfer_len = parent->scsi.transfer_len;
		/* [한국어] 전체 전송 길이 상속 — sub-task가 부분만 처리해도 전체값을 보유. */
		task->scsi.lun = parent->scsi.lun;
		/* [한국어] SCSI LUN 객체 포인터 상속. */
		task->scsi.cdb = parent->scsi.cdb;
		/* [한국어] CDB(Command Descriptor Block, SCSI 명령 바이트) 포인터 상속. */
		task->scsi.target_port = parent->scsi.target_port;
		/* [한국어] target SCSI port. */
		task->scsi.initiator_port = parent->scsi.initiator_port;
		/* [한국어] initiator SCSI port. */
		if (task->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV) {
			/* [한국어] Read sub-task — DataIn 카운터 증가. backpressure 판단에 사용. */
			conn->data_in_cnt++;
		}
	}

	return task;
	/* [한국어] 호출자는 이후 spdk_scsi_dev_queue_task 등으로 SCSI 레이어에 제출. */
}
