/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 밴드(band) 라이프사이클 관리 단계 (ftl_mngt_band.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 FTL의 핵심 데이터 단위인 "밴드(band)"의 생성/메타데이터 초기화/장식
 * (decorate)/주소 할당/마무리 단계를 담당한다. 밴드는 NAND erase block 단위에
 * 가까운 블록 묶음(기본 1 GiB = 262144개의 4 KiB block)이며, FTL은 모든 호스트 write를
 * 밴드 단위로 GC(Garbage Collection)/recovery한다.
 * 본 파일은 다음을 수행: (1) base bdev 용량으로 밴드 개수 계산 + 메타데이터 영역 예약,
 * (2) 각 밴드의 valid bitmap을 valid_map MD region 내부에 슬라이스로 매핑, (3) 큰 SSD에서
 * 여러 logical band를 하나의 physical reclaim unit(72 GiB)으로 묶어 sequentiality 향상,
 * (4) recovery 후 OPEN/FULL 밴드를 writer 큐에 재배치하고 P2L checkpoint 복원, (5) GC가
 * 시작 가능한 상태인지 검증.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 컨텍스트: 코어 spdk_thread, startup/recovery 파이프라인.
 *   호출 체인 (startup):
 *     startup desc → init_layout → ftl_mngt_init_bands (ftl_dev_init_bands)
 *                  → ftl_mngt_init_bands_md (각 밴드의 valid bitmap 슬라이스 + md 페이지)
 *                  → ftl_mngt_decorate_bands (logical→physical band grouping)
 *                  → ftl_mngt_initialize_band_address (각 밴드 start_addr / tail_md_addr 계산)
 *                  → recovery/restore_md → ftl_mngt_finalize_init_bands
 *                                          (OPEN/FULL band → writer 큐 + P2L 복원)
 *
 * === 타 모듈과의 연결 ===
 * - 의존: lib/ftl/ftl_band.[ch] (ftl_band 구조체, ftl_band_iter, ftl_band_*),
 *   lib/ftl/ftl_internal.h (FTL_BAND_TYPE_*, ftl_writer, ftl_band_set_owner),
 *   lib/ftl/ftl_layout.[ch] (FTL_LAYOUT_REGION_TYPE_*, ftl_layout_base_md_blocks),
 *   lib/ftl/utils/ftl_bitmap.[ch] (밴드별 valid bitmap),
 *   lib/ftl/ftl_nv_cache.[ch] (NV cache의 max seq id 조회),
 *   lib/ftl/utils/ftl_property.[ch] (RPC dump용 base_device 프로퍼티 등록),
 *   lib/ftl/ftl_writer.[ch] (writer_user / writer_gc 큐).
 * - 의존받음: ftl_mngt_startup.c (init_bands/decorate/initialize_band_address/
 *   finalize_init_bands step), ftl_mngt_recovery.c (recovery 중 finalize_init_bands).
 * - 데이터 흐름: dev->bands[N] 배열을 만들고 각 밴드의 md를 BAND_MD region 슬라이스에,
 *   valid bitmap을 VALID_MAP region 슬라이스에 매핑 → write 경로가 이 메타를 갱신 →
 *   GC가 valid bitmap으로 GC 후보 선택.
 *
 * === 주요 함수/구조체 요약 ===
 * - ftl_band_init_md()              : 단일 밴드의 valid bitmap + md 페이지 셋업.
 * - ftl_dev_init_bands()            : dev 차원 밴드 배열 calloc 및 free/shut 큐 초기화.
 * - ftl_dev_init_bands_md()         : 모든 밴드에 대해 ftl_band_init_md 반복.
 * - ftl_dev_deinit_bands()          : 밴드 배열 free.
 * - ftl_dev_deinit_bands_md()       : 모든 밴드 valid bitmap 해제.
 * - ftl_mngt_init/deinit_bands(_md)(): 위 4개의 step 어댑터.
 * - decorate_bands()                : 큰 SSD에서 다중 logical band를 physical band로 그룹화.
 * - ftl_mngt_decorate_bands()/initialize_band_address(): 어댑터.
 * - ftl_recover_max_seq()           : 모든 밴드/NV cache에서 최대 seq_id를 찾아 sb에 기록.
 * - _band_cmp()                     : qsort용 seq 오름차순 비교.
 * - next_high_prio_band()           : valid 블록이 가장 적은 shut band 선택(GC 후보).
 * - finalize_init_gc()              : free band 0개 시 GC가 진행 가능한지 검증.
 * - ftl_property_dump_base_dev()    : RPC bdev_ftl_get_properties용 band 상태 dump.
 * - ftl_mngt_finalize_init_bands()  : recovery 후 OPEN/FULL band를 writer에 재배치.
 */

#include "ftl_core.h"
/* [한국어] spdk_ftl_dev (dev->bands, dev->free_bands, dev->shut_bands, dev->layout 등). */
#include "ftl_mngt_steps.h"
/* [한국어] step 프로토타입. */
#include "ftl_band.h"
/* [한국어] ftl_band, ftl_band_iter, ftl_band_set_state, ftl_band_alloc_p2l_map 등. */
#include "ftl_internal.h"
/* [한국어] FTL_BAND_TYPE_*, FTL_DF_OBJ_ID_INVALID, ftl_writer 등 내부 정의. */

/*
 * [한국어]
 * ftl_band_init_md - 단일 밴드의 valid bitmap과 md 페이지를 layout MD에 매핑.
 *
 * @band: 초기화 대상 밴드. band->id, band->dev, band->start_addr가 셋팅되어 있어야 함
 *        (start_addr는 후속 step에서 셋팅되지만 valid bitmap 슬라이스 계산엔 필요).
 * @return: 0=성공, 음수 errno=실패.
 *
 * 동작:
 *  1) BAND_MD region에서 ftl_band_md 배열 버퍼 획득(전체 밴드의 메타가 한 곳에 모여 있음).
 *  2) VALID_MAP region에서 bitmap 백킹 버퍼 획득.
 *  3) band_num_blocks가 bitmap word 정렬(보통 64*8=512비트)에 맞는지 검증.
 *  4) p2l_map.valid = ftl_bitmap_create(VALID_MAP buffer + start_addr/8, band_valid_map_bytes)
 *     - 각 밴드는 전체 valid bitmap의 자기 슬라이스를 보는 형태(공유 메모리, 비트 단위 분할).
 *  5) band->md = &band_md[band->id] — BAND_MD 배열의 자기 슬롯.
 *  6) version 셋팅. fast_startup이 아니면 df_p2l_map을 INVALID로 (재할당 필요 표시).
 */
static int
ftl_band_init_md(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;
	struct ftl_md *band_info_md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_BAND_MD];
	/* [한국어] 모든 밴드의 md를 모아둔 BAND_MD region 핸들. */
	struct ftl_md *valid_map_md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_VALID_MAP];
	/* [한국어] 모든 밴드의 valid bitmap을 모아둔 VALID_MAP region 핸들. */
	uint64_t band_num_blocks = ftl_get_num_blocks_in_band(band->dev);
	size_t band_valid_map_bytes;
	struct ftl_band_md *band_md = ftl_md_get_buffer(band_info_md);
	/* [한국어] BAND_MD region의 인메모리 버퍼 시작 — 밴드 ID로 인덱싱 가능. */

	if (band_num_blocks % (ftl_bitmap_buffer_alignment * 8)) {
		/* [한국어] 한 밴드의 블록 수가 비트맵 정렬(보통 64B = 512bit) 배수가 아니면
		 * 비트맵 슬라이스 경계가 깨짐 — 강제 fail. */
		FTL_ERRLOG(dev, "The number of blocks in band is not divisible by bitmap word bits\n");
		return -EINVAL;
	}
	band_valid_map_bytes = band_num_blocks / 8;
	/* [한국어] 한 밴드의 valid bitmap 바이트 수 = 블록 수 / 8. */

	p2l_map->valid = ftl_bitmap_create(ftl_md_get_buffer(valid_map_md) +
					   band->start_addr / 8, band_valid_map_bytes);
	/* [한국어] VALID_MAP buffer의 (band->start_addr / 8) 오프셋을 시작으로 band_valid_map_bytes
	 * 만큼 슬라이스를 잡아 비트맵 핸들 생성. 모든 밴드가 한 region을 비트 단위로 분할 공유. */
	if (!p2l_map->valid) {
		return -ENOMEM;
	}

	band->md = &band_md[band->id];
	/* [한국어] BAND_MD 배열에서 자기 ID 슬롯에 대한 포인터 보관. */
	band->md->version = FTL_BAND_VERSION_CURRENT;
	/* [한국어] 현재 빌드의 band 메타 버전 기록 — upgrade 검증용. */
	if (!ftl_fast_startup(dev)) {
		/* [한국어] 일반 부팅: P2L map 객체는 아직 미할당이므로 INVALID로 표시.
		 * fast startup(SHM)이면 SHM에서 그대로 받아 쓰므로 덮지 않음. */
		band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
	}

	return 0;
}

