/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 */

/*
 * [한국어 설명] SPDK 유저스페이스 NVMe 드라이버 메인 진입 (nvme.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 유저스페이스 NVMe 드라이버의 *프로세스 단위* 라이프사이클을 담당하는 최상위
 * 진입 모듈이다. 핵심 책임:
 *   1) **드라이버 단일 인스턴스(`struct nvme_driver`) 관리**: 다중 프로세스 환경에서 공유되는
 *      hugepage 기반 글로벌 객체로, attached controller 리스트와 robust mutex(여러 프로세스 간
 *      crash-safe lock)를 보유.
 *   2) **Controller probe / attach / detach 진입점 제공**: spdk_nvme_probe / spdk_nvme_connect 의
 *      구현체, 비동기 probe context (struct spdk_nvme_probe_ctx) 라이프사이클 관리, 유저 콜백
 *      (probe_cb / attach_cb / remove_cb) 디스패치.
 *   3) **Admin command 동기 polling 헬퍼**: nvme_completion_poll_cb / nvme_wait_for_adminq_completion
 *      — admin 명령을 발행한 뒤 동기 대기하는 유틸 (Identify NS/Ctrlr 등 bring-up 시퀀스의 모든
 *      직렬 admin 호출이 이 헬퍼를 통과).
 *   4) **Reference counting**: 같은 컨트롤러를 여러 SPDK 프로세스가 attach할 수 있으므로 spdk
 *      shared memory의 robust mutex로 보호되는 ref count를 관리, ref count가 0이 될 때만 실제
 *      controller_destruct 수행.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe 스택의 *진입 layer*. 사용자 코드가 가장 먼저 호출하는 spdk_nvme_probe_async 류 API의
 * 정의가 이 파일에 거주하고, 내부에서 transport 추상화(PCIe / RDMA / TCP)·controller 상태머신·
 * NS 객체 라이프사이클(nvme_ns.c) 호출을 모두 조율한다.
 *
 *   user app
 *     ↓ spdk_nvme_probe(trid, cb_ctx, probe_cb, attach_cb, remove_cb)
 *   ★ nvme.c (이 파일):
 *     ├─ probe_ctx 생성·init_ctrlrs / attached_ctrlrs / failed_ctrlrs 리스트 관리
 *     ├─ transport 별 probe 위임 (nvme_transport_ctrlr_scan)
 *     ├─ probe_cb 호출 → 사용자가 attach 결정
 *     ├─ ctrlr_construct (nvme_ctrlr.c) 호출 → controller bring-up 상태머신 시작
 *     ├─ controller가 READY 되면 attach_cb 호출
 *     └─ destruct/detach 경로
 *
 * 실행 컨텍스트: **호스트 유저스페이스 SPDK thread**. 다중 프로세스(primary + secondaries) 환경
 * 에서는 robust mutex (g_spdk_nvme_driver->lock)로 attached_ctrlrs 리스트를 보호. 같은 ctrlr를
 * primary가 들고 secondary가 attach하는 multi-process 시나리오 지원.
 *
 * === 타 모듈과의 연결 ===
 * - 의존(이 파일이 호출하는 곳):
 *   · nvme_ctrlr.c (nvme_ctrlr_construct/destruct, nvme_ctrlr_process_init 상태머신 진행)
 *   · nvme_qpair.c (admin qpair 관리)
 *   · nvme_transport.c (PCIe/RDMA/TCP 추상화 — nvme_transport_ctrlr_scan/connect/disconnect)
 *   · nvme_io_msg.c (사용자 application과의 메시지 채널)
 *   · spdk/env.h (DPDK 기반 hugepage·shared memory·NUMA·PCIe 디바이스 enumeration)
 *   · spdk_robust_mutex (multi-process crash-safe mutex via PI futex)
 * - 의존받음(이 파일을 호출하는 곳):
 *   · 모든 SPDK 사용자 코드 (app/, examples/, module/bdev/nvme/)
 *   · nvme_ns.c (nvme_wait_for_adminq_completion 헬퍼 사용)
 *   · nvmf target (NVMe-oF subsystem이 backend로 spdk_nvme 사용 시)
 * - 공유 자료구조:
 *   · struct nvme_driver (정의: nvme_internal.h:1418) — 다중 프로세스 hugepage 공유 객체.
 *     g_spdk_nvme_driver 전역 포인터로 노출됨.
 *   · struct spdk_nvme_probe_ctx — 비동기 probe 진행 상태 (init_ctrlrs / attached / failed 리스트).
 *
 * === 주요 함수/구조체 요약 ===
 *  - spdk_nvme_probe_async / spdk_nvme_probe: 사용자 진입 API. transport scan을 트리거하고
 *    probe_ctx를 반환. 사용자는 spdk_nvme_probe_poll_async로 진행을 폴링.
 *  - nvme_completion_poll_cb: admin completion 일반 콜백 — status->done = true 로 표시 + cpl 복사.
 *    nvme_wait_for_adminq_completion 과 짝을 이루어 동기 polling 패턴 구성.
 *  - nvme_wait_for_adminq_completion: status->done 까지 admin qpair 폴링. timeout 180s.
 *  - nvme_ctrlr_detach_async / detach_poll_async: ref count 기반 비동기 detach (ref==1일 때만 destruct).
 *  - nvme_ctrlr_connected: ctrlr이 transport 단계에서 connect 성공 시 init_ctrlrs 리스트로 이동.
 *  - g_spdk_nvme_driver: process-shared 단일 driver 객체. hugepage SHARED 메모리에 거주.
 *  - g_nvme_attached_ctrlrs: process-local fabric ctrlr 리스트 (PCIe는 shared로 별도 관리).
 *  - SPDK_NVME_DRIVER_NAME: hugepage shared memory 영역 이름 — 다중 프로세스가 같은 이름으로 발견.
 */

/* [한국어] config.h: SPDK 빌드 시점 옵션 — VFIO_USER 지원, NUMA 설정, transport 활성화 비트 등.
 *  · CONFIG_VFIO_USER, CONFIG_RDMA, CONFIG_HAVE_FUSE3 등의 매크로 정의. */
#include "spdk/config.h"
/* [한국어] nvmf_spec.h: NVMe-over-Fabrics 명령 셋 정의 — Fabrics Connect/Disconnect/Property Get/Set 등. */
#include "spdk/nvmf_spec.h"
/* [한국어] string.h: SPDK 자체 문자열 헬퍼 (spdk_strerror, spdk_str_starts_with, ...). */
#include "spdk/string.h"
/* [한국어] env.h: DPDK 추상화 — hugepage 메모리, PCIe 열거, NUMA, mempool, robust_mutex. */
#include "spdk/env.h"
/* [한국어] nvme_internal.h: NVMe 드라이버 사적 자료구조·함수 — struct nvme_driver / spdk_nvme_ctrlr 등. */
#include "nvme_internal.h"
/* [한국어] nvme_io_msg.h: 사용자 app과의 메시지 큐 — 외부 thread가 io_msg producer 역할 가능. */
#include "nvme_io_msg.h"

/* [한국어] hugepage shared memory 영역 이름 — 다중 프로세스가 같은 이름으로 g_spdk_nvme_driver 인스턴스를 찾아 attach. */
#define SPDK_NVME_DRIVER_NAME "spdk_nvme_driver"

/* [한국어] 프로세스 간 공유되는 드라이버 단일 객체 — primary가 hugepage SHARED로 할당, secondary는 lookup 후 같은 가상 주소에 매핑.
 *  · attached_ctrlrs(공유) / robust_mutex / hotplug 콜백 등 글로벌 상태 보유.
 *  · NULL이면 nvme_driver_init 호출 필요. */
struct nvme_driver	*g_spdk_nvme_driver;
/* [한국어] 현재 프로세스의 PID 캐시 — multi-process attach 시 primary 식별에 사용 (getpid() 결과를 한 번 캐시). */
pid_t			g_spdk_nvme_pid;

/* gross timeout of 180 seconds in milliseconds */
/* [한국어] admin command 동기 대기의 전역 타임아웃 (ms). 180s = 3분 — Format/Sanitize 같은 장시간 admin도 커버.
 *  · nvme_wait_for_adminq_completion 가 이 값을 사용. 일반 IO timeout은 별도(qpair마다 옵션). */
static int g_nvme_driver_timeout_ms = 3 * 60 * 1000;

/* Per-process attached controller list */
/* [한국어] 현재 프로세스 *전용*으로 attach한 controller 리스트 — Fabrics(RDMA/TCP) 컨트롤러는 공유 안 됨.
 *  · PCIe 컨트롤러는 g_spdk_nvme_driver->shared_attached_ctrlrs (multi-process 공유) 로 들어감.
 *  · TAILQ_HEAD_INITIALIZER로 빈 리스트로 컴파일 타임 초기화. */
static TAILQ_HEAD(, spdk_nvme_ctrlr) g_nvme_attached_ctrlrs =
	TAILQ_HEAD_INITIALIZER(g_nvme_attached_ctrlrs);

/* Returns true if ctrlr should be stored on the multi-process shared_attached_ctrlrs list */
/*
 * [한국어]
 * nvme_ctrlr_shared - 이 controller가 multi-process 공유 리스트에 들어가야 하는지 판정
 *
 * @ctrlr: 대상 controller
 * @return: true = 공유 리스트(g_spdk_nvme_driver->shared_attached_ctrlrs), false = process-local 리스트
 *
 * 판정 기준: PCIe transport만 공유 가능. Fabrics(RDMA/TCP)는 connection 이 process마다 별도이므로 공유 X.
 *  · PCIe: BAR mapping이 hugepage 영역이라 secondary process도 같은 가상 주소로 접근 가능
 *  · Fabrics: TCP socket / RDMA QP 같은 process-local 자원이라 공유 불가
 */
static bool
nvme_ctrlr_shared(const struct spdk_nvme_ctrlr *ctrlr)
{
	/* [한국어] transport id의 trtype 비교 — PCIE 이면 공유 대상. */
	return ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE;
}

/*
 * [한국어]
 * nvme_ctrlr_connected - transport connect 성공 시 init_ctrlrs 리스트로 이동시키는 콜백
 *
 * @probe_ctx: 진행 중인 probe context
 * @ctrlr: connect 완료된 controller
 *
 * Probe state machine 진행 단계 중 한 노드 — transport scan에서 발견되어 connect까지 성공한 ctrlr를
 * "이제 admin 단계 진입 가능" 상태인 init_ctrlrs 리스트로 이동시킨다. 이후 nvme_ctrlr_process_init
 * 상태머신이 이 리스트를 polling 처리.
 *
 * Caller: 각 transport 구현체의 connect 콜백 (nvme_pcie_ctrlr_construct 등)
 */
void
nvme_ctrlr_connected(struct spdk_nvme_probe_ctx *probe_ctx,
		     struct spdk_nvme_ctrlr *ctrlr)
{
	/* [한국어] init_ctrlrs 리스트의 끝에 추가 — bring-up 진행 대기열. */
	TAILQ_INSERT_TAIL(&probe_ctx->init_ctrlrs, ctrlr, tailq);
}

/*
 * [한국어]
 * nvme_ctrlr_detach_async_finish - detach 완료 시 attached_ctrlrs 리스트에서 제거
 *
 * @ctrlr: 제거할 controller
 *
 * detach 비동기 완료 콜백으로 등록되어, controller_destruct이 끝나면 driver의 attached 리스트에서
 * 제거한다. 공유 / process-local 분기.
 *
 * 동기화: g_spdk_nvme_driver->lock (robust mutex) 보호 — 다른 프로세스 / 다른 thread와의 race 방지.
 */
static void
nvme_ctrlr_detach_async_finish(struct spdk_nvme_ctrlr *ctrlr)
{
	/* [한국어] 다중 프로세스 공유 mutex 획득 — robust mutex이므로 holder process가 죽어도 복구 가능. */
	nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
	/* [한국어] 공유 / 비공유 리스트 분기 — PCIe면 shared, 그 외면 process-local. */
	if (nvme_ctrlr_shared(ctrlr)) {
		/* [한국어] hugepage 공유 리스트에서 제거 — 다른 프로세스도 이 변경을 보게 됨. */
		TAILQ_REMOVE(&g_spdk_nvme_driver->shared_attached_ctrlrs, ctrlr, tailq);
	} else {
		/* [한국어] 현 프로세스의 process-local 리스트에서 제거. */
		TAILQ_REMOVE(&g_nvme_attached_ctrlrs, ctrlr, tailq);
	}
	/* [한국어] mutex 해제. */
	nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
}

/*
 * [한국어]
 * nvme_ctrlr_detach_async - controller detach 비동기 시작 (ref count 기반)
 *
 * @ctrlr: 대상 controller
 * @_ctx: out — destruct context 포인터 저장 (last reference 인 경우만)
 * @return: 0 성공, 음수 errno 실패 (-ENOMEM)
 *
 * 동작:
 *   - ref count 가 1이면 *마지막 참조* — destruct 시작 + ctx 할당하여 caller에 반환
 *     (caller는 이후 nvme_ctrlr_detach_poll_async 로 폴링)
 *   - ref count > 1 이면 단순히 ref 감소만 — 다른 process/thread가 아직 사용 중
 *
 * 동기화: g_spdk_nvme_driver->lock 으로 ref count 검사·감소를 atomic하게 보호.
 */
static int
nvme_ctrlr_detach_async(struct spdk_nvme_ctrlr *ctrlr,
			struct nvme_ctrlr_detach_ctx **_ctx)
{
	/* [한국어] last-ref 일 때만 할당되는 destruct context. */
	struct nvme_ctrlr_detach_ctx *ctx;
	/* [한국어] 현재 ref count 캐시. */
	int ref_count;

	/* [한국어] driver mutex 획득 — ref count 검사·수정의 atomicity 확보. */
	nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);

	/* [한국어] 현 ref count 조회 — multi-process attach 시 누적. */
	ref_count = nvme_ctrlr_get_ref_count(ctrlr);
	/* [한국어] invariant — detach는 ref count > 0 인 상태에서만 호출되어야 함 (caller 의무). */
	assert(ref_count > 0);

	/* [한국어] 마지막 참조인 경우 — 실제 destruct 진행. */
	if (ref_count == 1) {
		/* This is the last reference to the controller, so we need to
		 * allocate a context to destruct it.
		 */
		/* [한국어] destruct context 할당 (heap 0-init). 이 ctx로 비동기 destruct 진행 폴링. */
		ctx = calloc(1, sizeof(*ctx));
		/* [한국어] 할당 실패 — mutex 해제 후 errno 전파. */
		if (ctx == NULL) {
			nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);

			return -ENOMEM;
		}
		/* [한국어] context에 controller 포인터 + 완료 콜백 저장. */
		ctx->ctrlr = ctrlr;
		/* [한국어] destruct 완료 시 호출될 콜백 — attached 리스트에서 제거. */
		ctx->cb_fn = nvme_ctrlr_detach_async_finish;

		/* [한국어] 자기 process의 ref 감소 — ref count 0 도달 (실제 destruct 진행 가능 신호). */
		nvme_ctrlr_proc_put_ref(ctrlr);

		/* [한국어] io_msg 채널 해제 — 외부 thread의 메시지 발행 차단. */
		nvme_io_msg_ctrlr_detach(ctrlr);

		/* [한국어] 비동기 destruct 시작 — admin SQ/CQ 해제, transport disconnect, ns 트리 destruct 등. */
		nvme_ctrlr_destruct_async(ctrlr, ctx);

		/* [한국어] caller에 ctx 전달 — 폴링 진행에 사용. */
		*_ctx = ctx;
	} else {
		/* [한국어] 다른 process/thread가 아직 사용 중 — ref만 감소. ctx 할당 안 함. */
		nvme_ctrlr_proc_put_ref(ctrlr);
	}

	/* [한국어] mutex 해제. */
	nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);

	/* [한국어] 항상 0 (성공) — 위 ENOMEM 경로 외엔 실패 없음. */
	return 0;
}

/*
 * [한국어]
 * nvme_ctrlr_detach_poll_async - destruct context의 한 단계 폴링 + 완료 시 ctx free
 *
 * @ctx: detach context
 * @return: -EAGAIN (아직 진행 중), 0 (완료), 음수 errno (실패)
 *
 * destruct는 admin queue에 abort/shutdown 명령을 보내고 응답을 기다리는 등 비동기 단계가 있어,
 * 한 번에 끝나지 않을 수 있다. 호출자가 -EAGAIN 받으면 잠시 후 재호출.
 */
static int
nvme_ctrlr_detach_poll_async(struct nvme_ctrlr_detach_ctx *ctx)
{
	/* [한국어] 폴링 결과. */
	int rc;

	/* [한국어] destruct 한 단계 진행 — admin completion 폴링·shutdown 진행 등. */
	rc = nvme_ctrlr_destruct_poll_async(ctx->ctrlr, ctx);
	/* [한국어] -EAGAIN — 아직 진행 중. caller가 재시도해야 함. ctx는 보존. */
	if (rc == -EAGAIN) {
		return -EAGAIN;
	}

	/* [한국어] 완료(또는 실패) — context 더 이상 필요 없음, free. */
	free(ctx);

	/* [한국어] 0 또는 errno 그대로 caller에게 전파. */
	return rc;
}

/*
 * [한국어]
 * spdk_nvme_detach - 동기 detach API (사용자에게 노출되는 simplified wrapper)
 *
 * @ctrlr: detach 대상
 * @return: 0 성공, 음수 errno 실패
 *
 * 동작: nvme_ctrlr_detach_async 시작 → ctx==NULL 이면 (다른 프로세스가 아직 attach 중) 즉시 0 반환,
 *       그 외엔 nvme_delay(1000us) 간격으로 polling 하여 destruct 완료까지 대기.
 *
 * 1ms backoff polling — 사용자가 detach() 호출하고 결과만 받기 원하는 단순 케이스용. 비동기가
 * 필요하면 spdk_nvme_detach_async 사용.
 */
