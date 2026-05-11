/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL의 base/cache bdev 열기/닫기 단계 (ftl_mngt_bdev.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 FTL 디바이스가 사용하는 두 종류의 SPDK bdev — (1) base bdev (대용량 데이터
 * 저장용 NVMe SSD), (2) NV cache bdev (write 캐시용 빠른 영속 디바이스, 예: PMem 또는
 * 작은 NVMe) — 를 startup 단계에서 열고(claim) 셧다운 단계에서 닫는 step 함수들을
 * 제공한다. bdev 자체는 SPDK bdev 레이어가 관리하지만, FTL은 추가로 bdev_module_claim_bdev로
 * 독점 소유권을 잡고, 블록 크기/총 블록/zoned 여부/MD 크기/xfer_size 등을 검증한 뒤
 * dev 구조체와 layout tracker를 셋업한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 컨텍스트: 모두 FTL 코어 spdk_thread (startup/shutdown 파이프라인 step).
 *   호출 체인 (startup):
 *     ftl_mngt_call_dev_startup → ... → ftl_mngt_open_base_bdev →
 *       spdk_bdev_open_ext + claim + 검증 + io_channel + layout_tracker_init →
 *       ftl_mngt_next_step
 *     ... → ftl_mngt_open_cache_bdev → 동일 패턴 → next_step
 *   호출 체인 (shutdown — rollback_device 안에서 cleanup으로 호출):
 *     ftl_mngt_close_cache_bdev → spdk_put_io_channel + module_release + close + tracker_fini
 *     ftl_mngt_close_base_bdev  → 동일
 *
 * === 타 모듈과의 연결 ===
 * - 의존: SPDK bdev 레이어 (spdk_bdev_open_ext, spdk_bdev_module_claim_bdev,
 *   spdk_bdev_get_io_channel, spdk_bdev_get_block_size/num_blocks/md_size,
 *   spdk_bdev_is_zoned 등), lib/ftl/utils/ftl_layout_tracker_bdev.[ch] (region 할당
 *   추적), lib/ftl/ftl_nv_cache.[ch] (NV cache 디바이스 타입 카탈로그),
 *   lib/ftl/base/ (base 디바이스 타입 ops).
 * - 의존받음: ftl_mngt_startup.c (open step), ftl_mngt_misc.c (rollback 시 close 호출).
 * - 데이터 흐름: bdev 메타정보(블록 수, MD 크기) → dev->{xfer_size, num_blocks_in_band,
 *   md_size, is_zoned, base_type, nv_cache.nvc_type, layout_tracker} 까지 채워서
 *   이후 layout/recovery/IO 단계가 사용.
 *
 * === 주요 함수/구조체 요약 ===
 * - g_ftl_bdev_module       : bdev 소유권 claim용 더미 module (이름만 "ftl_lib").
 * - ftl_calculate_num_blocks_in_band(): 밴드 크기(블록 수) 결정 — 빌드 옵션 또는 1GiB.
 * - base_bdev_event_cb()    : base bdev 이벤트 콜백 (REMOVE 시 abort).
 * - ftl_mngt_open_base_bdev(): base bdev 열기/검증/io_channel/layout tracker 초기화.
 * - ftl_mngt_close_base_bdev(): 위의 역순 정리.
 * - nv_cache_bdev_event_cb(): cache bdev 이벤트 콜백 (REMOVE 시 abort).
 * - ftl_mngt_open_cache_bdev(): cache bdev 열기/검증/tracker 초기화.
 * - ftl_mngt_close_cache_bdev(): 위의 역순 정리.
 */

#include "spdk/bdev_module.h"
/* [한국어] spdk_bdev_module(claim용), spdk_bdev_open_ext, spdk_bdev_get_* 등 bdev API. */
#include "spdk/ftl.h"
/* [한국어] SPDK FTL 공개 API — spdk_ftl_conf 등. */

#include "ftl_nv_cache.h"
/* [한국어] ftl_nv_cache 구조체 및 ftl_nv_cache_device_get_type_by_bdev. */
#include "ftl_internal.h"
/* [한국어] FTL 내부 정의 (ftl_get_write_unit_size 등). */
#include "ftl_mngt_steps.h"
/* [한국어] step 프로토타입. */
#include "ftl_core.h"
/* [한국어] spdk_ftl_dev 정의. */
#include "utils/ftl_defs.h"
/* [한국어] FTL_BLOCK_SIZE, GiB 매크로 등. */
#include "utils/ftl_layout_tracker_bdev.h"
/* [한국어] ftl_layout_tracker_bdev_init/fini — bdev 내 region 할당 추적. */

#define MINIMUM_CACHE_SIZE_GIB 5
/* [한국어] NV cache bdev 최소 크기 (5 GiB). 미만이면 fail. write buffer로 너무 작으면
 * NAND flush 압박이 심해 GC 효율 저하. */
#define MINIMUM_BASE_SIZE_GIB 20
/* [한국어] base bdev 최소 크기 (20 GiB). 너무 작으면 메타데이터 영역과 사용자 데이터
 * 영역을 겹치지 않게 layout 잡기가 어려움. */

/*  Dummy bdev module used to to claim bdevs. */
/*
 * [한국어] FTL이 bdev를 독점 소유함을 SPDK bdev 레이어에 알리기 위한 더미 모듈.
 * spdk_bdev_module_claim_bdev에 본 모듈 포인터를 넘기면 다른 모듈/사용자가 같은
 * bdev를 다시 open하려 할 때 차단됨(EPERM). FTL 종료 시 release_bdev로 풀어줌.
 */
static struct spdk_bdev_module g_ftl_bdev_module = {
	.name   = "ftl_lib",
	/* [한국어] 모듈 식별 이름 — claim 충돌 시 로그에 표시됨. */
};

/*
 * [한국어]
 * ftl_calculate_num_blocks_in_band - 한 밴드의 블록 수 결정.
 *
 * @desc: base bdev desc (현재 사용 안 함 — 향후 인자화 예정 TODO).
 * @return: 한 밴드의 블록 수. ZONE 에뮬 빌드면 SPDK_FTL_ZONE_EMU_BLOCKS, 아니면 1 GiB / FTL_BLOCK_SIZE.
 *
 * 왜 이렇게 결정하는가: NAND의 erase block(super-block)에 가까운 단위로 묶어 GC 효율을
 * 높임. SPDK FTL 기본은 1 GiB(=262144 4KiB block)로 고정. ZNS 에뮬레이션 빌드는
 * 빌드 시 매크로로 강제.
 */
static inline uint64_t
ftl_calculate_num_blocks_in_band(struct spdk_bdev_desc *desc)
{
	/* TODO: this should be passed via input parameter */
#ifdef SPDK_FTL_ZONE_EMU_BLOCKS
	return SPDK_FTL_ZONE_EMU_BLOCKS;
	/* [한국어] ZNS 에뮬레이션 빌드 — 빌드 시 정의한 zone size 그대로 사용. */
#else
	return (1ULL << 30) / FTL_BLOCK_SIZE;
	/* [한국어] 기본값: 1 GiB / 4 KiB = 262144 블록. */
#endif
}

/*
 * [한국어]
 * base_bdev_event_cb - base bdev에서 이벤트(REMOVE/RESIZE 등) 발생 시 호출.
 *
 * 현재 구현은 REMOVE에 대해 abort — 실제 운영 환경에서 핫언플러그를 안전하게 처리하지
 * 못함 (TODO). 다른 이벤트는 무시.
 */
static void
base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		assert(0);
		/* [한국어] base 디바이스 핫언플러그는 미지원 — abort. */
		break;
	default:
		break;
		/* [한국어] 다른 이벤트(RESIZE, MEDIA_MANAGEMENT 등)는 silently ignore. */
	}
}

