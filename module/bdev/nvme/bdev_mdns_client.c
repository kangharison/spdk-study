/*  SPDX-License-Identifier: BSD-3-Clause
 *  Copyright (c) 2022 Dell Inc, or its subsidiaries.
 *  All rights reserved.
 */

/*
 * [한국어 설명] mDNS 기반 NVMe-oF Discovery 자동 발견 클라이언트 (bdev_mdns_client.c)
 *
 * === 파일의 역할 ===
 * NVMe-oF의 Centralized Discovery Controller(CDC)나 일반 Discovery 서비스를 mDNS
 * (Multicast DNS)로 자동 발견해 SPDK가 자동으로 attach하도록 만드는 모듈이다.
 * Avahi 클라이언트를 사용해 LAN에 광고된 _nvme-disc._tcp 같은 서비스를 browse하고,
 * 발견된 서비스에서 NVMe Discovery Controller의 IP/포트/SUBNQN을 추출한 뒤
 * bdev_nvme_start_discovery()를 호출해 NVMe-oF Discovery 흐름을 시작한다.
 *
 * 빌드 시 --with-avahi 옵션이 없으면 이 파일의 함수들은 -ENOTSUP을 반환하는 stub만 컴파일.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   사용자 RPC bdev_nvme_start_mdns_discovery
 *     → bdev_nvme_start_mdns_discovery() (이 파일)
 *     → Avahi: simple_poll/client/service_browser 초기화
 *     → SPDK poller bdev_nvme_avahi_iterate가 100ms마다 avahi_simple_poll_iterate 호출
 *     → Avahi가 광고된 서비스 발견 시 mdns_browse_handler → service_resolver_new
 *     → mdns_resolve_handler에서 TXT 파싱 (NQN, p=protocol)
 *     → entry_ctx 만들어 spdk_thread_send_msg로 app 스레드에 mdns_bdev_nvme_start_discovery 전송
 *     → app 스레드: bdev_nvme_start_discovery() (NVMe-oF Discovery Service에 connect)
 *     → 결과적으로 발견된 NVM 서브시스템들이 bdev_nvme로 자동 attach.
 *
 * 실행 컨텍스트: Avahi poller는 SPDK 임의 스레드(보통 첫 reactor)에서 100ms 주기로 실행.
 * Avahi resolve 콜백 → bdev_nvme_start_discovery 호출은 반드시 app 스레드여야 하므로
 * spdk_thread_send_msg로 cross-thread 전달.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: avahi-client, avahi-common (mDNS 클라이언트), spdk/thread.h (poller, send_msg),
 *         bdev_nvme.h (bdev_nvme_start_discovery), spdk/nvme.h (transport_id 등),
 *         spdk/jsonrpc.h (info 응답).
 * - 의존받음: bdev_nvme_rpc.c (RPC 핸들러가 본 파일의 start/stop/get_info 호출),
 *             bdev_nvme.h (전방 선언으로 함수 시그니처 공유).
 * - 데이터 흐름: LAN multicast → Avahi daemon → Avahi client → 본 파일 콜백 →
 *               bdev_nvme_start_discovery → NVMe-oF Discovery Controller에 TCP connect.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct mdns_discovery_ctx: 한 mDNS 디스커버리 세션의 라이프사이클 컨텍스트.
 * - struct mdns_discovery_entry_ctx: resolve된 디스커버리 컨트롤러 1개당 entry.
 * - mdns_browse_handler: 새 서비스 발견 시 service_resolver 생성.
 * - mdns_resolve_handler: 서비스 메타(IP/포트/TXT) 파싱 → entry 생성 → app 스레드로 메시지.
 * - bdev_nvme_avahi_iterate: SPDK poller. avahi_simple_poll_iterate를 100ms마다 호출.
 * - bdev_nvme_start_mdns_discovery / _stop_mdns_discovery: 외부 진입점.
 */

#include "spdk/stdinc.h"
#include "spdk/version.h"

#include "spdk_internal/event.h"   /* [한국어] SPDK 이벤트/스레드 내부 API. */

#include "spdk/assert.h"           /* [한국어] assert 매크로. */
#include "spdk/config.h"           /* [한국어] SPDK_CONFIG_AVAHI 매크로. */
#include "spdk/env.h"
#include "spdk/init.h"
#include "spdk/log.h"
#include "spdk/thread.h"           /* [한국어] spdk_thread_send_msg, app_thread. */
#include "spdk/trace.h"
#include "spdk/string.h"
#include "spdk/scheduler.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/nvme.h"             /* [한국어] spdk_nvme_transport_id 등. */
#include "spdk/module/bdev/nvme.h" /* [한국어] 외부 공개 옵션 구조체. */
#include "bdev_nvme.h"              /* [한국어] bdev_nvme_start_discovery 등. */

#ifdef SPDK_CONFIG_AVAHI
/* [한국어] Avahi 빌드 분기. --with-avahi 옵션 없으면 아래 #else 분기로 stub만 컴파일. */
#include <avahi-client/client.h>      /* [한국어] AvahiClient (mDNS 데몬과 통신). */
#include <avahi-client/lookup.h>      /* [한국어] AvahiServiceBrowser/Resolver. */
#include <avahi-common/simple-watch.h>/* [한국어] AvahiSimplePoll (이벤트 루프). */
#include <avahi-common/malloc.h>      /* [한국어] avahi_free. */
#include <avahi-common/error.h>       /* [한국어] avahi_strerror. */

/* [한국어] Avahi 메인 루프. SPDK poller가 100ms마다 iterate를 호출.
 * 모든 mdns_discovery_ctx가 공유. */
static AvahiSimplePoll *g_avahi_simple_poll = NULL;
/* [한국어] Avahi 데몬과의 클라이언트 핸들. service_browser_new 등에 인자로 전달.
 * 모든 디스커버리 세션이 공유. */
