/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI Portal Group(포털 그룹) 관리 헤더 (portal_grp.h)
 *
 * === 파일의 역할 ===
 * iSCSI 타깃의 listening endpoint(포털)와 그 묶음(Portal Group, PG)을 관리하는 자료구조와
 * API를 선언한다. 한 포털은 (host, port) 한 쌍으로 정의되며 spdk_sock_listen()으로
 * TCP listen 소켓을 만든다. 여러 포털은 PG로 묶여 동일한 인증 정책(CHAP)·sock_group·
 * acceptor poller를 공유한다. 즉 이 헤더는 SPDK iSCSI 타깃의 "수신 측 네트워크 입구"를
 * 정의한다. tgt_node와 (PG, IG) 매핑을 통해 어떤 LUN을 어떤 포털·이니시에이터 조합에
 * 노출할지 결정된다 (RFC 3720 §6 Login phase, §10 SendTargets).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (제어 평면):
 *   RPC iscsi_create_portal_group → iscsi_portal_grp_create() → iscsi_portal_create()
 *     → iscsi_portal_grp_add_portal() → iscsi_portal_grp_open()
 *       → spdk_sock_listen() (per-portal) + spdk_sock_group_add_sock()
 *       → SPDK_POLLER_REGISTER(iscsi_portal_group_poll, ACCEPT_TIMEOUT_US=1ms)
 * 데이터 평면:
 *   acceptor_poller (1ms 주기) → spdk_sock_group_poll → iscsi_portal_accept()
 *     → spdk_sock_accept() → iscsi_conn_construct() → 새 spdk_iscsi_conn 생성
 * 실행 컨텍스트: PG의 sock_group과 acceptor_poller는 SPDK thread(보통 main reactor)에서
 * 동작. 포털 등록·해제 등 변경은 g_iscsi.mutex로 직렬화.
 *
 * === 타 모듈과의 연결 ===
 * - iscsi.h: 전역 g_iscsi.portal_head, g_iscsi.pg_head, g_iscsi.mutex 공유. CHAP 기본값.
 * - conn.h: 새로 accept된 소켓을 conn으로 감싸기 위해 iscsi_conn_construct() 호출.
 * - sock.h (SPDK): spdk_sock_listen/accept/group, ACCEPT_TIMEOUT_US 단위 polling.
 * - tgt_node.h: tgt_node가 (PG, IG) 매핑으로 PG를 참조 (ref 카운트로 보호).
 * - JSON RPC: portal/PG 정보의 직렬화 (info/config_json).
 * 데이터 흐름: 외부 TCP SYN → listen 소켓 → accept → 새 spdk_sock → iscsi_conn_construct
 * → 로그인 phase → (PG, IG) 매핑 검사 → tgt_node에 attach.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_iscsi_portal: 단일 (host, port) 포털, listen sock, 부모 PG 포인터.
 * - struct spdk_iscsi_portal_grp: 포털들의 묶음, sock_group, acceptor_poller, CHAP 정책.
 * - iscsi_portal_create()/destroy(): 단일 포털 생성·해제 (전역 portal_head 등록).
 * - iscsi_portal_grp_create()/destroy(): PG 생성·해제 (CHAP 기본값 복사).
 * - iscsi_portal_grp_add_portal(): 포털을 PG에 부착 (PG의 head TAILQ).
 * - iscsi_portal_grp_open()/close_all(): listen 시작/일괄 종료.
 * - iscsi_portal_grp_register/unregister/find_by_tag(): 전역 pg_head 관리.
 * - iscsi_parse_redirect_addr(): 로그인 redirect 응답을 위한 sockaddr 파싱.
 */

#ifndef SPDK_PORTAL_GRP_H
#define SPDK_PORTAL_GRP_H
/* [한국어] 이중 include 방지 가드. */

#include "spdk/conf.h"
/* [한국어] (legacy) 설정 파일 파서. SPDK_CONF_* 매크로 등을 위해 포함. */
#include "spdk/cpuset.h"
/* [한국어] CPU 코어 마스크 표현. PG의 코어 친화도 표시 등에 사용 가능. */
#include "iscsi/iscsi.h"
/* [한국어] iSCSI 도메인 상수와 g_iscsi 전역. */

