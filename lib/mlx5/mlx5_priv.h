/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] Mellanox mlx5 direct-verbs 가속 경로의 내부 정의 헤더 (mlx5_priv.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK의 lib/mlx5 라이브러리 내부에서만 사용되는 자료구조, 인라인 헬퍼,
 * Work Queue Element(WQE) 빌드/제출 매크로, 그리고 Completion Queue(CQ) 폴링 헬퍼를
 * 정의한다. mlx5는 Mellanox(현 NVIDIA Networking)의 ConnectX 시리즈 RDMA NIC을 위한
 * 직접-verbs(direct verbs, DV) API로, 일반 libibverbs를 우회하여 사용자 공간에서 직접
 * QP/CQ 자원을 제어함으로써 RDMA·DMA·암호화 가속(AES-XTS) 성능을 끌어올린다. 이 헤더는
 * lib/mlx5의 .c 파일들(mlx5_qp.c, mlx5_dma.c, mlx5_crypto.c)에서 공유되는 비공개 정의를
 * 모아두는 곳이며, 외부 모듈에는 노출되지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택에서 lib/mlx5는 `lib/nvme`, `lib/nvmf`(NVMe-over-Fabrics RDMA transport),
 * 그리고 암호화 bdev 모듈의 가속 백엔드로 동작한다. 호출 체인은 다음과 같다:
 *   bdev_crypto / nvmf_rdma → spdk_internal/mlx5.h(공개 API) → mlx5_qp.c / mlx5_dma.c /
 *   mlx5_crypto.c (이 헤더 사용) → libmlx5dv (커널 RDMA core) → ConnectX-NIC 하드웨어
 * 즉 이 헤더의 헬퍼들은 호스트 유저스페이스에서 실행되며, 작성한 WQE를 NIC의 doorbell
 * MMIO 주소로 직접 전송하여 커널을 우회한다. SPDK reactor 스레드에서 polling 모드로
 * 호출되며, polling 루프가 CQE를 파싱하여 완료를 보고한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: <infiniband/mlx5dv.h>(직접-verbs 인터페이스), spdk/barrier.h(메모리 배리어),
 *   spdk_internal/mlx5.h(공개 mlx5 wrapper), 그리고 mlx5_ifc.h(자동 생성된 PRM 정의).
 *   PRM(Programmer's Reference Manual)은 Mellanox NIC의 펌웨어 인터페이스 스펙이며
 *   WQE/CQE 비트 레이아웃, opcode 등을 정의한다.
 * - 데이터 흐름: SPDK 상위 레이어가 spdk_mlx5_qp_rdma_write/read 같은 API를 호출하면,
 *   여기서 정의한 mlx5_qp_get_wqe_bb()로 SQ 버퍼의 다음 WQE 슬롯을 얻고, ctrl/raddr/data
 *   세그먼트를 채운 뒤 mlx5_ring_tx_db()로 doorbell을 두드린다. 완료는 NIC가 CQ 메모리에
 *   CQE를 기록하면서 발생하고, polling 코드가 mlx5_cq_get_cqe()로 이를 발견한다.
 * - 공유 상태: spdk_mlx5_cq는 24비트 qp_num을 12+12 비트 2D LUT으로 분해한 qps[][]
 *   테이블을 들고 있어 CQE의 qp_num을 O(1)로 spdk_mlx5_qp 포인터로 매핑한다. CQ 하나에
 *   여러 QP가 바인딩되며, 동일 CQ를 polling 하는 단일 reactor 스레드만 접근하므로
 *   락-프리 설계가 가능하다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct mlx5_hw_cq / mlx5_hw_qp: NIC가 보는 raw CQ/QP 메타데이터(MMIO/메모리 주소,
 *   WQE 카운트, doorbell 주소). 직접 폴링/제출용으로 캐시-라인 핫 데이터.
 * - struct spdk_mlx5_cq: hw_cq + qp 매핑 LUT + 상위 ibv_cq 핸들.
 * - struct spdk_mlx5_qp: hw_qp + 완료 추적 배열(completions) + sigmode + 상위 ibv_qp.
 * - mlx5_qp_get_wqe_bb(): 다음 빈 WQE Building Block 슬롯의 가상 주소 계산.
 * - mlx5_qp_set_comp(): 완료 콜백 매핑(wr_id 저장) 및 unsignaled WQE 누적 추적.
 * - mlx5_ring_tx_db(): doorbell record 갱신 + UAR(BlueFlame) MMIO write로 NIC 깨움.
 * - mlx5_cq_find_qp(): CQE에 박힌 qp_num으로부터 spdk_mlx5_qp 검색 (2D LUT).
 * - mlx5_get_pd_id(): ibv_pd로부터 PD index(pdn) 추출. crypto MKEY 등록 시 사용.
 */

/* [한국어] SPDK 표준 C 라이브러리 추상화: stddef, stdint, errno, stdio, string 등을
 * SPDK 빌드 컨벤션에 맞춰 한 번에 끌어온다. 플랫폼별 차이를 흡수한다. */
#include "spdk/stdinc.h"
/* [한국어] BSD-스타일 list/queue 매크로(TAILQ, LIST 등)를 제공. 본 파일에서 직접 쓰지 않더라도
 * 포함하는 .c 파일들이 SPDK 컨테이너 매크로를 사용한다. */
#include "spdk/queue.h"
/* [한국어] CPU 메모리 배리어(spdk_smp_wmb, spdk_wmb 등) 매크로 정의. NIC doorbell write
 * 직전 store 순서 보장에 필수. PRM 8.9.3.1 절에 따르면 WQE 메모리 쓰기 → DBR 갱신 →
 * UAR 쓰기 사이에 store fence가 들어가야 한다. */
#include "spdk/barrier.h"
/* [한국어] spdk_likely/spdk_unlikely 분기 힌트 매크로. polling 핫 패스에서 분기 예측 정확도
 * 향상을 위해 사용된다. */
#include "spdk/likely.h"

/* [한국어] Mellanox direct-verbs 헤더. 일반 libibverbs는 ibv_post_send 등 추상 인터페이스만
 * 노출하지만 mlx5dv는 NIC가 보는 raw SQ/CQ 메모리 주소, doorbell 주소, BlueFlame UAR 등을
 * 직접 매핑해주어 사용자 공간에서 WQE를 빌드해 doorbell만 두드리면 된다. */
#include <infiniband/mlx5dv.h>
/* [한국어] SPDK 내부 mlx5 wrapper API 정의(spdk_mlx5_qp/cq 타입, opaque 핸들 시그니처).
 * 이 헤더는 SPDK lib/* 내부 모듈끼리만 공유하며 spdk_internal 디렉토리 아래에 위치한다. */
#include "spdk_internal/mlx5.h"

/**
 * Low level CQ representation, suitable for the direct polling
 */
/*
 * [한국어]
 * struct mlx5_hw_cq - NIC가 직접 보는 raw CQ 메타데이터.
 *
 * direct polling 경로(spdk_mlx5_cq_poll_completions)에서 사용하기 위한 캐시-친화적 구조.
 * 일반 libibverbs의 ibv_poll_cq를 우회하고 CQ 메모리에 NIC가 기록한 CQE를 직접 읽는다.
 * 이는 시스템 콜 비용 회피와 polling 핫 패스 단축이 목적이다.
 */
struct mlx5_hw_cq {
	uint64_t cq_addr;
	/* [한국어] CQ 버퍼의 가상 주소(uintptr_t로 캐스팅된 형태).
	 * 설정자: mlx5_cq_init()에서 mlx5dv_init_obj가 채운 mlx5dv_cq.buf로 초기화.
	 * 읽는 자: mlx5_cq_get_cqe()가 ci(consumer index) × cqe_size 오프셋을 더해 다음 CQE를 읽음.
	 * 값 범위: 유저공간에 매핑된 CQ 메모리(보통 hugepage 위), NIC도 동일 주소를 DMA 타깃으로 사용.
	 * 동기화: NIC와 CPU가 공유하는 메모리이지만 cqe->op_own의 owner 비트로 hand-off하므로 락 불필요. */

	uint32_t cqe_cnt;
	/* [한국어] CQ에 들어갈 수 있는 CQE 개수(2의 거듭제곱). 인덱스 마스킹(ci & (cqe_cnt-1))에 사용.
	 * 설정자: mlx5_cq_init()에서 mlx5dv_cq.cqe_cnt를 그대로 저장.
	 * 읽는 자: mlx5_cq_get_cqe(), mlx5_cq_poll_one(), 그리고 owner-bit 비교 시 ci & cqe_cnt.
	 * 값 범위: 보통 사용자가 요청한 cqe_cnt를 NIC가 next-power-of-2로 올린 값. */

	uint32_t cqe_size;
	/* [한국어] CQE 한 개 크기(바이트). mlx5는 64B 또는 128B CQE를 지원한다.
	 * 설정자: mlx5_cq_init()이 사용자 attr->cqe_size로 mlx5dv_create_cq에 전달, init_obj로 회수.
	 * 읽는 자: mlx5_cq_get_cqe()가 인덱스 곱셈에 사용. 64B인 경우 128B의 절반(=상단 64B 헤더)에
	 *   메타데이터가, 128B인 경우 mini-CQE 패킹 등을 위해 추가 64B가 사용된다.
	 * 값 범위: 64 또는 128. */

	uint32_t ci;
	/* [한국어] Consumer Index — CQ에서 다음 폴링할 슬롯 번호(누적, wrap 안 함).
	 * 설정자: mlx5_cq_poll_one()가 유효 CQE를 소비할 때마다 ci++.
	 * 읽는 자: mlx5_cq_get_cqe()가 (ci & (cqe_cnt-1))로 슬롯 위치, (ci & cqe_cnt)로 owner 토글 비교.
	 * 값 범위: 0 ~ UINT32_MAX. wrap-around되지만 마스크 연산으로 인덱싱하므로 무방.
	 * 동기화: 단일 reactor 스레드가 polling을 독점하므로 atomic 불필요. */

	uint32_t cq_num;
	/* [한국어] NIC가 부여한 CQ 식별자(cqn). 디버그 로그/추적, devx 명령에서 CQ 지정 시 사용.
	 * 설정자: mlx5_cq_init()이 mlx5dv_cq.cqn을 저장. 변하지 않는 값. */
};

/**
 * Low level CQ representation, suitable for the WQEs submission.
 * Only submission queue is supported, receive queue is omitted since not used right now
 */
/*
 * [한국어]
 * struct mlx5_hw_qp - NIC가 직접 보는 raw QP 메타데이터(SQ 한정).
 *
 * SPDK lib/mlx5는 RC(Reliable Connection) QP의 송신 측만 사용한다(loopback / NVMe-oF
 * initiator side / DMA / crypto offload). 수신 큐(RQ)는 다른 경로에서 처리되거나 사용되지
 * 않으므로 본 구조체는 SQ만 추적한다. WQE를 직접 작성하고 doorbell을 두드리는 데 필요한
 * 모든 주소를 한 줄에 담아 캐시 hot-line을 최소화한다.
 */
struct mlx5_hw_qp {
	uint64_t dbr_addr;
	/* [한국어] Doorbell Record(DBR) 메모리의 가상 주소. NIC와 호스트가 공유하는 8B 영역으로,
	 * SQ producer index(sq_pi)를 little-endian 순서로 기록하면 NIC가 polling으로 발견하거나
	 * 곧이어 들어오는 UAR write의 힌트로 사용한다. PRM 8.9.3.1 절의 step 2에 해당.
	 * 설정자: mlx5_qp_init()에서 mlx5dv_qp.dbrec로 초기화. 읽는 자: mlx5_update_tx_db(). */

	uint64_t sq_addr;
	/* [한국어] Send Queue 버퍼(WQE 메모리 풀)의 시작 가상 주소.
	 * 설정자: mlx5_qp_init()에서 mlx5dv_qp.sq.buf로 초기화.
	 * 읽는 자: mlx5_qp_get_wqe_bb()가 (sq_pi & (sq_wqe_cnt-1)) * MLX5_SEND_WQE_BB(64B)
	 *   오프셋을 더해 다음 WQE Building Block 위치를 계산. */

	uint64_t sq_bf_addr;
	/* [한국어] BlueFlame(BF) UAR(User Access Region)의 MMIO 주소. NIC PCI BAR이 유저공간에
	 * 매핑된 영역으로, 여기에 ctrl segment의 첫 64bit를 직접 store하면 곧바로 doorbell이
	 * trigger되어 NIC이 새 WQE를 처리하기 시작한다. WC(Write-Combined) 매핑이면 store가
	 * 결합될 수 있어 별도 fence가 필요하다(아래 mlx5_ring_tx_db 참조).
	 * 설정자: mlx5_qp_init()이 mlx5dv_qp.bf.reg로 초기화. */

	uint32_t sq_wqe_cnt;
	/* [한국어] SQ 슬롯 개수(WQE Building Block 단위, 2의 거듭제곱). 1 BB = 64B.
	 * 설정자: mlx5_qp_init()이 mlx5dv_qp.sq.wqe_cnt 저장.
	 * 읽는 자: 인덱스 마스킹 sq_pi & (sq_wqe_cnt - 1) 및 wrap-around 검사. */

	uint16_t sq_pi;
	/* [한국어] Producer Index(누적, wrap 안 함). 호스트가 다음에 쓸 WQE 슬롯 번호를 가리킴.
	 * 설정자: mlx5_qp_wqe_submit()이 n_wqe_bb만큼 증가시킴.
	 * 읽는 자: doorbell record 기록 시 sq_pi 값을 그대로 NIC에 전달. mlx5_qp_get_wqe_bb 등.
	 * 값 범위: 0 ~ 0xFFFF. wrap되지만 sq_wqe_cnt는 < 64K이므로 modulo로 인덱싱 가능. */

	uint32_t sq_tx_db_nc;
	/* [한국어] BF UAR이 NC(non-combined) 매핑인지 여부. true면 추가 store fence 불필요.
	 * BlueField-1/2 SmartNIC에서는 NC 매핑이 기본이라 fence 비용을 아낄 수 있다.
	 * 설정자: mlx5_qp_init()이 mlx5dv_qp.bf.size==0 조건으로 결정.
	 * 읽는 자: mlx5_ring_tx_db()의 #if !aarch64 분기에서 fence 발행 여부 결정. */

	uint32_t qp_num;
	/* [한국어] NIC가 부여한 QP 번호(24비트, 상위 8비트는 0).
	 * 설정자: mlx5_qp_init()이 verbs_qp->qp_num을 그대로 저장.
	 * 읽는 자: WQE ctrl seg 작성, CQE의 sop_drop_qpn 매칭(mlx5_cq_find_qp), 디버그 로그. */
};

/* qp_num is 24 bits. 2D lookup table uses upper and lower 12 bits to find a qp by qp_num */
/* [한국어] qp_num을 상위 12비트와 하위 12비트로 분리하여 2D 테이블로 색인한다. 하나의 1D
 * 테이블 24비트(=16M 엔트리)를 만드는 대신 sparse한 2D LUT을 만들어 메모리를 절약한다.
 * CQ당 등록되는 QP는 보통 수십~수천 개에 그치므로 상위 12비트별로 hot한 슬롯만 lower table을
 * 동적 할당한다(mlx5_cq_add_qp 참조). */
#define SPDK_MLX5_QP_NUM_UPPER_SHIFT (12)
/* [한국어] 하위 12비트 마스크 = 0xFFF. (1 << 12) - 1 형태로 표현. */
#define SPDK_MLX5_QP_NUM_LOWER_MASK ((1 << SPDK_MLX5_QP_NUM_UPPER_SHIFT) - 1)
/* [한국어] LUT 슬롯 수 = 4096. spdk_mlx5_cq.qps[]가 이만큼의 엔트리(상위 12비트)를 갖고,
 * 각 엔트리가 다시 4096개의 (하위 12비트 → spdk_mlx5_qp*)를 가리키는 테이블 포인터를 보관. */
#define SPDK_MLX5_QP_NUM_LUT_SIZE (1 << 12)

/*
 * [한국어]
 * struct spdk_mlx5_cq - SPDK가 관리하는 mlx5 CQ 객체.
 *
 * 하나의 CQ에 다수의 QP가 바인딩될 수 있어, CQE 폴링 시 CQE 안의 qp_num으로부터 어느
 * spdk_mlx5_qp의 완료인지 역방향으로 찾아야 한다. 본 구조체의 qps[][2D LUT]가 그 역할.
 * verbs_cq는 RDMA core 정리/파괴 시 ibv_destroy_cq에 넘기기 위한 핸들이다.
 */
struct spdk_mlx5_cq {
	struct mlx5_hw_cq hw;
	/* [한국어] direct polling용 raw CQ 메타데이터. mlx5_cq_init()에서 채워짐. */

	struct {
		struct spdk_mlx5_qp **table;
		/* [한국어] 하위 12비트 → spdk_mlx5_qp* 매핑 테이블 포인터. count가 0이면 NULL.
		 * count > 0가 되는 시점에 calloc(SPDK_MLX5_QP_NUM_LUT_SIZE, ...)으로 할당된다. */
		uint32_t count;
		/* [한국어] 이 상위 12비트 슬롯에 등록된 QP 수. 0이 되면 table을 free하고 NULL로 리셋. */
	} qps [SPDK_MLX5_QP_NUM_LUT_SIZE];
	/* [한국어] qp_num의 상위 12비트로 인덱싱되는 1차 LUT. 4096 엔트리.
	 * 설정자: mlx5_cq_add_qp() / mlx5_cq_remove_qp().
	 * 읽는 자: mlx5_cq_find_qp() — CQE 폴링 시 qpn → qp 변환에 사용.
	 * 동기화: 동일 CQ를 다루는 reactor 스레드가 add/remove/poll을 모두 수행하므로 락 없음. */

	struct ibv_cq *verbs_cq;
	/* [한국어] 상위 libibverbs CQ 핸들. mlx5dv_create_cq의 결과를 ibv_cq_ex_to_cq로 변환한 것.
	 * 설정자: mlx5_cq_init(). 읽는 자: 파괴 경로(ibv_destroy_cq), QP 생성 시 send_cq/recv_cq 인자. */

	uint32_t qps_count;
	/* [한국어] 이 CQ에 바인딩된 총 QP 수. 0이 아니면 spdk_mlx5_cq_destroy가 EBUSY 반환. */
};

/*
 * [한국어]
 * struct mlx5_qp_sq_completion - SQ에 게시한 WQE 한 개(또는 unsignaled 그룹의 대표)에
 * 대응하는 호스트-측 완료 추적 엔트리.
 *
 * mlx5는 매 WQE마다 CQE를 만들지 않고 ctrl segment의 fm_ce_se = CQ_UPDATE 비트가 설정된
 * WQE에서만 CQE를 생성한다(unsignaled completion). SPDK는 wr_id 콜백 시그니처를 유지해야
 * 하므로 호스트 측에서 보조 배열로 wr_id와 누적된 unsignaled 개수를 추적한다.
 */
struct mlx5_qp_sq_completion {
	uint64_t wr_id;
	/* [한국어] 사용자가 spdk_mlx5_qp_rdma_write 등 API 호출 시 전달한 work-request ID.
	 * CQE 도착 시 이 wr_id를 spdk_mlx5_cq_completion.wr_id로 복원하여 호출자에게 보고.
	 * 인덱스: SQ 슬롯 번호(pi & (sq_wqe_cnt-1)). */

	/* Number of unsignaled completions before this one. Used to track qp overflow */
	uint32_t completions;
	/* [한국어] 이 signaled WQE까지 누적된 unsignaled WQE 수(자기 자신 포함).
	 * 설정자: mlx5_qp_set_comp() — fm_ce_se에 CQ_UPDATE가 있으면 nonsignaled_outstanding+n_bb를
	 *   기록한 뒤 nonsignaled_outstanding을 0으로 리셋.
	 * 읽는 자: mlx5_qp_get_comp_wr_id() — CQE를 받았을 때 tx_available에 더해 SQ 자리를 회수.
	 * 즉 unsignaled WQE들도 이 한 번의 CQE로 한꺼번에 회수된다. */
};

/*
 * [한국어]
 * struct spdk_mlx5_qp - SPDK가 관리하는 mlx5 QP 객체(송신 전용 RC QP).
 *
 * RDMA write/read, DMA, UMR, crypto MKEY configure 등 모든 송신 op가 이 QP를 통해 게시된다.
 * tx_available은 SQ 슬롯 잔량을 추적하며, 잔량 < bb_count일 때 호출자는 ENOMEM을 받고
 * 백오프하거나 CQ를 polling해서 자리를 비워야 한다.
 */
struct spdk_mlx5_qp {
	struct mlx5_hw_qp hw;
	/* [한국어] direct submission용 raw QP 메타데이터. */

	struct mlx5_qp_sq_completion *completions;
	/* [한국어] SQ 슬롯 수만큼 할당된 완료 추적 배열(posix_memalign 4096B 정렬).
	 * 슬롯 i = pi & (sq_wqe_cnt-1) 위치에 wr_id와 누적 unsignaled 수를 기록.
	 * 설정자: mlx5_qp_set_comp() — submit 시. 읽는 자: mlx5_qp_get_comp_wr_id() — 폴링 시. */

	/* Pointer to a last WQE controll segment written to SQ */
	struct mlx5_wqe_ctrl_seg *ctrl;
	/* [한국어] 가장 최근에 작성한 WQE의 control segment 포인터.
	 * - mlx5_ring_tx_db() 시 BlueFlame UAR으로 store할 첫 64bit의 출처.
	 * - SIG_LAST 모드에서 마지막 WQE의 fm_ce_se에 CQ_UPDATE 비트를 set하기 위해 추적. */

	struct spdk_mlx5_cq *cq;
	/* [한국어] 이 QP의 완료가 보고되는 CQ. spdk_mlx5_qp_create 시 인자로 받음.
	 * 설정자: spdk_mlx5_qp_create(). 읽는 자: spdk_mlx5_qp_destroy()에서 LUT 제거. */

	struct ibv_qp *verbs_qp;
	/* [한국어] 상위 libibverbs QP 핸들. mlx5dv_create_qp 결과.
	 * 설정자: mlx5_qp_init(). 읽는 자: 파괴 경로, modify_qp(에러 상태 천이), get_verbs_qp(외부 노출). */

	/* Number of WQEs submitted to HW which won't produce a CQE */
	uint16_t nonsignaled_outstanding;
	/* [한국어] 아직 누적 중인 unsignaled WQE 개수(WQE Building Block 단위).
	 * 설정자: mlx5_qp_set_comp() — CQ_UPDATE 비트 없는 WQE 게시 시 +n_bb, 있는 WQE 시 0으로 리셋.
	 * 다음 signaled WQE의 completions 필드에 흡수되어 한 번의 CQE로 일괄 회수된다. */

	uint16_t max_send_sge;
	/* [한국어] 한 WQE가 가질 수 있는 최대 data segment 수. 사용자가 cap.max_send_sge로 요청.
	 * mlx5_qp_rdma_op()가 sge_count > max_send_sge면 -E2BIG 반환. */

	/* Number of WQEs available for submission */
	uint16_t tx_available;
	/* [한국어] 송신 가능한 SQ 슬롯 잔량(WQE BB 단위).
	 * 설정자: 초기값 = sq_wqe_cnt. 게시 시 -=bb_count. 폴링 시 += completions[..].completions.
	 * tx_available < bb_count면 -ENOMEM. */

	uint16_t last_pi;
	/* [한국어] 마지막으로 게시한 WQE의 pi(절대값 아닌 슬롯 인덱스). SIG_LAST 모드에서
	 * mlx5_qp_update_comp가 completions[last_pi].completions에 누적값을 기록할 때 사용. */

	uint8_t sigmode;
	/* [한국어] Completion event mode. SPDK_MLX5_QP_SIG_NONE/ALL/LAST 중 하나.
	 * - NONE: 사용자가 준 fm_ce_se 그대로(원본 CE bit 사용).
	 * - ALL: 모든 WQE에 CQ_UPDATE 강제 → wr_id 추적 단순, but CQ 부하 ↑.
	 * - LAST: 마지막 WQE에만 CQ_UPDATE 강제 → batched submit 후 한 번의 CQE로 회수. */
};

/* [한국어] sigmode 열거값. 위 sigmode 필드 주석 참조. */
enum {
	/* Default mode, use flags passed by the user */
	SPDK_MLX5_QP_SIG_NONE = 0,
	/* [한국어] 사용자가 spdk_mlx5_qp_rdma_write 호출 시 flags의 CE 비트를 그대로 사용. */

	/* Enable completion for every control WQE segment, regardless of the flags passed by the user */
	SPDK_MLX5_QP_SIG_ALL = 1,
	/* [한국어] 모든 WQE의 fm_ce_se에 CQ_UPDATE 비트를 강제 OR. attr.sigall=true에 해당. */

	/* Enable completion only for the last control WQE segment, regardless of the flags passed by the user */
	SPDK_MLX5_QP_SIG_LAST = 2,
	/* [한국어] 마지막 WQE만 강제 CQ_UPDATE. spdk_mlx5_qp_complete_send()가 호출되는 시점에
	 *   ctrl->fm_ce_se 의 CE 비트를 lazy하게 CQ_UPDATE로 덮는다(mlx5_qp_tx_complete 참조). */
};

/**
 * Completion and Event mode (SPDK_MLX5_WQE_CTRL_CE_*)
 * Maps internal representation of completion events configuration to PRM values
 * g_mlx5_ce_map[][X] is fm_ce_se >> 2 & 0x3 */
/*
 * [한국어]
 * g_mlx5_ce_map - sigmode × 사용자 CE bit(2비트) → PRM CE 인코딩 매핑 테이블.
 *
 * 사용자가 SPDK 공개 flags의 비트 [3:2]에 표현한 CE 의도를 PRM(NIC 펌웨어)이 이해하는
 * SPDK_MLX5_WQE_CTRL_CE_* 코드값으로 변환한다. sigmode가 NONE/ALL/LAST일 때 동작이 달라진다:
 *   - NONE 행: 원본 CE를 거의 그대로 보존(0,1은 NO_FLUSH로 통합).
 *   - ALL 행:  index 0(원본=00, no completion)을 CQ_UPDATE로 강제(모든 완료 보고).
 *   - LAST 행: 모두 NO_FLUSH로 만들어 CQE 발생을 미루다가 마지막에 한 번에 처리.
 * 인덱스 3(=ECE)은 모든 sigmode에서 ECE(Enhanced Completion Event)를 그대로 유지.
 */
static uint8_t g_mlx5_ce_map[3][4] = {
	/* SPDK_MLX5_QP_SIG_NONE */
	[0] = {
		[0] = SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR,
		/* [한국어] 사용자=0(완료 알림 안 함) → NO_FLUSH(에러 시에만 CQE). */
		[1] = SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR,
		/* [한국어] 사용자=1 → 동일 NO_FLUSH (NONE 모드에선 1과 0을 동등 취급). */
		[2] = SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE,
		/* [한국어] 사용자=2(CQ 업데이트 요청) → CQ_UPDATE 그대로. */
		[3] = SPDK_MLX5_WQE_CTRL_CE_CQ_ECE
		/* [한국어] 사용자=3 → ECE(Enhanced Completion Event) 패스스루. */
	},
	/* SPDK_MLX5_QP_SIG_ALL */
	[1] = {
		[0] = SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE,
		/* [한국어] ALL 모드에선 사용자 0도 강제 CQ_UPDATE. */
		[1] = SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR,
		/* [한국어] 사용자=1만 NO_FLUSH로 escape hatch (특수 케이스). */
		[2] = SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE,
		[3] = SPDK_MLX5_WQE_CTRL_CE_CQ_ECE
	},
	/* SPDK_MLX5_QP_SIG_LAST */
	[2] = {
		[0] = SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR,
		[1] = SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR,
		[2] = SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR,
		/* [한국어] LAST 모드: 사용자 의도와 무관하게 모두 NO_FLUSH. 마지막 WQE는
		 *   spdk_mlx5_qp_complete_send() 호출 시 mlx5_qp_tx_complete()가 명시적으로 덮어쓴다. */
		[3] = SPDK_MLX5_WQE_CTRL_CE_CQ_ECE
		/* [한국어] ECE는 어떤 모드든 패스스루. */
	}
};

/*
 * [한국어]
 * struct mlx5_crypto_bsf_seg - AES-XTS 암호화/복호화를 위한 BSF(Block Signature Field) 세그먼트.
 *
 * UMR(User-Mode Memory Region) WQE의 일부로 작성되어 NIC 내장 crypto engine에 암호화 파라미터
 * (key reference, tweak, block size)를 전달한다. 이 세그먼트는 64B 정렬이며 PRM의 inline crypto
 * 섹션(MKEY context with crypto)에 정의된 비트 레이아웃을 따른다.
 */
struct mlx5_crypto_bsf_seg {
	uint8_t		size_type;
	/* [한국어] 상위 비트는 BSF 크기 인코딩, 하위는 type(=crypto). */
	uint8_t		enc_order;
	/* [한국어] 암호화 순서(transmit/receive 방향에 따른 enc_after_signature 등). */
	uint8_t		rsvd0;
	/* [한국어] 예약 바이트 — 0으로 채움. */
	uint8_t		enc_standard;
	/* [한국어] 암호 알고리즘 표준 식별자(AES-XTS). */
	__be32		raw_data_size;
	/* [한국어] 암호화 대상 데이터 크기(바이트, big-endian). */
	uint8_t		crypto_block_size_pointer;
	/* [한국어] block size 인덱스 포인터(512B/4096B 등 사전 정의 슬롯 선택). */
	uint8_t		rsvd1[7];
	/* [한국어] 예약. */
	uint8_t		xts_initial_tweak[16];
	/* [한국어] AES-XTS의 초기 tweak(보통 LBA를 LE/BE로 인코딩한 16B). */
	__be32		dek_pointer;
	/* [한국어] DEK(Data Encryption Key) 객체 ID. NIC 내부 key table 참조용. */
	uint8_t		rsvd2[4];
	/* [한국어] 예약. */
	uint8_t		keytag[8];
	/* [한국어] 64비트 keytag — 키 무결성 검증용 부수 데이터(키 생성 시 함께 등록). */
	uint8_t		rsvd3[16];
	/* [한국어] 예약 16B 패딩(BSF 64B 정렬 맞춤). */
};

/*
 * [한국어]
 * struct mlx5_sig_bsf_inl / mlx5_sig_bsf_seg / mlx5_wqe_set_psv_seg -
 * T10-DIF/CRC32C signature offload용 WQE 세그먼트들. SPDK는 NIC가 보호 정보(Protection Information)를
 * 자동 생성/검증하도록 BSF에 sig_type, dif_apptag, reftag 등을 채워 보낸다. 이는 NVMe end-to-end
 * data protection과 NVMe-oF 간의 PI 변환에 사용된다.
 */
struct mlx5_sig_bsf_inl {
	__be16 vld_refresh;
	/* [한국어] valid bit + refresh policy. PI block 새로 작성/검증 정책 제어. */
	__be16 dif_apptag;
	/* [한국어] T10-DIF Application tag(16비트). */
	__be32 dif_reftag;
	/* [한국어] T10-DIF Reference tag(32비트, 보통 LBA의 하위 32비트). */
	uint8_t sig_type;
	/* [한국어] signature 타입(T10-DIF, CRC32C 등). */
	uint8_t rp_inv_seed;
	/* [한국어] reference pattern / invariant seed bits. */
	uint8_t rsvd[3];
	/* [한국어] 예약. */
	uint8_t dif_inc_ref_guard_check;
	/* [한국어] guard/reftag/apptag 검사 활성화 비트맵. */
	__be16 dif_app_bitmask_check;
	/* [한국어] apptag 비트마스크(부분 일치 비교용). */
};

struct mlx5_sig_bsf_seg {
	struct mlx5_sig_bsf_basic {
		uint8_t bsf_size_sbs;
		/* [한국어] BSF 크기 + signature block selector. */
		uint8_t check_byte_mask;
		/* [한국어] 비교 시 어느 바이트를 검사할지 마스크. */
		union {
			uint8_t copy_byte_mask;
			uint8_t bs_selector;
			uint8_t rsvd_wflags;
		} wire;
		/* [한국어] wire(=네트워크 측) 도메인의 PI 설정. */
		union {
			uint8_t bs_selector;
			uint8_t rsvd_mflags;
		} mem;
		/* [한국어] memory(=호스트 측) 도메인의 PI 설정. */
		__be32 raw_data_size;
		/* [한국어] data 크기. */
		__be32 w_bfs_psv;
		/* [한국어] wire-side BFS PSV(Protection State Vector) index. */
		__be32 m_bfs_psv;
		/* [한국어] memory-side BFS PSV index. */
	} basic;
	/* [한국어] 기본(필수) signature 파라미터. */
	struct mlx5_sig_bsf_ext {
		__be32 t_init_gen_pro_size;
		/* [한국어] 보호 데이터 초기 생성 시 사이즈 파라미터(transit/internal). */
		__be32 rsvd_epi_size;
		/* [한국어] 예약 + epilog 영역 사이즈. */
		__be32 w_tfs_psv;
		/* [한국어] wire-side TFS PSV index. */
		__be32 m_tfs_psv;
		/* [한국어] memory-side TFS PSV index. */
	} ext;
	/* [한국어] 확장 signature 파라미터(transit/epilog/PSV). */
	struct mlx5_sig_bsf_inl w_inl;
	/* [한국어] wire 도메인 inline DIF 파라미터. */
	struct mlx5_sig_bsf_inl m_inl;
	/* [한국어] memory 도메인 inline DIF 파라미터. */
};

struct mlx5_wqe_set_psv_seg {
	__be32 psv_index;
	/* [한국어] PSV(Protection State Vector) 인덱스 — NIC 내부에 등록된 sig 객체 참조. */
	__be16 syndrome;
	/* [한국어] 검증 결과 syndrome(에러 발생 시). */
	uint8_t reserved[2];
	/* [한국어] 예약. */
	__be64 transient_signature;
	/* [한국어] transient(=transit) PI 값. SET_PSV WQE로 PSV 상태를 초기화/덮어쓰기. */
};

/*
 * [한국어]
 * mlx5_qp_fm_ce_se_update - 사용자가 전달한 fm_ce_se 바이트의 CE 비트를 sigmode에 맞춰 보정.
 *
 * @qp: 대상 QP(sigmode 사용).
 * @fm_ce_se: 8비트 control field (Fence Mode | Completion Event | Solicited Event).
 *            비트 [3:2]가 CE, [1:0]이 fm/se로 인코딩됨.
 * @return: sigmode를 적용한 새 fm_ce_se 바이트.
 *
 * 동기/배경: SPDK는 사용자에게 단순한 CE flag만 노출하고, sigmode(NONE/ALL/LAST)는 QP 생성 시
 * 결정된다. 매 WQE마다 g_mlx5_ce_map[sigmode][user_ce]로 lookup해 PRM CE 코드로 변환한다.
 * 호출 컨텍스트: WQE 빌드 핫 패스(인라인). 단일 reactor 스레드.
 *
 * 호출 체인:
 *   spdk_mlx5_qp_rdma_write/read → mlx5_qp_rdma_op → mlx5_dma_xfer_full/wrap_around
 *     → [mlx5_qp_fm_ce_se_update] → mlx5_set_ctrl_seg
 */
static inline uint8_t
mlx5_qp_fm_ce_se_update(struct spdk_mlx5_qp *qp, uint8_t fm_ce_se)
{
	uint8_t ce = (fm_ce_se >> 2) & 0x3;
	/* [한국어] CE 비트 [3:2]를 추출(0~3 범위). */

	assert((ce & (~0x3)) == 0);
	/* [한국어] ce가 2비트(0x3 마스크)에 들어가는지 디버그 빌드에서 검증. */
	fm_ce_se &= ~SPDK_MLX5_WQE_CTRL_CE_MASK;
	/* [한국어] 기존 CE 비트를 0으로 클리어. */
	fm_ce_se |= g_mlx5_ce_map[qp->sigmode][ce];
	/* [한국어] sigmode별 매핑 테이블에서 변환된 CE 코드를 OR로 삽입. */

	return fm_ce_se;
	/* [한국어] PRM 인코딩이 적용된 fm_ce_se 반환. ctrl seg fm_ce_se 필드에 직접 기록 가능. */
}

/*
 * [한국어]
 * mlx5_qp_get_wqe_bb - SQ에서 다음에 쓸 WQE Building Block(64B) 슬롯의 가상 주소를 계산.
 *
 * @hw_qp: raw QP 메타데이터.
 * @return: 다음 WQE BB의 시작 주소(void*).
 *
 * SQ는 sq_wqe_cnt × 64B 크기의 ring buffer다. sq_pi는 누적 producer index이고, 슬롯 위치는
 * (sq_pi & (sq_wqe_cnt - 1)) — 즉 power-of-2 크기 가정한 modulo 마스킹. 호출자는 받은 포인터에
 * ctrl/raddr/data segment 등을 바이트 단위로 채워 넣는다.
 *
 * 호출 체인:
 *   mlx5_dma_xfer_full/wrap_around → [mlx5_qp_get_wqe_bb]
 */
static inline void *
mlx5_qp_get_wqe_bb(struct mlx5_hw_qp *hw_qp)
{
	return (void *)hw_qp->sq_addr + (hw_qp->sq_pi & (hw_qp->sq_wqe_cnt - 1)) * MLX5_SEND_WQE_BB;
	/* [한국어] (SQ 시작 주소) + (현재 슬롯 인덱스) × 64B = 다음 WQE 시작 주소.
	 * MLX5_SEND_WQE_BB는 mlx5dv.h에서 정의된 64로, NIC가 인식하는 최소 WQE 단위. */
}

/*
 * [한국어]
 * mlx5_qp_get_next_wqebb - 현재 BB에서 다음 BB로 진행하며 ring wrap-around를 처리.
 *
 * @hw_qp: raw QP.
 * @to_end: SQ 끝까지 남은 바이트 수(in/out). 호출자가 매 BB 진입 시 64B씩 차감되도록 관리.
 * @cur: 현재 BB의 가상 주소.
 * @return: 다음 BB의 가상 주소(끝에 도달하면 SQ 시작 주소로 wrap).
 *
 * 디버그 dump(mlx5_qp_dump_wqe)와 large WQE의 wrap-around 작성 경로에서 사용. wrap이 일어나면
 * to_end를 sq_wqe_cnt × 64B로 리셋해 다시 끝까지의 거리를 표현한다.
 *
 * 호출 체인:
 *   mlx5_qp_dump_wqe → [mlx5_qp_get_next_wqebb]
 */
static inline void *
mlx5_qp_get_next_wqebb(struct mlx5_hw_qp *hw_qp, uint32_t *to_end, void *cur)
{
	*to_end -= MLX5_SEND_WQE_BB;
	/* [한국어] 한 BB 전진했으니 끝까지 거리에서 64B 차감. */
	if (*to_end == 0) { /* wqe buffer wap around */
		/* [한국어] 거리 0 → SQ 끝에 도달, ring 시작으로 되돌아감. */
		*to_end = hw_qp->sq_wqe_cnt * MLX5_SEND_WQE_BB;
		/* [한국어] 끝까지 거리를 다시 SQ 전체 크기로 리셋. */
		return (void *)(uintptr_t)hw_qp->sq_addr;
		/* [한국어] SQ 시작 주소를 다음 BB 위치로 반환. */
	}

	return ((char *)cur) + MLX5_SEND_WQE_BB;
	/* [한국어] wrap 안 일어났으면 cur+64B가 다음 BB. */
}

/*
 * [한국어]
 * mlx5_qp_set_comp - WQE 게시 시 호스트-측 완료 추적 엔트리를 갱신.
 *
 * @qp: QP.
 * @pi: 이번 WQE의 SQ 슬롯 인덱스(=sq_pi & (sq_wqe_cnt-1)).
 * @wr_id: 사용자 work-request ID(완료 보고 시 그대로 반환).
 * @fm_ce_se: 이 WQE의 ctrl seg fm_ce_se(이미 sigmode 변환 적용된 값).
 * @n_bb: 이 WQE가 차지하는 BB 수.
 *
 * CE 비트가 CQ_UPDATE면 NIC이 CQE를 발행할 것이므로 wr_id를 보존하고 누적된 nonsignaled 수를
 * 이 슬롯의 completions에 합산한다. CQE를 발행하지 않으면 nonsignaled_outstanding에만 더한다.
 *
 * 호출 체인:
 *   mlx5_dma_xfer_full/wrap_around → [mlx5_qp_set_comp]
 */
static inline void
mlx5_qp_set_comp(struct spdk_mlx5_qp *qp, uint16_t pi,
		 uint64_t wr_id, uint32_t fm_ce_se, uint32_t n_bb)
{
	qp->completions[pi].wr_id = wr_id;
	/* [한국어] 사용자 wr_id를 슬롯 pi에 보존. CQE 도착 시 mlx5_qp_get_comp_wr_id()가 회수. */
	if ((fm_ce_se & SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE) != SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE) {
		/* non-signaled WQE, accumulate it in outstanding */
		/* [한국어] CQ_UPDATE 비트 없음 → CQE 안 옴. 누적 카운터에 +n_bb. */
		qp->nonsignaled_outstanding += n_bb;
		qp->completions[pi].completions = 0;
		/* [한국어] 이 슬롯에는 0을 기록(자기 자신의 CQE가 안 오므로 회수도 다음 signaled WQE에서). */
		return;
	}

	/* Store number of previous nonsignaled WQEs */
	qp->completions[pi].completions = qp->nonsignaled_outstanding + n_bb;
	/* [한국어] signaled WQE: 자기 + 이전 모든 unsignaled 합계를 기록. CQE 도착 시 한 번에
	 * tx_available 회수. */
	qp->nonsignaled_outstanding = 0;
	/* [한국어] 누적 카운터 리셋. */
}

/* [한국어] 아키텍처별 store fence 매크로 정의.
 * - aarch64: dmb oshst (Outer Shareable Store) — 약한 메모리 모델이라 NIC와 호스트 간 store 순서를
 *   명시적으로 보장해야 한다. UAR(WC 매핑)에 대한 store가 다른 일반 store보다 먼저 보이지 않게.
 * - x86/x64: spdk_wmb() — TSO 모델이라 sfence면 충분하지만 WC 메모리에는 명시적 wmb 필요. */
#if defined(__aarch64__)
#define spdk_memory_bus_store_fence()  asm volatile("dmb oshst" ::: "memory")
#elif defined(__i386__) || defined(__x86_64__)
#define spdk_memory_bus_store_fence() spdk_wmb()
#endif

/*
 * [한국어]
 * mlx5_update_tx_db - Doorbell Record 메모리에 새 sq_pi를 기록.
 *
 * @qp: QP.
 *
 * PRM 8.9.3.1 step 2에 해당. 호스트가 새로 게시한 WQE를 NIC에 알리기 위한 첫 단계로,
 * 8B Doorbell Record(공유 메모리)에 sq_pi(big-endian 32bit)를 쓴다. 이후 store fence를 거쳐
 * UAR(MMIO)에 ctrl seg를 쓰면 NIC이 새 WQE 처리 시작.
 */
static inline void
mlx5_update_tx_db(struct spdk_mlx5_qp *qp)
{
	/*
	 * Use cpu barrier to prevent code reordering
	 */
	spdk_smp_wmb();
	/* [한국어] WQE 메모리 쓰기들이 DBR 갱신 전에 모두 보이도록 컴파일러+CPU store-store 배리어. */

	((uint32_t *)qp->hw.dbr_addr)[MLX5_SND_DBR] = htobe32(qp->hw.sq_pi);
	/* [한국어] DBR 영역의 send 슬롯(MLX5_SND_DBR=1)에 sq_pi를 BE32로 기록. NIC은 BE 머신처럼
	 * 인식하므로 호스트가 LE라면 byte-swap 필요. */
}

/*
 * [한국어]
 * mlx5_flush_tx_db - BlueFlame UAR에 ctrl seg의 첫 64bit를 직접 store해 doorbell 트리거.
 *
 * UAR은 NIC PCI BAR이 유저공간에 매핑된 영역으로, 여기에 store하는 즉시 PCIe write가 NIC에
 * 도달한다. 8B store가 곧바로 doorbell이 되며, ctrl seg 첫 8B에는 opcode/qp_num/pi 등이
 * 포함되어 NIC이 어떤 WQE인지 즉시 파싱 가능(BlueFlame fast path).
 */
static inline void
mlx5_flush_tx_db(struct spdk_mlx5_qp *qp, struct mlx5_wqe_ctrl_seg *ctrl)
{
	*(uint64_t *)(qp->hw.sq_bf_addr) = *(uint64_t *)ctrl;
	/* [한국어] 64bit unaligned-safe store: NIC PCI BAR(sq_bf_addr)에 ctrl 첫 8B를 그대로 복사.
	 * MMIO 쓰기지만 컴파일러는 일반 store처럼 코드를 생성한다 — 매핑이 WC/NC인지에 따라 결합 여부 결정. */
}

/*
 * [한국어]
 * mlx5_ring_tx_db - WQE 게시 후 doorbell을 울려 NIC이 처리를 시작하게 한다.
 *
 * @qp: QP.
 * @ctrl: 가장 최근 WQE의 ctrl seg(BlueFlame fast path에서 직접 store).
 *
 * PRM 8.9.3.1 절차:
 *   1. WQE 메모리 작성 (이미 호출 전에 완료)
 *   2. DBR 갱신
 *   3. UAR(BlueFlame)에 store → NIC trigger
 * WC 매핑 시에는 store가 결합되어 지연될 수 있으므로 fence를 한 번 더 발행한다.
 */
static inline void
mlx5_ring_tx_db(struct spdk_mlx5_qp *qp, struct mlx5_wqe_ctrl_seg *ctrl)
{
	/* 8.9.3.1  Posting a Work Request to Work Queue
	 * 1. Write WQE to the WQE buffer sequentially to previously-posted
	 *    WQE (on WQEBB granularity)
	 *
	 * 2. Update Doorbell Record associated with that queue by writing
	 *    the sq_wqebb_counter or wqe_counter for send and RQ respectively
	 */
	mlx5_update_tx_db(qp);
	/* [한국어] step 2: DBR 갱신(sq_pi 기록). */

	/* Make sure that doorbell record is written before ringing the doorbell */
	spdk_memory_bus_store_fence();
	/* [한국어] DBR write가 UAR write 이전에 PCIe로 빠져나가도록 store fence. */

	/* 3. For send request ring DoorBell by writing to the Doorbell
	 *    Register field in the UAR associated with that queue
	 */
	mlx5_flush_tx_db(qp, ctrl);
	/* [한국어] step 3: BlueFlame UAR에 ctrl 첫 8B store → NIC trigger. */

	/* If UAR is mapped as WC (write combined) we need another fence to
	 * force write. Otherwise it may take a long time.
	 * On BF2/1 uar is mapped as NC (non combined) and fence is not needed
	 * here.
	 */
#if !defined(__aarch64__)
	if (!qp->hw.sq_tx_db_nc) {
		spdk_memory_bus_store_fence();
		/* [한국어] WC 매핑이면 fence를 한 번 더 발행해 store가 즉시 PCIe로 flush되게. */
	}
#endif
}

/* [한국어] 디버그 빌드에서만 WQE dump 함수를 노출. release 빌드에서는 no-op. */
#ifdef DEBUG
void mlx5_qp_dump_wqe(struct spdk_mlx5_qp *qp, int n_wqe_bb);
#else
#define mlx5_qp_dump_wqe(...) do { } while (0)
#endif

/*
 * [한국어]
 * mlx5_qp_wqe_submit - WQE 게시(상태 업데이트만 — doorbell은 별도).
 *
 * @qp: QP.
 * @ctrl: 이번 WQE의 ctrl seg.
 * @n_wqe_bb: 이 WQE가 차지한 BB 수.
 * @ctrlr_pi: 이번 WQE 위치(슬롯 인덱스).
 *
 * "Delay ringing the doorbell": batched submit 패턴을 위해 doorbell은 즉시 울리지 않고
 * 호출자가 spdk_mlx5_qp_complete_send()로 플러시할 때까지 누적한다. 이는 PCIe write 횟수를
 * 줄여 처리량을 높인다.
 */
static inline void
mlx5_qp_wqe_submit(struct spdk_mlx5_qp *qp, struct mlx5_wqe_ctrl_seg *ctrl, uint16_t n_wqe_bb,
		   uint16_t ctrlr_pi)
{
	mlx5_qp_dump_wqe(qp, n_wqe_bb);
	/* [한국어] 디버그 빌드에서 WQE 16-DW dump (release에선 no-op). */

	/* Delay ringing the doorbell */
	qp->hw.sq_pi += n_wqe_bb;
	/* [한국어] producer index를 차지한 BB 수만큼 전진. */
	qp->last_pi = ctrlr_pi;
	/* [한국어] 마지막 ctrl 슬롯 추적(SIG_LAST 모드에서 사용). */
	qp->ctrl = ctrl;
	/* [한국어] 마지막 ctrl 포인터 추적(BlueFlame store 대상). */
}

/*
 * [한국어]
 * mlx5_set_ctrl_seg - WQE control segment 작성 (16B).
 *
 * @ctrl: ctrl seg 메모리 주소.
 * @pi: SQ 슬롯 번호.
 * @opcode: WQE opcode(MLX5_OPCODE_RDMA_WRITE, RDMA_READ, MMO 등).
 * @opmod: opcode modifier(서브타입).
 * @qp_num: 24비트 QP 번호.
 * @fm_ce_se: fence/CE/solicited 비트 인코딩.
 * @ds: data segment count(16B 단위 octoword 수, ctrl 자신 포함).
 * @signature: WQE signature (보통 0; mlx5dv가 자동 채움).
 * @imm: immediate data 또는 invalidate rkey.
 *
 * libmlx5dv의 mlx5dv_set_ctrl_seg 헬퍼를 호출하여 BE 인코딩된 16B ctrl seg를 작성한다.
 * 그 전에 offset 8의 4B를 0으로 클리어하는 이유는 mlx5dv가 일부 필드를 OR로 채우기 때문에
 * 잔여값이 섞이지 않도록 하기 위함.
 */
static inline void
mlx5_set_ctrl_seg(struct mlx5_wqe_ctrl_seg *ctrl, uint16_t pi,
		  uint8_t opcode, uint8_t opmod, uint32_t qp_num,
		  uint8_t fm_ce_se, uint8_t ds,
		  uint8_t signature, uint32_t imm)
{
	*(uint32_t *)((void *)ctrl + 8) = 0;
	/* [한국어] ctrl seg 오프셋 8(=fm_ce_se ~ signature 영역 일부)을 0으로 클리어 — 잔여값 제거. */
	mlx5dv_set_ctrl_seg(ctrl, pi, opcode, opmod, qp_num,
			    fm_ce_se, ds, signature, imm);
	/* [한국어] mlx5dv 헬퍼로 16B ctrl seg를 BE 인코딩하여 기록. */
}

/*
 * [한국어]
 * mlx5_cq_find_qp - CQE의 qp_num으로 spdk_mlx5_qp 포인터를 검색.
 *
 * @cq: 폴링 중인 CQ.
 * @qp_num: CQE에서 추출한 24비트 QP 번호.
 * @return: 매핑된 spdk_mlx5_qp* 또는 NULL(미등록).
 *
 * 2D LUT(상위 12 + 하위 12비트)을 통해 O(1) 검색. 상위 12비트 슬롯의 count가 0이면 빠른 NULL.
 * spdk_unlikely 힌트는 정상 경로에서는 거의 항상 등록된 QP라는 가정.
 *
 * 호출 체인:
 *   spdk_mlx5_cq_poll_completions → [mlx5_cq_find_qp]
 */
static inline struct spdk_mlx5_qp *
mlx5_cq_find_qp(struct spdk_mlx5_cq *cq, uint32_t qp_num)
{
	uint32_t qpn_upper = qp_num >> SPDK_MLX5_QP_NUM_UPPER_SHIFT;
	/* [한국어] 상위 12비트 추출. */
	uint32_t qpn_mask = qp_num & SPDK_MLX5_QP_NUM_LOWER_MASK;
	/* [한국어] 하위 12비트 추출. */

	if (spdk_unlikely(!cq->qps[qpn_upper].count)) {
		/* [한국어] 이 상위 슬롯에 등록된 QP가 0개면 lower table 자체가 NULL. */
		return NULL;
	}
	return cq->qps[qpn_upper].table[qpn_mask];
	/* [한국어] 2D 인덱싱으로 QP 포인터 반환. */
}

/*
 * [한국어]
 * mlx5_get_pd_id - ibv_pd로부터 PD index(pdn)를 추출.
 *
 * @pd: 입력 protection domain 핸들.
 * @pd_id: 출력. NIC 내부 PD 테이블 인덱스(=pdn).
 * @return: 0 성공, 음수 errno 실패.
 *
 * mlx5dv_init_obj(MLX5DV_OBJ_PD)로 driver-specific 정보를 얻고 그 안의 pdn을 반환한다.
 * crypto DEK 생성, MKEY 등록 등 PD index를 직접 PRM 명령에 박아 보내야 할 때 사용.
 *
 * 호출 체인:
 *   mlx5_crypto_dek_init → [mlx5_get_pd_id] → mlx5dv_init_obj
 */
static inline int
mlx5_get_pd_id(struct ibv_pd *pd, uint32_t *pd_id)
{
	struct mlx5dv_pd pd_info;
	/* [한국어] mlx5dv가 채워줄 PD 정보 구조체. */
	struct mlx5dv_obj obj;
	/* [한국어] init_obj 입출력 union. */
	int rc;

	if (!pd) {
		return -EINVAL;
		/* [한국어] NULL pd면 즉시 EINVAL. */
	}
	obj.pd.in = pd;
	/* [한국어] 입력: 일반 ibv_pd 핸들. */
	obj.pd.out = &pd_info;
	/* [한국어] 출력: 채워질 mlx5dv_pd. */
	rc = mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD);
	/* [한국어] mlx5dv에 PD 정보를 요청 — 이 호출은 내부 매핑만 수행하며 시스템 콜 없음. */
	if (rc) {
		return rc;
		/* [한국어] 실패 시 errno 그대로 반환. */
	}
	*pd_id = pd_info.pdn;
	/* [한국어] PD index 추출 — NIC 내부 자원 테이블 참조용. */

	return 0;
}
