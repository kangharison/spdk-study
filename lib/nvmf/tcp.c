/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019, 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] NVMe-oF TCP transport 구현 (tcp.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK NVMe-over-Fabrics target에서 "TCP" transport를 구현한다.
 * NVMe-oF는 RDMA, FC, TCP 등 여러 transport를 지원하며, TCP는 가장 보편적인
 * 일반 이더넷 환경용이다. 본 파일은 NVMe Transport Specification: TCP
 * (NVMe-TCP, NVM Express TCP Transport Specification 1.0a) 와이어 프로토콜을
 * SPDK target 측에서 구현한다:
 *   - PDU(Protocol Data Unit) 송수신: ICReq/ICResp, CapsuleCmd, CapsuleResp,
 *     H2C Data, C2H Data, R2T(Ready to Transfer), TermReq 등
 *   - HDgst(Header Digest)/DDgst(Data Digest) CRC32C 계산
 *   - per-CPU poll group 기반의 lockless 소켓 풀링 (spdk_sock_group)
 *   - in-capsule data, R2T 기반 호스트→컨트롤러 데이터 전송
 *   - C2H Data PDU로 컨트롤러→호스트 데이터 반환
 *   - TLS PSK 인증 및 keyring 통합 (nvme-tcp의 TLS 1.3 옵션)
 *   - zero-copy 옵션 (socket의 zerocopy_send_msg, bdev의 zcopy_start/end)
 *
 * NVMe-TCP의 주요 흐름은 다음과 같다:
 *   호스트 → CapsuleCmd PDU (NVMe SQE 포함, 작은 데이터는 in-capsule)
 *   target → R2T PDU (큰 데이터의 경우 호스트에 데이터 전송 요청)
 *   호스트 → H2C Data PDU (R2T 응답으로 데이터 전송)
 *   target → bdev에 IO 디스패치
 *   target → C2H Data PDU (read의 경우 데이터 반환)
 *   target → CapsuleResp PDU (NVMe CQE 포함)
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe-oF target 계층:
 *   spdk_nvmf_tgt
 *      └── spdk_nvmf_subsystem (subsystem.c)
 *      └── spdk_nvmf_transport (transport.c) ─ ops 디스패치
 *            └── spdk_nvmf_tcp_transport (본 파일) ─ TCP 전용 구현
 *                  └── spdk_nvmf_tcp_port ─ listening 소켓
 *                  └── spdk_nvmf_tcp_qpair ─ 호스트당 1개 (또는 admin/IO qpair)
 *                        └── spdk_sock ─ TCP 소켓 (sock 추상화)
 *                        └── nvme_tcp_pdu (송신/수신 큐)
 *                  └── spdk_nvmf_tcp_poll_group (per-CPU)
 *                        └── spdk_sock_group ─ epoll/io_uring/select
 *
 * 실행 컨텍스트:
 *   - 모든 데이터 plane 함수(nvmf_tcp_qpair_recv_pdu, nvmf_tcp_req_process,
 *     nvmf_tcp_sock_cb 등)는 해당 qpair가 속한 poll group의 thread에서 실행
 *   - control plane 함수(nvmf_tcp_create, nvmf_tcp_listen 등)는 SPDK init
 *     thread 또는 subsystem->thread에서 호출
 *   - poll_group은 각 reactor에 1개씩 배치되어 lockless polling
 *
 * === 타 모듈과의 연결 ===
 * - lib/sock: spdk_sock_*** API로 epoll/uring/posix 소켓 추상화 사용 (sock.c)
 *   소켓 콜백(nvmf_tcp_sock_cb)이 데이터 도착 이벤트의 진입점
 * - lib/nvmf/transport.c: spdk_nvmf_transport_ops 인터페이스 등록
 *   (spdk_nvmf_transport_tcp ops 테이블)
 * - lib/nvmf/ctrlr.c: 받은 NVMe 명령을 nvmf_request_exec로 위임
 *   완료 콜백 nvmf_tcp_req_complete가 ctrlr에서 호출됨
 * - lib/nvmf/subsystem.c: subsystem이 add_listener할 때 TCP listener 생성
 * - spdk_internal/nvme_tcp.h: PDU 헤더/디지스트 등 와이어 포맷 헬퍼
 * - spdk/accel: HDgst/DDgst CRC32C 계산 가속 (DSA 등)
 * - spdk/keyring: TLS PSK key 관리 (TLS 1.3 nvme-tcp)
 *
 * === 주요 함수/구조체 요약 ===
 * 핵심 진입 함수:
 *   - nvmf_tcp_sock_cb: 소켓에서 데이터 도착 시 호출되는 진입점
 *     → nvmf_tcp_sock_process → nvmf_tcp_qpair_process_recv_pdu_chain
 *   - nvmf_tcp_req_process: TCP 요청 객체의 상태 머신 진행 함수 (15+ 상태)
 *   - nvmf_tcp_qpair_recv_pdu: 와이어에서 PDU 한 개 읽고 핸들러 디스패치
 *   - nvmf_tcp_send_capsule_resp_pdu: NVMe CQE를 캡슐 응답 PDU로 송신
 *   - _nvmf_tcp_send_c2h_data: C2H Data PDU(read 응답) 송신
 *   - nvmf_tcp_h2c_data_payload_handle / nvmf_tcp_capsule_cmd_payload_handle
 *
 * Transport ops:
 *   - nvmf_tcp_create / destroy: transport 객체 라이프사이클
 *   - nvmf_tcp_listen / stop_listen: TCP 포트 listen/close
 *   - nvmf_tcp_accept: 새 connection accept
 *   - nvmf_tcp_poll_group_create / poll: per-CPU poll group
 *
 * 핵심 자료구조:
 *   - spdk_nvmf_tcp_transport: TCP transport 인스턴스 (전역)
 *   - spdk_nvmf_tcp_port:     listening port (trid 기반)
 *   - spdk_nvmf_tcp_qpair:    호스트와의 TCP qpair 연결
 *   - spdk_nvmf_tcp_req:      개별 NVMe 요청의 라이프사이클
 *   - spdk_nvmf_tcp_poll_group: per-CPU poll group
 *   - spdk_nvmf_tcp_req_state: 요청 상태 머신 (FREE→NEW→...→COMPLETED)
 */

#include "spdk/accel.h"				/* [한국어] HW 가속 (DSA/IAA) — CRC32C/memcopy 가속에 사용 */
#include "spdk/stdinc.h"			/* [한국어] 표준 C 헤더 모음 */
#include "spdk/crc32.h"				/* [한국어] CRC32C 계산 (HDgst/DDgst) */
#include "spdk/endian.h"			/* [한국어] from/to_be16/32 등 endian 변환 (NVMe-TCP wire format) */
#include "spdk/assert.h"			/* [한국어] SPDK_STATIC_ASSERT */
#include "spdk/thread.h"			/* [한국어] spdk_thread, spdk_poller, message */
#include "spdk/nvmf_transport.h"		/* [한국어] transport 등록 인터페이스 (transport_ops) */
#include "spdk/string.h"			/* [한국어] spdk_strerror */
#include "spdk/trace.h"				/* [한국어] trace point 등록 */
#include "spdk/util.h"				/* [한국어] SPDK_CONTAINEROF 등 매크로 */
#include "spdk/log.h"				/* [한국어] SPDK_*LOG 매크로 */
#include "spdk/keyring.h"			/* [한국어] TLS PSK key 관리 */
#include "spdk/sock.h"				/* [한국어] 소켓 추상화 (epoll/uring/posix) */

#include "spdk_internal/assert.h"		/* [한국어] SPDK_UNREACHABLE 등 */
#include "spdk_internal/nvme_tcp.h"		/* [한국어] NVMe-TCP PDU 정의 (host/target 공통) */

#include "nvmf_internal.h"			/* [한국어] subsystem/ctrlr 내부 정의 */
#include "transport.h"				/* [한국어] transport 내부 인터페이스 */

#include "spdk_internal/trace_defs.h"		/* [한국어] TRACE_TCP_xxx 등 trace ID 정의 */

#define MIN_SOCK_PIPE_SIZE 1024
/* [한국어] 소켓 pipe buffer 최소 크기 (Linux F_SETPIPE_SZ 제한) */

#define NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME 16
/* [한국어] 한 accept poller 호출에서 한 번에 accept 가능한 최대 신규 connection 수.
 * 하나의 listener에 다수의 호스트가 동시 connect 시도해도 polling fairness 유지. */

#define SPDK_NVMF_TCP_DEFAULT_MAX_SOCK_PRIORITY 16
/* [한국어] Linux SO_PRIORITY 최댓값 (qdisc 우선순위 큐 활용 시) */

#define SPDK_NVMF_TCP_DEFAULT_SOCK_PRIORITY 0
/* [한국어] 디폴트 소켓 우선순위 (best-effort) */

#define SPDK_NVMF_TCP_DEFAULT_CONTROL_MSG_NUM 32
/* [한국어] in-capsule data 한계 초과 명령에 대한 H2C 메시지용 사전 할당 버퍼 수 */

#define SPDK_NVMF_TCP_DEFAULT_SUCCESS_OPTIMIZATION true
/* [한국어] in-capsule write 등에서 success status가 자명할 때 별도 응답 PDU 생략 (와이어 절약) */

#define SPDK_NVMF_TCP_MIN_IO_QUEUE_DEPTH 2
#define SPDK_NVMF_TCP_MAX_IO_QUEUE_DEPTH 65535
#define SPDK_NVMF_TCP_MIN_ADMIN_QUEUE_DEPTH 2
#define SPDK_NVMF_TCP_MAX_ADMIN_QUEUE_DEPTH 4096
/* [한국어] IO/Admin qpair queue depth 허용 범위 (NVMe 스펙: SQE 개수 단위).
 * 호스트 connect 시 nego 후 min..max 범위 내에서 결정된다. */

#define SPDK_NVMF_TCP_DEFAULT_MAX_IO_QUEUE_DEPTH 128		/* [한국어] IO QD 디폴트 */
#define SPDK_NVMF_TCP_DEFAULT_MAX_ADMIN_QUEUE_DEPTH 128		/* [한국어] Admin QD 디폴트 */
#define SPDK_NVMF_TCP_DEFAULT_MAX_QPAIRS_PER_CTRLR 128		/* [한국어] ctrlr당 qpair 최대 개수 (admin 1 + IO N) */
#define SPDK_NVMF_TCP_DEFAULT_IN_CAPSULE_DATA_SIZE 4096		/* [한국어] CapsuleCmd PDU에 inline 첨부할 수 있는 데이터 최대 크기 (4KB) */
#define SPDK_NVMF_TCP_DEFAULT_MAX_IO_SIZE 131072		/* [한국어] 단일 IO 요청 최대 크기 (128KB) */
#define SPDK_NVMF_TCP_DEFAULT_IO_UNIT_SIZE 131072		/* [한국어] 디폴트 IO unit (전송 단위) */
#define SPDK_NVMF_TCP_DEFAULT_NUM_SHARED_BUFFERS 511		/* [한국어] transport-wide 공유 데이터 버퍼 풀 크기 */
#define SPDK_NVMF_TCP_DEFAULT_BUFFER_CACHE_SIZE UINT32_MAX	/* [한국어] poll group 별 버퍼 캐시 크기 (UINT32_MAX=무제한) */
#define SPDK_NVMF_TCP_DEFAULT_DIF_INSERT_OR_STRIP false		/* [한국어] DIF 자동 삽입/제거 비활성 */
#define SPDK_NVMF_TCP_DEFAULT_ABORT_TIMEOUT_SEC 1		/* [한국어] abort 명령 타임아웃 (초) */

/* [한국어] transport_ops 테이블의 forward declaration. 파일 끝에서 정의되며,
 * 본 파일 내부 함수들이 ops로 등록된다. spdk_nvmf_transport_create_async에서
 * "TCP" trtype에 대해 이 ops를 lookup. */
const struct spdk_nvmf_transport_ops spdk_nvmf_transport_tcp;

/* [한국어] TLS 디버그 로깅을 한 번만 출력하기 위한 플래그.
 * TLS 옵션 사용 시 "enabled" 한 번만 보이도록 첫 호출 후 true. */
static bool g_tls_log = false;

/* spdk nvmf related structure */
/* [한국어] TCP NVMe-oF 요청의 라이프사이클 상태 머신.
 * 호스트가 보낸 한 NVMe 명령은 PDU 수신부터 응답 전송 완료까지 다음 상태들을
 * 거친다. nvmf_tcp_req_process()의 거대한 switch가 이 머신의 본체이며,
 * 각 상태에서 비동기 자원 대기(버퍼/zcopy/bdev 실행/소켓 송신) 후 콜백으로
 * 다음 상태로 진행한다. trace point도 모든 상태 전이를 기록.
 *
 * 일반적인 write 명령 진행:
 *   FREE → NEW → NEED_BUFFER → HAVE_BUFFER → TRANSFERRING_HOST_TO_CONTROLLER
 *     → READY_TO_EXECUTE → EXECUTING → EXECUTED → READY_TO_COMPLETE
 *     → TRANSFERRING_CONTROLLER_TO_HOST → COMPLETED → FREE
 *
 * read 명령은 H2C 단계 생략, R2T 사용 안 함.
 * zcopy 활성화 시 AWAITING_ZCOPY_START/COMPLETED, AWAITING_ZCOPY_COMMIT,
 * AWAITING_ZCOPY_RELEASE 분기 추가. */
enum spdk_nvmf_tcp_req_state {

	/* The request is not currently in use */
	TCP_REQUEST_STATE_FREE = 0,
	/* [한국어] req 객체가 풀에 반환된 미사용 상태. STAILQ로 free list 유지.
	 * 설정자: nvmf_tcp_request_free / 초기화. 읽는 자: 신규 PDU 도착 시 alloc 시도. */

	/* Initial state when request first received */
	TCP_REQUEST_STATE_NEW = 1,
	/* [한국어] CapsuleCmd PDU 헤더만 수신된 직후 진입 상태.
	 * 설정자: nvmf_tcp_req_get / process_capsule_cmd. 다음: NEED_BUFFER. */

	/* The request is queued until a data buffer is available. */
	TCP_REQUEST_STATE_NEED_BUFFER = 2,
	/* [한국어] 데이터 전송용 IO 버퍼를 대기. transport buf_cache가 비면
	 * STAILQ_INSERT_TAIL(group->awaiting_buf)로 큐잉되어 대기. */

	/* The request has the data buffer available */
	TCP_REQUEST_STATE_HAVE_BUFFER = 3,
	/* [한국어] 버퍼 할당 완료. write의 경우 H2C/in-capsule 데이터 수신 단계로 전이. */

	/* The request is waiting for zcopy_start to finish */
	TCP_REQUEST_STATE_AWAITING_ZCOPY_START = 4,
	/* [한국어] zero-copy: bdev->zcopy_start 콜백 대기 (bdev가 직접 가리키는 메모리 획득). */

	/* The request has received a zero-copy buffer */
	TCP_REQUEST_STATE_ZCOPY_START_COMPLETED = 5,
	/* [한국어] zcopy_start 완료 — bdev로부터 받은 IOV로 데이터 plane 진행. */

	/* The request is currently transferring data from the host to the controller. */
	TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER = 6,
	/* [한국어] H2C Data PDU 또는 in-capsule data를 수신 중. write 경로에서만 사용. */

	/* The request is waiting for the R2T send acknowledgement. */
	TCP_REQUEST_STATE_AWAITING_R2T_ACK = 7,
	/* [한국어] R2T PDU를 보냈고 호스트의 H2C Data 응답을 대기 중 (ddgst_enable 시 ack 별도 검증). */

	/* The request is ready to execute at the block device */
	TCP_REQUEST_STATE_READY_TO_EXECUTE = 8,
	/* [한국어] 모든 데이터/헤더 준비 완료. 다음 단계에서 spdk_nvmf_request_exec 호출. */

	/* The request is currently executing at the block device */
	TCP_REQUEST_STATE_EXECUTING = 9,
	/* [한국어] bdev 레이어에 IO 디스패치 후 완료 콜백 대기. */

	/* The request is waiting for zcopy buffers to be committed */
	TCP_REQUEST_STATE_AWAITING_ZCOPY_COMMIT = 10,
	/* [한국어] zcopy 모드: bdev->zcopy_end(commit=true) 결과 대기 (write 데이터 영구화). */

	/* The request finished executing at the block device */
	TCP_REQUEST_STATE_EXECUTED = 11,
	/* [한국어] bdev 실행 완료 (status 확보). C2H Data PDU 송신 또는 응답 송신 단계 진입. */

	/* The request is ready to send a completion */
	TCP_REQUEST_STATE_READY_TO_COMPLETE = 12,
	/* [한국어] CapsuleResp(NVMe CQE) PDU 송신 직전. */

	/* The request is currently transferring final pdus from the controller to the host. */
	TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST = 13,
	/* [한국어] read 응답 C2H Data PDU 송신 중 또는 그 직전. */

	/* The request is waiting for zcopy buffers to be released (without committing) */
	TCP_REQUEST_STATE_AWAITING_ZCOPY_RELEASE = 14,
	/* [한국어] zcopy 모드: bdev->zcopy_end(commit=false) 대기 (read 후 buffer release). */

	/* The request completed and can be marked free. */
	TCP_REQUEST_STATE_COMPLETED = 15,
	/* [한국어] 모든 단계 종료. tcp_request_free로 가서 free list에 반환. */

	/* Terminator */
	TCP_REQUEST_NUM_STATES,
	/* [한국어] 상태 개수 (배열 크기/통계 카운터에 활용). 실제 상태 아님. */
};

/* [한국어] qpair 라이프사이클 상태.
 * 호스트와 TCP 연결되는 단위. admin/IO 분리 — admin qpair 1개 + IO qpair N개. */
enum nvmf_tcp_qpair_state {
	NVMF_TCP_QPAIR_STATE_INVALID = 0,
	/* [한국어] 초기화 전/이상 상태. 정상 흐름에서 머무르지 않음. */
	NVMF_TCP_QPAIR_STATE_INITIALIZING = 1,
	/* [한국어] accept 직후, ICReq/ICResp PDU 교환 중 (초기 capsule 시작 전). */
	NVMF_TCP_QPAIR_STATE_RUNNING = 2,
	/* [한국어] 정상 IO 처리 가능 상태. CapsuleCmd 수신 시작. */
	NVMF_TCP_QPAIR_STATE_EXITING = 3,
	/* [한국어] disconnect 진행 중. 진행 중인 req 정리, send queue flush. */
	NVMF_TCP_QPAIR_STATE_EXITED = 4,
	/* [한국어] 모든 정리 완료. ctrlr에게 destroy 통지 후 free 단계. */
};

static const char *spdk_nvmf_tcp_term_req_fes_str[] = {
	"Invalid PDU Header Field",
	"PDU Sequence Error",
	"Header Digiest Error",
	"Data Transfer Out of Range",
	"R2T Limit Exceeded",
	"Unsupported parameter",
};

static void
nvmf_tcp_trace(void)
{
	spdk_trace_register_owner_type(OWNER_TYPE_NVMF_TCP, 't');
	spdk_trace_register_object(OBJECT_NVMF_TCP_IO, 'r');
	spdk_trace_register_description("TCP_REQ_NEW",
					TRACE_TCP_REQUEST_STATE_NEW,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 1,
					SPDK_TRACE_ARG_TYPE_INT, "qd");
	spdk_trace_register_description("TCP_REQ_NEED_BUFFER",
					TRACE_TCP_REQUEST_STATE_NEED_BUFFER,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_HAVE_BUFFER",
					TRACE_TCP_REQUEST_STATE_HAVE_BUFFER,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_WAIT_ZCPY_START",
					TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_START,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_ZCPY_START_CPL",
					TRACE_TCP_REQUEST_STATE_ZCOPY_START_COMPLETED,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_TX_H_TO_C",
					TRACE_TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_RDY_TO_EXECUTE",
					TRACE_TCP_REQUEST_STATE_READY_TO_EXECUTE,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_EXECUTING",
					TRACE_TCP_REQUEST_STATE_EXECUTING,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_WAIT_ZCPY_CMT",
					TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_COMMIT,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_EXECUTED",
					TRACE_TCP_REQUEST_STATE_EXECUTED,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_RDY_TO_COMPLETE",
					TRACE_TCP_REQUEST_STATE_READY_TO_COMPLETE,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_TRANSFER_C2H",
					TRACE_TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_AWAIT_ZCPY_RLS",
					TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_RELEASE,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_COMPLETED",
					TRACE_TCP_REQUEST_STATE_COMPLETED,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "qd");
	spdk_trace_register_description("TCP_READ_DONE",
					TRACE_TCP_READ_FROM_SOCKET_DONE,
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_AWAIT_R2T_ACK",
					TRACE_TCP_REQUEST_STATE_AWAIT_R2T_ACK,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");

	spdk_trace_register_description("TCP_QP_CREATE", TRACE_TCP_QP_CREATE,
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_SOCK_INIT", TRACE_TCP_QP_SOCK_INIT,
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_STATE_CHANGE", TRACE_TCP_QP_STATE_CHANGE,
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "state");
	spdk_trace_register_description("TCP_QP_DISCONNECT", TRACE_TCP_QP_DISCONNECT,
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_DESTROY", TRACE_TCP_QP_DESTROY,
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_ABORT_REQ", TRACE_TCP_QP_ABORT_REQ,
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_RCV_STATE_CHANGE", TRACE_TCP_QP_RCV_STATE_CHANGE,
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "state");

	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_START, OBJECT_NVMF_TCP_IO, 1);
	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_DONE, OBJECT_NVMF_TCP_IO, 0);
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_QUEUE, OBJECT_NVMF_TCP_IO, 0);
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_PEND, OBJECT_NVMF_TCP_IO, 0);
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_COMPLETE, OBJECT_NVMF_TCP_IO, 0);
}
SPDK_TRACE_REGISTER_FN(nvmf_tcp_trace, "nvmf_tcp", TRACE_GROUP_NVMF_TCP)

/* [한국어] TCP transport에서 사용하는 1개 NVMe 요청의 컨테이너.
 * qpair 생성 시 reqs[resource_count]로 사전 할당되어 free_queue에서 재활용된다. */
struct spdk_nvmf_tcp_req  {
	struct spdk_nvmf_request		req;
	/* [한국어] 공통 nvmf request 구조 (transport-agnostic). cmd/rsp 포인터, IOV, length 등 보유.
	 * 설정자: nvmf_tcp_req_get에서 cmd 포인터 등 초기화. ctrlr.c에서 nvmf_request_exec 시 참조.
	 * 동기화: 한 req는 한 qpair의 단일 thread에서만 다뤄짐. */

	struct spdk_nvme_cpl			rsp;
	/* [한국어] 이 요청의 NVMe Completion Queue Entry (CQE) 저장소.
	 * 16바이트 NVMe 응답 — status/cid 등. CapsuleResp PDU의 payload로 송신됨.
	 * 설정자: bdev 완료 콜백 또는 ctrlr 처리 후 status field. */

	struct spdk_nvme_cmd			cmd;
	/* [한국어] 이 요청의 NVMe Submission Queue Entry (SQE) 사본.
	 * 64바이트 NVMe 명령 — opcode/nsid/cdw 등. CapsuleCmd PDU 헤더에서 복사.
	 * 호스트 측 cmd 메모리는 PDU lifecycle을 따르므로 본 사본을 유지. */

	/* A PDU that can be used for sending responses. This is
	 * not the incoming PDU! */
	struct nvme_tcp_pdu			*pdu;
	/* [한국어] 이 요청 전용으로 할당된 송신용 PDU 슬롯.
	 * R2T, C2H Data, CapsuleResp 등 여러 응답 PDU를 직렬로 재사용한다.
	 * 수신 PDU(pdu_in_progress)와는 별개. */

	/* In-capsule data buffer */
	uint8_t					*buf;
	/* [한국어] in-capsule data용 사전 할당 버퍼 (in_capsule_data_size 바이트).
	 * 작은 write의 경우 호스트가 CapsuleCmd PDU에 데이터를 inline 포함하는데,
	 * 그 데이터를 받아둘 곳. qpair가 bufs[] 배열에서 분할해 부여. */

	struct spdk_nvmf_tcp_req		*fused_pair;
	/* [한국어] NVMe Fused Operation(2개 명령을 atomic 쌍으로 처리)의 짝 req 포인터.
	 * 예: COMPARE-AND-WRITE 등. NULL이면 fused 아님. */