struct spdk_json_write_ctx;
/* [한국어] forward 선언 — JSON 직렬화 함수 시그니처용. spdk/json.h 직접 include 회피. */

struct spdk_iscsi_portal {
	/* [한국어] 단일 listening endpoint(host:port)를 표현. PG에 속하면서 동시에 전역
	 * portal_head에도 연결되어 (host, port) 중복을 검사할 수 있게 한다. */

	struct spdk_iscsi_portal_grp	*group;
	/* [한국어] 이 포털이 속한 PG에 대한 백포인터.
	 * 설정자: iscsi_portal_grp_add_portal()에서 PG에 부착될 때 설정.
	 * 읽는 자: iscsi_portal_open()/close()에서 PG의 sock_group을 참조하기 위해 사용.
	 * 값 범위: 부착 전 NULL, 부착 후 유효한 PG 포인터.
	 * 동기화: 부착·분리는 g_iscsi.mutex 보유 상태 또는 단일 RPC thread에서 수행. */

	char				host[MAX_PORTAL_ADDR + 1];
	/* [한국어] 호스트 주소 문자열 (IPv4/IPv6 또는 "0.0.0.0"/"[::]" 와일드카드).
	 * 설정자: iscsi_portal_create()에서 입력 검증 후 memcpy.
	 * 읽는 자: iscsi_portal_open()의 spdk_sock_listen(host, ...) 인자.
	 * 값 범위: 길이 ≤ MAX_PORTAL_ADDR(=256). "[*]"/"*"는 입력 시 자동 변환됨.
	 * 동기화: 생성 후 read-only. */

	char				port[MAX_PORTAL_PORT + 1];
	/* [한국어] 포트 번호 문자열 (예: "3260").
	 * 설정자: iscsi_portal_create()에서 memcpy.
	 * 읽는 자: iscsi_portal_open()에서 strtol로 정수 변환 후 listen.
	 * 값 범위: 길이 ≤ MAX_PORTAL_PORT(=32). 1~65535 정수가 허용.
	 * 동기화: 생성 후 read-only. */

	struct spdk_sock		*sock;
	/* [한국어] listen 소켓 핸들 (spdk_sock_listen 결과).
	 * 설정자: iscsi_portal_open()에서 listen 성공 시 대입.
	 * 읽는 자: iscsi_portal_close()에서 group_remove_sock + sock_close.
	 * 값 범위: 미오픈 NULL, 오픈 후 유효 포인터.
	 * 동기화: open/close는 RPC thread에서 단일하게 호출. */

	TAILQ_ENTRY(spdk_iscsi_portal)	per_pg_tailq;
	/* [한국어] 부모 PG의 head TAILQ에 연결되는 링크.
	 * 설정자: iscsi_portal_grp_add_portal()에서 INSERT_TAIL.
	 * 읽는 자: PG open/close 순회.
	 * 동기화: PG 단위로 RPC thread에서만 변경. */

	TAILQ_ENTRY(spdk_iscsi_portal)	g_tailq;
	/* [한국어] 전역 g_iscsi.portal_head TAILQ에 연결되는 링크.
	 * 설정자: iscsi_portal_create()에서 g_iscsi.mutex 보유 상태로 INSERT_TAIL.
	 * 읽는 자: iscsi_portal_find_by_addr()로 (host, port) 중복 검사.
	 * 동기화: g_iscsi.mutex. */
};

struct spdk_iscsi_portal_grp {
	/* [한국어] 한 묶음의 포털과 공통 인증 정책·acceptor를 담는 컨테이너. */

	int					ref;
	/* [한국어] 이 PG를 참조하는 tgt_node 매핑 수.
	 * 설정자: tgt_node가 PG에 attach될 때 ++, detach 시 --.
	 * 읽는 자: PG 삭제 RPC가 ref>0이면 거부.
	 * 동기화: g_iscsi.mutex. */

	int					tag;
	/* [한국어] PG 식별자 (사용자 지정 양의 정수).
	 * 설정자: iscsi_portal_grp_create(tag) 시 1회 설정 후 불변.
	 * 읽는 자: iscsi_portal_grp_find_by_tag(), tgt_node 매핑.
	 * 동기화: 생성 후 read-only. */

