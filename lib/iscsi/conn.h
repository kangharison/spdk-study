/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI Connection(연결) 헤더 (conn.h)
 *
 * === 파일의 역할 ===
 * 한 개의 TCP 연결을 표현하는 `spdk_iscsi_conn` 구조체와 연결 단위 API를 선언한다.
 * conn은 SPDK iSCSI 타깃의 데이터 평면 핵심 객체로 (1) 소켓 핸들, (2) PDU 수신/송신
 * 상태 머신, (3) 협상된 파라미터 상태, (4) CHAP 인증 상태, (5) 진행 중 task 큐들
 * (queued_r2t/active_r2t/queued_datain), (6) 다양한 타이머(login/logout/shutdown/nop),
 * (7) 인증된 (initiator, target) 식별자 등을 포함한다. accept된 모든 TCP 연결은
 * iscsi_conn_construct()로 conn으로 승격되며, 이후 conn은 단일 SPDK thread(poll group)에
 * 바인딩되어 lockless로 처리된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (수신 측):
 *   portal_grp.c::iscsi_portal_accept → iscsi_conn_construct() →
 *     poll_group에 STAILQ로 등록 → 매 polling tick마다 iscsi_handle_incoming_pdus →
 *     PDU recv state machine (AWAIT_PDU_HDR → AWAIT_PDU_PAYLOAD) → 로그인/SCSI 처리
 * 송신 측:
 *   conn->write_pdu_list에 응답 PDU 추가 → spdk_sock_writev_async (cb_fn 콜백) → 송신
 * 정리:
 *   iscsi_conn_logout/destruct → SCSI 정리 + 소켓 close + free.
 * 실행 컨텍스트: conn은 자신이 속한 poll_group의 SPDK thread에서만 다뤄진다 (thread-affine).
 * cross-thread 작업이 필요하면 spdk_thread_send_msg로 메시지 전달.
 *
 * === 타 모듈과의 연결 ===
 * - iscsi.h: 자료구조 (spdk_iscsi_pdu, spdk_iscsi_sess, spdk_iscsi_poll_group, mempool 등).
 * - param.h: conn->params, sess_param_state_negotiated[]가 협상 상태 추적.
 * - task.h: queued/active task TAILQ들이 spdk_iscsi_task를 매닮.
 * - portal_grp.h: conn 생성 시 portal/pg_tag 정보 제공.
 * - tgt_node.h: 로그인 후 conn->target에 attach, conn->dev로 SCSI 장치 접근.
 * - sock.h: spdk_sock으로 read/writev_async 비동기 송수신.
 * 데이터 흐름: TCP byte stream ↔ PDU 단위(BHS+AHS+Data+Digest) ↔ SCSI command ↔ bdev I/O.
 *
 * === 주요 함수/구조체 요약 ===
 * - enum iscsi_pdu_recv_state: 수신 상태 머신 4-state.
 * - struct spdk_iscsi_lun: conn이 attach한 LUN의 fast-path 디스크립터.
 * - struct spdk_iscsi_conn: 모든 연결 단위 상태 + 큐 + 타이머 + 인증.
 * - iscsi_conn_construct/destruct(): 생성/해제.
 * - iscsi_conn_handle_nop(): NOP-In/Out 처리 (keep-alive).
 * - iscsi_conn_schedule(): 적절한 poll_group으로 conn 이동 (load balancing).
 * - iscsi_conn_logout(): 로그아웃 시작.
 * - iscsi_conn_handle/abort_queued_datain_tasks(): Read sub-task 큐 처리/취소.
 * - iscsi_conn_read_data/readv_data/write_pdu(): I/O 헬퍼.
 * - iscsi_task_cpl/iscsi_task_mgmt_cpl(): SCSI 완료 콜백.
 */

#ifndef SPDK_ISCSI_CONN_H
#define SPDK_ISCSI_CONN_H
/* [한국어] 이중 include 방지. */

#include "spdk/stdinc.h"
/* [한국어] 표준 라이브러리. */

