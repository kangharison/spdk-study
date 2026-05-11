/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK RAID superblock(메타데이터) 관리 (bdev_raid_sb.c)
 *
 * === 파일의 역할 ===
 * 본 파일은 SPDK bdev RAID 모듈이 사용하는 "superblock"의 할당, 초기화, 디스크 영속화
 * (write), 디스크 로드(read), 무결성 검증(crc/시그너처/버전)을 담당한다. RAID superblock은
 * 각 base bdev의 LBA 0 영역에 기록되는 작은 메타데이터로, RAID 인스턴스 식별자(UUID),
 * 이름, 레벨(0/1/5F/concat), strip size, base bdev 슬롯/UUID/오프셋 등을 담는다.
 *
 * SPDK 데몬을 재시작했을 때 base bdev들을 examine 하면서 본 파일의 load 경로가 호출되어
 * superblock을 읽어들이고, 같은 RAID에 속한 base bdev들이 모두 발견되면 RAID 코어가
 * 자동으로 raid_bdev를 재구성한다. RAID 토폴로지가 변경(추가/제거/리빌드)되면 본 파일의
 * write 경로가 호출되어 모든 base bdev에 동일한 superblock을 fan-out 기록한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   bdev_raid_examine() / bdev_raid_create RPC
 *     → raid_bdev_load_base_bdev_superblock() [본 파일]
 *       → spdk_bdev_read (base bdev LBA 0)
 *         → raid_bdev_read_sb_cb → raid_bdev_parse_superblock → 사용자 콜백 cb_fn(sb,...)
 *
 *   raid_bdev configure 변경 (base bdev add/remove, 상태 갱신)
 *     → raid_bdev_init_superblock() / raid_bdev_write_superblock() [본 파일]
 *       → 모든 configured base bdev에 spdk_bdev_write fan-out
 *         → raid_bdev_write_superblock_cb → 카운터 감소 → 모두 완료 시 cb_fn 호출
 *
 * 실행 컨텍스트는 SPDK app thread (spdk_thread_get_app_thread). 모든 base bdev이 RAID
 * 메타데이터 갱신 시 동일 스레드에서 fan-out 되도록 강제한다 (write 시 assert 존재).
 *
 * === 타 모듈과의 연결 ===
 * - bdev_raid.h: struct raid_bdev_superblock, raid_bdev, raid_base_bdev_info, RAID_BDEV_SB_*
 *   상수, raid_bdev_load_sb_cb / raid_bdev_write_sb_cb 콜백 타입.
 * - spdk/bdev_module.h: spdk_bdev_read/write, spdk_bdev_io_wait_entry, spdk_bdev_is_md_interleaved
 *   등 lib/bdev 코어 인터페이스.
 * - spdk/crc32.h: superblock CRC32C 계산 (intel CRC32C 인스트럭션 가속).
 * - spdk/env.h: spdk_dma_zmalloc/realloc/free (hugepage·DMA 정렬 메모리 풀).
 * - spdk/util.h: SPDK_ALIGN_CEIL, spdk_divide_round_up, spdk_min 헬퍼.
 *
 * 데이터 흐름:
 *  - 메모리상 raid_bdev->sb : 호스트가 의미적으로 다루는 데이터 영역만의 superblock.
 *  - sb_io_buf            : 디스크 I/O에 직접 사용되는 버퍼.
 *      * 일반 bdev에서는 sb == sb_io_buf (한 버퍼 공유).
 *      * MD-interleaved bdev (bdev->blocklen = data_block_size + md_size, 데이터와 메타가
 *        블록 경계에 섞여 저장됨)에서는 별도 버퍼를 만들고 데이터 블록만 복사해서 기록/판독.
 *
 * === 주요 함수/구조체 요약 ===
 * - raid_bdev_alloc_superblock / free_superblock : sb 버퍼 lifecycle.
 * - raid_bdev_init_superblock                    : 메모리상 sb 필드 초기화 (UUID/level/strip/...)
 * - raid_bdev_write_superblock                   : 모든 base bdev에 fan-out write (CRC 갱신).
 * - raid_bdev_load_base_bdev_superblock          : 단일 base bdev의 LBA 0 영역에서 sb 읽기.
 * - raid_bdev_parse_superblock                   : 시그너처/길이/CRC/버전/슬롯 검증.
 * - raid_bdev_sb_update_crc / sb_check_crc       : CRC32C 산출과 검증.
 * - struct raid_bdev_write_sb_ctx                : fan-out write의 진행 상황과 콜백 보관.
 * - struct raid_bdev_read_sb_ctx                 : 비동기 read의 buffer/desc/콜백 보관.
 */

/* [한국어] spdk/bdev_module.h: bdev 모듈이 사용하는 내부 API. spdk_bdev_read/write,
 * spdk_bdev_is_md_interleaved, spdk_bdev_io_wait_entry, spdk_bdev_get_buf_align 등. */
#include "spdk/bdev_module.h"
/* [한국어] spdk/crc32.h: CRC32C (Castagnoli) 계산 헬퍼. SSE4.2 / x86 CRC32 명령으로 가속됨.
 * superblock 무결성 검증에 사용. */
#include "spdk/crc32.h"
/* [한국어] spdk/env.h: DMA 가능 메모리 (hugepage 기반) 할당 함수 spdk_dma_*. RAID superblock은
 * 디스크 DMA에 사용되므로 정렬된 hugepage 영역에서 잡아야 한다. */
#include "spdk/env.h"
/* [한국어] spdk/log.h: SPDK_ERRLOG, SPDK_DEBUGLOG, SPDK_LOG_REGISTER_COMPONENT 매크로. */
#include "spdk/log.h"
/* [한국어] spdk/string.h: spdk_strerror 등 문자열/오류 변환 헬퍼. */
#include "spdk/string.h"
/* [한국어] spdk/util.h: SPDK_ALIGN_CEIL, spdk_divide_round_up, spdk_min 같은 작은 유틸. */
#include "spdk/util.h"

/* [한국어] bdev_raid.h: RAID 코어 자료구조와 superblock 레이아웃 정의. RAID_BDEV_SB_SIG,
 * RAID_BDEV_SB_VERSION_*, RAID_BDEV_SB_MAX_LENGTH 등의 상수와 raid_bdev_superblock 구조체. */
#include "bdev_raid.h"

/*
 * [한국어]
 * struct raid_bdev_write_sb_ctx
 * superblock fan-out write 진행 상태 보관 컨텍스트.
 *
 * raid_bdev_write_superblock 호출 1회마다 1개씩 calloc되며, 모든 child write가 끝나면 free.
 * 모든 base bdev에 동일한 sb_io_buf를 동시에 기록하기 위해 N개의 child를 발행하고 carry
 * (status/remaining)를 한 곳에서 집계한다.
 *
 * 실행 컨텍스트: SPDK app thread (raid_bdev_write_superblock 진입 시 assert로 강제).
 * 따라서 status/remaining 갱신은 lockless (단일 스레드 직렬 실행).
 */
