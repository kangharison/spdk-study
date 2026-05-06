/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] JSON-RPC server transport (TCP/UDS) 구현 (jsonrpc_server_tcp.c)
 *
 * === 파일의 역할 ===
 * JSON-RPC 서버의 transport-side 코드. POSIX socket API(socket/bind/listen/accept/
 * recv/send/close)를 직접 사용하여 listen 소켓 1개 + 동시 64개 클라이언트
 * 연결을 polled-mode로 처리한다. 주요 책임:
 *   (1) spdk_jsonrpc_server_listen(): listen socket 생성 (AF_UNIX 또는 AF_INET),
 *       SOCK_NONBLOCK | SOCK_CLOEXEC 적용, conns_array[64] 정적 풀 초기화.
 *   (2) spdk_jsonrpc_server_poll(): reactor poller가 주기 호출 — accept 1회,
 *       각 conn에 대해 send → recv 순서로 진행, closed conn cleanup.
 *   (3) jsonrpc_server_accept/_conn_recv/_conn_send: 각 단계의 한 라운드 처리.
 *   (4) jsonrpc_server_send_response: 응답 큐 enqueue (다른 thread도 호출 가능).
 *   (5) jsonrpc_server_handle_request/_handle_error: server.c의 parser가 콜하는
 *       transport-side dispatch stub — 사용자 콜백으로 진입 / 표준 에러 응답.
 *   (6) close_cb 등록/해제: 연결 종료 시 1회 호출되는 사용자 cleanup 훅.
 * 모든 I/O는 non-blocking이고 커널을 polling으로 짜내므로 인터럽트/문맥교환이 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (외부 → 내부):
 *   사용자(앱 init): spdk_jsonrpc_server_listen("/var/tmp/spdk.sock", ...)
 *     → socket()/bind()/listen() → server 핸들 반환.
 *   사용자(reactor poller, 주기): spdk_jsonrpc_server_poll(server)
 *     → accept_loop(free_conns에 빈자리 있을 때) →
 *       각 conn별 jsonrpc_server_conn_send(send_queue를 socket으로 flush) →
 *       jsonrpc_server_conn_recv(socket→recv_buf→jsonrpc_parse_request 반복) →
 *       parse_request → parse_single_request → handle_request → 사용자 핸들러 →
 *       응답 작성 → end_response → jsonrpc_server_send_response → send_queue.
 *   사용자(앱 종료): spdk_jsonrpc_server_shutdown → 모든 conn close_cb + close.
 * 실행 컨텍스트: SPDK app thread(주로 main reactor의 default poller). 비동기
 * 핸들러가 다른 thread에서 send_response를 호출해도 안전하도록 큐 락 보호.
 *
 * === 타 모듈과의 연결 ===
 * 의존: jsonrpc_internal.h(공용 자료구조), spdk/string.h(spdk_strerror, spdk_parse_ip_addr 등),
 *       spdk/util.h(spdk_fd_set_nonblock 등).
 * 시스템 호출: socket(2), setsockopt(2)(SO_REUSEADDR), bind(2), listen(2), accept(2),
 *              recv(2), send(2), close(2). UDS의 경우 path가 sockaddr_un.sun_path에 들어감.
 * 데이터 흐름: 클라이언트 socket → recv() → conn->recv_buf → parse_request →
 *              request->send_buf (응답 작성) → send() → 클라이언트 socket.
 * 공유 자료구조: spdk_jsonrpc_server / _server_conn / _request — 모두 internal.h 정의.
 * 호출되는 server.c 함수: jsonrpc_parse_request, jsonrpc_free_request,
 * jsonrpc_complete_request, spdk_jsonrpc_send_error_response.
 * 호출하는 사용자 콜백: server->handle_request (RPC 메서드 디스패치),
 * conn->close_cb (연결 종료 이벤트 통지).
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_jsonrpc_server_listen(): listen 소켓 생성 + conns_array 초기화 + 풀 등록.
 *   - spdk_jsonrpc_server_shutdown(): 모든 conn 강제 종료 + listen 소켓 close + free.
 *   - spdk_jsonrpc_server_poll(): main poller 진입점 — accept + send + recv.
 *   - jsonrpc_server_accept(): 한 번의 accept 시도, 풀에서 conn 슬롯 가져오기.
 *   - jsonrpc_server_conn_recv(): recv() + jsonrpc_parse_request 반복.
 *   - jsonrpc_server_conn_send(): send_queue를 socket으로 부분 송신, 완료 시 free.
 *   - jsonrpc_server_dequeue_request(): send_queue에서 head 꺼내기 (queue_lock 보호).
 *   - jsonrpc_server_send_response(): outstanding→send 큐 이동 (다른 thread도 호출 가능).
 *   - jsonrpc_server_conn_close()/_remove(): 연결 정리 2단계 (close_cb 호출 + 풀 반환).
 *   - jsonrpc_server_handle_request/_handle_error: server.c parser가 호출하는 stub.
 *   - spdk_jsonrpc_conn_add_close_cb/_del_close_cb: 사용자 close 콜백 (de)register.
 */