#include "iscsi/iscsi.h"
/* [한국어] PDU/sess/poll_group 등. */
#include "spdk/queue.h"
/* [한국어] TAILQ/STAILQ 매크로. */
#include "spdk/cpuset.h"
/* [한국어] CPU 친화도(cpuset)는 conn의 poll_group 결정 시 사용. */
#include "spdk/scsi.h"
/* [한국어] spdk_scsi_dev/lun/port/desc 등. */

#include "spdk_internal/trace_defs.h"
/* [한국어] SPDK trace 시스템의 정의 — conn->trace_id로 trace 이벤트 발행. */

/*
 * MAX_CONNECTION_PARAMS: The numbers of the params in conn_param_table
 * MAX_SESSION_PARAMS: The numbers of the params in sess_param_table
 */
#define MAX_CONNECTION_PARAMS 14
/* [한국어] 연결 단위 협상 키 개수 (HeaderDigest, DataDigest, MaxRecvDataSegmentLength 등 14개). */
#define MAX_SESSION_PARAMS 19
/* [한국어] 세션 단위 협상 키 개수 (MaxConnections, MaxBurstLength, FirstBurstLength 등 19개). */

#define MAX_ADDRBUF 64
/* [한국어] IP 주소 문자열 버퍼 크기 (IPv4/IPv6 + scope id 여유). */
#define MAX_INITIATOR_ADDR (MAX_ADDRBUF)
/* [한국어] 이니시에이터 주소 문자열 최대 길이 (init_grp.h와 일관). */
#define MAX_TARGET_ADDR (MAX_ADDRBUF)
/* [한국어] 타깃 주소 문자열 최대 길이. */

enum iscsi_pdu_recv_state {
	/* [한국어] PDU 수신 상태 머신. iscsi_handle_incoming_pdus가 본 enum 값으로 분기. */

	/* Ready to wait for PDU */
	ISCSI_PDU_RECV_STATE_AWAIT_PDU_READY,
	/* [한국어] 새 PDU 수신을 위한 객체 준비 (mempool에서 spdk_iscsi_pdu 가져오기) 단계. */

	/* Active connection waiting for any PDU header */
	ISCSI_PDU_RECV_STATE_AWAIT_PDU_HDR,
	/* [한국어] BHS(48바이트) 헤더 수신 대기. 헤더 모두 받으면 길이 파싱 후 PAYLOAD 진입. */

	/* Active connection waiting for payload */
	ISCSI_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD,
	/* [한국어] AHS+Data Segment+Digest 수신 대기. 모두 받으면 PDU 디스패치. */

	/* Active connection does not wait for payload */
	ISCSI_PDU_RECV_STATE_ERROR,
	/* [한국어] 파싱 오류 — PDU 폐기 + 연결 종료 경로. */
};

struct spdk_poller;
/* [한국어] forward 선언 — thread.h 직접 의존 회피. */
struct spdk_iscsi_conn;
/* [한국어] forward 선언 — spdk_iscsi_lun이 conn을 가리키므로 자기 참조. */

struct spdk_iscsi_lun {
	/* [한국어] conn이 attach한 단일 LUN의 fast-path 정보. tgt_node가 가진 LUN 풀에서 선별. */

	struct spdk_iscsi_conn		*conn;
	/* [한국어] 부모 conn 백포인터.
	 * 설정자: conn에 LUN 부착 시.
	 * 동기화: conn thread. */

	struct spdk_scsi_lun		*lun;
	/* [한국어] SPDK SCSI 레이어의 LUN 객체 (bdev wrapper).
	 * 설정자: spdk_scsi_dev_get_lun으로 획득.
	 * 읽는 자: 모든 SCSI 명령 디스패치. */

	struct spdk_scsi_lun_desc	*desc;
	/* [한국어] LUN 핸들(descriptor) — open된 LUN 핸들로 자원 추적과 hot-remove 알림용. */

	struct spdk_poller		*remove_poller;
	/* [한국어] LUN hot-remove 시 진행 중 명령 정리를 위한 poller. */

	TAILQ_ENTRY(spdk_iscsi_lun)	tailq;
	/* [한국어] conn->luns TAILQ 링크. */
};

