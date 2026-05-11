/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL NVC chunk 메타데이터 v1 → v2 업그레이드 (ftl_chunk_upgrade.c)
 *
 * === 파일의 역할 ===
 * NVC chunk 메타데이터 영역의 디스크 포맷을 v1에서 v2로 마이그레이션한다.
 * 단, band 업그레이드와 다르게 chunk는 사용자 데이터가 모두 빠진 상태에서만(major upgrade)
 * 진행되므로 v1 콘텐츠를 보존하지 않고 v2 형식으로 0부터 다시 초기화하는 단순한 전략을 쓴다.
 * 따라서 흐름은 "v2 region 생성 → ftl_md_create → 모든 chunk md 초기화 → ftl_md_persist".
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   ftl_layout_verify / ftl_region_upgrade
 *     → nvc_upgrade_desc[v1].verify = v1_to_v2_upgrade_enabled
 *     → nvc_upgrade_desc[v1].upgrade = v1_to_v2_upgrade
 *       → setup_ctx(region_open + ftl_md_create) + initialize 모든 chunk md
 *       → ftl_md_persist → v1_to_v2_upgrade_md_cb → ftl_region_upgrade_completed
 *
 * === 타 모듈과의 연결 ===
 * - lib/ftl/upgrade/ftl_layout_upgrade.c : 디스크립터 호출 엔진
 * - lib/ftl/ftl_nv_cache.h               : ftl_nv_cache_chunk_md / FTL_NVC_VERSION_*
 * - lib/ftl/ftl_md.{c,h}                 : ftl_md_create/persist
 * - lib/ftl/nvc/ftl_nvc_dev.h            : md_layout_ops.region_create/open
 *
 * === 주요 함수/구조체 요약 ===
 * - struct upgrade_ctx                   : v2 region/ftl_md 보유
 * - v1_to_v2_upgrade_cleanup / _finish   : ctx 정리 + 상위 콜백
 * - v1_to_v2_upgrade_set                 : v2 chunk md 전체 초기화
 * - v1_to_v2_upgrade_md_cb               : persist 완료 콜백
 * - v1_to_v2_upgrade_setup_ctx           : v2 region open + ftl_md create + initialize
 * - v1_to_v2_upgrade                     : 진입점 (verify 통과 후)
 * - v1_to_v2_upgrade_enabled             : verify
 * - nvc_upgrade_desc                     : 버전별 디스크립터 테이블
 */

#include "ftl_nv_cache.h"          /* [한국어] ftl_nv_cache_chunk_md / FTL_NVC_VERSION_* */
#include "ftl_layout_upgrade.h"    /* [한국어] ftl_region_upgrade_desc / ctx */
#include "ftl_utils.h"             /* [한국어] FTL 공용 유틸 */

/* [한국어] chunk 업그레이드 컨텍스트.
 * v2 데이터에만 관심이 있으므로 v1 ftl_md는 만들지 않는다. */
struct upgrade_ctx {
	struct ftl_md			*md_v2;
	/* [한국어] v2 chunk md를 영속화할 ftl_md 핸들.
	 * 설정자: v1_to_v2_upgrade_setup_ctx (ftl_md_create)
	 * 해제자: v1_to_v2_upgrade_cleanup (ftl_md_destroy) */

	struct ftl_layout_region	reg_v2;
	/* [한국어] v2 region 사본 (region_open 결과). entry_size/num_entries를 finish에 전달. */
};

/*
 * [한국어]
 * v1_to_v2_upgrade_cleanup - chunk 업그레이드 ftl_md 해제 (idempotent)
 */
static void
v1_to_v2_upgrade_cleanup(struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	if (ctx->md_v2) {
		ftl_md_destroy(ctx->md_v2, 0);             /* [한국어] ftl_md 자원 회수 */
		ctx->md_v2 = NULL;                         /* [한국어] 재호출 안전 */
	}
}

