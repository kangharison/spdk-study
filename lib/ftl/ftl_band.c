/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright 2023 Solidigm All Rights Reserved
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 밴드 메타데이터/상태 관리 (ftl_band.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 FTL의 핵심 단위인 "밴드(band)"의 메타데이터(P2L 맵, 상태 머신, write count, valid map 등)를
 * 관리한다. 밴드는 NAND 플래시의 erase 단위(여러 zone의 묶음)에 대응하는 추상화로, 각 밴드는 자체 P2L
 * (Physical-to-Logical) 맵을 가지며 상태 머신(FREE → PREP → OPENING → OPEN → FULL → CLOSING → CLOSED)을
 * 따라 전이한다. 본 파일은 (1) 상태 천이 함수들, (2) P2L 맵 메모리 풀에서의 alloc/free, (3) 주소 변환
 * (밴드 ID ↔ ftl_addr ↔ block offset), (4) GC가 다음 reloc 대상 밴드를 선택하는 알고리즘
 * (ftl_band_search_next_to_reloc 및 invalidity/wr_cnt 비교 휴리스틱)을 책임진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: ftl_band_ops.c(I/O 발행) ← 본 파일 ← ftl_core.c(I/O 디스패처), ftl_writer/reloc/mngt
 *  (writer/relocation/management 흐름이 상태 천이를 트리거).
 * 본 파일은 디스크에 직접 I/O를 발행하지 않고, 메모리 상의 밴드 자료구조(p2l_map, band->md 등)를 다룬다.
 * I/O 발행은 ftl_band_ops.c가 담당하며, 두 파일은 짝을 이뤄 동작한다.
 * 실행 컨텍스트: 디바이스 단일 spdk_thread. 모든 밴드 자료구조 갱신은 그 스레드에서만 일어나므로
 * 락이 없다. dev->sb_shm은 공유 메모리(IPC)이지만 단일 owner가 갱신.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(호출자): ftl_writer(WRITE 시 ftl_band_set_p2l/set_addr 호출), ftl_reloc(GC 시
 *   ftl_band_search_next_to_reloc/iter API 사용), ftl_mngt(밴드 init/state 전환), ftl_band_ops(상태 천이
 *   콜백에서 본 파일의 set_state 호출).
 * - 하위(피호출자): ftl_mempool(p2l_pool/band_md_pool로부터 메모리 풀 alloc/free), ftl_p2l_ckpt(P2L
 *   체크포인트 영역 acquire/release), ftl_bitmap(valid_map 갱신), spdk_crc32c.
 * - 공유 자료구조: struct ftl_band(상태/P2L 맵/iter/queue_entry), struct spdk_ftl_dev::bands(밴드 배열),
 *   dev->free_bands/shut_bands(상태별 TAILQ), dev->sb_shm->gc_info(GC iterator state, dirty-shutdown
 *   복원에 사용).
 *
 * === 주요 함수/구조체 요약 ===
 * - ftl_band_set_state: 상태 머신 전이의 단일 진입점. 천이별 부속 작업(_ftl_band_set_free 등)을 디스패치.
 * - ftl_band_set_p2l/set_addr: 사용자 쓰기 시 LBA → 물리 주소 매핑을 P2L 맵과 valid bitmap에 기록.
 * - ftl_band_alloc_p2l_map / ftl_band_release_p2l_map: P2L 맵 메모리 ref-counted 관리.
 * - ftl_band_block_offset_from_addr / ftl_band_next_xfer_addr: 밴드 내 주소 산술 (xfer_size 정렬 고려).
 * - ftl_band_search_next_to_reloc: GC 휴리스틱(invalidity > 10%면 invalidity 비교, 비슷하면 wr_cnt가
 *   적은 쪽, 그래도 비슷하면 ID 작은 쪽). wear leveling과 회수 효율의 균형.
 * - ftl_bands_load_state / ftl_band_initialize_free_state: startup 시 밴드 메타데이터 로드 후 FREE
 *   리스트에 등록.
 */

/* [한국어] SPDK 표준 헤더 — 표준 C 헤더 추상화. */
#include "spdk/stdinc.h"
/* [한국어] CRC32C — P2L 맵 무결성 검증에 사용. */
#include "spdk/crc32.h"
/* [한국어] spdk_likely/spdk_unlikely 분기 예측 힌트. */
#include "spdk/likely.h"
/* [한국어] SPDK_COUNTOF 등 유틸 매크로. */
#include "spdk/util.h"
/* [한국어] FTL 공개 API(SPDK_FTL_MODE_CREATE 등). */
#include "spdk/ftl.h"

/* [한국어] 본 파일이 정의하는 함수 선언과 ftl_band 구조체. */
#include "ftl_band.h"
/* [한국어] ftl_io 객체와 IO 헬퍼 — band가 직접 IO를 만들지는 않지만 일부 헬퍼가 ftl_io 형식을 사용. */
#include "ftl_io.h"
/* [한국어] FTL 코어 — spdk_ftl_dev, ftl_get_num_blocks_in_band, ftl_apply_limits 등. */
#include "ftl_core.h"
/* [한국어] 디버그 로그 매크로(FTL_DEBUGLOG/FTL_ERRLOG). */
#include "ftl_debug.h"
/* [한국어] FTL 내부 공용 자료구조와 상수(FTL_BLOCK_SIZE, FTL_DF_OBJ_ID_INVALID 등). */
#include "ftl_internal.h"
/* [한국어] FTL 메타데이터 영역 헬퍼 — ftl_md_persist_entries 등. */
#include "utils/ftl_md.h"
/* [한국어] FTL 공통 정의 매크로(FTL_BAND_ID_INVALID 등). */
#include "utils/ftl_defs.h"

/*
 * [한국어]
 * ftl_band_tail_md_offset - 밴드 안에서 tail 메타데이터(P2L 맵)가 시작되는 블록 오프셋을 계산.
 *
 * @band: 대상 밴드.
 * @return: 밴드 시작점에서 tail md 시작점까지의 블록 수 = (밴드 총 블록 수) - (tail md 블록 수).
 *
 * 밴드 레이아웃: [user data ........... | tail md (P2L map)]
 * 사용자 데이터는 앞쪽부터 채워지고, 가장 끝 부분은 P2L 맵 메타데이터가 차지한다. 이 함수는
 * 그 경계점을 계산한다 — 밴드가 가득 찼는지(append iter가 이 위치에 도달했는지) 판단하는 기준.
 *
 * 호출 체인: 내부에서 ftl_band_filled, ftl_band_tail_md_addr, ftl_band_user_blocks_left 등이 사용.
 */
static uint64_t
ftl_band_tail_md_offset(const struct ftl_band *band)
{
	/* [한국어] (밴드 총 블록 수) - (tail md 블록 수) — 사용자 데이터 영역의 마지막 블록 다음 위치. */
	return ftl_get_num_blocks_in_band(band->dev) -
	       ftl_tail_md_num_blocks(band->dev);
}

/*
 * [한국어]
 * ftl_band_filled - iter offset이 사용자 영역의 끝(tail md 시작 위치)에 도달했는지 검사.
 *
 * @band:   대상 밴드.
 * @offset: 현재 iter offset (밴드 시작에서부터의 블록 수).
 * @return: 1이면 가득 참(이제 tail md 쓰기 단계로 진행), 0이면 아직 빈 공간 있음.
 *
 * ftl_band_rq_write/basic_rq_write에서 매 발행 후 호출되어, 가득 차면 FULL 상태로 천이하도록 트리거.
 */
int
ftl_band_filled(struct ftl_band *band, size_t offset)
{
	/* [한국어] 단순 비교 — iter가 정확히 tail md 시작 오프셋에 도달했는지. xfer_size 정렬 보장에 의존. */
	return offset == ftl_band_tail_md_offset(band);
}

/*
 * [한국어]
 * ftl_band_free_p2l_map - 밴드의 P2L 맵 메모리 풀 객체를 풀에 반납.
 *
 * @band: 대상 밴드.
 *
 * P2L 맵 버퍼는 ftl_mempool로 관리되는 hugepage-backed DMA-safe 영역이다. CLOSED/FREE 상태에서만 호출.
 * df_p2l_map ID도 INVALID로 리셋해 다음 alloc 시 새 객체가 잡히도록 한다.
 *
 * 호출 체인: ftl_band_release_p2l_map → ref_cnt가 0이 되면 본 함수 호출.
 */
static void
ftl_band_free_p2l_map(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;

	/* [한국어] 안전성 검사 — 데이터 쓰기/읽기 중인 밴드의 P2L 맵을 해제하면 안 된다. */
	assert(band->md->state == FTL_BAND_STATE_CLOSED ||
	       band->md->state == FTL_BAND_STATE_FREE);
	/* [한국어] 외부 참조가 모두 사라진 시점에서만 free 가능. */
	assert(p2l_map->ref_cnt == 0);
	/* [한국어] 이미 해제된 상태가 아닌지 확인. */
	assert(p2l_map->band_map != NULL);

	/* [한국어] 디스크 직렬화용 DF(disk-format) ID 리셋 — 다음 alloc 시 새 ID 할당. */
	band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
	/* [한국어] mempool에 객체 반환 — 다른 밴드에서 재사용 가능. */
	ftl_mempool_put(dev->p2l_pool, p2l_map->band_map);
	/* [한국어] 댕글링 포인터 방지. */
	p2l_map->band_map = NULL;
}


/*
 * [한국어]
 * ftl_band_free_md_entry - 밴드 메타데이터의 DMA 사본(band_dma_md)을 풀에 반납.
 *
 * @band: 대상 밴드.
 *
 * band_dma_md는 BAND_MD 영역에 영속화하기 위한 임시 DMA-safe 버퍼. 한 entry씩 풀에서 받아 사용 후 반납.
 */
static void
ftl_band_free_md_entry(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;

	/* [한국어] 사용 중 해제 방지 — 닫혀 있거나 free 상태에서만 허용. */
	assert(band->md->state == FTL_BAND_STATE_CLOSED ||
	       band->md->state == FTL_BAND_STATE_FREE);
	/* [한국어] double-free 방지. */
	assert(p2l_map->band_dma_md != NULL);

	/* [한국어] mempool 반납. */
	ftl_mempool_put(dev->band_md_pool, p2l_map->band_dma_md);
	p2l_map->band_dma_md = NULL;
}

/*
 * [한국어]
 * _ftl_band_set_free - 밴드 상태를 FREE로 천이할 때의 부속 작업.
 *
 * @band: 대상 밴드.
 *
 * (1) free_bands TAILQ에 추가, (2) close_seq_id 리셋, (3) reloc 플래그 클리어, (4) num_free 카운터 증가
 * 후 limit 적용(throttling), (5) p2l_map_checksum 0으로 초기화.
 *
 * 호출 체인: ftl_band_set_state(FREE) → 본 함수.
 */
static void
_ftl_band_set_free(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	/* Add the band to the free band list */
	/* [한국어] free 풀 TAILQ에 등록 — 다음 writer가 이 밴드를 가져갈 수 있게 됨. */
	TAILQ_INSERT_TAIL(&dev->free_bands, band, queue_entry);
	/* [한국어] close 순번 리셋 — free 밴드는 close_seq_id 의미 없음. */
	band->md->close_seq_id = 0;
	/* [한국어] reloc 플래그 해제 — 이전 GC 사이클의 잔재 정리. */
	band->reloc = false;

	/* [한국어] free 밴드 수 증가 후 dev 단위 limit 정책 재평가(예: 사용자 쓰기 throttling 해제). */
	dev->num_free++;
	ftl_apply_limits(dev);

	/* [한국어] free 상태에서는 P2L 맵 체크섬 의미 없음 — 0으로. */
	band->md->p2l_map_checksum = 0;
}

/*
 * [한국어]
 * _ftl_band_set_preparing - 밴드 상태를 PREP(쓰기 준비)로 천이할 때의 부속 작업.
 *
 * (1) free_bands에서 제거(다른 곳에서 못 가져가게), (2) wr_cnt 증가(wear leveling 추적),
 * (3) num_free 감소 + limit 재평가.
 *
 * 호출 체인: ftl_band_set_state(PREP) → 본 함수.
 */
static void
_ftl_band_set_preparing(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	/* Remove band from free list */
	/* [한국어] free 풀에서 빼서 점유 — 다른 사용자가 동시에 가져가지 못하도록. */
	TAILQ_REMOVE(&dev->free_bands, band, queue_entry);

	/* [한국어] write count 증가 — wear leveling 결정(wr_cnt가 적은 밴드를 우선 사용)에 사용. */
	band->md->wr_cnt++;

	/* [한국어] free 카운터 감소(언더플로 방지 검증). */
	assert(dev->num_free > 0);
	dev->num_free--;

	/* [한국어] free 밴드 부족이면 사용자 쓰기 throttling 작동. */
	ftl_apply_limits(dev);
}

/*
 * [한국어]
 * _ftl_band_set_closed_cb - 밴드 close 시 메타데이터 검증 완료 후 콜백.
 *
 * @band:  대상 밴드.
 * @valid: 메타데이터 일관성 검증 결과(반드시 true 가정 — false면 abort).
 *
 * 절차: (1) state를 CLOSED로 표기(free_md가 이를 검사함), (2) owner의 state_change_fn 알림,
 * (3) P2L 체크포인트 검증, (4) ref_cnt 0이면 P2L 맵 release, (5) shut_bands TAILQ에 추가
 * (이후 GC 후보로 검색됨).
 */
static void
_ftl_band_set_closed_cb(struct ftl_band *band, bool valid)
{
	struct spdk_ftl_dev *dev = band->dev;

	/* [한국어] 검증 실패는 데이터 손실을 의미 — assert로 차단. */
	assert(valid == true);

	/* Set the state as free_md() checks for that */
	/* [한국어] 상태를 CLOSED로 마킹 — 이후 release/free 함수들이 상태 검증을 통과할 수 있도록 먼저 설정. */
	band->md->state = FTL_BAND_STATE_CLOSED;
	if (band->owner.state_change_fn) {
		/* [한국어] owner(writer/reloc)에게 상태 변화 통보 — close 후 처리(예: 다음 밴드 획득)를 트리거. */
		band->owner.state_change_fn(band);
	}

	/* [한국어] P2L 체크포인트 검증 — 영속화된 체크포인트와 메모리 상태가 일관되는지 확인. */
	ftl_p2l_validate_ckpt(band);

	/* Free the P2L map if there are no outstanding IOs */
	/* [한국어] P2L 맵 ref_cnt 감소(0이면 풀에 반환). */
	ftl_band_release_p2l_map(band);
	assert(band->p2l_map.ref_cnt == 0);

	/* [한국어] CLOSED 밴드 풀(shut_bands)에 추가 — GC 검색 대상이 됨. */
	TAILQ_INSERT_TAIL(&dev->shut_bands, band, queue_entry);
}

/*
 * [한국어]
 * _ftl_band_set_closed - CLOSING → CLOSED 천이 진입점(검증을 통한 비동기 천이).
 *
 * @band: 대상 밴드.
 *
 * ftl_band_validate_md를 비동기 호출하고 완료 콜백에서 _ftl_band_set_closed_cb로 진입.
 * 검증 도중 실패하면 콜백에 valid=false가 들어와 abort된다.
 */
static void
_ftl_band_set_closed(struct ftl_band *band)
{
	/* Verify that band's metadata is consistent with l2p */
	/* [한국어] L2P와 밴드 메타데이터(P2L)의 일관성 검증을 비동기로 시작. 완료 시 콜백 호출. */
	ftl_band_validate_md(band, _ftl_band_set_closed_cb);
}

/*
 * [한국어]
 * ftl_band_tail_md_addr - 밴드 tail 메타데이터의 base bdev 상 LBA를 계산.
 *
 * @band: 대상 밴드.
 * @return: tail md 시작 LBA = (밴드 시작 LBA) + (tail md offset).
 *
 * tail md는 NVMe write의 xfer_size에 정렬되어 있어야 하며(어셈블/disassemble 호환), assert로 검증.
 */
ftl_addr
ftl_band_tail_md_addr(struct ftl_band *band)
{
	ftl_addr addr;

	/* Metadata should be aligned to xfer size */
	/* [한국어] 정렬 검증 — xfer_size로 나뉘지 않으면 NVMe 쓰기 단위가 맞지 않아 무결성 위험. */
	assert(ftl_band_tail_md_offset(band) % band->dev->xfer_size == 0);

	/* [한국어] 밴드 시작 LBA + 오프셋 = tail md 시작 LBA. */
	addr = ftl_band_tail_md_offset(band) + band->start_addr;

	return addr;
}

/*
 * [한국어]
 * ftl_band_get_state_name - 밴드 상태 enum을 사람이 읽을 수 있는 문자열로 변환.
 *
 * @band: 대상 밴드.
 * @return: "FREE"/"OPEN" 등의 정적 문자열. enum 범위 밖이면 "?".
 *
 * 디버그 로그/RPC 응답에서 상태를 표기할 때 사용.
 */
const char *
ftl_band_get_state_name(struct ftl_band *band)
{
	/* [한국어] enum 인덱스에 대응하는 이름 테이블 — enum 정의와 순서가 일치해야 한다. */
	static const char *names[] = {
		"FREE", "PREPARING", "OPENING", "OPEN", "FULL", "CLOSING",
		"CLOSED",
	};

	/* [한국어] 범위 검사 — out-of-range 인덱스는 메모리 침범. */
	assert(band->md->state < SPDK_COUNTOF(names));
	if (band->md->state < SPDK_COUNTOF(names)) {
		return names[band->md->state];
	} else {
		/* [한국어] 안전 fallback. assert가 release 빌드에선 무시될 수 있어 if도 함께. */
		assert(false);
		return "?";
	}
}

/*
 * [한국어]
 * ftl_band_set_state - 밴드 상태 전이의 단일 진입점(상태 머신 디스패처).
 *
 * @band:  대상 밴드.
 * @state: 목표 상태.
 *
 * 각 천이는 사전 조건(현재 상태)을 assert로 검증한 뒤, 부속 작업을 부속 함수에 위임한다.
 * CLOSED 천이는 비동기(metadata validate)이므로 함수 끝에서 즉시 state를 갱신하지 않고 return.
 *
 * 상태 머신 다이어그램:
 *   CLOSED → FREE         (band_free 후)
 *   FREE → PREP           (writer가 새 밴드 prep)
 *   PREP → OPENING        (open 메타데이터 영속화 시작)
 *   OPENING → OPEN        (open 메타데이터 영속화 완료)
 *   OPEN → FULL           (사용자 데이터 다 쓰임)
 *   FULL → CLOSING        (close 시퀀스 시작, P2L 맵 쓰기)
 *   CLOSING → CLOSED      (P2L 맵 + BAND_MD 영속화 완료)
 */
void
ftl_band_set_state(struct ftl_band *band, enum ftl_band_state state)
{
	switch (state) {
	case FTL_BAND_STATE_FREE:
		/* [한국어] FREE 천이는 CLOSED에서만 허용 — 사이클: CLOSED → FREE. */
		assert(band->md->state == FTL_BAND_STATE_CLOSED);
		_ftl_band_set_free(band);
		break;

	case FTL_BAND_STATE_PREP:
		/* [한국어] PREP 천이는 FREE에서만 허용 — writer가 free pool에서 가져온 직후. */
		assert(band->md->state == FTL_BAND_STATE_FREE);
		_ftl_band_set_preparing(band);
		break;

	case FTL_BAND_STATE_CLOSED:
		if (band->md->state != FTL_BAND_STATE_CLOSED) {
			/* [한국어] CLOSING에서만 CLOSED로 전이 가능. */
			assert(band->md->state == FTL_BAND_STATE_CLOSING);
			_ftl_band_set_closed(band);
			return; /* state can be changed asynchronously */
			/* [한국어] 비동기 검증 후 _ftl_band_set_closed_cb에서 state를 직접 갱신하므로 즉시 반환. */
		}
		break;

	case FTL_BAND_STATE_OPEN:
		/* [한국어] OPEN 시 P2L 맵은 비어 있으므로 체크섬 0으로 초기화. */
		band->md->p2l_map_checksum = 0;
		break;
	case FTL_BAND_STATE_OPENING:
	case FTL_BAND_STATE_FULL:
	case FTL_BAND_STATE_CLOSING:
		/* [한국어] 이 천이들은 추가 부속 작업 없이 단순 state 갱신만 수행. */
		break;
	default:
		FTL_ERRLOG(band->dev, "Unknown band state, %u", state);
		assert(false);
		break;
	}

	/* [한국어] 상태 갱신 — switch 안에서 return하지 않은 모든 경로가 여기로 떨어짐. */
	band->md->state = state;
}

/*
 * [한국어]
 * ftl_band_set_type - 밴드 type 설정(USER 데이터용 / GC 결과용 등).
 *
 * @band: 대상 밴드.
 * @type: FTL_BAND_TYPE_COMPACTION 또는 FTL_BAND_TYPE_GC.
 *
 * type은 통계 분류와 일부 상위 정책(예: GC 우선순위)에서 사용된다.
 */
void
ftl_band_set_type(struct ftl_band *band, enum ftl_band_type type)
{
	switch (type) {
	case FTL_BAND_TYPE_COMPACTION:
	case FTL_BAND_TYPE_GC:
		/* [한국어] 허용된 타입만 그대로 저장. */
		band->md->type = type;
		break;
	default:
		/* [한국어] 알 수 없는 타입 — 코드 버그를 잡기 위한 assert. */
		assert(false);
		break;
	}
}

/*
 * [한국어]
 * ftl_band_set_p2l - 사용자 쓰기 시 P2L 맵에 (LBA, seq_id) 엔트리 기록.
 *
 * @band:   대상 밴드.
 * @lba:    이 물리 위치에 매핑되는 사용자 LBA.
 * @addr:   물리 주소(ftl_addr) — 밴드 시작 + offset 형태.
 * @seq_id: 쓰기 순번(monotonic) — recovery 시 가장 최근 쓰기를 가려내는 데 사용.
 *
 * P2L(Physical-to-Logical) 맵: 밴드 안의 각 물리 블록에 어떤 LBA가 저장되었는지 기록한다.
 * GC 시 valid block을 새 밴드로 옮길 때 어떤 LBA를 다시 쓸지 알아내는 핵심 자료구조.
 * (반대 방향 매핑인 L2P는 lib/ftl/ftl_l2p_*.c에서 관리.)
 */
void
ftl_band_set_p2l(struct ftl_band *band, uint64_t lba, ftl_addr addr, uint64_t seq_id)
{
	struct ftl_p2l_map *p2l_map = &band->p2l_map;
	uint64_t offset;

	/* [한국어] 물리 주소 → 밴드 내 블록 오프셋 변환. */
	offset = ftl_band_block_offset_from_addr(band, addr);

	/* [한국어] 해당 오프셋에 LBA와 seq_id 기록. P2L 맵은 블록당 한 entry. */
	p2l_map->band_map[offset].lba = lba;
	p2l_map->band_map[offset].seq_id = seq_id;
}

/*
 * [한국어]
 * ftl_band_set_addr - 사용자 쓰기 결과를 valid map과 num_valid 카운터에 반영.
 *
 * @band: 대상 밴드.
 * @lba:  쓰인 LBA(여기서는 사용 안 함, 시그니처 호환용).
 * @addr: 물리 주소.
 *
 * 디바이스 단위 valid bitmap에 해당 비트를 set하고 밴드 단위 num_valid를 증가.
 * GC가 invalidity = 1 - num_valid/total을 계산해 reloc 우선순위를 결정할 때 사용.
 */
void
ftl_band_set_addr(struct ftl_band *band, uint64_t lba, ftl_addr addr)
{
	/* [한국어] 밴드 단위 valid 카운터 증가. */
	band->p2l_map.num_valid++;
	/* [한국어] 디바이스 단위 비트맵에 해당 물리 주소를 valid로 마킹. invalidate 시(예: 사용자가 같은 LBA를 덮어씀) 다른 곳에서 clear. */
	ftl_bitmap_set(band->dev->valid_map, addr);
}

/*
 * [한국어]
 * ftl_band_user_blocks_left - 현재 iter 위치 기준으로 더 쓸 수 있는 사용자 블록 수.
 *
 * @band:   대상 밴드.
 * @offset: 현재 iter 오프셋.
 * @return: tail md 시작 전까지 남은 블록 수. 이미 넘었으면 0.
 *
 * writer가 다음 발행에 몇 블록을 넣을지 결정할 때 사용.
 */
size_t
ftl_band_user_blocks_left(const struct ftl_band *band, size_t offset)
{
	/* [한국어] tail md 시작 오프셋 = 사용자 영역의 끝 + 1. */
	size_t tail_md_offset = ftl_band_tail_md_offset(band);

	if (spdk_unlikely(offset > tail_md_offset)) {
		/* [한국어] 비정상 상황(이미 tail md를 침범) — 안전 fallback으로 0 반환. */
		return 0;
	}

	/* [한국어] 남은 사용자 블록 수. */
	return tail_md_offset - offset;
}

/*
 * [한국어]
 * ftl_band_user_blocks - 한 밴드의 총 사용자 블록 수(상수, iter 위치 무관).
 *
 * @return: 밴드 총 블록 수 - tail md 블록 수.
 */
size_t
ftl_band_user_blocks(const struct ftl_band *band)
{
	/* [한국어] 밴드 전체 - tail md = user 영역 크기. */
	return ftl_get_num_blocks_in_band(band->dev) -
	       ftl_tail_md_num_blocks(band->dev);
}

/*
 * [한국어]
 * ftl_addr_get_band - 물리 주소에서 어느 밴드 ID에 속하는지 계산.
 *
 * @dev:  디바이스.
 * @addr: 물리 주소.
 * @return: 밴드 ID.
 *
 * 모든 밴드의 크기는 동일하다는 가정 하에 단순 나눗셈으로 산출.
 * dev->bands[0]의 시작 LBA를 기준으로 (addr - 시작) / 밴드 크기.
 */
static inline uint64_t
ftl_addr_get_band(const struct spdk_ftl_dev *dev, ftl_addr addr)
{
	/* [한국어] 첫 밴드 시작점 기준 상대 오프셋을 밴드 크기로 나눈 몫이 밴드 ID. */
	return (addr - dev->bands->start_addr) / ftl_get_num_blocks_in_band(dev);
}

/*
 * [한국어]
 * ftl_band_from_addr - 물리 주소를 보고 해당 밴드 객체 포인터 반환.
 *
 * @dev/@addr: 디바이스 + 물리 주소.
 * @return: 그 주소가 속한 ftl_band* (NULL 없음 — assert로 보장).
 */
struct ftl_band *
ftl_band_from_addr(struct spdk_ftl_dev *dev, ftl_addr addr)
{
	uint64_t band_id = ftl_addr_get_band(dev, addr);

	/* [한국어] band_id가 범위 내인지 검증 — out-of-range는 메모리 침범. */
	assert(band_id < ftl_get_num_bands(dev));
	return &dev->bands[band_id];
}

/*
 * [한국어]
 * ftl_band_block_offset_from_addr - 물리 주소를 밴드 내 블록 오프셋으로 변환.
 *
 * 동일 밴드 내 주소임을 assert로 확인.
 */
uint64_t
ftl_band_block_offset_from_addr(struct ftl_band *band, ftl_addr addr)
{
	/* [한국어] 입력 주소가 인자 band에 속하는지 검증. */
	assert(ftl_addr_get_band(band->dev, addr) == band->id);
	/* [한국어] 단순 차이 — 밴드 시작 LBA에서의 거리. */
	return addr - band->start_addr;
}

/*
 * [한국어]
 * ftl_band_next_xfer_addr - 현재 주소에서 num_blocks 만큼 진행한 다음 주소를 xfer_size 정렬 고려해 계산.
 *
 * @band:       대상 밴드.
 * @addr:       시작 주소.
 * @num_blocks: 진행할 블록 수.
 * @return:     새 주소(밴드를 넘어가면 FTL_ADDR_INVALID).
 *
 * NVMe 쓰기는 xfer_size(예: 16 블록)로 정렬되어야 효율이 좋고, 또한 P2L 맵 검증 시 xfer 단위로
 * 처리되므로 정렬 보장이 필요하다. 시작 주소가 정렬되지 않은 경우 정렬 단위 정수배만큼 진행 후
 * 잔여 블록은 끝에 다시 더한다(원래 비정렬을 보존).
 */
ftl_addr
ftl_band_next_xfer_addr(struct ftl_band *band, ftl_addr addr, size_t num_blocks)
{
	struct spdk_ftl_dev *dev = band->dev;
	size_t num_xfers;
	uint64_t offset;

	/* [한국어] 입력 주소가 같은 밴드 내인지 검증. */
	assert(ftl_addr_get_band(dev, addr) == band->id);

	/* [한국어] 밴드 내 시작 오프셋. */
	offset = addr - band->start_addr;

	/* In case starting address wasn't aligned to xfer_size, we'll align for consistent calculation
	 * purposes - the unaligned value will be preserved at the end however.
	 */
	/* [한국어] 비정렬분을 num_blocks에 흡수하고 offset은 정렬값으로 내림 — 정렬 기준 계산을 위함. */
	num_blocks += (offset % dev->xfer_size);
	offset -= (offset % dev->xfer_size);

	/* Calculate offset based on xfer_size aligned writes */
	/* [한국어] 정수 배수 부분만큼 offset 증가. */
	num_xfers = (num_blocks / dev->xfer_size);
	offset += num_xfers * dev->xfer_size;
	num_blocks -= num_xfers * dev->xfer_size;

	if (offset > ftl_get_num_blocks_in_band(dev)) {
		/* [한국어] 밴드 범위를 넘어감 — 잘못된 진행, 호출자에게 INVALID 반환. */
		return FTL_ADDR_INVALID;
	}

	/* If there's any unalignment (either starting addr value or num_blocks), reintroduce it to the final address
	 */
	if (num_blocks) {
		/* [한국어] 잔여 비정렬 블록을 마지막에 더해 원래 값 보존. */
		offset += num_blocks;
		if (offset > ftl_get_num_blocks_in_band(dev)) {
			return FTL_ADDR_INVALID;
		}
	}

	/* [한국어] 최종 주소 = 밴드 시작 LBA + 누적 오프셋. */
	addr = band->start_addr + offset;
	return addr;
}

/*
 * [한국어]
 * ftl_band_addr_from_block_offset - 밴드 + 오프셋 → 절대 ftl_addr 변환.
 */
ftl_addr
ftl_band_addr_from_block_offset(struct ftl_band *band, uint64_t block_off)
{
	ftl_addr addr;

	/* [한국어] 밴드 시작 LBA + 오프셋. */
	addr = block_off + band->start_addr;
	return addr;
}

/*
 * [한국어]
 * ftl_band_next_addr - 현재 주소에서 offset 블록 후의 주소(xfer 정렬 신경 안 씀).
 *
 * 단순 +offset이 아니라 우선 밴드 내 오프셋을 재계산하는 이유는 caller가 다른 밴드 주소를 넘긴 경우의
 * 안전성(assert를 통한 검증)을 보장하기 위함.
 */
ftl_addr
ftl_band_next_addr(struct ftl_band *band, ftl_addr addr, size_t offset)
{
	/* [한국어] 입력 주소를 오프셋으로 변환(밴드 ID 검증 동반). */
	uint64_t block_off = ftl_band_block_offset_from_addr(band, addr);

	/* [한국어] 오프셋 +=, 다시 ftl_addr로 변환. */
	return ftl_band_addr_from_block_offset(band, block_off + offset);
}

/*
 * [한국어]
 * ftl_band_acquire_p2l_map - P2L 맵 사용을 위한 ref_cnt 증가.
 *
 * 동일 밴드의 P2L 맵을 여러 곳(writer, reloc 등)이 동시 참조할 수 있어 ref-counting으로 관리.
 * 0 → free 트리거는 release 함수에서 수행.
 */
void
ftl_band_acquire_p2l_map(struct ftl_band *band)
{
	/* [한국어] 이미 alloc된 상태에서만 acquire 가능. */
	assert(band->p2l_map.band_map != NULL);
	band->p2l_map.ref_cnt++;
}

/*
 * [한국어]
 * ftl_band_alloc_md_entry - band_dma_md(메타데이터 DMA 사본) 메모리 풀에서 alloc.
 *
 * @return: 0 성공, -1 실패(풀 고갈).
 *
 * BAND_MD 영역 영속화 시 NVMe DMA의 source 버퍼로 쓰일 임시 사본을 얻고, 0으로 초기화.
 */
static int
ftl_band_alloc_md_entry(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;
	/* [한국어] BAND_MD region에서 entry 크기 정보 획득(블록 단위). */
	struct ftl_layout_region *region = ftl_layout_region_get(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD);

	/* [한국어] 풀에서 한 entry alloc — DMA-safe hugepage 영역. */
	p2l_map->band_dma_md = ftl_mempool_get(dev->band_md_pool);

	if (!p2l_map->band_dma_md) {
		/* [한국어] 풀 고갈 — 호출자가 retry 또는 ENOMEM 처리. */
		return -1;
	}

	/* [한국어] 0으로 초기화 — 디스크에 기록될 미정 필드의 비결정성 제거. region->entry_size는 블록 단위. */
	memset(p2l_map->band_dma_md, 0, region->entry_size * FTL_BLOCK_SIZE);
	return 0;
}

/*
 * [한국어]
 * ftl_band_alloc_p2l_map - P2L 맵과 그 메타데이터 entry를 풀에서 alloc.
 *
 * @return: 0 성공, -1 실패.
 *
 * 절차: (1) p2l_pool에서 P2L 맵 버퍼 획득, (2) band_dma_md alloc, (3) df_p2l_map ID 발급(disk 직렬화용),
 * (4) P2L 맵을 FTL_LBA_INVALID(0xFF...)로 초기화, (5) ref_cnt 1 acquire.
 *
 * P2L 맵을 -1(0xFF)로 초기화하는 이유: 미기록 슬롯이 valid LBA로 오해되지 않도록.
 */
int
ftl_band_alloc_p2l_map(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;

	/* [한국어] 사전 조건 검증 — 이미 alloc된 상태에서 다시 alloc하면 메모리 누수. */
	assert(p2l_map->ref_cnt == 0);
	assert(p2l_map->band_map == NULL);

	/* [한국어] 디스크 형식 ID는 INVALID여야 함(이전에 free되어 있어야). */
	assert(band->md->df_p2l_map == FTL_DF_OBJ_ID_INVALID);
	/* [한국어] P2L 맵 버퍼 획득. */
	p2l_map->band_map = ftl_mempool_get(dev->p2l_pool);
	if (!p2l_map->band_map) {
		return -1;
	}

	/* [한국어] band_dma_md(메타 영속화 DMA 사본) alloc. 실패 시 P2L도 되돌림. */
	if (ftl_band_alloc_md_entry(band)) {
		ftl_band_free_p2l_map(band);
		return -1;
	}

	/* [한국어] 디스크 형식 ID 발급 — BAND_MD에 저장되어 다음 부팅 시 mempool 객체 재바인딩에 사용. */
	band->md->df_p2l_map = ftl_mempool_get_df_obj_id(dev->p2l_pool, p2l_map->band_map);

	/* Set the P2L to FTL_LBA_INVALID */
	/* [한국어] 모든 슬롯을 -1(0xFF...)로 채워 INVALID 표시 — 미기록 슬롯이 valid로 오해되지 않게. */
	memset(p2l_map->band_map, -1, FTL_BLOCK_SIZE * ftl_p2l_map_num_blocks(band->dev));

	/* [한국어] ref_cnt 1로 시작. */
	ftl_band_acquire_p2l_map(band);
	return 0;
}

/*
 * [한국어]
 * ftl_band_open_p2l_map - 이미 영속화된 df_p2l_map ID로부터 P2L 맵을 풀에서 재바인딩.
 *
 * @return: 0 성공, -1 실패.
 *
 * 부팅 시 BAND_MD를 읽어 df_p2l_map ID가 valid한 밴드를 발견하면, 풀에 등록된 동일 객체를 다시 claim하여
 * 메모리에 매핑. memset으로 초기화하지 않음(디스크에서 읽어올 데이터가 들어 있어야 하므로).
 */
int
ftl_band_open_p2l_map(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;

	/* [한국어] 사전 조건 — 메모리에 아직 매핑되지 않아야. */
	assert(p2l_map->ref_cnt == 0);
	assert(p2l_map->band_map == NULL);

	/* [한국어] 영속화된 df_p2l_map ID가 있어야 — 이전에 alloc된 흔적. */
	assert(band->md->df_p2l_map != FTL_DF_OBJ_ID_INVALID);

	/* [한국어] DMA 사본 alloc. */
	if (ftl_band_alloc_md_entry(band)) {
		p2l_map->band_map = NULL;
		return -1;
	}

	/* [한국어] DF ID로 풀 객체 claim — 같은 메모리 슬롯이 재할당된다(부팅 시 mempool 레이아웃이 보존됨). */
	p2l_map->band_map = ftl_mempool_claim_df(dev->p2l_pool, band->md->df_p2l_map);

	/* [한국어] ref_cnt 1로 시작. */
	ftl_band_acquire_p2l_map(band);
	return 0;
}

/*
 * [한국어]
 * ftl_band_release_p2l_map - P2L 맵 ref_cnt 감소, 0이 되면 풀에 반환.
 *
 * @band: 대상 밴드.
 *
 * 부수 효과: ref_cnt가 0에 도달하면 (1) p2l_ckpt 영역 release(있을 시),
 * (2) p2l_map mempool put, (3) band_dma_md mempool put을 수행.
 */
void
ftl_band_release_p2l_map(struct ftl_band *band)
{
	struct ftl_p2l_map *p2l_map = &band->p2l_map;

	/* [한국어] 이미 free된 P2L 맵은 release 불가. */
	assert(p2l_map->band_map != NULL);
	assert(p2l_map->ref_cnt > 0);
	p2l_map->ref_cnt--;

	if (p2l_map->ref_cnt == 0) {
		if (p2l_map->p2l_ckpt) {
			/* [한국어] P2L 체크포인트 영역(NV cache 안의 영역)을 풀에 반환. */
			ftl_p2l_ckpt_release(band->dev, p2l_map->p2l_ckpt);
			p2l_map->p2l_ckpt = NULL;
		}
		/* [한국어] P2L 맵 버퍼와 메타데이터 사본 모두 반환. */
		ftl_band_free_p2l_map(band);
		ftl_band_free_md_entry(band);
	}
}

/*
 * [한국어]
 * ftl_band_p2l_map_addr - 밴드 내 P2L 맵의 시작 LBA 반환(=tail_md_addr).
 *
 * P2L 맵은 tail md 영역에 함께 저장되므로 tail_md_addr가 곧 P2L 맵 시작 위치.
 */
ftl_addr
ftl_band_p2l_map_addr(struct ftl_band *band)
{
	return band->tail_md_addr;
}

/*
 * [한국어]
 * ftl_band_write_prep - 밴드를 사용자 쓰기 직전에 준비.
 *
 * @band: 대상 밴드(상태 PREP인 밴드).
 * @return: 0 성공, -1 실패.
 *
 * 절차:
 *  1) P2L 맵 alloc (실패 시 -1)
 *  2) P2L 체크포인트 영역(NV cache 안) acquire — 사용자 쓰기 P2L 영속화 대상
 *  3) iter 초기화(밴드 시작점부터 append 시작)
 *  4) seq_id 발급(이 밴드의 첫 데이터에 부여될 순번)
 */
int
ftl_band_write_prep(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	/* [한국어] P2L 맵 메모리 alloc — 사용자 쓰기와 함께 (LBA, seq_id) 기록 준비. */
	if (ftl_band_alloc_p2l_map(band)) {
		return -1;
	}

	/* [한국어] P2L 체크포인트 슬롯 획득 — 디스크에 점진적으로 P2L을 영속화하기 위한 NV cache 영역. */
	band->p2l_map.p2l_ckpt = ftl_p2l_ckpt_acquire(dev);
	/* [한국어] 영속화될 region type을 메타데이터에 기록 — recovery 시 이 ID로 체크포인트 영역을 찾아 복구. */
	band->md->p2l_md_region = ftl_p2l_ckpt_region_type(band->p2l_map.p2l_ckpt);
	/* [한국어] iter 초기화 — 밴드 시작점에서 append 시작. */
	ftl_band_iter_init(band);

	/* [한국어] 이 밴드 데이터의 시작 seq_id 발급. dev 내 monotonic counter. */
	band->md->seq = ftl_get_next_seq_id(dev);

	FTL_DEBUGLOG(dev, "Band to write, id %u seq %"PRIu64"\n", band->id, band->md->seq);
	return 0;
}

/*
 * [한국어]
 * ftl_p2l_map_pool_elem_size - P2L 맵 mempool 요소 하나의 크기(바이트).
 *
 * @return: tail md 블록 수 * 블록 크기 — 한 밴드의 P2L 맵 전체 + 헤더가 들어가는 크기.
 *
 * mempool 초기화 시 element 크기를 결정하는 데 사용.
 */
size_t
ftl_p2l_map_pool_elem_size(struct spdk_ftl_dev *dev)
{
	/* Map pool element holds the whole tail md */
	/* [한국어] 한 밴드의 tail md 전체를 담을 수 있는 크기. */
	return ftl_tail_md_num_blocks(dev) * FTL_BLOCK_SIZE;
}

/*
 * [한국어]
 * ftl_band_invalidity - 밴드 invalidity 비율 (0.0 ~ 1.0).
 *
 * invalidity = 1 - valid/total. 1에 가까울수록 회수 효율이 좋음(GC 우선순위 높음).
 */
double
ftl_band_invalidity(struct ftl_band *band)
{
	/* [한국어] 현재 valid한 사용자 블록 수. */
	double valid = band->p2l_map.num_valid;
	/* [한국어] 사용자 영역 총 블록 수(tail md 제외). */
	double count = ftl_band_user_blocks(band);

	/* [한국어] 1 - 유효율 = invalidity. */
	return 1.0 - (valid / count);
}

/*
 * [한국어]
 * dump_bands_under_relocation - 디버그 로그: 현재 GC 사이클에 포함된 모든 밴드 정보 출력.
 *
 * @dev: 디바이스.
 *
 * GC는 한 번에 "phys group" 단위(num_logical_bands_in_physical 개)로 진행되며, 본 함수는 그 그룹에
 * 속한 밴드들의 phys_id/wr_cnt/invalidity를 출력한다. 진단/회수 효율 분석용.
 */
static void
dump_bands_under_relocation(struct spdk_ftl_dev *dev)
{
	/* [한국어] 현재 GC 사이클의 시작 밴드 ID. shm은 dirty-shutdown 후에도 유지되는 공유 메모리. */
	uint64_t i = dev->sb_shm->gc_info.current_band_id;
	/* [한국어] 그룹 끝 — current + group size. */
	uint64_t end = dev->sb_shm->gc_info.current_band_id + dev->num_logical_bands_in_physical;

	for (; i < end; i++) {
		struct ftl_band *band = &dev->bands[i];

		/* [한국어] 그룹 내 각 밴드의 wr_cnt와 invalidity(%)를 로그. */
		FTL_DEBUGLOG(dev, "Band, id %u, phys_is %u, wr cnt = %u, invalidity = %u%%\n",
			     band->id, band->phys_id, (uint32_t)band->md->wr_cnt,
			     (uint32_t)(ftl_band_invalidity(band) * 100));
	}
}

/*
 * [한국어]
 * is_band_relocateable - 밴드가 reloc 후보가 될 수 있는지 검사.
 *
 * @return: true이면 reloc 가능, false면 제외.
 *
 * 조건: (1) 상태가 CLOSED, (2) reloc 플래그가 false(이미 진행 중인 GC 대상이 아님).
 */
static bool
is_band_relocateable(struct ftl_band *band)
{
	/* Can only move data from closed bands */
	if (FTL_BAND_STATE_CLOSED != band->md->state) {
		/* [한국어] 닫힌 밴드가 아니면 reloc 불가 — open/full/closing 상태는 데이터가 변할 수 있음. */
		return false;
	}

	/* Band is already under relocation, skip it */
	if (band->reloc) {
		/* [한국어] 이미 다른 reloc 작업이 점유 중. */
		return false;
	}

	return true;
}

/*
 * [한국어]
 * get_band_phys_info - 동일 phys_id를 공유하는 밴드 그룹의 평균 invalidity와 wr_cnt 계산.
 *
 * @dev:        디바이스.
 * @phys_id:    물리 그룹 ID.
 * @invalidity: 출력. 그룹 평균 invalidity.
 * @wr_cnt:     출력. 그룹 평균 wr_cnt.
 *
 * physical group 단위로 GC를 결정하는 휴리스틱을 위해, 그룹 내 모든 reloc 가능한 밴드의 invalidity를
 * 합산 후 그룹 크기로 나눈다. wr_cnt는 모든 밴드(reloc 불가능 포함)의 평균.
 */
static void
get_band_phys_info(struct spdk_ftl_dev *dev, uint64_t phys_id,
		   double *invalidity, double *wr_cnt)
{
	struct ftl_band *band;
	/* [한국어] 그룹 시작 밴드 ID. */
	uint64_t band_id = phys_id * dev->num_logical_bands_in_physical;

	/* [한국어] 출력값 초기화. */
	*wr_cnt = *invalidity = 0.0L;
	for (; band_id < ftl_get_num_bands(dev); band_id++) {
		band = &dev->bands[band_id];

		if (phys_id != band->phys_id) {
			/* [한국어] 그룹 끝 도달(다음 그룹 시작) — 루프 종료. */
			break;
		}

		/* [한국어] wr_cnt는 모든 밴드 합산(wear leveling 평균). */
		*wr_cnt += band->md->wr_cnt;

		if (!is_band_relocateable(band)) {
			/* [한국어] reloc 불가 밴드는 invalidity 합산에서 제외(회수 가능한 밴드만 평가). */
			continue;
		}

		*invalidity += ftl_band_invalidity(band);
	}

	/* [한국어] 그룹 크기로 나눠 평균. */
	*invalidity /= dev->num_logical_bands_in_physical;
	*wr_cnt /= dev->num_logical_bands_in_physical;
}

/*
 * [한국어]
 * band_cmp - 두 밴드(혹은 그룹) 비교 함수: a가 b보다 GC 우선이면 true.
 *
 * 비교 우선순위:
 *  1) invalidity 차이가 0.1(10%p) 이상이면 invalidity 큰 쪽 우선.
 *  2) invalidity 비슷하면 wr_cnt 작은 쪽 우선(wear leveling — 덜 쓴 밴드를 선택).
 *  3) wr_cnt도 같으면 phys_id 작은 쪽 우선(deterministic tiebreaker).
 */
static bool
band_cmp(double a_invalidity, double a_wr_cnt,
	 double b_invalidity, double b_wr_cnt,
	 uint64_t a_id, uint64_t b_id)
{
	/* [한국어] 유효한 phys_id 인지 검증. */
	assert(a_id != FTL_BAND_PHYS_ID_INVALID);
	assert(b_id != FTL_BAND_PHYS_ID_INVALID);
	/* [한국어] invalidity 차의 절댓값 계산. */
	double diff = a_invalidity - b_invalidity;
	if (diff < 0.0L) {
		diff *= -1.0L;
	}

	/* Use the following metrics for picking bands for GC (in order):
	 * - relative invalidity
	 * - if invalidity is similar (within 10% points), then their write counts (how many times band was written to)
	 * - if write count is equal, then pick based on their placement on base device (lower LBAs win)
	 */
	if (diff > 0.1L) {
		/* [한국어] invalidity 차이가 의미 있게 크면 invalidity 큰 쪽 우선. */
		return a_invalidity > b_invalidity;
	}

	if (a_wr_cnt != b_wr_cnt) {
		/* [한국어] invalidity 비슷 — wear leveling을 위해 wr_cnt 적은 쪽 우선(덜 쓴 밴드). */
		return a_wr_cnt < b_wr_cnt;
	}

	/* [한국어] 두 메트릭 모두 동률 — phys_id 작은 쪽 (결정론적 tiebreaker). */
	return a_id < b_id;
}

/*
 * [한국어]
 * band_start_gc - 밴드를 reloc 진행 중 상태로 표시.
 *
 * shut_bands TAILQ에서 빼고 reloc 플래그를 set. 이후 GC가 끝날 때까지 다른 사이클이 이 밴드를 다시
 * 선택하지 않도록 한다.
 */
static void
band_start_gc(struct spdk_ftl_dev *dev, struct ftl_band *band)
{
	/* [한국어] reloc 가능 여부 강검증 — 통과하지 못하면 ftl_bug로 abort. */
	ftl_bug(false == is_band_relocateable(band));

	/* [한국어] CLOSED 풀에서 제거 — 이중 선택 방지. */
	TAILQ_REMOVE(&dev->shut_bands, band, queue_entry);
	band->reloc = true;

	FTL_DEBUGLOG(dev, "Band to GC, id %u\n", band->id);
}

/*
 * [한국어]
 * gc_high_priority_band - 높은 우선순위로 등록된 밴드가 있으면 즉시 GC 대상으로 반환.
 *
 * @return: 우선순위 밴드(있으면), 없으면 0.
 *
 * 외부에서 특정 밴드를 강제 GC하고 싶을 때 sb_shm->gc_info.band_id_high_prio에 ID를 등록한다.
 * 본 함수는 그것을 소모(INVALID로 리셋)하면서 밴드를 반환.
 */
static struct ftl_band *
gc_high_priority_band(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	/* [한국어] 공유 메모리에 등록된 우선순위 ID 읽기. */
	uint64_t high_prio_id = dev->sb_shm->gc_info.band_id_high_prio;

	if (FTL_BAND_ID_INVALID != high_prio_id) {
		/* [한국어] 범위 검증. */
		ftl_bug(high_prio_id >= dev->num_bands);

		band = &dev->bands[high_prio_id];
		/* [한국어] ID 소모 — 같은 밴드를 두 번 선택하지 않도록. */
		dev->sb_shm->gc_info.band_id_high_prio = FTL_BAND_ID_INVALID;

		/* [한국어] 밴드 reloc 시작 표시. */
		band_start_gc(dev, band);
		FTL_NOTICELOG(dev, "GC takes high priority band, id %u\n", band->id);
		return band;
	}

	return 0;
}

/*
 * [한국어]
 * ftl_band_reset_gc_iter - GC iterator 상태를 INVALID로 리셋.
 *
 * 디바이스 sb(super block)와 sb_shm(공유 메모리) 양쪽에 동일하게 기록.
 * dirty shutdown 후 또는 새 GC 사이클 시작 시 호출.
 */
static void
ftl_band_reset_gc_iter(struct spdk_ftl_dev *dev)
{
	/* [한국어] is_valid=0 — 이 sb의 gc_info는 무효. */
	dev->sb->gc_info.is_valid = 0;
	dev->sb->gc_info.current_band_id = FTL_BAND_ID_INVALID;
	dev->sb->gc_info.band_id_high_prio = FTL_BAND_ID_INVALID;
	dev->sb->gc_info.band_phys_id = FTL_BAND_PHYS_ID_INVALID;

	/* [한국어] 공유 메모리에도 동일 값 복제 — 다른 컴포넌트(예: ftl_mngt)가 같은 상태 보도록. */
	dev->sb_shm->gc_info = dev->sb->gc_info;
}

/*
 * [한국어]
 * ftl_band_search_next_to_reloc - 다음 GC reloc 대상 밴드를 찾는다.
 *
 * @dev: 디바이스.
 * @return: 선택된 밴드(없으면 NULL).
 *
 * 알고리즘:
 *  (a) 우선순위 밴드가 있으면 즉시 반환.
 *  (b) 현재 진행 중인 phys group(sb_shm->gc_info.band_phys_id)에서 reloc 가능한 밴드를 순차 검색.
 *  (c) 진행 중 그룹이 없으면 모든 phys group을 훑어 invalidity가 가장 높은 그룹을 선정,
 *      band_phys_id/current_band_id를 그 그룹으로 설정한 뒤 자기 자신을 재귀 호출하여 그룹 내 첫 밴드를 반환.
 *  (d) 모든 그룹의 invalidity가 0이면 GC iterator 리셋 후 NULL 반환.
 */
struct ftl_band *
ftl_band_search_next_to_reloc(struct spdk_ftl_dev *dev)
{
	double invalidity, max_invalidity = 0.0L;
	double wr_cnt, max_wr_cnt = 0.0L;
	uint64_t phys_id = FTL_BAND_PHYS_ID_INVALID;
	struct ftl_band *band;
	uint64_t i, band_count;
	uint64_t phys_count;

	/* [한국어] (a) 우선순위 밴드 — 외부 트리거가 있으면 즉시 반환. */
	band = gc_high_priority_band(dev);
	if (spdk_unlikely(NULL != band)) {
		return band;
	}

	/* [한국어] phys group 크기와 전체 밴드 수 캐시. */
	phys_count = dev->num_logical_bands_in_physical;
	band_count = ftl_get_num_bands(dev);

	/* [한국어] (b) 현재 진행 중인 phys group 안에서 reloc 가능한 밴드 검색.
	 *         iter는 sb_shm에 보관되어 dirty-shutdown 후에도 이어가기 가능. */
	for (; dev->sb_shm->gc_info.current_band_id < band_count;) {
		band = &dev->bands[dev->sb_shm->gc_info.current_band_id];
		if (band->phys_id != dev->sb_shm->gc_info.band_phys_id) {
			/* [한국어] 그룹을 벗어남 — 새 그룹 선정으로 이동. */
			break;
		}

		if (false == is_band_relocateable(band)) {
			/* [한국어] reloc 불가 — 다음 밴드. */
			dev->sb_shm->gc_info.current_band_id++;
			continue;
		}

		/* [한국어] 후보 발견 — reloc 시작 표시 후 반환. */
		band_start_gc(dev, band);
		return band;
	}

	/* [한국어] (c) 진행 중 그룹이 없거나 다 처리 — 모든 phys group을 훑어 가장 invalidity 높은 그룹 선정. */
	for (i = 0; i < band_count; i += phys_count) {
		band = &dev->bands[i];

		/* Calculate entire band physical group invalidity */
		/* [한국어] 그룹 단위 평균 invalidity와 wr_cnt 계산. */
		get_band_phys_info(dev, band->phys_id, &invalidity, &wr_cnt);

		if (invalidity != 0.0L) {
			/* [한국어] 첫 후보이거나 band_cmp가 더 우선이라고 판정하면 max를 갱신. */
			if (phys_id == FTL_BAND_PHYS_ID_INVALID ||
			    band_cmp(invalidity, wr_cnt, max_invalidity, max_wr_cnt,
				     band->phys_id, phys_id)) {
				max_wr_cnt = wr_cnt;
				phys_id = band->phys_id;

				if (invalidity > max_invalidity) {
					max_invalidity = invalidity;
				}
			}
		}
	}

	if (FTL_BAND_PHYS_ID_INVALID != phys_id) {
		FTL_DEBUGLOG(dev, "Band physical id %"PRIu64" to GC\n", phys_id);
		/* [한국어] 새 그룹 선정 — sb_shm 갱신 후 자기 자신을 재귀 호출하여 (b) 분기로 진입,
		 *         그룹 내 첫 reloc 가능 밴드를 반환. is_valid 토글로 atomic-like 갱신 효과. */
		dev->sb_shm->gc_info.is_valid = 0;
		dev->sb_shm->gc_info.current_band_id = phys_id * phys_count;
		dev->sb_shm->gc_info.band_phys_id = phys_id;
		dev->sb_shm->gc_info.is_valid = 1;
		dump_bands_under_relocation(dev);
		return ftl_band_search_next_to_reloc(dev);
	} else {
		/* [한국어] (d) 모든 그룹 invalidity 0 — GC할 게 없음. iter 초기화 후 NULL 반환. */
		ftl_band_reset_gc_iter(dev);
	}

	return NULL;
}

/*
 * [한국어]
 * ftl_band_init_gc_iter - 부팅 시 GC iterator 상태 초기화.
 *
 * @dev: 디바이스.
 *
 * 시나리오:
 *  - SPDK_FTL_MODE_CREATE(처음 생성): 무조건 리셋.
 *  - clean shutdown: sb의 gc_info를 sb_shm에 복사(이전 상태 이어감).
 *  - fast startup/recovery: 별도 처리(이미 다른 곳에서 복원됨).
 *  - dirty shutdown: GC 상태가 일관되지 않을 수 있어 리셋.
 */
void
ftl_band_init_gc_iter(struct spdk_ftl_dev *dev)
{
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		/* [한국어] 신규 생성 — GC iter 처음부터. */
		ftl_band_reset_gc_iter(dev);
		return;
	}

	if (dev->sb->clean) {
		/* [한국어] clean shutdown — 이전에 저장된 gc_info를 그대로 이어감. */
		dev->sb_shm->gc_info = dev->sb->gc_info;
		return;
	}

	if (ftl_fast_startup(dev) || ftl_fast_recovery(dev)) {
		/* [한국어] fast 경로 — 별도 복원 흐름이 처리하므로 여기서는 손대지 않음. */
		return;
	}

	/* We lost GC state due to dirty shutdown, reset GC state to start over */
	/* [한국어] dirty shutdown 후 일반 복구 — GC 상태 신뢰 불가, 안전을 위해 리셋. */
	ftl_band_reset_gc_iter(dev);
}

