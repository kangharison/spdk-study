/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL I/O 객체 수명 관리 (ftl_io.c)
 *
 * === 파일의 역할 ===
 * SPDK FTL의 I/O 단위 객체(struct ftl_io) 생명 주기 헬퍼들을 제공한다.
 * ftl_io는 bdev_io의 FTL 버전으로, 사용자 read/write/trim 요청을 FTL 내부 단계
 * (L2P pin → 밴드 결정 → 베이스/캐시 디바이스로 디스패치 → 완료)를 거치는 동안
 * 상태(io->pos, iov_pos, iov_off, status, flags 등)를 추적한다. 본 파일은:
 *  - inflight 카운터 증감(ftl_io_inc_req/dec_req)
 *  - iovec 진행/현재 주소 계산(ftl_io_advance/ftl_io_iovec_addr/_len_left)
 *  - 객체 초기화(ftl_io_init)
 *  - 완료 경로(ftl_io_complete) 및 EAGAIN 재스케줄(ftl_io_cb)
 *  - 실패 처리(ftl_io_fail), 재사용 위한 클리어(ftl_io_clear)
 * 등 "I/O 객체 자체"에 대한 연산만 다룬다. 실제 디바이스 디스패치는 별도 모듈 담당.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   bdev_ftl(read/write/unmap 콜백) → ftl_io_init(객체 채움)
 *     → ftl_l2p_pin → 밴드/캐시 결정 → ftl 내부 디스패치 → 완료 시 ftl_io_complete
 *       → ftl_io_cb → spdk_ring_enqueue(ioch->cq) → poller가 사용자 cb 호출
 * 실행 컨텍스트:
 *  - ftl_io_init/advance/...: 코어 스레드.
 *  - ftl_io_cb: 코어 스레드(완료가 코어 스레드로 모이도록 보장).
 *  - 사용자 콜백 자체는 ftl_io_channel의 poller에서 cq를 dequeue한 뒤 실행 — 사용자 채널 스레드.
 *
 * === 타 모듈과의 연결 ===
 * - ftl_io.h: struct ftl_io 정의 및 본 파일이 노출하는 함수 선언.
 * - ftl_l2p.h: ftl_l2p_unpin — PINNED I/O 완료 시 핀 해제.
 * - ftl_band.h: 완료 후 io->band 정리 등.
 * - ftl_io_channel: cq 링과 map_pool — 본 파일이 직접 접근하여 완료 통지.
 * - ftl_trace: I/O 트레이스 ID 부여 및 lba_io_init 이벤트.
 * 데이터 흐름: 사용자 iov[] → ftl_io.iov_pos/iov_off로 한 블록씩 진행 →
 * 베이스/캐시 디바이스 호출 시 ftl_io_iovec_addr로 현재 DMA 버퍼 주소 추출.
 * 공유 자료구조: dev->num_inflight (전 디바이스 inflight 카운터),
 * ioch->cq (lockless 링), ioch->map_pool (mempool).
 *
 * === 주요 함수/구조체 요약 ===
 * - ftl_io_inc_req/dec_req: dev/io의 inflight 카운터 증감.
 * - ftl_io_iovec/get_lba/current_lba: 현재 진행 위치의 iov/LBA 조회.
 * - ftl_io_advance: 처리한 블록 수만큼 io->pos와 iov_pos/iov_off 진행.
 * - ftl_iovec_num_blocks: iov 배열의 총 블록 수 계산(블록 정렬 검사 포함).
 * - ftl_io_iovec_addr/_len_left: 현재 DMA 버퍼 주소와 남은 블록 수.
 * - ftl_io_cb: 내부 완료 콜백(EAGAIN 재스케줄 포함, cq enqueue로 사용자에게 전달).
 * - ftl_io_init: 객체 0초기화 후 LBA/iov/콜백/타입을 설정.
 * - ftl_io_complete_verify: 읽기 완료 시 L2P가 도중에 바뀌었는지 검증.
 * - ftl_io_complete/fail/clear: 완료/실패/재사용 처리.
 */