struct raid_bdev_write_sb_ctx {
	struct raid_bdev *raid_bdev;
	/* [한국어] fan-out 대상 RAID bdev. base_bdev_info[] 와 sb_io_buf/sb_io_buf_size를 사용.
	 * 설정자: raid_bdev_write_superblock 진입 시.
	 * 읽는 자: _raid_bdev_write_superblock 루프와 완료 콜백.
	 * 동기화: app thread 단일 스레드 접근. */

	int status;
	/* [한국어] N개 child 중 하나라도 실패하면 errno (-EIO 등)를 보관. 마지막 child 완료 시
	 * 사용자 콜백에 그대로 전달.
	 * 설정자: raid_bdev_write_superblock_cb / _raid_bdev_write_superblock 에러 경로.
	 * 읽는 자: raid_bdev_write_sb_base_bdev_done (마지막 콜백 호출 시).
	 * 값 범위: 0(성공) 또는 음의 errno. 실패는 덮어쓰기로 마지막 실패값이 보고됨. */

	uint8_t submitted;
	/* [한국어] 지금까지 base bdev에 발행 시도된 child 수 (성공/실패/skip 모두 포함).
	 * 설정자: _raid_bdev_write_superblock 루프 안에서 base bdev 처리 시 증가.
	 * 읽는 자: ENOMEM 재진입 시 i = ctx->submitted부터 이어서 발행하는 데 사용.
	 * 값 범위: 0 ~ raid_bdev->num_base_bdevs. */

	uint8_t remaining;
	/* [한국어] 아직 완료 콜백을 받지 못한 child + 1(=발행 루프 자체) 카운터.
	 * 초기값 = num_base_bdevs + 1 (루프 종료 시 본인을 1 감소시켜 race-free 종료 보장).
	 * 0이 되는 순간 사용자 콜백 cb를 호출하고 ctx free.
	 * 동기화: app thread 단일 스레드 직렬 갱신. */

	raid_bdev_write_sb_cb cb;
	/* [한국어] 모든 child 완료 후 한 번 호출되는 사용자 콜백.
	 * 시그니처: void (*)(int status, struct raid_bdev*, void *cb_ctx).
	 * 설정자: raid_bdev_write_superblock 진입 시 사용자에게 받은 값 보관. */

	void *cb_ctx;
	/* [한국어] cb에 그대로 전달되는 사용자 컨텍스트. SPDK 콜백 관용구. */

	struct spdk_bdev_io_wait_entry wait_entry;
	/* [한국어] base bdev이 ENOMEM을 반환했을 때 자원 회복을 기다리는 큐 엔트리.
	 * cb_fn = _raid_bdev_write_superblock, cb_arg = ctx 로 등록되어 자원 회복 시 동일
	 * 함수가 ctx->submitted 부터 이어서 발행을 재개. lib/bdev 의 표준 wait 메커니즘. */
};

/*
 * [한국어]
 * struct raid_bdev_read_sb_ctx
 * 단일 base bdev에서 superblock을 비동기 read하는 동안 보관되는 컨텍스트.
 *
 * 두 단계로 read를 수행: (1) 기본 sizeof(struct raid_bdev_superblock) 만큼 읽고,
 * (2) sb->length를 읽어서 가변 길이 base_bdevs[]가 더 크면 buf를 realloc해 추가분을 read.
 */
struct raid_bdev_read_sb_ctx {
	struct spdk_bdev_desc *desc;
	/* [한국어] read 대상 base bdev의 SPDK descriptor. examine 경로에서 임시로 열린 desc.
	 * 설정자: raid_bdev_load_base_bdev_superblock 진입 시.
	 * 읽는 자: spdk_bdev_read 호출과 spdk_bdev_desc_get_bdev로 bdev 메타 추출 시. */

	struct spdk_io_channel *ch;
	/* [한국어] read에 사용할 spdk_io_channel (현재 SPDK thread에 매핑된 base bdev I/O 채널).
	 * 설정자: 호출자가 미리 준비. 읽는 자: spdk_bdev_read 호출. */

	raid_bdev_load_sb_cb cb;
	/* [한국어] 사용자 콜백. 시그니처: void (*)(struct raid_bdev_superblock*, int status, void*).
	 * 성공 시 sb 포인터, 실패 시 sb=NULL과 음수 status 전달. */

	void *cb_ctx;
	/* [한국어] cb에 그대로 전달되는 컨텍스트. */

	void *buf;
	/* [한국어] DMA-가능한 read 버퍼. 처음엔 superblock 헤더 크기만, 부족하면 realloc.
	 * 설정자: raid_bdev_load_base_bdev_superblock 의 spdk_dma_malloc, raid_bdev_read_sb_remainder
	 *         의 spdk_dma_realloc. 읽는 자: parse 단계에서 raid_bdev_superblock으로 캐스팅. */

	uint32_t buf_size;
	/* [한국어] 현재 buf의 크기 (바이트). bdev->blocklen 의 정수배. 부족 시 sb->length 만큼 키움.
	 * 값 범위: 최소 1*blocklen ~ 최대 RAID_BDEV_SB_MAX_LENGTH 정렬. */
};

/*
 * [한국어]
 * raid_bdev_alloc_superblock - raid_bdev 메모리상 superblock 버퍼 할당.
 *
 * @raid_bdev : 대상 RAID bdev. 호출 시점에 ->sb 는 NULL이어야 함.
 * @block_size: data block size (메타데이터 제외 실데이터 블록 크기).
 * @return    : 0 성공, -ENOMEM 실패.
 *
 * 동기/배경: superblock은 디스크 I/O에 사용되므로 hugepage·정렬 메모리(0x1000=4KiB 정렬)로
 * 잡고, 길이는 블록 크기 정렬 후 RAID_BDEV_SB_MAX_LENGTH 만큼. 실제 디스크 write에 쓰이는
 * sb_io_buf는 raid_bdev_alloc_sb_io_buf에서 별도로 처리 (MD-interleaved 케이스 분기).
 *
 * 호출 체인:
 *   raid_bdev_create / examine 경로 → 본 함수 → spdk_dma_zmalloc
 */