struct spdk_iscsi_conn {
	/* [한국어] 한 개의 iSCSI(=TCP) 연결의 전체 상태. mempool에서 미리 MAX_ISCSI_CONNECTIONS
	 * 개수만큼 할당되며 활용 시 SPDK_ISCSI_CONNECTION_MEMSET()으로 부분 0-초기화한다.
	 * 초기화 범위는 다음 멤버부터(=portal부터). id/is_valid는 풀 슬롯 메타. */

	int				id;
	/* [한국어] 풀 내 인덱스 (0..MAX_ISCSI_CONNECTIONS-1).
	 * 설정자: 풀 초기화 시 1회.
	 * 읽는 자: trace, 디버그.
	 * 동기화: 불변. */

	int				is_valid;
	/* [한국어] 풀 슬롯 사용 중 플래그 (0=free, 非0=in-use).
	 * 설정자: alloc/free 시.
	 * 동기화: 풀 락 또는 atomic. */

	/*
	 * All fields below this point are reinitialized each time the
	 *  connection object is allocated.  Make sure to update the
	 *  SPDK_ISCSI_CONNECTION_MEMSET() macro if changing which fields
	 *  are initialized when allocated.
	 */
	struct spdk_iscsi_portal	*portal;
	/* [한국어] 이 conn이 들어온 listening portal 백포인터.
	 * 설정자: iscsi_conn_construct(portal, sock).
	 * 읽는 자: portal_host/port 표시, ACL 검증. */

	int				pg_tag;
	/* [한국어] portal이 속한 PG의 tag 캐시 (빠른 매칭용).
	 * 설정자: construct 시 portal->group->tag 복사. */

	char				portal_host[MAX_PORTAL_ADDR + 1];
	/* [한국어] portal의 host 문자열 사본 — Login Response의 TargetAddress 구성에 사용. */
	char				portal_port[MAX_PORTAL_ADDR + 1];
	/* [한국어] portal의 port 문자열 사본. */

	struct spdk_iscsi_poll_group	*pg;
	/* [한국어] 이 conn을 폴링하는 SPDK iSCSI poll_group.
	 * 설정자: construct 시 또는 schedule()로 이동 시.
	 * 읽는 자: poll_group_poll(주기적). */

	struct spdk_sock		*sock;
	/* [한국어] TCP 소켓 핸들 (accept 결과).
	 * 설정자: construct.
	 * 읽는 자: 모든 송수신 경로.
	 * 동기화: conn thread. */

	struct spdk_iscsi_sess		*sess;
	/* [한국어] 이 conn이 속한 iSCSI 세션 (로그인 후 attach).
	 * 설정자: 로그인 성공 시. 단일 세션이 다수 conn을 가질 수 있음(MaxConnectionsPerSession). */

	enum iscsi_connection_state	state;
	/* [한국어] 연결 lifecycle 상태 (INVALID/RUNNING/EXITING/EXITED). */

	int				login_phase;
	/* [한국어] iSCSI Login 단계 (Security/Operational/FullFeature). RFC 3720 §6. */

	bool				is_logged_out;
	/* [한국어] 로그아웃 처리 완료 표시 — destruct 진행 가능 신호. */

	struct spdk_iscsi_pdu		*login_rsp_pdu;
	/* [한국어] 로그인 응답 PDU 임시 보관 (단계별로 응답 누적 후 한 번에 송신). */

	uint16_t			trace_id;
	/* [한국어] SPDK tracing 시 conn 식별 (16-bit, 풀 인덱스 base). */

	uint64_t	last_flush;
	/* [한국어] write_pdu_list를 마지막으로 flush한 TSC. */
	uint64_t	last_fill;
	/* [한국어] write_pdu_list에 마지막으로 추가한 TSC (지연 송신 판단). */
	uint64_t	last_nopin;
	/* [한국어] 마지막 NOP-In 송신 TSC (keep-alive interval 측정). */

	/* Timer used to destroy connection after requesting logout if
	 *  initiator does not send logout request.
	 */
	struct spdk_poller *logout_request_timer;
	/* [한국어] 타깃이 logout 요청 후 ISCSI_LOGOUT_REQUEST_TIMEOUT(=30s) 대기, 미응답 시 강제 destruct. */

