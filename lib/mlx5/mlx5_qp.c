/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] mlx5 Queue Pair / Completion Queue 생명주기 관리 (mlx5_qp.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 lib/mlx5의 *컨트롤-플레인* 구현으로, ConnectX NIC의 RC(Reliable Connection) QP와
 * CQ를 생성·연결(connect)·파괴하는 전 과정을 담당한다. 구체적으로:
 *   (1) mlx5dv_create_cq / mlx5dv_create_qp로 raw QP/CQ 객체 생성,
 *   (2) mlx5dv_init_obj로 NIC가 보는 raw 메모리 주소(SQ/CQ buf, DBR, BlueFlame UAR)를 추출하여
 *       spdk_mlx5_qp/cq.hw에 캐시,
 *   (3) loopback 방식으로 QP를 RST → INIT → RTR → RTS 상태머신을 PRM devx 명령으로 직접 천이
 *       (kernel 우회) — 단일 노드 내에서 자기 자신과 통신하는 QP를 만들기 위함,
 *   (4) CQ에 여러 QP를 바인딩할 때 사용하는 24비트 qp_num → spdk_mlx5_qp* 2D LUT 갱신,
 *   (5) ERROR 상태 강제 천이, verbs_qp 노출 등 보조 API.
 *
 * 데이터-플레인(WQE 작성, CQE 폴링)은 mlx5_dma.c / mlx5_umr.c에서 처리하며 본 파일은 자원 셋업/해제만 담당.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   상위 모듈(NVMe-oF RDMA, accel_mlx5 등) → spdk_mlx5_cq_create() → mlx5_cq_init()
 *     → mlx5dv_create_cq → mlx5dv_init_obj
 *   상위 모듈 → spdk_mlx5_qp_create() → mlx5_qp_init() → mlx5dv_create_qp → mlx5dv_init_obj
 *     → mlx5_qp_connect() → mlx5_qp_get_port_pkey_idx / mlx5_fill_qp_conn_caps / mlx5_check_port
 *     → mlx5_qp_loopback_conn() → rst→init→rtr→rts (각 단계 devx 명령) → mlx5_cq_add_qp() (LUT 등록)
 *   파괴: spdk_mlx5_qp_destroy → mlx5_cq_remove_qp + mlx5_qp_destroy → ibv_destroy_qp + free
 * 실행 컨텍스트: init 시점에 메인 스레드에서 호출. 데이터-플레인은 reactor 스레드.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: libibverbs(ibv_query_qp/port, ibv_destroy_*), libmlx5dv(create_cq/qp, init_obj, devx_qp_modify),
 *   mlx5_priv.h(spdk_mlx5_cq/qp 정의 + 인라인 헬퍼), mlx5_ifc.h(PRM 비트 정의),
 *   spdk_internal/rdma_utils.h, spdk_internal/assert.h(SPDK_UNREACHABLE).
 * - 데이터 흐름: 사용자 attr → mlx5_qp_init이 dv_qp_attr/mlx5_qp_attr 구성 → mlx5dv_create_qp →
 *   mlx5dv_init_obj로 raw 주소 추출 → posix_memalign으로 completions[] 할당 → mlx5_qp_connect로 RTS까지.
 *   파괴 시 역순.
 * - 공유 상태: spdk_mlx5_cq.qps[][] 2D LUT — 본 파일이 add/remove. mlx5_dma.c의 polling 코드가 read.
 *   같은 reactor 스레드에서만 호출되도록 호출자가 보장 → 락 없음.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct mlx5_qp_conn_caps: QP 연결 시 필요한 디바이스/포트 capability 모음(local 변수).
 * - mlx5_cq_init / mlx5_cq_deinit: CQ 생성/파괴 내부 핼퍼.
 * - mlx5_qp_init / mlx5_qp_destroy: QP 생성/파괴 내부 헬퍼.
 * - mlx5_qp_get_port_pkey_idx / mlx5_check_port / mlx5_fill_qp_conn_caps: connect 사전 정보 수집.
 * - mlx5_qp_loopback_conn_rts_2_init / init_2_rtr / rtr_2_rts: PRM devx 명령으로 상태 천이.
 * - mlx5_qp_loopback_conn / mlx5_qp_connect: 위 단계들을 묶어 RST→RTS까지 한 번에.
 * - mlx5_cq_add_qp / mlx5_cq_remove_qp: CQ의 2D LUT에 QP 등록/제거.
 * - spdk_mlx5_cq_create / destroy: CQ 공개 API.
 * - spdk_mlx5_qp_create / destroy: QP 공개 API.
 * - spdk_mlx5_qp_set_error_state: QP를 ERROR 상태로 강제 천이(graceful shutdown 시 잔여 WQE flush 유도).
 * - spdk_mlx5_qp_get_verbs_qp: 외부에 ibv_qp 핸들 노출(예: ibv_query_qp 등 verbs API 호환).
 */

/* [한국어] Mellanox direct-verbs 헤더 — mlx5dv_create_cq/qp, mlx5dv_init_obj, mlx5dv_devx_qp_modify 등. */
#include <infiniband/mlx5dv.h>

/* [한국어] lib/mlx5 내부 정의 헤더. */
#include "mlx5_priv.h"
/* [한국어] PRM 자동생성 비트 정의 — rst2init_qp_in/out, MLX5_CMD_OP_RST2INIT_QP, qpc 필드 등. */
#include "mlx5_ifc.h"
/* [한국어] SPDK_ERRLOG / NOTICELOG / DEBUGLOG. */
#include "spdk/log.h"
/* [한국어] spdk_u32log2 등 정수 유틸. log_rra_max/log_sra_max 인코딩에 사용. */
#include "spdk/util.h"

/* [한국어] SPDK_UNREACHABLE() — 도달해서는 안 되는 코드 경로 표식. 디버그 빌드 시 abort. */
#include "spdk_internal/assert.h"
/* [한국어] SPDK 내부 RDMA 공통 유틸(본 파일에서 직접 사용은 미미하나 의존성 유지). */
#include "spdk_internal/rdma_utils.h"

/* [한국어] RTR 상태로 천이할 때의 RQ Packet Sequence Number 초기값. loopback이라 임의 값 가능. */
#define MLX5_QP_RQ_PSN              0x4242
/* [한국어] 응답자(=수신) 측이 동시에 처리할 수 있는 최대 RDMA Read/Atomic 요청 수. PRM 한도 16. */
#define MLX5_QP_MAX_DEST_RD_ATOMIC      16
/* [한국어] RNR(Receiver Not Ready) NAK 시 대기 시간 인코딩. 12 = 0.96ms 정도(PRM 표 참조). */
#define MLX5_QP_RNR_TIMER               12
/* [한국어] IPv6/RoCEv2 GRH hop limit 기본값. loopback이라 1로도 충분하지만 64로 보수적 설정. */
#define MLX5_QP_HOP_LIMIT               64

/* RTS state params */
/* [한국어] RTS 상태 천이 파라미터들 — ack timeout, retry, RNR retry, max read 등. */
#define MLX5_QP_TIMEOUT            14
/* [한국어] ack timeout 인코딩. 14 = 약 0.27ms (PRM "Local ACK Timeout" 표 기준 4.096us × 2^14). */
#define MLX5_QP_RETRY_COUNT         7
/* [한국어] 응답 미수신 시 transport-level 재시도 횟수. 최대 7. */
#define MLX5_QP_RNR_RETRY           7
/* [한국어] RNR NAK 시 재시도 횟수. 7=infinite(PRM에서 0~6은 횟수, 7은 무한). */
#define MLX5_QP_MAX_RD_ATOMIC      16
/* [한국어] 요청자(=발신) 측이 동시에 보낼 수 있는 outstanding RDMA Read/Atomic 수. */
#define MLX5_QP_SQ_PSN         0x4242
/* [한국어] RTS 천이 시 SQ Packet Sequence Number 초기값. */

/*
 * [한국어]
 * struct mlx5_qp_conn_caps - QP 연결 시 한 번 수집해 단계별 천이 함수에 전달되는 컨텍스트.
 *
 * 본 파일 내부 전용. 디바이스/포트 capability 비트 + 포트 번호/pkey 등 RST→RTS 천이에 필요한
 * 값을 한 곳에 모은다.
 */
struct mlx5_qp_conn_caps {
	bool resources_on_nvme_emulation_manager;
	/* [한국어] BlueField NVMe 에뮬레이션 매니저(em) 자원으로 QP가 만들어졌는지. force-loopback 가능
	 * 여부 판정에 사용. */

	bool roce_enabled;
	/* [한국어] HCA 전역 RoCE 지원/활성 여부. */

	bool fl_when_roce_disabled;
	/* [한국어] RoCE 꺼진 상태에서 force-loopback RC QP 허용 여부. */

	bool fl_when_roce_enabled;
	/* [한국어] RoCE 켜진 상태에서 force-loopback 허용 여부. */

	bool port_ib_enabled;
	/* [한국어] 포트가 InfiniBand 링크 레이어인지(아니면 ETH/RoCE). IB이면 force-loopback 자동 가능. */

	uint8_t roce_version;
	/* [한국어] RoCE 버전(v1/v2). */

	uint8_t port;
	/* [한국어] QP가 묶인 포트 번호(보통 1). */

	uint16_t pkey_idx;
	/* [한국어] PKey 테이블 인덱스. IB에서 partitioning에 사용. */

	enum ibv_mtu mtu;
	/* [한국어] 포트 active MTU. RTR 천이 시 path_mtu로 전달. */
};

/* [한국어] 전방 선언 — mlx5_qp_init이 호출, 정의는 아래에. */
static int mlx5_qp_connect(struct spdk_mlx5_qp *qp);

/*
 * [한국어]
 * mlx5_cq_deinit - CQ 내부 자원 해제(verbs_cq destroy).
 *
 * 호출 체인: spdk_mlx5_cq_destroy → [mlx5_cq_deinit]
 */
static void
mlx5_cq_deinit(struct spdk_mlx5_cq *cq)
{
	if (cq->verbs_cq) {
		ibv_destroy_cq(cq->verbs_cq);
		/* [한국어] libibverbs로 CQ 파괴. CQ 메모리(NIC가 매핑한 hugepage)도 함께 해제됨. */
	}
}

/*
 * [한국어]
 * mlx5_cq_init - CQ 생성 + raw 메타데이터 추출.
 *
 * @pd: PD(CQ는 PD 소속이지만 NIC 자원으로는 분리). pd->context로 디바이스 식별.
 * @attr: SPDK 사용자 CQ 속성(cqe_cnt/size/comp_channel/comp_vector 등).
 * @cq: 출력 spdk_mlx5_cq.
 * @return: 0 성공, 음수 errno.
 *
 * mlx5dv_create_cq로 cqe_size 지정 가능한 CQ 생성 → mlx5dv_init_obj로 NIC가 보는 raw 주소 추출
 * (cq->hw.cq_addr/cqe_cnt/cqe_size/cq_num). IGNORE_OVERRUN 플래그로 오버런 시 자동 무시 동작
 * (운영 잡음 감소).
 *
 * 호출 체인: spdk_mlx5_cq_create → [mlx5_cq_init]
 */
static int
mlx5_cq_init(struct ibv_pd *pd, const struct spdk_mlx5_cq_attr *attr, struct spdk_mlx5_cq *cq)
{
	struct ibv_cq_init_attr_ex cq_attr = {
		.cqe = attr->cqe_cnt,
		/* [한국어] 사용자가 요청한 CQE 슬롯 수. NIC이 next-pow2로 올릴 수 있음. */
		.cq_context = attr->cq_context,
		/* [한국어] CQ에 연결될 사용자 컨텍스트(이벤트 콜백 등에서 사용). */
		.channel = attr->comp_channel,
		/* [한국어] completion event channel. polling 모드라면 NULL. */
		.comp_vector = attr->comp_vector,
		/* [한국어] MSI-X 인터럽트 벡터 번호(channel 사용 시). */
		.wc_flags = IBV_WC_STANDARD_FLAGS,
		/* [한국어] WC(work completion)에 채울 표준 필드 셋. */
		.comp_mask = IBV_CQ_INIT_ATTR_MASK_FLAGS,
		/* [한국어] flags 필드를 사용함을 표시. */
		.flags = IBV_CREATE_CQ_ATTR_IGNORE_OVERRUN
		/* [한국어] CQ 오버런(consumer가 너무 느림) 시 자동 drop. polling이 충분히 빠르면 발생 안 함. */
	};
	struct mlx5dv_cq_init_attr cq_ex_attr = {
		.comp_mask = MLX5DV_CQ_INIT_ATTR_MASK_CQE_SIZE,
		/* [한국어] cqe_size 필드 사용 표시. */
		.cqe_size = attr->cqe_size
		/* [한국어] CQE 한 개 크기(64 또는 128). 64면 mini-CQE 비활성. */
	};
	struct mlx5dv_obj dv_obj;
	struct mlx5dv_cq mlx5_cq;
	struct ibv_cq_ex *cq_ex;
	int rc;

	cq_ex = mlx5dv_create_cq(pd->context, &cq_attr, &cq_ex_attr);
	/* [한국어] mlx5 전용 CQ 생성. cqe_size 지정이 가능한 확장 API. */
	if (!cq_ex) {
		rc = -errno;
		SPDK_ERRLOG("mlx5dv_create_cq failed, errno %d\n", rc);
		return rc;
	}

	cq->verbs_cq = ibv_cq_ex_to_cq(cq_ex);
	/* [한국어] cq_ex(확장)에서 일반 ibv_cq 핸들 추출 — 파괴/QP 생성 인자에 필요. */
	assert(cq->verbs_cq);

	dv_obj.cq.in = cq->verbs_cq;
	dv_obj.cq.out = &mlx5_cq;
	/* [한국어] init_obj 입출력 union 설정. */

	/* Init CQ - CQ is marked as owned by DV for all consumer index related actions */
	rc = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_CQ);
	/* [한국어] DV가 CQ를 인수 — ci 추적 등은 사용자 직접 관리. NIC이 보는 raw 메모리 주소도 회수. */
	if (rc) {
		SPDK_ERRLOG("Failed to init DV CQ, rc %d\n", rc);
		ibv_destroy_cq(cq->verbs_cq);
		free(cq);
		/* [한국어] 주의: 이 시점의 cq는 호출자(spdk_mlx5_cq_create)가 calloc한 것 — 여기서 free하면
		 * 호출자가 별도로 또 free해 double-free 위험. 원본 코드 패턴을 그대로 두되 호출자에서 cq를
		 * 재할당하지 않도록 주의 필요. */
		return rc;
	}

	cq->hw.cq_addr = (uintptr_t)mlx5_cq.buf;
	/* [한국어] CQ 버퍼 가상 주소 — polling 코드가 ci 위치를 직접 읽음. */
	cq->hw.ci = 0;
	/* [한국어] consumer index 초기값 0. */
	cq->hw.cqe_cnt = mlx5_cq.cqe_cnt;
	/* [한국어] NIC이 결정한 실제 CQE 슬롯 수(요청값을 next-pow2로 올린 값). */
	cq->hw.cqe_size = mlx5_cq.cqe_size;
	/* [한국어] CQE 크기(64 또는 128). */
	cq->hw.cq_num = mlx5_cq.cqn;
	/* [한국어] NIC이 부여한 CQ 번호(cqn). */

	return 0;
}

