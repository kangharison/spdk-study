/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL layout/region 업그레이드 엔진 (ftl_layout_upgrade.c)
 *
 * === 파일의 역할 ===
 * FTL 영속 메타데이터(SB / band md / NVC chunk md / P2L checkpoint / trim log)의
 * region별 버전 업그레이드를 일관된 스테이트 머신으로 수행한다. 각 region의 버전별
 * 변환 로직은 `*_upgrade_desc[]` 테이블(ftl_band_upgrade.c, ftl_chunk_upgrade.c 등)이
 * 정의하고, 이 파일은 region을 순회하며 verify → upgrade → completed 단계를 호출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (초기화 시):
 *   ftl_mngt 단계 → ftl_layout_verify
 *     → 모든 region에 대해 region_verify (version chain 검증)
 *   ftl_mngt 단계 → ftl_superblock_upgrade (SB만 동기적으로 업그레이드)
 *   ftl_mngt 단계 → 본 엔진의 step driver
 *     → ftl_layout_upgrade_init_ctx → layout_upgrade_select_next_region
 *     → ftl_region_upgrade(ctx) → desc.upgrade() (비동기)
 *     → 콜백 체인 끝에 ftl_region_upgrade_completed → SB 갱신 + 다음 region
 *
 * === 타 모듈과의 연결 ===
 * - lib/ftl/upgrade/ftl_band_upgrade.c        : band_upgrade_desc 정의
 * - lib/ftl/upgrade/ftl_chunk_upgrade.c       : nvc_upgrade_desc 정의
 * - lib/ftl/upgrade/ftl_p2l_upgrade.c         : p2l_upgrade_desc
 * - lib/ftl/upgrade/ftl_sb_upgrade.c          : sb_upgrade_desc
 * - lib/ftl/upgrade/ftl_trim_log_upgrade.c    : trim_log_upgrade_desc
 * - lib/ftl/ftl_layout.c / ftl_sb.c           : SB layout 메타 갱신
 * - lib/ftl/utils/ftl_layout_tracker_bdev.c   : region 등록/제거
 *
 * === 주요 함수/구조체 요약 ===
 * - ftl_region_major_upgrade_enabled / _enabled / _disabled : 공용 verify helper
 * - layout_upgrade_desc[]                                    : region_type별 업그레이드 표
 * - ftl_layout_upgrade_get_latest_version                    : 최신 버전 조회
 * - region_verify                                            : 버전 체인 따라 verify 호출
 * - ftl_region_upgrade                                       : 다음 버전으로 업그레이드 시작
 * - ftl_region_upgrade_completed                             : 업그레이드 종료 후 SB 반영
 * - ftl_layout_verify / ftl_upgrade_layout_dump              : 외부 진입점
 * - ftl_superblock_upgrade                                   : SB 동기 업그레이드 루프
 * - layout_upgrade_select_next_region                        : 다음 처리할 region 선택
 * - ftl_layout_upgrade_init_ctx                              : 첫 region(SB)부터 시작 셋업
 * - ftl_layout_upgrade_drop_region                           : 옛 버전 region을 SB에서 제거
 */

#include "spdk/assert.h"                          /* [한국어] SPDK_STATIC_ASSERT */

#include "ftl_layout_upgrade.h"                   /* [한국어] desc/ctx/공용 helper 선언 */
#include "ftl_layout.h"                           /* [한국어] ftl_layout_region / type enum */
#include "ftl_sb_current.h"                       /* [한국어] 현재 SB 포맷 */
#include "ftl_sb_prev.h"                          /* [한국어] 이전 SB 포맷 (마이그레이션용) */
#include "ftl_core.h"                             /* [한국어] spdk_ftl_dev */
#include "ftl_band.h"                             /* [한국어] band 관련 타입 */
#include "utils/ftl_layout_tracker_bdev.h"        /* [한국어] 영역 추적기 (drop_region용) */

/*
 * [한국어]
 * ftl_region_major_upgrade_enabled - major upgrade 가능성 검증 helper
 *
 * @return: 0 가능, -1 불가
 *
 * "Major" 업그레이드 = 영속 데이터 구조 자체가 바뀌어 사용자 데이터가 깨끗이 비어 있어야
 * 진행 가능한 케이스. 일반 region_upgrade_enabled 검사(SB clean) + dev->sb->upgrade_ready
 * 플래그(이전 버전이 종료 시 "다음에 major upgrade 해도 됨"을 표시) 모두 통과해야 OK.
 */
