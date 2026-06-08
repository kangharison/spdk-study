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

/* [한국어] FES(Fatal Error Status) 코드별 사람이 읽을 수 있는 문자열 배열.
 * C2HTermReq PDU의 fes 필드 값(0~5)에 대응하며, 에러 로그 출력 시 참조.
 * NVMe-TCP 스펙 Table 5 정의 순서와 일치해야 한다. */
static const char *spdk_nvmf_tcp_term_req_fes_str[] = {
	"Invalid PDU Header Field",
	"PDU Sequence Error",
	"Header Digiest Error",
	"Data Transfer Out of Range",
	"R2T Limit Exceeded",
	"Unsupported parameter",
};

/*
 * [한국어]
 * nvmf_tcp_trace - NVMe-TCP 전용 trace point 등록
 *
 * SPDK trace 프레임워크에 본 파일이 사용하는 모든 trace point를 등록한다.
 * SPDK_TRACE_REGISTER_FN 매크로에 의해 프로세스 시작 시 자동 호출 (constructor 패턴).
 *
 * 등록된 trace point는 `spdk_trace` 유틸리티로 바이너리 trace 데이터를 분석할 때
 * 각 이벤트를 식별하는 데 사용된다. req 상태 전이(16개), qpair 이벤트(7개),
 * bdev/sock 연관 관계(3개)를 포함한다.
 *
 * 실행 컨텍스트: 프로세스 init (constructor), single-thread.
 *
 * 호출 체인:
 *   process startup → [nvmf_tcp_trace] → spdk_trace_register_description (per point)
 */
static void
nvmf_tcp_trace(void)
{
	spdk_trace_register_owner_type(OWNER_TYPE_NVMF_TCP, 't');	/* [한국어] TCP qpair 소유자 타입 등록 — trace에서 't'로 식별 */
	spdk_trace_register_object(OBJECT_NVMF_TCP_IO, 'r');		/* [한국어] TCP I/O 요청 객체 등록 — trace에서 'r'로 식별 */
	spdk_trace_register_description("TCP_REQ_NEW",	/* [한국어] req 신규 할당 — NEW 상태 진입 (qd=현재 queue depth) */
					TRACE_TCP_REQUEST_STATE_NEW,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 1,
					SPDK_TRACE_ARG_TYPE_INT, "qd");
	spdk_trace_register_description("TCP_REQ_NEED_BUFFER",	/* [한국어] 데이터 buffer 대기 상태 */
					TRACE_TCP_REQUEST_STATE_NEED_BUFFER,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_HAVE_BUFFER",	/* [한국어] buffer 획득 완료 상태 */
					TRACE_TCP_REQUEST_STATE_HAVE_BUFFER,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_WAIT_ZCPY_START",	/* [한국어] zcopy_start 결과 대기 */
					TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_START,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_ZCPY_START_CPL",	/* [한국어] zcopy_start 완료 */
					TRACE_TCP_REQUEST_STATE_ZCOPY_START_COMPLETED,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_TX_H_TO_C",	/* [한국어] H2C(호스트→컨트롤러) 데이터 수신 중 */
					TRACE_TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_RDY_TO_EXECUTE",	/* [한국어] bdev 실행 준비 완료 */
					TRACE_TCP_REQUEST_STATE_READY_TO_EXECUTE,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_EXECUTING",	/* [한국어] bdev 실행 중 */
					TRACE_TCP_REQUEST_STATE_EXECUTING,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_WAIT_ZCPY_CMT",	/* [한국어] zcopy commit 대기 */
					TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_COMMIT,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_EXECUTED",	/* [한국어] bdev 실행 완료 */
					TRACE_TCP_REQUEST_STATE_EXECUTED,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_RDY_TO_COMPLETE",	/* [한국어] 응답 PDU 송신 준비 완료 */
					TRACE_TCP_REQUEST_STATE_READY_TO_COMPLETE,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_TRANSFER_C2H",	/* [한국어] C2H(컨트롤러→호스트) 데이터 전송 중 */
					TRACE_TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_AWAIT_ZCPY_RLS",	/* [한국어] zcopy release 대기 */
					TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_RELEASE,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_COMPLETED",	/* [한국어] req 완료 — free로 반환 (qd=반환 후 queue depth) */
					TRACE_TCP_REQUEST_STATE_COMPLETED,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "qd");
	spdk_trace_register_description("TCP_READ_DONE",	/* [한국어] 소켓에서 데이터 수신 완료 이벤트 */
					TRACE_TCP_READ_FROM_SOCKET_DONE,
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_REQ_AWAIT_R2T_ACK",	/* [한국어] R2T 송신 후 H2C 응답 대기 */
					TRACE_TCP_REQUEST_STATE_AWAIT_R2T_ACK,
					OWNER_TYPE_NVMF_TCP, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");

	spdk_trace_register_description("TCP_QP_CREATE", TRACE_TCP_QP_CREATE,	/* [한국어] qpair 생성 이벤트 */
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_SOCK_INIT", TRACE_TCP_QP_SOCK_INIT,	/* [한국어] qpair 소켓 초기화 */
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_STATE_CHANGE", TRACE_TCP_QP_STATE_CHANGE,	/* [한국어] qpair 라이프사이클 상태 전이 (state=목표 상태) */
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "state");
	spdk_trace_register_description("TCP_QP_DISCONNECT", TRACE_TCP_QP_DISCONNECT,	/* [한국어] qpair 연결 끊김 이벤트 */
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_DESTROY", TRACE_TCP_QP_DESTROY,	/* [한국어] qpair 메모리 해제 이벤트 */
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_ABORT_REQ", TRACE_TCP_QP_ABORT_REQ,	/* [한국어] abort 명령 처리 이벤트 */
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_RCV_STATE_CHANGE", TRACE_TCP_QP_RCV_STATE_CHANGE,	/* [한국어] PDU 수신 상태 머신 전이 (state=목표 수신 상태) */
					OWNER_TYPE_NVMF_TCP, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "state");

	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_START, OBJECT_NVMF_TCP_IO, 1);	/* [한국어] bdev IO 시작 이벤트와 TCP IO 연관 */
	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_DONE, OBJECT_NVMF_TCP_IO, 0);	/* [한국어] bdev IO 완료 이벤트와 TCP IO 연관 */
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_QUEUE, OBJECT_NVMF_TCP_IO, 0);	/* [한국어] sock 요청 큐잉 이벤트와 TCP IO 연관 */
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_PEND, OBJECT_NVMF_TCP_IO, 0);	/* [한국어] sock 요청 pending 이벤트와 TCP IO 연관 */
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_COMPLETE, OBJECT_NVMF_TCP_IO, 0);	/* [한국어] sock 요청 완료 이벤트와 TCP IO 연관 */
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

/* [한국어] TCP transport opts JSON 디코더 테이블.
 * nvmf_tcp_create() 내부에서 transport_specific JSON 객체를 tcp_opts 구조체로 매핑.
 * 각 항목은 {JSON키, 구조체 오프셋, 디코딩 함수, optional} 순으로 지정된다.
 * optional=true: 해당 키가 JSON에 없으면 기본값 유지. */
static const struct spdk_json_object_decoder tcp_transport_opts_decoder[] = {
	{
		"c2h_success", offsetof(struct tcp_transport_opts, c2h_success),
		/* [한국어] "c2h_success" JSON bool 필드 → tcp_opts.c2h_success 매핑 (optional) */
		spdk_json_decode_bool, true
	},
	{
		"control_msg_num", offsetof(struct tcp_transport_opts, control_msg_num),
		/* [한국어] "control_msg_num" JSON uint16 필드 → tcp_opts.control_msg_num 매핑 (optional) */
		spdk_json_decode_uint16, true
	},
	{
		"sock_priority", offsetof(struct tcp_transport_opts, sock_priority),
		/* [한국어] "sock_priority" JSON uint32 필드 → tcp_opts.sock_priority 매핑 (optional) */
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

/*
 * [한국어]
 * nvmf_tcp_req_pdu_init - req의 전용 PDU 슬롯을 초기화하고 반환
 *
 * @tcp_req: 초기화할 req
 * @return:  0으로 초기화된 tcp_req->pdu (qpair 링크 설정 포함)
 *
 * 송신용 PDU를 빌드하기 전에 반드시 호출하여 이전 사용 잔여물을 제거한다.
 * pdu_in_use 플래그가 false인지 assert로 검증 — 동일 PDU를 동시에 이중 사용하는
 * 버그를 조기에 잡는다. memset 범위는 전체 pdu 크기이므로 qpair 포인터도
 * 일단 0으로 지워진 뒤 바로 올바른 값으로 재설정한다.
 *
 * 호출 체인:
 *   send_r2t_pdu / send_capsule_resp_pdu / nvmf_tcp_send_c2h_data → [nvmf_tcp_req_pdu_init]
 */
static inline struct nvme_tcp_pdu *
nvmf_tcp_req_pdu_init(struct spdk_nvmf_tcp_req *tcp_req)
{
	assert(tcp_req->pdu_in_use == false);	/* [한국어] 이중 사용 버그 감지 — PDU가 이미 in-flight이면 assert */

	memset(tcp_req->pdu, 0, sizeof(*tcp_req->pdu));	/* [한국어] 이전 사용 내용 전체 클리어 (헤더·data 포인터·체크섬 포함) */
	tcp_req->pdu->qpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);
	/* [한국어] qpair 포인터 복원 — memset으로 지워졌으므로 다운캐스트로 재설정 */

	return tcp_req->pdu;	/* [한국어] 초기화된 PDU 슬롯 반환 — 호출자가 헤더 필드를 채운 뒤 write_pdu 호출 */
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

/*
 * [한국어]
 * handle_await_req - AWAIT_REQ 상태에서 대기 중인 qpair에 PDU 처리를 재개시키는 메시지 콜백
 *
 * @arg: tqpair 포인터 (spdk_thread_send_msg의 cb_arg)
 *
 * nvmf_tcp_req_put에서 req가 free queue로 반환될 때 qpair가 AWAIT_REQ 상태이면
 * 이 메시지를 같은 thread에 enqueue한다. 메시지가 처리될 시점에 qpair가 여전히
 * AWAIT_REQ 상태이면 qpair_process를 호출해 다음 CapsuleCmd PDU 처리를 재개한다.
 *
 * await_req_msg_pending 플래그로 동일 메시지가 중복 enqueue되지 않도록 보호.
 *
 * 실행 컨텍스트: poll group spdk_thread (메시지 콜백, 동일 thread).
 *
 * 호출 체인:
 *   nvmf_tcp_req_put → spdk_thread_send_msg → [handle_await_req] → nvmf_tcp_qpair_process
 */
static void
handle_await_req(void *arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair = arg;	/* [한국어] 메시지 cb_arg를 tqpair로 캐스트 */

	tqpair->await_req_msg_pending = false;	/* [한국어] 메시지 처리 완료 — 플래그 해제해 다음 필요 시 재 enqueue 허용 */
	if (tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_REQ) {
		/* [한국어] 여전히 AWAIT_REQ 상태 — free된 req 슬롯을 사용해 다음 CapsuleCmd 처리 재개 */
		nvmf_tcp_qpair_process(tqpair);
	}
}

/*
 * [한국어]
 * nvmf_tcp_req_put - 완료된 req를 free queue로 반환 (working_queue 제거 + free_queue 삽입)
 *
 * @tqpair:   req가 속한 TCP qpair
 * @tcp_req:  반환할 req (COMPLETED 상태에서 호출됨)
 *
 * nvmf_tcp_req_process의 COMPLETED case에서 호출되어 req 슬롯을 재활용 풀로 돌려보낸다.
 * pdu_in_use 상태에서는 절대 호출 불가 — PDU 송신 완료 전 free를 금지.
 * free 후 qpair가 AWAIT_REQ 상태이면 handle_await_req 메시지를 enqueue해
 * 다음 PDU 수신이 재개되도록 한다.
 *
 * 실행 컨텍스트: poll group spdk_thread (단일 thread, 락 불필요).
 *
 * 호출 체인:
 *   nvmf_tcp_req_process (COMPLETED case) → [nvmf_tcp_req_put]
 *     → spdk_thread_send_msg(handle_await_req)
 */
static inline void
nvmf_tcp_req_put(struct spdk_nvmf_tcp_qpair *tqpair, struct spdk_nvmf_tcp_req *tcp_req)
{
	assert(!tcp_req->pdu_in_use);	/* [한국어] 송신 중 PDU를 가진 req는 반환 불가 — 이중 free 방지 */

	TAILQ_REMOVE(&tqpair->tcp_req_working_queue, tcp_req, state_link);	/* [한국어] working 리스트에서 제거 */
	TAILQ_INSERT_TAIL(&tqpair->tcp_req_free_queue, tcp_req, state_link);	/* [한국어] free 리스트 끝에 삽입 (FIFO 재활용) */
	tqpair->qpair.queue_depth--;	/* [한국어] QD 카운터 감소 — 호스트가 다음 명령을 보낼 수 있는 슬롯이 복구됨 */
	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_FREE);	/* [한국어] FREE 상태로 전이 (cntr 갱신) */
	if (tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_REQ &&
	    !tqpair->await_req_msg_pending) {
		/* [한국어] qpair가 req 슬롯 부족으로 AWAIT_REQ 상태로 멈춰있었다면
		 * 방금 반환된 슬롯을 쓸 수 있으므로 처리 재개 메시지 전송.
		 * await_req_msg_pending으로 중복 enqueue 방지. */
		tqpair->await_req_msg_pending = true;	/* [한국어] 메시지 enqueue 완료 표시 */
		spdk_thread_send_msg(spdk_get_thread(), handle_await_req, tqpair);
		/* [한국어] 동일 thread에 메시지 enqueue — 현재 콜백 스택 종료 후 처리됨 */
	}
}

/*
 * [한국어]
 * nvmf_tcp_req_get_buffers_done - 데이터 buffer 비동기 할당 완료 콜백 (transport_ops.req_get_buffers_done)
 *
 * @req: buffer 할당이 완료된 nvmf request
 *
 * spdk_nvmf_request_get_buffers()가 즉시 버퍼를 반환하지 못해 대기큐에 들어간 경우,
 * 버퍼가 생기면 이 콜백이 호출된다. NEED_BUFFER → HAVE_BUFFER로 전이 후 req_process로
 * 다음 단계를 진행한다 (in-capsule data 수신 또는 R2T 송신 단계).
 *
 * 실행 컨텍스트: poll group spdk_thread (버퍼 풀 해제 시 같은 thread에서 콜백).
 *
 * 호출 체인:
 *   nvmf_transport_req_get_buffers_done → [nvmf_tcp_req_get_buffers_done] → nvmf_tcp_req_process
 */
static void
nvmf_tcp_req_get_buffers_done(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_req *tcp_req;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_tcp_transport *ttransport;

	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_req, req);	/* [한국어] 공통 req → TCP req 다운캐스트 */
	transport = req->qpair->transport;					/* [한국어] qpair를 통해 transport 참조 */
	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);	/* [한국어] 공통 transport → TCP transport 다운캐스트 */

	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_HAVE_BUFFER);	/* [한국어] NEED_BUFFER → HAVE_BUFFER 전이 */
	nvmf_tcp_req_process(ttransport, tcp_req);			/* [한국어] 상태 머신 다음 단계 진행 */
}

/*
 * [한국어]
 * nvmf_tcp_request_free - req를 COMPLETED 상태로 강제 전이시켜 정리 절차 시작
 *
 * @cb_arg: spdk_nvmf_tcp_req 포인터 (PDU 송신 완료 콜백의 cb_arg로 사용됨)
 *
 * 주로 PDU 송신 완료 콜백(_req_pdu_write_done 등)의 cb_fn으로 등록되어 호출된다.
 * COMPLETED 상태로 전이 후 req_process를 호출하면 COMPLETED case에서
 * nvmf_tcp_req_put이 호출되어 req가 free queue로 반환된다.
 *
 * 실행 컨텍스트: poll group spdk_thread (sock writev_async 완료 콜백).
 *
 * 호출 체인:
 *   _req_pdu_write_done → [nvmf_tcp_request_free] → nvmf_tcp_req_process(COMPLETED)
 *     → nvmf_tcp_req_put
 */
static void
nvmf_tcp_request_free(void *cb_arg)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_req *tcp_req = cb_arg;	/* [한국어] cb_arg를 tcp_req로 캐스트 */

	assert(tcp_req != NULL);	/* [한국어] NULL 포인터는 논리 오류 — 조기 차단 */

	SPDK_DEBUGLOG(nvmf_tcp, "tcp_req=%p will be freed\n", tcp_req);
	ttransport = SPDK_CONTAINEROF(tcp_req->req.qpair->transport,
				      struct spdk_nvmf_tcp_transport, transport);
	/* [한국어] req의 qpair → transport → TCP transport 다운캐스트 */
	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_COMPLETED);	/* [한국어] 현재 상태 → COMPLETED 강제 전이 */
	nvmf_tcp_req_process(ttransport, tcp_req);	/* [한국어] COMPLETED case에서 버퍼 반환 + req_put 실행 */
}

/*
 * [한국어]
 * nvmf_tcp_req_free - transport_ops.req_free 콜백 구현 (nvmf_request 인터페이스 래퍼)
 *
 * @req: 해제할 공통 nvmf request
 *
 * transport-agnostic 레이어가 req를 강제 해제할 때 사용 (abort/disconnect 시).
 * 내부적으로 nvmf_tcp_request_free로 위임하여 COMPLETED → req_put 경로로 처리.
 *
 * 호출 체인:
 *   nvmf_ctrlr_abort_request / disconnect path → [nvmf_tcp_req_free]
 *     → nvmf_tcp_request_free → nvmf_tcp_req_process(COMPLETED)
 */
static void
nvmf_tcp_req_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_req *tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_req, req);
	/* [한국어] 공통 req → TCP req 다운캐스트 후 request_free로 위임 */

	nvmf_tcp_request_free(tcp_req);	/* [한국어] COMPLETED 전이 + req_put 처리 */
}

/*
 * [한국어]
 * nvmf_tcp_drain_state_queue - 특정 상태의 req들을 working queue에서 강제 완료 처리
 *
 * @tqpair: 대상 TCP qpair
 * @state:  드레인(강제 완료)할 req 상태 (FREE 상태는 이미 free이므로 제외)
 *
 * qpair 종료/disconnect 시 진행 중인 req들을 상태별로 정리하기 위해 호출된다.
 * TAILQ_FOREACH_SAFE를 사용해 순회 중 req가 free_queue로 이동해도 안전하게 처리.
 *
 * 실행 컨텍스트: poll group spdk_thread (cleanup_all_states에서 호출).
 *
 * 호출 체인:
 *   nvmf_tcp_cleanup_all_states → [nvmf_tcp_drain_state_queue] → nvmf_tcp_request_free
 */
static void
nvmf_tcp_drain_state_queue(struct spdk_nvmf_tcp_qpair *tqpair,
			   enum spdk_nvmf_tcp_req_state state)
{
	struct spdk_nvmf_tcp_req *tcp_req, *req_tmp;

	assert(state != TCP_REQUEST_STATE_FREE);	/* [한국어] FREE 상태는 이미 free queue에 있으므로 드레인 불필요 */
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->tcp_req_working_queue, state_link, req_tmp) {
		/* [한국어] FOREACH_SAFE: req가 free되어 리스트에서 제거되어도 req_tmp로 안전 순회 */
		if (state == tcp_req->state) {
			nvmf_tcp_request_free(tcp_req);	/* [한국어] 해당 상태의 req 강제 완료 처리 */
		}
	}
}

/*
 * [한국어]
 * nvmf_tcp_request_get_buffers_abort - NEED_BUFFER 상태로 대기 중인 req를 대기 큐에서 제거
 *
 * @tcp_req: abort할 req (반드시 NEED_BUFFER 상태)
 *
 * req는 두 가지 대기 큐 중 하나에 있을 수 있다:
 *   1) control_msg_list->waiting_for_msg_reqs: 작은 ICD 명령용 control msg 부족 대기
 *   2) transport 공통 iobuf 대기 큐 (nvmf_request_get_buffers_abort로 처리)
 * abort 요청(qpair disconnect/abort cmd) 시 이 함수로 대기 큐에서 제거한다.
 *
 * 실행 컨텍스트: poll group spdk_thread.
 *
 * 호출 체인:
 *   nvmf_tcp_abort_await_buffer_reqs / _nvmf_tcp_qpair_abort_request
 *     → [nvmf_tcp_request_get_buffers_abort]
 */
static inline void
nvmf_tcp_request_get_buffers_abort(struct spdk_nvmf_tcp_req *tcp_req)
{
	/* Request can wait either for the iobuf or control_msg */
	struct spdk_nvmf_qpair *qpair = tcp_req->req.qpair;	/* [한국어] 공통 qpair 참조 */
	struct spdk_nvmf_tcp_qpair *tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	/* [한국어] 공통 qpair → TCP qpair 다운캐스트 */
	struct spdk_nvmf_tcp_poll_group *tcp_group = tqpair->group;	/* [한국어] 이 qpair가 속한 poll group */
	struct spdk_nvmf_tcp_req *tmp_req, *abort_req;

	assert(tcp_req->state == TCP_REQUEST_STATE_NEED_BUFFER);	/* [한국어] NEED_BUFFER 상태가 아니면 논리 오류 */

	if (tcp_group->control_msg_list != NULL) {
		/* [한국어] control_msg 대기 큐가 존재하면 먼저 거기서 찾아본다 */
		STAILQ_FOREACH_SAFE(abort_req, &tcp_group->control_msg_list->waiting_for_msg_reqs,
				    control_msg_link, tmp_req) {
			/* [한국어] FOREACH_SAFE: 순회 중 req 제거 시 tmp_req로 안전 진행 */
			if (abort_req == tcp_req) {
				/* [한국어] 찾았다 — control_msg 대기 큐에서 제거 후 완료 */
				STAILQ_REMOVE(&tcp_group->control_msg_list->waiting_for_msg_reqs,
					      abort_req, spdk_nvmf_tcp_req, control_msg_link);
				return;
			}
		}
	}

	/* [한국어] control_msg 대기 큐에 없으면 transport 공통 iobuf 대기 큐에서 abort */
	if (!nvmf_request_get_buffers_abort(&tcp_req->req)) {
		SPDK_ERRLOG("Failed to abort tcp_req=%p\n", tcp_req);
		assert(0 && "Should never happen");	/* [한국어] 두 큐 모두 없으면 논리 오류 — assert crash */
	}
}

/*
 * [한국어]
 * nvmf_tcp_abort_await_buffer_reqs - qpair의 버퍼 대기 중인 모든 req를 abort 처리
 *
 * @tqpair: 대상 TCP qpair
 *
 * qpair가 poll group에서 제거될 때(nvmf_tcp_poll_group_remove) 호출되어
 * NEED_BUFFER 상태로 iobuf/control_msg를 기다리는 req들을 모두 정리한다.
 * 이 req들은 완료 콜백이 도달하지 않으므로 명시적으로 대기 큐에서 제거 후
 * COMPLETED로 전이시켜 cleanup 경로로 유도한다.
 *
 * 실행 컨텍스트: poll group spdk_thread.
 *
 * 호출 체인:
 *   nvmf_tcp_poll_group_remove → [nvmf_tcp_abort_await_buffer_reqs]
 *     → nvmf_tcp_request_get_buffers_abort (대기 큐 제거)
 *     → nvmf_tcp_req_set_state(COMPLETED)
 */
static void
nvmf_tcp_abort_await_buffer_reqs(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_req *tcp_req, *req_tmp;

	/* Remove requests waiting for buffer from the waiting list and mark as completed */
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->tcp_req_working_queue, state_link, req_tmp) {
		/* [한국어] FOREACH_SAFE: req 상태 변경 시에도 안전하게 순회 */
		if (tcp_req->state == TCP_REQUEST_STATE_NEED_BUFFER) {
			nvmf_tcp_request_get_buffers_abort(tcp_req);	/* [한국어] iobuf/control_msg 대기 큐에서 제거 */
			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_COMPLETED);
			/* [한국어] COMPLETED로 전이 — cleanup_all_states에서 처리될 예정 */
		}
	}
}

/*
 * [한국어]
 * nvmf_tcp_cleanup_all_states - qpair 종료 시 모든 활성 req 강제 완료 처리
 *
 * @tqpair: 정리할 TCP qpair
 *
 * _nvmf_tcp_qpair_destroy에서 호출되어 소켓이 닫힌 후 남아 있는 모든 req를
 * 순서에 맞게 drain한다. 상태 순서를 지켜야 하는 이유:
 *   - TRANSFERRING_CONTROLLER_TO_HOST: 소켓 닫히면 pdu write 완료 안 오므로 먼저 정리
 *   - NEW, EXECUTING, TRANSFERRING_HOST_TO_CONTROLLER, AWAITING_R2T_ACK: 중간 단계
 *   - COMPLETED: drain_state_queue가 request_free를 호출해 상태가 COMPLETED로 전이된 것들 최종 정리
 *
 * 실행 컨텍스트: poll group spdk_thread (qpair destroy 메시지 콜백).
 *
 * 호출 체인:
 *   _nvmf_tcp_qpair_destroy → [nvmf_tcp_cleanup_all_states]
 *     → nvmf_tcp_drain_state_queue (상태별 반복)
 */
static void
nvmf_tcp_cleanup_all_states(struct spdk_nvmf_tcp_qpair *tqpair)
{
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	/* [한국어] C2H 데이터 전송 중인 req 먼저 정리 — 소켓 닫혀 완료 콜백 안 오므로 */
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_NEW);
	/* [한국어] 방금 받은 req (cmd 복사 전) 정리 */
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_EXECUTING);
	/* [한국어] bdev 실행 중인 req 정리 — bdev가 비동기로 완료 예정이므로 중단 */
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
	/* [한국어] H2C 데이터 수신 중인 req 정리 */
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_AWAITING_R2T_ACK);
	/* [한국어] R2T 송신 후 H2C 응답 대기 중인 req 정리 */
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_COMPLETED);
	/* [한국어] 위 drain들이 COMPLETED로 전이시킨 req들 최종 정리 */
}

/*
 * [한국어]
 * nvmf_tcp_dump_qpair_req_contents - qpair의 req 상태별 내용을 에러 로그로 덤프
 *
 * @tqpair: 덤프할 TCP qpair
 *
 * _nvmf_tcp_qpair_destroy에서 cleanup 후 FREE 상태 req 수가 resource_count와
 * 다를 때(req 누수/불일치) 호출되어 디버깅 정보를 출력한다.
 * 모든 비-FREE 상태의 req 수와 각 req의 data_from_pool / opcode를 로깅한다.
 *
 * 실행 컨텍스트: poll group spdk_thread (qpair destroy 경로).
 *
 * 호출 체인:
 *   _nvmf_tcp_qpair_destroy → [nvmf_tcp_dump_qpair_req_contents]
 */
static void
nvmf_tcp_dump_qpair_req_contents(struct spdk_nvmf_tcp_qpair *tqpair)
{
	int i;
	struct spdk_nvmf_tcp_req *tcp_req;

	SPDK_ERRLOG("Dumping contents of queue pair (QID %d)\n", tqpair->qpair.qid);
	for (i = 1; i < TCP_REQUEST_NUM_STATES; i++) {
		/* [한국어] FREE(0) 제외 — 상태 1~15까지 카운터와 상세 내용 출력 */
		SPDK_ERRLOG("\tNum of requests in state[%d] = %u\n", i, tqpair->state_cntr[i]);
		TAILQ_FOREACH(tcp_req, &tqpair->tcp_req_working_queue, state_link) {
			if ((int)tcp_req->state == i) {
				SPDK_ERRLOG("\t\tRequest Data From Pool: %d\n", tcp_req->req.data_from_pool);
				/* [한국어] data_from_pool=1이면 공유 버퍼 풀 사용 중 (반환 필요) */
				SPDK_ERRLOG("\t\tRequest opcode: %d\n", tcp_req->req.cmd->nvmf_cmd.opcode);
				/* [한국어] NVMe opcode 출력 — 어떤 명령이 걸려 있는지 확인 */
			}
		}
	}
}

/*
 * [한국어]
 * _nvmf_tcp_qpair_destroy - qpair 메모리 전체 해제 및 콜백 통지 (실제 destroy 본체)
 *
 * @_tqpair: 파괴할 tqpair (spdk_thread_send_msg의 cb_arg)
 *
 * nvmf_tcp_qpair_destroy에서 메시지로 비동기 지연된 실제 파괴 함수.
 * 소켓 콜백 컨텍스트에서 직접 호출하면 spdk_sock_close가 pending req를 abort하지
 * 못하고 사용 후 해제(use-after-free)를 일으킬 수 있어 메시지로 지연한다 (#2471 수정).
 *
 * 단계:
 *   1) trace 기록
 *   2) 소켓 close (에러 시 NULL 수동 설정)
 *   3) 남은 req 모두 강제 완료 (cleanup_all_states)
 *   4) req 카운터 불일치 검사 + 덤프 (디버그)
 *   5) timeout_poller 해제
 *   6) DMA 메모리(pdus), reqs, bufs 순서대로 free
 *   7) trace owner 해제
 *   8) tqpair 구조체 free
 *   9) fini_cb_fn 콜백 호출 (transport common layer에 완료 통지)
 *
 * 실행 컨텍스트: poll group spdk_thread (메시지 콜백).
 *
 * 호출 체인:
 *   nvmf_tcp_qpair_destroy → spdk_thread_send_msg → [_nvmf_tcp_qpair_destroy]
 *     → fini_cb_fn (transport common layer)
 */
