/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2025, Oracle and/or its affiliates.
 */

/*
 * [한국어 설명] NVMe-oF Target Framework 코어 (nvmf.c)
 *
 * === 파일의 역할 ===
 * NVMe over Fabrics target의 최상위 컨테이너인 spdk_nvmf_tgt 객체와, target에 부속되는
 * referrals/listeners/transports/poll groups의 생명주기를 관리한다. 또한 트랜스포트가
 * accept한 새 qpair를 적절한 poll group에 자동 분배하는 라우팅, qpair disconnect의
 * 다단계 비동기 정리 시퀀스, target pause/resume(설정 변경 트랜잭션), JSON 설정 dump,
 * statistics 수집 등 NVMe-oF target의 "프레임워크" 기능 전반을 담당한다. ctrlr.c가
 * NVMe-oF 명령 처리(Connect/IO 명령 등)를, subsystem.c가 subsystem/namespace 관리를
 * 맡는 반면, 본 파일은 "그것들을 묶는 객체" 그 자체와 멀티스레드 디스패치를 다룬다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe-oF target 부트스트랩:
 *   subsystem 초기화 (RPC 서버 기동) -> spdk_nvmf_tgt_create()
 *     -> tgt 할당 + spdk_io_device_register(create/destroy poll_group cb)
 *   각 reactor에서 spdk_nvmf_poll_group_create() -> get_io_channel으로 spdk thread 1개당
 *     poll group 1개 자동 생성 (nvmf_tgt_create_poll_group cb).
 *   spdk_nvmf_tgt_add_transport() -> 모든 poll group에 트랜스포트 fan-out (spdk_for_each_channel).
 *   spdk_nvmf_tgt_listen_ext() -> 트랜스포트가 listen 시작.
 *   트랜스포트 accept 콜백 -> spdk_nvmf_tgt_new_qpair() -> 최적 poll group 선택 후
 *     spdk_thread_send_msg(_nvmf_poll_group_add).
 *   호스트 disconnect / 종료 -> spdk_nvmf_qpair_disconnect()의 단계별 비동기 정리.
 *   spdk_nvmf_tgt_destroy() -> 트랜스포트/subsystem/referral 모두 비동기 해제 후 free.
 * 실행 컨텍스트: 대부분의 함수는 application thread(주 thread). 일부(qpair_disconnect,
 * qpair_set_state)는 group->thread에서만 안전하게 실행되며 위반 시 assert로 강제.
 *
 * === 타 모듈과의 연결 ===
 * - nvmf_internal.h: spdk_nvmf_tgt/subsystem/poll_group/qpair/ctrlr/referral 모든 코어 타입.
 *   본 파일이 이 타입들의 생명주기 관리자 역할.
 * - transport.h: nvmf_transport_poll_group_create/destroy/add/remove 등 트랜스포트 추상 인터페이스.
 *   각 spdk_io_channel 콜백 안에서 호출되어 트랜스포트별 자원과 poll group을 결합.
 * - subsystem.c: spdk_nvmf_subsystem_destroy / nvmf_subsystem_remove_all_listeners 호출.
 *   subsystem들은 tgt->subsystems RB tree로 보관되며 destroy 시 cascade.
 * - ctrlr.c: nvmf_ctrlr_destruct/nvmf_qpair_auth_destroy 등 호출. qpair-ctrlr 분리 후
 *   ctrlr 객체 정리는 ctrlr->thread로 디스패치.
 * - mdns_server.c: nvmf_tgt_stop_mdns_prr 호출 (target 종료 시).
 * - spdk/thread.h: spdk_io_device_register / spdk_get_io_channel / spdk_for_each_channel /
 *   spdk_thread_send_msg - SPDK의 io_channel 추상화로 reactor당 1개 poll group을 매핑.
 * 데이터 흐름: 호스트 -> 트랜스포트 socket -> 트랜스포트 accept 콜백 -> spdk_nvmf_tgt_new_qpair
 *   -> get_optimal_poll_group / round-robin 선택 -> _nvmf_poll_group_add (msg) -> qpair-tgroup 결합
 *   -> 이후 polling은 group->thread가 담당.
 * 공유 자료구조:
 *   - g_nvmf_tgts (전역 TAILQ): 같은 프로세스 내 모든 target (보통 1개).
 *   - tgt->mutex: poll_groups TAILQ 보호 (cross-thread 등록/해제 직렬화).
 *   - group->mutex: current_unassociated_qpairs 보호.
 *   - qpair->disconnect_started: __atomic_test_and_set으로 중복 disconnect 방지.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_nvmf_tgt_create / spdk_nvmf_tgt_destroy: target 객체 생성/단계별 비동기 해제.
 * - spdk_nvmf_tgt_listen_ext / spdk_nvmf_tgt_stop_listen: 트랜스포트별 listen 시작/중단.
 * - spdk_nvmf_tgt_add_transport: 모든 poll group에 fan-out 형태로 트랜스포트 결합 (한 곳 실패 시 rollback).
 * - spdk_nvmf_tgt_pause_polling / resume_polling: 모든 트랜스포트의 polling을 일시 중지/재개 (설정 변경 트랜잭션).
 * - spdk_nvmf_tgt_new_qpair: 트랜스포트가 accept한 qpair를 poll group에 분배 (round-robin/optimal).
 * - spdk_nvmf_qpair_disconnect: 다단계 qpair 종료 (outstanding I/O drain -> tgroup 제거 -> ctrlr free).
 * - spdk_nvmf_tgt_add_referral / remove_referral: discovery 페이지에 광고할 referral 관리.
 * - spdk_nvmf_poll_group_add / remove: qpair를 트랜스포트별 tgroup에 add/remove + state 전이.
 * - spdk_nvmf_tgt_write_config_json: 현재 설정을 RPC 재현 가능한 JSON 시퀀스로 dump.
 * - struct nvmf_qpair_disconnect_ctx: 비동기 disconnect 정리 단계 간 ctx 전달 (qpair/ctrlr/qid 보존).
 * - struct nvmf_qpair_disconnect_many_ctx: poll group 종료 시 모든 qpair iter용 ctx.
 * - struct nvmf_tgt_pause_ctx / spdk_nvmf_tgt_add_transport_ctx: 비동기 fan-out 작업의 cb 컨텍스트.
 */

#include "spdk/stdinc.h"                            /* [한국어] 표준 C 헤더 묶음 - calloc/free/strncmp/snprintf/pthread_mutex 등 사용 */

#include "spdk/bdev.h"                              /* [한국어] spdk_bdev_get_name - subsystem JSON dump 시 namespace의 bdev 이름 출력 */
#include "spdk/bit_array.h"                         /* [한국어] spdk_bit_array_create/free - tgt->subsystem_ids (subsystem ID 풀) 관리 */
#include "spdk/thread.h"                            /* [한국어] spdk_thread/spdk_io_device/spdk_for_each_channel - reactor당 poll_group 매핑 인프라 */
#include "spdk/nvmf.h"                              /* [한국어] 외부 공개 API (spdk_nvmf_*) 시그니처 - 본 파일이 구현체 */
#include "spdk/endian.h"                            /* [한국어] from_be64 등 - JSON dump 시 NGUID/EUI64 빅엔디안 변환 */
#include "spdk/string.h"                            /* [한국어] spdk_strcpy_pad/spdk_min/spdk_mem_all_zero 등 문자열·메모리 헬퍼 */
#include "spdk/log.h"                               /* [한국어] SPDK_ERRLOG/SPDK_DEBUGLOG/SPDK_LOG_REGISTER_COMPONENT */
#include "spdk_internal/usdt.h"                     /* [한국어] SPDK_DTRACE_PROBE* - DTrace USDT(User Statically-Defined Tracing) 매크로 (관측성) */

#include "nvmf_internal.h"                          /* [한국어] spdk_nvmf_tgt/subsystem/poll_group/qpair 등 NVMe-oF 코어 내부 타입 */
#include "transport.h"                              /* [한국어] nvmf_transport_* 추상 인터페이스 - 트랜스포트별 ops 디스패치 */

SPDK_LOG_REGISTER_COMPONENT(nvmf)                   /* [한국어] "nvmf" 로그 카테고리 등록 - SPDK_DEBUGLOG(nvmf, ...)로 동적 활성화 가능 */

#define SPDK_NVMF_DEFAULT_MAX_SUBSYSTEMS 1024       /* [한국어] target이 보유 가능한 subsystem 개수 기본 상한 - opts.max_subsystems가 0/미설정일 때 적용 */

static TAILQ_HEAD(, spdk_nvmf_tgt) g_nvmf_tgts = TAILQ_HEAD_INITIALIZER(g_nvmf_tgts);
/* [한국어] 프로세스 전역 target 리스트 - 보통 NVMe-oF target은 1개지만 동시 다중 운용도 지원.
 * 설정자: spdk_nvmf_tgt_create()가 INSERT_HEAD, spdk_nvmf_tgt_destroy()가 REMOVE.
 * 읽는 자: spdk_nvmf_get_tgt(name) / spdk_nvmf_get_first_tgt / get_next_tgt - RPC 핸들러가 이름으로 조회.
 * 동기화: 모든 target 생성/검색은 application thread에서만 수행되어 lockless. */

spdk_nvmf_custom_discovery_filter g_custom_discovery_filter;
/* [한국어] 사용자 등록 가능한 Discovery filter 함수 포인터 (외부 공개 - extern).
 * 설정자: spdk_nvmf_set_custom_discovery_filter()로 RPC/init 시 등록.
 * 읽는 자: ctrlr_discovery.c의 nvmf_discovery_compare_trid()에서 MATCH_CUSTOM 비트 처리 시 호출.
 * 값 범위: NULL이면 CUSTOM 매치 비트가 켜진 target 생성을 거부. 비-NULL이면 (trid1, trid2)->bool 함수.
 * 동기화: 단일 thread(main)에서 설정/읽기 - 별도 락 없음. */

typedef void (*nvmf_qpair_disconnect_cpl)(void *ctx, int status);
/* [한국어] qpair disconnect 완료 콜백 시그니처 - 현재 본 파일에서는 직접 사용하지 않으나
 * 헤더 호환성/외부 호출자(예: subsystem destroy)와의 일관성을 위해 typedef 유지. */

/* supplied to a single call to nvmf_qpair_disconnect */
/*
 * [한국어]
 * nvmf_qpair_disconnect_ctx - 단일 qpair disconnect의 다단계 비동기 처리 컨텍스트
 *
 * spdk_nvmf_qpair_disconnect()는 (1) outstanding I/O drain, (2) trasport 자원 해제,
 * (3) ctrlr 비트마스크 해제 + 마지막 qpair면 ctrlr destruct - 의 3단계 비동기 단계로 진행한다.
 * 각 단계가 다른 thread(group/ctrlr/subsys) 사이를 send_msg로 건너뛰므로 그 사이에 정보 보존이 필요.
 */
struct nvmf_qpair_disconnect_ctx {
	struct spdk_nvmf_qpair *qpair;
	/* [한국어] 종료 대상 qpair.
	 * 설정자: spdk_nvmf_qpair_disconnect() 진입점에서 ctx에 저장.
	 * 읽는 자: _nvmf_qpair_destroy(), _nvmf_qpair_disconnect_msg().
	 * 값 범위: 항상 유효 - state는 DEACTIVATING으로 전이된 후.
	 * 동기화: ctx는 단계 간 send_msg로만 전달되어 한 번에 한 thread만 접근. */

	struct spdk_nvmf_ctrlr *ctrlr;
	/* [한국어] qpair가 속한 ctrlr (admin/io qpair 모두). NULL이면 Connect 미완료 상태.
	 * 설정자: _nvmf_qpair_destroy()에서 qpair->ctrlr 캡처.
	 * 읽는 자: _nvmf_transport_qpair_fini_complete()에서 qpair_mask clear와 ctrlr destruct 트리거.
	 * 값 범위: NULL(unassociated qpair) 또는 유효한 ctrlr 포인터.
	 * 동기화: ctx 단일 소유. */

	uint16_t qid;
	/* [한국어] qpair의 NVMe queue ID (0=admin, 1..=I/O queue).
	 * 설정자: _nvmf_qpair_destroy()에서 qpair->qid 캡처 (qpair는 곧 free되므로 미리 보존).
	 * 읽는 자: _nvmf_ctrlr_free_from_qpair()의 qpair_mask clear 인덱스로 사용.
	 * 값 범위: 0..max_io_qpairs.
	 * 동기화: ctx 단일 소유. */
};

/*
 * There are several times when we need to iterate through the list of all qpairs and selectively delete them.
 * In order to do this sequentially without overlap, we must provide a context to recover the next qpair from
 * to enable calling nvmf_qpair_disconnect on the next desired qpair.
 */
/*
 * [한국어]
 * nvmf_qpair_disconnect_many_ctx - 여러 qpair를 순차 disconnect하기 위한 컨텍스트
 *
 * 사용 사례: poll group destroy(_nvmf_tgt_disconnect_qpairs), subsystem pause/destroy 등에서
 * 한 번에 여러 qpair를 끊어야 한다. 일부는 -EINPROGRESS를 반환하므로 re-iter가 필요하고,
 * 그 사이 cb_fn 호출 시점도 보존해야 한다. 따라서 별도 ctx 구조체로 추적한다.
 */
struct nvmf_qpair_disconnect_many_ctx {
	struct spdk_nvmf_subsystem *subsystem;
	/* [한국어] (사용처에 따라) 대상 subsystem - 특정 subsystem의 qpair만 끊는 경로에서 사용.
	 * 설정자: subsystem.c의 pause/destroy 헬퍼.
	 * 읽는 자: 콜러 측 헬퍼.
	 * 값 범위: 유효 포인터 또는 NULL(전체 group 대상 케이스).
	 * 동기화: ctx 단일 소유. */

	struct spdk_nvmf_poll_group *group;
	/* [한국어] qpair 순회 대상 poll group.
	 * 설정자: nvmf_tgt_destroy_poll_group_qpairs()에서 저장.
	 * 읽는 자: _nvmf_tgt_disconnect_qpairs() iteration.
	 * 값 범위: 유효 포인터.
	 * 동기화: ctx 단일 소유, group->thread에서만 접근. */

	spdk_nvmf_poll_group_mod_done cpl_fn;
	/* [한국어] 모든 qpair 정리 완료 시 호출할 cb. NULL 가능.
	 * 설정자: 콜러가 지정 (예: subsystem pause 완료 통지).
	 * 읽는 자: 마지막 단계에서 cpl_fn(cpl_ctx, status) 호출.
	 * 값 범위: NULL 또는 유효 fn ptr.
	 * 동기화: ctx 단일 소유. */

	void *cpl_ctx;
	/* [한국어] cpl_fn에 전달할 사용자 컨텍스트.
	 * 설정자: 콜러.
	 * 읽는 자: cpl_fn 호출 시 인자.
	 * 값 범위: 임의 포인터.
	 * 동기화: ctx 단일 소유. */
};

/*
 * [한국어]
 * nvmf_tgt_find_referral - target의 referral 리스트에서 trid가 일치하는 항목 검색
 *
 * @tgt: 검색 대상 target
 * @trid: 비교할 transport id (subnqn/trtype/adrfam/traddr/trsvcid 모두 일치 검사)
 * @return: 일치하는 referral 포인터, 없으면 NULL
 *
 * spdk_nvme_transport_id_compare()는 모든 필드의 정확 일치를 검사한다(0 반환).
 * 실행 컨텍스트: application thread (호출자 모두 assert로 강제).
 */
static struct spdk_nvmf_referral *
nvmf_tgt_find_referral(struct spdk_nvmf_tgt *tgt,
		       const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_referral *referral;             /* [한국어] TAILQ 순회용 */

	TAILQ_FOREACH(referral, &tgt->referrals, link) { /* [한국어] target의 referrals 리스트 전체 순회 - O(n) */
		if (spdk_nvme_transport_id_compare(&referral->trid, trid) == 0) { /* [한국어] 모든 trid 필드 동일 시 0 반환 */
			return referral;                 /* [한국어] 매칭된 referral 반환 */
		}
	}

	return NULL;                                     /* [한국어] 매칭 없음 - 호출자(add)가 새로 만들지 결정 */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_add_referral - target에 NVMe-oF referral entry 추가 (Discovery 페이지에 광고)
 *
 * @tgt: referral을 추가할 target
 * @uopts: 사용자가 전달한 referral 옵션 (size 필드 ABI 호환성 보장)
 * @return: 0 성공 또는 이미 존재, -EINVAL 잘못된 NQN, -ENOMEM
 *
 * NVMe-oF 1.1+ Discovery Log Page에는 다른 Discovery/Storage subsystem으로 호스트를 안내하는
 * "Referral" entry를 포함할 수 있다. 본 함수는 RPC nvmf_discovery_add_referral에서 호출되어
 * 새 referral 객체를 만들고 entry 미리 채운 뒤 referrals 리스트에 추가한다. subnqn 미지정 시
 * Discovery NQN으로 기본 설정. 추가 후 spdk_nvmf_send_discovery_log_notice로 모든 discovery
 * ctrlr에 변경 AEN을 발사한다.
 *
 * uopts->size 처리: ABI 확장을 위해 사용자 구조체 크기와 본 빌드의 sizeof(opts) 중 작은 값만 복사 -
 * 추후 필드 추가 시 구버전/신버전 호환성 유지.
 *
 * 실행 컨텍스트: application thread (assert).
 *
 * 호출 체인:
 *   RPC nvmf_discovery_add_referral -> [본 함수] -> spdk_nvmf_send_discovery_log_notice
 */
int
spdk_nvmf_tgt_add_referral(struct spdk_nvmf_tgt *tgt,
			   const struct spdk_nvmf_referral_opts *uopts)
{
	struct spdk_nvmf_referral *referral;             /* [한국어] 새로 만들 또는 검색된 referral */
	struct spdk_nvmf_referral_opts opts = {};        /* [한국어] uopts 복사본 - 본 빌드 사이즈 기준 정규화 */
	struct spdk_nvme_transport_id *trid = &opts.trid; /* [한국어] opts 안의 trid 별칭 - 가독성용 */

	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] referrals 리스트는 app thread에서만 변경 - cross-thread 진입 방지 */

	memcpy(&opts, uopts, spdk_min(uopts->size, sizeof(opts))); /* [한국어] ABI 호환 size-aware 복사 - 누락 필드는 0 유지 */
	if (trid->subnqn[0] == '\0') {                   /* [한국어] subnqn 미지정 시 표준 Discovery NQN으로 기본 */
		snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN); /* [한국어] "nqn.2014-08.org.nvmexpress.discovery" 채움 */
	}

	if (!nvmf_nqn_is_valid(trid->subnqn)) {          /* [한국어] NQN 형식/길이 유효성 검사 (NVMe-oF 1.x NQN spec) */
		SPDK_ERRLOG("Invalid subsystem NQN\n");
		return -EINVAL;
	}

	/* If the entry already exists, just ignore it. */
	if (nvmf_tgt_find_referral(tgt, trid)) {         /* [한국어] 동일 trid 중복 추가는 멱등적으로 성공 처리 (idempotent RPC) */
		return 0;
	}

	referral = calloc(1, sizeof(*referral));         /* [한국어] zero-init 할당 */
	if (!referral) {                                 /* [한국어] OOM */
		SPDK_ERRLOG("Failed to allocate memory for a referral\n");
		return -ENOMEM;
	}

	referral->tgt = tgt;                             /* [한국어] back-pointer - host 변경 시 send_discovery_log_notice 호출용 */
	referral->entry.subtype = nvmf_nqn_is_discovery(trid->subnqn) ? /* [한국어] Discovery NQN이면 Discovery subtype, 아니면 NVM subtype */
				  SPDK_NVMF_SUBTYPE_DISCOVERY :
				  SPDK_NVMF_SUBTYPE_NVME;
	referral->entry.treq.secure_channel = opts.secure_channel ? /* [한국어] TREQ 필드 - secure channel 필수/선택 광고 (TLS 강제 여부) */
					      SPDK_NVMF_TREQ_SECURE_CHANNEL_REQUIRED :
					      SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED;
	referral->entry.cntlid = 0xffff;                 /* [한국어] Dynamic Controller ID - 호스트가 connect 시 동적 할당 의미 */
	referral->entry.trtype = trid->trtype;           /* [한국어] 트랜스포트 타입 (RDMA/TCP/FC 등) */
	referral->entry.adrfam = trid->adrfam;           /* [한국어] 주소 패밀리 (IPv4/IPv6/IB/FC) */
	memcpy(&referral->trid, trid, sizeof(struct spdk_nvme_transport_id)); /* [한국어] 원본 trid 보존 - 검색/제거 키 */
	spdk_strcpy_pad(referral->entry.subnqn, trid->subnqn, sizeof(trid->subnqn), '\0'); /* [한국어] entry의 subnqn은 NUL 패딩 (NVMe spec 요구) */
	spdk_strcpy_pad(referral->entry.trsvcid, trid->trsvcid, sizeof(referral->entry.trsvcid), ' '); /* [한국어] trsvcid는 space 패딩 (스펙 - ASCII trailing spaces) */
	spdk_strcpy_pad(referral->entry.traddr, trid->traddr, sizeof(referral->entry.traddr), ' '); /* [한국어] traddr도 space 패딩 */

	referral->allow_any_host = opts.allow_any_host;  /* [한국어] 호스트 화이트리스트 무시 플래그 - true면 모든 호스트에 광고 */
	TAILQ_INIT(&referral->hosts);                    /* [한국어] 호스트 화이트리스트 초기화 */

	TAILQ_INSERT_HEAD(&tgt->referrals, referral, link); /* [한국어] target referrals 리스트 prepend - 순서 무관 */
	spdk_nvmf_send_discovery_log_notice(tgt, NULL);  /* [한국어] genctr 증가 + 모든 discovery ctrlr에 AEN - hostnqn=NULL이므로 모든 호스트 대상 */

	return 0;                                        /* [한국어] 성공 */
}

/*
 * [한국어]
 * nvmf_referral_remove_host - referral 안의 host whitelist 항목 제거 (내부)
 *
 * @referral: 대상 referral
 * @host: 제거할 host 노드
 *
 * 호출자가 이미 host 검색을 마쳤다고 가정. 단순 list remove + free.
 * 실행 컨텍스트: application thread (assert).
 */
static void
nvmf_referral_remove_host(struct spdk_nvmf_referral *referral, struct spdk_nvmf_host *host)
{
	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] hosts TAILQ는 app thread에서만 변경 */

	TAILQ_REMOVE(&referral->hosts, host, link);      /* [한국어] 리스트에서 분리 - link 멤버 사용 */
	free(host);                                      /* [한국어] 노드 자체 해제 */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_remove_referral - target에서 referral 제거 (RPC nvmf_discovery_remove_referral)
 *
 * @tgt: 대상 target
 * @uopts: trid를 담은 옵션 (size 호환성)
 * @return: 0 성공, -ENOENT 미존재
 *
 * trid로 referral 검색 → host whitelist 모두 해제 → tgt에서 제거 → discovery AEN 발사 → free.
 * subnqn 미지정 시 Discovery NQN으로 보정.
 *
 * 호출 체인:
 *   RPC -> [본 함수] -> nvmf_tgt_find_referral -> spdk_nvmf_send_discovery_log_notice
 */
int
spdk_nvmf_tgt_remove_referral(struct spdk_nvmf_tgt *tgt,
			      const struct spdk_nvmf_referral_opts *uopts)
{
	struct spdk_nvmf_referral *referral;             /* [한국어] 검색해 올 대상 */
	struct spdk_nvmf_referral_opts opts = {};        /* [한국어] uopts 정규화 사본 */
	struct spdk_nvme_transport_id *trid = &opts.trid; /* [한국어] trid 별칭 */
	struct spdk_nvmf_host *host, *host_tmp;          /* [한국어] FOREACH_SAFE 순회용 (next 보존) */

	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] app thread 강제 */

	memcpy(&opts, uopts, spdk_min(uopts->size, sizeof(opts))); /* [한국어] ABI 호환 size-aware 복사 */
	if (trid->subnqn[0] == '\0') {                   /* [한국어] subnqn 미지정 시 Discovery NQN 적용 (find 매칭 일관성) */
		snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
	}

	referral = nvmf_tgt_find_referral(tgt, &opts.trid); /* [한국어] trid 매칭 referral 검색 */
	if (referral == NULL) {                          /* [한국어] 미존재 시 ENOENT */
		return -ENOENT;
	}

	TAILQ_FOREACH_SAFE(host, &referral->hosts, link, host_tmp) { /* [한국어] referral 소유 host whitelist 모두 해제 - SAFE는 free 중에도 next 안전 */
		nvmf_referral_remove_host(referral, host);
	}

	TAILQ_REMOVE(&tgt->referrals, referral, link);   /* [한국어] target에서 분리 */
	spdk_nvmf_send_discovery_log_notice(tgt, NULL);  /* [한국어] 모든 호스트에 AEN - referral 변경은 모든 ctrlr가 봐야 함 */

	free(referral);                                  /* [한국어] referral 노드 자체 해제 */

	return 0;
}

/*
 * [한국어]
 * nvmf_referral_find_host - referral 안에서 hostnqn으로 host 검색 (내부)
 *
 * @referral: 대상 referral
 * @hostnqn: 검색할 host NQN 문자열
 * @return: 찾은 host 또는 NULL
 *
 * NQN 비교는 정확 일치(strcmp). 호출자(add/remove/host_allowed)에서 NQN 유효성 사전 검증.
 */
static struct spdk_nvmf_host *
nvmf_referral_find_host(struct spdk_nvmf_referral *referral, const char *hostnqn)
{
	struct spdk_nvmf_host *host;                     /* [한국어] 순회용 */

	TAILQ_FOREACH(host, &referral->hosts, link) {    /* [한국어] referral의 host whitelist 전체 순회 */
		if (strcmp(hostnqn, host->nqn) == 0) {   /* [한국어] NQN 정확 일치 (NQN은 case-sensitive 사양 권장) */
			return host;
		}
	}

	return NULL;                                     /* [한국어] 미존재 */
}