	/* For login redirection, there are two types of portal groups, public and
	 * private portal groups. Public portal groups have their portals returned
	 * by a discovery session. Private portal groups do not have their portals
	 * returned by a discovery session. A public portal group may optionally
	 * specify a redirect portal for non-discovery logins. This redirect portal
	 * must be from a private portal group.
	 */
	bool					is_private;
	/* [한국어] 이 PG가 private(=Discovery 응답에 포함되지 않음) 인지 여부.
	 * 설정자: iscsi_portal_grp_create(tag, is_private) 시 RPC 입력으로 결정.
	 * 읽는 자: SendTargets 응답 작성, 로그인 redirect 검증.
	 * 값 범위: false(=public, 기본) / true(=private).
	 * 동기화: 생성 후 read-only. */

	bool					disable_chap;
	/* [한국어] CHAP 인증을 완전히 비활성화할지.
	 * 설정자: 생성 시 g_iscsi.disable_chap에서 복사, 이후 set_chap_params로 변경.
	 * 읽는 자: 로그인 단계 인증 흐름 결정.
	 * 동기화: g_iscsi.mutex 보유 상태에서 변경. */

	bool					require_chap;
	/* [한국어] CHAP 인증을 강제 요구할지.
	 * 설정자/읽는 자/동기화: disable_chap과 동일. */

	bool					mutual_chap;
	/* [한국어] 양방향 CHAP(target도 자신을 인증)을 요구할지.
	 * 설정자/읽는 자/동기화: 위와 동일. */

	int32_t					chap_group;
	/* [한국어] 사용할 인증 그룹 tag(=spdk_iscsi_auth_group의 tag).
	 * 설정자: 생성 시 g_iscsi.chap_group 복사 또는 set_chap_params.
	 * 읽는 자: iscsi_chap_get_authinfo(...).
	 * 값 범위: 0=글로벌 기본, 양수=특정 그룹.
	 * 동기화: g_iscsi.mutex. */

	struct spdk_sock_group			*sock_group;
	/* [한국어] 이 PG에 속한 listen 소켓들이 묶이는 SPDK sock_group.
	 * 설정자: iscsi_portal_grp_open()에서 spdk_sock_group_create.
	 * 읽는 자: iscsi_portal_open()의 add_sock, accept poller의 group_poll.
	 * 동기화: 단일 SPDK thread(메인 reactor)에서만 접근 — sock_group 자체는 thread-affine. */

	struct spdk_poller			*acceptor_poller;
	/* [한국어] sock_group을 ACCEPT_TIMEOUT_US(=1ms) 주기로 폴링하여 새 연결을 수락하는 poller.
	 * 설정자: iscsi_portal_grp_open()에서 SPDK_POLLER_REGISTER.
	 * 읽는 자: spdk_poller_pause/unpause, unregister.
	 * 실행 컨텍스트: SPDK thread의 polling 루프 안에서 ACCEPT_TIMEOUT_US 간격 호출.
	 * 동기화: 단일 thread polling 모델 — 별도 락 불필요. */

	TAILQ_ENTRY(spdk_iscsi_portal_grp)	tailq;
	/* [한국어] 전역 g_iscsi.pg_head 링크.
	 * 설정자: iscsi_portal_grp_register()에서 g_iscsi.mutex 보유 상태로 삽입.
	 * 읽는 자: iscsi_portal_grp_find_by_tag()의 TAILQ_FOREACH.
	 * 동기화: g_iscsi.mutex. */

	TAILQ_HEAD(, spdk_iscsi_portal)		head;
	/* [한국어] 이 PG에 속한 spdk_iscsi_portal 리스트의 헤드.
	 * 설정자: iscsi_portal_grp_create()에서 TAILQ_INIT, add_portal()로 INSERT_TAIL.
	 * 읽는 자: open/close 순회, JSON 직렬화.
	 * 동기화: PG 단위로 RPC thread에서 변경. */
};

/* SPDK iSCSI Portal Group management API */
/* [한국어] === 외부 API: RPC 핸들러와 셧다운 경로에서 호출. === */