static void
_nvmf_tcp_qpair_destroy(void *_tqpair)
{
	struct spdk_nvmf_tcp_qpair *tqpair = _tqpair;	/* [한국어] cb_arg를 tqpair로 캐스트 */
	spdk_nvmf_transport_qpair_fini_cb cb_fn = tqpair->fini_cb_fn;	/* [한국어] fini 완료 콜백 미리 저장 (free 전에) */
	void *cb_arg = tqpair->fini_cb_arg;			/* [한국어] 완료 콜백 인자 미리 저장 */
	int rc, err = 0;

	spdk_trace_record(TRACE_TCP_QP_DESTROY, tqpair->qpair.trace_id, 0, 0);
	/* [한국어] qpair destroy 이벤트 trace 기록 */

	SPDK_DEBUGLOG(nvmf_tcp, "enter\n");

	rc = spdk_sock_close(&tqpair->sock);	/* [한국어] TCP 소켓 닫기 — 커널에 FIN 전송 + fd 반환 */
	if (rc < 0 || tqpair->sock) {
		SPDK_ERRLOG("spdk_sock_close() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		/* Set it to NULL manually */
		tqpair->sock = NULL;	/* [한국어] close 실패 시 수동으로 NULL 설정해 이후 접근 방지 */
	}

	assert(err == 0);	/* [한국어] 여기까지 err가 0이어야 정상 — 이후 req 불일치 검사 시작 */
	nvmf_tcp_cleanup_all_states(tqpair);	/* [한국어] 소켓 닫힌 후 남은 req들 강제 완료 처리 */

	if (tqpair->state_cntr[TCP_REQUEST_STATE_FREE] != tqpair->resource_count) {
		/* [한국어] cleanup 후에도 FREE 상태 req 수가 resource_count와 다르면 누수 */
		SPDK_ERRLOG("tqpair(%p) free tcp request num is %u but should be %u\n", tqpair,
			    tqpair->state_cntr[TCP_REQUEST_STATE_FREE],
			    tqpair->resource_count);
		err++;	/* [한국어] 불일치 카운터 증가 — 이후 덤프 트리거 */
	}

	if (err > 0) {
		nvmf_tcp_dump_qpair_req_contents(tqpair);	/* [한국어] 불일치 있으면 모든 req 상태 상세 에러 로그 출력 */
	}

	/* The timeout poller might still be registered here if we close the qpair before host
	 * terminates the connection.
	 */
	spdk_poller_unregister(&tqpair->timeout_poller);	/* [한국어] wait_terminate timeout poller 해제 (이미 없으면 no-op) */
	spdk_dma_free(tqpair->pdus);	/* [한국어] DMA 페이지 정렬 PDU 배열 해제 (spdk_dma_zmalloc으로 할당됨) */
	free(tqpair->reqs);		/* [한국어] req 객체 배열 해제 (calloc으로 할당됨) */
	spdk_free(tqpair->bufs);	/* [한국어] in-capsule data 버퍼 해제 (spdk_zmalloc으로 할당됨) */
	spdk_trace_unregister_owner(tqpair->qpair.trace_id);	/* [한국어] trace 소유자 등록 해제 */
	free(tqpair);			/* [한국어] tqpair 구조체 자체 해제 — 이후 tqpair 접근 불가 */

	if (cb_fn != NULL) {
		cb_fn(cb_arg);	/* [한국어] transport common layer에 fini 완료 통지 — 연결된 ctrlr 정리 등 후속 처리 */
	}

	SPDK_DEBUGLOG(nvmf_tcp, "Leave\n");
}

/*
 * [한국어]
 * nvmf_tcp_qpair_destroy - qpair 파괴를 소켓 콜백 컨텍스트에서 분리하는 래퍼
 *
 * @tqpair: 파괴할 TCP qpair
 *
 * 소켓 이벤트 콜백 안에서 직접 spdk_sock_close를 호출하면 해당 콜백이 반환된 후에도
 * libspdk가 pending writev_async 완료를 처리하려 해제된 소켓을 접근할 수 있다
 * (use-after-free). 이를 방지하기 위해 spdk_thread_send_msg로 실제 파괴를
 * 현재 스택이 빠져나온 뒤로 지연한다. (이슈 #2471 수정)
 *
 * 실행 컨텍스트: poll group spdk_thread (qpair fini 요청 경로).
 *
 * 호출 체인:
 *   nvmf_tcp_port_accept(에러) / nvmf_tcp_close_qpair → [nvmf_tcp_qpair_destroy]
 *     → spdk_thread_send_msg → _nvmf_tcp_qpair_destroy
 */
static void
nvmf_tcp_qpair_destroy(struct spdk_nvmf_tcp_qpair *tqpair)
{
	/* Delay the destruction to make sure it isn't performed from the context of a sock
	 * callback.  Otherwise, spdk_sock_close() might not abort pending requests, causing their
	 * completions to be executed after the qpair is freed.  (Note: this fixed issue #2471.)
	 */
	spdk_thread_send_msg(spdk_get_thread(), _nvmf_tcp_qpair_destroy, tqpair);
	/* [한국어] 현재 spdk_thread에 _nvmf_tcp_qpair_destroy 메시지 enqueue — 소켓 콜백 스택 종료 후 실행됨 */
}

/*
 * [한국어]
 * nvmf_tcp_dump_opts - TCP transport 설정값을 JSON으로 직렬화 (transport_ops.dump_opts 콜백)
 *
 * @transport: 직렬화할 transport
 * @w:         JSON write 컨텍스트 (nvmf RPC layer가 전달)
 *
 * "nvmf_get_transports" RPC 응답에 TCP 특화 옵션을 포함시키기 위해 호출된다.
 * 현재 c2h_success(C2HData 성공 응답 생략 최적화 여부)와 sock_priority(SO_PRIORITY 값)를 기록.
 *
 * 실행 컨텍스트: 관리 스레드 (RPC 처리).
 *
 * 호출 체인:
 *   nvmf_rpc_get_transports_ctx 처리 → [nvmf_tcp_dump_opts]
 */
static void
nvmf_tcp_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_tcp_transport	*ttransport;
	assert(w != NULL);	/* [한국어] JSON write 컨텍스트 NULL은 프로그래밍 오류 */

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	/* [한국어] 공통 transport → TCP transport 다운캐스트 */
	spdk_json_write_named_bool(w, "c2h_success", ttransport->tcp_opts.c2h_success);
	/* [한국어] c2h_success: true면 C2HData에 별도 CapsuleResp 생략 (C2H 최적화 활성화 여부) */
	spdk_json_write_named_uint32(w, "sock_priority", ttransport->tcp_opts.sock_priority);
	/* [한국어] sock_priority: SO_PRIORITY 소켓 우선순위 값 (0=기본, 높을수록 우선 스케줄링) */
}

/*
 * [한국어]
 * nvmf_tcp_free_psk_entry - PSK 엔트리 메모리를 안전하게 해제 (PSK 값 zeroize)
 *
 * @entry: 해제할 tcp_psk_entry (NULL 허용)
 *
 * PSK(Pre-Shared Key) 버퍼를 spdk_memset_s로 안전하게 0으로 덮어쓴 뒤 keyring 참조 반환,
 * 구조체 메모리 해제. zeroize 없이 free하면 PSK 키 소재가 메모리에 잔류할 수 있으므로
 * 반드시 zeroize 후 해제한다.
 *
 * 실행 컨텍스트: 관리 스레드 (transport destroy / host remove 경로).
 *
 * 호출 체인:
 *   nvmf_tcp_destroy / nvmf_tcp_subsystem_remove_host → [nvmf_tcp_free_psk_entry]
 */
static void
nvmf_tcp_free_psk_entry(struct tcp_psk_entry *entry)
{
	if (entry == NULL) {
		return;	/* [한국어] NULL 엔트리는 no-op — 방어적 처리 */
	}

	spdk_memset_s(entry->psk, sizeof(entry->psk), 0, sizeof(entry->psk));
	/* [한국어] PSK 키 소재를 0으로 덮어씀 — 컴파일러 최적화로 제거 안 되는 안전 memset */
	spdk_keyring_put_key(entry->key);
	/* [한국어] keyring 참조 카운트 감소 — 마지막 참조면 keyring에서 키 제거 */
	free(entry);	/* [한국어] psk_entry 구조체 자체 해제 */
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

/*
 * [한국어]
 * nvmf_tcp_trsvcid_to_int - NVMe-oF trsvcid 문자열을 정수 TCP 포트 번호로 변환
 *
 * @trsvcid: "4420"과 같은 십진수 포트 번호 문자열
 * @return:  성공 시 1~65535 범위의 포트 번호; 실패(비숫자, 0, 65535 초과) 시 -1
 *
 * nvmf_tcp_canon_listen_trid에서 호출되어 사용자 제공 trsvcid를 검증한다.
 * strtoull로 파싱 후 전체 소비 여부(*end == '\0')와 범위를 검사한다.
 *
 * 호출 체인:
 *   nvmf_tcp_canon_listen_trid → [nvmf_tcp_trsvcid_to_int]
 */
static int
nvmf_tcp_trsvcid_to_int(const char *trsvcid)
{
	unsigned long long ull;
	char *end = NULL;

	ull = strtoull(trsvcid, &end, 10);	/* [한국어] 십진수 파싱 — end는 파싱이 멈춘 위치 */
	if (end == NULL || end == trsvcid || *end != '\0') {
		/* [한국어] end == trsvcid: 한 글자도 파싱 못함; *end != '\0': 비숫자 문자 남음 */
		return -1;
	}

	/* Valid TCP/IP port numbers are in [1, 65535] */
	if (ull == 0 || ull > 65535) {
		/* [한국어] 0번 포트 비허용; 65535 초과는 TCP 포트 범위 벗어남 */
		return -1;
	}

	return (int)ull;	/* [한국어] 유효한 포트 번호 반환 */
}

/**
 * Canonicalize a listen address trid.
 */
/*
 * [한국어]
 * nvmf_tcp_canon_listen_trid - listen trid를 표준(canonical) 형식으로 정규화
 *
 * @canon_trid: 결과를 저장할 빈 trid 구조체 (출력)
 * @trid:       사용자가 제공한 원본 trid (입력)
 * @return:     0 성공; -EINVAL trsvcid 파싱 실패
 *
 * trsvcid를 정수로 변환해 십진수 문자열로 재포맷(예: " 4420" → "4420")하고
 * trtype을 SPDK_NVME_TRANSPORT_TCP로 명시적으로 설정한다. 이로써 find_port에서
 * strcmp 기반 비교가 올바르게 동작한다.
 *
 * 호출 체인:
 *   nvmf_tcp_find_port / nvmf_tcp_listen / nvmf_tcp_stop_listen → [nvmf_tcp_canon_listen_trid]
 */
static int
nvmf_tcp_canon_listen_trid(struct spdk_nvme_transport_id *canon_trid,
			   const struct spdk_nvme_transport_id *trid)
{
	int trsvcid_int;

	trsvcid_int = nvmf_tcp_trsvcid_to_int(trid->trsvcid);	/* [한국어] 포트 문자열을 정수로 변환 + 범위 검증 */
	if (trsvcid_int < 0) {
		return -EINVAL;	/* [한국어] 유효하지 않은 포트 번호 */
	}

	memset(canon_trid, 0, sizeof(*canon_trid));	/* [한국어] 출력 구조체 초기화 */
	spdk_nvme_trid_populate_transport(canon_trid, SPDK_NVME_TRANSPORT_TCP);
	/* [한국어] trtype/trstring을 "TCP"로 명시 설정 */
	canon_trid->adrfam = trid->adrfam;	/* [한국어] IPv4/IPv6 주소 계열 복사 */
	snprintf(canon_trid->traddr, sizeof(canon_trid->traddr), "%s", trid->traddr);
	/* [한국어] IP 주소 문자열 복사 */
	snprintf(canon_trid->trsvcid, sizeof(canon_trid->trsvcid), "%d", trsvcid_int);
	/* [한국어] 정수 포트를 표준 십진 문자열로 재포맷 (공백 제거 등) */

	return 0;
}

/**
 * Find an existing listening port.
 */
/*
 * [한국어]
 * nvmf_tcp_find_port - 이미 listen 중인 포트를 trid로 검색
 *
 * @ttransport: TCP transport 인스턴스 (ports TAILQ 소유)
 * @trid:       찾을 포트의 trid (traddr + trsvcid)
 * @return:     일치하는 tcp_port 포인터; 없으면 NULL
 *
 * trid를 canon_trid로 정규화한 뒤 ttransport->ports 리스트를 순회하여
 * 동일한 trid를 가진 포트를 반환한다. nvmf_tcp_listen에서 중복 listen 방지,
 * nvmf_tcp_stop_listen에서 제거 대상 포트를 찾는 데 사용된다.
 *
 * 호출 체인:
 *   nvmf_tcp_listen / nvmf_tcp_stop_listen → [nvmf_tcp_find_port]
 */
static struct spdk_nvmf_tcp_port *
nvmf_tcp_find_port(struct spdk_nvmf_tcp_transport *ttransport,
		   const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvme_transport_id canon_trid;
	struct spdk_nvmf_tcp_port *port;

	if (nvmf_tcp_canon_listen_trid(&canon_trid, trid) != 0) {
		return NULL;	/* [한국어] trsvcid 파싱 실패 — 존재하지 않는 것과 동일하게 처리 */
	}

	TAILQ_FOREACH(port, &ttransport->ports, link) {
		/* [한국어] 등록된 포트 리스트 순회 — trid 완전 비교 */
		if (spdk_nvme_transport_id_compare(&canon_trid, port->trid) == 0) {
			return port;	/* [한국어] 일치하는 포트 발견 */
		}
	}

	return NULL;	/* [한국어] 일치하는 포트 없음 */
}

/*
 * [한국어]
 * tcp_sock_get_key - TLS 핸드셰이크 시 PSK ID로 PSK 값 조회 (sock layer 콜백)
 *
 * @out:         TLS PSK를 출력할 버퍼
 * @out_len:     out 버퍼 크기 (바이트)
 * @cipher:      출력: 선택된 TLS 1.3 암호군 문자열 포인터
 * @pskid:       호스트가 TLS 핸드셰이크에서 제시한 PSK 식별자 문자열
 * @get_key_ctx: nvmf_tcp_transport 포인터 (psks 리스트 접근용)
 * @return:      성공 시 0 이상(TLS PSK 바이트 수); -ENOBUFS(버퍼 부족); -ENOTSUP(미지원 암호군); -ENOENT(PSK 없음)
 *
 * TLS 1.3 PSK 핸드셰이크 중 sock layer(spdk_sock_tls)가 이 콜백을 호출한다.
 * pskid로 ttransport->psks 리스트를 검색하고, 찾으면 nvme_tcp_derive_tls_psk로
 * NVMe-TCP 스펙 TLS PSK 파생(KDF)을 수행해 out 버퍼에 채워 넣는다.
 * 암호군(cipher)도 NVME_TCP_CIPHER_*에서 TLS 표준 문자열로 변환해 반환.
 *
 * 실행 컨텍스트: listen sock group poll (TLS accept 경로), 관리 스레드.
 *
 * 호출 체인:
 *   spdk_sock_tls → [tcp_sock_get_key] → nvme_tcp_derive_tls_psk
 */
static int
tcp_sock_get_key(uint8_t *out, int out_len, const char **cipher, const char *pskid,
		 void *get_key_ctx)
{
	struct tcp_psk_entry *entry;
	struct spdk_nvmf_tcp_transport *ttransport = get_key_ctx;
	/* [한국어] get_key_ctx는 nvmf_tcp_listen 시 spdk_sock_listen_opts에 설정한 ttransport */
	size_t psk_len;
	int rc;

	TAILQ_FOREACH(entry, &ttransport->psks, link) {
		/* [한국어] 등록된 PSK 엔트리 리스트를 pskid로 순차 검색 */
		if (strcmp(pskid, entry->pskid) != 0) {
			continue;	/* [한국어] PSK ID 불일치 — 다음 엔트리로 */
		}

		psk_len = entry->psk_size;	/* [한국어] 저장된 PSK 원본 바이트 길이 */
		if ((size_t)out_len < psk_len) {
			/* [한국어] 출력 버퍼가 PSK보다 작으면 쓸 수 없음 */
			SPDK_ERRLOG("Out buffer of size: %" PRIu32 " cannot fit PSK of len: %lu\n",
				    out_len, psk_len);
			return -ENOBUFS;
		}

		/* Convert PSK to the TLS PSK format. */
		rc = nvme_tcp_derive_tls_psk(entry->psk, psk_len, pskid, out, out_len,
					     entry->tls_cipher_suite);
		/* [한국어] NVMe-TCP 스펙의 KDF(HKDF-Expand-Label)로 TLS PSK 파생 — out에 결과 저장 */
		if (rc < 0) {
			SPDK_ERRLOG("Could not generate TLS PSK\n");
		}

		switch (entry->tls_cipher_suite) {
		case NVME_TCP_CIPHER_AES_128_GCM_SHA256:
			*cipher = "TLS_AES_128_GCM_SHA256";
			/* [한국어] TLS 1.3 AES-128-GCM with SHA-256 암호군 문자열 반환 */
			break;
		case NVME_TCP_CIPHER_AES_256_GCM_SHA384:
			*cipher = "TLS_AES_256_GCM_SHA384";
			/* [한국어] TLS 1.3 AES-256-GCM with SHA-384 암호군 문자열 반환 */
			break;
		default:
			*cipher = NULL;	/* [한국어] 미지원 암호군 — NULL 반환 */
			return -ENOTSUP;
		}

		return rc;	/* [한국어] 파생된 TLS PSK 바이트 수 반환 */
	}

	SPDK_ERRLOG("Could not find PSK for identity: %s\n", pskid);
	/* [한국어] 일치하는 PSK 엔트리 없음 — TLS 핸드셰이크 실패로 이어짐 */

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

/*
 * [한국어]
 * nvmf_tcp_qpair_set_state - qpair 상태를 설정하고 trace 기록
 *
 * @tqpair: 상태를 변경할 TCP qpair
 * @state:  새로운 qpair 상태 (NVMF_TCP_QPAIR_STATE_* 중 하나)
 *
 * 단순 setter이지만 모든 qpair 상태 전이에 반드시 이 함수를 사용한다.
 * 직접 tqpair->state를 변경하면 trace가 누락되므로 금지.
 *
 * 호출 체인:
 *   nvmf_tcp_qpair_disconnect / nvmf_tcp_qpair_init / nvmf_tcp_icreq_handle 등
 *     → [nvmf_tcp_qpair_set_state]
 */
static void
nvmf_tcp_qpair_set_state(struct spdk_nvmf_tcp_qpair *tqpair, enum nvmf_tcp_qpair_state state)
{
	tqpair->state = state;	/* [한국어] 새 상태 기록 */
	spdk_trace_record(TRACE_TCP_QP_STATE_CHANGE, tqpair->qpair.trace_id, 0, 0,
			  (uint64_t)tqpair->state);
	/* [한국어] 상태 전이 이벤트를 trace에 기록 — 디버깅/분석 도구에서 qpair 수명 추적 가능 */
}

/*
 * [한국어]
 * nvmf_tcp_qpair_disconnect - qpair를 EXITING 상태로 전이하여 연결 종료 시작
 *
 * @tqpair: 연결을 끊을 TCP qpair
 *
 * 에러 수신(PDU 파싱 실패, C2HTermReq 수신 등) 또는 호스트/타깃 주도 연결 종료 시 호출.
 * RUNNING 이하 상태에서만 동작하며 이미 EXITING/EXITED이면 no-op.
 * spdk_nvmf_qpair_disconnect를 통해 nvmf common layer에 알리고,
 * 최종적으로 nvmf_tcp_close_qpair → nvmf_tcp_qpair_destroy 경로로 이어진다.
 *
 * 사전 조건: recv_state가 NVME_TCP_PDU_RECV_STATE_ERROR여야 한다.
 *
 * 호출 체인:
 *   nvmf_tcp_pdu_ch_handle(에러) / nvmf_tcp_sock_cb(에러) → [nvmf_tcp_qpair_disconnect]
 *     → spdk_nvmf_qpair_disconnect → nvmf_tcp_close_qpair → nvmf_tcp_qpair_destroy
 */
static void
nvmf_tcp_qpair_disconnect(struct spdk_nvmf_tcp_qpair *tqpair)
{
	SPDK_DEBUGLOG(nvmf_tcp, "Disconnecting qpair %p\n", tqpair);

	spdk_trace_record(TRACE_TCP_QP_DISCONNECT, tqpair->qpair.trace_id, 0, 0);
	/* [한국어] disconnect 이벤트를 trace에 기록 */

	if (tqpair->state <= NVMF_TCP_QPAIR_STATE_RUNNING) {
		/* [한국어] RUNNING 이하 상태 — 아직 EXITING/EXITED이 아닌 경우만 처리 */
		nvmf_tcp_qpair_set_state(tqpair, NVMF_TCP_QPAIR_STATE_EXITING);
		/* [한국어] RUNNING → EXITING 전이 — 이후 polling 루프는 이 상태를 보고 qpair 제거 */
		assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
		/* [한국어] disconnect 전에는 반드시 recv_state가 ERROR여야 함 */
		spdk_poller_unregister(&tqpair->timeout_poller);
		/* [한국어] 연결 종료 중이므로 timeout poller 불필요 — 해제 */

		/* This will end up calling nvmf_tcp_close_qpair */
		spdk_nvmf_qpair_disconnect(&tqpair->qpair);
		/* [한국어] nvmf common layer에 qpair disconnect 알림 — 결국 nvmf_tcp_close_qpair 호출 */
	}
}

/*
 * [한국어]
 * _mgmt_pdu_write_done - mgmt_pdu(ICResp/TermReq 등) 비동기 송신 완료 콜백
 *
 * @_tqpair: tqpair 포인터 (sock_req.cb_arg)
 * @err:     송신 결과 (0=성공, 음수=에러)
 *
 * ICResp, C2HTermReq 같은 관리 PDU 송신이 완료될 때 spdk_sock_writev_async가
 * 이 콜백을 호출한다. 에러 발생 시 recv_state를 QUIESCING으로 전이해
 * 더 이상 수신을 처리하지 않고 연결 종료로 유도한다.
 * 성공 시 mgmt_pdu->cb_fn을 호출해 다음 단계(예: ICResp 후 RUNNING 상태 전이)를 처리.
 *
 * 호출 체인:
 *   spdk_sock_writev_async → [_mgmt_pdu_write_done] → pdu->cb_fn(pdu->cb_arg)
 */
static void
_mgmt_pdu_write_done(void *_tqpair, int err)
{
	struct spdk_nvmf_tcp_qpair *tqpair = _tqpair;	/* [한국어] cb_arg를 tqpair로 캐스트 */
	struct nvme_tcp_pdu *pdu = tqpair->mgmt_pdu;	/* [한국어] qpair 전용 관리 PDU 슬롯 참조 */

	if (spdk_unlikely(err != 0)) {
		/* [한국어] 송신 실패 — QUIESCING으로 전이해 더 이상 수신 처리 중단 */
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
		return;
	}

	assert(pdu->cb_fn != NULL);	/* [한국어] 완료 콜백이 없으면 프로그래밍 오류 */
	pdu->cb_fn(pdu->cb_arg);	/* [한국어] 다음 단계 처리 콜백 호출 */
}

/*
 * [한국어]
 * _req_pdu_write_done - req PDU(CapsuleResp/R2T/C2HData) 비동기 송신 완료 콜백
 *
 * @req: spdk_nvmf_tcp_req 포인터 (sock_req.cb_arg)
 * @err: 송신 결과 (0=성공, 음수=에러)
 *
 * req 전용 PDU(tcp_req->pdu) 송신 완료 시 호출된다.
 * pdu_in_use 플래그를 해제해 PDU 슬롯을 재사용 가능 상태로 변경.
 *
 * 세 가지 처리 경로:
 *   1) req가 이미 COMPLETED 상태: 송신 완료를 기다리던 req를 즉시 free (nvmf_tcp_request_free)
 *   2) 송신 에러: QUIESCING으로 전이해 연결 종료 유도
 *   3) 성공: pdu->cb_fn 호출해 다음 단계 (예: R2T 후 H2C 수신 대기)
 *
 * 호출 체인:
 *   spdk_sock_writev_async → [_req_pdu_write_done] → (nvmf_tcp_request_free | pdu->cb_fn)
 */
static void
_req_pdu_write_done(void *req, int err)
{
	struct spdk_nvmf_tcp_req *tcp_req = req;	/* [한국어] cb_arg를 tcp_req로 캐스트 */
	struct nvme_tcp_pdu *pdu = tcp_req->pdu;	/* [한국어] req 전용 PDU 슬롯 */
	struct spdk_nvmf_tcp_qpair *tqpair = pdu->qpair;	/* [한국어] PDU에 저장된 qpair 참조 */

	assert(tcp_req->pdu_in_use);	/* [한국어] 송신 완료이므로 pdu_in_use가 true여야 함 */
	tcp_req->pdu_in_use = false;	/* [한국어] PDU 슬롯 해제 — 다음 PDU 송신에 재사용 가능 */

	/* If the request is in a completed state, we're waiting for write completion to free it */
	if (spdk_unlikely(tcp_req->state == TCP_REQUEST_STATE_COMPLETED)) {
		/* [한국어] COMPLETED 상태: PDU 송신이 완료되길 기다렸던 req — 즉시 free */
		nvmf_tcp_request_free(tcp_req);
		return;
	}

	if (spdk_unlikely(err != 0)) {
		/* [한국어] 송신 실패 — QUIESCING으로 전이해 연결 종료 유도 */
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
		return;
	}

	assert(pdu->cb_fn != NULL);	/* [한국어] 완료 콜백이 없으면 프로그래밍 오류 */
	pdu->cb_fn(pdu->cb_arg);	/* [한국어] 다음 단계 처리 (예: R2T 송신 후 H2C 수신 대기 설정) */
}

/*
 * [한국어]
 * _pdu_write_done - spdk_sock_writev_async의 직접 완료 콜백 (sock_req.cb_fn으로 등록)
 *
 * @pdu: 송신이 완료된 PDU
 * @err: 송신 결과 (0=성공, 음수=에러)
 *
 * PDU의 sock_req.cb_fn/cb_arg를 통해 _mgmt_pdu_write_done 또는 _req_pdu_write_done을
 * 호출하는 간접 호출 계층. nvme_tcp_pdu.sock_req.cb_fn은 항상 이 함수이며,
 * sock_req.cb_arg에 실제 콜백 (tqpair 또는 tcp_req)이 담긴다.
 *
 * 호출 체인:
 *   spdk_sock_writev_async → [_pdu_write_done] → sock_req.cb_fn(sock_req.cb_arg, err)
 */
static void
_pdu_write_done(struct nvme_tcp_pdu *pdu, int err)
{
	pdu->sock_req.cb_fn(pdu->sock_req.cb_arg, err);
	/* [한국어] sock_req에 등록된 콜백 호출 — _mgmt_pdu_write_done 또는 _req_pdu_write_done */
}

/*
 * [한국어]
 * tcp_sock_flush_cb - spdk_sock_flush를 비동기 메시지로 지연 실행하는 콜백
 *
 * @arg: spdk_nvmf_tcp_qpair 포인터
 *
 * ICResp, C2HTermReq 같은 중요 PDU는 커널 send buffer에 즉시 플러시해야 한다.
 * spdk_sock_writev_async는 비동기이므로 완료 직후 flush가 필요.
 * interrupt 모드에서는 -EAGAIN 시 메시지로 재시도, polling 모드에서는
 * 다음 polling 사이클에서 자연스럽게 처리된다.
 *
 * pending_flush 플래그로 동일 qpair에 중복 flush 메시지 방지.
 *
 * 호출 체인:
 *   _tcp_write_pdu → spdk_thread_send_msg → [tcp_sock_flush_cb] → spdk_sock_flush
 */
static void
tcp_sock_flush_cb(void *arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair = arg;	/* [한국어] 메시지 cb_arg를 tqpair로 캐스트 */
	int rc;

	tqpair->pending_flush = false;	/* [한국어] 플래그 해제 — 다음 필요 시 재 enqueue 허용 */
	rc = spdk_sock_flush(tqpair->sock);	/* [한국어] 커널 TCP send buffer 즉시 플러시 */
	if (rc < 0 && rc == -EAGAIN) {
		/* [한국어] -EAGAIN: 버퍼 공간 부족으로 flush 불완전 — 재시도 필요 */
		if (spdk_interrupt_mode_is_enabled()) {
			/* In interrupt mode we need to force a retry. In polling mode it will naturally
			 * try again. */
			spdk_thread_send_msg(spdk_get_thread(), tcp_sock_flush_cb, tqpair);
			/* [한국어] interrupt 모드: 다음 이벤트 루프에서 재시도 메시지 enqueue */
			return;
		}

		rc = 0;	/* [한국어] polling 모드: -EAGAIN은 무시 — 다음 poll에서 자연 처리 */
	}

	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_flush() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		/* [한국어] -EAGAIN 외 에러는 실제 실패 — 로그 출력 (연결 종료로 이어질 수 있음) */
	}
}

/*
 * [한국어]
 * _tcp_write_pdu - PDU를 iov 배열로 빌드하고 비동기 송신 시작
 *
 * @pdu: 송신할 PDU (qpair, hdr, data_iov 등이 설정된 상태)
 *
 * nvme_tcp_build_iovs로 헤더(+HDgst) + 데이터(+DDgst) iov 배열을 구성하고
 * spdk_sock_writev_async로 비동기 송신을 시작한다.
 * ICResp, C2HTermReq, interrupt 모드에서는 flush도 트리거한다.
 *
 * 실행 컨텍스트: poll group spdk_thread.
 *
 * 호출 체인:
 *   data_crc32_accel_done / pdu_data_crc32_compute → [_tcp_write_pdu]
 *     → spdk_sock_writev_async → _pdu_write_done
 */
static void
_tcp_write_pdu(struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_qpair *tqpair = pdu->qpair;	/* [한국어] PDU에 연결된 qpair */

	pdu->sock_req.iovcnt = nvme_tcp_build_iovs(pdu->iov, SPDK_COUNTOF(pdu->iov), pdu,
			       tqpair->host_hdgst_enable, tqpair->host_ddgst_enable, NULL);
	/* [한국어] 헤더(HDgst 포함 가능) + 데이터(DDgst 이미 계산됨) iov 배열 구성 */
	spdk_sock_writev_async(tqpair->sock, &pdu->sock_req);
	/* [한국어] 비동기 writev 시작 — 완료 시 _pdu_write_done → _mgmt_pdu_write_done//_req_pdu_write_done 호출 */

	if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP ||
	    pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ ||
	    spdk_interrupt_mode_is_enabled()) {
		/* [한국어] ICResp/TermReq는 호스트가 즉시 응답을 기대하므로 flush 필수
		 *          interrupt 모드는 polling이 없어 명시적 flush가 없으면 송신 지연 발생 */
		/* Async writes must be flushed */
		if (!tqpair->pending_flush) {
			/* [한국어] 중복 flush 메시지 방지 — 이미 enqueue됐으면 스킵 */
			tqpair->pending_flush = true;	/* [한국어] flush 메시지 enqueue 표시 */
			spdk_thread_send_msg(spdk_get_thread(), tcp_sock_flush_cb, tqpair);
			/* [한국어] 현재 spdk_thread에 flush 메시지 enqueue */
		}
	}
}

/*
 * [한국어]
 * data_crc32_accel_done - accel framework DDgst CRC32C 계산 완료 콜백
 *
 * @cb_arg: nvme_tcp_pdu 포인터
 * @status: 계산 결과 (0=성공, 비0=실패)
 *
 * spdk_accel_submit_crc32cv가 비동기로 CRC32C를 계산한 뒤 이 콜백을 호출한다.
 * 실패 시 _pdu_write_done(status)로 에러를 바로 전파.
 * 성공 시 CRC 값에 SPDK_CRC32C_XOR(0xFFFFFFFF)를 XOR하고 wire 포맷으로 변환 후
 * _tcp_write_pdu로 실제 송신을 시작한다.
 *
 * 호출 체인:
 *   pdu_data_crc32_compute → spdk_accel_submit_crc32cv → [data_crc32_accel_done]
 *     → _tcp_write_pdu
 */