#include "spdk/stdinc.h" /* [한국어] 표준 C 헤더(memset 등). */
#include "spdk/ftl.h"    /* [한국어] FTL 공개 API (스펙터 형 등). */
#include "spdk/likely.h" /* [한국어] spdk_unlikely — 분기 예측 힌트. */
#include "spdk/util.h"   /* [한국어] 보조 매크로(SPDK_CONTAINEROF 등). */

#include "ftl_io.h"     /* [한국어] struct ftl_io, ftl_io_channel_get_ctx, FTL_IO_* 플래그. */
#include "ftl_core.h"   /* [한국어] struct spdk_ftl_dev, FTL_BLOCK_SIZE, FTL_ADDR_INVALID. */
#include "ftl_band.h"   /* [한국어] 밴드 관련 헬퍼. */
#include "ftl_debug.h"  /* [한국어] 트레이스/디버그 로그. */

/*
 * [한국어]
 * ftl_io_inc_req - 디바이스/IO inflight 카운터 증가
 *
 * @io: 대상 ftl_io. io->dev로 디바이스 참조.
 * @return: 없음.
 *
 * I/O가 베이스/캐시 디바이스에 막 제출(submit)되는 시점에 호출되어,
 * 완료를 기다려야 할 디바이스 단위/IO 단위 요청 수를 1 증가시킨다.
 * dev->num_inflight는 graceful shutdown 시 inflight=0 도달을 polling하는 데 사용.
 * io->req_cnt는 한 ftl_io가 여러 하위 요청으로 쪼개질 때 미완료 분량 추적.
 * 실행 컨텍스트: 코어 스레드 (단일 스레드 → 락 없는 ++ 안전).
 */
void
ftl_io_inc_req(struct ftl_io *io)
{
	io->dev->num_inflight++; /* [한국어] 디바이스 전체 inflight 카운터 증가. shutdown 동기화 용도. */
	io->req_cnt++;            /* [한국어] 이 ftl_io가 발행한 하위 요청 카운터 증가. */
}

/*
 * [한국어]
 * ftl_io_dec_req - 디바이스/IO inflight 카운터 감소
 *
 * @io: 대상 ftl_io.
 * @return: 없음.
 *
 * 각 하위 요청의 완료 콜백에서 호출. assert로 underflow를 즉시 검출한다.
 * 실행 컨텍스트: 코어 스레드.
 */
void
ftl_io_dec_req(struct ftl_io *io)
{
	assert(io->dev->num_inflight > 0); /* [한국어] 짝이 안 맞으면 inc 누락 — 즉시 어설션. */
	assert(io->req_cnt > 0);            /* [한국어] 동일하게 io 단위 무결성 검증. */

	io->dev->num_inflight--;            /* [한국어] 디바이스 inflight 감소. */
	io->req_cnt--;                       /* [한국어] io 단위 inflight 감소. */
}

/*
 * [한국어]
 * ftl_io_iovec - ftl_io에 매달린 iov 배열의 시작 포인터 반환
 *
 * @io: 대상 ftl_io.
 * @return: io->iov 포인터.
 *
 * 단순 헬퍼이지만 함수로 분리하여 향후 iov 스토리지 변경(예: inline 배열)에 유연성 확보.
 * 실행 컨텍스트: 코어 스레드.
 */
struct iovec *
ftl_io_iovec(struct ftl_io *io)
{
	return &io->iov[0]; /* [한국어] io->iov는 사용자가 ftl_io_init으로 넘긴 iov[]를 그대로 가리킴. */
}

/*
 * [한국어]
 * ftl_io_get_lba - I/O 시작 LBA + 오프셋 위치의 LBA 반환
 *
 * @io: 대상 ftl_io.
 * @offset: 시작 LBA로부터의 블록 오프셋.
 * @return: io->lba + offset.
 *
 * FTL는 연속 LBA에 대한 I/O를 가정 → 현재 처리 블록의 LBA 계산은 단순 덧셈.
 * 실행 컨텍스트: 코어 스레드.
 */
