/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL dirty shutdown 복구 (recovery) 단계 (ftl_mngt_recovery.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 FTL이 비정상 종료(dirty shutdown — 슈퍼블록 clean 비트가 0인 상태)된 후
 * 부팅할 때 호출되는 복구 파이프라인을 정의한다. 복구의 핵심은 디스크에 영속화된
 * P2L checkpoint와 각 밴드의 tail metadata(P2L map)를 읽어 L2P(LBA→PBA 매핑)를 처음부터
 * 재구성하는 것이다. L2P 전체를 한꺼번에 메모리에 두면 RAM 부족이 발생하므로
 * 사용자 conf.l2p_dram_limit 만큼만 메모리에 올려놓고 LBA 범위를 N개 iteration으로
 * 나눠 복구하는 패턴(snippet 기반 iteration)을 사용한다.
 *
 * 또한 SHM 기반 빠른 복구(g_desc_recovery_shm) — fast restart 케이스에서 SHM에 보존된
 * L2P 캐시 상태(페이지 inout/dirty 등)를 그대로 픽업하는 경로도 별도 정의한다.
 * 추가로 전원 끊김 도중에 발생한 trim 트랜잭션을 완료/롤백하는 trim recovery sub-process
 * 두 개(g_desc_trim_recovery, g_desc_trim_shm_recovery)도 정의한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 컨텍스트: 코어 spdk_thread. ftl_mngt_select_restore_mode가 sb->clean == 0이면
 * ftl_mngt_recover를 호출 → call_process(g_desc_recovery)로 dirty 복구 파이프라인 실행.
 *   복구 흐름:
 *     ftl_mngt_recover → call_process(g_desc_recovery) →
 *       1) recovery_init   - L2P snippet 임시 buffer 할당 + iter 초기화
 *       2) restore_band_state - 디스크 BAND_MD 읽어 각 밴드 state 결정
 *       3) p2l_init_ckpt + p2l_restore_ckpt - P2L ckpt MD 메모리 로드
 *       4) p2l_ckpt_preprocess - 각 ckpt의 seq_id 추출
 *       5) open_bands_p2l - OPEN 밴드들에 대해 P2L map 복원
 *       6) nv_cache_restore_chunk_state
 *       7) recover_seq_id (max seq_id 결정)
 *       8) recover_trim - dirty trim 트랜잭션 완료/취소
 *       9) nv_cache_recover_open_chunk
 *      10) recovery iteration 반복:
 *          a) load_l2p (snippet read) - b) init_seq_ids - c) restore_chunk_l2p
 *          - d) restore_band_l2p (모든 밴드 P2L 순회) - e) restore_valid_map
 *          - f) save_l2p (snippet write back)
 *          - 모든 LBA 범위 처리 후 종료
 *      11) recovery_deinit - 임시 buffer 해제
 *      12) init_l2p / recovery_shm_l2p / finalize_init_bands / 등 일반 startup step
 *
 * === 타 모듈과의 연결 ===
 * - 의존: lib/ftl/utils/ftl_md.[ch] (MD 비동기 IO), lib/ftl/utils/ftl_addr_utils.h
 *   (ftl_addr_load/store), lib/ftl/ftl_l2p_cache.[ch] (L2P 캐시 MD 이름),
 *   lib/ftl/ftl_band.[ch] (밴드 P2L map, tail md, addr 변환), lib/ftl/ftl_nv_cache.[ch]
 *   (chunk P2L 복구), lib/ftl/utils/ftl_bitmap.[ch] (valid_map 갱신).
 * - 의존받음: lib/ftl/mngt/ftl_mngt_startup.c (select_restore_mode → ftl_mngt_recover).
 * - 데이터 흐름: 디스크 P2L_CKPT + 각 밴드 tail md → 메모리 L2P snippet → L2P MD region →
 *   유효 매핑이 dev->valid_map에 표시됨 → finalize_init_bands가 OPEN 밴드를 writer에 재바인딩.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ftl_mngt_recovery_ctx       : 복구 process ctx (snippet/iter/open_bands/seq_id 등).
 * - struct band_md_ctx                  : restore_band_l2p step의 ctx (qd, id, status).
 * - recovery_iter_done()/advance()      : LBA 범위 iteration 진행 헬퍼.
 * - ftl_mngt_recovery_init()/deinit()  : snippet buffer 할당/해제.
 * - recovery_iteration_cb / run_iteration: iteration sub-process 호출과 콜백.
 * - restore_band_state_cb + restore_band_state: BAND_MD 로드 후 각 밴드 state 결정.
 * - ftl_mngt_recovery_walk_band_tail_md: 모든 밴드의 tail md를 비동기 로드 (high QD).
 * - ftl_mngt_recovery_iteration_init_seq_ids: trim metadata로 snippet의 seq_id 초기화.
 * - l2p_cb / iteration_load_l2p / save_l2p: snippet read/write.
 * - restore_band_l2p_cb + iteration_restore_band_l2p: 한 밴드의 P2L을 snippet에 적용.
 * - restore_chunk_l2p_cb + iteration_restore_chunk_l2p: NV cache chunk P2L을 snippet에 적용.
 * - iteration_restore_valid_map        : snippet 기반 valid_map 비트 set + band num_valid 카운트.
 * - p2l_ckpt_preprocess / p2l_ckpt_restore_p2l: P2L ckpt별 seq_id로 밴드 ckpt 매칭/복원.
 * - ftl_mngt_recovery_pre_process_p2l + recover_seq_id + open_bands_p2l: 위 헬퍼 step 어댑터.
 * - ftl_mngt_restore_valid_counters     : SHM 복구 시 valid_map 카운터 재로드.
 * - trim_pending() + 일련의 ftl_mngt_recover_trim_*: dirty trim 트랜잭션 복원/완료.
 * - g_desc_trim_recovery / g_desc_trim_shm_recovery: trim 복구 sub-process.
 * - g_desc_recovery_iteration          : 한 LBA range를 처리하는 iteration sub-process.
 * - g_desc_recovery                     : 풀 복구 sub-process.
 * - g_desc_recovery_shm                 : SHM 기반 빠른 복구 sub-process.
 * - ftl_mngt_recover()                  : 외부 step 진입점.
 */

#include "spdk/bdev_module.h"
/* [한국어] spdk_bdev API (현재 직접 호출은 없으나 의존성 명시). */

#include "ftl_nv_cache.h"
/* [한국어] NV cache chunk 자료구조와 ftl_chunk_map_get_lba 등. */
#include "ftl_core.h"
#include "ftl_utils.h"
#include "ftl_band.h"
#include "ftl_internal.h"
#include "ftl_l2p_cache.h"
/* [한국어] FTL_L2P_CACHE_MD_NAME_* — recovery 시 임시 buffer 확보 위해 unlink할 SHM 이름들. */
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "utils/ftl_addr_utils.h"
/* [한국어] ftl_addr_load / ftl_addr_store — address 크기 가변(2/4/8B) 처리. */

/*
 * [한국어]
 * struct ftl_mngt_recovery_ctx - 복구 process 전체에서 공유하는 컨텍스트.
 *
 * 가장 중요한 부분은 l2p_snippet — L2P 전체를 한 번에 메모리에 두지 않고 일정 크기씩
 * 슬라이스(snippet)로 잘라 처리할 때 한 슬라이스의 임시 buffer를 보유.
 */
struct ftl_mngt_recovery_ctx {
	/* Main recovery FTL management process */
	struct ftl_mngt_process *main;
	/* [한국어] g_desc_recovery의 mngt 핸들. iteration sub-process 콜백에서
	 * continue_step / fail_step 호출 대상. */

	int status;
	/* [한국어] 누적 status — 첫 에러를 보존. */

	TAILQ_HEAD(, ftl_band) open_bands;
	/* [한국어] restore_band_state가 OPEN 상태로 식별한 밴드들의 임시 큐.
	 * open_bands_p2l에서 각 밴드 P2L 복원 후 shut_bands로 이동. */