int
raid_bdev_alloc_superblock(struct raid_bdev *raid_bdev, uint32_t block_size)
{
	/* [한국어] 새로 할당할 superblock 포인터 (실패 시 raid_bdev->sb 원상 유지). */
	struct raid_bdev_superblock *sb;

	/* [한국어] 이중 할당 방지: 호출 시점에 sb가 NULL이어야 함을 단언. */
	assert(raid_bdev->sb == NULL);

	/* [한국어] DMA 정렬(4KiB=0x1000)된 zero-init hugepage 메모리 할당.
	 * 길이는 RAID_BDEV_SB_MAX_LENGTH 를 block_size 경계로 올림 → MD-interleaved 케이스에서도
	 * 블록 단위 데이터 영역만으로 충분히 sb를 담을 수 있게 보장. */
	sb = spdk_dma_zmalloc(SPDK_ALIGN_CEIL(RAID_BDEV_SB_MAX_LENGTH, block_size), 0x1000, NULL);
	if (!sb) {
		/* [한국어] hugepage 부족 등으로 실패. 호출자가 -ENOMEM 처리. */
		SPDK_ERRLOG("Failed to allocate raid bdev sb buffer\n");
		return -ENOMEM;
	}

	/* [한국어] raid_bdev에 보관. 이후 init/write 경로에서 사용. */
	raid_bdev->sb = sb;

	return 0;
}

/*
 * [한국어]
 * raid_bdev_free_superblock - sb 와 sb_io_buf 메모리 해제.
 *
 * @raid_bdev: 대상 RAID bdev.
 *
 * MD-interleaved 케이스에서는 sb와 sb_io_buf가 별도로 할당되어 있으므로 둘 다 free한다.
 * 일반 케이스에서는 sb_io_buf == sb 이므로 sb 한 번만 free (sb_io_buf == sb 일 때 별도
 * free 안 함).
 */
void
raid_bdev_free_superblock(struct raid_bdev *raid_bdev)
{
	/* [한국어] sb_io_buf가 별도 할당된 경우 (MD-interleaved 분기). NULL이거나 sb와 같으면 skip. */
	if (raid_bdev->sb_io_buf != NULL && raid_bdev->sb_io_buf != raid_bdev->sb) {
		/* [한국어] 별도 버퍼는 MD-interleaved 분기에서만 만들어지므로 단언. */
		assert(spdk_bdev_is_md_interleaved(&raid_bdev->bdev));
		/* [한국어] 별도 DMA 버퍼 해제. */
		spdk_dma_free(raid_bdev->sb_io_buf);
		raid_bdev->sb_io_buf = NULL;
	}
	/* [한국어] sb 자체는 항상 spdk_dma_zmalloc으로 만들어졌으므로 spdk_dma_free로 해제. */
	spdk_dma_free(raid_bdev->sb);
	raid_bdev->sb = NULL;
}

/*
 * [한국어]
 * raid_bdev_init_superblock - 메모리상 sb의 모든 필드를 raid_bdev 현재 상태로 채움.
 *
 * @raid_bdev: 이미 base_bdev_info[]와 bdev.uuid/name/blockcnt 등이 채워진 RAID bdev.
 *
 * 동기/배경: 새 RAID 인스턴스 생성 시 한 번 호출되어 sb의 정적 부분(서명/버전/UUID/이름/
 * 레벨/strip/base bdev 슬롯 정보)을 채운다. CRC는 write 시점에 갱신되므로 여기선 미설정.
 *
 * 실행 컨텍스트: 새 RAID 생성 RPC 처리 (app thread).
 */
void
raid_bdev_init_superblock(struct raid_bdev *raid_bdev)
{
	/* [한국어] 작업 대상 sb 핸들. */
	struct raid_bdev_superblock *sb = raid_bdev->sb;
	/* [한국어] base bdev 순회 변수. */
	struct raid_base_bdev_info *base_info;
	/* [한국어] sb->base_bdevs[] 배열의 현재 작성 위치. */
	struct raid_bdev_sb_base_bdev *sb_base_bdev;

	/* [한국어] 시그너처: examine 시 raid_bdev superblock인지 식별하는 매직 바이트열. */
	memcpy(&sb->signature, RAID_BDEV_SB_SIG, sizeof(sb->signature));
	/* [한국어] superblock 포맷 버전 (하위 호환을 위한 major/minor 분리). */
	sb->version.major = RAID_BDEV_SB_VERSION_MAJOR;
	sb->version.minor = RAID_BDEV_SB_VERSION_MINOR;
	/* [한국어] RAID 인스턴스 UUID (사용자 지정 또는 자동 생성). 데몬 재시작 후 동일성 식별 키. */
	spdk_uuid_copy(&sb->uuid, &raid_bdev->bdev.uuid);
	/* [한국어] RAID bdev 이름 (RAID_BDEV_SB_NAME_SIZE 바이트로 잘리거나 패딩). */
	snprintf(sb->name, RAID_BDEV_SB_NAME_SIZE, "%s", raid_bdev->bdev.name);
	/* [한국어] 사용자에게 노출되는 RAID bdev의 총 블록 수. */
	sb->raid_size = raid_bdev->bdev.blockcnt;
	/* [한국어] 데이터 블록 크기 (MD 제외). MD-interleaved에선 bdev.blocklen != block_size. */
	sb->block_size = spdk_bdev_get_data_block_size(&raid_bdev->bdev);
	/* [한국어] RAID 레벨 (raid0, raid1, raid5f, concat 등). enum raid_level. */
	sb->level = raid_bdev->level;
	/* [한국어] strip 크기 (블록 단위). RAID 0/5F의 라운드 분배 단위. */
	sb->strip_size = raid_bdev->strip_size;
	/* TODO: sb->state */
	/* [한국어] base bdev 개수. num_base_bdevs는 현재 활성, base_bdevs_size는 sb 배열 크기.
	 * 동적으로 base bdev이 추가/제거되는 RAID 1에서는 두 값이 다를 수 있도록 분리되어 있음. */
	sb->num_base_bdevs = sb->base_bdevs_size = raid_bdev->num_base_bdevs;
	/* [한국어] sb 본체 + 가변 길이 base_bdevs[] 만큼의 총 길이. write/parse 시 이 길이를 사용. */
	sb->length = sizeof(*sb) + sizeof(*sb_base_bdev) * sb->base_bdevs_size;

	/* [한국어] base_bdevs 배열 시작점에 작성 커서 위치. */
	sb_base_bdev = &sb->base_bdevs[0];
	/* [한국어] 모든 base bdev을 순회하며 sb 슬롯에 메타 기록. */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		/* [한국어] base bdev UUID. examine 시 sb->base_bdevs[i].uuid로 base bdev을 매칭. */
		spdk_uuid_copy(&sb_base_bdev->uuid, &base_info->uuid);
		/* [한국어] base bdev 내 데이터 시작 오프셋 (sb 영역 + 알파를 건너뛴 위치). */
		sb_base_bdev->data_offset = base_info->data_offset;
		/* [한국어] base bdev에서 RAID가 사용하는 데이터 블록 수. */
		sb_base_bdev->data_size = base_info->data_size;
		/* [한국어] 슬롯 상태: configured(정상 동작 중). 결손/리빌드 상태는 별도 enum 값. */
		sb_base_bdev->state = RAID_SB_BASE_BDEV_CONFIGURED;
		/* [한국어] RAID 내 슬롯 번호 (0..num_base_bdevs-1). 슬롯 = "디스크 위치"이며,
		 * RAID 0/5F는 슬롯 순서가 striping/parity 계산에 의미를 가지므로 정확해야 함. */
		sb_base_bdev->slot = raid_bdev_base_bdev_slot(base_info);
		/* [한국어] 다음 슬롯으로 작성 커서 이동. */
		sb_base_bdev++;
	}
}

