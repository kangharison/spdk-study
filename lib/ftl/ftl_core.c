/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL I/O 디스패처 코어 (ftl_core.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK FTL(Flash Translation Layer)의 "심장"이다. 사용자 IO 진입점(spdk_ftl_writev/readv/unmap),
 * IO 큐 처리(rd_sq/wr_sq/trim_sq), 코어 폴러(ftl_core_poller — writer/reloc/nv_cache/l2p의 타임슬라이스 관리),
 * 단일 IO의 read 경로(L2P pin → 주소 룩업 → bdev read 발행 → 완료 처리), TRIM 처리(메타데이터/로그 영속화
 * 시퀀스), 통계 갱신/조회, 셧다운 절차, 전역 zero/read 버퍼 init/fini를 담당한다. 즉 FTL의 "사용자 인터페이스
 * + 메인 루프 + 공통 유틸"이 모인 파일이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: bdev_ftl 모듈(SPDK bdev 백엔드) → spdk_ftl_writev/readv/unmap → queue_io(SPDK ring 통해 dev
 *  스레드로 전달) → ftl_core_poller(매 polling 사이클) → ftl_process_io_queue → start_io → 큐별 처리.
 *  내부적으로 ftl_writer(쓰기), ftl_reloc(GC), ftl_nv_cache(NV 캐시 관리), ftl_l2p(L2P 맵 관리)가 코어
 *  폴러에서 매번 한 슬롯씩 실행되어 협조적 멀티태스킹을 형성한다.
 * 실행 컨텍스트: 디바이스 단일 spdk_thread(=core_thread)에 모든 처리가 고정. 다른 스레드(예: 사용자
 *  스레드)에서 IO를 ring으로 enqueue → 코어 스레드가 dequeue. lockless 디자인.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(호출자): bdev_ftl(spdk bdev 모듈), 사용자 애플리케이션. spdk_ftl_writev/readv/unmap이 진입.
 * - 하위(피호출자): ftl_io(IO 객체 init/complete), ftl_l2p(LBA → addr 룩업/pin), ftl_nv_cache(NV 캐시
 *   read/write/trim_seq_id), ftl_writer_*(writer state machine), ftl_reloc(GC tick), spdk bdev API
 *   (read_blocks/queue_io_wait), ftl_md(메타데이터 영속화), ftl_mngt(관리 작업, trim).
 * - 공유 자료구조: dev->rd_sq/wr_sq/trim_sq(소프트 IO 큐), dev->bands[](밴드 배열), dev->free_bands(자유
 *   밴드 풀), dev->stats(통계), dev->sb_shm(공유 메모리 — trim 진행 정보), g_ftl_write_buf/g_ftl_read_buf
 *   (전역 zero/패딩 버퍼).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_ftl_writev/readv/unmap: 사용자 IO 진입점. 검증 후 IO 객체에 채우고 ring으로 dev 스레드에 전달.
 * - ftl_core_poller: 디바이스 메인 폴러. 매 사이클 IO 큐 처리, writer 한 단계 진행, reloc/nv_cache/l2p
 *   tick. io_activity_total로 BUSY/IDLE 결정.
 * - ftl_process_io_queue: rd_sq/wr_sq/trim_sq에서 한 IO씩 꺼내 적절한 처리(read pin / nv cache write /
 *   trim) 시작.
 * - ftl_submit_read: read 경로의 핵심. L2P 룩업으로 연속 주소 묶음 만들고 NV cache 또는 base bdev에
 *   bdev_read_blocks 발행. ENOMEM 시 wait_entry로 재시도.
 * - ftl_invalidate_addr: 사용자 덮어쓰기/trim 시 valid_map에서 비트 클리어 + 밴드 P2L 맵 entry 무효화.
 * - ftl_apply_limits: free 밴드 수에 따라 사용자 쓰기 throttling 단계 결정.
 * - ftl_shutdown_complete: 셧다운 시 모든 서브시스템(NV cache/writer/reloc/L2P)이 halted인지 확인.
 * - ftl_process_trim / ftl_trim_log_*: TRIM 시퀀스 — seq_id 발급, trim_map 갱신, log → md 순서 영속화.
 */

/* [한국어] 분기 예측 힌트 매크로(spdk_likely/spdk_unlikely). */
#include "spdk/likely.h"
/* [한국어] 표준 C 헤더 추상화. */
#include "spdk/stdinc.h"
/* [한국어] NVMe 스펙 상수(SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS 등) — bdev_io 상태 분류용. */
#include "spdk/nvme.h"
/* [한국어] SPDK thread/poller — 디바이스 단일 스레드 모델 헬퍼. */
#include "spdk/thread.h"
/* [한국어] bdev 모듈 API — bdev_io_wait_entry 등. */
#include "spdk/bdev_module.h"
/* [한국어] string 헬퍼. */
#include "spdk/string.h"
/* [한국어] FTL 공개 API 선언(spdk_ftl_*). */
#include "spdk/ftl.h"
/* [한국어] CRC32 — 일부 서브시스템에서 사용(직접 호출은 없으나 헤더 의존성). */
#include "spdk/crc32.h"

/* [한국어] FTL 코어 자료구조와 헬퍼. */
#include "ftl_core.h"
/* [한국어] 밴드 헬퍼 — ftl_band_from_addr, ftl_band_set_state 등. */
#include "ftl_band.h"
/* [한국어] ftl_io 객체 init/complete/advance 등. */
#include "ftl_io.h"
/* [한국어] 디버그 매크로(FTL_ERRLOG, ftl_debug_inject_trim_error 등). */
#include "ftl_debug.h"
/* [한국어] 내부 상수와 자료구조(FTL_LBA_INVALID, FTL_LAYOUT_REGION_TYPE_* 등). */
#include "ftl_internal.h"
/* [한국어] 관리 작업(ftl_mngt_trim 등) — RPC/관리 경로의 trim 처리. */
#include "mngt/ftl_mngt.h"


/*
 * [한국어]
 * spdk_ftl_io_size - struct ftl_io의 크기를 외부에 노출.
 *
 * @return: sizeof(struct ftl_io).
 *
 * 사용자(예: bdev_ftl 모듈)가 미리 그 만큼의 메모리를 할당해 넘길 수 있도록 — IO 객체의 실제 크기는
 * 내부 구현에 가려져 있으므로 이 API를 거쳐야 한다.
 */
size_t
spdk_ftl_io_size(void)
{
	return sizeof(struct ftl_io);
}

/*
 * [한국어]
 * ftl_io_cmpl_cb - 사용자 read/write의 bdev 완료 콜백.
 *
 * @bdev_io/success/cb_arg: 완료된 bdev_io, 성공 여부, IO 컨텍스트(struct ftl_io*).
 *
 * 통계 갱신 → 실패 시 io->status에 -EIO 기록 → trace → req 카운터 감소 → 모든 sub-IO가 끝났으면
 * ftl_io_complete로 사용자 콜백 발화 → bdev_io free.
 *
 * 한 ftl_io는 여러 bdev_io로 분할될 수 있다(연속 주소 묶음 단위). 완료는 분할된 만큼 발생하며,
 * ftl_io_done(io)이 true가 될 때 비로소 사용자에게 통보된다.
 */
static void
ftl_io_cmpl_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_io *io = cb_arg;
	struct spdk_ftl_dev *dev = io->dev;

	/* [한국어] USER 트래픽 통계 누적 — bdev_io의 read/write 분류, NVMe 상태 코드 분석. */
	ftl_stats_bdev_io_completed(dev, FTL_STATS_TYPE_USER, bdev_io);

	if (spdk_unlikely(!success)) {
		/* [한국어] 어떤 sub-IO 한 개라도 실패하면 전체 IO를 -EIO로 마킹. */
		io->status = -EIO;
	}

	/* [한국어] 완료 trace event 기록 — 성능 분석/디버깅. */
	ftl_trace_completion(dev, io, FTL_TRACE_COMPLETION_DISK);

	/* [한국어] in-flight sub-IO 카운터 감소. */
	ftl_io_dec_req(io);
	if (ftl_io_done(io)) {
		/* [한국어] 모든 sub-IO 완료 — 사용자 콜백 호출. */
		ftl_io_complete(io);
	}

	/* [한국어] bdev_io 디스크립터를 채널의 mempool로 반납. */
	spdk_bdev_free_io(bdev_io);
}

/*
 * [한국어]
 * ftl_band_erase - 자유/닫힘 밴드를 PREP 상태로 전환(논리적 erase 시작점).
 *
 * @band: 대상 밴드(상태가 CLOSED 또는 FREE이어야 한다).
 *
 * 실제 NAND erase 명령은 드라이버 레벨에서 별도로 처리되며, 본 함수는 상태 머신상에서 erase 단계
 * 진입을 표시한다. PREP 상태는 _ftl_band_set_preparing에서 free_bands 풀에서 빼고 wr_cnt를 증가시킨다.
 */
