/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] RDMA Provider — Mellanox Direct Verbs(mlx5dv) 백엔드 (rdma_provider_mlx5_dv.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK RDMA Provider 추상화의 "Mellanox 전용 Direct Verbs(DV)" 구현이다.
 * rdma_provider_verbs.c와 동일한 spdk_rdma_provider_qp_*() API를 제공하지만, 내부적으로는
 * libmlx5dv가 노출하는 (1) mlx5dv_create_qp — UMR/MKEY 등 mlx5 고급 기능을 켤 수 있는
 * QP 생성, (2) ibv_qp_to_qp_ex() + ibv_wr_*() API — WR 빌드를 inline 빌드 모드로 진행해
 * 도어벨 직전까지 NIC 친화적 포맷으로 미리 채우는 fast-path를 사용한다. 또한 connect/accept 시
 * INIT→RTR→RTS의 QP state machine을 librdmacm에 맡기지 않고 직접 ibv_modify_qp로 옮긴다.
 * 이 백엔드는 spdk_mlx5_umr_implementer_is_registered()가 켜진 경우 accel 시퀀스(NIC offload)도
 * 지원한다고 보고한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 빌드 시 mlx5_dv 라이브러리가 있고 옵션이 켜진 경우 verbs 백엔드 대신 이 파일이 링크된다.
 * 콜 체인:
 *   lib/nvmf/rdma.c (NVMe-oF target) / module/bdev/nvme RDMA initiator
 *     → spdk_rdma_provider_qp_*() (이 파일)
 *         → mlx5dv_create_qp / ibv_qp_to_qp_ex / ibv_wr_start / ibv_wr_send / ibv_wr_rdma_*
 *         → ibv_wr_complete (= post + 도어벨) / ibv_modify_qp / rdma_establish / rdma_disconnect
 *           → libmlx5 → 커널 mlx5_ib → Mellanox HCA(ConnectX-5/6/7) 도어벨 MMIO
 * 인커밍 connection의 INIT/RTR/RTS 전이를 직접 수행하는 점에서 verbs 백엔드와 가장 큰 차이.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(caller): NVMe-oF target/initiator. 동일 API라 caller 코드는 백엔드를 신경 쓰지 않음.
 * - 하위(callee): libmlx5dv(mlx5dv_create_qp), libibverbs(ibv_qp_to_qp_ex/ibv_wr_*/ibv_modify_qp),
 *   librdmacm(rdma_init_qp_attr/rdma_establish/rdma_disconnect/rdma_accept).
 * - SPDK mlx5 헬퍼: spdk_internal/mlx5.h의 spdk_mlx5_umr_implementer_is_registered() —
 *   UMR(User Memory Region) 기능을 사용하는 데이터 변환 모듈이 등록되었는지 확인.
 * - memory domain: spdk_memory_domain_create(SPDK_DMA_DEVICE_TYPE_RDMA, ...)으로 PD-bound 도메인을
 *   생성하고 domain_transfer 콜백을 설정해 accel framework이 직접 호출 가능하게 함.
 * - 자료구조: struct spdk_rdma_mlx5_dv_qp(파일 내 정의) — base struct(spdk_rdma_provider_qp common)을
 *   감싸고 qpex/domain_ctx를 추가. 외부 API는 항상 &mlx5_qp->common을 반환하고 내부에서
 *   SPDK_CONTAINEROF로 래핑 struct로 복원.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_rdma_mlx5_dv_qp           : 공용 QP 핸들 + qpex(extended verbs 핸들) + domain_ctx.
 * - rdma_mlx5_dv_init_qpair() (static)    : INIT/RTR/RTS 3단계 ibv_modify_qp 직접 수행.
 * - spdk_rdma_provider_qp_create()        : mlx5dv_create_qp + qp_ex 추출 + memory_domain 등록.
 * - spdk_rdma_provider_qp_accept()        : 서버 accept — RTR/RTS 진입 후 rdma_accept.
 * - spdk_rdma_provider_qp_complete_connect(): 클라이언트 — RTR/RTS 진입 후 rdma_establish.
 * - spdk_rdma_provider_qp_destroy()       : qp + memory_domain + stats 자원 회수.
 * - spdk_rdma_provider_qp_disconnect()    : QP를 ERR로 옮기고 rdma_disconnect.
 * - spdk_rdma_provider_qp_queue_send_wrs(): ibv_wr_start + opcode별 ibv_wr_send/rdma_*로
 *                                           inline 빌드(NIC가 바로 소비할 포맷). flush까지 빌드만 유지.
 * - spdk_rdma_provider_qp_flush_send_wrs(): ibv_wr_complete으로 빌드 종료(=post+도어벨 1회).
 * - spdk_rdma_provider_accel_sequence_supported(): mlx5 UMR 등록 여부 동적 보고.
 */

#include <rdma/rdma_cma.h>
/* [한국어] librdmacm — connection manager API. INIT 단계에서 rdma_init_qp_attr로
 * QP attr를 채우고, complete_connect에서 rdma_establish로 ESTABLISHED 통보를 한다. */
#include <infiniband/mlx5dv.h>
/* [한국어] Mellanox Direct Verbs — mlx5dv_create_qp 등 mlx5 전용 확장 API. UMR/MKEY,
 * Atomic offload, raw packet 등 ConnectX 시리즈 고유 기능을 노출. */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 시스템 헤더 묶음. */
#include "spdk/string.h"
/* [한국어] spdk_strerror — errno → 메시지 변환. */
#include "spdk/likely.h"
/* [한국어] spdk_unlikely — 핫패스 분기 예측 힌트. */
#include "spdk/dma.h"
/* [한국어] spdk_memory_domain API — PD-bound DMA 영역 추적/조회용 추상화. */

#include "spdk_internal/rdma_provider.h"
/* [한국어] provider 추상화 헤더 — base struct spdk_rdma_provider_qp 정의. */
#include "spdk_internal/mlx5.h"
/* [한국어] SPDK 자체 mlx5 헬퍼 — accel offload 지원 여부 질의(UMR implementer 등록 확인). */
#include "spdk/log.h"
/* [한국어] 로그 매크로. */
#include "spdk/util.h"
/* [한국어] SPDK_CONTAINEROF — base struct 포인터를 감싼 struct로 복원하는 매크로(=container_of). */

/*
 * [한국어]
 * struct spdk_rdma_mlx5_dv_qp - mlx5_dv 백엔드 전용 QP 래퍼 구조체
 *
 * 외부에는 &common(=spdk_rdma_provider_qp*)만 노출되며, 백엔드 내부 함수가 SPDK_CONTAINEROF
 * 매크로로 이 wrapper struct로 복원해 qpex/domain_ctx에 접근한다. base struct 첫 멤버
 * 패턴은 SPDK 전반에서 다형성(polymorphism) 구현에 흔히 쓰인다.
 */
struct spdk_rdma_mlx5_dv_qp {
	struct spdk_rdma_provider_qp common;
	/* [한국어] 외부에 노출되는 base struct(반드시 첫 멤버여야 SPDK_CONTAINEROF 변환이 무료).
	 * qp/cm_id/send_wrs/recv_wrs/stats/domain/shared_stats 등 공용 필드를 모두 담는다.
	 * 설정자: qp_create()의 mlx5dv_create_qp 결과로 채움.
	 * 읽는 자: 외부 API 진입 시 caller가 &common 포인터를 보유한 채 모든 호출을 한다.
	 * 동기화: 단일 spdk_thread 소유 — 별도 락 없음. */

	struct spdk_memory_domain_rdma_ctx domain_ctx;
	/* [한국어] memory_domain 등록 시 user_ctx로 전달할 컨텍스트(ibv_pd 포인터를 담음).
	 * 설정자: qp_create()에서 domain_ctx.size, domain_ctx.ibv_pd를 채움.
	 * 읽는 자: spdk_memory_domain framework이 도메인 콜백 호출 시 user_ctx로 되돌려줌.
	 * 값 범위: size=sizeof 자기 자신, ibv_pd=유효한 PD 포인터. NULL 불가.
	 * 동기화: 도메인 라이프타임 동안 변경되지 않으므로 락 불필요. */

	struct ibv_qp_ex *qpex;
	/* [한국어] extended verbs 핸들 — ibv_wr_start/_send/_rdma_read/_rdma_write/_complete API의 입력.
	 * verbs 1.0 시절의 ibv_post_send가 WR 배열을 받는 batch 인터페이스였다면, qp_ex API는
	 * ibv_wr_start로 빌드를 시작하고 각 WR 필드(wr_id/flags)를 qpex 멤버에 직접 쓰며, 마지막에
	 * ibv_wr_complete으로 도어벨까지 끝낸다 — Mellanox NIC의 BlueFlame inline post를 활용하기 위함.
	 * 설정자: qp_create()에서 ibv_qp_to_qp_ex()로 추출.
	 * 읽는 자: queue_send_wrs/flush_send_wrs의 inline 빌드 경로.
	 * 동기화: QP 소유 스레드 단일 접근 — 락 없음. */
};

/*
 * [한국어]
 * rdma_mlx5_dv_init_qpair - QP를 INIT → RTR → RTS로 명시적 상태 전이
 *
 * @mlx5_qp: wrapper QP 핸들. cm_id가 connect_request 또는 route_resolved 단계여야 함.
 * @return: 0 성공, <0 = 실패. 실패하면 caller가 connection 자체를 abort.
 *
 * 동작: 각 단계마다 rdma_init_qp_attr()로 librdmacm이 추천하는 attr/mask를 받고,
 * ibv_modify_qp로 실제 상태 전이를 수행. verbs 백엔드는 librdmacm이 알아서 해 주는 일을
 * 직접 한다 — 그 이유는 mlx5_dv가 mlx5dv_create_qp로 만든 QP에 대해 librdmacm의
 * 자동 상태 전이를 사용할 수 없기 때문(librdmacm은 표준 ibv_qp만 추적).
 *
 * QP 상태 전이 (NVMe-oF over RDMA에서):
 *   RESET → INIT(자원 등록) → RTR(Ready-To-Receive, 수신 가능) → RTS(Ready-To-Send, 송신 가능)
 *
 * 호출 컨텍스트: RDMA-CM event 처리 스레드(connect_request 또는 established 직전).
 *
 * 호출 체인:
 *   spdk_rdma_provider_qp_accept/_complete_connect → 이 함수 → ibv_modify_qp x3
 */
static int
rdma_mlx5_dv_init_qpair(struct spdk_rdma_mlx5_dv_qp *mlx5_qp)
{
	struct ibv_qp_attr qp_attr;
	/* [한국어] ibv_modify_qp의 입력 — qp_state 외에도 path_mtu, dest_qp_num, rq_psn,
	 * max_dest_rd_atomic 등 단계별로 librdmacm이 채워 줄 필드가 많다. */
	int qp_attr_mask, rc;
	/* [한국어] qp_attr_mask는 attr의 어떤 필드를 사용할지 나타내는 비트마스크 — 단계마다 달라짐. */

	qp_attr.qp_state = IBV_QPS_INIT;
	/* [한국어] 1단계: RESET → INIT. INIT에서는 PD/포트번호/access_flags가 결정된다. */
	rc = rdma_init_qp_attr(mlx5_qp->common.cm_id, &qp_attr, &qp_attr_mask);
	/* [한국어] librdmacm이 cm_id의 협상 결과를 바탕으로 attr/mask를 채워 줌. mlx5dv_create_qp
	 * 로 만든 QP에 대해서도 cm_id에 attach만 되어 있으면 이 헬퍼는 동작한다. */
	if (rc) {
		SPDK_ERRLOG("Failed to init attr IBV_QPS_INIT, errno %s (%d)\n", spdk_strerror(errno), errno);
		return rc;
	}

	rc = ibv_modify_qp(mlx5_qp->common.qp, &qp_attr, qp_attr_mask);
	/* [한국어] 실제 상태 전이 — HCA의 QP 컨텍스트가 INIT 상태로 전환됨. */
	if (rc) {
		SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_INIT) failed, rc %d\n", rc);
		return rc;
	}

	qp_attr.qp_state = IBV_QPS_RTR;
	/* [한국어] 2단계: INIT → RTR. RTR에서는 path_mtu, dest_qp_num, rq_psn(starting receive PSN),
	 * 그리고 ah_attr(Address Handle — peer GID/LID/SL)이 결정. 이 시점부터 RX 큐 사용 가능. */
	rc = rdma_init_qp_attr(mlx5_qp->common.cm_id, &qp_attr, &qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("Failed to init attr IBV_QPS_RTR, errno %s (%d)\n", spdk_strerror(errno), errno);
		return rc;
	}

	rc = ibv_modify_qp(mlx5_qp->common.qp, &qp_attr, qp_attr_mask);
	/* [한국어] HCA QP를 RTR로 올림 — 이 시점부터 incoming SEND/WRITE를 받을 수 있다. */
	if (rc) {
		SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_RTR) failed, rc %d\n", rc);
		return rc;
	}

	qp_attr.qp_state = IBV_QPS_RTS;
	/* [한국어] 3단계: RTR → RTS. RTS에서는 sq_psn(starting send PSN), timeout, retry_cnt 등이
	 * 결정되고 SQ 사용이 가능해진다. NVMe-oF는 이 단계에서 capsule 송수신 시작. */
	rc = rdma_init_qp_attr(mlx5_qp->common.cm_id, &qp_attr, &qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("Failed to init attr IBV_QPS_RTR, errno %s (%d)\n", spdk_strerror(errno), errno);
		/* [한국어] (원본 메시지 typo: 사실 RTS인데 RTR이라고 찍혀 있음 — SPDK 원본 그대로 유지) */
		return rc;
	}

	rc = ibv_modify_qp(mlx5_qp->common.qp, &qp_attr, qp_attr_mask);
	/* [한국어] RTS 진입 — 송신 가능 상태. 이후 ibv_wr_xxx/ibv_post_send가 정상 동작. */
	if (rc) {
		SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_RTS) failed, rc %d\n", rc);
	}

	return rc;
	/* [한국어] 마지막 modify의 rc를 그대로 caller에 전달. */
}