/*
 * [한국어]
 * mlx5_qp_destroy - QP 내부 자원 해제(verbs_qp + completions[]).
 *
 * 호출 체인: spdk_mlx5_qp_destroy → [mlx5_qp_destroy]
 */
static void
mlx5_qp_destroy(struct spdk_mlx5_qp *qp)
{
	if (qp->verbs_qp) {
		ibv_destroy_qp(qp->verbs_qp);
		/* [한국어] verbs QP 파괴. SQ 버퍼/UAR/DBR 매핑도 함께 해제. */
	}
	if (qp->completions) {
		free(qp->completions);
		/* [한국어] 호스트-측 completion 추적 배열(posix_memalign 할당) 해제. */
	}
}

/*
 * [한국어]
 * mlx5_qp_init - QP 생성 + raw 메타데이터 추출 + completions 할당 + connect 호출.
 *
 * @pd: 소속 PD.
 * @attr: 사용자 QP 속성(cap/sigall/siglast).
 * @cq: send/recv CQ(하나 공유).
 * @qp: 출력.
 * @return: 0 성공, 음수 errno.
 *
 * QP는 RC 타입으로 고정, send_ops_flags에 RDMA write/read/send/MW bind 표시. mlx5_qp_attr에는
 * MKEY configure 지원 추가(UMR로 동적 MKEY 등록 가능). connect 실패 시 모든 자원 해제.
 *
 * 주의: SQ 슬롯 개수는 NIC이 max_send_wr × WR당 BB 수로 round-up 결정. NOTICELOG로 사용자에게
 * 실제 sq_wqe_cnt와 WR당 BB 수를 안내.
 *
 * 호출 체인: spdk_mlx5_qp_create → [mlx5_qp_init] → mlx5dv_create_qp / mlx5dv_init_obj / mlx5_qp_connect
 */