uint64_t
ftl_io_get_lba(const struct ftl_io *io, size_t offset)
{
	assert(offset < io->num_blocks); /* [한국어] offset이 I/O 길이 내에 있는지 검증. */
	return io->lba + offset;          /* [한국어] 시작 LBA에 오프셋을 더한 값. */
}

/*
 * [한국어]
 * ftl_io_current_lba - 현재 진행 중(io->pos)인 블록의 LBA 반환
 *
 * @io: 대상 ftl_io.
 * @return: io->lba + io->pos.
 *
 * io->pos는 advance를 통해 매 단계 증가하므로, 이 함수는 매 단계의 "지금 LBA"를 제공.
 * 실행 컨텍스트: 코어 스레드.
 */
uint64_t
ftl_io_current_lba(const struct ftl_io *io)
{
	return ftl_io_get_lba(io, io->pos); /* [한국어] 현재 위치 기반 LBA — 진행 단계마다 호출. */
}

/*
 * [한국어]
 * ftl_io_advance - 처리 완료된 블록 수만큼 io 진행 상태를 갱신
 *
 * @io: 대상 ftl_io.
 * @num_blocks: 이번 단계에서 처리한 블록 수.
 * @return: 없음.
 *
 * io->pos를 단순 증가시키는 것 외에, iov 단계도 함께 갱신해야 한다.
 * iov[]는 여러 버퍼로 쪼개진 사용자 메모리이므로, num_blocks를 흡수해 가며
 * iov_pos(현재 iov 인덱스)와 iov_off(그 iov 안의 블록 오프셋)을 갱신한다.
 * 한 iov를 다 소비하면 iov_pos++, iov_off=0으로 넘어간다.
 * 실행 컨텍스트: 코어 스레드.
 */
void
ftl_io_advance(struct ftl_io *io, size_t num_blocks)
{
	struct iovec *iov = ftl_io_iovec(io);            /* [한국어] iov 배열 시작. */
	size_t iov_blocks, block_left = num_blocks;       /* [한국어] 현재 iov의 블록 수, 미흡수 잔량. */

	io->pos += num_blocks;                            /* [한국어] 전체 진행도 갱신. */

	if (io->iov_cnt == 0) {                           /* [한국어] iov가 없는 I/O(예: 일부 trim)는 여기서 종료. */
		return;
	}

	while (block_left > 0) {                          /* [한국어] 남은 블록을 모두 흡수할 때까지. */
		assert(io->iov_pos < io->iov_cnt);            /* [한국어] iov 범위를 벗어나면 사용자 오류. */
		iov_blocks = iov[io->iov_pos].iov_len / FTL_BLOCK_SIZE; /* [한국어] 현재 iov가 보유한 블록 수. */

		if (io->iov_off + block_left < iov_blocks) {  /* [한국어] 현재 iov 내에서 모두 흡수 가능. */
			io->iov_off += block_left;                /* [한국어] 오프셋만 진행하고 종료. */
			break;
		}

		assert(iov_blocks > io->iov_off);             /* [한국어] iov_off 범위 검증. */
		block_left -= (iov_blocks - io->iov_off);     /* [한국어] 현재 iov의 남은 블록을 모두 소비. */
		io->iov_off = 0;                               /* [한국어] 다음 iov는 처음부터. */
		io->iov_pos++;                                 /* [한국어] 다음 iov로 이동. */
	}
}

/*
 * [한국어]
 * ftl_iovec_num_blocks - iov 배열 전체의 총 블록 수 계산 (정렬 검사 포함)
 *
 * @iov: iov 배열.
 * @iov_cnt: iov 개수.
 * @return: 총 블록 수. 어느 한 iov라도 FTL_BLOCK_SIZE 미정렬이면 0.
 *
 * I/O 진입 시점에 사용자 버퍼 정합성을 검증하는 용도로 사용. 0이면 호출자가
 * 즉시 -EINVAL로 거절.
 * 실행 컨텍스트: 코어 스레드(또는 사용자 스레드 진입 시점).
 */