/*
 * [한국어]
 * ftl_dev_init_bands - dev->bands 배열 calloc + free/shut 큐 초기화.
 *
 * @dev: FTL 디바이스. dev->base_bdev_desc, dev->num_blocks_in_band가 결정되어 있어야 함.
 * @return: 0=성공, -ENOMEM/-1=실패.
 *
 * 동작:
 *  1) base bdev 총 블록 수로 num_bands 추정 (blocks / blocks_in_band).
 *  2) 메타데이터(슈퍼블록·band_md·valid_map 등)에 필요한 블록을 밴드 단위로 환산해
 *     num_bands에서 차감 — 메타 영역과 데이터 영역이 겹치지 않게 함.
 *  3) free/shut 큐 초기화. 모든 밴드는 처음에 shut_bands에 들어감(restore가 진행될 때까지).
 *  4) bands 배열 calloc + 각 밴드 id/dev 셋팅.
 */
static int
ftl_dev_init_bands(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	uint64_t i, blocks, md_blocks, md_bands;

	/* Calculate initial number of bands */
	blocks = spdk_bdev_get_num_blocks(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));
	dev->num_bands = blocks / ftl_get_num_blocks_in_band(dev);
	/* [한국어] 1차 추정 = base bdev 전체를 밴드로 나눈 개수. */

	/* Calculate number of bands considering base device metadata size requirement */
	md_blocks = ftl_layout_base_md_blocks(dev);
	/* [한국어] 메타데이터(슈퍼블록·band_md·valid_map 등)에 필요한 base device 블록 수. */
	md_bands = spdk_divide_round_up(md_blocks, dev->num_blocks_in_band);
	/* [한국어] 메타에 잡아먹히는 밴드 수 (올림). */

	if (dev->num_bands > md_bands) {
		/* Save a band worth of space for metadata */
		/* [한국어] 메타용 밴드만큼 데이터용에서 빼서 충돌 방지. */
		dev->num_bands -= md_bands;
	} else {
		/* [한국어] 디바이스가 너무 작아 메타 영역도 못 챙김 — fail. */
		FTL_ERRLOG(dev, "Base device too small to store metadata\n");
		return -1;
	}

	TAILQ_INIT(&dev->free_bands);
	/* [한국어] 즉시 write 가능한 빈 밴드 큐 — 처음엔 비어 있음. */
	TAILQ_INIT(&dev->shut_bands);
	/* [한국어] 닫혀(closed)있거나 상태 확인 전 밴드 큐 — 처음에 모든 밴드가 들어감. */

	dev->num_free = 0;
	dev->bands = calloc(ftl_get_num_bands(dev), sizeof(*dev->bands));
	if (!dev->bands) {
		return -ENOMEM;
	}

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		band = &dev->bands[i];
		band->id = i;
		band->dev = dev;
		/* [한국어] 백포인터 — 콜백/헬퍼에서 dev 접근용. */

		/* Adding to shut_bands is necessary - see ftl_restore_band_close_cb() */
		/* [한국어] 모든 밴드를 일단 shut_bands에 — recovery가 각 밴드 상태를 보고
		 * 적절히 큐 이동(shut/free/open). */
		TAILQ_INSERT_TAIL(&dev->shut_bands, band, queue_entry);
	}

	return 0;
}

