/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (c) 2018-2019 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * [한국어 설명] NVMe-oF Fibre Channel 트랜스포트 헤더 (nvmf_fc.h)
 *
 * === 파일의 역할 ===
 * NVMe-over-Fabrics target의 Fibre Channel(FC) 트랜스포트 구현(fc.c, fc_ls.c)이 사용하는
 * 모든 자료구조, enum, 콜백 시그니처를 정의한다. RDMA/TCP와 달리 FC는 SAN 환경에서 동작하며
 * 고유한 개념(N_Port/Remote Port, Exchange, ABTS, LS, FCP_RSP/ERSP)이 많아 별도의
 * 모델링이 필요하다. SPDK는 FC HW를 추상화한 LLD(Low-Level Driver - 보통 Broadcom Emulex)와
 * 인터페이스하며, 본 헤더는 그 인터페이스의 SPDK 측 데이터 모델이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe-oF FC 흐름 (Initiator → Target):
 *   FC HBA(LLD) -> SPDK_FC_HW_PORT_INIT 이벤트 -> nvmf_fc_main_enqueue_event
 *     -> spdk_nvmf_fc_port + io_queues(spdk_nvmf_fc_hwqp[]) 생성
 *     -> hwqp을 fc_poll_group에 분배 (assign_queue_to_main_thread)
 *     -> reactor가 hwqp을 폴링 -> nvmf_fc_hwqp_process_frame
 *     -> Frame이 LS면 nvmf_fc_handle_ls_rqst (fc_ls.c)로 라우팅,
 *        FCP면 spdk_nvmf_fc_request 객체로 변환되어 ctrlr.c 처리
 * 호출 체인 관점:
 *   상위: NVMe-oF core (ctrlr.c, subsystem.c) - FC를 일반 트랜스포트로 사용
 *   본 헤더: FC 전용 데이터 모델
 *   하위: lib/nvmf/fc.c (코어 FC 트랜스포트), fc_ls.c (LS 명령 처리), LLD 드라이버 (외부)
 * 실행 컨텍스트:
 *   - main thread: FC 이벤트(SPDK_FC_HW_PORT_INIT 등) 처리
 *   - hwqp의 PG thread: hwqp 폴링/IO 처리 (poller API 콜백 방식으로 cross-thread 호출)
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/nvmf.h, spdk/nvmf_fc_spec.h: NVMe-oF FC 스펙(NVMe TP4045) 정의
 *   - spdk/nvme_spec.h: NVMe 명령/응답 layout
 *   - rte_hash: DPDK lockless hash - hwqp의 conn_id/rport hash 인덱스
 *   - nvmf_internal.h: spdk_nvmf_qpair/request/subsystem 등 공통 모델
 * 데이터 흐름:
 *   FC frame → hwqp RQ 디스크립터 → process_frame → fc_request 객체 → bdev → ERSP/FCP_RSP
 *   LS frame → ls_rqst → handle_ls_rqst → assoc/conn 생성/삭제
 * 공유 자료구조:
 *   - spdk_nvmf_fc_port: HW port 1개당 1개 (HBA의 물리 포트)
 *   - spdk_nvmf_fc_nport: 포트 안의 N_Port (vNIC/NPIV)
 *   - spdk_nvmf_fc_association: NVMe-oF 1:1 association (호스트-tgtport 페어)
 *   - spdk_nvmf_fc_conn: association 안의 NVMe queue (admin or IO)
 *   - spdk_nvmf_fc_hwqp: HW queue (RQ/WQ 단위) - reactor가 폴링하는 단위
 *   - spdk_nvmf_fc_xchg: Exchange (NVMe IO 1개당 1개의 FC exchange)
 *
 * === 주요 함수/구조체 요약 ===
 * 구조체:
 *   - spdk_nvmf_fc_port: HW port (HBA 물리 포트)
 *   - spdk_nvmf_fc_nport: N_Port (logical port - WWN 1개)
 *   - spdk_nvmf_fc_remote_port_info: Remote port (initiator)
 *   - spdk_nvmf_fc_association: NVMe-oF 호스트-타깃 association (conn 묶음)
 *   - spdk_nvmf_fc_conn: NVMe queue (qpair 임베드 - admin 또는 IO)
 *   - spdk_nvmf_fc_hwqp: HW queue (reactor poll 단위)
 *   - spdk_nvmf_fc_request: FC IO 요청 (req 임베드)
 *   - spdk_nvmf_fc_xchg: FC Exchange (xchg_id 식별)
 *   - spdk_nvmf_fc_ls_rqst: NVMe-oF LS 명령 요청
 *   - spdk_nvmf_fc_buffer_desc: DMA 버퍼 디스크립터
 *   - spdk_nvmf_fc_abts_ctx: ABTS(Abort Sequence) 처리 컨텍스트
 *   - spdk_nvmf_fc_errors: 카운터 모음
 *   - 다양한 poller_api_*_args / 이벤트 args
 * 함수:
 *   - nvmf_fc_main_enqueue_event: LLD가 SPDK 메인에 이벤트 큐잉
 *   - nvmf_fc_handle_ls_rqst: LS frame 처리 (Connect/Disconnect 등)
 *   - nvmf_fc_hwqp_process_frame: 일반 FCP frame 처리 (LLD가 호출)
 *   - nvmf_fc_request_*: 요청 상태/abort/완료 처리
 *   - nvmf_fc_poller_api_func: 임의 hwqp에 cross-thread API 호출
 *   - nvmf_fc_get_fc_req / nvmf_fc_get_conn: container_of 인라인 변환
 */

#ifndef __NVMF_FC_H__                               /* [한국어] 헤더 가드 시작 */
#define __NVMF_FC_H__                               /* [한국어] 가드 매크로 정의 */

#include "spdk/nvme.h"                              /* [한국어] NVMe 공개 타입 (spdk_nvme_transport_id 등) - FC trid 채우기용 */
#include "spdk/nvmf.h"                              /* [한국어] NVMe-oF 공개 객체 (qpair/request/transport) */
#include "spdk/assert.h"                            /* [한국어] SPDK_STATIC_ASSERT - 컴파일 타임 검증 (LS 버퍼 크기 등) */
#include "spdk/nvme_spec.h"                         /* [한국어] NVMe Spec 명령/응답 layout */
#include "spdk/nvmf_fc_spec.h"                      /* [한국어] NVMe-oF FC 스펙 (TP4045) - LS 명령, ERSP IU, frame_hdr 등 */
#include "spdk/thread.h"                            /* [한국어] spdk_thread/poller - hwqp PG thread 모델 */
#include "nvmf_internal.h"                          /* [한국어] 공통 NVMe-oF target 데이터 모델 - subsystem/ctrlr/qpair */
#include <rte_hash.h>                               /* [한국어] DPDK lockless hash 라이브러리 - hwqp의 conn_id/rport 인덱스 */

#define SPDK_NVMF_FC_TR_ADDR_LEN 64                 /* [한국어] FC traddr 문자열 최대 길이 ("nn-0xWWN:pn-0xWWN" 형식 충분) */
#define NVMF_FC_INVALID_CONN_ID UINT64_MAX          /* [한국어] 유효하지 않은 connection id 표시 - rte_hash lookup miss 등에서 사용 */

#define SPDK_FC_HW_DUMP_REASON_STR_MAX_SIZE 256     /* [한국어] HW port reset 시 dump 사유 문자열 최대 길이 */
#define SPDK_MAX_NUM_OF_FC_PORTS 32                 /* [한국어] target 1개가 관리 가능한 FC HW port 최대 수 - HBA 슬롯 한계 고려 */
#define SPDK_NVMF_PORT_ID_MAX_LEN 32                /* [한국어] N_Port의 사용자 가시 port_id 문자열 최대 길이 */

/*
 * FC HWQP pointer
 *
 * [한국어] LLD(저수준 FC 드라이버 - 보통 Broadcom Emulex/QLogic)가 관리하는 HW queue 핸들의 불투명 포인터.
 * SPDK 측에서는 LLD에 그대로 전달만 하고 내부 구조에 접근하지 않음 - LLD-별 다양한 구현 추상화.
 */
typedef void *spdk_nvmf_fc_lld_hwqp_t;

/*
 * FC LLD port.
 *
 * [한국어] LLD가 관리하는 HW port 핸들의 불투명 포인터.
 * SPDK는 LLD를 통해 port 동작(online/offline/reset)만 요청하고 내부에는 접근하지 않음.
 */
typedef void *spdk_nvmf_fc_lld_fc_port_t;

/*
 * FC HW port states.
 *
 * [한국어] HW port의 운영 상태 - LLD가 변경 시 SPDK_FC_HW_PORT_ONLINE/OFFLINE 이벤트로 전달.
 */
enum spdk_fc_port_state {
	SPDK_FC_PORT_OFFLINE = 0,
	/* [한국어] port가 offline - link down, 명시적 disable, reset 진행 등.
	 * 설정자: HW reset/offline 이벤트 처리.
	 * 읽는 자: nvmf_fc_port_is_offline, IO 처리 가드. */

	SPDK_FC_PORT_ONLINE = 1,
	/* [한국어] port가 정상 운영 - 새 association/IO 수락 가능.
	 * 설정자: HW online 이벤트.
	 * 읽는 자: nvmf_fc_port_is_online. */

	SPDK_FC_PORT_QUIESCED = 2,
	/* [한국어] port quiesced - 새 IO 거부, 진행 중 IO만 완료 (graceful drain).
	 * 설정자: graceful shutdown 시작 시.
	 * 읽는 자: nvmf_fc_is_port_dead 등 - dead port 취급. */
};

/*
 * [한국어] HW queue pair (RQ+WQ)의 운영 상태.
 */
enum spdk_fc_hwqp_state {
	SPDK_FC_HWQP_OFFLINE = 0,
	/* [한국어] hwqp 미활성 - poller가 폴링하지 않음. */

	SPDK_FC_HWQP_ONLINE = 1,
	/* [한국어] hwqp 활성 - poller가 매 주기 frame 폴링. */
};

/*
 * NVMF FC Object state
 * Add all the generic states of the object here.
 * Specific object states can be added separately
 *
 * [한국어] FC 객체(nport/conn/assoc)의 일반 상태 - 생성/삭제/부분 진행 추적.
 */
enum spdk_nvmf_fc_object_state {
	SPDK_NVMF_FC_OBJECT_CREATED = 0,
	/* [한국어] 정상 생성 완료 상태.
	 * 설정자: 객체 create 완료 시.
	 * 읽는 자: 사용 가능 여부 검증. */

	SPDK_NVMF_FC_OBJECT_TO_BE_DELETED = 1,
	/* [한국어] 삭제 진행 중 - 새 사용 거부.
	 * 설정자: delete 명령 진입 시.
	 * 읽는 자: 신규 op 거부 가드. */

	SPDK_NVMF_FC_OBJECT_ZOMBIE = 2,      /* Partial Create or Delete  */
	/* [한국어] 생성/삭제 중 에러로 정리 실패한 경우 - leakage 회피용 좀비 표시.
	 * 설정자: cleanup 실패 시.
	 * 읽는 자: 통계, 디버그 dump. */
};

/*
 * FC request state
 *
 * [한국어] FC IO 요청의 16단계 상태 머신.
 * Read: INIT→READ_BDEV→READ_XFER(송신)→READ_RSP→SUCCESS
 * Write: INIT→WRITE_BUFFS→WRITE_XFER(수신)→WRITE_BDEV→WRITE_RSP→SUCCESS
 * 명령에 따라 분기되며, abort/error 분기도 별도 상태로 추적.
 */
enum spdk_nvmf_fc_request_state {
	SPDK_NVMF_FC_REQ_INIT = 0,
	/* [한국어] 요청 초기화 직후 - 명령 디스패치 전. */

	SPDK_NVMF_FC_REQ_READ_BDEV,
	/* [한국어] Read 명령 - bdev에서 데이터 읽는 중. */

	SPDK_NVMF_FC_REQ_READ_XFER,
	/* [한국어] Read 명령 - 데이터를 호스트로 전송 중 (FC SEQ_INIT). */

	SPDK_NVMF_FC_REQ_READ_RSP,
	/* [한국어] Read 명령 - FCP_RSP/ERSP 응답 송신 중. */

	SPDK_NVMF_FC_REQ_WRITE_BUFFS,
	/* [한국어] Write 명령 - 수신 버퍼 할당 중. */

	SPDK_NVMF_FC_REQ_WRITE_XFER,
	/* [한국어] Write 명령 - 호스트에서 데이터 수신 중. */

	SPDK_NVMF_FC_REQ_WRITE_BDEV,
	/* [한국어] Write 명령 - bdev에 데이터 쓰기 중. */

	SPDK_NVMF_FC_REQ_WRITE_RSP,
	/* [한국어] Write 명령 - FCP_RSP 응답 송신 중. */

	SPDK_NVMF_FC_REQ_NONE_BDEV,
	/* [한국어] 데이터 없는 명령(Flush 등) - bdev 처리 중. */

	SPDK_NVMF_FC_REQ_NONE_RSP,
	/* [한국어] 데이터 없는 명령 - 응답 송신 중. */

	SPDK_NVMF_FC_REQ_SUCCESS,
	/* [한국어] 명령 정상 완료. */

	SPDK_NVMF_FC_REQ_FAILED,
	/* [한국어] 명령 실패 (NVMe SC 응답으로 처리). */

