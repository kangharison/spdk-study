/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] mlx5 UMR(User-Mode Memory Registration) WQE 빌드 / MKEY 풀 관리 (mlx5_umr.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVIDIA ConnectX 시리즈 NIC의 *UMR(User-Mode Memory Registration)* 기능을 SPDK에서
 * 활용하기 위한 코어 구현이다. UMR은 송신 큐(SQ)에 특별한 WQE(MLX5_OPCODE_UMR)를 직접 작성하여
 * NIC에게 MKEY(Memory Key, RDMA MR을 일반화한 NIC 내부 주소 변환 객체)의 속성을 inline으로
 * 갱신·확장하도록 지시하는 기술이다. SPDK는 이를 통해:
 *   (1) 여러 분산된 SGE를 단일 가상 lkey로 묶어 RDMA WRITE/READ에서 하나의 MR처럼 다룰 수 있게
 *       하고(memory-region indirection — KLM/MTT),
 *   (2) 그 MR을 통과하는 데이터에 inline AES-XTS 암복호화를 적용(crypto BSF),
 *   (3) T10 DIF/CRC32C 같은 서명(signature) 도메인 검증(signature BSF)도 적용한다.
 * 또한 PD(Protection Domain)당 MKEY 풀(SPDK mempool 기반 lock-free per-thread 캐시)을 관리해,
 * I/O 핫패스에서 매번 mlx5dv_devx_obj_create로 MKEY를 만들지 않고 미리 alloc된 풀에서 꺼내 쓴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   accel_mlx5 / module/bdev/crypto / nvmf_rdma offload
 *     → spdk_mlx5_mkey_pool_init/get_ref/get_bulk    (이 파일 — 컨트롤·풀)
 *     → spdk_mlx5_umr_configure / *_crypto / *_sig   (이 파일 — 데이터 평면 WQE 빌드)
 *         → mlx5dv_devx_obj_create / mlx5dv_devx_general_cmd (libmlx5dv direct verbs)
 *           → 커널 RDMA core → mlx5_ib 커널 드라이버 → ConnectX HCA(PRM devx command)
 *         → mlx5_qp_get_wqe_bb / mlx5_qp_wqe_submit (mlx5_qp.c — SQ ring buffer 직접 write)
 *           → BlueFlame/UAR 도어벨 register MMIO write
 * 실행 컨텍스트: SPDK reactor 스레드(유저스페이스, polled-mode). MKEY 풀은 PD별 글로벌 자원이며
 * pthread_mutex로 동기화되지만, 풀에서 꺼낸 후의 핫패스(spdk_mlx5_umr_configure)는 thread-local SQ에서만
 * 작동하므로 lock-free.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(caller): module/accel/mlx5 (AES-XTS·CRC offload), module/bdev/crypto, lib/nvmf/rdma(WIP),
 *   lib/idxd(WIP) — UMR로 등록된 lkey를 RDMA WRITE/READ의 sge에서 사용.
 * - 하위(callee):
 *     libmlx5dv direct verbs: mlx5dv_devx_obj_create/destroy, mlx5dv_devx_general_cmd —
 *       PRM(Programmer's Reference Manual)에 정의된 create_mkey/query_hca_cap/create_psv 명령을
 *       유저스페이스에서 직접 issue(커널은 권한 체크와 fd 매핑만).
 *     mlx5_qp.c: mlx5_qp_get_wqe_bb / mlx5_qp_get_next_wqebb / mlx5_qp_wqe_submit /
 *       mlx5_qp_set_comp / mlx5_set_ctrl_seg / mlx5_qp_fm_ce_se_update — SQ ring buffer 64B WQE BB
 *       단위 슬롯 할당과 도어벨 ring.
 *     SPDK mempool — get_bulk/put_bulk으로 thread-local 캐시 효율 극대화.
 * - 형제(같은 lib/mlx5 내): mlx5_crypto.c는 DEK 등록·tweak_mode capability 조회를 담당하고,
 *   본 파일은 그 DEK·tweak_mode를 받아 inline crypto BSF에 채워 WQE를 만든다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct mlx5_mkey                : mlx5dv_devx_obj 포인터와 mkey 인덱스(24b)를 짝지은 raw MKEY.
 * - struct spdk_mlx5_mkey_pool      : PD+flags(crypto/sig) 조합당 MKEY 풀, mempool + RB tree로 관리.
 * - struct mlx5_mkey_attr           : create_mkey PRM 명령용 속성(addr/size/log_entity_size/bsf_octowords).
 * - struct mlx5_relaxed_ordering_caps : query_hca_cap이 돌려주는 relaxed-ordering 지원 비트들.
 * - mlx5_mkey_create / _destroy     : DEVX_SET 매크로로 create_mkey 명령 빌드 → mlx5dv_devx_obj_create.
 * - spdk_mlx5_mkey_pool_init/destroy/get_ref/put_ref/get_bulk/put_bulk : 풀 라이프사이클 + 핫패스 alloc.
 * - spdk_mlx5_umr_configure         : 일반 UMR WQE(MTT만) — sge 모음 → 단일 lkey 인다이렉션.
 * - spdk_mlx5_umr_configure_crypto  : crypto BSF가 붙은 UMR WQE — AES-XTS inline crypto.
 * - spdk_mlx5_umr_configure_sig     : signature BSF가 붙은 UMR WQE — T10/CRC32C signature.
 * - mlx5_umr_configure_full/_with_wrap_around[_crypto/_sig] : SQ ring buffer 끝 wrap-around 분기.
 * - spdk_mlx5_create_psv / spdk_mlx5_destroy_psv / spdk_mlx5_qp_set_psv : PSV(Protected Signature Validator)
 *   — signature 도메인 검증용 상태 객체. SET_PSV WQE로 transient_signature(crc seed) 주입.
 * - spdk_mlx5_umr_implementer_register/_is_registered : 외부 모듈(accel_mlx5 등)이 UMR을 구현했음을
 *   알리는 글로벌 플래그 — rdma_provider_mlx5_dv.c가 이걸로 accel sequence 지원 여부를 결정.
 */

#include <infiniband/verbs.h>
/* [한국어] libibverbs — ibv_pd, ibv_sge, ibv_context 같은 RDMA verbs 기본 타입.
 * mlx5dv direct verbs는 ibv_pd*를 받아 PRM devx 명령을 issue하므로 같이 include. */

#include "spdk/log.h"
/* [한국어] SPDK 로그 매크로 — SPDK_ERRLOG, SPDK_WARNLOG, SPDK_DEBUGLOG. */
#include "spdk/util.h"
/* [한국어] SPDK 유틸리티 — SPDK_ALIGN_CEIL, SPDK_CEIL_DIV, SPDK_CONTAINEROF 등 매크로. */
#include "spdk/likely.h"
/* [한국어] spdk_unlikely — branch prediction hint, fast path 분기 표시. */
#include "spdk/thread.h"
/* [한국어] spdk_env_get_core_count — cache_per_thread 기본값 계산용. */
#include "spdk/tree.h"
/* [한국어] sys/tree.h 기반 RB tree 매크로 — mkey_index → spdk_mlx5_mkey_pool_obj 매핑 트리. */

#include "spdk_internal/rdma_utils.h"
/* [한국어] SPDK RDMA 유틸리티 — PD ref-count 풀(spdk_rdma_utils_get_pd 등). */
#include "mlx5_priv.h"
/* [한국어] lib/mlx5 내부 헤더 — struct spdk_mlx5_qp, mlx5_hw_qp, DEVX_SET 매크로 일부. */
#include "mlx5_ifc.h"
/* [한국어] mlx5 PRM 인터페이스 정의 — DEVX_SET/DEVX_GET, create_mkey_in/out, query_hca_cap 등
 * NIC가 이해하는 binary command 포맷을 C 매크로로 생성. */

#define MLX5_UMR_POOL_VALID_FLAGS_MASK (~(SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO | SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE))
/* [한국어] 허용되는 풀 플래그 비트마스크의 부정형 — caller가 이 마스크에 걸리는 비트를 켜면 invalid.
 * 즉 CRYPTO/SIGNATURE 외의 비트가 켜져 있으면 -EINVAL. */
#define MLX5_CRYPTO_BSF_P_TYPE_CRYPTO (0x1)
/* [한국어] BSF(Block Signature Format) 세그먼트의 P_TYPE 필드값 — 1=crypto.
 * PRM crypto_bsf_seg의 size_type 필드 하위 비트로 들어간다. */
#define MLX5_CRYPTO_BSF_SIZE_64B (0x2)
/* [한국어] crypto BSF 크기 인코딩 — 2 = 64바이트(MLX5_SEND_WQE_BB 1개 = 64B). bsf_size_sbs 상위 비트로. */

#define MLX5_SIG_BSF_SIZE_32B (0x1)
/* [한국어] signature BSF 크기 인코딩 — 1 = 32바이트. */
/* Transaction Format Selector */
#define MLX5_SIG_BSF_TFS_CRC32C (64)
/* [한국어] signature 도메인의 transaction format = CRC32C. PRM signature_bsf의 ext.tfs_psv 필드 상위. */
#define MLX5_SIG_BSF_TFS_SHIFT (24)
/* [한국어] tfs(8b)를 psv_index(24b) 위로 shift할 양. */
/* Transaction Init/Check_gen bits */
#define MLX5_SIG_BSF_EXT_M_T_CHECK_GEN (1u << 24)
/* [한국어] Memory(host-side) 도메인의 check/gen 활성 비트. */
#define MLX5_SIG_BSF_EXT_M_T_INIT (1u << 25)
/* [한국어] Memory 도메인의 init(seed 리셋) 비트. */
#define MLX5_SIG_BSF_EXT_W_T_CHECK_GEN (1u << 28)
/* [한국어] Wire(network-side) 도메인의 check/gen 활성. */
#define MLX5_SIG_BSF_EXT_W_T_INIT (1u << 29)
/* [한국어] Wire 도메인의 init 비트. */

RB_HEAD(mlx5_mkeys_tree, spdk_mlx5_mkey_pool_obj);
/* [한국어] sys/tree.h RB tree head 매크로 — mkey_index 정렬된 활성 MKEY 객체 트리.
 * sigerr 콜백에서 mkey_index로 pool_obj를 빠르게 찾을 때 사용. */

/*
 * [한국어]
 * struct mlx5_relaxed_ordering_caps - HCA가 지원하는 relaxed ordering 캡 비트 모음
 *
 * PCIe relaxed ordering은 TLP(Transaction Layer Packet)들의 순서 보장을 완화하여 throughput을
 * 높이는 옵션이다. ConnectX HCA는 read/write 방향과 일반/UMR mkey 카테고리별로 개별 캡을 노출한다.
 */
struct mlx5_relaxed_ordering_caps {
	bool relaxed_ordering_write_pci_enabled;
	/* [한국어] PCIe write 방향 relaxed ordering이 PCI 레벨에서 enable 되어 있는가.
	 * 설정자: mlx5_query_relaxed_ordering_caps의 query_hca_cap. */
	bool relaxed_ordering_write;
	/* [한국어] HCA 일반(MR/MKEY) write의 relaxed ordering 지원 여부. */
	bool relaxed_ordering_read;
	/* [한국어] HCA 일반 read의 relaxed ordering 지원 여부. */
	bool relaxed_ordering_write_umr;
	/* [한국어] UMR로 등록된 mkey에 대한 write의 relaxed ordering 지원 여부. */
	bool relaxed_ordering_read_umr;
	/* [한국어] UMR mkey에 대한 read의 relaxed ordering 지원 여부. */
};

/*
 * [한국어]
 * struct mlx5_mkey_attr - mlx5dv create_mkey PRM 명령을 빌드하기 위한 caller 인자 묶음
 */
struct mlx5_mkey_attr {
	uint64_t addr;
	/* [한국어] mkey가 가리키는 시작 가상 주소. UMR로 갱신할 거라 보통 0으로 시작. */
	uint64_t size;
	/* [한국어] mkey 범위 크기(byte). UMR 시점에 실제 길이로 갱신됨. */
	uint32_t log_entity_size;
	/* [한국어] indirection entry의 단위 크기 로그값 — 0이면 KLM(가변 길이) 모드,
	 * 0이 아니면 KLMFBS(고정 블록 크기) 모드. */
	struct mlx5_wqe_data_seg *klm;
	/* [한국어] KLM(Key-Length-Memory) entry 배열 포인터. NULL이면 풀용 free mkey로 만들어 둠. */
	uint32_t klm_count;
	/* [한국어] klm 배열 길이. 0이면 free=1 표시(아직 어떤 메모리도 가리키지 않음). */
	/* Size of bsf in octowords. If 0 then bsf is disabled */
	uint32_t bsf_octowords;
	/* [한국어] BSF(crypto/signature) 영역 크기(16B 단위). 0이면 BSF 비활성. */
	bool crypto_en;
	/* [한국어] crypto BSF 활성 플래그 — true면 inline AES-XTS 가능 mkey가 됨. */
	bool relaxed_ordering_write;
	/* [한국어] 이 mkey에 대한 PCIe write relaxed ordering 켜기. */
	bool relaxed_ordering_read;
	/* [한국어] read 방향 relaxed ordering. */
};

/*
 * [한국어]
 * struct mlx5_mkey - 풀이 관리하는 raw NIC MKEY 단위
 */
struct mlx5_mkey {
	struct mlx5dv_devx_obj *devx_obj;
	/* [한국어] mlx5dv_devx_obj_create가 반환한 NIC 내부 객체 핸들 — destroy 시 이 핸들로 회수. */
	uint32_t mkey;
	/* [한국어] 24비트 mkey_index와 8비트 variant(0x42)를 OR한 lkey/rkey 값.
	 * RDMA WR.sge.lkey나 UMR ctrl_seg.mkey 필드에 그대로 사용. */
	uint64_t addr;
	/* [한국어] 초기 등록 주소(보통 0). UMR로 갱신될 수 있음. */
};

/*
 * [한국어]
 * struct spdk_mlx5_mkey_pool - PD+flags 조합당 MKEY 풀
 *
 * 한 PD 안에서 같은 capability 조합(plain / crypto / signature)의 mkey들을 mempool로 관리.
 * 핫패스는 spdk_mempool_get_bulk로 lock-free하게 thread-local 캐시에서 받아쓴다.
 */
struct spdk_mlx5_mkey_pool {
	struct ibv_pd *pd;
	/* [한국어] 풀이 묶인 Protection Domain — entry 검색 키. */
	struct spdk_mempool *mpool;
	/* [한국어] SPDK mempool — spdk_mlx5_mkey_pool_obj 객체들의 thread-cached pool. */
	struct mlx5_mkeys_tree tree;
	/* [한국어] mkey_index 정렬 RB tree — sigerr 핸들러가 NIC가 알려준 인덱스로 pool_obj를 빠르게 찾을 때. */
	struct mlx5_mkey **mkeys;
	/* [한국어] 풀이 보유한 raw MKEY 포인터 배열(num_mkeys 길이). 풀 destroy 시 모두 mlx5_mkey_destroy. */
	uint32_t num_mkeys;
	/* [한국어] mkeys 배열 길이 — caller가 init 시 요청한 mkey_count. */
	uint32_t refcnt;
	/* [한국어] 이 풀을 참조 중인 외부 모듈 수 — get_ref/put_ref로 변경. 0이어야 destroy 가능. */
	uint32_t flags;
	/* [한국어] 풀 능력 비트 — SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO / _SIGNATURE / 0(plain). */
	TAILQ_ENTRY(spdk_mlx5_mkey_pool) link;
	/* [한국어] g_mkey_pools 글로벌 TAILQ 링크. 보호: g_mkey_pool_lock. */
};

static bool g_umr_implementer_registered;
/* [한국어] UMR을 사용하는 상위 구현체(accel_mlx5 등)가 등록되었는지 글로벌 플래그.
 * rdma_provider_mlx5_dv가 이 값을 보고 NIC offload accel sequence를 광고할지 결정. */

/*
 * [한국어]
 * mlx5_key_obj_compare - RB tree 비교 함수: mkey 인덱스로 정렬
 *
 * @key1, @key2: 비교할 두 풀 객체.
 * @return: -1 / 0 / 1.
 *
 * RB_GENERATE_STATIC이 만든 트리에서 insert/find 시 사용. mkey 필드는 24b NIC 인덱스라 단순 비교.
 */
static int
mlx5_key_obj_compare(struct spdk_mlx5_mkey_pool_obj *key1, struct spdk_mlx5_mkey_pool_obj *key2)
{
	return key1->mkey < key2->mkey ? -1 : key1->mkey > key2->mkey;
	/* [한국어] 단순 mkey index 비교 — 같으면 0(자동, 두 조건 모두 false). */
}

RB_GENERATE_STATIC(mlx5_mkeys_tree, spdk_mlx5_mkey_pool_obj, node, mlx5_key_obj_compare);
/* [한국어] sys/tree.h가 mlx5_mkeys_tree 타입에 대한 RB tree 함수(RB_INSERT/REMOVE/FIND 등)를
 * static 가시성으로 생성. node 필드와 mlx5_key_obj_compare를 사용. */

static TAILQ_HEAD(mlx5_mkey_pool_head,
		  spdk_mlx5_mkey_pool) g_mkey_pools = TAILQ_HEAD_INITIALIZER(g_mkey_pools);
/* [한국어] 활성 MKEY 풀들의 글로벌 TAILQ — PD+flags 매치로 검색. 보호: g_mkey_pool_lock. */
static pthread_mutex_t g_mkey_pool_lock = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] g_mkey_pools 보호 락. 풀 init/destroy/get_ref/put_ref 진입 시 잡음. */

#define SPDK_KLM_MAX_TRANSLATION_ENTRIES_NUM   128
/* [한국어] 한 mkey당 최대 KLM 변환 entry 수 — 128개 SGE를 단일 lkey로 묶을 수 있다. */

/*
 * [한국어]
 * mlx5_mkey_create - PRM create_mkey 명령으로 단일 NIC MKEY를 생성
 *
 * @pd: 등록할 Protection Domain.
 * @attr: 생성할 mkey의 속성(addr/size/log_entity_size/klm/bsf_octowords/crypto_en 등).
 * @return: 새 mlx5_mkey* (성공) 또는 NULL(실패).
 *
 * 동작:
 *  1) DEVX_SET 매크로로 create_mkey_in PRM 명령 페이로드를 채움.
 *  2) klm_count > 0이면 KLM entry들을 페이로드의 klm_pas_mtt 영역에 인코딩(htobe로 NIC byte order).
 *  3) mkey_context의 access flags(lw/lr/rw/rr=1, umr_en=1, qpn=0xffffff = unbound) 설정.
 *  4) crypto_en이면 crypto_en=1, bsf_octwords 지정해 BSF 활성.
 *  5) mlx5dv_devx_obj_create로 NIC에 issue → out에서 mkey_index 추출, variant 0x42와 OR해 24+8b mkey 완성.
 *
 * 호출 컨텍스트: mlx5_mkey_pool_create_mkey에서만 호출. 풀 init/expand 경로.
 *
 * 호출 체인:
 *   spdk_mlx5_mkey_pool_init → mlx5_mkey_pools_init → mlx5_mkey_pool_create_mkey → 이 함수
 *     → mlx5dv_devx_obj_create → 커널 → HCA(create_mkey PRM 명령 실행)
 */
static struct mlx5_mkey *
mlx5_mkey_create(struct ibv_pd *pd, struct mlx5_mkey_attr *attr)
{
	struct mlx5_wqe_data_seg *klms = attr->klm;
	/* [한국어] caller가 넘긴 KLM 배열 시작 포인터 — NULL이면 free mkey(풀용). */
	uint32_t klm_count = attr->klm_count;
	/* [한국어] KLM entry 수 — 0이면 어떤 메모리도 가리키지 않는 free mkey. */
	int in_size_dw = DEVX_ST_SZ_DW(create_mkey_in) +
			 (klm_count ? SPDK_ALIGN_CEIL(klm_count, 4) : 0) * DEVX_ST_SZ_DW(klm);
	/* [한국어] PRM create_mkey_in 명령 페이로드 크기(dword 단위) — 기본 헤더 + KLM entry 영역.
	 * KLM entry는 4개 단위(octword)로 정렬해야 하므로 SPDK_ALIGN_CEIL(klm_count, 4). */
	uint32_t in[in_size_dw];
	/* [한국어] 가변 길이 스택 배열 — NIC로 보낼 입력 명령 버퍼(VLA). */
	uint32_t out[DEVX_ST_SZ_DW(create_mkey_out)] = {0};
	/* [한국어] NIC가 채워 돌려줄 출력 버퍼(0 초기화) — mkey_index가 여기 들어옴. */
	void *mkc;
	/* [한국어] in 버퍼 안의 memory_key_mkey_entry 영역 포인터 — mkey context 필드 작성용. */
	uint32_t translation_size;
	/* [한국어] 4의 배수로 정렬된 KLM entry 수 — PAD entry 채우기 상한. */
	struct mlx5_mkey *cmkey;
	/* [한국어] 반환할 SPDK wrapper 구조체. */
	struct ibv_context *ctx = pd->context;
	/* [한국어] devx 명령을 issue할 디바이스 컨텍스트 — PD에서 추출. */
	uint32_t pd_id = 0;
	/* [한국어] PD의 NIC 내부 번호 — mkey가 어느 PD에 속하는지 지정. */
	uint32_t i;
	/* [한국어] KLM entry 루프 인덱스. */
	uint8_t *klm;
	/* [한국어] in 버퍼의 KLM 영역 작성 커서(바이트 포인터). */

	cmkey = calloc(1, sizeof(*cmkey));
	/* [한국어] wrapper 할당(0 초기화). */
	if (!cmkey) {
		/* [한국어] 메모리 부족 — 에러 로그 후 NULL 반환. */
		SPDK_ERRLOG("failed to alloc cross_mkey\n");
		return NULL;
	}

	memset(in, 0, in_size_dw * 4);
	/* [한국어] 명령 버퍼 전체 0 클리어(dword 4바이트 × 길이). */
	DEVX_SET(create_mkey_in, in, opcode, MLX5_CMD_OP_CREATE_MKEY);
	/* [한국어] PRM opcode = CREATE_MKEY — NIC에게 mkey 생성을 요청. */
	mkc = DEVX_ADDR_OF(create_mkey_in, in, memory_key_mkey_entry);
	/* [한국어] mkey context 서브구조 시작 주소 획득. */

	if (klm_count > 0) {
		/* [한국어] KLM entry가 있으면(즉시 메모리를 가리키는 mkey) entry들을 인코딩. */
		klm = (uint8_t *)DEVX_ADDR_OF(create_mkey_in, in, klm_pas_mtt);
		/* [한국어] KLM/PAS/MTT 변환 테이블 영역 시작 주소. */
		translation_size = SPDK_ALIGN_CEIL(klm_count, 4);
		/* [한국어] 4의 배수로 올림 — entry는 octword 단위 정렬 필요. */

		for (i = 0; i < klm_count; i++) {
			/* [한국어] 실제 KLM entry들을 NIC byte order로 채움. */
			DEVX_SET(klm, klm, byte_count, klms[i].byte_count);
			/* [한국어] 이 entry가 커버하는 바이트 수. */
			DEVX_SET(klm, klm, mkey, klms[i].lkey);
			/* [한국어] 참조할 하위 lkey(이미 등록된 MR의 key). */
			DEVX_SET64(klm, klm, address, klms[i].addr);
			/* [한국어] 가상 주소(64b). */
			klms += DEVX_ST_SZ_BYTES(klm);
			/* [한국어] 다음 KLM entry로 커서 전진(한 entry 크기만큼). */
		}

		for (; i < translation_size; i++) {
			/* [한국어] 정렬 패딩 — 나머지 entry는 0으로 채워 octword 경계 맞춤. */
			DEVX_SET(klm, klms, byte_count, 0x0);
			/* [한국어] 패딩 entry는 byte_count 0. */
			DEVX_SET(klm, klms, mkey, 0x0);
			/* [한국어] 패딩 entry mkey 0. */
			DEVX_SET64(klm, klms, address, 0x0);
			/* [한국어] 패딩 entry 주소 0. */
			klm += DEVX_ST_SZ_BYTES(klm);
			/* [한국어] 패딩 커서 전진. */
		}
	}

	DEVX_SET(mkc, mkc, access_mode_1_0, attr->log_entity_size ?
		 MLX5_MKC_ACCESS_MODE_KLMFBS :
		 MLX5_MKC_ACCESS_MODE_KLMS);
	/* [한국어] access mode 선택 — log_entity_size!=0이면 KLMFBS(고정 블록), 0이면 KLMS(가변 길이). */
	DEVX_SET(mkc, mkc, log_page_size, attr->log_entity_size);
	/* [한국어] 고정 블록 크기 로그값(KLMFBS 모드일 때 의미). */

	mlx5_get_pd_id(pd, &pd_id);
	/* [한국어] PD의 NIC 내부 번호 조회. */
	DEVX_SET(create_mkey_in, in, translations_octword_actual_size, klm_count);
	/* [한국어] 실제 사용 중인 변환 entry 수를 NIC에 알림. */
	if (klm_count == 0) {
		/* [한국어] entry가 없으면 free=1 — 아직 메모리를 가리키지 않는 풀용 mkey. */
		DEVX_SET(mkc, mkc, free, 0x1);
	}
	DEVX_SET(mkc, mkc, lw, 0x1);
	/* [한국어] local write 허용. */
	DEVX_SET(mkc, mkc, lr, 0x1);
	/* [한국어] local read 허용. */
	DEVX_SET(mkc, mkc, rw, 0x1);
	/* [한국어] remote write 허용(RDMA WRITE 대상 가능). */
	DEVX_SET(mkc, mkc, rr, 0x1);
	/* [한국어] remote read 허용. */
	DEVX_SET(mkc, mkc, umr_en, 1);
	/* [한국어] UMR enable — 이후 UMR WQE로 이 mkey를 갱신할 수 있게 함(핵심). */
	DEVX_SET(mkc, mkc, qpn, 0xffffff);
	/* [한국어] qpn=0xffffff = unbound — 특정 QP에 묶이지 않아 어느 QP의 UMR이든 갱신 가능. */
	DEVX_SET(mkc, mkc, pd, pd_id);
	/* [한국어] 소속 PD 번호. */
	DEVX_SET(mkc, mkc, translations_octword_size,
		 SPDK_KLM_MAX_TRANSLATION_ENTRIES_NUM);
	/* [한국어] mkey가 보유 가능한 최대 변환 entry 수(128) — UMR로 늘릴 수 있는 상한. */
	DEVX_SET(mkc, mkc, relaxed_ordering_write,
		 attr->relaxed_ordering_write);
	/* [한국어] PCIe write relaxed ordering 비트 — caps 조회 결과 반영. */
	DEVX_SET(mkc, mkc, relaxed_ordering_read,
		 attr->relaxed_ordering_read);
	/* [한국어] read 방향 relaxed ordering. */
	DEVX_SET64(mkc, mkc, start_addr, attr->addr);
	/* [한국어] 초기 시작 주소(보통 0, UMR로 갱신). */
	DEVX_SET64(mkc, mkc, len, attr->size);
	/* [한국어] 초기 길이(보통 0, UMR로 갱신). */
	DEVX_SET(mkc, mkc, mkey_7_0, 0x42);
	/* [한국어] mkey 하위 8비트 variant = 0x42 — out의 mkey_index와 OR해 최종 lkey 완성. */
	if (attr->crypto_en) {
		/* [한국어] crypto 활성 mkey면 crypto_en 비트 셋(inline AES-XTS 지원). */
		DEVX_SET(mkc, mkc, crypto_en, 1);
	}
	if (attr->bsf_octowords) {
		/* [한국어] BSF 영역이 있으면(crypto 또는 sig) bsf_en + 크기 지정. */
		DEVX_SET(mkc, mkc, bsf_en, 1);
		DEVX_SET(mkc, mkc, bsf_octword_size, attr->bsf_octowords);
	}

	cmkey->devx_obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out,
			  sizeof(out));
	/* [한국어] 완성된 명령을 NIC에 issue — 커널은 권한 체크만, 실제 mkey 생성은 HCA가 수행. */
	if (!cmkey->devx_obj) {
		/* [한국어] 생성 실패 — errno 로깅 후 정리 경로로. */
		SPDK_ERRLOG("mlx5dv_devx_obj_create() failed to create mkey, errno:%d\n", errno);
		goto out_err;
	}

	cmkey->mkey = DEVX_GET(create_mkey_out, out, mkey_index) << 8 | 0x42;
	/* [한국어] 최종 mkey = (24b mkey_index << 8) | 0x42 variant — lkey/rkey로 그대로 사용. */
	return cmkey;

out_err:
	free(cmkey);
	/* [한국어] 실패 시 wrapper 해제. */
	return NULL;
}

