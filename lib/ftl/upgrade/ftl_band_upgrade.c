/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL band 메타데이터 v1 → v2 업그레이드 (ftl_band_upgrade.c)
 *
 * === 파일의 역할 ===
 * FTL의 영속 메타데이터 중 "band metadata"(각 base 디바이스 band의 상태/시퀀스/주소 정보)
 * 의 디스크 포맷을 한 버전에서 다음 버전으로 안전히 마이그레이션한다.
 * 현재 정의된 변경: v1 → v2 (struct ftl_band_md 앞쪽에 version 필드를 삽입하기 위해
 * 기존 데이터 전체를 sizeof(version)만큼 뒤로 시프트하고 새 version 필드를 채움).
 * 이 파일은 region 단위 업그레이드 디스크립터(`band_upgrade_desc[]`)와 그에 매달리는
 * verify/upgrade 콜백을 제공한다. 실제 호출은 ftl_layout_upgrade.c의 region_verify
 * /ftl_region_upgrade에서 수행된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (initialize 시 자동):
 *   ftl_layout_verify / ftl_region_upgrade
 *     → band_upgrade_desc[v1].verify = v1_to_v2_upgrade_enabled
 *     → band_upgrade_desc[v1].upgrade = v2_upgrade
 *       → ftl_md_create + ftl_md_restore (v1 데이터 read)
 *       → v2_upgrade_md_restore_cb (in-place 변환) → ftl_md_persist (v2 write)
 *       → v2_upgrade_md_persist_cb → ftl_region_upgrade_completed (SB 업데이트)
 *
 * === 타 모듈과의 연결 ===
 * - lib/ftl/upgrade/ftl_layout_upgrade.c : 디스크립터 호출 엔진
 * - lib/ftl/ftl_band.h                   : struct ftl_band_md / FTL_BAND_VERSION_*
 * - lib/ftl/ftl_md.{c,h}                 : ftl_md_create/restore/persist + 콜백
 * - lib/ftl/nvc/ftl_nvc_dev.h            : md_layout_ops.region_create/open
 * - dev->sb (superblock)                  : 업그레이드 atomicity (upgrade_ready 플래그)
 *
 * === 주요 함수/구조체 요약 ===
 * - struct upgrade_ctx                  : ftl_md*와 region 사본 보유
 * - v2_upgrade_cleanup / _finish        : ctx 정리와 상위 콜백 호출
 * - v2_upgrade_md_restore_cb            : v1 read 완료 → in-place 시프트 + version 부여
 * - v2_upgrade_md_persist_cb            : v2 write 완료 → finish
 * - v2_upgrade_setup_ctx                : v2 region open + ftl_md create
 * - v2_upgrade                          : 진입점 (verify 통과 후 실제 마이그레이션 시작)
 * - v1_to_v2_upgrade_enabled            : verify — major upgrade 가능성 + v2 region 사전 생성
 * - band_upgrade_desc                   : 버전별 디스크립터 테이블
 */

#include "ftl_band.h"             /* [한국어] struct ftl_band_md, FTL_BAND_VERSION_* */
#include "ftl_layout_upgrade.h"   /* [한국어] ftl_region_upgrade_desc / ctx / 헬퍼 */

/* [한국어] v2 업그레이드 1회당 필요한 컨텍스트.
 * lctx->ctx에 매달려 ftl_md 콜백 체인 동안 살아 있고, finish에서 정리된다. */
struct upgrade_ctx {
	struct ftl_md			*md;
	/* [한국어] v2 형식으로 IO 할 ftl_md 핸들.
	 * 설정자: v2_upgrade_setup_ctx (ftl_md_create)
	 * 해제자: v2_upgrade_cleanup (ftl_md_destroy)
	 * 사용처: restore→persist 콜백 체인 */

	struct ftl_layout_region	reg;
	/* [한국어] v2 region의 사본 (region_open 결과).
	 * persist 시 ftl_md_set_region에 넘겨 어디에 쓸지 결정. */
};

