/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * GPT internal Interface
 */

/*
 * [한국어 설명] GPT(GUID Partition Table) 파싱 모듈 내부 헤더 (gpt.h)
 *
 * === 파일의 역할 ===
 * 베이스 bdev 첫 32KB를 읽어 GPT 헤더와 파티션 엔트리를 파싱하는 모듈의 내부 데이터 구조와
 * 함수를 정의한다. UEFI GPT 표준은 디스크 LBA0에 Protective MBR을 두고, LBA1에 Primary GPT
 * Header, 그 뒤에 Partition Entry Array를 둔다. Secondary는 디스크 끝에 같은 구조를 미러링한다.
 * 본 모듈은 Primary가 손상되어 있으면 Secondary로 fallback한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [bdev examiner subsystem]
 *     → vbdev_gpt.c (examine_disk 콜백)
 *         → spdk_bdev_read(첫 32KB)
 *             → gpt_parse_mbr / gpt_parse_partition_table (이 헤더)
 *                 → 파티션 N개에 대해 vbdev_gpt_partition을 spdk_bdev로 등록
 *
 * === 타 모듈과의 연결 ===
 * - vbdev_gpt.c : 본 헤더의 함수를 호출하는 상위 레이어.
 * - include/spdk/gpt_spec.h : GPT 표준 와이어 포맷(header/partition entry layout).
 * - 베이스 bdev : 읽기 대상.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_gpt : 파싱 상태/결과 컨테이너.
 * - enum spdk_gpt_parse_phase : Primary/Secondary 파싱 단계 enum.
 * - gpt_parse_mbr() : Protective MBR 검증.
 * - gpt_parse_partition_table() : GPT 헤더+엔트리 파싱.
 */

#ifndef SPDK_INTERNAL_GPT_H
#define SPDK_INTERNAL_GPT_H

#include "spdk/stdinc.h"
/* [한국어] 표준 헤더 모음. */

#include "spdk/gpt_spec.h"
/* [한국어] GPT 와이어 포맷 정의(spdk_gpt_header, spdk_gpt_partition_entry, spdk_gpt_guid). */
#include "spdk/log.h"
/* [한국어] SPDK_LOG_DEPRECATION_REGISTER 매크로 사용. */

/* [한국어] SPDK_GPT_PART_TYPE_GUID
 * SPDK가 만든 GPT 파티션의 Type GUID. 파티션 엔트리의 partition_type_guid 필드가
 * 이 값이어야 SPDK GPT examiner가 파티션을 vbdev로 등록한다. */
#define SPDK_GPT_PART_TYPE_GUID     SPDK_GPT_GUID(0x6527994e, 0x2c5a, 0x4eec, 0x9613, 0x8f5944074e8b)

/* PART_TYPE_GUID_OLD partitions will be constructed as bdevs with one fewer block than expected.
 * See GitHub issue #2801.
 */
#ifdef REGISTER_GUID_DEPRECATION
/* Register the deprecation in the header file, to make it clear to readers that this GUID
 * shouldn't be used for new SPDK GPT partitions.  We will never actually log this deprecation
 * though, since we are not recommending that users try to migrate existing partitions with the
 * old GUID to the new GUID. Wrap it in this REGISTER_GUID_DEPRECATION flag to avoid defining
 * this deprecation in multiple compilation units.
 */
/* [한국어] 위 영문 주석 부연: 옛 SPDK GPT GUID는 deprecated이며 새 파티션에 사용하면 안 된다.
 * 이미 만들어진 파티션은 호환을 위해 1블록 적게 노출되는 버그가 남아있다(GitHub #2801).
 * REGISTER_GUID_DEPRECATION 매크로는 .c 파일 1개에서만 실제 정의가 되도록 하기 위한 가드. */
SPDK_LOG_DEPRECATION_REGISTER(old_gpt_guid, "old gpt guid", "Never", SPDK_LOG_DEPRECATION_ALWAYS)
#endif
/* [한국어] SPDK_GPT_PART_TYPE_GUID_OLD: deprecated. 기존 디바이스 호환용으로만 인식. */
#define SPDK_GPT_PART_TYPE_GUID_OLD SPDK_GPT_GUID(0x7c5222bd, 0x8f5d, 0x4087, 0x9c00, 0xbf9843c7b58c)