	/*
	 * The PDU for a request may be used multiple times in serial over
	 * the request's lifetime. For example, first to send an R2T, then
	 * to send a completion. To catch mistakes where the PDU is used
	 * twice at the same time, add a debug flag here for init/fini.
	 */
	bool					pdu_in_use;
	/* [한국어] 본 req의 pdu가 현재 송신 중(in-flight)인지 여부 — 디버그용.
	 * true 상태에서 다시 PDU 빌드 요청이 오면 assert로 잡는다. */
	bool					has_in_capsule_data;
	/* [한국어] CapsuleCmd에 in-capsule data가 동봉되어 있는지. true이면 H2C/R2T 생략. */
	bool					fused_failed;
	/* [한국어] fused pair 처리 중 실패가 발생했는지. 짝 req에 status 전파용. */

	/* transfer_tag */
	uint16_t				ttag;
	/* [한국어] R2T/H2C Data PDU에서 호스트가 echo하는 transfer tag.
	 * target이 부여하며, 호스트는 H2C Data PDU에 동일 ttag로 응답해야 한다.
	 * 동시 outstanding R2T를 구분하는 키. */

	enum spdk_nvmf_tcp_req_state		state;
	/* [한국어] 현재 요청 상태 (FREE..COMPLETED). nvmf_tcp_req_set_state로 전이.
	 * trace point에 기록되어 perf script로 lifecycle 관측 가능. */

	/*
	 * h2c_offset is used when we receive the h2c_data PDU.
	 */
	uint32_t				h2c_offset;
	/* [한국어] write 시 H2C Data로 누적 수신된 바이트 오프셋.
	 * 한 R2T에 대해 여러 H2C Data PDU가 쪼개져 올 수 있어 누적 추적 필요.
	 * total req length와 같아질 때 데이터 수신 완료. */

	STAILQ_ENTRY(spdk_nvmf_tcp_req)		link;
	/* [한국어] free_queue에 매다는 단방향 리스트 링크. tcp_req_free_queue에서 재활용. */
	TAILQ_ENTRY(spdk_nvmf_tcp_req)		state_link;
	/* [한국어] working_queue 또는 동일 상태 큐에 매다는 양방향 링크. */
	STAILQ_ENTRY(spdk_nvmf_tcp_req)		control_msg_link;
	/* [한국어] control message 버퍼 대기 큐에 매다는 링크. */
};

/* [한국어] TCP qpair: 호스트와의 1개 TCP connection = 1개 NVMe queue pair.
 * Admin qpair 1개 + IO qpair N개로 ctrlr 구성. 모든 PDU 송수신/요청 상태가
 * 본 구조에서 관리되며, 단일 thread (poll_group의 thread)에서만 다뤄짐. */
struct spdk_nvmf_tcp_qpair {
	struct spdk_nvmf_qpair			qpair;
	/* [한국어] 공통 nvmf qpair 구조 (transport-agnostic). state, ctrlr 포인터, sq_count 등.
	 * SPDK_CONTAINEROF로 spdk_nvmf_qpair → spdk_nvmf_tcp_qpair 다운캐스트. */

	struct spdk_nvmf_tcp_poll_group		*group;
	/* [한국어] 이 qpair가 등록된 per-CPU poll group. 모든 처리가 이 poll group의 thread에서. */

	struct spdk_sock			*sock;
	/* [한국어] 이 qpair의 TCP 소켓 추상화. spdk_sock_readv/writev/group_add 등 호출 대상. */

	enum nvme_tcp_pdu_recv_state		recv_state;
	/* [한국어] PDU 수신 상태 머신 (HEADER → PSH → PAYLOAD → READY). 헤더와 payload를 분리 수신. */

	enum nvmf_tcp_qpair_state		state;
	/* [한국어] qpair 라이프사이클 상태 (INVALID/INITIALIZING/RUNNING/EXITING/EXITED). */

	/* PDU being actively received */
	struct nvme_tcp_pdu			*pdu_in_progress;
	/* [한국어] 현재 와이어에서 수신 중인 PDU. 헤더 + 데이터가 모두 모이면 핸들러 디스패치. */

	struct spdk_nvmf_tcp_req		*fused_first;
	/* [한국어] fused 명령 쌍의 첫 번째 명령 보관 — 두 번째가 도착하면 짝지어 처리. */

	/* Queues to track the requests in all states */
	TAILQ_HEAD(, spdk_nvmf_tcp_req)		tcp_req_working_queue;
	/* [한국어] 활성 처리 중인 req 리스트 (FREE 아닌 모든 상태). */
	TAILQ_HEAD(, spdk_nvmf_tcp_req)		tcp_req_free_queue;
	/* [한국어] 재할당 대기 중인 free req 리스트. nvmf_tcp_req_get에서 dequeue. */
	SLIST_HEAD(, nvme_tcp_pdu)		tcp_pdu_free_queue;
	/* [한국어] free PDU 리스트 (R2T/Data PDU 송신용 슬롯 풀). */
	/* Number of working pdus */
	uint32_t				tcp_pdu_working_count;
	/* [한국어] 동시 in-flight PDU 수 — 흐름 제어용 카운터. */

	/* Number of requests in each state */
	uint32_t				state_cntr[TCP_REQUEST_NUM_STATES];
	/* [한국어] 상태별 req 수 카운터 — 통계/디버그/QD 추적용. set_state에서 갱신. */

	uint8_t					cpda;
	/* [한국어] Controller PDU Data Alignment (NVMe-TCP 스펙) — 데이터 padding 정렬값.
	 * ICReq/ICResp에서 협상된 호스트 요구치를 따른다. */

	bool					host_hdgst_enable;
	/* [한국어] 호스트가 PDU Header CRC32C 디지스트를 사용하는지 (ICReq에서 협상). */
	bool					host_ddgst_enable;
	/* [한국어] 호스트가 Data CRC32C 디지스트를 사용하는지 (PDU 끝에 4-byte 추가). */

	bool					await_req_msg_pending;
	/* [한국어] poll loop에 await_buf 처리 메시지가 enqueue되어 있는지 — 중복 enqueue 방지. */

	/* This is a spare PDU used for sending special management
	 * operations. Primarily, this is used for the initial
	 * connection response and c2h termination request. */
	struct nvme_tcp_pdu			*mgmt_pdu;
	/* [한국어] req 풀과 별도의 관리용 PDU 슬롯 1개. ICResp, C2HTermReq 등 송신에 전용 사용. */

	/* Arrays of in-capsule buffers, requests, and pdus.
	 * Each array is 'resource_count' number of elements */
	void					*bufs;
	/* [한국어] in-capsule data 버퍼들의 연속 메모리 — resource_count * in_capsule_size. */
	struct spdk_nvmf_tcp_req		*reqs;
	/* [한국어] req 객체 배열 — qpair 생성 시 사전 할당, free_queue 초기 채움. */
	struct nvme_tcp_pdu			*pdus;
	/* [한국어] PDU 객체 배열 — req 1개당 1개 + mgmt_pdu 1개. */
	uint32_t				resource_count;
	/* [한국어] qpair에 사전 할당된 req/pdu 개수 = qpair queue depth. */
	uint32_t				recv_buf_size;
	/* [한국어] 소켓 SO_RCVBUF 크기 (커널 수신 버퍼) — 처리량 영향. */

	struct spdk_nvmf_tcp_port		*port;
	/* [한국어] 이 qpair가 accept된 listening port 역참조 (transport_id 조회 등에 사용). */

	/* IP address */
	char					initiator_addr[SPDK_NVMF_TRADDR_MAX_LEN];
	/* [한국어] 호스트(initiator) IP 주소 문자열 — 로깅/discovery에 사용. */
	char					target_addr[SPDK_NVMF_TRADDR_MAX_LEN];
	/* [한국어] target 측 local IP — 다중 NIC 환경에서 어느 NIC로 받았는지 식별. */

	/* IP port */
	uint16_t				initiator_port;
	/* [한국어] 호스트 측 ephemeral port. */
	uint16_t				target_port;
	/* [한국어] target 측 listen port (예: 4420). */

	/* Wait until the host terminates the connection (e.g. after sending C2HTermReq) */
	bool					wait_terminate;
	/* [한국어] target이 C2H TermReq를 보낸 후 호스트의 close를 기다리는 중. */

	/* Timer used to destroy qpair after detecting transport error issue if initiator does
	 *  not close the connection.
	 */
	struct spdk_poller			*timeout_poller;
	/* [한국어] 호스트 close 대기 타임아웃 poller — 일정 시간 내 close 안 오면 강제 destroy. */

	spdk_nvmf_transport_qpair_fini_cb	fini_cb_fn;
	void					*fini_cb_arg;
	/* [한국어] qpair fini 완료 시 호출될 callback (transport common layer가 등록). */

	TAILQ_ENTRY(spdk_nvmf_tcp_qpair)	link;
	/* [한국어] poll_group->qpairs 리스트에 매다는 링크. */
	bool					pending_flush;
	/* [한국어] 송신 큐에 flush 대기 중인 PDU가 있어 다음 poll에서 즉시 flush 필요. */
};

/* [한국어] in-capsule 한계 초과 명령에 대한 사전 할당 control message 버퍼 헤더.
 * msg_buf의 한 슬롯에 매핑되어 STAILQ로 free/waiting 관리. */
struct spdk_nvmf_tcp_control_msg {
	STAILQ_ENTRY(spdk_nvmf_tcp_control_msg) link;
	/* [한국어] free_msgs 또는 waiting 리스트에 매다는 링크. */
};

/* [한국어] poll group 단위 control message 풀 — 큰 명령용 staging buffer. */
struct spdk_nvmf_tcp_control_msg_list {
	void *msg_buf;
	/* [한국어] 사전 할당된 메모리 블록 (control_msg_num * in_capsule_size). */
	STAILQ_HEAD(, spdk_nvmf_tcp_control_msg) free_msgs;
	/* [한국어] 사용 가능한 control msg 슬롯 리스트. */
	STAILQ_HEAD(, spdk_nvmf_tcp_req) waiting_for_msg_reqs;
	/* [한국어] free 슬롯 부족으로 대기 중인 req 리스트 (FIFO). */
};

/* [한국어] per-CPU poll group: 한 reactor에 1개씩 배치되어 lockless polling. */
struct spdk_nvmf_tcp_poll_group {
	struct spdk_nvmf_transport_poll_group	group;
	/* [한국어] transport 공통 poll group (transport-agnostic). buf_cache, awaiting list 등. */

	struct spdk_sock_group			*sock_group;
	/* [한국어] 본 poll group이 관리하는 소켓 group (epoll/uring 등 1개 인스턴스).
	 * 모든 qpair의 sock이 이 group에 add되며 한 번의 poll로 다중 소켓 처리. */

	struct spdk_interrupt			*intr;
	/* [한국어] interrupt-mode 운영 시 epoll fd에 등록되는 interrupt 핸들 (옵션). */

	TAILQ_HEAD(, spdk_nvmf_tcp_qpair)	qpairs;
	/* [한국어] 본 poll group에 소속된 qpair 리스트. */

	struct spdk_io_channel			*accel_channel;
	/* [한국어] CRC32C 등 가속을 위한 accel framework 채널. NULL이면 SW fallback. */

	struct spdk_nvmf_tcp_control_msg_list	*control_msg_list;
	/* [한국어] 본 poll group의 control message 풀 (NULL이면 비활성). */

	TAILQ_ENTRY(spdk_nvmf_tcp_poll_group)	link;
	/* [한국어] transport->poll_groups 리스트 매기 링크. */
};

/* [한국어] TCP listening port: 한 trid (IP:port)에 1개. listener 1개에 N qpair accept. */
struct spdk_nvmf_tcp_port {
	const struct spdk_nvme_transport_id	*trid;
	/* [한국어] 이 port의 transport id (trtype=TCP, traddr, trsvcid). 변경 불가. */
	struct spdk_sock			*listen_sock;
	/* [한국어] 실제 listen 소켓 — accept poller가 이 소켓에서 새 connection 수락. */
	struct spdk_nvmf_transport		*transport;
	/* [한국어] 부모 transport 역참조 (accept된 qpair에 transport 연결시 사용). */
	TAILQ_ENTRY(spdk_nvmf_tcp_port)		link;
	/* [한국어] transport->ports 리스트에 매다는 링크. */
};

/* [한국어] TCP transport 사용자 옵션 (JSON config로 노출). */
struct tcp_transport_opts {
	bool		c2h_success;
	/* [한국어] in-capsule write 등 success가 자명한 경우 별도 응답 PDU 생략 (NVMe-TCP success optimization). */
	uint16_t	control_msg_num;
	/* [한국어] poll group당 control message 버퍼 슬롯 수 (디폴트 32). */
	uint32_t	sock_priority;
	/* [한국어] 소켓 SO_PRIORITY 값 (Linux qdisc 우선순위). */
};

/* [한국어] TLS 인증을 위한 PSK(Pre-Shared Key) 엔트리.
 * subsystem-host 쌍별로 1개. nvme-tcp의 TLS 1.3 (NVMe TCP 1.0a) 지원에 사용. */
struct tcp_psk_entry {
	char				hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	/* [한국어] 이 PSK가 적용될 호스트 NQN. */
	char				subnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	/* [한국어] 이 PSK가 적용될 subsystem NQN. */
	char				pskid[NVMF_PSK_IDENTITY_LEN];
	/* [한국어] TLS handshake에서 사용되는 PSK identity 문자열. */
	uint8_t				psk[SPDK_TLS_PSK_MAX_LEN];
	/* [한국어] 실제 PSK 바이트 (대칭키). 메모리에서 시간차로 zeroize 필요. */
	struct spdk_key			*key;
	/* [한국어] keyring에 등록된 키 핸들 — psk 메모리는 짧게 유지하고 핸들로 참조. */
	uint32_t			psk_size;
	/* [한국어] psk 실제 길이 (16/32/48/64 등). */
	enum nvme_tcp_cipher_suite	tls_cipher_suite;
	/* [한국어] TLS_AES_128_GCM_SHA256 등 cipher suite (NVMe-TCP 스펙). */
	TAILQ_ENTRY(tcp_psk_entry)	link;
	/* [한국어] transport->psks 리스트에 매다는 링크. */
};

/* [한국어] TCP transport 전역 인스턴스. spdk_nvmf_transport_create_async에서
 * "TCP" trtype으로 1개 생성됨. 모든 port/poll_group/PSK가 본 구조 아래에 매달림. */
struct spdk_nvmf_tcp_transport {
	struct spdk_nvmf_transport		transport;
	/* [한국어] 공통 transport 객체 — opts (max_io_size, num_shared_buffers 등), buf_pool 등 보유. */

	struct tcp_transport_opts               tcp_opts;
	/* [한국어] TCP 전용 옵션 (c2h_success, control_msg_num, sock_priority). */

	uint32_t				ack_timeout;
	/* [한국어] TCP keep-alive ack 타임아웃 (디스커넥트 감지 시간). */

	struct spdk_nvmf_tcp_poll_group		*next_pg;
	/* [한국어] 새 connection의 round-robin 분배를 위한 다음 poll group 포인터. */

	struct spdk_poller			*accept_poller;
	/* [한국어] 모든 listen 소켓을 polling 하는 accept poller — 일정 주기로 nvmf_tcp_accept 호출. */

	struct spdk_sock_group			*listen_sock_group;
	/* [한국어] 모든 listening sock을 묶은 group (epoll/uring으로 한 번에 polling). */

	struct spdk_interrupt			*intr;
	/* [한국어] interrupt-mode 운영 시 epoll fd handler. */

	TAILQ_HEAD(, spdk_nvmf_tcp_port)	ports;
	/* [한국어] 등록된 listening port 리스트 (각 trid별 1개). */
	TAILQ_HEAD(, spdk_nvmf_tcp_poll_group)	poll_groups;
	/* [한국어] 등록된 poll group 리스트 (per-CPU). */

	TAILQ_HEAD(, tcp_psk_entry)		psks;
	/* [한국어] TLS PSK 엔트리 리스트 (subsystem-host 쌍별). */
};

static const struct spdk_json_object_decoder tcp_transport_opts_decoder[] = {
	{
		"c2h_success", offsetof(struct tcp_transport_opts, c2h_success),
		spdk_json_decode_bool, true
	},
	{
		"control_msg_num", offsetof(struct tcp_transport_opts, control_msg_num),
		spdk_json_decode_uint16, true
	},
	{
		"sock_priority", offsetof(struct tcp_transport_opts, sock_priority),
		spdk_json_decode_uint32, true
	},
};

/* [한국어] forward decl: req 상태 머신 한 스텝 진행 (lifecycle 본체). */
static bool nvmf_tcp_req_process(struct spdk_nvmf_tcp_transport *ttransport,
				 struct spdk_nvmf_tcp_req *tcp_req);
/* [한국어] forward decl: poll group destroy. */
static void nvmf_tcp_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group);

/* [한국어] forward decl: read 응답 데이터 PDU(C2H Data) 송신. */
static void _nvmf_tcp_send_c2h_data(struct spdk_nvmf_tcp_qpair *tqpair,
				    struct spdk_nvmf_tcp_req *tcp_req);
/* [한국어] forward decl: qpair에서 다음 PDU 수신 진행. */
static void nvmf_tcp_qpair_process(struct spdk_nvmf_tcp_qpair *tqpair);

/*
 * [한국어]
 * nvmf_tcp_req_set_state - req 상태 전이 + state_cntr 카운터 갱신
 *
 * @tcp_req: 대상 요청
 * @state:   목표 상태
 *
 * 상태 변경 시마다 qpair의 state_cntr 배열을 갱신하여 상태별 req 수 추적.
 * 호출은 single-thread (poll group thread)에서만 발생하므로 락 불필요.
 */
static inline void
nvmf_tcp_req_set_state(struct spdk_nvmf_tcp_req *tcp_req,
		       enum spdk_nvmf_tcp_req_state state)
{
	struct spdk_nvmf_qpair *qpair;			/* [한국어] 공통 qpair */
	struct spdk_nvmf_tcp_qpair *tqpair;		/* [한국어] TCP-전용 qpair */

	qpair = tcp_req->req.qpair;
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);	/* [한국어] 다운캐스트 (offset 기반 컨테이너 참조) */

	assert(tqpair->state_cntr[tcp_req->state] > 0);	/* [한국어] 직전 상태 카운터가 0보다 크다 — 일관성 검증 */
	tqpair->state_cntr[tcp_req->state]--;		/* [한국어] 이전 상태 카운터 감소 */
	tqpair->state_cntr[state]++;			/* [한국어] 신규 상태 카운터 증가 */

	tcp_req->state = state;				/* [한국어] 실제 상태 필드 업데이트 */
}

static inline struct nvme_tcp_pdu *
nvmf_tcp_req_pdu_init(struct spdk_nvmf_tcp_req *tcp_req)
{
	assert(tcp_req->pdu_in_use == false);

	memset(tcp_req->pdu, 0, sizeof(*tcp_req->pdu));
	tcp_req->pdu->qpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);

	return tcp_req->pdu;
}

/*
 * [한국어]
 * nvmf_tcp_req_get - free queue에서 req 객체 1개 할당 (FREE → NEW 전이)
 *
 * @tqpair: 새 요청을 받을 qpair
 * @return: 할당된 req 또는 NULL (free queue가 비어 QD 한도 도달)
 *
 * CapsuleCmd PDU 헤더 수신 직후 호출되어 새 req를 free→working_queue로 옮긴다.
 * NULL 반환 시 호스트가 QD를 초과한 것 — 해당 PDU 처리는 wait 상태로 보류.
 */
static struct spdk_nvmf_tcp_req *
nvmf_tcp_req_get(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_req *tcp_req;

	tcp_req = TAILQ_FIRST(&tqpair->tcp_req_free_queue);	/* [한국어] free 리스트의 첫 번째 슬롯 시도 */
	if (spdk_unlikely(!tcp_req)) {				/* [한국어] free가 없다 — qpair QD 도달 (호스트 측 over-issue) */
		return NULL;
	}

	memset(&tcp_req->rsp, 0, sizeof(tcp_req->rsp));		/* [한국어] 이전 사용분 NVMe CQE 잔여물 0으로 클리어 */
	tcp_req->h2c_offset = 0;				/* [한국어] H2C 누적 수신 오프셋 초기화 */
	tcp_req->has_in_capsule_data = false;			/* [한국어] in-capsule data 플래그 리셋 */
	tcp_req->req.raw = 0; /* clear all flags */		/* [한국어] req 공통 플래그(union)를 한 번에 클리어 */
	tcp_req->req.zcopy_phase = NVMF_ZCOPY_PHASE_NONE;	/* [한국어] zcopy 단계 NONE으로 초기화 */
	tcp_req->req.cmd_cb_fn = NULL;				/* [한국어] 명령 처리 콜백 초기화 */

	TAILQ_REMOVE(&tqpair->tcp_req_free_queue, tcp_req, state_link);	/* [한국어] free 리스트에서 분리 */
	TAILQ_INSERT_TAIL(&tqpair->tcp_req_working_queue, tcp_req, state_link);	/* [한국어] working 리스트 끝에 삽입 */
	tqpair->qpair.queue_depth++;				/* [한국어] qpair QD 카운터 증가 (통계/상한 검증) */
	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_NEW);	/* [한국어] FREE → NEW 상태 전이 (cntr 갱신) */
	return tcp_req;
}

static void
handle_await_req(void *arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair = arg;

	tqpair->await_req_msg_pending = false;
	if (tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_REQ) {
		nvmf_tcp_qpair_process(tqpair);
	}
}

static inline void
nvmf_tcp_req_put(struct spdk_nvmf_tcp_qpair *tqpair, struct spdk_nvmf_tcp_req *tcp_req)
{
	assert(!tcp_req->pdu_in_use);

	TAILQ_REMOVE(&tqpair->tcp_req_working_queue, tcp_req, state_link);
	TAILQ_INSERT_TAIL(&tqpair->tcp_req_free_queue, tcp_req, state_link);
	tqpair->qpair.queue_depth--;
	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_FREE);
	if (tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_REQ &&
	    !tqpair->await_req_msg_pending) {
		tqpair->await_req_msg_pending = true;
		spdk_thread_send_msg(spdk_get_thread(), handle_await_req, tqpair);
	}
}

static void
nvmf_tcp_req_get_buffers_done(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_req *tcp_req;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_tcp_transport *ttransport;

	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_req, req);
	transport = req->qpair->transport;
	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_HAVE_BUFFER);
	nvmf_tcp_req_process(ttransport, tcp_req);
}

static void
nvmf_tcp_request_free(void *cb_arg)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_req *tcp_req = cb_arg;

	assert(tcp_req != NULL);

	SPDK_DEBUGLOG(nvmf_tcp, "tcp_req=%p will be freed\n", tcp_req);
	ttransport = SPDK_CONTAINEROF(tcp_req->req.qpair->transport,
				      struct spdk_nvmf_tcp_transport, transport);
	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_COMPLETED);
	nvmf_tcp_req_process(ttransport, tcp_req);
}

static void
nvmf_tcp_req_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_req *tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_req, req);

	nvmf_tcp_request_free(tcp_req);
}

static void
nvmf_tcp_drain_state_queue(struct spdk_nvmf_tcp_qpair *tqpair,
			   enum spdk_nvmf_tcp_req_state state)
{
	struct spdk_nvmf_tcp_req *tcp_req, *req_tmp;

	assert(state != TCP_REQUEST_STATE_FREE);
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->tcp_req_working_queue, state_link, req_tmp) {
		if (state == tcp_req->state) {
			nvmf_tcp_request_free(tcp_req);
		}
	}
}

static inline void
nvmf_tcp_request_get_buffers_abort(struct spdk_nvmf_tcp_req *tcp_req)
{
	/* Request can wait either for the iobuf or control_msg */
	struct spdk_nvmf_qpair *qpair = tcp_req->req.qpair;
	struct spdk_nvmf_tcp_qpair *tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	struct spdk_nvmf_tcp_poll_group *tcp_group = tqpair->group;
	struct spdk_nvmf_tcp_req *tmp_req, *abort_req;

	assert(tcp_req->state == TCP_REQUEST_STATE_NEED_BUFFER);

	if (tcp_group->control_msg_list != NULL) {
		STAILQ_FOREACH_SAFE(abort_req, &tcp_group->control_msg_list->waiting_for_msg_reqs,
				    control_msg_link, tmp_req) {
			if (abort_req == tcp_req) {
				STAILQ_REMOVE(&tcp_group->control_msg_list->waiting_for_msg_reqs,
					      abort_req, spdk_nvmf_tcp_req, control_msg_link);
				return;
			}
		}
	}

	if (!nvmf_request_get_buffers_abort(&tcp_req->req)) {
		SPDK_ERRLOG("Failed to abort tcp_req=%p\n", tcp_req);
		assert(0 && "Should never happen");
	}
}