/*
 * [한국어]
 * spdk_rdma_provider_qp_create - QP 생성 (mlx5_dv 백엔드)
 *
 * @cm_id: librdmacm이 만든 cm_id(주소 해상도 완료 또는 connect_request 수신 직후 상태).
 * @qp_attr: provider 입력 attr(pd, send/recv CQ, srq, capability, qp_context, stats,
 *           domain_transfer 등).
 * @return: 성공 시 &spdk_rdma_mlx5_dv_qp::common 포인터; 실패 시 NULL.
 *
 * 동작:
 *  1) ibv_qp_init_attr_ex 구성 — pd가 명시되지 않았으면 cm_id->pd 사용. comp_mask로
 *     IBV_QP_INIT_ATTR_PD와 IBV_QP_INIT_ATTR_SEND_OPS_FLAGS를 켜 ibv_qp_to_qp_ex로
 *     extended QP 핸들을 얻을 수 있게 함.
 *  2) wrapper struct alloc + stats 처리.
 *  3) mlx5dv_create_qp() — Mellanox HCA에 mlx5 전용 QP를 만들고 BlueFlame 도어벨 매핑.
 *     verbs API 대신 mlx5dv를 쓰는 이유는 UMR/MKEY 등 mlx5 전용 기능을 활성화하기 위함.
 *  4) ibv_qp_to_qp_ex — extended verbs 핸들(qpex)을 추출. 이후 send WR 빌드 경로가
 *     ibv_wr_start/_send/_rdma_*/_complete API를 사용한다.
 *  5) spdk_memory_domain_create(SPDK_DMA_DEVICE_TYPE_RDMA, ..., SPDK_RDMA_DMA_DEVICE) —
 *     이 PD에 묶인 RDMA 메모리 도메인을 생성. accel/bdev이 이 도메인을 인식하면 zero-copy
 *     RDMA 전송 경로가 가능해진다.
 *  6) domain_transfer 콜백이 주어졌고 accel offload가 지원되면, 도메인에 데이터 전송 콜백을 설정.
 *
 * 호출 체인:
 *   nvmf_rdma_handle_cm_event() → 이 함수 → mlx5dv_create_qp → 커널 mlx5_ib → HCA QP 생성
 */
