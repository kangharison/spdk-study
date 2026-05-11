/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 *   Copyright (c) Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] RDMA Provider — 표준 libibverbs 백엔드 (rdma_provider_verbs.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK RDMA Provider 추상화의 "표준 verbs" 구현이다. 다른 한 쪽 구현
 * (rdma_provider_mlx5_dv.c — Mellanox Direct Verbs)과 동일한 외부 API를 제공하지만,
 * 내부적으로는 librdmacm의 rdma_create_qp/rdma_accept/rdma_disconnect와 일반적인
 * libibverbs 함수만 사용한다. Mellanox HCA가 아닌 환경(Soft-RoCE, iWARP, Broadcom RoCE,
 * Chelsio iWARP 등)에서 SPDK NVMe-oF가 동작할 수 있도록 해 주는 폴백/이식성 백엔드다.
 * QP 생성/소멸/연결/단절, send WR 큐잉/플러시(단일 ibv_post_send 호출로 도어벨 1회 모음)를
 * 담당한다. accel_sequence(데이터 변환·압축 등 NIC offload)는 지원하지 않는다고 명시한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 빌드 시 mlx5_dv 지원 여부에 따라 이 파일 또는 rdma_provider_mlx5_dv.c 중 하나만 링크된다.
 * 콜 체인:
 *   lib/nvmf/rdma.c (NVMe-oF target transport) / module/bdev/nvme RDMA initiator
 *     → spdk_rdma_provider_qp_create/_accept/_disconnect/_destroy/_queue_send_wrs/_flush_send_wrs
 *         (이 파일)
 *           → rdma_create_qp / rdma_accept / rdma_disconnect (librdmacm)
 *           → ibv_post_send (libibverbs) → 커널 rdma-core → HCA 드라이버 → NIC 도어벨
 * SRQ와 receive 큐잉/플러시는 lib/rdma_provider/common.c에서 공통으로 처리되므로 이 파일에는
 * 없다. 이 파일은 send 측과 QP 라이프사이클만 담당.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(caller): NVMe-oF target(lib/nvmf/rdma.c)과 NVMe initiator(lib/nvme/nvme_rdma.c).
 *   QP 한 개당 NVMe-oF connection 한 개에 대응.
 * - 하위(callee): librdmacm(rdma_create_qp/_accept/_disconnect), libibverbs(ibv_post_send 등).
 * - 메모리 도메인: spdk_rdma_utils_get_memory_domain() / _put_memory_domain()을 통해
 *   PD에 묶인 spdk_memory_domain 객체를 공유 — bdev/accel 레이어가 같은 PD를 인식하기 위함.
 * - 자료구조: struct spdk_rdma_provider_qp (include/spdk_internal/rdma_provider.h).
 *   verbs 백엔드는 추가 필드 없이 base struct만 사용 — 반면 mlx5_dv 백엔드는 wrapper struct를 둠.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_rdma_provider_qp_create()           : ibv_qp_init_attr 구성 → rdma_create_qp.
 * - spdk_rdma_provider_qp_accept()           : 서버 측 active accept (rdma_accept 래퍼).
 * - spdk_rdma_provider_qp_complete_connect() : verbs 백엔드는 추가 작업 불필요(no-op).
 * - spdk_rdma_provider_qp_destroy()          : QP/통계/메모리 도메인 자원 회수.
 * - spdk_rdma_provider_qp_disconnect()       : rdma_disconnect 호출, iWARP의 EINVAL 특수 케이스 처리.
 * - spdk_rdma_provider_qp_queue_send_wrs()   : send WR 체인을 누적 큐에 append.
 * - spdk_rdma_provider_qp_flush_send_wrs()   : 누적된 send WR을 ibv_post_send 한 번에 제출.
 * - spdk_rdma_provider_accel_sequence_supported() : 항상 false(verbs는 UMR/MKEY offload 미지원).
 */

#include <rdma/rdma_cma.h>
/* [한국어] librdmacm — rdma_cm_id, rdma_create_qp, rdma_accept, rdma_disconnect 등.
 * RDMA-CM은 IB/RoCE/iWARP 트랜스포트를 추상화한 connection manager이며 IP 기반 주소 해상도를 제공. */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 include — stdint.h, stdbool.h, errno.h 등 자주 쓰는 시스템 헤더 묶음. */
#include "spdk/string.h"
/* [한국어] spdk_strerror() — errno → 메시지 변환 헬퍼. */
#include "spdk/likely.h"
/* [한국어] spdk_unlikely() — 빈 send 큐 분기 등 핫패스 분기 예측 힌트. */

#include "spdk_internal/rdma_provider.h"
/* [한국어] provider 추상화 타입(spdk_rdma_provider_qp/_qp_init_attr 등). */
#include "spdk_internal/rdma_utils.h"
/* [한국어] memory domain 공유 헬퍼(spdk_rdma_utils_get_memory_domain) 정의. */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG/SPDK_WARNLOG. */

/*
 * [한국어]
 * spdk_rdma_provider_qp_create - QP(Queue Pair) 생성 (verbs 백엔드)
 *
 * @cm_id: librdmacm이 만든 rdma_cm_id — IP 주소 해상도/connection 협상이 끝난 상태.
 *         이 위에 QP를 attach해 connection 진행 가능.
 * @qp_attr: provider 입력 attr — pd, send/recv CQ, srq, capability(max_send_wr 등),
 *           qp_context, stats 공유 여부, domain_transfer(accel offload 콜백 — verbs는 미지원).
 * @return: 성공 시 spdk_rdma_provider_qp*; 실패 시 NULL.
 *
 * 동작:
 *  1) ibv_qp_init_attr 구성 — qp_type은 RC(Reliable Connection)로 고정(NVMe-oF는 RC만 사용).
 *  2) domain_transfer 옵션은 mlx5_dv만 지원 → 들어오면 에러.
 *  3) provider QP 핸들 alloc + stats 처리(공유 vs 자체 alloc).
 *  4) rdma_create_qp() — librdmacm이 내부적으로 ibv_create_qp_ex/ibv_create_qp를 호출하고
 *     결과 QP를 cm_id->qp에 연결. 이 시점에 HCA에 QP 컨텍스트가 만들어지고 INIT 상태가 됨.
 *  5) PD에 대응하는 spdk_memory_domain을 얻거나 새로 만들어 공유 — bdev/accel이 같은 도메인
 *     안의 메모리를 인식해 zero-copy 경로를 사용할 수 있게 함.
 *
 * 호출 컨텍스트: NVMe-oF connect 협상이 RDMA_CM_EVENT_CONNECT_REQUEST(서버) 또는
 * RDMA_CM_EVENT_ROUTE_RESOLVED(클라이언트) 시점에 호출. RDMA-CM event 처리 스레드.
 *
 * 호출 체인:
 *   nvmf_rdma_handle_cm_event() / nvme_rdma_route_resolved() → 이 함수
 *     → rdma_create_qp() → ibv_create_qp() → 커널 → HCA QP 생성
 */
struct spdk_rdma_provider_qp *
spdk_rdma_provider_qp_create(struct rdma_cm_id *cm_id,
			     struct spdk_rdma_provider_qp_init_attr *qp_attr)
{
	struct spdk_rdma_provider_qp *spdk_rdma_qp;
	/* [한국어] SPDK 측 QP 핸들 — 아래에서 alloc될 변수. */
	int rc;
	/* [한국어] rdma_create_qp 등의 반환 코드. */
	struct ibv_qp_init_attr attr = {
		.qp_context = qp_attr->qp_context,
		/* [한국어] qp_context: HCA가 CQE에 그대로 실어서 돌려주지는 않지만, libibverbs가
		 * QP 객체에 보관 — caller(NVMe-oF)는 QP→상위 객체 역참조에 사용. */
		.send_cq = qp_attr->send_cq,
		/* [한국어] send completion이 도착할 CQ — poller가 ibv_poll_cq로 이 CQ를 폴링. */
		.recv_cq = qp_attr->recv_cq,
		/* [한국어] receive completion이 도착할 CQ — send_cq와 같을 수도, 분리될 수도 있음. */
		.srq = qp_attr->srq,
		/* [한국어] SRQ를 사용하면 이 QP의 recv 큐는 SRQ 풀에서 끌어쓴다(NULL이면 per-QP RX 큐). */
		.cap = qp_attr->cap,
		/* [한국어] max_send_wr/max_recv_wr/max_send_sge/max_recv_sge/max_inline_data.
		 * NVMe-oF는 보통 큰 값(수백~수천)을 요청. HCA가 지원 가능한 값으로 clip할 수 있음. */
		.qp_type = IBV_QPT_RC
		/* [한국어] Reliable Connection — 1:1 연결, 순서 보장, 자동 재전송.
		 * NVMe-oF over RDMA 스펙(섹션 7)이 RC를 요구. UD/UC는 사용 안 함. */
	};

	if (qp_attr->domain_transfer) {
		/* [한국어] domain_transfer는 accel 시퀀스(데이터 변환을 NIC가 in-place 수행)를 위한
		 * 콜백. mlx5의 UMR/MKEY 기능이 필요하므로 표준 verbs 백엔드에서는 즉시 거부. */
		SPDK_ERRLOG("verbs provider doesn't support memory domain transfer functionality");
		return NULL;
	}

	spdk_rdma_qp = calloc(1, sizeof(*spdk_rdma_qp));
	/* [한국어] provider QP 핸들 zero-init alloc(send_wrs/recv_wrs 리스트 헤드 NULL 보장). */
	if (!spdk_rdma_qp) {
		SPDK_ERRLOG("qp memory allocation failed\n");
		return NULL;
	}

	if (qp_attr->stats) {
		/* [한국어] caller가 stats를 공유 객체로 제공한 경우 — 보통 poll group 단위 누적용. */
		spdk_rdma_qp->stats = qp_attr->stats;
		spdk_rdma_qp->shared_stats = true;
	} else {
		/* [한국어] 자체 stats alloc — destroy 시 free 책임이 우리에게 있음. */
		spdk_rdma_qp->stats = calloc(1, sizeof(*spdk_rdma_qp->stats));
		if (!spdk_rdma_qp->stats) {
			SPDK_ERRLOG("qp statistics memory allocation failed\n");
			free(spdk_rdma_qp);
			return NULL;
		}
	}

	rc = rdma_create_qp(cm_id, qp_attr->pd, &attr);
	/* [한국어] librdmacm 호출 — 내부적으로 ibv_create_qp_ex/ibv_create_qp를 부르고 결과를
	 * cm_id->qp에 저장. 이 시점에 HCA에 QP 컨텍스트가 만들어지고 상태는 INIT.
	 * cm_id가 RTU(Ready-To-Use) 단계로 가려면 이후 connect/accept 절차 필요. */
	if (rc) {
		SPDK_ERRLOG("Failed to create qp, rc %d, errno %s (%d)\n", rc, spdk_strerror(errno), errno);
		if (!spdk_rdma_qp->shared_stats) {
			/* [한국어] 자체 stats만 free — 공유 stats는 caller가 관리. */
			free(spdk_rdma_qp->stats);
		}
		free(spdk_rdma_qp);
		return NULL;
	}
	spdk_rdma_qp->qp = cm_id->qp;
	/* [한국어] librdmacm이 cm_id->qp에 채워준 ibv_qp*를 provider 핸들에도 저장 — 이후
	 * ibv_post_send/ibv_post_recv 호출 시 이 포인터를 사용. */
	spdk_rdma_qp->cm_id = cm_id;
	/* [한국어] 연결 단절(rdma_disconnect)에 cm_id가 필요해 같이 보관. */
	spdk_rdma_qp->domain = spdk_rdma_utils_get_memory_domain(qp_attr->pd);
	/* [한국어] 이 PD에 연결된 spdk_memory_domain을 ref-count로 가져옴(없으면 새로 만듦).
	 * memory domain은 SPDK가 "이 메모리는 RDMA HCA가 직접 DMA 가능한 영역"임을 추적하는
	 * 추상화. bdev/accel 레이어가 zero-copy 결정을 내릴 때 이 도메인을 비교해 같은 HCA
	 * 안에서는 추가 복사 없이 RDMA 전송이 가능하도록 한다. */
	if (!spdk_rdma_qp->domain) {
		/* [한국어] 도메인 alloc 실패 — 부분 정리 후 NULL 반환(qp_destroy가 cleanup 다 처리). */
		spdk_rdma_provider_qp_destroy(spdk_rdma_qp);
		return NULL;
	}

	qp_attr->cap = attr.cap;
	/* [한국어] HCA가 clip한 실제 cap 값을 caller에 돌려줌 — caller는 이 값으로 SQ/RQ
	 * 깊이를 인식하고 백프레셔 정책을 조정. */

	return spdk_rdma_qp;
}

/*
 * [한국어]
 * spdk_rdma_provider_qp_accept - 서버 측 RDMA 연결 수락 (verbs 백엔드)
 *
 * @spdk_rdma_qp: 이미 _qp_create된 provider QP. cm_id가 connect_request 상태여야 함.
 * @conn_param: 클라이언트에 돌려줄 connection 파라미터(private_data, initiator_depth 등).
 * @return: rdma_accept()의 반환값(0 성공). librdmacm이 응답 패킷을 송신하고 RTS로 진행.
 *
 * verbs 백엔드는 INIT→RTR→RTS 상태 전이를 librdmacm이 알아서 처리하므로 단순 래퍼다
 * (mlx5_dv 백엔드는 직접 ibv_modify_qp로 상태를 옮기는 점이 다르다).
 *
 * 호출 체인:
 *   nvmf_rdma_handle_connect_event() → 이 함수 → rdma_accept (librdmacm) → CM 응답 송신
 */
int
spdk_rdma_provider_qp_accept(struct spdk_rdma_provider_qp *spdk_rdma_qp,
			     struct rdma_conn_param *conn_param)
{
	assert(spdk_rdma_qp != NULL);
	/* [한국어] caller가 유효한 QP를 넘겼는지 확인. */
	assert(spdk_rdma_qp->cm_id != NULL);
	/* [한국어] cm_id가 살아 있어야 rdma_accept가 동작. */

	return rdma_accept(spdk_rdma_qp->cm_id, conn_param);
	/* [한국어] librdmacm이 CM 응답을 송신하고 QP를 INIT→RTR→RTS로 자동 진행시킨다.
	 * 성공 시 이후 RDMA_CM_EVENT_ESTABLISHED가 caller에게 도착. */
}

/*
 * [한국어]
 * spdk_rdma_provider_qp_complete_connect - 클라이언트 connect 후처리
 *
 * @spdk_rdma_qp: provider QP.
 * @return: 항상 0. verbs 백엔드는 librdmacm이 모든 상태 전이를 처리하므로 추가 작업 없음.
 *
 * mlx5_dv 백엔드는 여기서 ibv_modify_qp로 INIT→RTR→RTS를 직접 진행하므로 분기가 존재한다.
 *
 * 호출 컨텍스트: 클라이언트가 RDMA_CM_EVENT_ESTABLISHED 이벤트를 받은 직후.
 *
 * 호출 체인:
 *   nvme_rdma_qpair_connect_poll() → 이 함수 → (no-op)
 */
int
spdk_rdma_provider_qp_complete_connect(struct spdk_rdma_provider_qp *spdk_rdma_qp)
{
	/* Nothing to be done for Verbs */
	/* [한국어] verbs는 librdmacm 자체가 RTU까지 모두 끝내 주므로 여기서 할 일 없음. */
	return 0;
}

/*
 * [한국어]
 * spdk_rdma_provider_qp_destroy - QP와 관련 자원(통계, 메모리 도메인) 해제
 *
 * @spdk_rdma_qp: 해제 대상 provider QP — NULL 비허용(assert).
 *
 * 동작:
 *  1) send_wrs.first가 남았다면 경고(누적된 send WR이 flush 안 된 상태로 destroy).
 *  2) rdma_destroy_qp(cm_id) — librdmacm이 ibv_destroy_qp를 부르고 cm_id->qp를 NULL로 만듦.
 *  3) 자체 alloc한 stats free.
 *  4) 메모리 도메인 ref decrement(공유 카운트 0이면 실제 해제).
 *  5) provider 핸들 free.
 *
 * 호출 체인:
 *   nvmf_rdma_qpair_destroy() → 이 함수 → rdma_destroy_qp → ibv_destroy_qp → 커널/HCA
 */
void
spdk_rdma_provider_qp_destroy(struct spdk_rdma_provider_qp *spdk_rdma_qp)
{
	assert(spdk_rdma_qp != NULL);
	/* [한국어] NULL 핸들로 destroy 호출 금지(에러 흐름에서 의도치 않은 NULL 방지). */

	if (spdk_rdma_qp->send_wrs.first != NULL) {
		/* [한국어] flush 안 된 send WR이 큐에 남은 채로 destroy — 누락된 transmit이
		 * 발생할 수 있음을 경고. 정상 종료에서는 flush 후 destroy가 맞다. */
		SPDK_WARNLOG("Destroying qpair with queued Work Requests\n");
	}

	if (spdk_rdma_qp->qp) {
		/* [한국어] QP가 살아 있을 때만 destroy 호출 — 부분 실패 흐름에선 NULL일 수 있음. */
		rdma_destroy_qp(spdk_rdma_qp->cm_id);
		/* [한국어] librdmacm이 내부적으로 ibv_destroy_qp(cm_id->qp)를 호출하고 cm_id->qp=NULL.
		 * 이 시점에 HCA QP 컨텍스트가 회수됨. cm_id 자체는 여전히 유효. */
	}

	if (!spdk_rdma_qp->shared_stats) {
		/* [한국어] 공유 stats가 아니면 자체 free. */
		free(spdk_rdma_qp->stats);
	}
	if (spdk_rdma_qp->domain) {
		/* [한국어] PD 단위 memory domain ref decrement — 마지막 참조면 spdk_memory_domain_destroy가
		 * 내부 호출되어 글로벌 g_memory_domains 리스트에서 제거. */
		spdk_rdma_utils_put_memory_domain(spdk_rdma_qp->domain);
	}

	free(spdk_rdma_qp);
	/* [한국어] SPDK 측 핸들 메모리 회수. */
}

/*
 * [한국어]
 * spdk_rdma_provider_qp_disconnect - QP 연결 단절 (verbs 백엔드)
 *
 * @spdk_rdma_qp: 단절 대상 QP.
 * @return: 0 성공, 실패 시 errno 기반 코드. iWARP의 EINVAL은 정상으로 간주해 0 반환.
 *
 * 동작: rdma_disconnect()를 호출. IB/RoCE에서는 librdmacm이 disconnect 메시지를 전송하고
 * 정상 종료 절차를 진행하지만, iWARP의 경우 transport 자체가 다른 단절 모델을 쓰므로
 * 이미 ERR 상태인 QP에 대해 EINVAL을 반환하는 것이 정상 동작이다(SPDK는 이를 흡수).
 *
 * 호출 체인:
 *   nvmf_rdma_qpair_destroy() / connection teardown → 이 함수 → rdma_disconnect
 */
int
spdk_rdma_provider_qp_disconnect(struct spdk_rdma_provider_qp *spdk_rdma_qp)
{
	int rc = 0;

	assert(spdk_rdma_qp != NULL);

	if (spdk_rdma_qp->cm_id) {
		/* [한국어] cm_id 있으면 명시적 단절 시도 — peer에게 disconnect 신호 송신. */
		rc = rdma_disconnect(spdk_rdma_qp->cm_id);
		if (rc) {
			if (errno == EINVAL && spdk_rdma_qp->qp->context->device->transport_type == IBV_TRANSPORT_IWARP) {
				/* rdma_disconnect may return an error and set errno to EINVAL in case of iWARP.
				 * This behaviour is expected since iWARP handles disconnect event other than IB and
				 * qpair is already in error state when we call rdma_disconnect */
				/* [한국어] iWARP는 transport 종료 모델이 IB와 달라 disconnect가 EINVAL을 돌려준다.
				 * 이미 QP가 ERR 상태라 추가로 할 일 없음 — caller가 에러로 받아들이면 안 되므로 0으로 마스킹.
				 * device->transport_type은 ibv_query_device로 확인 가능한 transport 종류 enum. */
				return 0;
			}
			SPDK_ERRLOG("rdma_disconnect failed, errno %s (%d)\n", spdk_strerror(errno), errno);
		}
	}

	return rc;
}

/*
 * [한국어]
 * spdk_rdma_provider_qp_queue_send_wrs - send WR 체인을 송신 큐에 큐잉(verbs 백엔드)
 *
 * @spdk_rdma_qp: 대상 QP.
 * @first: 새로 만든 send WR 체인의 첫 노드.
 * @return: true = 빈 큐에 처음 넣음(caller가 flush queue 등록), false = 누적 append.
 *
 * common.c의 recv 버전과 동일한 도어벨 amortization 패턴이지만, send 측은 mlx5_dv 백엔드와
 * 구현이 갈라지므로(이쪽은 plain 링크드리스트, mlx5_dv는 ibv_wr_start/ibv_wr_send API 사용)
 * common.c에 들어가지 않고 백엔드별로 따로 구현된다.
 *
 * 실행 컨텍스트: 단일 spdk_thread(QP 소유 reactor) — 락 불필요.
 *
 * 호출 체인:
 *   nvmf_rdma_request_send_data() → 이 함수 → flush 시 ibv_post_send
 */
bool
spdk_rdma_provider_qp_queue_send_wrs(struct spdk_rdma_provider_qp *spdk_rdma_qp,
				     struct ibv_send_wr *first)
{
	struct ibv_send_wr *last;
	/* [한국어] 새 체인의 끝 노드 추적용. */

	assert(spdk_rdma_qp);
	assert(first);

	spdk_rdma_qp->stats->send.num_submitted_wrs++;
	/* [한국어] 첫 노드 카운트(통계). */
	last = first;
	while (last->next != NULL) {
		/* [한국어] 체인을 끝까지 따라가며 노드 수 누적. caller가 한 번에 묶어 전달 가능. */
		last = last->next;
		spdk_rdma_qp->stats->send.num_submitted_wrs++;
	}

	if (spdk_rdma_qp->send_wrs.first == NULL) {
		/* [한국어] 빈 큐에 처음 넣는 케이스 — caller가 flush queue에 등록해야 함을 true로 알림. */
		spdk_rdma_qp->send_wrs.first = first;
		spdk_rdma_qp->send_wrs.last = last;
		return true;
	} else {
		/* [한국어] 기존 큐 끝에 append — last->next로 연결하고 last 갱신. */
		spdk_rdma_qp->send_wrs.last->next = first;
		spdk_rdma_qp->send_wrs.last = last;
		return false;
	}
}

/*
 * [한국어]
 * spdk_rdma_provider_qp_flush_send_wrs - 누적된 send WR을 ibv_post_send 한 번에 제출
 *
 * @spdk_rdma_qp: 대상 QP.
 * @bad_wr: 출력 — ibv_post_send 실패 시 첫 실패 WR. caller는 여기부터 정리.
 * @return: ibv_post_send 반환값.
 *
 * 동작: 누적된 첫 노드부터 next 체인 전체를 단일 ibv_post_send 호출로 제출 → HCA 도어벨
 * MMIO write 1회. NVMe-oF의 RDMA_READ/RDMA_WRITE/SEND가 모두 이 경로로 나간다.
 *
 * 호출 컨텍스트: poll group의 polling cycle 끝(또는 명시적 flush 시점).
 *
 * 호출 체인:
 *   nvmf_rdma_qpair_process_pending() → 이 함수 → ibv_post_send → HCA 도어벨 → NIC가 SQ fetch
 */
int
spdk_rdma_provider_qp_flush_send_wrs(struct spdk_rdma_provider_qp *spdk_rdma_qp,
				     struct ibv_send_wr **bad_wr)
{
	int rc;

	assert(spdk_rdma_qp);
	assert(bad_wr);

	if (spdk_unlikely(!spdk_rdma_qp->send_wrs.first)) {
		/* [한국어] 큐가 비었으면 도어벨 칠 일 없음 — 빠른 경로. */
		return 0;
	}

	rc = ibv_post_send(spdk_rdma_qp->qp, spdk_rdma_qp->send_wrs.first, bad_wr);
	/* [한국어] verbs API: 누적된 send WR 체인을 SQ에 한 번에 post.
	 * HCA의 SQ 도어벨 레지스터에 MMIO write 1회 → NIC가 SQ에서 디스크립터를 fetch해
	 * RDMA_READ/RDMA_WRITE/SEND 트랜잭션을 IB/RoCE/iWARP 와이어 위에 발생시킴. */

	spdk_rdma_qp->send_wrs.first = NULL;
	/* [한국어] 큐 헤드 리셋 — 다음 cycle 준비. */
	spdk_rdma_qp->stats->send.doorbell_updates++;
	/* [한국어] send 도어벨 카운터 누적 — 배치 효율 모니터링. */

	return rc;
	/* [한국어] caller는 rc!=0이면 *bad_wr부터 정리(보통 connection drop). */
}

/*
 * [한국어]
 * spdk_rdma_provider_accel_sequence_supported - accel 시퀀스(NIC offload) 지원 여부
 *
 * @return: false. 표준 verbs 백엔드는 mlx5의 UMR/MKEY를 못 쓰므로 in-place 데이터 변환 불가.
 *
 * 호출 컨텍스트: SPDK accel framework가 transport별 NIC offload 가능 여부를 질의할 때.
 *
 * 호출 체인:
 *   spdk_accel_sequence_*() / nvmf_rdma_accel_check() → 이 함수 → false
 */
bool
spdk_rdma_provider_accel_sequence_supported(void)
{
	return false;
	/* [한국어] verbs는 NIC가 데이터 변환을 in-place로 할 능력이 없다고 보고 — caller는
	 * 변환 작업을 SW(예: DSA, SW crypto)로 우회한다. mlx5_dv 백엔드는 spdk_mlx5_umr_implementer_is_registered()로 동적 결정. */
}
