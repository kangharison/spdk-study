/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 슈퍼블록 다중 버전 union 정의 헤더 (ftl_sb_upgrade.h)
 *
 * === 파일의 역할 ===
 * 디스크에서 읽은 슈퍼블록을 어느 버전(v2/v3/v5/current)으로 해석해야 할지 결정하는 단계에서
 * 사용하는 type-punning union을 정의한다. NV cache 또는 base bdev의 슈퍼블록 영역에서 4KiB
 * (실제로는 128KiB) 만큼 읽어와서 union ftl_superblock_ver* 로 캐스팅하면, header.version 값을
 * 보고 적절한 멤버(v2/v3/v5/current)로 좁혀 접근할 수 있다. packed 속성이 강제되어 있어
 * 디스크 레이아웃과 메모리 레이아웃이 정확히 일치한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 슈퍼블록 업그레이드 파이프라인의 핵심 매개 타입.
 * 호출 체인: ftl_superblock_upgrade() → 디스크 read → union ftl_superblock_ver* 캐스팅 →
 *   header.version 검사 → 해당 버전의 v*_check_magic() / md_layout_load_all() 호출 →
 *   필요 시 다음 버전으로 변환 → 최신 struct ftl_superblock 로 합류.
 * 실행 컨텍스트: SPDK reactor 스레드(FTL 디바이스 시작 mngt 단계). 동기 비교/캐스팅이라
 *   별도 동기화 없음.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/uuid.h(ftl_superblock 안의 spdk_uuid 멤버), ftl_sb_common.h
 *   (ftl_superblock_header, ftl_superblock_v3_md_region 등), ftl_sb_prev.h
 *   (v2/v3/v5 구조체 정의), ftl_sb_current.h (현재 버전 ftl_superblock 정의).
 * 의존되는 모듈: ftl_sb_v3.c, ftl_sb_v5.c, ftl_layout_upgrade.c, ftl_sb.c.
 * 데이터 흐름: 디스크 raw 바이트 → 이 union 위로 캐스팅 → header.version 분기 → 멤버 접근.
 * 공유 상태: union 자체는 자료구조 정의일 뿐 인스턴스를 보유하지 않음. 인스턴스는 호출자가
 *   ftl_layout 영역에 매핑되거나 buf로 할당.
 *
 * === 주요 함수/구조체 요약 ===
 * 함수는 없다. union의 멤버 구성은 다음과 같다:
 *   - header: 모든 버전이 동일하게 시작하는 공통 16바이트 헤더(magic/crc/version) — 버전 식별 용도.
 *   - v2: 가장 오래된 superblock(레거시). magic 계산 방식에 버그가 있어 별도 macro로 처리됨.
 *   - v3: blob 영역 도입 이전 형태로 md_layout이 head + chained list 형태.
 *   - v5: 현재 형태에 가까운 v5 (Solidigm 포팅). md_layout이 blob 영역으로 통합됨.
 *   - current: 컴파일 시점의 최신 버전(=현재 v5 별칭). 코드가 항상 최신을 가리키게 한다.
 */

#ifndef FTL_SB_UPGRADE_H
#define FTL_SB_UPGRADE_H
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "spdk/uuid.h"
/* [한국어] union 안의 ftl_superblock_v3 등이 spdk_uuid 멤버를 포함하므로 필요. */
#include "ftl_sb_common.h"
/* [한국어] ftl_superblock_header(공통 magic/crc/version)와 ftl_superblock_v3_md_region,
 * ftl_superblock_v5_md_blob_hdr 정의를 가져온다. union의 모든 버전이 header로 시작. */
#include "ftl_sb_prev.h"
/* [한국어] 이전 버전 슈퍼블록 구조체(struct ftl_superblock_v2, v3, v5) 정의.
 * 업그레이드 코드가 옛 버전을 읽기 위해 정확한 레이아웃을 알아야 한다. */
#include "ftl_sb_current.h"
/* [한국어] 현재 버전 struct ftl_superblock 정의 + FTL_SB_VERSION_CURRENT 매크로. */