struct spdk_rdma_provider_qp *
spdk_rdma_provider_qp_create(struct rdma_cm_id *cm_id,
			     struct spdk_rdma_provider_qp_init_attr *qp_attr)
{
	assert(cm_id);
	/* [한국어] cm_id 없으면 connection 협상 자체가 안 됨. */
	assert(qp_attr);

	struct ibv_qp *qp;
	/* [한국어] mlx5dv_create_qp의 반환값을 받을 임시 포인터. */
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	/* [한국어] wrapper QP 핸들 — 아래에서 alloc. */
	struct ibv_qp_init_attr_ex dv_qp_attr = {
		.qp_context = qp_attr->qp_context,
		/* [한국어] caller가 QP에 매단 식별자(보통 NVMe-oF qpair 포인터). */
		.send_cq = qp_attr->send_cq,
		/* [한국어] send completion 도착할 CQ. */
		.recv_cq = qp_attr->recv_cq,
		/* [한국어] receive completion 도착할 CQ. */
		.srq = qp_attr->srq,
		/* [한국어] SRQ 사용 시 RX 풀이 SRQ 공유 풀로 라우팅됨(NULL=per-QP RX). */
		.cap = qp_attr->cap,
		/* [한국어] max_send/recv_wr/sge, max_inline_data — HCA가 clip 가능. */
		.qp_type = IBV_QPT_RC,
		/* [한국어] NVMe-oF 스펙이 요구하는 RC. */
		.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS,
		/* [한국어] PD 필드 사용 + SEND_OPS_FLAGS 사용. SEND_OPS_FLAGS가 켜져야 이후
		 * ibv_qp_to_qp_ex로 extended 핸들을 받을 수 있고 ibv_wr_* API가 동작. */
		.pd = qp_attr->pd ? qp_attr->pd : cm_id->pd
		/* [한국어] caller가 PD를 지정했으면 그것을, 안 했으면 cm_id가 들고 있는 PD 사용.
		 * NVMe-oF target은 보통 device 단위로 PD를 공유하므로 caller가 명시 전달. */
	};
	struct spdk_memory_domain_ctx ctx = {};
	/* [한국어] memory_domain 생성 시 전달할 컨텍스트 wrapper. */
	int rc;

