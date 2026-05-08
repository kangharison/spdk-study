/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 슈퍼블록 이전 버전(v2/v3/v5) 자료구조 정의 (ftl_sb_prev.h)
 *
 * === 파일의 역할 ===
 * 옛 슈퍼블록 포맷의 인메모리 표현 구조체와 매크로를 한 곳에 모아둔다. 현재 빌드는 이 구조체로
 * 새 슈퍼블록을 만들지 않지만, 기존에 옛 버전으로 포맷된 디바이스를 마운트했을 때 해당 바이트들을
 * 정확히 해석하여 새 버전으로 변환할 수 있어야 한다. 정의되는 것은:
 *   - FTL_MAGIC_V2 / FTL_SUPERBLOCK_MAGIC_V2: pre-v3 magic (8비트 오류 호환성).
 *   - FTL_SB_VERSION_0..4: 옛 버전 번호 상수.
 *   - struct ftl_superblock_v2/v3/v5: 각 버전의 정확한 디스크 레이아웃.
 * v5 구조체는 ftl_sb_current.h의 ftl_superblock과 동일 레이아웃을 의도하되, 미래에 새 버전이
 * 도입되어 ftl_sb_current.h가 갱신되어도 v5 호환성이 깨지지 않도록 별도 보존된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 슈퍼블록 업그레이드 디스패처의 핵심 자료구조 정의.
 * 호출 체인: 디바이스 시작 → 디스크에서 슈퍼블록 read → union ftl_superblock_ver* 캐스팅 →
 *   header.magic으로 v2 vs v3+ 구분, header.version으로 v3/v5 구분 → 해당 멤버로 좁혀 접근.
 * 실행 컨텍스트: SPDK reactor 스레드(디바이스 시작/업그레이드 mngt 단계).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/uuid.h(uuid 멤버용), ftl_sb_common.h(공통 header/gc_info/v3_md_region/v5_blob_hdr).
 * 의존되는 모듈: upgrade/ftl_sb_upgrade.h(union ftl_superblock_ver), ftl_sb_v3.c, ftl_sb_v5.c,
 *   ftl_layout_upgrade.c.
 * 데이터 흐름: 디스크 raw 슈퍼블록 → union 위로 캐스팅 → 옛 멤버로 읽어 새 형태로 변환 후 영속화.
 *
 * === 주요 함수/구조체 요약 ===
 *   - FTL_MAGIC_V2(a,b,c,d): pre-v3 magic 패킹 매크로 (8비트 슬롯 사용 — 옛 버그 호환성).
 *   - FTL_SUPERBLOCK_MAGIC_V2: pre-v3 디바이스 식별 magic.
 *   - FTL_SB_VERSION_0..4: 옛 버전 번호.
 *   - struct ftl_superblock_v2: 가장 오래된 슈퍼블록 (use_append, max_active_relocs 등 옛 필드 포함).
 *   - struct ftl_superblock_v3: chained list MD 레이아웃 도입 버전(md_layout_head).
 *   - struct ftl_superblock_v5: blob 영역 도입 버전(현재 ftl_superblock과 동일 레이아웃).
 */

#ifndef FTL_SB_PREV_H
#define FTL_SB_PREV_H
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "spdk/uuid.h"
/* [한국어] struct spdk_uuid — 모든 슈퍼블록 버전이 가지는 UUID 멤버 정의용. */
#include "ftl_sb_common.h"
/* [한국어] ftl_superblock_header / gc_info / v3_md_region / v5_md_blob_hdr 등 공통 자료구조. */

/**
 * Magic number identifies FTL superblock
 *
 * This old (pre v3) version has a bug - it's generating the magic number off 16b numbers, but only utilizing 8b from each
 */
#define FTL_MAGIC_V2(a, b, c, d) \
    ((UINT64_C(a) << 24) | (UINT64_C(b) << 16) | (UINT64_C(c) << 8) | UINT64_C(d))
/* [한국어] pre-v3 magic 패킹 매크로 (버그 호환성).
 *  - 의도: 16비트 인자 4개를 64비트 magic으로 패킹.
 *  - 실제: 시프트가 24/16/8/0이라 각 인자 하위 8비트만 사용되어 상위 32비트가 항상 0인 버그.
 *  - 그럼에도 디스크에 그대로 기록됐기 때문에 이 매크로를 그대로 보존해야 옛 디바이스를 읽을 수 있다. */

#define FTL_SUPERBLOCK_MAGIC_V2 FTL_MAGIC_V2(0x1410, 0x1683, 0x1920, 0x1989)
/* [한국어] pre-v3 슈퍼블록 식별 magic — 동일한 4-tuple을 v2 매크로로 패킹한 결과.
 * v3 이후의 FTL_SUPERBLOCK_MAGIC과는 비트 패턴이 다르므로 마운트 시 둘 다 시도해 봐야 한다. */