/*
 * [한국어]
 * raid_bdev_alloc_sb_io_buf - 디스크 I/O에 직접 사용할 sb_io_buf 준비.
 *
 * @raid_bdev: ->sb 가 이미 채워진 RAID bdev.
 * @return   : 0 성공, -ENOMEM 실패.
 *
 * 두 분기:
 *  1) MD-interleaved bdev: 데이터+메타가 블록 경계에 섞여 저장되므로, sb 자체는 데이터
 *     블록만 담고 있으나 디스크에는 bdev->blocklen(=data+md) 단위로 기록해야 한다.
 *     별도의 sb_io_buf를 만들고, write 시점에 sb의 데이터 영역을 sb_io_buf의 데이터
 *     영역으로 복사 (MD 영역은 0/패딩으로 둠).
 *  2) 일반 bdev: blocklen == data_block_size 이므로 sb를 그대로 디스크에 쓰면 됨.
 *     sb_io_buf = sb 로 별칭만 부여 (단, free 시점에 둘 다 해제 안 되도록 비교).
 */
static int
raid_bdev_alloc_sb_io_buf(struct raid_bdev *raid_bdev)
{
	/* [한국어] 메모리상 sb 핸들. length 필드로 디스크 사용 영역 크기를 결정. */
	struct raid_bdev_superblock *sb = raid_bdev->sb;

	if (spdk_bdev_is_md_interleaved(&raid_bdev->bdev)) {
		/* [한국어] MD-interleaved 분기: 디스크 측 크기는 sb->length 를 data_block_size로 나눈
		 * 블록 수에 bdev->blocklen 을 곱한 값 (메타 영역 포함). */
		raid_bdev->sb_io_buf_size = spdk_divide_round_up(sb->length,
					    sb->block_size) * raid_bdev->bdev.blocklen;
		/* [한국어] 별도 DMA 정렬 버퍼 할당. 4KiB 정렬은 NVMe PRP 등 일반 정렬 요건 충족. */
		raid_bdev->sb_io_buf = spdk_dma_zmalloc(raid_bdev->sb_io_buf_size, 0x1000, NULL);
		if (!raid_bdev->sb_io_buf) {
			SPDK_ERRLOG("Failed to allocate raid bdev sb io buffer\n");
			return -ENOMEM;
		}
	} else {
		/* [한국어] 일반 bdev 분기: 디스크 측 크기는 sb->length 를 blocklen 경계로 올림.
		 * 같은 버퍼를 쓰므로 별도 할당 없이 별칭만 부여. free 시 해제 중복 방지를 위해
		 * raid_bdev_free_superblock 에서 sb_io_buf == sb 비교. */
		raid_bdev->sb_io_buf_size = SPDK_ALIGN_CEIL(sb->length, raid_bdev->bdev.blocklen);
		raid_bdev->sb_io_buf = raid_bdev->sb;
	}

	return 0;
}

/*
 * [한국어]
 * raid_bdev_sb_update_crc - sb의 CRC32C 필드를 현재 sb 내용으로 갱신.
 *
 * @sb: 대상 superblock. sb->length 만큼의 영역에 대해 CRC 산출.
 *
 * 절차: CRC 필드를 0으로 reset → 전체 sb를 입력으로 CRC32C 계산 → sb->crc 에 저장.
 * 검증 시(sb_check_crc)에도 동일한 reset → 계산 흐름을 따라야 일치한다.
 */
static void
raid_bdev_sb_update_crc(struct raid_bdev_superblock *sb)
{
	/* [한국어] CRC 필드를 0으로 초기화. CRC 자체는 계산에 포함되지 않아야 하므로 클리어 필수. */
	sb->crc = 0;
	/* [한국어] CRC32C (Castagnoli) 누적 계산. 시드 0에서 시작해 sb->length 만큼 입력. */
	sb->crc = spdk_crc32c_update(sb, sb->length, 0);
}

/*
 * [한국어]
 * raid_bdev_sb_check_crc - 디스크에서 읽어온 sb의 CRC가 유효한지 확인.
 *
 * @sb: 검증 대상 superblock.
 * @return: true 일치(유효), false 불일치(손상/이종 데이터).
 *
 * 알고리즘: 보관된 sb->crc(prev)를 백업 → update_crc로 다시 계산 → 새 crc 와 prev 비교 →
 * sb->crc 를 prev로 복원(side effect 없게). 멀티스레드에선 안전하지 않지만 examine 단계는
 * 단일 스레드 직렬 진행이라 문제 없음.
 */
static bool
raid_bdev_sb_check_crc(struct raid_bdev_superblock *sb)
{
	/* [한국어] 새로 계산한 CRC를 담을 변수와 보존된 원본 CRC. */
	uint32_t crc, prev = sb->crc;

	/* [한국어] 동일 절차로 다시 CRC 계산 (sb->crc는 0으로 reset 후 update). */
	raid_bdev_sb_update_crc(sb);
	crc = sb->crc;
	/* [한국어] sb 본체에 부수효과를 남기지 않도록 원본 CRC 복원. */
	sb->crc = prev;

	/* [한국어] 새로 계산한 CRC와 원본이 같으면 sb는 유효. */
	return crc == prev;
}

/*
 * [한국어]
 * raid_bdev_parse_superblock - 읽어온 sb 후보를 검증.
 *
 * @ctx: read 컨텍스트. ctx->buf가 디스크에서 읽은 raw 바이트.
 * @return : 0 = 유효, -EINVAL = 시그너처/버전/슬롯 오류, -EAGAIN = 길이 부족 (재read 필요).
 *
 * 검증 순서:
 *  1) 시그너처 (RAID_BDEV_SB_SIG): 가장 빠른 reject. 일반 데이터/타 RAID 모듈 식별 차단.
 *  2) 길이 검증: sb->length 가 max 이내이고 buf_size 안에 다 들어와 있는지.
 *     buf_size 부족 → -EAGAIN, 길이 자체가 비정상 → -EINVAL.
 *  3) CRC 검증.
 *  4) 메이저 버전 일치 (다르면 비호환).
 *  5) 마이너 버전 (현재 지원보다 높으면 경고만 남기고 진행 = forward compatibility).
 *  6) base_bdevs[].slot 이 num_base_bdevs 범위 내인지 (인덱싱 안전성).
 */