	uint64_t open_bands_num;
	/* [한국어] open_bands 큐의 밴드 수 (현재 사용 안 함 — 향후 확장용). */

	struct {
		struct ftl_layout_region region;
		/* [한국어] L2P 전체 region을 복사해 두고 current.offset/blocks를 snippet 단위로
		 * 갱신하며 "현재 처리 중인 슬라이스"를 표현. */

		struct ftl_md *md;
		/* [한국어] snippet 임시 buffer를 백킹하는 ftl_md 핸들 (SHM에 새로 만든 것). */

		uint64_t *l2p;
		/* [한국어] snippet의 LBA→PBA 매핑 buffer 시작 (md 버퍼의 앞부분). */

		uint64_t *seq_id;
		/* [한국어] snippet의 LBA별 seq_id buffer (md 버퍼의 뒷부분).
		 * 동일 LBA에 여러 매핑이 있을 때 가장 큰 seq를 가진 것이 최신. */

		uint64_t count;
		/* [한국어] snippet이 한 번에 처리할 LBA 수. */
	} l2p_snippet;

	struct {
		uint64_t block_limit;
		/* [한국어] iteration 한 번에 처리할 L2P region 블록 수. */

		uint64_t lba_first;
		/* [한국어] 현재 iteration의 시작 LBA. */
		uint64_t lba_last;
		/* [한국어] 현재 iteration의 끝 LBA (exclusive). */

		uint32_t i;
		/* [한국어] iteration 카운터 (디버그/로그용). */
	} iter;

	uint64_t p2l_ckpt_seq_id[FTL_LAYOUT_REGION_TYPE_P2L_COUNT];
	/* [한국어] 각 P2L checkpoint region의 seq_id 캐시. ckpt-band 매칭에 사용. */
};

static const struct ftl_mngt_process_desc g_desc_recovery_iteration;
static const struct ftl_mngt_process_desc g_desc_recovery;
static const struct ftl_mngt_process_desc g_desc_recovery_shm;
/* [한국어] 3개 desc forward — call_process가 정의 전에 참조. */

/*
 * [한국어]
 * recovery_iter_done - iteration이 모든 LBA를 처리했는지 검사.
 *
 * snippet.region.current.blocks == 0이면 더 처리할 것 없음.
 */
static bool
recovery_iter_done(struct spdk_ftl_dev *dev, struct ftl_mngt_recovery_ctx *ctx)
{
	return 0 == ctx->l2p_snippet.region.current.blocks;
}

/*
 * [한국어]
 * recovery_iter_advance - 다음 snippet 위치/LBA 범위로 진행.
 *
 * 동작:
 *  1) iter.i++ (디버그 카운터).
 *  2) snippet.region.current.offset를 이미 처리한 만큼 +=. blocks는 남은 region 크기와
 *     block_limit 중 작은 값.
 *  3) 처리 시작 블록(first_block)으로부터 lba_first/lba_last 재계산.
 *     LBA = block * (FTL_BLOCK_SIZE / addr_size) — L2P 페이지당 LBA 수.
 *  4) lba_last가 num_lbas 초과하면 num_lbas로 클램프.
 */
static void
recovery_iter_advance(struct spdk_ftl_dev *dev, struct ftl_mngt_recovery_ctx *ctx)
{
	struct ftl_layout_region *region, *snippet;
	uint64_t first_block, last_blocks;

	ctx->iter.i++;
	region = ftl_layout_region_get(dev, FTL_LAYOUT_REGION_TYPE_L2P);
	snippet = &ctx->l2p_snippet.region;

	/* Advance processed blocks */
	snippet->current.offset += snippet->current.blocks;
	snippet->current.blocks = region->current.offset + region->current.blocks - snippet->current.offset;
	snippet->current.blocks = spdk_min(snippet->current.blocks, ctx->iter.block_limit);

	first_block = snippet->current.offset - region->current.offset;
	ctx->iter.lba_first = first_block * (FTL_BLOCK_SIZE / dev->layout.l2p.addr_size);

	last_blocks = first_block + snippet->current.blocks;
	ctx->iter.lba_last = last_blocks * (FTL_BLOCK_SIZE / dev->layout.l2p.addr_size);

	if (ctx->iter.lba_last > dev->num_lbas) {
		ctx->iter.lba_last = dev->num_lbas;
	}
}

/*
 * [한국어]
 * ftl_mngt_recovery_init - 복구 시작 — snippet 임시 buffer 할당 + iter 초기화.
 *
 * 동작:
 *  1) ctx->main에 본 mngt 핸들 보관 (iteration cb에서 continue_step 호출 대상).
 *  2) fast_recovery면 snippet buffer 불필요 — 바로 next.
 *  3) L2P 캐시 SHM 파일 unlink — 복구 임시 buffer가 메모리 한계를 넘지 않도록 free 확보.
 *  4) mem_limit = conf.l2p_dram_limit MiB. lba_limit = mem_limit / (seq_id 8B + addr_size).
 *  5) iterations = ceil(num_lbas / lba_limit). block_limit = ceil(l2p_limit / 4 KiB).
 *  6) snippet.count = block_limit * lbas_in_block.
 *  7) ftl_md_create로 SHM에 snippet buffer (l2p_block + seq_id_block) 할당.
 *  8) snippet.l2p = buffer 시작, snippet.seq_id = l2p 뒤.
 *  9) recovery_iter_advance 호출(blocks=0 상태로) — 첫 iteration 시작 위치 결정.
 *  10) open_bands 큐 초기화.
 */