static void
ftl_band_erase(struct ftl_band *band)
{
	/* [한국어] 사전 조건 검증 — 다른 상태에서 erase하면 데이터 손실. */
	assert(band->md->state == FTL_BAND_STATE_CLOSED ||
	       band->md->state == FTL_BAND_STATE_FREE);

	/* [한국어] 상태를 PREP으로 천이. */
	ftl_band_set_state(band, FTL_BAND_STATE_PREP);
}

/*
 * [한국어]
 * ftl_get_limit - SPDK_FTL_LIMIT_* 임계값 읽기.
 *
 * 설정된 limit 배열에서 type별 값을 반환. throttling 단계 결정에 사용.
 */
static size_t
ftl_get_limit(const struct spdk_ftl_dev *dev, int type)
{
	/* [한국어] enum 범위 검증. */
	assert(type < SPDK_FTL_LIMIT_MAX);
	return dev->conf.limits[type];
}

/*
 * [한국어]
 * ftl_shutdown_complete - 셧다운 절차가 모두 끝났는지 검사하고, 진행되지 않은 항목은 halt 호출로 트리거.
 *
 * @dev:    디바이스.
 * @return: true면 모든 in-flight/서브시스템이 정지 — 안전하게 종료 가능. false면 다음 polling에서 다시 시도.
 *
 * 검사 순서:
 *  (1) num_inflight: 디바이스 단위 in-flight IO 카운터 0인지.
 *  (2) NV cache halted (아니면 halt 호출).
 *  (3) writer_user halted.
 *  (4) reloc halted.
 *  (5) writer_gc halted.
 *  (6) NV cache chunks 모두 idle.
 *  (7) 모든 밴드의 queue_depth==0 && state != CLOSING.
 *  (8) L2P halted (아니면 halt 호출).
 *
 * 순서는 의존성을 고려한 것 — 위에서 아래로 갈수록 더 깊은 단계의 자원.
 */
static bool
ftl_shutdown_complete(struct spdk_ftl_dev *dev)
{
	uint64_t i;

	if (dev->num_inflight) {
		/* [한국어] 아직 처리 중인 사용자 IO 있음. */
		return false;
	}

	if (!ftl_nv_cache_is_halted(&dev->nv_cache)) {
		/* [한국어] NV cache가 처리 중 — halt 트리거 후 다음 사이클에 다시 검사. */
		ftl_nv_cache_halt(&dev->nv_cache);
		return false;
	}

	if (!ftl_writer_is_halted(&dev->writer_user)) {
		ftl_writer_halt(&dev->writer_user);
		return false;
	}

	if (!ftl_reloc_is_halted(dev->reloc)) {
		ftl_reloc_halt(dev->reloc);
		return false;
	}

	if (!ftl_writer_is_halted(&dev->writer_gc)) {
		ftl_writer_halt(&dev->writer_gc);
		return false;
	}

	if (!ftl_nv_cache_chunks_busy(&dev->nv_cache)) {
		/* [한국어] busy chunk 처리 중 — 끝날 때까지 대기. */
		return false;
	}

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		/* [한국어] in-flight bdev IO나 close 진행 중 밴드 검사. */
		if (dev->bands[i].queue_depth ||
		    dev->bands[i].md->state == FTL_BAND_STATE_CLOSING) {
			return false;
		}
	}

	if (!ftl_l2p_is_halted(dev)) {
		/* [한국어] L2P 마지막 — 다른 모든 게 끝난 후에야 L2P 정리. */
		ftl_l2p_halt(dev);
		return false;
	}

	/* [한국어] 모두 정지 — 안전 종료. */
	return true;
}

/*
 * [한국어]
 * ftl_apply_limits - 자유 밴드 수에 따라 사용자 쓰기 throttling 단계 결정.
 *
 * @dev: 디바이스.
 *
 * SPDK_FTL_LIMIT_CRIT(가장 엄격) → SPDK_FTL_LIMIT_MAX 순으로 임계값을 검사하여 첫 번째로 num_free가
 * limit 이하인 단계를 dev->limit에 기록. 통계의 limits[] 카운터도 증가. 실제 throttling은 다른 곳에서
 * 이 dev->limit를 보고 결정한다(예: ftl_nv_cache_throttle).
 */
void
ftl_apply_limits(struct spdk_ftl_dev *dev)
{
	size_t limit;
	struct ftl_stats *stats = &dev->stats;
	int i;

	/*  Clear existing limit */
	/* [한국어] 일단 가장 느슨한 단계(MAX = 제한 없음)로 초기화. */
	dev->limit = SPDK_FTL_LIMIT_MAX;

	for (i = SPDK_FTL_LIMIT_CRIT; i < SPDK_FTL_LIMIT_MAX; ++i) {
		/* [한국어] 단계별 임계값 읽기. */
		limit = ftl_get_limit(dev, i);

		if (dev->num_free <= limit) {
			/* [한국어] 자유 밴드가 임계 이하 — 이 단계 throttle 작동. 통계에 카운트하고 기록. */
			stats->limits[i]++;
			dev->limit = i;
			break;
		}
	}

	/* [한국어] trace 이벤트 — 단계 변화를 외부에서 추적. */
	ftl_trace_limits(dev, dev->limit, dev->num_free);
}

/*
 * [한국어]
 * ftl_invalidate_addr - 특정 주소를 invalid로 마킹(사용자 덮어쓰기/trim/GC 후 src 무효화).
 *
 * @dev:  디바이스.
 * @addr: 무효화할 물리 주소.
 *
 * 처리:
 *  - addr이 NV cache에 있다면 valid_map만 클리어(P2L 맵은 NV cache 측에서 관리).
 *  - base에 있다면 valid_map 클리어 + 밴드의 num_valid 감소 + (open/full 밴드면 P2L 맵 entry도 무효화).
 *
 * P2L 맵 entry 무효화는 close 시 P2L과 L2P의 일관성을 보장하기 위함. 이미 닫힌 밴드는 P2L이
 * 동결되어 있으므로 handle하지 않는다(별도 trim_log/지연 invalidation으로 처리).
 *
 * 동시 쓰기 시 race: "두 write가 같은 LBA에 동시 스케줄"되면 valid_map 비트가 이미 클리어되어 있을 수
 * 있어 if문으로 안전하게 검사 후 처리.
 */
void
ftl_invalidate_addr(struct spdk_ftl_dev *dev, ftl_addr addr)
{
	struct ftl_band *band;
	struct ftl_p2l_map *p2l_map;

	if (ftl_addr_in_nvc(dev, addr)) {
		/* [한국어] NV cache 영역 — valid bitmap만 클리어, NV cache 내부 P2L은 chunk 메타에서 관리됨. */
		ftl_bitmap_clear(dev->valid_map, addr);
		return;
	}

	/* [한국어] base 영역 — 어느 밴드에 속하는지 찾고 P2L 맵 갱신. */
	band = ftl_band_from_addr(dev, addr);
	p2l_map = &band->p2l_map;

	/* The bit might be already cleared if two writes are scheduled to the */
	/* same LBA at the same time */
	if (ftl_bitmap_get(dev->valid_map, addr)) {
		/* [한국어] 비트가 set이면 카운터 감소 + 클리어. set이 아니면 이미 처리됨(race 방어). */
		assert(p2l_map->num_valid > 0);
		ftl_bitmap_clear(dev->valid_map, addr);
		p2l_map->num_valid--;
	}

	/* Invalidate open/full band p2l_map entry to keep p2l and l2p
	 * consistency when band is going to close state */
	if (FTL_BAND_STATE_OPEN == band->md->state || FTL_BAND_STATE_FULL == band->md->state) {
		/* [한국어] open/full 상태에서만 P2L 맵 entry를 INVALID로 — 닫힌 밴드는 P2L이 영속화 직전이거나 동결. */
		p2l_map->band_map[ftl_band_block_offset_from_addr(band, addr)].lba = FTL_LBA_INVALID;
		p2l_map->band_map[ftl_band_block_offset_from_addr(band, addr)].seq_id = 0;
	}
}

/*
 * [한국어]
 * ftl_read_canceled - rc가 -EFAULT인지(=L2P invalid라서 read 불가) 검사.
 *
 * 읽으려는 LBA가 trim되었거나 한 번도 쓰지 않은 영역인 경우 ftl_get_next_read_addr가 -EFAULT 반환.
 * 그 경우 호출자는 사용자 버퍼를 0으로 채운다.
 */
static int
ftl_read_canceled(int rc)
{
	return rc == -EFAULT;
}

/*
 * [한국어]
 * ftl_get_next_read_addr - 현재 io->pos부터 연속된 주소 묶음을 L2P에서 찾는다.
 *
 * @io:    IO 컨텍스트.
 * @addr:  출력. 첫 LBA에 매핑된 주소.
 * @return: 묶을 수 있는 블록 수(>=1), 또는 -EFAULT(첫 LBA 자체가 INVALID).
 *
 * 동작:
 *  1) 첫 LBA를 L2P에서 룩업 → addr_cached(=NV cache 여부) 결정.
 *  2) 다음 LBA부터 (a) addr가 INVALID, (b) NV cache 여부가 다름, (c) 주소가 비연속이면 끊는다.
 *  3) 묶음을 io->map[]에 캐시(이후 ftl_io_iovec_addr 등이 사용).
 *
 * NV cache와 base의 경계를 넘는 묶음은 한 bdev read로 처리할 수 없어 별도 IO로 분할되어야 한다.
 */