static AvahiClient *g_avahi_client = NULL;

/*
 * [한국어]
 * struct mdns_discovery_entry_ctx - 발견된 1개의 NVMe-oF Discovery Controller 엔트리.
 *
 * 한 mDNS 세션이 여러 디스커버리 컨트롤러를 발견할 수 있으며, 각 발견에 대해 이 구조체가
 * 만들어져 entry list에 추가된다.
 */
struct mdns_discovery_entry_ctx {
	char                                            name[256];
	/* [한국어] 자동 생성된 엔트리 이름. 형식: "<base_name><seqno>_nvme".
	 * bdev_nvme_start_discovery에 base_name 인자로 전달됨. */
	struct spdk_nvme_transport_id                   trid;
	/* [한국어] resolve된 트랜스포트 ID (TCP/IPv4 + traddr/trsvcid/subnqn).
	 * 현재 코드는 TCP+IPv4만 처리. */
	struct spdk_nvme_ctrlr_opts                     drv_opts;
	/* [한국어] lib/nvme 드라이버 옵션 사본 (hostnqn 등 포함). */
	TAILQ_ENTRY(mdns_discovery_entry_ctx)           tailq;
	/* [한국어] 부모 ctx의 mdns_discovery_entry_ctxs 리스트 노드. */
	struct mdns_discovery_ctx                       *ctx;
	/* [한국어] 부모 mdns_discovery_ctx 역참조 (start_discovery에서 옵션 참조용). */
};

/*
 * [한국어]
 * struct mdns_discovery_ctx - 한 mDNS 디스커버리 세션의 라이프사이클.
 *
 * 사용자가 bdev_nvme_start_mdns_discovery로 시작하면 1개 생성되어 g_mdns_discovery_ctxs에
 * 등록되고, stop_mdns_discovery로 종료될 때까지 poller가 Avahi 이벤트 루프를 돌린다.
 */
struct mdns_discovery_ctx {
	char                                    *name;
	/* [한국어] 사용자가 지정한 base_name. 발견된 ctrlr 이름 prefix로 사용. */
	char                                    *svcname;
	/* [한국어] mDNS 서비스 타입 문자열 (예: "_nvme-disc._tcp"). */
	char                                    *hostnqn;
	/* [한국어] 호스트 NQN 사본 (drv_opts에서 복제). entry별로 strncpy. */
	AvahiServiceBrowser                     *sb;
	/* [한국어] Avahi 서비스 브라우저 핸들. svcname을 watch. */
	struct spdk_poller                      *poller;
	/* [한국어] bdev_nvme_avahi_iterate를 100ms마다 호출하는 poller. */
	struct spdk_nvme_ctrlr_opts             drv_opts;
	/* [한국어] 디스커버리/attach에 사용할 lib/nvme 옵션 사본. */
	struct spdk_bdev_nvme_ctrlr_opts        bdev_opts;
	/* [한국어] bdev_nvme 옵션 사본. */
	uint32_t                                seqno;
	/* [한국어] entry 이름 부여용 시퀀스. resolve할 때마다 ++. */
	bool                                    stop;
	/* [한국어] true면 다음 poller 실행 시 자기 자신을 unregister하고 ctx 해제. */
	TAILQ_ENTRY(mdns_discovery_ctx)         tailq;
	/* [한국어] g_mdns_discovery_ctxs 리스트 노드. */
	TAILQ_HEAD(, mdns_discovery_entry_ctx)  mdns_discovery_entry_ctxs;
	/* [한국어] 이 세션이 발견한 모든 entry 리스트. */
};

/* [한국어] 모든 mDNS 디스커버리 세션의 전역 헤드. */
TAILQ_HEAD(mdns_discovery_ctxs, mdns_discovery_ctx);
static struct mdns_discovery_ctxs g_mdns_discovery_ctxs = TAILQ_HEAD_INITIALIZER(
			g_mdns_discovery_ctxs);

/*
 * [한국어]
 * create_mdns_discovery_entry_ctx - 발견된 디스커버리 컨트롤러용 entry 컨텍스트 생성.
 *
 * @ctx: 부모 mDNS 세션.
 * @trid: resolve된 트랜스포트 ID (TCP IP/포트/SUBNQN).
 * @return: 새 entry 또는 NULL (OOM).
 *
 * entry 이름은 "<base_name><seqno>_nvme" 형식 (충돌 방지로 seqno 증가).
 * drv_opts는 부모에서 복사하되 hostnqn은 부모의 hostnqn 문자열로 강제 통일.
 * 컨텍스트: Avahi resolve 콜백 (poller 스레드).
 */
static struct mdns_discovery_entry_ctx *
create_mdns_discovery_entry_ctx(struct mdns_discovery_ctx *ctx, struct spdk_nvme_transport_id *trid)
{
	struct mdns_discovery_entry_ctx *new_ctx;

	assert(ctx);
	assert(trid);
	new_ctx = calloc(1, sizeof(*new_ctx));
	if (new_ctx == NULL) {
		SPDK_ERRLOG("could not allocate new mdns_entry_ctx\n");
		return NULL;
	}

	new_ctx->ctx = ctx;                                                              /* [한국어] 부모 세션 역참조. */
	memcpy(&new_ctx->trid, trid, sizeof(struct spdk_nvme_transport_id));             /* [한국어] trid 복사. */
	snprintf(new_ctx->name, sizeof(new_ctx->name), "%s%u_nvme", ctx->name, ctx->seqno); /* [한국어] 이름 자동 생성. */
	memcpy(&new_ctx->drv_opts, &ctx->drv_opts, sizeof(ctx->drv_opts));               /* [한국어] 옵션 복사. */
	snprintf(new_ctx->drv_opts.hostnqn, sizeof(ctx->drv_opts.hostnqn), "%s", ctx->hostnqn); /* [한국어] hostnqn 통일. */
	ctx->seqno = ctx->seqno + 1;                                                      /* [한국어] 다음 entry용 시퀀스++. */
	return new_ctx;
}