	SPDK_NVMF_FC_REQ_ABORTED,
	/* [한국어] 호스트 ABTS 또는 내부 abort로 종료. */

	SPDK_NVMF_FC_REQ_BDEV_ABORTED,
	/* [한국어] bdev 레벨에서 abort 처리 (bdev abort callback). */

	SPDK_NVMF_FC_REQ_PENDING,
	/* [한국어] hwqp 자원 부족 등으로 대기 큐(in_use_reqs/pending)에 보류. */

	SPDK_NVMF_FC_REQ_FUSED_WAITING,
	/* [한국어] Fused 명령(Compare+Write) - 첫 번째가 와서 두 번째 대기 중. */

	SPDK_NVMF_FC_REQ_MAX_STATE,
	/* [한국어] enum count sentinel - 상태 수 = 16. 디버그 문자열 배열 크기 등에 사용. */
};

/*
 * Generic DMA buffer descriptor
 *
 * [한국어] DMA 가능 버퍼 디스크립터 - LLD가 hwqp RQ/WQ에 등록하는 frame 버퍼 표현.
 */
struct spdk_nvmf_fc_buffer_desc {
	void *virt;
	/* [한국어] 가상 주소 - SPDK가 데이터 접근 시 사용.
	 * 설정자: LLD가 hwqp init 시 할당.
	 * 읽는 자: SPDK가 frame 디코드 시.
	 * 동기화: hwqp PG thread 단독. */

	uint64_t phys;
	/* [한국어] 물리(IOVA) 주소 - LLD가 HW에 DMA 디스크립터 작성 시.
	 * 값 범위: hugepage 기반 핀된 주소. */

	size_t len;
	/* [한국어] 버퍼 크기 (바이트). */

	/* Internal */
	uint32_t buf_index;
	/* [한국어] LLD-내부 버퍼 인덱스 (RQ 슬롯 번호 등) - LLD가 할당, SPDK는 그대로 반환. */
};

/*
 * ABTS handling context
 *
 * [한국어] ABTS(Abort Sequence) 처리 컨텍스트.
 * 호스트가 진행 중인 FC sequence를 abort하기 위해 ABTS frame 송신 → SPDK target은 모든 hwqp을
 * 동기화한 후 정리하고 응답. 이 객체는 그 진행 상태를 추적.
 */
struct spdk_nvmf_fc_abts_ctx {
	bool handled;
	/* [한국어] ABTS가 적어도 한 hwqp에서 매칭되어 처리되었는지.
	 * 설정자: hwqp이 oxid 매칭 발견 시 true.
	 * 읽는 자: 모든 hwqp 응답 후 최종 처리. */

	uint16_t hwqps_responded;
	/* [한국어] 지금까지 ABTS 응답 완료한 hwqp 개수.
	 * 설정자: 각 hwqp이 처리 완료 후 ++.
	 * 읽는 자: num_hwqps와 비교해 모든 hwqp 처리 완료 검사. */

	uint16_t rpi;
	/* [한국어] Remote Port Index - LLD가 부여한 원격 호스트 포트 식별자. */

	uint16_t oxid;
	/* [한국어] Originator Exchange ID - 호스트가 시작한 exchange ID. */

	uint16_t rxid;
	/* [한국어] Responder Exchange ID - 응답자(타깃)의 exchange ID. */

	struct spdk_nvmf_fc_nport *nport;
	/* [한국어] ABTS 대상 N_Port 포인터 (back-pointer). */

	uint16_t nport_hdl;
	/* [한국어] N_Port 핸들 (LLD ID). */

	uint8_t port_hdl;
	/* [한국어] HW port 핸들. */

	void *abts_poller_args;
	/* [한국어] hwqp별 ABTS poller에 전달되는 args (poller_api 내부용). */

	void *sync_poller_args;
	/* [한국어] queue sync poller args - ABTS 처리 전 hwqp drain용. */

	int num_hwqps;
	/* [한국어] ABTS 검색 대상 hwqp 총 개수 (port의 모든 io queue).
	 * 설정자: ABTS 진입 시 fc_port->num_io_queues 복사.
	 * 읽는 자: hwqps_responded와 비교. */

	bool queue_synced;
	/* [한국어] queue sync(drain) 단계 완료 여부. */

	uint64_t u_id;
	/* [한국어] queue sync용 unique tag - 다중 ABTS 동시 진행 시 식별.
	 * 설정자: ABTS 진입 시 단조 증가.
	 * 읽는 자: queue sync_done 콜백 매칭. */

	struct spdk_nvmf_fc_hwqp *ls_hwqp;
	/* [한국어] LS hwqp - ABTS 응답이 LS 큐로 송신될 때 사용. */

	uint16_t fcp_rq_id;
	/* [한국어] FCP RQ ID - LLD에 응답 송신 시 어떤 RQ 컨텍스트인지. */
};

/*
 * NVME FC transport errors
 *
 * [한국어] hwqp별 에러 카운터 모음 - 디버그/통계용.
 * 모든 필드는 hwqp PG thread에서만 ++되어 atomic 불필요.
 * 각 카운터의 의미는 LLD/HW 에러 종류별로 분리되어 trouble shoot에 사용된다.
 */
struct spdk_nvmf_fc_errors {
	uint32_t no_xchg;
	/* [한국어] Exchange 자원 고갈로 IO 거부한 횟수. */

	uint32_t nport_invalid;
	/* [한국어] frame의 D_ID가 등록된 nport와 매칭 안 됨. */

	uint32_t unknown_frame;
	/* [한국어] 해독 불가능한 frame 도착. */

	uint32_t wqe_cmplt_err;
	/* [한국어] WQE(Work Queue Entry) 완료 시 에러 status. */

	uint32_t wqe_write_err;
	/* [한국어] WQE 작성(post) 실패. */

	uint32_t rq_status_err;
	/* [한국어] RQ(Receive Queue) 완료 status 에러. */

	uint32_t rq_buf_len_err;
	/* [한국어] RQ 버퍼 길이가 frame 길이와 불일치. */

	uint32_t rq_id_err;
	/* [한국어] RQ ID 검증 실패. */

	uint32_t rq_index_err;
	/* [한국어] RQ 버퍼 인덱스 범위 밖. */

	uint32_t invalid_cq_type;
	/* [한국어] CQ(Completion Queue) 타입이 예상 외. */

	uint32_t invalid_cq_id;
	/* [한국어] CQ ID 검증 실패. */

	uint32_t fc_req_buf_err;
	/* [한국어] fc_request 버퍼 처리 에러. */

	uint32_t buf_alloc_err;
	/* [한국어] 버퍼 할당(mempool 고갈) 실패. */

	uint32_t unexpected_err;
	/* [한국어] 분류되지 않은 예상 외 에러. */

	uint32_t nvme_cmd_iu_err;
	/* [한국어] NVMe Command IU(Information Unit) 파싱 에러. */

	uint32_t nvme_cmd_xfer_err;
	/* [한국어] NVMe 데이터 전송 시 에러. */

	uint32_t queue_entry_invalid;
	/* [한국어] queue entry 데이터 무효. */

	uint32_t invalid_conn_err;
	/* [한국어] frame의 conn_id가 등록된 connection과 매칭 안 됨. */

	uint32_t fcp_rsp_failure;
	/* [한국어] FCP_RSP 송신 실패. */

	uint32_t write_failed;
	/* [한국어] Write 명령 처리 실패. */

	uint32_t read_failed;
	/* [한국어] Read 명령 처리 실패. */

	uint32_t rport_invalid;
	/* [한국어] frame의 S_ID가 등록된 remote port와 매칭 안 됨. */

	uint32_t num_aborted;
	/* [한국어] abort 처리한 IO 누적 수. */

	uint32_t num_abts_sent;
	/* [한국어] 송신한 ABTS frame 수 (target-initiated abort). */
};

/*
 *  Send Single Request/Response Sequence.
 *
 * [한국어] 단일 LS 요청/응답 페어를 위한 버퍼 컨테이너.
 * Disconnect LS를 initiator에 송신할 때 사용 - 명시적 disconnect 기능.
 */
struct spdk_nvmf_fc_srsr_bufs {
	void *rqst;
	/* [한국어] LS 요청 버퍼 가상 주소 (target → initiator 송신 페이로드).
	 * 설정자: snd_disconn_bufs alloc 시.
	 * 읽는 자: LLD send 호출. */

	size_t rqst_len;
	/* [한국어] 요청 버퍼 길이 (FCNVME_MAX_LS_REQ_SIZE 이내). */

	void *rsp;
	/* [한국어] 응답 수신 버퍼 (initiator의 응답 LS 저장). */

	size_t rsp_len;
	/* [한국어] 응답 버퍼 길이. */

	uint16_t rpi;
	/* [한국어] Remote Port Index - 어디로 송신할지 LLD에 지시. */
};

/*
 * [한국어] qpair 비동기 제거 컨텍스트.
 * fc_conn 종료 시 spdk_thread_send_msg로 다른 thread에 전달되는 데이터.
 */
struct spdk_nvmf_fc_qpair_remove_ctx {
	struct spdk_nvmf_qpair *qpair;
	/* [한국어] 제거할 qpair (fc_conn에 임베드된 본체). */

	spdk_nvmf_transport_qpair_fini_cb cb_fn;
	/* [한국어] 제거 완료 콜백 (트랜스포트 ops 시그니처). */

	void *cb_ctx;
	/* [한국어] 콜백 인자. */

	struct spdk_thread *qpair_thread;
	/* [한국어] qpair가 바인딩된 thread - 콜백을 호출할 thread.
	 * 설정자: 진입 시 spdk_get_thread().
	 * 읽는 자: cb_fn 호출 시 send_msg 대상. */
};

/*
 * Struct representing a nport
 *
 * [한국어] N_Port - HW port 안의 논리 포트 (NPIV/vNIC).
 * 한 HW port가 여러 N_Port를 가질 수 있고, 각 N_Port는 별도 WWN/D_ID를 가진다.
 * NVMe-oF target은 N_Port 단위로 nodename/portname을 광고한다.
 */
struct spdk_nvmf_fc_nport {

	uint16_t nport_hdl;
	/* [한국어] N_Port 핸들 (HW port 내부에서 unique).
	 * 설정자: NPORT_CREATE 이벤트 처리.
	 * 읽는 자: nport lookup (nport_find). */

	uint8_t port_hdl;
	/* [한국어] 소속 HW port 핸들 (back-pointer 보조). */

	uint32_t d_id;
	/* [한국어] FC 24-bit Destination ID (nport 자체의 FC address).
	 * 설정자: NPORT_CREATE 이벤트.
	 * 읽는 자: 들어온 frame의 D_ID 매칭. */

	enum spdk_nvmf_fc_object_state nport_state;
	/* [한국어] N_Port 상태 (CREATED/TO_BE_DELETED/ZOMBIE). */

	struct spdk_nvmf_fc_wwn fc_nodename;
	/* [한국어] FC node WWN (HBA/노드 식별).
	 * 설정자: NPORT_CREATE.
	 * 읽는 자: trid traddr 빌드 (nn-0x...). */

	struct spdk_nvmf_fc_wwn fc_portname;
	/* [한국어] FC port WWN (포트 식별).
	 * 설정자: NPORT_CREATE.
	 * 읽는 자: trid traddr 빌드 (pn-0x...). */

	/* list of remote ports (i.e. initiators) connected to nport */
	TAILQ_HEAD(, spdk_nvmf_fc_remote_port_info) rem_port_list;
	/* [한국어] 이 nport에 PRLI한 initiator(원격 포트) 리스트.
	 * 설정자: IT_ADD 이벤트.
	 * 읽는 자: rport lookup. */

	uint32_t rport_count;
	/* [한국어] 등록된 원격 포트 수 (rem_port_list 길이 캐시). */

	void *vendor_data;	/* available for vendor use */
	/* [한국어] LLD-vendor가 자유롭게 사용 가능한 컨텍스트 슬롯.
	 * SPDK는 read/write하지 않음 - LLD 전용. */

	/* list of associations to nport */
	TAILQ_HEAD(, spdk_nvmf_fc_association) fc_associations;
	/* [한국어] 이 nport에 만들어진 NVMe-oF association 리스트.
	 * 설정자: assoc 생성/제거 시.
	 * 읽는 자: cleanup 순회, dump. */

	uint32_t assoc_count;
	/* [한국어] association 개수 캐시. */

	struct spdk_nvmf_fc_port *fc_port;
	/* [한국어] 소속 HW port back-pointer. */

	TAILQ_ENTRY(spdk_nvmf_fc_nport) link; /* list of nports on a hw port. */
	/* [한국어] fc_port->nport_list 노드. */
};

typedef void (*spdk_nvmf_fc_caller_cb)(void *hwqp, int32_t status, void *args);
/* [한국어] FC 일반 caller 콜백 타입 - request abort, queue sync 등 다양한 비동기 작업의 완료 통지.
 * @hwqp: 작업이 처리된 hwqp.
 * @status: 0=성공, 음수=errno.
 * @args: 호출자 정의 컨텍스트. */

/*
 * NVMF FC Connection
 *
 * [한국어] FC 위의 NVMe-oF connection - admin 또는 IO queue 1개를 표현.
 * 첫 멤버가 spdk_nvmf_qpair이므로 container_of 패턴으로 qpair ↔ fc_conn 변환 가능.
 * association에 묶이며, 각 conn은 정확히 1개의 hwqp에 매핑됨.
 */