static int
raid_bdev_parse_superblock(struct raid_bdev_read_sb_ctx *ctx)
{
	/* [한국어] 디스크에서 읽은 raw 바이트를 sb 구조체로 캐스팅. 시그너처 검증 전이므로
	 * 신뢰할 수 없는 데이터일 가능성을 염두에 두고 length, crc 모두 sanity check 한다. */
	struct raid_bdev_superblock *sb = ctx->buf;
	/* [한국어] 어느 base bdev에서 읽었는지 진단 메시지에 사용. */
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ctx->desc);
	struct raid_bdev_sb_base_bdev *sb_base_bdev;
	uint8_t i;

	/* [한국어] 시그너처 비교. 다르면 RAID superblock이 아님 → -EINVAL. (디버그 로그만 출력하여
	 * 다른 모듈/일반 데이터 디스크에서 examine 호출이 와도 시끄럽지 않게 한다.) */
	if (memcmp(sb->signature, RAID_BDEV_SB_SIG, sizeof(sb->signature))) {
		SPDK_DEBUGLOG(bdev_raid_sb, "invalid signature\n");
		return -EINVAL;
	}

	/* [한국어] sb->length 가 buf_size를 넘으면 두 가지 케이스:
	 *  - 정상: base_bdevs_size가 커서 추가 read가 필요 → -EAGAIN 으로 호출자에게 재read 요청.
	 *  - 비정상: max 한도를 초과 → -EINVAL.
	 * 비교는 데이터 블록 단위로 round_up 하여 (md 영역 차이를 무시하고) 수행. */
	if (spdk_divide_round_up(sb->length, spdk_bdev_get_data_block_size(bdev)) >
	    spdk_divide_round_up(ctx->buf_size, bdev->blocklen)) {
		if (sb->length > RAID_BDEV_SB_MAX_LENGTH) {
			SPDK_WARNLOG("Incorrect superblock length on bdev %s\n",
				     spdk_bdev_get_name(bdev));
			return -EINVAL;
		}

		/* [한국어] 길이가 정상 범위 내이지만 현재 buf로는 부족 → 추가 read 요청. */
		return -EAGAIN;
	}

	/* [한국어] CRC 검증. 손상된 디스크/부분 write 흔적 등을 거르는 마지막 무결성 게이트. */
	if (!raid_bdev_sb_check_crc(sb)) {
		SPDK_WARNLOG("Incorrect superblock crc on bdev %s\n", spdk_bdev_get_name(bdev));
		return -EINVAL;
	}

	/* [한국어] 메이저 버전 다르면 ABI 불일치 가정. 자동 마이그레이션은 수행하지 않음. */
	if (sb->version.major != RAID_BDEV_SB_VERSION_MAJOR) {
		SPDK_ERRLOG("Not supported superblock major version %d on bdev %s\n",
			    sb->version.major, spdk_bdev_get_name(bdev));
		return -EINVAL;
	}

	/* [한국어] 마이너 버전이 더 높으면 = 미래 버전. 보수적으로 경고 후 사용 시도 (forward compat). */
	if (sb->version.minor > RAID_BDEV_SB_VERSION_MINOR) {
		SPDK_WARNLOG("Superblock minor version %d on bdev %s is higher than the currently supported: %d\n",
			     sb->version.minor, spdk_bdev_get_name(bdev), RAID_BDEV_SB_VERSION_MINOR);
	}

	/* [한국어] 모든 base bdev 슬롯 번호의 범위 검사. 슬롯이 num_base_bdevs 이상이면 base_info[]
	 * 인덱싱 시 OOB 발생 가능성. 손상된 메타데이터로부터 RAID 코어를 보호하기 위한 방어 코드. */
	for (i = 0; i < sb->base_bdevs_size; i++) {
		sb_base_bdev = &sb->base_bdevs[i];
		if (sb_base_bdev->slot >= sb->num_base_bdevs) {
			SPDK_WARNLOG("Invalid superblock base bdev slot number %u on bdev %s\n",
				     sb_base_bdev->slot, spdk_bdev_get_name(bdev));
			return -EINVAL;
		}
	}

	/* [한국어] 모든 검증 통과. 호출자가 sb를 안전하게 사용할 수 있다. */
	return 0;
}

/*
 * [한국어]
 * raid_bdev_read_sb_ctx_free - read 컨텍스트와 그 안의 DMA 버퍼 해제.
 *
 * @ctx: 해제 대상. 호출자는 콜백을 한 번 더 부르지 않도록 책임을 짐.
 */
static void
raid_bdev_read_sb_ctx_free(struct raid_bdev_read_sb_ctx *ctx)
{
	/* [한국어] DMA 버퍼 (hugepage) 해제. */
	spdk_dma_free(ctx->buf);

	/* [한국어] ctx 자체는 calloc으로 잡았으므로 free. */
	free(ctx);
}

/* [한국어] 전방 선언: read_sb_remainder가 다시 read를 발행하면서 같은 콜백을 사용하기 위함. */
static void raid_bdev_read_sb_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

/*
 * [한국어]
 * raid_bdev_read_sb_remainder - sb->length 가 더 큼을 알게 되어 추가 read를 수행.
 *
 * @ctx: 1차 read를 마친 컨텍스트. ctx->buf 는 이미 sizeof(sb) 만큼의 데이터를 갖고 있고,
 *       sb->length 가 그보다 크다는 사실이 parse에서 -EAGAIN 으로 알려졌다.
 * @return: 0 read 발행 성공 (콜백이 후속 처리), 음수 errno 즉시 실패.
 *
 * 절차:
 *  1) ctx->buf_size 를 sb->length (RAID_BDEV_SB_MAX_LENGTH 상한) 까지 확장하기 위해 realloc.
 *  2) 기존 buf_size 뒤쪽(=새로 늘어난 영역)만 추가로 read (offset=buf_size_prev).
 *  3) 콜백이 다시 호출되어 parse 재시도.
 */