/*
 * [한국어]
 * mdns_bdev_nvme_start_discovery - app 스레드에서 실행되는 디스커버리 시작 wrapper.
 *
 * Avahi resolve 콜백은 poller 스레드에서 실행되지만 bdev_nvme_start_discovery는
 * 반드시 app 스레드에서 호출해야 하므로, spdk_thread_send_msg로 메시지를 보내고
 * 그 메시지의 cb_fn이 이 함수다.
 *
 * 인자 from_mdns=true는 bdev_nvme.c에서 save_config 시 mDNS로 발견된 디스커버리는
 * 직접 저장 안 하고 mdns config에서 복원하도록 표시.
 */
static void
mdns_bdev_nvme_start_discovery(void *_entry_ctx)
{
	int status;
	struct mdns_discovery_entry_ctx *entry_ctx = _entry_ctx;

	assert(_entry_ctx);
	/* [한국어] NVMe-oF Discovery 시작. timeout=0(=기본값), from_mdns=true.
	 * 콜백은 NULL → 결과 무시 (실패하면 ERRLOG만). */
	status = bdev_nvme_start_discovery(&entry_ctx->trid, entry_ctx->name,
					   &entry_ctx->ctx->drv_opts,
					   &entry_ctx->ctx->bdev_opts,
					   0, true, NULL, NULL);
	if (status) {
		SPDK_ERRLOG("Error starting discovery for name %s addr %s port %s subnqn %s &trid %p\n",
			    entry_ctx->ctx->name, entry_ctx->trid.traddr, entry_ctx->trid.trsvcid,
			    entry_ctx->trid.subnqn, &entry_ctx->trid);
	}
}

/*
 * [한국어]
 * free_mdns_discovery_entry_ctx - 부모 ctx의 모든 entry를 일괄 해제.
 *
 * 호출자: free_mdns_discovery_ctx (세션 종료 시).
 */
static void
free_mdns_discovery_entry_ctx(struct mdns_discovery_ctx *ctx)
{
	struct mdns_discovery_entry_ctx *entry_ctx, *tmp;

	if (!ctx) {
		return;
	}

	TAILQ_FOREACH_SAFE(entry_ctx, &ctx->mdns_discovery_entry_ctxs, tailq, tmp) {
		TAILQ_REMOVE(&ctx->mdns_discovery_entry_ctxs, entry_ctx, tailq);
		free(entry_ctx);
	}
}

/*
 * [한국어]
 * free_mdns_discovery_ctx - mDNS 세션 컨텍스트 전체 해제.
 *
 * Avahi service_browser, 모든 entry, strdup 문자열들 일괄 정리.
 * 호출자: bdev_nvme_avahi_iterate가 stop 감지 후 호출.
 */
static void
free_mdns_discovery_ctx(struct mdns_discovery_ctx *ctx)
{
	if (!ctx) {
		return;
	}

	free(ctx->name);
	free(ctx->svcname);
	free(ctx->hostnqn);
	avahi_service_browser_free(ctx->sb);   /* [한국어] Avahi 서비스 브라우저 해제 (콜백 등록 취소). */
	free_mdns_discovery_entry_ctx(ctx);
	free(ctx);
}

/* get_key_val_avahi_resolve_txt - Search for the key string in the TXT received
 *                            from Avavi daemon and return its value.
 *   input
 *       txt: TXT returned by Ahavi daemon will be of format
 *            "NQN=nqn.1988-11.com.dell:SFSS:1:20221122170722e8" "p=tcp foo" and the
 *            AvahiStringList txt is a linked list with each node holding a
 *            key-value pair like key:p value:tcp
 *
 *       key: Key string to search in the txt list
 *   output
 *       Returns the value for the key or NULL if key is not present
 *       Returned string needs to be freed with avahi_free()
 */
/*
 * [한국어]
 * get_key_val_avahi_resolve_txt - mDNS TXT 레코드의 key=value 페어에서 value 조회.
 *
 * @txt: Avahi가 반환한 TXT 레코드 (key=value 페어들의 연결 리스트).
 * @key: 찾을 키 (예: "NQN", "p").
 * @return: 찾은 value 문자열 (호출자가 avahi_free로 해제), 없으면 NULL.
 *
 * NVMe-oF Discovery mDNS TXT 표준 필드:
 *   - NQN: 서브시스템 NQN
 *   - p:   transport protocol ("tcp" 등)
 */
static char *
get_key_val_avahi_resolve_txt(AvahiStringList *txt, const char *key)
{
	char *k = NULL, *v = NULL;   /* [한국어] Avahi가 채워줄 key/value 포인터. */
	AvahiStringList *p = NULL;    /* [한국어] 매치된 리스트 노드. */
	int r;

	if (!txt || !key) {
		return NULL;
	}

	/* [한국어] Avahi 헬퍼로 key를 가진 노드를 찾는다. */
	p = avahi_string_list_find(txt, key);
	if (!p) {
		return NULL;
	}

	/* [한국어] 노드를 key/value로 분리. 메모리는 Avahi가 할당. */
	r = avahi_string_list_get_pair(p, &k, &v, NULL);
	if (r < 0) {
		return NULL;
	}

	avahi_free(k);   /* [한국어] key 문자열은 우리가 안 씀 → 해제. */
	return v;         /* [한국어] value는 호출자에게 소유권 이전. */
}

