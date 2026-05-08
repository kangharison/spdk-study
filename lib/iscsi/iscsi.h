/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK iSCSI 타깃 핵심 헤더 (iscsi.h)
 *
 * === 파일의 역할 ===
 * SPDK iSCSI 타깃 구현의 "공통 도메인 헤더". RFC 3720이 정의한 iSCSI 프로토콜의
 * (1) 글로벌 상수(MAX_*, DEFAULT_*), (2) 핵심 자료구조 (PDU, session, opts, globals,
 * poll_group, mempool 객체 mobj, CHAP 인증 컨텍스트), (3) 상태 머신 enum (connection_state,
 * chap_phase, session_type), (4) PDU 처리·세션 관리·CHAP·메모리관리 외부 API의 선언을
 * 모두 모은다. 다른 모든 lib/iscsi/* 파일은 본 헤더에 의존한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK iSCSI 타깃의 "도메인 모델" 정의. 호출 체인의 한 곳을 지목하기보다 다음 모든 흐름의
 * 공통 어휘를 제공한다:
 *   - 제어 평면: spdk_iscsi_init/fini, spdk_iscsi_opts (RPC 진입점)
 *   - 데이터 평면 수신: iscsi_handle_incoming_pdus → BHS/AHS/DataSegment/Digest 파싱 →
 *     opcode 분기 (로그인/SCSI/Data-Out/SNACK/...)
 *   - 데이터 평면 송신: iscsi_get_pdu/put_pdu, iscsi_build_iovs, iscsi_pdu_calc_*_digest
 *   - 세션/CHAP: spdk_iscsi_sess, iscsi_chap_auth, auth_group/auth_secret
 *   - 메모리: pdu_pool, pdu_immediate_data_pool, pdu_data_out_pool, session_pool, task_pool
 *     (DPDK rte_mempool 기반 spdk_mempool)
 *   - poll_group: 다중 reactor에서 conn을 분산 폴링 (sock_group 한 개 + nop_poller)
 * 실행 컨텍스트: 자료구조 자체는 thread-affine로 사용되며, 전역 g_iscsi는 mutex 보호.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/iscsi_spec.h: RFC 3720 와이어 포맷 (iscsi_bhs 등). 본 파일에서 PDU 구성 시 사용.
 * - spdk/sock.h: spdk_sock_request, spdk_sock_group — PDU 비동기 송신.
 * - spdk/scsi.h: spdk_scsi_dev/lun/task — SCSI 레이어 인터페이스.
 * - spdk/bdev.h: 최종 I/O 디바이스 (간접 의존).
 * - spdk/dif.h: T10-DIF 데이터 무결성 처리 (PDU에 dif_ctx 보유).
 * - spdk/thread.h, env.h: poller, mempool 등 SPDK 인프라.
 * - param.h: 협상 파라미터 리스트.
 * 데이터 흐름 요약: 외부 TCP byte → spdk_iscsi_pdu (mempool) → opcode 분기 → spdk_iscsi_task
 *   → spdk_scsi_task → bdev → 응답 PDU → write_pdu_list → spdk_sock_writev_async.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_mobj: mempool 객체 wrapper (data buf + 길이).
 * - struct spdk_iscsi_pdu: PDU 한 개 (BHS+AHS+Data+Digest+sock_req+iov[]+sense).
 * - struct iscsi_chap_auth: CHAP 인증 진행 상태 (challenge/response).
 * - struct spdk_iscsi_sess: iSCSI 세션 (다수 conn 묶음, ISID/TSIH).
 * - struct spdk_iscsi_poll_group: poller + nop_poller + connections STAILQ.
 * - struct spdk_iscsi_opts: 부팅 옵션 (RPC iscsi_set_options).
 * - struct spdk_iscsi_globals: 전역 g_iscsi (모든 target/IG/PG/auth/session).
 * - spdk_iscsi_init/fini(): 비동기 초기화/종료.
 * - iscsi_handle_incoming_pdus(): 연결 단위 수신 디스패처.
 * - iscsi_get_pdu/put_pdu(): mempool 기반 PDU lifecycle.
 * - iscsi_pdu_calc_header_digest/data_digest(): CRC32C 계산.
 * - iscsi_queue_task(): SCSI 레이어로 task 제출.
 */

#ifndef SPDK_ISCSI_H
#define SPDK_ISCSI_H
/* [한국어] include guard. */

#include "spdk/stdinc.h"
/* [한국어] 표준 라이브러리. */
#include "spdk/env.h"
/* [한국어] DPDK env 추상화 (spdk_mempool, hugepage 메모리). */
#include "spdk/bdev.h"
/* [한국어] bdev 레이어 — 최종 SCSI dev backend. */
#include "spdk/iscsi_spec.h"
/* [한국어] RFC 3720 와이어 포맷 정의 (iscsi_bhs 48바이트 BHS 등). */
#include "spdk/thread.h"
/* [한국어] SPDK thread/poller 추상화. */
#include "spdk/sock.h"
/* [한국어] SPDK 소켓 추상 (spdk_sock_request 등). */

#include "spdk/scsi.h"
/* [한국어] SPDK SCSI 레이어 인터페이스 (LUN/task/dev). */
#include "iscsi/param.h"
/* [한국어] 협상 파라미터 자료구조. */

#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT 매크로. */
#include "spdk/dif.h"
/* [한국어] T10-DIF 데이터 무결성 컨텍스트. */
#include "spdk/util.h"
/* [한국어] SPDK_CONTAINEROF 등. */

#define SPDK_ISCSI_DEFAULT_NODEBASE "iqn.2016-06.io.spdk"
/* [한국어] 자동 생성된 target IQN의 base 도메인 prefix. RFC 3721 §1.1 IQN 형식 — 이를
 * 베이스로 ":<TargetName>"을 붙여 완전한 IQN을 만든다. */