struct spdk_nvmf_fc_conn {
	struct spdk_nvmf_qpair qpair;
	/* [한국어] 임베드된 공통 qpair 본체. 첫 멤버 = nvmf_fc_get_conn 캐스팅 안전성 보장.
	 * 설정자: fc_conn 생성 시 init.
	 * 읽는 자: 모든 NVMe-oF core 코드.
	 * 동기화: hwqp PG thread 단일 사용. */

	struct spdk_nvme_transport_id trid;
	/* [한국어] 이 conn의 트랜스포트 식별자 (FC traddr/trsvcid).
	 * 설정자: nvmf_fc_create_trid 사용해 채움.
	 * 읽는 자: 트랜스포트 매핑. */

	uint32_t s_id;
	/* [한국어] Source ID - initiator(호스트)의 FC address.
	 * 설정자: Connect LS 처리 시.
	 * 읽는 자: outgoing frame의 D_ID 설정. */

	uint32_t d_id;
	/* [한국어] Destination ID - tgtport의 FC address.
	 * 설정자: Connect LS 처리.
	 * 읽는 자: outgoing frame의 S_ID 설정. */

	uint64_t conn_id;
	/* [한국어] FC NVMe connection ID - LLD가 부여, frame과 conn 매핑에 사용.
	 * 설정자: Connect LS 처리.
	 * 읽는 자: hwqp의 connection_list_hash로 frame → conn lookup.
	 * 값 범위: NVMF_FC_INVALID_CONN_ID(=UINT64_MAX) 외. */

	struct spdk_nvmf_fc_hwqp *hwqp;
	/* [한국어] 이 conn을 처리하는 hwqp - 모든 IO는 이 hwqp을 통해.
	 * 설정자: ADD_CONNECTION poller API.
	 * 읽는 자: 모든 IO 송수신. */

	uint16_t esrp_ratio;
	/* [한국어] ERSP(Extended Response) 송신 비율 - rsp_count가 이 값에 도달하면 ERSP 송신.
	 * NVMe-oF FC 스펙 - 일반 FCP_RSP 대신 추가 정보 담는 ERSP IU. */

	uint16_t rsp_count;
	/* [한국어] 마지막 ERSP 이후 송신한 응답 수 카운터 - esrp_ratio와 비교. */

	uint32_t rsn;
	/* [한국어] Response Sequence Number - 단조 증가, ERSP에 포함.
	 * 설정자: 응답 송신 시 ++. */

	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t max_queue_depth;
	/* [한국어] queue 깊이 (NVMe SQ size).
	 * 설정자: Connect LS의 sqsize.
	 * 읽는 자: 자원 할당, IO 한계 검사. */

	uint16_t max_rw_depth;
	/* [한국어] read/write 동시 진행 한계. */

	/* The current number of I/O outstanding on this connection. This number
	 * includes all I/O from the time the capsule is first received until it is
	 * completed.
	 */
	uint16_t cur_queue_depth;
	/* [한국어] 현재 outstanding IO 수 (캡슐 수신부터 완료까지).
	 * 설정자: 명령 시작 시 ++, 완료 시 --.
	 * 읽는 자: 백프레셔 검사, dump. */

	/* number of read/write requests that are outstanding */
	uint16_t cur_fc_rw_depth;
	/* [한국어] 현재 outstanding read/write 수 (max_rw_depth 한계). */

	TAILQ_HEAD(, spdk_nvmf_fc_request) fused_waiting_queue;
	/* [한국어] Fused 명령(Compare+Write)에서 첫 명령 도착 후 두 번째 대기 큐. */

	struct spdk_nvmf_fc_association *fc_assoc;
	/* [한국어] 소속 association back-pointer. */

	uint16_t rpi;
	/* [한국어] Remote Port Index (LLD 자원 매핑). */

	/* for association's connection list */
	TAILQ_ENTRY(spdk_nvmf_fc_conn) assoc_link;
	/* [한국어] assoc->fc_conns 리스트의 노드. */

	/* for associations's available connection list */
	TAILQ_ENTRY(spdk_nvmf_fc_conn) assoc_avail_link;
	/* [한국어] assoc->avail_fc_conns(미사용 풀)의 노드. */

	/* for hwqp's rport connection list link  */
	TAILQ_ENTRY(spdk_nvmf_fc_conn) rport_link;
	/* [한국어] hwqp의 rport별 conn 리스트 노드. */

	/* Per connection fc_req pool */
	STAILQ_HEAD(, spdk_nvmf_fc_pooled_request) pool_queue;
	/* [한국어] 이 conn 전용 fc_request 풀의 free 리스트.
	 * 설정자: nvmf_fc_create_conn_reqpool 시 채움.
	 * 읽는 자: 새 IO 시작 시 alloc. */

	/* Memory for the fc_req pool objects */
	struct spdk_nvmf_fc_pooled_request *pool_memory;
	/* [한국어] 풀 객체들의 메모리 백킹 (한 번에 할당). */

	/* Pool size */
	uint32_t pool_size;
	/* [한국어] 풀 크기 (max_queue_depth + 여분). */

	/* Current free elem in pool */
	uint32_t pool_free_elems;
	/* [한국어] 풀에 남은 free 요청 수. */

	TAILQ_HEAD(, spdk_nvmf_fc_request) in_use_reqs;
	/* [한국어] 현재 처리 중인 fc_request 리스트 (디버그/abort용). */

	enum spdk_nvmf_fc_object_state conn_state;
	/* [한국어] conn 상태 (CREATED/TO_BE_DELETED/ZOMBIE). */

	/* New QP create context. */
	struct nvmf_fc_ls_op_ctx *create_opd;
	/* [한국어] Create 작업 진행 중 LS op 컨텍스트 (없으면 NULL). */

	/* Delete conn callback list */
	void *ls_del_op_ctx;
	/* [한국어] Delete 작업 콜백 컨텍스트 - LS Disconnect 응답 후 처리할 콜백 체인. */

	bool qpair_fini_done;
	/* [한국어] qpair fini 완료 플래그. */

	spdk_nvmf_fc_caller_cb qpair_fini_done_cb;
	/* [한국어] qpair fini 완료 콜백. */

	void *qpair_fini_done_cb_args;
	/* [한국어] qpair_fini_done_cb 인자. */
};

/*
 * Structure for maintaining the FC exchanges
 *
 * [한국어] FC Exchange - NVMe IO 1개 또는 LS 1개에 대응.
 * 호스트가 시작한 sequence를 응답(또는 ABTS)할 때 식별자로 사용.
 */
struct spdk_nvmf_fc_xchg {
	uint32_t xchg_id;   /* The actual xchg identifier */
	/* [한국어] 실제 exchange identifier (LLD가 할당, IO마다 unique).
	 * 설정자: LLD가 frame 도착 시 할당.
	 * 읽는 자: ABTS 매칭, 응답 송신 시 OX_ID/RX_ID 채움. */

	/* Internal */
	TAILQ_ENTRY(spdk_nvmf_fc_xchg) link;
	/* [한국어] (사용 예약) - 미사용/디버그 리스트 노드. */

	bool active;
	/* [한국어] true면 진행 중 - 자원 풀에서 alloc된 상태. */

	bool aborted;
	/* [한국어] ABTS로 abort 요청됨 표시. */

	bool send_abts; /* Valid if is_aborted is set. */
	/* [한국어] target이 abts를 송신해야 함 (abort 시 옵션). aborted=true일 때만 의미. */
};

/*
 *  FC poll group structure
 *
 * [한국어] FC 트랜스포트 별 poll group - 상위 nvmf poll group의 transport-specific 부분.
 * 첫 멤버가 spdk_nvmf_transport_poll_group이므로 trans group ↔ fc poll group 캐스팅 가능.
 * 한 PG에 여러 hwqp이 묶일 수 있다.
 */
struct spdk_nvmf_fc_poll_group {
	struct spdk_nvmf_transport_poll_group group;
	/* [한국어] 임베드된 공통 transport poll group 본체.
	 * 설정자: poll_group_create.
	 * 읽는 자: NVMe-oF core. */

	struct spdk_nvmf_tgt *nvmf_tgt;
	/* [한국어] 소속 target back-pointer. */

	uint32_t hwqp_count; /* number of hwqp's assigned to this pg */
	/* [한국어] 이 PG에 할당된 hwqp 수 (hwqp_list 길이 캐시). */

	TAILQ_HEAD(, spdk_nvmf_fc_hwqp) hwqp_list;
	/* [한국어] 이 PG에 묶인 hwqp 리스트 - poller가 모두 폴링.
	 * 설정자: poll_group_add_hwqp.
	 * 읽는 자: poll loop, dump. */

	TAILQ_ENTRY(spdk_nvmf_fc_poll_group) link;
	/* [한국어] 전역 fc poll group 리스트 노드. */
};

/*
 *  HWQP poller structure passed from main thread
 *
 * [한국어] HW Queue Pair - LLD의 RQ+WQ+CQ 세트를 SPDK가 폴링하는 단위.
 * 한 hwqp = 한 reactor thread가 폴링 - cross-thread 호출은 poller_api_*로 예약.
 * 모든 IO/LS/ABTS는 어떤 hwqp에 속한 frame으로 들어와 hwqp PG thread에서 처리.
 */
struct spdk_nvmf_fc_hwqp {
	enum spdk_fc_hwqp_state state;  /* queue state (for poller) */
	/* [한국어] hwqp 운영 상태 (ONLINE/OFFLINE).
	 * 설정자: HW port 상태 변경 + hwqp_set_online/offline.
	 * 읽는 자: poll 진입 가드. */

	bool is_ls_queue;
	/* [한국어] LS 전용 큐인지 - true면 LS 명령만, false면 FCP IO도. */

	uint32_t lcore_id;   /* core hwqp is running on (for tracing purposes only) */
	/* [한국어] hwqp이 묶인 lcore (DPDK 코어 번호) - tracing/debug 전용. */

	struct spdk_thread *thread;  /* thread hwqp is running on */
	/* [한국어] hwqp이 폴링되는 spdk_thread.
	 * 설정자: poll_group에 할당 시.
	 * 읽는 자: cross-thread 호출 시 send_msg 대상. */

	uint32_t hwqp_id;    /* A unique id (per physical port) for a hwqp */
	/* [한국어] HW port 안에서의 hwqp 고유 ID.
	 * 설정자: HW port init 시.
	 * 읽는 자: 디버그, LLD 매칭. */

	spdk_nvmf_fc_lld_hwqp_t queues;    /* vendor HW queue set */
	/* [한국어] LLD가 관리하는 실제 HW queue 핸들 (불투명).
	 * 설정자: HW port init 이벤트로 LLD가 전달.
	 * 읽는 자: LLD 모든 호출에 인자로 패스. */

	struct spdk_nvmf_fc_port *fc_port; /* HW port structure for these queues */
	/* [한국어] 소속 HW port back-pointer. */

	struct spdk_nvmf_fc_poll_group *fgroup;
	/* [한국어] 이 hwqp을 폴링하는 fc poll group. */

	/* qpair (fc_connection) list */
	uint32_t num_conns; /* number of connections to queue */
	/* [한국어] 이 hwqp에 매핑된 connection 수.
	 * 설정자: conn add/remove 시.
	 * 읽는 자: 통계, 분배 결정. */

	struct rte_hash *connection_list_hash;
	/* [한국어] conn_id → fc_conn 포인터 lockless hash (DPDK rte_hash).
	 * 설정자: conn add 시 insert.
	 * 읽는 자: frame 도착 시 conn_id로 빠른 lookup.
	 * 동기화: rte_hash는 reader-writer 동시성 보장 (reader 안전). */

	struct rte_hash *rport_list_hash;
	/* [한국어] rpi → rport_list (conn 리스트) lockless hash.
	 * 설정자: rport add 시.
	 * 읽는 자: ABTS, IT_DELETE 시 해당 rport의 모든 conn 순회. */

	TAILQ_HEAD(, spdk_nvmf_fc_request) in_use_reqs;
	/* [한국어] 이 hwqp에서 진행 중인 fc_request 리스트 - abort/dump 시 순회. */

	struct spdk_nvmf_fc_errors counters;
	/* [한국어] hwqp별 에러 통계. */

	/* Pending LS request waiting for FC resource */
	TAILQ_HEAD(, spdk_nvmf_fc_ls_rqst) ls_pending_queue;
	/* [한국어] 자원 부족으로 처리 대기 중인 LS 요청 큐.
	 * 설정자: 자원 부족 시 enqueue.
	 * 읽는 자: 자원 회수 후 process_pending_ls_rqsts. */

	/* Sync req list */
	TAILQ_HEAD(, spdk_nvmf_fc_poller_api_queue_sync_args) sync_cbs;
	/* [한국어] 진행 중 queue sync 요청 목록 (drain/ABTS 처리용). */

	TAILQ_ENTRY(spdk_nvmf_fc_hwqp) link;
	/* [한국어] poll group의 hwqp_list 노드. */

	void *context;			/* Vendor specific context data */
	/* [한국어] LLD 전용 컨텍스트. SPDK는 미접근. */
};

/*
 * FC HW port.
 *
 * [한국어] FC HW port (HBA의 물리 포트).
 * 한 port에 LS hwqp 1개 + IO hwqp 여러 개 + nport 여러 개를 보유.
 */