/*
 * [한국어]
 * get_spdk_nvme_transport_from_proto_str - "tcp" 문자열을 SPDK_NVME_TRANSPORT_TCP enum으로 변환.
 *
 * 현재는 TCP만 지원. RDMA/FC mDNS 광고를 지원하려면 여기에 추가해야 함.
 */
static int
get_spdk_nvme_transport_from_proto_str(char *protocol, enum spdk_nvme_transport_type *trtype)
{
	int status = -1;   /* [한국어] 기본 에러. */

	if (!protocol || !trtype) {
		return status;
	}

	if (strcmp("tcp", protocol) == 0) {
		*trtype = SPDK_NVME_TRANSPORT_TCP;
		return 0;
	}

	return status;
}

/*
 * [한국어]
 * get_spdk_nvme_adrfam_from_avahi_addr - Avahi 주소 패밀리를 SPDK 주소 패밀리로 변환.
 *
 * Avahi는 INET(IPv4)/INET6 구분, SPDK는 SPDK_NVMF_ADRFAM_IPV4/_IPV6. NULL이면 IPv4 기본.
 */
static enum spdk_nvmf_adrfam
get_spdk_nvme_adrfam_from_avahi_addr(const AvahiAddress *address) {

	if (!address)
	{
		/* Return ipv4 by default */
		return SPDK_NVMF_ADRFAM_IPV4;
	}

	switch (address->proto)
	{
	case AVAHI_PROTO_INET:
		return SPDK_NVMF_ADRFAM_IPV4;   /* [한국어] IPv4. */
	case AVAHI_PROTO_INET6:
		return SPDK_NVMF_ADRFAM_IPV6;   /* [한국어] IPv6. */
	default:
		return SPDK_NVMF_ADRFAM_IPV4;   /* [한국어] 그 외(Unspec 등)는 IPv4 fallback. */
	}
}

/*
 * [한국어]
 * get_mdns_discovery_ctx_by_svcname - svcname으로 디스커버리 세션 lookup.
 *
 * resolve 콜백은 svc_type (=svcname) 문자열만 알려주므로 이 lookup이 필요하다.
 */
static struct mdns_discovery_ctx *
get_mdns_discovery_ctx_by_svcname(const char *svcname)
{
	struct mdns_discovery_ctx *ctx = NULL, *tmp_ctx = NULL;

	if (!svcname) {
		return NULL;
	}

	TAILQ_FOREACH_SAFE(ctx, &g_mdns_discovery_ctxs, tailq, tmp_ctx) {
		if (strcmp(ctx->svcname, svcname) == 0) {
			return ctx;
		}
	}
	return NULL;
}

/*
 * [한국어]
 * mdns_resolve_handler - Avahi 서비스 resolve 완료 콜백.
 *
 * @resolver: Avahi resolver 핸들 (콜백 종료 시 free).
 * @resolve_event: AVAHI_RESOLVER_FOUND/FAILURE 등.
 * @svc_name/svc_type/svc_domain: 서비스 식별자.
 * @host_name/host_address/port: 서비스가 위치한 호스트의 IP/포트.
 * @txt: TXT 레코드 (NQN, p 등 메타데이터).
 *
 * 동작 (FOUND 케이스):
 *   1) IP 주소를 ipaddr 문자열로 포맷.
 *   2) svc_type으로 mdns_discovery_ctx lookup.
 *   3) trid 할당, IPv4만 처리 (IPv6/UNIX 등은 skip).
 *   4) TXT에서 NQN, p(protocol) 추출.
 *   5) 중복 entry 검사 (이미 같은 trid가 있는지).
 *   6) entry 생성 후 spdk_thread_send_msg로 app 스레드에 시작 메시지 전달.
 * resolve 끝에는 항상 resolver_free.
 *
 * 실행 컨텍스트: Avahi poller 스레드 (bdev_nvme_avahi_iterate가 호출). app 스레드 아님!
 */