#include "jsonrpc_internal.h"      /* [한국어] 본 라이브러리 내부 자료구조 */
#include "spdk/string.h"           /* [한국어] spdk_strerror(errno→문자열) */
#include "spdk/util.h"             /* [한국어] spdk_fd_set_nonblock — fd O_NONBLOCK fcntl 래퍼 */

/*
 * [한국어]
 * spdk_jsonrpc_server_listen - JSON-RPC server 인스턴스 생성 + listen 소켓 준비
 *
 * @domain: AF_UNIX (UDS — 권장 전송로 "/var/tmp/spdk.sock") 또는 AF_INET/INET6 (TCP).
 * @protocol: TCP면 IPPROTO_TCP, UDS면 보통 0.
 * @listen_addr: bind할 sockaddr 포인터(사용자가 sockaddr_un 또는 sockaddr_in으로 채움).
 * @addrlen: sockaddr 크기.
 * @handle_request: 메서드 디스패치 사용자 콜백 — server.c의 parse_single_request가
 *                  jsonrpc_server_handle_request를 통해 이 콜백으로 진입한다.
 * @return: 성공 시 server 핸들, 실패 시 NULL(SPDK_ERRLOG로 사유 출력).
 *
 * 라이브러리의 진입점. SPDK 앱이 RPC 서버를 띄우는 표준 시퀀스:
 *   spdk_jsonrpc_server_listen → reactor poller에 spdk_jsonrpc_server_poll 등록 →
 *   사용자 RPC 핸들러 호출 → 종료 시 spdk_jsonrpc_server_shutdown.
 *
 * 동작 단계:
 *   1) server 구조체 calloc.
 *   2) free_conns/conns TAILQ 초기화 후, conns_array[64]를 모두 free_conns로 push.
 *   3) socket(..., SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, ...).
 *      - SOCK_NONBLOCK: polled-mode이므로 accept/recv/send 모두 EAGAIN 반환.
 *      - SOCK_CLOEXEC: fork+exec 시 fd 누수 방지(SPDK 자체 fork 거의 없지만 안전망).
 *   4) SO_REUSEADDR: 비정상 종료 후 즉시 재바인드 가능하도록.
 *   5) bind() → listen(backlog=512). RPC는 control-plane이라 backlog는 넉넉히.
 *
 * 호출 체인: 사용자(spdk_app_start 내부 RPC 초기화) → [이 함수] → POSIX socket API.
 */