/*
 * [한국어]
 * v2_upgrade_cleanup - 업그레이드 컨텍스트의 ftl_md를 해제
 *
 * @lctx: 진행 중인 layout upgrade 컨텍스트
 *
 * 정상/실패 모두에서 호출 가능 — 이미 NULL이면 no-op. setup 단계가 실패한 직후나
 * finish 직전에 안전하게 호출하기 위해 idempotent 하게 만들어졌다.
 */
static void
v2_upgrade_cleanup(struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;               /* [한국어] 업그레이드 전용 컨텍스트 */

	if (ctx->md) {                                     /* [한국어] 이미 해제된 경우 스킵 */
		ftl_md_destroy(ctx->md, 0);                /* [한국어] ftl_md 자원 회수 (mode 0 = 기본) */
		ctx->md = NULL;                            /* [한국어] 재호출 안전을 위해 NULL */
	}
}

/*
 * [한국어]
 * v2_upgrade_finish - 업그레이드 종료 통지 (성공/실패 공통)
 *
 * @dev:    FTL 디바이스
 * @lctx:   업그레이드 컨텍스트
 * @status: 0 성공, !=0 실패 코드
 *
 * cleanup 후 ftl_region_upgrade_completed로 상위(layout_upgrade)에 결과를 보고.
 * entry_size/num_entries는 v2 region의 값을 그대로 전달해 dev->layout 갱신에 사용.
 */
static void
v2_upgrade_finish(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx, int status)
{
	struct upgrade_ctx *ctx = lctx->ctx;               /* [한국어] entry_size/num_entries 출처 */

	v2_upgrade_cleanup(lctx);                          /* [한국어] ftl_md 해제 */
	ftl_region_upgrade_completed(dev, lctx, ctx->reg.entry_size, ctx->reg.num_entries, status);
}

/*
 * [한국어]
 * v2_upgrade_md_persist_cb - v2 형식으로 영속화 완료 콜백
 *
 * @md:     영속화에 사용된 ftl_md (cb_ctx에 lctx 보관)
 * @status: 0 성공, !=0 실패
 *
 * v2 영속화가 끝나면 마이그레이션이 사실상 완료된 상태. finish로 SB 업데이트로 진행.
 */
static void
v2_upgrade_md_persist_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_layout_upgrade_ctx *lctx = md->owner.cb_ctx; /* [한국어] cb_ctx 복원 */

	v2_upgrade_finish(dev, lctx, status);              /* [한국어] 결과 보고 */
}

/*
 * [한국어]
 * v2_upgrade_md_restore_cb - v1 read 완료 → in-place로 v2 변환 → persist 트리거
 *
 * @md:     read에 사용된 ftl_md (= v1 데이터 보관 중)
 * @status: read 성공/실패
 *
 * 핵심 변환 로직:
 * 1) ftl_md_get_buffer로 in-memory 버퍼(= v1 데이터) 획득
 * 2) 각 ftl_band_md 항목마다:
 *    - memmove로 데이터를 sizeof(version) 만큼 뒤로 시프트
 *    - 시작 자리에 FTL_BAND_VERSION_2 기록
 *    - state가 CLOSED/FREE가 아닌 경우 (=in-flight) 변환 거부
 * 3) cb를 persist_cb로 바꾸고 region을 v2 region으로 갱신 후 ftl_md_persist
 *
 * Atomicity: 부분 persist 후 크래시 시에도 v1 region 데이터는 아직 SB에서 살아 있으므로
 * 재시작 시 다시 v1을 읽어 처음부터 마이그레이션을 반복 — idempotent.
 */
