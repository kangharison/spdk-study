/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 디스크 레이아웃 관리 (ftl_layout.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 FTL이 두 개의 백엔드 디바이스(base bdev = 본 데이터 영역, nv_cache bdev = NVRAM/Optane 같은
 * 비휘발성 캐시) 위에 어떤 메타데이터/데이터 영역(region)을 어디에 배치할지를 결정하고 영속화한다.
 * 핵심 region들: 슈퍼블록(SB), L2P 맵, 밴드 메타데이터(BAND_MD), valid map, P2L 체크포인트(P2L_CKPT_*),
 * trim 메타/로그, NV cache 메타/데이터, base device 데이터. 본 파일은 (1) 새로 생성 모드 vs 기존
 * 로드 모드 vs 레거시 디폴트 모드의 분기, (2) 각 region의 크기/위치 계산(superblock 정렬 단위 사용),
 * (3) region 간 중첩 검증(ftl_validate_regions), (4) region을 슈퍼블록 blob에 직렬화(ftl_layout_blob_*)하는
 * 책임을 진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: ftl_mngt 초기화 단계 → 본 파일의 ftl_layout_setup_superblock → ftl_layout_setup → 각 region
 *  생성 헬퍼 → nvc_type/base_type ops(md_layout_ops.region_create/region_open). region이 결정되면 ftl_md
 *  서브시스템이 그 region을 기반으로 메타데이터 영속화 객체를 만든다.
 * 실행 컨텍스트: 모두 디바이스 초기화 단계에서 단일 spdk_thread에서 호출된다. I/O 경로는 아니므로
 * 성능 critical path가 아니다.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(호출자): ftl_mngt(start-up management 시퀀스), ftl_init(디바이스 초기화).
 * - 하위(피호출자): nv_cache.nvc_type->ops.md_layout_ops, base_type->ops.md_layout_ops(각 백엔드 종속
 *   region 생성 함수), ftl_superblock(SB blob area load/store/apply), ftl_layout_tracker_bdev(bdev에 등록된
 *   region 트래커).
 * - 공유 자료구조: struct ftl_layout(전 region 배열, l2p/p2l/nvc/base 메타정보), struct ftl_layout_region
 *   (region별 offset/blocks/entry_size/bdev_desc/ioch). dev->sb 또한 본 파일이 v5+ 인지 확인해 모드 결정에
 *   영향을 받는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - ftl_layout_setup_superblock: 부팅 시 가장 먼저 SB region(NV cache 측)과 SB_BASE(base 측 미러)를 만든다.
 * - ftl_layout_setup: 모드(생성/로드/레거시)에 따라 모든 region 배치 결정. 검증 후 SB blob에 저장.
 * - layout_setup_default_nvc/base: 새 layout 생성 시 호출되는 region 생성 시퀀스.
 * - layout_setup_legacy_default_*: SB v4 이하에서 v5로 업그레이드할 때 사용하는 호환 시퀀스.
 * - layout_load: SB v5+에서 NV cache/base의 layout blob을 읽어 region 배열을 그대로 복원.
 * - ftl_validate_regions: 같은 bdev_desc를 공유하는 region들이 offset 범위에서 겹치지 않는지 검증.
 * - ftl_layout_blob_store/load: region 배열의 (type, entry_size, num_entries)를 슈퍼블록 blob 영역에
 *   직렬화/역직렬화.
 * - enum ftl_layout_setup_mode: LOAD_CURRENT(SB v5+에서 로드), NO_RESTRICT(처음 생성), LEGACY_DEFAULT
 *   (SB pre-v5 → v5 업그레이드).
 */

/* [한국어] SPDK bdev 공개 API — bdev 객체에서 num_blocks/write_unit_size 등을 얻기 위해. */
#include "spdk/bdev.h"

/* [한국어] FTL 코어 자료구조 — spdk_ftl_dev, ftl_get_num_blocks_in_band 등. */
#include "ftl_core.h"
/* [한국어] FTL 유틸 매크로(spdk_round_up, ftl_bug 등). */
#include "ftl_utils.h"
/* [한국어] 밴드 헬퍼 — ftl_band_user_blocks 사용. */
#include "ftl_band.h"
/* [한국어] 본 파일의 외부 인터페이스(ftl_layout 자료구조와 region 정의). */
#include "ftl_layout.h"
/* [한국어] NV cache region 크기 계산용 — ftl_nv_cache_chunk_tail_md_num_blocks. */
#include "ftl_nv_cache.h"
/* [한국어] 슈퍼블록 영속화/로드 함수 선언. */
#include "ftl_sb.h"
/* [한국어] NV cache 디바이스 ops vtable — md_layout_ops를 통해 백엔드 종속 region 생성. */
#include "nvc/ftl_nvc_dev.h"
/* [한국어] bdev 위에 region을 트래킹/조회하는 헬퍼 — 레거시 layout 검증 시 사용. */
#include "utils/ftl_layout_tracker_bdev.h"
/* [한국어] layout 업그레이드(예: pre-v5 → v5) 헬퍼 — 새 region placeholder 추가용. */
#include "upgrade/ftl_layout_upgrade.h"

/*
 * [한국어]
 * ftl_layout_setup_mode - layout 설정 모드 enum.
 *
 * - FTL_LAYOUT_SETUP_MODE_LOAD_CURRENT: SB v5 이상에서 NV cache/base의 layout blob을 그대로 로드하여
 *   region 배열 복원. region 생성/이동 없음.
 * - FTL_LAYOUT_SETUP_MODE_NO_RESTRICT: 처음 생성(SPDK_FTL_MODE_CREATE) — 자유롭게 region 배치.
 * - FTL_LAYOUT_SETUP_MODE_LEGACY_DEFAULT: SB pre-v5 → v5 업그레이드 — pre-v5 시절의 정적 region 배치를
 *   재구성한 뒤 v5 SB로 변환.
 */
enum ftl_layout_setup_mode {
	FTL_LAYOUT_SETUP_MODE_LOAD_CURRENT = 0,
	/* [한국어] 기존 SB v5+의 layout blob을 그대로 로드하여 region 배열 복원. */
	FTL_LAYOUT_SETUP_MODE_NO_RESTRICT,
	/* [한국어] 처음 생성하는 경우 — region 자유 배치. */
	FTL_LAYOUT_SETUP_MODE_LEGACY_DEFAULT,
	/* [한국어] pre-v5 SB의 정적 layout을 호환 모드로 재구성. */
};

/*
 * [한국어]
 * blocks2mib - 블록 수를 MiB 단위 double로 변환(로그 표기용).
 *
 * @blocks: 블록 수.
 * @return: blocks * FTL_BLOCK_SIZE / (1024 * 1024) (단위: MiB).
 *
 * 단순 helper — 사람이 읽기 쉬운 단위로 region 크기를 출력.
 */
static inline double
blocks2mib(uint64_t blocks)
{
	double result;

	/* [한국어] 정수에서 double로 캐스팅 후 바이트 → KiB → MiB 변환. */
	result = blocks;
	result *= FTL_BLOCK_SIZE;
	result /= 1024UL;
	result /= 1024UL;

	return result;
}

/*
 * [한국어]
 * superblock_region_size - 슈퍼블록 region 크기(바이트).
 *
 * @dev: 디바이스.
 * @return: max(write_unit_size, FTL_SUPERBLOCK_SIZE)를 write_unit_size로 올림 정렬한 값.
 *
 * 슈퍼블록은 모든 region의 정렬 단위(alignment)이기도 해서, NVMe write_unit_size와 SB 자체 크기 중 더 큰
 * 쪽에 맞춰 정렬을 취한다. write_unit_size는 NVMe atomic write 단위와 관련 — atomic하게 쓰려면 이 단위로
 * 정렬되어야 한다.
 */
static uint64_t
superblock_region_size(struct spdk_ftl_dev *dev)
{
	/* [한국어] base bdev의 write_unit_size를 바이트 단위로 환산 — atomic write 단위. */
	const struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
	uint64_t wus = spdk_bdev_get_write_unit_size(bdev) * FTL_BLOCK_SIZE;

	if (wus > FTL_SUPERBLOCK_SIZE) {
		/* [한국어] NVMe write unit이 SB보다 크면 한 unit 전체가 region. */
		return wus;
	} else {
		/* [한국어] 그 외에는 SB 크기를 wus로 올림 정렬. */
		return spdk_round_up(FTL_SUPERBLOCK_SIZE, wus);
	}
}

/*
 * [한국어]
 * superblock_region_blocks - 슈퍼블록 region 크기(블록 단위).
 */
static uint64_t
superblock_region_blocks(struct spdk_ftl_dev *dev)
{
	/* [한국어] 바이트 → 블록 변환. */
	return superblock_region_size(dev) / FTL_BLOCK_SIZE;
}

/*
 * [한국어]
 * ftl_md_region_blocks - 임의 바이트 크기를 SB 정렬 기준 블록 수로 변환.
 *
 * @dev:   디바이스.
 * @bytes: region에 담을 메타데이터 총 바이트 수.
 * @return: SB region 크기로 정렬된 블록 수.
 *
 * 모든 메타데이터 region은 SB 정렬 단위로 시작/끝나야 검증을 통과하므로, region 크기 계산 시 이 함수를 사용.
 */
uint64_t
ftl_md_region_blocks(struct spdk_ftl_dev *dev, uint64_t bytes)
{
	/* [한국어] SB region 크기를 정렬 단위로 사용. */
	const uint64_t alignment = superblock_region_size(dev);
	uint64_t result;

	/* [한국어] bytes를 정렬 단위로 올림 후 블록으로 환산. */
	result = spdk_round_up(bytes, alignment);
	result /= FTL_BLOCK_SIZE;

	return result;
}

/*
 * [한국어]
 * ftl_md_region_align_blocks - 블록 수를 SB 정렬 단위(블록 단위)로 올림.
 *
 * 호출자가 이미 블록 단위로 계산해 둔 값을 정렬 단위에 맞춰 올림할 때 사용.
 */
uint64_t
ftl_md_region_align_blocks(struct spdk_ftl_dev *dev, uint64_t blocks)
{
	/* [한국어] SB region 블록 수를 정렬 단위로 사용. */
	const uint64_t alignment = superblock_region_blocks(dev);
	uint64_t result;

	/* [한국어] 단순 올림. */
	result = spdk_round_up(blocks, alignment);

	return result;
}

/*
 * [한국어]
 * ftl_md_region_name - region type enum을 사람이 읽을 수 있는 짧은 이름으로 변환.
 *
 * 디버그 로그/dump_region 등에서 region 식별에 사용.
 */
const char *
ftl_md_region_name(enum ftl_layout_region_type reg_type)
{
	/* [한국어] enum을 인덱스로 사용하는 문자열 테이블 — designated initializer로 안전한 매핑. */
	static const char *md_region_name[FTL_LAYOUT_REGION_TYPE_MAX] = {
		[FTL_LAYOUT_REGION_TYPE_SB] = "sb",
		/* [한국어] 슈퍼블록(NV cache 측, primary). */
		[FTL_LAYOUT_REGION_TYPE_SB_BASE] = "sb_mirror",
		/* [한국어] 슈퍼블록 미러(base 측). NV cache 손상 시 복구용. */
		[FTL_LAYOUT_REGION_TYPE_L2P] = "l2p",
		/* [한국어] L2P 맵 — LBA → 물리 주소 매핑. */
		[FTL_LAYOUT_REGION_TYPE_BAND_MD] = "band_md",
		/* [한국어] 밴드 메타데이터 — 각 밴드의 상태/checksum/seq_id. */
		[FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR] = "band_md_mirror",
		/* [한국어] 밴드 메타데이터 미러. */
		[FTL_LAYOUT_REGION_TYPE_VALID_MAP] = "vmap",
		/* [한국어] 디바이스 단위 valid bitmap. */
		[FTL_LAYOUT_REGION_TYPE_NVC_MD] = "nvc_md",
		/* [한국어] NV cache chunk 메타데이터. */
		[FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR] = "nvc_md_mirror",
		/* [한국어] NVC 메타 미러. */
		[FTL_LAYOUT_REGION_TYPE_DATA_NVC] = "data_nvc",
		/* [한국어] NV cache 데이터 영역(사용자 쓰기가 먼저 캐시되는 곳). */
		[FTL_LAYOUT_REGION_TYPE_DATA_BASE] = "data_btm",
		/* [한국어] base 데이터 영역(밴드들이 차지). */
		[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC] = "p2l0",
		/* [한국어] P2L 체크포인트 — GC용. */
		[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC_NEXT] = "p2l1",
		/* [한국어] P2L 체크포인트 — GC 다음 사이클용(double-buffering). */
		[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP] = "p2l2",
		/* [한국어] P2L 체크포인트 — compaction용. */
		[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP_NEXT] = "p2l3",
		/* [한국어] P2L 체크포인트 — compaction 다음 사이클용. */
		[FTL_LAYOUT_REGION_TYPE_TRIM_MD] = "trim_md",
		/* [한국어] TRIM 메타데이터. */
		[FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR] = "trim_md_mirror",
		[FTL_LAYOUT_REGION_TYPE_TRIM_LOG] = "trim_log",
		/* [한국어] TRIM 진행 로그(write-ahead). */
		[FTL_LAYOUT_REGION_TYPE_TRIM_LOG_MIRROR] = "trim_log_mirror",
		[FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MIN] = "p2l_log_io1",
		/* [한국어] P2L 로그 I/O — write 경로의 P2L 로깅용. */
		[FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MAX] = "p2l_log_io2",
	};
	const char *reg_name = md_region_name[reg_type];

	/* [한국어] enum 범위 + null 보장. */
	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);
	assert(reg_name != NULL);
	return reg_name;
}