size_t
ftl_iovec_num_blocks(struct iovec *iov, size_t iov_cnt)
{
	size_t num_blocks = 0, i = 0;        /* [한국어] 누적 블록 수와 인덱스. */

	for (; i < iov_cnt; ++i) {
		if (iov[i].iov_len & (FTL_BLOCK_SIZE - 1)) { /* [한국어] iov_len이 FTL_BLOCK_SIZE의 배수가 아니면 미정렬. */
			return 0;                                /* [한국어] 0은 "유효하지 않음" 신호. */
		}

		num_blocks += iov[i].iov_len / FTL_BLOCK_SIZE; /* [한국어] 누적. */
	}

	return num_blocks;
}

/*
 * [한국어]
 * ftl_io_iovec_addr - 현재 진행 단계의 DMA 버퍼 주소 반환
 *
 * @io: 대상 ftl_io.
 * @return: 현재 iov[iov_pos]의 base + iov_off*FTL_BLOCK_SIZE 위치 포인터.
 *
 * 베이스/캐시 디바이스에 read/write 명령을 발행할 때, "지금 어디 메모리로/에서 DMA할지"
 * 를 결정하기 위해 호출. iov_off 단위는 블록이므로 바이트로 변환한 뒤 base에 가산.
 * 실행 컨텍스트: 코어 스레드.
 */
void *
ftl_io_iovec_addr(struct ftl_io *io)
{
	assert(io->iov_pos < io->iov_cnt); /* [한국어] iov 범위 검증. */
	assert(io->iov_off * FTL_BLOCK_SIZE < ftl_io_iovec(io)[io->iov_pos].iov_len); /* [한국어] 오프셋이 iov_len 안에 있어야 함. */

	return (char *)ftl_io_iovec(io)[io->iov_pos].iov_base +
	       io->iov_off * FTL_BLOCK_SIZE;
	/* [한국어] base + (블록 오프셋 × FTL_BLOCK_SIZE) — 정확한 바이트 위치. */
}

/*
 * [한국어]
 * ftl_io_iovec_len_left - 현재 iov에서 아직 처리되지 않은 블록 수 반환
 *
 * @io: 대상 ftl_io.
 * @return: 현재 iov에서 남은 블록 수. iov_pos가 끝까지 가면 0.
 *
 * 한 단계에서 디바이스에 제출할 수 있는 최대 블록 수를 결정하는 데 사용.
 * 실행 컨텍스트: 코어 스레드.
 */
size_t
ftl_io_iovec_len_left(struct ftl_io *io)
{
	if (io->iov_pos < io->iov_cnt) {
		struct iovec *iov = ftl_io_iovec(io);
		return iov[io->iov_pos].iov_len / FTL_BLOCK_SIZE - io->iov_off;
		/* [한국어] iov 총 블록 수 - 현재 오프셋 = 남은 블록 수. */
	} else {
		return 0; /* [한국어] iov 모두 소진. */
	}
}

/*
 * [한국어]
 * ftl_io_cb - 내부 완료 트램폴린 (EAGAIN 재스케줄 + cq enqueue)
 *
 * @io: 완료된 ftl_io.
 * @arg: io->cb_ctx (현재 미사용 — 시그니처 통일을 위해 인자 유지).
 * @status: 완료 상태. 0 성공, 음수 errno. -EAGAIN은 재시도 신호.
 *
 * 동작:
 *  1) status가 0이 아니면 io->status 보관.
 *  2) -EAGAIN이면: io를 클리어하고 type에 따라 dev의 read/write/trim submission queue
 *     선두에 다시 삽입 → 다음 poller 사이클에 재처리.
 *  3) 재스케줄된 경우 io->status는 0이 되어 추가 처리 없이 종료.
 *  4) 그 외 완료는 io->map(P2L 추적용 임시 맵)이 있으면 mempool로 반환.
 *  5) 채널 cq(spdk_ring) 끝에 io를 enqueue → 사용자 채널 poller가 dequeue 후 user_fn 호출.
 * 실행 컨텍스트: 코어 스레드. cq enqueue는 lockless 링이라 cross-thread 안전.
 *
 * 호출 체인:
 *   ftl_io_complete → [ftl_io_cb] → spdk_ring_enqueue
 *     → (다른 스레드 poller) ftl_io_channel poller → user_fn
 */
