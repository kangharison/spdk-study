/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI 연결(conn) 라이프사이클 관리 모듈 (conn.c)
 *
 * === 파일의 역할 ===
 * iSCSI 타깃에서 한 개의 TCP 연결(=spdk_iscsi_conn)을 생성·실행·종료하기 위한
 * 모든 라이프사이클 로직을 담는 파일이다. 클라이언트(initiator)가 portal에 TCP로
 * 접속하면 spdk_iscsi_portal accept 경로에서 iscsi_conn_construct() 가 호출되어
 * 연결이 생성되고, 이후 sock_group의 epoll/poll 콜백(iscsi_conn_sock_cb)이 이
 * 연결의 PDU 수신 처리를 담당한다. 연결이 종료되거나 logout/timeout 등의 이유로
 * 더 이상 사용되지 않으면 iscsi_conn_destruct() 경로를 통해 LUN/세션/태스크 자원을
 * 모두 회수하고 free pool로 되돌려진다. 또한 NOPIN keepalive 송신, 큐된 DataIN
 * 태스크의 분할/취소 등 연결 단위 부가 기능도 이곳에 위치한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   accept thread (lib/iscsi/portal.c::portal_accept)
 *     → iscsi_conn_construct(portal, sock)        ; 연결 생성/free pool에서 할당
 *       → spdk_thread_send_msg(pg_thread, iscsi_conn_start, conn)  ; poll group 스레드로 이관
 *         → iscsi_poll_group_add_conn(pg, conn)   ; sock_group_add_sock + recv 콜백 등록
 *
 *   poll group thread (lib/iscsi/iscsi_subsystem.c::iscsi_poll_group_poll)
 *     → spdk_sock_group_poll → iscsi_conn_sock_cb(conn)
 *       → iscsi_handle_incoming_pdus()  ; lib/iscsi/iscsi.c 의 PDU 디코더 호출
 *         → SCSI cmd → iscsi_queue_task → bdev I/O 제출
 *           → completion → iscsi_task_cpl(scsi_task) → iscsi_conn_write_pdu(rsp)
 *
 * 실행 컨텍스트:
 *   - allocate_conn / free_conn / iscsi_drop_conns 등 글로벌 풀 조작은 g_conns_mutex 보호.
 *   - iscsi_conn_construct() 는 acceptor 스레드(보통 main reactor)에서 호출.
 *   - iscsi_conn_start 이후의 거의 모든 함수(특히 *_cpl, *_sock_cb)는 이 conn 이 배정된
 *     poll group 의 spdk_thread 컨텍스트에서만 실행되어야 한다 (thread affinity).
 *   - 다른 스레드에서 conn 상태를 바꾸어야 할 때는 spdk_thread_send_msg() 로 메시지 전달.
 *
 * === 타 모듈과의 연결 ===
 * - lib/iscsi/iscsi.c        : PDU 디코딩/디스패치(iscsi_handle_incoming_pdus 등) 진입점.
 * - lib/iscsi/task.c         : iscsi_task_get/put, iscsi_queue_task, iscsi_task_response 등.
 * - lib/iscsi/portal_grp.c   : 타깃이 listen 중인 portal/portal_group 메타데이터.
 * - lib/iscsi/tgt_node.c     : target node, LUN 매핑, target->pg 스케줄링.
 * - lib/iscsi/param.c        : iscsi_param_* (login key/value 협상).
 * - include/spdk/sock.h      : spdk_sock / spdk_sock_group (TCP 추상화).
 * - include/spdk/scsi.h      : spdk_scsi_lun_open/close, spdk_scsi_task (LUN 핫리무브 처리).
 * - include/spdk/thread.h    : SPDK_POLLER_REGISTER, spdk_thread_send_msg 등.
 *
 * 핵심 공유 자료구조:
 *   - g_conns_array        : 부팅 시 calloc 으로 만든 MAX_ISCSI_CONNECTIONS 크기 풀.
 *   - g_free_conns         : 사용되지 않는 conn 의 free list (TAILQ).
 *   - g_active_conns       : 현재 사용 중인 conn 의 active list (TAILQ).
 *   - g_conns_mutex        : g_free_conns / g_active_conns / conn->is_valid 보호.
 *   - struct spdk_iscsi_conn 의 필드들은 conn 이 배정된 poll group 스레드에서만 만져야 함.
 *
 * === 주요 함수/구조체 요약 ===
 *  - allocate_conn()/_free_conn()        : free 풀에서 conn 슬롯 획득/반납.
 *  - initialize_iscsi_conns()            : 부팅 시 풀 초기화.
 *  - iscsi_conn_construct()              : accept 직후 연결 컨텍스트 만들고 poll group에 배치.
 *  - iscsi_conn_start()/_sock_cb()       : poll group 스레드에서 실제 PDU 수신 처리 시작.
 *  - iscsi_conn_destruct()/_iscsi_conn_destruct() : graceful shutdown 진입.
 *  - iscsi_conn_stop()/_iscsi_conn_check_shutdown() : 모든 inflight 작업 정리 후 free.
 *  - iscsi_conn_write_pdu()              : 송신 큐에 PDU 삽입 + spdk_sock_writev_async.
 *  - iscsi_conn_handle_nop()/iscsi_conn_send_nopin() : keepalive (NOP-In) 관리.
 *  - iscsi_conn_schedule()/_full_feature_migrate()   : Full Feature Phase 진입 후 가장 한가한
 *                                          poll group 으로 conn 이전(load balancing).
 *  - iscsi_conn_handle_queued_datain_tasks() : 대용량 read 응답을 분할 송신.
 *  - shutdown_iscsi_conns()/iscsi_conn_check_shutdown() : 데몬 종료 시 모든 conn logout.
 */

#include "spdk/stdinc.h"                           /* [한국어] SPDK 표준 헤더(stdint, string, errno 등) — POSIX 의존성 추상화. */

#include "spdk/endian.h"                           /* [한국어] from_be32/to_be32 등 엔디안 변환 — iSCSI BHS 의 빅엔디안 필드 처리에 필요. */
#include "spdk/env.h"                              /* [한국어] spdk_get_ticks/spdk_get_ticks_hz — TSC(Time Stamp Counter) 기반 시간. */
#include "spdk/likely.h"                           /* [한국어] spdk_likely/spdk_unlikely — branch prediction hint. */
#include "spdk/thread.h"                           /* [한국어] SPDK_POLLER_REGISTER, spdk_thread_send_msg, io_channel API. */
#include "spdk/queue.h"                            /* [한국어] TAILQ_/STAILQ_ 매크로 — lock 없는 단일 스레드용 연결 리스트. */
#include "spdk/trace.h"                            /* [한국어] spdk_trace_record — 저오버헤드 트레이스 포인트(분석용). */
#include "spdk/sock.h"                             /* [한국어] spdk_sock/spdk_sock_group — TCP/uring 추상화 (kernel/uring/posix 백엔드). */
#include "spdk/string.h"                           /* [한국어] spdk_strerror 등 문자열 유틸. */

#include "spdk/log.h"                              /* [한국어] SPDK_ERRLOG/SPDK_DEBUGLOG — 로깅 매크로. */

#include "iscsi/task.h"                            /* [한국어] spdk_iscsi_task / iscsi_task_get/put, iscsi_queue_task. */
#include "iscsi/conn.h"                            /* [한국어] spdk_iscsi_conn 구조체 정의 + 본 파일의 외부 API 선언. */
#include "iscsi/tgt_node.h"                        /* [한국어] spdk_iscsi_tgt_node, iscsi_tgt_node_cleanup_luns 등. */
#include "iscsi/portal_grp.h"                      /* [한국어] spdk_iscsi_portal/portal_grp — listen 정보. */

/* [한국어] CRC32C 4바이트 워드를 little-endian 바이트 순으로 BUF 에 기록.
 * iSCSI Header/Data Digest 필드는 PDU 직후/데이터 직후에 4바이트로 직렬화되며,
 * RFC3720 §10.2.3 (HeaderDigest) / §10.2.4 (DataDigest) 에 따라 wire 포맷은 LSB-first 다.
 * 매크로로 4번 캐스팅·시프트를 펼쳐 컴파일러가 인라인으로 처리하도록 한다. */
#define MAKE_DIGEST_WORD(BUF, CRC32C) \
        (   ((*((uint8_t *)(BUF)+0)) = (uint8_t)((uint32_t)(CRC32C) >> 0)), \
            ((*((uint8_t *)(BUF)+1)) = (uint8_t)((uint32_t)(CRC32C) >> 8)), \
            ((*((uint8_t *)(BUF)+2)) = (uint8_t)((uint32_t)(CRC32C) >> 16)), \
            ((*((uint8_t *)(BUF)+3)) = (uint8_t)((uint32_t)(CRC32C) >> 24)))

/* [한국어] conn 구조체 재사용 시 portal 필드 이후를 0 으로 초기화하기 위한 헬퍼.
 * portal 보다 앞에 있는 필드 (예: id, conn_link)는 풀 슬롯 식별자/링크라서 보존해야 한다.
 * allocate_conn() 에서 사용되며, 이전 사용자가 남긴 잔여 상태를 모두 지운다. */
#define SPDK_ISCSI_CONNECTION_MEMSET(conn)		\
	memset(&(conn)->portal, 0, sizeof(*(conn)) -	\
		offsetof(struct spdk_iscsi_conn, portal));

/* [한국어] enum→문자열 변환을 짧게 쓰기 위한 매크로 — switch case 안에서 한 줄 변환. */
#define SPDK_ISCSI_CONNECTION_STATUS(status, rnstr) case(status): return(rnstr)

/* [한국어] 모든 conn 슬롯을 담는 정적 배열. initialize_iscsi_conns() 에서 calloc 으로 할당.
 * 설정자: initialize_iscsi_conns()
 * 읽는 자: allocate_conn(), iscsi_get_active_conns(), iscsi_drop_conns(), shutdown_iscsi_conns().
 * 동기화: g_conns_mutex 보호 (alloc/free, drop 시).
 * 값 범위: NULL(미초기화) 또는 MAX_ISCSI_CONNECTIONS 만큼의 conn 배열. */
static struct spdk_iscsi_conn *g_conns_array = NULL;

/* [한국어] 사용 가능한 conn 슬롯의 free list. allocate_conn() 이 head 에서 꺼낸다. */
static TAILQ_HEAD(, spdk_iscsi_conn) g_free_conns = TAILQ_HEAD_INITIALIZER(g_free_conns);
/* [한국어] 현재 사용 중인 conn 슬롯의 active list. shutdown/drop 시 순회 대상. */
static TAILQ_HEAD(, spdk_iscsi_conn) g_active_conns = TAILQ_HEAD_INITIALIZER(g_active_conns);

/* [한국어] 두 글로벌 리스트와 conn->is_valid 를 보호하는 뮤텍스.
 * 여러 스레드(acceptor + 다수 poll group 스레드 + RPC 스레드)가 alloc/free 가능하므로
 * 글로벌 풀 단위에서 lock 이 필요. (단, conn 내부 필드는 owning 스레드 단일 접근 원칙) */
static pthread_mutex_t g_conns_mutex = PTHREAD_MUTEX_INITIALIZER;

/* [한국어] 데몬 종료(shutdown_iscsi_conns) 시 모든 conn 이 정리되었는지 1ms 마다 확인하는 폴러. */
static struct spdk_poller *g_shutdown_timer = NULL;

/* [한국어] 전방 선언 — 소켓 그룹에 등록되는 recv 콜백. epoll/uring 이벤트 발생 시 호출됨. */
static void iscsi_conn_sock_cb(void *arg, struct spdk_sock_group *group,
			       struct spdk_sock *sock);

/*
 * [한국어]
 * allocate_conn - free 풀에서 미사용 conn 슬롯을 가져와 active 리스트로 옮긴다.
 *
 * @return: 할당된 conn 포인터(메모리는 0 으로 초기화됨), 풀이 비었으면 NULL.
 *
 * iSCSI 동시 연결 수를 컴파일 타임 상수(MAX_ISCSI_CONNECTIONS)로 제한하므로
 * malloc 대신 정적 풀에서 슬롯을 꺼낸다. (메모리 단편화 회피, 슬롯 ID 안정화)
 *
 * 실행 컨텍스트:
 *   - 주로 acceptor 스레드(iscsi_conn_construct 경로)에서 호출되지만,
 *     다른 컨텍스트에서도 안전하도록 g_conns_mutex 로 보호된다.
 *
 * 호출 체인:
 *   iscsi_conn_construct() → allocate_conn()
 */
static struct spdk_iscsi_conn *
allocate_conn(void)
{
	struct spdk_iscsi_conn	*conn;                  /* [한국어] 반환할 conn 포인터(없으면 NULL). */

	pthread_mutex_lock(&g_conns_mutex);             /* [한국어] 글로벌 풀 보호 — 다른 스레드의 alloc/free/drop 와 직렬화. */
	conn = TAILQ_FIRST(&g_free_conns);              /* [한국어] free 리스트의 헤드 슬롯을 후보로 선택. */
	if (conn != NULL) {                             /* [한국어] 풀에 여유가 있는 경우만 진행. */
		assert(!conn->is_valid);                /* [한국어] free 리스트에 있는 슬롯은 항상 is_valid=0 이어야 함(불변식). */
		TAILQ_REMOVE(&g_free_conns, conn, conn_link); /* [한국어] free 리스트에서 떼어낸다. */
		SPDK_ISCSI_CONNECTION_MEMSET(conn);     /* [한국어] portal 이후 모든 필드 0 클리어 — 이전 사용자 잔여상태 제거. */
		conn->is_valid = 1;                     /* [한국어] 사용 중 표시 — RPC 등에서 풀을 순회할 때 유효 슬롯 식별 기준. */

		TAILQ_INSERT_TAIL(&g_active_conns, conn, conn_link); /* [한국어] active 리스트 끝에 등록. */
	}
	pthread_mutex_unlock(&g_conns_mutex);           /* [한국어] 글로벌 풀 락 해제. */

	return conn;                                    /* [한국어] 호출자에게 슬롯 반환(또는 NULL). */
}

/*
 * [한국어]
 * _free_conn - 락이 이미 잡힌 상태에서 conn 슬롯을 active → free 풀로 되돌린다.
 *
 * @conn: 회수할 conn (이미 모든 inflight 자원이 정리되어 있어야 함).
 *
 * iscsi_conn_free()/iscsi_conn_construct error path 에서 호출되는 내부 헬퍼.
 * g_conns_mutex 가 잡혀있다고 가정하므로 직접 호출 시 주의.
 */
static void
_free_conn(struct spdk_iscsi_conn *conn)
{
	TAILQ_REMOVE(&g_active_conns, conn, conn_link); /* [한국어] active 리스트에서 제거. */

	memset(conn->portal_host, 0, sizeof(conn->portal_host)); /* [한국어] RPC dump 시 흔적 남지 않도록 IP/host 문자열 클리어. */
	memset(conn->portal_port, 0, sizeof(conn->portal_port)); /* [한국어] 포트 문자열도 클리어. */
	conn->is_valid = 0;                             /* [한국어] 슬롯 상태를 미사용으로 전환 — RPC 가 이 슬롯을 무시하도록. */

	TAILQ_INSERT_TAIL(&g_free_conns, conn, conn_link); /* [한국어] free 리스트 끝에 반환. */
}

/*
 * [한국어]
 * free_conn - _free_conn 의 락 wrapping 버전. 외부에서 락 없이 호출할 때 사용.
 *
 * @conn: 회수할 conn.
 *
 * 호출 체인:
 *   iscsi_conn_construct() error path → free_conn()
 */
static void
free_conn(struct spdk_iscsi_conn *conn)
{
	pthread_mutex_lock(&g_conns_mutex);             /* [한국어] 글로벌 풀 락 획득. */
	_free_conn(conn);                               /* [한국어] 실제 회수 작업 위임. */
	pthread_mutex_unlock(&g_conns_mutex);           /* [한국어] 락 해제. */
}

/*
 * [한국어]
 * _iscsi_conns_cleanup - 데몬 종료 시 conn 풀 메모리를 free 한다.
 *
 * shutdown 완료 콜백(iscsi_conn_check_shutdown_cb)에서 호출되며,
 * 모든 conn 이 EXITED 후 free 풀로 돌아온 시점에서만 안전하다.
 */
