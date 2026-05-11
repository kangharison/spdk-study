/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 메타데이터(MD) region 라이프사이클 step (ftl_mngt_md.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 FTL의 모든 메타데이터(MD) region — 슈퍼블록(SB)/SB mirror, NV cache MD,
 * 밴드 MD, valid map, P2L checkpoint, trim map, L2P MD 등 — 의 라이프사이클 step을
 * 정의한다. 다음을 담당:
 *  - layout 셋업 (init_layout)
 *  - 모든 region의 ftl_md 핸들 메모리 할당/해제 (init/deinit_md) 및 mirror region 셋업
 *  - 종합 영속화 sub-process (desc_persist) — clean shutdown 시 모든 MD를 디스크에 flush
 *  - 빠른 영속화 sub-process (desc_fast_persist) — fast shutdown 시 NV cache MD만 SHM에
 *  - 종합 복원 sub-process (desc_restore) — clean startup 시 모든 MD를 디스크에서 로드
 *  - 슈퍼블록 init/load/validate/persist (desc_init_sb, desc_restore_sb 등 sub-process)
 *  - 디바이스 dirty/clean 상태 마킹 (set_dirty/set_clean/set_shm_clean)
 *  - 슈퍼블록 SHM mirror 처리 — fast restart 시 SHM에서 SB 즉시 픽업
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 컨텍스트: 코어 spdk_thread (startup/shutdown/recovery 파이프라인 step).
 *   디스크 I/O가 많이 발생하는 step들이라 모두 ftl_md 비동기 API(persist/restore/clear)에
 *   의존하며, 완료 콜백(persist_cb/restore_cb)에서 next/fail step.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: lib/ftl/utils/ftl_md.[ch] (모든 MD I/O), lib/ftl/ftl_layout.[ch] (region 정의),
 *   lib/ftl/ftl_sb.[ch] (슈퍼블록 마법수/CRC/version/blob area),
 *   lib/ftl/upgrade/ftl_layout_upgrade.h, ftl_sb_upgrade.h (SB upgrade),
 *   lib/ftl/ftl_nv_cache.[ch], lib/ftl/ftl_band.[ch] (band MD load).
 * - 의존받음: ftl_mngt_startup.c, ftl_mngt_shutdown.c, ftl_mngt_recovery.c.
 * - 데이터 흐름: 디스크 ↔ dev->layout.md[*] (인메모리 ftl_md) ↔ 각 모듈
 *   (sb, valid_map, band->md 등). 모든 region은 layout.md[type] 인덱스로 통일 관리.
 *
 * === 주요 함수/구조체 요약 ===
 * - ftl_mngt_init_layout()              : ftl_layout_setup 어댑터.
 * - is_buffer_needed()                  : SB/data/mirror/L2P_FLAT 등 버퍼 필요 여부 판단.
 * - ftl_mngt_init_md()/deinit_md()      : 모든 MD region 핸들 일괄 alloc/free + mirror 셋업.
 * - persist_cb / persist()              : 단일 region 비동기 영속화 헬퍼.
 * - ftl_md_restore_region()             : restore 후 모듈별 후처리(nv_cache/valid_map/band).
 * - restore_cb / restore()              : 단일 region 비동기 복원 헬퍼.
 * - persist_nv_cache_metadata / fast_persist_nv_cache_metadata.
 * - persist_vld_map_metadata / persist_p2l_metadata / persist_band_info_metadata /
 *   persist_trim_metadata / persist_super_block.
 * - get_sb_crc()                        : 슈퍼블록 CRC 계산 (자기 CRC 필드 제외).
 * - desc_persist                        : clean shutdown 영속화 sub-process.
 * - desc_fast_persist                   : fast shutdown 영속화 sub-process.
 * - ftl_mngt_persist_md / fast_persist_md : 외부 step 진입점.
 * - ftl_mngt_init_default_sb / set_dirty / set_clean / set_shm_clean.
 * - ftl_mngt_load_sb / validate_sb / persist_superblock.
 * - desc_restore_sb / desc_init_sb       : SB sub-process.
 * - ftl_mngt_superblock_init / deinit    : SB MD region + SHM mirror 셋업/해제.
 * - desc_restore                        : 모든 MD 복원 sub-process.
 * - ftl_mngt_restore_md                 : 외부 step 진입점.
 */

#include "spdk/thread.h"
#include "spdk/crc32.h"
/* [한국어] spdk_crc32c_update — 슈퍼블록 무결성 검증용. */
#include "spdk/string.h"
/* [한국어] spdk_strcpy_pad — 슈퍼블록 base/nvc 디바이스 이름 복사. */

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_utils.h"
#include "ftl_band.h"
#include "ftl_internal.h"
#include "ftl_sb.h"
/* [한국어] FTL_SUPERBLOCK_MAGIC, FTL_SB_VERSION_*, ftl_superblock_check_magic, blob_area. */
#include "base/ftl_base_dev.h"
/* [한국어] base device ops (md_layout_ops). */
#include "nvc/ftl_nvc_dev.h"
/* [한국어] NV cache device ops. */
#include "upgrade/ftl_layout_upgrade.h"
#include "upgrade/ftl_sb_upgrade.h"
/* [한국어] ftl_superblock_upgrade — 구버전 SB를 현재 버전으로 변환. */

/*
 * [한국어]
 * ftl_mngt_init_layout - dev->layout 셋업 step. ftl_layout_setup 어댑터.
 *
 * ftl_layout_setup이 base/nvc bdev의 size를 보고 region 위치/크기를 모두 결정.
 */
void
ftl_mngt_init_layout(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_layout_setup(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

/*
 * [한국어]
 * is_buffer_needed - region이 별도 인메모리 buffer를 가져야 하는지 판단.
 *
 * @return: false면 buffer 없음 (SB/data region 또는 mirror region 또는 L2P_FLAT 모드).
 *
 * 동작:
 *  - SB / SB_BASE: 슈퍼블록은 superblock_init에서 별도 처리하므로 init_md에서 skip.
 *  - DATA_NVC / DATA_BASE: 사용자 데이터 영역 — 인메모리 buffer 가질 필요 없음 (디스크 직접 사용).
 *  - *_MIRROR: mirror region은 원본 region의 buffer를 share — 자기 buffer 안 가짐.
 *  - L2P (SPDK_FTL_L2P_FLAT 빌드): L2P가 디스크 직접 매핑 — buffer 없음.
 *  - P2L_LOG_IO_*: 옵션 region — buffer 자동 생성 안 함.
 *  - 그 외: 버퍼 필요(true).
 */
static bool
is_buffer_needed(enum ftl_layout_region_type type)
{
	switch (type) {
	case FTL_LAYOUT_REGION_TYPE_SB:
	case FTL_LAYOUT_REGION_TYPE_SB_BASE:
	case FTL_LAYOUT_REGION_TYPE_DATA_NVC:
	case FTL_LAYOUT_REGION_TYPE_DATA_BASE:
	case FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR:
	case FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR:
#ifndef SPDK_FTL_L2P_FLAT
	case FTL_LAYOUT_REGION_TYPE_L2P:
#endif
	case FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR:
	case FTL_LAYOUT_REGION_TYPE_TRIM_LOG_MIRROR:
	case FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MIN:
	case FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MAX:
		return false;

	default:
		return true;
	}
}

/*
 * [한국어]
 * ftl_mngt_init_md - 모든 MD region의 ftl_md 핸들 일괄 alloc + mirror 셋업.
 *
 * 동작:
 *  1) 첫 루프: 0 ~ FTL_LAYOUT_REGION_TYPE_MAX 순회.
 *     - region이 layout에 없으면 skip.
 *     - 이미 layout->md[i]가 있으면 skip (SB 등 사전 셋업된 것).
 *     - is_buffer_needed로 buffer 필요 판단해 md_flags 결정.
 *     - ftl_md_create로 핸들 생성. NULL이면 fail.
 *  2) 둘째 루프: mirror region 셋업.
 *     - region->mirror_type이 INVALID 아니고 mirror가 buffer 없는 경우만:
 *       원본 md의 dev/data_blocks/data 등을 mirror md에 share. is_mirror = true.
 */
void
ftl_mngt_init_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region;
	struct ftl_md *md, *md_mirror;
	enum ftl_layout_region_type i;
	int md_flags;

	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; i++) {
		region = ftl_layout_region_get(dev, i);
		if (!region) {
			continue;
		}
		assert(i == region->type);
		if (layout->md[i]) {
			/*
			 * Some metadata objects are initialized by other FTL
			 * components. At the moment it's only used by superblock (and its mirror) -
			 * during load time we need to read it earlier in order to get the layout for the
			 * other regions.
			 */
			/* [한국어] 슈퍼블록은 layout 결정에 필요해서 먼저 셋업되어 있음 — skip. */
			continue;
		}
		md_flags = is_buffer_needed(i) ? ftl_md_create_region_flags(dev,
				region->type) : FTL_MD_CREATE_NO_MEM;
		/* [한국어] buffer 필요하면 region별 플래그(SHM 등), 아니면 NO_MEM (외부 buffer 사용). */
		layout->md[i] = ftl_md_create(dev, region->current.blocks, region->vss_blksz, region->name,
					      md_flags, region);
		if (NULL == layout->md[i]) {
			ftl_mngt_fail_step(mngt);
			return;
		}
	}

	/* Initialize mirror regions */
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; i++) {
		region = ftl_layout_region_get(dev, i);
		if (!region) {
			continue;
		}
		assert(i == region->type);
		if (region->mirror_type != FTL_LAYOUT_REGION_TYPE_INVALID &&
		    !is_buffer_needed(region->mirror_type)) {
			md = layout->md[i];
			md_mirror = layout->md[region->mirror_type];

			md_mirror->dev = md->dev;
			md_mirror->data_blocks = md->data_blocks;
			md_mirror->data = md->data;
			/* [한국어] mirror가 원본 buffer를 share — 디스크 write를 두 위치에 중복
			 * 발행해서 한쪽이 손상돼도 복구 가능. */
			if (md_mirror->region->vss_blksz == md->region->vss_blksz) {
				md_mirror->vss_data = md->vss_data;
			}
			md_mirror->region = ftl_layout_region_get(dev, region->mirror_type);
			ftl_bug(md_mirror->region == NULL);
			md_mirror->is_mirror = true;
		}
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_deinit_md - 모든 MD region 핸들 해제.
 */
void
ftl_mngt_deinit_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region;
	enum ftl_layout_region_type i;

	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; i++) {
		region = ftl_layout_region_get(dev, i);
		if (!region) {
			continue;
		}
		if (layout->md[i]) {
			ftl_md_destroy(layout->md[i], ftl_md_destroy_region_flags(dev, region->type));
			layout->md[i] = NULL;
		}
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * persist_cb - ftl_md_persist 비동기 완료 콜백.
 */
static void
persist_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
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
 * persist - 단일 region을 비동기 영속화 (디스크에 flush).
 */
static void
persist(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
	enum ftl_layout_region_type type)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_md *md;

	assert(type < FTL_LAYOUT_REGION_TYPE_MAX);

	md = layout->md[type];
	if (!md) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	md->owner.cb_ctx = mngt;
	md->cb = persist_cb;
	ftl_md_persist(md);
	/* [한국어] 비동기 디스크 write 발행. mirror가 있으면 자동으로 함께 발행. */
}

/*
 * [한국어]
 * ftl_md_restore_region - region을 디스크에서 메모리로 read한 후 모듈별 후처리.
 *
 * read 자체는 ftl_md_restore가 담당, 본 함수는 restore_cb 안에서 호출되어
 * - NVC_MD: nv_cache 모듈에 chunk state load
 * - VALID_MAP: valid_map_load_state (밴드별 num_valid 카운트 재계산 등)
 * - BAND_MD: ftl_bands_load_state (각 밴드 state/seq 등 로드)
 * 다른 region은 read만 끝내면 됨.
 */
static int
ftl_md_restore_region(struct spdk_ftl_dev *dev, int region_type)
{
	int status = 0;
	switch (region_type) {
	case FTL_LAYOUT_REGION_TYPE_NVC_MD:
		status = ftl_nv_cache_load_state(&dev->nv_cache);
		break;
	case FTL_LAYOUT_REGION_TYPE_VALID_MAP:
		ftl_valid_map_load_state(dev);
		break;
	case FTL_LAYOUT_REGION_TYPE_BAND_MD:
		status = ftl_bands_load_state(dev);
		break;
	default:
		break;
	}
	return status;
}

/*
 * [한국어]
 * restore_cb - ftl_md_restore 비동기 완료 콜백.
 *
 * 동작: status 검사 후 region별 후처리(ftl_md_restore_region) 실행, 실패 시 fail.
 */
static void
restore_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;
	const struct ftl_layout_region *region = ftl_md_get_region(md);

	if (status) {
		/* Restore error, end step */
		ftl_mngt_fail_step(mngt);
		return;
	}

	assert(region);
	status = ftl_md_restore_region(dev, region->type);

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

/*
 * [한국어]
 * restore - 단일 region을 디스크에서 메모리로 비동기 복원.
 */
static void
restore(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt, enum ftl_layout_region_type type)
{
	struct ftl_layout *layout = &dev->layout;
	assert(type < FTL_LAYOUT_REGION_TYPE_MAX);
	struct ftl_md *md = layout->md[type];

	if (!md) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	md->owner.cb_ctx = mngt;
	md->cb = restore_cb;
	ftl_md_restore(md);
}

/*
 * [한국어]
 * ftl_mngt_persist_nv_cache_metadata - NV cache MD region 영속화.
 *
 * 먼저 ftl_nv_cache_save_state로 메모리 chunk state를 직렬화 후 region을 디스크에 write.
 */
void
ftl_mngt_persist_nv_cache_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_nv_cache_save_state(&dev->nv_cache)) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_NVC_MD);
}