struct spdk_jsonrpc_server *
spdk_jsonrpc_server_listen(int domain, int protocol,
			   struct sockaddr *listen_addr, socklen_t addrlen,
			   spdk_jsonrpc_handle_request_fn handle_request)
{
	struct spdk_jsonrpc_server *server;
	int rc, val, i;

	server = calloc(1, sizeof(struct spdk_jsonrpc_server));  /* [한국어] server 인스턴스 zero-init alloc */
	if (server == NULL) {
		/* [한국어] OOM — 호출자에게 NULL 반환, errno는 calloc이 설정 */
		return NULL;
	}

	TAILQ_INIT(&server->free_conns);  /* [한국어] free 리스트 초기화 (head NULL) */
	TAILQ_INIT(&server->conns);       /* [한국어] active 리스트 초기화 */

	for (i = 0; i < SPDK_JSONRPC_MAX_CONNS; i++) {
		/* [한국어] 정적 배열의 모든 슬롯을 free 리스트에 push — accept 때 head에서 꺼냄 */
		TAILQ_INSERT_TAIL(&server->free_conns, &server->conns_array[i], link);
	}

	server->handle_request = handle_request;  /* [한국어] 메서드 디스패치 콜백 저장 (read-only after this) */

	server->sockfd = socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
	/* [한국어] listen 소켓 생성. NONBLOCK은 accept가 즉시 반환하도록, CLOEXEC는
	 * exec 시 fd 누수 방지. 도메인이 AF_UNIX/AF_INET 무관하게 같은 코드 경로. */
	if (server->sockfd < 0) {
		SPDK_ERRLOG("socket() failed\n");
		free(server);
		return NULL;
	}

	val = 1;
	rc = setsockopt(server->sockfd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
	/* [한국어] SO_REUSEADDR — TIME_WAIT 상태의 이전 소켓이 있어도 같은 주소에 즉시 bind 가능.
	 * RPC는 server 재시작이 잦은 control-plane이라 이 옵션이 사실상 필수. */
	if (rc != 0) {
		SPDK_ERRLOG("could not set SO_REUSEADDR sock option: %s\n", spdk_strerror(errno));
		close(server->sockfd);
		free(server);
		return NULL;
	}

	rc = bind(server->sockfd, listen_addr, addrlen);
	/* [한국어] sockaddr 구조체에 따라 UDS path 또는 TCP ip:port 바인딩. */
	if (rc != 0) {
		SPDK_ERRLOG("could not bind JSON-RPC server: %s\n", spdk_strerror(errno));
		close(server->sockfd);
		free(server);
		return NULL;
	}

	rc = listen(server->sockfd, 512);
	/* [한국어] backlog 512 — 동시에 연결 시도가 몰려도 SYN 큐가 넘치지 않도록.
	 * 실제 동시 연결 한도는 SPDK_JSONRPC_MAX_CONNS=64이지만, listen backlog는 충분히 크게 잡음. */
	if (rc != 0) {
		SPDK_ERRLOG("listen() failed, errno = %d\n", errno);
		close(server->sockfd);
		free(server);
		return NULL;
	}

	return server;  /* [한국어] 성공 — 사용자는 이 핸들로 spdk_jsonrpc_server_poll 호출 */
}

/*
 * [한국어]
 * jsonrpc_server_dequeue_request - send_queue head 1건을 락 보호 하에 꺼내기
 *
 * @conn: 대상 연결.
 * @return: head 요청(없으면 NULL).
 *
 * 송신 루프(jsonrpc_server_conn_send)가 다음 송신 대상을 꺼낼 때 사용.
 * spinlock 보호: send_response가 다른 thread에서 enqueue할 수 있어 race 가능.
 */
static struct spdk_jsonrpc_request *
jsonrpc_server_dequeue_request(struct spdk_jsonrpc_server_conn *conn)
{
	struct spdk_jsonrpc_request *request = NULL;

	pthread_spin_lock(&conn->queue_lock);   /* [한국어] send_queue 보호 */
	request = STAILQ_FIRST(&conn->send_queue);  /* [한국어] head peek */
	if (request) {
		STAILQ_REMOVE_HEAD(&conn->send_queue, link);  /* [한국어] head 제거 (소유권 caller로 이전) */
	}
	pthread_spin_unlock(&conn->queue_lock);
	return request;
}

/*
 * [한국어]
 * jsonrpc_server_free_conn_request - 연결 종료 시 모든 in-flight 요청 정리
 *
 * @conn: 종료 중인 연결.
 *
 * conn_close 시점에 호출되어:
 *   1) 부분 송신 중이던 send_request를 free.
 *   2) outstanding_queue의 요청들은 핸들러가 다른 thread에서 실행 중일 수 있어
 *      free하지 않고 conn=NULL로만 마킹 — 핸들러가 send_response 시 conn==NULL 검사로
 *      free_request 호출, byte 송신은 skip.
 *   3) send_queue의 모든 요청은 송신 안 하고 free.
 */
static void
jsonrpc_server_free_conn_request(struct spdk_jsonrpc_server_conn *conn)
{
	struct spdk_jsonrpc_request *request;

	jsonrpc_free_request(conn->send_request);  /* [한국어] 부분 송신 중이던 요청 정리 (NULL이면 no-op) */
	conn->send_request = NULL ;

	pthread_spin_lock(&conn->queue_lock);
	/* There might still be some requests being processed.
	 * We need to tell them that this connection is closed. */
	STAILQ_FOREACH(request, &conn->outstanding_queue, link) {
		/* [한국어] outstanding_queue의 요청들은 다른 thread의 핸들러가 실행 중일 수 있음.
		 * 여기서 free하면 use-after-free 위험 — conn=NULL로만 표시.
		 * 핸들러는 응답 작성 후 send_response에서 conn==NULL을 보고 free_request만 수행. */
		request->conn = NULL;
	}
	pthread_spin_unlock(&conn->queue_lock);

	while ((request = jsonrpc_server_dequeue_request(conn)) != NULL) {
		/* [한국어] send_queue에 들어온 응답들은 핸들러 측 처리가 끝난 상태 — 안전하게 free.
		 * 송신 안 하고 폐기되지만 연결이 닫혔으므로 클라이언트도 응답을 기다리지 않음. */
		jsonrpc_free_request(request);
	}
}

/*
 * [한국어]
 * jsonrpc_server_conn_close - 단일 연결의 socket close + close_cb 호출
 *
 * @conn: 종료할 연결.
 *
 * 멱등(idempotent): sockfd<0이면 이미 close된 상태로 간주하고 no-op.
 * close 순서: closed=true → in-flight 요청 정리 → close(sockfd) → sockfd=-1 →
 *             close_cb 호출(등록되어 있으면).
 *
 * 호출 체인:
 *   spdk_jsonrpc_server_poll() (closed && outstanding==0 시),
 *   spdk_jsonrpc_server_shutdown() (강제 종료) →
 *   [이 함수] → close(2), close_cb.
 */
static void
jsonrpc_server_conn_close(struct spdk_jsonrpc_server_conn *conn)
{
	conn->closed = true;  /* [한국어] 이후 recv 루프는 이 conn을 skip */

	if (conn->sockfd >= 0) {
		/* [한국어] 아직 close되지 않은 경우만 — 이중 close 방지 */
		jsonrpc_server_free_conn_request(conn);  /* [한국어] in-flight 요청 정리 */
		close(conn->sockfd);                     /* [한국어] 커널 fd 해제 — 클라이언트 측에 EOF 전달 */
		conn->sockfd = -1;                       /* [한국어] sentinel — 다음 호출은 no-op */

		if (conn->close_cb) {
			/* [한국어] 사용자가 등록한 cleanup 콜백 호출 (예 spdk/rpc.c의 RPC 등록표 정리) */
			conn->close_cb(conn, conn->close_cb_ctx);
		}
	}
}

/*
 * [한국어]
 * spdk_jsonrpc_server_shutdown - server 인스턴스 강제 종료 + 메모리 free (공개 API)
 *
 * @server: 종료할 server.
 *
 * 앱 종료 시 호출. listen 소켓 close → 모든 active conn 강제 종료 → server free.
 * 새 연결은 listen 소켓이 닫혀 들어오지 못하고, 기존 연결은 in-flight 요청을 폐기.
 * conns_array는 server 구조체 내부에 inline이므로 별도 free 불필요.
 *
 * 호출 체인: 사용자(spdk_app_stop 내부) → [이 함수] → conn_close × N → free.
 */
void
spdk_jsonrpc_server_shutdown(struct spdk_jsonrpc_server *server)
{
	struct spdk_jsonrpc_server_conn *conn;

	close(server->sockfd);  /* [한국어] listen 소켓 close — 새 accept 차단 */

	TAILQ_FOREACH(conn, &server->conns, link) {
		/* [한국어] 모든 active conn에 대해 close (강제). cleanup 처리는 conn_close가 담당.
		 * 주의: conn_remove까지는 안 부르므로 conn 자체는 conns_array에 남는다 — 곧 free된다. */
		jsonrpc_server_conn_close(conn);
	}

	free(server);  /* [한국어] server 구조체와 conns_array를 모두 한 번에 free (inline 배열) */
}

/*
 * [한국어]
 * jsonrpc_server_conn_remove - 종료된 conn을 active 리스트에서 빼서 free 풀로 반환
 *
 * @conn: 닫힌(sockfd==-1, outstanding==0) 연결.
 *
 * conn_close가 시스템 리소스를 정리한 후, 슬롯을 재사용 가능 상태로 만든다.
 * spinlock destroy → assertion(send_queue 비어 있음) → conns→free_conns 이동.
 *
 * 호출 체인: spdk_jsonrpc_server_poll() (sockfd==-1 && outstanding==0) →
 *           [이 함수] → conn_close (멱등이므로 안전) + 풀 반환.
 */
static void
jsonrpc_server_conn_remove(struct spdk_jsonrpc_server_conn *conn)
{
	struct spdk_jsonrpc_server *server = conn->server;

	jsonrpc_server_conn_close(conn);  /* [한국어] 멱등 — 이미 close됐으면 no-op */

	pthread_spin_destroy(&conn->queue_lock);  /* [한국어] spinlock 자원 해제 — 다음 accept 시 재초기화 */
	assert(STAILQ_EMPTY(&conn->send_queue));  /* [한국어] free_conn_request에서 모두 비웠어야 함 */

	TAILQ_REMOVE(&server->conns, conn, link);     /* [한국어] active 리스트에서 제거 */
	TAILQ_INSERT_HEAD(&server->free_conns, conn, link);  /* [한국어] free 풀의 head로 반환 (LIFO 재사용) */
}

/*
 * [한국어]
 * spdk_jsonrpc_conn_add_close_cb - 연결 종료 시 호출될 cleanup 콜백 등록 (공개 API)
 *
 * @conn: 콜백을 등록할 연결.
 * @cb: 종료 시 호출될 함수(현재 conn 1개당 1개만 등록 가능).
 * @ctx: cb에 전달할 user context.
 * @return: 0 성공, -EEXIST(같은 cb/ctx 이미 등록됨), -ENOSPC(다른 cb 등록되어 있음).
 *
 * 멀티슬롯이 아닌 단일 슬롯 콜백 — 한 conn에 한 명의 owner만 cleanup 책임을 진다.
 * spdk/rpc.c가 사용 — 메서드 등록표의 conn-scoped 데이터를 정리.
 *
 * 호출 체인: 사용자 RPC 핸들러 → [이 함수] → queue_lock 보호 하에 등록.
 */
int
spdk_jsonrpc_conn_add_close_cb(struct spdk_jsonrpc_server_conn *conn,
			       spdk_jsonrpc_conn_closed_fn cb, void *ctx)
{
	int rc = 0;

	pthread_spin_lock(&conn->queue_lock);  /* [한국어] close_cb 멤버 보호 — conn_close가 다른 thread에서 읽음 */
	if (conn->close_cb == NULL) {
		/* [한국어] 비어 있으면 등록 — 정상 경로 */
		conn->close_cb = cb;
		conn->close_cb_ctx = ctx;
	} else {
		/* [한국어] 이미 누군가 등록 — 같은 (cb,ctx)면 EEXIST(중복 등록), 다르면 ENOSPC(슬롯 점유) */
		rc = conn->close_cb == cb && conn->close_cb_ctx == ctx ? -EEXIST : -ENOSPC;
	}
	pthread_spin_unlock(&conn->queue_lock);

	return rc;
}

/*
 * [한국어]
 * spdk_jsonrpc_conn_del_close_cb - 등록된 close 콜백 해제 (공개 API)
 *
 * @conn: 콜백이 등록된 연결.
 * @cb: 등록 시 사용한 cb (검증).
 * @ctx: 등록 시 사용한 ctx (검증).
 * @return: 0 성공, -ENOENT(미등록 또는 cb/ctx 불일치).
 *
 * 정확히 같은 (cb, ctx)로 등록된 콜백만 제거 가능 — 다른 모듈이 잘못 해제하지 않도록.
 *
 * 호출 체인: 사용자 RPC 핸들러(연결 종료 추적이 더 이상 필요 없을 때) → [이 함수].
 */
int
spdk_jsonrpc_conn_del_close_cb(struct spdk_jsonrpc_server_conn *conn,
			       spdk_jsonrpc_conn_closed_fn cb, void *ctx)
{
	int rc = 0;

	pthread_spin_lock(&conn->queue_lock);
	if (conn->close_cb == NULL || conn->close_cb != cb || conn->close_cb_ctx != ctx) {
		/* [한국어] 미등록 또는 (cb,ctx) 불일치 — 잘못된 해제 시도 */
		rc = -ENOENT;
	} else {
		/* [한국어] 정상 — 슬롯 비우기 (ctx는 다음 등록자가 어차피 덮어씀) */
		conn->close_cb = NULL;
	}
	pthread_spin_unlock(&conn->queue_lock);

	return rc;
}

/*
 * [한국어]
 * jsonrpc_server_accept - 한 번의 accept 시도 + free 풀에서 conn 슬롯 활성화
 *
 * @server: 대상 server.
 * @return: 0 성공(또는 EAGAIN — non-blocking에서 정상), -1 치명적 에러.
 *
 * server_poll에서 free_conns에 빈자리가 있을 때만 호출된다(64개 슬롯 만석이면 호출 안 함).
 * accept가 성공하면 free_conns의 head 슬롯을 가져와 초기화, conns 리스트로 이동.
 *
 * 한 번에 1개만 accept하므로 동시 다발 연결 시도가 있어도 server_poll은 라운드 로빈으로
 * 처리된다 — control plane이라 burst connection이 없어 충분.
 */
static int
jsonrpc_server_accept(struct spdk_jsonrpc_server *server)
{
	struct spdk_jsonrpc_server_conn *conn;
	int rc;

	rc = accept(server->sockfd, NULL, NULL);
	/* [한국어] accept(2): listen 큐에서 1건의 연결을 가져옴.
	 * NULL/NULL: peer 주소 정보 미수집(RPC는 주소 기반 인증/라우팅 안 함).
	 * SOCK_NONBLOCK 상속하지는 않으므로 아래에서 spdk_fd_set_nonblock 별도 호출. */
	if (rc >= 0) {
		conn = TAILQ_FIRST(&server->free_conns);
		assert(conn != NULL);  /* [한국어] caller가 free_conns 비어있지 않음을 보장 */

		conn->server = server;       /* [한국어] 역참조 — handle_request 디스패치에 사용 */
		conn->sockfd = rc;           /* [한국어] accept가 반환한 새 socket fd */
		conn->closed = false;        /* [한국어] 새 연결 — 활성 상태 */
		conn->recv_len = 0;          /* [한국어] recv 버퍼 비어 있음 */
		conn->outstanding_requests = 0;  /* [한국어] in-flight 요청 0 */
		STAILQ_INIT(&conn->send_queue);  /* [한국어] 송신 대기 큐 초기화 */
		STAILQ_INIT(&conn->outstanding_queue);  /* [한국어] 처리 중 큐 초기화 */
		conn->send_request = NULL;   /* [한국어] 부분 송신 cursor 없음 */

		if (pthread_spin_init(&conn->queue_lock, PTHREAD_PROCESS_PRIVATE)) {
			/* [한국어] PROCESS_PRIVATE: 같은 프로세스 내 thread간 동기화 한정.
			 * cross-process 공유 불필요(SPDK app은 단일 프로세스). */
			SPDK_ERRLOG("Unable to create queue lock for socket: %d", conn->sockfd);
			close(conn->sockfd);
			return -1;
		}

		if (spdk_fd_set_nonblock(conn->sockfd) < 0) {
			/* [한국어] fcntl(F_SETFL, O_NONBLOCK) 래퍼 — accept 결과 fd는 기본 blocking이므로
			 * polled-mode를 위해 명시적으로 non-blocking 전환 필수. */
			close(conn->sockfd);
			pthread_spin_destroy(&conn->queue_lock);
			return -1;
		}

		TAILQ_REMOVE(&server->free_conns, conn, link);  /* [한국어] free 풀에서 제거 */
		TAILQ_INSERT_TAIL(&server->conns, conn, link);  /* [한국어] active 리스트에 추가 */
		return 0;
	}

	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
		/* [한국어] non-blocking이므로 listen 큐가 비면 EAGAIN — 정상, 다음 poll에서 재시도. */
		return 0;
	}

	return -1;  /* [한국어] 다른 errno는 진짜 에러 — 호출자가 server 종료 여부 결정 */
}