static void
ftl_io_cb(struct ftl_io *io, void *arg, int status)
{
	struct ftl_io_channel *ioch = ftl_io_channel_get_ctx(io->ioch); /* [한국어] spdk_io_channel → ftl_io_channel 컨텍스트 변환. */
	size_t result  __attribute__((unused));                           /* [한국어] enqueue 결과 보관 (assert에서만 사용). */

	if (spdk_unlikely(status)) {                /* [한국어] 에러 경로 — 비-0 status. */
		io->status = status;                    /* [한국어] 상태 보관. */

		if (-EAGAIN == status) {                 /* [한국어] 일시적 실패 — 재시도 신호. */
			/* IO has to be rescheduled again */
			/* [한국어] 타입별로 해당 submission queue 선두에 다시 끼워 넣어 즉시 재처리. */
			switch (io->type) {
			case FTL_IO_READ:
				ftl_io_clear(io);                                              /* [한국어] 진행 상태 초기화. */
				TAILQ_INSERT_HEAD(&io->dev->rd_sq, io, queue_entry);          /* [한국어] 읽기 큐 선두 삽입. */
				break;
			case FTL_IO_WRITE:
				ftl_io_clear(io);
				TAILQ_INSERT_HEAD(&io->dev->wr_sq, io, queue_entry);          /* [한국어] 쓰기 큐 선두 삽입. */
				break;
			case FTL_IO_TRIM:
				ftl_io_clear(io);
				TAILQ_INSERT_HEAD(&io->dev->trim_sq, io, queue_entry);        /* [한국어] TRIM 큐 선두 삽입. */
				break;
			default:
				/* Unknown IO type, complete to the user */
				/* [한국어] 알 수 없는 타입은 재스케줄 불가 — 사용자에게 그대로 완료 통지. */
				assert(0);
				break;
			}

		}

		if (!io->status) {
			/* IO rescheduled, return from the function */
			/* [한국어] 재스케줄된 경로에서는 status가 0이 되었으므로 추가 처리 없이 종료. */
			return;
		}
	}

	if (io->map) {                              /* [한국어] 읽기 검증용 임시 P2L 맵이 있으면 풀에 반환. */
		ftl_mempool_put(ioch->map_pool, io->map);
	}

	result = spdk_ring_enqueue(ioch->cq, (void **)&io, 1, NULL);
	/* [한국어] 채널 완료 큐(cq)에 lockless 링으로 io 포인터 enqueue.
	 * cq는 사용자 채널 poller가 dequeue하여 user_fn을 호출 → 완료 콜백이 사용자 스레드에서 실행됨. */
	assert(result != 0); /* [한국어] enqueue는 1개 슬롯이면 충분하므로 항상 성공해야 함. */
}

/*
 * [한국어]
 * ftl_io_init - ftl_io 객체 0초기화 후 사용자 인자 채움
 *
 * @_ioch: spdk_io_channel — ftl_io_channel 컨텍스트로 변환됨.
 * @io: 초기화할 ftl_io 슬롯 (호출자가 미리 할당).
 * @lba: 시작 LBA.
 * @num_blocks: I/O 길이(블록 수).
 * @iov: 사용자 iov 배열 (수명은 호출자 보장).
 * @iov_cnt: iov 개수.
 * @cb_fn: 사용자 완료 콜백.
 * @cb_ctx: 사용자 콜백 컨텍스트.
 * @type: FTL_IO_READ/WRITE/TRIM.
 * @return: 0 성공.
 *
 * 동작:
 *  - memset으로 io 전체 0 초기화 (이전 상태 흔적 제거).
 *  - 사용자 인자(lba/iov/cb 등)를 채우고 trace ID 부여.
 *  - addr는 INVALID로 초기화 — 이후 단계에서 L2P 조회로 채움.
 * 실행 컨텍스트: 코어 스레드 (또는 사용자 진입 스레드).
 */