static void
_iscsi_conns_cleanup(void)
{
	free(g_conns_array);                            /* [한국어] calloc 한 풀 배열 해제 — 이후 g_conns_array 는 dangling. */
}

/*
 * [한국어]
 * initialize_iscsi_conns - iSCSI 서브시스템 초기화 시 conn 풀을 만든다.
 *
 * @return: 성공 0, 메모리 부족 시 -ENOMEM.
 *
 * 부팅 단계(iscsi_subsystem_init)에서 1회만 호출된다.
 * MAX_ISCSI_CONNECTIONS 개의 슬롯을 calloc 으로 한꺼번에 잡고, 모두 free list 에 넣어
 * 향후 accept 시 allocate_conn() 으로 바로 꺼낼 수 있게 한다.
 *
 * 호출 체인:
 *   spdk_subsystem_init → iscsi_subsystem_init → initialize_iscsi_conns()
 */
int
initialize_iscsi_conns(void)
{
	uint32_t i;                                     /* [한국어] 루프 인덱스(슬롯 번호). */

	SPDK_DEBUGLOG(iscsi, "spdk_iscsi_init\n");      /* [한국어] 디버그 로그 — iscsi 카테고리 활성화 시 출력. */

	g_conns_array = calloc(MAX_ISCSI_CONNECTIONS, sizeof(struct spdk_iscsi_conn)); /* [한국어] 0-초기화된 풀 배열 할당. */
	if (g_conns_array == NULL) {                    /* [한국어] 메모리 부족 시 초기화 실패. */
		return -ENOMEM;                         /* [한국어] 호출자가 subsystem 초기화 실패로 처리. */
	}

	for (i = 0; i < MAX_ISCSI_CONNECTIONS; i++) {   /* [한국어] 모든 슬롯에 ID 부여 후 free 리스트에 등록. */
		g_conns_array[i].id = i;                /* [한국어] 슬롯 인덱스를 conn->id 로 영구 고정 (RPC dump/추적용). */
		TAILQ_INSERT_TAIL(&g_free_conns, &g_conns_array[i], conn_link); /* [한국어] free 리스트에 순서대로 push. */
	}

	return 0;                                       /* [한국어] 성공 반환. */
}

/*
 * [한국어]
 * iscsi_poll_group_add_conn - 지정된 poll group(=spdk_thread)의 sock_group 에 conn 의 소켓을 등록한다.
 *
 * @pg:   conn 을 폴링할 poll group(spdk_iscsi_poll_group, 1코어=1pg).
 * @conn: 등록할 연결 컨텍스트.
 *
 * spdk_sock_group_add_sock() 은 epoll/uring/kernel poll 추상화 위에 conn 의 소켓을 등록하고
 * 데이터 수신 시 iscsi_conn_sock_cb 콜백이 호출되도록 한다. 이후 별도의 read poller 없이
 * 이벤트 기반으로 PDU 처리가 시작된다.
 *
 * 실행 컨텍스트:
 *   - 반드시 pg 가 속한 spdk_thread 에서 호출되어야 함 (sock_group 자료구조의 thread affinity).
 *   - 따라서 다른 스레드에서는 spdk_thread_send_msg() 로 우회 호출.
 */
static void
iscsi_poll_group_add_conn(struct spdk_iscsi_poll_group *pg, struct spdk_iscsi_conn *conn)
{
	int rc;                                         /* [한국어] sock_group 등록 결과(<0 이면 errno). */

	rc = spdk_sock_group_add_sock(pg->sock_group, conn->sock, iscsi_conn_sock_cb, conn); /* [한국어] epoll-style 등록 + 콜백 바인딩. */
	if (rc < 0) {                                   /* [한국어] 등록 실패 — 보통 EBADF/ENOMEM. */
		SPDK_ERRLOG("spdk_sock_group_add_sock() failed, sock=%p, conn=%p, rc %d: %s\n", conn->sock, conn,
			    rc, spdk_strerror(-rc));    /* [한국어] 에러 로그 후 연결 추가는 포기 (conn 은 호출자가 정리). */
		return;
	}

	conn->is_stopped = false;                       /* [한국어] poll 대상 활성 상태 표시 — full feature migration 등에서 참조. */
	STAILQ_INSERT_TAIL(&pg->connections, conn, pg_link); /* [한국어] pg 가 관리하는 conn 리스트에도 추가(통계/순회용). */
}

/*
 * [한국어]
 * iscsi_poll_group_remove_conn - poll group 의 sock_group 과 conn 리스트에서 연결을 분리한다.
 *
 * @pg:   소속 poll group.
 * @conn: 제거할 연결.
 *
 * 연결 종료(_iscsi_conn_destruct) 또는 다른 poll group 으로의 마이그레이션
 * (iscsi_conn_schedule) 시 호출. sock_group 등록 해제 후에는 더 이상 sock_cb 가 호출되지 않는다.
 *
 * 실행 컨텍스트:
 *   - pg 가 속한 spdk_thread 에서만 호출되어야 한다(같은 affinity 규칙).
 */
static void
iscsi_poll_group_remove_conn(struct spdk_iscsi_poll_group *pg, struct spdk_iscsi_conn *conn)
{
	int rc;                                         /* [한국어] sock_group_remove_sock 반환값. */

	assert(conn->sock != NULL);                     /* [한국어] 이미 닫힌 소켓을 두 번 제거하면 안 됨. */
	rc = spdk_sock_group_remove_sock(pg->sock_group, conn->sock); /* [한국어] epoll 에서 fd 해제 — 콜백 더 이상 호출 안됨. */
	if (rc < 0) {                                   /* [한국어] 실패해도 진행(이미 분리 상태일 수 있음). */
		SPDK_ERRLOG("spdk_sock_group_remove_sock() failed, sock=%p, conn=%p, rc %d: %s\n", conn->sock, conn,
			    rc, spdk_strerror(-rc));
	}

	conn->is_stopped = true;                        /* [한국어] poll 비활성화 표시. */
	STAILQ_REMOVE(&pg->connections, conn, spdk_iscsi_conn, pg_link); /* [한국어] pg conn 리스트에서 분리. */
}

/*
 * [한국어]
 * login_timeout - login phase 진입 후 ISCSI_LOGIN_TIMEOUT 초 내에 완료되지 못하면
 *                 연결을 강제 종료하는 폴러 콜백.
 *
 * @arg: spdk_iscsi_conn 포인터.
 * @return: SPDK_POLLER_BUSY (한 번 동작했음을 알림 — busy/idle 통계용).
 *
 * 악성 또는 응답 없는 initiator 가 login PDU 만 보낸 채 멈추는 상황에서
 * 풀 슬롯이 영원히 점유되는 것을 방지한다.
 *
 * 실행 컨텍스트: conn 이 배치된 poll group 의 spdk_thread.
 */
static int
login_timeout(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;             /* [한국어] poller 등록 시 넘긴 conn. */

	if (conn->state < ISCSI_CONN_STATE_EXITING) {   /* [한국어] 아직 종료 절차에 진입 안 한 경우만 강제 전이. */
		conn->state = ISCSI_CONN_STATE_EXITING; /* [한국어] EXITING 으로 전환 — 다음 sock_cb 가 destruct 진입. */
	}
	spdk_poller_unregister(&conn->login_timer);     /* [한국어] 자기 자신(폴러) 해제 — 1회성 타임아웃. */

	return SPDK_POLLER_BUSY;                        /* [한국어] poller framework 통계 — 의미 있는 작업 했음. */
}

/*
 * [한국어]
 * iscsi_conn_start - poll group 스레드에서 호출되는 연결 시작 메시지 핸들러.
 *
 * @ctx: spdk_iscsi_conn (메시지 페이로드).
 *
 * iscsi_conn_construct() 가 acceptor 스레드에서 spdk_thread_send_msg(...) 로 보내며,
 * 실제 sock_group 등록과 login 타이머 가동은 이 함수에서 수행한다 — sock_group 은
 * 그것이 속한 spdk_thread 에서만 조작 가능하기 때문이다.
 */
static void
iscsi_conn_start(void *ctx)
{
	struct spdk_iscsi_conn *conn = ctx;             /* [한국어] 메시지 페이로드 → conn 복원. */

	iscsi_poll_group_add_conn(conn->pg, conn);      /* [한국어] sock_group 에 등록 — 이후부터 PDU 수신 가능. */

	conn->login_timer = SPDK_POLLER_REGISTER(login_timeout, conn, ISCSI_LOGIN_TIMEOUT * 1000000); /* [한국어] login 단계 타임아웃 폴러 등록(초→마이크로초 변환). */
}

/*
 * [한국어]
 * iscsi_conn_construct - accept 직후 새 iSCSI 연결을 구성하고 poll group 에 배정한다.
 *
 * @portal: 이 연결이 들어온 listening portal (어떤 IP:port 가 accept 했는지 추적용).
 * @sock:   accept 로 만들어진 spdk_sock (이 함수가 소유권 인수).
 * @return: 성공 0, 실패 -1 (이 경우 sock 은 호출자가 close 책임).
 *
 * 단계:
 *  1) 풀에서 conn 슬롯 할당 (allocate_conn).
 *  2) 글로벌 g_iscsi 설정값 복사 (timeout, NOPIN 주기 등) — 보호: g_iscsi.mutex.
 *  3) portal 정보 / CHAP 정책 / sock 핸들 저장.
 *  4) state = INVALID, login_phase = SecurityNegotiation 으로 시작.
 *  5) 모든 PDU/태스크/LUN 큐 TAILQ 초기화.
 *  6) sock 의 peer/local 주소를 이름 문자열로 보관 (RPC dump/로그용).
 *  7) recv low watermark = 1 (헤더 1바이트만 와도 콜백 — 저지연).
 *  8) 첫 poll group 선택, conn->pg 저장, trace owner 등록.
 *  9) spdk_thread_send_msg 로 poll group 스레드에서 iscsi_conn_start 실행 의뢰.
 *
 * 실행 컨텍스트:
 *   - acceptor 스레드(보통 main reactor)에서 호출.
 *   - poll group 등록은 cross-thread 이므로 메시지 큐를 통해 비동기 위임.
 *
 * 호출 체인:
 *   portal_grp.c::portal_accept() → iscsi_conn_construct() → spdk_thread_send_msg(iscsi_conn_start)
 */
int
iscsi_conn_construct(struct spdk_iscsi_portal *portal,
		     struct spdk_sock *sock)
{
	struct spdk_iscsi_poll_group *pg;               /* [한국어] 이 연결을 배치할 poll group. */
	struct spdk_iscsi_conn *conn;                   /* [한국어] 새로 할당될 연결 슬롯. */
	int i, rc;                                      /* [한국어] 루프 인덱스 / 시스템콜 결과. */

	conn = allocate_conn();                         /* [한국어] free 풀에서 슬롯 한 개 획득. */
	if (conn == NULL) {                             /* [한국어] MAX_ISCSI_CONNECTIONS 초과 시 실패. */
		SPDK_ERRLOG("Could not allocate connection.\n");
		return -1;
	}

	pthread_mutex_lock(&g_iscsi.mutex);             /* [한국어] 글로벌 iSCSI 설정 읽기 — RPC 와 직렬화. */
	conn->timeout = g_iscsi.timeout * spdk_get_ticks_hz(); /* seconds to TSC */ /* [한국어] NOPIN 응답 timeout 기준 (TSC 단위로 미리 환산). */
	conn->nopininterval = g_iscsi.nopininterval;    /* [한국어] NOPIN 송신 간격(초). */
	conn->nopininterval *= spdk_get_ticks_hz(); /* seconds to TSC */ /* [한국어] 위 값을 TSC 단위로 환산 — spdk_get_ticks 비교에 직접 사용. */
	conn->last_nopin = spdk_get_ticks();            /* [한국어] 마지막 NOPIN 송수신 시각 — 첫 측정 기준. */
	conn->nop_outstanding = false;                  /* [한국어] 응답 대기 중인 NOPIN 없음. */
	conn->data_out_cnt = 0;                         /* [한국어] 진행 중 R2T → DataOUT 카운트(write 흐름). */
	conn->data_in_cnt = 0;                          /* [한국어] 진행 중 대용량 DataIN read 카운트(MaxLargeDataInPerConnection 제한용). */
	conn->disable_chap = portal->group->disable_chap; /* [한국어] CHAP 비활성 여부(portal group 정책). */
	conn->require_chap = portal->group->require_chap; /* [한국어] CHAP 필수 여부. */
	conn->mutual_chap = portal->group->mutual_chap; /* [한국어] 상호 CHAP 여부. */
	conn->chap_group = portal->group->chap_group;   /* [한국어] CHAP 인증 그룹 ID(account 매핑). */
	pthread_mutex_unlock(&g_iscsi.mutex);           /* [한국어] 설정 읽기 완료. */
	conn->MaxRecvDataSegmentLength = 8192; /* RFC3720(12.12) */ /* [한국어] login 협상 전 기본값 — 8KB (RFC3720 §12.12). */

	conn->portal = portal;                          /* [한국어] accept 한 portal 포인터 보관. */
	conn->pg_tag = portal->group->tag;              /* [한국어] portal group tag — target 의 portal_group 매칭 시 사용. */
	memcpy(conn->portal_host, portal->host, strlen(portal->host)); /* [한국어] listen IP 문자열 복사 (RPC dump용). */
	memcpy(conn->portal_port, portal->port, strlen(portal->port)); /* [한국어] listen 포트 문자열 복사. */
	conn->sock = sock;                              /* [한국어] sock 소유권 이전. close 는 destruct 경로에서 수행. */

	conn->state = ISCSI_CONN_STATE_INVALID;         /* [한국어] 초기 상태 — 아직 login 전. */
	conn->login_phase = ISCSI_SECURITY_NEGOTIATION_PHASE; /* [한국어] 처음은 보안 협상 단계 (RFC3720 §5.3). */
	conn->ttt = 0;                                  /* [한국어] Target Transfer Tag 초기값 — R2T 발급 시 ++. */

	conn->partial_text_parameter = NULL;            /* [한국어] Text PDU 분할 누적 버퍼 — 필요 시 동적 할당. */

	for (i = 0; i < MAX_CONNECTION_PARAMS; i++) {   /* [한국어] connection 파라미터 협상 완료 플래그 모두 false. */
		conn->conn_param_state_negotiated[i] = false;
	}

	for (i = 0; i < MAX_SESSION_PARAMS; i++) {      /* [한국어] session 파라미터 협상 완료 플래그 모두 false. */
		conn->sess_param_state_negotiated[i] = false;
	}

	conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_AWAIT_PDU_READY; /* [한국어] PDU 수신 FSM 시작 상태 — BHS 수신 대기. */

	TAILQ_INIT(&conn->write_pdu_list);              /* [한국어] 송신 대기 큐 — write_pdu 호출로 enqueue. */
	TAILQ_INIT(&conn->snack_pdu_list);              /* [한국어] ErrorRecoveryLevel>=1 시 재전송 대비 보관. */
	TAILQ_INIT(&conn->queued_r2t_tasks);            /* [한국어] R2T 보낼 차례 대기. */
	TAILQ_INIT(&conn->active_r2t_tasks);            /* [한국어] R2T 발급되어 DataOUT 기다리는 중. */
	TAILQ_INIT(&conn->queued_datain_tasks);         /* [한국어] 대용량 read response 분할 송신 큐. */
	TAILQ_INIT(&conn->luns);                        /* [한국어] full feature 진입 후 open 한 spdk_iscsi_lun 리스트. */

	rc = spdk_sock_getaddr(sock, conn->target_addr, sizeof conn->target_addr, NULL,
			       conn->initiator_addr, sizeof conn->initiator_addr, NULL); /* [한국어] sock 의 local/peer 주소 문자열 추출. */
	if (rc < 0) {                                   /* [한국어] getsockname/getpeername 실패. */
		SPDK_ERRLOG("spdk_sock_getaddr() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		goto error_return;
	}

	/* set low water mark */
	rc = spdk_sock_set_recvlowat(conn->sock, 1);    /* [한국어] SO_RCVLOWAT=1 — 1바이트만 와도 epoll readable, 저지연 PDU 처리. */
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_set_recvlowat() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		goto error_return;
	}