	/* Timer used to destroy connection after logout if initiator does
	 *  not close the connection.
	 */
	struct spdk_poller *logout_timer;
	/* [한국어] logout 응답 후 ISCSI_LOGOUT_TIMEOUT(=5s) 대기, 미close 시 강제 close. */

	/* Timer used to wait for connection to close
	 */
	struct spdk_poller *shutdown_timer;
	/* [한국어] 종료 단계에서 진행 중 PDU/태스크 정리 대기 타이머. */

	/* Timer used to destroy connection after creating this connection
	 *  if login process does not complete.
	 */
	struct spdk_poller *login_timer;
	/* [한국어] ISCSI_LOGIN_TIMEOUT(=30s) 내 로그인 미완료 시 강제 destruct. */

	struct spdk_iscsi_pdu *pdu_in_progress;
	/* [한국어] 현재 수신 중인 PDU 임시 객체. AWAIT_PDU_HDR/PAYLOAD에서 점진 채움. */
	enum iscsi_pdu_recv_state pdu_recv_state;
	/* [한국어] 수신 상태 머신 현재 위치. */

	TAILQ_HEAD(, spdk_iscsi_pdu) write_pdu_list;
	/* [한국어] 송신 대기 PDU 큐 (FIFO). spdk_sock_writev_async가 비워감. */
	TAILQ_HEAD(, spdk_iscsi_pdu) snack_pdu_list;
	/* [한국어] SNACK 처리 위해 잠시 보관 중인 PDU들 (재전송 후보). */

	uint32_t pending_r2t;
	/* [한국어] 이 conn에서 발행되어 응답 미수신 R2T 합계. MaxR2TPerConnection 제한. */

	uint16_t cid;
	/* [한국어] iSCSI CID (Connection ID, RFC 3720 §3.2.3). */

	/* IP address */
	char initiator_addr[MAX_INITIATOR_ADDR];
	/* [한국어] 클라이언트 측 IP 문자열 (ACL 검사 + 진단 로그). */
	char target_addr[MAX_TARGET_ADDR];
	/* [한국어] 타깃 측 IP 문자열 (소켓 getsockname 결과). */

	/* Initiator/Target port binds */
	char				initiator_name[MAX_INITIATOR_NAME];
	/* [한국어] 클라이언트가 로그인에서 알려준 InitiatorName(IQN). ACL/세션 키. */
	struct spdk_scsi_port		*initiator_port;
	/* [한국어] SCSI 측 initiator port 객체 (ITT/Reservation 추적용). */
	char				target_short_name[MAX_TARGET_NAME];
	/* [한국어] 협상에서 받은 TargetName 문자열 캐시. */
	struct spdk_scsi_port		*target_port;
	/* [한국어] SCSI 측 target port 객체. */
	struct spdk_iscsi_tgt_node	*target;
	/* [한국어] 이 conn이 attach된 target node. 로그인 후 결정. */
	struct spdk_scsi_dev		*dev;
	/* [한국어] target_node의 SCSI dev 캐시 — fast path. */

	/* To handle the case that SendTargets response is split into
	 * multiple PDUs due to very small MaxRecvDataSegmentLength.
	 */
	uint32_t			send_tgt_completed_size;
	/* [한국어] SendTargets 응답이 다수 PDU로 분할될 때 누적 출력 크기. */
	struct iscsi_param		*params_text;
	/* [한국어] Text 네고시에이션 단계의 임시 params 리스트. */

	/* for fast access */
	int header_digest;
	/* [한국어] 협상 결과 HeaderDigest=CRC32C(1) 또는 None(0). 매 PDU 검증/계산에 빠른 분기. */
	int data_digest;
	/* [한국어] DataDigest=CRC32C/None. */
	int full_feature;
	/* [한국어] FullFeature 단계 진입 여부 (1=진입). */
	int scheduled;
	/* [한국어] poll_group에 schedule 완료 플래그. */

