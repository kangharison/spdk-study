/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe over Fabrics discovery service
 */

/*
 * [한국어 설명] NVMe-oF Discovery Controller 구현 (ctrlr_discovery.c)
 *
 * === 파일의 역할 ===
 * NVMe over Fabrics 호스트는 target에 접속하기 전 "어떤 subsystem이 어떤 listener에서
 * 노출되는지"를 먼저 알아야 한다. NVMe-oF 1.x 스펙은 이를 위해 Discovery Service라는
 * 특수 NQN(`nqn.2014-08.org.nvmexpress.discovery`)을 정의하고, 호스트가 이 Discovery
 * Subsystem에 Connect 후 Get Log Page (LID=0x70 Discovery Log Page) 명령으로 엔트리 목록을
 * 받도록 한다. 본 파일은 그 Discovery Log Page 생성/전송과 Discovery Log Change
 * Asynchronous Event Notification (AEN) 발사 로직을 구현한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   호스트 -> Connect(Discovery NQN) -> 일반 Connect 처리(ctrlr.c)
 *   호스트 -> Get Log Page (LID=Discovery) -> nvmf_ctrlr_get_log_page()(ctrlr.c)
 *      -> nvmf_get_discovery_log_page_async() [본 파일]
 *      -> spdk_thread_send_msg(app_thread, nvmf_get_discovery_log_page, ctx)
 *      -> nvmf_get_discovery_log_page() [app thread context]
 *      -> nvmf_generate_discovery_log() [모든 subsystem/listener 순회]
 *      -> 결과를 req->iov에 복사 후 spdk_nvmf_request_complete().
 * 한편 listener/subsystem 추가/제거 시 spdk_nvmf_send_discovery_log_notice()가 호출되어
 * Discovery Subsystem에 연결된 모든 호스트 ctrlr에 AEN을 보낸다. 이 AEN은 RFC4 spec의
 * Discovery Log Change(LID=0x70 항목) 알림이다. 실행 컨텍스트는 application thread
 * (spdk_thread_get_app_thread())로 직렬화되어 lockless 패턴을 유지한다.
 *
 * === 타 모듈과의 연결 ===
 * - nvmf_internal.h: spdk_nvmf_tgt/subsystem/ctrlr/listener/referral 등 코어 타입.
 *   discovery_genctr(64bit 카운터)와 discovery_filter, referrals TAILQ를 공유한다.
 * - transport.h: nvmf_transport_listener_discover() - 트랜스포트가 자신만의 트랜스포트
 *   타입별 필드(예: TCP는 TSAS=NONE, RDMA는 RDMA_QPTYPE/PRTYPE 등)를 entry에 채운다.
 * - spdk/nvmf_spec.h: Discovery Log Page 관련 구조체/매크로 (subtype, eflags 비트 등).
 * - g_custom_discovery_filter (nvmf.c 정의): 사용자 정의 필터 함수 포인터 - 매칭 조건
 *   확장에 사용.
 * - listener/subsystem 변경 측: subsystem.c, transport.c가 변경 후 본 파일의
 *   send_discovery_log_notice()를 트리거.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_nvmf_send_discovery_log_notice: discovery_genctr 증가 후 AEN 발사 (모든
 *   discovery ctrlr에 spdk_thread_send_msg 디스패치).
 * - nvmf_discovery_compare_trtype/tr_addr/tr_svcid/trid: discovery_filter 비트마스크에
 *   따른 trid 매칭.
 * - nvmf_generate_discovery_log: subsystem 순회 + listener 매칭 + referral 첨부로
 *   spdk_nvmf_discovery_log_page를 동적 realloc으로 구성.
 * - nvmf_get_discovery_log_page (msg handler): 생성된 log를 req->iov에 (offset, length)
 *   기준으로 분할 복사 + 잔여 영역 zero fill + completion 송신.
 * - nvmf_get_discovery_log_page_async: 호스트의 Get Log Page를 받아 ctx 할당 후
 *   app thread로 전송하는 진입점.
 * - struct nvmf_discovery_log_ctx: 비동기 처리 컨텍스트 (hostnqn 사본, offset/length,
 *   cmd_source_trid, rae 비트 등 보존).
 */

#include "spdk/stdinc.h"                            /* [한국어] 표준 C 헤더 묶음 - calloc/free/strdup/memcpy/strcasecmp/snprintf 등 사용 */

#include "nvmf_internal.h"                          /* [한국어] tgt/subsystem/ctrlr/listener/referral 등 NVMe-oF 코어 내부 타입 */
#include "transport.h"                              /* [한국어] nvmf_transport_listener_discover - 트랜스포트별 entry 채움 콜백 */

#include "spdk/string.h"                            /* [한국어] spdk_min/spdk_strerror 등 - 본 파일에서 spdk_min 사용 */
#include "spdk/trace.h"                             /* [한국어] SPDK 트레이스 매크로 (TRACE_NVMF_*) - 디버깅 인프라 */
#include "spdk/nvmf_spec.h"                         /* [한국어] Discovery Log Page 구조체/플래그 (subtype/eflags) - NVMe-oF 1.x 스펙 정의 */
#include "spdk_internal/assert.h"                   /* [한국어] SPDK 내부 assert 매크로 */

