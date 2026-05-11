/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright 2023 Solidigm All Rights Reserved
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 밴드 I/O 연산 구현 (ftl_band_ops.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK FTL(Flash Translation Layer)의 "밴드(band)" 단위 I/O 연산을 구현한다.
 * 밴드란 NAND 플래시의 erase 단위(여러 zone의 묶음)를 추상화한 것으로, FTL은 로그-구조(log-structured)
 * 방식으로 밴드를 한쪽 끝에서부터 순차적으로 채우고, 가득 차면 닫고, GC(garbage collection) 시 통째로
 * 비워(erase) 재사용한다. 이 파일은 그러한 밴드의 라이프사이클(open → write/read → close → free)에
 * 관련된 ftl_rq(전체 요청)/ftl_basic_rq(메타데이터용 단순 요청) 단위의 비동기 I/O 발행과 완료 처리를
 * 책임진다. 또한 GC가 어떤 밴드를 재배치(reloc)할지 선택하고 그 P2L(Physical-to-Logical) 메타데이터를
 * 읽어오는 진입점(ftl_band_get_next_gc)도 여기에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: writer/compaction/GC 모듈(ftl_writer.c, ftl_nv_cache.c, ftl_reloc.c) → 본 파일의
 *  ftl_band_rq_write/read 또는 ftl_band_basic_rq_write/read → SPDK bdev 계층(spdk_bdev_write_blocks)
 *  → bdev_nvme 모듈 → lib/nvme 유저스페이스 NVMe 드라이버 → PCIe NVMe SSD.
 * 실행 컨텍스트: SPDK FTL은 각 디바이스가 단일 spdk_thread(즉 단일 reactor 코어)에 고정되어 동작하며,
 *  본 파일의 모든 함수는 그 디바이스 스레드에서만 호출된다. 따라서 cross-thread 동기화나 락은 없으며,
 *  완료 콜백 또한 같은 스레드의 polling 루프에서 디스패치된다.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(호출자): ftl_writer(사용자 쓰기/relocation 결과 P2L 메타데이터 기록), ftl_reloc(GC 시 데이터를
 *   읽어 새 밴드로 옮김), ftl_mngt(밴드 open/close 상태 천이 관리). 이들은 ftl_rq에 콜백을 설치한 뒤
 *   본 파일의 함수를 호출한다.
 * - 하위(피호출자): SPDK bdev API (spdk_bdev_write_blocks/read_blocks/queue_io_wait, spdk_bdev_free_io),
 *   ftl_md(메타데이터 영역 영속화: ftl_md_persist_entries), ftl_p2l_ckpt(P2L 체크포인트 발행),
 *   ftl_stats(I/O 통계 갱신), ftl_band(밴드 상태 천이/iter 진행).
 * - 공유 자료구조: struct ftl_band(밴드 상태/큐 깊이/메타데이터/p2l_map), struct ftl_rq/ftl_basic_rq
 *   (요청 객체, owner 콜백 보유), struct spdk_ftl_dev(디바이스 단위 통계와 base_bdev_desc/io 채널).
 *
 * === 주요 함수/구조체 요약 ===
 * - ftl_band_rq_write(band, rq): 사용자 데이터 쓰기. compaction/GC 결과를 밴드의 현재 iter 위치에 기록하고
 *   완료 시 P2L 체크포인트를 발행한다. 밴드가 가득 차면 FULL 상태로 천이.
 * - ftl_band_rq_read(band, rq): GC 시 밴드에서 데이터를 읽어 reloc 버퍼로 가져온다.
 * - ftl_band_basic_rq_write/read: 단순(메타데이터용) 요청. 밴드 P2L 맵 등 작은 단위 I/O.
 * - ftl_band_open/close/free: 밴드 라이프사이클 천이. p2l_map의 dma 사본을 준비하고 ftl_md_persist_entries로
 *   밴드 메타데이터 영역에 영속화한 뒤 상태(OPEN/CLOSED/FREE)로 set.
 * - ftl_band_read_tail_brq_md, ftl_band_get_next_gc: 밴드의 tail 메타데이터(P2L map)를 읽어와 검증하고
 *   GC 대상 밴드를 선정한다.
 * - 정적 콜백들(write_rq_end, read_rq_end, write_brq_end, read_brq_end, band_open_cb, band_close_cb,
 *   band_map_write_cb, band_free_cb, read_md_cb, read_tail_md_cb): 각 비동기 I/O의 완료 시점에서
 *   상태를 천이시키고 다음 단계 콜백을 발화하는 핵심 stitching 코드.
 */

/* [한국어] SPDK 표준 헤더 모음 — POSIX/표준 C 헤더의 SPDK 추상화. malloc/string/types 등 기본 의존성을 한 번에 가져온다. */
#include "spdk/stdinc.h"
/* [한국어] SPDK queue.h — sys/queue.h 기반의 STAILQ/LIST/TAILQ 매크로. ftl_rq가 사용하는 list 노드를 위해 포함. */
#include "spdk/queue.h"
/* [한국어] bdev_module 헤더 — spdk_bdev_io_wait_entry, spdk_bdev_queue_io_wait 등 bdev 모듈/사용자가 공유하는 API. */
#include "spdk/bdev_module.h"

/* [한국어] FTL 코어: spdk_ftl_dev, ftl_get_next_seq_id, ftl_abort 등 코어 자료구조와 헬퍼. */
#include "ftl_core.h"
/* [한국어] FTL 밴드 정의: ftl_band, ftl_band_state enum, ftl_band_set_state/iter_advance 등 밴드 헬퍼. */
#include "ftl_band.h"
/* [한국어] FTL 내부 공용: ftl_rq/ftl_basic_rq, ftl_layout_region, FTL_LAYOUT_REGION_TYPE_*, FTL_BLOCK_SIZE 매크로 등. */
#include "ftl_internal.h"

/*
 * [한국어]
 * write_rq_end - ftl_rq(전체 요청) 단위 사용자 데이터 쓰기 완료 콜백.
 *
 * @bdev_io: 완료된 bdev 단계의 I/O 디스크립터. 통계 갱신 후 spdk_bdev_free_io로 반환한다.
 * @success: bdev 계층이 성공/실패를 알려준다(true=정상 완료, false=하드웨어/타임아웃 등 오류).
 * @arg:     queue 시점에 등록한 콜백 인자 — 여기서는 struct ftl_rq * (요청 객체).
 *
 * 동기/배경: ftl_band_rq_write에서 spdk_bdev_write_blocks가 완료되면 SPDK bdev 모듈이 이 콜백을
 * 디바이스 스레드에서 호출한다. 이 시점에서 통계 카운터를 갱신하고, 성공 시에는 P2L 체크포인트를
 * 비휘발성 캐시에 발행해 데이터-메타데이터의 일관성을 보장한다. 실패 시에는 빌드 옵션에 따라 재시도하거나
 * 즉시 abort하여 inconsistent state로의 진행을 차단한다.
 *
 * 호출 체인:
 *   ftl_band_rq_write → ftl_band_rq_bdev_write → spdk_bdev_write_blocks → (NVMe 완료 폴링) → write_rq_end
 *   → 성공: ftl_p2l_ckpt_issue / 실패: rq->owner.cb 또는 ftl_abort
 *
 * 실행 컨텍스트: 디바이스가 고정된 spdk_thread(reactor)의 polling 콜백. 같은 스레드에서만 ftl_rq를
 * 다루므로 별도 동기화는 불필요.
 */
static void
write_rq_end(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	/* [한국어] 콜백 인자에서 ftl_rq를 복원. queue 시점에 spdk_bdev_write_blocks의 cb_arg로 rq를 그대로 전달했기 때문. */
	struct ftl_rq *rq = arg;
	/* [한국어] 통계 갱신과 P2L 체크포인트 발행에 필요한 디바이스 컨텍스트. ftl_rq 생성 시 dev 포인터를 보관해 둔다. */
	struct spdk_ftl_dev *dev = rq->dev;

	/* [한국어] FTL 통계 갱신 — owner.compaction이 true면 compaction(NV 캐시→base) I/O, 아니면 GC reloc 결과 I/O로 분류.
	 *         FTL_STATS_TYPE_CMP/GC 카테고리에 bdev_io의 latency/byte 카운터를 적산한다. */
	ftl_stats_bdev_io_completed(dev, rq->owner.compaction ? FTL_STATS_TYPE_CMP : FTL_STATS_TYPE_GC,
				    bdev_io);
	/* [한국어] bdev_io 디스크립터 반납 — bdev 채널의 mempool로 돌아간다. 이 시점 이후 bdev_io 접근 금지. */
	spdk_bdev_free_io(bdev_io);

	/* [한국어] 요청 객체에 성공 여부를 기록. 후속 콜백 체인이 이 값을 보고 분기한다. */
	rq->success = success;
	/* [한국어] spdk_likely: 분기 예측 힌트(__builtin_expect). 정상 경로(I/O 성공)에 대한 최적화. */
	if (spdk_likely(rq->success)) {
		/* [한국어] 데이터가 base bdev에 성공적으로 안착했으므로 P2L 체크포인트(메타데이터) 발행으로 진행.
		 *         이로써 LBA→PBA 매핑이 비휘발성 캐시에 기록되어 crash recovery 시 재구성 가능. */
		ftl_p2l_ckpt_issue(rq);
	} else {
#ifdef SPDK_FTL_RETRY_ON_ERROR
		/* [한국어] 빌드 옵션 SPDK_FTL_RETRY_ON_ERROR가 켜진 경우의 재시도 경로.
		 *         밴드 큐 깊이가 양수인 것을 확인 후 감소시킨다(이 요청이 가져갔던 슬롯 반납). */
		assert(rq->io.band->queue_depth > 0);
		rq->io.band->queue_depth--;
		/* [한국어] writer 콜백을 호출 — writer는 실패한 rq를 다시 큐잉할지 결정한다. */
		rq->owner.cb(rq);

#else
		/* [한국어] 기본 빌드: 데이터 쓰기 실패는 데이터 손실 위험이 크므로 즉시 abort하여 추가 진행을 막는다. */
		ftl_abort();
#endif
	}
}

/*
 * [한국어]
 * ftl_band_rq_bdev_write - SPDK bdev 계층으로 실제 write 명령을 발행하는 헬퍼.
 *
 * @_rq: 콜백/직접 호출 모두에서 통일된 시그니처를 위해 void* 로 받은 ftl_rq 포인터.
 *
 * 이 함수가 별도 함수로 분리된 이유는 ENOMEM(채널의 bdev_io 풀 고갈) 시
 * spdk_bdev_queue_io_wait()로 자기 자신을 cb로 등록해 추후 재진입(retry)하기 위함이다.
 * SPDK bdev에서 bdev_io 객체는 채널마다 고정 크기 mempool에서 할당되므로, 일시적으로 부족해지면
 * 다른 I/O가 완료되어 객체가 반환될 때까지 기다려야 한다.
 *
 * 호출 체인:
 *   ftl_band_rq_write → ftl_band_rq_bdev_write → spdk_bdev_write_blocks
 *   (ENOMEM 시) → spdk_bdev_queue_io_wait → 추후 ftl_band_rq_bdev_write 재호출
 *
 * 실행 컨텍스트: 디바이스 스레드(폴링 컨텍스트). 동기 호출 또는 wait_entry의 콜백.
 */
static void
ftl_band_rq_bdev_write(void *_rq)
{
	/* [한국어] void*로 받은 인자를 ftl_rq*로 복원. */
	struct ftl_rq *rq = _rq;
	/* [한국어] 이 요청이 향하는 밴드. ftl_band_rq_write에서 rq->io.band에 미리 설정해 둠. */
	struct ftl_band *band = rq->io.band;
	/* [한국어] base bdev 디스크립터/채널을 얻기 위한 디바이스 컨텍스트. */
	struct spdk_ftl_dev *dev = band->dev;
	int rc;

	/* [한국어] base bdev에 비동기 write 발행.
	 *  - dev->base_bdev_desc: 사용자 데이터를 저장하는 base bdev의 open desc.
	 *  - dev->base_ioch:      현재 디바이스 스레드에 바인딩된 bdev I/O 채널(코어 로컬, lockless).
	 *  - rq->io_payload:      DMA 가능한 사용자 버퍼(hugepage 영역). NVMe DMA의 소스가 된다.
	 *  - rq->io.addr:         밴드 내 현재 iter가 가리키는 base bdev LBA (밴드 첫 LBA + offset).
	 *  - rq->num_blocks:      이번에 쓸 블록 수.
	 *  - write_rq_end:        bdev 모듈이 SQE를 NVMe SQ에 제출하고 CQE 폴링으로 완료되면 호출할 콜백.
	 *  - rq:                  콜백 인자로 그대로 전달되어 ftl_rq를 복원하는 데 사용된다. */
	rc = spdk_bdev_write_blocks(dev->base_bdev_desc, dev->base_ioch,
				    rq->io_payload, rq->io.addr, rq->num_blocks,
				    write_rq_end, rq);

	/* [한국어] 비정상 반환은 흔치 않은 분기 — 분기 예측 힌트로 명시. */
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			/* [한국어] bdev_io mempool 고갈. 같은 함수 자신을 콜백으로 등록해 객체가 반환되면 다시 시도한다. */
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
			rq->io.bdev_io_wait.bdev = bdev;
			rq->io.bdev_io_wait.cb_fn = ftl_band_rq_bdev_write;
			rq->io.bdev_io_wait.cb_arg = rq;
			/* [한국어] SPDK bdev에 wait_entry를 등록 — bdev_io가 회수되면 cb_fn이 호출되어 자연스럽게 재시도. */
			spdk_bdev_queue_io_wait(bdev, dev->base_ioch, &rq->io.bdev_io_wait);
		} else {
			/* [한국어] ENOMEM 외 오류(예: -EINVAL, 디바이스 분리)는 회복 불가능 — 즉시 abort. */
			ftl_abort();
		}
	}
}

