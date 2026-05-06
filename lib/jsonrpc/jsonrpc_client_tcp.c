/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] JSON-RPC client transport (TCP/UDS) 구현 (jsonrpc_client_tcp.c)
 *
 * === 파일의 역할 ===
 * JSON-RPC 클라이언트의 transport-side: POSIX socket API로 서버에 연결하고,
 * 비동기 send/recv를 polled-mode로 구동한다. 주요 책임:
 *   (1) spdk_jsonrpc_client_connect(): "/var/tmp/spdk.sock" 같은 UDS path 또는
 *       "host:port" TCP 주소를 받아 socket(NONBLOCK) + connect 시도. 비동기
 *       connect의 EINPROGRESS는 정상 — poll에서 완료 확인.
 *   (2) spdk_jsonrpc_client_poll(): poll(2)로 1개 fd의 read/write 이벤트 대기 후
 *       jsonrpc_client_send_request / jsonrpc_client_recv 호출.
 *   (3) jsonrpc_client_poll_connecting(): 비동기 connect 진행 중일 때의 별도 poll
 *       경로 — POLLOUT 이벤트로 connect 완료 확인 + SO_ERROR 검사.
 *   (4) recv_buf 동적 확장 (recv_buf_expand): 큰 응답을 받기 위한 2배 grow.
 *   (5) 요청 lifecycle: create_request / free_request / send_request,
 *       응답 lifecycle: get_response / free_response.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 흐름:
 *   사용자(rpc.py 또는 SPDK 내부 도구) →
 *   spdk_jsonrpc_client_connect("/var/tmp/spdk.sock", AF_UNIX) →
 *   loop: spdk_jsonrpc_client_poll(client, timeout_ms) →
 *           connecting 상태면 jsonrpc_client_poll_connecting() →
 *           connected 상태면 jsonrpc_client_poll() →
 *               POLLOUT: jsonrpc_client_send_request → send() →
 *               POLLIN : jsonrpc_client_recv → recv() → jsonrpc_parse_response (client.c) →
 *           응답 ready면 사용자가 spdk_jsonrpc_client_get_response()로 회수.
 * 이 파일은 jsonrpc_server_tcp.c와 대칭이지만, 1개 연결만 다루고 동시 in-flight도
 * 1건이라 훨씬 단순하다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/string.h(spdk_strerror, spdk_parse_ip_addr), spdk/util.h, jsonrpc_internal.h.
 * 시스템 호출: socket(2), connect(2), poll(2), send(2), recv(2), close(2),
 *              setsockopt SO_ERROR (비동기 connect 결과 확인용),
 *              getaddrinfo(3) (TCP 주소 lookup), inet_*(IP 변환).
 * 호출되는 client.c 함수: jsonrpc_parse_response (응답 파싱).
 * 데이터 흐름: 사용자 spdk_json_write → client.c → request->send_buf → [이 파일] send() →
 *              socket → 서버 → socket → [이 파일] recv() → client->recv_buf →
 *              jsonrpc_parse_response → client->resp → 사용자.
 *
 * === 주요 함수/구조체 요약 ===
 *   - RPC_DEFAULT_PORT "5260": TCP RPC 기본 포트.
 *   - spdk_jsonrpc_client_connect/_close: 연결 lifecycle.
 *   - jsonrpc_client_connect: AF_UNIX/AF_INET 공통 connect 헬퍼 (non-blocking).
 *   - spdk_jsonrpc_client_poll: 메인 polling 진입점 (connected/connecting 분기).
 *   - jsonrpc_client_poll: connected 상태의 poll(2) + send/recv 실행.
 *   - jsonrpc_client_poll_connecting: 비동기 connect 완료 대기.
 *   - jsonrpc_client_send_request: send_buf를 socket으로 부분 송신.
 *   - jsonrpc_client_recv: recv() + recv_buf 확장 + jsonrpc_parse_response.
 *   - recv_buf_expand: recv_buf 2배 확장 (32MB 한도).
 *   - spdk_jsonrpc_client_create_request/_free_request: 요청 buf 메모리 관리.
 *   - spdk_jsonrpc_client_send_request: 요청을 client 송신 슬롯에 등록 (1개만).
 *   - spdk_jsonrpc_client_get_response/_free_response: 응답 회수 + free.
 */