static void
data_crc32_accel_done(void *cb_arg, int status)
{
	struct nvme_tcp_pdu *pdu = cb_arg;	/* [한국어] cb_arg를 PDU로 캐스트 */

	if (spdk_unlikely(status)) {
		/* [한국어] accel 계산 실패 — 에러를 write_done 경로로 즉시 전파 */
		SPDK_ERRLOG("Failed to compute the data digest for pdu =%p\n", pdu);
		_pdu_write_done(pdu, status);
		return;
	}

	pdu->data_digest_crc32 ^= SPDK_CRC32C_XOR;
	/* [한국어] CRC32C 최종 처리: 0xFFFFFFFF XOR (NVMe-TCP 스펙 요구사항, iSCSI와 동일) */
	MAKE_DIGEST_WORD(pdu->data_digest, pdu->data_digest_crc32);
	/* [한국어] 4바이트 리틀엔디안으로 wire 포맷 변환하여 PDU의 data_digest 필드에 기록 */

	_tcp_write_pdu(pdu);	/* [한국어] DDgst 계산 완료 — 이제 PDU 송신 시작 */
}

/*
 * [한국어]
 * pdu_data_crc32_compute - PDU 송신 전 Data Digest(DDgst) CRC32C 계산 시작
 *
 * @pdu: 송신 예정 PDU (data_iov, data_iovcnt, data_len 설정됨)
 *
 * host_ddgst_enable이고 데이터가 있는 PDU 타입이면 CRC32C를 계산한다.
 * 가능하면 accel framework (DSA/IAA/소프트웨어 fallback)로 비동기 계산하고
 * data_crc32_accel_done 콜백 후 PDU를 송신.
 * accel 불가 조건(DIF 컨텍스트 있음, 정렬 불일치, group 없음)이면 동기 계산 후 즉시 송신.
 * DDgst 불필요한 경우는 _tcp_write_pdu를 직접 호출한다.
 *
 * 호출 체인:
 *   nvmf_tcp_qpair_write_pdu / nvmf_tcp_qpair_write_req_pdu → [pdu_data_crc32_compute]
 *     → (spdk_accel_submit_crc32cv → data_crc32_accel_done) | _tcp_write_pdu
 */
static void
pdu_data_crc32_compute(struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_qpair *tqpair = pdu->qpair;	/* [한국어] PDU에 연결된 qpair */
	int rc = 0;

	/* Data Digest */
	if (pdu->data_len > 0 && g_nvme_tcp_ddgst[pdu->hdr.common.pdu_type] && tqpair->host_ddgst_enable) {
		/* [한국어] 데이터 있고, 해당 PDU 타입이 DDgst 지원하며, 호스트가 DDgst 요청한 경우 */
		/* Only support this limitated case for the first step */
		if (spdk_likely(!pdu->dif_ctx && (pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT == 0)
				&& tqpair->group)) {
			/* [한국어] accel 최적 경로 조건:
			 *   - DIF 컨텍스트 없음 (DIF/DIX와 DDgst 동시 처리 미지원)
			 *   - 데이터 길이가 4바이트 정렬 (DIGEST_ALIGNMENT=4)
			 *   - poll group에 accel_channel 있음 */
			rc = spdk_accel_submit_crc32cv(tqpair->group->accel_channel, &pdu->data_digest_crc32, pdu->data_iov,
						       pdu->data_iovcnt, 0, data_crc32_accel_done, pdu);
			/* [한국어] accel framework에 scatter-gather CRC32C 비동기 계산 요청
			 *          완료 시 data_crc32_accel_done 콜백 호출 */
			if (spdk_likely(rc == 0)) {
				return;	/* [한국어] 비동기 계산 시작 — 완료 콜백 기다림 */
			}
		} else {
			pdu->data_digest_crc32 = nvme_tcp_pdu_calc_data_digest(pdu);
			/* [한국어] 동기 소프트웨어 CRC32C 계산 (DIF/정렬불일치/group없음 fallback) */
		}
		data_crc32_accel_done(pdu, rc);	/* [한국어] 동기 계산 완료 후 직접 done 처리 */
	} else {
		_tcp_write_pdu(pdu);	/* [한국어] DDgst 불필요 — 바로 PDU 송신 */
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

/*
 * [한국어]
 * nvmf_tcp_qpair_write_mgmt_pdu - qpair의 관리 PDU(ICResp/TermReq 등) 비동기 송신 시작
 *
 * @tqpair: 송신할 qpair
 * @cb_fn:  송신 완료 후 호출할 콜백 (예: send_icresp_complete)
 * @cb_arg: 콜백 인자
 *
 * tqpair->mgmt_pdu에 _mgmt_pdu_write_done 콜백을 설정하고 nvmf_tcp_qpair_write_pdu를 호출.
 * mgmt_pdu는 ICResp, C2HTermReq 같은 제어 PDU에 사용되는 전용 PDU 슬롯이다.
 *
 * 호출 체인:
 *   nvmf_tcp_send_icresp_complete / nvmf_tcp_send_c2h_term_req → [nvmf_tcp_qpair_write_mgmt_pdu]
 *     → nvmf_tcp_qpair_write_pdu → pdu_data_crc32_compute → _tcp_write_pdu
 */
static void
nvmf_tcp_qpair_write_mgmt_pdu(struct spdk_nvmf_tcp_qpair *tqpair,
			      nvme_tcp_qpair_xfer_complete_cb cb_fn,
			      void *cb_arg)
{
	struct nvme_tcp_pdu *pdu = tqpair->mgmt_pdu;	/* [한국어] 관리 PDU 슬롯 참조 */

	pdu->sock_req.cb_fn = _mgmt_pdu_write_done;	/* [한국어] 비동기 송신 완료 시 호출될 sock 콜백 */
	pdu->sock_req.cb_arg = tqpair;			/* [한국어] _mgmt_pdu_write_done에 전달할 인자 */

	nvmf_tcp_qpair_write_pdu(tqpair, pdu, cb_fn, cb_arg);
	/* [한국어] HDgst 계산 + (DDgst 계산) + writev_async 시작 */
}

/*
 * [한국어]
 * nvmf_tcp_qpair_write_req_pdu - req 전용 PDU(CapsuleResp/R2T/C2HData) 비동기 송신 시작
 *
 * @tqpair:   송신할 qpair
 * @tcp_req:  PDU를 소유한 tcp_req (tcp_req->pdu에 헤더/데이터 설정됨)
 * @cb_fn:    송신 완료 후 호출할 콜백
 * @cb_arg:   콜백 인자
 *
 * tcp_req->pdu에 _req_pdu_write_done 콜백을 설정하고 pdu_in_use 플래그를 set 후
 * nvmf_tcp_qpair_write_pdu를 호출한다. pdu_in_use는 req가 PDU 송신 중임을 표시하며
 * 완료 전 free를 금지한다.
 *
 * 호출 체인:
 *   nvmf_tcp_send_r2t_pdu / nvmf_tcp_send_capsule_resp_pdu / _nvmf_tcp_send_c2h_data
 *     → [nvmf_tcp_qpair_write_req_pdu] → nvmf_tcp_qpair_write_pdu
 */
static void
nvmf_tcp_qpair_write_req_pdu(struct spdk_nvmf_tcp_qpair *tqpair,
			     struct spdk_nvmf_tcp_req *tcp_req,
			     nvme_tcp_qpair_xfer_complete_cb cb_fn,
			     void *cb_arg)
{
	struct nvme_tcp_pdu *pdu = tcp_req->pdu;	/* [한국어] req 전용 PDU 슬롯 */

	pdu->sock_req.cb_fn = _req_pdu_write_done;	/* [한국어] 비동기 송신 완료 시 호출될 sock 콜백 */
	pdu->sock_req.cb_arg = tcp_req;			/* [한국어] _req_pdu_write_done에 전달할 인자 */

	assert(!tcp_req->pdu_in_use);	/* [한국어] 이미 PDU 송신 중이면 중복 송신 — 논리 오류 */
	tcp_req->pdu_in_use = true;	/* [한국어] PDU 사용 중 표시 — 완료 전 req free 금지 */

	nvmf_tcp_qpair_write_pdu(tqpair, pdu, cb_fn, cb_arg);
	/* [한국어] HDgst 계산 + (DDgst 계산) + writev_async 시작 */
}

/*
 * [한국어]
 * nvmf_tcp_qpair_init_mem_resource - qpair의 req/PDU/버퍼 메모리 풀 초기화
 *
 * @tqpair: 초기화할 TCP qpair
 * @return: 0 성공; -1 메모리 할당 실패
 *
 * 연결 수락(accept) 후 qpair가 실제 I/O를 처리하기 전에 한 번 호출된다.
 * 다음 메모리를 할당하고 초기화한다:
 *   1) tqpair->reqs[resource_count]: req 슬롯 배열 (calloc)
 *   2) tqpair->bufs[resource_count * icd_size]: in-capsule 데이터 수신 버퍼 (spdk_zmalloc, DMA)
 *   3) tqpair->pdus[2*resource_count+1]: PDU 슬롯 배열 (spdk_dma_zmalloc, DMA)
 *      - [0..N-1]: req 전용 PDU (tcp_req->pdu 할당)
 *      - [N..2N-1]: 수신 PDU 슬롯 (tcp_pdu_free_queue에 삽입)
 *      - [2N]: mgmt_pdu (ICResp/TermReq 전용)
 * 각 req에 ttag(1-based), buf, cmd/rsp 포인터, FREE 초기 상태를 설정하고
 * free_queue에 삽입한다.
 *
 * 호출 체인:
 *   nvmf_tcp_qpair_sock_init → [nvmf_tcp_qpair_init_mem_resource]
 */
static int
nvmf_tcp_qpair_init_mem_resource(struct spdk_nvmf_tcp_qpair *tqpair)
{
	uint32_t i;
	struct spdk_nvmf_transport_opts *opts;
	uint32_t in_capsule_data_size;

	opts = &tqpair->qpair.transport->opts;	/* [한국어] transport 공통 옵션 참조 */

	in_capsule_data_size = opts->in_capsule_data_size;	/* [한국어] in-capsule 데이터 최대 크기 */
	if (opts->dif_insert_or_strip) {
		in_capsule_data_size = SPDK_BDEV_BUF_SIZE_WITH_MD(in_capsule_data_size);
		/* [한국어] DIF/DIX 모드: 메타데이터(T10 DIF) 포함 크기로 확장 (512+8=520, 4096+8=4104 등) */
	}

	tqpair->resource_count = opts->max_queue_depth;
	/* [한국어] qpair의 최대 동시 req 수 = max_queue_depth (ICReq에서 협상되지 않고 transport 설정 사용) */

	tqpair->reqs = calloc(tqpair->resource_count, sizeof(*tqpair->reqs));
	/* [한국어] req 슬롯 배열 할당 (calloc: 0 초기화) */
	if (!tqpair->reqs) {
		SPDK_ERRLOG("Unable to allocate reqs on tqpair=%p\n", tqpair);
		return -1;
	}

	if (in_capsule_data_size) {
		tqpair->bufs = spdk_zmalloc(tqpair->resource_count * in_capsule_data_size, 0x1000,
					    NULL, SPDK_ENV_LCORE_ID_ANY,
					    SPDK_MALLOC_DMA);
		/* [한국어] in-capsule 데이터 수신 버퍼 할당 (DMA 가능, 4KB 정렬)
		 *          각 req에 icd_size 크기씩 할당 — CapsuleCmd 페이로드를 직접 여기에 수신 */
		if (!tqpair->bufs) {
			SPDK_ERRLOG("Unable to allocate bufs on tqpair=%p.\n", tqpair);
			return -1;
		}
	}
	/* prepare memory space for receiving pdus and tcp_req */
	/* Add additional 1 member, which will be used for mgmt_pdu owned by the tqpair */
	tqpair->pdus = spdk_dma_zmalloc((2 * tqpair->resource_count + 1) * sizeof(*tqpair->pdus), 0x1000,
					NULL);
	/* [한국어] PDU 슬롯 배열 할당 (DMA 가능, 4KB 정렬):
	 *   2*N개: req PDU(N) + 수신 PDU 풀(N)
	 *   +1: mgmt_pdu 전용 슬롯 */
	if (!tqpair->pdus) {
		SPDK_ERRLOG("Unable to allocate pdu pool on tqpair =%p.\n", tqpair);
		return -1;
	}

	for (i = 0; i < tqpair->resource_count; i++) {
		/* [한국어] 첫 번째 N개 PDU를 각 req에 1:1 할당 (req 전용 PDU) */
		struct spdk_nvmf_tcp_req *tcp_req = &tqpair->reqs[i];

		tcp_req->ttag = i + 1;	/* [한국어] Transfer Tag: 1-based (0은 "태그 없음"을 의미하므로 제외) */
		tcp_req->req.qpair = &tqpair->qpair;	/* [한국어] 공통 qpair 역참조 설정 */

		tcp_req->pdu = &tqpair->pdus[i];	/* [한국어] req 전용 PDU 슬롯 연결 */
		tcp_req->pdu->qpair = tqpair;		/* [한국어] PDU에서 qpair 역참조 설정 */

		/* Set up memory to receive commands */
		if (tqpair->bufs) {
			tcp_req->buf = (void *)((uintptr_t)tqpair->bufs + (i * in_capsule_data_size));
			/* [한국어] req i의 in-capsule 버퍼 포인터 = bufs 배열 base + i * icd_size */
		}

		/* Set the cmdn and rsp */
		tcp_req->req.rsp = (union nvmf_c2h_msg *)&tcp_req->rsp;
		/* [한국어] 공통 req의 rsp 포인터를 tcp_req 내장 rsp에 연결 (추가 alloc 불필요) */
		tcp_req->req.cmd = (union nvmf_h2c_msg *)&tcp_req->cmd;
		/* [한국어] 공통 req의 cmd 포인터를 tcp_req 내장 cmd에 연결 */

		tcp_req->req.stripped_data = NULL;	/* [한국어] DIF strip 데이터 포인터 초기화 */

		/* Initialize request state to FREE */
		tcp_req->state = TCP_REQUEST_STATE_FREE;	/* [한국어] 직접 설정 (set_state는 cntr 관리용이므로 초기화에서만 직접 접근) */
		TAILQ_INSERT_TAIL(&tqpair->tcp_req_free_queue, tcp_req, state_link);
		/* [한국어] free_queue에 추가 */
		tqpair->state_cntr[TCP_REQUEST_STATE_FREE]++;
		/* [한국어] FREE 상태 카운터 수동 증가 (set_state 우회했으므로) */
	}

	for (; i < 2 * tqpair->resource_count; i++) {
		/* [한국어] 다음 N개 PDU를 수신 PDU 풀로 초기화 */
		struct nvme_tcp_pdu *pdu = &tqpair->pdus[i];

		pdu->qpair = tqpair;		/* [한국어] PDU에 qpair 역참조 설정 */
		SLIST_INSERT_HEAD(&tqpair->tcp_pdu_free_queue, pdu, slist);
		/* [한국어] 수신 PDU free 풀에 삽입 (스택처럼 LIFO — 마지막 삽입이 가장 먼저 사용됨) */
	}

	tqpair->mgmt_pdu = &tqpair->pdus[i];	/* [한국어] 마지막 PDU 슬롯을 mgmt_pdu로 지정 */
	tqpair->mgmt_pdu->qpair = tqpair;	/* [한국어] mgmt_pdu qpair 역참조 설정 */
	tqpair->pdu_in_progress = SLIST_FIRST(&tqpair->tcp_pdu_free_queue);
	/* [한국어] 현재 수신 중인 PDU 슬롯 초기화 — free pool에서 첫 번째 항목 */
	SLIST_REMOVE_HEAD(&tqpair->tcp_pdu_free_queue, slist);
	/* [한국어] free pool에서 pdu_in_progress로 사용할 슬롯 제거 */
	tqpair->tcp_pdu_working_count = 1;
	/* [한국어] 현재 사용 중인 PDU 수 = 1 (pdu_in_progress) */

	tqpair->recv_buf_size = (in_capsule_data_size + sizeof(struct spdk_nvme_tcp_cmd) + 2 *
				 SPDK_NVME_TCP_DIGEST_LEN) * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR;
	/* [한국어] 소켓 수신 버퍼 크기 = (ICD + cmd헤더 + 2*HDgst크기) * FACTOR(2)
	 *          커널 TCP 수신 버퍼가 이보다 크도록 spdk_sock_set_recvbuf에서 설정됨 */

	return 0;
}

/*
 * [한국어]
 * nvmf_tcp_qpair_init - qpair 공통 초기화 (transport_ops.qpair_init 콜백)
 *
 * @qpair: 초기화할 공통 qpair
 * @return: 0 (항상 성공)
 *
 * nvmf_transport_qpair_create 후 transport-specific 초기화를 위해 호출.
 * req/PDU 큐 초기화, 소켓 수신 버퍼 크기 계산, HDgst/DDgst 기본값(true) 설정.
 * 실제 메모리 할당은 nvmf_tcp_qpair_init_mem_resource에서 수행.
 *
 * 실행 컨텍스트: accept poller (새 연결 수락 경로).
 *
 * 호출 체인:
 *   nvmf_tcp_handle_connect → nvmf_transport_qpair_create → [nvmf_tcp_qpair_init]
 *     → nvmf_tcp_qpair_sock_init
 */
static int
nvmf_tcp_qpair_init(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_qpair *tqpair;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	/* [한국어] 공통 qpair → TCP qpair 다운캐스트 */

	SPDK_DEBUGLOG(nvmf_tcp, "New TCP Connection: %p\n", qpair);

	spdk_trace_record(TRACE_TCP_QP_CREATE, tqpair->qpair.trace_id, 0, 0);
	/* [한국어] qpair 생성 이벤트 trace 기록 */

	/* Initialise request state queues of the qpair */
	TAILQ_INIT(&tqpair->tcp_req_free_queue);	/* [한국어] req free 큐 초기화 */
	TAILQ_INIT(&tqpair->tcp_req_working_queue);	/* [한국어] req working 큐 초기화 */
	SLIST_INIT(&tqpair->tcp_pdu_free_queue);	/* [한국어] PDU free 풀 초기화 */
	tqpair->qpair.queue_depth = 0;			/* [한국어] 현재 진행 중 req 수 0으로 초기화 */

	tqpair->host_hdgst_enable = true;	/* [한국어] 초기값 true — ICReq에서 호스트 요청에 따라 재설정됨 */
	tqpair->host_ddgst_enable = true;	/* [한국어] 초기값 true — ICReq에서 호스트 요청에 따라 재설정됨 */

	return 0;
}

static int
nvmf_tcp_qpair_sock_init(struct spdk_nvmf_tcp_qpair *tqpair)
{
	char saddr[SPDK_NVMF_TRADDR_MAX_LEN], caddr[SPDK_NVMF_TRADDR_MAX_LEN];
	/* [한국어] 서버 주소(saddr)와 클라이언트 주소(caddr) 문자열 버퍼 */
	uint16_t sport, cport;
	/* 1 for colon, up to 5 for port number, 1 for null terminator */
	char owner[sizeof(caddr) + 1 + 5 + 1];
	/* [한국어] trace owner 문자열 = "클라이언트IP:포트" */
	int rc;

	rc = spdk_sock_getaddr(tqpair->sock, saddr, sizeof(saddr), &sport,
			       caddr, sizeof(caddr), &cport);
	/* [한국어] getsockname + getpeername — 로컬(서버) 주소와 원격(클라이언트) 주소/포트 획득 */
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_getaddr() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		return rc;
	}
	/* update buffer size for owner when changing format or arguments here */
	snprintf(owner, sizeof(owner), "%s:%d", caddr, cport);
	/* [한국어] trace owner 이름 = "클라이언트IP:포트" 형식 (디버깅 시 어느 호스트 연결인지 식별) */
	tqpair->qpair.trace_id = spdk_trace_register_owner(OWNER_TYPE_NVMF_TCP, owner);
	/* [한국어] trace 시스템에 이 qpair 소유자 등록 — 이후 trace_record 호출 시 이 ID 사용 */
	spdk_trace_record(TRACE_TCP_QP_SOCK_INIT, tqpair->qpair.trace_id, 0, 0);
	/* [한국어] sock 초기화 완료 trace 기록 */

	/* set low water mark */
	rc = spdk_sock_set_recvlowat(tqpair->sock, 1);
	/* [한국어] SO_RCVLOWAT = 1 바이트 설정 — 1바이트라도 수신되면 readable 이벤트 발생
	 *          (기본값이 1이지만 명시적 설정으로 예상 동작 보장) */
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_set_recvlowat() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		return rc;
	}

	return 0;	/* [한국어] 초기화 성공 */
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

/*
 * [한국어]
 * nvmf_tcp_qpair_sock_init - qpair 소켓 기본 설정 초기화
 *
 * @tqpair: 초기화할 TCP qpair (sock 필드에 수락된 소켓이 설정됨)
 * @return: 0 성공; 음수 에러
 *
 * accept 후 소켓에서 원격/로컬 주소를 추출하고 trace owner를 등록한다.
 * 수신 버퍼 크기를 transport 옵션에 맞게 설정하고,
 * SO_PRIORITY를 tcp_opts.sock_priority로 설정한다.
 * 마지막으로 메모리 리소스(nvmf_tcp_qpair_init_mem_resource)를 초기화한다.
 *
 * 호출 체인:
 *   nvmf_tcp_handle_connect → [nvmf_tcp_qpair_sock_init]
 *     → nvmf_tcp_qpair_init_mem_resource
 */

/*
 * [한국어]
 * nvmf_tcp_port_accept - 하나의 listen 포트에서 최대 N개 연결 일괄 accept
 *
 * @port:   accept할 listening port 객체
 * @return: 이번 호출에서 accept한 연결 수
 *
 * 단일 콜백에서 NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME(=16)개 이하의 연결을 처리한다.
 * 이 한도는 한 포트가 accept poller를 독점해 다른 포트를 굶기지 않기 위한 fairness 한도.
 * 각 accept 후 nvmf_tcp_handle_connect를 호출해 qpair 생성/등록을 시작한다.
 *
 * 실행 컨텍스트: accept poller thread (nvmf_tcp_accept_cb 경유).
 *
 * 호출 체인:
 *   nvmf_tcp_accept_cb → [nvmf_tcp_port_accept] → nvmf_tcp_handle_connect (연결당)
 */
static uint32_t
nvmf_tcp_port_accept(struct spdk_nvmf_tcp_port *port)
{
	struct spdk_sock *sock;
	uint32_t count = 0;	/* [한국어] 이 호출에서 accept한 연결 수 */
	int i;

	for (i = 0; i < NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME; i++) {
		/* [한국어] 공정성 한도: 최대 NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME(=16)회 시도 */
		sock = spdk_sock_accept(port->listen_sock);	/* [한국어] 비블로킹 accept() — 큐에 연결 없으면 NULL 반환 */
		if (sock == NULL) {
			break;	/* [한국어] 더 이상 accept할 연결 없음 — 루프 종료 */
		}
		count++;	/* [한국어] accept 성공 카운터 */
		nvmf_tcp_handle_connect(port, sock);		/* [한국어] 수락된 소켓으로 tqpair 생성 및 tgt에 등록 */
	}

	return count;	/* [한국어] accept한 연결 수 (호출자가 BUSY/IDLE 판단에 사용) */
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

/*
 * [한국어]
 * nvmf_tcp_discover - discovery log page entry 채우기 (transport_ops.discover 콜백)
 *
 * @transport: TCP transport
 * @trid:      이 항목에 해당하는 listen trid
 * @entry:     채울 discovery log page entry (출력)
 *
 * nvmf_get_discovery_log_page RPC 처리 시 각 listen port당 한 번 호출된다.
 * TCP 특화 필드(trtype, adrfam, trsvcid, traddr, treq, tsas)를 채운다.
 * SSL 소켓이면 TLS 1.3 보안 채널 필수(REQUIRED)로 표시, 일반 TCP면 NONE.
 *
 * 호출 체인:
 *   nvmf RPC get_discovery_log_page → [nvmf_tcp_discover]
 */
static void
nvmf_tcp_discover(struct spdk_nvmf_transport *transport,
		  struct spdk_nvme_transport_id *trid,
		  struct spdk_nvmf_discovery_log_page_entry *entry)
{
	struct spdk_nvmf_tcp_port *port;
	struct spdk_nvmf_tcp_transport *ttransport;

	entry->trtype = SPDK_NVMF_TRTYPE_TCP;	/* [한국어] 전송 타입 = TCP */
	entry->adrfam = trid->adrfam;		/* [한국어] 주소 계열 = IPv4 or IPv6 */

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	/* [한국어] trsvcid 필드에 포트 번호를 공백 패딩으로 채움 (NVMe-oF discovery log page 형식) */
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');
	/* [한국어] traddr 필드에 IP 주소를 공백 패딩으로 채움 */

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	/* [한국어] 공통 transport → TCP transport 다운캐스트 */
	port = nvmf_tcp_find_port(ttransport, trid);
	/* [한국어] trid에 해당하는 listen port 객체 조회 (TLS 여부 확인용) */

	assert(port != NULL);	/* [한국어] discover는 listen 중인 port에만 호출되어야 함 */

	if (strcmp(spdk_sock_get_impl_name(port->listen_sock), "ssl") == 0) {
		/* [한국어] SSL(TLS) listen sock이면 보안 채널 필수로 표시 */
		entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_REQUIRED;
		entry->tsas.tcp.sectype = SPDK_NVME_TCP_SECURITY_TLS_1_3;
		/* [한국어] TLS 1.3 보안 채널 사용 명시 */
	} else {
		entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED;
		/* [한국어] 일반 TCP: 보안 채널 불필요 */
		entry->tsas.tcp.sectype = SPDK_NVME_TCP_SECURITY_NONE;
	}
}

/*
 * [한국어]
 * nvmf_tcp_control_msg_list_create - control_msg_list 및 버퍼 풀 생성
 *
 * @num_messages: 생성할 control_msg 버퍼 수 (tcp_opts.control_msg_num)
 * @return:        생성된 control_msg_list 포인터; 실패 시 NULL
 *
 * poll group 생성 시 nvmf_tcp_poll_group_create에서 호출된다.
 * num_messages개의 SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE(=8192) 크기 버퍼를 DMA 할당하고
 * free_msgs STAILQ로 관리한다. 이 버퍼들은 ICD가 transport->in_capsule_data_size보다 크지만
 * 8192 이하인 명령(주로 admin/fabric)의 in-capsule 데이터를 수신하는 데 사용된다.
 *
 * 호출 체인:
 *   nvmf_tcp_poll_group_create → [nvmf_tcp_control_msg_list_create]
 */
static struct spdk_nvmf_tcp_control_msg_list *
nvmf_tcp_control_msg_list_create(uint16_t num_messages)
{
	struct spdk_nvmf_tcp_control_msg_list *list;
	struct spdk_nvmf_tcp_control_msg *msg;
	uint16_t i;

	list = calloc(1, sizeof(*list));	/* [한국어] 리스트 헤더 구조체 할당 */
	if (!list) {
		SPDK_ERRLOG("Failed to allocate memory for list structure\n");
		return NULL;
	}

	list->msg_buf = spdk_zmalloc(num_messages * SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE,
				     NVMF_DATA_BUFFER_ALIGNMENT, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	/* [한국어] control_msg 버퍼 연속 배열 할당 (DMA 가능, NUMA-agnostic)
	 *          각 슬롯은 8192바이트 — NVMe-TCP ICD 최대 크기 (스펙 정의) */
	if (!list->msg_buf) {
		SPDK_ERRLOG("Failed to allocate memory for control message buffers\n");
		free(list);
		return NULL;
	}

	STAILQ_INIT(&list->free_msgs);			/* [한국어] free msg 큐 초기화 */
	STAILQ_INIT(&list->waiting_for_msg_reqs);	/* [한국어] msg 대기 req 큐 초기화 */

	for (i = 0; i < num_messages; i++) {
		/* [한국어] 각 슬롯을 spdk_nvmf_tcp_control_msg 포인터로 캐스트해 free 큐에 추가 */
		msg = (struct spdk_nvmf_tcp_control_msg *)((char *)list->msg_buf + i *
				SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE);
		/* [한국어] 연속 버퍼에서 i번째 슬롯 포인터 계산 */
		STAILQ_INSERT_TAIL(&list->free_msgs, msg, link);
		/* [한국어] free 큐 꼬리에 추가 */
	}

	return list;
}

/*
 * [한국어]
 * nvmf_tcp_control_msg_list_free - control_msg_list 및 버퍼 풀 해제
 *
 * @list: 해제할 control_msg_list (NULL 허용)
 *
 * poll group 파괴(nvmf_tcp_poll_group_destroy) 시 호출.
 * msg_buf DMA 메모리와 list 구조체를 해제한다.
 *
 * 호출 체인:
 *   nvmf_tcp_poll_group_destroy → [nvmf_tcp_control_msg_list_free]
 */
static void
nvmf_tcp_control_msg_list_free(struct spdk_nvmf_tcp_control_msg_list *list)
{
	if (!list) {
		return;	/* [한국어] NULL 리스트 — no-op (방어적 처리) */
	}

	spdk_free(list->msg_buf);	/* [한국어] DMA 버퍼 해제 (spdk_zmalloc으로 할당됨) */
	free(list);			/* [한국어] 리스트 헤더 구조체 해제 */
}

static int nvmf_tcp_poll_group_poll(struct spdk_nvmf_transport_poll_group *group);

/*
 * [한국어]
 * nvmf_tcp_poll_group_intr - interrupt 모드에서 poll group 이벤트 처리 (poller 콜백)
 *
 * @ctx: spdk_nvmf_transport_poll_group 포인터
 * @return: SPDK_POLLER_BUSY (작업 있음) / SPDK_POLLER_IDLE (없음)
 *
 * interrupt 모드(spdk_interrupt_mode_is_enabled())에서 epoll 이벤트 발생 시
 * 이 함수가 reactor에 의해 호출된다. polling 모드에서는 nvmf_tcp_poll_group_poll이
 * 직접 주기적으로 호출되고, 이 함수는 사용되지 않는다.
 *
 * 호출 체인:
 *   interrupt 이벤트 → reactor → [nvmf_tcp_poll_group_intr] → nvmf_tcp_poll_group_poll
 */
static int
nvmf_tcp_poll_group_intr(void *ctx)
{
	struct spdk_nvmf_transport_poll_group *group = ctx;	/* [한국어] cb_arg를 poll_group으로 캐스트 */
	int ret = 0;

	ret = nvmf_tcp_poll_group_poll(group);	/* [한국어] 실제 polling 로직 위임 */

	return ret != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
	/* [한국어] 처리한 이벤트 있으면 BUSY — reactor가 이번 tick에 더 실행할 수 있도록 힌트 */
}

/*
 * [한국어]
 * nvmf_tcp_poll_group_create - TCP poll group 생성 (transport_ops.poll_group_create 콜백)
 *
 * @transport: TCP transport
 * @group:     nvmf 공통 poll_group (spdk_nvmf_poll_group)
 * @return:    생성된 poll_group 포인터 (&tgroup->group); 실패 시 NULL
 *
 * reactor 당 하나의 poll group을 생성하며, 다음을 초기화한다:
 *   1) tgroup 구조체 calloc
 *   2) sock_group 생성 (qpair 소켓 이벤트 polling에 사용)
 *   3) ICD가 8192 미만이면 control_msg_list 생성 (admin/fabric 명령용)
 *   4) accel_channel 생성 (CRC32C/DIF accel 오프로드에 사용)
 *   5) ttransport->poll_groups 리스트에 추가, next_pg 초기화
 *   6) interrupt 모드이면 sock_group fd에 interrupt handler 등록
 *
 * 실패 경로: cleanup 레이블로 이동해 nvmf_tcp_poll_group_destroy 호출 후 NULL 반환.
 *
 * 호출 체인:
 *   spdk_nvmf_poll_group_create → [nvmf_tcp_poll_group_create]
 */
static struct spdk_nvmf_transport_poll_group *
nvmf_tcp_poll_group_create(struct spdk_nvmf_transport *transport,
			   struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_tcp_transport	*ttransport;
	struct spdk_nvmf_tcp_poll_group *tgroup;

	tgroup = calloc(1, sizeof(*tgroup));	/* [한국어] poll group 구조체 할당 및 0 초기화 */
	if (!tgroup) {
		return NULL;
	}

	tgroup->sock_group = spdk_sock_group_create(&tgroup->group);
	/* [한국어] sock_group 생성 — qpair 소켓의 readable/writable 이벤트를 묶어서 polling */
	if (!tgroup->sock_group) {
		goto cleanup;	/* [한국어] sock_group 생성 실패 — cleanup 후 NULL 반환 */
	}

	TAILQ_INIT(&tgroup->qpairs);	/* [한국어] 이 poll group 소속 qpair 리스트 초기화 */

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	/* [한국어] 공통 transport → TCP transport 다운캐스트 */

	if (transport->opts.in_capsule_data_size < SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE) {
		/* [한국어] ICD 설정이 8192 미만: admin/fabric 명령(8192 이하 ICD)의 in-capsule 데이터를
		 *          수신하기 위해 추가 control_msg 버퍼 풀이 필요 */
		SPDK_DEBUGLOG(nvmf_tcp, "ICD %u is less than min required for admin/fabric commands (%u). "
			      "Creating control messages list\n", transport->opts.in_capsule_data_size,
			      SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE);
		tgroup->control_msg_list = nvmf_tcp_control_msg_list_create(ttransport->tcp_opts.control_msg_num);
		/* [한국어] control_msg_num개의 8192바이트 버퍼 풀 생성 */
		if (!tgroup->control_msg_list) {
			goto cleanup;	/* [한국어] 할당 실패 */
		}
	}

	tgroup->accel_channel = spdk_accel_get_io_channel();
	/* [한국어] accel 프레임워크 I/O 채널 획득 — CRC32C/DDgst 계산 오프로드에 사용 (DSA/IAA/SW) */
	if (spdk_unlikely(!tgroup->accel_channel)) {
		SPDK_ERRLOG("Cannot create accel_channel for tgroup=%p\n", tgroup);
		goto cleanup;
	}

	TAILQ_INSERT_TAIL(&ttransport->poll_groups, tgroup, link);
	/* [한국어] ttransport의 poll_groups 리스트에 추가 (get_optimal_poll_group에서 라운드로빈 사용) */
	if (ttransport->next_pg == NULL) {
		ttransport->next_pg = tgroup;	/* [한국어] 첫 번째 poll group이 next_pg의 시작점 */
	}

	if (spdk_interrupt_mode_is_enabled()) {
		/* [한국어] interrupt 모드: epoll fd에 sock_group 이벤트 핸들러 등록 */
		tgroup->intr = SPDK_INTERRUPT_REGISTER_FOR_EVENTS(spdk_sock_group_get_interruptfd(
					tgroup->sock_group),
				SPDK_INTERRUPT_EVENT_IN | SPDK_INTERRUPT_EVENT_OUT,
				nvmf_tcp_poll_group_intr, &tgroup->group);
		/* [한국어] IN(수신) + OUT(송신) 이벤트 모두 등록 — 데이터 read/write 양방향 */
		if (tgroup->intr == NULL) {
			SPDK_ERRLOG("Failed to register interrupt for sock group\n");
			goto cleanup;
		}
	}

	return &tgroup->group;	/* [한국어] 공통 poll_group 포인터 반환 (transport-agnostic 인터페이스) */

cleanup:
	nvmf_tcp_poll_group_destroy(&tgroup->group);	/* [한국어] 부분 초기화된 tgroup 정리 */
	return NULL;
}

/*
 * [한국어]
 * nvmf_tcp_get_optimal_poll_group - 소켓에 최적화된 poll group 선택
 *
 * @qpair: 최적 poll group을 찾을 TCP qpair
 * @return: 최적 poll_group 포인터; 실패 시 NULL
 *
 * 새 qpair(tqpair)를 어느 poll group에 할당할지 결정한다.
 * spdk_sock_get_optimal_sock_group에 hint(next_pg의 sock_group)를 제공해
 * NIC RSS(Receive Side Scaling) 큐와 같은 CPU의 poll group을 찾는다.
 * 최적 group이 없으면 hint를 사용하고 next_pg를 라운드로빈으로 전진.
 *
 * 실행 컨텍스트: tgt 관리 스레드 (qpair 등록 경로).
 *
 * 호출 체인:
 *   spdk_nvmf_tgt_new_qpair → nvmf_poll_group_get_optimal → [nvmf_tcp_get_optimal_poll_group]
 */
static struct spdk_nvmf_transport_poll_group *
nvmf_tcp_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_poll_group **pg;
	struct spdk_nvmf_tcp_qpair *tqpair;
	struct spdk_sock_group *group = NULL, *hint = NULL;
	int rc;

	ttransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_tcp_transport, transport);
	/* [한국어] 공통 transport → TCP transport 다운캐스트 */

	if (TAILQ_EMPTY(&ttransport->poll_groups)) {
		return NULL;	/* [한국어] 등록된 poll group 없음 (아직 reactor 시작 전) */
	}

	pg = &ttransport->next_pg;		/* [한국어] 라운드로빈 힌트 포인터의 포인터 */
	assert(*pg != NULL);
	hint = (*pg)->sock_group;		/* [한국어] 현재 힌트 poll group의 sock_group */

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	rc = spdk_sock_get_optimal_sock_group(tqpair->sock, &group, hint);
	/* [한국어] NIC RSS 등을 통해 tqpair->sock에 최적인 sock_group 쿼리 */
	if (rc != 0) {
		return NULL;	/* [한국어] 최적화 지원 안 하는 sock impl → 호출자가 라운드로빈 사용 */
	} else if (group != NULL) {
		/* Optimal poll group was found */
		return spdk_sock_group_get_ctx(group);
		/* [한국어] NIC가 RSS 큐에 매핑한 sock_group 반환 — NUMA/RSS 최적 배치 */
	}

	/* The hint was used for optimal poll group, advance next_pg. */
	*pg = TAILQ_NEXT(*pg, link);	/* [한국어] 힌트가 선택됨 — 다음 poll group으로 전진 (라운드로빈) */
	if (*pg == NULL) {
		*pg = TAILQ_FIRST(&ttransport->poll_groups);	/* [한국어] 리스트 끝에 도달하면 처음으로 wrap */
	}

	return spdk_sock_group_get_ctx(hint);	/* [한국어] 힌트 sock_group의 poll_group 반환 */
}

/*
 * [한국어]
 * nvmf_tcp_poll_group_destroy - TCP poll group 파괴 (transport_ops.poll_group_destroy 콜백)
 *
 * @group: 파괴할 transport poll_group
 *
 * interrupt handler 해제, sock_group 닫기, control_msg_list free, accel_channel 반환,
 * poll_groups 리스트에서 제거, next_pg 업데이트, tgroup free를 순서대로 수행.
 * group.transport가 NULL인 경우(poll_group_create 실패 경로)는 free만 하고 반환.
 *
 * 호출 체인:
 *   spdk_nvmf_poll_group_destroy / nvmf_tcp_poll_group_create(에러) → [nvmf_tcp_poll_group_destroy]
 */
static void
nvmf_tcp_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_tcp_poll_group *tgroup, *next_tgroup;
	struct spdk_nvmf_tcp_transport *ttransport;
	int rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	/* [한국어] 공통 poll_group → TCP poll_group 다운캐스트 */
	spdk_interrupt_unregister(&tgroup->intr);	/* [한국어] interrupt 모드 epoll 핸들러 해제 (없으면 no-op) */
	rc = spdk_sock_group_close(&tgroup->sock_group);	/* [한국어] sock_group 닫기 (epoll fd 반환) */
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_group_close() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		assert(false);
	}

	if (tgroup->control_msg_list) {
		nvmf_tcp_control_msg_list_free(tgroup->control_msg_list);
		/* [한국어] control_msg 버퍼 풀 해제 (ICD<8192일 때만 존재) */
	}

	if (tgroup->accel_channel) {
		spdk_put_io_channel(tgroup->accel_channel);
		/* [한국어] accel I/O 채널 반환 (accel 프레임워크 참조 카운트 감소) */
	}

	if (tgroup->group.transport == NULL) {
		/* Transport can be NULL when nvmf_tcp_poll_group_create()
		 * calls this function directly in a failure path. */
		free(tgroup);	/* [한국어] 생성 실패 경로: 리스트에 추가되기 전 — 구조체만 free */
		return;
	}

	ttransport = SPDK_CONTAINEROF(tgroup->group.transport, struct spdk_nvmf_tcp_transport, transport);
	/* [한국어] poll_group의 transport → TCP transport 다운캐스트 */

	next_tgroup = TAILQ_NEXT(tgroup, link);		/* [한국어] 다음 poll group 미리 저장 (리스트에서 제거 전) */
	TAILQ_REMOVE(&ttransport->poll_groups, tgroup, link);	/* [한국어] poll_groups 리스트에서 제거 */
	if (next_tgroup == NULL) {
		next_tgroup = TAILQ_FIRST(&ttransport->poll_groups);
		/* [한국어] 마지막 poll group이었으면 다음을 첫 번째로 wrap */
	}
	if (ttransport->next_pg == tgroup) {
		ttransport->next_pg = next_tgroup;
		/* [한국어] next_pg가 삭제되는 tgroup을 가리키고 있었으면 다음으로 업데이트 */
	}

	free(tgroup);	/* [한국어] poll_group 구조체 해제 */
}