/*
 * [한국어]
 * ftl_band_rq_write - 사용자/compaction/GC가 만든 ftl_rq를 밴드의 현재 iter 위치에 쓰기.
 *
 * @band: 데이터를 쓸 대상 밴드(상태가 OPEN이어야 한다).
 * @rq:   쓸 데이터를 담은 ftl_rq. 호출자는 owner(콜백)와 io_payload, num_blocks를 미리 채워둔다.
 *
 * 이 함수는 (1) rq에 밴드/주소를 묶어주고, (2) 실제 bdev write를 발행하고, (3) 밴드의 큐 깊이/iter를
 * 진행시키며, (4) 밴드가 가득 차면 FULL 상태로 천이하면서 owner의 state_change_fn을 호출한다.
 * iter 진행은 데이터 발행 직후에 수행되어, 다음 ftl_rq가 현재 위치와 겹치지 않도록 한다(non-overlapping
 * append 보장).
 *
 * 호출 체인:
 *   writer/compaction/reloc → ftl_band_rq_write → ftl_band_rq_bdev_write → spdk_bdev_write_blocks
 *
 * 실행 컨텍스트: 디바이스 스레드. 같은 밴드에 대한 호출은 직렬화되어 있다고 가정.
 */
void
ftl_band_rq_write(struct ftl_band *band, struct ftl_rq *rq)
{
	/* [한국어] 통계 갱신 등에 사용할 디바이스 컨텍스트. */
	struct spdk_ftl_dev *dev = band->dev;

	/* [한국어] 성공 플래그를 false로 초기화 — 완료 콜백에서 success 값을 대입할 때까지 안전한 기본값. */
	rq->success = false;
	/* [한국어] 이 rq가 어느 밴드에 속하는지 기록 — 콜백에서 큐 깊이 감소 등에 사용. */
	rq->io.band = band;
	/* [한국어] 쓰기 시작 LBA = 밴드의 현재 iter 주소. iter는 밴드 시작 LBA + 누적 offset 형태로 관리된다. */
	rq->io.addr = band->md->iter.addr;

	/* [한국어] 실제 bdev write 발행. 내부에서 ENOMEM 시 wait_entry로 재시도된다. */
	ftl_band_rq_bdev_write(rq);

	/* [한국어] 밴드 큐 깊이 증가 — 미완료 I/O 슬롯 카운트. 완료 콜백에서 다시 감소된다. */
	band->queue_depth++;
	/* [한국어] 디바이스 단위 누적 I/O 활동량 통계. 모니터링/Wear leveling 분석에 활용. */
	dev->stats.io_activity_total += rq->num_blocks;

	/* [한국어] 밴드의 iter를 num_blocks만큼 진행 — 다음 append 시작점이 이동한다.
	 *         (실제 데이터는 비동기로 디스크에 쓰이지만, 논리적 append 위치는 즉시 전진해야 다음 발행이 겹치지 않는다.) */
	ftl_band_iter_advance(band, rq->num_blocks);
	/* [한국어] iter offset이 밴드 용량에 도달하면 밴드를 FULL 상태로 표시. */
	if (ftl_band_filled(band, band->md->iter.offset)) {
		ftl_band_set_state(band, FTL_BAND_STATE_FULL);
		/* [한국어] writer/소유자에게 상태 변화 통보 — writer가 새 밴드를 할당하고 close 시퀀스를 트리거하도록. */
		band->owner.state_change_fn(band);
	}
}

