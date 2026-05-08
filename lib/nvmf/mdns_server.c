/*  SPDX-License-Identifier: BSD-3-Clause
 *  Copyright (c) 2022 Dell Inc, or its subsidiaries.
 *  Copyright (c) 2024 Samsung Electronics Co., Ltd. All rights reserved.
 *  All rights reserved.
 */

/*
 * [한국어 설명] NVMe-oF Discovery 서비스의 mDNS publish 구현 (mdns_server.c)
 *
 * === 파일의 역할 ===
 * NVMe-oF 1.1+ 스펙의 "Centralized Discovery Controller (CDC) Pull Registration Request" 또는
 * "분산 mDNS Discovery"를 위해 NVMe-oF target이 자기 자신을 mDNS로 광고(publish)하는 기능.
 * 호스트는 _nvme-disc._tcp.local 서비스를 mDNS로 검색해 사용 가능한 Discovery 컨트롤러를
 * 발견할 수 있다. 본 파일은 Avahi(리눅스 mDNS 라이브러리) 클라이언트를 통해 SPDK target에
 * 등록된 Discovery 서브시스템(SPDK_NVMF_DISCOVERY_NQN)의 listener들을 mDNS 엔트리로 변환해
 * 광고한다. SPDK_CONFIG_AVAHI가 켜진 빌드에서만 컴파일되며, 그렇지 않으면 stubs.c가 대체.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   RPC nvmf_publish_mdns_prr(tgt_name) -> spdk_nvmf_rpc 핸들러
 *     -> nvmf_publish_mdns_prr() (본 파일)
 *        -> avahi_simple_poll_new (Avahi 메인 루프 객체)
 *        -> avahi_client_new (avahi-daemon에 연결)
 *        -> SPDK_POLLER_REGISTER(nvmf_avahi_publish_iterate, 100ms 주기)
 *           -> avahi_simple_poll_iterate (Avahi 이벤트 펌프)
 *              -> publish_client_new_callback (Avahi 상태 변경 알림)
 *                 -> publish_pull_registration_request
 *                    -> avahi_entry_group_add_listeners (서비스 등록)
 * 호스트 측은 mDNS 쿼리(_nvme-disc._tcp.local)로 이 광고를 받아 Discovery Log를 가져오기
 * 위한 Connect를 시작한다.
 * 실행 컨텍스트: target main thread (RPC 핸들러 스레드)에서 모든 함수 실행. SPDK poller가
 * 100ms 주기로 Avahi의 fd 이벤트를 폴링.
 *
 * === 타 모듈과의 연결 ===
 * - libavahi-client/-common: mDNS 등록/광고/이벤트 펌프 라이브러리 (외부)
 * - SPDK_NVMF_DISCOVERY_NQN: Discovery 서브시스템의 표준 NQN ("nqn.2014-08.org.nvmexpress.discovery")
 * - spdk_nvmf_subsystem_listener: 광고할 listener 정보 (trtype/traddr/trsvcid)
 * - spdk_poller: SPDK 스레드의 주기 콜백 (Avahi fd 펌프 역할)
 * 데이터 흐름: subsystem->listeners 순회 -> 각 listener의 trtype/trsvcid를 Avahi service entry로
 * 변환 -> avahi-daemon이 NIC를 통해 mDNS 멀티캐스트로 광고.
 * 공유 자료구조:
 *   g_avahi_publish_simple_poll, g_avahi_publish_client, g_avahi_entry_group, g_mdns_publish_ctx
 *   -- 모두 단일 main thread 가정의 전역 (한 번에 하나의 target만 publish 가능).
 *
 * === 주요 함수/구조체 요약 ===
 * - nvmf_publish_mdns_prr: mDNS publish 시작 (RPC 진입점) - Avahi 클라이언트/poller 생성
 * - nvmf_tgt_stop_mdns_prr: mDNS publish 중단 - poller 해제 및 Avahi 자원 정리
 * - nvmf_tgt_update_mdns_prr: listener 변경 시 엔트리 reset 후 재등록
 * - nvmf_avahi_publish_iterate: SPDK poller 콜백 - Avahi 이벤트 한 번 펌프
 * - publish_client_new_callback: Avahi 상태 머신 변경 콜백 (RUNNING/FAILURE/COLLISION)
 * - avahi_entry_group_add_listeners: subsystem listener -> Avahi service entry 변환 핵심
 * - struct mdns_publish_ctx: publish 세션 컨텍스트 (poller + subsystem + tgt)
 */

#include "spdk/stdinc.h"                            /* [한국어] 표준 라이브러리 (string.h, stdlib.h 등) */
#include "spdk/env.h"                               /* [한국어] DPDK 추상화 - 메모리/스레드 환경 */
#include "spdk/thread.h"                            /* [한국어] spdk_poller, spdk_thread API - 주기 콜백 등록에 사용 */
#include "nvmf_internal.h"                          /* [한국어] spdk_nvmf_tgt/subsystem/listener 내부 타입 */
#include "spdk/log.h"                               /* [한국어] SPDK_ERRLOG/SPDK_INFOLOG */
#include "spdk/config.h"                            /* [한국어] SPDK_CONFIG_AVAHI 매크로 - Avahi 빌드 분기 */
#include "spdk/nvme.h"                              /* [한국어] SPDK_NVME_TRANSPORT_TCP/RDMA 등 트랜스포트 종류 enum */
#include "spdk/string.h"                            /* [한국어] spdk_strtol 등 문자열 헬퍼 */