/*
 * [한국어]
 * nvmf_tcp_qpair_set_recv_state - qpair PDU 수신 상태 전이 및 부수 처리
 *
 * @tqpair: 상태를 변경할 TCP qpair
 * @state:  새로운 수신 상태 (NVME_TCP_PDU_RECV_STATE_* 중 하나)
 *
 * 수신 상태 머신의 상태 전이 함수. 동일 상태로 전이는 에러 로그 출력.
 * QUIESCING으로 전이 시 현재 수신 중인 PDU(pdu_in_progress)를 free pool로 반환해
 * 더 이상 수신을 진행하지 않도록 한다.
 * 수신 상태에 따라 SPDK_DEBUGLOG로 상태 변화를 기록하고 trace도 남긴다.
 *
 * 호출 체인:
 *   nvmf_tcp_pdu_ch_handle / nvmf_tcp_pdu_payload_handle / 에러 처리 경로
 *     → [nvmf_tcp_qpair_set_recv_state]
 */
static void
nvmf_tcp_qpair_set_recv_state(struct spdk_nvmf_tcp_qpair *tqpair,
			      enum nvme_tcp_pdu_recv_state state)
{
	if (tqpair->recv_state == state) {
		/* [한국어] 동일 상태로 전이 시도 — 논리 오류지만 치명적이지 않음 */
		SPDK_ERRLOG("The recv state of tqpair=%p is same with the state(%d) to be set\n",
			    tqpair, state);
		return;
	}

	if (spdk_unlikely(state == NVME_TCP_PDU_RECV_STATE_QUIESCING)) {
		/* [한국어] QUIESCING 전이: 더 이상 PDU를 수신하지 않으므로 진행 중인 PDU 슬롯 반환 */
		if (tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH && tqpair->pdu_in_progress) {
			/* [한국어] 현재 PDU 헤더 수신 중이었으면 — pdu_in_progress를 free pool로 반환 */
			SLIST_INSERT_HEAD(&tqpair->tcp_pdu_free_queue, tqpair->pdu_in_progress, slist);
			tqpair->tcp_pdu_working_count--;
		}
	}

	if (spdk_unlikely(state == NVME_TCP_PDU_RECV_STATE_ERROR)) {
		/* [한국어] ERROR 상태로 전이할 때는 모든 working PDU가 이미 반환됐어야 함 */
		assert(tqpair->tcp_pdu_working_count == 0);
	}

	SPDK_DEBUGLOG(nvmf_tcp, "tqpair(%p) recv state=%d\n", tqpair, state);
	tqpair->recv_state = state;	/* [한국어] 새 수신 상태 저장 */

	spdk_trace_record(TRACE_TCP_QP_RCV_STATE_CHANGE, tqpair->qpair.trace_id, 0, 0,
			  (uint64_t)tqpair->recv_state);
	/* [한국어] 수신 상태 변화를 trace에 기록 */
}

/*
 * [한국어]
 * nvmf_tcp_qpair_handle_timeout - C2HTermReq 송신 후 호스트 종료 대기 타임아웃 처리
 *
 * @ctx: tqpair 포인터 (poller cb_arg)
 * @return: SPDK_POLLER_BUSY
 *
 * C2HTermReq를 보낸 후 호스트가 FIN을 보내지 않고 SPDK_NVME_TCP_QPAIR_EXIT_TIMEOUT(=30초)이
 * 지나면 이 poller가 강제로 disconnect를 시작한다.
 * recv_state는 ERROR여야 하며 (C2HTermReq 송신으로 QUIESCING → ERROR 전이).
 *
 * 호출 체인:
 *   nvmf_tcp_send_c2h_term_req_complete → SPDK_POLLER_REGISTER → [nvmf_tcp_qpair_handle_timeout]
 *     → nvmf_tcp_qpair_disconnect
 */
static int
nvmf_tcp_qpair_handle_timeout(void *ctx)
{
	struct spdk_nvmf_tcp_qpair *tqpair = ctx;	/* [한국어] poller cb_arg를 tqpair로 캐스트 */

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	/* [한국어] 타임아웃 poller는 항상 ERROR 상태에서만 동작 */

	SPDK_ERRLOG("No pdu coming for tqpair=%p within %d seconds\n", tqpair,
		    SPDK_NVME_TCP_QPAIR_EXIT_TIMEOUT);

	nvmf_tcp_qpair_disconnect(tqpair);	/* [한국어] 타임아웃 — 강제 disconnect 시작 */
	return SPDK_POLLER_BUSY;		/* [한국어] disconnect 처리중 — reactor에 busy 반환 */
}

/*
 * [한국어]
 * nvmf_tcp_send_c2h_term_req_complete - C2HTermReq PDU 송신 완료 콜백
 *
 * @cb_arg: tqpair 포인터
 *
 * C2HTermReq 관리 PDU 송신이 완료되면 호출된다.
 * 아직 timeout_poller가 등록되지 않은 경우 SPDK_NVME_TCP_QPAIR_EXIT_TIMEOUT(=30초)
 * 타임아웃 poller를 등록해 호스트의 FIN을 일정 시간 기다린다.
 *
 * 호출 체인:
 *   _mgmt_pdu_write_done → [nvmf_tcp_send_c2h_term_req_complete]
 *     → SPDK_POLLER_REGISTER(nvmf_tcp_qpair_handle_timeout)
 */
static void
nvmf_tcp_send_c2h_term_req_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair = (struct spdk_nvmf_tcp_qpair *)cb_arg;
	/* [한국어] cb_arg를 tqpair로 캐스트 */

	if (!tqpair->timeout_poller) {
		/* [한국어] 아직 타임아웃 poller 없음 — 등록 (중복 방지) */
		tqpair->timeout_poller = SPDK_POLLER_REGISTER(nvmf_tcp_qpair_handle_timeout, tqpair,
					 SPDK_NVME_TCP_QPAIR_EXIT_TIMEOUT * 1000000);
		/* [한국어] 30초 후 호스트가 응답 없으면 강제 disconnect
		 *          단위: 마이크로초 (1초 = 1000000) */
	}
}

/*
 * [한국어]
 * nvmf_tcp_send_c2h_term_req - 호스트에 C2HTermReq(연결 종료) PDU 송신
 *
 * @tqpair:       종료할 TCP qpair
 * @pdu:          오류를 유발한 원본 PDU (에러 데이터 복사용)
 * @fes:          Fatal Error Status 코드 (NVMe-TCP Table 5)
 * @error_offset: FEI 필드에 넣을 오류 필드 오프셋 (fes가 INVALID_HEADER/DATA일 때)
 *
 * 프로토콜 오류 감지 시 호스트에 종료를 통보하는 C2HTermReq PDU를 구성하고 송신.
 * mgmt_pdu 슬롯에 C2HTermReq 헤더를 채우고, 오류를 일으킨 원본 PDU 헤더를 payload로 첨부.
 * QUIESCING으로 전이해 더 이상 수신을 처리하지 않도록 하고,
 * 완료 콜백(nvmf_tcp_send_c2h_term_req_complete)에서 타임아웃 poller 등록.
 *
 * 호출 체인:
 *   nvmf_tcp_pdu_ch_handle / nvmf_tcp_icreq_handle 등 → [nvmf_tcp_send_c2h_term_req]
 *     → nvmf_tcp_qpair_write_mgmt_pdu → nvmf_tcp_send_c2h_term_req_complete
 */
static void
nvmf_tcp_send_c2h_term_req(struct spdk_nvmf_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
			   enum spdk_nvme_tcp_term_req_fes fes, uint32_t error_offset)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_term_req_hdr *c2h_term_req;
	uint32_t c2h_term_req_hdr_len = sizeof(*c2h_term_req);
	uint32_t copy_len;

	rsp_pdu = tqpair->mgmt_pdu;	/* [한국어] 관리 PDU 슬롯 사용 (ICResp, TermReq 전용) */

	c2h_term_req = &rsp_pdu->hdr.term_req;			/* [한국어] 응답 PDU의 term_req 헤더 필드 */
	c2h_term_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ;
	/* [한국어] PDU 타입 = C2H_TERM_REQ (타깃→호스트 연결 종료 요청) */
	c2h_term_req->common.hlen = c2h_term_req_hdr_len;	/* [한국어] 헤더 길이 */
	c2h_term_req->fes = fes;				/* [한국어] Fatal Error Status 코드 설정 */

	if ((fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		/* [한국어] 헤더/데이터 필드 오류인 경우 FEI(Fatal Error Information)에 오류 필드 오프셋 기록 */
		DSET32(&c2h_term_req->fei, error_offset);
		/* [한국어] DSET32: 리틀엔디안 32비트로 오프셋을 wire 포맷으로 저장 */
	}

	copy_len = spdk_min(pdu->hdr.common.hlen, SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE);
	/* [한국어] 원본 PDU 헤더를 최대 SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE(=152)바이트까지만 복사 */

	/* Copy the error info into the buffer */
	memcpy((uint8_t *)rsp_pdu->hdr.raw + c2h_term_req_hdr_len, pdu->hdr.raw, copy_len);
	/* [한국어] C2HTermReq 헤더 바로 뒤에 오류 원인 PDU 헤더를 복사 (호스트 디버깅용) */
	nvme_tcp_pdu_set_data(rsp_pdu, (uint8_t *)rsp_pdu->hdr.raw + c2h_term_req_hdr_len, copy_len);
	/* [한국어] PDU의 data_iov를 이 복사된 에러 데이터 영역으로 설정 */

	/* Contain the header of the wrong received pdu */
	c2h_term_req->common.plen = c2h_term_req->common.hlen + copy_len;
	/* [한국어] 전체 PDU 길이 = 헤더 + 에러 데이터 길이 */
	tqpair->wait_terminate = true;		/* [한국어] TermReq 대기 중 표시 */
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
	/* [한국어] QUIESCING 전이 — 더 이상 PDU 수신 처리 안 함 */
	nvmf_tcp_qpair_write_mgmt_pdu(tqpair, nvmf_tcp_send_c2h_term_req_complete, tqpair);
	/* [한국어] C2HTermReq PDU 비동기 송신 시작, 완료 시 타임아웃 poller 등록 */
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

/*
 * [한국어]
 * nvmf_tcp_pdu_c2h_data_complete - C2HData PDU 한 조각 송신 완료 콜백
 *
 * @cb_arg: spdk_nvmf_tcp_req 포인터
 *
 * read 응답의 C2HData PDU 한 조각 송신이 완료될 때마다 호출된다.
 * rw_offset이 req.length에 미달하면 아직 보낼 데이터가 남아 있으므로
 * _nvmf_tcp_send_c2h_data를 재귀 호출해 다음 조각을 송신한다.
 * 모두 보내면 SUCCESS 플래그 유무에 따라:
 *   - SUCCESS flag 있음 (c2h_success 최적화): req를 바로 free (CapsuleResp 생략)
 *   - SUCCESS flag 없음: CapsuleResp PDU로 명시적 완료 통지
 *
 * 호출 체인:
 *   _req_pdu_write_done → [nvmf_tcp_pdu_c2h_data_complete]
 *     → (_nvmf_tcp_send_c2h_data | nvmf_tcp_request_free | nvmf_tcp_send_capsule_resp_pdu)
 */
static void
nvmf_tcp_pdu_c2h_data_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_req *tcp_req = cb_arg;	/* [한국어] cb_arg를 tcp_req로 캐스트 */
	struct spdk_nvmf_tcp_qpair *tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair,
					     struct spdk_nvmf_tcp_qpair, qpair);
	/* [한국어] qpair → TCP qpair 다운캐스트 */

	assert(tqpair != NULL);

	if (spdk_unlikely(tcp_req->pdu->rw_offset < tcp_req->req.length)) {
		/* [한국어] 아직 보낼 데이터 남음 — 다음 C2HData 조각 송신 */
		SPDK_DEBUGLOG(nvmf_tcp, "sending another C2H part, offset %u length %u\n", tcp_req->pdu->rw_offset,
			      tcp_req->req.length);
		_nvmf_tcp_send_c2h_data(tqpair, tcp_req);
		return;
	}

	if (tcp_req->pdu->hdr.c2h_data.common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) {
		/* [한국어] SUCCESS flag 있음 (c2h_success=true): 호스트가 마지막 C2HData로 성공 알림 수신
		 *          CapsuleResp 없이 req를 바로 free (C2H success optimization) */
		nvmf_tcp_request_free(tcp_req);
	} else {
		/* [한국어] SUCCESS flag 없음: 별도 CapsuleResp PDU로 NVMe CQE 명시 전송 */
		nvmf_tcp_send_capsule_resp_pdu(tcp_req, tqpair);
	}
}

/*
 * [한국어]
 * nvmf_tcp_r2t_complete - R2T PDU 송신 완료 콜백 — H2C 수신 대기 시작
 *
 * @cb_arg: spdk_nvmf_tcp_req 포인터
 *
 * R2T PDU 송신이 완료되면 req 상태를 TRANSFERRING_HOST_TO_CONTROLLER로 전이해
 * 호스트의 H2C Data PDU를 기다린다.
 * 이미 H2C 데이터가 모두 도착한 특수한 경우(zcopy/in-capsule 등으로 h2c_offset==length)
 * 는 즉시 READY_TO_EXECUTE로 전이해 실행을 시작한다.
 *
 * 호출 체인:
 *   _req_pdu_write_done → [nvmf_tcp_r2t_complete]
 *     → (nvmf_tcp_req_process READY_TO_EXECUTE | 대기)
 */