/* [한국어] 전방 선언 — read_rq_end에서 실패 재시도용으로 호출하기 위해 미리 선언. */
static void ftl_band_rq_bdev_read(void *_entry);

/*
 * [한국어]
 * read_rq_end - ftl_rq의 entry 단위 읽기 완료 콜백.
 *
 * @bdev_io: 완료된 bdev I/O. 통계 갱신 후 free.
 * @success: 성공/실패. 실패 시 같은 entry를 재발행한다(데이터 누락보다 재시도가 안전).
 * @arg:     queue 시점의 ftl_rq_entry 포인터.
 *
 * 읽기는 GC가 밴드 데이터를 reloc 버퍼로 옮기기 위해 사용된다. 여러 개의 entry가 한 ftl_rq에 모여
 * 있으며, 이 콜백은 entry 단위로 발생한다. 모든 entry 완료 통합은 owner.cb(rq)에서 다뤄진다(여기서는
 * 단일 entry 단위에서 완료/재시도만 수행).
 *
 * 호출 체인:
 *   ftl_band_rq_read → ftl_band_rq_bdev_read → spdk_bdev_read_blocks → read_rq_end → rq->owner.cb
 *
 * 실행 컨텍스트: 디바이스 스레드 polling 콜백.
 */
static void
read_rq_end(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	/* [한국어] cb_arg로 등록했던 entry 포인터 복원. */
	struct ftl_rq_entry *entry = arg;
	/* [한국어] entry가 속한 밴드 — entry->io.band에 ftl_band_rq_read에서 미리 설정. */
	struct ftl_band *band = entry->io.band;
	/* [한국어] entry → 부모 ftl_rq 복원. ftl_rq_from_entry는 container_of 패턴으로 entries 배열에서 부모를 역계산. */
	struct ftl_rq *rq = ftl_rq_from_entry(entry);

	/* [한국어] GC용 읽기로 통계 분류 — 사용자 트래픽과 GC 트래픽을 구분해 write amplification 분석에 활용. */
	ftl_stats_bdev_io_completed(band->dev, FTL_STATS_TYPE_GC, bdev_io);

	/* [한국어] 부모 rq에 성공 플래그 기록. */
	rq->success = success;
	if (spdk_unlikely(!success)) {
		/* [한국어] 읽기 실패 — 같은 entry를 다시 발행하여 재시도. bdev_io는 free 후 새 객체로 발행된다. */
		ftl_band_rq_bdev_read(entry);
		spdk_bdev_free_io(bdev_io);
		return;
	}

	/* [한국어] 정상 완료 — 밴드 큐 깊이 감소. */
	assert(band->queue_depth > 0);
	band->queue_depth--;

	/* [한국어] owner(리로케이터)에게 완료 통보. owner는 entry 묶음의 진행 상태를 추적해 모두 도착하면 다음 단계로. */
	rq->owner.cb(rq);
	/* [한국어] bdev_io 디스크립터 반납 — 콜백 처리가 끝난 뒤 마지막에 free. */
	spdk_bdev_free_io(bdev_io);
}

/*
 * [한국어]
 * ftl_band_rq_bdev_read - entry 단위로 base bdev에 read 발행.
 *
 * @_entry: void*로 통일된 시그니처. ftl_rq_entry 포인터.
 *
 * 읽기는 entry 단위로 발행된다. ftl_rq 안에 여러 entry가 있고, 각 entry가 다른 LBA 범위를 읽을 수 있다.
 * (단편화된 reloc 시 한 번에 모이지 않는 LBA 묶음을 처리하기 위함.)
 *
 * 호출 체인:
 *   ftl_band_rq_read → ftl_band_rq_bdev_read → spdk_bdev_read_blocks
 *   (ENOMEM 시) → spdk_bdev_queue_io_wait → 추후 재진입
 */
static void
ftl_band_rq_bdev_read(void *_entry)
{
	/* [한국어] entry/rq/dev 풀이 — entry가 속한 부모 rq에서 dev를 가져온다. */
	struct ftl_rq_entry *entry = _entry;
	struct ftl_rq *rq = ftl_rq_from_entry(entry);
	struct spdk_ftl_dev *dev = rq->dev;
	int rc;

	/* [한국어] 비동기 read 발행. 인자 의미는 write 경로와 동일하나 대상 LBA/길이는 entry->bdev_io 필드에서 가져온다.
	 *         (entry는 reloc 시 LBA가 비연속일 수 있어 자체 offset/num_blocks 필드를 별도로 보관) */
	rc = spdk_bdev_read_blocks(dev->base_bdev_desc, dev->base_ioch, entry->io_payload,
				   entry->bdev_io.offset_blocks, entry->bdev_io.num_blocks,
				   read_rq_end, entry);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			/* [한국어] bdev_io 풀 고갈 — wait_entry 등록으로 재시도. write 경로와 동일 패턴. */
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
			entry->bdev_io.wait_entry.bdev = bdev;
			entry->bdev_io.wait_entry.cb_fn = ftl_band_rq_bdev_read;
			entry->bdev_io.wait_entry.cb_arg = entry;
			spdk_bdev_queue_io_wait(bdev, dev->base_ioch, &entry->bdev_io.wait_entry);
		} else {
			/* [한국어] 기타 오류 — 회복 불가능, abort. */
			ftl_abort();
		}
	}
}