	/* set default params */
	rc = iscsi_conn_params_init(&conn->params);     /* [한국어] connection-scope 파라미터 기본값(키-값) 채우기. */
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_conn_params_init() failed\n");
		goto error_return;
	}
	conn->logout_request_timer = NULL;              /* [한국어] target → initiator 로 logout 요청 시 사용할 타이머. */
	conn->logout_timer = NULL;                      /* [한국어] initiator 의 logout 응답 대기 타이머. */
	conn->shutdown_timer = NULL;                    /* [한국어] inflight task drain 대기 타이머. */
	SPDK_DEBUGLOG(iscsi, "Launching connection on acceptor thread\n");
	conn->pending_task_cnt = 0;                     /* [한국어] inflight SCSI 태스크 수 — 0 이 되어야 free 가능. */

	/* Get the first poll group. */
	pg = TAILQ_FIRST(&g_iscsi.poll_group_head);     /* [한국어] 일단 첫 poll group 에 임시 배치(login 단계). */
	if (pg == NULL) {                               /* [한국어] poll group 이 하나도 없으면 치명적 — 부팅 버그. */
		SPDK_ERRLOG("There is no poll group.\n");
		assert(false);
		goto error_return;
	}

	conn->pg = pg;                                  /* [한국어] 현재 소속 poll group 설정. */
	conn->trace_id = spdk_trace_register_owner(OWNER_TYPE_ISCSI_CONN, conn->initiator_addr); /* [한국어] trace 시각화에서 이 conn 을 식별할 ID 발급. */
	spdk_thread_send_msg(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(pg)),
			     iscsi_conn_start, conn); /* [한국어] pg 스레드에서 iscsi_conn_start 실행 — 실제 sock 등록. */
	return 0;

error_return:
	iscsi_param_free(conn->params);                 /* [한국어] 부분 초기화된 파라미터 리스트 해제. */
	free_conn(conn);                                /* [한국어] 풀로 슬롯 반환 — sock 은 호출자가 close. */
	return -1;
}

/*
 * [한국어]
 * iscsi_conn_free_pdu - 송신/수신 PDU 의 자원을 해제하고 완료 콜백을 호출한다.
 *
 * @conn: 이 PDU 가 속한 연결.
 * @pdu : 해제할 PDU.
 *
 * PDU 생성 시 항상 cb_fn 이 등록되며(보통 iscsi_conn_pdu_generic_complete),
 * 여기서 task → put, pdu → put 순으로 정리한 뒤 cb_fn 을 호출한다.
 * cb_fn 호출 시점에는 pdu 메모리가 이미 풀로 반환된 뒤이므로 cb_arg 는 사전 캡처.
 *
 * 호출 컨텍스트: poll group 스레드 (write 완료 / 수신 에러 / drain 등).
 */
void
iscsi_conn_free_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	iscsi_conn_xfer_complete_cb cb_fn;              /* [한국어] 등록된 송신 완료 콜백 함수 포인터. */
	void *cb_arg;                                   /* [한국어] cb_fn 에 전달할 컨텍스트. */

	cb_fn = pdu->cb_fn;                             /* [한국어] 콜백 함수 캡처(pdu put 후에도 호출 가능하도록). */
	cb_arg = pdu->cb_arg;                           /* [한국어] 콜백 인자 캡처. */

	assert(cb_fn != NULL);                          /* [한국어] 모든 PDU 는 cb_fn 이 반드시 설정되어 있어야 함. */
	pdu->cb_fn = NULL;                              /* [한국어] 두 번 호출 방지(이중 free 가드). */

	if (pdu->task) {                                /* [한국어] 연관된 SCSI 태스크가 있으면 함께 해제. */
		iscsi_task_put(pdu->task);              /* [한국어] task 참조 카운트 감소. */
	}
	iscsi_put_pdu(pdu);                             /* [한국어] PDU 메모리 풀로 반환. */

	cb_fn(cb_arg);                                  /* [한국어] 등록된 후속 작업 실행 (예: write 완료 통지). */
}

/*
 * [한국어]
 * iscsi_conn_free_tasks - 연결에 남아있는 inflight PDU/태스크를 모두 정리한다.
 *
 * @conn: 종료 절차 중인 연결.
 * @return: 0 = 모두 정리됨(free 진행 가능), -1 = 아직 pending_task_cnt > 0 이라 미완료.
 *
 * 종료 순서가 중요하다:
 *   1) snack_pdu_list (재전송 대기) — 이미 flush 된 PDU 라 안전하게 free.
 *   2) queued_datain_tasks (대용량 read 분할 대기, 아직 큐잉 안된 것) — put.
 *   3) write_pdu_list (송신 큐) — free 마지막에 처리.
 *      이유: free 도중 iscsi_conn_handle_queued_datain_tasks() 가 호출될 수 있고
 *            이는 다시 write_pdu_list 에 PDU 를 추가하기 때문.
 *
 * 호출자는 -1 이면 shutdown_timer 등록하여 1ms 후 재시도.
 */
static int
iscsi_conn_free_tasks(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_pdu *pdu, *tmp_pdu;           /* [한국어] PDU 순회용 (SAFE 변형 사용). */
	struct spdk_iscsi_task *iscsi_task, *tmp_iscsi_task; /* [한국어] task 순회용. */

	TAILQ_FOREACH_SAFE(pdu, &conn->snack_pdu_list, tailq, tmp_pdu) { /* [한국어] (1) snack 리스트 정리. */
		TAILQ_REMOVE(&conn->snack_pdu_list, pdu, tailq); /* [한국어] 리스트에서 분리. */
		iscsi_conn_free_pdu(conn, pdu);          /* [한국어] PDU 와 연관 태스크 free. */
	}

	TAILQ_FOREACH_SAFE(iscsi_task, &conn->queued_datain_tasks, link, tmp_iscsi_task) { /* [한국어] (2) 분할 read 대기 태스크. */
		if (!iscsi_task->is_queued) {           /* [한국어] 이미 bdev 에 큐잉된 건 completion 이 알아서 정리하므로 skip. */
			TAILQ_REMOVE(&conn->queued_datain_tasks, iscsi_task, link);
			iscsi_task_put(iscsi_task);     /* [한국어] 아직 큐잉 안된 태스크만 즉시 put. */
		}
	}

	/* We have to parse conn->write_pdu_list in the end.  In iscsi_conn_free_pdu(),
	 *  iscsi_conn_handle_queued_datain_tasks() may be called, and
	 *  iscsi_conn_handle_queued_datain_tasks() will parse conn->queued_datain_tasks
	 *  and may stack some PDUs to conn->write_pdu_list.  Hence when we come here, we
	 *  have to ensure there is no associated task in conn->queued_datain_tasks.
	 */
	TAILQ_FOREACH_SAFE(pdu, &conn->write_pdu_list, tailq, tmp_pdu) { /* [한국어] (3) 송신 큐 정리 — 새 PDU 가 더 안 쌓이도록 마지막에. */
		TAILQ_REMOVE(&conn->write_pdu_list, pdu, tailq);
		iscsi_conn_free_pdu(conn, pdu);
	}

	if (conn->pending_task_cnt) {                   /* [한국어] 아직 bdev 에서 응답 대기 중인 태스크가 있으면 미완료. */
		return -1;                              /* [한국어] 호출자: shutdown 폴러로 재시도. */
	}

	return 0;                                       /* [한국어] 모든 자원 정리 완료. */
}

/*
 * [한국어]
 * iscsi_conn_cleanup_backend - 종료되는 conn 의 inflight bdev I/O 를 abort 한다.
 *
 * @conn: 종료 절차 중인 연결.
 *
 * 다중 connection 세션(MC/S, RFC3720 §3.2.1)에서는 한 conn 만 끊긴 경우
 * 다른 initiator/세션의 I/O 를 잘못 abort 할 위험이 있어, 마지막 conn 일 때만
 * 또는 단일 initiator 정책일 때만 LUN abort 를 수행한다.
 *
 * 호출 체인:
 *   iscsi_conn_destruct() → iscsi_conn_cleanup_backend() → iscsi_tgt_node_cleanup_luns()
 */
static void
iscsi_conn_cleanup_backend(struct spdk_iscsi_conn *conn)
{
	int rc;                                         /* [한국어] cleanup_luns 결과. */
	struct spdk_iscsi_tgt_node *target;             /* [한국어] 이 세션이 붙은 타깃 노드. */

	if (conn->sess->connections > 1) {              /* [한국어] MC/S 다중 연결 — 다른 conn 이 살아있으므로 LUN drain 지연. */
		/* connection specific cleanup */
	} else if (!g_iscsi.AllowDuplicateIsid) {       /* [한국어] 동일 ISID 중복 허용 설정이 아닌 경우 (단일 initiator 가정 가능). */
		/*
		 * a> a target is connected by a single initiator, cleanup backend cancels inflight
		 *    IOs and the resources (of this initiator) are reclaimed as soon as possible.
		 * b> a target is connected by multiple initiators, one of these initiators
		 *    disconnects with inflight IOs, resetting backend bdev leads all the inflight
		 *    IOs (of multiple initiators) aborted. In this scenario, drain inflight IOs of
		 *    the disconnected initiator instead.
		 */
		target = conn->sess->target;            /* [한국어] 세션이 점유한 target. */
		if (target != NULL && iscsi_get_active_conns(target) == 1) { /* [한국어] 이 conn 이 마지막 활성 연결인 경우만. */
			rc = iscsi_tgt_node_cleanup_luns(conn, target); /* [한국어] LUN 별 inflight I/O abort. */
			if (rc < 0) {
				SPDK_ERRLOG("target abort failed\n");
			}
		}
	}
}

/*
 * [한국어]
 * iscsi_conn_free - 세션에서 conn 을 분리하고 슬롯을 free 풀로 반환한다.
 *
 * @conn: 정리할 연결.
 *
 * 세션 측 conns[] 배열에서 자기 자신을 빼고, 마지막 conn 이면 세션도 해제한다.
 * g_conns_mutex 보호 구간에서 동작 — RPC dump 와의 경쟁 방지.
 *
 * 호출 체인:
 *   _iscsi_conn_destruct()/_iscsi_conn_check_shutdown() → iscsi_conn_free()
 */
static void
iscsi_conn_free(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_sess *sess;                   /* [한국어] 이 conn 이 속한 iSCSI 세션 (없을 수도 있음). */
	int idx;                                        /* [한국어] sess->conns[] 안에서 자기 위치. */
	uint32_t i;                                     /* [한국어] 루프 인덱스. */

	pthread_mutex_lock(&g_conns_mutex);             /* [한국어] 글로벌 풀 + RPC 와 직렬화. */

	if (conn->sess == NULL) {                       /* [한국어] login 실패 등으로 세션이 안 만들어진 경우 → 슬롯만 회수. */
		goto end;
	}

	idx = -1;                                       /* [한국어] 세션 내 위치 미발견 표식. */
	sess = conn->sess;                              /* [한국어] 세션 포인터 캡처. */
	conn->sess = NULL;                              /* [한국어] 양방향 참조 끊기. */

	for (i = 0; i < sess->connections; i++) {       /* [한국어] sess->conns 배열에서 자신 위치 탐색. */
		if (sess->conns[i] == conn) {
			idx = i;
			break;
		}
	}

	if (idx < 0) {                                  /* [한국어] 보호 위반 — 세션은 알지만 본인이 없음. */
		SPDK_ERRLOG("remove conn not found\n");
	} else {
		for (i = idx; i < sess->connections - 1; i++) { /* [한국어] 자기 뒤 항목들 한 칸씩 앞으로 당김(고정 배열 압축). */
			sess->conns[i] = sess->conns[i + 1];
		}
		sess->conns[sess->connections - 1] = NULL; /* [한국어] 마지막 슬롯 NULL 처리. */
		sess->connections--;                    /* [한국어] 활성 conn 수 감소. */

		if (sess->connections == 0) {           /* [한국어] 세션의 모든 conn 이 사라짐 → 세션도 해제. */
			/* cleanup last connection */
			SPDK_DEBUGLOG(iscsi,
				      "cleanup last conn free sess\n");
			iscsi_free_sess(sess);          /* [한국어] 세션 자원 해제 (TSIH 등 반환). */
		}
	}

	SPDK_DEBUGLOG(iscsi, "Terminating connections(tsih %d): %d\n",
		      sess->tsih, sess->connections);   /* [한국어] 디버그: 잔여 conn 수 출력. (sess 가 free 되어도 주소 유효 가정 — 직전 free 후 read 임. 디버그 전용) */

end:
	SPDK_DEBUGLOG(iscsi, "cleanup free conn\n");
	iscsi_param_free(conn->params);                 /* [한국어] connection 파라미터(키-값 리스트) 해제. */
	_free_conn(conn);                               /* [한국어] 슬롯을 free 풀로 반납. */

	pthread_mutex_unlock(&g_conns_mutex);           /* [한국어] 락 해제. */
}

/*
 * [한국어]
 * iscsi_conn_close_lun - 연결이 점유한 단일 LUN 디스크립터를 해제한다.
 *
 * @conn:      소속 연결.
 * @iscsi_lun: open 시 calloc 으로 만든 spdk_iscsi_lun.
 *
 * spdk_scsi_lun_open() 으로 받은 desc 와 io_channel 을 짝맞춰 정리하고,
 * 핫리무브 폴러도 해제한 뒤 conn 의 luns 리스트에서 빼고 free 한다.
 */
static void
iscsi_conn_close_lun(struct spdk_iscsi_conn *conn,
		     struct spdk_iscsi_lun *iscsi_lun)
{
	if (iscsi_lun == NULL) {                        /* [한국어] NULL 가드 — 부분 초기화 실패 시 안전. */
		return;
	}

	spdk_scsi_lun_free_io_channel(iscsi_lun->desc); /* [한국어] LUN 의 I/O 채널(thread-local SCSI 자원) 반환. */
	spdk_scsi_lun_close(iscsi_lun->desc);           /* [한국어] LUN open 시 받은 desc 닫기 — refcount 감소. */
	spdk_poller_unregister(&iscsi_lun->remove_poller); /* [한국어] 핫리무브 polling 중이면 해제. */

	TAILQ_REMOVE(&conn->luns, iscsi_lun, tailq);    /* [한국어] conn 의 LUN 리스트에서 분리. */

	free(iscsi_lun);                                /* [한국어] 래퍼 구조체 free. */

}

/*
 * [한국어]
 * iscsi_conn_close_luns - 연결이 들고 있던 모든 LUN 을 닫는다.
 *
 * @conn: 종료 절차 중인 연결.
 *
 * iscsi_conn_stop()/error path 에서 호출된다.
 */
static void
iscsi_conn_close_luns(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_lun *iscsi_lun, *tmp;         /* [한국어] SAFE 순회 변수 쌍. */

	TAILQ_FOREACH_SAFE(iscsi_lun, &conn->luns, tailq, tmp) { /* [한국어] 리스트가 변경되어도 안전한 순회. */
		iscsi_conn_close_lun(conn, iscsi_lun);  /* [한국어] 개별 close + free. */
	}
}

/*
 * [한국어]
 * iscsi_conn_check_tasks_for_lun - 특정 LUN 에 대한 inflight task/PDU 가 더 없는지 확인.
 *
 * @conn: 연결.
 * @lun : 핫리무브 대상 LUN.
 * @return: true = 정리 안전, false = 아직 task/PDU 잔존.
 *
 * SCSI bdev 핫리무브 시 호출자(iscsi_conn_remove_lun 폴러)가 안전한 시점인지
 * 1ms 마다 폴링해서 결정한다. snack 큐는 이미 flush 후라 즉시 폐기 가능.
 */
static bool
iscsi_conn_check_tasks_for_lun(struct spdk_iscsi_conn *conn,
			       struct spdk_scsi_lun *lun)
{
	struct spdk_iscsi_pdu *pdu, *tmp_pdu;           /* [한국어] PDU 순회 변수. */
	struct spdk_iscsi_task *task;                   /* [한국어] task 검사 변수. */

	assert(lun != NULL);                            /* [한국어] LUN 인자 필수. */