/*
 * [한국어]
 * ftl_valid_map_load_state - 부팅 시 각 밴드의 num_valid를 valid bitmap으로부터 재계산.
 *
 * @dev: 디바이스.
 *
 * valid_map 비트의 set count를 세어 num_valid에 채움 — invalidity 계산의 기초가 된다.
 */
void
ftl_valid_map_load_state(struct spdk_ftl_dev *dev)
{
	uint64_t i;
	struct ftl_band *band;

	for (i = 0; i < dev->num_bands; i++) {
		band = &dev->bands[i];
		/* [한국어] 밴드 단위 비트맵의 1 비트 수를 카운트해 num_valid 복원. */
		band->p2l_map.num_valid = ftl_bitmap_count_set(band->p2l_map.valid);
	}
}

/*
 * [한국어]
 * ftl_band_initialize_free_state - startup 시 FREE 상태 밴드를 free_bands TAILQ로 옮김.
 *
 * 부팅 직후 모든 밴드는 일단 shut_bands에 들어가 있으므로, FREE 상태로 표시된 밴드만 골라
 * shut_bands에서 빼고 _ftl_band_set_free로 처리한다.
 */
void
ftl_band_initialize_free_state(struct ftl_band *band)
{
	/* All bands start on the shut list during startup, removing it manually here */
	/* [한국어] startup 단계에서는 전부 shut_bands에 있음 — 수동으로 제거. */
	TAILQ_REMOVE(&band->dev->shut_bands, band, queue_entry);
	/* [한국어] FREE 부속 작업(free 풀 추가, num_free++ 등)을 수행. */
	_ftl_band_set_free(band);
}