int
spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr)
{
	/* [한국어] last-ref 일 때만 채워짐 — NULL 이면 다른 프로세스가 아직 사용 중. */
	struct nvme_ctrlr_detach_ctx *ctx = NULL;
	/* [한국어] return code. */
	int rc;

	/* [한국어] detach 비동기 시작 — ctx 할당 시도. */
	rc = nvme_ctrlr_detach_async(ctrlr, &ctx);
	/* [한국어] errno 발생 — 즉시 caller에게 전파. */
	if (rc != 0) {
		return rc;
	} else if (ctx == NULL) {
		/* ctrlr was detached from the caller process but any other process
		 * still attaches it.
		 */
		/* [한국어] 마지막 참조 아님 — ref만 감소했고 destruct는 진행되지 않음. 즉시 성공 반환. */
		return 0;
	}

	/* [한국어] destruct가 시작됨 — 완료까지 폴링 루프. */
	while (1) {
		/* [한국어] 한 단계 폴링. */
		rc = nvme_ctrlr_detach_poll_async(ctx);
		/* [한국어] -EAGAIN 외엔 종료 (성공/실패 어느 쪽이든). */
		if (rc != -EAGAIN) {
			break;
		}
		/* [한국어] 1ms backoff — busy spin 회피하면서 polling 빈도 적정선 유지. */
		nvme_delay(1000);
	}

	/* [한국어] 항상 0 반환 — 위 nvme_ctrlr_detach_poll_async가 free 후 rc 반환했으나 여기선 무시.
	 *  · 의도: destruct 단계의 errno는 caller에게 의미 없음 (이미 감지된 hot-removal 등). */
	return 0;
}

/*
 * [한국어]
 * spdk_nvme_detach_async - 비동기 detach API (다중 ctrlr 일괄 detach 지원)
 *
 * @ctrlr: detach할 controller
 * @_detach_ctx: [in/out] 컨테이너 컨텍스트 — NULL이면 새로 할당, 기존이면 그곳에 누적
 * @return 0 성공(또는 다른 프로세스가 사용 중이라 ref만 감소), -EINVAL/-ENOMEM/기타 errno.
 *
 * 비동기 detach의 가치:
 *   - 여러 ctrlr를 한 detach_ctx에 누적 → 한 번의 polling으로 모두 완료까지 대기 가능
 *   - 사용자 reactor loop가 spdk_nvme_detach_poll_async 한 번 호출로 진행
 *
 * 동작:
 *   [1] 인자 검증.
 *   [2] *_detach_ctx == NULL이면 새 컨테이너 할당 + 빈 TAILQ 초기화. 있으면 재사용.
 *   [3] nvme_ctrlr_detach_async로 ctrlr별 destruct context 받기.
 *   [4] ctx == NULL (다른 프로세스가 아직 사용 중이라 ref만 감소된 케이스):
 *       - 컨테이너가 새로 할당됐고 비어 있으면 free (누수 방지) + 결과 그대로 반환.
 *   [5] 정상이면 TAILQ에 추가하고 사용자에게 컨테이너 노출.
 */
int
spdk_nvme_detach_async(struct spdk_nvme_ctrlr *ctrlr,
		       struct spdk_nvme_detach_ctx **_detach_ctx)
{
	struct spdk_nvme_detach_ctx *detach_ctx;
                                  /* [한국어] 다중 ctrlr를 묶는 상위 컨테이너 — TAILQ로 destruct context들 보관 */
	struct nvme_ctrlr_detach_ctx *ctx = NULL;
                                  /* [한국어] last-ref 일 때만 채워지는 단일 ctrlr destruct context */
	int rc;

	if (ctrlr == NULL || _detach_ctx == NULL) {
                                  /* [한국어] 두 인자 모두 필수 — NULL이면 사용자 버그 */
		return -EINVAL;
	}

	/* Use a context header to poll detachment for multiple controllers.
	 * Allocate an new one if not allocated yet, or use the passed one otherwise.
	 */
	detach_ctx = *_detach_ctx;
                                  /* [한국어] 기존 컨테이너 검색 — 사용자가 첫 호출 시 *_detach_ctx=NULL 줘야 함 */
	if (detach_ctx == NULL) {
		detach_ctx = calloc(1, sizeof(*detach_ctx));
                                  /* [한국어] 첫 호출 — 새 컨테이너 할당 (TAILQ head 0-init) */
		if (detach_ctx == NULL) {
			return -ENOMEM;
		}
		TAILQ_INIT(&detach_ctx->head);
                                  /* [한국어] 빈 TAILQ 명시적 초기화 (calloc로 0이지만 TAILQ_INIT는 미래 호환) */
	}

	rc = nvme_ctrlr_detach_async(ctrlr, &ctx);
                                  /* [한국어] ref count 검사 + (last-ref면) ctx 할당 + destruct 시작 */
	if (rc != 0 || ctx == NULL) {
		/* If this detach failed and the context header is empty, it means we just
		 * allocated the header and need to free it before returning.
		 */
		if (TAILQ_EMPTY(&detach_ctx->head)) {
                                  /* [한국어] 컨테이너에 누적된 ctx 없음 = 우리가 방금 할당했고 추가 못 함 → 누수 방지 free.
                                   *  rc=0 + ctx=NULL은 다른 프로세스가 사용 중인 정상 케이스, 그래도 빈 컨테이너는 free. */
			free(detach_ctx);
		}
		return rc;
	}

	/* Append a context for this detachment to the context header. */
	TAILQ_INSERT_TAIL(&detach_ctx->head, ctx, link);
                                  /* [한국어] 컨테이너에 누적 — 이후 detach_poll_async가 모든 ctx 일괄 폴링 */

	*_detach_ctx = detach_ctx;
                                  /* [한국어] 사용자 변수 갱신 — 다음 호출 시 같은 컨테이너 재사용 */

	return 0;
}

/*
 * [한국어]
 * spdk_nvme_detach_poll_async - detach 컨테이너 한 단계 폴링 (사용자 reactor 루프에서 호출)
 *
 * @detach_ctx: spdk_nvme_detach_async가 반환한 컨테이너
 * @return 0(모두 완료, 컨테이너 자동 free), -EAGAIN(아직 진행 중), -EINVAL.
 *
 * 동작:
 *   TAILQ를 SAFE 순회하며 각 ctx에 대해 destruct 한 단계 진행.
 *   - -EAGAIN: 미완료 → 다시 머리에 INSERT (FIFO 순서 보존, 다음 폴링 사이클에서 재시도)
 *   - 그 외: 완료 → ctx는 nvme_ctrlr_detach_poll_async가 free함 (다시 INSERT 안 함)
 *   순회 후 컨테이너 비었으면 컨테이너도 free + 0, 아니면 -EAGAIN.
 *
 * 사용 패턴:
 *   while (spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) { reactor_loop_other_work(); }
 */
int
spdk_nvme_detach_poll_async(struct spdk_nvme_detach_ctx *detach_ctx)
{
	struct nvme_ctrlr_detach_ctx *ctx, *tmp_ctx;
                                  /* [한국어] SAFE 순회 — 본문에서 REMOVE/INSERT 안전 */
	int rc;

	if (detach_ctx == NULL) {
		return -EINVAL;
                                  /* [한국어] 인자 검증 — NULL은 사용자 버그 */
	}

	TAILQ_FOREACH_SAFE(ctx, &detach_ctx->head, link, tmp_ctx) {
		TAILQ_REMOVE(&detach_ctx->head, ctx, link);
                                  /* [한국어] 일단 빼두고 — 진행 결과에 따라 다시 넣을지 결정 */

		rc = nvme_ctrlr_detach_poll_async(ctx);
		if (rc == -EAGAIN) {
			/* If not -EAGAIN, ctx was freed by nvme_ctrlr_detach_poll_async(). */
			TAILQ_INSERT_HEAD(&detach_ctx->head, ctx, link);
                                  /* [한국어] 미완료 — head에 다시 넣음. INSERT_HEAD는 다음 순회에서 같은 항목을
                                   *  먼저 보지 않게 함 (이번 sweep는 다음 항목으로 진행, 다음 sweep에서 재시도) */
		}
                                  /* [한국어] 0 또는 음수 errno면 ctx는 이미 free됨 → 컨테이너에서 영구 제거 */
	}

	if (!TAILQ_EMPTY(&detach_ctx->head)) {
		return -EAGAIN;
                                  /* [한국어] 아직 미완료 ctx 잔존 — 사용자에게 더 폴링하라 신호 */
	}

	free(detach_ctx);
                                  /* [한국어] 모두 완료 — 컨테이너 자체도 free */
	return 0;
}

/*
 * [한국어]
 * spdk_nvme_detach_poll - 동기 detach 폴링 wrapper (사용자가 결과만 받기 원할 때)
 *
 * @detach_ctx: detach 컨테이너 (NULL 허용 — noop)
 *
 * 동작: detach_poll_async를 -EAGAIN 아닐 때까지 busy-spin 호출.
 *       NULL 인자는 noop (사용자가 detach_async가 ctx 할당 못 했을 때 안전하게 호출 가능).
 *
 * 주의: busy-spin이라 CPU 점유. reactor 컨텍스트에서는 사용 자제 — async 버전 사용 권장.
 */
void
spdk_nvme_detach_poll(struct spdk_nvme_detach_ctx *detach_ctx)
{
	while (detach_ctx && spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) {
                                  /* [한국어] NULL 안전 short-circuit + -EAGAIN 동안 무한 루프.
                                   *  완료(0 반환) 시 컨테이너는 detach_poll_async가 free함. */
		;
	}
}

/*
 * [한국어]
 * nvme_completion_poll_cb - admin/IO 명령 완료 콜백 (동기 polling 패턴의 핵심)
 *
 * @arg: nvme_completion_poll_status 포인터 (caller가 발행 시 cb_arg로 전달)
 * @cpl: NVMe completion entry (CQE) — sct, sc, cdw0, cid 포함
 *
 * 이 콜백은 admin SQ/CQ 또는 IO SQ/CQ의 process_completions 루프에서 호출되어, 미리 약속된
 * "동기 대기 패턴"에 따라:
 *   1) timed_out 인 경우 — caller가 이미 포기하고 떠난 상태. status·dma_data 메모리 누수 방지를 위해
 *      여기서 자력 해제. (caller는 더 이상 status를 가지고 있지 않음)
 *   2) 정상 완료 — CQE를 status->cpl 에 복사하고 done=true 표시. 대기 중인 caller가 polling으로 감지.
 *
 * Caller (콜백 호출자):
 *   nvme_qpair_process_completions / spdk_nvme_poll_group_process_completions 가
 *   각 cid에 등록된 cb_fn을 dispatch할 때.
 *
 * 짝꿍: nvme_wait_for_completion_poll / nvme_wait_for_adminq_completion 가 status->done 폴링.
 */
void
nvme_completion_poll_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	/* [한국어] cb_arg를 정확한 타입으로 cast — caller가 nvme_completion_poll_status* 를 넘겼다는 가정. */
	struct nvme_completion_poll_status	*status = arg;

	/* [한국어] timed_out 분기 — caller가 이미 timeout 처리하고 떠났음.
	 *  · status는 caller가 포기한 메모리 — 여기서 free하지 않으면 leak.
	 *  · dma_data도 함께 정리 (caller가 발행 시 buffer를 status에 저장해뒀음). */
	if (status->timed_out) {
		/* There is no routine waiting for the completion of this request, free allocated memory */
		/* [한국어] hugepage DMA 버퍼 반환. */
		spdk_free(status->dma_data);
		/* [한국어] heap status tracker 반환. */
		free(status);
		/* [한국어] caller 없음 — 여기서 종료. CQE 데이터는 버려짐. */
		return;
	}

	/*
	 * Copy status into the argument passed by the caller, so that
	 *  the caller can check the status to determine if the
	 *  the request passed or failed.
	 */
	/* [한국어] CQE 전체를 status->cpl 에 복사 — caller가 SC/SCT/cdw0 등 검사 가능. */
	memcpy(&status->cpl, cpl, sizeof(*cpl));
	/* [한국어] release-store semantics 의도 — caller의 polling이 done=true 보면 cpl 도 visible.
	 *  · 같은 thread 안의 polling이라 메모리 순서는 자연스럽게 보장됨 (SPDK는 single-threaded admin path). */
	status->done = true;
}

/*
 * [한국어]
 * dummy_disconnected_qpair_cb - poll group 처리 시 disconnected qpair 콜백 placeholder
 *
 * @qpair: disconnected 상태의 qpair
 * @poll_group_ctx: 사용 안 함
 *
 * spdk_nvme_poll_group_process_completions 가 disconnected qpair 발견 시 호출하는 콜백 인자에
 * NULL 대신 no-op 함수 포인터를 넘기기 위한 placeholder. nvme_wait_for_completion_poll 의
 * "잠시 대기" 시나리오에서는 해당 qpair의 disconnect를 적극적으로 처리할 의무가 없음.
 */
static void
dummy_disconnected_qpair_cb(struct spdk_nvme_qpair *qpair, void *poll_group_ctx)
{
	/* [한국어] no-op — 의도적 빈 콜백. */
}

/*
 * [한국어]
 * nvme_wait_for_completion_poll - status->done 도달까지 단발 polling 사이클 (헬퍼)
 *
 * @qpair: 검사할 qpair (admin 또는 IO)
 * @status: 완료 추적 객체 — done=true 또는 timeout/error 까지 caller가 반복 호출
 * @return 0(성공 + done), -EAGAIN(아직 미도착), -EIO(SC 에러), -ECANCELED(timeout/transport 에러).
 *
 * 동작 단계:
 *   [1] admin queue면 ctrlr lock 획득 — admin SQ는 multi-process race 방지 위해 직렬화 필요.
 *   [2] poll group 있으면 poll_group_process_completions(이종 트랜스포트 통합),
 *       없으면 단일 qpair process_completions.
 *   [3] admin queue 락 해제.
 *   [4] rc < 0: 트랜스포트 에러 — status에 SCT_GENERIC/SC_ABORTED_SQ_DELETION 합성 후 error 분기.
 *   [5] timeout 검사 — done=false + timeout_tsc != 0 + now > timeout_tsc 면 error 분기.
 *   [6] PCIe 장치 link 검사 — CSTS register all-ones 면 hot-removal로 판단 → error 분기.
 *   [7] 정상 분기:
 *       - !done: -EAGAIN (아직 응답 안 옴)
 *       - cpl 에러: -EIO
 *       - 그 외: 0 (정상 완료)
 *   error: status->done=false 상태에서 timed_out=true 마킹 → 늦은 callback이 자기 자신 free.
 *          -ECANCELED 반환.
 *
 * 호출자: nvme_wait_for_adminq_completion (반복 호출), nvme_fabric_qpair_authenticate_poll 등.
 */
int
nvme_wait_for_completion_poll(struct spdk_nvme_qpair *qpair,
			      struct nvme_completion_poll_status *status)
{
	int rc;

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_ctrlr_lock(qpair->ctrlr);
                                  /* [한국어] admin queue 폴링은 multi-process 안전성을 위해 ctrlr lock 보호 */
	}

	if (qpair->poll_group) {
                                  /* [한국어] poll group 등록된 qpair — group 단위 일괄 폴링 (다른 qpair들도 함께 진행) */
		rc = (int)spdk_nvme_poll_group_process_completions(qpair->poll_group->group, 0,
				dummy_disconnected_qpair_cb);
                                  /* [한국어] 0=무제한 폴링, dummy cb로 disconnected는 무시 */
	} else {
                                  /* [한국어] 독립 qpair — 단일 트랜스포트 호출 */
		rc = spdk_nvme_qpair_process_completions(qpair, 0);
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_ctrlr_unlock(qpair->ctrlr);
                                  /* [한국어] admin lock 해제 */
	}

	if (rc < 0) {
		status->cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		status->cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
                                  /* [한국어] 트랜스포트 에러 — caller가 sct/sc 검사할 수 있도록 합성된 CQE 채움 */
		goto error;
	}

	if (!status->done && status->timeout_tsc && spdk_get_ticks() > status->timeout_tsc) {
                                  /* [한국어] 미응답 + timeout 활성(non-zero) + 절대 시각 초과 → timeout 처리 */
		goto error;
	}

	if (qpair->ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
                                  /* [한국어] PCIe 전용 link 헬스 검사 — hot-removal 빠른 감지 */
		union spdk_nvme_csts_register csts = spdk_nvme_ctrlr_get_regs_csts(qpair->ctrlr);
                                  /* [한국어] CSTS(Controller Status) register MMIO read */
		if (csts.raw == SPDK_NVME_INVALID_REGISTER_VALUE) {
                                  /* [한국어] all-ones — PCIe link 끊겨 BAR 응답 무효. nvme_pcie.c의 SIGBUS 핸들러가
                                   *  BAR을 anonymous 메모리로 remap한 상태. */
			status->cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			status->cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto error;
		}
	}

	if (!status->done) {
		return -EAGAIN;
                                  /* [한국어] 응답 미도착 — caller가 한 번 더 폴링 */
	} else if (spdk_nvme_cpl_is_error(&status->cpl)) {
		return -EIO;
                                  /* [한국어] 도착했지만 SC 에러 (sct != 0 || sc != 0) */
	} else {
		return 0;
                                  /* [한국어] 정상 완료 */
	}
error:
	/* Either transport error occurred or we've timed out.  Either way, if the response hasn't
	 * been received yet, mark the command as timed out, so the status gets freed when the
	 * command is completed or aborted.
	 */
	if (!status->done) {
		status->timed_out = true;
                                  /* [한국어] 늦은 callback이 status 메모리 free하도록 표시.
                                   *  (nvme_completion_poll_cb의 timed_out 분기가 status + dma_data 모두 free) */
	}

	return -ECANCELED;
                                  /* [한국어] 트랜스포트 에러 또는 timeout — 사용자 진단 신호 */
}

/*
 * [한국어]
 * nvme_wait_for_adminq_completion - admin qpair에 발행된 명령의 완료를 동기 polling 대기
 *
 * @ctrlr: 대상 controller (admin qpair = ctrlr->adminq)
 * @status: poll status — 발행 시 cb_arg로 등록된 객체. 완료 시 status->done=true 가 됨.
 * @release: true 면 정상 완료 시 status를 자동으로 free (caller가 더 들고 있을 필요 없음)
 *
 * @return: 0 정상 완료, -EIO SC 에러, -ECANCELED timeout/transport 에러
 *
 * 이 함수가 왜 필요한가:
 *   - bring-up 시퀀스(Identify Ctrl/NS/Active NS list 등)는 직렬이라 동기 패턴이 자연스러움.
 *   - 비동기 콜백 인터페이스를 동기 대기로 감싸는 thin wrapper. 사용처가 매우 많음 (nvme_ctrlr.c,
 *     nvme_ns.c, nvme_qpair.c, nvme_ctrlr_cmd.c 등).
 *
 * 동작:
 *   1) timeout 절대 tick 계산 (admin_timeout_ms 기반, 0이면 무한 대기)
 *   2) status->cpl 초기화
 *   3) nvme_wait_for_completion_poll 을 -EAGAIN 아닐 때까지 반복 (실제 polling 루프)
 *   4) release 이고 timed_out 아니면 status free (caller 부담 줄임)
 *
 * 실행 컨텍스트: 호스트 SPDK thread, single-threaded admin path 가정. timeout이 명시되지 않으면
 * 무한 대기 가능 — caller가 deadlock 회피 책임.
 */