#ifdef SPDK_CONFIG_AVAHI                            /* [한국어] Avahi 라이브러리 사용 빌드에서만 본 구현 컴파일 */
#include <avahi-client/client.h>                    /* [한국어] AvahiClient 핵심 객체 - avahi-daemon과의 IPC 클라이언트 */
#include <avahi-client/publish.h>                   /* [한국어] AvahiEntryGroup, avahi_entry_group_add_service_strlst 등 - 서비스 등록 API */
#include <avahi-client/lookup.h>                    /* [한국어] (현재는 publish만 사용 - 향후 lookup 확장 대비) */
#include <avahi-common/simple-watch.h>              /* [한국어] AvahiSimplePoll - 단일 스레드용 메인 루프 (SPDK poller로 펌프) */
#include <avahi-common/malloc.h>                    /* [한국어] avahi_malloc/free - Avahi 내부 할당자 */
#include <avahi-common/error.h>                     /* [한국어] avahi_strerror, AVAHI_ERR_* 코드 - 에러 메시지 변환 */

#define NVMF_MAX_DNS_NAME_LENGTH 255                /* [한국어] DNS 레이블 최대 길이 (RFC 1035 SECTION 2.3.4) - 서비스 이름 버퍼 크기 */

static AvahiSimplePoll *g_avahi_publish_simple_poll = NULL;
/* [한국어] Avahi 단일 스레드 메인 루프 객체.
 * 설정자: nvmf_publish_mdns_prr()에서 avahi_simple_poll_new()로 1회 생성.
 * 읽는 자: nvmf_avahi_publish_iterate()에서 매 100ms 펌프, avahi_client_new에서 fd 등록.
 * 값 범위: NULL이면 미초기화/해제됨 - 유효한 publish 세션이 없는 상태.
 * 동기화: target main thread에서만 접근 가정 - 별도 락 없음. */

static AvahiClient *g_avahi_publish_client = NULL;
/* [한국어] avahi-daemon과의 IPC 클라이언트 핸들.
 * 설정자: nvmf_publish_mdns_prr()에서 avahi_client_new()로 생성.
 * 읽는 자: publish_pull_registration_request()(entry group을 client에 바인딩),
 *         publish_client_new_callback()(상태 변경 통지).
 * 값 범위: NULL이면 미연결/해제됨, 비-NULL이면 daemon 연결 핸들.
 * 동기화: main thread 단일 사용. */

static AvahiEntryGroup *g_avahi_entry_group = NULL;
/* [한국어] mDNS 서비스 엔트리들의 그룹 (atomic commit 단위).
 * 설정자: publish_pull_registration_request()에서 avahi_entry_group_new로 생성.
 * 읽는 자: avahi_entry_group_add_listeners()(엔트리 추가),
 *         nvmf_tgt_update_mdns_prr()(reset 후 재추가),
 *         nvmf_avahi_publish_destroy()(해제).
 * 값 범위: NULL이면 그룹 미생성/해제, 비-NULL이면 활성 그룹.
 * 동기화: main thread 단일 사용. */

struct mdns_publish_ctx {
	struct spdk_poller		*poller;
	/* [한국어] Avahi 이벤트를 100ms마다 펌프하는 SPDK poller 핸들.
	 * 설정자: nvmf_publish_mdns_prr() 마지막에 SPDK_POLLER_REGISTER로 등록.
	 * 읽는 자: nvmf_ctx_stop_mdns_prr()에서 spdk_poller_unregister.
	 * 값 범위: 유효 핸들 - NULL이면 등록 실패.
	 * 동기화: poller 콜백은 등록 스레드(main)에서만 실행되므로 별도 락 없음. */

	struct spdk_nvmf_subsystem	*subsystem;
	/* [한국어] 광고 대상 Discovery 서브시스템 (SPDK_NVMF_DISCOVERY_NQN).
	 * 설정자: nvmf_publish_mdns_prr()에서 spdk_nvmf_tgt_find_subsystem 결과 저장.
	 * 읽는 자: avahi_entry_group_add_listeners()(listener 순회 시 subsystem->listeners 사용).
	 * 값 범위: 유효한 spdk_nvmf_subsystem 포인터 (Discovery 타입).
	 * 동기화: subsystem->listeners 순회 시 main thread 일관성에 의존. */

	struct spdk_nvmf_tgt		*tgt;
	/* [한국어] 이 publish 세션이 속한 NVMe-oF target.
	 * 설정자: nvmf_publish_mdns_prr() 호출자가 전달한 tgt.
	 * 읽는 자: nvmf_tgt_is_mdns_running()(같은 tgt에 대한 중복 publish 검사).
	 * 값 범위: 유효한 spdk_nvmf_tgt 포인터.
	 * 동기화: tgt 자체는 다른 락으로 보호되나 본 필드는 read-only로 사용. */
};

