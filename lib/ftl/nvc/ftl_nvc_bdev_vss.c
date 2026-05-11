/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

/*
 * [한국어 설명] VSS NVC bdev 디바이스 모델 (ftl_nvc_bdev_vss.c)
 *
 * === 파일의 역할 ===
 * VSS(vector spare storage)를 지원하는 bdev — 즉 한 LBA마다 별도의 메타데이터 슬롯이
 * 마련된 디바이스(ftl_md_vss 크기) — 를 NVC로 사용하는 모델 구현이다.
 * 사용자 데이터와 메타데이터를 한 IO로 묶어 spdk_bdev_writev_blocks_with_md 로 함께
 * 기록하기 때문에 별도의 P2L 로그 영역이 필요 없고, 메타데이터의 atomicity가 데이터와
 * 함께 보장된다. 복구 시에도 데이터 영역을 read하면서 with_md로 같이 가져온 메타로
 * P2L 맵을 그대로 재구성한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (사용자 쓰기):
 *   ftl_nv_cache_submit_user_write
 *     → nvc_type->ops.write = write_io
 *       → ftl_nv_cache_fill_md (LBA/seq_id 등 채움)
 *       → spdk_bdev_writev_blocks_with_md (데이터+메타 함께 발사)
 *       → write_io_cb → ftl_nv_cache_write_complete
 * 호출 체인 (open chunk 복구):
 *   ftl_mngt 복구 → ops.recover_open_chunk → desc_recover_open_chunk
 *     → nvc_recover_open_chunk_read_vss (read_with_md 반복)
 *     → 각 응답에서 md->nv_cache.lba/seq_id로 P2L 맵 재구성
 *
 * === 타 모듈과의 연결 ===
 * - lib/ftl/nvc/ftl_nvc_dev.c        : 디스크립터 등록 시스템
 * - lib/ftl/nvc/ftl_nvc_bdev_common.c: layout helper 공유
 * - lib/ftl/ftl_nv_cache.c           : write_complete/fill_md/chunk_set_addr 등 코어 콜백
 * - include/spdk_internal/ftl_md.h   : union ftl_md_vss 정의
 * - lib/bdev/                        : spdk_bdev_*_with_md API
 *
 * === 주요 함수/구조체 요약 ===
 * - is_bdev_compatible       : bdev가 separate-md / md_size==sizeof(ftl_md_vss) 인지 검사
 * - write_io / write_io_cb   : 데이터+메타 1단계 비동기 쓰기
 * - nvc_recover_open_chunk_* : open chunk를 데이터+메타 read로 복구하는 mngt 절차
 * - struct nvc_recover_open_chunk_ctx : 복구 컨텍스트
 * - struct nvc_bdev_vss      : 디스크립터 (FTL_NV_CACHE_DEVICE_TYPE_REGISTER)
 */

#include "ftl_nvc_dev.h"                       /* [한국어] NVC 디스크립터/ops */
#include "ftl_core.h"                          /* [한국어] spdk_ftl_dev */
#include "ftl_layout.h"                        /* [한국어] layout region */
#include "utils/ftl_layout_tracker_bdev.h"     /* [한국어] layout tracker */
#include "mngt/ftl_mngt.h"                     /* [한국어] 관리 절차 (복구) */
#include "ftl_nvc_bdev_common.h"               /* [한국어] 공통 layout helper */

/*
 * [한국어]
 * is_bdev_compatible - 후보 bdev가 VSS 모델로 사용 가능한지 검사
 *
 * @dev:  FTL 디바이스
 * @bdev: 검사 대상 bdev
 * @return: 모든 조건을 만족하면 true
 *
 * 조건:
 * 1) is_md_separate=true: VSS는 데이터 버퍼와 분리된 별도 metadata 버퍼를 받음
 * 2) md_size == sizeof(ftl_md_vss): FTL 정의 메타 구조체와 정확히 일치해야 함
 * 3) DIF 비활성: FTL은 DIF를 직접 다루지 않음 — DIF 활성 bdev은 거부
 * 4) zero buffer 충분: 한 번의 메타 전송 분량이 공용 zero buffer 안에 들어가야 함
 *    (정렬/제로 패딩 시 사용)
 */
