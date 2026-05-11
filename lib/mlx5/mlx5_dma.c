/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] mlx5 DMA / RDMA write·read WQE 게시 및 CQE 폴링 구현 (mlx5_dma.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 lib/mlx5 라이브러리의 *데이터 평면(data-path)* 구현을 담당한다. 구체적으로는
 * (1) RDMA WRITE/READ Work Queue Element(WQE)를 SQ 버퍼에 직접 작성하여 NIC에 게시하고,
 * (2) Completion Queue(CQ)를 polling하여 완료된 WR의 wr_id/상태를 호출자에게 보고하며,
 * (3) error CQE의 syndrome 코드를 사람이 읽을 수 있는 진단 메시지로 디코드한다. 모든 함수가
 * libmlx5dv direct-verbs 기반으로, 일반 ibv_post_send/ibv_poll_cq를 우회하여 시스템 콜과 추상화
 * 비용을 제거한 핫-패스이다. 또한 SQ ring buffer의 끝-경계를 넘는 large WQE를 위한 wrap-around
 * 작성 경로(`mlx5_dma_xfer_wrap_around`)도 포함한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 호출 체인은 다음과 같다:
 *   상위 모듈(예: NVMe-oF RDMA initiator, accel_mlx5, encrypted bdev) →
 *   spdk_mlx5_qp_rdma_write/read() / spdk_mlx5_qp_complete_send() / spdk_mlx5_cq_poll_completions()
 *     → 본 파일의 mlx5_qp_rdma_op() → mlx5_dma_xfer_full() 또는 mlx5_dma_xfer_wrap_around()
 *     → mlx5_priv.h의 인라인 헬퍼(mlx5_qp_get_wqe_bb, mlx5_set_ctrl_seg, mlx5_qp_wqe_submit,
 *       mlx5_ring_tx_db) → libmlx5dv → ConnectX NIC 하드웨어
 * 호스트 유저스페이스에서 단일 SPDK reactor 스레드가 polling 모드로 실행한다. NIC의 doorbell은
 * UAR(User Access Region) MMIO write로 trigger되며, 완료는 CQ 메모리에 NIC가 쓴 CQE를 호스트가
 * polling으로 읽어 처리한다. 즉 인터럽트도 시스템 콜도 없다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: mlx5_priv.h(struct spdk_mlx5_qp/cq, hw_qp/cq, 인라인 헬퍼), mlx5_ifc.h(PRM 비트 정의),
 *   spdk/log.h, spdk/util.h(SPDK_CEIL_DIV), spdk/barrier.h(memory barrier; 인라인 헬퍼에서 사용),
 *   spdk/likely.h(분기 힌트), spdk_internal/rdma_utils.h, spdk_internal/mlx5.h(공개 wrapper API).
 * - 데이터 흐름: 호출자 → ibv_sge[] 배열을 받아 SQ 슬롯에 ctrl/raddr/data segment를 채우고 →
 *   NIC가 DMA로 데이터 영역을 읽어 RDMA로 송신/원격 메모리 write/read 수행 → CQE를 CQ 메모리에 기록 →
 *   spdk_mlx5_cq_poll_completions가 CQE를 파싱해 wr_id/상태를 comp[]에 채워 호출자에게 반환.
 * - 공유 상태: spdk_mlx5_qp.completions[] 배열(슬롯 단위 wr_id/누적 unsignaled 카운트), tx_available,
 *   nonsignaled_outstanding, sq_pi/last_pi/ctrl 등이 단일 reactor 스레드 내에서 갱신됨. 락 없음.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct _mlx5_err_cqe: 에러 CQE의 64B 레이아웃(에러 syndrome, opcode/qpn, wqe_counter 등 추출).
 * - struct mlx5_sigerr_cqe: T10-DIF/CRC32C signature 에러 전용 CQE 레이아웃.
 * - mlx5_cqe_err_opcode(): 에러 CQE의 opcode를 사람이 읽을 수 있는 문자열로 변환.
 * - mlx5_cqe_err(): 에러 CQE 처리 → SPDK_WARNLOG 출력 + syndrome 반환.
 * - mlx5_dma_xfer_full() / mlx5_dma_xfer_wrap_around(): RDMA WQE를 SQ에 작성(연속 / wrap 분기).
 * - mlx5_qp_rdma_op(): bb_count 계산 → SQ 잔량 검사 → 두 작성 경로로 분기.
 * - spdk_mlx5_qp_rdma_write/read(): 공개 API. opcode를 확정하여 mlx5_qp_rdma_op 위임.
 * - mlx5_cq_poll_one(): CQ에서 다음 CQE 한 개를 읽고 owner 비트로 유효성 검사.
 * - mlx5_qp_get_comp_wr_id(): CQE의 wqe_counter로부터 보존된 wr_id 회수 + tx_available 복원.
 * - spdk_mlx5_cq_poll_completions(): CQ를 batch polling, max_completions까지 채워 반환.
 * - spdk_mlx5_qp_complete_send(): 누적된 WQE의 doorbell을 일괄 trigger(batched submit flush).
 * - mlx5_qp_dump_wqe(): DEBUG 빌드에서 SQ에 작성된 WQE를 16-DW(=64B) 단위로 hex dump.
 */

/* [한국어] lib/mlx5 내부 정의 헤더: spdk_mlx5_qp/cq, mlx5_hw_qp/cq, 인라인 헬퍼 일체. */
#include "mlx5_priv.h"
/* [한국어] PRM(Programmer's Reference Manual) 자동 생성 헤더 — opcode/CQE syndrome/HCA cap 등 정의. */
#include "mlx5_ifc.h"
/* [한국어] SPDK_DEBUGLOG / SPDK_WARNLOG / SPDK_ERRLOG 매크로. */
#include "spdk/log.h"
/* [한국어] SPDK_CEIL_DIV, SPDK_COUNTOF 등 산술 유틸. bb_count 계산에 사용. */
#include "spdk/util.h"
/* [한국어] CPU 메모리 배리어 (인라인 헬퍼 mlx5_ring_tx_db에서 사용; 본 .c는 직접 안 쓰나 의존). */
#include "spdk/barrier.h"
/* [한국어] spdk_likely / spdk_unlikely 분기 예측 힌트 — polling 핫 패스 최적화. */
#include "spdk/likely.h"

/* [한국어] SPDK 내부 RDMA 공통 유틸(spdk_rdma_utils_*) — PD/MR 캐싱 등. */
#include "spdk_internal/rdma_utils.h"
/* [한국어] SPDK lib/* 간 공유되는 mlx5 wrapper API 시그니처(spdk_mlx5_qp_rdma_write 등 선언). */
#include "spdk_internal/mlx5.h"

/* [한국어] 송신 CQ의 CQE 한 개 크기(바이트). 64B CQE 모드 가정.
 * 컴파일 시 상수로 mlx5_cq_get_cqe()에 전달되어 분기/곱셈이 인라인 시 상수 폴딩되도록 한다. */
#define MLX5_DMA_Q_TX_CQE_SIZE  64

/*
 * [한국어]
 * struct _mlx5_err_cqe - 에러 CQE의 64B 레이아웃(언더스코어 prefix는 internal-only 표기).
 *
 * NIC가 WR 처리 중 오류를 만나면 일반 CQE 자리에 이 형태로 정보를 기록한다.
 * mlx5dv는 mlx5_err_cqe라는 동일 구조를 노출할 수 있으나 본 파일은 명시적 비트 추출을 위해
 * 자체 정의를 사용한다. 모든 멀티바이트 필드는 big-endian이며 호스트는 be32toh/be16toh로 변환한다.
 */
struct _mlx5_err_cqe {
	uint8_t		rsvd0[32];
	/* [한국어] 예약/패딩 영역(상위 32B). PRM 정의의 일반 CQE 헤더 부분과 정렬되며 직접 사용하지 않음. */
	uint32_t	srqn;
	/* [한국어] Shared Receive Queue Number — 본 파일은 SRQ를 사용하지 않으므로 무시 대상. */
	uint8_t		rsvd1[16];
	/* [한국어] 예약 16B 패딩. */
	uint8_t		hw_err_synd;
	/* [한국어] HW 단(NIC ASIC)에서 발생한 syndrome 코드. SPDK_WARNLOG에 hex로 노출. */
	uint8_t		rsvd2[1];
	/* [한국어] 예약 1B. */
	uint8_t		vendor_err_synd;
	/* [한국어] Vendor-specific syndrome — Mellanox 정의 추가 코드. */
	uint8_t		syndrome;
	/* [한국어] PRM 표준 syndrome (MLX5_CQE_SYNDROME_*). 본 파일이 switch로 디코드. */
	uint32_t	s_wqe_opcode_qpn;
	/* [한국어] 상위 8b = WR opcode, 하위 24b = QP 번호. be32toh 후 분리. */
	uint16_t	wqe_counter;
	/* [한국어] 에러를 일으킨 WQE의 카운터(BE16) — be16toh로 디버그 출력. SQ 슬롯 추적용. */
	uint8_t		signature;
	/* [한국어] WQE signature 1B. */
	uint8_t		op_own;
	/* [한국어] 상위 4b = CQE opcode 종류(REQ_ERR/RESP_ERR/...), 하위 4b = owner bit 등. */
};

/*
 * [한국어]
 * struct mlx5_sigerr_cqe - T10-DIF/CRC32C signature 검증 실패 시 생성되는 CQE.
 *
 * AES-XTS encrypted bdev 또는 NVMe end-to-end PI를 사용하는 경로에서, 보호 정보(PI) 검증이
 * 실패하면 NIC이 일반 CQE 대신 이 형태로 expected/actual 값과 syndrome을 보고한다.
 * 본 파일은 정의만 두고 직접 사용하진 않으나, 향후 sig 검증 결과 핸들링 시 cast 대상이다.
 */
struct mlx5_sigerr_cqe {
	uint8_t		rsvd0[16];
	/* [한국어] 예약 헤더 16B. */
	uint32_t	expected_trans_sig;
	/* [한국어] NIC가 기대한 transient signature(BE32). */
	uint32_t	actual_trans_sig;
	/* [한국어] 실제 수신/계산된 transient signature. */
	uint32_t	expected_ref_tag;
	/* [한국어] T10-DIF reference tag 기댓값(보통 LBA 하위 32비트). */
	uint32_t	actual_ref_tag;
	/* [한국어] 실제 ref tag. */
	uint16_t	syndrome;
	/* [한국어] sig 에러 syndrome 16비트(BE) — guard/apptag/reftag 어느 부분이 어긋났는지. */
	uint8_t		sig_type;
	/* [한국어] T10-DIF / CRC32C / etc. 식별. */
	uint8_t		domain;
	/* [한국어] wire(=네트워크) / memory(=호스트) 도메인 구분. */
	uint32_t	mkey;
	/* [한국어] 검증 실패 시점의 MKEY(메모리 영역 핸들). */
	uint64_t	sig_err_offset;
	/* [한국어] 영역 내 에러 발생 바이트 오프셋(BE64). */
	uint8_t		rsvd30[14];
	/* [한국어] 예약 14B. */
	uint8_t		signature;
	/* [한국어] CQE 자체의 signature byte. */
	uint8_t		op_own;
	/* [한국어] CQE opcode + owner bit. */
};

/*
 * [한국어]
 * mlx5_cqe_err_opcode - 에러 CQE의 opcode를 사람이 읽을 수 있는 ASCII 문자열로 변환.
 *
 * @ecqe: 에러 CQE 포인터.
 * @return: 정적 문자열("RDMA_WRITE", "SEND", "RDMA_READ", ... 등). 매핑 안 되면 빈 문자열.
 *
 * SPDK_WARNLOG의 진단 메시지에 opcode 이름을 함께 포함시키기 위한 헬퍼. op_own의 상위 4비트가
 * REQ_ERR(요청자 측) 또는 RESP_ERR(응답자 측)인지에 따라 wqe_err_opcode 해석이 달라진다.
 * 컨텍스트: spdk_mlx5_cq_poll_completions의 핫-패스 *밖*에서만 호출됨(에러 분기).
 *
 * 호출 체인:
 *   spdk_mlx5_cq_poll_completions → mlx5_cqe_err → [mlx5_cqe_err_opcode]
 */
static const char *
mlx5_cqe_err_opcode(struct _mlx5_err_cqe *ecqe)
{
	uint8_t wqe_err_opcode = be32toh(ecqe->s_wqe_opcode_qpn) >> 24;
	/* [한국어] BE32 필드를 호스트 바이트오더로 변환 후 상위 8비트 추출 = WR opcode. */

	switch (ecqe->op_own >> 4) {
	/* [한국어] op_own 상위 4비트 = CQE 종류. REQ_ERR vs RESP_ERR로 분기. */
	case MLX5_CQE_REQ_ERR:
		/* [한국어] 요청자(=발신) 측 에러 — 우리가 게시한 WQE가 실패. opcode 디코드 진행. */
		switch (wqe_err_opcode) {
		case MLX5_OPCODE_RDMA_WRITE_IMM:
		case MLX5_OPCODE_RDMA_WRITE:
			/* [한국어] RDMA WRITE (with/without immediate data) — 원격 메모리에 쓰기. */
			return "RDMA_WRITE";
		case MLX5_OPCODE_SEND_IMM:
		case MLX5_OPCODE_SEND:
		case MLX5_OPCODE_SEND_INVAL:
			/* [한국어] SEND 계열 (with imm / invalidate rkey) — 원격 RQ로 메시지. */
			return "SEND";
		case MLX5_OPCODE_RDMA_READ:
			/* [한국어] RDMA READ — 원격 메모리에서 읽기. */
			return "RDMA_READ";
		case MLX5_OPCODE_ATOMIC_CS:
			/* [한국어] Atomic Compare-and-Swap (8B). */
			return "COMPARE_SWAP";
		case MLX5_OPCODE_ATOMIC_FA:
			/* [한국어] Atomic Fetch-and-Add. */
			return "FETCH_ADD";
		case MLX5_OPCODE_ATOMIC_MASKED_CS:
			/* [한국어] 마스크 기반 CS (확장 atomic). */
			return "MASKED_COMPARE_SWAP";
		case MLX5_OPCODE_ATOMIC_MASKED_FA:
			/* [한국어] 마스크 기반 FA. */
			return "MASKED_FETCH_ADD";
		case MLX5_OPCODE_MMO:
			/* [한국어] MMO opcode — Memory-to-Memory Offload. ConnectX의 GGA(General Generic Accelerator)
			 *   엔진을 통한 호스트 메모리간 DMA copy/crypto/compress 등 가속. */
			return "GGA_DMA";
		default:
			return "";
			/* [한국어] 알 수 없는 opcode → 빈 문자열. */
		}
	case MLX5_CQE_RESP_ERR:
		/* [한국어] 응답자(=수신) 측 에러 — 들어온 RECV가 실패. */
		return "RECV";
	default:
		return "";
		/* [한국어] 그 외 종류 → 빈 문자열. */
	}
}

/*
 * [한국어]
 * mlx5_cqe_err - 에러 CQE 처리: syndrome 디코드 + 진단 로그 출력 + syndrome 반환.
 *
 * @cqe: 일반 CQE 포인터(에러임이 확인된 후 _mlx5_err_cqe로 reinterpret).
 * @return: PRM syndrome 코드(IBV_WC_*가 아닌 mlx5 raw syndrome — 호출자에 그대로 전달).
 *
 * spdk_mlx5_cq_poll_completions가 opcode != MLX5_CQE_REQ로 판정한 경우 호출. WR_FLUSH는
 * QP가 ERROR 상태로 천이된 후 큐에 남아있던 WQE들의 정상 회수 경로이므로 DEBUGLOG로만 출력
 * (운영 중 정상 시나리오). 그 외는 WARNLOG로 syndrome 종류를 사람이 읽을 수 있는 문자열과 함께
 * 출력.
 *
 * 호출 체인:
 *   spdk_mlx5_cq_poll_completions → [mlx5_cqe_err] → mlx5_cqe_err_opcode
 */
static int
mlx5_cqe_err(struct mlx5_cqe64 *cqe)
{
	struct _mlx5_err_cqe *ecqe = (struct _mlx5_err_cqe *)cqe;
	/* [한국어] 64B CQE를 에러 CQE 레이아웃으로 reinterpret — 동일한 64B 영역을 다른 시야로 해석. */
	uint16_t wqe_counter;
	uint32_t qp_num = 0;
	char info[200] = {0};
	/* [한국어] WARNLOG에 들어갈 사람-친화 syndrome 문자열 버퍼. 0으로 초기화. */

	wqe_counter = be16toh(ecqe->wqe_counter);
	/* [한국어] 에러 WQE의 카운터(BE→호스트 바이트오더). SQ 슬롯 디버깅에 사용. */
	qp_num = be32toh(ecqe->s_wqe_opcode_qpn) & ((1 << 24) - 1);
	/* [한국어] s_wqe_opcode_qpn의 하위 24비트 = QP 번호. 상위 8비트는 opcode이므로 마스크로 분리. */

	if (ecqe->syndrome == MLX5_CQE_SYNDROME_WR_FLUSH_ERR) {
		/* [한국어] QP가 ERROR 상태로 천이되며 잔여 WQE들이 flush된 경우 — 정상 종료 경로.
		 * WARNLOG가 아니라 DEBUGLOG로 출력해 운영 잡음 줄임. */
		SPDK_DEBUGLOG(mlx5, "QP 0x%x wqe[%d] is flushed\n", qp_num, wqe_counter);
		return ecqe->syndrome;
		/* [한국어] flush syndrome을 그대로 호출자에 전달(ENOMEM 등 전파 가능). */
	}

	switch (ecqe->syndrome) {
	/* [한국어] PRM Table "Completion Queue Entry Syndrome" 기준 디코드. 각 case는 표준 RDMA
	 *   syndrome으로, IBV_WC_LOC_LEN_ERR 등 verbs 표준과 1:1 대응. */
	case MLX5_CQE_SYNDROME_LOCAL_LENGTH_ERR:
		snprintf(info, sizeof(info), "Local length");
		/* [한국어] WR length가 SGE 합과 불일치하거나 페이지 경계 문제. */
		break;
	case MLX5_CQE_SYNDROME_LOCAL_QP_OP_ERR:
		snprintf(info, sizeof(info), "Local QP operation");
		/* [한국어] WQE 자체 구성 오류(예: 잘못된 opcode, 잘못된 ds count). */
		break;
	case MLX5_CQE_SYNDROME_LOCAL_PROT_ERR:
		snprintf(info, sizeof(info), "Local protection");
		/* [한국어] 로컬 메모리 보호 위반(MR 권한 부족, 잘못된 lkey 등). */
		break;
	case MLX5_CQE_SYNDROME_WR_FLUSH_ERR:
		snprintf(info, sizeof(info), "WR flushed because QP in error state");
		/* [한국어] (위 if에서 이미 처리됐지만 방어적으로 유지) QP error 상태에서 자동 flush. */
		break;
	case MLX5_CQE_SYNDROME_MW_BIND_ERR:
		snprintf(info, sizeof(info), "Memory window bind");
		/* [한국어] Memory Window bind 실패 — type 1/2 MW 권한 문제. */
		break;
	case MLX5_CQE_SYNDROME_BAD_RESP_ERR:
		snprintf(info, sizeof(info), "Bad response");
		/* [한국어] 원격이 잘못된 응답 형식 — opcode mismatch 등. */
		break;
	case MLX5_CQE_SYNDROME_LOCAL_ACCESS_ERR:
		snprintf(info, sizeof(info), "Local access");
		/* [한국어] 로컬 메모리 접근 오류(MR 미등록 영역에 DMA 시도 등). */
		break;
	case MLX5_CQE_SYNDROME_REMOTE_INVAL_REQ_ERR:
		snprintf(info, sizeof(info), "Invalid request");
		/* [한국어] 원격 측에서 우리의 요청을 무효라고 거부 — opcode 미지원 등. */
		break;
	case MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR:
		snprintf(info, sizeof(info), "Remote access");
		/* [한국어] 원격 MR rkey 권한 부족 / 영역 밖 접근. */
		break;
	case MLX5_CQE_SYNDROME_REMOTE_OP_ERR:
		snprintf(info, sizeof(info), "Remote QP");
		/* [한국어] 원격 QP가 ERROR 상태로 천이됨. */
		break;
	case MLX5_CQE_SYNDROME_TRANSPORT_RETRY_EXC_ERR:
		snprintf(info, sizeof(info), "Transport retry count exceeded");
		/* [한국어] retry_cnt 초과 — 패킷 손실/응답 미수신 누적. RC QP 한계. */
		break;
	case MLX5_CQE_SYNDROME_RNR_RETRY_EXC_ERR:
		snprintf(info, sizeof(info), "Receive-no-ready retry count exceeded");
		/* [한국어] RNR(Receiver Not Ready) retry 초과 — 원격 RQ에 빈 WR 부족. */
		break;
	case MLX5_CQE_SYNDROME_REMOTE_ABORTED_ERR:
		snprintf(info, sizeof(info), "Remote side aborted");
		/* [한국어] 원격이 명시적으로 abort. */
		break;
	default:
		snprintf(info, sizeof(info), "Generic");
		/* [한국어] 위 매핑에 없는 syndrome — 향후 PRM 추가 코드 대응 시 확장. */
		break;
	}
	SPDK_WARNLOG("Error on QP 0x%x wqe[%03d]: %s (synd 0x%x vend 0x%x hw 0x%x) opcode %s\n",
		     qp_num, wqe_counter, info, ecqe->syndrome, ecqe->vendor_err_synd, ecqe->hw_err_synd,
		     mlx5_cqe_err_opcode(ecqe));
	/* [한국어] 한 줄 요약: 어느 QP의 어느 WQE가 어떤 표준/벤더/HW syndrome으로 어떤 opcode에서 실패했는지 출력. */

	return ecqe->syndrome;
	/* [한국어] PRM raw syndrome 반환 — 상위 호출자가 IBV_WC_* 변환 없이 그대로 사용자에게 보고. */
}

/*
 * DATA WQE LAYOUT:
 * ----------------------------------
 * | gen_ctrl |   rseg   |   dseg   |
 * ----------------------------------
 *   16bytes    16bytes    16bytes * sge_count
 */
/* [한국어] WQE 레이아웃 다이어그램(영문 원주석):
 *   - gen_ctrl(16B): mlx5_wqe_ctrl_seg — opcode, qp_num, ds, fm_ce_se, signature 등.
 *   - rseg(16B):    mlx5_wqe_raddr_seg — 원격 가상주소(raddr) + remote key(rkey).
 *   - dseg(16B × N): mlx5_wqe_data_seg — local addr/length/lkey (각 SGE 1개당 16B).
 *   총 WQE 크기 = 16 × (2 + sge_count) bytes. ds(data segment count) 필드 = 2 + sge_count.
 *   1 BB(Building Block) = 64B이므로 sge_count ≤ 2면 1 BB, 그 이상은 추가 BB가 필요. */

/*
 * [한국어]
 * mlx5_dma_xfer_full - SQ 끝까지 충분한 공간이 있을 때(연속) RDMA WQE를 작성·게시.
 *
 * @qp: 대상 QP.
 * @sge: scatter-gather element 배열(local addr/length/lkey).
 * @sge_count: SGE 개수.
 * @raddr: 원격 가상주소(RDMA write/read 대상).
 * @rkey: 원격 측 MR의 remote key.
 * @op: WQE opcode (MLX5_OPCODE_RDMA_WRITE / RDMA_READ).
 * @flags: SPDK CE flags(비트 [3:2]) + fm/se. mlx5_qp_fm_ce_se_update로 sigmode 변환.
 * @wr_id: 사용자 work-request ID — 완료 시 그대로 반환.
 * @bb_count: 이 WQE가 차지할 BB 수(호출자가 사전 계산).
 *
 * SQ ring buffer의 wrap-around가 발생하지 않을 때 호출되는 핫 패스. ctrl/raddr/data segment를
 * 순서대로 작성하고 mlx5_qp_wqe_submit으로 producer index를 전진시킨 뒤, mlx5_qp_set_comp로
 * 호스트-측 완료 추적을 갱신한다. doorbell은 즉시 울리지 않고 batched submit 후
 * spdk_mlx5_qp_complete_send에서 한 번에 trigger.
 *
 * 컨텍스트: 단일 reactor 스레드. 인라인 처리되어 분기/곱셈은 컴파일 타임 상수폴딩 가능.
 *
 * 호출 체인:
 *   spdk_mlx5_qp_rdma_write/read → mlx5_qp_rdma_op → [mlx5_dma_xfer_full]
 */
static inline void
mlx5_dma_xfer_full(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t sge_count, uint64_t raddr,
		   uint32_t rkey, int op, uint32_t flags, uint64_t wr_id, uint32_t bb_count)
{
	struct mlx5_hw_qp *hw_qp = &qp->hw;
	/* [한국어] raw QP 메타데이터 단축 참조. SQ 주소/카운트/pi 직접 접근. */
	struct mlx5_wqe_ctrl_seg *ctrl;
	/* [한국어] 16B control segment 포인터(opcode/qp_num/ds/fm_ce_se 등). */
	struct mlx5_wqe_raddr_seg *rseg;
	/* [한국어] 16B remote address segment(raddr+rkey). */
	struct mlx5_wqe_data_seg *dseg;
	/* [한국어] 16B data segment(들). sge_count만큼 연속 배치. */
	uint8_t fm_ce_se;
	/* [한국어] sigmode 변환된 fm/CE/se 비트 (g_mlx5_ce_map 참조). */
	uint32_t i, pi;

	fm_ce_se = mlx5_qp_fm_ce_se_update(qp, (uint8_t)flags);
	/* [한국어] 사용자 flags의 CE 비트를 sigmode(NONE/ALL/LAST)에 맞게 PRM 코드로 변환. */

	/* absolute PI value */
	pi = hw_qp->sq_pi & (hw_qp->sq_wqe_cnt - 1);
	/* [한국어] 누적 sq_pi의 하위 비트만 추출 → SQ 슬롯 인덱스(0 ~ sq_wqe_cnt-1).
	 * sq_wqe_cnt는 2의 거듭제곱이라 mask로 modulo. */
	SPDK_DEBUGLOG(mlx5, "opc %d, sge_count %u, bb_count %u, orig pi %u, fm_ce_se %x\n", op, sge_count,
		      bb_count, hw_qp->sq_pi, fm_ce_se);
	/* [한국어] 디버그: opcode/sge/bb/pi/fm_ce_se 한 줄 출력. SPDK_DEBUGLOG는 release에서 no-op. */

	ctrl = (struct mlx5_wqe_ctrl_seg *) mlx5_qp_get_wqe_bb(hw_qp);
	/* [한국어] sq_addr + slot*64 위치(다음 BB 시작 주소)를 ctrl seg로 캐스팅. WQE 첫 16B 작성 대상. */
	/* WQE size in octowords (16-byte units). DS (data segment) accounts for all the segments in the WQE
	 * as summarized in WQE construction */
	mlx5_set_ctrl_seg(ctrl, hw_qp->sq_pi, op, 0, hw_qp->qp_num, fm_ce_se, 2 + sge_count, 0, 0);
	/* [한국어] ctrl seg 16B 작성. ds=2+sge_count(=ctrl 1 + raddr 1 + data N octoword).
	 * mlx5_set_ctrl_seg는 mlx5dv_set_ctrl_seg를 BE 인코딩하여 호출. */

	rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
	/* [한국어] ctrl 다음 16B = raddr seg 시작. 포인터 산술 +1 = +16B(struct 크기). */
	rseg->raddr = htobe64(raddr);
	/* [한국어] 원격 가상주소를 BE64로 인코딩 — NIC PRM이 BE 정렬 강제. */
	rseg->rkey  = htobe32(rkey);
	/* [한국어] 원격 MR의 rkey를 BE32로 인코딩. */
	rseg->reserved = 0;
	/* [한국어] 예약 필드는 반드시 0. */

	dseg = (struct mlx5_wqe_data_seg *)(rseg + 1);
	/* [한국어] raddr 다음 16B = 첫 data seg 시작. */
	for (i = 0; i < sge_count; i++) {
		mlx5dv_set_data_seg(dseg, sge[i].length, sge[i].lkey, sge[i].addr);
		/* [한국어] mlx5dv 헬퍼로 16B data seg 작성: length/lkey/addr를 BE 인코딩.
		 * NIC이 이 SGE를 따라 호스트 메모리에서 DMA로 데이터를 가져간다(write) 또는 채워준다(read). */
		dseg = dseg + 1;
		/* [한국어] 다음 SGE를 위해 16B 전진. */
	}

	mlx5_qp_wqe_submit(qp, ctrl, bb_count, pi);
	/* [한국어] sq_pi += bb_count, last_pi/ctrl 추적. doorbell은 아직 안 울림. */

	mlx5_qp_set_comp(qp, pi, wr_id, fm_ce_se, bb_count);
	/* [한국어] 호스트-측 completion 엔트리 갱신: wr_id 보존 + signaled/unsignaled 누적 처리. */
	assert(qp->tx_available >= bb_count);
	/* [한국어] 디버그 빌드: 잔량 부족 호출은 호출자(mlx5_qp_rdma_op)가 사전 검증해야 함. */
	qp->tx_available -= bb_count;
	/* [한국어] SQ 잔량을 차감. CQE 도착 시 mlx5_qp_get_comp_wr_id가 다시 회수. */
}

/*
 * [한국어]
 * mlx5_dma_xfer_wrap_around - SQ ring 끝-경계를 넘어가는 large WQE를 분할 작성.
 *
 * 매개변수는 mlx5_dma_xfer_full과 동일하나, 차이점은 data segment 작성 중 SQ 끝에 도달하면
 * 다시 SQ 시작 주소로 wrap해 계속 작성한다는 점이다. ctrl/raddr seg는 기본 가정상 첫 BB 안에
 * 들어가지만 sge_count가 많으면 dseg가 ring을 넘어갈 수 있다.
 *
 * to_end 변수가 SQ 끝까지의 남은 바이트를 추적하며, 각 segment 작성 후 16씩 차감한다.
 * to_end == 0이 되면 dseg를 hw_qp->sq_addr로 리셋하여 ring 시작에서 이어 쓴다.
 *
 * 호출 체인:
 *   spdk_mlx5_qp_rdma_write/read → mlx5_qp_rdma_op → [mlx5_dma_xfer_wrap_around]
 */
static inline void
mlx5_dma_xfer_wrap_around(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t sge_count,
			  uint64_t raddr, uint32_t rkey, int op, uint32_t flags, uint64_t wr_id, uint32_t bb_count)
{
	struct mlx5_hw_qp *hw_qp = &qp->hw;
	/* [한국어] raw QP 메타데이터. */
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_raddr_seg *rseg;
	struct mlx5_wqe_data_seg *dseg;
	uint8_t fm_ce_se;
	uint32_t i, to_end, pi;
	/* [한국어] to_end: 현재 작성 위치에서 SQ 끝까지 남은 바이트. wrap 판정 기준. */

	fm_ce_se = mlx5_qp_fm_ce_se_update(qp, (uint8_t)flags);
	/* [한국어] CE 비트 sigmode 변환. */

	/* absolute PI value */
	pi = hw_qp->sq_pi & (hw_qp->sq_wqe_cnt - 1);
	/* [한국어] 슬롯 인덱스. */
	SPDK_DEBUGLOG(mlx5, "opc %d, sge_count %u, bb_count %u, orig pi %u, fm_ce_se %x\n", op, sge_count,
		      bb_count, pi, fm_ce_se);
	/* [한국어] 디버그 출력. */

	to_end = (hw_qp->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	/* [한국어] 현재 슬롯에서 SQ 끝까지 남은 바이트 = (남은 슬롯 수) × 64B. */
	ctrl = (struct mlx5_wqe_ctrl_seg *) mlx5_qp_get_wqe_bb(hw_qp);
	/* WQE size in octowords (16-byte units). DS (data segment) accounts for all the segments in the WQE
	 * as summarized in WQE construction */
	mlx5_set_ctrl_seg(ctrl, hw_qp->sq_pi, op, 0, hw_qp->qp_num, fm_ce_se, 2 + sge_count, 0, 0);
	/* [한국어] ctrl seg 16B 작성. */
	to_end -= sizeof(struct mlx5_wqe_ctrl_seg); /* 16 bytes */
	/* [한국어] ctrl seg 16B 소비 → 끝까지 거리 16B 차감. */

	rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
	/* [한국어] 다음 16B 위치 = raddr seg. */
	rseg->raddr = htobe64(raddr);
	/* [한국어] 원격 주소 BE64. */
	rseg->rkey  = htobe32(rkey);
	/* [한국어] 원격 rkey BE32. */
	rseg->reserved = 0;
	/* [한국어] 예약. */
	to_end -= sizeof(struct mlx5_wqe_raddr_seg); /* 16 bytes */
	/* [한국어] raddr seg 16B 소비. */

	dseg = (struct mlx5_wqe_data_seg *)(rseg + 1);
	/* [한국어] 첫 data seg 위치. */
	for (i = 0; i < sge_count; i++) {
		mlx5dv_set_data_seg(dseg, sge[i].length, sge[i].lkey, sge[i].addr);
		/* [한국어] data seg 16B 작성 (length/lkey/addr BE 인코딩). */
		to_end -= sizeof(struct mlx5_wqe_data_seg); /* 16 bytes */
		/* [한국어] 16B 소비. */
		if (to_end != 0) {
			dseg = dseg + 1;
			/* [한국어] 끝 안 도달 → 다음 16B로 정상 전진. */
		} else {
			/* Start from the beginning of SQ */
			/* [한국어] SQ 끝 도달 → ring wrap. */
			dseg = (struct mlx5_wqe_data_seg *)(hw_qp->sq_addr);
			/* [한국어] dseg를 SQ 시작 주소로 리셋. */
			to_end = hw_qp->sq_wqe_cnt * MLX5_SEND_WQE_BB;
			/* [한국어] 끝까지 거리를 다시 SQ 전체 크기로. */
		}
	}

	mlx5_qp_wqe_submit(qp, ctrl, bb_count, pi);
	/* [한국어] producer index 전진 + ctrl/last_pi 갱신. */

	mlx5_qp_set_comp(qp, pi, wr_id, fm_ce_se, bb_count);
	/* [한국어] 호스트 completion 엔트리 갱신. */
	assert(qp->tx_available >= bb_count);
	qp->tx_available -= bb_count;
	/* [한국어] SQ 잔량 차감. */
}

/*
 * [한국어]
 * mlx5_qp_rdma_op - RDMA WRITE/READ 공통 진입점: bb_count 계산 + wrap 분기.
 *
 * @qp: QP. @sge/@sge_count: SGE 배열. @dstaddr: 원격 가상주소. @rkey: 원격 rkey.
 * @wrid: wr_id. @flags: SPDK CE/fence flags. @op: opcode.
 * @return: 0 성공, -ENOMEM(SQ 잔량 부족) 또는 -E2BIG(sge_count 초과).
 *
 * BB(64B)당 ctrl 1 + raddr 1 + data 2 = 4 octoword(=64B). 따라서 sge_count ≤ 2면 1 BB,
 * 3 이상이면 추가로 ceil((sge_count-2)/4) BB가 필요(BB당 4개의 dseg). 잔량 검사 후 SQ 끝까지
 * 충분하면 full 경로, 아니면 wrap_around 경로로 분기.
 *
 * 호출 체인:
 *   spdk_mlx5_qp_rdma_write/read → [mlx5_qp_rdma_op] → mlx5_dma_xfer_full / wrap_around
 */
static inline int
mlx5_qp_rdma_op(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t sge_count, uint64_t dstaddr,
		uint32_t rkey, uint64_t wrid, uint32_t flags, int op)
{
	struct mlx5_hw_qp *hw_qp = &qp->hw;
	/* [한국어] raw QP 메타데이터 단축 참조. */
	uint32_t to_end, pi, bb_count;

	/* One bb (building block) is 64 bytes - 4 octowords
	 * It can hold control segment + raddr segment + 2 sge segments.
	 * If sge_count (data segments) is bigger than 2 then we consume additional bb */
	bb_count = (sge_count <= 2) ? 1 : 1 + SPDK_CEIL_DIV(sge_count - 2, 4);
	/* [한국어] WQE가 차지할 BB 수 계산.
	 * sge ≤ 2: 1 BB(ctrl+raddr+2 dseg = 64B).
	 * sge > 2: 첫 1 BB + (남은 dseg 수 / 4 올림) BB. 한 BB당 dseg 4개 들어감. */

	if (spdk_unlikely(bb_count > qp->tx_available)) {
		/* [한국어] SQ 슬롯 잔량 부족 — 호출자가 polling으로 자리 비울 때까지 backoff 필요. */
		return -ENOMEM;
	}
	if (spdk_unlikely(sge_count > qp->max_send_sge)) {
		/* [한국어] QP 생성 시 cap.max_send_sge로 약속한 한도 초과 — 단편화 등 호출자 책임. */
		return -E2BIG;
	}
	pi = hw_qp->sq_pi & (hw_qp->sq_wqe_cnt - 1);
	/* [한국어] 슬롯 인덱스. */
	to_end = (hw_qp->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	/* [한국어] 현재 위치에서 SQ 끝까지 남은 바이트. wrap 분기 판정 기준. */

	if (spdk_likely(to_end >= bb_count * MLX5_SEND_WQE_BB)) {
		/* [한국어] 정상 경로(연속 BB 작성 가능). 핫 패스 — likely 힌트. */
		mlx5_dma_xfer_full(qp, sge, sge_count, dstaddr, rkey, op, flags, wrid, bb_count);
	} else {
		/* [한국어] WQE가 SQ 끝을 넘어감 → wrap-around 작성. */
		mlx5_dma_xfer_wrap_around(qp, sge, sge_count, dstaddr, rkey, op, flags, wrid, bb_count);
	}

	return 0;
	/* [한국어] 게시 성공 — doorbell은 spdk_mlx5_qp_complete_send 호출 시 trigger. */
}

/*
 * [한국어]
 * spdk_mlx5_qp_rdma_write - RDMA WRITE WQE 게시 공개 API.
 *
 * @qp/@sge/@sge_count/@dstaddr/@rkey/@wrid/@flags: 위 mlx5_qp_rdma_op 참조.
 * @return: 0 성공 / -ENOMEM / -E2BIG.
 *
 * RDMA WRITE는 원격 메모리에 우리 데이터를 쓰는 단방향 op로 응답이 필요 없다(immediate 없는 경우).
 * NVMe-oF RDMA initiator나 storage offload data path에서 사용.
 *
 * 호출 체인:
 *   상위 모듈(NVMe-oF, accel_mlx5 등) → [spdk_mlx5_qp_rdma_write] → mlx5_qp_rdma_op
 */
int
spdk_mlx5_qp_rdma_write(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t sge_count,
			uint64_t dstaddr, uint32_t rkey, uint64_t wrid, uint32_t flags)
{
	return mlx5_qp_rdma_op(qp, sge, sge_count, dstaddr, rkey, wrid, flags, MLX5_OPCODE_RDMA_WRITE);
	/* [한국어] opcode를 RDMA_WRITE로 고정하여 공통 처리 위임. */
}

/*
 * [한국어]
 * spdk_mlx5_qp_rdma_read - RDMA READ WQE 게시 공개 API.
 *
 * RDMA READ는 원격 메모리에서 데이터를 우리 쪽으로 가져온다. 원격 응답이 필요하므로
 * RC QP 한정으로 동작. SGE는 *수신* 버퍼를 가리킨다.
 *
 * 호출 체인:
 *   상위 모듈 → [spdk_mlx5_qp_rdma_read] → mlx5_qp_rdma_op
 */
int
spdk_mlx5_qp_rdma_read(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t sge_count,
		       uint64_t dstaddr, uint32_t rkey, uint64_t wrid, uint32_t flags)
{
	return mlx5_qp_rdma_op(qp, sge, sge_count, dstaddr, rkey, wrid, flags, MLX5_OPCODE_RDMA_READ);
	/* [한국어] opcode를 RDMA_READ로 고정. */
}

/* polling start */
/* [한국어] 이하 polling 영역: CQ에서 CQE를 한 개씩 꺼내 wr_id/상태로 변환하는 함수들. */

/*
 * [한국어]
 * mlx5_qp_update_comp - SIG_LAST 모드에서 마지막 WQE의 누적 unsignaled 완료 수를 기록.
 *
 * @qp: QP.
 *
 * SIG_LAST 모드의 동작: 사용자가 일련의 WQE를 게시하는 동안 CE 비트는 모두 NO_FLUSH로 보내고,
 * spdk_mlx5_qp_complete_send 시점에 마지막 ctrl seg의 fm_ce_se만 CQ_UPDATE로 덮어쓴다. 이때
 * completions[last_pi].completions에 누적 unsignaled 수를 기록해 CQE가 도착하면 한 번에 회수.
 *
 * 호출 체인:
 *   spdk_mlx5_qp_complete_send → mlx5_qp_tx_complete → [mlx5_qp_update_comp]
 */
static inline void
mlx5_qp_update_comp(struct spdk_mlx5_qp *qp)
{
	qp->completions[qp->last_pi].completions = qp->nonsignaled_outstanding;
	/* [한국어] last_pi 슬롯에 누적 unsignaled WQE 수를 기록. CQE 도착 시 tx_available 회수량. */
	qp->nonsignaled_outstanding = 0;
	/* [한국어] 누적 카운터 리셋. */
}

/*
 * [한국어]
 * mlx5_qp_tx_complete - 누적된 WQE의 doorbell을 trigger (batched submit flush).
 *
 * @qp: QP.
 *
 * SIG_LAST 모드면 마지막 ctrl seg의 fm_ce_se에서 기존 CE 비트를 클리어하고 CQ_UPDATE로 덮은 뒤
 * mlx5_qp_update_comp로 호스트-측 카운터를 정리한다. 그 후 mlx5_ring_tx_db()로 DBR 갱신 + UAR
 * (BlueFlame) MMIO write를 발행해 NIC을 깨운다.
 *
 * 호출 체인:
 *   spdk_mlx5_qp_complete_send → [mlx5_qp_tx_complete] → mlx5_ring_tx_db
 */
static inline void
mlx5_qp_tx_complete(struct spdk_mlx5_qp *qp)
{
	if (qp->sigmode == SPDK_MLX5_QP_SIG_LAST) {
		/* [한국어] SIG_LAST 모드 한정 처리: 마지막 WQE에만 lazy하게 CQ_UPDATE 비트 set. */
		qp->ctrl->fm_ce_se &= ~SPDK_MLX5_WQE_CTRL_CE_MASK;
		/* [한국어] 기존 CE 비트(NO_FLUSH 등) 제거. */
		qp->ctrl->fm_ce_se |= SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE;
		/* [한국어] CQ_UPDATE를 set — NIC이 이 WQE 완료 시 CQE 발행. */
		mlx5_qp_update_comp(qp);
		/* [한국어] last_pi 슬롯에 누적 unsignaled 수 기록. */
	}
	mlx5_ring_tx_db(qp, qp->ctrl);
	/* [한국어] DBR 갱신 + BlueFlame UAR write로 NIC trigger (mlx5_priv.h 참조). */
}

/*
 * [한국어]
 * mlx5_cq_get_cqe - CQ ring에서 ci 위치의 CQE 가상주소를 계산.
 *
 * @hw_cq: raw CQ 메타데이터.
 * @cqe_size: CQE 크기(64 또는 128).
 * @return: 64B 헤더로 캐스팅된 CQE 포인터.
 *
 * 64B/128B CQE 모두 첫 64B에 메인 헤더(opcode/byte_cnt/qpn/wqe_counter/op_own)가 들어 있으나,
 * 128B 모드에서는 NIC이 두 번째 64B에 메인을 적고 첫 64B에 mini-CQE를 패킹할 수 있다.
 * 따라서 cqe_size==128이면 cqe+1(=두 번째 64B)을 반환해 메인 영역을 가리키게 한다.
 *
 * 호출 체인:
 *   mlx5_cq_poll_one → [mlx5_cq_get_cqe]
 */
static inline struct mlx5_cqe64 *
mlx5_cq_get_cqe(struct mlx5_hw_cq *hw_cq, int cqe_size)
{
	struct mlx5_cqe64 *cqe;

	/* note: that the cq_size is known at the compilation time. We pass it
	 * down here so that branch and multiplication will be done at the
	 * compile time during inlining
	 */
	cqe = (struct mlx5_cqe64 *)(hw_cq->cq_addr + (hw_cq->ci & (hw_cq->cqe_cnt - 1)) *
				    cqe_size);
	/* [한국어] (CQ 시작 주소) + (현재 슬롯 인덱스) × cqe_size. cqe_cnt는 2^k이라 mask로 modulo.
	 * cqe_size가 컴파일 타임 상수면 곱셈도 상수 폴딩됨(인라인 시). */
	return cqe_size == 64 ? cqe : cqe + 1;
	/* [한국어] 64B 모드면 그대로, 128B 모드면 +64B(=메인 헤더 위치) 반환. 분기는 컴파일 타임 결정. */
}


/*
 * [한국어]
 * mlx5_cq_poll_one - CQ에서 다음 CQE 한 개를 polling으로 획득.
 *
 * @hw_cq: raw CQ.
 * @cqe_size: CQE 크기.
 * @return: 유효 CQE 포인터 또는 NULL(아직 NIC이 안 적었거나 INVALID opcode).
 *
 * CQE의 owner 비트와 ci&cqe_cnt(누적 카운터의 wrap-라운드 비트)를 비교해 유효성 검사.
 * NIC은 새 CQE를 적을 때 owner 비트를 토글하므로, 호스트가 기대하는 owner 값과 일치하면 유효.
 * 유효하면 ci++로 슬롯 진행.
 *
 * 호출 체인:
 *   spdk_mlx5_cq_poll_completions → [mlx5_cq_poll_one] → mlx5_cq_get_cqe
 */
static inline struct mlx5_cqe64 *
mlx5_cq_poll_one(struct mlx5_hw_cq *hw_cq, int cqe_size)
{
	struct mlx5_cqe64 *cqe;

	cqe = mlx5_cq_get_cqe(hw_cq, cqe_size);
	/* [한국어] 다음 슬롯의 CQE 주소 계산. */

	/* cqe is hw owned */
	if (mlx5dv_get_cqe_owner(cqe) == !(hw_cq->ci & hw_cq->cqe_cnt)) {
		/* [한국어] owner 비트가 호스트 기대값과 다르면 NIC이 아직 안 적은 슬롯.
		 * ci & cqe_cnt: ci가 한 바퀴 돌 때마다 토글되는 wrap 비트. 이걸 NOT 하여 NIC이
		 * 다음 라운드에 적을 owner 값과 비교. */
		return NULL;
	}

	/* and must have valid opcode */
	if (mlx5dv_get_cqe_opcode(cqe) == MLX5_CQE_INVALID) {
		/* [한국어] owner는 맞지만 opcode가 INVALID면 NIC이 아직 쓰는 중일 수 있음. NULL 반환. */
		return NULL;
	}

	hw_cq->ci++;
	/* [한국어] CQE 한 개 소비 → consumer index 전진. */

	SPDK_DEBUGLOG(mlx5,
		      "cq: 0x%x ci: %d CQ opcode %d size %d wqe_counter %d scatter32 %d scatter64 %d\n",
		      hw_cq->cq_num, hw_cq->ci,
		      mlx5dv_get_cqe_opcode(cqe),
		      be32toh(cqe->byte_cnt),
		      be16toh(cqe->wqe_counter),
		      cqe->op_own & MLX5_INLINE_SCATTER_32,
		      cqe->op_own & MLX5_INLINE_SCATTER_64);
	/* [한국어] 디버그 출력: cq#/ci/opcode/byte 수/wqe 카운터/inline scatter 비트. */
	return cqe;
}

/*
 * [한국어]
 * mlx5_qp_get_comp_wr_id - CQE의 wqe_counter로부터 호스트에 보존된 wr_id를 회수하고
 * tx_available을 복원.
 *
 * @qp: 이 CQE의 소유 QP.
 * @cqe: 유효 CQE.
 * @return: 사용자에게 보고할 wr_id.
 *
 * CQE의 wqe_counter는 SQ 슬롯 인덱스(BE16)이므로 sq_mask로 modulo하여 completions[] 인덱스로 변환.
 * completions[idx].completions는 자기+이전 unsignaled 합계이므로, 이를 tx_available에 더하면
 * 한 번의 CQE로 여러 WQE 슬롯을 동시에 회수할 수 있다.
 *
 * 호출 체인:
 *   spdk_mlx5_cq_poll_completions → [mlx5_qp_get_comp_wr_id]
 */
static inline uint64_t
mlx5_qp_get_comp_wr_id(struct spdk_mlx5_qp *qp, struct mlx5_cqe64 *cqe)
{
	uint16_t comp_idx;
	uint32_t sq_mask;

	sq_mask = qp->hw.sq_wqe_cnt - 1;
	/* [한국어] SQ 슬롯 마스크(2^k - 1). */
	comp_idx = be16toh(cqe->wqe_counter) & sq_mask;
	/* [한국어] CQE 안의 wqe_counter(BE16)를 디코드 후 마스킹 → completions[] 인덱스. */
	SPDK_DEBUGLOG(mlx5, "got cpl, wqe_counter %u, comp_idx %u; wrid %"PRIx64", cpls %u\n",
		      cqe->wqe_counter, comp_idx, qp->completions[comp_idx].wr_id, qp->completions[comp_idx].completions);
	/* [한국어] 디버그: 어떤 슬롯의 wr_id가 몇 개의 unsignaled 동반자와 함께 회수되는지. */
	/* If we have several unsignaled WRs, we accumulate them in the completion of the next signaled WR */
	qp->tx_available += qp->completions[comp_idx].completions;
	/* [한국어] 누적 BB 수만큼 SQ 잔량 회수 — 다음 게시를 위해 슬롯이 비워짐. */

	return qp->completions[comp_idx].wr_id;
	/* [한국어] 게시 시 보존했던 사용자 wr_id 반환. */
}

/*
 * [한국어]
 * spdk_mlx5_cq_poll_completions - CQ를 batch polling, max_completions까지 채워 반환.
 *
 * @cq: 폴링 대상 CQ.
 * @comp: 호출자가 제공한 출력 배열 — wr_id/status가 채워짐.
 * @max_completions: 한 번에 회수할 최대 완료 수.
 * @return: 실제 회수된 완료 수 (0이면 더 없음), 또는 음수 errno(QP 미등록 등).
 *
 * 동작:
 *   1. CQ에서 CQE를 한 개 꺼냄(없으면 break).
 *   2. CQE의 sop_drop_qpn 하위 24b로부터 mlx5_cq_find_qp로 QP 검색.
 *   3. opcode가 MLX5_CQE_REQ면 정상 — wr_id 회수 + IBV_WC_SUCCESS.
 *      아니면 mlx5_cqe_err로 syndrome 디코드 + 진단 로그.
 *   4. n < max_completions까지 반복.
 *
 * 컨텍스트: SPDK reactor 스레드의 polling 루프(spdk_poller)에서 주기적으로 호출.
 * 단일 스레드가 같은 CQ를 독점하므로 락 없음.
 *
 * 호출 체인:
 *   상위 모듈(예: NVMe-oF transport poller, accel_mlx5 poller)
 *     → [spdk_mlx5_cq_poll_completions] → mlx5_cq_poll_one / mlx5_cq_find_qp / mlx5_qp_get_comp_wr_id / mlx5_cqe_err
 */
int
spdk_mlx5_cq_poll_completions(struct spdk_mlx5_cq *cq, struct spdk_mlx5_cq_completion *comp,
			      int max_completions)
{
	struct spdk_mlx5_qp *qp;
	struct mlx5_cqe64 *cqe;
	uint8_t opcode;
	int n = 0;

	do {
		cqe = mlx5_cq_poll_one(&cq->hw, MLX5_DMA_Q_TX_CQE_SIZE);
		/* [한국어] 다음 유효 CQE 획득(없으면 NULL). 64B CQE 가정. */
		if (!cqe) {
			break;
			/* [한국어] CQ 비었음 — 더 이상 회수할 게 없으니 종료. */
		}

		qp = mlx5_cq_find_qp(cq, be32toh(cqe->sop_drop_qpn) & 0xffffff);
		/* [한국어] sop_drop_qpn(BE32)의 하위 24비트 = QP 번호 → 2D LUT으로 QP 검색. */
		if (spdk_unlikely(!qp)) {
			/* [한국어] CQ에 등록되지 않은 QP의 CQE → 비정상. ENODEV로 호출자에 보고. */
			return -ENODEV;
		}

		opcode = mlx5dv_get_cqe_opcode(cqe);
		/* [한국어] op_own 상위 4비트 = CQE 종류(REQ/REQ_ERR/RESP/...). */
		comp[n].wr_id = mlx5_qp_get_comp_wr_id(qp, cqe);
		/* [한국어] 보존된 wr_id 회수 + tx_available 복원. */
		if (spdk_likely(opcode == MLX5_CQE_REQ)) {
			/* [한국어] 정상 요청자(send) 완료 — 핫 패스. */
			comp[n].status = IBV_WC_SUCCESS;
		} else {
			/* [한국어] 에러 또는 RESP 종류 — syndrome 디코드. */
			comp[n].status = mlx5_cqe_err(cqe);
		}
		n++;
	} while (n < max_completions);
	/* [한국어] max_completions까지 또는 CQ가 빌 때까지 batch 폴링. */

	return n;
	/* [한국어] 회수된 CQE 수 반환. 0이면 호출자가 다음 polling 사이클에서 재시도. */
}

/*
 * [한국어]
 * spdk_mlx5_qp_complete_send - 누적된 WQE의 doorbell을 trigger (batched submit flush).
 *
 * @qp: QP.
 *
 * mlx5_qp_wqe_submit은 sq_pi만 전진시키고 doorbell을 울리지 않는다. 호출자가 여러 WR을 모아
 * 게시한 뒤 본 함수를 호출해 한 번의 PCIe MMIO write로 모두를 NIC에 알린다 → PCIe write 횟수
 * 절감으로 처리량 향상.
 *
 * SIG_LAST 모드에서는 마지막 WQE에만 CQ_UPDATE 비트가 lazy하게 set된다.
 *
 * 호출 체인:
 *   상위 모듈 → [spdk_mlx5_qp_complete_send] → mlx5_qp_tx_complete → mlx5_ring_tx_db
 */
void
spdk_mlx5_qp_complete_send(struct spdk_mlx5_qp *qp)
{
	mlx5_qp_tx_complete(qp);
	/* [한국어] 단순 위임 — 인라인 헬퍼에서 SIG_LAST 처리 + doorbell. */
}

#ifdef DEBUG
/*
 * [한국어]
 * mlx5_qp_dump_wqe - DEBUG 빌드 한정. 마지막에 게시한 WQE를 16-DW(=64B) 단위로 hex dump.
 *
 * @qp: QP.
 * @n_wqe_bb: 덤프할 BB 수.
 *
 * SPDK_LOG_mlx5_sq 로그 플래그가 켜진 경우에만 출력. SQ에서 ring wrap을 만나면
 * mlx5_qp_get_next_wqebb로 이어 작성된 BB를 따라간다. 진단용으로 PRM 명세와 비교 시 유용.
 *
 * 호출 체인:
 *   mlx5_qp_wqe_submit → mlx5_qp_dump_wqe (DEBUG 빌드만)
 */
void
mlx5_qp_dump_wqe(struct spdk_mlx5_qp *qp, int n_wqe_bb)
{
	struct mlx5_hw_qp *hw = &qp->hw;
	/* [한국어] raw QP. */
	uint32_t pi;
	uint32_t to_end;
	uint32_t *wqe;
	int i;
	extern struct spdk_log_flag SPDK_LOG_mlx5_sq;
	/* [한국어] mlx5_sq 로그 플래그 — 외부 정의(SPDK_LOG_REGISTER_COMPONENT). */

	if (!SPDK_LOG_mlx5_sq.enabled) {
		return;
		/* [한국어] 로그 플래그 꺼져있으면 즉시 반환(성능 영향 없음). */
	}

	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	/* [한국어] 슬롯 인덱스 — sq_pi는 이미 wqe_submit에서 전진한 상태이므로 다음 슬롯이 가리켜질 수 있음. */
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	/* [한국어] 끝까지 거리. */
	wqe = mlx5_qp_get_wqe_bb(hw);
	/* [한국어] 현재 슬롯의 BB 시작 주소. */

	SPDK_DEBUGLOG(mlx5_sq, "QP: qpn 0x%" PRIx32 ", wqe_index 0x%" PRIx32 ", addr %p\n",
		      hw->qp_num, pi, wqe);
	/* [한국어] 헤더 한 줄: QP/슬롯/주소. */
	for (i = 0; i < n_wqe_bb; i++) {
		fprintf(stderr,
			"%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n"
			"%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n"
			"%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n"
			"%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			be32toh(wqe[0]),  be32toh(wqe[1]),  be32toh(wqe[2]),  be32toh(wqe[3]),
			be32toh(wqe[4]),  be32toh(wqe[5]),  be32toh(wqe[6]),  be32toh(wqe[7]),
			be32toh(wqe[8]),  be32toh(wqe[9]),  be32toh(wqe[10]), be32toh(wqe[11]),
			be32toh(wqe[12]), be32toh(wqe[13]), be32toh(wqe[14]), be32toh(wqe[15]));
		/* [한국어] 64B(=16 DW) 한 BB를 4행 × 4열로 hex dump. BE→host 변환 후 출력. */
		wqe = mlx5_qp_get_next_wqebb(hw, &to_end, wqe);
		/* [한국어] 다음 BB로 진행(ring wrap 자동 처리). */
	}
}
#endif

/* [한국어] mlx5_sq 로그 컴포넌트 등록 — SPDK 로그 시스템에 새 채널을 추가.
 * 사용자는 spdk_log_set_flag("mlx5_sq")로 켜면 위 dump가 활성화된다. */
SPDK_LOG_REGISTER_COMPONENT(mlx5_sq)
