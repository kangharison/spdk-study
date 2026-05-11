/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

/*
 * [한국어 설명] NVC bdev 공통 helper 구현 (ftl_nvc_bdev_common.c)
 *
 * === 파일의 역할 ===
 * VSS / non-VSS NVC 디바이스 모델이 공유하는 layout 관련 공통 helper를 제공한다.
 * - chunk가 NVC layout tracker 상 활성(데이터 저장 가능)인지 판정
 * - 메타데이터 region 생성/오픈 (md_layout_ops.region_create/region_open 구현)
 * 두 모델 모두 NVC backend가 SPDK bdev이고, region 배치는 동일한 layout tracker로
 * 추적되므로 이 공통 구현을 함수 포인터로 등록해 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   FTL 코어/upgrade 코드
 *     → nvc_type->ops.md_layout_ops.region_create/open
 *     → [이 파일의 함수]
 *     → ftl_layout_tracker_bdev_*() (실제 영역 배치 자료구조 조작)
 * is_chunk_active는 NVC가 chunk 단위로 데이터 영역을 스캔할 때 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/ftl/nvc/ftl_nvc_bdev_vss.c / non_vss.c : ops 등록 클라이언트
 * - lib/ftl/utils/ftl_layout_tracker_bdev.c    : 실제 layout 자료구조
 * - lib/ftl/ftl_layout.c                       : ftl_md_region_blocks/align 헬퍼
 *
 * === 주요 함수/구조체 요약 ===
 * - ftl_nvc_bdev_common_is_chunk_active : chunk가 free 영역에 속하면 true
 * - md_region_setup                     : ftl_layout_region 공통 필드 채움 (static)
 * - ftl_nvc_bdev_common_region_create   : 새 region을 superblock에 추가 (정렬 후)
 * - ftl_nvc_bdev_common_region_open     : 등록된 region을 layout 객체로 가져오기
 */

#include "ftl_nvc_bdev_common.h"               /* [한국어] 본 파일이 구현하는 공개 API */
#include "ftl_nvc_dev.h"                       /* [한국어] NVC ops 정의 (md_layout_ops 포함) */
#include "ftl_core.h"                          /* [한국어] spdk_ftl_dev 본체 */
#include "ftl_layout.h"                        /* [한국어] ftl_layout_region 등 layout 타입 */
#include "utils/ftl_layout_tracker_bdev.h"     /* [한국어] bdev 영역 배치 자료구조 */
#include "mngt/ftl_mngt.h"                     /* [한국어] 관리 절차 헬퍼 (현재 직접 사용은 없음) */

/*
 * [한국어]
 * ftl_nvc_bdev_common_is_chunk_active - chunk가 활성(데이터 저장 가능) 영역인지 검사
 *
 * @dev: FTL 디바이스
 * @chunk_offset: 검사할 chunk의 NVC 내 시작 블록 오프셋
 * @return: free 영역으로 잡혀 있으면 true, 아니면 false
 *
 * 구현 트릭: ftl_layout_tracker_bdev_insert_region을 FTL_LAYOUT_REGION_TYPE_INVALID,
 * version 0으로 호출하면 "이 범위가 free 영역에 깨끗이 들어가는지"를 dry-run으로
 * 검사할 수 있다. 들어가면 free region 디스크립터를 반환하고, 다른 region과 겹치면
 * NULL을 반환한다. 실제 영역을 추가하지는 않는다 (insert_region의 INVALID 모드 의미).
 *
 * 호출 체인:
 *   nvc_type->ops.is_chunk_active → [이 함수]
 */
bool
ftl_nvc_bdev_common_is_chunk_active(struct spdk_ftl_dev *dev, uint64_t chunk_offset)
{
	/* [한국어] dry-run insert: chunk 범위가 free 공간에 들어가는지 시뮬레이션.
	 * INVALID/version 0 조합은 layout tracker 측에서 "검사 전용" 호출로 인식한다. */
	const struct ftl_layout_tracker_bdev_region_props *reg_free = ftl_layout_tracker_bdev_insert_region(
				dev->nvc_layout_tracker, FTL_LAYOUT_REGION_TYPE_INVALID, 0, chunk_offset,
				dev->layout.nvc.chunk_data_blocks);

	/* [한국어] 결과가 있다면 반드시 FREE 영역이어야 한다 — tracker 불변식 검증. */
	assert(!reg_free || reg_free->type == FTL_LAYOUT_REGION_TYPE_FREE);
	return reg_free != NULL;                          /* [한국어] free 영역 포함 시 true */
}

/*
 * [한국어]
 * md_region_setup - ftl_layout_region 공통 필드 채움 (static helper)
 *
 * @dev:      FTL 디바이스
 * @reg_type: 채워 넣을 region 종류 (P2L log 등)
 * @region:   결과를 받을 region 객체 (호출자가 소유)
 *
 * region_open 성공 시 호출되어 type/이름/bdev/ioch/VSS 블록 사이즈 같은
 * "어디에 저장될지" 정보를 한 번에 세팅한다. 버전·오프셋·블록 수 같은 가변 정보는
 * 호출자가 별도로 채운다.
 */
static void
md_region_setup(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
		struct ftl_layout_region *region)
{
	assert(region);                                   /* [한국어] NULL 방어 — 호출자 계약 */
	region->type = reg_type;                          /* [한국어] region 종류 식별자 */
	region->mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID; /* [한국어] 기본은 미러 미사용 */
	region->name = ftl_md_region_name(reg_type);      /* [한국어] 사람이 읽을 이름 (로그용) */