static bool
is_bdev_compatible(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	if (!spdk_bdev_is_md_separate(bdev)) {             /* [한국어] separate-md 미지원 → 거부 */
		/* It doesn't support separate metadata buffer IO */
		return false;
	}

	if (spdk_bdev_get_md_size(bdev) != sizeof(union ftl_md_vss)) { /* [한국어] 사이즈 불일치 거부 */
		/* Bdev's metadata is invalid size */
		return false;
	}

	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) { /* [한국어] DIF 사용 시 거부 */
		/* Unsupported DIF type used by bdev */
		return false;
	}

	/* [한국어] 한 번의 transfer가 zero 패딩 버퍼에 들어가는지 — 너무 큰 transfer는 거부. */
	if (ftl_md_xfer_blocks(dev) * spdk_bdev_get_md_size(bdev) > FTL_ZERO_BUFFER_SIZE) {
		FTL_ERRLOG(dev, "Zero buffer too small for bdev %s metadata transfer\n",
			   spdk_bdev_get_name(bdev));
		return false;
	}

	return true;
}

/*
 * [한국어]
 * write_io_cb - 데이터+메타 쓰기 완료 콜백
 *
 * @bdev_io: 완료된 SPDK bdev IO
 * @success: 성공 여부
 * @cb_arg:  ftl_io* (write_io에서 등록)
 *
 * VSS 모델은 데이터+메타가 한 IO이므로 이 콜백 한 번이면 사용자 쓰기가 완전히
 * 영속화된 것. 통계 갱신 → bdev_io 해제 → 메타 버퍼를 풀에 반환 → 사용자 완료 보고.
 *
 * 실행 컨텍스트: bdev 완료 콜백 — 발사 스레드와 동일.
 */
static void
write_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_io *io = cb_arg;                        /* [한국어] cb_arg → ftl_io 복원 */
	struct ftl_nv_cache *nv_cache = &io->dev->nv_cache;/* [한국어] NVC 컨텍스트 */

	ftl_stats_bdev_io_completed(io->dev, FTL_STATS_TYPE_USER, bdev_io); /* [한국어] 통계 갱신 */

	spdk_bdev_free_io(bdev_io);                        /* [한국어] bdev_io 해제 */

	ftl_mempool_put(nv_cache->md_pool, io->md);        /* [한국어] 메타 버퍼 풀에 반환 */

	ftl_nv_cache_write_complete(io, success);          /* [한국어] 사용자 완료 통지 */
}

/* [한국어] 전방 선언 — _nvc_vss_write이 호출하므로 사전 선언 필요. */
static void write_io(struct ftl_io *io);

/*
 * [한국어]
 * _nvc_vss_write - bdev_io_wait queue가 깨우는 retry trampoline
 *
 * @io: void*로 넘어온 ftl_io* — write_io를 다시 호출하는 어댑터.
 */
static void
_nvc_vss_write(void *io)
{
	write_io(io);                                      /* [한국어] retry */
}

/*
 * [한국어]
 * write_io - VSS 모델 사용자 쓰기 발사 (ops.write 콜백)
 *
 * @io: FTL 사용자 쓰기 IO
 *
 * 단계:
 * 1) 메타 버퍼를 풀(md_pool)에서 획득
 * 2) ftl_nv_cache_fill_md로 LBA/seq_id 등 메타 필드 채움
 * 3) spdk_bdev_writev_blocks_with_md로 데이터+메타 동시 발사
 * 4) ENOMEM 시 메타 버퍼를 즉시 반환하고 bdev_io_wait queue에 등록
 *
 * 핫 패스 — 매 사용자 쓰기마다 호출.
 *
 * 호출 체인:
 *   nvc_type->ops.write = write_io
 *     → spdk_bdev_writev_blocks_with_md(... write_io_cb)
 *     → 완료: write_io_cb → ftl_nv_cache_write_complete
 */