/*
 * [한국어]
 * v1_to_v2_upgrade_finish - 업그레이드 종료 통지 (성공/실패 공통)
 *
 * cleanup 후 ftl_region_upgrade_completed로 layout upgrade 엔진에 결과 보고.
 */
static void
v1_to_v2_upgrade_finish(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx, int status)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	v1_to_v2_upgrade_cleanup(lctx);
	ftl_region_upgrade_completed(dev, lctx, ctx->reg_v2.entry_size, ctx->reg_v2.num_entries, status);
}

/*
 * [한국어]
 * v1_to_v2_upgrade_set - v2 chunk md 전체를 초기 상태로 채움
 *
 * Major upgrade 가정 — chunk에 사용자 데이터가 없으므로 v1 콘텐츠를 보존할 필요 없이
 * 모든 chunk md를 ftl_nv_cache_chunk_md_initialize 로 빈 상태로 초기화한다.
 */
static void
v1_to_v2_upgrade_set(struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;
	struct ftl_nv_cache_chunk_md *md = ftl_md_get_buffer(ctx->md_v2); /* [한국어] in-memory v2 배열 */

	assert(sizeof(struct ftl_nv_cache_chunk_md) == FTL_BLOCK_SIZE); /* [한국어] 1 chunk md = 1 block */
	/* [한국어] region.current.blocks 는 chunk_count 와 같은 수의 1-block 엔트리. */
	for (uint64_t i = 0; i < ctx->reg_v2.current.blocks; i++, md++) {
		ftl_nv_cache_chunk_md_initialize(md);      /* [한국어] 빈 chunk md 초기값 세팅 */
	}
}

/*
 * [한국어]
 * v1_to_v2_upgrade_md_cb - persist 완료 콜백
 *
 * 영속화 종료 후 finish로 정리/통지. status가 그대로 전파됨.
 */
static void
v1_to_v2_upgrade_md_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_layout_upgrade_ctx *lctx = md->owner.cb_ctx;

	v1_to_v2_upgrade_finish(dev, lctx, status);
}

/*
 * [한국어]
 * v1_to_v2_upgrade_setup_ctx - v2 region open, ftl_md 생성, in-memory 초기화
 *
 * @type: region 종류 (예: FTL_LAYOUT_REGION_TYPE_NVC_MD)
 * @return: 0 성공, -1 실패
 */
static int
v1_to_v2_upgrade_setup_ctx(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx,
			   uint32_t type)
{
	struct upgrade_ctx *ctx = lctx->ctx;
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;

	assert(sizeof(struct ftl_nv_cache_chunk_md) == FTL_BLOCK_SIZE);

	/* Create the new NV cache metadata region - v2 */
	/* [한국어] verify 단계에서 사전에 만들어 둔 v2 region을 가져옴. */
	if (md_ops->region_open(dev, type, FTL_NVC_VERSION_2, sizeof(struct ftl_nv_cache_chunk_md),
				dev->layout.nvc.chunk_count, &ctx->reg_v2)) {
		return -1;
	}
	/* [한국어] v2 region 위에서 동작할 ftl_md 핸들 생성 (HEAP 모드 = in-memory 버퍼). */
	ctx->md_v2 = ftl_md_create(dev, ctx->reg_v2.current.blocks, 0, ctx->reg_v2.name, FTL_MD_CREATE_HEAP,
				   &ctx->reg_v2);
	if (!ctx->md_v2) {
		return -1;
	}

	ctx->md_v2->owner.cb_ctx = lctx;                   /* [한국어] persist_cb에서 lctx 복원용 */
	ctx->md_v2->cb = v1_to_v2_upgrade_md_cb;           /* [한국어] persist 완료 콜백 등록 */
	v1_to_v2_upgrade_set(lctx);                        /* [한국어] v2 메모리 버퍼를 빈 chunk md로 채움 */

	return 0;
}