#include "spdk/log.h"                               /* [한국어] SPDK_ERRLOG / SPDK_DEBUGLOG 로깅 매크로 */

/*
 * [한국어]
 * spdk_nvmf_send_discovery_log_notice - Discovery Log Change AEN 발사
 *
 * @tgt: 알림을 보낼 NVMe-oF target
 * @hostnqn: 특정 호스트만 대상으로 할 때 NQN 문자열, NULL이면 모든 호스트 대상
 *
 * Listener 추가/삭제, subsystem 활성/비활성, host whitelist 변경 등 Discovery Log Page의
 * 내용에 영향을 주는 변경이 일어나면 호출자가 본 함수를 트리거한다. 이 함수는
 * (1) tgt->discovery_genctr를 증가시켜 다음에 생성될 log page의 generation counter를
 * 갱신하고, (2) Discovery Subsystem(NQN=nqn.2014-08.org.nvmexpress.discovery)에 붙어 있는
 * 각 ctrlr 스레드로 nvmf_ctrlr_async_event_discovery_log_change_notice를 spdk_thread_send_msg로
 * 디스패치한다. 메시지를 받은 ctrlr의 thread는 자신의 AEN 마스크를 검사 후 호스트로
 * Discovery Log Change AEN을 송신한다.
 *
 * 실행 컨텍스트: 호출자에 따라 다양 (RPC handler, listener 추가 경로 등). 호출 즉시
 * genctr만 증가시키고 실제 AEN 송신은 각 ctrlr 스레드에서 비동기로 일어난다.
 *
 * 호출 체인:
 *   subsystem.c/transport.c (listener add/remove 등) -> [본 함수]
 *      -> spdk_thread_send_msg -> ctrlr 스레드 -> nvmf_ctrlr_async_event_discovery_log_change_notice
 */
void
spdk_nvmf_send_discovery_log_notice(struct spdk_nvmf_tgt *tgt, const char *hostnqn)
{
	struct spdk_nvmf_subsystem *discovery_subsystem; /* [한국어] Discovery NQN으로 찾을 subsystem 포인터 - 없으면 알림 대상 없음 */
	struct spdk_nvmf_ctrlr *ctrlr;                   /* [한국어] discovery subsystem에 연결된 controller 순회용 임시 포인터 */

	tgt->discovery_genctr++;                         /* [한국어] generation counter 증가 - 호스트가 다음 log page 헤더의 genctr 변화로 변경 감지 */
	discovery_subsystem = spdk_nvmf_tgt_find_subsystem(tgt, SPDK_NVMF_DISCOVERY_NQN); /* [한국어] 표준 Discovery NQN(nqn.2014-08.org.nvmexpress.discovery) subsystem 조회 */

	if (discovery_subsystem) {                       /* [한국어] discovery subsystem이 활성일 때만 AEN 의미 있음 */
		/** There is a change in discovery log for hosts with given hostnqn */
		TAILQ_FOREACH(ctrlr, &discovery_subsystem->ctrlrs, link) { /* [한국어] discovery subsystem에 연결된 모든 ctrlr 순회 - link는 ctrlr.subsys 리스트 노드 */
			if (hostnqn == NULL || strcmp(hostnqn, ctrlr->hostnqn) == 0) { /* [한국어] hostnqn==NULL이면 모두, 아니면 해당 호스트만 - 권한 변경은 특정 호스트에만 영향 */
				spdk_thread_send_msg(ctrlr->thread, nvmf_ctrlr_async_event_discovery_log_change_notice, ctrlr); /* [한국어] cross-thread 메시지 - ctrlr이 소유한 SPDK thread로 lockless 전달, 실제 AEN 송신은 그 스레드에서 */
			}
		}
	}
}

/*
 * [한국어]
 * nvmf_discovery_compare_trtype - 두 transport id의 트랜스포트 타입 비교
 *
 * @trid1: 비교 기준 1 (보통 listener의 trid)
 * @trid2: 비교 기준 2 (보통 호스트가 Connect한 source trid)
 * @return: 같은 트랜스포트면 true
 *
 * 표준 트랜스포트(SPDK_NVME_TRANSPORT_RDMA/TCP/FC 등)는 trtype enum 비교로 충분하지만,
 * SPDK는 사용자 정의 트랜스포트(SPDK_NVME_TRANSPORT_CUSTOM)를 trstring 문자열로 식별하므로
 * 이 경우엔 strcasecmp로 비교해야 한다. NVMe-oF 1.1 spec 기준 trtype은 1바이트 enum.
 */
static bool
nvmf_discovery_compare_trtype(const struct spdk_nvme_transport_id *trid1,
			      const struct spdk_nvme_transport_id *trid2)
{
	if (trid1->trtype == SPDK_NVME_TRANSPORT_CUSTOM) { /* [한국어] CUSTOM 트랜스포트(예: 사용자 정의 TCP fork)는 trstring으로만 식별 가능 */
		return strcasecmp(trid1->trstring, trid2->trstring) == 0; /* [한국어] 대소문자 무시 비교 - "TCP", "tcp" 등 표기 일관성 보장 */
	} else {                                         /* [한국어] 표준 trtype(RDMA/TCP/FC/PCIe)은 enum 정수 비교만으로 충분 */
		return trid1->trtype == trid2->trtype;   /* [한국어] enum 비교 - O(1) */
	}
}

