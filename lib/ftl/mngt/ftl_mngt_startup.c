/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 디바이스 startup 단계 파이프라인 및 trim 진입점 (ftl_mngt_startup.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 FTL 디바이스 부팅(startup)에 필요한 4가지 step 파이프라인을 정의하고
 * 외부 진입점(ftl_mngt_call_dev_startup, ftl_mngt_trim, ftl_mngt_rollback_device)을
 * 노출한다. 부팅은 다음 분기 트리로 진행된다:
 *
 *   ftl_mngt_call_dev_startup
 *     └── desc_startup (모든 부팅 공통: bdev open, layout, MD region 셋업…)
 *           └── 마지막 step "Select startup mode" → 분기:
 *                  ├── (CREATE 모드)   desc_first_start (첫 포맷 — L2P clear, P2L wipe…)
 *                  └── (LOAD 모드)     desc_restore
 *                                        └── "Select recovery mode" → 분기:
 *                                              ├── (clean SB)  desc_clean_start
 *                                              │                (디스크 MD 그대로 픽업)
 *                                              └── (dirty SB)  ftl_mngt_recover()
 *                                                              (P2L 재생 풀 복구 — 별도 파일)
 *
 * 또한 RPC trim 진입점(ftl_mngt_trim)과 rollback 진입점도 본 파일에서 정의한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 컨텍스트: ftl_mngt_call_dev_startup은 spdk_ftl_dev_init(외부 RPC 또는 bdev_ftl
 * create) 경로에서 호출. 이후 모든 step은 코어 spdk_thread에서 비동기 진행.
 * trim은 RPC 또는 bdev API에서 LBA 범위 unmap 요청 시 호출.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: ftl_mngt_steps.h의 거의 모든 step 함수, ftl_core.h(spdk_ftl_dev), ftl_mngt.h(API).
 * - 의존받음: lib/ftl/ftl_core.c (spdk_ftl_dev_init, RPC 핸들러).
 * - 데이터 흐름: dev 구조체가 단계별로 채워지며 마지막에 RUNNING 상태가 됨.
 *
 * === 주요 함수/구조체 요약 ===
 * - desc_startup        : 모든 부팅 공통 step (bdev/layout/MD/IO ch).
 * - desc_first_start    : 첫 포맷 step (L2P clear, P2L wipe, set_dirty, finalize).
 * - desc_restore        : "복구 모드 선택" 한 step만 가지는 분기점 wrapper.
 * - desc_clean_start    : clean SB 부팅 step (디스크 MD 그대로 픽업).
 * - ftl_mngt_select_startup_mode() : CREATE vs LOAD 분기.
 * - ftl_mngt_select_restore_mode() : clean vs dirty 분기.
 * - ftl_mngt_call_dev_startup()    : 외부 startup 진입점.
 * - struct ftl_trim_ctx, ftl_mngt_trim() : RPC trim 진입점.
 * - g_desc_trim                     : trim sub-process desc.
 * - ftl_mngt_rollback_device()      : startup의 cleanup만 역순 실행.
 */

#include "ftl_core.h"
/* [한국어] spdk_ftl_dev 정의 (dev->conf, dev->sb, dev->ioch 등). */
#include "ftl_mngt.h"
/* [한국어] ftl_mngt_process 및 process_execute / call_process / call_process_rollback API. */
#include "ftl_mngt_steps.h"
/* [한국어] 모든 step 함수 프로토타입 — desc 배열에 등록. */

static const struct ftl_mngt_process_desc desc_startup;
static const struct ftl_mngt_process_desc desc_first_start;
static const struct ftl_mngt_process_desc desc_restore;
static const struct ftl_mngt_process_desc desc_clean_start;
/* [한국어] 4개 desc의 forward 선언 — select 함수가 정의 전에 참조하므로 필요. */

/*
 * [한국어]
 * ftl_mngt_select_startup_mode - desc_startup 마지막 step. CREATE / LOAD 분기.
 *
 * @dev:  FTL 디바이스. dev->conf.mode의 SPDK_FTL_MODE_CREATE 비트로 분기.
 * @mngt: 현재 desc_startup의 mngt 핸들.
 *
 * 동작: CREATE면 desc_first_start sub-process 호출, 아니면 desc_restore.
 *
 * 호출 체인: desc_startup의 마지막 "Select startup mode" step → 본 함수 →
 *           ftl_mngt_call_process(child) → child 끝나면 desc_startup 종료.
 */
static void
ftl_mngt_select_startup_mode(struct spdk_ftl_dev *dev,
			     struct ftl_mngt_process *mngt)
{
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		/* [한국어] 첫 포맷 모드 (RPC bdev_ftl_create --mode 또는 conf.mode 비트). */
		ftl_mngt_call_process(mngt, &desc_first_start, NULL);
	} else {
		/* [한국어] 기존 디바이스 로드 모드. */
		ftl_mngt_call_process(mngt, &desc_restore, NULL);
	}
}