static struct mdns_publish_ctx *g_mdns_publish_ctx = NULL;
/* [한국어] 현재 활성 mDNS publish 세션 (전역 - 한 번에 하나만 허용).
 * 설정자: nvmf_publish_mdns_prr() 끝에서 publish_ctx 저장.
 * 읽는 자: nvmf_tgt_is_mdns_running(), nvmf_tgt_update_mdns_prr().
 * 값 범위: NULL이면 publish 미수행, 비-NULL이면 활성 세션.
 * 동기화: main thread 단일 사용 가정. */

/*
 * [한국어]
 * nvmf_avahi_publish_destroy - Avahi 자원 일괄 해제 (역순)
 *
 * @ctx: 해제할 publish 컨텍스트 (이 함수 끝에서 free)
 *
 * Avahi 자원 의존성: entry_group ⊂ client ⊂ simple_poll. 따라서 entry_group → client → simple_poll
 * 순으로 해제해야 안전하다. 각 자원은 NULL 체크 후 해제 + 전역 NULL 초기화로 idempotent하게 만든다.
 * 마지막에 g_mdns_publish_ctx도 NULL로 만들어 새로운 publish가 가능하도록 한다.
 * 실행 컨텍스트: main thread (RPC 핸들러 또는 poller 에러 경로).
 *
 * 호출 체인:
 *   nvmf_publish_mdns_prr(실패 경로) / nvmf_ctx_stop_mdns_prr -> [본 함수] -> avahi_*_free
 */
static void
nvmf_avahi_publish_destroy(struct mdns_publish_ctx *ctx)
{
	if (g_avahi_entry_group) {                  /* [한국어] entry group이 생성되어 있으면 먼저 해제 - 의존성 역순 */
		avahi_entry_group_free(g_avahi_entry_group); /* [한국어] mDNS 등록된 서비스 엔트리들을 즉시 withdraw + 메모리 해제 */
		g_avahi_entry_group = NULL;         /* [한국어] 전역 NULL 초기화 - 이중 해제 방지 */
	}

	if (g_avahi_publish_client) {               /* [한국어] avahi-daemon 연결 핸들이 살아 있으면 해제 */
		avahi_client_free(g_avahi_publish_client); /* [한국어] daemon과의 D-Bus 연결 종료 + 내부 자원 해제 */
		g_avahi_publish_client = NULL;      /* [한국어] 전역 NULL 초기화 */
	}

	if (g_avahi_publish_simple_poll) {          /* [한국어] 메인 루프 객체 마지막에 해제 - client가 이 fd에 의존했었음 */
		avahi_simple_poll_free(g_avahi_publish_simple_poll); /* [한국어] watch fd, timeout 자원 해제 */
		g_avahi_publish_simple_poll = NULL; /* [한국어] 전역 NULL 초기화 */
	}

	g_mdns_publish_ctx = NULL;                  /* [한국어] 활성 세션 표지 제거 - 새로운 publish 허용 */
	free(ctx);                                  /* [한국어] calloc으로 할당된 컨텍스트 본체 해제 */
}

/*
 * [한국어]
 * nvmf_avahi_publish_iterate - SPDK poller 콜백으로 Avahi 이벤트 한 번 펌프
 *
 * @arg: mdns_publish_ctx 포인터 (SPDK_POLLER_REGISTER 등록 시 전달된 인자)
 * @return: SPDK_POLLER_BUSY (이벤트 처리 여부 무관 - 단순화)
 *
 * Avahi의 메인 루프(avahi_simple_poll)는 외부 이벤트 루프와 통합 가능하도록 "한 번 펌프" 인터페이스를
 * 제공한다. SPDK는 자체 reactor에서 Avahi의 이벤트 루프를 별도 스레드 없이 100ms마다 호출해
 * mDNS 패킷 처리/타이머 진행을 시킨다 (timeout=0이므로 non-blocking).
 * -EAGAIN이 아닌 음수 반환은 메인 루프 종료 신호로 간주, poller 해제 후 정리.
 * 실행 컨텍스트: 등록한 spdk_thread (main thread).
 *
 * 호출 체인:
 *   reactor -> spdk_poller cb (100ms 주기) -> [본 함수] -> avahi_simple_poll_iterate
 */