int
ftl_io_init(struct spdk_io_channel *_ioch, struct ftl_io *io, uint64_t lba, size_t num_blocks,
	    struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_ctx, int type)
{
	struct ftl_io_channel *ioch = ftl_io_channel_get_ctx(_ioch); /* [한국어] 채널 컨텍스트 변환. */
	struct spdk_ftl_dev *dev = ioch->dev;                          /* [한국어] 채널이 가리키는 디바이스. */

	memset(io, 0, sizeof(struct ftl_io));        /* [한국어] 전체 0 초기화 — flags/pos/iov_pos 등 깨끗한 시작. */
	io->ioch = _ioch;                             /* [한국어] 사용자가 넘긴 채널 핸들 보관. */

	io->flags |= FTL_IO_INITIALIZED;              /* [한국어] init 완료 표식 (디버그/검증용). */
	io->type = type;                              /* [한국어] READ/WRITE/TRIM. EAGAIN 재스케줄 분기 키. */
	io->dev = dev;                                /* [한국어] 디바이스 역참조. */
	io->addr = FTL_ADDR_INVALID;                  /* [한국어] 물리 주소는 아직 미정 — L2P 단계에서 결정. */
	io->cb_ctx = cb_ctx;                          /* [한국어] 사용자 cb 컨텍스트. */
	io->lba = lba;                                /* [한국어] 시작 LBA. */
	io->user_fn = cb_fn;                          /* [한국어] 사용자 완료 콜백 (cq dequeue 시 호출). */
	io->iov = iov;                                /* [한국어] 사용자 iov 포인터(수명은 호출자 보장). */
	io->iov_cnt = iov_cnt;                        /* [한국어] iov 개수. */
	io->num_blocks = num_blocks;                  /* [한국어] 총 블록 수. */
	io->trace = ftl_trace_alloc_id(dev);          /* [한국어] 트레이스 ID 부여 (디버그 빌드에서 활용). */

	ftl_trace_lba_io_init(io->dev, io);           /* [한국어] 트레이스 이벤트 — I/O 시작. */
	return 0;                                      /* [한국어] 항상 성공(현 시점 입력 검증 없음). */
}

/*
 * [한국어]
 * ftl_io_complete_verify - 읽기 완료 시 L2P 일관성 재검증
 *
 * @io: 완료된 ftl_io.
 * @return: 없음. 불일치 발견 시 io->status = -EAGAIN으로 설정 → ftl_io_cb가 재스케줄.
 *
 * 사용자 read I/O가 진행되는 도중에 GC/컴팩션이 동일 LBA의 데이터를 다른 위치로
 * 옮길 수 있다. 이 경우 read가 가져온 데이터는 stale일 수 있으므로,
 * I/O 시작 시 io->map[i]에 기록해 둔 ftl_addr와 현재 L2P가 일치하는지 검사한다.
 * 불일치하면 -EAGAIN으로 재시도를 요청한다.
 * 실행 컨텍스트: 코어 스레드.
 *
 * 호출 체인:
 *   ftl_io_complete → [ftl_io_complete_verify] → ftl_l2p_get
 */
static void
ftl_io_complete_verify(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev; /* [한국어] FTL 디바이스. */
	uint64_t i;                          /* [한국어] 블록 인덱스. */
	uint64_t lba = io->lba;              /* [한국어] 진행할 LBA. */

	assert(io->num_blocks <= dev->xfer_size); /* [한국어] 단일 transfer 단위 내에 있는지 검증. */

	if (FTL_IO_WRITE == io->type) {           /* [한국어] 쓰기는 검증 대상 아님 (방금 우리가 쓴 데이터이므로). */
		return;
	}

	if (spdk_unlikely(io->status)) {          /* [한국어] 이미 에러가 있으면 추가 검증 불필요. */
		return;
	}

	for (i = 0; i < io->num_blocks; i++, lba++) {
		ftl_addr current_addr = ftl_l2p_get(dev, lba); /* [한국어] 현재 L2P가 가리키는 주소. */

		/* If user read request gets stuck for whatever reason, then it's possible the LBA
		 * has been relocated by GC or compaction and it may no longer be safe to return data
		 * from that address */
		/* [한국어] 시작 시점에 기록한 io->map[i]와 현재 L2P가 다르면 도중에 위치가 이동된 것 — stale 가능성. */
		if (spdk_unlikely(current_addr != io->map[i])) {
			io->status = -EAGAIN;            /* [한국어] 재시도 요청. ftl_io_cb가 큐에 재삽입. */
			break;                            /* [한국어] 한 블록만 어긋나도 전체 재시도이므로 추가 검사 불필요. */
		}
	}
}