/*
 * [한국어]
 * ftl_dev_init_bands_md - 모든 밴드에 대해 ftl_band_init_md 반복.
 *
 * 한 밴드라도 실패하면 즉시 break, 나머지는 deinit_bands_md가 NULL 체크로 안전 처리.
 */
static int
ftl_dev_init_bands_md(struct spdk_ftl_dev *dev)
{
	uint64_t i;
	int rc = 0;

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		rc = ftl_band_init_md(&dev->bands[i]);
		if (rc) {
			FTL_ERRLOG(dev, "Failed to initialize metadata structures for band [%lu]\n", i);
			break;
		}
	}

	return rc;
}

/*
 * [한국어]
 * ftl_dev_deinit_bands - dev->bands 배열 해제.
 */
static void
ftl_dev_deinit_bands(struct spdk_ftl_dev *dev)
{
	free(dev->bands);
	/* [한국어] 배열 자체만 해제. 비트맵/md 슬라이스는 deinit_bands_md가 별도 처리. */
}

/*
 * [한국어]
 * ftl_dev_deinit_bands_md - 모든 밴드의 valid bitmap 해제 + md 포인터 NULL.
 */
static void
ftl_dev_deinit_bands_md(struct spdk_ftl_dev *dev)
{
	if (dev->bands) {
		uint64_t i;
		for (i = 0; i < dev->num_bands; ++i) {
			struct ftl_band *band = &dev->bands[i];

			ftl_bitmap_destroy(band->p2l_map.valid);
			/* [한국어] 비트맵 핸들 해제 — 백킹 buffer는 valid_map MD region이 보관. */
			band->p2l_map.valid = NULL;

			band->md = NULL;
			/* [한국어] BAND_MD region의 슬롯 포인터 dangling 방지. */
		}
	}
}