static void
mdns_resolve_handler(
	AvahiServiceResolver *resolver,
	AVAHI_GCC_UNUSED AvahiIfIndex intf,
	AVAHI_GCC_UNUSED AvahiProtocol avahi_protocol,
	AvahiResolverEvent resolve_event,
	const char *svc_name,
	const char *svc_type,
	const char *svc_domain,
	const char *host_name,
	const AvahiAddress *host_address,
	uint16_t port,
	AvahiStringList *txt,
	AvahiLookupResultFlags result_flags,
	AVAHI_GCC_UNUSED void *user_data)
{
	assert(resolver);
	/* The handler gets called whenever a service has been resolved
	   successfully or timed out */
	switch (resolve_event) {
	case AVAHI_RESOLVER_FOUND: {
		/* [한국어] FOUND 케이스: 서비스 정보가 모두 도착함 → trid 구성. */
		char ipaddr[SPDK_NVMF_TRADDR_MAX_LEN + 1], port_str[SPDK_NVMF_TRSVCID_MAX_LEN + 1], *str;
		struct spdk_nvme_transport_id *trid = NULL;
		char *subnqn = NULL, *proto = NULL;
		struct mdns_discovery_ctx *ctx = NULL;
		struct mdns_discovery_entry_ctx *entry_ctx = NULL;
		int status = -1;

		memset(ipaddr, 0, sizeof(ipaddr));     /* [한국어] 주소 버퍼 0-init. */
		memset(port_str, 0, sizeof(port_str)); /* [한국어] 포트 버퍼 0-init. */
		SPDK_INFOLOG(bdev_nvme, "Service '%s' of type '%s' in domain '%s'\n", svc_name, svc_type,
			     svc_domain);
		avahi_address_snprint(ipaddr, sizeof(ipaddr), host_address);
		snprintf(port_str, sizeof(port_str), "%d", port);
		str = avahi_string_list_to_string(txt);
		SPDK_INFOLOG(bdev_nvme,
			     "\t%s:%u (%s)\n"
			     "\tTXT=%s\n"
			     "\tcookie is %u\n"
			     "\tis_local: %i\n"
			     "\tour_own: %i\n"
			     "\twide_area: %i\n"
			     "\tmulticast: %i\n"
			     "\tcached: %i\n",
			     host_name, port, ipaddr,
			     str,
			     avahi_string_list_get_service_cookie(txt),
			     !!(result_flags & AVAHI_LOOKUP_RESULT_LOCAL),
			     !!(result_flags & AVAHI_LOOKUP_RESULT_OUR_OWN),
			     !!(result_flags & AVAHI_LOOKUP_RESULT_WIDE_AREA),
			     !!(result_flags & AVAHI_LOOKUP_RESULT_MULTICAST),
			     !!(result_flags & AVAHI_LOOKUP_RESULT_CACHED));
		avahi_free(str);

		/* [한국어] 1단계: svc_type으로 부모 mDNS 세션 lookup. */
		ctx = get_mdns_discovery_ctx_by_svcname(svc_type);
		if (!ctx) {
			SPDK_ERRLOG("Unknown Service '%s'\n", svc_type);
			break;
		}

		/* [한국어] 2단계: trid 동적 할당. */
		trid = (struct spdk_nvme_transport_id *) calloc(1, sizeof(struct spdk_nvme_transport_id));
		if (!trid) {
			SPDK_ERRLOG(" Error allocating memory for trid\n");
			break;
		}
		/* [한국어] 3단계: 주소 패밀리 결정. 현재는 IPv4만 처리. */
		trid->adrfam = get_spdk_nvme_adrfam_from_avahi_addr(host_address);
		if (trid->adrfam != SPDK_NVMF_ADRFAM_IPV4) {
			/* TODO: For now process only ipv4 addresses */
			SPDK_INFOLOG(bdev_nvme, "trid family is not IPV4 %d\n", trid->adrfam);
			free(trid);
			break;
		}
		/* [한국어] 4단계: TXT에서 subnqn 추출. NVMe-oF 표준 mDNS 키. */
		subnqn = get_key_val_avahi_resolve_txt(txt, "NQN");
		if (!subnqn) {
			free(trid);
			SPDK_ERRLOG("subnqn received is empty for service %s\n", ctx->svcname);
			break;
		}
		/* [한국어] 5단계: TXT에서 protocol(p) 추출 ("tcp" 등). */
		proto = get_key_val_avahi_resolve_txt(txt, "p");
		if (!proto) {
			free(trid);
			avahi_free(subnqn);
			SPDK_ERRLOG("Protocol not received for service %s\n", ctx->svcname);
			break;
		}
		/* [한국어] 6단계: protocol 문자열 → SPDK trtype enum. */
		status = get_spdk_nvme_transport_from_proto_str(proto, &trid->trtype);
		if (status) {
			free(trid);
			avahi_free(subnqn);
			avahi_free(proto);
			SPDK_ERRLOG("Unable to derive nvme transport type  for service %s\n", ctx->svcname);
			break;
		}
		/* [한국어] 7단계: trid 필드 채우기 (IP, 포트, SUBNQN). */
		snprintf(trid->traddr, sizeof(trid->traddr), "%s", ipaddr);
		snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%s", port_str);
		snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", subnqn);
		/* [한국어] 8단계: 중복 entry 체크 (이미 발견한 컨트롤러면 skip). */
		TAILQ_FOREACH(entry_ctx, &ctx->mdns_discovery_entry_ctxs, tailq) {
			if (!spdk_nvme_transport_id_compare(trid, &entry_ctx->trid)) {
				SPDK_ERRLOG("mDNS discovery entry exists already. trid->traddr: %s trid->trsvcid: %s\n",
					    trid->traddr, trid->trsvcid);
				free(trid);
				avahi_free(subnqn);
				avahi_free(proto);
				avahi_service_resolver_free(resolver);
				return;
			}
		}
		/* [한국어] 9단계: entry 생성 + 부모 리스트에 추가. */
		entry_ctx = create_mdns_discovery_entry_ctx(ctx, trid);
		TAILQ_INSERT_TAIL(&ctx->mdns_discovery_entry_ctxs, entry_ctx, tailq);
		/* [한국어] 10단계: app 스레드에 디스커버리 시작 메시지 전송 (cross-thread).
		 * bdev_nvme_start_discovery는 반드시 app 스레드에서만 호출 가능. */
		spdk_thread_send_msg(spdk_thread_get_app_thread(), mdns_bdev_nvme_start_discovery, entry_ctx);
		free(trid);                /* [한국어] entry_ctx에 복사했으므로 임시 trid 해제. */
		avahi_free(subnqn);        /* [한국어] Avahi가 할당한 메모리. */
		avahi_free(proto);
		break;
	}
	case AVAHI_RESOLVER_FAILURE:
		/* [한국어] 타임아웃 또는 resolve 실패. */
		SPDK_ERRLOG("(Resolver) Failed to resolve service '%s' of type '%s' in domain '%s': %s\n",
			    svc_name, svc_type, svc_domain,
			    avahi_strerror(avahi_client_errno(avahi_service_resolver_get_client(resolver))));
		break;
	default:
		SPDK_ERRLOG("Unknown Avahi resolver event: %d", resolve_event);
	}
	avahi_service_resolver_free(resolver);   /* [한국어] resolver는 한 번 쓰고 free. */
}