static void
nvmf_tcp_abort_await_buffer_reqs(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_req *tcp_req, *req_tmp;

	/* Remove requests waiting for buffer from the waiting list and mark as completed */
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->tcp_req_working_queue, state_link, req_tmp) {
		if (tcp_req->state == TCP_REQUEST_STATE_NEED_BUFFER) {
			nvmf_tcp_request_get_buffers_abort(tcp_req);
			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_COMPLETED);
		}
	}
}

static void
nvmf_tcp_cleanup_all_states(struct spdk_nvmf_tcp_qpair *tqpair)
{
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_NEW);
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_EXECUTING);
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_AWAITING_R2T_ACK);
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_COMPLETED);
}

static void
nvmf_tcp_dump_qpair_req_contents(struct spdk_nvmf_tcp_qpair *tqpair)
{
	int i;
	struct spdk_nvmf_tcp_req *tcp_req;

	SPDK_ERRLOG("Dumping contents of queue pair (QID %d)\n", tqpair->qpair.qid);
	for (i = 1; i < TCP_REQUEST_NUM_STATES; i++) {
		SPDK_ERRLOG("\tNum of requests in state[%d] = %u\n", i, tqpair->state_cntr[i]);
		TAILQ_FOREACH(tcp_req, &tqpair->tcp_req_working_queue, state_link) {
			if ((int)tcp_req->state == i) {
				SPDK_ERRLOG("\t\tRequest Data From Pool: %d\n", tcp_req->req.data_from_pool);
				SPDK_ERRLOG("\t\tRequest opcode: %d\n", tcp_req->req.cmd->nvmf_cmd.opcode);
			}
		}
	}
}

static void
_nvmf_tcp_qpair_destroy(void *_tqpair)
{
	struct spdk_nvmf_tcp_qpair *tqpair = _tqpair;
	spdk_nvmf_transport_qpair_fini_cb cb_fn = tqpair->fini_cb_fn;
	void *cb_arg = tqpair->fini_cb_arg;
	int rc, err = 0;

	spdk_trace_record(TRACE_TCP_QP_DESTROY, tqpair->qpair.trace_id, 0, 0);

	SPDK_DEBUGLOG(nvmf_tcp, "enter\n");

	rc = spdk_sock_close(&tqpair->sock);
	if (rc < 0 || tqpair->sock) {
		SPDK_ERRLOG("spdk_sock_close() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		/* Set it to NULL manually */
		tqpair->sock = NULL;
	}

	assert(err == 0);
	nvmf_tcp_cleanup_all_states(tqpair);

	if (tqpair->state_cntr[TCP_REQUEST_STATE_FREE] != tqpair->resource_count) {
		SPDK_ERRLOG("tqpair(%p) free tcp request num is %u but should be %u\n", tqpair,
			    tqpair->state_cntr[TCP_REQUEST_STATE_FREE],
			    tqpair->resource_count);
		err++;
	}

	if (err > 0) {
		nvmf_tcp_dump_qpair_req_contents(tqpair);
	}

	/* The timeout poller might still be registered here if we close the qpair before host
	 * terminates the connection.
	 */
	spdk_poller_unregister(&tqpair->timeout_poller);
	spdk_dma_free(tqpair->pdus);
	free(tqpair->reqs);
	spdk_free(tqpair->bufs);
	spdk_trace_unregister_owner(tqpair->qpair.trace_id);
	free(tqpair);

	if (cb_fn != NULL) {
		cb_fn(cb_arg);
	}

	SPDK_DEBUGLOG(nvmf_tcp, "Leave\n");
}

static void
nvmf_tcp_qpair_destroy(struct spdk_nvmf_tcp_qpair *tqpair)
{
	/* Delay the destruction to make sure it isn't performed from the context of a sock
	 * callback.  Otherwise, spdk_sock_close() might not abort pending requests, causing their
	 * completions to be executed after the qpair is freed.  (Note: this fixed issue #2471.)
	 */
	spdk_thread_send_msg(spdk_get_thread(), _nvmf_tcp_qpair_destroy, tqpair);
}

static void
nvmf_tcp_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_tcp_transport	*ttransport;
	assert(w != NULL);

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	spdk_json_write_named_bool(w, "c2h_success", ttransport->tcp_opts.c2h_success);
	spdk_json_write_named_uint32(w, "sock_priority", ttransport->tcp_opts.sock_priority);
}

static void
nvmf_tcp_free_psk_entry(struct tcp_psk_entry *entry)
{
	if (entry == NULL) {
		return;
	}

	spdk_memset_s(entry->psk, sizeof(entry->psk), 0, sizeof(entry->psk));
	spdk_keyring_put_key(entry->key);
	free(entry);
}

/*
 * [한국어]
 * nvmf_tcp_destroy - TCP transport 인스턴스 파괴 (transport_ops.destroy 콜백)
 *
 * @transport: 파괴할 transport
 * @cb_fn:     완료 콜백 (NULL 가능)
 *
 * 모든 PSK 엔트리 zeroize/free, accept poller 해제, listen_sock_group close,
 * ttransport 메모리 해제. tgt destroy 단계에서 호출됨. 동기 API.
 */
static void
nvmf_tcp_destroy(struct spdk_nvmf_transport *transport,
		 spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_tcp_transport	*ttransport;
	struct tcp_psk_entry *entry, *tmp;
	int rc;

	assert(transport != NULL);
	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	TAILQ_FOREACH_SAFE(entry, &ttransport->psks, link, tmp) {
		TAILQ_REMOVE(&ttransport->psks, entry, link);
		nvmf_tcp_free_psk_entry(entry);
	}

	spdk_poller_unregister(&ttransport->accept_poller);
	spdk_interrupt_unregister(&ttransport->intr);
	rc = spdk_sock_group_close(&ttransport->listen_sock_group);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_group_close() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		assert(false);
	}

	free(ttransport);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

static int nvmf_tcp_accept(void *ctx);

static void nvmf_tcp_accept_cb(void *ctx, struct spdk_sock_group *group, struct spdk_sock *sock);

/*
 * [한국어]
 * nvmf_tcp_create - TCP transport 인스턴스 생성 (transport_ops.create 콜백)
 *
 * @opts: 사용자/JSON 옵션 (max_io_size, num_shared_buffers, in_capsule_data_size 등)
 * @return: 생성된 transport (실패 시 NULL)
 *
 * spdk_nvmf_transport_create("TCP", opts)에서 호출되는 진입점. 다음을 수행:
 *   1) ttransport calloc + 옵션 디코드 (transport_specific JSON → tcp_opts)
 *   2) 옵션 sanity check (sock_priority, queue_depth 한계 등)
 *   3) accept poller 등록 (NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME 주기)
 *   4) listen_sock_group 생성 (모든 listen 소켓을 한 group에 묶음)
 *   5) interrupt-mode 운영이면 epoll fd에 interrupt handler 등록
 *
 * 실행 컨텍스트: SPDK init thread (nvmf_tgt 시작 시).
 */
static struct spdk_nvmf_transport *
nvmf_tcp_create(struct spdk_nvmf_transport_opts *opts)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	uint32_t sge_count;
	uint32_t min_shared_buffers;
	uint64_t period;

	ttransport = calloc(1, sizeof(*ttransport));
	if (!ttransport) {
		return NULL;
	}

	TAILQ_INIT(&ttransport->ports);
	TAILQ_INIT(&ttransport->poll_groups);
	TAILQ_INIT(&ttransport->psks);

	ttransport->transport.ops = &spdk_nvmf_transport_tcp;

	ttransport->tcp_opts.c2h_success = SPDK_NVMF_TCP_DEFAULT_SUCCESS_OPTIMIZATION;
	ttransport->tcp_opts.sock_priority = SPDK_NVMF_TCP_DEFAULT_SOCK_PRIORITY;
	ttransport->tcp_opts.control_msg_num = SPDK_NVMF_TCP_DEFAULT_CONTROL_MSG_NUM;
	if (opts->transport_specific != NULL &&
	    spdk_json_decode_object_relaxed(opts->transport_specific, tcp_transport_opts_decoder,
					    SPDK_COUNTOF(tcp_transport_opts_decoder),
					    &ttransport->tcp_opts)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		free(ttransport);
		return NULL;
	}

	SPDK_NOTICELOG("*** TCP Transport Init ***\n");

	SPDK_INFOLOG(nvmf_tcp, "*** TCP Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_io_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  in_capsule_data_size=%d, max_aq_depth=%d\n"
		     "  num_shared_buffers=%d, c2h_success=%d,\n"
		     "  dif_insert_or_strip=%d, sock_priority=%d\n"
		     "  abort_timeout_sec=%d, control_msg_num=%hu\n"
		     "  ack_timeout=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr - 1,
		     opts->io_unit_size,
		     opts->in_capsule_data_size,
		     opts->max_aq_depth,
		     opts->num_shared_buffers,
		     ttransport->tcp_opts.c2h_success,
		     opts->dif_insert_or_strip,
		     ttransport->tcp_opts.sock_priority,
		     opts->abort_timeout_sec,
		     ttransport->tcp_opts.control_msg_num,
		     opts->ack_timeout);

	if (ttransport->tcp_opts.sock_priority > SPDK_NVMF_TCP_DEFAULT_MAX_SOCK_PRIORITY) {
		SPDK_ERRLOG("Unsupported socket_priority=%d, the current range is: 0 to %d\n"
			    "you can use man 7 socket to view the range of priority under SO_PRIORITY item\n",
			    ttransport->tcp_opts.sock_priority, SPDK_NVMF_TCP_DEFAULT_MAX_SOCK_PRIORITY);
		free(ttransport);
		return NULL;
	}

	if (ttransport->tcp_opts.control_msg_num == 0 &&
	    opts->in_capsule_data_size < SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE) {
		SPDK_WARNLOG("TCP param control_msg_num can't be 0 if ICD is less than %u bytes. Using default value %u\n",
			     SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE, SPDK_NVMF_TCP_DEFAULT_CONTROL_MSG_NUM);
		ttransport->tcp_opts.control_msg_num = SPDK_NVMF_TCP_DEFAULT_CONTROL_MSG_NUM;
	}

	/* I/O unit size cannot be larger than max I/O size */
	if (opts->io_unit_size > opts->max_io_size) {
		SPDK_WARNLOG("TCP param io_unit_size %u can't be larger than max_io_size %u. Using max_io_size as io_unit_size\n",
			     opts->io_unit_size, opts->max_io_size);
		opts->io_unit_size = opts->max_io_size;
	}

	/* In capsule data size cannot be larger than max I/O size */
	if (opts->in_capsule_data_size > opts->max_io_size) {
		SPDK_WARNLOG("TCP param ICD size %u can't be larger than max_io_size %u. Using max_io_size as ICD size\n",
			     opts->io_unit_size, opts->max_io_size);
		opts->in_capsule_data_size = opts->max_io_size;
	}

	/* max IO queue depth cannot be smaller than 2 or larger than 65535.
	 * We will not check SPDK_NVMF_TCP_MAX_IO_QUEUE_DEPTH, because max_queue_depth is 16bits and always not larger than 64k. */
	if (opts->max_queue_depth < SPDK_NVMF_TCP_MIN_IO_QUEUE_DEPTH) {
		SPDK_WARNLOG("TCP param max_queue_depth %u can't be smaller than %u or larger than %u. Using default value %u\n",
			     opts->max_queue_depth, SPDK_NVMF_TCP_MIN_IO_QUEUE_DEPTH,
			     SPDK_NVMF_TCP_MAX_IO_QUEUE_DEPTH, SPDK_NVMF_TCP_DEFAULT_MAX_IO_QUEUE_DEPTH);
		opts->max_queue_depth = SPDK_NVMF_TCP_DEFAULT_MAX_IO_QUEUE_DEPTH;
	}

	/* max admin queue depth cannot be smaller than 2 or larger than 4096 */
	if (opts->max_aq_depth < SPDK_NVMF_TCP_MIN_ADMIN_QUEUE_DEPTH ||
	    opts->max_aq_depth > SPDK_NVMF_TCP_MAX_ADMIN_QUEUE_DEPTH) {
		SPDK_WARNLOG("TCP param max_aq_depth %u can't be smaller than %u or larger than %u. Using default value %u\n",
			     opts->max_aq_depth, SPDK_NVMF_TCP_MIN_ADMIN_QUEUE_DEPTH,
			     SPDK_NVMF_TCP_MAX_ADMIN_QUEUE_DEPTH, SPDK_NVMF_TCP_DEFAULT_MAX_ADMIN_QUEUE_DEPTH);
		opts->max_aq_depth = SPDK_NVMF_TCP_DEFAULT_MAX_ADMIN_QUEUE_DEPTH;
	}

	sge_count = opts->max_io_size / opts->io_unit_size;
	if (sge_count > SPDK_NVMF_MAX_SGL_ENTRIES) {
		SPDK_ERRLOG("Unsupported IO Unit size specified, %d bytes\n", opts->io_unit_size);
		free(ttransport);
		return NULL;
	}

	/* If buf_cache_size == UINT32_MAX, we will dynamically pick a cache size later that we know will fit. */
	if (opts->buf_cache_size < UINT32_MAX) {
		min_shared_buffers = spdk_env_get_core_count() * opts->buf_cache_size;
		if (min_shared_buffers > opts->num_shared_buffers) {
			SPDK_ERRLOG("There are not enough buffers to satisfy "
				    "per-poll group caches for each thread. (%" PRIu32 ") "
				    "supplied. (%" PRIu32 ") required\n", opts->num_shared_buffers, min_shared_buffers);
			SPDK_ERRLOG("Please specify a larger number of shared buffers\n");
			free(ttransport);
			return NULL;
		}
	}

	period = spdk_interrupt_mode_is_enabled() ? 0 : opts->acceptor_poll_rate;
	ttransport->accept_poller = SPDK_POLLER_REGISTER(nvmf_tcp_accept, &ttransport->transport, period);
	if (!ttransport->accept_poller) {
		free(ttransport);
		return NULL;
	}

	spdk_poller_register_interrupt(ttransport->accept_poller, NULL, NULL);

	ttransport->listen_sock_group = spdk_sock_group_create(NULL);
	if (ttransport->listen_sock_group == NULL) {
		SPDK_ERRLOG("Failed to create socket group for listen sockets\n");
		spdk_poller_unregister(&ttransport->accept_poller);
		free(ttransport);
		return NULL;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		ttransport->intr = SPDK_INTERRUPT_REGISTER_FOR_EVENTS(spdk_sock_group_get_interruptfd(
					   ttransport->listen_sock_group),
				   SPDK_INTERRUPT_EVENT_IN | SPDK_INTERRUPT_EVENT_OUT, nvmf_tcp_accept,
				   &ttransport->transport);
		if (ttransport->intr == NULL) {
			SPDK_ERRLOG("Failed to register interrupt for listen socker sock group\n");
			spdk_sock_group_close(&ttransport->listen_sock_group);
			spdk_poller_unregister(&ttransport->accept_poller);
			free(ttransport);
			return NULL;
		}
	}

	return &ttransport->transport;
}

static int
nvmf_tcp_trsvcid_to_int(const char *trsvcid)
{
	unsigned long long ull;
	char *end = NULL;

	ull = strtoull(trsvcid, &end, 10);
	if (end == NULL || end == trsvcid || *end != '\0') {
		return -1;
	}

	/* Valid TCP/IP port numbers are in [1, 65535] */
	if (ull == 0 || ull > 65535) {
		return -1;
	}

	return (int)ull;
}

/**
 * Canonicalize a listen address trid.
 */
static int
nvmf_tcp_canon_listen_trid(struct spdk_nvme_transport_id *canon_trid,
			   const struct spdk_nvme_transport_id *trid)
{
	int trsvcid_int;

	trsvcid_int = nvmf_tcp_trsvcid_to_int(trid->trsvcid);
	if (trsvcid_int < 0) {
		return -EINVAL;
	}

	memset(canon_trid, 0, sizeof(*canon_trid));
	spdk_nvme_trid_populate_transport(canon_trid, SPDK_NVME_TRANSPORT_TCP);
	canon_trid->adrfam = trid->adrfam;
	snprintf(canon_trid->traddr, sizeof(canon_trid->traddr), "%s", trid->traddr);
	snprintf(canon_trid->trsvcid, sizeof(canon_trid->trsvcid), "%d", trsvcid_int);

	return 0;
}

/**
 * Find an existing listening port.
 */
static struct spdk_nvmf_tcp_port *
nvmf_tcp_find_port(struct spdk_nvmf_tcp_transport *ttransport,
		   const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvme_transport_id canon_trid;
	struct spdk_nvmf_tcp_port *port;

	if (nvmf_tcp_canon_listen_trid(&canon_trid, trid) != 0) {
		return NULL;
	}

	TAILQ_FOREACH(port, &ttransport->ports, link) {
		if (spdk_nvme_transport_id_compare(&canon_trid, port->trid) == 0) {
			return port;
		}
	}

	return NULL;
}

static int
tcp_sock_get_key(uint8_t *out, int out_len, const char **cipher, const char *pskid,
		 void *get_key_ctx)
{
	struct tcp_psk_entry *entry;
	struct spdk_nvmf_tcp_transport *ttransport = get_key_ctx;
	size_t psk_len;
	int rc;

	TAILQ_FOREACH(entry, &ttransport->psks, link) {
		if (strcmp(pskid, entry->pskid) != 0) {
			continue;
		}

		psk_len = entry->psk_size;
		if ((size_t)out_len < psk_len) {
			SPDK_ERRLOG("Out buffer of size: %" PRIu32 " cannot fit PSK of len: %lu\n",
				    out_len, psk_len);
			return -ENOBUFS;
		}

		/* Convert PSK to the TLS PSK format. */
		rc = nvme_tcp_derive_tls_psk(entry->psk, psk_len, pskid, out, out_len,
					     entry->tls_cipher_suite);
		if (rc < 0) {
			SPDK_ERRLOG("Could not generate TLS PSK\n");
		}

		switch (entry->tls_cipher_suite) {
		case NVME_TCP_CIPHER_AES_128_GCM_SHA256:
			*cipher = "TLS_AES_128_GCM_SHA256";
			break;
		case NVME_TCP_CIPHER_AES_256_GCM_SHA384:
			*cipher = "TLS_AES_256_GCM_SHA384";
			break;
		default:
			*cipher = NULL;
			return -ENOTSUP;
		}

		return rc;
	}

	SPDK_ERRLOG("Could not find PSK for identity: %s\n", pskid);

	return -EINVAL;
}

/*
 * [한국어]
 * nvmf_tcp_listen - 새 TCP listening port 생성 (transport_ops.listen 콜백)
 *
 * @transport:    TCP transport
 * @trid:         listen할 transport id (trtype=TCP, traddr=IP, trsvcid=port)
 * @listen_opts:  옵션 (secure_channel(TLS), sock_impl 등)
 * @return: 0=성공, -EINVAL/-ENOMEM/-errno
 *
 * spdk_nvmf_tgt_listen()에서 호출. 다음을 수행:
 *   1) trsvcid 정수 변환 + port 객체 calloc
 *   2) sock opts 설정 (priority, ack_timeout)
 *   3) TLS 옵션 처리: secure_channel=true이면 sock_impl="ssl" 강제,
 *      tcp_sock_get_key 콜백 등록 (TLS 1.3 PSK)
 *   4) spdk_sock_listen_ext: 실제 listen() 호출 — IPv4/IPv6/TLS 자동 분기
 *   5) listen_sock_group에 등록 — accept 콜백 nvmf_tcp_accept_cb 바인딩
 *   6) ports 리스트에 추가
 *
 * 호출 후 호스트의 connect 시 nvmf_tcp_accept_cb가 sock group polling을
 * 통해 호출되어 새 qpair가 생성된다.
 */
static int
nvmf_tcp_listen(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
		struct spdk_nvmf_listen_opts *listen_opts)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_port *port;
	int trsvcid_int;
	uint8_t adrfam;
	const char *sock_impl_name;
	struct spdk_sock_impl_opts impl_opts;
	size_t impl_opts_size = sizeof(impl_opts);
	struct spdk_sock_opts opts;
	int rc;

	if (!strlen(trid->trsvcid)) {
		SPDK_ERRLOG("Service id is required\n");
		return -EINVAL;
	}

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	trsvcid_int = nvmf_tcp_trsvcid_to_int(trid->trsvcid);
	if (trsvcid_int < 0) {
		SPDK_ERRLOG("Invalid trsvcid '%s'\n", trid->trsvcid);
		return -EINVAL;
	}

	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		return -ENOMEM;
	}

	port->trid = trid;

	sock_impl_name = NULL;

	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	opts.priority = ttransport->tcp_opts.sock_priority;
	opts.ack_timeout = transport->opts.ack_timeout;
	if (listen_opts->secure_channel) {
		if (listen_opts->sock_impl &&
		    strncmp("ssl", listen_opts->sock_impl, strlen(listen_opts->sock_impl))) {
			SPDK_ERRLOG("Enabling secure_channel while specifying a sock_impl different from 'ssl' is unsupported");
			free(port);
			return -EINVAL;
		}
		listen_opts->sock_impl = "ssl";
	}

	if (listen_opts->sock_impl) {
		sock_impl_name = listen_opts->sock_impl;
		spdk_sock_impl_get_opts(sock_impl_name, &impl_opts, &impl_opts_size);

		if (!strncmp("ssl", sock_impl_name, strlen(sock_impl_name))) {
			if (!g_tls_log) {
				SPDK_NOTICELOG("TLS support is considered experimental\n");
				g_tls_log = true;
			}
			impl_opts.tls_version = SPDK_TLS_VERSION_1_3;
			impl_opts.get_key = tcp_sock_get_key;
			impl_opts.get_key_ctx = ttransport;
			impl_opts.tls_cipher_suites = "TLS_AES_256_GCM_SHA384:TLS_AES_128_GCM_SHA256";
		}

		opts.impl_opts = &impl_opts;
		opts.impl_opts_size = sizeof(impl_opts);
	}

	port->listen_sock = spdk_sock_listen_ext(trid->traddr, trsvcid_int,
			    sock_impl_name, &opts);
	if (port->listen_sock == NULL) {
		SPDK_ERRLOG("spdk_sock_listen(%s, %d) failed: %s (%d)\n",
			    trid->traddr, trsvcid_int,
			    spdk_strerror(errno), errno);
		free(port);
		return -errno;
	}

	if (spdk_sock_is_ipv4(port->listen_sock)) {
		adrfam = SPDK_NVMF_ADRFAM_IPV4;
	} else if (spdk_sock_is_ipv6(port->listen_sock)) {
		adrfam = SPDK_NVMF_ADRFAM_IPV6;
	} else {
		SPDK_ERRLOG("Unhandled socket type\n");
		adrfam = 0;
	}

	if (adrfam != trid->adrfam) {
		SPDK_ERRLOG("Socket address family mismatch\n");
		spdk_sock_close(&port->listen_sock);
		free(port);
		return -EINVAL;
	}

	rc = spdk_sock_group_add_sock(ttransport->listen_sock_group, port->listen_sock, nvmf_tcp_accept_cb,
				      port);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_group_add_sock() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		spdk_sock_close(&port->listen_sock);
		free(port);
		return rc;
	}

	port->transport = transport;

	SPDK_NOTICELOG("*** NVMe/TCP Target Listening on %s port %s ***\n",
		       trid->traddr, trid->trsvcid);

	TAILQ_INSERT_TAIL(&ttransport->ports, port, link);
	return 0;
}

/*
 * [한국어]
 * nvmf_tcp_stop_listen - listening port 제거 (transport_ops.stop_listen 콜백)
 *
 * @trid: 중지할 listen 주소
 *
 * 해당 trid의 port를 listen_sock_group에서 제거하고 sock close + free.
 * 기존 accept된 qpair는 영향 없으나 신규 connect는 차단된다.
 */