static void
ftl_mngt_recovery_init(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	const uint64_t lbas_in_block = FTL_BLOCK_SIZE / dev->layout.l2p.addr_size;
	uint64_t mem_limit, lba_limit, l2p_limit, iterations, seq_limit;
	uint64_t l2p_limit_block, seq_limit_block, md_blocks;
	int md_flags;

	ctx->main = mngt;

	if (ftl_fast_recovery(dev)) {
		/* If shared memory fast recovery then we don't need temporary buffers */
		/* [한국어] SHM 픽업 — snippet 불필요. g_desc_recovery_shm으로 처리. */
		ftl_mngt_next_step(mngt);
		return;
	}

	/*
	 * Recovery process allocates temporary buffers, to not exceed memory limit free L2P
	 * metadata buffers if they exist, they will be recreated in L2P initialization phase
	 */
	/* [한국어] L2P 캐시가 SHM에 잡고 있는 메모리를 풀어줌 — 복구 buffer 위해 메모리 확보.
	 * L2P는 init_l2p 단계에서 다시 생성됨. */
	ftl_md_unlink(dev, FTL_L2P_CACHE_MD_NAME_L1, ftl_md_create_shm_flags(dev));
	ftl_md_unlink(dev, FTL_L2P_CACHE_MD_NAME_L2, ftl_md_create_shm_flags(dev));
	ftl_md_unlink(dev, FTL_L2P_CACHE_MD_NAME_L2_CTX, ftl_md_create_shm_flags(dev));

	/* Below values are in byte unit */
	mem_limit = dev->conf.l2p_dram_limit * MiB;
	mem_limit = spdk_min(mem_limit, spdk_round_up(dev->num_lbas * dev->layout.l2p.addr_size, MiB));
	/* [한국어] L2P 전체 크기보다 작으면 그 값으로 클램프 — 작은 디바이스에서 메모리 절약. */

	lba_limit = mem_limit / (sizeof(uint64_t) + dev->layout.l2p.addr_size);
	/* [한국어] LBA마다 8B(seq_id) + addr_size(매핑) 메모리 필요 — mem_limit으로 처리 가능한 LBA 수. */
	l2p_limit = lba_limit * dev->layout.l2p.addr_size;
	iterations = spdk_divide_round_up(dev->num_lbas, lba_limit);

	ctx->iter.block_limit = spdk_divide_round_up(l2p_limit, FTL_BLOCK_SIZE);

	/* Round to block size */
	ctx->l2p_snippet.count = ctx->iter.block_limit * lbas_in_block;

	seq_limit = ctx->l2p_snippet.count * sizeof(uint64_t);

	FTL_NOTICELOG(dev, "Recovery memory limit: %"PRIu64"MiB\n", (uint64_t)(mem_limit / MiB));
	FTL_NOTICELOG(dev, "L2P resident size: %"PRIu64"MiB\n", (uint64_t)(l2p_limit / MiB));
	FTL_NOTICELOG(dev, "Seq ID resident size: %"PRIu64"MiB\n", (uint64_t)(seq_limit / MiB));
	FTL_NOTICELOG(dev, "Recovery iterations: %"PRIu64"\n", iterations);
	dev->sb->ckpt_seq_id = 0;

	/* Initialize region */
	ctx->l2p_snippet.region = *ftl_layout_region_get(dev, FTL_LAYOUT_REGION_TYPE_L2P);
	/* Limit blocks in region, it will be needed for ftl_md_set_region */
	ctx->l2p_snippet.region.current.blocks = ctx->iter.block_limit;

	l2p_limit_block = ctx->iter.block_limit;
	seq_limit_block = spdk_divide_round_up(seq_limit, FTL_BLOCK_SIZE);

	md_blocks = l2p_limit_block + seq_limit_block;
	md_flags = FTL_MD_CREATE_SHM | FTL_MD_CREATE_SHM_NEW;

	/* Initialize snippet of L2P metadata */
	ctx->l2p_snippet.md = ftl_md_create(dev, md_blocks, 0, "l2p_recovery", md_flags,
					    &ctx->l2p_snippet.region);
	/* [한국어] SHM에 새 region 생성 — 복구 끝나면 unlink. */
	if (!ctx->l2p_snippet.md) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	ctx->l2p_snippet.l2p = ftl_md_get_buffer(ctx->l2p_snippet.md);

	/* Initialize recovery iterator, we call it with blocks set to zero,
	 * it means zero block done (processed), thanks that it will recalculate
	 *  offsets and starting LBA to initial position */
	ctx->l2p_snippet.region.current.blocks = 0;
	recovery_iter_advance(dev, ctx);
	/* [한국어] blocks=0 상태에서 advance하면 첫 iteration 시작 위치(offset=region.start, lba=0)로 셋. */

	/* Initialize snippet of sequence IDs */
	ctx->l2p_snippet.seq_id = (uint64_t *)((char *)ftl_md_get_buffer(ctx->l2p_snippet.md) +
					       (l2p_limit_block * FTL_BLOCK_SIZE));
	/* [한국어] seq_id buffer는 l2p buffer 바로 뒤에 위치. */

	TAILQ_INIT(&ctx->open_bands);
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_recovery_deinit - snippet 임시 buffer 해제.
 */
static void
ftl_mngt_recovery_deinit(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *ctx = ftl_mngt_get_process_ctx(mngt);

	ftl_md_destroy(ctx->l2p_snippet.md, 0);
	ctx->l2p_snippet.md = NULL;
	ctx->l2p_snippet.seq_id = NULL;

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * recovery_iteration_cb - iteration sub-process 완료 콜백.
 *
 * 동작: iter advance → status에 따라 main을 fail/continue_step (= run_iteration 재호출).
 */
static void
recovery_iteration_cb(struct spdk_ftl_dev *dev, void *_ctx, int status)
{
	struct ftl_mngt_recovery_ctx *ctx = _ctx;

	recovery_iter_advance(dev, ctx);

	if (status) {
		ftl_mngt_fail_step(ctx->main);
	} else {
		ftl_mngt_continue_step(ctx->main);
		/* [한국어] 같은 step 재호출 → run_iteration이 다시 호출되며 다음 snippet 처리. */
	}
}

/*
 * [한국어]
 * ftl_mngt_recovery_run_iteration - "다음 iteration" step. 끝났으면 next, 아니면 sub-process 호출.
 */
static void
ftl_mngt_recovery_run_iteration(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *ctx = ftl_mngt_get_process_ctx(mngt);

	if (ftl_fast_recovery(dev)) {
		ftl_mngt_skip_step(mngt);
		return;
	}

	if (recovery_iter_done(dev, ctx)) {
		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_process_execute(dev, &g_desc_recovery_iteration, recovery_iteration_cb, ctx);
	}
}

/*
 * [한국어]
 * restore_band_state_cb - BAND_MD 디스크 read 완료 후 각 밴드 state로 큐 분류.
 *
 * 모든 밴드를 순회:
 *  - FREE: ftl_band_initialize_free_state로 free_bands 큐에 INSERT.
 *  - OPEN: shut_bands에서 빼서 ctx->open_bands(임시 큐)에 push.
 *  - CLOSED: 그대로 shut_bands에 둠.
 *  - 그 외 = 손상.
 */
static void
restore_band_state_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;
	struct ftl_mngt_recovery_ctx *pctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_band *band;
	uint64_t num_bands = ftl_get_num_bands(dev);
	uint64_t i;

	if (status) {
		/* Restore error, end step */
		ftl_mngt_fail_step(mngt);
		return;
	}

	for (i = 0; i < num_bands; i++) {
		band = &dev->bands[i];

		switch (band->md->state) {
		case FTL_BAND_STATE_FREE:
			ftl_band_initialize_free_state(band);
			break;
		case FTL_BAND_STATE_OPEN:
			TAILQ_REMOVE(&band->dev->shut_bands, band, queue_entry);
			TAILQ_INSERT_HEAD(&pctx->open_bands, band, queue_entry);
			/* [한국어] OPEN은 P2L 복원 필요 — 임시 큐로 분리. */
			break;
		case FTL_BAND_STATE_CLOSED:
			break;
		default:
			status = -EINVAL;
		}
	}

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_recovery_restore_band_state - BAND_MD 비동기 read step.
 */
static void
ftl_mngt_recovery_restore_band_state(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_BAND_MD];

	md->owner.cb_ctx = mngt;
	md->cb = restore_band_state_cb;
	ftl_md_restore(md);
}

/*
 * [한국어]
 * struct band_md_ctx - walk_band_tail_md step의 ctx (high QD 비동기 진행 상태).
 */
struct band_md_ctx {
	int status;
	/* [한국어] 누적 status. */
	uint64_t qd;
	/* [한국어] 현재 in-flight 비동기 read 수 — 0이고 모든 밴드 처리 끝나면 step 종료. */
	uint64_t id;
	/* [한국어] 다음 처리할 밴드 ID. */
};

/*
 * [한국어]
 * ftl_mngt_recovery_walk_band_tail_md - 모든 밴드의 tail metadata를 high QD로 비동기 read.
 *
 * @cb: 각 밴드 read 완료 시 호출될 사용자 콜백.
 *
 * 동작: 다음 패턴으로 호출됨 — 처음 한 번 + cb에서 continue_step으로 반복 호출.
 *  1) qd == 0 && id == num_bands면 모두 끝 — fail/next.
 *  2) id < num_bands 동안:
 *     - FREE band면 skip.
 *     - OPEN/FULL band면 P2L map 이미 메모리에 있음 — 즉시 cb 호출 후 다음 밴드.
 *     - CLOSED band면 ckpt_seq_id 이상이면 skip, 아니면 P2L map 할당 + tail md read 발행.
 *     - p2l_alloc 실패 시 break (메모리 부족 — 다음 iteration에서 재시도).
 *  3) qd가 0이면 continue_step (cb 한 번도 안 불려서 dispatch 필요).
 */
static void
ftl_mngt_recovery_walk_band_tail_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
				    ftl_band_md_cb cb)
{
	struct band_md_ctx *sctx = ftl_mngt_get_step_ctx(mngt);
	uint64_t num_bands = ftl_get_num_bands(dev);

	/*
	 * This function generates a high queue depth and will utilize ftl_mngt_continue_step during completions to make sure all bands
	 * are processed before returning an error (if any were found) or continuing on.
	 */
	if (0 == sctx->qd && sctx->id == num_bands) {
		if (sctx->status) {
			ftl_mngt_fail_step(mngt);
		} else {
			ftl_mngt_next_step(mngt);
		}
		return;
	}

	while (sctx->id < num_bands) {
		struct ftl_band *band = &dev->bands[sctx->id];

		if (FTL_BAND_STATE_FREE == band->md->state) {
			sctx->id++;
			continue;
		}

		if (FTL_BAND_STATE_OPEN == band->md->state || FTL_BAND_STATE_FULL == band->md->state) {
			/* This band is already open and has valid P2L map */
			/* [한국어] OPEN/FULL은 open_bands_p2l에서 이미 P2L 메모리 복원함 — read 불필요. */
			sctx->id++;
			sctx->qd++;
			ftl_band_acquire_p2l_map(band);
			cb(band, mngt, FTL_MD_SUCCESS);
			continue;
		} else {
			if (dev->sb->ckpt_seq_id && (band->md->close_seq_id <= dev->sb->ckpt_seq_id)) {
				/* [한국어] checkpoint 이전에 이미 close된 밴드 — checkpoint 데이터로 복구 가능 → skip. */
				sctx->id++;
				continue;
			}

			band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
			if (ftl_band_alloc_p2l_map(band)) {
				/* No more free P2L map, try later */
				/* [한국어] P2L pool 고갈 — 일단 break, 나중에 in-flight 완료되면 재시도. */
				break;
			}
		}

		sctx->id++;
		ftl_band_read_tail_brq_md(band, cb, mngt);
		sctx->qd++;
	}

	if (0 == sctx->qd) {
		/*
		 * No QD could happen due to all leftover bands being in free state.
		 * For streamlining of all potential error handling (since many bands are reading P2L at the same time),
		 * we're using ftl_mngt_continue_step to arrive at the same spot of checking for mngt step end (see beginning of function).
		 */
		/* [한국어] in-flight도 없고 새 발행도 없음 — continue_step으로 본 함수 재진입해 종료 분기. */
		ftl_mngt_continue_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_recovery_iteration_init_seq_ids - 현재 iteration의 LBA 범위에 대해
 *                                            snippet의 seq_id를 trim_map의 값으로 초기화.
 *
 * trim_map은 L2P page 단위로 trim 발생 시점 seq_id 보관. snippet의 각 LBA에 대해
 * "이 LBA가 trim된 시점" seq_id를 셋팅하면 이후 P2L 복원 시 그보다 오래된 매핑은 무시됨.
 */
static void
ftl_mngt_recovery_iteration_init_seq_ids(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *ctx = ftl_mngt_get_caller_ctx(mngt);
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD];
	uint64_t *trim_map = ftl_md_get_buffer(md);
	uint64_t page_id, trim_seq_id;
	uint32_t lbas_in_page = FTL_BLOCK_SIZE / dev->layout.l2p.addr_size;
	uint64_t lba, lba_off;

	if (dev->sb->ckpt_seq_id) {
		/* [한국어] sb에 checkpoint seq_id 저장된 케이스(부분 체크포인트 복구)는 미지원. */
		FTL_ERRLOG(dev, "Checkpoint recovery not supported!\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	for (lba = ctx->iter.lba_first; lba < ctx->iter.lba_last; lba++) {
		lba_off = lba - ctx->iter.lba_first;
		page_id = lba / lbas_in_page;

		assert(page_id < ftl_md_get_buffer_size(md) / sizeof(*trim_map));
		assert(page_id < ftl_layout_region_get(dev, FTL_LAYOUT_REGION_TYPE_L2P)->current.blocks);
		assert(lba_off < ctx->l2p_snippet.count);

		trim_seq_id = trim_map[page_id];

		ctx->l2p_snippet.seq_id[lba_off] = trim_seq_id;
		ftl_addr_store(dev, ctx->l2p_snippet.l2p, lba_off, FTL_ADDR_INVALID);
		/* [한국어] snippet의 매핑은 일단 모두 INVALID로 초기화. 이후 P2L 복원이 채움. */
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * l2p_cb - L2P snippet read/write 완료 콜백.
 */
static void
l2p_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_recovery_iteration_load_l2p - 현재 snippet 영역을 디스크 L2P region에서 read.
 *
 * 이전에 save된 L2P 데이터가 있으면 그걸 base로, 없으면 0으로 채워진 buffer가 됨.
 * 이후 init_seq_ids가 trim seq를 덮어씀.
 */
static void
ftl_mngt_recovery_iteration_load_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *ctx = ftl_mngt_get_caller_ctx(mngt);
	struct ftl_md *md = ctx->l2p_snippet.md;
	struct ftl_layout_region *region = &ctx->l2p_snippet.region;

	FTL_NOTICELOG(dev, "L2P recovery, iteration %u\n", ctx->iter.i);
	FTL_NOTICELOG(dev, "Load L2P, blocks [%"PRIu64", %"PRIu64"), LBAs [%"PRIu64", %"PRIu64")\n",
		      region->current.offset, region->current.offset + region->current.blocks,
		      ctx->iter.lba_first, ctx->iter.lba_last);

	ftl_md_set_region(md, &ctx->l2p_snippet.region);
	/* [한국어] md의 region을 현재 snippet 위치로 갱신 — 다음 read가 이 영역만 대상으로. */

	md->owner.cb_ctx = mngt;
	md->cb = l2p_cb;
	ftl_md_restore(md);
}

/*
 * [한국어]
 * ftl_mngt_recovery_iteration_save_l2p - 갱신된 snippet을 디스크 L2P region에 write back.
 */
static void
ftl_mngt_recovery_iteration_save_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *ctx = ftl_mngt_get_caller_ctx(mngt);
	struct ftl_md *md = ctx->l2p_snippet.md;

	md->owner.cb_ctx = mngt;
	md->cb = l2p_cb;
	ftl_md_persist(md);
}

/*
 * [한국어]
 * restore_band_l2p_cb - 단일 밴드 tail md read 완료 콜백. P2L map을 snippet에 적용.
 *
 * @band: read 완료된 밴드. band->p2l_map.band_map에 P2L 데이터 들어 있음.
 * @cntx: mngt 핸들.
 * @status: read 결과.
 *
 * 동작:
 *  1) status 검사 + P2L CRC 검증 (CLOSED 밴드만).
 *  2) P2L map의 모든 항목 순회:
 *     - lba INVALID이면 skip.
 *     - lba가 num_lbas 초과면 손상 → fail.
 *     - 현재 iteration 범위 밖이면 skip (snippet에 안 들어감).
 *     - snippet의 seq_id가 더 크면 (= 더 최신 매핑이 이미 있음):
 *         OPEN/FULL 밴드의 P2L에 있는 이 entry를 INVALID로 표시 (stale).
 *         skip.
 *     - 아니면 이 entry가 더 최신이거나 동률 — snippet 갱신:
 *         curr_addr가 다른 OPEN/FULL 밴드를 가리키는 경우 그 밴드 P2L도 stale로 마킹.
 *         snippet.l2p[lba_off] = addr, snippet.seq_id[lba_off] = seq_id.
 *  3) p2l map release, qd--.
 *  4) continue_step → walk_band_tail_md 재호출 → 다음 밴드 발행.
 */
static void
restore_band_l2p_cb(struct ftl_band *band, void *cntx, enum ftl_md_status status)
{
	struct ftl_mngt_process *mngt = cntx;
	struct ftl_mngt_recovery_ctx *pctx = ftl_mngt_get_caller_ctx(mngt);
	struct band_md_ctx *sctx = ftl_mngt_get_step_ctx(mngt);
	struct spdk_ftl_dev *dev = band->dev;
	ftl_addr addr, curr_addr;
	uint64_t i, lba, seq_id, num_blks_in_band;
	uint32_t band_map_crc;
	int rc = 0;

	if (status != FTL_MD_SUCCESS) {
		FTL_ERRLOG(dev, "L2P band restore error, failed to read P2L map\n");
		rc = -EIO;
		goto cleanup;
	}

	band_map_crc = spdk_crc32c_update(band->p2l_map.band_map,
					  ftl_tail_md_num_blocks(band->dev) * FTL_BLOCK_SIZE, 0);

	/* P2L map is only valid if the band state is closed */
	if (FTL_BAND_STATE_CLOSED == band->md->state && band->md->p2l_map_checksum != band_map_crc) {
		/* [한국어] CLOSED 밴드는 P2L CRC 검증 — 불일치 = 디스크 손상. */
		FTL_ERRLOG(dev, "L2P band restore error, inconsistent P2L map CRC\n");
		ftl_stats_crc_error(dev, FTL_STATS_TYPE_MD_BASE);
		rc = -EINVAL;
		goto cleanup;
	}

	num_blks_in_band = ftl_get_num_blocks_in_band(dev);
	for (i = 0; i < num_blks_in_band; ++i) {
		uint64_t lba_off;
		lba = band->p2l_map.band_map[i].lba;
		seq_id = band->p2l_map.band_map[i].seq_id;

		if (lba == FTL_LBA_INVALID) {
			continue;
		}
		if (lba >= dev->num_lbas) {
			FTL_ERRLOG(dev, "L2P band restore ERROR, LBA out of range\n");
			rc = -EINVAL;
			break;
		}
		if (lba < pctx->iter.lba_first || lba >= pctx->iter.lba_last) {
			/* [한국어] 현재 snippet 범위 밖 — 다른 iteration에서 처리됨. */
			continue;
		}

		lba_off = lba - pctx->iter.lba_first;
		if (seq_id < pctx->l2p_snippet.seq_id[lba_off]) {

			/* Overlapped band/chunk has newer data - invalidate P2L map on open/full band  */
			/* [한국어] 다른 곳에 더 새 매핑이 있음 — OPEN/FULL 밴드면 이 entry를 stale로 마킹. */
			if (FTL_BAND_STATE_OPEN == band->md->state || FTL_BAND_STATE_FULL == band->md->state) {
				addr = ftl_band_addr_from_block_offset(band, i);
				ftl_band_set_p2l(band, FTL_LBA_INVALID, addr, 0);
			}

			/* Newer data already recovered */
			continue;
		}

		addr = ftl_band_addr_from_block_offset(band, i);

		curr_addr = ftl_addr_load(dev, pctx->l2p_snippet.l2p, lba_off);

		/* Overlapped band/chunk has newer data - invalidate P2L map on open/full band  */
		if (curr_addr != FTL_ADDR_INVALID && !ftl_addr_in_nvc(dev, curr_addr) && curr_addr != addr) {
			/* [한국어] snippet에 다른 base 밴드가 이 LBA를 가리키는 경우 — 그 밴드의 P2L에서
			 * 이 entry를 stale로 마킹. NV cache 매핑은 별도 처리. */
			struct ftl_band *curr_band = ftl_band_from_addr(dev, curr_addr);

			if (FTL_BAND_STATE_OPEN == curr_band->md->state || FTL_BAND_STATE_FULL == curr_band->md->state) {
				size_t prev_offset = ftl_band_block_offset_from_addr(curr_band, curr_addr);
				if (curr_band->p2l_map.band_map[prev_offset].lba == lba &&
				    seq_id >= curr_band->p2l_map.band_map[prev_offset].seq_id) {
					ftl_band_set_p2l(curr_band, FTL_LBA_INVALID, curr_addr, 0);
				}
			}
		}

		ftl_addr_store(dev, pctx->l2p_snippet.l2p, lba_off, addr);
		pctx->l2p_snippet.seq_id[lba_off] = seq_id;
	}


cleanup:
	ftl_band_release_p2l_map(band);

	sctx->qd--;
	if (rc) {
		sctx->status = rc;
	}

	ftl_mngt_continue_step(mngt);
	/* [한국어] walk_band_tail_md 재호출 → 다음 밴드 발행 또는 종료. */
}

/*
 * [한국어]
 * ftl_mngt_recovery_iteration_restore_band_l2p - 모든 밴드 P2L을 snippet에 적용 (high QD).
 */
static void
ftl_mngt_recovery_iteration_restore_band_l2p(struct spdk_ftl_dev *dev,
		struct ftl_mngt_process *mngt)
{
	ftl_mngt_recovery_walk_band_tail_md(dev, mngt, restore_band_l2p_cb);
}

/*
 * [한국어]
 * restore_chunk_l2p_cb - 단일 NV cache chunk의 P2L을 snippet에 적용.
 *
 * @chunk: 처리할 NV cache chunk.
 * @ctx:   ftl_mngt_recovery_ctx.
 * @return: 0=성공, -1=실패.
 *
 * band 버전과 동일한 패턴 — chunk_map 순회, seq 비교 후 snippet 갱신.
 * 차이점: chunk 주소는 ftl_addr_from_nvc_offset으로 변환, NV cache는 OPEN 상태 마킹 안 함.
 */
static int
restore_chunk_l2p_cb(struct ftl_nv_cache_chunk *chunk, void *ctx)
{
	struct ftl_mngt_recovery_ctx *pctx = ctx;
	struct spdk_ftl_dev *dev;
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	ftl_addr addr;
	const uint64_t seq_id = chunk->md->seq_id;
	uint64_t i, lba;
	uint32_t chunk_map_crc;

	dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);

	chunk_map_crc = spdk_crc32c_update(chunk->p2l_map.chunk_map,
					   ftl_nv_cache_chunk_tail_md_num_blocks(chunk->nv_cache) * FTL_BLOCK_SIZE, 0);
	if (chunk->md->p2l_map_checksum != chunk_map_crc) {
		ftl_stats_crc_error(dev, FTL_STATS_TYPE_MD_NV_CACHE);
		return -1;
	}

	for (i = 0; i < nv_cache->chunk_blocks; ++i) {
		uint64_t lba_off;

		lba = ftl_chunk_map_get_lba(chunk, i);

		if (lba == FTL_LBA_INVALID) {
			continue;
		}
		if (lba >= dev->num_lbas) {
			FTL_ERRLOG(dev, "L2P Chunk restore ERROR, LBA out of range\n");
			return -1;
		}
		if (lba < pctx->iter.lba_first || lba >= pctx->iter.lba_last) {
			continue;
		}

		lba_off = lba - pctx->iter.lba_first;
		if (seq_id < pctx->l2p_snippet.seq_id[lba_off]) {
			/* Newer data already recovered */
			continue;
		}

		addr = ftl_addr_from_nvc_offset(dev, chunk->offset + i);
		ftl_addr_store(dev, pctx->l2p_snippet.l2p, lba_off, addr);
		pctx->l2p_snippet.seq_id[lba_off] = seq_id;
	}

	return 0;
}

/*
 * [한국어]
 * ftl_mngt_recovery_iteration_restore_chunk_l2p - NV cache 모든 chunk P2L을 snippet에 적용.
 *
 * 실제 chunk 순회는 ftl_mngt_nv_cache_restore_l2p이 담당, 본 함수는 콜백만 등록.
 */
static void
ftl_mngt_recovery_iteration_restore_chunk_l2p(struct spdk_ftl_dev *dev,
		struct ftl_mngt_process *mngt)
{
	ftl_mngt_nv_cache_restore_l2p(dev, mngt, restore_chunk_l2p_cb, ftl_mngt_get_caller_ctx(mngt));
}

/*
 * [한국어]
 * ftl_mngt_recovery_iteration_restore_valid_map - snippet의 매핑들에 대해 valid_map과
 *                                                  band->p2l_map.num_valid를 갱신.
 *
 * snippet의 각 매핑(addr)에 대해:
 *  - INVALID이면 skip.
 *  - base 영역이면 band->p2l_map.num_valid++.
 *  - valid_map이 이미 set이면 더블 ref = 손상 → fail.
 *  - 아니면 valid_map 비트 set.
 */
static void
ftl_mngt_recovery_iteration_restore_valid_map(struct spdk_ftl_dev *dev,
		struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *pctx = ftl_mngt_get_caller_ctx(mngt);
	uint64_t lba, lba_off;
	ftl_addr addr;

	for (lba = pctx->iter.lba_first; lba < pctx->iter.lba_last; lba++) {
		lba_off = lba - pctx->iter.lba_first;
		addr = ftl_addr_load(dev, pctx->l2p_snippet.l2p, lba_off);

		if (addr == FTL_ADDR_INVALID) {
			continue;
		}

		if (!ftl_addr_in_nvc(dev, addr)) {
			struct ftl_band *band = ftl_band_from_addr(dev, addr);
			band->p2l_map.num_valid++;
		}

		if (ftl_bitmap_get(dev->valid_map, addr)) {
			assert(false);
			ftl_mngt_fail_step(mngt);
			return;
		} else {
			ftl_bitmap_set(dev->valid_map, addr);
		}
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * p2l_ckpt_preprocess - 모든 P2L checkpoint region의 seq_id를 ctx 캐시에 저장.
 *
 * 이후 p2l_ckpt_restore_p2l이 밴드의 seq와 매칭되는 ckpt를 빠르게 찾을 수 있도록.
 */
static void
p2l_ckpt_preprocess(struct spdk_ftl_dev *dev, struct ftl_mngt_recovery_ctx *pctx)
{
	uint64_t seq_id;
	int md_region, ckpt_id;

	for (md_region = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     md_region <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX; md_region++) {
		ckpt_id = md_region - FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
		seq_id = ftl_mngt_p2l_ckpt_get_seq_id(dev, md_region);
		pctx->p2l_ckpt_seq_id[ckpt_id] = seq_id;
		FTL_NOTICELOG(dev, "P2L ckpt_id=%d found seq_id=%"PRIu64"\n", ckpt_id, seq_id);
	}
}

/*
 * [한국어]
 * p2l_ckpt_restore_p2l - 단일 OPEN 밴드에 대해 매칭되는 P2L ckpt를 찾아 P2L 복원.
 *
 * @return: 0=성공/매칭 없음(빈 밴드), -1=복원 실패.
 *
 * 동작:
 *  1) band->p2l_map.band_map을 -1(INVALID)로 fill.
 *  2) 모든 ckpt 순회 — seq_id가 band->md->seq와 일치하는 ckpt 발견 시
 *     ftl_mngt_p2l_ckpt_restore로 복원.
 *  3) 매칭 ckpt 없으면 빈 밴드로 처리(write pointer 0).
 */
static int
p2l_ckpt_restore_p2l(struct ftl_mngt_recovery_ctx *pctx, struct ftl_band *band)
{
	uint64_t seq_id;
	int md_region, ckpt_id;

	memset(band->p2l_map.band_map, -1,
	       FTL_BLOCK_SIZE * ftl_p2l_map_num_blocks(band->dev));
	/* [한국어] 모든 entry를 INVALID(-1)로 — ckpt에 없는 블록은 미사용으로 인식. */

	for (md_region = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     md_region <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX; md_region++) {
		ckpt_id = md_region - FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
		seq_id = pctx->p2l_ckpt_seq_id[ckpt_id];
		if (seq_id == band->md->seq) {
			FTL_NOTICELOG(band->dev, "Restore band P2L band_id=%u ckpt_id=%d seq_id=%"
				      PRIu64"\n", band->id, ckpt_id, seq_id);
			return ftl_mngt_p2l_ckpt_restore(band, md_region, seq_id);
		}
	}

	/* Band opened but no valid blocks within it, set write pointer to 0 */
	/* [한국어] 매칭 ckpt 없음 = 밴드를 열었지만 아직 데이터가 없는 상태 — write pointer 0으로. */
	ftl_band_iter_init(band);
	FTL_NOTICELOG(band->dev, "Restore band P2L band_id=%u, band_seq_id=%"PRIu64" does not"
		      " match any P2L checkpoint\n", band->id, band->md->seq);
	return 0;
}

/*
 * [한국어]
 * ftl_mngt_recovery_pre_process_p2l - p2l_ckpt_preprocess step 어댑터.
 */
static void
ftl_mngt_recovery_pre_process_p2l(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *pctx = ftl_mngt_get_process_ctx(mngt);

	p2l_ckpt_preprocess(dev, pctx);
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_recover_seq_id - max seq_id 결정 step 어댑터 (band_mngt 파일의 함수 호출).
 */
static void
ftl_mngt_recover_seq_id(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_recover_max_seq(dev);
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_recovery_open_bands_p2l - OPEN 밴드들에 대해 P2L 복원 + writer 준비.
 *
 * 첫 호출 시 모든 OPEN 밴드의 P2L map을 alloc하고 ckpt에서 복원. 이후 한 번에 한 밴드씩
 * shut_bands로 이동시키며 continue_step으로 반복 호출.
 *
 * 동작:
 *  1) open_bands 큐가 비었으면 종료 (status에 따라 fail/next).
 *  2) 첫 진입(step ctx 없음)이면 모든 OPEN 밴드 P2L 준비:
 *     - df_p2l_map = INVALID로 셋팅.
 *     - p2l_map alloc.
 *     - ckpt에서 P2L 복원.
 *     - p2l_map.p2l_ckpt 미할당이면 region_type 기반 acquire.
 *  3) 첫 OPEN 밴드를 큐에서 빼서 shut_bands로 이동.
 *  4) 밴드가 가득 찼으면 state를 FULL로.
 *  5) continue_step으로 반복 — 모든 밴드 처리 후 빈 큐로 종료.
 */