/*
 * [한국어]
 * ftl_mngt_open_base_bdev - base NVMe bdev를 열고 dev 구조체에 메타 셋업.
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 동작:
 *  1) spdk_bdev_open_ext로 dev->conf.base_bdev 이름의 bdev 열기 (write enable).
 *  2) spdk_bdev_module_claim_bdev로 g_ftl_bdev_module 명의 독점 소유.
 *  3) block_size 검증 (== FTL_BLOCK_SIZE 4KiB).
 *  4) 총 용량 검증 (>= MINIMUM_BASE_SIZE_GIB).
 *  5) base I/O channel 획득 (코어 스레드 내 호출이므로 codec thread 별 채널은 추후).
 *  6) xfer_size = ftl_get_write_unit_size — NAND write granularity. 2의 거듭제곱이어야 함.
 *  7) base_type = ftl_base_device_get_type_by_bdev — dev 종류별 ops 카탈로그 룩업.
 *  8) md_size = bdev metadata size (per-block metadata).
 *  9) num_blocks_in_band, is_zoned 캐시. zoned이면 fail (현재 미지원).
 *  10) layout_tracker_bdev_init — 밴드/MD region 할당 추적자 생성.
 *  11) next_step.
 *
 * 실패는 어느 단계든 goto error → fail_step. 부분 자원은 ftl_mngt_close_base_bdev가
 * NULL 체크하며 안전하게 정리.
 */