static int
mlx5_qp_init(struct ibv_pd *pd, const struct spdk_mlx5_qp_attr *attr, struct ibv_cq *cq,
	     struct spdk_mlx5_qp *qp)
{
	struct mlx5dv_qp dv_qp;
	struct mlx5dv_obj dv_obj;
	struct ibv_qp_init_attr_ex dv_qp_attr = {
		.cap = attr->cap,
		/* [한국어] max_send_wr/max_send_sge/max_recv_wr/max_recv_sge/max_inline_data. */
		.qp_type = IBV_QPT_RC,
		/* [한국어] RC(Reliable Connection) — 본 라이브러리는 RC만 지원. */
		.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS,
		/* [한국어] PD 필드와 send_ops_flags 필드 사용 표시. */
		.pd = pd,
		.send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE | IBV_QP_EX_WITH_SEND | IBV_QP_EX_WITH_RDMA_READ | IBV_QP_EX_WITH_BIND_MW,
		/* [한국어] 이 QP가 게시할 수 있는 op 종류. RDMA_WRITE/READ + SEND + BIND_MW. */
		.send_cq = cq,
		.recv_cq = cq,
		/* [한국어] send/recv CQ 동일 — 단일 polling 루프로 모두 처리. */
		.sq_sig_all = attr->sigall,
		/* [한국어] 모든 SQ WR에 자동 CQ_UPDATE 표시. SPDK_MLX5_QP_SIG_ALL 모드와 연동. */
	};
	/* Attrs required for MKEYs registration */
	struct mlx5dv_qp_init_attr mlx5_qp_attr = {
		.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_SEND_OPS_FLAGS,
		.send_ops_flags = MLX5DV_QP_EX_WITH_MKEY_CONFIGURE
		/* [한국어] mlx5 전용: 이 QP로 MKEY 동적 등록(UMR) WQE 게시 가능. crypto/sig/scatter-gather에 사용. */
	};
	int rc;

	if (attr->sigall && attr->siglast) {
		SPDK_ERRLOG("Params sigall and siglast can't be enabled simultaneously\n");
		return -EINVAL;
		/* [한국어] 두 옵션은 상호 배타적 — 사용자 입력 검증. */
	}

	qp->verbs_qp = mlx5dv_create_qp(pd->context, &dv_qp_attr, &mlx5_qp_attr);
	/* [한국어] QP 생성. 두 attr 구조체를 모두 전달 — verbs 표준 + mlx5 확장. */
	if (!qp->verbs_qp) {
		rc = -errno;
		SPDK_ERRLOG("Failed to create qp, rc %d\n", rc);
		return rc;
	}

	dv_obj.qp.in = qp->verbs_qp;
	dv_obj.qp.out = &dv_qp;