/*
 * [한국어]
 * spdk_nvmf_referral_add_host - referral의 host whitelist에 hostnqn 추가
 *
 * @referral: 대상 referral
 * @hostnqn: 추가할 호스트 NQN
 * @return: 0 성공, -EINVAL 잘못된 인자, -EEXIST 중복, -ENOMEM 할당 실패
 *
 * referral.allow_any_host=false인 경우에만 의미 있음. 추가 후 해당 호스트에만 AEN 발사.
 *
 * 호출 체인:
 *   RPC nvmf_discovery_referral_add_host -> [본 함수] -> spdk_nvmf_send_discovery_log_notice(... hostnqn)
 */
int
spdk_nvmf_referral_add_host(struct spdk_nvmf_referral *referral,
			    const char *hostnqn)
{
	struct spdk_nvmf_host *host;                     /* [한국어] 새로 만들 host 노드 */

	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] app thread 강제 */

	if (referral == NULL) {                          /* [한국어] 방어적 NULL 체크 - RPC 인자 오류 가능 */
		return -EINVAL;
	}

	if (!nvmf_nqn_is_valid(hostnqn)) {               /* [한국어] NQN 형식 검사 */
		return -EINVAL;
	}

	if (nvmf_referral_find_host(referral, hostnqn)) { /* [한국어] 중복 추가 거부 */
		return -EEXIST;
	}

	host = calloc(1, sizeof(*host));                 /* [한국어] zero-init 할당 */
	if (!host) {                                     /* [한국어] OOM */
		return -ENOMEM;
	}

	snprintf(host->nqn, sizeof(host->nqn), "%s", hostnqn); /* [한국어] NQN 복사 (최대 SPDK_NVMF_NQN_MAX_LEN+1) */
	TAILQ_INSERT_HEAD(&referral->hosts, host, link); /* [한국어] whitelist에 추가 */

	spdk_nvmf_send_discovery_log_notice(referral->tgt, hostnqn); /* [한국어] 해당 호스트에만 AEN - 권한 변경은 그 호스트만 영향 */
	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_referral_remove_host - referral의 host whitelist에서 제거
 *
 * @referral: 대상 referral
 * @hostnqn: 제거할 호스트 NQN
 * @return: 0 성공, -EINVAL 잘못된 인자, -ENOENT 미등록
 */
int
spdk_nvmf_referral_remove_host(struct spdk_nvmf_referral *referral,
			       const char *hostnqn)
{
	struct spdk_nvmf_host *host;                     /* [한국어] 검색해 올 대상 */

	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] app thread 강제 */

	if (referral == NULL) {                          /* [한국어] 방어적 NULL 체크 */
		return -EINVAL;
	}

	if (!nvmf_nqn_is_valid(hostnqn)) {               /* [한국어] NQN 검증 */
		return -EINVAL;
	}

	host = nvmf_referral_find_host(referral, hostnqn); /* [한국어] 검색 */
	if (!host) {                                     /* [한국어] 미등록 호스트 */
		return -ENOENT;
	}

	nvmf_referral_remove_host(referral, host);       /* [한국어] 리스트에서 분리 + free */

	spdk_nvmf_send_discovery_log_notice(referral->tgt, hostnqn); /* [한국어] 해당 호스트에만 AEN - 이제 referral 안 보임 */
	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_referral_set_allow_any_host - whitelist 무시 플래그 설정
 *
 * @referral: 대상 referral
 * @allow_any_host: true면 모든 호스트에 referral 광고
 * @return: 0 (변경 없음/변경 적용 모두 성공)
 *
 * 값이 동일하면 no-op 반환 (불필요한 AEN 회피).
 */
int
spdk_nvmf_referral_set_allow_any_host(struct spdk_nvmf_referral *referral,
				      bool allow_any_host)
{
	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] app thread 강제 */

	if (referral->allow_any_host == allow_any_host) { /* [한국어] 변경이 없으면 AEN 발사 없이 즉시 반환 (idempotent) */
		return 0;
	}

	referral->allow_any_host = allow_any_host;       /* [한국어] 플래그 갱신 */

	spdk_nvmf_send_discovery_log_notice(referral->tgt, NULL); /* [한국어] 모든 호스트에 AEN - 누가 영향받을지 모르므로 broadcast */
	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_referral_get_allow_any_host - allow_any_host 플래그 조회
 *
 * @return: 현재 플래그 값
 *
 * 단순 getter. 실행 컨텍스트: app thread.
 */
bool
spdk_nvmf_referral_get_allow_any_host(struct spdk_nvmf_referral *referral)
{
	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] app thread 일관성 보장 */
	return referral->allow_any_host;                 /* [한국어] 단순 필드 read */
}

/*
 * [한국어]
 * spdk_nvmf_referral_host_allowed - 특정 호스트가 referral을 볼 수 있는지 검사
 *
 * @referral: 대상 referral
 * @hostnqn: 호스트 NQN
 * @return: true면 허용 (whitelist 무시 또는 등록된 호스트)
 *
 * Discovery Log Page 생성 시 호스트별 referral 노출 여부 결정에 사용된다.
 * NQN 형식 오류는 false (보안 측면 - 잘못된 NQN은 거부).
 */
bool
spdk_nvmf_referral_host_allowed(struct spdk_nvmf_referral *referral,
				const char *hostnqn)
{
	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] app thread 강제 */

	if (!nvmf_nqn_is_valid(hostnqn)) {               /* [한국어] 유효하지 않은 NQN은 즉시 거부 */
		return false;
	}

	return referral->allow_any_host || nvmf_referral_find_host(referral, hostnqn); /* [한국어] OR 단락평가 - allow_any_host=true면 검색 생략 */
}


/*
 * [한국어]
 * spdk_nvmf_referral_get_first_host - referral의 host whitelist 첫 항목
 *
 * @referral: 대상 referral
 * @return: 첫 host 또는 NULL (빈 리스트)
 *
 * RPC 응답 dump 등에서 호스트 목록 순회 시작점.
 */
struct spdk_nvmf_host *
spdk_nvmf_referral_get_first_host(struct spdk_nvmf_referral *referral)
{
	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] app thread - 순회 일관성 */
	return TAILQ_FIRST(&referral->hosts);            /* [한국어] TAILQ 매크로 - 첫 노드 또는 NULL */
}

/*
 * [한국어]
 * spdk_nvmf_referral_get_next_host - 다음 host whitelist 항목
 *
 * @referral: 대상 referral (현재 사용 안 됨 - 시그니처 일관성)
 * @prev_host: 이전 host
 * @return: 다음 host 또는 NULL
 */
struct spdk_nvmf_host *
spdk_nvmf_referral_get_next_host(struct spdk_nvmf_referral *referral,
				 struct spdk_nvmf_host *prev_host)
{
	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] app thread */
	return TAILQ_NEXT(prev_host, link);              /* [한국어] link 멤버 다음 노드 */
}


/*
 * [한국어]
 * spdk_nvmf_tgt_get_first_referral - target의 첫 referral
 *
 * RPC dump (예: nvmf_discovery_get_referrals) 시작점.
 */
struct spdk_nvmf_referral *
spdk_nvmf_tgt_get_first_referral(struct spdk_nvmf_tgt *tgt)
{
	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] app thread */
	return TAILQ_FIRST(&tgt->referrals);             /* [한국어] referrals TAILQ 첫 노드 */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_referral_get_next - 다음 referral
 */
struct spdk_nvmf_referral *
spdk_nvmf_tgt_referral_get_next(struct spdk_nvmf_tgt *tgt,
				struct spdk_nvmf_referral *prev_referral)
{
	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] app thread */
	return TAILQ_NEXT(prev_referral, link);          /* [한국어] 다음 노드 */
}

/*
 * [한국어]
 * spdk_nvmf_referral_get_trid - referral의 transport id 포인터 조회
 *
 * @return: 내부 trid 포인터 (호출자는 수정 금지 - const)
 *
 * RPC dump 시 traddr/trsvcid 등 출력에 사용.
 */
const struct spdk_nvme_transport_id *
spdk_nvmf_referral_get_trid(struct spdk_nvmf_referral *referral)
{
	assert(spdk_thread_is_app_thread(NULL));         /* [한국어] app thread */
	return &referral->trid;                          /* [한국어] 내부 보관 trid 포인터 - read-only 사용 */
}

/*
 * [한국어]
 * nvmf_qpair_set_state - qpair 상태 머신 전이 (단일 포인트)
 *
 * @qpair: 상태를 바꿀 qpair
 * @state: 새 상태 (UNINITIALIZED/CONNECTING/AUTHENTICATING/ACTIVE/DEACTIVATING/ERROR 등)
 *
 * qpair 상태는 항상 group->thread에서만 수정되어야 한다 - SPDK NVMe-oF는 qpair를 그 thread에
 * 고정(thread-local)해 lockless 운용하기 때문. 다른 thread가 직접 수정하면 race 발생.
 * 본 함수는 assert로 위반을 검출한다.
 */
void
nvmf_qpair_set_state(struct spdk_nvmf_qpair *qpair,
		     enum spdk_nvmf_qpair_state state)
{
	assert(qpair != NULL);                           /* [한국어] 호출자 NULL 인자 방어 */
	assert(qpair->group->thread == spdk_get_thread()); /* [한국어] thread affinity 검증 - cross-thread 상태 수정은 race */

	qpair->state = state;                            /* [한국어] 단일 포인트 write - 위 assert로 보호된 영역에서 lockless */
}

/*
 * Reset and clean up the poll group (I/O channel code will actually free the
 * group).
 */
/*
 * [한국어]
 * nvmf_tgt_cleanup_poll_group - poll group의 내부 자원만 정리 (group 자체 free는 상위 io_channel)
 *
 * @group: 정리할 poll group
 *
 * spdk_io_channel_register/unregister가 group(=ctx_buf) 자체의 메모리는 관리하므로,
 * 본 함수는 group 안에 동적으로 채워진 자원만 해제한다:
 *   1) 트랜스포트별 tgroup들 (qpair는 이미 다 빠진 상태) destroy.
 *   2) 모든 subsystem의 ns_info[].channel(spdk_io_channel) 해제 + ns_info 배열 free.
 *   3) sgroups 배열 자체 free.
 *   4) destroy_cb_fn 등록되어 있으면 호출 (예: spdk_nvmf_poll_group_destroy 비동기 완료 알림).
 *
 * 호출 체인:
 *   nvmf_tgt_destroy_poll_group / nvmf_tgt_create_poll_group(에러 rollback) -> [본 함수]
 */
static void
nvmf_tgt_cleanup_poll_group(struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_transport_poll_group *tgroup, *tmp; /* [한국어] FOREACH_SAFE 순회용 - destroy 중에도 next 안전 */
	struct spdk_nvmf_subsystem_poll_group *sgroup;   /* [한국어] subsystem poll group 슬롯 포인터 */
	uint32_t sid, nsid;                              /* [한국어] subsystem index, namespace index */

	TAILQ_FOREACH_SAFE(tgroup, &group->tgroups, link, tmp) { /* [한국어] 트랜스포트별 group 순회 */
		TAILQ_REMOVE(&group->tgroups, tgroup, link); /* [한국어] 리스트에서 분리 */
		nvmf_transport_poll_group_destroy(tgroup); /* [한국어] 트랜스포트 ops->poll_group_destroy 디스패치 - tgroup 자체도 free */
	}

	for (sid = 0; sid < group->num_sgroups; sid++) { /* [한국어] subsystem 슬롯 순회 (max_subsystems 크기) */
		sgroup = &group->sgroups[sid];           /* [한국어] sid번째 subsystem poll group */

		assert(sgroup != NULL);                  /* [한국어] 배열 내부 슬롯이므로 항상 비-NULL이어야 - 디버그 안전망 */

		for (nsid = 0; nsid < sgroup->num_ns; nsid++) { /* [한국어] subsystem 안 namespace 슬롯 순회 */
			if (sgroup->ns_info[nsid].channel) { /* [한국어] bdev I/O channel이 있으면 해제 - bdev_io_channel은 reactor당 1개 */
				spdk_put_io_channel(sgroup->ns_info[nsid].channel); /* [한국어] bdev에 io_channel 반환 - 참조 카운트 감소 */
				sgroup->ns_info[nsid].channel = NULL; /* [한국어] dangling 방지 */
			}
		}

		free(sgroup->ns_info);                   /* [한국어] ns_info 배열 free - subsystem.c가 add_namespace 시 alloc했음 */
	}

	free(group->sgroups);                            /* [한국어] sgroups 배열 자체 해제 - max_subsystems만큼 calloc된 것 */

	if (group->destroy_cb_fn) {                      /* [한국어] spdk_nvmf_poll_group_destroy()가 등록한 비동기 완료 cb */
		group->destroy_cb_fn(group->destroy_cb_arg, 0); /* [한국어] 호출자에게 cleanup 완료 통지 */
	}
}

/*
 * Callback to unregister a poll group from the target, and clean up its state.
 */
/*
 * [한국어]
 * nvmf_tgt_destroy_poll_group - spdk_io_device_register의 destroy 콜백 (각 thread에서 호출됨)
 *
 * @io_device: target 포인터 (등록 시 io_device로 사용됨)
 * @ctx_buf: poll_group 메모리 (sizeof(spdk_nvmf_poll_group) 만큼 io_channel가 할당)
 *
 * spdk_put_io_channel의 refcount가 0에 도달하면 SPDK io_channel이 본 콜백을 호출한다.
 * tgt->mutex로 poll_groups 리스트에서 제거 (cross-thread - 다른 reactor가 동시에 같은 작업 가능)
 * 후 cleanup_poll_group 위임.
 *
 * 실행 컨텍스트: io_channel을 마지막으로 put하는 thread (보통 group->thread).
 *
 * 호출 체인:
 *   spdk_put_io_channel(refcount=0) -> [본 함수] -> nvmf_tgt_cleanup_poll_group
 */
static void
nvmf_tgt_destroy_poll_group(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_tgt *tgt = io_device;           /* [한국어] register 시 등록한 io_device 복원 */
	struct spdk_nvmf_poll_group *group = ctx_buf;    /* [한국어] io_channel이 관리하는 group ctx 복원 */

	SPDK_DTRACE_PROBE1_TICKS(nvmf_destroy_poll_group, spdk_thread_get_id(group->thread)); /* [한국어] DTrace probe - group destroy 시점 추적 */

	pthread_mutex_lock(&tgt->mutex);                 /* [한국어] tgt->poll_groups TAILQ 보호 - cross-thread 등록/해제 race 방지 */
	TAILQ_REMOVE(&tgt->poll_groups, group, link);    /* [한국어] target의 poll_groups에서 제거 */
	tgt->num_poll_groups--;                          /* [한국어] count 감소 - 통계/round-robin 분배 기준 */
	pthread_mutex_unlock(&tgt->mutex);

	assert(!(tgt->state == NVMF_TGT_PAUSING || tgt->state == NVMF_TGT_RESUMING)); /* [한국어] pause/resume 트랜잭션 중에는 group 변경 금지 - 일관성 위반 방어 */
	nvmf_tgt_cleanup_poll_group(group);              /* [한국어] 내부 자원 정리 위임 */
}

/*
 * [한국어]
 * nvmf_poll_group_add_transport - poll group에 transport 결합 (tgroup 생성)
 *
 * @group: 대상 poll group
 * @transport: 결합할 transport
 * @return: 0 성공/이미 결합됨, -1 트랜스포트 측 생성 실패
 *
 * 동일 트랜스포트가 이미 결합되어 있으면 idempotent하게 0 반환. 아니면 트랜스포트 ops로
 * tgroup(spdk_nvmf_transport_poll_group)을 생성해 group->tgroups TAILQ에 추가.
 * tgroup은 트랜스포트별 polling/큐 관리 단위 (TCP는 socket epoll, RDMA는 CQ poll).
 *
 * 실행 컨텍스트: group->thread (create_poll_group 안에서, 또는 add_transport fan-out 안).
 */
static int
nvmf_poll_group_add_transport(struct spdk_nvmf_poll_group *group,
			      struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_transport_poll_group *tgroup = nvmf_get_transport_poll_group(group, transport); /* [한국어] 이미 결합된 tgroup 검색 */

	if (tgroup != NULL) {
		/* Transport already in the poll group */
		return 0;                                /* [한국어] 멱등 - fan-out에서 같은 트랜스포트 중복 호출 안전 */
	}

	tgroup = nvmf_transport_poll_group_create(transport, group); /* [한국어] 트랜스포트 ops->poll_group_create 디스패치 - TCP/RDMA별 자원 할당 */
	if (!tgroup) {                                   /* [한국어] 트랜스포트 측 자원 부족/에러 */
		SPDK_ERRLOG("Unable to create poll group for transport\n");
		return -1;
	}
	SPDK_DTRACE_PROBE2_TICKS(nvmf_transport_poll_group_create, transport,
				 spdk_thread_get_id(group->thread)); /* [한국어] DTrace probe - 어느 thread에서 어느 transport에 결합됐는지 추적 */

	tgroup->group = group;                           /* [한국어] back-pointer - tgroup이 자신의 부모 group을 알도록 */
	TAILQ_INSERT_TAIL(&group->tgroups, tgroup, link); /* [한국어] 결합 리스트에 등록 - 이후 polling iter에서 순회 */

	return 0;
}

/*
 * [한국어]
 * nvmf_tgt_create_poll_group - spdk_io_device_register의 create 콜백 (각 thread에서 호출됨)
 *
 * @io_device: target 포인터
 * @ctx_buf: poll_group 메모리 (io_channel이 sizeof(spdk_nvmf_poll_group) 만큼 alloc)
 * @return: 0 성공, 음수 실패 (io_channel 생성도 실패)
 *
 * spdk_get_io_channel(tgt) 호출 시 본 thread에 group이 없으면 본 콜백이 호출되어 1회성 초기화한다.
 * (1) 기본 필드 초기화 (tgroups/qpairs TAILQ, thread back-ptr, mutex).
 * (2) tgt에 등록된 모든 transport에 대해 tgroup 생성 (fan-in).
 * (3) sgroups 배열 할당 (max_subsystems 크기) + queued TAILQ 초기화.
 * (4) tgt의 모든 subsystem을 group에 추가 (ns_info 채움).
 * (5) tgt->poll_groups에 등록 + count 증가 (mutex 보호).
 * 어느 단계든 실패 시 cleanup_poll_group 호출 후 에러 반환.
 *
 * 실행 컨텍스트: spdk_get_io_channel을 호출한 thread (예: app thread).
 *
 * 호출 체인:
 *   spdk_nvmf_poll_group_create -> spdk_get_io_channel(tgt) -> [본 콜백]
 *      -> nvmf_poll_group_add_transport (각 transport)
 *      -> nvmf_poll_group_add_subsystem (각 subsystem, subsystem.c)
 */
static int
nvmf_tgt_create_poll_group(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_tgt *tgt = io_device;           /* [한국어] register 시 등록한 io_device 복원 */
	struct spdk_nvmf_poll_group *group = ctx_buf;    /* [한국어] io_channel이 alloc한 ctx 영역 (zero 초기화 보장 안 됨) */
	struct spdk_nvmf_transport *transport;           /* [한국어] target에 등록된 트랜스포트 순회용 */
	struct spdk_nvmf_subsystem *subsystem;           /* [한국어] subsystem 순회용 */
	struct spdk_thread *thread = spdk_get_thread();  /* [한국어] 현재 SPDK thread - group 소유 thread로 저장 */
	uint32_t i;                                      /* [한국어] sgroups 초기화 루프 인덱스 */
	int rc;                                          /* [한국어] add_transport 반환값 */

	group->tgt = tgt;                                /* [한국어] back-pointer */
	TAILQ_INIT(&group->tgroups);                     /* [한국어] 트랜스포트 group 리스트 초기화 */
	TAILQ_INIT(&group->qpairs);                      /* [한국어] qpair 리스트 초기화 (이 group이 폴링하는 qpair들) */
	group->thread = thread;                          /* [한국어] thread affinity 기록 - 모든 group 작업이 이 thread에서 직렬화 */
	pthread_mutex_init(&group->mutex, NULL);         /* [한국어] current_unassociated_qpairs 등 cross-thread 카운터 보호 */

	SPDK_DTRACE_PROBE1_TICKS(nvmf_create_poll_group, spdk_thread_get_id(thread)); /* [한국어] DTrace - 새 group 생성 시점 */

	TAILQ_FOREACH(transport, &tgt->transports, link) { /* [한국어] target에 이미 등록된 모든 transport와 결합 */
		rc = nvmf_poll_group_add_transport(group, transport);
		if (rc != 0) {                           /* [한국어] 한 transport라도 실패 시 부분 결과 정리 후 에러 */
			nvmf_tgt_cleanup_poll_group(group);
			return rc;
		}
	}

	group->num_sgroups = tgt->max_subsystems;        /* [한국어] sgroups 배열 크기 - subsystem ID는 0..max_subsystems-1 */
	group->sgroups = calloc(tgt->max_subsystems, sizeof(struct spdk_nvmf_subsystem_poll_group)); /* [한국어] subsystem 슬롯 배열 zero 할당 */
	if (!group->sgroups) {                           /* [한국어] OOM */
		nvmf_tgt_cleanup_poll_group(group);
		return -ENOMEM;
	}

	for (i = 0; i < tgt->max_subsystems; i++) {      /* [한국어] 각 subsystem 슬롯의 queued TAILQ 초기화 - 미할당 subsystem도 안전히 빈 리스트 보장 */
		TAILQ_INIT(&group->sgroups[i].queued);
	}

	for (subsystem = spdk_nvmf_subsystem_get_first(tgt); /* [한국어] tgt의 모든 subsystem을 본 group에 등록 - ns_info 채움 */
	     subsystem != NULL;
	     subsystem = spdk_nvmf_subsystem_get_next(subsystem)) {
		if (nvmf_poll_group_add_subsystem(group, subsystem, NULL, NULL) != 0) { /* [한국어] subsystem.c의 헬퍼 - bdev io_channel 할당 등 */
			nvmf_tgt_cleanup_poll_group(group);
			return -1;
		}
	}

	pthread_mutex_lock(&tgt->mutex);                 /* [한국어] cross-thread - 다른 reactor도 동시에 본 콜백 진입 가능 */
	tgt->num_poll_groups++;                          /* [한국어] 활성 group 카운트 - new_qpair 분배 가중치 */
	TAILQ_INSERT_TAIL(&tgt->poll_groups, group, link); /* [한국어] target의 group 리스트 끝에 추가 */
	pthread_mutex_unlock(&tgt->mutex);

	return 0;                                        /* [한국어] 성공 - io_channel 생성 완료 */
}

/*
 * [한국어]
 * _nvmf_tgt_disconnect_qpairs - poll group의 모든 qpair를 차례로 disconnect (재진입 가능)
 *
 * @ctx: nvmf_qpair_disconnect_many_ctx 포인터
 *
 * spdk_nvmf_qpair_disconnect는 비동기이므로 한 번에 모두 끝나지 않는다. 본 함수는
 * (1) 모든 qpair에 disconnect 시도, (2) 다 비었으면 io_channel 반환 + ctx free,
 * (3) 일부가 -EINPROGRESS면 자기 자신을 다시 send_msg해 재시도하는 패턴이다.
 * 이로써 단일 thread에서 lockless로 비동기 정리 루프를 구현.
 *
 * 실행 컨텍스트: group->thread.
 *
 * 호출 체인:
 *   nvmf_tgt_destroy_poll_group_qpairs -> [본 함수] -> spdk_nvmf_qpair_disconnect (각 qpair)
 *     -> (모든 qpair 정리되면) spdk_put_io_channel -> nvmf_tgt_destroy_poll_group cb
 *     -> (잔여 있으면) spdk_thread_send_msg(self) -> [본 함수 재진입]
 */
static void
_nvmf_tgt_disconnect_qpairs(void *ctx)
{
	struct spdk_nvmf_qpair *qpair, *qpair_tmp;       /* [한국어] FOREACH_SAFE 순회용 */
	struct nvmf_qpair_disconnect_many_ctx *qpair_ctx = ctx; /* [한국어] msg 인자 복원 */
	struct spdk_nvmf_poll_group *group = qpair_ctx->group; /* [한국어] 정리 대상 group */
	struct spdk_io_channel *ch;                      /* [한국어] group 종료 시 반환할 io_channel */
	int rc;                                          /* [한국어] disconnect 반환값 */

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, qpair_tmp) { /* [한국어] group의 모든 qpair 순회 (disconnect가 리스트에서 제거할 수 있어 SAFE 필요) */
		rc = spdk_nvmf_qpair_disconnect(qpair);  /* [한국어] qpair 단계별 종료 시작 - 동기 또는 -EINPROGRESS 반환 */
		if (rc && rc != -EINPROGRESS) {          /* [한국어] -EINPROGRESS는 정상(이미 진행 중) - 그 외 에러는 더 진행 못함 */
			break;
		}
	}

	if (TAILQ_EMPTY(&group->qpairs)) {               /* [한국어] 모든 qpair 정리 완료 - io_channel을 마지막으로 반환 */
		/* When the refcount from the channels reaches 0, nvmf_tgt_destroy_poll_group will be called. */
		ch = spdk_io_channel_from_ctx(group);    /* [한국어] group 포인터 -> io_channel 포인터 (ctx_buf 역참조) */
		spdk_put_io_channel(ch);                 /* [한국어] refcount 감소 - 0 도달 시 nvmf_tgt_destroy_poll_group 콜백 트리거 */
		free(qpair_ctx);                         /* [한국어] iter ctx 해제 - 더 이상 재시도 불필요 */
		return;
	}

	/* Some qpairs are in process of being disconnected. Send a message and try to remove them again */
	spdk_thread_send_msg(spdk_get_thread(), _nvmf_tgt_disconnect_qpairs, ctx); /* [한국어] 자기 자신에게 메시지 전송 - 다음 reactor turn에서 재시도 (busy-loop 회피) */
}

/*
 * [한국어]
 * nvmf_tgt_destroy_poll_group_qpairs - poll group destroy 시 qpair 정리 루프 시작
 *
 * @group: 정리할 poll group
 *
 * spdk_nvmf_poll_group_destroy의 진입점. ctx를 alloc 후 _nvmf_tgt_disconnect_qpairs를
 * 즉시 호출해 정리 루프 시작. 중간 단계의 io_channel 반환은 위 함수가 모든 qpair 정리 후 수행.
 */