int
nvme_wait_for_adminq_completion(struct spdk_nvme_ctrlr *ctrlr,
				struct nvme_completion_poll_status *status, bool release)
{
	/* [한국어] timeout 마이크로초 단위 — 0 이면 disable. */
	uint64_t timeout_in_usecs = ctrlr->opts.admin_timeout_ms * 1000;
	/* [한국어] return code. */
	int rc;

	/* [한국어] timeout 활성 — 절대 tick 시각 계산 (현재 ticks + 변환된 tick 수). */
	if (timeout_in_usecs) {
		/* [한국어] timeout_tsc = now + (us * tick_hz / us_per_sec).
		 *  · spdk_get_ticks_hz() = TSC 주파수, SPDK_SEC_TO_USEC = 1_000_000. */
		status->timeout_tsc = spdk_get_ticks() + timeout_in_usecs *
				      spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
	} else {
		/* [한국어] 0 = 무한 대기. nvme_wait_for_completion_poll가 timeout 검사 skip. */
		status->timeout_tsc = 0;
	}

	/* [한국어] CQE raw status 초기화 — 이전 호출 잔존값 방지. */
	status->cpl.status_raw = 0;
	/* [한국어] polling 루프 — done=true 또는 timeout/error 까지 반복.
	 *  · -EAGAIN = "아직 응답 안 옴" → 한 번 더 polling.
	 *  · 기타 rc = 종료 (성공/실패 어느 쪽이든). */
	do {
		rc = nvme_wait_for_completion_poll(ctrlr->adminq, status);
	} while (rc == -EAGAIN);

	/* [한국어] release 옵션 + timeout 아니면 status 메모리 해제 — caller가 status를 더 안 봐도 되는 케이스.
	 *  · timed_out 일 때는 free 안 함 — 늦은 콜백이 status에 쓸 수 있어 위험. */
	if (release && !status->timed_out) {
		free(status);
	}

	/* [한국어] 0 / -EIO / -ECANCELED 그대로 caller에게. */
	return rc;
}

/*
 * [한국어]
 * nvme_user_copy_cmd_complete - user_copy 패턴의 완료 콜백 (DMA 버퍼 → 사용자 버퍼 복사 후 사용자 cb 호출)
 *
 * @arg: nvme_request 포인터 (제출 시 등록됨)
 * @cpl: NVMe completion entry
 *
 * user_copy 패턴 의미: 사용자가 일반 malloc 메모리(DMA 불가)를 줘도 admin 명령 발행 가능하도록,
 * 내부에서 DMA-capable 버퍼를 따로 할당해 복사 → 명령 제출 → 완료 시 결과를 사용자 버퍼로 복사.
 *
 * 동작:
 *   [1] 사용자 버퍼 + payload 있는 경우:
 *       - CONTIG payload만 지원 (assert).
 *       - data transfer 방향 검사 — CONTROLLER_TO_HOST 또는 BIDIRECTIONAL이면
 *         DMA 버퍼 → user_buffer 복사 (사용자에게 결과 전달).
 *         HOST_TO_CONTROLLER는 이미 제출 시 복사했으므로 재복사 불필요.
 *       - PID 일치 검증 (multi-process 안전성).
 *   [2] 사용자 cb_fn / cb_arg 추출 후 req 정리 (DMA 버퍼 + req 자체 free).
 *   [3] 사용자 원본 콜백 호출 — 사용자 입장에서는 자기가 발행한 명령의 완료처럼 보임.
 *
 * 사용처: nvme_allocate_request_user_copy로 만든 req의 모든 완료 경로.
 */
static void
nvme_user_copy_cmd_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_request *req = arg;
	spdk_nvme_cmd_cb user_cb_fn;
	void *user_cb_arg;
	enum spdk_nvme_data_transfer xfer;

	if (req->user_buffer && req->payload_size) {
		/* Copy back to the user buffer */
		assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);
                                  /* [한국어] user_copy 경로는 CONTIG 페이로드만 지원 — SGL은 다른 경로 */
		xfer = spdk_nvme_opc_get_data_transfer(req->cmd.opc);
                                  /* [한국어] opcode에서 data transfer 방향 추출 */
		if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST ||
		    xfer == SPDK_NVME_DATA_BIDIRECTIONAL) {
                                  /* [한국어] 컨트롤러→호스트 방향만 결과 복사 필요 (호스트→컨트롤러는 이미 제출 시 복사됨) */
			assert(req->pid == getpid());
                                  /* [한국어] multi-process 안전성 — 발행한 프로세스에서만 복사 (다른 프로세스 메모리 접근 금지) */
			memcpy(req->user_buffer, req->payload.contig_or_cb_arg, req->payload_size);
                                  /* [한국어] DMA 버퍼 → 사용자 버퍼 복사 — 사용자에게 결과 전달 */
		}
	}

	user_cb_fn = req->user_cb_fn;
	user_cb_arg = req->user_cb_arg;
                                  /* [한국어] cleanup 전에 cb 정보 보존 (cleanup이 req 메모리 회수) */
	nvme_cleanup_user_req(req);
                                  /* [한국어] DMA 버퍼 + req 자체 정리 */

	/* Call the user's original callback now that the buffer has been copied */
	user_cb_fn(user_cb_arg, cpl);
                                  /* [한국어] 사용자 cb 호출 — 사용자 입장에서는 직접 발행한 명령 완료처럼 보임 */

}

/**
 * Allocate a request as well as a DMA-capable buffer to copy to/from the user's buffer.
 *
 * This is intended for use in non-fast-path functions (admin commands, reservations, etc.)
 * where the overhead of a copy is not a problem.
 */
/*
 * [한국어]
 * nvme_allocate_request_user_copy - 사용자 일반 메모리 + DMA-capable 사본 한 쌍 생성 (헬퍼)
 *
 * @qpair: 제출 대상 qpair
 * @buffer: 사용자 일반 메모리 (DMA 불가능해도 됨, NULL 허용)
 * @payload_size: 페이로드 크기 (0 허용 — 페이로드 없는 명령)
 * @cb_fn: 사용자 완료 콜백 (req->user_cb_fn에 저장됨)
 * @cb_arg: 사용자 콜백 컨텍스트
 * @host_to_controller: true면 제출 즉시 buffer→dma_buffer 복사 (HOST_TO_CONTROLLER 명령용)
 * @return 새 nvme_request 또는 NULL.
 *
 * 사용처: admin command, reservation 등 fast-path 아닌 곳 — 복사 오버헤드가 무시 가능.
 *
 * 동작:
 *   [1] buffer + size > 0 이면 spdk_zmalloc(4KiB align, DMA-capable) — hugepage NUMA-local.
 *   [2] host_to_controller 면 즉시 user→dma 복사 (제출 전에 데이터 준비).
 *   [3] nvme_allocate_request_contig으로 req 생성, 완료 cb는 nvme_user_copy_cmd_complete로 hook.
 *   [4] 실패 시 dma_buffer free.
 *   [5] 성공 시 user_cb_fn/user_cb_arg/user_buffer 저장 + cb_arg=req로 자기 자신 가리킴.
 *
 * 라이프사이클: 완료 시 nvme_user_copy_cmd_complete가 (필요시) 결과 복사 + dma_buffer free + 사용자 cb 호출.
 */
struct nvme_request *
nvme_allocate_request_user_copy(struct spdk_nvme_qpair *qpair,
				void *buffer, uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
				void *cb_arg, bool host_to_controller)
{
	struct nvme_request *req;
	void *dma_buffer = NULL;

	if (buffer && payload_size) {
                                  /* [한국어] 페이로드 있는 경우만 DMA 버퍼 할당 */
		dma_buffer = spdk_zmalloc(payload_size, 4096, NULL,
					  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
                                  /* [한국어] 4KiB align — NVMe DMA 요구사항. NUMA_ID_ANY = 적절한 노드 자동 선택. */
		if (!dma_buffer) {
			return NULL;
		}

		if (host_to_controller) {
			memcpy(dma_buffer, buffer, payload_size);
                                  /* [한국어] 제출 전 미리 복사 — 명령이 컨트롤러에 보낼 데이터 준비 */
		}
	}

	req = nvme_allocate_request_contig(qpair, dma_buffer, payload_size, nvme_user_copy_cmd_complete,
					   NULL);
                                  /* [한국어] req 생성 — 완료 cb는 user_copy 전용 hook (CONTROLLER_TO_HOST면 결과 복사) */
	if (!req) {
		spdk_free(dma_buffer);
                                  /* [한국어] req 할당 실패 — 이미 만든 dma_buffer 누수 방지 */
		return NULL;
	}

	req->user_cb_fn = cb_fn;
	req->user_cb_arg = cb_arg;
	req->user_buffer = buffer;
                                  /* [한국어] 사용자 정보 보존 — 완료 hook에서 사용 */
	req->cb_arg = req;
                                  /* [한국어] cb_arg가 자기 자신 — hook이 req에서 user_cb_fn 등 회수 가능하도록 */

	return req;
}

/**
 * Check if a request has exceeded the controller timeout.
 *
 * \param req request to check for timeout.
 * \param cid command ID for command submitted by req (will be passed to timeout_cb_fn)
 * \param active_proc per-process data for the controller associated with req
 * \param now_tick current time from spdk_get_ticks()
 * \return 0 if requests submitted more recently than req should still be checked for timeouts, or
 * 1 if requests newer than req need not be checked.
 *
 * The request's timeout callback will be called if needed; the caller is only responsible for
 * calling this function on each outstanding request.
 */
/*
 * [한국어]
 * nvme_request_check_timeout - 단일 inflight request의 timeout 검사 + 콜백 트리거
 *
 * @req: 검사 대상 request (in-flight 명령)
 * @cid: command ID (req->cid 와 같으나 명시적 인자로 받음 — timeout_cb_fn 시그니처 호환)
 * @active_proc: 현재 process의 controller 부속 데이터 (timeout_io_ticks, timeout_cb_fn 보유)
 * @now_tick: spdk_get_ticks() 호출자가 측정한 현재 tick
 * @return: 0 — 더 오래된 request도 계속 검사해야 함
 *          1 — 이 request가 아직 timeout 안 됨 (정렬 가정 — 더 최근은 검사 불필요)
 *
 * Linux blk-mq의 blk_mq_check_expired에 해당하는 SPDK 버전. Admin 명령은 별도 timeout 사용.
 *
 * 동작:
 *   1) admin queue인 경우 admin timeout 사용
 *      · AER(Async Event Request)는 특성상 무한 대기 — timeout 대상 아님 (skip 0 반환)
 *      · KEEP_ALIVE는 keep_alive_timeout_ms 옵션 사용
 *      · qpair=NULL 로 설정하여 사용자 콜백에 admin queue 노출 회피
 *   2) 이미 timeout 처리됐거나 submit_tick=0 (제출 안 됨) — skip
 *   3) PID 불일치 — 이 process가 발행한 게 아니므로 skip (multi-process 시나리오)
 *
 * Caller: nvme_qpair_process_completions 의 timeout sweep 단계
 */
int
nvme_request_check_timeout(struct nvme_request *req, uint16_t cid,
			   struct spdk_nvme_ctrlr_process *active_proc,
			   uint64_t now_tick)
{
	/* [한국어] req 소속 qpair 캐시. admin 분기에서 NULL로 덮어쓰일 수 있음. */
	struct spdk_nvme_qpair *qpair = req->qpair;
	/* [한국어] ctrlr 캐시. */
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	/* [한국어] timeout 임계 — IO queue 기본값으로 시작, admin 분기에서 덮어씀. */
	uint64_t timeout_ticks = active_proc->timeout_io_ticks;

	/* [한국어] caller invariant — timeout 콜백이 등록되지 않은 process는 이 함수를 호출하면 안 됨. */
	assert(active_proc->timeout_cb_fn != NULL);

	/* [한국어] admin queue 여부 분기. spdk_unlikely로 IO 분기를 fast path 힌트. */
	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		/* [한국어] AER (Async Event Request) — 항상 outstanding이고 NS 변경 등 device-initiated 이벤트 대기.
		 *  · 본질적으로 timeout 대상 아님 → skip (0 반환). */
		if (req->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			return 0;
		}

		/* [한국어] admin 명령은 별도 timeout 적용. */
		timeout_ticks = active_proc->timeout_admin_ticks;
		/* [한국어] KEEP_ALIVE — keep_alive_timeout_ms 옵션이 명시되어 있으면 그 값 사용.
		 *  · NVMe-oF에서 host가 살아있음을 controller에 주기적으로 알리는 명령. */
		if (req->cmd.opc == SPDK_NVME_OPC_KEEP_ALIVE && ctrlr->opts.keep_alive_timeout_ms) {
			/* [한국어] ms → ticks 변환 (tick_hz / 1000). */
			timeout_ticks = ctrlr->opts.keep_alive_timeout_ms * spdk_get_ticks_hz() / SPDK_SEC_TO_MSEC;
		}

		/*
		 * We don't want to expose the admin queue to the user, so when
		 * we're timing out admin commands set the qpair to NULL.
		 */
		/* [한국어] qpair NULL clear — timeout_cb_fn 시그니처에 qpair가 들어가는데, 사용자 콜백은
		 *  admin qpair 자체를 알 필요 없음 (사용자는 IO qpair만 다룸). */
		qpair = NULL;
	}

	/* [한국어] 이미 timeout 처리된 req 또는 미제출 — skip. */
	if (spdk_unlikely(req->timed_out || req->submit_tick == 0)) {
		return 0;
	}

	/* [한국어] PID 불일치 — multi-process에서 다른 프로세스가 발행한 req. 이 프로세스가 timeout 처리 책임 없음. */
	if (spdk_unlikely(req->pid != g_spdk_nvme_pid)) {
		return 0;
	}

	if (spdk_likely(req->submit_tick + timeout_ticks > now_tick)) {
		return 1;
	}

	req->timed_out = true;
	active_proc->timeout_cb_fn(active_proc->timeout_cb_arg, ctrlr, qpair, cid);
	return 0;
}

/*
 * [한국어]
 * nvme_robust_mutex_init_shared - PI futex 기반 multi-process robust mutex 초기화
 *
 * @mtx: 초기화할 pthread_mutex_t (hugepage 공유 영역에 위치)
 * @return 0 성공, -1 실패.
 *
 * "robust" 의미: holder process가 lock 보유 상태로 죽으면 다음 lock 시도자가 EOWNERDEAD를 받고
 *               consistent 호출로 복구 가능. 일반 mutex는 holder가 죽으면 영구 deadlock.
 *
 * "PROCESS_SHARED" 의미: hugepage SHARED 영역에 두면 다른 프로세스도 같은 mutex로 동기화 가능.
 *                       기본 PRIVATE 모드는 단일 프로세스 내에서만 의미.
 *
 * Linux: pthread_mutexattr_setpshared(SHARED) + setrobust(ROBUST) → PI futex로 구현.
 * FreeBSD: robust mutex 미지원 — 일반 mutex로 대체. Multi-process는 안전하나 crash 복구 안 됨.
 *
 * 호출처: nvme_driver_init (g_spdk_nvme_driver->lock 초기화) +
 *         nvme_ctrlr.c (active_proc->lock 등 컨트롤러 단위 mutex).
 */
int
nvme_robust_mutex_init_shared(pthread_mutex_t *mtx)
{
	int rc = 0;

#ifdef __FreeBSD__
	pthread_mutex_init(mtx, NULL);
                                  /* [한국어] FreeBSD — robust 미지원, 일반 mutex만 가능. 멀티프로세스 안전이지만 crash 복구 X. */
#else
	pthread_mutexattr_t attr;
                                  /* [한국어] Linux: pthread mutex 속성 객체 — pshared + robust 설정 후 init에 전달 */

	if (pthread_mutexattr_init(&attr)) {
		return -1;
                                  /* [한국어] 속성 객체 init 실패 — 매우 드물지만 OOM 등 */
	}
	if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) ||
	    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) ||
	    pthread_mutex_init(mtx, &attr)) {
                                  /* [한국어] 3개 단계 short-circuit OR로 묶음:
                                   *  1. PROCESS_SHARED: 다른 프로세스에서도 같은 mutex 동기화 인식
                                   *  2. ROBUST: holder 사망 시 EOWNERDEAD 알림 + consistent 복구 가능
                                   *  3. mutex 자체 초기화 (속성 적용)
                                   *  하나라도 실패하면 rc=-1. */
		rc = -1;
	}
	pthread_mutexattr_destroy(&attr);
                                  /* [한국어] attr는 임시 객체 — init 후 더 이상 필요 없으므로 즉시 destroy (성공/실패 무관) */
#endif

	return rc;
}

/*
 * [한국어]
 * nvme_driver_init - g_spdk_nvme_driver 전역 객체 초기화 (multi-process 지원)
 *
 * @return: 0 성공, -1 실패 (메모리 할당 실패 / mutex init 실패 / secondary timeout)
 *
 * 이 함수가 왜 필요한가:
 *   - SPDK는 multi-process 모델 — primary가 hugepage 영역을 reserve하고, secondary는 lookup으로
 *     같은 메모리에 attach. 이 모델에서 g_spdk_nvme_driver 단일 인스턴스를 어떻게 공유할지가 핵심.
 *   - 다중 thread/프로세스가 동시에 spdk_nvme_probe 호출해도 단 한 번만 init 되도록 lock으로 보호.
 *
 * 동작:
 *   1) process-private mutex(g_init_mutex)로 진입 직렬화
 *   2) primary process:
 *      - memzone_reserve로 hugepage SHARED 메모리에 nvme_driver 객체 할당
 *      - robust mutex 초기화 (multi-process crash-safe lock)
 *      - hotplug_fd, hotplug_pci_devices 등 글로벌 상태 초기화
 *      - initialized = true 마킹
 *   3) secondary process:
 *      - memzone_lookup으로 primary가 reserve한 영역 찾기
 *      - initialized = true 까지 polling 대기 (180s timeout)
 *
 * 실행 컨텍스트: spdk_nvme_probe_async 의 첫 호출 시 자동 진입. 멱등 — 이미 init되어 있으면 즉시 반환.
 */