static int
ftl_get_next_read_addr(struct ftl_io *io, ftl_addr *addr)
{
	struct spdk_ftl_dev *dev = io->dev;
	ftl_addr next_addr;
	size_t i;
	bool addr_cached = false;

	/* [한국어] 현재 LBA의 L2P 매핑 룩업. */
	*addr = ftl_l2p_get(dev, ftl_io_current_lba(io));
	io->map[io->pos] = *addr;

	/* If the address is invalid, skip it */
	if (*addr == FTL_ADDR_INVALID) {
		/* [한국어] 매핑 없음 — 호출자는 0으로 채워 처리. */
		return -EFAULT;
	}

	/* [한국어] 첫 주소가 NV cache인지 base인지 — 이후 묶음은 같은 영역이어야 한다. */
	addr_cached = ftl_addr_in_nvc(dev, *addr);

	for (i = 1; i < ftl_io_iovec_len_left(io); ++i) {
		/* [한국어] 다음 LBA 룩업. */
		next_addr = ftl_l2p_get(dev, ftl_io_get_lba(io, io->pos + i));

		if (next_addr == FTL_ADDR_INVALID) {
			/* [한국어] INVALID 만나면 묶음 종료. */
			break;
		}

		/* It's not enough to check for contiguity, if user data is on the last block
		 * of base device and first nvc, then they're 'contiguous', but can't be handled
		 * with one read request.
		 */
		if (addr_cached != ftl_addr_in_nvc(dev, next_addr)) {
			/* [한국어] NV cache <-> base 경계 — 다른 bdev이므로 묶음 종료. */
			break;
		}

		if (*addr + i != next_addr) {
			/* [한국어] 주소 비연속 — 묶음 종료. */
			break;
		}

		/* [한국어] 묶음에 포함 — io->map에 기록. */
		io->map[io->pos + i] = next_addr;
	}

	return i;
}

/* [한국어] 전방 선언 — 자기 자신을 wait_entry 콜백으로 등록하기 위해. */
static void ftl_submit_read(struct ftl_io *io);

/*
 * [한국어]
 * _ftl_submit_read - void* 시그니처 wrapper(spdk_bdev_queue_io_wait의 cb_fn 형식).
 */
static void
_ftl_submit_read(void *_io)
{
	struct ftl_io *io = _io;

	ftl_submit_read(io);
}

/*
 * [한국어]
 * ftl_submit_read - read 경로의 핵심 루프. 사용자 IO를 sub-IO로 분할해 bdev에 발행.
 *
 * @io: read IO.
 *
 * 알고리즘:
 *  while (io->pos < num_blocks):
 *    1) ftl_get_next_read_addr — 연속 주소 묶음 num_blocks 결정.
 *    2) -EFAULT면 0으로 채우고 다음 블록.
 *    3) NV cache 영역이면 ftl_nv_cache_read, base이면 spdk_bdev_read_blocks 발행.
 *    4) ENOMEM이면 wait_entry 등록 후 종료(나중에 재시도).
 *    5) inc_req + advance.
 *  루프 종료 후 모든 sub-IO가 즉시 끝난 경우(예: 전부 EFAULT)면 ftl_io_complete.
 */
static void
ftl_submit_read(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	ftl_addr addr;
	int rc = 0, num_blocks;

	while (io->pos < io->num_blocks) {
		/* [한국어] 연속 주소 묶음 결정. */
		num_blocks = ftl_get_next_read_addr(io, &addr);
		rc = num_blocks;

		/* User LBA doesn't hold valid data (trimmed or never written to), fill with 0 and skip this block */
		if (ftl_read_canceled(rc)) {
			/* [한국어] 매핑 없음 — 사용자 버퍼를 0으로 채움(POSIX read 의미: 미기록 영역=0). */
			memset(ftl_io_iovec_addr(io), 0, FTL_BLOCK_SIZE);
			ftl_io_advance(io, 1);
			continue;
		}

		assert(num_blocks > 0);

		/* [한국어] 제출 trace event — 성능 분석. */
		ftl_trace_submission(dev, io, addr, num_blocks);

		if (ftl_addr_in_nvc(dev, addr)) {
			/* [한국어] NV cache 영역 — NV cache 모듈이 chunk 위치 변환 후 bdev read 발행. */
			rc = ftl_nv_cache_read(io, addr, num_blocks, ftl_io_cmpl_cb, io);
		} else {
			/* [한국어] base 영역 — 직접 base bdev에 read 발행. addr가 base bdev LBA와 1:1 대응. */
			rc = spdk_bdev_read_blocks(dev->base_bdev_desc, dev->base_ioch,
						   ftl_io_iovec_addr(io),
						   addr, num_blocks, ftl_io_cmpl_cb, io);
		}

		if (spdk_unlikely(rc)) {
			if (rc == -ENOMEM) {
				/* [한국어] bdev_io 풀 고갈 — 어떤 bdev/채널에 wait할지 결정 후 등록. */
				struct spdk_bdev *bdev;
				struct spdk_io_channel *ch;

				if (ftl_addr_in_nvc(dev, addr)) {
					bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);
					ch = dev->nv_cache.cache_ioch;
				} else {
					bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
					ch = dev->base_ioch;
				}
				/* [한국어] wait_entry로 자기 자신을 cb로 등록 — 추후 bdev_io 회수 시 재시도. */
				io->bdev_io_wait.bdev = bdev;
				io->bdev_io_wait.cb_fn = _ftl_submit_read;
				io->bdev_io_wait.cb_arg = io;
				spdk_bdev_queue_io_wait(bdev, ch, &io->bdev_io_wait);
				return;
			} else {
				/* [한국어] 회복 불가능한 오류 — abort. */
				ftl_abort();
			}
		}

		/* [한국어] in-flight 카운터 증가, IO pos 진행. */
		ftl_io_inc_req(io);
		ftl_io_advance(io, num_blocks);
	}

	/* If we didn't have to read anything from the device, */
	/* complete the request right away */
	if (ftl_io_done(io)) {
		/* [한국어] 모든 블록이 EFAULT(=0으로 채움)였다면 발행한 sub-IO가 없음 — 즉시 완료. */
		ftl_io_complete(io);
	}
}

/*
 * [한국어]
 * ftl_needs_reloc - GC가 시작되어야 하는지 검사(자유 밴드가 LIMIT_START 이하인지).
 *
 * @return: true면 reloc 모듈이 적극적으로 GC 사이클을 진행해야 한다.
 *
 * reloc은 코어 폴러에서 매번 한 단계씩 진행되는데, 이 검사가 true면 GC 우선순위를 높인다.
 */
bool
ftl_needs_reloc(struct spdk_ftl_dev *dev)
{
	size_t limit = ftl_get_limit(dev, SPDK_FTL_LIMIT_START);

	if (dev->num_free <= limit) {
		return true;
	}

	return false;
}

/*
 * [한국어]
 * spdk_ftl_dev_get_attrs - 디바이스 속성을 사용자에게 노출.
 *
 * @attrs:      출력 구조체.
 * @attrs_size: 사용자가 제공한 구조체 크기(ABI 호환을 위해).
 *
 * 사용자가 num_blocks(LBA 수), block_size(바이트), optimum_io_size(xfer_size) 정보를 얻음.
 * SPDK bdev API 등에서 호출.
 */
void
spdk_ftl_dev_get_attrs(const struct spdk_ftl_dev *dev, struct spdk_ftl_attrs *attrs,
		       size_t attrs_size)
{
	/* [한국어] 사용자 LBA 수(over-provisioning 차감 후). */
	attrs->num_blocks = dev->num_lbas;
	/* [한국어] 블록 크기는 4KiB 고정. */
	attrs->block_size = FTL_BLOCK_SIZE;
	/* [한국어] 최적 IO 크기 = NVMe xfer_size — 사용자에게 정렬 권고. */
	attrs->optimum_io_size = dev->xfer_size;
	/* NOTE: check any new fields in attrs against attrs_size */
	/* [한국어] 새 필드 추가 시 attrs_size로 ABI 호환 검사 필요(현재는 미구현). */
}

/*
 * [한국어]
 * ftl_io_pin_cb - L2P 페이지 pin 완료 콜백.
 *
 * @dev/@status/@pin_ctx: 디바이스, pin 결과, pin 컨텍스트(cb_ctx에 ftl_io 보관).
 *
 * read를 진행하기 전 해당 LBA의 L2P 페이지가 메모리에 상주(pin)해 있어야 한다(읽는 동안 evict되지
 * 않도록). pin 성공 시 FTL_IO_PINNED 플래그를 set하고 ftl_submit_read로 진행. 실패 시 -EAGAIN으로
 * 사용자에게 통보(상위가 retry).
 */