static int
nvmf_avahi_publish_iterate(void *arg)
{
	struct mdns_publish_ctx *ctx = arg;         /* [한국어] poller 등록 시 넘긴 컨텍스트 복원 - 콜백 전용 인자 */
	int rc;                                     /* [한국어] avahi_simple_poll_iterate 반환값 */

	if (ctx == NULL) {                          /* [한국어] 방어적 체크 - 정상 흐름에서는 NULL이 올 수 없음 */
		assert(false);                      /* [한국어] 디버그 빌드에서 즉시 실패시켜 버그 노출 */
		return SPDK_POLLER_IDLE;            /* [한국어] 릴리스 빌드에서는 idle로 폴백 (poller는 계속 동작) */
	}

	rc = avahi_simple_poll_iterate(g_avahi_publish_simple_poll, 0); /* [한국어] non-blocking(timeout=0) Avahi 이벤트 펌프 - mDNS 패킷/타이머 진행 */
	if (rc && rc != -EAGAIN) {                  /* [한국어] -EAGAIN은 정상(이벤트 없음) - 그 외 음수는 루프 종료 요청 */
		SPDK_ERRLOG("avahi publish poll returned error\n"); /* [한국어] Avahi 내부 에러 로깅 */
		spdk_poller_unregister(&ctx->poller); /* [한국어] poller 자체를 reactor에서 해제 - 더 이상 호출 안 됨 */
		nvmf_avahi_publish_destroy(ctx);    /* [한국어] Avahi 자원과 컨텍스트 일괄 해제 */
		return SPDK_POLLER_BUSY;            /* [한국어] 한 번의 작업(에러 처리)을 했음을 알림 */
	}

	return SPDK_POLLER_BUSY;                    /* [한국어] 작업 시도했음을 reactor 통계에 알림 (실제로 idle이어도 BUSY로 단순화) */
}

/*
 * [한국어]
 * nvmf_ctx_stop_mdns_prr - publish 컨텍스트 중지 (poller 해제 + 자원 정리)
 *
 * @ctx: 중지할 컨텍스트
 *
 * nvmf_tgt_stop_mdns_prr 또는 콜백 에러 경로에서 호출되어 poller를 해제하고 Avahi 자원을 정리한다.
 * 실행 컨텍스트: main thread.
 */
static void
nvmf_ctx_stop_mdns_prr(struct mdns_publish_ctx *ctx)
{
	SPDK_INFOLOG(nvmf, "Stopping avahi publish poller\n"); /* [한국어] 정상 종료 알림 - 디버그 추적 도움 */
	spdk_poller_unregister(&ctx->poller);       /* [한국어] reactor에서 100ms poller 등록 해제 - poller 핸들이 NULL로 됨 */
	nvmf_avahi_publish_destroy(ctx);            /* [한국어] Avahi 자원 + 컨텍스트 메모리 해제 */
}

/*
 * [한국어]
 * nvmf_tgt_is_mdns_running - 특정 target에 대한 mDNS publish가 활성인지 검사
 *
 * @tgt: 검사할 target
 * @return: true면 해당 tgt에 대한 publish가 진행 중
 *
 * 단일 전역 g_mdns_publish_ctx만 사용하므로 tgt가 일치할 때만 true.
 * 실행 컨텍스트: main thread.
 */
static bool
nvmf_tgt_is_mdns_running(struct spdk_nvmf_tgt *tgt)
{
	if (g_mdns_publish_ctx && g_mdns_publish_ctx->tgt == tgt) { /* [한국어] 활성 세션이 있고 + 같은 tgt에 대한 것인지 확인 */
		return true;                        /* [한국어] 이 target은 현재 mDNS publish 중 */
	}
	return false;                               /* [한국어] 미수행 또는 다른 tgt에 대한 publish */
}

/*
 * [한국어]
 * nvmf_tgt_stop_mdns_prr - 외부 진입점: target에 대한 mDNS publish 중지
 *
 * @tgt: 중지할 target
 *
 * RPC 또는 spdk_nvmf_tgt_destroy 경로에서 호출. 활성이면 정리, 아니면 no-op.
 * 실행 컨텍스트: main thread.
 *
 * 호출 체인:
 *   RPC nvmf_publish_mdns_prr stop / tgt destroy -> [본 함수] -> nvmf_ctx_stop_mdns_prr
 */
void
nvmf_tgt_stop_mdns_prr(struct spdk_nvmf_tgt *tgt)
{
	if (nvmf_tgt_is_mdns_running(tgt) == true) { /* [한국어] 활성 publish인지 확인 후 정리 - 아니면 no-op */
		nvmf_ctx_stop_mdns_prr(g_mdns_publish_ctx); /* [한국어] 활성 컨텍스트로 정리 위임 */
		return;                             /* [한국어] 정리 후 명시적 반환 - 가독성 */
	}
}

/*
 * [한국어]
 * avahi_entry_group_add_listeners - subsystem의 listener들을 Avahi service entry로 변환·등록
 *
 * @avahi_entry_group: 추가할 entry group (commit 단위)
 * @subsystem: Discovery 서브시스템 (listener TAILQ 보유)
 *
 * 각 listener에 대해:
 *   - 비활성(active false)이거나, 지원하지 않는 trtype은 skip
 *   - TCP/RDMA 중 TCP만 정확히 지원 (현재 SPDK는 RoCE/iWARP를 trtype에서 구분 불가하므로 RDMA 광고는 skip)
 *   - service name = "spdk0", "spdk1", ... (id 카운터로 구분)
 *   - service type = "_nvme-disc._tcp" (mDNS-NVMe 표준)
 *   - TXT record: "p=tcp", "nqn=<discovery NQN>" (호스트가 어떤 프로토콜/타깃인지 식별)
 * 모든 항목 추가 후 entry_group_commit으로 mDNS에 atomic 게시.
 * 실행 컨텍스트: main thread (Avahi 콜백 또는 update 경로에서 호출).
 *
 * 호출 체인:
 *   publish_pull_registration_request / nvmf_tgt_update_mdns_prr -> [본 함수]
 *     -> avahi_entry_group_add_service_strlst -> avahi_entry_group_commit
 */