struct spdk_nvmf_fc_port {
	uint8_t port_hdl;
	/* [한국어] HW port 핸들 (전역 unique). */

	spdk_nvmf_fc_lld_fc_port_t lld_fc_port;
	/* [한국어] LLD-소유 HW port 핸들 (불투명). */

	enum spdk_fc_port_state hw_port_status;
	/* [한국어] port 상태 (OFFLINE/ONLINE/QUIESCED). */

	uint16_t fcp_rq_id;
	/* [한국어] FCP RQ ID base - SCSI/NVMe RQ 식별 시작. */

	struct spdk_nvmf_fc_hwqp ls_queue;
	/* [한국어] LS 전용 hwqp 1개 - Connect/Disconnect 등 LS frame만 처리. */

	uint32_t num_io_queues;
	/* [한국어] io_queues 배열 크기 (IO hwqp 개수). */

	struct spdk_nvmf_fc_hwqp *io_queues;
	/* [한국어] IO hwqp 배열 - FCP frame 처리.
	 * 설정자: HW_PORT_INIT 이벤트 처리.
	 * 읽는 자: ABTS 시 모든 io queue 순회 등. */

	/*
	 * List of nports on this HW port.
	 */
	TAILQ_HEAD(, spdk_nvmf_fc_nport)nport_list;
	/* [한국어] 이 port에 등록된 N_Port 리스트. */

	int	num_nports;
	/* [한국어] N_Port 수 캐시. */

	TAILQ_ENTRY(spdk_nvmf_fc_port) link;
	/* [한국어] 전역 fc_port_list 노드. */

	struct spdk_mempool *io_resource_pool; /* Pools to store bdev_io's for this port */
	/* [한국어] 이 port의 bdev_io 객체용 mempool (DPDK lockless pool).
	 * 설정자: port init 시 alloc.
	 * 읽는 자: IO 시작 시 spdk_mempool_get. */

	void *port_ctx;
	/* [한국어] LLD 전용 port 컨텍스트. */
};

/*
 * NVMF FC Request
 *
 * [한국어] FC IO 요청 본체 - 한 NVMe 명령(IO 또는 LS-after-Connect 등)에 1:1 대응.
 * 첫 멤버 = spdk_nvmf_request이므로 nvmf_fc_get_fc_req로 container_of 변환 가능.
 * conn->in_use_reqs / hwqp->in_use_reqs에 동시 매달림.
 */
struct spdk_nvmf_fc_request {
	struct spdk_nvmf_request req;
	/* [한국어] 임베드된 공통 nvmf_request 본체. 첫 멤버 = 캐스팅 안전.
	 * 설정자: fc_request alloc 시 init.
	 * 읽는 자: NVMe-oF core 모든 코드. */

	union nvmf_h2c_msg cmd;
	/* [한국어] H2C(Host-to-Controller) 명령 캡슐 사본 (host frame에서 복사).
	 * 설정자: hwqp_process_frame 시.
	 * 읽는 자: 명령 디스패치. */

	struct spdk_nvmf_fc_ersp_iu ersp;
	/* [한국어] ERSP(Extended Response) IU 버퍼 - 응답 송신 시 채움. */

	uint32_t poller_lcore; /* for tracing purposes only */
	/* [한국어] 처리 중인 lcore - tracing 전용. */

	struct spdk_thread *poller_thread;
	/* [한국어] 처리 중인 spdk_thread - 콜백 스케줄링에 사용. */

	struct spdk_nvmf_fc_xchg *xchg;
	/* [한국어] 이 요청에 할당된 FC Exchange.
	 * 설정자: 요청 시작 시 alloc.
	 * 읽는 자: 응답 송신, ABTS 처리. */

	uint16_t oxid;
	/* [한국어] Originator Exchange ID - frame에서 추출.
	 * 설정자: process_frame.
	 * 읽는 자: 응답 frame의 ox_id 채움. */

	uint16_t rpi;
	/* [한국어] Remote Port Index. */

	struct spdk_nvmf_fc_conn *fc_conn;
	/* [한국어] 소속 connection back-pointer. */

	struct spdk_nvmf_fc_hwqp *hwqp;
	/* [한국어] 처리 hwqp back-pointer. */

	int state;
	/* [한국어] 요청 상태 (enum spdk_nvmf_fc_request_state).
	 * 설정자: nvmf_fc_request_set_state.
	 * 읽는 자: 상태 머신 전이, abort 처리. */

	uint32_t transferred_len;
	/* [한국어] 지금까지 전송된 데이터 길이 (Read XFER 등에서 누적). */

	bool is_aborted;
	/* [한국어] abort 요청됨 표시 - 진행 중 작업 완료 시 abort 경로로. */

	uint32_t magic;
	/* [한국어] 매직 넘버 (디버그) - free된 메모리 use-after-free 검출 등. */

	uint32_t s_id;
	/* [한국어] frame source ID 사본. */

	uint32_t d_id;
	/* [한국어] frame destination ID 사본. */

	uint32_t csn;
	/* [한국어] Command Sequence Number - NVMe-oF FC 스펙. */

	uint32_t app_id;
	/* [한국어] application id (CSCTL 추적/구분용). */

	uint8_t csctl;
	/* [한국어] CS_CTL(Class-Specific Control) 필드 - FC frame 헤더의 priority 등. */

	TAILQ_ENTRY(spdk_nvmf_fc_request) link;
	/* [한국어] hwqp->in_use_reqs 노드. */

	TAILQ_ENTRY(spdk_nvmf_fc_request) conn_link;
	/* [한국어] conn->in_use_reqs 노드. */

	TAILQ_ENTRY(spdk_nvmf_fc_request) fused_link;
	/* [한국어] conn->fused_waiting_queue 노드 (fused 명령 대기). */

	TAILQ_HEAD(, spdk_nvmf_fc_caller_ctx) abort_cbs;
	/* [한국어] abort 완료 시 호출할 콜백 체인 (다중 abort 요청자 지원). */
};

SPDK_STATIC_ASSERT(!offsetof(struct spdk_nvmf_fc_request, req),
		   "FC request and NVMF request address don't match.");
/* [한국어] req가 첫 멤버여야 nvmf_fc_get_fc_req(container_of)가 안전 - 컴파일 타임 검증. */

/*
 * [한국어] fc_request 풀 free list 노드용 마커.
 * fc_request 메모리 영역을 풀에 매달 때 union/casting으로 이 구조체 노드 사용.
 */
struct spdk_nvmf_fc_pooled_request {
	STAILQ_ENTRY(spdk_nvmf_fc_pooled_request) pool_link;
	/* [한국어] conn->pool_queue STAILQ의 노드.
	 * 설정자: 요청 free 시 enqueue.
	 * 읽는 자: 새 요청 alloc 시 dequeue. */
};

/*
 * NVMF FC Association
 *
 * [한국어] NVMe-oF FC Association - 호스트와 tgtport 간의 한 세션 (NVMe-oF 1.x 11.4).
 * 한 association은 1개의 admin connection + 여러 IO connection을 가진다.
 * Connect LS의 첫 회로 만들어지고, Disconnect LS 또는 ABTS로 종료.
 */
struct spdk_nvmf_fc_association {
	uint64_t assoc_id;
	/* [한국어] association ID - tgtport-scoped unique.
	 * 설정자: Create_Association LS 처리 시 할당.
	 * 읽는 자: subsequent Create_Connection LS 매칭. */

	uint32_t s_id;
	/* [한국어] initiator의 S_ID. */

	struct spdk_nvmf_fc_nport *tgtport;
	/* [한국어] 소속 tgt N_Port back-pointer. */

	struct spdk_nvmf_fc_remote_port_info *rport;
	/* [한국어] association 상대방 원격 포트 정보. */

	struct spdk_nvmf_subsystem *subsystem;
	/* [한국어] association이 접속한 NVMe subsystem. */

	struct spdk_nvmf_transport *nvmf_transport;
	/* [한국어] 트랜스포트 back-pointer (FC 인스턴스). */

	enum spdk_nvmf_fc_object_state assoc_state;
	/* [한국어] association 상태 (CREATED/TO_BE_DELETED/ZOMBIE). */

	char host_id[FCNVME_ASSOC_HOSTID_LEN];
	/* [한국어] Create_Association LS의 hostid 필드 사본 (UUID 형태). */

	char host_nqn[SPDK_NVME_NQN_FIELD_SIZE];
	/* [한국어] Create_Association LS의 host NQN. */

	char sub_nqn[SPDK_NVME_NQN_FIELD_SIZE];
	/* [한국어] Create_Association LS의 subsystem NQN - subsystem lookup 키. */

	struct spdk_nvmf_fc_conn *aq_conn; /* connection for admin queue */
	/* [한국어] admin queue connection (qid=0).
	 * 설정자: 첫 connection이 admin이면 여기 저장.
	 * 읽는 자: AER 송신 등 admin 전용 경로. */

	uint16_t conn_count;
	/* [한국어] 활성 connection 수. */

	TAILQ_HEAD(, spdk_nvmf_fc_conn) fc_conns;
	/* [한국어] 활성 connection 리스트. */

	void *conns_buf;
	/* [한국어] connection 객체들 메모리 백킹 (한 번에 alloc). */

	TAILQ_HEAD(, spdk_nvmf_fc_conn) avail_fc_conns;
	/* [한국어] 미사용 conn 풀 (Create_Association 시 미리 alloc된 conn들). */

	TAILQ_ENTRY(spdk_nvmf_fc_association) link;
	/* [한국어] tgtport->fc_associations 노드. */

	/* for port's association free list */
	TAILQ_ENTRY(spdk_nvmf_fc_association) port_free_assoc_list_link;
	/* [한국어] port의 free assoc 풀 노드 (사전 할당). */

	void *ls_del_op_ctx; /* delete assoc. callback list */
	/* [한국어] Delete_Association LS 응답 후 콜백 체인. */

	/* disconnect cmd buffers (sent to initiator) */
	struct spdk_nvmf_fc_srsr_bufs *snd_disconn_bufs;
	/* [한국어] target-initiated Disconnect LS 송신 버퍼 (타겟이 먼저 끊을 때).
	 * 설정자: assoc 삭제 시 alloc.
	 * 읽는 자: LS 송신 후 free. */
};

/*
 * FC Remote Port
 *
 * [한국어] 원격 포트(initiator) 정보 - PRLI한 호스트 1개당 1개.
 */
struct spdk_nvmf_fc_remote_port_info {
	uint32_t s_id;
	/* [한국어] 원격 포트의 FC Source ID.
	 * 설정자: IT_ADD 이벤트 처리.
	 * 읽는 자: frame의 S_ID 매칭. */

	uint32_t rpi;
	/* [한국어] Remote Port Index (LLD 자원 매핑 키). */

	uint32_t assoc_count;
	/* [한국어] 이 rport에 묶인 association 수. */

	struct spdk_nvmf_fc_wwn fc_nodename;
	/* [한국어] 원격 노드 WWN. */

	struct spdk_nvmf_fc_wwn fc_portname;
	/* [한국어] 원격 포트 WWN. */

	enum spdk_nvmf_fc_object_state rport_state;
	/* [한국어] rport 상태. */

	TAILQ_ENTRY(spdk_nvmf_fc_remote_port_info) link;
	/* [한국어] nport->rem_port_list 노드. */
};

/*
 * Poller API error codes
 *
 * [한국어] poller API 호출 결과 코드 - 비동기 콜백의 ret 인자.
 */
enum spdk_nvmf_fc_poller_api_ret {
	SPDK_NVMF_FC_POLLER_API_SUCCESS = 0,
	/* [한국어] 정상 처리 완료. */

	SPDK_NVMF_FC_POLLER_API_ERROR,
	/* [한국어] 일반 에러. */

	SPDK_NVMF_FC_POLLER_API_INVALID_ARG,
	/* [한국어] 잘못된 인자 (NULL 포인터, 범위 밖). */

	SPDK_NVMF_FC_POLLER_API_NO_CONN_ID,
	/* [한국어] conn_id 매칭 실패 (rte_hash lookup miss). */

	SPDK_NVMF_FC_POLLER_API_DUP_CONN_ID,
	/* [한국어] conn_id 중복 (이미 등록됨). */

	SPDK_NVMF_FC_POLLER_API_OXID_NOT_FOUND,
	/* [한국어] ABTS의 oxid 매칭 IO 없음. */
};

/*
 * Poller API definitions
 *
 * [한국어] hwqp poller에 cross-thread로 요청 가능한 작업 종류.
 * 모든 hwqp 작업은 hwqp PG thread에서만 안전하므로, 다른 thread에서 요청 시 spdk_thread_send_msg
 * 패턴으로 이 enum을 사용해 디스패치.
 */
enum spdk_nvmf_fc_poller_api {
	SPDK_NVMF_FC_POLLER_API_ADD_CONNECTION,
	/* [한국어] 새 connection을 hwqp에 추가 (hash insert). */

	SPDK_NVMF_FC_POLLER_API_DEL_CONNECTION,
	/* [한국어] connection을 hwqp에서 제거. */

	SPDK_NVMF_FC_POLLER_API_QUIESCE_QUEUE,
	/* [한국어] hwqp drain - 진행 중 IO만 완료, 새 IO 차단. */

	SPDK_NVMF_FC_POLLER_API_ACTIVATE_QUEUE,
	/* [한국어] hwqp 활성화 - quiesce 해제. */