#define DEFAULT_MAXR2T 4
/* [한국어] 동시 미완료 R2T 기본값. RFC 3720 §12.16. */
#define MAX_INITIATOR_PORT_NAME 256
/* [한국어] iSCSI ISID와 호스트 이름을 합친 표현의 최대 길이. */
#define MAX_INITIATOR_NAME 223
/* [한국어] RFC 3720 IQN 최대 길이 (223자). */
#define MAX_TARGET_NAME 223
/* [한국어] target IQN 최대 길이 (이니시에이터와 동일). */

#define MAX_PORTAL 1024
/* [한국어] 시스템 전체 portal 최대 수. */
#define MAX_INITIATOR 256
/* [한국어] 한 IG 당 이름 최대 수 (init_grp.h 참조). */
#define MAX_NETMASK 256
/* [한국어] 한 IG 당 넷마스크 최대 수. */
#define MAX_ISCSI_CONNECTIONS 1024
/* [한국어] 시스템 전체 동시 conn 최대 수 (mempool 크기). */
#define MAX_PORTAL_ADDR 256
/* [한국어] portal host 문자열 최대 길이. */
#define MAX_PORTAL_PORT 32
/* [한국어] portal port 문자열 최대 길이. */

#define DEFAULT_PORT 3260
/* [한국어] iSCSI well-known TCP port (IANA). */
#define DEFAULT_MAX_SESSIONS 128
/* [한국어] 시스템 전체 동시 세션 기본값. */
#define DEFAULT_MAX_CONNECTIONS_PER_SESSION 2
/* [한국어] 한 세션당 conn 기본값 (RFC 3720 §12.2). */
#define DEFAULT_MAXOUTSTANDINGR2T 1
/* [한국어] 동시 미완료 R2T 기본 1. */
#define DEFAULT_DEFAULTTIME2WAIT 2
/* [한국어] 로그아웃 후 재로그인 대기 시간(초) 기본값. */
#define DEFAULT_DEFAULTTIME2RETAIN 20
/* [한국어] 세션 상태 보유 시간(초) 기본값. */
#define DEFAULT_INITIALR2T true
/* [한국어] InitialR2T 기본 Yes (RFC 3720 §12.10). */
#define DEFAULT_IMMEDIATEDATA true
/* [한국어] ImmediateData 기본 Yes (즉시 데이터 동봉 허용). */
#define DEFAULT_DATAPDUINORDER true
/* [한국어] Data-Out PDU 순서 보장. */
#define DEFAULT_DATASEQUENCEINORDER true
/* [한국어] Data sequence 순서 보장. */
#define DEFAULT_ERRORRECOVERYLEVEL 0
/* [한국어] ERL=0 (가장 단순한 에러 회복: 세션 재시작). */
#define DEFAULT_TIMEOUT 60
/* [한국어] inactive timeout 60초. */
#define MAX_NOPININTERVAL 60
/* [한국어] NOP-In 자동 송신 간격 최댓값 60초. */
#define DEFAULT_NOPININTERVAL 30
/* [한국어] NOP-In 자동 송신 간격 기본 30초 (keep-alive). */

/*
 * SPDK iSCSI target currently only supports 64KB as the maximum data segment length
 *  it can receive from initiators.  Other values may work, but no guarantees.
 */
#define SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH  65536
/* [한국어] 타깃이 한 PDU 데이터 segment로 받을 수 있는 최대 바이트 (=64KB).
 * mempool data buffer 크기와 일치. 더 큰 값은 미지원. */

/*
 * Defines maximum number of data out buffers each connection can have in
 *  use at any given time.
 */
#define MAX_DATA_OUT_PER_CONNECTION 16
/* [한국어] 연결당 동시 사용 가능한 Data-Out 누적 버퍼 수 한계. */

/*
 * Defines default maximum number of data in buffers each connection can have in
 *  use at any given time. So this limit does not affect I/O smaller than
 *  SPDK_BDEV_SMALL_BUF_MAX_SIZE.
 */
#define DEFAULT_MAX_LARGE_DATAIN_PER_CONNECTION 64
/* [한국어] 연결당 동시 사용 가능한 큰 DataIn 버퍼 수 (큰 read 분할용). */

#define SPDK_ISCSI_MAX_BURST_LENGTH	\
		(SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH * MAX_DATA_OUT_PER_CONNECTION)
/* [한국어] 한 R2T burst의 최대 데이터 길이 = 64KB × 16 = 1MB.
 * RFC 3720 §12.13 MaxBurstLength의 SPDK 상한. */

/*
 * Defines default maximum amount in bytes of unsolicited data the iSCSI
 *  initiator may send to the SPDK iSCSI target during the execution of
 *  a single SCSI command. And it is smaller than the MaxBurstLength.
 */
#define SPDK_ISCSI_FIRST_BURST_LENGTH	8192
/* [한국어] FirstBurstLength 기본 8KB — 초청 받지 않은 immediate/unsolicited 데이터 합계. */

/*
 * Defines minimum amount in bytes of unsolicited data the iSCSI initiator
 *  may send to the SPDK iSCSI target during the execution of a single
 *  SCSI command.
 */
#define SPDK_ISCSI_MIN_FIRST_BURST_LENGTH	512
/* [한국어] FirstBurstLength 최솟값 512B (RFC 3720 §12.14 MUST ≥ 512). */

#define SPDK_ISCSI_MAX_FIRST_BURST_LENGTH	16777215
/* [한국어] FirstBurstLength 최댓값 (16M-1, 24-bit 길이 필드 한계). */

/*
 * Defines default maximum queue depth per connection and this can be
 * changed by configuration file.
 */
#define DEFAULT_MAX_QUEUE_DEPTH	64
/* [한국어] 연결당 동시 미완료 SCSI command 기본값. */

/** Defines how long we should wait for a logout request when the target
 *   requests logout to the initiator asynchronously.
 */
#define ISCSI_LOGOUT_REQUEST_TIMEOUT 30 /* in seconds */
/* [한국어] 비동기 logout 요청 후 응답 대기 30초. 미응답 시 강제 종료. */

/** Defines how long we should wait for a TCP close after responding to a
 *   logout request, before terminating the connection ourselves.
 */
