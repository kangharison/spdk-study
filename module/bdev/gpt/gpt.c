/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] GPT (GUID Partition Table) 파싱 코어 (gpt.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 디스크에서 읽어온 raw 바이트 버퍼로부터 GPT 헤더와 파티션 엔트리 배열을
 * 파싱하여 검증하는 순수 함수 집합을 제공한다. 즉 디스크 I/O 는 하지 않으며, 호출자
 * (vbdev_gpt.c) 가 미리 spdk_bdev_read 로 채워둔 spdk_gpt::buf 만 사용한다. GPT 는
 * EFI 표준 파티션 테이블 형식으로 (1) LBA 0 의 Protective MBR, (2) LBA 1 의 Primary
 * Header, (3) LBA 2~ 의 Partition Entry Array, 그리고 디스크 끝의 (4) Secondary
 * Header + Entry Array 복사본으로 구성된다. 본 파일은 두 종류의 파싱 단계
 * (PARSE_PHASE_PRIMARY / SECONDARY) 를 같은 함수로 처리하기 위해 phase 분기를 둔다.
 * CRC32 검증(헤더 자체 + 파티션 엔트리 배열 전체) 으로 무결성을 보장한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * vbdev_gpt 모듈은 SPDK 의 .examine_disk 콜백을 사용한다. bdev 가 등록되면 lib/bdev 가
 * vbdev_gpt_examine 을 호출 → vbdev_gpt_read_gpt 가 디스크 앞부분을 spdk_bdev_read 로
 * 가져옴 → 본 파일의 gpt_parse_mbr / gpt_parse_partition_table 로 파싱 → 각 valid
 * 파티션을 spdk_bdev_part 로 등록해 가상 bdev (예: "Nvme0n1p1", "Nvme0n1p2") 로 노출.
 *   [lib/bdev: bdev examine] → [vbdev_gpt.c] → [본 파일: gpt.c]
 *                                              → spdk_bdev_part 등록
 * 실행 컨텍스트: 호스트 유저스페이스 SPDK app thread. 디스크 read 콜백이 발사된 직후.
 *
 * === 타 모듈과의 연결 ===
 * - 입력: spdk_gpt 구조체 (gpt.h 정의). buf/buf_size/sector_size/total_sectors/parse_phase
 *   필드를 호출자가 채워둠.
 * - 출력: spdk_gpt::header (검증된 헤더 포인터), partitions (파티션 엔트리 배열 포인터).
 *   호출자는 partitions[i] 의 starting_lba/ending_lba/part_type_guid/unique_partition_guid
 *   를 읽어 bdev part 를 만든다.
 * - 의존: spdk/crc32 (IEEE CRC32), spdk/endian (little-endian → host 변환).
 * - GPT 표준 참조: UEFI Specification "GPT" chapter.
 *
 * === 주요 함수/구조체 요약 ===
 * - gpt_parse_mbr: LBA 0 의 protective MBR 시그니처와 OS type=0xEE 확인.
 * - gpt_parse_partition_table: 헤더 검증 후 파티션 엔트리 배열 CRC 검증.
 * - gpt_read_header: GPT 시그니처/CRC/my_lba 검증.
 * - gpt_read_partitions: 엔트리 배열 CRC 검증.
 * - gpt_lba_range_check: usable_lba_start/end 가 일관된지.
 * - gpt_check_mbr: MBR 의 0xAA55 시그니처와 protective entry 확인.
 * - gpt_get_header_buf / gpt_get_partitions_buf / gpt_get_expected_head_lba: phase 별 분기.
 */

#include "gpt.h"                  /* [한국어] spdk_gpt 구조체, spdk_gpt_header, spdk_gpt_partition_entry, spdk_mbr 정의 (vbdev_gpt.c 와 공유). */

#include "spdk/crc32.h"           /* [한국어] spdk_crc32_ieee_update — GPT 헤더와 파티션 엔트리 배열 CRC 검증용 (IEEE 802.3 다항식). */
#include "spdk/endian.h"          /* [한국어] from_le16/32/64, to_le32 — GPT 는 little-endian on-disk 포맷이므로 host 바이트 순서로 변환. */
#include "spdk/event.h"           /* [한국어] (역사적 인클루드 — 현재 직접 사용하지 않으나 컴파일 호환성 유지). */

#include "spdk/log.h"             /* [한국어] SPDK_ERRLOG/DEBUGLOG — 파싱 실패 진단 로그. */

/* [한국어] GPT 표준 상수들. UEFI 스펙의 5.3 GPT 섹션 참조. */
#define GPT_PRIMARY_PARTITION_TABLE_LBA 0x1   /* [한국어] Primary GPT header 가 위치한 LBA — MBR(LBA 0) 다음. */
#define PRIMARY_PARTITION_NUMBER 4            /* [한국어] MBR partition table 의 엔트리 수 (legacy DOS partition 4개). protective MBR 검사에서 사용. */
#define GPT_PROTECTIVE_MBR 1                  /* [한국어] gpt_check_mbr 가 "이건 GPT protective MBR 이다" 를 표시하는 내부 마커. */
#define SPDK_MAX_NUM_PARTITION_ENTRIES 128    /* [한국어] SPDK 가 지원하는 최대 파티션 엔트리 수. UEFI 표준 기본값 128 과 일치 — 그 이상은 거부. */

/*
 * [한국어]
 * gpt_get_expected_head_lba - phase 별 GPT 헤더가 위치해야 할 LBA 반환.
 *
 * @gpt: parse 컨텍스트.
 * @return: PRIMARY 면 LBA 1, SECONDARY 면 디스크 마지막 LBA (lba_end).
 *
 * GPT 헤더의 my_lba 필드와 비교하여 손상/오배치 검출.
 */
static uint64_t
gpt_get_expected_head_lba(struct spdk_gpt *gpt)
{
	switch (gpt->parse_phase) {
	case SPDK_GPT_PARSE_PHASE_PRIMARY:
		return GPT_PRIMARY_PARTITION_TABLE_LBA;   /* [한국어] 디스크 앞쪽 — LBA 1. */
	case SPDK_GPT_PARSE_PHASE_SECONDARY:
		return gpt->lba_end;                      /* [한국어] 디스크 끝 — total_sectors-1. */
	default:
		assert(false);                            /* [한국어] 잘못된 phase — 호출자 버그. */
	}
	return 0;                                         /* [한국어] unreachable (assert 후) — 컴파일러 경고 회피용. */
}

/*
 * [한국어]
 * gpt_get_header_buf - 현재 phase 에 맞춰 spdk_gpt::buf 내부의 헤더 시작 주소 계산.
 *
 * @gpt: parse 컨텍스트 (buf, buf_size, sector_size 필요).
 * @return: 헤더가 들어있는 위치를 spdk_gpt_header* 로 cast.
 *
 * PRIMARY: buf 의 LBA 1 위치. SECONDARY: buf 의 마지막 sector (호출자가 secondary 읽을
 * 때 디스크 끝의 buf_size 만큼을 secondary_offset 에서 읽어왔다고 가정).
 */
static struct spdk_gpt_header *
gpt_get_header_buf(struct spdk_gpt *gpt)
{
	switch (gpt->parse_phase) {
	case SPDK_GPT_PARSE_PHASE_PRIMARY:
		return (struct spdk_gpt_header *)
		       (gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);  /* [한국어] buf+1*sector_size. */
	case SPDK_GPT_PARSE_PHASE_SECONDARY:
		return (struct spdk_gpt_header *)
		       (gpt->buf + (gpt->buf_size - gpt->sector_size));                  /* [한국어] buf 끝 sector. */
	default:
		assert(false);
	}
	return NULL;
}

/*
 * [한국어]
 * gpt_get_partitions_buf - phase 별 파티션 엔트리 배열 시작 주소 계산 + bound 검증.
 *
 * @gpt: parse 컨텍스트.
 * @total_partition_size: 전체 엔트리 배열 바이트 크기 (num_entries * entry_size).
 * @partition_start_lba: 헤더의 partition_entry_lba 필드 값.
 * @return: 엔트리 배열 시작 주소. 버퍼 부족 시 NULL.
 *
 * PRIMARY: 헤더가 알려주는 partition_entry_lba 위치로 직접 점프.
 * SECONDARY: 디스크 끝 기준으로 partition_entry_lba 가 buf 의 어디에 위치하는지 역산.
 */
static struct spdk_gpt_partition_entry *
gpt_get_partitions_buf(struct spdk_gpt *gpt, uint64_t total_partition_size,
		       uint64_t partition_start_lba)
{
	uint64_t secondary_total_size;                                   /* [한국어] secondary 케이스에서 entry 배열이 차지하는 바이트 총량. */

	switch (gpt->parse_phase) {
	case SPDK_GPT_PARSE_PHASE_PRIMARY:
		if ((total_partition_size + partition_start_lba * gpt->sector_size) >
		    gpt->buf_size) {                                 /* [한국어] 우리가 읽어둔 버퍼 끝을 벗어나면 거부. */
			SPDK_ERRLOG("Buffer size is not enough\n");
			return NULL;
		}
		return (struct spdk_gpt_partition_entry *)
		       (gpt->buf + partition_start_lba * gpt->sector_size);  /* [한국어] buf 시작부터 partition_start_lba 만큼 오프셋. */
	case SPDK_GPT_PARSE_PHASE_SECONDARY:
		secondary_total_size = (gpt->lba_end - partition_start_lba + 1) * gpt->sector_size;  /* [한국어] 엔트리 배열 + secondary 헤더 까지의 사이즈. */
		if (secondary_total_size > gpt->buf_size) {              /* [한국어] 버퍼 부족 — 호출자가 더 큰 buf 필요. */
			SPDK_ERRLOG("Buffer size is not enough\n");
			return NULL;
		}
		return (struct spdk_gpt_partition_entry *)
		       (gpt->buf + (gpt->buf_size - secondary_total_size));  /* [한국어] 디스크 끝 기준 역산. */
	default:
		assert(false);
	}
	return NULL;
}

/*
 * [한국어]
 * gpt_read_partitions - 파티션 엔트리 배열을 찾아 CRC32 로 무결성 검증.
 *
 * @gpt: parse 컨텍스트 (header 가 이미 set 되어 있어야 함).
 * @return: 0 성공 (gpt->partitions 설정), -1 실패.
 *
 * 검증 항목:
 *  1) num_partition_entries 가 SPDK 한계(128) 이하인가.
 *  2) size_of_partition_entry 가 우리가 아는 struct 크기 (128B) 와 동일한가.
 *  3) 엔트리 배열 전체 CRC32 가 헤더의 partition_entry_array_crc32 와 일치하는가.
 */
static int
gpt_read_partitions(struct spdk_gpt *gpt)
{
	uint32_t total_partition_size, num_partition_entries, partition_entry_size;  /* [한국어] 엔트리 수, 엔트리 1개 크기, 둘의 곱. */
	uint64_t partition_start_lba;                                                /* [한국어] 엔트리 배열이 시작하는 LBA. */
	struct spdk_gpt_header *head = gpt->header;                                  /* [한국어] gpt_read_header 가 검증해 박아둔 헤더. */
	uint32_t crc32;                                                              /* [한국어] 계산한 CRC32. */

	num_partition_entries = from_le32(&head->num_partition_entries);             /* [한국어] LE → host 변환. */
	if (num_partition_entries > SPDK_MAX_NUM_PARTITION_ENTRIES) {                /* [한국어] 비정상적으로 큰 값 — 손상 가능성. */
		SPDK_ERRLOG("Num_partition_entries=%u which exceeds max=%u\n",
			    num_partition_entries, SPDK_MAX_NUM_PARTITION_ENTRIES);
		return -1;
	}

	partition_entry_size = from_le32(&head->size_of_partition_entry);            /* [한국어] 엔트리 1개 크기 (UEFI 표준 128). */
	if (partition_entry_size != sizeof(struct spdk_gpt_partition_entry)) {       /* [한국어] SPDK struct 정의와 불일치 — 미래 확장 비호환. */
		SPDK_ERRLOG("Partition_entry_size(%x) != expected(%zx)\n",
			    partition_entry_size, sizeof(struct spdk_gpt_partition_entry));
		return -1;
	}

	total_partition_size = num_partition_entries * partition_entry_size;         /* [한국어] CRC 계산 대상 범위. */
	partition_start_lba = from_le64(&head->partition_entry_lba);                 /* [한국어] 헤더가 알려주는 배열 시작 LBA. */
	gpt->partitions = gpt_get_partitions_buf(gpt, total_partition_size,
			  partition_start_lba);                                      /* [한국어] phase 별 buf 오프셋 계산. */
	if (!gpt->partitions) {
		SPDK_ERRLOG("Failed to get gpt partitions buf\n");
		return -1;
	}

	crc32 = spdk_crc32_ieee_update(gpt->partitions, total_partition_size, ~0);   /* [한국어] CRC32 계산 (초기값 0xFFFFFFFF). */
	crc32 ^= ~0;                                                                 /* [한국어] 최종 ~ 적용 — IEEE 표준. */

	if (crc32 != from_le32(&head->partition_entry_array_crc32)) {                /* [한국어] 헤더가 기억한 CRC 와 비교. */
		SPDK_ERRLOG("GPT partition entry array crc32 did not match\n");
		return -1;
	}

	return 0;                                                                    /* [한국어] 엔트리 배열 검증 통과 — 호출자가 partitions[] 사용 가능. */
}

/*
 * [한국어]
 * gpt_lba_range_check - 헤더의 usable LBA 범위 일관성 검증.
 *
 * @head: 검증할 GPT 헤더.
 * @lba_end: 디스크 마지막 LBA (total_sectors - 1).
 * @return: 0 정상, -1 비정상.
 *
 * 검증:
 *  - usable_lba_start <= usable_lba_end
 *  - usable_lba_end <= 디스크 마지막 LBA
 *  - LBA 1 (primary header 위치) 이 usable 범위에 포함되면 안 됨 (헤더 자체에 partition 데이터 쓰면 안 됨)
 */
static int
gpt_lba_range_check(struct spdk_gpt_header *head, uint64_t lba_end)
{
	uint64_t usable_lba_start, usable_lba_end;                              /* [한국어] 파티션이 차지 가능한 첫/마지막 LBA. */

	usable_lba_start = from_le64(&head->first_usable_lba);
	usable_lba_end = from_le64(&head->last_usable_lba);

	if (usable_lba_end < usable_lba_start) {                                /* [한국어] 역전 — 손상. */
		SPDK_ERRLOG("Head's usable_lba_end(%" PRIu64 ") < usable_lba_start(%" PRIu64 ")\n",
			    usable_lba_end, usable_lba_start);
		return -1;
	}

	if (usable_lba_end > lba_end) {                                         /* [한국어] 디스크 끝을 벗어남 — 손상. */
		SPDK_ERRLOG("Head's usable_lba_end(%" PRIu64 ") > lba_end(%" PRIu64 ")\n",
			    usable_lba_end, lba_end);
		return -1;
	}

	if ((usable_lba_start < GPT_PRIMARY_PARTITION_TABLE_LBA) &&             /* [한국어] usable 영역이 primary header 를 포함하면 비정상. */
	    (GPT_PRIMARY_PARTITION_TABLE_LBA < usable_lba_end)) {
		SPDK_ERRLOG("Head lba is not in the usable range\n");
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * gpt_read_header - GPT 헤더의 시그니처/CRC32/my_lba 검증 후 spdk_gpt::header 에 박는다.
 *
 * @gpt: parse 컨텍스트.
 * @return: 0 성공, -1 실패.
 *
 * GPT 헤더 검증 절차:
 *  1) header_size 가 합리적 범위인가 (struct 크기 이상, sector_size 이하).
 *  2) CRC32 검증 — 헤더 자체의 header_crc32 필드를 0 으로 두고 계산.
 *  3) gpt_signature ("EFI PART") 일치.
 *  4) my_lba 가 phase 별 기대값(LBA 1 또는 lba_end) 과 일치.
 *  5) usable LBA 범위 일관성.
 *
 * 주의: CRC 계산을 위해 header_crc32 필드를 임시로 0 으로 만들었다가 다시 복원.
 * buf 가 호출자가 disk 에서 읽은 사본이므로 복원하지 않아도 디스크에는 영향 없지만,
 * 외부에서 같은 buf 를 재사용할 수 있으므로 복원하는 게 안전.
 */