	rc = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_QP);
	/* [한국어] NIC이 보는 raw 주소(SQ buf, DBR, BlueFlame UAR)와 sq_wqe_cnt 추출. */
	if (rc) {
		ibv_destroy_qp(qp->verbs_qp);
		SPDK_ERRLOG("Failed to init DV QP, rc %d\n", rc);
		return rc;
	}

	qp->hw.sq_addr = (uint64_t)dv_qp.sq.buf;
	/* [한국어] SQ 버퍼 가상 주소 — WQE 작성 대상. */
	qp->hw.dbr_addr = (uint64_t)dv_qp.dbrec;
	/* [한국어] Doorbell Record 주소 — sq_pi 기록. */
	qp->hw.sq_bf_addr = (uint64_t)dv_qp.bf.reg;
	/* [한국어] BlueFlame UAR(MMIO) 주소 — ctrl seg 8B store로 doorbell 발사. */
	qp->hw.sq_wqe_cnt = dv_qp.sq.wqe_cnt;
	/* [한국어] SQ 슬롯 수(2의 거듭제곱). */

	SPDK_NOTICELOG("mlx5 QP, sq size %u WQE_BB. %u send_wrs -> %u WQE_BB per send WR\n",
		       qp->hw.sq_wqe_cnt, attr->cap.max_send_wr, qp->hw.sq_wqe_cnt / attr->cap.max_send_wr);
	/* [한국어] 진단 로그 — SQ 총 BB 수, 사용자 요청 WR 수, WR당 평균 BB 수. 튜닝 시 참고. */

	qp->hw.qp_num = qp->verbs_qp->qp_num;
	/* [한국어] NIC 부여 24비트 QP 번호 캐싱. */

	qp->hw.sq_tx_db_nc = dv_qp.bf.size == 0;
	/* [한국어] BF UAR이 NC(non-combined) 매핑이면 size=0. true면 추가 fence 불필요. */
	qp->tx_available = qp->hw.sq_wqe_cnt;
	/* [한국어] SQ 슬롯 잔량 초기값 = 전체 슬롯 수. */
	qp->max_send_sge = attr->cap.max_send_sge;
	/* [한국어] WR당 SGE 한도. */
	rc = posix_memalign((void **)&qp->completions, 4096, qp->hw.sq_wqe_cnt * sizeof(*qp->completions));
	/* [한국어] 호스트-측 completion 추적 배열을 4KB 정렬로 할당. 페이지 정렬은 캐시-라인 충돌 회피
	 * + DMA 등 향후 확장 대비. */
	if (rc) {
		ibv_destroy_qp(qp->verbs_qp);
		SPDK_ERRLOG("Failed to alloc completions\n");
		return rc;
	}
	qp->sigmode = SPDK_MLX5_QP_SIG_NONE;
	/* [한국어] 기본 sigmode = NONE(사용자 flags 그대로). */
	if (attr->sigall) {
		qp->sigmode = SPDK_MLX5_QP_SIG_ALL;
		/* [한국어] 모든 WQE에 CQ_UPDATE 강제. */
	} else if (attr->siglast) {
		qp->sigmode = SPDK_MLX5_QP_SIG_LAST;
		/* [한국어] 마지막 WQE에만 lazy CQ_UPDATE — batched submit 효율. */
	}

	rc = mlx5_qp_connect(qp);
	/* [한국어] QP를 RST → INIT → RTR → RTS까지 연결. 실패 시 자원 해제. */
	if (rc) {
		ibv_destroy_qp(qp->verbs_qp);
		free(qp->completions);
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * mlx5_qp_get_port_pkey_idx - QP의 현재 port/pkey_index 조회 (ibv_query_qp).
 *
 * connect 시작 시점에 QP가 어느 포트에 묶였는지 확인하기 위함. mlx5dv_create_qp의 결과로 자동
 * 결정됨.
 */
static int
mlx5_qp_get_port_pkey_idx(struct spdk_mlx5_qp *qp, struct mlx5_qp_conn_caps *conn_caps)
{
	struct ibv_qp_attr attr = {};
	struct ibv_qp_init_attr init_attr = {};
	int attr_mask = IBV_QP_PKEY_INDEX | IBV_QP_PORT;
	/* [한국어] pkey_index와 port_num만 조회. */
	int rc;

	rc = ibv_query_qp(qp->verbs_qp, &attr, attr_mask, &init_attr);
	/* [한국어] verbs API로 QP 속성 조회 — 시스템 콜. */
	if (rc) {
		SPDK_ERRLOG("Failed to query qp %p %u\n", qp, qp->hw.qp_num);
		return rc;
	}
	conn_caps->port = attr.port_num;
	conn_caps->pkey_idx = attr.pkey_index;

	return 0;
}

/*
 * [한국어]
 * mlx5_check_port - 포트 link layer 검사 + MTU 결정 + IB GRH 가능 여부 검사.
 *
 * IB 링크: GRH(Global Routing Header) 강제 요구하면 본 라이브러리(local addressing 가정) 미지원.
 * ETH 링크: MTU를 4096으로 고정(RoCE는 path MTU 협상 없이 고정값 사용 가능).
 */
static int
mlx5_check_port(struct ibv_context *ctx, struct mlx5_qp_conn_caps *conn_caps)
{
	struct ibv_port_attr port_attr = {};
	int rc;

	conn_caps->port_ib_enabled = false;
	/* [한국어] 기본값 false — IB로 판정될 때만 true로. */

	rc = ibv_query_port(ctx, conn_caps->port, &port_attr);
	if (rc) {
		return rc;
	}

	if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
		/* we only support local IB addressing for now */
		/* [한국어] 본 라이브러리는 같은 노드 내 loopback만 지원 — 멀티-호스트 IB는 미지원. */
		if (port_attr.flags & IBV_QPF_GRH_REQUIRED) {
			SPDK_ERRLOG("IB enabled and GRH addressing is required but only local addressing is supported\n");
			return -1;
			/* [한국어] GRH 강제 요구 → 글로벌 라우팅 필요 → 본 라이브러리 부적합. */
		}
		conn_caps->mtu = port_attr.active_mtu;
		/* [한국어] IB는 active_mtu가 정확한 값. */
		conn_caps->port_ib_enabled = true;
		return 0;
	}

	if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET) {
		return -1;
		/* [한국어] IB도 ETH도 아니면 비정상. */
	}

	conn_caps->mtu = IBV_MTU_4096;
	/* [한국어] RoCE 가정 — MTU 4096으로 충분. (실제 path MTU와 별개로 RC 패킷 단위) */

	return 0;
}

/*
 * [한국어]
 * mlx5_fill_qp_conn_caps - HCA capability를 query_hca_cap devx 명령으로 조회.
 *
 * @context: 디바이스 ctx.
 * @conn_caps: 출력. resources_on_nvme_emulation_manager / fl_when_roce_disabled / roce_enabled /
 *             (RoCE 활성 시) roce_version / fl_when_roce_enabled.
 *
 * 두 단계 query: GENERAL_DEVICE 그리고 (RoCE 활성 시) ROCE.
 *
 * 호출 체인: mlx5_qp_connect → [mlx5_fill_qp_conn_caps]
 */
static int
mlx5_fill_qp_conn_caps(struct ibv_context *context,
		       struct mlx5_qp_conn_caps *conn_caps)
{
	uint8_t in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {0};
	int rc;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	/* [한국어] opcode = QUERY_HCA_CAP. */
	DEVX_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE);
	/* [한국어] op_mod = GENERAL_DEVICE (GET_CUR 비트 미포함; PRM에서 자동 처리). */
	rc = mlx5dv_devx_general_cmd(context, in, sizeof(in), out,
				     sizeof(out));
	if (rc) {
		return rc;
	}

	conn_caps->resources_on_nvme_emulation_manager =
		DEVX_GET(query_hca_cap_out, out,
			 capability.cmd_hca_cap.resources_on_nvme_emulation_manager);
	/* [한국어] BlueField NVMe 에뮬레이션 매니저 자원 사용 여부 — force-loopback 가능 판정. */
	conn_caps->fl_when_roce_disabled = DEVX_GET(query_hca_cap_out, out,
					   capability.cmd_hca_cap.fl_rc_qp_when_roce_disabled);
	/* [한국어] RoCE 꺼졌을 때 force-loopback RC QP 허용 비트. */
	conn_caps->roce_enabled = DEVX_GET(query_hca_cap_out, out,
					   capability.cmd_hca_cap.roce);
	/* [한국어] HCA의 RoCE 일반 활성 비트. */
	if (!conn_caps->roce_enabled) {
		goto out;
		/* [한국어] RoCE 꺼져있으면 ROCE op_mod 조회 의미 없음 — 바로 종료. */
	}

	memset(in, 0, sizeof(in));
	memset(out, 0, sizeof(out));
	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod, MLX5_SET_HCA_CAP_OP_MOD_ROCE);
	/* [한국어] 두 번째 조회 — ROCE 카테고리. */
	rc = mlx5dv_devx_general_cmd(context, in, sizeof(in), out,
				     sizeof(out));
	if (rc) {
		return rc;
	}

	conn_caps->roce_version = DEVX_GET(query_hca_cap_out, out,
					   capability.roce_caps.roce_version);
	/* [한국어] RoCE 버전(v1/v2). */
	conn_caps->fl_when_roce_enabled = DEVX_GET(query_hca_cap_out,
					  out, capability.roce_caps.fl_rc_qp_when_roce_enabled);
	/* [한국어] RoCE 활성 시 force-loopback 허용 비트. */