static void
nvmf_tcp_stop_listen(struct spdk_nvmf_transport *transport,
		     const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_port *port;
	int rc;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	SPDK_DEBUGLOG(nvmf_tcp, "Removing listen address %s port %s\n",
		      trid->traddr, trid->trsvcid);

	port = nvmf_tcp_find_port(ttransport, trid);
	if (port) {
		rc = spdk_sock_group_remove_sock(ttransport->listen_sock_group, port->listen_sock);
		if (rc < 0) {
			SPDK_ERRLOG("spdk_sock_group_remove_sock() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		}

		TAILQ_REMOVE(&ttransport->ports, port, link);
		spdk_sock_close(&port->listen_sock);
		free(port);
	}
}

static void nvmf_tcp_qpair_set_recv_state(struct spdk_nvmf_tcp_qpair *tqpair,
		enum nvme_tcp_pdu_recv_state state);

static void
nvmf_tcp_qpair_set_state(struct spdk_nvmf_tcp_qpair *tqpair, enum nvmf_tcp_qpair_state state)
{
	tqpair->state = state;
	spdk_trace_record(TRACE_TCP_QP_STATE_CHANGE, tqpair->qpair.trace_id, 0, 0,
			  (uint64_t)tqpair->state);
}

static void
nvmf_tcp_qpair_disconnect(struct spdk_nvmf_tcp_qpair *tqpair)
{
	SPDK_DEBUGLOG(nvmf_tcp, "Disconnecting qpair %p\n", tqpair);

	spdk_trace_record(TRACE_TCP_QP_DISCONNECT, tqpair->qpair.trace_id, 0, 0);

	if (tqpair->state <= NVMF_TCP_QPAIR_STATE_RUNNING) {
		nvmf_tcp_qpair_set_state(tqpair, NVMF_TCP_QPAIR_STATE_EXITING);
		assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
		spdk_poller_unregister(&tqpair->timeout_poller);

		/* This will end up calling nvmf_tcp_close_qpair */
		spdk_nvmf_qpair_disconnect(&tqpair->qpair);
	}
}

static void
_mgmt_pdu_write_done(void *_tqpair, int err)
{
	struct spdk_nvmf_tcp_qpair *tqpair = _tqpair;
	struct nvme_tcp_pdu *pdu = tqpair->mgmt_pdu;

	if (spdk_unlikely(err != 0)) {
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
		return;
	}

	assert(pdu->cb_fn != NULL);
	pdu->cb_fn(pdu->cb_arg);
}

static void
_req_pdu_write_done(void *req, int err)
{
	struct spdk_nvmf_tcp_req *tcp_req = req;
	struct nvme_tcp_pdu *pdu = tcp_req->pdu;
	struct spdk_nvmf_tcp_qpair *tqpair = pdu->qpair;

	assert(tcp_req->pdu_in_use);
	tcp_req->pdu_in_use = false;

	/* If the request is in a completed state, we're waiting for write completion to free it */
	if (spdk_unlikely(tcp_req->state == TCP_REQUEST_STATE_COMPLETED)) {
		nvmf_tcp_request_free(tcp_req);
		return;
	}

	if (spdk_unlikely(err != 0)) {
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
		return;
	}

	assert(pdu->cb_fn != NULL);
	pdu->cb_fn(pdu->cb_arg);
}

static void
_pdu_write_done(struct nvme_tcp_pdu *pdu, int err)
{
	pdu->sock_req.cb_fn(pdu->sock_req.cb_arg, err);
}

static void
tcp_sock_flush_cb(void *arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair = arg;
	int rc;

	tqpair->pending_flush = false;
	rc = spdk_sock_flush(tqpair->sock);
	if (rc < 0 && rc == -EAGAIN) {
		if (spdk_interrupt_mode_is_enabled()) {
			/* In interrupt mode we need to force a retry. In polling mode it will naturally
			 * try again. */
			spdk_thread_send_msg(spdk_get_thread(), tcp_sock_flush_cb, tqpair);
			return;
		}

		rc = 0;
	}

	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_flush() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
	}
}

static void
_tcp_write_pdu(struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_qpair *tqpair = pdu->qpair;

	pdu->sock_req.iovcnt = nvme_tcp_build_iovs(pdu->iov, SPDK_COUNTOF(pdu->iov), pdu,
			       tqpair->host_hdgst_enable, tqpair->host_ddgst_enable, NULL);
	spdk_sock_writev_async(tqpair->sock, &pdu->sock_req);

	if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP ||
	    pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ ||
	    spdk_interrupt_mode_is_enabled()) {
		/* Async writes must be flushed */
		if (!tqpair->pending_flush) {
			tqpair->pending_flush = true;
			spdk_thread_send_msg(spdk_get_thread(), tcp_sock_flush_cb, tqpair);
		}
	}
}

static void
data_crc32_accel_done(void *cb_arg, int status)
{
	struct nvme_tcp_pdu *pdu = cb_arg;

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("Failed to compute the data digest for pdu =%p\n", pdu);
		_pdu_write_done(pdu, status);
		return;
	}

	pdu->data_digest_crc32 ^= SPDK_CRC32C_XOR;
	MAKE_DIGEST_WORD(pdu->data_digest, pdu->data_digest_crc32);

	_tcp_write_pdu(pdu);
}

static void
pdu_data_crc32_compute(struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_qpair *tqpair = pdu->qpair;
	int rc = 0;

	/* Data Digest */
	if (pdu->data_len > 0 && g_nvme_tcp_ddgst[pdu->hdr.common.pdu_type] && tqpair->host_ddgst_enable) {
		/* Only support this limitated case for the first step */
		if (spdk_likely(!pdu->dif_ctx && (pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT == 0)
				&& tqpair->group)) {
			rc = spdk_accel_submit_crc32cv(tqpair->group->accel_channel, &pdu->data_digest_crc32, pdu->data_iov,
						       pdu->data_iovcnt, 0, data_crc32_accel_done, pdu);
			if (spdk_likely(rc == 0)) {
				return;
			}
		} else {
			pdu->data_digest_crc32 = nvme_tcp_pdu_calc_data_digest(pdu);
		}
		data_crc32_accel_done(pdu, rc);
	} else {
		_tcp_write_pdu(pdu);
	}
}

static void
nvmf_tcp_qpair_write_pdu(struct spdk_nvmf_tcp_qpair *tqpair,
			 struct nvme_tcp_pdu *pdu,
			 nvme_tcp_qpair_xfer_complete_cb cb_fn,
			 void *cb_arg)
{
	int hlen;
	uint32_t crc32c;

	assert(tqpair->pdu_in_progress != pdu);

	hlen = pdu->hdr.common.hlen;
	pdu->cb_fn = cb_fn;
	pdu->cb_arg = cb_arg;

	pdu->iov[0].iov_base = &pdu->hdr.raw;
	pdu->iov[0].iov_len = hlen;

	/* Header Digest */
	if (g_nvme_tcp_hdgst[pdu->hdr.common.pdu_type] && tqpair->host_hdgst_enable) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		MAKE_DIGEST_WORD((uint8_t *)pdu->hdr.raw + hlen, crc32c);
	}

	/* Data Digest */
	pdu_data_crc32_compute(pdu);
}

static void
nvmf_tcp_qpair_write_mgmt_pdu(struct spdk_nvmf_tcp_qpair *tqpair,
			      nvme_tcp_qpair_xfer_complete_cb cb_fn,
			      void *cb_arg)
{
	struct nvme_tcp_pdu *pdu = tqpair->mgmt_pdu;

	pdu->sock_req.cb_fn = _mgmt_pdu_write_done;
	pdu->sock_req.cb_arg = tqpair;

	nvmf_tcp_qpair_write_pdu(tqpair, pdu, cb_fn, cb_arg);
}

static void
nvmf_tcp_qpair_write_req_pdu(struct spdk_nvmf_tcp_qpair *tqpair,
			     struct spdk_nvmf_tcp_req *tcp_req,
			     nvme_tcp_qpair_xfer_complete_cb cb_fn,
			     void *cb_arg)
{
	struct nvme_tcp_pdu *pdu = tcp_req->pdu;

	pdu->sock_req.cb_fn = _req_pdu_write_done;
	pdu->sock_req.cb_arg = tcp_req;

	assert(!tcp_req->pdu_in_use);
	tcp_req->pdu_in_use = true;

	nvmf_tcp_qpair_write_pdu(tqpair, pdu, cb_fn, cb_arg);
}

static int
nvmf_tcp_qpair_init_mem_resource(struct spdk_nvmf_tcp_qpair *tqpair)
{
	uint32_t i;
	struct spdk_nvmf_transport_opts *opts;
	uint32_t in_capsule_data_size;

	opts = &tqpair->qpair.transport->opts;

	in_capsule_data_size = opts->in_capsule_data_size;
	if (opts->dif_insert_or_strip) {
		in_capsule_data_size = SPDK_BDEV_BUF_SIZE_WITH_MD(in_capsule_data_size);
	}

	tqpair->resource_count = opts->max_queue_depth;

	tqpair->reqs = calloc(tqpair->resource_count, sizeof(*tqpair->reqs));
	if (!tqpair->reqs) {
		SPDK_ERRLOG("Unable to allocate reqs on tqpair=%p\n", tqpair);
		return -1;
	}

	if (in_capsule_data_size) {
		tqpair->bufs = spdk_zmalloc(tqpair->resource_count * in_capsule_data_size, 0x1000,
					    NULL, SPDK_ENV_LCORE_ID_ANY,
					    SPDK_MALLOC_DMA);
		if (!tqpair->bufs) {
			SPDK_ERRLOG("Unable to allocate bufs on tqpair=%p.\n", tqpair);
			return -1;
		}
	}
	/* prepare memory space for receiving pdus and tcp_req */
	/* Add additional 1 member, which will be used for mgmt_pdu owned by the tqpair */
	tqpair->pdus = spdk_dma_zmalloc((2 * tqpair->resource_count + 1) * sizeof(*tqpair->pdus), 0x1000,
					NULL);
	if (!tqpair->pdus) {
		SPDK_ERRLOG("Unable to allocate pdu pool on tqpair =%p.\n", tqpair);
		return -1;
	}

	for (i = 0; i < tqpair->resource_count; i++) {
		struct spdk_nvmf_tcp_req *tcp_req = &tqpair->reqs[i];

		tcp_req->ttag = i + 1;
		tcp_req->req.qpair = &tqpair->qpair;

		tcp_req->pdu = &tqpair->pdus[i];
		tcp_req->pdu->qpair = tqpair;

		/* Set up memory to receive commands */
		if (tqpair->bufs) {
			tcp_req->buf = (void *)((uintptr_t)tqpair->bufs + (i * in_capsule_data_size));
		}

		/* Set the cmdn and rsp */
		tcp_req->req.rsp = (union nvmf_c2h_msg *)&tcp_req->rsp;
		tcp_req->req.cmd = (union nvmf_h2c_msg *)&tcp_req->cmd;

		tcp_req->req.stripped_data = NULL;

		/* Initialize request state to FREE */
		tcp_req->state = TCP_REQUEST_STATE_FREE;
		TAILQ_INSERT_TAIL(&tqpair->tcp_req_free_queue, tcp_req, state_link);
		tqpair->state_cntr[TCP_REQUEST_STATE_FREE]++;
	}

	for (; i < 2 * tqpair->resource_count; i++) {
		struct nvme_tcp_pdu *pdu = &tqpair->pdus[i];

		pdu->qpair = tqpair;
		SLIST_INSERT_HEAD(&tqpair->tcp_pdu_free_queue, pdu, slist);
	}

	tqpair->mgmt_pdu = &tqpair->pdus[i];
	tqpair->mgmt_pdu->qpair = tqpair;
	tqpair->pdu_in_progress = SLIST_FIRST(&tqpair->tcp_pdu_free_queue);
	SLIST_REMOVE_HEAD(&tqpair->tcp_pdu_free_queue, slist);
	tqpair->tcp_pdu_working_count = 1;

	tqpair->recv_buf_size = (in_capsule_data_size + sizeof(struct spdk_nvme_tcp_cmd) + 2 *
				 SPDK_NVME_TCP_DIGEST_LEN) * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR;

	return 0;
}

static int
nvmf_tcp_qpair_init(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_qpair *tqpair;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	SPDK_DEBUGLOG(nvmf_tcp, "New TCP Connection: %p\n", qpair);

	spdk_trace_record(TRACE_TCP_QP_CREATE, tqpair->qpair.trace_id, 0, 0);

	/* Initialise request state queues of the qpair */
	TAILQ_INIT(&tqpair->tcp_req_free_queue);
	TAILQ_INIT(&tqpair->tcp_req_working_queue);
	SLIST_INIT(&tqpair->tcp_pdu_free_queue);
	tqpair->qpair.queue_depth = 0;

	tqpair->host_hdgst_enable = true;
	tqpair->host_ddgst_enable = true;

	return 0;
}

static int
nvmf_tcp_qpair_sock_init(struct spdk_nvmf_tcp_qpair *tqpair)
{
	char saddr[SPDK_NVMF_TRADDR_MAX_LEN], caddr[SPDK_NVMF_TRADDR_MAX_LEN];
	uint16_t sport, cport;
	/* 1 for colon, up to 5 for port number, 1 for null terminator */
	char owner[sizeof(caddr) + 1 + 5 + 1];
	int rc;

	rc = spdk_sock_getaddr(tqpair->sock, saddr, sizeof(saddr), &sport,
			       caddr, sizeof(caddr), &cport);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_getaddr() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		return rc;
	}
	/* update buffer size for owner when changing format or arguments here */
	snprintf(owner, sizeof(owner), "%s:%d", caddr, cport);
	tqpair->qpair.trace_id = spdk_trace_register_owner(OWNER_TYPE_NVMF_TCP, owner);
	spdk_trace_record(TRACE_TCP_QP_SOCK_INIT, tqpair->qpair.trace_id, 0, 0);

	/* set low water mark */
	rc = spdk_sock_set_recvlowat(tqpair->sock, 1);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_set_recvlowat() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * nvmf_tcp_handle_connect - accept된 새 sock에 대한 qpair 객체 생성/등록
 *
 * @port: 이 connect를 받은 listening port (역참조용)
 * @sock: spdk_sock_accept이 반환한 신규 connection sock
 *
 * 호스트 connect 직후 호출되어 다음을 수행:
 *   1) tqpair calloc + sock 바인딩
 *   2) target/initiator 주소 정보 추출 (getsockname/getpeername)
 *   3) NUMA id 추출 (NUMA-aware poll group 분배 위해)
 *   4) spdk_nvmf_tgt_new_qpair 호출 — 적절한 poll group을 골라 qpair 등록
 *      → 이후 ICReq/ICResp 시퀀스부터 NVMe-TCP 핸드셰이크 시작
 *
 * 실행 컨텍스트: accept poller thread (보통 init thread).
 */
static void
nvmf_tcp_handle_connect(struct spdk_nvmf_tcp_port *port, struct spdk_sock *sock)
{
	struct spdk_nvmf_tcp_qpair *tqpair;	/* [한국어] 새로 생성할 TCP qpair */
	int rc;

	SPDK_DEBUGLOG(nvmf_tcp, "New connection accepted on %s port %s\n",
		      port->trid->traddr, port->trid->trsvcid);

	tqpair = calloc(1, sizeof(struct spdk_nvmf_tcp_qpair));	/* [한국어] qpair 메모리 0 초기화 할당 */
	if (tqpair == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		spdk_sock_close(&sock);				/* [한국어] 메모리 부족 — sock 정리하고 connection 거부 */
		return;
	}

	tqpair->sock = sock;					/* [한국어] qpair에 sock 결합 */
	tqpair->state_cntr[TCP_REQUEST_STATE_FREE] = 0;		/* [한국어] req 카운터 초기화 (실제 req는 init 단계에서 부여) */
	tqpair->port = port;					/* [한국어] 어느 listener에서 왔는지 역참조 */
	tqpair->qpair.transport = port->transport;		/* [한국어] 공통 qpair에 transport 포인터 */
	tqpair->qpair.numa.id_valid = 1;
	tqpair->qpair.numa.id = spdk_sock_get_numa_id(sock);	/* [한국어] sock fd 기반 NUMA id — poll group 분배 시 활용 */

	rc = spdk_sock_getaddr(tqpair->sock, tqpair->target_addr,
			       sizeof(tqpair->target_addr), &tqpair->target_port,
			       tqpair->initiator_addr, sizeof(tqpair->initiator_addr),
			       &tqpair->initiator_port);
	/* [한국어] getsockname + getpeername — local/remote IP:port 문자열 획득 (로깅/discovery 용) */
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_getaddr() failed, tqpair=%p, rc %d: %s\n", tqpair, rc, spdk_strerror(-rc));
		nvmf_tcp_qpair_destroy(tqpair);			/* [한국어] 주소 추출 실패 — qpair 정리 후 종료 */
		return;
	}

	spdk_nvmf_tgt_new_qpair(port->transport->tgt, &tqpair->qpair);
	/* [한국어] tgt에 신규 qpair 통지 — tgt가 적절한 poll group 선택해 add_qpair 콜백 호출.
	 * 이후 ICReq 수신부터 본격적인 NVMe-TCP 처리가 poll group thread에서 시작. */
}

static uint32_t
nvmf_tcp_port_accept(struct spdk_nvmf_tcp_port *port)
{
	struct spdk_sock *sock;
	uint32_t count = 0;
	int i;

	for (i = 0; i < NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME; i++) {
		sock = spdk_sock_accept(port->listen_sock);
		if (sock == NULL) {
			break;
		}
		count++;
		nvmf_tcp_handle_connect(port, sock);
	}

	return count;
}

/*
 * [한국어]
 * nvmf_tcp_accept - accept poller 본체 (transport 생성 시 등록됨)
 *
 * @ctx: transport 포인터
 * @return: SPDK_POLLER_BUSY/IDLE
 *
 * spdk_sock_group_poll로 listen_sock_group을 polling — readable한 listen 소켓이
 * 있으면 등록된 콜백 nvmf_tcp_accept_cb가 호출되어 nvmf_tcp_port_accept로 진입.
 * acceptor_poll_rate 주기로 SPDK 메인 thread에서 실행됨.
 */
static int
nvmf_tcp_accept(void *ctx)
{
	struct spdk_nvmf_transport *transport = ctx;		/* [한국어] context는 transport 포인터 */
	struct spdk_nvmf_tcp_transport *ttransport;
	int rc;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);	/* [한국어] 다운캐스트 */

	rc = spdk_sock_group_poll(ttransport->listen_sock_group);
	/* [한국어] 모든 listen 소켓을 한 번 polling — accept-able 소켓의 콜백 invoke.
	 * 내부적으로 epoll_wait/uring_wait 1회. */
	if (rc < 0) {
		SPDK_ERRLOG("Failed to poll sock_group=%p (%d): %s\n", ttransport->listen_sock_group, rc,
			    spdk_strerror(-rc));
	}

	return rc != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;	/* [한국어] 진행 작업 있으면 BUSY 반환 — reactor 통계용 */
}

/*
 * [한국어]
 * nvmf_tcp_accept_cb - listen 소켓이 readable 할 때 sock_group이 호출하는 콜백
 *
 * @ctx: spdk_sock_group_add_sock에서 등록된 cb_arg (= port 포인터)
 * @sock: 콜백을 트리거한 listen 소켓
 *
 * 한 번의 콜백에서 NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME(=16) 만큼 accept 시도.
 * 다중 connect가 큐잉되어 있어도 fairness 유지. */
static void
nvmf_tcp_accept_cb(void *ctx, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_nvmf_tcp_port *port = ctx;

	nvmf_tcp_port_accept(port);
}

static void
nvmf_tcp_discover(struct spdk_nvmf_transport *transport,
		  struct spdk_nvme_transport_id *trid,
		  struct spdk_nvmf_discovery_log_page_entry *entry)
{
	struct spdk_nvmf_tcp_port *port;
	struct spdk_nvmf_tcp_transport *ttransport;

	entry->trtype = SPDK_NVMF_TRTYPE_TCP;
	entry->adrfam = trid->adrfam;

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	port = nvmf_tcp_find_port(ttransport, trid);

	assert(port != NULL);

	if (strcmp(spdk_sock_get_impl_name(port->listen_sock), "ssl") == 0) {
		entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_REQUIRED;
		entry->tsas.tcp.sectype = SPDK_NVME_TCP_SECURITY_TLS_1_3;
	} else {
		entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED;
		entry->tsas.tcp.sectype = SPDK_NVME_TCP_SECURITY_NONE;
	}
}

static struct spdk_nvmf_tcp_control_msg_list *
nvmf_tcp_control_msg_list_create(uint16_t num_messages)
{
	struct spdk_nvmf_tcp_control_msg_list *list;
	struct spdk_nvmf_tcp_control_msg *msg;
	uint16_t i;

	list = calloc(1, sizeof(*list));
	if (!list) {
		SPDK_ERRLOG("Failed to allocate memory for list structure\n");
		return NULL;
	}

	list->msg_buf = spdk_zmalloc(num_messages * SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE,
				     NVMF_DATA_BUFFER_ALIGNMENT, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!list->msg_buf) {
		SPDK_ERRLOG("Failed to allocate memory for control message buffers\n");
		free(list);
		return NULL;
	}

	STAILQ_INIT(&list->free_msgs);
	STAILQ_INIT(&list->waiting_for_msg_reqs);

	for (i = 0; i < num_messages; i++) {
		msg = (struct spdk_nvmf_tcp_control_msg *)((char *)list->msg_buf + i *
				SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE);
		STAILQ_INSERT_TAIL(&list->free_msgs, msg, link);
	}

	return list;
}

static void
nvmf_tcp_control_msg_list_free(struct spdk_nvmf_tcp_control_msg_list *list)
{
	if (!list) {
		return;
	}

	spdk_free(list->msg_buf);
	free(list);
}

static int nvmf_tcp_poll_group_poll(struct spdk_nvmf_transport_poll_group *group);

static int
nvmf_tcp_poll_group_intr(void *ctx)
{
	struct spdk_nvmf_transport_poll_group *group = ctx;
	int ret = 0;

	ret = nvmf_tcp_poll_group_poll(group);

	return ret != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static struct spdk_nvmf_transport_poll_group *
nvmf_tcp_poll_group_create(struct spdk_nvmf_transport *transport,
			   struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_tcp_transport	*ttransport;
	struct spdk_nvmf_tcp_poll_group *tgroup;

	tgroup = calloc(1, sizeof(*tgroup));
	if (!tgroup) {
		return NULL;
	}

	tgroup->sock_group = spdk_sock_group_create(&tgroup->group);
	if (!tgroup->sock_group) {
		goto cleanup;
	}

	TAILQ_INIT(&tgroup->qpairs);

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	if (transport->opts.in_capsule_data_size < SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE) {
		SPDK_DEBUGLOG(nvmf_tcp, "ICD %u is less than min required for admin/fabric commands (%u). "
			      "Creating control messages list\n", transport->opts.in_capsule_data_size,
			      SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE);
		tgroup->control_msg_list = nvmf_tcp_control_msg_list_create(ttransport->tcp_opts.control_msg_num);
		if (!tgroup->control_msg_list) {
			goto cleanup;
		}
	}

	tgroup->accel_channel = spdk_accel_get_io_channel();
	if (spdk_unlikely(!tgroup->accel_channel)) {
		SPDK_ERRLOG("Cannot create accel_channel for tgroup=%p\n", tgroup);
		goto cleanup;
	}

	TAILQ_INSERT_TAIL(&ttransport->poll_groups, tgroup, link);
	if (ttransport->next_pg == NULL) {
		ttransport->next_pg = tgroup;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		tgroup->intr = SPDK_INTERRUPT_REGISTER_FOR_EVENTS(spdk_sock_group_get_interruptfd(
					tgroup->sock_group),
				SPDK_INTERRUPT_EVENT_IN | SPDK_INTERRUPT_EVENT_OUT,
				nvmf_tcp_poll_group_intr, &tgroup->group);
		if (tgroup->intr == NULL) {
			SPDK_ERRLOG("Failed to register interrupt for sock group\n");
			goto cleanup;
		}
	}

	return &tgroup->group;

cleanup:
	nvmf_tcp_poll_group_destroy(&tgroup->group);
	return NULL;
}

static struct spdk_nvmf_transport_poll_group *
nvmf_tcp_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_poll_group **pg;
	struct spdk_nvmf_tcp_qpair *tqpair;
	struct spdk_sock_group *group = NULL, *hint = NULL;
	int rc;

	ttransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_tcp_transport, transport);

	if (TAILQ_EMPTY(&ttransport->poll_groups)) {
		return NULL;
	}

	pg = &ttransport->next_pg;
	assert(*pg != NULL);
	hint = (*pg)->sock_group;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	rc = spdk_sock_get_optimal_sock_group(tqpair->sock, &group, hint);
	if (rc != 0) {
		return NULL;
	} else if (group != NULL) {
		/* Optimal poll group was found */
		return spdk_sock_group_get_ctx(group);
	}

	/* The hint was used for optimal poll group, advance next_pg. */
	*pg = TAILQ_NEXT(*pg, link);
	if (*pg == NULL) {
		*pg = TAILQ_FIRST(&ttransport->poll_groups);
	}

	return spdk_sock_group_get_ctx(hint);
}