void
ftl_mngt_open_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	uint32_t block_size;
	uint64_t num_blocks;
	const char *bdev_name = dev->conf.base_bdev;
	/* [한국어] 사용자가 RPC/conf로 지정한 base bdev 이름. */
	struct spdk_bdev *bdev;

	if (spdk_bdev_open_ext(bdev_name, true, base_bdev_event_cb,
			       dev, &dev->base_bdev_desc)) {
		/* [한국어] open 실패 — bdev가 존재하지 않거나 다른 모듈이 점유 중. */
		FTL_ERRLOG(dev, "Unable to open bdev: %s\n", bdev_name);
		goto error;
	}

	bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
	/* [한국어] desc로부터 spdk_bdev 객체 추출 (메타 정보 조회용). */

	if (spdk_bdev_module_claim_bdev(bdev, dev->base_bdev_desc, &g_ftl_bdev_module)) {
		/* clear the desc so that we don't try to release the claim on cleanup */
		/* [한국어] claim 실패(이미 다른 모듈이 점유). desc를 NULL로 clear해서
		 * close_base_bdev가 release_bdev를 안 부르게 함 — 우리가 claim한 적 없음. */
		spdk_bdev_close(dev->base_bdev_desc);
		dev->base_bdev_desc = NULL;
		FTL_ERRLOG(dev, "Unable to claim bdev %s\n", bdev_name);
		goto error;
	}

	block_size = spdk_bdev_get_block_size(bdev);
	if (block_size != FTL_BLOCK_SIZE) {
		/* [한국어] FTL은 4KiB 블록만 지원. 그 외 크기는 fail. */
		FTL_ERRLOG(dev, "Unsupported block size (%"PRIu32")\n", block_size);
		goto error;
	}

	num_blocks = spdk_bdev_get_num_blocks(bdev);

	if (num_blocks * block_size < MINIMUM_BASE_SIZE_GIB * GiB) {
		/* [한국어] 디바이스 용량이 최소 요건 미만 — layout 불가. */
		FTL_ERRLOG(dev, "Bdev %s is too small, requires, at least %uGiB capacity\n",
			   spdk_bdev_get_name(bdev), MINIMUM_BASE_SIZE_GIB);
		goto error;
	}

	dev->base_ioch = spdk_bdev_get_io_channel(dev->base_bdev_desc);
	/* [한국어] base bdev에 I/O 발행할 코어 스레드용 채널 획득. */
	if (!dev->base_ioch) {
		FTL_ERRLOG(dev, "Failed to create base bdev IO channel\n");
		goto error;
	}

	dev->xfer_size = ftl_get_write_unit_size(bdev);
	/* [한국어] NAND 쓰기 단위(예: 192 KiB = 48 4KB block) — 한 번 write로 PMU(program management unit)에
	 * 묶어 보낼 블록 수. 작으면 throughput ↓, 크면 latency ↑. */
	if (!spdk_u32_is_pow2(dev->xfer_size)) {
		/* [한국어] FTL 알고리즘이 2의 거듭제곱 가정 — 마스크/시프트로 빠른 모듈로 사용. */
		FTL_ERRLOG(dev,
			   "Unsupported xfer_size (%"PRIu64") - only power of 2 blocks xfer_size is supported\n",
			   dev->xfer_size);
		goto error;
	}

	dev->base_type = ftl_base_device_get_type_by_bdev(dev, bdev);
	/* [한국어] base bdev의 모듈 종류(NVMe / nullblk / etc)에 맞는 ops 테이블 룩업. */
	if (!dev->base_type) {
		FTL_ERRLOG(dev, "Failed to get base device type\n");
		goto error;
	}
	/* TODO: validate size when base device VSS usage gets added */
	dev->md_size = spdk_bdev_get_md_size(bdev);
	/* [한국어] per-block metadata(VSS, Variable Sector Size) 크기. NVMe metadata. */

	if (!dev->base_type->ops.md_layout_ops.region_create) {
		/* [한국어] base type ops에 region_create 미구현 — layout 진행 불가. */
		FTL_ERRLOG(dev, "Base device doesn't implement md_layout_ops\n");
		goto error;
	}

	/* Cache frequently used values */
	dev->num_blocks_in_band = ftl_calculate_num_blocks_in_band(dev->base_bdev_desc);
	/* [한국어] 한 밴드의 블록 수 — 이후 모든 LBA→band 변환에 사용. */
	dev->is_zoned = spdk_bdev_is_zoned(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));
	/* [한국어] ZNS 디바이스 여부. */

	if (dev->is_zoned) {
		/* TODO - current FTL code isn't fully compatible with ZNS drives */
		/* [한국어] ZNS는 명시적 zone management 필요 — 현재 미지원. */
		FTL_ERRLOG(dev, "Creating FTL on Zoned devices is not supported\n");
		goto error;
	}

	dev->base_layout_tracker = ftl_layout_tracker_bdev_init(spdk_bdev_get_num_blocks(bdev));
	/* [한국어] base bdev의 region(밴드 + MD 영역) 할당을 추적할 자료구조 생성. */
	if (!dev->base_layout_tracker) {
		FTL_ERRLOG(dev, "Failed to instantiate layout tracker for base device\n");
		goto error;
	}

	ftl_mngt_next_step(mngt);
	return;