static void
ftl_io_pin_cb(struct spdk_ftl_dev *dev, int status, struct ftl_l2p_pin_ctx *pin_ctx)
{
	struct ftl_io *io = pin_ctx->cb_ctx;

	if (spdk_unlikely(status != 0)) {
		/* Retry on the internal L2P fault */
		/* [한국어] L2P fault — 페이지 로드 실패. 사용자에게 EAGAIN으로 통보(retry 권고). */
		io->status = -EAGAIN;
		ftl_io_complete(io);
		return;
	}

	/* [한국어] pin 성공 — 플래그 set 후 read 발행. */
	io->flags |= FTL_IO_PINNED;
	ftl_submit_read(io);
}

/*
 * [한국어]
 * ftl_io_pin - read 시 L2P 페이지를 pin(또는 retry라면 skip).
 *
 * 처음 진입이면 ftl_l2p_pin로 LBA 범위의 L2P 페이지를 메모리에 로드/카운트 증가.
 * 이미 pin된 retry 경로면 ftl_l2p_pin_skip으로 다시 pin하지 않고 그냥 콜백 호출(중복 ref 회피).
 */
static void
ftl_io_pin(struct ftl_io *io)
{
	if (spdk_unlikely(io->flags & FTL_IO_PINNED)) {
		/*
		 * The IO is in a retry path and it had been pinned already.
		 * Continue with further processing.
		 */
		/* [한국어] retry 경로 — 이미 pin된 상태에서 다시 pin하면 ref 누수. skip 헬퍼로 콜백만 호출. */
		ftl_l2p_pin_skip(io->dev, ftl_io_pin_cb, io, &io->l2p_pin_ctx);
	} else {
		/* First time when pinning the IO */
		/* [한국어] 처음 — LBA 범위의 L2P 페이지를 pin. 비동기 — 완료 시 ftl_io_pin_cb. */
		ftl_l2p_pin(io->dev, io->lba, io->num_blocks,
			    ftl_io_pin_cb, io, &io->l2p_pin_ctx);
	}
}

/*
 * [한국어]
 * start_io - dequeue된 IO를 type별 큐(rd_sq/wr_sq/trim_sq)에 적재.
 *
 * @io: 처리할 IO.
 *
 * 절차:
 *  1) ioch->map_pool에서 매핑 버퍼 alloc(연속 주소 묶음 캐시용).
 *  2) type에 따라 rd/wr/trim_sq에 enqueue.
 *  3) 알 수 없는 type이면 -EOPNOTSUPP로 즉시 완료.
 *
 * map_pool ENOMEM 시 -ENOMEM으로 즉시 완료(상위가 retry).
 */
static void
start_io(struct ftl_io *io)
{
	struct ftl_io_channel *ioch = ftl_io_channel_get_ctx(io->ioch);
	struct spdk_ftl_dev *dev = io->dev;

	/* [한국어] map_pool에서 매핑 버퍼 alloc — io->num_blocks 길이의 ftl_addr 배열. */
	io->map = ftl_mempool_get(ioch->map_pool);
	if (spdk_unlikely(!io->map)) {
		/* [한국어] 풀 고갈 — 사용자에게 ENOMEM 즉시 통보. */
		io->status = -ENOMEM;
		ftl_io_complete(io);
		return;
	}

	switch (io->type) {
	case FTL_IO_READ:
		/* [한국어] 읽기 큐에 적재 — 폴러가 ftl_io_pin부터 처리. */
		TAILQ_INSERT_TAIL(&dev->rd_sq, io, queue_entry);
		break;
	case FTL_IO_WRITE:
		/* [한국어] 쓰기 큐 — NV cache write 단계로 진입. */
		TAILQ_INSERT_TAIL(&dev->wr_sq, io, queue_entry);
		break;
	case FTL_IO_TRIM:
		/* [한국어] TRIM 큐 — seq_id 발급 후 메타데이터/로그 영속화. */
		TAILQ_INSERT_TAIL(&dev->trim_sq, io, queue_entry);
		break;
	default:
		/* [한국어] 알 수 없는 type — 사용자에게 EOPNOTSUPP. */
		io->status = -EOPNOTSUPP;
		ftl_io_complete(io);
	}
}

/*
 * [한국어]
 * queue_io - 사용자 IO를 ring으로 dev 스레드에 전달.
 *
 * @dev: 디바이스.
 * @io:  IO.
 * @return: 0 성공, -EAGAIN ring full.
 *
 * SPDK ring(rte_ring 기반 lockless MPSC)을 사용해 다른 스레드에서 dev 스레드로 IO를 전달.
 * 사용자 스레드는 enqueue만 하고 dev 스레드의 폴러가 dequeue.
 */