	SPDK_NVMF_FC_POLLER_API_ABTS_RECEIVED,
	/* [한국어] ABTS frame 도착 처리 위임. */

	SPDK_NVMF_FC_POLLER_API_REQ_ABORT_COMPLETE,
	/* [한국어] 요청 abort 완료 통지. */

	SPDK_NVMF_FC_POLLER_API_ADAPTER_EVENT,
	/* [한국어] LLD 어댑터 이벤트 처리. */

	SPDK_NVMF_FC_POLLER_API_AEN,
	/* [한국어] Asynchronous Event Notification 처리. */

	SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC,
	/* [한국어] hwqp queue sync 시작 (drain marker 송신). */

	SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC_DONE,
	/* [한국어] queue sync 완료 통지 (sync marker 도착). */

	SPDK_NVMF_FC_POLLER_API_ADD_HWQP,
	/* [한국어] poll group에 hwqp 추가. */

	SPDK_NVMF_FC_POLLER_API_REMOVE_HWQP,
	/* [한국어] poll group에서 hwqp 제거. */
};

/*
 * Poller API callback function proto
 *
 * [한국어] poller API 비동기 완료 콜백 시그니처.
 */
typedef void (*spdk_nvmf_fc_poller_api_cb)(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret);

/*
 * Poller API callback data
 *
 * [한국어] 모든 poller API args에 임베드되는 공통 콜백 정보.
 */
struct spdk_nvmf_fc_poller_api_cb_info {
	struct spdk_thread *cb_thread;
	/* [한국어] 콜백을 실행할 thread (요청 시작 thread).
	 * 설정자: poller API 호출 전에 spdk_get_thread().
	 * 읽는 자: 작업 완료 후 spdk_thread_send_msg 대상. */

	spdk_nvmf_fc_poller_api_cb cb_func;
	/* [한국어] 콜백 함수.
	 * 설정자: 호출자.
	 * 읽는 자: 완료 시 호출. */

	void *cb_data;
	/* [한국어] cb_func에 전달될 컨텍스트. */

	enum spdk_nvmf_fc_poller_api_ret ret;
	/* [한국어] 처리 결과 - poller가 작업 완료 시 채움.
	 * 읽는 자: cb_func가 ret로 받음. */
};

/*
 * Poller API structures
 *
 * [한국어] 각 poller API 호출별 전용 args 구조체. 첫 멤버는 보통 대상 객체.
 */

/* [한국어] ADD_CONNECTION용 args. */
struct spdk_nvmf_fc_poller_api_add_connection_args {
	struct spdk_nvmf_fc_conn *fc_conn;
	/* [한국어] 추가할 connection. */

	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
	/* [한국어] 완료 콜백 정보. */
};

/* [한국어] DEL_CONNECTION용 args. */
struct spdk_nvmf_fc_poller_api_del_connection_args {
	struct spdk_nvmf_fc_conn *fc_conn;
	/* [한국어] 제거할 connection. */

	struct spdk_nvmf_fc_hwqp *hwqp;
	/* [한국어] 대상 hwqp. */

	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
	/* [한국어] 완료 콜백. */

	bool send_abts;
	/* [한국어] true면 진행 중 IO들에 ABTS 송신 후 abort. */

	/* internal */
	int fc_request_cnt;
	/* [한국어] 진행 중 abort 요청 카운터 - 모두 0이 되면 콜백 호출. */

	bool backend_initiated;
	/* [한국어] target 측에서 시작한 disconnect인지 (vs 호스트). */
};

/* [한국어] QUIESCE_QUEUE용 args. */
struct spdk_nvmf_fc_poller_api_quiesce_queue_args {
	void   *ctx;
	/* [한국어] 호출자 컨텍스트 (예: 상위 ABTS ctx). */

	struct spdk_nvmf_fc_hwqp *hwqp;
	/* [한국어] quiesce 대상 hwqp. */

	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
	/* [한국어] 완료 콜백. */
};

/* [한국어] ACTIVATE_QUEUE용 args. */
struct spdk_nvmf_fc_poller_api_activate_queue_args {
	struct spdk_nvmf_fc_hwqp *hwqp;
	/* [한국어] 활성화 대상. */

	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
	/* [한국어] 완료 콜백. */
};

/* [한국어] ABTS_RECEIVED용 args. */
struct spdk_nvmf_fc_poller_api_abts_recvd_args {
	struct spdk_nvmf_fc_abts_ctx *ctx;
	/* [한국어] ABTS 처리 컨텍스트 (oxid/rxid 등 포함). */

	struct spdk_nvmf_fc_hwqp *hwqp;
	/* [한국어] ABTS 검색 대상 hwqp. */

	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
	/* [한국어] 완료 콜백. */
};

/* [한국어] QUEUE_SYNC_DONE용 args. */
struct spdk_nvmf_fc_poller_api_queue_sync_done_args {
	struct spdk_nvmf_fc_hwqp *hwqp;
	/* [한국어] 대상 hwqp. */

	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
	/* [한국어] 완료 콜백. */

	uint64_t tag;
	/* [한국어] sync marker tag - 진행 중 sync들 식별. */
};

typedef void (*spdk_nvmf_fc_remove_hwqp_cb)(void *ctx, int err);
/* [한국어] REMOVE_HWQP 완료 콜백 시그니처.
 * @ctx: 호출자 컨텍스트.
 * @err: 0 성공, 음수 errno. */

/* [한국어] REMOVE_HWQP용 args. */
struct spdk_nvmf_fc_poller_api_remove_hwqp_args {
	struct spdk_nvmf_fc_hwqp *hwqp;
	/* [한국어] 제거 대상 hwqp. */

	spdk_nvmf_fc_remove_hwqp_cb cb_fn;
	/* [한국어] 완료 콜백 (전용 시그니처). */

	void *cb_ctx;
	/* [한국어] 콜백 컨텍스트. */

	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
	/* [한국어] 공통 콜백 정보 (보조). */
};

/*
 * [한국어] hwqp의 rport별 conn 그룹 - rport_list_hash의 value type.
 */
struct spdk_nvmf_fc_hwqp_rport {
	uint16_t rpi;
	/* [한국어] 키 (Remote Port Index). */

	TAILQ_HEAD(, spdk_nvmf_fc_conn) conn_list;
	/* [한국어] 이 rport에 묶인 conn 리스트.
	 * 설정자: conn add (rpi별).
	 * 읽는 자: IT_DELETE 시 해당 rport conn 일괄 정리. */
};

/*
 * NVMF LS request structure
 *
 * [한국어] NVMe-oF Link Service 요청 - Connect/Disconnect 등 control plane 명령.
 * LS hwqp이 받은 frame 1개당 1개 생성.
 */
struct spdk_nvmf_fc_ls_rqst {
	struct spdk_nvmf_fc_buffer_desc rqstbuf;
	/* [한국어] LS 요청 페이로드 버퍼 (호스트 → target).
	 * 설정자: LS frame 도착 시 LLD가 채움.
	 * 읽는 자: handle_ls_rqst가 디코드. */

	struct spdk_nvmf_fc_buffer_desc rspbuf;
	/* [한국어] LS 응답 페이로드 버퍼 (target → 호스트).
	 * 설정자: LS 처리 후 응답 빌드. */

	uint32_t rqst_len;
	/* [한국어] 요청 페이로드 길이. */

	uint32_t rsp_len;
	/* [한국어] 응답 페이로드 길이. */

	uint32_t rpi;
	/* [한국어] Remote Port Index - 응답 송신 대상. */

	struct spdk_nvmf_fc_xchg *xchg;
	/* [한국어] 이 LS에 할당된 Exchange. */

	uint16_t oxid;
	/* [한국어] Originator Exchange ID. */

	void *private_data; /* for LLD only (LS does not touch) */
	/* [한국어] LLD 전용 - SPDK는 미접근. */

	TAILQ_ENTRY(spdk_nvmf_fc_ls_rqst) ls_pending_link;
	/* [한국어] hwqp->ls_pending_queue 노드 (자원 부족 시 대기). */

	uint32_t s_id;
	/* [한국어] frame source ID (initiator). */

	uint32_t d_id;
	/* [한국어] frame destination ID (target nport). */

	struct spdk_nvmf_fc_nport *nport;
	/* [한국어] 대상 N_Port 포인터. */

	struct spdk_nvmf_fc_remote_port_info *rport;
	/* [한국어] 원격 포트 포인터 (S_ID로 lookup). */

	struct spdk_nvmf_tgt *nvmf_tgt;
	/* [한국어] subsystem lookup용 target back-pointer. */
};

/*
 * RQ Buffer LS Overlay Structure
 *
 * [한국어] LLD의 RQ 버퍼(고정 크기 FCNVME_MAX_LS_BUFFER_SIZE)에 LS 요청 데이터를 overlay하는 구조.
 * 한 RQ 버퍼 안에 [요청 페이로드][응답 페이로드][SPDK ls_rqst 메타][padding] 순서로 배치.
 * SPDK_STATIC_ASSERT로 전체 크기가 RQ 버퍼와 정확히 일치하도록 검증.
 */
#define FCNVME_LS_RSVD_SIZE (FCNVME_MAX_LS_BUFFER_SIZE - \
	(sizeof(struct spdk_nvmf_fc_ls_rqst) + FCNVME_MAX_LS_REQ_SIZE + FCNVME_MAX_LS_RSP_SIZE))
/* [한국어] padding 크기 - 나머지 영역을 채워 RQ 버퍼 정렬 유지. */

struct spdk_nvmf_fc_rq_buf_ls_request {
	uint8_t rqst[FCNVME_MAX_LS_REQ_SIZE];
	/* [한국어] 요청 페이로드 영역 (FC frame 데이터). */

	uint8_t resp[FCNVME_MAX_LS_RSP_SIZE];
	/* [한국어] 응답 페이로드 영역 (target이 채움). */

	struct spdk_nvmf_fc_ls_rqst ls_rqst;
	/* [한국어] SPDK용 LS 요청 메타데이터. */

	uint8_t rsvd[FCNVME_LS_RSVD_SIZE];
	/* [한국어] padding - 정렬 유지. */
};

SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_rq_buf_ls_request) ==
		   FCNVME_MAX_LS_BUFFER_SIZE, "LS RQ Buffer overflow");
/* [한국어] overlay 구조 크기가 RQ 버퍼와 정확히 일치하는지 컴파일 타임 검증. */

/* Poller API structures (arguments and callback data */
typedef void (*spdk_nvmf_fc_del_assoc_cb)(void *arg, uint32_t err);
/* [한국어] association 삭제 완료 콜백.
 * @arg: 호출자 컨텍스트.
 * @err: 0 성공, 음수 errno. */

typedef void (*spdk_nvmf_fc_del_conn_cb)(void *arg);
/* [한국어] connection 삭제 완료 콜백 (에러 인자 없음). */

/* [한국어] LS Create_Connection 처리 시 ADD_CONNECTION poller API 호출용 컨텍스트. */
struct spdk_nvmf_fc_ls_add_conn_api_data {
	struct spdk_nvmf_fc_poller_api_add_connection_args args;
	/* [한국어] poller API에 전달할 args. */

	struct spdk_nvmf_fc_ls_rqst *ls_rqst;
	/* [한국어] 원본 LS 요청 - 응답 빌드에 필요. */

	struct spdk_nvmf_fc_association *assoc;
	/* [한국어] 대상 association. */

	bool aq_conn; /* true if adding connection for new association */
	/* [한국어] true면 새 association의 첫(admin) connection 추가 - 별도 처리. */
};

/* Disconnect (connection) request functions */
/* [한국어] LS Disconnect 처리 시 DEL_CONNECTION poller API 호출용 컨텍스트. */
struct spdk_nvmf_fc_ls_del_conn_api_data {
	struct spdk_nvmf_fc_poller_api_del_connection_args args;
	/* [한국어] poller API args. */

	struct spdk_nvmf_fc_ls_rqst *ls_rqst;
	/* [한국어] 원본 Disconnect LS 요청. */

	struct spdk_nvmf_fc_association *assoc;
	/* [한국어] 대상 association. */

	bool aq_conn; /* true if deleting AQ connection */
	/* [한국어] admin queue conn 삭제 여부 - 마지막이면 association 삭제 트리거. */

	spdk_nvmf_fc_del_conn_cb del_conn_cb;
	/* [한국어] 삭제 완료 콜백. */

	void *del_conn_cb_data;
	/* [한국어] 콜백 컨텍스트. */
};

/* used by LS disconnect association cmd handling */
/* [한국어] LS Disconnect_Association 처리용. */
struct spdk_nvmf_fc_ls_disconn_assoc_api_data {
	struct spdk_nvmf_fc_nport *tgtport;
	/* [한국어] 대상 N_Port. */

	struct spdk_nvmf_fc_ls_rqst *ls_rqst;
	/* [한국어] 원본 LS 요청. */
};

/* used by delete association call */
/* [한국어] association 삭제 처리 (LS-driven 또는 backend-initiated) 컨텍스트. */
struct spdk_nvmf_fc_delete_assoc_api_data {
	struct spdk_nvmf_fc_poller_api_del_connection_args args;
	/* [한국어] DEL_CONNECTION poller API args (assoc 안의 모든 conn에 대해 호출). */

	struct spdk_nvmf_fc_association *assoc;
	/* [한국어] 삭제 대상 association. */