static void
nvmf_tgt_destroy_poll_group_qpairs(struct spdk_nvmf_poll_group *group)
{
	struct nvmf_qpair_disconnect_many_ctx *ctx;      /* [한국어] iter 컨텍스트 */

	SPDK_DTRACE_PROBE1_TICKS(nvmf_destroy_poll_group_qpairs, spdk_thread_get_id(group->thread)); /* [한국어] DTrace - destroy 시작 시점 */

	ctx = calloc(1, sizeof(struct nvmf_qpair_disconnect_many_ctx)); /* [한국어] zero-init 할당 */
	if (!ctx) {                                      /* [한국어] OOM - 정리 루프 시작 불가, 누수 가능성 있지만 destroy 진행 (best-effort) */
		SPDK_ERRLOG("Failed to allocate memory for destroy poll group ctx\n");
		return;
	}

	ctx->group = group;                              /* [한국어] iter 대상 보존 */
	_nvmf_tgt_disconnect_qpairs(ctx);                /* [한국어] 즉시 첫 iteration 시작 */
}

/*
 * [한국어]
 * spdk_nvmf_set_custom_discovery_filter - custom discovery filter 콜백 등록
 *
 * @filter: (trid1, trid2)->bool 함수 포인터
 *
 * SPDK_NVMF_TGT_DISCOVERY_MATCH_CUSTOM 비트가 켜진 target은 nvmf_discovery_compare_trid()에서
 * 본 함수 포인터를 호출한다. NULL이면 MATCH_CUSTOM 비트 사용 불가 (tgt_create에서 거부).
 */
void
spdk_nvmf_set_custom_discovery_filter(spdk_nvmf_custom_discovery_filter filter)
{
	g_custom_discovery_filter = filter;              /* [한국어] 단순 전역 fn ptr 설정 - app 시작 시 1회 등록 가정 */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_create - NVMe-oF target 객체 생성 (외부 공개 API)
 *
 * @_opts: 사용자 옵션 구조체 (size 필드 ABI 호환성)
 * @return: 생성된 tgt 포인터, 실패 시 NULL
 *
 * 단계:
 *   1) 옵션 정규화 (max_subsystems/discovery_filter 기본값 반영, ABI size-aware copy).
 *   2) 이름 길이/유일성 검사. discovery filter CUSTOM 비트 호환성 검사.
 *   3) tgt 할당 후 모든 필드 초기화 (TAILQ, RB tree, mutex, bit_array).
 *   4) spdk_io_device_register로 reactor당 poll_group 자동 매핑 등록.
 *   5) g_nvmf_tgts에 추가 후 RUNNING 상태 진입.
 *
 * 실행 컨텍스트: application thread (g_nvmf_tgts는 lockless 가정).
 *
 * 호출 체인:
 *   RPC nvmf_create_target / app subsystem init -> [본 함수] -> spdk_io_device_register
 *     -> (이후 reactor당) nvmf_tgt_create_poll_group
 */
struct spdk_nvmf_tgt *
spdk_nvmf_tgt_create(struct spdk_nvmf_target_opts *_opts)
{
	struct spdk_nvmf_tgt *tgt, *tmp_tgt;             /* [한국어] tgt: 새로 만들 객체, tmp_tgt: 이름 중복 검사용 */
	struct spdk_nvmf_target_opts opts = {            /* [한국어] 옵션 기본값으로 초기화 - 이후 사용자 값 덮어쓰기 */
		.max_subsystems = SPDK_NVMF_DEFAULT_MAX_SUBSYSTEMS, /* [한국어] 1024개 subsystem 기본 */
		.discovery_filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY, /* [한국어] 기본은 모든 listener 노출 */
	};

	memcpy(&opts, _opts, _opts->size);               /* [한국어] ABI size-aware 복사 - _opts->size 만큼만 복사 (구버전 호환) */
	if (strnlen(opts.name, NVMF_TGT_NAME_MAX_LENGTH) == NVMF_TGT_NAME_MAX_LENGTH) { /* [한국어] 이름이 정확히 max 길이면 NUL 미종료 가능성 - 거부 */
		SPDK_ERRLOG("Provided target name exceeds the max length of %u.\n", NVMF_TGT_NAME_MAX_LENGTH);
		return NULL;
	}

	TAILQ_FOREACH(tmp_tgt, &g_nvmf_tgts, link) {     /* [한국어] 기존 target들과 이름 중복 검사 */
		if (!strncmp(opts.name, tmp_tgt->name, NVMF_TGT_NAME_MAX_LENGTH)) {
			SPDK_ERRLOG("Provided target name must be unique.\n");
			return NULL;
		}
	}

	if ((opts.discovery_filter & SPDK_NVMF_TGT_DISCOVERY_MATCH_CUSTOM) && /* [한국어] CUSTOM 비트가 켜졌는데 */
	    !g_custom_discovery_filter) {                /* [한국어] 콜백이 등록 안 되어 있으면 호출 시 segfault - 사전 거부 */
		SPDK_ERRLOG("Custom discovery filter callback is NULL.\n");
		return NULL;
	}

	tgt = calloc(1, sizeof(*tgt));                   /* [한국어] zero-init 할당 */
	if (!tgt) {                                      /* [한국어] OOM */
		return NULL;
	}

	snprintf(tgt->name, NVMF_TGT_NAME_MAX_LENGTH, "%s", opts.name); /* [한국어] 이름 복사 (자동 NUL termination) */

	if (!opts.max_subsystems) {                      /* [한국어] 0이면 기본값 사용 - 사용자가 명시적으로 0을 줘도 안전 */
		tgt->max_subsystems = SPDK_NVMF_DEFAULT_MAX_SUBSYSTEMS;
	} else {
		tgt->max_subsystems = opts.max_subsystems;
	}

	tgt->crdt[0] = opts.crdt[0];                     /* [한국어] Command Retry Delay Time 1 (CRDT1) - 호스트에 전달할 retry delay (NVMe spec) */
	tgt->crdt[1] = opts.crdt[1];                     /* [한국어] CRDT2 */
	tgt->crdt[2] = opts.crdt[2];                     /* [한국어] CRDT3 - 각 인덱스는 다른 retry 정책에 사용됨 */
	tgt->discovery_filter = opts.discovery_filter;   /* [한국어] discovery 매칭 정책 비트마스크 보존 */
	tgt->discovery_genctr = 0;                       /* [한국어] generation counter 시작값 - 변경 시 증가 */
	tgt->dhchap_digests = opts.dhchap_digests;       /* [한국어] DH-HMAC-CHAP 인증의 허용 digest 알고리즘 비트마스크 (SHA-256/384/512) */
	tgt->dhchap_dhgroups = opts.dhchap_dhgroups;     /* [한국어] DH 그룹 비트마스크 (FFDHE2048/3072/...) */
	TAILQ_INIT(&tgt->transports);                    /* [한국어] 트랜스포트 리스트 초기화 (TCP/RDMA 추후 add) */
	TAILQ_INIT(&tgt->poll_groups);                   /* [한국어] poll group 리스트 (reactor당 1개씩) */
	TAILQ_INIT(&tgt->referrals);                     /* [한국어] referral 리스트 */
	tgt->num_poll_groups = 0;                        /* [한국어] 통계/round-robin 분배 카운터 */

	tgt->subsystem_ids = spdk_bit_array_create(tgt->max_subsystems); /* [한국어] subsystem ID 풀 - 0..max-1 비트 할당/해제 */
	if (tgt->subsystem_ids == NULL) {                /* [한국어] 비트 배열 할당 실패 - tgt rollback */
		free(tgt);
		return NULL;
	}

	RB_INIT(&tgt->subsystems);                       /* [한국어] NQN 키 RB tree 초기화 - find_subsystem 시 O(log n) 검색 */

	pthread_mutex_init(&tgt->mutex, NULL);           /* [한국어] poll_groups 리스트 cross-thread 보호 */

	spdk_io_device_register(tgt,                     /* [한국어] target을 io_device로 등록 - 모든 SPDK thread가 자기 group을 자동 생성 */
				nvmf_tgt_create_poll_group, /* [한국어] thread별 group 최초 alloc 시 호출되는 cb */
				nvmf_tgt_destroy_poll_group, /* [한국어] 마지막 io_channel 반환 시 호출되는 cb */
				sizeof(struct spdk_nvmf_poll_group), /* [한국어] ctx_buf 크기 - io_channel이 이만큼 alloc */
				tgt->name);              /* [한국어] device 이름 (디버그/추적용) */

	tgt->state = NVMF_TGT_RUNNING;                   /* [한국어] 상태 머신: RUNNING (PAUSING/PAUSED/RESUMING은 옵션 변경 트랜잭션) */

	TAILQ_INSERT_HEAD(&g_nvmf_tgts, tgt, link);      /* [한국어] 전역 리스트 prepend */

	return tgt;                                      /* [한국어] 생성된 target 반환 - 호출자가 listen/add_transport 등으로 추가 구성 */
}

/*
 * [한국어]
 * _nvmf_tgt_destroy_next_transport - target의 트랜스포트들을 하나씩 비동기 destroy
 *
 * @ctx: spdk_nvmf_tgt 포인터
 *
 * destroy 시퀀스의 마지막 단계 - subsystem/poll group이 모두 정리된 후 호출되어 transports를
 * FIFO로 하나씩 destroy한다. 각 transport destroy는 비동기이므로 콜백 chaining으로 진행:
 *   transport[0] destroy -> 콜백에서 본 함수 재호출 -> transport[1] destroy -> ...
 *   리스트 비면 -> tgt 자체 free + 사용자 cb 호출.
 */
static void
_nvmf_tgt_destroy_next_transport(void *ctx)
{
	struct spdk_nvmf_tgt *tgt = ctx;                 /* [한국어] 콜백에 전달된 tgt 복원 */
	struct spdk_nvmf_transport *transport;           /* [한국어] 다음 정리할 트랜스포트 */

	if (!TAILQ_EMPTY(&tgt->transports)) {            /* [한국어] 정리할 transport가 남아 있으면 1개 처리 후 콜백으로 재진입 */
		transport = TAILQ_FIRST(&tgt->transports);
		TAILQ_REMOVE(&tgt->transports, transport, link); /* [한국어] 리스트에서 분리 - destroy 도중 재방문 방지 */
		spdk_nvmf_transport_destroy(transport, _nvmf_tgt_destroy_next_transport, tgt); /* [한국어] 비동기 destroy + 콜백으로 재귀 (실제 free는 transport.c) */
	} else {                                         /* [한국어] 모든 transport 정리됨 - 최종 cleanup */
		spdk_nvmf_tgt_destroy_done_fn *destroy_cb_fn = tgt->destroy_cb_fn; /* [한국어] 사용자 cb 보존 (free 후 사용 위해) */
		void *destroy_cb_arg = tgt->destroy_cb_arg;

		pthread_mutex_destroy(&tgt->mutex);      /* [한국어] mutex 자원 해제 */
		free(tgt);                               /* [한국어] tgt 본체 해제 - 이후 tgt 접근 금지 */

		if (destroy_cb_fn) {                     /* [한국어] 사용자 cb 호출 (RPC 응답 등) */
			destroy_cb_fn(destroy_cb_arg, 0);
		}
	}
}

/*
 * [한국어]
 * nvmf_tgt_destroy_cb - spdk_io_device_unregister의 unregister 콜백
 *
 * @io_device: target 포인터
 *
 * spdk_nvmf_tgt_destroy 호출 후 모든 reactor의 io_channel이 반환되면 SPDK io_device가 본 콜백
 * 호출. 단계: (1) referrals 모두 free, (2) mDNS publish 중지, (3) 모든 subsystem destroy
 * (subsystem destroy도 비동기 - -EINPROGRESS 시 본 함수가 다시 호출되어 다음 subsystem 처리),
 * (4) subsystem_ids bit_array 해제, (5) transport destroy 시퀀스 시작.
 *
 * 실행 컨텍스트: io_device unregister가 호출된 thread.
 */
static void
nvmf_tgt_destroy_cb(void *io_device)
{
	struct spdk_nvmf_tgt *tgt = io_device;           /* [한국어] tgt 복원 */
	struct spdk_nvmf_subsystem *subsystem, *subsystem_next; /* [한국어] subsystem 순회 시 free 안전을 위한 next 사전 캡처 */
	int rc;                                          /* [한국어] subsystem destroy 반환 */
	struct spdk_nvmf_referral *referral;             /* [한국어] referral 일괄 정리 */

	while ((referral = TAILQ_FIRST(&tgt->referrals))) { /* [한국어] referral은 동기적으로 즉시 정리 가능 */
		TAILQ_REMOVE(&tgt->referrals, referral, link);
		free(referral);
	}

	nvmf_tgt_stop_mdns_prr(tgt);                     /* [한국어] mDNS publish 중지 - Avahi 자원 해제 (또는 stub no-op) */

	/* We will be freeing subsystems in this loop, so we always need to get the next one
	 * ahead of time, since we can't call get_next() on a subsystem that's been freed.
	 */
	for (subsystem = spdk_nvmf_subsystem_get_first(tgt), /* [한국어] 첫 subsystem과 그 다음 subsystem 동시 캡처 */
	     subsystem_next = spdk_nvmf_subsystem_get_next(subsystem);
	     subsystem != NULL;
	     subsystem = subsystem_next,             /* [한국어] 다음 iteration: 이미 free된 subsystem에 get_next 호출 회피 */
	     subsystem_next = spdk_nvmf_subsystem_get_next(subsystem_next)) {
		nvmf_subsystem_remove_all_listeners(subsystem, true); /* [한국어] subsystem의 모든 listener 정리 (true=함께 free) */

		rc = spdk_nvmf_subsystem_destroy(subsystem, nvmf_tgt_destroy_cb, tgt); /* [한국어] subsystem destroy - 완료 시 본 함수 재호출 */
		if (rc) {
			if (rc == -EINPROGRESS) {        /* [한국어] 비동기 진행 중 - 콜백으로 재진입할 것이므로 여기서 즉시 return */
				/* If rc is -EINPROGRESS, nvmf_tgt_destroy_cb will be called again when subsystem #i
				 * is destroyed, nvmf_tgt_destroy_cb will continue to destroy other subsystems if any */
				return;
			} else {                         /* [한국어] 동기 에러 - best-effort로 다음 subsystem 진행 */
				SPDK_ERRLOG("Failed to destroy subsystem %s, rc %d\n", subsystem->subnqn, rc);
			}
		}
	}
	spdk_bit_array_free(&tgt->subsystem_ids);        /* [한국어] subsystem ID 비트 풀 해제 - 모든 subsystem 정리 후 안전 */
	_nvmf_tgt_destroy_next_transport(tgt);           /* [한국어] 마지막으로 transport 정리 시퀀스 시작 (그 안에서 tgt free) */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_destroy - target 비동기 destroy 진입점 (외부 공개 API)
 *
 * @tgt: 종료할 target
 * @cb_fn: 모든 정리 완료 시 호출할 cb (NULL 허용)
 * @cb_arg: cb_fn 인자
 *
 * 실제 정리는 다단계 비동기로 진행: g_nvmf_tgts에서 즉시 제거 → io_device_unregister
 * (모든 io_channel 반환을 기다림) → nvmf_tgt_destroy_cb → subsystem/transport 정리 → free.
 *
 * pause/resume 트랜잭션 중 destroy는 일관성 위반이므로 assert로 거부.
 */
void
spdk_nvmf_tgt_destroy(struct spdk_nvmf_tgt *tgt,
		      spdk_nvmf_tgt_destroy_done_fn cb_fn,
		      void *cb_arg)
{
	assert(!(tgt->state == NVMF_TGT_PAUSING || tgt->state == NVMF_TGT_RESUMING)); /* [한국어] 트랜잭션 중 destroy 금지 */

	tgt->destroy_cb_fn = cb_fn;                      /* [한국어] 비동기 완료 cb 보존 */
	tgt->destroy_cb_arg = cb_arg;

	TAILQ_REMOVE(&g_nvmf_tgts, tgt, link);           /* [한국어] 전역 리스트에서 즉시 제거 - 새 RPC가 본 tgt를 못 찾도록 */

	spdk_io_device_unregister(tgt, nvmf_tgt_destroy_cb); /* [한국어] io_device 등록 해제 - 모든 io_channel 반환 후 cb 호출 */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_get_name - target 이름 조회
 *
 * @return: NUL 종료 문자열 (호출자는 수정 금지)
 */
const char *
spdk_nvmf_tgt_get_name(struct spdk_nvmf_tgt *tgt)
{
	return tgt->name;                                /* [한국어] 단순 필드 read - 이름은 생성 후 불변 */
}

/*
 * [한국어]
 * spdk_nvmf_get_tgt - 이름으로 target 검색 (이름 NULL이면 단일 target만 반환)
 *
 * @name: 이름 문자열 또는 NULL
 * @return: 매칭 target 또는 NULL
 *
 * 단일 target 사용 시 RPC 사용자가 이름을 생략해도 동작하도록 편의 처리.
 * target 2개 이상이면 이름 필수 (NULL 시 NULL 반환).
 */
struct spdk_nvmf_tgt *
spdk_nvmf_get_tgt(const char *name)
{
	struct spdk_nvmf_tgt *tgt;                       /* [한국어] 순회용 */
	uint32_t num_targets = 0;                        /* [한국어] 전체 target 개수 카운트 */

	TAILQ_FOREACH(tgt, &g_nvmf_tgts, link) {         /* [한국어] 전역 target 리스트 순회 */
		if (name) {                              /* [한국어] 이름이 주어지면 일치 검사 */
			if (!strncmp(tgt->name, name, NVMF_TGT_NAME_MAX_LENGTH)) {
				return tgt;              /* [한국어] 매칭 즉시 반환 */
			}
		}
		num_targets++;                           /* [한국어] 전체 카운트 (이름 미지정 단일 target 케이스용) */
	}

	/*
	 * special case. If there is only one target and
	 * no name was specified, return the only available
	 * target. If there is more than one target, name must
	 * be specified.
	 */
	if (!name && num_targets == 1) {                 /* [한국어] 단일 target일 때만 이름 생략 허용 */
		return TAILQ_FIRST(&g_nvmf_tgts);
	}

	return NULL;                                     /* [한국어] 미매칭 또는 모호한 케이스 */
}

/*
 * [한국어]
 * spdk_nvmf_get_first_tgt - 전역 target 리스트의 첫 항목
 */
struct spdk_nvmf_tgt *
spdk_nvmf_get_first_tgt(void)
{
	return TAILQ_FIRST(&g_nvmf_tgts);                /* [한국어] TAILQ_FIRST - NULL 가능 */
}

/*
 * [한국어]
 * spdk_nvmf_get_next_tgt - target 순회 다음 항목
 */
struct spdk_nvmf_tgt *
spdk_nvmf_get_next_tgt(struct spdk_nvmf_tgt *prev)
{
	return TAILQ_NEXT(prev, link);                   /* [한국어] link 멤버 다음 노드 */
}

/*
 * [한국어]
 * nvmf_write_nvme_subsystem_config - NVMe subsystem 설정을 JSON RPC 명령 스트림으로 직렬화
 *
 * @w: spdk_json_write_ctx (JSON 출력)
 * @subsystem: 직렬화할 NVMe subsystem (SUBTYPE_NVME 전용)
 *
 * SPDK의 "save_config" 패턴: 현재 런타임 설정을 RPC 명령 시퀀스로 출력해, 같은 명령을 재실행하면
 * 동일한 상태가 복원되도록 한다. 이 함수는 한 NVMe subsystem에 대해:
 *   1) nvmf_create_subsystem (NQN, allow_any_host, serial, model, max_ns, cntlid 범위, ANA, etc.)
 *   2) 각 host에 대해 nvmf_subsystem_add_host (DH-CHAP key 포함)
 *   3) 각 NS에 대해 nvmf_subsystem_add_ns (bdev_name, nguid, eui64, uuid, anagrpid, no_auto_visible)
 *   4) NS의 visible host에 대해 nvmf_ns_add_host
 *
 * Discovery subsystem(SPDK_NVMF_SUBTYPE_DISCOVERY)은 본 함수를 호출하지 않는다 (assert로 보호).
 *
 * 핵심 매크로 사용:
 *   - SPDK_STATIC_ASSERT: nguid/eui64 크기 컴파일 타임 검증.
 *   - from_be64: 빅엔디언 64비트를 호스트 바이트오더로 변환 (NVMe 식별자는 BE 저장).
 *
 * 실행 컨텍스트: RPC handler thread (보통 메인 thread). subsystem 자료는 read-only 접근.
 *
 * 호출 체인:
 *   spdk_nvmf_tgt_write_config_json -> nvmf_write_subsystem_config_json -> [본 함수]
 */
static void
nvmf_write_nvme_subsystem_config(struct spdk_json_write_ctx *w,
				 struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_host *host;                     /* [한국어] subsystem에 허용된 host 순회용 */
	struct spdk_nvmf_ns *ns;                         /* [한국어] subsystem의 NS 순회용 */
	struct spdk_nvmf_ns_opts ns_opts;                /* [한국어] NS 옵션 (size-aware copy로 채워짐) */
	uint32_t max_namespaces;                         /* [한국어] subsystem의 max_namespaces 값 */
	struct spdk_nvmf_transport *transport;           /* [한국어] 트랜스포트별 host dump 호출용 */

	assert(spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME); /* [한국어] NVMe subsystem 전용 - Discovery는 다른 경로 */

	/* { */
	spdk_json_write_object_begin(w);                 /* [한국어] outer object 시작 - {"method":..., "params":...} */
	spdk_json_write_named_string(w, "method", "nvmf_create_subsystem"); /* [한국어] RPC method 이름 - 재실행 시 같은 RPC 호출 */

	/*     "params" : { */
	spdk_json_write_named_object_begin(w, "params"); /* [한국어] params 중첩 object */
	spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem)); /* [한국어] subsystem 식별자 NQN */
	spdk_json_write_named_bool(w, "allow_any_host", spdk_nvmf_subsystem_get_allow_any_host(subsystem)); /* [한국어] 임의 호스트 허용 여부 */
	spdk_json_write_named_string(w, "serial_number", spdk_nvmf_subsystem_get_sn(subsystem)); /* [한국어] NVMe SN (Identify Controller에 표기) */
	spdk_json_write_named_string(w, "model_number", spdk_nvmf_subsystem_get_mn(subsystem)); /* [한국어] NVMe MN */

	max_namespaces = spdk_nvmf_subsystem_get_max_namespaces(subsystem); /* [한국어] subsystem이 지원하는 최대 NS 수 */
	if (max_namespaces != 0) {                       /* [한국어] 0이면 default(=무제한 또는 빌드타임 default) - JSON에 출력 안함 */
		spdk_json_write_named_uint32(w, "max_namespaces", max_namespaces);
	}

	spdk_json_write_named_uint32(w, "min_cntlid", spdk_nvmf_subsystem_get_min_cntlid(subsystem)); /* [한국어] 컨트롤러 ID 할당 범위 시작 */
	spdk_json_write_named_uint32(w, "max_cntlid", spdk_nvmf_subsystem_get_max_cntlid(subsystem)); /* [한국어] 컨트롤러 ID 할당 범위 끝 */
	spdk_json_write_named_bool(w, "ana_reporting", spdk_nvmf_subsystem_get_ana_reporting(subsystem)); /* [한국어] ANA reporting 활성 여부 (NVMe 1.4) */
	spdk_json_write_named_uint64(w, "max_discard_size_kib", subsystem->max_discard_size_kib); /* [한국어] Dataset Management Discard 명령 최대 크기 (KiB) */
	spdk_json_write_named_uint64(w, "max_write_zeroes_size_kib", subsystem->max_write_zeroes_size_kib); /* [한국어] Write Zeroes 최대 크기 (KiB) */
	spdk_json_write_named_bool(w, "passthrough", subsystem->passthrough); /* [한국어] passthrough 모드 (NVMe-oF 명령을 backing NVMe로 그대로 전달) */
	spdk_json_write_named_bool(w, "enable_nssr", subsystem->nssr_enabled); /* [한국어] NVM Subsystem Reset 지원 여부 */

	/*     } "params" */
	spdk_json_write_object_end(w);                   /* [한국어] params object 종료 */

	/* } */
	spdk_json_write_object_end(w);                   /* [한국어] outer object 종료 (nvmf_create_subsystem 명령 완성) */

	for (host = spdk_nvmf_subsystem_get_first_host(subsystem); host != NULL; /* [한국어] subsystem이 허용한 모든 host 순회 */
	     host = spdk_nvmf_subsystem_get_next_host(subsystem, host)) {

		spdk_json_write_object_begin(w);         /* [한국어] nvmf_subsystem_add_host 명령 시작 */
		spdk_json_write_named_string(w, "method", "nvmf_subsystem_add_host");

		/*     "params" : { */
		spdk_json_write_named_object_begin(w, "params");

		spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem)); /* [한국어] subsystem NQN */
		spdk_json_write_named_string(w, "host", spdk_nvmf_host_get_nqn(host)); /* [한국어] 허용 host NQN */
		if (host->dhchap_key != NULL) {          /* [한국어] DH-HMAC-CHAP 인증 - host 키 등록되어 있으면 출력 */
			spdk_json_write_named_string(w, "dhchap_key",
						     spdk_key_get_name(host->dhchap_key)); /* [한국어] keyring 이름만 출력 (실제 키 데이터는 보안상 출력 안함) */
		}
		if (host->dhchap_ctrlr_key != NULL) {    /* [한국어] DH-HMAC-CHAP 양방향 인증 - controller 키 */
			spdk_json_write_named_string(w, "dhchap_ctrlr_key",
						     spdk_key_get_name(host->dhchap_ctrlr_key));
		}
		TAILQ_FOREACH(transport, &subsystem->tgt->transports, link) { /* [한국어] 트랜스포트별 host 추가 옵션 dump */
			if (transport->ops->subsystem_dump_host != NULL) { /* [한국어] 트랜스포트가 host dump ops 제공하면 호출 (TCP는 PSK 등) */
				transport->ops->subsystem_dump_host(transport, subsystem, host->nqn, w);
			}
		}

		/*     } "params" */
		spdk_json_write_object_end(w);

		/* } */
		spdk_json_write_object_end(w);           /* [한국어] add_host 명령 완성 */
	}

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL; /* [한국어] subsystem의 모든 NS 순회 */
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		spdk_nvmf_ns_get_opts(ns, &ns_opts, sizeof(ns_opts)); /* [한국어] NS 옵션 size-aware copy - struct 신구 호환 */

		spdk_json_write_object_begin(w);         /* [한국어] nvmf_subsystem_add_ns 명령 시작 */
		spdk_json_write_named_string(w, "method", "nvmf_subsystem_add_ns");

		/*     "params" : { */
		spdk_json_write_named_object_begin(w, "params");

		spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));

		/*     "namespace" : { */
		spdk_json_write_named_object_begin(w, "namespace"); /* [한국어] namespace 중첩 object */

		spdk_json_write_named_uint32(w, "nsid", spdk_nvmf_ns_get_id(ns)); /* [한국어] NS 식별자 (1-based) */
		spdk_json_write_named_string(w, "bdev_name", spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns))); /* [한국어] backing bdev 이름 */

		if (ns->ptpl_file != NULL) {             /* [한국어] PTPL(Persist Through Power Loss) reservation 파일 (전원 차단 후에도 예약 보존) */
			spdk_json_write_named_string(w, "ptpl_file", ns->ptpl_file);
		}

		if (!spdk_mem_all_zero(ns_opts.nguid, sizeof(ns_opts.nguid))) { /* [한국어] NGUID(Namespace GUID, 16 byte)가 0이 아니면 출력 (NVMe 1.2) */
			SPDK_STATIC_ASSERT(sizeof(ns_opts.nguid) == sizeof(uint64_t) * 2, "size mismatch"); /* [한국어] 컴파일 타임 - NGUID는 16 byte */
			spdk_json_write_named_string_fmt(w, "nguid", "%016"PRIX64"%016"PRIX64, from_be64(&ns_opts.nguid[0]),
							 from_be64(&ns_opts.nguid[8])); /* [한국어] BE -> 호스트 변환 후 32자 hex 문자열 */
		}

		if (!spdk_mem_all_zero(ns_opts.eui64, sizeof(ns_opts.eui64))) { /* [한국어] EUI-64(IEEE Extended Unique Identifier, 8 byte) 0 아니면 출력 */
			SPDK_STATIC_ASSERT(sizeof(ns_opts.eui64) == sizeof(uint64_t), "size mismatch");
			spdk_json_write_named_string_fmt(w, "eui64", "%016"PRIX64, from_be64(&ns_opts.eui64));
		}

		if (!spdk_uuid_is_null(&ns_opts.uuid)) { /* [한국어] NS UUID(NVMe 1.3 NIDT 0x03)가 비어있지 않으면 출력 */
			spdk_json_write_named_uuid(w, "uuid",  &ns_opts.uuid); /* [한국어] 표준 UUID 문자열 형식으로 출력 */
		}

		if (spdk_nvmf_subsystem_get_ana_reporting(subsystem)) { /* [한국어] ANA 활성화된 subsystem만 anagrpid 출력 */
			spdk_json_write_named_uint32(w, "anagrpid", ns_opts.anagrpid);
		}

		spdk_json_write_named_bool(w, "no_auto_visible", !ns->always_visible); /* [한국어] always_visible 반전 - false면 모든 host에 자동 visible (default true) */

		/*     "namespace" */
		spdk_json_write_object_end(w);           /* [한국어] namespace object 종료 */

		/*     } "params" */
		spdk_json_write_object_end(w);

		/* } */
		spdk_json_write_object_end(w);           /* [한국어] add_ns 명령 완성 */

		TAILQ_FOREACH(host, &ns->hosts, link) {  /* [한국어] no_auto_visible NS의 visible host 등록 명령 출력 */
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "method", "nvmf_ns_add_host"); /* [한국어] NS-host 가시성 명령 */
			spdk_json_write_named_object_begin(w, "params");
			spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));
			spdk_json_write_named_uint32(w, "nsid", spdk_nvmf_ns_get_id(ns));
			spdk_json_write_named_string(w, "host", spdk_nvmf_host_get_nqn(host));
			spdk_json_write_object_end(w);
			spdk_json_write_object_end(w);
		}
	}
}