	assert(dv_qp_attr.pd);
	/* [한국어] PD가 어디서든 와야 함 — caller 또는 cm_id에서. 둘 다 NULL이면 fatal. */

	mlx5_qp = calloc(1, sizeof(*mlx5_qp));
	/* [한국어] wrapper struct zero-init alloc. base/extension 필드 모두 0/NULL로 시작. */
	if (!mlx5_qp) {
		SPDK_ERRLOG("qp memory allocation failed\n");
		return NULL;
	}

	if (qp_attr->stats) {
		/* [한국어] 외부 공유 stats — destroy 시 free 안 함. */
		mlx5_qp->common.stats = qp_attr->stats;
		mlx5_qp->common.shared_stats = true;
	} else {
		/* [한국어] 자체 stats alloc — destroy에서 free. */
		mlx5_qp->common.stats = calloc(1, sizeof(*mlx5_qp->common.stats));
		if (!mlx5_qp->common.stats) {
			SPDK_ERRLOG("qp statistics memory allocation failed\n");
			free(mlx5_qp);
			return NULL;
		}
	}

	qp = mlx5dv_create_qp(cm_id->verbs, &dv_qp_attr, NULL);
	/* [한국어] Mellanox Direct Verbs로 QP 생성. cm_id->verbs는 ibv_context*. NULL은
	 * mlx5dv 전용 추가 attr 미사용을 의미(BlueFlame, raw packet 등 추가 옵션 OFF).
	 * 표준 ibv_create_qp_ex와 달리 mlx5dv_create_qp는 mlx5 고유 자원을 활성화할 수 있다. */

	if (!qp) {
		/* [한국어] HCA 자원 부족 또는 attr 거부 — 자체 alloc 자원만 회수하고 NULL 반환. */
		SPDK_ERRLOG("Failed to create qpair, errno %s (%d)\n", spdk_strerror(errno), errno);
		if (!mlx5_qp->common.shared_stats) {
			free(mlx5_qp->common.stats);
		}
		free(mlx5_qp);
		return NULL;
	}

	mlx5_qp->common.qp = qp;
	/* [한국어] base struct에도 ibv_qp* 저장 — 외부 코드가 동일 필드로 접근. */
	mlx5_qp->common.cm_id = cm_id;
	/* [한국어] 단절/상태 전이에 필요한 cm_id 보관. */
	mlx5_qp->qpex = ibv_qp_to_qp_ex(qp);
	/* [한국어] extended verbs 핸들 추출 — 이후 ibv_wr_start/ibv_wr_send/ibv_wr_rdma_*/ibv_wr_complete
	 * API의 첫 인자로 들어간다. SEND_OPS_FLAGS comp_mask가 켜져야 NULL이 아닌 값을 반환. */