/*
 * [한국어]
 * mlx5_mkey_destroy - 단일 raw MKEY 해제 (devx_obj 회수 + free)
 *
 * @mkey: 해제할 MKEY.
 * @return: 0 성공, 음수 = mlx5dv_devx_obj_destroy 실패.
 *
 * 호출 체인:
 *   mlx5_mkey_pool_destroy → 이 함수 → mlx5dv_devx_obj_destroy → 커널 → HCA(destroy_mkey)
 */
static int
mlx5_mkey_destroy(struct mlx5_mkey *mkey)
{
	int ret = 0;

	if (mkey->devx_obj) {
		/* [한국어] NIC 측 mkey 객체 회수 — destroy_mkey PRM 명령. */
		ret = mlx5dv_devx_obj_destroy(mkey->devx_obj);
	}

	free(mkey);
	/* [한국어] SPDK 측 wrapper 해제. */

	return ret;
}

/*
 * [한국어]
 * mlx5_query_relaxed_ordering_caps - HCA가 노출하는 relaxed-ordering capabilities 조회
 *
 * @context: 대상 디바이스의 ibv_context*.
 * @caps: 출력 — 각 카테고리별 지원 비트 채워짐.
 * @return: 0 성공, 음수 = mlx5dv_devx_general_cmd 실패.
 *
 * PRM의 query_hca_cap (op_mod = GENERAL_DEVICE_CAP_2) 명령으로 cmd_hca_cap 구조 안의 relaxed_ordering_*
 * 필드들을 읽는다. mlx5_mkey_pool_create_mkey가 이 값을 보고 mkey의 relaxed_ordering_write/read를 설정.
 */