/*
 * [한국어]
 * ftl_mngt_fast_persist_nv_cache_metadata - SHM에만 NV cache state 저장(디스크 write 생략).
 */
static void
ftl_mngt_fast_persist_nv_cache_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_nv_cache_save_state(&dev->nv_cache)) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_persist_vld_map_metadata - VALID_MAP region 영속화.
 */
static void
ftl_mngt_persist_vld_map_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_VALID_MAP);
}

/*
 * [한국어]
 * ftl_mngt_persist_p2l_metadata - 런타임 P2L 동기화 후 모든 P2L_CKPT region 일괄 영속화.
 *
 * step ctx로 ftl_p2l_sync_ctx 사용 (ctx_size로 desc에 등록됨). md_region 카운터를
 * P2L_CKPT_MIN으로 셋팅 후 ftl_mngt_persist_bands_p2l 호출 — 이 함수가 region을 하나씩
 * 영속화하며 continue_step으로 본 step을 반복 호출.
 */
static void
ftl_mngt_persist_p2l_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	/* Sync runtime P2L to persist any invalidation that may have happened */

	struct ftl_p2l_sync_ctx *ctx = ftl_mngt_get_step_ctx(mngt);

	/*
	 * ftl_mngt_persist_bands_p2l will increment the md_region before the step_continue for next regions
	 */
	if (ctx->md_region <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN) {
		ctx->md_region = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	}
	ftl_mngt_persist_bands_p2l(mngt);
}

