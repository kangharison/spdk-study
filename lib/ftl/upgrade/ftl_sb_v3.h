/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 슈퍼블록 v3 호환성 API 헤더 (ftl_sb_v3.h)
 *
 * === 파일의 역할 ===
 * 옛 슈퍼블록 v3 포맷을 읽고 검증하기 위한 함수들의 선언. v3는 MD 레이아웃을
 * "head + chained list"(struct ftl_superblock_v3_md_region 노드들의 연결) 형태로 저장하던
 * 중간 버전이다. 현재 코드는 더 이상 v3 포맷으로 슈퍼블록을 새로 만들지 않지만, 기존 v3
 * 디바이스를 마운트했을 때 그 내용을 읽고 v5 포맷으로 업그레이드 할 수 있어야 하므로
 * 이 인터페이스가 살아남아 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 슈퍼블록 업그레이드 파이프라인의 v3 단계 전용 헬퍼.
 * 호출 체인: ftl_layout_upgrade.c / ftl_sb.c → 이 헤더의 v3 전용 함수 → 디스크에서 읽힌
 *   union ftl_superblock_ver의 v3 멤버 접근 → MD 레이아웃 chained list 순회.
 * 실행 컨텍스트: 디바이스 시작 mngt 단계, SPDK reactor 스레드.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/uuid.h, ftl_sb_common.h(ftl_superblock_v3_md_region 정의),
 *   ftl_sb_current.h(현재 ftl_superblock 비교용).
 * 의존되는 모듈: ftl_sb_v3.c(구현), ftl_layout_upgrade.c(v3 → v5 마이그레이션 디스패처),
 *   ftl_sb.c.
 * 데이터 흐름: 디스크 v3 슈퍼블록 → union ftl_superblock_ver* → 이 함수들로 검증/순회 →
 *   v5 형태로 변환되어 영속화.
 *
 * === 주요 함수/구조체 요약 ===
 *   - ftl_superblock_v3_check_magic: v3 magic 일치 여부 동기 검사.
 *   - ftl_superblock_v3_md_layout_is_empty: MD 레이아웃 chained list가 비었는지 판정.
 *   - ftl_superblock_v3_md_region_overflow: MD region이 디바이스 용량을 초과하는지 검사.
 *   - ftl_superblock_v3_md_layout_load_all: chained list를 모두 읽어 dev->layout에 매핑.
 *   - ftl_superblock_v3_md_layout_dump: v3 MD 레이아웃을 로그로 출력(디버그).
 */

#ifndef FTL_SB_V3_H
#define FTL_SB_V3_H
/* [한국어] 헤더 가드. */

#include "spdk/uuid.h"
/* [한국어] uuid 멤버를 가진 슈퍼블록 구조체들이 멤버로 들어있어 필요. */
#include "ftl_sb_common.h"
/* [한국어] ftl_superblock_v3_md_region 정의 (v3 MD 레이아웃의 chained list 노드 구조). */
#include "ftl_sb_current.h"
/* [한국어] 현재 ftl_superblock 정의 — v3 → 현재 변환 비교 시 필요. */

struct spdk_ftl_dev;
/* [한국어] forward declaration — ftl_core.h가 정의. */
struct ftl_layout_region;
/* [한국어] forward declaration — ftl_layout.h가 정의. */
union ftl_superblock_ver;
/* [한국어] forward declaration — upgrade/ftl_sb_upgrade.h가 정의 (다중 버전 union). */

/*
 * [한국어]
 * ftl_superblock_v3_check_magic - v3 슈퍼블록의 magic 필드가 v3 magic 값과 일치하는지 동기 검사
 *
 * @param sb_ver: union ftl_superblock_ver* — 디스크에서 막 읽은 raw 슈퍼블록을 가리킴.
 * @return: true = v3 magic 일치(이 슈퍼블록은 v3로 해석 가능), false = 다른 버전이거나 손상.
 *
 * 실행 컨텍스트: 디바이스 시작 단계의 슈퍼블록 디스패처(SPDK reactor, 단일 스레드, 동기).
 * caller: ftl_layout_upgrade.c의 슈퍼블록 버전 자동 감지 루프.
 */
bool ftl_superblock_v3_check_magic(union ftl_superblock_ver *sb_ver);

/*
 * [한국어]
 * ftl_superblock_v3_md_layout_is_empty - v3 MD 레이아웃 chained list가 비어있는지 동기 판정
 *
 * @param sb_ver: 디스크에서 읽은 v3 형태 슈퍼블록.
 * @return: true = MD 레이아웃 head가 비었음(첫 부팅 직후 등), false = 노드가 하나 이상 있음.
 */
bool ftl_superblock_v3_md_layout_is_empty(union ftl_superblock_ver *sb_ver);

/*
 * [한국어]
 * ftl_superblock_v3_md_region_overflow - 한 MD region이 디바이스의 가용 영역을 초과하는지 검사
 *
 * @param dev: FTL 디바이스 — 디바이스 용량 정보를 위해 사용.
 * @param sb_reg: chained list의 한 노드(struct ftl_superblock_v3_md_region) 포인터.
 * @return: true = blk_offs + blk_sz가 디바이스 영역을 벗어남(손상/이상), false = 정상.
 *
 * 동기/배경: 옛 디바이스에서 손상된 MD 레이아웃을 마운트하다 메모리/디스크 침범하지 않도록 사전 체크.
 */
bool ftl_superblock_v3_md_region_overflow(struct spdk_ftl_dev *dev,
		struct ftl_superblock_v3_md_region *sb_reg);

/*
 * [한국어]
 * ftl_superblock_v3_md_layout_load_all - v3 chained list 전체를 따라가며 MD 레이아웃을 dev->layout에 적재
 *
 * @param dev: FTL 디바이스 — 결과는 dev->layout 영역 배열에 저장됨.
 * @return: 0 = 성공, 음수 = 에러(체인 손상, 영역 겹침 등).
 *
 * 호출 체인: 디바이스 시작 mngt 단계 → [이 함수] → ftl_df_get_obj_ptr로 next 포인터 환원하며 순회.
 */
int ftl_superblock_v3_md_layout_load_all(struct spdk_ftl_dev *dev);

/*
 * [한국어]
 * ftl_superblock_v3_md_layout_dump - v3 MD 레이아웃 chained list를 로그로 출력 (디버그)
 *
 * @param dev: FTL 디바이스.
 * 반환값 없음.
 */
void ftl_superblock_v3_md_layout_dump(struct spdk_ftl_dev *dev);

#endif /* FTL_SB_V3_H */
/* [한국어] 헤더 가드 종료. */
