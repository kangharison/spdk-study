/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI Portal/Portal Group 관리 구현 (portal_grp.c)
 *
 * === 파일의 역할 ===
 * 본 파일은 SPDK iSCSI 타깃의 "수신 측 네트워크 입구"인 Portal과 Portal Group(PG)에
 * 대한 모든 운용 로직을 구현한다. 구체적으로 (1) 포털 객체 생성·등록·소멸, (2) PG
 * 컨테이너 생성·등록·소멸, (3) listen 소켓 오픈/클로즈, (4) ACCEPT_TIMEOUT_US(=1ms)
 * 주기로 spdk_sock_group을 폴링하여 새 TCP 연결을 수락하는 acceptor poller, (5) JSON
 * RPC 직렬화, (6) 로그인 redirect용 sockaddr 파싱을 담당한다. 이 파일이 만든 acceptor가
 * 새 TCP 연결을 받을 때마다 iscsi_conn_construct()를 호출해 spdk_iscsi_conn을 생성하므로
 * 모든 iSCSI 세션의 "최초 진입점"에 해당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 제어 평면 호출 체인:
 *   RPC iscsi_create_portal_group → iscsi_portal_grp_create() → iscsi_portal_grp_register()
 *   RPC iscsi_create_portal_group_portal_addr → iscsi_portal_create() →
 *     iscsi_portal_grp_add_portal() → iscsi_portal_grp_open()
 *       → spdk_sock_group_create + SPDK_POLLER_REGISTER(iscsi_portal_group_poll, 1ms)
 *       → 각 포털: iscsi_portal_open() → spdk_sock_listen + spdk_sock_group_add_sock
 *         (callback=iscsi_portal_accept).
 * 데이터 평면 (런타임):
 *   SPDK reactor 폴링 루프 → acceptor_poller(1ms) → iscsi_portal_group_poll →
 *   spdk_sock_group_poll → (readable이면) iscsi_portal_accept → spdk_sock_accept →
 *   iscsi_conn_construct() → 새 iSCSI conn 생성 → 로그인 phase 진입.
 * 셧다운: iscsi_portal_grp_close_all() → 각 포털 close → group_poller unregister → free.
 * 실행 컨텍스트: PG의 sock_group/acceptor_poller는 단일 SPDK thread(보통 메인 reactor)에
 * 바인딩되어 있어 lockless로 동작한다. 전역 리스트(portal_head/pg_head) 변경만 mutex 보호.
 *
 * === 타 모듈과의 연결 ===
 * - iscsi.h: g_iscsi 전역, MAX_PORTAL_*, CHAP 기본값.
 * - conn.h: iscsi_conn_construct(portal, sock) — accept된 소켓을 conn으로 승격.
 * - sock.h (SPDK): spdk_sock_listen/accept/group/group_poll, request callback.
 * - tgt_node.h: 본 파일에서 등록된 PG는 tgt_node의 (PG, IG) 매핑을 통해 사용된다.
 * - JSON RPC: spdk_json_write_ctx로 PG/포털 정보 직렬화.
 * 데이터 흐름: RPC 입력 → portal/PG 생성 → listen 소켓 오픈 → reactor 폴링 → accept →
 *   conn 생성 → 로그인 → tgt_node attach.
 *
 * === 주요 함수/구조체 요약 ===
 * - iscsi_portal_accept(): sock_group 콜백, 새 연결을 conn으로 변환.
 * - iscsi_portal_group_poll(): SPDK poller 콜백, sock_group_poll 한 번 호출.
 * - iscsi_portal_create()/destroy(): 단일 포털 생성·해제 (전역 portal_head 등록).
 * - iscsi_portal_open()/close(): listen 시작/종료, sock_group 등록/해제.
 * - iscsi_portal_grp_create()/destroy(): PG 생성·해제.
 * - iscsi_portal_grp_open(): sock_group/acceptor 등록 후 모든 포털 listen.
 * - iscsi_portal_grp_register/unregister/find_by_tag(): 전역 pg_head 관리.
 * - iscsi_parse_redirect_addr(): redirect용 numeric getaddrinfo.
 * - iscsi_portal_grp(s)_info_json/config_json(): RPC 직렬화.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 라이브러리 일괄 포함. errno, calloc 등. */

#include "spdk/sock.h"
/* [한국어] SPDK 소켓 추상화 — spdk_sock_listen/accept/group_*. POSIX/uring/VPP backend 공통 API. */
#include "spdk/string.h"
/* [한국어] spdk_strerror() 등. */

#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG/WARNLOG/DEBUGLOG 매크로. */

#include "iscsi/iscsi.h"
/* [한국어] g_iscsi 전역, 도메인 상수. */
#include "iscsi/conn.h"
/* [한국어] iscsi_conn_construct — accept된 소켓을 conn 객체로 승격. */
#include "iscsi/portal_grp.h"
/* [한국어] 본 파일에서 구현하는 외부 API의 선언. */
#include "iscsi/tgt_node.h"
/* [한국어] (전방 의존) PG와 tgt_node 매핑 정보. 일부 빌드에서 forward 의존성 해결용. */