	/* We can remove deferred PDUs safely because they are already flushed. */
	TAILQ_FOREACH_SAFE(pdu, &conn->snack_pdu_list, tailq, tmp_pdu) { /* [한국어] snack 리스트에서 해당 LUN 항목 즉시 폐기. */
		if (lun == pdu->task->scsi.lun) {
			TAILQ_REMOVE(&conn->snack_pdu_list, pdu, tailq);
			iscsi_conn_free_pdu(conn, pdu);
		}
	}

	TAILQ_FOREACH(task, &conn->queued_datain_tasks, link) { /* [한국어] 분할 read 대기 task 가 있으면 미완. */
		if (lun == task->scsi.lun) {
			return false;
		}
	}

	/* This check loop works even when connection exits in the middle of LUN hotplug
	 *  because all PDUs in write_pdu_list are removed in iscsi_conn_free_tasks().
	 */
	TAILQ_FOREACH(pdu, &conn->write_pdu_list, tailq) { /* [한국어] 송신 큐에 해당 LUN 의 응답이 남아있는지 검사. */
		if (pdu->task && lun == pdu->task->scsi.lun) {
			return false;
		}
	}

	return true;                                    /* [한국어] 모두 비어있음 → close 안전. */
}

/*
 * [한국어]
 * iscsi_conn_remove_lun - LUN 핫리무브 시 inflight 정리될 때까지 기다리는 1ms 폴러.
 *
 * @ctx: spdk_iscsi_lun.
 * @return: SPDK_POLLER_BUSY (계속 활성).
 *
 * 안전한 시점이 되면 iscsi_conn_close_lun 으로 디스크립터 해제 + 폴러 자체도 해제됨.
 */
static int
iscsi_conn_remove_lun(void *ctx)
{
	struct spdk_iscsi_lun *iscsi_lun = ctx;         /* [한국어] 폴러 인자에서 LUN 래퍼 복원. */
	struct spdk_iscsi_conn *conn = iscsi_lun->conn; /* [한국어] 소속 연결. */
	struct spdk_scsi_lun *lun = iscsi_lun->lun;     /* [한국어] SCSI 레이어 LUN. */

	if (!iscsi_conn_check_tasks_for_lun(conn, lun)) { /* [한국어] 아직 잔여 task/PDU 가 있으면 다음 tick 대기. */
		return SPDK_POLLER_BUSY;
	}
	iscsi_conn_close_lun(conn, iscsi_lun);          /* [한국어] 안전 — close + 폴러 자체 해제. */
	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * _iscsi_conn_hotremove_lun - 메시지로 전달된 LUN 핫리무브 요청을 conn 의 스레드에서 처리.
 *
 * @ctx: spdk_iscsi_lun.
 *
 * spdk_scsi_lun_close 콜백은 어떤 스레드에서든 호출될 수 있으므로,
 * conn 자료구조 조작은 반드시 owning poll group 스레드에서 실행되도록 메시지 위임.
 */
static void
_iscsi_conn_hotremove_lun(void *ctx)
{
	struct spdk_iscsi_lun *iscsi_lun = ctx;
	struct spdk_iscsi_conn *conn = iscsi_lun->conn;
	struct spdk_scsi_lun *lun = iscsi_lun->lun;

	assert(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(conn->pg)) ==
	       spdk_get_thread());                     /* [한국어] 반드시 conn 의 owning thread 에서만 실행 — affinity 확인. */

	/* If a connection is already in stating status, just return */
	if (conn->state >= ISCSI_CONN_STATE_EXITING) {  /* [한국어] 이미 종료 중이면 중복 처리 회피. */
		return;
	}

	iscsi_clear_all_transfer_task(conn, lun, NULL); /* [한국어] 이 LUN 의 R2T/DataOUT 태스크 큐 정리. */

	iscsi_lun->remove_poller = SPDK_POLLER_REGISTER(iscsi_conn_remove_lun, iscsi_lun,
				   1000);                /* [한국어] 1ms 마다 inflight 잔여 검사 후 close 시도. */
}

/*
 * [한국어]
 * iscsi_conn_hotremove_lun - SCSI 레이어가 LUN 제거를 통보할 때 호출되는 콜백.
 *
 * @lun:        제거되는 SCSI LUN.
 * @remove_ctx: spdk_scsi_lun_open 시 등록한 컨텍스트(spdk_iscsi_lun).
 *
 * SCSI 콜백은 임의 스레드에서 호출 가능 → conn 의 poll group 스레드로 메시지 위임.
 */
static void
iscsi_conn_hotremove_lun(struct spdk_scsi_lun *lun, void *remove_ctx)
{
	struct spdk_iscsi_lun *iscsi_lun = remove_ctx;
	struct spdk_iscsi_conn *conn = iscsi_lun->conn;

	spdk_thread_send_msg(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(conn->pg)),
			     _iscsi_conn_hotremove_lun, iscsi_lun); /* [한국어] cross-thread → owning thread 로 위임. */
}

/*
 * [한국어]
 * iscsi_conn_open_lun - 연결이 단일 LUN 에 대한 SCSI desc 와 io_channel 을 확보한다.
 *
 * @conn: 연결.
 * @lun : 열 LUN (target dev 안의 LUN).
 * @return: 0 성공, 음수(-ENOMEM/SCSI 오류).
 *
 * Full Feature Phase 진입 시 conn 의 target dev 안 모든 LUN 에 대해 호출되며,
 * 핫리무브 콜백을 함께 등록한다 (initiator 가 사용 중인데 LUN 이 빠지는 상황 대응).
 */
static int
iscsi_conn_open_lun(struct spdk_iscsi_conn *conn, struct spdk_scsi_lun *lun)
{
	int rc;                                         /* [한국어] SCSI 호출 결과. */
	struct spdk_iscsi_lun *iscsi_lun;               /* [한국어] iSCSI 래퍼 구조체. */

	iscsi_lun = calloc(1, sizeof(*iscsi_lun));      /* [한국어] 0-초기화된 래퍼 할당. */
	if (iscsi_lun == NULL) {
		return -ENOMEM;
	}

	iscsi_lun->conn = conn;                         /* [한국어] 역참조용 conn 저장. */
	iscsi_lun->lun = lun;                           /* [한국어] SCSI LUN 보관. */

	rc = spdk_scsi_lun_open(lun, iscsi_conn_hotremove_lun, iscsi_lun, &iscsi_lun->desc); /* [한국어] LUN 참조 잡기 + 핫리무브 콜백 등록. */
	if (rc != 0) {
		free(iscsi_lun);
		return rc;
	}

	rc = spdk_scsi_lun_allocate_io_channel(iscsi_lun->desc); /* [한국어] 이 스레드에 LUN io_channel 확보 (thread-local). */
	if (rc != 0) {
		spdk_scsi_lun_close(iscsi_lun->desc);
		free(iscsi_lun);
		return rc;
	}

	TAILQ_INSERT_TAIL(&conn->luns, iscsi_lun, tailq); /* [한국어] conn 의 LUN 리스트에 등록. */

	return 0;
}

/*
 * [한국어]
 * iscsi_conn_open_luns - 연결의 target 안 모든 LUN 에 대해 open 을 수행.
 *
 * @conn: 연결.
 * @return: 0 성공, -1 실패(중간 실패 시 이미 열린 것 모두 close).
 */
static int
iscsi_conn_open_luns(struct spdk_iscsi_conn *conn)
{
	int rc;                                         /* [한국어] open 결과. */
	struct spdk_scsi_lun *lun;                      /* [한국어] target dev 의 각 LUN. */

	for (lun = spdk_scsi_dev_get_first_lun(conn->dev); lun != NULL;
	     lun = spdk_scsi_dev_get_next_lun(lun)) {   /* [한국어] target dev 안 모든 LUN 순회. */
		rc = iscsi_conn_open_lun(conn, lun);    /* [한국어] LUN 별 open. */
		if (rc != 0) {
			goto error;
		}
	}

	return 0;

error:
	iscsi_conn_close_luns(conn);                    /* [한국어] 부분 성공 시 모두 롤백 — 자원 누수 방지. */
	return -1;
}

/**
 *  This function will stop executing the specified connection.
 */
/*
 * [한국어]
 * iscsi_conn_stop - EXITED 상태에 도달한 연결의 target/poll group 카운터를 정리.
 *
 * @conn: 종료된 연결.
 *
 * full_feature 단계까지 진입했던 정상 세션이라면:
 *  - target->num_active_conns 감소.
 *  - 마지막이면 pg->num_active_targets 감소 (poll group 부하 통계).
 *  - LUN 디스크립터 정리.
 *
 * scheduled==0 인 경우(login 단계에서 끊어진 경우)는 위 카운터에 더해진 적이 없어 skip.
 *
 * 실행 컨텍스트: conn 의 owning poll group 스레드.
 */
static void
iscsi_conn_stop(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_tgt_node *target;             /* [한국어] 통계 갱신 대상 target. */

	assert(conn->state == ISCSI_CONN_STATE_EXITED); /* [한국어] 반드시 EXITED 상태에서만 호출 — 호출자 보장. */
	assert(conn->data_in_cnt == 0);                 /* [한국어] inflight DataIN 0 이어야 자원 누수 없음. */
	assert(conn->data_out_cnt == 0);                /* [한국어] inflight DataOUT 0. */

	if (conn->sess != NULL &&
	    conn->sess->session_type == SESSION_TYPE_NORMAL &&
	    conn->full_feature) {                       /* [한국어] Full Feature 까지 진입한 정상 세션만 카운터 정리. */
		target = conn->sess->target;

		pthread_mutex_lock(&g_iscsi.mutex);     /* [한국어] 글로벌 + target 락 순서 — deadlock 방지 일관성. */
		pthread_mutex_lock(&target->mutex);
		if (conn->scheduled != 0) {             /* [한국어] iscsi_conn_schedule 까지 갔던 conn 만 카운터에 반영되어 있음. */
			target->num_active_conns--;
			if (target->num_active_conns == 0) { /* [한국어] target 의 마지막 conn — poll group 통계도 감소. */
				assert(target->pg != NULL);
				target->pg->num_active_targets--;
			}
		}
		pthread_mutex_unlock(&target->mutex);
		pthread_mutex_unlock(&g_iscsi.mutex);

		iscsi_conn_close_luns(conn);            /* [한국어] LUN 디스크립터 모두 close. */
	}

	assert(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(conn->pg)) ==
	       spdk_get_thread());                     /* [한국어] owning thread affinity 재확인. */
}

/*
 * [한국어]
 * _iscsi_conn_check_shutdown - 1ms 폴러: 잔여 inflight task 가 사라지면 destruct 완료.
 *
 * @arg: spdk_iscsi_conn.
 * @return: SPDK_POLLER_BUSY.
 *
 * iscsi_conn_free_tasks 가 -1 을 반환했을 때 등록되어, free 가능 시점까지 백오프.
 */
static int
_iscsi_conn_check_shutdown(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;
	int rc;

	rc = iscsi_conn_free_tasks(conn);               /* [한국어] 잔여 PDU/태스크 다시 정리 시도. */
	if (rc < 0) {
		return SPDK_POLLER_BUSY;                /* [한국어] 아직 미완 — 다음 tick 대기. */
	}

	spdk_poller_unregister(&conn->shutdown_timer);  /* [한국어] 자기 자신 해제(폴링 종료). */

	iscsi_conn_stop(conn);                          /* [한국어] target/pg 카운터 정리. */
	iscsi_conn_free(conn);                          /* [한국어] 슬롯 free. */

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * _iscsi_conn_destruct - 실제 종료 절차(소켓/태스크/타이머 정리). poll group 스레드에서 호출.
 *
 * @conn: 종료할 연결 (이미 state >= EXITED 가정).
 *
 * 단계:
 *  1) sock_group 분리, sock close.
 *  2) inflight transfer task 큐 정리.
 *  3) login/logout 등 폴러 해제.
 *  4) free_tasks() 가 0 이면 즉시 stop+free, 아니면 shutdown_timer 등록 후 폴링.
 */
static void
_iscsi_conn_destruct(struct spdk_iscsi_conn *conn)
{
	int rc;

	iscsi_poll_group_remove_conn(conn->pg, conn);   /* [한국어] sock_group 분리 — 더 이상 sock_cb 호출 없음. */
	spdk_sock_close(&conn->sock);                   /* [한국어] TCP close, &conn->sock 은 NULL 로 변경. */
	iscsi_clear_all_transfer_task(conn, NULL, NULL); /* [한국어] R2T/DataOUT 큐 모든 LUN 대상 정리. */
	spdk_poller_unregister(&conn->logout_request_timer); /* [한국어] target→initiator logout 요청 폴러 해제. */
	spdk_poller_unregister(&conn->logout_timer);    /* [한국어] logout 응답 대기 폴러 해제. */
	spdk_poller_unregister(&conn->login_timer);     /* [한국어] login phase 타임아웃 폴러 해제. */

	rc = iscsi_conn_free_tasks(conn);               /* [한국어] PDU/태스크 정리 시도. */
	if (rc < 0) {
		/* The connection cannot be freed yet. Check back later. */
		conn->shutdown_timer = SPDK_POLLER_REGISTER(_iscsi_conn_check_shutdown, conn, 1000); /* [한국어] 1ms 폴러로 재시도. */
	} else {
		iscsi_conn_stop(conn);                  /* [한국어] target/pg 카운터 정리. */
		iscsi_conn_free(conn);                  /* [한국어] 슬롯 회수. */
	}
}

/*
 * [한국어]
 * _iscsi_conn_check_pending_tasks - bdev pending I/O 가 모두 끝날 때까지 대기하는 폴러.
 *
 * @arg: spdk_iscsi_conn.
 *
 * iscsi_conn_destruct() 진입 시 dev 에 pending task 가 있으면 등록되어,
 * 모두 끝나야 _iscsi_conn_destruct 로 진행.
 */
static int
_iscsi_conn_check_pending_tasks(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	if (conn->dev != NULL &&
	    spdk_scsi_dev_has_pending_tasks(conn->dev, conn->initiator_port)) { /* [한국어] 이 initiator 포트 명의의 inflight 가 남아있는지 확인. */
		return SPDK_POLLER_BUSY;
	}

	spdk_poller_unregister(&conn->shutdown_timer);  /* [한국어] 자기 자신 해제. */

	_iscsi_conn_destruct(conn);                     /* [한국어] 본격 destruct 진입. */

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * iscsi_conn_destruct - 외부에서 호출하는 연결 종료 진입점.
 *
 * @conn: 종료할 연결.
 *
 * 절차:
 *  1) state 가 이미 EXITED 면 무시.
 *  2) state = EXITED 로 전환.
 *  3) pdu_in_progress (수신 중이던 PDU) 해제 — 진행 중이던 SCSI/DataOUT 은 abort 처리.
 *  4) backend bdev I/O cleanup (단일 initiator 시).
 *  5) dev 에 pending task 있으면 1ms 폴러로 대기, 아니면 바로 _iscsi_conn_destruct.
 *
 * 호출 위치:
 *   iscsi_handle_incoming_pdus 의 EXITING 감지, NOPIN timeout, login_timeout 등.
 */