static void
write_io(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;                /* [한국어] FTL 디바이스 */
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;    /* [한국어] NVC 컨텍스트 */
	int rc;                                            /* [한국어] bdev 발사 결과 */

	io->md = ftl_mempool_get(dev->nv_cache.md_pool);   /* [한국어] 메타 버퍼 획득 (DMA-가능) */
	if (spdk_unlikely(!io->md)) {
		ftl_abort();                               /* [한국어] 풀 고갈은 핫패스 위반 — abort */
	}

	ftl_nv_cache_fill_md(io);                          /* [한국어] LBA/seq_id 등 메타 채움 */

	/* [한국어] 데이터+메타를 한 IO로 발사 — VSS 모델의 핵심 이점. */
	rc = spdk_bdev_writev_blocks_with_md(nv_cache->bdev_desc, nv_cache->cache_ioch,
					     io->iov, io->iov_cnt, io->md,
					     ftl_addr_to_nvc_offset(dev, io->addr), io->num_blocks,
					     write_io_cb, io);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {                       /* [한국어] bdev_io 풀 고갈 — 큐 등록 후 재시도 */
			struct spdk_bdev *bdev;

			ftl_mempool_put(nv_cache->md_pool, io->md); /* [한국어] 메타 버퍼 즉시 반환 */
			io->md = NULL;                     /* [한국어] retry 시 다시 잡도록 NULL */

			bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);
			io->bdev_io_wait.bdev = bdev;      /* [한국어] 어느 bdev에서 기다릴지 */
			io->bdev_io_wait.cb_fn = _nvc_vss_write; /* [한국어] retry trampoline */
			io->bdev_io_wait.cb_arg = io;
			spdk_bdev_queue_io_wait(bdev, nv_cache->cache_ioch, &io->bdev_io_wait);
		} else {
			ftl_abort();                       /* [한국어] 그 외 에러 — 복구 불가 */
		}
	}
}

/* [한국어] open chunk 복구 컨텍스트 (VSS 모델 전용).
 * VSS는 데이터+메타를 같이 read하면 P2L가 그대로 복원되므로, 한 chunk를 여러 번에
 * 나눠 read하면서 진행 상태를 이 컨텍스트에 누적한다. */
struct nvc_recover_open_chunk_ctx {
	struct ftl_nv_cache_chunk *chunk;
	/* [한국어] 복구 대상 chunk.
	 * 설정자: nvc_recover_open_chunk_init_handler
	 * 읽는 자: read_vss / read_vss_cb */

	struct ftl_rq *rq;
	/* [한국어] 한 번 read에 사용할 request — 데이터 payload + metadata payload 보유.
	 * 설정자: init_handler에서 ftl_rq_new(md_size 포함) 할당
	 * 해제자: deinit_handler에서 ftl_rq_del */

	uint64_t addr;
	/* [한국어] 다음 read 시작 NVC 블록 오프셋. read 완료마다 blocks 만큼 전진. */

	uint64_t to_read;
	/* [한국어] 남은 read 블록 수 (chunk의 tail 메타 영역 직전까지).
	 * 0이 되면 read 단계 완료 → ftl_mngt_next_step. */
};

/*
 * [한국어]
 * nvc_recover_open_chunk_read_vss_cb - 분할 read 1건 완료 콜백
 *
 * 응답에 함께 들어온 metadata 슬롯(io_md)을 한 블록씩 검사하면서 P2L 맵을 복구한다.
 * - md->nv_cache.seq_id가 chunk seq_id와 다르면 (= 옛 데이터 잔재) LBA를 INVALID 처리
 * - 같으면 그대로 (LBA, addr+i) 매핑을 in-memory P2L에 기록
 *
 * 진행 상황을 컨텍스트(addr/to_read)에 반영하고 같은 단계를 다시 실행해 다음 분할
 * read를 트리거한다 (ftl_mngt_continue_step).
 */