static void
v2_upgrade_md_restore_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_layout_upgrade_ctx *lctx = md->owner.cb_ctx; /* [한국어] cb_ctx 복원 */
	struct upgrade_ctx *ctx = lctx->ctx;
	struct ftl_band_md *band = ftl_md_get_buffer(md);  /* [한국어] in-memory band 배열 (v1 layout) */
	uint64_t move = sizeof(band->version);             /* [한국어] 시프트 크기 = version 필드 크기 */

	if (status) {                                      /* [한국어] read 실패 → 즉시 종료 */
		v2_upgrade_finish(dev, lctx, status);
		return;
	}

	/* If the upgrade process is interrupted while only part of the update persisted,
	 * then the V1 version will be read from again and this section will rewrite the whole band md.
	 */
	for (uint64_t i = 0; i < dev->num_bands; i++, band++) {
		char *buffer = (char *)band;               /* [한국어] 바이트 단위 시프트용 */

		/* [한국어] v1 → v2: 기존 내용을 뒤로 밀고 앞쪽 sizeof(version) 바이트에 버전 기록. */
		memmove(buffer + move, buffer, sizeof(*band) - move);
		band->version = FTL_BAND_VERSION_2;        /* [한국어] 새 버전 마커 */

		/* [한국어] 마이그레이션 가능 상태: CLOSED/FREE만 허용 (in-flight 데이터는 변환 불가). */
		if (band->state != FTL_BAND_STATE_CLOSED && band->state != FTL_BAND_STATE_FREE) {
			v2_upgrade_finish(dev, lctx, -EINVAL);
			return;
		}
	}

	/* [한국어] persist 단계로 전이 — 콜백/region 교체 후 ftl_md_persist 시작. */
	ctx->md->cb = v2_upgrade_md_persist_cb;
	ftl_md_set_region(ctx->md, &ctx->reg);             /* [한국어] v2 region으로 쓰기 위치 변경 */
	ftl_md_persist(ctx->md);                           /* [한국어] 비동기 쓰기 시작 */
}

/*
 * [한국어]
 * v2_upgrade_setup_ctx - v2 region open + ftl_md 생성
 *
 * @return: 0 성공, -1 실패
 *
 * v2 region을 open(=v1_to_v2_upgrade_enabled에서 미리 만들어 둔 region을 가져옴)하고,
 * v1 region 블록 수와 일치하는지 확인한 뒤 ftl_md를 v1 region 정보로 생성한다
 * (read는 v1 region에서 한다는 점에 주의).
 */
static int
v2_upgrade_setup_ctx(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;

	assert(sizeof(struct ftl_band_md) == FTL_BLOCK_SIZE); /* [한국어] 1 band md = 1 block 가정 */

	if (lctx->reg->num_entries != dev->num_bands) {    /* [한국어] v1 region 엔트리 수 검증 */
		return -1;
	}

	/* Open metadata region */
	/* [한국어] v1_to_v2_upgrade_enabled에서 사전에 등록된 v2 region을 가져옴. */
	if (md_ops->region_open(dev, lctx->reg->type, FTL_BAND_VERSION_2, sizeof(struct ftl_band_md),
				dev->num_bands, &ctx->reg)) {
		return -1;
	}

	if (lctx->reg->current.blocks != ctx->reg.current.blocks) { /* [한국어] 두 region 크기 일치 검증 */
		return -1;
	}

	/* [한국어] ftl_md는 v1 region 위치로 생성 — restore가 v1 데이터를 읽기 위함. */
	ctx->md = ftl_md_create(dev, lctx->reg->current.blocks, 0, ctx->reg.name, FTL_MD_CREATE_HEAP,
				lctx->reg);
	if (!ctx->md) {
		return -1;
	}

	ctx->md->owner.cb_ctx = lctx;                      /* [한국어] 콜백 복원용 */
	ctx->md->cb = v2_upgrade_md_restore_cb;            /* [한국어] read 완료 → in-place 변환 */

	return 0;
}

/*
 * [한국어]
 * v2_upgrade - v1 → v2 마이그레이션 진입점 (디스크립터의 .upgrade)
 *
 * @return: 0 성공(비동기 시작), -1 실패
 *
 * setup 후 ftl_md_restore(=v1 read)를 시작하면 콜백 체인이 자동 진행:
 *   restore → restore_cb (in-place 변환) → persist → persist_cb → finish
 *
 * 호출 체인:
 *   ftl_region_upgrade → desc.upgrade = v2_upgrade
 *     → v2_upgrade_setup_ctx → ftl_md_restore → … → ftl_region_upgrade_completed
 */