/*
 * [한국어]
 * ftl_mngt_select_restore_mode - desc_restore의 유일한 step. clean / dirty 분기.
 *
 * @dev:  FTL 디바이스. dev->sb->clean(슈퍼블록 clean 비트)으로 분기.
 * @mngt: desc_restore 핸들.
 *
 * 동작:
 *  - clean: desc_clean_start sub-process로 빠른 부팅(디스크 MD 그대로 픽업).
 *  - dirty: ftl_mngt_recover 호출(별도 파일 — P2L log replay 풀 복구).
 *           recover는 sub-process가 아니라 desc_restore의 step으로 직접 처리.
 */
static void
ftl_mngt_select_restore_mode(struct spdk_ftl_dev *dev,
			     struct ftl_mngt_process *mngt)
{
	if (dev->sb->clean) {
		/* [한국어] 정상 셧다운 — fast path. */
		ftl_mngt_call_process(mngt, &desc_clean_start, NULL);
	} else {
		/* [한국어] dirty shutdown — 풀 recovery 필요. */
		ftl_mngt_recover(dev, mngt);
	}
}

/*
 * Common startup steps required by FTL in all cases (creation, load, dirty shutdown recovery).
 * Includes actions like opening the devices, calculating the expected size and version of metadata, etc.
 */
/*
 * [한국어]
 * desc_startup - 모든 부팅 경로 공통 step 파이프라인.
 *
 * cleanup이 함께 등록된 step은 도중 실패 시 그 step의 자원을 정리.
 * 마지막 step "Select startup mode"가 CREATE/LOAD 분기를 트리거.
 */