static int
queue_io(struct spdk_ftl_dev *dev, struct ftl_io *io)
{
	size_t result;
	struct ftl_io_channel *ioch = ftl_io_channel_get_ctx(io->ioch);

	/* [한국어] ring SQ에 io 포인터 enqueue — lockless. result는 enqueue된 항목 수(0이면 ring full). */
	result = spdk_ring_enqueue(ioch->sq, (void **)&io, 1, NULL);
	if (spdk_unlikely(0 == result)) {
		/* [한국어] ring 가득 — 사용자에게 backpressure로 EAGAIN. */
		return -EAGAIN;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_ftl_writev - 사용자 비동기 쓰기 진입점.
 *
 * @dev:     FTL 디바이스.
 * @io:      사전할당된 ftl_io 객체(spdk_ftl_io_size 만큼).
 * @ch:      io_channel(코어 스레드의 핸들).
 * @lba:     시작 LBA.
 * @lba_cnt: 블록 수.
 * @iov/iov_cnt: 사용자 데이터 iovec.
 * @cb_fn/cb_arg: 완료 콜백.
 * @return:  0 성공(콜백 통해 결과 통보), 음수 에러.
 *
 * 검증 후 ftl_io_init으로 객체 채우고 ring으로 enqueue. 이후는 비동기.
 */
int
spdk_ftl_writev(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
		uint64_t lba, uint64_t lba_cnt, struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn,
		void *cb_arg)
{
	int rc;

	if (iov_cnt == 0) {
		/* [한국어] 빈 iovec — 의미 없는 IO. */
		return -EINVAL;
	}

	if (lba_cnt == 0) {
		/* [한국어] 0 블록 — 의미 없는 IO. */
		return -EINVAL;
	}

	if (lba_cnt != ftl_iovec_num_blocks(iov, iov_cnt)) {
		/* [한국어] iovec 총 길이와 lba_cnt 불일치 — 사용자 코드 버그. */
		FTL_ERRLOG(dev, "Invalid IO vector to handle, device %s, LBA %"PRIu64"\n",
			   dev->conf.name, lba);
		return -EINVAL;
	}

	if (!dev->initialized) {
		/* [한국어] 디바이스 초기화 미완료 — 재시도 권고. */
		return -EBUSY;
	}

	/* [한국어] IO 객체 초기화 — type=WRITE. */
	rc = ftl_io_init(ch, io, lba, lba_cnt, iov, iov_cnt, cb_fn, cb_arg, FTL_IO_WRITE);
	if (rc) {
		return rc;
	}

	/* [한국어] ring으로 dev 스레드에 enqueue. */
	return queue_io(dev, io);
}

/*
 * [한국어]
 * spdk_ftl_readv - 사용자 비동기 읽기 진입점. spdk_ftl_writev와 동일 패턴, type=READ.
 */
int
spdk_ftl_readv(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
	       uint64_t lba, uint64_t lba_cnt, struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg)
{
	int rc;

	/* [한국어] 위 writev와 동일한 검증 시퀀스. */
	if (iov_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt != ftl_iovec_num_blocks(iov, iov_cnt)) {
		FTL_ERRLOG(dev, "Invalid IO vector to handle, device %s, LBA %"PRIu64"\n",
			   dev->conf.name, lba);
		return -EINVAL;
	}

	if (!dev->initialized) {
		return -EBUSY;
	}

	/* [한국어] IO 객체 초기화 — type=READ. iovec은 출력 버퍼로 사용된다. */
	rc = ftl_io_init(ch, io, lba, lba_cnt, iov, iov_cnt, cb_fn, cb_arg, FTL_IO_READ);
	if (rc) {
		return rc;
	}

	return queue_io(dev, io);
}

/*
 * [한국어]
 * ftl_trim - 내부 TRIM 진입점(unmap 처리 헬퍼). iovec 없는 IO를 init 후 queue.
 */
int
ftl_trim(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
	 uint64_t lba, uint64_t lba_cnt, spdk_ftl_fn cb_fn, void *cb_arg)
{
	int rc;

	/* [한국어] TRIM은 데이터 전달 없음 — iov=NULL/iov_cnt=0. */
	rc = ftl_io_init(ch, io, lba, lba_cnt, NULL, 0, cb_fn, cb_arg, FTL_IO_TRIM);
	if (rc) {
		return rc;
	}

	return queue_io(dev, io);
}

/*
 * [한국어]
 * spdk_ftl_unmap - 사용자 unmap(=trim) 진입점.
 *
 * @dev/@io/@ch/@lba/@lba_cnt/@cb_fn/@cb_arg: 일반 IO 진입점과 동일.
 *
 * 동작:
 *  - 정렬 검증: lba와 lba_cnt가 lbas_in_page(=L2P 페이지 단위) 정렬이어야 의미 있는 trim.
 *  - 미정렬 IO: 사용자 IO면 NOP(즉시 0 완료), 관리/RPC IO면 EINVAL.
 *  - 정렬되어 있다면 io 유무에 따라 ftl_trim 또는 ftl_mngt_trim 분기.
 *
 * lbas_in_page 정렬을 강제하는 이유: trim 메타데이터는 L2P 페이지 단위로 관리되므로 부분 페이지 trim은
 * 다른 LBA의 매핑까지 무효화될 위험이 있어 안전하게 NOP 처리.
 */
int
spdk_ftl_unmap(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
	       uint64_t lba, uint64_t lba_cnt, spdk_ftl_fn cb_fn, void *cb_arg)
{
	int rc;
	/* [한국어] L2P 페이지 한 개에 들어가는 LBA 수 — trim 정렬 단위(보통 1MiB). */
	uint64_t alignment = dev->layout.l2p.lbas_in_page;

	if (lba_cnt == 0) {
		/* [한국어] 0 블록 — 무의미. */
		return -EINVAL;
	}

	if (lba + lba_cnt < lba_cnt) {
		/* [한국어] 정수 오버플로 — 잘못된 인자. */
		return -EINVAL;
	}

	if (lba + lba_cnt > dev->num_lbas) {
		/* [한국어] LBA 범위 초과. */
		return -EINVAL;
	}

	if (!dev->initialized) {
		return -EBUSY;
	}

	if (lba % alignment || lba_cnt % alignment) {
		/* [한국어] 정렬되지 않은 trim. */
		if (!io) {
			/* This is management/RPC path, its parameters must be aligned to 1MiB. */
			/* [한국어] 관리 경로(io=NULL)는 1MiB 정렬 필수. */
			return -EINVAL;
		}

		/* Otherwise unaligned IO requests are NOPs */
		/* [한국어] 사용자 IO 경로는 안전하게 NOP — IO 객체 init 후 즉시 성공 완료. */
		rc = ftl_io_init(ch, io, lba, lba_cnt, NULL, 0, cb_fn, cb_arg, FTL_IO_TRIM);
		if (rc) {
			return rc;
		}

		io->status = 0;
		ftl_io_complete(io);
		return 0;
	}

	if (io) {
		/* [한국어] 사용자 IO 경로 — ftl_trim으로 큐에 enqueue. */
		rc = ftl_trim(dev, io, ch, lba, lba_cnt, cb_fn, cb_arg);
	} else {
		/* [한국어] 관리 경로 — ftl_mngt_trim으로 직접 처리(동기적/비동기 mngt 시퀀스). */
		rc = ftl_mngt_trim(dev, lba, lba_cnt, cb_fn, cb_arg);
	}

	return rc;
}

/* [한국어] IO 큐의 한 batch 크기 — 한 번에 ring에서 dequeue할 최대 IO 수. */
#define FTL_IO_QUEUE_BATCH 16
/*
 * [한국어]
 * ftl_io_channel_poll - IO 채널의 CQ 폴러(io_channel당 1개 등록).
 *
 * @arg:    struct ftl_io_channel*.
 * @return: SPDK_POLLER_BUSY/IDLE.
 *
 * 코어 스레드가 처리한 IO를 사용자 스레드에 통보하기 위한 ring CQ. 사용자 콜백(user_fn)을 사용자
 * 스레드 컨텍스트에서 호출.
 */
int
ftl_io_channel_poll(void *arg)
{
	struct ftl_io_channel *ch = arg;
	void *ios[FTL_IO_QUEUE_BATCH];
	uint64_t i, count;

	/* [한국어] CQ에서 batch만큼 dequeue. */
	count = spdk_ring_dequeue(ch->cq, ios, FTL_IO_QUEUE_BATCH);
	if (count == 0) {
		return SPDK_POLLER_IDLE;
	}

	for (i = 0; i < count; i++) {
		struct ftl_io *io = ios[i];
		/* [한국어] 사용자 콜백 호출 — 사용자 스레드 컨텍스트에서 실행. */
		io->user_fn(io->cb_ctx, io->status);
	}

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * ftl_process_io_channel - 한 io_channel의 SQ에서 IO를 dequeue해 처리(start_io).
 *
 * 코어 스레드 폴러에서 매번 호출되어 ring SQ를 처리. lockless MPSC 패턴.
 */
static void
ftl_process_io_channel(struct spdk_ftl_dev *dev, struct ftl_io_channel *ioch)
{
	void *ios[FTL_IO_QUEUE_BATCH];
	size_t count, i;

	/* [한국어] SQ에서 batch만큼 dequeue. */
	count = spdk_ring_dequeue(ioch->sq, ios, FTL_IO_QUEUE_BATCH);
	if (count == 0) {
		return;
	}

	for (i = 0; i < count; i++) {
		struct ftl_io *io = ios[i];
		/* [한국어] type별 SQ로 적재. */
		start_io(io);
	}
}

/*
 * [한국어]
 * ftl_trim_log_clear - in-memory trim_log 헤더를 0으로 초기화(다음 trim 준비).
 *
 * trim 시퀀스의 마지막 단계 — log를 비워서 다음 trim에 깨끗한 상태로 시작.
 */
static void
ftl_trim_log_clear(struct spdk_ftl_dev *dev)
{
	struct ftl_trim_log *log = ftl_md_get_buffer(dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_LOG]);

	/* [한국어] 헤더만 0으로 — body는 다음 trim에서 새로 채워짐. */
	memset(&log->hdr, 0, sizeof(log->hdr));
}

/*
 * [한국어]
 * ftl_trim_finish - TRIM 시퀀스의 최종 마무리(콜백 발화 + 큐 깊이 감소).
 *
 * status가 0이 아니면 빌드 옵션에 따라 retry(IO를 trim_sq 헤드에 다시 넣음) 또는 status 보고.
 */
static void
ftl_trim_finish(struct ftl_io *io, int status)
{
	/* [한국어] trim 진행 카운터 감소(0이 되면 다음 trim 가능). */
	io->dev->trim_qd--;

	if (spdk_unlikely(status)) {
#ifdef SPDK_FTL_RETRY_ON_ERROR
		/* [한국어] 재시도 모드 — IO 상태 클리어 후 trim_sq 헤드에 다시 넣음. */
		ftl_io_clear(io);
		TAILQ_INSERT_HEAD(&io->dev->trim_sq, io, queue_entry);
		return;
#else
		io->status = status;
#endif
	}

	/* [한국어] 사용자 콜백 호출. */
	ftl_io_complete(io);
}

/*
 * [한국어]
 * ftl_trim_log_close_cb - trim 시퀀스의 마지막 log 영속화 완료 콜백.
 */
static void
ftl_trim_log_close_cb(int status, void *cb_arg)
{
	struct ftl_io *io = cb_arg;

	/* [한국어] 종료 — 사용자에게 결과 통보. */
	ftl_trim_finish(io, status);
}

/*
 * [한국어]
 * ftl_trim_log_persist - in-memory trim_log을 디스크에 영속화.
 *
 * @io: trim IO.
 * @cb: 영속화 완료 콜백(시점에 따라 ftl_trim_log_open_cb 또는 ftl_trim_log_close_cb).
 *
 * trim_log는 entry 1개짜리 region — md->buffer를 그대로 쓰면 됨.
 */
static void
ftl_trim_log_persist(struct ftl_io *io, ftl_md_io_entry_cb cb)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_md *trim_log = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_LOG];

	/* [한국어] entry 0번 1개 영속화. trim_md_io_entry_ctx는 retry 컨텍스트. */
	ftl_md_persist_entries(trim_log, 0, 1, ftl_md_get_buffer(trim_log), NULL,
			       cb, io, &dev->trim_md_io_entry_ctx);
}

/*
 * [한국어]
 * ftl_trim_md_cb - trim_md 영속화 완료 콜백 — 다음 단계(trim_log 정리/영속화)로 진행.
 */
static void
ftl_trim_md_cb(int status, void *cb_arg)
{
	struct ftl_io *io = cb_arg;
	struct spdk_ftl_dev *dev = io->dev;

	if (status) {
		/* [한국어] trim_md 실패 status 보존. */
		io->status = status;
	}
	/* [한국어] log를 0으로 클리어한 뒤 그것을 디스크에 다시 영속화 → close_cb로 종료. */
	ftl_trim_log_clear(dev);
	ftl_trim_log_persist(io, ftl_trim_log_close_cb);
}