#define FTL_SB_VERSION_0	0
/* [한국어] 가장 초기 버전(거의 사용되지 않음). */
#define FTL_SB_VERSION_1	1
/* [한국어] v1 — 부분적 변경. */
#define FTL_SB_VERSION_2	2
/* [한국어] v2 — struct ftl_superblock_v2 레이아웃에 대응. */
#define FTL_SB_VERSION_3	3
/* [한국어] v3 — chained list MD 레이아웃(md_layout_head) 도입. */
#define FTL_SB_VERSION_4	4
/* [한국어] v4 — v3와 v5 사이의 중간 버전(필드 일부 변경). */

struct ftl_superblock_v2 {
	struct ftl_superblock_header	header;
	/* [한국어] 공통 헤더(magic/crc/version 24바이트). 모든 버전이 동일. */

	struct spdk_uuid		uuid;
	/* [한국어] FTL 인스턴스 UUID 16바이트. */

	/* Current sequence number */
	uint64_t			seq_id;
	/* [한국어] 마지막으로 발급된 시퀀스 번호. */

	/* Flag describing clean shutdown */
	uint64_t			clean;
	/* [한국어] 정상 종료 여부. 0=dirty, 1=clean. */

	/* Number of surfaced LBAs */
	uint64_t			lba_cnt;
	/* [한국어] 사용자에게 노출되는 LBA 개수. */
	/* Number of reserved addresses not exposed to the user */
	size_t				lba_rsvd;
	/* [한국어] 예약 주소 개수(over-provisioning) — v3 이후 overprovisioning 비율로 대체됨.
	 * 설정자: 생성 시. 읽는 자: v2→v3 변환 시 overprovisioning(%) 계산 입력. */

	/* Maximum IO depth per band relocate */
	size_t				max_reloc_qdepth;
	/* [한국어] 밴드 relocate 최대 큐 깊이. v3 이후에도 동일 의미로 보존됨. */

	/* Maximum active band relocates */
	size_t				max_active_relocs;
	/* [한국어] 동시 진행 가능 밴드 relocate 수 — v3 이후에는 동적으로 결정되므로 폐기됨. */

	/* Use append instead of write */
	bool				use_append;
	/* [한국어] write 대신 append 명령 사용 여부 — ZNS/Open-Channel 호환 구간. v3 이후 폐기. */

	/* Maximum supported number of IO channels */
	uint32_t			max_io_channels;
	/* [한국어] 지원 가능 I/O 채널 최대 개수. v3 이후 자동 결정으로 변경. */

	/* Last L2P checkpoint +1 (i.e. min_seq_id, 0:no ckpt) */
	uint64_t			ckpt_seq_id;
	/* [한국어] L2P 체크포인트 시퀀스 번호. 0=체크포인트 없음. v3 이후에도 동일 보존. */

	struct ftl_superblock_gc_info	gc_info;
	/* [한국어] GC 진행 상태 임베드(32바이트). 모든 버전 공통. */
};
/* [한국어] v2 구조체는 packed 속성이 빠져 있다(원본 코드). 컴파일러 패딩이 들어갈 수 있어
 * 디스크 호환성에 위험이 있지만 이미 영속된 v2 디바이스가 그 패딩 가정으로 기록됐다면 그대로 유지해야 한다. */


SPDK_STATIC_ASSERT(offsetof(struct ftl_superblock_v2, header) == 0,
		   "Invalid placement of header");
/* [한국어] header가 첫 멤버임을 강제 — 모든 버전 호환성의 기본. */

SPDK_STATIC_ASSERT(FTL_SUPERBLOCK_SIZE >= sizeof(struct ftl_superblock_v2),
		   "FTL SB metadata size is invalid");
/* [한국어] 영구 영역(128 KiB)이 v2 구조보다 크거나 같아야 함을 강제. */

struct ftl_superblock_v3 {
	struct ftl_superblock_header	header;
	/* [한국어] 공통 헤더(magic/crc/version 24바이트). */

	struct spdk_uuid		uuid;
	/* [한국어] FTL 인스턴스 UUID. */

	/* Current sequence number */
	uint64_t			seq_id;
	/* [한국어] 마지막 시퀀스 번호. */

	/* Flag describing clean shutdown */
	uint64_t			clean;
	/* [한국어] 정상 종료 여부 플래그. */

	/* Number of surfaced LBAs */
	uint64_t			lba_cnt;
	/* [한국어] 사용자 노출 LBA 개수. */

	/* Percentage of base device blocks not exposed to the user */
	uint64_t			overprovisioning;
	/* [한국어] over-provisioning 비율(%). v2의 lba_rsvd 절대값을 비율로 일반화한 결과. */

	/* Maximum IO depth per band relocate */
	uint64_t			max_reloc_qdepth;
	/* [한국어] relocate 최대 큐 깊이 (v2와 동일). */