	if (!mlx5_qp->qpex) {
		/* [한국어] HCA가 extended ops를 지원하지 않는 경우 — fatal. */
		spdk_rdma_provider_qp_destroy(&mlx5_qp->common);
		return NULL;
	}
	mlx5_qp->domain_ctx.size = sizeof(mlx5_qp->domain_ctx);
	/* [한국어] forward/backward compat을 위한 size 필드 — framework이 sizeof를 비교해
	 * 어떤 버전의 ctx인지 식별. */
	mlx5_qp->domain_ctx.ibv_pd = qp_attr->pd;
	/* [한국어] 도메인 콜백이 PD를 알아야 ibv_reg_mr 등을 호출할 수 있음. */
	ctx.size = sizeof(ctx);
	/* [한국어] 외부 wrapper도 size 명시. */
	ctx.user_ctx = &mlx5_qp->domain_ctx;
	/* [한국어] framework가 콜백 시 다시 돌려줄 user 데이터 포인터. */
	ctx.user_ctx_size = mlx5_qp->domain_ctx.size;
	/* [한국어] user_ctx의 크기. */
	rc = spdk_memory_domain_create(&mlx5_qp->common.domain, SPDK_DMA_DEVICE_TYPE_RDMA, &ctx,
				       SPDK_RDMA_DMA_DEVICE);
	/* [한국어] SPDK_DMA_DEVICE_TYPE_RDMA 도메인 생성 — accel/bdev이 이 도메인을 인식하면
	 * 페이로드를 RDMA HCA가 직접 DMA할 수 있다고 판단하고 zero-copy 경로를 선택한다.
	 * SPDK_RDMA_DMA_DEVICE는 도메인 식별자 문자열. */
	if (rc) {
		SPDK_ERRLOG("Failed to create memory domain\n");
		spdk_rdma_provider_qp_destroy(&mlx5_qp->common);
		return NULL;
	}
	if (qp_attr->domain_transfer) {
		/* [한국어] caller가 데이터 변환 콜백(예: NIC 측 압축/암호화)을 제공한 경우. */
		if (!spdk_rdma_provider_accel_sequence_supported()) {
			/* [한국어] 그러나 mlx5 UMR이 등록되지 않아 NIC가 in-place 변환을 못 함 — 거부. */
			SPDK_ERRLOG("Data transfer functionality is not supported\n");
			spdk_rdma_provider_qp_destroy(&mlx5_qp->common);
			return NULL;
		}
		spdk_memory_domain_set_data_transfer(mlx5_qp->common.domain, qp_attr->domain_transfer);
		/* [한국어] 도메인에 변환 콜백 설치 — accel framework이 이 콜백으로 NIC offload 변환
		 * 시퀀스를 빌드. */
	}

	qp_attr->cap = dv_qp_attr.cap;
	/* [한국어] HCA가 실제로 할당한 cap 값 회신 — caller가 그에 맞춰 자원 사이징. */

	return &mlx5_qp->common;
	/* [한국어] base struct 포인터를 반환 — caller는 백엔드 정체를 모른 채 동일 인터페이스 사용. */
}

/*
 * [한국어]
 * spdk_rdma_provider_qp_accept - 서버 측 RDMA 연결 수락 (mlx5_dv 백엔드)
 *
 * @spdk_rdma_qp: provider QP(=&mlx5_qp->common).
 * @conn_param: 클라이언트에 돌려줄 connection 파라미터.
 * @return: 0 성공, -1 실패(errno 세팅).
 *
 * verbs 백엔드와 달리 librdmacm이 INIT/RTR/RTS를 알아서 처리해 주지 않으므로, 여기서 먼저
 * rdma_mlx5_dv_init_qpair()로 명시적 상태 전이를 끝내고 마지막에 rdma_accept을 호출한다
 * (rdma_accept는 CM 응답 패킷을 송신해 클라이언트에 ESTABLISHED를 통보하는 역할만 함).
 *
 * 호출 체인:
 *   nvmf_rdma_handle_connect_event() → 이 함수
 *     → rdma_mlx5_dv_init_qpair → ibv_modify_qp x3
 *     → rdma_accept → CM 응답 송신
 */
int
spdk_rdma_provider_qp_accept(struct spdk_rdma_provider_qp *spdk_rdma_qp,
			     struct rdma_conn_param *conn_param)
{
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;

	assert(spdk_rdma_qp != NULL);
	assert(spdk_rdma_qp->cm_id != NULL);

	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);
	/* [한국어] base struct 포인터 → wrapper struct 포인터 복원. common이 첫 멤버라
	 * 오프셋이 0이지만, 매크로를 통해 future-proof하게 표현. */

	/* NVMEoF target must move qpair to RTS state */
	/* [한국어] verbs 백엔드와 다른 점: 여기선 RTS까지 직접 올린다(librdmacm 자동 진행 미사용). */
	if (rdma_mlx5_dv_init_qpair(mlx5_qp) != 0) {
		SPDK_ERRLOG("Failed to initialize qpair\n");
		/* Set errno to be compliant with rdma_accept behaviour */
		/* [한국어] 본래 rdma_accept가 실패 시 errno를 세팅하는 컨벤션. caller가 errno를 보고
		 * 분기하므로 ECONNABORTED를 명시 세팅해 일관성 유지. */
		errno = ECONNABORTED;
		return -1;
	}

	return rdma_accept(spdk_rdma_qp->cm_id, conn_param);
	/* [한국어] CM 응답 패킷 송신 → 클라이언트가 RDMA_CM_EVENT_ESTABLISHED를 수신. */
}

/*
 * [한국어]
 * spdk_rdma_provider_qp_complete_connect - 클라이언트 connect 후처리 (mlx5_dv 백엔드)
 *
 * @spdk_rdma_qp: provider QP.
 * @return: 0 성공, 실패 시 errno 기반 코드.
 *
 * verbs 백엔드는 no-op이지만 mlx5_dv는 다음 두 가지를 직접 수행:
 *  1) rdma_mlx5_dv_init_qpair() — INIT/RTR/RTS 상태 전이.
 *  2) rdma_establish() — 클라이언트 측에서 ESTABLISHED 통보(librdmacm).
 *
 * 호출 체인:
 *   nvme_rdma_qpair_connect_poll() → 이 함수 → ibv_modify_qp x3 + rdma_establish
 */