/*
 * [한국어]
 * ftl_trim_log_open_cb - trim 시퀀스의 첫 단계(open log) 영속화 완료 콜백.
 *
 * @cb_arg: trim IO.
 *
 * 다음 단계로 진행: trim_md의 일부분(이번 trim에 영향받는 페이지 범위)을 디스크에 영속화. 완료 시
 * ftl_trim_md_cb가 호출되어 log 클리어 → close 단계로 이어짐.
 *
 * trim 시퀀스 흐름:
 *  1) ftl_process_trim → in-memory trim_map/trim_log 갱신
 *  2) ftl_trim_log_persist(open_cb) — log를 먼저 디스크에 기록(write-ahead)
 *  3) [본 함수] trim_md(L2P 매핑 무효화 정보) 영속화 발행
 *  4) ftl_trim_md_cb — log 클리어 + 다시 영속화(close)
 *  5) ftl_trim_log_close_cb → ftl_trim_finish — 사용자 콜백
 *
 * write-ahead pattern: log를 먼저 쓰면 crash 시 log를 보고 trim을 재실행 가능.
 */
static void
ftl_trim_log_open_cb(int status, void *cb_arg)
{
	struct ftl_io *io = cb_arg;
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_md *trim_md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD];
	uint64_t first, entries;
	/* [한국어] trim_md 한 블록당 entries(=uint64_t) 수. */
	const uint64_t entries_in_block = FTL_BLOCK_SIZE / sizeof(uint64_t);
	char *buffer;

	if (status) {
		/* [한국어] log 영속화 실패 — 사용자 통보. */
		ftl_trim_finish(io, status);
		return;
	}

	/* Map trim space into L2P pages */
	/* [한국어] LBA → L2P 페이지 인덱스로 변환(이번 trim이 영향주는 페이지 범위). */
	first = io->lba / dev->layout.l2p.lbas_in_page;
	entries = io->num_blocks / dev->layout.l2p.lbas_in_page;
	/* Map pages into trim metadata location */
	/* [한국어] 페이지 인덱스 → trim_md 블록 인덱스로 변환(= 페이지 인덱스 / entries_in_block). */
	first = first / entries_in_block;
	entries = spdk_divide_round_up(entries, entries_in_block);

	/* Get trim metadata buffer */
	/* [한국어] 영속화할 trim_md의 부분 버퍼 위치 계산. */
	buffer = (char *)ftl_md_get_buffer(trim_md) + (FTL_BLOCK_SIZE * first);

	/* Persist the trim metadata snippet which corresponds to the trim IO */
	/* [한국어] 부분 영속화 — trim_md의 [first, first+entries) 블록만 디스크에 기록. */
	ftl_md_persist_entries(trim_md, first, entries, buffer, NULL,
			       ftl_trim_md_cb, io, &dev->trim_md_io_entry_ctx);
}

/*
 * [한국어]
 * ftl_set_trim_map - in-memory trim_map과 trim_log을 갱신.
 *
 * @dev:        디바이스.
 * @lba:        시작 LBA.
 * @num_blocks: 트림 블록 수.
 * @seq_id:     이 trim의 seq_id(NV cache에서 발급).
 *
 * trim_map(비트맵)에 영향받는 L2P 페이지 비트를 set하고, trim_md 페이지 배열에 seq_id를 기록.
 * trim_log 헤더에는 trim 정보(start_lba/num_blocks/seq_id)를 기록 — recovery 시 사용.
 */
void
ftl_set_trim_map(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t num_blocks, uint64_t seq_id)
{
	uint64_t first_page, num_pages;
	uint64_t lbas_in_page = dev->layout.l2p.lbas_in_page;
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD];
	uint64_t *page = ftl_md_get_buffer(md);
	struct ftl_trim_log *log;
	size_t i;

	/* [한국어] LBA → L2P 페이지 인덱스. */
	first_page = lba / lbas_in_page;
	num_pages = num_blocks / lbas_in_page;

	/* Fill trim metadata */
	for (i = first_page; i < first_page + num_pages; ++i) {
		/* [한국어] trim_map 비트맵 set — 이 페이지가 trim됨을 표시. */
		ftl_bitmap_set(dev->trim_map, i);
		/* [한국어] trim_md 페이지에 seq_id 기록 — L2P가 이 시점 이후 매핑은 무효로 간주. */
		page[i] = seq_id;
	}

	/* Fill trim log */
	/* [한국어] trim_log 헤더에 이번 trim 정보 기록 — recovery 시 재실행에 사용. */
	log = ftl_md_get_buffer(dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_LOG]);
	log->hdr.trim.seq_id = seq_id;
	log->hdr.trim.num_blocks = num_blocks;
	log->hdr.trim.start_lba = lba;
}

/*
 * [한국어]
 * ftl_process_trim - TRIM 시퀀스의 시작점(seq_id 발급 + in-memory 갱신 + 영속화 발행).
 *
 * @return: true면 발행 성공, false면 NV cache에 open chunk가 없어 대기 필요(상위가 retry).
 *
 * 절차:
 *  1) NV cache에서 trim seq_id 발급 시도 — open chunk가 없으면 0 반환(대기).
 *  2) trim_in_progress=true / trim_qd++ — 다음 trim 차단.
 *  3) sb_shm에 진행 정보 기록(공유 메모리 — recovery/디버깅용).
 *  4) ftl_set_trim_map으로 in-memory 갱신.
 *  5) sb_shm.in_progress=false (디버그 inject 포인트와 함께).
 *  6) ftl_trim_log_persist(open_cb)로 영속화 시퀀스 시작.
 */
static bool
ftl_process_trim(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	uint64_t seq_id;

	/* [한국어] NV cache에 open chunk가 있어야 seq_id 발급 가능 — 없으면 0 반환. */
	seq_id = ftl_nv_cache_acquire_trim_seq_id(&dev->nv_cache);
	if (seq_id == 0) {
		return false;
	}

	/* [한국어] trim 진행 표시. */
	dev->trim_in_progress = true;
	dev->trim_qd++;

	/* [한국어] 공유 메모리에 진행 정보 기록 — recovery/관찰용. */
	dev->sb_shm->trim.start_lba = io->lba;
	dev->sb_shm->trim.num_blocks = io->num_blocks;
	dev->sb_shm->trim.seq_id = seq_id;
	dev->sb_shm->trim.in_progress = true;
	/* [한국어] in-memory trim_map/trim_log 갱신. */
	ftl_set_trim_map(dev, io->lba, io->num_blocks, seq_id);
	/* [한국어] 디버그 inject 포인트 — 테스트 시 trim 도중 crash 시뮬레이션. */
	ftl_debug_inject_trim_error();
	dev->sb_shm->trim.in_progress = false;

	/* [한국어] 영속화 시퀀스 시작 — log → md → log 클리어 순. */
	ftl_trim_log_persist(io, ftl_trim_log_open_cb);
	return true;
}

/*
 * [한국어]
 * ftl_process_io_queue - rd_sq/wr_sq/trim_sq에서 IO를 한 개씩 꺼내 처리.
 *
 * 코어 폴러에서 매번 호출. 한 번에 큐 하나당 1개 IO만 처리(읽기) 또는 write throttle을 보고 다수 처리.
 *
 * 처리 순서: read → write(throttle 체크) → trim(qd==0 체크) → 모든 io_channel 의 SQ.
 */
static void
ftl_process_io_queue(struct spdk_ftl_dev *dev)
{
	struct ftl_io_channel *ioch;
	struct ftl_io *io;

	/* TODO: Try to figure out a mechanism to batch more requests at the same time,
	 * with keeping enough resources (pinned pages), between reads, writes and gc/compaction
	 */
	if (!TAILQ_EMPTY(&dev->rd_sq)) {
		/* [한국어] 읽기 큐 — 한 번에 1개씩 ftl_io_pin 진입. pin은 비동기. */
		io = TAILQ_FIRST(&dev->rd_sq);
		TAILQ_REMOVE(&dev->rd_sq, io, queue_entry);
		assert(io->type == FTL_IO_READ);
		ftl_io_pin(io);
		ftl_add_io_activity(dev);
	}

	while (!TAILQ_EMPTY(&dev->wr_sq) && !ftl_nv_cache_throttle(dev)) {
		/* [한국어] 쓰기 큐 — NV cache가 throttle 상태가 아니면 가능한 만큼 처리.
		 *         ftl_nv_cache_write가 false 반환 시(예: chunk 가득) 큐 헤드에 다시 넣고 break. */
		io = TAILQ_FIRST(&dev->wr_sq);
		TAILQ_REMOVE(&dev->wr_sq, io, queue_entry);
		assert(io->type == FTL_IO_WRITE);
		if (!ftl_nv_cache_write(io)) {
			TAILQ_INSERT_HEAD(&dev->wr_sq, io, queue_entry);
			break;
		}
		ftl_add_io_activity(dev);
	}

	if (!TAILQ_EMPTY(&dev->trim_sq) && dev->trim_qd == 0) {
		/* [한국어] trim_qd==0 — 직렬화: 한 번에 한 trim만 진행(트림은 메타데이터에 광범위 영향). */
		io = TAILQ_FIRST(&dev->trim_sq);
		TAILQ_REMOVE(&dev->trim_sq, io, queue_entry);
		assert(io->type == FTL_IO_TRIM);

		/*
		 * Trim operation requires generating a sequence id for itself, which it gets based on the open chunk
		 * in nv cache. If there are no open chunks (because we're in the middle of state transition or compaction
		 * lagged behind), then we need to wait for the nv cache to resolve the situation - it's fine to just put the
		 * trim and try again later.
		 */
		if (!ftl_process_trim(io)) {
			/* [한국어] open chunk 없음 — 큐 헤드에 다시 넣고 다음 사이클에 재시도. */
			TAILQ_INSERT_HEAD(&dev->trim_sq, io, queue_entry);
		} else {
			ftl_add_io_activity(dev);
		}
	}

	/* [한국어] 모든 io_channel의 SQ에서 새 IO를 dequeue해 type별 큐로 분배. */
	TAILQ_FOREACH(ioch, &dev->ioch_queue, entry) {
		ftl_process_io_channel(dev, ioch);
	}
}