/*
 * [한국어]
 * is_region_disabled - region이 비활성(미할당) 상태인지 검사.
 *
 * @return: blocks==0 && offset==INVALID이면 true.
 *
 * 모든 region이 항상 사용되지는 않으며(예: 미러가 없는 경우), 비활성은 검증/dump에서 건너뛴다.
 */
static bool
is_region_disabled(struct ftl_layout_region *region)
{
	/* [한국어] 두 조건 모두 만족 시 region은 미할당. */
	return region->current.blocks == 0 && region->current.offset == FTL_ADDR_INVALID;
}

/*
 * [한국어]
 * dump_region - 한 region의 offset/blocks를 사람이 읽기 쉬운 형식으로 로그.
 *
 * @dev:    디바이스(로그 prefix용).
 * @region: 출력할 region.
 *
 * 비활성 region은 건너뛰고, SB 정렬 검증 후 NOTICELOG 두 줄 출력(offset, blocks를 MiB로).
 */
static void
dump_region(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	if (is_region_disabled(region)) {
		/* [한국어] 비활성 region은 출력 생략. */
		return;
	}

	/* [한국어] offset/blocks가 SB 정렬 단위로 정렬되어 있어야 한다. 그렇지 않으면 atomic write 보장 깨짐. */
	assert(!(region->current.offset % superblock_region_blocks(dev)));
	assert(!(region->current.blocks % superblock_region_blocks(dev)));

	/* [한국어] region 이름 + 위치 + 크기 출력. */
	FTL_NOTICELOG(dev, "Region %s\n", region->name);
	FTL_NOTICELOG(dev, "	offset:                      %.2f MiB\n",
		      blocks2mib(region->current.offset));
	FTL_NOTICELOG(dev, "	blocks:                      %.2f MiB\n",
		      blocks2mib(region->current.blocks));
}