static void
nvmf_tcp_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_tcp_poll_group *tgroup, *next_tgroup;
	struct spdk_nvmf_tcp_transport *ttransport;
	int rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	spdk_interrupt_unregister(&tgroup->intr);
	rc = spdk_sock_group_close(&tgroup->sock_group);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_group_close() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		assert(false);
	}

	if (tgroup->control_msg_list) {
		nvmf_tcp_control_msg_list_free(tgroup->control_msg_list);
	}

	if (tgroup->accel_channel) {
		spdk_put_io_channel(tgroup->accel_channel);
	}

	if (tgroup->group.transport == NULL) {
		/* Transport can be NULL when nvmf_tcp_poll_group_create()
		 * calls this function directly in a failure path. */
		free(tgroup);
		return;
	}

	ttransport = SPDK_CONTAINEROF(tgroup->group.transport, struct spdk_nvmf_tcp_transport, transport);

	next_tgroup = TAILQ_NEXT(tgroup, link);
	TAILQ_REMOVE(&ttransport->poll_groups, tgroup, link);
	if (next_tgroup == NULL) {
		next_tgroup = TAILQ_FIRST(&ttransport->poll_groups);
	}
	if (ttransport->next_pg == tgroup) {
		ttransport->next_pg = next_tgroup;
	}

	free(tgroup);
}

static void
nvmf_tcp_qpair_set_recv_state(struct spdk_nvmf_tcp_qpair *tqpair,
			      enum nvme_tcp_pdu_recv_state state)
{
	if (tqpair->recv_state == state) {
		SPDK_ERRLOG("The recv state of tqpair=%p is same with the state(%d) to be set\n",
			    tqpair, state);
		return;
	}

	if (spdk_unlikely(state == NVME_TCP_PDU_RECV_STATE_QUIESCING)) {
		if (tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH && tqpair->pdu_in_progress) {
			SLIST_INSERT_HEAD(&tqpair->tcp_pdu_free_queue, tqpair->pdu_in_progress, slist);
			tqpair->tcp_pdu_working_count--;
		}
	}

	if (spdk_unlikely(state == NVME_TCP_PDU_RECV_STATE_ERROR)) {
		assert(tqpair->tcp_pdu_working_count == 0);
	}

	SPDK_DEBUGLOG(nvmf_tcp, "tqpair(%p) recv state=%d\n", tqpair, state);
	tqpair->recv_state = state;

	spdk_trace_record(TRACE_TCP_QP_RCV_STATE_CHANGE, tqpair->qpair.trace_id, 0, 0,
			  (uint64_t)tqpair->recv_state);
}

static int
nvmf_tcp_qpair_handle_timeout(void *ctx)
{
	struct spdk_nvmf_tcp_qpair *tqpair = ctx;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);

	SPDK_ERRLOG("No pdu coming for tqpair=%p within %d seconds\n", tqpair,
		    SPDK_NVME_TCP_QPAIR_EXIT_TIMEOUT);

	nvmf_tcp_qpair_disconnect(tqpair);
	return SPDK_POLLER_BUSY;
}

static void
nvmf_tcp_send_c2h_term_req_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair = (struct spdk_nvmf_tcp_qpair *)cb_arg;

	if (!tqpair->timeout_poller) {
		tqpair->timeout_poller = SPDK_POLLER_REGISTER(nvmf_tcp_qpair_handle_timeout, tqpair,
					 SPDK_NVME_TCP_QPAIR_EXIT_TIMEOUT * 1000000);
	}
}

static void
nvmf_tcp_send_c2h_term_req(struct spdk_nvmf_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
			   enum spdk_nvme_tcp_term_req_fes fes, uint32_t error_offset)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_term_req_hdr *c2h_term_req;
	uint32_t c2h_term_req_hdr_len = sizeof(*c2h_term_req);
	uint32_t copy_len;

	rsp_pdu = tqpair->mgmt_pdu;

	c2h_term_req = &rsp_pdu->hdr.term_req;
	c2h_term_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ;
	c2h_term_req->common.hlen = c2h_term_req_hdr_len;
	c2h_term_req->fes = fes;

	if ((fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		DSET32(&c2h_term_req->fei, error_offset);
	}

	copy_len = spdk_min(pdu->hdr.common.hlen, SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE);

	/* Copy the error info into the buffer */
	memcpy((uint8_t *)rsp_pdu->hdr.raw + c2h_term_req_hdr_len, pdu->hdr.raw, copy_len);
	nvme_tcp_pdu_set_data(rsp_pdu, (uint8_t *)rsp_pdu->hdr.raw + c2h_term_req_hdr_len, copy_len);

	/* Contain the header of the wrong received pdu */
	c2h_term_req->common.plen = c2h_term_req->common.hlen + copy_len;
	tqpair->wait_terminate = true;
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
	nvmf_tcp_qpair_write_mgmt_pdu(tqpair, nvmf_tcp_send_c2h_term_req_complete, tqpair);
}

/*
 * [한국어]
 * nvmf_tcp_capsule_cmd_hdr_handle - CapsuleCmd PDU의 헤더(NVMe SQE) 수신 완료 핸들러
 *
 * @ttransport: TCP transport
 * @tqpair:     수신한 qpair
 * @pdu:        헤더 수신이 끝난 PDU (in_progress)
 *
 * NVMe-TCP의 CapsuleCmd는 호스트가 보내는 NVMe 명령 PDU.
 * 헤더에는 64-byte SQE가 포함되며, 이 함수는:
 *   1) free queue에서 새 req 객체 alloc → NEW 상태
 *   2) PDU에 req 연결 (payload 수신 시 어디에 쓸지 결정용)
 *   3) nvmf_tcp_req_process로 상태 머신 진행
 *
 * QD 도달로 alloc 실패 시 QUIESCING으로 전환 (호스트가 over-issue).
 */
static void
nvmf_tcp_capsule_cmd_hdr_handle(struct spdk_nvmf_tcp_transport *ttransport,
				struct spdk_nvmf_tcp_qpair *tqpair,
				struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_req *tcp_req;

	assert(pdu->psh_valid_bytes == pdu->psh_len);
	assert(pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD);

	tcp_req = nvmf_tcp_req_get(tqpair);
	if (!tcp_req) {
		/* Directly return and make the allocation retry again.  This can happen if we're
		 * using asynchronous writes to send the response to the host or when releasing
		 * zero-copy buffers after a response has been sent.  In both cases, the host might
		 * receive the response before we've finished processing the request and is free to
		 * send another one.
		 */
		if (tqpair->state_cntr[TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST] > 0 ||
		    tqpair->state_cntr[TCP_REQUEST_STATE_AWAITING_ZCOPY_RELEASE] > 0) {
			return;
		}

		/* The host sent more commands than the maximum queue depth. */
		SPDK_ERRLOG("Cannot allocate tcp_req on tqpair=%p\n", tqpair);
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
		return;
	}

	pdu->req = tcp_req;
	assert(tcp_req->state == TCP_REQUEST_STATE_NEW);
	nvmf_tcp_req_process(ttransport, tcp_req);
}

/*
 * [한국어]
 * nvmf_tcp_capsule_cmd_payload_handle - CapsuleCmd PDU의 payload(in-capsule data) 수신 완료 핸들러
 *
 * 헤더가 H2C 가능을 표시하면 payload는 in-capsule data — 즉시 사용 가능.
 * 그렇지 않으면 R2T 송신이 필요. 다음을 수행:
 *   1) pdu->pdo 헤더 필드 검증 (out-of-range는 TermReq로 거부)
 *   2) 직전 단계에서 status가 transport error로 설정되어 있으면 곧장 COMPLETE
 *   3) 정상이면 READY_TO_EXECUTE로 전이 후 req_process
 *
 * Zero-copy 요청은 in-capsule data를 지원하지 않음 (assert).
 */
static void
nvmf_tcp_capsule_cmd_payload_handle(struct spdk_nvmf_tcp_transport *ttransport,
				    struct spdk_nvmf_tcp_qpair *tqpair,
				    struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_req *tcp_req;
	struct spdk_nvme_tcp_cmd *capsule_cmd;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	struct spdk_nvme_cpl *rsp;

	capsule_cmd = &pdu->hdr.capsule_cmd;
	tcp_req = pdu->req;
	assert(tcp_req != NULL);

	/* Zero-copy requests don't support ICD */
	assert(!spdk_nvmf_request_using_zcopy(&tcp_req->req));

	if (capsule_cmd->common.pdo > SPDK_NVME_TCP_PDU_PDO_MAX_OFFSET) {
		SPDK_ERRLOG("Expected ICReq capsule_cmd pdu offset <= %d, got %c\n",
			    SPDK_NVME_TCP_PDU_PDO_MAX_OFFSET, capsule_cmd->common.pdo);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdo);
		goto err;
	}

	rsp = &tcp_req->req.rsp->nvme_cpl;
	if (spdk_unlikely(rsp->status.sc == SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR)) {
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
	} else {
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
	}

	nvmf_tcp_req_process(ttransport, tcp_req);

	return;
err:
	nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

/*
 * [한국어]
 * nvmf_tcp_h2c_data_hdr_handle - H2C Data PDU의 헤더 수신 완료 핸들러
 *
 * H2C(Host-to-Controller) Data PDU는 호스트가 R2T 응답으로 보내는 write 데이터.
 * 헤더에는 ttag(transfer tag), cccid, datao(offset), datal(length) 포함.
 * 이 함수는:
 *   1) ttag 유효성 검증 (resource_count 이내) — invalid면 TermReq
 *   2) ttag로 reqs 배열에서 해당 req 복원
 *   3) req 상태가 H2C 대기 중인지 검증
 *   4) cccid 일치 검증
 *   5) datao가 직전 누적 h2c_offset과 일치하는지 (out-of-order 검출)
 *   6) datao+datal이 req 전체 길이를 초과하지 않는지
 *   7) 정상이면 PDU의 데이터 buffer를 req->iov로 설정 → payload 수신 시 직접 채워짐
 *
 * 와이어 검증 실패 시 모두 TermReq(C2H)로 전환.
 */
static void
nvmf_tcp_h2c_data_hdr_handle(struct spdk_nvmf_tcp_transport *ttransport,
			     struct spdk_nvmf_tcp_qpair *tqpair,
			     struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_req *tcp_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes = 0;
	struct spdk_nvme_tcp_h2c_data_hdr *h2c_data;

	h2c_data = &pdu->hdr.h2c_data;

	SPDK_DEBUGLOG(nvmf_tcp, "tqpair=%p, r2t_info: datao=%u, datal=%u, cccid=%u, ttag=%u\n",
		      tqpair, h2c_data->datao, h2c_data->datal, h2c_data->cccid, h2c_data->ttag);

	if (h2c_data->ttag > tqpair->resource_count) {
		SPDK_DEBUGLOG(nvmf_tcp, "ttag %u is larger than allowed %u.\n", h2c_data->ttag,
			      tqpair->resource_count);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
		error_offset = offsetof(struct spdk_nvme_tcp_h2c_data_hdr, ttag);
		goto err;
	}

	tcp_req = &tqpair->reqs[h2c_data->ttag - 1];

	if (spdk_unlikely(tcp_req->state != TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER &&
			  tcp_req->state != TCP_REQUEST_STATE_AWAITING_R2T_ACK)) {
		SPDK_DEBUGLOG(nvmf_tcp, "tcp_req(%p), tqpair=%p, has error state in %d\n", tcp_req, tqpair,
			      tcp_req->state);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_h2c_data_hdr, ttag);
		goto err;
	}

	if (spdk_unlikely(tcp_req->req.cmd->nvme_cmd.cid != h2c_data->cccid)) {
		SPDK_DEBUGLOG(nvmf_tcp, "tcp_req(%p), tqpair=%p, expected %u but %u for cccid.\n", tcp_req, tqpair,
			      tcp_req->req.cmd->nvme_cmd.cid, h2c_data->cccid);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
		error_offset = offsetof(struct spdk_nvme_tcp_h2c_data_hdr, cccid);
		goto err;
	}

	if (tcp_req->h2c_offset != h2c_data->datao) {
		SPDK_DEBUGLOG(nvmf_tcp,
			      "tcp_req(%p), tqpair=%p, expected data offset %u, but data offset is %u\n",
			      tcp_req, tqpair, tcp_req->h2c_offset, h2c_data->datao);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		goto err;
	}

	if ((h2c_data->datao + h2c_data->datal) > tcp_req->req.length) {
		SPDK_DEBUGLOG(nvmf_tcp,
			      "tcp_req(%p), tqpair=%p,  (datao=%u + datal=%u) exceeds requested length=%u\n",
			      tcp_req, tqpair, h2c_data->datao, h2c_data->datal, tcp_req->req.length);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		goto err;
	}

	pdu->req = tcp_req;

	if (spdk_unlikely(tcp_req->req.dif_enabled)) {
		pdu->dif_ctx = &tcp_req->req.dif.dif_ctx;
	}

	nvme_tcp_pdu_set_data_buf(pdu, tcp_req->req.iov, tcp_req->req.iovcnt,
				  h2c_data->datao, h2c_data->datal);
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;

err:
	nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

/*
 * [한국어]
 * nvmf_tcp_send_capsule_resp_pdu - NVMe CQE를 CapsuleResp PDU로 송신
 *
 * @tcp_req: 응답 보낼 요청 (rsp.nvme_cpl에 NVMe CQE 들어 있음)
 * @tqpair:  송신 경로의 qpair
 *
 * NVMe-TCP CapsuleResp PDU는 16-byte NVMe CQE를 헤더 payload로 포함.
 * read의 경우 C2H Data PDU에 SUCCESS flag로 CQE 생략 가능 (success optimization).
 * write 등은 본 PDU로 status 통지. 헤더만 있고 payload는 없음 (HDgst 옵션).
 *
 * 송신 큐잉 후 완료 콜백 nvmf_tcp_request_free에서 req를 free queue로 반환.
 */
static void
nvmf_tcp_send_capsule_resp_pdu(struct spdk_nvmf_tcp_req *tcp_req,
			       struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_rsp *capsule_resp;

	SPDK_DEBUGLOG(nvmf_tcp, "enter, tqpair=%p\n", tqpair);

	rsp_pdu = nvmf_tcp_req_pdu_init(tcp_req);
	assert(rsp_pdu != NULL);

	capsule_resp = &rsp_pdu->hdr.capsule_resp;
	capsule_resp->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP;
	capsule_resp->common.plen = capsule_resp->common.hlen = sizeof(*capsule_resp);
	capsule_resp->rccqe = tcp_req->req.rsp->nvme_cpl;
	if (tqpair->host_hdgst_enable) {
		capsule_resp->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		capsule_resp->common.plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	nvmf_tcp_qpair_write_req_pdu(tqpair, tcp_req, nvmf_tcp_request_free, tcp_req);
}

static void
nvmf_tcp_pdu_c2h_data_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_req *tcp_req = cb_arg;
	struct spdk_nvmf_tcp_qpair *tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair,
					     struct spdk_nvmf_tcp_qpair, qpair);

	assert(tqpair != NULL);

	if (spdk_unlikely(tcp_req->pdu->rw_offset < tcp_req->req.length)) {
		SPDK_DEBUGLOG(nvmf_tcp, "sending another C2H part, offset %u length %u\n", tcp_req->pdu->rw_offset,
			      tcp_req->req.length);
		_nvmf_tcp_send_c2h_data(tqpair, tcp_req);
		return;
	}

	if (tcp_req->pdu->hdr.c2h_data.common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) {
		nvmf_tcp_request_free(tcp_req);
	} else {
		nvmf_tcp_send_capsule_resp_pdu(tcp_req, tqpair);
	}
}

static void
nvmf_tcp_r2t_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_req *tcp_req = cb_arg;
	struct spdk_nvmf_tcp_transport *ttransport;

	ttransport = SPDK_CONTAINEROF(tcp_req->req.qpair->transport,
				      struct spdk_nvmf_tcp_transport, transport);

	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);

	if (tcp_req->h2c_offset == tcp_req->req.length) {
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
		nvmf_tcp_req_process(ttransport, tcp_req);
	}
}

/*
 * [한국어]
 * nvmf_tcp_send_r2t_pdu - R2T(Ready to Transfer) PDU 송신
 *
 * R2T는 호스트에게 "이 cccid/ttag로 r2to부터 r2tl 바이트의 H2C Data를 보내줘"
 * 라고 요청하는 PDU. write 데이터가 in-capsule data 한도를 초과할 때 송신.
 * 호스트는 응답으로 H2C Data PDU(들)을 보낸다.
 *
 * 송신 후 req 상태를 AWAITING_R2T_ACK로 전이. 완료 콜백 r2t_complete에서
 * H2C 누적 수신이 끝났는지 확인 후 다음 단계로 진행.
 */
static void
nvmf_tcp_send_r2t_pdu(struct spdk_nvmf_tcp_qpair *tqpair,
		      struct spdk_nvmf_tcp_req *tcp_req)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_r2t_hdr *r2t;

	rsp_pdu = nvmf_tcp_req_pdu_init(tcp_req);
	assert(rsp_pdu != NULL);

	r2t = &rsp_pdu->hdr.r2t;
	r2t->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_R2T;
	r2t->common.plen = r2t->common.hlen = sizeof(*r2t);

	if (tqpair->host_hdgst_enable) {
		r2t->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		r2t->common.plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	r2t->cccid = tcp_req->req.cmd->nvme_cmd.cid;
	r2t->ttag = tcp_req->ttag;
	r2t->r2to = tcp_req->h2c_offset;
	r2t->r2tl = tcp_req->req.length;

	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_AWAITING_R2T_ACK);

	SPDK_DEBUGLOG(nvmf_tcp,
		      "tcp_req(%p) on tqpair(%p), r2t_info: cccid=%u, ttag=%u, r2to=%u, r2tl=%u\n",
		      tcp_req, tqpair, r2t->cccid, r2t->ttag, r2t->r2to, r2t->r2tl);
	nvmf_tcp_qpair_write_req_pdu(tqpair, tcp_req, nvmf_tcp_r2t_complete, tcp_req);
}

/*
 * [한국어]
 * nvmf_tcp_h2c_data_payload_handle - H2C Data PDU의 payload(write 데이터) 수신 완료 핸들러
 *
 * 호스트가 보낸 데이터 청크가 모두 수신됨. h2c_offset += datal로 누적.
 * 누적이 req 전체 길이에 도달하면 READY_TO_EXECUTE 상태로 전이하고
 * req_process로 진행 — 다음 단계는 spdk_nvmf_request_exec(bdev에 IO 디스패치).
 *
 * H2C가 여러 PDU에 쪼개져 올 수 있으므로 partial 시에는 다음 PDU 대기.
 */
static void
nvmf_tcp_h2c_data_payload_handle(struct spdk_nvmf_tcp_transport *ttransport,
				 struct spdk_nvmf_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_req *tcp_req;
	struct spdk_nvme_cpl *rsp;

	tcp_req = pdu->req;
	assert(tcp_req != NULL);

	SPDK_DEBUGLOG(nvmf_tcp, "enter\n");

	tcp_req->h2c_offset += pdu->data_len;

	/* Wait for all of the data to arrive AND for the initial R2T PDU send to be
	 * acknowledged before moving on. */
	if (tcp_req->h2c_offset == tcp_req->req.length &&
	    tcp_req->state == TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER) {
		/* After receiving all the h2c data, we need to check whether there is
		 * transient transport error */
		rsp = &tcp_req->req.rsp->nvme_cpl;
		if (spdk_unlikely(rsp->status.sc == SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR)) {
			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
		} else {
			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
		}
		nvmf_tcp_req_process(ttransport, tcp_req);
	}
}

static void
nvmf_tcp_h2c_term_req_dump(struct spdk_nvme_tcp_term_req_hdr *h2c_term_req)
{
	SPDK_ERRLOG("Error info of pdu(%p): %s\n", h2c_term_req,
		    spdk_nvmf_tcp_term_req_fes_str[h2c_term_req->fes]);
	if ((h2c_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (h2c_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		SPDK_DEBUGLOG(nvmf_tcp, "The offset from the start of the PDU header is %u\n",
			      DGET32(h2c_term_req->fei));
	}
}

static void
nvmf_tcp_h2c_term_req_hdr_handle(struct spdk_nvmf_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req = &pdu->hdr.term_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	if (h2c_term_req->fes > SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER) {
		SPDK_ERRLOG("Fatal Error Status(FES) is unknown for h2c_term_req pdu=%p\n", pdu);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_term_req_hdr, fes);
		goto end;
	}

	/* set the data buffer */
	nvme_tcp_pdu_set_data(pdu, (uint8_t *)pdu->hdr.raw + h2c_term_req->common.hlen,
			      h2c_term_req->common.plen - h2c_term_req->common.hlen);
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;
end:
	nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvmf_tcp_h2c_term_req_payload_handle(struct spdk_nvmf_tcp_qpair *tqpair,
				     struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req = &pdu->hdr.term_req;

	nvmf_tcp_h2c_term_req_dump(h2c_term_req);
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
}

static void
_nvmf_tcp_pdu_payload_handle(struct spdk_nvmf_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_transport *ttransport = SPDK_CONTAINEROF(tqpair->qpair.transport,
			struct spdk_nvmf_tcp_transport, transport);

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
		nvmf_tcp_capsule_cmd_payload_handle(ttransport, tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
		nvmf_tcp_h2c_data_payload_handle(ttransport, tqpair, pdu);
		break;

	case SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ:
		nvmf_tcp_h2c_term_req_payload_handle(tqpair, pdu);
		break;

	default:
		/* The code should not go to here */
		SPDK_ERRLOG("ERROR pdu type %d\n", pdu->hdr.common.pdu_type);
		break;
	}
	SLIST_INSERT_HEAD(&tqpair->tcp_pdu_free_queue, pdu, slist);
	tqpair->tcp_pdu_working_count--;
}

static inline void
nvmf_tcp_req_set_cpl(struct spdk_nvmf_tcp_req *treq, int sct, int sc)
{
	treq->req.rsp->nvme_cpl.status.sct = sct;
	treq->req.rsp->nvme_cpl.status.sc = sc;
	treq->req.rsp->nvme_cpl.cid = treq->req.cmd->nvme_cmd.cid;
}

static void
data_crc32_calc_done(void *cb_arg, int status)
{
	struct nvme_tcp_pdu *pdu = cb_arg;
	struct spdk_nvmf_tcp_qpair *tqpair = pdu->qpair;

	/* async crc32 calculation is failed and use direct calculation to check */
	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("Data digest on tqpair=(%p) with pdu=%p failed to be calculated asynchronously\n",
			    tqpair, pdu);
		pdu->data_digest_crc32 = nvme_tcp_pdu_calc_data_digest(pdu);
	}
	pdu->data_digest_crc32 ^= SPDK_CRC32C_XOR;
	if (!MATCH_DIGEST_WORD(pdu->data_digest, pdu->data_digest_crc32)) {
		SPDK_ERRLOG("Data digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
		assert(pdu->req != NULL);
		nvmf_tcp_req_set_cpl(pdu->req, SPDK_NVME_SCT_GENERIC,
				     SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR);
	}
	_nvmf_tcp_pdu_payload_handle(tqpair, pdu);
}

static void
nvmf_tcp_pdu_payload_handle(struct spdk_nvmf_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	int rc = 0;
	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	tqpair->pdu_in_progress = NULL;
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	SPDK_DEBUGLOG(nvmf_tcp, "enter\n");
	/* check data digest if need */
	if (pdu->ddgst_enable) {
		if (tqpair->qpair.qid != 0 && !pdu->dif_ctx && tqpair->group &&
		    (pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT == 0)) {
			rc = spdk_accel_submit_crc32cv(tqpair->group->accel_channel, &pdu->data_digest_crc32, pdu->data_iov,
						       pdu->data_iovcnt, 0, data_crc32_calc_done, pdu);
			if (spdk_likely(rc == 0)) {
				return;
			}
		} else {
			pdu->data_digest_crc32 = nvme_tcp_pdu_calc_data_digest(pdu);
		}
		data_crc32_calc_done(pdu, rc);
	} else {
		_nvmf_tcp_pdu_payload_handle(tqpair, pdu);
	}
}

static void
nvmf_tcp_send_icresp_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair = cb_arg;

	nvmf_tcp_qpair_set_state(tqpair, NVMF_TCP_QPAIR_STATE_RUNNING);
}

static void
nvmf_tcp_icreq_handle(struct spdk_nvmf_tcp_transport *ttransport,
		      struct spdk_nvmf_tcp_qpair *tqpair,
		      struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_ic_req *ic_req = &pdu->hdr.ic_req;
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_ic_resp *ic_resp;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	int rc;

	/* Only PFV 0 is defined currently */
	if (ic_req->pfv != 0) {
		SPDK_ERRLOG("Expected ICReq PFV %u, got %u\n", 0u, ic_req->pfv);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_req, pfv);
		goto end;
	}

	/* This value is 0’s based value in units of dwords should not be larger than SPDK_NVME_TCP_HPDA_MAX */
	if (ic_req->hpda > SPDK_NVME_TCP_HPDA_MAX) {
		SPDK_ERRLOG("ICReq HPDA out of range 0 to 31, got %u\n", ic_req->hpda);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_req, hpda);
		goto end;
	}

	/* MAXR2T is 0's based */
	SPDK_DEBUGLOG(nvmf_tcp, "maxr2t =%u\n", (ic_req->maxr2t + 1u));

	tqpair->host_hdgst_enable = ic_req->dgst.bits.hdgst_enable ? true : false;
	if (!tqpair->host_hdgst_enable) {
		tqpair->recv_buf_size -= SPDK_NVME_TCP_DIGEST_LEN * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR;
	}

	tqpair->host_ddgst_enable = ic_req->dgst.bits.ddgst_enable ? true : false;
	if (!tqpair->host_ddgst_enable) {
		tqpair->recv_buf_size -= SPDK_NVME_TCP_DIGEST_LEN * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR;
	}

	tqpair->recv_buf_size = spdk_max(tqpair->recv_buf_size, MIN_SOCK_PIPE_SIZE);
	/* Now that we know whether digests are enabled, properly size the receive buffer */
	rc = spdk_sock_set_recvbuf(tqpair->sock, tqpair->recv_buf_size);
	if (rc < 0) {
		SPDK_WARNLOG("spdk_sock_set_recvbuf() failed, rc %d: %s. Unable to allocate enough memory for receive buffer on tqpair=%p with size=%d\n",
			     rc, spdk_strerror(-rc), tqpair, tqpair->recv_buf_size);
		/* Not fatal. */
	}

	tqpair->cpda = spdk_min(ic_req->hpda, SPDK_NVME_TCP_CPDA_MAX);
	SPDK_DEBUGLOG(nvmf_tcp, "cpda of tqpair=(%p) is : %u\n", tqpair, tqpair->cpda);

	rsp_pdu = tqpair->mgmt_pdu;

	ic_resp = &rsp_pdu->hdr.ic_resp;
	ic_resp->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_RESP;
	ic_resp->common.hlen = ic_resp->common.plen =  sizeof(*ic_resp);
	ic_resp->pfv = 0;
	ic_resp->cpda = tqpair->cpda;
	ic_resp->maxh2cdata = ttransport->transport.opts.max_io_size;
	ic_resp->dgst.bits.hdgst_enable = tqpair->host_hdgst_enable ? 1 : 0;
	ic_resp->dgst.bits.ddgst_enable = tqpair->host_ddgst_enable ? 1 : 0;

	SPDK_DEBUGLOG(nvmf_tcp, "host_hdgst_enable: %u\n", tqpair->host_hdgst_enable);
	SPDK_DEBUGLOG(nvmf_tcp, "host_ddgst_enable: %u\n", tqpair->host_ddgst_enable);

	nvmf_tcp_qpair_set_state(tqpair, NVMF_TCP_QPAIR_STATE_INITIALIZING);
	nvmf_tcp_qpair_write_mgmt_pdu(tqpair, nvmf_tcp_send_icresp_complete, tqpair);
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	return;
end:
	nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvmf_tcp_pdu_psh_handle(struct spdk_nvmf_tcp_qpair *tqpair,
			struct spdk_nvmf_tcp_transport *ttransport)
{
	struct nvme_tcp_pdu *pdu;
	int rc;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
	pdu = tqpair->pdu_in_progress;