/*
 * [한국어]
 * nvmf_discovery_compare_tr_addr - 두 transport id의 주소(adrfam+traddr) 비교
 *
 * @trid1: 비교 기준 1
 * @trid2: 비교 기준 2
 * @return: 같은 주소(주소 패밀리 + 주소 문자열)면 true
 *
 * adrfam(IPv4/IPv6/IB/FC 등) 일치 + traddr 문자열 일치를 모두 만족해야 같은 주소로 본다.
 * IP 주소 비교에서 대소문자는 의미 없지만 IPv6의 경우 콜론 표기도 strcasecmp로 동일 취급.
 */
static bool
nvmf_discovery_compare_tr_addr(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	return trid1->adrfam == trid2->adrfam && strcasecmp(trid1->traddr, trid2->traddr) == 0; /* [한국어] adrfam(enum) + traddr(문자열) 둘 다 일치해야 같은 주소 */
}

/*
 * [한국어]
 * nvmf_discovery_compare_tr_svcid - 두 transport id의 서비스 ID(포트) 비교
 *
 * @trid1: 비교 기준 1
 * @trid2: 비교 기준 2
 * @return: trsvcid(주로 TCP 포트 문자열)가 같으면 true
 *
 * TCP/RDMA 모두 trsvcid는 문자열로 표현(예: "4420"). strcasecmp로 일관 처리.
 */
static bool
nvmf_discovery_compare_tr_svcid(const struct spdk_nvme_transport_id *trid1,
				const struct spdk_nvme_transport_id *trid2)
{
	return strcasecmp(trid1->trsvcid, trid2->trsvcid) == 0; /* [한국어] svcid 문자열 비교 - "4420"==포트 4420 등 */
}

/*
 * [한국어]
 * nvmf_discovery_compare_trid - filter 비트마스크에 따라 trid를 다층 비교
 *
 * @filter: SPDK_NVMF_TGT_DISCOVERY_MATCH_* 비트마스크 (tgt->discovery_filter)
 * @trid1: 보통 listener의 trid (target side)
 * @trid2: 보통 호스트가 connect한 source trid (cmd_source_trid)
 * @return: filter가 요구하는 모든 항목이 일치하면 true (= log page에 포함)
 *
 * 운영자는 RPC nvmf_set_discovery_filter로 "어떤 listener를 호스트에게 보여줄지"를
 * 비트 단위로 제어할 수 있다. 예를 들어 MATCH_TRANSPORT_TYPE만 켜면 호스트가 connect한
 * 트랜스포트와 같은 트랜스포트의 listener만 노출되고, 다른 비트가 추가되면 더 엄격해진다.
 * MATCH_CUSTOM 비트는 g_custom_discovery_filter 함수 포인터(외부 등록)로 위임한다.
 *
 * 호출 체인:
 *   nvmf_generate_discovery_log() -> [본 함수]
 */
static bool
nvmf_discovery_compare_trid(uint32_t filter,
			    const struct spdk_nvme_transport_id *trid1,
			    const struct spdk_nvme_transport_id *trid2)
{
	if ((filter & SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE) != 0 && /* [한국어] 트랜스포트 타입 매칭 비트가 켜져 있는지 검사 */
	    !nvmf_discovery_compare_trtype(trid1, trid2)) {                /* [한국어] 켜져 있는데 불일치면 이 listener 제외 */
		SPDK_DEBUGLOG(nvmf, "transport type mismatch between %d (%s) and %d (%s)\n", /* [한국어] 디버그 - 누락 사유 추적 */
			      trid1->trtype, trid1->trstring, trid2->trtype, trid2->trstring);
		return false;                            /* [한국어] 매칭 실패 - 호출자(generate_log)에서 이 listener 건너뜀 */
	}

	if ((filter & SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS) != 0 && /* [한국어] 주소 매칭 비트 - 보통 같은 NIC로 들어온 호스트에게만 동일 주소 노출 */
	    !nvmf_discovery_compare_tr_addr(trid1, trid2)) {
		SPDK_DEBUGLOG(nvmf, "transport addr mismatch between %s and %s\n",
			      trid1->traddr, trid2->traddr);
		return false;
	}

	if ((filter & SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID) != 0 && /* [한국어] svcid(포트) 매칭 비트 - 같은 포트로 들어온 호스트에게만 동일 포트 listener 노출 */
	    !nvmf_discovery_compare_tr_svcid(trid1, trid2)) {
		SPDK_DEBUGLOG(nvmf, "transport svcid mismatch between %s and %s\n",
			      trid1->trsvcid, trid2->trsvcid);
		return false;
	}

	if ((filter & SPDK_NVMF_TGT_DISCOVERY_MATCH_CUSTOM) != 0 &&         /* [한국어] CUSTOM 비트 - 외부에서 등록한 함수가 mismatch 판단 시 제외 */
	    g_custom_discovery_filter(trid1, trid2)) {                       /* [한국어] g_custom_discovery_filter는 nvmf.c에 정의된 전역 fn ptr (사용자 등록) */
		SPDK_DEBUGLOG(nvmf, "custom discovery filter mismatch\n");
		return false;
	}

	return true;                                     /* [한국어] 모든 활성 비트 검사 통과 - 이 listener를 log page에 포함 */
}