/*
 * [한국어]
 * ftl_validate_regions - 같은 bdev_desc를 공유하는 region들이 offset 범위에서 겹치지 않는지 검증.
 *
 * @dev:    디바이스.
 * @layout: layout 구조체.
 * @return: 0 정상, -1 겹침 발견(에러 로그 출력).
 *
 * 알고리즘: 모든 (i, j) 쌍에 대해 (i != j이고 같은 bdev_desc일 때) [r1.begin, r1.end]와 [r2.begin, r2.end]
 * 가 교차하는지 검사. 교차 조건: max(begin) <= min(end).
 */
int
ftl_validate_regions(struct spdk_ftl_dev *dev, struct ftl_layout *layout)
{
	enum ftl_layout_region_type i, j;

	/* Validate if regions doesn't overlap each other  */
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; i++) {
		struct ftl_layout_region *r1 = ftl_layout_region_get(dev, i);

		if (!r1 || is_region_disabled(r1)) {
			/* [한국어] 미설정/비활성은 건너뜀. */
			continue;
		}

		for (j = 0; j < FTL_LAYOUT_REGION_TYPE_MAX; j++) {
			struct ftl_layout_region *r2 = ftl_layout_region_get(dev, j);

			if (!r2 || is_region_disabled(r2)) {
				continue;
			}

			if (r1->bdev_desc != r2->bdev_desc) {
				/* [한국어] 다른 bdev면 주소 공간이 분리 — 겹침 검사 의미 없음. */
				continue;
			}

			if (i == j) {
				/* [한국어] 같은 region 자기 자신 — 건너뜀. */
				continue;
			}

			/* [한국어] 두 region의 [begin, end] 구간 계산. */
			uint64_t r1_begin = r1->current.offset;
			uint64_t r1_end = r1->current.offset + r1->current.blocks - 1;
			uint64_t r2_begin = r2->current.offset;
			uint64_t r2_end = r2->current.offset + r2->current.blocks - 1;

			/* [한국어] 교차 검사: max(begin) <= min(end)이면 겹침. */
			if (spdk_max(r1_begin, r2_begin) <= spdk_min(r1_end, r2_end)) {
				FTL_ERRLOG(dev, "Layout initialization ERROR, two regions overlap, "
					   "%s and %s\n", r1->name, r2->name);
				return -1;
			}
		}
	}

	return 0;
}

/*
 * [한국어]
 * get_num_user_lbas - 사용자에게 노출되는 LBA 수 계산.
 *
 * @dev: 디바이스.
 * @return: 전체 base 블록 수에서 over-provisioning 비율을 뺀 값.
 *
 * over-provisioning(OP)은 GC를 위한 여유 공간으로 예비된 비율. 보통 10~20% 정도. 사용자에게 보이는
 * LBA 공간은 (base 전체) * (1 - OP/100).
 */
static uint64_t
get_num_user_lbas(struct spdk_ftl_dev *dev)
{
	uint64_t blocks;

	/* [한국어] base 디바이스의 밴드 총 블록 수. */
	blocks = dev->num_bands * ftl_get_num_blocks_in_band(dev);
	/* [한국어] OP 만큼 차감 — 정수 곱셈으로 부동소수점 오차 회피. */
	blocks = (blocks * (100 - dev->conf.overprovisioning)) / 100;

	return blocks;
}

/*
 * [한국어]
 * ftl_layout_region_get - region type으로 layout->region[reg_type] 포인터 얻기.
 *
 * @return: 해당 region 포인터(type이 일치하면), 미설정이면 NULL.
 *
 * type 검증으로 INVALID 상태의 region 사용을 방지.
 */
struct ftl_layout_region *
ftl_layout_region_get(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type)
{
	struct ftl_layout_region *reg = &dev->layout.region[reg_type];

	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);
	/* [한국어] 활성 region만 반환 — INVALID로 마킹된 슬롯은 NULL. */
	return reg->type == reg_type ? reg : NULL;
}

/*
 * [한국어]
 * ftl_layout_base_offset - base device의 데이터 영역 끝 offset(=valid map 시작 위치).
 *
 * 모든 밴드 데이터가 차지하는 총 블록 수를 반환. base bdev의 [0, ftl_layout_base_offset)이 데이터,
 * 그 뒤에 valid map이 배치된다.
 */
uint64_t
ftl_layout_base_offset(struct spdk_ftl_dev *dev)
{
	/* [한국어] num_bands * 밴드 블록 수 — base에 데이터를 위해 예약된 영역의 끝. */
	return dev->num_bands * ftl_get_num_blocks_in_band(dev);
}

/*
 * [한국어]
 * layout_region_create_nvc - NV cache 위에 region을 새로 생성하고 open.
 *
 * @dev/@reg_type/@reg_version: 생성할 region 정보.
 * @entry_size, @entry_count: 한 entry 크기와 entry 개수.
 *
 * NV cache 백엔드의 md_layout_ops에 위임 — 백엔드(예: chunked nvc, log-on-bdev 등)에 따라 region 배치
 * 전략이 다르다.
 */
static int
layout_region_create_nvc(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
			 uint32_t reg_version, size_t entry_size, size_t entry_count)
{
	/* [한국어] NV cache 백엔드의 region 생성 ops. */
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;
	/* [한국어] region이 차지할 블록 수 = ceil(총 바이트 / SB 정렬). */
	size_t reg_blks = ftl_md_region_blocks(dev, entry_count * entry_size);

	/* [한국어] region 공간 할당(트래커에 등록). */
	if (md_ops->region_create(dev, reg_type, reg_version, reg_blks)) {
		return -1;
	}
	/* [한국어] region 메타데이터(layout->region[reg_type])에 entry_size/count/bdev_desc/ioch 채우기. */
	if (md_ops->region_open(dev, reg_type, reg_version, entry_size, entry_count,
				&dev->layout.region[reg_type])) {
		return -1;
	}
	return 0;
}

/*
 * [한국어]
 * layout_region_create_base - base 디바이스 위에 region 생성/open.
 *
 * NV cache 버전과 동일 패턴. base_type ops를 사용한다는 차이만 있음.
 */
static int
layout_region_create_base(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
			  uint32_t reg_version, size_t entry_size, size_t entry_count)
{
	/* [한국어] base 디바이스의 md_layout_ops. */
	const struct ftl_md_layout_ops *md_ops = &dev->base_type->ops.md_layout_ops;
	size_t reg_blks = ftl_md_region_blocks(dev, entry_count * entry_size);

	/* [한국어] base 디바이스에 region 공간 할당. */
	if (md_ops->region_create(dev, reg_type, reg_version, reg_blks)) {
		return -1;
	}
	/* [한국어] region open(메타데이터 채움). */
	if (md_ops->region_open(dev, reg_type, reg_version, entry_size, entry_count,
				&dev->layout.region[reg_type])) {
		return -1;
	}
	return 0;
}

/*
 * [한국어]
 * legacy_layout_verify_region - 레거시 layout 트래커에서 reg_type을 단 한 번만 발견하는지 검증.
 *
 * pre-v5 SB는 region을 트래커에 정확히 한 번만 등록하므로, 두 번 이상 발견되면 형식 오류.
 * find_next_region을 반복 호출해 연속 발견된 entry가 동일하다면 OK.
 */
static void
legacy_layout_verify_region(struct ftl_layout_tracker_bdev *layout_tracker,
			    enum ftl_layout_region_type reg_type, uint32_t reg_version)
{
	const struct ftl_layout_tracker_bdev_region_props *reg_search_ctx = NULL;
	const struct ftl_layout_tracker_bdev_region_props *reg_found = NULL;