/*
 * [한국어]
 * ftl_band_rq_read - 밴드에서 ftl_rq 단위로 데이터를 읽는다(GC 시 reloc 입력).
 *
 * @band: 읽을 밴드(닫힌(CLOSED) 또는 FULL 밴드 — GC 대상).
 * @rq:   읽을 묶음. rq->iter.idx/iter.count로 entries 배열의 어느 entry부터 몇 개를 발행할지 지정.
 *
 * 한 번 호출에서 한 entry만 발행한다(현재 코드 흐름 기준). reloc은 여러 LBA를 모아 점진적으로 읽어
 * 채운 뒤 새 밴드에 쓰는 구조이므로, iter.idx를 늘려가며 반복 호출된다.
 */
void
ftl_band_rq_read(struct ftl_band *band, struct ftl_rq *rq)
{
	struct spdk_ftl_dev *dev = band->dev;
	/* [한국어] 현재 iter 인덱스에 해당하는 entry — 이번 호출이 발행할 단일 단위. */
	struct ftl_rq_entry *entry = &rq->entries[rq->iter.idx];

	/* [한국어] 안전성 검사: idx+count가 전체 num_blocks를 넘지 않아야 한다. */
	assert(rq->iter.idx + rq->iter.count <= rq->num_blocks);

	/* [한국어] 성공/밴드/주소를 entry와 rq 양쪽에 설정 — 콜백에서 entry로부터 밴드/rq를 복원할 수 있도록. */
	rq->success = false;
	rq->io.band = band;
	rq->io.addr = band->md->iter.addr;
	entry->io.band = band;
	/* [한국어] entry의 bdev I/O 파라미터 — read_blocks 호출 시 그대로 전달된다. */
	entry->bdev_io.offset_blocks = rq->io.addr;
	entry->bdev_io.num_blocks = rq->iter.count;

	/* [한국어] 실제 read 발행. */
	ftl_band_rq_bdev_read(entry);

	/* [한국어] 통계 갱신과 큐 깊이 증가 — 완료 시 read_rq_end에서 감소된다. */
	dev->stats.io_activity_total += rq->num_blocks;
	band->queue_depth++;
}

/*
 * [한국어]
 * write_brq_end - ftl_basic_rq(메타데이터/단순 요청) 쓰기 완료 콜백.
 *
 * @bdev_io/success/arg: 위 write_rq_end와 동일 패턴.
 *
 * basic_rq는 entry 분할이 없는 단순 형태로, 주로 P2L 맵 등 메타데이터를 한 번에 기록할 때 사용된다.
 * 통계 카테고리는 FTL_STATS_TYPE_MD_BASE(base bdev에 쓰는 메타데이터)로 분류된다.
 */
static void
write_brq_end(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	/* [한국어] arg로 등록된 basic_rq 포인터 복원. */
	struct ftl_basic_rq *brq = arg;
	struct ftl_band *band = brq->io.band;

	/* [한국어] base bdev 메타데이터 카테고리로 통계 누적. */
	ftl_stats_bdev_io_completed(band->dev, FTL_STATS_TYPE_MD_BASE, bdev_io);

	/* [한국어] 성공 여부 저장 — 호출자가 이 값을 보고 다음 단계 분기. */
	brq->success = success;

	/* [한국어] 큐 깊이 감소. */
	assert(band->queue_depth > 0);
	band->queue_depth--;

	/* [한국어] owner 콜백 호출 — band_close의 경우 band_map_write_cb가 등록되어 있다. */
	brq->owner.cb(brq);
	/* [한국어] bdev_io 반납. */
	spdk_bdev_free_io(bdev_io);
}

/*
 * [한국어]
 * ftl_band_brq_bdev_write - basic_rq를 bdev write로 발행.
 *
 * @_brq: ftl_basic_rq 포인터.
 *
 * 인자 구성과 ENOMEM 처리 패턴은 ftl_band_rq_bdev_write와 동일하다. 차이점은 entry 분할이 없고
 * brq->io_payload/io.addr/num_blocks를 그대로 사용한다는 점.
 */
static void
ftl_band_brq_bdev_write(void *_brq)
{
	struct ftl_basic_rq *brq = _brq;
	struct spdk_ftl_dev *dev = brq->dev;
	int rc;

	/* [한국어] 단순 요청 단위로 base bdev에 write — basic_rq는 한 번에 io.addr부터 num_blocks를 기록. */
	rc = spdk_bdev_write_blocks(dev->base_bdev_desc, dev->base_ioch,
				    brq->io_payload, brq->io.addr,
				    brq->num_blocks, write_brq_end, brq);

	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			/* [한국어] mempool 고갈 시 자기 자신을 wait_entry로 등록해 추후 재시도. */
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
			brq->io.bdev_io_wait.bdev = bdev;
			brq->io.bdev_io_wait.cb_fn = ftl_band_brq_bdev_write;
			brq->io.bdev_io_wait.cb_arg = brq;
			spdk_bdev_queue_io_wait(bdev, dev->base_ioch, &brq->io.bdev_io_wait);
		} else {
			ftl_abort();
		}
	}
}

/*
 * [한국어]
 * ftl_band_basic_rq_write - 밴드의 현재 iter 위치에 basic_rq를 쓴다(주로 P2L 맵 영속화에 사용).
 *
 * @band: 대상 밴드.
 * @brq:  쓸 단순 요청. 호출 측에서 io_payload/num_blocks를 채우고 owner.cb를 등록해 둔다.
 *
 * close 절차에서 P2L 맵을 밴드의 tail 영역에 기록할 때 호출된다. iter를 num_blocks만큼 전진시키고,
 * 밴드가 가득 차면 FULL로 천이한다(논리적으로 close가 진행되며 끝까지 쓰는 케이스).
 */
void
ftl_band_basic_rq_write(struct ftl_band *band, struct ftl_basic_rq *brq)
{
	struct spdk_ftl_dev *dev = band->dev;

	/* [한국어] basic_rq에 쓰기 시작 LBA와 밴드 포인터, 성공 플래그 초기값을 설정. */
	brq->io.addr = band->md->iter.addr;
	brq->io.band = band;
	brq->success = false;

	/* [한국어] 실제 bdev write 발행. */
	ftl_band_brq_bdev_write(brq);

	/* [한국어] 통계와 큐 깊이 갱신 — write 후 iter 진행. */
	dev->stats.io_activity_total += brq->num_blocks;
	band->queue_depth++;
	ftl_band_iter_advance(band, brq->num_blocks);
	/* [한국어] FULL 천이 검사 — close 시퀀스에서 P2L 맵 기록이 밴드 끝을 채우는 경우 자연스럽게 FULL이 된다. */
	if (ftl_band_filled(band, band->md->iter.offset)) {
		ftl_band_set_state(band, FTL_BAND_STATE_FULL);
		band->owner.state_change_fn(band);
	}
}