int
ftl_region_major_upgrade_enabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	if (ftl_region_upgrade_enabled(dev, region)) {     /* [한국어] 기본 SB clean 검사 */
		return -1;
	}

	if (dev->sb->upgrade_ready) {                      /* [한국어] 이전 버전이 명시적으로 허용했는지 */
		return 0;
	} else {
		FTL_ERRLOG(dev, "FTL major upgrade ERROR, required upgrade shutdown in the previous version\n");
		return -1;
	}
}

/*
 * [한국어]
 * ftl_region_upgrade_disabled - 업그레이드 비활성 verify (placeholder)
 *
 * 항상 -1을 반환 — 어떤 버전에서 다음 버전으로의 업그레이드가 명시적으로 막힌 경우
 * 디스크립터의 .verify로 등록한다 (예: VERSION_0 같은 알 수 없는 옛 버전).
 */
int
ftl_region_upgrade_disabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	return -1;
}

/*
 * [한국어]
 * ftl_region_upgrade_enabled - 일반 업그레이드 가능성 검증
 *
 * @return: 0 SB가 clean하고 shm 미클린 = 안전, -1 dirty
 *
 * SB가 dirty면 영속 메타가 크래시 시점에서 일관되지 않은 가능성이 있어 업그레이드
 * 위험 → 거부. shm_clean==0은 마운트 직후 정상 상태.
 */
int
ftl_region_upgrade_enabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	if (!(dev->sb->clean == 1 && dev->sb_shm->shm_clean == 0)) { /* [한국어] SB clean 조건 */
		FTL_ERRLOG(dev, "FTL region upgrade: SB dirty\n");
		return -1;
	}
	return 0;
}

#ifndef UTEST
/* [한국어] 각 region 카테고리별 버전 디스크립터 테이블 — 다른 .c 파일에서 정의.
 * 이 파일은 layout_upgrade_desc[]를 통해 region_type → 적절한 desc 표를 매핑한다. */
extern struct ftl_region_upgrade_desc sb_upgrade_desc[];        /* [한국어] SB 버전 표 */
extern struct ftl_region_upgrade_desc p2l_upgrade_desc[];       /* [한국어] P2L checkpoint 버전 표 */
extern struct ftl_region_upgrade_desc nvc_upgrade_desc[];       /* [한국어] NVC chunk md 버전 표 */
extern struct ftl_region_upgrade_desc band_upgrade_desc[];      /* [한국어] Band md 버전 표 */
extern struct ftl_region_upgrade_desc trim_log_upgrade_desc[];  /* [한국어] Trim log 버전 표 */

/* [한국어] region_type → upgrade desc 표 매핑.
 * 빈 슬롯 {}는 "이 region은 업그레이드 대상이 아님"을 의미 — 데이터 영역 등.
 * 두 미러 region은 본체와 동일한 desc 표를 공유 (SB_BASE/MD_MIRROR/TRIM 미러 등). */