/*
 * [한국어]
 * ftl_mngt_init_bands - bands 배열 생성 step 어댑터. 동기 호출.
 */
void
ftl_mngt_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_dev_init_bands(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_init_bands_md - 모든 밴드 md/bitmap 셋업 step 어댑터.
 */
void
ftl_mngt_init_bands_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_dev_init_bands_md(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_deinit_bands - bands 배열 해제 step 어댑터.
 */
void
ftl_mngt_deinit_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_dev_deinit_bands(dev);
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_deinit_bands_md - 모든 밴드 md/bitmap 해제 step 어댑터.
 */
void
ftl_mngt_deinit_bands_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_dev_deinit_bands_md(dev);
	ftl_mngt_next_step(mngt);
}

/*
 * For grouping multiple logical bands (1GiB) to make any IOs more sequential from the drive's
 * perspective. Improves WAF.
 */
/*
 * [한국어] base bdev에서 GC가 한 번에 묶어서 처리할 reclaim unit의 크기.
 * 큰 SSD (>1 TiB)에서는 한 logical band(1 GiB)가 너무 작아 NAND 내부에서 fragmentation이
 * 심해질 수 있다. 여러 logical band를 한 physical band로 묶어 sequential하게 GC하면
 * SSD 내부 GC 효율과 WAF(Write Amplification Factor)가 개선된다. 72 GiB는 SPDK FTL의
 * 경험적 값. */
#define BASE_BDEV_RECLAIM_UNIT_SIZE (72 * GiB)

/*
 * [한국어]
 * decorate_bands - 큰 SSD에서 logical band 여러 개를 physical band로 그룹화하고
 *                  정렬 안 맞는 끝부분 logical band를 폐기.
 *
 * @dev: FTL 디바이스. dev->bands가 이미 init되어 있어야 함.
 *
 * 동작:
 *  1) reclaim_unit_num_blocks = 72 GiB / 4 KiB. 1 GiB band의 정수 배.
 *  2) base 용량이 1 TiB 이상이면 num_logical_in_phys = reclaim_unit / band (예: 72).
 *     1 TiB 미만이면 2개씩만 묶음 (작은 SSD는 그룹화 효과 적음).
 *  3) 총 밴드 수가 그룹 크기 배수가 안 맞으면 끝부분 num_to_drop개 폐기.
 *  4) 0..num_bands-num_to_drop 순회하며 phys_id 부여 (그룹마다 ++).
 *  5) 폐기될 밴드는 shut_bands에서 빼고 num_bands 감소.
 *  6) num_logical_bands_in_physical 저장 — 이후 GC가 이 단위로 동작.
 */
static void
decorate_bands(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	uint64_t i, num_to_drop, phys_id = 0;
	uint64_t num_blocks, num_bands;
	uint64_t num_blocks_in_band = ftl_get_num_blocks_in_band(dev);
	uint64_t reclaim_unit_num_blocks = BASE_BDEV_RECLAIM_UNIT_SIZE / FTL_BLOCK_SIZE;
	uint32_t num_logical_in_phys = 2;
	/* [한국어] 기본값: 작은 SSD에서는 2개씩만 그룹. */

	assert(reclaim_unit_num_blocks % num_blocks_in_band == 0);
	/* [한국어] reclaim unit이 logical band 크기의 정수 배여야 그룹화가 깔끔. */

	num_blocks = spdk_bdev_get_num_blocks(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));

	/* For base bdev bigger than 1TB take reclaim uint size for grouping GC bands */
	if (num_blocks > (TiB / FTL_BLOCK_SIZE)) {
		/* [한국어] base가 1 TiB보다 크면 72 GiB / 1 GiB = 72개씩 그룹. */
		assert(reclaim_unit_num_blocks < num_blocks);
		num_logical_in_phys = reclaim_unit_num_blocks / num_blocks_in_band;
	}

	num_to_drop = ftl_get_num_bands(dev) % num_logical_in_phys;
	/* [한국어] 그룹 크기 배수에 안 맞는 잉여 logical band 수 — 폐기 대상. */

	i = 0;
	while (i < ftl_get_num_bands(dev) - num_to_drop) {
		band = &dev->bands[i];

		band->phys_id = phys_id;
		/* [한국어] physical band ID 부여 — 같은 그룹은 같은 phys_id. */
		i++;
		if (i % num_logical_in_phys == 0) {
			/* [한국어] 그룹 채워짐 — 다음 그룹 ID로. */
			phys_id++;
		}
	}

	/* Mark not aligned logical bands as broken */
	num_bands = ftl_get_num_bands(dev);
	while (i < num_bands) {
		/* [한국어] 정렬 안 맞는 끝부분 logical band들을 폐기 — shut_bands에서 제거,
		 * num_bands 감소. 이 밴드들의 메모리 자체는 dev->bands 배열에 남지만 사용 안 함. */
		band = &dev->bands[i];
		dev->num_bands--;
		TAILQ_REMOVE(&dev->shut_bands, band, queue_entry);
		i++;
	}

	dev->num_logical_bands_in_physical = num_logical_in_phys;
	/* [한국어] 이후 GC가 한 번에 처리할 logical band 개수 저장. */
}