error:
	/* [한국어] 어느 단계든 실패 시 fail_step으로 롤백. close_base_bdev가 NULL 안전하게
	 * 잔여 자원 정리. */
	ftl_mngt_fail_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_close_base_bdev - base bdev 닫기 (역순 정리).
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 각 자원이 할당되어 있을 때만 정리하도록 NULL 가드 — open_base_bdev 도중 실패해도
 * 부분 정리 가능.
 */
void
ftl_mngt_close_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (dev->base_ioch) {
		/* [한국어] I/O 채널 반환. */
		spdk_put_io_channel(dev->base_ioch);
		dev->base_ioch = NULL;
	}

	if (dev->base_bdev_desc) {
		struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);

		spdk_bdev_module_release_bdev(bdev);
		/* [한국어] claim 해제 — 다른 모듈이 다시 open 가능해짐. */
		spdk_bdev_close(dev->base_bdev_desc);
		/* [한국어] desc 해제. */

		dev->base_bdev_desc = NULL;
	}

	if (dev->base_layout_tracker) {
		ftl_layout_tracker_bdev_fini(dev->base_layout_tracker);
		/* [한국어] region 할당 추적자 해제. */
		dev->base_layout_tracker = NULL;
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * nv_cache_bdev_event_cb - NV cache bdev 이벤트 콜백.
 *
 * REMOVE 발생 시 abort. base와 동일 패턴.
 */
static void
nv_cache_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		assert(0);
		/* [한국어] cache bdev 핫언플러그 미지원. */
		break;
	default:
		break;
	}
}

/*
 * [한국어]
 * ftl_mngt_open_cache_bdev - NV cache bdev를 열고 nv_cache 구조체에 셋업.
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 동작은 base 버전과 거의 동일하지만 dev->nv_cache 하위 구조체와 nvc_type을 다룬다는 차이.
 * NV cache는 write buffer로 사용되며 base보다 빠른 매체(PMem 또는 작은 NVMe)가 권장.
 */