/* [한국어] GPT 파싱용 버퍼 크기. MBR(LBA0) + GPT 헤더(LBA1) + 일부 partition entries를 담기에 충분. */
#define SPDK_GPT_BUFFER_SIZE 32768  /* 32KB */
/* [한국어] GUID 동일성 비교 매크로. memcmp로 raw 비교 — UUID 길이가 정확히 16 byte라는 전제. */
#define	SPDK_GPT_GUID_EQUAL(x,y) (memcmp(x, y, sizeof(struct spdk_gpt_guid)) == 0)

/*
 * [한국어] enum spdk_gpt_parse_phase
 * GPT는 디스크에 Primary와 Secondary 두 벌이 저장되므로 파서가 어느 쪽을 처리 중인지를 표시.
 */
enum spdk_gpt_parse_phase {
	SPDK_GPT_PARSE_PHASE_INVALID = 0,
	/* [한국어] 초기값 / 잘못된 상태. parse 호출 전. */
	SPDK_GPT_PARSE_PHASE_PRIMARY,
	/* [한국어] Primary GPT(디스크 시작) 파싱 중. 실패 시 Secondary로 fallback. */
	SPDK_GPT_PARSE_PHASE_SECONDARY,
	/* [한국어] Secondary GPT(디스크 끝) 파싱 중. */
};

/*
 * [한국어] struct spdk_gpt
 * GPT 파싱의 입력/출력을 모두 담는 컨테이너. examiner가 읽어둔 raw buffer와 디스크 메타데이터를
 * 입력으로 받고, 파싱 후에는 header와 partitions가 buf 내부의 적절한 오프셋을 가리키도록 설정된다.
 */
struct spdk_gpt {
	uint8_t parse_phase;
	/* [한국어] 현재 파싱 단계(enum spdk_gpt_parse_phase 값).
	 * 설정자: examiner가 Primary 시도 후 실패 시 Secondary로 변경. 읽는 자: gpt_parse_*. */
	unsigned char *buf;
	/* [한국어] 디스크에서 읽은 raw 바이트 버퍼 시작. examiner가 spdk_bdev_read로 채워둠. */
	uint64_t buf_size;
	/* [한국어] buf 길이(보통 SPDK_GPT_BUFFER_SIZE). */
	uint64_t lba_start;
	/* [한국어] 파싱된 디스크의 사용 가능 LBA 시작. GPT header.first_usable_lba에서 채움. */
	uint64_t lba_end;
	/* [한국어] 사용 가능 LBA 끝. header.last_usable_lba. */
	uint64_t total_sectors;
	/* [한국어] 디스크의 총 섹터 수. examiner가 bdev->blockcnt에서 채워줌. */
	uint32_t sector_size;
	/* [한국어] 섹터 크기(바이트). examiner가 bdev->blocklen에서 채워줌. */
	struct spdk_gpt_header *header;
	/* [한국어] buf 내부에서 해석된 GPT 헤더 포인터. parse 후 유효. */
	struct spdk_gpt_partition_entry *partitions;
	/* [한국어] buf 내부에서 시작하는 partition entry 배열 포인터. header.num_partition_entries 길이. */
};

/*
 * [한국어]
 * gpt_parse_mbr - LBA0의 Protective MBR을 검증
 * @gpt: 파싱 컨텍스트. buf/buf_size/sector_size 사전 채움 필요.
 * @return: 0 성공(GPT 디스크임을 확인), 음수 -errno.
 */
int gpt_parse_mbr(struct spdk_gpt *gpt);

/*
 * [한국어]
 * gpt_parse_partition_table - LBA1 헤더 + 엔트리 배열을 파싱
 * @gpt: 위와 동일. parse_phase로 Primary/Secondary 표시.
 * @return: 0 성공, 음수 -errno (CRC32 불일치, 매직 불일치, num_entries 과대 등).
 */
int gpt_parse_partition_table(struct spdk_gpt *gpt);

#endif  /* SPDK_INTERNAL_GPT_H */