/*
 * [한국어]
 * ftl_mngt_decorate_bands - decorate_bands 호출 step 어댑터.
 */
void
ftl_mngt_decorate_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	decorate_bands(dev);
	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_initialize_band_address - 각 밴드의 base bdev 시작 PBA + tail md PBA 계산.
 *
 * @dev:  FTL 디바이스. layout.md[DATA_BASE]가 결정되어 있어야 함.
 * @mngt: ftl_mngt 핸들.
 *
 * 동작: DATA_BASE region의 시작 offset에 i*num_blocks_in_band를 더해 각 밴드의
 *      start_addr 계산. tail_md_addr = ftl_band_tail_md_addr(band) (밴드 끝부분 P2L map용).
 */
void
ftl_mngt_initialize_band_address(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_band *band;
	struct ftl_md *data_md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_DATA_BASE];
	/* [한국어] base bdev 내 사용자 데이터 영역 region 핸들. */
	uint64_t i;

	for (i = 0; i < ftl_get_num_bands(dev); i++) {
		band = &dev->bands[i];
		band->start_addr = data_md->region->current.offset + i * dev->num_blocks_in_band;
		/* [한국어] DATA_BASE region 시작 + 밴드 ID*밴드크기 = 이 밴드의 PBA 시작. */
		band->tail_md_addr = ftl_band_tail_md_addr(band);
		/* [한국어] 밴드 끝부분 메타데이터(P2L map) 영역의 PBA — write 마지막에 기록. */
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_recover_max_seq - 모든 밴드와 NV cache에서 최대 seq_id를 찾아 sb에 기록.
 *
 * @dev: FTL 디바이스.
 *
 * 왜 필요한가: recovery 후 다음 write가 사용할 seq_id는 기존 모든 데이터의 seq보다
 * 커야 ordering이 유지됨. 또한 GC/user writer의 last_seq_id를 기준으로 새 데이터를
 * 정렬 가능하도록 셋팅.
 */
void
ftl_recover_max_seq(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	uint64_t band_close_seq_id = 0, band_open_seq_id = 0;
	uint64_t chunk_close_seq_id = 0, chunk_open_seq_id = 0;
	uint64_t max = 0;

	TAILQ_FOREACH(band, &dev->shut_bands, queue_entry) {
		/* [한국어] shut_bands에 있는 모든 밴드(closed/open/full)에서 최대 seq 추출. */
		band_open_seq_id = spdk_max(band_open_seq_id, band->md->seq);
		band_close_seq_id = spdk_max(band_close_seq_id, band->md->close_seq_id);
	}
	ftl_nv_cache_get_max_seq_id(&dev->nv_cache, &chunk_open_seq_id, &chunk_close_seq_id);
	/* [한국어] NV cache chunk들의 최대 seq도 조회. */


	dev->nv_cache.last_seq_id = chunk_close_seq_id;
	/* [한국어] NV cache는 이미 close된 chunk 다음 seq부터 재사용. */
	dev->writer_gc.last_seq_id = band_close_seq_id;
	/* [한국어] GC writer는 이미 close된 band 다음 seq부터 사용. */
	dev->writer_user.last_seq_id = band_close_seq_id;
	/* [한국어] 사용자 writer도 마찬가지. */

	max = spdk_max(max, band_open_seq_id);
	max = spdk_max(max, band_close_seq_id);
	max = spdk_max(max, chunk_open_seq_id);
	max = spdk_max(max, chunk_close_seq_id);
	/* [한국어] 4개 후보 중 가장 큰 값을 sb->seq_id로 기록 — 다음 write가 사용할 base. */

	dev->sb->seq_id = max;
}

/*
 * [한국어]
 * _band_cmp - qsort용 ftl_band 포인터 배열 비교 함수, seq_id 오름차순.
 *
 * recovery 후 OPEN 밴드를 writer에 재배치할 때 seq 순서대로 정렬해 ordering 보존.
 */
static int
_band_cmp(const void *_a, const void *_b)
{
	struct ftl_band *a, *b;

	a = *((struct ftl_band **)_a);
	/* [한국어] qsort는 element를 가리키는 포인터를 넘기므로 dereference. */
	b = *((struct ftl_band **)_b);

	return a->md->seq - b->md->seq;
	/* [한국어] 음수 = a가 먼저, 양수 = b가 먼저. */
}

/*
 * [한국어]
 * next_high_prio_band - shut_bands 중 valid 블록이 가장 적은 밴드 선택 (GC 우선순위).
 *
 * @dev: FTL 디바이스.
 * @return: 가장 valid 적은 밴드 (NULL 가능 — shut_bands가 비었을 때).
 *
 * 왜 필요한가: free band가 없을 때 GC를 시작해야 하는데, 그 GC가 가장 적은 데이터만
 * relocate하면 가장 빨리 free 공간을 회수할 수 있음.
 */
static struct ftl_band *
next_high_prio_band(struct spdk_ftl_dev *dev)
{
	struct ftl_band *result = NULL, *band;
	uint64_t validity = UINT64_MAX;
	/* [한국어] 최솟값을 찾기 위한 sentinel. */

	TAILQ_FOREACH(band, &dev->shut_bands, queue_entry) {
		if (band->p2l_map.num_valid < validity) {
			result = band;
			validity = result->p2l_map.num_valid;
		}
	}

	return result;
}

/*
 * [한국어]
 * finalize_init_gc - free band 0개 시 GC가 진행 가능한지 검증, 가능하면 high_prio band 셋팅.
 *
 * @dev: FTL 디바이스.
 * @return: 0=OK(GC 가능 또는 free band 있음), -1=불가능(데이터 사용량이 100%).
 *
 * 동작:
 *  1) ftl_band_init_gc_iter — GC 순회자 초기화.
 *  2) sb_shm->gc_info.band_id_high_prio = INVALID로 일단 리셋.
 *  3) free band가 없으면(num_free == 0):
 *     - GC writer 가용 블록 수 조회.
 *     - 우선 search_next_to_reloc로 GC 후보 1개 시도.
 *     - 그 후보가 valid 블록 수 ≤ free 블록 수면 OK (이동 가능).
 *     - 아니면 next_high_prio_band로 가장 작은 후보 다시 검색.
 *     - 그 후보도 안 들어가면 fatal — 디바이스가 사용량 100% 직전이라 GC 불가.
 */
static int
finalize_init_gc(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	uint64_t free_blocks, blocks_to_move;

	ftl_band_init_gc_iter(dev);
	/* [한국어] GC 다음 후보 선택용 순회자 초기화. */
	dev->sb_shm->gc_info.band_id_high_prio = FTL_BAND_ID_INVALID;
	/* [한국어] high priority band 일단 없음으로 셋팅. */

	if (0 == dev->num_free) {
		/* Get number of available blocks in writer */
		/* [한국어] free band 0 — GC가 새 데이터를 어디로 옮길 수 있는지 확인. */
		free_blocks = ftl_writer_get_free_blocks(&dev->writer_gc);

		/*
		 * First, check a band candidate to GC
		 */
		/* [한국어] 일반 GC 후보(LRU/seq 기반). */
		band = ftl_band_search_next_to_reloc(dev);
		ftl_bug(NULL == band);
		/* [한국어] free=0 + 후보 없음 = 절대 일어나면 안 되는 상태. */
		blocks_to_move = band->p2l_map.num_valid;
		if (blocks_to_move <= free_blocks) {
			/* This GC band can be moved */
			/* [한국어] 후보의 valid가 GC writer 가용 공간에 들어감 — OK. */
			return 0;
		}

		/*
		 * The GC candidate cannot be moved because no enough space. We need to find
		 * another band.
		 */
		/* [한국어] 일반 후보가 너무 큼 — valid가 가장 적은 후보로 재검색. */
		band = next_high_prio_band(dev);
		ftl_bug(NULL == band);

		if (band->p2l_map.num_valid > free_blocks) {
			/* [한국어] 가장 작은 후보도 안 들어감 — 디바이스 폭발 직전. fatal. */
			FTL_ERRLOG(dev, "CRITICAL ERROR, no more free bands and cannot start\n");
			return -1;
		} else {
			/* GC needs to start using this band */
			/* [한국어] 이 high prio 밴드를 강제로 GC 우선순위로 셋팅. */
			dev->sb_shm->gc_info.band_id_high_prio = band->id;
		}
	}

	return 0;
}

/*
 * [한국어]
 * ftl_property_dump_base_dev - RPC bdev_ftl_get_properties 응답에 band 상태 dump.
 *
 * @dev: FTL 디바이스.
 * @property: 호출 컨텍스트 (사용 안 함).
 * @w: JSON write 컨텍스트.
 *
 * 출력: "bands": [{"id": N, "state": "...", "validity": 0..1.0}, ...].
 * - validity = 1 - invalidity (1.0 = 모두 유효, 0 = 모두 trim/덮어쓰기 됨).
 */
static void
ftl_property_dump_base_dev(struct spdk_ftl_dev *dev, const struct ftl_property *property,
			   struct spdk_json_write_ctx *w)
{
	uint64_t i;
	struct ftl_band *band;

	spdk_json_write_named_array_begin(w, "bands");
	for (i = 0, band = dev->bands; i < ftl_get_num_bands(dev); i++, band++) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint64(w, "id", i);
		spdk_json_write_named_string(w, "state", ftl_band_get_state_name(band));
		spdk_json_write_named_double(w, "validity", 1.0 - ftl_band_invalidity(band));
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
}

/*
 * [한국어]
 * ftl_mngt_finalize_init_bands - recovery/restore 후 OPEN/FULL band를 writer에 재배치.
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 가장 복잡한 step. 동작:
 *  1) ftl_recover_max_seq — sb seq_id 갱신.
 *  2) base_device 프로퍼티 등록 (RPC dump용).
 *  3) free_bands에 있는 band의 df_p2l_map을 INVALID로(클린 상태).
 *  4) shut_bands를 순회하며:
 *     - OPEN/FULL인 band는 open_bands[] 배열에 모음 (writer에 재배치 필요).
 *     - CREATE 모드면 나머지를 FREE로 강제 (포맷 첫 부팅).
 *     - 아니면 num_shut++.
 *  5) open_bands를 seq 오름차순 qsort.
 *  6) 각 open band를 type에 따라 writer_user 또는 writer_gc에 할당:
 *     - FULL이면 writer->full_bands 큐에 push.
 *     - OPEN이면 writer->band(없으면) / writer->next_band에 셋팅.
 *     - num_bands 카운트 + ftl_band_set_owner.
 *  7) fast_startup이면 SHM에서 P2L map 그대로 open + iter 복원 + ckpt restore_shm_clean.
 *     일반 부팅이면 P2L map 새로 alloc + iter 초기화 + ckpt restore_clean.
 *  8) fast_startup일 때 p2l_pool 외부 mempool 초기화.
 *  9) num_free 재계산.
 *  10) total = shut + open + free 가 num_bands와 일치해야 함 (consistency 검증).
 *  11) finalize_init_gc로 GC 시작 가능 검증.
 */
void
ftl_mngt_finalize_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_band *band, *temp_band, *open_bands[FTL_MAX_OPEN_BANDS];
	struct ftl_writer *writer = NULL;
	uint64_t i, num_open = 0, num_shut = 0;
	uint64_t offset;
	bool fast_startup = ftl_fast_startup(dev);

	ftl_recover_max_seq(dev);
	/* [한국어] sb seq_id를 모든 밴드/cache 중 최대로 갱신. */
	ftl_property_register(dev, "base_device", NULL, 0, NULL, NULL, ftl_property_dump_base_dev, NULL,
			      NULL, true);
	/* [한국어] RPC bdev_ftl_get_properties 응답에 base_device 섹션 추가. */

	TAILQ_FOREACH_SAFE(band, &dev->free_bands, queue_entry, temp_band) {
		/* [한국어] free band는 P2L map 안 가짐 — INVALID로 표시. */
		band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
	}

	TAILQ_FOREACH_SAFE(band, &dev->shut_bands, queue_entry, temp_band) {
		if (band->md->state == FTL_BAND_STATE_OPEN ||
		    band->md->state == FTL_BAND_STATE_FULL) {
			/* [한국어] OPEN/FULL은 write 도중에 셧다운된 밴드 — writer에 재바인딩 필요. */
			TAILQ_REMOVE(&dev->shut_bands, band, queue_entry);
			open_bands[num_open++] = band;
			assert(num_open <= FTL_MAX_OPEN_BANDS);
			continue;
		}

		if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
			/* [한국어] CREATE 모드(첫 포맷) — 모든 닫힌 밴드를 FREE로 reset. */
			TAILQ_REMOVE(&dev->shut_bands, band, queue_entry);
			assert(band->md->state == FTL_BAND_STATE_FREE);
			band->md->state = FTL_BAND_STATE_CLOSED;
			/* [한국어] set_state가 transition 검증을 하므로 일단 CLOSED를 거쳐 FREE로. */
			ftl_band_set_state(band, FTL_BAND_STATE_FREE);
		} else {
			/* [한국어] 일반 부팅 — 닫힌 밴드는 그냥 카운트만. */
			num_shut++;
		}

		band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
	}

	/* Assign open bands to writers and alloc necessary resources */
	qsort(open_bands, num_open, sizeof(open_bands[0]), _band_cmp);
	/* [한국어] seq 오름차순으로 정렬 — 가장 오래된 것부터 writer에 바인딩. */

	for (i = 0; i < num_open; ++i) {
		band = open_bands[i];

		if (band->md->type == FTL_BAND_TYPE_COMPACTION) {
			/* [한국어] 사용자 write로 채워진 밴드 — user writer로. */
			writer = &dev->writer_user;
		} else if (band->md->type == FTL_BAND_TYPE_GC) {
			/* [한국어] GC가 만든 밴드 — gc writer로. */
			writer = &dev->writer_gc;
		} else {
			assert(false);
			/* [한국어] 알 수 없는 type — 손상. */
		}

		if (band->md->state == FTL_BAND_STATE_FULL) {
			/* [한국어] FULL은 더 이상 write 불가 — close 대기 큐로. */
			TAILQ_INSERT_TAIL(&writer->full_bands, band, queue_entry);
		} else {
			/* [한국어] OPEN — writer의 현재/다음 밴드로 바인딩. */
			if (writer->band == NULL) {
				writer->band = band;
			} else {
				writer->next_band = band;
			}
		}

		writer->num_bands++;
		ftl_band_set_owner(band, ftl_writer_band_state_change, writer);
		/* [한국어] 밴드 상태 변화 시 writer가 콜백 받도록 owner 셋팅. */

		if (fast_startup) {
			/* [한국어] SHM 픽업 — P2L map은 SHM df_p2l_map 그대로 open만 하고 끝. */
			FTL_NOTICELOG(dev, "SHM: band open P2L map df_id 0x%"PRIx64"\n", band->md->df_p2l_map);
			if (ftl_band_open_p2l_map(band)) {
				ftl_mngt_fail_step(mngt);
				return;
			}

			offset = band->md->iter.offset;
			ftl_band_iter_init(band);
			ftl_band_iter_set(band, offset);
			/* [한국어] iter를 초기화한 뒤 저장된 offset으로 복원 — write 재개 위치. */
			ftl_mngt_p2l_ckpt_restore_shm_clean(band);
			/* [한국어] SHM clean이라 P2L checkpoint도 SHM에서 픽업. */
		} else if (dev->sb->clean) {
			/* [한국어] clean shutdown 후 일반 부팅 — P2L map 새로 alloc. */
			band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
			if (ftl_band_alloc_p2l_map(band)) {
				ftl_mngt_fail_step(mngt);
				return;
			}

			offset = band->md->iter.offset;
			ftl_band_iter_init(band);
			ftl_band_iter_set(band, offset);

			if (ftl_mngt_p2l_ckpt_restore_clean(band)) {
				/* [한국어] 디스크 P2L ckpt에서 복원 실패. */
				ftl_mngt_fail_step(mngt);
				return;
			}
		}
		/* [한국어] dirty shutdown 케이스(!sb->clean & !fast_startup)는 recovery
		 * sub-process가 별도로 처리. 여기서는 아무것도 안 함. */
	}

	if (fast_startup) {
		ftl_mempool_initialize_ext(dev->p2l_pool);
		/* [한국어] SHM에서 픽업한 외부 메모리 풀 초기화 — 핸들/인덱스만 다시 매핑. */
	}


	/* Recalculate number of free bands */
	dev->num_free = 0;
	TAILQ_FOREACH(band, &dev->free_bands, queue_entry) {
		assert(band->md->state == FTL_BAND_STATE_FREE);
		dev->num_free++;
	}
	ftl_apply_limits(dev);
	/* [한국어] 호스트 write throttling 제한값을 num_free에 따라 갱신. */

	if ((num_shut + num_open + dev->num_free) != ftl_get_num_bands(dev)) {
		/* [한국어] 합이 안 맞음 = 어떤 밴드가 누락됨 = 자료구조 손상. fail. */
		FTL_ERRLOG(dev, "ERROR, band list inconsistent state\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (finalize_init_gc(dev)) {
		/* [한국어] GC 시작 불가능 — 디바이스 사용량 100% 직전. */
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}