static const struct ftl_mngt_process_desc desc_startup = {
	.name = "FTL startup",
	.steps = {
		{
			.name = "Check configuration",
			/* [한국어] dev->conf 유효성(bdev 이름, 캐시 크기 등) 검증. */
			.action = ftl_mngt_check_conf,
		},
		{
			.name = "Open base bdev",
			/* [한국어] 데이터용 NVMe bdev 열기 + claim + 검증. */
			.action = ftl_mngt_open_base_bdev,
			.cleanup = ftl_mngt_close_base_bdev
		},
		{
			.name = "Open cache bdev",
			/* [한국어] NV cache bdev 열기. */
			.action = ftl_mngt_open_cache_bdev,
			.cleanup = ftl_mngt_close_cache_bdev
		},
		{
			.name = "Initialize superblock",
			/* [한국어] 슈퍼블록 메모리 핸들 + 디스크 read 준비. */
			.action = ftl_mngt_superblock_init,
			.cleanup = ftl_mngt_superblock_deinit
		},
		{
			.name = "Initialize memory pools",
			/* [한국어] DPDK hugepage 기반 mempool들(io, p2l 등) 생성. */
			.action = ftl_mngt_init_mem_pools,
			.cleanup = ftl_mngt_deinit_mem_pools
		},
		{
			.name = "Initialize bands",
			/* [한국어] dev->bands 배열 calloc + free/shut 큐 초기화. */
			.action = ftl_mngt_init_bands,
			.cleanup = ftl_mngt_deinit_bands
		},
		{
			.name = "Register IO device",
			/* [한국어] dev를 SPDK io_device로 등록 — 다른 thread가 채널 받을 수 있음. */
			.action = ftl_mngt_register_io_device,
			.cleanup = ftl_mngt_unregister_io_device
		},
		{
			.name = "Initialize core IO channel",
			/* [한국어] 코어 스레드용 ftl_io_channel 획득. */
			.action = ftl_mngt_init_io_channel,
			.cleanup = ftl_mngt_deinit_io_channel
		},
		{
			.name = "Decorate bands",
			/* [한국어] 큰 SSD에서 logical band 그룹화 → physical band ID 부여. */
			.action = ftl_mngt_decorate_bands
		},
		{
			.name = "Initialize layout",
			/* [한국어] dev->layout(밴드/MD region 위치) 계산. */
			.action = ftl_mngt_init_layout
		},
		{
			.name = "Verify layout",
			/* [한국어] 디스크 layout이 슈퍼블록과 일치하는지 검증. */
			.action = ftl_mngt_layout_verify,
		},
		{
			.name = "Upgrade layout",
			/* [한국어] 구버전 layout이면 region 단위 업그레이드 sub-process 호출. */
			.action = ftl_mngt_layout_upgrade,
		},
		{
			.name = "Scrub NV cache",
			/* [한국어] NV cache 영역 0 fill (CREATE 시 잔여 데이터 지움). */
			.action = ftl_mngt_scrub_nv_cache,
		},
		{
			.name = "Initialize metadata",
			/* [한국어] 모든 MD region 핸들(ftl_md *) 메모리 생성. */
			.action = ftl_mngt_init_md,
			.cleanup = ftl_mngt_deinit_md
		},
		{
			.name = "Initialize band addresses",
			/* [한국어] 각 밴드의 base bdev 시작 PBA + tail md PBA 계산. */
			.action = ftl_mngt_initialize_band_address
		},
		{
			.name = "Initialize NV cache",
			/* [한국어] NV cache 모듈(chunk allocator, write buf 등) 초기화. */
			.action = ftl_mngt_init_nv_cache,
			.cleanup = ftl_mngt_deinit_nv_cache
		},
		{
			.name = "Initialize valid map",
			/* [한국어] 전역 valid bitmap 메모리 할당. */
			.action = ftl_mngt_init_vld_map,
			.cleanup = ftl_mngt_deinit_vld_map
		},
		{
			.name = "Initialize trim map",
			/* [한국어] trim 누적용 비트맵 할당. */
			.action = ftl_mngt_init_trim_map,
			.cleanup = ftl_mngt_deinit_trim_map
		},
		{
			.name = "Initialize bands metadata",
			/* [한국어] 각 밴드의 md 페이지 + valid bitmap 슬라이스 매핑. */
			.action = ftl_mngt_init_bands_md,
			.cleanup = ftl_mngt_deinit_bands_md
		},
		{
			.name = "Initialize reloc",
			/* [한국어] GC/relocation 모듈 초기화. */
			.action = ftl_mngt_init_reloc,
			.cleanup = ftl_mngt_deinit_reloc
		},
		{
			.name = "Select startup mode",
			/* [한국어] 분기 step — CREATE면 first_start, LOAD면 restore sub-process. */
			.action = ftl_mngt_select_startup_mode
		},
		{}
		/* [한국어] sentinel. */
	}
};

/*
 * Steps executed when creating FTL for the first time - most important being scrubbing
 * old data/metadata (so it's not leaked during dirty shutdown recovery) and laying out
 * regions for the new metadata (initializing band states, etc).
 */
/*
 * [한국어]
 * desc_first_start - SPDK_FTL_MODE_CREATE 부팅 step.
 *
 * 첫 포맷이라 모든 메타를 0/INVALID로 초기화 + 디스크 영속화. 마지막에 set_dirty로
 * 슈퍼블록 dirty 비트를 셋하고(write 시작 표시) finalize_startup으로 RUNNING 상태 진입.
 */
