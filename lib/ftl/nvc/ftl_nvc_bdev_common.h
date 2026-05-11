/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

/*
 * [한국어 설명] NVC bdev 공통 helper 헤더 (ftl_nvc_bdev_common.h)
 *
 * === 파일의 역할 ===
 * VSS와 non-VSS 두 NVC 백엔드 구현이 공유하는 layout/region helper들의 공개 인터페이스
 * 를 정의한다. 두 모델 모두 SPDK bdev를 backend로 쓰면서 chunk 활성성 검사와
 * 메타데이터 region 생성/오픈 로직은 동일하므로 공통 함수로 빼서 코드 중복을 제거한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * ftl_nvc_bdev_vss / non_vss 디스크립터의 ops.is_chunk_active와 md_layout_ops에
 * 이 헤더의 함수들이 직접 함수 포인터로 등록된다.
 * 호출 체인:
 *   FTL 코어 → nvc_type->ops.md_layout_ops.region_create
 *           → ftl_nvc_bdev_common_region_create()
 *           → ftl_layout_tracker_bdev_add_region()
 *
 * === 타 모듈과의 연결 ===
 * - lib/ftl/nvc/ftl_nvc_bdev_common.c : 실제 구현
 * - lib/ftl/nvc/ftl_nvc_bdev_vss.c    : ops 등록 시 이 함수 사용
 * - lib/ftl/nvc/ftl_nvc_bdev_non_vss.c: ops 등록 시 이 함수 사용
 * - lib/ftl/utils/ftl_layout_tracker_bdev.c : 영역 배치 추적기 (백엔드)
 *
 * === 주요 함수/구조체 요약 ===
 * - ftl_nvc_bdev_common_is_chunk_active : chunk가 layout tracker 상 활성인지 검사
 * - ftl_nvc_bdev_common_region_create   : 새 메타데이터 region을 superblock에 추가
 * - ftl_nvc_bdev_common_region_open     : 등록된 region을 ftl_layout_region 객체로 채움
 */

#ifndef FTL_NVC_BDEV_COMMON_H
#define FTL_NVC_BDEV_COMMON_H

#include "ftl_core.h"   /* [한국어] struct spdk_ftl_dev 정의 */
#include "ftl_layout.h" /* [한국어] ftl_layout_region 등 layout 타입 */

/* [한국어] chunk_offset이 nvc_layout_tracker 상 FREE 영역에 속하는지 검사.
 * NVC가 chunk 단위로 데이터 저장에 사용 가능한지를 판단하는 빠른 헬퍼이다. */
bool ftl_nvc_bdev_common_is_chunk_active(struct spdk_ftl_dev *dev, uint64_t chunk_offset);

/* [한국어] 새 메타데이터 region을 superblock(layout tracker)에 등록.
 * 이미 동일 (type, version) 영역이 있으면 실패하며, 이는 upgrade 흐름에서
 * 두 번째 호출이 의도적으로 실패하는 시나리오에 활용된다. */
int ftl_nvc_bdev_common_region_create(struct spdk_ftl_dev *dev,
				      enum ftl_layout_region_type reg_type,
				      uint32_t reg_version, size_t reg_blks);

/* [한국어] superblock에 등록된 region 정보를 읽어와 ftl_layout_region 객체에 채움.
 * region이 NULL이면 존재 여부만 확인하는 dry-run으로 동작한다. */
int ftl_nvc_bdev_common_region_open(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
				    uint32_t reg_version, size_t entry_size, size_t entry_count,
				    struct ftl_layout_region *region);

#endif /* FTL_NVC_BDEV_COMMON_H */