/*
 * [한국어]
 * nvmf_generate_discovery_log - tgt 전체를 스캔해 Discovery Log Page를 동적 생성
 *
 * @tgt: 대상 NVMe-oF target
 * @hostnqn: 요청 호스트의 NQN - 호스트 권한 필터링에 사용
 * @log_page_size: [out] 생성된 log page 전체 바이트 크기 (헤더 + N*entry)
 * @cmd_source_trid: 호스트가 connect한 source trid (filter 매칭 기준)
 * @return: 동적 할당된 spdk_nvmf_discovery_log_page* (호출자가 free) 또는 NULL(메모리 실패)
 *
 * NVMe-oF 1.x Discovery Log Page 형식:
 *   [discovery_log_page header (genctr/numrec/...)] + entries[numrec]
 * 본 함수는 매번 호출 시 헤더 1개로 시작해, 각 매칭되는 listener마다 realloc으로 entry 한
 * 칸씩 늘려가며 채운다. 마지막에 referral(다른 target으로의 직접 알림)을 추가한다.
 *
 * 동작 단계:
 *   1) 모든 subsystem 순회. INACTIVE/DEACTIVATING은 건너뜀.
 *   2) host whitelist 검사 (host allowed 아니면 건너뜀).
 *   3) subsystem의 모든 listener 순회 → discovery_filter 매칭 → entry 채움
 *      → 트랜스포트별 후처리 콜백(nvmf_transport_listener_discover) 호출.
 *   4) tgt->referrals 리스트 순회 → 권한 검사 후 referral entry 복사.
 *   5) numrec/genctr/log_page_size 채움 후 반환.
 *
 * 실행 컨텍스트: 반드시 application thread (assert로 강제). subsystem 리스트는
 * app_thread에서만 변경되어 lockless 순회 가능.
 *
 * 호출 체인:
 *   nvmf_get_discovery_log_page() (app thread msg handler) -> [본 함수]
 *      -> nvmf_transport_listener_discover() -> 트랜스포트별 ops->listener_discover
 */