void
ftl_mngt_open_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct spdk_bdev *bdev;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	const char *bdev_name = dev->conf.cache_bdev;
	/* [한국어] 사용자가 지정한 cache bdev 이름. */
	const struct ftl_md_layout_ops *md_ops;

	if (spdk_bdev_open_ext(bdev_name, true, nv_cache_bdev_event_cb, dev,
			       &nv_cache->bdev_desc)) {
		FTL_ERRLOG(dev, "Unable to open bdev: %s\n", bdev_name);
		goto error;
	}

	bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);

	if (spdk_bdev_module_claim_bdev(bdev, nv_cache->bdev_desc, &g_ftl_bdev_module)) {
		/* clear the desc so that we don't try to release the claim on cleanup */
		/* [한국어] claim 실패 시 close하고 desc=NULL로 → cleanup이 release 안 함. */
		spdk_bdev_close(nv_cache->bdev_desc);
		nv_cache->bdev_desc = NULL;
		FTL_ERRLOG(dev, "Unable to claim bdev %s\n", bdev_name);
		goto error;
	}

	FTL_NOTICELOG(dev, "Using %s as write buffer cache\n", spdk_bdev_get_name(bdev));

	if (spdk_bdev_get_block_size(bdev) != FTL_BLOCK_SIZE) {
		/* [한국어] cache bdev도 4KiB 블록 강제. */
		FTL_ERRLOG(dev, "Unsupported block size (%d)\n",
			   spdk_bdev_get_block_size(bdev));
		goto error;
	}

	nv_cache->cache_ioch = spdk_bdev_get_io_channel(nv_cache->bdev_desc);
	/* [한국어] cache bdev용 코어 채널. */
	if (!nv_cache->cache_ioch) {
		FTL_ERRLOG(dev, "Failed to create cache IO channel for NV Cache\n");
		goto error;
	}

	if (bdev->blockcnt * bdev->blocklen < MINIMUM_CACHE_SIZE_GIB * GiB) {
		/* [한국어] cache 용량 최소 요건 미달. */
		FTL_ERRLOG(dev, "Bdev %s is too small, requires, at least %uGiB capacity\n",
			   spdk_bdev_get_name(bdev), MINIMUM_CACHE_SIZE_GIB);
		goto error;
	}
	nv_cache->md_size = spdk_bdev_get_md_size(bdev);
	/* [한국어] cache bdev의 per-block metadata 크기 (먼저 bdev 값으로 셋팅 후 아래에서 강제 덮어씀). */

	nv_cache->nvc_type = ftl_nv_cache_device_get_type_by_bdev(dev, bdev);
	/* [한국어] cache bdev 종류별 ops 테이블 룩업 (예: PMem용, NVMe용). */
	if (!nv_cache->nvc_type) {
		FTL_ERRLOG(dev, "Failed to get NV Cache device type\n");
		goto error;
	}
	nv_cache->md_size = sizeof(union ftl_md_vss);
	/* [한국어] FTL은 NV cache의 per-block VSS metadata를 자체 구조체(ftl_md_vss)로 재정의 —
	 * bdev 값과 다르더라도 강제로 덮어씀. */

	md_ops = &nv_cache->nvc_type->ops.md_layout_ops;
	if (!md_ops->region_create) {
		/* [한국어] NV cache 타입 ops에 region_create 미구현 — layout 불가. */
		FTL_ERRLOG(dev, "NV Cache device doesn't implement md_layout_ops\n");
		goto error;
	}

	dev->nvc_layout_tracker = ftl_layout_tracker_bdev_init(spdk_bdev_get_num_blocks(bdev));
	/* [한국어] NV cache bdev의 region 할당 추적자 생성. */
	if (!dev->nvc_layout_tracker) {
		FTL_ERRLOG(dev, "Failed to instantiate layout tracker for nvc device\n");
		goto error;
	}

	FTL_NOTICELOG(dev, "Using %s as NV Cache device\n", nv_cache->nvc_type->name);
	ftl_mngt_next_step(mngt);
	return;
error:
	ftl_mngt_fail_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_close_cache_bdev - NV cache bdev 닫기 (역순 정리).
 */
void
ftl_mngt_close_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (dev->nv_cache.cache_ioch) {
		spdk_put_io_channel(dev->nv_cache.cache_ioch);
		dev->nv_cache.cache_ioch = NULL;
	}

	if (dev->nv_cache.bdev_desc) {
		struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);

		spdk_bdev_module_release_bdev(bdev);
		spdk_bdev_close(dev->nv_cache.bdev_desc);

		dev->nv_cache.bdev_desc = NULL;
	}

	if (dev->nvc_layout_tracker) {
		ftl_layout_tracker_bdev_fini(dev->nvc_layout_tracker);
		dev->nvc_layout_tracker = NULL;
	}

	ftl_mngt_next_step(mngt);
}