/*
 * [한국어]
 * nvmf_write_subsystem_config_json - subsystem(NVMe + listener)을 JSON RPC 스트림으로 직렬화
 *
 * @w: JSON 출력 컨텍스트
 * @subsystem: 직렬화할 subsystem
 *
 * 동작:
 *   1) NVMe subsystem이면 NVMe 본체 설정 직렬화 위임 (nvmf_write_nvme_subsystem_config).
 *   2) 모든 활성 listener에 대해 nvmf_subsystem_add_listener 명령 출력:
 *      - listen_address 객체 (trtype/traddr/trsvcid 등 trid)
 *      - 트랜스포트별 listen_dump_opts (TCP는 PSK, RDMA는 RKEY 옵션 등)
 *      - secure_channel, sock_impl 옵션
 *
 * Discovery subsystem은 NVMe 본체 직렬화는 건너뛰고 listener만 출력 (Discovery는 자동 생성).
 *
 * 호출 체인:
 *   spdk_nvmf_tgt_write_config_json -> [본 함수] -> nvmf_write_nvme_subsystem_config (NVMe만)
 */
static void
nvmf_write_subsystem_config_json(struct spdk_json_write_ctx *w,
				 struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_listener *listener;   /* [한국어] subsystem listener 순회용 */
	struct spdk_nvmf_transport *transport;           /* [한국어] listener 트랜스포트별 옵션 dump용 */

	if (spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME) { /* [한국어] NVMe subsystem만 본체 설정 출력 (Discovery는 메타데이터 자동 생성) */
		nvmf_write_nvme_subsystem_config(w, subsystem);
	}

	TAILQ_FOREACH(listener, &subsystem->listeners, link) { /* [한국어] subsystem의 모든 listener 순회 */
		if (!nvmf_subsystem_listener_is_active(listener)) { /* [한국어] inactive listener는 직렬화하지 않음 (transient state) */
			continue;
		}

		transport = listener->transport;         /* [한국어] listener가 속한 transport */

		spdk_json_write_object_begin(w);         /* [한국어] nvmf_subsystem_add_listener 명령 시작 */
		spdk_json_write_named_string(w, "method", "nvmf_subsystem_add_listener");

		/*     "params" : { */
		spdk_json_write_named_object_begin(w, "params");

		spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));

		spdk_json_write_named_object_begin(w, "listen_address"); /* [한국어] listener 주소 nested object */
		nvmf_transport_listen_dump_trid(listener->trid, w); /* [한국어] trtype/adrfam/traddr/trsvcid 직렬화 */
		spdk_json_write_object_end(w);
		if (transport->ops->listen_dump_opts) {  /* [한국어] 트랜스포트별 추가 옵션 (TCP는 PSK keyring, RDMA는 추가 plugin opts 등) */
			transport->ops->listen_dump_opts(transport, listener->trid, w);
		}

		spdk_json_write_named_bool(w, "secure_channel", listener->opts.secure_channel); /* [한국어] TLS 등 보안 채널 요구 여부 */

		if (listener->opts.sock_impl) {          /* [한국어] sock 구현체 명시 (posix/uring 등) - 지정한 경우만 */
			spdk_json_write_named_string(w, "sock_impl", listener->opts.sock_impl);
		}

		/*     } "params" */
		spdk_json_write_object_end(w);

		/* } */
		spdk_json_write_object_end(w);           /* [한국어] add_listener 명령 완성 */
	}
}

/*
 * [한국어]
 * spdk_nvmf_tgt_write_config_json - NVMe-oF target 전체 설정을 JSON RPC 스트림으로 직렬화 (외부 공개 API)
 *
 * @w: JSON 출력 컨텍스트
 * @tgt: 직렬화 대상 target
 *
 * SPDK save_config 패턴의 NVMe-oF 진입점. 출력 RPC 명령 순서:
 *   1) nvmf_set_max_subsystems (max_subsystems)
 *   2) nvmf_set_crdt (crdt[0..2] - Command Retry Delay Times, NVMe 1.4)
 *   3) 각 transport에 대해 nvmf_create_transport (트랜스포트 옵션 모두 포함)
 *   4) 각 referral에 대해 nvmf_discovery_add_referral
 *   5) 각 subsystem에 대해 nvmf_write_subsystem_config_json (NVMe + listener + host + NS + reservation)
 *
 * 결과 JSON을 RPC 서버에 같은 순서로 재생하면 동일 상태가 복원된다 (transport는 subsystem보다 먼저
 * 만들어져야 listener가 그 transport를 참조할 수 있으므로 순서 중요).
 *
 * 실행 컨텍스트: RPC handler thread (메인 thread). 모든 자료 read-only 접근.
 *
 * 호출 체인:
 *   RPC save_config / save_subsystem_config -> [본 함수]
 *     -> nvmf_transport_dump_opts (트랜스포트 옵션)
 *     -> nvmf_write_subsystem_config_json (각 subsystem)
 */
void
spdk_nvmf_tgt_write_config_json(struct spdk_json_write_ctx *w, struct spdk_nvmf_tgt *tgt)
{
	struct spdk_nvmf_subsystem *subsystem;           /* [한국어] subsystem 순회용 */
	struct spdk_nvmf_transport *transport;           /* [한국어] transport 순회용 */
	struct spdk_nvmf_referral *referral;             /* [한국어] referral 순회용 (Discovery 응답에 포함될 외부 reference) */

	spdk_json_write_object_begin(w);                 /* [한국어] nvmf_set_max_subsystems 명령 시작 */
	spdk_json_write_named_string(w, "method", "nvmf_set_max_subsystems");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint32(w, "max_subsystems", tgt->max_subsystems); /* [한국어] target이 보유한 최대 subsystem 수 */
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);                   /* [한국어] set_max_subsystems 명령 완성 */

	spdk_json_write_object_begin(w);                 /* [한국어] nvmf_set_crdt 명령 시작 */
	spdk_json_write_named_string(w, "method", "nvmf_set_crdt");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint32(w, "crdt1", tgt->crdt[0]); /* [한국어] CRDT1 - 첫 번째 retry delay (100ms 단위, NVMe 1.4) */
	spdk_json_write_named_uint32(w, "crdt2", tgt->crdt[1]); /* [한국어] CRDT2 */
	spdk_json_write_named_uint32(w, "crdt3", tgt->crdt[2]); /* [한국어] CRDT3 */
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);

	/* write transports */
	TAILQ_FOREACH(transport, &tgt->transports, link) { /* [한국어] target의 모든 transport 순회 */
		spdk_json_write_object_begin(w);         /* [한국어] nvmf_create_transport 명령 */
		spdk_json_write_named_string(w, "method", "nvmf_create_transport");
		nvmf_transport_dump_opts(transport, w, true); /* [한국어] 트랜스포트 옵션 모두 dump (true=trtype 포함) */
		spdk_json_write_object_end(w);
	}

	TAILQ_FOREACH(referral, &tgt->referrals, link) { /* [한국어] target의 모든 referral 순회 */
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "nvmf_discovery_add_referral"); /* [한국어] Discovery referral 추가 RPC */

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_object_begin(w, "address");
		nvmf_transport_listen_dump_trid(&referral->trid, w); /* [한국어] referral 주소 (trtype/traddr/trsvcid) */
		spdk_json_write_object_end(w);
		spdk_json_write_named_bool(w, "secure_channel",
					   referral->entry.treq.secure_channel ==
					   SPDK_NVMF_TREQ_SECURE_CHANNEL_REQUIRED); /* [한국어] treq 필드의 secure_channel 비트로 보안 채널 요구 판정 */
		spdk_json_write_named_string(w, "subnqn", referral->trid.subnqn); /* [한국어] referral 대상 subsystem NQN */
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	subsystem = spdk_nvmf_subsystem_get_first(tgt);  /* [한국어] subsystem 순회 시작 */
	while (subsystem) {                              /* [한국어] 모든 subsystem 직렬화 */
		nvmf_write_subsystem_config_json(w, subsystem); /* [한국어] subsystem 본체 + listener 출력 */
		subsystem = spdk_nvmf_subsystem_get_next(subsystem);
	}
}

/*
 * [한국어]
 * nvmf_listen_opts_copy - listen_opts 구조체를 ABI 호환 size-aware로 복사
 *
 * @opts: 복사 대상
 * @opts_src: 복사 원본
 * @opts_size: 사용자가 전달한 구조체 크기 (이 크기 안의 필드만 복사)
 *
 * SPDK는 공개 옵션 구조체에 size 필드를 두어 새 필드 추가 시 구버전 사용자 코드가 깨지지 않도록
 * 한다. 본 매크로 트릭(SET_FIELD)은 각 필드의 offsetof+sizeof가 opts_size 안에 들어갈 때만 복사.
 * SPDK_STATIC_ASSERT는 새 필드 추가 시 본 함수도 함께 갱신하도록 빌드 타임에 강제.
 */
static void
nvmf_listen_opts_copy(struct spdk_nvmf_listen_opts *opts,
		      const struct spdk_nvmf_listen_opts *opts_src, size_t opts_size)
{
	assert(opts);                                    /* [한국어] 대상 NULL 방어 */
	assert(opts_src);                                /* [한국어] 원본 NULL 방어 */

	opts->opts_size = opts_size;                     /* [한국어] 복사된 크기 기록 - 이후 호출자가 읽을 때 동일 정책 사용 */

#define SET_FIELD(field) \
    if (offsetof(struct spdk_nvmf_listen_opts, field) + sizeof(opts->field) <= opts_size) { \
                 opts->field = opts_src->field; \
    } \
/* [한국어] 필드별 size-aware 복사 매크로 - opts_size 범위 안에 필드가 들어갈 때만 복사 */

	SET_FIELD(transport_specific);                   /* [한국어] 트랜스포트별 추가 옵션 (JSON 객체) */
	SET_FIELD(secure_channel);                       /* [한국어] TLS 강제 여부 (NVMe-oF 1.1+) */
	SET_FIELD(ana_state);                            /* [한국어] ANA(Asymmetric Namespace Access) 초기 상태 */
	SET_FIELD(sock_impl);                            /* [한국어] 소켓 구현 선택 (posix/uring 등 - sock 추상화) */
#undef SET_FIELD

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_listen_opts) == 32, "Incorrect size"); /* [한국어] 빌드 타임 - 구조체 변경 시 본 함수 갱신 강제 */
}

/*
 * [한국어]
 * spdk_nvmf_listen_opts_init - listen_opts 기본값 초기화 (외부 공개 API)
 *
 * @opts: 초기화할 옵션 구조체
 * @opts_size: 사용자 빌드의 구조체 크기 (sizeof(spdk_nvmf_listen_opts)를 전달)
 *
 * 사용자가 명시적으로 호출 후 일부 필드만 덮어쓰는 패턴을 권장. 기본값: ANA Optimized.
 */
void
spdk_nvmf_listen_opts_init(struct spdk_nvmf_listen_opts *opts, size_t opts_size)
{
	struct spdk_nvmf_listen_opts opts_local = {};    /* [한국어] 본 빌드의 전체 사이즈로 zero-init - 모든 필드 0/false */

	/* local version of opts should have defaults set here */
	opts_local.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE; /* [한국어] ANA 기본 = Optimized (가장 활성 상태) */
	nvmf_listen_opts_copy(opts, &opts_local, opts_size); /* [한국어] 사용자 size 만큼 복사 */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_listen_ext - 지정 트랜스포트에서 listen 시작
 *
 * @tgt: 대상 target
 * @trid: 트랜스포트/주소/포트 정보 (trstring으로 트랜스포트 찾고 traddr/trsvcid로 bind)
 * @opts: listen 옵션 (NULL/opts_size==0 거부)
 * @return: 0 성공, -EINVAL 잘못된 인자, 트랜스포트별 음수 에러
 *
 * RPC nvmf_subsystem_add_listener / nvmf_listen_address_add 등의 진입점.
 * trid->trstring으로 트랜스포트 객체 검색 → 트랜스포트의 listen ops 호출.
 *
 * 호출 체인:
 *   RPC -> [본 함수] -> spdk_nvmf_tgt_get_transport -> spdk_nvmf_transport_listen
 *     -> 트랜스포트별 ops->listen (TCP는 socket bind/listen, RDMA는 cm_id bind 등)
 */
int
spdk_nvmf_tgt_listen_ext(struct spdk_nvmf_tgt *tgt, const struct spdk_nvme_transport_id *trid,
			 struct spdk_nvmf_listen_opts *opts)
{
	struct spdk_nvmf_transport *transport;           /* [한국어] 검색해 올 트랜스포트 */
	int rc;                                          /* [한국어] listen 결과 */
	struct spdk_nvmf_listen_opts opts_local = {};    /* [한국어] 정규화된 사본 - 호출자 opts에 의존하지 않음 */

	if (!opts) {                                     /* [한국어] 사용자 인자 NULL 방어 */
		SPDK_ERRLOG("opts should not be NULL\n");
		return -EINVAL;
	}

	if (!opts->opts_size) {                          /* [한국어] size=0이면 어떤 필드도 유효하지 않음 - 거부 */
		SPDK_ERRLOG("The opts_size in opts structure should not be zero\n");
		return -EINVAL;
	}

	transport = spdk_nvmf_tgt_get_transport(tgt, trid->trstring); /* [한국어] trstring("TCP"/"RDMA"/...)으로 트랜스포트 검색 */
	if (!transport) {                                /* [한국어] 사용자가 nvmf_create_transport를 먼저 안 했을 가능성 */
		SPDK_ERRLOG("Unable to find %s transport. The transport must be created first also make sure it is properly registered.\n",
			    trid->trstring);
		return -EINVAL;
	}

	nvmf_listen_opts_copy(&opts_local, opts, opts->opts_size); /* [한국어] 사용자 opts -> 로컬 사본 (ABI 호환) */
	rc = spdk_nvmf_transport_listen(transport, trid, &opts_local); /* [한국어] 트랜스포트별 listen 디스패치 */
	if (rc < 0) {
		SPDK_ERRLOG("Unable to listen on address '%s'\n", trid->traddr);
	}

	return rc;                                       /* [한국어] 트랜스포트 측 결과 그대로 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_stop_listen - 지정 트랜스포트의 listen 중단
 *
 * @tgt: 대상 target
 * @trid: 식별 trid
 * @return: 0 성공, -EINVAL 트랜스포트 미발견, 음수 트랜스포트 에러
 *
 * 호출 체인:
 *   RPC nvmf_listen_address_remove -> [본 함수] -> spdk_nvmf_transport_stop_listen
 */
int
spdk_nvmf_tgt_stop_listen(struct spdk_nvmf_tgt *tgt,
			  const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_transport *transport;           /* [한국어] 검색해 올 트랜스포트 */
	int rc;                                          /* [한국어] stop_listen 결과 */

	transport = spdk_nvmf_tgt_get_transport(tgt, trid->trstring); /* [한국어] trstring으로 트랜스포트 검색 */
	if (!transport) {                                /* [한국어] 미존재 - 잘못된 trstring */
		SPDK_ERRLOG("Unable to find %s transport. The transport must be created first also make sure it is properly registered.\n",
			    trid->trstring);
		return -EINVAL;
	}

	rc = spdk_nvmf_transport_stop_listen(transport, trid); /* [한국어] 트랜스포트별 stop_listen ops - 소켓 close, listener 자원 free */
	if (rc < 0) {
		SPDK_ERRLOG("Failed to stop listening on address '%s'\n", trid->traddr);
		return rc;
	}
	return 0;                                        /* [한국어] 성공 */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_add_transport_ctx - target에 transport를 fan-out 추가할 때의 컨텍스트
 *
 * spdk_for_each_channel(tgt, _nvmf_tgt_add_transport, ctx, _nvmf_tgt_add_transport_done):
 * 모든 reactor의 poll group에 본 ctx를 들고 visit하며 tgroup 결합. 한 곳이라도 실패하면 status를
 * 보존한 채 _nvmf_tgt_remove_transport로 rollback iteration을 시작.
 */
struct spdk_nvmf_tgt_add_transport_ctx {
	struct spdk_nvmf_tgt *tgt;
	/* [한국어] 추가 대상 target.
	 * 설정자: spdk_nvmf_tgt_add_transport 진입 시 저장.
	 * 읽는 자: rollback fan-out에서 spdk_for_each_channel(tgt, ...) 인자.
	 * 값 범위: 유효 포인터.
	 * 동기화: ctx 단일 소유. */

	struct spdk_nvmf_transport *transport;
	/* [한국어] 추가할 transport 객체.
	 * 설정자: 진입 시 저장.
	 * 읽는 자: _nvmf_tgt_add_transport(각 group에 결합), _nvmf_tgt_remove_transport(rollback 시 분리).
	 * 값 범위: 유효 포인터.
	 * 동기화: ctx 단일 소유. */

	spdk_nvmf_tgt_add_transport_done_fn cb_fn;
	/* [한국어] 완료/실패 통지 콜백.
	 * 설정자: 진입 시 사용자 cb 저장.
	 * 읽는 자: _nvmf_tgt_add_transport_done 또는 _nvmf_tgt_remove_transport_done에서 호출.
	 * 값 범위: 유효 fn ptr.
	 * 동기화: ctx 단일 소유. */

	void *cb_arg;
	/* [한국어] cb_fn에 전달할 사용자 컨텍스트.
	 * 동기화: ctx 단일 소유. */

	int status;
	/* [한국어] fan-out 도중 발생한 첫 에러 상태 보존 (rollback 후 cb_fn에 그대로 전달).
	 * 설정자: _nvmf_tgt_add_transport_done이 status≠0이면 저장.
	 * 읽는 자: _nvmf_tgt_remove_transport_done이 cb_fn(arg, ctx->status)에 사용.
	 * 동기화: ctx 단일 소유 (단일 thread iterator). */
};

/*
 * [한국어]
 * _nvmf_tgt_remove_transport_done - rollback fan-out 완료 콜백
 *
 * @i: io_channel iterator
 * @status: rollback iter 상태 (보통 0)
 *
 * add 도중 실패해 rollback iter를 돌린 결과 보고. 사용자 cb에 원래 add 실패 status 전달.
 */
static void
_nvmf_tgt_remove_transport_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_tgt_add_transport_ctx *ctx = spdk_io_channel_iter_get_ctx(i); /* [한국어] iter 시작 시 등록한 ctx 복원 */

	ctx->cb_fn(ctx->cb_arg, ctx->status);            /* [한국어] 원래 add 실패 status를 사용자에게 전달 (rollback 자체 status 무시) */
	free(ctx);                                       /* [한국어] ctx 해제 - 모든 fan-out 완료 */
}

/*
 * [한국어]
 * _nvmf_tgt_remove_transport - 각 poll group에서 ctx->transport에 해당하는 tgroup을 제거 (rollback)
 *
 * @i: io_channel iterator (현재 group의 channel)
 *
 * spdk_for_each_channel이 모든 reactor에 본 함수를 visit으로 호출. 각 group의 tgroups에서 해당
 * transport와 매칭되는 tgroup을 찾아 분리/destroy 후 다음 channel로 진행.
 *
 * 실행 컨텍스트: 각 channel의 group->thread (iter가 thread 디스패치).
 */
static void
_nvmf_tgt_remove_transport(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_tgt_add_transport_ctx *ctx = spdk_io_channel_iter_get_ctx(i); /* [한국어] iter ctx 복원 */
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i); /* [한국어] 현재 visit 중 channel */
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch); /* [한국어] channel ctx_buf -> group */
	struct spdk_nvmf_transport_poll_group *tgroup, *tmp; /* [한국어] FOREACH_SAFE 순회용 */

	TAILQ_FOREACH_SAFE(tgroup, &group->tgroups, link, tmp) { /* [한국어] group의 tgroups 순회 */
		if (tgroup->transport == ctx->transport) { /* [한국어] 추가 대상 transport와 일치하는 tgroup만 제거 */
			TAILQ_REMOVE(&group->tgroups, tgroup, link);
			nvmf_transport_poll_group_destroy(tgroup); /* [한국어] 트랜스포트별 destroy ops + tgroup free */
		}
	}

	spdk_for_each_channel_continue(i, 0);            /* [한국어] 다음 channel(다른 reactor)로 iter 진행 - 0=성공 */
}

/*
 * [한국어]
 * _nvmf_tgt_add_transport_done - add fan-out 완료 콜백
 *
 * @i: iter
 * @status: 0 성공 / 음수: 어느 channel에서 add 실패
 *
 * 성공 시: tgt->transports에 transport 등록 + 사용자 cb 호출 + ctx free.
 * 실패 시: rollback iter 시작 (이미 결합된 일부 group에서 tgroup 분리). rollback 완료 시점에
 * _nvmf_tgt_remove_transport_done이 사용자 cb 호출.
 */
static void
_nvmf_tgt_add_transport_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_tgt_add_transport_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	if (status) {                                    /* [한국어] add 도중 한 곳이라도 실패 - rollback 필요 */
		ctx->status = status;                    /* [한국어] 사용자에게 전달할 원본 status 보존 */
		spdk_for_each_channel(ctx->tgt,          /* [한국어] rollback iter 시작 - 모든 channel 재방문해 tgroup 제거 */
				      _nvmf_tgt_remove_transport,
				      ctx,
				      _nvmf_tgt_remove_transport_done);
		return;
	}

	ctx->transport->tgt = ctx->tgt;                  /* [한국어] transport->tgt back-pointer 설정 - 트랜스포트가 부모 target 인지 */
	TAILQ_INSERT_TAIL(&ctx->tgt->transports, ctx->transport, link); /* [한국어] target의 transport 리스트에 등록 */
	ctx->cb_fn(ctx->cb_arg, status);                 /* [한국어] 사용자에게 성공 통지 (status=0) */
	free(ctx);
}