	SPDK_DEBUGLOG(nvmf_tcp, "pdu type of tqpair(%p) is %d\n", tqpair,
		      pdu->hdr.common.pdu_type);
	/* check header digest if needed */
	if (pdu->has_hdgst) {
		SPDK_DEBUGLOG(nvmf_tcp, "Compare the header of pdu=%p on tqpair=%p\n", pdu, tqpair);
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		rc = MATCH_DIGEST_WORD((uint8_t *)pdu->hdr.raw + pdu->hdr.common.hlen, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("Header digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_HDGST_ERROR;
			nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
			return;

		}
	}

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_IC_REQ:
		nvmf_tcp_icreq_handle(ttransport, tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_REQ);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
		nvmf_tcp_h2c_data_hdr_handle(ttransport, tqpair, pdu);
		break;

	case SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ:
		nvmf_tcp_h2c_term_req_hdr_handle(tqpair, pdu);
		break;

	default:
		SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->pdu_in_progress->hdr.common.pdu_type);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = 1;
		nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
		break;
	}
}

static void
nvmf_tcp_pdu_ch_handle(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *pdu;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	uint8_t expected_hlen, pdo;
	bool plen_error = false, pdo_error = false;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
	pdu = tqpair->pdu_in_progress;
	assert(pdu);
	if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_REQ) {
		if (tqpair->state != NVMF_TCP_QPAIR_STATE_INVALID) {
			SPDK_ERRLOG("Already received ICreq PDU, and reject this pdu=%p\n", pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}
		expected_hlen = sizeof(struct spdk_nvme_tcp_ic_req);
		if (pdu->hdr.common.plen != expected_hlen) {
			plen_error = true;
		}
	} else {
		if (tqpair->state != NVMF_TCP_QPAIR_STATE_RUNNING) {
			SPDK_ERRLOG("The TCP/IP connection is not negotiated\n");
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}

		switch (pdu->hdr.common.pdu_type) {
		case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
			expected_hlen = sizeof(struct spdk_nvme_tcp_cmd);
			pdo = pdu->hdr.common.pdo;
			if ((tqpair->cpda != 0) && (pdo % ((tqpair->cpda + 1) << 2) != 0)) {
				pdo_error = true;
				break;
			}

			if (pdu->hdr.common.plen < expected_hlen) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
			expected_hlen = sizeof(struct spdk_nvme_tcp_h2c_data_hdr);
			pdo = pdu->hdr.common.pdo;
			if ((tqpair->cpda != 0) && (pdo % ((tqpair->cpda + 1) << 2) != 0)) {
				pdo_error = true;
				break;
			}
			if (pdu->hdr.common.plen < expected_hlen) {
				plen_error = true;
			}
			break;

		case SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ:
			expected_hlen = sizeof(struct spdk_nvme_tcp_term_req_hdr);
			if ((pdu->hdr.common.plen <= expected_hlen) ||
			    (pdu->hdr.common.plen > SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE)) {
				plen_error = true;
			}
			break;

		default:
			SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", pdu->hdr.common.pdu_type);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdu_type);
			goto err;
		}
	}

	if (pdu->hdr.common.hlen != expected_hlen) {
		SPDK_ERRLOG("PDU type=0x%02x, Expected ICReq header length %u, got %u on tqpair=%p\n",
			    pdu->hdr.common.pdu_type,
			    expected_hlen, pdu->hdr.common.hlen, tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, hlen);
		goto err;
	} else if (pdo_error) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdo);
	} else if (plen_error) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, plen);
		goto err;
	} else {
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
		nvme_tcp_pdu_calc_psh_len(tqpair->pdu_in_progress, tqpair->host_hdgst_enable);
		return;
	}
err:
	nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

/*
 * [한국어]
 * nvmf_tcp_sock_process - qpair의 PDU 수신 상태 머신 진행 (수신 데이터 plane 본체)
 *
 * @tqpair: 처리할 TCP qpair
 * @return: NVME_TCP_PDU_IN_PROGRESS=다음 polling 대기, 0=완료, 음수=오류
 *
 * 소켓에서 데이터가 도착했을 때(또는 polling tick마다) 호출되어 다음 단계들을
 * 수행. 한 번 호출에서 여러 PDU를 연속 처리 가능하도록 do-while 루프 사용.
 *
 * PDU 수신 상태 머신:
 *   AWAIT_PDU_READY (free PDU 슬롯 확보)
 *     → AWAIT_PDU_CH (8-byte common header 수신)
 *     → AWAIT_PDU_PSH (PDU-specific header 수신, HDgst 포함)
 *     → AWAIT_REQ (CapsuleCmd만: req 슬롯 확보)
 *     → AWAIT_PDU_BUF (req의 데이터 buffer 할당 대기)
 *     → AWAIT_PDU_PAYLOAD (payload 데이터 수신, DDgst 포함)
 *     → AWAIT_PDU_READY (다음 PDU)
 *
 * 각 단계에서 nvme_tcp_read_data로 소켓에서 read하고 헤더/payload를 누적.
 * 누적이 완료되면 해당 핸들러(_ch_handle, _psh_handle 등)로 디스패치.
 *
 * 호출 체인:
 *   nvmf_tcp_sock_cb (sock_group 콜백) → [nvmf_tcp_sock_process]
 *     → nvme_tcp_read_data (sock readv)
 *     → nvmf_tcp_pdu_ch_handle / pdu_psh_handle / capsule_cmd_payload_handle / ...
 */
static int
nvmf_tcp_sock_process(struct spdk_nvmf_tcp_qpair *tqpair)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu;
	enum nvme_tcp_pdu_recv_state prev_state;	/* [한국어] 무한 루프 방지용 — 이전 상태와 동일하면 break */
	uint32_t data_len;
	struct spdk_nvmf_tcp_transport *ttransport = SPDK_CONTAINEROF(tqpair->qpair.transport,
			struct spdk_nvmf_tcp_transport, transport);	/* [한국어] 다운캐스트로 ttransport 획득 */

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = tqpair->recv_state;	/* [한국어] 진행 여부 판단용 스냅샷 */
		SPDK_DEBUGLOG(nvmf_tcp, "tqpair(%p) recv pdu entering state %d\n", tqpair, prev_state);

		pdu = tqpair->pdu_in_progress;
		assert(pdu != NULL ||
		       tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY ||
		       tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING ||
		       tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
		/* [한국어] pdu_in_progress가 NULL일 수 있는 상태는 위 3가지뿐 — 일관성 검증 */

		switch (tqpair->recv_state) {
		/* Wait for the common header  */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
			if (!pdu) {						/* [한국어] free PDU 슬롯 확보 (R2T/Data 송신과 공유) */
				pdu = SLIST_FIRST(&tqpair->tcp_pdu_free_queue);
				if (spdk_unlikely(!pdu)) {
					return NVME_TCP_PDU_IN_PROGRESS;	/* [한국어] free PDU 부족 — 다음 polling에 재시도 */
				}
				SLIST_REMOVE_HEAD(&tqpair->tcp_pdu_free_queue, slist);
				tqpair->pdu_in_progress = pdu;
				tqpair->tcp_pdu_working_count++;
			}
			memset(pdu, 0, offsetof(struct nvme_tcp_pdu, qpair));	/* [한국어] qpair 필드 보존 + 나머지 클리어 */
			nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);	/* [한국어] CH 수신 상태로 전이 */
		/* FALLTHROUGH */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH:
			if (spdk_unlikely(tqpair->state == NVMF_TCP_QPAIR_STATE_INITIALIZING)) {
				return rc;					/* [한국어] init 중에는 PDU 처리 보류 — ICReq/Resp 단계 후 RUNNING으로 전이 후 처리 */
			}

			rc = nvme_tcp_read_data(tqpair->sock,
						sizeof(struct spdk_nvme_tcp_common_pdu_hdr) - pdu->ch_valid_bytes,
						(void *)&pdu->hdr.common + pdu->ch_valid_bytes);
			/* [한국어] 8-byte common header 부분 read — 누적 위치는 ch_valid_bytes로 추적 */
			if (rc < 0) {
				SPDK_DEBUGLOG(nvmf_tcp, "will disconnect tqpair=%p\n", tqpair);
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
				break;					/* [한국어] sock 오류 — disconnect 절차 진입 */
			} else if (rc > 0) {
				pdu->ch_valid_bytes += rc;
				spdk_trace_record(TRACE_TCP_READ_FROM_SOCKET_DONE, tqpair->qpair.trace_id, rc, 0);
			}

			if (pdu->ch_valid_bytes < sizeof(struct spdk_nvme_tcp_common_pdu_hdr)) {
				return NVME_TCP_PDU_IN_PROGRESS;	/* [한국어] 헤더 partial — 다음에 이어 받음 */
			}

			/* The command header of this PDU has now been read from the socket. */
			nvmf_tcp_pdu_ch_handle(tqpair);			/* [한국어] common header 수신 완료 — 검증 + 다음 단계 결정 */
			break;
		/* Wait for the pdu specific header  */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH:
			rc = nvme_tcp_read_data(tqpair->sock,
						pdu->psh_len - pdu->psh_valid_bytes,
						(void *)&pdu->hdr.raw + sizeof(struct spdk_nvme_tcp_common_pdu_hdr) + pdu->psh_valid_bytes);
			if (rc < 0) {
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
				break;
			} else if (rc > 0) {
				spdk_trace_record(TRACE_TCP_READ_FROM_SOCKET_DONE, tqpair->qpair.trace_id, rc, 0);
				pdu->psh_valid_bytes += rc;
			}

			if (pdu->psh_valid_bytes < pdu->psh_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* All header(ch, psh, head digits) of this PDU has now been read from the socket. */
			nvmf_tcp_pdu_psh_handle(tqpair, ttransport);
			break;
		/* Wait for the req slot */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_REQ:
			nvmf_tcp_capsule_cmd_hdr_handle(ttransport, tqpair, pdu);
			break;
		/* Wait for the request processing loop to acquire a buffer for the PDU */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_BUF:
			break;
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
			/* check whether the data is valid, if not we just return */
			if (!pdu->data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			data_len = pdu->data_len;
			/* data digest */
			if (spdk_unlikely((pdu->hdr.common.pdu_type != SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ) &&
					  tqpair->host_ddgst_enable)) {
				data_len += SPDK_NVME_TCP_DIGEST_LEN;
				pdu->ddgst_enable = true;
			}

			rc = nvme_tcp_read_payload_data(tqpair->sock, pdu);
			if (rc < 0) {
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
				break;
			}
			pdu->rw_offset += rc;

			if (pdu->rw_offset < data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* Generate and insert DIF to whole data block received if DIF is enabled */
			if (spdk_unlikely(pdu->dif_ctx != NULL) &&
			    spdk_dif_generate_stream(pdu->data_iov, pdu->data_iovcnt, 0, data_len,
						     pdu->dif_ctx) != 0) {
				SPDK_ERRLOG("DIF generate failed\n");
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
				break;
			}

			/* All of this PDU has now been read from the socket. */
			nvmf_tcp_pdu_payload_handle(tqpair, pdu);
			break;
		case NVME_TCP_PDU_RECV_STATE_QUIESCING:
			if (tqpair->tcp_pdu_working_count != 0) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}
			nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
			break;
		case NVME_TCP_PDU_RECV_STATE_ERROR:
			if (spdk_sock_is_connected(tqpair->sock) && tqpair->wait_terminate) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}
			return NVME_TCP_PDU_FATAL;
		default:
			SPDK_ERRLOG("The state(%d) is invalid\n", tqpair->recv_state);
			abort();
			break;
		}
	} while (tqpair->recv_state != prev_state);

	return rc;
}

static inline void *
nvmf_tcp_control_msg_get(struct spdk_nvmf_tcp_control_msg_list *list,
			 struct spdk_nvmf_tcp_req *tcp_req)
{
	struct spdk_nvmf_tcp_control_msg *msg;

	assert(list);

	msg = STAILQ_FIRST(&list->free_msgs);
	if (!msg) {
		SPDK_DEBUGLOG(nvmf_tcp, "Out of control messages\n");
		STAILQ_INSERT_TAIL(&list->waiting_for_msg_reqs, tcp_req, control_msg_link);
		return NULL;
	}
	STAILQ_REMOVE_HEAD(&list->free_msgs, link);
	return msg;
}

static inline void
nvmf_tcp_control_msg_put(struct spdk_nvmf_tcp_control_msg_list *list, void *_msg)
{
	struct spdk_nvmf_tcp_control_msg *msg = _msg;
	struct spdk_nvmf_tcp_req *tcp_req;
	struct spdk_nvmf_tcp_transport *ttransport;

	assert(list);
	STAILQ_INSERT_HEAD(&list->free_msgs, msg, link);
	if (!STAILQ_EMPTY(&list->waiting_for_msg_reqs)) {
		tcp_req = STAILQ_FIRST(&list->waiting_for_msg_reqs);
		STAILQ_REMOVE_HEAD(&list->waiting_for_msg_reqs, control_msg_link);
		ttransport = SPDK_CONTAINEROF(tcp_req->req.qpair->transport,
					      struct spdk_nvmf_tcp_transport, transport);
		nvmf_tcp_req_process(ttransport, tcp_req);
	}
}

static void
nvmf_tcp_req_parse_sgl(struct spdk_nvmf_tcp_req *tcp_req,
		       struct spdk_nvmf_transport *transport,
		       struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_request		*req = &tcp_req->req;
	struct spdk_nvme_cmd			*cmd;
	struct spdk_nvme_sgl_descriptor		*sgl;
	struct spdk_nvmf_tcp_poll_group		*tgroup;
	enum spdk_nvme_tcp_term_req_fes		fes;
	struct nvme_tcp_pdu			*pdu;
	struct spdk_nvmf_tcp_qpair		*tqpair;
	uint32_t				length, error_offset = 0;

	cmd = &req->cmd->nvme_cmd;
	sgl = &cmd->dptr.sgl1;

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK &&
	    sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_TRANSPORT) {
		/* get request length from sgl */
		length = sgl->unkeyed.length;
		if (spdk_unlikely(length > transport->opts.max_io_size)) {
			SPDK_ERRLOG("SGL length 0x%x exceeds max io size 0x%x\n",
				    length, transport->opts.max_io_size);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED;
			goto fatal_err;
		}

		/* fill request length and populate iovs */
		req->length = length;

		SPDK_DEBUGLOG(nvmf_tcp, "Data requested length= 0x%x\n", length);

		if (spdk_unlikely(req->dif_enabled)) {
			req->dif.orig_length = length;
			length = spdk_dif_get_length_with_md(length, &req->dif.dif_ctx);
			req->dif.elba_length = length;
		}

		if (nvmf_ctrlr_use_zcopy(req)) {
			SPDK_DEBUGLOG(nvmf_tcp, "Using zero-copy to execute request %p\n", tcp_req);
			req->data_from_pool = false;
			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_HAVE_BUFFER);
			return;
		}

		if (spdk_nvmf_request_get_buffers(req, group, transport, length)) {
			/* No available buffers. Queue this request up. */
			SPDK_DEBUGLOG(nvmf_tcp, "No available large data buffers. Queueing request %p\n",
				      tcp_req);
			return;
		}

		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_HAVE_BUFFER);
		SPDK_DEBUGLOG(nvmf_tcp, "Request %p took %d buffer/s from central pool, and data=%p\n",
			      tcp_req, req->iovcnt, req->iov[0].iov_base);

		return;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		uint64_t offset = sgl->address;
		uint32_t max_len = transport->opts.in_capsule_data_size;

		assert(tcp_req->has_in_capsule_data);
		/* Capsule Cmd with In-capsule Data should get data length from pdu header */
		tqpair = tcp_req->pdu->qpair;
		/* receiving pdu is not same with the pdu in tcp_req */
		pdu = tqpair->pdu_in_progress;
		length = pdu->hdr.common.plen - pdu->psh_len - sizeof(struct spdk_nvme_tcp_common_pdu_hdr);
		if (tqpair->host_ddgst_enable) {
			length -= SPDK_NVME_TCP_DIGEST_LEN;
		}
		/* This error is not defined in NVMe/TCP spec, take this error as fatal error */
		if (spdk_unlikely(length != sgl->unkeyed.length)) {
			SPDK_ERRLOG("In-Capsule Data length 0x%x is not equal to SGL data length 0x%x\n",
				    length, sgl->unkeyed.length);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, plen);
			goto fatal_err;
		}

		SPDK_DEBUGLOG(nvmf_tcp, "In-capsule data: offset 0x%" PRIx64 ", length 0x%x\n",
			      offset, length);

		/* The NVMe/TCP transport does not use ICDOFF to control the in-capsule data offset. ICDOFF should be '0' */
		if (spdk_unlikely(offset != 0)) {
			/* Not defined fatal error in NVMe/TCP spec, handle this error as a fatal error */
			SPDK_ERRLOG("In-capsule offset 0x%" PRIx64 " should be ZERO in NVMe/TCP\n", offset);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER;
			error_offset = offsetof(struct spdk_nvme_tcp_cmd, ccsqe.dptr.sgl1.address);
			goto fatal_err;
		}

		if (spdk_unlikely(length > max_len)) {
			/* According to the SPEC we should support ICD up to 8192 bytes for admin and fabric commands */
			if (length <= SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE &&
			    (cmd->opc == SPDK_NVME_OPC_FABRIC || req->qpair->qid == 0)) {

				/* Get a buffer from dedicated list */
				SPDK_DEBUGLOG(nvmf_tcp, "Getting a buffer from control msg list\n");
				tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
				assert(tgroup->control_msg_list);
				req->iov[0].iov_base = nvmf_tcp_control_msg_get(tgroup->control_msg_list, tcp_req);
				if (!req->iov[0].iov_base) {
					/* No available buffers. Queue this request up. */
					SPDK_DEBUGLOG(nvmf_tcp, "No available ICD buffers. Queueing request %p\n", tcp_req);
					nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_BUF);
					return;
				}
			} else {
				SPDK_ERRLOG("In-capsule data length 0x%x exceeds capsule length 0x%x\n",
					    length, max_len);
				fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED;
				goto fatal_err;
			}
		} else {
			req->iov[0].iov_base = tcp_req->buf;
		}

		req->length = length;
		req->data_from_pool = false;

		if (spdk_unlikely(req->dif_enabled)) {
			length = spdk_dif_get_length_with_md(length, &req->dif.dif_ctx);
			req->dif.elba_length = length;
		}

		req->iov[0].iov_len = length;
		req->iovcnt = 1;
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_HAVE_BUFFER);

		return;
	}
	/* If we want to handle the problem here, then we can't skip the following data segment.
	 * Because this function runs before reading data part, now handle all errors as fatal errors. */
	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER;
	error_offset = offsetof(struct spdk_nvme_tcp_cmd, ccsqe.dptr.sgl1.generic);
fatal_err:
	nvmf_tcp_send_c2h_term_req(tcp_req->pdu->qpair, tcp_req->pdu, fes, error_offset);
}

static inline enum spdk_nvme_media_error_status_code
nvmf_tcp_dif_error_to_compl_status(uint8_t err_type) {
	enum spdk_nvme_media_error_status_code result;

	switch (err_type)
	{
	case SPDK_DIF_REFTAG_ERROR:
		result = SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_APPTAG_ERROR:
		result = SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_GUARD_ERROR:
		result = SPDK_NVME_SC_GUARD_CHECK_ERROR;
		break;
	default:
		SPDK_UNREACHABLE();
		break;
	}

	return result;
}

/*
 * [한국어]
 * _nvmf_tcp_send_c2h_data - C2H Data PDU(read 응답 데이터) 송신
 *
 * @tqpair: 송신 경로
 * @tcp_req: 응답 보낼 read 요청
 *
 * NVMe-TCP의 read 응답은 데이터 자체를 C2H Data PDU로 전송하며,
 * 마지막 PDU에 LAST_PDU 플래그를 둔다. c2h_success 옵션이 켜져 있고 status가
 * 0이면 SUCCESS 플래그를 추가해 별도 CapsuleResp PDU 송신을 생략 (와이어 절약).
 *
 * 큰 데이터는 여러 PDU에 쪼개져 전송될 수 있어 rw_offset 누적 추적.
 * cpda(Controller PDU Data Alignment)가 설정되어 있으면 padding을 추가해
 * payload offset을 정렬한다.
 *
 * 송신 후 완료 콜백 nvmf_tcp_pdu_c2h_data_complete에서 partial인지 체크하고
 * 추가 PDU를 더 보내거나 capsule response로 마무리.
 */