void
iscsi_conn_destruct(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_pdu *pdu;
	struct spdk_iscsi_task *task;
	int opcode;

	/* If a connection is already in exited status, just return */
	if (conn->state >= ISCSI_CONN_STATE_EXITED) {   /* [한국어] 이미 종료된 conn 재진입 가드. */
		return;
	}

	conn->state = ISCSI_CONN_STATE_EXITED;          /* [한국어] EXITED 로 즉시 전환 — destruct 한 번만 진행. */

	/*
	 * Each connection pre-allocates its next PDU - make sure these get
	 *  freed here.
	 */
	pdu = conn->pdu_in_progress;                    /* [한국어] 현재 수신 중이던 PDU(헤더만 받았거나 데이터 일부) 캡처. */
	if (pdu) {
		/* remove the task left in the PDU too. */
		task = pdu->task;                       /* [한국어] PDU 와 짝지어진 SCSI task 가 있으면 abort. */
		if (task) {
			opcode = pdu->bhs.opcode;       /* [한국어] BHS opcode 로 어떤 종류의 PDU 였는지 식별. */
			switch (opcode) {
			case ISCSI_OP_SCSI:             /* [한국어] SCSI Command — abort 후 응답 송신 경로 진입. */
			case ISCSI_OP_SCSI_DATAOUT:     /* [한국어] DataOUT — write 데이터 진행 중 중단. */
				spdk_scsi_task_process_abort(&task->scsi); /* [한국어] SCSI task abort 처리. */
				iscsi_task_cpl(&task->scsi); /* [한국어] completion 경로로 통지(cleanup 마무리). */
				break;
			default:
				SPDK_ERRLOG("unexpected opcode %x\n", opcode); /* [한국어] 그 외 opcode 는 단순 put. */
				iscsi_task_put(task);
				break;
			}
		}
		iscsi_put_pdu(pdu);                     /* [한국어] PDU 메모리 회수. */
		conn->pdu_in_progress = NULL;
	}

	if (conn->sess != NULL && conn->pending_task_cnt > 0) { /* [한국어] inflight 가 남아있으면 backend abort 시도. */
		iscsi_conn_cleanup_backend(conn);
	}

	if (conn->dev != NULL &&
	    spdk_scsi_dev_has_pending_tasks(conn->dev, conn->initiator_port)) { /* [한국어] dev 단에 아직 inflight 면 폴링 대기. */
		conn->shutdown_timer = SPDK_POLLER_REGISTER(_iscsi_conn_check_pending_tasks, conn, 1000);
	} else {
		_iscsi_conn_destruct(conn);             /* [한국어] 그 외엔 즉시 destruct. */
	}
}

/*
 * [한국어]
 * iscsi_get_active_conns - 활성 연결 수를 센다(전체 또는 특정 target).
 *
 * @target: NULL = 전체, 지정 시 해당 target 만 카운트.
 * @return: 활성 연결 수.
 *
 * shutdown 진행 여부, target 별 부하 계산 등에서 호출.
 */
int
iscsi_get_active_conns(struct spdk_iscsi_tgt_node *target)
{
	struct spdk_iscsi_conn *conn;
	int num = 0;

	if (g_conns_array == MAP_FAILED) {              /* [한국어] 풀이 깔끔하게 init 안된 경우 0 반환 (안전 가드). */
		return 0;
	}

	pthread_mutex_lock(&g_conns_mutex);             /* [한국어] active 리스트 순회 동안 alloc/free 와 충돌 방지. */
	TAILQ_FOREACH(conn, &g_active_conns, conn_link) {
		if (target == NULL || conn->target == target) { /* [한국어] target 필터 일치 시 카운트. */
			num++;
		}
	}
	pthread_mutex_unlock(&g_conns_mutex);
	return num;
}

/*
 * [한국어]
 * iscsi_conn_check_shutdown_cb - 모든 conn 이 사라진 후 풀 메모리 free + 상위에 통보.
 *
 * @arg1: unused.
 *
 * iscsi_conn_check_shutdown 폴러에서 send_msg 로 전달되어 호출.
 */
static void
iscsi_conn_check_shutdown_cb(void *arg1)
{
	_iscsi_conns_cleanup();                         /* [한국어] g_conns_array free. */
	shutdown_iscsi_conns_done();                    /* [한국어] iscsi_subsystem 에 종료 완료 통지. */
}

/*
 * [한국어]
 * iscsi_conn_check_shutdown - 데몬 종료 시 활성 conn 이 0 이 될 때까지 폴링.
 *
 * @arg: unused.
 * @return: SPDK_POLLER_BUSY.
 *
 * shutdown_iscsi_conns 가 모든 conn 에 logout 요청을 보낸 후 등록되어,
 * 모두 EXITED → free 풀 반환되면 cleanup 콜백을 호출한다.
 */
static int
iscsi_conn_check_shutdown(void *arg)
{
	if (iscsi_get_active_conns(NULL) != 0) {        /* [한국어] 아직 활성 conn 이 있으면 다음 tick. */
		return SPDK_POLLER_BUSY;
	}

	spdk_poller_unregister(&g_shutdown_timer);      /* [한국어] 자기 자신 해제. */

	spdk_thread_send_msg(spdk_get_thread(), iscsi_conn_check_shutdown_cb, NULL); /* [한국어] 같은 스레드 메시지 큐로 cleanup 위임 (안전한 unwind). */

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * iscsi_send_logout_request - target 이 initiator 에게 비동기 logout 을 요청 (Async PDU).
 *
 * @conn: 대상 연결.
 *
 * RFC3720 §10.9 (Asynchronous Message), AsyncEvent=1 (logout 요청).
 * 응답을 기다리는 동안 logout_request_timer 가 동작하며, 시간 내 안 오면 강제 종료.
 */
static void
iscsi_send_logout_request(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_pdu *rsp_pdu;                 /* [한국어] 새 송신 PDU. */
	struct iscsi_bhs_async *rsph;                   /* [한국어] BHS 를 Async PDU 형태로 캐스팅. */

	rsp_pdu = iscsi_get_pdu(conn);                  /* [한국어] PDU 풀에서 0-초기화된 PDU 획득. */
	assert(rsp_pdu != NULL);                        /* [한국어] 풀 고갈 시 abort — 운영상 발생하면 PDU 풀 크기 조정 필요. */

	rsph = (struct iscsi_bhs_async *)&rsp_pdu->bhs;
	rsp_pdu->data = NULL;                           /* [한국어] AHS/data segment 없음. */

	rsph->opcode = ISCSI_OP_ASYNC;                  /* [한국어] BHS opcode = 0x32 (Async Message). */
	to_be32(&rsph->ffffffff, 0xFFFFFFFF);           /* [한국어] LUN 필드는 0xFFFFFFFF (RFC3720 §10.9, "all LUNs"). */
	rsph->async_event = 1;                          /* [한국어] AsyncEvent=1 → "Initiator should request logout". */
	to_be16(&rsph->param3, ISCSI_LOGOUT_REQUEST_TIMEOUT); /* [한국어] Parameter3 = 권장 logout timeout (초). */

	to_be32(&rsph->stat_sn, conn->StatSN);          /* [한국어] StatSN 직렬화 (target → initiator). */
	conn->StatSN++;                                 /* [한국어] 이 응답으로 StatSN 1 증가. */
	to_be32(&rsph->exp_cmd_sn, conn->sess->ExpCmdSN); /* [한국어] target 이 다음에 기대하는 CmdSN. */
	to_be32(&rsph->max_cmd_sn, conn->sess->MaxCmdSN); /* [한국어] flow control 윈도우 상한 CmdSN. */

	iscsi_conn_write_pdu(conn, rsp_pdu, iscsi_conn_pdu_generic_complete, NULL); /* [한국어] 송신 큐에 push (비동기 writev). */
}

/*
 * [한국어]
 * logout_request_timeout - logout 요청 후 응답 없을 시 강제 종료 폴러.
 */
static int
logout_request_timeout(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	if (conn->state < ISCSI_CONN_STATE_EXITING) {   /* [한국어] 아직 종료 진입 전이면 강제 EXITING. */
		conn->state = ISCSI_CONN_STATE_EXITING;
	}

	spdk_poller_unregister(&conn->logout_request_timer); /* [한국어] 자기 자신 해제 — 1회성. */

	return SPDK_POLLER_BUSY;
}

/* If the connection is running and logout is not requested yet, request logout
 * to initiator and wait for the logout process to start.
 */
/*
 * [한국어]
 * _iscsi_conn_request_logout - poll group 스레드에서 실제 logout 요청을 보내는 핸들러.
 *
 * @ctx: spdk_iscsi_conn.
 *
 * 이미 logout 요청이 진행 중이거나 RUNNING 이 아닌 경우는 무시.
 */
static void
_iscsi_conn_request_logout(void *ctx)
{
	struct spdk_iscsi_conn *conn = ctx;

	if (conn->state > ISCSI_CONN_STATE_RUNNING ||
	    conn->logout_request_timer != NULL) {       /* [한국어] 이미 종료 진행 중이거나 logout 요청 중복 방지. */
		return;
	}

	iscsi_send_logout_request(conn);                /* [한국어] Async PDU 송신. */

	conn->logout_request_timer = SPDK_POLLER_REGISTER(logout_request_timeout,
				     conn, ISCSI_LOGOUT_REQUEST_TIMEOUT * 1000000); /* [한국어] 응답 대기 타이머 가동. */
}

/*
 * [한국어]
 * iscsi_conn_request_logout - 임의 스레드에서 호출 가능한 logout 요청 진입점.
 *
 * @conn: 대상 연결.
 *
 * - INVALID(login 단계): 즉시 EXITING + login_timer 해제.
 * - RUNNING + 요청 미발송: poll group 스레드에 메시지 위임.
 * - 그 외: 무시.
 */
static void
iscsi_conn_request_logout(struct spdk_iscsi_conn *conn)
{
	struct spdk_thread *thread;

	if (conn->state == ISCSI_CONN_STATE_INVALID) {
		/* Move it to EXITING state if the connection is in login. */
		conn->state = ISCSI_CONN_STATE_EXITING;
		spdk_poller_unregister(&conn->login_timer); /* [한국어] login 타이머도 해제. */
	} else if (conn->state == ISCSI_CONN_STATE_RUNNING &&
		   conn->logout_request_timer == NULL) {
		thread = spdk_io_channel_get_thread(spdk_io_channel_from_ctx(conn->pg));
		spdk_thread_send_msg(thread, _iscsi_conn_request_logout, conn); /* [한국어] cross-thread 위임. */
	}
}

/*
 * [한국어]
 * iscsi_conns_request_logout - target 또는 portal_group_tag 기준으로 일괄 logout 요청.
 *
 * @target: NULL = 전체, 지정 시 해당 target 만.
 * @pg_tag: -1 = 전체 portal group, 값 지정 시 해당 portal group 의 conn 만.
 *
 * RPC iscsi_target_node_remove 등에서 사용 — 특정 target/portal 의 모든 세션 종료.
 */
void
iscsi_conns_request_logout(struct spdk_iscsi_tgt_node *target, int pg_tag)
{
	struct spdk_iscsi_conn	*conn;

	if (g_conns_array == MAP_FAILED) {              /* [한국어] 풀 미초기화 가드. */
		return;
	}

	pthread_mutex_lock(&g_conns_mutex);
	TAILQ_FOREACH(conn, &g_active_conns, conn_link) {
		if ((target == NULL) ||
		    (conn->target == target && (pg_tag < 0 || conn->pg_tag == pg_tag))) { /* [한국어] 필터 매칭. */
			iscsi_conn_request_logout(conn);
		}
	}
	pthread_mutex_unlock(&g_conns_mutex);
}

/*
 * [한국어]
 * shutdown_iscsi_conns - 데몬 종료 시 모든 연결에 logout 요청 + cleanup 폴러 등록.
 *
 * iscsi_subsystem_fini 경로에서 호출. 모두 EXITED → free 풀 반환되면
 * iscsi_conn_check_shutdown_cb 가 shutdown_iscsi_conns_done 으로 상위에 통지.
 */
void
shutdown_iscsi_conns(void)
{
	iscsi_conns_request_logout(NULL, -1);           /* [한국어] 모든 conn 에 logout 요청. */

	g_shutdown_timer = SPDK_POLLER_REGISTER(iscsi_conn_check_shutdown, NULL, 1000); /* [한국어] 1ms 폴러로 진행 감시. */
}

/* Do not set conn->state if the connection has already started exiting.
 *  This ensures we do not move a connection from EXITED state back to EXITING.
 */
/*
 * [한국어]
 * _iscsi_conn_drop - 단순히 state 를 EXITING 으로 끌어올리는 메시지 핸들러.
 *
 * iscsi_drop_conns 에서 cross-thread 로 사용.
 */
static void
_iscsi_conn_drop(void *ctx)
{
	struct spdk_iscsi_conn *conn = ctx;

	if (conn->state < ISCSI_CONN_STATE_EXITING) {   /* [한국어] EXITED 로 이미 갔다면 되돌리지 않음. */
		conn->state = ISCSI_CONN_STATE_EXITING;
	}
}

/*
 * [한국어]
 * iscsi_drop_conns - 동일 initiator_name/포트로 이미 붙어있는 옛 연결을 끊는다(replace).
 *
 * @conn:        새 연결(자기 자신은 제외).
 * @conn_match:  비교 대상 문자열 (drop_all=true → initiator_name, false → SCSI port name).
 * @drop_all:    1 = 같은 initiator_name 의 모든 conn drop, 0 = 같은 SCSI port 의 conn drop.
 * @return: 0 (실패 케이스 없음, 단지 활성 conn 순회 후 메시지 송신).
 *
 * RFC3720 의 ISID/SessionType 정책에 따라 동일 initiator 가 새 세션을 열면
 * 기존 세션을 정리해야 할 때가 있다. 그때 호출됨.
 *
 * 실행 컨텍스트: 임의 스레드(cross-thread 메시지로 owning thread 에 위임).
 */
int
iscsi_drop_conns(struct spdk_iscsi_conn *conn, const char *conn_match,
		 int drop_all)
{
	struct spdk_iscsi_conn	*xconn;                 /* [한국어] 비교 대상 conn. */
	const char		*xconn_match;           /* [한국어] xconn 측 매칭 문자열. */
	struct spdk_thread	*thread;                /* [한국어] xconn 의 owning thread. */
	int			num;                    /* [한국어] drop 한 conn 수. */

	SPDK_DEBUGLOG(iscsi, "iscsi_drop_conns\n");

	num = 0;
	pthread_mutex_lock(&g_conns_mutex);
	if (g_conns_array == MAP_FAILED) {
		goto exit;
	}

	TAILQ_FOREACH(xconn, &g_active_conns, conn_link) {
		if (xconn == conn) {                    /* [한국어] 새로 들어온 자기 자신은 건너뜀. */
			continue;
		}

		if (!drop_all && xconn->initiator_port == NULL) { /* [한국어] SCSI 포트 비교 모드인데 포트가 아직 안 잡힘 → skip. */
			continue;
		}

		xconn_match =
			drop_all ? xconn->initiator_name : spdk_scsi_port_get_name(xconn->initiator_port); /* [한국어] 비교 기준 선택. */

		if (!strcasecmp(conn_match, xconn_match) &&
		    conn->target == xconn->target) {    /* [한국어] 동일 이름 + 동일 target → drop 대상 확정. */

			if (num == 0) {
				/*
				 * Only print this message before we report the
				 *  first dropped connection.
				 */
				SPDK_ERRLOG("drop old connections %s by %s\n",
					    conn->target->name, conn_match); /* [한국어] 첫 drop 직전에만 한 번 큰 메시지. */
			}

			SPDK_ERRLOG("exiting conn by %s (%s)\n",
				    xconn_match, xconn->initiator_addr); /* [한국어] 각 drop 대상 conn 의 식별 정보 로그. */
			if (xconn->sess != NULL) {
				SPDK_DEBUGLOG(iscsi, "TSIH=%u\n", xconn->sess->tsih);
			} else {
				SPDK_DEBUGLOG(iscsi, "TSIH=xx\n"); /* [한국어] 세션이 아직 안 잡힌 conn (login 단계). */
			}

			SPDK_DEBUGLOG(iscsi, "CID=%u\n", xconn->cid);

			thread = spdk_io_channel_get_thread(spdk_io_channel_from_ctx(xconn->pg));
			spdk_thread_send_msg(thread, _iscsi_conn_drop, xconn); /* [한국어] xconn 의 owning thread 에 EXITING 전이 메시지. */

			num++;
		}
	}

exit:
	pthread_mutex_unlock(&g_conns_mutex);

	if (num != 0) {
		SPDK_ERRLOG("exiting %d conns\n", num);
	}

	return 0;
}

/*
 * [한국어]
 * _iscsi_conn_abort_queued_datain_task - 분할 read 대기 중인 단일 task 의 잔여를 abort.
 *
 * @conn: 연결.
 * @task: queued_datain_tasks 에 있는 primary task.
 * @return: 0 = 처리, -1 = 잔여 슬롯 부족 → 호출자가 나중에 재시도.
 *
 * 큰 read I/O 는 SPDK_BDEV_LARGE_BUF_MAX_SIZE 단위로 잘려서 차례로 제출되며,
 * 도중 abort 가 들어오면 남은 분할분에 대해 abort subtask 를 한 번에 만들어 completion 으로 보낸다.
 */
static int
_iscsi_conn_abort_queued_datain_task(struct spdk_iscsi_conn *conn,
				     struct spdk_iscsi_task *task)
{
	struct spdk_iscsi_task *subtask;                /* [한국어] abort 처리용 subtask. */
	uint32_t remaining_size;                        /* [한국어] 아직 보내지 않은 데이터 바이트 수. */

	if (conn->data_in_cnt >= g_iscsi.MaxLargeDataInPerConnection) { /* [한국어] 동시 대용량 read 슬롯이 부족 → 후속에 재시도. */
		return -1;
	}

	assert(task->current_data_offset <= task->scsi.transfer_len); /* [한국어] 진행 오프셋 불변식. */
	/* Stop split and abort read I/O for remaining data. */
	if (task->current_data_offset < task->scsi.transfer_len) {
		remaining_size = task->scsi.transfer_len - task->current_data_offset;
		subtask = iscsi_task_get(conn, task, iscsi_task_cpl);
		assert(subtask != NULL);
		subtask->scsi.offset = task->current_data_offset; /* [한국어] subtask 의 오프셋 = 잔여 시작. */
		subtask->scsi.length = remaining_size;  /* [한국어] subtask 길이 = 잔여 전체. */
		spdk_scsi_task_set_data(&subtask->scsi, NULL, 0); /* [한국어] 실제 데이터 없음 — abort 라 빈 데이터. */
		task->current_data_offset += subtask->scsi.length; /* [한국어] primary 오프셋 완료 위치까지 이동. */

		subtask->scsi.transfer_len = subtask->scsi.length;
		spdk_scsi_task_process_abort(&subtask->scsi); /* [한국어] subtask 를 abort 상태로 마킹. */
		iscsi_task_cpl(&subtask->scsi);         /* [한국어] iSCSI 응답 PDU 생성 경로로 진입. */
	}

	/* Remove the primary task from the list because all subtasks are submitted
	 *  or aborted.
	 */
	assert(task->current_data_offset == task->scsi.transfer_len); /* [한국어] 모든 잔여가 처리됨 보장. */
	TAILQ_REMOVE(&conn->queued_datain_tasks, task, link); /* [한국어] queue 에서 제거. */
	return 0;
}

/*
 * [한국어]
 * iscsi_conn_abort_queued_datain_task - 특정 reference task tag 에 매칭되는 분할 read 를 abort.
 *
 * @conn:          연결.
 * @ref_task_tag:  task management(예: ABORT TASK)에서 가리키는 ITT.
 * @return: 처리 결과 (0=성공, -1=후속 재시도 필요).
 *
 * RFC3720 §10.5 (ABORT TASK) 경로에서 사용. ITT 가 매칭되면 _iscsi_conn_abort_queued_datain_task 호출.
 */
int
iscsi_conn_abort_queued_datain_task(struct spdk_iscsi_conn *conn,
				    uint32_t ref_task_tag)
{
	struct spdk_iscsi_task *task;

	TAILQ_FOREACH(task, &conn->queued_datain_tasks, link) {
		if (task->tag == ref_task_tag) {        /* [한국어] ITT 매칭 검사. */
			return _iscsi_conn_abort_queued_datain_task(conn, task);
		}
	}

	return 0;                                       /* [한국어] 매칭 없음 → 이미 처리된 task 일 수 있으므로 성공으로 간주. */
}

/*
 * [한국어]
 * iscsi_conn_abort_queued_datain_tasks - LUN/PDU 기준으로 큐된 read 분할 task 일괄 abort.
 *
 * @conn: 연결.
 * @lun:  대상 LUN (NULL = 전체).
 * @pdu:  기준 PDU (NULL = 전체) — 자신보다 CmdSN 이 작은 것만 abort 대상.
 * @return: 0 = 모두 처리, 음수 = 슬롯 부족 등.
 *
 * LUN reset / hot-remove / older command abort 시 사용.
 */
int
iscsi_conn_abort_queued_datain_tasks(struct spdk_iscsi_conn *conn,
				     struct spdk_scsi_lun *lun,
				     struct spdk_iscsi_pdu *pdu)
{
	struct spdk_iscsi_task *task, *task_tmp;
	struct spdk_iscsi_pdu *pdu_tmp;
	int rc;

	TAILQ_FOREACH_SAFE(task, &conn->queued_datain_tasks, link, task_tmp) {
		pdu_tmp = iscsi_task_get_pdu(task);
		if ((lun == NULL || lun == task->scsi.lun) &&
		    (pdu == NULL || (spdk_sn32_lt(pdu_tmp->cmd_sn, pdu->cmd_sn)))) { /* [한국어] sn32_lt = 모듈로 32 SN 비교 (CmdSN wrap-around 안전). */
			rc = _iscsi_conn_abort_queued_datain_task(conn, task);
			if (rc != 0) {
				return rc;
			}
		}
	}

	return 0;
}

/*
 * [한국어]
 * iscsi_conn_handle_queued_datain_tasks - 큐된 대용량 read 의 다음 분할을 제출한다.
 *
 * @conn: 연결.
 * @return: 0 (성공/예외 분기 모두 0).
 *
 * iSCSI 응답 PDU 송신 완료 콜백에서 호출되어, 슬롯이 비는 만큼 차례로 SPDK_BDEV_LARGE_BUF_MAX_SIZE
 * 크기의 subtask 를 만들어 bdev 로 보낸다. LUN 이 사라진 경우엔 NULL_LUN 응답으로 마무리.
 */
int
iscsi_conn_handle_queued_datain_tasks(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_task *task;

	while (!TAILQ_EMPTY(&conn->queued_datain_tasks) &&
	       conn->data_in_cnt < g_iscsi.MaxLargeDataInPerConnection) { /* [한국어] 큐가 있고 동시 처리 슬롯이 남았으면 진행. */
		task = TAILQ_FIRST(&conn->queued_datain_tasks);
		assert(task->current_data_offset <= task->scsi.transfer_len);
		if (task->current_data_offset < task->scsi.transfer_len) {
			struct spdk_iscsi_task *subtask;
			uint32_t remaining_size = 0;

			remaining_size = task->scsi.transfer_len - task->current_data_offset;
			subtask = iscsi_task_get(conn, task, iscsi_task_cpl);
			assert(subtask != NULL);
			subtask->scsi.offset = task->current_data_offset;
			spdk_scsi_task_set_data(&subtask->scsi, NULL, 0);

			if (spdk_scsi_dev_get_lun(conn->dev, task->lun_id) == NULL) {
				/* Stop submitting split read I/Os for remaining data. */
				TAILQ_REMOVE(&conn->queued_datain_tasks, task, link);
				task->current_data_offset += remaining_size; /* [한국어] 잔여 전체 소비 처리. */
				assert(task->current_data_offset == task->scsi.transfer_len);
				subtask->scsi.transfer_len = remaining_size;
				spdk_scsi_task_process_null_lun(&subtask->scsi); /* [한국어] LUN 없음 응답 — SCSI status 채움. */
				iscsi_task_cpl(&subtask->scsi);
				return 0;
			}

			subtask->scsi.length = spdk_min(SPDK_BDEV_LARGE_BUF_MAX_SIZE, remaining_size); /* [한국어] 한 번에 보낼 청크 크기 결정. */
			task->current_data_offset += subtask->scsi.length;
			iscsi_queue_task(conn, subtask);  /* [한국어] bdev 로 read 제출. */
		}
		if (task->current_data_offset == task->scsi.transfer_len) {
			TAILQ_REMOVE(&conn->queued_datain_tasks, task, link); /* [한국어] 모든 분할 제출 끝 → 큐에서 제거. */
		}
	}
	return 0;
}

/*
 * [한국어]
 * iscsi_task_mgmt_cpl - SCSI task management 함수 완료 콜백.
 *
 * @scsi_task: 완료된 SCSI task (내장 iSCSI task 로 복원).
 *
 * RFC3720 §10.5 (TaskMgmt) 의 응답 PDU 를 보낸 뒤 task put.
 * SCSI 레이어가 task management 완료 시 호출하는 cb_fn 으로 등록되어 있음.
 */
void
iscsi_task_mgmt_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_iscsi_task *task = iscsi_task_from_scsi_task(scsi_task); /* [한국어] container_of 패턴으로 iSCSI 래퍼 복원. */

	iscsi_task_mgmt_response(task->conn, task);     /* [한국어] TaskMgmtResp PDU 송신 큐에 push. */
	iscsi_task_put(task);                           /* [한국어] task 참조 해제. */
}