static int
mlx5_query_relaxed_ordering_caps(struct ibv_context *context,
				 struct mlx5_relaxed_ordering_caps *caps)
{
	uint8_t in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {};
	/* [한국어] query_hca_cap 명령 입력 버퍼(0 초기화). */
	uint8_t out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {};
	/* [한국어] NIC가 캡 비트를 채워 돌려줄 출력 버퍼. */
	int ret;
	/* [한국어] devx 명령 반환값 — 0 성공, 음수 실패. */

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	/* [한국어] opcode = QUERY_HCA_CAP — HCA capability 조회 요청. */
	DEVX_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE_CAP_2);
	/* [한국어] op_mod = GENERAL_DEVICE_CAP_2 — relaxed_ordering_* 비트가 들어있는 cap 페이지 선택. */
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in),
				      out, sizeof(out));
	/* [한국어] general devx 명령 issue — 커널 경유 HCA에서 cap 읽어옴. */
	if (ret) {
		/* [한국어] 조회 실패 시 그대로 에러 반환. */
		return ret;
	}

	caps->relaxed_ordering_write_pci_enabled = DEVX_GET(query_hca_cap_out,
			out, capability.cmd_hca_cap.relaxed_ordering_write_pci_enabled);
	/* [한국어] PCI 레벨에서 write relaxed ordering이 enable 되었는지. */
	caps->relaxed_ordering_write = DEVX_GET(query_hca_cap_out, out,
						capability.cmd_hca_cap.relaxed_ordering_write);
	/* [한국어] 일반 MR/MKEY write relaxed ordering 지원 여부. */
	caps->relaxed_ordering_read = DEVX_GET(query_hca_cap_out, out,
					       capability.cmd_hca_cap.relaxed_ordering_read);
	/* [한국어] 일반 read relaxed ordering 지원 여부. */
	caps->relaxed_ordering_write_umr = DEVX_GET(query_hca_cap_out,
					   out, capability.cmd_hca_cap.relaxed_ordering_write_umr);
	/* [한국어] UMR mkey write relaxed ordering 지원 여부. */
	caps->relaxed_ordering_read_umr = DEVX_GET(query_hca_cap_out,
					  out, capability.cmd_hca_cap.relaxed_ordering_read_umr);
	/* [한국어] UMR mkey read relaxed ordering 지원 여부. */
	return 0;
}

/*
 * [한국어]
 * mlx5_mkey_pool_create_mkey - 풀에 들어갈 단일 MKEY 생성 (flags에 따라 crypto/sig BSF 크기 결정)
 *
 * @_mkey: 출력 — 새 MKEY 포인터.
 * @pd: PD.
 * @caps: 미리 조회된 relaxed-ordering 캡.
 * @flags: SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO/_SIGNATURE 조합.
 * @return: 0 또는 -EINVAL.
 *
 * flags에 따라 bsf_size를 64(crypto) 또는 64(sig)로 잡고(둘 다 동시 활성은 캡 부족으로 금지),
 * mlx5_mkey_create를 호출.
 */
static int
mlx5_mkey_pool_create_mkey(struct mlx5_mkey **_mkey, struct ibv_pd *pd,
			   struct mlx5_relaxed_ordering_caps *caps, uint32_t flags)
{
	struct mlx5_mkey *mkey;
	/* [한국어] 생성된 raw MKEY를 받을 임시 포인터. */
	struct mlx5_mkey_attr mkey_attr = {};
	/* [한국어] create_mkey 인자 묶음(0 초기화). */
	uint32_t bsf_size = 0;
	/* [한국어] BSF 영역 총 크기(byte) — crypto/sig 플래그에 따라 누적. */

	mkey_attr.addr = 0;
	/* [한국어] 풀용 mkey는 초기 주소 0(나중 UMR로 갱신). */
	mkey_attr.size = 0;
	/* [한국어] 초기 길이 0. */
	mkey_attr.log_entity_size = 0;
	/* [한국어] 0 → KLMS(가변 길이) 모드. */
	mkey_attr.relaxed_ordering_write = caps->relaxed_ordering_write;
	/* [한국어] 조회된 캡을 그대로 반영(write). */
	mkey_attr.relaxed_ordering_read = caps->relaxed_ordering_read;
	/* [한국어] read 방향 relaxed ordering 반영. */
	mkey_attr.klm_count = 0;
	/* [한국어] 풀용은 KLM 없이 free mkey로 생성. */
	mkey_attr.klm = NULL;
	/* [한국어] KLM 배열 없음. */
	if (flags & SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO) {
		/* [한국어] crypto 풀이면 crypto BSF(64B) 활성화. */
		mkey_attr.crypto_en = true;
		bsf_size += 64;
	}
	if (flags & SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE) {
		/* [한국어] signature 풀이면 sig BSF(64B) 추가(crypto와 동시 활성은 상위에서 금지됨). */
		bsf_size += 64;
	}
	mkey_attr.bsf_octowords = bsf_size / 16;
	/* [한국어] byte → octoword(16B) 단위 변환 후 attr에 저장(0이면 BSF 비활성). */

	mkey = mlx5_mkey_create(pd, &mkey_attr);
	/* [한국어] 실제 NIC MKEY 생성(create_mkey PRM 명령). */
	if (!mkey) {
		/* [한국어] 생성 실패 — 디바이스명 로깅 후 -EINVAL. */
		SPDK_ERRLOG("Failed to create mkey on dev %s\n", pd->context->device->name);
		return -EINVAL;
	}
	*_mkey = mkey;
	/* [한국어] 출력 포인터에 생성된 mkey 전달. */

	return 0;
}

/*
 * [한국어]
 * mlx5_set_mkey_in_pool - SPDK mempool ctor 콜백: pool_obj 객체와 raw MKEY를 짝맞춤
 *
 * @mp: mempool 핸들.
 * @cb_arg: spdk_mempool_create_ctor 시 넘긴 spdk_mlx5_mkey_pool*.
 * @_mkey: 초기화할 spdk_mlx5_mkey_pool_obj 객체.
 * @obj_idx: 이 객체의 인덱스(0 ~ num_mkeys-1).
 *
 * 동작: pool->mkeys[obj_idx]가 가진 mkey 인덱스를 pool_obj에 복사하고 RB tree에 insert.
 * pool_obj는 외부에 노출되는 핸들이고, raw mlx5_mkey는 내부 자원이다.
 */
static void
mlx5_set_mkey_in_pool(struct spdk_mempool *mp, void *cb_arg, void *_mkey, unsigned obj_idx)
{
	struct spdk_mlx5_mkey_pool_obj *mkey = _mkey;
	/* [한국어] mempool이 초기화 중인 외부 노출 pool_obj. */
	struct spdk_mlx5_mkey_pool *pool = cb_arg;
	/* [한국어] ctor 인자로 받은 소속 풀. */

	assert(obj_idx < pool->num_mkeys);
	/* [한국어] 인덱스 범위 검증 — mkeys 배열 길이 내. */
	assert(pool->mkeys[obj_idx] != NULL);
	/* [한국어] 대응하는 raw mkey가 이미 생성되어 있어야 함. */
	mkey->mkey = pool->mkeys[obj_idx]->mkey;
	/* [한국어] raw MKEY의 lkey 값을 pool_obj에 복사 — 핫패스에서 이 값으로 UMR/sge 작성. */
	mkey->pool_flag = pool->flags & 0xf;
	/* [한국어] 풀 능력 플래그(crypto/sig) 하위 4비트를 obj에 기록. */
	mkey->sig.sigerr_count = 1;
	/* [한국어] signature error toggle 카운터 초기값 1 — UMR mkey_seg_sig가 LSB를 toggle 비트로 사용. */
	mkey->sig.sigerr = false;
	/* [한국어] 아직 서명 오류 없음. */

	RB_INSERT(mlx5_mkeys_tree, &pool->tree, mkey);
	/* [한국어] mkey_index 정렬 RB tree에 등록 — sigerr 핸들러가 인덱스로 역참조할 수 있게. */
}

static const char *g_mkey_pool_names[] = {
	[SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO] = "crypto",
	[SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE] = "signature",
};
/* [한국어] flag 비트 → 풀 이름 문자열(mempool 이름 prefix). 디버그/통계용. */

/*
 * [한국어]
 * mlx5_mkey_pool_destroy - 풀 자원 해제 (mempool + raw mkeys + TAILQ 해제)
 *
 * @pool: 해제할 풀.
 *
 * 호출 컨텍스트: g_mkey_pool_lock 보유 상태. 외부 spdk_mlx5_mkey_pool_destroy 또는 init 실패 경로.
 *
 * 호출 체인:
 *   spdk_mlx5_mkey_pool_destroy → 이 함수 → spdk_mempool_free / mlx5_mkey_destroy
 */
static void
mlx5_mkey_pool_destroy(struct spdk_mlx5_mkey_pool *pool)
{
	uint32_t i;
	/* [한국어] mkeys 배열 순회 인덱스. */

	if (pool->mpool) {
		/* [한국어] mempool이 생성되어 있으면 먼저 해제(pool_obj들 회수). */
		spdk_mempool_free(pool->mpool);
	}
	if (pool->mkeys) {
		/* [한국어] raw mkey 배열이 있으면 각 NIC mkey를 destroy. */
		for (i = 0; i < pool->num_mkeys; i++) {
			if (pool->mkeys[i]) {
				/* [한국어] 부분 init 실패 시 NULL일 수 있으므로 가드 후 destroy. */
				mlx5_mkey_destroy(pool->mkeys[i]);
				pool->mkeys[i] = NULL;
			}
		}
		free(pool->mkeys);
		/* [한국어] 포인터 배열 자체 해제. */
	}
	TAILQ_REMOVE(&g_mkey_pools, pool, link);
	/* [한국어] 글로벌 풀 리스트에서 제거(호출자가 g_mkey_pool_lock 보유 중). */
	free(pool);
	/* [한국어] 풀 구조체 해제. */
}

/*
 * [한국어]
 * mlx5_mkey_pools_init - 풀 객체 alloc + relaxed-ordering 캡 조회 + N개 mkey 생성 + mempool 생성
 *
 * @params: 풀 파라미터(mkey_count, cache_per_thread, flags).
 * @pd: 등록할 PD.
 * @return: 0 또는 음수 errno. 실패 시 부분 alloc된 자원도 정리.
 *
 * 호출 체인:
 *   spdk_mlx5_mkey_pool_init → 이 함수 → mlx5_query_relaxed_ordering_caps → mlx5_mkey_pool_create_mkey
 *     → spdk_mempool_create_ctor(set_mkey_in_pool 콜백) → mempool가 모든 pool_obj 초기화
 */
static int
mlx5_mkey_pools_init(struct spdk_mlx5_mkey_pool_param *params, struct ibv_pd *pd)
{
	struct spdk_mlx5_mkey_pool *new_pool;
	/* [한국어] 새로 생성할 풀 구조체. */
	struct mlx5_mkey **mkeys;
	/* [한국어] raw MKEY 포인터 배열(mkey_count 길이). */
	struct mlx5_relaxed_ordering_caps caps;
	/* [한국어] 한 번 조회해 모든 mkey에 공유할 relaxed-ordering 캡. */
	uint32_t j, pdn;
	/* [한국어] j=mkey 생성 루프 인덱스, pdn=PD 번호(풀 이름·로그용). */
	int rc;
	/* [한국어] 반환 코드 — 실패 시 err 라벨로 점프. */
	char pool_name[32];
	/* [한국어] mempool 이름 버퍼(dev_flags_pdn 형식). */

	new_pool = calloc(1, sizeof(*new_pool));
	/* [한국어] 풀 구조체 할당. */
	if (!new_pool) {
		/* [한국어] 메모리 부족 — 단, err 라벨이 new_pool을 destroy하므로 여기선 직접 반환. */
		rc = -ENOMEM;
		goto err;
	}
	TAILQ_INSERT_TAIL(&g_mkey_pools, new_pool, link);
	/* [한국어] 글로벌 리스트에 미리 등록 — 실패 시 destroy가 TAILQ_REMOVE로 정리하게. */
	rc = mlx5_query_relaxed_ordering_caps(pd->context, &caps);
	/* [한국어] HCA relaxed-ordering 캡 조회(mkey마다 반복 안 하려고 1회만). */
	if (rc) {
		/* [한국어] 캡 조회 실패 — 로깅 후 정리. */
		SPDK_ERRLOG("Failed to get relaxed ordering capabilities, dev %s\n",
			    pd->context->device->dev_name);
		goto err;
	}
	mkeys = calloc(params->mkey_count, sizeof(struct mlx5_mkey *));
	/* [한국어] raw mkey 포인터 배열 할당. */
	if (!mkeys) {
		rc = -ENOMEM;
		goto err;
	}
	new_pool->mkeys = mkeys;
	/* [한국어] 풀에 배열 연결(destroy가 회수할 수 있게). */
	new_pool->num_mkeys = params->mkey_count;
	/* [한국어] 배열 길이 기록. */
	new_pool->pd = pd;
	/* [한국어] 소속 PD 기록(검색 키). */
	new_pool->flags = params->flags;
	/* [한국어] 능력 플래그 기록. */
	for (j = 0; j < params->mkey_count; j++) {
		/* [한국어] 요청한 개수만큼 NIC mkey 생성. */
		rc = mlx5_mkey_pool_create_mkey(&mkeys[j], pd, &caps, params->flags);
		if (rc) {
			/* [한국어] 하나라도 실패하면 정리(이미 만든 것도 destroy가 회수). */
			goto err;
		}
	}
	rc = mlx5_get_pd_id(pd, &pdn);
	/* [한국어] mempool 이름에 쓸 PD 번호 조회. */
	if (rc) {
		SPDK_ERRLOG("Failed to get pdn, pd %p\n", pd);
		goto err;
	}
	rc = snprintf(pool_name, 32, "%s_%s_%04u", pd->context->device->name,
		      g_mkey_pool_names[new_pool->flags], pdn);
	/* [한국어] mempool 이름 = "디바이스명_플래그명_PD번호" (디버그/통계 식별용). */
	if (rc < 0) {
		/* [한국어] snprintf 인코딩 오류. */
		goto err;
	}
	RB_INIT(&new_pool->tree);
	/* [한국어] RB tree 초기화(ctor가 각 obj를 insert하기 전에). */
	new_pool->mpool = spdk_mempool_create_ctor(pool_name, params->mkey_count,
			  sizeof(struct spdk_mlx5_mkey_pool_obj),
			  params->cache_per_thread, SPDK_ENV_NUMA_ID_ANY,
			  mlx5_set_mkey_in_pool, new_pool);
	/* [한국어] DPDK mempool 생성 + ctor로 모든 pool_obj를 raw mkey와 짝맞춤.
	 * cache_per_thread는 코어별 thread-local 캐시 크기(핫패스 lock-free의 핵심).
	 * SPDK_ENV_NUMA_ID_ANY = NUMA 노드 무관 hugepage 사용. */
	if (!new_pool->mpool) {
		/* [한국어] mempool 생성 실패(hugepage 부족 등). */
		SPDK_ERRLOG("Failed to create mempool\n");
		rc = -ENOMEM;
		goto err;
	}

	return 0;

err:
	mlx5_mkey_pool_destroy(new_pool);
	/* [한국어] 부분 alloc된 자원(mempool/mkeys/TAILQ) 모두 정리. */

	return rc;
}

/*
 * [한국어]
 * mlx5_mkey_pool_get - g_mkey_pools에서 (pd, flags) 매치 풀 검색 (락은 외부에서)
 *
 * @return: 매치 풀 또는 NULL.
 */