int
nvme_driver_init(void)
{
	/* [한국어] process-private mutex — 같은 프로세스 내 다중 thread 동시 호출 시 직렬화.
	 *  · static + PTHREAD_MUTEX_INITIALIZER → 컴파일 타임 0-init, 별도 init 호출 불필요. */
	static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;
	/* [한국어] return code. */
	int ret = 0;

	/* Use a special process-private mutex to ensure the global
	 * nvme driver object (g_spdk_nvme_driver) gets initialized by
	 * only one thread.  Once that object is established and its
	 * mutex is initialized, we can unlock this mutex and use that
	 * one instead.
	 */
	/* [한국어] 진입 직렬화. 같은 프로세스의 두 번째 thread는 여기서 대기. */
	pthread_mutex_lock(&g_init_mutex);

	/* Each process needs its own pid. */
	/* [한국어] PID 캐시 — multi-process 시나리오에서 자기 프로세스 식별용. getpid()는 그리 비싸지 않으나 캐시. */
	g_spdk_nvme_pid = getpid();

	/*
	 * Only one thread from one process will do this driver init work.
	 * The primary process will reserve the shared memory and do the
	 *  initialization.
	 * The secondary process will lookup the existing reserved memory.
	 */
	/* [한국어] primary / secondary 분기. */
	if (spdk_process_is_primary()) {
		/* The unique named memzone already reserved. */
		/* [한국어] 같은 primary 안에서 이미 init된 경우 — 멱등 처리. */
		if (g_spdk_nvme_driver != NULL) {
			pthread_mutex_unlock(&g_init_mutex);
			return 0;
		} else {
			/* [한국어] primary 최초 진입 — hugepage SHARED 영역에 nvme_driver 객체 할당.
			 *  · NO_IOVA_CONTIG: 이 객체는 DMA 대상 아니므로 IOVA 연속 보장 불필요 (메모리 절약). */
			g_spdk_nvme_driver = spdk_memzone_reserve(SPDK_NVME_DRIVER_NAME,
					     sizeof(struct nvme_driver), SPDK_ENV_NUMA_ID_ANY,
					     SPDK_MEMZONE_NO_IOVA_CONTIG);
		}

		/* [한국어] hugepage 부족 등 — 치명적 실패. */
		if (g_spdk_nvme_driver == NULL) {
			SPDK_ERRLOG("primary process failed to reserve memory\n");
			pthread_mutex_unlock(&g_init_mutex);
			return -1;
		}
	} else {
		/* [한국어] secondary 분기 — primary가 만든 영역을 이름으로 lookup. */
		g_spdk_nvme_driver = spdk_memzone_lookup(SPDK_NVME_DRIVER_NAME);

		/* The unique named memzone already reserved by the primary process. */
		/* [한국어] lookup 성공 — primary가 영역은 만들었음. 그러나 primary의 init이 끝났는지 별도 대기. */
		if (g_spdk_nvme_driver != NULL) {
			/* [한국어] primary init 완료 대기 시간 (ms). */
			int ms_waited = 0;

			/* Wait the nvme driver to get initialized. */
			/* [한국어] initialized=true 또는 timeout(180s)까지 1ms 간격 polling. */
			while ((g_spdk_nvme_driver->initialized == false) &&
			       (ms_waited < g_nvme_driver_timeout_ms)) {
				/* [한국어] 1ms씩 누적. */
				ms_waited++;
				/* [한국어] 1ms backoff — busy-wait 회피. */
				nvme_delay(1000); /* delay 1ms */
			}
			/* [한국어] timeout 도달 — primary가 죽었거나 init 실패. */
			if (g_spdk_nvme_driver->initialized == false) {
				SPDK_ERRLOG("timeout waiting for primary process to init\n");
				pthread_mutex_unlock(&g_init_mutex);
				return -1;
			}
		} else {
			/* [한국어] primary가 아직 시작 안 함 — secondary는 primary 없이 못 시작. */
			SPDK_ERRLOG("primary process is not started yet\n");
			pthread_mutex_unlock(&g_init_mutex);
			return -1;
		}

		/* [한국어] secondary는 여기서 종료 — primary가 채운 g_spdk_nvme_driver만 가지고 동작. */
		pthread_mutex_unlock(&g_init_mutex);
		return 0;
	}

	/*
	 * At this moment, only one thread from the primary process will do
	 * the g_spdk_nvme_driver initialization
	 */
	/* [한국어] 위 secondary 분기 return으로 빠진 후 — 여기 도달은 primary 단일 thread만. */
	assert(spdk_process_is_primary());

	/* [한국어] robust mutex 초기화 — pthread mutex의 PI futex 기반 변형으로,
	 *  holder process가 죽어도 다음 lock 시도자가 EOWNERDEAD 받고 복구 가능. */
	ret = nvme_robust_mutex_init_shared(&g_spdk_nvme_driver->lock);
	/* [한국어] mutex init 실패 — 매우 드물지만 hugepage 손상 등. */
	if (ret != 0) {
		SPDK_ERRLOG("failed to initialize mutex\n");
		/* [한국어] 할당된 hugepage 영역도 반환 — clean failure. */
		spdk_memzone_free(SPDK_NVME_DRIVER_NAME);
		pthread_mutex_unlock(&g_init_mutex);
		return ret;
	}

	/* The lock in the shared g_spdk_nvme_driver object is now ready to
	 * be used - so we can unlock the g_init_mutex here.
	 */
	/* [한국어] init mutex 해제 — 이제 robust mutex로 직렬화 가능 (process-shared). */
	pthread_mutex_unlock(&g_init_mutex);
	/* [한국어] 본격적인 driver 객체 init은 robust mutex 보호 하에서 진행. */
	nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);

	/* [한국어] 명시적으로 false — secondary가 이 시점에 상태 보면 polling 계속. */
	g_spdk_nvme_driver->initialized = false;
	/* [한국어] hotplug 이벤트 listen FD — uevent netlink socket. PCIe device add/remove 감지용. */
	g_spdk_nvme_driver->hotplug_fd = spdk_pci_event_listen();
	/* [한국어] netlink 미지원 환경 — DEBUG 로그만, 치명적 아님. hotplug 비활성으로 계속. */
	if (g_spdk_nvme_driver->hotplug_fd < 0) {
		SPDK_DEBUGLOG(nvme, "Failed to open uevent netlink socket\n");
	}

	/* [한국어] PCIe 공유 ctrlr 리스트 빈 상태로 초기화 — 추후 attach 성공 시 여기에 매달림. */
	TAILQ_INIT(&g_spdk_nvme_driver->shared_attached_ctrlrs);

	/* [한국어] 기본 Host Identifier(64-bit Extended Host ID) UUID 생성 — NVMe-oF 등에서 host 식별용. */
	spdk_uuid_generate(&g_spdk_nvme_driver->default_extended_host_id);

	/* [한국어] robust mutex 해제 — 이제 secondary 들이 정상 진입 가능.
	 *  · 단, initialized=true 마킹은 caller(spdk_nvme_probe)가 transport 등록 등 추가 init 후 수행. */
	nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);

	/* [한국어] 0(성공) 또는 위에서 set된 ret. 통상 0. */
	return ret;
}

/* This function must only be called while holding g_spdk_nvme_driver->lock */
/*
 * [한국어]
 * nvme_ctrlr_probe - 발견된 단일 controller에 대한 probe 단계 처리
 *
 * @trid: 발견된 controller의 transport ID (PCIe BDF, NVMe-oF address 등)
 * @probe_ctx: 진행 중인 probe context (probe_cb / attach_cb / remove_cb 보유)
 * @devhandle: transport-specific device handle (PCIe면 spdk_pci_device*)
 * @return: 0 — 정상 처리 (init_ctrlrs 추가 또는 이미 attached), 1 — probe_cb가 false (skip), 음수 errno 실패
 *
 * 동기화: caller(transport scan)가 g_spdk_nvme_driver->lock 보유 상태로 호출
 *
 * 동작:
 *   1) 기본 ctrlr_opts 채우기 (timeout, qpair 수 등)
 *   2) probe_cb 호출 → false 면 1 반환 (사용자가 attach 거부)
 *   3) 같은 trid의 ctrlr가 이미 attached 인지 검사:
 *      · 있고 destruct 중 — EBUSY 실패
 *      · 있고 정상 — ref count 증가 + 사용자 attach_cb 호출
 *   4) 신규 — transport별 ctrlr_construct 호출 후 init_ctrlrs 리스트에 추가
 */
int
nvme_ctrlr_probe(const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvme_probe_ctx *probe_ctx, void *devhandle)
{
	/* [한국어] 신규 또는 기존 controller. */
	struct spdk_nvme_ctrlr *ctrlr;
	/* [한국어] 사용자에게 노출할 ctrlr 옵션 (probe_cb가 수정 가능). */
	struct spdk_nvme_ctrlr_opts opts;

	/* [한국어] caller invariant — trid는 transport scan에서 valid한 객체 전달 보장. */
	assert(trid != NULL);

	/* [한국어] 기본 옵션 채우기 — IO qpair 수, timeout, NQN 기본값 등. */
	spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, sizeof(opts));

	/* [한국어] probe_cb 호출 (사용자가 attach 결정).
	 *  · NULL 또는 true 반환 → attach 진행
	 *  · false 반환 → 1 반환 (skip). */
	if (!probe_ctx->probe_cb || probe_ctx->probe_cb(probe_ctx->cb_ctx, trid, &opts)) {
		/* [한국어] 같은 trid + hostnqn 조합으로 이미 attached된 ctrlr 검색 (lock 보유 중이라 _unsafe). */
		ctrlr = nvme_get_ctrlr_by_trid_unsafe(trid, opts.hostnqn);
		/* [한국어] 이미 존재 — 신규 attach 대신 ref count 증가로 처리. */
		if (ctrlr) {
			/* This ctrlr already exists. */

			/* [한국어] destruct 중인 ctrlr — 새 attach 거부 (race 방지). */
			if (ctrlr->is_destructed) {
				/* This ctrlr is being destructed asynchronously. */
				NVME_CTRLR_ERRLOG(ctrlr, "NVMe controller for SSD: %s is being destructed\n",
						  trid->traddr);
				/* [한국어] 사용자에게 attach 실패 통지. */
				probe_ctx->attach_fail_cb(probe_ctx->cb_ctx, trid, -EBUSY);
				return -EBUSY;
			}

			/* Increase the ref count before calling attach_cb() as the user may
			* call nvme_detach() immediately. */
			/* [한국어] ref 먼저 증가 — 사용자가 attach_cb 안에서 detach 호출해도 안전. */
			nvme_ctrlr_proc_get_ref(ctrlr);

			/* [한국어] attach_cb 호출 — 사용자에게 ctrlr 객체 전달.
			 *  · lock을 풀고 호출 (사용자 콜백이 SPDK 내부 함수 재진입할 수 있으므로 deadlock 회피). */
			if (probe_ctx->attach_cb) {
				nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
				probe_ctx->attach_cb(probe_ctx->cb_ctx, &ctrlr->trid, ctrlr, &ctrlr->opts);
				/* [한국어] 콜백 종료 후 lock 재획득 — 호출자(transport scan)가 lock 보유 상태로 돌아간다고 약속. */
				nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
			}
			return 0;
		}

		/* [한국어] 신규 controller — transport별 construct (PCIe / RDMA / TCP).
		 *  · 이 시점에선 BAR 매핑·기본 객체 할당까지만. 실제 admin enable 등은 process_init에서. */
		ctrlr = nvme_transport_ctrlr_construct(trid, &opts, devhandle);
		/* [한국어] construct 실패 — 보통 BAR 매핑 실패 또는 OOM. */
		if (ctrlr == NULL) {
			SPDK_ERRLOG("Failed to construct NVMe controller for SSD: %s\n", trid->traddr);
			probe_ctx->attach_fail_cb(probe_ctx->cb_ctx, trid, -ENODEV);
			return -1;
		}
		/* [한국어] hot-removal 콜백 + 사용자 cb_ctx 저장 — 추후 hotplug 이벤트에 대응. */
		ctrlr->remove_cb = probe_ctx->remove_cb;
		ctrlr->cb_ctx = probe_ctx->cb_ctx;

		/* [한국어] init_ctrlrs 리스트에 추가 — process_init 상태머신 진행 대상. */
		TAILQ_INSERT_TAIL(&probe_ctx->init_ctrlrs, ctrlr, tailq);
		return 0;
	}

	/* [한국어] probe_cb 가 false — skip. caller에게 1 반환. */
	return 1;
}

/*
 * [한국어]
 * nvme_ctrlr_poll_internal - 단일 ctrlr의 process_init 한 단계 폴링 + 상태 전환 처리
 *
 * @ctrlr: init_ctrlrs 리스트의 ctrlr 1개
 * @probe_ctx: 진행 중인 probe context (init/failed 리스트, 콜백)
 *
 * 호출 컨텍스트: spdk_nvme_probe_poll_async가 init_ctrlrs SAFE 순회 안에서 호출.
 *
 * 3분기 동작:
 *   [1] process_init이 0 아닌 값 반환 = 초기화 실패
 *       - init_ctrlrs에서 제거, attach_fail_cb 호출, ctrlr->is_failed=true 마킹
 *       - destruct context 할당 후 failed_ctxs에 누적 → 비동기 destruct 진행
 *       - calloc 실패 시 동기 destruct fallback
 *   [2] state != READY = 아직 진행 중 → return (다음 polling 시 재진입)
 *   [3] state == READY = 초기화 완료
 *       - io_producers STAILQ 초기화
 *       - init_ctrlrs에서 제거 → attached 리스트로 이동 (PCIe면 shared, 그 외 process-local)
 *       - ref count 증가 (attach_cb에서 사용자가 즉시 detach 호출 가능 대비)
 *       - attach_cb 호출 → 사용자가 ctrlr 사용 시작
 */
static void
nvme_ctrlr_poll_internal(struct spdk_nvme_ctrlr *ctrlr,
			 struct spdk_nvme_probe_ctx *probe_ctx)
{
	int rc = 0;
	struct nvme_ctrlr_detach_ctx *detach_ctx;

	rc = nvme_ctrlr_process_init(ctrlr);
                                  /* [한국어] ctrlr 상태머신 한 단계 진행 — CC.EN 설정/identify/AER 등 40+ 상태 전이 */

	if (rc) {
		/* Controller failed to initialize. */
		TAILQ_REMOVE(&probe_ctx->init_ctrlrs, ctrlr, tailq);
                                  /* [한국어] 진행 대기열에서 제거 — 더 이상 polling 대상 아님 */
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to initialize SSD: %s\n", ctrlr->trid.traddr);
		probe_ctx->attach_fail_cb(probe_ctx->cb_ctx, &ctrlr->trid, rc);
                                  /* [한국어] 사용자에게 실패 통지 — rc로 원인 진단 가능 */
		nvme_ctrlr_lock(ctrlr);
		nvme_ctrlr_fail(ctrlr, false);
                                  /* [한국어] is_failed=true 마킹 + outstanding I/O 모두 abort. false=hot_remove 아님 */
		nvme_ctrlr_unlock(ctrlr);

		/* allocate a context to detach this controller asynchronously */
		detach_ctx = calloc(1, sizeof(*detach_ctx));
                                  /* [한국어] destruct context 할당 — 비동기 destruct 진행 추적용 */
		if (detach_ctx == NULL) {
			NVME_CTRLR_WARNLOG(ctrlr,
					   "Failed to allocate asynchronous detach context. Performing synchronous destruct.\n");
			nvme_ctrlr_destruct(ctrlr);
                                  /* [한국어] OOM fallback — 동기 destruct (폴링 중 blocking이지만 어쩔 수 없음) */
			return;
		}
		detach_ctx->ctrlr = ctrlr;
		TAILQ_INSERT_TAIL(&probe_ctx->failed_ctxs.head, detach_ctx, link);
                                  /* [한국어] failed_ctxs에 누적 — probe_poll_async가 destruct 진행 폴링 */
		nvme_ctrlr_destruct_async(ctrlr, detach_ctx);
                                  /* [한국어] 비동기 destruct 시작 — admin shutdown 등 진행 */
		return;
	}

	if (ctrlr->state != NVME_CTRLR_STATE_READY) {
		return;
                                  /* [한국어] 아직 진행 중 — 다음 polling 사이클에서 재호출 */
	}

	STAILQ_INIT(&ctrlr->io_producers);
                                  /* [한국어] io_producers 빈 STAILQ 초기화 — io_msg 채널 등록처. READY 시점에 한 번만. */

	/*
	 * Controller has been initialized.
	 *  Move it to the attached_ctrlrs list.
	 */
	TAILQ_REMOVE(&probe_ctx->init_ctrlrs, ctrlr, tailq);
                                  /* [한국어] 진행 대기열에서 제거 — 이제 attached 리스트로 이동 */

	nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
                                  /* [한국어] driver lock — attached 리스트 race 방지 */
	if (nvme_ctrlr_shared(ctrlr)) {
		TAILQ_INSERT_TAIL(&g_spdk_nvme_driver->shared_attached_ctrlrs, ctrlr, tailq);
                                  /* [한국어] PCIe → multi-process 공유 리스트 */
	} else {
		TAILQ_INSERT_TAIL(&g_nvme_attached_ctrlrs, ctrlr, tailq);
                                  /* [한국어] Fabrics → process-local 리스트 */
	}

	/*
	 * Increase the ref count before calling attach_cb() as the user may
	 * call nvme_detach() immediately.
	 */
	nvme_ctrlr_proc_get_ref(ctrlr);
                                  /* [한국어] attach_cb 안에서 사용자가 detach 호출해도 안전하도록 ref 먼저 증가 */
	nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);

	if (probe_ctx->attach_cb) {
		probe_ctx->attach_cb(probe_ctx->cb_ctx, &ctrlr->trid, ctrlr, &ctrlr->opts);
                                  /* [한국어] 사용자 attach_cb 호출 — 사용자가 ctrlr 객체 받고 사용 시작 */
	}
}