	region->bdev_desc = dev->nv_cache.bdev_desc;      /* [한국어] backend bdev 디스크립터 */
	region->ioch = dev->nv_cache.cache_ioch;          /* [한국어] 이 region에 사용할 IO channel */
	region->vss_blksz = dev->nv_cache.md_size;        /* [한국어] VSS 영역 블록당 메타 크기 */
}

/*
 * [한국어]
 * ftl_nvc_bdev_common_region_create - 새 메타데이터 region을 layout tracker에 등록
 *
 * @dev:         FTL 디바이스
 * @reg_type:    region 종류
 * @reg_version: region 버전 (upgrade 호환성에 중요)
 * @reg_blks:    필요한 블록 수 (정렬 전 값)
 * @return: 0 성공, -1 실패 (이미 등록되었거나 free 공간 부족 등)
 *
 * 호출 시 reg_blks를 메타데이터 정렬 단위로 올림 정렬한 뒤 layout tracker에 영역을
 * 추가한다. upgrade 시 동일 (type, version)을 두 번 추가하면 두 번째 호출이 -1을
 * 돌려주는 것을 활용해 멱등 동작을 보장한다.
 *
 * 호출 체인:
 *   ops.md_layout_ops.region_create → [이 함수] → ftl_layout_tracker_bdev_add_region()
 */
int
ftl_nvc_bdev_common_region_create(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
				  uint32_t reg_version, size_t reg_blks)
{
	const struct ftl_layout_tracker_bdev_region_props *reg_props;  /* [한국어] 결과 영역 속성 */

	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);    /* [한국어] enum 범위 검증 */
	reg_blks = ftl_md_region_align_blocks(dev, reg_blks); /* [한국어] 메타 정렬 경계로 올림 */

	/* [한국어] 실제로 layout tracker에 영역 등록 — 충돌 시 NULL 반환. */
	reg_props = ftl_layout_tracker_bdev_add_region(dev->nvc_layout_tracker, reg_type, reg_version,
			reg_blks, 0);
	if (!reg_props) {
		return -1;                                /* [한국어] 등록 실패 (중복/공간부족) */
	}
	assert(reg_props->type == reg_type);              /* [한국어] tracker 결과 무결성 검증 */
	assert(reg_props->ver == reg_version);
	assert(reg_props->blk_sz == reg_blks);
	/* [한국어] 등록 영역이 NVC 전체 블록 범위 안에 있는지 — 경계 위반 방어. */
	assert(reg_props->blk_offs + reg_blks <= dev->layout.nvc.total_blocks);
	return 0;
}

/*
 * [한국어]
 * ftl_nvc_bdev_common_region_open - 등록된 region을 ftl_layout_region 객체로 채워 반환
 *
 * @dev:         FTL 디바이스
 * @reg_type:    찾을 region 종류
 * @reg_version: 찾을 region 버전 (upgrade 시 옛/새 버전 구분)
 * @entry_size:  엔트리 1개 크기 (바이트, FTL_BLOCK_SIZE 배수)
 * @entry_count: 엔트리 개수
 * @region:      결과를 채울 객체 (NULL이면 존재 여부 검사만 수행)
 * @return: 0 성공, -1 실패 (영역 없음 또는 공간 부족)
 *
 * tracker에 등록된 같은 type의 영역들을 순회하며 reg_version과 일치하는 첫 번째
 * 영역을 선택한다. region이 NULL이면 dry-run으로 동작해 영역 존재만 확인한다
 * (upgrade 흐름에서 사전 검증용).
 *
 * 호출 체인:
 *   ops.md_layout_ops.region_open → [이 함수]
 *     → ftl_layout_tracker_bdev_find_next_region() → md_region_setup()
 */
int
ftl_nvc_bdev_common_region_open(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
				uint32_t reg_version,
				size_t entry_size, size_t entry_count, struct ftl_layout_region *region)
{
	const struct ftl_layout_tracker_bdev_region_props *reg_search_ctx = NULL; /* [한국어] 순회 커서 */
	uint64_t reg_blks = ftl_md_region_blocks(dev, entry_size * entry_count);  /* [한국어] 필요 블록 수 산출 */

	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);    /* [한국어] enum 범위 검증 */

	/* [한국어] 같은 type의 등록 영역들을 순회하며 reg_version 일치 항목 탐색.
	 * 일치 시 break, 끝까지 못 찾으면 reg_search_ctx == NULL 상태로 빠져나감. */
	while (true) {
		ftl_layout_tracker_bdev_find_next_region(dev->nvc_layout_tracker, reg_type, &reg_search_ctx);
		if (!reg_search_ctx || reg_search_ctx->ver == reg_version) {
			break;
		}
	}

	/* [한국어] 영역이 없거나 등록된 크기가 요청보다 작으면 실패. */
	if (!reg_search_ctx || reg_search_ctx->blk_sz < reg_blks) {
		/* Region not found or insufficient space */
		return -1;
	}

	/* [한국어] dry-run 모드 (region == NULL) — 존재 확인만 하고 성공 반환. */
	if (!region) {
		return 0;
	}

	md_region_setup(dev, reg_type, region);           /* [한국어] 공통 필드 세팅 */

	/* [한국어] 가변 필드 채움. entry_size는 블록 단위로 변환 (FTL_BLOCK_SIZE 가정). */
	region->entry_size = entry_size / FTL_BLOCK_SIZE;
	region->num_entries = entry_count;

	/* [한국어] current 슬롯에 현 버전의 위치 정보 복사 — IO 시 이 값을 사용. */
	region->current.version = reg_version;
	region->current.offset = reg_search_ctx->blk_offs;
	region->current.blocks = reg_search_ctx->blk_sz;

	return 0;
}