static struct ftl_layout_upgrade_desc_list layout_upgrade_desc[] = {
	[FTL_LAYOUT_REGION_TYPE_SB] = {
		.latest_ver = FTL_SB_VERSION_CURRENT,
		.count = FTL_SB_VERSION_CURRENT,
		.desc = sb_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_SB_BASE] = {
		.latest_ver = FTL_SB_VERSION_CURRENT,
		.count = FTL_SB_VERSION_CURRENT,
		.desc = sb_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_L2P] = {},
	[FTL_LAYOUT_REGION_TYPE_BAND_MD] = {
		.latest_ver = FTL_BAND_VERSION_CURRENT,
		.count = FTL_BAND_VERSION_CURRENT,
		.desc = band_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR] = {
		.latest_ver = FTL_BAND_VERSION_CURRENT,
		.count = FTL_BAND_VERSION_CURRENT,
		.desc = band_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_VALID_MAP] = {},
	[FTL_LAYOUT_REGION_TYPE_NVC_MD] = {
		.latest_ver = FTL_NVC_VERSION_CURRENT,
		.count = FTL_NVC_VERSION_CURRENT,
		.desc = nvc_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR] = {
		.latest_ver = FTL_NVC_VERSION_CURRENT,
		.count = FTL_NVC_VERSION_CURRENT,
		.desc = nvc_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_DATA_NVC] = {},
	[FTL_LAYOUT_REGION_TYPE_DATA_BASE] = {},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC] = {
		.latest_ver = FTL_P2L_VERSION_CURRENT,
		.count = FTL_P2L_VERSION_CURRENT,
		.desc = p2l_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC_NEXT] = {
		.latest_ver = FTL_P2L_VERSION_CURRENT,
		.count = FTL_P2L_VERSION_CURRENT,
		.desc = p2l_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP] = {
		.latest_ver = FTL_P2L_VERSION_CURRENT,
		.count = FTL_P2L_VERSION_CURRENT,
		.desc = p2l_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP_NEXT] = {
		.latest_ver = FTL_P2L_VERSION_CURRENT,
		.count = FTL_P2L_VERSION_CURRENT,
		.desc = p2l_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_TRIM_MD] = {},
	[FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR] = {},
	[FTL_LAYOUT_REGION_TYPE_TRIM_LOG] = {
		.latest_ver = FTL_TRIM_LOG_VERSION_CURRENT,
		.count = FTL_TRIM_LOG_VERSION_CURRENT,
		.desc = trim_log_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_TRIM_LOG_MIRROR] = {
		.latest_ver = FTL_TRIM_LOG_VERSION_CURRENT,
		.count = FTL_TRIM_LOG_VERSION_CURRENT,
		.desc = trim_log_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MIN] = {},
	[FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MAX] = {},
};

/* [한국어] 표 크기 = region_type 개수 검증 — 새 region_type 추가 시 표 갱신 강제. */
SPDK_STATIC_ASSERT(sizeof(layout_upgrade_desc) / sizeof(*layout_upgrade_desc) ==
		   FTL_LAYOUT_REGION_TYPE_MAX,
		   "Missing layout upgrade descriptors");
#endif

/*
 * [한국어]
 * ftl_layout_upgrade_get_latest_version - region_type의 최신 버전 조회
 *
 * 외부에서 "이 region은 최종적으로 어느 버전까지 가야 하는가?"를 알고 싶을 때 사용.
 */
uint64_t
ftl_layout_upgrade_get_latest_version(enum ftl_layout_region_type reg_type)
{
	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);
	return layout_upgrade_desc[reg_type].latest_ver;
}

/*
 * [한국어]
 * region_verify - 현재 region 버전부터 최신 버전까지 verify 체인 호출 (static)
 *
 * @return: 0 = 모든 버전 가능, !=0 = 어느 단계에서 실패
 *
 * verify는 부수효과 없는 검증이 아니라 v2 region 사전 생성 등 사이드 이펙트가
 * 있을 수 있다 (각 .verify 구현 참고). 따라서 이 함수가 끝나면 SB에는 향후
 * 업그레이드에 필요한 새 region 슬롯이 미리 등록되어 있는 상태가 된다.
 */
static int
region_verify(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	uint64_t ver;

	assert(ctx->reg);
	ver = ctx->reg->current.version;                   /* [한국어] 현재 디스크 버전 */
	if (ver > ctx->upgrade->latest_ver) {              /* [한국어] 미래 버전 = 알 수 없음 */
		FTL_ERRLOG(dev, "Unknown region version\n");
		return -1;
	}

	while (ver < ctx->upgrade->latest_ver) {           /* [한국어] 최신까지 한 단계씩 verify */
		int rc = ctx->upgrade->desc[ver].verify(dev, ctx->reg);
		if (rc) {
			return rc;                         /* [한국어] 한 단계라도 실패 → 전체 실패 */
		}
		ftl_bug(ver > ctx->upgrade->desc[ver].new_version);     /* [한국어] 버전 후퇴 금지 */
		ftl_bug(ctx->upgrade->desc[ver].new_version > ctx->upgrade->latest_ver); /* [한국어] 미래 도약 금지 */
		ver = ctx->upgrade->desc[ver].new_version; /* [한국어] 다음 버전으로 진행 */
	}
	return 0;
}

/*
 * [한국어]
 * ftl_region_upgrade - 현재 버전을 한 단계 업그레이드 시작
 *
 * @return: 0 시작 성공(또는 이미 최신), !=0 실패
 *
 * desc.upgrade는 일반적으로 비동기 — 이 함수는 시작만 하고, 완료 시 desc.upgrade가
 * ftl_region_upgrade_completed를 호출해 결과를 보고한다. ctx->next_reg_ver에 다음
 * 버전을 미리 적어 두어 completed에서 SB 갱신에 사용한다.
 */