#include "spdk/string.h"           /* [한국어] spdk_strerror, spdk_parse_ip_addr */
#include "jsonrpc_internal.h"      /* [한국어] client 자료구조, jsonrpc_parse_response */
#include "spdk/util.h"             /* [한국어] SPDK_CONTAINEROF (free_response에서 사용) */

/* [한국어] TCP 모드의 기본 포트 — IANA 미등록 사설 포트.
 * AF_UNIX 모드에서는 무시됨. 사용자가 "host" (포트 생략)로 connect하면 이 값 사용. */
#define RPC_DEFAULT_PORT	"5260"

/*
 * [한국어]
 * jsonrpc_client_send_request - 등록된 요청을 socket으로 부분 송신
 *
 * @client: 대상 client.
 * @return: 0 성공/EINTR(재시도), 음수 errno (EAGAIN은 이 위치에서는 발생 안 함 — POLLOUT 후 호출).
 *
 * client->request가 NULL이면 송신할 게 없어 즉시 0 반환.
 * send_len > 0이면 send() 시도, 부분 송신은 send_offset 가산으로 추적.
 * send_len이 0이 되면 요청 송신 완료 — slot 비우고 free.
 *
 * 호출 컨텍스트: jsonrpc_client_poll에서 POLLOUT 이벤트가 발생했을 때만 호출 —
 * 그래서 send()가 EAGAIN 반환은 거의 없음(EINTR만 처리).
 *
 * 주의: 에러 메시지가 "poll() failed"인 것은 historical typo — 실제로는 send 실패.
 */
static int
jsonrpc_client_send_request(struct spdk_jsonrpc_client *client)
{
	ssize_t rc;
	struct spdk_jsonrpc_client_request *request = client->request;

	if (!request) {
		/* [한국어] 송신 슬롯이 비어 있음 — 정상 (no-op) */
		return 0;
	}

	if (request->send_len > 0) {
		rc = send(client->sockfd, request->send_buf + request->send_offset,
			  request->send_len, 0);
		/* [한국어] non-blocking send. flags=0. */
		if (rc < 0) {
			/* For EINTR we pretend that nothing was send. */
			if (errno == EINTR) {
				/* [한국어] 시그널 인터럽트 — 다음 poll에서 재시도. */
				rc = 0;
			} else {
				rc = -errno;
				SPDK_ERRLOG("poll() failed (%d): %s\n", errno, spdk_strerror(errno));
				/* [한국어] EPIPE 등 — 연결 끊김. 호출자가 처리(client_poll에 -errno 반환). */
			}

			return rc;
		}

		request->send_offset += rc;  /* [한국어] cursor 전진 */
		request->send_len -= rc;     /* [한국어] 잔여 byte 감소 */
	}

	if (request->send_len == 0) {
		/* [한국어] 완전 송신 — slot 비움 + 메모리 해제. 다음 send_request 가능. */
		client->request = NULL;
		spdk_jsonrpc_client_free_request(request);
	}

	return 0;
}

/*
 * [한국어]
 * recv_buf_expand - recv_buf 크기를 2배로 확장
 *
 * @client: 대상 client.
 * @return: 0 성공, -ENOSPC(32MB 한도 초과), -ENOMEM(realloc 실패).
 *
 * 큰 응답(예: bdev 리스트, IO 통계 dump)을 받을 수 있도록 동적 grow.
 * 한도는 SEND_BUF_SIZE_MAX(32MB)를 공유 — 송수신 한도 동일.
 */