	while (true) {
		/* [한국어] reg_type의 다음 인스턴스를 찾음(없으면 reg_search_ctx=NULL). */
		ftl_layout_tracker_bdev_find_next_region(layout_tracker, reg_type, &reg_search_ctx);
		if (!reg_search_ctx) {
			break;
		}

		/* Only a single region version is present in upgrade from the legacy layout */
		/* [한국어] 레거시는 단일 버전만 — 다른 버전이 들어 있으면 코드 가정 위반. */
		ftl_bug(reg_search_ctx->ver != reg_version);
		/* [한국어] 두 번 이상 발견되면 중복 — 레거시 가정 위반. */
		ftl_bug(reg_found != NULL);

		reg_found = reg_search_ctx;
	}
}

/*
 * [한국어]
 * legacy_layout_region_open_nvc - 레거시 NV cache region을 검증 후 open만 수행.
 *
 * 레거시는 region이 이미 트래커에 등록되어 있으므로 새로 create하지 않고, 기존 등록 정보를 그대로 받아
 * region 메타데이터를 채운다(open).
 */
static int
legacy_layout_region_open_nvc(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
			      uint32_t reg_version, size_t entry_size, size_t entry_count)
{
	struct ftl_layout_region *reg = &dev->layout.region[reg_type];
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;

	/* [한국어] 레거시 가정 검증. */
	legacy_layout_verify_region(dev->nvc_layout_tracker, reg_type, reg_version);
	/* [한국어] open만 수행 — create는 이미 되어 있다고 가정. */
	return md_ops->region_open(dev, reg_type, reg_version, entry_size, entry_count, reg);
}

/*
 * [한국어]
 * legacy_layout_region_open_base - base 디바이스용 레거시 open 함수.
 *
 * NB: 검증은 nvc_layout_tracker에서 하고 open은 base ops로 — 이는 코드의 일관성 조정 결과로 보이며,
 * 사실상 두 백엔드 트래커 정보가 동일한 region 정보를 공유하는 케이스를 가정.
 */
static int
legacy_layout_region_open_base(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
			       uint32_t reg_version, size_t entry_size, size_t entry_count)
{
	struct ftl_layout_region *reg = &dev->layout.region[reg_type];
	const struct ftl_md_layout_ops *md_ops = &dev->base_type->ops.md_layout_ops;

	/* [한국어] 레거시 검증(공유된 nvc 트래커 사용). */
	legacy_layout_verify_region(dev->nvc_layout_tracker, reg_type, reg_version);
	return md_ops->region_open(dev, reg_type, reg_version, entry_size, entry_count, reg);
}

/*
 * [한국어]
 * layout_setup_legacy_default_nvc - pre-v5 NV cache 레거시 layout을 v5 region 배열로 재구성.
 *
 * 절차: L2P → BAND_MD/MIRROR → P2L_CKPT_* → TRIM_MD/MIRROR → NVC_MD/MIRROR → DATA_NVC 순서로
 * 각 region을 open. chunk_count는 DATA_NVC 크기에서 역산. 끝에 TRIM_LOG/MIRROR placeholder 추가.
 */
static int
layout_setup_legacy_default_nvc(struct spdk_ftl_dev *dev)
{
	int region_type;
	uint64_t blocks, chunk_count;
	struct ftl_layout *layout = &dev->layout;
	const struct ftl_layout_tracker_bdev_region_props *reg_search_ctx = NULL;

	/* Initialize L2P region */
	/* [한국어] L2P 크기 = (LBA 수 * 한 entry 바이트). entry는 4 또는 8바이트. */
	blocks = ftl_md_region_blocks(dev, layout->l2p.addr_size * dev->num_lbas);
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_L2P, 0, FTL_BLOCK_SIZE,
					  blocks)) {
		goto error;
	}

	/* Initialize band info metadata */
	/* [한국어] 밴드 메타 entry = struct ftl_band_md, 개수 = 밴드 수. */
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD, FTL_BAND_VERSION_1,
					  sizeof(struct ftl_band_md), ftl_get_num_bands(dev))) {
		goto error;
	}

	/* Initialize band info metadata mirror */
	/* [한국어] 동일 정보의 미러 — NV cache 손상 시 복구용. */
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR, FTL_BAND_VERSION_1,
					  sizeof(struct ftl_band_md), ftl_get_num_bands(dev))) {
		goto error;
	}
	/* [한국어] 본체 region에 mirror_type 연결 — ftl_md가 자동 미러 동기화에 사용. */
	layout->region[FTL_LAYOUT_REGION_TYPE_BAND_MD].mirror_type = FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR;

	/*
	 * Initialize P2L checkpointing regions
	 */
	/* [한국어] P2L 체크포인트 region들(GC/GC_NEXT/COMP/COMP_NEXT) 4종. 레거시는 이미 트래커에 크기가 등록됨. */
	for (region_type = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     region_type <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX;
	     region_type++) {
		const struct ftl_layout_tracker_bdev_region_props *reg_search_ctx = NULL;

		/* Get legacy number of blocks */
		/* [한국어] 트래커에서 이 region의 등록된 블록 수를 가져옴. */
		ftl_layout_tracker_bdev_find_next_region(dev->nvc_layout_tracker, region_type, &reg_search_ctx);
		if (!reg_search_ctx || reg_search_ctx->ver != FTL_P2L_VERSION_1) {
			goto error;
		}
		blocks = reg_search_ctx->blk_sz;

		/* [한국어] 그 크기로 region open. */
		if (legacy_layout_region_open_nvc(dev, region_type, FTL_P2L_VERSION_1, FTL_BLOCK_SIZE, blocks)) {
			goto error;
		}
	}

	/*
	 * Initialize trim metadata region
	 */
	/* [한국어] trim md 크기는 L2P region 크기와 동일하게(해당 LBA 범위와 매칭). */
	blocks = layout->region[FTL_LAYOUT_REGION_TYPE_L2P].current.blocks;
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_TRIM_MD, 0, sizeof(uint64_t),
					  blocks)) {
		goto error;
	}

	/* Initialize trim metadata mirror region */
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR, 0, sizeof(uint64_t),
					  blocks)) {
		goto error;
	}
	layout->region[FTL_LAYOUT_REGION_TYPE_TRIM_MD].mirror_type = FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR;

	/* Restore chunk count */
	/* [한국어] DATA_NVC region 크기에서 chunk 수 역산 — chunk_count = data 블록 수 / 밴드 블록 수. */
	ftl_layout_tracker_bdev_find_next_region(dev->nvc_layout_tracker, FTL_LAYOUT_REGION_TYPE_DATA_NVC,
			&reg_search_ctx);
	if (!reg_search_ctx || reg_search_ctx->ver != 0) {
		goto error;
	}
	blocks = reg_search_ctx->blk_sz;
	chunk_count = blocks / ftl_get_num_blocks_in_band(dev);
	if (0 == chunk_count) {
		/* [한국어] chunk 수 0이면 NV cache 크기 부족 — 에러. */
		goto error;
	}

	/*
	 * Initialize NV Cache metadata
	 */
	/* [한국어] NVC 메타: chunk마다 한 entry. */
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_NVC_MD, FTL_NVC_VERSION_1,
					  sizeof(struct ftl_nv_cache_chunk_md), chunk_count)) {
		goto error;
	}

	/*
	 * Initialize NV Cache metadata mirror
	 */
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR, FTL_NVC_VERSION_1,
					  sizeof(struct ftl_nv_cache_chunk_md), chunk_count)) {
		goto error;
	}
	layout->region[FTL_LAYOUT_REGION_TYPE_NVC_MD].mirror_type = FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR;

	/*
	 * Initialize data region on NV cache
	 */
	/* [한국어] DATA_NVC: entry_size = chunk_data_blocks * 블록 크기, 개수 = chunk_count. */
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_DATA_NVC, 0,
					  layout->nvc.chunk_data_blocks * FTL_BLOCK_SIZE, chunk_count)) {
		goto error;
	}

	/* Here is the place to add necessary region placeholders for the creation of new regions */
	/* [한국어] v5에서 추가된 TRIM_LOG/MIRROR region이 pre-v5에는 없으므로 placeholder로 등록 — 이후 업그레이드
	 *         과정에서 적절한 위치에 채워짐. */
	ftl_layout_upgrade_add_region_placeholder(dev, dev->nvc_layout_tracker,
			FTL_LAYOUT_REGION_TYPE_TRIM_LOG);
	ftl_layout_upgrade_add_region_placeholder(dev, dev->nvc_layout_tracker,
			FTL_LAYOUT_REGION_TYPE_TRIM_LOG_MIRROR);

	return 0;