/*
 * [한국어]
 * v1_to_v2_upgrade - chunk md v1 → v2 마이그레이션 진입점
 *
 * Major upgrade 보장 → v1 데이터 보존 불필요 → setup 후 바로 persist.
 *
 * 호출 체인:
 *   ftl_region_upgrade → desc.upgrade = v1_to_v2_upgrade
 *     → setup_ctx → ftl_md_persist → v1_to_v2_upgrade_md_cb → ftl_region_upgrade_completed
 */
static int
v1_to_v2_upgrade(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	/*
	 * Chunks at this point should be fully drained of user data (major upgrade). This means that it's safe to reinitialize
	 * the MD and fully change the structure layout (we're not interpreting the metadata contents at this point).
	 * Once we're done the version of the region in the superblock will be updated.
	 */

	if (v1_to_v2_upgrade_setup_ctx(dev, lctx, lctx->reg->type)) { /* [한국어] 컨텍스트 준비 */
		goto error;
	}
	ftl_md_persist(ctx->md_v2);                        /* [한국어] v2 데이터를 비동기 영속화 */
	return 0;

error:
	v1_to_v2_upgrade_cleanup(lctx);
	return -1;
}

/*
 * [한국어]
 * v1_to_v2_upgrade_enabled - verify: major upgrade 가능 + v2 region 사전 예약
 *
 * Band 업그레이드와 동일한 idempotent 패턴 — region_create 실패 시 region_open으로
 * 사전 작업이 이미 끝나 있는지 확인하고 통과시킨다.
 */
static int
v1_to_v2_upgrade_enabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;

	assert(sizeof(struct ftl_nv_cache_chunk_md) == FTL_BLOCK_SIZE);

	if (ftl_region_major_upgrade_enabled(dev, region)) { /* [한국어] major upgrade 전제 검사 */
		return -1;
	}

	/* Create the new NV cache metadata region (v2) up front - this allocates a separate entry in the superblock and
	 * area on the cache for us. This is to reserve space for other region upgrades allocating new regions and it
	 * allows us to do an atomic upgrade of the whole region.
	 *
	 * If the upgrade is stopped by power failure/crash after the V2 region has been added, then the upgrade process
	 * will start again (since V1 still exists), but region_create will fail (since the v2 region has already been
	 * created). In such a case only verification of the region length by region_open is needed.
	 *
	 * Once the upgrade is fully done, the old v1 region entry will be removed from the SB and its area on the cache
	 * freed.
	 */
	/* [한국어] 시도 1: v2 region 신규 생성. 이미 있으면 region_open으로 검증만. */
	if (md_ops->region_create(dev, region->type, FTL_NVC_VERSION_2, dev->layout.nvc.chunk_count) &&
	    md_ops->region_open(dev, region->type, FTL_NVC_VERSION_2, sizeof(struct ftl_nv_cache_chunk_md),
				dev->layout.nvc.chunk_count, NULL)) {
		return -1;
	}

	return 0;
}

/* [한국어] NVC chunk md 버전별 업그레이드 디스크립터 테이블.
 * v0: disabled. v1: → v2 마이그레이션. */
struct ftl_region_upgrade_desc nvc_upgrade_desc[] = {
	[FTL_NVC_VERSION_0] = {
		.verify = ftl_region_upgrade_disabled,     /* [한국어] v0 → 비활성 */
	},
	[FTL_NVC_VERSION_1] = {
		.verify = v1_to_v2_upgrade_enabled,
		.ctx_size = sizeof(struct upgrade_ctx),
		.new_version = FTL_NVC_VERSION_2,
		.upgrade = v1_to_v2_upgrade,
	},
};

/* [한국어] 디스크립터 수 = CURRENT 인덱스 — 새 버전 추가 시 표 갱신 강제. */
SPDK_STATIC_ASSERT(SPDK_COUNTOF(nvc_upgrade_desc) == FTL_NVC_VERSION_CURRENT,
		   "Missing NVC region upgrade descriptors");