static int
recv_buf_expand(struct spdk_jsonrpc_client *client)
{
	uint8_t *new_buf;

	if (client->recv_buf_size * 2 > SPDK_JSONRPC_SEND_BUF_SIZE_MAX) {
		/* [한국어] 32MB 초과 시도 — 거부. 비정상적으로 큰 응답 차단. */
		return -ENOSPC;
	}

	new_buf = realloc(client->recv_buf, client->recv_buf_size * 2);
	if (new_buf == NULL) {
		SPDK_ERRLOG("Resizing recv_buf failed (current size %zu, new size %zu)\n",
			    client->recv_buf_size, client->recv_buf_size * 2);
		return -ENOMEM;
	}

	client->recv_buf = new_buf;
	client->recv_buf_size *= 2;  /* [한국어] capacity 2배 — amortized O(1) recv */

	return 0;
}

/*
 * [한국어]
 * jsonrpc_client_resp_ready_count - 회수 가능한 응답이 있으면 1, 없으면 0
 *
 * @client: 대상 client.
 * @return: 0 또는 1.
 *
 * spdk_jsonrpc_client_poll의 반환값으로 사용 — 사용자 코드가 "응답 도착했나?" 판단.
 */
static int
jsonrpc_client_resp_ready_count(struct spdk_jsonrpc_client *client)
{
	return client->resp != NULL && client->resp->ready ? 1 : 0;
	/* [한국어] resp가 alloc되어 있고 파싱까지 완료(ready=true)된 경우만 1. */
}

/*
 * [한국어]
 * jsonrpc_client_recv - socket에서 byte 수신 + 응답 파싱 시도
 *
 * @client: 대상 client.
 * @return: 1=응답 ready, 0=incomplete(더 받아야 함), -EIO=EOF, 음수=에러.
 *
 * 동작:
 *   1) recv_buf가 NULL이면 32KB로 첫 alloc.
 *   2) recv_buf가 거의 다 찼으면(buf_size-1) 2배로 확장 — null 종단 자리 1byte 보존.
 *   3) recv() 호출. EAGAIN은 POLLIN 이후라 거의 없지만 EINTR은 0 반환.
 *   4) recv()==0이면 EOF — 서버 종료. -EIO 반환.
 *   5) 받은 byte를 buf에 누적, NUL 종단 추가(SPDK_DEBUGLOG 안전성).
 *   6) jsonrpc_parse_response(client.c)로 파싱 시도 — 결과를 그대로 반환.
 *
 * 호출 컨텍스트: jsonrpc_client_poll에서 POLLIN 이벤트가 발생했을 때만 호출.
 */
static int
jsonrpc_client_recv(struct spdk_jsonrpc_client *client)
{
	ssize_t rc;

	if (client->recv_buf == NULL) {
		/* [한국어] 첫 recv 또는 직전 응답이 회수돼 buf 소유권을 넘긴 상태 — 새 buf alloc. */
		client->recv_buf = malloc(SPDK_JSONRPC_SEND_BUF_SIZE_INIT);
		if (!client->recv_buf) {
			rc = errno;
			SPDK_ERRLOG("malloc() failed (%d): %s\n", (int)rc, spdk_strerror(rc));
			return -rc;
		}
		client->recv_buf_size = SPDK_JSONRPC_SEND_BUF_SIZE_INIT;  /* [한국어] 32KB */
		client->recv_offset = 0;
	} else if (client->recv_offset == client->recv_buf_size - 1) {
		/* [한국어] capacity-1 만큼 채워짐 (NUL 종단 자리 빼고) — 확장 필요. */
		rc = recv_buf_expand(client);
		if (rc) {
			return rc;
		}
	}

	rc = recv(client->sockfd, client->recv_buf + client->recv_offset,
		  client->recv_buf_size - client->recv_offset - 1, 0);
	/* [한국어] -1: NUL 종단 자리 1byte 항상 예약. */
	if (rc < 0) {
		/* For EINTR we pretend that nothing was received. */
		if (errno == EINTR) {
			/* [한국어] 시그널 인터럽트 — 다음 poll에서 재시도, 0 반환. */
			return 0;
		} else {
			rc = -errno;
			SPDK_ERRLOG("recv() failed (%d): %s\n", errno, spdk_strerror(errno));
			return rc;
		}
	} else if (rc == 0) {
		/* [한국어] EOF — 서버가 close. -EIO로 conn 종료 시그널. */
		return -EIO;
	}

	client->recv_offset += rc;                  /* [한국어] 누적 byte 갱신 */
	client->recv_buf[client->recv_offset] = '\0';  /* [한국어] NUL 종단 — 디버그 로그 출력 시 안전 */

	/* Check to see if we have received a full JSON value. */
	return jsonrpc_parse_response(client);
	/* [한국어] client.c의 파서 호출 — 1=응답 완료, 0=incomplete, 음수=파싱 에러. */
}

