/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 잡다 기능 step (mempool, reloc, NV cache, valid/trim map, properties)
 *               (ftl_mngt_misc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 단일 책임 모듈로 묶이지 않는 잡다한 step 함수들과 RPC property
 * get/set 진입점을 모아둔 곳이다. 다음을 포함한다:
 *  - check_conf (사용자 conf 검증)
 *  - mempool 초기화/해제 (p2l_pool, band_md_pool — DPDK 기반 lockless pool)
 *  - reloc(GC) 모듈 init/deinit
 *  - NV cache 모듈 init/deinit + 첫 포맷 시 scrub
 *  - finalize_startup (디바이스 RUNNING 상태 진입)
 *  - core poller start/stop
 *  - dump_stats (RPC bdev_ftl_get_stats가 사용)
 *  - valid_map / trim_map 초기화/해제 + clear
 *  - RPC ftl_get_properties / ftl_set_property 핸들러 (ftl_property 모듈 위에)
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 컨텍스트: 거의 모든 step은 ftl_mngt 파이프라인에서 코어 스레드 호출.
 * RPC 진입점(spdk_ftl_get_properties / spdk_ftl_set_property)은 임의 thread에서
 * 호출되어 코어 스레드로 메시지/process 디스패치.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: lib/ftl/utils/ftl_mempool.[ch], ftl_md.[ch], ftl_property.[ch], ftl_bitmap.[ch],
 *   lib/ftl/ftl_reloc.[ch], ftl_nv_cache.[ch], ftl_l2p.[ch], ftl_writer.[ch], ftl_debug.[ch],
 *   lib/ftl/ftl_conf.[ch], ftl_core.h.
 * - 의존받음: ftl_mngt_startup.c, ftl_mngt_shutdown.c, ftl_mngt_recovery.c.
 *
 * === 주요 함수/구조체 요약 ===
 * - ftl_mngt_check_conf()             : conf 유효성 검증.
 * - init_p2l_map_pool()/init_band_md_pool() : mempool 생성 헬퍼.
 * - ftl_mngt_init/deinit_mem_pools()  : 위 두 풀의 step 어댑터.
 * - ftl_mngt_init/deinit_reloc()      : GC/relocation 모듈 init/free.
 * - ftl_mngt_init/deinit_nv_cache()   : NV cache 모듈 init/free.
 * - user_clear_cb / ftl_mngt_scrub_nv_cache() : 첫 포맷/major upgrade 시 NV cache 0 fill.
 * - ftl_mngt_finalize_startup()       : 디바이스 RUNNING 진입 + 모든 모듈 resume.
 * - ftl_mngt_start/stop_core_poller() : core_poller 등록/해제.
 * - ftl_mngt_dump_stats()             : 운영 통계 로그 출력.
 * - ftl_mngt_init/deinit_vld_map()    : 전역 valid bitmap 핸들 생성/해제.
 * - ftl_mngt_init/deinit_trim_map()   : trim_bitmap MD 생성/해제.
 * - trim_clear_cb + trim_metadata/log_clear() : trim MD region 0 클리어.
 * - struct ftl_mngt_property_caller_ctx + ftl_get/set_properties* :
 *   RPC property API 구현.
 * - desc_set_property : property 설정 sub-process desc.
 */

#include "ftl_core.h"
/* [한국어] spdk_ftl_dev 정의. */
#include "ftl_utils.h"
/* [한국어] ftl_md_create/destroy 등 유틸. */
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_band.h"
#include "ftl_internal.h"
#include "ftl_nv_cache.h"
#include "ftl_debug.h"
/* [한국어] ftl_dev_dump_bands / ftl_dev_dump_stats. */
#include "ftl_utils.h"
/* [한국어] 중복 include — 의존성 명시 강조용 (헤더 가드로 안전). */

/*
 * [한국어]
 * ftl_mngt_check_conf - dev->conf 유효성 검증 step.
 *
 * ftl_conf_is_valid가 false면 fail (필수 필드 누락 / 범위 위반).
 */
void
ftl_mngt_check_conf(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_conf_is_valid(&dev->conf)) {
		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

/*
 * [한국어]
 * init_p2l_map_pool - P2L map용 mempool을 SHM-aware MD region 위에 구축.
 *
 * @dev: FTL 디바이스. @return: 0/-ENOMEM.
 *
 * 동작:
 *  1) 한 element가 차지할 블록 수 = ceil(p2l_map_pool_elem_size / 4 KiB).
 *  2) 전체 풀의 블록 수 = P2L_MEMPOOL_SIZE * el_blks.
 *  3) ftl_md_create로 SHM 또는 일반 메모리에 백킹 buffer 확보.
 *  4) 그 buffer를 mempool로 wrap (ftl_mempool_create_ext).
 *  5) fast_startup이 아니면 mempool 인덱스 초기화 (fast면 SHM에서 이미 초기화됨).
 */
static int
init_p2l_map_pool(struct spdk_ftl_dev *dev)
{
	size_t p2l_pool_el_blks = spdk_divide_round_up(ftl_p2l_map_pool_elem_size(dev), FTL_BLOCK_SIZE);
	size_t p2l_pool_buf_blks = P2L_MEMPOOL_SIZE * p2l_pool_el_blks;
	void *p2l_pool_buf;

	dev->p2l_pool_md = ftl_md_create(dev, p2l_pool_buf_blks, 0, "p2l_pool",
					 ftl_md_create_shm_flags(dev), NULL);
	/* [한국어] SHM-aware MD region 생성 — fast restart 시 다음 프로세스가 그대로 픽업. */
	if (!dev->p2l_pool_md) {
		return -ENOMEM;
	}

	p2l_pool_buf = ftl_md_get_buffer(dev->p2l_pool_md);
	dev->p2l_pool = ftl_mempool_create_ext(p2l_pool_buf, P2L_MEMPOOL_SIZE,
					       p2l_pool_el_blks * FTL_BLOCK_SIZE,
					       FTL_BLOCK_SIZE);
	/* [한국어] 외부 buffer를 받는 mempool — DPDK rte_mempool과 유사하지만 외부 메모리 위에 구축. */
	if (!dev->p2l_pool) {
		return -ENOMEM;
	}

	if (!ftl_fast_startup(dev)) {
		/* [한국어] 일반 부팅 — mempool free list 인덱스를 새로 초기화. */
		ftl_mempool_initialize_ext(dev->p2l_pool);
	}

	return 0;
}

/*
 * [한국어]
 * init_band_md_pool - 임시 band metadata mempool 생성.
 *
 * 일반 hugepage 위에 calloc 같은 mempool. P2L map보다 작아서 SHM 없이 충분.
 */
static int
init_band_md_pool(struct spdk_ftl_dev *dev)
{
	dev->band_md_pool = ftl_mempool_create(P2L_MEMPOOL_SIZE,
					       sizeof(struct ftl_band_md),
					       FTL_BLOCK_SIZE,
					       SPDK_ENV_NUMA_ID_ANY);
	/* [한국어] count=P2L_MEMPOOL_SIZE, element=ftl_band_md, 4 KiB align, NUMA ANY. */
	if (!dev->band_md_pool) {
		return -ENOMEM;
	}

	return 0;
}

/*
 * [한국어]
 * ftl_mngt_init_mem_pools - p2l_pool과 band_md_pool 두 개를 step에서 초기화.
 */
void
ftl_mngt_init_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (init_p2l_map_pool(dev)) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (init_band_md_pool(dev)) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_deinit_mem_pools - 두 mempool과 p2l MD region 해제.
 */
void
ftl_mngt_deinit_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (dev->p2l_pool) {
		ftl_mempool_destroy_ext(dev->p2l_pool);
		dev->p2l_pool = NULL;
	}

	if (dev->p2l_pool_md) {
		ftl_md_destroy(dev->p2l_pool_md, ftl_md_destroy_shm_flags(dev));
		/* [한국어] SHM 모드면 munmap, 일반이면 spdk_dma_free. */
		dev->p2l_pool_md = NULL;
	}

	if (dev->band_md_pool) {
		ftl_mempool_destroy(dev->band_md_pool);
		dev->band_md_pool = NULL;
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_init_reloc - GC/relocation 모듈 초기화. dev->reloc에 저장.
 */
void
ftl_mngt_init_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->reloc = ftl_reloc_init(dev);
	if (!dev->reloc) {
		FTL_ERRLOG(dev, "Unable to initialize reloc structures\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_deinit_reloc - reloc 모듈 해제.
 */
void
ftl_mngt_deinit_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_reloc_free(dev->reloc);
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_init_nv_cache - NV cache 모듈 초기화 (chunk 큐, write buffer 등).
 */
void
ftl_mngt_init_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_nv_cache_init(dev)) {
		FTL_ERRLOG(dev, "Unable to initialize persistent cache\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_deinit_nv_cache - NV cache 모듈 해제.
 */
void
ftl_mngt_deinit_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_nv_cache_deinit(dev);
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * user_clear_cb - NV cache scrub 완료 콜백.
 */
static void
user_clear_cb(struct spdk_ftl_dev *dev, void *cb_ctx, int status)
{
	struct ftl_mngt_process *mngt = cb_ctx;

	if (status) {
		FTL_ERRLOG(ftl_mngt_get_dev(mngt), "FTL NV Cache: ERROR of clearing user cache data\n");
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_scrub_nv_cache - 첫 포맷 또는 major upgrade 시 NV cache 사용자 데이터 영역을 0으로.
 *
 * 왜 필요한가: dirty shutdown 후 recovery는 chunk 안의 valid block을 seq_id 기반으로 인식.
 * 짧은 테스트에서 새 head MD의 seq_id가 이전 인스턴스의 VSS와 정렬되어 잘못된 데이터를
 * "유효"로 인식할 위험 있음. scrub으로 잔여 데이터를 명시적으로 지워 이런 ghost recovery 방지.
 */
void
ftl_mngt_scrub_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	bool is_first_start = (dev->conf.mode & SPDK_FTL_MODE_CREATE) != 0;
	/* [한국어] CREATE 모드 = 첫 포맷. */
	bool is_major_upgrade = dev->sb->clean == 1 && dev->sb_shm->shm_clean == 0 &&
				dev->sb->upgrade_ready == 1;
	/* [한국어] sb는 clean인데 SHM은 dirty + upgrade_ready 셋팅 = 메이저 업그레이드 신호. */

	if (is_first_start || is_major_upgrade) {
		FTL_NOTICELOG(dev, "NV cache data region needs scrubbing, this may take a while.\n");
		FTL_NOTICELOG(dev, "Scrubbing %"PRIu64" chunks\n", dev->layout.nvc.chunk_count);

		/* Need to scrub user data, so in case of dirty shutdown the recovery won't
		 * pull in data during open chunks recovery from any previous instance (since during short
		 * tests it's very likely that chunks seq_id will be in line between new head md and old VSS)
		 */
		ftl_nv_cache_scrub(dev, user_clear_cb, mngt);
		/* [한국어] 비동기 — 모든 chunk 사용자 데이터 영역에 0 fill 발행. */
	} else {
		/* [한국어] 일반 부팅 — scrub 불필요. 시간 절약 위해 skip. */
		ftl_mngt_skip_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_finalize_startup - 디바이스 RUNNING 상태 진입 + 모든 모듈 resume.
 *
 * 동작:
 *  1) trim_map에 set 비트가 있으면 trim_in_progress = true.
 *  2) superblock_version 프로퍼티 등록 (RPC dump용).
 *  3) stats.limits 0 클리어 (init 도중 잘못 증가된 것).
 *  4) initialized = 1 / sb_shm->shm_ready = true.
 *  5) L2P / reloc / writer_user / writer_gc / nv_cache 모두 resume — 호스트 I/O 시작.
 */
void
ftl_mngt_finalize_startup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_bitmap_find_first_set(dev->trim_map, 0, UINT64_MAX) != UINT64_MAX) {
		/* [한국어] trim_map에 보류 trim이 있음 — trim flush 필요 표시. */
		dev->trim_in_progress = true;
	}

	ftl_property_register(dev, "superblock_version", &dev->sb->header.version,
			      sizeof(dev->sb->header.version), NULL, NULL,
			      ftl_property_dump_uint64, NULL, NULL, false);

	/* Clear the limit applications as they're incremented incorrectly by
	 * the initialization code.
	 */
	memset(dev->stats.limits, 0, sizeof(dev->stats.limits));
	dev->initialized = 1;
	/* [한국어] 호스트 I/O를 받을 준비 완료 신호. spdk_ftl_writev/readv 등이 본 플래그 확인. */
	dev->sb_shm->shm_ready = true;
	/* [한국어] SHM 측에도 ready 마크 — 다른 프로세스가 SHM 픽업 가능 표시. */

	ftl_l2p_resume(dev);
	/* [한국어] L2P 리퍼/캐시 폴러 resume. */
	ftl_reloc_resume(dev->reloc);
	/* [한국어] GC 시작. */
	ftl_writer_resume(&dev->writer_user);
	/* [한국어] 사용자 writer 시작 — 호스트 write가 밴드에 기록 가능. */
	ftl_writer_resume(&dev->writer_gc);
	/* [한국어] GC writer 시작. */
	ftl_nv_cache_resume(&dev->nv_cache);
	/* [한국어] NV cache write 처리 시작. */

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_start_core_poller - dev->core_poller 등록.
 *
 * core_poller는 무한 루프에서 ftl_core_poller(dev)를 호출 — GC, write buffer flush,
 * P2L checkpoint, IO completion 등 모든 백그라운드 잡을 polling.
 */
void
ftl_mngt_start_core_poller(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->core_poller = SPDK_POLLER_REGISTER(ftl_core_poller, dev, 0);
	/* [한국어] 0us 주기 = 매 reactor tick마다 호출 (busy polling). */
	if (!dev->core_poller) {
		FTL_ERRLOG(dev, "Unable to register core poller\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_stop_core_poller - 코어 폴러 정지 step.
 *
 * dev->halt = true로 폴러에게 정지 신호. 폴러가 자기 안에서 스스로 unregister하면
 * dev->core_poller = NULL이 됨. 그 전까지는 continue_step으로 본 step 재호출하며 기다림.
 */
void
ftl_mngt_stop_core_poller(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->halt = true;
	/* [한국어] core_poller 안에서 검사하는 정지 신호 플래그. */

	if (dev->core_poller) {
		/* [한국어] 폴러가 아직 살아있음 — 다음 reactor tick에서 다시 검사. */
		ftl_mngt_continue_step(mngt);
	} else {
		/* [한국어] 폴러가 자체 unregister 완료 — 다음 step. */
		ftl_mngt_next_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_dump_stats - 운영 통계 로그 출력 step.
 */
void
ftl_mngt_dump_stats(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_dev_dump_bands(dev);
	/* [한국어] 모든 밴드 상태 요약 (state별 count 등). */
	ftl_dev_dump_stats(dev);
	/* [한국어] WAF, GC count, write/read bytes 등 운영 통계. */
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_init_vld_map - 전역 valid bitmap 핸들을 VALID_MAP MD region 위에 생성.
 */
void
ftl_mngt_init_vld_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *valid_map_md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_VALID_MAP];

	dev->valid_map = ftl_bitmap_create(ftl_md_get_buffer(valid_map_md),
					   ftl_md_get_buffer_size(valid_map_md));
	/* [한국어] VALID_MAP region의 인메모리 buffer 위에 비트맵 핸들 wrap.
	 * 전체 PBA(base + nvc) 1비트씩 = valid 여부. */
	if (!dev->valid_map) {
		FTL_ERRLOG(dev, "Failed to create valid map\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_deinit_vld_map - valid bitmap 핸들 해제. buffer는 MD region이 보관.
 */
void
ftl_mngt_deinit_vld_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (dev->valid_map) {
		ftl_bitmap_destroy(dev->valid_map);
		dev->valid_map = NULL;
	}

	ftl_mngt_next_step(mngt);
}
/*
 * [한국어]
 * ftl_mngt_init_trim_map - trim_bitmap MD region 생성 + 핸들 wrap.
 *
 * trim_map은 L2P page 단위로 trim 보류 상태를 추적 — 1비트가 한 L2P page를 의미.
 */
void
ftl_mngt_init_trim_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	uint64_t num_l2p_pages = spdk_divide_round_up(dev->num_lbas, dev->layout.l2p.lbas_in_page);
	/* [한국어] L2P page 수 = 전체 LBA / page당 LBA 수. */
	uint64_t map_blocks = ftl_bitmap_bits_to_blocks(num_l2p_pages);
	/* [한국어] num_l2p_pages 비트를 담을 4 KiB 블록 수. */

	dev->trim_map_md = ftl_md_create(dev,
					 map_blocks,
					 0,
					 "trim_bitmap",
					 ftl_md_create_shm_flags(dev), NULL);
	/* [한국어] SHM-aware MD region 생성 (셧다운 시 SHM에 보존 가능). */

	if (!dev->trim_map_md) {
		FTL_ERRLOG(dev, "Failed to create trim bitmap md\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	dev->trim_map = ftl_bitmap_create(ftl_md_get_buffer(dev->trim_map_md),
					  ftl_md_get_buffer_size(dev->trim_map_md));
	/* [한국어] 그 buffer 위에 비트맵 핸들 wrap. */

	if (!dev->trim_map) {
		FTL_ERRLOG(dev, "Failed to create trim map\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * trim_clear_cb - trim MD region clear 완료 콜백 (trim_metadata/log_clear 공용).
 */
static void
trim_clear_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
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
 * ftl_mngt_trim_metadata_clear - TRIM_MD region 0 클리어 (첫 포맷 시).
 */
void
ftl_mngt_trim_metadata_clear(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD];

	md->cb = trim_clear_cb;
	md->owner.cb_ctx = mngt;
	ftl_md_clear(md, 0, NULL);
	/* [한국어] 비동기 0 fill — 완료는 trim_clear_cb. */
}

/*
 * [한국어]
 * ftl_mngt_trim_log_clear - TRIM_LOG region 0 클리어.
 */
void
ftl_mngt_trim_log_clear(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_LOG];

	md->cb = trim_clear_cb;
	md->owner.cb_ctx = mngt;
	ftl_md_clear(md, 0, NULL);
}

/*
 * [한국어]
 * ftl_mngt_deinit_trim_map - trim 비트맵과 MD region 해제.
 */
void
ftl_mngt_deinit_trim_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_bitmap_destroy(dev->trim_map);
	dev->trim_map = NULL;

	ftl_md_destroy(dev->trim_map_md, ftl_md_destroy_shm_flags(dev));
	dev->trim_map_md = NULL;

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * struct ftl_mngt_property_caller_ctx - RPC property get/set의 caller context.
 *
 * 호출자(임의 thread)가 코어 스레드에서 진행되는 작업 결과를 받기 위해 cb 등록.
 */
struct ftl_mngt_property_caller_ctx {
	struct spdk_ftl_dev *dev;
	/* [한국어] 대상 디바이스. */
	struct spdk_jsonrpc_request *request;
	/* [한국어] 진행 중인 JSON-RPC 요청 객체 — get_properties가 이 객체에 직접 dump. */
	spdk_ftl_fn cb_fn;
	/* [한국어] 호출자 콜백. */
	void *cb_arg;
	/* [한국어] 콜백 인자. */
	struct spdk_thread *cb_thread;
	/* [한국어] 호출자 thread — 콜백 실행 thread. */
	const char *property;
	/* [한국어] 설정/조회할 property 이름. */
	const char *value;
	/* [한국어] set일 때 새 값 (string). */
	size_t value_size;
	/* [한국어] value 바이트 크기. */
};

/*
 * [한국어]
 * ftl_get_properties_cb - 호출자 thread에서 실행되는 사용자 cb wrapper.
 */
static void
ftl_get_properties_cb(void *arg)
{
	struct ftl_mngt_property_caller_ctx *cctx = arg;

	cctx->cb_fn(cctx->cb_arg, 0);
	free(cctx);
}

/*
 * [한국어]
 * ftl_get_properties_msg - 코어 스레드에서 실행되는 dump 작업.
 *
 * ftl_property_dump가 모든 등록된 property를 JSON-RPC 응답에 직렬화. 끝나면 호출자
 * thread에 ftl_get_properties_cb 메시지로 결과 통지.
 */
static void
ftl_get_properties_msg(void *arg)
{
	struct ftl_mngt_property_caller_ctx *cctx = arg;

	ftl_property_dump(cctx->dev, cctx->request);
	spdk_thread_send_msg(cctx->cb_thread, ftl_get_properties_cb, cctx);
}

/*
 * [한국어]
 * spdk_ftl_get_properties - 외부 RPC 진입점. 모든 property를 JSON-RPC 응답에 dump.
 *
 * @dev: 디바이스. @request: JSON-RPC 요청. @cb_fn/cb_arg: 완료 콜백.
 *
 * 동작: ctx alloc + 코어 스레드에 ftl_get_properties_msg 디스패치.
 *
 * 실행 컨텍스트: RPC 핸들러 thread. 코어 스레드에서 실제 dump.
 */
int
spdk_ftl_get_properties(struct spdk_ftl_dev *dev, struct spdk_jsonrpc_request *request,
			spdk_ftl_fn cb_fn, void *cb_arg)
{
	struct ftl_mngt_property_caller_ctx *ctx = calloc(1, sizeof(*ctx));

	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->dev = dev;
	ctx->request = request;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->cb_thread = spdk_get_thread();

	spdk_thread_send_msg(dev->core_thread, ftl_get_properties_msg, ctx);

	return 0;
}

/*
 * [한국어]
 * struct ftl_set_property_process_ctx - set_property sub-process의 process ctx.
 *
 * decode 단계가 string → 내부 binary로 변환한 결과를 보관.
 */
struct ftl_set_property_process_ctx {
	void *value;
	/* [한국어] decode된 binary value. cleanup에서 free. */
	size_t value_size;
};

/*
 * [한국어]
 * ftl_mngt_set_property_decode - property string → binary로 변환하는 step.
 *
 * caller_ctx의 property 이름과 string value를 받아 ftl_property_decode로 변환.
 * 결과는 process_ctx의 value/value_size에 저장.
 */
static void
ftl_mngt_set_property_decode(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_set_property_process_ctx *pctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_mngt_property_caller_ctx *cctx = ftl_mngt_get_caller_ctx(mngt);

	if (ftl_property_decode(dev, cctx->property, cctx->value, cctx->value_size,
				&pctx->value, &pctx->value_size)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_set_property - decode된 binary value를 property에 적용하는 step.
 *
 * ftl_property_set은 비동기일 수 있어 next_step 호출은 ftl_property_set 내부의 mngt 진행에
 * 위임 (실패 시만 본 함수가 fail_step).
 */
static void
ftl_mngt_set_property(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_set_property_process_ctx *pctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_mngt_property_caller_ctx *cctx = ftl_mngt_get_caller_ctx(mngt);

	if (ftl_property_set(dev, mngt, cctx->property, pctx->value, pctx->value_size)) {
		ftl_mngt_fail_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_set_property_cleanup - decode된 value 메모리 해제 (cleanup + 마지막 step).
 */
static void
ftl_mngt_set_property_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_set_property_process_ctx *pctx = ftl_mngt_get_process_ctx(mngt);
	free(pctx->value);
	pctx->value = NULL;
	pctx->value_size = 0;
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * desc_set_property - property 설정 sub-process desc.
 *
 * decode → set → cleanup 3단계. 모든 step의 cleanup도 set_property_cleanup이라 어디서
 * 실패해도 value 누수 없음.
 */
static const struct ftl_mngt_process_desc desc_set_property = {
	.name = "Set FTL property",
	.ctx_size = sizeof(struct ftl_set_property_process_ctx),
	.steps = {
		{
			.name = "Decode property",
			.action = ftl_mngt_set_property_decode,
			.cleanup = ftl_mngt_set_property_cleanup
		},
		{
			.name = "Set property",
			.action = ftl_mngt_set_property,
			.cleanup = ftl_mngt_set_property_cleanup
		},
		{
			.name = "Property setting cleanup",
			.action = ftl_mngt_set_property_cleanup,
		},
		{}
	}
};

/*
 * [한국어]
 * ftl_mngt_property_caller_cb - desc_set_property 완료 콜백.
 */
static void
ftl_mngt_property_caller_cb(struct spdk_ftl_dev *dev, void *ctx, int status)
{
	struct ftl_mngt_property_caller_ctx *cctx = ctx;

	cctx->cb_fn(cctx->cb_arg, status);
	free(cctx);
}

/*
 * [한국어]
 * spdk_ftl_set_property - 외부 RPC 진입점. property 설정.
 *
 * desc_set_property를 process_execute로 시작. 실행 컨텍스트는 caller thread이지만
 * step 액션은 코어 스레드에서.
 */
int
spdk_ftl_set_property(struct spdk_ftl_dev *dev,
		      const char *property, const char *value, size_t value_size,
		      spdk_ftl_fn cb_fn, void *cb_arg)
{
	int rc;
	struct ftl_mngt_property_caller_ctx *cctx = calloc(1, sizeof(*cctx));

	if (cctx == NULL) {
		return -EAGAIN;
	}
	cctx->cb_fn = cb_fn;
	cctx->cb_arg = cb_arg;
	cctx->property = property;
	cctx->value = value;
	cctx->value_size = value_size;

	rc = ftl_mngt_process_execute(dev, &desc_set_property, ftl_mngt_property_caller_cb, cctx);
	if (rc) {
		free(cctx);
	}

	return rc;
}