static void
nvmf_tcp_r2t_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_req *tcp_req = cb_arg;	/* [한국어] cb_arg를 tcp_req로 캐스트 */
	struct spdk_nvmf_tcp_transport *ttransport;

	ttransport = SPDK_CONTAINEROF(tcp_req->req.qpair->transport,
				      struct spdk_nvmf_tcp_transport, transport);
	/* [한국어] qpair → transport → TCP transport 다운캐스트 */

	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
	/* [한국어] AWAITING_R2T_ACK → TRANSFERRING_HOST_TO_CONTROLLER 전이 (H2C 수신 대기) */

	if (tcp_req->h2c_offset == tcp_req->req.length) {
		/* [한국어] 이미 모든 H2C 데이터 수신 완료 (R2T가 불필요했던 특수 경우) */
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
		nvmf_tcp_req_process(ttransport, tcp_req);	/* [한국어] 즉시 실행 단계로 진행 */
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

/*
 * [한국어]
 * nvmf_tcp_h2c_term_req_dump - H2CTermReq PDU의 에러 내용을 로그에 출력
 *
 * @h2c_term_req: 덤프할 H2CTermReq PDU 헤더
 *
 * 호스트가 보낸 H2CTermReq(연결 종료 요청) PDU의 FES 문자열과
 * FEI(오류 필드 오프셋)를 에러 로그로 출력한다. 디버깅 전용.
 *
 * 호출 체인:
 *   nvmf_tcp_h2c_term_req_payload_handle → [nvmf_tcp_h2c_term_req_dump]
 */
static void
nvmf_tcp_h2c_term_req_dump(struct spdk_nvme_tcp_term_req_hdr *h2c_term_req)
{
	SPDK_ERRLOG("Error info of pdu(%p): %s\n", h2c_term_req,
		    spdk_nvmf_tcp_term_req_fes_str[h2c_term_req->fes]);
	/* [한국어] FES 코드를 사람이 읽을 수 있는 문자열로 변환해 출력 */
	if ((h2c_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (h2c_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		/* [한국어] FEI가 의미 있는 FES 코드에서만 오프셋 출력 */
		SPDK_DEBUGLOG(nvmf_tcp, "The offset from the start of the PDU header is %u\n",
			      DGET32(h2c_term_req->fei));
		/* [한국어] DGET32: 리틀엔디안 4바이트를 uint32_t로 읽기 */
	}
}

/*
 * [한국어]
 * nvmf_tcp_h2c_term_req_hdr_handle - H2CTermReq PDU 헤더 수신 완료 핸들러
 *
 * @tqpair: 수신한 qpair
 * @pdu:    헤더 수신이 끝난 PDU
 *
 * 호스트가 연결 종료를 요청하는 H2CTermReq PDU의 헤더 처리.
 * FES 코드 검증 후 payload(에러 데이터) 수신으로 전이한다.
 * 알 수 없는 FES 코드이면 C2HTermReq로 에러 응답.
 *
 * 호출 체인:
 *   nvmf_tcp_pdu_psh_handle → [nvmf_tcp_h2c_term_req_hdr_handle]
 */
static void
nvmf_tcp_h2c_term_req_hdr_handle(struct spdk_nvmf_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req = &pdu->hdr.term_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	if (h2c_term_req->fes > SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER) {
		/* [한국어] 알 수 없는 FES 코드 — C2HTermReq로 에러 응답 */
		SPDK_ERRLOG("Fatal Error Status(FES) is unknown for h2c_term_req pdu=%p\n", pdu);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_term_req_hdr, fes);
		goto end;
	}

	/* set the data buffer */
	nvme_tcp_pdu_set_data(pdu, (uint8_t *)pdu->hdr.raw + h2c_term_req->common.hlen,
			      h2c_term_req->common.plen - h2c_term_req->common.hlen);
	/* [한국어] payload = 헤더 바로 뒤부터 (plen - hlen) 바이트 — 에러 데이터 수신 준비 */
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	/* [한국어] payload 수신 대기 상태로 전이 */
	return;
end:
	nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
	/* [한국어] FES 오류 응답 후 연결 종료 */
}

/*
 * [한국어]
 * nvmf_tcp_h2c_term_req_payload_handle - H2CTermReq payload 수신 완료 핸들러
 *
 * @tqpair: 수신한 qpair
 * @pdu:    payload 수신이 끝난 PDU
 *
 * H2CTermReq의 에러 데이터(payload)를 모두 수신한 후 에러 내용을 로그에 덤프하고
 * QUIESCING 상태로 전이해 연결 종료를 시작한다.
 *
 * 호출 체인:
 *   _nvmf_tcp_pdu_payload_handle → [nvmf_tcp_h2c_term_req_payload_handle]
 *     → nvmf_tcp_qpair_set_recv_state(QUIESCING)
 */
static void
nvmf_tcp_h2c_term_req_payload_handle(struct spdk_nvmf_tcp_qpair *tqpair,
				     struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req = &pdu->hdr.term_req;

	nvmf_tcp_h2c_term_req_dump(h2c_term_req);		/* [한국어] 에러 내용 로그 출력 */
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
	/* [한국어] 호스트가 연결 종료를 원함 — QUIESCING으로 연결 종료 절차 시작 */
}

/*
 * [한국어]
 * _nvmf_tcp_pdu_payload_handle - PDU 타입별 payload 처리 디스패처
 *
 * @tqpair: 수신한 qpair
 * @pdu:    payload 수신이 완료된 PDU
 *
 * PDU 타입에 따라 적절한 payload 핸들러를 호출하고,
 * 처리 완료 후 PDU 슬롯을 free pool로 반환한다.
 * 이 함수는 DDgst 검증(nvmf_tcp_pdu_payload_handle)이나
 * accel CRC 완료(data_crc32_calc_done) 후 호출된다.
 *
 * 호출 체인:
 *   data_crc32_calc_done / nvmf_tcp_pdu_payload_handle → [_nvmf_tcp_pdu_payload_handle]
 *     → (capsule_cmd_payload_handle | h2c_data_payload_handle | h2c_term_req_payload_handle)
 */
static void
_nvmf_tcp_pdu_payload_handle(struct spdk_nvmf_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_transport *ttransport = SPDK_CONTAINEROF(tqpair->qpair.transport,
			struct spdk_nvmf_tcp_transport, transport);
	/* [한국어] qpair → transport → TCP transport 다운캐스트 */

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
		nvmf_tcp_capsule_cmd_payload_handle(ttransport, tqpair, pdu);
		/* [한국어] CapsuleCmd in-capsule data 수신 완료 처리 */
		break;
	case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
		nvmf_tcp_h2c_data_payload_handle(ttransport, tqpair, pdu);
		/* [한국어] H2C Data(write data) 수신 완료 처리 */
		break;

	case SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ:
		nvmf_tcp_h2c_term_req_payload_handle(tqpair, pdu);
		/* [한국어] 호스트 연결 종료 요청 에러 데이터 처리 */
		break;

	default:
		/* The code should not go to here */
		SPDK_ERRLOG("ERROR pdu type %d\n", pdu->hdr.common.pdu_type);
		/* [한국어] payload 있는 PDU는 위 3개뿐 — 여기 오면 논리 오류 */
		break;
	}
	SLIST_INSERT_HEAD(&tqpair->tcp_pdu_free_queue, pdu, slist);
	/* [한국어] PDU 슬롯 처리 완료 — free pool로 반환 */
	tqpair->tcp_pdu_working_count--;
	/* [한국어] working PDU 카운터 감소 */
}

/*
 * [한국어]
 * nvmf_tcp_req_set_cpl - req의 NVMe CQE에 완료 상태를 설정
 *
 * @treq: 완료 상태를 설정할 TCP req
 * @sct:  Status Code Type (SPDK_NVME_SCT_*)
 * @sc:   Status Code (SPDK_NVME_SC_*)
 *
 * DDgst 에러 등 transport 오류 발생 시 req의 CQE에 TRANSIENT_TRANSPORT_ERROR를
 * 설정하기 위해 사용한다. cid도 cmd에서 복사해 호스트가 어느 명령 응답인지 식별 가능.
 *
 * 호출 체인:
 *   data_crc32_calc_done → [nvmf_tcp_req_set_cpl]
 */
static inline void
nvmf_tcp_req_set_cpl(struct spdk_nvmf_tcp_req *treq, int sct, int sc)
{
	treq->req.rsp->nvme_cpl.status.sct = sct;	/* [한국어] Status Code Type 설정 */
	treq->req.rsp->nvme_cpl.status.sc = sc;		/* [한국어] Status Code 설정 */
	treq->req.rsp->nvme_cpl.cid = treq->req.cmd->nvme_cmd.cid;
	/* [한국어] CID 복사 — 호스트가 응답을 어느 명령과 매핑할지 식별 */
}

/*
 * [한국어]
 * data_crc32_calc_done - 수신 PDU DDgst(Data Digest) accel CRC32C 검증 완료 콜백
 *
 * @cb_arg: nvme_tcp_pdu 포인터
 * @status: accel 계산 결과 (0=성공, 비0=accel 실패)
 *
 * 수신한 H2C/CapsuleCmd payload의 DDgst 검증이 accel로 비동기 완료될 때 호출.
 * accel 실패 시 동기 소프트웨어 CRC32C로 fallback.
 * CRC 불일치 시 req에 TRANSIENT_TRANSPORT_ERROR 설정해 에러 완료 경로로 유도.
 * 검증 후 _nvmf_tcp_pdu_payload_handle로 정상 payload 처리 계속 진행.
 *
 * 호출 체인:
 *   nvmf_tcp_pdu_payload_handle → spdk_accel_submit_crc32cv → [data_crc32_calc_done]
 *     → _nvmf_tcp_pdu_payload_handle
 */
static void
data_crc32_calc_done(void *cb_arg, int status)
{
	struct nvme_tcp_pdu *pdu = cb_arg;		/* [한국어] cb_arg를 PDU로 캐스트 */
	struct spdk_nvmf_tcp_qpair *tqpair = pdu->qpair;	/* [한국어] PDU에 연결된 qpair */

	/* async crc32 calculation is failed and use direct calculation to check */
	if (spdk_unlikely(status)) {
		/* [한국어] accel 실패 — 소프트웨어 CRC32C로 동기 검증 */
		SPDK_ERRLOG("Data digest on tqpair=(%p) with pdu=%p failed to be calculated asynchronously\n",
			    tqpair, pdu);
		pdu->data_digest_crc32 = nvme_tcp_pdu_calc_data_digest(pdu);
		/* [한국어] 소프트웨어 CRC32C 계산으로 fallback */
	}
	pdu->data_digest_crc32 ^= SPDK_CRC32C_XOR;
	/* [한국어] CRC32C 최종 처리: 0xFFFFFFFF XOR (NVMe-TCP 스펙 요구사항) */
	if (!MATCH_DIGEST_WORD(pdu->data_digest, pdu->data_digest_crc32)) {
		/* [한국어] wire에서 수신한 DDgst와 계산값 불일치 — 데이터 손상 감지 */
		SPDK_ERRLOG("Data digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
		assert(pdu->req != NULL);
		nvmf_tcp_req_set_cpl(pdu->req, SPDK_NVME_SCT_GENERIC,
				     SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR);
		/* [한국어] req CQE에 TRANSIENT_TRANSPORT_ERROR 설정 — 이후 READY_TO_COMPLETE 전이 */
	}
	_nvmf_tcp_pdu_payload_handle(tqpair, pdu);
	/* [한국어] DDgst 검증 완료 — payload 타입별 처리로 진행 */
}

/*
 * [한국어]
 * nvmf_tcp_pdu_payload_handle - PDU payload 수신 완료 처리 진입점
 *
 * @tqpair: 수신한 qpair
 * @pdu:    payload 수신이 완료된 PDU
 *
 * pdu_in_progress를 NULL로 초기화하고 AWAIT_PDU_READY 상태로 전이.
 * DDgst가 활성화되어 있으면:
 *   - IO qpair & DIF 없음 & 정렬됨 & group 있음: accel CRC32C 비동기 검증 시작
 *   - 그 외: 동기 CRC32C 검증 후 _nvmf_tcp_pdu_payload_handle 바로 호출
 * DDgst 비활성화: _nvmf_tcp_pdu_payload_handle 직접 호출.
 *
 * 호출 체인:
 *   nvmf_tcp_sock_process(AWAIT_PDU_PAYLOAD) → [nvmf_tcp_pdu_payload_handle]
 *     → (spdk_accel_submit_crc32cv → data_crc32_calc_done | _nvmf_tcp_pdu_payload_handle)
 */
static void
nvmf_tcp_pdu_payload_handle(struct spdk_nvmf_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	int rc = 0;
	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	/* [한국어] payload 수신 완료는 AWAIT_PDU_PAYLOAD 상태에서만 발생 */
	tqpair->pdu_in_progress = NULL;		/* [한국어] payload 수신 완료 — pdu_in_progress 해제 */
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	/* [한국어] 다음 PDU 헤더 수신 준비 상태로 전이 */
	SPDK_DEBUGLOG(nvmf_tcp, "enter\n");
	/* check data digest if need */
	if (pdu->ddgst_enable) {
		/* [한국어] DDgst 활성화 — 수신한 DDgst를 계산값과 비교 검증 */
		if (tqpair->qpair.qid != 0 && !pdu->dif_ctx && tqpair->group &&
		    (pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT == 0)) {
			/* [한국어] accel 최적 경로 조건:
			 *   qpair.qid != 0: IO qpair (admin qpair는 accel 사용 안 함)
			 *   dif_ctx == NULL: DIF/DIX와 DDgst 동시 처리 미지원
			 *   group != NULL: accel_channel 있음
			 *   데이터 길이 4바이트 정렬 */
			rc = spdk_accel_submit_crc32cv(tqpair->group->accel_channel, &pdu->data_digest_crc32, pdu->data_iov,
						       pdu->data_iovcnt, 0, data_crc32_calc_done, pdu);
			/* [한국어] accel CRC32C 비동기 검증 요청 — 완료 시 data_crc32_calc_done 호출 */
			if (spdk_likely(rc == 0)) {
				return;	/* [한국어] 비동기 시작 성공 — 완료 콜백 기다림 */
			}
		} else {
			pdu->data_digest_crc32 = nvme_tcp_pdu_calc_data_digest(pdu);
			/* [한국어] 조건 미충족 — 동기 소프트웨어 CRC32C 계산 */
		}
		data_crc32_calc_done(pdu, rc);	/* [한국어] 동기 계산 완료 후 직접 done 처리 */
	} else {
		_nvmf_tcp_pdu_payload_handle(tqpair, pdu);
		/* [한국어] DDgst 불필요 — 바로 payload 타입별 처리 */
	}
}

/*
 * [한국어]
 * nvmf_tcp_send_icresp_complete - ICResp(Initialize Connection Response) 송신 완료 콜백
 *
 * @cb_arg: tqpair 포인터
 *
 * ICResp PDU 송신 완료 후 qpair 상태를 INITIALIZING → RUNNING으로 전이.
 * RUNNING 상태부터 CapsuleCmd, H2C Data 등 실제 I/O PDU를 처리한다.
 *
 * 호출 체인:
 *   _mgmt_pdu_write_done → [nvmf_tcp_send_icresp_complete]
 *     → nvmf_tcp_qpair_set_state(RUNNING)
 */
static void
nvmf_tcp_send_icresp_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair = cb_arg;	/* [한국어] cb_arg를 tqpair로 캐스트 */

	nvmf_tcp_qpair_set_state(tqpair, NVMF_TCP_QPAIR_STATE_RUNNING);
	/* [한국어] ICResp 송신 완료 → RUNNING 상태 전이 — 이제 일반 I/O 처리 가능 */
}

/*
 * [한국어]
 * nvmf_tcp_icreq_handle - ICReq(Initialize Connection Request) PDU 처리
 *
 * @ttransport: TCP transport (max_io_size 등 옵션 참조용)
 * @tqpair:     초기화 중인 TCP qpair
 * @pdu:        수신한 ICReq PDU
 *
 * NVMe-TCP 연결 초기화의 핵심 함수. 호스트가 보낸 ICReq를 파싱하고 검증한 뒤
 * ICResp를 구성해 응답한다. 다음을 수행:
 *   1) PFV(PDU Format Version) 검증 — 현재 0만 지원
 *   2) HPDA(Header Padding Data Alignment, dwords 단위) 범위 검증
 *   3) HDgst/DDgst 활성화 여부 결정 (ic_req->dgst.bits)
 *   4) 수신 버퍼 크기 조정 (digest 여부에 따라)
 *   5) CPDA = min(ic_req->hpda, CPDA_MAX) 설정
 *   6) ICResp 헤더 구성 (pfv=0, cpda, maxh2cdata, dgst)
 *   7) INITIALIZING 상태로 전이, ICResp 비동기 송신
 *   8) AWAIT_PDU_READY로 수신 상태 전이
 *
 * 실행 컨텍스트: poll group spdk_thread (ICReq 수신 경로).
 *
 * 호출 체인:
 *   nvmf_tcp_pdu_psh_handle → [nvmf_tcp_icreq_handle]
 *     → nvmf_tcp_qpair_write_mgmt_pdu → nvmf_tcp_send_icresp_complete
 */
static void
nvmf_tcp_icreq_handle(struct spdk_nvmf_tcp_transport *ttransport,
		      struct spdk_nvmf_tcp_qpair *tqpair,
		      struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_ic_req *ic_req = &pdu->hdr.ic_req;	/* [한국어] ICReq PDU 헤더 */
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_ic_resp *ic_resp;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	int rc;

	/* Only PFV 0 is defined currently */
	if (ic_req->pfv != 0) {
		/* [한국어] PFV(PDU Format Version) != 0이면 미지원 버전 — TermReq 응답 */
		SPDK_ERRLOG("Expected ICReq PFV %u, got %u\n", 0u, ic_req->pfv);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_req, pfv);
		goto end;
	}

	/* This value is 0’s based value in units of dwords should not be larger than SPDK_NVME_TCP_HPDA_MAX */
	if (ic_req->hpda > SPDK_NVME_TCP_HPDA_MAX) {
		/* [한국어] HPDA(헤더 패딩 정렬, 0-based dwords) 범위 초과 (0~31) */
		SPDK_ERRLOG("ICReq HPDA out of range 0 to 31, got %u\n", ic_req->hpda);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_req, hpda);
		goto end;
	}

	/* MAXR2T is 0’s based */
	SPDK_DEBUGLOG(nvmf_tcp, "maxr2t =%u\n", (ic_req->maxr2t + 1u));
	/* [한국어] maxr2t: 호스트가 허용하는 최대 미완료 R2T 수 (0-based, 실제값+1) */

	tqpair->host_hdgst_enable = ic_req->dgst.bits.hdgst_enable ? true : false;
	/* [한국어] HDgst: 호스트가 요청한 Header Digest 활성화 여부 */
	if (!tqpair->host_hdgst_enable) {
		tqpair->recv_buf_size -= SPDK_NVME_TCP_DIGEST_LEN * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR;
		/* [한국어] HDgst 비활성 — 수신 버퍼에서 HDgst 공간 제거 (4바이트 * FACTOR) */
	}

	tqpair->host_ddgst_enable = ic_req->dgst.bits.ddgst_enable ? true : false;
	/* [한국어] DDgst: 호스트가 요청한 Data Digest 활성화 여부 */
	if (!tqpair->host_ddgst_enable) {
		tqpair->recv_buf_size -= SPDK_NVME_TCP_DIGEST_LEN * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR;
		/* [한국어] DDgst 비활성 — 수신 버퍼에서 DDgst 공간 제거 */
	}

	tqpair->recv_buf_size = spdk_max(tqpair->recv_buf_size, MIN_SOCK_PIPE_SIZE);
	/* [한국어] digest 제거 후에도 최소 소켓 파이프 크기 보장 */
	/* Now that we know whether digests are enabled, properly size the receive buffer */
	rc = spdk_sock_set_recvbuf(tqpair->sock, tqpair->recv_buf_size);
	/* [한국어] SO_RCVBUF 설정 — 커널 TCP 수신 버퍼 크기를 계산된 값으로 설정 */
	if (rc < 0) {
		SPDK_WARNLOG("spdk_sock_set_recvbuf() failed, rc %d: %s. Unable to allocate enough memory for receive buffer on tqpair=%p with size=%d\n",
			     rc, spdk_strerror(-rc), tqpair, tqpair->recv_buf_size);
		/* Not fatal. */
		/* [한국어] 버퍼 크기 조정 실패는 치명적이지 않음 — 기본 크기로 계속 */
	}

	tqpair->cpda = spdk_min(ic_req->hpda, SPDK_NVME_TCP_CPDA_MAX);
	/* [한국어] CPDA(Controller PDU Data Alignment) = min(호스트 HPDA, CPDA_MAX)
	 *          이후 데이터 PDU의 data offset 정렬에 사용 */
	SPDK_DEBUGLOG(nvmf_tcp, "cpda of tqpair=(%p) is : %u\n", tqpair, tqpair->cpda);

	rsp_pdu = tqpair->mgmt_pdu;	/* [한국어] ICResp도 mgmt_pdu 슬롯 사용 */

	ic_resp = &rsp_pdu->hdr.ic_resp;			/* [한국어] ICResp 헤더 필드 */
	ic_resp->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_RESP;	/* [한국어] PDU 타입 = IC_RESP */
	ic_resp->common.hlen = ic_resp->common.plen =  sizeof(*ic_resp);
	/* [한국어] ICResp는 payload 없으므로 hlen == plen */
	ic_resp->pfv = 0;			/* [한국어] PFV = 0 (현재 유일하게 정의된 버전) */
	ic_resp->cpda = tqpair->cpda;		/* [한국어] 협상된 CPDA 반환 */
	ic_resp->maxh2cdata = ttransport->transport.opts.max_io_size;
	/* [한국어] 타깃이 한 H2C Data PDU에서 허용하는 최대 데이터 크기 = max_io_size */
	ic_resp->dgst.bits.hdgst_enable = tqpair->host_hdgst_enable ? 1 : 0;
	/* [한국어] 협상된 HDgst 활성화 여부를 응답에 포함 */
	ic_resp->dgst.bits.ddgst_enable = tqpair->host_ddgst_enable ? 1 : 0;
	/* [한국어] 협상된 DDgst 활성화 여부를 응답에 포함 */

	SPDK_DEBUGLOG(nvmf_tcp, "host_hdgst_enable: %u\n", tqpair->host_hdgst_enable);
	SPDK_DEBUGLOG(nvmf_tcp, "host_ddgst_enable: %u\n", tqpair->host_ddgst_enable);

	nvmf_tcp_qpair_set_state(tqpair, NVMF_TCP_QPAIR_STATE_INITIALIZING);
	/* [한국어] NEW → INITIALIZING 상태 전이 (ICResp 송신 중) */
	nvmf_tcp_qpair_write_mgmt_pdu(tqpair, nvmf_tcp_send_icresp_complete, tqpair);
	/* [한국어] ICResp PDU 비동기 송신 시작, 완료 시 RUNNING 전이 */
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	/* [한국어] 다음 PDU(CapsuleCmd 등) 수신 준비 상태로 전이 */
	return;
end:
	nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
	/* [한국어] ICReq 검증 실패 — C2HTermReq로 연결 종료 통보 */
}

/*
 * [한국어]
 * nvmf_tcp_pdu_psh_handle - PDU 특정 헤더(PSH) 수신 완료 핸들러
 *
 * @tqpair:       수신한 qpair
 * @ttransport:   TCP transport (핸들러에 전달)
 *
 * AWAIT_PDU_PSH 상태에서 공통 헤더(CH) + 특정 헤더(PSH)가 모두 수신되면 호출.
 * HDgst 활성화 시 헤더 CRC32C를 검증하고 불일치 시 C2HTermReq 응답.
 * PDU 타입에 따라 해당 핸들러로 디스패치:
 *   - IC_REQ: nvmf_tcp_icreq_handle (연결 초기화)
 *   - CAPSULE_CMD: AWAIT_REQ 상태로만 전이 (payload 처리는 다음 state에서)
 *   - H2C_DATA: nvmf_tcp_h2c_data_hdr_handle
 *   - H2C_TERM_REQ: nvmf_tcp_h2c_term_req_hdr_handle
 *   - 기타: C2HTermReq로 에러 응답
 *
 * 실행 컨텍스트: poll group spdk_thread (nvmf_tcp_sock_process 경유).
 *
 * 호출 체인:
 *   nvmf_tcp_sock_process(AWAIT_PDU_PSH) → [nvmf_tcp_pdu_psh_handle]
 *     → 타입별 핸들러
 */
static void
nvmf_tcp_pdu_psh_handle(struct spdk_nvmf_tcp_qpair *tqpair,
			struct spdk_nvmf_tcp_transport *ttransport)
{
	struct nvme_tcp_pdu *pdu;
	int rc;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
	/* [한국어] PSH 수신 완료는 AWAIT_PDU_PSH 상태에서만 발생 */
	pdu = tqpair->pdu_in_progress;	/* [한국어] 현재 수신 중인 PDU 슬롯 */

	SPDK_DEBUGLOG(nvmf_tcp, "pdu type of tqpair(%p) is %d\n", tqpair,
		      pdu->hdr.common.pdu_type);
	/* check header digest if needed */
	if (pdu->has_hdgst) {
		/* [한국어] HDgst 활성화된 PDU — CRC32C 검증 */
		SPDK_DEBUGLOG(nvmf_tcp, "Compare the header of pdu=%p on tqpair=%p\n", pdu, tqpair);
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		/* [한국어] 헤더 CRC32C 계산 (소프트웨어 동기 계산) */
		rc = MATCH_DIGEST_WORD((uint8_t *)pdu->hdr.raw + pdu->hdr.common.hlen, crc32c);
		/* [한국어] wire에서 수신한 HDgst(헤더 바로 뒤 4바이트)와 비교 */
		if (rc == 0) {
			/* [한국어] HDgst 불일치 — 헤더 손상 감지, TermReq로 에러 응답 */
			SPDK_ERRLOG("Header digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_HDGST_ERROR;
			nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
			return;

		}
	}

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_IC_REQ:
		nvmf_tcp_icreq_handle(ttransport, tqpair, pdu);
		/* [한국어] 연결 초기화 요청 처리 — ICResp 응답 후 RUNNING 전이 */
		break;
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_REQ);
		/* [한국어] CapsuleCmd: 헤더만 수신 완료, req 할당 후 payload 처리 대기 */
		break;
	case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
		nvmf_tcp_h2c_data_hdr_handle(ttransport, tqpair, pdu);
		/* [한국어] H2C Data(write data) 헤더 처리 — H2C 데이터 수신 준비 */
		break;

	case SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ:
		nvmf_tcp_h2c_term_req_hdr_handle(tqpair, pdu);
		/* [한국어] 호스트 연결 종료 요청 헤더 처리 */
		break;

	default:
		/* [한국어] 타깃이 수신할 수 없는 PDU 타입 (예: IC_RESP, C2H_DATA, R2T 등) */
		SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->pdu_in_progress->hdr.common.pdu_type);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = 1;	/* [한국어] pdu_type 필드 오프셋 = 1 */
		nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
		break;
	}
}

/*
 * [한국어]
 * nvmf_tcp_pdu_ch_handle - 8바이트 공통 헤더(CH) 수신 완료 후 검증 및 다음 단계 결정
 *
 * @tqpair: 처리 대상 TCP qpair
 * @return: 없음 (void). 오류 시 nvmf_tcp_send_c2h_term_req로 TermReq 송신 후 QUIESCING 전이.
 *
 * PDU 수신 상태 머신의 AWAIT_PDU_CH 단계에서 공통 헤더 8바이트 수신이 완료됐을 때 호출된다.
 * 역할: (1) qpair 상태와 PDU 타입의 정합성 검증, (2) hlen/plen/pdo 필드 유효성 검증,
 * (3) 정상이면 AWAIT_PDU_PSH로 전이 + PSH 수신 길이 계산, (4) 오류이면 C2HTermReq 송신.
 *
 * 검증 항목:
 *   - ICReq: qpair가 INVALID 상태(최초 연결)여야만 허용 (중복 ICReq 차단)
 *   - 기타 PDU: qpair가 RUNNING 상태여야 허용 (협상 완료 전에는 불가)
 *   - CapsuleCmd/H2CData: CPDA 정렬 검사 (cpda != 0이면 pdo가 (cpda+1)*4의 배수여야 함)
 *   - 모든 PDU: hlen == expected_hlen, plen >= expected_hlen (H2CTermReq는 범위 검사)
 *
 * 실행 컨텍스트: poll group 스레드 (nvmf_tcp_sock_process 내 단일 스레드 직렬 실행)
 *
 * 호출 체인:
 *   nvmf_tcp_sock_process(AWAIT_PDU_CH) → [nvmf_tcp_pdu_ch_handle]
 *     → nvmf_tcp_qpair_set_recv_state(AWAIT_PDU_PSH) (정상)
 *     → nvmf_tcp_send_c2h_term_req (오류)
 */
static void
nvmf_tcp_pdu_ch_handle(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *pdu;
	uint32_t error_offset = 0;	/* [한국어] FEI(Fatal Error Information): 오류가 있는 헤더 필드 오프셋 */
	enum spdk_nvme_tcp_term_req_fes fes;	/* [한국어] FES(Fatal Error Status): TermReq에 실을 오류 코드 */
	uint8_t expected_hlen, pdo;	/* [한국어] expected_hlen: PDU 타입에 따른 기대 헤더 길이; pdo: PDU Data Offset */
	bool plen_error = false, pdo_error = false;	/* [한국어] plen/pdo 오류 플래그 (goto err 방지용 지연 처리) */

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
	/* [한국어] 이 함수는 AWAIT_PDU_CH 상태에서만 호출 가능 — 불변식 검증 */
	pdu = tqpair->pdu_in_progress;		/* [한국어] 현재 수신 중인 PDU 슬롯 참조 */
	assert(pdu);	/* [한국어] AWAIT_PDU_CH에서 pdu는 항상 유효해야 함 */
	if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_REQ) {
		/* [한국어] ICReq PDU — 연결 초기화 요청. INVALID 상태에서만 허용 */
		if (tqpair->state != NVMF_TCP_QPAIR_STATE_INVALID) {
			/* [한국어] 이미 협상이 완료된 qpair에 또다른 ICReq 수신 — 시퀀스 오류 */
			SPDK_ERRLOG("Already received ICreq PDU, and reject this pdu=%p\n", pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}
		expected_hlen = sizeof(struct spdk_nvme_tcp_ic_req);
		/* [한국어] ICReq 헤더는 고정 크기 (spdk_nvme_tcp_ic_req 구조체) */
		if (pdu->hdr.common.plen != expected_hlen) {
			/* [한국어] ICReq는 plen == hlen이어야 함 (페이로드 없음) */
			plen_error = true;
		}
	} else {
		/* [한국어] ICReq 이외 PDU — RUNNING 상태(협상 완료)여야만 허용 */
		if (tqpair->state != NVMF_TCP_QPAIR_STATE_RUNNING) {
			/* [한국어] 협상 전에 데이터 PDU 수신 — 시퀀스 오류 */
			SPDK_ERRLOG("The TCP/IP connection is not negotiated\n");
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}

		switch (pdu->hdr.common.pdu_type) {
		case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
			/* [한국어] CapsuleCmd: NVMe 명령 캡슐 PDU. 헤더=spdk_nvme_tcp_cmd */
			expected_hlen = sizeof(struct spdk_nvme_tcp_cmd);
			pdo = pdu->hdr.common.pdo;	/* [한국어] PDU Data Offset: 데이터 시작 오프셋 */
			if ((tqpair->cpda != 0) && (pdo % ((tqpair->cpda + 1) << 2) != 0)) {
				/* [한국어] CPDA(Controller PDU Data Alignment) 위반:
				 * cpda != 0이면 pdo가 (cpda+1)*4의 배수여야 함.
				 * 예: cpda=1 → 8바이트 정렬, cpda=3 → 16바이트 정렬. */
				pdo_error = true;
				break;
			}

			if (pdu->hdr.common.plen < expected_hlen) {
				/* [한국어] plen이 최소 헤더 크기보다 작음 — 잘린 PDU */
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
			/* [한국어] H2CData: Host-to-Controller 데이터 PDU. R2T에 대한 응답. */
			expected_hlen = sizeof(struct spdk_nvme_tcp_h2c_data_hdr);
			pdo = pdu->hdr.common.pdo;	/* [한국어] H2CData의 PDO도 CPDA 정렬 대상 */
			if ((tqpair->cpda != 0) && (pdo % ((tqpair->cpda + 1) << 2) != 0)) {
				/* [한국어] H2CData도 CPDA 정렬 위반 시 오류 */
				pdo_error = true;
				break;
			}
			if (pdu->hdr.common.plen < expected_hlen) {
				plen_error = true;
			}
			break;

		case SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ:
			/* [한국어] H2CTermReq: 호스트 측 연결 종료 요청 PDU */
			expected_hlen = sizeof(struct spdk_nvme_tcp_term_req_hdr);
			if ((pdu->hdr.common.plen <= expected_hlen) ||
			    (pdu->hdr.common.plen > SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE)) {
				/* [한국어] TermReq는 hdr보다 커야 하고(오류 정보 포함)
				 * 최대 크기(SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE=152)를 초과해서는 안 됨 */
				plen_error = true;
			}
			break;

		default:
			/* [한국어] 타깃이 수신할 수 없는 PDU 타입 (예: IC_RESP, C2H_DATA, R2T 등) */
			SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", pdu->hdr.common.pdu_type);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdu_type);
			/* [한국어] FEI = pdu_type 필드의 오프셋 (헤더 내 위치) */
			goto err;
		}
	}

	if (pdu->hdr.common.hlen != expected_hlen) {
		/* [한국어] hlen 불일치: 수신된 헤더 길이가 PDU 타입에 따른 기대값과 다름 */
		SPDK_ERRLOG("PDU type=0x%02x, Expected ICReq header length %u, got %u on tqpair=%p\n",
			    pdu->hdr.common.pdu_type,
			    expected_hlen, pdu->hdr.common.hlen, tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, hlen);
		/* [한국어] FEI = hlen 필드 오프셋 */
		goto err;
	} else if (pdo_error) {
		/* [한국어] PDO 정렬 오류: goto err를 통해 TermReq 송신 */
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdo);
		/* [한국어] FEI = pdo 필드 오프셋 */
	} else if (plen_error) {
		/* [한국어] plen 오류: TermReq 송신 */
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, plen);
		/* [한국어] FEI = plen 필드 오프셋 */
		goto err;
	} else {
		/* [한국어] 모든 검증 통과 — AWAIT_PDU_PSH 상태로 전이 */
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
		/* [한국어] 상태 전이: AWAIT_PDU_CH → AWAIT_PDU_PSH */
		nvme_tcp_pdu_calc_psh_len(tqpair->pdu_in_progress, tqpair->host_hdgst_enable);
		/* [한국어] PSH 수신 바이트 수 계산: hlen + (HDgst 활성이면 4바이트) - 8 (CH 제외) */
		return;
	}
err:
	nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
	/* [한국어] 검증 실패 — C2HTermReq PDU 송신 후 QUIESCING 전이 */
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

/*
 * [한국어]
 * nvmf_tcp_control_msg_get - control_msg 버퍼 할당 (없으면 req를 대기 큐에 삽입)
 *
 * @list:    control_msg_list (free_msgs와 waiting_for_msg_reqs 관리)
 * @tcp_req: 버퍼를 요청하는 req (할당 불가 시 대기 큐에 삽입)
 * @return:  할당된 버퍼 포인터; 없으면 NULL (NEED_BUFFER 상태 유지)
 *
 * ICD가 transport->in_capsule_data_size를 초과하는 admin/fabric 명령의
 * in-capsule 데이터 수신을 위해 8192바이트 control_msg 버퍼를 할당한다.
 * 버퍼가 없으면 tcp_req를 waiting_for_msg_reqs 큐에 삽입하고 NULL 반환.
 * 이후 nvmf_tcp_control_msg_put에서 버퍼 반환 시 대기 req가 깨어난다.
 *
 * 호출 체인:
 *   nvmf_tcp_req_parse_sgl → [nvmf_tcp_control_msg_get]
 */
static inline void *
nvmf_tcp_control_msg_get(struct spdk_nvmf_tcp_control_msg_list *list,
			 struct spdk_nvmf_tcp_req *tcp_req)
{
	struct spdk_nvmf_tcp_control_msg *msg;

	assert(list);	/* [한국어] list가 NULL이면 논리 오류 */