/*
 * [한국어]
 * process_completed_read_subtask_list_in_order - 정렬된 분할 read subtask 들을 순서대로 응답.
 *
 * @conn:    연결.
 * @primary: 분할 read 의 primary task (subtask_list 보유).
 *
 * DataSequenceInOrder=Yes 인 세션에서 분할 subtask 가 도착 순서와 무관하게 완료될 수 있어,
 * primary->bytes_completed 와 subtask->offset 이 맞는 것부터 응답으로 보낸다.
 */
static void
process_completed_read_subtask_list_in_order(struct spdk_iscsi_conn *conn,
		struct spdk_iscsi_task *primary)
{
	struct spdk_iscsi_task *subtask, *tmp;

	TAILQ_FOREACH_SAFE(subtask, &primary->subtask_list, subtask_link, tmp) {
		if (subtask->scsi.offset == primary->bytes_completed) { /* [한국어] 이번에 응답할 차례인지 검사. */
			TAILQ_REMOVE(&primary->subtask_list, subtask, subtask_link);
			primary->bytes_completed += subtask->scsi.length; /* [한국어] primary 진행도 갱신. */
			if (primary->bytes_completed == primary->scsi.transfer_len) {
				iscsi_task_put(primary);        /* [한국어] 마지막 subtask면 primary put. */
			}
			iscsi_task_response(conn, subtask);     /* [한국어] DataIN + 필요 시 SCSI Response 송신. */
			iscsi_task_put(subtask);                /* [한국어] subtask 참조 감소. */
		} else {
			break;                                  /* [한국어] 순서가 안 맞으면 중단 — 다음 도착을 기다림. */
		}
	}
}

/*
 * [한국어]
 * process_read_task_completion - read I/O 완료 처리 (분할 여부에 따라 분기).
 *
 * @conn:    연결.
 * @task:    완료된 task (primary 자체이거나 subtask 일 수 있음).
 * @primary: read 의 primary task.
 *
 * 처리 흐름:
 *  - 에러 발생 시 모든 형제 subtask 와 primary 에 상태 전파.
 *  - task==primary 면 단발 read 처리.
 *  - DataSequenceInOrder=No → 즉시 응답.
 *  - DataSequenceInOrder=Yes → out-of-order 시 subtask_list 에 정렬 삽입 후 in-order 처리.
 */
static void
process_read_task_completion(struct spdk_iscsi_conn *conn,
			     struct spdk_iscsi_task *task,
			     struct spdk_iscsi_task *primary)
{
	struct spdk_iscsi_task *tmp;

	if (task->scsi.status != SPDK_SCSI_STATUS_GOOD) {
		if (primary->scsi.status == SPDK_SCSI_STATUS_GOOD) {
			/* If the status of the completed subtask, task, is the
			 * first failure, copy it to out-of-order subtasks, and
			 * remember it as the status of the SCSI Read Command.
			 */
			TAILQ_FOREACH(tmp, &primary->subtask_list, subtask_link) {
				spdk_scsi_task_copy_status(&tmp->scsi, &task->scsi); /* [한국어] 모든 대기 subtask 에 에러 전파. */
			}
			spdk_scsi_task_copy_status(&primary->scsi, &task->scsi); /* [한국어] primary 도 에러 상태로 마킹. */
		}
	} else if (primary->scsi.status != SPDK_SCSI_STATUS_GOOD) {
		/* Even if the status of the completed subtask is success,
		 * if there are any failed subtask ever, copy the first failed
		 * status to it.
		 */
		spdk_scsi_task_copy_status(&task->scsi, &primary->scsi); /* [한국어] 이미 누가 실패했으면 새 subtask 도 실패로 통일. */
	}

	if (task == primary) {
		/* If read I/O size is not larger than SPDK_BDEV_LARGE_BUF_MAX_SIZE,
		 * the primary task which processes the SCSI Read Command PDU is
		 * submitted directly. Hence send SCSI Response PDU for the primary
		 * task simply.
		 */
		primary->bytes_completed = task->scsi.length;
		assert(primary->bytes_completed == task->scsi.transfer_len);
		iscsi_task_response(conn, task);
		iscsi_task_put(task);
	} else if (!conn->sess->DataSequenceInOrder) {
		/* If DataSequenceInOrder is No, send SCSI Response PDU for the completed
		 * subtask without any deferral.
		 */
		primary->bytes_completed += task->scsi.length;
		if (primary->bytes_completed == primary->scsi.transfer_len) {
			iscsi_task_put(primary);
		}
		iscsi_task_response(conn, task);
		iscsi_task_put(task);
	} else {
		/* If DataSequenceInOrder is Yes, if the completed subtask is out-of-order,
		 * it is deferred until all preceding subtasks send SCSI Response PDU.
		 */
		if (task->scsi.offset != primary->bytes_completed) {
			TAILQ_FOREACH(tmp, &primary->subtask_list, subtask_link) {
				if (task->scsi.offset < tmp->scsi.offset) {
					TAILQ_INSERT_BEFORE(tmp, task, subtask_link); /* [한국어] offset 오름차순으로 정렬 삽입. */
					return;
				}
			}

			TAILQ_INSERT_TAIL(&primary->subtask_list, task, subtask_link); /* [한국어] 가장 큰 offset 이면 끝에 삽입. */
		} else {
			TAILQ_INSERT_HEAD(&primary->subtask_list, task, subtask_link); /* [한국어] 마침 차례라면 head 에 두고 in-order 처리. */
			process_completed_read_subtask_list_in_order(conn, primary);
		}
	}
}

/*
 * [한국어]
 * process_non_read_task_completion - write/지원 task 완료 처리.
 *
 * @conn:    연결.
 * @task:    완료된 task (primary 또는 분할 write subtask).
 * @primary: write 의 primary.
 *
 * write 는 보통 R2T → DataOUT 의 다단계 흐름이며, 마지막에 SCSI Response 를 한 번 보낸다.
 * is_r2t_active 가 true 면 primary 가 응답 송신 + transfer task 제거를 담당.
 */
static void
process_non_read_task_completion(struct spdk_iscsi_conn *conn,
				 struct spdk_iscsi_task *task,
				 struct spdk_iscsi_task *primary)
{
	primary->bytes_completed += task->scsi.length;  /* [한국어] write 진행도 갱신. */

	if (task == primary) {
		/* This was a small write with no R2T. */
		iscsi_task_response(conn, task);        /* [한국어] R2T 없는 짧은 write — 즉시 응답. */
		iscsi_task_put(task);
		return;
	}

	if (task->scsi.status == SPDK_SCSI_STATUS_GOOD) {
		primary->scsi.data_transferred += task->scsi.data_transferred; /* [한국어] 누적 데이터 길이 합산. */
	} else if (primary->scsi.status == SPDK_SCSI_STATUS_GOOD) {
		/* If the status of this subtask is the first failure, copy it to
		 * the primary task.
		 */
		spdk_scsi_task_copy_status(&primary->scsi, &task->scsi);
	}

	if (primary->bytes_completed == primary->scsi.transfer_len) {
		/* If LUN is removed in the middle of the iSCSI write sequence,
		 *  primary might complete the write to the initiator because it is not
		 *  ensured that the initiator will send all data requested by R2Ts.
		 *
		 * We check it and skip the following if primary is completed. (see
		 *  iscsi_clear_all_transfer_task() in iscsi.c.)
		 */
		if (primary->is_r2t_active) {
			iscsi_task_response(conn, primary); /* [한국어] R2T 흐름이 활성이었으면 primary 명의로 SCSI Response 송신. */
			iscsi_del_transfer_task(conn, primary->tag); /* [한국어] active_r2t_tasks 에서 제거. */
		} else {
			iscsi_task_response(conn, task);
		}
	}
	iscsi_task_put(task);
}