/*
 * [한국어]
 * jsonrpc_server_handle_request - parse 단계의 success path → 사용자 콜백 디스패치 stub
 *
 * @request, @method, @params: parse_single_request가 검증 통과 후 전달.
 *
 * server.c의 parser는 transport-agnostic하게 짜여 있어 사용자 콜백 호출을 직접
 * 하지 않고 이 stub을 거친다. server->handle_request는 spdk_jsonrpc_server_listen 등록 콜백.
 */
void
jsonrpc_server_handle_request(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *method, const struct spdk_json_val *params)
{
	request->conn->server->handle_request(request, method, params);
	/* [한국어] 사용자 RPC 핸들러로 진입 — 보통 spdk/rpc.c의 jsonrpc_handler가
	 * method 이름을 보고 등록표에서 lookup 후 실제 핸들러 함수 호출. */
}

/*
 * [한국어]
 * jsonrpc_server_handle_error - parse 실패 등에 대한 표준 에러 응답
 *
 * @request: 에러 응답을 보낼 요청.
 * @error: SPDK_JSONRPC_ERROR_* 표준 코드.
 *
 * code별 표준 메시지 문자열을 매핑하여 spdk_jsonrpc_send_error_response 호출.
 * 사양 §5.1의 표준 에러 코드들에 사람이 읽을 수 있는 영문 메시지 부여.
 */
