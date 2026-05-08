/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 슈퍼블록 현재 버전 공개 API 헤더 (ftl_sb.h)
 *
 * === 파일의 역할 ===
 * 디바이스 마운트 후 정상 동작 단계에서 사용되는 "현재 버전" 슈퍼블록 조작 API의 진입점이다.
 * 매직 검증, blob 영역 비었는지 확인/검증, blob 영역 store/load, MD 레이아웃 영역 단위 업그레이드,
 * 전체 레이아웃 적용/덤프 등의 함수를 선언한다. 실제 구현은 ftl_sb.c에 있고, 이 헤더는 외부에서
 * 호출 가능한 인터페이스만 노출한다. 같은 기능의 v3/v5 버전별 구현(ftl_sb_v3.h, ftl_sb_v5.h)이
 * 별도로 있고, 이 파일은 "현재 활성 슈퍼블록 버전" 호출자 인터페이스에 해당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 슈퍼블록 라이프사이클의 외부 인터페이스 레이어.
 * 호출 체인: ftl mngt 단계(시작/정지/업그레이드) → 이 헤더의 함수 → ftl_sb.c 구현 →
 *   필요 시 ftl_sb_current 또는 ftl_sb_v5 내부 구현 위임 → bdev_read/write_blocks 비동기 호출.
 * 실행 컨텍스트: SPDK reactor 스레드. 일부 함수(load/store/upgrade)는 비동기로 mngt 다음 단계를
 *   콜백으로 트리거한다. 매직 검사/블롭 비었는지 검사 같은 정수 함수는 동기.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/uuid.h, ftl_sb_common.h(공통 헤더), ftl_sb_current.h(struct ftl_superblock 정의).
 * 의존되는 모듈: ftl_mngt_startup.c, ftl_mngt_upgrade.c, ftl_layout_upgrade.c, ftl_sb.c(구현).
 * 데이터 흐름: 디스크 슈퍼블록 ↔ 인메모리 ftl_superblock buf ↔ blob 영역(가변 길이 메타데이터 직렬화)
 *   ↔ FTL 레이아웃 영역(ftl_layout_region 배열).
 * 공유 상태: 단일 슈퍼블록 인스턴스(dev->sb)가 모든 함수의 암묵적 인자.
 *
 * === 주요 함수/구조체 요약 ===
 *   - ftl_superblock_check_magic(sb): magic 필드가 FTL_SUPERBLOCK_MAGIC인지 동기 검사.
 *   - ftl_superblock_is_blob_area_empty(sb): blob_area_end로부터 blob이 비었는지 동기 판정.
 *   - ftl_superblock_validate_blob_area(dev): blob 헤더 무결성/크기 검증.
 *   - ftl_superblock_store_blob_area(dev): 인메모리 MD 레이아웃을 blob 영역으로 직렬화 후 영속화.
 *   - ftl_superblock_load_blob_area(dev): 영속 blob에서 MD 레이아웃 정보를 로드.
 *   - ftl_superblock_md_layout_upgrade_region: 특정 영역의 버전을 new_version으로 마이그레이션.
 *   - ftl_superblock_md_layout_apply(dev): 갱신된 MD 레이아웃을 dev->layout에 반영.
 *   - ftl_superblock_md_layout_dump(dev): 현재 슈퍼블록 MD 레이아웃을 로그로 덤프(디버그).
 */

#ifndef FTL_SB_H
#define FTL_SB_H
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "spdk/uuid.h"
/* [한국어] struct ftl_superblock 안에 spdk_uuid 멤버가 있어 필요. */
#include "ftl_sb_common.h"
/* [한국어] ftl_superblock_header, ftl_superblock_gc_info, ftl_superblock_v5_md_blob_hdr 정의. */
#include "ftl_sb_current.h"
/* [한국어] 현재 슈퍼블록 struct ftl_superblock 정의 + FTL_SB_VERSION_CURRENT. */

struct spdk_ftl_dev;
/* [한국어] forward declaration — 순환 의존 방지. ftl_core.h가 정의. */
struct ftl_layout_region;
/* [한국어] forward declaration — ftl_layout.h가 정의. 슈퍼블록 MD layout 영역 단위 디스크립터. */

