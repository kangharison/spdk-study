/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK NVMe-oF RDMA Target Transport (rdma.c)
 *
 * === 파일의 역할 ===
 * SPDK NVMe-oF (NVMe over Fabrics) target 의 **RDMA transport 구현**.
 * NVMe-oF host 가 RDMA (RoCEv2 / iWARP / InfiniBand) 를 통해 보내는 NVMe
 * capsule (command + 데이터) 을 받아, 내부 bdev 레이어로 dispatch 하고
 * 응답을 반대 경로로 돌려보낸다. host 측 lib/nvme/nvme_rdma.c 와 짝.
 *
 * 핵심 추상화:
 *   - QP (Queue Pair): 1 admin QP + N IO QP per connection (NVMe spec 그대로).
 *   - WR (Work Request): RDMA 트랜잭션 단위. NVMe capsule = SEND WR, 데이터
 *     이동 = RDMA READ/WRITE WR.
 *   - SGL: scatter-gather list — Memory Region 등록된 호스트 메모리 영역.
 *   - CQ (Completion Queue): WR 완료 통지 (ibv_poll_cq).
 *   - Poll Group: 여러 QP 를 묶어 단일 reactor 스레드가 CQ 폴링.
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe-oF target stack (in-process):
 *   [Host] --(RDMA capsule)--> [RDMA transport (이 파일)]
 *      → spdk_nvmf_request_exec
 *      → nvmf_ctrlr_process_admin/io_cmd
 *      → bdev layer (lib/bdev)
 *      → bdev module (NVMe / Malloc / AIO / ...)
 *      → 백엔드 디바이스
 *
 * Transport vtable 등록: spdk_nvmf_transport_rdma (파일 끝의 ops 구조체) 가
 * SPDK_NVMF_TRANSPORT_REGISTER 매크로로 등록되어 lib/nvmf/transport.c 의
 * vtable 에 들어간다. TCP/FC/VFIOUSER 도 같은 패턴.
 *
 * 실행 컨텍스트: SPDK reactor 스레드 (각 poll group 이 하나의 스레드에 affinity).
 * 모든 RDMA 작업이 polled-mode — 인터럽트 없이 ibv_poll_cq 반복.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk_internal/rdma_provider.h (libibverbs 추상화), rdma_utils.h
 *   (Memory Region 풀), nvmf_internal.h (target 공통 구조체), transport.h
 *   (vtable 인터페이스).
 * - 의존하는 모듈: lib/nvmf/transport.c (transport registration), 외부
 *   사용자는 nvmf_create_transport(type="RDMA") 로 활성화.
 * - 데이터 흐름: ibv_post_recv 미리 깔린 RECV WR 풀에 capsule 도착 →
 *   request state machine 진행 → bdev exec → SEND WR 로 응답.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_nvmf_rdma_request : 1개 NVMe-oF request 의 상태/리소스 컨테이너.
 * - struct spdk_nvmf_rdma_qpair   : 1개 RDMA QP + 연결된 request pool.
 * - struct spdk_nvmf_rdma_poller  : 1 QP set 을 폴링하는 단위 (poll group 안의 한 device).
 * - struct spdk_nvmf_rdma_port    : listen address (CM connect 수신).
 * - struct spdk_nvmf_rdma_transport : 전체 RDMA transport 인스턴스.
 * - enum spdk_nvmf_rdma_request_state : 16-state machine (FREE → NEW → ...→ COMPLETED).
 * - nvmf_rdma_request_process()     : 상태 머신 단일 step 진행. 핵심 dispatcher.
 * - nvmf_rdma_poll_group_poll()     : poll group 단위 CQ poll + 이벤트 처리.
 * - nvmf_rdma_destroy_drained_qpair: QP 종료 cleanup.
 *
 * === RDMA 8 종 state machine 요약 ===
 *  FREE → NEW (capsule 도착)
 *  → NEED_BUFFER → HAVE_BUFFER (bdev iobuf 할당)
 *  → DATA_TRANSFER_TO_CONTROLLER_PENDING → TRANSFERRING_HOST_TO_CONTROLLER
 *    (write 의 경우 RDMA READ 발행)
 *  → READY_TO_EXECUTE → EXECUTING → EXECUTED (bdev I/O)
 *  → DATA_TRANSFER_TO_HOST_PENDING → TRANSFERRING_CONTROLLER_TO_HOST
 *    (read 의 경우 RDMA WRITE 발행)
 *  → READY_TO_COMPLETE_PENDING → READY_TO_COMPLETE → COMPLETING → COMPLETED
 *    (SEND WR 로 응답 capsule 전송)
 *  → FREE (재사용)
 *
 * 각 PENDING 상태는 RDMA queue depth (max_send_wr) 가 부족할 때 대기 위치 —
 * 깊이 사용량 추적 (current_send_depth, current_read_depth) 이 핵심.
 */

#include "spdk/stdinc.h"                            /* [한국어] 표준 라이브러리 헤더 묶음 - stdint/stdlib/string 등 */

#include "spdk/config.h"                            /* [한국어] ./configure 생성 매크로 - SPDK_CONFIG_RDMA 등 */
#include "spdk/thread.h"                            /* [한국어] spdk_thread/poller/io_channel - reactor 모델 */
#include "spdk/likely.h"                            /* [한국어] spdk_likely/unlikely - hot path 분기 힌트 (CQ poll loop) */
#include "spdk/nvmf_transport.h"                    /* [한국어] spdk_nvmf_transport_ops - 본 파일이 채워 등록할 vtable */
#include "spdk/string.h"                            /* [한국어] spdk_strerror, spdk_sprintf_alloc 등 문자열 헬퍼 */
#include "spdk/trace.h"                            /* [한국어] spdk_trace_record - RDMA WR 단계별 latency 추적 */
#include "spdk/tree.h"                              /* [한국어] RB_INIT/RB_INSERT - qpair tree 등 RB-tree 자료구조 */
#include "spdk/util.h"                              /* [한국어] SPDK_COUNTOF 등 잡유틸 */

#include "spdk_internal/assert.h"                   /* [한국어] SPDK 내부 assert 매크로 (release 빌드에서도 동작 가능) */
#include "spdk/log.h"                               /* [한국어] SPDK_ERRLOG/DEBUGLOG */
#include "spdk_internal/rdma_provider.h"            /* [한국어] libibverbs를 SPDK가 추상화한 인터페이스 - ibv_qp/cq/pd/mr 등 */
#include "spdk_internal/rdma_utils.h"               /* [한국어] Memory Region 풀, RDMA 주소 유틸 등 헬퍼 */

#include "nvmf_internal.h"                          /* [한국어] spdk_nvmf_tgt/subsystem/request/qpair 등 NVMe-oF 코어 내부 타입 */
#include "transport.h"                              /* [한국어] nvmf_transport_* dispatch thunk 선언 */

#include "spdk_internal/trace_defs.h"               /* [한국어] RDMA 전용 trace point ID 매크로 */

struct spdk_nvme_rdma_hooks g_nvmf_hooks = {};
/* [한국어] RDMA 사용자 정의 hooks — 외부 라이브러리가 MR 등록·인증 등을 가로채는 콜백 슬롯.
 * 기본값 = 모두 NULL (gateway 없이 SPDK 가 직접 ibv_reg_mr). nvmf_rdma_init_hooks 로 설정. */

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma;
/* [한국어] 파일 끝의 vtable 정의에 대한 forward — SPDK_NVMF_TRANSPORT_REGISTER 매크로가
 * 이 심볼을 ELF .init_array constructor 에서 transport list 에 등록. */

/*
 RDMA Connection Resource Defaults
 */
#define NVMF_DEFAULT_MSDBD		16
/* [한국어] Maximum SGL Descriptors Block Descriptors — host 가 한 capsule 에 보낼 수 있는
 * SGL descriptor 의 최대 수. 16 = NVMe-oF spec 의 기본값. 더 큰 IO 가 필요하면 multiple
 * RDMA READ/WRITE 로 분할. */

#define NVMF_DEFAULT_TX_SGE		SPDK_NVMF_MAX_SGL_ENTRIES
/* [한국어] Transmit side (controller → host 응답) 의 ibv_sge 배열 크기. */

#define NVMF_DEFAULT_RSP_SGE		1
/* [한국어] response capsule (16B NVMe cqe + metadata) 의 SGE 수 — 단일 contiguous buffer. */

#define NVMF_DEFAULT_RX_SGE		2
/* [한국어] receive (host → controller) 의 SGE 수 = 2 (capsule header + immediate data). */

#define NVMF_RDMA_MAX_EVENTS_PER_POLL	32
/* [한국어] 한 번의 rdma_get_cm_event 호출에서 처리할 RDMA CM 이벤트 최대 개수.
 * 이벤트 폭주 시 다른 작업이 starve 되는 것을 막기 위한 batching 한도. */

SPDK_STATIC_ASSERT(NVMF_DEFAULT_MSDBD <= SPDK_NVMF_MAX_SGL_ENTRIES,
		   "MSDBD must not exceed SPDK_NVMF_MAX_SGL_ENTRIES");

/* The RDMA completion queue size */
#define DEFAULT_NVMF_RDMA_CQ_SIZE	4096
/* [한국어] CQ 한 개의 깊이 — 4096 entries. 여러 QP 가 한 CQ 를 공유하므로 모든 QP 의
 * in-flight WR 총합을 수용해야 함. */

#define MAX_WR_PER_QP(queue_depth)	(queue_depth * 3 + 2)
/* [한국어] QP 1개당 최대 WR 수 계산식. queue_depth 1개 NVMe command 당:
 *  - 1 RECV WR (capsule 수신)
 *  - 1 SEND WR (응답)
 *  - 1 RDMA READ/WRITE WR (데이터 이동)
 * 합 3개 + admin/keep_alive 등 special 2개 여유. */

/*
 * [한국어] enum spdk_nvmf_rdma_request_state
 *
 * NVMe-oF RDMA request 의 16-state state machine. capsule 수신 → bdev 실행 →
 * 응답 전송의 전 생명주기를 표현. nvmf_rdma_request_process() 가 단일 step 진행
 * (한 함수 호출 = 한 상태 전이 시도).
 *
 * 핵심 패턴: 각 *_PENDING 상태는 "RDMA queue depth 부족으로 대기" 상태.
 *  - DATA_TRANSFER_TO_CONTROLLER_PENDING: RDMA READ 발행 위한 depth 대기
 *  - DATA_TRANSFER_TO_HOST_PENDING: RDMA WRITE 발행 위한 depth 대기
 *  - READY_TO_COMPLETE_PENDING: SEND 발행 위한 depth 대기
 * QP 의 current_send_depth / current_read_depth 카운터가 한도 도달하면 PENDING 진입,
 * 다른 WR 완료로 depth 회복 시 다음 상태로 진행.
 *
 * 흐름 다이어그램 (write):
 *   FREE → NEW → NEED_BUFFER → HAVE_BUFFER
 *     → DATA_XFER_TO_CTRL_PENDING → TRANSFERRING_HOST_TO_CONTROLLER
 *     → READY_TO_EXECUTE → EXECUTING → EXECUTED
 *     → READY_TO_COMPLETE_PENDING → READY_TO_COMPLETE → COMPLETING → COMPLETED → FREE
 *
 * 흐름 (read):
 *   FREE → NEW → NEED_BUFFER → HAVE_BUFFER
 *     → READY_TO_EXECUTE → EXECUTING → EXECUTED
 *     → DATA_XFER_TO_HOST_PENDING → TRANSFERRING_CONTROLLER_TO_HOST
 *     → READY_TO_COMPLETE_PENDING → READY_TO_COMPLETE → COMPLETING → COMPLETED → FREE
 */
enum spdk_nvmf_rdma_request_state {
	/* The request is not currently in use */
	RDMA_REQUEST_STATE_FREE = 0,
	/* [한국어] 0 — 풀의 free list 에 있는 상태. 다음 RECV WR 완료 시 NEW 로 전이. */

	/* Initial state when request first received */
	RDMA_REQUEST_STATE_NEW,
	/* [한국어] capsule RECV 완료 직후 진입. command opcode/SGL 파싱 + 다음 단계 결정. */

	/* The request is queued until a data buffer is available. */
	RDMA_REQUEST_STATE_NEED_BUFFER,
	/* [한국어] bdev iobuf 풀이 비어 있어 데이터 버퍼 대기. iobuf 회수 시 HAVE_BUFFER. */

	/* The request has a data buffer available. */
	RDMA_REQUEST_STATE_HAVE_BUFFER,
	/* [한국어] 버퍼 확보. read/write 방향에 따라 다음 단계 분기. */

	/* The request is waiting on RDMA queue depth availability
	 * to transfer data from the host to the controller.
	 */
	RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING,
	/* [한국어] write 경로 — host 의 데이터를 RDMA READ 로 가져와야 하는데 current_read_depth
	 * 한도라 대기. 다른 READ 완료 시 TRANSFERRING_HOST_TO_CONTROLLER. */

	/* The request is currently transferring data from the host to the controller. */
	RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
	/* [한국어] RDMA READ WR 발행됨. ibv_poll_cq 가 RDMA READ 완료 통지 시 READY_TO_EXECUTE. */

	/* The request is ready to execute at the block device */
	RDMA_REQUEST_STATE_READY_TO_EXECUTE,
	/* [한국어] 데이터 + command 준비 완료. spdk_nvmf_request_exec 호출 직전. */

	/* The request is currently executing at the block device */
	RDMA_REQUEST_STATE_EXECUTING,
	/* [한국어] bdev I/O 발행됨. bdev 완료 콜백 시 EXECUTED. */

	/* The request finished executing at the block device */
	RDMA_REQUEST_STATE_EXECUTED,
	/* [한국어] bdev 완료. read 면 데이터 전송, write 면 응답만 보내면 됨. */

	/* The request is waiting on RDMA queue depth availability
	 * to transfer data from the controller to the host.
	 */
	RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING,
	/* [한국어] read 경로 — host 로 데이터를 RDMA WRITE 로 전송해야 하는데 send_depth 한도라 대기. */

	/* The request is waiting on RDMA queue depth availability
	 * to send response to the host.
	 */
	RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING,
	/* [한국어] 응답 capsule SEND 위한 depth 대기. */

	/* The request is ready to send a completion */
	RDMA_REQUEST_STATE_READY_TO_COMPLETE,
	/* [한국어] SEND WR 발행 직전. depth 확보 완료. */

	/* The request is currently transferring data from the controller to the host. */
	RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
	/* [한국어] RDMA WRITE 발행됨 (read 경로). 완료 시 READY_TO_COMPLETE_PENDING. */

	/* The request currently has an outstanding completion without an
	 * associated data transfer.
	 */
	RDMA_REQUEST_STATE_COMPLETING,
	/* [한국어] SEND WR 발행됨. SEND 완료 통지 시 COMPLETED. */

	/* The request completed and can be marked free. */
	RDMA_REQUEST_STATE_COMPLETED,
	/* [한국어] 모든 작업 끝. cleanup 후 FREE 로 회수, 새 RECV WR 재게시. */

	/* Terminator */
	RDMA_REQUEST_NUM_STATES,
	/* [한국어] enum count — 통계 배열 크기로 사용. */
};

/*
 * [한국어]
 * nvmf_trace - SPDK trace 프레임워크에 NVMe-oF RDMA 전용 tpoint 를 등록.
 *
 * @return: void (등록 실패 시 assert, 정상이면 조용히 반환).
 *
 * SPDK_TRACE_REGISTER_FN 매크로로 .init_array 섹션에 등록되어 main() 진입 전
 * 자동 호출. spdk_trace_init() 이후 tpoint 를 찾을 수 있도록 미리 등록.
 *
 * 등록 대상:
 *   - OBJECT_NVMF_RDMA_IO ('r'): request 단위 trace object — 상태 전이를 추적하는
 *     논리 entity. spdk_trace_parser 가 request 생애주기를 시각화할 때 사용.
 *   - 13개 request state tpoint (NEW → COMPLETED): request 상태 전이마다 기록.
 *     NEW/COMPLETED 는 'ext' 형식(복수 인자), 나머지는 단일 qpair 포인터 인자.
 *   - 5개 QP lifecycle tpoint (QP_CREATE, IBV_ASYNC_EVENT, CM_ASYNC_EVENT,
 *     QP_DISCONNECT, QP_DESTROY): OBJECT_NONE — request 와 무관한 QP 수준 이벤트.
 *   - BDEV_IO_START/DONE 와 OBJECT_NVMF_RDMA_IO 의 relation 등록: bdev trace 와
 *     nvmf trace 를 연결하여 end-to-end latency 분석 가능.
 *
 * 실행 컨텍스트: 프로세스 초기화 단계 (single-thread). 재진입 없음.
 *
 * 호출 체인:
 *   ELF .init_array → SPDK_TRACE_REGISTER_FN → [nvmf_trace]
 *     → spdk_trace_register_object
 *     → spdk_trace_register_description_ext (NEW, COMPLETED — 멀티인자 tpoint)
 *     → spdk_trace_register_description (나머지 tpoints — 단일 인자)
 *     → spdk_trace_tpoint_register_relation (bdev 연결)
 */
static void
nvmf_trace(void)
{
	/* [한국어] OBJECT_NVMF_RDMA_IO: 'r' 식별자로 request 단위 trace object 등록.
	 * spdk_trace 가 request 생애주기를 추적하는 논리 단위. */
	spdk_trace_register_object(OBJECT_NVMF_RDMA_IO, 'r');

	/* [한국어] NEW/COMPLETED 는 복수 인자(qpair ptr + qd)이므로 ext 형식 사용. */
	struct spdk_trace_tpoint_opts opts[] = {
		{
			/* [한국어] RDMA_REQ_NEW: state FREE → NEW 전이 기록. qd = queue depth. */
			"RDMA_REQ_NEW", TRACE_RDMA_REQUEST_STATE_NEW,
			OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 1,  /* [한국어] 1 = new object */
			{
				{ "qpair", SPDK_TRACE_ARG_TYPE_PTR, 8 },  /* [한국어] qpair 포인터 (8 byte) */
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }      /* [한국어] 현재 queue depth (4 byte) */
			}
		},
		{
			/* [한국어] RDMA_REQ_COMPLETED: state COMPLETED — request 생애 종료. */
			"RDMA_REQ_COMPLETED", TRACE_RDMA_REQUEST_STATE_COMPLETED,
			OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,  /* [한국어] 0 = existing object */
			{
				{ "qpair", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }
			}
		},
	};

	/* [한국어] ext API: 복수 인자 tpoint 를 배열로 일괄 등록. */
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
	/* [한국어] NEED_BUFFER: 버퍼 풀에서 대기 중 상태 진입. */
	spdk_trace_register_description("RDMA_REQ_NEED_BUFFER", TRACE_RDMA_REQUEST_STATE_NEED_BUFFER,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	/* [한국어] HAVE_BUFFER: 버퍼 획득 완료 — parse_sgl/fill_iovs 진행 예정. */
	spdk_trace_register_description("RDMA_REQ_HAVE_BUFFER", TRACE_RDMA_REQUEST_STATE_HAVE_BUFFER,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	/* [한국어] TX_PENDING_C2H: Controller→Host(read) RDMA WRITE 발행 대기 (max_send_depth 초과). */
	spdk_trace_register_description("RDMA_REQ_TX_PENDING_C2H",
					TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	/* [한국어] TX_PENDING_H2C: Host→Controller(write) RDMA READ 발행 대기 (max_read_depth 초과). */
	spdk_trace_register_description("RDMA_REQ_TX_PENDING_H2C",
					TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	/* [한국어] TX_H2C: RDMA READ WR 발행 완료 — host 메모리에서 데이터 읽어오는 중. */
	spdk_trace_register_description("RDMA_REQ_TX_H2C",
					TRACE_RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	/* [한국어] RDY_TO_EXECUTE: 데이터 수신 완료 — bdev I/O 실행 대기 상태. */
	spdk_trace_register_description("RDMA_REQ_RDY_TO_EXECUTE",
					TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	/* [한국어] EXECUTING: spdk_nvmf_request_exec() 호출 — bdev I/O 진행 중. */
	spdk_trace_register_description("RDMA_REQ_EXECUTING",
					TRACE_RDMA_REQUEST_STATE_EXECUTING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	/* [한국어] EXECUTED: bdev I/O 완료 콜백 수신 — 응답 경로 진입. */
	spdk_trace_register_description("RDMA_REQ_EXECUTED",
					TRACE_RDMA_REQUEST_STATE_EXECUTED,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	/* [한국어] RDY2COMPL_PEND: SEND WR 발행 대기 (max_send_depth 초과, SEND 큐 대기). */
	spdk_trace_register_description("RDMA_REQ_RDY2COMPL_PEND",
					TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	/* [한국어] RDY_TO_COMPL: SEND WR 발행 준비 완료 — 즉시 post_send 예정. */
	spdk_trace_register_description("RDMA_REQ_RDY_TO_COMPL",
					TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	/* [한국어] COMPLETING_C2H: RDMA WRITE WR 발행 완료 — CQE 대기 중 (read 응답 전송). */
	spdk_trace_register_description("RDMA_REQ_COMPLETING_C2H",
					TRACE_RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	/* [한국어] COMPLETING: SEND WR 발행 완료 — CQE 대기 중 (최종 응답 capsule 전송). */
	spdk_trace_register_description("RDMA_REQ_COMPLETING",
					TRACE_RDMA_REQUEST_STATE_COMPLETING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");

	/* [한국어] QP lifecycle tpoints — OBJECT_NONE: request 와 무관한 QP 수준 이벤트. */
	spdk_trace_register_description("RDMA_QP_CREATE", TRACE_RDMA_QP_CREATE,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");  /* [한국어] QP 생성 (qpair_initialize) */
	spdk_trace_register_description("RDMA_IBV_ASYNC_EVENT", TRACE_RDMA_IBV_ASYNC_EVENT,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "type");  /* [한국어] ibv async event type */
	spdk_trace_register_description("RDMA_CM_ASYNC_EVENT", TRACE_RDMA_CM_ASYNC_EVENT,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "type");  /* [한국어] rdma_cm event type */
	spdk_trace_register_description("RDMA_QP_DISCONNECT", TRACE_RDMA_QP_DISCONNECT,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");  /* [한국어] QP disconnect 시작 */
	spdk_trace_register_description("RDMA_QP_DESTROY", TRACE_RDMA_QP_DESTROY,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");  /* [한국어] QP 완전 해제 */

	/* [한국어] bdev trace 와 nvmf RDMA trace 를 연결 — end-to-end latency 분석 가능.
	 * BDEV_IO_START 시점에 OBJECT_NVMF_RDMA_IO 를 attach (인자 인덱스 1),
	 * BDEV_IO_DONE 시점에 detach (인덱스 0). */
	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_START, OBJECT_NVMF_RDMA_IO, 1);
	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_DONE, OBJECT_NVMF_RDMA_IO, 0);
}
/* [한국어] ELF .init_array constructor — main() 진입 전 nvmf_trace() 자동 호출.
 * "nvmf_rdma" 이름으로 TRACE_GROUP_NVMF_RDMA 에 묶임. */
SPDK_TRACE_REGISTER_FN(nvmf_trace, "nvmf_rdma", TRACE_GROUP_NVMF_RDMA)

/*
 * [한국어] enum spdk_nvmf_rdma_wr_type
 *
 * ibv_poll_cq 가 반환한 CQE 에서 어떤 종류의 WR 가 완료됐는지 식별하는 type tag.
 * CQE 의 wr_id 는 spdk_nvmf_rdma_wr 포인터로 캐스팅되며, 이 type 필드를 확인해
 * 올바른 핸들러(recv/send/data) 로 분기. ibverbs 자체는 WR 종류를 CQE 에 직접
 * 노출하지 않으므로 별도 type tag 가 필요.
 */
enum spdk_nvmf_rdma_wr_type {
	RDMA_WR_TYPE_RECV,
	/* [한국어] RECV WR 완료 — 새 capsule 도착. nvmf_rdma_recv.rdma_wr 에 설정.
	 * 읽는 자: nvmf_rdma_poller_poll() 의 CQE 처리 분기. */

	RDMA_WR_TYPE_SEND,
	/* [한국어] SEND WR 완료 — 응답 capsule 전송 완료. rsp.wr 에 해당.
	 * 완료 시 current_send_depth 감소 + pending_rdma_send_queue 에서 대기 req 처리. */

	RDMA_WR_TYPE_DATA,
	/* [한국어] RDMA READ 또는 RDMA WRITE WR 완료 — 데이터 전송 완료.
	 * READ 완료: num_outstanding_data_wr 감소, 0 되면 READY_TO_EXECUTE 전이.
	 * WRITE 완료: current_send_depth 감소, 이후 SEND WR 발행 (응답 capsule). */
};

/*
 * [한국어] struct spdk_nvmf_rdma_wr
 *
 * ibv_send_wr / ibv_recv_wr 의 wr_id 에 embed 되는 type tag wrapper.
 * wr_id 에 이 구조체 포인터를 저장 → CQE 수신 시 type 으로 WR 종류 식별.
 * 크기 최소화(1 byte)로 WR 앞에 inline embedding 가능.
 */
struct spdk_nvmf_rdma_wr {
	/* Uses enum spdk_nvmf_rdma_wr_type */
	uint8_t type;
	/* [한국어] WR 종류 식별자 (RECV/SEND/DATA).
	 * 설정자: nvmf_rdma_resources_create() — recv 슬롯마다 RECV 설정;
	 *          nvmf_rdma_setup_request() — req 마다 SEND/DATA 설정.
	 * 읽는 자: nvmf_rdma_poller_poll() — (spdk_nvmf_rdma_wr *)wc.wr_id 후 type 확인.
	 * 값 범위: enum spdk_nvmf_rdma_wr_type 3가지만 허용.
	 * 동기화: QP 는 고정 reactor 스레드에서만 접근 — 락 불필요. */
};

/* This structure holds commands as they are received off the wire.
 * It must be dynamically paired with a full request object
 * (spdk_nvmf_rdma_request) to service a request. It is separate
 * from the request because RDMA does not appear to order
 * completions, so occasionally we'll get a new incoming
 * command when there aren't any free request objects.
 */
/*
 * [한국어] struct spdk_nvmf_rdma_recv
 *
 * 1개의 RECV WR (이벤트 발생기) 의 wrapper. capsule 1개 수신을 위해 미리
 * qpair 에 게시되어 있는 객체. RECV WR 완료 시 ibv_poll_cq 가 본 객체를
 * 가리키는 wr_id 를 반환 → handler 가 request state machine 진행.
 *
 * 미리 게시 패턴: 새 connection 시 max_queue_depth 개의 RECV 를 한꺼번에
 * 게시 → host capsule 도착마다 RECV 1개 소비 → request 처리 끝나면 새 RECV 재게시.
 *
 * 필드:
 *  - wr: ibverbs RECV WR (sg_list/num_sge/wr_id 설정).
 *  - sgl[2]: 1=capsule header(64B nvme cmd), 2=in-capsule immediate data.
 *  - qpair: 소속 QP (cb 에서 역추적).
 *  - buf: in-capsule data 가 도착할 호스트 메모리 영역.
 *  - rdma_wr: type tag — POLL_CQ 에서 RECV vs SEND vs RDMA_READ 구분.
 *  - receive_tsc: capsule 수신 시각 — latency 측정.
 */
struct spdk_nvmf_rdma_recv {
	struct ibv_recv_wr			wr;
	/* [한국어] libibverbs RECV WR 구조체.
	 * 설정자: nvmf_rdma_resources_create() — wr.wr_id = &rdma_wr, sg_list = sgl, num_sge 설정.
	 * 읽는 자: ibv_post_recv()/ibv_post_srq_recv() — HW에 WR 게시.
	 * 값 범위: 유효한 ibv_recv_wr. wr.next = NULL (단일 WR로 게시) 또는 batching 시 체인.
	 * 동기화: QP 고정 reactor 스레드 접근 — 별도 락 없음. */

	struct ibv_sge				sgl[NVMF_DEFAULT_RX_SGE];
	/* [한국어] RECV WR 의 Scatter-Gather List (SGE, Scatter-Gather Element) 배열.
	 * 설정자: nvmf_rdma_resources_create() — sgl[0] = cmds[i] (64B capsule header),
	 *          sgl[1] = bufs[i] (in-capsule data, in_capsule_data_size 크기).
	 * 읽는 자: RDMA HW — capsule 도착 시 이 SGE 가 가리키는 메모리에 DMA 기록.
	 * 값 범위: NVMF_DEFAULT_RX_SGE = 2 (capsule + in-capsule).
	 * 동기화: RECV WR 게시 후 HW 가 기록하므로, CQE 수신 전까지 sgl 메모리에 접근 금지. */

	struct spdk_nvmf_rdma_qpair		*qpair;
	/* [한국어] 이 RECV 가 소속된 QP 역참조 포인터.
	 * 설정자: nvmf_rdma_resources_create() — 각 recv 슬롯 초기화 시 설정.
	 * 읽는 자: nvmf_rdma_poller_poll() — RECV CQE 수신 후 qpair 역추적.
	 * 값 범위: 유효한 rqpair 포인터 (NULL 불가). SRQ 모드에서는 여러 QP 가 공유 recv 풀 사용.
	 * 동기화: SRQ 모드에서 recv 완료 시점에 실제 qpair 가 결정 — 그 전까지 NULL 가능. */

	/* In-capsule data buffer */
	uint8_t					*buf;
	/* [한국어] In-capsule 데이터 수신 버퍼 (NVMe-oF ICD, In-Capsule Data).
	 * 설정자: nvmf_rdma_resources_create() — bufs 배열에서 i번째 슬롯 포인터 저장.
	 * 읽는 자: nvmf_rdma_request_parse_icd() — ICD 유효 시 이 버퍼에서 req.iov 구성.
	 * 값 범위: DMA-가능 hugepage 메모리 (in_capsule_data_size 크기); NULL 이면 ICD 미지원.
	 * 동기화: RECV WR 게시 후 CQE 수신 전까지 HW 가 DMA 기록 중 — 접근 금지. */

	struct spdk_nvmf_rdma_wr		rdma_wr;
	/* [한국어] CQE 에서 WR 종류 식별용 type tag (RDMA_WR_TYPE_RECV 고정).
	 * 설정자: nvmf_rdma_resources_create() — rdma_wr.type = RDMA_WR_TYPE_RECV.
	 * 읽는 자: nvmf_rdma_poller_poll() — (spdk_nvmf_rdma_wr *)wc.wr_id 캐스팅 후 type 확인.
	 * 값 범위: RDMA_WR_TYPE_RECV 만 사용.
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */

	uint64_t				receive_tsc;
	/* [한국어] 이 RECV WR 완료 시각 (TSC 단위, rdtsc 기반).
	 * 설정자: nvmf_rdma_poller_poll() — RECV CQE 처리 시 spdk_get_ticks() 기록.
	 * 읽는 자: nvmf_rdma_request_process() — NEW 상태 진입 시 rdma_req->receive_tsc 에 복사.
	 *           통계: request_latency 계산 (완료 시각 - receive_tsc).
	 * 값 범위: 0 ~ UINT64_MAX (단조 증가, TSC 기반).
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	STAILQ_ENTRY(spdk_nvmf_rdma_recv)	link;
	/* [한국어] free list (resources->incoming_queue) 또는 미사용 recv 풀 연결 노드.
	 * 설정자: nvmf_rdma_poller_poll() — RECV CQE 처리 시 incoming_queue 에 enqueue.
	 *          nvmf_rdma_request_process() — NEW 처리 후 dequeue.
	 *          nvmf_rdma_qpair_queue_recv_wrs() — 처리 완료 후 새 RECV 재게시.
	 * 읽는 자: nvmf_rdma_request_process() — incoming_queue STAILQ_FIRST 로 꺼냄.
	 * 값 범위: 리스트 연결 노드 (다음 포인터, 리스트 미소속 시 미정의).
	 * 동기화: resources 는 단일 reactor 스레드 접근 — 락 불필요. */
};

/*
 * [한국어] struct spdk_nvmf_rdma_request_data
 *
 * RDMA READ/WRITE WR 1개 + 그 SGL 배열. request 가 데이터 전송 시 이 객체로
 * WR 빌드. multi-SGL payload (SGL 16 entries 초과) 의 경우 mempool 에서 추가
 * data_wr 객체를 빌려와 chained list 로 구성.
 */
struct spdk_nvmf_rdma_request_data {
	struct ibv_send_wr		wr;
	/* [한국어] libibverbs SEND WR 구조체 (RDMA READ 또는 RDMA WRITE opcode 설정).
	 * 설정자: nvmf_rdma_fill_wr_sgl() / nvmf_rdma_fill_wr_sgl_with_dif() —
	 *          opcode(IBV_WR_RDMA_READ/WRITE), remote_addr, rkey, sg_list, num_sge 설정.
	 *          nvmf_request_alloc_wrs() — wr.wr_id = (uintptr_t)&rdma_req->data_wr,
	 *          wr.next = &rsp.wr (SEND WR chain 마지막에 연결).
	 * 읽는 자: ibv_post_send() — HW에 WR 게시.
	 * 값 범위: IBV_WR_RDMA_READ (host→ctrl, NVMe write) 또는 IBV_WR_RDMA_WRITE (ctrl→host, NVMe read).
	 * 동기화: 발행 후 RDMA CQE 수신 전까지 HW 사용 중 — wr 필드 수정 금지. */

	struct ibv_sge			sgl[SPDK_NVMF_MAX_SGL_ENTRIES];
	/* [한국어] 데이터 전송 WR 의 로컬 SGE 배열.
	 * 설정자: nvmf_rdma_fill_wr_sgl() — 각 iov entry 를 SGE로 변환 (addr/length/lkey).
	 *          nvmf_rdma_fill_wr_sgl_with_dif() — DIF 스트립 버퍼도 추가 SGE 로 포함.
	 * 읽는 자: ibv_post_send() 를 통해 RDMA HW.
	 * 값 범위: SPDK_NVMF_MAX_SGL_ENTRIES(16)개 — 초과 시 mempool 에서 추가 data_wr 할당.
	 * 동기화: 발행 후 CQE 수신 전 HW 가 DMA로 접근 — CQE 수신 후에만 reclaim 가능. */
};

/*
 * [한국어] struct spdk_nvmf_rdma_request
 *
 * 1개 NVMe-oF request 의 RDMA-specific 컨테이너. spdk_nvmf_request (공통 NVMe-oF
 * request) 를 포함하면서 RDMA WR/SGL/state 등을 추가. resources 풀에서 미리
 * max_queue_depth 만큼 할당.
 *
 * 핵심 필드:
 *  - req: 공통 NVMe-oF request (cmd/rsp/iov 등).
 *  - state: enum spdk_nvmf_rdma_request_state — 16-state machine 의 현재 위치.
 *  - data_wr/rsp_wr: type tag — RDMA READ/WRITE WR 와 SEND WR 구분.
 *  - recv: 이 request 가 처리 중인 RECV (capsule 출처).
 *  - rsp.wr/sgl: 응답 capsule SEND WR.
 *  - data: 데이터 전송 WR (single SGL 케이스).
 *  - num_outstanding_data_wr: 발행됐지만 아직 완료 안 된 RDMA WR 수.
 *  - num_remaining_data_wr: 아직 발행 안 한 data WR 수 (split 시).
 *  - fused_pair: NVMe FUSE (Compare+Write 짝) 의 second request 포인터.
 *  - state_link: 같은 state 의 다른 request 들과 STAILQ 연결.
 *  - transfer_wr / remaining_tranfer_in_wrs: chained WR list (multi-SGL 분할).
 */
struct spdk_nvmf_rdma_request {
	struct spdk_nvmf_request		req;
	/* [한국어] 공통 NVMe-oF request (cmd/rsp/iov/qpair 등 포함).
	 * 설정자: nvmf_rdma_resources_create() — 초기 할당 및 req.qpair 연결.
	 *          nvmf_rdma_request_process() — cmd/rsp 포인터를 recv/resources 에서 연결.
	 * 읽는 자: spdk_nvmf_request_exec() / nvmf_ctrlr_process_* — NVMe 명령 실행.
	 * 값 범위: 유효한 spdk_nvmf_request 구조체 (초기화 후 항상 유효).
	 * 동기화: QP 고정 reactor 스레드 — 락 없음. EXECUTING 상태 중 bdev 스레드 콜백 주의. */

	bool					fused_failed;
	/* [한국어] NVMe FUSE(Fused Operation) 중 first 명령 실패 여부.
	 * 설정자: nvmf_rdma_check_fused_ordering() — first 명령 실패 시 true 로 설정.
	 * 읽는 자: nvmf_rdma_request_process() — FUSE second 명령 처리 시 확인,
	 *           true 면 second 도 abort 완료 상태로 처리.
	 * 값 범위: true(실패)/false(정상).
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	struct spdk_nvmf_rdma_wr		data_wr;
	/* [한국어] 데이터 WR(RDMA READ/WRITE)의 type tag (RDMA_WR_TYPE_DATA).
	 * 설정자: nvmf_rdma_setup_request() — data_wr.type = RDMA_WR_TYPE_DATA.
	 *          nvmf_request_alloc_wrs() — data.wr.wr_id = (uintptr_t)&data_wr.
	 * 읽는 자: nvmf_rdma_poller_poll() — CQE 처리 시 type 확인으로 DATA WR 완료 식별.
	 * 값 범위: RDMA_WR_TYPE_DATA 고정.
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */

	struct spdk_nvmf_rdma_wr		rsp_wr;
	/* [한국어] 응답 SEND WR의 type tag (RDMA_WR_TYPE_SEND).
	 * 설정자: nvmf_rdma_setup_request() — rsp_wr.type = RDMA_WR_TYPE_SEND.
	 *          rsp.wr.wr_id = (uintptr_t)&rsp_wr.
	 * 읽는 자: nvmf_rdma_poller_poll() — SEND CQE 수신 시 SEND WR 완료 식별.
	 * 값 범위: RDMA_WR_TYPE_SEND 고정.
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */

	/* Uses enum spdk_nvmf_rdma_request_state */
	uint8_t					state;
	/* [한국어] 현재 state machine 상태 (enum spdk_nvmf_rdma_request_state).
	 * 설정자: nvmf_rdma_request_process() — 상태 전이마다 직접 설정.
	 *          _nvmf_rdma_request_free() — FREE 로 초기화.
	 * 읽는 자: nvmf_rdma_request_process() / nvmf_rdma_poller_poll() / abort 경로.
	 * 값 범위: 0(FREE) ~ 15(COMPLETED), 16-state machine.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요 (EXECUTING 예외: bdev 콜백). */

	/* Data offset in req.iov */
	uint32_t				offset;
	/* [한국어] multi-part RDMA READ/WRITE 시 현재까지 처리된 iov 데이터 byte offset.
	 * 설정자: nvmf_rdma_request_fill_iovs() — 부분 처리 후 진행 위치 기록.
	 *          nvmf_rdma_request_reset_transfer_in() — 0으로 리셋.
	 * 읽는 자: request_prepare_transfer_in_part() — 다음 WR 발행 시작점 참조.
	 * 값 범위: 0 ~ 전체 payload 크기 (byte).
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	struct spdk_nvmf_rdma_recv		*recv;
	/* [한국어] 이 request 를 트리거한 RECV WR (캡슐 수신 객체) 역참조.
	 * 설정자: nvmf_rdma_request_process() — NEW 상태 진입 시 recv 연결.
	 * 읽는 자: nvmf_rdma_dump_request() — 디버그 덤프;
	 *           nvmf_rdma_qpair_queue_recv_wrs() — 처리 완료 후 recv 재게시.
	 * 값 범위: 유효한 spdk_nvmf_rdma_recv 포인터 (NEW~COMPLETED 상태에서 유효).
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	struct {
		struct	ibv_send_wr		wr;
		/* [한국어] 응답 capsule 전송용 ibv_send_wr (opcode = IBV_WR_SEND 또는 SEND_WITH_INV).
		 * 설정자: nvmf_rdma_setup_request() — wr.wr_id/sgl/num_sge/opcode 설정.
		 * 읽는 자: ibv_post_send() — READY_TO_COMPLETE 상태에서 발행.
		 * 값 범위: SEND 또는 SEND_WITH_INV (MR 무효화 최적화).
		 * 동기화: 발행 후 SEND CQE 수신 전까지 HW 사용 중 — wr 수정 금지. */

		struct	ibv_sge			sgl[NVMF_DEFAULT_RSP_SGE];
		/* [한국어] 응답 SEND WR 의 SGE 배열 (capsule body, 16B NVMe cqe).
		 * 설정자: nvmf_rdma_setup_request() — sgl[0].addr = &resources->cpls[i].
		 * 읽는 자: RDMA HW (SEND WR 발행 시).
		 * 값 범위: NVMF_DEFAULT_RSP_SGE = 1 (응답은 항상 단일 SGE).
		 * 동기화: 발행 후 CQE 수신 전 HW 접근 중. */
	} rsp;

	uint16_t				iovpos;
	/* [한국어] req.iov[] 에서 다음 처리할 iov 인덱스 (multi-iov split 추적).
	 * 설정자: nvmf_rdma_request_fill_iovs() — 처리 완료한 iov 수만큼 증가.
	 *          nvmf_rdma_request_reset_transfer_in() — 0으로 리셋.
	 * 읽는 자: request_prepare_transfer_in_part() — 다음 batch iov 시작점.
	 * 값 범위: 0 ~ req.iovcnt (req.iovcnt 초과 불가).
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	uint16_t				num_outstanding_data_wr;
	/* [한국어] 발행 완료됐지만 아직 CQE 를 받지 못한 데이터 WR (RDMA READ/WRITE) 수.
	 * 설정자: request_transfer_in()/request_transfer_out() — WR 발행 시 증가.
	 *          nvmf_rdma_poller_poll() — DATA WR CQE 수신 시 감소.
	 *          nvmf_rdma_request_free_data() — 해제 시 0으로 초기화.
	 * 읽는 자: nvmf_rdma_request_process() — 0 이 되면 READY_TO_EXECUTE/READY_TO_COMPLETE 전이.
	 * 값 범위: 0 ~ max_read_depth (또는 max_send_depth).
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	/* Used to split Write IO with multi SGL payload */
	uint16_t				num_remaining_data_wr;
	/* [한국어] 아직 발행되지 않은 data WR 수 (multi-SGL payload 분할 시).
	 * NVMe write 의 SGL 이 max_read_depth 를 초과하는 경우, WR 을 배치로 나눠
	 * 발행하며 남은 배치 수를 추적.
	 * 설정자: nvmf_rdma_calc_num_wrs() — 필요한 WR 총 수 계산 후 설정.
	 *          request_transfer_in() — WR 발행 시마다 감소.
	 *          nvmf_rdma_request_reset_transfer_in() — 재발행 시 복원.
	 * 읽는 자: request_prepare_transfer_in_part() — 남은 WR 개수 확인.
	 * 값 범위: 0 ~ SPDK_NVMF_MAX_SGL_ENTRIES.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	uint64_t				receive_tsc;
	/* [한국어] 이 request 를 트리거한 capsule 수신 TSC (recv->receive_tsc 복사본).
	 * 설정자: nvmf_rdma_request_process() — NEW 상태에서 recv->receive_tsc 를 복사.
	 * 읽는 자: nvmf_rdma_poller_poll() — 완료 시 request_latency 통계 계산에 사용.
	 * 값 범위: 0 ~ UINT64_MAX (단조 증가, TSC 기반).
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	struct spdk_nvmf_rdma_request		*fused_pair;
	/* [한국어] NVMe FUSE 명령 쌍의 파트너 포인터.
	 * FUSE_FIRST 를 가진 request 에 FUSE_SECOND request 가 도착하면 이 포인터를 연결.
	 * 설정자: nvmf_rdma_check_fused_ordering() — FUSE_SECOND 도착 시 FIRST.fused_pair = SECOND.
	 * 읽는 자: nvmf_rdma_request_process() — FUSE 쌍이 모두 준비됐는지 확인.
	 * 값 범위: NULL (단순 request), 또는 유효한 fused_pair 포인터.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	STAILQ_ENTRY(spdk_nvmf_rdma_request)	state_link;
	/* [한국어] 상태별 STAILQ 연결 노드.
	 * pending_rdma_read_queue (DATA_XFER_TO_CTRL_PENDING),
	 * pending_rdma_write_queue (DATA_XFER_TO_HOST_PENDING),
	 * pending_rdma_send_queue (READY_TO_COMPLETE_PENDING) 중 하나에 소속.
	 * 설정자: nvmf_rdma_request_process() — PENDING 상태 진입 시 STAILQ_INSERT_TAIL.
	 *          _nvmf_rdma_qpair_abort_request() — abort 시 STAILQ_REMOVE.
	 * 읽는 자: nvmf_rdma_qpair_process_pending() — depth 여유 생길 때 dequeue.
	 * 값 범위: 리스트 노드 (소속 리스트 외 접근 undefined).
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	struct ibv_send_wr			*remaining_tranfer_in_wrs;
	/* [한국어] multi-SGL split 에서 아직 발행되지 않은 WR chain 의 선두 포인터.
	 * (오타 주의: "transfer" 가 "tranfer"로 철자 오류이나 코드 수정 없이 유지)
	 * 설정자: nvmf_rdma_request_fill_iovs() — 첫 batch 이후 남은 WR chain 저장.
	 *          nvmf_rdma_request_free_data() — 처리 완료 후 NULL 로 초기화.
	 * 읽는 자: request_prepare_transfer_in_part() — 다음 batch WR 발행 시.
	 *           _nvmf_rdma_request_free_data() — mempool 반환 시.
	 * 값 범위: NULL (단순 요청) 또는 유효한 ibv_send_wr 체인 포인터.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	struct ibv_send_wr			*transfer_wr;
	/* [한국어] 현재 발행 중인 (또는 발행 완료된) 데이터 WR chain 의 선두 포인터.
	 * 설정자: request_transfer_in()/request_transfer_out() — ibv_post_send 직전 설정.
	 *          nvmf_rdma_request_free_data() — 해제 시 NULL.
	 * 읽는 자: nvmf_rdma_request_free_data() — mempool 반환용 chain 순회.
	 *           nvmf_rdma_request_reset_transfer_in() — 재발행 준비 시.
	 * 값 범위: NULL 또는 유효한 ibv_send_wr 체인 포인터.
	 * 동기화: 발행 후 CQE 수신 전 HW 사용 중 — wr 수정 금지. */

	struct spdk_nvmf_rdma_request_data	data;
	/* [한국어] 데이터 전송용 WR + SGL 컨테이너 (inline embedded, mempool 불필요한 첫 WR).
	 * 설정자: nvmf_rdma_fill_wr_sgl() — 첫 번째 데이터 WR 빌드.
	 *          nvmf_rdma_request_free_data() — SGL 초기화.
	 * 읽는 자: ibv_post_send() — HW 에 발행.
	 * 값 범위: 유효한 ibv_send_wr + SGE 배열 (발행 시).
	 * 동기화: 발행 후 CQE 수신 전 HW 접근 중 — 수정 금지. */
};

/*
 * [한국어] struct spdk_nvmf_rdma_resource_opts
 *
 * nvmf_rdma_resources_create() 에 전달하는 입력 파라미터 묶음.
 * QP 별 자원(shared=false) 또는 SRQ 공유 자원(shared=true) 생성 시 모두 사용.
 * 이 구조체는 stack 에 임시로 생성되어 create 함수에 전달 후 폐기된다.
 */
struct spdk_nvmf_rdma_resource_opts {
	struct spdk_nvmf_rdma_qpair	*qpair;
	/* [한국어] 자원을 소유할 QP (또는 SRQ 모드에서 대표 qpair).
	 * 설정자: nvmf_rdma_qpair_initialize() / nvmf_rdma_poller_create() — 생성 시 전달.
	 * 읽는 자: nvmf_rdma_resources_create() — recv 슬롯 qpair 포인터 초기화에 사용.
	 * 값 범위: 유효한 rqpair 포인터 (NULL 불가).
	 * 동기화: 구조체 자체가 stack 임시 변수 — thread-local. */

	/* qp points either to an ibv_qp object or an ibv_srq object depending on the value of shared. */
	void				*qp;
	/* [한국어] shared=false 시 ibv_qp 포인터, shared=true 시 ibv_srq 포인터 (void* 다형성).
	 * 설정자: nvmf_rdma_qpair_initialize() — rdma_qp->qp (ibv_qp).
	 *          nvmf_rdma_poller_create() — srq->srq (ibv_srq).
	 * 읽는 자: nvmf_rdma_resources_create() — RECV WR 게시 대상 선택.
	 * 값 범위: 유효한 ibv_qp 또는 ibv_srq 포인터 (NULL 불가).
	 * 동기화: stack 임시 구조체 — thread-local. */

	struct spdk_rdma_utils_mem_map	*map;
	/* [한국어] DMA 메모리 등록 map (lkey/rkey 조회용).
	 * 설정자: nvmf_rdma_qpair_initialize() / nvmf_rdma_poller_create() — device->map 전달.
	 * 읽는 자: nvmf_rdma_resources_create() — cmds/cpls/bufs 버퍼의 lkey 등록 시.
	 * 값 범위: 유효한 spdk_rdma_utils_mem_map (device 초기화 시 생성된 것).
	 * 동기화: map 자체는 read-only 조회만 — 별도 락 불필요. */

	uint32_t			max_queue_depth;
	/* [한국어] 이 QP(또는 SRQ)에서 동시에 처리할 수 있는 최대 I/O 수.
	 * 설정자: nvmf_rdma_qpair_initialize() — rqpair->max_queue_depth 전달.
	 *          nvmf_rdma_poller_create() — max_srq_depth 전달.
	 * 읽는 자: nvmf_rdma_resources_create() — reqs/recvs/cmds/cpls/bufs 배열 크기 결정.
	 * 값 범위: 1 ~ NVMF 설정 최대값 (보통 128~1024).
	 * 동기화: 초기화 후 변경 없음 — 락 불필요. */

	uint32_t			in_capsule_data_size;
	/* [한국어] In-capsule 데이터 최대 크기 (byte). bufs 배열의 슬롯 크기 결정.
	 * 설정자: nvmf_rdma_qpair_initialize() — transport opts 에서 가져온 값.
	 * 읽는 자: nvmf_rdma_resources_create() — bufs 총 크기 = max_queue_depth * in_capsule_data_size.
	 * 값 범위: 0(ICD 비활성) 또는 4096의 배수 (NVMe-oF spec: 최대 transport 협상치).
	 * 동기화: 초기화 후 변경 없음 — 락 불필요. */

	bool				shared;
	/* [한국어] SRQ (Shared Receive Queue) 모드 여부.
	 * true: SRQ — poller 레벨 공유 RECV 풀 (모든 QP 공유, 메모리 절약).
	 * false: per-QP RECV — 각 QP 가 독립적 RECV 풀 (더 간단하지만 메모리 많이 사용).
	 * 설정자: nvmf_rdma_qpair_initialize() (false) / nvmf_rdma_poller_create() (true).
	 * 읽는 자: nvmf_rdma_resources_create() — RECV WR 게시 함수 분기.
	 * 동기화: 초기화 후 변경 없음 — 락 불필요. */
};

/*
 * [한국어] struct spdk_nvmf_rdma_resources
 *
 * QP(또는 SRQ) 의 런타임 I/O 자원 풀 컨테이너. nvmf_rdma_resources_create() 가
 * max_queue_depth 크기 배열들을 DMA-capable hugepage 로 일괄 할당하고 이 구조체에
 * 저장. 모든 배열은 인덱스로 1:1 대응 (reqs[i] ↔ recvs[i] ↔ cmds[i] ↔ cpls[i] ↔ bufs[i]).
 */
struct spdk_nvmf_rdma_resources {
	/* Array of size "max_queue_depth" containing RDMA requests. */
	struct spdk_nvmf_rdma_request		*reqs;
	/* [한국어] RDMA request 객체 배열 (max_queue_depth 크기).
	 * 설정자: nvmf_rdma_resources_create() — spdk_zmalloc DMA 할당 후 free_queue 에 등록.
	 * 읽는 자: free_queue 에서 꺼내 사용; nvmf_rdma_dump_qpair_contents() — 디버그 덤프.
	 *           _nvmf_rdma_qpair_abort_request() — abort 대상 검색.
	 * 값 범위: max_queue_depth 개의 유효한 spdk_nvmf_rdma_request (해제 시 NULL).
	 * 동기화: free_queue STAILQ 로 관리 — 단일 reactor 스레드 접근, 락 없음. */

	/* Array of size "max_queue_depth" containing RDMA recvs. */
	struct spdk_nvmf_rdma_recv		*recvs;
	/* [한국어] RECV WR 메타데이터 배열 (max_queue_depth 크기).
	 * 설정자: nvmf_rdma_resources_create() — spdk_zmalloc 후 각 슬롯 WR 초기화 + RECV 게시.
	 * 읽는 자: nvmf_rdma_poller_poll() — RECV CQE wc.wr_id → rdma_wr → recv 복원.
	 *           nvmf_rdma_qpair_queue_recv_wrs() — 처리 완료 후 재게시.
	 * 값 범위: max_queue_depth 개의 유효한 spdk_nvmf_rdma_recv (해제 시 NULL).
	 * 동기화: incoming_queue STAILQ 로 관리 — 단일 reactor 스레드, 락 없음. */

	/* Array of size "max_queue_depth" containing 64 byte capsules
	 * used for receive.
	 */
	union nvmf_h2c_msg			*cmds;
	/* [한국어] Host-to-Controller 캡슐 수신 버퍼 배열 (각 64B NVMe 명령 capsule).
	 * 설정자: nvmf_rdma_resources_create() — spdk_zmalloc (DMA, 4KB align);
	 *          recvs[i].sgl[0].addr = cmds[i] 로 연결.
	 * 읽는 자: nvmf_rdma_request_process() — NEW 상태에서 req.cmd = &cmds[i] 설정.
	 *           nvmf_ctrlr_process_* — NVMe 명령 파싱.
	 * 값 범위: max_queue_depth 개 (각 64B, RDMA HW 가 DMA 기록).
	 * 동기화: RECV WR 게시 중 HW DMA 기록 중 — CQE 수신 후에만 접근 가능. */

	/* Array of size "max_queue_depth" containing 16 byte completions
	 * to be sent back to the user.
	 */
	union nvmf_c2h_msg			*cpls;
	/* [한국어] Controller-to-Host 완료 응답 버퍼 배열 (각 16B NVMe completion capsule).
	 * 설정자: nvmf_rdma_resources_create() — spdk_zmalloc (DMA, 4KB align);
	 *          reqs[i].rsp.sgl[0].addr = cpls[i] 로 연결.
	 * 읽는 자: nvmf_rdma_request_process() — NEW 상태에서 req.rsp = &cpls[i] 설정;
	 *           SEND WR 발행 시 RDMA HW 가 DMA 읽어 host 로 전송.
	 * 값 범위: max_queue_depth 개 (각 16B).
	 * 동기화: SEND WR 발행 후 SEND CQE 수신 전까지 HW 접근 중 — 수정 금지. */

	/* Array of size "max_queue_depth * InCapsuleDataSize" containing
	 * buffers to be used for in capsule data.
	 */
	void					*bufs;
	/* [한국어] In-capsule 데이터 수신 버퍼 배열 (max_queue_depth * in_capsule_data_size byte).
	 * 설정자: nvmf_rdma_resources_create() — spdk_zmalloc (DMA, 4KB align);
	 *          recvs[i].sgl[1].addr = bufs[i * in_capsule_data_size] 로 연결 (ICD 사용 시).
	 * 읽는 자: nvmf_rdma_request_parse_icd() — ICD 유효 시 req.iov 구성에 이 버퍼 사용.
	 * 값 범위: in_capsule_data_size 가 0이면 NULL; 아니면 DMA 가능 hugepage.
	 * 동기화: RECV WR 게시 후 CQE 수신 전 HW DMA 기록 중 — 접근 금지. */

	/* Receives that are waiting for a request object */
	STAILQ_HEAD(, spdk_nvmf_rdma_recv)	incoming_queue;
	/* [한국어] 도착한 RECV 중 아직 free request 가 없어 대기 중인 recv 큐.
	 * RDMA 는 completions 순서를 보장하지 않아, 드물게 free request 가 없는 상태에서
	 * 새 capsule 이 도착할 수 있음 — 이 큐에서 대기 후 free_queue 에 request 생기면 처리.
	 * 설정자: nvmf_rdma_poller_poll() — free_queue 가 비어 있으면 incoming_queue 에 push.
	 * 읽는 자: nvmf_rdma_request_process() — NEW 상태 처리 시 incoming_queue 에서 꺼냄.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	/* Queue to track free requests */
	STAILQ_HEAD(, spdk_nvmf_rdma_request)	free_queue;
	/* [한국어] 현재 사용 가능한(FREE 상태) request 객체 풀 큐.
	 * 설정자: nvmf_rdma_resources_create() — 초기화 시 reqs 배열 전체 enqueue.
	 *          _nvmf_rdma_request_free() — request 완료 후 STAILQ_INSERT_HEAD 로 반환.
	 * 읽는 자: nvmf_rdma_request_process() — NEW 상태에서 STAILQ_FIRST/REMOVE 로 획득.
	 * 값 범위: 0(모두 in-flight) ~ max_queue_depth(모두 idle).
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */
};

/* [한국어] ibv async event 처리 시 사용되는 콜백 함수 포인터 타입.
 * 설정자: nvmf_rdma_qpair_last_wqe_event() 등 async event 핸들러.
 * 읽는 자: nvmf_rdma_qpair_process_last_wqe_event() — last_wqe_reached 처리.
 * 값 범위: 유효한 함수 포인터 또는 NULL.
 * 동기화: reactor 스레드 send_msg 를 통해 안전하게 호출. */
typedef void (*spdk_nvmf_rdma_qpair_ibv_event)(struct spdk_nvmf_rdma_qpair *rqpair);

/* [한국어] poller destroy 완료 콜백 타입. rpoller->destroy_cb 에 저장.
 * device removal 시 poller drain 완료 후 호출 — 실제 해제 트리거.
 * 값 범위: 유효한 함수 포인터 (destroy_cb_ctx 와 항상 쌍으로 사용). */
typedef void (*spdk_poller_destroy_cb)(void *ctx);

/*
 * [한국어] struct spdk_nvmf_rdma_ibv_event_ctx
 *
 * ibv async event 처리를 위한 비동기 컨텍스트 구조체.
 * last_wqe_reached event 처리 시 spdk_thread_send_msg() 의 ctx 인자로 전달.
 * heap 할당 후 콜백 완료 시 free.
 */
struct spdk_nvmf_rdma_ibv_event_ctx {
	struct spdk_nvmf_rdma_qpair			*rqpair;
	/* [한국어] 이 async event 가 발생한 QP 역참조.
	 * 설정자: nvmf_rdma_send_qpair_last_wqe_event() — ctx 할당 시 rqpair 저장.
	 * 읽는 자: nvmf_rdma_qpair_process_last_wqe_event() — 콜백에서 qpair 접근.
	 * 값 범위: 유효한 rqpair 포인터 (ctx 생존 기간 동안 유효 보장).
	 * 동기화: spdk_thread_send_msg 로 reactor 스레드에서 실행 — 락 불필요. */
};

/*
 * [한국어] struct spdk_nvmf_rdma_qpair
 *
 * NVMe-oF RDMA Queue Pair. 1 connection = 1 admin QP + N IO QP. 각 QP 가
 * ibverbs QP 1개 + 별도의 RECV 풀 + SEND 큐 + RDMA READ depth 추적을 가짐.
 *
 * 핵심 필드 그룹:
 *  - qpair: 공통 NVMe-oF qpair (qid, state, listener 등).
 *  - device/poller: 소속 RDMA device + 폴링하는 poller (1 poller = 1 reactor thread).
 *  - rdma_qp/cm_id: ibverbs QP + RDMA CM connection ID.
 *  - srq: Shared Receive Queue (모든 QP 가 RECV 풀 공유, 메모리 절약).
 *  - qp_num: QP number (rdma_cm 의 RB tree key, O(log n) 검색).
 *  - max_*/current_*_depth: WR 깊이 한도 + 현재 사용량 (state PENDING 트리거).
 *  - resources: RDMA request/recv/cmd/cpl 풀.
 *  - pending_rdma_{read,write,send}_queue: queue depth 부족으로 대기 중인 request.
 *  - fused_first: NVMe FUSE 의 first request (second 대기 중).
 *  - last_wqe_reached: async event 처리 완료 표시.
 *  - to_close: nvmf_rdma_close_qpair 호출됨 (graceful shutdown 시작).
 */
struct spdk_nvmf_rdma_qpair {
	struct spdk_nvmf_qpair			qpair;

	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poller		*poller;

	struct spdk_rdma_provider_qp		*rdma_qp;
	struct rdma_cm_id			*cm_id;
	struct spdk_rdma_provider_srq		*srq;
	struct rdma_cm_id			*listen_id;

	/* Cache the QP number to improve QP search by RB tree. */
	uint32_t				qp_num;
	/* [한국어] ibverbs QP number 캐시 — RB tree (poller->qpairs) 의 key.
	 * 매번 rdma_qp 에서 가져오는 것보다 캐시가 O(log n) 검색에서 가속. */

	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t				max_queue_depth;
	/* [한국어] = NVMe-oF qsize. resources 풀 크기 결정. */

	/* The maximum number of active RDMA READ and ATOMIC operations at one time */
	uint16_t				max_read_depth;
	/* [한국어] = init_depth (CONNECT 시 협상). RDMA READ depth 한도. */

	/* The maximum number of RDMA SEND operations at one time */
	uint32_t				max_send_depth;
	/* [한국어] SEND/WRITE 통합 한도 (post_send 의 ord 와 다름). MAX_WR_PER_QP 산정. */

	/* The current number of outstanding WRs from this qpair's
	 * recv queue. Should not exceed device->attr.max_queue_depth.
	 */
	uint16_t				current_recv_depth;

	/* The current number of active RDMA READ operations */
	uint16_t				current_read_depth;
	/* [한국어] RDMA READ in-flight 수. max_read_depth 도달 시 state DATA_XFER_TO_CTRL_PENDING. */

	/* The current number of posted WRs from this qpair's
	 * send queue. Should not exceed max_send_depth.
	 */
	uint32_t				current_send_depth;
	/* [한국어] SEND/RDMA WRITE in-flight 합산. max_send_depth 도달 시 PENDING. */

	/* The maximum number of SGEs per WR on the send queue */
	uint32_t				max_send_sge;
	/* [한국어] SEND/RDMA WRITE WR 당 최대 SGE(Scatter-Gather Element) 수.
	 * 설정자: nvmf_rdma_qpair_initialize() — device_attr.max_sge 로 클램프.
	 * 읽는 자: nvmf_rdma_fill_wr_sgl() — SGE 배열 overflow 방지.
	 * 값 범위: 1 ~ device_attr.max_sge (HCA 하드웨어 한계).
	 * 동기화: 초기화 후 변경 없음 — 락 불필요. */

	/* The maximum number of SGEs per WR on the recv queue */
	uint32_t				max_recv_sge;
	/* [한국어] RECV WR 당 최대 SGE 수.
	 * 설정자: nvmf_rdma_qpair_initialize() — NVMF_DEFAULT_RX_SGE(2) 로 설정.
	 * 읽는 자: nvmf_rdma_resources_create() — recv WR sgl 초기화 시.
	 * 값 범위: NVMF_DEFAULT_RX_SGE(2) = capsule header + in-capsule data.
	 * 동기화: 초기화 후 변경 없음 — 락 불필요. */

	bool					ibv_in_error_state;
	/* [한국어] ibv QP 가 error 상태에 진입했음을 표시.
	 * IBV_EVENT_QP_FATAL / IBV_EVENT_QP_ACCESS_ERR 등 async error event 수신 시 true.
	 * 설정자: nvmf_process_ib_event() — error 관련 async event 처리 시.
	 * 읽는 자: _poller_reset_failed_recvs() / _poller_reset_failed_sends() —
	 *           error 상태의 QP WR 를 처리하는 recovery 경로.
	 * 값 범위: true(error)/false(정상).
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	struct spdk_nvmf_rdma_resources		*resources;
	/* [한국어] 이 QP(또는 공유 SRQ)의 request/recv/cmd/cpl/buf 풀.
	 * 설정자: nvmf_rdma_qpair_initialize() — per-QP 자원 생성;
	 *          nvmf_rdma_poll_group_add() — SRQ 공유 모드 시 poller->resources 연결.
	 * 읽는 자: nvmf_rdma_request_process() / nvmf_rdma_dump_qpair_contents() 등.
	 * 값 범위: 유효한 spdk_nvmf_rdma_resources 포인터 (NULL 이면 초기화 전).
	 * 동기화: QP 고정 reactor 스레드 접근 — 락 불필요. */

	STAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_rdma_read_queue;
	/* [한국어] RDMA READ 발행 대기 큐 (state DATA_TRANSFER_TO_CONTROLLER_PENDING).
	 * max_read_depth 초과 시 request 를 여기에 넣어 depth 여유 생길 때까지 대기.
	 * 설정자: nvmf_rdma_request_process() — PENDING 상태 진입 시 enqueue.
	 * 읽는 자: nvmf_rdma_qpair_process_pending() — RECV CQE 처리 후 dequeue.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	STAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_rdma_write_queue;
	/* [한국어] RDMA WRITE 발행 대기 큐 (state DATA_TRANSFER_TO_HOST_PENDING).
	 * max_send_depth 초과 시 read 응답용 WRITE request 가 여기서 대기.
	 * 설정자: nvmf_rdma_request_process() — PENDING 상태 진입 시 enqueue.
	 * 읽는 자: nvmf_rdma_qpair_process_pending() — SEND CQE 처리 후 dequeue.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	STAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_rdma_send_queue;
	/* [한국어] SEND WR 발행 대기 큐 (state READY_TO_COMPLETE_PENDING).
	 * max_send_depth 초과 시 응답 SEND request 가 여기서 대기.
	 * 설정자: nvmf_rdma_request_process() / nvmf_rdma_request_set_abort_status() — enqueue.
	 * 읽는 자: nvmf_rdma_qpair_process_pending() — SEND CQE 처리 후 dequeue.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	RB_ENTRY(spdk_nvmf_rdma_qpair)		node;
	/* [한국어] poller->qpairs RB tree 의 노드 (키: qp_num).
	 * 설정자: RB_INSERT — nvmf_rdma_poll_group_add() 시 tree 에 삽입.
	 *          RB_REMOVE — nvmf_rdma_poll_group_remove() 시 제거.
	 * 읽는 자: get_rdma_qpair_from_wc() — CQE 의 qp_num 으로 O(log n) QP 검색.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	TAILQ_ENTRY(spdk_nvmf_rdma_qpair)	active_link;
	/* [한국어] poller->active_qpairs TAILQ 연결 노드 (활성 QP 순회용).
	 * 설정자: nvmf_rdma_poll_group_add() — TAILQ_INSERT_TAIL.
	 *          nvmf_rdma_poll_group_remove() — TAILQ_REMOVE.
	 * 읽는 자: nvmf_rdma_poller_poll() — 모든 active QP 순회.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	STAILQ_ENTRY(spdk_nvmf_rdma_qpair)	recv_link;
	/* [한국어] poller->qpairs_pending_recv STAILQ 연결 노드.
	 * RECV WR 재게시 대기 QP 리스트 — SRQ 가득 찼을 때 사용.
	 * 설정자: _poller_submit_recvs() / nvmf_rdma_qpair_queue_recv_wrs().
	 * 읽는 자: _poller_submit_recvs() — 순서대로 RECV 게시 시도.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	STAILQ_ENTRY(spdk_nvmf_rdma_qpair)	send_link;
	/* [한국어] poller->qpairs_pending_send STAILQ 연결 노드.
	 * SEND WR batching 대기 QP 리스트 (no_wr_batching=false 일 때).
	 * 설정자: nvmf_rdma_request_process() — READY_TO_COMPLETE 에서 batch 에 추가.
	 * 읽는 자: _poller_submit_sends() — CQ poll 배치 종료 시 일괄 post_send.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	/* Points to the a request that has fuse bits set to
	 * SPDK_NVME_CMD_FUSE_FIRST, when the qpair is waiting
	 * for the request that has SPDK_NVME_CMD_FUSE_SECOND.
	 */
	struct spdk_nvmf_rdma_request		*fused_first;
	/* [한국어] NVMe FUSE (Fused Operation) 에서 FUSE_FIRST 플래그를 가진 request 포인터.
	 * Compare+Write 같은 atomic FUSE 명령은 FIRST + SECOND 가 쌍으로 처리되어야 함.
	 * FIRST 도착 후 SECOND 를 기다리는 동안 이 포인터로 저장.
	 * 설정자: nvmf_rdma_check_fused_ordering() — FUSE_FIRST 도착 시 rqpair->fused_first = req.
	 *          nvmf_rdma_check_fused_ordering() — FUSE_SECOND 와 매칭 완료 후 NULL 로 초기화.
	 * 읽는 자: nvmf_rdma_check_fused_ordering() — SECOND 도착 시 FIRST 와 연결.
	 * 값 범위: NULL (대기 중인 FIRST 없음) 또는 유효한 request 포인터.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	/*
	 * io_channel which is used to destroy qpair when it is removed from poll group
	 */
	struct spdk_io_channel		*destruct_channel;
	/* [한국어] QP 가 poll_group 에서 제거될 때 정리 작업에 사용할 io_channel.
	 * nvmf_rdma_poll_group_remove() 에서 qpair 제거 시 이 channel 로
	 * spdk_put_io_channel() 을 통해 정리.
	 * 설정자: nvmf_rdma_poll_group_add() — spdk_get_io_channel() 호출 결과 저장.
	 * 읽는 자: nvmf_rdma_poll_group_remove() — 채널 해제 시.
	 * 값 범위: 유효한 io_channel (NULL 이면 미연결).
	 * 동기화: reactor 스레드 안에서만 접근 — 락 불필요. */

	/* ctx for async processing of last_wqe_reached event */
	struct spdk_nvmf_rdma_ibv_event_ctx	*last_wqe_reached_ctx;
	/* [한국어] IBV_EVENT_QP_LAST_WQE_REACHED 비동기 처리를 위한 컨텍스트 포인터.
	 * ibv async event 는 별도 스레드에서 발생할 수 있어 send_msg 로 reactor 에 전달.
	 * 설정자: nvmf_rdma_send_qpair_last_wqe_event() — ctx 할당 후 저장.
	 * 읽는 자: nvmf_rdma_qpair_process_last_wqe_event() — 처리 완료 후 free 및 NULL.
	 * 값 범위: NULL (미처리) 또는 heap-allocated ctx 포인터.
	 * 동기화: reactor send_msg 를 통해 단일 스레드 접근 보장. */

	/* Lets us know that we have received the last_wqe event. */
	bool					last_wqe_reached;
	/* [한국어] IBV_EVENT_QP_LAST_WQE_REACHED 이벤트 수신 완료 플래그.
	 * QP error → SRQ 모드에서 QP 의 마지막 WR 완료 시 발생하는 이벤트.
	 * 이 플래그가 true 가 되면 nvmf_rdma_destroy_drained_qpair() 가 QP 파괴 가능.
	 * 설정자: nvmf_rdma_handle_last_wqe_reached() — 이벤트 처리 완료 시 true.
	 * 읽는 자: nvmf_rdma_destroy_drained_qpair() — last_wqe 전까지 파괴 보류.
	 * 값 범위: true/false.
	 * 동기화: send_msg 를 통해 reactor 스레드에서만 설정 — 락 불필요. */

	/* Indicate that nvmf_rdma_close_qpair is called */
	bool					to_close;
	/* [한국어] nvmf_rdma_close_qpair() 가 호출됐음을 표시 (graceful shutdown 진행 중).
	 * true 면 새로운 I/O 를 거절하고 in-flight 완료 후 파괴 경로 진입.
	 * 설정자: nvmf_rdma_close_qpair() — vtable qpair_fini 콜백.
	 * 읽는 자: nvmf_rdma_destroy_drained_qpair() — drain 완료 확인;
	 *           nvmf_rdma_request_process() — to_close=true 면 NEW 처리 거절.
	 * 값 범위: true(종료 중)/false(정상).
	 * 동기화: reactor 스레드 접근 — 락 불필요. */

	/* Save the listen_trid by rqpair itself instead of get it from listen_id
	 * everytime, because the listen_id may be freed before the qpair is destroyed */
	struct spdk_nvme_transport_id		listen_trid;
	/* [한국어] 이 QP 가 연결된 listen port 의 TID (Transport ID) 캐시.
	 * listen_id 는 port 제거 시 QP 보다 먼저 해제될 수 있어 직접 캐싱.
	 * 설정자: nvmf_rdma_connect() — 연결 수락 시 listen_id 에서 복사.
	 * 읽는 자: nvmf_rdma_qpair_get_listen_trid() — vtable 콜백으로 RPC reporting.
	 * 값 범위: 유효한 spdk_nvme_transport_id (연결 수락 후 항상 유효).
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */
};

/*
 * [한국어] struct spdk_nvmf_rdma_poller_stat
 *
 * poller 단위 성능 통계. nvmf_rdma_poller_poll() 이 매 iteration 마다 증가.
 * RPC nvmf_get_stats 요청 시 nvmf_rdma_poll_group_dump_stat() 이 JSON 으로 직렬화.
 * 모든 카운터는 단조 증가(monotonically increasing) — 누적 값. 리셋 없음.
 */
struct spdk_nvmf_rdma_poller_stat {
	uint64_t				completions;
	/* [한국어] ibv_poll_cq 가 반환한 총 CQE 수 (RECV + SEND + DATA WR 합산).
	 * 설정자: nvmf_rdma_poller_poll() — ibv_poll_cq 반환값(count) 누적.
	 * 읽는 자: nvmf_rdma_poll_group_dump_stat() — JSON "completions" 필드.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	uint64_t				polls;
	/* [한국어] nvmf_rdma_poller_poll() 호출 횟수 (idle 포함 총 poll 수).
	 * 설정자: nvmf_rdma_poller_poll() — 매 호출마다 ++.
	 * 읽는 자: JSON "polls" 필드.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	uint64_t				idle_polls;
	/* [한국어] ibv_poll_cq 가 0 개를 반환한 빈 poll 횟수 (CPU 활용 분석).
	 * idle_polls / polls = idle ratio (이상적: 낮을수록 good).
	 * 설정자: nvmf_rdma_poller_poll() — count == 0 일 때 ++.
	 * 읽는 자: JSON "idle_polls" 필드.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	uint64_t				requests;
	/* [한국어] 처리 완료된 총 NVMe-oF request 수.
	 * 설정자: nvmf_rdma_poller_poll() — RECV CQE 처리 시 request 받을 때 ++.
	 * 읽는 자: JSON "requests" 필드.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	uint64_t				request_latency;
	/* [한국어] 완료된 request 의 capsule 수신 ~ SEND CQE 까지 TSC 합산 (latency 통계).
	 * request_latency / requests = 평균 request latency (TSC 단위).
	 * 설정자: nvmf_rdma_poller_poll() — SEND CQE 수신 시 (ticks - receive_tsc) 누적.
	 * 읽는 자: JSON "request_latency" 필드.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	uint64_t				pending_free_request;
	/* [한국어] free_queue 가 비어 있어 incoming_queue 에서 대기한 총 횟수.
	 * 설정자: nvmf_rdma_poller_poll() — RECV CQE 처리 시 free_queue empty면 ++.
	 * 읽는 자: JSON "pending_free_request" 필드.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	uint64_t				pending_rdma_read;
	/* [한국어] pending_rdma_read_queue 에 enqueue 된 총 횟수 (RDMA READ depth 포화).
	 * 설정자: nvmf_rdma_request_process() — DATA_XFER_TO_CTRL_PENDING enqueue 시 ++.
	 * 읽는 자: JSON "pending_rdma_read" 필드.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	uint64_t				pending_rdma_write;
	/* [한국어] pending_rdma_write_queue 에 enqueue 된 총 횟수 (RDMA WRITE depth 포화).
	 * 설정자: nvmf_rdma_request_process() — DATA_XFER_TO_HOST_PENDING enqueue 시 ++.
	 * 읽는 자: JSON "pending_rdma_write" 필드.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	uint64_t				pending_rdma_send;
	/* [한국어] pending_rdma_send_queue 에 enqueue 된 총 횟수 (SEND depth 포화).
	 * 설정자: nvmf_rdma_request_process() — READY_TO_COMPLETE_PENDING enqueue 시 ++.
	 * 읽는 자: JSON "pending_rdma_send" 필드.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	struct spdk_rdma_provider_qp_stats	qp_stats;
	/* [한국어] provider 레벨 QP 통계 (send/recv doorbell_updates, num_submitted_wrs).
	 * spdk_rdma_provider_qp_stats 는 {send, recv} × {num_submitted_wrs, doorbell_updates}.
	 * 설정자: spdk_rdma_provider_qp_submit_*() 내부에서 누적.
	 * 읽는 자: nvmf_rdma_poll_group_dump_stat() — total_send_wrs/send_doorbell_updates 등.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */
};

/*
 * [한국어] struct spdk_nvmf_rdma_poller
 *
 * 1 (device, poll_group) 페어당 1 poller. 같은 ibv_context (HCA) 의 모든 QP 가
 * 같은 poller 의 단일 CQ 를 공유 — ibv_poll_cq 한 번에 여러 QP 완료 수확.
 * device 가 N 개면 poll_group 안에 poller 가 N 개 (TAILQ).
 *
 * SRQ (Shared Receive Queue) 모드: max_srq_depth > 0 이면 모든 QP 가 한 SRQ 공유 →
 * RECV WR 풀 메모리 절약 (N * queue_depth → 단일 max_srq_depth). 미사용 시 QP 별 RECV.
 *
 * 핵심 필드:
 *  - cq: 공유 CQ. ibv_poll_cq 의 단위.
 *  - cq_intr/comp_channel: 인터럽트 모드용 (polled 보다 드물게 사용).
 *  - srq/resources: SRQ 모드 자원.
 *  - qpairs (RB tree): qp_num 으로 QP 검색 (CQE 의 qp_num 으로 역추적).
 *  - qpairs_pending_recv/send: 자원 부족으로 대기 중인 QP 리스트.
 *  - stat: completions/polls/idle_polls/pending_* 통계.
 */
struct spdk_nvmf_rdma_poller {
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poll_group	*group;

	int					num_cqe;
	/* [한국어] 현재 CQ 깊이. SRQ depth 변경 시 함께 조정. */
	int					required_num_wr;
	/* [한국어] 모든 active QP 의 WR 수요 합. cq resize 트리거. */
	struct ibv_cq				*cq;
	struct spdk_interrupt			*cq_intr;
	struct ibv_comp_channel			*comp_channel;

	/* The maximum number of I/O outstanding on the shared receive queue at one time */
	uint16_t				max_srq_depth;
	bool					need_destroy;
	/* [한국어] poller 가 곧 destroy 될 예정 — graceful drain 후 회수. */

	/* Shared receive queue */
	struct spdk_rdma_provider_srq		*srq;

	struct spdk_nvmf_rdma_resources		*resources;
	struct spdk_nvmf_rdma_poller_stat	stat;

	spdk_poller_destroy_cb			destroy_cb;
	void					*destroy_cb_ctx;

	RB_HEAD(qpairs_tree, spdk_nvmf_rdma_qpair) qpairs;
	/* [한국어] qp_num key, O(log n) 검색. CQE 처리 시 자주 사용. */

	TAILQ_HEAD(, spdk_nvmf_rdma_qpair)	active_qpairs;
	/* [한국어] 활성 QP 리스트 — iteration 용. */

	STAILQ_HEAD(, spdk_nvmf_rdma_qpair)	qpairs_pending_recv;
	/* [한국어] RECV 게시 대기 QP (SRQ 모드에서 SRQ 가득 찰 때). */

	STAILQ_HEAD(, spdk_nvmf_rdma_qpair)	qpairs_pending_send;
	/* [한국어] SEND batching 대기 QP (no_wr_batching=false 일 때 묶어서 post_send). */

	TAILQ_ENTRY(spdk_nvmf_rdma_poller)	link;
};

/*
 * [한국어] struct spdk_nvmf_rdma_poll_group_stat
 *
 * poll_group 단위 통계. 현재는 pending_data_buffer 하나만 가짐.
 * nvmf_rdma_poll_group_dump_stat() 에서 JSON "pending_data_buffer" 로 출력.
 */
struct spdk_nvmf_rdma_poll_group_stat {
	uint64_t				pending_data_buffer;
	/* [한국어] 버퍼 풀 대기 중인 request 의 총 데이터 크기 합산 (byte).
	 * NEED_BUFFER 상태에서 대기 중인 모든 request 의 payload 크기를 누적.
	 * 설정자: nvmf_rdma_poll_group_insert_need_buffer_req() — request 가 pending_buf_queue
	 *          에 enqueue 될 때 payload 크기 더함.
	 *          nvmf_rdma_request_process() — 버퍼 획득 후 HAVE_BUFFER 전이 시 감산.
	 * 읽는 자: nvmf_rdma_poll_group_dump_stat() — JSON "pending_data_buffer" 필드.
	 * 값 범위: 0 ~ (max_queue_depth * max_io_size).
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */
};

/*
 * [한국어] struct spdk_nvmf_rdma_poll_group
 *
 * 1 reactor 스레드에 1개 생성되는 NVMe-oF RDMA poll group.
 * 각 RDMA device 마다 poller 1개를 소유하며, 모든 poller 의 CQ 를
 * nvmf_rdma_poll_group_poll() 이 순회하여 이벤트 수확.
 * nvmf_rdma_transport 의 poll_groups TAILQ 에 연결되어 transport 레벨에서 관리.
 */
struct spdk_nvmf_rdma_poll_group {
	struct spdk_nvmf_transport_poll_group		group;
	/* [한국어] 공통 NVMe-oF transport poll_group (qpairs TAILQ, pending_buf_queue 포함).
	 * SPDK_CONTAINEROF 로 rgroup ↔ group 변환. vtable 콜백의 인자 타입.
	 * 설정자: nvmf_rdma_poll_group_create() — 초기화.
	 * 읽는 자: transport 공통 레이어 — add/remove qpair, buffer 관리.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	struct spdk_nvmf_rdma_poll_group_stat		stat;
	/* [한국어] poll_group 단위 통계 (pending_data_buffer).
	 * 설정자: nvmf_rdma_poll_group_insert_need_buffer_req() / nvmf_rdma_request_process().
	 * 읽는 자: nvmf_rdma_poll_group_dump_stat() — RPC JSON 출력.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	TAILQ_HEAD(, spdk_nvmf_rdma_poller)		pollers;
	/* [한국어] 이 poll_group 에 속한 poller 목록 (device 당 1개).
	 * 설정자: nvmf_rdma_poll_group_create() / _nvmf_rdma_register_poller_in_group() — 추가.
	 *          nvmf_rdma_poller_destroy() — 제거.
	 * 읽는 자: nvmf_rdma_poll_group_poll() — TAILQ_FOREACH_SAFE 로 순회.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	TAILQ_ENTRY(spdk_nvmf_rdma_poll_group)		link;
	/* [한국어] rtransport->poll_groups TAILQ 연결 노드.
	 * 설정자: nvmf_rdma_poll_group_create() — TAILQ_INSERT_TAIL.
	 *          nvmf_rdma_poll_group_destroy() — TAILQ_REMOVE.
	 * 읽는 자: nvmf_rdma_get_optimal_poll_group() — NUMA-aware QP routing.
	 * 동기화: poll_group 접근은 reactor 스레드, transport TAILQ 는 reactor 0에서 관리. */
};

/*
 * [한국어] struct spdk_nvmf_rdma_conn_sched
 *
 * 새 connection 이 어느 poll_group 에 배정될지 결정하는 round-robin 스케줄러 상태.
 * admin QP 와 IO QP 는 독립적인 round-robin 순서를 가짐 — 부하 분산.
 */
struct spdk_nvmf_rdma_conn_sched {
	struct spdk_nvmf_rdma_poll_group *next_admin_pg;
	/* [한국어] 다음 admin QP 연결이 배정될 poll_group 포인터 (round-robin 순서).
	 * 설정자: nvmf_rdma_get_optimal_poll_group() — 배정 후 다음 pg 로 순환.
	 * 읽는 자: nvmf_rdma_connect() — admin QP 생성 시 어느 reactor 에 배치할지 결정.
	 * 값 범위: transport->poll_groups TAILQ 의 유효한 포인터.
	 * 동기화: nvmf_rdma_connect() 는 accept_poller 스레드에서 실행 — 락 필요 여부 확인. */

	struct spdk_nvmf_rdma_poll_group *next_io_pg;
	/* [한국어] 다음 IO QP 연결이 배정될 poll_group 포인터 (round-robin 순서).
	 * 설정자: nvmf_rdma_get_optimal_poll_group() — IO QP 배정 후 다음 pg 로 순환.
	 * 읽는 자: nvmf_rdma_connect() — IO QP 생성 시 reactor 배치 결정.
	 * 값 범위: transport->poll_groups TAILQ 의 유효한 포인터.
	 * 동기화: admin_pg 와 동일 — accept_poller 단일 스레드 접근. */
};

/* Assuming rdma_cm uses just one protection domain per ibv_context. */
/*
 * [한국어] struct spdk_nvmf_rdma_device
 *
 * 1개 RDMA HCA(Host Channel Adapter)를 나타내는 device 객체.
 * ibv_context (HW 채널) + PD (Protection Domain) + mem_map 을 포함.
 * transport 당 devices TAILQ 에 등록되며, device 당 poller 가 1개씩 생성.
 */
struct spdk_nvmf_rdma_device {
	struct ibv_device_attr			attr;
	/* [한국어] HCA 하드웨어 속성 (max_qp, max_sge, max_cqe 등).
	 * 설정자: create_ib_device() — ibv_query_device() 결과 저장.
	 * 읽는 자: nvmf_rdma_qpair_initialize() — max_send_sge 클램프;
	 *           nvmf_rdma_resize_cq() — max_cqe 한도 확인.
	 * 값 범위: HCA 하드웨어 고유값 (변경 없음).
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */

	struct ibv_context			*context;
	/* [한국어] libfabric/libibverbs 의 HCA context (ibv_open_device 결과).
	 * 설정자: create_ib_device() — ibv_open_device().
	 * 읽는 자: nvmf_rdma_poller_poll() 의 ibv_get_device_name() — 통계 출력.
	 *           nvmf_rdma_poller_create() — ibv_create_cq() 인자.
	 * 값 범위: 유효한 ibv_context (device 존재하는 한 유효).
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */

	struct spdk_rdma_utils_mem_map		*map;
	/* [한국어] 이 device 의 메모리 등록 맵 (DMA 주소 변환 + lkey/rkey 조회).
	 * 설정자: create_ib_device() — spdk_rdma_utils_create_mem_map().
	 *          destroy_ib_device() — spdk_rdma_utils_free_mem_map().
	 * 읽는 자: nvmf_rdma_resources_create() — cmds/cpls/bufs 의 lkey 등록;
	 *           nvmf_rdma_fill_wr_sgl() — data buffer lkey 조회.
	 * 값 범위: 유효한 mem_map (device 생존 중 유효).
	 * 동기화: 초기화 후 여러 스레드 read-only 접근 — 락 불필요. */

	struct ibv_pd				*pd;
	/* [한국어] Protection Domain (PD) — MR(Memory Region) 등록과 QP 의 보안 도메인.
	 * 같은 PD 의 MR lkey/rkey 만 서로 참조 가능 (보안 격리).
	 * 설정자: create_ib_device() — ibv_alloc_pd().
	 *          destroy_ib_device() — ibv_dealloc_pd().
	 * 읽는 자: nvmf_rdma_qpair_initialize() — ibv QP 생성 시 pd 전달.
	 * 값 범위: 유효한 ibv_pd (device 생존 중 유효).
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */

	struct spdk_interrupt			*async_intr;
	/* [한국어] ibv async event fd 의 SPDK interrupt 핸들 (인터럽트 모드).
	 * polled 모드에서는 poll_fds 를 통해 직접 폴링, 인터럽트 모드에서는 이 핸들 사용.
	 * 설정자: create_ib_device() — spdk_interrupt_register().
	 *          destroy_ib_device() — spdk_interrupt_unregister().
	 * 읽는 자: async event handler — QP error, port change 등 처리.
	 * 값 범위: NULL (폴링 모드) 또는 유효한 spdk_interrupt 핸들.
	 * 동기화: reactor 스레드 접근 — 락 불필요. */

	int					num_srq;
	/* [한국어] 이 device 에 생성된 SRQ 수 (SRQ 모드 활성화 시).
	 * 설정자: nvmf_rdma_poller_create() — SRQ 생성 시 ++.
	 *          nvmf_rdma_poller_destroy() — SRQ 해제 시 --.
	 * 읽는 자: nvmf_rdma_check_devices_context() — device 상태 확인.
	 * 값 범위: 0 ~ poll_group 수.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	bool					need_destroy;
	/* [한국어] 이 device 가 제거(hot-remove)되어 파괴 예약된 상태.
	 * true 면 새 QP 생성 금지, poller drain 완료 후 destroy_ib_device() 호출 예약.
	 * 설정자: nvmf_rdma_handle_device_removal() — device removal event 수신 시.
	 * 읽는 자: nvmf_rdma_check_devices_context() — drain 완료 여부 확인.
	 * 값 범위: true/false.
	 * 동기화: reactor 스레드 접근 — 락 불필요. */

	bool					ready_to_destroy;
	/* [한국어] 모든 poller 가 drain 완료되어 device 즉시 파괴 가능 상태.
	 * 설정자: _nvmf_rdma_remove_poller_in_group_cb() — 마지막 poller destroy 시 true.
	 * 읽는 자: _nvmf_rdma_remove_destroyed_device() — true 인 device 제거.
	 * 값 범위: true/false.
	 * 동기화: spdk_thread_send_msg 로 reactor 스레드에서만 설정. */

	bool					is_ready;
	/* [한국어] device 가 완전히 초기화되어 QP/SRQ 생성 가능 상태.
	 * false 면 신규 connection 수락 보류 (port 추가 후 device scan 전 과도기).
	 * 설정자: create_ib_device() / nvmf_rdma_rescan_devices() — 초기화 완료 시 true.
	 * 읽는 자: nvmf_rdma_connect() — device 사용 가능 여부 확인.
	 * 값 범위: true/false.
	 * 동기화: 단일 reactor 스레드 접근 — 락 불필요. */

	TAILQ_ENTRY(spdk_nvmf_rdma_device)	link;
	/* [한국어] rtransport->devices TAILQ 연결 노드.
	 * 설정자: create_ib_device() — TAILQ_INSERT_TAIL.
	 *          destroy_ib_device() — TAILQ_REMOVE.
	 * 읽는 자: nvmf_rdma_find_ib_device() — 순회 검색.
	 * 동기화: transport 레벨 단일 스레드 접근 — 락 불필요. */
};

/*
 * [한국어] struct spdk_nvmf_rdma_port
 *
 * rdma_cm listen port 1개를 나타내는 객체. nvmf_rdma_listen() 이 rdma_bind_addr +
 * rdma_listen 완료 후 생성하여 transport->ports TAILQ 에 등록.
 */
struct spdk_nvmf_rdma_port {
	const struct spdk_nvme_transport_id	*trid;
	/* [한국어] 이 listen port 의 Transport ID (주소/포트/프로토콜 정보).
	 * 설정자: nvmf_rdma_listen() — 인자로 받은 trid 포인터 저장.
	 * 읽는 자: nvmf_rdma_stop_listen_ex() — 매칭할 trid 비교.
	 * 값 범위: 유효한 spdk_nvme_transport_id (subsystem 생존 중 유효).
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */

	struct rdma_cm_id			*id;
	/* [한국어] rdma_cm listen cm_id (rdma_bind_addr + rdma_listen 결과).
	 * 설정자: nvmf_rdma_listen() — rdma_create_id() 후 bind+listen.
	 *          nvmf_rdma_stop_listen_ex() — rdma_destroy_id() 로 해제.
	 * 읽는 자: nvmf_rdma_disconnect_qpairs_on_port() — port 제거 시 QP disconnect.
	 * 값 범위: 유효한 rdma_cm_id (listen 상태).
	 * 동기화: accept poller 스레드 접근 — 단일 스레드 전용. */

	struct spdk_nvmf_rdma_device		*device;
	/* [한국어] 이 port 가 바인딩된 RDMA device 역참조.
	 * 설정자: nvmf_rdma_listen() — port 생성 시 device 조회 후 설정.
	 * 읽는 자: nvmf_rdma_handle_cm_event_port_removal() — device removal 처리.
	 * 값 범위: 유효한 rdevice 포인터.
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */

	TAILQ_ENTRY(spdk_nvmf_rdma_port)	link;
	/* [한국어] rtransport->ports 또는 retry_ports TAILQ 연결 노드.
	 * 설정자: nvmf_rdma_listen() — ports 에 추가; nvmf_rdma_retry_listen_port() — retry_ports.
	 *          nvmf_rdma_stop_listen_ex() — 제거.
	 * 동기화: accept poller 스레드 단일 접근 — 락 불필요. */
};

/*
 * [한국어] struct rdma_transport_opts
 *
 * RDMA transport 특화 설정 옵션 (JSON/RPC 로 수신).
 * nvmf_rdma_create() 에서 spdk_nvmf_transport_create_opts 의 transport_specific 에서
 * rdma_transport_opts_decoder 를 통해 디코딩.
 */
struct rdma_transport_opts {
	int		num_cqe;
	/* [한국어] CQ(Completion Queue) 크기 (엔트리 수).
	 * 설정자: rdma_transport_opts_decoder JSON 파싱 또는 기본값 DEFAULT_NVMF_RDMA_CQ_SIZE.
	 * 읽는 자: nvmf_rdma_poller_create() — ibv_create_cq() 인자.
	 *           nvmf_rdma_resize_cq() — 동적 조정 상한.
	 * 값 범위: > 0; 너무 작으면 CQ overflow 에러 발생.
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */

	uint32_t	max_srq_depth;
	/* [한국어] SRQ (Shared Receive Queue) 깊이 (RECV WR 수).
	 * 설정자: JSON "max_srq_depth" 또는 기본값 SPDK_NVMF_RDMA_DEFAULT_SRQ_DEPTH.
	 * 읽는 자: nvmf_rdma_poller_create() — ibv_create_srq() 인자.
	 * 값 범위: > 0 (SRQ 활성 시); no_srq=true 이면 무시.
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */

	bool		no_srq;
	/* [한국어] SRQ 비활성화 플래그. true 면 각 QP 가 독립적 RECV 풀 사용.
	 * 메모리 가용량이 높은 환경이나 SRQ 지원 안 하는 HW 에서 사용.
	 * 설정자: JSON "no_srq" 또는 기본값 false (SRQ 활성).
	 * 읽는 자: nvmf_rdma_poller_create() — SRQ 생성 여부 분기.
	 *           nvmf_rdma_qpair_initialize() — per-QP RECV 풀 생성 분기.
	 * 값 범위: true/false.
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */

	bool		no_wr_batching;
	/* [한국어] WR batching 비활성화 플래그.
	 * false(기본): CQ poll 배치 종료 후 일괄 ibv_post_send — doorbell 횟수 최소화.
	 * true: 각 WR 즉시 post — 낮은 지연 우선.
	 * 설정자: JSON "no_wr_batching" 또는 기본값 false.
	 * 읽는 자: nvmf_rdma_request_process() — batching 여부로 qpairs_pending_send 분기.
	 *           _poller_submit_sends() — 일괄 post 트리거.
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */

	int		acceptor_backlog;
	/* [한국어] rdma_listen() 의 backlog 파라미터 (OS 레벨 연결 대기 큐 깊이).
	 * 설정자: JSON "acceptor_backlog" 또는 기본값 SPDK_NVMF_RDMA_ACCEPT_BACKLOG.
	 * 읽는 자: nvmf_rdma_listen() — rdma_listen(id, backlog) 인자.
	 * 값 범위: > 0; 너무 작으면 burst connection 시 연결 손실.
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */
};

/*
 * [한국어] struct spdk_nvmf_rdma_transport
 *
 * NVMe-oF RDMA transport 최상위 컨테이너. 1 transport 인스턴스에 1개 생성.
 * nvmf_rdma_create() 가 할당하고 nvmf_rdma_destroy() 가 해제.
 * 모든 RDMA device, port, poll_group 목록을 소유.
 */
struct spdk_nvmf_rdma_transport {
	struct spdk_nvmf_transport	transport;
	/* [한국어] 공통 NVMe-oF transport (opts, subsystem list 등). SPDK_CONTAINEROF 로 변환.
	 * 설정자: nvmf_rdma_create() — spdk_nvmf_transport_create() 통해 초기화.
	 * 읽는 자: 공통 transport 레이어 — vtable 콜백의 첫 인자.
	 * 동기화: transport 레벨 단일 스레드 접근. */

	struct rdma_transport_opts	rdma_opts;
	/* [한국어] RDMA 특화 설정 (num_cqe, max_srq_depth, no_srq 등).
	 * 설정자: nvmf_rdma_create() — JSON opts 디코딩 후 저장.
	 * 읽는 자: nvmf_rdma_poller_create() / nvmf_rdma_qpair_initialize() — 각종 크기 결정.
	 * 동기화: 초기화 후 읽기 전용 — 락 불필요. */

	struct spdk_nvmf_rdma_conn_sched conn_sched;
	/* [한국어] 신규 connection 의 round-robin poll_group 배정 스케줄러.
	 * 설정자: nvmf_rdma_get_optimal_poll_group() — 배정 후 next_pg 순환.
	 * 읽는 자: nvmf_rdma_connect() — 새 QP 의 reactor 선택.
	 * 동기화: accept_poller 스레드 단일 접근 — 락 불필요. */

	struct rdma_event_channel	*event_channel;
	/* [한국어] rdma_cm 이벤트 채널 (rdma_create_event_channel 결과).
	 * 모든 CM 이벤트 (CONNECT_REQUEST, DISCONNECT 등) 가 이 채널로 전달.
	 * 설정자: nvmf_rdma_create() — rdma_create_event_channel().
	 *          nvmf_rdma_destroy() — rdma_destroy_event_channel().
	 * 읽는 자: nvmf_process_cm_events() — rdma_get_cm_event() 로 이벤트 수확.
	 *           accept_poller / poll_fds 를 통해 폴링.
	 * 동기화: accept_poller 스레드 단일 접근 — 락 불필요. */

	struct spdk_mempool		*data_wr_pool;
	/* [한국어] multi-SGL split 시 추가 data_wr 객체를 빌려오는 mempool.
	 * spdk_nvmf_rdma_request_data 크기의 pool — 기본 embedded data.wr 가 SGE 초과 시 사용.
	 * 설정자: nvmf_rdma_create() — spdk_mempool_create().
	 *          nvmf_rdma_destroy() — spdk_mempool_free().
	 * 읽는 자: nvmf_request_alloc_wrs() — pool 에서 할당;
	 *           _nvmf_rdma_request_free_data() — pool 에 반환.
	 * 동기화: spdk_mempool 자체가 thread-safe (lockless ring 기반). */

	struct spdk_poller		*accept_poller;
	/* [한국어] CM 이벤트 및 IB async 이벤트를 폴링하는 SPDK poller.
	 * 주기: 매 reactor iteration 마다 poll_fds 를 pollfd 로 확인.
	 * 설정자: nvmf_rdma_create() — SPDK_POLLER_REGISTER(nvmf_rdma_accept, ...).
	 *          nvmf_rdma_destroy() — spdk_poller_unregister().
	 * 읽는 자: nvmf_rdma_accept() — poll_fds 폴링 + cm_event/ib_event 처리.
	 * 동기화: SPDK reactor 단일 스레드 실행 — 락 불필요. */

	/* fields used to poll RDMA/IB events */
	nfds_t			npoll_fds;
	/* [한국어] poll_fds 배열의 유효 엔트리 수.
	 * 설정자: generate_poll_fds() — device+event_channel fd 수 계산;
	 *          free_poll_fds() — 0으로 초기화.
	 * 읽는 자: nvmf_rdma_accept() — poll(poll_fds, npoll_fds, 0) 인자.
	 * 값 범위: 0 ~ (device 수 + 1) [event_channel 포함].
	 * 동기화: accept_poller 스레드 단일 접근 — 락 불필요. */

	struct pollfd		*poll_fds;
	/* [한국어] POSIX poll() 인자용 fd 배열 (event_channel fd + device async event fd).
	 * 설정자: generate_poll_fds() — 각 device 의 ibv async event fd + event_channel fd 추가.
	 *          free_poll_fds() — free().
	 * 읽는 자: nvmf_rdma_accept() — poll(poll_fds, npoll_fds, 0) 으로 이벤트 감지.
	 * 값 범위: 유효한 pollfd 배열 (npoll_fds 개).
	 * 동기화: accept_poller 스레드 단일 접근 — 락 불필요. */

	struct spdk_interrupt   *cm_event_intr;
	/* [한국어] event_channel fd 의 SPDK interrupt 핸들 (인터럽트 모드 CM 이벤트 처리).
	 * polled 모드에서는 accept_poller 가 직접 poll(), 인터럽트 모드에서는 이 핸들로 처리.
	 * 설정자: nvmf_rdma_create() — spdk_interrupt_register().
	 *          nvmf_rdma_destroy() — spdk_interrupt_unregister().
	 * 값 범위: NULL (폴링 모드) 또는 유효한 spdk_interrupt 핸들.
	 * 동기화: reactor 스레드 접근 — 락 불필요. */

	TAILQ_HEAD(, spdk_nvmf_rdma_device)	devices;
	/* [한국어] 발견된 모든 RDMA device 목록.
	 * 설정자: create_ib_device() — TAILQ_INSERT_TAIL;
	 *          destroy_ib_device() — TAILQ_REMOVE.
	 * 읽는 자: nvmf_rdma_listen() — port 바인딩 시 device 조회;
	 *           nvmf_rdma_poll_group_create() — device 당 poller 생성.
	 * 동기화: transport 레벨 단일 스레드 접근 — 락 불필요. */

	TAILQ_HEAD(, spdk_nvmf_rdma_port)	ports;
	/* [한국어] 현재 listen 중인 port 목록.
	 * 설정자: nvmf_rdma_listen() — TAILQ_INSERT_TAIL;
	 *          nvmf_rdma_stop_listen_ex() — TAILQ_REMOVE.
	 * 읽는 자: nvmf_rdma_disconnect_qpairs_on_port() — port 제거 시 QP disconnect.
	 * 동기화: accept_poller 스레드 단일 접근 — 락 불필요. */

	TAILQ_HEAD(, spdk_nvmf_rdma_poll_group)	poll_groups;
	/* [한국어] 모든 poll_group 목록 (reactor 당 1개).
	 * 설정자: nvmf_rdma_poll_group_create() — TAILQ_INSERT_TAIL;
	 *          nvmf_rdma_poll_group_destroy() — TAILQ_REMOVE.
	 * 읽는 자: nvmf_rdma_get_optimal_poll_group() — round-robin 선택;
	 *           nvmf_rdma_listen() — poll_group 당 poller 생성.
	 * 동기화: poll_group 생성/삭제는 transport 스레드, 개별 pg 접근은 reactor 스레드. */

	/* ports that are removed unexpectedly and need retry listen */
	TAILQ_HEAD(, spdk_nvmf_rdma_port)		retry_ports;
	/* [한국어] 예기치 않게 제거된(hot-remove) port 의 재listen 대기 목록.
	 * device removal 후 device 가 다시 나타나면 이 port 들을 재시도.
	 * 설정자: nvmf_rdma_handle_cm_event_port_removal() — TAILQ_INSERT_TAIL.
	 *          nvmf_rdma_retry_listen_port() — 재시도 완료 후 TAILQ_REMOVE.
	 * 읽는 자: nvmf_rdma_retry_listen_port() — accept_poller 에서 주기적 재시도.
	 * 동기화: accept_poller 스레드 단일 접근 — 락 불필요. */
};

/*
 * [한국어] struct poller_manage_ctx
 *
 * device removal 또는 poller 관리 작업의 비동기 컨텍스트.
 * spdk_thread_send_msg() 로 특정 reactor 스레드에 작업을 전달할 때 사용.
 * heap 할당 후 콜백 완료 시 free.
 */
struct poller_manage_ctx {
	struct spdk_nvmf_rdma_transport		*rtransport;
	/* [한국어] 상위 transport 역참조 — 최종 정리 단계에서 device 목록 접근.
	 * 설정자: nvmf_rdma_manage_poller() — 컨텍스트 생성 시.
	 * 읽는 자: _nvmf_rdma_remove_destroyed_device() — device destroy 시.
	 * 값 범위: 유효한 rtransport 포인터 (생존 기간 보장).
	 * 동기화: send_msg 로 단일 스레드 접근 — 락 불필요. */

	struct spdk_nvmf_rdma_poll_group	*rgroup;
	/* [한국어] 작업 대상 poll_group 역참조.
	 * 설정자: nvmf_rdma_manage_poller() — 컨텍스트 생성 시.
	 * 읽는 자: _nvmf_rdma_remove_poller_in_group() — 해당 group 의 poller 제거.
	 * 값 범위: 유효한 rgroup 포인터.
	 * 동기화: 해당 rgroup 의 reactor 스레드에서만 실행. */

	struct spdk_nvmf_rdma_poller		*rpoller;
	/* [한국어] 작업 대상 poller 역참조.
	 * 설정자: nvmf_rdma_manage_poller() — 컨텍스트 생성 시.
	 * 읽는 자: _nvmf_rdma_remove_poller_in_group() — poller need_destroy 마킹.
	 * 값 범위: 유효한 rpoller 포인터 (이미 제거된 경우 TAILQ 순회로 확인).
	 * 동기화: 해당 reactor 스레드에서만 실행. */

	struct spdk_nvmf_rdma_device		*device;
	/* [한국어] 제거 중인 RDMA device 역참조.
	 * 설정자: nvmf_rdma_manage_poller() — device removal 시.
	 * 읽는 자: _nvmf_rdma_remove_poller_in_group_cb() — 마지막 poller destroy 완료 시
	 *           device->ready_to_destroy = true 설정.
	 * 값 범위: 유효한 rdevice 포인터 (device 파괴 전까지 유효).
	 * 동기화: 비동기 콜백 체인 — 단일 스레드 실행 보장. */

	struct spdk_thread			*thread;
	/* [한국어] 최종 device cleanup 을 처리할 스레드 (보통 transport 의 primary 스레드).
	 * _nvmf_rdma_remove_poller_in_group_cb() 가 이 스레드로 send_msg.
	 * 설정자: nvmf_rdma_manage_poller() — spdk_get_thread() 로 현재 스레드.
	 * 읽는 자: _nvmf_rdma_remove_poller_in_group_cb() — device 정리 msg 전송.
	 * 동기화: 스레드 포인터 자체는 불변 — 락 불필요. */

	volatile int				*inflight_op_counter;
	/* [한국어] 아직 완료되지 않은 비동기 관리 작업 수 카운터 (원자 감소용).
	 * 모든 poll_group 에 send_msg 후 카운터가 0 이 되면 device drain 완료.
	 * 설정자: nvmf_rdma_manage_poller() — 작업 수 만큼 초기화.
	 * 읽는 자: nvmf_rdma_all_pollers_management_done() — 0 여부 확인.
	 * 값 범위: 0 ~ poll_group 수.
	 * 동기화: volatile — 여러 reactor 스레드에서 감소 가능, atomic 연산 필요. */
};

/* [한국어] rdma_transport_opts_decoder: RDMA transport 특화 JSON 옵션 디코더 테이블.
 * nvmf_rdma_create() 에서 spdk_json_decode_object() 에 전달 — JSON RPC 의
 * transport_specific 필드를 rdma_transport_opts 구조체로 파싱.
 * 각 항목: { JSON 필드명, 구조체 필드 offset, 디코더 함수, 선택적(true) }.
 * true = 해당 필드가 없어도 에러 아님 (기본값 사용). */
static const struct spdk_json_object_decoder rdma_transport_opts_decoder[] = {
	{
		"num_cqe", offsetof(struct rdma_transport_opts, num_cqe),
		spdk_json_decode_int32, true  /* [한국어] CQ 크기 — 기본값 DEFAULT_NVMF_RDMA_CQ_SIZE */
	},
	{
		"max_srq_depth", offsetof(struct rdma_transport_opts, max_srq_depth),
		spdk_json_decode_uint32, true  /* [한국어] SRQ 깊이 — 기본값 SPDK_NVMF_RDMA_DEFAULT_SRQ_DEPTH */
	},
	{
		"no_srq", offsetof(struct rdma_transport_opts, no_srq),
		spdk_json_decode_bool, true  /* [한국어] SRQ 비활성화 — 기본값 false (SRQ 활성) */
	},
	{
		"no_wr_batching", offsetof(struct rdma_transport_opts, no_wr_batching),
		spdk_json_decode_bool, true  /* [한국어] WR batching 비활성화 — 기본값 false (batching 활성) */
	},
	{
		"acceptor_backlog", offsetof(struct rdma_transport_opts, acceptor_backlog),
		spdk_json_decode_int32, true  /* [한국어] rdma_listen backlog — 기본값 SPDK_NVMF_RDMA_ACCEPT_BACKLOG */
	},
};

/*
 * [한국어]
 * nvmf_rdma_qpair_compare - RB tree 비교 함수 (qp_num 오름차순).
 *
 * @rqpair1: 비교 대상 QP 1.
 * @rqpair2: 비교 대상 QP 2.
 * @return: rqpair1.qp_num < rqpair2.qp_num 이면 -1,
 *          rqpair1.qp_num > rqpair2.qp_num 이면 1,
 *          같으면 0.
 *
 * RB_GENERATE_STATIC 매크로가 생성하는 qpairs_tree 의 비교 함수.
 * poller->qpairs RB tree 를 qp_num 으로 키잉 — CQE 의 qp_num 으로 O(log n) QP 검색.
 *
 * 호출 체인:
 *   RB_INSERT/RB_FIND/RB_REMOVE → [nvmf_rdma_qpair_compare]
 */
static int
nvmf_rdma_qpair_compare(struct spdk_nvmf_rdma_qpair *rqpair1, struct spdk_nvmf_rdma_qpair *rqpair2)
{
	/* [한국어] 삼항 연산으로 < 0, 0, > 0 반환 — RB tree 정렬 기준 (qp_num 오름차순). */
	return rqpair1->qp_num < rqpair2->qp_num ? -1 : rqpair1->qp_num > rqpair2->qp_num;
}

/* [한국어] qpairs_tree RB tree 구현 매크로 — qp_num 키, nvmf_rdma_qpair_compare 비교.
 * STATIC: 이 파일 내부에서만 사용 (외부 링크 없음).
 * 생성 함수: RB_INSERT, RB_REMOVE, RB_FIND, RB_NFIND, RB_MIN 등. */
RB_GENERATE_STATIC(qpairs_tree, spdk_nvmf_rdma_qpair, node, nvmf_rdma_qpair_compare);

/* [한국어] 전방 선언 — request 상태 기계 핵심 함수 (아래에서 정의).
 * 여러 함수에서 호출하므로 파일 상단에 선언. */
static bool nvmf_rdma_request_process(struct spdk_nvmf_rdma_transport *rtransport,
				      struct spdk_nvmf_rdma_request *rdma_req);

/* [한국어] 전방 선언 — batching 모드에서 CQ poll 배치 종료 시 일괄 post_send. */
static void _poller_submit_sends(struct spdk_nvmf_rdma_transport *rtransport,
				 struct spdk_nvmf_rdma_poller *rpoller);

/* [한국어] 전방 선언 — RECV WR 재게시 일괄 처리 (pending_recv queue 처리). */
static void _poller_submit_recvs(struct spdk_nvmf_rdma_transport *rtransport,
				 struct spdk_nvmf_rdma_poller *rpoller);

/* [한국어] 전방 선언 — device removal 완료 후 ready_to_destroy device 일괄 제거. */
static void _nvmf_rdma_remove_destroyed_device(void *c);

/* [한국어] 전방 선언 — request 완료 후 FREE 상태로 반환 (vtable req_free 콜백). */
static void nvmf_rdma_request_free(struct spdk_nvmf_request *req);

/* [한국어] 전방 선언 — poller 단위 CQ 폴링 핫패스 (매 reactor iteration 에서 호출). */
static int nvmf_rdma_poller_poll(struct spdk_nvmf_rdma_transport *rtransport,
				 struct spdk_nvmf_rdma_poller *rpoller);

/*
 * [한국어]
 * nvmf_rdma_dif_error_to_compl_status - SPDK DIF 에러 코드를 NVMe 미디어 에러 상태 코드로 변환.
 *
 * @err_type: SPDK DIF 에러 타입 (SPDK_DIF_REFTAG_ERROR / APPTAG_ERROR / GUARD_ERROR).
 * @return: 대응하는 enum spdk_nvme_media_error_status_code.
 *
 * T10 PI (Protection Information, DIF) 검증 실패 시 NVMe completion 에 적절한
 * status code 를 설정하기 위해 사용. DIF = Data Integrity Field, T10 PI 스펙의
 * guard/apptag/reftag 세 가지 검증을 각각 NVMe 스펙의 미디어 에러로 매핑.
 *
 * 실행 컨텍스트: nvmf_rdma_fill_wr_sgl_with_dif() 의 에러 경로에서 호출.
 *
 * 호출 체인:
 *   nvmf_rdma_fill_wr_sgl_with_dif() → [nvmf_rdma_dif_error_to_compl_status]
 */
static inline enum spdk_nvme_media_error_status_code
nvmf_rdma_dif_error_to_compl_status(uint8_t err_type) {
	enum spdk_nvme_media_error_status_code result;  /* [한국어] 반환할 NVMe 미디어 에러 코드. */
	switch (err_type)
	{
	case SPDK_DIF_REFTAG_ERROR:
		/* [한국어] Reference Tag 불일치 — NVMe SC_REFERENCE_TAG_CHECK_ERROR (0x284). */
		result = SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_APPTAG_ERROR:
		/* [한국어] Application Tag 불일치 — NVMe SC_APPLICATION_TAG_CHECK_ERROR (0x285). */
		result = SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_GUARD_ERROR:
		/* [한국어] Guard CRC 불일치 — NVMe SC_GUARD_CHECK_ERROR (0x282). */
		result = SPDK_NVME_SC_GUARD_CHECK_ERROR;
		break;
	default:
		/* [한국어] 알 수 없는 DIF 에러 — 코드 도달 불가 (UNREACHABLE assert). */
		SPDK_UNREACHABLE();
	}

	return result;  /* [한국어] 매핑된 NVMe 미디어 에러 코드 반환. */
}

/*
 * Return data_wrs to pool starting from \b data_wr
 * Request's own response and data WR are excluded
 */
/*
 * [한국어]
 * _nvmf_rdma_request_free_data - data_wr 체인을 순회하며 mempool 에 반환 (내부 헬퍼).
 *
 * @rdma_req: 이 WR chain 을 소유한 request — data.wr 가 체인의 첫 embedded WR.
 * @data_wr: 반환 시작 WR 포인터 (rdma_req->transfer_wr 또는 remaining_tranfer_in_wrs).
 * @pool: WR 객체를 반환할 rtransport->data_wr_pool.
 * @return: void.
 *
 * multi-SGL split 시 mempool 에서 빌려온 추가 data_wr 객체들을 순회하여 일괄 반환.
 * rdma_req->data.wr (embedded) 는 mempool 소유가 아니므로 반환 대상에서 제외 — 단,
 * SGL 은 초기화(memset 0).
 *
 * 체인 순회 종료 조건: wr_id 가 req_wrid 와 다르거나, next 가 NULL 이거나 rsp.wr 인 경우.
 *
 * 실행 컨텍스트: nvmf_rdma_request_free_data() 에서 호출 — 단일 reactor 스레드.
 *
 * 호출 체인:
 *   nvmf_rdma_request_free_data() → [_nvmf_rdma_request_free_data]
 *     → spdk_mempool_put_bulk
 */
static void
_nvmf_rdma_request_free_data(struct spdk_nvmf_rdma_request *rdma_req,
			     struct ibv_send_wr *data_wr,
			     struct spdk_mempool *pool)
{
	/* [한국어] 일괄 반환할 WR 포인터 임시 배열 (최대 SPDK_NVMF_MAX_SGL_ENTRIES 개). */
	struct spdk_nvmf_rdma_request_data	*work_requests[SPDK_NVMF_MAX_SGL_ENTRIES];
	struct spdk_nvmf_rdma_request_data	*nvmf_data;  /* [한국어] 현재 WR 의 컨테이너 포인터. */
	struct ibv_send_wr			*next_send_wr;  /* [한국어] 체인 다음 WR (순회용 임시 저장). */
	/* [한국어] req_wrid: embedded data.wr 의 wr_id — mempool 소유 WR 식별 기준.
	 * wr_id == req_wrid 인 WR 만 처리 대상 (다른 request WR 혼입 방지). */
	uint64_t				req_wrid = (uint64_t)&rdma_req->data_wr;
	uint32_t				num_wrs = 0;  /* [한국어] 반환 대상 WR 수. */

	/* [한국어] data_wr 체인을 순회하며 이 request 소유의 WR 만 처리. */
	while (data_wr && data_wr->wr_id == req_wrid) {
		/* [한국어] wr → container struct 복원 (SPDK_CONTAINEROF = offsetof 역산). */
		nvmf_data = SPDK_CONTAINEROF(data_wr, struct spdk_nvmf_rdma_request_data, wr);
		/* [한국어] SGE 배열 초기화 — mempool 반환 전 stale 주소/lkey 제거. */
		memset(nvmf_data->sgl, 0, sizeof(data_wr->sg_list[0]) * data_wr->num_sge);
		data_wr->num_sge = 0;  /* [한국어] SGE 수 초기화. */
		next_send_wr = data_wr->next;  /* [한국어] 다음 WR 를 미리 저장 (next 를 NULL 로 끊기 전). */
		/* [한국어] embedded WR (data.wr) 는 mempool 소유 아님 — 반환 배열에서 제외.
		 * 나머지 WR (mempool 할당) 는 next = NULL 로 체인 분리 후 배열에 추가. */
		if (data_wr != &rdma_req->data.wr) {
			data_wr->next = NULL;  /* [한국어] 체인 분리 — dangling next 방지. */
			assert(num_wrs < SPDK_NVMF_MAX_SGL_ENTRIES);  /* [한국어] 배열 overflow 방지. */
			work_requests[num_wrs] = nvmf_data;  /* [한국어] 반환 배열에 추가. */
			num_wrs++;
		}
		/* [한국어] 다음 WR 로 이동 — next 가 NULL 이거나 rsp.wr 이면 데이터 체인 끝. */
		data_wr = (!next_send_wr || next_send_wr == &rdma_req->rsp.wr) ? NULL : next_send_wr;
	}

	/* [한국어] 수집한 mempool WR 들을 일괄 반환 — 반환 없으면 스킵. */
	if (num_wrs) {
		spdk_mempool_put_bulk(pool, (void **) work_requests, num_wrs);
	}
}

/*
 * [한국어]
 * nvmf_rdma_request_free_data - request 의 데이터 WR 체인 전체를 해제하는 래퍼.
 *
 * @rdma_req: 데이터 WR 를 해제할 request.
 * @rtransport: data_wr_pool 에 접근하기 위한 transport.
 * @return: void.
 *
 * transfer_wr 체인과 remaining_tranfer_in_wrs 체인 두 가지를 모두 해제.
 * num_outstanding_data_wr 을 먼저 0으로 초기화 후 실제 WR 반환.
 * 마지막으로 embedded WR(data.wr, rsp.wr) 의 next 포인터를 NULL 로 초기화
 * — 재사용 시 stale next 포인터로 인한 WR chain 오염 방지.
 *
 * 실행 컨텍스트: nvmf_rdma_request_process() / _nvmf_rdma_request_free() — reactor 스레드.
 *
 * 호출 체인:
 *   _nvmf_rdma_request_free() → [nvmf_rdma_request_free_data]
 *     → _nvmf_rdma_request_free_data → spdk_mempool_put_bulk
 */
static void
nvmf_rdma_request_free_data(struct spdk_nvmf_rdma_request *rdma_req,
			    struct spdk_nvmf_rdma_transport *rtransport)
{
	rdma_req->num_outstanding_data_wr = 0;  /* [한국어] in-flight data WR 수 먼저 초기화. */

	/* [한국어] transfer_wr 체인 (발행된 WR 들) 을 mempool 에 반환. */
	_nvmf_rdma_request_free_data(rdma_req, rdma_req->transfer_wr, rtransport->data_wr_pool);

	/* [한국어] remaining 체인 (아직 발행 안 된 split WR 들) 반환 — split 요청에만 존재. */
	if (rdma_req->remaining_tranfer_in_wrs) {
		_nvmf_rdma_request_free_data(rdma_req, rdma_req->remaining_tranfer_in_wrs,
					     rtransport->data_wr_pool);
		rdma_req->remaining_tranfer_in_wrs = NULL;  /* [한국어] dangling 포인터 방지. */
	}

	/* [한국어] embedded WR 의 next 포인터 초기화 — 재사용 시 WR chain 오염 방지. */
	rdma_req->data.wr.next = NULL;
	rdma_req->rsp.wr.next = NULL;
}

/*
 * [한국어]
 * nvmf_rdma_dump_request - 단일 request 의 디버그 상태를 에러 로그로 출력.
 *
 * @req: 덤프할 request.
 * @return: void.
 *
 * QP error 또는 타임아웃 시 nvmf_rdma_dump_qpair_contents() 에서 호출.
 * data_from_pool, opcode, recv wr_id 출력 — stuck request 원인 분석용.
 *
 * 실행 컨텍스트: 에러 경로 — reactor 스레드.
 *
 * 호출 체인:
 *   nvmf_rdma_dump_qpair_contents() → [nvmf_rdma_dump_request]
 */
static void
nvmf_rdma_dump_request(struct spdk_nvmf_rdma_request *req)
{
	/* [한국어] data_from_pool: 데이터 버퍼가 큰 풀에서 왔는지 여부 출력. */
	SPDK_ERRLOG("\t\tRequest Data From Pool: %d\n", req->req.data_from_pool);
	/* [한국어] cmd 유효 시 NVMe opcode 출력 — 어떤 명령이 걸렸는지 식별. */
	if (req->req.cmd) {
		SPDK_ERRLOG("\t\tRequest opcode: %d\n", req->req.cmd->nvmf_cmd.opcode);
	}
	/* [한국어] recv 유효 시 wr_id 출력 — 어느 RECV 슬롯과 연결됐는지 추적. */
	if (req->recv) {
		SPDK_ERRLOG("\t\tRequest recv wr_id%lu\n", req->recv->wr.wr_id);
	}
}

/*
 * [한국어]
 * nvmf_rdma_dump_qpair_contents - QP 의 모든 non-FREE request 상태 덤프.
 *
 * @rqpair: 덤프할 QP.
 * @return: void.
 *
 * QP error / 연결 실패 시 진단용. FREE 가 아닌 request (in-flight 중인 것) 만 출력.
 *
 * 실행 컨텍스트: 에러 경로 — reactor 스레드.
 *
 * 호출 체인:
 *   nvmf_rdma_destroy_drained_qpair() 에러 경로 → [nvmf_rdma_dump_qpair_contents]
 *     → nvmf_rdma_dump_request
 */
static void
nvmf_rdma_dump_qpair_contents(struct spdk_nvmf_rdma_qpair *rqpair)
{
	int i;  /* [한국어] request 배열 순회 인덱스. */

	/* [한국어] QP ID 와 함께 덤프 시작 메시지 출력. */
	SPDK_ERRLOG("Dumping contents of queue pair (QID %d)\n", rqpair->qpair.qid);
	/* [한국어] reqs 배열 전체 순회 — FREE 가 아닌 것만 상세 덤프. */
	for (i = 0; i < rqpair->max_queue_depth; i++) {
		if (rqpair->resources->reqs[i].state != RDMA_REQUEST_STATE_FREE) {
			nvmf_rdma_dump_request(&rqpair->resources->reqs[i]);
		}
	}
}

/*
 * [한국어]
 * nvmf_rdma_resources_destroy - resources 구조체와 모든 DMA 버퍼 해제.
 *
 * @resources: 해제할 resources 객체 (nvmf_rdma_resources_create 로 생성된 것).
 * @return: void.
 *
 * spdk_free (hugepage DMA 메모리) + 일반 free (구조체 자체) 순서로 해제.
 * nvmf_rdma_qpair_destroy() 또는 nvmf_rdma_poller_destroy() 에서 호출.
 *
 * 실행 컨텍스트: reactor 스레드 (QP/poller 파괴 경로).
 *
 * 호출 체인:
 *   nvmf_rdma_qpair_destroy() / nvmf_rdma_poller_destroy() → [nvmf_rdma_resources_destroy]
 */
static void
nvmf_rdma_resources_destroy(struct spdk_nvmf_rdma_resources *resources)
{
	spdk_free(resources->cmds);   /* [한국어] 캡슐 수신 버퍼 (DMA hugepage) 해제. */
	spdk_free(resources->cpls);   /* [한국어] 완료 응답 버퍼 (DMA hugepage) 해제. */
	spdk_free(resources->bufs);   /* [한국어] in-capsule 데이터 버퍼 (DMA hugepage) 해제. */
	spdk_free(resources->reqs);   /* [한국어] request 객체 배열 (DMA hugepage) 해제. */
	spdk_free(resources->recvs);  /* [한국어] recv 객체 배열 (DMA hugepage) 해제. */
	free(resources);              /* [한국어] resources 구조체 자체 (calloc) 해제. */
}


/*
 * [한국어]
 * nvmf_rdma_resources_create - QP 또는 SRQ 의 request/recv/cmd/cpl/buf 풀 일괄 할당 + 초기화.
 *
 * @opts: 입력 — qpair (또는 SRQ), qp 포인터, mem map, max_queue_depth, in_capsule_data_size, shared.
 * @return: 할당된 resources 객체, 실패 시 NULL.
 *
 * 한 QP (non-SRQ) 또는 한 SRQ (모든 공유 QP) 의 lifetime 자원을 일괄 셋업.
 * 핵심 자원 5종 (모두 SPDK_MALLOC_DMA = hugepage):
 *   - reqs[N]:  spdk_nvmf_rdma_request 풀 (free_queue 로 관리).
 *   - recvs[N]: spdk_nvmf_rdma_recv 풀 (RECV WR 메타데이터).
 *   - cmds[N]:  64B NVMe capsule 버퍼 (RECV WR 의 sgl[0] 가 가리킴).
 *   - cpls[N]:  16B NVMe completion 버퍼 (SEND WR sgl[0]).
 *   - bufs[N * in_capsule_data_size]: in-capsule 데이터용 (small write 즉시 전송).
 *
 * 단계:
 *   1. resources 구조체 calloc.
 *   2. 5개 풀 spdk_zmalloc (4KB align, DMA).
 *   3. incoming_queue / free_queue STAILQ 초기화.
 *   4. opts->shared 면 SRQ, 아니면 per-QP QP 로 분기.
 *   5. 각 recv 슬롯에 대해:
 *      - sgl[0] = cmds[i] (capsule), sgl[1] = bufs[i] (in-capsule data, 옵션).
 *      - lkey 등록 (MR 의 local key).
 *      - wr_id = &rdma_wr (CQE 에서 type tag 회수용).
 *      - SRQ 또는 QP 의 RECV WR queue 에 enqueue.
 *   6. 각 req 슬롯에 대해:
 *      - rsp = cpls[i] 연결 (응답 쓸 위치).
 *      - SEND WR / RDMA WR 초기화 (sgl 포인터, opcode 등).
 *      - state = FREE, free_queue 등록.
 *   7. (loop 끝) RECV WR 일괄 flush — ibv_post_recv 한 번 호출로 N 개 RECV 게시.
 *
 * 미리 N 개 RECV 게시 패턴: 첫 capsule 도착 전에 max_queue_depth 만큼 RECV WR 가
 * NIC 큐에 깔려 있어야 함 — 안 그러면 도착한 RDMA SEND 가 RNR (Receiver Not Ready) 로
 * NAK 됨 → connection drop. NVMe-oF 가 host 측에서 inflight 제한을 지키지만 안전 마진 필수.
 *
 * 호출 체인:
 *   nvmf_rdma_qpair_create (QP 모드) / nvmf_rdma_poller_create (SRQ 모드)
 *     → [본 함수]
 *     → spdk_zmalloc x5 (hugepage 풀)
 *     → ibv_reg_mr (memory map 등록, opts->map 활용)
 *     → flush_recv_wrs (ibv_post_recv x1)
 */
static struct spdk_nvmf_rdma_resources *
nvmf_rdma_resources_create(struct spdk_nvmf_rdma_resource_opts *opts)
{
	struct spdk_nvmf_rdma_resources		*resources;
	struct spdk_nvmf_rdma_request		*rdma_req;
	struct spdk_nvmf_rdma_recv		*rdma_recv;
	struct spdk_rdma_provider_qp		*qp = NULL;
	struct spdk_rdma_provider_srq		*srq = NULL;
	struct ibv_recv_wr			*bad_wr = NULL;
	struct spdk_rdma_utils_memory_translation translation;
	uint32_t				i;
	int					rc = 0;

	resources = calloc(1, sizeof(struct spdk_nvmf_rdma_resources));
	if (!resources) {
		SPDK_ERRLOG("Unable to allocate resources for receive queue.\n");
		return NULL;
	}

	/* [한국어] 5개 풀 일괄 할당 — 모두 4KB align, SPDK_MALLOC_DMA (hugepage + pinned).
	 * RDMA NIC 가 DMA 로 직접 read/write 하므로 hugepage 가 IOVA 효율적. */
	resources->reqs = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->reqs),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->recvs = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->recvs),
					0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->cmds = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->cmds),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->cpls = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->cpls),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	/* [한국어] in-capsule data: host 가 small write (capsule 와 함께) 보낼 때 받을 영역.
	 * size 0 이면 미사용 (모든 데이터를 RDMA READ 로 가져옴). */
	if (opts->in_capsule_data_size > 0) {
		resources->bufs = spdk_zmalloc(opts->max_queue_depth * opts->in_capsule_data_size,
					       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
					       SPDK_MALLOC_DMA);
	}

	if (!resources->reqs || !resources->recvs || !resources->cmds ||
	    !resources->cpls || (opts->in_capsule_data_size && !resources->bufs)) {
		SPDK_ERRLOG("Unable to allocate sufficient memory for RDMA queue.\n");
		goto cleanup;
	}

	SPDK_DEBUGLOG(rdma, "Command Array: %p Length: %lx\n",
		      resources->cmds, opts->max_queue_depth * sizeof(*resources->cmds));
	SPDK_DEBUGLOG(rdma, "Completion Array: %p Length: %lx\n",
		      resources->cpls, opts->max_queue_depth * sizeof(*resources->cpls));
	if (resources->bufs) {
		SPDK_DEBUGLOG(rdma, "In Capsule Data Array: %p Length: %x\n",
			      resources->bufs, opts->max_queue_depth *
			      opts->in_capsule_data_size);
	}

	/* Initialize queues */
	STAILQ_INIT(&resources->incoming_queue);
	STAILQ_INIT(&resources->free_queue);

	if (opts->shared) {
		srq = (struct spdk_rdma_provider_srq *)opts->qp;
	} else {
		qp = (struct spdk_rdma_provider_qp *)opts->qp;
	}

	for (i = 0; i < opts->max_queue_depth; i++) {
		rdma_recv = &resources->recvs[i];
		rdma_recv->qpair = opts->qpair;

		/* Set up memory to receive commands */
		if (resources->bufs) {
			rdma_recv->buf = (void *)((uintptr_t)resources->bufs + (i *
						  opts->in_capsule_data_size));
		}

		rdma_recv->rdma_wr.type = RDMA_WR_TYPE_RECV;

		rdma_recv->sgl[0].addr = (uintptr_t)&resources->cmds[i];
		rdma_recv->sgl[0].length = sizeof(resources->cmds[i]);
		rc = spdk_rdma_utils_get_translation(opts->map, &resources->cmds[i], sizeof(resources->cmds[i]),
						     &translation);
		if (rc) {
			goto cleanup;
		}
		rdma_recv->sgl[0].lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);
		rdma_recv->wr.num_sge = 1;

		if (rdma_recv->buf) {
			rdma_recv->sgl[1].addr = (uintptr_t)rdma_recv->buf;
			rdma_recv->sgl[1].length = opts->in_capsule_data_size;
			rc = spdk_rdma_utils_get_translation(opts->map, rdma_recv->buf, opts->in_capsule_data_size,
							     &translation);
			if (rc) {
				goto cleanup;
			}
			rdma_recv->sgl[1].lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);
			rdma_recv->wr.num_sge++;
		}

		rdma_recv->wr.wr_id = (uintptr_t)&rdma_recv->rdma_wr;
		rdma_recv->wr.sg_list = rdma_recv->sgl;
		if (srq) {
			spdk_rdma_provider_srq_queue_recv_wrs(srq, &rdma_recv->wr);
		} else {
			spdk_rdma_provider_qp_queue_recv_wrs(qp, &rdma_recv->wr);
		}
	}

	for (i = 0; i < opts->max_queue_depth; i++) {
		rdma_req = &resources->reqs[i];

		if (opts->qpair != NULL) {
			rdma_req->req.qpair = &opts->qpair->qpair;
		} else {
			rdma_req->req.qpair = NULL;
		}
		rdma_req->req.cmd = NULL;
		rdma_req->req.iovcnt = 0;
		rdma_req->req.stripped_data = NULL;

		/* Set up memory to send responses */
		rdma_req->req.rsp = &resources->cpls[i];

		rdma_req->rsp.sgl[0].addr = (uintptr_t)&resources->cpls[i];
		rdma_req->rsp.sgl[0].length = sizeof(resources->cpls[i]);
		rc = spdk_rdma_utils_get_translation(opts->map, &resources->cpls[i], sizeof(resources->cpls[i]),
						     &translation);
		if (rc) {
			goto cleanup;
		}
		rdma_req->rsp.sgl[0].lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);

		rdma_req->rsp_wr.type = RDMA_WR_TYPE_SEND;
		rdma_req->rsp.wr.wr_id = (uintptr_t)&rdma_req->rsp_wr;
		rdma_req->rsp.wr.next = NULL;
		rdma_req->rsp.wr.opcode = IBV_WR_SEND;
		rdma_req->rsp.wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->rsp.wr.sg_list = rdma_req->rsp.sgl;
		rdma_req->rsp.wr.num_sge = SPDK_COUNTOF(rdma_req->rsp.sgl);

		/* Set up memory for data buffers */
		rdma_req->data_wr.type = RDMA_WR_TYPE_DATA;
		rdma_req->data.wr.wr_id = (uintptr_t)&rdma_req->data_wr;
		rdma_req->data.wr.next = NULL;
		rdma_req->data.wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->data.wr.sg_list = rdma_req->data.sgl;
		rdma_req->data.wr.num_sge = SPDK_COUNTOF(rdma_req->data.sgl);

		/* Initialize request state to FREE */
		rdma_req->state = RDMA_REQUEST_STATE_FREE;
		STAILQ_INSERT_TAIL(&resources->free_queue, rdma_req, state_link);
	}

	if (srq) {
		rc = spdk_rdma_provider_srq_flush_recv_wrs(srq, &bad_wr);
	} else {
		rc = spdk_rdma_provider_qp_flush_recv_wrs(qp, &bad_wr);
	}

	if (rc) {
		goto cleanup;
	}

	return resources;

cleanup:
	nvmf_rdma_resources_destroy(resources);
	return NULL;
}

/*
 * [한국어]
 * nvmf_rdma_qpair_clean_ibv_events - QP 파괴 전 ibv async event 컨텍스트 무효화.
 *
 * @rqpair: 파괴될 QP.
 * @return: void.
 *
 * QP 파괴 전 outstanding async event ctx 가 있으면 ctx->rqpair 를 NULL 로 설정.
 * send_msg 로 전달된 ctx 콜백 (nvmf_rdma_qpair_process_last_wqe_event) 이 나중에
 * 실행되더라도 이미 파괴된 rqpair 에 접근하지 않도록 방어. ctx 자체의 free 는
 * 콜백이 담당.
 *
 * 실행 컨텍스트: nvmf_rdma_qpair_destroy() 경로 — reactor 스레드.
 *
 * 호출 체인:
 *   nvmf_rdma_qpair_destroy() → [nvmf_rdma_qpair_clean_ibv_events]
 */
static void
nvmf_rdma_qpair_clean_ibv_events(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *ctx;  /* [한국어] 정리할 async event ctx 포인터. */

	ctx = rqpair->last_wqe_reached_ctx;  /* [한국어] 현재 outstanding ctx 가져옴. */
	if (ctx) {
		ctx->rqpair = NULL;  /* [한국어] rqpair 무효화 — 콜백이 freed rqpair 에 접근 차단. */
		/* Memory allocated for ctx is freed in nvmf_rdma_qpair_process_last_wqe_event */
		/* [한국어] ctx 자체는 콜백이 free — 여기서는 포인터 NULL 만. */
		rqpair->last_wqe_reached_ctx = NULL;  /* [한국어] rqpair 측 포인터도 NULL — double-free 방지. */
	}
}

static void nvmf_rdma_poller_destroy(struct spdk_nvmf_rdma_poller *poller);

/*
 * [한국어]
 * nvmf_rdma_qpair_destroy - RDMA QP 의 모든 자원 강제 회수.
 *
 * @rqpair: 파괴할 QP.
 *
 * 정상 disconnect 경로 (close_qpair → disconnect → drained → destroy) 의 마지막 단계.
 * 또는 에러 경로 (peer crash, device removal) 의 직접 호출.
 *
 * 단계:
 *   1. **In-flight request 강제 정리**: queue_depth > 0 이면 (정상은 0 이어야 함)
 *      모든 FREE 아닌 request 를 request_process 로 통과시켜 COMPLETED 로 보냄.
 *      ibv_in_error_state 또는 비활성 QP 조건으로 fast path 통해 회수.
 *   2. **poller 에서 제거**: RB tree (qpairs) 에서 빼고 active_qpairs 에서 빠짐을 assert.
 *   3. **SRQ 모드의 incoming_queue 정리**: 미처리 RECV 를 SRQ 로 재게시.
 *   4. **ibverbs QP destroy**: rdma_provider_qp_destroy.
 *   5. **poller->required_num_wr 감소**: 다음 cq resize 결정에 반영.
 *   6. **resources_destroy** (SRQ 미사용 시 — SRQ 면 poller 가 공유 resources 소유).
 *   7. **async ibv event ctx 정리**: last_wqe_reached_ctx 등.
 *   8. **destruct_channel io_channel put**.
 *   9. **poller 가 빈 상태 + need_destroy → poller_destroy** (cascading cleanup).
 *  10. **cm_id destroy** 가 마지막 — cma device 가 cq destroy 전에 free 되면 안 됨.
 *  11. **rqpair 자체 free**.
 *
 * 호출 체인:
 *   spdk_nvmf_qpair_disconnect → nvmf_rdma_close_qpair → drained → [본 함수]
 *   또는 device removal / peer crash → [본 함수] 직접
 */
static void
nvmf_rdma_qpair_destroy(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_recv	*rdma_recv, *recv_tmp;
	struct ibv_recv_wr		*bad_recv_wr = NULL;
	int				rc;

	spdk_trace_record(TRACE_RDMA_QP_DESTROY, 0, 0, (uintptr_t)rqpair);

	/* [한국어] queue_depth > 0 = drain 안 됨 (비정상 경로). 강제로 in-flight 정리. */
	if (rqpair->qpair.queue_depth != 0) {
		struct spdk_nvmf_qpair *qpair = &rqpair->qpair;
		struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(qpair->transport,
				struct spdk_nvmf_rdma_transport, transport);
		struct spdk_nvmf_rdma_request *req;
		uint32_t i, max_req_count = 0;

		SPDK_WARNLOG("Destroying qpair when queue depth is %d\n", rqpair->qpair.queue_depth);

		/* [한국어] SRQ 모드면 req 풀이 SRQ 공유라 max_srq_depth, 아니면 per-QP queue_depth.
		 * 진단 덤프 (SRQ 미사용 시만 — SRQ 면 다른 QP 의 req 도 섞여 출력 무의미). */
		if (rqpair->srq == NULL) {
			nvmf_rdma_dump_qpair_contents(rqpair);
			max_req_count = rqpair->max_queue_depth;
		} else if (rqpair->poller && rqpair->resources) {
			max_req_count = rqpair->poller->max_srq_depth;
		}

		SPDK_DEBUGLOG(rdma, "Release incomplete requests\n");
		/* [한국어] 풀의 모든 슬롯 순회 — 이 qpair 의 것 + FREE 아니면 request_process 로 강제 진행.
		 * ibv_in_error_state/비활성 fast path 가 COMPLETED 로 보내 자원 회수. */
		for (i = 0; i < max_req_count; i++) {
			req = &rqpair->resources->reqs[i];
			if (req->req.qpair == qpair && req->state != RDMA_REQUEST_STATE_FREE) {
				/* nvmf_rdma_request_process checks qpair ibv and internal state
				 * and completes a request */
				nvmf_rdma_request_process(rtransport, req);
			}
		}
		assert(rqpair->qpair.queue_depth == 0);
	}

	if (rqpair->poller) {
		/* [한국어] poller 의 RB tree (qp_num key) 에서 제거 — 이후 CQE 가 와도 추적 안 됨. */
		RB_REMOVE(qpairs_tree, &rqpair->poller->qpairs, rqpair);
		assert(TAILQ_ENTRY_NOT_ENQUEUED(rqpair, active_link));

		if (rqpair->srq != NULL && rqpair->resources != NULL) {
			/* Drop all received but unprocessed commands for this queue and return them to SRQ */
			/* [한국어] SRQ 모드: 이 QP 의 미처리 RECV 들을 SRQ 에 재게시 (다른 QP 가 사용 가능). */
			STAILQ_FOREACH_SAFE(rdma_recv, &rqpair->resources->incoming_queue, link, recv_tmp) {
				if (rqpair == rdma_recv->qpair) {
					STAILQ_REMOVE(&rqpair->resources->incoming_queue, rdma_recv, spdk_nvmf_rdma_recv, link);
					spdk_rdma_provider_srq_queue_recv_wrs(rqpair->srq, &rdma_recv->wr);
					rc = spdk_rdma_provider_srq_flush_recv_wrs(rqpair->srq, &bad_recv_wr);
					if (rc) {
						SPDK_ERRLOG("Unable to re-post rx descriptor\n");
					}
				}
			}
		}
	}

	if (rqpair->cm_id) {
		if (rqpair->rdma_qp != NULL) {
			/* [한국어] ibverbs QP destroy — 모든 pending WR cancel + QP 자원 회수. */
			spdk_rdma_provider_qp_destroy(rqpair->rdma_qp);
			rqpair->rdma_qp = NULL;
		}

		/* [한국어] poller 의 WR 수요 추적 — required_num_wr 가 줄면 다음 resize 시 CQ 축소. */
		if (rqpair->poller != NULL && rqpair->srq == NULL) {
			rqpair->poller->required_num_wr -= MAX_WR_PER_QP(rqpair->max_queue_depth);
		}
	}

	/* [한국어] SRQ 미사용 시만 per-QP resources 해제 — SRQ 면 poller 공유. */
	if (rqpair->srq == NULL && rqpair->resources != NULL) {
		nvmf_rdma_resources_destroy(rqpair->resources);
	}

	nvmf_rdma_qpair_clean_ibv_events(rqpair);

	if (rqpair->destruct_channel) {
		spdk_put_io_channel(rqpair->destruct_channel);
		rqpair->destruct_channel = NULL;
	}

	/* [한국어] cascading destroy: 이 QP 의 destroy 로 poller 가 빈 상태 + 종료 예약돼 있으면
	 * poller 자체도 회수 — 의존 chain 의 자연스러운 끝맺음. */
	if (rqpair->poller && rqpair->poller->need_destroy && RB_EMPTY(&rqpair->poller->qpairs)) {
		nvmf_rdma_poller_destroy(rqpair->poller);
	}

	/* destroy cm_id last so cma device will not be freed before we destroy the cq. */
	/* [한국어] cm_id 가 cma device 의 마지막 reference — cq destroy 전에 free 되면 BUG. */
	if (rqpair->cm_id) {
		rdma_destroy_id(rqpair->cm_id);
	}

	free(rqpair);
}

/*
 * [한국어]
 * nvmf_rdma_resize_cq - 새 QP 추가 시 CQ(Completion Queue) 크기를 동적으로 확장.
 *
 * @rqpair: 새로 추가되는 QP (이 QP 의 WR 수요 반영).
 * @device: QP 가 속한 RDMA device (max_cqe 한도 + transport_type 확인).
 * @return: 0 성공, -1 실패 (iWARP resize 미지원 또는 max_cqe 초과).
 *
 * RDMA CQ 는 연결된 모든 QP 의 WR 완료를 담아야 한다. 새 QP 가 추가되면 그 QP 의
 * MAX_WR_PER_QP(max_queue_depth) 만큼 CQ 수요가 증가 — 현재 CQ 가 부족하면 ibv_resize_cq.
 *
 * 계산 방식:
 *   required_num_wr = poller->required_num_wr + MAX_WR_PER_QP(new_qp)
 *   num_cqe = max(현재 CQ × 2, required_num_wr)  [두 배 증가 전략 = amortized O(1)]
 *   num_cqe = min(num_cqe, device->attr.max_cqe)  [HW 한도 초과 불가]
 *
 * iWARP 는 CQ resize 를 지원하지 않으므로 초기 CQ 크기를 충분히 설정해야 함.
 *
 * 실행 컨텍스트: nvmf_rdma_qpair_initialize() — reactor 스레드 (QP 추가 시).
 *
 * 호출 체인:
 *   nvmf_rdma_qpair_initialize() → [nvmf_rdma_resize_cq] → ibv_resize_cq
 */
static int
nvmf_rdma_resize_cq(struct spdk_nvmf_rdma_qpair *rqpair, struct spdk_nvmf_rdma_device *device)
{
	struct spdk_nvmf_rdma_poller	*rpoller;       /* [한국어] 이 QP 가 소속될 poller. */
	int				rc, num_cqe, required_num_wr;  /* [한국어] resize 결과/새 CQ 크기/필요 WR 수. */

	/* Enlarge CQ size dynamically */
	rpoller = rqpair->poller;  /* [한국어] QP 의 poller 참조 (CQ 소유자). */
	/* [한국어] 기존 required_num_wr + 새 QP 의 WR 수요 = 전체 수요. */
	required_num_wr = rpoller->required_num_wr + MAX_WR_PER_QP(rqpair->max_queue_depth);
	num_cqe = rpoller->num_cqe;  /* [한국어] 현재 CQ 깊이. */
	/* [한국어] 현재 CQ 가 부족하면 두 배 증가 전략 (amortized O(1) resize). */
	if (num_cqe < required_num_wr) {
		num_cqe = spdk_max(num_cqe * 2, required_num_wr);  /* [한국어] 최소 필요량 보장. */
		num_cqe = spdk_min(num_cqe, device->attr.max_cqe);  /* [한국어] HW 최대 한도 클램프. */
	}

	/* [한국어] 실제 크기 변화가 있을 때만 resize 시도. */
	if (rpoller->num_cqe != num_cqe) {
		/* [한국어] iWARP 는 ibv_resize_cq 를 지원하지 않음 — 초기 CQ 충분히 설정 필요. */
		if (device->context->device->transport_type == IBV_TRANSPORT_IWARP) {
			SPDK_ERRLOG("iWARP doesn't support CQ resize. Current capacity %u, required %u\n"
				    "Using CQ of insufficient size may lead to CQ overrun\n", rpoller->num_cqe, num_cqe);
			return -1;  /* [한국어] 실패 반환 — QP 초기화 중단. */
		}
		/* [한국어] device 의 max_cqe 초과 — 클램프 후에도 부족 (너무 많은 QP 연결). */
		if (required_num_wr > device->attr.max_cqe) {
			SPDK_ERRLOG("RDMA CQE requirement (%d) exceeds device max_cqe limitation (%d)\n",
				    required_num_wr, device->attr.max_cqe);
			return -1;  /* [한국어] 실패 — max_cqe 초과. */
		}

		SPDK_DEBUGLOG(rdma, "Resize RDMA CQ from %d to %d\n", rpoller->num_cqe, num_cqe);
		/* [한국어] libibverbs CQ resize 시스템 콜 — 커널에 새 크기 요청. */
		rc = ibv_resize_cq(rpoller->cq, num_cqe);
		if (rc) {
			SPDK_ERRLOG("RDMA CQ resize failed: errno %d: %s\n", errno, spdk_strerror(errno));
			return -1;  /* [한국어] resize 실패 — errno 로 원인 진단. */
		}

		rpoller->num_cqe = num_cqe;  /* [한국어] 성공 시 새 CQ 크기 기록. */
	}

	rpoller->required_num_wr = required_num_wr;  /* [한국어] 총 WR 수요 업데이트 — 다음 QP 추가 시 기준. */
	return 0;  /* [한국어] 성공. */
}

/*
 * [한국어]
 * nvmf_rdma_qpair_initialize - ★ 실제 ibverbs QP 생성 + resources 풀 + STAILQ 초기화 ★.
 *
 * @qpair: 공통 NVMe-oF qpair (poll_group 할당 후 호출됨).
 * @return: 0 성공 — accept 발행 준비됨, -1 실패 (cm_id 도 destroy 됨).
 *
 * nvmf_rdma_connect 가 calloc 한 rqpair (max_queue_depth/max_read_depth 협상 완료) 에 대해
 * 본 함수가 ibverbs 자원을 실제 할당. poll_group_add 가 본 함수 호출 (간접적으로).
 *
 * 단계:
 *   1. **QP init attr 채우기**:
 *      - qp_context = rqpair (CQE 의 qp_num 으로 역추적 시 사용).
 *      - pd = device->pd (Protection Domain — MR 의 lkey/rkey 검증 컨텍스트).
 *      - send_cq = recv_cq = poller->cq (모든 QP 가 한 CQ 공유).
 *      - SRQ 모드 → srq 지정, 아니면 max_recv_wr 지정.
 *      - max_send_wr = max_queue_depth * 2 (SEND + RDMA op 각각).
 *      - max_send_sge / max_recv_sge = device 의 SGE 한도와 SPDK 기본값 의 min.
 *      - stats = poller->stat.qp_stats (per-QP RDMA 통계 누적 위치).
 *   2. **CQ resize** (non-SRQ 만): 새 QP 의 WR 수요만큼 CQ 깊이 확장.
 *   3. **ibverbs QP create** (spdk_rdma_provider_qp_create) — pd/cq/srq 묶어 RC QP 생성.
 *   4. **qp_num 캐시** + max_send_depth/sge/recv_sge 확정 (실제 NIC 가 줄여줬을 수 있음).
 *   5. **resources_create** (non-SRQ 만 — SRQ 면 poller resources 공유).
 *   6. **3개 PENDING STAILQ + recv_depth/queue_depth 0 초기화**.
 *
 * 에러 경로: cm_id 도 함께 destroy (호출자가 별도 cleanup 안 해도 됨).
 *
 * accept 발행은 본 함수 후 nvmf_rdma_qpair_accept 가 별도 단계로 — 본 함수는 자원만 준비.
 *
 * 호출 체인:
 *   spdk_nvmf_tgt_new_qpair → poll_group_add → [본 함수]
 *     → ibverbs QP create + resources 풀 → 이후 accept 발행
 */
static int
nvmf_rdma_qpair_initialize(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_transport		*transport;
	struct spdk_nvmf_rdma_resource_opts	opts;
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_rdma_provider_qp_init_attr	qp_init_attr = {};

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	device = rqpair->device;

	/* [한국어] QP init attr — ibverbs ibv_create_qp 의 ibv_qp_init_attr_ex 와 매칭. */
	qp_init_attr.qp_context	= rqpair;
	qp_init_attr.pd		= device->pd;
	qp_init_attr.send_cq	= rqpair->poller->cq;
	qp_init_attr.recv_cq	= rqpair->poller->cq;

	/* [한국어] SRQ 모드면 자체 RECV 큐 없이 SRQ 공유, 아니면 per-QP RECV 큐 크기 지정. */
	if (rqpair->srq) {
		qp_init_attr.srq		= rqpair->srq->srq;
	} else {
		qp_init_attr.cap.max_recv_wr	= rqpair->max_queue_depth;
	}

	/* SEND, READ, and WRITE operations */
	/* [한국어] max_send_wr = depth * 2 — 1 NVMe command 당 (SEND 응답) + (RDMA READ/WRITE 데이터) 각 1.
	 * NIC 의 max_qp_wr 한도 안에서 협상 결과로 줄어들 수 있음. */
	qp_init_attr.cap.max_send_wr	= (uint32_t)rqpair->max_queue_depth * 2;
	qp_init_attr.cap.max_send_sge	= spdk_min((uint32_t)device->attr.max_sge, NVMF_DEFAULT_TX_SGE);
	qp_init_attr.cap.max_recv_sge	= spdk_min((uint32_t)device->attr.max_sge, NVMF_DEFAULT_RX_SGE);
	qp_init_attr.stats		= &rqpair->poller->stat.qp_stats;

	/* [한국어] CQ 확장 — 새 QP 가 추가되면 CQ 도 더 큰 깊이 필요 (poller->required_num_wr 증가). */
	if (rqpair->srq == NULL && nvmf_rdma_resize_cq(rqpair, device) < 0) {
		SPDK_ERRLOG("Failed to resize the completion queue. Cannot initialize qpair.\n");
		goto error;
	}

	/* [한국어] ★ 실제 ibverbs QP 생성 ★ — RC (Reliable Connection) type, pd/cq/srq 묶기. */
	rqpair->rdma_qp = spdk_rdma_provider_qp_create(rqpair->cm_id, &qp_init_attr);
	if (!rqpair->rdma_qp) {
		goto error;
	}

	/* [한국어] qp_num 캐시 — 이후 poller_poll 의 RB tree 검색에 사용 (CQE → QP 역추적). */
	rqpair->qp_num = rqpair->rdma_qp->qp->qp_num;

	/* [한국어] NIC 가 실제로 부여한 값으로 max_* 재확정 — 요청 값보다 작을 수 있음. */
	rqpair->max_send_depth = spdk_min((uint32_t)(rqpair->max_queue_depth * 2),
					  qp_init_attr.cap.max_send_wr);
	rqpair->max_send_sge = spdk_min(NVMF_DEFAULT_TX_SGE, qp_init_attr.cap.max_send_sge);
	rqpair->max_recv_sge = spdk_min(NVMF_DEFAULT_RX_SGE, qp_init_attr.cap.max_recv_sge);
	spdk_trace_record(TRACE_RDMA_QP_CREATE, 0, 0, (uintptr_t)rqpair);
	SPDK_DEBUGLOG(rdma, "New RDMA Connection: %p\n", qpair);

	/* [한국어] resources 풀 — SRQ 미사용 시 per-QP 별도 풀, SRQ 면 poller resources 공유. */
	if (rqpair->poller->srq == NULL) {
		rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);
		transport = &rtransport->transport;

		opts.qp = rqpair->rdma_qp;
		opts.map = device->map;
		opts.qpair = rqpair;
		opts.shared = false;
		opts.max_queue_depth = rqpair->max_queue_depth;
		opts.in_capsule_data_size = transport->opts.in_capsule_data_size;

		rqpair->resources = nvmf_rdma_resources_create(&opts);

		if (!rqpair->resources) {
			SPDK_ERRLOG("Unable to allocate resources for receive queue.\n");
			rdma_destroy_qp(rqpair->cm_id);
			goto error;
		}
	} else {
		/* [한국어] SRQ 모드: 풀 공유 — 메모리 절약 (N QP × queue_depth → 단일 max_srq_depth). */
		rqpair->resources = rqpair->poller->resources;
	}

	/* [한국어] runtime 카운터 + STAILQ 초기화 — accept 후 첫 RECV 도착 전 깨끗한 상태. */
	rqpair->current_recv_depth = 0;
	STAILQ_INIT(&rqpair->pending_rdma_read_queue);
	STAILQ_INIT(&rqpair->pending_rdma_write_queue);
	STAILQ_INIT(&rqpair->pending_rdma_send_queue);
	rqpair->qpair.queue_depth = 0;

	return 0;

error:
	/* [한국어] 실패 시 cm_id 도 함께 destroy — connect 가 calloc 한 rqpair 는 호출자가 free. */
	rdma_destroy_id(rqpair->cm_id);
	rqpair->cm_id = NULL;
	return -1;
}

/* Append the given recv wr structure to the resource structs outstanding recvs list. */
/* This function accepts either a single wr or the first wr in a linked list. */
/*
 * [한국어]
 * nvmf_rdma_qpair_queue_recv_wrs - RECV WR 을 SRQ 또는 per-QP RECV 큐에 게시(enqueue).
 *
 * @rqpair: RECV 를 게시할 QP.
 * @first: 게시할 RECV WR 의 선두 (단일 또는 linked list).
 * @return: void.
 *
 * request 처리 완료 후 해당 RECV 슬롯을 새 capsule 수신을 위해 재게시.
 * SRQ 모드: 모든 QP 공유 SRQ 에 enqueue → spdk_rdma_provider_srq_queue_recv_wrs.
 * per-QP 모드: QP 별 RECV 큐에 enqueue → spdk_rdma_provider_qp_queue_recv_wrs.
 *   - per-QP 큐가 가득 차면 rqpair 를 qpairs_pending_recv 에 등록 → 다음 iteration 에 재시도.
 * no_wr_batching=true 면 즉시 flush(ibv_post_recv), false 면 batch 종료 시 일괄 flush.
 *
 * 실행 컨텍스트: nvmf_rdma_request_process() 의 COMPLETING 상태 — reactor 스레드.
 *
 * 호출 체인:
 *   nvmf_rdma_request_process(COMPLETING) → request_transfer_out → [본 함수]
 *     → spdk_rdma_provider_srq_queue_recv_wrs 또는 qp_queue_recv_wrs
 *     → (no_wr_batching) _poller_submit_recvs → ibv_post_recv
 */
static void
nvmf_rdma_qpair_queue_recv_wrs(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_recv_wr *first)
{
	/* [한국어] transport opts 에서 no_wr_batching 확인 — batching 모드 분기에 사용. */
	struct spdk_nvmf_rdma_transport *rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
			struct spdk_nvmf_rdma_transport, transport);

	/* [한국어] SRQ 모드: 공유 SRQ 에 enqueue (개별 QP 의 srq 포인터로 확인). */
	if (rqpair->srq != NULL) {
		spdk_rdma_provider_srq_queue_recv_wrs(rqpair->srq, first);
	} else {
		/* [한국어] per-QP: QP 별 RECV 큐에 WR 추가. 큐 가득 참 = true 반환. */
		if (spdk_rdma_provider_qp_queue_recv_wrs(rqpair->rdma_qp, first)) {
			/* [한국어] 큐 full → rqpair 를 pending_recv 에 등록 — 다음 iteration 에 flush 시도. */
			STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_recv, rqpair, recv_link);
		}
	}

	/* [한국어] no_wr_batching=true 면 즉시 ibv_post_recv 발행, false 면 batch 종료 시 일괄. */
	if (rtransport->rdma_opts.no_wr_batching) {
		_poller_submit_recvs(rtransport, rqpair->poller);
	}
}

/*
 * [한국어]
 * request_transfer_in - NVMe write 의 데이터 수신을 위해 RDMA READ WR 발행.
 *
 * @req: RDMA READ 를 발행할 NVMe-oF request (xfer=HOST_TO_CONTROLLER).
 * @return: void.
 *
 * NVMe write 명령 처리 시 host 의 데이터 버퍼에서 controller 의 버퍼로 데이터를
 * 읽어오는 RDMA READ WR 을 QP 에 게시. transfer_wr 체인이 이미 fill_wr_sgl 로
 * 준비돼 있으며, 본 함수는 실제 post(게시)와 depth 카운터 업데이트만 담당.
 *
 * current_read_depth / current_send_depth 증가 — 한도 초과 방지는 이미
 * nvmf_rdma_request_process 가 PENDING 상태로 대기시켜서 보장.
 *
 * 실행 컨텍스트: nvmf_rdma_request_process() — TRANSFERRING_HOST_TO_CONTROLLER 상태 진입 시.
 *
 * 호출 체인:
 *   nvmf_rdma_request_process(HAVE_BUFFER) → [request_transfer_in]
 *     → spdk_rdma_provider_qp_queue_send_wrs → (no_wr_batching) ibv_post_send
 */
static inline void
request_transfer_in(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_request	*rdma_req;    /* [한국어] RDMA request 컨테이너. */
	struct spdk_nvmf_qpair		*qpair;       /* [한국어] 공통 qpair. */
	struct spdk_nvmf_rdma_qpair	*rqpair;      /* [한국어] RDMA QP. */
	struct spdk_nvmf_rdma_transport *rtransport;  /* [한국어] transport (opts 확인용). */

	qpair = req->qpair;  /* [한국어] 공통 qpair 가져오기. */
	rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
				      struct spdk_nvmf_rdma_transport, transport);

	assert(req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);  /* [한국어] write 방향 확인. */
	assert(rdma_req != NULL);

	/* [한국어] RDMA READ WR chain 을 QP send queue 에 enqueue.
	 * true 반환 = 첫 enqueue 이므로 QP 를 pending_send 에 등록 (batch 대상). */
	if (spdk_rdma_provider_qp_queue_send_wrs(rqpair->rdma_qp, rdma_req->transfer_wr)) {
		STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_send, rqpair, send_link);
	}
	/* [한국어] no_wr_batching=true 면 즉시 ibv_post_send, false 면 batch 종료 시. */
	if (rtransport->rdma_opts.no_wr_batching) {
		_poller_submit_sends(rtransport, rqpair->poller);
	}

	/* [한국어] RDMA READ depth 카운터 증가 — max_read_depth 내에 있음은 호출 전 보장. */
	assert(rqpair->current_read_depth + rdma_req->num_outstanding_data_wr <= rqpair->max_read_depth);
	rqpair->current_read_depth += rdma_req->num_outstanding_data_wr;
	/* [한국어] SEND depth 도 증가 (RDMA READ 도 send queue 의 WR 소비). */
	assert(rqpair->current_send_depth + rdma_req->num_outstanding_data_wr <= rqpair->max_send_depth);
	rqpair->current_send_depth += rdma_req->num_outstanding_data_wr;
}

/*
 * [한국어]
 * nvmf_rdma_request_reset_transfer_in - 첫 batch RDMA READ 완료 후 다음 batch 로 전환.
 *
 * @rdma_req: split multi-WR request.
 * @rtransport: data_wr_pool 접근용 transport.
 * @return: void.
 *
 * multi-SGL write 에서 RDMA READ WR 을 max_read_depth 단위로 나눠 발행할 때,
 * 첫 batch 의 WR 들이 모두 완료(num_outstanding_data_wr → 0)되면 본 함수로
 * transfer_wr 포인터를 다음 batch(remaining_tranfer_in_wrs) 로 이동.
 * 완료된 첫 batch WR 들은 mempool 에 반환.
 *
 * 실행 컨텍스트: nvmf_rdma_request_process() — DATA_WR CQE 처리 후 split 감지 시.
 *
 * 호출 체인:
 *   nvmf_rdma_request_process(TRANSFERRING_HOST_TO_CTRL, remaining WRs exist)
 *     → [nvmf_rdma_request_reset_transfer_in] → _nvmf_rdma_request_free_data
 */
static inline void
nvmf_rdma_request_reset_transfer_in(struct spdk_nvmf_rdma_request *rdma_req,
				    struct spdk_nvmf_rdma_transport *rtransport)
{
	/* Put completed WRs back to pool and move transfer_wr pointer */
	/* [한국어] 완료된 첫 batch WR 들 mempool 반환 (embedded data.wr 제외). */
	_nvmf_rdma_request_free_data(rdma_req, rdma_req->transfer_wr, rtransport->data_wr_pool);
	/* [한국어] 다음 batch 로 전환 — remaining 이 새 transfer_wr 가 됨. */
	rdma_req->transfer_wr = rdma_req->remaining_tranfer_in_wrs;
	rdma_req->remaining_tranfer_in_wrs = NULL;  /* [한국어] remaining 포인터 초기화 — 단일 batch로 만듦. */
	/* [한국어] num_outstanding = 다음 batch 크기, remaining = 0 (이제 이 batch가 전부). */
	rdma_req->num_outstanding_data_wr = rdma_req->num_remaining_data_wr;
	rdma_req->num_remaining_data_wr = 0;
}

/*
 * [한국어]
 * request_prepare_transfer_in_part - multi-SGL write WR chain 을 max_read_depth 단위로 분할.
 *
 * @req: 분할 대상 write request (xfer=HOST_TO_CONTROLLER).
 * @num_reads_available: 현재 발행 가능한 RDMA READ WR 수 (max_read_depth 여유).
 * @return: 항상 0 (실패 없음).
 *
 * 전체 WR chain 이 num_reads_available 보다 길면, chain 을 num_reads_available 위치에서
 * 잘라 첫 batch(transfer_wr ~ wr) 와 나머지(remaining_tranfer_in_wrs ~ 끝)로 분리.
 * 첫 batch 만 즉시 발행하고, 나머지는 첫 batch CQE 수신 후 reset_transfer_in 으로 이어받음.
 *
 * chain 을 wr->next = NULL 로 자름 — ibv_post_send 가 NULL 에서 멈춤.
 *
 * 실행 컨텍스트: nvmf_rdma_request_process() — 새 QP 에 RDMA READ 발행 시 depth 체크 후.
 *
 * 호출 체인:
 *   nvmf_rdma_request_process(DATA_XFER_TO_CTRL_PENDING) → [request_prepare_transfer_in_part]
 *     → request_transfer_in (첫 batch 발행)
 */
static inline int
request_prepare_transfer_in_part(struct spdk_nvmf_request *req, uint32_t num_reads_available)
{
	struct spdk_nvmf_rdma_request	*rdma_req;  /* [한국어] RDMA request 컨테이너. */
	struct ibv_send_wr		*wr;          /* [한국어] 체인 순회 포인터. */
	uint32_t i;                               /* [한국어] 순회 카운터. */

	rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);

	/* [한국어] 전제 조건 확인 — 발행 가능 depth > 0, 전체 WR 수 > 가용량. */
	assert(req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);
	assert(rdma_req != NULL);
	assert(num_reads_available > 0);
	assert(rdma_req->num_outstanding_data_wr > num_reads_available);
	wr = rdma_req->transfer_wr;  /* [한국어] chain 의 선두 WR 부터 순회 시작. */

	/* [한국어] num_reads_available 번째 WR 까지 순회 (0-based: i < N-1 이면 N번째에서 멈춤). */
	for (i = 0; i < num_reads_available - 1; i++) {
		wr = wr->next;  /* [한국어] 다음 WR 로 이동. */
	}

	/* [한국어] wr 이후가 나머지 batch — remaining 포인터로 저장. */
	rdma_req->remaining_tranfer_in_wrs = wr->next;
	/* [한국어] 나머지 WR 수 계산. */
	rdma_req->num_remaining_data_wr = rdma_req->num_outstanding_data_wr - num_reads_available;
	/* [한국어] 첫 batch 의 WR 수를 가용량으로 한정. */
	rdma_req->num_outstanding_data_wr = num_reads_available;
	/* Break chain of WRs to send only part. Once this portion completes, we continue sending RDMA_READs */
	/* [한국어] chain 절단 — ibv_post_send 가 NULL 에서 종료하도록. */
	wr->next = NULL;

	return 0;  /* [한국어] 항상 성공. */
}

/*
 * [한국어]
 * request_transfer_out - NVMe read 응답을 위해 RDMA WRITE + SEND WR chain 발행.
 *
 * @req: 응답을 전송할 NVMe-oF request (성공 시 xfer=CONTROLLER_TO_HOST 가능).
 * @data_posted: [출력] RDMA WRITE data WR 가 실제 발행됐으면 1, 아니면 0.
 * @return: 항상 0.
 *
 * NVMe read 또는 write (성공/실패 무관) 의 응답 전송 단계.
 *
 * 동작:
 *   1. sq_head 증가 — NVMe completion 의 sqhd 필드 업데이트 (SQ head 추적).
 *   2. RECV 슬롯 재게시 — 이 capsule 을 받은 RECV WR 을 새 capsule 수신용으로 반환.
 *      current_recv_depth 감소.
 *   3. 에러 시: data WR 없이 rsp.wr (SEND) 만 발행.
 *      성공 + read 방향: transfer_wr (RDMA WRITE chain) → rsp.wr 순서로 발행.
 *      성공 + write/nodata 방향: rsp.wr (SEND) 만 발행.
 *   4. no_wr_batching 또는 interrupt_mode 면 즉시 ibv_post_send.
 *   5. current_send_depth += data WR 수 + 1(rsp WR).
 *
 * 실행 컨텍스트: nvmf_rdma_request_process() — READY_TO_COMPLETE 상태 진입 시.
 *
 * 호출 체인:
 *   nvmf_rdma_request_process(READY_TO_COMPLETE) → [request_transfer_out]
 *     → nvmf_rdma_qpair_queue_recv_wrs (RECV 재게시)
 *     → spdk_rdma_provider_qp_queue_send_wrs → (no_wr_batching) ibv_post_send
 */
static int
request_transfer_out(struct spdk_nvmf_request *req, int *data_posted)
{
	int				num_outstanding_data_wr = 0;  /* [한국어] 발행한 data WR 수 (통계/depth용). */
	struct spdk_nvmf_rdma_request	*rdma_req;    /* [한국어] RDMA request 컨테이너. */
	struct spdk_nvmf_qpair		*qpair;       /* [한국어] 공통 qpair. */
	struct spdk_nvmf_rdma_qpair	*rqpair;      /* [한국어] RDMA QP. */
	struct spdk_nvme_cpl		*rsp;         /* [한국어] NVMe completion 구조체 포인터. */
	struct ibv_send_wr		*first = NULL;    /* [한국어] 발행할 WR chain 의 선두. */
	struct spdk_nvmf_rdma_transport *rtransport;  /* [한국어] transport opts 접근용. */

	*data_posted = 0;  /* [한국어] 초기화 — data WR 없음 가정. */
	qpair = req->qpair;
	rsp = &req->rsp->nvme_cpl;  /* [한국어] NVMe completion entry 포인터. */
	rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
				      struct spdk_nvmf_rdma_transport, transport);

	/* Advance our sq_head pointer */
	/* [한국어] SQ(Submission Queue) head 포인터 순환 증가 — sqhd 필드에 기록. */
	if (qpair->sq_head == qpair->sq_head_max) {
		qpair->sq_head = 0;  /* [한국어] 환형 큐 wrap-around. */
	} else {
		qpair->sq_head++;
	}
	rsp->sqhd = qpair->sq_head;  /* [한국어] host 에 전달할 sq_head 값 설정. */

	/* queue the capsule for the recv buffer */
	/* [한국어] 이 capsule 을 수신한 RECV 슬롯을 새 capsule 수신을 위해 재게시. */
	assert(rdma_req->recv != NULL);  /* [한국어] recv 가 NULL 이면 버그. */

	nvmf_rdma_qpair_queue_recv_wrs(rqpair, &rdma_req->recv->wr);  /* [한국어] RECV WR 재게시. */

	rdma_req->recv = NULL;  /* [한국어] recv 포인터 해제 — 재게시됐으므로 더 이상 소유 안 함. */
	assert(rqpair->current_recv_depth > 0);
	rqpair->current_recv_depth--;  /* [한국어] RECV 슬롯 1개 반환 → depth 감소. */

	/* Build the response which consists of optional
	 * RDMA WRITEs to transfer data, plus an RDMA SEND
	 * containing the response.
	 */
	/* [한국어] 기본: rsp.wr 만 발행 (write 방향 또는 에러). */
	first = &rdma_req->rsp.wr;

	if (spdk_unlikely(spdk_nvme_cpl_is_error(rsp))) {
		/* On failure, data was not read from the controller. So clear the
		 * number of outstanding data WRs to zero.
		 */
		/* [한국어] 에러 응답: data WR 없이 rsp SEND 만 발행 — outstanding 0으로 초기화. */
		rdma_req->num_outstanding_data_wr = 0;
	} else if (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		/* [한국어] 성공한 read: RDMA WRITE(data) chain 이 rsp.wr 앞에 먼저 발행됨. */
		first = rdma_req->transfer_wr;  /* [한국어] RDMA WRITE WR chain 이 선두. */
		*data_posted = 1;  /* [한국어] data WR 발행됨 표시 — 호출자가 WRITE CQE 대기. */
		num_outstanding_data_wr = rdma_req->num_outstanding_data_wr;
	}
	/* [한국어] WR chain 을 QP send queue 에 enqueue. true = 첫 enqueue → pending_send 등록. */
	if (spdk_rdma_provider_qp_queue_send_wrs(rqpair->rdma_qp, first)) {
		STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_send, rqpair, send_link);
	}
	/* [한국어] no_wr_batching 또는 interrupt 모드면 즉시 ibv_post_send. */
	if (rtransport->rdma_opts.no_wr_batching || spdk_interrupt_mode_is_enabled()) {
		_poller_submit_sends(rtransport, rqpair->poller);
	}

	/* +1 for the rsp wr */
	/* [한국어] SEND depth 증가: data WR 수 + 응답 SEND WR 1개. */
	assert(rqpair->current_send_depth + num_outstanding_data_wr + 1 <= rqpair->max_send_depth);
	rqpair->current_send_depth += num_outstanding_data_wr + 1;

	return 0;  /* [한국어] 항상 성공. */
}

/*
 * [한국어]
 * nvmf_rdma_event_accept - RDMA CM CONNECT_REQUEST 에 대해 ACCEPT 응답 발행.
 *
 * @id: 호스트가 연결 요청한 CM id.
 * @rqpair: 이미 생성된 QP (qpair_initialize 완료 후 호출됨).
 * @return: 0 성공, 음수 실패.
 *
 * rdma_conn_param 에 NVMe-oF RDMA accept private_data 를 채워 rdma_accept 발행:
 *   - recfmt=0 (NVMe-oF spec §3.3: Record Format 버전 0).
 *   - crqsize=max_queue_depth (controller 측 큐 깊이 — 호스트에게 알림).
 *   - RDMA_PS_TCP 시 responder_resources=0 (target 은 host 의 RDMA READ 를 받지 않음),
 *     initiator_depth=max_read_depth (target 이 host 에 RDMA READ 발행할 수 있는 최대 depth).
 *   - rnr_retry_count=0x7 (무한 재시도 — host NIC 가 RNR_RETRY_EXCEEDED 반환 방지).
 *   - srq/qp_num: rdma cm API 미사용 시 initiator 에 전달할 추가 정보.
 *
 * 실행 컨텍스트: nvmf_rdma_qpair_initialize() 마지막 단계 — reactor 스레드.
 *
 * 호출 체인:
 *   nvmf_rdma_qpair_initialize() → [nvmf_rdma_event_accept]
 *     → spdk_rdma_provider_qp_accept → rdma_accept (librdmacm)
 */
static int
nvmf_rdma_event_accept(struct rdma_cm_id *id, struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_accept_private_data	accept_data;    /* [한국어] NVMe-oF accept private data. */
	struct rdma_conn_param				ctrlr_event_data = {};  /* [한국어] CM 연결 파라미터 초기화. */
	int						rc;

	accept_data.recfmt = 0;  /* [한국어] NVMe-oF §3.3: Record Format = 0. */
	accept_data.crqsize = rqpair->max_queue_depth;  /* [한국어] 협상된 controller 측 큐 깊이 전달. */

	ctrlr_event_data.private_data = &accept_data;  /* [한국어] accept private data 포인터. */
	ctrlr_event_data.private_data_len = sizeof(accept_data);
	/* [한국어] RDMA_PS_TCP (iWARP 포함) 시 READ depth 명시 — IB 는 CM 에서 자동 협상. */
	if (id->ps == RDMA_PS_TCP) {
		ctrlr_event_data.responder_resources = 0; /* We accept 0 reads from the host */
		/* [한국어] target → host RDMA READ 의 최대 depth (호스트의 responder_resources 한도). */
		ctrlr_event_data.initiator_depth = rqpair->max_read_depth;
	}

	/* Configure infinite retries for the initiator side qpair.
	 * We need to pass this value to the initiator to prevent the
	 * initiator side NIC from completing SEND requests back to the
	 * initiator with status rnr_retry_count_exceeded. */
	/* [한국어] RNR(Receiver Not Ready) 재시도 횟수 = 0x7 (무한 재시도). */
	ctrlr_event_data.rnr_retry_count = 0x7;

	/* When qpair is created without use of rdma cm API, an additional
	 * information must be provided to initiator in the connection response:
	 * whether qpair is using SRQ and its qp_num
	 * Fields below are ignored by rdma cm if qpair has been
	 * created using rdma cm API. */
	/* [한국어] SRQ 사용 여부와 qp_num 을 initiator 에 알림 (rdma cm API 미사용 시). */
	ctrlr_event_data.srq = rqpair->srq ? 1 : 0;
	ctrlr_event_data.qp_num = rqpair->qp_num;

	/* [한국어] rdma CM accept 발행 — 호스트가 ESTABLISHED 이벤트 수신하면 연결 완료. */
	rc = spdk_rdma_provider_qp_accept(rqpair->rdma_qp, &ctrlr_event_data);
	if (rc) {
		SPDK_ERRLOG("Error %d on spdk_rdma_provider_qp_accept\n", errno);
	} else {
		SPDK_DEBUGLOG(rdma, "Sent back the accept\n");
	}

	return rc;  /* [한국어] 0 성공, 음수 실패. */
}

/*
 * [한국어]
 * nvmf_rdma_event_reject - RDMA CM CONNECT_REQUEST 에 대해 REJECT 응답 발행.
 *
 * @id: 호스트가 연결 요청한 CM id.
 * @error: NVMe-oF RDMA 에러 코드 (spdk_nvmf_rdma_transport_error enum).
 * @return: void.
 *
 * 연결 거절 사유를 private_data 에 담아 rdma_reject 호출.
 * 호스트는 RDMA_CM_EVENT_REJECTED 와 함께 rej_data.sts 에러 코드를 수신.
 * recfmt=0 (NVMe-oF §3.3 Record Format 버전 0).
 *
 * 실행 컨텍스트: nvmf_rdma_connect() 의 에러 경로 — accept_poller 스레드.
 *
 * 호출 체인:
 *   nvmf_rdma_connect() [에러] → [nvmf_rdma_event_reject] → rdma_reject (librdmacm)
 */
static void
nvmf_rdma_event_reject(struct rdma_cm_id *id, enum spdk_nvmf_rdma_transport_error error)
{
	struct spdk_nvmf_rdma_reject_private_data	rej_data;  /* [한국어] 거절 private data. */

	rej_data.recfmt = 0;  /* [한국어] NVMe-oF §3.3: Record Format = 0. */
	rej_data.sts = error;  /* [한국어] 거절 사유 에러 코드. */

	rdma_reject(id, &rej_data, sizeof(rej_data));  /* [한국어] librdmacm reject — 호스트에 CM REJECTED 전달. */
}

/* [한국어] 전방 선언 — cm_id 에서 peer/local trid 를 추출하는 헬퍼. */
static void nvmf_rdma_trid_from_cm_id(struct rdma_cm_id *id,
				      struct spdk_nvme_transport_id *trid,
				      bool peer);

/*
 * [한국어]
 * nvmf_rdma_connect - RDMA_CM_EVENT_CONNECT_REQUEST 처리 — 새 QP 생성 + accept.
 *
 * @transport: 공통 transport.
 * @event: CM 이벤트 (CONNECT_REQUEST). private_data 에 호스트의 nvmf_rdma_request_private_data
 *        (qid, hrqsize, hsqsize, recfmt, cntlid) 포함.
 * @return: 0 성공, 음수 errno (실패 시 자동 reject 발행).
 *
 * CM CONNECT_REQUEST 가 도착하면 본 함수가 다음을 수행:
 *   1. private_data 검증 (RECFMT==0, 길이 충분).
 *   2. ★ Queue depth 협상 ★ — 4단계 min 계산:
 *      - transport.opts.max_queue_depth (target 설정)
 *      - local NIC max_qp_wr / max_qp_init_rd_atom (NIC HW 한도)
 *      - remote initiator_depth (호스트 NIC 한도)
 *      - private_data->hrqsize, hsqsize+1 (호스트 큐 크기)
 *      각 단계 spdk_min 으로 누적 → 최소값이 협상 결과.
 *   3. rqpair 객체 calloc + 기본 필드 셋업.
 *   4. spdk_nvmf_tgt_new_qpair 호출 → 상위 NVMe-oF 레이어에 새 qpair 등록 →
 *      Fabrics CONNECT capsule 처리 → poll_group 할당 → qpair_initialize 호출.
 *   5. qpair_initialize 가 실제 ibverbs QP 생성 + RECV pre-post + rdma_accept 발행.
 *
 * 거절 경로: 어느 단계 실패 시 nvmf_rdma_event_reject 로 CM REJECT 발행 → 호스트는
 * RDMA_CM_EVENT_REJECTED 받아 error 인식.
 *
 * 호스트의 responder_resources 검사: NVMe-oF spec 상 호스트는 RDMA READ/atomic 수행 안 함
 * (target 만 수행) — 따라서 host 의 responder_resources 는 0 이어야 함. 비제로면 경고만.
 *
 * NUMA 인식: cm_id 의 NIC 가 어느 NUMA node 에 있는지 spdk_rdma_cm_id_get_numa_id 로 조회 →
 * 이후 poll_group 할당 시 같은 NUMA reactor 선호.
 *
 * 호출 체인:
 *   nvmf_process_cm_events (CONNECT_REQUEST) → [본 함수]
 *     → queue depth 협상
 *     → spdk_nvmf_tgt_new_qpair
 *       → poll_group_add → nvmf_rdma_qpair_initialize (실제 QP create + accept)
 */
static int
nvmf_rdma_connect(struct spdk_nvmf_transport *transport, struct rdma_cm_event *event)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_qpair	*rqpair = NULL;
	struct spdk_nvmf_rdma_port	*port;
	struct rdma_conn_param		*rdma_param = NULL;
	const struct spdk_nvmf_rdma_request_private_data *private_data = NULL;
	uint16_t			max_queue_depth;
	uint16_t			max_read_depth;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	assert(event->id != NULL); /* Impossible. Can't even reject the connection. */
	assert(event->id->verbs != NULL); /* Impossible. No way to handle this. */

	rdma_param = &event->param.conn;
	if (rdma_param->private_data == NULL ||
	    rdma_param->private_data_len < sizeof(struct spdk_nvmf_rdma_request_private_data)) {
		SPDK_ERRLOG("connect request: no private data provided\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH);
		return -1;
	}

	private_data = rdma_param->private_data;
	if (private_data->recfmt != 0) {
		SPDK_ERRLOG("Received RDMA private data with RECFMT != 0\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_RECFMT);
		return -1;
	}

	SPDK_DEBUGLOG(rdma, "Connect Recv on fabric intf name %s, dev_name %s\n",
		      event->id->verbs->device->name, event->id->verbs->device->dev_name);

	port = event->listen_id->context;
	SPDK_DEBUGLOG(rdma, "Listen Id was %p with verbs %p. ListenAddr: %p\n",
		      event->listen_id, event->listen_id->verbs, port);

	/* Figure out the supported queue depth. This is a multi-step process
	 * that takes into account hardware maximums, host provided values,
	 * and our target's internal memory limits */

	SPDK_DEBUGLOG(rdma, "Calculating Queue Depth\n");

	/* Start with the maximum queue depth allowed by the target */
	max_queue_depth = rtransport->transport.opts.max_queue_depth;
	max_read_depth = rtransport->transport.opts.max_queue_depth;
	SPDK_DEBUGLOG(rdma, "Target Max Queue Depth: %d\n",
		      rtransport->transport.opts.max_queue_depth);

	/* Next check the local NIC's hardware limitations */
	SPDK_DEBUGLOG(rdma,
		      "Local NIC Max Send/Recv Queue Depth: %d Max Read/Write Queue Depth: %d\n",
		      port->device->attr.max_qp_wr, port->device->attr.max_qp_rd_atom);
	max_queue_depth = spdk_min(max_queue_depth, port->device->attr.max_qp_wr);
	max_read_depth = spdk_min(max_read_depth, port->device->attr.max_qp_init_rd_atom);

	/* Next check the remote NIC's hardware limitations */
	SPDK_DEBUGLOG(rdma,
		      "Host (Initiator) NIC Max Incoming RDMA R/W operations: %d Max Outgoing RDMA R/W operations: %d\n",
		      rdma_param->initiator_depth, rdma_param->responder_resources);
	/* from man3 rdma_get_cm_event
	 * responder_resources - Specifies the number of responder resources that is requested by the recipient.
	 * The responder_resources field must match the initiator depth specified by the remote node when running
	 * the rdma_connect and rdma_accept functions. */
	if (rdma_param->responder_resources != 0) {
		if (private_data->qid) {
			SPDK_DEBUGLOG(rdma, "Host (Initiator) is not allowed to use RDMA operations,"
				      " responder_resources must be 0 but set to %u\n",
				      rdma_param->responder_resources);
		} else {
			SPDK_WARNLOG("Host (Initiator) is not allowed to use RDMA operations,"
				     " responder_resources must be 0 but set to %u\n",
				     rdma_param->responder_resources);
		}
	}
	/* from man3 rdma_get_cm_event
	 * initiator_depth - Specifies the maximum number of outstanding RDMA read operations that the recipient holds.
	 * The initiator_depth field must match the responder resources specified by the remote node when running
	 * the rdma_connect and rdma_accept functions. */
	if (rdma_param->initiator_depth == 0) {
		SPDK_ERRLOG("Host (Initiator) doesn't support RDMA_READ or atomic operations\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_IRD);
		return -1;
	}
	max_read_depth = spdk_min(max_read_depth, rdma_param->initiator_depth);

	SPDK_DEBUGLOG(rdma, "Host Receive Queue Size: %d\n", private_data->hrqsize);
	SPDK_DEBUGLOG(rdma, "Host Send Queue Size: %d\n", private_data->hsqsize);
	max_queue_depth = spdk_min(max_queue_depth, private_data->hrqsize);
	max_queue_depth = spdk_min(max_queue_depth, private_data->hsqsize + 1);

	SPDK_DEBUGLOG(rdma, "Final Negotiated Queue Depth: %d R/W Depth: %d\n",
		      max_queue_depth, max_read_depth);

	rqpair = calloc(1, sizeof(struct spdk_nvmf_rdma_qpair));
	if (rqpair == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
		return -1;
	}

	rqpair->device = port->device;
	rqpair->max_queue_depth = max_queue_depth;
	rqpair->max_read_depth = max_read_depth;
	rqpair->cm_id = event->id;
	rqpair->listen_id = event->listen_id;
	rqpair->qpair.transport = transport;
	/* use qid from the private data to determine the qpair type
	   qid will be set to the appropriate value when the controller is created */
	rqpair->qpair.qid = private_data->qid;
	rqpair->qpair.numa.id_valid = 1;
	rqpair->qpair.numa.id = spdk_rdma_cm_id_get_numa_id(rqpair->cm_id);

	event->id->context = &rqpair->qpair;

	spdk_nvmf_tgt_new_qpair(transport->tgt, &rqpair->qpair);

	nvmf_rdma_trid_from_cm_id(rqpair->listen_id, &rqpair->listen_trid, false);

	return 0;
}

/*
 * [한국어]
 * nvmf_rdma_setup_wr - ibv_send_wr 의 opcode/send_flags/next 를 xfer 방향에 따라 설정.
 *
 * @wr: 설정할 SEND WR.
 * @next: 이 WR 다음에 연결할 WR (chain tail 이면 rsp.wr).
 * @xfer: 데이터 전송 방향 (CONTROLLER_TO_HOST=read=RDMA_WRITE / HOST_TO_CONTROLLER=write=RDMA_READ).
 * @return: void.
 *
 * NVMe 관점의 xfer 방향과 RDMA WR opcode 의 대응:
 *   - NVMe read (ctrl→host) = IBV_WR_RDMA_WRITE (controller 가 host 메모리에 기록).
 *     send_flags=0 (SIGNALED 없음 — RDMA WRITE 완료는 chain 의 마지막 SEND CQE 로 확인).
 *   - NVMe write (host→ctrl) = IBV_WR_RDMA_READ (controller 가 host 메모리에서 읽어옴).
 *     send_flags=IBV_SEND_SIGNALED (CQE 생성 필요 — num_outstanding_data_wr 감소 트리거).
 *
 * 실행 컨텍스트: nvmf_rdma_setup_request() / nvmf_request_alloc_wrs() — reactor 스레드.
 *
 * 호출 체인:
 *   nvmf_rdma_setup_request() → [nvmf_rdma_setup_wr]
 *   nvmf_request_alloc_wrs() → [nvmf_rdma_setup_wr]
 */
static inline void
nvmf_rdma_setup_wr(struct ibv_send_wr *wr, struct ibv_send_wr *next,
		   enum spdk_nvme_data_transfer xfer)
{
	if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		/* [한국어] NVMe read: controller → host 방향. RDMA WRITE opcode.
		 * send_flags=0: SIGNALED 없음 — 마지막 SEND CQE 가 data 완료를 암시. */
		wr->opcode = IBV_WR_RDMA_WRITE;
		wr->send_flags = 0;
		wr->next = next;  /* [한국어] read 방향: next WR 으로 체인 연결. */
	} else if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		/* [한국어] NVMe write: host → controller 방향. RDMA READ opcode.
		 * IBV_SEND_SIGNALED: 각 RDMA READ WR 완료마다 CQE 생성 — num_outstanding 감소용. */
		wr->opcode = IBV_WR_RDMA_READ;
		wr->send_flags = IBV_SEND_SIGNALED;
		wr->next = NULL;  /* [한국어] write 방향: 각 WR 이 독립적 — chain 없음. */
	} else {
		/* [한국어] 양방향 없는 방향 (BIDIRECTIONAL) 은 RDMA WR 로 표현 불가 — bug. */
		assert(0);
	}
}

/*
 * [한국어]
 * nvmf_request_alloc_wrs - multi-SGL 데이터 전송에 필요한 추가 WR 들을 mempool 에서 할당.
 *
 * @rtransport: data_wr_pool 에 접근하기 위한 transport.
 * @rdma_req: WR chain 을 추가할 request.
 * @num_sgl_descriptors: 추가로 필요한 WR 수 (embedded data.wr 이후 필요한 extra WR 수).
 * @return: 0 성공, -EINVAL (초과), -ENOMEM (pool 부족).
 *
 * 단일 WR 의 SGE 배열(SPDK_NVMF_MAX_SGL_ENTRIES=16개)로 전체 payload 를 담을 수 없을 때
 * 추가 WR 들을 data_wr_pool 에서 할당하여 embedded data.wr 에 chain 으로 연결.
 *
 * 결과 WR chain 구조:
 *   rdma_req->data.wr → work_requests[0]->wr → ... → work_requests[N-1]->wr → rsp.wr
 * 각 WR 의 wr_id = rdma_req->data.wr.wr_id (CQE 에서 type tag 복원용).
 *
 * 실행 컨텍스트: nvmf_rdma_request_fill_iovs() / nvmf_rdma_request_fill_iovs_multi_sgl() — reactor 스레드.
 *
 * 호출 체인:
 *   nvmf_rdma_request_fill_iovs() → [nvmf_request_alloc_wrs]
 *     → spdk_mempool_get_bulk (data_wr_pool)
 *     → nvmf_rdma_setup_wr (각 WR opcode/flags/next 설정)
 */
static int
nvmf_request_alloc_wrs(struct spdk_nvmf_rdma_transport *rtransport,
		       struct spdk_nvmf_rdma_request *rdma_req,
		       uint32_t num_sgl_descriptors)
{
	struct spdk_nvmf_rdma_request_data	*work_requests[SPDK_NVMF_MAX_SGL_ENTRIES];  /* [한국어] 할당받은 extra WR 임시 배열. */
	struct spdk_nvmf_rdma_request_data	*current_data_wr;  /* [한국어] chain 구성 중인 현재 WR 포인터. */
	uint32_t				i;

	/* [한국어] 요청 WR 수가 배열 한도를 초과하면 즉시 거절. */
	if (spdk_unlikely(num_sgl_descriptors > SPDK_NVMF_MAX_SGL_ENTRIES)) {
		SPDK_ERRLOG("Requested too much entries (%u), the limit is %u\n",
			    num_sgl_descriptors, SPDK_NVMF_MAX_SGL_ENTRIES);
		return -EINVAL;
	}

	/* [한국어] data_wr_pool 에서 일괄 할당 — pool 부족 시 -ENOMEM. */
	if (spdk_unlikely(spdk_mempool_get_bulk(rtransport->data_wr_pool, (void **)work_requests,
						num_sgl_descriptors))) {
		return -ENOMEM;
	}

	/* [한국어] chain 구성 시작 — embedded data.wr 이 선두. */
	current_data_wr = &rdma_req->data;

	/* [한국어] 각 extra WR 를 현재 WR 에 체인 연결 + WR 초기화. */
	for (i = 0; i < num_sgl_descriptors; i++) {
		nvmf_rdma_setup_wr(&current_data_wr->wr, &work_requests[i]->wr, rdma_req->req.xfer);
		current_data_wr->wr.next = &work_requests[i]->wr;  /* [한국어] 체인 연결. */
		current_data_wr = work_requests[i];  /* [한국어] 다음 WR 로 이동. */
		current_data_wr->wr.sg_list = current_data_wr->sgl;  /* [한국어] SGE 배열 포인터 설정. */
		current_data_wr->wr.wr_id = rdma_req->data.wr.wr_id;  /* [한국어] wr_id 통일 — CQE type tag 복원. */
	}

	/* [한국어] 마지막 WR 의 next = rsp.wr (RDMA WRITE chain 끝에 SEND 연결). */
	nvmf_rdma_setup_wr(&current_data_wr->wr, &rdma_req->rsp.wr, rdma_req->req.xfer);

	return 0;  /* [한국어] 성공. */
}

/*
 * [한국어]
 * nvmf_rdma_setup_request - 단순 SGL 의 request 에 대해 embedded data.wr 초기 설정.
 *
 * @rdma_req: 설정할 request (NVMe command 의 SGL1 이 KEYED_SGL 타입 전제).
 * @return: void.
 *
 * NVMe command 의 dptr.sgl1 (첫 번째 SGL descriptor) 에서 rkey/remote_addr 를 읽어
 * data.wr 의 RDMA remote 측 정보를 설정. 단일 WR 로 전체 payload 를 처리할 수 있는
 * 간단한 경우(단순 SGL)에 사용.
 *
 * rkey(Remote Key): host 의 MR(Memory Region)에 대한 접근 키.
 * remote_addr: host 메모리에서 RDMA READ/WRITE 할 시작 주소.
 *
 * 실행 컨텍스트: nvmf_rdma_request_fill_iovs() / parse_sgl() — reactor 스레드.
 *
 * 호출 체인:
 *   nvmf_rdma_request_fill_iovs() → [nvmf_rdma_setup_request] → nvmf_rdma_setup_wr
 */
static inline void
nvmf_rdma_setup_request(struct spdk_nvmf_rdma_request *rdma_req)
{
	struct ibv_send_wr		*wr = &rdma_req->data.wr;  /* [한국어] embedded data WR 포인터. */
	struct spdk_nvme_sgl_descriptor	*sgl = &rdma_req->req.cmd->nvme_cmd.dptr.sgl1;  /* [한국어] 명령의 첫 SGL descriptor. */

	/* [한국어] SGL descriptor 의 keyed.key → RDMA WR 의 rkey (Remote Key). */
	wr->wr.rdma.rkey = sgl->keyed.key;
	/* [한국어] SGL descriptor 의 address → RDMA WR 의 remote_addr (host 메모리 주소). */
	wr->wr.rdma.remote_addr = sgl->address;
	/* [한국어] opcode/send_flags/next 설정 (방향에 따라 WRITE 또는 READ). */
	nvmf_rdma_setup_wr(wr, &rdma_req->rsp.wr, rdma_req->req.xfer);
}

/*
 * [한국어]
 * nvmf_rdma_update_remote_addr - multi-WR chain 의 각 WR 에 remote_addr 를 순차적으로 갱신.
 *
 * @rdma_req: WR chain 이 구성된 request (data.wr 선두).
 * @num_wrs: 업데이트할 WR 수 (chain 길이).
 * @return: void.
 *
 * fill_wr_sgl 이 로컬 SGE 를 채운 후, 각 WR 가 처리하는 데이터 크기만큼 remote_addr 를
 * 증가시켜 remote 주소를 연속 메모리처럼 매핑. rkey 는 동일한 MR 이므로 동일 값 복사.
 *
 * remote_addr_offset 계산: WR N 의 remote_addr = sgl.address + Σ(WR0..N-1 의 총 SGE 길이).
 *
 * 실행 컨텍스트: nvmf_rdma_request_fill_iovs() — reactor 스레드.
 *
 * 호출 체인:
 *   nvmf_rdma_request_fill_iovs() → fill_wr_sgl → [nvmf_rdma_update_remote_addr]
 */
static inline void
nvmf_rdma_update_remote_addr(struct spdk_nvmf_rdma_request *rdma_req, uint32_t num_wrs)
{
	struct ibv_send_wr		*wr = &rdma_req->data.wr;  /* [한국어] chain 선두 WR. */
	struct spdk_nvme_sgl_descriptor	*sgl = &rdma_req->req.cmd->nvme_cmd.dptr.sgl1;  /* [한국어] 명령의 SGL1. */
	uint32_t			i;     /* [한국어] WR 순회 카운터. */
	int				j;     /* [한국어] SGE 순회 카운터. */
	uint64_t			remote_addr_offset = 0;  /* [한국어] 이전 WR 들이 커버한 총 바이트. */

	/* [한국어] 각 WR 에 rkey 복사 + 현재 offset 기반 remote_addr 설정. */
	for (i = 0; i < num_wrs; ++i) {
		wr->wr.rdma.rkey = sgl->keyed.key;  /* [한국어] 동일 MR — rkey 동일. */
		wr->wr.rdma.remote_addr = sgl->address + remote_addr_offset;  /* [한국어] offset 적용 remote 주소. */
		/* [한국어] 이 WR 의 모든 SGE 길이 합산 → 다음 WR 의 offset. */
		for (j = 0; j < wr->num_sge; ++j) {
			remote_addr_offset += wr->sg_list[j].length;
		}
		wr = wr->next;  /* [한국어] 다음 WR 로 이동. */
	}
}

/*
 * [한국어]
 * nvmf_rdma_fill_wr_sgl - req->iov[] 의 버퍼 영역들을 ibv_send_wr 의 SGE 배열로 채움.
 *
 * @device: RDMA device (메모리 맵 조회용).
 * @rdma_req: SGE 를 채울 request (iovpos/offset 상태 참조).
 * @wr: SGE 를 채울 ibv_send_wr.
 * @total_length: 이 WR 가 커버해야 할 총 바이트.
 * @return: 0 성공, 음수 errno (메모리 변환 실패 또는 SGE 부족).
 *
 * iov 배열을 순회하며 각 iov entry 를 SGE 로 변환:
 *   - spdk_rdma_utils_get_translation: DMA 메모리 맵 조회 → lkey(로컬 접근 키) 획득.
 *   - SGE.addr = iov.iov_base + offset (이전 WR 이 처리한 offset 에서 시작).
 *   - SGE.length = min(iov 나머지, total_length).
 *   - iov 완전 소비 시 iovpos++ / offset=0.
 *
 * 처리 후 iovpos/offset 이 다음 fill 의 시작점으로 갱신 — multi-WR chain 에서 상태 이어받음.
 * num_sge 가 SPDK_NVMF_MAX_SGL_ENTRIES 에 도달 전 total_length 가 남으면 -EINVAL.
 *
 * 실행 컨텍스트: nvmf_rdma_request_fill_iovs() — reactor 스레드.
 *
 * 호출 체인:
 *   nvmf_rdma_request_fill_iovs() → [nvmf_rdma_fill_wr_sgl]
 *     → spdk_rdma_utils_get_translation (lkey 조회)
 */
static int
nvmf_rdma_fill_wr_sgl(struct spdk_nvmf_rdma_device *device,
		      struct spdk_nvmf_rdma_request *rdma_req,
		      struct ibv_send_wr *wr,
		      uint32_t total_length)
{
	struct spdk_rdma_utils_memory_translation mem_translation;  /* [한국어] DMA 주소 변환 결과 (lkey 포함). */
	struct ibv_sge	*sg_ele;  /* [한국어] 현재 채우고 있는 SGE 포인터. */
	struct iovec *iov;        /* [한국어] 현재 처리 중인 iov entry. */
	uint32_t lkey, remaining; /* [한국어] 로컬 접근 키, 이 SGE 에서 처리할 바이트. */
	int rc;

	wr->num_sge = 0;  /* [한국어] SGE 수 초기화. */

	/* [한국어] total_length 가 소진되거나 SGE 배열이 가득 찰 때까지 반복. */
	while (total_length && wr->num_sge < SPDK_NVMF_MAX_SGL_ENTRIES) {
		iov = &rdma_req->req.iov[rdma_req->iovpos];  /* [한국어] 현재 iov entry. */
		/* [한국어] device mem_map 에서 iov 주소의 lkey 조회. */
		rc = spdk_rdma_utils_get_translation(device->map, iov->iov_base, iov->iov_len, &mem_translation);
		if (spdk_unlikely(rc)) {
			return rc;  /* [한국어] 메모리 변환 실패 (MR 미등록 등). */
		}

		lkey = spdk_rdma_utils_memory_translation_get_lkey(&mem_translation);  /* [한국어] lkey 추출. */
		sg_ele = &wr->sg_list[wr->num_sge];  /* [한국어] 다음 빈 SGE 슬롯. */
		/* [한국어] 이 SGE 에서 처리할 크기 = min(iov 나머지, total_length). */
		remaining = spdk_min((uint32_t)iov->iov_len - rdma_req->offset, total_length);

		sg_ele->lkey = lkey;  /* [한국어] 로컬 접근 키 설정. */
		sg_ele->addr = (uintptr_t)iov->iov_base + rdma_req->offset;  /* [한국어] offset 적용 주소. */
		sg_ele->length = remaining;  /* [한국어] 이 SGE 가 커버하는 바이트 수. */
		SPDK_DEBUGLOG(rdma, "sge[%d] %p addr 0x%"PRIx64", len %u\n", wr->num_sge, sg_ele, sg_ele->addr,
			      sg_ele->length);
		rdma_req->offset += sg_ele->length;  /* [한국어] iov 내 offset 진행. */
		total_length -= sg_ele->length;      /* [한국어] 남은 처리 바이트 감소. */
		wr->num_sge++;                       /* [한국어] SGE 수 증가. */

		/* [한국어] iov 를 완전히 소비했으면 다음 iov 로 이동. */
		if (rdma_req->offset == iov->iov_len) {
			rdma_req->offset = 0;   /* [한국어] 새 iov 의 offset 은 0 부터. */
			rdma_req->iovpos++;     /* [한국어] 다음 iov 로 이동. */
		}
	}

	/* [한국어] SGE 배열이 가득 찼는데 아직 total_length 가 남으면 에러. */
	if (spdk_unlikely(total_length)) {
		SPDK_ERRLOG("Not enough SG entries to hold data buffer\n");
		return -EINVAL;
	}

	return 0;  /* [한국어] 성공. */
}

static int
nvmf_rdma_fill_wr_sgl_with_dif(struct spdk_nvmf_rdma_device *device,
			       struct spdk_nvmf_rdma_request *rdma_req,
			       struct ibv_send_wr *wr,
			       uint32_t total_length,
			       uint32_t num_extra_wrs)
{
	struct spdk_rdma_utils_memory_translation mem_translation;
	struct spdk_dif_ctx *dif_ctx = &rdma_req->req.dif.dif_ctx;
	struct ibv_sge *sg_ele;
	struct iovec *iov;
	struct iovec *rdma_iov;
	uint32_t lkey, remaining;
	uint32_t remaining_data_block, data_block_size, md_size;
	uint32_t sge_len;
	int rc;

	data_block_size = dif_ctx->block_size - dif_ctx->md_size;

	if (spdk_likely(!rdma_req->req.stripped_data)) {
		rdma_iov = rdma_req->req.iov;
		remaining_data_block = data_block_size;
		md_size = dif_ctx->md_size;
	} else {
		rdma_iov = rdma_req->req.stripped_data->iov;
		total_length = total_length / dif_ctx->block_size * data_block_size;
		remaining_data_block = total_length;
		md_size = 0;
	}

	wr->num_sge = 0;

	while (total_length && (num_extra_wrs || wr->num_sge < SPDK_NVMF_MAX_SGL_ENTRIES)) {
		iov = rdma_iov + rdma_req->iovpos;
		rc = spdk_rdma_utils_get_translation(device->map, iov->iov_base, iov->iov_len, &mem_translation);
		if (spdk_unlikely(rc)) {
			return rc;
		}

		lkey = spdk_rdma_utils_memory_translation_get_lkey(&mem_translation);
		sg_ele = &wr->sg_list[wr->num_sge];
		remaining = spdk_min((uint32_t)iov->iov_len - rdma_req->offset, total_length);

		while (remaining) {
			if (wr->num_sge >= SPDK_NVMF_MAX_SGL_ENTRIES) {
				if (num_extra_wrs > 0 && wr->next) {
					wr = wr->next;
					wr->num_sge = 0;
					sg_ele = &wr->sg_list[wr->num_sge];
					num_extra_wrs--;
				} else {
					break;
				}
			}
			sg_ele->lkey = lkey;
			sg_ele->addr = (uintptr_t)((char *)iov->iov_base + rdma_req->offset);
			sge_len = spdk_min(remaining, remaining_data_block);
			sg_ele->length = sge_len;
			SPDK_DEBUGLOG(rdma, "sge[%d] %p addr 0x%"PRIx64", len %u\n", wr->num_sge, sg_ele,
				      sg_ele->addr, sg_ele->length);
			remaining -= sge_len;
			remaining_data_block -= sge_len;
			rdma_req->offset += sge_len;
			total_length -= sge_len;

			sg_ele++;
			wr->num_sge++;

			if (remaining_data_block == 0) {
				/* skip metadata */
				rdma_req->offset += md_size;
				total_length -= md_size;
				/* Metadata that do not fit this IO buffer will be included in the next IO buffer */
				remaining -= spdk_min(remaining, md_size);
				remaining_data_block = data_block_size;
			}

			if (remaining == 0) {
				/* By subtracting the size of the last IOV from the offset, we ensure that we skip
				   the remaining metadata bits at the beginning of the next buffer */
				rdma_req->offset -= spdk_min(iov->iov_len, rdma_req->offset);
				rdma_req->iovpos++;
			}
		}
	}

	if (spdk_unlikely(total_length)) {
		SPDK_ERRLOG("Not enough SG entries to hold data buffer\n");
		return -EINVAL;
	}

	return 0;
}

static inline uint32_t
nvmf_rdma_calc_num_wrs(uint32_t length, uint32_t io_unit_size, uint32_t block_size)
{
	/* estimate the number of SG entries and WRs needed to process the request */
	uint32_t num_sge = 0;
	uint32_t i;
	uint32_t num_buffers = SPDK_CEIL_DIV(length, io_unit_size);

	for (i = 0; i < num_buffers && length > 0; i++) {
		uint32_t buffer_len = spdk_min(length, io_unit_size);
		uint32_t num_sge_in_block = SPDK_CEIL_DIV(buffer_len, block_size);

		if (num_sge_in_block * block_size > buffer_len) {
			++num_sge_in_block;
		}
		num_sge += num_sge_in_block;
		length -= buffer_len;
	}
	return SPDK_CEIL_DIV(num_sge, SPDK_NVMF_MAX_SGL_ENTRIES);
}

/*
 * [한국어]
 * nvmf_rdma_request_fill_iovs - bdev iobuf 의 chunk 들을 ibv_sge[] 로 변환 + lkey 등록.
 *
 * @rtransport: transport.
 * @device: RDMA device (memory map 등록 컨텍스트).
 * @rdma_req: 처리할 request.
 * @length: 전송 데이터 길이.
 * @return: 0 성공, 음수 errno.
 *
 * parse_sgl 이 get_buffers 로 iobuf 빌리기 성공한 후 호출. iobuf 의 4KB chunks 가
 * req->iov[] 에 들어 있고, 본 함수가 그것을 RDMA WR 의 ibv_sge[] 로 변환 + 각 영역의
 * lkey (local key) 등록.
 *
 * 단계:
 *   1. DIF + read 방향 + multi-block 이면 stripped_buffers 추가 할당 (DIF strip 용 임시 버퍼).
 *      실패 시 fallback (req->iov 자체 사용).
 *   2. DIF enabled → calc_num_wrs 로 필요한 WR 수 계산 (block_size 단위로 split).
 *      multiple WR 시 추가 WR mempool 에서 빌림 (data_wr_pool).
 *      fill_wr_sgl_with_dif: PRACT/PRCHK 비트 처리 포함한 sgl 채우기.
 *   3. DIF 미사용 (일반 경로) → 단순 fill_wr_sgl.
 *   4. num_outstanding_data_wr 설정 = poller_poll 의 RDMA op 완료 카운트와 매칭.
 *
 * err_exit: 실패 시 빌린 iobuf + WR mempool 자원 모두 반환 — 누수 방지.
 *
 * DIF 처리 비대칭:
 *   - Write (host→ctrl): host SGL 에 PI 가 인터리브 — RDMA READ 후 bdev 가 검증.
 *   - Read (ctrl→host) multi-block: bdev 가 PI strip 한 영역을 stripped_buffer 에 받은 후
 *     re-insert 해서 RDMA WRITE.
 *
 * 호출 체인:
 *   request_parse_sgl → [본 함수] → fill_wr_sgl(_with_dif) → spdk_rdma_utils_get_translation
 *     (lkey 룩업/등록)
 */
static int
nvmf_rdma_request_fill_iovs(struct spdk_nvmf_rdma_transport *rtransport,
			    struct spdk_nvmf_rdma_device *device,
			    struct spdk_nvmf_rdma_request *rdma_req,
			    uint32_t length)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_request		*req = &rdma_req->req;
	struct ibv_send_wr			*wr = &rdma_req->data.wr;
	int					rc = 0;
	uint32_t				num_wrs = 1;

	rqpair = SPDK_CONTAINEROF(req->qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rgroup = rqpair->poller->group;

	/* [한국어] max_send_sge 초과 = host 가 보낸 SGL 이 NIC 한도 초과. parse 단계에서 막혀야 함. */
	assert(req->iovcnt <= rqpair->max_send_sge);

	/* When dif_insert_or_strip is true and the I/O data length is greater than one block,
	 * the stripped_buffers are got for DIF stripping. */
	/* [한국어] DIF strip 시 임시 buffer 필요 — bdev 가 PI 제거한 데이터를 받을 영역.
	 * 할당 실패 시 그냥 req.iov 재사용 (fallback, 성능 약간 떨어짐). */
	if (spdk_unlikely(req->dif_enabled && (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST)
			  && (req->dif.elba_length > req->dif.dif_ctx.block_size))) {
		rc = nvmf_request_get_stripped_buffers(req, &rgroup->group,
						       &rtransport->transport, req->dif.orig_length);
		if (rc != 0) {
			SPDK_INFOLOG(rdma, "Get stripped buffers fail %d, fallback to req.iov.\n", rc);
		}
	}

	rdma_req->iovpos = 0;

	if (spdk_unlikely(req->dif_enabled)) {
		/* [한국어] DIF 경로: block_size 단위로 SGL split 필요. block 경계 가로지르는 single
		 * RDMA WR 불가 → calc 로 WR 수 결정. */
		num_wrs = nvmf_rdma_calc_num_wrs(length, rtransport->transport.opts.io_unit_size,
						 req->dif.dif_ctx.block_size);
		if (num_wrs > 1) {
			/* [한국어] data_wr_pool 에서 추가 WR (num_wrs-1) 개 빌림 — chained list. */
			rc = nvmf_request_alloc_wrs(rtransport, rdma_req, num_wrs - 1);
			if (spdk_unlikely(rc != 0)) {
				goto err_exit;
			}
		}

		/* [한국어] PRACT/PRCHK bit (cmd->dword12 의 PI flags) 와 block_size 정렬해 SGL 빌드. */
		rc = nvmf_rdma_fill_wr_sgl_with_dif(device, rdma_req, wr, length, num_wrs - 1);
		if (spdk_unlikely(rc != 0)) {
			goto err_exit;
		}

		if (num_wrs > 1) {
			/* [한국어] split 된 각 WR 의 remote address 누적 갱신 (호스트 메모리 offset). */
			nvmf_rdma_update_remote_addr(rdma_req, num_wrs);
		}
	} else {
		/* [한국어] 일반 경로 — 단일 WR, iov 그대로 SGL 매핑. */
		rc = nvmf_rdma_fill_wr_sgl(device, rdma_req, wr, length);
		if (spdk_unlikely(rc != 0)) {
			goto err_exit;
		}
	}

	/* set the number of outstanding data WRs for this request. */
	/* [한국어] poller_poll 이 RDMA_READ/WRITE 완료 카운트와 매칭 — 모두 도착할 때까지 다음 state 진행 X. */
	rdma_req->num_outstanding_data_wr = num_wrs;

	return rc;

err_exit:
	/* [한국어] 실패 시 빌린 자원 일괄 반환 — iobuf + 추가 WR 들. */
	spdk_nvmf_request_free_buffers(req, &rgroup->group, &rtransport->transport);
	nvmf_rdma_request_free_data(rdma_req, rtransport);
	req->iovcnt = 0;
	return rc;
}

static int
nvmf_rdma_request_fill_iovs_multi_sgl(struct spdk_nvmf_rdma_transport *rtransport,
				      struct spdk_nvmf_rdma_device *device,
				      struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct ibv_send_wr			*current_wr;
	struct spdk_nvmf_request		*req = &rdma_req->req;
	struct spdk_nvme_sgl_descriptor		*inline_segment, *desc;
	uint32_t				num_sgl_descriptors;
	uint32_t				lengths[SPDK_NVMF_MAX_SGL_ENTRIES], total_length = 0;
	uint32_t				i;
	int					rc;

	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rgroup = rqpair->poller->group;

	inline_segment = &req->cmd->nvme_cmd.dptr.sgl1;
	assert(inline_segment->generic.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT);
	assert(inline_segment->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);

	num_sgl_descriptors = inline_segment->unkeyed.length / sizeof(struct spdk_nvme_sgl_descriptor);
	assert(num_sgl_descriptors <= SPDK_NVMF_MAX_SGL_ENTRIES);

	desc = (struct spdk_nvme_sgl_descriptor *)rdma_req->recv->buf + inline_segment->address;
	for (i = 0; i < num_sgl_descriptors; i++) {
		if (spdk_likely(!req->dif_enabled)) {
			lengths[i] = desc->keyed.length;
		} else {
			req->dif.orig_length += desc->keyed.length;
			lengths[i] = spdk_dif_get_length_with_md(desc->keyed.length, &req->dif.dif_ctx);
			req->dif.elba_length += lengths[i];
		}
		total_length += lengths[i];
		desc++;
	}

	if (spdk_unlikely(total_length > rtransport->transport.opts.max_io_size)) {
		SPDK_ERRLOG("Multi SGL length 0x%x exceeds max io size 0x%x\n",
			    total_length, rtransport->transport.opts.max_io_size);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return -EINVAL;
	}

	rc = nvmf_request_alloc_wrs(rtransport, rdma_req, num_sgl_descriptors - 1);
	if (spdk_unlikely(rc != 0)) {
		return -ENOMEM;
	}

	rc = spdk_nvmf_request_get_buffers(req, &rgroup->group, &rtransport->transport, total_length);
	if (spdk_unlikely(rc != 0)) {
		nvmf_rdma_request_free_data(rdma_req, rtransport);
		return rc;
	}

	/* When dif_insert_or_strip is true and the I/O data length is greater than one block,
	 * the stripped_buffers are got for DIF stripping. */
	if (spdk_unlikely(req->dif_enabled && (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST)
			  && (req->dif.elba_length > req->dif.dif_ctx.block_size))) {
		rc = nvmf_request_get_stripped_buffers(req, &rgroup->group,
						       &rtransport->transport, req->dif.orig_length);
		if (spdk_unlikely(rc != 0)) {
			SPDK_INFOLOG(rdma, "Get stripped buffers fail %d, fallback to req.iov.\n", rc);
		}
	}

	/* The first WR must always be the embedded data WR. This is how we unwind them later. */
	current_wr = &rdma_req->data.wr;
	assert(current_wr != NULL);

	req->length = 0;
	rdma_req->iovpos = 0;
	desc = (struct spdk_nvme_sgl_descriptor *)rdma_req->recv->buf + inline_segment->address;
	for (i = 0; i < num_sgl_descriptors; i++) {
		/* The descriptors must be keyed data block descriptors with an address, not an offset. */
		if (spdk_unlikely(desc->generic.type != SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK ||
				  desc->keyed.subtype != SPDK_NVME_SGL_SUBTYPE_ADDRESS)) {
			rc = -EINVAL;
			goto err_exit;
		}

		if (spdk_likely(!req->dif_enabled)) {
			rc = nvmf_rdma_fill_wr_sgl(device, rdma_req, current_wr, lengths[i]);
		} else {
			rc = nvmf_rdma_fill_wr_sgl_with_dif(device, rdma_req, current_wr,
							    lengths[i], 0);
		}
		if (spdk_unlikely(rc != 0)) {
			rc = -ENOMEM;
			goto err_exit;
		}

		req->length += desc->keyed.length;
		current_wr->wr.rdma.rkey = desc->keyed.key;
		current_wr->wr.rdma.remote_addr = desc->address;
		current_wr = current_wr->next;
		desc++;
	}

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
	/* Go back to the last descriptor in the list. */
	desc--;
	if ((device->attr.device_cap_flags & IBV_DEVICE_MEM_MGT_EXTENSIONS) != 0) {
		if (desc->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY) {
			rdma_req->rsp.wr.opcode = IBV_WR_SEND_WITH_INV;
			rdma_req->rsp.wr.imm_data = desc->keyed.key;
		}
	}
#endif

	rdma_req->num_outstanding_data_wr = num_sgl_descriptors;

	return 0;

err_exit:
	spdk_nvmf_request_free_buffers(req, &rgroup->group, &rtransport->transport);
	nvmf_rdma_request_free_data(rdma_req, rtransport);
	return rc;
}

static inline int
nvmf_rdma_request_parse_icd(struct spdk_nvmf_rdma_transport *rtransport,
			    struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_request *req = &rdma_req->req;
	struct spdk_nvme_sgl_descriptor *sgl = &req->cmd->nvme_cmd.dptr.sgl1;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t offset = sgl->address;
	uint32_t max_len = rtransport->transport.opts.in_capsule_data_size;

	assert(sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
	       sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	SPDK_DEBUGLOG(nvmf, "In-capsule data: offset 0x%" PRIx64 ", length 0x%x\n",
		      offset, sgl->unkeyed.length);

	if (spdk_unlikely(offset > max_len)) {
		SPDK_ERRLOG("In-capsule offset 0x%" PRIx64 " exceeds capsule length 0x%x\n",
			    offset, max_len);
		rsp->status.sc = SPDK_NVME_SC_INVALID_SGL_OFFSET;
		return -1;
	}
	max_len -= (uint32_t)offset;

	if (spdk_unlikely(sgl->unkeyed.length > max_len)) {
		SPDK_ERRLOG("In-capsule data length 0x%x exceeds capsule length 0x%x\n",
			    sgl->unkeyed.length, max_len);
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return -1;
	}

	rdma_req->num_outstanding_data_wr = 0;
	req->data_from_pool = false;
	req->length = sgl->unkeyed.length;

	assert(rdma_req->recv != NULL);
	assert(rdma_req->recv->buf != NULL);

	req->iov[0].iov_base = rdma_req->recv->buf + offset;
	req->iov[0].iov_len = req->length;
	req->iovcnt = 1;

	return 0;
}

/*
 * [한국어]
 * nvmf_rdma_request_parse_sgl - NVMe command 의 SGL descriptor 를 파싱해 데이터 전송 경로 결정.
 *
 * @rtransport: transport.
 * @device: RDMA device (lkey/rkey 등록 컨텍스트).
 * @rdma_req: 처리할 request (NEW 상태 진입 직후).
 * @return: 0 = 성공 (HAVE_BUFFER 진행 가능 또는 NEED_BUFFER 로 queued),
 *          -1 = SGL 오류 (cpl.status 에 NVMe 에러 코드 설정됨).
 *
 * SGL Descriptor 3종 (NVMe spec §4.4):
 *   1. **KEYED_DATA_BLOCK + ADDRESS/INVALIDATE_KEY** (★ 대다수): host 메모리 1개 영역.
 *      RDMA READ/WRITE 의 target. keyed.address/length/key = 호스트의 IOVA/길이/rkey.
 *      INVALIDATE_KEY subtype: 응답 SEND 시 SEND_WITH_INV 로 자동 invalidate (성능 최적화).
 *   2. **LAST_SEGMENT + OFFSET** (multi-SGL): 여러 SGL element 포함. 큰 IO 가 16 entries
 *      초과 시. nvmf_rdma_request_fill_iovs_multi_sgl 가 처리.
 *   3. **DATA_BLOCK + OFFSET** (in-capsule data): capsule 자체에 데이터 포함 — write 의
 *      small IO 경로. 본 함수는 처리 안 함 (NEW 단계에서 별도 분기).
 *
 * 핵심 단계 (KEYED 경로):
 *   1. length = sgl->keyed.length, max_io_size 검증.
 *   2. SEND_WITH_INV 지원 device + INVALIDATE_KEY subtype → rsp.wr.opcode 변경.
 *   3. DIF enabled 면 metadata 포함 length 로 확장.
 *   4. spdk_nvmf_request_get_buffers — bdev iobuf 풀에서 length 만큼 빌리기.
 *      실패 시 0 반환 (NEED_BUFFER queue 로 이동).
 *   5. nvmf_rdma_request_fill_iovs — 호스트 SGL → 로컬 iov 매핑.
 *
 * 호출 체인:
 *   request_process (state=NEW) → [본 함수]
 *     → keyed: get_buffers → fill_iovs (단일 SGL)
 *     → multi: fill_iovs_multi_sgl (여러 SGL 순회 + 각각 mempool 빌림)
 */
static int
nvmf_rdma_request_parse_sgl(struct spdk_nvmf_rdma_transport *rtransport,
			    struct spdk_nvmf_rdma_device *device,
			    struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_request		*req = &rdma_req->req;
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvme_cpl			*rsp;
	struct spdk_nvme_sgl_descriptor		*sgl;
	int					rc;
	uint32_t				length;

	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rsp = &req->rsp->nvme_cpl;
	/* [한국어] dptr.sgl1 = command 의 첫 SGL descriptor (16B). NVMe-oF 는 PRP 가 아닌 SGL 만. */
	sgl = &req->cmd->nvme_cmd.dptr.sgl1;

	/* [한국어] 케이스 1: KEYED_DATA_BLOCK + (ADDRESS|INVALIDATE_KEY) — 가장 흔한 경로. */
	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
	    (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS ||
	     sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY)) {

		length = sgl->keyed.length;
		/* [한국어] transport 정책 상한 검증 — DoS 회피. SGL_LENGTH_INVALID 응답. */
		if (spdk_unlikely(length > rtransport->transport.opts.max_io_size)) {
			SPDK_ERRLOG("SGL length 0x%x exceeds max io size 0x%x\n",
				    length, rtransport->transport.opts.max_io_size);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}
#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
		/* [한국어] SEND_WITH_INV 최적화 (MEM_MGT_EXTENSIONS 지원 NIC 한정):
		 * 응답 SEND 자체에 host MR invalidate 신호를 piggyback — host 가 별도 invalidate
		 * 안 해도 됨 → roundtrip 1회 감소. */
		if ((device->attr.device_cap_flags & IBV_DEVICE_MEM_MGT_EXTENSIONS) != 0) {
			if (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY) {
				rdma_req->rsp.wr.opcode = IBV_WR_SEND_WITH_INV;
				rdma_req->rsp.wr.imm_data = sgl->keyed.key;
			}
		}
#endif

		/* fill request length and populate iovs */
		req->length = length;
		/* rdma wr specifics */
		/* [한국어] RDMA WR 의 remote addr/length/rkey 세팅. */
		nvmf_rdma_setup_request(rdma_req);
		/* [한국어] DIF (Data Integrity Field) 활성화 시 metadata 8B/512B 가 데이터 사이에
		 * 삽입되므로 실제 전송 length 가 늘어남 (PI 메타 포함). */
		if (spdk_unlikely(req->dif_enabled)) {
			req->dif.orig_length = length;
			length = spdk_dif_get_length_with_md(length, &req->dif.dif_ctx);
			req->dif.elba_length = length;
		}

		/* [한국어] bdev iobuf 풀에서 length 만큼 빌리기 — pre-allocated 4KB chunks 다수.
		 * 실패 시 NEED_BUFFER 큐로 가서 다른 request 완료를 기다림. */
		rc = spdk_nvmf_request_get_buffers(req, &rqpair->poller->group->group, &rtransport->transport,
						   length);
		if (spdk_unlikely(rc != 0)) {
			/* No available buffers. Queue this request up. */
			SPDK_DEBUGLOG(rdma, "No available large data buffers. Queueing request %p\n", rdma_req);
			return 0;
		}

		/* [한국어] iobuf 의 4KB chunks 를 iov[] 로 매핑 + 각 iov 의 lkey 등록. */
		rc = nvmf_rdma_request_fill_iovs(rtransport, device, rdma_req, length);
		if (spdk_unlikely(rc < 0)) {
			SPDK_ERRLOG("SGL length exceeds the max I/O size\n");
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		SPDK_DEBUGLOG(rdma, "Request %p took %d buffer/s from central pool\n", rdma_req,
			      req->iovcnt);

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		/* [한국어] 케이스 2: LAST_SEGMENT — 큰 IO 로 단일 SGL 한도 (16 entries) 초과. */

		rc = nvmf_rdma_request_fill_iovs_multi_sgl(rtransport, device, rdma_req);
		if (spdk_unlikely(rc == -ENOMEM)) {
			SPDK_DEBUGLOG(rdma, "No available large data buffers. Queueing request %p\n", rdma_req);
			return 0;
		} else if (spdk_unlikely(rc == -EINVAL)) {
			SPDK_ERRLOG("Multi SGL element request length exceeds the max I/O size\n");
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		SPDK_DEBUGLOG(rdma, "Request %p took %d buffer/s from central pool\n", rdma_req,
			      req->iovcnt);

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		/* [한국어] 케이스 3: in-capsule data (ICD) — capsule 자체에 data 포함.
		 * 이 경로는 NEW 단계 이전에 별도 처리되어야 함 (capsule buffer 에서 직접 사용).
		 * 본 함수 도달 = 호출 순서 버그 → assert. */
		SPDK_ERRLOG("Request %p with ICD called in wrong place\n", rdma_req);
		assert(0);
		return -EINVAL;
	}

	/* [한국어] 알 수 없는 SGL type/subtype 조합 — host 가 spec 위반 또는 unsupported 사용. */
	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	rsp->status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	return -1;
}

static void
_nvmf_rdma_request_free(struct spdk_nvmf_rdma_request *rdma_req,
			struct spdk_nvmf_rdma_transport	*rtransport)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_poll_group	*rgroup;

	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	if (rdma_req->req.data_from_pool) {
		rgroup = rqpair->poller->group;

		spdk_nvmf_request_free_buffers(&rdma_req->req, &rgroup->group, &rtransport->transport);
	}
	if (rdma_req->req.stripped_data) {
		nvmf_request_free_stripped_buffers(&rdma_req->req,
						   &rqpair->poller->group->group,
						   &rtransport->transport);
	}
	nvmf_rdma_request_free_data(rdma_req, rtransport);
	rdma_req->req.length = 0;
	rdma_req->req.iovcnt = 0;
	rdma_req->req.raw = 0; /* clear all flags */
	rdma_req->req.cmd_cb_fn = NULL;
	rdma_req->offset = 0;
	rdma_req->fused_failed = false;
	rdma_req->transfer_wr = NULL;
	if (rdma_req->fused_pair) {
		/* This req was part of a valid fused pair, but failed before it got to
		 * READ_TO_EXECUTE state.  This means we need to fail the other request
		 * in the pair, because it is no longer part of a valid pair.  If the pair
		 * already reached READY_TO_EXECUTE state, we need to kick it.
		 */
		rdma_req->fused_pair->fused_failed = true;
		if (rdma_req->fused_pair->state == RDMA_REQUEST_STATE_READY_TO_EXECUTE) {
			nvmf_rdma_request_process(rtransport, rdma_req->fused_pair);
		}
		rdma_req->fused_pair = NULL;
	}
	memset(&rdma_req->req.dif, 0, sizeof(rdma_req->req.dif));

	STAILQ_INSERT_HEAD(&rqpair->resources->free_queue, rdma_req, state_link);
	rqpair->qpair.queue_depth--;
	rdma_req->state = RDMA_REQUEST_STATE_FREE;
	if (rqpair->qpair.queue_depth == 0) {
		assert(TAILQ_ENTRY_ENQUEUED(rqpair, active_link));
		TAILQ_REMOVE_CLEAR(&rqpair->poller->active_qpairs, rqpair, active_link);
	}
}

static void
nvmf_rdma_check_fused_ordering(struct spdk_nvmf_rdma_transport *rtransport,
			       struct spdk_nvmf_rdma_qpair *rqpair,
			       struct spdk_nvmf_rdma_request *rdma_req)
{
	enum spdk_nvme_cmd_fuse last, next;

	last = rqpair->fused_first ? rqpair->fused_first->req.cmd->nvme_cmd.fuse : SPDK_NVME_CMD_FUSE_NONE;
	next = rdma_req->req.cmd->nvme_cmd.fuse;

	assert(last != SPDK_NVME_CMD_FUSE_SECOND);

	if (spdk_likely(last == SPDK_NVME_CMD_FUSE_NONE && next == SPDK_NVME_CMD_FUSE_NONE)) {
		return;
	}

	if (last == SPDK_NVME_CMD_FUSE_FIRST) {
		if (next == SPDK_NVME_CMD_FUSE_SECOND) {
			/* This is a valid pair of fused commands.  Point them at each other
			 * so they can be submitted consecutively once ready to be executed.
			 */
			rqpair->fused_first->fused_pair = rdma_req;
			rdma_req->fused_pair = rqpair->fused_first;
			rqpair->fused_first = NULL;
			return;
		} else {
			/* Mark the last req as failed since it wasn't followed by a SECOND. */
			rqpair->fused_first->fused_failed = true;

			/* If the last req is in READY_TO_EXECUTE state, then call
			 * nvmf_rdma_request_process(), otherwise nothing else will kick it.
			 */
			if (rqpair->fused_first->state == RDMA_REQUEST_STATE_READY_TO_EXECUTE) {
				nvmf_rdma_request_process(rtransport, rqpair->fused_first);
			}

			rqpair->fused_first = NULL;
		}
	}

	if (next == SPDK_NVME_CMD_FUSE_FIRST) {
		/* Set rqpair->fused_first here so that we know to check that the next request
		 * is a SECOND (and to fail this one if it isn't).
		 */
		rqpair->fused_first = rdma_req;
	} else if (next == SPDK_NVME_CMD_FUSE_SECOND) {
		/* Mark this req failed since it ia SECOND and the last one was not a FIRST. */
		rdma_req->fused_failed = true;
	}
}

static void
nvmf_rdma_poll_group_insert_need_buffer_req(struct spdk_nvmf_rdma_poll_group *rgroup,
		struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_request *r;

	/* CONNECT commands have a timeout, so we need to avoid a CONNECT command
	 * from getting buried behind a long list of other non-FABRIC requests
	 * waiting for a buffer. Note that even though the CONNECT command's data is
	 * in-capsule, the request still goes to this STAILQ.
	 */
	if (spdk_likely(rdma_req->req.cmd->nvme_cmd.opc != SPDK_NVME_OPC_FABRIC)) {
		/* This is the most likely case. */
		STAILQ_INSERT_TAIL(&rgroup->group.pending_buf_queue, &rdma_req->req, buf_link);
		return;
	} else {
		/* STAILQ doesn't have INSERT_BEFORE, so we need to either INSERT_HEAD
		 * or INSERT_AFTER. Put it after any other FABRIC commands that are
		 * already in the queue.
		 */
		r = STAILQ_FIRST(&rgroup->group.pending_buf_queue);
		if (r == NULL || r->cmd->nvme_cmd.opc != SPDK_NVME_OPC_FABRIC) {
			STAILQ_INSERT_HEAD(&rgroup->group.pending_buf_queue, &rdma_req->req, buf_link);
			return;
		}
		while (true) {
			struct spdk_nvmf_request *next;

			next = STAILQ_NEXT(r, buf_link);
			if (next == NULL || next->cmd->nvme_cmd.opc != SPDK_NVME_OPC_FABRIC) {
				STAILQ_INSERT_AFTER(&rgroup->group.pending_buf_queue, r, &rdma_req->req, buf_link);
				return;
			}
			r = next;
		}
	}
}

/*
 * [한국어]
 * nvmf_rdma_request_process - NVMe-oF RDMA request 의 16-state machine dispatcher.
 *
 * @rtransport: 전체 RDMA transport (mempool, opts 등).
 * @rdma_req: 진행할 request.
 * @return: true = 한 단계 이상 상태가 전이됨 (호출자가 다른 request 도 진행할 만한 진척).
 *          false = 아무 전이 없음 (대기 상태 그대로).
 *
 * 핵심 dispatcher. ibv_poll_cq 가 RECV/SEND/RDMA READ/WRITE 완료 통지할 때마다 호출되며,
 * do-while 루프로 "전이가 일어나는 한 계속" 진행 (back-to-back 전이 — 예: HAVE_BUFFER →
 * READY_TO_EXECUTE → EXECUTING 까지 한 호출에 처리). prev_state 와 비교해 progress 판정.
 *
 * 핵심 패턴:
 *   1. **에러 fast path**: QP 가 ibv_in_error_state 또는 비활성이면 현재 state 의 대기
 *      큐(STAILQ)에서 빼낸 후 COMPLETED 로 강제 전이 → 자원 회수.
 *   2. **do-while progress loop**: 한 호출이 여러 단계 진행. 예: bdev 완료 콜백이 EXECUTED
 *      로 세팅 → 본 함수가 → DATA_XFER_TO_HOST_PENDING (depth 충분 시 → TRANSFERRING_*
 *      → READY_TO_COMPLETE → COMPLETING → COMPLETED 까지 한 번에).
 *   3. **PENDING 상태 진입 조건**: queue depth (current_read/send_depth) 가 한도 도달
 *      시 PENDING 큐(qpair->pending_rdma_{read,write,send}_queue)에 enqueue. 다른 WR
 *      완료로 depth 회복 시 poller_poll 이 큐 head 부터 다시 본 함수 호출.
 *   4. **state_link 큐 일관성**: PENDING 상태와 큐 멤버십이 1:1 매칭. 에러 fast path 가
 *      STAILQ_REMOVE 정확히 호출해야 dangling 방지.
 *
 * 실행 컨텍스트: reactor 스레드 (poller_poll 의 콜백 안에서). lockless — 같은 qpair 의
 * request 는 항상 같은 reactor 스레드가 처리 (qpair affinity 보장).
 *
 * 호출 체인:
 *   nvmf_rdma_poller_poll → (CQE 처리) → [본 함수]
 *   nvmf_rdma_request_complete (bdev 완료 콜백) → [본 함수]
 *   nvmf_rdma_resources_create → [본 함수] (RECV 도착 시 NEW 진입)
 */
bool
nvmf_rdma_request_process(struct spdk_nvmf_rdma_transport *rtransport,
			  struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvme_cpl		*rsp = &rdma_req->req.rsp->nvme_cpl;
	int				rc;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_nvme_sgl_descriptor *sgl;
	enum spdk_nvmf_rdma_request_state prev_state;
	bool				progress = false;
	int				data_posted;
	uint32_t			num_blocks, num_rdma_reads_available, qdepth;

	/* [한국어] req → rqpair 역추적. SPDK_CONTAINEROF = container_of 매크로
	 * (offsetof 트릭으로 embedding struct 포인터 복원). */
	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	device = rqpair->device;
	rgroup = rqpair->poller->group;

	/* [한국어] FREE 상태에서 본 함수가 불리면 버그 — 풀에서 꺼낸 후에만 진행 가능. */
	assert(rdma_req->state != RDMA_REQUEST_STATE_FREE);

	/* If the queue pair is in an error state, force the request to the completed state
	 * to release resources. */
	/* [한국어] === 에러 fast path ===
	 * QP 가 ibverbs 에러 상태로 전이됐거나 (예: peer disconnect, port down) 비활성화되면
	 * 어떤 state 든 더 진행해도 의미 없음 → COMPLETED 로 직행해 자원 회수.
	 *
	 * 다만 현재 state 가 "어떤 큐에 enqueue 된 상태" 면 그 큐에서 먼저 빼야 함 — 안 그러면
	 * STAILQ 가 free 된 객체를 가리켜 다음 enqueue 시 corruption. switch 가 그 정리. */
	if (spdk_unlikely(rqpair->ibv_in_error_state || !spdk_nvmf_qpair_is_active(&rqpair->qpair))) {
		switch (rdma_req->state) {
		case RDMA_REQUEST_STATE_NEED_BUFFER:
			/* [한국어] iobuf 대기 큐 (poll_group 공용). */
			STAILQ_REMOVE(&rgroup->group.pending_buf_queue, &rdma_req->req, spdk_nvmf_request, buf_link);
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
			/* [한국어] RDMA READ depth 대기 큐. */
			STAILQ_REMOVE(&rqpair->pending_rdma_read_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			if (rdma_req->num_remaining_data_wr) {
				/* Partially sent request is still in the pending_rdma_read_queue,
				 * remove it before completing */
				/* [한국어] multi-SGL split 중에 일부만 발행한 상태 — pending_rdma_read_queue 에도
				 * 여전히 있을 수 있으므로 강제 제거. num_remaining_data_wr=0 으로 추가 처리 차단. */
				rdma_req->num_remaining_data_wr = 0;
				STAILQ_REMOVE(&rqpair->pending_rdma_read_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
			}
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
			/* [한국어] RDMA WRITE depth 대기 큐. */
			STAILQ_REMOVE(&rqpair->pending_rdma_write_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING:
			/* [한국어] SEND depth 대기 큐. */
			STAILQ_REMOVE(&rqpair->pending_rdma_send_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
			break;
		default:
			break;
		}
		rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
	}

	/* The loop here is to allow for several back-to-back state changes. */
	/* [한국어] === 진척 루프 ===
	 * 한 호출이 가능한 한 많은 상태 전이를 처리 — 예: depth 가 충분하면
	 * EXECUTED → DATA_XFER_TO_HOST_PENDING(skip) → TRANSFERRING → READY_TO_COMPLETE_PENDING(skip)
	 * → READY_TO_COMPLETE → COMPLETING → COMPLETED 까지 한 번에 진행.
	 * prev_state == state 면 진척 없음 → 루프 종료. */
	do {
		prev_state = rdma_req->state;

		SPDK_DEBUGLOG(rdma, "Request %p entering state %d\n", rdma_req, prev_state);

		switch (rdma_req->state) {
		case RDMA_REQUEST_STATE_FREE:
			/* Some external code must kick a request into RDMA_REQUEST_STATE_NEW
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_NEW:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_NEW, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair, rqpair->qpair.queue_depth);
			rdma_recv = rdma_req->recv;

			/* The first element of the SGL is the NVMe command */
			rdma_req->req.cmd = (union nvmf_h2c_msg *)rdma_recv->sgl[0].addr;
			memset(rdma_req->req.rsp, 0, sizeof(*rdma_req->req.rsp));
			rdma_req->transfer_wr = &rdma_req->data.wr;

			if (spdk_unlikely(rqpair->ibv_in_error_state || !spdk_nvmf_qpair_is_active(&rqpair->qpair))) {
				rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
				break;
			}

			if (spdk_unlikely(spdk_nvmf_request_get_dif_ctx(&rdma_req->req, &rdma_req->req.dif.dif_ctx))) {
				rdma_req->req.dif_enabled = true;
			}

			nvmf_rdma_check_fused_ordering(rtransport, rqpair, rdma_req);

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
			rdma_req->rsp.wr.opcode = IBV_WR_SEND;
			rdma_req->rsp.wr.imm_data = 0;
#endif

			/* The next state transition depends on the data transfer needs of this request. */
			rdma_req->req.xfer = spdk_nvmf_req_get_xfer(&rdma_req->req);

			if (spdk_unlikely(rdma_req->req.xfer == SPDK_NVME_DATA_BIDIRECTIONAL)) {
				rsp->status.sct = SPDK_NVME_SCT_GENERIC;
				rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
				STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
				SPDK_DEBUGLOG(rdma, "Request %p: invalid xfer type (BIDIRECTIONAL)\n", rdma_req);
				break;
			}

			/* If no data to transfer, ready to execute. */
			if (rdma_req->req.xfer == SPDK_NVME_DATA_NONE) {
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
				break;
			}

			sgl = &rdma_req->req.cmd->nvme_cmd.dptr.sgl1;
			if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
			    sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
				/* In-capsule data, no need to get a buffer */
				rc = nvmf_rdma_request_parse_icd(rtransport, rdma_req);
				if (spdk_unlikely(rc < 0)) {
					STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, rdma_req, state_link);
					rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
					break;
				}
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
				break;
			}
			rdma_req->state = RDMA_REQUEST_STATE_NEED_BUFFER;
			nvmf_rdma_poll_group_insert_need_buffer_req(rgroup, rdma_req);
			break;
		case RDMA_REQUEST_STATE_NEED_BUFFER:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_NEED_BUFFER, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			assert(rdma_req->req.xfer != SPDK_NVME_DATA_NONE);

			if (&rdma_req->req != STAILQ_FIRST(&rgroup->group.pending_buf_queue)) {
				/* This request needs to wait in line to obtain a buffer */
				break;
			}

			/* Try to get a data buffer */
			rc = nvmf_rdma_request_parse_sgl(rtransport, device, rdma_req);
			if (spdk_unlikely(rc < 0)) {
				STAILQ_REMOVE_HEAD(&rgroup->group.pending_buf_queue, buf_link);
				STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
				break;
			}

			if (rdma_req->req.iovcnt == 0) {
				/* No buffers available. */
				rgroup->stat.pending_data_buffer++;
				break;
			}

			STAILQ_REMOVE_HEAD(&rgroup->group.pending_buf_queue, buf_link);
			rdma_req->state = RDMA_REQUEST_STATE_HAVE_BUFFER;
			break;
		case RDMA_REQUEST_STATE_HAVE_BUFFER:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_HAVE_BUFFER, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			/* If data is transferring from host to controller and the data didn't
			 * arrive using in capsule data, we need to do a transfer from the host.
			 */
			if (rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER &&
			    rdma_req->req.data_from_pool) {
				STAILQ_INSERT_TAIL(&rqpair->pending_rdma_read_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING;
				break;
			}

			rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (rdma_req != STAILQ_FIRST(&rqpair->pending_rdma_read_queue)) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}
			assert(rqpair->max_send_depth >= rqpair->current_send_depth);
			qdepth = rqpair->max_send_depth - rqpair->current_send_depth;
			assert(rqpair->max_read_depth >= rqpair->current_read_depth);
			num_rdma_reads_available = rqpair->max_read_depth - rqpair->current_read_depth;
			if (rdma_req->num_outstanding_data_wr > qdepth ||
			    rdma_req->num_outstanding_data_wr > num_rdma_reads_available) {
				if (num_rdma_reads_available && qdepth) {
					/* Send as much as we can */
					request_prepare_transfer_in_part(&rdma_req->req, spdk_min(num_rdma_reads_available, qdepth));
				} else {
					/* We can only have so many WRs outstanding. we have to wait until some finish. */
					rqpair->poller->stat.pending_rdma_read++;
					break;
				}
			}

			/* We have already verified that this request is the head of the queue. */
			if (rdma_req->num_remaining_data_wr == 0) {
				STAILQ_REMOVE_HEAD(&rqpair->pending_rdma_read_queue, state_link);
			}

			request_transfer_in(&rdma_req->req);
			rdma_req->state = RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER;

			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_READY_TO_EXECUTE:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (spdk_unlikely(rdma_req->req.dif_enabled)) {
				if (rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
					/* generate DIF for write operation */
					num_blocks = SPDK_CEIL_DIV(rdma_req->req.dif.elba_length, rdma_req->req.dif.dif_ctx.block_size);
					assert(num_blocks > 0);

					rc = spdk_dif_generate(rdma_req->req.iov, rdma_req->req.iovcnt,
							       num_blocks, &rdma_req->req.dif.dif_ctx);
					if (rc != 0) {
						SPDK_ERRLOG("DIF generation failed\n");
						rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
						spdk_nvmf_qpair_disconnect(&rqpair->qpair);
						break;
					}
				}

				assert(rdma_req->req.dif.elba_length >= rdma_req->req.length);
				/* set extended length before IO operation */
				rdma_req->req.length = rdma_req->req.dif.elba_length;
			}

			if (rdma_req->req.cmd->nvme_cmd.fuse != SPDK_NVME_CMD_FUSE_NONE) {
				if (rdma_req->fused_failed) {
					/* This request failed FUSED semantics.  Fail it immediately, without
					 * even sending it to the target layer.
					 */
					rsp->status.sct = SPDK_NVME_SCT_GENERIC;
					rsp->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
					STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, rdma_req, state_link);
					rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
					break;
				}

				if (rdma_req->fused_pair == NULL ||
				    rdma_req->fused_pair->state != RDMA_REQUEST_STATE_READY_TO_EXECUTE) {
					/* This request is ready to execute, but either we don't know yet if it's
					 * valid - i.e. this is a FIRST but we haven't received the next
					 * request yet or the other request of this fused pair isn't ready to
					 * execute.  So break here and this request will get processed later either
					 * when the other request is ready or we find that this request isn't valid.
					 */
					break;
				}
			}

			/* If we get to this point, and this request is a fused command, we know that
			 * it is part of valid sequence (FIRST followed by a SECOND) and that both
			 * requests are READY_TO_EXECUTE. So call spdk_nvmf_request_exec() both on this
			 * request, and the other request of the fused pair, in the correct order.
			 * Also clear the ->fused_pair pointers on both requests, since after this point
			 * we no longer need to maintain the relationship between these two requests.
			 */
			if (rdma_req->req.cmd->nvme_cmd.fuse == SPDK_NVME_CMD_FUSE_SECOND) {
				assert(rdma_req->fused_pair != NULL);
				assert(rdma_req->fused_pair->fused_pair != NULL);
				rdma_req->fused_pair->state = RDMA_REQUEST_STATE_EXECUTING;
				spdk_nvmf_request_exec(&rdma_req->fused_pair->req);
				rdma_req->fused_pair->fused_pair = NULL;
				rdma_req->fused_pair = NULL;
			}
			rdma_req->state = RDMA_REQUEST_STATE_EXECUTING;
			spdk_nvmf_request_exec(&rdma_req->req);
			if (rdma_req->req.cmd->nvme_cmd.fuse == SPDK_NVME_CMD_FUSE_FIRST) {
				assert(rdma_req->fused_pair != NULL);
				assert(rdma_req->fused_pair->fused_pair != NULL);
				rdma_req->fused_pair->state = RDMA_REQUEST_STATE_EXECUTING;
				spdk_nvmf_request_exec(&rdma_req->fused_pair->req);
				rdma_req->fused_pair->fused_pair = NULL;
				rdma_req->fused_pair = NULL;
			}
			break;
		case RDMA_REQUEST_STATE_EXECUTING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_EXECUTING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_EXECUTED:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_EXECUTED, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			if (spdk_nvme_cpl_is_success(rsp) &&
			    rdma_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
				STAILQ_INSERT_TAIL(&rqpair->pending_rdma_write_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING;
			} else {
				STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
			}
			if (spdk_unlikely(rdma_req->req.dif_enabled)) {
				/* restore the original length */
				rdma_req->req.length = rdma_req->req.dif.orig_length;

				if (rdma_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
					struct spdk_dif_error error_blk;

					num_blocks = SPDK_CEIL_DIV(rdma_req->req.dif.elba_length, rdma_req->req.dif.dif_ctx.block_size);
					if (!rdma_req->req.stripped_data) {
						rc = spdk_dif_verify(rdma_req->req.iov, rdma_req->req.iovcnt, num_blocks,
								     &rdma_req->req.dif.dif_ctx, &error_blk);
					} else {
						rc = spdk_dif_verify_copy(rdma_req->req.stripped_data->iov,
									  rdma_req->req.stripped_data->iovcnt,
									  rdma_req->req.iov, rdma_req->req.iovcnt, num_blocks,
									  &rdma_req->req.dif.dif_ctx, &error_blk);
					}
					if (rc) {
						struct spdk_nvme_cpl *rsp = &rdma_req->req.rsp->nvme_cpl;

						SPDK_ERRLOG("DIF error detected. type=%d, offset=%" PRIu32 "\n", error_blk.err_type,
							    error_blk.err_offset);
						rsp->status.sct = SPDK_NVME_SCT_MEDIA_ERROR;
						rsp->status.sc = nvmf_rdma_dif_error_to_compl_status(error_blk.err_type);
						STAILQ_REMOVE(&rqpair->pending_rdma_write_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
						STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, rdma_req, state_link);
						rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
					}
				}
			}
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (rdma_req != STAILQ_FIRST(&rqpair->pending_rdma_write_queue)) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}
			if ((rqpair->current_send_depth + rdma_req->num_outstanding_data_wr + 1) >
			    rqpair->max_send_depth) {
				/* We can only have so many WRs outstanding. we have to wait until some finish.
				 * +1 since each request has an additional wr in the resp. */
				rqpair->poller->stat.pending_rdma_write++;
				break;
			}

			/* We have already verified that this request is the head of the queue. */
			STAILQ_REMOVE_HEAD(&rqpair->pending_rdma_write_queue, state_link);

			/* The data transfer will be kicked off from
			 * RDMA_REQUEST_STATE_READY_TO_COMPLETE state.
			 * We verified that data + response fit into send queue, so we can go to the next state directly
			 */
			rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (rdma_req != STAILQ_FIRST(&rqpair->pending_rdma_send_queue)) {
				/* This request needs to wait in line to send the completion */
				break;
			}

			assert(rqpair->current_send_depth <= rqpair->max_send_depth);
			if (rqpair->current_send_depth == rqpair->max_send_depth) {
				/* We can only have so many WRs outstanding. we have to wait until some finish */
				rqpair->poller->stat.pending_rdma_send++;
				break;
			}

			/* We have already verified that this request is the head of the queue. */
			STAILQ_REMOVE_HEAD(&rqpair->pending_rdma_send_queue, state_link);

			/* The response sending will be kicked off from
			 * RDMA_REQUEST_STATE_READY_TO_COMPLETE state.
			 */
			rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			rc = request_transfer_out(&rdma_req->req, &data_posted);
			assert(rc == 0); /* No good way to handle this currently */
			if (spdk_unlikely(rc)) {
				rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
			} else {
				rdma_req->state = data_posted ? RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST :
						  RDMA_REQUEST_STATE_COMPLETING;
			}
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_COMPLETING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_COMPLETING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_COMPLETED:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_COMPLETED, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair, rqpair->qpair.queue_depth);

			rqpair->poller->stat.request_latency += spdk_get_ticks() - rdma_req->receive_tsc;
			nvmf_rdma_request_free(&rdma_req->req);
			break;
		case RDMA_REQUEST_NUM_STATES:
		default:
			assert(0);
			break;
		}

		/* [한국어] state 가 바뀌었으면 진척 표시 — 호출자가 다른 request 도 진행 시도. */
		if (rdma_req->state != prev_state) {
			progress = true;
		}
	} while (rdma_req->state != prev_state);
	/* [한국어] 한 iteration 에서 state 변동 없음 = 모든 가능한 전이 완료 또는 자원 대기. */

	return progress;
}

/* Public API callbacks begin here */

#define SPDK_NVMF_RDMA_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_RDMA_DEFAULT_AQ_DEPTH 128
#define SPDK_NVMF_RDMA_DEFAULT_SRQ_DEPTH 4096
#define SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR 128
#define SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE 131072
#define SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE (SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE / SPDK_NVMF_MAX_SGL_ENTRIES)
#define SPDK_NVMF_RDMA_DEFAULT_NUM_SHARED_BUFFERS 4095
#define SPDK_NVMF_RDMA_DEFAULT_BUFFER_CACHE_SIZE UINT32_MAX
#define SPDK_NVMF_RDMA_DEFAULT_NO_SRQ false
#define SPDK_NVMF_RDMA_DIF_INSERT_OR_STRIP false
#define SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG 100
#define SPDK_NVMF_RDMA_DEFAULT_ABORT_TIMEOUT_SEC 1
#define SPDK_NVMF_RDMA_DEFAULT_NO_WR_BATCHING false
#define SPDK_NVMF_RDMA_DEFAULT_DATA_WR_POOL_SIZE 4095

static void
nvmf_rdma_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		SPDK_NVMF_RDMA_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr =	SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size =	SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =		SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =		SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE;
	opts->max_aq_depth =		SPDK_NVMF_RDMA_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =	SPDK_NVMF_RDMA_DEFAULT_NUM_SHARED_BUFFERS;
	opts->buf_cache_size =		SPDK_NVMF_RDMA_DEFAULT_BUFFER_CACHE_SIZE;
	opts->dif_insert_or_strip =	SPDK_NVMF_RDMA_DIF_INSERT_OR_STRIP;
	opts->abort_timeout_sec =	SPDK_NVMF_RDMA_DEFAULT_ABORT_TIMEOUT_SEC;
	opts->transport_specific =      NULL;
	opts->data_wr_pool_size	=	SPDK_NVMF_RDMA_DEFAULT_DATA_WR_POOL_SIZE;
}

static void nvmf_rdma_destroy(struct spdk_nvmf_transport *transport,
			      spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg);

static inline bool
nvmf_rdma_is_rxe_device(struct spdk_nvmf_rdma_device *device)
{
	return device->attr.vendor_id == SPDK_RDMA_RXE_VENDOR_ID_OLD ||
	       device->attr.vendor_id == SPDK_RDMA_RXE_VENDOR_ID_NEW;
}

static int nvmf_rdma_accept(void *ctx);
static bool nvmf_rdma_retry_listen_port(struct spdk_nvmf_rdma_transport *rtransport);
static void destroy_ib_device(struct spdk_nvmf_rdma_transport *rtransport,
			      struct spdk_nvmf_rdma_device *device);
static int nvmf_rdma_poll_group_intr(void *ctx);
static int nvmf_rdma_poll_group_poll(struct spdk_nvmf_transport_poll_group *group);

static int
create_ib_device(struct spdk_nvmf_rdma_transport *rtransport, struct ibv_context *context,
		 struct spdk_nvmf_rdma_device **new_device)
{
	struct spdk_nvmf_rdma_device	*device;
	int				rc = 0;

	device = calloc(1, sizeof(*device));
	if (!device) {
		SPDK_ERRLOG("Unable to allocate memory for RDMA devices.\n");
		return -ENOMEM;
	}
	device->context = context;
	rc = ibv_query_device(device->context, &device->attr);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to query RDMA device attributes.\n");
		free(device);
		return rc;
	}

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
	if ((device->attr.device_cap_flags & IBV_DEVICE_MEM_MGT_EXTENSIONS) == 0) {
		SPDK_WARNLOG("The libibverbs on this system supports SEND_WITH_INVALIDATE,");
		SPDK_WARNLOG("but the device with vendor ID %u does not.\n", device->attr.vendor_id);
	}

	/**
	 * The vendor ID is assigned by the IEEE and an ID of 0 implies Soft-RoCE.
	 * The Soft-RoCE RXE driver does not currently support send with invalidate,
	 * but incorrectly reports that it does. There are changes making their way
	 * through the kernel now that will enable this feature. When they are merged,
	 * we can conditionally enable this feature.
	 *
	 * TODO: enable this for versions of the kernel rxe driver that support it.
	 */
	if (nvmf_rdma_is_rxe_device(device)) {
		device->attr.device_cap_flags &= ~(IBV_DEVICE_MEM_MGT_EXTENSIONS);
	}
#endif

	rc = spdk_fd_set_nonblock(device->context->async_fd);
	if (rc < 0) {
		free(device);
		return rc;
	}

	TAILQ_INSERT_TAIL(&rtransport->devices, device, link);
	SPDK_DEBUGLOG(rdma, "New device %p is added to RDMA transport\n", device);

	if (g_nvmf_hooks.get_ibv_pd) {
		device->pd = g_nvmf_hooks.get_ibv_pd(NULL, device->context);
	} else {
		device->pd = ibv_alloc_pd(device->context);
	}

	if (!device->pd) {
		SPDK_ERRLOG("Unable to allocate protection domain.\n");
		destroy_ib_device(rtransport, device);
		return -ENOMEM;
	}

	assert(device->map == NULL);

	device->map = spdk_rdma_utils_create_mem_map(device->pd, &g_nvmf_hooks, IBV_ACCESS_LOCAL_WRITE);
	if (!device->map) {
		SPDK_ERRLOG("Unable to allocate memory map for listen address\n");
		destroy_ib_device(rtransport, device);
		return -ENOMEM;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		device->async_intr = SPDK_INTERRUPT_REGISTER(device->context->async_fd, nvmf_rdma_accept,
				     &rtransport->transport);
		if (device->async_intr == NULL) {
			SPDK_ERRLOG("Failed to register interrupt on async fd %d\n", device->context->async_fd);
			destroy_ib_device(rtransport, device);
			return -ENOMEM;
		}
	}

	assert(device->map != NULL);
	assert(device->pd != NULL);

	if (new_device) {
		*new_device = device;
	}
	SPDK_NOTICELOG("Create IB device %s(%p/%p) succeed.\n", ibv_get_device_name(context->device),
		       device, context);

	return 0;
}

static void
free_poll_fds(struct spdk_nvmf_rdma_transport *rtransport)
{
	if (rtransport->poll_fds) {
		free(rtransport->poll_fds);
		rtransport->poll_fds = NULL;
	}
	rtransport->npoll_fds = 0;
}

static int
generate_poll_fds(struct spdk_nvmf_rdma_transport *rtransport)
{
	/* Set up poll descriptor array to monitor events from RDMA and IB
	 * in a single poll syscall
	 */
	int device_count = 0;
	int i = 0;
	struct spdk_nvmf_rdma_device *device, *tmp;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
		device_count++;
	}

	rtransport->npoll_fds = device_count + 1;

	rtransport->poll_fds = calloc(rtransport->npoll_fds, sizeof(struct pollfd));
	if (rtransport->poll_fds == NULL) {
		SPDK_ERRLOG("poll_fds allocation failed\n");
		return -ENOMEM;
	}

	rtransport->poll_fds[i].fd = rtransport->event_channel->fd;
	rtransport->poll_fds[i++].events = POLLIN;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
		rtransport->poll_fds[i].fd = device->context->async_fd;
		rtransport->poll_fds[i++].events = POLLIN;
	}

	return 0;
}

static struct spdk_nvmf_transport *
nvmf_rdma_create(struct spdk_nvmf_transport_opts *opts)
{
	int rc;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_device	*device;
	struct ibv_context		**contexts;
	size_t				data_wr_pool_size;
	uint32_t			i;
	uint32_t			sge_count;
	uint32_t			min_shared_buffers;
	uint32_t			min_in_capsule_data_size;
	int				max_device_sge = SPDK_NVMF_MAX_SGL_ENTRIES;
	uint64_t			period;

	rtransport = calloc(1, sizeof(*rtransport));
	if (!rtransport) {
		return NULL;
	}

	TAILQ_INIT(&rtransport->devices);
	TAILQ_INIT(&rtransport->ports);
	TAILQ_INIT(&rtransport->poll_groups);
	TAILQ_INIT(&rtransport->retry_ports);

	rtransport->transport.ops = &spdk_nvmf_transport_rdma;
	rtransport->rdma_opts.num_cqe = DEFAULT_NVMF_RDMA_CQ_SIZE;
	rtransport->rdma_opts.max_srq_depth = SPDK_NVMF_RDMA_DEFAULT_SRQ_DEPTH;
	rtransport->rdma_opts.no_srq = SPDK_NVMF_RDMA_DEFAULT_NO_SRQ;
	rtransport->rdma_opts.acceptor_backlog = SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG;
	rtransport->rdma_opts.no_wr_batching = SPDK_NVMF_RDMA_DEFAULT_NO_WR_BATCHING;
	if (opts->transport_specific != NULL &&
	    spdk_json_decode_object_relaxed(opts->transport_specific, rdma_transport_opts_decoder,
					    SPDK_COUNTOF(rdma_transport_opts_decoder),
					    &rtransport->rdma_opts)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	SPDK_INFOLOG(rdma, "*** RDMA Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_io_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  in_capsule_data_size=%d, max_aq_depth=%d,\n"
		     "  num_shared_buffers=%d, num_cqe=%d, max_srq_depth=%d, no_srq=%d,"
		     "  acceptor_backlog=%d, no_wr_batching=%d abort_timeout_sec=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr - 1,
		     opts->io_unit_size,
		     opts->in_capsule_data_size,
		     opts->max_aq_depth,
		     opts->num_shared_buffers,
		     rtransport->rdma_opts.num_cqe,
		     rtransport->rdma_opts.max_srq_depth,
		     rtransport->rdma_opts.no_srq,
		     rtransport->rdma_opts.acceptor_backlog,
		     rtransport->rdma_opts.no_wr_batching,
		     opts->abort_timeout_sec);

	/* I/O unit size cannot be larger than max I/O size */
	if (opts->io_unit_size > opts->max_io_size) {
		opts->io_unit_size = opts->max_io_size;
	}

	if (rtransport->rdma_opts.acceptor_backlog <= 0) {
		SPDK_ERRLOG("The acceptor backlog cannot be less than 1, setting to the default value of (%d).\n",
			    SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG);
		rtransport->rdma_opts.acceptor_backlog = SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG;
	}

	if (opts->num_shared_buffers < (SPDK_NVMF_MAX_SGL_ENTRIES * 2)) {
		SPDK_ERRLOG("The number of shared data buffers (%d) is less than"
			    "the minimum number required to guarantee that forward progress can be made (%d)\n",
			    opts->num_shared_buffers, (SPDK_NVMF_MAX_SGL_ENTRIES * 2));
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	/* If buf_cache_size == UINT32_MAX, we will dynamically pick a cache size later that we know will fit. */
	if (opts->buf_cache_size < UINT32_MAX) {
		min_shared_buffers = spdk_env_get_core_count() * opts->buf_cache_size;
		if (min_shared_buffers > opts->num_shared_buffers) {
			SPDK_ERRLOG("There are not enough buffers to satisfy"
				    "per-poll group caches for each thread. (%" PRIu32 ")"
				    "supplied. (%" PRIu32 ") required\n", opts->num_shared_buffers, min_shared_buffers);
			SPDK_ERRLOG("Please specify a larger number of shared buffers\n");
			nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
			return NULL;
		}
	}

	sge_count = opts->max_io_size / opts->io_unit_size;
	if (sge_count > NVMF_DEFAULT_TX_SGE) {
		SPDK_ERRLOG("Unsupported IO Unit size specified, %d bytes\n", opts->io_unit_size);
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	min_in_capsule_data_size = sizeof(struct spdk_nvme_sgl_descriptor) * SPDK_NVMF_MAX_SGL_ENTRIES;
	if (opts->in_capsule_data_size < min_in_capsule_data_size) {
		SPDK_WARNLOG("In capsule data size is set to %u, this is minimum size required to support msdbd=16\n",
			     min_in_capsule_data_size);
		opts->in_capsule_data_size = min_in_capsule_data_size;
	}

	rtransport->event_channel = rdma_create_event_channel();
	if (rtransport->event_channel == NULL) {
		SPDK_ERRLOG("rdma_create_event_channel() failed, %s\n", spdk_strerror(errno));
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	if (spdk_fd_set_nonblock(rtransport->event_channel->fd) < 0) {
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	data_wr_pool_size = opts->data_wr_pool_size;
	if (data_wr_pool_size < SPDK_NVMF_MAX_SGL_ENTRIES * 2 * spdk_env_get_core_count()) {
		data_wr_pool_size = SPDK_NVMF_MAX_SGL_ENTRIES * 2 * spdk_env_get_core_count();
		SPDK_NOTICELOG("data_wr_pool_size is changed to %zu to guarantee enough cache for handling "
			       "at least one IO in each core\n", data_wr_pool_size);
	}
	rtransport->data_wr_pool = spdk_mempool_create("spdk_nvmf_rdma_wr_data", data_wr_pool_size,
				   sizeof(struct spdk_nvmf_rdma_request_data), SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				   SPDK_ENV_NUMA_ID_ANY);
	if (!rtransport->data_wr_pool) {
		if (spdk_mempool_lookup("spdk_nvmf_rdma_wr_data") != NULL) {
			SPDK_ERRLOG("Unable to allocate work request pool for poll group: already exists\n");
			SPDK_ERRLOG("Probably running in multiprocess environment, which is "
				    "unsupported by the nvmf library\n");
		} else {
			SPDK_ERRLOG("Unable to allocate work request pool for poll group\n");
		}
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	contexts = rdma_get_devices(NULL);
	if (contexts == NULL) {
		SPDK_ERRLOG("rdma_get_devices() failed: %s (%d)\n", spdk_strerror(errno), errno);
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	i = 0;
	rc = 0;
	while (contexts[i] != NULL) {
		rc = create_ib_device(rtransport, contexts[i], &device);
		if (rc < 0) {
			break;
		}
		i++;
		max_device_sge = spdk_min(max_device_sge, device->attr.max_sge);
		device->is_ready = true;
	}
	rdma_free_devices(contexts);

	if (opts->io_unit_size * max_device_sge < opts->max_io_size) {
		/* divide and round up. */
		opts->io_unit_size = (opts->max_io_size + max_device_sge - 1) / max_device_sge;

		/* round up to the nearest 4k. */
		opts->io_unit_size = (opts->io_unit_size + NVMF_DATA_BUFFER_ALIGNMENT - 1) & ~NVMF_DATA_BUFFER_MASK;

		opts->io_unit_size = spdk_max(opts->io_unit_size, SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE);
		SPDK_NOTICELOG("Adjusting the io unit size to fit the device's maximum I/O size. New I/O unit size %u\n",
			       opts->io_unit_size);
	}

	if (rc < 0) {
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	rc = generate_poll_fds(rtransport);
	if (rc < 0) {
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	period = spdk_interrupt_mode_is_enabled() ? 0 : opts->acceptor_poll_rate;
	rtransport->accept_poller = SPDK_POLLER_REGISTER(nvmf_rdma_accept, &rtransport->transport, period);
	if (!rtransport->accept_poller) {
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		spdk_poller_register_interrupt(rtransport->accept_poller, NULL, NULL);
		rtransport->cm_event_intr = SPDK_INTERRUPT_REGISTER(rtransport->event_channel->fd, nvmf_rdma_accept,
					    &rtransport->transport);

		if (rtransport->cm_event_intr == NULL) {
			SPDK_ERRLOG("Failed to register interrupt for CM event channel\n");
			nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
			return NULL;
		}
	}

	return &rtransport->transport;
}

static void
destroy_ib_device(struct spdk_nvmf_rdma_transport *rtransport,
		  struct spdk_nvmf_rdma_device *device)
{
	TAILQ_REMOVE(&rtransport->devices, device, link);
	spdk_rdma_utils_free_mem_map(&device->map);
	spdk_interrupt_unregister(&device->async_intr);
	if (device->pd) {
		if (!g_nvmf_hooks.get_ibv_pd) {
			ibv_dealloc_pd(device->pd);
		}
	}
	SPDK_DEBUGLOG(rdma, "IB device [%p] is destroyed.\n", device);
	free(device);
}

static void
nvmf_rdma_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	assert(w != NULL);

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	spdk_json_write_named_uint32(w, "max_srq_depth", rtransport->rdma_opts.max_srq_depth);
	spdk_json_write_named_bool(w, "no_srq", rtransport->rdma_opts.no_srq);
	if (rtransport->rdma_opts.no_srq == true) {
		spdk_json_write_named_int32(w, "num_cqe", rtransport->rdma_opts.num_cqe);
	}
	spdk_json_write_named_int32(w, "acceptor_backlog", rtransport->rdma_opts.acceptor_backlog);
	spdk_json_write_named_bool(w, "no_wr_batching", rtransport->rdma_opts.no_wr_batching);
}

static void
nvmf_rdma_destroy(struct spdk_nvmf_transport *transport,
		  spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_port	*port, *port_tmp;
	struct spdk_nvmf_rdma_device	*device, *device_tmp;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	TAILQ_FOREACH_SAFE(port, &rtransport->retry_ports, link, port_tmp) {
		TAILQ_REMOVE(&rtransport->retry_ports, port, link);
		free(port);
	}

	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, port_tmp) {
		TAILQ_REMOVE(&rtransport->ports, port, link);
		rdma_destroy_id(port->id);
		free(port);
	}
	spdk_interrupt_unregister(&rtransport->cm_event_intr);
	spdk_poller_unregister(&rtransport->accept_poller);
	free_poll_fds(rtransport);

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, device_tmp) {
		destroy_ib_device(rtransport, device);
	}

	if (rtransport->data_wr_pool != NULL) {
		if (spdk_mempool_count(rtransport->data_wr_pool) != transport->opts.data_wr_pool_size) {
			SPDK_ERRLOG("transport wr pool count is %zu but should be %u\n",
				    spdk_mempool_count(rtransport->data_wr_pool),
				    transport->opts.max_queue_depth * SPDK_NVMF_MAX_SGL_ENTRIES);
		}
	}

	spdk_mempool_free(rtransport->data_wr_pool);
	if (rtransport->event_channel != NULL) {
		rdma_destroy_event_channel(rtransport->event_channel);
	}

	free(rtransport);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

static void nvmf_rdma_trid_from_cm_id(struct rdma_cm_id *id,
				      struct spdk_nvme_transport_id *trid,
				      bool peer);

static bool nvmf_rdma_rescan_devices(struct spdk_nvmf_rdma_transport *rtransport);

static int
nvmf_rdma_listen(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvmf_listen_opts *listen_opts)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvmf_rdma_port	*port, *tmp_port;
	struct addrinfo			*res;
	struct addrinfo			hints;
	int				family;
	int				rc;
	long int			port_val;
	bool				is_retry = false;

	if (!strlen(trid->trsvcid)) {
		SPDK_ERRLOG("Service id is required\n");
		return -EINVAL;
	}

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	assert(rtransport->event_channel != NULL);

	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		return -ENOMEM;
	}

	port->trid = trid;

	switch (trid->adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		family = AF_INET;
		break;
	case SPDK_NVMF_ADRFAM_IPV6:
		family = AF_INET6;
		break;
	default:
		SPDK_ERRLOG("Unhandled ADRFAM %d\n", trid->adrfam);
		free(port);
		return -EINVAL;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;

	/* Range check the trsvcid. Fail in 3 cases:
	 * < 0: means that spdk_strtol hit an error
	 * 0: this results in ephemeral port which we don't want
	 * > 65535: port too high
	 */
	port_val = spdk_strtol(trid->trsvcid, 10);
	if (port_val <= 0 || port_val > 65535) {
		SPDK_ERRLOG("invalid trsvcid %s\n", trid->trsvcid);
		free(port);
		return -EINVAL;
	}

	rc = getaddrinfo(trid->traddr, trid->trsvcid, &hints, &res);
	if (rc) {
		SPDK_ERRLOG("getaddrinfo failed: %s (%d)\n", gai_strerror(rc), rc);
		free(port);
		return -(abs(rc));
	}

	rc = rdma_create_id(rtransport->event_channel, &port->id, port, RDMA_PS_TCP);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_create_id() failed\n");
		freeaddrinfo(res);
		free(port);
		return rc;
	}

	rc = rdma_bind_addr(port->id, res->ai_addr);
	freeaddrinfo(res);

	if (rc < 0) {
		TAILQ_FOREACH(tmp_port, &rtransport->retry_ports, link) {
			if (spdk_nvme_transport_id_compare(tmp_port->trid, trid) == 0) {
				is_retry = true;
				break;
			}
		}
		if (!is_retry) {
			SPDK_ERRLOG("rdma_bind_addr() failed\n");
		}
		rdma_destroy_id(port->id);
		free(port);
		return rc;
	}

	if (!port->id->verbs) {
		SPDK_ERRLOG("ibv_context is null\n");
		rdma_destroy_id(port->id);
		free(port);
		return -1;
	}

	rc = rdma_listen(port->id, rtransport->rdma_opts.acceptor_backlog);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_listen() failed\n");
		rdma_destroy_id(port->id);
		free(port);
		return rc;
	}

	TAILQ_FOREACH(device, &rtransport->devices, link) {
		if (device->context == port->id->verbs && device->is_ready && !device->need_destroy) {
			port->device = device;
			break;
		}
	}
	if (!port->device) {
		SPDK_ERRLOG("Accepted a connection with verbs %p, but unable to find a corresponding device.\n",
			    port->id->verbs);
		rdma_destroy_id(port->id);
		free(port);
		nvmf_rdma_rescan_devices(rtransport);
		return -EINVAL;
	}

	SPDK_NOTICELOG("*** NVMe/RDMA Target Listening on %s port %s ***\n",
		       trid->traddr, trid->trsvcid);

	TAILQ_INSERT_TAIL(&rtransport->ports, port, link);
	return 0;
}

static void
nvmf_rdma_stop_listen_ex(struct spdk_nvmf_transport *transport,
			 const struct spdk_nvme_transport_id *trid, bool need_retry)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_port	*port, *tmp;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	if (!need_retry) {
		TAILQ_FOREACH_SAFE(port, &rtransport->retry_ports, link, tmp) {
			if (spdk_nvme_transport_id_compare(port->trid, trid) == 0) {
				TAILQ_REMOVE(&rtransport->retry_ports, port, link);
				free(port);
			}
		}
	}

	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, tmp) {
		if (spdk_nvme_transport_id_compare(port->trid, trid) == 0) {
			SPDK_DEBUGLOG(rdma, "Port %s:%s removed. need retry: %d\n",
				      port->trid->traddr, port->trid->trsvcid, need_retry);
			TAILQ_REMOVE(&rtransport->ports, port, link);
			rdma_destroy_id(port->id);
			port->id = NULL;
			port->device = NULL;
			if (need_retry) {
				TAILQ_INSERT_TAIL(&rtransport->retry_ports, port, link);
			} else {
				free(port);
			}
			break;
		}
	}
}

static void
nvmf_rdma_stop_listen(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid)
{
	nvmf_rdma_stop_listen_ex(transport, trid, false);
}

static void _nvmf_rdma_register_poller_in_group(void *c);
static void _nvmf_rdma_remove_poller_in_group(void *c);

static bool
nvmf_rdma_all_pollers_management_done(void *c)
{
	struct poller_manage_ctx	*ctx = c;
	int				counter;

	counter = __atomic_sub_fetch(ctx->inflight_op_counter, 1, __ATOMIC_SEQ_CST);
	SPDK_DEBUGLOG(rdma, "nvmf_rdma_all_pollers_management_done called. counter: %d, poller: %p\n",
		      counter, ctx->rpoller);

	if (counter == 0) {
		free((void *)ctx->inflight_op_counter);
	}
	free(ctx);

	return counter == 0;
}

static int
nvmf_rdma_manage_poller(struct spdk_nvmf_rdma_transport *rtransport,
			struct spdk_nvmf_rdma_device *device, bool *has_inflight, bool is_add)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*rpoller;
	struct spdk_nvmf_poll_group		*poll_group;
	struct poller_manage_ctx		*ctx;
	bool					found;
	int					*inflight_counter;
	spdk_msg_fn				do_fn;

	*has_inflight = false;
	do_fn = is_add ? _nvmf_rdma_register_poller_in_group : _nvmf_rdma_remove_poller_in_group;
	inflight_counter = calloc(1, sizeof(int));
	if (!inflight_counter) {
		SPDK_ERRLOG("Failed to allocate inflight counter when removing pollers\n");
		return -ENOMEM;
	}

	TAILQ_FOREACH(rgroup, &rtransport->poll_groups, link) {
		(*inflight_counter)++;
	}

	TAILQ_FOREACH(rgroup, &rtransport->poll_groups, link) {
		found = false;
		TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
			if (rpoller->device == device) {
				found = true;
				break;
			}
		}
		if (found == is_add) {
			__atomic_fetch_sub(inflight_counter, 1, __ATOMIC_SEQ_CST);
			continue;
		}

		ctx = calloc(1, sizeof(struct poller_manage_ctx));
		if (!ctx) {
			SPDK_ERRLOG("Failed to allocate poller_manage_ctx when removing pollers\n");
			if (!*has_inflight) {
				free(inflight_counter);
			}
			return -ENOMEM;
		}

		ctx->rtransport = rtransport;
		ctx->rgroup = rgroup;
		ctx->rpoller = rpoller;
		ctx->device = device;
		ctx->thread = spdk_get_thread();
		ctx->inflight_op_counter = inflight_counter;
		*has_inflight = true;

		poll_group = rgroup->group.group;
		if (poll_group->thread != spdk_get_thread()) {
			spdk_thread_send_msg(poll_group->thread, do_fn, ctx);
		} else {
			do_fn(ctx);
		}
	}

	if (!*has_inflight) {
		free(inflight_counter);
	}

	return 0;
}

static void nvmf_rdma_handle_device_removal(struct spdk_nvmf_rdma_transport *rtransport,
		struct spdk_nvmf_rdma_device *device);

static struct spdk_nvmf_rdma_device *
nvmf_rdma_find_ib_device(struct spdk_nvmf_rdma_transport *rtransport,
			 struct ibv_context *context)
{
	struct spdk_nvmf_rdma_device	*device, *tmp_device;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp_device) {
		if (device->need_destroy) {
			continue;
		}

		if (strcmp(device->context->device->dev_name, context->device->dev_name) == 0) {
			return device;
		}
	}

	return NULL;
}

static bool
nvmf_rdma_check_devices_context(struct spdk_nvmf_rdma_transport *rtransport,
				struct ibv_context *context)
{
	struct spdk_nvmf_rdma_device	*old_device, *new_device;
	int				rc = 0;
	bool				has_inflight;

	old_device = nvmf_rdma_find_ib_device(rtransport, context);

	if (old_device) {
		if (old_device->context != context && !old_device->need_destroy && old_device->is_ready) {
			/* context may not have time to be cleaned when rescan. exactly one context
			 * is valid for a device so this context must be invalid and just remove it. */
			SPDK_WARNLOG("Device %p has a invalid context %p\n", old_device, old_device->context);
			old_device->need_destroy = true;
			nvmf_rdma_handle_device_removal(rtransport, old_device);
		}
		return false;
	}

	rc = create_ib_device(rtransport, context, &new_device);
	/* TODO: update transport opts. */
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create ib device for context: %s(%p)\n",
			    ibv_get_device_name(context->device), context);
		return false;
	}

	rc = nvmf_rdma_manage_poller(rtransport, new_device, &has_inflight, true);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to add poller for device context: %s(%p)\n",
			    ibv_get_device_name(context->device), context);
		return false;
	}

	if (has_inflight) {
		new_device->is_ready = true;
	}

	return true;
}

static bool
nvmf_rdma_rescan_devices(struct spdk_nvmf_rdma_transport *rtransport)
{
	struct spdk_nvmf_rdma_device	*device;
	struct ibv_device		**ibv_device_list = NULL;
	struct ibv_context		**contexts = NULL;
	int				i = 0;
	int				num_dev = 0;
	bool				new_create = false, has_new_device = false;
	struct ibv_context		*tmp_verbs = NULL;

	/* do not rescan when any device is destroying, or context may be freed when
	 * regenerating the poll fds.
	 */
	TAILQ_FOREACH(device, &rtransport->devices, link) {
		if (device->need_destroy) {
			return false;
		}
	}

	ibv_device_list = ibv_get_device_list(&num_dev);

	/* There is a bug in librdmacm. If verbs init failed in rdma_get_devices, it'll be
	 * marked as dead verbs and never be init again. So we need to make sure the
	 * verbs is available before we call rdma_get_devices. */
	if (num_dev >= 0) {
		for (i = 0; i < num_dev; i++) {
			tmp_verbs = ibv_open_device(ibv_device_list[i]);
			if (!tmp_verbs) {
				SPDK_WARNLOG("Failed to init ibv device %p, err %d. Skip rescan.\n", ibv_device_list[i], errno);
				break;
			}
			if (nvmf_rdma_find_ib_device(rtransport, tmp_verbs) == NULL) {
				SPDK_DEBUGLOG(rdma, "Find new verbs init ibv device %p(%s).\n", ibv_device_list[i],
					      tmp_verbs->device->dev_name);
				has_new_device = true;
			}
			ibv_close_device(tmp_verbs);
		}
		ibv_free_device_list(ibv_device_list);
		if (!tmp_verbs || !has_new_device) {
			return false;
		}
	}

	contexts = rdma_get_devices(NULL);

	for (i = 0; contexts && contexts[i] != NULL; i++) {
		new_create |= nvmf_rdma_check_devices_context(rtransport, contexts[i]);
	}

	if (new_create) {
		free_poll_fds(rtransport);
		generate_poll_fds(rtransport);
	}

	if (contexts) {
		rdma_free_devices(contexts);
	}

	return new_create;
}

static bool
nvmf_rdma_retry_listen_port(struct spdk_nvmf_rdma_transport *rtransport)
{
	struct spdk_nvmf_rdma_port	*port, *tmp_port;
	int				rc = 0;
	bool				new_create = false;

	if (TAILQ_EMPTY(&rtransport->retry_ports)) {
		return false;
	}

	new_create = nvmf_rdma_rescan_devices(rtransport);

	TAILQ_FOREACH_SAFE(port, &rtransport->retry_ports, link, tmp_port) {
		rc = nvmf_rdma_listen(&rtransport->transport, port->trid, NULL);

		TAILQ_REMOVE(&rtransport->retry_ports, port, link);
		if (rc) {
			if (new_create) {
				SPDK_ERRLOG("Found new IB device but port %s:%s is still failed(%d) to listen.\n",
					    port->trid->traddr, port->trid->trsvcid, rc);
			}
			TAILQ_INSERT_TAIL(&rtransport->retry_ports, port, link);
			break;
		} else {
			SPDK_NOTICELOG("Port %s:%s come back\n", port->trid->traddr, port->trid->trsvcid);
			free(port);
		}
	}

	return true;
}

static void
/*
 * [한국어]
 * nvmf_rdma_qpair_process_pending - QP 의 4개 PENDING 큐 + free→incoming pairing 처리.
 *
 * @rtransport: transport 컨텍스트.
 * @rqpair: 처리할 QP.
 * @drain: true = 모든 큐를 끝까지 비움 (close 경로), false = 진척 없으면 중단 (정상 폴링).
 *
 * 핵심 정책: 5단계 우선순위로 PENDING 큐들을 순회.
 *
 * 우선순위 (낮은 번호 = 먼저):
 *   1. **pending_rdma_send_queue** (응답 SEND 대기) — 가장 먼저: in-flight 가 끝나야 자원 회수.
 *   2. **pending_rdma_read_queue** (RDMA READ 대기, write 데이터 가져오기) — 다음:
 *      read 제약 (max_read_depth) 이 write 보다 엄격하므로 우선 처리.
 *   3. **pending_rdma_write_queue** (RDMA WRITE 대기, read 데이터 보내기) — 그 다음.
 *   4. **group.pending_buf_queue** (iobuf 부족 대기) — 그 다음: bdev 자원 회복 후.
 *   5. **free_queue ↔ incoming_queue pairing** — 마지막: 새 capsule 처리 시작.
 *
 * pairing 패턴 (단계 5): RECV 도착 → incoming_queue 적재, free request → free_queue 적재.
 * 둘 다 있으면 짝지어 request->state=NEW 진입 → request_process 가 state machine 시작.
 *
 * drain 모드: close_qpair 가 호출 시 true. 한 단계 진척 없어도 다음 단계 시도 — 가능한 한
 * 많이 정리. 정상 폴링은 false 로 → 한 단계 안 되면 중단 (다음 iteration 에 재시도).
 *
 * pending_rdma_read_queue 의 TRANSFERRING_* 건너뛰기: 같은 큐에 발행됐지만 아직 완료 안 된
 * request 도 들어 있을 수 있음 (state_link 일관성 유지). state 검사로 fair processing 보장.
 *
 * pending_free_request 통계: incoming 있는데 free 없음 = 호스트가 일시적으로 max_queue_depth
 * 초과해 보내는 중. 모니터링 지표.
 *
 * 호출 체인:
 *   poller_poll (정상 폴링 끝) → [본 함수, drain=false]
 *   destroy_drained_qpair → [본 함수, drain=true]
 */
static inline void
nvmf_rdma_qpair_process_pending(struct spdk_nvmf_rdma_transport *rtransport,
				struct spdk_nvmf_rdma_qpair *rqpair, bool drain)
{
	struct spdk_nvmf_request *req, *tmp;
	struct spdk_nvmf_rdma_request	*rdma_req, *req_tmp;
	struct spdk_nvmf_rdma_resources *resources;

	/* First process requests which are waiting for response to be sent */
	/* [한국어] 1순위: SEND PENDING — 응답 자원 회복 우선 (가장 마지막 단계의 대기). */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->pending_rdma_send_queue, state_link, req_tmp) {
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* We process I/O in the data transfer pending queue at the highest priority. */
	/* [한국어] 2순위: RDMA READ PENDING — read 제약 (max_read_depth) 이 send 보다 엄격하므로
	 * 자원 회복 즉시 발행해야 fairness. */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->pending_rdma_read_queue, state_link, req_tmp) {
		if (rdma_req->state != RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING) {
			/* Requests in this queue might be in state RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
			 * they are transmitting data over network but we keep them in the list to guarantee
			 * fair processing. */
			/* [한국어] 같은 큐의 TRANSFERRING 상태 entry skip — 발행됐지만 완료 대기 중.
			 * 큐에 남겨두면 fair processing 의 cursor 역할. */
			continue;
		}
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* Then RDMA writes since reads have stronger restrictions than writes */
	/* [한국어] 3순위: RDMA WRITE PENDING — read 다음 우선순위. */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->pending_rdma_write_queue, state_link, req_tmp) {
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* Then we handle request waiting on memory buffers. */
	/* [한국어] 4순위: iobuf 대기 (poll_group 공용 큐) — bdev 측 자원이라 RDMA 와 별개. */
	STAILQ_FOREACH_SAFE(req, &rqpair->poller->group->group.pending_buf_queue, buf_link, tmp) {
		rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* [한국어] 5순위 (가장 마지막): 새 RECV → 새 request 시작. */
	resources = rqpair->resources;
	/* [한국어] free request + incoming capsule 둘 다 있을 때만 pairing. */
	while (!STAILQ_EMPTY(&resources->free_queue) && !STAILQ_EMPTY(&resources->incoming_queue)) {
		rdma_req = STAILQ_FIRST(&resources->free_queue);
		STAILQ_REMOVE_HEAD(&resources->free_queue, state_link);
		rdma_req->recv = STAILQ_FIRST(&resources->incoming_queue);
		STAILQ_REMOVE_HEAD(&resources->incoming_queue, link);

		/* [한국어] SRQ 모드: req 는 poller 풀에서 빌렸으므로 qpair 연결은 RECV 의 것에서 가져옴. */
		if (rqpair->srq != NULL) {
			rdma_req->req.qpair = &rdma_req->recv->qpair->qpair;
		}

		rdma_req->receive_tsc = rdma_req->recv->receive_tsc;
		rdma_req->state = RDMA_REQUEST_STATE_NEW;
		/* [한국어] state machine 시작 — 진척 못 하면 중단 (drain 무관). */
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false) {
			break;
		}
	}
	/* [한국어] capsule 있는데 처리할 request 슬롯 없음 — host 가 max_queue_depth 초과 중.
	 * 통계만 증가 (정상은 호스트가 backpressure 인지해 보내기 멈춤). */
	if (!STAILQ_EMPTY(&resources->incoming_queue) && STAILQ_EMPTY(&resources->free_queue)) {
		rqpair->poller->stat.pending_free_request++;
	}
}

static inline void
nvmf_rdma_poller_process_pending_qpairs(struct spdk_nvmf_rdma_transport *rtransport,
					struct spdk_nvmf_rdma_poller *rpoller)
{
	struct spdk_nvmf_rdma_qpair *rqpair, *tmp;

	TAILQ_FOREACH_SAFE(rqpair, &rpoller->active_qpairs, active_link, tmp) {
		nvmf_rdma_qpair_process_pending(rtransport, rqpair, false);
	}
}

static inline bool
nvmf_rdma_device_supports_last_wqe_reached(struct spdk_nvmf_rdma_device *device)
{
	/* iWARP transport and SoftRoCE driver don't support LAST_WQE_REACHED ibv async event */
	return !nvmf_rdma_is_rxe_device(device) &&
	       device->context->device->transport_type != IBV_TRANSPORT_IWARP;
}

/*
 * [한국어]
 * nvmf_rdma_destroy_drained_qpair - QP 의 drain 조건 확인 후 충족 시 destroy.
 *
 * @rqpair: 검사할 QP.
 *
 * close_qpair 가 to_close=true 마킹 후 호출, 또는 poller_poll 의 매 iteration 끝에서
 * "비활성 QP" 마다 호출. drain 조건이 안 차면 즉시 반환 — 다음 iteration 에서 재시도.
 *
 * Drain 조건 (모두 충족해야 destroy):
 *   1. to_close=true (close_qpair 호출됨).
 *   2. send_depth = 0 (in-flight SEND/RDMA WRITE 없음).
 *   3. non-SRQ: recv_depth = max_queue_depth (모든 RECV 가 사용됨 — 마지막 capsule 까지).
 *      SRQ: last_wqe_reached 이벤트 수신 (iWARP/SoftRoCE 제외 — 거기는 다른 조건).
 *   4. fused_first = NULL (pending fused command 없음).
 *
 * Force destroy 예외: poller->need_destroy 면 위 조건 무시하고 강제 destroy
 * (device removal 등 비상 상황).
 *
 * 왜 모든 조건 필요한가:
 *   - send_depth > 0 에서 destroy 시 in-flight SEND 의 CQE 가 free 된 메모리 가리킴.
 *   - recv_depth < max 면 아직 RECV 게시된 게 남아 host 가 capsule 보내면 도착 (lost).
 *   - last_wqe_reached: SRQ 의 RECV WR 가 모두 consume 됐다는 ibv async event.
 *
 * 비동기 본질: 본 함수는 polling driven. drain 안 됐으면 다음 iteration 에서 다시 검사.
 *
 * 호출 체인:
 *   close_qpair → [본 함수] (drain 즉시 가능하면 destroy)
 *   poller_poll 각 iter → [본 함수] (조건 다시 검사)
 */
static void
nvmf_rdma_destroy_drained_qpair(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_transport *rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
			struct spdk_nvmf_rdma_transport, transport);

	if (rqpair->poller) {
		/* a qpair might be destroyed before being added to a poll group */
		/* [한국어] pending_rdma_*_queue 의 모든 request 강제 처리 — drain 진행 가속. */
		nvmf_rdma_qpair_process_pending(rtransport, rqpair, true);
	}

	/* nvmf_rdma_close_qpair is not called */
	/* [한국어] to_close 아직 마킹 안 됐으면 사용자가 종료 의사 표시 안 함 — 아무것도 안 함. */
	if (!rqpair->to_close) {
		return;
	}

	/* device is already destroyed and we should force destroy this qpair. */
	/* [한국어] device removal 등 비상 상황 — drain 조건 무시하고 강제 정리. */
	if (rqpair->poller && rqpair->poller->need_destroy) {
		nvmf_rdma_qpair_destroy(rqpair);
		return;
	}

	/* In non SRQ path, we will reach rqpair->max_queue_depth. In SRQ path, we will get the last_wqe event. */
	/* [한국어] SEND/RDMA WRITE in-flight 있으면 — CQE 가 free 된 메모리 가리킬 위험으로 대기. */
	if (rqpair->current_send_depth != 0) {
		return;
	}

	if (rqpair->srq == NULL && rqpair->current_recv_depth != rqpair->max_queue_depth) {
		return;
	}

	/* For devices that support LAST_WQE_REACHED with srq, we need to
	 * wait to destroy the qpair until that event has been received.
	 */
	if (rqpair->srq != NULL && rqpair->last_wqe_reached == false &&
	    nvmf_rdma_device_supports_last_wqe_reached(rqpair->device)) {
		return;
	}

	assert(rqpair->qpair.state == SPDK_NVMF_QPAIR_UNINITIALIZED ||
	       rqpair->qpair.state == SPDK_NVMF_QPAIR_ERROR);

	nvmf_rdma_qpair_destroy(rqpair);
}

/*
 * [한국어]
 * nvmf_rdma_disconnect - RDMA_CM_EVENT_DISCONNECTED / DEVICE_REMOVAL(w/qp) 처리.
 *
 * @evt: CM 이벤트 (DISCONNECTED 또는 DEVICE_REMOVAL).
 * @event_acked: out — true 로 세팅 (본 함수가 ack 책임).
 * @return: 0 성공, -1 cm_id 또는 context 누락.
 *
 * peer 가 RDMA disconnect 시작 또는 device removal 시 호출. spdk_nvmf_qpair_disconnect 가
 * 상위 NVMe-oF 레이어 거쳐 transport vtable 의 close_qpair (= nvmf_rdma_close_qpair) 호출 →
 * to_close 마킹 후 drain 시작.
 *
 * ack 책임 분기: process_cm_events 가 event_acked=false 면 본 함수가 직접 ack 후 true 세팅.
 * 이유: disconnect 처리가 비동기 (qpair_disconnect 가 즉시 destroy 하지 않음)이라 evt 의
 * 수명이 본 함수 반환 후에는 신뢰 불가 → 미리 ack 해 evt buffer 해제.
 *
 * 호출 체인:
 *   nvmf_process_cm_events → DISCONNECTED → [본 함수]
 *     → rdma_ack_cm_event (evt 즉시 해제)
 *     → spdk_nvmf_qpair_disconnect → close_qpair → drain → destroy
 */
static int
nvmf_rdma_disconnect(struct rdma_cm_event *evt, bool *event_acked)
{
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;

	if (evt->id == NULL) {
		SPDK_ERRLOG("disconnect request: missing cm_id\n");
		return -1;
	}

	/* [한국어] cm_id->context = qpair (connect 시점에 등록). NULL = connect 못한 채 disconnect 도착. */
	qpair = evt->id->context;
	if (qpair == NULL) {
		SPDK_ERRLOG("disconnect request: no active connection\n");
		return -1;
	}

	/* [한국어] ack 우선 — evt buffer 즉시 해제, disconnect 처리는 비동기라 evt 수명 보장 X. */
	rdma_ack_cm_event(evt);
	*event_acked = true;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	spdk_trace_record(TRACE_RDMA_QP_DISCONNECT, 0, 0, (uintptr_t)rqpair);

	/* [한국어] 상위 NVMe-oF 레이어를 통해 transport vtable 의 close_qpair 호출 chain 시작. */
	spdk_nvmf_qpair_disconnect(&rqpair->qpair);

	return 0;
}

#ifdef DEBUG
static const char *CM_EVENT_STR[] = {
	"RDMA_CM_EVENT_ADDR_RESOLVED",
	"RDMA_CM_EVENT_ADDR_ERROR",
	"RDMA_CM_EVENT_ROUTE_RESOLVED",
	"RDMA_CM_EVENT_ROUTE_ERROR",
	"RDMA_CM_EVENT_CONNECT_REQUEST",
	"RDMA_CM_EVENT_CONNECT_RESPONSE",
	"RDMA_CM_EVENT_CONNECT_ERROR",
	"RDMA_CM_EVENT_UNREACHABLE",
	"RDMA_CM_EVENT_REJECTED",
	"RDMA_CM_EVENT_ESTABLISHED",
	"RDMA_CM_EVENT_DISCONNECTED",
	"RDMA_CM_EVENT_DEVICE_REMOVAL",
	"RDMA_CM_EVENT_MULTICAST_JOIN",
	"RDMA_CM_EVENT_MULTICAST_ERROR",
	"RDMA_CM_EVENT_ADDR_CHANGE",
	"RDMA_CM_EVENT_TIMEWAIT_EXIT"
};
#endif /* DEBUG */

static void
nvmf_rdma_disconnect_qpairs_on_port(struct spdk_nvmf_rdma_transport *rtransport,
				    struct spdk_nvmf_rdma_port *port)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*rpoller;
	struct spdk_nvmf_rdma_qpair		*rqpair;

	TAILQ_FOREACH(rgroup, &rtransport->poll_groups, link) {
		TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
			RB_FOREACH(rqpair, qpairs_tree, &rpoller->qpairs) {
				if (rqpair->listen_id == port->id) {
					spdk_nvmf_qpair_disconnect(&rqpair->qpair);
				}
			}
		}
	}
}

static bool
nvmf_rdma_handle_cm_event_addr_change(struct spdk_nvmf_transport *transport,
				      struct rdma_cm_event *event)
{
	const struct spdk_nvme_transport_id	*trid;
	struct spdk_nvmf_rdma_port		*port;
	struct spdk_nvmf_rdma_transport		*rtransport;
	bool					event_acked = false;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	TAILQ_FOREACH(port, &rtransport->ports, link) {
		if (port->id == event->id) {
			SPDK_ERRLOG("ADDR_CHANGE: IP %s:%s migrated\n", port->trid->traddr, port->trid->trsvcid);
			rdma_ack_cm_event(event);
			event_acked = true;
			trid = port->trid;
			break;
		}
	}

	if (event_acked) {
		nvmf_rdma_disconnect_qpairs_on_port(rtransport, port);

		nvmf_rdma_stop_listen(transport, trid);
		nvmf_rdma_listen(transport, trid, NULL);
	}

	return event_acked;
}

static void
nvmf_rdma_handle_device_removal(struct spdk_nvmf_rdma_transport *rtransport,
				struct spdk_nvmf_rdma_device *device)
{
	struct spdk_nvmf_rdma_port	*port, *port_tmp;
	int				rc;
	bool				has_inflight;

	rc = nvmf_rdma_manage_poller(rtransport, device, &has_inflight, false);
	if (rc) {
		SPDK_ERRLOG("Failed to handle device removal, rc %d\n", rc);
		return;
	}

	if (!has_inflight) {
		/* no pollers, destroy the device */
		device->ready_to_destroy = true;
		spdk_thread_send_msg(spdk_get_thread(), _nvmf_rdma_remove_destroyed_device, rtransport);
	}

	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, port_tmp) {
		if (port->device == device) {
			SPDK_NOTICELOG("Port %s:%s on device %s is being removed.\n",
				       port->trid->traddr,
				       port->trid->trsvcid,
				       ibv_get_device_name(port->device->context->device));

			/* keep NVMF listener and only destroy structures of the
			 * RDMA transport. when the device comes back we can retry listening
			 * and the application's workflow will not be interrupted.
			 */
			nvmf_rdma_stop_listen_ex(&rtransport->transport, port->trid, true);
		}
	}
}

static void
nvmf_rdma_handle_cm_event_port_removal(struct spdk_nvmf_transport *transport,
				       struct rdma_cm_event *event)
{
	struct spdk_nvmf_rdma_port		*port, *tmp_port;
	struct spdk_nvmf_rdma_transport		*rtransport;

	port = event->id->context;
	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	rdma_ack_cm_event(event);

	/* if device removal happens during ctrl qpair disconnecting, it's possible that we receive
	 * an DEVICE_REMOVAL event on qpair but the id->qp is just NULL. So we should make sure that
	 * we are handling a port event here.
	 */
	TAILQ_FOREACH(tmp_port, &rtransport->ports, link) {
		if (port == tmp_port && port->device && !port->device->need_destroy) {
			port->device->need_destroy = true;
			nvmf_rdma_handle_device_removal(rtransport, port->device);
		}
	}
}

/*
 * [한국어]
 * nvmf_process_cm_events - RDMA Connection Manager (CM) 이벤트 큐 폴링 + 디스패치.
 *
 * @transport: 공통 transport 컨텍스트.
 * @max_events: 한 번에 처리할 최대 이벤트 수 (NVMF_RDMA_MAX_EVENTS_PER_POLL = 32).
 *
 * RDMA CM (rdma_cm.h) 은 connection 수명주기를 관리하는 이벤트 채널. accept_poller 가
 * fd readiness 감지 시 본 함수 호출 → 모든 pending 이벤트 처리.
 *
 * CM 이벤트 종류:
 *   - **ADDR/ROUTE_RESOLVED/ERROR**: client 측 이벤트 (target 은 무시).
 *   - **CONNECT_REQUEST**: ★ 핵심 — host 가 새 connection 요청 → nvmf_rdma_connect 호출
 *     (QP create + accept).
 *   - **CONNECT_RESPONSE/UNREACHABLE/REJECTED**: client 측 이벤트.
 *   - **ESTABLISHED**: 3-way handshake 완료. 별도 처리 불필요 (qpair 가 이미 ENABLED).
 *   - **DISCONNECTED**: peer 또는 자기 disconnect → close_qpair.
 *   - **DEVICE_REMOVAL**: NIC unplug — qp 있으면 disconnect, 없으면 port removal 처리.
 *   - **TIMEWAIT_EXIT**: TCP TIME_WAIT 의 RDMA 대응 — 5초 후 cm_id 자원 회수 가능 신호.
 *
 * 이벤트 ack: rdma_ack_cm_event 가 필수 (커널이 이벤트 buffer 회수). 각 핸들러가 self-ack
 * 하거나 event_acked=false 시 본 함수가 마지막에 ack.
 *
 * batching 32: 한 번에 폭주 이벤트 처리하되 너무 오래 점유 않게 상한.
 *
 * 호출 체인:
 *   nvmf_rdma_accept (accept_poller) → [본 함수]
 *     → switch(event) → nvmf_rdma_connect (CONNECT_REQUEST)
 *                     → nvmf_rdma_disconnect (DISCONNECTED)
 *                     → nvmf_rdma_handle_cm_event_port_removal (DEVICE_REMOVAL w/o qp)
 *     → rdma_ack_cm_event
 */
static void
nvmf_process_cm_events(struct spdk_nvmf_transport *transport, uint32_t max_events)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct rdma_cm_event		*event;
	uint32_t			i;
	int				rc;
	bool				event_acked;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	if (rtransport->event_channel == NULL) {
		return;
	}

	for (i = 0; i < max_events; i++) {
		event_acked = false;
		/* [한국어] non-blocking rdma_get_cm_event — fd 가 non-blocking 모드면 EAGAIN 으로 빈 큐 표시.
		 * 다른 에러는 로그 후 루프 종료. */
		rc = rdma_get_cm_event(rtransport->event_channel, &event);
		if (rc) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				SPDK_ERRLOG("Acceptor Event Error: %s\n", spdk_strerror(errno));
			}
			break;
		}

		SPDK_DEBUGLOG(rdma, "Acceptor Event: %s\n", CM_EVENT_STR[event->event]);

		spdk_trace_record(TRACE_RDMA_CM_ASYNC_EVENT, 0, 0, 0, event->event);

		switch (event->event) {
		case RDMA_CM_EVENT_ADDR_RESOLVED:
		case RDMA_CM_EVENT_ADDR_ERROR:
		case RDMA_CM_EVENT_ROUTE_RESOLVED:
		case RDMA_CM_EVENT_ROUTE_ERROR:
			/* No action required. The target never attempts to resolve routes. */
			break;
		case RDMA_CM_EVENT_CONNECT_REQUEST:
			rc = nvmf_rdma_connect(transport, event);
			if (rc < 0) {
				SPDK_ERRLOG("Unable to process connect event. rc: %d\n", rc);
				break;
			}
			break;
		case RDMA_CM_EVENT_CONNECT_RESPONSE:
			/* The target never initiates a new connection. So this will not occur. */
			break;
		case RDMA_CM_EVENT_CONNECT_ERROR:
			/* Can this happen? The docs say it can, but not sure what causes it. */
			break;
		case RDMA_CM_EVENT_UNREACHABLE:
		case RDMA_CM_EVENT_REJECTED:
			/* These only occur on the client side. */
			break;
		case RDMA_CM_EVENT_ESTABLISHED:
			/* TODO: Should we be waiting for this event anywhere? */
			break;
		case RDMA_CM_EVENT_DISCONNECTED:
			rc = nvmf_rdma_disconnect(event, &event_acked);
			if (rc < 0) {
				SPDK_ERRLOG("Unable to process disconnect event. rc: %d\n", rc);
				break;
			}
			break;
		case RDMA_CM_EVENT_DEVICE_REMOVAL:
			/* In case of device removal, kernel IB part triggers IBV_EVENT_DEVICE_FATAL
			 * which triggers RDMA_CM_EVENT_DEVICE_REMOVAL on all cma_id’s.
			 * Once these events are sent to SPDK, we should release all IB resources and
			 * don't make attempts to call any ibv_query/modify/create functions. We can only call
			 * ibv_destroy* functions to release user space memory allocated by IB. All kernel
			 * resources are already cleaned. */
			if (event->id->qp) {
				/* If rdma_cm event has a valid `qp` pointer then the event refers to the
				 * corresponding qpair. Otherwise the event refers to a listening device. */
				rc = nvmf_rdma_disconnect(event, &event_acked);
				if (rc < 0) {
					SPDK_ERRLOG("Unable to process disconnect event. rc: %d\n", rc);
					break;
				}
			} else {
				nvmf_rdma_handle_cm_event_port_removal(transport, event);
				event_acked = true;
			}
			break;
		case RDMA_CM_EVENT_MULTICAST_JOIN:
		case RDMA_CM_EVENT_MULTICAST_ERROR:
			/* Multicast is not used */
			break;
		case RDMA_CM_EVENT_ADDR_CHANGE:
			event_acked = nvmf_rdma_handle_cm_event_addr_change(transport, event);
			break;
		case RDMA_CM_EVENT_TIMEWAIT_EXIT:
			/* For now, do nothing. The target never re-uses queue pairs. */
			break;
		default:
			SPDK_ERRLOG("Unexpected Acceptor Event [%d]\n", event->event);
			break;
		}
		if (!event_acked) {
			rdma_ack_cm_event(event);
		}
	}
}

static void
nvmf_rdma_handle_last_wqe_reached(struct spdk_nvmf_rdma_qpair *rqpair)
{
	rqpair->last_wqe_reached = true;
	nvmf_rdma_destroy_drained_qpair(rqpair);
}

static void
nvmf_rdma_qpair_process_last_wqe_event(void *ctx)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *event_ctx = ctx;
	struct spdk_nvmf_rdma_qpair *rqpair;

	rqpair = event_ctx->rqpair;

	if (rqpair) {
		assert(event_ctx == rqpair->last_wqe_reached_ctx);
		rqpair->last_wqe_reached_ctx = NULL;
		nvmf_rdma_handle_last_wqe_reached(rqpair);
	}
	free(event_ctx);
}

static int
nvmf_rdma_send_qpair_last_wqe_event(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *ctx;
	struct spdk_thread *thr = NULL;

	if (rqpair->qpair.group) {
		thr = rqpair->qpair.group->thread;
	} else if (rqpair->destruct_channel) {
		thr = spdk_io_channel_get_thread(rqpair->destruct_channel);
	}

	if (!thr) {
		SPDK_DEBUGLOG(rdma, "rqpair %p has no thread\n", rqpair);
		return -EINVAL;
	}

	if (rqpair->last_wqe_reached || rqpair->last_wqe_reached_ctx != NULL) {
		SPDK_ERRLOG("LAST_WQE_REACHED already received for rqpair %p\n", rqpair);
		return -EALREADY;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->rqpair = rqpair;
	rqpair->last_wqe_reached_ctx = ctx;

	spdk_thread_send_msg(thr, nvmf_rdma_qpair_process_last_wqe_event, ctx);

	return 0;
}

static int
nvmf_process_ib_event(struct spdk_nvmf_rdma_device *device)
{
	int				rc;
	struct spdk_nvmf_rdma_qpair	*rqpair = NULL;
	struct ibv_async_event		event;

	rc = ibv_get_async_event(device->context, &event);

	if (rc) {
		/* In non-blocking mode -1 means there are no events available */
		return rc;
	}

	switch (event.event_type) {
	case IBV_EVENT_QP_FATAL:
	case IBV_EVENT_QP_LAST_WQE_REACHED:
	case IBV_EVENT_QP_REQ_ERR:
	case IBV_EVENT_QP_ACCESS_ERR:
	case IBV_EVENT_COMM_EST:
	case IBV_EVENT_PATH_MIG:
	case IBV_EVENT_PATH_MIG_ERR:
		rqpair = event.element.qp->qp_context;
		if (!rqpair) {
			/* Any QP event for NVMe-RDMA initiator may be returned. */
			SPDK_NOTICELOG("Async QP event for unknown QP: %s\n",
				       ibv_event_type_str(event.event_type));
			break;
		}

		switch (event.event_type) {
		case IBV_EVENT_QP_FATAL:
			SPDK_ERRLOG("Fatal event received for rqpair %p\n", rqpair);
			spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0,
					  (uintptr_t)rqpair, event.event_type);
			rqpair->ibv_in_error_state = true;
			spdk_nvmf_qpair_disconnect(&rqpair->qpair);
			break;
		case IBV_EVENT_QP_LAST_WQE_REACHED:
			/* This event only occurs for shared receive queues. */
			SPDK_DEBUGLOG(rdma, "Last WQE reached event received for rqpair %p\n", rqpair);
			rc = nvmf_rdma_send_qpair_last_wqe_event(rqpair);
			if (rc) {
				SPDK_WARNLOG("Failed to send LAST_WQE_REACHED event. rqpair %p, err %d\n", rqpair, rc);
				rqpair->last_wqe_reached = true;
			}
			break;
		case IBV_EVENT_QP_REQ_ERR:
		case IBV_EVENT_QP_ACCESS_ERR:
		case IBV_EVENT_COMM_EST:
		case IBV_EVENT_PATH_MIG:
		case IBV_EVENT_PATH_MIG_ERR:
			SPDK_NOTICELOG("Async QP event: %s\n",
				       ibv_event_type_str(event.event_type));
			spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0,
					  (uintptr_t)rqpair, event.event_type);
			rqpair->ibv_in_error_state = true;
			break;
		default:
			break;
		}
		break;
	case IBV_EVENT_DEVICE_FATAL:
		SPDK_ERRLOG("Device Fatal event[%s] received on %s. device: %p\n",
			    ibv_event_type_str(event.event_type), ibv_get_device_name(device->context->device), device);
		device->need_destroy = true;
		break;
	case IBV_EVENT_CQ_ERR:
	case IBV_EVENT_PORT_ACTIVE:
	case IBV_EVENT_PORT_ERR:
	case IBV_EVENT_LID_CHANGE:
	case IBV_EVENT_PKEY_CHANGE:
	case IBV_EVENT_SM_CHANGE:
	case IBV_EVENT_SRQ_ERR:
	case IBV_EVENT_SRQ_LIMIT_REACHED:
	case IBV_EVENT_CLIENT_REREGISTER:
	case IBV_EVENT_GID_CHANGE:
	case IBV_EVENT_SQ_DRAINED:
	default:
		SPDK_NOTICELOG("Async event: %s\n",
			       ibv_event_type_str(event.event_type));
		spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0, 0, event.event_type);
		break;
	}
	ibv_ack_async_event(&event);

	return 0;
}

static void
nvmf_process_ib_events(struct spdk_nvmf_rdma_device *device, uint32_t max_events)
{
	int rc = 0;
	uint32_t i = 0;

	for (i = 0; i < max_events; i++) {
		rc = nvmf_process_ib_event(device);
		if (rc) {
			break;
		}
	}

	SPDK_DEBUGLOG(rdma, "Device %s: %u events processed\n", device->context->device->name, i);
}

/*
 * [한국어]
 * nvmf_rdma_accept - listener accept poller. RDMA CM 이벤트 + IB async 이벤트 통합 처리.
 *
 * @ctx: spdk_nvmf_transport (transport 등록 시 등록한 컨텍스트).
 * @return: SPDK_POLLER_BUSY (이벤트 처리함) 또는 SPDK_POLLER_IDLE (없음 — backoff).
 *
 * Transport 전체에서 1개만 도는 poller. SPDK reactor 의 main thread 에서 등록 (보통).
 * 처리 이벤트 2종류:
 *   1. **RDMA CM event** (poll_fds[0]): 새 connection 요청 (RDMA_CM_EVENT_CONNECT_REQUEST),
 *      ADDR_RESOLVED, ESTABLISHED, DISCONNECTED 등. nvmf_process_cm_events 가 type 별 분기.
 *   2. **IB async event** (poll_fds[1+]): per-device 의 비동기 이벤트 (FATAL, PORT_DOWN,
 *      QP last_wqe_reached 등). device 별 fd 가 N 개. nvmf_process_ib_events 가 처리.
 *
 * 동작:
 *   1. retry_listen_port: 이전에 실패한 listen 재시도 (포트 충돌 후 timeout 등).
 *   2. poll(timeout=0): non-blocking 으로 모든 fd 의 readiness 검사.
 *   3. nfds 분배: fd[0] 처리 후 device 별 fd 순회.
 *   4. device 가 need_destroy 면 handle_device_removal 호출 (모든 poller/qpair cascading 정리).
 *
 * poll(0) 사용: blocking poll 이 아닌 SPDK reactor 패턴 — 매 iteration 마다 호출되므로
 * blocking 하면 다른 poller 가 starve. timeout=0 으로 즉시 반환.
 *
 * 호출 체인:
 *   SPDK reactor poller registration → [본 함수]
 *     → poll(non-blocking)
 *     → nvmf_process_cm_events (CONNECT_REQUEST → 새 QP create + accept)
 *     → nvmf_process_ib_events (per-device async event)
 *     → nvmf_rdma_handle_device_removal (device disappear 시)
 */
static int
nvmf_rdma_accept(void *ctx)
{
	int	nfds, i = 0;
	struct spdk_nvmf_transport *transport = ctx;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_device *device, *tmp;
	uint32_t count;
	short revents;
	bool do_retry;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	/* [한국어] 이전 listen 실패 (port 충돌 등) 가 timeout 됐으면 재시도. */
	do_retry = nvmf_rdma_retry_listen_port(rtransport);

	/* [한국어] non-blocking poll (timeout=0) — reactor 가 매 iteration 호출하므로 즉시 반환. */
	count = nfds = poll(rtransport->poll_fds, rtransport->npoll_fds, 0);

	if (nfds <= 0) {
		/* [한국어] retry 했으면 BUSY (효과 있음), 아니면 IDLE (다음 reactor 가 backoff 결정). */
		return do_retry ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
	}

	/* The first poll descriptor is RDMA CM event */
	/* [한국어] poll_fds[0] = rdma_event_channel fd (RDMA CM 의 통합 이벤트 채널).
	 * connect/disconnect/addr_resolved 등 모든 CM 이벤트가 여기로 옴. */
	if (rtransport->poll_fds[i++].revents & POLLIN) {
		nvmf_process_cm_events(transport, NVMF_RDMA_MAX_EVENTS_PER_POLL);
		nfds--;
	}

	if (nfds == 0) {
		return SPDK_POLLER_BUSY;
	}

	/* Second and subsequent poll descriptors are IB async events */
	/* [한국어] poll_fds[1+] = device 별 async event fd. PORT_DOWN, FATAL, last_wqe_reached 등. */
	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
		revents = rtransport->poll_fds[i++].revents;
		if (revents & POLLIN) {
			if (spdk_likely(!device->need_destroy)) {
				nvmf_process_ib_events(device, NVMF_RDMA_MAX_EVENTS_PER_POLL);
				/* [한국어] 이벤트 처리 중 device removal 감지 시 cascading cleanup 시작. */
				if (spdk_unlikely(device->need_destroy)) {
					nvmf_rdma_handle_device_removal(rtransport, device);
				}
			}
			nfds--;
		} else if (revents & POLLNVAL || revents & POLLHUP) {
			/* [한국어] device fd 가 비정상 종료 (hot-unplug 등) — log 후 무시 (removal 경로가 처리). */
			SPDK_ERRLOG("Receive unknown revent %x on device %p\n", (int)revents, device);
			nfds--;
		}
	}
	/* check all flagged fd's have been served */
	assert(nfds == 0);

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
nvmf_rdma_cdata_init(struct spdk_nvmf_transport *transport, struct spdk_nvmf_subsystem *subsystem,
		     struct spdk_nvmf_ctrlr_data *cdata)
{
	cdata->nvmf_specific.msdbd = NVMF_DEFAULT_MSDBD;

	/* Disable in-capsule data transfer for RDMA controller when dif_insert_or_strip is enabled
	since in-capsule data only works with NVME drives that support SGL memory layout */
	if (transport->opts.dif_insert_or_strip) {
		cdata->nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
	}

	if (cdata->nvmf_specific.ioccsz > ((sizeof(struct spdk_nvme_cmd) + 0x1000) / 16)) {
		SPDK_WARNLOG("RDMA is configured to support up to 16 SGL entries while in capsule"
			     " data is greater than 4KiB.\n");
		SPDK_WARNLOG("When used in conjunction with the NVMe-oF initiator from the Linux "
			     "kernel between versions 5.4 and 5.12 data corruption may occur for "
			     "writes that are not a multiple of 4KiB in size.\n");
	}
}

static void
nvmf_rdma_discover(struct spdk_nvmf_transport *transport,
		   struct spdk_nvme_transport_id *trid,
		   struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = SPDK_NVMF_TRTYPE_RDMA;
	entry->adrfam = trid->adrfam;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED;

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');

	entry->tsas.rdma.rdma_qptype = SPDK_NVMF_RDMA_QPTYPE_RELIABLE_CONNECTED;
	entry->tsas.rdma.rdma_prtype = SPDK_NVMF_RDMA_PRTYPE_NONE;
	entry->tsas.rdma.rdma_cms = SPDK_NVMF_RDMA_CMS_RDMA_CM;
}

static int
nvmf_rdma_poller_create(struct spdk_nvmf_rdma_transport *rtransport,
			struct spdk_nvmf_rdma_poll_group *rgroup, struct spdk_nvmf_rdma_device *device,
			struct spdk_nvmf_rdma_poller **out_poller)
{
	struct spdk_nvmf_rdma_poller		*poller;
	struct spdk_rdma_provider_srq_init_attr	srq_init_attr;
	struct spdk_nvmf_rdma_resource_opts	opts;
	int					num_cqe, rc;
	uint32_t				events	= SPDK_INTERRUPT_EVENT_IN | SPDK_INTERRUPT_EVENT_OUT;

	poller = calloc(1, sizeof(*poller));
	if (!poller) {
		SPDK_ERRLOG("Unable to allocate memory for new RDMA poller\n");
		return -1;
	}

	poller->device = device;
	poller->group = rgroup;
	*out_poller = poller;

	RB_INIT(&poller->qpairs);
	STAILQ_INIT(&poller->qpairs_pending_send);
	STAILQ_INIT(&poller->qpairs_pending_recv);
	TAILQ_INIT(&poller->active_qpairs);

	TAILQ_INSERT_TAIL(&rgroup->pollers, poller, link);
	SPDK_DEBUGLOG(rdma, "Create poller %p on device %p in poll group %p.\n", poller, device, rgroup);
	if (rtransport->rdma_opts.no_srq == false && device->num_srq < device->attr.max_srq) {
		if ((int)rtransport->rdma_opts.max_srq_depth > device->attr.max_srq_wr) {
			SPDK_WARNLOG("Requested SRQ depth %u, max supported by dev %s is %d\n",
				     rtransport->rdma_opts.max_srq_depth, device->context->device->name, device->attr.max_srq_wr);
		}
		poller->max_srq_depth = spdk_min((int)rtransport->rdma_opts.max_srq_depth, device->attr.max_srq_wr);

		device->num_srq++;
		memset(&srq_init_attr, 0, sizeof(srq_init_attr));
		srq_init_attr.pd = device->pd;
		srq_init_attr.stats = &poller->stat.qp_stats.recv;
		srq_init_attr.srq_init_attr.attr.max_wr = poller->max_srq_depth;
		srq_init_attr.srq_init_attr.attr.max_sge = spdk_min(device->attr.max_sge, NVMF_DEFAULT_RX_SGE);
		poller->srq = spdk_rdma_provider_srq_create(&srq_init_attr);
		if (!poller->srq) {
			SPDK_ERRLOG("Unable to create shared receive queue, errno %d\n", errno);
			return -1;
		}

		opts.qp = poller->srq;
		opts.map = device->map;
		opts.qpair = NULL;
		opts.shared = true;
		opts.max_queue_depth = poller->max_srq_depth;
		opts.in_capsule_data_size = rtransport->transport.opts.in_capsule_data_size;

		poller->resources = nvmf_rdma_resources_create(&opts);
		if (!poller->resources) {
			SPDK_ERRLOG("Unable to allocate resources for shared receive queue.\n");
			return -1;
		}
	}

	/*
	 * When using an srq, we can limit the completion queue at startup.
	 * The following formula represents the calculation:
	 * num_cqe = num_recv + num_data_wr + num_send_wr.
	 * where num_recv=num_data_wr=and num_send_wr=poller->max_srq_depth
	 */
	if (poller->srq) {
		num_cqe = poller->max_srq_depth * 3;
	} else {
		num_cqe = rtransport->rdma_opts.num_cqe;
	}
	if (spdk_interrupt_mode_is_enabled()) {
		poller->comp_channel = ibv_create_comp_channel(device->context);
		if (poller->comp_channel == NULL) {
			SPDK_ERRLOG("Unable to create completion channel\n");
			return -1;
		}

		rc = spdk_fd_set_nonblock(poller->comp_channel->fd);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to set completion channel fd %d to non-blocking\n", poller->comp_channel->fd);
			return -1;
		}
	}
	poller->cq = ibv_create_cq(device->context, num_cqe, poller, poller->comp_channel, 0);
	if (!poller->cq) {
		SPDK_ERRLOG("Unable to create completion queue\n");
		return -1;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		poller->cq_intr = SPDK_INTERRUPT_REGISTER_FOR_EVENTS(poller->comp_channel->fd,
				  events, nvmf_rdma_poll_group_intr, poller);
		if (!poller->cq_intr) {
			SPDK_ERRLOG("Unable to register CQ\n");
			return -1;
		}

		/* Request notification for next event before polling */
		rc = ibv_req_notify_cq(poller->cq, 0);
		if (rc != 0) {
			SPDK_ERRLOG("ibv_req_notify_cq failed: %s\n", spdk_strerror(errno));
			return -1;
		}
	}
	poller->num_cqe = num_cqe;
	return 0;
}

static void
_nvmf_rdma_register_poller_in_group(void *c)
{
	struct spdk_nvmf_rdma_poller	*poller = NULL;
	struct poller_manage_ctx	*ctx = c;
	struct spdk_nvmf_rdma_device	*device;
	int				rc;

	rc = nvmf_rdma_poller_create(ctx->rtransport, ctx->rgroup, ctx->device, &poller);
	if (rc < 0 && poller) {
		nvmf_rdma_poller_destroy(poller);
	}

	device = ctx->device;
	if (nvmf_rdma_all_pollers_management_done(ctx)) {
		device->is_ready = true;
	}
}

/*
 * Interrupt callback for poll group - called when CQ has events ready
 */
static int
nvmf_rdma_poll_group_intr(void *ctx)
{
	struct spdk_nvmf_rdma_poller *poller = ctx;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int rc = 0;
	int count = 0;

	rtransport = SPDK_CONTAINEROF(poller->group->group.transport, struct spdk_nvmf_rdma_transport,
				      transport);
	rc = ibv_get_cq_event(poller->comp_channel, &ev_cq, &ev_ctx);
	if (rc != 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			/* No event available - spurious wakeup */
			return 0;
		} else {
			SPDK_ERRLOG("ibv_get_cq_event failed: %s\n", spdk_strerror(errno));
			return -1;
		}
	}
	/* CQ events has to be acknowledged to avoid spurious interrupt */
	ibv_ack_cq_events(ev_cq, 1);
	rc = ibv_req_notify_cq(ev_cq, 0);
	if (rc != 0) {
		SPDK_ERRLOG("ibv_req_notify_cq failed: %s\n",
			    spdk_strerror(errno));
		return -1;
	}
	do {
		rc = nvmf_rdma_poller_poll(rtransport, poller);
		if (rc > 0) {
			count += rc;
		}
	} while (rc > 0);

	return rc ? rc : count;
}

static void nvmf_rdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group);

static struct spdk_nvmf_transport_poll_group *
nvmf_rdma_poll_group_create(struct spdk_nvmf_transport *transport,
			    struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*poller;
	struct spdk_nvmf_rdma_device		*device;
	int					rc;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	rgroup = calloc(1, sizeof(*rgroup));
	if (!rgroup) {
		return NULL;
	}

	TAILQ_INIT(&rgroup->pollers);

	TAILQ_FOREACH(device, &rtransport->devices, link) {
		rc = nvmf_rdma_poller_create(rtransport, rgroup, device, &poller);
		if (rc < 0) {
			nvmf_rdma_poll_group_destroy(&rgroup->group);
			return NULL;
		}
	}

	TAILQ_INSERT_TAIL(&rtransport->poll_groups, rgroup, link);
	if (rtransport->conn_sched.next_admin_pg == NULL) {
		rtransport->conn_sched.next_admin_pg = rgroup;
		rtransport->conn_sched.next_io_pg = rgroup;
	}

	return &rgroup->group;
}

static uint32_t
nvmf_poll_group_get_io_qpair_count(struct spdk_nvmf_poll_group *pg)
{
	uint32_t count;

	/* Just assume that unassociated qpairs will eventually be io
	 * qpairs.  This is close enough for the use cases for this
	 * function.
	 */
	pthread_mutex_lock(&pg->mutex);
	count = pg->stat.current_io_qpairs + pg->current_unassociated_qpairs;
	pthread_mutex_unlock(&pg->mutex);

	return count;
}

static struct spdk_nvmf_transport_poll_group *
nvmf_rdma_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_poll_group **pg;
	struct spdk_nvmf_transport_poll_group *result;
	uint32_t count;

	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);

	if (TAILQ_EMPTY(&rtransport->poll_groups)) {
		return NULL;
	}

	if (qpair->qid == 0) {
		pg = &rtransport->conn_sched.next_admin_pg;
	} else {
		struct spdk_nvmf_rdma_poll_group *pg_min, *pg_start, *pg_current;
		uint32_t min_value;

		pg = &rtransport->conn_sched.next_io_pg;
		pg_min = *pg;
		pg_start = *pg;
		pg_current = *pg;
		min_value = nvmf_poll_group_get_io_qpair_count(pg_current->group.group);

		while (1) {
			count = nvmf_poll_group_get_io_qpair_count(pg_current->group.group);

			if (count < min_value) {
				min_value = count;
				pg_min = pg_current;
			}

			pg_current = TAILQ_NEXT(pg_current, link);
			if (pg_current == NULL) {
				pg_current = TAILQ_FIRST(&rtransport->poll_groups);
			}

			if (pg_current == pg_start || min_value == 0) {
				break;
			}
		}
		*pg = pg_min;
	}

	assert(*pg != NULL);

	result = &(*pg)->group;

	*pg = TAILQ_NEXT(*pg, link);
	if (*pg == NULL) {
		*pg = TAILQ_FIRST(&rtransport->poll_groups);
	}

	return result;
}

static void
nvmf_rdma_poller_destroy(struct spdk_nvmf_rdma_poller *poller)
{
	struct spdk_nvmf_rdma_qpair	*qpair, *tmp_qpair;
	int				rc;

	TAILQ_REMOVE(&poller->group->pollers, poller, link);
	RB_FOREACH_SAFE(qpair, qpairs_tree, &poller->qpairs, tmp_qpair) {
		nvmf_rdma_qpair_destroy(qpair);
	}

	if (poller->srq) {
		if (poller->resources) {
			nvmf_rdma_resources_destroy(poller->resources);
		}
		spdk_rdma_provider_srq_destroy(poller->srq);
		SPDK_DEBUGLOG(rdma, "Destroyed RDMA shared queue %p\n", poller->srq);
	}

	spdk_interrupt_unregister(&poller->cq_intr);
	if (poller->comp_channel) {
		ibv_destroy_comp_channel(poller->comp_channel);
	}
	if (poller->cq) {
		rc = ibv_destroy_cq(poller->cq);
		if (rc != 0) {
			SPDK_ERRLOG("Destroy cq return %d, error: %s\n", rc, strerror(errno));
		}
	}

	if (poller->destroy_cb) {
		poller->destroy_cb(poller->destroy_cb_ctx);
		poller->destroy_cb = NULL;
	}

	free(poller);
}

static void
nvmf_rdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup, *next_rgroup;
	struct spdk_nvmf_rdma_poller		*poller, *tmp;
	struct spdk_nvmf_rdma_transport		*rtransport;

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);
	if (!rgroup) {
		return;
	}

	TAILQ_FOREACH_SAFE(poller, &rgroup->pollers, link, tmp) {
		nvmf_rdma_poller_destroy(poller);
	}

	if (rgroup->group.transport == NULL) {
		/* Transport can be NULL when nvmf_rdma_poll_group_create()
		 * calls this function directly in a failure path. */
		free(rgroup);
		return;
	}

	rtransport = SPDK_CONTAINEROF(rgroup->group.transport, struct spdk_nvmf_rdma_transport, transport);

	next_rgroup = TAILQ_NEXT(rgroup, link);
	TAILQ_REMOVE(&rtransport->poll_groups, rgroup, link);
	if (next_rgroup == NULL) {
		next_rgroup = TAILQ_FIRST(&rtransport->poll_groups);
	}
	if (rtransport->conn_sched.next_admin_pg == rgroup) {
		rtransport->conn_sched.next_admin_pg = next_rgroup;
	}
	if (rtransport->conn_sched.next_io_pg == rgroup) {
		rtransport->conn_sched.next_io_pg = next_rgroup;
	}

	free(rgroup);
}

static void
nvmf_rdma_qpair_reject_connection(struct spdk_nvmf_rdma_qpair *rqpair)
{
	if (rqpair->cm_id != NULL) {
		nvmf_rdma_event_reject(rqpair->cm_id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
	}
}

static int
nvmf_rdma_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			 struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poller		*poller;
	int					rc;

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	device = rqpair->device;

	TAILQ_FOREACH(poller, &rgroup->pollers, link) {
		if (poller->device == device) {
			break;
		}
	}

	if (!poller) {
		SPDK_ERRLOG("No poller found for device.\n");
		return -1;
	}

	if (poller->need_destroy) {
		SPDK_ERRLOG("Poller is destroying.\n");
		return -1;
	}

	rqpair->poller = poller;
	rqpair->srq = rqpair->poller->srq;

	rc = nvmf_rdma_qpair_initialize(qpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to initialize nvmf_rdma_qpair with qpair=%p\n", qpair);
		rqpair->poller = NULL;
		rqpair->srq = NULL;
		return -1;
	}

	RB_INSERT(qpairs_tree, &poller->qpairs, rqpair);

	rc = nvmf_rdma_event_accept(rqpair->cm_id, rqpair);
	if (rc) {
		/* Try to reject, but we probably can't */
		nvmf_rdma_qpair_reject_connection(rqpair);
		return -1;
	}

	return 0;
}

static int
nvmf_rdma_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
			    struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	assert(group->transport->tgt != NULL);

	rqpair->destruct_channel = spdk_get_io_channel(group->transport->tgt);

	if (!rqpair->destruct_channel) {
		SPDK_WARNLOG("failed to get io_channel, qpair %p\n", qpair);
		return 0;
	}

	/* Sanity check that we get io_channel on the correct thread */
	if (qpair->group) {
		assert(qpair->group->thread == spdk_io_channel_get_thread(rqpair->destruct_channel));
	}

	return 0;
}

static void
nvmf_rdma_request_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_request	*rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(req->qpair->transport,
			struct spdk_nvmf_rdma_transport, transport);
	struct spdk_nvmf_rdma_qpair *rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair,
					      struct spdk_nvmf_rdma_qpair, qpair);

	/*
	 * AER requests are freed when a qpair is destroyed. The recv corresponding to that request
	 * needs to be returned to the shared receive queue or the poll group will eventually be
	 * starved of RECV structures.
	 */
	if (rqpair->srq && rdma_req->recv) {
		int rc;
		struct ibv_recv_wr *bad_recv_wr;

		spdk_rdma_provider_srq_queue_recv_wrs(rqpair->srq, &rdma_req->recv->wr);
		rc = spdk_rdma_provider_srq_flush_recv_wrs(rqpair->srq, &bad_recv_wr);
		if (rc) {
			SPDK_ERRLOG("Unable to re-post rx descriptor\n");
		}
	}

	_nvmf_rdma_request_free(rdma_req, rtransport);
}

static void
nvmf_rdma_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(req->qpair->transport,
			struct spdk_nvmf_rdma_transport, transport);
	struct spdk_nvmf_rdma_request	*rdma_req = SPDK_CONTAINEROF(req,
			struct spdk_nvmf_rdma_request, req);
	struct spdk_nvmf_rdma_qpair     *rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair,
			struct spdk_nvmf_rdma_qpair, qpair);

	if (spdk_unlikely(rqpair->ibv_in_error_state)) {
		/* The connection is dead. Move the request directly to the completed state. */
		rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
	} else {
		/* The connection is alive, so process the request as normal */
		rdma_req->state = RDMA_REQUEST_STATE_EXECUTED;
	}

	nvmf_rdma_request_process(rtransport, rdma_req);
}

/*
 * [한국어]
 * nvmf_rdma_close_qpair - QP graceful shutdown 진입점 (transport vtable 콜백).
 *
 * @qpair: 종료할 QP (공통 spdk_nvmf_qpair, 본 함수가 rdma_qpair 로 캐스팅).
 * @cb_fn/cb_arg: 종료 완료 콜백.
 *
 * NVMe-oF 상위 레이어 (lib/nvmf/ctrlr.c 등) 가 QP 를 종료시키기로 결정했을 때 호출.
 * 본 함수는 to_close 플래그만 세팅 후 destroy_drained 호출 — 실제 destroy 는 모든 in-flight
 * 가 drain 된 후에 일어남 (비동기).
 *
 * 단계:
 *   1. to_close=true 마킹 — 이후 request_process 가 새 NEW 진입 시 즉시 COMPLETED 로 보냄.
 *   2. UNINITIALIZED 상태 (CONNECT 도 못 받음) 면 reject_connection 으로 rdma_reject 보냄.
 *   3. rdma_qp 존재하면 rdma_disconnect 발행 — peer 에게 disconnect 통지.
 *   4. destroy_drained_qpair 시도 — drain 이 즉시 완료되면 destroy, 아니면 다음 poller iter.
 *   5. cb_fn 호출 (호출자에게 close 완료 통지).
 *
 * 비동기 destroy 흐름:
 *   close_qpair → to_close=true → 다음 poller_poll 들이 in-flight 정리 →
 *   destroy_drained_qpair 가 조건 충족 시 qpair_destroy 호출.
 *
 * 호출 체인:
 *   spdk_nvmf_qpair_disconnect / 상위 NVMe-oF → transport vtable.close_qpair = [본 함수]
 */
static void
nvmf_rdma_close_qpair(struct spdk_nvmf_qpair *qpair,
		      spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_rdma_qpair *rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	/* [한국어] to_close=true — 이후 NEW request 들 모두 즉시 COMPLETED 로 보냄. */
	rqpair->to_close = true;

	/* [한국어] CONNECT 도 못 받은 상태면 RDMA CM reject 발행 (peer 가 connection 실패 인식). */
	if (rqpair->qpair.state == SPDK_NVMF_QPAIR_UNINITIALIZED) {
		nvmf_rdma_qpair_reject_connection(rqpair);
	}
	/* [한국어] rdma_qp 가 있으면 정상 disconnect 통지 (RDMA_CM_EVENT_DISCONNECTED 발생). */
	if (rqpair->rdma_qp) {
		spdk_rdma_provider_qp_disconnect(rqpair->rdma_qp);
	}

	/* [한국어] drained 조건 (queue_depth=0 등) 충족 시 즉시 destroy, 아니면 폴링 진행 중. */
	nvmf_rdma_destroy_drained_qpair(rqpair);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

static struct spdk_nvmf_rdma_qpair *
get_rdma_qpair_from_wc(struct spdk_nvmf_rdma_poller *rpoller, struct ibv_wc *wc)
{
	struct spdk_nvmf_rdma_qpair find;

	find.qp_num = wc->qp_num;

	return RB_FIND(qpairs_tree, &rpoller->qpairs, &find);
}

#ifdef DEBUG
static int
nvmf_rdma_req_is_completing(struct spdk_nvmf_rdma_request *rdma_req)
{
	return rdma_req->state == RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST ||
	       rdma_req->state == RDMA_REQUEST_STATE_COMPLETING;
}
#endif

static void
_poller_reset_failed_recvs(struct spdk_nvmf_rdma_poller *rpoller, struct ibv_recv_wr *bad_recv_wr,
			   int rc)
{
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_nvmf_rdma_wr	*bad_rdma_wr;

	SPDK_ERRLOG("Failed to post a recv for the poller %p with errno %d\n", rpoller, -rc);
	while (bad_recv_wr != NULL) {
		bad_rdma_wr = (struct spdk_nvmf_rdma_wr *)bad_recv_wr->wr_id;
		rdma_recv = SPDK_CONTAINEROF(bad_rdma_wr, struct spdk_nvmf_rdma_recv, rdma_wr);

		rdma_recv->qpair->current_recv_depth++;
		bad_recv_wr = bad_recv_wr->next;
		SPDK_ERRLOG("Failed to post a recv for the qpair %p with errno %d\n", rdma_recv->qpair, -rc);
		spdk_nvmf_qpair_disconnect(&rdma_recv->qpair->qpair);
	}
}

static void
_qp_reset_failed_recvs(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_recv_wr *bad_recv_wr, int rc)
{
	SPDK_ERRLOG("Failed to post a recv for the qpair %p with errno %d\n", rqpair, -rc);
	while (bad_recv_wr != NULL) {
		bad_recv_wr = bad_recv_wr->next;
		rqpair->current_recv_depth++;
	}
	spdk_nvmf_qpair_disconnect(&rqpair->qpair);
}

static void
_poller_submit_recvs(struct spdk_nvmf_rdma_transport *rtransport,
		     struct spdk_nvmf_rdma_poller *rpoller)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct ibv_recv_wr		*bad_recv_wr;
	int				rc;

	if (rpoller->srq) {
		rc = spdk_rdma_provider_srq_flush_recv_wrs(rpoller->srq, &bad_recv_wr);
		if (spdk_unlikely(rc)) {
			_poller_reset_failed_recvs(rpoller, bad_recv_wr, rc);
		}
	} else {
		while (!STAILQ_EMPTY(&rpoller->qpairs_pending_recv)) {
			rqpair = STAILQ_FIRST(&rpoller->qpairs_pending_recv);
			rc = spdk_rdma_provider_qp_flush_recv_wrs(rqpair->rdma_qp, &bad_recv_wr);
			if (spdk_unlikely(rc)) {
				_qp_reset_failed_recvs(rqpair, bad_recv_wr, rc);
			}
			STAILQ_REMOVE_HEAD(&rpoller->qpairs_pending_recv, recv_link);
		}
	}
}

static void
_qp_reset_failed_sends(struct spdk_nvmf_rdma_transport *rtransport,
		       struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_send_wr *bad_wr, int rc)
{
	struct spdk_nvmf_rdma_wr	*bad_rdma_wr;
	struct spdk_nvmf_rdma_request	*prev_rdma_req = NULL, *cur_rdma_req = NULL;

	SPDK_ERRLOG("Failed to post a send for the qpair %p with errno %d\n", rqpair, -rc);
	for (; bad_wr != NULL; bad_wr = bad_wr->next) {
		bad_rdma_wr = (struct spdk_nvmf_rdma_wr *)bad_wr->wr_id;
		assert(rqpair->current_send_depth > 0);
		rqpair->current_send_depth--;
		switch (bad_rdma_wr->type) {
		case RDMA_WR_TYPE_DATA:
			cur_rdma_req = SPDK_CONTAINEROF(bad_rdma_wr, struct spdk_nvmf_rdma_request, data_wr);
			if (bad_wr->opcode == IBV_WR_RDMA_READ) {
				assert(rqpair->current_read_depth > 0);
				rqpair->current_read_depth--;
			}
			break;
		case RDMA_WR_TYPE_SEND:
			cur_rdma_req = SPDK_CONTAINEROF(bad_rdma_wr, struct spdk_nvmf_rdma_request, rsp_wr);
			break;
		default:
			SPDK_ERRLOG("Found a RECV in the list of pending SEND requests for qpair %p\n", rqpair);
			prev_rdma_req = cur_rdma_req;
			continue;
		}

		if (prev_rdma_req == cur_rdma_req) {
			/* this request was handled by an earlier wr. i.e. we were performing an nvme read. */
			/* We only have to check against prev_wr since each requests wrs are contiguous in this list. */
			continue;
		}

		switch (cur_rdma_req->state) {
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			cur_rdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, cur_rdma_req, state_link);
			cur_rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
		case RDMA_REQUEST_STATE_COMPLETING:
			cur_rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
			break;
		default:
			SPDK_ERRLOG("Found a request in a bad state %d when draining pending SEND requests for qpair %p\n",
				    cur_rdma_req->state, rqpair);
			continue;
		}

		nvmf_rdma_request_process(rtransport, cur_rdma_req);
		prev_rdma_req = cur_rdma_req;
	}

	if (spdk_nvmf_qpair_is_active(&rqpair->qpair)) {
		/* Disconnect the connection. */
		spdk_nvmf_qpair_disconnect(&rqpair->qpair);
	}

}

static void
_poller_submit_sends(struct spdk_nvmf_rdma_transport *rtransport,
		     struct spdk_nvmf_rdma_poller *rpoller)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct ibv_send_wr		*bad_wr = NULL;
	int				rc;

	while (!STAILQ_EMPTY(&rpoller->qpairs_pending_send)) {
		rqpair = STAILQ_FIRST(&rpoller->qpairs_pending_send);
		rc = spdk_rdma_provider_qp_flush_send_wrs(rqpair->rdma_qp, &bad_wr);

		/* bad wr always points to the first wr that failed. */
		if (spdk_unlikely(rc)) {
			_qp_reset_failed_sends(rtransport, rqpair, bad_wr, rc);
		}
		STAILQ_REMOVE_HEAD(&rpoller->qpairs_pending_send, send_link);
	}
}

static const char *
nvmf_rdma_wr_type_str(enum spdk_nvmf_rdma_wr_type wr_type)
{
	switch (wr_type) {
	case RDMA_WR_TYPE_RECV:
		return "RECV";
	case RDMA_WR_TYPE_SEND:
		return "SEND";
	case RDMA_WR_TYPE_DATA:
		return "DATA";
	default:
		SPDK_ERRLOG("Unknown WR type %d\n", wr_type);
		SPDK_UNREACHABLE();
	}
}

static inline void
nvmf_rdma_log_wc_status(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_wc *wc)
{
	enum spdk_nvmf_rdma_wr_type wr_type = ((struct spdk_nvmf_rdma_wr *)wc->wr_id)->type;

	if (wc->status == IBV_WC_WR_FLUSH_ERR) {
		/* If qpair is in ERR state, we will receive completions for all posted and not completed
		 * Work Requests with IBV_WC_WR_FLUSH_ERR status. Don't log an error in that case */
		SPDK_DEBUGLOG(rdma,
			      "Error on CQ %p, (qp state %d, in_error %d) request 0x%lu, type %s, status: (%d): %s\n",
			      rqpair->poller->cq, rqpair->qpair.state, rqpair->ibv_in_error_state, wc->wr_id,
			      nvmf_rdma_wr_type_str(wr_type), wc->status, ibv_wc_status_str(wc->status));
	} else {
		SPDK_ERRLOG("Error on CQ %p, (qp state %d, in_error %d) request 0x%lu, type %s, status: (%d): %s\n",
			    rqpair->poller->cq, rqpair->qpair.state, rqpair->ibv_in_error_state, wc->wr_id,
			    nvmf_rdma_wr_type_str(wr_type), wc->status, ibv_wc_status_str(wc->status));
	}
}

/*
 * [한국어]
 * nvmf_rdma_poller_poll - 단일 poller 의 CQ 폴링 + WR type 별 dispatch.
 *
 * @rtransport: 전체 RDMA transport.
 * @rpoller: 폴링할 poller (= device + group 페어).
 * @return: 처리한 완료 수 (SEND only — request 완료 카운트), 음수 = ibv_poll_cq 에러.
 *
 * ★ NVMe-oF RDMA 의 hot path ★ — 매 reactor iteration 마다 호출.
 *
 * 단계:
 *   1. need_destroy 면 (device removal 등) 모든 QP destroy 시도 후 0 반환.
 *   2. ibv_poll_cq 로 최대 32개 CQE 한 번에 수확 (batching).
 *      - 0: 통계 idle_polls++ (busy 아님 — backoff 결정에 활용)
 *      - 음수: 에러 → 즉시 -1 반환 (호출자 logic 중단)
 *   3. 각 CQE 의 wr_id → rdma_wr 캐스팅 → type 분기:
 *      - SEND: rsp_wr 의 rdma_wr → request 의 응답 SEND 완료 → state COMPLETED 전이
 *              + send_depth 감소 (RDMA WRITE chain + SEND 자체 = num_outstanding_data_wr+1)
 *      - RECV: 새 capsule 도착 → SRQ 모드면 wc->qp_num 으로 QP 역추적 → request 풀에서
 *              free request 할당 → NEW 진입
 *      - RDMA READ/WRITE: data transfer 완료 → 다음 state 로
 *   4. 각 dispatch 후 nvmf_rdma_request_process 호출 → state machine 진행.
 *
 * 배치 크기 32: 한 폴링에서 32개 CQE 처리. ibv_poll_cq 1회 syscall 비용을 amortize.
 * 너무 크게 잡으면 stack 의 wc 배열 크기 부담.
 *
 * 통계: polls/idle_polls/completions/request_latency — RPC 로 노출되어 모니터링.
 *
 * 실행 컨텍스트: reactor 스레드. 다른 thread 가 같은 poller 의 CQ 폴링 금지 (lockless).
 *
 * 호출 체인:
 *   nvmf_rdma_poll_group_poll → [본 함수] → ibv_poll_cq → switch(wr_type) →
 *     nvmf_rdma_request_process (state machine 진행)
 */
static int
nvmf_rdma_poller_poll(struct spdk_nvmf_rdma_transport *rtransport,
		      struct spdk_nvmf_rdma_poller *rpoller)
{
	struct ibv_wc wc[32];
	/* [한국어] stack 의 CQE 배열 — 32 = ibv_poll_cq batching 한도. */
	struct spdk_nvmf_rdma_wr	*rdma_wr;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_nvmf_rdma_qpair	*rqpair, *tmp_rqpair;
	int reaped, i;
	int count = 0;
	int rc;
	bool error = false;
	uint64_t poll_tsc = spdk_get_ticks();

	/* [한국어] poller destroy 예약된 상태 — CQ 폴링 의미 없으므로 QP 들만 강제 정리.
	 * RB_FOREACH_SAFE: drain 중에 RB tree 에서 QP 제거되므로 next 미리 캐싱. */
	if (spdk_unlikely(rpoller->need_destroy)) {
		/* If qpair is closed before poller destroy, nvmf_rdma_destroy_drained_qpair may not
		 * be called because we cannot poll anything from cq. So we call that here to force
		 * destroy the qpair after to_close turning true.
		 */
		RB_FOREACH_SAFE(rqpair, qpairs_tree, &rpoller->qpairs, tmp_rqpair) {
			nvmf_rdma_destroy_drained_qpair(rqpair);
		}
		return 0;
	}

	/* Poll for completing operations. */
	/* [한국어] ibverbs CQ poll — non-blocking, 0 = 없음, 음수 = 에러, 양수 = 수확된 CQE 수.
	 * polled-mode 패턴: 인터럽트 안 쓰고 매 iteration 호출 → 낮은 latency. */
	reaped = ibv_poll_cq(rpoller->cq, 32, wc);
	if (spdk_unlikely(reaped < 0)) {
		SPDK_ERRLOG("Error polling CQ! (%d): %s\n",
			    errno, spdk_strerror(errno));
		return -1;
	} else if (reaped == 0) {
		/* [한국어] 빈 poll — backoff 결정 / 인터럽트 모드 wakeup 휴리스틱에 활용. */
		rpoller->stat.idle_polls++;
	}

	rpoller->stat.polls++;
	rpoller->stat.completions += reaped;

	for (i = 0; i < reaped; i++) {

		rdma_wr = (struct spdk_nvmf_rdma_wr *)wc[i].wr_id;

		switch (rdma_wr->type) {
		case RDMA_WR_TYPE_SEND:
			rdma_req = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvmf_rdma_request, rsp_wr);
			rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

			if (spdk_likely(!wc[i].status)) {
				count++;
				assert(wc[i].opcode == IBV_WC_SEND);
				assert(nvmf_rdma_req_is_completing(rdma_req));
			}

			rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
			/* RDMA_WRITE operation completed. +1 since it was chained with rsp WR */
			assert(rqpair->current_send_depth >= (uint32_t)rdma_req->num_outstanding_data_wr + 1);
			rqpair->current_send_depth -= rdma_req->num_outstanding_data_wr + 1;
			rdma_req->num_outstanding_data_wr = 0;

			nvmf_rdma_request_process(rtransport, rdma_req);
			break;
		case RDMA_WR_TYPE_RECV:
			/* rdma_recv->qpair will be invalid if using an SRQ.  In that case we have to get the qpair from the wc. */
			rdma_recv = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvmf_rdma_recv, rdma_wr);
			/* [한국어] === RECV case ===
			 * SRQ 모드: SRQ 는 여러 QP 가 공유하므로 RECV CQE 에 qpair 정보가 없음.
			 * wc->qp_num 으로 QP 역추적 — get_rdma_qpair_from_wc 가 poller 의 RB tree 검색.
			 * QP 가 이미 destroy 됐으면 (= 늦게 도착한 stale CQE) RECV WR 만 SRQ 에 재게시. */
			if (rpoller->srq != NULL) {
				rdma_recv->qpair = get_rdma_qpair_from_wc(rpoller, &wc[i]);
				/* It is possible that there are still some completions for destroyed QP
				 * associated with SRQ. We just ignore these late completions and re-post
				 * receive WRs back to SRQ.
				 */
				if (spdk_unlikely(NULL == rdma_recv->qpair)) {
					struct ibv_recv_wr *bad_wr;

					/* [한국어] WR chain 끊고 SRQ 큐에 enqueue → flush 로 ibv_post_srq_recv 발행.
					 * WR 자체는 재사용 (free 하지 않음). */
					rdma_recv->wr.next = NULL;
					spdk_rdma_provider_srq_queue_recv_wrs(rpoller->srq, &rdma_recv->wr);
					rc = spdk_rdma_provider_srq_flush_recv_wrs(rpoller->srq, &bad_wr);
					if (rc) {
						SPDK_ERRLOG("Failed to re-post recv WR to SRQ, err %d\n", rc);
					}
					continue;
				}
			}
			rqpair = rdma_recv->qpair;

			assert(rqpair != NULL);
			if (spdk_likely(!wc[i].status)) {
				assert(wc[i].opcode == IBV_WC_RECV);
				/* [한국어] queue depth 초과 = 호스트가 spec 위반 (max_queue_depth 보다 많이 보냄)
				 * → connection 강제 종료. 정상 호스트라면 NVMe-oF spec 의 SQ size 협상 결과를 지킴. */
				if (rqpair->current_recv_depth >= rqpair->max_queue_depth) {
					spdk_nvmf_qpair_disconnect(&rqpair->qpair);
					break;
				}
			}

			/* [한국어] RECV 정상 처리:
			 *  - WR chain 끊기 (이전 batch 영향 차단)
			 *  - recv_depth 카운터 증가
			 *  - receive_tsc 기록 (latency 측정)
			 *  - resources.incoming_queue 에 enqueue → request_process 가 NEW 진입 시 dequeue
			 *  - 빈 큐 → 활성 큐 전이 시 active_qpairs TAILQ 등록 (다음 polling pass 에서 처리) */
			rdma_recv->wr.next = NULL;
			rqpair->current_recv_depth++;
			rdma_recv->receive_tsc = poll_tsc;
			rpoller->stat.requests++;
			STAILQ_INSERT_TAIL(&rqpair->resources->incoming_queue, rdma_recv, link);
			if (rqpair->qpair.queue_depth == 0) {
				assert(TAILQ_ENTRY_NOT_ENQUEUED(rqpair, active_link));
				TAILQ_INSERT_TAIL(&rpoller->active_qpairs, rqpair, active_link);
			}
			rqpair->qpair.queue_depth++;
			break;
		case RDMA_WR_TYPE_DATA:
			/* [한국어] === RDMA READ/WRITE case ===
			 * data_wr 의 rdma_wr → request 역추적. RDMA READ (write payload 가져오기) 또는
			 * RDMA WRITE (read payload 보내기) 완료. */
			rdma_req = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvmf_rdma_request, data_wr);
			rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

			assert(rdma_req->num_outstanding_data_wr > 0);

			/* [한국어] depth 회복: SEND 슬롯 + RDMA op 마다 1씩. */
			rqpair->current_send_depth--;
			rdma_req->num_outstanding_data_wr--;
			if (spdk_likely(!wc[i].status)) {
				assert(wc[i].opcode == IBV_WC_RDMA_READ);
				rqpair->current_read_depth--;
				/* wait for all outstanding reads associated with the same rdma_req to complete before proceeding. */
				/* [한국어] 한 request 의 모든 RDMA READ 가 끝났을 때만 다음 state 로 — 부분 완료
				 * 시점에 데이터 일부만 도착한 상태에서 bdev exec 하면 잘못된 데이터 읽음. */
				if (rdma_req->num_outstanding_data_wr == 0) {
					if (rdma_req->num_remaining_data_wr) {
						/* Only part of RDMA_READ operations was submitted, process the rest */
						/* [한국어] multi-SGL split: 한 번에 모든 WR 발행 못 한 경우 (depth 부족 등).
						 * 다음 chunk 발행을 위해 PENDING 큐에 재투입. */
						nvmf_rdma_request_reset_transfer_in(rdma_req, rtransport);
						rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING;
						nvmf_rdma_request_process(rtransport, rdma_req);
						break;
					}
					/* [한국어] 모든 데이터 도착 — bdev 실행 단계로. */
					rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
					nvmf_rdma_request_process(rtransport, rdma_req);
				}
			} else {
				/* If the data transfer fails still force the queue into the error state,
				 * if we were performing an RDMA_READ, we need to force the request into a
				 * completed state since it wasn't linked to a send. However, in the RDMA_WRITE
				 * case, we should wait for the SEND to complete. */
				/* [한국어] 에러 처리 비대칭:
				 *  - RDMA READ 실패: SEND 와 연결돼 있지 않으므로 SEND 완료를 기다릴 수 없음 →
				 *    여기서 직접 COMPLETED 로 전이.
				 *  - RDMA WRITE 실패: rsp SEND 가 chain 으로 발행되었으므로 SEND 완료 콜백을 기다림. */
				if (rdma_req->data.wr.opcode == IBV_WR_RDMA_READ) {
					rqpair->current_read_depth--;
					if (rdma_req->num_outstanding_data_wr == 0) {
						if (rdma_req->num_remaining_data_wr) {
							/* Partially sent request is still in the pending_rdma_read_queue,
							 * remove it now before completing */
							rdma_req->num_remaining_data_wr = 0;
							STAILQ_REMOVE(&rqpair->pending_rdma_read_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
						}
						rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
						nvmf_rdma_request_process(rtransport, rdma_req);
					}
				}
			}
			break;
		default:
			SPDK_ERRLOG("Received an unknown opcode on the CQ: %d\n", wc[i].opcode);
			continue;
		}

		/* Handle error conditions */
		/* [한국어] === 공통 에러 후처리 ===
		 * 모든 WR type 의 에러: ibv_in_error_state=true 마킹 → 이후 request_process 가
		 * 모든 request 를 COMPLETED 로 fast path. error 플래그로 반환값 -1 결정. */
		if (spdk_unlikely(wc[i].status)) {
			rqpair->ibv_in_error_state = true;
			nvmf_rdma_log_wc_status(rqpair, &wc[i]);

			error = true;

			/* [한국어] QP active 면 graceful disconnect, 이미 종료 중이면 강제 destroy. */
			if (spdk_nvmf_qpair_is_active(&rqpair->qpair)) {
				/* Disconnect the connection. */
				spdk_nvmf_qpair_disconnect(&rqpair->qpair);
			} else {
				nvmf_rdma_destroy_drained_qpair(rqpair);
			}
			continue;
		}

		/* [한국어] 정상 처리지만 QP 가 active 아님 (disconnect 진행 중) — drain 완료 시 destroy. */
		if (spdk_unlikely(!spdk_nvmf_qpair_is_active(&rqpair->qpair))) {
			nvmf_rdma_destroy_drained_qpair(rqpair);
		}
	}

	/* [한국어] PENDING 큐에서 대기 중이던 request 들 다시 처리 시도 (depth 회복됐을 수 있음). */
	nvmf_rdma_poller_process_pending_qpairs(rtransport, rpoller);

	if (spdk_unlikely(error == true)) {
		return -1;
	}

	/* submit outstanding work requests. */
	/* [한국어] batching 종료 — 누적된 RECV / SEND WR 들을 한 번의 ibv_post_recv / post_send
	 * 로 발행 (no_wr_batching 옵션 false 일 때). syscall 횟수 amortize. */
	_poller_submit_recvs(rtransport, rpoller);
	_poller_submit_sends(rtransport, rpoller);

	return count;
}

static void
_nvmf_rdma_remove_destroyed_device(void *c)
{
	struct spdk_nvmf_rdma_transport	*rtransport = c;
	struct spdk_nvmf_rdma_device	*device, *device_tmp;
	int				rc;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, device_tmp) {
		if (device->ready_to_destroy) {
			destroy_ib_device(rtransport, device);
		}
	}

	free_poll_fds(rtransport);
	rc = generate_poll_fds(rtransport);
	/* cannot handle fd allocation error here */
	if (rc != 0) {
		SPDK_ERRLOG("Failed to generate poll fds after remove ib device.\n");
	}
}

static void
_nvmf_rdma_remove_poller_in_group_cb(void *c)
{
	struct poller_manage_ctx	*ctx = c;
	struct spdk_nvmf_rdma_transport	*rtransport = ctx->rtransport;
	struct spdk_nvmf_rdma_device	*device = ctx->device;
	struct spdk_thread		*thread = ctx->thread;

	if (nvmf_rdma_all_pollers_management_done(c)) {
		/* destroy device when last poller is destroyed */
		device->ready_to_destroy = true;
		spdk_thread_send_msg(thread, _nvmf_rdma_remove_destroyed_device, rtransport);
	}
}

static void
_nvmf_rdma_remove_poller_in_group(void *c)
{
	struct poller_manage_ctx		*ctx = c;
	struct spdk_nvmf_rdma_poller            *rpoller;

	/* Check if the poller referred to by this context was already removed.
	 * If it was already removed, simply call the usual destroy_cb function
	 * directly.
	 */
	TAILQ_FOREACH(rpoller, &ctx->rgroup->pollers, link) {
		if (rpoller == ctx->rpoller) {
			break;
		}
	}

	if (rpoller != NULL) {
		rpoller->need_destroy = true;
		rpoller->destroy_cb_ctx = ctx;
		rpoller->destroy_cb = _nvmf_rdma_remove_poller_in_group_cb;
		/* qp will be disconnected after receiving a RDMA_CM_EVENT_DEVICE_REMOVAL event. */
		if (RB_EMPTY(&ctx->rpoller->qpairs)) {
			nvmf_rdma_poller_destroy(ctx->rpoller);
		}
	} else {
		_nvmf_rdma_remove_poller_in_group_cb(ctx);
	}
}

/*
 * [한국어]
 * nvmf_rdma_poll_group_poll - poll_group 의 모든 poller 를 순회하며 CQ 폴링.
 *
 * @group: NVMe-oF 공통 poll_group (transport vtable 콜백 시그니처).
 * @return: 처리한 CQE 수 (>0), 또는 첫 에러 코드 (음수).
 *
 * SPDK reactor 의 poller 콜백으로 매 iteration 마다 호출. poll_group 안에는 device 마다
 * poller 가 1개씩 있고, 각 poller 가 (device, group) 페어의 모든 QP 의 CQE 를 단일 CQ
 * 로 수확. 모든 poller 의 처리량 합산이 본 함수의 반환값.
 *
 * 에러 처리: rpoller_poll 이 음수 반환해도 즉시 멈추지 않고 다른 poller 는 계속 진행 —
 * 한 device 의 일시적 에러가 다른 device 의 처리를 막지 않게. 첫 에러만 rc2 에 기록.
 *
 * TAILQ_FOREACH_SAFE 사용 이유: rpoller_poll 안에서 device removal 처리 시 poller 가
 * destroy 될 수 있으므로 next pointer 미리 캐싱.
 *
 * 실행 컨텍스트: SPDK reactor 스레드, polled-mode (인터럽트 없음 — 단 cq_intr 등록 시
 * fd 트리거로 wakeup).
 *
 * 호출 체인:
 *   SPDK reactor poller 콜백 → [본 함수] → nvmf_rdma_poller_poll (per-device CQ poll)
 *     → ibv_poll_cq → nvmf_rdma_request_process (state machine 진행)
 */
static int
nvmf_rdma_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvmf_rdma_poller	*rpoller, *tmp;
	int				count = 0, rc, rc2 = 0;

	rtransport = SPDK_CONTAINEROF(group->transport, struct spdk_nvmf_rdma_transport, transport);
	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);

	/* [한국어] poll_group 의 모든 poller 순회 — device 1개당 poller 1개.
	 * _SAFE: poller_poll 내부에서 destroy 가능성 → next 미리 캐싱. */
	TAILQ_FOREACH_SAFE(rpoller, &rgroup->pollers, link, tmp) {
		rc = nvmf_rdma_poller_poll(rtransport, rpoller);
		if (spdk_unlikely(rc < 0)) {
			/* [한국어] 첫 에러만 기록하고 다른 poller 계속 처리 — fault isolation. */
			if (rc2 == 0) {
				rc2 = rc;
			}
			continue;
		}
		count += rc;
	}

	/* [한국어] 에러 있었으면 음수 반환 (호출자가 처리), 아니면 처리한 CQE 수 누적. */
	return rc2 ? rc2 : count;
}

static void
nvmf_rdma_trid_from_cm_id(struct rdma_cm_id *id,
			  struct spdk_nvme_transport_id *trid,
			  bool peer)
{
	struct sockaddr *saddr;
	uint16_t port;

	spdk_nvme_trid_populate_transport(trid, SPDK_NVME_TRANSPORT_RDMA);

	if (peer) {
		saddr = rdma_get_peer_addr(id);
	} else {
		saddr = rdma_get_local_addr(id);
	}
	switch (saddr->sa_family) {
	case AF_INET: {
		struct sockaddr_in *saddr_in = (struct sockaddr_in *)saddr;

		trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;
		inet_ntop(AF_INET, &saddr_in->sin_addr,
			  trid->traddr, sizeof(trid->traddr));
		if (peer) {
			port = ntohs(rdma_get_dst_port(id));
		} else {
			port = ntohs(rdma_get_src_port(id));
		}
		snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%u", port);
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 *saddr_in = (struct sockaddr_in6 *)saddr;
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV6;
		inet_ntop(AF_INET6, &saddr_in->sin6_addr,
			  trid->traddr, sizeof(trid->traddr));
		if (peer) {
			port = ntohs(rdma_get_dst_port(id));
		} else {
			port = ntohs(rdma_get_src_port(id));
		}
		snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%u", port);
		break;
	}
	default:
		SPDK_ERRLOG("Unsupported address family %d\n", saddr->sa_family);
		assert(false);
	}
}

static int
nvmf_rdma_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	if (rqpair->cm_id == NULL) {
		SPDK_WARNLOG("cm_id is NULL for qpair %p\n", qpair);
		return -1;
	}

	nvmf_rdma_trid_from_cm_id(rqpair->cm_id, trid, true);

	return 0;
}

static int
nvmf_rdma_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			       struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	if (rqpair->cm_id == NULL) {
		SPDK_WARNLOG("cm_id is NULL for qpair %p\n", qpair);
		return -1;
	}

	nvmf_rdma_trid_from_cm_id(rqpair->cm_id, trid, false);

	return 0;
}

static int
nvmf_rdma_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	memcpy(trid, &rqpair->listen_trid, sizeof(struct spdk_nvme_transport_id));

	return 0;
}

void
spdk_nvmf_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks)
{
	g_nvmf_hooks = *hooks;
}

static void
nvmf_rdma_request_set_abort_status(struct spdk_nvmf_request *req,
				   struct spdk_nvmf_rdma_request *rdma_req_to_abort,
				   struct spdk_nvmf_rdma_qpair *rqpair)
{
	rdma_req_to_abort->req.rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	rdma_req_to_abort->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	/* Ensure cid is correct in case abort was requested before IO is being executed */
	rdma_req_to_abort->req.rsp->nvme_cpl.sqid = 0;
	rdma_req_to_abort->req.rsp->nvme_cpl.status.p = 0;
	rdma_req_to_abort->req.rsp->nvme_cpl.cid = rdma_req_to_abort->req.cmd->nvme_cmd.cid;

	STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, rdma_req_to_abort, state_link);
	rdma_req_to_abort->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;

	req->rsp->nvme_cpl.cdw0 &= ~1U;	/* Command was successfully aborted. */
}

static int
_nvmf_rdma_qpair_abort_request(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_rdma_request *rdma_req_to_abort = SPDK_CONTAINEROF(
				req->req_to_abort, struct spdk_nvmf_rdma_request, req);
	struct spdk_nvmf_rdma_qpair *rqpair = SPDK_CONTAINEROF(req->req_to_abort->qpair,
					      struct spdk_nvmf_rdma_qpair, qpair);
	int rc;

	spdk_poller_unregister(&req->poller);

	switch (rdma_req_to_abort->state) {
	case RDMA_REQUEST_STATE_EXECUTING:
		rc = nvmf_ctrlr_abort_request(req);
		if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS) {
			return SPDK_POLLER_BUSY;
		}
		break;

	case RDMA_REQUEST_STATE_NEED_BUFFER:
		STAILQ_REMOVE(&rqpair->poller->group->group.pending_buf_queue,
			      &rdma_req_to_abort->req, spdk_nvmf_request, buf_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort, rqpair);
		break;

	case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
		STAILQ_REMOVE(&rqpair->pending_rdma_read_queue, rdma_req_to_abort,
			      spdk_nvmf_rdma_request, state_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort, rqpair);
		break;

	case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
		STAILQ_REMOVE(&rqpair->pending_rdma_write_queue, rdma_req_to_abort,
			      spdk_nvmf_rdma_request, state_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort, rqpair);
		break;

	case RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING:
		/* Remove req from the list here to re-use common function */
		STAILQ_REMOVE(&rqpair->pending_rdma_send_queue, rdma_req_to_abort,
			      spdk_nvmf_rdma_request, state_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort, rqpair);
		break;

	case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
		if (spdk_get_ticks() < req->timeout_tsc) {
			req->poller = SPDK_POLLER_REGISTER(_nvmf_rdma_qpair_abort_request, req, 0);
			return SPDK_POLLER_BUSY;
		}
		break;

	default:
		break;
	}

	spdk_nvmf_request_complete(req);
	return SPDK_POLLER_BUSY;
}

static void
nvmf_rdma_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_qpair *rqpair;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_transport *transport;
	uint16_t cid;
	uint32_t i, max_req_count;
	struct spdk_nvmf_rdma_request *rdma_req_to_abort = NULL, *rdma_req;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);
	transport = &rtransport->transport;

	cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;
	max_req_count = rqpair->srq == NULL ? rqpair->max_queue_depth : rqpair->poller->max_srq_depth;

	for (i = 0; i < max_req_count; i++) {
		rdma_req = &rqpair->resources->reqs[i];
		/* When SRQ == NULL, rqpair has its own requests and req.qpair pointer always points to the qpair
		 * When SRQ != NULL all rqpairs share common requests and qpair pointer is assigned when we start to
		 * process a request. So in both cases all requests which are not in FREE state have valid qpair ptr */
		if (rdma_req->state != RDMA_REQUEST_STATE_FREE && rdma_req->req.cmd->nvme_cmd.cid == cid &&
		    rdma_req->req.qpair == qpair) {
			rdma_req_to_abort = rdma_req;
			break;
		}
	}

	if (rdma_req_to_abort == NULL) {
		spdk_nvmf_request_complete(req);
		return;
	}

	req->req_to_abort = &rdma_req_to_abort->req;
	req->timeout_tsc = spdk_get_ticks() +
			   transport->opts.abort_timeout_sec * spdk_get_ticks_hz();
	req->poller = NULL;

	_nvmf_rdma_qpair_abort_request(req);
}

static void
nvmf_rdma_poll_group_dump_stat(struct spdk_nvmf_transport_poll_group *group,
			       struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvmf_rdma_poller *rpoller;

	assert(w != NULL);

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);

	spdk_json_write_named_uint64(w, "pending_data_buffer", rgroup->stat.pending_data_buffer);

	spdk_json_write_named_array_begin(w, "devices");

	TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "name",
					     ibv_get_device_name(rpoller->device->context->device));
		spdk_json_write_named_uint64(w, "polls",
					     rpoller->stat.polls);
		spdk_json_write_named_uint64(w, "idle_polls",
					     rpoller->stat.idle_polls);
		spdk_json_write_named_uint64(w, "completions",
					     rpoller->stat.completions);
		spdk_json_write_named_uint64(w, "requests",
					     rpoller->stat.requests);
		spdk_json_write_named_uint64(w, "request_latency",
					     rpoller->stat.request_latency);
		spdk_json_write_named_uint64(w, "pending_free_request",
					     rpoller->stat.pending_free_request);
		spdk_json_write_named_uint64(w, "pending_rdma_read",
					     rpoller->stat.pending_rdma_read);
		spdk_json_write_named_uint64(w, "pending_rdma_write",
					     rpoller->stat.pending_rdma_write);
		spdk_json_write_named_uint64(w, "pending_rdma_send",
					     rpoller->stat.pending_rdma_send);
		spdk_json_write_named_uint64(w, "total_send_wrs",
					     rpoller->stat.qp_stats.send.num_submitted_wrs);
		spdk_json_write_named_uint64(w, "send_doorbell_updates",
					     rpoller->stat.qp_stats.send.doorbell_updates);
		spdk_json_write_named_uint64(w, "total_recv_wrs",
					     rpoller->stat.qp_stats.recv.num_submitted_wrs);
		spdk_json_write_named_uint64(w, "recv_doorbell_updates",
					     rpoller->stat.qp_stats.recv.doorbell_updates);
		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
}

/*
 * [한국어]
 * spdk_nvmf_transport_rdma - ★ NVMe-oF RDMA transport vtable ★.
 *
 * lib/nvmf/transport.c 의 SPDK_NVMF_TRANSPORT_REGISTER 가 본 구조체를 등록 → 상위 NVMe-oF
 * 레이어가 type="RDMA" 로 함수 디스패치. TCP/FC/VFIOUSER 도 같은 패턴 (각자 자기 ops 등록).
 *
 * 그룹별 콜백:
 *  - **identification**: name="RDMA", type=SPDK_NVME_TRANSPORT_RDMA (enum).
 *  - **transport lifecycle**: opts_init (기본값 채움), create (transport 객체 생성),
 *    dump_opts (RPC dump), destroy.
 *  - **listener lifecycle**: listen (bind+listen), stop_listen (port 회수),
 *    cdata_init (ctrlr_data 의 RDMA-specific 필드 — msdbd, ioccsz),
 *    listener_discover (discovery target 의 entry 빌드).
 *  - **poll_group lifecycle**: create/destroy + add/remove qpair + poll.
 *    get_optimal_poll_group: 새 qpair 의 NUMA-aware poll group 선택.
 *  - **request lifecycle**: req_free (state COMPLETED 도달 시), req_complete (bdev 완료 콜백).
 *  - **qpair operations**: qpair_fini (close — to_close 마킹), 3종 trid getter (peer/local/listen),
 *    qpair_abort_request (NVMe Abort 명령 처리).
 *  - **stats**: poll_group_dump_stat (RPC dump 시 JSON 으로 통계 출력).
 *
 * 호출 흐름 예시 (host 의 read I/O):
 *   1. host RDMA SEND capsule → poll_group_poll → RECV CQE → request_process(NEW)
 *   2. request_parse_sgl → get_buffers → fill_iovs → state READY_TO_EXECUTE
 *   3. spdk_nvmf_request_exec → bdev I/O → bdev 콜백 → req_complete
 *   4. request_process(EXECUTED) → RDMA WRITE 발행 → request_process(READY_TO_COMPLETE) → SEND
 *   5. SEND CQE → request_process(COMPLETED) → req_free
 */
const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma = {
	.name = "RDMA",
	.type = SPDK_NVME_TRANSPORT_RDMA,
	.opts_init = nvmf_rdma_opts_init,                /* [한국어] transport opts 기본값 (max_queue_depth 등) */
	.create = nvmf_rdma_create,                       /* [한국어] transport 객체 생성 + event_channel + accept_poller */
	.dump_opts = nvmf_rdma_dump_opts,                 /* [한국어] RPC nvmf_get_transports 응답에 RDMA opts JSON */
	.destroy = nvmf_rdma_destroy,                     /* [한국어] transport 해제 — ports/devices/poll_groups 일괄 정리 */

	.listen = nvmf_rdma_listen,                       /* [한국어] subsystem listen — rdma_bind_addr + rdma_listen */
	.stop_listen = nvmf_rdma_stop_listen,             /* [한국어] listen port 회수 */
	.cdata_init = nvmf_rdma_cdata_init,               /* [한국어] Identify Controller 응답의 nvmf_specific.msdbd 설정 */

	.listener_discover = nvmf_rdma_discover,          /* [한국어] discovery target — log page entry 빌드 */

	.poll_group_create = nvmf_rdma_poll_group_create, /* [한국어] reactor 별 poll_group + per-device poller 생성 */
	.get_optimal_poll_group = nvmf_rdma_get_optimal_poll_group, /* [한국어] NUMA-aware QP → poll_group 라우팅 */
	.poll_group_destroy = nvmf_rdma_poll_group_destroy,
	.poll_group_add = nvmf_rdma_poll_group_add,       /* [한국어] qpair 를 poll_group 에 추가 + qpair_initialize 트리거 */
	.poll_group_remove = nvmf_rdma_poll_group_remove,
	.poll_group_poll = nvmf_rdma_poll_group_poll,     /* [한국어] ★ 매 reactor iteration hot path */

	.req_free = nvmf_rdma_request_free,               /* [한국어] state COMPLETED 도달 시 풀에 반환 */
	.req_complete = nvmf_rdma_request_complete,       /* [한국어] bdev I/O 완료 콜백 — EXECUTED 로 전이 */

	.qpair_fini = nvmf_rdma_close_qpair,              /* [한국어] graceful shutdown — to_close 마킹 */
	.qpair_get_peer_trid = nvmf_rdma_qpair_get_peer_trid,     /* [한국어] host 측 trid (RPC reporting) */
	.qpair_get_local_trid = nvmf_rdma_qpair_get_local_trid,   /* [한국어] target 측 trid */
	.qpair_get_listen_trid = nvmf_rdma_qpair_get_listen_trid, /* [한국어] listen port 의 trid */
	.qpair_abort_request = nvmf_rdma_qpair_abort_request,     /* [한국어] NVMe Abort admin 명령 처리 */

	.poll_group_dump_stat = nvmf_rdma_poll_group_dump_stat,   /* [한국어] RPC stats JSON 출력 */
};

/* [한국어] ELF .init_array constructor — main() 진입 전 transport 리스트에 등록.
 * 이후 외부 RPC (nvmf_create_transport 등) 가 type="RDMA" 로 본 vtable 조회 가능. */
SPDK_NVMF_TRANSPORT_REGISTER(rdma, &spdk_nvmf_transport_rdma);
/* [한국어] SPDK 로그 컴포넌트 등록 — SPDK_DEBUGLOG(rdma, ...) 가 "rdma" 컴포넌트로 분류. */
SPDK_LOG_REGISTER_COMPONENT(rdma)