static struct spdk_mlx5_mkey_pool *
mlx5_mkey_pool_get(struct ibv_pd *pd, uint32_t flags)
{
	struct spdk_mlx5_mkey_pool *pool;

	TAILQ_FOREACH(pool, &g_mkey_pools, link) {
		/* [한국어] PD + flags 양쪽 매치 entry 검색. */
		if (pool->pd == pd && pool->flags == flags) {
			return pool;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * spdk_mlx5_mkey_pool_init - 외부 API: PD+flags 조합 MKEY 풀을 새로 생성
 *
 * @params: 풀 파라미터(mkey_count, cache_per_thread, flags).
 * @pd: 등록할 PD.
 * @return: 0 / -EINVAL / -EEXIST / 음수 errno.
 *
 * 동작: 입력 validation → 동일 (pd, flags) 풀이 이미 있으면 -EEXIST → 새로 생성.
 * cache_per_thread가 0이면 mkey_count의 75%를 코어 수로 나눠 자동 설정.
 *
 * 호출 체인:
 *   accel_mlx5/RPC handler → 이 함수 → mlx5_mkey_pools_init → mlx5_mkey_pool_create_mkey → NIC
 */
int
spdk_mlx5_mkey_pool_init(struct spdk_mlx5_mkey_pool_param *params, struct ibv_pd *pd)
{
	int rc;
	/* [한국어] 내부 init 반환값. */

	if (!pd) {
		/* [한국어] PD 필수. */
		return -EINVAL;
	}

	if (!params || !params->mkey_count) {
		/* [한국어] params와 mkey_count(>0) 필수. */
		return -EINVAL;
	}
	if ((params->flags & MLX5_UMR_POOL_VALID_FLAGS_MASK) != 0) {
		/* [한국어] CRYPTO/SIGNATURE 외 비트가 켜져 있으면 invalid. */
		SPDK_ERRLOG("Invalid flags %x\n", params->flags);
		return -EINVAL;
	}
	if ((params->flags & (SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO | SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE)) ==
	    (SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO | SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE)) {
		/* [한국어] crypto와 signature 동시 활성은 HCA BSF 캡 부족으로 미지원. */
		SPDK_ERRLOG("Both crypto and signature capabilities are not supported\n");
		return -EINVAL;
	}
	if (params->cache_per_thread > params->mkey_count || !params->cache_per_thread) {
		/* [한국어] 캐시 크기가 비정상(전체 초과) 또는 0이면 자동 산정 —
		 * 전체 mkey의 75%를 코어 수로 균등 분배(핫패스 캐시 hit율 최적화). */
		params->cache_per_thread = params->mkey_count * 3 / 4 / spdk_env_get_core_count();
	}

	pthread_mutex_lock(&g_mkey_pool_lock);
	/* [한국어] 글로벌 풀 리스트 보호 락 획득(컨트롤 평면 — 핫패스 아님). */
	if (mlx5_mkey_pool_get(pd, params->flags) != NULL) {
		/* [한국어] 동일 (pd, flags) 풀이 이미 있으면 중복 생성 거부. */
		pthread_mutex_unlock(&g_mkey_pool_lock);
		return -EEXIST;
	}

	rc = mlx5_mkey_pools_init(params, pd);
	/* [한국어] 락 보유 상태로 실제 풀 생성. */
	pthread_mutex_unlock(&g_mkey_pool_lock);
	/* [한국어] 락 해제. */

	return rc;
}

/*
 * [한국어]
 * spdk_mlx5_mkey_pool_destroy - 외부 API: 풀 해제 (refcnt>0이면 -EAGAIN)
 *
 * @flags, @pd: 풀 식별자.
 * @return: 0 성공, -EINVAL/-ENODEV/-EAGAIN.
 */
int
spdk_mlx5_mkey_pool_destroy(uint32_t flags, struct ibv_pd *pd)
{
	struct spdk_mlx5_mkey_pool *pool;
	/* [한국어] 검색된 풀. */
	int rc = 0;
	/* [한국어] 결과 코드(기본 성공). */

	if (!pd) {
		/* [한국어] PD 필수. */
		return -EINVAL;
	}

	if ((flags & MLX5_UMR_POOL_VALID_FLAGS_MASK) != 0) {
		/* [한국어] 유효하지 않은 플래그 비트. */
		SPDK_ERRLOG("Invalid flags %x\n", flags);
		return -EINVAL;
	}

	pthread_mutex_lock(&g_mkey_pool_lock);
	/* [한국어] 글로벌 리스트 보호 락. */
	pool = mlx5_mkey_pool_get(pd, flags);
	/* [한국어] (pd, flags)로 풀 검색. */
	if (!pool) {
		/* [한국어] 존재하지 않으면 -ENODEV. */
		SPDK_ERRLOG("Cant find a pool for PD %p, flags %x\n", pd, flags);
		pthread_mutex_unlock(&g_mkey_pool_lock);
		return -ENODEV;
	}
	if (pool->refcnt) {
		/* [한국어] 아직 참조 중인 모듈이 있으면 삭제 거부(-EAGAIN). */
		SPDK_WARNLOG("Can't delete pool pd %p, dev %s\n", pool->pd, pool->pd->context->device->dev_name);
		rc = -EAGAIN;
	} else {
		/* [한국어] refcnt==0이면 안전하게 해제. */
		mlx5_mkey_pool_destroy(pool);
	}
	pthread_mutex_unlock(&g_mkey_pool_lock);
	/* [한국어] 락 해제. */

	return rc;
}

/*
 * [한국어]
 * spdk_mlx5_mkey_pool_get_ref - 풀 참조 +1 (caller가 풀을 사용 중임을 표시)
 *
 * @return: 풀 핸들 또는 NULL.
 *
 * 호출 컨텍스트: accel_mlx5 같은 상위 모듈이 풀에 접근하기 전에 호출하여 destroy가 동시에 일어나지
 * 않도록 보호.
 */
struct spdk_mlx5_mkey_pool *
spdk_mlx5_mkey_pool_get_ref(struct ibv_pd *pd, uint32_t flags)
{
	struct spdk_mlx5_mkey_pool *pool;
	/* [한국어] 검색된 풀. */

	if ((flags & MLX5_UMR_POOL_VALID_FLAGS_MASK) != 0) {
		/* [한국어] 유효하지 않은 플래그. */
		SPDK_ERRLOG("Invalid flags %x\n", flags);
		return NULL;
	}

	pthread_mutex_lock(&g_mkey_pool_lock);
	/* [한국어] refcnt 갱신은 락 보호 하에. */
	pool = mlx5_mkey_pool_get(pd, flags);
	/* [한국어] (pd, flags) 풀 검색. */
	if (pool) {
		/* [한국어] 존재 시 참조 +1 — destroy가 동시에 일어나지 못하도록. */
		pool->refcnt++;
	}
	pthread_mutex_unlock(&g_mkey_pool_lock);
	/* [한국어] 락 해제. */

	return pool;
}

/*
 * [한국어]
 * spdk_mlx5_mkey_pool_put_ref - 풀 참조 -1.
 */
void
spdk_mlx5_mkey_pool_put_ref(struct spdk_mlx5_mkey_pool *pool)
{
	pthread_mutex_lock(&g_mkey_pool_lock);
	/* [한국어] refcnt 갱신 락 보호. */
	pool->refcnt--;
	/* [한국어] 참조 -1 — 0이 되면 destroy 가능. */
	pthread_mutex_unlock(&g_mkey_pool_lock);
	/* [한국어] 락 해제. */
}

/*
 * [한국어]
 * spdk_mlx5_mkey_pool_get_bulk - 핫패스 alloc: 풀에서 N개 mkey_pool_obj를 빼서 caller 배열에 채움
 *
 * @return: spdk_mempool_get_bulk 반환값(0 성공). thread-local 캐시에서 lock-free 처리.
 */
int
spdk_mlx5_mkey_pool_get_bulk(struct spdk_mlx5_mkey_pool *pool,
			     struct spdk_mlx5_mkey_pool_obj **mkeys, uint32_t mkeys_count)
{
	assert(pool->mpool);
	/* [한국어] 풀이 정상 init되어 mempool이 존재해야 함. */

	return spdk_mempool_get_bulk(pool->mpool, (void **)mkeys, mkeys_count);
	/* [한국어] DPDK rte_mempool 기반 — 코어 캐시 hit 시 syscall/lock 없이 즉시 반환. */
}

/*
 * [한국어]
 * spdk_mlx5_mkey_pool_put_bulk - 핫패스 free: N개 객체를 풀에 반납.
 */
void
spdk_mlx5_mkey_pool_put_bulk(struct spdk_mlx5_mkey_pool *pool,
			     struct spdk_mlx5_mkey_pool_obj **mkeys, uint32_t mkeys_count)
{
	assert(pool->mpool);
	/* [한국어] mempool 존재 검증. */

	spdk_mempool_put_bulk(pool->mpool, (void **)mkeys, mkeys_count);
	/* [한국어] N개 객체를 thread-local 캐시로 반납(lock-free). */
}

/*
 * [한국어]
 * _mlx5_set_umr_ctrl_seg_mtt - UMR ctrl segment의 MTT 부분 채움 (mkey_mask 추가 가능)
 *
 * UMR ctrl segment는 NIC에게 "이 mkey의 어떤 필드를 갱신할 것인가"를 mkey_mask로 알리고,
 * 변환 entry 수(klms_octowords)와 INLINE 플래그를 설정한다. 모든 UMR 변형이 공통으로 사용.
 */
static inline void
_mlx5_set_umr_ctrl_seg_mtt(struct mlx5_wqe_umr_ctrl_seg *ctrl, uint32_t klms_octowords,
			   uint64_t mkey_mask)
{
	ctrl->flags |= MLX5_WQE_UMR_CTRL_FLAG_INLINE;
	/* [한국어] INLINE 플래그 — KLM entry들을 WQE 안에 직접 담는다(별도 버퍼 참조 안 함). */
	ctrl->klm_octowords = htobe16(klms_octowords);
	/* [한국어] 변환 entry 수(octword 단위)를 BE로 인코딩. */
	/*
	 * Going to modify two properties of KLM mkey:
	 *  1. 'free' field: change this mkey from in free to in use
	 *  2. 'len' field: to include the total bytes in iovec
	 */
	mkey_mask |= MLX5_WQE_UMR_CTRL_MKEY_MASK_FREE | MLX5_WQE_UMR_CTRL_MKEY_MASK_LEN;
	/* [한국어] 갱신할 mkey 필드 마스크에 free(in-use 전환)와 len(총 길이)을 추가. */

	ctrl->mkey_mask |= htobe64(mkey_mask);
	/* [한국어] 최종 mkey_mask를 BE로 기록 — NIC는 마스크에 켜진 필드만 갱신. */
}

/*
 * [한국어]
 * mlx5_set_umr_ctrl_seg_mtt - plain UMR ctrl seg(시그너처 마스크 없음).
 */
static inline void
mlx5_set_umr_ctrl_seg_mtt(struct mlx5_wqe_umr_ctrl_seg *ctrl, uint32_t klms_octowords)
{
	_mlx5_set_umr_ctrl_seg_mtt(ctrl, klms_octowords, 0);
	/* [한국어] 추가 mkey_mask 없이 호출(free/len만 갱신). */
}

/*
 * [한국어]
 * mlx5_set_umr_ctrl_seg_mtt_sig - signature UMR ctrl seg (sig_err 마스크 추가)
 *
 * sigerr는 NIC가 signature 검증 실패를 알릴 때 mkey 상태에 기록되는 비트로, mkey_mask에 포함시켜
 * 이번 UMR로 reset함을 NIC에 알린다.
 */
static inline void
mlx5_set_umr_ctrl_seg_mtt_sig(struct mlx5_wqe_umr_ctrl_seg *ctrl, uint32_t klms_octowords)
{
	_mlx5_set_umr_ctrl_seg_mtt(ctrl, klms_octowords, MLX5_WQE_UMR_CTRL_MKEY_MASK_SIG_ERR);
	/* [한국어] SIG_ERR 마스크 추가 — 이번 UMR로 이전 signature 오류 상태를 reset함을 NIC에 알림. */
}

/*
 * [한국어]
 * mlx5_set_umr_ctrl_seg_bsf_size - BSF 영역 크기를 octoword(16B) 단위로 ctrl_seg에 인코딩
 *
 * bsf_size는 바이트 단위. 16으로 나누고 4의 배수로 ceil-align해서 NIC가 기대하는 단위로 변환.
 */
static inline void
mlx5_set_umr_ctrl_seg_bsf_size(struct mlx5_wqe_umr_ctrl_seg *ctrl, int bsf_size)
{
	ctrl->bsf_octowords = htobe16(SPDK_ALIGN_CEIL(SPDK_CEIL_DIV(bsf_size, 16), 4));
	/* [한국어] big-endian으로 변환 — NIC는 항상 BE로 필드를 읽음. */
}

/*
 * [한국어]
 * mlx5_set_umr_mkey_seg_mtt - mkey_context_seg의 len 필드를 umr_len으로 채움
 *
 * mkey의 유효 길이를 NIC에 알리는 단계 — RDMA WR가 이 mkey로 access할 때 이 범위 밖이면 PROT 에러.
 */
static inline void
mlx5_set_umr_mkey_seg_mtt(struct mlx5_wqe_mkey_context_seg *mkey,
			  struct spdk_mlx5_umr_attr *umr_attr)
{
	mkey->len = htobe64(umr_attr->umr_len);
	/* [한국어] mkey 유효 길이를 BE로 기록 — 이 범위 밖 access는 PROT 에러. */
}

/*
 * [한국어]
 * mlx5_set_umr_mkey_seg - mkey_context_seg 전체를 zero-init 후 len만 채움
 */
static void
mlx5_set_umr_mkey_seg(struct mlx5_wqe_mkey_context_seg *mkey,
		      struct spdk_mlx5_umr_attr *umr_attr)
{
	memset(mkey, 0, 64);
	/* [한국어] WQE segment는 64B 단위. 이전 cycle의 stale 데이터 제거. */
	mlx5_set_umr_mkey_seg_mtt(mkey, umr_attr);
	/* [한국어] len 필드만 채움(나머지는 0). */
}

/*
 * [한국어]
 * mlx5_set_umr_mkey_seg_sig - mkey_context_seg에 signature 도메인 플래그 채움
 *
 * sigerr_count의 LSB를 bit 26 위치(flags_pd 필드 안)에 인코딩 — NIC가 toggle 비트로 추적.
 */
static void
mlx5_set_umr_mkey_seg_sig(struct mlx5_wqe_mkey_context_seg *mkey,
			  struct spdk_mlx5_umr_sig_attr *sig_attr)
{
	mkey->flags_pd = htobe32((sig_attr->sigerr_count & 1) << 26);
	/* [한국어] sigerr_count의 LSB를 bit26에 인코딩 — NIC가 signature 오류 toggle 비트로 추적. */
}

/*
 * [한국어]
 * mlx5_set_umr_inline_klm_seg - 단일 KLM entry 빌드 (ibv_sge → klm)
 *
 * KLM entry는 NIC가 mkey의 indirection을 따라갈 때 참조하는 (length, mkey/lkey, address) 트리플.
 * 모든 필드는 big-endian.
 */
static inline void
mlx5_set_umr_inline_klm_seg(struct mlx5_wqe_umr_klm_seg *klm, struct ibv_sge *sge)
{
	klm->byte_count = htobe32(sge->length);
	/* [한국어] 이 entry가 커버하는 길이(BE). */
	klm->mkey = htobe32(sge->lkey);
	/* [한국어] 참조할 하위 lkey(이미 등록된 MR)(BE). */
	klm->address = htobe64(sge->addr);
	/* [한국어] 가상 주소(BE). */
}

/*
 * [한국어]
 * mlx5_build_inline_mtt - SQ ring buffer에 KLM entry들을 wrap-around 안전하게 작성
 *
 * @qp: 대상 hw QP — sq_pi, sq_wqe_cnt 등 ring 상태 보유.
 * @to_end: 입출력 — SQ ring buffer 끝까지 남은 바이트 수, get_next_wqebb 호출마다 갱신.
 * @dst_klm: 첫 KLM 작성 위치(현재 WQE BB 내부).
 * @umr_attr: SGE 배열과 sge_count 보유.
 * @return: 마지막 KLM 다음 WQE BB의 시작 포인터(다음 segment 작성 위치).
 *
 * SQ ring은 cyclic buffer이므로 한 WQE가 끝에서 wrap될 수 있다. 4개 KLM(=64B=1 WQE BB) 단위로
 * mlx5_qp_get_next_wqebb를 호출해 ring 경계를 안전하게 처리한다.
 */
static void *
mlx5_build_inline_mtt(struct mlx5_hw_qp *qp, uint32_t *to_end, struct mlx5_wqe_umr_klm_seg *dst_klm,
		      struct spdk_mlx5_umr_attr *umr_attr)
{
	struct ibv_sge *src_sge = umr_attr->sge;
	/* [한국어] 원본 SGE 배열 커서. */
	int num_wqebbs = umr_attr->sge_count / 4;
	/* [한국어] 4개 KLM = 1 WQE BB(64B). 완전히 채워지는 BB 개수. */
	int tail = umr_attr->sge_count & 0x3;
	/* [한국어] 4로 나눈 나머지 — 마지막 부분 BB의 entry 수(0~3). */
	int i;
	/* [한국어] 루프 인덱스. */

	for (i = 0; i < num_wqebbs; i++) {
		/* [한국어] 4개씩 묶어 한 WQE BB를 채운 뒤 ring 경계를 안전하게 넘어감. */
		mlx5_set_umr_inline_klm_seg(&dst_klm[0], src_sge++);
		/* [한국어] BB 내 첫 KLM. */
		mlx5_set_umr_inline_klm_seg(&dst_klm[1], src_sge++);
		/* [한국어] 두 번째 KLM. */
		mlx5_set_umr_inline_klm_seg(&dst_klm[2], src_sge++);
		/* [한국어] 세 번째 KLM. */
		mlx5_set_umr_inline_klm_seg(&dst_klm[3], src_sge++);
		/* [한국어] 네 번째 KLM(이 BB 완성). */
		/* sizeof(*dst_klm) * 4 == MLX5_SEND_WQE_BB */
		dst_klm = mlx5_qp_get_next_wqebb(qp, to_end, dst_klm);
		/* [한국어] 다음 WQE BB로 이동 — SQ ring 끝이면 begin으로 wrap, to_end 갱신. */
	}

	if (!tail) {
		/* [한국어] 나머지 entry가 없으면 여기서 종료. */
		return dst_klm;
	}

	for (i = 0; i < tail; i++) {
		/* [한국어] 부분 BB의 남은 entry(1~3개) 채움. */
		mlx5_set_umr_inline_klm_seg(&dst_klm[i], src_sge++);
	}

	/* Fill PAD entries to make whole mtt aligned to 64B(MLX5_SEND_WQE_BB) */
	memset(&dst_klm[i], 0, MLX5_SEND_WQE_BB - sizeof(struct mlx5_wqe_umr_klm_seg) * tail);
	/* [한국어] 부분 BB의 나머지를 0 패딩 — 전체 MTT를 64B 정렬 유지(NIC 요구). */

	return mlx5_qp_get_next_wqebb(qp, to_end, dst_klm);
	/* [한국어] 패딩 BB 다음 위치(BSF 작성 시작점) 반환. */
}

/*
 * [한국어]
 * mlx5_set_umr_crypto_bsf_seg - crypto BSF(Block Signature Format) 세그먼트 빌드
 *
 * @bsf: 작성 대상 64B BSF 세그먼트.
 * @attr: tweak_mode, xts_iv(LBA), enc_order, bs_selector, dek_obj_id, keytag.
 * @raw_data_size: 평문 데이터 길이.
 * @bsf_size: BSF 크기 인코딩(64B=2).
 *
 * AES-XTS는 LBA를 tweak로 사용. tweak_mode에 따라 LBA를 16B IV의 어느 쪽 8B에 배치할지(LE/BE) 결정.
 * dek_obj_id는 mlx5_crypto.c가 등록한 NIC 내부 key table 인덱스.
 */
static inline void
mlx5_set_umr_crypto_bsf_seg(struct mlx5_crypto_bsf_seg *bsf, struct spdk_mlx5_umr_crypto_attr *attr,
			    uint32_t raw_data_size, uint8_t bsf_size)
{
	uint64_t *iv = (void *)bsf->xts_initial_tweak;
	/* [한국어] 16B XTS initial tweak 영역을 8B 두 워드로 보는 별칭. */

	memset(bsf, 0, sizeof(*bsf));
	/* [한국어] BSF 64B 전체 0 클리어(stale 제거). */
	switch (attr->tweak_mode) {
	/* [한국어] AES-XTS tweak로 쓸 LBA를 어느 8B에 어느 endianness로 둘지 결정. */
	case SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_SIMPLE_LBA_LE:
		/* [한국어] little-endian — LBA를 하위 8B(iv[0])에 LE로. */
		iv[0] = htole64(attr->xts_iv);
		iv[1] = 0;
		break;
	case SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_SIMPLE_LBA_BE:
		/* [한국어] big-endian — LBA를 상위 8B(iv[1])에 BE로. */
		iv[0] = 0;
		iv[1] = htobe64(attr->xts_iv);
		break;
	default:
		/* [한국어] 지원하지 않는 tweak 모드 — 개발 빌드에서 abort. */
		assert(false && "unsupported tweak mode");
		break;
	}

	bsf->size_type = (bsf_size << 6) | MLX5_CRYPTO_BSF_P_TYPE_CRYPTO;
	/* [한국어] 상위 비트=BSF 크기(64B=2), 하위 비트=P_TYPE crypto(1). */
	bsf->enc_order = attr->enc_order;
	/* [한국어] 암호화/복호화 순서(메모리→와이어 또는 반대) 지정. */
	bsf->raw_data_size = htobe32(raw_data_size);
	/* [한국어] 평문 데이터 길이(BE). */
	bsf->crypto_block_size_pointer = attr->bs_selector;
	/* [한국어] 블록 크기 셀렉터(512/4096 등 사전 등록 인덱스). */
	bsf->dek_pointer = htobe32(attr->dek_obj_id);
	/* [한국어] DEK(Data Encryption Key) NIC 내부 인덱스 — mlx5_crypto.c가 등록한 값(BE). */
	*((uint64_t *)bsf->keytag) = attr->keytag;
	/* [한국어] keytag — DEK 무결성 검증용 태그(NIC가 키 매칭 확인). */
}

/*
 * [한국어]
 * mlx5_get_crc32c_tfs - CRC32C TFS(Transaction Format Selector) 코드 + seed 비트 OR
 *
 * @seed: 0 또는 0xffffffff. NIC가 지원하는 두 가지 시작값만 허용.
 * @return: 8비트 TFS 코드.
 */
static inline uint8_t
mlx5_get_crc32c_tfs(uint32_t seed)
{
	assert(seed == 0 || seed == 0xffffffff);
	return MLX5_SIG_BSF_TFS_CRC32C | !seed;
	/* [한국어] seed==0xffffffff면 !seed=0이라 추가 비트 없음, seed==0이면 1 비트가 켜짐. */
}

/*
 * [한국어]
 * mlx5_set_umr_sig_bsf_seg - signature BSF 세그먼트 빌드 (T10/CRC32C)
 *
 * @bsf: 작성 대상 BSF.
 * @attr: domain(MEM/WIRE), seed, psv_index, raw_data_size, init, check_gen.
 *
 * signature 도메인 두 가지: MEM(host-side checksum) / WIRE(network-side). domain에 따라 t_init_gen 비트
 * 위치가 다름. init=true면 새 stream 시작(seed 리셋), check_gen=true면 검증 모드 활성.
 */
static inline void
mlx5_set_umr_sig_bsf_seg(struct mlx5_sig_bsf_seg *bsf,
			 struct spdk_mlx5_umr_sig_attr *attr)
{
	uint8_t bsf_size = MLX5_SIG_BSF_SIZE_32B;
	/* [한국어] signature BSF 크기 인코딩 = 32B. */
	uint32_t tfs_psv;
	/* [한국어] TFS(transaction format) 8b + psv_index 24b 합성값. */
	uint32_t init_gen;
	/* [한국어] init/check_gen 비트 묶음(도메인별 위치 다름). */

	memset(bsf, 0, sizeof(*bsf));
	/* [한국어] BSF 전체 0 클리어. */
	bsf->basic.bsf_size_sbs = (bsf_size << 6);
	/* [한국어] 크기 필드를 상위 비트에 배치. */
	bsf->basic.raw_data_size = htobe32(attr->raw_data_size);
	/* [한국어] 검사 대상 데이터 길이(BE). */
	bsf->basic.check_byte_mask = 0xff;
	/* [한국어] 모든 검사 바이트 활성(전 바이트 signature 적용). */

	tfs_psv = mlx5_get_crc32c_tfs(attr->seed);
	/* [한국어] seed에 맞는 CRC32C TFS 코드 획득. */
	tfs_psv = tfs_psv << MLX5_SIG_BSF_TFS_SHIFT;
	/* [한국어] TFS(8b)를 상위 24비트로 shift. */
	tfs_psv |= attr->psv_index & 0xffffff;
	/* [한국어] 하위 24비트에 PSV 인덱스 OR. */

	if (attr->domain == SPDK_MLX5_UMR_SIG_DOMAIN_WIRE) {
		/* [한국어] WIRE(network-side) 도메인 — w_ 필드들에 작성. */
		bsf->ext.w_tfs_psv = htobe32(tfs_psv);
		/* [한국어] wire TFS+PSV(BE). */
		init_gen = attr->init ? MLX5_SIG_BSF_EXT_W_T_INIT : 0;
		/* [한국어] init이면 wire init(seed 리셋) 비트. */
		if (attr->check_gen) {
			/* [한국어] 검증 모드면 wire check/gen 비트 추가. */
			init_gen |= MLX5_SIG_BSF_EXT_W_T_CHECK_GEN;
		}
		bsf->ext.t_init_gen_pro_size = htobe32(init_gen);
		/* [한국어] 합성된 init/gen 비트 기록(BE). */
	} else {
		/* [한국어] MEM(host-side) 도메인 — m_ 필드들에 작성. */
		bsf->ext.m_tfs_psv = htobe32(tfs_psv);
		/* [한국어] memory TFS+PSV(BE). */
		init_gen = attr->init ? MLX5_SIG_BSF_EXT_M_T_INIT : 0;
		/* [한국어] memory init 비트. */
		if (attr->check_gen) {
			/* [한국어] memory check/gen 비트. */
			init_gen |= MLX5_SIG_BSF_EXT_M_T_CHECK_GEN;
		}
		bsf->ext.t_init_gen_pro_size = htobe32(init_gen);
		/* [한국어] 합성 기록(BE). */
	}
}

/*
 * [한국어]
 * mlx5_umr_configure_with_wrap_around_crypto - SQ 끝 wrap-around가 발생하는 경우의 crypto UMR WQE 빌드
 *
 * @qp: 대상 QP.
 * @umr_attr: SGE + mkey + umr_len.
 * @crypto_attr: tweak_mode + DEK + IV.
 * @wr_id: 사용자 wr_id — CQE에서 다시 돌려받음.
 * @flags: SPDK_MLX5_WQE_CTRL_* (fence/signal/se 등).
 * @wqe_size: 전체 WQE 크기(byte). umr_wqe_n_bb로 BB 단위 환산.
 * @umr_wqe_n_bb: WQE BB(64B) 개수.
 * @mtt_size: KLM entry 수(4의 배수로 정렬).
 *
 * SQ ring 끝에서 wrap-around가 일어나는 경우 — get_next_wqebb로 ring 경계를 안전하게 넘기며 작성.
 */
static inline void
mlx5_umr_configure_with_wrap_around_crypto(struct spdk_mlx5_qp *qp,
		struct spdk_mlx5_umr_attr *umr_attr, struct spdk_mlx5_umr_crypto_attr *crypto_attr, uint64_t wr_id,
		uint32_t flags, uint32_t wqe_size, uint32_t umr_wqe_n_bb, uint32_t mtt_size)
{
	struct mlx5_hw_qp *hw = &qp->hw;
	/* [한국어] SQ ring 상태(sq_pi/sq_wqe_cnt/qp_num)를 가진 하드웨어 QP. */
	struct mlx5_wqe_ctrl_seg *ctrl;
	/* [한국어] WQE 시작(첫 BB) 포인터 — submit 시 기준. */
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	/* [한국어] general ctrl segment(opcode/크기/mkey). */
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	/* [한국어] UMR ctrl segment(mkey_mask/klm_octowords/bsf_size). */
	struct mlx5_wqe_mkey_context_seg *mkey;
	/* [한국어] mkey context segment(len 등). */
	struct mlx5_wqe_umr_klm_seg *klm;
	/* [한국어] KLM 배열 시작. */
	struct mlx5_crypto_bsf_seg *bsf;
	/* [한국어] crypto BSF 세그먼트(KLM 뒤). */
	uint8_t fm_ce_se;
	/* [한국어] fence/completion-event/solicited 비트 묶음(ctrl_seg에 들어감). */
	uint32_t pi, to_end;
	/* [한국어] pi=SQ producer index(wqe_cnt 마스크), to_end=ring 끝까지 남은 바이트. */

	fm_ce_se = mlx5_qp_fm_ce_se_update(qp, (uint8_t)flags);
	/* [한국어] flags를 fence/CE/SE 비트로 변환(QP signaling 정책 반영). */

	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	/* [한국어] SQ에서 다음 WQE BB 슬롯 할당(첫 BB). */
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	/* [한국어] producer index를 ring 크기로 마스킹(2의 거듭제곱이라 & 사용). */
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	/* [한국어] 현재 pi부터 ring 끝까지 남은 바이트 — wrap 처리 기준. */

	/*
	 * sizeof(gen_ctrl) + sizeof(umr_ctrl) == MLX5_SEND_WQE_BB,
	 * so do not need to worry about wqe buffer wrap around.
	 *
	 * build genenal ctrl segment
	 */
	gen_ctrl = ctrl;
	/* [한국어] 첫 BB 안에 gen_ctrl(16B)+umr_ctrl(48B)이 들어가므로 wrap 걱정 없음. */
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_UMR, 0,
			  hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0,
			  htobe32(umr_attr->mkey));
	/* [한국어] ctrl_seg 작성 — opcode=UMR, ds=wqe_size/16, 갱신 대상 mkey 지정. */

	/* build umr ctrl segment */
	umr_ctrl = (struct mlx5_wqe_umr_ctrl_seg *)(gen_ctrl + 1);
	/* [한국어] gen_ctrl 바로 뒤가 umr_ctrl. */
	memset(umr_ctrl, 0, sizeof(*umr_ctrl));
	/* [한국어] umr_ctrl 클리어. */
	mlx5_set_umr_ctrl_seg_mtt(umr_ctrl, mtt_size);
	/* [한국어] KLM entry 수 + INLINE + free/len 마스크 설정. */
	mlx5_set_umr_ctrl_seg_bsf_size(umr_ctrl, sizeof(struct mlx5_crypto_bsf_seg));
	/* [한국어] crypto BSF 크기를 octoword 단위로 인코딩. */

	/* build mkey context segment */
	mkey = mlx5_qp_get_next_wqebb(hw, &to_end, ctrl);
	/* [한국어] 다음 BB로 이동(wrap 안전) — mkey context 작성 위치. */
	mlx5_set_umr_mkey_seg(mkey, umr_attr);
	/* [한국어] mkey len 채움. */

	klm = mlx5_qp_get_next_wqebb(hw, &to_end, mkey);
	/* [한국어] 또 다음 BB로 — KLM 배열 시작 위치. */
	bsf = mlx5_build_inline_mtt(hw, &to_end, klm, umr_attr);
	/* [한국어] KLM entry들을 wrap-안전하게 채우고 BSF 작성 시작점 반환. */

	mlx5_set_umr_crypto_bsf_seg(bsf, crypto_attr, umr_attr->umr_len, MLX5_CRYPTO_BSF_SIZE_64B);
	/* [한국어] crypto BSF 채움(tweak/DEK/raw_data_size). */

	mlx5_qp_wqe_submit(qp, ctrl, umr_wqe_n_bb, pi);
	/* [한국어] 완성 WQE를 SQ에 제출 — sq_pi 전진 + 도어벨 ring(BlueFlame/UAR MMIO write). */

	mlx5_qp_set_comp(qp, pi, wr_id, fm_ce_se, umr_wqe_n_bb);
	/* [한국어] 완료 추적용 wr_id 등록 — CQE 수신 시 이 wr_id를 caller에 반환. */
	assert(qp->tx_available >= umr_wqe_n_bb);
	/* [한국어] 가용 BB가 충분한지 검증(상위에서 이미 체크됨). */
	qp->tx_available -= umr_wqe_n_bb;
	/* [한국어] 사용한 BB만큼 가용량 감소. */
}

/*
 * [한국어]
 * mlx5_umr_configure_full_crypto - SQ ring 끝 wrap이 없는 경우의 crypto UMR WQE 빌드 (fast path)
 *
 * 모든 segment(gen_ctrl, umr_ctrl, mkey_ctx, KLM array, BSF)가 contiguous하게 SQ에 들어가므로
 * 단순 포인터 산술로 작성. 일반적인 경우 90% 이상이 이 경로.
 */
static inline void
mlx5_umr_configure_full_crypto(struct spdk_mlx5_qp *dv_qp, struct spdk_mlx5_umr_attr *umr_attr,
			       struct spdk_mlx5_umr_crypto_attr *crypto_attr, uint64_t wr_id,
			       uint32_t flags, uint32_t wqe_size, uint32_t umr_wqe_n_bb,
			       uint32_t mtt_size)
{
	struct mlx5_hw_qp *hw = &dv_qp->hw;
	/* [한국어] SQ ring 상태 보유 hw QP. */
	struct mlx5_wqe_ctrl_seg *ctrl;
	/* [한국어] WQE 시작 포인터. */
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	/* [한국어] general ctrl segment. */
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	/* [한국어] UMR ctrl segment. */
	struct mlx5_wqe_mkey_context_seg *mkey;
	/* [한국어] mkey context segment. */
	struct mlx5_wqe_umr_klm_seg *klm;
	/* [한국어] KLM 작성 커서. */
	struct mlx5_crypto_bsf_seg *bsf;
	/* [한국어] crypto BSF 세그먼트. */
	uint8_t fm_ce_se;
	/* [한국어] fence/CE/SE 비트. */
	uint32_t pi;
	/* [한국어] SQ producer index(마스킹됨). */
	uint32_t i;
	/* [한국어] KLM/PAD 루프 인덱스. */

	fm_ce_se = mlx5_qp_fm_ce_se_update(dv_qp, (uint8_t)flags);
	/* [한국어] signaling 비트 산출. */

	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	/* [한국어] WQE 시작 BB 할당. */
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	/* [한국어] producer index 마스킹. */
	gen_ctrl = ctrl;
	/* [한국어] gen_ctrl = WQE 시작. */
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_UMR, 0,
			  hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0,
			  htobe32(umr_attr->mkey));
	/* [한국어] ctrl_seg(opcode=UMR, ds, mkey) 작성. */

	/* build umr ctrl segment */
	umr_ctrl = (struct mlx5_wqe_umr_ctrl_seg *)(gen_ctrl + 1);
	/* [한국어] 연속 배치라 단순 +1 포인터 산술(wrap 없음). */
	memset(umr_ctrl, 0, sizeof(*umr_ctrl));
	/* [한국어] umr_ctrl 클리어. */
	mlx5_set_umr_ctrl_seg_mtt(umr_ctrl, mtt_size);
	/* [한국어] KLM 수 + INLINE + free/len 마스크. */
	mlx5_set_umr_ctrl_seg_bsf_size(umr_ctrl, sizeof(struct mlx5_crypto_bsf_seg));
	/* [한국어] crypto BSF 크기 인코딩. */

	/* build mkey context segment */
	mkey = (struct mlx5_wqe_mkey_context_seg *)(umr_ctrl + 1);
	/* [한국어] umr_ctrl 다음이 mkey context. */
	mlx5_set_umr_mkey_seg(mkey, umr_attr);
	/* [한국어] mkey len 채움. */

	klm = (struct mlx5_wqe_umr_klm_seg *)(mkey + 1);
	/* [한국어] mkey 다음부터 KLM 배열. */
	for (i = 0; i < umr_attr->sge_count; i++) {
		/* [한국어] 각 SGE를 KLM entry로 변환(연속 배치라 wrap 없음). */
		mlx5_set_umr_inline_klm_seg(klm, &umr_attr->sge[i]);
		/* sizeof(*klm) * 4 == MLX5_SEND_WQE_BB */
		klm = klm + 1;
		/* [한국어] 다음 KLM으로. */
	}
	/* fill PAD if existing */
	/* PAD entries is to make whole mtt aligned to 64B(MLX5_SEND_WQE_BB),
	 * So it will not happen wrap around during fill PAD entries. */
	for (; i < mtt_size; i++) {
		/* [한국어] 64B 정렬을 위한 PAD entry를 0으로 채움. */
		memset(klm, 0, sizeof(*klm));
		klm = klm + 1;
	}

	bsf = (struct mlx5_crypto_bsf_seg *)klm;
	/* [한국어] PAD 다음이 crypto BSF. */
	mlx5_set_umr_crypto_bsf_seg(bsf, crypto_attr, umr_attr->umr_len, MLX5_CRYPTO_BSF_SIZE_64B);
	/* [한국어] crypto BSF 채움. */

	mlx5_qp_wqe_submit(dv_qp, ctrl, umr_wqe_n_bb, pi);
	/* [한국어] WQE 제출 + 도어벨 ring. */

	mlx5_qp_set_comp(dv_qp, pi, wr_id, fm_ce_se, umr_wqe_n_bb);
	/* [한국어] 완료 추적 등록. */
	assert(dv_qp->tx_available >= umr_wqe_n_bb);
	/* [한국어] 가용량 검증. */
	dv_qp->tx_available -= umr_wqe_n_bb;
	/* [한국어] 가용량 감소. */
}

/*
 * [한국어]
 * spdk_mlx5_umr_configure_crypto - 외부 API: crypto UMR WQE 생성·SQ에 게시
 *
 * @qp: SPDK mlx5 QP.
 * @umr_attr: 갱신할 mkey + SGE 배열 + umr_len.
 * @crypto_attr: tweak_mode/IV/DEK/keytag.
 * @wr_id: CQE에서 식별할 사용자 ID.
 * @flags: 동기화·시그널 비트.
 * @return: 0 / -EINVAL / -ENOMEM / -E2BIG.
 *
 * 동작:
 *  1) WQE 레이아웃 계산: gen_ctrl(16) + umr_ctrl(48) + mkey_ctx(64) + KLM array(16*aligned) + crypto BSF(64).
 *  2) BB 개수 계산. tx_available 부족하면 -ENOMEM.
 *  3) SQ ring 끝까지 남은 공간(to_end) < wqe_size면 wrap-around 경로, 아니면 full 경로.
 *
 * 호출 체인:
 *   accel_mlx5 → 이 함수 → mlx5_umr_configure_full_crypto / *_with_wrap_around_crypto
 *     → mlx5_qp_wqe_submit → BlueFlame/UAR doorbell MMIO write → NIC가 SQ fetch
 */
int
spdk_mlx5_umr_configure_crypto(struct spdk_mlx5_qp *qp, struct spdk_mlx5_umr_attr *umr_attr,
			       struct spdk_mlx5_umr_crypto_attr *crypto_attr, uint64_t wr_id, uint32_t flags)
{
	struct mlx5_hw_qp *hw = &qp->hw;
	/* [한국어] SQ ring 상태. */
	uint32_t pi, to_end, umr_wqe_n_bb;
	/* [한국어] pi=producer index, to_end=ring 끝까지 남은 바이트, umr_wqe_n_bb=WQE BB 개수. */
	uint32_t wqe_size, mtt_size;
	/* [한국어] wqe_size=전체 WQE 바이트, mtt_size=4 정렬 KLM entry 수. */
	uint32_t inline_klm_size;
	/* [한국어] KLM 배열 영역 바이트 크기. */

	if (!spdk_unlikely(umr_attr->sge_count)) {
		/* [한국어] SGE가 0개면 등록할 게 없음 — -EINVAL. */
		return -EINVAL;
	}

	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	/* [한국어] producer index 마스킹. */
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	/* [한국어] ring 끝까지 남은 바이트(wrap 판단용). */

	/*
	 * UMR WQE LAYOUT:
	 * -----------------------------------------------------------------------
	 * | gen_ctrl | umr_ctrl | mkey_ctx | inline klm mtt | inline crypto bsf |
	 * -----------------------------------------------------------------------
	 *   16bytes    48bytes    64bytes   sge_count*16 bytes      64 bytes
	 *
	 * Note: size of inline klm mtt should be aligned to 64 bytes.
	 */
	wqe_size = sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_umr_ctrl_seg) +
		   sizeof(struct mlx5_wqe_mkey_context_seg);
	/* [한국어] 고정 헤더 합(16+48+64=128B). */
	mtt_size = SPDK_ALIGN_CEIL(umr_attr->sge_count, 4);
	/* [한국어] KLM entry 수를 4의 배수로 올림(octword 정렬). */
	inline_klm_size = mtt_size * sizeof(struct mlx5_wqe_umr_klm_seg);
	/* [한국어] KLM 영역 바이트 크기(entry당 16B). */
	wqe_size += inline_klm_size;
	/* [한국어] KLM 영역 추가. */
	wqe_size += sizeof(struct mlx5_crypto_bsf_seg);
	/* [한국어] crypto BSF(64B) 추가. */

	umr_wqe_n_bb = SPDK_CEIL_DIV(wqe_size, MLX5_SEND_WQE_BB);
	/* [한국어] 64B BB 단위로 올림 — 필요한 BB 개수. */
	if (spdk_unlikely(umr_wqe_n_bb > qp->tx_available)) {
		/* [한국어] SQ 가용 BB 부족 — caller가 나중에 재시도(-ENOMEM). */
		return -ENOMEM;
	}
	if (spdk_unlikely(umr_attr->sge_count > qp->max_send_sge)) {
		/* [한국어] SGE 수가 QP 한도 초과 — -E2BIG. */
		return -E2BIG;
	}

	if (spdk_unlikely(to_end < wqe_size)) {
		/* [한국어] WQE가 ring 끝을 넘어가면 wrap-around 경로(드묾). */
		mlx5_umr_configure_with_wrap_around_crypto(qp, umr_attr, crypto_attr, wr_id, flags, wqe_size,
				umr_wqe_n_bb,
				mtt_size);
	} else {
		/* [한국어] ring 안에 연속으로 들어가면 fast path. */
		mlx5_umr_configure_full_crypto(qp, umr_attr, crypto_attr, wr_id, flags, wqe_size, umr_wqe_n_bb,
					       mtt_size);
	}

	return 0;
}