out:
	SPDK_DEBUGLOG(mlx5, "RoCE Caps: enabled %d ver %d fl allowed %d\n",
		      conn_caps->roce_enabled, conn_caps->roce_version,
		      conn_caps->roce_enabled ? conn_caps->fl_when_roce_enabled :
		      conn_caps->fl_when_roce_disabled);
	/* [한국어] 진단 로그: RoCE 활성/버전/force-loopback 가능 여부. */
	return 0;
}

/*
 * [한국어]
 * mlx5_qp_loopback_conn_rts_2_init - QP를 RST 상태에서 INIT 상태로 천이 (devx).
 *
 * @qp: QP. @qp_attr/@attr_mask: 표준 verbs ibv_qp_attr 형식의 입력.
 *
 * RST→INIT 천이 시 PRM 명령은 rst2init_qp(_in/_out). qpc(QP context) 내 pm_state, pkey_index,
 * vhca_port_num, rre/rwe(원격 read/write enable)를 설정. 함수명 오타("rts_2_init")는 실제로는
 * RST→INIT를 의미.
 *
 * 왜 verbs ibv_modify_qp가 아니라 devx인가? loopback connect는 일반 connection manager 없이
 * 우리가 직접 모든 파라미터를 채워 보내야 하기 때문 — kernel이 모르므로 이후 RTR→RTS도 devx로 가야 일관됨.
 */
static int
mlx5_qp_loopback_conn_rts_2_init(struct spdk_mlx5_qp *qp, struct ibv_qp_attr *qp_attr,
				 int attr_mask)
{
	uint8_t in[DEVX_ST_SZ_BYTES(rst2init_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(rst2init_qp_out)] = {0};
	void *qpc = DEVX_ADDR_OF(rst2init_qp_in, in, qpc);
	/* [한국어] in 버퍼 안의 qpc(QP context) 시작 주소. */
	int rc;

	DEVX_SET(rst2init_qp_in, in, opcode, MLX5_CMD_OP_RST2INIT_QP);
	/* [한국어] opcode = RST2INIT_QP. */
	DEVX_SET(rst2init_qp_in, in, qpn, qp->hw.qp_num);
	/* [한국어] 천이 대상 QP 번호. */
	DEVX_SET(qpc, qpc, pm_state, MLX5_QP_PM_MIGRATED);
	/* [한국어] Path Migration 상태 = MIGRATED(기본). */

	if (attr_mask & IBV_QP_PKEY_INDEX)
		DEVX_SET(qpc, qpc, primary_address_path.pkey_index,
			 qp_attr->pkey_index);
		/* [한국어] PKey 인덱스. IB partitioning에 사용. */

	if (attr_mask & IBV_QP_PORT)
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num,
			 qp_attr->port_num);
		/* [한국어] vhca_port_num — virtual HCA의 포트 번호(SR-IOV/multi-host 환경 고려). */

	if (attr_mask & IBV_QP_ACCESS_FLAGS) {
		if (qp_attr->qp_access_flags & IBV_ACCESS_REMOTE_READ) {
			DEVX_SET(qpc, qpc, rre, 1);
			/* [한국어] rre = Remote Read Enable. 다른 노드가 우리 메모리를 RDMA READ 가능. */
		}
		if (qp_attr->qp_access_flags & IBV_ACCESS_REMOTE_WRITE) {
			DEVX_SET(qpc, qpc, rwe, 1);
			/* [한국어] rwe = Remote Write Enable. */
		}
	}

	rc = mlx5dv_devx_qp_modify(qp->verbs_qp, in, sizeof(in), out, sizeof(out));
	/* [한국어] QP 상태 천이 명령 — 동기. */
	if (rc) {
		SPDK_ERRLOG("failed to modify qp to init, errno = %d\n", rc);
	}

	return rc;

}

/*
 * [한국어]
 * mlx5_qp_loopback_conn_init_2_rtr - QP를 INIT 상태에서 RTR(Ready To Receive)로 천이.
 *
 * RTR로 가면 응답자(=수신) 측 동작 활성화. 필요 정보: path_mtu, dest_qp_num(자기 자신),
 * rq_psn, ack_timeout, pkey_index, vhca_port_num, log_rra_max(=받을 수 있는 outstanding read/atomic),
 * min_rnr_nak. AV(Address Vector)에 fl=1을 set해 force-loopback 표시.
 *
 * log_msg_max=30: IB 스펙상 한 메시지의 최대 크기 = 2^30 바이트(=1 GiB). 본 라이브러리의 사실상 무제한.
 */
static int
mlx5_qp_loopback_conn_init_2_rtr(struct spdk_mlx5_qp *qp, struct ibv_qp_attr *qp_attr,
				 int attr_mask)
{
	uint8_t in[DEVX_ST_SZ_BYTES(init2rtr_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(init2rtr_qp_out)] = {0};
	void *qpc = DEVX_ADDR_OF(init2rtr_qp_in, in, qpc);
	int rc;

	DEVX_SET(init2rtr_qp_in, in, opcode, MLX5_CMD_OP_INIT2RTR_QP);
	DEVX_SET(init2rtr_qp_in, in, qpn, qp->hw.qp_num);

	/* 30 is the maximum value for Infiniband QPs */
	DEVX_SET(qpc, qpc, log_msg_max, 30);
	/* [한국어] 한 메시지 최대 크기 = 2^30 = 1 GiB. PRM/IB 스펙 한도. */

	/* TODO: add more attributes */
	if (attr_mask & IBV_QP_PATH_MTU) {
		DEVX_SET(qpc, qpc, mtu, qp_attr->path_mtu);
		/* [한국어] path MTU(IBV_MTU_256~4096). */
	}
	if (attr_mask & IBV_QP_DEST_QPN) {
		DEVX_SET(qpc, qpc, remote_qpn, qp_attr->dest_qp_num);
		/* [한국어] 상대 QP 번호 — loopback이라 자기 자신의 qp_num. */
	}
	if (attr_mask & IBV_QP_RQ_PSN) {
		DEVX_SET(qpc, qpc, next_rcv_psn, qp_attr->rq_psn & 0xffffff);
		/* [한국어] RQ Packet Sequence Number 초기값(24비트). */
	}
	if (attr_mask & IBV_QP_TIMEOUT)
		DEVX_SET(qpc, qpc, primary_address_path.ack_timeout,
			 qp_attr->timeout);
	if (attr_mask & IBV_QP_PKEY_INDEX)
		DEVX_SET(qpc, qpc, primary_address_path.pkey_index,
			 qp_attr->pkey_index);
	if (attr_mask & IBV_QP_PORT)
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num,
			 qp_attr->port_num);
	if (attr_mask & IBV_QP_MAX_DEST_RD_ATOMIC)
		DEVX_SET(qpc, qpc, log_rra_max,
			 spdk_u32log2(qp_attr->max_dest_rd_atomic));
		/* [한국어] receive-side responder atomic max — log2 인코딩 (16 → 4). */
	if (attr_mask & IBV_QP_MIN_RNR_TIMER) {
		DEVX_SET(qpc, qpc, min_rnr_nak, qp_attr->min_rnr_timer);
		/* [한국어] RNR NAK 발생 시 wait 시간 인코딩(0~31, PRM 표 참조). */
	}
	if (attr_mask & IBV_QP_AV) {
		DEVX_SET(qpc, qpc, primary_address_path.fl, 1);
		/* [한국어] fl(force-loopback) 비트 set — 패킷이 wire로 안 나가고 NIC 내부에서 loopback. */
	}

	rc = mlx5dv_devx_qp_modify(qp->verbs_qp, in, sizeof(in), out, sizeof(out));
	if (rc) {
		SPDK_ERRLOG("failed to modify qp to rtr with errno = %d\n", rc);
	}

	return rc;
}