static void
ftl_mngt_recovery_open_bands_p2l(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *pctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_band *band;

	if (TAILQ_EMPTY(&pctx->open_bands)) {
		FTL_NOTICELOG(dev, "No more open bands to recover from P2L\n");
		if (pctx->status) {
			ftl_mngt_fail_step(mngt);
		} else {
			ftl_mngt_next_step(mngt);
		}
		return;
	}

	if (!ftl_mngt_get_step_ctx(mngt)) {
		ftl_mngt_alloc_step_ctx(mngt, sizeof(bool));

		/* Step first time called, initialize */
		TAILQ_FOREACH(band, &pctx->open_bands, queue_entry) {
			band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
			if (ftl_band_alloc_p2l_map(band)) {
				FTL_ERRLOG(dev, "Open band recovery ERROR, Cannot allocate P2L map\n");
				ftl_mngt_fail_step(mngt);
				return;
			}

			if (p2l_ckpt_restore_p2l(pctx, band)) {
				FTL_ERRLOG(dev, "Open band recovery ERROR, Cannot restore P2L\n");
				ftl_mngt_fail_step(mngt);
				return;
			}

			if (!band->p2l_map.p2l_ckpt) {
				band->p2l_map.p2l_ckpt = ftl_p2l_ckpt_acquire_region_type(dev, band->md->p2l_md_region);
				if (!band->p2l_map.p2l_ckpt) {
					FTL_ERRLOG(dev, "Open band recovery ERROR, Cannot acquire P2L\n");
					ftl_mngt_fail_step(mngt);
					return;
				}
			}
		}
	}

	band = TAILQ_FIRST(&pctx->open_bands);

	if (ftl_band_filled(band, band->md->iter.offset)) {
		band->md->state = FTL_BAND_STATE_FULL;
		/* [한국어] 밴드가 이미 꽉 참 — FULL로 마킹. */
	}

	/* In a next step (finalize band initialization) this band will
	 * be assigned to the writer. So temporary we move this band
	 * to the closed list, and in the next step it will be moved to
	 * the writer from such list.
	 */
	/* [한국어] open_bands에서 shut_bands로 이동 — finalize_init_bands가 이 큐를 보고
	 * OPEN/FULL 밴드를 적절한 writer에 재바인딩. */
	TAILQ_REMOVE(&pctx->open_bands, band, queue_entry);
	TAILQ_INSERT_TAIL(&dev->shut_bands, band, queue_entry);

	FTL_NOTICELOG(dev, "Open band recovered, id = %u, seq id %"PRIu64", write offset %"PRIu64"\n",
		      band->id, band->md->seq, band->md->iter.offset);

	ftl_mngt_continue_step(mngt);
	/* [한국어] 본 함수 재호출 — 다음 OPEN 밴드 처리 또는 종료. */
}