/*
 * [한국어]
 * mlx5_umr_configure_with_wrap_around_sig - SQ wrap-around가 발생하는 signature UMR WQE 빌드
 *
 * crypto 버전과 동일한 패턴이나 BSF가 mlx5_sig_bsf_seg(32B 변형)이고 mkey_ctx에 sig flag도 함께 설정.
 */
static inline void
mlx5_umr_configure_with_wrap_around_sig(struct spdk_mlx5_qp *dv_qp,
					struct spdk_mlx5_umr_attr *umr_attr,
					struct spdk_mlx5_umr_sig_attr *sig_attr, uint64_t wr_id,
					uint32_t flags, uint32_t wqe_size, uint32_t umr_wqe_n_bb, uint32_t mtt_size)
{
	struct mlx5_hw_qp *hw = &dv_qp->hw;
	/* [한국어] SQ ring 상태. */
	struct mlx5_wqe_ctrl_seg *ctrl;
	/* [한국어] WQE 시작. */
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	/* [한국어] general ctrl segment. */
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	/* [한국어] UMR ctrl segment. */
	struct mlx5_wqe_mkey_context_seg *mkey;
	/* [한국어] mkey context segment. */
	struct mlx5_wqe_umr_klm_seg *klm;
	/* [한국어] KLM 배열. */
	struct mlx5_sig_bsf_seg *bsf;
	/* [한국어] signature BSF(32B 변형). */
	uint8_t fm_ce_se;
	/* [한국어] signaling 비트. */
	uint32_t pi, to_end;
	/* [한국어] producer index와 ring 잔여 바이트. */

	fm_ce_se = mlx5_qp_fm_ce_se_update(dv_qp, (uint8_t)flags);
	/* [한국어] signaling 비트 산출. */

	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	/* [한국어] WQE 시작 BB 할당. */
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	/* [한국어] producer index 마스킹. */
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	/* [한국어] ring 끝까지 남은 바이트. */

	/*
	 * sizeof(gen_ctrl) + sizeof(umr_ctrl) == MLX5_SEND_WQE_BB,
	 * so do not need to worry about wqe buffer wrap around.
	 *
	 * build genenal ctrl segment
	 */
	gen_ctrl = ctrl;
	/* [한국어] 첫 BB에 gen+umr ctrl이 들어감. */
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_UMR, 0,
			  hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0,
			  htobe32(umr_attr->mkey));
	/* [한국어] ctrl_seg 작성. */

	/* build umr ctrl segment */
	umr_ctrl = (struct mlx5_wqe_umr_ctrl_seg *)(gen_ctrl + 1);
	/* [한국어] gen_ctrl 다음이 umr_ctrl. */
	memset(umr_ctrl, 0, sizeof(*umr_ctrl));
	/* [한국어] 클리어. */
	mlx5_set_umr_ctrl_seg_mtt_sig(umr_ctrl, mtt_size);
	/* [한국어] sig 변형 — free/len에 더해 SIG_ERR 마스크도 설정. */
	mlx5_set_umr_ctrl_seg_bsf_size(umr_ctrl, sizeof(struct mlx5_sig_bsf_seg));
	/* [한국어] sig BSF 크기 인코딩. */

	/* build mkey context segment */
	mkey = mlx5_qp_get_next_wqebb(hw, &to_end, ctrl);
	/* [한국어] 다음 BB로(wrap 안전) — mkey context. */
	mlx5_set_umr_mkey_seg(mkey, umr_attr);
	/* [한국어] mkey len 채움. */
	mlx5_set_umr_mkey_seg_sig(mkey, sig_attr);
	/* [한국어] sigerr toggle 비트 추가(sig 전용). */

	klm = mlx5_qp_get_next_wqebb(hw, &to_end, mkey);
	/* [한국어] 다음 BB로 — KLM 시작. */
	bsf = mlx5_build_inline_mtt(hw, &to_end, klm, umr_attr);
	/* [한국어] KLM 채우고 BSF 시작점 반환. */

	mlx5_set_umr_sig_bsf_seg(bsf, sig_attr);
	/* [한국어] signature BSF 채움(TFS/PSV/init/check_gen). */

	mlx5_qp_wqe_submit(dv_qp, ctrl, umr_wqe_n_bb, pi);
	/* [한국어] WQE 제출 + 도어벨. */

	mlx5_qp_set_comp(dv_qp, pi, wr_id, fm_ce_se, umr_wqe_n_bb);
	/* [한국어] 완료 추적 등록. */
	assert(dv_qp->tx_available >= umr_wqe_n_bb);
	/* [한국어] 가용량 검증. */
	dv_qp->tx_available -= umr_wqe_n_bb;
	/* [한국어] 가용량 감소. */
}