error:
	/* [한국어] 에러 경로: 어디에서 실패했든 단일 메시지로 보고. */
	FTL_ERRLOG(dev, "Invalid legacy NV Cache metadata layout\n");
	return -1;
}

/*
 * [한국어]
 * layout_setup_legacy_default_base - pre-v5 base 디바이스 레거시 layout 구성.
 *
 * base 디바이스 레이아웃: [SB | data | valid map]. SB는 ftl_layout_setup_superblock에서 별도로 처리.
 * 본 함수는 DATA_BASE region(밴드 데이터) + VALID_MAP region을 구성.
 */
static int
layout_setup_legacy_default_base(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;

	/* Base device layout is as follows:
	 * - superblock
	 * - data
	 * - valid map
	 */
	/* [한국어] DATA_BASE: 모든 밴드의 데이터 블록 수만큼 차지. */
	if (layout_region_create_base(dev, FTL_LAYOUT_REGION_TYPE_DATA_BASE, 0, FTL_BLOCK_SIZE,
				      ftl_layout_base_offset(dev))) {
		return -1;
	}

	/* [한국어] VALID_MAP: 비트맵 — 1비트/블록. base + nvc 전체 블록 / 8 바이트. */
	if (legacy_layout_region_open_base(dev, FTL_LAYOUT_REGION_TYPE_VALID_MAP, 0, FTL_BLOCK_SIZE,
					   ftl_md_region_blocks(dev, spdk_divide_round_up(layout->base.total_blocks + layout->nvc.total_blocks,
							   8)))) {
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * layout_setup_legacy_default - 레거시 nvc + base 둘 다 구성.
 *
 * 두 단계 중 하나라도 실패하면 -1.
 */
static int
layout_setup_legacy_default(struct spdk_ftl_dev *dev)
{
	if (layout_setup_legacy_default_nvc(dev) || layout_setup_legacy_default_base(dev)) {
		return -1;
	}
	return 0;
}

/*
 * [한국어]
 * layout_setup_default_nvc - 새 NV cache layout 생성(처음 생성 모드 또는 v5 신규 디바이스).
 *
 * legacy_default와 거의 동일 구조이지만, region_create로 새 영역을 할당하고(legacy는 trakcer에 이미 있는
 * 것 사용), TRIM_LOG/MIRROR가 정상 region으로 포함된다. 또 chunk_count가 layout->nvc에 직접 저장되어
 * 있다고 가정.
 */
static int
layout_setup_default_nvc(struct spdk_ftl_dev *dev)
{
	int region_type;
	uint64_t blocks;
	struct ftl_layout *layout = &dev->layout;

	/* Initialize L2P region */
	/* [한국어] L2P region 새 생성. */
	blocks = ftl_md_region_blocks(dev, layout->l2p.addr_size * dev->num_lbas);
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_L2P, 0, FTL_BLOCK_SIZE, blocks)) {
		goto error;
	}

	/* Initialize band info metadata */
	/* [한국어] 현재 버전(BAND_VERSION_CURRENT)으로 BAND_MD 생성. */
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD, FTL_BAND_VERSION_CURRENT,
				     sizeof(struct ftl_band_md), ftl_get_num_bands(dev))) {
		goto error;
	}

	/* Initialize band info metadata mirror */
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR, FTL_BAND_VERSION_CURRENT,
				     sizeof(struct ftl_band_md), ftl_get_num_bands(dev))) {
		goto error;
	}
	layout->region[FTL_LAYOUT_REGION_TYPE_BAND_MD].mirror_type = FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR;

	/*
	 * Initialize P2L checkpointing regions
	 */
	for (region_type = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     region_type <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX;
	     region_type++) {
		/* [한국어] P2L 체크포인트 4개 모두 동일한 ckpt_pages 크기로 생성(double-buffering 위함). */
		if (layout_region_create_nvc(dev, region_type, FTL_P2L_VERSION_CURRENT, FTL_BLOCK_SIZE,
					     layout->p2l.ckpt_pages)) {
			goto error;
		}
	}

	/*
	 * Initialize trim metadata region
	 */
	/* [한국어] L2P 블록 수 → 8바이트 entry로 변환된 크기를 다시 블록 단위로. */
	blocks = layout->region[FTL_LAYOUT_REGION_TYPE_L2P].current.blocks;
	blocks = spdk_divide_round_up(blocks * sizeof(uint64_t), FTL_BLOCK_SIZE);
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_TRIM_MD, 0, FTL_BLOCK_SIZE, blocks)) {
		goto error;
	}

	/* Initialize trim metadata mirror region */
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR, 0, FTL_BLOCK_SIZE,
				     blocks)) {
		goto error;
	}
	layout->region[FTL_LAYOUT_REGION_TYPE_TRIM_MD].mirror_type = FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR;

	/*
	 * Initialize trim log region
	 */
	/* [한국어] TRIM_LOG: 진행 중 trim의 write-ahead log. entry 1개. */
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_TRIM_LOG, FTL_TRIM_LOG_VERSION_CURRENT,
				     sizeof(struct ftl_trim_log), 1)) {
		goto error;
	}

	/* Initialize trim log mirror region */
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_TRIM_LOG_MIRROR,
				     FTL_TRIM_LOG_VERSION_CURRENT,
				     sizeof(struct ftl_trim_log), 1)) {
		goto error;
	}
	layout->region[FTL_LAYOUT_REGION_TYPE_TRIM_LOG].mirror_type =
		FTL_LAYOUT_REGION_TYPE_TRIM_LOG_MIRROR;

	/*
	 * Initialize NV Cache metadata
	 */
	if (0 == layout->nvc.chunk_count) {
		/* [한국어] chunk_count가 0이면 NV cache가 너무 작음 — 에러. */
		goto error;
	}
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_NVC_MD, FTL_NVC_VERSION_CURRENT,
				     sizeof(struct ftl_nv_cache_chunk_md), layout->nvc.chunk_count)) {
		goto error;
	}

	/*
	 * Initialize NV Cache metadata mirror
	 */
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR, FTL_NVC_VERSION_CURRENT,
				     sizeof(struct ftl_nv_cache_chunk_md), layout->nvc.chunk_count)) {
		goto error;
	}
	layout->region[FTL_LAYOUT_REGION_TYPE_NVC_MD].mirror_type = FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR;

	if (dev->nv_cache.nvc_type->ops.setup_layout) {
		/* [한국어] NV cache 백엔드별 추가 setup 훅 — DATA_NVC 등 백엔드 종속 region을 생성. */
		return dev->nv_cache.nvc_type->ops.setup_layout(dev);
	}

	return 0;

error:
	FTL_ERRLOG(dev, "Insufficient NV Cache capacity to preserve metadata\n");
	return -1;
}