/*
 * [한국어]
 * mdns_browse_handler - Avahi 서비스 브라우저 콜백.
 *
 * @browser_event: NEW(새 서비스), REMOVE, ALL_FOR_NOW, CACHE_EXHAUSTED, FAILURE.
 * @svc_name/svc_type/svc_domain: 광고된 서비스 식별자.
 *
 * 동작:
 *   - NEW: avahi_service_resolver_new로 resolver를 생성. resolver는 mdns_resolve_handler에서 free.
 *   - REMOVE: 자동 정리 미구현 (사용자가 수동으로 stop_discovery 호출 필요).
 *   - ALL_FOR_NOW/CACHE_EXHAUSTED: 정보용 로그만.
 *   - FAILURE: 에러 로그.
 *
 * 실행 컨텍스트: Avahi poller 스레드 (bdev_nvme_avahi_iterate).
 */
static void
mdns_browse_handler(
	AvahiServiceBrowser *browser,
	AvahiIfIndex intf,
	AvahiProtocol avahi_protocol,
	AvahiBrowserEvent browser_event,
	const char *svc_name,
	const char *svc_type,
	const char *svc_domain,
	AVAHI_GCC_UNUSED AvahiLookupResultFlags result_flags,
	void *user_data)
{
	AvahiClient *client = user_data;

	assert(browser);
	/* The handler gets called whenever a new service becomes available
	   or removed from the LAN */
	switch (browser_event) {
	case AVAHI_BROWSER_NEW:
		SPDK_DEBUGLOG(bdev_nvme, "(Browser) NEW: service '%s' of type '%s' in domain '%s'\n", svc_name,
			      svc_type,
			      svc_domain);
		/* We ignore the returned resolver object. In the callback
		   function we free it. If the server is terminated before
		   the callback function is called the server will free
		   the resolver for us. */
		/* [한국어] resolver 생성. 결과는 무시 (콜백에서 free). 실패하면 ERRLOG만. */
		if (!(avahi_service_resolver_new(client, intf, avahi_protocol, svc_name, svc_type, svc_domain,
						 AVAHI_PROTO_UNSPEC, 0,
						 mdns_resolve_handler, client))) {
			SPDK_ERRLOG("Failed to resolve service '%s': %s\n", svc_name,
				    avahi_strerror(avahi_client_errno(client)));
		}
		break;
	case AVAHI_BROWSER_REMOVE:
		SPDK_ERRLOG("(Browser) REMOVE: service '%s' of type '%s' in domain '%s'\n", svc_name, svc_type,
			    svc_domain);
		/* On remove, we are not doing the automatic cleanup of connections
		 * to the targets that were learnt from the CDC, for which remove event has
		 * been received. If required, user can clear the connections manually by
		 * invoking bdev_nvme_stop_discovery. We can implement the automatic cleanup
		 * later, if there is a requirement in the future.
		 */
		/* [한국어] 위 영문 주석 요약: REMOVE 이벤트에 대해 자동 cleanup하지 않음.
		 * 이미 attach된 컨트롤러는 사용자가 stop_discovery로 직접 정리해야 함. */
		break;
	case AVAHI_BROWSER_ALL_FOR_NOW:
	case AVAHI_BROWSER_CACHE_EXHAUSTED:
		/* [한국어] mDNS 캐시 초기 스캔 완료 신호. 정보 로그만. */
		SPDK_INFOLOG(bdev_nvme, "(Browser) %s\n",
			     browser_event == AVAHI_BROWSER_CACHE_EXHAUSTED ? "CACHE_EXHAUSTED" : "ALL_FOR_NOW");
		break;
	case AVAHI_BROWSER_FAILURE:
		SPDK_ERRLOG("(Browser) Failure: %s\n",
			    avahi_strerror(avahi_client_errno(avahi_service_browser_get_client(browser))));
		return;
	default:
		SPDK_ERRLOG("Unknown Avahi browser event: %d", browser_event);
	}
}

/*
 * [한국어]
 * client_handler - Avahi 클라이언트 상태 변경 콜백.
 * 데몬 연결 끊김 등 FAILURE 상태만 로그.
 */
static void
client_handler(AvahiClient *client, AvahiClientState avahi_state, AVAHI_GCC_UNUSED void *user_data)
{
	assert(client);
	/* The handler gets called whenever the client or server state changes */
	if (avahi_state == AVAHI_CLIENT_FAILURE) {
		SPDK_ERRLOG("Server connection failure: %s\n", avahi_strerror(avahi_client_errno(client)));
	}
}

/*
 * [한국어]
 * bdev_nvme_avahi_iterate - SPDK poller 콜백. Avahi 이벤트 루프를 한 번 돌린다.
 *
 * @arg: mdns_discovery_ctx 포인터.
 * @return: SPDK_POLLER_BUSY (실제 일을 했음) / SPDK_POLLER_IDLE.
 *
 * 동작:
 *   - ctx->stop이면 자기 자신 unregister + ctx 해제 (세션 종료).
 *   - g_avahi_simple_poll이 사라졌으면 unregister만 (정상 종료 경로).
 *   - 그 외엔 avahi_simple_poll_iterate(0)로 non-blocking iterate.
 *
 * 100ms 주기 (SPDK_POLLER_REGISTER에서 100*1000us 지정).
 * 실행 컨텍스트: poller가 등록된 SPDK 스레드 (보통 첫 reactor).
 */