/*
 * [한국어]
 * nvme_init_controllers - probe_poll_async 동기 wrapper (모든 ctrlr 처리 완료까지 busy-wait)
 *
 * @probe_ctx: probe context
 * @return spdk_nvme_probe_poll_async의 마지막 반환값 (0 또는 음수 errno).
 *
 * 동작: -EAGAIN 동안 무한 반복 → 0 또는 errno 받으면 반환.
 *
 * 사용처: spdk_nvme_probe(_ext) 동기 wrapper의 마지막 단계 — 비동기 probe를 동기적으로 마침.
 *         CPU 집약 — reactor 컨텍스트에서는 권장 안 함 (사용자가 직접 probe_poll_async 호출).
 */
static int
nvme_init_controllers(struct spdk_nvme_probe_ctx *probe_ctx)
{
	int rc = 0;

	while (true) {
		rc = spdk_nvme_probe_poll_async(probe_ctx);
                                  /* [한국어] 한 단계 폴링 — init/failed 리스트 진행 */
		if (rc != -EAGAIN) {
			return rc;
                                  /* [한국어] 0(완료) 또는 음수(에러) — 즉시 반환 */
		}
                                  /* [한국어] -EAGAIN — 아직 처리할 ctrlr 남음, 계속 반복 */
	}

	return rc;
                                  /* [한국어] unreachable (위 while(true)가 영원히 루프), 컴파일러 만족용 */
}

/* This function must not be called while holding g_spdk_nvme_driver->lock */
/*
 * [한국어]
 * nvme_get_ctrlr_by_trid - trid + hostnqn 으로 attached controller 검색 (lock 자동 획득)
 *
 * @trid: 검색할 transport ID (PCIe BDF or NVMe-oF address)
 * @hostnqn: 검색 조건의 hostnqn (NULL이면 trid만 비교)
 * @return: 일치하는 controller 또는 NULL
 *
 * caller가 lock을 잡지 않은 상태에서 호출하는 thin wrapper. 내부에서 lock 획득/해제 자동 처리.
 */
static struct spdk_nvme_ctrlr *
nvme_get_ctrlr_by_trid(const struct spdk_nvme_transport_id *trid, const char *hostnqn)
{
	/* [한국어] 결과 ctrlr. */
	struct spdk_nvme_ctrlr *ctrlr;

	/* [한국어] driver lock 획득 — process-local + shared 리스트 모두 안전하게 순회. */
	nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
	/* [한국어] 실제 검색은 _unsafe 버전에 위임. */
	ctrlr = nvme_get_ctrlr_by_trid_unsafe(trid, hostnqn);
	/* [한국어] lock 해제. */
	nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);

	/* [한국어] 결과 반환 — caller는 lock 없는 상태에서 ctrlr 사용 (race 위험은 ref count로 별도 관리). */
	return ctrlr;
}

/* This function must be called while holding g_spdk_nvme_driver->lock */
/*
 * [한국어]
 * nvme_get_ctrlr_by_trid_unsafe - 양 리스트 순회로 ctrlr 검색 (lock 보유 가정)
 *
 * @trid: transport ID
 * @hostnqn: hostnqn (NULL이면 무시)
 * @return: 일치 ctrlr 또는 NULL
 *
 * "_unsafe" 접미사는 호출자가 g_spdk_nvme_driver->lock 을 보유한 상태로 호출해야 한다는 의미.
 *
 * 검색 범위:
 *   1) g_nvme_attached_ctrlrs (process-local — Fabrics)
 *   2) g_spdk_nvme_driver->shared_attached_ctrlrs (multi-process 공유 — PCIe)
 *
 * 일치 조건: trid 비교 == 0 AND (hostnqn 미지정 OR hostnqn 일치)
 */
struct spdk_nvme_ctrlr *
nvme_get_ctrlr_by_trid_unsafe(const struct spdk_nvme_transport_id *trid, const char *hostnqn)
{
	/* [한국어] 순회 변수. */
	struct spdk_nvme_ctrlr *ctrlr;

	/* Search per-process list */
	/* [한국어] process-local 리스트 순회 — Fabrics(RDMA/TCP) ctrlr들. */
	TAILQ_FOREACH(ctrlr, &g_nvme_attached_ctrlrs, tailq) {
		/* [한국어] trid 일치 검사 — 다르면 다음 ctrlr로. */
		if (spdk_nvme_transport_id_compare(&ctrlr->trid, trid) != 0) {
			continue;
		}
		/* [한국어] hostnqn 명시되었으면 일치 검사. */
		if (hostnqn && strcmp(ctrlr->opts.hostnqn, hostnqn) != 0) {
			continue;
		}
		/* [한국어] 양 조건 모두 일치 — 즉시 반환. */
		return ctrlr;
	}

	/* Search multi-process shared list */
	/* [한국어] 공유 리스트 순회 — PCIe ctrlr들 (다른 프로세스가 attach한 것도 보임). */
	TAILQ_FOREACH(ctrlr, &g_spdk_nvme_driver->shared_attached_ctrlrs, tailq) {
		/* [한국어] trid 비교. */
		if (spdk_nvme_transport_id_compare(&ctrlr->trid, trid) != 0) {
			continue;
		}
		/* [한국어] hostnqn 비교. */
		if (hostnqn && strcmp(ctrlr->opts.hostnqn, hostnqn) != 0) {
			continue;
		}
		/* [한국어] 일치. */
		return ctrlr;
	}

	/* [한국어] 양쪽 모두에서 못 찾음. */
	return NULL;
}

/* This function must only be called while holding g_spdk_nvme_driver->lock */
/*
 * [한국어]
 * nvme_probe_internal - probe의 실제 동작 함수 (transport scan + secondary 자동 attach)
 *
 * @probe_ctx: 진행 중인 probe context (trid, 콜백 보유)
 * @direct_connect: true=NVMe-oF 직접 연결 모드(spdk_nvme_connect 경로),
 *                  false=enumerate 모드(spdk_nvme_probe 경로)
 * @return 0 성공, -1 실패.
 *
 * "_internal" 접미사: nvme.c 외부에서는 호출 안 됨. probe_async_ext와 connect_async가 호출자.
 *
 * 동작 5단계:
 *   [1] trstring 파생 — 사용자가 비워뒀으면 trtype에서 자동 채움 (PCIe, RDMA, TCP 등 문자열).
 *   [2] 트랜스포트 등록 여부 검사 — 미등록이면 -1.
 *   [3] driver lock 획득 → nvme_transport_ctrlr_scan으로 트랜스포트별 enumerate.
 *       실패 시 init_ctrlrs 정리 + attach_fail_cb 호출 후 -1.
 *   [4] secondary + PCIe 모드: shared_attached_ctrlrs 순회 — primary가 이미 attach한 ctrlr들에
 *       대해 자동 attach (이 프로세스 컨텍스트만 추가). 사용자 trid 매칭 + hostnqn 일치 + 이 프로세스가
 *       이미 init 했는지 검사.
 *   [5] lock 해제 + 0 반환.
 */
static int
nvme_probe_internal(struct spdk_nvme_probe_ctx *probe_ctx,
		    bool direct_connect)
{
	int rc;
	struct spdk_nvme_ctrlr *ctrlr, *ctrlr_tmp;
	const struct spdk_nvme_ctrlr_opts *opts = probe_ctx->opts;

	if (strlen(probe_ctx->trid.trstring) == 0) {
		/* If user didn't provide trstring, derive it from trtype */
		spdk_nvme_trid_populate_transport(&probe_ctx->trid, probe_ctx->trid.trtype);
                                  /* [한국어] trstring이 비어 있으면 trtype에서 표준 문자열 채움 ("PCIE", "RDMA" 등) */
	}

	if (!spdk_nvme_transport_available_by_name(probe_ctx->trid.trstring)) {
                                  /* [한국어] 트랜스포트 레지스트리에 등록된 트랜스포트인지 검사 (SPDK_NVME_TRANSPORT_REGISTER로 등록됨) */
		SPDK_ERRLOG("NVMe trtype %u (%s) not available\n",
			    probe_ctx->trid.trtype, probe_ctx->trid.trstring);
		return -1;
	}

	nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
                                  /* [한국어] driver lock — scan 중 attached 리스트 race 방지 */

	rc = nvme_transport_ctrlr_scan(probe_ctx, direct_connect);
                                  /* [한국어] 트랜스포트별 vtable의 ctrlr_scan 호출.
                                   *  PCIe: DPDK enumerate → 각 device마다 nvme_ctrlr_probe.
                                   *  Fabrics direct_connect=true: 단일 trid로 nvme_ctrlr_probe 1회. */
	if (rc != 0) {
		SPDK_ERRLOG("NVMe ctrlr scan failed\n");
		TAILQ_FOREACH_SAFE(ctrlr, &probe_ctx->init_ctrlrs, tailq, ctrlr_tmp) {
                                  /* [한국어] scan 실패 — 부분 추가된 init_ctrlrs 정리 */
			TAILQ_REMOVE(&probe_ctx->init_ctrlrs, ctrlr, tailq);
			probe_ctx->attach_fail_cb(probe_ctx->cb_ctx, &ctrlr->trid, -EFAULT);
                                  /* [한국어] 사용자에게 각 ctrlr별 실패 통지 */
			nvme_transport_ctrlr_destruct(ctrlr);
                                  /* [한국어] 트랜스포트별 정리 (BAR unmap 등) */
		}
		nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
		return -1;
	}

	/*
	 * Probe controllers on the shared_attached_ctrlrs list
	 */
	if (!spdk_process_is_primary() && (probe_ctx->trid.trtype == SPDK_NVME_TRANSPORT_PCIE)) {
                                  /* [한국어] secondary + PCIe — primary가 이미 attach한 PCIe ctrlr들에 자동 attach 시도.
                                   *  shared_attached_ctrlrs에 이미 ctrlr 객체가 존재하므로 새로 만들 필요 없음. */
		TAILQ_FOREACH(ctrlr, &g_spdk_nvme_driver->shared_attached_ctrlrs, tailq) {
			/* Do not attach other ctrlrs if user specify a valid trid */
			if ((strlen(probe_ctx->trid.traddr) != 0) &&
			    (spdk_nvme_transport_id_compare(&probe_ctx->trid, &ctrlr->trid))) {
                                  /* [한국어] 사용자가 특정 trid 지정 — 그 trid와 다른 ctrlr는 skip */
				continue;
			}

			if (opts && strcmp(opts->hostnqn, ctrlr->opts.hostnqn) != 0) {
                                  /* [한국어] hostnqn 불일치 — 다른 host로 attach된 ctrlr는 이 attach 대상 아님 */
				continue;
			}

			/* Do not attach if we failed to initialize it in this process */
			if (nvme_ctrlr_get_current_process(ctrlr) == NULL) {
                                  /* [한국어] 이 프로세스 컨텍스트가 이 ctrlr에 등록 안 됨 — 트랜스포트 scan에서 init 실패한 케이스 */
				continue;
			}

			nvme_ctrlr_proc_get_ref(ctrlr);
                                  /* [한국어] ref 증가 — attach_cb 안에서 사용자가 detach 호출해도 안전 */

			/*
			 * Unlock while calling attach_cb() so the user can call other functions
			 *  that may take the driver lock, like nvme_detach().
			 */
			if (probe_ctx->attach_cb) {
				nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
                                  /* [한국어] 사용자 콜백 동안 lock 해제 — 사용자가 detach 등 lock 잡는 함수 호출 가능 */
				probe_ctx->attach_cb(probe_ctx->cb_ctx, &ctrlr->trid, ctrlr, &ctrlr->opts);
				nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
                                  /* [한국어] 콜백 종료 후 lock 재획득 — 다음 순회 안전성 */
			}
		}
	}

	nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);

	return 0;
}

/*
 * [한국어]
 * nvme_dummy_attach_fail_cb - 사용자가 attach_fail_cb 미지정 시 사용되는 기본 콜백
 *
 * 동작: SPDK_ERRLOG로 trid 정보와 errno를 출력. 사용자에게 별도 통지 없음.
 *
 * 의도: legacy spdk_nvme_probe(attach_fail_cb 인자 없음) 호환 — 실패 정보를 최소한 로그에는 남김.
 */
static void
nvme_dummy_attach_fail_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
			  int rc)
{
	SPDK_ERRLOG("Failed to attach nvme ctrlr: trtype=%s adrfam=%s traddr=%s trsvcid=%s "
		    "subnqn=%s, %s\n", spdk_nvme_transport_id_trtype_str(trid->trtype),
		    spdk_nvme_transport_id_adrfam_str(trid->adrfam), trid->traddr, trid->trsvcid,
		    trid->subnqn, spdk_strerror(-rc));
                                  /* [한국어] 모든 trid 필드 + errno 문자열 — 사용자 진단을 위한 단일 로그 라인 */
}

/*
 * [한국어]
 * nvme_probe_ctx_init - probe_ctx 필드 일괄 초기화 (헬퍼)
 *
 * @probe_ctx: heap 할당된 빈 컨텍스트
 * @trid:      검색할 transport ID
 * @opts:      ctrlr 옵션 (NULL 허용)
 * @cb_ctx:    사용자 컨텍스트
 * @probe_cb / attach_cb / attach_fail_cb / remove_cb: 사용자 콜백 4종
 *
 * 동작:
 *   trid 복사 (포인터가 아니라 값 복사 — caller가 trid 메모리 free 가능),
 *   콜백 4개 저장 — attach_fail_cb는 NULL이면 dummy로 fall-through,
 *   init/failed 리스트 빈 상태 명시 초기화.
 */
static void
nvme_probe_ctx_init(struct spdk_nvme_probe_ctx *probe_ctx,
		    const struct spdk_nvme_transport_id *trid,
		    const struct spdk_nvme_ctrlr_opts *opts,
		    void *cb_ctx,
		    spdk_nvme_probe_cb probe_cb,
		    spdk_nvme_attach_cb attach_cb,
		    spdk_nvme_attach_fail_cb attach_fail_cb,
		    spdk_nvme_remove_cb remove_cb)
{
	probe_ctx->trid = *trid;
                                  /* [한국어] 값 복사 — caller가 trid 메모리 free해도 probe_ctx는 안전 */
	probe_ctx->opts = opts;
                                  /* [한국어] opts는 포인터 보존 (사용자 보장 — probe 종료까지 유효) */
	probe_ctx->cb_ctx = cb_ctx;
	probe_ctx->probe_cb = probe_cb;
	probe_ctx->attach_cb = attach_cb;
	if (attach_fail_cb != NULL) {
		probe_ctx->attach_fail_cb = attach_fail_cb;
                                  /* [한국어] 사용자 지정 fail 콜백 사용 */
	} else {
		probe_ctx->attach_fail_cb = nvme_dummy_attach_fail_cb;
                                  /* [한국어] legacy 호환 — dummy로 SPDK_ERRLOG만 */
	}
	probe_ctx->remove_cb = remove_cb;
	TAILQ_INIT(&probe_ctx->init_ctrlrs);
                                  /* [한국어] 진행 중 ctrlr 빈 리스트로 초기화 */
	TAILQ_INIT(&probe_ctx->failed_ctxs.head);
                                  /* [한국어] 실패 ctrlr destruct context 빈 리스트로 초기화 */
}

/*
 * [한국어]
 * spdk_nvme_probe - SPDK NVMe driver의 *가장 핵심적인* 사용자 진입 API (동기 wrapper)
 *
 * @trid: 검색할 transport ID. NULL이면 모든 PCIe NVMe device 자동 enumerate.
 * @cb_ctx: 사용자 컨텍스트 — 모든 콜백에 전달됨 (probe_cb / attach_cb / remove_cb)
 * @probe_cb: 각 발견 device에 대해 호출 — true 반환하면 attach 진행, false면 skip
 * @attach_cb: attach 성공 시 호출 — 사용자에게 spdk_nvme_ctrlr 전달, 이후 사용 가능
 * @remove_cb: hot-removal 감지 시 호출 (선택)
 * @return: 0 성공, -1 실패
 *
 * 이 함수가 SPDK NVMe driver의 main entry point. 사용자 코드(perf, examples, bdev_nvme)는 거의
 * 모두 이 함수로 시작.
 *
 * 동작: 내부적으로 spdk_nvme_probe_ext에 attach_fail_cb=NULL로 위임 (legacy 호환).
 *
 * 호출 체인:
 *   user → spdk_nvme_probe → spdk_nvme_probe_ext → spdk_nvme_probe_async_ext
 *     → nvme_driver_init (g_spdk_nvme_driver 초기화)
 *     → nvme_probe_internal → nvme_transport_ctrlr_scan (transport별 enumerate)
 *       → 각 device 발견 시 nvme_ctrlr_probe → probe_cb → ctrlr_construct
 *     → nvme_init_controllers (process_init 상태머신 진행)
 *       → controller READY 도달 시 attach_cb 호출
 */
int
spdk_nvme_probe(const struct spdk_nvme_transport_id *trid, void *cb_ctx,
		spdk_nvme_probe_cb probe_cb, spdk_nvme_attach_cb attach_cb,
		spdk_nvme_remove_cb remove_cb)
{
	/* [한국어] _ext 버전에 attach_fail_cb=NULL 위임 — 실패 시 내부 dummy 콜백이 SPDK_ERRLOG로 처리. */
	return spdk_nvme_probe_ext(trid, cb_ctx, probe_cb, attach_cb, NULL, remove_cb);
}

/*
 * [한국어]
 * spdk_nvme_probe_ext - probe API의 확장 버전 (attach_fail_cb 추가)
 *
 * @attach_fail_cb: attach 실패 시 호출 — 사용자가 실패 원인을 받아 처리. NULL이면 dummy 사용.
 * 그 외 인자: spdk_nvme_probe와 동일
 *
 * 동작:
 *   1) trid==NULL 이면 PCIe 전체 enumerate trid 생성
 *   2) spdk_nvme_probe_async_ext 호출하여 probe_ctx 생성 + 비동기 probe 시작
 *   3) probe_ctx로 nvme_init_controllers 폴링 → 모든 ctrlr가 READY 또는 실패까지 대기
 *
 * 동기 wrapper지만 내부는 비동기 + polling 패턴.
 */