static const struct ftl_mngt_process_desc desc_first_start = {
	.name = "FTL first start",
	.steps = {
		{
			.name = "Initialize L2P",
			.action = ftl_mngt_init_l2p,
			.cleanup = ftl_mngt_deinit_l2p
		},
		{
			.name = "Clear L2P",
			/* [한국어] 모든 LBA 매핑을 unmapped로 set (잔여 데이터 leak 방지). */
			.action = ftl_mngt_clear_l2p,
		},
		{
			.name = "Finalize band initialization",
			/* [한국어] band를 free/shut 큐에 적절히 분배. */
			.action = ftl_mngt_finalize_init_bands,
		},
		{
			.name = "Save initial band info metadata",
			/* [한국어] 깨끗한 band MD를 디스크에 영속화. */
			.action = ftl_mngt_persist_band_info_metadata,
		},
		{
			.name = "Save initial chunk info metadata",
			/* [한국어] NV cache chunk MD 영속화. */
			.action = ftl_mngt_persist_nv_cache_metadata,
		},
		{
			.name = "Initialize P2L checkpointing",
			.action = ftl_mngt_p2l_init_ckpt,
			.cleanup = ftl_mngt_p2l_deinit_ckpt
		},
		{
			.name = "Wipe P2L region",
			/* [한국어] P2L checkpoint MD region 0으로 클리어 (이전 데이터 leak 방지). */
			.action = ftl_mngt_p2l_wipe,
		},
		{
			.name = "Wipe P2L Log IO region",
			/* [한국어] P2L IO log MD region 클리어 (옵션 활성 시만). */
			.action = ftl_mngt_p2l_log_io_wipe,
		},
		{
			.name = "Clear trim map",
			/* [한국어] trim_map MD region 클리어. */
			.action = ftl_mngt_trim_metadata_clear,
		},
		{
			.name = "Clear trim log",
			/* [한국어] trim log MD region 클리어. */
			.action = ftl_mngt_trim_log_clear,
		},
		{
			.name = "Set FTL dirty state",
			/* [한국어] 슈퍼블록 dirty 비트 set + 디스크 flush — 이 시점부터 어떤
			 * 비정상 종료가 있으면 다음 부팅에 recovery 진입. */
			.action = ftl_mngt_set_dirty,
		},
		{
			.name = "Start core poller",
			/* [한국어] dev->core_poller 등록 — GC/wbuf flush 등 백그라운드 잡 시작. */
			.action = ftl_mngt_start_core_poller,
			.cleanup = ftl_mngt_stop_core_poller
		},
		{
			.name = "Finalize initialization",
			/* [한국어] 디바이스 상태 RUNNING 전환 — 호스트 I/O 받기 시작. */
			.action = ftl_mngt_finalize_startup,
		},
		{}
	}
};

/*
 * Step utilized on loading of an FTL instance - decides on dirty/clean shutdown path.
 */
/*
 * [한국어]
 * desc_restore - 단 한 step "Select recovery mode"를 갖는 분기 wrapper.
 *
 * 본 desc는 분기 결정만 하고 실제 작업은 child desc 또는 ftl_mngt_recover에 위임.
 * 두 child가 자체 step에서 finalize_startup까지 처리.
 */
static const struct ftl_mngt_process_desc desc_restore = {
	.name = "FTL restore",
	.steps = {
		{
			.name = "Select recovery mode",
			.action = ftl_mngt_select_restore_mode,
		},
		{}
	}
};

/*
 * Loading of FTL after clean shutdown.
 */
/*
 * [한국어]
 * desc_clean_start - clean shutdown 후 부팅 step (가장 빠른 경로).
 *
 * 디스크 MD를 그대로 메모리로 read하면 끝. recovery 같은 P2L log replay 없음.
 * 마지막에 set_dirty + finalize.
 */
static const struct ftl_mngt_process_desc desc_clean_start = {
	.name = "Clean startup",
	.steps = {
		{
			.name = "Restore metadata",
			/* [한국어] 모든 MD region을 디스크에서 메모리로 read. */
			.action = ftl_mngt_restore_md
		},
		{
			.name = "Initialize P2L checkpointing",
			.action = ftl_mngt_p2l_init_ckpt,
			.cleanup = ftl_mngt_p2l_deinit_ckpt
		},
		{
			.name = "Restore P2L checkpoints",
			/* [한국어] 모든 P2L ckpt region 디스크에서 메모리로 병렬 read. */
			.action = ftl_mngt_p2l_restore_ckpt
		},
		{
			.name = "Initialize L2P",
			.action = ftl_mngt_init_l2p,
			.cleanup = ftl_mngt_deinit_l2p
		},
		{
			.name = "Restore L2P",
			/* [한국어] 디스크 L2P MD region에서 메모리 L2P로 read. */
			.action = ftl_mngt_restore_l2p,
		},
		{
			.name = "Finalize band initialization",
			/* [한국어] OPEN/FULL band를 writer에 재바인딩. */
			.action = ftl_mngt_finalize_init_bands,
		},
		{
			.name = "Start core poller",
			.action = ftl_mngt_start_core_poller,
			.cleanup = ftl_mngt_stop_core_poller
		},
		{
			.name = "Self test on startup",
			/* [한국어] FTL_SELF_TEST 환경변수 있으면 L2P/valid_map 일관성 검증. */
			.action = ftl_mngt_self_test,
		},
		{
			.name = "Set FTL dirty state",
			/* [한국어] write 시작 표시. */
			.action = ftl_mngt_set_dirty,
		},
		{
			.name = "Finalize initialization",
			.action = ftl_mngt_finalize_startup,
		},
		{}
	}
};