/*
 * [한국어]
 * ftl_mngt_restore_valid_counters - SHM 복구 시 valid_map 카운터(밴드별 num_valid) 재로드.
 */
static void
ftl_mngt_restore_valid_counters(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_valid_map_load_state(dev);
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * trim_pending - TRIM_LOG에 미완료 trim 트랜잭션이 있는지 검사.
 *
 * trim은 atomic하지 않아 여러 단계로 진행되는데, 도중에 전원이 끊기면 TRIM_LOG에
 * "이 트랜잭션 진행 중" 표시가 남아 있음. 본 함수는 그 표시를 검사.
 */
static bool
trim_pending(struct spdk_ftl_dev *dev)
{
	struct ftl_trim_log *log = ftl_md_get_buffer(dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_LOG]);

	if (log->hdr.trim.seq_id) {
		return true;
	}

	return false;
}

/*
 * [한국어]
 * ftl_mngt_recover_trim_cb - trim md persist/restore 완료 콜백.
 */
static void
ftl_mngt_recover_trim_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;
	if (!status) {
		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_complete_trim - SHM의 trim 진행 상태를 완료시키는 step (SHM 복구 경로).
 *
 * sb_shm->trim.in_progress가 true면 그 시점의 (start_lba, num_blocks, seq_id)로 trim_map
 * 갱신. 또한 trim_pending이 true면 로그 출력.
 */
static void
ftl_mngt_complete_trim(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	uint64_t start_lba, num_blocks, seq_id;

	if (dev->sb_shm->trim.in_progress) {
		start_lba = dev->sb_shm->trim.start_lba;
		num_blocks = dev->sb_shm->trim.num_blocks;
		seq_id = dev->sb_shm->trim.seq_id;
		assert(seq_id <= dev->sb->seq_id);
		ftl_set_trim_map(dev, start_lba, num_blocks, seq_id);
	}

	if (trim_pending(dev)) {
		struct ftl_trim_log *log = ftl_md_get_buffer(dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_LOG]);
		FTL_NOTICELOG(dev, "Incomplete trim detected lba: %"PRIu64" num_blocks: %"PRIu64"\n",
			      log->hdr.trim.start_lba, log->hdr.trim.num_blocks);
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_recover_trim_md - TRIM_MD region 디스크에서 read.
 */
static void
ftl_mngt_recover_trim_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD];

	md->owner.cb_ctx = mngt;
	md->cb = ftl_mngt_recover_trim_cb;
	ftl_md_restore(md);
}