int
spdk_rdma_provider_qp_complete_connect(struct spdk_rdma_provider_qp *spdk_rdma_qp)
{
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	int rc;

	assert(spdk_rdma_qp);

	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);
	/* [한국어] wrapper 복원. */

	rc = rdma_mlx5_dv_init_qpair(mlx5_qp);
	/* [한국어] INIT/RTR/RTS 명시적 전이. 이 단계가 끝나야 송수신 가능. */
	if (rc) {
		SPDK_ERRLOG("Failed to initialize qpair\n");
		return rc;
	}

	rc = rdma_establish(mlx5_qp->common.cm_id);
	/* [한국어] librdmacm에 "이 cm_id가 ESTABLISHED 상태임을 마무리"하라고 통지.
	 * cm event 채널을 통한 마지막 RTU 동기화 단계. */
	if (rc) {
		SPDK_ERRLOG("rdma_establish failed, errno %s (%d)\n", spdk_strerror(errno), errno);
	}

	return rc;
}

/*
 * [한국어]
 * spdk_rdma_provider_qp_destroy - QP, memory_domain, stats 등 자원 회수 (mlx5_dv 백엔드)
 *
 * @spdk_rdma_qp: provider QP — NULL 비허용.
 *
 * verbs 백엔드와 달리 rdma_destroy_qp 대신 ibv_destroy_qp를 직접 부른다(mlx5dv_create_qp로
 * 만든 QP는 librdmacm이 추적하지 않을 수 있어 안전하게 ibv_destroy_qp 사용).
 *
 * 호출 체인:
 *   nvmf_rdma_qpair_destroy() → 이 함수
 *     → ibv_destroy_qp + spdk_memory_domain_destroy
 */
void
spdk_rdma_provider_qp_destroy(struct spdk_rdma_provider_qp *spdk_rdma_qp)
{
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	int rc;
	/* [한국어] ibv_destroy_qp 반환 코드 보관용. */

	assert(spdk_rdma_qp != NULL);

	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);
	/* [한국어] wrapper 복원. */

	if (spdk_rdma_qp->send_wrs.first != NULL) {
		/* [한국어] flush 안 된 send WR 잔존 — 송신 누락 가능성 경고. */
		SPDK_WARNLOG("Destroying qpair with queued Work Requests\n");
	}

	if (!mlx5_qp->common.shared_stats) {
		/* [한국어] 자체 stats만 free. */
		free(mlx5_qp->common.stats);
	}

	if (mlx5_qp->common.qp) {
		/* [한국어] QP 살아 있을 때만 destroy. */
		rc = ibv_destroy_qp(mlx5_qp->common.qp);
		/* [한국어] mlx5dv_create_qp로 만든 QP는 ibv_destroy_qp로 해제(librdmacm rdma_destroy_qp
		 * 가 아닌 점에 주의). 실패 시 leak이지만 다른 자원은 계속 정리. */
		if (rc) {
			SPDK_ERRLOG("Failed to destroy ibv qp %p, rc %d\n", mlx5_qp->common.qp, rc);
		}
	}
	if (spdk_rdma_qp->domain) {
		/* [한국어] mlx5_dv는 자체 memory_domain을 새로 만들었으므로 destroy.
		 * (verbs 백엔드는 rdma_utils의 공유 도메인을 ref-count로 빌렸기에 _put_memory_domain 사용 — 차이점!) */
		spdk_memory_domain_destroy(spdk_rdma_qp->domain);
	}

	free(mlx5_qp);
	/* [한국어] wrapper struct free. */
}

/*
 * [한국어]
 * spdk_rdma_provider_qp_disconnect - QP 연결 단절 (mlx5_dv 백엔드)
 *
 * @spdk_rdma_qp: 단절 대상.
 * @return: 0 성공, 그 외 실패.
 *
 * verbs 백엔드와 다른 점: 먼저 ibv_modify_qp로 QP를 명시적으로 ERR 상태로 옮겨 진행 중인
 * 모든 outstanding WR을 정리(완료 큐로 flush as ERROR)하고, 그 후 rdma_disconnect로
 * peer 통보. ERR 상태로 옮기는 이유는 mlx5의 graceful drain을 명시적으로 트리거하기 위함.
 *
 * 호출 체인:
 *   nvmf_rdma_qpair_destroy() → 이 함수 → ibv_modify_qp(ERR) → rdma_disconnect
 */
int
spdk_rdma_provider_qp_disconnect(struct spdk_rdma_provider_qp *spdk_rdma_qp)
{
	int rc = 0;

	assert(spdk_rdma_qp != NULL);

	if (spdk_rdma_qp->qp) {
		struct ibv_qp_attr qp_attr = {.qp_state = IBV_QPS_ERR};
		/* [한국어] qp_state만 세팅. mask로 IBV_QP_STATE만 켜서 다른 필드는 무시되게 한다. */

		rc = ibv_modify_qp(spdk_rdma_qp->qp, &qp_attr, IBV_QP_STATE);
		/* [한국어] QP를 ERR 상태로 강제 — outstanding WR이 모두 IBV_WC_*_ERR completion으로
		 * 빠져 나오게 한다. 이렇게 해야 caller(NVMe-oF poller)가 모든 WC를 회수해 cleanup 가능. */
		if (rc) {
			SPDK_ERRLOG("Failed to modify ibv qp %p state to ERR, rc %d\n", spdk_rdma_qp->qp, rc);
			return rc;
		}
	}

	if (spdk_rdma_qp->cm_id) {
		/* [한국어] cm_id 살아 있으면 peer에게 명시적 disconnect 통보. */
		rc = rdma_disconnect(spdk_rdma_qp->cm_id);
		if (rc) {
			SPDK_ERRLOG("rdma_disconnect failed, errno %s (%d)\n", spdk_strerror(errno), errno);
		}
	}

	return rc;
}