int
spdk_nvme_probe_ext(const struct spdk_nvme_transport_id *trid, void *cb_ctx,
		    spdk_nvme_probe_cb probe_cb, spdk_nvme_attach_cb attach_cb,
		    spdk_nvme_attach_fail_cb attach_fail_cb, spdk_nvme_remove_cb remove_cb)
{
	/* [한국어] trid==NULL 케이스에서 사용할 PCIe 기본 trid (스택 객체). */
	struct spdk_nvme_transport_id trid_pcie;
	/* [한국어] 비동기 probe context — async API 가 할당해 줌. */
	struct spdk_nvme_probe_ctx *probe_ctx;

	/* [한국어] trid 미지정 — PCIe 전체 enumerate 의도로 해석. */
	if (trid == NULL) {
		/* [한국어] 빈 trid 0-init. */
		memset(&trid_pcie, 0, sizeof(trid_pcie));
		/* [한국어] trtype = PCIe 로 명시적으로 채움. */
		spdk_nvme_trid_populate_transport(&trid_pcie, SPDK_NVME_TRANSPORT_PCIE);
		/* [한국어] 사용자 인자 trid를 로컬 trid_pcie로 redirect. */
		trid = &trid_pcie;
	}

	/* [한국어] 비동기 probe 시작 — probe_ctx 할당 + transport scan + ctrlr_construct까지 진행. */
	probe_ctx = spdk_nvme_probe_async_ext(trid, cb_ctx, probe_cb,
					      attach_cb, attach_fail_cb, remove_cb);
	/* [한국어] probe_ctx==NULL 이면 nvme_driver_init 또는 transport_scan 실패. */
	if (!probe_ctx) {
		SPDK_ERRLOG("Create probe context failed\n");
		return -1;
	}

	/*
	 * Keep going even if one or more nvme_attach() calls failed,
	 *  but maintain the value of rc to signal errors when we return.
	 */
	return nvme_init_controllers(probe_ctx);
}

/*
 * [한국어]
 * nvme_connect_probe_cb - spdk_nvme_connect 경로 전용 probe_cb (사용자 opts를 강제 적용)
 *
 * @cb_ctx: 사용자 opts 포인터 (probe_ctx 생성 시 cb_ctx로 등록됨)
 * @trid:   대상 ctrlr trid
 * @opts:   기본 opts (덮어씀)
 * @return  항상 true (attach 진행)
 *
 * 의도: spdk_nvme_connect는 probe API 위에 구축됨 — connect의 opts를 모든 발견 ctrlr에 강제 적용.
 *       (connect는 단일 trid 대상이므로 한 ctrlr만 검사)
 */
static bool
nvme_connect_probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		      struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_ctrlr_opts *requested_opts = cb_ctx;
                                  /* [한국어] probe_ctx_init 시 cb_ctx에 사용자 opts 포인터 저장한 것 회수 */

	assert(requested_opts);
	memcpy(opts, requested_opts, sizeof(*opts));
                                  /* [한국어] 기본 opts를 사용자 opts로 완전 덮어쓰기 */

	return true;
                                  /* [한국어] 항상 attach 진행 — connect의 명시적 의도 */
}

/*
 * [한국어]
 * nvme_ctrlr_opts_init - 사용자 opts를 ABI-호환 방식으로 라이브러리 opts에 복사
 *
 * @opts:           [out] 라이브러리가 사용할 정상화된 opts
 * @opts_user:      사용자가 제공한 opts (구버전 헤더로 빌드되어 작은 크기일 수 있음)
 * @opts_size_user: 사용자 opts 크기 — ABI 호환의 핵심
 *
 * ABI 호환 패턴 (FIELD_OK + SET_FIELD 매크로):
 *   - 라이브러리 헤더에 새 필드가 추가되어도, 사용자가 옛 헤더로 빌드한 경우
 *     opts_size_user는 옛 크기 → 새 필드 offset+sizeof > opts_size_user → 복사 안 함.
 *   - 결과: 옛 빌드 사용자도 안전하게 동작 (새 필드는 라이브러리 기본값 유지).
 *
 * 동작:
 *   [1] 라이브러리 기본 opts 채우기.
 *   [2] FIELD_OK으로 검사하며 사용자 값으로 덮어씀 (각 필드별).
 *   [3] 배열 필드는 SET_FIELD_ARRAY로 memcpy.
 *
 * 매크로 #undef은 함수 끝에서 — 다른 함수와 충돌 방지.
 */
static void
nvme_ctrlr_opts_init(struct spdk_nvme_ctrlr_opts *opts,
		     const struct spdk_nvme_ctrlr_opts *opts_user,
		     size_t opts_size_user)
{
	assert(opts);
	assert(opts_user);

	spdk_nvme_ctrlr_get_default_ctrlr_opts(opts, opts_size_user);
                                  /* [한국어] 기본값으로 채우기 — opts_size_user를 전달해 사용자가 알고 있는 크기까지만 채워짐 */

#define FIELD_OK(field) \
	offsetof(struct spdk_nvme_ctrlr_opts, field) + sizeof(opts->field) <= (opts->opts_size)
                                  /* [한국어] 필드의 offset + 크기가 opts_size 이내면 사용자가 알고 있다는 뜻 → 복사 OK */

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
			opts->field = opts_user->field; \
	}
                                  /* [한국어] 단일 필드 복사 — FIELD_OK 검사 통과 시만 */

#define SET_FIELD_ARRAY(field) \
	if (FIELD_OK(field)) { \
		memcpy(opts->field, opts_user->field, sizeof(opts_user->field)); \
	}
                                  /* [한국어] 배열 필드 복사 — sizeof(opts_user->field)는 컴파일 타임 배열 크기 */

	SET_FIELD(num_io_queues);
	SET_FIELD(use_cmb_sqs);
	SET_FIELD(no_shn_notification);
	SET_FIELD(enable_interrupts);
	SET_FIELD(arb_mechanism);
	SET_FIELD(arbitration_burst);
	SET_FIELD(low_priority_weight);
	SET_FIELD(medium_priority_weight);
	SET_FIELD(high_priority_weight);
	SET_FIELD(keep_alive_timeout_ms);
	SET_FIELD(transport_retry_count);
	SET_FIELD(io_queue_size);
	SET_FIELD_ARRAY(hostnqn);
	SET_FIELD(io_queue_requests);
	SET_FIELD_ARRAY(src_addr);
	SET_FIELD_ARRAY(src_svcid);
	SET_FIELD_ARRAY(host_id);
	SET_FIELD_ARRAY(extended_host_id);
	SET_FIELD(command_set);
	SET_FIELD(admin_timeout_ms);
	SET_FIELD(header_digest);
	SET_FIELD(data_digest);
	SET_FIELD(disable_error_logging);
	SET_FIELD(transport_ack_timeout);
	SET_FIELD(admin_queue_size);
	SET_FIELD(fabrics_connect_timeout_us);
	SET_FIELD(disable_read_ana_log_page);
	SET_FIELD(disable_read_changed_ns_list_log_page);
	SET_FIELD(tls_psk);
	SET_FIELD(dhchap_key);
	SET_FIELD(dhchap_ctrlr_key);
	SET_FIELD(dhchap_digests);
	SET_FIELD(dhchap_dhgroups);

#undef FIELD_OK
#undef SET_FIELD
#undef SET_FIELD_ARRAY
}

/*
 * [한국어]
 * spdk_nvme_connect - NVMe-oF (Fabrics) 단일 controller 직접 연결 (PCIe도 가능)
 *
 * @trid: 연결할 controller의 transport ID — Fabrics면 IP:port:subnqn 형태
 * @opts: ctrlr 옵션 (NULL이면 기본값)
 * @opts_size: ABI compat — 사용자가 빌드한 spdk_nvme_ctrlr_opts 크기
 * @return: 연결된 controller 또는 NULL
 *
 * spdk_nvme_probe와의 차이:
 *   - probe: enumerate 후 사용자 콜백으로 attach 결정 (multi-device)
 *   - connect: 단일 trid에 직접 연결 (NVMe-oF 시나리오에 자연스러움 — IP/포트 알고 시작)
 *
 * 동작:
 *   1) trid 검증 (NULL이면 실패)
 *   2) nvme_driver_init (멱등 — 이미 init되어 있으면 즉시 반환)
 *   3) opts 처리 + hostnqn 추출
 *   4) spdk_nvme_connect_async로 probe_ctx 생성
 *   5) nvme_init_controllers 폴링 — READY 까지 대기
 *   6) attach된 ctrlr 검색하여 반환
 */
struct spdk_nvme_ctrlr *
spdk_nvme_connect(const struct spdk_nvme_transport_id *trid,
		  const struct spdk_nvme_ctrlr_opts *opts, size_t opts_size)
{
	/* [한국어] return code. */
	int rc;
	/* [한국어] 결과 ctrlr — 성공 시 채워짐, 실패 시 NULL 반환. */
	struct spdk_nvme_ctrlr *ctrlr = NULL;
	/* [한국어] 비동기 probe context. */
	struct spdk_nvme_probe_ctx *probe_ctx;
	/* [한국어] 정상화된 옵션 포인터 — opts 주어지면 opts_local 가리킴. */
	struct spdk_nvme_ctrlr_opts *opts_local_p = NULL;
	/* [한국어] ABI 보정된 로컬 옵션 사본. */
	struct spdk_nvme_ctrlr_opts opts_local;
	/* [한국어] hostnqn 검색용 — opts가 있으면 거기서, 없으면 기본 host UUID 기반 NQN. */
	char hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];

	/* [한국어] trid 필수 — Fabrics 연결은 endpoint를 알아야 시작 가능. */
	if (trid == NULL) {
		SPDK_ERRLOG("No transport ID specified\n");
		return NULL;
	}

	/* [한국어] driver init 보장 — 멱등. */
	rc = nvme_driver_init();
	if (rc != 0) {
		return NULL;
	}

	/* [한국어] 기본 hostnqn 채우기 — UUID 기반 자동 생성 (예: "nqn.2014-08.org.nvmexpress:uuid:..."). */
	nvme_get_default_hostnqn(hostnqn, sizeof(hostnqn));
	/* [한국어] 사용자 opts 명시 — ABI 호환 처리 후 opts_local에 정상화된 사본 저장. */
	if (opts) {
		opts_local_p = &opts_local;
		/* [한국어] FIELD_OK 매크로 기반 ABI-safe 필드별 복사. opts_size_user보다 작은 필드만 복사. */
		nvme_ctrlr_opts_init(opts_local_p, opts, opts_size);
		/* [한국어] hostnqn은 opts에서 명시된 값으로 덮어씀 — 사용자 의도 우선. */
		memcpy(hostnqn, opts_local.hostnqn, sizeof(hostnqn));
	}

	/* [한국어] 비동기 connect 시작 — probe_ctx 할당 + transport connect 진행. */
	probe_ctx = spdk_nvme_connect_async(trid, opts_local_p, NULL);
	if (!probe_ctx) {
		SPDK_ERRLOG("Create probe context failed\n");
		return NULL;
	}

	/* [한국어] init 상태머신 폴링 — READY 또는 fatal 까지 대기. */
	rc = nvme_init_controllers(probe_ctx);
	if (rc != 0) {
		return NULL;
	}

	/* [한국어] 연결 성공 — trid + hostnqn으로 attached 리스트에서 ctrlr 검색하여 반환.
	 *  · attach_cb 가 따로 없는 connect API에선 이 검색이 결과 전달 채널. */
	ctrlr = nvme_get_ctrlr_by_trid(trid, hostnqn);

	/* [한국어] ctrlr 또는 NULL (race로 누군가 detach해버린 경우 등) 반환. */
	return ctrlr;
}

/*
 * [한국어]
 * spdk_nvme_trid_populate_transport - trtype 으로부터 표준 trstring 자동 채움 (사용자 노출 API)
 *
 * @trid:   [in/out] 갱신할 transport ID — trtype + trstring 둘 다 채워짐
 * @trtype: PCIe / RDMA / TCP / FC / VFIOUSER / CUSTOM
 *
 * 동작: trtype 저장 + 매핑 테이블에서 표준 문자열 추출 → trstring snprintf.
 *       문자열은 SPDK_NVME_TRANSPORT_NAME_* 상수 사용 (대문자 정규화).
 *
 * 사용처: 사용자가 trid->trtype만 채우고 trstring은 비웠을 때, probe_internal에서 자동 호출.
 */
void
spdk_nvme_trid_populate_transport(struct spdk_nvme_transport_id *trid,
				  enum spdk_nvme_transport_type trtype)
{
	const char *trstring;

	trid->trtype = trtype;
                                  /* [한국어] trtype 먼저 저장 */
	switch (trtype) {
	case SPDK_NVME_TRANSPORT_FC:
		trstring = SPDK_NVME_TRANSPORT_NAME_FC;
                                  /* [한국어] "FC" — Fibre Channel */
		break;
	case SPDK_NVME_TRANSPORT_PCIE:
		trstring = SPDK_NVME_TRANSPORT_NAME_PCIE;
                                  /* [한국어] "PCIE" — 로컬 PCIe NVMe */
		break;
	case SPDK_NVME_TRANSPORT_RDMA:
		trstring = SPDK_NVME_TRANSPORT_NAME_RDMA;
                                  /* [한국어] "RDMA" — Infiniband/RoCE */
		break;
	case SPDK_NVME_TRANSPORT_TCP:
		trstring = SPDK_NVME_TRANSPORT_NAME_TCP;
                                  /* [한국어] "TCP" — NVMe over TCP */
		break;
	case SPDK_NVME_TRANSPORT_VFIOUSER:
		trstring = SPDK_NVME_TRANSPORT_NAME_VFIOUSER;
                                  /* [한국어] "VFIOUSER" — vfio-user 가상화 트랜스포트 */
		break;
	case SPDK_NVME_TRANSPORT_CUSTOM:
		trstring = SPDK_NVME_TRANSPORT_NAME_CUSTOM;
                                  /* [한국어] "CUSTOM" — 외부 등록 트랜스포트용 placeholder */
		break;
	default:
		SPDK_ERRLOG("no available transports\n");
		assert(0);
                                  /* [한국어] 미정의 trtype — 사용자 버그. 디버그 빌드 abort. */
		return;
	}
	snprintf(trid->trstring, SPDK_NVMF_TRSTRING_MAX_LEN, "%s", trstring);
                                  /* [한국어] 안전 복사 — 최대 길이 제한 */
}

/*
 * [한국어]
 * spdk_nvme_transport_id_populate_trstring - 사용자 입력 문자열을 대문자 정규화하여 trstring에 저장
 *
 * @trid:     [out] 갱신할 transport ID
 * @trstring: 사용자 입력 (대소문자 무관)
 * @return    0 성공, -EINVAL.
 *
 * 동작: 한 글자씩 toupper 후 복사 — 트랜스포트 레지스트리는 대문자로 등록되어 있으므로
 *       사용자 입력을 대문자로 정규화하면 등록 트랜스포트와 매칭 가능.
 *
 * GCC-11 LTO false positive 회피 — strnlen 대신 수동 루프 사용 (GitHub #2391).
 */
int
spdk_nvme_transport_id_populate_trstring(struct spdk_nvme_transport_id *trid, const char *trstring)
{
	int i = 0;

	if (trid == NULL || trstring == NULL) {
		return -EINVAL;
	}

	/* Note: gcc-11 has some false positive -Wstringop-overread warnings with LTO builds if we
	 * use strnlen here.  So do the trstring copy manually instead.  See GitHub issue #2391.
	 */

	/* cast official trstring to uppercase version of input. */
	while (i < SPDK_NVMF_TRSTRING_MAX_LEN && trstring[i] != 0) {
                                  /* [한국어] 한 글자씩 처리 — 최대 길이 또는 NUL 만나면 종료 */
		trid->trstring[i] = toupper(trstring[i]);
                                  /* [한국어] 대문자 정규화 — 트랜스포트 레지스트리와 매칭 위해 */
		i++;
	}

	if (trstring[i] != 0) {
                                  /* [한국어] 길이 초과로 NUL 못 만남 — 사용자 입력 너무 김 */
		return -EINVAL;
	} else {
		trid->trstring[i] = 0;
                                  /* [한국어] NUL 종결 명시 — 사용자 입력의 NUL 위치까지 복사됨 */
		return 0;
	}
}

/*
 * [한국어]
 * spdk_nvme_transport_id_parse_trtype - trtype 문자열 → enum 변환 (사용자 노출 API)
 *
 * @trtype: [out] 결과 enum
 * @str:    "PCIe"/"RDMA"/"FC"/"TCP"/"VFIOUSER" 또는 임의 문자열(=CUSTOM)
 * @return  0 (항상 성공 — 미지정 케이스는 CUSTOM으로 fall-through), -EINVAL.
 *
 * 사용자가 RPC/CLI로 trtype 지정한 문자열을 파싱.
 */
int
spdk_nvme_transport_id_parse_trtype(enum spdk_nvme_transport_type *trtype, const char *str)
{
	if (trtype == NULL || str == NULL) {
		return -EINVAL;
	}

	if (strcasecmp(str, "PCIe") == 0) {
		*trtype = SPDK_NVME_TRANSPORT_PCIE;
	} else if (strcasecmp(str, "RDMA") == 0) {
		*trtype = SPDK_NVME_TRANSPORT_RDMA;
	} else if (strcasecmp(str, "FC") == 0) {
		*trtype = SPDK_NVME_TRANSPORT_FC;
	} else if (strcasecmp(str, "TCP") == 0) {
		*trtype = SPDK_NVME_TRANSPORT_TCP;
	} else if (strcasecmp(str, "VFIOUSER") == 0) {
		*trtype = SPDK_NVME_TRANSPORT_VFIOUSER;
	} else {
		*trtype = SPDK_NVME_TRANSPORT_CUSTOM;
                                  /* [한국어] 표준 5종 외 = CUSTOM (외부 등록 트랜스포트 가능성) */
	}
	return 0;
}

/*
 * [한국어]
 * spdk_nvme_transport_id_trtype_str - trtype enum → 사람용 문자열 (사용자 노출 API)
 *
 * @return 표준 표기 문자열 (예: "PCIe") 또는 NULL (미정의 enum).
 *
 * 위 parse_trtype과 비대칭 케이스: 출력은 mixed-case ("PCIe"), 입력은 강건성을 위해 대소문자 무관.
 */
const char *
spdk_nvme_transport_id_trtype_str(enum spdk_nvme_transport_type trtype)
{
	switch (trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		return "PCIe";
	case SPDK_NVME_TRANSPORT_RDMA:
		return "RDMA";
	case SPDK_NVME_TRANSPORT_FC:
		return "FC";
	case SPDK_NVME_TRANSPORT_TCP:
		return "TCP";
	case SPDK_NVME_TRANSPORT_VFIOUSER:
		return "VFIOUSER";
	case SPDK_NVME_TRANSPORT_CUSTOM:
		return "CUSTOM";
	default:
		return NULL;
                                  /* [한국어] 미정의 enum — 호출자가 진단 가능하도록 NULL 반환 */
	}
}