/*
 * [한국어]
 * ftl_mngt_persist_band_info_metadata - BAND_MD region 영속화.
 */
void
ftl_mngt_persist_band_info_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_BAND_MD);
}

/*
 * [한국어]
 * ftl_mngt_persist_trim_metadata - TRIM_MD region 영속화.
 */
static void
ftl_mngt_persist_trim_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_TRIM_MD);
}

/*
 * [한국어]
 * get_sb_crc - 슈퍼블록 CRC32C 계산. CRC 필드 자체는 제외하고 나머지 영역 전체 해시.
 *
 * v2와 v3+ 슈퍼블록의 크기/포맷이 다르므로 분기:
 *  - v2: ftl_superblock_v2 크기까지만 계산.
 *  - v3+: 슈퍼블록 전체(FTL_SUPERBLOCK_SIZE)까지 계산.
 */
static uint32_t
get_sb_crc(struct ftl_superblock *sb)
{
	uint32_t crc = 0;

	/* Calculate CRC excluding CRC field in superblock */
	void *buffer = sb;
	size_t offset = offsetof(struct ftl_superblock, header.crc);
	size_t size = offset;
	crc = spdk_crc32c_update(buffer, size, crc);
	/* [한국어] header.crc 직전까지 해시. */

	buffer += offset + sizeof(sb->header.crc);
	if (sb->header.version > FTL_SB_VERSION_2) {
		/* whole buf for v3 and on: */
		size = FTL_SUPERBLOCK_SIZE - offset - sizeof(sb->header.crc);
		crc = spdk_crc32c_update(buffer, size, crc);
	} else {
		/* special for sb v2 only: */
		/* [한국어] v2 호환 — 구버전 SB는 v2 크기까지만 해시. */
		size = sizeof(struct ftl_superblock_v2) - offset - sizeof(sb->header.crc);
		sb->header.crc = spdk_crc32c_update(buffer, size, crc);
	}

	return crc;
}