	/* Reserved field */
	uint8_t				reserved3[16];
	/* [한국어] 미래 확장 예약 16바이트 — v5에서도 동일 위치 보존. */

	/* Last L2P checkpoint +1 (i.e. min_seq_id, 0:no ckpt) */
	uint64_t			ckpt_seq_id;
	/* [한국어] L2P 체크포인트 시퀀스 번호. */

	struct ftl_superblock_gc_info	gc_info;
	/* [한국어] GC 상태 임베드(32바이트). */

	struct ftl_superblock_v3_md_region	md_layout_head;
	/* [한국어] v3에서 도입된 MD 레이아웃 chained list의 첫 노드.
	 * df_next로 다음 노드를 따라가며 모든 MD 영역을 열거 가능.
	 * 설정자: 슈퍼블록 빌드 시 첫 영역으로 채움. 읽는 자: ftl_superblock_v3_md_layout_load_all. */
} __attribute__((packed));
/* [한국어] v3부터는 packed 강제 — chained list 노드의 디스크 레이아웃 안정성을 위해. */

SPDK_STATIC_ASSERT(offsetof(struct ftl_superblock_v3, header) == 0,
		   "Invalid placement of header");
/* [한국어] header 위치 강제. */

SPDK_STATIC_ASSERT(FTL_SUPERBLOCK_SIZE >= sizeof(struct ftl_superblock_v3),
		   "FTL SB metadata size is invalid");
/* [한국어] 영구 영역 충분 크기 강제. */

struct ftl_superblock_v5 {
	struct ftl_superblock_header	header;
	/* [한국어] 공통 헤더. */

	struct spdk_uuid		uuid;
	/* [한국어] FTL 인스턴스 UUID. */

	/* Current sequence number */
	uint64_t			seq_id;
	/* [한국어] 시퀀스 번호. */

	/* Flag describing clean shutdown */
	uint64_t			clean;
	/* [한국어] 정상 종료 플래그. */

	/* Number of surfaced LBAs */
	uint64_t			lba_cnt;
	/* [한국어] 사용자 LBA 개수. */

	/* Percentage of base device blocks not exposed to the user */
	uint64_t			overprovisioning;
	/* [한국어] over-provisioning 비율. */

	/* Maximum IO depth per band relocate */
	uint64_t			max_reloc_qdepth;
	/* [한국어] relocate 최대 큐 깊이. */

	/* Reserved field */
	uint8_t				reserved3[16];
	/* [한국어] 예약 16바이트 — v3와 동일 위치. */

	/* Last L2P checkpoint +1 (i.e. min_seq_id, 0:no ckpt) */
	uint64_t			ckpt_seq_id;
	/* [한국어] L2P 체크포인트 시퀀스 번호. */

	struct ftl_superblock_gc_info	gc_info;
	/* [한국어] GC 상태 임베드. */

	/* Points to the end of blob area */
	ftl_df_obj_id			blob_area_end;
	/* [한국어] v5 blob 영역의 끝 오프셋(슈퍼블록 buf 시작 기준).
	 * 설정자: blob 객체 직렬화 시 갱신. 읽는 자: load 시 blob 끝 결정.
	 * INVALID이면 blob 비어 있음. */

	/* NVC device name */
	char				nvc_dev_name[16];
	/* [한국어] NV cache bdev 이름(고정 16바이트). */

	/* NVC-stored MD layout tracking info */
	struct ftl_superblock_v5_md_blob_hdr	md_layout_nvc;
	/* [한국어] NV cache 저장 MD 레이아웃 추적 blob 헤더. */

	/* Base device name */
	char					base_dev_name[16];
	/* [한국어] base bdev 이름. */

	/* Base dev-stored MD layout tracking info */
	struct ftl_superblock_v5_md_blob_hdr	md_layout_base;
	/* [한국어] base 디바이스 저장 MD 레이아웃 추적 blob 헤더. */

	/* FTL layout params */
	struct ftl_superblock_v5_md_blob_hdr	layout_params;
	/* [한국어] FTL 레이아웃 파라미터 blob 헤더. */

	/* Start of the blob area */
	char blob_area[0];
	/* [한국어] 가변 길이 blob 영역 시작점(zero-length array). FTL_SUPERBLOCK_SIZE까지 확장 가능. */
} __attribute__((packed));
/* [한국어] packed — 디스크 레이아웃 1:1. */

SPDK_STATIC_ASSERT(offsetof(struct ftl_superblock_v5, header) == 0,
		   "Invalid placement of header");
/* [한국어] header 위치 강제. */

SPDK_STATIC_ASSERT(FTL_SUPERBLOCK_SIZE >= sizeof(struct ftl_superblock_v5),
		   "FTL SB metadata size is invalid");
/* [한국어] 영구 영역 충분 크기 강제. */

#endif /* FTL_SB_PREV_H */
/* [한국어] 헤더 가드 종료. */