int
ftl_region_upgrade(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	int rc = 0;
	uint64_t ver;

	assert(ctx->reg);
	assert(ctx->reg->current.version <= ctx->upgrade->latest_ver);
	ver = ctx->reg->current.version;
	if (ver < ctx->upgrade->latest_ver) {              /* [한국어] 아직 최신 아님 — 한 단계 진행 */
		ctx->next_reg_ver = ctx->upgrade->desc[ver].new_version;
		rc = ctx->upgrade->desc[ver].upgrade(dev, ctx); /* [한국어] 비동기 마이그레이션 시작 */
	}
	return rc;
}

/*
 * [한국어]
 * ftl_region_upgrade_completed - 한 단계 업그레이드 완료 후처리
 *
 * @entry_size, num_entries: 새 버전의 entry 사이즈/개수 (dev->layout 갱신용, 0이면 미변경)
 * @status: 0 성공, !=0 실패
 *
 * 성공 시:
 * 1) (SB 외 region) ftl_superblock_md_layout_upgrade_region으로 SB의 region 버전 갱신
 * 2) entry_size/num_entries 변경 사항을 dev->layout.region에 반영
 * 3) ctx->reg->current.version = next_reg_ver 로 in-memory 버전 진행
 * 그 후 등록된 비동기 콜백(ctx->cb)이 있으면 호출 (다음 region 처리로 진행).
 *
 * 실패 시: in-memory 상태는 그대로 두고 콜백만 호출 → 호출자가 적절한 에러 처리.
 */
void
ftl_region_upgrade_completed(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx,
			     uint64_t entry_size, uint64_t num_entries, int status)
{
	int rc;

	assert(ctx->reg);
	assert(ctx->reg->current.version < ctx->next_reg_ver);
	assert(ctx->next_reg_ver <= ctx->upgrade->latest_ver);

	if (!status) {                                     /* [한국어] 성공 케이스만 SB/layout 갱신 */
		if (ctx->reg->type != FTL_LAYOUT_REGION_TYPE_SB) {
			/* Superblock region is always default-created in the latest version - see ftl_layout_setup_superblock() */
			/* [한국어] SB region은 항상 최신으로 자동 생성되므로 SB 자체는 layout 갱신 불요. */
			rc = ftl_superblock_md_layout_upgrade_region(dev, ctx->reg, ctx->next_reg_ver);
			if (entry_size && num_entries) {   /* [한국어] entry 메타가 바뀌었으면 갱신 */
				dev->layout.region[ctx->reg->type].entry_size = entry_size;
				dev->layout.region[ctx->reg->type].num_entries = num_entries;
			}

			ftl_bug(rc != 0);                  /* [한국어] SB 갱신 실패는 복구 불가 */
		}

		ctx->reg->current.version = ctx->next_reg_ver; /* [한국어] in-memory 버전 진행 */
	}

	if (ctx->cb) {
		ctx->cb(dev, ctx->cb_ctx, status);         /* [한국어] 비동기 후속 단계 트리거 */
	}
}

/*
 * [한국어]
 * ftl_layout_verify - 모든 region을 순회하며 verify 체인 수행
 *
 * @return: 0 모두 OK, -1 실패
 *
 * 초기화 시 SB 업그레이드 직후에 호출되어, region별로 현재 버전이 최신까지 도달
 * 가능한지 사전 검증한다. 부수효과로 새 버전 region 슬롯 사전 등록도 수행됨.
 */
int
ftl_layout_verify(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_upgrade_ctx ctx = {0};           /* [한국어] 빈 컨텍스트 (per-region 재사용) */
	enum ftl_layout_region_type reg_type;

	/**
	 * Upon SB upgrade some MD regions may be missing in the MD layout blob - e.g. v3 to v5, FTL_LAYOUT_REGION_TYPE_DATA_BASE.
	 * The regions couldn't have be added in the SB upgrade path, as the FTL layout wasn't initialized at that point.
	 * Now that the FTL layout is initialized, add the missing regions and store the MD layout blob again.
	 */

	if (ftl_validate_regions(dev, layout)) {           /* [한국어] 영역 무결성 사전 검사 */
		return -1;
	}

	for (reg_type = 0; reg_type < FTL_LAYOUT_REGION_TYPE_MAX; reg_type++) {
		ctx.reg = ftl_layout_region_get(dev, reg_type);
		ctx.upgrade = &layout_upgrade_desc[reg_type];
		if (!ctx.reg) {                            /* [한국어] 사용 안 하는 region은 스킵 */
			continue;
		}

		if (region_verify(dev, &ctx)) {            /* [한국어] 버전 체인 verify */
			return -1;
		}
	}

	return 0;
}