/*
 * [한국어]
 * ftl_core_poller - 디바이스 메인 폴러. SPDK_POLLER_REGISTER로 등록되어 매 사이클 호출.
 *
 * @ctx:    struct spdk_ftl_dev*.
 * @return: SPDK_POLLER_BUSY(이번 사이클에 활동 있음) 또는 IDLE.
 *
 * 단계:
 *  (1) 셧다운 검사: dev->halt && shutdown_complete이면 폴러 unregister 후 IDLE.
 *  (2) IO 큐 처리.
 *  (3) writer_user/writer_gc 한 단계 진행 (pre-rolled state machine).
 *  (4) reloc tick (GC 한 단계).
 *  (5) NV cache process.
 *  (6) L2P process (페이지 evict/load 등).
 *  (7) io_activity_total 변화로 BUSY/IDLE 반환.
 */
int
ftl_core_poller(void *ctx)
{
	struct spdk_ftl_dev *dev = ctx;
	/* [한국어] 진입 시점의 누적 활동량 — 이번 사이클에서 변화가 있으면 BUSY. */
	uint64_t io_activity_total_old = dev->stats.io_activity_total;

	if (dev->halt && ftl_shutdown_complete(dev)) {
		/* [한국어] 셧다운 요청 + 모든 서브시스템 정지 — 폴러 등록 해제 후 종료. */
		spdk_poller_unregister(&dev->core_poller);
		return SPDK_POLLER_IDLE;
	}

	/* [한국어] 한 사이클의 협조적 멀티태스킹 — 각 모듈이 한 단계씩 진행. */
	ftl_process_io_queue(dev);
	ftl_writer_run(&dev->writer_user);
	ftl_writer_run(&dev->writer_gc);
	ftl_reloc(dev->reloc);
	ftl_nv_cache_process(dev);
	ftl_l2p_process(dev);

	if (io_activity_total_old != dev->stats.io_activity_total) {
		/* [한국어] 활동 있음 — SPDK 스케줄러에 BUSY 통보(이 코어를 계속 polling 우선). */
		return SPDK_POLLER_BUSY;
	}

	/* [한국어] 활동 없음 — 다른 코어/스레드에 양보 가능. */
	return SPDK_POLLER_IDLE;
}

/*
 * [한국어]
 * ftl_band_get_next_free - free_bands 풀에서 첫 밴드를 꺼내고 erase 단계로 진입.
 *
 * @return: 꺼낸 밴드(없으면 NULL).
 *
 * writer가 새 밴드를 필요로 할 때 호출. ftl_band_erase는 상태를 PREP으로 전환해 free에서 빠지고 wr_cnt 증가.
 */
struct ftl_band *
ftl_band_get_next_free(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band = NULL;

	if (!TAILQ_EMPTY(&dev->free_bands)) {
		/* [한국어] free 풀의 첫 밴드 꺼냄 — TAILQ_REMOVE는 ftl_band_erase 안의 상태 전이가 다시 한 번 처리. */
		band = TAILQ_FIRST(&dev->free_bands);
		TAILQ_REMOVE(&dev->free_bands, band, queue_entry);
		/* [한국어] PREP 상태로 전이(논리적 erase 시작). */
		ftl_band_erase(band);
	}

	return band;
}

/* [한국어] 전역 zero-fill 버퍼 — DMA-safe. write/read 시 패딩 또는 zero-fill에 사용. */
void *g_ftl_write_buf;
void *g_ftl_read_buf;

/*
 * [한국어]
 * spdk_ftl_init - FTL 라이브러리 전역 초기화. 두 개의 hugepage DMA 버퍼 alloc.
 *
 * @return: 0 성공, -ENOMEM 실패.
 *
 * 모든 디바이스가 공유하는 패딩/zero-fill 버퍼. spdk_zmalloc(SPDK_MALLOC_DMA)로 hugepage에서 alloc되어
 * NVMe DMA 가능. 한 번만 호출되어야 한다(전역 변수).
 */
int
spdk_ftl_init(void)
{
	/* [한국어] write 시 사용할 zero 패딩 버퍼 — 부분 페이지를 0으로 채울 때. */
	g_ftl_write_buf = spdk_zmalloc(FTL_ZERO_BUFFER_SIZE, FTL_ZERO_BUFFER_SIZE, NULL,
				       SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!g_ftl_write_buf) {
		return -ENOMEM;
	}

	/* [한국어] read 시 사용할 dump 버퍼 — 사용자에게 노출하지 않는 영역의 read 결과 흡수. */
	g_ftl_read_buf = spdk_zmalloc(FTL_ZERO_BUFFER_SIZE, FTL_ZERO_BUFFER_SIZE, NULL,
				      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!g_ftl_read_buf) {
		/* [한국어] read 버퍼 alloc 실패 — write 버퍼 되돌림. */
		spdk_free(g_ftl_write_buf);
		g_ftl_write_buf = NULL;
		return -ENOMEM;
	}
	return 0;
}

/*
 * [한국어]
 * spdk_ftl_fini - 전역 자원 해제.
 */
void
spdk_ftl_fini(void)
{
	/* [한국어] hugepage 버퍼 둘 다 해제. */
	spdk_free(g_ftl_write_buf);
	spdk_free(g_ftl_read_buf);
}

/*
 * [한국어]
 * spdk_ftl_dev_set_fast_shutdown - 디바이스에 빠른 셧다운 모드 플래그 설정.
 *
 * fast_shutdown=true면 다음 셧다운 시 메타데이터 정합성 보장보다 속도 우선(예: dirty 상태 허용).
 */
void
spdk_ftl_dev_set_fast_shutdown(struct spdk_ftl_dev *dev, bool fast_shutdown)
{
	assert(dev);
	dev->conf.fast_shutdown = fast_shutdown;
}

/*
 * [한국어]
 * ftl_stats_bdev_io_completed - bdev IO 완료 시 type별/그룹별 통계 갱신.
 *
 * @dev/@type/@bdev_io: 디바이스, 통계 카테고리(USER/GC/CMP/MD_BASE 등), 완료된 bdev IO.
 *
 * 동작:
 *  - bdev_io의 IO 종류(READ/WRITE)로 stats_group 결정.
 *  - NVMe 상태 코드 분석:
 *    GENERIC + SUCCESS → ios++, blocks 누적.
 *    MEDIA_ERROR → errors.media++.
 *    그 외 → errors.other++.
 *
 * NVMe 상태(SCT/SC) 의미: NVMe 1.x 스펙 Figure 31. Status Code Types/Status Codes.
 * - SCT_GENERIC(0): 일반 상태. SC_SUCCESS(0)는 정상 완료.
 * - SCT_MEDIA_ERROR(2): NAND 미디어 오류(read uncorrectable 등). 디바이스 건강성 모니터링에 중요.
 */
void
ftl_stats_bdev_io_completed(struct spdk_ftl_dev *dev, enum ftl_stats_type type,
			    struct spdk_bdev_io *bdev_io)
{
	struct ftl_stats_entry *stats_entry = &dev->stats.entries[type];
	struct ftl_stats_group *stats_group;
	uint32_t cdw0;
	int sct;
	int sc;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		stats_group = &stats_entry->read;
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		/* [한국어] 일반 write와 write_zeroes 모두 write 그룹에 누적. */
		stats_group = &stats_entry->write;
		break;
	default:
		/* [한국어] 그 외 IO 타입(flush, unmap 등)은 통계 미집계 — 정책 결정. */
		return;
	}

	/* [한국어] bdev_io에서 NVMe completion(CDW0/SCT/SC) 추출. */
	spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);

	if (sct == SPDK_NVME_SCT_GENERIC && sc == SPDK_NVME_SC_SUCCESS) {
		/* [한국어] 정상 완료 — IO 수와 블록 수 누적. */
		stats_group->ios++;
		stats_group->blocks += bdev_io->u.bdev.num_blocks;
	} else if (sct == SPDK_NVME_SCT_MEDIA_ERROR) {
		/* [한국어] NAND 미디어 오류 — 별도 카운터(SMART 추적). */
		stats_group->errors.media++;
	} else {
		/* [한국어] 그 외 오류(generic but non-zero, NVMe-specific 등). */
		stats_group->errors.other++;
	}
}