/*
 * [한국어]
 * ftl_mngt_call_dev_startup - 외부 startup 진입점.
 *
 * @dev: FTL 디바이스. @cb: 완료 콜백. @cb_cntx: 콜백 인자.
 *
 * 그저 desc_startup으로 process_execute 호출. 모든 분기는 desc_startup 안에서 처리.
 */
int
ftl_mngt_call_dev_startup(struct spdk_ftl_dev *dev, ftl_mngt_completion cb, void *cb_cntx)
{
	return ftl_mngt_process_execute(dev, &desc_startup, cb, cb_cntx);
}

/*
 * [한국어]
 * struct ftl_trim_ctx - RPC trim 호출의 caller context.
 *
 * 호출자(임의 thread)가 trim 결과를 받기 위해 본 ctx에 cb/lba 보관. 코어 스레드에서
 * trim 완료 후 ctx->thread로 메시지를 보내 콜백 실행.
 */
struct ftl_trim_ctx {
	uint64_t lba;
	/* [한국어] trim 시작 LBA. */
	uint64_t num_blocks;
	/* [한국어] trim할 블록 수. */
	spdk_ftl_fn cb_fn;
	/* [한국어] 호출자 콜백 — (void *cb_arg, int status). */
	void *cb_arg;
	/* [한국어] 콜백에 그대로 전달될 인자. */
	struct spdk_thread *thread;
	/* [한국어] 호출자 thread — 콜백을 이 thread에서 실행하도록 message dispatch. */
	int status;
	/* [한국어] trim 결과 (0/-errno). */
};

/*
 * [한국어]
 * ftl_mngt_process_trim_cb - spdk_ftl_unmap 비동기 완료 콜백 (코어 스레드).
 *
 * @ctx: ftl_mngt_process 핸들 (mngt). @status: trim 결과.
 */
static void
ftl_mngt_process_trim_cb(void *ctx, int status)
{
	struct ftl_mngt_process *mngt = ctx;

	if (status) {
		ftl_mngt_fail_step(ctx);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_process_trim - g_desc_trim의 단일 step. spdk_ftl_unmap 발행.
 *
 * @dev:  FTL 디바이스.
 * @mngt: trim sub-process mngt 핸들.
 *
 * 동작:
 *  1) process_ctx에 ftl_io 메모리(g_desc_trim.ctx_size)가 자동 할당되어 있음.
 *  2) caller_ctx로부터 ftl_trim_ctx(lba, num_blocks) 획득.
 *  3) dev->ioch 없으면 fail (코어 채널 미준비).
 *  4) spdk_ftl_unmap 호출 — 비동기, 완료는 process_trim_cb로.
 *  5) -EAGAIN(자원 일시 부족)이면 continue_step으로 본 함수 재호출 → 자동 재시도.
 */
static void
ftl_mngt_process_trim(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_io *io = ftl_mngt_get_process_ctx(mngt);
	/* [한국어] g_desc_trim.ctx_size = sizeof(ftl_io)로 자동 할당된 io 객체. */
	struct ftl_trim_ctx *ctx = ftl_mngt_get_caller_ctx(mngt);
	/* [한국어] ftl_mngt_trim에서 init_ctx로 넘긴 trim 정보. */
	int rc;

	if (!dev->ioch) {
		/* [한국어] 코어 ioch가 아직 없거나 셧다운 중 — 더 진행 못함. */
		ftl_mngt_fail_step(mngt);
		return;
	}

	rc = spdk_ftl_unmap(dev, io, dev->ioch, ctx->lba, ctx->num_blocks, ftl_mngt_process_trim_cb, mngt);
	/* [한국어] FTL unmap 발행. 동기 성공 = 0(콜백 호출됨), 비동기 in-flight도 0,
	 * -EAGAIN = mempool 부족 등 일시 실패. */
	if (rc == -EAGAIN) {
		/* [한국어] 다음 reactor tick에서 다시 시도. */
		ftl_mngt_continue_step(mngt);
	}
}

/*
 * RPC trim path.
 */