static int
bdev_nvme_avahi_iterate(void *arg)
{
	struct mdns_discovery_ctx *ctx = arg;
	int rc;

	if (ctx->stop) {
		/* [한국어] 종료 요청 → 자기 등록 해제 + 전역 리스트에서 제거 + ctx 해제. */
		SPDK_INFOLOG(bdev_nvme, "Stopping avahi poller for service %s\n", ctx->svcname);
		spdk_poller_unregister(&ctx->poller);
		TAILQ_REMOVE(&g_mdns_discovery_ctxs, ctx, tailq);
		free_mdns_discovery_ctx(ctx);
		return SPDK_POLLER_IDLE;
	}

	if (g_avahi_simple_poll == NULL) {
		/* [한국어] Avahi 메인 루프가 사라진 비정상 상태 → 자기 등록 해제. */
		spdk_poller_unregister(&ctx->poller);
		return SPDK_POLLER_IDLE;
	}

	/* [한국어] non-blocking iterate (timeout=0). 이벤트 처리 후 즉시 반환. */
	rc = avahi_simple_poll_iterate(g_avahi_simple_poll, 0);
	if (rc && rc != -EAGAIN) {
		SPDK_ERRLOG("avahi poll returned error for service: %s/n", ctx->svcname);
		return SPDK_POLLER_IDLE;
	}

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * bdev_nvme_start_mdns_discovery - bdev_nvme.h §2 참조. mDNS 디스커버리 세션 시작.
 *
 * 동작:
 *   1) 같은 base_name 또는 svcname의 세션이 이미 있으면 -EEXIST.
 *   2) Avahi simple poll/client가 없으면 lazy 생성 (모든 세션이 공유).
 *   3) avahi_service_browser_new로 svcname을 watch 시작.
 *   4) ctx 할당 + 옵션 사본 + g_mdns_discovery_ctxs 등록.
 *   5) 100ms 주기 poller 등록 (bdev_nvme_avahi_iterate).
 * 실행 컨텍스트: 반드시 app 스레드 (assert로 강제).
 */
int
bdev_nvme_start_mdns_discovery(const char *base_name,
			       const char *svcname,
			       struct spdk_nvme_ctrlr_opts *drv_opts,
			       struct spdk_bdev_nvme_ctrlr_opts *bdev_opts)
{
	AvahiServiceBrowser *sb = NULL;
	int error;
	struct mdns_discovery_ctx *ctx;

	assert(base_name);
	assert(svcname);
	assert(spdk_thread_is_app_thread(NULL));   /* [한국어] app 스레드 강제. */

	/* [한국어] 1단계: 중복 세션 방지. */
	TAILQ_FOREACH(ctx, &g_mdns_discovery_ctxs, tailq) {
		if (strcmp(ctx->name, base_name) == 0) {
			SPDK_ERRLOG("mDNS discovery already running with name %s\n", base_name);
			return -EEXIST;
		}

		if (strcmp(ctx->svcname, svcname) == 0) {
			SPDK_ERRLOG("mDNS discovery already running for service %s\n", svcname);
			return -EEXIST;
		}
	}

	if (g_avahi_simple_poll == NULL) {

		/* Allocate main loop object */
		/* [한국어] 2단계: Avahi 메인 루프 lazy 생성 (모든 세션 공유). */
		if (!(g_avahi_simple_poll = avahi_simple_poll_new())) {
			SPDK_ERRLOG("Failed to create poll object for mDNS discovery for service: %s.\n", svcname);
			return -ENOMEM;
		}
	}

	if (g_avahi_client == NULL) {

		/* Allocate a new client */
		/* [한국어] 3단계: Avahi 데몬 연결 lazy 생성. */
		g_avahi_client = avahi_client_new(avahi_simple_poll_get(g_avahi_simple_poll), 0, client_handler,
						  NULL, &error);
		/* Check whether creating the client object succeeded */
		if (!g_avahi_client) {
			SPDK_ERRLOG("Failed to create mDNS client for service:%s Error: %s\n", svcname,
				    avahi_strerror(error));
			return -ENOMEM;
		}
	}

	/* Create the service browser */
	/* [한국어] 4단계: 서비스 브라우저 생성. svcname을 모든 인터페이스/프로토콜에서 watch. */
	if (!(sb = avahi_service_browser_new(g_avahi_client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, svcname,
					     NULL, 0, mdns_browse_handler, g_avahi_client))) {
		SPDK_ERRLOG("Failed to create service browser for service: %s Error: %s\n", svcname,
			    avahi_strerror(avahi_client_errno(g_avahi_client)));
		return -ENOMEM;
	}

	/* [한국어] 5단계: ctx 할당. */
	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Error creating mDNS discovery ctx for service: %s\n", svcname);
		avahi_service_browser_free(sb);
		return -ENOMEM;
	}

	/* [한국어] 6단계: svcname 사본. */
	ctx->svcname = strdup(svcname);
	if (ctx->svcname == NULL) {
		SPDK_ERRLOG("Error creating mDNS discovery ctx svcname for service: %s\n", svcname);
		free_mdns_discovery_ctx(ctx);
		avahi_service_browser_free(sb);
		return -ENOMEM;
	}
	/* [한국어] 7단계: base_name 사본. */
	ctx->name = strdup(base_name);
	if (ctx->name == NULL) {
		SPDK_ERRLOG("Error creating mDNS discovery ctx name for service: %s\n", svcname);
		free_mdns_discovery_ctx(ctx);
		avahi_service_browser_free(sb);
		return -ENOMEM;
	}
	/* [한국어] 8단계: 옵션 구조체 사본 (entry 만들 때 그대로 사용). */
	memcpy(&ctx->drv_opts, drv_opts, sizeof(*drv_opts));
	memcpy(&ctx->bdev_opts, bdev_opts, sizeof(*bdev_opts));
	ctx->sb = sb;
	TAILQ_INIT(&ctx->mdns_discovery_entry_ctxs);
	/* Even if user did not specify hostnqn, we can still strdup("\0"); */
	/* [한국어] hostnqn은 사용자가 안 줬어도 빈 문자열이라도 strdup해서 일관성 유지. */
	ctx->hostnqn = strdup(ctx->drv_opts.hostnqn);
	if (ctx->hostnqn == NULL) {
		SPDK_ERRLOG("Error creating mDNS discovery ctx hostnqn for service: %s\n", svcname);
		free_mdns_discovery_ctx(ctx);
		return -ENOMEM;
	}
	/* Start the poller for the Avahi client browser */
	/* [한국어] 9단계: 전역 리스트에 등록 + 100ms 주기 poller 시작. */
	TAILQ_INSERT_TAIL(&g_mdns_discovery_ctxs, ctx, tailq);
	ctx->poller = SPDK_POLLER_REGISTER(bdev_nvme_avahi_iterate, ctx, 100 * 1000);
	return 0;
}