/*
 * [한국어]
 * layout_setup_default_base - 신규 base 디바이스 layout 구성.
 *
 * legacy_default_base와 동일 — DATA_BASE + VALID_MAP. valid_map 크기 계산만 명시적.
 */
static int
layout_setup_default_base(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;
	uint64_t valid_map_size;

	/* Base device layout is as follows:
	 * - superblock
	 * - data
	 * - valid map
	 */
	/* [한국어] DATA_BASE 생성 — 모든 밴드의 데이터 블록 합. */
	if (layout_region_create_base(dev, FTL_LAYOUT_REGION_TYPE_DATA_BASE, 0, FTL_BLOCK_SIZE,
				      ftl_layout_base_offset(dev))) {
		return -1;
	}

	/* [한국어] valid bitmap: base + nvc 전체 블록당 1비트 → 바이트 → 블록 변환. */
	valid_map_size = spdk_divide_round_up(layout->base.total_blocks + layout->nvc.total_blocks, 8);
	if (layout_region_create_base(dev, FTL_LAYOUT_REGION_TYPE_VALID_MAP, 0, FTL_BLOCK_SIZE,
				      ftl_md_region_blocks(dev, valid_map_size))) {
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * layout_setup_default - 신규 nvc + base layout 구성 wrapper.
 */
static int
layout_setup_default(struct spdk_ftl_dev *dev)
{
	if (layout_setup_default_nvc(dev) || layout_setup_default_base(dev)) {
		return -1;
	}
	return 0;
}

/*
 * [한국어]
 * layout_load - SB v5+에서 layout blob을 SB로부터 로드해 region 배열 복원.
 *
 * 두 단계: (1) SB blob 영역(layout 정보) 로드, (2) 각 region에 적용. 신규 region 생성 없음.
 */
static int
layout_load(struct spdk_ftl_dev *dev)
{
	/* [한국어] SB의 blob 영역(layout 직렬화 데이터)을 디스크에서 메모리로 로드. */
	if (ftl_superblock_load_blob_area(dev)) {
		return -1;
	}
	/* [한국어] 로드한 blob을 dev->layout.region 배열에 적용 — region 메타 복원. */
	if (ftl_superblock_md_layout_apply(dev)) {
		return -1;
	}
	return 0;
}

/*
 * [한국어]
 * ftl_layout_setup - layout 설정 진입점. 모드 결정 → region 생성/로드 → 검증 → SB 저장.
 *
 * @dev: 디바이스.
 * @return: 0 성공, -EINVAL 실패.
 *
 * 처리 분기:
 *  (1) MODE_CREATE: NO_RESTRICT — 새 layout 생성 자유.
 *  (2) SB blob 영역이 비어 있음(pre-v5): LEGACY_DEFAULT — 정적 layout 재구성.
 *  (3) 그 외: LOAD_CURRENT — SB v5+의 blob을 그대로 로드.
 *
 * 그 후 모든 region을 INVALID로 마킹 → L2P 파라미터(addr_length/size/lbas_in_page) 계산 →
 * P2L ckpt_pages, NV cache chunk_count 등 파생값 계산 → 모드별 setup → 중첩 검증 → SB blob 저장.
 */
int
ftl_layout_setup(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;
	uint64_t i;
	uint64_t num_lbas;
	enum ftl_layout_setup_mode setup_mode;
	int rc;

	/*
	 * SB v5 adds the ability to create MD regions dynamically, i.e. depending on the underlying device type.
	 * For compatibility reasons:
	 * 1. When upgrading from pre-v5 SB, only the legacy default layout is created.
	 *    Pre-v5: some regions were static and not stored in the SB layout. These must be created to match
	 *            the legacy default layout.
	 *    v5: all regions are stored in the SB layout. Upon the SB upgrade, the legacy default layout
	 *        is updated with pre-v5 layout stored in the SB. The whole layout is then stored in v5 SB.
	 *
	 * 2. When SB v5 or later was loaded, the layout is instantiated from the nvc and base layout blobs.
	 *    No default layout is created.
	 *
	 * 3. When the FTL layout is being created for the first time, there are no restrictions.
	 *
	 * Any new regions to be created in cases (1) and (2) can only be placed in the unallocated area
	 * of the underlying device.
	 */

	/* [한국어] 모드 결정 — 위 세 분기. */
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		setup_mode = FTL_LAYOUT_SETUP_MODE_NO_RESTRICT;
	} else if (ftl_superblock_is_blob_area_empty(dev->sb)) {
		/* [한국어] SB의 blob 영역이 비어 있음 → pre-v5에서 업그레이드. */
		setup_mode = FTL_LAYOUT_SETUP_MODE_LEGACY_DEFAULT;
	} else {
		/* [한국어] SB v5+에 layout blob이 있음 → 그대로 로드. */
		setup_mode = FTL_LAYOUT_SETUP_MODE_LOAD_CURRENT;
	}
	FTL_NOTICELOG(dev, "FTL layout setup mode %d\n", (int)setup_mode);

	/* Invalidate all regions */
	/* [한국어] SB/SB_BASE를 제외한 모든 region을 INVALID로 초기화 — 이후 mode별 setup이 채움. */
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; ++i) {
		if (i == FTL_LAYOUT_REGION_TYPE_SB || i == FTL_LAYOUT_REGION_TYPE_SB_BASE) {
			/* Super block has been already initialized */
			/* [한국어] SB는 이미 ftl_layout_setup_superblock에서 초기화됨 — 건드리지 않음. */
			continue;
		}

		layout->region[i].mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
		/* Mark the region inactive */
		/* [한국어] type을 INVALID로 — ftl_layout_region_get가 NULL 반환하도록. */
		layout->region[i].type = FTL_LAYOUT_REGION_TYPE_INVALID;
	}

	/*
	 * Initialize L2P information
	 */
	/* [한국어] 사용자 LBA 수 = base 전체 - OP. */
	num_lbas = get_num_user_lbas(dev);
	if (dev->num_lbas == 0) {
		/* [한국어] 처음 생성: dev->num_lbas와 SB lba_cnt 둘 다 채움. */
		assert(dev->conf.mode & SPDK_FTL_MODE_CREATE);
		dev->num_lbas = num_lbas;
		dev->sb->lba_cnt = num_lbas;
	} else if (dev->num_lbas != num_lbas) {
		/* [한국어] 기존 디바이스의 lba_cnt와 현재 계산 결과가 다르면 디바이스 크기/구성 변경 — 안전 종료. */
		FTL_ERRLOG(dev, "Mismatched FTL num_lbas\n");
		return -EINVAL;
	}
	/* [한국어] 주소 길이(bit) = ceil(log2(전체 블록 수)) + 1. */
	layout->l2p.addr_length = spdk_u64log2(layout->base.total_blocks + layout->nvc.total_blocks) + 1;
	/* [한국어] 32비트 이내면 4바이트, 초과면 8바이트 entry. */
	layout->l2p.addr_size = layout->l2p.addr_length > 32 ? 8 : 4;
	/* [한국어] 한 페이지(블록) 안에 들어갈 LBA entry 수. */
	layout->l2p.lbas_in_page = FTL_BLOCK_SIZE / layout->l2p.addr_size;

	/* Setup P2L ckpt */
	/* [한국어] xfer_size를 P2L entry 수로 나눠 한 xfer가 몇 페이지 P2L 데이터를 만드는지 계산. */
	layout->p2l.pages_per_xfer = spdk_divide_round_up(dev->xfer_size, FTL_NUM_P2L_ENTRIES_NO_VSS);
	/* [한국어] 한 밴드당 P2L 체크포인트 페이지 수 = ceil(밴드 블록 수 / xfer_size) * pages_per_xfer. */
	layout->p2l.ckpt_pages = spdk_divide_round_up(ftl_get_num_blocks_in_band(dev),
				 dev->xfer_size) * layout->p2l.pages_per_xfer;

	/* [한국어] NV cache 한 chunk = 한 밴드 크기. */
	layout->nvc.chunk_data_blocks = ftl_get_num_blocks_in_band(dev);
	layout->nvc.chunk_count = layout->nvc.total_blocks / ftl_get_num_blocks_in_band(dev);
	layout->nvc.chunk_tail_md_num_blocks = ftl_nv_cache_chunk_tail_md_num_blocks(&dev->nv_cache);

	/* [한국어] base 사용 가능 블록과 사용자 블록 수 캐시. */
	layout->base.num_usable_blocks = ftl_get_num_blocks_in_band(dev);
	layout->base.user_blocks = ftl_band_user_blocks(dev->bands);

	/* [한국어] 모드별 layout 구성 디스패치. */
	switch (setup_mode) {
	case FTL_LAYOUT_SETUP_MODE_LEGACY_DEFAULT:
		if (layout_setup_legacy_default(dev)) {
			return -EINVAL;
		}
		break;

	case FTL_LAYOUT_SETUP_MODE_LOAD_CURRENT:
		if (layout_load(dev)) {
			return -EINVAL;
		}
		break;

	case FTL_LAYOUT_SETUP_MODE_NO_RESTRICT:
		if (layout_setup_default(dev)) {
			return -EINVAL;
		}
		break;

	default:
		/* [한국어] enum 외 값 — 코드 버그. */
		ftl_abort();
		break;
	}

	/* [한국어] region 중첩 검증. */
	if (ftl_validate_regions(dev, layout)) {
		return -EINVAL;
	}

	/* [한국어] layout 정보를 SB blob 영역에 직렬화 저장 — 다음 부팅 시 LOAD_CURRENT 경로에 사용. */
	rc = ftl_superblock_store_blob_area(dev);

	/* [한국어] 사람이 읽을 수 있는 layout 요약 로그. */
	FTL_NOTICELOG(dev, "Base device capacity:         %.2f MiB\n",
		      blocks2mib(layout->base.total_blocks));
	FTL_NOTICELOG(dev, "NV cache device capacity:       %.2f MiB\n",
		      blocks2mib(layout->nvc.total_blocks));
	FTL_NOTICELOG(dev, "L2P entries:                    %"PRIu64"\n", dev->num_lbas);
	FTL_NOTICELOG(dev, "L2P address size:               %"PRIu64"\n", layout->l2p.addr_size);
	FTL_NOTICELOG(dev, "P2L checkpoint pages:           %"PRIu64"\n", layout->p2l.ckpt_pages);
	FTL_NOTICELOG(dev, "NV cache chunk count            %"PRIu64"\n", dev->layout.nvc.chunk_count);

	return rc;
}