struct spdk_ftl_dev;
/* [한국어] forward declaration — 헤더 간 순환 참조 회피. ftl_core.h가 정의하는 FTL 메인 디바이스 컨텍스트. */
struct ftl_layout_region;
/* [한국어] forward declaration — ftl_layout.h 정의. 슈퍼블록 안의 메타데이터 영역 위치 정보. */

union ftl_superblock_ver {
	struct ftl_superblock_header header;
	/* [한국어] 모든 버전이 공통으로 시작하는 16바이트 헤더(magic/crc/version).
	 * 설정자: 디스크에서 read 직후 자동으로 union의 첫 16바이트가 이 멤버로 보인다.
	 * 읽는 자: 업그레이드 디스패처가 header.magic으로 v2 vs v3+ 구분, header.version으로 v3/v5 구분.
	 * 값 범위: magic은 FTL_SUPERBLOCK_MAGIC 또는 FTL_SUPERBLOCK_MAGIC_V2.
	 *   version은 FTL_SB_VERSION_0..FTL_SB_VERSION_CURRENT 범위.
	 * 동기화: 슈퍼블록 read는 디바이스 init 단계의 단일 스레드에서만 발생하므로 락 불필요. */

	struct ftl_superblock_v2 v2;
	/* [한국어] 슈퍼블록 v2 (가장 오래된 형태) 전체 구조 시각.
	 * 설정자: 디스크에서 v2 슈퍼블록을 가진 옛 디바이스를 마운트했을 때 union 위로 캐스팅.
	 * 읽는 자: ftl_superblock_v2 → v3 변환 코드가 lba_cnt, max_active_relocs 같은 옛 필드를 새 형태로 마이그레이션.
	 * 값 범위: ftl_sb_prev.h에 정의된 모든 필드 — header, uuid, seq_id, clean, lba_cnt 등.
	 * 동기화: 동일하게 단일 스레드 init 단계. */

	struct ftl_superblock_v3 v3;
	/* [한국어] 슈퍼블록 v3 — md_layout이 chained 리스트(md_layout_head)로 시작하는 중간 버전.
	 * 설정자: v2→v3 업그레이드 결과 또는 v3 디바이스 읽을 때.
	 * 읽는 자: ftl_sb_v3.c (v3_check_magic, v3_md_layout_load_all, v3_md_region_overflow 등)와
	 *   v3→v5 변환 코드.
	 * 값 범위: ftl_sb_prev.h::ftl_superblock_v3 정의대로 packed 레이아웃. */

	struct ftl_superblock_v5 v5;
	/* [한국어] 슈퍼블록 v5 — md_layout이 blob 영역(blob_area_end + char blob_area[0])로
	 * 통합된 형태. 현재 FTL_SB_VERSION_CURRENT와 사실상 동일.
	 * 설정자: v3→v5 업그레이드 결과 또는 v5 디바이스 마운트 시.
	 * 읽는 자: ftl_sb_v5.c (v5_validate_blob_area, v5_load_blob_area 등). */

	struct ftl_superblock current;
	/* [한국어] 컴파일 시점의 "최신" 슈퍼블록 별칭 (현재 v5와 동일 레이아웃이지만 코드가 항상 최신을
	 * 가리키게 함). 새 버전이 도입되면 ftl_sb_current.h에서 typedef만 갱신하면 된다.
	 * 설정자: 디바이스 정상 마운트 시 ftl_superblock 직접 사용 경로.
	 * 읽는 자: 일반 FTL 동작 코드 — 슈퍼블록 read/write/seq_id 갱신 등.
	 * 값 범위: ftl_sb_current.h::struct ftl_superblock 정의대로. */
} __attribute__((packed));
/* [한국어] packed 속성으로 union 멤버들이 컴파일러 패딩 없이 디스크 레이아웃과 일치하도록 강제.
 * v2/v3/v5/current 어느 멤버로 접근해도 디스크 raw 바이트와 1대1 대응되며,
 * 이는 슈퍼블록의 영구 호환성을 위한 필수 조건이다. */

#endif /* FTL_SB_UPGRADE_H */
/* [한국어] 헤더 가드 종료. */