static void
avahi_entry_group_add_listeners(AvahiEntryGroup *avahi_entry_group,
				struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_listener *listener; /* [한국어] TAILQ 순회용 임시 - 한 번에 하나의 listener를 가리킴 */
	const char *name_base = "spdk";             /* [한국어] mDNS 서비스 이름 prefix - "spdk0", "spdk1" 등으로 확장됨 */
	const char *type_base = "_nvme-disc";       /* [한국어] mDNS 서비스 타입 prefix - 표준 NVMe discovery 서비스 식별자 */
	const char *domain = "local";               /* [한국어] mDNS 도메인 - link-local 멀티캐스트 도메인 */
	char *protocol;                             /* [한국어] 프로토콜 문자열 ("tcp" 등) - trtype에 따라 결정 */
	char name[NVMF_MAX_DNS_NAME_LENGTH];        /* [한국어] 서비스 이름 버퍼 - "spdk0" 등 */
	char type[NVMF_MAX_DNS_NAME_LENGTH];        /* [한국어] 서비스 타입 버퍼 - "_nvme-disc._tcp" 등 */
	char txt_protocol[NVMF_MAX_DNS_NAME_LENGTH]; /* [한국어] TXT record "p=<proto>" */
	char txt_nqn[NVMF_MAX_DNS_NAME_LENGTH];     /* [한국어] TXT record "nqn=<discovery nqn>" */
	AvahiStringList *txt = NULL;                /* [한국어] Avahi가 사용하는 TXT 레코드 연결 리스트 - 매 listener마다 빌드/해제 */
	uint16_t port;                              /* [한국어] mDNS 광고 포트 - listener trsvcid에서 파싱 */
	uint16_t id = 0;                            /* [한국어] 서비스 이름 suffix 카운터 - 등록 가능한 listener마다 1씩 증가 */

	TAILQ_FOREACH(listener, &subsystem->listeners, link) { /* [한국어] Discovery 서브시스템에 등록된 모든 listener 순회 */
		if (!nvmf_subsystem_listener_is_active(listener)) { /* [한국어] 비활성 listener는 건너뜀 - listen 콜백 미완료 등 */
			continue;
		}

		if (listener->trid->trtype == SPDK_NVME_TRANSPORT_TCP) { /* [한국어] TCP transport - mDNS PRR 표준 지원 */
			protocol = "tcp";           /* [한국어] _nvme-disc._tcp 형태로 광고 */
		} else if (listener->trid->trtype == SPDK_NVME_TRANSPORT_RDMA) { /* [한국어] RDMA - RoCE(udp 위)/iWARP(tcp 위) 구분 정보 부재로 skip */
			SPDK_ERRLOG("Current SPDK doesn't distinguish RoCE(udp) and iWARP(tcp). Skip adding listener id %d to avahi entry",
				    listener->id);  /* [한국어] 사용자에게 RDMA listener는 광고 안 됨을 알림 */
			continue;
		} else {                            /* [한국어] FC/vfio_user 등 mDNS PRR 미지원 트랜스포트 */
			SPDK_ERRLOG("mDNS PRR does not support trtype %d", listener->trid->trtype); /* [한국어] 미지원 trtype 로깅 */
			continue;
		}

		snprintf(type, sizeof(type), "%s._%s", type_base, protocol); /* [한국어] "_nvme-disc._tcp" 등 mDNS 서비스 타입 조립 */
		snprintf(name, sizeof(name), "%s%d", name_base, id++); /* [한국어] 고유 인스턴스 이름 "spdk<n>" - id 후증가로 listener마다 unique */
		snprintf(txt_protocol, sizeof(txt_protocol), "p=%s", protocol); /* [한국어] TXT 키-값 "p=tcp" - 호스트가 프로토콜 즉시 식별 */
		snprintf(txt_nqn, sizeof(txt_nqn), "nqn=%s", SPDK_NVMF_DISCOVERY_NQN); /* [한국어] TXT "nqn=<discovery NQN>" - 표준 Discovery NQN 명시 */
		txt = avahi_string_list_add(txt, txt_protocol); /* [한국어] TXT 리스트에 protocol 항목 prepend */
		txt = avahi_string_list_add(txt, txt_nqn); /* [한국어] TXT 리스트에 nqn 항목 prepend */
		port = spdk_strtol(listener->trid->trsvcid, 10); /* [한국어] trsvcid 문자열("4420" 등)을 10진수 정수 포트로 파싱 */

		if (avahi_entry_group_add_service_strlst(avahi_entry_group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
				0, name, type, domain, NULL, port, txt) < 0) { /* [한국어] mDNS 서비스 엔트리 등록 - IF_UNSPEC=모든 인터페이스, PROTO_UNSPEC=v4/v6 모두 */
			SPDK_ERRLOG("Failed to add avahi service name: %s, type: %s, domain: %s, port: %d",
				    name, type, domain, port); /* [한국어] 등록 실패 시 어떤 엔트리가 실패했는지 명시 */
		}
		avahi_string_list_free(txt);        /* [한국어] add_service_strlst가 내부 복사하므로 호출 후 해제 안전 */
		txt = NULL;                         /* [한국어] 다음 루프에서 새로 빌드하기 위해 NULL 초기화 */
	}

	avahi_entry_group_commit(avahi_entry_group); /* [한국어] 모든 엔트리를 atomic하게 mDNS에 게시 - commit 후 호스트가 검색 가능 */
}