/*
 * [한국어]
 * jsonrpc_client_poll - connected 상태의 메인 polling 라운드
 *
 * @client: 대상 client.
 * @timeout: poll(2) timeout (ms). -1=무한, 0=즉시 반환.
 * @return: 1=응답 ready, 0=아직 안 됨, 음수 errno.
 *
 * poll(2)로 1개 fd의 POLLIN/POLLOUT을 동시에 대기. 이벤트가 오면 send 먼저,
 * recv 나중. recv가 -EAGAIN을 반환하는 케이스(IN_PLACE incomplete)는 정상으로 변환.
 * 마지막에 응답 ready 여부를 반환값으로 노출 — 사용자 코드의 dispatch 신호.
 */
static int
jsonrpc_client_poll(struct spdk_jsonrpc_client *client, int timeout)
{
	int rc;
	struct pollfd pfd = { .fd = client->sockfd, .events = POLLIN | POLLOUT };
	/* [한국어] 송수신 동시 모니터링. POLLIN=read 가능, POLLOUT=write 가능. */

	rc = poll(&pfd, 1, timeout);  /* [한국어] poll(2): 1개 fd, timeout ms 만큼 대기 */
	if (rc == -1) {
		if (errno == EINTR) {
			/* For EINTR we pretend that nothing was received nor send. */
			rc = 0;
		} else {
			rc = -errno;
			SPDK_ERRLOG("poll() failed (%d): %s\n", errno, spdk_strerror(errno));
		}
	} else if (rc > 0) {
		/* [한국어] 이벤트 1개 이상 발생 */
		rc = 0;

		if (pfd.revents & POLLOUT) {
			/* [한국어] 송신 가능 — 큐에 요청이 있으면 byte 흘려보냄. */
			rc = jsonrpc_client_send_request(client);
		}

		if (rc == 0 && (pfd.revents & POLLIN)) {
			/* [한국어] send 성공 후에만 recv 시도. read 가능 — 응답 byte 받기 시도. */
			rc = jsonrpc_client_recv(client);
			/* Incomplete message in buffer isn't an error. */
			if (rc == -EAGAIN) {
				/* [한국어] (이 경로는 보통 발생 안 함 — 안전망) */
				rc = 0;
			}
		}
	}
	/* [한국어] rc==0(timeout)이면 그대로 ready 검사로 진행. */

	return rc ? rc : jsonrpc_client_resp_ready_count(client);
	/* [한국어] 에러면 음수 그대로, 그 외엔 0 또는 1(응답 ready 여부). */
}

/*
 * [한국어]
 * jsonrpc_client_poll_connecting - 비동기 connect 완료 대기 (별도 poll 경로)
 *
 * @client: 대상 client (connect EINPROGRESS 상태).
 * @timeout: poll timeout (ms).
 * @return: 0 연결 성공(client->connected=true), -ENOTCONN 아직 진행 중,
 *          -EIO 연결 실패.
 *
 * non-blocking connect는 EINPROGRESS로 즉시 반환하므로, POLLOUT 이벤트로 완료를
 * 감지한 뒤 SO_ERROR로 실제 결과 코드를 확인해야 한다(connect는 fail이라도 POLLOUT이 옴).
 *
 * 호출 체인: spdk_jsonrpc_client_poll → connecting 상태일 때 → [이 함수].
 */