/*
 * [한국어]
 * _nvmf_tgt_add_transport - 각 poll group에 transport 결합 (fan-out 단위 작업)
 *
 * @i: iter
 *
 * 각 reactor의 group에서 nvmf_poll_group_add_transport 호출. 실패 시 rc를 iter에 전달해
 * 전체 iter가 _done에서 rollback 분기로 진입하도록 한다.
 */
static void
_nvmf_tgt_add_transport(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_tgt_add_transport_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i); /* [한국어] 현재 group의 channel */
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch); /* [한국어] group 포인터 */
	int rc;

	rc = nvmf_poll_group_add_transport(group, ctx->transport); /* [한국어] 본 group에 tgroup 생성/결합 */
	spdk_for_each_channel_continue(i, rc);           /* [한국어] iter에 rc 전달 - 비-0이면 _done이 rollback 분기 */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_add_transport - target에 새 transport를 모든 reactor 일괄 결합 (외부 공개 API)
 *
 * @tgt: 대상 target
 * @transport: 추가할 transport
 * @cb_fn: 비동기 완료 cb
 * @cb_arg: cb_fn 인자
 *
 * 중복 추가 시 -EEXIST. fan-out은 spdk_for_each_channel로 모든 reactor에 디스패치되며 한 곳이라도
 * 실패하면 자동 rollback. 이 트랜잭션 패턴 덕분에 일관성이 유지된다.
 *
 * 호출 체인:
 *   RPC nvmf_create_transport -> spdk_nvmf_transport_create -> [본 함수]
 *      -> spdk_for_each_channel(_nvmf_tgt_add_transport, _nvmf_tgt_add_transport_done)
 */
void
spdk_nvmf_tgt_add_transport(struct spdk_nvmf_tgt *tgt,
			    struct spdk_nvmf_transport *transport,
			    spdk_nvmf_tgt_add_transport_done_fn cb_fn,
			    void *cb_arg)
{
	struct spdk_nvmf_tgt_add_transport_ctx *ctx;     /* [한국어] iter ctx */

	SPDK_DTRACE_PROBE2_TICKS(nvmf_tgt_add_transport, transport, tgt->name); /* [한국어] DTrace - 어떤 transport가 어떤 tgt에 추가되는지 */

	if (spdk_nvmf_tgt_get_transport(tgt, transport->ops->name)) { /* [한국어] 이름 중복 검사 - 같은 트랜스포트 두 번 추가 거부 */
		cb_fn(cb_arg, -EEXIST);
		return; /* transport already created */
	}

	ctx = calloc(1, sizeof(*ctx));                   /* [한국어] zero-init 할당 */
	if (!ctx) {                                      /* [한국어] OOM */
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->tgt = tgt;                                  /* [한국어] iter 단계마다 사용 - 대상 target */
	ctx->transport = transport;                      /* [한국어] 추가할 transport */
	ctx->cb_fn = cb_fn;                              /* [한국어] 완료 통지 cb */
	ctx->cb_arg = cb_arg;                            /* [한국어] 사용자 ctx */

	spdk_for_each_channel(tgt,                       /* [한국어] tgt(io_device)의 모든 channel 순회 (=각 reactor의 group) */
			      _nvmf_tgt_add_transport, /* [한국어] 각 channel에서 실행될 함수 */
			      ctx,                       /* [한국어] iter ctx (각 channel에서 spdk_io_channel_iter_get_ctx로 복원) */
			      _nvmf_tgt_add_transport_done); /* [한국어] 모든 channel 완료 후 호출되는 cb */
}

/*
 * [한국어]
 * struct nvmf_tgt_pause_ctx - target 폴링 일시 정지/재개 fan-out 컨텍스트
 *
 * 모든 reactor의 poll_group을 spdk_for_each_channel로 순회하며 트랜스포트별 poll_group을
 * pause/resume할 때, iter visit 함수가 thread를 넘나들기 때문에 사용자 cb 정보를 보존하는 ctx.
 * pause/resume이 동일한 ctx 구조를 공유한다 (resume에서도 nvmf_tgt_pause_ctx를 재사용).
 */
struct nvmf_tgt_pause_ctx {
	struct spdk_nvmf_tgt *tgt;
	/* [한국어] pause/resume 대상 target 포인터.
	 * 설정자: spdk_nvmf_tgt_{pause,resume}_polling가 calloc 직후 저장.
	 * 읽는 자: _done 콜백에서 tgt->state 전이에 사용.
	 * 값 범위: 유효한 spdk_nvmf_tgt 포인터 (NULL 불가).
	 * 동기화: ctx 단일 소유 — 한 호출이 끝날 때까지만 유효. */

	spdk_nvmf_tgt_pause_polling_cb_fn cb_fn;
	/* [한국어] 모든 fan-out 완료 후 호출되는 사용자 콜백.
	 * pause용/resume용이 시그니처는 동일해 동일 필드를 재사용한다.
	 * 설정자: 외부 공개 API에서 사용자가 전달한 cb_fn 보존.
	 * 읽는 자: _nvmf_tgt_{pause,resume}_polling_done이 호출.
	 * 값 범위: NULL일 수 있으나 실무상 비-NULL을 가정. */

	void *cb_arg;
	/* [한국어] cb_fn에 그대로 전달되는 사용자 컨텍스트 (RPC 응답 컨텍스트 등).
	 * 동기화: ctx 단일 소유. */
};

/*
 * [한국어]
 * _nvmf_tgt_pause_polling_done - pause fan-out 완료 콜백
 *
 * @i: spdk_for_each_channel iterator
 * @status: fan-out 도중 발생한 첫 에러 상태(0이면 성공)
 *
 * 모든 reactor의 poll_group에 대해 트랜스포트별 poll_group_pause를 적용한 뒤 호출된다.
 * tgt->state를 NVMF_TGT_PAUSING(전이 중)에서 NVMF_TGT_PAUSED(완전 정지)로 전이시키고,
 * 사용자 cb로 결과를 통지한 뒤 ctx를 해제한다.
 *
 * 실행 컨텍스트: spdk_for_each_channel를 시작했던 thread (본 파일의 외부 API에서 호출되었던 thread).
 *
 * 호출 체인:
 *   spdk_nvmf_tgt_pause_polling -> spdk_for_each_channel -> _nvmf_tgt_pause_polling (각 reactor)
 *     -> [본 함수] -> 사용자 cb_fn
 */
static void
_nvmf_tgt_pause_polling_done(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_tgt_pause_ctx *ctx = spdk_io_channel_iter_get_ctx(i); /* [한국어] iter 시작 시 등록한 ctx 복원 */

	ctx->tgt->state = NVMF_TGT_PAUSED;               /* [한국어] PAUSING -> PAUSED 전이 - 이후 resume까지 새 polling 활동 없음 */

	ctx->cb_fn(ctx->cb_arg, status);                 /* [한국어] 사용자에게 fan-out 결과 통지 (status 그대로 전달) */
	free(ctx);                                       /* [한국어] 마지막 단계 - ctx 해제 */
}

/*
 * [한국어]
 * _nvmf_tgt_pause_polling - 각 poll_group의 모든 transport poll_group을 일시 정지 (fan-out 단위)
 *
 * @i: spdk_for_each_channel iterator (현재 visit 중인 channel)
 *
 * spdk_for_each_channel이 모든 reactor에 본 함수를 디스패치한다. 각 group의 tgroups 리스트를 순회하며
 * 트랜스포트별 pause 콜백(예: TCP는 epoll 이벤트 정지)을 호출. polled-mode 환경에서 실시간 제어 평면이
 * 일시적으로 dataplane을 멈출 수 있게 해주며, 통상 라이브 마이그레이션/관리 작업 전에 사용된다.
 *
 * 실행 컨텍스트: 각 channel의 group->thread (iter가 thread별로 디스패치).
 *
 * 호출 체인:
 *   spdk_nvmf_tgt_pause_polling -> spdk_for_each_channel -> [본 함수] (각 reactor)
 *     -> nvmf_transport_poll_group_pause (트랜스포트별 ops->poll_group_pause)
 */
static void
_nvmf_tgt_pause_polling(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i); /* [한국어] 현재 visit 중인 channel (특정 reactor의 group) */
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch); /* [한국어] channel ctx_buf -> poll_group 복원 */
	struct spdk_nvmf_transport_poll_group *tgroup;   /* [한국어] 순회용 트랜스포트별 poll_group */

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {   /* [한국어] 본 group에 등록된 모든 transport poll_group 순회 */
		nvmf_transport_poll_group_pause(tgroup); /* [한국어] 트랜스포트별 ops->poll_group_pause 호출 - 트랜스포트 폴링 일시 정지 */
	}

	spdk_for_each_channel_continue(i, 0);            /* [한국어] 다음 channel(다른 reactor)로 iter 진행 - 0=성공 */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_pause_polling - target의 모든 reactor 폴링을 비동기로 일시 정지 (외부 공개 API)
 *
 * @tgt: 정지할 target
 * @cb_fn: 모든 reactor 정지 완료 후 호출되는 cb
 * @cb_arg: cb_fn에 전달할 사용자 컨텍스트
 * @return: 0 fan-out 시작 성공 / -EBUSY 이미 전이 중 / -EINVAL 비정상 상태 / -ENOMEM
 *
 * 상태 머신: RUNNING -> PAUSING (fan-out 시작) -> PAUSED (모든 reactor 정지 완료, _done 콜백).
 * 트랜잭션 도중 호출이 들어오면(PAUSING/RESUMING) -EBUSY 반환. 호출 가능 상태는 RUNNING뿐.
 *
 * 실행 컨텍스트: 통상 SPDK 메인 thread (RPC dispatch context). fan-out은 모든 reactor로 분산.
 *
 * 호출 체인:
 *   RPC nvmf_tgt_pause_polling -> [본 함수]
 *     -> spdk_for_each_channel(_nvmf_tgt_pause_polling, _nvmf_tgt_pause_polling_done)
 */
int
spdk_nvmf_tgt_pause_polling(struct spdk_nvmf_tgt *tgt, spdk_nvmf_tgt_pause_polling_cb_fn cb_fn,
			    void *cb_arg)
{
	struct nvmf_tgt_pause_ctx *ctx;                  /* [한국어] fan-out ctx (cb 정보 보존용) */

	SPDK_DTRACE_PROBE2_TICKS(nvmf_tgt_pause_polling, tgt, tgt->name); /* [한국어] DTrace - 어떤 target의 polling이 정지되는지 추적 */

	switch (tgt->state) {                            /* [한국어] target 상태 머신 검사 - 호출 가능 상태인가? */
	case NVMF_TGT_PAUSING:                           /* [한국어] 이미 일시 정지 진행 중 - 동시 호출 거부 */
	case NVMF_TGT_RESUMING:                          /* [한국어] 재개 진행 중 - 일관성 위해 거부 */
		return -EBUSY;
	case NVMF_TGT_RUNNING:                           /* [한국어] 정상 실행 중에서만 정지 가능 */
		break;
	default:                                         /* [한국어] CREATING/DESTROYING 등 비정상 상태 거부 */
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));                   /* [한국어] zero-init ctx 할당 */
	if (!ctx) {                                      /* [한국어] OOM */
		return -ENOMEM;
	}


	tgt->state = NVMF_TGT_PAUSING;                   /* [한국어] 전이 중 상태 진입 - 이후 호출은 -EBUSY 반환 */

	ctx->tgt = tgt;                                  /* [한국어] iter 단계에서 사용할 target back-pointer */
	ctx->cb_fn = cb_fn;                              /* [한국어] 완료 cb 보존 */
	ctx->cb_arg = cb_arg;                            /* [한국어] 사용자 ctx 보존 */

	spdk_for_each_channel(tgt,                       /* [한국어] tgt(io_device)의 모든 channel 순회 (=각 reactor의 group) */
			      _nvmf_tgt_pause_polling, /* [한국어] 각 channel에서 실행될 visit 함수 */
			      ctx,                       /* [한국어] iter ctx (각 channel에서 spdk_io_channel_iter_get_ctx로 복원) */
			      _nvmf_tgt_pause_polling_done); /* [한국어] 모든 channel 완료 후 호출되는 cb */
	return 0;                                        /* [한국어] 비동기 시작 성공 - 실제 완료는 _done에서 통지 */
}

/*
 * [한국어]
 * _nvmf_tgt_resume_polling_done - resume fan-out 완료 콜백
 *
 * @i: iter
 * @status: fan-out 결과 (0=성공)
 *
 * 모든 reactor의 트랜스포트 polling을 재개한 뒤 호출. tgt->state를 RESUMING -> RUNNING으로 전이.
 *
 * 실행 컨텍스트: spdk_for_each_channel 시작 thread.
 */
static void
_nvmf_tgt_resume_polling_done(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_tgt_pause_ctx *ctx = spdk_io_channel_iter_get_ctx(i); /* [한국어] iter ctx 복원 (pause/resume 동일 구조 재사용) */

	ctx->tgt->state = NVMF_TGT_RUNNING;              /* [한국어] RESUMING -> RUNNING - 정상 동작 재개 */

	ctx->cb_fn(ctx->cb_arg, status);                 /* [한국어] 사용자에게 결과 통지 */
	free(ctx);                                       /* [한국어] ctx 해제 - fan-out 종료 */
}

/*
 * [한국어]
 * _nvmf_tgt_resume_polling - 각 poll_group의 모든 transport poll_group을 재개 (fan-out 단위)
 *
 * @i: iter
 *
 * 각 reactor에서 본 함수가 호출되어 트랜스포트별 ops->poll_group_resume를 호출. pause로 정지된
 * 트랜스포트별 폴링/이벤트 처리를 다시 시작한다.
 *
 * 실행 컨텍스트: 각 channel의 group->thread.
 */
static void
_nvmf_tgt_resume_polling(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i); /* [한국어] 현재 channel */
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch); /* [한국어] group 포인터 복원 */
	struct spdk_nvmf_transport_poll_group *tgroup;   /* [한국어] 트랜스포트별 poll_group 순회용 */

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {   /* [한국어] 본 group의 모든 트랜스포트 poll_group 순회 */
		nvmf_transport_poll_group_resume(tgroup); /* [한국어] 트랜스포트별 ops->poll_group_resume - 폴링 재개 */
	}

	spdk_for_each_channel_continue(i, 0);            /* [한국어] 다음 channel로 iter 진행 */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_resume_polling - 일시 정지된 target의 폴링을 비동기로 재개 (외부 공개 API)
 *
 * @tgt: 재개할 target
 * @cb_fn: 모든 reactor 재개 완료 후 호출되는 cb
 * @cb_arg: cb_fn에 전달할 사용자 컨텍스트
 * @return: 0 / -EBUSY / -EINVAL / -ENOMEM
 *
 * 상태 머신: PAUSED -> RESUMING -> RUNNING. PAUSED 상태에서만 호출 가능.
 *
 * 호출 체인:
 *   RPC nvmf_tgt_resume_polling -> [본 함수]
 *     -> spdk_for_each_channel(_nvmf_tgt_resume_polling, _nvmf_tgt_resume_polling_done)
 */
int
spdk_nvmf_tgt_resume_polling(struct spdk_nvmf_tgt *tgt, spdk_nvmf_tgt_resume_polling_cb_fn cb_fn,
			     void *cb_arg)
{
	struct nvmf_tgt_pause_ctx *ctx;                  /* [한국어] fan-out ctx (pause와 구조 공유) */

	SPDK_DTRACE_PROBE2_TICKS(nvmf_tgt_resume_polling, tgt, tgt->name); /* [한국어] DTrace - 어떤 target이 재개되는지 추적 */

	switch (tgt->state) {                            /* [한국어] 상태 머신 검사 */
	case NVMF_TGT_PAUSING:                           /* [한국어] 정지 진행 중 - 거부 */
	case NVMF_TGT_RESUMING:                          /* [한국어] 이미 재개 진행 중 - 거부 */
		return -EBUSY;
	case NVMF_TGT_PAUSED:                            /* [한국어] 정지 상태에서만 재개 허용 */
		break;
	default:                                         /* [한국어] RUNNING 등 - 의미 없는 호출 거부 */
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));                   /* [한국어] zero-init ctx 할당 */
	if (!ctx) {
		return -ENOMEM;
	}

	tgt->state = NVMF_TGT_RESUMING;                  /* [한국어] 전이 중 상태 진입 - 동시 호출 차단 */

	ctx->tgt = tgt;                                  /* [한국어] iter ctx 채우기 */
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(tgt,                       /* [한국어] 모든 reactor에 fan-out */
			      _nvmf_tgt_resume_polling,
			      ctx,
			      _nvmf_tgt_resume_polling_done);
	return 0;                                        /* [한국어] 비동기 시작 성공 */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_find_subsystem - subnqn 문자열로 target에서 subsystem 검색 (외부 공개 API)
 *
 * @tgt: 검색 대상 target
 * @subnqn: 찾을 NQN(NVMe Qualified Name) 문자열 (null-terminated)
 * @return: 매칭되는 subsystem 포인터 / NULL (입력 비정상 또는 매칭 없음)
 *
 * NVMe-oF Connect 명령 처리 시 호스트가 제출한 SUBNQN으로 대상 subsystem을 조회하는 핵심 경로다.
 * 입력 보안 검증으로 SPDK_NVMF_NQN_MAX_LEN+1 범위 안에 null 종결자가 있는지 검사 — 길이 검증 누락 시
 * 임의 메모리 읽기 위험. RB tree(subsystem_tree)에서 O(log n)으로 조회한다.
 *
 * 실행 컨텍스트: 어느 thread에서나 호출 가능 (RB tree는 subsystem 추가/삭제 시 메인 thread에서만
 * 변형되며, find는 read-only로 안전).
 *
 * 호출 체인:
 *   nvmf_ctrlr_connect 등 Connect 처리 -> [본 함수] -> RB_FIND(subsystem_tree)
 */
struct spdk_nvmf_subsystem *
spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt, const char *subnqn)
{
	struct spdk_nvmf_subsystem subsystem;            /* [한국어] RB_FIND key용 임시 객체 - subnqn 필드만 사용 */

	if (!subnqn) {                                   /* [한국어] 입력 NULL 방어 */
		return NULL;
	}

	/* Ensure that subnqn is null terminated */
	if (!memchr(subnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1)) { /* [한국어] 보안 검증 - SPDK_NVMF_NQN_MAX_LEN+1 범위 내에 \0이 있어야 함 (스펙 준수, 버퍼 오버플로우 방지) */
		SPDK_ERRLOG("Connect SUBNQN is not null terminated\n");
		return NULL;
	}

	snprintf(subsystem.subnqn, sizeof(subsystem.subnqn), "%s", subnqn); /* [한국어] key 객체에 subnqn 복사 (안전한 길이 제한 복사) */
	return RB_FIND(subsystem_tree, &tgt->subsystems, &subsystem); /* [한국어] BSD RB tree 조회 - 비교자는 subnqn 문자열 비교 */
}

/*
 * [한국어]
 * spdk_nvmf_tgt_get_transport - transport name으로 target에 등록된 transport 조회 (외부 공개 API)
 *
 * @tgt: 검색 대상 target
 * @transport_name: 트랜스포트 이름 ("RDMA", "TCP", "FC" 등)
 * @return: 매칭되는 transport 포인터 / NULL
 *
 * 같은 transport는 한 target에 한 번만 등록 가능하므로 중복 추가 방지 검사용으로도 쓰인다.
 * 비교는 strncasecmp로 대소문자 무시 (SPDK_NVMF_TRSTRING_MAX_LEN까지).
 *
 * 실행 컨텍스트: 메인 thread (transports 리스트는 add/remove가 메인 thread에서만 일어남).
 *
 * 호출 체인:
 *   spdk_nvmf_tgt_add_transport (중복 검사용) -> [본 함수]
 *   nvmf_subsystem_listener_add 등 -> [본 함수]
 */
struct spdk_nvmf_transport *
spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt, const char *transport_name)
{
	struct spdk_nvmf_transport *transport;           /* [한국어] 순회용 transport 포인터 */

	TAILQ_FOREACH(transport, &tgt->transports, link) { /* [한국어] target에 등록된 모든 transport 순회 */
		if (!strncasecmp(transport->ops->name, transport_name, SPDK_NVMF_TRSTRING_MAX_LEN)) { /* [한국어] 대소문자 무시 비교, 최대 SPDK_NVMF_TRSTRING_MAX_LEN까지 - "TCP"/"tcp" 동일 처리 */
			return transport;                /* [한국어] 매칭 - 즉시 반환 */
		}
	}
	return NULL;                                     /* [한국어] 매칭 없음 - 미등록 트랜스포트 */
}

/*
 * [한국어]
 * struct nvmf_new_qpair_ctx - 새 qpair을 group->thread로 위임할 때 쓰는 cross-thread 메시지 ctx
 *
 * 트랜스포트 accept 콜백은 보통 어셉트 thread에서 호출되지만, qpair 등록은 group->thread에서만
 * 수행되어야 lockless 보장이 깨지지 않는다. 이 ctx는 spdk_thread_send_msg로 group->thread에
 * qpair 추가 작업을 위임할 때 인자를 보존한다.
 */
struct nvmf_new_qpair_ctx {
	struct spdk_nvmf_qpair *qpair;
	/* [한국어] group에 추가할 신규 qpair 포인터.
	 * 설정자: spdk_nvmf_tgt_new_qpair가 calloc 직후 저장.
	 * 읽는 자: _nvmf_poll_group_add가 group->thread에서 group에 결합. */

	struct spdk_nvmf_poll_group *group;
	/* [한국어] qpair을 결합시킬 대상 poll_group.
	 * 설정자: spdk_nvmf_tgt_new_qpair에서 optimal/round-robin으로 선택해 저장.
	 * 읽는 자: _nvmf_poll_group_add. */
};

/*
 * [한국어]
 * _nvmf_poll_group_add - group->thread로 위임된 qpair 추가 메시지 핸들러
 *
 * @_ctx: nvmf_new_qpair_ctx (송신 측 spdk_nvmf_tgt_new_qpair에서 alloc)
 *
 * 1) ctx에서 qpair/group 추출 후 ctx 즉시 free.
 * 2) spdk_nvmf_poll_group_add로 본격 결합 (실패하면 unassociated 카운터 감소 후 disconnect).
 *
 * 실행 컨텍스트: group->thread (spdk_thread_send_msg가 디스패치).
 *
 * 호출 체인:
 *   spdk_nvmf_tgt_new_qpair -> spdk_thread_send_msg -> [본 함수]
 *     -> spdk_nvmf_poll_group_add 또는 실패 시 spdk_nvmf_qpair_disconnect
 */
static void
_nvmf_poll_group_add(void *_ctx)
{
	struct nvmf_new_qpair_ctx *ctx = _ctx;           /* [한국어] void* -> 구조체 캐스팅 */
	struct spdk_nvmf_qpair *qpair = ctx->qpair;      /* [한국어] 추가할 qpair 추출 */
	struct spdk_nvmf_poll_group *group = ctx->group; /* [한국어] 대상 poll_group 추출 */

	free(_ctx);                                      /* [한국어] ctx는 더 이상 필요 없음 - 즉시 해제 (오류 시 leak 방지) */

	if (spdk_nvmf_poll_group_add(group, qpair) != 0) { /* [한국어] tgroup 결합 + qpairs 리스트 삽입 + 상태 CONNECTING 전이 */
		SPDK_ERRLOG("Unable to add the qpair to a poll group.\n");

		assert(qpair->state == SPDK_NVMF_QPAIR_UNINITIALIZED); /* [한국어] add 실패 시 qpair는 여전히 UNINITIALIZED여야 함 */
		pthread_mutex_lock(&group->mutex);       /* [한국어] unassociated 카운터는 cross-thread accessible -> mutex 보호 */
		assert(group->current_unassociated_qpairs > 0); /* [한국어] tgt_new_qpair에서 +1 했으므로 0보다 커야 함 */
		group->current_unassociated_qpairs--;    /* [한국어] 결합 실패 -> 카운터 원복 */
		pthread_mutex_unlock(&group->mutex);

		spdk_nvmf_qpair_disconnect(qpair);       /* [한국어] qpair 정리 - UNINITIALIZED 경로로 즉시 fini */
	}
}

/*
 * [한국어]
 * spdk_nvmf_tgt_new_qpair - 트랜스포트 accept 시 신규 qpair을 적절한 poll_group에 배치 (외부 공개 API)
 *
 * @tgt: 대상 target
 * @qpair: 신규 qpair (트랜스포트 layer가 accept로 막 생성)
 *
 * 이 함수가 NVMe-oF 데이터플레인의 진입점이다. 트랜스포트(TCP/RDMA)가 호스트 연결을 받아 qpair을
 * 만든 직후 호출한다. 동작 단계:
 *   1) 트랜스포트가 optimal_poll_group 콜백을 제공하면 거기에 위임 (예: RDMA는 cq affinity).
 *   2) 없거나 NULL 반환 시 round-robin으로 next_poll_group 선택 (TAILQ 순환).
 *   3) cross-thread 위임 ctx를 만들고 unassociated 카운터를 +1 (group->thread에서 -1 또는 결합 시 다른 카운터로 옮김).
 *   4) spdk_thread_send_msg로 group->thread에 _nvmf_poll_group_add 호출 위임.
 *
 * 실행 컨텍스트: 트랜스포트 accept thread (TCP는 accept poller thread, RDMA는 listener thread). qpair 결합 자체는 group->thread.
 *
 * 호출 체인:
 *   tgt_listen_poller / RDMA accept -> 트랜스포트 ops->new_qpair_cb -> [본 함수]
 *     -> spdk_thread_send_msg(group->thread, _nvmf_poll_group_add)
 */