/*
 * [한국어]
 * ftl_upgrade_layout_dump - layout 검증 후 디버그 덤프
 *
 * 운영자가 현재 SB의 layout 메타를 확인하고 싶을 때 사용.
 */
int
ftl_upgrade_layout_dump(struct spdk_ftl_dev *dev)
{
	if (ftl_validate_regions(dev, &dev->layout)) {     /* [한국어] 무결성 검사 */
		return -1;
	}

	ftl_layout_dump(dev);                              /* [한국어] in-memory layout 출력 */
	ftl_superblock_md_layout_dump(dev);                /* [한국어] SB의 layout 블롭 출력 */
	return 0;
}

/*
 * [한국어]
 * ftl_superblock_upgrade - SB region을 최신 버전까지 동기적으로 업그레이드
 *
 * SB는 다른 region과 달리 동기적으로 처리한다 — SB가 마이그레이션되어야 다른 region의
 * layout 정보를 정확히 알 수 있기 때문이다. 따라서 desc.upgrade는 동기 함수만 사용된다.
 *
 * SB_BASE는 같은 DMA 버퍼를 공유하므로 SB store 시 자동 미러됨 — 마지막에 in-memory
 * 버전만 동기화해 주면 된다.
 */
int
ftl_superblock_upgrade(struct spdk_ftl_dev *dev)
{
	struct ftl_layout_upgrade_ctx ctx = {0};
	struct ftl_layout_region *reg = ftl_layout_region_get(dev, FTL_LAYOUT_REGION_TYPE_SB);
	int rc;

	ctx.reg = reg;
	ctx.upgrade = &layout_upgrade_desc[FTL_LAYOUT_REGION_TYPE_SB];
	reg->current.version = dev->sb->header.version;    /* [한국어] 디스크 SB의 실제 버전 반영 */

	rc = region_verify(dev, &ctx);                     /* [한국어] 가능성 사전 검증 */
	if (rc) {
		return rc;
	}

	while (reg->current.version < ctx.upgrade->latest_ver) {
		rc = ftl_region_upgrade(dev, &ctx);        /* [한국어] 한 단계 업그레이드 (동기) */
		if (rc) {
			return rc;
		}
		/* SB upgrades are all synchronous */
		ftl_region_upgrade_completed(dev, &ctx, 0, 0, rc); /* [한국어] 즉시 완료 처리 */
	}

	/* The mirror shares the same DMA buf, so it is automatically updated upon SB store */
	dev->layout.region[FTL_LAYOUT_REGION_TYPE_SB_BASE].current.version = reg->current.version;
	return 0;
}

/*
 * [한국어]
 * layout_upgrade_select_next_region - 다음 처리할 region 선택 (스테이트 머신 보조)
 *
 * @return: FTL_LAYOUT_UPGRADE_CONTINUE / DONE / FAULT
 *
 * 현재 region이 최신이거나 INVALID면 다음 region으로 진행, 아직 옛 버전이면 처리할
 * 것이 있다는 의미로 CONTINUE 반환. 미래 버전이 발견되면 FAULT.
 */