static int
jsonrpc_client_poll_connecting(struct spdk_jsonrpc_client *client, int timeout)
{
	socklen_t rc_len;
	int rc;

	struct pollfd pfd = {
		.fd = client->sockfd,
		.events = POLLOUT  /* [한국어] connect 완료는 write 가능 이벤트로 통보됨 */
	};

	rc = poll(&pfd, 1, timeout);
	if (rc == 0) {
		/* [한국어] timeout — 아직 진행 중. 사용자가 다시 호출해야 함. */
		return -ENOTCONN;
	} else if (rc == -1) {
		if (errno != EINTR) {
			SPDK_ERRLOG("poll() failed (%d): %s\n", errno, spdk_strerror(errno));
			goto err;
		}

		/* We are still not connected. Caller will have to call us again. */
		/* [한국어] EINTR — 시그널 인터럽트, 재시도. */
		return -ENOTCONN;
	} else if (pfd.revents & ~POLLOUT) {
		/* We only poll for POLLOUT */
		/* [한국어] POLLERR/POLLHUP 등 다른 이벤트 — connect 실패. */
		goto err;
	} else if ((pfd.revents & POLLOUT) == 0) {
		/* Is this even possible to get here? */
		/* [한국어] poll>0인데 POLLOUT 미설정 — 이론상 도달 불가, 방어적 처리. */
		return -ENOTCONN;
	}

	rc_len = sizeof(int);
	/* connection might fail so need to check SO_ERROR. */
	if (getsockopt(client->sockfd, SOL_SOCKET, SO_ERROR, &rc, &rc_len) == -1) {
		/* [한국어] SO_ERROR 자체가 실패 — drastic. */
		goto err;
	}

	if (rc == 0) {
		/* [한국어] 실제 connect 결과 코드가 0 — 연결 성공! */
		client->connected = true;
		return 0;
	}
	/* [한국어] rc!=0이면 connect 실패한 errno (예: ECONNREFUSED) — err로 fall through. */

err:
	return -EIO;
}

/*
 * [한국어]
 * jsonrpc_client_connect - AF_UNIX/AF_INET 공통 socket+connect 헬퍼
 *
 * @client: 대상 client.
 * @domain: AF_UNIX 또는 AF_INET/INET6.
 * @protocol: 0(UDS) 또는 IPPROTO_TCP.
 * @server_addr: 서버 주소.
 * @addrlen: 주소 크기.
 * @return: 0 즉시 연결 성공, -EINPROGRESS 비동기 진행 중(정상), 기타 음수=실패.
 *
 * non-blocking connect — UDS/TCP 모두 동일 코드 경로. EINPROGRESS는 정상 — 호출자
 * (spdk_jsonrpc_client_connect)가 -EINPROGRESS를 무시하고 client 핸들 반환,
 * 사용자가 spdk_jsonrpc_client_poll로 완료 대기.
 */
static int
jsonrpc_client_connect(struct spdk_jsonrpc_client *client, int domain, int protocol,
		       struct sockaddr *server_addr, socklen_t addrlen)
{
	int rc;

	client->sockfd = socket(domain, SOCK_STREAM | SOCK_NONBLOCK, protocol);
	/* [한국어] non-blocking socket. CLOEXEC는 안 붙어 있음(historical) — fork 시 fd 누수 가능성.
	 * 클라이언트는 fork 거의 안 해서 실제 영향 미미. */
	if (client->sockfd < 0) {
		rc = errno;
		SPDK_ERRLOG("socket() failed\n");
		return -rc;
	}

	rc = connect(client->sockfd, server_addr, addrlen);
	/* [한국어] non-blocking connect — 보통 -1+EINPROGRESS 반환(localhost UDS는 즉시 0 가능). */
	if (rc != 0) {
		rc = errno;
		if (rc != EINPROGRESS) {
			/* [한국어] 진짜 에러(예 ECONNREFUSED, ENOENT(UDS path 없음)). */
			SPDK_ERRLOG("could not connect to JSON-RPC server: %s\n", spdk_strerror(errno));
			goto err;
		}
		/* [한국어] EINPROGRESS는 정상 — connected=false 유지, 호출자가 poll로 대기. */
	} else {
		/* [한국어] 즉시 연결 성공 (UDS 같은 빠른 경로). */
		client->connected = true;
	}