/*
 * [한국어]
 * ftl_layout_setup_superblock - 부팅 가장 앞에서 호출되어 SB와 SB_BASE region을 생성.
 *
 * @dev:    디바이스.
 * @return: 0 성공, -1 실패.
 *
 * 절차:
 *  (1) base/nvc bdev에서 num_blocks 가져와 layout->base/nvc.total_blocks에 저장.
 *  (2) SB region(NV cache) 생성 — entry_size = SB region 크기, entry 1개.
 *  (3) SB_BASE region(base 미러) 생성 — 동일 크기.
 *  (4) SB ↔ SB_BASE 미러 연결.
 *  (5) base 디바이스에 SB가 저장 가능한지(끝부분에 공간 있는지) 검증.
 */
int
ftl_layout_setup_superblock(struct spdk_ftl_dev *dev)
{
	const struct spdk_bdev *bdev;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = &layout->region[FTL_LAYOUT_REGION_TYPE_SB];
	uint64_t total_blocks, offset, left;

	/* [한국어] SB 메타데이터 객체가 아직 없어야(중복 초기화 방지). */
	assert(layout->md[FTL_LAYOUT_REGION_TYPE_SB] == NULL);

	/* [한국어] base bdev 총 블록 수 캐시. */
	bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
	layout->base.total_blocks = spdk_bdev_get_num_blocks(bdev);

	/* [한국어] NV cache bdev 총 블록 수 캐시. */
	bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);
	layout->nvc.total_blocks = spdk_bdev_get_num_blocks(bdev);

	/* Initialize superblock region */
	/* [한국어] 1차 SB(NV cache) 생성 — entry 1개, 크기는 SB region 크기. */
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_SB, FTL_SB_VERSION_CURRENT,
				     superblock_region_size(dev), 1)) {
		FTL_ERRLOG(dev, "Error when setting up primary super block\n");
		return -1;
	}

	/* [한국어] open 후 bdev_desc/ioch가 채워졌는지, offset이 0인지(NV cache 시작점) 검증. */
	assert(region->bdev_desc != NULL);
	assert(region->ioch != NULL);
	assert(region->current.offset == 0);

	/* [한국어] 2차 SB(base 미러) — base bdev에 SB 사본을 두어 NV cache 손상 시 복구 가능. */
	if (layout_region_create_base(dev, FTL_LAYOUT_REGION_TYPE_SB_BASE, FTL_SB_VERSION_CURRENT,
				      superblock_region_size(dev), 1)) {
		FTL_ERRLOG(dev, "Error when setting up secondary super block\n");
		return -1;
	}
	/* [한국어] 1차 SB의 mirror_type을 SB_BASE로 연결 — ftl_md가 자동 미러 동기화에 사용. */
	layout->region[FTL_LAYOUT_REGION_TYPE_SB].mirror_type = FTL_LAYOUT_REGION_TYPE_SB_BASE;

	region = &layout->region[FTL_LAYOUT_REGION_TYPE_SB_BASE];
	/* [한국어] base 측 SB도 offset 0 — base bdev의 시작 부분에 위치. */
	assert(region->current.offset == 0);

	/* Check if SB can be stored at the end of base device */
	/* [한국어] base 디바이스에 SB가 잘 들어가는지 끝부분 위치 검증. underflow/overflow 방지. */
	total_blocks = spdk_bdev_get_num_blocks(
			       spdk_bdev_desc_get_bdev(dev->base_bdev_desc));
	offset = region->current.offset + region->current.blocks;
	left = total_blocks - offset;
	if ((left > total_blocks) || (offset > total_blocks)) {
		/* [한국어] underflow(left > total) 또는 overflow(offset > total) — 디바이스가 너무 작음. */
		FTL_ERRLOG(dev, "Error when setup base device super block\n");
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * ftl_layout_clear_superblock - SB region을 트래커에서 제거(언마운트/정리 단계).
 *
 * 새로 생성될 디바이스를 위해 기존 SB 등록 해제. NV cache와 base 둘 다.
 */
int
ftl_layout_clear_superblock(struct spdk_ftl_dev *dev)
{
	int rc;

	/* [한국어] NV cache 측 SB 트래커 제거. */
	rc = ftl_layout_tracker_bdev_rm_region(dev->nvc_layout_tracker, FTL_LAYOUT_REGION_TYPE_SB,
					       FTL_SB_VERSION_CURRENT);
	if (rc) {
		return rc;
	}

	/* [한국어] base 측 SB_BASE 트래커 제거. */
	return ftl_layout_tracker_bdev_rm_region(dev->base_layout_tracker, FTL_LAYOUT_REGION_TYPE_SB_BASE,
			FTL_SB_VERSION_CURRENT);
}

/*
 * [한국어]
 * ftl_layout_dump - 모든 region을 NV cache/base 두 그룹으로 나눠 NOTICELOG 출력.
 *
 * 진단/지원용 — 디바이스 시작 시 또는 RPC로 호출되어 현재 layout을 가시화.
 */
void
ftl_layout_dump(struct spdk_ftl_dev *dev)
{
	struct ftl_layout_region *reg;
	enum ftl_layout_region_type i;

	FTL_NOTICELOG(dev, "NV cache layout:\n");
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; ++i) {
		reg = ftl_layout_region_get(dev, i);
		if (reg && reg->bdev_desc == dev->nv_cache.bdev_desc) {
			/* [한국어] NV cache에 위치한 region만 dump. */
			dump_region(dev, reg);
		}
	}
	FTL_NOTICELOG(dev, "Base device layout:\n");
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; ++i) {
		reg = ftl_layout_region_get(dev, i);
		if (reg && reg->bdev_desc == dev->base_bdev_desc) {
			/* [한국어] base에 위치한 region만 dump. */
			dump_region(dev, reg);
		}
	}
}