static int
raid_bdev_read_sb_remainder(struct raid_bdev_read_sb_ctx *ctx)
{
	/* [한국어] 1차 read에서 가져온 sb 헤더 (length 등을 사용). */
	struct raid_bdev_superblock *sb = ctx->buf;
	/* [한국어] 디스크 측 정렬 단위 결정에 사용. */
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ctx->desc);
	/* [한국어] realloc 전 buf 크기 (이미 읽혀 있는 영역). */
	uint32_t buf_size_prev;
	void *buf;
	int rc;

	/* [한국어] 이전 buf 크기를 보존하여 부분 read 의 시작 오프셋으로 사용. */
	buf_size_prev = ctx->buf_size;
	/* [한국어] 새 buf_size 계산: min(sb->length, MAX) 를 데이터 블록 크기로 round_up 후
	 * blocklen으로 곱해 디스크 측 길이로 환산. MD-interleaved 분기에서도 정확한 read 길이. */
	ctx->buf_size = spdk_divide_round_up(spdk_min(sb->length, RAID_BDEV_SB_MAX_LENGTH),
					     spdk_bdev_get_data_block_size(bdev)) * bdev->blocklen;
	/* [한국어] DMA 정렬 realloc. 정렬은 bdev이 권장하는 buf_align (NVMe는 보통 512/4096). */
	buf = spdk_dma_realloc(ctx->buf, ctx->buf_size, spdk_bdev_get_buf_align(bdev), NULL);
	if (buf == NULL) {
		SPDK_ERRLOG("Failed to reallocate buffer\n");
		return -ENOMEM;
	}
	/* [한국어] realloc 결과 적용 (이전 ctx->buf는 무효). */
	ctx->buf = buf;

	/* [한국어] 새로 늘어난 영역만 read: offset = buf_size_prev, length = buf_size - buf_size_prev.
	 * 이때 read offset은 base bdev의 LBA 0부터 시작하지 않고 buf_size_prev 바이트 뒤를 의미하지만,
	 * spdk_bdev_read의 인자는 (desc, ch, buf, offset, length) 형식이며 offset도 buf_size_prev로
	 * 지정되어 디스크 첫 sb 영역의 buf_size_prev 바이트 뒤를 읽어 버퍼 뒷부분에 채움. */
	rc = spdk_bdev_read(ctx->desc, ctx->ch, ctx->buf + buf_size_prev, buf_size_prev,
			    ctx->buf_size - buf_size_prev, raid_bdev_read_sb_cb, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to read bdev %s superblock remainder: %s\n",
			    spdk_bdev_get_name(bdev), spdk_strerror(-rc));
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * raid_bdev_read_sb_cb - sb read 완료 콜백 (1차/2차 공통).
 *
 * @bdev_io: 완료된 read child bdev_io.
 * @success: read 성공 여부.
 * @cb_arg : raid_bdev_read_sb_ctx*.
 *
 * 처리 단계:
 *  1) MD-interleaved bdev에서 buf_size > 1 블록이면, 디스크 측 blocklen 간격으로 데이터 블록만
 *     앞으로 압축 (memmove). → buf 안은 "데이터만 있는 sb" 형태가 됨.
 *  2) bdev_io free.
 *  3) 실패면 -EIO 로 사용자 콜백 호출 후 정리.
 *  4) parse 결과:
 *     - 0      : 성공 → sb 포인터를 cb로 전달.
 *     - -EAGAIN: 추가 read 필요 → read_sb_remainder 호출 (성공 시 즉시 return, 콜백이 다시 호출).
 *     - 그 외   : 실패 (sb=NULL, status=음수).
 */
static void
raid_bdev_read_sb_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct raid_bdev_read_sb_ctx *ctx = cb_arg;
	/* [한국어] 사용자 콜백에 넘길 sb 포인터. 실패 경로에선 NULL 유지. */
	struct raid_bdev_superblock *sb = NULL;
	int status;

	/* [한국어] MD-interleaved 케이스 후처리: 디스크에는 [data][md][data][md]... 순으로 들어 있으므로
	 * data 영역만 앞쪽으로 끌어모아 sb 구조체 바이트 레이아웃과 일치시킨다. 첫 블록(i=0)은
	 * 이미 올바른 위치이므로 i=1 부터 시작. memmove는 src/dst overlap 안전. */
	if (spdk_bdev_is_md_interleaved(bdev_io->bdev) && ctx->buf_size > bdev->blocklen) {
		const uint32_t data_block_size = spdk_bdev_get_data_block_size(bdev);
		uint32_t i;

		for (i = 1; i < ctx->buf_size / bdev->blocklen; i++) {
			memmove(ctx->buf + (i * data_block_size),
				ctx->buf + (i * bdev->blocklen),
				data_block_size);
		}
	}

	/* [한국어] read child bdev_io 반납. 같은 스레드에서 호출되어 안전. */
	spdk_bdev_free_io(bdev_io);

	if (!success) {
		/* [한국어] 디스크 read 실패 → 표준 EIO 로 보고 후 정리. */
		status = -EIO;
		goto out;
	}

	/* [한국어] 정상 read → sb 검증 시도. */
	status = raid_bdev_parse_superblock(ctx);
	if (status == -EAGAIN) {
		/* [한국어] sb->length가 더 커서 추가 read가 필요. remainder 발행 후 본 콜백을 다시 받음. */
		status = raid_bdev_read_sb_remainder(ctx);
		if (status == 0) {
			/* [한국어] 추가 read 발행 성공 → 후속 콜백에서 처리. 본 호출은 여기서 종료. */
			return;
		}
	} else if (status != 0) {
		/* [한국어] 시그너처/CRC/버전/슬롯 등 회복 불가 오류. */
		SPDK_DEBUGLOG(bdev_raid_sb, "failed to parse bdev %s superblock\n",
			      spdk_bdev_get_name(spdk_bdev_desc_get_bdev(ctx->desc)));
	} else {
		/* [한국어] 검증 통과 → 사용자에게 sb 포인터 전달 준비. */
		sb = ctx->buf;
	}
out:
	/* [한국어] 사용자 콜백 호출. status==0 이면 sb 유효, 그 외엔 sb=NULL. */
	ctx->cb(sb, status, ctx->cb_ctx);

	/* [한국어] read 컨텍스트 정리. ctx->buf == sb 이므로, 사용자가 sb 내용을 보존해야 한다면
	 * 콜백 안에서 깊은 복사를 수행해야 한다 (현재 RAID 코어가 그렇게 처리). */
	raid_bdev_read_sb_ctx_free(ctx);
}

/*
 * [한국어]
 * raid_bdev_load_base_bdev_superblock - 단일 base bdev에서 sb를 비동기 read 시작.
 *
 * @desc  : examine 중에 임시로 열린 base bdev descriptor.
 * @ch    : 현재 SPDK thread에 매핑된 spdk_io_channel.
 * @cb    : 검증 후 호출될 사용자 콜백.
 * @cb_ctx: cb에 전달할 컨텍스트.
 * @return: 0 read 시작 성공, 음수 errno 즉시 실패 (콜백 호출 안 됨).
 *
 * 두 단계 read의 1단계만 발행. ctx 라이프사이클은 콜백 → free 까지.
 *
 * 호출 체인:
 *   bdev_raid_examine → 본 함수 → spdk_bdev_read → raid_bdev_read_sb_cb → 사용자 cb
 */