/*
 * [한국어]
 * nvmf_tgt_update_mdns_prr - listener 추가/삭제 시 mDNS 엔트리 갱신
 *
 * @tgt: 갱신 대상 target
 * @return: 0 성공 (또는 publish 비활성 상태로 no-op), -EINVAL 실패
 *
 * subsystem의 listener가 변경되면(add/remove) entry group을 reset 후 다시 add_listeners를 호출해
 * 광고를 새로고침한다. mDNS PRR이 활성화되어 있지 않으면 0으로 조용히 반환.
 * 실행 컨텍스트: main thread (subsystem listener 변경 콜백 경로).
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_add/remove_listener -> [본 함수] -> avahi_entry_group_reset
 *     -> avahi_entry_group_add_listeners
 */
int
nvmf_tgt_update_mdns_prr(struct spdk_nvmf_tgt *tgt)
{
	int rc;                                     /* [한국어] avahi_entry_group_reset 반환 코드 */

	if (nvmf_tgt_is_mdns_running(tgt) == false || g_avahi_entry_group == NULL) { /* [한국어] publish 비활성이면 갱신 자체 의미 없음 - 0 반환 */
		SPDK_INFOLOG(nvmf,
			     "nvmf_tgt_update_mdns_prr is only supported when mDNS servier is running on target\n"); /* [한국어] 사용자 안내 (오타: "servier"는 원본 유지) */
		return 0;                           /* [한국어] 갱신 불필요는 에러가 아님 - 호출자(listener add 경로)가 실패로 인식하지 않도록 */
	}

	rc = avahi_entry_group_reset(g_avahi_entry_group); /* [한국어] 기존 엔트리 모두 제거 (mDNS withdraw + 내부 정리) */
	if (rc) {                                   /* [한국어] reset 실패 - Avahi 내부 상태 이상 */
		SPDK_ERRLOG("Failed to reset avahi_entry_group"); /* [한국어] 실패 로깅 */
		return -EINVAL;                     /* [한국어] 호출자에게 갱신 실패 알림 */
	}

	avahi_entry_group_add_listeners(g_avahi_entry_group, g_mdns_publish_ctx->subsystem); /* [한국어] 현재 listener 상태로 엔트리 재구성 + commit */

	return 0;                                   /* [한국어] 갱신 성공 */
}

/*
 * [한국어]
 * publish_pull_registration_request - Avahi 클라이언트 RUNNING 상태에서 entry group 생성·등록
 *
 * @client: 활성 AvahiClient
 * @publish_ctx: publish 세션 컨텍스트
 * @return: 0 성공, -1 entry_group 생성 실패
 *
 * 클라이언트가 처음 RUNNING이 되었을 때 단 한 번 entry group을 만들고 listener들을 등록한다.
 * 이후 RUNNING 상태가 다시 트리거되어도 g_avahi_entry_group이 비-NULL이면 중복 등록 회피.
 * 실행 컨텍스트: Avahi 콜백 (main thread, poller iterate 안에서).
 */
static int
publish_pull_registration_request(AvahiClient *client, struct mdns_publish_ctx *publish_ctx)
{
	struct spdk_nvmf_subsystem *subsystem = publish_ctx->subsystem; /* [한국어] 광고 대상 Discovery 서브시스템 - 컨텍스트에서 추출 */

	if (g_avahi_entry_group != NULL) {          /* [한국어] 이미 등록된 그룹이 있으면 (재진입 시) 중복 등록 회피 */
		return 0;                           /* [한국어] no-op 성공 */
	}

	g_avahi_entry_group = avahi_entry_group_new(client, NULL, NULL); /* [한국어] 새 entry group 생성 (콜백 NULL = 상태 변경 무시), client 소유 */
	if (g_avahi_entry_group == NULL) {          /* [한국어] 메모리 부족 또는 클라이언트 상태 이상 */
		SPDK_ERRLOG("avahi_entry_group_new failure: %s\n", avahi_strerror(avahi_client_errno(client))); /* [한국어] Avahi 에러 코드를 문자열로 변환해 로깅 */
		return -1;                          /* [한국어] 호출자(콜백)가 publish 컨텍스트 정리하도록 신호 */
	}

	avahi_entry_group_add_listeners(g_avahi_entry_group, subsystem); /* [한국어] 처음으로 listener들을 mDNS에 등록 + commit */

	return 0;                                   /* [한국어] 성공 */
}