/*
 * [한국어] g_desc_trim - trim sub-process desc. ctx_size로 ftl_io 자동 확보.
 */
static const struct ftl_mngt_process_desc g_desc_trim = {
	.name = "FTL trim",
	.ctx_size = sizeof(struct ftl_io),
	/* [한국어] process_ctx로 ftl_io 객체 1개를 자동 할당받음 — process_trim에서 사용. */
	.steps = {
		{
			.name = "Process trim",
			.action = ftl_mngt_process_trim,
		},
		{}
	}
};

/*
 * [한국어]
 * trim_user_cb - 호출자 thread에서 실행되는 사용자 콜백 wrapper.
 *
 * @_ctx: ftl_trim_ctx.
 *
 * 동작: 호출자가 등록한 cb_fn(cb_arg, status)을 호출 후 ctx free.
 * 코어 스레드에서 ftl_mngt_trim_cb가 send_msg로 본 함수를 호출자 thread에 dispatch.
 */
static void
trim_user_cb(void *_ctx)
{
	struct ftl_trim_ctx *ctx = _ctx;

	ctx->cb_fn(ctx->cb_arg, ctx->status);
	free(ctx);
}

/*
 * [한국어]
 * ftl_mngt_trim_cb - g_desc_trim 완료 콜백 (코어 스레드).
 *
 * @dev: FTL 디바이스. @_ctx: ftl_trim_ctx. @status: 결과.
 *
 * 동작: status를 ctx에 저장 후 호출자 thread로 trim_user_cb 메시지 전송.
 * 콜백을 호출자 thread에서 실행해야 호출자의 락/자료구조 일관성 유지.
 */
static void
ftl_mngt_trim_cb(struct spdk_ftl_dev *dev, void *_ctx, int status)
{
	struct ftl_trim_ctx *ctx = _ctx;
	ctx->status = status;

	spdk_thread_send_msg(ctx->thread, trim_user_cb, ctx);
	/* [한국어] 호출자 thread에서 cb 실행 — cross-thread 안전성 보장. */
}

/*
 * [한국어]
 * ftl_mngt_trim - 외부 trim 진입점 (RPC 또는 bdev API에서 호출).
 *
 * @dev:        FTL 디바이스.
 * @lba:        trim 시작 LBA.
 * @num_blocks: trim할 블록 수.
 * @cb:         완료 콜백.
 * @cb_cntx:    콜백 인자.
 * @return:     0=process 시작 성공(완료는 비동기 cb), -EAGAIN=ctx 할당 실패.
 *
 * 동작:
 *  1) ftl_trim_ctx calloc.
 *  2) 인자 보관 + spdk_get_thread()로 호출자 thread 기록.
 *  3) g_desc_trim sub-process 실행 — 완료 시 ftl_mngt_trim_cb → trim_user_cb → cb.
 *
 * 실행 컨텍스트: 호출자 thread (보통 RPC handler의 thread). 본 함수는 즉시 return,
 * 실제 trim은 코어 스레드에서 비동기 진행.
 */
int
ftl_mngt_trim(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t num_blocks, spdk_ftl_fn cb,
	      void *cb_cntx)
{
	struct ftl_trim_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -EAGAIN;
	}

	ctx->lba = lba;
	ctx->num_blocks = num_blocks;
	ctx->cb_fn = cb;
	ctx->cb_arg = cb_cntx;
	ctx->thread = spdk_get_thread();
	/* [한국어] 호출자 thread 기록 — 완료 콜백을 이 thread에서 실행. */

	return ftl_mngt_process_execute(dev, &g_desc_trim, ftl_mngt_trim_cb, ctx);
}

/*
 * [한국어]
 * ftl_mngt_rollback_device - desc_startup의 cleanup만 역순 실행 (자원 해제 전용).
 *
 * @dev:  FTL 디바이스.
 * @mngt: 호출자 mngt 핸들 (보통 shutdown desc).
 *
 * 왜 필요한가: shutdown 마지막 단계에서 startup이 만든 모든 자원(bdev/mempool/MD/
 * layout/...)을 안전하게 역순 해제. desc_startup의 모든 step에 cleanup이 정의되어
 * 있으므로 process_rollback 호출만으로 모든 cleanup이 역순 실행됨.
 */
void
ftl_mngt_rollback_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process_rollback(mngt, &desc_startup);
}