/*
 * [한국어]
 * iscsi_task_cpl - bdev I/O 완료 콜백 (SCSI task → iSCSI task 응답으로 변환).
 *
 * @scsi_task: 완료된 SCSI task.
 *
 * 모든 read/write 완료 흐름의 진입점.
 * read 인지 write 인지에 따라 분기하고, trace point 도 함께 기록한다.
 *
 * 실행 컨텍스트:
 *   - bdev 완료 콜백 → conn 의 owning poll group 스레드 (bdev I/O channel 의 thread).
 *
 * 호출 체인:
 *   bdev module IO completion → spdk_bdev_io_complete → spdk_scsi_lun completion
 *     → iscsi_task_cpl() → process_(non_)read_task_completion → iscsi_task_response
 *       → iscsi_conn_write_pdu → spdk_sock_writev_async
 */
void
iscsi_task_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_iscsi_task *primary;
	struct spdk_iscsi_task *task = iscsi_task_from_scsi_task(scsi_task);
	struct spdk_iscsi_conn *conn = task->conn;
	struct spdk_iscsi_pdu *pdu = task->pdu;

	spdk_trace_record(TRACE_ISCSI_TASK_DONE, conn->trace_id, 0, (uintptr_t)task); /* [한국어] trace 기록 — 분석 도구에서 후속 분석. */

	task->is_queued = false;                        /* [한국어] bdev 큐에서 빠졌음을 표시. */
	primary = iscsi_task_get_primary(task);         /* [한국어] subtask 라면 primary 포인터 추적. */

	if (iscsi_task_is_read(primary)) {
		process_read_task_completion(conn, task, primary);
	} else {
		process_non_read_task_completion(conn, task, primary);
	}
	if (!task->parent) {                            /* [한국어] parent 없는 task(=primary)면 PDU 단위 완료 trace 도 기록. */
		spdk_trace_record(TRACE_ISCSI_PDU_COMPLETED, conn->trace_id, 0, (uintptr_t)pdu);
	}
}

/*
 * [한국어]
 * iscsi_conn_send_nopin - target → initiator 로 NOP-In keepalive PDU 송신.
 *
 * @conn: 대상 연결 (Full Feature, Normal session 인 경우만 동작).
 *
 * RFC3720 §10.18 (NOP-In). ITT=0xFFFFFFFF, TTT=conn->id 로 보내며
 * initiator 는 같은 TTT 로 NOP-Out 을 응답한다. 응답이 timeout 내 없으면 연결 종료.
 */
static void
iscsi_conn_send_nopin(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_pdu *rsp_pdu;                 /* [한국어] 송신 PDU. */
	struct iscsi_bhs_nop_in *rsp;                   /* [한국어] BHS 를 NOP-In 으로 캐스팅. */
	/* Only send nopin if we have logged in and are in a normal session. */
	if (conn->sess == NULL ||
	    !conn->full_feature ||
	    !iscsi_param_eq_val(conn->sess->params, "SessionType", "Normal")) { /* [한국어] Discovery 세션엔 NOP-In 안 보냄. */
		return;
	}
	SPDK_DEBUGLOG(iscsi, "send NOPIN isid=%"PRIx64", tsih=%u, cid=%u\n",
		      conn->sess->isid, conn->sess->tsih, conn->cid);
	SPDK_DEBUGLOG(iscsi, "StatSN=%u, ExpCmdSN=%u, MaxCmdSN=%u\n",
		      conn->StatSN, conn->sess->ExpCmdSN,
		      conn->sess->MaxCmdSN);
	rsp_pdu = iscsi_get_pdu(conn);                  /* [한국어] PDU 풀에서 0-초기화 PDU. */
	rsp = (struct iscsi_bhs_nop_in *) &rsp_pdu->bhs;
	rsp_pdu->data = NULL;                           /* [한국어] data segment 없음. */
	/*
	 * iscsi_get_pdu() memset's the PDU for us, so only fill out the needed
	 *  fields.
	 */
	rsp->opcode = ISCSI_OP_NOPIN;                   /* [한국어] BHS opcode = 0x20. */
	rsp->flags = 0x80;                              /* [한국어] Final bit (F=1). */
	/*
	 * Technically the to_be32() is not needed here, since
	 *  to_be32(0xFFFFFFFU) returns 0xFFFFFFFFU.
	 */
	to_be32(&rsp->itt, 0xFFFFFFFFU);                /* [한국어] ITT = 모두 1 → "no associated SCSI command" (RFC3720 §10.18). */
	to_be32(&rsp->ttt, conn->id);                   /* [한국어] TTT = conn id → initiator 의 NOP-Out 응답 매칭용. */
	to_be32(&rsp->stat_sn, conn->StatSN);
	to_be32(&rsp->exp_cmd_sn, conn->sess->ExpCmdSN);
	to_be32(&rsp->max_cmd_sn, conn->sess->MaxCmdSN);
	iscsi_conn_write_pdu(conn, rsp_pdu, iscsi_conn_pdu_generic_complete, NULL);
	conn->last_nopin = spdk_get_ticks();            /* [한국어] 마지막 NOP-In 시각 갱신 — timeout 측정 기준. */
	conn->nop_outstanding = true;                   /* [한국어] 응답 대기 플래그 set — 다음 NOP-Out 수신 시 클리어. */
}

/*
 * [한국어]
 * iscsi_conn_handle_nop - poll group 의 nop_poller 가 주기적으로 호출하는 NOP 관리 함수.
 *
 * @conn: 대상 연결.
 *
 * 두 가지 일을 한다:
 *  1) NOP-Out 응답을 conn->timeout 내 못 받으면 EXITING 으로 강제 전이.
 *  2) NOP-In 송신 간격(nopininterval)이 지나면 새 NOP-In 송신.
 *
 * 호출 컨텍스트: poll group nop_poller (1초 정도 주기) → conn 의 owning thread.
 */
void
iscsi_conn_handle_nop(struct spdk_iscsi_conn *conn)
{
	uint64_t	tsc;

	/**
	  * This function will be executed by nop_poller of iSCSI polling group, so
	  * we need to check the connection state first, then do the nop interval
	  * expiration check work.
	  */
	if ((conn->state == ISCSI_CONN_STATE_EXITED) ||
	    (conn->state == ISCSI_CONN_STATE_EXITING)) { /* [한국어] 종료 중 conn 은 NOP 처리 안 함. */
		return;
	}

	/* Check for nop interval expiration */
	tsc = spdk_get_ticks();                         /* [한국어] 현재 TSC (TSC=Time Stamp Counter, CPU 내장 클럭). */
	if (conn->nop_outstanding) {
		if ((tsc - conn->last_nopin) > conn->timeout) { /* [한국어] 응답이 timeout 안에 안 옴 → 연결 끊기. */
			SPDK_ERRLOG("Timed out waiting for NOP-Out response from initiator\n");
			SPDK_ERRLOG("  tsc=0x%" PRIx64 ", last_nopin=0x%" PRIx64 "\n", tsc, conn->last_nopin);
			SPDK_ERRLOG("  initiator=%s, target=%s\n", conn->initiator_name,
				    conn->target_short_name);
			conn->state = ISCSI_CONN_STATE_EXITING;
		}
	} else if (tsc - conn->last_nopin > conn->nopininterval) { /* [한국어] 송신 간격 도래 → 새 NOP-In. */
		iscsi_conn_send_nopin(conn);
	}
}

/**
 * \brief Reads data for the specified iSCSI connection from its TCP socket.
 *
 * The TCP socket is marked as non-blocking, so this function may not read
 * all data requested.
 *
 * Returns SPDK_ISCSI_CONNECTION_FATAL if the recv() operation indicates a fatal
 * error with the TCP connection (including if the TCP connection was closed
 * unexpectedly.
 *
 * Otherwise returns the number of bytes successfully read.
 */
/*
 * [한국어]
 * iscsi_conn_read_data - 단일 버퍼로 PDU 데이터를 socket 에서 읽어온다.
 *
 * @conn:  대상 연결.
 * @bytes: 요청 길이.
 * @buf:   대상 버퍼.
 * @return: >0 = 실제 읽은 바이트, 0 = EAGAIN, SPDK_ISCSI_CONNECTION_FATAL = 종료 필요.
 *
 * non-blocking sock 이므로 부분 읽기 가능. iscsi_handle_incoming_pdus 의
 * BHS/AHS/HeaderDigest/Data/DataDigest 단계에서 차례로 호출.
 */
int
iscsi_conn_read_data(struct spdk_iscsi_conn *conn, int bytes,
		     void *buf)
{
	int rc;

	if (bytes == 0) {
		return 0;                               /* [한국어] 0 바이트 요청은 즉시 0 반환. */
	}

	rc = spdk_sock_recv(conn->sock, buf, bytes);    /* [한국어] sock 추상화의 recv (POSIX/uring 등). */
	if (rc > 0) {
		spdk_trace_record(TRACE_ISCSI_READ_FROM_SOCKET_DONE, conn->trace_id, rc, 0); /* [한국어] trace 기록. */
		return rc;
	}

	if (rc < 0) {
		if (rc == -EAGAIN || rc == -EWOULDBLOCK) {
			return 0;                       /* [한국어] non-blocking 일시 부족 — 0 반환으로 호출자 재시도 허용. */
		}

		/* For connect reset issue, do not output error log */
		if (rc == -ECONNRESET) {
			SPDK_DEBUGLOG(iscsi, "spdk_sock_recv() failed, rc %d: %s\n", rc, spdk_strerror(-rc)); /* [한국어] RST 는 디버그 로그만 — 흔한 정상 종료. */
		} else {
			SPDK_ERRLOG("spdk_sock_recv() failed, rc %d: %s\n", rc, spdk_strerror(-rc)); /* [한국어] 그 외 에러는 ERR 로그. */
		}
	}

	/* connection closed */
	return SPDK_ISCSI_CONNECTION_FATAL;             /* [한국어] rc==0(EOF) 또는 rc<0(에러) → 호출자가 EXITING 전이. */
}

/*
 * [한국어]
 * iscsi_conn_readv_data - iovec scatter recv. iovcnt==1 이면 recv 로 폴백.
 *
 * @conn:   대상 연결.
 * @iov:    수신 iovec 배열.
 * @iovcnt: iovec 수.
 * @return: read 바이트 / 0 / FATAL.
 *
 * data segment 가 여러 메모리 영역(헤더 padding 포함)에 분산되어 있을 때 사용.
 */
int
iscsi_conn_readv_data(struct spdk_iscsi_conn *conn,
		      struct iovec *iov, int iovcnt)
{
	int rc;

	if (iov == NULL || iovcnt == 0) {
		return 0;
	}

	if (iovcnt == 1) {
		return iscsi_conn_read_data(conn, iov[0].iov_len,
					    iov[0].iov_base);  /* [한국어] 단일 iovec 은 일반 recv 로 처리 (오버헤드 절감). */
	}

	rc = spdk_sock_readv(conn->sock, iov, iovcnt);  /* [한국어] readv 추상화 — kernel 의 readv() 또는 uring SQE. */
	if (rc > 0) {
		spdk_trace_record(TRACE_ISCSI_READ_FROM_SOCKET_DONE, conn->trace_id, rc, 0);
		return rc;
	}