/*
 * [한국어]
 * ftl_superblock_check_magic - 슈퍼블록 헤더의 magic 필드가 현재 버전 magic과 일치하는지 동기 검사
 *
 * @param sb: 검사 대상 인메모리 슈퍼블록 포인터(NULL 아님).
 * @return: true = magic 일치(현재 버전), false = 불일치(이전 버전 또는 손상). 호출자는 false면 업그레이드 디스패처로.
 *
 * 실행 컨텍스트: 디바이스 시작 단계, SPDK reactor 스레드, 동기 호출.
 */
bool ftl_superblock_check_magic(struct ftl_superblock *sb);

/*
 * [한국어]
 * ftl_superblock_is_blob_area_empty - blob_area_end 필드로 blob 영역이 미사용인지 동기 판정
 *
 * @param sb: 슈퍼블록 포인터.
 * @return: true = blob 비어있음(첫 마운트 또는 마이그레이션 직후), false = blob에 데이터가 있음.
 *
 * 동기/배경: blob 영역은 가변 길이 MD 레이아웃 정보 저장소. 비었으면 새로 빌드, 있으면 load.
 */
bool ftl_superblock_is_blob_area_empty(struct ftl_superblock *sb);

/*
 * [한국어]
 * ftl_superblock_validate_blob_area - blob 영역 안의 헤더와 길이가 정합한지 동기 검증
 *
 * @param dev: FTL 디바이스 — dev->sb로 슈퍼블록 접근.
 * @return: true = 유효, false = 손상/불일치 — 호출자는 마운트 실패 처리.
 */
bool ftl_superblock_validate_blob_area(struct spdk_ftl_dev *dev);

/*
 * [한국어]
 * ftl_superblock_store_blob_area - 인메모리 MD 레이아웃을 blob 형태로 직렬화 후 영속화
 *
 * @param dev: FTL 디바이스.
 * @return: 0 = 성공(또는 비동기 시작 성공), 음수 = 에러.
 *
 * 동기/배경: layout 변경 후 슈퍼블록을 디스크에 반영해 다음 부팅에서도 같은 레이아웃을 보게 함.
 * 실행 컨텍스트: SPDK reactor — 내부적으로 bdev write 비동기 호출 가능.
 */
int ftl_superblock_store_blob_area(struct spdk_ftl_dev *dev);

/*
 * [한국어]
 * ftl_superblock_load_blob_area - 영속 blob에서 MD 레이아웃 메타데이터를 인메모리로 복원
 *
 * @param dev: FTL 디바이스.
 * @return: 0 = 성공, 음수 = 에러.
 *
 * 호출 체인: 디바이스 시작 mngt 단계 → [이 함수] → 인메모리 dev->layout에 영역 기록.
 */
int ftl_superblock_load_blob_area(struct spdk_ftl_dev *dev);

/*
 * [한국어]
 * ftl_superblock_md_layout_upgrade_region - 한 MD 영역을 new_version으로 마이그레이션
 *
 * @param dev: FTL 디바이스.
 * @param reg: 업그레이드 대상 ftl_layout_region.
 * @param new_version: 적용할 새 버전 번호.
 * @return: 0 = 성공, 음수 = 에러.
 *
 * 동기/배경: 펌웨어 업그레이드처럼 MD 영역 포맷이 바뀔 때, 이 함수로 영역별 버전을 갱신.
 */
int ftl_superblock_md_layout_upgrade_region(struct spdk_ftl_dev *dev,
		struct ftl_layout_region *reg, uint32_t new_version);

/*
 * [한국어]
 * ftl_superblock_md_layout_apply - 슈퍼블록의 현재 MD 레이아웃을 dev->layout 인메모리 구조로 적용
 *
 * @param dev: FTL 디바이스.
 * @return: 0 = 성공, 음수 = 에러(레이아웃 모순 등).
 */
int ftl_superblock_md_layout_apply(struct spdk_ftl_dev *dev);

/*
 * [한국어]
 * ftl_superblock_md_layout_dump - 현재 슈퍼블록 MD 레이아웃을 로그로 출력 (디버그)
 *
 * @param dev: FTL 디바이스.
 * 반환값 없음 — 결과는 SPDK 로그로 흐른다.
 */
void ftl_superblock_md_layout_dump(struct spdk_ftl_dev *dev);

#endif /* FTL_SB_H */
/* [한국어] 헤더 가드 종료. */