/*
 * [한국어]
 * publish_client_new_callback - Avahi 클라이언트 상태 머신 변경 콜백
 *
 * @client: 상태가 변한 AvahiClient
 * @avahi_state: 새 상태 (S_RUNNING/CONNECTING/S_REGISTERING/FAILURE/S_COLLISION)
 * @user_data: 등록 시 전달한 mdns_publish_ctx
 *
 * avahi_client_new에 등록되어 daemon 연결/등록 상태가 바뀔 때마다 호출된다.
 * - S_RUNNING: 데몬 연결 + 호스트네임 등록 완료 → 서비스 entry를 생성
 * - CONNECTING: 데몬과 연결 시도 중 (info 로깅)
 * - S_REGISTERING: 호스트네임 등록 중 (info 로깅)
 * - FAILURE/S_COLLISION: 복구 불가 → publish 세션 종료
 * 실행 컨텍스트: avahi_simple_poll_iterate 안에서 호출됨 (main thread).
 */
static void
publish_client_new_callback(AvahiClient *client, AvahiClientState avahi_state,
			    AVAHI_GCC_UNUSED void *user_data)
{
	int rc;                                     /* [한국어] publish_pull_registration_request 반환 */
	struct mdns_publish_ctx *publish_ctx = user_data; /* [한국어] 콜백 user_data로 전달된 publish 컨텍스트 복원 */

	switch (avahi_state) {                      /* [한국어] Avahi 클라이언트 상태별 분기 */
	case AVAHI_CLIENT_S_RUNNING:                /* [한국어] 데몬 연결 + 호스트네임 등록 완료 - 서비스 등록 가능 시점 */
		rc = publish_pull_registration_request(client, publish_ctx); /* [한국어] entry group 생성 및 listener 등록 시도 */
		if (rc) {                           /* [한국어] entry group 생성 실패 시 publish 세션 종료 */
			nvmf_ctx_stop_mdns_prr(publish_ctx); /* [한국어] poller 해제 + Avahi 자원 정리 */
		}
		break;
	case AVAHI_CLIENT_CONNECTING:               /* [한국어] avahi-daemon이 아직 미기동/연결 진행 중 */
		SPDK_INFOLOG(nvmf, "Avahi client waiting for avahi-daemon"); /* [한국어] 진행 정보 로깅 - 에러 아님 */
		break;
	case AVAHI_CLIENT_S_REGISTERING:            /* [한국어] 호스트네임을 mDNS에 등록하는 중 */
		SPDK_INFOLOG(nvmf, "Avahi client registering service"); /* [한국어] 진행 정보 */
		break;
	case AVAHI_CLIENT_FAILURE:                  /* [한국어] 데몬 연결 끊어짐 등 복구 불가 에러 */
		SPDK_ERRLOG("Server connection failure: %s\n", avahi_strerror(avahi_client_errno(client))); /* [한국어] Avahi 에러 사유 로깅 */
		nvmf_ctx_stop_mdns_prr(publish_ctx); /* [한국어] publish 세션 종료 */
		break;
	case AVAHI_CLIENT_S_COLLISION:              /* [한국어] 호스트네임 충돌 - 같은 이름의 다른 노드가 이미 mDNS에 존재 */
		SPDK_ERRLOG("Avahi client name is already used in the mDNS"); /* [한국어] 충돌 사실 로깅 */
		nvmf_ctx_stop_mdns_prr(publish_ctx); /* [한국어] publish 세션 종료 - 이름 변경 후 재시도 필요 */
		break;
	default:                                    /* [한국어] 새 Avahi 버전에서 추가된 상태 등 미처리 케이스 */
		SPDK_ERRLOG("Avahi client is in unsupported state"); /* [한국어] 디버그 단서 로깅 */
		break;
	}
}

/*
 * [한국어]
 * nvmf_publish_mdns_prr - mDNS publish 시작 (외부 진입점)
 *
 * @tgt: publish할 NVMe-oF target (Discovery 서브시스템 보유 필수)
 * @return: 0 성공, -EEXIST 같은 tgt 이미 publish 중, -EINVAL Discovery listener 부재,
 *          -ENOMEM Avahi 객체 생성 실패
 *
 * 단계:
 *   1) 이미 다른 publish 세션이 있으면 거부 (단일 세션 정책)
 *   2) Discovery 서브시스템 검색 + listener 존재 확인
 *   3) publish_ctx 할당 + Avahi simple_poll/client 생성 (콜백 등록)
 *   4) SPDK_POLLER_REGISTER로 100ms 주기 펌프 poller 등록
 * 실패 시 부분 자원을 즉시 해제하고 적절한 errno 반환.
 * 실행 컨텍스트: main thread (RPC 핸들러).
 *
 * 호출 체인:
 *   RPC nvmf_publish_mdns_prr -> [본 함수] -> avahi_simple_poll_new -> avahi_client_new
 *     -> SPDK_POLLER_REGISTER(nvmf_avahi_publish_iterate)
 */