/*
 * [한국어]
 * spdk_rdma_provider_qp_queue_send_wrs - send WR 체인을 inline 빌드 모드로 누적 (mlx5_dv 백엔드)
 *
 * @spdk_rdma_qp: 대상 QP.
 * @first: 새로 만든 send WR 체인의 첫 노드(보통 1개~여러 개).
 * @return: true = 빈 큐에 처음 넣음(caller가 flush queue 등록 필요), false = 누적 append.
 *
 * verbs 백엔드는 단순히 링크드리스트에 첨부했지만, mlx5_dv 백엔드는 이미 ibv_wr_start로
 * inline 빌드를 시작해 각 WR을 ibv_wr_send/_send_inv/_rdma_read/_rdma_write로 즉시 빌드
 * 한다(Mellanox NIC의 BlueFlame 메모리 매핑에 직접 쓰기 시작). 이렇게 하면 flush 시점에는
 * ibv_wr_complete만 호출하면 되어 도어벨 latency가 최소화된다.
 *
 * 첫 호출(빈 큐)일 때만 ibv_wr_start를 부르고, 이후 호출은 같은 빌드 트랜잭션에 append.
 * 모든 호출이 같은 spdk_thread에서 일어나므로 트랜잭션 일관성 유지 가능(락 불필요).
 *
 * 호출 체인:
 *   nvmf_rdma_request_send_data() → 이 함수 → ibv_wr_start(첫 호출만) + ibv_wr_*
 */
bool
spdk_rdma_provider_qp_queue_send_wrs(struct spdk_rdma_provider_qp *spdk_rdma_qp,
				     struct ibv_send_wr *first)
{
	struct ibv_send_wr *tmp;
	/* [한국어] 체인 순회용 임시 포인터. */
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	bool is_first;
	/* [한국어] "이번 호출이 빈 큐에 처음 넣은 호출인가" 플래그 — return value 결정 + ibv_wr_start 호출 여부. */

	assert(spdk_rdma_qp);
	assert(first);

	is_first = spdk_rdma_qp->send_wrs.first == NULL;
	/* [한국어] 빈 큐 판단 — 빈 큐였으면 ibv_wr_start로 새 빌드 트랜잭션 시작 필요. */
	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	if (is_first) {
		ibv_wr_start(mlx5_qp->qpex);
		/* [한국어] inline send 빌드 트랜잭션 시작 — qpex 내부 상태 머신을 BUILD 모드로.
		 * 이후 ibv_wr_*는 BlueFlame 또는 SQ 메모리 영역에 직접 쓰며, ibv_wr_complete까지
		 * 도어벨은 발생하지 않는다. */
		spdk_rdma_qp->send_wrs.first = first;
		/* [한국어] 큐 헤드 기록 — flush_send_wrs가 NULL/non-NULL 판단에 사용. */
	} else {
		spdk_rdma_qp->send_wrs.last->next = first;
		/* [한국어] 이미 빌드 진행 중이면 단순 링크드리스트 append(빌드 자체는 아래 for문에서). */
	}

	for (tmp = first; tmp != NULL; tmp = tmp->next) {
		/* [한국어] 새로 들어온 WR 체인의 모든 노드를 순회하며 mlx5_dv 빌드 API로 옮긴다. */
		mlx5_qp->qpex->wr_id = tmp->wr_id;
		/* [한국어] WR 식별자 — completion 시 wc->wr_id로 회수되어 caller가 어떤 요청이었는지 식별. */
		mlx5_qp->qpex->wr_flags = tmp->send_flags;
		/* [한국어] IBV_SEND_SIGNALED/_INLINE/_FENCE/_SOLICITED 등 send 플래그.
		 * SIGNALED가 켜져야 CQE가 발생, FENCE는 이전 RDMA_READ 완료 후 진행 보장 등. */

		switch (tmp->opcode) {
		case IBV_WR_SEND:
			ibv_wr_send(mlx5_qp->qpex);
			/* [한국어] 일반 SEND — peer의 RX 큐에 capsule 페이로드 도착(payload는 sg_list로 별도 등록). */
			break;
		case IBV_WR_SEND_WITH_INV:
			ibv_wr_send_inv(mlx5_qp->qpex, tmp->invalidate_rkey);
			/* [한국어] SEND with Invalidate — peer가 등록한 rkey를 invalidate. NVMe-oF에서
			 * RDMA_READ/_WRITE 끝난 후 임시 rkey를 한 번에 무효화하는 데 사용. */
			break;
		case IBV_WR_RDMA_READ:
			ibv_wr_rdma_read(mlx5_qp->qpex, tmp->wr.rdma.rkey, tmp->wr.rdma.remote_addr);
			/* [한국어] RDMA READ — peer 메모리(remote_addr+rkey)를 읽어 내 sg_list 영역으로 DMA.
			 * NVMe-oF에서 host의 write 명령 시 host data를 끌어오는 데 사용. */
			break;
		case IBV_WR_RDMA_WRITE:
			ibv_wr_rdma_write(mlx5_qp->qpex, tmp->wr.rdma.rkey, tmp->wr.rdma.remote_addr);
			/* [한국어] RDMA WRITE — 내 sg_list 영역에서 peer 메모리(remote_addr+rkey)로 DMA.
			 * NVMe-oF에서 host read 명령 시 컨트롤러가 데이터를 host로 푸시하는 데 사용. */
			break;
		default:
			SPDK_ERRLOG("Unexpected opcode %d\n", tmp->opcode);
			/* [한국어] mlx5_dv 백엔드는 SPDK NVMe-oF가 사용하는 4개 opcode만 지원 — 그 외는
			 * 코딩 버그. assert(0)으로 디버그 빌드에서 즉시 abort. */
			assert(0);
		}

		ibv_wr_set_sge_list(mlx5_qp->qpex, tmp->num_sge, tmp->sg_list);
		/* [한국어] 위 opcode가 사용할 SGE(Scatter-Gather Element) 리스트 등록.
		 * 각 SGE = (addr, length, lkey) 트리플. lkey는 PD에 등록된 MR의 local key. */

		spdk_rdma_qp->send_wrs.last = tmp;
		/* [한국어] 큐 끝 갱신 — 다음 queue 호출이 append 위치로 사용. */
		spdk_rdma_qp->stats->send.num_submitted_wrs++;
		/* [한국어] 통계 누적. */
	}