#define ISCSI_LOGOUT_TIMEOUT 5 /* in seconds */
/* [한국어] logout 응답 후 클라이언트가 close하지 않을 때 5초 대기 후 강제 close. */

/** Defines how long we should wait until login process completes. */
#define ISCSI_LOGIN_TIMEOUT 30 /* in seconds */
/* [한국어] 연결 수락 후 30초 내 로그인 미완료 시 강제 destruct. */

/* For spdk_iscsi_login_in related function use, we need to avoid the conflict
 * with other errors
 * */
#define SPDK_ISCSI_LOGIN_ERROR_RESPONSE -1000
/* [한국어] 로그인 응답 단계 에러 — 일반 errno와 충돌 없는 큰 음수. */
#define SPDK_ISCSI_LOGIN_ERROR_PARAMETER -1001
/* [한국어] 로그인 파라미터 검증 실패. */
#define SPDK_ISCSI_PARAMETER_EXCHANGE_NOT_ONCE -1002
/* [한국어] 동일 키의 중복 협상(RFC 위반) 감지. */

#define ISCSI_AHS_LEN 60
/* [한국어] PDU의 AHS(Additional Header Segment) 최대 60바이트 — bidirectional read AHS,
 * extended CDB AHS 등을 충분히 담을 수 있는 크기. */

struct spdk_mobj {
	/* [한국어] mempool에서 빌린 데이터 버퍼 wrapper. PDU 데이터 segment에 사용. */

	struct spdk_mempool *mp;
	/* [한국어] 이 객체가 속한 mempool. put 시 어디로 돌려보낼지 알기 위함.
	 * 설정자: mempool 초기화 시 element 별로 설정.
	 * 동기화: mempool 자체가 lock-free 큐. */

	void *buf;
	/* [한국어] 데이터 버퍼 시작 주소 (SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH 크기 보장).
	 * 설정자: 초기화 시 1회. 변경 안 됨. */

	uint32_t data_len;
	/* [한국어] 현재 buf에 채워진 유효 데이터 길이.
	 * 설정자: PDU 수신/송신 시.
	 * 읽는 자: 송신 iovec 구성. */
};

/*
 * Maximum number of SGL elements, i.e.,
 * BHS, AHS, Header Digest, Data Segment and Data Digest.
 */
#define SPDK_ISCSI_MAX_SGL_DESCRIPTORS	(5)
/* [한국어] PDU 한 개의 송수신을 위한 iovec 슬롯 수 = 5
 * (BHS, AHS, HeaderDigest, Data, DataDigest). */

#define SPDK_CRC32C_INITIAL	0xffffffffUL
/* [한국어] CRC32C 초기값 (RFC 3385). */
#define SPDK_CRC32C_XOR		0xffffffffUL
/* [한국어] CRC32C 최종 XOR 값. */

typedef void (*iscsi_conn_xfer_complete_cb)(void *cb_arg);
/* [한국어] PDU 송신 완료 콜백 시그니처. write_pdu_list에서 비워질 때 호출. */

struct spdk_iscsi_pdu {
	/* [한국어] iSCSI PDU 한 개를 표현. mempool에서 할당되며 ref 카운트로 다중 task 공유. */

	struct iscsi_bhs bhs;
	/* [한국어] Basic Header Segment (RFC 3720 §10.1, 48바이트 고정).
	 * 설정자: 수신 PDU는 sock recv로, 송신 PDU는 opcode별 builder가 채움.
	 * 읽는 자: opcode 분기 디스패처, AHS/Data 길이 파싱.
	 * 동기화: 단일 conn thread. */

	/* Merge multiple Data-OUT PDUs in a sequence into a subtask up to
	 * SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH or the final PDU comes.
	 *
	 * Both the size of a data buffer and MaxRecvDataSegmentLength are
	 * SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH at most. Hence the data segment of
	 * a Data-OUT PDU can be split into two data buffers at most.
	 */
	struct spdk_mobj *mobj[2];
	/* [한국어] Data-Out 누적용 mobj 슬롯 (최대 2개 — 버퍼 경계 걸침 처리).
	 * 설정자: Data-Out 누적 시 task의 mobj를 복사.
	 * 읽는 자: 누적 완료 후 SCSI 제출 시. */

	bool is_rejected;
	/* [한국어] PDU가 reject 처리됐는지 표시 (Reject PDU 송신 시 cleanup 결정). */
	uint8_t *data;
	/* [한국어] 데이터 segment 시작 포인터 (mobj->buf로 가리킬 수도, AHS 영역의 일부일 수도). */
	uint8_t header_digest[ISCSI_DIGEST_LEN];
	/* [한국어] 송수신용 HeaderDigest 4바이트 버퍼. */
	uint8_t data_digest[ISCSI_DIGEST_LEN];
	/* [한국어] 송수신용 DataDigest 4바이트 버퍼. */
	size_t data_segment_len;
	/* [한국어] 데이터 segment 실제 길이. */
	int bhs_valid_bytes;
	/* [한국어] 수신 진행 중 BHS에 받은 바이트 수 (점진 read). */
	int ahs_valid_bytes;
	/* [한국어] 수신 진행 중 AHS에 받은 바이트 수. */
	uint32_t data_valid_bytes;
	/* [한국어] 수신 진행 중 데이터 segment에 받은 바이트 수. */
	int hdigest_valid_bytes;
	/* [한국어] HeaderDigest 수신 진행 바이트 수. */
	int ddigest_valid_bytes;
	/* [한국어] DataDigest 수신 진행 바이트 수. */
	int ref;
	/* [한국어] 참조 카운트. associate_pdu에서 ++, put_pdu에서 --. 0 도달 시 mempool 반환. */
	bool data_from_mempool;  /* indicate whether the data buffer is allocated from mempool */
	/* [한국어] data 버퍼가 mempool에서 빌린 것인지 여부 (free 경로 분기에 사용). */
	struct spdk_iscsi_task *task; /* data tied to a task buffer */
	/* [한국어] 이 PDU와 연관된 task. SCSI Command/Response/Data 모두 task와 1:1 또는 N:1. */
	uint32_t cmd_sn;
	/* [한국어] 송수신 PDU의 CmdSN/StatSN 캐시. */
	uint32_t writev_offset;
	/* [한국어] writev_async가 부분 송신했을 때 다음 시작 오프셋. */
	uint32_t data_buf_len;
	/* [한국어] data 버퍼 크기 (mobj 크기와 같거나 그 합). */
	uint32_t data_offset;
	/* [한국어] 송신 PDU의 LBA offset (DataIn). */
	uint32_t crc32c;
	/* [한국어] 진행 중 CRC32C 누적값. */
	bool dif_insert_or_strip;
	/* [한국어] T10-DIF insert/strip 활성화 여부. */
	struct spdk_dif_ctx dif_ctx;
	/* [한국어] T10-DIF 컨텍스트 (block size, ref tag 시드 등). */
	struct spdk_iscsi_conn *conn;
	/* [한국어] 이 PDU가 속한 conn. */