/*
 * [한국어]
 * read_brq_end - basic_rq 읽기 완료 콜백.
 *
 * 패턴은 write_brq_end와 동일. 메타데이터 read 통계를 누적하고 owner.cb를 호출.
 * 실패에 대한 자체 재시도는 없으며(상위 콜백 read_md_cb/read_tail_md_cb가 재발행 결정),
 * 콜백에서 owner.cb가 success=false를 보면 재시도 분기로 진입한다.
 */
static void
read_brq_end(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct ftl_basic_rq *brq = arg;
	struct ftl_band *band = brq->io.band;

	/* [한국어] 메타데이터 read 통계 갱신. */
	ftl_stats_bdev_io_completed(band->dev, FTL_STATS_TYPE_MD_BASE, bdev_io);

	/* [한국어] 성공 여부 기록. */
	brq->success = success;

	/* [한국어] 큐 깊이 감소. */
	assert(band->queue_depth > 0);
	band->queue_depth--;

	/* [한국어] owner 콜백 호출. */
	brq->owner.cb(brq);
	spdk_bdev_free_io(bdev_io);
}

/*
 * [한국어]
 * ftl_band_brq_bdev_read - basic_rq를 bdev read로 발행.
 *
 * 패턴은 ftl_band_brq_bdev_write의 read 버전. ENOMEM 시 wait_entry로 재시도.
 */
static void
ftl_band_brq_bdev_read(void *_brq)
{
	struct ftl_basic_rq *brq = _brq;
	struct spdk_ftl_dev *dev = brq->dev;
	int rc;

	/* [한국어] base bdev에서 brq->io.addr 위치를 num_blocks만큼 읽어 io_payload로 가져온다. */
	rc = spdk_bdev_read_blocks(dev->base_bdev_desc, dev->base_ioch,
				   brq->io_payload, brq->io.addr,
				   brq->num_blocks, read_brq_end, brq);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
			brq->io.bdev_io_wait.bdev = bdev;
			brq->io.bdev_io_wait.cb_fn = ftl_band_brq_bdev_read;
			brq->io.bdev_io_wait.cb_arg = brq;
			spdk_bdev_queue_io_wait(bdev, dev->base_ioch, &brq->io.bdev_io_wait);
		} else {
			ftl_abort();
		}
	}
}

/*
 * [한국어]
 * ftl_band_basic_rq_read - 밴드의 brq->io.addr 위치를 basic_rq로 읽기.
 *
 * @band: 대상 밴드.
 * @brq:  읽을 단순 요청. 호출자가 io.addr/num_blocks/io_payload/owner를 미리 설정.
 *
 * 메타데이터 영속화 영역(P2L 맵 등)을 디스크에서 메모리로 가져올 때 사용된다.
 */
void
ftl_band_basic_rq_read(struct ftl_band *band, struct ftl_basic_rq *brq)
{
	struct spdk_ftl_dev *dev = brq->dev;

	/* [한국어] 콜백에서 사용할 밴드 역참조 보관. */
	brq->io.band = band;

	/* [한국어] 실제 read 발행. */
	ftl_band_brq_bdev_read(brq);

	/* [한국어] 큐 깊이 증가와 통계 누적. */
	brq->io.band->queue_depth++;
	dev->stats.io_activity_total += brq->num_blocks;
}

/*
 * [한국어]
 * band_open_cb - 밴드 OPEN 메타데이터 영속화 완료 콜백.
 *
 * @status: ftl_md_persist_entries 결과 코드 (0=성공).
 * @cb_arg: 콜백 인자, 여기서는 struct ftl_band *.
 *
 * 밴드를 사용 가능 상태(OPEN)로 만들기 위해서는 밴드 메타데이터 영역(FTL_LAYOUT_REGION_TYPE_BAND_MD)에
 * 새 상태를 영속화한 뒤에야 비로소 in-memory 상태를 OPEN으로 천이해야 한다(crash consistency).
 * 영속화 실패 시 빌드 옵션에 따라 재시도하거나 abort한다.
 */
static void
band_open_cb(int status, void *cb_arg)
{
	/* [한국어] 콜백 인자에서 밴드 복원. */
	struct ftl_band *band = cb_arg;

	if (spdk_unlikely(status)) {
#ifdef SPDK_FTL_RETRY_ON_ERROR
		/* [한국어] 메타데이터 영속화 실패 — 동일 컨텍스트(persist_entry_ctx)를 재사용해 재시도. */
		ftl_md_persist_entry_retry(&band->md_persist_entry_ctx);
		return;
#else
		ftl_abort();
#endif
	}

	/* [한국어] 영속화 성공 — 비로소 in-memory 상태를 OPEN으로 천이. 이후 writer가 이 밴드에 append 가능. */
	ftl_band_set_state(band, FTL_BAND_STATE_OPEN);
}

/*
 * [한국어]
 * ftl_band_open - 자유(FREE) 밴드를 사용 가능 상태로 여는 진입점.
 *
 * @band: open할 밴드(상태가 FREE이고 P2L valid block이 0이어야 한다).
 * @type: 이 밴드의 사용 목적(USER, GC 등). ftl_band_set_type가 type을 기록.
 *
 * 절차: (1) 타입 설정 후 OPENING 상태로 천이, (2) p2l_map의 dma 사본(band_dma_md)에 현재 메타데이터를
 * 복사하고 state를 OPEN으로 마킹, (3) band 메타데이터 영역에 entry를 영속화, (4) 완료 콜백
 * (band_open_cb)에서 in-memory 상태를 OPEN으로 천이.
 *
 * dma 사본을 사용하는 이유: SPDK NVMe DMA는 hugepage 등록된 영역에서만 가능하므로, 임시 메타데이터를
 * DMA-safe 버퍼에 복사한 후 ftl_md가 그 버퍼를 그대로 NVMe DMA로 디스크에 기록한다.
 */
void
ftl_band_open(struct ftl_band *band, enum ftl_band_type type)
{
	/* [한국어] 디바이스/메타데이터 영역/region 정보 풀이. */
	struct spdk_ftl_dev *dev = band->dev;
	/* [한국어] 밴드 메타데이터(layout region BAND_MD) 핸들 — 디바이스 layout 초기화 시 등록됨. */
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_BAND_MD];
	/* [한국어] BAND_MD 영역의 region 정의(entry_size 등 포함) — 한 entry가 차지하는 블록 수 등을 알기 위함. */
	struct ftl_layout_region *region = ftl_layout_region_get(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD);
	struct ftl_p2l_map *p2l_map = &band->p2l_map;

	/* [한국어] 밴드 type을 USER/GC/...로 설정 — 통계와 reloc 우선순위 결정에 사용. */
	ftl_band_set_type(band, type);
	/* [한국어] 일단 OPENING 상태로 표시 — 메타데이터 영속화가 끝나기 전까지는 데이터를 쓸 수 없는 임시 상태. */
	ftl_band_set_state(band, FTL_BAND_STATE_OPENING);

	/* [한국어] in-memory band->md를 DMA-safe 사본 band_dma_md로 복사. region->entry_size는 블록 단위, FTL_BLOCK_SIZE는 바이트 변환. */
	memcpy(p2l_map->band_dma_md, band->md, region->entry_size * FTL_BLOCK_SIZE);
	/* [한국어] 사본의 상태 필드를 OPEN으로 마킹 — 디스크에 기록될 값. */
	p2l_map->band_dma_md->state = FTL_BAND_STATE_OPEN;
	/* [한국어] open 시점에는 P2L 맵이 비어 있으므로 체크섬 0. */
	p2l_map->band_dma_md->p2l_map_checksum = 0;

	if (spdk_unlikely(0 != band->p2l_map.num_valid)) {
		/*
		 * This is inconsistent state, a band with valid block,
		 * it could be moved on the free list
		 */
		/* [한국어] 자유 밴드인데 valid block이 남아 있다면 데이터 손실 위험 — assert로 차단하고 abort. */
		assert(false && 0 == band->p2l_map.num_valid);
		ftl_abort();
	}

	/* [한국어] 밴드 메타데이터 영역의 band->id 슬롯 1개에 band_dma_md를 영속화 발행.
	 *         완료 시 band_open_cb가 호출되어 OPEN 상태로 천이. md_persist_entry_ctx는 재시도용 컨텍스트 보관용. */
	ftl_md_persist_entries(md, band->id, 1, p2l_map->band_dma_md, NULL,
			       band_open_cb, band, &band->md_persist_entry_ctx);
}