static int
layout_upgrade_select_next_region(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	struct ftl_layout_region *reg;
	uint64_t reg_ver, reg_latest_ver;
	uint32_t reg_type = ctx->reg->type;

	while (reg_type != FTL_LAYOUT_REGION_TYPE_MAX) {
		assert(ctx->reg);
		assert(ctx->upgrade);
		reg = ctx->reg;
		reg_latest_ver = ctx->upgrade->latest_ver;
		reg_ver = reg->current.version;

		if (reg_ver == reg_latest_ver || reg->type == FTL_LAYOUT_REGION_TYPE_INVALID) {
			/* select the next region to upgrade */
			/* [한국어] 처리할 게 없는 region — 다음으로 이동. */
			reg_type++;
			if (reg_type == FTL_LAYOUT_REGION_TYPE_MAX) {
				break;
			}
			ctx->reg++;                        /* [한국어] dev->layout.region 배열을 직접 전진 */
			ctx->upgrade++;                    /* [한국어] 표도 동일한 인덱스로 전진 */
		} else if (reg_ver < reg_latest_ver) {
			/* qualify region version to upgrade */
			/* [한국어] 옛 버전 — 호출자가 ftl_region_upgrade를 진행해야 함. */
			return FTL_LAYOUT_UPGRADE_CONTINUE;
		} else {
			/* unknown version */
			/* [한국어] 미래 버전이면 이 코드보다 새 FTL이 만든 데이터 — 진행 불가. */
			assert(reg_ver <= reg_latest_ver);
			FTL_ERRLOG(dev, "Region %d upgrade fault: version %"PRIu64"/%"PRIu64"\n", reg_type, reg_ver,
				   reg_latest_ver);
			return FTL_LAYOUT_UPGRADE_FAULT;
		}
	}

	return FTL_LAYOUT_UPGRADE_DONE;                    /* [한국어] 끝까지 도달 = 모두 최신 */
}

/*
 * [한국어]
 * ftl_layout_upgrade_init_ctx - 첫 호출 시 ctx를 SB region에서 시작하도록 셋업
 *
 * 이후 호출에서는 ctx가 살아 있어 다음 region 선택만 수행. ftl_mngt 단계에서 step
 * 진입 직후 호출되어 처리할 region이 더 있는지 결정한다.
 */
int
ftl_layout_upgrade_init_ctx(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	if (!ctx->reg) {
		ctx->reg = ftl_layout_region_get(dev, 0);  /* [한국어] index 0 = SB region */
		ctx->upgrade = &layout_upgrade_desc[0];
		SPDK_STATIC_ASSERT(FTL_LAYOUT_REGION_TYPE_SB == 0, "Invalid SB region type");
	}

	return layout_upgrade_select_next_region(dev, ctx);
}

/*
 * [한국어]
 * ftl_layout_upgrade_region_get_latest_version - 위 함수와 동일 (래퍼)
 *
 * 동일 기능의 함수가 두 이름으로 노출되는 이유는 외부 헤더의 호환성 — 사용처가
 * 어느 한 이름을 쓰면 둘 다 호환되도록 보장한다.
 */
uint64_t
ftl_layout_upgrade_region_get_latest_version(enum ftl_layout_region_type reg_type)
{
	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);
	return layout_upgrade_desc[reg_type].latest_ver;
}

/*
 * [한국어]
 * ftl_layout_upgrade_drop_region - 옛 버전 region을 SB layout 추적기에서 제거
 *
 * @layout_tracker: nvc 또는 base bdev의 layout tracker
 * @reg_type, reg_ver: 제거할 region 식별자
 * @return: 0 성공, -1 다른 같은 type region이 남아 있어 제거 불완전
 *
 * v2 마이그레이션이 끝난 뒤 v1 region을 정리할 때 사용. 제거 후 동일 type이 추적기에
 * 남아 있으면 inconsistency — 에러 반환. 모두 사라졌으면 dev->layout.region[]도
 * INVALID로 만들어 in-memory 슬롯도 정리한다.
 */
int
ftl_layout_upgrade_drop_region(struct spdk_ftl_dev *dev,
			       struct ftl_layout_tracker_bdev *layout_tracker,
			       enum ftl_layout_region_type reg_type, uint32_t reg_ver)
{
	const struct ftl_layout_tracker_bdev_region_props *reg_search_ctx = NULL;
	int rc = ftl_layout_tracker_bdev_rm_region(layout_tracker, reg_type, reg_ver); /* [한국어] tracker에서 제거 시도 */

	/* [한국어] 같은 type의 다른 버전이 더 남아 있는지 확인 — 남아 있으면 inconsistent. */
	ftl_layout_tracker_bdev_find_next_region(layout_tracker, reg_type, &reg_search_ctx);
	if (reg_search_ctx) {
		FTL_ERRLOG(dev,
			   "Error when dropping region type %"PRId32", ver %"PRIu32": rc:%"PRId32" but found reg ver %"PRIu32"\n",
			   reg_type, reg_ver, rc, reg_search_ctx->ver);
		return -1;
	}
	dev->layout.region[reg_type].type = FTL_LAYOUT_REGION_TYPE_INVALID; /* [한국어] in-memory 슬롯 무효화 */
	return 0;
}