void
spdk_nvmf_tgt_new_qpair(struct spdk_nvmf_tgt *tgt, struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_poll_group *group;              /* [한국어] qpair을 결합시킬 대상 group */
	struct nvmf_new_qpair_ctx *ctx;                  /* [한국어] cross-thread 메시지 ctx */

	group = spdk_nvmf_get_optimal_poll_group(qpair); /* [한국어] 트랜스포트가 추천하는 group 조회 (예: RDMA는 cq affinity 기반) */
	if (group == NULL) {                             /* [한국어] 트랜스포트가 추천 없음 - round-robin 사용 */
		if (tgt->next_poll_group == NULL) {      /* [한국어] 라운드 시작 또는 끝까지 순회한 경우 */
			tgt->next_poll_group = TAILQ_FIRST(&tgt->poll_groups); /* [한국어] 첫 group으로 wrap-around */
			if (tgt->next_poll_group == NULL) { /* [한국어] poll_group이 하나도 없음 - 초기화 미완료 */
				SPDK_ERRLOG("No poll groups exist.\n");
				spdk_nvmf_qpair_disconnect(qpair); /* [한국어] qpair 즉시 정리 */
				return;
			}
		}
		group = tgt->next_poll_group;            /* [한국어] 현재 라운드의 후보 선택 */
		tgt->next_poll_group = TAILQ_NEXT(group, link); /* [한국어] 다음 호출은 다음 group을 선택하도록 포인터 전진 (NULL이면 다음 호출에서 wrap-around) */
	}

	ctx = calloc(1, sizeof(*ctx));                   /* [한국어] cross-thread 메시지 ctx 할당 */
	if (!ctx) {                                      /* [한국어] OOM */
		SPDK_ERRLOG("Unable to send message to poll group.\n");
		spdk_nvmf_qpair_disconnect(qpair);       /* [한국어] qpair 즉시 정리 */
		return;
	}

	ctx->qpair = qpair;                              /* [한국어] 신규 qpair 보존 */
	ctx->group = group;                              /* [한국어] 결합 대상 group 보존 */

	pthread_mutex_lock(&group->mutex);               /* [한국어] cross-thread 접근 카운터 보호 */
	group->current_unassociated_qpairs++;            /* [한국어] connect 미완료 qpair 카운터 +1 (Connect 명령 도착 전까지 stat에 반영) */
	pthread_mutex_unlock(&group->mutex);

	spdk_thread_send_msg(group->thread, _nvmf_poll_group_add, ctx); /* [한국어] group->thread에서 _nvmf_poll_group_add 실행 위임 - lockless 결합 보장 */
}

/*
 * [한국어]
 * spdk_nvmf_poll_group_create - 호출 thread에 새 poll_group을 생성/획득 (외부 공개 API)
 *
 * @tgt: target (io_device)
 * @return: poll_group 포인터 / NULL (실패)
 *
 * 내부적으로 spdk_get_io_channel(tgt)을 호출해 현재 thread의 channel을 받는다. SPDK io_channel
 * 구현은 thread당 channel을 캐시하므로 같은 thread에서 두 번 호출하면 동일 channel + ref 증가가
 * 일어난다. channel ctx_buf에 저장된 spdk_nvmf_poll_group이 본 함수의 반환값.
 *
 * 어플리케이션 초기화 시 각 reactor에서 한 번씩 호출되며, 이로써 모든 reactor에 poll_group이 배치된다.
 *
 * 실행 컨텍스트: poll_group을 만들고 싶은 reactor의 thread.
 *
 * 호출 체인:
 *   nvmf_tgt 앱의 setup -> [본 함수] -> spdk_get_io_channel
 *     -> nvmf_tgt_create_poll_group (channel create cb) -> spdk_io_channel_get_ctx
 */
struct spdk_nvmf_poll_group *
spdk_nvmf_poll_group_create(struct spdk_nvmf_tgt *tgt)
{
	struct spdk_io_channel *ch;                      /* [한국어] thread별 io_channel 핸들 */

	ch = spdk_get_io_channel(tgt);                   /* [한국어] 현재 thread의 channel 획득 - 없으면 nvmf_tgt_create_poll_group 트리거 */
	if (!ch) {                                       /* [한국어] 생성 실패 */
		SPDK_ERRLOG("Unable to get I/O channel for target\n");
		return NULL;
	}

	return spdk_io_channel_get_ctx(ch);              /* [한국어] channel ctx_buf -> spdk_nvmf_poll_group (channel 생성 시 채워둔 값) */
}

/*
 * [한국어]
 * spdk_nvmf_poll_group_destroy - poll_group을 비동기로 폐기 (외부 공개 API)
 *
 * @group: 폐기할 poll_group
 * @cb_fn: 폐기 완료 후 호출되는 cb (NULL 가능)
 * @cb_arg: cb_fn 인자
 *
 * 다단계 비동기 정리:
 *   1) destroy_cb_fn/cb_arg를 group에 저장 (소멸 완료 시 호출용).
 *   2) nvmf_tgt_destroy_poll_group_qpairs로 group의 모든 qpair을 disconnect.
 *      마지막 qpair 정리 후 spdk_put_io_channel(ch) -> nvmf_tgt_destroy_poll_group(channel destroy cb)
 *      에서 destroy_cb_fn(arg, status) 호출.
 *
 * 핵심: io_channel을 직접 put하지 않고 qpair drain을 거쳐 간접적으로 put 되도록 설계.
 *
 * 실행 컨텍스트: group->thread (qpair 정리는 모두 이 thread).
 *
 * 호출 체인:
 *   spdk_nvmf_tgt_destroy / 앱 종료 -> [본 함수]
 *     -> nvmf_tgt_destroy_poll_group_qpairs -> spdk_nvmf_qpair_disconnect (각 qpair)
 *     -> 모두 정리되면 spdk_put_io_channel -> nvmf_tgt_destroy_poll_group -> destroy_cb_fn
 */
void
spdk_nvmf_poll_group_destroy(struct spdk_nvmf_poll_group *group,
			     spdk_nvmf_poll_group_destroy_done_fn cb_fn,
			     void *cb_arg)
{
	assert(group->destroy_cb_fn == NULL);            /* [한국어] 동시 destroy 호출 방지 - 이전 호출이 끝나기 전 재호출 금지 */
	group->destroy_cb_fn = cb_fn;                    /* [한국어] 채널 destroy 완료 시 호출될 cb 등록 */
	group->destroy_cb_arg = cb_arg;                  /* [한국어] cb 인자 등록 */

	/* This function will put the io_channel associated with this poll group */
	nvmf_tgt_destroy_poll_group_qpairs(group);       /* [한국어] qpair 모두 disconnect -> 마지막 qpair fini 시점에 spdk_put_io_channel */
}

/*
 * [한국어]
 * spdk_nvmf_poll_group_add - 신규 qpair을 poll_group에 결합 (외부 공개 API)
 *
 * @group: 결합 대상 poll_group
 * @qpair: 결합할 qpair (트랜스포트 layer가 막 만든 신규 qpair)
 * @return: 0 성공 / -1 트랜스포트 poll_group 미존재 / 트랜스포트 ops->poll_group_add 에러
 *
 * 동작 단계:
 *   1) qpair의 outstanding 리스트 초기화, group/ctrlr 백포인터 설정, disconnect_started 플래그 초기화.
 *   2) qpair의 transport에 해당하는 tgroup 조회 (없으면 -1).
 *   3) 트랜스포트별 poll_group_add ops 호출 (TCP는 epoll에 등록, RDMA는 cq에 attach 등).
 *   4) 성공 시 group->qpairs 리스트에 추가하고 상태를 CONNECTING으로 전이.
 *
 * 실행 컨텍스트: group->thread. 일반적으로 _nvmf_poll_group_add 위임 후 진입.
 *
 * 호출 체인:
 *   _nvmf_poll_group_add -> [본 함수]
 *     -> nvmf_get_transport_poll_group / nvmf_transport_poll_group_add
 *     -> nvmf_qpair_set_state(CONNECTING)
 */
int
spdk_nvmf_poll_group_add(struct spdk_nvmf_poll_group *group,
			 struct spdk_nvmf_qpair *qpair)
{
	int rc;                                          /* [한국어] 트랜스포트 add 결과 */
	struct spdk_nvmf_transport_poll_group *tgroup;   /* [한국어] qpair->transport에 매칭되는 tgroup */

	TAILQ_INIT(&qpair->outstanding);                 /* [한국어] outstanding 요청 리스트 초기화 - 이후 cmd dispatch가 여기에 추가 */
	qpair->group = group;                            /* [한국어] qpair -> group 백포인터 (qpair가 어느 group에 묶였는지 추적) */
	qpair->ctrlr = NULL;                             /* [한국어] Connect 명령 처리 후에만 ctrlr이 결합됨 - 초기에는 NULL */
	qpair->disconnect_started = false;               /* [한국어] disconnect 단일 호출 보장 atomic 초기화 */

	tgroup = nvmf_get_transport_poll_group(group, qpair->transport); /* [한국어] qpair의 transport에 해당하는 tgroup 조회 (group->tgroups 순회) */
	if (tgroup == NULL) {                            /* [한국어] 해당 transport가 group에 결합되지 않음 - 비정상 (transport 없이 qpair 생성될 수 없어야) */
		return -1;
	}

	rc = nvmf_transport_poll_group_add(tgroup, qpair); /* [한국어] 트랜스포트별 poll_group_add ops 호출 - TCP는 epoll add, RDMA는 cq attach 등 */

	/* We add the qpair to the group only it is successfully added into the tgroup */
	if (rc == 0) {                                   /* [한국어] 트랜스포트 결합 성공 시에만 group 리스트에도 추가 */
		SPDK_DTRACE_PROBE2_TICKS(nvmf_poll_group_add_qpair, qpair, spdk_thread_get_id(group->thread)); /* [한국어] DTrace - 어떤 qpair가 어떤 thread에 추가되는지 */
		TAILQ_INSERT_TAIL(&group->qpairs, qpair, link); /* [한국어] group->qpairs에 등록 - poller가 순회 가능 */
		nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_CONNECTING); /* [한국어] UNINITIALIZED -> CONNECTING - Connect 명령 수신 대기 */
	}

	return rc;                                       /* [한국어] 호출자(_nvmf_poll_group_add)는 0이 아니면 disconnect 처리 */
}

/*
 * [한국어]
 * _nvmf_ctrlr_destruct - ctrlr 소유 thread(subsys->thread)로 위임된 ctrlr 파괴 메시지 핸들러
 *
 * @ctx: ctrlr 포인터 (void* cast)
 *
 * spdk_thread_send_msg로 subsys->thread에 디스패치되며, 실제 ctrlr 자료 해제는 ctrlr.c의
 * nvmf_ctrlr_destruct로 이양한다. ctrlr 파괴는 NVMe-oF 연결 종료의 마지막 단계 — 이 함수가
 * 끝나면 ctrlr 자료구조가 메모리에서 사라진다.
 *
 * 실행 컨텍스트: ctrlr->subsys->thread (ctrlr 자료구조 소유 thread).
 *
 * 호출 체인:
 *   _nvmf_ctrlr_free_from_qpair (마지막 qpair 정리 시) -> spdk_thread_send_msg -> [본 함수]
 *     -> nvmf_ctrlr_destruct
 */
static void
_nvmf_ctrlr_destruct(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx;             /* [한국어] void* -> ctrlr 캐스팅 */

	nvmf_ctrlr_destruct(ctrlr);                      /* [한국어] ctrlr.c의 본격 파괴 함수 호출 - subsys->ctrlrs에서 제거, 메모리 해제 */
}

/*
 * [한국어]
 * _nvmf_ctrlr_free_from_qpair - qpair fini 후 ctrlr의 qpair_mask에서 해당 qid 비트 해제
 *
 * @ctx: nvmf_qpair_disconnect_ctx (qid, ctrlr 포함)
 *
 * 동작:
 *   1) qid에 해당하는 비트를 qpair_mask에서 클리어.
 *   2) 남은 qpair 수가 0이면 ctrlr 파괴 단계로 진입 (in_destruct=true 후 subsys->thread에 destruct 메시지).
 *   3) ctx 해제.
 *
 * 핵심: NVMe ctrlr은 admin+IO qpair의 집합이며, 모든 qpair이 정리되어야 ctrlr 자체를 해제할 수 있다.
 * qpair_mask는 spdk_bit_array로 qid 단위 plat 표현 — bit 0=admin, 1~N=IO.
 *
 * 실행 컨텍스트: ctrlr->thread. _nvmf_transport_qpair_fini_complete가 send_msg로 위임.
 *
 * 호출 체인:
 *   _nvmf_transport_qpair_fini_complete -> spdk_thread_send_msg(ctrlr->thread, [본 함수])
 *     -> 마지막 qpair면 spdk_thread_send_msg(subsys->thread, _nvmf_ctrlr_destruct)
 */
static void
_nvmf_ctrlr_free_from_qpair(void *ctx)
{
	struct nvmf_qpair_disconnect_ctx *qpair_ctx = ctx; /* [한국어] disconnect ctx에서 qid/ctrlr 추출 */
	struct spdk_nvmf_ctrlr *ctrlr = qpair_ctx->ctrlr; /* [한국어] 대상 ctrlr */
	uint32_t count;                                  /* [한국어] 남아있는 qpair 수 */

	spdk_bit_array_clear(ctrlr->qpair_mask, qpair_ctx->qid); /* [한국어] qid 비트 0으로 - 이 qpair은 더 이상 ctrlr에 속하지 않음 */
	SPDK_DEBUGLOG(nvmf, "qpair_mask cleared, qid %u\n", qpair_ctx->qid);
	count = spdk_bit_array_count_set(ctrlr->qpair_mask); /* [한국어] 남은 qpair 수 카운트 (set bit 개수) */
	if (count == 0) {                                /* [한국어] 모든 qpair이 정리됨 - ctrlr 파괴 진입 */
		assert(!ctrlr->in_destruct);             /* [한국어] 중복 진입 방지 - 이미 파괴 중이면 ctrlr 상태 일관성 깨짐 */
		SPDK_DEBUGLOG(nvmf, "Last qpair %u, destroy ctrlr 0x%hx\n", qpair_ctx->qid, ctrlr->cntlid);
		ctrlr->in_destruct = true;               /* [한국어] 파괴 진행 중 플래그 - 새 qpair 결합 차단 */
		spdk_thread_send_msg(ctrlr->subsys->thread, _nvmf_ctrlr_destruct, ctrlr); /* [한국어] subsys->thread로 ctrlr 파괴 위임 (subsys->ctrlrs 리스트는 subsys->thread 소유) */
	}
	free(qpair_ctx);                                 /* [한국어] disconnect ctx 해제 - 비동기 chain 종료 */
}

/*
 * [한국어]
 * _nvmf_transport_qpair_fini_complete - 트랜스포트 qpair_fini 완료 cb (qpair fini chain 중간 단계)
 *
 * @cb_ctx: nvmf_qpair_disconnect_ctx
 *
 * 트랜스포트 layer가 qpair 자원(소켓, RDMA QP 등) 해제를 끝냈을 때 호출된다. 다음 단계는
 * ctrlr->qpair_mask에서 qid 비트 해제 — 이는 ctrlr->thread에서 수행되어야 하므로 cross-thread send_msg.
 * ctrlr이 NULL이면(Connect 미완료 qpair) 단순히 ctx만 free.
 *
 * Admin qpair(qid=0) 특수 처리: ctrlr->admin_qpair 포인터를 NULL로 설정 — admin qpair과 ctrlr은
 * 같은 thread를 소유하므로 안전하게 직접 NULL 가능.
 *
 * 실행 컨텍스트: 트랜스포트가 fini를 완료한 thread (보통 group->thread).
 *
 * 호출 체인:
 *   _nvmf_qpair_destroy -> nvmf_transport_qpair_fini -> [본 함수]
 *     -> ctrlr 있으면: spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_free_from_qpair)
 *     -> ctrlr 없으면: free(qpair_ctx)
 */
static void
_nvmf_transport_qpair_fini_complete(void *cb_ctx)
{
	struct nvmf_qpair_disconnect_ctx *qpair_ctx = cb_ctx; /* [한국어] disconnect ctx 복원 (qid, ctrlr 보존) */
	struct spdk_nvmf_ctrlr *ctrlr;                   /* [한국어] qpair이 속한 ctrlr (없을 수 있음) */

	ctrlr = qpair_ctx->ctrlr;                        /* [한국어] _nvmf_qpair_destroy가 보존해 둔 ctrlr 포인터 */
	SPDK_DEBUGLOG(nvmf, "Finish destroying qid %u\n", qpair_ctx->qid);

	if (ctrlr) {                                     /* [한국어] Connect 완료된 qpair - ctrlr과 결합되어 있음 */
		if (qpair_ctx->qid == 0) {               /* [한국어] qid=0은 admin qpair (NVMe 스펙) */
			/* Admin qpair is removed, so set the pointer to NULL.
			 * This operation is safe since we are on ctrlr thread now, admin qpair's thread is the same
			 * as controller's thread */
			assert(ctrlr->thread == spdk_get_thread()); /* [한국어] admin qpair과 ctrlr은 같은 thread를 소유한다는 SPDK 불변식 */
			ctrlr->admin_qpair = NULL;       /* [한국어] admin qpair 백포인터 클리어 - 이후 admin 명령 라우팅 불가 */
		}
		/* Free qpair id from controller's bit mask and destroy the controller if it is the last qpair */
		if (ctrlr->thread) {                     /* [한국어] ctrlr->thread가 설정된 경우 (정상 경로) - cross-thread 위임 */
			spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_free_from_qpair, qpair_ctx); /* [한국어] qpair_mask 해제는 ctrlr->thread에서 직렬화 */
		} else {                                 /* [한국어] ctrlr->thread 미설정 (초기화 도중) - 즉시 직접 호출 */
			_nvmf_ctrlr_free_from_qpair(qpair_ctx);
		}
	} else {                                         /* [한국어] ctrlr 없음 - Connect 명령 도착 전에 disconnect된 qpair */
		free(qpair_ctx);                         /* [한국어] chain 종료 - ctx만 해제 */
	}
}

/*
 * [한국어]
 * spdk_nvmf_poll_group_remove - qpair을 poll_group에서 제거 (외부 공개 API)
 *
 * @qpair: 제거할 qpair
 *
 * disconnect 경로의 일부로, 트랜스포트 poll_group에서 qpair을 분리하고 group->qpairs 리스트에서도
 * 제거한다. 트랜스포트 ops->poll_group_remove가 ENOTSUP을 반환할 수 있다 (일부 트랜스포트는 별도
 * remove 단계가 필요 없음 — 이는 정상으로 간주, 에러 로그만 생략).
 *
 * 호출 후 qpair->group은 NULL — 이후 어떤 함수도 qpair을 통해 group에 접근하면 안 된다.
 *
 * 실행 컨텍스트: group->thread (qpair 소유 thread).
 *
 * 호출 체인:
 *   _nvmf_qpair_destroy -> [본 함수]
 *     -> nvmf_get_transport_poll_group / nvmf_transport_poll_group_remove
 */
void
spdk_nvmf_poll_group_remove(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_transport_poll_group *tgroup;   /* [한국어] qpair->transport에 매칭되는 tgroup */
	int rc;                                          /* [한국어] 트랜스포트 remove 결과 */

	SPDK_DTRACE_PROBE2_TICKS(nvmf_poll_group_remove_qpair, qpair,
				 spdk_thread_get_id(qpair->group->thread)); /* [한국어] DTrace - 어떤 qpair이 어떤 thread에서 제거되는지 */
	nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_ERROR); /* [한국어] qpair 상태를 ERROR로 - 이후 어떤 명령도 dispatch 금지 */

	/* Find the tgroup and remove the qpair from the tgroup */
	tgroup = nvmf_get_transport_poll_group(qpair->group, qpair->transport); /* [한국어] qpair의 transport에 해당하는 tgroup 조회 */
	if (tgroup != NULL) {                            /* [한국어] tgroup이 정상 결합되어 있으면 트랜스포트별 remove 호출 */
		rc = nvmf_transport_poll_group_remove(tgroup, qpair); /* [한국어] 트랜스포트별 ops->poll_group_remove (TCP는 epoll에서 제거 등) */
		if (rc && (rc != ENOTSUP)) {             /* [한국어] ENOTSUP은 일부 트랜스포트의 정상 case - 그 외 에러는 로그 */
			SPDK_ERRLOG("Cannot remove qpair=%p from transport group=%p\n",
				    qpair, tgroup);
		}
	}

	TAILQ_REMOVE(&qpair->group->qpairs, qpair, link); /* [한국어] group->qpairs 리스트에서 분리 - 이후 group 순회 시 보이지 않음 */
	qpair->group = NULL;                             /* [한국어] 백포인터 무효화 - 이후 코드가 stale 참조하면 즉시 NPE */
}

/*
 * [한국어]
 * _nvmf_qpair_sgroup_req_clean - subsystem poll_group의 queued 리스트에서 특정 qpair 요청 정리
 *
 * @sgroup: 정리 대상 subsystem_poll_group
 * @qpair: 정리할 qpair
 *
 * subsystem이 PAUSED 상태로 들어가면 Connect 등 새 명령이 sgroup->queued에 적체된다.
 * qpair이 disconnect될 때 그 qpair의 적체 요청들을 모두 free해야 leak이 없다. FOREACH_SAFE를
 * 써서 순회 중 삭제 안전성 보장.
 *
 * 실행 컨텍스트: group->thread (sgroup은 group 소유).
 *
 * 호출 체인:
 *   _nvmf_qpair_destroy -> [본 함수] (각 sgroup마다)
 *     -> nvmf_transport_req_free (트랜스포트별 req free ops)
 */
static void
_nvmf_qpair_sgroup_req_clean(struct spdk_nvmf_subsystem_poll_group *sgroup,
			     const struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_request *req, *tmp;             /* [한국어] FOREACH_SAFE 순회용 (req 삭제 시 다음 포인터 보존) */
	TAILQ_FOREACH_SAFE(req, &sgroup->queued, link, tmp) { /* [한국어] sgroup의 queued 요청 순회 */
		if (req->qpair == qpair) {               /* [한국어] 정리 대상 qpair에 속한 요청만 */
			TAILQ_REMOVE(&sgroup->queued, req, link); /* [한국어] 큐에서 분리 */
			nvmf_transport_req_free(req);    /* [한국어] 트랜스포트별 req free ops - 메모리 풀에 반환 */
		}
	}
}

/*
 * [한국어]
 * _nvmf_qpair_destroy - qpair 본격 파괴 단계 (drain 후 호출되거나 outstanding 없을 때 직접 호출)
 *
 * @ctx: nvmf_qpair_disconnect_ctx
 * @status: 보통 0 (drain 완료)
 *
 * disconnect chain의 핵심. 동작 단계:
 *   1) DEACTIVATING 상태 검증 + qid 보존.
 *   2) connect_received 여부에 따라 stat 카운터 감소:
 *      - connect 완료된 qpair: admin/io_qpairs current 카운터 감소.
 *      - connect 미완료: current_unassociated_qpairs 카운터 감소 (mutex 보호).
 *   3) 적체된 요청 정리:
 *      - ctrlr 있으면 해당 subsystem sgroup만,
 *      - 없으면 모든 sgroup을 순회해 이 qpair 요청을 free.
 *   4) 인증(auth) 자료 파괴.
 *   5) ctrlr 백업 후 spdk_nvmf_poll_group_remove로 group/tgroup에서 분리.
 *   6) nvmf_transport_qpair_fini 호출 (트랜스포트별 자원 해제) - 완료 시 _nvmf_transport_qpair_fini_complete.
 *
 * 실행 컨텍스트: group->thread.
 *
 * 호출 체인:
 *   spdk_nvmf_qpair_disconnect (outstanding 없을 때) -> [본 함수]
 *   또는 outstanding drain 완료 시 ctrlr.c의 cmd 완료 경로에서 state_cb=[본 함수] 호출
 *     -> spdk_nvmf_poll_group_remove -> nvmf_transport_qpair_fini
 *     -> _nvmf_transport_qpair_fini_complete -> _nvmf_ctrlr_free_from_qpair (ctrlr->thread)
 *     -> 마지막이면 _nvmf_ctrlr_destruct (subsys->thread)
 */
static void
_nvmf_qpair_destroy(void *ctx, int status)
{
	struct nvmf_qpair_disconnect_ctx *qpair_ctx = ctx; /* [한국어] disconnect ctx 복원 */
	struct spdk_nvmf_qpair *qpair = qpair_ctx->qpair; /* [한국어] 정리 대상 qpair */
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;    /* [한국어] qpair이 속한 ctrlr (NULL: Connect 미완료) */
	struct spdk_nvmf_subsystem_poll_group *sgroup;   /* [한국어] subsystem별 poll_group (적체 요청 정리용) */
	uint32_t sid;                                    /* [한국어] subsystem id 순회용 */

	assert(qpair->state == SPDK_NVMF_QPAIR_DEACTIVATING); /* [한국어] disconnect가 상태를 DEACTIVATING으로 만든 뒤에만 진입 */
	qpair_ctx->qid = qpair->qid;                     /* [한국어] qid를 ctx에 보존 - 이후 chain에서 qpair 자체는 파괴되므로 qid는 ctx로 전달 */

	if (qpair->connect_received) {                   /* [한국어] Connect 명령을 정상 처리한 qpair (stat에 admin/io_qpairs로 잡힘) */
		if (0 == qpair->qid) {                   /* [한국어] admin qpair (qid=0) */
			assert(qpair->group->stat.current_admin_qpairs > 0);
			qpair->group->stat.current_admin_qpairs--; /* [한국어] 활성 admin qpair 카운터 감소 */
		} else {                                 /* [한국어] IO qpair (qid≥1) */
			assert(qpair->group->stat.current_io_qpairs > 0);
			qpair->group->stat.current_io_qpairs--; /* [한국어] 활성 IO qpair 카운터 감소 */
		}
	} else {                                         /* [한국어] Connect 미완료 - unassociated 카운터에 잡혀있음 (cross-thread accessible) */
		pthread_mutex_lock(&qpair->group->mutex); /* [한국어] unassociated 카운터 보호용 mutex */
		assert(qpair->group->current_unassociated_qpairs > 0);
		qpair->group->current_unassociated_qpairs--; /* [한국어] unassociated -1 */
		pthread_mutex_unlock(&qpair->group->mutex);
	}

	if (ctrlr) {                                     /* [한국어] ctrlr 결합된 qpair - 해당 subsystem만 정리 */
		sgroup = &qpair->group->sgroups[ctrlr->subsys->id]; /* [한국어] subsystem id로 sgroup 인덱싱 */
		_nvmf_qpair_sgroup_req_clean(sgroup, qpair); /* [한국어] 이 qpair의 queued 요청 free */
	} else {                                         /* [한국어] ctrlr 없음 - 어느 subsystem에 속하는지 알 수 없으므로 모든 sgroup 검사 */
		for (sid = 0; sid < qpair->group->num_sgroups; sid++) {
			sgroup = &qpair->group->sgroups[sid]; /* [한국어] sid번 subsystem의 sgroup */
			assert(sgroup != NULL);
			_nvmf_qpair_sgroup_req_clean(sgroup, qpair); /* [한국어] 이 sgroup에서 qpair 요청 정리 */
		}
	}

	nvmf_qpair_auth_destroy(qpair);                  /* [한국어] DH-HMAC-CHAP 등 인증 자료 해제 (NVMe-oF 1.1 보안) */
	qpair_ctx->ctrlr = ctrlr;                        /* [한국어] ctx에 ctrlr 보존 - 이후 _nvmf_transport_qpair_fini_complete가 사용 (qpair은 fini 후 stale) */
	spdk_nvmf_poll_group_remove(qpair);              /* [한국어] tgroup/group에서 qpair 분리 + qpair->group=NULL */
	nvmf_transport_qpair_fini(qpair, _nvmf_transport_qpair_fini_complete, qpair_ctx); /* [한국어] 트랜스포트별 fini 호출 - 완료 시 _nvmf_transport_qpair_fini_complete (chain 다음 단계) */
}