	struct iscsi_param *params;
	/* [한국어] 연결 단위 파라미터 리스트 (param.h). */
	bool sess_param_state_negotiated[MAX_SESSION_PARAMS];
	/* [한국어] 세션 파라미터 키별 협상 완료 플래그 (중복 협상 차단). */
	bool conn_param_state_negotiated[MAX_CONNECTION_PARAMS];
	/* [한국어] 연결 파라미터 키별 협상 완료 플래그. */
	struct iscsi_chap_auth auth;
	/* [한국어] CHAP 인증 진행 상태 (challenge/response 등 임시 값). */
	bool authenticated;
	/* [한국어] 인증 완료 플래그 — FullFeature 진입 조건. */
	bool disable_chap;
	/* [한국어] portal_group에서 상속받은 CHAP 비활성 플래그. */
	bool require_chap;
	/* [한국어] CHAP 강제 요구 여부. */
	bool mutual_chap;
	/* [한국어] 양방향 CHAP 요구 여부. */
	int32_t chap_group;
	/* [한국어] 사용할 CHAP auth_group tag. */
	uint32_t pending_task_cnt;
	/* [한국어] SCSI 레이어에 제출했지만 미완료 task 수. destruct 시 0 대기. */
	uint32_t data_out_cnt;
	/* [한국어] 미처리 Data-Out PDU 카운트. */
	uint32_t data_in_cnt;
	/* [한국어] 송신 대기 DataIn PDU 카운트 (Read 분할). */

	uint64_t timeout;
	/* [한국어] 일반 inactivity timeout (DEFAULT_TIMEOUT=60s 등). */
	uint64_t nopininterval;
	/* [한국어] NOP-In 자동 송신 주기 (DEFAULT_NOPININTERVAL=30s). */
	bool nop_outstanding;
	/* [한국어] NOP-In 송신 후 NOP-Out 응답 대기 중 플래그 (이중 송신 방지). */

	/*
	 * This is the maximum data segment length that iscsi target can send
	 *  to the initiator on this connection.  Not to be confused with the
	 *  maximum data segment length that initiators can send to iscsi target, which
	 *  is statically defined as SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH.
	 */
	int MaxRecvDataSegmentLength;
	/* [한국어] 협상 결과 — 타깃이 한 PDU에 송신 가능한 최대 데이터 segment 길이.
	 * 호스트가 announce한 MaxRecvDataSegmentLength이며, 타깃의 수신 한도는 별도 상수
	 * SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH(=64KB)로 고정. */

	uint32_t StatSN;
	/* [한국어] 다음 송신할 응답의 Status SN (RFC 3720 §10.4). */
	uint32_t exp_statsn;
	/* [한국어] 호스트가 ack한 ExpStatSN (재전송 결정). */
	uint32_t ttt; /* target transfer tag */
	/* [한국어] 다음 발행할 TTT 값 (R2T용 task별 TTT 카운터의 conn-base). */
	char *partial_text_parameter;
	/* [한국어] Text 네고시에이션에서 PDU 경계로 잘린 마지막 key/value 부분문자열. */

	STAILQ_ENTRY(spdk_iscsi_conn) pg_link;
	/* [한국어] poll_group의 connections STAILQ에 매다는 링크. */
	bool			is_stopped;  /* Set true when connection is stopped for migration */
	/* [한국어] poll_group 간 conn 이동(load balance) 도중에는 true. */
	TAILQ_HEAD(queued_r2t_tasks, spdk_iscsi_task)	queued_r2t_tasks;
	/* [한국어] 아직 R2T를 발행하지 않은 Write task 큐. */
	TAILQ_HEAD(active_r2t_tasks, spdk_iscsi_task)	active_r2t_tasks;
	/* [한국어] R2T 발행 후 Data-Out 대기 중인 Write task 큐. */
	TAILQ_HEAD(queued_datain_tasks, spdk_iscsi_task)	queued_datain_tasks;
	/* [한국어] DataIn 송신 대기 Read sub-task 큐. */

	TAILQ_HEAD(, spdk_iscsi_lun)	luns;
	/* [한국어] 이 conn이 attach한 LUN 디스크립터 리스트. */

	TAILQ_ENTRY(spdk_iscsi_conn)	conn_link;
	/* [한국어] 전역/그룹 conn 리스트(예: 동일 target의 conn 목록) 링크. */
};