	iscsi_conn_xfer_complete_cb		cb_fn;
	/* [한국어] 송신 완료 후 호출할 콜백. NULL이면 기본 처리. */
	void					*cb_arg;
	/* [한국어] 콜백 ctx. */

	/* The sock request ends with a 0 length iovec. Place the actual iovec immediately
	 * after it. There is a static assert below to check if the compiler inserted
	 * any unwanted padding */
	int32_t						mapped_length;
	/* [한국어] iov[]에 매핑된 총 바이트 수 — sock writev_async 결과 검증. */
	struct spdk_sock_request			sock_req;
	/* [한국어] 비동기 송신 요청 객체 (SPDK 소켓 백엔드가 iovec 시작점을 sock_req 뒤에서 가정). */
	struct iovec					iov[SPDK_ISCSI_MAX_SGL_DESCRIPTORS];
	/* [한국어] 송신용 iovec 5개 슬롯. sock_req 직후 메모리에 위치해야 함 (아래 STATIC_ASSERT). */
	TAILQ_ENTRY(spdk_iscsi_pdu)	tailq;
	/* [한국어] write_pdu_list 또는 snack_pdu_list 등에 매다는 링크. */


	/*
	 * 60 bytes of AHS should suffice for now.
	 * This should always be at the end of PDU data structure.
	 * we need to not zero this out when doing memory clear.
	 */
	uint8_t ahs[ISCSI_AHS_LEN];
	/* [한국어] AHS 버퍼 (60바이트). 구조체 끝에 배치하여 fast clear 시 제외 가능. */

	struct {
		uint16_t length; /* iSCSI SenseLength (big-endian) */
		/* [한국어] SCSI sense data 길이 (big-endian on-wire). */
		uint8_t data[32];
		/* [한국어] sense data 버퍼 (CHECK CONDITION 응답용). */
	} sense;
};
SPDK_STATIC_ASSERT(offsetof(struct spdk_iscsi_pdu,
			    sock_req) + sizeof(struct spdk_sock_request) == offsetof(struct spdk_iscsi_pdu, iov),
		   "Compiler inserted padding between iov and sock_req");
/* [한국어] sock_req와 iov 사이에 padding이 없어야 함을 컴파일 타임에 보장.
 * SPDK 소켓 백엔드가 sock_req 끝 직후를 iovec 시작점으로 사용하기 때문. */

enum iscsi_connection_state {
	/* [한국어] conn lifecycle 상태. */

	ISCSI_CONN_STATE_INVALID = 0,
	/* [한국어] 풀 슬롯 미사용 또는 미초기화. */
	ISCSI_CONN_STATE_RUNNING = 1,
	/* [한국어] 정상 동작 중 (로그인 후 또는 로그인 진행 중). */
	ISCSI_CONN_STATE_EXITING = 2,
	/* [한국어] destruct 진행 중 (진행 중 task 정리 대기). */
	ISCSI_CONN_STATE_EXITED = 3,
	/* [한국어] destruct 완료, free 가능. */
};

enum iscsi_chap_phase {
	/* [한국어] CHAP 인증 4단계 상태 머신 (RFC 3720 §11.1). */

	ISCSI_CHAP_PHASE_NONE = 0,
	/* [한국어] 인증 미시작 또는 비활성. */
	ISCSI_CHAP_PHASE_WAIT_A = 1,
	/* [한국어] CHAP_A(알고리즘 협상) 응답 대기. */
	ISCSI_CHAP_PHASE_WAIT_NR = 2,
	/* [한국어] CHAP_N(이름)+CHAP_R(응답) 대기. */
	ISCSI_CHAP_PHASE_END = 3,
	/* [한국어] 인증 종료(성공 또는 실패). */
};

enum session_type {
	/* [한국어] iSCSI 세션 종류 (RFC 3720 §3.4). */

	SESSION_TYPE_INVALID = 0,
	/* [한국어] 미결정. */
	SESSION_TYPE_NORMAL = 1,
	/* [한국어] Normal session — SCSI I/O. */
	SESSION_TYPE_DISCOVERY = 2,
	/* [한국어] Discovery session — SendTargets만 허용, I/O 불가. */
};

#define ISCSI_CHAP_CHALLENGE_LEN	1024
/* [한국어] CHAP challenge 최대 길이 (1KB). */
#define ISCSI_CHAP_MAX_USER_LEN		255
/* [한국어] CHAP 사용자 이름 최대 길이. */
#define ISCSI_CHAP_MAX_SECRET_LEN	255
/* [한국어] CHAP secret 최대 길이. */

struct iscsi_chap_auth {
	/* [한국어] 한 conn의 CHAP 인증 진행 상태 (challenge/response 등 일시 값). */

	enum iscsi_chap_phase chap_phase;
	/* [한국어] 현재 인증 phase. */

	char user[ISCSI_CHAP_MAX_USER_LEN + 1];
	/* [한국어] 클라이언트 CHAP 사용자명. */
	char secret[ISCSI_CHAP_MAX_SECRET_LEN + 1];
	/* [한국어] 클라이언트 CHAP secret (auth_group에서 lookup 결과). */
	char muser[ISCSI_CHAP_MAX_USER_LEN + 1];
	/* [한국어] mutual CHAP — 타깃 측 사용자명. */
	char msecret[ISCSI_CHAP_MAX_SECRET_LEN + 1];
	/* [한국어] mutual CHAP — 타깃 측 secret. */