/*
 * [한국어]
 * spdk_nvme_transport_id_parse_adrfam - 주소 패밀리 문자열 → enum 변환 (사용자 노출 API)
 *
 * @adrfam: [out] 결과 enum
 * @str:    "IPv4"/"IPv6"/"IB"/"FC" (대소문자 무관)
 * @return  0 성공, -EINVAL/-ENOENT.
 *
 * NVMe-oF의 traddr 해석에 사용 — IPv4/6은 IP 주소, IB는 GID, FC는 WWN.
 */
int
spdk_nvme_transport_id_parse_adrfam(enum spdk_nvmf_adrfam *adrfam, const char *str)
{
	if (adrfam == NULL || str == NULL) {
		return -EINVAL;
	}

	if (strcasecmp(str, "IPv4") == 0) {
		*adrfam = SPDK_NVMF_ADRFAM_IPV4;
	} else if (strcasecmp(str, "IPv6") == 0) {
		*adrfam = SPDK_NVMF_ADRFAM_IPV6;
	} else if (strcasecmp(str, "IB") == 0) {
		*adrfam = SPDK_NVMF_ADRFAM_IB;
                                  /* [한국어] InfiniBand GID — RDMA 트랜스포트 */
	} else if (strcasecmp(str, "FC") == 0) {
		*adrfam = SPDK_NVMF_ADRFAM_FC;
                                  /* [한국어] Fibre Channel WWN — FC 트랜스포트 */
	} else {
		return -ENOENT;
                                  /* [한국어] 미지원 문자열 — parse_trtype과 다르게 CUSTOM 같은 fall-through 없음 */
	}
	return 0;
}

/*
 * [한국어]
 * spdk_nvme_transport_id_adrfam_str - adrfam enum → 문자열 (사용자 노출 API)
 */
const char *
spdk_nvme_transport_id_adrfam_str(enum spdk_nvmf_adrfam adrfam)
{
	switch (adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		return "IPv4";
	case SPDK_NVMF_ADRFAM_IPV6:
		return "IPv6";
	case SPDK_NVMF_ADRFAM_IB:
		return "IB";
	case SPDK_NVMF_ADRFAM_FC:
		return "FC";
	default:
		return NULL;
	}
}

/*
 * [한국어]
 * parse_next_key - "key:value" 또는 "key=value" 단일 항목 파서 (transport_id_parse 헬퍼)
 *
 * @str:          [in/out] 입력 문자열 포인터의 포인터 — 파싱 후 다음 항목 위치로 전진
 * @key:          [out] 키 버퍼
 * @val:          [out] 값 버퍼
 * @key_buf_size: 키 버퍼 크기
 * @val_buf_size: 값 버퍼 크기
 * @return        값 길이 (>0) 또는 0(에러).
 *
 * 입력 형식: "trtype:PCIe traddr:0000:01:00.0 ..." (공백/탭/줄바꿈 구분)
 *           구분자는 ':' 또는 '=' 둘 다 허용 — 둘 다 있으면 먼저 등장하는 것 우선.
 *
 * 동작:
 *   [1] leading whitespace 스킵.
 *   [2] ':' 위치 찾기, 없으면 '=' 위치. 둘 다 없으면 에러.
 *   [3] ':'와 '='이 모두 있고 '='이 먼저면 '='을 구분자로.
 *   [4] 키 길이 검증 + 복사 + NUL 종결.
 *   [5] 구분자 다음부터 값 — whitespace까지의 길이로 추출 + 검증 + 복사 + NUL 종결.
 *   [6] str을 값 끝으로 전진.
 */
static size_t
parse_next_key(const char **str, char *key, char *val, size_t key_buf_size, size_t val_buf_size)
{

	const char *sep, *sep1;
	const char *whitespace = " \t\n";
                                  /* [한국어] 토큰 구분자 — 공백/탭/개행 */
	size_t key_len, val_len;

	*str += strspn(*str, whitespace);
                                  /* [한국어] leading whitespace 스킵 — 다음 키 시작 위치로 전진 */

	sep = strchr(*str, ':');
	if (!sep) {
		sep = strchr(*str, '=');
                                  /* [한국어] ':' 없으면 '='로 fallback */
		if (!sep) {
			SPDK_ERRLOG("Key without ':' or '=' separator\n");
			return 0;
                                  /* [한국어] 둘 다 없음 — 형식 위반 */
		}
	} else {
		sep1 = strchr(*str, '=');
		if ((sep1 != NULL) && (sep1 < sep)) {
			sep = sep1;
                                  /* [한국어] ':'와 '=' 모두 있고 '='이 더 앞이면 '=' 사용 (먼저 등장 우선) */
		}
	}

	key_len = sep - *str;
                                  /* [한국어] 키 길이 = 시작 위치부터 구분자까지의 거리 */
	if (key_len >= key_buf_size) {
                                  /* [한국어] 버퍼 초과 — NUL 종결 자리 없음 */
		SPDK_ERRLOG("Key length %zu greater than maximum allowed %zu\n",
			    key_len, key_buf_size - 1);
		return 0;
	}

	memcpy(key, *str, key_len);
	key[key_len] = '\0';
                                  /* [한국어] 키 추출 + NUL 종결 */

	*str += key_len + 1; /* Skip key: */
                                  /* [한국어] 키 + 구분자 1바이트 스킵 — 값 시작 위치로 */
	val_len = strcspn(*str, whitespace);
                                  /* [한국어] 값 길이 = 다음 whitespace까지의 거리 */
	if (val_len == 0) {
                                  /* [한국어] 빈 값 — 형식 위반 */
		SPDK_ERRLOG("Key without value\n");
		return 0;
	}

	if (val_len >= val_buf_size) {
                                  /* [한국어] 버퍼 초과 */
		SPDK_ERRLOG("Value length %zu greater than maximum allowed %zu\n",
			    val_len, val_buf_size - 1);
		return 0;
	}

	memcpy(val, *str, val_len);
	val[val_len] = '\0';
                                  /* [한국어] 값 추출 + NUL 종결 */

	*str += val_len;
                                  /* [한국어] 값 끝으로 전진 — 다음 호출 시 leading whitespace 스킵부터 시작 */

	return val_len;
}

/*
 * [한국어]
 * spdk_nvme_transport_id_parse - 전체 transport ID 문자열 파싱 (사용자 노출 API)
 *
 * @trid: [out] 결과 transport ID
 * @str:  "trtype:RDMA adrfam:IPv4 traddr:192.168.0.1 trsvcid:4420 subnqn:nqn..." 형태
 * @return 0 성공, -EINVAL.
 *
 * 사용처: RPC, CLI, 사용자 설정 파일에서 trid 문자열 파싱.
 *
 * 인식 키:
 *   trtype, adrfam, traddr, trsvcid, priority, subnqn — trid 필드에 저장
 *   hostaddr, hostsvcid, hostnqn, ns, alt_traddr — 무시 (다른 객체용 또는 application-level)
 *   알 수 없는 키 — 에러 로그만 출력하고 계속 (fail-fast 안 함)
 */
int
spdk_nvme_transport_id_parse(struct spdk_nvme_transport_id *trid, const char *str)
{
	size_t val_len;
	char key[32];
	char val[1024];
                                  /* [한국어] subnqn 등 긴 값을 위해 1KiB 버퍼 — 스택에 큰 편이지만 1회 호출이므로 OK */

	if (trid == NULL || str == NULL) {
		return -EINVAL;
	}

	while (*str != '\0') {
                                  /* [한국어] 입력 끝까지 반복 — parse_next_key가 *str을 전진 */

		val_len = parse_next_key(&str, key, val, sizeof(key), sizeof(val));

		if (val_len == 0) {
			SPDK_ERRLOG("Failed to parse transport ID\n");
			return -EINVAL;
                                  /* [한국어] 형식 위반 — fail-fast */
		}

		if (strcasecmp(key, "trtype") == 0) {
			if (spdk_nvme_transport_id_populate_trstring(trid, val) != 0) {
				SPDK_ERRLOG("invalid transport '%s'\n", val);
				return -EINVAL;
			}
			if (spdk_nvme_transport_id_parse_trtype(&trid->trtype, val) != 0) {
				SPDK_ERRLOG("Unknown trtype '%s'\n", val);
				return -EINVAL;
			}
		} else if (strcasecmp(key, "adrfam") == 0) {
			if (spdk_nvme_transport_id_parse_adrfam(&trid->adrfam, val) != 0) {
				SPDK_ERRLOG("Unknown adrfam '%s'\n", val);
				return -EINVAL;
			}
		} else if (strcasecmp(key, "traddr") == 0) {
			if (val_len > SPDK_NVMF_TRADDR_MAX_LEN) {
				SPDK_ERRLOG("traddr length %zu greater than maximum allowed %u\n",
					    val_len, SPDK_NVMF_TRADDR_MAX_LEN);
				return -EINVAL;
			}
			memcpy(trid->traddr, val, val_len + 1);
		} else if (strcasecmp(key, "trsvcid") == 0) {
			if (val_len > SPDK_NVMF_TRSVCID_MAX_LEN) {
				SPDK_ERRLOG("trsvcid length %zu greater than maximum allowed %u\n",
					    val_len, SPDK_NVMF_TRSVCID_MAX_LEN);
				return -EINVAL;
			}
			memcpy(trid->trsvcid, val, val_len + 1);
		} else if (strcasecmp(key, "priority") == 0) {
			if (val_len > SPDK_NVMF_PRIORITY_MAX_LEN) {
				SPDK_ERRLOG("priority length %zu greater than maximum allowed %u\n",
					    val_len, SPDK_NVMF_PRIORITY_MAX_LEN);
				return -EINVAL;
			}
			trid->priority = spdk_strtol(val, 10);
		} else if (strcasecmp(key, "subnqn") == 0) {
			if (val_len > SPDK_NVMF_NQN_MAX_LEN) {
				SPDK_ERRLOG("subnqn length %zu greater than maximum allowed %u\n",
					    val_len, SPDK_NVMF_NQN_MAX_LEN);
				return -EINVAL;
			}
			memcpy(trid->subnqn, val, val_len + 1);
		} else if (strcasecmp(key, "hostaddr") == 0) {
			continue;
		} else if (strcasecmp(key, "hostsvcid") == 0) {
			continue;
		} else if (strcasecmp(key, "hostnqn") == 0) {
			continue;
		} else if (strcasecmp(key, "ns") == 0) {
			/*
			 * Special case.  The namespace id parameter may
			 * optionally be passed in the transport id string
			 * for an SPDK application (e.g. spdk_nvme_perf)
			 * and additionally parsed therein to limit
			 * targeting a specific namespace.  For this
			 * scenario, just silently ignore this key
			 * rather than letting it default to logging
			 * it as an invalid key.
			 */
			continue;
		} else if (strcasecmp(key, "alt_traddr") == 0) {
			/*
			 * Used by applications for enabling transport ID failover.
			 * Please see the case above for more information on custom parameters.
			 */
			continue;
		} else {
			SPDK_ERRLOG("Unknown transport ID key '%s'\n", key);
		}
	}

	return 0;
}

/*
 * [한국어]
 * spdk_nvme_host_id_parse - host ID 문자열 파싱 (사용자 노출 API)
 *
 * @hostid: [out] 결과 host ID — hostaddr/hostsvcid 만 채움
 * @str:    transport_id_parse와 같은 문자열 (공유 — 사용자가 한 문자열에 둘 다 섞어 넣을 수 있음)
 * @return  0 성공, -EINVAL.
 *
 * 동작: 같은 문자열을 두 번 파싱 — transport_id_parse는 trid 필드만, 이 함수는 host 필드만.
 *       각자 무시할 키는 continue. 모르는 키는 에러 로그만.
 */
int
spdk_nvme_host_id_parse(struct spdk_nvme_host_id *hostid, const char *str)
{

	size_t key_size = 32;
	size_t val_size = 1024;
	size_t val_len;
	char key[key_size];
	char val[val_size];
                                  /* [한국어] VLA 사용 — 컴파일러 GCC 확장. C99에서는 표준이지만 일부 컴파일러에서는 거부. */

	if (hostid == NULL || str == NULL) {
		return -EINVAL;
	}

	while (*str != '\0') {

		val_len = parse_next_key(&str, key, val, key_size, val_size);

		if (val_len == 0) {
			SPDK_ERRLOG("Failed to parse host ID\n");
			return val_len;
		}

		/* Ignore the rest of the options from the transport ID. */
		if (strcasecmp(key, "trtype") == 0) {
			continue;
		} else if (strcasecmp(key, "adrfam") == 0) {
			continue;
		} else if (strcasecmp(key, "traddr") == 0) {
			continue;
		} else if (strcasecmp(key, "trsvcid") == 0) {
			continue;
		} else if (strcasecmp(key, "subnqn") == 0) {
			continue;
		} else if (strcasecmp(key, "priority") == 0) {
			continue;
		} else if (strcasecmp(key, "ns") == 0) {
			continue;
		} else if (strcasecmp(key, "hostaddr") == 0) {
			if (val_len > SPDK_NVMF_TRADDR_MAX_LEN) {
				SPDK_ERRLOG("hostaddr length %zu greater than maximum allowed %u\n",
					    val_len, SPDK_NVMF_TRADDR_MAX_LEN);
				return -EINVAL;
			}
			memcpy(hostid->hostaddr, val, val_len + 1);

		} else if (strcasecmp(key, "hostsvcid") == 0) {
			if (val_len > SPDK_NVMF_TRSVCID_MAX_LEN) {
				SPDK_ERRLOG("trsvcid length %zu greater than maximum allowed %u\n",
					    val_len, SPDK_NVMF_TRSVCID_MAX_LEN);
				return -EINVAL;
			}
			memcpy(hostid->hostsvcid, val, val_len + 1);
		} else {
			SPDK_ERRLOG("Unknown transport ID key '%s'\n", key);
		}
	}

	return 0;
}

/*
 * [한국어]
 * cmp_int - int 두 값의 단순 비교 (qsort/cmp 시그니처 호환)
 *
 * 주의: a - b는 overflow 가능 (int 범위에서 매우 큰 양수 - 음수). 이 파일에서는 작은 enum
 * 비교에만 사용되므로 문제 없음.
 */
static int
cmp_int(int a, int b)
{
	return a - b;
                                  /* [한국어] 표준 cmp 시맨틱 — 음수/0/양수로 대소 표현 */
}

/*
 * [한국어]
 * spdk_nvme_transport_id_compare - 두 trid 동등성 비교 (사용자 노출 API)
 *
 * @return 0 동등, 음수 trid1<trid2, 양수 trid1>trid2.
 *
 * 비교 우선순위:
 *   [1] trtype (CUSTOM은 trstring 비교, 그 외는 enum 정수 비교)
 *   [2] PCIe면 traddr만 비교 (정규화된 PCI BDF 주소) — 다른 필드 무시
 *   [3] Fabrics면: traddr, adrfam, trsvcid, subnqn 순차 비교 (모두 일치해야 동등)
 *
 * subnqn은 case-sensitive (NVMe-oF NQN은 정확 일치 요구), 나머지는 case-insensitive.
 */
int
spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	int cmp;

	if (trid1->trtype == SPDK_NVME_TRANSPORT_CUSTOM) {
		cmp = strcasecmp(trid1->trstring, trid2->trstring);
                                  /* [한국어] CUSTOM은 enum 값이 의미 없음 — trstring으로 비교 */
	} else {
		cmp = cmp_int(trid1->trtype, trid2->trtype);
                                  /* [한국어] 표준 트랜스포트는 enum 정수 비교 */
	}

	if (cmp) {
		return cmp;
                                  /* [한국어] trtype 다르면 즉시 결과 반환 */
	}

	if (trid1->trtype == SPDK_NVME_TRANSPORT_PCIE) {
                                  /* [한국어] PCIe 분기 — traddr만 비교 (다른 필드는 PCIe에서 의미 없음) */
		struct spdk_pci_addr pci_addr1 = {};
		struct spdk_pci_addr pci_addr2 = {};

		/* Normalize PCI addresses before comparing */
		if (spdk_pci_addr_parse(&pci_addr1, trid1->traddr) < 0 ||
		    spdk_pci_addr_parse(&pci_addr2, trid2->traddr) < 0) {
                                  /* [한국어] BDF 파싱 실패 — 비교 불가능, 임의 음수 반환 */
			return -1;
		}

		/* PCIe transport ID only uses trtype and traddr */
		return spdk_pci_addr_compare(&pci_addr1, &pci_addr2);
                                  /* [한국어] 정규화된 BDF 비교 — 형식 차이("0000:01:00.0" vs "01:00.0") 흡수 */
	}

	cmp = strcasecmp(trid1->traddr, trid2->traddr);
	if (cmp) {
		return cmp;
                                  /* [한국어] Fabrics: 주소(IP/IB/FC) 우선 비교 */
	}

	cmp = cmp_int(trid1->adrfam, trid2->adrfam);
	if (cmp) {
		return cmp;
                                  /* [한국어] adrfam 비교 (IPv4 vs IPv6 구분) */
	}

	cmp = strcasecmp(trid1->trsvcid, trid2->trsvcid);
	if (cmp) {
		return cmp;
                                  /* [한국어] 서비스 ID(포트 번호) 비교 */
	}

	cmp = strcmp(trid1->subnqn, trid2->subnqn);
	if (cmp) {
		return cmp;
                                  /* [한국어] subnqn 비교 — case-sensitive (NVMe-oF 스펙 요구) */
	}

	return 0;
                                  /* [한국어] 모든 필드 일치 — 동등 */
}

/*
 * [한국어]
 * spdk_nvme_prchk_flags_parse - PI(Protection Information) check 플래그 문자열 파싱 (사용자 노출 API)
 *
 * @prchk_flags: [out] 결과 플래그 (SPDK_NVME_IO_FLAGS_PRCHK_* OR)
 * @str:         "prchk:reftag|guard" 또는 "prchk:reftag", "prchk:guard"
 * @return       0 성공, -EINVAL.
 *
 * NVMe T10 DIF/DIX 보호 정보 체크 옵션:
 *   reftag: Reference Tag(LBA 기반) 검증
 *   guard:  Guard(CRC) 검증
 */