void iscsi_task_cpl(struct spdk_scsi_task *scsi_task);
/* [한국어] SCSI 명령 완료 시 호출되는 콜백. iscsi_task로 변환 후 응답 PDU 작성. */
void iscsi_task_mgmt_cpl(struct spdk_scsi_task *scsi_task);
/* [한국어] Task Management Function 완료 콜백 (ABORT TASK 등). */

int initialize_iscsi_conns(void);
/* [한국어] 풀 mempool과 poll_group들 초기화. spdk_iscsi_init 단계 호출. */
void shutdown_iscsi_conns(void);
/* [한국어] 모든 conn destruct 시작 + 비동기 완료 후 콜백 호출. */
void iscsi_conns_request_logout(struct spdk_iscsi_tgt_node *target, int pg_tag);
/* [한국어] target에 attach된 모든 conn에 비동기 logout 요청 (target 정리 시). */
int iscsi_get_active_conns(struct spdk_iscsi_tgt_node *target);
/* [한국어] target에 attach된 활성 conn 수 반환. */

int iscsi_conn_construct(struct spdk_iscsi_portal *portal, struct spdk_sock *sock);
/* [한국어] accept된 소켓을 conn으로 승격, poll_group에 등록. portal_grp.c가 호출. */
void iscsi_conn_destruct(struct spdk_iscsi_conn *conn);
/* [한국어] conn 정리 시작 (비동기). 진행 중 task가 모두 끝나야 free. */
void iscsi_conn_handle_nop(struct spdk_iscsi_conn *conn);
/* [한국어] NOP-In/Out keep-alive 처리. */
void iscsi_conn_schedule(struct spdk_iscsi_conn *conn);
/* [한국어] 적합한 poll_group으로 conn 이동 (cpu mask 정책 기반). */
void iscsi_conn_logout(struct spdk_iscsi_conn *conn);
/* [한국어] 로그아웃 시작 (logout_request_timer 설치). */
int iscsi_drop_conns(struct spdk_iscsi_conn *conn,
		     const char *conn_match, int drop_all);
/* [한국어] 일치하는 conn들을 강제 종료. */
int iscsi_conn_handle_queued_datain_tasks(struct spdk_iscsi_conn *conn);
/* [한국어] queued_datain_tasks의 Read sub-task들을 가능한 만큼 송신. */
int iscsi_conn_abort_queued_datain_task(struct spdk_iscsi_conn *conn,
					uint32_t ref_task_tag);
/* [한국어] 특정 ITT의 Read sub-task를 큐에서 abort. */
int iscsi_conn_abort_queued_datain_tasks(struct spdk_iscsi_conn *conn,
		struct spdk_scsi_lun *lun,
		struct spdk_iscsi_pdu *pdu);
/* [한국어] 특정 LUN(또는 전체)의 Read sub-task abort (LUN reset 등). */

int iscsi_conn_read_data(struct spdk_iscsi_conn *conn, int len, void *buf);
/* [한국어] 단일 버퍼로부터 비차단 read (PDU 헤더 read 등). 음수=에러, 0=EAGAIN, >0=읽은 바이트. */
int iscsi_conn_readv_data(struct spdk_iscsi_conn *conn,
			  struct iovec *iov, int iovcnt);
/* [한국어] iovec 다중 버퍼 read (PDU payload split). */
void iscsi_conn_write_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu,
			  iscsi_conn_xfer_complete_cb cb_fn,
			  void *cb_arg);
/* [한국어] PDU를 write_pdu_list에 추가, 송신 완료 시 cb_fn(cb_arg) 호출. */

void iscsi_conn_free_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu);
/* [한국어] PDU 자원 해제 (mempool 반환). */

void iscsi_conn_info_json(struct spdk_json_write_ctx *w, struct spdk_iscsi_conn *conn);
/* [한국어] conn 진단 정보를 JSON RPC 응답으로 출력. */
void iscsi_conn_pdu_generic_complete(void *cb_arg);
/* [한국어] 범용 송신 완료 콜백 (특별한 후처리 없을 때 사용). */
#endif /* SPDK_ISCSI_CONN_H */
/* [한국어] include guard 종료. */