	uint8_t chap_id[1];
	/* [한국어] CHAP_I (Identifier) 1바이트 — 클라이언트 검증용. */
	uint8_t chap_mid[1];
	/* [한국어] mutual CHAP의 ID. */
	int chap_challenge_len;
	/* [한국어] 클라이언트에 보낸 challenge 길이. */
	uint8_t chap_challenge[ISCSI_CHAP_CHALLENGE_LEN];
	/* [한국어] 클라이언트에 보낸 challenge 데이터 (랜덤). */
	int chap_mchallenge_len;
	/* [한국어] mutual challenge(타깃→이니시에이터) 길이. */
	uint8_t chap_mchallenge[ISCSI_CHAP_CHALLENGE_LEN];
	/* [한국어] mutual challenge 데이터. */
};

struct spdk_iscsi_auth_secret {
	/* [한국어] 인증 그룹의 단일 (사용자, secret) 쌍. */

	char user[ISCSI_CHAP_MAX_USER_LEN + 1];
	/* [한국어] 사용자명 키. */
	char secret[ISCSI_CHAP_MAX_SECRET_LEN + 1];
	/* [한국어] secret. */
	char muser[ISCSI_CHAP_MAX_USER_LEN + 1];
	/* [한국어] mutual user (필요 시). */
	char msecret[ISCSI_CHAP_MAX_SECRET_LEN + 1];
	/* [한국어] mutual secret. */
	TAILQ_ENTRY(spdk_iscsi_auth_secret) tailq;
	/* [한국어] auth_group.secret_head 링크. */
};

struct spdk_iscsi_auth_group {
	/* [한국어] 다수 (사용자, secret)을 묶는 인증 그룹. PG/target에서 chap_group tag로 참조. */

	int32_t tag;
	/* [한국어] 식별자 (양수). */
	TAILQ_HEAD(, spdk_iscsi_auth_secret) secret_head;
	/* [한국어] 그룹 내 secret들의 헤드. */
	TAILQ_ENTRY(spdk_iscsi_auth_group) tailq;
	/* [한국어] g_iscsi.auth_group_head 링크. */
};

struct spdk_iscsi_sess {
	/* [한국어] iSCSI 세션 — 같은 (initiator, target, ISID) 쌍의 conn 묶음. */

	uint32_t connections;
	/* [한국어] 현재 attach된 conn 수. */
	struct spdk_iscsi_conn **conns;
	/* [한국어] conn 포인터 배열 (MaxConnections 크기). */

	struct spdk_scsi_port *initiator_port;
	/* [한국어] SCSI 측 initiator port (세션 단위 reservation 추적). */
	int tag;
	/* [한국어] 세션을 발급한 target의 portal_group tag. */

	uint64_t isid;
	/* [한국어] Initiator Session ID (RFC 3720 §10.12.5, 48-bit이지만 64로 보관). */
	uint16_t tsih;
	/* [한국어] Target Session Identifying Handle (target이 발급). */
	struct spdk_iscsi_tgt_node *target;
	/* [한국어] 세션이 attach된 target_node. */
	int queue_depth;
	/* [한국어] 세션 단위 SCSI 큐 깊이. */

	struct iscsi_param *params;
	/* [한국어] 세션 협상 파라미터 리스트. */

	enum session_type session_type;
	/* [한국어] Normal 또는 Discovery. */
	uint32_t MaxConnections;
	/* [한국어] 협상된 MaxConnections. */
	uint32_t MaxOutstandingR2T;
	/* [한국어] 협상된 MaxOutstandingR2T. */
	uint32_t DefaultTime2Wait;
	/* [한국어] 협상된 DefaultTime2Wait. */
	uint32_t DefaultTime2Retain;
	/* [한국어] 협상된 DefaultTime2Retain. */
	uint32_t FirstBurstLength;
	/* [한국어] 협상된 FirstBurstLength. */
	uint32_t MaxBurstLength;
	/* [한국어] 협상된 MaxBurstLength. */
	bool InitialR2T;
	/* [한국어] 협상된 InitialR2T (true=R2T 강제, false=unsolicited 데이터 허용). */
	bool ImmediateData;
	/* [한국어] 협상된 ImmediateData. */
	bool DataPDUInOrder;
	/* [한국어] 협상된 DataPDUInOrder. */
	bool DataSequenceInOrder;
	/* [한국어] 협상된 DataSequenceInOrder. */
	uint32_t ErrorRecoveryLevel;
	/* [한국어] 협상된 ERL. */

	uint32_t ExpCmdSN;
	/* [한국어] 다음 기대 CmdSN. */
	uint32_t MaxCmdSN;
	/* [한국어] 클라이언트에 알려준 송신 가능 최대 CmdSN. */

	uint32_t current_text_itt;
	/* [한국어] 현재 진행 중인 Text 네고시에이션의 ITT. */
};

struct spdk_iscsi_poll_group {
	/* [한국어] 다수 conn을 묶어 한 SPDK thread에서 폴링하는 단위. */

	struct spdk_poller				*poller;
	/* [한국어] sock_group_poll을 호출하는 main poller. */
	struct spdk_poller				*nop_poller;
	/* [한국어] NOP-In keep-alive를 주기적으로 처리하는 보조 poller. */
	STAILQ_HEAD(connections, spdk_iscsi_conn)	connections;
	/* [한국어] 이 poll_group에 등록된 conn 리스트. */
	struct spdk_sock_group				*sock_group;
	/* [한국어] conn 소켓들이 등록되는 sock_group. */
	TAILQ_ENTRY(spdk_iscsi_poll_group)		link;
	/* [한국어] g_iscsi.poll_group_head 링크. */
	uint32_t					num_active_targets;
	/* [한국어] 이 group이 폴링하는 활성 target 수 (load balance 결정에 사용). */
};