	msg = STAILQ_FIRST(&list->free_msgs);	/* [한국어] free 큐 선두에서 버퍼 조회 */
	if (!msg) {
		/* [한국어] 사용 가능한 버퍼 없음 — req를 대기 큐에 삽입 후 NULL 반환 */
		SPDK_DEBUGLOG(nvmf_tcp, "Out of control messages\n");
		STAILQ_INSERT_TAIL(&list->waiting_for_msg_reqs, tcp_req, control_msg_link);
		return NULL;
	}
	STAILQ_REMOVE_HEAD(&list->free_msgs, link);	/* [한국어] free 큐 선두에서 제거 */
	return msg;	/* [한국어] 할당된 버퍼 반환 */
}

/*
 * [한국어]
 * nvmf_tcp_control_msg_put - control_msg 버퍼 반환 및 대기 req 처리 재개
 *
 * @list:  control_msg_list
 * @_msg:  반환할 버퍼 포인터
 *
 * admin/fabric 명령의 in-capsule 데이터 수신이 완료된 후 버퍼를 반환.
 * 반환 후 waiting_for_msg_reqs 대기 큐에 req가 있으면 첫 번째 req를 꺼내
 * nvmf_tcp_req_process로 처리 재개한다 (NEED_BUFFER → HAVE_BUFFER 전이 경로).
 *
 * 호출 체인:
 *   nvmf_tcp_req_process(HAVE_BUFFER, in-capsule 경로) → [nvmf_tcp_control_msg_put]
 *     → nvmf_tcp_req_process(대기 중인 다음 req)
 */
static inline void
nvmf_tcp_control_msg_put(struct spdk_nvmf_tcp_control_msg_list *list, void *_msg)
{
	struct spdk_nvmf_tcp_control_msg *msg = _msg;		/* [한국어] void* → control_msg* 캐스트 */
	struct spdk_nvmf_tcp_req *tcp_req;
	struct spdk_nvmf_tcp_transport *ttransport;

	assert(list);	/* [한국어] list NULL은 논리 오류 */
	STAILQ_INSERT_HEAD(&list->free_msgs, msg, link);
	/* [한국어] 버퍼를 free 큐 선두에 반환 (LIFO: 가장 최근 반환 버퍼가 다음에 사용됨) */
	if (!STAILQ_EMPTY(&list->waiting_for_msg_reqs)) {
		/* [한국어] 대기 중인 req 있음 — 첫 번째 req 꺼내 처리 재개 */
		tcp_req = STAILQ_FIRST(&list->waiting_for_msg_reqs);
		STAILQ_REMOVE_HEAD(&list->waiting_for_msg_reqs, control_msg_link);
		/* [한국어] 대기 큐에서 제거 */
		ttransport = SPDK_CONTAINEROF(tcp_req->req.qpair->transport,
					      struct spdk_nvmf_tcp_transport, transport);
		/* [한국어] req의 qpair → transport → TCP transport 다운캐스트 */
		nvmf_tcp_req_process(ttransport, tcp_req);
		/* [한국어] 이 req가 이제 control_msg를 얻을 수 있으므로 처리 재개 */
	}
}

/*
 * [한국어]
 * nvmf_tcp_req_parse_sgl - NVMe 명령의 SGL(Scatter-Gather List) 디스크립터를 파싱해 req 버퍼 결정
 *
 * @tcp_req:   파싱할 NVMe-oF TCP 요청 (req.cmd.nvme_cmd.dptr.sgl1 참조)
 * @transport: 이 qpair가 속한 transport (max_io_size, in_capsule_data_size 확인용)
 * @group:     이 qpair의 poll_group (control_msg_list 및 buffer pool 접근용)
 * @return:    없음 (void). 성공 시 TCP_REQUEST_STATE_HAVE_BUFFER로 전이, 실패 시 fatal_err.
 *
 * NVMe-TCP의 SGL 디스크립터 타입에 따라 세 가지 경로로 분기:
 *
 * [경로 1] TRANSPORT_DATA_BLOCK / SUBTYPE_TRANSPORT (일반 I/O):
 *   - sgl.unkeyed.length가 데이터 크기
 *   - zcopy 가능이면 data_from_pool=false, HAVE_BUFFER로 즉시 전이
 *   - 아니면 대형 버퍼 풀(spdk_nvmf_request_get_buffers)에서 버퍼 할당
 *   - 버퍼 없으면 NEED_BUFFER 상태에서 대기 (get_buffers_done 콜백에서 재개)
 *
 * [경로 2] DATA_BLOCK / SUBTYPE_OFFSET (in-capsule 데이터):
 *   - CapsuleCmd PDU 내에 데이터가 포함된 경우 (ICD: In-Capsule Data)
 *   - 데이터 길이는 pdu->hdr.common.plen에서 계산 (SGL 필드와 일치 검증)
 *   - length <= in_capsule_data_size: tcp_req->buf (per-qpair 사전할당 버퍼) 사용
 *   - length > in_capsule_data_size 이고 <= 8192 이고 admin/fabric 명령:
 *       control_msg_list에서 8192바이트 버퍼 할당; 없으면 AWAIT_PDU_BUF 대기
 *   - 그 외: DATA_TRANSFER_LIMIT_EXCEEDED fatal error
 *   - ICDOFF는 항상 0이어야 함 (NVMe-TCP 스펙 요구사항)
 *
 * [경로 3] 기타 SGL 타입:
 *   - Fatal error: C2HTermReq 송신
 *
 * 실행 컨텍스트: poll group 스레드 (nvmf_tcp_req_process의 NEED_BUFFER 케이스에서 호출)
 *
 * 호출 체인:
 *   nvmf_tcp_req_process(TCP_REQUEST_STATE_NEED_BUFFER) → [nvmf_tcp_req_parse_sgl]
 *     → spdk_nvmf_request_get_buffers (경로 1, 비zcopy)
 *     → nvmf_tcp_control_msg_get (경로 2, 대형 ICD)
 *     → nvmf_tcp_send_c2h_term_req (경로 3, 오류)
 */
static void
nvmf_tcp_req_parse_sgl(struct spdk_nvmf_tcp_req *tcp_req,
		       struct spdk_nvmf_transport *transport,
		       struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_request		*req = &tcp_req->req;	/* [한국어] 공통 nvmf_request 래퍼 */
	struct spdk_nvme_cmd			*cmd;
	struct spdk_nvme_sgl_descriptor		*sgl;		/* [한국어] NVMe 명령의 SGL 디스크립터 포인터 */
	struct spdk_nvmf_tcp_poll_group		*tgroup;	/* [한국어] poll_group TCP 다운캐스트 (control_msg_list 접근) */
	enum spdk_nvme_tcp_term_req_fes		fes;		/* [한국어] 오류 시 TermReq에 실을 FES 코드 */
	struct nvme_tcp_pdu			*pdu;		/* [한국어] 수신 중인 PDU (ICD 길이 계산용) */
	struct spdk_nvmf_tcp_qpair		*tqpair;	/* [한국어] ICD 경로에서 tqpair 참조용 */
	uint32_t				length, error_offset = 0;

	cmd = &req->cmd->nvme_cmd;	/* [한국어] NVMe 명령 헤더 (opcode, SGL 포함) */
	sgl = &cmd->dptr.sgl1;		/* [한국어] SGL 디스크립터 1번 (NVMe-TCP는 SGL1만 사용) */

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK &&
	    sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_TRANSPORT) {
		/* [한국어] 경로 1: Transport Data Block SGL — 일반 I/O (write/read).
		 * 버퍼는 transport buffer pool에서 할당하거나 zcopy를 사용한다. */

		/* get request length from sgl */
		length = sgl->unkeyed.length;	/* [한국어] SGL에서 직접 데이터 전송 길이 추출 */
		if (spdk_unlikely(length > transport->opts.max_io_size)) {
			/* [한국어] 요청 크기가 max_io_size 초과 — 데이터 전송 한계 초과 오류 */
			SPDK_ERRLOG("SGL length 0x%x exceeds max io size 0x%x\n",
				    length, transport->opts.max_io_size);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED;
			goto fatal_err;
		}

		/* fill request length and populate iovs */
		req->length = length;	/* [한국어] nvmf_request에 데이터 길이 기록 */

		SPDK_DEBUGLOG(nvmf_tcp, "Data requested length= 0x%x\n", length);

		if (spdk_unlikely(req->dif_enabled)) {
			/* [한국어] DIF(Data Integrity Field) 활성화 시 메타데이터 포함 길이 계산.
			 * orig_length: 사용자 데이터 길이, elba_length: DIF 포함 실제 버퍼 크기 */
			req->dif.orig_length = length;
			length = spdk_dif_get_length_with_md(length, &req->dif.dif_ctx);
			/* [한국어] DIF 컨텍스트 기반으로 메타데이터 포함 전체 길이 계산 */
			req->dif.elba_length = length;
		}

		if (nvmf_ctrlr_use_zcopy(req)) {
			/* [한국어] zcopy 가능: bdev가 직접 버퍼를 제공하므로 transport 버퍼 불필요.
			 * zcopy_start 시점에 bdev에서 버퍼를 얻어 사용. */
			SPDK_DEBUGLOG(nvmf_tcp, "Using zero-copy to execute request %p\n", tcp_req);
			req->data_from_pool = false;	/* [한국어] transport buffer pool 미사용 표시 */
			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_HAVE_BUFFER);
			/* [한국어] zcopy 경로는 버퍼 없이도 HAVE_BUFFER로 전이 (bdev가 버퍼 제공) */
			return;
		}

		if (spdk_nvmf_request_get_buffers(req, group, transport, length)) {
			/* No available buffers. Queue this request up. */
			/* [한국어] transport buffer pool에서 버퍼 할당 실패 — NEED_BUFFER 상태 유지.
			 * nvmf_tcp_req_get_buffers_done 콜백에서 버퍼 확보 시 재개됨. */
			SPDK_DEBUGLOG(nvmf_tcp, "No available large data buffers. Queueing request %p\n",
				      tcp_req);
			return;
		}

		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_HAVE_BUFFER);
		/* [한국어] 버퍼 할당 성공 — HAVE_BUFFER로 전이 */
		SPDK_DEBUGLOG(nvmf_tcp, "Request %p took %d buffer/s from central pool, and data=%p\n",
			      tcp_req, req->iovcnt, req->iov[0].iov_base);

		return;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		/* [한국어] 경로 2: In-Capsule Data (ICD) — CapsuleCmd PDU 내에 데이터 포함.
		 * 주로 admin 명령(identify, set-features 등)이나 fabric 명령(connect 등)에 사용.
		 * SUBTYPE_OFFSET은 NVMe-TCP 전용: offset 필드는 항상 0이어야 한다. */
		uint64_t offset = sgl->address;		/* [한국어] NVMe-TCP에서 항상 0이어야 하는 오프셋 */
		uint32_t max_len = transport->opts.in_capsule_data_size;
		/* [한국어] 일반 in-capsule 허용 최대 크기 (기본 4096바이트) */

		assert(tcp_req->has_in_capsule_data);
		/* [한국어] CapsuleCmd PSH 처리 시 has_in_capsule_data=true로 설정됨 확인 */
		/* Capsule Cmd with In-capsule Data should get data length from pdu header */
		tqpair = tcp_req->pdu->qpair;	/* [한국어] tcp_req가 소속된 qpair */
		/* receiving pdu is not same with the pdu in tcp_req */
		pdu = tqpair->pdu_in_progress;
		/* [한국어] 수신 중인 PDU (tcp_req->pdu와 다를 수 있으므로 pdu_in_progress 사용) */
		length = pdu->hdr.common.plen - pdu->psh_len - sizeof(struct spdk_nvme_tcp_common_pdu_hdr);
		/* [한국어] ICD 길이 = 전체 PDU 길이 - PSH 길이 - 공통 헤더(8바이트).
		 * plen: PDU 전체 바이트 수 (CH + PSH + DDgst + Data)
		 * psh_len: PDU 타입별 특정 헤더 길이 + (HDgst 있으면 4바이트) */
		if (tqpair->host_ddgst_enable) {
			length -= SPDK_NVME_TCP_DIGEST_LEN;
			/* [한국어] DDgst(Data Digest, 4바이트 CRC32C) 활성화 시 payload에서 제외 */
		}
		/* This error is not defined in NVMe/TCP spec, take this error as fatal error */
		if (spdk_unlikely(length != sgl->unkeyed.length)) {
			/* [한국어] PDU 헤더에서 계산한 ICD 길이와 SGL에 명시된 길이 불일치 — fatal error */
			SPDK_ERRLOG("In-Capsule Data length 0x%x is not equal to SGL data length 0x%x\n",
				    length, sgl->unkeyed.length);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, plen);
			/* [한국어] FEI = plen 필드 오프셋 (PDU 길이 필드가 잘못됨) */
			goto fatal_err;
		}

		SPDK_DEBUGLOG(nvmf_tcp, "In-capsule data: offset 0x%" PRIx64 ", length 0x%x\n",
			      offset, length);

		/* The NVMe/TCP transport does not use ICDOFF to control the in-capsule data offset. ICDOFF should be '0' */
		if (spdk_unlikely(offset != 0)) {
			/* Not defined fatal error in NVMe/TCP spec, handle this error as a fatal error */
			/* [한국어] NVMe-TCP는 ICDOFF(In-Capsule Data Offset)을 사용하지 않음.
			 * 스펙상 address 필드는 항상 0이어야 한다. 0이 아니면 fatal error. */
			SPDK_ERRLOG("In-capsule offset 0x%" PRIx64 " should be ZERO in NVMe/TCP\n", offset);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER;
			error_offset = offsetof(struct spdk_nvme_tcp_cmd, ccsqe.dptr.sgl1.address);
			/* [한국어] FEI = SGL1.address 필드 오프셋 */
			goto fatal_err;
		}

		if (spdk_unlikely(length > max_len)) {
			/* According to the SPEC we should support ICD up to 8192 bytes for admin and fabric commands */
			/* [한국어] ICD 길이가 일반 in_capsule_data_size를 초과.
			 * NVMe-TCP 스펙: admin/fabric 명령은 최대 8192바이트(SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE)까지 허용. */
			if (length <= SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE &&
			    (cmd->opc == SPDK_NVME_OPC_FABRIC || req->qpair->qid == 0)) {
				/* [한국어] admin(qid==0) 또는 fabric 명령이고 8192 이하:
				 * control_msg_list의 전용 8192바이트 버퍼에서 할당 */

				/* Get a buffer from dedicated list */
				SPDK_DEBUGLOG(nvmf_tcp, "Getting a buffer from control msg list\n");
				tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
				/* [한국어] poll_group → tcp_poll_group 다운캐스트 */
				assert(tgroup->control_msg_list);
				req->iov[0].iov_base = nvmf_tcp_control_msg_get(tgroup->control_msg_list, tcp_req);
				/* [한국어] control_msg_list에서 8192바이트 버퍼 할당 시도 */
				if (!req->iov[0].iov_base) {
					/* No available buffers. Queue this request up. */
					/* [한국어] 버퍼 없음 — req를 waiting_for_msg_reqs에 삽입하고
					 * 수신 상태를 AWAIT_PDU_BUF로 전이 (버퍼 확보까지 PDU 수신 중단) */
					SPDK_DEBUGLOG(nvmf_tcp, "No available ICD buffers. Queueing request %p\n", tcp_req);
					nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_BUF);
					return;
				}
			} else {
				/* [한국어] I/O 명령(qid != 0)이거나 8192 초과: 데이터 전송 한계 초과 오류 */
				SPDK_ERRLOG("In-capsule data length 0x%x exceeds capsule length 0x%x\n",
					    length, max_len);
				fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED;
				goto fatal_err;
			}
		} else {
			req->iov[0].iov_base = tcp_req->buf;
			/* [한국어] 일반 ICD 크기: per-qpair 사전할당 버퍼(tcp_req->buf) 직접 사용 */
		}

		req->length = length;		/* [한국어] 실제 I/O 데이터 길이 기록 */
		req->data_from_pool = false;	/* [한국어] transport buffer pool 미사용 (ICD는 전용 버퍼 사용) */

		if (spdk_unlikely(req->dif_enabled)) {
			/* [한국어] DIF 활성화 시: 메타데이터 포함 길이로 elba_length 조정 */
			length = spdk_dif_get_length_with_md(length, &req->dif.dif_ctx);
			req->dif.elba_length = length;
		}

		req->iov[0].iov_len = length;	/* [한국어] iov 벡터 길이 설정 */
		req->iovcnt = 1;		/* [한국어] ICD는 항상 단일 iov */
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_HAVE_BUFFER);
		/* [한국어] 버퍼 준비 완료 — HAVE_BUFFER로 전이 */

		return;
	}
	/* If we want to handle the problem here, then we can't skip the following data segment.
	 * Because this function runs before reading data part, now handle all errors as fatal errors. */
	/* [한국어] 경로 3: 인식할 수 없는 SGL 타입 — fatal error.
	 * 이미 PDU 수신 중이므로 데이터 부분을 건너뛸 수 없어 항상 fatal error로 처리. */
	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER;
	error_offset = offsetof(struct spdk_nvme_tcp_cmd, ccsqe.dptr.sgl1.generic);
	/* [한국어] FEI = SGL1.generic 필드 오프셋 */
fatal_err:
	nvmf_tcp_send_c2h_term_req(tcp_req->pdu->qpair, tcp_req->pdu, fes, error_offset);
	/* [한국어] fatal 오류: C2HTermReq 송신 후 QUIESCING → QUIESCED → destroy */
}

/*
 * [한국어]
 * nvmf_tcp_dif_error_to_compl_status - DIF 오류 타입을 NVMe 완료 상태 코드로 변환
 *
 * @err_type: SPDK DIF 오류 타입 (SPDK_DIF_REFTAG_ERROR / APPTAG_ERROR / GUARD_ERROR)
 * @return:   대응하는 NVMe 미디어 오류 상태 코드 (SCT=MEDIA_ERROR_STATUS 범주)
 *
 * DIF(Data Integrity Field, T10 PI: Protection Information)는 각 데이터 블록에
 * 8바이트 보호 정보(Guard, AppTag, RefTag)를 추가해 데이터 무결성을 보장한다.
 * SPDK accel/DIF 라이브러리가 검사 결과로 반환한 err_type을 NVMe 스펙의
 * 완료 큐 상태 코드(SC)로 변환해 호스트에게 보고하기 위한 변환 함수이다.
 *
 * 변환 대응:
 *   SPDK_DIF_REFTAG_ERROR → SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR (0x284)
 *   SPDK_DIF_APPTAG_ERROR → SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR (0x285)
 *   SPDK_DIF_GUARD_ERROR  → SPDK_NVME_SC_GUARD_CHECK_ERROR (0x281)
 *
 * 호출 체인:
 *   _nvmf_tcp_send_c2h_data (DIF 검증 실패 시) → [nvmf_tcp_dif_error_to_compl_status]
 *     → nvmf_tcp_req_set_cpl
 */
static inline enum spdk_nvme_media_error_status_code
nvmf_tcp_dif_error_to_compl_status(uint8_t err_type) {
	enum spdk_nvme_media_error_status_code result;

	switch (err_type)
	{
	case SPDK_DIF_REFTAG_ERROR:
		/* [한국어] 참조 태그(LBA 번호 기반) 불일치 */
		result = SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_APPTAG_ERROR:
		/* [한국어] 애플리케이션 태그 불일치 (호스트 설정 값과 불일치) */
		result = SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_GUARD_ERROR:
		/* [한국어] Guard(CRC16) 불일치 — 데이터 손상 */
		result = SPDK_NVME_SC_GUARD_CHECK_ERROR;
		break;
	default:
		SPDK_UNREACHABLE();	/* [한국어] 정의되지 않은 DIF 오류 타입 — 도달 불가 */
		break;
	}

	return result;	/* [한국어] NVMe SC 코드 반환 — 호출자가 CQE에 기록 */
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

/*
 * [한국어]
 * nvmf_tcp_send_c2h_data - C2H Data PDU 송신 진입점 (req PDU 초기화 후 _nvmf_tcp_send_c2h_data 호출)
 *
 * @tqpair:  데이터를 송신할 TCP qpair
 * @tcp_req: 전송할 read 요청
 *
 * _nvmf_tcp_send_c2h_data를 호출하기 전에 req PDU 슬롯을 초기화(nvmf_tcp_req_pdu_init)한다.
 * 이는 request_transfer_out에서 처음 C2H 송신을 시작할 때의 표준 진입점이다.
 * 이후 부분 전송의 경우 nvmf_tcp_pdu_c2h_data_complete에서 _nvmf_tcp_send_c2h_data를
 * 직접 호출(PDU 재초기화 없이)한다.
 *
 * 호출 체인:
 *   request_transfer_out → [nvmf_tcp_send_c2h_data] → nvmf_tcp_req_pdu_init
 *                                                     → _nvmf_tcp_send_c2h_data
 */
static void
nvmf_tcp_send_c2h_data(struct spdk_nvmf_tcp_qpair *tqpair,
		       struct spdk_nvmf_tcp_req *tcp_req)
{
	nvmf_tcp_req_pdu_init(tcp_req);		/* [한국어] req의 응답 PDU 슬롯 초기화 (rsp_pdu 셋업) */
	_nvmf_tcp_send_c2h_data(tqpair, tcp_req);	/* [한국어] C2H Data PDU 구성 및 비동기 송신 시작 */
}

/*
 * [한국어]
 * request_transfer_out - NVMe 명령 실행 완료 후 응답 데이터/상태를 호스트로 전송
 *
 * @req:    완료된 NVMe-oF 요청 (req->rsp에 응답 상태 기록됨)
 * @return: 0 (항상 성공. 실패는 내부 콜백에서 처리)
 *
 * spdk_nvmf_request_exec 완료 후 nvmf common layer가 호출하는 transport 콜백.
 * sq_head를 증가시켜 CQE에 기록한 후, 명령 성공+read이면 C2H Data PDU를 송신하고
 * 그 외(오류 또는 write/admin)이면 CapsuleResp PDU를 송신한다.
 * TRANSFERRING_CONTROLLER_TO_HOST 상태로 전이한 후 실제 PDU 송신을 트리거.
 *
 * 실행 컨텍스트: poll group 스레드 (nvmf_tcp_req_process의 EXECUTED 케이스에서 호출)
 *
 * 호출 체인:
 *   nvmf_tcp_req_process(EXECUTED) → [request_transfer_out]
 *     → nvmf_tcp_send_c2h_data (read 성공)
 *     → nvmf_tcp_send_capsule_resp_pdu (write/admin/오류)
 */
static int
request_transfer_out(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_req	*tcp_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	struct spdk_nvme_cpl		*rsp;

	SPDK_DEBUGLOG(nvmf_tcp, "enter\n");

	qpair = req->qpair;		/* [한국어] 이 요청이 속한 qpair */
	rsp = &req->rsp->nvme_cpl;	/* [한국어] NVMe 완료 큐 엔트리(CQE) 포인터 */
	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_req, req);
	/* [한국어] 공통 nvmf_request → tcp_req 다운캐스트 */

	/* Advance our sq_head pointer */
	if (qpair->sq_head == qpair->sq_head_max) {
		/* [한국어] sq_head가 최대에 도달하면 0으로 랩어라운드 (modular 증가) */
		qpair->sq_head = 0;
	} else {
		qpair->sq_head++;
		/* [한국어] SQ(Submission Queue) 헤드 포인터 1 증가 — 처리한 명령 수 추적 */
	}
	rsp->sqhd = qpair->sq_head;
	/* [한국어] CQE의 sqhd 필드에 현재 sq_head 기록.
	 * 호스트는 이 값으로 타깃이 SQ의 어디까지 처리했는지 알 수 있음. */

	tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);
	/* [한국어] qpair → TCP qpair 다운캐스트 */
	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	/* [한국어] 상태 전이: EXECUTED → TRANSFERRING_CONTROLLER_TO_HOST */
	if (spdk_nvme_cpl_is_success(rsp) && req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		/* [한국어] 성공한 read 명령: 데이터를 C2H Data PDU로 전송 */
		nvmf_tcp_send_c2h_data(tqpair, tcp_req);
	} else {
		/* [한국어] 오류이거나 write/admin 명령: 데이터 없이 CapsuleResp PDU로 완료 상태만 전송 */
		nvmf_tcp_send_capsule_resp_pdu(tcp_req, tqpair);
	}

	return 0;	/* [한국어] 항상 0 반환 (실제 완료는 PDU 송신 콜백에서 처리) */
}

/*
 * [한국어]
 * nvmf_tcp_check_fused_ordering - fused 명령(Compare-and-Write 등) 페어링 검증
 *
 * @ttransport: TCP transport 컨텍스트
 * @tqpair:     명령을 수신한 qpair
 * @tcp_req:    새로 수신된 요청 (FIRST 또는 SECOND 또는 일반 명령)
 *
 * NVMe 스펙의 Fused Operation: FIRST 명령과 SECOND 명령이 반드시 연속으로 도착해야 하며,
 * 두 명령이 짝이 맞을 때만 함께 실행된다 (예: Compare-and-Write).
 * tqpair->fused_first로 이전 FIRST 명령을 추적하며, 새 req가 SECOND이면 페어링 완료.
 * FIRST 다음에 SECOND가 아닌 명령이 오거나 SECOND 앞에 FIRST가 없으면 오류 처리.
 *
 * 실행 컨텍스트: poll group 스레드 (nvmf_tcp_req_process의 NEW 케이스에서 호출)
 *
 * 호출 체인:
 *   nvmf_tcp_req_process(NEW) → [nvmf_tcp_check_fused_ordering]
 *     → nvmf_tcp_req_process(fused_first, READY_TO_EXECUTE 시 즉시 실행)
 */
static void
nvmf_tcp_check_fused_ordering(struct spdk_nvmf_tcp_transport *ttransport,
			      struct spdk_nvmf_tcp_qpair *tqpair,
			      struct spdk_nvmf_tcp_req *tcp_req)
{
	enum spdk_nvme_cmd_fuse last, next;

	last = tqpair->fused_first ? tqpair->fused_first->cmd.fuse : SPDK_NVME_CMD_FUSE_NONE;
	/* [한국어] 이전 FIRST req의 fuse 필드 (없으면 NONE) */
	next = tcp_req->cmd.fuse;	/* [한국어] 현재 req의 fuse 필드 */

	assert(last != SPDK_NVME_CMD_FUSE_SECOND);
	/* [한국어] fused_first에는 FIRST 명령만 저장돼야 함 — 불변식 검증 */

	if (spdk_likely(last == SPDK_NVME_CMD_FUSE_NONE && next == SPDK_NVME_CMD_FUSE_NONE)) {
		/* [한국어] 가장 흔한 경우: 일반 명령 — fused 처리 불필요, 즉시 반환 */
		return;
	}

	if (last == SPDK_NVME_CMD_FUSE_FIRST) {
		if (next == SPDK_NVME_CMD_FUSE_SECOND) {
			/* This is a valid pair of fused commands.  Point them at each other
			 * so they can be submitted consecutively once ready to be executed.
			 */
			/* [한국어] 유효한 FIRST+SECOND 페어 — 서로를 fused_pair로 연결 */
			tqpair->fused_first->fused_pair = tcp_req;
			tcp_req->fused_pair = tqpair->fused_first;
			tqpair->fused_first = NULL;	/* [한국어] 페어링 완료, FIRST 추적 해제 */
			return;
		} else {
			/* Mark the last req as failed since it wasn't followed by a SECOND. */
			/* [한국어] FIRST 다음에 SECOND가 아닌 명령 도착 — FIRST를 실패로 처리 */
			tqpair->fused_first->fused_failed = true;

			/*
			 * If the last req is in READY_TO_EXECUTE state, then call
			 * nvmf_tcp_req_process(), otherwise nothing else will kick it.
			 */
			/* [한국어] 이미 READY_TO_EXECUTE 상태이면 상태 머신을 수동으로 진행
			 * (fused 오류로 즉시 실패 완료를 위해) */
			if (tqpair->fused_first->state == TCP_REQUEST_STATE_READY_TO_EXECUTE) {
				nvmf_tcp_req_process(ttransport, tqpair->fused_first);
			}

			tqpair->fused_first = NULL;	/* [한국어] FIRST 추적 해제 */
		}
	}

	if (next == SPDK_NVME_CMD_FUSE_FIRST) {
		/* Set tqpair->fused_first here so that we know to check that the next request
		 * is a SECOND (and to fail this one if it isn't).
		 */
		/* [한국어] 새 FIRST 명령 수신 — 다음 req가 SECOND인지 확인하기 위해 추적 */
		tqpair->fused_first = tcp_req;
	} else if (next == SPDK_NVME_CMD_FUSE_SECOND) {
		/* Mark this req failed since it is a SECOND and the last one was not a FIRST. */
		/* [한국어] SECOND이지만 앞에 FIRST가 없음 — 이 req를 실패로 마킹 */
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
	/* [한국어] nvmf_qpair → tcp_qpair 다운캐스트 */
	group = &tqpair->group->group;	/* [한국어] 이 qpair가 속한 poll_group의 공통 group 포인터 */
	assert(tcp_req->state != TCP_REQUEST_STATE_FREE);
	/* [한국어] FREE 상태의 req는 이 함수로 진입하면 안 됨 — 논리 오류 방지 */

	/* If the qpair is not active, we need to abort the outstanding requests. */
	if (!spdk_nvmf_qpair_is_active(&tqpair->qpair)) {
		/* [한국어] qpair가 비활성(disconnecting/disconnected) 상태.
		 * 진행 중인 모든 req를 즉시 COMPLETED로 전이시켜 cleanup 처리. */
		if (tcp_req->state == TCP_REQUEST_STATE_NEED_BUFFER) {
			nvmf_tcp_request_get_buffers_abort(tcp_req);
			/* [한국어] 버퍼 요청 대기 큐에서 제거 (버퍼 콜백이 오지 않도록) */
		}
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_COMPLETED);
		/* [한국어] 강제로 COMPLETED로 전이 — 아래 switch에서 req_put으로 정리됨 */
	}

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = tcp_req->state;	/* [한국어] 이번 이터레이션 시작 상태 기록 (진행 여부 판단용) */

		SPDK_DEBUGLOG(nvmf_tcp, "Request %p entering state %d on tqpair=%p\n", tcp_req, prev_state,
			      tqpair);

		switch (tcp_req->state) {
		case TCP_REQUEST_STATE_FREE:
			/* Some external code must kick a request into TCP_REQUEST_STATE_NEW
			 * to escape this state. */
			/* [한국어] FREE 상태: 외부 코드(capsule_cmd_payload_handle)가 NEW로 전이시켜야만 탈출.
			 * 이 case는 직접 도달 불가 (assert로 걸러짐). */
			break;
		case TCP_REQUEST_STATE_NEW:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_NEW, tqpair->qpair.trace_id, 0, (uintptr_t)tcp_req,
					  tqpair->qpair.queue_depth);
			/* [한국어] SPDK trace: req lifecycle 추적용 타임스탬프 기록 */

