/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 슈퍼블록 현재 버전(v5) 자료구조 정의 (ftl_sb_current.h)
 *
 * === 파일의 역할 ===
 * 컴파일 시점의 "최신" 슈퍼블록 구조체 struct ftl_superblock과 그 버전 상수
 * (FTL_SB_VERSION_5, FTL_SB_VERSION_CURRENT)를 정의한다. 이 구조체는 NV cache에 영구 저장되며
 * FTL 인스턴스의 모든 핵심 영구 메타데이터(시퀀스 번호, UUID, LBA 개수, GC 상태, blob 영역
 * 메타데이터 트래킹 등)를 담는다. 기본 길이는 ~수백 바이트이지만 끝에 가변 blob_area[0]이 있어
 * 실제 영역은 FTL_SUPERBLOCK_SIZE(128 KiB)까지 확장 가능하다. 새 버전이 추가되면 이 파일 안의
 * 구조체와 매크로만 갱신하면 다른 모든 코드가 "현재 버전"으로 자연스럽게 따라온다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 슈퍼블록 라이프사이클의 인메모리/영구 표현의 정점. 모든 FTL 코드가 dev->sb로 이 구조체에 접근.
 * 호출 체인: ftl_mngt_*(시작/정지/업그레이드) → dev->sb 조작 → ftl_sb.c가 영속화/로드.
 * 실행 컨텍스트: SPDK reactor 스레드에서만 접근. 디바이스 시작/정지/주기 영속화 시점에 갱신.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/uuid.h(spdk_uuid 타입), ftl_sb_common.h(header/gc_info/blob_hdr 등).
 * 의존되는 모듈: ftl_sb.h, ftl_sb.c, ftl_sb_v5.c, ftl_layout_upgrade.c, ftl_core.c,
 *   ftl_band.c, ftl_l2p.c 등 사실상 모든 FTL 핵심 코드.
 * 데이터 흐름: ftl_superblock 인스턴스 ↔ NV cache 슈퍼블록 영역(128 KiB) ↔ 디스크 read/write.
 *
 * === 주요 함수/구조체 요약 ===
 * 함수는 없으며 구조체와 매크로만 정의.
 *   - FTL_SB_VERSION_5 = 5: 현재 슈퍼블록 버전 번호.
 *   - FTL_SB_VERSION_CURRENT: 코드가 항상 가리켜야 할 "최신" 버전 별칭.
 *   - struct ftl_superblock: NV cache에 영구 저장되는 슈퍼블록 본체 (packed).
 *     주요 필드: header(magic/crc/version), uuid, seq_id, clean, lba_cnt, overprovisioning,
 *     max_reloc_qdepth, upgrade_ready, ckpt_seq_id, gc_info, blob_area_end, nvc_dev_name,
 *     md_layout_nvc, base_dev_name, md_layout_base, layout_params, blob_area[0].
 */

#ifndef FTL_SB_CURRENT_H
#define FTL_SB_CURRENT_H
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "spdk/uuid.h"
/* [한국어] struct spdk_uuid 정의 — 슈퍼블록의 uuid 멤버에 사용. */
#include "ftl_sb_common.h"
/* [한국어] ftl_superblock_header, ftl_superblock_gc_info, ftl_superblock_v5_md_blob_hdr 정의. */

#define FTL_SB_VERSION_5			5
/* [한국어] 슈퍼블록 v5 버전 번호 상수. ftl_superblock.header.version에 기록되며,
 * 마운트 시 디스패처가 이 값을 보고 v5 처리 경로 선택. */
#define FTL_SB_VERSION_CURRENT			FTL_SB_VERSION_5
/* [한국어] 컴파일 시점의 "현재 최신" 슈퍼블록 버전. 새 버전 도입 시 이 매크로만 갱신하면
 * 모든 빌드 시스템이 자동으로 새 버전으로 따라온다. */

struct ftl_superblock {
	struct ftl_superblock_header	header;
	/* [한국어] 슈퍼블록 공통 헤더(magic/crc/version 24바이트). 모든 버전의 첫머리에 위치.
	 * 설정자: 슈퍼블록 빌드 시 채워짐. 읽는 자: 마운트 시 매직/버전 검증.
	 * 동기화: 영구 영속화 시 단일 mngt 스레드에서만 갱신. */

	struct spdk_uuid		uuid;
	/* [한국어] FTL 인스턴스 고유 UUID — 다른 FTL 인스턴스와 식별 가능하게 함.
	 * 설정자: 첫 ftl create 시 UUID 생성. 읽는 자: 마운트 시 사용자가 제공한 UUID와 일치 검증.
	 * 값 범위: spdk_uuid 16바이트 임의 값. 동기화: 빌드 후 read-only. */

	/* Current sequence number */
	uint64_t			seq_id;
	/* [한국어] 현재 시퀀스 번호 — 모든 write에 부여되어 dirty 복구 시 순서 결정에 사용.
	 * 설정자: write 발생 시마다 단조 증가, 슈퍼블록 영속화 시 디스크에 기록.
	 * 읽는 자: 부팅 시 직전 시퀀스 복원. 동기화: 단일 스레드 갱신(원자성). */

	/* Flag describing clean shutdown */
	uint64_t			clean;
	/* [한국어] 직전 종료가 정상이었는지 표시(0=dirty, 1=clean).
	 * 설정자: 시작 시 0으로 초기화, 정상 shutdown 직전 1로 갱신 후 영속화.
	 * 읽는 자: 부팅 시 0이면 dirty 복구 경로 선택, 1이면 빠른 부팅. */