static struct spdk_nvmf_discovery_log_page *
nvmf_generate_discovery_log(struct spdk_nvmf_tgt *tgt, const char *hostnqn, size_t *log_page_size,
			    struct spdk_nvme_transport_id *cmd_source_trid)
{
	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] app thread에서만 실행되어야 - subsystem/listener 리스트가 lockless 가정 */

	uint64_t numrec = 0;                             /* [한국어] log page에 채운 entry 개수 누적 - 헤더의 numrec 필드로 기록 */
	struct spdk_nvmf_subsystem *subsystem;           /* [한국어] subsystem 순회용 */
	struct spdk_nvmf_subsystem_listener *listener;   /* [한국어] subsystem 내부 listener 순회용 */
	struct spdk_nvmf_discovery_log_page_entry *entry; /* [한국어] 채울 entry 슬롯 포인터 (realloc 후 갱신) */
	struct spdk_nvmf_discovery_log_page *disc_log;   /* [한국어] 동적 확장되는 log page 본체 */
	size_t cur_size;                                 /* [한국어] 현재까지 누적된 log page 바이트 크기 */
	struct spdk_nvmf_referral *referral;             /* [한국어] referral 순회용 */

	SPDK_DEBUGLOG(nvmf, "Generating log page for genctr %" PRIu64 "\n", /* [한국어] 디버그 - 현재 generation counter 출력 */
		      tgt->discovery_genctr);

	cur_size = sizeof(struct spdk_nvmf_discovery_log_page); /* [한국어] 헤더만 담는 최소 크기로 시작 - entry 0개 */
	disc_log = calloc(1, cur_size);                  /* [한국어] zero 초기화로 헤더 할당 - genctr/numrec 등 0으로 시작 */
	if (disc_log == NULL) {                          /* [한국어] OOM - 호스트에 internal device error로 응답하기 위해 NULL 반환 */
		SPDK_ERRLOG("Discovery log page memory allocation error\n");
		return NULL;
	}

	for (subsystem = spdk_nvmf_subsystem_get_first(tgt); /* [한국어] tgt의 모든 subsystem 순회 시작 - get_first/next는 lockless iterator */
	     subsystem != NULL;
	     subsystem = spdk_nvmf_subsystem_get_next(subsystem)) {
		if ((subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE) || /* [한국어] 비활성 상태는 호스트에게 노출하지 않음 - 외부에서 보면 없는 것과 동일 */
		    (subsystem->state == SPDK_NVMF_SUBSYSTEM_DEACTIVATING)) { /* [한국어] 종료 중인 것도 제외 - 새 connect 받지 않음 */
			continue;
		}

		if (!spdk_nvmf_subsystem_host_allowed(subsystem, hostnqn)) { /* [한국어] subsystem의 host whitelist 검사 - 권한 없는 호스트에게는 숨김 */
			continue;
		}

		TAILQ_FOREACH(listener, &subsystem->listeners, link) { /* [한국어] subsystem에 등록된 모든 listener 순회 (subsystem.listeners TAILQ) */
			if (!nvmf_subsystem_listener_is_active(listener)) { /* [한국어] listener가 활성 상태(트랜스포트 listen 성공)일 때만 노출 */
				continue;
			}

			if (!nvmf_discovery_compare_trid(tgt->discovery_filter, listener->trid, cmd_source_trid)) { /* [한국어] discovery_filter 비트마스크에 따른 trid 매칭 - 미매칭 listener 제외 */
				continue;
			}

			SPDK_DEBUGLOG(nvmf, "listener %s:%s trtype %s\n", listener->trid->traddr, listener->trid->trsvcid, /* [한국어] 디버그 - 어떤 listener가 포함되는지 추적 */
				      listener->trid->trstring);

			size_t new_size = cur_size + sizeof(*entry); /* [한국어] entry 한 칸 추가한 새 크기 */
			void *new_log_page = realloc(disc_log, new_size); /* [한국어] log page 재할당 - 기존 데이터 보존 */

			if (new_log_page == NULL) {      /* [한국어] realloc 실패 - 기존 disc_log은 여전히 유효, 부분 결과로 종료 */
				SPDK_ERRLOG("Discovery log page memory allocation error\n");
				break;
			}

			disc_log = new_log_page;         /* [한국어] 재할당 성공 - 포인터 갱신 (이전 포인터는 무효) */
			cur_size = new_size;             /* [한국어] 누적 크기 갱신 */

			entry = &disc_log->entries[numrec]; /* [한국어] 새로 늘어난 슬롯 포인터 - entries는 헤더 뒤 가변 배열 */
			memset(entry, 0, sizeof(*entry)); /* [한국어] entry 전체 0 초기화 - 미사용 필드는 reserved 0 유지 (NVMe spec 요구) */
			entry->portid = listener->id;    /* [한국어] Port Identifier - SPDK가 listener에 부여한 고유 ID */
			entry->cntlid = 0xffff;          /* [한국어] Controller ID = 0xFFFF (Dynamic) - 호스트가 connect 시 SPDK가 동적 할당함을 알림 */
			entry->asqsz = listener->transport->opts.max_aq_depth; /* [한국어] Admin Submission Queue Size - 트랜스포트 옵션의 admin queue 깊이 광고 */
			entry->subtype = subsystem->subtype; /* [한국어] Subsystem Type - NVM/Discovery(Current/Referral) 구분 */
			snprintf(entry->subnqn, sizeof(entry->subnqn), "%s", subsystem->subnqn); /* [한국어] Subsystem NQN 복사 - 최대 256바이트 (NVMe spec NVMF Discovery Log Page Entry) */

			if (subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT) { /* [한국어] 본 target 자신이 Current Discovery Subsystem이면 추가 플래그 설정 */
				/* Each listener in the Current Discovery Subsystem provides access
				 * to the same Discovery Log Pages, so set the Duplicate Returned
				 * Information flag. */
				entry->eflags |= SPDK_NVMF_DISCOVERY_LOG_EFLAGS_DUPRETINFO; /* [한국어] DUPRETINFO - 같은 정보 중복 반환 가능 비트 (호스트가 캐시 활용 가능) */
				/* Since the SPDK NVMeoF target supports Asynchronous Event Request
				 * and Keep Alive commands, set the Explicit Persistent Connection
				 * Support for Discovery flag. */
				entry->eflags |= SPDK_NVMF_DISCOVERY_LOG_EFLAGS_EPCSD; /* [한국어] EPCSD - 명시적 영구 연결 지원 비트 (Keep Alive + AEN 지원 광고) */
			}

			nvmf_transport_listener_discover(listener->transport, listener->trid, entry); /* [한국어] 트랜스포트별 후처리 - TCP는 TSAS=NONE, RDMA는 RDMA_QPTYPE/PRTYPE/CMS 등 채움 */

			numrec++;                        /* [한국어] entry 카운트 증가 - 헤더 numrec에 최종 반영 */
		}
	}

	TAILQ_FOREACH(referral, &tgt->referrals, link) { /* [한국어] tgt에 등록된 referral(다른 discovery target 광고) 순회 */
		SPDK_DEBUGLOG(nvmf, "referral %s:%s trtype %s\n", referral->trid.traddr, referral->trid.trsvcid,
			      referral->trid.trstring);

		if (!spdk_nvmf_referral_host_allowed(referral, hostnqn)) { /* [한국어] referral도 호스트 화이트리스트 적용 가능 */
			continue;
		}

		size_t new_size = cur_size + sizeof(*entry); /* [한국어] entry 한 칸 추가 */
		void *new_log_page = realloc(disc_log, new_size); /* [한국어] log page 확장 */

		if (new_log_page == NULL) {              /* [한국어] OOM - 부분 결과로 break (이미 채운 entry는 유효) */
			SPDK_ERRLOG("Discovery log page memory allocation error\n");
			break;
		}

		disc_log = new_log_page;                 /* [한국어] 포인터 갱신 */
		cur_size = new_size;

		entry = &disc_log->entries[numrec];      /* [한국어] 새 슬롯 포인터 */
		memcpy(entry, &referral->entry, sizeof(*entry)); /* [한국어] referral은 미리 만들어진 entry를 통째로 복사 (RPC nvmf_discovery_add_referral에서 구성) */

		numrec++;
	}


	disc_log->numrec = numrec;                       /* [한국어] 헤더의 entry 수 기록 - 호스트가 이만큼 읽어야 함을 인지 */
	disc_log->genctr = tgt->discovery_genctr;        /* [한국어] 헤더의 generation counter - 호스트가 이전 값과 비교해 변경 감지 */
	*log_page_size = cur_size;                       /* [한국어] 호출자에게 전체 크기 전달 (offset/length copy 시 경계 검사용) */

	return disc_log;                                 /* [한국어] 호출자가 사용 후 free 책임 */
}