static int
gpt_read_header(struct spdk_gpt *gpt)
{
	uint32_t head_size;                                                /* [한국어] 헤더 자체 길이 (CRC 계산 범위). */
	uint32_t new_crc, original_crc;                                    /* [한국어] 계산값, 디스크에 기록된 값. */
	uint64_t my_lba, head_lba;                                         /* [한국어] 헤더의 my_lba, 기대 LBA. */
	struct spdk_gpt_header *head;                                      /* [한국어] buf 내부 헤더 주소. */

	head = gpt_get_header_buf(gpt);                                    /* [한국어] phase 별 위치 계산. */
	if (!head) {
		SPDK_ERRLOG("Failed to get gpt header buf\n");
		return -1;
	}

	head_size = from_le32(&head->header_size);                         /* [한국어] LE → host. */
	if (head_size < sizeof(*head) || head_size > gpt->sector_size) {   /* [한국어] 비합리적 길이 — 손상. */
		SPDK_ERRLOG("head_size=%u\n", head_size);
		return -1;
	}

	original_crc = from_le32(&head->header_crc32);                     /* [한국어] 디스크에 기록된 CRC 보존. */
	head->header_crc32 = 0;                                            /* [한국어] 자기 자신을 포함한 CRC 를 계산할 때는 그 필드를 0 으로 둬야 함 (GPT 규약). */
	new_crc = spdk_crc32_ieee_update(head, from_le32(&head->header_size), ~0);  /* [한국어] header_size 만큼 CRC 계산. */
	new_crc ^= ~0;                                                     /* [한국어] IEEE 최종 ~. */
	/* restore header crc32 */
	to_le32(&head->header_crc32, original_crc);                        /* [한국어] 필드 복원. */

	if (new_crc != original_crc) {                                     /* [한국어] CRC 불일치 → 손상 헤더. */
		SPDK_ERRLOG("head crc32 does not match, provided=%u, calculated=%u\n",
			    original_crc, new_crc);
		return -1;
	}

	if (memcmp(SPDK_GPT_SIGNATURE, head->gpt_signature,
		   sizeof(head->gpt_signature))) {                         /* [한국어] "EFI PART" 매직 — 아니면 GPT 자체가 아님. */
		SPDK_ERRLOG("signature did not match\n");
		return -1;
	}

	head_lba = gpt_get_expected_head_lba(gpt);                         /* [한국어] phase 별 기대 LBA. */
	my_lba = from_le64(&head->my_lba);                                 /* [한국어] 헤더가 주장하는 자기 위치. */
	if (my_lba != head_lba) {                                          /* [한국어] 불일치 — primary 가 secondary 위치에 있다거나 vice versa. */
		SPDK_ERRLOG("head my_lba(%" PRIu64 ") != expected(%" PRIu64 ")\n",
			    my_lba, head_lba);
		return -1;
	}

	if (gpt_lba_range_check(head, gpt->lba_end)) {                     /* [한국어] usable 범위 검증. */
		SPDK_ERRLOG("lba range check error\n");
		return -1;
	}

	gpt->header = head;                                                /* [한국어] 검증 통과 — 호출자가 안전하게 사용 가능. */
	return 0;
}