/*
 * [한국어]
 * spdk_ftl_get_io_channel - 사용자가 FTL 디바이스의 io_channel을 얻는 API.
 *
 * 내부적으로 SPDK io_device_register/get_io_channel 패턴 사용 — 각 사용자 스레드별 채널 인스턴스 생성.
 */
struct spdk_io_channel *
spdk_ftl_get_io_channel(struct spdk_ftl_dev *dev)
{
	return spdk_get_io_channel(dev);
}

/*
 * [한국어]
 * ftl_stats_crc_error - CRC 검증 실패 통계 카운트(주로 P2L 맵 read 시 검증 실패).
 *
 * read 그룹에 errors.crc++ 누적. read 경로에서만 CRC 검증을 수행하기에 read 그룹에 기록.
 */
void
ftl_stats_crc_error(struct spdk_ftl_dev *dev, enum ftl_stats_type type)
{

	struct ftl_stats_entry *stats_entry = &dev->stats.entries[type];
	struct ftl_stats_group *stats_group = &stats_entry->read;

	/* [한국어] CRC 오류 카운터 증가 — 디바이스 건강성/메타데이터 무결성 추적. */
	stats_group->errors.crc++;
}

/*
 * [한국어]
 * struct ftl_get_stats_ctx - 통계 조회 비동기 컨텍스트.
 *
 * 통계는 dev 스레드에서만 안전하게 읽을 수 있으므로, 다른 스레드의 요청은 메시지로 dev 스레드로
 * 보내고 결과를 다시 호출자 스레드로 송신해 콜백한다(cross-thread 안전 패턴).
 */
struct ftl_get_stats_ctx {
	struct spdk_ftl_dev *dev;
	/* [한국어] 통계를 읽어올 대상 FTL 디바이스 포인터.
	 * 설정자: spdk_ftl_get_stats()가 호출자 인자 dev를 그대로 보관.
	 * 읽는 자: _ftl_get_stats()(dev 스레드 컨텍스트)가 dev->stats를 복사할 때 참조.
	 * 값 범위: 유효한 spdk_ftl_dev 포인터(NULL 불가). 디바이스 수명 동안 유효.
	 * 동기화: dev->stats 접근은 dev->core_thread에서만 일어나도록 메시지 패싱으로 고정 — 별도 락 불필요. */

	struct ftl_stats *stats;
	/* [한국어] 사용자가 제공한 출력 버퍼 — 복사된 통계 스냅샷이 기록될 곳.
	 * 설정자: spdk_ftl_get_stats()가 호출자 인자 stats를 보관.
	 * 읽는 자: _ftl_get_stats()가 *stats = dev->stats로 채우고, _ftl_get_stats_cb()가 cb_fn에 넘김.
	 * 값 범위: 호출자 소유의 ftl_stats 버퍼(콜백이 끝날 때까지 유효해야 함).
	 * 동기화: 쓰기는 dev 스레드에서, 읽기(콜백)는 호출자 스레드에서 — 메시지 순서로 happens-before 보장. */

	struct spdk_thread *thread;
	/* [한국어] 통계 조회를 시작한 호출자 스레드 핸들 — 콜백을 같은 스레드에서 발화하기 위해 저장.
	 * 설정자: spdk_ftl_get_stats()에서 spdk_get_thread()로 현재 스레드 캡처.
	 * 읽는 자: _ftl_get_stats()가 결과 복사 후 이 스레드로 spdk_thread_send_msg() 송신할 때 사용.
	 * 값 범위: 유효한 spdk_thread 포인터. cross-thread 안전 패턴의 복귀 지점.
	 * 동기화: SPDK 메시지 큐(lockless)를 통해 dev 스레드 → 이 스레드로 제어가 되돌아간다. */

	spdk_ftl_stats_fn cb_fn;
	/* [한국어] 통계 복사 완료 시 호출자에게 결과를 통보하는 사용자 콜백 함수 포인터.
	 * 설정자: spdk_ftl_get_stats()가 호출자 인자 cb_fn 보관.
	 * 읽는 자: _ftl_get_stats_cb()(호출자 스레드 컨텍스트)가 cb_fn(stats, cb_arg)로 호출.
	 * 값 범위: 유효한 spdk_ftl_stats_fn(NULL 불가). 정확히 1회 호출됨.
	 * 동기화: 호출자 스레드에서만 실행되므로 콜백 내부에서 별도 락 고려 불필요. */

	void *cb_arg;
	/* [한국어] cb_fn에 그대로 전달되는 사용자 정의 불투명 인자.
	 * 설정자: spdk_ftl_get_stats()가 호출자 인자 cb_arg 보관.
	 * 읽는 자: _ftl_get_stats_cb()가 cb_fn(stats, cb_arg)의 두 번째 인자로 전달.
	 * 값 범위: 사용자 임의 포인터(NULL 허용) — FTL 내부에서는 의미 해석하지 않음.
	 * 동기화: FTL은 이 값을 역참조하지 않으므로 동기화 대상 아님. */
};

/*
 * [한국어]
 * _ftl_get_stats_cb - 호출자 스레드에서 실행되는 통계 콜백 발화 함수.
 *
 * dev 스레드에서 통계를 복사 완료하면 호출자 스레드로 메시지를 보내 본 함수가 실행된다.
 * 사용자 콜백 호출 후 컨텍스트 free.
 */
static void
_ftl_get_stats_cb(void *_ctx)
{
	struct ftl_get_stats_ctx *stats_ctx = _ctx;

	/* [한국어] 사용자 콜백 호출 — 호출자 스레드 컨텍스트. */
	stats_ctx->cb_fn(stats_ctx->stats, stats_ctx->cb_arg);
	/* [한국어] 동적 alloc된 컨텍스트 해제. */
	free(stats_ctx);
}

/*
 * [한국어]
 * _ftl_get_stats - dev 스레드에서 실행되는 통계 복사 함수.
 *
 * dev->stats를 사용자 출력 버퍼에 복사 후 호출자 스레드로 메시지 송신.
 */
static void
_ftl_get_stats(void *_ctx)
{
	struct ftl_get_stats_ctx *stats_ctx = _ctx;

	/* [한국어] dev->stats 전체를 사용자 버퍼에 복사 — dev 스레드에서만 안전. */
	*stats_ctx->stats = stats_ctx->dev->stats;

	/* [한국어] 호출자 스레드로 메시지 — 콜백 발화. */
	spdk_thread_send_msg(stats_ctx->thread, _ftl_get_stats_cb, stats_ctx);
}

/*
 * [한국어]
 * spdk_ftl_get_stats - 사용자에게 노출되는 비동기 통계 조회 API.
 *
 * @dev/@stats/@cb_fn/@cb_arg: 디바이스, 출력 버퍼, 완료 콜백, 콜백 인자.
 * @return: 0 dispatch 성공(콜백 통해 결과), -ENOMEM 컨텍스트 alloc 실패.
 *
 * 절차:
 *  1) ftl_get_stats_ctx alloc(calloc로 0 초기화).
 *  2) 호출자 스레드 정보 보관.
 *  3) dev->core_thread로 메시지 송신 — _ftl_get_stats가 dev 스레드에서 실행.
 *
 * cross-thread 안전성을 위해 두 번의 thread_send_msg 사용(호출자 → dev → 호출자).
 */
int
spdk_ftl_get_stats(struct spdk_ftl_dev *dev, struct ftl_stats *stats, spdk_ftl_stats_fn cb_fn,
		   void *cb_arg)
{
	struct ftl_get_stats_ctx *stats_ctx;

	/* [한국어] 컨텍스트 alloc — calloc으로 0 초기화. */
	stats_ctx = calloc(1, sizeof(struct ftl_get_stats_ctx));
	if (!stats_ctx) {
		return -ENOMEM;
	}

	/* [한국어] 컨텍스트 채우기. */
	stats_ctx->dev = dev;
	stats_ctx->stats = stats;
	stats_ctx->cb_fn = cb_fn;
	stats_ctx->cb_arg = cb_arg;
	/* [한국어] 호출자 스레드 ID 저장 — _ftl_get_stats가 결과를 보낼 대상. */
	stats_ctx->thread = spdk_get_thread();

	/* [한국어] dev 스레드로 메시지 — _ftl_get_stats가 거기서 실행됨. */
	spdk_thread_send_msg(dev->core_thread, _ftl_get_stats, stats_ctx);

	return 0;
}

/* [한국어] SPDK 로그 컴포넌트 등록 — "ftl_core" 이름으로 디버그 레벨 제어 가능. */
SPDK_LOG_REGISTER_COMPONENT(ftl_core)