/* Async discovery log page generation context */
/*
 * [한국어]
 * nvmf_discovery_log_ctx - Get Log Page (Discovery) 비동기 처리 컨텍스트
 *
 * Get Log Page는 호스트의 임의 SPDK thread에서 도착하지만, subsystem/listener 리스트는
 * application thread에서만 일관되게 순회 가능하므로 spdk_thread_send_msg로 디스패치한다.
 * 이때 원래 요청 정보를 보존하기 위해 본 구조체를 ctx로 사용한다.
 */
struct nvmf_discovery_log_ctx {
	struct spdk_nvmf_request *req;
	/* [한국어] 원본 NVMe-oF 요청 객체 - 응답 송신과 iov(payload) 접근에 사용.
	 * 설정자: nvmf_get_discovery_log_page_async가 호스트 요청을 받자마자 보존.
	 * 읽는 자: app thread의 nvmf_get_discovery_log_page handler.
	 * 값 범위: 유효한 spdk_nvmf_request 포인터, 처리 끝까지 살아있도록 호출자(ctrlr.c)가 보장.
	 * 동기화: 처리 중 다른 스레드가 접근하지 않음 (req는 처리 완료 시까지 한 스레드에서만 다룸). */

	struct spdk_nvmf_tgt *tgt;
	/* [한국어] 대상 NVMe-oF target 포인터.
	 * 설정자: async 진입점에서 req->qpair->ctrlr->subsys->tgt 캡처.
	 * 읽는 자: nvmf_generate_discovery_log()에 전달.
	 * 값 범위: 항상 유효한 tgt - subsys가 살아있으면 tgt도 살아있다.
	 * 동기화: app thread에서만 자료구조 변경되므로 lockless 순회. */

	char *hostnqn;
	/* [한국어] 요청 호스트의 NQN strdup 사본.
	 * 설정자: async 진입점에서 strdup(req->qpair->ctrlr->hostnqn).
	 * 읽는 자: nvmf_generate_discovery_log()의 host_allowed/referral_allowed 검사.
	 * 값 범위: NUL 종료 문자열, 최대 256바이트.
	 * 동기화: ctx 소유자(스레드)만 접근. handler 종료 시 free. */

	uint64_t offset;
	/* [한국어] Get Log Page의 LPO(Log Page Offset) - 호스트가 큰 log page를 분할 read 시 시작 오프셋.
	 * 설정자: 호스트가 보낸 NVMe Get Log Page 명령의 LPOL/LPOU 결합값.
	 * 읽는 자: handler에서 log page 시작 + offset부터 length만큼 복사.
	 * 값 범위: 0 이상 64비트 - log_page_size를 넘으면 INVALID_FIELD로 응답.
	 * 동기화: ctx 단일 소유. */

	uint32_t length;
	/* [한국어] Get Log Page의 NUMD(Number of Dwords)에서 환산한 바이트 길이.
	 * 설정자: ctrlr.c가 (numd+1)*4 등으로 계산해 넘김.
	 * 읽는 자: handler가 iov로 복사할 최대 바이트.
	 * 값 범위: 0 이상 32비트.
	 * 동기화: ctx 단일 소유. */

	struct spdk_nvme_transport_id cmd_source_trid;
	/* [한국어] 호스트가 이 Get Log Page를 보낸 source 트랜스포트(qpair)의 trid 사본.
	 * 설정자: async 진입점에서 호출자가 채운 cmd_source_trid를 그대로 복사 보존.
	 * 읽는 자: generate_discovery_log -> compare_trid에서 listener와 매칭.
	 * 값 범위: spdk_nvme_transport_id 구조체 전체 (trtype/adrfam/traddr/trsvcid 등).
	 * 동기화: ctx 단일 소유. */

	bool rae;
	/* [한국어] Retain Asynchronous Event - Get Log Page 명령의 LSP/RAE 비트.
	 * 설정자: 호스트가 명령에서 RAE=1로 보내면 AEN mask 유지.
	 * 읽는 자: handler가 RAE=0이면 nvmf_ctrlr_unmask_aen으로 다음 변경 알림 재무장.
	 * 값 범위: false/true.
	 * 동기화: ctx 단일 소유. */
};