struct spdk_iscsi_portal *iscsi_portal_create(const char *host, const char *port);
/* [한국어] (host, port) 포털을 생성, 전역 portal_head에 등록 (중복 검사). 실패 시 NULL. */
void iscsi_portal_destroy(struct spdk_iscsi_portal *p);
/* [한국어] 포털을 전역 portal_head에서 분리하고 free. listen sock은 close되어 있어야 함. */

struct spdk_iscsi_portal_grp *iscsi_portal_grp_create(int tag, bool is_private);
/* [한국어] 빈 PG 객체를 만들고 CHAP 기본값을 g_iscsi에서 복사. 등록은 별도 단계. */
void iscsi_portal_grp_add_portal(struct spdk_iscsi_portal_grp *pg,
				 struct spdk_iscsi_portal *p);
/* [한국어] 포털을 PG의 head 리스트에 부착(p->group=pg 동시 설정). */
struct spdk_iscsi_portal *iscsi_portal_grp_find_portal_by_addr(
	struct spdk_iscsi_portal_grp *pg, const char *host, const char *port);
/* [한국어] PG 내에서 (host, port)로 포털 검색 (전역 검색이 아닌 PG 범위). */

void iscsi_portal_grp_destroy(struct spdk_iscsi_portal_grp *pg);
/* [한국어] PG에 속한 포털을 모두 destroy 후 PG 자체 free. close는 호출자 책임. */
void iscsi_portal_grp_release(struct spdk_iscsi_portal_grp *pg);
/* [한국어] close + destroy를 한 번에 수행하는 편의 함수. */
int iscsi_parse_portal_grps(void);
/* [한국어] (legacy) conf 파일 기반 PG 일괄 파싱. */
void iscsi_portal_grps_destroy(void);
/* [한국어] 모든 PG 일괄 destroy (셧다운 경로). */
int iscsi_portal_grp_register(struct spdk_iscsi_portal_grp *pg);
/* [한국어] PG를 g_iscsi.pg_head에 등록 (tag 중복 검사 포함). */
struct spdk_iscsi_portal_grp *iscsi_portal_grp_unregister(int tag);
/* [한국어] tag로 PG를 g_iscsi.pg_head에서 떼어 반환 (호출자가 destroy 책임). */
struct spdk_iscsi_portal_grp *iscsi_portal_grp_find_by_tag(int tag);
/* [한국어] tag로 PG 검색. 호출자가 g_iscsi.mutex 보유 가정. */
int iscsi_portal_grp_open(struct spdk_iscsi_portal_grp *pg, bool pause);
/* [한국어] PG의 sock_group/acceptor_poller 생성 + 모든 포털 listen 시작. pause=true면
 * poller를 일시정지 상태로 등록 (이후 resume 가능). */
int iscsi_portal_grp_set_chap_params(struct spdk_iscsi_portal_grp *pg,
				     bool disable_chap, bool require_chap,
				     bool mutual_chap, int32_t chap_group);
/* [한국어] PG의 CHAP 정책 변경. 유효성 검증(iscsi_check_chap_params) 후 대입. */

void iscsi_portal_grp_close_all(void);
/* [한국어] 모든 PG의 listen 소켓을 일괄 close (셧다운 또는 일괄 정지 경로). */
void iscsi_portal_grps_info_json(struct spdk_json_write_ctx *w);
/* [한국어] 모든 PG의 현재 상태를 JSON으로 출력 (RPC get). */
void iscsi_portal_grps_config_json(struct spdk_json_write_ctx *w);
/* [한국어] 모든 PG를 재생성 RPC method/params 형태로 출력 (save_config). */

int iscsi_parse_redirect_addr(struct sockaddr_storage *sa,
			      const char *host, const char *port);
/* [한국어] 로그인 redirect 응답에 사용할 (host, port) 문자열을 numeric host/service 모드로
 * getaddrinfo 호출하여 sockaddr_storage로 채운다. AI_NUMERICHOST|NUMERICSERV로 DNS 조회 회피. */

#endif /* SPDK_PORTAL_GRP_H */
/* [한국어] include guard 종료. */