/*
 * [한국어]
 * mlx5_qp_loopback_conn_rtr_2_rts - QP를 RTR에서 RTS(Ready To Send)로 천이.
 *
 * RTS는 송신 활성 상태. 필요 정보: ack_timeout, retry_cnt, sq_psn, rnr_retry, log_sra_max
 * (=내가 보낼 수 있는 outstanding read/atomic).
 */
static int
mlx5_qp_loopback_conn_rtr_2_rts(struct spdk_mlx5_qp *qp, struct ibv_qp_attr *qp_attr, int attr_mask)
{
	uint8_t in[DEVX_ST_SZ_BYTES(rtr2rts_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(rtr2rts_qp_out)] = {0};
	void *qpc = DEVX_ADDR_OF(rtr2rts_qp_in, in, qpc);
	int rc;

	DEVX_SET(rtr2rts_qp_in, in, opcode, MLX5_CMD_OP_RTR2RTS_QP);
	DEVX_SET(rtr2rts_qp_in, in, qpn, qp->hw.qp_num);

	if (attr_mask & IBV_QP_TIMEOUT)
		DEVX_SET(qpc, qpc, primary_address_path.ack_timeout,
			 qp_attr->timeout);
	if (attr_mask & IBV_QP_RETRY_CNT) {
		DEVX_SET(qpc, qpc, retry_count, qp_attr->retry_cnt);
		/* [한국어] transport 재시도 횟수. */
	}
	if (attr_mask & IBV_QP_SQ_PSN) {
		DEVX_SET(qpc, qpc, next_send_psn, qp_attr->sq_psn & 0xffffff);
		/* [한국어] SQ PSN 초기값(24비트). */
	}
	if (attr_mask & IBV_QP_RNR_RETRY) {
		DEVX_SET(qpc, qpc, rnr_retry, qp_attr->rnr_retry);
		/* [한국어] RNR 재시도 횟수(0~7, 7=infinite). */
	}
	if (attr_mask & IBV_QP_MAX_QP_RD_ATOMIC)
		DEVX_SET(qpc, qpc, log_sra_max,
			 spdk_u32log2(qp_attr->max_rd_atomic));
		/* [한국어] send-side requestor atomic max — log2 인코딩. */

	rc = mlx5dv_devx_qp_modify(qp->verbs_qp, in, sizeof(in), out, sizeof(out));
	if (rc) {
		SPDK_ERRLOG("failed to modify qp to rts with errno = %d\n", rc);
	}

	return rc;
}


/*
 * [한국어]
 * mlx5_qp_loopback_conn - QP를 RST → INIT → RTR → RTS까지 순차 천이.
 *
 * @qp: QP. @caps: connect 시 수집한 디바이스/포트 cap.
 *
 * 1단계 RST→INIT: pkey/port + RW/RR access flag.
 * 2단계 INIT→RTR: dest_qp_num=자기 qpn, mtu, rq_psn, max_dest_rd_atomic, min_rnr_timer, AV(fl=1).
 * 3단계 RTR→RTS: ack_timeout, retry_cnt, sq_psn, rnr_retry, max_rd_atomic.
 *
 * verbs ibv_modify_qp는 kernel을 거쳐야 하지만 여기서는 모두 devx로 직접 수행 — kernel이 본 QP가
 * RTR/RTS인 것을 모르므로 한 번 devx로 시작하면 끝까지 devx로 가야 일관성 유지.
 */
static int
mlx5_qp_loopback_conn(struct spdk_mlx5_qp *qp, struct mlx5_qp_conn_caps *caps)
{
	struct ibv_qp_attr qp_attr = {};
	int rc, attr_mask = IBV_QP_STATE |
			    IBV_QP_PKEY_INDEX |
			    IBV_QP_PORT |
			    IBV_QP_ACCESS_FLAGS;

	qp_attr.qp_state = IBV_QPS_INIT;
	qp_attr.pkey_index = caps->pkey_idx;
	qp_attr.port_num = caps->port;
	qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
	/* [한국어] 1단계 입력 셋업 — 원격(자기 자신)의 RW/RR 둘 다 허용. */

	rc = mlx5_qp_loopback_conn_rts_2_init(qp, &qp_attr, attr_mask);
	/* [한국어] RST → INIT. */
	if (rc) {
		return rc;
	}

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.dest_qp_num = qp->hw.qp_num;
	/* [한국어] loopback이라 dest = 자기 자신. */
	qp_attr.qp_state = IBV_QPS_RTR;
	qp_attr.path_mtu = caps->mtu;
	qp_attr.rq_psn = MLX5_QP_RQ_PSN;
	qp_attr.max_dest_rd_atomic = MLX5_QP_MAX_DEST_RD_ATOMIC;
	qp_attr.min_rnr_timer = MLX5_QP_RNR_TIMER;
	qp_attr.ah_attr.port_num = caps->port;
	qp_attr.ah_attr.grh.hop_limit = MLX5_QP_HOP_LIMIT;
	/* [한국어] 2단계 입력 셋업. ah_attr는 actually 사용 안 되지만(AV에 fl=1만 set) 형식상 채움. */

	attr_mask = IBV_QP_STATE              |
		    IBV_QP_AV                 |
		    IBV_QP_PATH_MTU           |
		    IBV_QP_DEST_QPN           |
		    IBV_QP_RQ_PSN             |
		    IBV_QP_MAX_DEST_RD_ATOMIC |
		    IBV_QP_MIN_RNR_TIMER;

	rc = mlx5_qp_loopback_conn_init_2_rtr(qp, &qp_attr, attr_mask);
	/* [한국어] INIT → RTR. */
	if (rc) {
		return rc;
	}

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_RTS;
	qp_attr.timeout = MLX5_QP_TIMEOUT;
	qp_attr.retry_cnt = MLX5_QP_RETRY_COUNT;
	qp_attr.sq_psn = MLX5_QP_SQ_PSN;
	qp_attr.rnr_retry = MLX5_QP_RNR_RETRY;
	qp_attr.max_rd_atomic = MLX5_QP_MAX_RD_ATOMIC;
	attr_mask = IBV_QP_STATE              |
		    IBV_QP_TIMEOUT            |
		    IBV_QP_RETRY_CNT          |
		    IBV_QP_RNR_RETRY          |
		    IBV_QP_SQ_PSN             |
		    IBV_QP_MAX_QP_RD_ATOMIC;
	/* once QPs were moved to RTR using devx, they must also move to RTS
	 * using devx since kernel doesn't know QPs are on RTR state
	 */
	/* [한국어] 영문 원주석: kernel은 QP가 devx로 RTR된 사실을 모르므로 RTS도 devx로 처리해야 함. */
	return mlx5_qp_loopback_conn_rtr_2_rts(qp, &qp_attr, attr_mask);
	/* [한국어] RTR → RTS. */
}

/*
 * [한국어]
 * mlx5_qp_connect - connect 진입점: cap 수집 + force-loopback 가능 검사 + 상태머신 천이.
 *
 * @qp: connect 대상.
 * @return: 0 성공, -ENOTSUP(force-loopback 미지원), 그 외 음수 errno.
 *
 * 1. port/pkey 조회.
 * 2. HCA cap 조회 (RoCE/fl_*).
 * 3. port 검사 (IB GRH 등).
 * 4. force-loopback 가능 조건 충족 검사:
 *    - port가 IB이거나
 *    - resources_on_nvme_emulation_manager + (RoCE 활성에 따른 fl 비트)
 *    그렇지 않은 경우, NVMe emulation 자원이라면 ENOTSUP, 아니면 통과(일반 RoCE? 본 코드 흐름상
 *    fall through되지만 첫 if 빈 블록은 의도적인 "조건 충족 시 아무것도 안 함" 패턴).
 * 5. mlx5_qp_loopback_conn으로 RST→RTS 천이.
 *
 * 호출 체인: mlx5_qp_init → [mlx5_qp_connect] → mlx5_qp_loopback_conn
 */
static int
mlx5_qp_connect(struct spdk_mlx5_qp *qp)
{
	struct mlx5_qp_conn_caps conn_caps = {};
	struct ibv_context *context = qp->verbs_qp->context;
	int rc;

	rc = mlx5_qp_get_port_pkey_idx(qp, &conn_caps);
	/* [한국어] QP가 묶인 포트 번호와 pkey index 조회. */
	if (rc) {
		return rc;
	}
	rc = mlx5_fill_qp_conn_caps(context, &conn_caps);
	/* [한국어] HCA 전역 RoCE/fl cap 조회. */
	if (rc) {
		return rc;
	}
	rc = mlx5_check_port(context, &conn_caps);
	/* [한국어] 포트 link layer + MTU 결정. */
	if (rc) {
		return rc;
	}

	/* Check if force-loopback is supported */
	if (conn_caps.port_ib_enabled || (conn_caps.resources_on_nvme_emulation_manager &&
					  ((conn_caps.roce_enabled && conn_caps.fl_when_roce_enabled) ||
					   (!conn_caps.roce_enabled && conn_caps.fl_when_roce_disabled)))) {
		/* [한국어] force-loopback 가능 — IB 포트이거나 emul 매니저+fl 비트 조합 충족.
		 * 빈 블록은 "조건 충족 시 그냥 통과" 패턴 — 다음 mlx5_qp_loopback_conn 호출로 진행. */
	} else if (conn_caps.resources_on_nvme_emulation_manager) {
		SPDK_ERRLOG("Force-loopback QP is not supported. Cannot create queue.\n");
		return -ENOTSUP;
		/* [한국어] emul 매니저 자원이지만 fl 비트가 없음 → 환경적 미지원. */
	}

	return mlx5_qp_loopback_conn(qp, &conn_caps);
	/* [한국어] 실제 상태 천이. */
}

/*
 * [한국어]
 * mlx5_cq_remove_qp - CQ의 2D LUT에서 QP를 제거.
 *
 * qp_num 상위 12비트로 lower table 슬롯, 하위 12비트로 lower table 인덱스 결정.
 * lower table의 마지막 QP가 제거되면 lower table 자체를 free.
 *
 * 호출 체인: spdk_mlx5_qp_destroy → [mlx5_cq_remove_qp]
 */
static void
mlx5_cq_remove_qp(struct spdk_mlx5_cq *cq, struct spdk_mlx5_qp *qp)
{
	uint32_t qpn_upper = qp->hw.qp_num >> SPDK_MLX5_QP_NUM_UPPER_SHIFT;
	/* [한국어] 상위 12비트 — outer LUT 슬롯. */
	uint32_t qpn_mask = qp->hw.qp_num & SPDK_MLX5_QP_NUM_LOWER_MASK;
	/* [한국어] 하위 12비트 — inner LUT 인덱스. */

	if (cq->qps[qpn_upper].count) {
		/* [한국어] 정상 경로 — 해당 outer 슬롯에 inner table 존재. */
		cq->qps[qpn_upper].table[qpn_mask] = NULL;
		/* [한국어] inner LUT에서 매핑 제거. */
		cq->qps[qpn_upper].count--;
		/* [한국어] outer 슬롯의 카운트 감소. */
		cq->qps_count--;
		/* [한국어] CQ 전체 QP 카운트 감소. */
		if (!cq->qps[qpn_upper].count) {
			free(cq->qps[qpn_upper].table);
			/* [한국어] inner LUT가 비었으면 메모리 해제 — sparse 유지. */
		}
	} else {
		/* [한국어] 등록되지 않은 QP 제거 시도 — 호출자 버그. */
		SPDK_ERRLOG("incorrect count, cq %p, qp %p, qpn %u\n", cq, qp, qp->hw.qp_num);
		SPDK_UNREACHABLE();
		/* [한국어] 디버그 빌드: abort. release: __builtin_unreachable로 옵티마이저 힌트. */
	}
}

/*
 * [한국어]
 * mlx5_cq_add_qp - CQ의 2D LUT에 QP 등록.
 *
 * 동일 outer 슬롯의 첫 등록이면 inner LUT을 calloc(SPDK_MLX5_QP_NUM_LUT_SIZE × ptr) 할당.
 * 충돌(이미 등록됨)은 호출자 버그 — UNREACHABLE.
 *
 * 호출 체인: spdk_mlx5_qp_create → [mlx5_cq_add_qp]
 */
static int
mlx5_cq_add_qp(struct spdk_mlx5_cq *cq, struct spdk_mlx5_qp *qp)
{
	uint32_t qpn_upper = qp->hw.qp_num >> SPDK_MLX5_QP_NUM_UPPER_SHIFT;
	uint32_t qpn_mask = qp->hw.qp_num & SPDK_MLX5_QP_NUM_LOWER_MASK;

	if (!cq->qps[qpn_upper].count) {
		/* [한국어] 이 outer 슬롯의 첫 등록 — inner LUT 4096 엔트리 할당. */
		cq->qps[qpn_upper].table = calloc(SPDK_MLX5_QP_NUM_LUT_SIZE, sizeof(*cq->qps[qpn_upper].table));
		if (!cq->qps[qpn_upper].table) {
			return -ENOMEM;
		}
	}
	if (cq->qps[qpn_upper].table[qpn_mask]) {
		/* [한국어] 이미 등록된 슬롯에 또 등록 시도 — qp_num이 NIC 내부에서 유일해야 하므로 발생 불가. */
		SPDK_ERRLOG("incorrect entry, cq %p, qp %p, qpn %u\n", cq, qp, qp->hw.qp_num);
		SPDK_UNREACHABLE();
	}
	cq->qps[qpn_upper].count++;
	/* [한국어] outer 슬롯 카운트 증가. */
	cq->qps_count++;
	/* [한국어] CQ 전체 카운트 증가. */
	cq->qps[qpn_upper].table[qpn_mask] = qp;
	/* [한국어] inner LUT에 QP 포인터 저장. */

	return 0;
}

/*
 * [한국어]
 * spdk_mlx5_cq_create - CQ 생성 공개 API.
 *
 * @pd: 소속 PD. @cq_attr: 사용자 속성. @cq_out: 출력.
 * @return: 0 성공, 음수 errno.
 *
 * calloc → mlx5_cq_init. init이 ibv_destroy_cq + free(cq)도 수행하므로 호출자는 init 실패 시
 * cq를 다시 free하지 않아야 함(원본 코드 패턴).
 */
int
spdk_mlx5_cq_create(struct ibv_pd *pd, struct spdk_mlx5_cq_attr *cq_attr,
		    struct spdk_mlx5_cq **cq_out)
{
	struct spdk_mlx5_cq *cq;
	int rc;

	cq = calloc(1, sizeof(*cq));
	/* [한국어] CQ 객체 + 4096 엔트리 outer LUT 한 번에 0-초기화. 큰 zero-fill이라 비용 큰 편. */
	if (!cq) {
		return -ENOMEM;
	}

	rc = mlx5_cq_init(pd, cq_attr, cq);
	if (rc) {
		free(cq);
		/* [한국어] 주의: mlx5_cq_init의 일부 실패 경로에서 이미 free(cq)를 호출하므로
		 * double-free 위험. 원본 코드 그대로 유지하나 향후 정리 필요. */
		return rc;
	}
	*cq_out = cq;

	return 0;
}

/*
 * [한국어]
 * spdk_mlx5_cq_destroy - CQ 파괴 공개 API.
 *
 * 바인딩된 QP가 남아있으면 EBUSY — 호출자가 모든 QP를 먼저 destroy해야 함.
 */
int
spdk_mlx5_cq_destroy(struct spdk_mlx5_cq *cq)
{
	if (cq->qps_count) {
		SPDK_ERRLOG("CQ has %u bound QPs\n", cq->qps_count);
		return -EBUSY;
		/* [한국어] 안전 검사 — 활성 QP가 있는 CQ를 파괴하면 dangling 참조. */
	}

	mlx5_cq_deinit(cq);
	/* [한국어] verbs CQ 파괴. */
	free(cq);
	/* [한국어] CQ 객체 메모리 해제. */

	return 0;
}

/*
 * [한국어]
 * spdk_mlx5_qp_create - QP 생성 공개 API.
 *
 * @pd/@cq/@qp_attr/@qp_out: 사용자 입력/출력.
 * @return: 0 성공, 음수 errno.
 *
 * 흐름: calloc → mlx5_qp_init(=create + init_obj + completions + connect) → cq 바인딩 → CQ LUT 등록.
 * LUT 등록 실패 시 mlx5_qp_destroy로 자원 정리.
 */
int
spdk_mlx5_qp_create(struct ibv_pd *pd, struct spdk_mlx5_cq *cq, struct spdk_mlx5_qp_attr *qp_attr,
		    struct spdk_mlx5_qp **qp_out)
{
	int rc;
	struct spdk_mlx5_qp *qp;

	qp = calloc(1, sizeof(*qp));
	/* [한국어] QP 객체 0-초기화 할당. */
	if (!qp) {
		return -ENOMEM;
	}

	rc = mlx5_qp_init(pd, qp_attr, cq->verbs_cq, qp);
	/* [한국어] QP 생성 + RST→RTS 천이까지 한 번에. */
	if (rc) {
		free(qp);
		return rc;
	}
	qp->cq = cq;
	/* [한국어] CQ 역참조 보관 — destroy 시 LUT 제거에 사용. */
	rc = mlx5_cq_add_qp(cq, qp);
	/* [한국어] CQ의 2D LUT에 QP 등록 — polling 시 qp_num → qp 매핑 가능. */
	if (rc) {
		mlx5_qp_destroy(qp);
		free(qp);
		return rc;
	}
	*qp_out = qp;

	return 0;
}

/*
 * [한국어]
 * spdk_mlx5_qp_destroy - QP 파괴 공개 API.
 *
 * LUT 제거 → verbs_qp 파괴 + completions 해제 → qp 자체 해제.
 */
void
spdk_mlx5_qp_destroy(struct spdk_mlx5_qp *qp)
{
	mlx5_cq_remove_qp(qp->cq, qp);
	/* [한국어] 먼저 LUT에서 제거 — 이후 들어오는 CQE가 매핑되지 못해도 정상(에러 처리). */
	mlx5_qp_destroy(qp);
	/* [한국어] verbs QP 파괴 + completions free. */
	free(qp);
}

/*
 * [한국어]
 * spdk_mlx5_qp_set_error_state - QP를 ERROR 상태로 강제 천이.
 *
 * 사용 시나리오: graceful shutdown에서 잔여 outstanding WQE를 모두 flush(syndrome=WR_FLUSH_ERR로
 * CQE 발행)하여 호스트-측 자원 회수를 유도. ibv_modify_qp는 kernel 경로지만 ERROR 천이는 어디서든
 * 가능하므로 verbs API 사용.
 */
int
spdk_mlx5_qp_set_error_state(struct spdk_mlx5_qp *qp)
{
	struct ibv_qp_attr attr = {
		.qp_state = IBV_QPS_ERR,
		/* [한국어] 목표 상태 = ERROR. */
	};

	return ibv_modify_qp(qp->verbs_qp, &attr, IBV_QP_STATE);
	/* [한국어] kernel verbs 경로로 상태 변경. 천이 후 NIC이 잔여 WQE에 대해 flush CQE 발행. */
}

/*
 * [한국어]
 * spdk_mlx5_qp_get_verbs_qp - 외부에 ibv_qp 핸들 노출.
 *
 * 일부 외부 라이브러리/디버그 도구가 ibv_qp 포인터를 요구할 때 사용. 호출자는 핸들의 lifetime이
 * spdk_mlx5_qp 객체에 종속됨을 인지해야 함.
 */
struct ibv_qp *
spdk_mlx5_qp_get_verbs_qp(struct spdk_mlx5_qp *qp)
{
	return qp->verbs_qp;
}