struct spdk_iscsi_opts {
	/* [한국어] 부팅/RPC 시 한 번 설정되는 글로벌 옵션. spdk_iscsi_init에 전달. */

	char *authfile;
	/* [한국어] 인증 정보 conf 파일 경로 (옵션). */
	char *nodebase;
	/* [한국어] 자동 생성 IQN의 base ("iqn.2016-06.io.spdk" 등). */
	int32_t timeout;
	/* [한국어] 기본 inactive timeout. */
	int32_t nopininterval;
	/* [한국어] NOP-In 자동 송신 주기. */
	bool disable_chap;
	/* [한국어] 글로벌 CHAP 비활성. */
	bool require_chap;
	/* [한국어] 글로벌 CHAP 강제. */
	bool mutual_chap;
	/* [한국어] 글로벌 mutual CHAP 강제. */
	int32_t chap_group;
	/* [한국어] 글로벌 기본 auth_group tag. */
	uint32_t MaxSessions;
	/* [한국어] 시스템 전체 동시 세션 한도. */
	uint32_t MaxConnectionsPerSession;
	/* [한국어] 세션당 conn 한도. */
	uint32_t MaxConnections;
	/* [한국어] 시스템 전체 conn 한도. */
	uint32_t MaxQueueDepth;
	/* [한국어] 연결당 SCSI 큐 깊이. */
	uint32_t DefaultTime2Wait;
	/* [한국어] 기본 협상값. */
	uint32_t DefaultTime2Retain;
	/* [한국어] 기본 협상값. */
	uint32_t FirstBurstLength;
	/* [한국어] 기본 협상값. */
	bool ImmediateData;
	/* [한국어] 기본 협상값. */
	uint32_t ErrorRecoveryLevel;
	/* [한국어] 기본 ERL. */
	bool AllowDuplicateIsid;
	/* [한국어] 동일 ISID로 다중 로그인 허용 여부. */
	uint32_t MaxLargeDataInPerConnection;
	/* [한국어] 큰 DataIn 동시 처리 한도. */
	uint32_t MaxR2TPerConnection;
	/* [한국어] 연결당 미완 R2T 한도. */
	uint32_t pdu_pool_size;
	/* [한국어] PDU mempool 크기. */
	uint32_t immediate_data_pool_size;
	/* [한국어] immediate data mempool 크기. */
	uint32_t data_out_pool_size;
	/* [한국어] data-out mempool 크기. */
};

struct spdk_iscsi_globals {
	/* [한국어] iSCSI 타깃 전역 상태. extern struct spdk_iscsi_globals g_iscsi 1개 인스턴스. */

	char *authfile;
	/* [한국어] auth 파일 경로 (opts에서 복사). */
	char *nodebase;
	/* [한국어] IQN base. */
	pthread_mutex_t mutex;
	/* [한국어] 전역 자료구조 변경(target/IG/PG/auth_group/poll_group) 직렬화 락.
	 * 잡는 곳: 거의 모든 RPC 핸들러와 일부 셧다운 경로.
	 * 동기화: pthread_mutex. */
	uint32_t refcnt;
	/* [한국어] 외부 모듈이 SPDK iSCSI를 hold하는 카운트 (예: vhost-scsi). 0이 되면 fini 가능. */
	TAILQ_HEAD(, spdk_iscsi_portal)		portal_head;
	/* [한국어] 등록된 모든 portal. */
	TAILQ_HEAD(, spdk_iscsi_portal_grp)	pg_head;
	/* [한국어] 등록된 모든 PG. */
	TAILQ_HEAD(, spdk_iscsi_init_grp)	ig_head;
	/* [한국어] 등록된 모든 IG. */
	TAILQ_HEAD(, spdk_iscsi_tgt_node)	target_head;
	/* [한국어] 등록된 모든 target_node. */
	TAILQ_HEAD(, spdk_iscsi_auth_group)	auth_group_head;
	/* [한국어] 등록된 모든 auth_group. */
	TAILQ_HEAD(, spdk_iscsi_poll_group)	poll_group_head;
	/* [한국어] 활성 poll_group 리스트 (각 SPDK thread에 1개). */

	int32_t timeout;
	int32_t nopininterval;
	bool disable_chap;
	bool require_chap;
	bool mutual_chap;
	int32_t chap_group;
	/* [한국어] opts와 동일한 글로벌 정책 값들 (런타임 시 PG/target 생성 기본값). */

	uint32_t MaxSessions;
	uint32_t MaxConnectionsPerSession;
	uint32_t MaxConnections;
	uint32_t MaxQueueDepth;
	uint32_t DefaultTime2Wait;
	uint32_t DefaultTime2Retain;
	uint32_t FirstBurstLength;
	bool ImmediateData;
	uint32_t ErrorRecoveryLevel;
	bool AllowDuplicateIsid;
	uint32_t MaxLargeDataInPerConnection;
	uint32_t MaxR2TPerConnection;
	uint32_t pdu_pool_size;
	uint32_t immediate_data_pool_size;
	uint32_t data_out_pool_size;
	/* [한국어] opts에서 복사된 글로벌 한도/기본값들. 변경 시 mutex 보호. */

	struct spdk_mempool *pdu_pool;
	/* [한국어] PDU 객체 mempool (MAX_ISCSI_CONNECTIONS × queue_depth 등 기준 크기). */
	struct spdk_mempool *pdu_immediate_data_pool;
	/* [한국어] immediate data 버퍼 mempool. */
	struct spdk_mempool *pdu_data_out_pool;
	/* [한국어] Data-Out 누적 버퍼 mempool. */
	struct spdk_mempool *session_pool;
	/* [한국어] 세션 객체 mempool. */
	struct spdk_mempool *task_pool;
	/* [한국어] task 객체 mempool. */

	struct spdk_iscsi_sess	**session;
	/* [한국어] tsih → 세션 룩업 테이블 (인덱스 기반). */
};