/*
 * [한국어]
 * band_close_cb - 밴드 CLOSE 메타데이터 영속화 완료 콜백.
 *
 * @status/cb_arg: 영속화 결과와 밴드 포인터.
 *
 * P2L 맵이 디스크에 기록되고 그 체크섬도 BAND_MD 영역의 메타데이터에 기록된 후에야 비로소
 * 밴드를 CLOSED로 표시할 수 있다. checksum을 in-memory band->md에도 동기화하여 이후 read 시 검증 가능.
 */
static void
band_close_cb(int status, void *cb_arg)
{
	struct ftl_band *band = cb_arg;

	if (spdk_unlikely(status)) {
#ifdef SPDK_FTL_RETRY_ON_ERROR
		/* [한국어] 메타데이터 영속화 실패 — 재시도. */
		ftl_md_persist_entry_retry(&band->md_persist_entry_ctx);
		return;
#else
		ftl_abort();
#endif
	}

	/* [한국어] dma 사본에 계산된 P2L 체크섬을 in-memory md에 반영 — 이후 GC가 같은 밴드를 다시 읽을 때 일치 검증에 사용. */
	band->md->p2l_map_checksum = band->p2l_map.band_dma_md->p2l_map_checksum;
	/* [한국어] 비로소 CLOSED 상태로 천이. 이 상태의 밴드는 GC reloc 후보이거나 long-lived 데이터 영역. */
	ftl_band_set_state(band, FTL_BAND_STATE_CLOSED);
}

/*
 * [한국어]
 * band_map_write_cb - P2L 맵 영속화(밴드 tail 메타데이터 쓰기) 완료 콜백.
 *
 * @brq: 직전에 발행된 ftl_basic_rq(P2L 맵 쓰기 요청).
 *
 * close 절차의 단계:
 *  1) ftl_band_close → ftl_basic_rq로 P2L 맵을 밴드 tail 영역에 기록 (bdev write)
 *  2) 본 콜백이 호출됨 — 성공이면 CRC32C 계산 후 BAND_MD 영역에 새 상태(CLOSED)와 체크섬을 영속화
 *  3) band_close_cb에서 in-memory 상태를 CLOSED로 천이
 *
 * P2L 맵 쓰기 자체가 실패하면 빌드 옵션에 따라 같은 brq를 재발행해 retry한다.
 */
static void
band_map_write_cb(struct ftl_basic_rq *brq)
{
	/* [한국어] brq에 묶인 밴드/p2l_map/dev/region/md 풀이. */
	struct ftl_band *band = brq->io.band;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_layout_region *region = ftl_layout_region_get(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD);
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_BAND_MD];
	uint32_t band_map_crc;

	if (spdk_likely(brq->success)) {

		/* [한국어] 밴드 P2L 맵 전체에 대해 CRC32C 계산 — 이 값을 BAND_MD entry에 기록해 두면
		 *         이후 read 시 일치 검증으로 메타데이터 무결성 확인 가능. */
		band_map_crc = spdk_crc32c_update(p2l_map->band_map,
						  ftl_tail_md_num_blocks(dev) * FTL_BLOCK_SIZE, 0);
		/* [한국어] in-memory band->md를 dma 사본으로 복사 후 상태/체크섬을 갱신. */
		memcpy(p2l_map->band_dma_md, band->md, region->entry_size * FTL_BLOCK_SIZE);
		p2l_map->band_dma_md->state = FTL_BAND_STATE_CLOSED;
		p2l_map->band_dma_md->p2l_map_checksum = band_map_crc;

		/* [한국어] BAND_MD 영역에 영속화 발행 — 완료 시 band_close_cb에서 in-memory 상태를 CLOSED로 천이. */
		ftl_md_persist_entries(md, band->id, 1, p2l_map->band_dma_md, NULL,
				       band_close_cb, band, &band->md_persist_entry_ctx);
	} else {
#ifdef SPDK_FTL_RETRY_ON_ERROR
		/* Try to retry in case of failure */
		/* [한국어] P2L 맵 쓰기 실패 — 같은 brq를 재발행해 재시도. queue_depth는 다시 1 증가. */
		ftl_band_brq_bdev_write(brq);
		band->queue_depth++;
#else
		ftl_abort();
#endif
	}
}

/*
 * [한국어]
 * ftl_band_close - 가득 찬 밴드를 닫는 진입점(close 시퀀스 시작).
 *
 * @band: close 대상 밴드(상태가 FULL 또는 OPEN이지만 더 이상 쓰지 않을 밴드).
 *
 * 절차:
 *  1) 새 close_seq_id 발급(이 밴드가 닫힌 시점을 기록 — recovery 시 순서 결정에 사용)
 *  2) CLOSING 상태로 천이
 *  3) basic_rq(metadata_rq)에 P2L 맵 버퍼와 길이 설정
 *  4) ftl_band_basic_rq_write로 P2L 맵을 밴드 tail에 기록 (이 쓰기가 곧 밴드를 채워 FULL → CLOSING 마무리)
 *  5) 완료 시 band_map_write_cb → BAND_MD 영속화 → band_close_cb → CLOSED 상태로 천이
 *
 * 영구 저장 순서가 "P2L 맵 먼저, 그 다음 BAND_MD"인 이유는 P2L 맵 데이터가 안전하게 디스크에 도달한 후에야
 * BAND_MD에 그 체크섬과 CLOSED 상태를 기록할 수 있기 때문(crash 시 BAND_MD가 CLOSED인데 P2L 맵이 깨져
 * 있으면 검증 실패 → 자동 복구 불가).
 */
void
ftl_band_close(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	/* [한국어] P2L 맵 메모리 버퍼 — 밴드 단위 LBA→PBA 매핑 정보를 담고 있다. */
	void *metadata = band->p2l_map.band_map;
	/* [한국어] 밴드 tail 메타데이터 길이(블록 단위) — 디바이스 layout에 의해 결정된다. */
	uint64_t num_blocks = ftl_tail_md_num_blocks(dev);

	/* Write P2L map first, after completion, set the state to close on nvcache, then internally */
	/* [한국어] close 순번 발급 — recovery 시 close 순서로 의존성 그래프를 재구성하기 위해 단조 증가 ID 부여. */
	band->md->close_seq_id = ftl_get_next_seq_id(dev);
	/* [한국어] 즉시 CLOSING 상태로 표시 — writer가 더 이상 이 밴드에 발행하지 않도록 차단. */
	ftl_band_set_state(band, FTL_BAND_STATE_CLOSING);
	/* [한국어] basic_rq 초기화 — 버퍼/길이/dev 포인터 등 기본 필드 세팅. */
	ftl_basic_rq_init(dev, &band->metadata_rq, metadata, num_blocks);
	/* [한국어] 완료 콜백 등록 — band_map_write_cb로 진입해 BAND_MD 영속화로 이어진다. */
	ftl_basic_rq_set_owner(&band->metadata_rq, band_map_write_cb, band);

	/* [한국어] P2L 맵을 밴드 tail에 쓰는 basic_rq를 발행. 비동기 — 콜백 체인으로 close 시퀀스가 진행된다. */
	ftl_band_basic_rq_write(band, &band->metadata_rq);
}