static void
nvc_recover_open_chunk_read_vss_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_mngt_process *mngt = cb_arg;            /* [한국어] mngt 핸들 복원 */
	struct spdk_ftl_dev *dev = ftl_mngt_get_dev(mngt); /* [한국어] FTL 디바이스 */
	struct nvc_recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt); /* [한국어] 사용자 컨텍스트 */
	struct ftl_nv_cache_chunk *chunk = ctx->chunk;     /* [한국어] 복구 대상 chunk */
	struct ftl_rq *rq = ctx->rq;                       /* [한국어] read에 사용한 request */
	union ftl_md_vss *md;                              /* [한국어] 블록별 메타 슬롯 */
	uint64_t cache_offset = bdev_io->u.bdev.offset_blocks; /* [한국어] read한 NVC 시작 오프셋 */
	uint64_t blocks = bdev_io->u.bdev.num_blocks;      /* [한국어] read한 블록 수 */
	ftl_addr addr = ftl_addr_from_nvc_offset(dev, cache_offset); /* [한국어] NVC 오프셋 → ftl_addr */

	spdk_bdev_free_io(bdev_io);                        /* [한국어] bdev_io 해제 */
	if (!success) {                                    /* [한국어] read 실패 → 단계 fail */
		ftl_mngt_fail_step(mngt);
		return;
	}

	/* Rebuild P2L map */
	/* [한국어] 응답 블록을 한 칸씩 순회하며 P2L 재구성. */
	for (rq->iter.idx = 0; rq->iter.idx < blocks; rq->iter.idx++) {
		md = rq->entries[rq->iter.idx].io_md;      /* [한국어] 이 블록의 메타 슬롯 */
		/* [한국어] 같은 chunk의 현재 seq_id가 아니면 옛 데이터(또는 garbage) — 무효화. */
		if (md->nv_cache.seq_id != chunk->md->seq_id) {
			md->nv_cache.lba = FTL_LBA_INVALID;
			md->nv_cache.seq_id = 0;
		}

		/* [한국어] (LBA → addr+i) 매핑을 in-memory P2L에 기록. INVALID LBA는 무시 처리. */
		ftl_nv_cache_chunk_set_addr(chunk, md->nv_cache.lba, addr + rq->iter.idx);
	}

	assert(ctx->to_read >= blocks);                    /* [한국어] 진행 상태 무결성 */
	ctx->addr += blocks;                               /* [한국어] 다음 read 시작 위치 전진 */
	ctx->to_read -= blocks;                            /* [한국어] 남은 양 감소 */
	ftl_mngt_continue_step(mngt);                      /* [한국어] 같은 단계 다시 실행 → 다음 분할 read */

}

/*
 * [한국어]
 * nvc_recover_open_chunk_read_vss - chunk의 다음 분할을 read (mngt step action)
 *
 * 남은 to_read와 rq 용량 중 작은 값을 한 번에 read한다. 0이면 단계 완료.
 */
static void
nvc_recover_open_chunk_read_vss(struct spdk_ftl_dev *dev,
				struct ftl_mngt_process *mngt)
{
	struct nvc_recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	uint64_t blocks = spdk_min(ctx->rq->num_blocks, ctx->to_read); /* [한국어] 이번 read 분량 */
	int rc;

	if (blocks) {
		/* [한국어] 데이터+메타 동시 read — VSS 모델의 복구 핵심. */
		rc = spdk_bdev_read_blocks_with_md(dev->nv_cache.bdev_desc, dev->nv_cache.cache_ioch,
						   ctx->rq->io_payload, ctx->rq->io_md, ctx->addr, blocks,
						   nvc_recover_open_chunk_read_vss_cb, mngt);
		if (rc) {
			ftl_mngt_fail_step(mngt);          /* [한국어] 발사 실패 → 단계 실패 */
			return;
		}
	} else {
		ftl_mngt_next_step(mngt);                  /* [한국어] 더 read할 것 없음 — 다음 단계로 */
	}
}