/*
 * [한국어]
 * _nvmf_qpair_disconnect_msg - cross-thread 위임된 disconnect 재호출 핸들러
 *
 * @ctx: nvmf_qpair_disconnect_ctx (qpair 보존)
 *
 * spdk_nvmf_qpair_disconnect가 cross-thread 호출이면 group->thread로 본 함수를 send_msg해
 * disconnect를 재시작한다. 본 함수는 단순히 disconnect를 다시 호출하고 ctx를 free.
 *
 * 실행 컨텍스트: group->thread (cross-thread 위임의 도착 thread).
 *
 * 호출 체인:
 *   spdk_nvmf_qpair_disconnect (cross-thread 분기) -> spdk_thread_send_msg -> [본 함수]
 *     -> spdk_nvmf_qpair_disconnect (group->thread에서 재진입)
 */
static void
_nvmf_qpair_disconnect_msg(void *ctx)
{
	struct nvmf_qpair_disconnect_ctx *qpair_ctx = ctx; /* [한국어] ctx 복원 */

	spdk_nvmf_qpair_disconnect(qpair_ctx->qpair);    /* [한국어] group->thread에서 disconnect 재호출 - 이번엔 동일 thread 분기 진입 */
	free(ctx);                                       /* [한국어] 위임 ctx 해제 (qpair 파괴는 별도 ctx로 진행) */
}

/*
 * [한국어]
 * spdk_nvmf_qpair_disconnect - qpair 비동기 종료 (외부 공개 API의 핵심)
 *
 * @qpair: 종료할 qpair
 * @return: 0 성공/진행 중, -EINPROGRESS 이미 다른 호출이 진행 중, -ENOMEM
 *
 * 다단계 비동기 정리:
 *   1) atomic_test_and_set(disconnect_started)으로 단일 호출 보장 (이미 진행 중이면 -EINPROGRESS).
 *   2) UNINITIALIZED 상태면 transport_qpair_fini만 호출하고 즉시 반환 (Connect 미완료 케이스).
 *   3) cross-thread 호출이면 atomic clear 후 group->thread로 send_msg해 재진입 (qpair는 그 thread 소유).
 *   4) 같은 thread면 상태를 DEACTIVATING으로 전이.
 *   5) outstanding I/O가 있으면 drain까지 대기 (state_cb=_nvmf_qpair_destroy 등록 + abort 시작).
 *      drain 완료 시 ctrlr.c의 cmd 완료 경로가 state_cb 호출 → _nvmf_qpair_destroy.
 *   6) 없으면 즉시 _nvmf_qpair_destroy 호출 (transport_qpair_fini까지 chain).
 *
 * 실행 컨텍스트: 어느 thread에서나 호출 가능. 단, 핵심 정리는 group->thread에서 직렬화.
 *
 * 호출 체인:
 *   호스트 disconnect / 트랜스포트 에러 / tgt destroy -> [본 함수]
 *     -> _nvmf_qpair_destroy -> spdk_nvmf_poll_group_remove -> nvmf_transport_qpair_fini
 *     -> _nvmf_transport_qpair_fini_complete -> _nvmf_ctrlr_free_from_qpair (ctrlr->thread)
 *     -> 마지막 qpair면 _nvmf_ctrlr_destruct (subsys->thread)
 */
int
spdk_nvmf_qpair_disconnect(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_poll_group *group = qpair->group; /* [한국어] qpair가 속한 group (NULL 가능: UNINITIALIZED 케이스) */
	struct nvmf_qpair_disconnect_ctx *qpair_ctx;     /* [한국어] 비동기 단계 ctx */

	if (__atomic_test_and_set(&qpair->disconnect_started, __ATOMIC_RELAXED)) { /* [한국어] atomic 1로 set, 이전 값 반환 - true면 이미 다른 호출이 진행 중 */
		return -EINPROGRESS;                     /* [한국어] 중복 disconnect 방지 - 호출자는 단일 정리를 보장받음 */
	}

	/* If we get a qpair in the uninitialized state, we can just destroy it immediately */
	if (qpair->state == SPDK_NVMF_QPAIR_UNINITIALIZED) { /* [한국어] Connect가 완료되지 않은 qpair - group/ctrlr 결합 전 */
		nvmf_transport_qpair_fini(qpair, NULL, NULL); /* [한국어] 트랜스포트 자원만 해제 (cb 없음) */
		return 0;
	}

	assert(group != NULL);                           /* [한국어] UNINITIALIZED가 아니면 group이 반드시 결합되어 있어야 */
	if (spdk_get_thread() != group->thread) {        /* [한국어] cross-thread 호출 - 정리는 group->thread로 위임해야 lockless 보장 */
		/* clear the atomic so we can set it on the next call on the proper thread. */
		__atomic_clear(&qpair->disconnect_started, __ATOMIC_RELAXED); /* [한국어] atomic 리셋 - group thread에서 다시 set하도록 */
		qpair_ctx = calloc(1, sizeof(struct nvmf_qpair_disconnect_ctx)); /* [한국어] msg 인자용 ctx alloc */
		if (!qpair_ctx) {
			SPDK_ERRLOG("Unable to allocate context for nvmf_qpair_disconnect\n");
			return -ENOMEM;
		}
		qpair_ctx->qpair = qpair;
		spdk_thread_send_msg(group->thread, _nvmf_qpair_disconnect_msg, qpair_ctx); /* [한국어] group->thread에서 _nvmf_qpair_disconnect_msg가 본 함수 재호출 */
		return 0;
	}

	SPDK_DTRACE_PROBE2_TICKS(nvmf_qpair_disconnect, qpair, spdk_thread_get_id(group->thread)); /* [한국어] DTrace - disconnect 시작 시점 */
	assert(spdk_nvmf_qpair_is_active(qpair));        /* [한국어] active 상태 (CONNECTING/AUTHENTICATING/ACTIVE 등)에서만 정상 진입 */
	nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_DEACTIVATING); /* [한국어] 상태 전이 - 이후 새 명령 dispatch 불가 */

	qpair_ctx = calloc(1, sizeof(struct nvmf_qpair_disconnect_ctx)); /* [한국어] 본격적 정리용 ctx alloc */
	if (!qpair_ctx) {
		SPDK_ERRLOG("Unable to allocate context for nvmf_qpair_disconnect\n");
		return -ENOMEM;
	}

	qpair_ctx->qpair = qpair;                        /* [한국어] 정리 대상 보존 */

	/* Check for outstanding I/O */
	if (!TAILQ_EMPTY(&qpair->outstanding)) {         /* [한국어] 진행 중 I/O가 있으면 drain 후 정리 */
		SPDK_DTRACE_PROBE2_TICKS(nvmf_poll_group_drain_qpair, qpair, spdk_thread_get_id(group->thread)); /* [한국어] DTrace - drain 시작 */
		qpair->state_cb = _nvmf_qpair_destroy;   /* [한국어] drain 완료 시 호출될 cb 등록 (마지막 outstanding 완료 시 ctrlr.c가 호출) */
		qpair->state_cb_arg = qpair_ctx;         /* [한국어] cb에 전달할 ctx */
		nvmf_qpair_abort_pending_zcopy_reqs(qpair); /* [한국어] zcopy 진행 중 요청 abort - drain 가속 */
		nvmf_qpair_free_aer(qpair);              /* [한국어] AER(Async Event Request) 명령 즉시 응답 처리 - drain 가속 */
		return 0;                                /* [한국어] 비동기 진행 중 - 사용자에게는 성공 통지 (실제 정리는 cb에서) */
	}

	_nvmf_qpair_destroy(qpair_ctx, 0);               /* [한국어] outstanding 없음 - 동기적으로 즉시 정리 */

	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_qpair_get_peer_trid - qpair의 원격 호스트 transport_id 조회 (외부 공개 API)
 *
 * @qpair: 조회할 qpair
 * @trid: 결과를 채울 출력 버퍼 (호출자가 제공)
 * @return: 0 성공 / 음수 트랜스포트 에러
 *
 * spdk_nvme_transport_id는 트랜스포트 종류(TCP/RDMA/FC), 주소(traddr), 서비스(trsvcid), NQN 등을
 * 담는 공통 식별자. peer는 호스트(원격) 측을 의미. 진단/로깅 용도로 자주 호출.
 *
 * 실행 컨텍스트: 어느 thread에서나 호출 가능 (트랜스포트별 ops가 thread-safe해야).
 *
 * 호출 체인:
 *   RPC nvmf_subsystem_get_qpairs / 디버그 코드 -> [본 함수]
 *     -> nvmf_transport_qpair_get_peer_trid (트랜스포트별 ops)
 */
int
spdk_nvmf_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	memset(trid, 0, sizeof(*trid));                  /* [한국어] 출력 버퍼 zero-init - 트랜스포트가 부분적으로 채워도 나머지가 stale 되지 않도록 */
	return nvmf_transport_qpair_get_peer_trid(qpair, trid); /* [한국어] 트랜스포트별 ops 호출 - TCP는 getpeername(), RDMA는 RDMA CM API */
}

/*
 * [한국어]
 * spdk_nvmf_qpair_get_local_trid - qpair의 로컬(자기 측) transport_id 조회 (외부 공개 API)
 *
 * @qpair: 조회 qpair
 * @trid: 출력 버퍼
 * @return: 0 / 음수
 *
 * 자기 측 listen 주소/포트 정보. multi-listener 환경에서 어느 listener를 통해 들어왔는지 추적용.
 *
 * 호출 체인:
 *   진단/RPC -> [본 함수] -> nvmf_transport_qpair_get_local_trid
 */
int
spdk_nvmf_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			       struct spdk_nvme_transport_id *trid)
{
	memset(trid, 0, sizeof(*trid));                  /* [한국어] zero-init */
	return nvmf_transport_qpair_get_local_trid(qpair, trid); /* [한국어] 트랜스포트별 ops - TCP는 getsockname() */
}

/*
 * [한국어]
 * spdk_nvmf_qpair_get_listen_trid - qpair을 받아준 listener의 transport_id 조회 (외부 공개 API)
 *
 * @qpair: 조회 qpair
 * @trid: 출력 버퍼
 * @return: 0 / 음수
 *
 * local_trid와 비슷하지만 NQN(subsystem) 정보까지 포함될 수 있어, "이 qpair은 어떤 listener subnqn에
 * 매칭되어 들어왔는가"를 답한다. ANA/Discovery 응답 생성에 활용.
 *
 * 호출 체인:
 *   ctrlr.c의 Discovery 처리 / RPC -> [본 함수] -> nvmf_transport_qpair_get_listen_trid
 */
int
spdk_nvmf_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				struct spdk_nvme_transport_id *trid)
{
	memset(trid, 0, sizeof(*trid));                  /* [한국어] zero-init */
	return nvmf_transport_qpair_get_listen_trid(qpair, trid); /* [한국어] 트랜스포트별 ops */
}

/*
 * [한국어]
 * poll_group_update_subsystem - poll_group의 subsystem 상태(namespace 추가/삭제/리사이즈/ANA 변경)를 동기화 (내부 헬퍼)
 *
 * @group: 갱신 대상 poll_group (현재 thread에서 처리)
 * @subsystem: 갱신 원본 subsystem (메인 thread에서 변경된 상태)
 * @return: 0 성공 / -ENOMEM
 *
 * NVMe-oF subsystem에 namespace(NS)가 추가/제거되거나 backing bdev가 교체/리사이즈되면, 모든
 * poll_group의 sgroup에 그 변화를 전파해야 한다. 이 함수가 한 group의 sgroup을 동기화하는 핵심.
 *
 * 동작 단계:
 *   1) sgroup->ns_info 배열 크기를 subsystem->max_nsid에 맞춰 (첫 호출이면) 할당.
 *   2) 각 ns_info에 대해 4가지 케이스 처리:
 *      a) NS 사라짐 (ns=NULL, ch≠NULL): bdev io_channel put, ns_changed.
 *      b) NS 새로 등장 (ns≠NULL, ch=NULL): bdev io_channel 획득, ns_changed.
 *      c) NS 교체됨 (UUID 다름): old ch put + new ch 획득, ns_changed.
 *      d) NS 크기 변경 또는 ANA 그룹 ID 변경 -> ns_changed/ana_changed 플래그.
 *   3) 변화가 있었으면 같은 thread의 ctrlr들에게 AER(Asynchronous Event Request) 통지.
 *
 * 핵심 자료구조:
 *   - spdk_nvmf_subsystem_pg_ns_info: 각 NS에 대해 (channel, uuid, num_blocks, anagrpid, reservation) 캐시.
 *   - subsystem->ns[i]: NS 1-based가 아닌 0-based 배열 (i+1이 nsid).
 *
 * 실행 컨텍스트: group->thread (각 reactor에서 메시지로 디스패치되어 호출됨).
 *
 * 호출 체인:
 *   subsystem.c의 NS 추가/삭제 RPC -> 모든 group에 send_msg
 *     -> nvmf_poll_group_update_subsystem -> [본 함수]
 *     -> spdk_bdev_get_io_channel / spdk_put_io_channel
 *     -> nvmf_ctrlr_async_event_ns_notice / nvmf_ctrlr_async_event_ana_change_notice
 */
static int
poll_group_update_subsystem(struct spdk_nvmf_poll_group *group,
			    struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;   /* [한국어] subsystem id별 poll_group 슬롯 */
	uint32_t i;                                      /* [한국어] NS 인덱스 (0-based) */
	struct spdk_nvmf_ns *ns;                         /* [한국어] subsystem의 i번 NS (NULL 가능: NS 슬롯 비어있음) */
	struct spdk_io_channel *ch;                      /* [한국어] bdev io_channel - ns_info에 캐시 */
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;  /* [한국어] sgroup의 i번 ns_info 슬롯 */
	struct spdk_nvmf_ctrlr *ctrlr;                   /* [한국어] AER 통지 대상 ctrlr 순회용 */
	bool ns_changed, ana_changed;                    /* [한국어] 어떤 변화가 있었는지 플래그 (둘 다 AER 트리거) */

	/* Make sure our poll group has memory for this subsystem allocated */
	if (subsystem->id >= group->num_sgroups) {       /* [한국어] sgroups 배열 범위 검사 - subsystem 추가가 group 생성 후라면 미리 grown 되어야 함 */
		return -ENOMEM;
	}

	sgroup = &group->sgroups[subsystem->id];         /* [한국어] subsystem id로 sgroup 인덱싱 */

	/* Make sure the array of namespace information is the correct size */
	if (sgroup->num_ns == 0 && subsystem->max_nsid > 0) { /* [한국어] 첫 NS 할당 - sgroup에 ns_info 배열이 없으면 새로 만든다 */
		/* First allocation */
		sgroup->ns_info = calloc(subsystem->max_nsid, sizeof(struct spdk_nvmf_subsystem_pg_ns_info)); /* [한국어] max_nsid 크기 zero-init 배열 할당 */
		if (!sgroup->ns_info) {                  /* [한국어] OOM */
			return -ENOMEM;
		}
		sgroup->num_ns = subsystem->max_nsid;    /* [한국어] 배열 크기 보존 - 이후 비교에 사용 */
	}

	ns_changed = false;                              /* [한국어] NS 추가/삭제/UUID 변경/리사이즈 발생 여부 */
	ana_changed = false;                             /* [한국어] ANA(Asymmetric Namespace Access) 그룹 변경 여부 */

	/* Detect bdevs that were added or removed */
	for (i = 0; i < sgroup->num_ns; i++) {           /* [한국어] 모든 NS 슬롯 검사 */
		ns = subsystem->ns[i];                   /* [한국어] subsystem이 가진 i번 NS (메인 thread가 갱신한 최신 상태) */
		ns_info = &sgroup->ns_info[i];           /* [한국어] sgroup이 가진 i번 NS 캐시 (이전 thread-local 상태) */
		ch = ns_info->channel;                   /* [한국어] 이전 io_channel (없을 수도 있음) */

		if (ns == NULL && ch == NULL) {          /* [한국어] 양쪽 모두 NULL - 변화 없음 (빈 슬롯) */
			/* Both NULL. Leave empty */
		} else if (ns == NULL && ch != NULL) {   /* [한국어] NS 삭제됨 - 캐시된 channel을 정리해야 */
			/* There was a channel here, but the namespace is gone. */
			ns_changed = true;               /* [한국어] AER 통지 트리거 */
			spdk_put_io_channel(ch);         /* [한국어] bdev io_channel ref-1 - 마지막이면 destroy */
			ns_info->channel = NULL;         /* [한국어] 캐시 클리어 */
		} else if (ns != NULL && ch == NULL) {   /* [한국어] NS 새로 등장 - bdev io_channel 획득 필요 */
			/* A namespace appeared but there is no channel yet */
			ns_changed = true;
			ch = spdk_bdev_get_io_channel(ns->desc); /* [한국어] bdev에 새 io_channel 요청 (현재 thread용) */
			if (ch == NULL) {                /* [한국어] 채널 획득 실패 */
				SPDK_ERRLOG("Could not allocate I/O channel.\n");
				return -ENOMEM;
			}
			ns_info->channel = ch;           /* [한국어] 캐시에 저장 - 이후 I/O 명령은 이 channel로 디스패치 */
		} else if (spdk_uuid_compare(&ns_info->uuid, spdk_bdev_get_uuid(ns->bdev)) != 0) { /* [한국어] 같은 슬롯에 다른 backing bdev로 교체됨 (UUID로 식별) */
			/* A namespace was here before, but was replaced by a new one. */
			ns_changed = true;
			spdk_put_io_channel(ns_info->channel); /* [한국어] 옛 bdev io_channel 반환 */
			memset(ns_info, 0, sizeof(*ns_info)); /* [한국어] 캐시 통째로 초기화 - reservation 등 stale 데이터 제거 */

			ch = spdk_bdev_get_io_channel(ns->desc); /* [한국어] 새 bdev에 io_channel 요청 */
			if (ch == NULL) {
				SPDK_ERRLOG("Could not allocate I/O channel.\n");
				return -ENOMEM;
			}
			ns_info->channel = ch;
		} else if (ns_info->num_blocks != spdk_bdev_get_num_blocks(ns->bdev)) { /* [한국어] 같은 NS인데 block 수만 변함 - 리사이즈 (online resize 지원) */
			/* Namespace is still there but size has changed */
			SPDK_DEBUGLOG(nvmf, "Namespace resized: subsystem_id %u,"
				      " nsid %u, pg %p, old %" PRIu64 ", new %" PRIu64 "\n",
				      subsystem->id,
				      ns->nsid,
				      group,
				      ns_info->num_blocks,
				      spdk_bdev_get_num_blocks(ns->bdev));
			ns_changed = true;               /* [한국어] AER로 호스트에 알림 (NS Attribute Changed) */
		} else if (ns_info->anagrpid != ns->anagrpid) { /* [한국어] 같은 NS인데 ANA 그룹만 변경 (다중 경로 라우팅 변경) */
			/* Namespace is still there but ANA group ID has changed */
			SPDK_DEBUGLOG(nvmf, "ANA group ID changed: subsystem_id %u,"
				      "nsid %u, pg %p, old %u, new %u\n",
				      subsystem->id,
				      ns->nsid,
				      group,
				      ns_info->anagrpid,
				      ns->anagrpid);
			ana_changed = true;              /* [한국어] AER로 호스트에 ANA Change Notice 통보 */
		}

		if (ns == NULL) {                        /* [한국어] NS가 없으면 캐시 통째로 클리어 */
			memset(ns_info, 0, sizeof(*ns_info));
		} else {                                 /* [한국어] NS 있음 - 캐시 갱신 */
			ns_info->uuid = *spdk_bdev_get_uuid(ns->bdev); /* [한국어] UUID 캐시 (다음 update 호출 비교용) */
			ns_info->num_blocks = spdk_bdev_get_num_blocks(ns->bdev); /* [한국어] 블록 수 캐시 */
			ns_info->anagrpid = ns->anagrpid; /* [한국어] ANA 그룹 ID 캐시 */
			nvmf_subsystem_poll_group_update_ns_reservation(ns, ns_info); /* [한국어] reservation(NVMe 예약) 상태도 sgroup-local 복사 */
		}
	}

	if (ns_changed || ana_changed) {                 /* [한국어] 변화가 있으면 영향받는 ctrlr에 AER 발사 */
		TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) { /* [한국어] subsystem의 모든 ctrlr 순회 */
			if (ctrlr->thread != spdk_get_thread()) { /* [한국어] 현재 thread가 ctrlr 소유 thread가 아니면 스킵 - 다른 그룹의 update 호출에서 처리 */
				continue;
			}
			/* It is possible that a ctrlr was added but the admin_qpair hasn't been
			 * assigned yet.
			 */
			if (!ctrlr->admin_qpair) {       /* [한국어] admin qpair 미결합 ctrlr - AER 보낼 수단 없음 */
				continue;
			}
			if (ctrlr->admin_qpair->group == group) { /* [한국어] admin qpair이 이 group에 속한 ctrlr만 처리 (현재 thread에 admin이 있는 경우) */
				if (ns_changed) {
					nvmf_ctrlr_async_event_ns_notice(ctrlr); /* [한국어] NS Attribute Changed AER (NVMe 1.3+) */
				}
				if (ana_changed) {
					nvmf_ctrlr_async_event_ana_change_notice(ctrlr); /* [한국어] ANA Change AER (NVMe 1.4+) */
				}
			}
		}
	}

	return 0;                                        /* [한국어] 모든 NS 처리 성공 */
}

/*
 * [한국어]
 * nvmf_poll_group_update_subsystem - poll_group의 subsystem 상태 동기화 (내부 라이브러리 API 진입점)
 *
 * @group: 대상 group
 * @subsystem: 대상 subsystem
 * @return: 0 / -ENOMEM
 *
 * 같은 lib/nvmf/ 내부 모듈(subsystem.c 등)에서 호출하는 wrapper. 정적 함수
 * poll_group_update_subsystem을 외부에서 호출 가능하게 노출.
 *
 * 호출 체인:
 *   subsystem.c (NS 변경 메시지 수신) -> [본 함수] -> poll_group_update_subsystem
 */
int
nvmf_poll_group_update_subsystem(struct spdk_nvmf_poll_group *group,
				 struct spdk_nvmf_subsystem *subsystem)
{
	return poll_group_update_subsystem(group, subsystem); /* [한국어] 정적 헬퍼로 위임 */
}

/*
 * [한국어]
 * nvmf_poll_group_add_subsystem - poll_group에 subsystem을 결합 (내부 라이브러리 API)
 *
 * @group: 대상 group
 * @subsystem: 결합할 subsystem
 * @cb_fn: 완료 콜백 (NULL 가능)
 * @cb_arg: cb_fn 인자
 * @return: 0 성공 / 음수 에러
 *
 * subsystem이 새로 ACTIVE 상태로 시작될 때, 모든 poll_group에 sgroup 슬롯을 활성화하기 위해 호출.
 *
 * 동작:
 *   1) 사전 조건: sgroup->queued가 비어 있어야 함 (방어적: 비어있지 않으면 모두 free).
 *   2) poll_group_update_subsystem으로 NS 캐시 동기화.
 *   3) sgroup->state = ACTIVE, 모든 ns_info[i].state = ACTIVE.
 *
 * 실행 컨텍스트: group->thread.
 *
 * 호출 체인:
 *   subsystem.c subsystem_state_change -> 모든 group에 send_msg
 *     -> [본 함수] -> poll_group_update_subsystem (성공) 또는 nvmf_poll_group_remove_subsystem (실패 rollback)
 */
int
nvmf_poll_group_add_subsystem(struct spdk_nvmf_poll_group *group,
			      struct spdk_nvmf_subsystem *subsystem,
			      spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
	int rc = 0;                                      /* [한국어] update 결과 */
	struct spdk_nvmf_subsystem_poll_group *sgroup = &group->sgroups[subsystem->id]; /* [한국어] 결합 대상 sgroup 슬롯 */
	struct spdk_nvmf_request *req, *tmp;             /* [한국어] queued 요청 정리용 */
	uint32_t i;                                      /* [한국어] ns_info 순회 인덱스 */

	if (!TAILQ_EMPTY(&sgroup->queued)) {             /* [한국어] 방어적 검증 - add 시점에는 queued가 비어 있어야 정상 */
		SPDK_ERRLOG("sgroup->queued not empty when adding subsystem\n");
		TAILQ_FOREACH_SAFE(req, &sgroup->queued, link, tmp) { /* [한국어] 비정상 적체 - 모두 free */
			TAILQ_REMOVE(&sgroup->queued, req, link);
			nvmf_transport_req_free(req);
		}
	}

	rc = poll_group_update_subsystem(group, subsystem); /* [한국어] NS 캐시 동기화 (ns_info 배열 첫 할당 + bdev io_channel 획득) */
	if (rc) {                                        /* [한국어] update 실패 - rollback */
		nvmf_poll_group_remove_subsystem(group, subsystem, NULL, NULL); /* [한국어] sgroup 정리 (cb 없이 fire-and-forget) */
		goto fini;
	}

	sgroup->state = SPDK_NVMF_SUBSYSTEM_ACTIVE;      /* [한국어] sgroup ACTIVE - 이후 이 group에서 본 subsystem 명령 수락 */

	for (i = 0; i < sgroup->num_ns; i++) {           /* [한국어] 모든 NS 슬롯 ACTIVE로 전이 */
		sgroup->ns_info[i].state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	}

fini:
	if (cb_fn) {                                     /* [한국어] 사용자 cb 통지 (성공/실패 상태 그대로 전달) */
		cb_fn(cb_arg, rc);
	}

	SPDK_DTRACE_PROBE2_TICKS(nvmf_poll_group_add_subsystem, spdk_thread_get_id(group->thread),
				 subsystem->subnqn);                /* [한국어] DTrace - 어떤 thread에 어떤 subsystem이 결합되는지 */

	return rc;                                       /* [한국어] 호출자에게도 즉시 반환 (cb와 별개로) */
}