/*
 * [한국어]
 * mlx5_umr_configure_full_sig - SQ wrap이 없는 경우의 signature UMR WQE 빌드 (fast path)
 */
static inline void
mlx5_umr_configure_full_sig(struct spdk_mlx5_qp *dv_qp, struct spdk_mlx5_umr_attr *umr_attr,
			    struct spdk_mlx5_umr_sig_attr *sig_attr, uint64_t wr_id,
			    uint32_t flags, uint32_t wqe_size, uint32_t umr_wqe_n_bb,
			    uint32_t mtt_size)
{
	struct mlx5_hw_qp *hw = &dv_qp->hw;
	/* [한국어] SQ ring 상태. */
	struct mlx5_wqe_ctrl_seg *ctrl;
	/* [한국어] WQE 시작. */
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	/* [한국어] general ctrl. */
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	/* [한국어] UMR ctrl. */
	struct mlx5_wqe_mkey_context_seg *mkey;
	/* [한국어] mkey context. */
	struct mlx5_wqe_umr_klm_seg *klm;
	/* [한국어] KLM 커서. */
	struct mlx5_sig_bsf_seg *bsf;
	/* [한국어] signature BSF. */
	uint8_t fm_ce_se;
	/* [한국어] signaling 비트. */
	uint32_t pi;
	/* [한국어] producer index. */
	uint32_t i;
	/* [한국어] KLM/PAD 인덱스. */

	fm_ce_se = mlx5_qp_fm_ce_se_update(dv_qp, (uint8_t)flags);
	/* [한국어] signaling 비트 산출. */

	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	/* [한국어] WQE 시작 BB 할당. */
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	/* [한국어] producer index 마스킹. */
	gen_ctrl = ctrl;
	/* [한국어] gen_ctrl = 시작. */
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_UMR, 0,
			  hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0,
			  htobe32(umr_attr->mkey));
	/* [한국어] ctrl_seg 작성. */

	/* build umr ctrl segment */
	umr_ctrl = (struct mlx5_wqe_umr_ctrl_seg *)(gen_ctrl + 1);
	/* [한국어] 연속 배치(+1). */
	memset(umr_ctrl, 0, sizeof(*umr_ctrl));
	/* [한국어] 클리어. */
	mlx5_set_umr_ctrl_seg_mtt_sig(umr_ctrl, mtt_size);
	/* [한국어] sig 변형(SIG_ERR 마스크 포함). */
	mlx5_set_umr_ctrl_seg_bsf_size(umr_ctrl, sizeof(struct mlx5_sig_bsf_seg));
	/* [한국어] sig BSF 크기. */

	/* build mkey context segment */
	mkey = (struct mlx5_wqe_mkey_context_seg *)(umr_ctrl + 1);
	/* [한국어] umr_ctrl 다음. */
	memset(mkey, 0, sizeof(*mkey));
	/* [한국어] mkey context 클리어(set_umr_mkey_seg 대신 직접 — sig는 len+sig 두 단계). */
	mlx5_set_umr_mkey_seg_mtt(mkey, umr_attr);
	/* [한국어] len 채움. */
	mlx5_set_umr_mkey_seg_sig(mkey, sig_attr);
	/* [한국어] sigerr toggle 비트. */

	klm = (struct mlx5_wqe_umr_klm_seg *)(mkey + 1);
	/* [한국어] mkey 다음부터 KLM. */
	for (i = 0; i < umr_attr->sge_count; i++) {
		/* [한국어] 각 SGE를 KLM entry로(연속 배치). */
		mlx5_set_umr_inline_klm_seg(klm, &umr_attr->sge[i]);
		/* sizeof(*klm) * 4 == MLX5_SEND_WQE_BB */
		klm = klm + 1;
	}
	/* fill PAD if existing */
	/* PAD entries is to make whole mtt aligned to 64B(MLX5_SEND_WQE_BB),
	 * So it will not happen warp around during fill PAD entries. */
	for (; i < mtt_size; i++) {
		/* [한국어] 64B 정렬용 PAD 0 채움. */
		memset(klm, 0, sizeof(*klm));
		klm = klm + 1;
	}

	bsf = (struct mlx5_sig_bsf_seg *)klm;
	/* [한국어] PAD 다음이 sig BSF. */
	mlx5_set_umr_sig_bsf_seg(bsf, sig_attr);
	/* [한국어] signature BSF 채움. */

	mlx5_qp_wqe_submit(dv_qp, ctrl, umr_wqe_n_bb, pi);
	/* [한국어] WQE 제출 + 도어벨. */

	mlx5_qp_set_comp(dv_qp, pi, wr_id, fm_ce_se, umr_wqe_n_bb);
	/* [한국어] 완료 추적 등록. */
	assert(dv_qp->tx_available >= umr_wqe_n_bb);
	/* [한국어] 가용량 검증. */
	dv_qp->tx_available -= umr_wqe_n_bb;
	/* [한국어] 가용량 감소. */
}