#define PORTNUMSTRLEN 32
/* [한국어] 포트 번호를 문자열로 변환 시 충분한 버퍼 크기 (32바이트 — IPv6 brackets 등 마진 포함). */
#define ACCEPT_TIMEOUT_US 1000 /* 1ms */
/* [한국어] acceptor_poller의 폴링 주기(마이크로초). 1ms = 1000us — 새 연결 수락 latency 목표.
 * 너무 작으면 CPU 점유 증가, 너무 크면 connect 응답 지연. 1ms는 SPDK 표준 절충값. */

/*
 * [한국어]
 * iscsi_portal_accept - sock_group이 readable 이벤트를 알릴 때 호출되는 callback.
 *
 * @arg: 콜백 등록 시 전달된 spdk_iscsi_portal* (이 listen 소켓의 부모 포털).
 * @group: 이벤트가 발생한 sock_group (PG의 sock_group).
 * @listen_sock: 수락 가능한 listen 소켓.
 *
 * 동작: while 루프에서 spdk_sock_accept를 반복 호출 — EAGAIN/EWOULDBLOCK이 나오면
 * 더 이상 대기 중인 연결이 없으므로 종료. 매 accept 성공마다 iscsi_conn_construct로
 * 새 spdk_iscsi_conn 생성 (실패 시 sock close 후 즉시 break하여 다음 iteration에 다시 시도).
 * 실행 컨텍스트: 등록된 sock_group의 polling thread (메인 reactor).
 *
 * 호출 체인:
 *   iscsi_portal_group_poll → spdk_sock_group_poll → [iscsi_portal_accept] →
 *     spdk_sock_accept → iscsi_conn_construct
 */
static void
iscsi_portal_accept(void *arg, struct spdk_sock_group *group, struct spdk_sock *listen_sock)
{
	struct spdk_iscsi_portal	*portal = arg;
	/* [한국어] 콜백 인자에서 부모 포털 복원. iscsi_portal_open에서 등록 시 portal 자신을 전달. */
	struct spdk_sock		*sock;
	/* [한국어] accept 결과 새 연결 소켓 (NULL이면 더 이상 대기 없음). */
	int				rc;
	/* [한국어] iscsi_conn_construct 반환 코드. */

	while (1) {
		/* [한국어] 한 번의 polling 이벤트에서 가능한 모든 pending 연결을 처리(burst accept). */
		sock = spdk_sock_accept(listen_sock);
		/* [한국어] 비차단 accept — 대기 중 없으면 NULL+EAGAIN. */
		if (sock != NULL) {
			rc = iscsi_conn_construct(portal, sock);
			/* [한국어] 새 소켓을 spdk_iscsi_conn으로 승격. 내부에서 로그인 PDU 수신 준비까지 완료. */
			if (rc < 0) {
				/* [한국어] conn 생성 실패: 소켓 닫고 루프 종료 (다음 polling tick에 재시도). */
				spdk_sock_close(&sock);
				SPDK_ERRLOG("spdk_iscsi_connection_construct() failed\n");
				break;
			}
		} else {
			/* [한국어] accept 실패. EAGAIN/EWOULDBLOCK은 정상 — 단지 더 대기 없음. */
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				/* [한국어] 그 외 errno는 진짜 오류 → 로그. */
				SPDK_ERRLOG("accept error(%d): %s\n", errno, spdk_strerror(errno));
			}
			break;
			/* [한국어] 어떤 경우든 NULL이면 루프 종료. */
		}
	}
}

/*
 * [한국어]
 * iscsi_portal_group_poll - PG의 acceptor_poller로 등록된 SPDK poller 콜백.
 *
 * @arg: 등록 시 전달된 spdk_iscsi_portal_grp* 포인터.
 * @return: 처리한 이벤트 수 (SPDK_POLLER_BUSY/IDLE 결정에 사용).
 *
 * spdk_sock_group_poll 한 번 호출. group 내에 readable 이벤트가 있으면 등록된 콜백
 * (여기서는 iscsi_portal_accept)을 자동 호출한다.
 * 실행 컨텍스트: SPDK_POLLER_REGISTER로 등록된 SPDK thread의 polling 루프.
 *
 * 호출 체인:
 *   reactor 폴링 → poller 콜백 → [iscsi_portal_group_poll] → spdk_sock_group_poll →
 *     iscsi_portal_accept (콜백)
 */
static int
iscsi_portal_group_poll(void *arg)
{
	struct spdk_iscsi_portal_grp *group = arg;
	/* [한국어] 콜백 인자에서 PG 복원. */

	return spdk_sock_group_poll(group->sock_group);
	/* [한국어] sock_group의 모든 listen 소켓을 한 번 폴링. backend(epoll/uring)에 따라
	 * 사용 자원·동작이 다르나 SPDK API로 추상화. */
}

/*
 * [한국어]
 * iscsi_portal_find_by_addr - 전역 portal_head에서 (host, port)로 포털 검색.
 *
 * @host: 호스트 문자열.
 * @port: 포트 문자열.
 * @return: 일치 포털 또는 NULL.
 *
 * 중복 등록 검사용. 호출자는 g_iscsi.mutex 보유 상태에서 호출해야 한다.
 *
 * 호출 체인:
 *   iscsi_portal_create → [iscsi_portal_find_by_addr] → strcmp
 */
static struct spdk_iscsi_portal *
iscsi_portal_find_by_addr(const char *host, const char *port)
{
	struct spdk_iscsi_portal *p;
	/* [한국어] 순회용 변수. */

	TAILQ_FOREACH(p, &g_iscsi.portal_head, g_tailq) {
		/* [한국어] 전역 portal_head를 순회. g_tailq 링크 사용. */
		if (!strcmp(p->host, host) && !strcmp(p->port, port)) {
			/* [한국어] (host, port) 정확 매치 — 중복으로 판정. */
			return p;
		}
	}

	return NULL;
	/* [한국어] 미발견. */
}