int
raid_bdev_load_base_bdev_superblock(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				    raid_bdev_load_sb_cb cb, void *cb_ctx)
{
	/* [한국어] 정렬과 buf 크기 산출에 base bdev 메타 사용. */
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct raid_bdev_read_sb_ctx *ctx;
	int rc;

	/* [한국어] 사용자 콜백은 필수. 없으면 결과 전달 경로가 사라지므로 즉시 abort. */
	assert(cb != NULL);

	/* [한국어] read 컨텍스트 zeroed 할당. */
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	/* [한국어] 컨텍스트 필드 채움. */
	ctx->desc = desc;
	ctx->ch = ch;
	ctx->cb = cb;
	ctx->cb_ctx = cb_ctx;
	/* [한국어] 1차 read 크기 = sizeof(struct raid_bdev_superblock) 을 데이터 블록 단위로 round_up
	 * 후 blocklen 곱 (MD-interleaved 시 데이터+메타 함께). 가변 base_bdevs[] 영역은 우선 안 읽고,
	 * sb->length 가 더 크면 -EAGAIN → remainder 단계에서 마저 읽음. */
	ctx->buf_size = spdk_divide_round_up(sizeof(struct raid_bdev_superblock),
					     spdk_bdev_get_data_block_size(bdev)) * bdev->blocklen;
	/* [한국어] DMA 정렬 (bdev 권장) 메모리 할당. */
	ctx->buf = spdk_dma_malloc(ctx->buf_size, spdk_bdev_get_buf_align(bdev), NULL);
	if (!ctx->buf) {
		rc = -ENOMEM;
		goto err;
	}

	/* [한국어] base bdev LBA 0 부터 buf_size 만큼 read 발행. 완료 시 raid_bdev_read_sb_cb. */
	rc = spdk_bdev_read(desc, ch, ctx->buf, 0, ctx->buf_size, raid_bdev_read_sb_cb, ctx);
	if (rc) {
		goto err;
	}

	return 0;
err:
	/* [한국어] 발행 전 실패: 컨텍스트 정리 (사용자 콜백은 호출하지 않음 — 호출자가 rc를 보고 처리). */
	raid_bdev_read_sb_ctx_free(ctx);

	return rc;
}

/*
 * [한국어]
 * raid_bdev_write_sb_base_bdev_done - fan-out write 진행 카운터 갱신 + 종료 처리.
 *
 * @status: 본 child write 결과 (0 또는 음수 errno).
 * @ctx   : fan-out 컨텍스트.
 *
 * 핵심 규칙: 한 child라도 실패하면 ctx->status에 마지막 실패값을 보관. ctx->remaining 이
 * 0이 되는 순간이 모든 child + 발행 루프 종료가 합쳐진 시점이며, 이때 사용자 콜백 호출.
 */
static void
raid_bdev_write_sb_base_bdev_done(int status, struct raid_bdev_write_sb_ctx *ctx)
{
	/* [한국어] 실패 status를 저장. 여러 child가 실패하면 마지막 값으로 덮어쓰지만, 호출자
	 * 입장에서 "0이면 모두 성공, 0이 아니면 실패가 있었다"만 알면 됨. */
	if (status != 0) {
		ctx->status = status;
	}

	/* [한국어] 카운터 1 감소. 0이 되는 순간 (= 모든 child 완료 + 발행 루프 종료) 사용자 cb 호출.
	 * 단일 스레드(app thread) 직렬 실행이므로 atomic 불요. */
	if (--ctx->remaining == 0) {
		ctx->cb(ctx->status, ctx->raid_bdev, ctx->cb_ctx);
		free(ctx);
	}
}

/*
 * [한국어]
 * raid_bdev_write_superblock_cb - 단일 base bdev sb write 완료 콜백.
 *
 * @bdev_io: 완료된 write child bdev_io.
 * @success: write 성공 여부.
 * @cb_arg : fan-out ctx.
 */
static void
raid_bdev_write_superblock_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_write_sb_ctx *ctx = cb_arg;
	int status = 0;

	if (!success) {
		/* [한국어] superblock write 실패는 메타 일관성 위협. 진단 로그 후 EIO로 보고. */
		SPDK_ERRLOG("Failed to save superblock on bdev %s\n", bdev_io->bdev->name);
		status = -EIO;
	}

	/* [한국어] child bdev_io 반납. */
	spdk_bdev_free_io(bdev_io);

	/* [한국어] fan-out 카운터 갱신 + 모든 자식 + 루프 종료 시 사용자 콜백. */
	raid_bdev_write_sb_base_bdev_done(status, ctx);
}

/*
 * [한국어]
 * _raid_bdev_write_superblock - 모든 configured base bdev에 sb_io_buf를 write fan-out.
 *
 * @_ctx: void* 형태로 들어온 raid_bdev_write_sb_ctx (bdev_io_wait 콜백 시그니처에 맞춤).
 *
 * 실행 컨텍스트: app thread. 동일 함수가 두 가지 경로로 진입할 수 있다:
 *  1) raid_bdev_write_superblock 의 마지막 라인에서 직접 호출 (최초 발행).
 *  2) ENOMEM 발생 시 spdk_bdev_queue_io_wait → 자원 회복 → cb_fn으로 재호출.
 * ctx->submitted 를 보고 이미 발행한 child는 건너뛰며 순차적으로 발행.
 *
 * 종료 규약: 루프 끝에서 raid_bdev_write_sb_base_bdev_done(0, ctx) 를 한 번 호출. 이는
 * remaining 카운터를 1 감소시켜 "발행 루프 자체"의 카운트를 차감 → 모든 child가 이미
 * 끝났더라도 발행 루프가 종료되어야 사용자 콜백이 호출되도록 보장.
 */