/*
 * [한국어]
 * ftl_io_complete - I/O 완료 처리 진입점
 *
 * @io: 완료된 ftl_io.
 * @return: 없음.
 *
 * 동작:
 *  1) FTL_IO_INITIALIZED 플래그 해제 + done=true.
 *  2) FTL_IO_PINNED(읽기 등 L2P pin이 걸린 I/O)면 검증 후 unpin.
 *  3) ftl_io_cb로 진입 — 재스케줄 또는 사용자 cq enqueue.
 * 실행 컨텍스트: 코어 스레드. 디바이스 완료 콜백 또는 내부 단계 종료에서 호출.
 *
 * 호출 체인:
 *   bdev/nv_cache 완료 콜백 → [ftl_io_complete] → ftl_io_complete_verify
 *     → ftl_l2p_unpin → ftl_io_cb → 사용자 스레드의 user_fn
 */
void
ftl_io_complete(struct ftl_io *io)
{
	io->flags &= ~FTL_IO_INITIALIZED; /* [한국어] 더 이상 활성 I/O가 아님 — 디버그 검증용. */
	io->done = true;                   /* [한국어] 완료 플래그 (poller 내 가시성용). */

	if (io->flags & FTL_IO_PINNED) {   /* [한국어] L2P pin이 걸려 있던 I/O는 검증과 unpin이 필요. */
		ftl_io_complete_verify(io);     /* [한국어] L2P 일관성 검사 — 필요 시 -EAGAIN 설정. */
		ftl_l2p_unpin(io->dev, io->lba, io->num_blocks); /* [한국어] pin 해제 — 페이지가 evict 가능 상태로. */
	}

	ftl_io_cb(io, io->cb_ctx, io->status); /* [한국어] 재스케줄 또는 사용자 cq enqueue. */
}

/*
 * [한국어]
 * ftl_io_fail - I/O 즉시 실패 처리
 *
 * @io: 대상 ftl_io.
 * @status: 실패 코드(음수 errno).
 *
 * 더 이상 진행하지 않고 남은 블록 수만큼 advance해 pos=num_blocks 상태로 만든다.
 * 호출자(상위 디스패처)가 그 후 ftl_io_complete를 호출하여 정상 완료 경로를 탄다.
 * 실행 컨텍스트: 코어 스레드.
 */
void
ftl_io_fail(struct ftl_io *io, int status)
{
	io->status = status;                              /* [한국어] 사용자에게 그대로 전달될 상태 코드. */
	ftl_io_advance(io, io->num_blocks - io->pos);     /* [한국어] 남은 분량을 한 번에 진행해 완료 상태로. */
}

/*
 * [한국어]
 * ftl_io_clear - ftl_io 진행 상태 초기화 (재사용/재시도 시 사용)
 *
 * @io: 대상 ftl_io.
 *
 * EAGAIN 재스케줄 경로에서 호출되어 pos/iov 위치/플래그/밴드 참조를 모두 0으로 되돌린다.
 * 단, lba/num_blocks/iov/사용자 cb 등은 유지되어 동일 I/O를 다시 시도할 수 있게 한다.
 * 실행 컨텍스트: 코어 스레드.
 */
void
ftl_io_clear(struct ftl_io *io)
{
	io->req_cnt = io->pos = io->iov_pos = io->iov_off = 0; /* [한국어] 진행 카운터/위치 모두 리셋. */
	io->done = false;                                       /* [한국어] 완료 표식 해제. */
	io->status = 0;                                          /* [한국어] 이전 에러 코드 폐기. */
	io->flags = 0;                                           /* [한국어] PINNED 등 플래그 모두 해제. */
	io->band = NULL;                                         /* [한국어] 이전에 결정된 밴드 참조 해제 — 재배치 가능. */
}