/*
 * [한국어]
 * gpt_check_mbr - LBA 0 의 Protective MBR 형식 검증.
 *
 * @gpt: parse 컨텍스트.
 * @return: 0 정상 protective MBR, -1 비-GPT 또는 손상.
 *
 * Protective MBR 의 목적: 레거시 OS/도구가 GPT 디스크를 "비어 있는" 것으로 오인하지
 * 않도록, MBR partition table 의 첫 엔트리에 type=0xEE (GPT protective) 를 박아둔다.
 * 검증:
 *  - MBR 시그니처 0xAA55.
 *  - 네 엔트리 중 하나가 OS type 0xEE.
 *  - 그 엔트리의 start_lba == 1 (LBA 1 = primary header).
 *  - 그 엔트리의 size_lba == total_sectors-1 또는 0xFFFFFFFF (2TB 초과 디스크 표기).
 */
static int
gpt_check_mbr(struct spdk_gpt *gpt)
{
	int i, primary_partition = 0;                                                       /* [한국어] protective entry 인덱스. */
	uint32_t total_lba_size = 0, ret = 0, expected_start_lba;                           /* [한국어] 크기, 발견 플래그, 기대 시작 LBA. */
	struct spdk_mbr *mbr;                                                               /* [한국어] buf 시작을 MBR 로 해석. */

	mbr = (struct spdk_mbr *)gpt->buf;                                                  /* [한국어] LBA 0 = MBR. */
	if (from_le16(&mbr->mbr_signature) != SPDK_MBR_SIGNATURE) {                         /* [한국어] 0xAA55 매직 검사. */
		SPDK_DEBUGLOG(gpt_parse, "Signature mismatch, provided=%x,"
			      "expected=%x\n", from_le16(&mbr->disk_signature),
			      SPDK_MBR_SIGNATURE);
		return -1;
	}

	for (i = 0; i < PRIMARY_PARTITION_NUMBER; i++) {                                    /* [한국어] 4 MBR 엔트리 순회. */
		if (mbr->partitions[i].os_type == SPDK_MBR_OS_TYPE_GPT_PROTECTIVE) {        /* [한국어] type 0xEE 발견. */
			primary_partition = i;
			ret = GPT_PROTECTIVE_MBR;                                           /* [한국어] 발견 마킹. */
			break;
		}
	}

	if (ret == GPT_PROTECTIVE_MBR) {
		expected_start_lba = GPT_PRIMARY_PARTITION_TABLE_LBA;                       /* [한국어] LBA 1 이어야 함. */
		if (from_le32(&mbr->partitions[primary_partition].start_lba) != expected_start_lba) {
			SPDK_DEBUGLOG(gpt_parse, "start lba mismatch, provided=%u, expected=%u\n",
				      from_le32(&mbr->partitions[primary_partition].start_lba),
				      expected_start_lba);
			return -1;
		}

		total_lba_size = from_le32(&mbr->partitions[primary_partition].size_lba);
		if ((total_lba_size != ((uint32_t) gpt->total_sectors - 1)) &&              /* [한국어] 디스크 전체 - 1 이거나... */
		    (total_lba_size != 0xFFFFFFFF)) {                                       /* [한국어] ...2TB 초과 시 saturate 값. */
			SPDK_DEBUGLOG(gpt_parse,
				      "GPT Primary MBR size does not equal: (record_size %u != actual_size %u)!\n",
				      total_lba_size, (uint32_t) gpt->total_sectors - 1);
			return -1;
		}
	} else {
		SPDK_DEBUGLOG(gpt_parse, "Currently only support GPT Protective MBR format\n");  /* [한국어] hybrid MBR 등 미지원. */
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * gpt_parse_mbr - 공개 API. buf 의 LBA 0 위치에서 protective MBR 만 검증.
 *
 * @gpt: parse 컨텍스트 (buf, total_sectors 채워져 있어야 함).
 * @return: 0 정상 GPT 디스크, -1 비-GPT.
 *
 * 호출자: vbdev_gpt.c 의 gpt_bdev_complete (primary 읽기 완료 후).
 */
int
gpt_parse_mbr(struct spdk_gpt *gpt)
{
	int rc;

	if (!gpt || !gpt->buf) {                                                            /* [한국어] 방어 — NULL deref 방지. */
		SPDK_ERRLOG("Gpt and the related buffer should not be NULL\n");
		return -1;
	}

	rc = gpt_check_mbr(gpt);                                                            /* [한국어] 실제 검증 위임. */
	if (rc) {
		SPDK_DEBUGLOG(gpt_parse, "Failed to detect gpt in MBR\n");
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * gpt_parse_partition_table - 공개 API. 헤더 + 엔트리 배열 검증을 한 번에 실행.
 *
 * @gpt: parse 컨텍스트.
 * @return: 0 성공 (gpt->header, gpt->partitions 사용 가능), -1 실패.
 *
 * 호출자: vbdev_gpt.c 가 primary 와 secondary 양쪽 단계에서 호출.
 * primary 실패 시 호출자가 phase 를 SECONDARY 로 바꾸고 디스크 끝을 다시 읽은 뒤 재호출.
 */
int
gpt_parse_partition_table(struct spdk_gpt *gpt)
{
	int rc;

	rc = gpt_read_header(gpt);                                                          /* [한국어] 헤더 검증. */
	if (rc) {
		SPDK_ERRLOG("Failed to read gpt header\n");
		return rc;
	}

	rc = gpt_read_partitions(gpt);                                                      /* [한국어] 엔트리 배열 검증. */
	if (rc) {
		SPDK_ERRLOG("Failed to read gpt partitions\n");
		return rc;
	}

	return 0;
}

/* [한국어] SPDK log 컴포넌트 'gpt_parse' 등록 — DEBUGLOG 토글 단위. */
SPDK_LOG_REGISTER_COMPONENT(gpt_parse)