	return is_first;
	/* [한국어] caller가 첫 호출에만 flush queue에 자기 자신을 등록하도록. */
}

/*
 * [한국어]
 * spdk_rdma_provider_qp_flush_send_wrs - inline 빌드 트랜잭션을 종료(=post + 도어벨) (mlx5_dv 백엔드)
 *
 * @spdk_rdma_qp: 대상 QP.
 * @bad_wr: 출력 — ibv_wr_complete 실패 시 첫 노드를 가리키게 함(이 시점에 NIC에 못 갔음).
 * @return: ibv_wr_complete 반환값(0 성공).
 *
 * 동작: ibv_wr_complete()는 BlueFlame/SQ에 빌드된 모든 WR을 한 번에 NIC로 commit하고
 * 도어벨까지 친다. 빌드 트랜잭션이 종료되므로 다음 queue_send_wrs 호출은 다시 ibv_wr_start로
 * 새 트랜잭션을 시작해야 한다.
 *
 * 호출 체인:
 *   nvmf_rdma_qpair_process_pending() → 이 함수
 *     → ibv_wr_complete → BlueFlame/MMIO 도어벨 → NIC가 SQ fetch
 */
int
spdk_rdma_provider_qp_flush_send_wrs(struct spdk_rdma_provider_qp *spdk_rdma_qp,
				     struct ibv_send_wr **bad_wr)
{
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	int rc;

	assert(bad_wr);
	assert(spdk_rdma_qp);

	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	if (spdk_unlikely(spdk_rdma_qp->send_wrs.first == NULL)) {
		/* [한국어] 큐가 비어 있으면 빌드 트랜잭션 자체가 시작되지 않았으니 complete 호출 불필요. */
		return 0;
	}

	rc = ibv_wr_complete(mlx5_qp->qpex);
	/* [한국어] 빌드 트랜잭션 종료 = 모든 빌드된 WR이 한 번의 도어벨 MMIO write로 NIC에 commit.
	 * Mellanox BlueFlame 경로에서는 도어벨 register에 바로 inline post되어 latency 최소.
	 * 실패 시 NIC에 아무 WR도 도달하지 못한 상태. */

	if (spdk_unlikely(rc)) {
		/* If ibv_wr_complete reports an error that means that no WRs are posted to NIC */
		/* [한국어] 실패 시 caller에게 첫 WR 포인터를 돌려준다 — caller는 이 시점부터 정리. */
		*bad_wr = spdk_rdma_qp->send_wrs.first;
	}

	spdk_rdma_qp->send_wrs.first = NULL;
	/* [한국어] 큐 헤드 리셋 — 다음 queue_send_wrs 호출이 새 트랜잭션을 시작하도록. */
	spdk_rdma_qp->stats->send.doorbell_updates++;
	/* [한국어] send 도어벨 카운트 누적 — 배치 효율 모니터링. */

	return rc;
}

/*
 * [한국어]
 * spdk_rdma_provider_accel_sequence_supported - accel 시퀀스(NIC offload) 지원 여부
 *
 * @return: spdk_mlx5_umr_implementer_is_registered() — UMR(User MR) 기능을 사용하는 모듈
 *          (예: mlx5 accel 모듈)이 등록된 경우 true.
 *
 * verbs 백엔드는 항상 false였지만, mlx5_dv는 UMR 사용 가능 시 true를 반환해 caller(accel
 * framework)가 데이터 변환을 NIC에 offload하도록 한다.
 *
 * 호출 체인:
 *   spdk_accel_sequence_*() / nvmf_rdma_accel_check() → 이 함수 → spdk_mlx5_umr_implementer_is_registered
 */
bool
spdk_rdma_provider_accel_sequence_supported(void)
{
	return spdk_mlx5_umr_implementer_is_registered();
	/* [한국어] mlx5 UMR 기능을 사용하는 모듈이 등록되어 있는지 동적 질의 — 등록 시 true.
	 * UMR이 있으면 NIC가 데이터 변환(예: in-place 압축)을 직접 수행 가능. */
}