			/* copy the cmd from the receive pdu */
			tcp_req->cmd = tqpair->pdu_in_progress->hdr.capsule_cmd.ccsqe;
			/* [한국어] CapsuleCmd PDU의 NVMe Submission Queue Entry(SQE)를 req에 복사.
			 * 이후 PDU 수신이 다음 PDU로 진행돼도 cmd 내용이 유지됨. */

			if (spdk_unlikely(spdk_nvmf_request_get_dif_ctx(&tcp_req->req, &tcp_req->req.dif.dif_ctx))) {
				/* [한국어] DIF(Data Integrity Field, T10 PI) 활성화 요청:
				 * dif_ctx에 DIF 파라미터 초기화하고 pdu에도 참조 설정 */
				tcp_req->req.dif_enabled = true;
				tqpair->pdu_in_progress->dif_ctx = &tcp_req->req.dif.dif_ctx;
				/* [한국어] 수신 PDU에 dif_ctx 연결 — payload 수신 시 DIF 검증에 사용 */
			}

			nvmf_tcp_check_fused_ordering(ttransport, tqpair, tcp_req);
			/* [한국어] fused 명령(FIRST/SECOND) 순서 검증 및 페어링 처리 */

			/* The next state transition depends on the data transfer needs of this request. */
			tcp_req->req.xfer = spdk_nvmf_req_get_xfer(&tcp_req->req);
			/* [한국어] NVMe 명령 opcode에서 데이터 전송 방향 결정:
			 * NONE(no data), HOST_TO_CONTROLLER(write), CONTROLLER_TO_HOST(read), BIDIRECTIONAL */

			if (spdk_unlikely(tcp_req->req.xfer == SPDK_NVME_DATA_BIDIRECTIONAL)) {
				/* [한국어] 양방향 전송: NVMe-TCP에서 미지원 — INVALID_OPCODE로 즉시 실패 */
				nvmf_tcp_req_set_cpl(tcp_req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_INVALID_OPCODE);
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
				/* [한국어] 수신 상태 머신을 READY로 리셋 (다음 PDU 수신 준비) */
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
				SPDK_DEBUGLOG(nvmf_tcp, "Request %p: invalid xfer type (BIDIRECTIONAL)\n", tcp_req);
				break;
			}

			/* If no data to transfer, ready to execute. */
			if (tcp_req->req.xfer == SPDK_NVME_DATA_NONE) {
				/* Reset the tqpair receiving pdu state */
				/* [한국어] 데이터 없음(admin 명령 등): 즉시 READY_TO_EXECUTE로 전이.
				 * 수신 상태를 READY로 리셋하여 다음 PDU 바로 수신 가능하게 함. */
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
				break;
			}

			pdu = tqpair->pdu_in_progress;		/* [한국어] 현재 CapsuleCmd PDU 참조 */
			plen = pdu->hdr.common.hlen;		/* [한국어] PDU 헤더 길이 */
			if (tqpair->host_hdgst_enable) {
				plen += SPDK_NVME_TCP_DIGEST_LEN;	/* [한국어] HDgst(4바이트) 포함 */
			}
			if (pdu->hdr.common.plen != plen) {
				/* [한국어] plen > hlen+hdgst: PDU에 in-capsule 데이터 포함됨.
				 * has_in_capsule_data=true로 표시 — HAVE_BUFFER에서 payload 수신 경로 선택. */
				tcp_req->has_in_capsule_data = true;
			} else {
				/* Data is transmitted by C2H PDUs */
				/* [한국어] plen == hlen+hdgst: 데이터 없음(in-capsule 없음).
				 * write 명령은 R2T 이후 H2CData PDU로 데이터 수신.
				 * 수신 상태를 READY로 리셋하여 다음 PDU 수신 시작. */
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
			}

			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_NEED_BUFFER);
			/* [한국어] 상태 전이: NEW → NEED_BUFFER (버퍼 할당 단계) */
			break;
		case TCP_REQUEST_STATE_NEED_BUFFER:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_NEED_BUFFER, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);

			assert(tcp_req->req.xfer != SPDK_NVME_DATA_NONE);
			/* [한국어] NEED_BUFFER는 데이터 전송이 필요한 경우에만 도달 */

			/* Try to get a data buffer */
			nvmf_tcp_req_parse_sgl(tcp_req, transport, group);
			/* [한국어] SGL 파싱 + 버퍼 할당:
			 * 성공 시 HAVE_BUFFER 전이, 버퍼 없으면 이 상태 유지(나중에 콜백으로 재시도),
			 * ICD 초과 시 AWAIT_PDU_BUF로 전이 */
			break;
		case TCP_REQUEST_STATE_HAVE_BUFFER:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_HAVE_BUFFER, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			/* Get a zcopy buffer if the request can be serviced through zcopy */
			if (spdk_nvmf_request_using_zcopy(&tcp_req->req)) {
				/* [한국어] zcopy 가능: bdev에서 직접 버퍼를 제공받아 zero-copy로 처리.
				 * AWAITING_ZCOPY_START로 전이 후 zcopy_start 완료 콜백에서 재개. */
				if (spdk_unlikely(tcp_req->req.dif_enabled)) {
					/* [한국어] DIF 활성화: elba_length(DIF 포함 크기)로 req.length 갱신 */
					assert(tcp_req->req.dif.elba_length >= tcp_req->req.length);
					tcp_req->req.length = tcp_req->req.dif.elba_length;
				}

				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_AWAITING_ZCOPY_START);
				spdk_nvmf_request_zcopy_start(&tcp_req->req);
				/* [한국어] bdev에 zcopy 버퍼 제공 요청 — 완료 시 ZCOPY_START_COMPLETED 전이 */
				break;
			}

			assert(tcp_req->req.iovcnt > 0);
			/* [한국어] 비-zcopy 경로: iov 배열에 유효한 버퍼가 있어야 함 */

			/* If data is transferring from host to controller, we need to do a transfer from the host. */
			if (tcp_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
				/* [한국어] write 명령 (H2C): 호스트로부터 데이터 수신이 필요 */
				if (tcp_req->req.data_from_pool) {
					/* [한국어] transport buffer pool에서 할당된 버퍼:
					 * R2T PDU를 송신하여 호스트에게 데이터 전송 시작 알림.
					 * AWAITING_R2T_ACK 상태로 전이됨 (nvmf_tcp_send_r2t_pdu 내에서). */
					SPDK_DEBUGLOG(nvmf_tcp, "Sending R2T for tcp_req(%p) on tqpair=%p\n", tcp_req, tqpair);
					nvmf_tcp_send_r2t_pdu(tqpair, tcp_req);
				} else {
					/* [한국어] in-capsule 데이터: CapsuleCmd PDU 내에 이미 데이터가 포함됨.
					 * R2T 불필요 — PDU payload 수신 경로로 직접 전환. */
					struct nvme_tcp_pdu *pdu;

					nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
					/* [한국어] 상태 전이: HAVE_BUFFER → TRANSFERRING_HOST_TO_CONTROLLER */

					pdu = tqpair->pdu_in_progress;
					/* [한국어] 현재 수신 중인 CapsuleCmd PDU — in-capsule 데이터 포함 */
					SPDK_DEBUGLOG(nvmf_tcp, "Not need to send r2t for tcp_req(%p) on tqpair=%p\n", tcp_req,
						      tqpair);
					/* No need to send r2t, contained in the capsuled data */
					nvme_tcp_pdu_set_data_buf(pdu, tcp_req->req.iov, tcp_req->req.iovcnt,
								  0, tcp_req->req.length);
					/* [한국어] PDU의 데이터 버퍼를 req.iov로 설정 (데이터 수신 위치 지정) */
					nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
					/* [한국어] 상태 머신 전이: AWAIT_PDU_PAYLOAD (실제 데이터 바이트 수신) */
				}
				break;
			}

			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
			/* [한국어] read 명령(C2H): 버퍼 준비 완료 → 즉시 READY_TO_EXECUTE로 전이 */
			break;
		case TCP_REQUEST_STATE_AWAITING_ZCOPY_START:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_START, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			/* Some external code must kick a request into  TCP_REQUEST_STATE_ZCOPY_START_COMPLETED
			 * to escape this state. */
			/* [한국어] bdev zcopy_start 완료를 기다리는 상태.
			 * bdev 콜백이 ZCOPY_START_COMPLETED로 전이시키면 다음 이터레이션에서 진행. */
			break;
		case TCP_REQUEST_STATE_ZCOPY_START_COMPLETED:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_ZCOPY_START_COMPLETED, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			if (spdk_unlikely(spdk_nvme_cpl_is_error(&tcp_req->req.rsp->nvme_cpl))) {
				/* [한국어] zcopy_start 실패 (bdev 오류): 데이터 전송 없이 오류 응답 */
				SPDK_DEBUGLOG(nvmf_tcp, "Zero-copy start failed for tcp_req(%p) on tqpair=%p\n",
					      tcp_req, tqpair);
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
				break;
			}
			if (tcp_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
				/* [한국어] zcopy write: bdev 버퍼 확보 완료 → R2T 송신으로 호스트에게 데이터 요청 */
				SPDK_DEBUGLOG(nvmf_tcp, "Sending R2T for tcp_req(%p) on tqpair=%p\n", tcp_req, tqpair);
				nvmf_tcp_send_r2t_pdu(tqpair, tcp_req);
			} else {
				/* [한국어] zcopy read: bdev 버퍼가 이미 데이터를 갖고 있으므로 즉시 EXECUTED */
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_EXECUTED);
			}
			break;
		case TCP_REQUEST_STATE_AWAITING_R2T_ACK:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_AWAIT_R2T_ACK, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			/* The R2T completion or the h2c data incoming will kick it out of this state. */
			/* [한국어] R2T 송신 완료(nvmf_tcp_r2t_complete) 또는 H2CData 수신이
			 * TRANSFERRING_HOST_TO_CONTROLLER로 전이시킬 때까지 대기. */
			break;
		case TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:

			spdk_trace_record(TRACE_TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER, tqpair->qpair.trace_id,
					  0, (uintptr_t)tcp_req);
			/* Some external code must kick a request into TCP_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			/* [한국어] 호스트에서 데이터 수신 중. H2CData PDU 수신 완료(h2c_data_payload_handle)가
			 * rw_offset == length 이면 READY_TO_EXECUTE로 전이. */
			break;
		case TCP_REQUEST_STATE_READY_TO_EXECUTE:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_READY_TO_EXECUTE, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);

			if (spdk_unlikely(tcp_req->req.dif_enabled)) {
				/* [한국어] DIF 활성화: bdev 실행에는 DIF 포함 전체 길이(elba_length) 사용 */
				assert(tcp_req->req.dif.elba_length >= tcp_req->req.length);
				tcp_req->req.length = tcp_req->req.dif.elba_length;
			}

			if (tcp_req->cmd.fuse != SPDK_NVME_CMD_FUSE_NONE) {
				/* [한국어] fused 명령 처리 */
				if (tcp_req->fused_failed) {
					/* This request failed FUSED semantics.  Fail it immediately, without
					 * even sending it to the target layer.
					 */
					/* [한국어] fused 실패 마킹됨: ABORTED_MISSING_FUSED로 즉시 실패 응답 */
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
					/* [한국어] fused 페어의 상대방이 아직 준비 안 됨 또는 SECOND 미수신.
					 * 상대방이 READY_TO_EXECUTE에 도달하면 이 req도 다시 처리됨. */
					break;
				}
			}

			if (!spdk_nvmf_request_using_zcopy(&tcp_req->req)) {
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_EXECUTING);
				/* [한국어] EXECUTING 상태로 전이 후 spdk_nvmf_request_exec 호출 */
				/* If we get to this point, and this request is a fused command, we know that
				 * it is part of a valid sequence (FIRST followed by a SECOND) and that both
				 * requests are READY_TO_EXECUTE.  So call spdk_nvmf_request_exec() both on this
				 * request, and the other request of the fused pair, in the correct order.
				 * Also clear the ->fused_pair pointers on both requests, since after this point
				 * we no longer need to maintain the relationship between these two requests.
				 */
				if (tcp_req->cmd.fuse == SPDK_NVME_CMD_FUSE_SECOND) {
					/* [한국어] SECOND 명령이면 FIRST를 먼저 실행 (원자적 순서 보장) */
					assert(tcp_req->fused_pair != NULL);
					assert(tcp_req->fused_pair->fused_pair == tcp_req);
					nvmf_tcp_req_set_state(tcp_req->fused_pair, TCP_REQUEST_STATE_EXECUTING);
					spdk_nvmf_request_exec(&tcp_req->fused_pair->req);
					/* [한국어] FIRST 먼저 bdev에 디스패치 */
					tcp_req->fused_pair->fused_pair = NULL;
					tcp_req->fused_pair = NULL;
					/* [한국어] fused 관계 해제 (이후 독립적으로 완료 처리) */
				}
				spdk_nvmf_request_exec(&tcp_req->req);
				/* [한국어] 이 req를 bdev/admin layer에 디스패치 (비동기 실행 시작) */
				if (tcp_req->cmd.fuse == SPDK_NVME_CMD_FUSE_FIRST) {
					/* [한국어] FIRST 명령이면 SECOND를 이어서 실행 */
					assert(tcp_req->fused_pair != NULL);
					assert(tcp_req->fused_pair->fused_pair == tcp_req);
					nvmf_tcp_req_set_state(tcp_req->fused_pair, TCP_REQUEST_STATE_EXECUTING);
					spdk_nvmf_request_exec(&tcp_req->fused_pair->req);
					/* [한국어] SECOND도 bdev에 디스패치 */
					tcp_req->fused_pair->fused_pair = NULL;
					tcp_req->fused_pair = NULL;
				}
			} else {
				/* For zero-copy, only requests with data coming from host to the
				 * controller can end up here. */
				/* [한국어] zcopy write 경로: 호스트에서 받은 데이터가 bdev 버퍼에 있음.
				 * zcopy_end(commit=true)로 bdev에 write 완료 통보. */
				assert(tcp_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_AWAITING_ZCOPY_COMMIT);
				spdk_nvmf_request_zcopy_end(&tcp_req->req, true);
				/* [한국어] zcopy commit: bdev에 "데이터 준비됨" 통보 — 완료 시 EXECUTED 전이 */
			}

			break;
		case TCP_REQUEST_STATE_EXECUTING:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_EXECUTING, tqpair->qpair.trace_id, 0, (uintptr_t)tcp_req);
			/* Some external code must kick a request into TCP_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			/* [한국어] bdev/admin 비동기 실행 중. 완료 콜백(nvmf_tcp_request_complete)이
			 * EXECUTED로 전이시킬 때까지 대기. */
			break;
		case TCP_REQUEST_STATE_AWAITING_ZCOPY_COMMIT:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_COMMIT, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			/* Some external code must kick a request into TCP_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			/* [한국어] zcopy write commit 완료 대기. bdev가 write 완료하면 EXECUTED로 전이. */
			break;
		case TCP_REQUEST_STATE_EXECUTED:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_EXECUTED, tqpair->qpair.trace_id, 0, (uintptr_t)tcp_req);

			if (spdk_unlikely(tcp_req->req.dif_enabled)) {
				/* [한국어] DIF 완료 후 req.length를 원래 사용자 데이터 길이로 복원
				 * (elba_length로 변경됐던 것을 원복) */
				tcp_req->req.length = tcp_req->req.dif.orig_length;
			}

			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
			/* [한국어] 상태 전이: EXECUTED → READY_TO_COMPLETE */
			break;
		case TCP_REQUEST_STATE_READY_TO_COMPLETE:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_READY_TO_COMPLETE, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			if (request_transfer_out(&tcp_req->req) != 0) {
				assert(0); /* No good way to handle this currently */
			}
			/* [한국어] request_transfer_out: sq_head 증가 후 C2H Data PDU 또는 CapsuleResp PDU 송신.
			 * 현재 항상 0 반환. */
			break;
		case TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST, tqpair->qpair.trace_id,
					  0, (uintptr_t)tcp_req);
			/* Some external code must kick a request into TCP_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			/* [한국어] C2H Data PDU(또는 CapsuleResp) 송신 중.
			 * PDU 송신 완료 콜백(_req_pdu_write_done → nvmf_tcp_request_free)이
			 * COMPLETED로 전이시킬 때까지 대기. */
			break;
		case TCP_REQUEST_STATE_AWAITING_ZCOPY_RELEASE:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_RELEASE, tqpair->qpair.trace_id, 0,
					  (uintptr_t)tcp_req);
			/* Some external code must kick a request into TCP_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			/* [한국어] zcopy read 완료 후 bdev 버퍼 반환(zcopy_end(false)) 대기.
			 * bdev 반환 완료 콜백이 COMPLETED로 전이. */
			break;
		case TCP_REQUEST_STATE_COMPLETED:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_COMPLETED, tqpair->qpair.trace_id, 0, (uintptr_t)tcp_req,
					  tqpair->qpair.queue_depth);
			/* If there's an outstanding PDU sent to the host, the request is completed
			 * due to the qpair being disconnected.  We must delay the completion until
			 * that write is done to avoid freeing the request twice. */
			if (spdk_unlikely(tcp_req->pdu_in_use)) {
				/* [한국어] 아직 PDU 송신 중 (pdu_in_use=true): req를 free하면 PDU가 참조하는
				 * 메모리가 해제될 수 있음. 송신 완료 콜백까지 req free 연기. */
				SPDK_DEBUGLOG(nvmf_tcp, "Delaying completion due to outstanding "
					      "write on req=%p\n", tcp_req);
				/* This can only happen for zcopy requests */
				assert(spdk_nvmf_request_using_zcopy(&tcp_req->req));
				assert(!spdk_nvmf_qpair_is_active(&tqpair->qpair));
				break;
			}

			if (tcp_req->req.data_from_pool) {
				/* [한국어] transport buffer pool에서 할당된 버퍼: pool에 반환 */
				spdk_nvmf_request_free_buffers(&tcp_req->req, group, transport);
			} else if (spdk_unlikely(tcp_req->has_in_capsule_data &&
						 (tcp_req->cmd.opc == SPDK_NVME_OPC_FABRIC ||
						  tqpair->qpair.qid == 0) && tcp_req->req.length > transport->opts.in_capsule_data_size)) {
				/* [한국어] control_msg_list에서 할당된 버퍼(대형 ICD): 해당 리스트에 반환.
				 * admin(qid=0) 또는 fabric 명령이고 ICD > in_capsule_data_size인 경우. */
				tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
				assert(tgroup->control_msg_list);
				SPDK_DEBUGLOG(nvmf_tcp, "Put buf to control msg list\n");
				nvmf_tcp_control_msg_put(tgroup->control_msg_list,
							 tcp_req->req.iov[0].iov_base);
				/* [한국어] control_msg 버퍼 반환 + 대기 중인 req 처리 재개 */
			} else if (tcp_req->req.zcopy_bdev_io != NULL) {
				/* If the request has an unreleased zcopy bdev_io, it's either a
				 * read, a failed write, or the qpair is being disconnected */
				/* [한국어] zcopy bdev_io가 아직 반환되지 않음 (read, 실패 write, 또는 disconnect):
				 * AWAITING_ZCOPY_RELEASE로 전이 후 zcopy_end(false)로 bdev에 버퍼 반환 요청 */
				assert(spdk_nvmf_request_using_zcopy(&tcp_req->req));
				assert(tcp_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST ||
				       spdk_nvme_cpl_is_error(&tcp_req->req.rsp->nvme_cpl) ||
				       !spdk_nvmf_qpair_is_active(&tqpair->qpair));
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_AWAITING_ZCOPY_RELEASE);
				spdk_nvmf_request_zcopy_end(&tcp_req->req, false);
				/* [한국어] zcopy 버퍼 반환 요청 (commit 없이) */
				break;
			}
			tcp_req->req.length = 0;	/* [한국어] req 재사용을 위한 필드 초기화 */
			tcp_req->req.iovcnt = 0;	/* [한국어] iov 카운트 리셋 */
			tcp_req->fused_failed = false;	/* [한국어] fused 실패 플래그 초기화 */
			if (tcp_req->fused_pair) {
				/* This req was part of a valid fused pair, but failed before it got to
				 * READ_TO_EXECUTE state.  This means we need to fail the other request
				 * in the pair, because it is no longer part of a valid pair.  If the pair
				 * already reached READY_TO_EXECUTE state, we need to kick it.
				 */
				/* [한국어] READY_TO_EXECUTE 이전에 이 req가 실패 → 페어의 상대방도 실패 처리.
				 * 상대방이 이미 READY_TO_EXECUTE이면 즉시 상태 머신 진행(fused 실패로 처리됨). */
				tcp_req->fused_pair->fused_failed = true;
				if (tcp_req->fused_pair->state == TCP_REQUEST_STATE_READY_TO_EXECUTE) {
					nvmf_tcp_req_process(ttransport, tcp_req->fused_pair);
				}
				tcp_req->fused_pair = NULL;	/* [한국어] fused 관계 해제 */
			}

			nvmf_tcp_req_put(tqpair, tcp_req);
			/* [한국어] req 슬롯을 free 큐에 반환 — 다음 명령에 재사용 */
			break;
		case TCP_REQUEST_NUM_STATES:
		default:
			assert(0);	/* [한국어] 정의되지 않은 상태 — 도달 불가 */
			break;
		}

		if (tcp_req->state != prev_state) {
			progress = true;	/* [한국어] 이번 이터레이션에서 상태 변화 발생 */
		}
	} while (tcp_req->state != prev_state);
	/* [한국어] 상태가 변화하는 동안 계속 루프 — 연속 전이를 한 번의 호출에서 처리 */

	return progress;
	/* [한국어] true이면 진행 있었음 (caller의 통계 추적 또는 추가 처리 결정에 사용) */
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

/*
 * [한국어]
 * nvmf_tcp_poll_group_add - 새 qpair를 poll group에 등록 (transport_ops.poll_group_add)
 *
 * @group: 등록할 대상 poll group (nvmf_tcp_get_optimal_poll_group이 선택한 그룹)
 * @qpair: 등록할 qpair (이미 accept되어 sock이 할당됨)
 * @return: 0=성공, -1=실패
 *
 * 새 TCP 연결이 accept된 후 호출되어 qpair를 poll group의 sock_group에 등록한다.
 * 등록 순서:
 *   1. qpair_sock_init: SO_RCVLOWAT, trace owner 등 소켓 옵션 설정
 *   2. qpair_init: 수신/송신 큐, HDgst/DDgst 초기화
 *   3. qpair_init_mem_resource: req 슬롯 배열, PDU 슬롯 배열, cmd 버퍼 등 DMA 할당
 *   4. sock_group_add_sock: epoll/uring에 소켓 등록 (콜백=nvmf_tcp_sock_cb)
 * 성공 시 tqpair->group 설정, qpair를 tgroup->qpairs 리스트에 추가.
 *
 * 실행 컨텍스트: poll group thread (spdk_thread_send_msg로 deliver됨)
 *
 * 호출 체인:
 *   nvmf_tcp_handle_connect → nvmf_transport_qpair_fini / 또는 common layer
 *     → [nvmf_tcp_poll_group_add]
 */
static int
nvmf_tcp_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_poll_group	*tgroup;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	int				rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	/* [한국어] 공통 poll_group → TCP poll_group 다운캐스트 */
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	/* [한국어] 공통 qpair → TCP qpair 다운캐스트 */

	rc =  nvmf_tcp_qpair_sock_init(tqpair);
	/* [한국어] 소켓 옵션 설정: getsockname/getpeername, trace owner, SO_RCVLOWAT=1 */
	if (rc != 0) {
		SPDK_ERRLOG("Cannot set sock opt for tqpair=%p\n", tqpair);
		return -1;
	}

	rc = nvmf_tcp_qpair_init(&tqpair->qpair);
	/* [한국어] qpair 내부 상태 초기화: 큐, HDgst/DDgst 기본값 등 */
	if (rc < 0) {
		SPDK_ERRLOG("Cannot init tqpair=%p\n", tqpair);
		return -1;
	}

	rc = nvmf_tcp_qpair_init_mem_resource(tqpair);
	/* [한국어] PDU 슬롯 배열, req 슬롯 배열, in-capsule 버퍼 등 메모리 할당 */
	if (rc < 0) {
		SPDK_ERRLOG("Cannot init memory resource info for tqpair=%p\n", tqpair);
		return -1;
	}

	rc = spdk_sock_group_add_sock(tgroup->sock_group, tqpair->sock,
				      nvmf_tcp_sock_cb, tqpair);
	/* [한국어] 소켓을 sock_group(epoll/uring)에 등록.
	 * 소켓에 데이터 도착 시 nvmf_tcp_sock_cb(tqpair) 호출됨. */
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_group_add_sock() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		return -1;
	}

	tqpair->group = tgroup;	/* [한국어] 이 qpair가 속한 poll_group 역참조 저장 */
	nvmf_tcp_qpair_set_state(tqpair, NVMF_TCP_QPAIR_STATE_INVALID);
	/* [한국어] 초기 상태: INVALID (ICReq 대기 중. 아직 협상 안 됨) */
	TAILQ_INSERT_TAIL(&tgroup->qpairs, tqpair, link);
	/* [한국어] poll group의 qpair 리스트에 추가 (이후 polling 대상이 됨) */

	return 0;
}

/*
 * [한국어]
 * nvmf_tcp_poll_group_remove - qpair를 poll group에서 제거 (transport_ops.poll_group_remove)
 *
 * @group: qpair가 속한 poll group
 * @qpair: 제거할 qpair
 * @return: 0=성공, 음수=sock_group_remove_sock 실패 코드
 *
 * qpair 종료 절차의 일환으로 호출. 다음 순서로 정리:
 *   1. AWAIT_REQ 상태이면 QUIESCING으로 전이 (await_req 리스트에서 제거 + 재등록 방지)
 *   2. tgroup->qpairs 리스트에서 제거
 *   3. spdk_sock_flush로 미전송 데이터 최선 전송 시도 (best-effort, 실패 무시)
 *   4. sock_group_remove_sock으로 epoll/uring에서 소켓 제거
 *   5. nvmf_tcp_abort_await_buffer_reqs로 버퍼 대기 중인 req 강제 중단
 *
 * 실행 컨텍스트: poll group thread
 *
 * 호출 체인:
 *   spdk_nvmf_qpair_disconnect → ... → [nvmf_tcp_poll_group_remove]
 *     → nvmf_tcp_abort_await_buffer_reqs → nvmf_tcp_close_qpair
 */
static int
nvmf_tcp_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
			   struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_poll_group	*tgroup;
	struct spdk_nvmf_tcp_qpair		*tqpair;
	int				rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	assert(tqpair->group == tgroup);	/* [한국어] 이 qpair가 이 tgroup에 속해 있어야 함 */

	SPDK_DEBUGLOG(nvmf_tcp, "remove tqpair=%p from the tgroup=%p\n", tqpair, tgroup);
	if (tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_REQ) {
		/* Change the state to move the qpair from the await_req list to the main list
		 * and prevent adding it again later by nvmf_tcp_qpair_set_recv_state() */
		/* [한국어] AWAIT_REQ 상태: await_req 리스트에 있음.
		 * QUIESCING으로 전이하면 qpair_set_recv_state 내부에서 await_req 리스트에서 제거됨.
		 * 이후 QUIESCING 재진입 방지를 위해 명시적 전이. */
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
	}
	TAILQ_REMOVE(&tgroup->qpairs, tqpair, link);
	/* [한국어] poll group의 qpair 리스트에서 제거 — 이후 polling 대상에서 제외 */

	/* Try to force out any pending writes, intentionally do not check rc as it is best effort try. */
	spdk_sock_flush(tqpair->sock);
	/* [한국어] 소켓의 미전송 write 버퍼를 강제로 flush (best-effort; 실패해도 계속 진행) */

	rc = spdk_sock_group_remove_sock(tgroup->sock_group, tqpair->sock);
	/* [한국어] epoll/uring에서 소켓 이벤트 감시 해제 */
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_group_remove_sock() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
	}

	nvmf_tcp_abort_await_buffer_reqs(tqpair);
	/* [한국어] 버퍼 대기 중인 req (NEED_BUFFER 상태) 강제 COMPLETED로 전이 */
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

/*
 * [한국어]
 * nvmf_tcp_close_qpair - qpair 파괴 최종 단계 (transport_ops.qpair_fini 콜백)
 *
 * @qpair:   파괴할 qpair
 * @cb_fn:   파괴 완료 후 nvmf common layer에 통지할 콜백
 * @cb_arg:  콜백 인자
 *
 * poll_group_remove 이후 nvmf common layer가 호출하는 transport 콜백.
 * fini_cb_fn/arg를 저장하고 상태를 EXITED로 전이 후 nvmf_tcp_qpair_destroy를 호출.
 * nvmf_tcp_qpair_destroy는 소켓 닫기 + 메모리 해제 후 cb_fn을 통해 common layer에 완료 통지.
 *
 * 실행 컨텍스트: poll group thread (nvmf_tcp_poll_group_remove 이후 동일 스레드)
 *
 * 호출 체인:
 *   spdk_nvmf_qpair_disconnect → nvmf_tcp_poll_group_remove
 *     → [nvmf_tcp_close_qpair] → nvmf_tcp_qpair_destroy → _nvmf_tcp_qpair_destroy → cb_fn
 */