static void
_raid_bdev_write_superblock(void *_ctx)
{
	struct raid_bdev_write_sb_ctx *ctx = _ctx;
	struct raid_bdev *raid_bdev = ctx->raid_bdev;
	struct raid_base_bdev_info *base_info;
	uint8_t i;
	int rc;

	/* [한국어] ctx->submitted 부터 이어서 발행 (초기엔 0, ENOMEM 회복 시엔 중단된 인덱스). */
	for (i = ctx->submitted; i < raid_bdev->num_base_bdevs; i++) {
		base_info = &raid_bdev->base_bdev_info[i];

		/* [한국어] 미설정 또는 곧 제거 예정 슬롯은 sb write 대상 아님. carry 카운터를 1
		 * 감소시키고 (즉시 "완료"로 처리) submitted 만 증가시키며 진행. */
		if (!base_info->is_configured || base_info->remove_scheduled) {
			/* [한국어] 마지막 1은 발행 루프 자체 카운트이므로, child 1을 빼기 전엔 1보다 커야 함. */
			assert(ctx->remaining > 1);
			raid_bdev_write_sb_base_bdev_done(0, ctx);
			ctx->submitted++;
			continue;
		}

		/* [한국어] base bdev에 sb_io_buf 를 LBA 0 부터 sb_io_buf_size 만큼 write.
		 * app_thread_ch 는 RAID 코어가 미리 만들어둔 app thread 용 채널이라 본 함수의
		 * 실행 스레드 = app thread 와 일치하여 안전. 완료 시 raid_bdev_write_superblock_cb. */
		rc = spdk_bdev_write(base_info->desc, base_info->app_thread_ch,
				     raid_bdev->sb_io_buf, 0, raid_bdev->sb_io_buf_size,
				     raid_bdev_write_superblock_cb, ctx);
		if (rc != 0) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(base_info->desc);

			if (rc == -ENOMEM) {
				/* [한국어] 자원 부족: bdev_io_wait 큐에 본 함수를 등록하고 중단.
				 * 자원이 회복되면 SPDK가 _raid_bdev_write_superblock(ctx)을 다시 호출 →
				 * ctx->submitted 부터 이어서 발행을 재개. submitted를 증가시키지 않고
				 * return하므로 동일 base bdev이 재시도됨. */
				ctx->wait_entry.bdev = bdev;
				ctx->wait_entry.cb_fn = _raid_bdev_write_superblock;
				ctx->wait_entry.cb_arg = ctx;
				spdk_bdev_queue_io_wait(bdev, base_info->app_thread_ch, &ctx->wait_entry);
				return;
			}

			/* [한국어] ENOMEM이 아닌 즉시 실패: 이 child의 몫만 done(rc) 으로 보고. */
			assert(ctx->remaining > 1);
			raid_bdev_write_sb_base_bdev_done(rc, ctx);
		}

		/* [한국어] 발행을 시도했으므로(성공/즉시실패 모두) submitted 증가. ENOMEM 경로는 위에서
		 * return되어 여기 도달하지 않으므로 재발행이 보장됨. */
		ctx->submitted++;
	}

	/* [한국어] 발행 루프 종료를 1 감소로 보고. remaining = 1 인 상태(모든 child 이미 콜백)에서
	 * 이 호출이 0으로 떨어뜨려 사용자 cb를 부른다. 즉 "발행 루프"도 가상의 1개 child로 취급. */
	raid_bdev_write_sb_base_bdev_done(0, ctx);
}

/*
 * [한국어]
 * raid_bdev_write_superblock - sb 갱신을 모든 base bdev에 영속화 시작.
 *
 * @raid_bdev: 대상 RAID bdev. ->sb 가 init 또는 갱신된 상태여야 함.
 * @cb       : 모든 base bdev write 완료 후 한 번 호출될 콜백.
 * @cb_ctx   : cb에 전달할 컨텍스트.
 *
 * 동기/배경: RAID 토폴로지 변경 시 또는 seq_number 갱신 시 호출. 본 함수는 비동기이며,
 * 실패는 cb의 첫 인자 status로 보고된다 (반환값 없음). 실패는 즉시 (sb_io_buf 할당/ctx alloc
 * 단계) 또는 비동기 (개별 child write) 모두 cb로 통합 처리.
 *
 * 주요 단계:
 *  1) sb_io_buf 가 없으면 alloc.
 *  2) ctx alloc, remaining = num_base_bdevs + 1 (자식 + 루프).
 *  3) sb->seq_number 증가, CRC 갱신.
 *  4) MD-interleaved 분기에서 sb 데이터 → sb_io_buf 의 데이터 영역으로 블록 단위 복사.
 *  5) _raid_bdev_write_superblock 으로 fan-out 시작.
 *
 * 실행 컨텍스트: 반드시 app thread (assert).
 */
void
raid_bdev_write_superblock(struct raid_bdev *raid_bdev, raid_bdev_write_sb_cb cb, void *cb_ctx)
{
	struct raid_bdev_write_sb_ctx *ctx;
	struct raid_bdev_superblock *sb = raid_bdev->sb;
	int rc;

	/* [한국어] 본 함수는 app thread에서만 호출되어야 한다 (모든 base bdev에 fan-out 하려면
	 * 각 base bdev의 app_thread_ch를 사용하며, 이 채널은 app thread 전용). */
	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	/* [한국어] sb는 alloc/init 후에 호출돼야 한다. */
	assert(sb != NULL);
	/* [한국어] 결과 통지 채널이 없으면 의미가 없다. */
	assert(cb != NULL);

	if (raid_bdev->sb_io_buf == NULL) {
		/* [한국어] 첫 write 호출 시 sb_io_buf 준비 (MD-interleaved 분기 포함). */
		rc = raid_bdev_alloc_sb_io_buf(raid_bdev);
		if (rc != 0) {
			goto err;
		}
	}

	/* [한국어] fan-out 진행 컨텍스트 zeroed alloc. */
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		rc = -ENOMEM;
		goto err;
	}

	ctx->raid_bdev = raid_bdev;
	/* [한국어] 자식 N개 + 발행 루프 1개 = N+1. _raid_bdev_write_superblock 종료 시 1 감소. */
	ctx->remaining = raid_bdev->num_base_bdevs + 1;
	ctx->cb = cb;
	ctx->cb_ctx = cb_ctx;

	/* [한국어] 단조 증가 시퀀스 번호 (여러 base bdev에서 가장 큰 seq를 선택해 latest 결정). */
	sb->seq_number++;
	/* [한국어] 시퀀스 등 변경된 필드를 반영해 CRC 갱신. write 직전 마지막 단계. */
	raid_bdev_sb_update_crc(sb);

	if (spdk_bdev_is_md_interleaved(&raid_bdev->bdev)) {
		/* [한국어] MD-interleaved 분기: sb (데이터만 연속) → sb_io_buf (데이터+메타 인터리브)
		 * 로 블록 단위 복사. 메타 영역은 그대로(0/이전 값) 두며, RAID는 메타 영역을 직접
		 * 사용하지 않는다 (SSD가 자체 ECC/PI 등으로 사용할 수 있음). */
		void *sb_buf = sb;
		uint32_t i;

		for (i = 0; i < raid_bdev->sb_io_buf_size / raid_bdev->bdev.blocklen; i++) {
			memcpy(raid_bdev->sb_io_buf + (i * raid_bdev->bdev.blocklen),
			       sb_buf + (i * sb->block_size), sb->block_size);
		}
	}

	/* [한국어] 동기적으로 fan-out 시작. 내부에서 비동기 write 발행 후 즉시 반환. */
	_raid_bdev_write_superblock(ctx);
	return;
err:
	/* [한국어] 동기 실패 경로: 사용자 cb를 즉시 호출해 일관된 에러 통지. */
	cb(rc, raid_bdev, cb_ctx);
}

/* [한국어] "bdev_raid_sb" 디버그 컴포넌트 등록. SPDK_DEBUGLOG의 첫 인자에 사용되며 RPC
 * log_set_flag 로 런타임 활성화. examine 중 sb 검증 실패 같은 흔한 상황을 시끄럽지 않게
 * 디버그 로그로 격리하는 데 쓰인다. */
SPDK_LOG_REGISTER_COMPONENT(bdev_raid_sb)