/*
 * [한국어]
 * nvmf_get_discovery_log_page - app thread에서 실행되는 실제 log 생성/복사 핸들러
 *
 * @arg: nvmf_discovery_log_ctx 포인터
 *
 * spdk_thread_send_msg로 application thread로 디스패치된 후 실행된다. 동작:
 *   1) nvmf_generate_discovery_log()로 log page를 동적 생성.
 *   2) 호스트 요청의 (offset, length)에 맞춰 req->iov 배열에 분할 복사.
 *   3) 부족분은 0으로 채움 (호스트는 length만큼 받기를 기대).
 *   4) RAE=0이면 AEN 마스크 해제 (다음 변경 알림 활성화).
 *   5) 에러 발생 시 NVMe Generic Status: Invalid Field 응답.
 *   6) ctx 메모리 해제 + spdk_nvmf_request_complete로 호스트에 응답.
 *
 * 실행 컨텍스트: application thread (assert로 강제). 본 thread는 모든 subsystem/listener
 * 변경의 직렬화 지점이므로 안전한 lockless 순회가 가능하다.
 *
 * 호출 체인:
 *   nvmf_get_discovery_log_page_async() -> spdk_thread_send_msg(app_thread, this) -> [본 핸들러]
 *      -> nvmf_generate_discovery_log() -> spdk_nvmf_request_complete()
 */
static void
nvmf_get_discovery_log_page(void *arg)
{
	struct nvmf_discovery_log_ctx *ctx = arg;        /* [한국어] msg payload(ctx) 캐스팅 */
	struct spdk_nvmf_request *req = ctx->req;        /* [한국어] 응답 보낼 원본 요청 */
	struct spdk_nvmf_discovery_log_page *discovery_log_page; /* [한국어] 생성된 log page (free 책임 본 핸들러) */
	size_t log_page_size = 0;                        /* [한국어] generate가 채워줄 전체 크기 - 경계 검사용 */
	size_t copy_len = 0;                             /* [한국어] 현재 iov 슬롯에 복사할 바이트 수 */
	size_t zero_len = 0;                             /* [한국어] 마지막 슬롯의 잔여(zero fill 영역) */
	struct iovec *tmp;                               /* [한국어] req->iov 배열 순회용 */
	uint64_t offset = ctx->offset;                   /* [한국어] log_page 내 현재 복사 시작 오프셋 (진행하며 증가) */
	uint32_t length = ctx->length;                   /* [한국어] 호스트가 요청한 남은 바이트 수 (진행하며 감소) */
	int rc = 0;                                      /* [한국어] 최종 에러 코드 - complete 단계에서 status 변환 */

	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] application thread에서만 실행되어야 함 */

	discovery_log_page = nvmf_generate_discovery_log(ctx->tgt, ctx->hostnqn,    /* [한국어] log page 본체 동적 생성 - subsystem/listener/referral 스캔 */
			     &log_page_size, &ctx->cmd_source_trid);

	if (offset >= log_page_size) {                   /* [한국어] 호스트가 요청한 시작 오프셋이 log page 크기를 넘으면 잘못된 요청 */
		SPDK_ERRLOG("Invalid Get log page discovery offset: (%" PRIu64 "), log page size (%zu)\n",
			    offset, log_page_size);
		rc = -EINVAL;                            /* [한국어] complete 단계에서 INVALID_FIELD로 변환 */
		free(discovery_log_page);                /* [한국어] 부분 생성된 log page 해제 - 누수 방지 */
		goto complete;
	}

	/* Copy the valid part of the discovery log page, if any */
	if (discovery_log_page) {                        /* [한국어] generate가 NULL 반환 안했으면 (할당 성공) */
		for (tmp = req->iov; tmp < req->iov + req->iovcnt; tmp++) { /* [한국어] req의 모든 iov 슬롯 순회 - 호스트 페이로드는 SGL/PRP 결과 여러 조각일 수 있음 */
			copy_len = spdk_min(tmp->iov_len, length); /* [한국어] 이 슬롯 크기와 남은 length 중 작은 쪽 (overflow 방지) */
			copy_len = spdk_min(log_page_size - offset, copy_len); /* [한국어] log page 끝까지 남은 바이트와도 비교 */

			memcpy(tmp->iov_base, (char *)discovery_log_page + offset, copy_len); /* [한국어] log page의 offset 위치부터 호스트 메모리(iov)로 복사 */

			offset += copy_len;              /* [한국어] log page 내 위치 진행 */
			length -= copy_len;              /* [한국어] 호스트가 더 받을 양 감소 */
			zero_len = tmp->iov_len - copy_len; /* [한국어] 이 슬롯에서 채우지 못한 잔여 - log_page 끝났으면 0으로 채울 영역 */
			if (log_page_size <= offset || length == 0) { /* [한국어] log 끝 도달 또는 호스트 요청 만족 시 break */
				break;
			}
		}
		/* Zero out the rest of the payload */
		if (zero_len) {                          /* [한국어] 마지막 복사 슬롯에 잔여 영역이 있으면 0으로 채움 - 보안상 이전 메모리 누설 방지 */
			memset((char *)tmp->iov_base + copy_len, 0, zero_len);
		}

		for (++tmp; tmp < req->iov + req->iovcnt; tmp++) { /* [한국어] 이후 슬롯들도 모두 0으로 - 호스트가 length만큼 받기를 예상하지만 SGL은 더 클 수 있음 */
			memset((char *)tmp->iov_base, 0, tmp->iov_len);
		}

		free(discovery_log_page);                /* [한국어] 임시 log page 해제 */
	}