/*
 * [한국어]
 * band_free_cb - 밴드를 FREE 상태로 영속화 완료 콜백.
 *
 * @status/ctx: 영속화 결과와 밴드 포인터.
 *
 * 밴드를 자유 풀로 돌릴 때 BAND_MD에 새 상태(FREE)를 영속화한 후 in-memory p2l_map 자원을 해제하고
 * 상태를 FREE로 천이한다. ref_cnt 검증으로 외부 참조가 모두 해제되었는지 확인.
 */
static void
band_free_cb(int status, void *ctx)
{
	struct ftl_band *band = (struct ftl_band *)ctx;

	if (spdk_unlikely(status)) {
#ifdef SPDK_FTL_RETRY_ON_ERROR
		/* [한국어] 영속화 실패 — 재시도. */
		ftl_md_persist_entry_retry(&band->md_persist_entry_ctx);
		return;
#else
		ftl_abort();
#endif
	}

	/* [한국어] in-memory P2L 맵 메모리 해제 — 자유 밴드는 P2L을 들고 있을 필요가 없음. */
	ftl_band_release_p2l_map(band);
	/* [한국어] 디버그 로그 — 어느 밴드가 free 풀로 돌아가는지 추적. */
	FTL_DEBUGLOG(band->dev, "Band is going to free state. Band id: %u\n", band->id);
	/* [한국어] 비로소 FREE 상태로 천이 — 이후 ftl_band_open이 이 밴드를 다시 선택할 수 있게 됨. */
	ftl_band_set_state(band, FTL_BAND_STATE_FREE);
	/* [한국어] 외부 참조(ref_cnt)가 모두 해제되어 있어야 정상. P2L 참조가 남아 있으면 use-after-free 위험. */
	assert(0 == band->p2l_map.ref_cnt);
}

/*
 * [한국어]
 * ftl_band_free - 밴드를 자유 풀로 반환하는 진입점.
 *
 * @band: free할 밴드(이전에 erase가 완료되어 모든 valid block이 옮겨진 상태여야 한다).
 *
 * 절차: dma 사본에 FREE 상태와 0 체크섬/0 close_seq_id를 기록 후 BAND_MD 영속화 발행.
 * 완료 콜백 band_free_cb에서 in-memory 상태/리소스 정리.
 */
void
ftl_band_free(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_BAND_MD];
	struct ftl_layout_region *region = ftl_layout_region_get(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD);

	/* [한국어] in-memory band->md를 DMA-safe 사본으로 복사 — 디스크에 쓸 데이터를 준비. */
	memcpy(p2l_map->band_dma_md, band->md, region->entry_size * FTL_BLOCK_SIZE);
	/* [한국어] FREE 상태로 마킹. */
	p2l_map->band_dma_md->state = FTL_BAND_STATE_FREE;
	/* [한국어] free 밴드는 close_seq_id/checksum 의미 없음 — 0으로 초기화. */
	p2l_map->band_dma_md->close_seq_id = 0;
	p2l_map->band_dma_md->p2l_map_checksum = 0;

	/* [한국어] BAND_MD 영역에 영속화 발행 — 완료 시 band_free_cb. */
	ftl_md_persist_entries(md, band->id, 1, p2l_map->band_dma_md, NULL,
			       band_free_cb, band, &band->md_persist_entry_ctx);

	/* TODO: The whole band erase code should probably be done here instead */
	/* [한국어] 향후 개선 메모: 현재는 erase 명령 발행이 별도 경로(ftl_band_erase 등)에 흩어져 있어 통합 예정. */
}

/*
 * [한국어]
 * read_md_cb - GC 시작을 위한 P2L 맵 read 완료 콜백.
 *
 * @brq: 방금 완료된 P2L 맵 read 요청.
 *
 * GC 후보로 선택된 밴드의 P2L 맵을 읽어와 메모리에 올리고, 디스크에 저장된 체크섬과 일치하는지
 * 검증한다. 불일치 시 GC 에러 통계를 누적하고 success=false로 owner.ops_fn에 통지.
 *
 * 호출 체인:
 *   ftl_band_get_next_gc → read_md → _read_md → ftl_band_basic_rq_read → read_brq_end → read_md_cb
 */
static void
read_md_cb(struct ftl_basic_rq *brq)
{
	/* [한국어] owner.priv에 저장해 둔 밴드 포인터 복원. */
	struct ftl_band *band = brq->owner.priv;
	struct spdk_ftl_dev *dev = band->dev;
	ftl_band_ops_cb cb;
	uint32_t band_map_crc;
	bool success = true;
	void *priv;

	/* [한국어] GC 진입점이 등록한 콜백/컨텍스트 임시 보관 — 검증 후 호출하기 위함. */
	cb = band->owner.ops_fn;
	priv = band->owner.priv;

	if (!brq->success) {
		/* [한국어] 읽기 실패 — 같은 요청을 다시 발행해 재시도. (영속화 도중의 transient error 대비.) */
		ftl_band_basic_rq_read(band, &band->metadata_rq);
		return;
	}

	/* [한국어] 메모리에 올린 P2L 맵에 대해 CRC32C 계산 후 BAND_MD에 기록된 체크섬과 비교. */
	band_map_crc = spdk_crc32c_update(band->p2l_map.band_map,
					  ftl_tail_md_num_blocks(band->dev) * FTL_BLOCK_SIZE, 0);
	if (band->md->p2l_map_checksum && band->md->p2l_map_checksum != band_map_crc) {
		/* [한국어] 체크섬 불일치 — 메타데이터 손상 가능성. GC를 진행하면 데이터 손실 위험이 있어 실패로 처리. */
		FTL_ERRLOG(dev, "GC error, inconsistent P2L map CRC\n");
		success = false;

		/* [한국어] FTL 통계에 CRC 에러 기록. */
		ftl_stats_crc_error(band->dev, FTL_STATS_TYPE_GC);
	}
	/* [한국어] owner 슬롯 비우기 — 다음 GC 사이클에서 재사용 가능하도록. */
	band->owner.ops_fn = NULL;
	band->owner.priv = NULL;
	/* [한국어] reloc(GC) 모듈에 통보 — 검증 결과(success)에 따라 GC를 진행하거나 다른 밴드를 선택. */
	cb(band, priv, success);
}

/*
 * [한국어]
 * _read_md - GC 대상 밴드의 P2L 맵을 디스크에서 읽기 위한 준비/발행.
 *
 * @band: GC 후보 밴드.
 * @return: 0 성공, -ENOMEM 메모리 부족(P2L 맵 버퍼 할당 실패).
 *
 * P2L 맵 버퍼를 동적 할당한 뒤 basic_rq를 구성해 ftl_band_basic_rq_read로 발행한다.
 * ENOMEM은 일시적 자원 부족이므로 호출자(read_md)에서 spdk_thread_send_msg로 다음 폴링 사이클에 재시도.
 */