/*
 * [한국어]
 * _nvmf_poll_group_remove_subsystem_cb - subsystem 제거 완료 콜백 (모든 qpair drain 후 호출)
 *
 * @ctx: nvmf_qpair_disconnect_many_ctx
 * @status: drain 결과 (0=성공)
 *
 * 모든 qpair이 disconnect된 후 sgroup의 NS 캐시(io_channel 등)를 정리하고 사용자 cb에 통지.
 *
 * 단계:
 *   1) ctx에서 group/subsystem/cpl_fn/cpl_ctx 추출.
 *   2) status≠0이면 곧장 fini로 점프 (정리 스킵).
 *   3) sgroup의 모든 ns_info[].channel을 put_io_channel.
 *   4) sgroup->ns_info 배열 free, num_ns=0.
 *   5) ctx free + cpl_fn(cpl_ctx, status) 호출.
 *
 * 실행 컨텍스트: group->thread.
 *
 * 호출 체인:
 *   nvmf_poll_group_remove_subsystem_msg (qpair 모두 drain) -> [본 함수]
 *     -> spdk_put_io_channel (각 NS) -> 사용자 cb_fn
 */
static void
_nvmf_poll_group_remove_subsystem_cb(void *ctx, int status)
{
	struct nvmf_qpair_disconnect_many_ctx *qpair_ctx = ctx; /* [한국어] disconnect-many ctx 복원 */
	struct spdk_nvmf_subsystem *subsystem;           /* [한국어] 제거 대상 subsystem */
	struct spdk_nvmf_poll_group *group;              /* [한국어] 대상 group */
	struct spdk_nvmf_subsystem_poll_group *sgroup;   /* [한국어] sgroup 슬롯 */
	spdk_nvmf_poll_group_mod_done cpl_fn = NULL;     /* [한국어] 사용자 통지 cb */
	void *cpl_ctx = NULL;                            /* [한국어] cb 인자 */
	uint32_t nsid;                                   /* [한국어] NS 인덱스 */

	group = qpair_ctx->group;                        /* [한국어] ctx에서 추출 */
	subsystem = qpair_ctx->subsystem;
	cpl_fn = qpair_ctx->cpl_fn;
	cpl_ctx = qpair_ctx->cpl_ctx;
	sgroup = &group->sgroups[subsystem->id];         /* [한국어] sgroup 슬롯 인덱싱 */

	if (status) {                                    /* [한국어] drain 단계에서 에러 - 정리 단계 스킵하고 cb만 통지 */
		goto fini;
	}

	for (nsid = 0; nsid < sgroup->num_ns; nsid++) {  /* [한국어] 모든 NS 캐시의 io_channel 정리 */
		if (sgroup->ns_info[nsid].channel) {     /* [한국어] 활성 channel만 put */
			spdk_put_io_channel(sgroup->ns_info[nsid].channel); /* [한국어] bdev io_channel ref-1 */
			sgroup->ns_info[nsid].channel = NULL; /* [한국어] 캐시 클리어 */
		}
	}

	sgroup->num_ns = 0;                              /* [한국어] NS 배열 무효화 */
	free(sgroup->ns_info);                           /* [한국어] ns_info 배열 자체 해제 */
	sgroup->ns_info = NULL;                          /* [한국어] 포인터 NULL - 다음 add 호출 시 재할당 트리거 */
fini:
	free(qpair_ctx);                                 /* [한국어] disconnect-many ctx 해제 */
	if (cpl_fn) {                                    /* [한국어] 사용자 cb 호출 (status 그대로 전달) */
		cpl_fn(cpl_ctx, status);
	}
}

/* [한국어] forward declaration - 자기 자신에게 send_msg하는 폴링 패턴(타이트 retry) 때문에 필요 */
static void nvmf_poll_group_remove_subsystem_msg(void *ctx);

/*
 * [한국어]
 * nvmf_poll_group_remove_subsystem_msg - subsystem 제거 시 모든 qpair을 drain하는 retry 메시지 핸들러
 *
 * @ctx: nvmf_qpair_disconnect_many_ctx
 *
 * 동작 패턴 (self-loop):
 *   1) group의 qpairs를 순회하며 ctrlr.subsys==target subsystem인 qpair을 disconnect.
 *   2) 하나도 발견 못 하면 _cb 호출하고 종료.
 *   3) 발견했으면 disconnect는 비동기이므로 곧장 자기 자신에게 send_msg해 다음 round 진행.
 *
 * 이 패턴은 disconnect가 비동기(EINPROGRESS)라서 실제 정리 완료를 기다려야 하는데, 별도
 * 콜백 체인을 만들지 않고 매 메시지 사이클마다 한 번 더 검사하는 단순한 폴링 방식이다.
 *
 * 실행 컨텍스트: group->thread (자기 자신에게 send_msg).
 *
 * 호출 체인:
 *   nvmf_poll_group_remove_subsystem -> [본 함수]
 *     -> qpair 발견: spdk_nvmf_qpair_disconnect + spdk_thread_send_msg(self, [본 함수])
 *     -> 모두 정리됨: _nvmf_poll_group_remove_subsystem_cb
 */
static void
nvmf_poll_group_remove_subsystem_msg(void *ctx)
{
	struct spdk_nvmf_qpair *qpair, *qpair_tmp;       /* [한국어] FOREACH_SAFE 순회용 */
	struct spdk_nvmf_subsystem *subsystem;           /* [한국어] 제거 대상 subsystem */
	struct spdk_nvmf_poll_group *group;              /* [한국어] 대상 group */
	struct nvmf_qpair_disconnect_many_ctx *qpair_ctx = ctx; /* [한국어] ctx 복원 */
	bool qpairs_found = false;                       /* [한국어] 이번 round에서 disconnect 대상을 찾았는가 */
	int rc = 0;                                      /* [한국어] disconnect 결과 */

	group = qpair_ctx->group;
	subsystem = qpair_ctx->subsystem;

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, qpair_tmp) { /* [한국어] group의 모든 qpair 순회 */
		if ((qpair->ctrlr != NULL) && (qpair->ctrlr->subsys == subsystem)) { /* [한국어] 본 subsystem에 속한 qpair만 정리 대상 */
			qpairs_found = true;             /* [한국어] 이번 round 정리 대상 발견 */
			rc = spdk_nvmf_qpair_disconnect(qpair); /* [한국어] qpair 비동기 disconnect 시작 */
			if (rc && rc != -EINPROGRESS) {  /* [한국어] -EINPROGRESS는 이미 진행 중인 정상 케이스, 그 외 에러는 break */
				break;
			}
		}
	}

	if (!qpairs_found) {                             /* [한국어] 정리 대상 모두 사라짐 - 정리 단계로 */
		_nvmf_poll_group_remove_subsystem_cb(ctx, 0);
		return;
	}

	/* Some qpairs are in process of being disconnected. Send a message and try to remove them again */
	spdk_thread_send_msg(spdk_get_thread(), nvmf_poll_group_remove_subsystem_msg, ctx); /* [한국어] 자기 자신에게 send_msg - 다음 라운드 검사 (지연 폴링) */
}

/*
 * [한국어]
 * nvmf_poll_group_remove_subsystem - poll_group에서 subsystem을 제거 (내부 라이브러리 API)
 *
 * @group: 대상 group
 * @subsystem: 제거할 subsystem
 * @cb_fn: 완료 콜백
 * @cb_arg: cb_fn 인자
 *
 * 동작:
 *   1) ctx 할당 + sgroup state INACTIVE 전이 (이후 새 명령 거부).
 *   2) ns_info[].state도 모두 INACTIVE.
 *   3) nvmf_poll_group_remove_subsystem_msg 호출 (qpair drain 폴링 시작).
 *   4) 모든 qpair drain 완료 시 _cb가 NS 캐시 정리하고 사용자 cb 호출.
 *
 * 실행 컨텍스트: group->thread.
 *
 * 호출 체인:
 *   subsystem state change (PAUSED/INACTIVE) -> 모든 group send_msg -> [본 함수]
 *     -> nvmf_poll_group_remove_subsystem_msg (drain 폴링)
 *     -> _nvmf_poll_group_remove_subsystem_cb (NS 캐시 정리 + 사용자 cb)
 */
void
nvmf_poll_group_remove_subsystem(struct spdk_nvmf_poll_group *group,
				 struct spdk_nvmf_subsystem *subsystem,
				 spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;   /* [한국어] sgroup 슬롯 */
	struct nvmf_qpair_disconnect_many_ctx *ctx;      /* [한국어] disconnect-many ctx (drain 폴링 + 정리 cb 정보) */
	uint32_t i;                                      /* [한국어] ns_info 순회 */

	SPDK_DTRACE_PROBE3_TICKS(nvmf_poll_group_remove_subsystem, group, spdk_thread_get_id(group->thread),
				 subsystem->subnqn);                /* [한국어] DTrace - 어떤 group/thread에서 어떤 subsystem이 제거되는지 */

	ctx = calloc(1, sizeof(struct nvmf_qpair_disconnect_many_ctx)); /* [한국어] zero-init ctx */
	if (!ctx) {                                      /* [한국어] OOM */
		SPDK_ERRLOG("Unable to allocate memory for context to remove poll subsystem\n");
		if (cb_fn) {
			cb_fn(cb_arg, -1);               /* [한국어] -1로 즉시 통지 */
		}
		return;
	}

	ctx->group = group;                              /* [한국어] ctx 채우기 */
	ctx->subsystem = subsystem;
	ctx->cpl_fn = cb_fn;
	ctx->cpl_ctx = cb_arg;

	sgroup = &group->sgroups[subsystem->id];         /* [한국어] sgroup 슬롯 인덱싱 */
	sgroup->state = SPDK_NVMF_SUBSYSTEM_INACTIVE;    /* [한국어] 즉시 INACTIVE - 이후 새 명령은 모두 거부됨 */

	for (i = 0; i < sgroup->num_ns; i++) {           /* [한국어] 모든 NS 슬롯도 INACTIVE로 (NS 단위 명령도 거부) */
		sgroup->ns_info[i].state = SPDK_NVMF_SUBSYSTEM_INACTIVE;
	}

	nvmf_poll_group_remove_subsystem_msg(ctx);       /* [한국어] qpair drain 폴링 시작 (자기 자신에게 send_msg하는 retry 패턴) */
}

/*
 * [한국어]
 * nvmf_poll_group_pause_subsystem - poll_group의 subsystem을 일시 정지 (내부 라이브러리 API)
 *
 * @group: 대상 group
 * @subsystem: 정지할 subsystem
 * @nsid: 1-based NS ID, SPDK_NVME_GLOBAL_NS_TAG(=UINT32_MAX)면 모든 NS 정지
 * @cb_fn: 완료 콜백
 * @cb_arg: cb_fn 인자
 *
 * subsystem live-update(NS 추가/제거 등) 전에 dataplane을 잠시 정지하는 mechanism.
 *
 * 상태 머신: ACTIVE -> PAUSING -> PAUSED.
 *
 * 동작:
 *   1) sgroup이 이미 PAUSED면 즉시 cb 호출하고 return (idempotent).
 *   2) sgroup->state = PAUSING. 대상 ns_info[].state = PAUSING.
 *   3) mgmt_io 또는 ns_info[].io_outstanding이 있으면 cb를 sgroup에 보존하고 return —
 *      마지막 outstanding 완료 시 ctrlr.c가 cb를 트리거한다.
 *   4) outstanding=0이면 즉시 PAUSED로 전이 + cb 호출.
 *
 * 핵심 트릭: nsid - 1 < sgroup->num_ns 검사는 nsid=0(invalid)도 implicit하게 거부한다.
 * (0 - 1 = UINT32_MAX이므로 num_ns보다 큼)
 *
 * 실행 컨텍스트: group->thread.
 *
 * 호출 체인:
 *   subsystem.c subsystem_state_change(PAUSED) -> 모든 group send_msg -> [본 함수]
 *     -> outstanding 0: 즉시 cb
 *     -> outstanding ≠0: ctrlr.c의 cmd 완료 시점에 cb 트리거
 */
void
nvmf_poll_group_pause_subsystem(struct spdk_nvmf_poll_group *group,
				struct spdk_nvmf_subsystem *subsystem,
				uint32_t nsid,
				spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;   /* [한국어] sgroup 슬롯 */
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info = NULL; /* [한국어] 단일 NS 모드일 때 그 NS의 ns_info 백업 */
	int rc = 0;                                      /* [한국어] 결과 */
	uint32_t i;                                      /* [한국어] NS 순회 인덱스 */

	if (subsystem->id >= group->num_sgroups) {       /* [한국어] sgroups 배열 범위 검사 */
		rc = -1;
		goto fini;
	}

	sgroup = &group->sgroups[subsystem->id];         /* [한국어] sgroup 슬롯 인덱싱 */
	if (sgroup->state == SPDK_NVMF_SUBSYSTEM_PAUSED) { /* [한국어] 이미 PAUSED - idempotent (cb로 0 통지하고 종료) */
		goto fini;
	}
	sgroup->state = SPDK_NVMF_SUBSYSTEM_PAUSING;     /* [한국어] PAUSING 상태 - 새 명령은 sgroup->queued로 적체됨 */

	if (nsid == SPDK_NVME_GLOBAL_NS_TAG) {           /* [한국어] 글로벌 NS 태그 (UINT32_MAX) - 모든 NS 정지 */
		for (i = 0; i < sgroup->num_ns; i++) {   /* [한국어] 모든 ns_info를 PAUSING으로 */
			ns_info = &sgroup->ns_info[i];
			ns_info->state = SPDK_NVMF_SUBSYSTEM_PAUSING;
		}
	} else {                                         /* [한국어] 특정 NS만 정지 (nsid - 1로 0-based 인덱스 변환) */
		/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
		if (nsid - 1 < sgroup->num_ns) {         /* [한국어] nsid=0이면 wrap-around로 검사 실패 - 0은 invalid NSID이므로 자연스럽게 거부 */
			ns_info  = &sgroup->ns_info[nsid - 1]; /* [한국어] 1-based -> 0-based */
			ns_info->state = SPDK_NVMF_SUBSYSTEM_PAUSING;
		}
	}

	if (sgroup->mgmt_io_outstanding > 0) {           /* [한국어] subsystem 단위 management I/O(예약/AER 등) 진행 중 - drain 후 cb */
		assert(sgroup->cb_fn == NULL);           /* [한국어] cb 슬롯이 비어 있어야 - 이전 pause/resume이 cb를 보존한 채 끝나면 안 됨 */
		sgroup->cb_fn = cb_fn;                   /* [한국어] cb 슬롯에 보존 - drain 완료 시 ctrlr.c가 호출 */
		assert(sgroup->cb_arg == NULL);
		sgroup->cb_arg = cb_arg;
		return;
	}

	if (nsid == SPDK_NVME_GLOBAL_NS_TAG) {           /* [한국어] 글로벌 모드 - 모든 NS의 outstanding 검사 */
		for (i = 0; i < sgroup->num_ns; i++) {
			ns_info = &sgroup->ns_info[i];

			if (ns_info->io_outstanding > 0) { /* [한국어] 이 NS에 진행 중 I/O 있음 - drain 대기 */
				assert(sgroup->cb_fn == NULL);
				sgroup->cb_fn = cb_fn;
				assert(sgroup->cb_arg == NULL);
				sgroup->cb_arg = cb_arg;
				return;
			}
		}
	} else {                                         /* [한국어] 단일 NS 모드 - 그 NS만 검사 */
		if (ns_info != NULL && ns_info->io_outstanding > 0) {
			assert(sgroup->cb_fn == NULL);
			sgroup->cb_fn = cb_fn;
			assert(sgroup->cb_arg == NULL);
			sgroup->cb_arg = cb_arg;
			return;
		}
	}

	assert(sgroup->mgmt_io_outstanding == 0);        /* [한국어] outstanding 모두 0 확인 */
	sgroup->state = SPDK_NVMF_SUBSYSTEM_PAUSED;      /* [한국어] PAUSING -> PAUSED 전이 - 즉시 정지 완료 */
fini:
	if (cb_fn) {                                     /* [한국어] outstanding 없거나 에러 케이스 - 즉시 사용자 cb */
		cb_fn(cb_arg, rc);
	}
}

/*
 * [한국어]
 * nvmf_poll_group_resume_subsystem - PAUSED subsystem을 ACTIVE로 재개 (내부 라이브러리 API)
 *
 * @group: 대상 group
 * @subsystem: 재개할 subsystem
 * @cb_fn: 완료 콜백
 * @cb_arg: cb_fn 인자
 *
 * 상태 머신: PAUSED -> (update 적용) -> ACTIVE.
 *
 * 동작:
 *   1) 이미 ACTIVE면 idempotent하게 즉시 cb.
 *   2) poll_group_update_subsystem 호출 - PAUSED 동안 변경된 NS 상태를 sgroup에 반영.
 *   3) ns_info[].state = ACTIVE, sgroup->state = ACTIVE.
 *   4) sgroup->queued에 적체된 요청을 모두 release - zcopy/일반 경로 분기로 exec.
 *
 * 실행 컨텍스트: group->thread.
 *
 * 호출 체인:
 *   subsystem.c subsystem_state_change(ACTIVE) -> 모든 group send_msg -> [본 함수]
 *     -> poll_group_update_subsystem -> spdk_nvmf_request_exec/zcopy_start (적체 요청)
 */
void
nvmf_poll_group_resume_subsystem(struct spdk_nvmf_poll_group *group,
				 struct spdk_nvmf_subsystem *subsystem,
				 spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
	struct spdk_nvmf_request *req, *tmp;             /* [한국어] queued 요청 release용 FOREACH_SAFE */
	struct spdk_nvmf_subsystem_poll_group *sgroup;   /* [한국어] sgroup 슬롯 */
	int rc = 0;                                      /* [한국어] 결과 */
	uint32_t i;                                      /* [한국어] ns_info 순회 인덱스 */

	if (subsystem->id >= group->num_sgroups) {       /* [한국어] sgroups 배열 범위 검사 */
		rc = -1;
		goto fini;
	}

	sgroup = &group->sgroups[subsystem->id];

	if (sgroup->state == SPDK_NVMF_SUBSYSTEM_ACTIVE) { /* [한국어] 이미 ACTIVE - idempotent */
		goto fini;
	}

	rc = poll_group_update_subsystem(group, subsystem); /* [한국어] PAUSED 동안 발생한 NS 변경 반영 (NS 추가/리사이즈 등) */
	if (rc) {                                        /* [한국어] update 실패 - resume도 실패 처리 */
		goto fini;
	}

	for (i = 0; i < sgroup->num_ns; i++) {           /* [한국어] 모든 NS 슬롯 ACTIVE로 */
		sgroup->ns_info[i].state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	}

	sgroup->state = SPDK_NVMF_SUBSYSTEM_ACTIVE;      /* [한국어] sgroup 본체도 ACTIVE - 이후 새 명령 정상 dispatch */

	/* Release all queued requests */
	TAILQ_FOREACH_SAFE(req, &sgroup->queued, link, tmp) { /* [한국어] PAUSED 동안 적체된 요청 모두 release */
		TAILQ_REMOVE(&sgroup->queued, req, link); /* [한국어] queue에서 분리 */
		if (spdk_nvmf_request_using_zcopy(req)) { /* [한국어] zero-copy 요청은 zcopy 경로로 (RDMA write 등) */
			spdk_nvmf_request_zcopy_start(req);
		} else {                                 /* [한국어] 일반 요청은 표준 exec 경로 */
			spdk_nvmf_request_exec(req);
		}

	}
fini:
	if (cb_fn) {                                     /* [한국어] 사용자 cb 통지 */
		cb_fn(cb_arg, rc);
	}
}


/*
 * [한국어]
 * spdk_nvmf_get_optimal_poll_group - qpair에 최적인 poll_group 반환 (외부 공개 API)
 *
 * @qpair: 신규 qpair (트랜스포트가 막 accept한 qpair)
 * @return: 최적 poll_group / NULL (트랜스포트가 추천 없음)
 *
 * 트랜스포트별 ops->get_optimal_poll_group이 구현되어 있으면 그 결과를 반환한다. 예를 들어
 * RDMA는 qpair의 cq affinity와 매칭되는 thread의 group을 추천 — 같은 코어에서 cq 폴링과
 * qpair 처리가 일어나면 cache locality 이점.
 *
 * 호출 체인:
 *   spdk_nvmf_tgt_new_qpair -> [본 함수]
 *     -> nvmf_transport_get_optimal_poll_group (트랜스포트별 ops->get_optimal_poll_group)
 */
struct spdk_nvmf_poll_group *
spdk_nvmf_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_transport_poll_group *tgroup;   /* [한국어] 트랜스포트별 추천 tgroup */

	tgroup = nvmf_transport_get_optimal_poll_group(qpair->transport, qpair); /* [한국어] 트랜스포트 ops 호출 - 추천 tgroup 반환 (없으면 NULL) */

	if (tgroup == NULL) {                            /* [한국어] 트랜스포트가 추천 없음 - 호출자가 round-robin으로 선택 */
		return NULL;
	}

	return tgroup->group;                            /* [한국어] tgroup의 부모 group 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_poll_group_dump_stat - poll_group의 통계를 JSON으로 직렬화 (외부 공개 API)
 *
 * @group: 대상 group
 * @w: spdk_json_write_ctx (JSON 출력 컨텍스트)
 *
 * RPC nvmf_get_stats 응답 생성에 호출된다. 각 poll_group마다:
 *   - name: thread 이름
 *   - admin_qpairs/io_qpairs: 누적 qpair 수
 *   - current_admin_qpairs/current_io_qpairs: 현재 활성 qpair 수
 *   - pending_bdev_io: bdev 큐에 대기 중인 I/O
 *   - completed_nvme_io: 완료된 NVMe I/O 수
 *   - transports[]: 트랜스포트별 통계 (ops->poll_group_dump_stat이 있으면 호출)
 *
 * trtype 필드는 backward compat 때문에 트랜스포트 "이름"을 담는다 (실제 NVMe trtype 코드 아님).
 *
 * 실행 컨텍스트: group->thread (group 통계는 그 thread만 갱신).
 *
 * 호출 체인:
 *   RPC nvmf_get_stats -> 모든 group send_msg -> [본 함수]
 *     -> spdk_json_write_* / 트랜스포트 ops->poll_group_dump_stat
 */
void
spdk_nvmf_poll_group_dump_stat(struct spdk_nvmf_poll_group *group, struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_transport_poll_group *tgroup;   /* [한국어] 트랜스포트별 통계 순회용 */

	spdk_json_write_object_begin(w);                 /* [한국어] JSON object 시작 */

	spdk_json_write_named_string(w, "name", spdk_thread_get_name(spdk_get_thread())); /* [한국어] thread 이름 (사람이 읽기 쉬운 식별자) */
	spdk_json_write_named_uint32(w, "admin_qpairs", group->stat.admin_qpairs); /* [한국어] 누적 admin qpair 수 (현재까지 결합된 총량) */
	spdk_json_write_named_uint32(w, "io_qpairs", group->stat.io_qpairs); /* [한국어] 누적 IO qpair 수 */
	spdk_json_write_named_uint32(w, "current_admin_qpairs", group->stat.current_admin_qpairs); /* [한국어] 현재 활성 admin qpair 수 */
	spdk_json_write_named_uint32(w, "current_io_qpairs", group->stat.current_io_qpairs); /* [한국어] 현재 활성 IO qpair 수 */
	spdk_json_write_named_uint64(w, "pending_bdev_io", group->stat.pending_bdev_io); /* [한국어] bdev 큐에 대기 중 I/O (백압 측정) */
	spdk_json_write_named_uint64(w, "completed_nvme_io", group->stat.completed_nvme_io); /* [한국어] 완료된 NVMe I/O 누적 카운터 (성능 측정) */

	spdk_json_write_named_array_begin(w, "transports"); /* [한국어] transports 배열 시작 */

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {   /* [한국어] group의 모든 transport poll_group 순회 */
		spdk_json_write_object_begin(w);         /* [한국어] 각 transport 정보 object 시작 */
		/*
		 * The trtype field intentionally contains a transport name as this is more informative.
		 * The field has not been renamed for backward compatibility.
		 */
		spdk_json_write_named_string(w, "trtype", spdk_nvmf_get_transport_name(tgroup->transport)); /* [한국어] backward compat - 실제 NVMe trtype 코드가 아닌 트랜스포트 이름 ("TCP", "RDMA") */

		if (tgroup->transport->ops->poll_group_dump_stat) { /* [한국어] 트랜스포트가 자체 통계 dump 구현했으면 호출 */
			tgroup->transport->ops->poll_group_dump_stat(tgroup, w); /* [한국어] 트랜스포트 고유 통계 (TCP는 sock 통계, RDMA는 cq 통계 등) 추가 */
		}

		spdk_json_write_object_end(w);           /* [한국어] transport object 종료 */
	}

	spdk_json_write_array_end(w);                    /* [한국어] transports 배열 종료 */
	spdk_json_write_object_end(w);                   /* [한국어] poll_group object 종료 */
}