complete:
	if (rc == 0 && !ctx->rae) {                      /* [한국어] 성공 + RAE=0이면 AEN 마스크 해제 - 다음 변경 알림 가능하게 */
		nvmf_ctrlr_unmask_aen(req->qpair->ctrlr, SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE_MASK_BIT); /* [한국어] Discovery Log Change AEN 비트 unmask */
	}

	if (rc != 0) {                                   /* [한국어] 에러 발생 - NVMe completion에 status 채움 */
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC; /* [한국어] Status Code Type = Generic */
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD; /* [한국어] Status Code = Invalid Field in Command (offset 범위 초과 등) */
	}

	free(ctx->hostnqn);                              /* [한국어] strdup된 hostnqn 사본 해제 */
	free(ctx);                                       /* [한국어] ctx 자체 해제 - msg dispatch가 끝났으므로 더 이상 필요 없음 */

	spdk_nvmf_request_complete(req);                 /* [한국어] 호스트로 응답 송신 - 트랜스포트별 op->req_complete 호출 */
}

/*
 * [한국어]
 * nvmf_get_discovery_log_page_async - Get Log Page (Discovery) 진입점, app thread로 디스패치
 *
 * @req: 호스트의 NVMe-oF Get Log Page 요청
 * @offset: Log Page Offset (LPO)
 * @length: 요청 바이트 수 (NUMD에서 환산)
 * @cmd_source_trid: 호스트 qpair의 트랜스포트 trid (filter 매칭용)
 * @rae: Retain AE 비트 (true면 AEN 마스크 유지)
 *
 * 호출 시점은 ctrlr.c의 Get Log Page 디스패치(LID=0x70)에서. 이 함수는 임의 스레드(qpair의
 * polling group 스레드)에서 실행되므로 직접 subsystem 리스트를 만지지 않고, ctx에 정보를
 * 보존한 뒤 application thread로 spdk_thread_send_msg를 통해 비동기 디스패치한다.
 *
 * 에러 경로(ctx 할당/strdup 실패): 호스트에 Internal Device Error로 즉시 응답.
 *
 * 호출 체인:
 *   호스트 -> Get Log Page (LID=0x70) -> ctrlr.c 디스패치 -> [본 함수]
 *      -> spdk_thread_send_msg(app_thread, nvmf_get_discovery_log_page, ctx)
 */
void
nvmf_get_discovery_log_page_async(struct spdk_nvmf_request *req,
				  uint64_t offset, uint32_t length,
				  struct spdk_nvme_transport_id *cmd_source_trid,
				  bool rae)
{
	struct nvmf_discovery_log_ctx *ctx;              /* [한국어] 비동기 처리 컨텍스트 - app thread로 넘겨질 페이로드 */

	ctx = calloc(1, sizeof(*ctx));                   /* [한국어] zero-init 할당 - 모든 필드 0/NULL부터 채움 */
	if (!ctx) {                                      /* [한국어] OOM - 즉시 에러 응답 */
		SPDK_ERRLOG("Failed to allocate discovery log context\n");
		goto error;
	}

	ctx->req = req;                                  /* [한국어] 원본 요청 보존 */
	ctx->tgt = req->qpair->ctrlr->subsys->tgt;       /* [한국어] 대상 target 캡처 - qpair->ctrlr->subsys->tgt 체인 */
	ctx->hostnqn = strdup(req->qpair->ctrlr->hostnqn); /* [한국어] hostnqn 사본 - 비동기 처리 중 ctrlr이 사라져도 NQN 보존 */
	if (!ctx->hostnqn) {                             /* [한국어] strdup 실패 (OOM) - ctx 해제 후 에러 응답 */
		SPDK_ERRLOG("Failed to duplicate hostnqn\n");
		free(ctx);
		goto error;
	}
	ctx->offset = offset;                            /* [한국어] LPO 보존 */
	ctx->length = length;                            /* [한국어] 요청 길이 보존 */
	ctx->cmd_source_trid = *cmd_source_trid;         /* [한국어] source trid 통째 복사 (구조체 값 복사) */
	ctx->rae = rae;                                  /* [한국어] RAE 비트 보존 */

	spdk_thread_send_msg(spdk_thread_get_app_thread(), nvmf_get_discovery_log_page, ctx); /* [한국어] application thread로 lockless 메시지 디스패치 - 거기서 실제 처리 */
	return;

error:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC; /* [한국어] Generic Status Code Type */
	req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR; /* [한국어] Internal Device Error - 메모리 할당 실패 등 일반 내부 오류 */
	spdk_nvmf_request_complete(req);                 /* [한국어] 즉시 호스트에 응답 - 비동기 디스패치 자체에 실패 */
}