/*
 * [한국어]
 * mdns_stop_discovery_entry - 한 mDNS 세션의 모든 entry에 대해 bdev_nvme_stop_discovery 호출.
 *
 * mDNS는 디스커버리 컨트롤러 발견까지만 담당하고, 실제 attach는 bdev_nvme_start_discovery가
 * 별도 세션을 만들어 진행하므로 stop 시점에 그것들도 함께 종료해야 한다.
 */
static void
mdns_stop_discovery_entry(struct mdns_discovery_ctx *ctx)
{
	struct mdns_discovery_entry_ctx *entry_ctx = NULL;

	assert(ctx);

	TAILQ_FOREACH(entry_ctx, &ctx->mdns_discovery_entry_ctxs, tailq) {
		bdev_nvme_stop_discovery(entry_ctx->name, NULL, NULL);
	}
}

/*
 * [한국어]
 * bdev_nvme_stop_mdns_discovery - bdev_nvme.h §2 참조. mDNS 세션 종료 요청.
 *
 * 즉시 해제하지 않고 ctx->stop=true만 설정 → 다음 poller 실행 시 안전하게 정리.
 * Avahi callback이 진행 중일 수 있으므로 비동기 종료가 더 안전.
 */
int
bdev_nvme_stop_mdns_discovery(const char *name)
{
	struct mdns_discovery_ctx *ctx;

	assert(name);
	TAILQ_FOREACH(ctx, &g_mdns_discovery_ctxs, tailq) {
		if (strcmp(name, ctx->name) == 0) {
			if (ctx->stop) {
				return -EALREADY;   /* [한국어] 이미 종료 진행 중. */
			}
			/* set stop to true to stop the mdns poller instance */
			ctx->stop = true;
			mdns_stop_discovery_entry(ctx);   /* [한국어] 자식 entry 디스커버리도 stop. */
			return 0;
		}
	}

	return -ENOENT;
}

/*
 * [한국어]
 * bdev_nvme_get_mdns_discovery_info - bdev_nvme.h §2 참조. RPC 응답으로 mDNS 세션 정보 출력.
 *
 * 출력 JSON: 배열의 각 원소는 {name, svcname, referrals: [{name, trid: {...}}]}.
 */
void
bdev_nvme_get_mdns_discovery_info(struct spdk_jsonrpc_request *request)
{
	struct mdns_discovery_ctx *ctx;
	struct mdns_discovery_entry_ctx *entry_ctx;
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(ctx, &g_mdns_discovery_ctxs, tailq) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "name", ctx->name);
		spdk_json_write_named_string(w, "svcname", ctx->svcname);

		/* [한국어] referrals 배열: 발견된 디스커버리 컨트롤러 목록. */
		spdk_json_write_named_array_begin(w, "referrals");
		TAILQ_FOREACH(entry_ctx, &ctx->mdns_discovery_entry_ctxs, tailq) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "name", entry_ctx->name);
			spdk_json_write_named_object_begin(w, "trid");
			nvme_bdev_dump_trid_json(&entry_ctx->trid, w);   /* [한국어] trid를 표준 형식으로 직렬화. */
			spdk_json_write_object_end(w);
			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);

		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}

/*
 * [한국어]
 * bdev_nvme_mdns_discovery_config_json - save_config 시 활성 mDNS 세션을 JSON으로 출력.
 *
 * SPDK 재시작 시 같은 mDNS 세션을 자동 복원할 수 있도록 method/params 형태로 저장.
 */
void
bdev_nvme_mdns_discovery_config_json(struct spdk_json_write_ctx *w)
{
	struct mdns_discovery_ctx *ctx;

	TAILQ_FOREACH(ctx, &g_mdns_discovery_ctxs, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "method", "bdev_nvme_start_mdns_discovery");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", ctx->name);
		spdk_json_write_named_string(w, "svcname", ctx->svcname);
		spdk_json_write_named_string(w, "hostnqn", ctx->hostnqn);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}
}

#else /* SPDK_CONFIG_AVAHI */

/*
 * [한국어] 아래는 --with-avahi 미빌드 시 컴파일되는 stub 구현부.
 * 모든 함수가 -ENOTSUP을 반환하거나 no-op. RPC 호출자는 빌드 옵션 부재를 인지할 수 있음.
 */

int
bdev_nvme_start_mdns_discovery(const char *base_name,
			       const char *svcname,
			       struct spdk_nvme_ctrlr_opts *drv_opts,
			       struct spdk_bdev_nvme_ctrlr_opts *bdev_opts)
{
	SPDK_ERRLOG("spdk not built with --with-avahi option\n");
	return -ENOTSUP;
}

int
bdev_nvme_stop_mdns_discovery(const char *name)
{
	SPDK_ERRLOG("spdk not built with --with-avahi option\n");
	return -ENOTSUP;
}

void
bdev_nvme_get_mdns_discovery_info(struct spdk_jsonrpc_request *request)
{
	SPDK_ERRLOG("spdk not built with --with-avahi option\n");
	spdk_jsonrpc_send_error_response(request, -ENOTSUP, spdk_strerror(ENOTSUP));
}

void
bdev_nvme_mdns_discovery_config_json(struct spdk_json_write_ctx *w)
{
	/* Empty function to be invoked, when SPDK is built without --with-avahi */
	/* [한국어] save_config가 호출되어도 mDNS 세션 자체가 없으므로 아무것도 안 함. */
}

#endif
