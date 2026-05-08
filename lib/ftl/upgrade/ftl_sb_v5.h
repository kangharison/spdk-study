/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Solidigm.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 슈퍼블록 v5 호환성 API 헤더 (ftl_sb_v5.h)
 *
 * === 파일의 역할 ===
 * 슈퍼블록 v5 포맷 전용 API 선언. v5는 v3의 chained list MD 레이아웃을 폐지하고 대신
 * "blob 영역(가변 길이 직렬화 영역)"으로 통합한 형태이다. 슈퍼블록의 끝부분에 char blob_area[0]
 * 가변 배열이 있고, blob_area_end 필드(df 오프셋)가 그 끝을 가리킨다. 이 헤더는 blob 영역의
 * 비어있는지 검사, 무결성 검증, store/load, 영역 업그레이드/적용/덤프 함수들을 노출한다.
 * 현재 FTL_SB_VERSION_CURRENT == 5 이므로 일반적인 마운트 경로도 사실상 이 v5 함수들을 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 슈퍼블록 lifecyle의 "현재 버전" 동작 + "v3 → v5 업그레이드 결과" 작성 둘 다 담당.
 * 호출 체인: ftl_sb.c 또는 ftl_layout_upgrade.c → 이 헤더의 v5 전용 함수 → blob 영역 직렬화/역직렬화.
 * 실행 컨텍스트: SPDK reactor 스레드 (디바이스 마운트, 정지, 업그레이드 단계).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/uuid.h, ftl_sb_common.h(blob_hdr 정의), ftl_sb_current.h,
 *   upgrade/ftl_sb_prev.h(v5 구조체 정의).
 * 의존되는 모듈: ftl_sb_v5.c(구현), ftl_sb.c(현재 버전 디스패처), ftl_layout_upgrade.c.
 * 데이터 흐름: 인메모리 dev->layout MD region들 → blob 직렬화 → 슈퍼블록 buf → bdev write.
 *
 * === 주요 함수/구조체 요약 ===
 *   - ftl_superblock_v5_is_blob_area_empty: blob_area_end가 INVALID인지로 비었는지 판정.
 *   - ftl_superblock_v5_validate_blob_area: blob 헤더와 길이가 정합한지 검증.
 *   - ftl_superblock_v5_store_blob_area: 인메모리 layout을 blob으로 직렬화 후 영속화.
 *   - ftl_superblock_v5_load_blob_area: 영속 blob에서 layout 정보를 인메모리로 복원.
 *   - ftl_superblock_v5_md_layout_upgrade_region: 한 MD 영역의 버전을 갱신.
 *   - ftl_superblock_v5_md_layout_apply: 갱신된 layout을 dev->layout에 반영.
 *   - ftl_superblock_v5_md_layout_dump: v5 MD 레이아웃을 로그로 출력(디버그).
 */

#ifndef FTL_SB_V5_H
#define FTL_SB_V5_H
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "spdk/uuid.h"
/* [한국어] 슈퍼블록 안의 spdk_uuid 멤버 때문에 필요. */
#include "ftl_sb_common.h"
/* [한국어] ftl_superblock_v5_md_blob_hdr 정의 — blob 영역 안의 16바이트 헤더. */
#include "ftl_sb_current.h"
/* [한국어] 현재 struct ftl_superblock 정의(v5와 사실상 동일 레이아웃). */
#include "upgrade/ftl_sb_prev.h"
/* [한국어] struct ftl_superblock_v5 정의 — 이전 버전 호환성 union용. */

struct spdk_ftl_dev;
/* [한국어] forward declaration — ftl_core.h가 정의. */
struct ftl_layout_region;
/* [한국어] forward declaration — ftl_layout.h가 정의. */
union ftl_superblock_ver;
/* [한국어] forward declaration — upgrade/ftl_sb_upgrade.h가 정의. */

/*
 * [한국어]
 * ftl_superblock_v5_is_blob_area_empty - v5 슈퍼블록의 blob 영역이 비어있는지 동기 판정
 *
 * @param sb_ver: 디스크에서 막 읽은 raw 슈퍼블록 union.
 * @return: true = blob_area_end가 INVALID(첫 부팅 또는 직전 마이그레이션 직후), false = blob에 데이터 존재.
 *
 * 실행 컨텍스트: 디바이스 시작 mngt 단계, 단일 스레드 동기 호출.
 */
bool ftl_superblock_v5_is_blob_area_empty(union ftl_superblock_ver *sb_ver);

/*
 * [한국어]
 * ftl_superblock_v5_validate_blob_area - blob 영역 안의 헤더(blob_sz, df_id 등)가 정합한지 동기 검증
 *
 * @param dev: FTL 디바이스 — dev->sb로 슈퍼블록 접근.
 * @return: true = 정합, false = 손상 — 호출자는 마운트 실패로 판단해 복구 또는 abort.
 */
bool ftl_superblock_v5_validate_blob_area(struct spdk_ftl_dev *dev);

/*
 * [한국어]
 * ftl_superblock_v5_store_blob_area - 인메모리 MD layout을 blob 형태로 직렬화 후 슈퍼블록에 영속화
 *
 * @param dev: FTL 디바이스.
 * @return: 0 = 성공, 음수 = 에러(공간 부족, bdev write 실패 등).
 *
 * 동기/배경: 디바이스 정지/업그레이드/주요 layout 변경 시 호출되어 디스크와 인메모리 상태를 동기화.
 * 실행 컨텍스트: SPDK reactor 스레드 — 내부적으로 bdev write 비동기 호출 가능.
 */
int ftl_superblock_v5_store_blob_area(struct spdk_ftl_dev *dev);

/*
 * [한국어]
 * ftl_superblock_v5_load_blob_area - 영속 blob에서 MD layout 정보를 인메모리 dev->layout에 복원
 *
 * @param dev: FTL 디바이스.
 * @return: 0 = 성공, 음수 = 에러.
 *
 * 호출 체인: 디바이스 마운트 mngt 단계 → [이 함수] → ftl_df_get_obj_ptr로 blob 안의 객체 환원.
 */
int ftl_superblock_v5_load_blob_area(struct spdk_ftl_dev *dev);

/*
 * [한국어]
 * ftl_superblock_v5_md_layout_upgrade_region - 한 MD 영역을 new_version으로 마이그레이션
 *
 * @param dev: FTL 디바이스.
 * @param reg: 업그레이드 대상 layout 영역.
 * @param new_version: 적용할 새 버전 번호.
 * @return: 0 = 성공, 음수 = 에러.
 */
int ftl_superblock_v5_md_layout_upgrade_region(struct spdk_ftl_dev *dev,
		struct ftl_layout_region *reg, uint32_t new_version);

/*
 * [한국어]
 * ftl_superblock_v5_md_layout_apply - blob에서 로드/업그레이드된 layout을 dev->layout 인메모리 구조에 반영
 *
 * @param dev: FTL 디바이스.
 * @return: 0 = 성공, 음수 = 에러(영역 겹침, 크기 모순 등).
 */
int ftl_superblock_v5_md_layout_apply(struct spdk_ftl_dev *dev);

/*
 * [한국어]
 * ftl_superblock_v5_md_layout_dump - v5 MD 레이아웃을 로그로 출력 (디버그)
 *
 * @param dev: FTL 디바이스.
 * 반환값 없음.
 */
void ftl_superblock_v5_md_layout_dump(struct spdk_ftl_dev *dev);

#endif /* FTL_SB_V5_H */
/* [한국어] 헤더 가드 종료. */