void
jsonrpc_server_handle_error(struct spdk_jsonrpc_request *request, int error)
{
	const char *msg;

	switch (error) {
	case SPDK_JSONRPC_ERROR_PARSE_ERROR:        /* [한국어] -32700 invalid JSON */
		msg = "Parse error";
		break;

	case SPDK_JSONRPC_ERROR_INVALID_REQUEST:    /* [한국어] -32600 valid JSON이지만 RPC 스키마 위반 */
		msg = "Invalid request";
		break;

	case SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND:   /* [한국어] -32601 등록되지 않은 method */
		msg = "Method not found";
		break;

	case SPDK_JSONRPC_ERROR_INVALID_PARAMS:     /* [한국어] -32602 params 디코딩 실패 */
		msg = "Invalid parameters";
		break;

	case SPDK_JSONRPC_ERROR_INTERNAL_ERROR:     /* [한국어] -32603 핸들러 내부 에러 */
		msg = "Internal error";
		break;

	default:
		msg = "Error";  /* [한국어] 사용자 정의 코드 등 — generic 메시지 */
		break;
	}

	spdk_jsonrpc_send_error_response(request, error, msg);
	/* [한국어] server.c의 표준 에러 응답 빌더로 진입 — {jsonrpc, id, "error":{code, message}} 작성. */
}