	return -rc;  /* [한국어] 0 또는 -EINPROGRESS — 호출자가 EINPROGRESS는 정상으로 처리 */
err:
	close(client->sockfd);
	client->sockfd = -1;
	return -rc;
}

/*
 * [한국어]
 * spdk_jsonrpc_client_connect - JSON-RPC 클라이언트 인스턴스 생성 + 서버 연결 (공개 API)
 *
 * @addr: 서버 주소. AF_UNIX면 UDS path("/var/tmp/spdk.sock"), AF_INET이면 "host" 또는 "host:port".
 * @addr_family: AF_UNIX 또는 AF_INET/INET6.
 * @return: client 핸들 (NULL이면 errno 설정).
 *
 * SPDK rpc.py 같은 클라이언트의 표준 진입점. 비동기 connect를 사용하므로 -EINPROGRESS도
 * 정상 결과 — 사용자는 client 핸들을 받고 spdk_jsonrpc_client_poll로 완료 대기.
 *
 * 동작:
 *   AF_UNIX: sockaddr_un 채운 후 jsonrpc_client_connect 호출.
 *   AF_INET: spdk_parse_ip_addr로 host:port 분리 → getaddrinfo로 lookup →
 *            결과 첫 번째 주소로 connect 시도.
 */
struct spdk_jsonrpc_client *
spdk_jsonrpc_client_connect(const char *addr, int addr_family)
{
	struct spdk_jsonrpc_client *client = calloc(1, sizeof(struct spdk_jsonrpc_client));
	/* Unix Domain Socket */
	struct sockaddr_un addr_un = {};      /* [한국어] UDS sockaddr — 미사용 시에도 zero-init */
	char *add_in = NULL;                  /* [한국어] TCP 모드에서 strdup된 사본 (free에서 해제) */
	int rc;

	if (client == NULL) {
		SPDK_ERRLOG("%s\n", spdk_strerror(errno));
		return NULL;  /* [한국어] OOM */
	}

	if (addr_family == AF_UNIX) {
		addr_un.sun_family = AF_UNIX;
		rc = snprintf(addr_un.sun_path, sizeof(addr_un.sun_path), "%s", addr);
		/* [한국어] sun_path는 보통 108byte — UDS path 길이 제한.
		 * snprintf로 안전 복사 + 잘림 검출. */
		if (rc < 0 || (size_t)rc >= sizeof(addr_un.sun_path)) {
			rc = -EINVAL;
			SPDK_ERRLOG("RPC Listen address Unix socket path too long\n");
			goto err;
		}

		rc = jsonrpc_client_connect(client, AF_UNIX, 0, (struct sockaddr *)&addr_un, sizeof(addr_un));
		/* [한국어] UDS 연결 시도 — 보통 즉시 connected=true. */
	} else {
		/* TCP/IP socket */
		struct addrinfo		hints;
		struct addrinfo		*res;
		char *host, *port;

		add_in = strdup(addr);  /* [한국어] addr는 const, 파싱은 in-place라 사본 필요 */
		if (!add_in) {
			rc = -errno;
			SPDK_ERRLOG("%s\n", spdk_strerror(errno));
			goto err;
		}

		rc = spdk_parse_ip_addr(add_in, &host, &port);
		/* [한국어] "host:port" 또는 "[ipv6]:port" 형식을 host/port로 분리 (in-place). */
		if (rc) {
			SPDK_ERRLOG("Invalid listen address '%s'\n", addr);
			goto err;
		}

		if (port == NULL) {
			port = RPC_DEFAULT_PORT;  /* [한국어] 포트 생략 시 5260 */
		}

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;       /* [한국어] IPv4/IPv6 모두 허용 */
		hints.ai_socktype = SOCK_STREAM;   /* [한국어] TCP */
		hints.ai_protocol = IPPROTO_TCP;

		rc = getaddrinfo(host, port, &hints, &res);
		/* [한국어] DNS/getservbyname 통합 lookup — host="localhost", port="5260" 등도 처리. */
		if (rc != 0) {
			SPDK_ERRLOG("Unable to look up RPC connect address '%s' (%d): %s\n", addr, rc, gai_strerror(rc));
			rc = -(abs(rc));  /* [한국어] getaddrinfo의 양수 EAI_* 코드를 음수 errno-like로 변환 */
			goto err;
		}

		rc = jsonrpc_client_connect(client, res->ai_family, res->ai_protocol, res->ai_addr,
					    res->ai_addrlen);
		/* [한국어] 첫 번째 주소로만 시도 — 다중 A 레코드 fallback은 안 함. */
		freeaddrinfo(res);
	}

err:
	if (rc != 0 && rc != -EINPROGRESS) {
		/* [한국어] 진짜 실패만 cleanup. EINPROGRESS는 정상 — 호출자에게 client 반환. */
		free(client);
		client = NULL;
		errno = -rc;  /* [한국어] errno set — 사용자가 NULL 반환 시 errno 확인 가능 */
	}

	free(add_in);  /* [한국어] strdup 사본 해제(NULL이면 free 안전) */
	return client;
}