/*
 * [한국어]
 * ftl_mngt_recover_trim_md_persist - 보류 trim이 있으면 TRIM_MD를 디스크에 write.
 */
static void
ftl_mngt_recover_trim_md_persist(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD];

	if (!trim_pending(dev)) {
		/* No pending trim logged */
		ftl_mngt_skip_step(mngt);
		return;
	}

	md->owner.cb_ctx = mngt;
	md->cb = ftl_mngt_recover_trim_cb;
	ftl_md_persist(md);
}

/*
 * [한국어]
 * ftl_mngt_recover_trim_log_cb - TRIM_LOG read 완료 후 trim 트랜잭션 완료 처리.
 *
 * trim_pending이면 LOG에 기록된 (lba, num_blocks, seq_id)로 TRIM_MD의 해당 page들을
 * seq_id로 갱신 (단, 기존 값이 더 크면 손상 → fail).
 *
 * lba/num_blocks가 page 정렬 안 맞으면 손상. page[i]가 seq_id보다 크면 손상.
 */
static void
ftl_mngt_recover_trim_log_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;
	struct ftl_trim_log *log = ftl_md_get_buffer(md);
	uint64_t *page;

	if (status) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (!trim_pending(dev)) {
		/* No pending trim logged */
		ftl_mngt_skip_step(mngt);
		return;
	}

	/* Pending trim, complete the trim transaction */
	const uint64_t seq_id = log->hdr.trim.seq_id;
	const uint64_t lba = log->hdr.trim.start_lba;
	const uint64_t num_blocks = log->hdr.trim.num_blocks;
	const uint64_t lbas_in_page = FTL_BLOCK_SIZE / dev->layout.l2p.addr_size;
	const uint64_t first_page = lba / lbas_in_page;
	const uint64_t num_pages = num_blocks / lbas_in_page;

	page = ftl_md_get_buffer(dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD]);

	if (lba % lbas_in_page || num_blocks % lbas_in_page) {
		FTL_ERRLOG(dev, "Invalid trim log content\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	for (uint64_t i = first_page; i < first_page + num_pages; ++i) {
		if (page[i] > seq_id) {
			FTL_ERRLOG(dev, "Invalid trim metadata content\n");
			ftl_mngt_fail_step(mngt);
			return;
		}
		page[i] = seq_id;
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_recover_trim_log - TRIM_LOG region 디스크에서 read.
 */
static void
ftl_mngt_recover_trim_log(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_LOG];

	md->owner.cb_ctx = mngt;
	md->cb = ftl_mngt_recover_trim_log_cb;
	ftl_md_restore(md);
}

/*
 * [한국어]
 * ftl_mngt_recover_trim_persist - 보류 trim이 있으면 TRIM_LOG 디스크에 write.
 */
static void
ftl_mngt_recover_trim_persist(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_LOG];

	if (!trim_pending(dev)) {
		/* No pending trim logged */
		ftl_mngt_skip_step(mngt);
		return;
	}

	md->owner.cb_ctx = mngt;
	md->cb = ftl_mngt_recover_trim_cb;
	ftl_md_persist(md);
}