static void
_nvmf_tcp_send_c2h_data(struct spdk_nvmf_tcp_qpair *tqpair,
			struct spdk_nvmf_tcp_req *tcp_req)
{
	struct spdk_nvmf_tcp_transport *ttransport = SPDK_CONTAINEROF(
				tqpair->qpair.transport, struct spdk_nvmf_tcp_transport, transport);
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data;
	uint32_t plen, pdo, alignment;
	int rc;

	SPDK_DEBUGLOG(nvmf_tcp, "enter\n");

	rsp_pdu = tcp_req->pdu;
	assert(rsp_pdu != NULL);

	c2h_data = &rsp_pdu->hdr.c2h_data;
	c2h_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_C2H_DATA;
	plen = c2h_data->common.hlen = sizeof(*c2h_data);

	if (tqpair->host_hdgst_enable) {
		plen += SPDK_NVME_TCP_DIGEST_LEN;
		c2h_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
	}

	/* set the psh */
	c2h_data->cccid = tcp_req->req.cmd->nvme_cmd.cid;
	c2h_data->datal = tcp_req->req.length - tcp_req->pdu->rw_offset;
	c2h_data->datao = tcp_req->pdu->rw_offset;

	/* set the padding */
	rsp_pdu->padding_len = 0;
	pdo = plen;
	if (tqpair->cpda) {
		alignment = (tqpair->cpda + 1) << 2;
		if (plen % alignment != 0) {
			pdo = (plen + alignment) / alignment * alignment;
			rsp_pdu->padding_len = pdo - plen;
			plen = pdo;
		}
	}

	c2h_data->common.pdo = pdo;
	plen += c2h_data->datal;
	if (tqpair->host_ddgst_enable) {
		c2h_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	c2h_data->common.plen = plen;

	if (spdk_unlikely(tcp_req->req.dif_enabled)) {
		rsp_pdu->dif_ctx = &tcp_req->req.dif.dif_ctx;
	}

	nvme_tcp_pdu_set_data_buf(rsp_pdu, tcp_req->req.iov, tcp_req->req.iovcnt,
				  c2h_data->datao, c2h_data->datal);


	c2h_data->common.flags |= SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU;
	/* Need to send the capsule response if response is not all 0 */
	if (ttransport->tcp_opts.c2h_success &&
	    tcp_req->rsp.cdw0 == 0 && tcp_req->rsp.cdw1 == 0) {
		c2h_data->common.flags |= SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS;
	}

	if (spdk_unlikely(tcp_req->req.dif_enabled)) {
		struct spdk_nvme_cpl *rsp = &tcp_req->req.rsp->nvme_cpl;
		struct spdk_dif_error err_blk = {};
		uint32_t mapped_length = 0;
		uint32_t available_iovs = SPDK_COUNTOF(rsp_pdu->iov);
		uint32_t ddgst_len = 0;

		if (tqpair->host_ddgst_enable) {
			/* Data digest consumes additional iov entry */
			available_iovs--;
			/* plen needs to be updated since nvme_tcp_build_iovs compares expected and actual plen */
			ddgst_len = SPDK_NVME_TCP_DIGEST_LEN;
			c2h_data->common.plen -= ddgst_len;
		}
		/* Temp call to estimate if data can be described by limited number of iovs.
		 * iov vector will be rebuilt in nvmf_tcp_qpair_write_pdu */
		nvme_tcp_build_iovs(rsp_pdu->iov, available_iovs, rsp_pdu, tqpair->host_hdgst_enable,
				    false, &mapped_length);

		if (mapped_length != c2h_data->common.plen) {
			c2h_data->datal = mapped_length - (c2h_data->common.plen - c2h_data->datal);
			SPDK_DEBUGLOG(nvmf_tcp,
				      "Part C2H, data_len %u (of %u), PDU len %u, updated PDU len %u, offset %u\n",
				      c2h_data->datal, tcp_req->req.length, c2h_data->common.plen, mapped_length, rsp_pdu->rw_offset);
			c2h_data->common.plen = mapped_length;

			/* Rebuild pdu->data_iov since data length is changed */
			nvme_tcp_pdu_set_data_buf(rsp_pdu, tcp_req->req.iov, tcp_req->req.iovcnt, c2h_data->datao,
						  c2h_data->datal);

			c2h_data->common.flags &= ~(SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU |
						    SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS);
		}

		c2h_data->common.plen += ddgst_len;

		assert(rsp_pdu->rw_offset <= tcp_req->req.length);

		rc = spdk_dif_verify_stream(rsp_pdu->data_iov, rsp_pdu->data_iovcnt,
					    0, rsp_pdu->data_len, rsp_pdu->dif_ctx, &err_blk);
		if (rc != 0) {
			SPDK_ERRLOG("DIF error detected. type=%d, offset=%" PRIu32 "\n",
				    err_blk.err_type, err_blk.err_offset);
			rsp->status.sct = SPDK_NVME_SCT_MEDIA_ERROR;
			rsp->status.sc = nvmf_tcp_dif_error_to_compl_status(err_blk.err_type);
			nvmf_tcp_send_capsule_resp_pdu(tcp_req, tqpair);
			return;
		}
	}

	rsp_pdu->rw_offset += c2h_data->datal;
	nvmf_tcp_qpair_write_req_pdu(tqpair, tcp_req, nvmf_tcp_pdu_c2h_data_complete, tcp_req);
}

static void
nvmf_tcp_send_c2h_data(struct spdk_nvmf_tcp_qpair *tqpair,
		       struct spdk_nvmf_tcp_req *tcp_req)
{
	nvmf_tcp_req_pdu_init(tcp_req);
	_nvmf_tcp_send_c2h_data(tqpair, tcp_req);
}

static int
request_transfer_out(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_req	*tcp_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	struct spdk_nvme_cpl		*rsp;

	SPDK_DEBUGLOG(nvmf_tcp, "enter\n");

	qpair = req->qpair;
	rsp = &req->rsp->nvme_cpl;
	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_req, req);

	/* Advance our sq_head pointer */
	if (qpair->sq_head == qpair->sq_head_max) {
		qpair->sq_head = 0;
	} else {
		qpair->sq_head++;
	}
	rsp->sqhd = qpair->sq_head;

	tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);
	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	if (spdk_nvme_cpl_is_success(rsp) && req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		nvmf_tcp_send_c2h_data(tqpair, tcp_req);
	} else {
		nvmf_tcp_send_capsule_resp_pdu(tcp_req, tqpair);
	}

	return 0;
}

static void
nvmf_tcp_check_fused_ordering(struct spdk_nvmf_tcp_transport *ttransport,
			      struct spdk_nvmf_tcp_qpair *tqpair,
			      struct spdk_nvmf_tcp_req *tcp_req)
{
	enum spdk_nvme_cmd_fuse last, next;

	last = tqpair->fused_first ? tqpair->fused_first->cmd.fuse : SPDK_NVME_CMD_FUSE_NONE;
	next = tcp_req->cmd.fuse;

	assert(last != SPDK_NVME_CMD_FUSE_SECOND);

	if (spdk_likely(last == SPDK_NVME_CMD_FUSE_NONE && next == SPDK_NVME_CMD_FUSE_NONE)) {
		return;
	}

	if (last == SPDK_NVME_CMD_FUSE_FIRST) {
		if (next == SPDK_NVME_CMD_FUSE_SECOND) {
			/* This is a valid pair of fused commands.  Point them at each other
			 * so they can be submitted consecutively once ready to be executed.
			 */
			tqpair->fused_first->fused_pair = tcp_req;
			tcp_req->fused_pair = tqpair->fused_first;
			tqpair->fused_first = NULL;
			return;
		} else {
			/* Mark the last req as failed since it wasn't followed by a SECOND. */
			tqpair->fused_first->fused_failed = true;

			/*
			 * If the last req is in READY_TO_EXECUTE state, then call
			 * nvmf_tcp_req_process(), otherwise nothing else will kick it.
			 */
			if (tqpair->fused_first->state == TCP_REQUEST_STATE_READY_TO_EXECUTE) {
				nvmf_tcp_req_process(ttransport, tqpair->fused_first);
			}

			tqpair->fused_first = NULL;
		}
	}

	if (next == SPDK_NVME_CMD_FUSE_FIRST) {
		/* Set tqpair->fused_first here so that we know to check that the next request
		 * is a SECOND (and to fail this one if it isn't).
		 */
		tqpair->fused_first = tcp_req;
	} else if (next == SPDK_NVME_CMD_FUSE_SECOND) {
		/* Mark this req failed since it is a SECOND and the last one was not a FIRST. */
		tcp_req->fused_failed = true;
	}
}

/*
 * [한국어]
 * nvmf_tcp_req_process - TCP req의 상태 머신 본체 (가장 핵심 함수)
 *
 * @ttransport: TCP transport 컨텍스트
 * @tcp_req:    상태 진행할 요청
 * @return: 진행 여부 (true=상태 변화 발생)
 *
 * 한 요청의 lifecycle 전체(NEW → ... → COMPLETED → FREE)를 단계별로 진행시킨다.
 * 거대한 switch에서 각 상태마다 다음 작업을 수행:
 *   - NEW: cmd 복사, IO/admin 분류, in-capsule data 검사 → NEED_BUFFER
 *   - NEED_BUFFER: transport buf cache에서 데이터 buffer 요청 → HAVE_BUFFER
 *   - HAVE_BUFFER: in-capsule이면 PAYLOAD 수신 시작, 아니면 R2T 송신
 *   - TRANSFERRING_HOST_TO_CONTROLLER: 모든 H2C 수신 완료 시 READY_TO_EXECUTE
 *   - READY_TO_EXECUTE: spdk_nvmf_request_exec → bdev/admin 디스패치
 *   - EXECUTING / EXECUTED: bdev 완료 콜백 후 응답 단계 결정
 *   - READY_TO_COMPLETE: capsule_resp PDU 송신
 *   - TRANSFERRING_CONTROLLER_TO_HOST: read 응답 C2H Data 송신
 *   - COMPLETED: req_put → free queue 반환
 * 비동기 자원 대기 시 즉시 종료하고, 자원 도착 시 콜백으로 다시 호출.
 *
 * 상태 변화가 일어나는 한 do-while 루프로 연속 진행 (latency 최적화).
 *
 * 호출 위치: 진입 시점은 다양함 — 새 PDU 도착(capsule_cmd_payload_handle),
 * H2C 완료(h2c_data_payload_handle), bdev 완료(req_complete), 버퍼 도착
 * (request_get_buffers_done), R2T ack(r2t_complete) 등.
 *
 * 호출 체인:
 *   다양한 진입점 → [nvmf_tcp_req_process] → spdk_nvmf_request_exec /
 *     send_r2t_pdu / send_capsule_resp_pdu / _send_c2h_data / req_put / ...
 */
static bool
nvmf_tcp_req_process(struct spdk_nvmf_tcp_transport *ttransport,
		     struct spdk_nvmf_tcp_req *tcp_req)
{
	struct spdk_nvmf_tcp_qpair		*tqpair;
	uint32_t				plen;
	struct nvme_tcp_pdu			*pdu;
	enum spdk_nvmf_tcp_req_state		prev_state;
	bool					progress = false;
	struct spdk_nvmf_transport		*transport = &ttransport->transport;
	struct spdk_nvmf_transport_poll_group	*group;
	struct spdk_nvmf_tcp_poll_group		*tgroup;

	tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);
	group = &tqpair->group->group;
	assert(tcp_req->state != TCP_REQUEST_STATE_FREE);

	/* If the qpair is not active, we need to abort the outstanding requests. */
	if (!spdk_nvmf_qpair_is_active(&tqpair->qpair)) {
		if (tcp_req->state == TCP_REQUEST_STATE_NEED_BUFFER) {
			nvmf_tcp_request_get_buffers_abort(tcp_req);
		}
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_COMPLETED);
	}

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = tcp_req->state;

		SPDK_DEBUGLOG(nvmf_tcp, "Request %p entering state %d on tqpair=%p\n", tcp_req, prev_state,
			      tqpair);

		switch (tcp_req->state) {
		case TCP_REQUEST_STATE_FREE:
			/* Some external code must kick a request into TCP_REQUEST_STATE_NEW
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_NEW:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_NEW, tqpair->qpair.trace_id, 0, (uintptr_t)tcp_req,
					  tqpair->qpair.queue_depth);

			/* copy the cmd from the receive pdu */
			tcp_req->cmd = tqpair->pdu_in_progress->hdr.capsule_cmd.ccsqe;

			if (spdk_unlikely(spdk_nvmf_request_get_dif_ctx(&tcp_req->req, &tcp_req->req.dif.dif_ctx))) {
				tcp_req->req.dif_enabled = true;
				tqpair->pdu_in_progress->dif_ctx = &tcp_req->req.dif.dif_ctx;
			}

			nvmf_tcp_check_fused_ordering(ttransport, tqpair, tcp_req);

			/* The next state transition depends on the data transfer needs of this request. */
			tcp_req->req.xfer = spdk_nvmf_req_get_xfer(&tcp_req->req);

			if (spdk_unlikely(tcp_req->req.xfer == SPDK_NVME_DATA_BIDIRECTIONAL)) {
				nvmf_tcp_req_set_cpl(tcp_req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_INVALID_OPCODE);
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
				SPDK_DEBUGLOG(nvmf_tcp, "Request %p: invalid xfer type (BIDIRECTIONAL)\n", tcp_req);
				break;
			}

			/* If no data to transfer, ready to execute. */
			if (tcp_req->req.xfer == SPDK_NVME_DATA_NONE) {
				/* Reset the tqpair receiving pdu state */
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
				break;
			}

			pdu = tqpair->pdu_in_progress;
			plen = pdu->hdr.common.hlen;
			if (tqpair->host_hdgst_enable) {
				plen += SPDK_NVME_TCP_DIGEST_LEN;
			}
			if (pdu->hdr.common.plen != plen) {
				tcp_req->has_in_capsule_data = true;
			} else {
				/* Data is transmitted by C2H PDUs */
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
			}

			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_NEED_BUFFER);
			break;
		case TCP_REQUEST_STATE_NEED_BUFFER:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_NEED_BUFFER, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);

			assert(tcp_req->req.xfer != SPDK_NVME_DATA_NONE);

			/* Try to get a data buffer */
			nvmf_tcp_req_parse_sgl(tcp_req, transport, group);
			break;
		case TCP_REQUEST_STATE_HAVE_BUFFER:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_HAVE_BUFFER, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			/* Get a zcopy buffer if the request can be serviced through zcopy */
			if (spdk_nvmf_request_using_zcopy(&tcp_req->req)) {
				if (spdk_unlikely(tcp_req->req.dif_enabled)) {
					assert(tcp_req->req.dif.elba_length >= tcp_req->req.length);
					tcp_req->req.length = tcp_req->req.dif.elba_length;
				}

				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_AWAITING_ZCOPY_START);
				spdk_nvmf_request_zcopy_start(&tcp_req->req);
				break;
			}

			assert(tcp_req->req.iovcnt > 0);

			/* If data is transferring from host to controller, we need to do a transfer from the host. */
			if (tcp_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
				if (tcp_req->req.data_from_pool) {
					SPDK_DEBUGLOG(nvmf_tcp, "Sending R2T for tcp_req(%p) on tqpair=%p\n", tcp_req, tqpair);
					nvmf_tcp_send_r2t_pdu(tqpair, tcp_req);
				} else {
					struct nvme_tcp_pdu *pdu;

					nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);

					pdu = tqpair->pdu_in_progress;
					SPDK_DEBUGLOG(nvmf_tcp, "Not need to send r2t for tcp_req(%p) on tqpair=%p\n", tcp_req,
						      tqpair);
					/* No need to send r2t, contained in the capsuled data */
					nvme_tcp_pdu_set_data_buf(pdu, tcp_req->req.iov, tcp_req->req.iovcnt,
								  0, tcp_req->req.length);
					nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
				}
				break;
			}

			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
			break;
		case TCP_REQUEST_STATE_AWAITING_ZCOPY_START:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_START, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			/* Some external code must kick a request into  TCP_REQUEST_STATE_ZCOPY_START_COMPLETED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_ZCOPY_START_COMPLETED:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_ZCOPY_START_COMPLETED, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			if (spdk_unlikely(spdk_nvme_cpl_is_error(&tcp_req->req.rsp->nvme_cpl))) {
				SPDK_DEBUGLOG(nvmf_tcp, "Zero-copy start failed for tcp_req(%p) on tqpair=%p\n",
					      tcp_req, tqpair);
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
				break;
			}
			if (tcp_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
				SPDK_DEBUGLOG(nvmf_tcp, "Sending R2T for tcp_req(%p) on tqpair=%p\n", tcp_req, tqpair);
				nvmf_tcp_send_r2t_pdu(tqpair, tcp_req);
			} else {
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_EXECUTED);
			}
			break;
		case TCP_REQUEST_STATE_AWAITING_R2T_ACK:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_AWAIT_R2T_ACK, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			/* The R2T completion or the h2c data incoming will kick it out of this state. */
			break;
		case TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:

			spdk_trace_record(TRACE_TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER, tqpair->qpair.trace_id,
					  0, (uintptr_t)tcp_req);
			/* Some external code must kick a request into TCP_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_READY_TO_EXECUTE:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_READY_TO_EXECUTE, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);

			if (spdk_unlikely(tcp_req->req.dif_enabled)) {
				assert(tcp_req->req.dif.elba_length >= tcp_req->req.length);
				tcp_req->req.length = tcp_req->req.dif.elba_length;
			}

			if (tcp_req->cmd.fuse != SPDK_NVME_CMD_FUSE_NONE) {
				if (tcp_req->fused_failed) {
					/* This request failed FUSED semantics.  Fail it immediately, without
					 * even sending it to the target layer.
					 */
					nvmf_tcp_req_set_cpl(tcp_req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_ABORTED_MISSING_FUSED);
					nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
					break;
				}

				if (tcp_req->fused_pair == NULL ||
				    tcp_req->fused_pair->state != TCP_REQUEST_STATE_READY_TO_EXECUTE) {
					/* This request is ready to execute, but either we don't know yet if it's
					 * valid - i.e. this is a FIRST but we haven't received the next request yet),
					 * or the other request of this fused pair isn't ready to execute. So
					 * break here and this request will get processed later either when the
					 * other request is ready or we find that this request isn't valid.
					 */
					break;
				}
			}

			if (!spdk_nvmf_request_using_zcopy(&tcp_req->req)) {
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_EXECUTING);
				/* If we get to this point, and this request is a fused command, we know that
				 * it is part of a valid sequence (FIRST followed by a SECOND) and that both
				 * requests are READY_TO_EXECUTE.  So call spdk_nvmf_request_exec() both on this
				 * request, and the other request of the fused pair, in the correct order.
				 * Also clear the ->fused_pair pointers on both requests, since after this point
				 * we no longer need to maintain the relationship between these two requests.
				 */
				if (tcp_req->cmd.fuse == SPDK_NVME_CMD_FUSE_SECOND) {
					assert(tcp_req->fused_pair != NULL);
					assert(tcp_req->fused_pair->fused_pair == tcp_req);
					nvmf_tcp_req_set_state(tcp_req->fused_pair, TCP_REQUEST_STATE_EXECUTING);
					spdk_nvmf_request_exec(&tcp_req->fused_pair->req);
					tcp_req->fused_pair->fused_pair = NULL;
					tcp_req->fused_pair = NULL;
				}
				spdk_nvmf_request_exec(&tcp_req->req);
				if (tcp_req->cmd.fuse == SPDK_NVME_CMD_FUSE_FIRST) {
					assert(tcp_req->fused_pair != NULL);
					assert(tcp_req->fused_pair->fused_pair == tcp_req);
					nvmf_tcp_req_set_state(tcp_req->fused_pair, TCP_REQUEST_STATE_EXECUTING);
					spdk_nvmf_request_exec(&tcp_req->fused_pair->req);
					tcp_req->fused_pair->fused_pair = NULL;
					tcp_req->fused_pair = NULL;
				}
			} else {
				/* For zero-copy, only requests with data coming from host to the
				 * controller can end up here. */
				assert(tcp_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_AWAITING_ZCOPY_COMMIT);
				spdk_nvmf_request_zcopy_end(&tcp_req->req, true);
			}

			break;
		case TCP_REQUEST_STATE_EXECUTING:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_EXECUTING, tqpair->qpair.trace_id, 0, (uintptr_t)tcp_req);
			/* Some external code must kick a request into TCP_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_AWAITING_ZCOPY_COMMIT:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_COMMIT, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			/* Some external code must kick a request into TCP_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_EXECUTED:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_EXECUTED, tqpair->qpair.trace_id, 0, (uintptr_t)tcp_req);

			if (spdk_unlikely(tcp_req->req.dif_enabled)) {
				tcp_req->req.length = tcp_req->req.dif.orig_length;
			}

			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
			break;
		case TCP_REQUEST_STATE_READY_TO_COMPLETE:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_READY_TO_COMPLETE, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			if (request_transfer_out(&tcp_req->req) != 0) {
				assert(0); /* No good way to handle this currently */
			}
			break;
		case TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST, tqpair->qpair.trace_id,
					  0, (uintptr_t)tcp_req);
			/* Some external code must kick a request into TCP_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_AWAITING_ZCOPY_RELEASE:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_RELEASE, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			/* Some external code must kick a request into TCP_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_COMPLETED:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_COMPLETED, tqpair->qpair.trace_id, 0, (uintptr_t)tcp_req,
					  tqpair->qpair.queue_depth);
			/* If there's an outstanding PDU sent to the host, the request is completed
			 * due to the qpair being disconnected.  We must delay the completion until
			 * that write is done to avoid freeing the request twice. */
			if (spdk_unlikely(tcp_req->pdu_in_use)) {
				SPDK_DEBUGLOG(nvmf_tcp, "Delaying completion due to outstanding "
					      "write on req=%p\n", tcp_req);
				/* This can only happen for zcopy requests */
				assert(spdk_nvmf_request_using_zcopy(&tcp_req->req));
				assert(!spdk_nvmf_qpair_is_active(&tqpair->qpair));
				break;
			}

			if (tcp_req->req.data_from_pool) {
				spdk_nvmf_request_free_buffers(&tcp_req->req, group, transport);
			} else if (spdk_unlikely(tcp_req->has_in_capsule_data &&
						 (tcp_req->cmd.opc == SPDK_NVME_OPC_FABRIC ||
						  tqpair->qpair.qid == 0) && tcp_req->req.length > transport->opts.in_capsule_data_size)) {
				tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
				assert(tgroup->control_msg_list);
				SPDK_DEBUGLOG(nvmf_tcp, "Put buf to control msg list\n");
				nvmf_tcp_control_msg_put(tgroup->control_msg_list,
							 tcp_req->req.iov[0].iov_base);
			} else if (tcp_req->req.zcopy_bdev_io != NULL) {
				/* If the request has an unreleased zcopy bdev_io, it's either a
				 * read, a failed write, or the qpair is being disconnected */
				assert(spdk_nvmf_request_using_zcopy(&tcp_req->req));
				assert(tcp_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST ||
				       spdk_nvme_cpl_is_error(&tcp_req->req.rsp->nvme_cpl) ||
				       !spdk_nvmf_qpair_is_active(&tqpair->qpair));
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_AWAITING_ZCOPY_RELEASE);
				spdk_nvmf_request_zcopy_end(&tcp_req->req, false);
				break;
			}
			tcp_req->req.length = 0;
			tcp_req->req.iovcnt = 0;
			tcp_req->fused_failed = false;
			if (tcp_req->fused_pair) {
				/* This req was part of a valid fused pair, but failed before it got to
				 * READ_TO_EXECUTE state.  This means we need to fail the other request
				 * in the pair, because it is no longer part of a valid pair.  If the pair
				 * already reached READY_TO_EXECUTE state, we need to kick it.
				 */
				tcp_req->fused_pair->fused_failed = true;
				if (tcp_req->fused_pair->state == TCP_REQUEST_STATE_READY_TO_EXECUTE) {
					nvmf_tcp_req_process(ttransport, tcp_req->fused_pair);
				}
				tcp_req->fused_pair = NULL;
			}

			nvmf_tcp_req_put(tqpair, tcp_req);
			break;
		case TCP_REQUEST_NUM_STATES:
		default:
			assert(0);
			break;
		}

		if (tcp_req->state != prev_state) {
			progress = true;
		}
	} while (tcp_req->state != prev_state);

	return progress;
}