	bool from_ls_rqst;   /* true = request came for LS */
	/* [한국어] true면 LS Disconnect_Association으로 시작, false면 target backend 시작. */

	spdk_nvmf_fc_del_assoc_cb del_assoc_cb;
	/* [한국어] 완료 콜백. */

	void *del_assoc_cb_data;
	/* [한국어] 콜백 컨텍스트. */
};

/*
 * [한국어] LS 작업 컨텍스트의 union 컨테이너 - 다양한 LS 작업 유형을 한 객체로 관리.
 * next_op_ctx로 단일 연결 리스트 형태 - 순차적 작업 chain 가능 (예: assoc 삭제 → conn 삭제 chain).
 */
struct nvmf_fc_ls_op_ctx {
	union {
		struct spdk_nvmf_fc_ls_add_conn_api_data add_conn;
		/* [한국어] Create_Connection LS용. */

		struct spdk_nvmf_fc_ls_del_conn_api_data del_conn;
		/* [한국어] Disconnect (connection) LS용. */

		struct spdk_nvmf_fc_ls_disconn_assoc_api_data disconn_assoc;
		/* [한국어] Disconnect_Association LS용. */

		struct spdk_nvmf_fc_delete_assoc_api_data del_assoc;
		/* [한국어] association 삭제 (chained) 컨텍스트. */
	} u;
	struct  nvmf_fc_ls_op_ctx *next_op_ctx;
	/* [한국어] 다음 작업 노드 - chained pipeline. */
};

/* [한국어] QUEUE_SYNC poller API용 args - hwqp drain marker 송신/수확. */
struct spdk_nvmf_fc_poller_api_queue_sync_args {
	uint64_t u_id;
	/* [한국어] sync marker unique ID - 다중 동시 sync 식별. */

	struct spdk_nvmf_fc_hwqp *hwqp;
	/* [한국어] sync 대상 hwqp. */

	struct spdk_nvmf_fc_poller_api_cb_info cb_info;
	/* [한국어] 완료 콜백. */

	/* Used internally by poller */
	TAILQ_ENTRY(spdk_nvmf_fc_poller_api_queue_sync_args) link;
	/* [한국어] hwqp->sync_cbs 큐의 노드 (poller 내부 사용). */
};

/**
 * Following defines and structures are used to pass messages between main thread
 * and FCT driver.
 *
 * [한국어] LLD(FCT 드라이버)와 SPDK main thread 간 이벤트 종류.
 * LLD가 nvmf_fc_main_enqueue_event로 이벤트 큐잉 → main thread가 비동기 처리 → cb_func로 응답.
 */
enum spdk_fc_event {
	SPDK_FC_HW_PORT_INIT,
	/* [한국어] 새 HW port 초기화 (HBA 부팅/insertion). */

	SPDK_FC_HW_PORT_FREE,
	/* [한국어] HW port 자원 해제. */

	SPDK_FC_HW_PORT_ONLINE,
	/* [한국어] port link up - 사용 가능. */

	SPDK_FC_HW_PORT_OFFLINE,
	/* [한국어] port link down. */

	SPDK_FC_HW_PORT_RESET,
	/* [한국어] port reset 요청 (옵션으로 queue dump 포함). */

	SPDK_FC_NPORT_CREATE,
	/* [한국어] N_Port 생성 (NPIV vNIC 등). */

	SPDK_FC_NPORT_DELETE,
	/* [한국어] N_Port 삭제. */

	SPDK_FC_IT_ADD,    /* PRLI */
	/* [한국어] Initiator-Target nexus 추가 (PRLI 완료) - rport 등록. */

	SPDK_FC_IT_DELETE, /* PRLI */
	/* [한국어] I-T nexus 제거 (initiator logout). */

	SPDK_FC_ABTS_RECV,
	/* [한국어] ABTS frame 수신. */

	SPDK_FC_HW_PORT_DUMP,
	/* [한국어] 디버그용 port 상태 dump 요청. */

	SPDK_FC_UNRECOVERABLE_ERR,
	/* [한국어] 복구 불가 에러 - HBA fatal 등. */

	SPDK_FC_EVENT_MAX,
	/* [한국어] enum count sentinel. */
};

/**
 * Arguments for to dump assoc id
 *
 * [한국어] association ID 디버그 dump 인자.
 */
struct spdk_nvmf_fc_dump_assoc_id_args {
	uint8_t                           pport_handle;
	/* [한국어] physical port handle. */

	uint16_t                          nport_handle;
	/* [한국어] N_Port handle. */

	uint32_t                          assoc_id;
	/* [한국어] dump할 assoc ID. */
};

/**
 * Arguments for HW port init event.
 *
 * [한국어] HW port init 이벤트 args - LLD가 SPDK에 새 port 정보 전달.
 */
struct spdk_nvmf_fc_hw_port_init_args {
	spdk_nvmf_fc_lld_fc_port_t     lld_fc_port;
	/* [한국어] LLD-소유 port 핸들. */

	uint32_t                       ls_queue_size;
	/* [한국어] LS 큐 깊이 (RQ entry 수). */

	spdk_nvmf_fc_lld_hwqp_t        ls_queue;
	/* [한국어] LS 전용 hwqp 핸들. */

	uint32_t                       io_queue_size;
	/* [한국어] IO 큐 1개 깊이. */

	uint32_t                       io_queue_cnt;
	/* [한국어] IO 큐 개수. */

	spdk_nvmf_fc_lld_hwqp_t       *io_queues;
	/* [한국어] IO hwqp 핸들 배열. */

	void                          *cb_ctx;
	/* [한국어] 응답 콜백 컨텍스트. */

	void                          *port_ctx;
	/* [한국어] LLD-vendor port 컨텍스트. */

	uint8_t                        port_handle;
	/* [한국어] SPDK 측 port handle (전역 unique). */

	uint8_t                        nvme_aq_index;  /* io_queue used for nvme admin queue */
	/* [한국어] io_queues 중 NVMe admin queue로 사용할 인덱스. */

	uint16_t                       fcp_rq_id; /* Base rq ID of SCSI queue */
	/* [한국어] FCP RQ ID base. */
};

/**
 * Arguments for HW port online event.
 *
 * [한국어] port online 이벤트 args.
 */
struct spdk_nvmf_fc_hw_port_online_args {
	uint8_t port_handle;
	/* [한국어] online이 된 port handle. */

	void   *cb_ctx;
	/* [한국어] 응답 콜백 컨텍스트. */
};

/**
 * Arguments for HW port offline event.
 *
 * [한국어] port offline 이벤트 args.
 */
struct spdk_nvmf_fc_hw_port_offline_args {
	uint8_t port_handle;
	/* [한국어] offline이 된 port handle. */

	void   *cb_ctx;
	/* [한국어] 콜백 컨텍스트. */
};

/**
 * Arguments for n-port add event.
 *
 * [한국어] N_Port 생성 이벤트 args.
 */
struct spdk_nvmf_fc_nport_create_args {
	uint8_t                     port_handle;
	/* [한국어] HW port handle. */

	uint16_t                    nport_handle;
	/* [한국어] 새 N_Port handle. */

	struct spdk_uuid            container_uuid; /* UUID of the nports container */
	/* [한국어] N_Port가 속한 컨테이너의 UUID (그룹 식별). */

	struct spdk_uuid            nport_uuid;     /* Unique UUID for the nport */
	/* [한국어] N_Port 고유 UUID. */

	uint32_t                    d_id;
	/* [한국어] N_Port FC address. */

	struct spdk_nvmf_fc_wwn fc_nodename;
	/* [한국어] N_Port node WWN. */

	struct spdk_nvmf_fc_wwn fc_portname;
	/* [한국어] N_Port port WWN. */

	uint32_t                    subsys_id; /* Subsystemid */
	/* [한국어] 연관 subsystem id. */

	char                        port_id[SPDK_NVMF_PORT_ID_MAX_LEN];
	/* [한국어] 사용자 가시 port_id 문자열. */

	void                       *cb_ctx;
	/* [한국어] 콜백 컨텍스트. */
};

/**
 * Arguments for n-port delete event.
 *
 * [한국어] N_Port 삭제 이벤트 args.
 */
struct spdk_nvmf_fc_nport_delete_args {
	uint8_t  port_handle;
	/* [한국어] HW port. */

	uint32_t nport_handle;
	/* [한국어] 삭제할 N_Port. */

	uint32_t subsys_id; /* Subsystem id */
	/* [한국어] 연관 subsystem id. */

	void    *cb_ctx;
	/* [한국어] 콜백 컨텍스트. */
};

/**
 * Arguments for I_T add event.
 *
 * [한국어] PRLI 완료로 I-T nexus 등록 이벤트.
 */
struct spdk_nvmf_fc_hw_i_t_add_args {
	uint8_t                      port_handle;
	/* [한국어] HW port. */

	uint32_t                     nport_handle;
	/* [한국어] target N_Port. */

	uint16_t                     itn_handle;
	/* [한국어] LLD-소유 I-T nexus 핸들. */

	uint32_t                     rpi;
	/* [한국어] Remote Port Index. */

	uint32_t                     s_id;
	/* [한국어] initiator FC address. */

	uint32_t                     initiator_prli_info;
	/* [한국어] PRLI에서 initiator가 광고한 service params. */

	uint32_t                     target_prli_info; /* populated by the SPDK main */
	/* [한국어] target이 응답할 service params - SPDK가 채움 (nvmf_fc_get_prli_service_params). */

	struct spdk_nvmf_fc_wwn  fc_nodename;
	/* [한국어] initiator node WWN. */

	struct spdk_nvmf_fc_wwn  fc_portname;
	/* [한국어] initiator port WWN. */

	void                        *cb_ctx;
	/* [한국어] 콜백 컨텍스트. */
};

/**
 * Arguments for I_T delete event.
 *
 * [한국어] I-T nexus 삭제 이벤트.
 */
struct spdk_nvmf_fc_hw_i_t_delete_args {
	uint8_t  port_handle;
	/* [한국어] HW port. */

	uint32_t nport_handle;
	/* [한국어] N_Port. */

	uint16_t itn_handle;    /* Only used by FC LLD driver; unused in SPDK */
	/* [한국어] LLD 전용 - SPDK 미사용. */

	uint32_t rpi;
	/* [한국어] Remote Port Index. */

	uint32_t s_id;
	/* [한국어] initiator address. */

	void    *cb_ctx;
	/* [한국어] 콜백 컨텍스트. */
};

/**
 * Arguments for ABTS  event.
 *
 * [한국어] ABTS frame 수신 이벤트 args.
 */
struct spdk_nvmf_fc_abts_args {
	uint8_t  port_handle;
	/* [한국어] HW port. */

	uint32_t nport_handle;
	/* [한국어] N_Port. */

	uint32_t rpi;
	/* [한국어] initiator rpi. */

	uint16_t oxid, rxid;
	/* [한국어] abort 대상 exchange ID들. */

	void    *cb_ctx;
	/* [한국어] 콜백 컨텍스트. */
};

/**
 * Arguments for port reset event.
 *
 * [한국어] HW port reset 이벤트 args (옵션 dump 포함).
 */
struct spdk_nvmf_fc_hw_port_reset_args {
	uint8_t    port_handle;
	/* [한국어] reset 대상. */

	bool       dump_queues;
	/* [한국어] true면 reset 전 queue 상태 dump. */

	char       reason[SPDK_FC_HW_DUMP_REASON_STR_MAX_SIZE];
	/* [한국어] reset 사유 문자열 (디버그). */

	uint32_t **dump_buf;
	/* [한국어] dump 출력 버퍼 (이중 포인터 - LLD가 alloc해 SPDK가 채움). */

	void      *cb_ctx;
	/* [한국어] 콜백 컨텍스트. */
};

/**
 * Arguments for unrecoverable error event
 *
 * [한국어] 복구 불가 에러 이벤트 - 현재는 추가 인자 없음.
 */
struct spdk_nvmf_fc_unrecoverable_error_event_args {
};

/* [한국어] HW port free 이벤트 args. */
struct spdk_nvmf_fc_hw_port_free_args {
	uint8_t port_handle;
	/* [한국어] free할 port. */

	void    *cb_ctx;
	/* [한국어] 콜백 컨텍스트. */
};

/**
 * Callback function to the FCT driver.
 *
 * [한국어] 이벤트 처리 완료를 LLD에 알리는 콜백 시그니처.
 * @port_handle: 처리한 port.
 * @event_type: 처리한 이벤트 종류.
 * @arg: 원래 args 포인터.
 * @err: 0 성공, 음수 errno.
 */
typedef void (*spdk_nvmf_fc_callback)(uint8_t port_handle,
				      enum spdk_fc_event event_type,
				      void *arg, int err);

/* [한국어] REMOVE_HWQP 일괄 처리 시 진행 카운팅 콜백 args. */
struct spdk_nvmf_fc_remove_hwqp_cb_args {
	uint16_t pending_remove_hwqp;
	/* [한국어] 남은 hwqp 제거 수 - 0 도달 시 cb_fn 호출. */

	spdk_nvmf_fc_callback cb_fn;
	/* [한국어] LLD callback. */

	void *cb_args;
	/* [한국어] cb_fn args. */
};