/*
 * [한국어]
 * ftl_mngt_persist_super_block - 슈퍼블록을 최신 상태로 갱신 후 영속화.
 *
 * gc_info(SHM에서 갱신되는 GC 진행 상태) 복사 + CRC 재계산 후 SB region에 write.
 */
static void
ftl_mngt_persist_super_block(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->sb->overprovisioning = dev->conf.overprovisioning;
	dev->sb->gc_info = dev->sb_shm->gc_info;
	/* [한국어] SHM에 누적된 GC 진행 정보를 disk SB에 동기화. */
	dev->sb->header.crc = get_sb_crc(dev->sb);
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_SB);
}

/*
 * Persists all necessary metadata (band state, P2L, etc) during FTL's clean shutdown.
 */
/*
 * [한국어]
 * desc_persist - clean shutdown 시 호출되는 모든 MD 영속화 sub-process.
 *
 * 순서가 중요: 사용자 데이터 관련 MD(NV cache, valid_map, P2L, band)를 먼저 영속화하고
 * 마지막에 슈퍼블록을 영속화해 "이 시점까지 모든 MD가 일관성 있다"는 것을 보장.
 */
static const struct ftl_mngt_process_desc desc_persist = {
	.name = "Persist metadata",
	.steps = {
		{
			.name = "Persist NV cache metadata",
			.action = ftl_mngt_persist_nv_cache_metadata,
		},
		{
			.name = "Persist valid map metadata",
			.action = ftl_mngt_persist_vld_map_metadata,
		},
		{
			.name = "Persist P2L metadata",
			.action = ftl_mngt_persist_p2l_metadata,
			.ctx_size = sizeof(struct ftl_p2l_sync_ctx),
			/* [한국어] P2L 영속화는 region 순회 상태 보존 필요. */
		},
		{
			.name = "Persist band info metadata",
			.action = ftl_mngt_persist_band_info_metadata,
		},
		{
			.name = "Persist trim metadata",
			.action = ftl_mngt_persist_trim_metadata,
		},
		{
			.name = "Persist superblock",
			.action = ftl_mngt_persist_super_block,
		},
		{}
	}
};