/*
 * [한국어]
 * nvmf_tcp_qpair_process - qpair에서 PDU 수신 진행 + 오류 처리
 *
 * sock_process 결과가 음수면 disconnect 절차를 트리거.
 * 본 함수는 sock_cb나 메시지 콜백에서 진입점으로 호출됨.
 */
static void
nvmf_tcp_qpair_process(struct spdk_nvmf_tcp_qpair *tqpair)
{
	int rc;

	assert(tqpair != NULL);
	rc = nvmf_tcp_sock_process(tqpair);	/* [한국어] PDU 수신 상태 머신 한 번 돌리기 */

	/* If there was a new socket error, disconnect */
	if (rc < 0) {
		nvmf_tcp_qpair_disconnect(tqpair);	/* [한국어] sock 오류 — 연결 종료 절차 진입 */
	}
}

/*
 * [한국어]
 * nvmf_tcp_sock_cb - 소켓에 데이터 도착 시 sock_group이 호출하는 콜백
 *
 * @arg: spdk_sock_group_add_sock에 등록된 cb_arg (= tqpair 포인터)
 *
 * poll_group_poll → spdk_sock_group_poll → 본 콜백 → qpair_process로 이어지는
 * NVMe-TCP 데이터 plane의 진입점. 동일 thread(poll group thread)에서 호출됨.
 */
static void
nvmf_tcp_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_nvmf_tcp_qpair *tqpair = arg;

	nvmf_tcp_qpair_process(tqpair);
}

static int
nvmf_tcp_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_poll_group	*tgroup;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	int				rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	rc =  nvmf_tcp_qpair_sock_init(tqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Cannot set sock opt for tqpair=%p\n", tqpair);
		return -1;
	}

	rc = nvmf_tcp_qpair_init(&tqpair->qpair);
	if (rc < 0) {
		SPDK_ERRLOG("Cannot init tqpair=%p\n", tqpair);
		return -1;
	}

	rc = nvmf_tcp_qpair_init_mem_resource(tqpair);
	if (rc < 0) {
		SPDK_ERRLOG("Cannot init memory resource info for tqpair=%p\n", tqpair);
		return -1;
	}

	rc = spdk_sock_group_add_sock(tgroup->sock_group, tqpair->sock,
				      nvmf_tcp_sock_cb, tqpair);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_group_add_sock() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		return -1;
	}

	tqpair->group = tgroup;
	nvmf_tcp_qpair_set_state(tqpair, NVMF_TCP_QPAIR_STATE_INVALID);
	TAILQ_INSERT_TAIL(&tgroup->qpairs, tqpair, link);

	return 0;
}

static int
nvmf_tcp_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
			   struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_poll_group	*tgroup;
	struct spdk_nvmf_tcp_qpair		*tqpair;
	int				rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	assert(tqpair->group == tgroup);

	SPDK_DEBUGLOG(nvmf_tcp, "remove tqpair=%p from the tgroup=%p\n", tqpair, tgroup);
	if (tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_REQ) {
		/* Change the state to move the qpair from the await_req list to the main list
		 * and prevent adding it again later by nvmf_tcp_qpair_set_recv_state() */
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
	}
	TAILQ_REMOVE(&tgroup->qpairs, tqpair, link);

	/* Try to force out any pending writes, intentionally do not check rc as it is best effort try. */
	spdk_sock_flush(tqpair->sock);

	rc = spdk_sock_group_remove_sock(tgroup->sock_group, tqpair->sock);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_group_remove_sock() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
	}

	nvmf_tcp_abort_await_buffer_reqs(tqpair);
	return rc;
}

/*
 * [한국어]
 * nvmf_tcp_req_complete - bdev/ctrlr가 IO 처리 완료 후 호출하는 콜백 (transport_ops.req_complete)
 *
 * @req: 완료된 nvmf request (rsp.nvme_cpl에 status 채워져 있음)
 *
 * spdk_nvmf_request_complete가 호출되면 결국 transport ops를 통해 본 함수로
 * 도달. 현재 state에 따라 EXECUTED / ZCOPY_START_COMPLETED / COMPLETED로 전이
 * 후 req_process로 다음 단계 진행 (응답 PDU 송신 등).
 */
static void
nvmf_tcp_req_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_req *tcp_req;

	ttransport = SPDK_CONTAINEROF(req->qpair->transport, struct spdk_nvmf_tcp_transport, transport);
	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_req, req);

	switch (tcp_req->state) {
	case TCP_REQUEST_STATE_EXECUTING:
	case TCP_REQUEST_STATE_AWAITING_ZCOPY_COMMIT:
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_EXECUTED);
		break;
	case TCP_REQUEST_STATE_AWAITING_ZCOPY_START:
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_ZCOPY_START_COMPLETED);
		break;
	case TCP_REQUEST_STATE_AWAITING_ZCOPY_RELEASE:
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_COMPLETED);
		break;
	default:
		SPDK_ERRLOG("Unexpected request state %d (cntlid:%d, qid:%d)\n",
			    tcp_req->state, req->qpair->ctrlr->cntlid, req->qpair->qid);
		assert(0 && "Unexpected request state");
		break;
	}

	nvmf_tcp_req_process(ttransport, tcp_req);
}

static void
nvmf_tcp_close_qpair(struct spdk_nvmf_qpair *qpair,
		     spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair;

	SPDK_DEBUGLOG(nvmf_tcp, "Qpair: %p\n", qpair);

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	assert(tqpair->fini_cb_fn == NULL);
	tqpair->fini_cb_fn = cb_fn;
	tqpair->fini_cb_arg = cb_arg;

	nvmf_tcp_qpair_set_state(tqpair, NVMF_TCP_QPAIR_STATE_EXITED);
	nvmf_tcp_qpair_destroy(tqpair);
}

/*
 * [한국어]
 * nvmf_tcp_poll_group_poll - poll group의 1회 polling 진입점 (transport_ops.poll_group_poll)
 *
 * @group: 호출된 poll group
 * @return: 처리한 작업 수 (>0=BUSY, 0=IDLE, <0=오류)
 *
 * spdk_nvmf_poll_group_poll에서 호출되며, 본 group의 sock_group을 polling하여
 * readable한 sock에 대해 nvmf_tcp_sock_cb를 트리거한다. SPDK reactor 루프의
 * 매 iteration마다 호출됨 — 즉, polling 모드의 본체.
 */
static int
nvmf_tcp_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_tcp_poll_group *tgroup;
	int rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);

	if (spdk_unlikely(TAILQ_EMPTY(&tgroup->qpairs))) {
		return 0;	/* [한국어] 등록된 qpair가 없으면 polling 생략 */
	}

	rc = spdk_sock_group_poll(tgroup->sock_group);	/* [한국어] epoll/uring/select 1회 — readable sock의 콜백 호출 */
	if (spdk_unlikely(rc < 0)) {
		SPDK_ERRLOG("spdk_sock_group_poll() failed, sock_group=%p, rc %d: %s\n", tgroup->sock_group, rc,
			    spdk_strerror(-rc));
	}

	return rc;
}

static void
nvmf_tcp_qpair_get_trid(struct spdk_nvmf_qpair *qpair,
			struct spdk_nvme_transport_id *trid, bool peer)
{
	struct spdk_nvmf_tcp_qpair     *tqpair;
	uint16_t			port;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	spdk_nvme_trid_populate_transport(trid, SPDK_NVME_TRANSPORT_TCP);

	if (peer) {
		snprintf(trid->traddr, sizeof(trid->traddr), "%s", tqpair->initiator_addr);
		port = tqpair->initiator_port;
	} else {
		snprintf(trid->traddr, sizeof(trid->traddr), "%s", tqpair->target_addr);
		port = tqpair->target_port;
	}

	if (spdk_sock_is_ipv4(tqpair->sock)) {
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;
	} else if (spdk_sock_is_ipv6(tqpair->sock)) {
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV6;
	} else {
		SPDK_ERRLOG("Unsupported socket type for qpair: %p\n", qpair);
		assert(false);
	}

	snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%d", port);
}

static int
nvmf_tcp_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	nvmf_tcp_qpair_get_trid(qpair, trid, 0);

	return 0;
}

static int
nvmf_tcp_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			     struct spdk_nvme_transport_id *trid)
{
	nvmf_tcp_qpair_get_trid(qpair, trid, 1);

	return 0;
}

static int
nvmf_tcp_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
			       struct spdk_nvme_transport_id *trid)
{
	nvmf_tcp_qpair_get_trid(qpair, trid, 0);

	return 0;
}

static void
nvmf_tcp_req_set_abort_status(struct spdk_nvmf_request *req,
			      struct spdk_nvmf_tcp_req *tcp_req_to_abort)
{
	nvmf_tcp_req_set_cpl(tcp_req_to_abort, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_ABORTED_BY_REQUEST);
	nvmf_tcp_req_set_state(tcp_req_to_abort, TCP_REQUEST_STATE_READY_TO_COMPLETE);

	req->rsp->nvme_cpl.cdw0 &= ~1U; /* Command was successfully aborted. */
}

static int
_nvmf_tcp_qpair_abort_request(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_tcp_req *tcp_req_to_abort = SPDK_CONTAINEROF(req->req_to_abort,
			struct spdk_nvmf_tcp_req, req);
	struct spdk_nvmf_tcp_qpair *tqpair = SPDK_CONTAINEROF(req->req_to_abort->qpair,
					     struct spdk_nvmf_tcp_qpair, qpair);
	struct spdk_nvmf_tcp_transport *ttransport = SPDK_CONTAINEROF(tqpair->qpair.transport,
			struct spdk_nvmf_tcp_transport, transport);
	int rc;

	spdk_poller_unregister(&req->poller);

	switch (tcp_req_to_abort->state) {
	case TCP_REQUEST_STATE_EXECUTING:
	case TCP_REQUEST_STATE_AWAITING_ZCOPY_START:
	case TCP_REQUEST_STATE_AWAITING_ZCOPY_COMMIT:
		rc = nvmf_ctrlr_abort_request(req);
		if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS) {
			return SPDK_POLLER_BUSY;
		}
		break;

	case TCP_REQUEST_STATE_NEED_BUFFER:
		nvmf_tcp_request_get_buffers_abort(tcp_req_to_abort);
		nvmf_tcp_req_set_abort_status(req, tcp_req_to_abort);
		nvmf_tcp_req_process(ttransport, tcp_req_to_abort);
		break;

	case TCP_REQUEST_STATE_AWAITING_R2T_ACK:
	case TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
		if (spdk_get_ticks() < req->timeout_tsc) {
			req->poller = SPDK_POLLER_REGISTER(_nvmf_tcp_qpair_abort_request, req, 0);
			return SPDK_POLLER_BUSY;
		}
		break;

	default:
		/* Requests in other states are either un-abortable (e.g.
		 * TRANSFERRING_CONTROLLER_TO_HOST) or should never end up here, as they're
		 * immediately transitioned to other states in nvmf_tcp_req_process() (e.g.
		 * READY_TO_EXECUTE).  But it is fine to end up here, as we'll simply complete the
		 * abort request with the bit0 of dword0 set (command not aborted).
		 */
		break;
	}

	spdk_nvmf_request_complete(req);
	return SPDK_POLLER_BUSY;
}

static void
nvmf_tcp_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
			     struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_qpair *tqpair;
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_transport *transport;
	uint16_t cid;
	uint32_t i;
	struct spdk_nvmf_tcp_req *tcp_req_to_abort = NULL;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	ttransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_tcp_transport, transport);
	transport = &ttransport->transport;

	cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;

	for (i = 0; i < tqpair->resource_count; i++) {
		if (tqpair->reqs[i].state != TCP_REQUEST_STATE_FREE &&
		    tqpair->reqs[i].req.cmd->nvme_cmd.cid == cid) {
			tcp_req_to_abort = &tqpair->reqs[i];
			break;
		}
	}

	spdk_trace_record(TRACE_TCP_QP_ABORT_REQ, tqpair->qpair.trace_id, 0, (uintptr_t)req);

	if (tcp_req_to_abort == NULL) {
		spdk_nvmf_request_complete(req);
		return;
	}

	req->req_to_abort = &tcp_req_to_abort->req;
	req->timeout_tsc = spdk_get_ticks() +
			   transport->opts.abort_timeout_sec * spdk_get_ticks_hz();
	req->poller = NULL;

	_nvmf_tcp_qpair_abort_request(req);
}

struct tcp_subsystem_add_host_opts {
	char *psk;
};

static const struct spdk_json_object_decoder tcp_subsystem_add_host_opts_decoder[] = {
	{"psk", offsetof(struct tcp_subsystem_add_host_opts, psk), spdk_json_decode_string, true},
};

static int
nvmf_tcp_subsystem_add_host(struct spdk_nvmf_transport *transport,
			    const struct spdk_nvmf_subsystem *subsystem,
			    const char *hostnqn,
			    const struct spdk_json_val *transport_specific)
{
	struct tcp_subsystem_add_host_opts opts;
	struct spdk_nvmf_tcp_transport *ttransport;
	struct tcp_psk_entry *tmp, *entry = NULL;
	uint8_t psk_configured[SPDK_TLS_PSK_MAX_LEN] = {};
	char psk_interchange[SPDK_TLS_PSK_MAX_LEN + 1] = {};
	uint8_t tls_cipher_suite;
	int rc = 0;
	uint8_t psk_retained_hash;
	uint64_t psk_configured_size;

	if (transport_specific == NULL) {
		return 0;
	}

	assert(transport != NULL);
	assert(subsystem != NULL);

	memset(&opts, 0, sizeof(opts));

	/* Decode PSK (either name of a key or file path) */
	if (spdk_json_decode_object_relaxed(transport_specific, tcp_subsystem_add_host_opts_decoder,
					    SPDK_COUNTOF(tcp_subsystem_add_host_opts_decoder), &opts)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		return -EINVAL;
	}

	if (opts.psk == NULL) {
		return 0;
	}

	entry = calloc(1, sizeof(struct tcp_psk_entry));
	if (entry == NULL) {
		SPDK_ERRLOG("Unable to allocate memory for PSK entry!\n");
		rc = -ENOMEM;
		goto end;
	}

	entry->key = spdk_keyring_get_key(opts.psk);
	if (entry->key == NULL) {
		SPDK_ERRLOG("Key '%s' does not exist\n", opts.psk);
		rc = -EINVAL;
		goto end;
	}

	rc = spdk_key_get_key(entry->key, psk_interchange, SPDK_TLS_PSK_MAX_LEN);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to retrieve PSK '%s'\n", opts.psk);
		rc = -EINVAL;
		goto end;
	}

	/* Parse PSK interchange to get length of base64 encoded data.
	 * This is then used to decide which cipher suite should be used
	 * to generate PSK identity and TLS PSK later on. */
	rc = nvme_tcp_parse_interchange_psk(psk_interchange, psk_configured, sizeof(psk_configured),
					    &psk_configured_size, &psk_retained_hash);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to parse PSK interchange!\n");
		goto end;
	}

	/* The Base64 string encodes the configured PSK (32 or 48 bytes binary).
	 * This check also ensures that psk_configured_size is smaller than
	 * psk_retained buffer size. */
	if (psk_configured_size == SHA256_DIGEST_LENGTH) {
		tls_cipher_suite = NVME_TCP_CIPHER_AES_128_GCM_SHA256;
	} else if (psk_configured_size == SHA384_DIGEST_LENGTH) {
		tls_cipher_suite = NVME_TCP_CIPHER_AES_256_GCM_SHA384;
	} else {
		SPDK_ERRLOG("Unrecognized cipher suite!\n");
		rc = -EINVAL;
		goto end;
	}

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	/* Generate PSK identity. */
	rc = nvme_tcp_generate_psk_identity(entry->pskid, sizeof(entry->pskid), hostnqn,
					    subsystem->subnqn, tls_cipher_suite);
	if (rc) {
		rc = -EINVAL;
		goto end;
	}
	/* Check if PSK identity entry already exists. */
	TAILQ_FOREACH(tmp, &ttransport->psks, link) {
		if (strncmp(tmp->pskid, entry->pskid, NVMF_PSK_IDENTITY_LEN) == 0) {
			SPDK_ERRLOG("Given PSK identity: %s entry already exists!\n", entry->pskid);
			rc = -EEXIST;
			goto end;
		}
	}

	if (snprintf(entry->hostnqn, sizeof(entry->hostnqn), "%s", hostnqn) < 0) {
		SPDK_ERRLOG("Could not write hostnqn string!\n");
		rc = -EINVAL;
		goto end;
	}
	if (snprintf(entry->subnqn, sizeof(entry->subnqn), "%s", subsystem->subnqn) < 0) {
		SPDK_ERRLOG("Could not write subnqn string!\n");
		rc = -EINVAL;
		goto end;
	}

	entry->tls_cipher_suite = tls_cipher_suite;

	/* No hash indicates that Configured PSK must be used as Retained PSK. */
	if (psk_retained_hash == NVME_TCP_HASH_ALGORITHM_NONE) {
		/* Psk configured is either 32 or 48 bytes long. */
		memcpy(entry->psk, psk_configured, psk_configured_size);
		entry->psk_size = psk_configured_size;
	} else {
		/* Derive retained PSK. */
		rc = nvme_tcp_derive_retained_psk(psk_configured, psk_configured_size, hostnqn, entry->psk,
						  SPDK_TLS_PSK_MAX_LEN, psk_retained_hash);
		if (rc < 0) {
			SPDK_ERRLOG("Unable to derive retained PSK!\n");
			goto end;
		}
		entry->psk_size = rc;
	}

	TAILQ_INSERT_TAIL(&ttransport->psks, entry, link);
	rc = 0;

end:
	spdk_memset_s(psk_configured, sizeof(psk_configured), 0, sizeof(psk_configured));
	spdk_memset_s(psk_interchange, sizeof(psk_interchange), 0, sizeof(psk_interchange));

	free(opts.psk);
	if (rc != 0) {
		nvmf_tcp_free_psk_entry(entry);
	}

	return rc;
}

static void
nvmf_tcp_subsystem_remove_host(struct spdk_nvmf_transport *transport,
			       const struct spdk_nvmf_subsystem *subsystem,
			       const char *hostnqn)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct tcp_psk_entry *entry, *tmp;

	assert(transport != NULL);
	assert(subsystem != NULL);

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	TAILQ_FOREACH_SAFE(entry, &ttransport->psks, link, tmp) {
		if ((strncmp(entry->hostnqn, hostnqn, SPDK_NVMF_NQN_MAX_LEN)) == 0 &&
		    (strncmp(entry->subnqn, subsystem->subnqn, SPDK_NVMF_NQN_MAX_LEN)) == 0) {
			TAILQ_REMOVE(&ttransport->psks, entry, link);
			nvmf_tcp_free_psk_entry(entry);
			break;
		}
	}
}

static void
nvmf_tcp_subsystem_dump_host(struct spdk_nvmf_transport *transport,
			     const struct spdk_nvmf_subsystem *subsystem, const char *hostnqn,
			     struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct tcp_psk_entry *entry;

	assert(transport != NULL);
	assert(subsystem != NULL);

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	TAILQ_FOREACH(entry, &ttransport->psks, link) {
		if ((strncmp(entry->hostnqn, hostnqn, SPDK_NVMF_NQN_MAX_LEN)) == 0 &&
		    (strncmp(entry->subnqn, subsystem->subnqn, SPDK_NVMF_NQN_MAX_LEN)) == 0) {
			spdk_json_write_named_string(w, "psk",  spdk_key_get_name(entry->key));
			break;
		}
	}
}

static void
nvmf_tcp_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		SPDK_NVMF_TCP_DEFAULT_MAX_IO_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr =	SPDK_NVMF_TCP_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size =	SPDK_NVMF_TCP_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =		SPDK_NVMF_TCP_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =		SPDK_NVMF_TCP_DEFAULT_IO_UNIT_SIZE;
	opts->max_aq_depth =		SPDK_NVMF_TCP_DEFAULT_MAX_ADMIN_QUEUE_DEPTH;
	opts->num_shared_buffers =	SPDK_NVMF_TCP_DEFAULT_NUM_SHARED_BUFFERS;
	opts->buf_cache_size =		SPDK_NVMF_TCP_DEFAULT_BUFFER_CACHE_SIZE;
	opts->dif_insert_or_strip =	SPDK_NVMF_TCP_DEFAULT_DIF_INSERT_OR_STRIP;
	opts->abort_timeout_sec =	SPDK_NVMF_TCP_DEFAULT_ABORT_TIMEOUT_SEC;
	opts->transport_specific =      NULL;
}

/*
 * [한국어] TCP transport ops 디스패치 테이블.
 * spdk_nvmf_transport_create("TCP", ...) 호출 시 본 ops 테이블이 lookup되어
 * 모든 transport-agnostic 호출이 본 파일의 함수로 위임된다. 새 transport를
 * 추가하려면 이 테이블 형식을 따라 모든 콜백을 구현하면 된다.
 */
const struct spdk_nvmf_transport_ops spdk_nvmf_transport_tcp = {
	.name = "TCP",			/* [한국어] JSON config의 trtype="TCP"와 매칭되는 이름 */
	.type = SPDK_NVME_TRANSPORT_TCP,	/* [한국어] NVMe-oF 스펙 trtype 값 */
	.opts_init = nvmf_tcp_opts_init,	/* [한국어] 디폴트 transport opts 채우기 */
	.create = nvmf_tcp_create,		/* [한국어] transport 인스턴스 생성 */
	.dump_opts = nvmf_tcp_dump_opts,	/* [한국어] JSON dump (RPC subsystem_get_config) */
	.destroy = nvmf_tcp_destroy,		/* [한국어] transport 인스턴스 파괴 */

	.listen = nvmf_tcp_listen,		/* [한국어] TCP listen 시작 */
	.stop_listen = nvmf_tcp_stop_listen,	/* [한국어] TCP listen 종료 */

	.listener_discover = nvmf_tcp_discover,	/* [한국어] discovery log entry 채우기 */

	.poll_group_create = nvmf_tcp_poll_group_create,	/* [한국어] per-CPU poll group 생성 */
	.get_optimal_poll_group = nvmf_tcp_get_optimal_poll_group,	/* [한국어] 새 qpair에 최적 poll group 선택 */
	.poll_group_destroy = nvmf_tcp_poll_group_destroy,	/* [한국어] poll group 파괴 */
	.poll_group_add = nvmf_tcp_poll_group_add,		/* [한국어] qpair를 poll group에 등록 */
	.poll_group_remove = nvmf_tcp_poll_group_remove,	/* [한국어] qpair를 poll group에서 제거 */
	.poll_group_poll = nvmf_tcp_poll_group_poll,		/* [한국어] reactor 매 iteration polling 본체 */

	.req_free = nvmf_tcp_req_free,				/* [한국어] req 강제 free (abort) */
	.req_complete = nvmf_tcp_req_complete,			/* [한국어] bdev 완료 후 transport에 통지 */
	.req_get_buffers_done = nvmf_tcp_req_get_buffers_done,	/* [한국어] 데이터 buffer 할당 완료 콜백 */

	.qpair_fini = nvmf_tcp_close_qpair,			/* [한국어] qpair 종료 */
	.qpair_get_local_trid = nvmf_tcp_qpair_get_local_trid,	/* [한국어] target 측 trid 추출 */
	.qpair_get_peer_trid = nvmf_tcp_qpair_get_peer_trid,	/* [한국어] 호스트 측 trid 추출 */
	.qpair_get_listen_trid = nvmf_tcp_qpair_get_listen_trid,	/* [한국어] listen trid 추출 */
	.qpair_abort_request = nvmf_tcp_qpair_abort_request,	/* [한국어] abort 명령 처리 */
	.subsystem_add_host = nvmf_tcp_subsystem_add_host,	/* [한국어] subsystem ACL host 추가 시 PSK 등록 */
	.subsystem_remove_host = nvmf_tcp_subsystem_remove_host,	/* [한국어] PSK 제거 */
	.subsystem_dump_host = nvmf_tcp_subsystem_dump_host,	/* [한국어] PSK JSON dump */
};

/* [한국어] 전역 transport registry에 본 TCP ops 등록.
 * 매크로 안에서 constructor attribute로 init 시 자동 등록 → spdk_nvmf_transport_lookup 가능. */
SPDK_NVMF_TRANSPORT_REGISTER(tcp, &spdk_nvmf_transport_tcp);
/* [한국어] 본 파일의 디버그 로그 컴포넌트 등록 — `--log-flags nvmf_tcp` 옵션으로 활성화. */
SPDK_LOG_REGISTER_COMPONENT(nvmf_tcp)