/*
 * [한국어]
 * jsonrpc_server_conn_recv - socket에서 byte 수신 + 가능한 모든 요청 파싱·디스패치
 *
 * @conn: 대상 연결.
 * @return: 0 정상(EAGAIN 포함), -1 치명적 에러(connection 종료해야 함).
 *
 * 한 번의 poll 라운드에서 수행:
 *   1) recv()를 한 번만 호출(여유 공간만큼) — non-blocking이므로 EAGAIN이면 0 반환.
 *   2) recv()==0이면 클라이언트 정상 종료(EOF) — closed=true 마킹.
 *   3) recv_buf의 누적 데이터를 jsonrpc_parse_request로 반복 파싱:
 *      각 호출이 1건의 요청을 소비. 0 반환(incomplete)이면 종료, -1이면 치명적.
 *   4) 파싱된 만큼을 memmove로 buf 시작으로 압축 — 다음 recv가 이어 쓸 자리 확보.
 *
 * recv를 한 번만 호출하는 이유: poll fairness — 한 conn에서 무한 polling 시
 * 다른 conn이 starvation. 한 라운드 1회 recv는 SPDK의 polled-mode 표준 패턴.
 */
static int
jsonrpc_server_conn_recv(struct spdk_jsonrpc_server_conn *conn)
{
	ssize_t rc, offset;
	size_t recv_avail = SPDK_JSONRPC_RECV_BUF_SIZE - conn->recv_len;
	/* [한국어] 잔여 capacity — recv_buf는 32KB 고정, recv_len은 누적 byte. */

	rc = recv(conn->sockfd, conn->recv_buf + conn->recv_len, recv_avail, 0);
	/* [한국어] recv(2): non-blocking. flags=0 (regular). 잔여 자리에 직접 write. */
	if (rc == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			/* [한국어] 정상 — 받을 데이터 없음(EAGAIN), 또는 시그널 인터럽트.
			 * 다음 poll에서 재시도. */
			return 0;
		}
		SPDK_DEBUGLOG(rpc, "recv() failed: %s\n", spdk_strerror(errno));
		return -1;  /* [한국어] ECONNRESET 등 — 진짜 에러. caller가 conn 종료. */
	}

	if (rc == 0) {
		/* [한국어] EOF — 클라이언트가 정상적으로 close()함. closed=true 마킹.
		 * outstanding_requests==0이 되면 다음 poll에서 정리됨. */
		SPDK_DEBUGLOG(rpc, "remote closed connection\n");
		conn->closed = true;
		return 0;
	}

	conn->recv_len += rc;  /* [한국어] 받은 byte만큼 buf 길이 증가 */

	offset = 0;
	do {
		rc = jsonrpc_parse_request(conn, conn->recv_buf + offset, conn->recv_len - offset);
		/* [한국어] 한 번에 1개 요청 파싱·디스패치. 반환값:
		 *   >0 = 소비한 byte 수(다음 요청을 위해 offset 가산),
		 *   0  = INCOMPLETE(이번 라운드 종료),
		 *   <0 = 치명적 에러(파싱 실패는 server.c 안에서 에러 응답 보낸 후 -1 반환). */
		if (rc < 0) {
			SPDK_ERRLOG("jsonrpc parse request failed\n");
			return -1;
		}

		offset += rc;
	} while (rc > 0);
	/* [한국어] 한 chunk에 여러 개 요청이 들어와 있을 수 있으므로 incomplete까지 반복. */

	if (offset > 0) {
		/*
		 * Successfully parsed a requests - move any data past the end of the
		 * parsed requests down to the beginning.
		 */
		assert((size_t)offset <= conn->recv_len);
		memmove(conn->recv_buf, conn->recv_buf + offset, conn->recv_len - offset);
		/* [한국어] 소비된 부분을 잘라내고 잔여(incomplete) 부분을 buf 시작으로 이동.
		 * memmove는 overlap 안전 — memcpy를 쓰면 UB. */
		conn->recv_len -= offset;
	}

	return 0;
}