int
nvmf_publish_mdns_prr(struct spdk_nvmf_tgt *tgt)
{
	int error;                                  /* [한국어] avahi_client_new가 출력으로 채우는 에러 코드 */
	struct mdns_publish_ctx *publish_ctx = NULL; /* [한국어] 새로 만들 publish 컨텍스트 */
	struct spdk_nvmf_subsystem *subsystem = NULL; /* [한국어] 검색해 올 Discovery 서브시스템 */

	if (g_mdns_publish_ctx != NULL) {           /* [한국어] 이미 활성 publish가 있는 경우 - 단일 세션 정책 */
		if (g_mdns_publish_ctx->tgt == tgt) { /* [한국어] 같은 target에 대해 중복 호출 - EEXIST 반환 */
			SPDK_ERRLOG("mDNS server is already running on target %s.\n", tgt->name); /* [한국어] 중복 시작 사실 로깅 */
			return -EEXIST;
		}
		SPDK_ERRLOG("mDNS server does not support publishing multiple targets simultaneously."); /* [한국어] 다른 target 동시 publish 미지원 알림 */
		return -EINVAL;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, SPDK_NVMF_DISCOVERY_NQN); /* [한국어] target에서 Discovery 서브시스템(SPDK_NVMF_DISCOVERY_NQN)을 검색 */
	if (TAILQ_EMPTY(&subsystem->listeners)) {   /* [한국어] Discovery에 listener가 없으면 광고할 것이 없음 */
		SPDK_ERRLOG("Discovery subsystem has no listeners.\n"); /* [한국어] 사용자에게 listener 추가 유도 메시지 */
		return -EINVAL;
	}

	publish_ctx = calloc(1, sizeof(*publish_ctx)); /* [한국어] 컨텍스트 0으로 초기화 할당 - poller/subsystem/tgt 슬롯 */
	if (publish_ctx == NULL) {                  /* [한국어] 메모리 부족 */
		SPDK_ERRLOG("Error creating mDNS publish ctx\n"); /* [한국어] 할당 실패 로깅 */
		return -ENOMEM;
	}
	publish_ctx->subsystem = subsystem;         /* [한국어] 광고 대상 서브시스템 저장 */
	publish_ctx->tgt = tgt;                     /* [한국어] 소속 target 저장 (중복 검사용) */
	/* Allocate main loop object */
	g_avahi_publish_simple_poll = avahi_simple_poll_new(); /* [한국어] Avahi 단일 스레드 메인 루프 객체 생성 - SPDK poller가 이걸 펌프 */
	if (g_avahi_publish_simple_poll == NULL) {  /* [한국어] 메모리 부족 또는 시스템 리소스 부족 */
		SPDK_ERRLOG("Failed to create poll object for mDNS publish.\n"); /* [한국어] 실패 사유 로깅 */
		nvmf_avahi_publish_destroy(publish_ctx); /* [한국어] 부분 자원 해제 (publish_ctx free 포함) */
		return -ENOMEM;
	}

	assert(g_avahi_publish_client == NULL);     /* [한국어] 클라이언트가 NULL 상태에서 진입했는지 확인 - 이전 정리 누락 검출 */

	/* Allocate a new client */
	g_avahi_publish_client = avahi_client_new(avahi_simple_poll_get(g_avahi_publish_simple_poll),
				 0, publish_client_new_callback, publish_ctx, &error); /* [한국어] avahi-daemon에 D-Bus 연결 + 상태 콜백 등록. 0=flags, error=출력 코드 */
	/* Check whether creating the client object succeeded */
	if (g_avahi_publish_client == NULL) {       /* [한국어] daemon 부재/IPC 실패/메모리 부족 등 */
		SPDK_ERRLOG("Failed to create mDNS client Error: %s\n", avahi_strerror(error)); /* [한국어] Avahi 에러 코드 문자열로 변환해 로깅 */
		nvmf_avahi_publish_destroy(publish_ctx); /* [한국어] simple_poll까지 만들었으므로 일괄 정리 */
		return -ENOMEM;
	}

	g_mdns_publish_ctx = publish_ctx;           /* [한국어] 활성 세션으로 등록 - 이후 stop/update 함수가 참조 */
	publish_ctx->poller = SPDK_POLLER_REGISTER(nvmf_avahi_publish_iterate, publish_ctx, 100 * 1000); /* [한국어] 100ms(=100*1000us) 주기 poller 등록 - Avahi 이벤트 펌프 시작 */
	return 0;                                   /* [한국어] publish 시작 성공 - 실제 등록은 클라이언트 RUNNING 콜백에서 비동기 진행 */
}
#endif                                              /* [한국어] SPDK_CONFIG_AVAHI 블록 끝 */