/* Assumes caller allocated host and port strings on the heap */
/*
 * [한국어]
 * iscsi_portal_create - 단일 포털 객체를 생성하고 전역 portal_head에 등록.
 *
 * @host: 호스트 문자열 (caller heap, 본 함수 내에서 복사하므로 caller free 가능).
 * @port: 포트 문자열.
 * @return: 새 포털 포인터, 실패 시 NULL.
 *
 * 동작: 길이 검증 → calloc → "[*]"/"*" 와일드카드 자동 정규화 → 문자열 복사 → 전역
 * 중복 검사(락 보유) → INSERT_TAIL. 등록 성공 후에는 PG에 add_portal로 부착해야 함.
 *
 * 호출 체인:
 *   RPC iscsi_create_portal_group_portal_addr → [iscsi_portal_create]
 */
struct spdk_iscsi_portal *
iscsi_portal_create(const char *host, const char *port)
{
	struct spdk_iscsi_portal *p = NULL, *tmp;
	/* [한국어] p: 새로 만들 포털, tmp: 중복 검사 결과. */

	assert(host != NULL);
	/* [한국어] caller invariant 검증. */
	assert(port != NULL);
	/* [한국어] 동일. */

	if (strlen(host) > MAX_PORTAL_ADDR || strlen(port) > MAX_PORTAL_PORT) {
		/* [한국어] 길이 상한 검사 — 버퍼 오버런 방지. */
		return NULL;
	}

	p = calloc(1, sizeof(*p));
	/* [한국어] 0-초기화 할당. sock=NULL, group=NULL 초기 상태. */
	if (!p) {
		SPDK_ERRLOG("calloc() failed for portal\n");
		return NULL;
	}

	/* check and overwrite abbreviation of wildcard */
	if (strcasecmp(host, "[*]") == 0) {
		/* [한국어] 레거시 IPv6 와일드카드 표기 "[*]" — 표준 "[::]"으로 변환. */
		SPDK_WARNLOG("Please use \"[::]\" as IPv6 wildcard\n");
		SPDK_WARNLOG("Convert \"[*]\" to \"[::]\" automatically\n");
		SPDK_WARNLOG("(Use of \"[*]\" will be deprecated in a future release)");
		snprintf(p->host, sizeof(p->host), "[::]");
		/* [한국어] 표준 IPv6 wildcard. */
	} else if (strcasecmp(host, "*") == 0) {
		/* [한국어] 레거시 IPv4 와일드카드 — "0.0.0.0"으로 변환. */
		SPDK_WARNLOG("Please use \"0.0.0.0\" as IPv4 wildcard\n");
		SPDK_WARNLOG("Convert \"*\" to \"0.0.0.0\" automatically\n");
		SPDK_WARNLOG("(Use of \"[*]\" will be deprecated in a future release)");
		snprintf(p->host, sizeof(p->host), "0.0.0.0");
		/* [한국어] 표준 IPv4 wildcard. */
	} else {
		/* [한국어] 일반 host 문자열은 그대로 복사. */
		memcpy(p->host, host, strlen(host));
		/* [한국어] calloc에 의해 NUL 미리 0으로 채워져 있어 별도 NUL 처리 불필요. */
	}

	memcpy(p->port, port, strlen(port));
	/* [한국어] 포트 문자열 복사. */

	p->sock = NULL;
	/* [한국어] 아직 listen 전 상태. iscsi_portal_open에서 채움. */
	p->group = NULL; /* set at a later time by caller */
	/* [한국어] PG에 부착될 때 add_portal에서 채움. */

	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] 전역 portal_head 보호 락 획득. */
	tmp = iscsi_portal_find_by_addr(host, port);
	/* [한국어] (host, port) 중복 검사. 정규화 전 원본 입력으로 검색하는 것에 주의. */
	if (tmp != NULL) {
		/* [한국어] 이미 존재 — 락 풀고 free 후 NULL 반환. */
		pthread_mutex_unlock(&g_iscsi.mutex);
		SPDK_ERRLOG("portal (%s, %s) already exists\n", host, port);
		goto error_out;
	}

	TAILQ_INSERT_TAIL(&g_iscsi.portal_head, p, g_tailq);
	/* [한국어] 전역 리스트에 등록. 이후 다른 thread에서 보일 수 있음. */
	pthread_mutex_unlock(&g_iscsi.mutex);
	/* [한국어] 락 해제. */

	return p;
	/* [한국어] 등록된 포털 반환. */

error_out:
	free(p);
	/* [한국어] 락 해제 후 free (호출자 노출 전이므로 안전). */

	return NULL;
}

/*
 * [한국어]
 * iscsi_portal_destroy - 포털을 전역 portal_head에서 분리 후 free.
 *
 * @p: 대상 포털 (NULL 불가).
 *
 * 호출 시점: PG에서 이미 떨어져 있고 listen 소켓도 close된 상태여야 한다 (iscsi_portal_close
 * 선행). 이 함수는 전역 리스트 분리 + free만 수행.
 */