int
spdk_nvme_prchk_flags_parse(uint32_t *prchk_flags, const char *str)
{
	size_t val_len;
	char key[32];
	char val[1024];

	if (prchk_flags == NULL || str == NULL) {
		return -EINVAL;
	}

	while (*str != '\0') {
		val_len = parse_next_key(&str, key, val, sizeof(key), sizeof(val));

		if (val_len == 0) {
			SPDK_ERRLOG("Failed to parse prchk\n");
			return -EINVAL;
		}

		if (strcasecmp(key, "prchk") == 0) {
                                  /* [한국어] "prchk" 키 발견 — 값에서 reftag/guard substring 검색 */
			if (strcasestr(val, "reftag") != NULL) {
				*prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;
			}
			if (strcasestr(val, "guard") != NULL) {
				*prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_GUARD;
			}
                                  /* [한국어] 두 substring 모두 검사 — "reftag|guard" 형식 지원 */
		} else {
			SPDK_ERRLOG("Unknown key '%s'\n", key);
			return -EINVAL;
                                  /* [한국어] prchk 외 키는 fail-fast (transport_id_parse는 무시했지만 여기는 엄격) */
		}
	}

	return 0;
}

/*
 * [한국어]
 * spdk_nvme_prchk_flags_str - 플래그 → 사람용 문자열 (사용자 노출 API)
 *
 * 4가지 조합 모두 처리: NULL / "prchk:guard" / "prchk:reftag" / "prchk:reftag|guard".
 */
const char *
spdk_nvme_prchk_flags_str(uint32_t prchk_flags)
{
	if (prchk_flags & SPDK_NVME_IO_FLAGS_PRCHK_REFTAG) {
		if (prchk_flags & SPDK_NVME_IO_FLAGS_PRCHK_GUARD) {
			return "prchk:reftag|guard";
                                  /* [한국어] 둘 다 켜짐 */
		} else {
			return "prchk:reftag";
		}
	} else {
		if (prchk_flags & SPDK_NVME_IO_FLAGS_PRCHK_GUARD) {
			return "prchk:guard";
		} else {
			return NULL;
                                  /* [한국어] 둘 다 꺼짐 — NULL이 "no PI check" 의미 */
		}
	}
}

/*
 * [한국어]
 * spdk_nvme_scan_attached - 이미 attach된 ctrlr들에 대해 트랜스포트별 추가 scan (사용자 노출 API)
 *
 * @trid: scan 대상 trtype (PCIe면 PCIe 트랜스포트의 attached scan)
 * @return 0 성공, 음수 errno.
 *
 * 사용처: spdk_nvme_probe와 다르게 — 이미 알고 있는 ctrlr들의 NS 변경 등 incremental update.
 *         내부적으로 빈 probe_ctx 만들어 트랜스포트별 ctrlr_scan_attached 호출 후 정리.
 *
 * 호출 빈도: PCIe는 거의 사용 안 함, 일부 트랜스포트(VFIO-USER 등)에서 NS hot-add 감지용.
 */
int
spdk_nvme_scan_attached(const struct spdk_nvme_transport_id *trid)
{
	int rc;
	struct spdk_nvme_probe_ctx *probe_ctx;

	rc = nvme_driver_init();
	if (rc != 0) {
		return rc;
                                  /* [한국어] driver init 실패 (nvme_driver_init 멱등) */
	}

	probe_ctx = calloc(1, sizeof(*probe_ctx));
                                  /* [한국어] 임시 probe_ctx — 트랜스포트 scan에 trid 전달용 */
	if (!probe_ctx) {
		return -ENOMEM;
	}

	nvme_probe_ctx_init(probe_ctx, trid, NULL, NULL, NULL, NULL, NULL, NULL);
                                  /* [한국어] 모든 콜백 NULL — scan만 하고 attach 안 함 */

	nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
	rc = nvme_transport_ctrlr_scan_attached(probe_ctx);
                                  /* [한국어] 트랜스포트 vtable의 scan_attached 호출 */
	nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
	free(probe_ctx);
                                  /* [한국어] probe_ctx는 임시 — 즉시 free */

	return rc < 0 ? rc : 0;
                                  /* [한국어] 음수만 errno로, 양수는 0으로 정규화 (의미 없는 양수 반환 방지) */
}

/*
 * [한국어]
 * spdk_nvme_probe_async - 비동기 probe 시작 (legacy 4-arg 버전, attach_fail_cb 미지원)
 *
 * @return: probe_ctx — 사용자가 spdk_nvme_probe_poll_async 로 진행 폴링
 *
 * 단순 wrapper — attach_fail_cb=NULL 로 _ext 위임.
 */
struct spdk_nvme_probe_ctx *
spdk_nvme_probe_async(const struct spdk_nvme_transport_id *trid,
		      void *cb_ctx,
		      spdk_nvme_probe_cb probe_cb,
		      spdk_nvme_attach_cb attach_cb,
		      spdk_nvme_remove_cb remove_cb)
{
	/* [한국어] _ext 버전 위임 — attach_fail_cb=NULL → 내부 dummy 사용. */
	return spdk_nvme_probe_async_ext(trid, cb_ctx, probe_cb, attach_cb, NULL, remove_cb);
}

/*
 * [한국어]
 * spdk_nvme_probe_async_ext - 비동기 probe의 진짜 구현체 (probe_ctx 생성 + scan 시작)
 *
 * @return: probe_ctx 또는 NULL (실패)
 *
 * 동작:
 *   1) nvme_driver_init 멱등 호출
 *   2) probe_ctx 할당 (heap)
 *   3) probe_ctx_init: 사용자 콜백·옵션·trid 저장 + init/failed 리스트 빈 상태로 초기화
 *   4) nvme_probe_internal(probe_ctx, false) — direct_connect=false: enumerate 모드
 *      · transport scan 트리거
 *      · 발견된 각 device를 init_ctrlrs 리스트로
 *
 * 반환된 probe_ctx는 사용자가 spdk_nvme_probe_poll_async 로 폴링.
 */
struct spdk_nvme_probe_ctx *
spdk_nvme_probe_async_ext(const struct spdk_nvme_transport_id *trid,
			  void *cb_ctx,
			  spdk_nvme_probe_cb probe_cb,
			  spdk_nvme_attach_cb attach_cb,
			  spdk_nvme_attach_fail_cb attach_fail_cb,
			  spdk_nvme_remove_cb remove_cb)
{
	/* [한국어] return code. */
	int rc;
	/* [한국어] 비동기 컨텍스트 — heap 할당. */
	struct spdk_nvme_probe_ctx *probe_ctx;

	/* [한국어] driver 초기화 보장 (멱등). */
	rc = nvme_driver_init();
	if (rc != 0) {
		return NULL;
	}

	/* [한국어] probe_ctx heap 할당 + 0-init. */
	probe_ctx = calloc(1, sizeof(*probe_ctx));
	if (!probe_ctx) {
		return NULL;
	}

	/* [한국어] probe_ctx 필드 초기화 — trid/opts/cb 저장, init·failed 리스트 빈 상태. */
	nvme_probe_ctx_init(probe_ctx, trid, NULL, cb_ctx, probe_cb, attach_cb, attach_fail_cb,
			    remove_cb);
	/* [한국어] 본격 probe — transport별 scan 트리거. direct_connect=false 면 enumerate 모드. */
	rc = nvme_probe_internal(probe_ctx, false);
	/* [한국어] scan 실패 — probe_ctx 정리하고 NULL 반환. */
	if (rc != 0) {
		free(probe_ctx);
		return NULL;
	}

	/* [한국어] caller에 probe_ctx 전달 — 이후 spdk_nvme_probe_poll_async로 진행 폴링. */
	return probe_ctx;
}

/*
 * [한국어]
 * spdk_nvme_probe_poll_async - probe_ctx 진행 한 단계 폴링 (사용자가 반복 호출)
 *
 * @return: 0 — 모든 ctrlr 처리 완료, probe_ctx 자동 free 됨
 *          -EAGAIN — 아직 진행 중 (더 폴링 필요)
 *
 * 이 함수가 비동기 probe API의 핵심 — 사용자가 자기 reactor loop에서 주기적으로 호출하여
 * controller bring-up 상태머신을 진행시킨다. 모든 ctrlr이 READY 또는 실패 처리될 때까지 -EAGAIN.
 *
 * 동작:
 *   1) secondary process + PCIe + lookup 완료 — 즉시 0 (probe_ctx free 후)
 *   2) init_ctrlrs 리스트의 모든 ctrlr에 대해 nvme_ctrlr_poll_internal 호출
 *      · 각 ctrlr의 process_init 상태머신 한 단계 진행
 *      · READY 도달하면 attached_ctrlrs로 이동 + attach_cb
 *      · 실패면 failed_ctxs로 이동 + destruct 시작
 *   3) failed_ctxs 폴링 — destruct 완료된 것 제거
 *   4) 양 리스트 비면 driver->initialized=true 마킹 + probe_ctx free → 0 반환
 */
int
spdk_nvme_probe_poll_async(struct spdk_nvme_probe_ctx *probe_ctx)
{
	/* [한국어] init_ctrlrs 리스트 순회 변수. */
	struct spdk_nvme_ctrlr *ctrlr, *ctrlr_tmp;
	/* [한국어] failed_ctxs 리스트 순회 변수. */
	struct nvme_ctrlr_detach_ctx *detach_ctx, *detach_ctx_tmp;
	/* [한국어] return code. */
	int rc;

	/* [한국어] secondary + PCIe → primary가 모든 작업 했음. secondary는 lookup만 했으므로 즉시 종료.
	 *  · probe_ctx free 후 0 반환. */
	if (!spdk_process_is_primary() && probe_ctx->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		free(probe_ctx);
		return 0;
	}

	/* [한국어] init 진행 중 ctrlr 각각에 대해 한 단계 폴링. _SAFE는 순회 중 리스트 변경(remove) 안전 보장. */
	TAILQ_FOREACH_SAFE(ctrlr, &probe_ctx->init_ctrlrs, tailq, ctrlr_tmp) {
		/* [한국어] process_init 상태머신 한 단계 + READY 도달 시 attached로 이동 처리. */
		nvme_ctrlr_poll_internal(ctrlr, probe_ctx);
	}

	/* poll failed controllers destruction */
	/* [한국어] init 실패한 ctrlr들의 destruct 진행 폴링. */
	TAILQ_FOREACH_SAFE(detach_ctx, &probe_ctx->failed_ctxs.head, link, detach_ctx_tmp) {
		/* [한국어] destruct 한 단계 진행. */
		rc = nvme_ctrlr_destruct_poll_async(detach_ctx->ctrlr, detach_ctx);
		/* [한국어] 진행 중 — 다음 detach_ctx로. */
		if (rc == -EAGAIN) {
			continue;
		}

		/* [한국어] 0 아니고 -EAGAIN 아니면 실제 실패 — 로그만 출력하고 계속. */
		if (rc != 0) {
			SPDK_ERRLOG("Failure while polling the controller destruction (rc = %d)\n", rc);
		}

		/* [한국어] 완료(성공/실패) — 리스트에서 제거 후 ctx 메모리 반환. */
		TAILQ_REMOVE(&probe_ctx->failed_ctxs.head, detach_ctx, link);
		free(detach_ctx);
	}

	if (TAILQ_EMPTY(&probe_ctx->init_ctrlrs) && TAILQ_EMPTY(&probe_ctx->failed_ctxs.head)) {
		nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
		g_spdk_nvme_driver->initialized = true;
		nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
		free(probe_ctx);
		return 0;
	}

	return -EAGAIN;
}

/*
 * [한국어]
 * spdk_nvme_connect_async - NVMe-oF 비동기 직접 연결 (사용자 노출 API)
 *
 * @trid:     단일 ctrlr trid (Fabrics면 IP:port:subnqn)
 * @opts:     ctrlr 옵션 (NULL 허용)
 * @attach_cb: attach 성공 시 호출 (NULL이면 spdk_nvme_connect의 polling으로만 검색)
 * @return    probe_ctx 또는 NULL.
 *
 * spdk_nvme_probe_async와의 차이:
 *   - direct_connect=true → 단일 trid에 직접 연결, enumerate 안 함
 *   - probe_cb는 opts 있으면 connect_probe_cb로 자동 등록 (사용자 opts 강제 적용)
 */
struct spdk_nvme_probe_ctx *
spdk_nvme_connect_async(const struct spdk_nvme_transport_id *trid,
			const struct spdk_nvme_ctrlr_opts *opts,
			spdk_nvme_attach_cb attach_cb)
{
	int rc;
	spdk_nvme_probe_cb probe_cb = NULL;
	struct spdk_nvme_probe_ctx *probe_ctx;

	rc = nvme_driver_init();
	if (rc != 0) {
		return NULL;
	}

	probe_ctx = calloc(1, sizeof(*probe_ctx));
	if (!probe_ctx) {
		return NULL;
	}

	if (opts) {
		probe_cb = nvme_connect_probe_cb;
                                  /* [한국어] opts 있으면 모든 발견 ctrlr에 강제 적용하는 cb 등록 */
	}

	nvme_probe_ctx_init(probe_ctx, trid, opts, (void *)opts, probe_cb, attach_cb, NULL, NULL);
                                  /* [한국어] cb_ctx에 opts 포인터를 그대로 전달 — connect_probe_cb가 회수해 사용 */
	rc = nvme_probe_internal(probe_ctx, true);
                                  /* [한국어] direct_connect=true — 트랜스포트 ctrlr_scan에 단일 trid 모드 시그널 */
	if (rc != 0) {
		free(probe_ctx);
		return NULL;
	}

	return probe_ctx;
}

/*
 * [한국어]
 * nvme_parse_addr - 호스트명/IP + 서비스명/포트 → sockaddr_storage 변환 (내부 헬퍼)
 *
 * @sa:      [out] 채워질 socket address
 * @family:  AF_INET / AF_INET6 / AF_UNSPEC
 * @addr:    호스트명 또는 IP 문자열
 * @service: 서비스명 또는 포트 번호 문자열 (NULL 허용)
 * @port:    [out] 포트 번호 (service가 NULL 아니면)
 * @return   0 성공, 음수 errno (-EINVAL 또는 -|gai 코드|).
 *
 * 사용처: nvme_tcp.c / nvme_rdma.c가 trid의 traddr/trsvcid를 sockaddr로 변환할 때.
 *
 * 동작:
 *   getaddrinfo 호출 → 첫 결과의 ai_addr를 sa에 복사.
 *   service 문자열은 spdk_strtol로 1~65535 범위 검증 후 *port에 저장.
 */
int
nvme_parse_addr(struct sockaddr_storage *sa, int family, const char *addr, const char *service,
		long int *port)
{
	struct addrinfo *res;
	struct addrinfo hints;
                                  /* [한국어] getaddrinfo 호출 인자 — family/socktype 제약 */
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
                                  /* [한국어] AF_INET/AF_INET6/AF_UNSPEC 중 하나 */
	hints.ai_socktype = SOCK_STREAM;
                                  /* [한국어] TCP — RDMA도 connection-oriented이므로 SOCK_STREAM */
	hints.ai_protocol = 0;
                                  /* [한국어] 자동 선택 — TCP는 IPPROTO_TCP */

	if (service != NULL) {
		*port = spdk_strtol(service, 10);
                                  /* [한국어] 10진수 파싱 — port 번호 추출 */
		if (*port <= 0 || *port >= 65536) {
			SPDK_ERRLOG("Invalid port: %s\n", service);
			return -EINVAL;
                                  /* [한국어] 유효 포트 범위 1~65535 */
		}
	}

	ret = getaddrinfo(addr, service, &hints, &res);
                                  /* [한국어] DNS lookup + service 변환 — 결과 res는 linked list */
	if (ret) {
		SPDK_ERRLOG("getaddrinfo failed: %s (%d)\n", gai_strerror(ret), ret);
		return -(abs(ret));
                                  /* [한국어] gai 에러 코드를 음수 errno로 정규화 */
	}

	if (res->ai_addrlen > sizeof(*sa)) {
		SPDK_ERRLOG("getaddrinfo() ai_addrlen %zu too large\n", (size_t)res->ai_addrlen);
		ret = -EINVAL;
                                  /* [한국어] 비정상적으로 큰 주소 — sockaddr_storage 초과 */
	} else {
		memcpy(sa, res->ai_addr, res->ai_addrlen);
                                  /* [한국어] 첫 결과 사용 — multi-homed 등 대비 안 함 (대부분 첫 결과로 충분) */
	}

	freeaddrinfo(res);
                                  /* [한국어] getaddrinfo 결과 linked list 해제 — 위 if/else 어느 분기든 호출 */
	return ret;
}

/*
 * [한국어]
 * nvme_get_default_hostnqn - UUID 기반 기본 hostnqn 생성
 *
 * @buf: [out] hostnqn 버퍼
 * @len: buf 크기
 * @return 0 성공, -EINVAL (snprintf 실패 또는 버퍼 부족).
 *
 * 형식: "nqn.2014-08.org.nvmexpress:uuid:<uuid-lowercase>"
 *  - "nqn.2014-08.org.nvmexpress:uuid:" — NVMe-oF 표준 UUID-NQN 접두사 (RFC 4122 + NVMe-oF 1.0)
 *  - <uuid>: g_spdk_nvme_driver->default_extended_host_id (driver_init에서 RAND 생성된 UUID)
 *
 * 사용처: 사용자가 hostnqn을 명시하지 않으면 이 자동 생성 NQN 사용 — 같은 SPDK 프로세스의
 *         모든 연결이 같은 hostnqn 공유 (driver_init 한 번 + 같은 hugepage 영역).
 */
int
nvme_get_default_hostnqn(char *buf, int len)
{
	char uuid[SPDK_UUID_STRING_LEN];
                                  /* [한국어] UUID 표준 문자열 (8-4-4-4-12 hex) */
	int rc;

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &g_spdk_nvme_driver->default_extended_host_id);
                                  /* [한국어] UUID → 소문자 표준 표기 (RFC 4122 형식) */
	rc = snprintf(buf, len, "nqn.2014-08.org.nvmexpress:uuid:%s", uuid);
                                  /* [한국어] NVMe-oF UUID-NQN 표준 접두사 + UUID */
	if (rc < 0 || rc >= len) {
                                  /* [한국어] snprintf 에러(rc<0) 또는 버퍼 부족(rc>=len, NUL 잘림) */
		return -EINVAL;
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(nvme)
                                  /* [한국어] SPDK 로그 컴포넌트 "nvme" 등록 — SPDK_DEBUGLOG(nvme, ...) 등이 이 이름으로 분류.
                                   *  사용자가 spdk_log_set_print_level 등으로 개별 제어 가능. */