/*
 * [한국어]
 * spdk_jsonrpc_client_close - 클라이언트 해제 (공개 API)
 *
 * @client: 종료할 client.
 *
 * socket close + 누적 buf 해제 + 미회수 응답이 있으면 free + client 자체 free.
 * close_cb 같은 비동기 cleanup 메커니즘은 없음 — 단순 동기 free.
 */
void
spdk_jsonrpc_client_close(struct spdk_jsonrpc_client *client)
{
	if (client->sockfd >= 0) {
		close(client->sockfd);  /* [한국어] socket fd 해제 — 서버에 EOF 전달 */
	}

	free(client->recv_buf);  /* [한국어] 미파싱 잔여 buf 해제 (NULL 안전) */
	if (client->resp) {
		/* [한국어] 사용자가 get_response 안 한 경우 — 강제 free. */
		spdk_jsonrpc_client_free_response(&client->resp->jsonrpc);
	}

	free(client);
}

/*
 * [한국어]
 * spdk_jsonrpc_client_create_request - 빈 요청 객체 alloc (공개 API)
 *
 * @return: 빈 요청 (send_buf 32KB 할당된 상태). NULL=OOM.
 *
 * 사용자가 spdk_jsonrpc_begin_request로 채울 컨테이너 제공.
 * send_buf는 spdk_json_write_ctx의 sink.
 */
struct spdk_jsonrpc_client_request *
spdk_jsonrpc_client_create_request(void)
{
	struct spdk_jsonrpc_client_request *request;

	request = calloc(1, sizeof(*request));  /* [한국어] zero-init: send_offset/send_len=0 */
	if (request == NULL) {
		return NULL;
	}

	/* memory malloc for send-buf */
	request->send_buf = malloc(SPDK_JSONRPC_SEND_BUF_SIZE_INIT);
	/* [한국어] 32KB 초기 capacity — write_cb가 부족 시 2배씩 확장. */
	if (!request->send_buf) {
		SPDK_ERRLOG("memory malloc for send-buf failed\n");
		free(request);
		return NULL;
	}
	request->send_buf_size = SPDK_JSONRPC_SEND_BUF_SIZE_INIT;

	return request;
}

/*
 * [한국어]
 * spdk_jsonrpc_client_free_request - 요청 buf와 자체 해제 (공개 API)
 *
 * @req: 해제 대상.
 *
 * 송신 완료 후 자동으로 호출되거나, 송신 전에 사용자가 폐기할 때 호출.
 */
void
spdk_jsonrpc_client_free_request(struct spdk_jsonrpc_client_request *req)
{
	free(req->send_buf);  /* [한국어] send_buf는 항상 alloc되어 있음 (create_request가 보장) */
	free(req);
}