/*
 * [한국어]
 * spdk_mlx5_umr_configure_sig - 외부 API: signature UMR WQE 빌드·SQ 게시
 *
 * crypto 버전과 거의 동일하나 BSF는 signature(32B) 변형. T10-DIF/CRC32C 도메인 변환·검증에 사용.
 */
int
spdk_mlx5_umr_configure_sig(struct spdk_mlx5_qp *qp, struct spdk_mlx5_umr_attr *umr_attr,
			    struct spdk_mlx5_umr_sig_attr *sig_attr, uint64_t wr_id, uint32_t flags)
{
	struct mlx5_hw_qp *hw = &qp->hw;
	/* [한국어] SQ ring 상태. */
	uint32_t pi, to_end, umr_wqe_n_bb;
	/* [한국어] producer index / ring 잔여 / WQE BB 개수. */
	uint32_t wqe_size, mtt_size;
	/* [한국어] 전체 WQE 크기 / 4 정렬 KLM 수. */
	uint32_t inline_klm_size;
	/* [한국어] KLM 영역 바이트 크기. */

	if (!spdk_unlikely(umr_attr->sge_count)) {
		/* [한국어] SGE 0개 — -EINVAL. */
		return -EINVAL;
	}

	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	/* [한국어] producer index 마스킹. */
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	/* [한국어] ring 끝까지 남은 바이트. */

	/*
	 * UMR WQE LAYOUT:
	 * -----------------------------------------------------------------------
	 * | gen_ctrl | umr_ctrl | mkey_ctx | inline klm mtt | inline sig bsf |
	 * -----------------------------------------------------------------------
	 *   16bytes    48bytes    64bytes   sg_count*16 bytes      64 bytes
	 *
	 * Note: size of inline klm mtt should be aligned to 64 bytes.
	 */
	wqe_size = sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_umr_ctrl_seg) +
		   sizeof(struct mlx5_wqe_mkey_context_seg);
	/* [한국어] 고정 헤더 합(128B). */
	mtt_size = SPDK_ALIGN_CEIL(umr_attr->sge_count, 4);
	/* [한국어] KLM 수 4 정렬. */
	inline_klm_size = mtt_size * sizeof(struct mlx5_wqe_umr_klm_seg);
	/* [한국어] KLM 영역 바이트. */
	wqe_size += inline_klm_size;
	/* [한국어] KLM 추가. */
	wqe_size += sizeof(struct mlx5_sig_bsf_seg);
	/* [한국어] signature BSF(64B 슬롯) 추가. */

	umr_wqe_n_bb = SPDK_CEIL_DIV(wqe_size, MLX5_SEND_WQE_BB);
	/* [한국어] BB 개수. */
	if (spdk_unlikely(umr_wqe_n_bb > qp->tx_available)) {
		/* [한국어] 가용 BB 부족. */
		return -ENOMEM;
	}
	if (spdk_unlikely(umr_attr->sge_count > qp->max_send_sge)) {
		/* [한국어] SGE 한도 초과. */
		return -E2BIG;
	}

	if (spdk_unlikely(to_end < wqe_size)) {
		/* [한국어] ring 끝 넘어가면 wrap 경로. */
		mlx5_umr_configure_with_wrap_around_sig(qp, umr_attr, sig_attr, wr_id, flags, wqe_size,
							umr_wqe_n_bb, mtt_size);
	} else {
		/* [한국어] 연속 fast path. */
		mlx5_umr_configure_full_sig(qp, umr_attr, sig_attr, wr_id, flags, wqe_size, umr_wqe_n_bb,
					    mtt_size);
	}

	return 0;
}

/*
 * [한국어]
 * mlx5_umr_configure_full - plain UMR(BSF 없음, MTT만) WQE 빌드 — SQ wrap 없는 경우
 *
 * crypto/sig 변형의 base — 여러 SGE를 단일 lkey로 묶기만 함. 가장 단순한 UMR 형태.
 */
static inline void
mlx5_umr_configure_full(struct spdk_mlx5_qp *dv_qp, struct spdk_mlx5_umr_attr *umr_attr,
			uint64_t wr_id, uint32_t flags, uint32_t wqe_size, uint32_t umr_wqe_n_bb,
			uint32_t mtt_size)
{
	struct mlx5_hw_qp *hw = &dv_qp->hw;
	/* [한국어] SQ ring 상태. */
	struct mlx5_wqe_ctrl_seg *ctrl;
	/* [한국어] WQE 시작. */
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	/* [한국어] general ctrl. */
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	/* [한국어] UMR ctrl. */
	struct mlx5_wqe_mkey_context_seg *mkey;
	/* [한국어] mkey context. */
	struct mlx5_wqe_umr_klm_seg *klm;
	/* [한국어] KLM 커서. */
	uint8_t fm_ce_se;
	/* [한국어] signaling 비트. */
	uint32_t pi;
	/* [한국어] producer index. */
	uint32_t i;
	/* [한국어] KLM/PAD 인덱스. */

	fm_ce_se = mlx5_qp_fm_ce_se_update(dv_qp, (uint8_t)flags);
	/* [한국어] signaling 비트 산출. */

	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	/* [한국어] WQE 시작 BB 할당. */
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	/* [한국어] producer index 마스킹. */

	gen_ctrl = ctrl;
	/* [한국어] gen_ctrl = 시작. */
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_UMR, 0,
			  hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0,
			  htobe32(umr_attr->mkey));
	/* [한국어] ctrl_seg 작성. */

	/* build umr ctrl segment */
	umr_ctrl = (struct mlx5_wqe_umr_ctrl_seg *)(gen_ctrl + 1);
	/* [한국어] gen_ctrl 다음. */
	memset(umr_ctrl, 0, sizeof(*umr_ctrl));
	/* [한국어] 클리어. */
	mlx5_set_umr_ctrl_seg_mtt(umr_ctrl, mtt_size);
	/* [한국어] plain — BSF 없이 KLM 수 + free/len 마스크만. */

	/* build mkey context segment */
	mkey = (struct mlx5_wqe_mkey_context_seg *)(umr_ctrl + 1);
	/* [한국어] umr_ctrl 다음. */
	mlx5_set_umr_mkey_seg(mkey, umr_attr);
	/* [한국어] mkey len 채움. */

	klm = (struct mlx5_wqe_umr_klm_seg *)(mkey + 1);
	/* [한국어] mkey 다음부터 KLM. */
	for (i = 0; i < umr_attr->sge_count; i++) {
		/* [한국어] 각 SGE → KLM entry. */
		mlx5_set_umr_inline_klm_seg(klm, &umr_attr->sge[i]);
		/* sizeof(*klm) * 4 == MLX5_SEND_WQE_BB */
		klm = klm + 1;
	}
	/* fill PAD if existing */
	/* PAD entries is to make whole mtt aligned to 64B(MLX5_SEND_WQE_BB),
	 * So it will not happen warp around during fill PAD entries. */
	for (; i < mtt_size; i++) {
		/* [한국어] 64B 정렬용 PAD 0 채움. */
		memset(klm, 0, sizeof(*klm));
		klm = klm + 1;
	}

	mlx5_qp_wqe_submit(dv_qp, ctrl, umr_wqe_n_bb, pi);
	/* [한국어] WQE 제출 + 도어벨. */

	mlx5_qp_set_comp(dv_qp, pi, wr_id, fm_ce_se, umr_wqe_n_bb);
	/* [한국어] 완료 추적 등록. */
	assert(dv_qp->tx_available >= umr_wqe_n_bb);
	/* [한국어] 가용량 검증. */
	dv_qp->tx_available -= umr_wqe_n_bb;
	/* [한국어] 가용량 감소. */
}

/*
 * [한국어]
 * mlx5_umr_configure_with_wrap_around - plain UMR WQE 빌드 — SQ wrap-around 발생 경우
 */