/*
 * [한국어]
 * jsonrpc_server_send_response - 응답 작성 완료 후 send_queue로 이동
 *
 * @request: 응답 작성이 끝난 요청.
 *
 * "Might be called from any thread" — 비동기 RPC 핸들러가 다른 thread에서 완료를
 * 알릴 수 있어 conn->queue_lock으로 outstanding_queue↔send_queue 이동을 보호.
 *
 * conn==NULL인 경우는 jsonrpc_server_free_conn_request가 미리 마킹한 상태 —
 * 연결이 이미 닫혔으므로 응답 송신 불가, free_request로만 정리.
 *
 * 호출 체인:
 *   end_response() / skip_response() (server.c) → [이 함수] →
 *   queue 이동 → jsonrpc_server_conn_send() (다음 poll 라운드).
 */
void
jsonrpc_server_send_response(struct spdk_jsonrpc_request *request)
{
	struct spdk_jsonrpc_server_conn *conn = request->conn;

	if (conn == NULL) {
		/* We cannot respond to the request, because the connection is closed. */
		/* [한국어] free_conn_request가 conn을 NULL로 마킹한 케이스 — 연결 끊김. */
		SPDK_WARNLOG("Unable to send response: connection closed.\n");
		jsonrpc_free_request(request);  /* [한국어] queue 작업 없이 메모리만 해제 */
		return;
	}

	/* Queue the response to be sent */
	pthread_spin_lock(&conn->queue_lock);
	STAILQ_REMOVE(&conn->outstanding_queue, request, spdk_jsonrpc_request, link);
	/* [한국어] 처리 중 큐에서 제거. 비동기 핸들러 thread도 호출하므로 락 필수. */
	STAILQ_INSERT_TAIL(&conn->send_queue, request, link);
	/* [한국어] 송신 대기 큐의 tail에 추가 — server poll thread가 dequeue해서 send. */
	pthread_spin_unlock(&conn->queue_lock);
}


/*
 * [한국어]
 * jsonrpc_server_conn_send - send_queue를 socket으로 부분 송신
 *
 * @conn: 대상 연결.
 * @return: 0 정상, -1 치명적 send 에러(연결 종료).
 *
 * 한 라운드에서 가능한 만큼 송신:
 *   1) outstanding_requests==0이면 short-circuit(아무것도 송신할 게 없음).
 *   2) send_request가 NULL이면 send_queue에서 head dequeue.
 *   3) send() 호출 — EAGAIN이면 다음 라운드 재시도.
 *   4) 한 요청을 모두 송신했으면 jsonrpc_complete_request로 free, 다음 요청 시도(more 라벨).
 *
 * 부분 송신을 send_offset/send_len으로 추적 — TCP 송신 버퍼가 가득 차면 부분만 나가고
 * 다음 poll에서 이어서 송신. label 'more'는 "다음 응답 시도" 루프 — goto가 가독성 더 나음.
 */