/**
 * Enqueue an FCT event to main thread
 *
 * \param event_type Type of the event.
 * \param args Pointer to the argument structure.
 * \param cb_func Callback function into fc driver.
 *
 * \return 0 on success, non-zero on failure.
 *
 * [한국어]
 * nvmf_fc_main_enqueue_event - LLD에서 SPDK main thread로 이벤트 큐잉
 *
 * 이 함수는 LLD가 어떤 thread에서든 호출 가능 - 내부적으로 spdk_thread_send_msg로 main thread에
 * 작업을 예약. 처리 완료 후 cb_func가 호출됨 (LLD thread는 가능).
 */
int nvmf_fc_main_enqueue_event(enum spdk_fc_event event_type,
			       void *args,
			       spdk_nvmf_fc_callback cb_func);

/*
 * dump info
 *
 * [한국어] queue dump 출력용 buffer + offset 컨텍스트.
 */
struct spdk_nvmf_fc_queue_dump_info {
	char *buffer;
	/* [한국어] dump 출력 버퍼 (LLD가 alloc). */

	int   offset;
	/* [한국어] 현재 쓰기 위치 - vsnprintf 마다 advance. */
};
#define SPDK_FC_HW_DUMP_BUF_SIZE (10 * 4096)        /* [한국어] dump 버퍼 표준 크기 = 40KB - 한 hwqp의 모든 큐 상태 담기에 충분 */

/*
 * [한국어]
 * nvmf_fc_dump_buf_print - dump 버퍼에 printf-style로 안전하게 추가
 *
 * @dump_info: dump 컨텍스트 (buffer/offset)
 * @fmt: printf 포맷
 * ...: 가변 인자
 *
 * 남은 공간이 있을 때만 작성하고, 잘리면 offset을 buffer_size로 클램프.
 * 실행 컨텍스트: dump 처리 중인 main thread 또는 hwqp poller.
 */
static inline void
nvmf_fc_dump_buf_print(struct spdk_nvmf_fc_queue_dump_info *dump_info, char *fmt, ...)
{
	uint64_t buffer_size = SPDK_FC_HW_DUMP_BUF_SIZE; /* [한국어] dump 버퍼 표준 크기 캐시 */
	int32_t avail = (int32_t)(buffer_size - dump_info->offset); /* [한국어] 남은 공간 계산 - 음수 방지 위해 signed */

	if (avail > 0) {                            /* [한국어] 공간 남았을 때만 작성 - 가득 찬 후에는 silently drop */
		va_list ap;                         /* [한국어] 가변 인자 처리 핸들 */
		int32_t written;                    /* [한국어] vsnprintf 반환 - 실제 작성한 바이트 수 또는 절단 시 필요 길이 */

		va_start(ap, fmt);                  /* [한국어] 가변 인자 시작 - fmt 다음 인자부터 ap로 접근 */
		written = vsnprintf(dump_info->buffer + dump_info->offset, avail, fmt, ap); /* [한국어] 버퍼 끝부터 avail 만큼 작성 - NUL 포함 */
		if (written >= avail) {             /* [한국어] 절단 발생 - offset을 끝까지 클램프 */
			dump_info->offset += avail;
		} else {                            /* [한국어] 정상 작성 - 실제 길이만큼 advance */
			dump_info->offset += written;
		}
		va_end(ap);                         /* [한국어] 가변 인자 종료 */
	}
}

/*
 * NVMF FC caller callback definitions
 *
 * [한국어] 일반 caller 콜백 노드 - abort 등에서 다중 콜백 등록 가능 (TAILQ).
 */

struct spdk_nvmf_fc_caller_ctx {
	void *ctx;
	/* [한국어] 호출자 컨텍스트 (예: hwqp 포인터). */

	spdk_nvmf_fc_caller_cb cb;
	/* [한국어] 콜백 함수. */

	void *cb_args;
	/* [한국어] 콜백 인자. */

	TAILQ_ENTRY(spdk_nvmf_fc_caller_ctx) link;
	/* [한국어] 부모 객체의 콜백 체인 노드 (예: fc_request->abort_cbs). */
};

/*
 * NVMF FC Exchange Info (for debug)
 *
 * [한국어] Exchange 풀 통계 (디버그용).
 */
struct spdk_nvmf_fc_xchg_info {
	uint32_t xchg_base;
	/* [한국어] xchg ID 시작 base. */

	uint32_t xchg_total_count;
	/* [한국어] 총 exchange 수. */

	uint32_t xchg_avail_count;
	/* [한국어] 사용 가능한(free) exchange 수. */

	uint32_t send_frame_xchg_id;
	/* [한국어] send_frame 전용 xchg ID. */

	uint8_t send_frame_seqid;
	/* [한국어] send_frame 시퀀스 ID. */
};

/*
 * NVMF FC inline and function prototypes
 */

/*
 * [한국어]
 * nvmf_fc_get_fc_req - spdk_nvmf_request → spdk_nvmf_fc_request 변환 (container_of)
 *
 * @req: 공통 NVMe-oF request 포인터
 * @return: 임베드한 FC request 포인터
 *
 * SPDK_STATIC_ASSERT(offsetof==0)로 첫 멤버임이 보장되므로 단순 캐스팅이 안전.
 */
static inline struct spdk_nvmf_fc_request *
nvmf_fc_get_fc_req(struct spdk_nvmf_request *req)
{
	return (struct spdk_nvmf_fc_request *)      /* [한국어] container_of 패턴 - offsetof로 부모 구조체 시작 주소 계산 */
	       ((uintptr_t)req - offsetof(struct spdk_nvmf_fc_request, req)); /* [한국어] req가 첫 멤버라 offset=0이지만 일반화된 형태로 작성 */
}

/*
 * [한국어]
 * nvmf_fc_is_port_dead - hwqp이 속한 port가 dead 상태인지
 *
 * QUIESCED는 사실상 dead로 취급 - 새 IO 거부.
 */
static inline bool
nvmf_fc_is_port_dead(struct spdk_nvmf_fc_hwqp *hwqp)
{
	switch (hwqp->fc_port->hw_port_status) {    /* [한국어] hwqp이 속한 HW port 상태 분기 */
	case SPDK_FC_PORT_QUIESCED:                 /* [한국어] quiesce된 port - drain 중이므로 새 IO 거부 */
		return true;
	default:                                    /* [한국어] OFFLINE/ONLINE은 dead 아님 (OFFLINE도 추후 복귀 가능) */
		return false;
	}
}

/*
 * [한국어]
 * nvmf_fc_req_in_xfer - 요청이 데이터/응답 전송 중 상태인지
 *
 * 전송 중인 요청은 abort 시 ABTS 송신이 필요할 수 있음.
 */
static inline bool
nvmf_fc_req_in_xfer(struct spdk_nvmf_fc_request *fc_req)
{
	switch (fc_req->state) {                    /* [한국어] 요청 상태 분기 */
	case SPDK_NVMF_FC_REQ_READ_XFER:            /* [한국어] Read 데이터 전송 중 */
	case SPDK_NVMF_FC_REQ_READ_RSP:             /* [한국어] Read 응답 전송 중 */
	case SPDK_NVMF_FC_REQ_WRITE_XFER:           /* [한국어] Write 데이터 수신 중 */
	case SPDK_NVMF_FC_REQ_WRITE_RSP:            /* [한국어] Write 응답 전송 중 */
	case SPDK_NVMF_FC_REQ_NONE_RSP:             /* [한국어] 데이터 없는 응답 전송 중 */
		return true;                        /* [한국어] 위 상태들은 wire 위에 frame이 있는 상태 */
	default:                                    /* [한국어] BDEV/INIT 등은 wire 위에 frame 없음 - abort 시 단순 정리 */
		return false;
	}
}

/*
 * [한국어]
 * nvmf_fc_create_trid - WWN 쌍으로부터 NVMe-oF FC trid 채우기
 *
 * @trid: 출력 trid
 * @n_wwn: node WWN
 * @p_wwn: port WWN
 *
 * traddr = "nn-0x<node>:pn-0x<port>" 형식으로 NVMe-oF FC 표준 형식 사용.
 * trsvcid = "none" (FC는 포트 개념 없음 - WWN이 주소).
 */
static inline void
nvmf_fc_create_trid(struct spdk_nvme_transport_id *trid, uint64_t n_wwn, uint64_t p_wwn)
{
	spdk_nvme_trid_populate_transport(trid, SPDK_NVME_TRANSPORT_FC); /* [한국어] trtype=FC, trstring 채움 */
	trid->adrfam = SPDK_NVMF_ADRFAM_FC;         /* [한국어] address family를 FC로 명시 */
	snprintf(trid->trsvcid, sizeof(trid->trsvcid), "none"); /* [한국어] FC는 트랜스포트 service id 미사용 - "none" */
	snprintf(trid->traddr, sizeof(trid->traddr), "nn-0x%lx:pn-0x%lx", n_wwn, p_wwn); /* [한국어] FC traddr 표준 형식 - node WWN + port WWN hex */
}

/*
 * [한국어]
 * nvmf_fc_ls_init - HW port의 LS 처리 초기화 (LS hwqp 준비)
 */
void nvmf_fc_ls_init(struct spdk_nvmf_fc_port *fc_port);

/*
 * [한국어]
 * nvmf_fc_ls_fini - LS 처리 자원 해제
 */
void nvmf_fc_ls_fini(struct spdk_nvmf_fc_port *fc_port);

/*
 * [한국어]
 * nvmf_fc_handle_ls_rqst - LS frame 디코드 및 디스패치 (Connect/Disconnect 등)
 *
 * @ls_rqst: 도착한 LS 요청
 *
 * fc_ls.c의 핵심 진입점. opcode에 따라 association/connection 생성/삭제로 분기.
 * 실행 컨텍스트: LS hwqp PG thread.
 */
void nvmf_fc_handle_ls_rqst(struct spdk_nvmf_fc_ls_rqst *ls_rqst);

/*
 * [한국어]
 * nvmf_fc_ls_add_conn_failure - Create_Connection LS 처리 실패 시 정리
 *
 * @assoc/@ls_rqst/@fc_conn: 부분 생성된 자원
 * @aq_conn: admin queue conn 시도였는지 (true면 association도 정리)
 */
void nvmf_fc_ls_add_conn_failure(
	struct spdk_nvmf_fc_association *assoc,
	struct spdk_nvmf_fc_ls_rqst *ls_rqst,
	struct spdk_nvmf_fc_conn *fc_conn,
	bool aq_conn);

/*
 * [한국어]
 * nvmf_fc_init_hwqp - hwqp 자료구조 초기화 (rte_hash, 카운터 등)
 */
int nvmf_fc_init_hwqp(struct spdk_nvmf_fc_port *fc_port, struct spdk_nvmf_fc_hwqp *hwqp);

/*
 * [한국어]
 * nvmf_fc_hwqp_find_fc_conn - hwqp의 conn_id hash에서 conn 검색
 *
 * @return: 매칭 conn 또는 NULL
 */
struct spdk_nvmf_fc_conn *nvmf_fc_hwqp_find_fc_conn(struct spdk_nvmf_fc_hwqp *hwqp,
		uint64_t conn_id);

/*
 * [한국어]
 * nvmf_fc_port_lookup - port_hdl로 fc_port 검색 (전역 fc_port_list)
 */
struct spdk_nvmf_fc_port *nvmf_fc_port_lookup(uint8_t port_hdl);

/*
 * [한국어]
 * nvmf_fc_port_is_offline - HW port가 OFFLINE 상태인지
 */
bool nvmf_fc_port_is_offline(struct spdk_nvmf_fc_port *fc_port);

/*
 * [한국어]
 * nvmf_fc_port_set_offline - port 상태를 OFFLINE으로 설정 (검증 포함)
 */
int nvmf_fc_port_set_offline(struct spdk_nvmf_fc_port *fc_port);

/*
 * [한국어]
 * nvmf_fc_port_is_online - port가 ONLINE 상태인지
 */
bool nvmf_fc_port_is_online(struct spdk_nvmf_fc_port *fc_port);

/*
 * [한국어]
 * nvmf_fc_port_set_online - port 상태를 ONLINE으로 설정
 */
int nvmf_fc_port_set_online(struct spdk_nvmf_fc_port *fc_port);

/*
 * [한국어]
 * nvmf_fc_rport_set_state - remote port 상태 전이 (CREATED/TO_BE_DELETED/ZOMBIE)
 */
int nvmf_fc_rport_set_state(struct spdk_nvmf_fc_remote_port_info *rport,
			    enum spdk_nvmf_fc_object_state state);

/*
 * [한국어]
 * nvmf_fc_port_add - 전역 fc_port_list에 port 추가
 */
void nvmf_fc_port_add(struct spdk_nvmf_fc_port *fc_port);

/*
 * [한국어]
 * nvmf_fc_port_add_nport - port에 N_Port 추가
 */
int nvmf_fc_port_add_nport(struct spdk_nvmf_fc_port *fc_port,
			   struct spdk_nvmf_fc_nport *nport);

/*
 * [한국어]
 * nvmf_fc_port_remove_nport - port에서 N_Port 제거
 */
int nvmf_fc_port_remove_nport(struct spdk_nvmf_fc_port *fc_port,
			      struct spdk_nvmf_fc_nport *nport);

/*
 * [한국어]
 * nvmf_fc_nport_find - (port_hdl, nport_hdl)로 N_Port 검색
 */
struct spdk_nvmf_fc_nport *nvmf_fc_nport_find(uint8_t port_hdl, uint16_t nport_hdl);