/*
 * [한국어]
 * ftl_mngt_persist_md - 외부 step. desc_persist sub-process 호출.
 */
void
ftl_mngt_persist_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process(mngt, &desc_persist, NULL);
}

/*
 * Fast clean shutdown path - skips the persistence of most metadata regions and
 * relies on their shared memory state instead.
 */
/*
 * [한국어]
 * desc_fast_persist - fast shutdown(SHM 핸드오프) 시 사용. NV cache state만 SHM에 저장,
 * 나머지 MD는 SHM에 이미 매핑되어 있으므로 디스크 flush 안 함.
 */
static const struct ftl_mngt_process_desc desc_fast_persist = {
	.name = "Fast persist metadata",
	.steps = {
		{
			.name = "Fast persist NV cache metadata",
			.action = ftl_mngt_fast_persist_nv_cache_metadata,
		},
		{}
	}
};

/*
 * [한국어]
 * ftl_mngt_fast_persist_md - 외부 step. desc_fast_persist 호출.
 */
void
ftl_mngt_fast_persist_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process(mngt, &desc_fast_persist, NULL);
}

/*
 * [한국어]
 * ftl_mngt_init_default_sb - 첫 포맷용 기본 슈퍼블록 값 채우기.
 *
 * magic / version / uuid / overprovisioning / max_reloc_qdepth 등을 기본값으로 셋팅.
 * sb->clean = 0 (dirty로 시작). 첫 startup 동안 어떤 비정상 종료가 있으면 다음 부팅에
 * recovery 진입.
 */
void
ftl_mngt_init_default_sb(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->header.magic = FTL_SUPERBLOCK_MAGIC;
	sb->header.version = FTL_SB_VERSION_CURRENT;
	sb->uuid = dev->conf.uuid;
	sb->clean = 0;
	dev->sb_shm->shm_clean = false;
	sb->ckpt_seq_id = 0;

	/* Max 16 IO depth per band relocate */
	sb->max_reloc_qdepth = 16;

	sb->overprovisioning = dev->conf.overprovisioning;

	ftl_band_init_gc_iter(dev);

	/* md layout isn't initialized yet.
	 * empty region list => all regions in the default location */
	/* [한국어] base/nvc 디바이스 이름 슈퍼블록에 기록 — 다음 부팅 시 정합성 검증용. */
	spdk_strcpy_pad(sb->base_dev_name, dev->base_type->name,
			SPDK_COUNTOF(sb->base_dev_name), '\0');
	sb->md_layout_base.df_id = FTL_DF_OBJ_ID_INVALID;
	/* [한국어] base 측 md layout이 아직 결정되지 않음 표시. */

	spdk_strcpy_pad(sb->nvc_dev_name, dev->nv_cache.nvc_type->name,
			SPDK_COUNTOF(sb->nvc_dev_name), '\0');
	sb->md_layout_nvc.df_id = FTL_DF_OBJ_ID_INVALID;

	sb->header.crc = get_sb_crc(sb);

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_set_dirty - 슈퍼블록 dirty 마크 + 디스크 영속화.
 *
 * 첫 write 직전에 호출되어 "이후 비정상 종료 시 recovery 필요" 표시.
 * upgrade_ready도 false로 — 업그레이드 중간 상태가 아님 표시.
 */
void
ftl_mngt_set_dirty(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->clean = 0;
	sb->upgrade_ready = false;
	dev->sb_shm->shm_clean = false;
	sb->header.crc = get_sb_crc(sb);
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_SB);
}

/*
 * [한국어]
 * ftl_mngt_set_clean - 슈퍼블록 clean 마크 + 디스크 영속화 + SHM ready 클리어.
 *
 * clean shutdown의 마지막 단계 직전 호출. 다음 부팅에서 recovery 생략 가능.
 * upgrade_ready는 conf.prep_upgrade_on_shutdown가 true면 set — 다음 부팅이 layout
 * 업그레이드 가능 상태임을 표시.
 */
void
ftl_mngt_set_clean(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->clean = 1;
	sb->upgrade_ready = dev->conf.prep_upgrade_on_shutdown;
	dev->sb_shm->shm_clean = false;
	sb->header.crc = get_sb_crc(sb);
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_SB);

	dev->sb_shm->shm_ready = false;
	/* [한국어] SHM 측 ready 마크 해제 — 다른 프로세스가 픽업하기 전에 확실히 표시. */
}

/*
 * [한국어]
 * ftl_mngt_set_shm_clean - SHM 슈퍼블록만 clean 마크 (디스크 write 안 함).
 *
 * fast shutdown용. 디스크 SB는 dirty 상태 유지 — 만약 SHM 픽업이 실패하면 정식 recovery로 fallback.
 */