/*
 * [한국어]
 * ftl_bands_load_state - 부팅 시 모든 밴드의 메타데이터를 검증하고 FREE 상태인 밴드를 free 풀로 등록.
 *
 * @dev:    디바이스.
 * @return: 0 성공, -1 버전 불일치(영속화 포맷 mismatch — 마이그레이션 필요).
 *
 * 각 밴드의 md->version이 현재 코드의 FTL_BAND_VERSION_CURRENT와 일치해야 부팅 진행 가능.
 */
int
ftl_bands_load_state(struct spdk_ftl_dev *dev)
{
	uint64_t i;
	struct ftl_band *band;

	for (i = 0; i < dev->num_bands; i++) {
		band = &dev->bands[i];

		if (band->md->version != FTL_BAND_VERSION_CURRENT) {
			/* [한국어] 디스크에 저장된 메타데이터 버전이 현재 코드와 다름 — 안전 종료. */
			FTL_ERRLOG(dev, "Invalid band version detected, %"PRIu64" (expected %d)\n",
				   band->md->version, FTL_BAND_VERSION_CURRENT);
			return -1;
		}

		if (band->md->state == FTL_BAND_STATE_FREE) {
			/* [한국어] FREE 상태 밴드를 free_bands 풀에 등록. */
			ftl_band_initialize_free_state(band);
		}
	}

	return 0;
}