/*
 * [한국어]
 * ftl_mngt_recover_trim_log_clear - 보류 trim 처리 끝나면 LOG 헤더를 0으로 클리어.
 */
static void
ftl_mngt_recover_trim_log_clear(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_LOG];
	struct ftl_trim_log *log = ftl_md_get_buffer(md);

	if (!trim_pending(dev)) {
		/* No pending trim logged */
		ftl_mngt_skip_step(mngt);
		return;
	}

	memset(&log->hdr, 0, sizeof(log->hdr));
	md->owner.cb_ctx = mngt;
	md->cb = ftl_mngt_recover_trim_cb;
	ftl_md_persist(md);
}

/*
 * [한국어]
 * g_desc_trim_recovery - 일반 dirty 복구 시 trim 복원 sub-process.
 *
 * 순서: TRIM_MD read → TRIM_LOG read+적용 → TRIM_MD write → TRIM_LOG clear.
 */
static const struct ftl_mngt_process_desc g_desc_trim_recovery = {
	.name = "FTL trim recovery ",
	.steps = {
		{
			.name = "Recover trim metadata",
			.action = ftl_mngt_recover_trim_md,
		},
		{
			.name = "Recover trim log",
			.action = ftl_mngt_recover_trim_log,
		},
		{
			.name = "Persist trim metadata",
			.action = ftl_mngt_recover_trim_md_persist,
		},
		{
			.name = "Clear trim log",
			.action = ftl_mngt_recover_trim_log_clear,
		},
		{}
	}
};

/*
 * [한국어]
 * g_desc_trim_shm_recovery - SHM 복구 시 trim 복원 sub-process.
 *
 * SHM에는 메모리 trim_map이 이미 살아 있으므로 read 단계 생략, complete_trim으로
 * 메모리 trim_map만 갱신 후 LOG/MD persist.
 */