	/* Number of surfaced LBAs */
	uint64_t			lba_cnt;
	/* [한국어] 사용자에게 노출되는 LBA 개수(over-provisioning 제외 후의 실제 사용 가능 용량).
	 * 설정자: 첫 생성 시 결정, 이후 read-only. 읽는 자: I/O 경계 검증. */

	/* Percentage of base device blocks not exposed to the user */
	uint64_t			overprovisioning;
	/* [한국어] over-provisioning 비율(%) — base bdev 블록 중 사용자에게 노출하지 않고
	 * GC/wear-leveling 여유로 두는 비율. 설정자: 생성 시. 읽는 자: GC 정책 결정. */

	/* Maximum IO depth per band relocate */
	uint64_t			max_reloc_qdepth;
	/* [한국어] 한 밴드를 GC relocation할 때 동시에 발행 가능한 최대 I/O 깊이.
	 * 설정자: 생성 시. 읽는 자: ftl_reloc.c가 동시 작업 수 제한에 사용. */

	/* Flag indicates that the FTL is ready for upgrade */
	uint8_t				upgrade_ready;
	/* [한국어] FTL이 업그레이드 가능한 상태인지 플래그(예: dirty 데이터 없이 정리됨).
	 * 설정자: shutdown 직전에 true. 읽는 자: 업그레이드 디스패처가 안전 검증 시. */

	/* Reserved field */
	uint8_t				reserved3[15];
	/* [한국어] 미래 확장 예약 15바이트(upgrade_ready와 합쳐 16바이트 정렬).
	 * 디스크 호환성을 위해 0으로 채워두며 새 필드 추가 시 여기를 잠식. */

	/* Last L2P checkpoint +1 (i.e. min_seq_id, 0:no ckpt) */
	uint64_t			ckpt_seq_id;
	/* [한국어] 가장 최근 L2P 체크포인트의 시퀀스 번호 + 1 (즉 다음에 시작할 min_seq_id).
	 * 0이면 체크포인트 없음. 설정자: 체크포인트 영속화 후 갱신. 읽는 자: 부팅 시 복구 시작점. */

	struct ftl_superblock_gc_info	gc_info;
	/* [한국어] GC 진행 상황(band_id_high_prio, current_band_id, band_phys_id, is_valid) 임베디드.
	 * 32바이트. 동기화: 슈퍼블록 영속화는 단일 mngt 스레드에서만. */

	/* Points to the end of blob area */
	ftl_df_obj_id			blob_area_end;
	/* [한국어] blob 영역의 끝 오프셋(슈퍼블록 buf 시작 기준).
	 * 설정자: blob에 객체 직렬화할 때 갱신. 읽는 자: load 시 blob 끝 결정.
	 * INVALID이면 blob 영역이 비어있음. */

	/* NVC device name */
	char				nvc_dev_name[16];
	/* [한국어] NV cache 백엔드 bdev 이름(고정 길이 16바이트, NUL 종료 포함).
	 * 설정자: 생성 시. 읽는 자: 마운트 시 사용자가 지정한 nvc bdev 이름과 일치 검증. */

	/* NVC-stored MD layout tracking info */
	struct ftl_superblock_v5_md_blob_hdr	md_layout_nvc;
	/* [한국어] NV cache에 저장되는 MD 레이아웃 추적 정보(blob 헤더 16바이트).
	 * 설정자: layout 저장 시. 읽는 자: layout 로드 시 blob 안의 위치 환원. */

	/* Base device name */
	char					base_dev_name[16];
	/* [한국어] base bdev 이름(주 영구 저장소). NV cache와 짝을 이뤄 FTL 한 인스턴스를 구성.
	 * 설정자: 생성 시. 읽는 자: 마운트 시 일치 검증. */

	/* Base dev-stored MD layout tracking info */
	struct ftl_superblock_v5_md_blob_hdr	md_layout_base;
	/* [한국어] base 디바이스에 저장되는 MD 레이아웃 추적 정보. */

	/* FTL layout params */
	struct ftl_superblock_v5_md_blob_hdr	layout_params;
	/* [한국어] FTL 레이아웃 파라미터(블록 크기, 영역 정렬 등) blob 헤더. */

	/* Start of the blob area */
	char blob_area[0];
	/* [한국어] 가변 길이 blob 영역 시작점(zero-length array, GNU C 확장).
	 * 슈퍼블록 끝부터 FTL_SUPERBLOCK_SIZE 까지의 영역에 임의 길이 객체를 직렬화 가능.
	 * 설정자: blob 객체 직렬화 코드. 읽는 자: ftl_df_get_obj_ptr로 환원해 객체 접근. */
} __attribute__((packed));
/* [한국어] packed — 디스크 레이아웃과 1:1, 컴파일러가 패딩을 추가하지 않도록 강제. */

SPDK_STATIC_ASSERT(offsetof(struct ftl_superblock, header) == 0,
		   "Invalid placement of header");
/* [한국어] header가 정확히 첫 번째 멤버(오프셋 0)임을 컴파일 시 강제 — 모든 버전 호환성 핵심.
 * 디스패처가 첫 24바이트를 ftl_superblock_header*로 캐스팅해서 magic/version을 확인하기 때문. */

SPDK_STATIC_ASSERT(FTL_SUPERBLOCK_SIZE >= sizeof(struct ftl_superblock),
		   "FTL SB metadata size is invalid");
/* [한국어] 영구 영역 크기(128 KiB)가 ftl_superblock 고정 부분보다 크거나 같음을 강제.
 * 누군가 필드를 추가해 슈퍼블록이 128 KiB를 초과하면 빌드가 실패해 사고를 사전 차단. */

#endif /* FTL_SB_CURRENT_H */
/* [한국어] 헤더 가드 종료. */