static inline void
mlx5_umr_configure_with_wrap_around(struct spdk_mlx5_qp *dv_qp, struct spdk_mlx5_umr_attr *umr_attr,
				    uint64_t wr_id, uint32_t flags, uint32_t wqe_size, uint32_t umr_wqe_n_bb,
				    uint32_t mtt_size)
{
	struct mlx5_hw_qp *hw = &dv_qp->hw;
	/* [한국어] SQ ring 상태. */
	struct mlx5_wqe_ctrl_seg *ctrl;
	/* [한국어] WQE 시작. */
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	/* [한국어] general ctrl. */
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	/* [한국어] UMR ctrl. */
	struct mlx5_wqe_mkey_context_seg *mkey;
	/* [한국어] mkey context. */
	struct mlx5_wqe_umr_klm_seg *klm;
	/* [한국어] KLM 시작. */
	uint8_t fm_ce_se;
	/* [한국어] signaling 비트. */
	uint32_t pi, to_end;
	/* [한국어] producer index와 ring 잔여 바이트. */

	fm_ce_se = mlx5_qp_fm_ce_se_update(dv_qp, (uint8_t)flags);
	/* [한국어] signaling 비트 산출. */

	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	/* [한국어] WQE 시작 BB 할당. */
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	/* [한국어] producer index 마스킹. */
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	/* [한국어] ring 끝까지 남은 바이트. */
	/*
	 * sizeof(gen_ctrl) + sizeof(umr_ctrl) == MLX5_SEND_WQE_BB,
	 * so do not need to worry about wqe buffer wrap around.
	 *
	 * build genenal ctrl segment
	 */
	gen_ctrl = ctrl;
	/* [한국어] 첫 BB에 gen+umr ctrl이 들어감(wrap 무관). */
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_UMR, 0,
			  hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0,
			  htobe32(umr_attr->mkey));
	/* [한국어] ctrl_seg 작성. */

	/* build umr ctrl segment */
	umr_ctrl = (struct mlx5_wqe_umr_ctrl_seg *)(gen_ctrl + 1);
	/* [한국어] gen_ctrl 다음. */
	memset(umr_ctrl, 0, sizeof(*umr_ctrl));
	/* [한국어] 클리어. */
	mlx5_set_umr_ctrl_seg_mtt(umr_ctrl, mtt_size);
	/* [한국어] plain — KLM 수 + free/len 마스크. */

	/* build mkey context segment */
	mkey = mlx5_qp_get_next_wqebb(hw, &to_end, ctrl);
	/* [한국어] 다음 BB로(wrap 안전) — mkey context. */
	mlx5_set_umr_mkey_seg(mkey, umr_attr);
	/* [한국어] mkey len 채움. */

	klm = mlx5_qp_get_next_wqebb(hw, &to_end, mkey);
	/* [한국어] 다음 BB로 — KLM 시작. */
	mlx5_build_inline_mtt(hw, &to_end, klm, umr_attr);
	/* [한국어] KLM 채움(plain은 BSF 없으므로 반환값 미사용). */

	mlx5_qp_wqe_submit(dv_qp, ctrl, umr_wqe_n_bb, pi);
	/* [한국어] WQE 제출 + 도어벨. */

	mlx5_qp_set_comp(dv_qp, pi, wr_id, fm_ce_se, umr_wqe_n_bb);
	/* [한국어] 완료 추적 등록. */
	assert(dv_qp->tx_available >= umr_wqe_n_bb);
	/* [한국어] 가용량 검증. */
	dv_qp->tx_available -= umr_wqe_n_bb;
	/* [한국어] 가용량 감소. */
}

/*
 * [한국어]
 * spdk_mlx5_umr_configure - 외부 API: plain UMR WQE 빌드·SQ 게시 (BSF 없음)
 *
 * 여러 SGE를 단일 lkey로 묶는 가장 기본적인 UMR — 후속 RDMA WR이 그 lkey 하나로 모든 SGE에 access.
 */
int
spdk_mlx5_umr_configure(struct spdk_mlx5_qp *qp, struct spdk_mlx5_umr_attr *umr_attr,
			uint64_t wr_id, uint32_t flags)
{
	struct mlx5_hw_qp *hw = &qp->hw;
	/* [한국어] SQ ring 상태. */
	uint32_t pi, to_end, umr_wqe_n_bb;
	/* [한국어] producer index / ring 잔여 / WQE BB 개수. */
	uint32_t wqe_size, mtt_size;
	/* [한국어] 전체 WQE 크기 / 4 정렬 KLM 수. */
	uint32_t inline_klm_size;
	/* [한국어] KLM 영역 바이트. */

	if (!spdk_unlikely(umr_attr->sge_count)) {
		/* [한국어] SGE 0개 — -EINVAL. */
		return -EINVAL;
	}

	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	/* [한국어] producer index 마스킹. */
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	/* [한국어] ring 끝까지 남은 바이트. */

	/*
	 * UMR WQE LAYOUT:
	 * ---------------------------------------------------
	 * | gen_ctrl | umr_ctrl | mkey_ctx | inline klm mtt |
	 * ---------------------------------------------------
	 *   16bytes    48bytes    64bytes   sg_count*16 bytes
	 *
	 * Note: size of inline klm mtt should be aligned to 64 bytes.
	 */
	wqe_size = sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_umr_ctrl_seg) + sizeof(
			   struct mlx5_wqe_mkey_context_seg);
	/* [한국어] 고정 헤더 합(128B) — plain은 BSF가 없음. */
	mtt_size = SPDK_ALIGN_CEIL(umr_attr->sge_count, 4);
	/* [한국어] KLM 수 4 정렬. */
	inline_klm_size = mtt_size * sizeof(union mlx5_wqe_umr_inline_seg);
	/* [한국어] KLM 영역 바이트(union 크기 사용). */
	wqe_size += inline_klm_size;
	/* [한국어] KLM 추가(BSF 없음). */

	umr_wqe_n_bb = SPDK_CEIL_DIV(wqe_size, MLX5_SEND_WQE_BB);
	/* [한국어] BB 개수. */
	if (spdk_unlikely(umr_wqe_n_bb > qp->tx_available)) {
		/* [한국어] 가용 BB 부족. */
		return -ENOMEM;
	}
	if (spdk_unlikely(umr_attr->sge_count > qp->max_send_sge)) {
		/* [한국어] SGE 한도 초과. */
		return -E2BIG;
	}

	if (spdk_unlikely(to_end < wqe_size)) {
		/* [한국어] ring 끝 넘어가면 wrap 경로. */
		mlx5_umr_configure_with_wrap_around(qp, umr_attr, wr_id, flags, wqe_size, umr_wqe_n_bb,
						    mtt_size);
	} else {
		/* [한국어] 연속 fast path. */
		mlx5_umr_configure_full(qp, umr_attr, wr_id, flags, wqe_size, umr_wqe_n_bb, mtt_size);
	}

	return 0;
}

/*
 * [한국어]
 * mlx5_cmd_create_psv - PRM create_psv 명령 issue
 *
 * @context: 디바이스 컨텍스트.
 * @pdn: PD 번호.
 * @psv_index: 출력 — NIC가 할당한 PSV 인덱스.
 * @return: mlx5dv_devx_obj* 또는 NULL.
 *
 * PSV(Protected Signature Validator)는 signature 검증의 transient state(CRC 누적값)를 NIC에 저장하는
 * 작은 컨트롤 객체. UMR sig BSF가 이 PSV 인덱스를 참조.
 */
static struct mlx5dv_devx_obj *
mlx5_cmd_create_psv(struct ibv_context *context, uint32_t pdn, uint32_t *psv_index)
{
	uint32_t out[DEVX_ST_SZ_DW(create_psv_out)] = {};
	/* [한국어] NIC가 채울 출력 — psv0_index 포함. */
	uint32_t in[DEVX_ST_SZ_DW(create_psv_in)] = {};
	/* [한국어] create_psv 명령 입력. */
	struct mlx5dv_devx_obj *obj;
	/* [한국어] 생성된 PSV devx 객체 핸들. */

	assert(context);
	/* [한국어] 디바이스 컨텍스트 필수. */
	assert(psv_index);
	/* [한국어] 출력 인덱스 포인터 필수. */

	DEVX_SET(create_psv_in, in, opcode, MLX5_CMD_OP_CREATE_PSV);
	/* [한국어] opcode = CREATE_PSV. */
	DEVX_SET(create_psv_in, in, pd, pdn);
	/* [한국어] 소속 PD 번호. */
	DEVX_SET(create_psv_in, in, num_psv, 1);
	/* [한국어] 한 번에 1개 PSV 생성. */

	obj = mlx5dv_devx_obj_create(context, in, sizeof(in), out, sizeof(out));
	/* [한국어] NIC에 issue — HCA가 PSV 객체 생성. */
	if (obj) {
		/* [한국어] 성공 시 할당된 PSV 인덱스 추출. */
		*psv_index = DEVX_GET(create_psv_out, out, psv0_index);
	}

	return obj;
}

/*
 * [한국어]
 * spdk_mlx5_create_psv - 외부 API: PSV 객체 생성 (signature offload용)
 *
 * @pd: 소속 PD.
 * @return: spdk_mlx5_psv* 또는 NULL.
 *
 * 호출 체인: accel_mlx5(signature feature) → 이 함수 → mlx5_cmd_create_psv → NIC create_psv.
 */
struct spdk_mlx5_psv *
spdk_mlx5_create_psv(struct ibv_pd *pd)
{
	uint32_t pdn;
	/* [한국어] PD 번호(create_psv에 필요). */
	struct spdk_mlx5_psv *psv;
	/* [한국어] 반환할 SPDK PSV wrapper. */
	int err;
	/* [한국어] pd_id 조회 결과. */

	assert(pd);
	/* [한국어] PD 필수. */

	err = mlx5_get_pd_id(pd, &pdn);
	/* [한국어] PD 번호 조회. */
	if (err) {
		/* [한국어] 실패 시 NULL. */
		return NULL;
	}

	psv = calloc(1, sizeof(*psv));
	/* [한국어] wrapper 할당. */
	if (!psv) {
		/* [한국어] 메모리 부족. */
		return NULL;
	}

	psv->devx_obj = mlx5_cmd_create_psv(pd->context, pdn, &psv->index);
	/* [한국어] NIC PSV 객체 생성 + index 채움. */
	if (!psv->devx_obj) {
		/* [한국어] 생성 실패 시 wrapper 해제 후 NULL. */
		free(psv);
		return NULL;
	}

	return psv;
}

/*
 * [한국어]
 * spdk_mlx5_destroy_psv - PSV 해제.
 */
int
spdk_mlx5_destroy_psv(struct spdk_mlx5_psv *psv)
{
	int ret;
	/* [한국어] destroy 결과. */

	ret = mlx5dv_devx_obj_destroy(psv->devx_obj);
	/* [한국어] NIC PSV 객체 회수. */
	if (!ret) {
		/* [한국어] 성공 시에만 wrapper 해제(실패 시 누수보다 객체 유지가 안전). */
		free(psv);
	}

	return ret;
}

/*
 * [한국어]
 * spdk_mlx5_qp_set_psv - SET_PSV WQE를 SQ에 게시하여 PSV의 transient signature(CRC seed) 갱신
 *
 * @qp: 대상 QP.
 * @psv_index: 갱신할 PSV 인덱스.
 * @crc_seed: 32b seed — transient_signature 상위 32비트에 인코딩됨(하위는 0).
 * @wr_id: CQE 식별자.
 * @flags: 동기화 비트.
 * @return: 0 또는 -ENOMEM.
 *
 * SET_PSV WQE는 ctrl_seg + set_psv_seg(나머지는 0 패딩)로 단일 WQE BB(64B) 안에 들어가는 작은 WQE.
 * 일반적으로 signature stream의 시작 직전 호출되어 PSV를 초기화.
 */
int
spdk_mlx5_qp_set_psv(struct spdk_mlx5_qp *qp, uint32_t psv_index, uint32_t crc_seed, uint64_t wr_id,
		     uint32_t flags)
{
	struct mlx5_hw_qp *hw = &qp->hw;
	/* [한국어] SQ ring 상태. */
	uint32_t pi, wqe_size, wqe_n_bb;
	/* [한국어] producer index / WQE 바이트 / BB 개수(항상 1). */
	struct mlx5_wqe_ctrl_seg *ctrl;
	/* [한국어] WQE 시작. */
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	/* [한국어] general ctrl. */
	struct mlx5_wqe_set_psv_seg *psv;
	/* [한국어] SET_PSV 세그먼트. */
	uint8_t fm_ce_se;
	/* [한국어] signaling 비트. */
	uint64_t transient_signature = (uint64_t)crc_seed << 32;
	/* [한국어] 32b CRC seed를 64b 상위 절반에 배치 — transient_signature 필드 포맷. */

	wqe_size = sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_set_psv_seg);
	/* [한국어] SET_PSV WQE 크기(ctrl + psv seg). */
	/* The size of SET_PSV WQE is constant and smaller than WQE BB. */
	assert(wqe_size < MLX5_SEND_WQE_BB);
	/* [한국어] 단일 BB(64B) 안에 들어감을 보장(wrap 처리 불필요). */
	wqe_n_bb = 1;
	/* [한국어] 항상 1 BB. */
	if (spdk_unlikely(wqe_n_bb > qp->tx_available)) {
		/* [한국어] SQ 가용 BB 부족. */
		return -ENOMEM;
	}

	fm_ce_se = mlx5_qp_fm_ce_se_update(qp, (uint8_t)flags);
	/* [한국어] signaling 비트 산출. */
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	/* [한국어] producer index 마스킹. */
	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	/* [한국어] WQE BB 할당. */
	gen_ctrl = ctrl;
	/* [한국어] gen_ctrl = 시작. */
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_SET_PSV, 0, hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0, 0);
	/* [한국어] ctrl_seg — opcode=SET_PSV, mkey 인자 0(PSV는 mkey 무관). */

	/* build umr PSV segment */
	psv = (struct mlx5_wqe_set_psv_seg *)(gen_ctrl + 1);
	/* [한국어] gen_ctrl 다음이 PSV seg. */
	/* Zeroing the set_psv segment and WQE padding. */
	memset(psv, 0, MLX5_SEND_WQE_BB - sizeof(struct mlx5_wqe_ctrl_seg));
	/* [한국어] PSV seg + WQE 패딩을 0으로(BB의 나머지 전부). */
	psv->psv_index = htobe32(psv_index);
	/* [한국어] 갱신할 PSV 인덱스(BE). */
	psv->transient_signature = htobe64(transient_signature);
	/* [한국어] CRC seed를 transient signature로 주입(BE) — signature stream 초기화. */

	mlx5_qp_wqe_submit(qp, ctrl, wqe_n_bb, pi);
	/* [한국어] WQE 제출 + 도어벨. */
	mlx5_qp_set_comp(qp, pi, wr_id, fm_ce_se, wqe_n_bb);
	/* [한국어] 완료 추적 등록. */
	assert(qp->tx_available >= wqe_n_bb);
	/* [한국어] 가용량 검증. */
	qp->tx_available -= wqe_n_bb;
	/* [한국어] 가용량 감소. */

	return 0;
}

/*
 * [한국어]
 * spdk_mlx5_umr_implementer_register - UMR을 사용하는 상위 모듈이 자신을 등록/해제
 *
 * @registered: true=등록, false=해제.
 *
 * accel_mlx5 같은 NIC offload 구현체가 init 시 true로 호출. rdma_provider_mlx5_dv가 accel_sequence
 * 지원 capability 보고를 결정할 때 이 플래그를 참조.
 */
void
spdk_mlx5_umr_implementer_register(bool registered)
{
	g_umr_implementer_registered = registered;
	/* [한국어] 글로벌 플래그 갱신 — 등록/해제. 단일 reactor init 시점 호출이라 락 불필요. */
}

/*
 * [한국어]
 * spdk_mlx5_umr_implementer_is_registered - 위 플래그 조회.
 */
bool
spdk_mlx5_umr_implementer_is_registered(void)
{
	return g_umr_implementer_registered;
	/* [한국어] 등록 여부 반환 — rdma_provider_mlx5_dv가 accel sequence 광고 결정에 사용. */
}