/*
 * [한국어]
 * ftl_layout_base_md_blocks - base 디바이스에 필요한 메타데이터 총 블록 수 추정.
 *
 * @dev:    디바이스.
 * @return: valid map + SB region이 차지할 블록 수.
 *
 * data 영역 크기 결정 시 메타데이터에 필요한 공간을 빼주기 위해 사용.
 */
uint64_t
ftl_layout_base_md_blocks(struct spdk_ftl_dev *dev)
{
	const struct spdk_bdev *bdev;
	uint64_t md_blocks = 0, total_blocks = 0;

	/* [한국어] base + nvc 전체 블록 수 합산. */
	bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
	total_blocks += spdk_bdev_get_num_blocks(bdev);

	bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);
	total_blocks += spdk_bdev_get_num_blocks(bdev);

	/* Count space needed for validity map */
	/* [한국어] valid map 블록 수: 전체 블록 / 8(비트) → 바이트, 그 다음 region 정렬 적용. */
	md_blocks += ftl_md_region_blocks(dev, spdk_divide_round_up(total_blocks, 8));

	/* Count space needed for superblock */
	/* [한국어] SB region 블록 수도 더함. */
	md_blocks += superblock_region_blocks(dev);
	return md_blocks;
}

/*
 * [한국어]
 * struct layout_blob_entry - SB blob 영역에 직렬화할 region 메타데이터 entry.
 *
 * 한 region의 핵심 정보(type, entry_size, num_entries)를 packed 구조로 디스크에 기록.
 * offset/blocks는 트래커에서 별도로 관리되므로 여기엔 포함되지 않음.
 */
struct layout_blob_entry {
	uint32_t type;
	/* [한국어] region type enum 값 — 4바이트로 직렬화. */
	uint64_t entry_size;
	/* [한국어] entry 한 개의 바이트 크기. */
	uint64_t num_entries;
	/* [한국어] entry 개수. */
} __attribute__((packed));
/* [한국어] packed: 컴파일러가 자연 정렬을 위해 padding을 넣지 않게 함 — 디스크 형식 안정성 보장. */

/*
 * [한국어]
 * ftl_layout_blob_store - 모든 region 정보를 blob 버퍼에 직렬화.
 *
 * @dev:         디바이스.
 * @blob_buf:    출력 버퍼.
 * @blob_buf_sz: 버퍼 크기.
 * @return:      직렬화한 바이트 수(0이면 실패: 버퍼 부족).
 *
 * 모든 region type을 순회하며 layout_blob_entry로 직렬화. SB blob 영역에 저장되어 다음 부팅 시
 * LOAD_CURRENT 경로에서 그대로 복원된다.
 */
size_t
ftl_layout_blob_store(struct spdk_ftl_dev *dev, void *blob_buf, size_t blob_buf_sz)
{
	struct layout_blob_entry *blob_entry = blob_buf;
	struct ftl_layout_region *reg;
	enum ftl_layout_region_type reg_type;
	size_t blob_sz = 0;

	for (reg_type = 0; reg_type < FTL_LAYOUT_REGION_TYPE_MAX; reg_type++) {
		if (blob_sz + sizeof(*blob_entry) > blob_buf_sz) {
			/* [한국어] 버퍼 부족 — 실패로 0 반환. */
			return 0;
		}

		/* [한국어] region 정보를 entry에 채움. */
		reg = &dev->layout.region[reg_type];
		blob_entry->type = reg_type;
		blob_entry->entry_size = reg->entry_size;
		blob_entry->num_entries = reg->num_entries;

		/* [한국어] 다음 entry로 진행 + 누적 크기 갱신. */
		blob_entry++;
		blob_sz += sizeof(*blob_entry);
	}

	return blob_sz;
}

/*
 * [한국어]
 * ftl_layout_blob_load - blob 버퍼에서 region 정보를 역직렬화하여 layout->region에 복원.
 *
 * @dev:      디바이스.
 * @blob_buf: 입력 blob 버퍼.
 * @blob_sz:  blob 바이트 수.
 * @return:   0 성공, -1 형식 오류.
 *
 * 형식 검증: blob_sz가 entry 크기의 정수배여야 하며, type 값이 유효 범위 내여야 한다.
 */
int
ftl_layout_blob_load(struct spdk_ftl_dev *dev, void *blob_buf, size_t blob_sz)
{
	struct layout_blob_entry *blob_entry = blob_buf;
	/* [한국어] entry 개수 = blob_sz / sizeof entry. */
	size_t blob_entry_num = blob_sz / sizeof(*blob_entry);
	struct layout_blob_entry *blob_entry_end = blob_entry + blob_entry_num;
	struct ftl_layout_region *reg;

	if (blob_sz % sizeof(*blob_entry) != 0) {
		/* Invalid blob size */
		/* [한국어] 정수배가 아니면 손상된 blob — 거부. */
		return -1;
	}

	for (; blob_entry < blob_entry_end; blob_entry++) {
		/* Verify the type */
		if (blob_entry->type >= FTL_LAYOUT_REGION_TYPE_MAX) {
			/* [한국어] type 값이 enum 범위 밖 — 손상/포맷 mismatch. */
			return -1;
		}

		/* Load the entry */
		/* [한국어] layout->region[type]에 entry_size/num_entries 복원. */
		reg = &dev->layout.region[blob_entry->type];
		reg->entry_size = blob_entry->entry_size;
		reg->num_entries = blob_entry->num_entries;
	}

	return 0;
}

/*
 * [한국어]
 * ftl_layout_upgrade_add_region_placeholder - 업그레이드 시 새 region을 placeholder로 등록.
 *
 * @dev/@layout_tracker/@reg_type: 디바이스, 트래커, 추가할 region 타입.
 *
 * 트래커에 이미 등록된 region이면 아무것도 안 함. 미등록이면 type/version=0/offset=UINT64_MAX/blocks=0
 * 으로 placeholder를 등록 — 후속 업그레이드 단계에서 적절한 위치에 채워질 빈 슬롯.
 */
void
ftl_layout_upgrade_add_region_placeholder(struct spdk_ftl_dev *dev,
		struct ftl_layout_tracker_bdev *layout_tracker, enum ftl_layout_region_type reg_type)
{
	const struct ftl_layout_tracker_bdev_region_props *reg_search_ctx = NULL;

	/* [한국어] 이미 등록된 region 검색. */
	ftl_layout_tracker_bdev_find_next_region(layout_tracker, reg_type, &reg_search_ctx);
	if (reg_search_ctx) {
		/* [한국어] 이미 존재 — placeholder 추가 불필요. */
		return;
	}

	/* [한국어] placeholder 등록 — 후속 업그레이드 단계에서 적절한 offset/blocks로 채워짐. */
	dev->layout.region[reg_type].type = reg_type;
	dev->layout.region[reg_type].current.version = 0;
	/* [한국어] offset = UINT64_MAX(미할당 sentinel), blocks = 0(빈 region). */
	dev->layout.region[reg_type].current.offset = UINT64_MAX;
	dev->layout.region[reg_type].current.blocks = 0;
}