void
iscsi_portal_destroy(struct spdk_iscsi_portal *p)
{
	assert(p != NULL);
	/* [한국어] 인자 사전 조건. */

	SPDK_DEBUGLOG(iscsi, "iscsi_portal_destroy\n");
	/* [한국어] 진단 로그. */

	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] 전역 락. */
	TAILQ_REMOVE(&g_iscsi.portal_head, p, g_tailq);
	/* [한국어] portal_head에서 분리. */
	pthread_mutex_unlock(&g_iscsi.mutex);
	/* [한국어] 해제. */

	free(p);
	/* [한국어] 메모리 반환. */

}

/*
 * [한국어]
 * iscsi_portal_open - 포털의 listen 소켓을 열고 PG의 sock_group에 등록.
 *
 * @p: 대상 포털 (이미 PG에 add_portal된 상태여야 함).
 * @return: 0 성공, -1 실패.
 *
 * 동작: 이미 열려있는지 검사 → 포트 문자열 정수 변환·검증 → spdk_sock_listen →
 *   sock_group_add_sock(callback=iscsi_portal_accept). add 실패 시 listen 소켓 close.
 * 실행 컨텍스트: PG open 경로 (RPC thread 또는 메인 reactor).
 */
static int
iscsi_portal_open(struct spdk_iscsi_portal *p)
{
	struct spdk_sock *sock;
	/* [한국어] listen 결과 소켓. */
	int port;
	/* [한국어] 정수 변환된 포트 번호. */
	int rc;
	/* [한국어] add_sock 결과. */

	if (p->sock != NULL) {
		/* [한국어] 중복 오픈 방지 — 이미 열려있으면 에러. */
		SPDK_ERRLOG("portal (%s, %s) is already opened\n",
			    p->host, p->port);
		return -1;
	}

	port = (int)strtol(p->port, NULL, 0);
	/* [한국어] 포트 문자열을 자동 base(0=10진/16진/8진 자동) 정수로 변환. */
	if (port <= 0 || port > 65535) {
		/* [한국어] TCP 포트 유효 범위 검증. */
		SPDK_ERRLOG("invalid port %s\n", p->port);
		return -1;
	}

	sock = spdk_sock_listen(p->host, port, NULL);
	/* [한국어] SPDK 소켓 추상화로 listen. backend(epoll/uring)에 따라 내부 구현 차이. */
	if (sock == NULL) {
		/* [한국어] listen 실패 (주소 충돌, 권한 등). */
		SPDK_ERRLOG("listen error %.64s.%d\n", p->host, port);
		return -1;
	}

	rc = spdk_sock_group_add_sock(p->group->sock_group, sock, iscsi_portal_accept, p);
	/* [한국어] PG의 sock_group에 등록. 콜백 인자로 portal 자신을 전달 — accept 콜백에서
	 * 이 포털 식별 가능. */
	if (rc < 0) {
		/* [한국어] add 실패: 소켓 close 후 -1 반환. */
		SPDK_ERRLOG("spdk_sock_group_add_sock() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		spdk_sock_close(&sock);
		return -1;
	}

	p->sock = sock;
	/* [한국어] 성공 시 sock 보관 — close 시점에 사용. */

	return 0;
}

/*
 * [한국어]
 * iscsi_portal_close - 포털의 listen 소켓을 sock_group에서 빼고 close.
 *
 * @p: 대상 포털.
 *
 * sock==NULL이면 no-op (이미 닫힘 또는 미오픈). 실행 컨텍스트는 SPDK thread.
 */
static void
iscsi_portal_close(struct spdk_iscsi_portal *p)
{
	int rc;
	/* [한국어] remove_sock 반환 코드. */

	if (p->sock) {
		/* [한국어] 열린 상태일 때만 닫기. */
		SPDK_DEBUGLOG(iscsi, "close portal (%s, %s)\n",
			      p->host, p->port);

		rc = spdk_sock_group_remove_sock(p->group->sock_group, p->sock);
		/* [한국어] sock_group에서 등록 해제. accept 콜백 더 이상 호출되지 않음. */
		if (rc < 0) {
			/* [한국어] remove 실패라도 close는 진행 — 진단 로그만. */
			SPDK_ERRLOG("spdk_sock_group_remove_sock() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		}

		spdk_sock_close(&p->sock);
		/* [한국어] 소켓 close 및 p->sock=NULL 설정 (spdk_sock_close가 포인터 인자로 받아 NULL로 만듦). */
	}
}

/*
 * [한국어]
 * iscsi_parse_redirect_addr - 로그인 redirect 응답용 sockaddr 파싱.
 *
 * @sa: 출력 sockaddr_storage 버퍼.
 * @host: 숫자 IPv4/IPv6 문자열.
 * @port: 숫자 포트 문자열.
 * @return: 0 성공, -EINVAL 또는 -getaddrinfo errno.
 *
 * AI_NUMERICHOST|AI_NUMERICSERV 플래그로 DNS 조회를 회피하여 빠르고 결정적인 변환을
 * 보장한다. 반환된 sockaddr은 iSCSI Login Response의 TargetAddress key로 사용된다
 * (RFC 3720 §5.3.3 Redirect login).
 *
 * 호출 체인:
 *   tgt_node redirect 처리 → [iscsi_parse_redirect_addr] → getaddrinfo
 */
int
iscsi_parse_redirect_addr(struct sockaddr_storage *sa,
			  const char *host, const char *port)
{
	struct addrinfo hints, *res;
	/* [한국어] getaddrinfo 입력 hint 및 결과 리스트. */
	int rc;
	/* [한국어] 반환 코드. */

	if (host == NULL || port == NULL) {
		/* [한국어] 인자 검증. */
		return -EINVAL;
	}

	memset(&hints, 0, sizeof(hints));
	/* [한국어] hint 0-초기화. */
	hints.ai_family = PF_UNSPEC;
	/* [한국어] IPv4/IPv6 모두 허용. */
	hints.ai_socktype = SOCK_STREAM;
	/* [한국어] TCP. */
	hints.ai_flags = AI_NUMERICSERV;
	/* [한국어] port가 서비스 이름이 아닌 숫자라고 가정 — getservbyname 우회. */
	hints.ai_flags |= AI_NUMERICHOST;
	/* [한국어] host가 IP 리터럴이라고 가정 — DNS 조회 우회. */
	rc = getaddrinfo(host, port, &hints, &res);
	/* [한국어] glibc getaddrinfo. AI_NUMERIC* 덕분에 동기 차단 없이 빠르게 반환. */
	if (rc != 0) {
		/* [한국어] gai 에러 — 부호 음수로 일관 변환 후 반환. */
		SPDK_ERRLOG("getaddinrfo failed: %s (%d)\n", gai_strerror(rc), rc);
		return -(abs(rc));
	}

	if (res->ai_addrlen > sizeof(*sa)) {
		/* [한국어] sockaddr_storage 크기 초과는 정상 케이스 아님(IPv6도 들어가도록 큰 union). */
		SPDK_ERRLOG("getaddrinfo() ai_addrlen %zu too large\n",
			    (size_t)res->ai_addrlen);
		rc = -EINVAL;
	} else {
		/* [한국어] 결과 sockaddr를 출력 버퍼로 복사. */
		memcpy(sa, res->ai_addr, res->ai_addrlen);
	}

	freeaddrinfo(res);
	/* [한국어] glibc가 할당한 결과 리스트 free. */
	return rc;
}

/*
 * [한국어]
 * iscsi_portal_grp_create - 빈 PG 객체를 만들고 CHAP 기본값을 g_iscsi에서 복사.
 *
 * @tag: PG 식별자.
 * @is_private: Discovery 응답 노출 여부.
 * @return: 새 PG 또는 NULL.
 *
 * 등록(g_iscsi.pg_head 삽입)은 별도 단계(register)에서 수행. 본 함수는 메모리 할당과
 * 초기화만 담당.
 */
struct spdk_iscsi_portal_grp *
iscsi_portal_grp_create(int tag, bool is_private)
{
	struct spdk_iscsi_portal_grp *pg = malloc(sizeof(*pg));
	/* [한국어] PG 객체 할당 (calloc 아닌 malloc — 아래에서 모든 필드를 명시 초기화). */

	if (!pg) {
		SPDK_ERRLOG("malloc() failed for portal group\n");
		return NULL;
	}

	pg->ref = 0;
	/* [한국어] 신규 PG는 참조자 없음. */
	pg->tag = tag;
	/* [한국어] 식별자. */
	pg->is_private = is_private;
	/* [한국어] private 여부. */

	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] CHAP 기본값을 일관되게 읽기 위해 락 획득. */
	pg->disable_chap = g_iscsi.disable_chap;
	pg->require_chap = g_iscsi.require_chap;
	pg->mutual_chap = g_iscsi.mutual_chap;
	pg->chap_group = g_iscsi.chap_group;
	/* [한국어] 글로벌 CHAP 정책을 PG의 초기값으로 상속. 이후 RPC로 PG별 변경 가능. */
	pthread_mutex_unlock(&g_iscsi.mutex);
	/* [한국어] 락 해제. */

	TAILQ_INIT(&pg->head);
	/* [한국어] 포털 리스트 헤드 초기화. */

	return pg;
	/* [한국어] 미등록 PG 객체 반환. */
}

/*
 * [한국어]
 * iscsi_portal_grp_destroy - PG에 속한 포털을 모두 destroy 후 PG 자체 free.
 *
 * @pg: 대상 PG. close는 호출자가 선행 책임.
 *
 * 동작: PG의 head 리스트가 빌 때까지 첫 포털을 분리·destroy 반복 → free.
 * iscsi_portal_destroy가 g_iscsi.mutex를 내부에서 잡으므로 본 함수는 락 보유 상태로
 * 호출되면 안 됨 (deadlock 위험).
 */
void
iscsi_portal_grp_destroy(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal	*p;
	/* [한국어] 분리·destroy 대상 포털 임시 변수. */

	assert(pg != NULL);
	/* [한국어] invariant. */

	SPDK_DEBUGLOG(iscsi, "iscsi_portal_grp_destroy\n");
	/* [한국어] 진단 로그. */
	while (!TAILQ_EMPTY(&pg->head)) {
		/* [한국어] 모든 포털을 한 개씩 처리. */
		p = TAILQ_FIRST(&pg->head);
		/* [한국어] 첫 포털 가져옴. */
		TAILQ_REMOVE(&pg->head, p, per_pg_tailq);
		/* [한국어] PG에서 분리. */
		iscsi_portal_destroy(p);
		/* [한국어] 전역 portal_head에서 분리 + free. */
	}
	free(pg);
	/* [한국어] PG 자체 free. */
}

/*
 * [한국어]
 * iscsi_portal_grp_register - PG를 g_iscsi.pg_head에 등록 (tag 중복 검사 포함).
 *
 * @pg: 등록할 PG.
 * @return: 0 성공, -1 (tag 중복).
 *
 * find_by_tag는 g_iscsi.mutex 가정이지만 본 함수가 락 내부에서 호출. iscsi_init_grp_register와
 * 동일 패턴.
 */
int
iscsi_portal_grp_register(struct spdk_iscsi_portal_grp *pg)
{
	int rc = -1;
	/* [한국어] 기본 실패값. */
	struct spdk_iscsi_portal_grp *tmp;
	/* [한국어] 중복 검사 결과. */

	assert(pg != NULL);
	/* [한국어] invariant. */

	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] 락 획득. */
	tmp = iscsi_portal_grp_find_by_tag(pg->tag);
	/* [한국어] 동일 tag 존재 여부. */
	if (tmp == NULL) {
		/* [한국어] 미존재 — 안전하게 삽입. */
		TAILQ_INSERT_TAIL(&g_iscsi.pg_head, pg, tailq);
		rc = 0;
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	/* [한국어] 해제. */
	return rc;
	/* [한국어] 0 또는 -1. */
}

/*
 * [한국어]
 * iscsi_portal_grp_add_portal - 포털을 PG의 head 리스트에 부착(p->group=pg 설정).
 *
 * @pg: 컨테이너 PG.
 * @p: 부착할 포털 (이미 iscsi_portal_create로 전역 등록된 상태).
 *
 * lock-free — caller가 단일 RPC thread에서 순차적으로 호출한다고 가정.
 */
void
iscsi_portal_grp_add_portal(struct spdk_iscsi_portal_grp *pg,
			    struct spdk_iscsi_portal *p)
{
	assert(pg != NULL);
	/* [한국어] invariant. */
	assert(p != NULL);
	/* [한국어] invariant. */

	p->group = pg;
	/* [한국어] 백포인터 설정 — open/close에서 sock_group 접근에 사용. */
	TAILQ_INSERT_TAIL(&pg->head, p, per_pg_tailq);
	/* [한국어] PG 리스트에 부착. */
}

/*
 * [한국어]
 * iscsi_portal_grp_find_portal_by_addr - PG 내에서 (host, port) 일치 포털 검색.
 *
 * @pg: 검색 대상 PG.
 * @host/port: 키.
 * @return: 포털 또는 NULL.
 *
 * iscsi_portal_find_by_addr는 전역 검색이지만, 이 함수는 PG 내부 검색에 한정된다.
 */
struct spdk_iscsi_portal *
iscsi_portal_grp_find_portal_by_addr(struct spdk_iscsi_portal_grp *pg,
				     const char *host, const char *port)
{
	struct spdk_iscsi_portal *p;
	/* [한국어] 순회용. */

	TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
		/* [한국어] PG 리스트 순회. */
		if (!strcmp(p->host, host) && !strcmp(p->port, port)) {
			/* [한국어] (host, port) 정확 매치. */
			return p;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * iscsi_portal_grp_set_chap_params - PG의 CHAP 정책 변경.
 *
 * @pg: 대상 PG.
 * @disable/require/mutual_chap, chap_group: 새 정책.
 * @return: 0 성공, -EINVAL (정합성 위반).
 *
 * iscsi_check_chap_params로 (disable, require, mutual, group) 조합의 유효성을 먼저
 * 검증하고 통과하면 일괄 대입.
 */
int
iscsi_portal_grp_set_chap_params(struct spdk_iscsi_portal_grp *pg,
				 bool disable_chap, bool require_chap,
				 bool mutual_chap, int32_t chap_group)
{
	if (!iscsi_check_chap_params(disable_chap, require_chap,
				     mutual_chap, chap_group)) {
		/* [한국어] 모순된 조합(예: disable=true이면서 require=true) 거부. */
		return -EINVAL;
	}

	pg->disable_chap = disable_chap;
	pg->require_chap = require_chap;
	pg->mutual_chap = mutual_chap;
	pg->chap_group = chap_group;
	/* [한국어] 정책 일괄 갱신. 이후 새 로그인부터 적용. */

	return 0;
}

/*
 * [한국어]
 * iscsi_portal_grp_find_by_tag - tag로 PG 검색.
 *
 * @tag: 식별자.
 * @return: PG 또는 NULL.
 *
 * 호출자가 g_iscsi.mutex 보유 상태에서 호출해야 일관 결과 보장.
 */
struct spdk_iscsi_portal_grp *
iscsi_portal_grp_find_by_tag(int tag)
{
	struct spdk_iscsi_portal_grp *pg;
	/* [한국어] 순회용. */

	TAILQ_FOREACH(pg, &g_iscsi.pg_head, tailq) {
		/* [한국어] 등록된 모든 PG 순회. */
		if (pg->tag == tag) {
			/* [한국어] tag 일치. */
			return pg;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * iscsi_portal_grps_destroy - 모든 PG 일괄 destroy (셧다운 경로).
 *
 * 락을 획득한 채 헤드의 첫 PG를 분리하고, destroy 호출 직전에 락을 풀고 다시 잡는다.
 * iscsi_portal_grp_destroy 안에서 portal_destroy가 g_iscsi.mutex를 다시 획득하므로
 * 재진입성 회피를 위해 락을 잠시 풀었다 다시 잡는 패턴이 사용된다.
 */
void
iscsi_portal_grps_destroy(void)
{
	struct spdk_iscsi_portal_grp *pg;
	/* [한국어] 처리 대상 PG. */

	SPDK_DEBUGLOG(iscsi, "iscsi_portal_grps_destroy\n");
	/* [한국어] 진단 로그. */
	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] 락 획득. */
	while (!TAILQ_EMPTY(&g_iscsi.pg_head)) {
		/* [한국어] PG 리스트가 빌 때까지 반복. */
		pg = TAILQ_FIRST(&g_iscsi.pg_head);
		/* [한국어] 첫 PG. */
		TAILQ_REMOVE(&g_iscsi.pg_head, pg, tailq);
		/* [한국어] 전역에서 분리. */
		pthread_mutex_unlock(&g_iscsi.mutex);
		/* [한국어] portal_destroy가 같은 락을 잡으므로 임시 해제. */
		iscsi_portal_grp_destroy(pg);
		/* [한국어] PG와 자식 포털 free. */
		pthread_mutex_lock(&g_iscsi.mutex);
		/* [한국어] 다음 iteration 위해 다시 획득. */
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	/* [한국어] 최종 해제. */
}

/*
 * [한국어]
 * iscsi_portal_grp_open - PG의 sock_group과 acceptor_poller를 만들고 모든 포털 listen.
 *
 * @pg: 대상 PG.
 * @pause: true면 acceptor_poller를 일시정지 상태로 등록 (이후 unpause로 시작).
 * @return: 0 성공, 음수 errno.
 *
 * 단계: spdk_sock_group_create → SPDK_POLLER_REGISTER(iscsi_portal_group_poll, 1ms)
 *  → 옵션으로 poller 일시정지 → 모든 포털 iscsi_portal_open 호출.
 * 일시정지 옵션은 reactor가 아직 시작 안 된 상태에서 listen 큐에 connect 요청이
 * 누적되는 짧은 갭을 허용한다(주석 참조).
 */
int
iscsi_portal_grp_open(struct spdk_iscsi_portal_grp *pg, bool pause)
{
	struct spdk_iscsi_portal *p;
	/* [한국어] 순회용 포털. */
	int rc;
	/* [한국어] portal_open 결과. */

	pg->sock_group = spdk_sock_group_create(pg);
	/* [한국어] PG용 sock_group 생성. ctx로 pg 자신을 전달 (그룹 콜백에서 식별 가능). */
	if (pg->sock_group == NULL) {
		return -ENOMEM;
	}

	/*
	 * When the portal is created by config file, incoming connection
	 * requests for the socket are pended to accept until reactors start.
	 * However the gap between listen() and accept() will be slight and
	 * the requests will be queued by the nonzero backlog of the socket
	 * or resend by TCP.
	 */
	pg->acceptor_poller = SPDK_POLLER_REGISTER(iscsi_portal_group_poll, pg, ACCEPT_TIMEOUT_US);
	/* [한국어] 1ms 주기의 poller 등록. SPDK_POLLER_REGISTER 매크로는 spdk_poller_register
	 * 내부 호출 + 호출자 정보 (이름) 첨부. ctx=pg, 콜백=iscsi_portal_group_poll. */
	if (pg->acceptor_poller == NULL) {
		/* [한국어] 등록 실패 — sock_group close 후 -ENOMEM. */
		spdk_sock_group_close(&pg->sock_group);
		return -ENOMEM;
	}

	if (pause) {
		/* [한국어] 호출자가 명시적으로 시작 시점을 제어하고 싶으면 일단 정지. */
		spdk_poller_pause(pg->acceptor_poller);
	}

	TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
		/* [한국어] PG 내 모든 포털을 listen 시작. */
		rc = iscsi_portal_open(p);
		if (rc < 0) {
			/* [한국어] 어느 하나라도 실패 — 부분 성공 상태로 그대로 반환.
			 * (호출자가 close_all/destroy로 정리 책임). */
			return rc;
		}


	}
	return 0;
	/* [한국어] 모두 성공. */
}

/*
 * [한국어]
 * iscsi_portal_grp_close - PG의 acceptor poller 해제 + 모든 포털 close + sock_group close.
 *
 * @pg: 대상 PG.
 *
 * 종료 순서: poller 먼저 unregister하여 새 accept 콜백이 호출되지 않도록 한 뒤
 * 포털들을 close, 마지막으로 sock_group을 close하여 자원 회수.
 */
static void
iscsi_portal_grp_close(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal *p;
	/* [한국어] 순회용. */

	spdk_poller_unregister(&pg->acceptor_poller);
	/* [한국어] poller 해제 — &포인터로 전달하면 NULL로 셋팅됨. */

	TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
		/* [한국어] 모든 포털 close (group_remove + sock_close). */
		iscsi_portal_close(p);
	}

	spdk_sock_group_close(&pg->sock_group);
	/* [한국어] sock_group 자원 회수. */
}

/*
 * [한국어]
 * iscsi_portal_grp_close_all - 모든 PG의 listen을 일괄 종료.
 *
 * 셧다운 단계 또는 일괄 정지 RPC 경로에서 사용. g_iscsi.mutex를 잡은 채 close
 * (close 자체는 SPDK 자원만 다루므로 deadlock 없음).
 */
void
iscsi_portal_grp_close_all(void)
{
	struct spdk_iscsi_portal_grp *pg;
	/* [한국어] 순회용. */

	SPDK_DEBUGLOG(iscsi, "iscsi_portal_grp_close_all\n");
	/* [한국어] 진단 로그. */
	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] 락 획득. */
	TAILQ_FOREACH(pg, &g_iscsi.pg_head, tailq) {
		/* [한국어] 모든 PG에 대해 close. */
		iscsi_portal_grp_close(pg);
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	/* [한국어] 해제. */
}

/*
 * [한국어]
 * iscsi_portal_grp_unregister - tag로 PG를 g_iscsi.pg_head에서 분리만 하고 반환.
 *
 * @tag: 식별자.
 * @return: 분리된 PG (호출자가 destroy 책임) 또는 NULL.
 *
 * 삭제 RPC가 ref 검사 등 추가 단계를 거친 후 별도로 destroy/release하기 위함.
 */
struct spdk_iscsi_portal_grp *
iscsi_portal_grp_unregister(int tag)
{
	struct spdk_iscsi_portal_grp *pg;
	/* [한국어] 검색용. */

	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] 락. */
	TAILQ_FOREACH(pg, &g_iscsi.pg_head, tailq) {
		/* [한국어] 등록 PG 순회. */
		if (pg->tag == tag) {
			/* [한국어] 일치 — 분리하고 반환. */
			TAILQ_REMOVE(&g_iscsi.pg_head, pg, tailq);
			pthread_mutex_unlock(&g_iscsi.mutex);
			return pg;
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	/* [한국어] 미발견. */
	return NULL;
}

/*
 * [한국어]
 * iscsi_portal_grp_release - close + destroy 일괄 수행 (편의 함수).
 *
 * @pg: 대상 PG (이미 unregister된 상태여야 함).
 */
void
iscsi_portal_grp_release(struct spdk_iscsi_portal_grp *pg)
{
	iscsi_portal_grp_close(pg);
	/* [한국어] listen 자원 정리. */
	iscsi_portal_grp_destroy(pg);
	/* [한국어] PG와 포털 메모리 free. */
}

/*
 * [한국어]
 * iscsi_portal_grp_info_json - 단일 PG를 JSON 객체로 직렬화.
 *
 * 출력: { "tag":..., "portals":[{"host":..,"port":..}, ...], "private": bool }
 */
static void
iscsi_portal_grp_info_json(struct spdk_iscsi_portal_grp *pg,
			   struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_portal *portal;
	/* [한국어] 순회용. */

	spdk_json_write_object_begin(w);
	/* [한국어] { 시작. */

	spdk_json_write_named_int32(w, "tag", pg->tag);
	/* [한국어] "tag": <int>. */

	spdk_json_write_named_array_begin(w, "portals");
	/* [한국어] "portals": [ . */
	TAILQ_FOREACH(portal, &pg->head, per_pg_tailq) {
		/* [한국어] PG 내 포털 순회. */
		spdk_json_write_object_begin(w);
		/* [한국어] 각 포털을 object로 출력. */

		spdk_json_write_named_string(w, "host", portal->host);
		/* [한국어] "host": "...". */
		spdk_json_write_named_string(w, "port", portal->port);
		/* [한국어] "port": "...". */

		spdk_json_write_object_end(w);
		/* [한국어] 포털 object 종료. */
	}
	spdk_json_write_array_end(w);
	/* [한국어] portals 배열 종료. */

	spdk_json_write_named_bool(w, "private", pg->is_private);
	/* [한국어] "private": bool. */

	spdk_json_write_object_end(w);
	/* [한국어] PG object 종료. */
}

/*
 * [한국어]
 * iscsi_portal_grp_config_json - PG를 재생성 RPC method/params 형태로 직렬화.
 *
 * 출력: { "method":"iscsi_create_portal_group", "params": {...info...} }
 */
static void
iscsi_portal_grp_config_json(struct spdk_iscsi_portal_grp *pg,
			     struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	/* [한국어] wrapper 시작. */

	spdk_json_write_named_string(w, "method", "iscsi_create_portal_group");
	/* [한국어] RPC 이름. */

	spdk_json_write_name(w, "params");
	/* [한국어] params 키. */
	iscsi_portal_grp_info_json(pg, w);
	/* [한국어] info 형태를 그대로 params 값으로. */

	spdk_json_write_object_end(w);
	/* [한국어] wrapper 종료. */
}

/*
 * [한국어]
 * iscsi_portal_grps_info_json - 모든 PG를 정보 JSON으로 출력.
 */
void
iscsi_portal_grps_info_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_portal_grp *pg;
	/* [한국어] 순회용. */

	TAILQ_FOREACH(pg, &g_iscsi.pg_head, tailq) {
		/* [한국어] 등록된 PG 순회 출력. */
		iscsi_portal_grp_info_json(pg, w);
	}
}

/*
 * [한국어]
 * iscsi_portal_grps_config_json - 모든 PG를 재생성 RPC method/params 형태로 출력.
 */
void
iscsi_portal_grps_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_portal_grp *pg;
	/* [한국어] 순회용. */

	TAILQ_FOREACH(pg, &g_iscsi.pg_head, tailq) {
		/* [한국어] 모든 PG에 대해 method+params 출력. */
		iscsi_portal_grp_config_json(pg, w);
	}
}