void
ftl_mngt_set_shm_clean(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->clean = 1;
	dev->sb_shm->shm_clean = true;
	sb->header.crc = get_sb_crc(sb);
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_load_sb - 슈퍼블록을 디스크 또는 SHM에서 로드.
 *
 * 동작:
 *  - fast_startup이면 SHM에 이미 매핑된 SB region을 사용 — restore_region만 호출(read 생략).
 *  - 아니면 일반 비동기 디스크 read.
 */
void
ftl_mngt_load_sb(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	FTL_NOTICELOG(dev, "SHM: clean %"PRIu64", shm_clean %d\n", dev->sb->clean, dev->sb_shm->shm_clean);

	if (!ftl_fast_startup(dev)) {
		restore(dev, mngt, FTL_LAYOUT_REGION_TYPE_SB);
		return;
	}

	FTL_DEBUGLOG(dev, "SHM: found SB\n");
	if (ftl_md_restore_region(dev, FTL_LAYOUT_REGION_TYPE_SB)) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_validate_sb - 로드된 슈퍼블록의 magic / CRC / version / UUID / lba_cnt /
 *                        overprovisioning / blob area 검증.
 *
 * 모든 검증 통과 시 dev->num_lbas / dev->conf.overprovisioning 갱신.
 */
void
ftl_mngt_validate_sb(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	if (!ftl_superblock_check_magic(sb)) {
		FTL_ERRLOG(dev, "Invalid FTL superblock magic\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (sb->header.crc != get_sb_crc(sb)) {
		/* [한국어] 슈퍼블록이 손상됨 (write 도중 전원 끊김 등). */
		FTL_ERRLOG(dev, "Invalid FTL superblock CRC\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (ftl_superblock_upgrade(dev)) {
		/* [한국어] 구버전 SB → 현재 버전 변환. 실패면 dirty거나 미지원 버전. */
		FTL_ERRLOG(dev, "FTL superblock dirty or invalid version\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (spdk_uuid_compare(&sb->uuid, &dev->conf.uuid) != 0) {
		/* [한국어] UUID 불일치 = 다른 디바이스. */
		FTL_ERRLOG(dev, "Invalid FTL superblock UUID\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (sb->lba_cnt == 0) {
		FTL_ERRLOG(dev, "Invalid FTL superblock lba_cnt\n");
		ftl_mngt_fail_step(mngt);
		return;
	}
	dev->num_lbas = sb->lba_cnt;

	/* The sb has just been read. Validate and update the conf */
	if (sb->overprovisioning == 0 || sb->overprovisioning >= 100) {
		FTL_ERRLOG(dev, "Invalid FTL superblock lba_rsvd\n");
		ftl_mngt_fail_step(mngt);
		return;
	}
	dev->conf.overprovisioning = sb->overprovisioning;

	if (!ftl_superblock_validate_blob_area(dev)) {
		/* [한국어] blob area는 region별 메타(version 등) 보관 — 손상 시 layout 정보 잃음. */
		FTL_ERRLOG(dev, "Corrupted FTL superblock blob area\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

/*
 * Loads and verifies superblock contents - utilized during the load of an FTL
 * instance (both from a clean and dirty shutdown).
 */
/*
 * [한국어]
 * desc_restore_sb - SB load + validate sub-process. ftl_mngt_superblock_init이 호출.
 */
static const struct ftl_mngt_process_desc desc_restore_sb = {
	.name = "SB restore",
	.steps = {
		{
			.name = "Load super block",
			.action = ftl_mngt_load_sb
		},
		{
			.name = "Validate super block",
			.action = ftl_mngt_validate_sb
		},
		{}
	}
};

/*
 * Initializes the superblock fields during first startup of FTL
 */
/*
 * [한국어]
 * desc_init_sb - 첫 포맷 시 SB 기본값 채우기 sub-process. ftl_mngt_superblock_init이 호출.
 */
static const struct ftl_mngt_process_desc desc_init_sb = {
	.name = "SB initialize",
	.steps = {
		{
			.name = "Default-initialize superblock",
			.action = ftl_mngt_init_default_sb,
		},
		{}
	}
};

/*
 * [한국어]
 * ftl_mngt_superblock_init - 슈퍼블록 region + SHM mirror + base mirror 셋업.
 *
 * 가장 복잡한 SB 셋업 함수. 동작:
 *  1) CREATE 모드면 새 UUID 생성 (SHM에 SB 만들기 전이라 conf.uuid 갱신 가능).
 *  2) shm_retry: SB SHM region 생성 시도.
 *     - 첫 시도는 기존 SHM open 시도 — fast restart 케이스에 유효한 SHM 픽업.
 *     - 실패 시 SHM_NEW 플래그 추가해 새로 생성, retry.
 *  3) sb_shm pointer 셋팅.
 *  4) ftl_layout_setup_superblock — SB region의 위치/크기 결정.
 *  5) SB MD region 생성 (NVC 측). 같은 retry 패턴.
 *  6) sb pointer 셋팅.
 *  7) SB_BASE region 생성 — base bdev에 SB 미러링 (NO_MEM 플래그로 buffer 공유).
 *  8) mirror md에 원본 buffer share.
 *  9) CREATE면 desc_init_sb, LOAD면 desc_restore_sb 호출.
 */
void
ftl_mngt_superblock_init(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md;
	struct ftl_md *md_mirror;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = &layout->region[FTL_LAYOUT_REGION_TYPE_SB];
	char uuid[SPDK_UUID_STRING_LEN];
	int md_create_flags = ftl_md_create_region_flags(dev, FTL_LAYOUT_REGION_TYPE_SB);

	/* Must generate UUID before MD create on SHM for the SB */
	/* [한국어] SHM에 SB 만들기 전에 UUID 결정 — UUID가 SHM 식별에도 쓰임. */
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		spdk_uuid_generate(&dev->conf.uuid);
		spdk_uuid_fmt_lower(uuid, sizeof(uuid), &dev->conf.uuid);
		FTL_NOTICELOG(dev, "Create new FTL, UUID %s\n", uuid);
	}

shm_retry:
	/* Allocate md buf */
	dev->sb_shm = NULL;
	dev->sb_shm_md = ftl_md_create(dev, spdk_divide_round_up(sizeof(*dev->sb_shm), FTL_BLOCK_SIZE),
				       0, "sb_shm",
				       md_create_flags, NULL);
	if (dev->sb_shm_md == NULL) {
		/* The first attempt may fail when trying to open SHM - try to create new */
		/* [한국어] SHM open 실패 시 NEW 플래그로 재시도 — 정상 부팅 케이스. */
		if ((md_create_flags & FTL_MD_CREATE_SHM_NEW) == 0) {
			md_create_flags |= FTL_MD_CREATE_SHM_NEW;
			goto shm_retry;
		}
		if (dev->sb_shm_md == NULL) {
			ftl_mngt_fail_step(mngt);
			return;
		}
	}

	dev->sb_shm = ftl_md_get_buffer(dev->sb_shm_md);

	/* Setup the layout of a superblock */
	if (ftl_layout_setup_superblock(dev)) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	/* Allocate md buf */
	layout->md[FTL_LAYOUT_REGION_TYPE_SB] = ftl_md_create(dev, region->current.blocks,
						region->vss_blksz, region->name,
						md_create_flags, region);
	if (NULL == layout->md[FTL_LAYOUT_REGION_TYPE_SB]) {
		/* The first attempt may fail when trying to open SHM - try to create new */
		if ((md_create_flags & FTL_MD_CREATE_SHM_NEW) == 0) {
			md_create_flags |= FTL_MD_CREATE_SHM_NEW;
			ftl_md_destroy(dev->sb_shm_md, 0);
			dev->sb_shm_md = NULL;
			if (ftl_layout_clear_superblock(dev)) {
				ftl_mngt_fail_step(mngt);
				return;
			}
			goto shm_retry;
		}
		ftl_mngt_fail_step(mngt);
		return;
	}

	/* Link the md buf to the device */
	dev->sb = ftl_md_get_buffer(layout->md[FTL_LAYOUT_REGION_TYPE_SB]);

	/* Setup superblock mirror to QLC */
	/* [한국어] base bdev (QLC) 쪽에도 SB mirror 둠 — NV cache가 손상돼도 base에서 복구 가능. */
	region = &layout->region[FTL_LAYOUT_REGION_TYPE_SB_BASE];
	layout->md[FTL_LAYOUT_REGION_TYPE_SB_BASE] = ftl_md_create(dev, region->current.blocks,
			region->vss_blksz, NULL, FTL_MD_CREATE_NO_MEM, region);
	if (NULL == layout->md[FTL_LAYOUT_REGION_TYPE_SB_BASE]) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	/* Initialize mirror region buffer */
	md = layout->md[FTL_LAYOUT_REGION_TYPE_SB];
	md_mirror = layout->md[FTL_LAYOUT_REGION_TYPE_SB_BASE];

	md_mirror->dev = md->dev;
	md_mirror->data_blocks = md->data_blocks;
	md_mirror->data = md->data;
	/* [한국어] 같은 buffer를 share — write가 양쪽 region에 동시 발행됨. */
	md_mirror->is_mirror = true;

	/* Initialize the superblock */
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		ftl_mngt_call_process(mngt, &desc_init_sb, NULL);
	} else {
		ftl_mngt_call_process(mngt, &desc_restore_sb, NULL);
	}
}

/*
 * [한국어]
 * ftl_mngt_superblock_deinit - SB region + SB_BASE region + sb_shm region 해제.
 */
void
ftl_mngt_superblock_deinit(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;

	if (layout->md[FTL_LAYOUT_REGION_TYPE_SB]) {
		ftl_md_destroy(layout->md[FTL_LAYOUT_REGION_TYPE_SB],
			       ftl_md_destroy_region_flags(dev, FTL_LAYOUT_REGION_TYPE_SB));
		layout->md[FTL_LAYOUT_REGION_TYPE_SB] = NULL;
	}

	if (layout->md[FTL_LAYOUT_REGION_TYPE_SB_BASE]) {
		ftl_md_destroy(layout->md[FTL_LAYOUT_REGION_TYPE_SB_BASE], 0);
		layout->md[FTL_LAYOUT_REGION_TYPE_SB_BASE] = NULL;
	}

	ftl_md_destroy(dev->sb_shm_md, ftl_md_destroy_shm_flags(dev));
	dev->sb_shm_md = NULL;
	dev->sb_shm = NULL;

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_restore_nv_cache_metadata - NVC_MD region 복원 (fast startup이면 SHM에서).
 */
static void
ftl_mngt_restore_nv_cache_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_fast_startup(dev)) {
		FTL_DEBUGLOG(dev, "SHM: found nv cache md\n");
		if (ftl_md_restore_region(dev, FTL_LAYOUT_REGION_TYPE_NVC_MD)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ftl_mngt_next_step(mngt);
		return;
	}
	restore(dev, mngt, FTL_LAYOUT_REGION_TYPE_NVC_MD);
}

/*
 * [한국어]
 * ftl_mngt_restore_vld_map_metadata - VALID_MAP region 복원.
 */
static void
ftl_mngt_restore_vld_map_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_fast_startup(dev)) {
		FTL_DEBUGLOG(dev, "SHM: found vldmap\n");
		if (ftl_md_restore_region(dev, FTL_LAYOUT_REGION_TYPE_VALID_MAP)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ftl_mngt_next_step(mngt);
		return;
	}
	restore(dev, mngt, FTL_LAYOUT_REGION_TYPE_VALID_MAP);
}

/*
 * [한국어]
 * ftl_mngt_restore_band_info_metadata - BAND_MD region 복원.
 */
static void
ftl_mngt_restore_band_info_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_fast_startup(dev)) {
		FTL_DEBUGLOG(dev, "SHM: found band md\n");
		if (ftl_md_restore_region(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ftl_mngt_next_step(mngt);
		return;
	}
	restore(dev, mngt, FTL_LAYOUT_REGION_TYPE_BAND_MD);
}

/*
 * [한국어]
 * ftl_mngt_restore_trim_metadata - TRIM_MD region 복원.
 */
static void
ftl_mngt_restore_trim_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_fast_startup(dev)) {
		FTL_DEBUGLOG(dev, "SHM: found trim md\n");
		if (ftl_md_restore_region(dev, FTL_LAYOUT_REGION_TYPE_TRIM_MD)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ftl_mngt_next_step(mngt);
		return;
	}
	restore(dev, mngt, FTL_LAYOUT_REGION_TYPE_TRIM_MD);
}

/*
 * Loads metadata after a clean shutdown.
 */
/*
 * [한국어]
 * desc_restore - clean startup 시 모든 MD region을 복원하는 sub-process.
 *
 * 순서: NV cache MD → valid map → band MD → trim MD. 슈퍼블록은 superblock_init에서 별도 로드.
 * P2L checkpoint는 desc_clean_start의 별도 step (ftl_mngt_p2l_restore_ckpt).
 * L2P는 desc_clean_start의 또 다른 별도 step (ftl_mngt_restore_l2p).
 */
static const struct ftl_mngt_process_desc desc_restore = {
	.name = "Restore metadata",
	.steps = {
		{
			.name = "Restore NV cache metadata",
			.action = ftl_mngt_restore_nv_cache_metadata,
		},
		{
			.name = "Restore valid map metadata",
			.action = ftl_mngt_restore_vld_map_metadata,
		},
		{
			.name = "Restore band info metadata",
			.action = ftl_mngt_restore_band_info_metadata,
		},
		{
			.name = "Restore trim metadata",
			.action = ftl_mngt_restore_trim_metadata,
		},
		{}
	}
};

/*
 * [한국어]
 * ftl_mngt_restore_md - 외부 step. desc_restore 호출.
 */
void
ftl_mngt_restore_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process(mngt, &desc_restore, NULL);
}

/*
 * [한국어]
 * ftl_mngt_persist_superblock - SB region만 영속화 (CRC 재계산 후 write).
 *
 * 다른 step의 일부로 자주 호출됨 (예: layout upgrade의 region마다 SB 갱신 보존).
 */
void
ftl_mngt_persist_superblock(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->sb->header.crc = get_sb_crc(dev->sb);
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_SB);
}