	if (rc < 0) {
		if (rc == -EAGAIN || rc == -EWOULDBLOCK) {
			return 0;
		}

		/* For connect reset issue, do not output error log */
		if (rc == -ECONNRESET) {
			SPDK_DEBUGLOG(iscsi, "spdk_sock_readv() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		} else {
			SPDK_ERRLOG("spdk_sock_readv() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		}
	}

	/* connection closed */
	return SPDK_ISCSI_CONNECTION_FATAL;
}

/*
 * [한국어]
 * iscsi_is_free_pdu_deferred - 송신 완료 PDU 를 즉시 free 하지 않고 snack 큐로 보낼지 판정.
 *
 * @pdu: 검사 대상 PDU.
 * @return: true = snack 큐 보존, false = 즉시 free.
 *
 * R2T / DataIN PDU 는 ErrorRecoveryLevel>=1 일 때 SNACK 재전송 대비 보관 필요 (RFC3720 §6).
 */
static bool
iscsi_is_free_pdu_deferred(struct spdk_iscsi_pdu *pdu)
{
	if (pdu == NULL) {
		return false;
	}

	if (pdu->bhs.opcode == ISCSI_OP_R2T ||
	    pdu->bhs.opcode == ISCSI_OP_SCSI_DATAIN) {
		return true;
	}

	return false;
}

/*
 * [한국어]
 * iscsi_dif_verify - 송신 직전 데이터 PDU 의 DIF (Data Integrity Field) 검증.
 *
 * @pdu:     검증 대상 PDU.
 * @dif_ctx: DIF 컨텍스트 (block size, type, guard 등).
 * @return:  0 성공, !=0 → 데이터 무결성 오류 발견.
 *
 * NVMe T10 DIF/DIX 모델: 각 블록 끝에 8바이트 PI 가 붙어 GUARD/APPTAG/REFTAG 가 검증된다.
 * iSCSI Target 이 bdev 에서 read 한 데이터를 initiator 에 보내기 전 마지막 검증.
 */
static int
iscsi_dif_verify(struct spdk_iscsi_pdu *pdu, struct spdk_dif_ctx *dif_ctx)
{
	struct iovec iov;                               /* [한국어] 단일 iovec 으로 spdk_dif_verify 호출. */
	struct spdk_dif_error err_blk = {};             /* [한국어] 오류 정보 출력 버퍼. */
	uint32_t num_blocks;                            /* [한국어] data_buf_len / block_size. */
	int rc;

	iov.iov_base = pdu->data;
	iov.iov_len = pdu->data_buf_len;
	num_blocks = pdu->data_buf_len / dif_ctx->block_size;

	rc = spdk_dif_verify(&iov, 1, num_blocks, dif_ctx, &err_blk); /* [한국어] util/dif.c 의 GUARD/REFTAG/APPTAG 검사. */
	if (rc != 0) {
		SPDK_ERRLOG("DIF error detected. type=%d, offset=%" PRIu32 "\n",
			    err_blk.err_type, err_blk.err_offset);
	}

	return rc;
}

/*
 * [한국어]
 * _iscsi_conn_pdu_write_done - spdk_sock_writev_async 의 송신 완료 콜백.
 *
 * @cb_arg: spdk_iscsi_pdu (sock_req.cb_arg).
 * @err:    0 성공, 음수 errno.
 *
 * 처리:
 *  - conn 이 EXITING 이상이면 다른 경로(destruct)에서 정리하므로 즉시 return.
 *  - write_pdu_list 에서 분리.
 *  - 에러 발생 시 EXITING 으로 전이.
 *  - ErrorRecoveryLevel>=1 + R2T/DataIN 이면 snack 큐로 이동(재전송 대비), 아니면 free.
 */
static void
_iscsi_conn_pdu_write_done(void *cb_arg, int err)
{
	struct spdk_iscsi_pdu *pdu = cb_arg;
	struct spdk_iscsi_conn *conn = pdu->conn;

	assert(conn != NULL);

	if (spdk_unlikely(conn->state >= ISCSI_CONN_STATE_EXITING)) {
		/* The other policy will recycle the resource */
		return;
	}

	TAILQ_REMOVE(&conn->write_pdu_list, pdu, tailq);

	if (err != 0) {
		conn->state = ISCSI_CONN_STATE_EXITING;
	}

	if ((conn->full_feature) &&
	    (conn->sess->ErrorRecoveryLevel >= 1) &&
	    iscsi_is_free_pdu_deferred(pdu)) {       /* [한국어] ERL1 이상 + 재전송 대상이면 snack 큐 보존. */
		SPDK_DEBUGLOG(iscsi, "stat_sn=%d\n",
			      from_be32(&pdu->bhs.stat_sn));
		TAILQ_INSERT_TAIL(&conn->snack_pdu_list, pdu,
				  tailq);
	} else {
		iscsi_conn_free_pdu(conn, pdu);          /* [한국어] 보통의 PDU 는 즉시 free. */
	}
}

/*
 * [한국어]
 * iscsi_conn_pdu_generic_complete - 별도 후속 작업이 필요 없는 PDU 의 완료 콜백 (no-op).
 *
 * cb_fn 필드는 NULL 이면 안 되므로 placeholder 로 사용된다.
 */
void
iscsi_conn_pdu_generic_complete(void *cb_arg)
{
}

/*
 * [한국어]
 * iscsi_conn_write_pdu - PDU 를 송신 큐에 넣고 비동기 writev 시작.
 *
 * @conn:   대상 연결.
 * @pdu:    송신할 PDU (BHS/data 채워진 상태).
 * @cb_fn:  송신 완료 후 호출할 콜백.
 * @cb_arg: 콜백 인자.
 *
 * 단계:
 *  1) DIF 보호 필요 시 검증 (실패 시 EXITING).
 *  2) Login Response 가 아니면 Header/Data Digest CRC32C 계산 + 직렬화.
 *  3) write_pdu_list 에 push (송신 추적용).
 *  4) iovec 빌드 + spdk_sock_writev_async 제출.
 *  5) 완료 시 _iscsi_conn_pdu_write_done 콜백.
 *
 * 호출 위치: iscsi_task_response, login_rsp, NOPIN, AsyncReq 등 모든 송신 경로.
 */
void
iscsi_conn_write_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu,
		     iscsi_conn_xfer_complete_cb cb_fn,
		     void *cb_arg)
{
	uint32_t crc32c;                                /* [한국어] CRC32C 결과(헤더/데이터 다이제스트). */
	ssize_t rc;

	if (spdk_unlikely(pdu->dif_insert_or_strip)) {
		rc = iscsi_dif_verify(pdu, &pdu->dif_ctx); /* [한국어] DIF strip 시 무결성 검증. */
		if (rc != 0) {
			iscsi_conn_free_pdu(conn, pdu);
			conn->state = ISCSI_CONN_STATE_EXITING; /* [한국어] DIF 오류는 치명 — 연결 종료. */
			return;
		}
	}

	if (pdu->bhs.opcode != ISCSI_OP_LOGIN_RSP) {
		/* Header Digest */
		if (conn->header_digest) {
			crc32c = iscsi_pdu_calc_header_digest(pdu); /* [한국어] BHS+AHS CRC32C 계산. */
			MAKE_DIGEST_WORD(pdu->header_digest, crc32c); /* [한국어] LE 4바이트로 직렬화. */
		}

		/* Data Digest */
		if (conn->data_digest && DGET24(pdu->bhs.data_segment_len) != 0) {
			crc32c = iscsi_pdu_calc_data_digest(pdu); /* [한국어] data segment CRC32C. */
			MAKE_DIGEST_WORD(pdu->data_digest, crc32c);
		}
	}

	pdu->cb_fn = cb_fn;                             /* [한국어] free_pdu 시 호출할 콜백 저장. */
	pdu->cb_arg = cb_arg;
	TAILQ_INSERT_TAIL(&conn->write_pdu_list, pdu, tailq); /* [한국어] 송신 추적 큐. */

	if (spdk_unlikely(conn->state >= ISCSI_CONN_STATE_EXITING)) {
		return;                                 /* [한국어] 종료 단계 진입했으면 송신 안 함 — destruct 가 정리. */
	}
	pdu->sock_req.iovcnt = iscsi_build_iovs(conn, pdu->iov, SPDK_COUNTOF(pdu->iov), pdu,
						&pdu->mapped_length); /* [한국어] BHS/AHS/digest/data padding 등을 iovec 으로 빌드. */
	pdu->sock_req.cb_fn = _iscsi_conn_pdu_write_done;
	pdu->sock_req.cb_arg = pdu;

	spdk_sock_writev_async(conn->sock, &pdu->sock_req); /* [한국어] sock 추상화 비동기 writev — 완료 시 콜백 호출. */
}

/*
 * [한국어]
 * iscsi_conn_sock_cb - sock_group 가 readable 이벤트를 알릴 때 호출되는 콜백.
 *
 * @arg:   spdk_iscsi_conn (sock_group_add_sock 시 등록).
 * @group: sock group.
 * @sock:  readable 소켓.
 *
 * 실제 PDU 디코딩은 iscsi_handle_incoming_pdus 가 수행하며, 실패 시 EXITING 전이.
 */
static void
iscsi_conn_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_iscsi_conn *conn = arg;
	int rc;

	assert(conn != NULL);

	if ((conn->state == ISCSI_CONN_STATE_EXITED) ||
	    (conn->state == ISCSI_CONN_STATE_EXITING)) { /* [한국어] 종료 중이면 더 처리하지 않음. */
		return;
	}

	/* Handle incoming PDUs */
	rc = iscsi_handle_incoming_pdus(conn);          /* [한국어] PDU 수신 FSM 진행 (lib/iscsi/iscsi.c). */
	if (rc < 0) {
		conn->state = ISCSI_CONN_STATE_EXITING; /* [한국어] PDU 처리 실패 → 종료 전이. */
	}
}

/*
 * [한국어]
 * iscsi_conn_full_feature_migrate - 새 poll group 에서 LUN open 후 sock_group 등록.
 *
 * @arg: spdk_iscsi_conn.
 *
 * iscsi_conn_schedule 의 cross-thread 메시지 핸들러. Normal 세션이면 LUN 디스크립터 확보.
 * 실패해도 일단 sock_group 에는 등록하여, 다음 poll 에서 destruct 가 호출되도록 한다.
 */
static void
iscsi_conn_full_feature_migrate(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;
	int rc;

	assert(conn->state != ISCSI_CONN_STATE_EXITED);

	/* Note: it is possible that connection could have moved to EXITING
	 * state after this message was sent. We will still add it to the
	 * poll group in this case.  When the poll group is polled
	 * again, it will call iscsi_conn_destruct() on it.
	 */

	if (conn->sess->session_type == SESSION_TYPE_NORMAL) {
		rc = iscsi_conn_open_luns(conn);
		if (rc != 0) {
			/* If opening LUNs failed, it is a fatal error. At the first poll in the
			 * assigned poll group, this connection will be destructed.
			 */
			conn->state = ISCSI_CONN_STATE_EXITING;
		}
	}

	/* Add this connection to the assigned poll group. */
	iscsi_poll_group_add_conn(conn->pg, conn);      /* [한국어] 새 pg 의 sock_group 에 등록. */
}

/*
 * [한국어]
 * iscsi_get_idlest_poll_group - num_active_targets 가 가장 적은 poll group 반환.
 *
 * @return: 가장 한가한 poll group (없으면 NULL — 실제로는 항상 존재).
 *
 * 부하 분산용 — 새 target 의 첫 conn 이 들어왔을 때 어떤 코어로 보낼지 결정.
 */
static struct spdk_iscsi_poll_group *
iscsi_get_idlest_poll_group(void)
{
	struct spdk_iscsi_poll_group *pg, *idle_pg = NULL;
	uint32_t min_num_targets = UINT32_MAX;

	TAILQ_FOREACH(pg, &g_iscsi.poll_group_head, link) {
		if (pg->num_active_targets == 0) {
			return pg;                      /* [한국어] target 0인 pg 발견 — 즉시 반환(최선). */
		} else if (pg->num_active_targets < min_num_targets) {
			min_num_targets = pg->num_active_targets;
			idle_pg = pg;
		}
	}

	return idle_pg;
}

/*
 * [한국어]
 * iscsi_conn_schedule - Normal 세션이 Full Feature 단계 진입 시 conn 을 적합한 poll group 으로 이전.
 *
 * @conn: full_feature 진입 직전인 연결.
 *
 * - Discovery/Non-normal 세션은 acceptor 스레드에 그대로 둠 (트래픽 적음).
 * - target 의 첫 conn 이면 가장 한가한 pg 선택 + target->pg 저장.
 * - 같은 target 의 후속 conn 은 이미 정해진 target->pg 에 합류 (cache locality).
 * - 현재 pg 에서 sock 분리 후, send_msg 로 새 pg 스레드에서 LUN open + sock 등록.
 *
 * 호출 위치: iscsi.c 의 login 처리 마지막 단계(state 전이 직전).
 */
void
iscsi_conn_schedule(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_poll_group	*pg;
	struct spdk_iscsi_tgt_node	*target;

	if (conn->sess->session_type != SESSION_TYPE_NORMAL) {
		/* Leave all non-normal sessions on the acceptor
		 * thread. */
		return;
	}
	pthread_mutex_lock(&g_iscsi.mutex);

	target = conn->sess->target;
	pthread_mutex_lock(&target->mutex);
	target->num_active_conns++;
	if (target->num_active_conns == 1) {
		/**
		 * This is the only active connection for this target node.
		 *  Pick the idlest poll group.
		 */
		pg = iscsi_get_idlest_poll_group();
		assert(pg != NULL);

		pg->num_active_targets++;               /* [한국어] 선택된 pg 의 target 수 증가. */

		/* Save the pg in the target node so it can be used for any other connections to this target node. */
		target->pg = pg;                        /* [한국어] target 단위로 pg 고정 — 같은 LUN io_channel 재사용. */
	} else {
		/**
		 * There are other active connections for this target node.
		 */
		pg = target->pg;                        /* [한국어] 이미 정해진 pg 사용. */
	}

	pthread_mutex_unlock(&target->mutex);
	pthread_mutex_unlock(&g_iscsi.mutex);

	assert(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(conn->pg)) ==
	       spdk_get_thread());                     /* [한국어] 현재 conn 의 owning thread 에서만 호출되어야 함. */

	/* Remove this connection from the previous poll group */
	iscsi_poll_group_remove_conn(conn->pg, conn);

	conn->pg = pg;                                  /* [한국어] 새 pg 로 교체 (sock_cb 가 호출되기 전에). */
	conn->scheduled = 1;                            /* [한국어] schedule 적용됨 표시 — stop 시 카운터 감소 조건. */

	spdk_thread_send_msg(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(pg)),
			     iscsi_conn_full_feature_migrate, conn); /* [한국어] 새 pg 스레드에서 sock 재등록 + LUN open. */
}

/*
 * [한국어]
 * logout_timeout - logout 응답 송신 후 일정 시간 내 RST/close 가 없으면 EXITING 강제.
 */
static int
logout_timeout(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	if (conn->state < ISCSI_CONN_STATE_EXITING) {
		conn->state = ISCSI_CONN_STATE_EXITING;
	}

	spdk_poller_unregister(&conn->logout_timer);

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * iscsi_conn_logout - LogoutResponse 를 보낸 직후 호출되어 정리 타이머 가동.
 *
 * @conn: 대상 연결.
 *
 * iscsi.c 의 logout 처리 흐름에서 호출. is_logged_out=true 표시 + 1초(또는 설정값) 후 EXITING.
 */
void
iscsi_conn_logout(struct spdk_iscsi_conn *conn)
{
	conn->is_logged_out = true;                     /* [한국어] logout 완료 플래그. */
	conn->logout_timer = SPDK_POLLER_REGISTER(logout_timeout, conn, ISCSI_LOGOUT_TIMEOUT * 1000000);
}

/*
 * [한국어]
 * iscsi_conn_get_state - state enum → 짧은 문자열 (RPC dump 용).
 */
static const char *
iscsi_conn_get_state(struct spdk_iscsi_conn *conn)
{
	switch (conn->state) {
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_CONN_STATE_INVALID, "invalid");
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_CONN_STATE_RUNNING, "running");
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_CONN_STATE_EXITING, "exiting");
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_CONN_STATE_EXITED, "exited");
	}
	return "unknown";
}

/*
 * [한국어]
 * iscsi_conn_get_login_phase - login phase enum → 문자열 (RPC dump 용).
 */
static const char *
iscsi_conn_get_login_phase(struct spdk_iscsi_conn *conn)
{
	switch (conn->login_phase) {
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_SECURITY_NEGOTIATION_PHASE, "security_negotiation_phase");
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_OPERATIONAL_NEGOTIATION_PHASE, "operational_negotiation_phase");
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_FULL_FEATURE_PHASE, "full_feature_phase");
	}
	return "not_started";
}

/*
 * [한국어]
 * iscsi_conn_trace - SPDK trace framework 에 iSCSI 모듈의 owner/object/tpoint 등록.
 *
 * SPDK_TRACE_REGISTER_FN 에 의해 모듈 로드 시 자동 호출.
 * 등록된 tpoint 들은 spdk_trace_record(...) 호출로 사용되고, spdk_trace 도구로 시각화.
 */
static void
iscsi_conn_trace(void)
{
	spdk_trace_register_owner_type(OWNER_TYPE_ISCSI_CONN, 'c'); /* [한국어] iSCSI conn 을 trace 의 owner type 으로 등록(문자 'c'). */
	spdk_trace_register_object(OBJECT_ISCSI_PDU, 'p');         /* [한국어] iSCSI PDU 를 trace object type 으로 등록. */
	spdk_trace_register_description("ISCSI_READ_DONE", TRACE_ISCSI_READ_FROM_SOCKET_DONE,
					OWNER_TYPE_ISCSI_CONN, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("ISCSI_READ_PDU", TRACE_ISCSI_READ_PDU,
					OWNER_TYPE_ISCSI_CONN, OBJECT_ISCSI_PDU, 1,
					SPDK_TRACE_ARG_TYPE_INT, "opc");
	spdk_trace_register_description("ISCSI_TASK_DONE", TRACE_ISCSI_TASK_DONE,
					OWNER_TYPE_ISCSI_CONN, OBJECT_SCSI_TASK, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("ISCSI_TASK_QUEUE", TRACE_ISCSI_TASK_QUEUE,
					OWNER_TYPE_ISCSI_CONN, OBJECT_SCSI_TASK, 1,
					SPDK_TRACE_ARG_TYPE_PTR, "pdu");
	spdk_trace_register_description("ISCSI_TASK_EXECUTED", TRACE_ISCSI_TASK_EXECUTED,
					OWNER_TYPE_ISCSI_CONN, OBJECT_ISCSI_PDU, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("ISCSI_PDU_COMPLETED", TRACE_ISCSI_PDU_COMPLETED,
					OWNER_TYPE_ISCSI_CONN, OBJECT_ISCSI_PDU, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_QUEUE, OBJECT_ISCSI_PDU, 0);     /* [한국어] sock 송신큐 queue 이벤트가 PDU object 와 관련 있음을 trace 에 알림. */
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_PEND, OBJECT_ISCSI_PDU, 0);
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_COMPLETE, OBJECT_ISCSI_PDU, 0);
}
SPDK_TRACE_REGISTER_FN(iscsi_conn_trace, "iscsi_conn", TRACE_GROUP_ISCSI)        /* [한국어] 부팅 시 iscsi_conn_trace 자동 호출 — iscsi 그룹 소속. */

/*
 * [한국어]
 * iscsi_conn_info_json - 단일 conn 의 상태를 JSON 객체로 직렬화 (RPC iscsi_get_connections 응답).
 *
 * @w:    JSON write context.
 * @conn: 직렬화 대상.
 *
 * is_valid==0 슬롯은 출력하지 않음. sess 가 아직 없으면 tsih=-1 로 표기.
 */
void
iscsi_conn_info_json(struct spdk_json_write_ctx *w, struct spdk_iscsi_conn *conn)
{
	uint16_t tsih;

	if (!conn->is_valid) {                          /* [한국어] free 풀 슬롯은 무시. */
		return;
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_int32(w, "id", conn->id); /* [한국어] 슬롯 ID. */

	spdk_json_write_named_int32(w, "cid", conn->cid); /* [한국어] iSCSI Connection ID. */

	/*
	 * If we try to return data for a connection that has not
	 *  logged in yet, the session will not be set.  So in this
	 *  case, return -1 for the tsih rather than segfaulting
	 *  on the null conn->sess.
	 */
	if (conn->sess == NULL) {
		tsih = -1;
	} else {
		tsih = conn->sess->tsih;
	}
	spdk_json_write_named_int32(w, "tsih", tsih);   /* [한국어] Target Session Identifying Handle. */

	spdk_json_write_named_string(w, "state", iscsi_conn_get_state(conn));

	spdk_json_write_named_string(w, "login_phase", iscsi_conn_get_login_phase(conn));

	spdk_json_write_named_string(w, "initiator_addr", conn->initiator_addr);

	spdk_json_write_named_string(w, "target_addr", conn->target_addr);

	spdk_json_write_named_string(w, "target_node_name", conn->target_short_name);

	spdk_json_write_named_string(w, "thread_name",
				     spdk_thread_get_name(spdk_get_thread())); /* [한국어] 호출이 일어난 스레드 이름 (디버깅용). */

	spdk_json_write_object_end(w);
}