#define ISCSI_SECURITY_NEGOTIATION_PHASE	0
/* [한국어] 로그인 phase 0: SecurityNegotiation (CHAP 등). RFC 3720 §5.3.2. */
#define ISCSI_OPERATIONAL_NEGOTIATION_PHASE	1
/* [한국어] phase 1: OperationalNegotiation (전송 파라미터). */
#define ISCSI_NSG_RESERVED_CODE			2
/* [한국어] NSG reserved (사용 안 됨). */
#define ISCSI_FULL_FEATURE_PHASE		3
/* [한국어] phase 3: FullFeature (정상 I/O). */

/* logout reason */
#define ISCSI_LOGOUT_REASON_CLOSE_SESSION		0
/* [한국어] 세션 전체 종료. */
#define ISCSI_LOGOUT_REASON_CLOSE_CONNECTION		1
/* [한국어] 단일 conn만 종료 (세션 유지). */
#define ISCSI_LOGOUT_REASON_REMOVE_CONN_FOR_RECOVERY	2
/* [한국어] 회복용 conn 제거 (ERL2+). */

enum spdk_error_codes {
	/* [한국어] 내부 에러 코드 (SPDK iSCSI 전용). */
	SPDK_ISCSI_CONNECTION_FATAL	= -1,
	/* [한국어] 연결 회복 불가 — destruct 진행. */
	SPDK_PDU_FATAL		= -2,
	/* [한국어] 현재 PDU만 폐기 (연결은 유지 가능). */
};

#define DGET24(B)											\
	(((  (uint32_t) *((uint8_t *)(B)+0)) << 16)				\
	 | (((uint32_t) *((uint8_t *)(B)+1)) << 8)				\
	 | (((uint32_t) *((uint8_t *)(B)+2)) << 0))
/* [한국어] 24-bit big-endian 정수를 byte pointer (B)에서 추출하여 uint32_t로 반환.
 * iSCSI BHS의 Data Segment Length(=24bit) 등에 사용. RFC 3720 PDU 와이어 포맷. */

#define DSET24(B,D)													\
	(((*((uint8_t *)(B)+0)) = (uint8_t)((uint32_t)(D) >> 16)),		\
	 ((*((uint8_t *)(B)+1)) = (uint8_t)((uint32_t)(D) >> 8)),		\
	 ((*((uint8_t *)(B)+2)) = (uint8_t)((uint32_t)(D) >> 0)))
/* [한국어] DGET24의 역 — uint32_t 값 D의 하위 24비트를 (B+0..B+2)에 big-endian 저장. */

#define xstrdup(s) (s ? strdup(s) : (char *)NULL)
/* [한국어] strdup의 NULL-safe wrapper. NULL 입력은 NULL 반환. */

extern struct spdk_iscsi_globals g_iscsi;
/* [한국어] 전역 iSCSI 상태 인스턴스. 정의는 lib/iscsi/iscsi.c. */
extern struct spdk_iscsi_opts *g_spdk_iscsi_opts;
/* [한국어] init 단계에서 사용하는 옵션 포인터 (init 후 free 또는 g_iscsi에 복사됨). */

struct spdk_iscsi_task;
/* [한국어] forward — task.h 의존 회피. */
struct spdk_json_write_ctx;
/* [한국어] forward — json.h 의존 회피. */

typedef void (*spdk_iscsi_init_cb)(void *cb_arg, int rc);
/* [한국어] spdk_iscsi_init 비동기 완료 콜백 시그니처. */

void spdk_iscsi_init(spdk_iscsi_init_cb cb_fn, void *cb_arg);
/* [한국어] iSCSI 서브시스템 비동기 초기화 (mempool, poll_group, RPC 등록 등).
 * 완료 시 cb_fn(cb_arg, rc)를 메인 thread에서 호출. */
typedef void (*spdk_iscsi_fini_cb)(void *arg);
/* [한국어] fini 완료 콜백. */
void spdk_iscsi_fini(spdk_iscsi_fini_cb cb_fn, void *cb_arg);
/* [한국어] 모든 conn/target/PG/IG 정리 후 mempool 해제, 콜백 호출. */
void shutdown_iscsi_conns_done(void);
/* [한국어] shutdown_iscsi_conns 비동기 종료 시 fini 진행을 깨우는 신호. */
void spdk_iscsi_config_json(struct spdk_json_write_ctx *w);
/* [한국어] iSCSI 전체 구성을 save_config 형태로 JSON 출력. */

struct spdk_iscsi_opts *iscsi_opts_alloc(void);
/* [한국어] 기본값 채워진 opts 새 객체 할당. */
void iscsi_opts_free(struct spdk_iscsi_opts *opts);
/* [한국어] opts free (authfile/nodebase 문자열 포함). */
struct spdk_iscsi_opts *iscsi_opts_copy(struct spdk_iscsi_opts *src);
/* [한국어] opts deep copy (문자열 strdup 포함). */
void iscsi_opts_info_json(struct spdk_json_write_ctx *w);
/* [한국어] 현재 opts를 JSON으로 출력. */
int iscsi_set_discovery_auth(bool disable_chap, bool require_chap,
			     bool mutual_chap, int32_t chap_group);
/* [한국어] Discovery 세션의 인증 정책 설정 (RPC iscsi_set_discovery_auth). */
int iscsi_chap_get_authinfo(struct iscsi_chap_auth *auth, const char *authuser,
			    int ag_tag);
/* [한국어] auth_group(tag=ag_tag)에서 authuser 검색, secret을 auth->secret으로 채움. */
int iscsi_add_auth_group(int32_t tag, struct spdk_iscsi_auth_group **_group);
/* [한국어] tag로 새 auth_group 생성·등록. 반환 group은 *_group으로. */
struct spdk_iscsi_auth_group *iscsi_find_auth_group_by_tag(int32_t tag);
/* [한국어] tag로 auth_group 검색. */
void iscsi_delete_auth_group(struct spdk_iscsi_auth_group *group);
/* [한국어] auth_group과 그 secret 전부 free. */
int iscsi_auth_group_add_secret(struct spdk_iscsi_auth_group *group,
				const char *user, const char *secret,
				const char *muser, const char *msecret);