static int
v2_upgrade(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	if (v2_upgrade_setup_ctx(dev, lctx)) {             /* [한국어] 컨텍스트 준비 실패 → 정리 후 -1 */
		goto error;
	}
	/* At this point we're reading the contents of the v1 md */
	ftl_md_restore(ctx->md);                           /* [한국어] v1 데이터 read 시작 (비동기) */
	return 0;
error:
	v2_upgrade_cleanup(lctx);
	return -1;
}

/*
 * [한국어]
 * v1_to_v2_upgrade_enabled - v1 → v2 마이그레이션 가능성 verify
 *
 * @dev:    FTL 디바이스
 * @region: 현재 region 정보 (v1)
 * @return: 0 = 마이그레이션 가능 (또는 이미 사전 작업 완료), -1 = 불가
 *
 * 1) major upgrade가 허용된 상태(이전 종료가 깨끗했고 SB clean 등)인지 확인
 * 2) v2 region을 superblock에 사전 생성 — 다른 region 업그레이드들이 공간을 차지하기 전에
 *    이 region 자리를 미리 예약해 두기 위함. 이를 통해 한 region이 통째로 atomic
 *    하게 새 버전으로 교체될 수 있다.
 * 3) 이미 만들어져 있으면(중단 후 재시도 케이스) region_create가 실패하므로 region_open
 *    의 검증만 통과해도 OK로 간주.
 *
 * Power-fail 복구: V2 region이 SB에 추가된 직후 크래시 → 재시작 시 V1이 여전히 존재해
 * 같은 흐름이 다시 진입하고, region_create 실패는 정상 → region_open 검증으로 통과.
 */
static int
v1_to_v2_upgrade_enabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;

	if (ftl_region_major_upgrade_enabled(dev, region)) { /* [한국어] major upgrade 전제 조건 검사 */
		return -1;
	}

	/* Create the new band metadata region (v2) up front - this allocates a separate entry in the superblock and
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
	/* [한국어] 시도 1: 새 v2 region 생성. 실패하면 → 이미 존재 가능성 → region_open으로 확인. */
	if (md_ops->region_create(dev, region->type, FTL_BAND_VERSION_2, dev->num_bands) &&
	    md_ops->region_open(dev, region->type, FTL_BAND_VERSION_2, sizeof(struct ftl_band_md),
				dev->num_bands, NULL)) {
		return -1;
	}

	return 0;
}

/* [한국어] band region 버전별 업그레이드 디스크립터 테이블.
 * 인덱스는 "현재 버전". v0은 알 수 없는 옛 포맷이라 disabled.
 * v1은 verify로 v2 가능성을 검사하고 upgrade로 실제 마이그레이션을 수행. */
struct ftl_region_upgrade_desc band_upgrade_desc[] = {
	[FTL_BAND_VERSION_0] = {
		.verify = ftl_region_upgrade_disabled,     /* [한국어] v0에서 다음으로의 업그레이드는 비활성 */
	},
	[FTL_BAND_VERSION_1] = {
		.verify = v1_to_v2_upgrade_enabled,        /* [한국어] 가능성 검사 + v2 region 예약 */
		.ctx_size = sizeof(struct upgrade_ctx),    /* [한국어] lctx->ctx 할당 크기 */
		.new_version = FTL_BAND_VERSION_2,         /* [한국어] verify 통과 시 도달할 버전 */
		.upgrade = v2_upgrade,                     /* [한국어] 비동기 마이그레이션 진입점 */
	},
};

/* [한국어] 디스크립터 수가 CURRENT 버전 인덱스와 일치하는지 컴파일타임 검증.
 * 새 버전 추가 시 이 단언이 깨지면서 표 갱신을 잊지 않게 도와준다. */
SPDK_STATIC_ASSERT(SPDK_COUNTOF(band_upgrade_desc) == FTL_BAND_VERSION_CURRENT,
		   "Missing band region upgrade descriptors");