/*
 * [한국어]
 * spdk_jsonrpc_client_poll - 클라이언트 polling 진입점 (공개 API)
 *
 * @client: 대상 client.
 * @timeout: poll(2) timeout (ms).
 * @return: 1=응답 ready, 0=대기 중, 음수 errno.
 *
 * connected/connecting 상태에 따라 분기 — 비동기 connect 중이면 connect 완료 대기,
 * 연결됐으면 일반 send/recv polling.
 *
 * 호출 체인: 사용자 loop → [이 함수] → jsonrpc_client_poll/_poll_connecting →
 * 응답 ready 시 사용자가 spdk_jsonrpc_client_get_response로 회수.
 */
int
spdk_jsonrpc_client_poll(struct spdk_jsonrpc_client *client, int timeout)
{
	if (client->connected) {
		return jsonrpc_client_poll(client, timeout);
	} else {
		return jsonrpc_client_poll_connecting(client, timeout);
	}
}

/*
 * [한국어]
 * spdk_jsonrpc_client_send_request - 요청을 client 송신 슬롯에 등록 (공개 API)
 *
 * @client: 대상 client.
 * @req: 보낼 요청.
 * @return: 0 성공, -ENOSPC(이전 요청이 아직 송신 중).
 *
 * client는 동시 1건의 in-flight 요청만 허용 — 한 번에 하나만 등록 가능.
 * 실제 송신은 jsonrpc_client_poll의 POLLOUT 이벤트 시점에 발생.
 */
int
spdk_jsonrpc_client_send_request(struct spdk_jsonrpc_client *client,
				 struct spdk_jsonrpc_client_request *req)
{
	if (client->request != NULL) {
		/* [한국어] 송신 슬롯 점유 — 사용자는 이전 요청의 응답을 받기 전까지 대기. */
		return -ENOSPC;
	}

	client->request = req;  /* [한국어] 슬롯 등록 — 이후 poll의 POLLOUT 시 send. */
	return 0;
}

/*
 * [한국어]
 * spdk_jsonrpc_client_get_response - 파싱 완료된 응답 회수 (공개 API)
 *
 * @client: 대상 client.
 * @return: 응답 포인터 (NULL이면 아직 ready 아님).
 *
 * 회수 후 client->resp는 NULL — 다음 응답을 받을 수 있음.
 * 반환된 응답은 사용자가 spdk_jsonrpc_client_free_response로 해제 책임.
 */
struct spdk_jsonrpc_client_response *
spdk_jsonrpc_client_get_response(struct spdk_jsonrpc_client *client)
{
	struct spdk_jsonrpc_client_response_internal *r;

	r = client->resp;
	if (r == NULL || r->ready == false) {
		/* [한국어] 슬롯이 비었거나 파싱 미완료 — NULL 반환. */
		return NULL;
	}

	client->resp = NULL;  /* [한국어] 슬롯 비움 — 다음 응답 수신 가능 */
	return &r->jsonrpc;   /* [한국어] internal의 첫 필드 — 외부 노출 형식 */
}

/*
 * [한국어]
 * spdk_jsonrpc_client_free_response - 응답 객체와 buf 해제 (공개 API)
 *
 * @resp: get_response가 반환한 응답.
 *
 * SPDK_CONTAINEROF로 외부 노출 jsonrpc 필드에서 internal 객체 복원 후
 * 한 번에 r->buf(raw JSON), r 자체 해제. values 토큰 배열은 r 내부 flexible array라
 * 별도 free 불필요.
 */
void
spdk_jsonrpc_client_free_response(struct spdk_jsonrpc_client_response *resp)
{
	struct spdk_jsonrpc_client_response_internal *r;

	if (!resp) {
		return;  /* [한국어] NULL 입력 방어 */
	}

	r = SPDK_CONTAINEROF(resp, struct spdk_jsonrpc_client_response_internal, jsonrpc);
	/* [한국어] CONTAINEROF: 멤버 포인터에서 컨테이너 시작 주소 계산 — Linux container_of 패턴. */
	free(r->buf);  /* [한국어] raw JSON 텍스트 buf 해제 (string 토큰들이 가리키던 메모리) */
	free(r);       /* [한국어] internal 객체 자체 해제 (values flexible array 포함) */
}