/* [한국어] secret 한 개 추가 (사용자 중복 시 -EEXIST). */
int iscsi_auth_group_delete_secret(struct spdk_iscsi_auth_group *group,
				   const char *user);
/* [한국어] 사용자명으로 secret 제거. */
void iscsi_auth_groups_info_json(struct spdk_json_write_ctx *w);
/* [한국어] 모든 auth_group 정보를 JSON 출력. */

void iscsi_task_response(struct spdk_iscsi_conn *conn,
			 struct spdk_iscsi_task *task);
/* [한국어] task 완료 시 SCSI Response PDU 작성·송신. */
int iscsi_build_iovs(struct spdk_iscsi_conn *conn, struct iovec *iovs, int iovcnt,
		     struct spdk_iscsi_pdu *pdu, uint32_t *mapped_length);
/* [한국어] PDU의 모든 segment(BHS+AHS+HDigest+Data+DDigest)를 iovec 배열에 매핑.
 * 반환=사용된 iovec 수, *mapped_length=총 매핑 바이트. */
int iscsi_handle_incoming_pdus(struct spdk_iscsi_conn *conn);
/* [한국어] conn에서 가능한 모든 수신 PDU를 처리. burst recv→opcode 디스패치 루프.
 * 반환: 처리한 PDU 수 또는 음수 에러. */
void iscsi_task_mgmt_response(struct spdk_iscsi_conn *conn,
			      struct spdk_iscsi_task *task);
/* [한국어] Task Management Function 응답 PDU 작성·송신. */

void iscsi_free_sess(struct spdk_iscsi_sess *sess);
/* [한국어] 세션 객체 free (conn 모두 해제된 상태에서 호출). */
void iscsi_clear_all_transfer_task(struct spdk_iscsi_conn *conn,
				   struct spdk_scsi_lun *lun,
				   struct spdk_iscsi_pdu *pdu);
/* [한국어] LUN reset/abort 시 진행 중 transfer task 모두 정리. */
bool iscsi_del_transfer_task(struct spdk_iscsi_conn *conn, uint32_t CmdSN);
/* [한국어] 특정 CmdSN의 transfer task 1개를 강제 제거 (Task Mgmt). */

uint32_t iscsi_pdu_calc_header_digest(struct spdk_iscsi_pdu *pdu);
/* [한국어] BHS+AHS의 CRC32C 계산 (RFC 3720 §10.2.1.7 HeaderDigest). */
uint32_t iscsi_pdu_calc_data_digest(struct spdk_iscsi_pdu *pdu);
/* [한국어] Data Segment의 CRC32C 계산. */

/* Memory management */
/* [한국어] === PDU/task mempool 관련 헬퍼들 === */
void iscsi_put_pdu(struct spdk_iscsi_pdu *pdu);
/* [한국어] PDU ref-- ; 0이면 mobj 반환 + mempool로 PDU 객체 반환. */
struct spdk_iscsi_pdu *iscsi_get_pdu(struct spdk_iscsi_conn *conn);
/* [한국어] mempool에서 PDU 1개 할당 + 초기화. ref=1로 시작. */
void iscsi_op_abort_task_set(struct spdk_iscsi_task *task,
			     uint8_t function);
/* [한국어] ABORT TASK SET 등 Task Mgmt 함수 처리 entry. */
void iscsi_queue_task(struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task);
/* [한국어] task를 SCSI 레이어에 제출 (spdk_scsi_dev_queue_task). */

/*
 * [한국어]
 * iscsi_datapool_get - data mempool에서 mobj 1개 할당.
 *
 * @pool: pdu_data_out_pool 또는 pdu_immediate_data_pool.
 * @return: 할당된 mobj 또는 NULL(고갈).
 */
static inline struct spdk_mobj *
iscsi_datapool_get(struct spdk_mempool *pool)
{
	return spdk_mempool_get(pool);
	/* [한국어] DPDK rte_mempool_get wrapper. lockless ring 기반 → fast path. */
}

/*
 * [한국어]
 * iscsi_datapool_put - mobj를 mempool에 반환.
 *
 * @mobj: 반환할 mobj (NULL 불가). data_len을 0으로 리셋한다.
 */
static inline void
iscsi_datapool_put(struct spdk_mobj *mobj)
{
	assert(mobj != NULL);
	/* [한국어] caller invariant. */

	mobj->data_len = 0;
	/* [한국어] 다음 사용자가 stale 길이를 보지 않도록 0으로 리셋. */
	spdk_mempool_put(mobj->mp, (void *)mobj);
	/* [한국어] mobj는 자신이 속한 mempool을 mp 필드로 기억 — 거기에 반환. */
}

/*
 * [한국어]
 * iscsi_get_max_immediate_data_size - 한 PDU에 immediate data로 들어갈 수 있는 worst-case 크기.
 *
 * @return: 워스트 케이스 바이트 수 (FirstBurstLength + digest + AHS 여유).
 *
 * 본 값으로 immediate_data mempool buffer 크기를 결정.
 */
static inline uint32_t
iscsi_get_max_immediate_data_size(void)
{
	/*
	 * Specify enough extra space in addition to FirstBurstLength to
	 *  account for a header digest, data digest and additional header
	 *  segments (AHS).  These are not normally used but they do not
	 *  take up much space and we need to make sure the worst-case scenario
	 *  can be satisfied by the size returned here.
	 */
	return g_iscsi.FirstBurstLength +
	       ISCSI_DIGEST_LEN + /* data digest */
	       ISCSI_DIGEST_LEN + /* header digest */
	       8 +		   /* bidirectional AHS */
	       52;		   /* extended CDB AHS (for a 64-byte CDB) */
	/* [한국어] FirstBurstLength + 4 + 4 + 8 + 52. ISCSI_DIGEST_LEN=4(CRC32C). 합계는 PDU
	 * 한 개 worst-case 데이터 영역 크기. mempool buf 크기 결정의 기준. */
}

#endif /* SPDK_ISCSI_H */
/* [한국어] include guard 종료. */