static int
jsonrpc_server_conn_send(struct spdk_jsonrpc_server_conn *conn)
{
	struct spdk_jsonrpc_request *request;
	ssize_t rc;

more:
	if (conn->outstanding_requests == 0) {
		/* [한국어] in-flight 요청 0이면 큐도 비어 있음 — 빠른 종료 (락 없이 read 안전:
		 * outstanding_requests는 핸들러 thread에서 증가하지만 send 라운드에 영향 없음). */
		return 0;
	}

	if (conn->send_request == NULL) {
		/* [한국어] 부분 송신 cursor 없음 — 새 요청 dequeue. */
		conn->send_request = jsonrpc_server_dequeue_request(conn);
	}

	request = conn->send_request;
	if (request == NULL) {
		/* Nothing to send right now */
		/* [한국어] outstanding_requests>0이지만 send_queue는 비어 있음 —
		 * 핸들러가 응답 작성 중. 다음 라운드까지 대기. */
		return 0;
	}

	if (request->send_offset == 0) {
		/* A byte for the null terminator is included in the send buffer. */
		/* [한국어] send_buf는 +1 alloc되어 있어 NUL 종단 자리 확보됨. trace log용. */
		request->send_buf[request->send_len] = '\0';
	}

	if (request->send_len > 0) {
		rc = send(conn->sockfd, request->send_buf + request->send_offset,
			  request->send_len, 0);
		/* [한국어] send(2): non-blocking. flags=0. 부분 송신 가능. */
		if (rc < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				/* [한국어] TCP 송신 버퍼 가득 — 다음 라운드 재시도. */
				return 0;
			}

			SPDK_DEBUGLOG(rpc, "send() failed: %s\n", spdk_strerror(errno));
			return -1;  /* [한국어] EPIPE 등 — 진짜 에러. caller가 conn 종료. */
		}

		request->send_offset += rc;  /* [한국어] cursor 전진 */
		request->send_len -= rc;     /* [한국어] 잔여 송신 byte 감소 */
	}

	if (request->send_len == 0) {
		/*
		 * Full response has been sent.
		 * Free it and set send_request to NULL to move on to the next queued response.
		 */
		conn->send_request = NULL;
		jsonrpc_complete_request(request);  /* [한국어] trace log + free_request — outstanding_requests 감소 */
		goto more;  /* [한국어] 다음 요청도 즉시 송신 시도 (busy 루프 아님 — EAGAIN/empty면 즉시 break) */
	}

	return 0;  /* [한국어] 부분 송신 — 다음 poll에서 이어서 */
}

/*
 * [한국어]
 * spdk_jsonrpc_server_poll - JSON-RPC server의 메인 polling 진입점 (공개 API)
 *
 * @server: 대상 server.
 * @return: 항상 0 (현재 구현은 fatal 종료가 없음).
 *
 * 사용자가 reactor poller에 등록하여 주기 호출. 한 호출에서 다음을 수행:
 *   Phase 1 (cleanup): 모든 conn을 traverse(_SAFE — remove 가능).
 *     - closed && outstanding==0이면 conn_close,
 *     - sockfd==-1 && outstanding==0이면 conn_remove(슬롯 회수).
 *   Phase 2 (accept): free 슬롯이 있을 때만 1회 accept 시도.
 *   Phase 3 (I/O): 모든 active conn에 대해 send → recv 순서로 1라운드 처리.
 *     send를 먼저 하는 이유: 이미 대기 중인 응답을 빨리 흘려보내 클라이언트
 *     latency 개선. recv는 closed가 false인 conn만(EOF 후 추가 recv 무의미).
 *
 * 호출 체인:
 *   reactor poller (주기 호출) → [이 함수] → accept/recv/send 라운드 →
 *   parse_request → 사용자 핸들러.
 *
 * 실행 컨텍스트: 단일 SPDK app thread. 락 사용은 큐 조작 한정 — non-blocking I/O는
 * 이 thread만 수행하므로 socket 단위 race 없음.
 */
int
spdk_jsonrpc_server_poll(struct spdk_jsonrpc_server *server)
{
	int rc;
	struct spdk_jsonrpc_server_conn *conn, *conn_tmp;

	TAILQ_FOREACH_SAFE(conn, &server->conns, link, conn_tmp) {
		/* [한국어] _SAFE 매크로: traverse 도중 remove 가능 — conn_remove가 list 수정. */

		/* If we can't receive and there are no outstanding requests close the connection. */
		if (conn->closed == true && conn->outstanding_requests == 0) {
			/* [한국어] EOF/에러로 closed 상태이면서 in-flight 요청도 0 → 안전하게 close.
			 * outstanding이 남아 있으면 핸들러가 아직 작업 중이므로 기다린다. */
			jsonrpc_server_conn_close(conn);
		}

		if (conn->sockfd == -1 && conn->outstanding_requests == 0) {
			/* [한국어] close까지 끝났으면 슬롯을 free 풀로 반환. */
			jsonrpc_server_conn_remove(conn);
		}
	}

	/* Check listen socket */
	if (!TAILQ_EMPTY(&server->free_conns)) {
		/* [한국어] 풀에 빈 슬롯이 있을 때만 accept — 한도 초과 시 listen 큐에 쌓아둠. */
		jsonrpc_server_accept(server);
	}

	TAILQ_FOREACH(conn, &server->conns, link) {
		if (conn->sockfd == -1) {
			/* [한국어] cleanup 단계에서 close됐지만 outstanding이 남아 remove 못 한 상태.
			 * I/O 시도 불가 — skip. */
			continue;
		}

		rc = jsonrpc_server_conn_send(conn);  /* [한국어] 큐의 응답을 socket으로 flush */
		if (rc != 0) {
			/* [한국어] EPIPE 등 send 실패 — 연결 종료로 간주. */
			jsonrpc_server_conn_close(conn);
			continue;
		}

		if (!conn->closed) {
			/* [한국어] EOF 받지 않은 active 연결만 recv. */
			rc = jsonrpc_server_conn_recv(conn);
			if (rc != 0) {
				jsonrpc_server_conn_close(conn);
			}
		}
	}

	return 0;  /* [한국어] 항상 성공 — 개별 conn 에러는 conn_close로 처리됨 */
}