/*
 * [한국어]
 * nvc_recover_open_chunk_init_handler - 복구 mngt 컨텍스트 초기화
 *
 * @init_ctx: 복구 대상 chunk 포인터 (recover_open_chunk가 전달)
 * @return: 0 성공, -ENOMEM 실패
 *
 * read에 사용할 ftl_rq를 할당하고, 복구 시작 주소(chunk->offset)와 read해야 할 길이
 * (chunk_tail_md_offset = chunk 데이터 영역 길이)를 컨텍스트에 세팅한다.
 */
static int
nvc_recover_open_chunk_init_handler(struct spdk_ftl_dev *dev,
				    struct ftl_mngt_process *mngt, void *init_ctx)
{
	struct nvc_recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);

	ctx->chunk = init_ctx;                             /* [한국어] 대상 chunk 저장 */
	ctx->rq = ftl_rq_new(dev, dev->nv_cache.md_size);  /* [한국어] read용 rq 할당 (md 동반) */
	if (NULL == ctx->rq) {
		return -ENOMEM;                            /* [한국어] 자원 부족 */
	}

	ctx->addr = ctx->chunk->offset;                    /* [한국어] chunk 시작 주소부터 read */
	ctx->to_read = chunk_tail_md_offset(&dev->nv_cache); /* [한국어] tail 메타 영역 직전까지 */

	return 0;
}

/*
 * [한국어]
 * nvc_recover_open_chunk_deinit_handler - 복구 컨텍스트 해제
 *
 * init에서 할당한 rq 해제. mngt 프로세스가 끝날 때 자동 호출됨.
 */
static void
nvc_recover_open_chunk_deinit_handler(struct spdk_ftl_dev *dev,
				      struct ftl_mngt_process *mngt)
{
	struct nvc_recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);

	ftl_rq_del(ctx->rq);                               /* [한국어] rq 해제 */
}

/* [한국어] open chunk 복구 mngt 프로세스 정의.
 * read_vss 단계는 ftl_mngt_continue_step로 자기 자신을 반복 호출하면서 chunk 전체를
 * 다 읽을 때까지 진행한다. */
static const struct ftl_mngt_process_desc desc_recover_open_chunk = {
	.name = "Recover open chunk",
	.ctx_size = sizeof(struct nvc_recover_open_chunk_ctx),
	.init_handler = nvc_recover_open_chunk_init_handler,
	.deinit_handler = nvc_recover_open_chunk_deinit_handler,
	.steps = {
		{
			.name = "Chunk recovery, read vss",
			.action = nvc_recover_open_chunk_read_vss
		},
		{}                                         /* [한국어] terminator */
	}
};

/*
 * [한국어]
 * nvc_recover_open_chunk - 외부 진입점 (ops.recover_open_chunk)
 *
 * 부모 mngt 안에서 desc_recover_open_chunk 자식 프로세스를 시작한다.
 */
static void
nvc_recover_open_chunk(struct spdk_ftl_dev *dev,
		       struct ftl_mngt_process *mngt,
		       struct ftl_nv_cache_chunk *chunk)
{
	ftl_mngt_call_process(mngt, &desc_recover_open_chunk, chunk);
}

/* [한국어] VSS NVC 모델 정적 디스크립터.
 * 이름 "bdev" — 일반적인 ftl bdev 매칭 시 우선 검사된다.
 * VSS 모델은 P2L 로그가 필요 없으므로 init/deinit/on_chunk_x/process/setup_layout 콜백 미정의. */
struct ftl_nv_cache_device_type nvc_bdev_vss = {
	.name = "bdev",
	.features = {
	},
	.ops = {
		.is_bdev_compatible = is_bdev_compatible,
		.is_chunk_active = ftl_nvc_bdev_common_is_chunk_active, /* [한국어] 공통 */
		.md_layout_ops = {
			.region_create = ftl_nvc_bdev_common_region_create, /* [한국어] 공통 */
			.region_open = ftl_nvc_bdev_common_region_open,     /* [한국어] 공통 */
		},
		.write = write_io,
		.recover_open_chunk = nvc_recover_open_chunk,
	}
};
/* [한국어] 컨스트럭터 자동 등록. */
FTL_NV_CACHE_DEVICE_TYPE_REGISTER(nvc_bdev_vss)