static int
_read_md(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_basic_rq *rq = &band->metadata_rq;

	/* [한국어] P2L 맵 메모리 할당 — 디스크에서 읽어올 버퍼. */
	if (ftl_band_alloc_p2l_map(band)) {
		return -ENOMEM;
	}

	/* Read P2L map */
	/* [한국어] basic_rq를 P2L 맵 버퍼/블록 수로 초기화. */
	ftl_basic_rq_init(dev, rq, band->p2l_map.band_map, ftl_p2l_map_num_blocks(dev));
	/* [한국어] 완료 콜백 등록 — read_md_cb로 진입해 체크섬 검증. */
	ftl_basic_rq_set_owner(rq, read_md_cb, band);

	/* [한국어] 읽기 대상: 밴드 메타데이터(tail) 영역의 P2L 맵 시작 LBA. */
	rq->io.band = band;
	rq->io.addr = ftl_band_p2l_map_addr(band);

	/* [한국어] 비동기 읽기 발행. */
	ftl_band_basic_rq_read(band, &band->metadata_rq);

	return 0;
}

/*
 * [한국어]
 * read_md - _read_md 래퍼: ENOMEM 시 자기 자신을 메시지로 재예약.
 *
 * @band: GC 후보 밴드(void* 시그니처는 spdk_thread_send_msg에 직접 넘기기 위함).
 *
 * 같은 spdk_thread에 메시지를 보내 다음 polling 사이클에 재진입한다(자원이 풀려 있을 가능성에 기댐).
 * cross-thread send가 아니라 self-send이므로 단일 스레드 내 deferred execution 패턴.
 */
static void
read_md(void *band)
{
	int rc;

	/* [한국어] 핵심 동작 시도. */
	rc = _read_md(band);
	if (spdk_unlikely(rc)) {
		/* [한국어] -ENOMEM 등 — 같은 스레드에 메시지로 재예약(폴링 큐의 다음 라운드에 처리). */
		spdk_thread_send_msg(spdk_get_thread(), read_md, band);
	}
}

/*
 * [한국어]
 * read_tail_md_cb - 밴드 tail 메타데이터 read 완료 콜백.
 *
 * @brq: 방금 완료된 메타데이터 read 요청.
 *
 * read_md_cb와 비슷한 구조이나 외부에서 ftl_band_read_tail_brq_md를 통해 호출되는 일반 메타데이터 읽기.
 * 실패 시 같은 요청을 재발행하고, 성공 시 등록된 md_fn을 호출하여 결과 통보.
 */
static void
read_tail_md_cb(struct ftl_basic_rq *brq)
{
	struct ftl_band *band = brq->owner.priv;
	enum ftl_md_status status = FTL_MD_IO_FAILURE;
	ftl_band_md_cb cb;
	void *priv;

	if (spdk_unlikely(!brq->success)) {
		/* Retries the read in case of error */
		/* [한국어] 일시적 read 오류 — 같은 요청을 재발행해 retry. */
		ftl_band_basic_rq_read(band, &band->metadata_rq);
		return;
	}

	/* [한국어] 등록된 md 콜백/priv 임시 보관 후 슬롯 비우기. */
	cb = band->owner.md_fn;
	band->owner.md_fn = NULL;

	priv = band->owner.priv;
	band->owner.priv = NULL;

	/* [한국어] 성공 상태로 결과 분류 — 호출자에게 메타데이터 read가 정상 완료됐음을 통보. */
	status = FTL_MD_SUCCESS;

	cb(band, priv, status);
}

/*
 * [한국어]
 * ftl_band_read_tail_brq_md - 밴드의 tail 메타데이터(P2L 맵 등)를 읽는 외부 진입점.
 *
 * @band: 대상 밴드.
 * @cb:   완료 통보 콜백 (성공/실패를 ftl_md_status로 받는다).
 * @cntx: 콜백 인자.
 *
 * recovery/diagnostic 경로에서 밴드의 tail 메타데이터를 비동기로 읽고 싶을 때 사용한다.
 * owner.md_fn/priv 슬롯이 점유되어 있지 않아야 한다(단일 진행 보장).
 */
void
ftl_band_read_tail_brq_md(struct ftl_band *band, ftl_band_md_cb cb, void *cntx)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_basic_rq *rq = &band->metadata_rq;

	/* [한국어] basic_rq 초기화 — 읽을 길이는 tail md 블록 수. */
	ftl_basic_rq_init(dev, rq, band->p2l_map.band_map, ftl_tail_md_num_blocks(dev));
	/* [한국어] 완료 콜백 등록 — read_tail_md_cb. */
	ftl_basic_rq_set_owner(rq, read_tail_md_cb, band);

	/* [한국어] owner 슬롯 점유 확인 — 다른 진행 중인 메타데이터 읽기와 충돌 방지. */
	assert(!band->owner.md_fn);
	assert(!band->owner.priv);
	band->owner.md_fn = cb;
	band->owner.priv = cntx;

	/* [한국어] 읽을 위치는 밴드의 tail md 시작 LBA. */
	rq->io.band = band;
	rq->io.addr = band->tail_md_addr;

	/* [한국어] 비동기 읽기 발행. */
	ftl_band_basic_rq_read(band, &band->metadata_rq);
}

/*
 * [한국어]
 * ftl_band_get_next_gc - GC 대상 밴드를 선정하고 P2L 맵을 비동기 read한다.
 *
 * @dev:  FTL 디바이스 컨텍스트.
 * @cb:   GC 모듈이 등록한 콜백 — (band, ctx, success) 시그니처. P2L 맵 검증까지 마친 후 호출됨.
 * @cntx: 콜백 인자.
 *
 * GC의 진입점. ftl_band_search_next_to_reloc로 후보 밴드(가장 valid block이 적은 닫힌 밴드 등)를
 * 찾고, 없으면 즉시 cb(NULL, false)로 통보. 있으면 owner 슬롯을 점유하고 read_md를 호출해 P2L 맵을
 * 읽어 검증한다.
 */
void
ftl_band_get_next_gc(struct spdk_ftl_dev *dev, ftl_band_ops_cb cb, void *cntx)
{
	/* [한국어] 디스크 사용량/유효 블록 수 등을 기준으로 다음 GC 후보 밴드 선택. */
	struct ftl_band *band = ftl_band_search_next_to_reloc(dev);

	/* if disk is very small, GC start very early that no band is ready for it */
	if (spdk_unlikely(!band)) {
		/* [한국어] 후보가 없는 경우(디스크가 매우 작거나 모든 밴드가 사용 중) — GC 모듈에 NULL/실패로 즉시 통보. */
		cb(NULL, cntx, false);
		return;
	}

	/* Only one owner is allowed */
	/* [한국어] 동시에 한 GC 작업만 진행되어야 함을 보장. queue_depth/owner 슬롯 모두 비어 있어야 한다. */
	assert(!band->queue_depth);
	assert(!band->owner.ops_fn);
	assert(!band->owner.priv);
	band->owner.ops_fn = cb;
	band->owner.priv = cntx;

	/* [한국어] P2L 맵 read 발행 — 메모리 부족 시 자체 재시도(spdk_thread_send_msg)로 처리. */
	read_md(band);
}