static const struct ftl_mngt_process_desc g_desc_trim_shm_recovery = {
	.name = "FTL trim shared memory recovery ",
	.steps = {
		{
			.name = "Complete trim transaction",
			.action = ftl_mngt_complete_trim,
		},
		{
			.name = "Persist trim log",
			.action = ftl_mngt_recover_trim_persist,
		},
		{
			.name = "Persist trim metadata",
			.action = ftl_mngt_recover_trim_md_persist,
		},
		{
			.name = "Clear trim log",
			.action = ftl_mngt_recover_trim_log_clear,
		},
		{}
	}
};

/*
 * [한국어]
 * ftl_mngt_recover_trim - trim 복원 step. fast_recovery면 skip, 아니면 g_desc_trim_recovery.
 */
static void
ftl_mngt_recover_trim(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_fast_recovery(dev)) {
		ftl_mngt_skip_step(mngt);
		return;
	}

	ftl_mngt_call_process(mngt, &g_desc_trim_recovery, NULL);
}

/*
 * [한국어]
 * ftl_mngt_recover_trim_shm - SHM 복구 시 trim 복원 step.
 */
static void
ftl_mngt_recover_trim_shm(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process(mngt, &g_desc_trim_shm_recovery, NULL);
}

/*
 * [한국어]
 * ftl_mngt_recovery_shm_l2p - L2P를 SHM에서 복구할지 판단하는 step.
 *
 * fast_recovery면 g_desc_recovery_shm sub-process 호출, 아니면 skip (이미 일반 iteration이
 * L2P를 디스크에 save해둠).
 */
static void
ftl_mngt_recovery_shm_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_fast_recovery(dev)) {
		ftl_mngt_call_process(mngt, &g_desc_recovery_shm, NULL);
	} else {
		ftl_mngt_skip_step(mngt);
	}
}

/*
 * During dirty shutdown recovery, the whole L2P needs to be reconstructed. However,
 * recreating it all at the same time may take up to much DRAM, so it's done in multiple
 * iterations. This process describes the recovery of a part of L2P in one iteration.
 */
/*
 * [한국어]
 * g_desc_recovery_iteration - 한 LBA range를 처리하는 iteration sub-process.
 *
 * 각 iteration의 6단계:
 *  1) Load L2P  - 디스크 L2P region에서 snippet 영역 read.
 *  2) Initialize sequence IDs - trim_map의 page seq_id로 snippet seq_id 초기화.
 *  3) Restore chunk L2P - 모든 NV cache chunk P2L을 snippet에 적용.
 *  4) Restore band L2P  - 모든 base 밴드 tail md를 high QD로 read해서 snippet에 적용.
 *  5) Restore valid map - snippet 매핑을 valid_map과 band->num_valid에 반영.
 *  6) Save L2P - 갱신된 snippet을 디스크에 write back.
 */
static const struct ftl_mngt_process_desc g_desc_recovery_iteration = {
	.name = "FTL recovery iteration",
	.steps = {
		{
			.name = "Load L2P",
			.action = ftl_mngt_recovery_iteration_load_l2p,
		},
		{
			.name = "Initialize sequence IDs",
			.action = ftl_mngt_recovery_iteration_init_seq_ids,
		},
		{
			.name = "Restore chunk L2P",
			.action = ftl_mngt_recovery_iteration_restore_chunk_l2p,
		},
		{
			.name = "Restore band L2P",
			.ctx_size = sizeof(struct band_md_ctx),
			.action = ftl_mngt_recovery_iteration_restore_band_l2p,
		},
		{
			.name = "Restore valid map",
			.action = ftl_mngt_recovery_iteration_restore_valid_map,
		},
		{
			.name = "Save L2P",
			.action = ftl_mngt_recovery_iteration_save_l2p,
		},
		{}
	}
};

/*
 * Loading of FTL after dirty shutdown. Recovers metadata, L2P, decides on amount of recovery
 * iterations to be executed (dependent on ratio of L2P cache size and total L2P size)
 */
/*
 * [한국어]
 * g_desc_recovery - dirty shutdown 후 풀 복구 sub-process.
 *
 * 매우 긴 step 시퀀스 — 본 파일 상단 "전체 아키텍처에서의 위치" 섹션의 복구 흐름 참조.
 */
static const struct ftl_mngt_process_desc g_desc_recovery = {
	.name = "FTL recovery",
	.ctx_size = sizeof(struct ftl_mngt_recovery_ctx),
	.steps = {
		{
			.name = "Initialize recovery",
			.action = ftl_mngt_recovery_init,
			.cleanup = ftl_mngt_recovery_deinit
		},
		{
			.name = "Recover band state",
			.action = ftl_mngt_recovery_restore_band_state,
		},
		{
			.name = "Initialize P2L checkpointing",
			.action = ftl_mngt_p2l_init_ckpt,
			.cleanup = ftl_mngt_p2l_deinit_ckpt
		},
		{
			.name = "Restore P2L checkpoints",
			.action = ftl_mngt_p2l_restore_ckpt
		},
		{
			.name = "Preprocess P2L checkpoints",
			.action = ftl_mngt_recovery_pre_process_p2l
		},
		{
			.name = "Recover open bands P2L",
			.action = ftl_mngt_recovery_open_bands_p2l
		},
		{
			.name = "Recover chunk state",
			.action = ftl_mngt_nv_cache_restore_chunk_state
		},
		{
			.name = "Recover max seq ID",
			.action = ftl_mngt_recover_seq_id
		},
		{
			.name = "Recover trim",
			.action = ftl_mngt_recover_trim
		},
		{
			.name = "Recover open chunks P2L",
			.action = ftl_mngt_nv_cache_recover_open_chunk
		},
		{
			.name = "Recovery iterations",
			.action = ftl_mngt_recovery_run_iteration,
			/* [한국어] continue_step으로 자기 자신을 반복 호출해 모든 LBA 범위 처리. */
		},
		{
			.name = "Deinitialize recovery",
			.action = ftl_mngt_recovery_deinit
		},
		{
			.name = "Initialize L2P",
			.action = ftl_mngt_init_l2p,
			.cleanup = ftl_mngt_deinit_l2p
		},
		{
			.name = "Recover L2P from shared memory",
			.action = ftl_mngt_recovery_shm_l2p,
		},
		{
			.name = "Finalize band initialization",
			.action = ftl_mngt_finalize_init_bands,
		},
		{
			.name = "Start core poller",
			.action = ftl_mngt_start_core_poller,
			.cleanup = ftl_mngt_stop_core_poller
		},
		{
			.name = "Self test on startup",
			.action = ftl_mngt_self_test
		},
		{
			.name = "Finalize initialization",
			.action = ftl_mngt_finalize_startup,
		},
		{}
	}
};

/*
 * Shared memory specific steps for dirty shutdown recovery - main task is rebuilding the state of
 * L2P cache (paged in/out status, dirtiness etc. of individual pages).
 */
/*
 * [한국어]
 * g_desc_recovery_shm - SHM 기반 빠른 복구 sub-process.
 *
 * SHM에 L2P cache 메타가 보존되어 있을 때 — 단순 restore + valid_map 카운트 재로드 +
 * trim 복원만 수행. iteration 불필요.
 */
static const struct ftl_mngt_process_desc g_desc_recovery_shm = {
	.name = "FTL recovery from SHM",
	.ctx_size = sizeof(struct ftl_mngt_recovery_ctx),
	.steps = {
		{
			.name = "Restore L2P from shared memory",
			.action = ftl_mngt_restore_l2p,
		},
		{
			.name = "Restore valid maps counters",
			.action = ftl_mngt_restore_valid_counters,
		},
		{
			.name = "Recover trim from shared memory",
			.action = ftl_mngt_recover_trim_shm,
		},
		{}
	}
};

/*
 * [한국어]
 * ftl_mngt_recover - 외부 step. g_desc_recovery sub-process 호출.
 *
 * 호출 체인:
 *   desc_restore → ftl_mngt_select_restore_mode (sb dirty) → ftl_mngt_recover →
 *     call_process(g_desc_recovery)
 */
void
ftl_mngt_recover(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process(mngt, &g_desc_recovery, NULL);
}