static void
nvmf_tcp_close_qpair(struct spdk_nvmf_qpair *qpair,
		     spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair;

	SPDK_DEBUGLOG(nvmf_tcp, "Qpair: %p\n", qpair);

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	/* [한국어] 공통 qpair → TCP qpair 다운캐스트 */

	assert(tqpair->fini_cb_fn == NULL);	/* [한국어] 중복 close 방지 — 이미 등록된 콜백 없어야 함 */
	tqpair->fini_cb_fn = cb_fn;		/* [한국어] 완료 콜백 저장 */
	tqpair->fini_cb_arg = cb_arg;		/* [한국어] 완료 콜백 인자 저장 */

	nvmf_tcp_qpair_set_state(tqpair, NVMF_TCP_QPAIR_STATE_EXITED);
	/* [한국어] qpair 상태를 EXITED로 전이 — 더 이상 새 명령 수신 불가 */
	nvmf_tcp_qpair_destroy(tqpair);
	/* [한국어] 소켓 닫기 + 메모리 해제 + cb_fn 호출 (spdk_thread_send_msg 경로) */
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

/*
 * [한국어]
 * nvmf_tcp_qpair_get_trid - qpair의 로컬 또는 피어 transport ID 추출
 *
 * @qpair: 대상 qpair
 * @trid:  결과를 저장할 transport ID 구조체
 * @peer:  true=피어(호스트/initiator) 주소, false=로컬(타깃) 주소
 *
 * qpair_init 시 getsockname/getpeername으로 저장한 주소/포트를 trid 형식으로 변환.
 * trtype=TCP, adrfam=IPv4 또는 IPv6, traddr=IP 문자열, trsvcid=포트 번호 문자열.
 *
 * 호출 체인:
 *   nvmf_tcp_qpair_get_local_trid / get_peer_trid / get_listen_trid → [nvmf_tcp_qpair_get_trid]
 */
static void
nvmf_tcp_qpair_get_trid(struct spdk_nvmf_qpair *qpair,
			struct spdk_nvme_transport_id *trid, bool peer)
{
	struct spdk_nvmf_tcp_qpair     *tqpair;
	uint16_t			port;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	/* [한국어] 공통 qpair → TCP qpair 다운캐스트 */
	spdk_nvme_trid_populate_transport(trid, SPDK_NVME_TRANSPORT_TCP);
	/* [한국어] trid->trtype = SPDK_NVME_TRANSPORT_TCP (trtype 필드 설정) */

	if (peer) {
		/* [한국어] 피어(initiator) 주소: 호스트 IP와 ephemeral 포트 */
		snprintf(trid->traddr, sizeof(trid->traddr), "%s", tqpair->initiator_addr);
		port = tqpair->initiator_port;
	} else {
		/* [한국어] 로컬(target) 주소: 타깃 IP와 listen 포트 (예: 4420) */
		snprintf(trid->traddr, sizeof(trid->traddr), "%s", tqpair->target_addr);
		port = tqpair->target_port;
	}

	if (spdk_sock_is_ipv4(tqpair->sock)) {
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;	/* [한국어] IPv4 주소 패밀리 */
	} else if (spdk_sock_is_ipv6(tqpair->sock)) {
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV6;	/* [한국어] IPv6 주소 패밀리 */
	} else {
		SPDK_ERRLOG("Unsupported socket type for qpair: %p\n", qpair);
		assert(false);	/* [한국어] IPv4/IPv6 외의 소켓 타입 — 지원 불가 */
	}

	snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%d", port);
	/* [한국어] 포트 번호를 문자열로 변환하여 trsvcid에 저장 */
}

/*
 * [한국어]
 * nvmf_tcp_qpair_get_local_trid - qpair의 로컬(타깃) 측 trid 반환 (transport_ops.qpair_get_local_trid)
 *
 * @qpair: 대상 qpair
 * @trid:  결과 저장 구조체
 * @return: 0 (항상 성공)
 *
 * discovery log entry 또는 identify controller 응답에서 자신(타깃)의 주소를 반환할 때 사용.
 */
static int
nvmf_tcp_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	nvmf_tcp_qpair_get_trid(qpair, trid, 0);	/* [한국어] peer=false: 로컬(타깃) 주소 */

	return 0;
}

/*
 * [한국어]
 * nvmf_tcp_qpair_get_peer_trid - qpair의 피어(호스트/initiator) 측 trid 반환 (transport_ops.qpair_get_peer_trid)
 *
 * @qpair: 대상 qpair
 * @trid:  결과 저장 구조체
 * @return: 0 (항상 성공)
 *
 * 접속한 initiator(호스트)의 IP와 포트를 반환. 주로 로그/디버그 용도.
 */
static int
nvmf_tcp_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			     struct spdk_nvme_transport_id *trid)
{
	nvmf_tcp_qpair_get_trid(qpair, trid, 1);	/* [한국어] peer=true: 피어(호스트) 주소 */

	return 0;
}

/*
 * [한국어]
 * nvmf_tcp_qpair_get_listen_trid - qpair의 listen 측 trid 반환 (transport_ops.qpair_get_listen_trid)
 *
 * @qpair: 대상 qpair
 * @trid:  결과 저장 구조체
 * @return: 0 (항상 성공)
 *
 * 현재 구현은 get_local_trid와 동일 (peer=false). listen 포트가 qpair의 로컬 포트와 일치하기 때문.
 */
static int
nvmf_tcp_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
			       struct spdk_nvme_transport_id *trid)
{
	nvmf_tcp_qpair_get_trid(qpair, trid, 0);	/* [한국어] peer=false: 로컬(listen) 주소 */

	return 0;
}

/*
 * [한국어]
 * nvmf_tcp_req_set_abort_status - abort할 req의 완료 상태를 ABORTED_BY_REQUEST로 설정
 *
 * @req:              abort 명령 자체의 nvmf_request (abort req)
 * @tcp_req_to_abort: abort 대상 req
 *
 * 대상 req의 CQE를 ABORTED_BY_REQUEST로 설정하고 READY_TO_COMPLETE 상태로 전이.
 * abort req의 CDW0 bit0을 클리어하여 "abort 성공" 표시.
 *
 * 호출 체인:
 *   _nvmf_tcp_qpair_abort_request → [nvmf_tcp_req_set_abort_status]
 */
static void
nvmf_tcp_req_set_abort_status(struct spdk_nvmf_request *req,
			      struct spdk_nvmf_tcp_req *tcp_req_to_abort)
{
	nvmf_tcp_req_set_cpl(tcp_req_to_abort, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_ABORTED_BY_REQUEST);
	/* [한국어] 대상 req의 CQE를 ABORTED_BY_REQUEST(SCT=GENERIC, SC=0x08)로 설정 */
	nvmf_tcp_req_set_state(tcp_req_to_abort, TCP_REQUEST_STATE_READY_TO_COMPLETE);
	/* [한국어] 대상 req를 READY_TO_COMPLETE로 전이 → 상태 머신에서 완료 처리 */

	req->rsp->nvme_cpl.cdw0 &= ~1U; /* Command was successfully aborted. */
	/* [한국어] abort req 자체의 CDW0 bit0을 클리어 = "명령이 성공적으로 abort됨" 표시.
	 * (bit0=1이면 abort 실패를 의미) */
}

/*
 * [한국어]
 * _nvmf_tcp_qpair_abort_request - abort 대상 req 상태에 따른 실제 abort 처리 (poller 콜백)
 *
 * @ctx: abort req (spdk_nvmf_request*)
 * @return: SPDK_POLLER_BUSY (항상)
 *
 * nvmf_tcp_qpair_abort_request에서 최초 호출되거나 포인터로 등록되어 반복 호출됨.
 * 대상 req 상태에 따라:
 *   - EXECUTING/AWAITING_ZCOPY: nvmf_ctrlr_abort_request 위임 (비동기면 재시도)
 *   - NEED_BUFFER: 버퍼 요청 중단 후 즉시 abort 상태로 처리
 *   - AWAITING_R2T_ACK/TRANSFERRING_H2C: timeout_tsc까지 폴링 대기 (타임아웃 후 포기)
 *   - 기타: abort 불가 (CDW0 bit0 set 상태로 abort req 완료 = "abort 실패")
 *
 * 실행 컨텍스트: poll group thread (poller 콜백)
 *
 * 호출 체인:
 *   nvmf_tcp_qpair_abort_request → [_nvmf_tcp_qpair_abort_request] (→ 재귀적으로 poller 등록)
 */
static int
_nvmf_tcp_qpair_abort_request(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;		/* [한국어] abort 명령 req */
	struct spdk_nvmf_tcp_req *tcp_req_to_abort = SPDK_CONTAINEROF(req->req_to_abort,
			struct spdk_nvmf_tcp_req, req);		/* [한국어] abort 대상 TCP req */
	struct spdk_nvmf_tcp_qpair *tqpair = SPDK_CONTAINEROF(req->req_to_abort->qpair,
					     struct spdk_nvmf_tcp_qpair, qpair);
	/* [한국어] abort 대상의 qpair → TCP qpair 다운캐스트 */
	struct spdk_nvmf_tcp_transport *ttransport = SPDK_CONTAINEROF(tqpair->qpair.transport,
			struct spdk_nvmf_tcp_transport, transport);
	int rc;

	spdk_poller_unregister(&req->poller);
	/* [한국어] 이전에 등록된 poller가 있으면 해제 (재시도 루프 시작 시 클린업) */

	switch (tcp_req_to_abort->state) {
	case TCP_REQUEST_STATE_EXECUTING:
	case TCP_REQUEST_STATE_AWAITING_ZCOPY_START:
	case TCP_REQUEST_STATE_AWAITING_ZCOPY_COMMIT:
		/* [한국어] bdev에서 실행 중인 req: nvmf_ctrlr_abort_request에 위임.
		 * ASYNCHRONOUS이면 나중에 콜백이 오므로 재시도 불필요 — poller 재등록. */
		rc = nvmf_ctrlr_abort_request(req);
		if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS) {
			return SPDK_POLLER_BUSY;
		}
		break;

	case TCP_REQUEST_STATE_NEED_BUFFER:
		/* [한국어] 버퍼 대기 중: 대기 큐에서 제거 후 즉시 abort 상태로 처리 */
		nvmf_tcp_request_get_buffers_abort(tcp_req_to_abort);
		nvmf_tcp_req_set_abort_status(req, tcp_req_to_abort);
		nvmf_tcp_req_process(ttransport, tcp_req_to_abort);
		/* [한국어] READY_TO_COMPLETE → request_transfer_out → 응답 PDU 송신 */
		break;

	case TCP_REQUEST_STATE_AWAITING_R2T_ACK:
	case TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
		/* [한국어] R2T 또는 H2C 데이터 전송 중: 완료를 기다리거나 timeout까지 대기.
		 * timeout_tsc 이전이면 poller 재등록하여 나중에 다시 시도. */
		if (spdk_get_ticks() < req->timeout_tsc) {
			req->poller = SPDK_POLLER_REGISTER(_nvmf_tcp_qpair_abort_request, req, 0);
			/* [한국어] 0µs 주기로 재등록 — 다음 poller tick에 재시도 */
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
		/* [한국어] abort 불가 상태 (예: TRANSFERRING_CONTROLLER_TO_HOST): abort 실패로 완료.
		 * CDW0 bit0이 1인 상태로 abort req를 완료 = "abort 실패" 호스트에 통보. */
		break;
	}

	spdk_nvmf_request_complete(req);
	/* [한국어] abort req 자체를 완료 처리 (transport 콜백 → CQE 호스트 송신) */
	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * nvmf_tcp_qpair_abort_request - NVMe abort 명령 처리 진입점 (transport_ops.qpair_abort_request)
 *
 * @qpair: abort 명령이 수신된 qpair
 * @req:   abort 명령 자체의 nvmf_request (CDW10에 abort할 CID 포함)
 *
 * NVMe abort 명령(opcode 0x08): CDW10.cid 필드로 지정된 CID를 가진 진행 중인 req를
 * 찾아 abort 처리를 시도한다. 해당 CID의 req를 찾지 못하면 즉시 완료(abort 불가).
 * 찾으면 _nvmf_tcp_qpair_abort_request를 통해 상태별 abort 처리.
 *
 * 호출 체인:
 *   nvmf common layer → [nvmf_tcp_qpair_abort_request]
 *     → _nvmf_tcp_qpair_abort_request
 */
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
	/* [한국어] abort 명령의 CDW10.cid: 취소할 명령의 Command ID */

	for (i = 0; i < tqpair->resource_count; i++) {
		/* [한국어] qpair의 모든 req 슬롯을 순회하여 해당 CID를 가진 req 탐색 */
		if (tqpair->reqs[i].state != TCP_REQUEST_STATE_FREE &&
		    tqpair->reqs[i].req.cmd->nvme_cmd.cid == cid) {
			tcp_req_to_abort = &tqpair->reqs[i];
			/* [한국어] abort 대상 req 발견 */
			break;
		}
	}

	spdk_trace_record(TRACE_TCP_QP_ABORT_REQ, tqpair->qpair.trace_id, 0, (uintptr_t)req);
	/* [한국어] SPDK trace: abort 요청 기록 */

	if (tcp_req_to_abort == NULL) {
		/* [한국어] 해당 CID의 req를 찾지 못함: 이미 완료됐거나 존재하지 않음.
		 * abort req를 즉시 완료 — CDW0 bit0=1(abort 실패) 상태로 */
		spdk_nvmf_request_complete(req);
		return;
	}

	req->req_to_abort = &tcp_req_to_abort->req;
	/* [한국어] abort req에 대상 req 참조 저장 */
	req->timeout_tsc = spdk_get_ticks() +
			   transport->opts.abort_timeout_sec * spdk_get_ticks_hz();
	/* [한국어] abort 타임아웃 기한 설정: 현재 시각 + abort_timeout_sec(기본 1초) */
	req->poller = NULL;

	_nvmf_tcp_qpair_abort_request(req);
	/* [한국어] 실제 abort 처리 시작 */
}

/*
 * [한국어]
 * tcp_subsystem_add_host_opts - subsystem_add_host RPC의 TCP 전용 파라미터
 *
 * JSON 스키마:
 *   {"psk": "<keyring_key_name_or_path>"}
 * psk 필드가 없으면 TLS 없이(일반 TCP) 해당 호스트가 허용됨.
 */
struct tcp_subsystem_add_host_opts {
	char *psk;
	/* [한국어] PSK 키 이름 또는 경로 (keyring에 등록된 키 이름).
	 * 설정자: spdk_json_decode_object_relaxed가 JSON에서 추출해 설정.
	 * 읽는 자: nvmf_tcp_subsystem_add_host가 keyring에서 키를 조회하는 데 사용.
	 * 값 범위: NULL(TLS 없음) 또는 유효한 keyring 키 이름 문자열.
	 * 동기화: 단일 스레드 RPC 컨텍스트에서만 사용; 별도 락 불필요. */
};

/*
 * [한국어]
 * tcp_subsystem_add_host_opts_decoder - JSON 디코더 배열
 * spdk_json_decode_object_relaxed에 전달되어 "psk" 키를 tcp_subsystem_add_host_opts.psk에 매핑.
 */
static const struct spdk_json_object_decoder tcp_subsystem_add_host_opts_decoder[] = {
	{"psk", offsetof(struct tcp_subsystem_add_host_opts, psk), spdk_json_decode_string, true},
	/* [한국어] "psk" 키 → opts.psk 필드 (optional=true: 없어도 OK) */
};

/*
 * [한국어]
 * nvmf_tcp_subsystem_add_host - subsystem에 호스트를 추가할 때 TCP PSK 설정 처리
 *   (transport_ops.subsystem_add_host)
 *
 * @transport:          TCP transport 인스턴스
 * @subsystem:          호스트를 추가할 NVMe-oF 서브시스템
 * @hostnqn:            허용할 호스트의 NQN
 * @transport_specific: JSON 형식의 TCP 전용 파라미터 ({"psk": "..."} 또는 NULL)
 * @return:             0=성공, 음수=오류 코드
 *
 * PSK(Pre-Shared Key) 기반 TLS 1.3 설정 흐름:
 *   1. JSON에서 psk 키 이름 추출
 *   2. spdk_keyring에서 키 값 조회 (interchange format: base64)
 *   3. interchange PSK 파싱 → configured PSK 길이 확인 → 암호화 스위트 결정
 *      (32바이트=AES-128-GCM-SHA256, 48바이트=AES-256-GCM-SHA384)
 *   4. PSK identity 생성: hostnqn + subnqn + cipher suite 조합
 *   5. 중복 PSK identity 검사
 *   6. retained PSK 유도: hash 없으면 configured PSK 그대로, 있으면 KDF 적용
 *   7. tcp_psk_entry를 ttransport->psks 리스트에 추가
 * 완료 후 psk_configured/interchange 메모리를 0으로 초기화 (보안 지움).
 *
 * 실행 컨텍스트: RPC 처리 스레드 (단일 스레드)
 *
 * 호출 체인:
 *   nvmf RPC subsystem_add_host → nvmf common layer → [nvmf_tcp_subsystem_add_host]
 */
static int
nvmf_tcp_subsystem_add_host(struct spdk_nvmf_transport *transport,
			    const struct spdk_nvmf_subsystem *subsystem,
			    const char *hostnqn,
			    const struct spdk_json_val *transport_specific)
{
	struct tcp_subsystem_add_host_opts opts;
	struct spdk_nvmf_tcp_transport *ttransport;
	struct tcp_psk_entry *tmp, *entry = NULL;
	uint8_t psk_configured[SPDK_TLS_PSK_MAX_LEN] = {};	/* [한국어] Configured PSK (바이너리, 32 또는 48바이트) */
	char psk_interchange[SPDK_TLS_PSK_MAX_LEN + 1] = {};	/* [한국어] PSK interchange format (base64 인코딩 문자열) */
	uint8_t tls_cipher_suite;	/* [한국어] TLS 암호화 스위트 코드 (AES-128 또는 AES-256) */
	int rc = 0;
	uint8_t psk_retained_hash;	/* [한국어] retained PSK 생성에 사용할 해시 알고리즘 */
	uint64_t psk_configured_size;	/* [한국어] configured PSK 바이너리 길이 (32 또는 48) */

	if (transport_specific == NULL) {
		/* [한국어] transport_specific이 없으면 PSK 설정 없음 → TLS 없이 허용 */
		return 0;
	}

	assert(transport != NULL);
	assert(subsystem != NULL);

	memset(&opts, 0, sizeof(opts));	/* [한국어] opts 초기화 (포인터 NULL 설정) */

	/* Decode PSK (either name of a key or file path) */
	if (spdk_json_decode_object_relaxed(transport_specific, tcp_subsystem_add_host_opts_decoder,
					    SPDK_COUNTOF(tcp_subsystem_add_host_opts_decoder), &opts)) {
		/* [한국어] JSON 파싱 실패: 잘못된 형식 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		return -EINVAL;
	}

	if (opts.psk == NULL) {
		/* [한국어] psk 필드 없음 → PSK 없이 추가 (일반 TCP 연결 허용) */
		return 0;
	}

	entry = calloc(1, sizeof(struct tcp_psk_entry));
	/* [한국어] PSK 엔트리 할당 (hostnqn, subnqn, PSK 값 등 저장) */
	if (entry == NULL) {
		SPDK_ERRLOG("Unable to allocate memory for PSK entry!\n");
		rc = -ENOMEM;
		goto end;
	}

	entry->key = spdk_keyring_get_key(opts.psk);
	/* [한국어] keyring에서 해당 이름의 키 핸들 조회 */
	if (entry->key == NULL) {
		SPDK_ERRLOG("Key '%s' does not exist\n", opts.psk);
		rc = -EINVAL;
		goto end;
	}

	rc = spdk_key_get_key(entry->key, psk_interchange, SPDK_TLS_PSK_MAX_LEN);
	/* [한국어] 키 값을 interchange format(base64)으로 추출 */
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
	/* [한국어] base64 interchange PSK → 바이너리 configured PSK 변환 + 메타 추출 */
	if (rc < 0) {
		SPDK_ERRLOG("Failed to parse PSK interchange!\n");
		goto end;
	}

	/* The Base64 string encodes the configured PSK (32 or 48 bytes binary).
	 * This check also ensures that psk_configured_size is smaller than
	 * psk_retained buffer size. */
	if (psk_configured_size == SHA256_DIGEST_LENGTH) {
		/* [한국어] 32바이트 PSK → AES-128-GCM-SHA256 암호화 스위트 사용 */
		tls_cipher_suite = NVME_TCP_CIPHER_AES_128_GCM_SHA256;
	} else if (psk_configured_size == SHA384_DIGEST_LENGTH) {
		/* [한국어] 48바이트 PSK → AES-256-GCM-SHA384 암호화 스위트 사용 */
		tls_cipher_suite = NVME_TCP_CIPHER_AES_256_GCM_SHA384;
	} else {
		SPDK_ERRLOG("Unrecognized cipher suite!\n");
		rc = -EINVAL;
		goto end;
	}

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	/* [한국어] 공통 transport → TCP transport 다운캐스트 */
	/* Generate PSK identity. */
	rc = nvme_tcp_generate_psk_identity(entry->pskid, sizeof(entry->pskid), hostnqn,
					    subsystem->subnqn, tls_cipher_suite);
	/* [한국어] PSK identity = hostnqn + subnqn + cipher suite 조합의 문자열
	 * TLS 핸드셰이크 시 클라이언트가 제시하는 PSK identity와 매칭에 사용 */
	if (rc) {
		rc = -EINVAL;
		goto end;
	}
	/* Check if PSK identity entry already exists. */
	TAILQ_FOREACH(tmp, &ttransport->psks, link) {
		/* [한국어] 동일한 PSK identity 중복 체크 */
		if (strncmp(tmp->pskid, entry->pskid, NVMF_PSK_IDENTITY_LEN) == 0) {
			SPDK_ERRLOG("Given PSK identity: %s entry already exists!\n", entry->pskid);
			rc = -EEXIST;
			goto end;
		}
	}

	if (snprintf(entry->hostnqn, sizeof(entry->hostnqn), "%s", hostnqn) < 0) {
		/* [한국어] hostnqn 문자열 저장 실패 (길이 초과 등) */
		SPDK_ERRLOG("Could not write hostnqn string!\n");
		rc = -EINVAL;
		goto end;
	}
	if (snprintf(entry->subnqn, sizeof(entry->subnqn), "%s", subsystem->subnqn) < 0) {
		/* [한국어] subnqn 문자열 저장 실패 */
		SPDK_ERRLOG("Could not write subnqn string!\n");
		rc = -EINVAL;
		goto end;
	}

	entry->tls_cipher_suite = tls_cipher_suite;
	/* [한국어] 결정된 암호화 스위트를 엔트리에 저장 */

	/* No hash indicates that Configured PSK must be used as Retained PSK. */
	if (psk_retained_hash == NVME_TCP_HASH_ALGORITHM_NONE) {
		/* Psk configured is either 32 or 48 bytes long. */
		/* [한국어] 해시 없음: Configured PSK = Retained PSK (직접 복사) */
		memcpy(entry->psk, psk_configured, psk_configured_size);
		entry->psk_size = psk_configured_size;
	} else {
		/* Derive retained PSK. */
		/* [한국어] 해시 있음: HKDF-SHA256/SHA384로 Retained PSK 유도
		 * Configured PSK + hostnqn → Retained PSK */
		rc = nvme_tcp_derive_retained_psk(psk_configured, psk_configured_size, hostnqn, entry->psk,
						  SPDK_TLS_PSK_MAX_LEN, psk_retained_hash);
		if (rc < 0) {
			SPDK_ERRLOG("Unable to derive retained PSK!\n");
			goto end;
		}
		entry->psk_size = rc;	/* [한국어] 유도된 retained PSK 크기 */
	}

	TAILQ_INSERT_TAIL(&ttransport->psks, entry, link);
	/* [한국어] transport의 PSK 리스트에 새 엔트리 추가 */
	rc = 0;

end:
	spdk_memset_s(psk_configured, sizeof(psk_configured), 0, sizeof(psk_configured));
	/* [한국어] 보안 지움: configured PSK 바이너리를 스택에서 0으로 초기화 */
	spdk_memset_s(psk_interchange, sizeof(psk_interchange), 0, sizeof(psk_interchange));
	/* [한국어] 보안 지움: interchange PSK 문자열을 0으로 초기화 */

	free(opts.psk);	/* [한국어] JSON 디코더가 할당한 psk 문자열 해제 */
	if (rc != 0) {
		nvmf_tcp_free_psk_entry(entry);	/* [한국어] 오류 시 할당된 엔트리 해제 */
	}

	return rc;
}

/*
 * [한국어]
 * nvmf_tcp_subsystem_remove_host - subsystem에서 호스트 제거 시 PSK 엔트리 삭제
 *   (transport_ops.subsystem_remove_host)
 *
 * @transport:  TCP transport 인스턴스
 * @subsystem:  호스트를 제거할 서브시스템
 * @hostnqn:    제거할 호스트의 NQN
 *
 * ttransport->psks 리스트에서 hostnqn+subnqn 쌍이 일치하는 PSK 엔트리를 찾아 제거.
 * 엔트리를 리스트에서 제거하고 nvmf_tcp_free_psk_entry로 메모리 해제.
 *
 * 호출 체인:
 *   nvmf RPC subsystem_remove_host → nvmf common layer → [nvmf_tcp_subsystem_remove_host]
 */
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
	/* [한국어] 공통 transport → TCP transport 다운캐스트 */
	TAILQ_FOREACH_SAFE(entry, &ttransport->psks, link, tmp) {
		/* [한국어] 안전한 순회 (remove 중에도 tmp로 다음 포인터 보존) */
		if ((strncmp(entry->hostnqn, hostnqn, SPDK_NVMF_NQN_MAX_LEN)) == 0 &&
		    (strncmp(entry->subnqn, subsystem->subnqn, SPDK_NVMF_NQN_MAX_LEN)) == 0) {
			/* [한국어] hostnqn과 subnqn이 모두 일치하는 PSK 엔트리 발견 */
			TAILQ_REMOVE(&ttransport->psks, entry, link);
			/* [한국어] PSK 리스트에서 제거 */
			nvmf_tcp_free_psk_entry(entry);
			/* [한국어] PSK 메모리 해제 (key 반환 포함) */
			break;	/* [한국어] hostnqn+subnqn 쌍은 유일하므로 첫 일치 후 종료 */
		}
	}
}

/*
 * [한국어]
 * nvmf_tcp_subsystem_dump_host - subsystem JSON dump 시 TCP 전용 호스트 설정 출력
 *   (transport_ops.subsystem_dump_host)
 *
 * @transport:  TCP transport 인스턴스
 * @subsystem:  덤프할 서브시스템
 * @hostnqn:    덤프할 호스트 NQN
 * @w:          JSON 작성 컨텍스트
 *
 * hostnqn+subnqn 쌍에 해당하는 PSK 엔트리가 있으면 {"psk": "<key_name>"}을 JSON에 출력.
 * 없으면 아무것도 출력하지 않는다 (일반 TCP 연결로 허용된 호스트).
 *
 * 호출 체인:
 *   nvmf RPC subsystem_get_config → [nvmf_tcp_subsystem_dump_host]
 */
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
	/* [한국어] 공통 transport → TCP transport 다운캐스트 */
	TAILQ_FOREACH(entry, &ttransport->psks, link) {
		if ((strncmp(entry->hostnqn, hostnqn, SPDK_NVMF_NQN_MAX_LEN)) == 0 &&
		    (strncmp(entry->subnqn, subsystem->subnqn, SPDK_NVMF_NQN_MAX_LEN)) == 0) {
			/* [한국어] 일치하는 PSK 엔트리 발견 — key 이름을 JSON에 기록 */
			spdk_json_write_named_string(w, "psk",  spdk_key_get_name(entry->key));
			/* [한국어] JSON: {"psk": "<keyring 키 이름>"} */
			break;	/* [한국어] 유일한 엔트리이므로 첫 일치 후 종료 */
		}
	}
}

/*
 * [한국어]
 * nvmf_tcp_opts_init - TCP transport의 기본 옵션 값 초기화 (transport_ops.opts_init)
 *
 * @opts: 초기화할 transport 옵션 구조체 (호출 후 기본값으로 채워짐)
 *
 * spdk_nvmf_transport_create 호출 전에 불리며, TCP transport의 기본 파라미터를
 * opts 구조체에 채운다. 이후 사용자가 일부 값을 override할 수 있다.
 *
 * 기본값 매핑:
 *   max_queue_depth    → SPDK_NVMF_TCP_DEFAULT_MAX_IO_QUEUE_DEPTH    (128)
 *   max_qpairs_per_ctrlr→ SPDK_NVMF_TCP_DEFAULT_MAX_QPAIRS_PER_CTRLR (128)
 *   in_capsule_data_size→ SPDK_NVMF_TCP_DEFAULT_IN_CAPSULE_DATA_SIZE  (4096)
 *   max_io_size        → SPDK_NVMF_TCP_DEFAULT_MAX_IO_SIZE            (131072)
 *   io_unit_size       → SPDK_NVMF_TCP_DEFAULT_IO_UNIT_SIZE           (131072)
 *   max_aq_depth       → SPDK_NVMF_TCP_DEFAULT_MAX_ADMIN_QUEUE_DEPTH  (32)
 *   num_shared_buffers → SPDK_NVMF_TCP_DEFAULT_NUM_SHARED_BUFFERS     (511)
 *   buf_cache_size     → SPDK_NVMF_TCP_DEFAULT_BUFFER_CACHE_SIZE      (32)
 *   dif_insert_or_strip→ SPDK_NVMF_TCP_DEFAULT_DIF_INSERT_OR_STRIP    (false)
 *   abort_timeout_sec  → SPDK_NVMF_TCP_DEFAULT_ABORT_TIMEOUT_SEC      (1)
 */
static void
nvmf_tcp_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		SPDK_NVMF_TCP_DEFAULT_MAX_IO_QUEUE_DEPTH;
	/* [한국어] I/O qpair의 최대 SQ 깊이 (한 qpair에서 동시 처리 가능한 최대 명령 수) */
	opts->max_qpairs_per_ctrlr =	SPDK_NVMF_TCP_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	/* [한국어] 하나의 controller(NQN)가 가질 수 있는 최대 qpair 수 */
	opts->in_capsule_data_size =	SPDK_NVMF_TCP_DEFAULT_IN_CAPSULE_DATA_SIZE;
	/* [한국어] CapsuleCmd PDU의 in-capsule 데이터 최대 크기 (바이트) */
	opts->max_io_size =		SPDK_NVMF_TCP_DEFAULT_MAX_IO_SIZE;
	/* [한국어] 단일 I/O 요청의 최대 데이터 크기 (바이트) */
	opts->io_unit_size =		SPDK_NVMF_TCP_DEFAULT_IO_UNIT_SIZE;
	/* [한국어] 버퍼 풀의 개별 버퍼 크기 (보통 max_io_size와 동일) */
	opts->max_aq_depth =		SPDK_NVMF_TCP_DEFAULT_MAX_ADMIN_QUEUE_DEPTH;
	/* [한국어] Admin Queue(qid=0)의 최대 깊이 */
	opts->num_shared_buffers =	SPDK_NVMF_TCP_DEFAULT_NUM_SHARED_BUFFERS;
	/* [한국어] transport-level 공유 버퍼 풀 크기 (전체 버퍼 수) */
	opts->buf_cache_size =		SPDK_NVMF_TCP_DEFAULT_BUFFER_CACHE_SIZE;
	/* [한국어] per-poll-group 버퍼 캐시 크기 (공유 풀에서 pre-fetch하는 수) */
	opts->dif_insert_or_strip =	SPDK_NVMF_TCP_DEFAULT_DIF_INSERT_OR_STRIP;
	/* [한국어] T10 DIF(Data Integrity Field) 삽입/제거 활성화 여부 */
	opts->abort_timeout_sec =	SPDK_NVMF_TCP_DEFAULT_ABORT_TIMEOUT_SEC;
	/* [한국어] abort 명령의 최대 대기 시간 (초) */
	opts->transport_specific =      NULL;
	/* [한국어] TCP 전용 추가 옵션 (현재 미사용) */
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