/*
 * [한국어]
 * nvmf_fc_nport_set_state - N_Port 상태 전이
 */
int nvmf_fc_nport_set_state(struct spdk_nvmf_fc_nport *nport,
			    enum spdk_nvmf_fc_object_state state);

/*
 * [한국어]
 * nvmf_fc_nport_add_rem_port - N_Port에 원격 포트 추가
 */
bool nvmf_fc_nport_add_rem_port(struct spdk_nvmf_fc_nport *nport,
				struct spdk_nvmf_fc_remote_port_info *rem_port);

/*
 * [한국어]
 * nvmf_fc_nport_remove_rem_port - N_Port에서 원격 포트 제거
 */
bool nvmf_fc_nport_remove_rem_port(struct spdk_nvmf_fc_nport *nport,
				   struct spdk_nvmf_fc_remote_port_info *rem_port);

/*
 * [한국어]
 * nvmf_fc_nport_has_no_rport - N_Port에 등록된 rport가 없는지 (cleanup 가능 여부)
 */
bool nvmf_fc_nport_has_no_rport(struct spdk_nvmf_fc_nport *nport);

/*
 * [한국어]
 * nvmf_fc_assoc_set_state - association 상태 전이
 */
int nvmf_fc_assoc_set_state(struct spdk_nvmf_fc_association *assoc,
			    enum spdk_nvmf_fc_object_state state);

/*
 * [한국어]
 * nvmf_fc_delete_association - association 삭제 (모든 conn disconnect)
 *
 * @send_abts: 진행 중 IO에 ABTS 송신 후 abort
 * @backend_initiated: target 측에서 시작한 삭제인지
 * @del_assoc_cb/@cb_data: 완료 콜백
 */
int nvmf_fc_delete_association(struct spdk_nvmf_fc_nport *tgtport,
			       uint64_t assoc_id, bool send_abts, bool backend_initiated,
			       spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
			       void *cb_data);

/*
 * [한국어]
 * nvmf_fc_delete_connection - 단일 connection 삭제
 */
int nvmf_fc_delete_connection(struct spdk_nvmf_fc_conn *fc_conn, bool send_abts,
			      bool backend_initiated, spdk_nvmf_fc_del_conn_cb cb_fn,
			      void *cb_data);

/*
 * [한국어]
 * nvmf_ctrlr_is_on_nport - ctrlr이 특정 (port_hdl, nport_hdl) 위에 있는지
 *
 * port/nport 삭제 시 영향받는 ctrlr 식별에 사용.
 */
bool nvmf_ctrlr_is_on_nport(uint8_t port_hdl, uint16_t nport_hdl,
			    struct spdk_nvmf_ctrlr *ctrlr);

/*
 * [한국어]
 * nvmf_fc_assign_queue_to_main_thread - hwqp을 main thread에 임시 배정
 *
 * 초기 init 시 main thread에서 처리 후 적절한 PG로 재할당하는 패턴.
 */
void nvmf_fc_assign_queue_to_main_thread(struct spdk_nvmf_fc_hwqp *hwqp);

/*
 * [한국어]
 * nvmf_fc_poll_group_valid - poll group이 유효한 fc poll group인지 검증
 */
bool nvmf_fc_poll_group_valid(struct spdk_nvmf_fc_poll_group *fgroup);

/*
 * [한국어]
 * nvmf_fc_poll_group_add_hwqp - 적절한 PG에 hwqp 추가 (라운드로빈 등)
 */
void nvmf_fc_poll_group_add_hwqp(struct spdk_nvmf_fc_hwqp *hwqp);

/*
 * [한국어]
 * nvmf_fc_poll_group_remove_hwqp - PG에서 hwqp 제거 (비동기)
 */
void nvmf_fc_poll_group_remove_hwqp(struct spdk_nvmf_fc_hwqp *hwqp,
				    spdk_nvmf_fc_remove_hwqp_cb cb_fn, void *cb_ctx);

/*
 * [한국어]
 * nvmf_fc_hwqp_set_online - hwqp 활성화
 */
int nvmf_fc_hwqp_set_online(struct spdk_nvmf_fc_hwqp *hwqp);

/*
 * [한국어]
 * nvmf_fc_hwqp_set_offline - hwqp 비활성화
 */
int nvmf_fc_hwqp_set_offline(struct spdk_nvmf_fc_hwqp *hwqp);

/*
 * [한국어]
 * nvmf_fc_get_prli_service_params - PRLI 응답에 사용할 target service parameters
 *
 * @return: target_prli_info에 채울 32-bit 값 (NVMe-oF FC 스펙에 따른 비트 조합)
 */
uint32_t nvmf_fc_get_prli_service_params(void);

/*
 * [한국어]
 * nvmf_fc_handle_abts_frame - ABTS frame 처리 진입점
 *
 * @nport/@rpi/@oxid/@rxid: ABTS 식별 정보
 *
 * 모든 hwqp에 query 후 매칭 IO를 abort.
 */
void nvmf_fc_handle_abts_frame(struct spdk_nvmf_fc_nport *nport, uint16_t rpi, uint16_t oxid,
			       uint16_t rxid);

/*
 * [한국어]
 * nvmf_fc_request_abort - 단일 fc_request abort 요청
 *
 * @send_abts: target → initiator로 ABTS frame 송신
 * @cb/@cb_args: abort 완료 콜백 (체인 등록 가능)
 */
void nvmf_fc_request_abort(struct spdk_nvmf_fc_request *fc_req, bool send_abts,
			   spdk_nvmf_fc_caller_cb cb, void *cb_args);

/*
 * [한국어]
 * nvmf_fc_get_tgt - FC 트랜스포트가 사용하는 nvmf_tgt 포인터 (전역)
 */
struct spdk_nvmf_tgt *nvmf_fc_get_tgt(void);

/*
 * [한국어]
 * nvmf_fc_get_main_thread - FC 트랜스포트의 main thread (이벤트 처리 thread)
 */
struct spdk_thread *nvmf_fc_get_main_thread(void);

/*
 * These functions are called by low level FC driver
 *
 * [한국어] 아래 함수들은 LLD가 직접 호출 (또는 SPDK 내부 hot path).
 */

/*
 * [한국어]
 * nvmf_fc_get_conn - spdk_nvmf_qpair → spdk_nvmf_fc_conn 변환 (container_of)
 *
 * qpair가 fc_conn의 첫 멤버이므로 안전.
 */
static inline struct spdk_nvmf_fc_conn *
nvmf_fc_get_conn(struct spdk_nvmf_qpair *qpair)
{
	return (struct spdk_nvmf_fc_conn *)         /* [한국어] container_of - qpair에서 fc_conn 시작 주소 계산 */
	       ((uintptr_t)qpair - offsetof(struct spdk_nvmf_fc_conn, qpair)); /* [한국어] qpair offset(=0) 빼서 부모 포인터 획득 */
}

/*
 * [한국어]
 * nvmf_fc_advance_conn_sqhead - SQ head 포인터 1 advance (wrap 처리)
 *
 * NVMe SQ는 ring 구조 - 호스트가 보낸 명령 1개를 소비할 때마다 head 진행.
 * 응답 캡슐의 sqhd 필드에 채움 → 호스트의 SQ 흐름 제어.
 */
static inline uint16_t
nvmf_fc_advance_conn_sqhead(struct spdk_nvmf_qpair *qpair)
{
	/* advance sq_head pointer - wrap if needed */
	qpair->sq_head = (qpair->sq_head == qpair->sq_head_max) ? /* [한국어] 끝 도달 시 0으로 wrap, 아니면 +1 */
			 0 : (qpair->sq_head + 1);
	return qpair->sq_head;                      /* [한국어] 새 sq_head 반환 - 응답 캡슐에 사용 */
}

/*
 * [한국어]
 * nvmf_fc_use_send_frame - 이 요청이 send_frame 최적화 경로 사용해야 하는지
 *
 * send_frame은 단일 frame 응답 빠른 경로 - 현재 keep-alive에만 사용.
 * app_id가 있으면 (어플리케이션 추적) 일반 경로로.
 */
static inline bool
nvmf_fc_use_send_frame(struct spdk_nvmf_fc_request *fc_req)
{
	struct spdk_nvmf_request *req = &fc_req->req; /* [한국어] 임베드된 nvmf_request 참조 */

	if (fc_req->app_id) {                       /* [한국어] application id가 있으면 추적용 정규 경로 사용 */
		return false;
	}

	/* For now use for only keepalives. */
	if (req->qpair->qid == 0 &&                 /* [한국어] admin queue + Keep Alive 조합만 send_frame 최적화 */
	    (req->cmd->nvme_cmd.opc == SPDK_NVME_OPC_KEEP_ALIVE)) { /* [한국어] NVMe Keep Alive opcode (0x18) */
		return true;
	}
	return false;                               /* [한국어] 그 외 모든 경우는 정규 경로 */
}

/*
 * [한국어]
 * nvmf_fc_poller_api_func - hwqp poller에 cross-thread API 호출
 *
 * @hwqp: 대상 hwqp
 * @api: 호출할 API 종류 (enum)
 * @api_args: API별 args 구조체 포인터
 * @return: poller_api_ret 코드
 *
 * 다른 thread가 hwqp 작업을 요청할 때 사용 - 내부적으로 hwqp PG thread로 send_msg 후 처리.
 */
enum spdk_nvmf_fc_poller_api_ret nvmf_fc_poller_api_func(
	struct spdk_nvmf_fc_hwqp *hwqp,
	enum spdk_nvmf_fc_poller_api api,
	void *api_args);

/*
 * [한국어]
 * nvmf_fc_hwqp_process_frame - 일반 FCP frame 처리 (LLD가 호출)
 *
 * @hwqp: frame이 도착한 hwqp
 * @buff_idx: RQ 버퍼 인덱스
 * @frame: FC frame header
 * @buffer: 페이로드 버퍼
 * @plen: 페이로드 길이
 *
 * LLD가 frame을 수신하면 SPDK에 위임 - conn lookup → fc_request alloc → 명령 디스패치.
 */
int nvmf_fc_hwqp_process_frame(struct spdk_nvmf_fc_hwqp *hwqp, uint32_t buff_idx,
			       struct spdk_nvmf_fc_frame_hdr *frame,
			       struct spdk_nvmf_fc_buffer_desc *buffer, uint32_t plen);

/*
 * [한국어]
 * nvmf_fc_hwqp_process_pending_reqs - 자원 부족으로 대기 중이던 요청들 재처리
 *
 * 자원 회수 후 호출되어 in_use_reqs/pending에서 retry.
 */
void nvmf_fc_hwqp_process_pending_reqs(struct spdk_nvmf_fc_hwqp *hwqp);

/*
 * [한국어]
 * nvmf_fc_hwqp_process_pending_ls_rqsts - hwqp->ls_pending_queue의 LS 요청 재처리
 */
void nvmf_fc_hwqp_process_pending_ls_rqsts(struct spdk_nvmf_fc_hwqp *hwqp);

/*
 * [한국어]
 * nvmf_fc_request_set_state - fc_request 상태 전이 (디버그 추적 포함)
 */
void nvmf_fc_request_set_state(struct spdk_nvmf_fc_request *fc_req,
			       enum spdk_nvmf_fc_request_state state);

/*
 * [한국어]
 * nvmf_fc_request_get_state_str - 상태 enum을 사람이 읽을 수 있는 문자열로 변환 (디버그용)
 */
char *nvmf_fc_request_get_state_str(int state);

/*
 * [한국어]
 * _nvmf_fc_request_free - fc_request 해제 (내부용 - underscore prefix 주의)
 *
 * conn->pool_queue로 반환, in_use_reqs에서 제거.
 */
void _nvmf_fc_request_free(struct spdk_nvmf_fc_request *fc_req);

/*
 * [한국어]
 * nvmf_fc_request_abort_complete - abort 완료 처리 콜백 (poller_api에서 호출)
 *
 * @arg1: fc_request 포인터
 */
void nvmf_fc_request_abort_complete(void *arg1);

/*
 * [한국어]
 * nvmf_fc_send_ersp_required - 응답을 ERSP IU로 송신해야 하는지 결정
 *
 * @rsp_cnt: 마지막 ERSP 이후 송신한 응답 수
 * @xfer_len: 이번 응답의 데이터 길이
 *
 * esrp_ratio 도달, 데이터 길이 mismatch 등 ERSP 강제 조건 검사.
 */
bool nvmf_fc_send_ersp_required(struct spdk_nvmf_fc_request *fc_req,
				uint32_t rsp_cnt, uint32_t xfer_len);

/*
 * [한국어]
 * nvmf_fc_handle_rsp - 응답 송신 처리 (FCP_RSP 또는 ERSP IU)
 *
 * @return: 0 성공, 음수 에러
 */
int nvmf_fc_handle_rsp(struct spdk_nvmf_fc_request *req);

/*
 * [한국어]
 * nvmf_fc_create_conn_reqpool - connection 별 fc_request 풀 생성 (max_queue_depth 크기)
 */
int nvmf_fc_create_conn_reqpool(struct spdk_nvmf_fc_conn *fc_conn);

/*
 * [한국어]
 * nvmf_fc_free_conn_reqpool - connection 별 풀 해제
 */
void nvmf_fc_free_conn_reqpool(struct spdk_nvmf_fc_conn *fc_conn);

#endif                                              /* [한국어] 헤더 가드 종료 */
