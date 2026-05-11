/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

/*
 * [한국어 설명] non-VSS NVC bdev 디바이스 모델 (ftl_nvc_bdev_non_vss.c)
 *
 * === 파일의 역할 ===
 * VSS(vector spare storage) 메타데이터를 지원하지 않는 일반 SPDK bdev를 NVC로 사용하기
 * 위한 NVC 디바이스 모델 구현이다. VSS가 없으므로 사용자 데이터와 짝이 되는 메타데이터
 * (LBA, seq_id 등)를 별도의 P2L(Physical-to-Logical) 로그 영역에 기록한다.
 * 즉, 한 번의 사용자 쓰기는 (1) 데이터 블록 쓰기 → (2) P2L 로그 append라는 2단계로
 * 구성되며, P2L 로그 콜백(p2l_log_cb)이 두 단계가 모두 끝났을 때 사용자 완료를 보고한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (사용자 쓰기):
 *   ftl_nv_cache_submit_user_write
 *     → nvc_type->ops.write = write_io
 *       → spdk_bdev_writev_blocks (데이터)
 *         → write_io_cb
 *           → ftl_p2l_log_io (메타데이터 append, 비동기)
 *             → p2l_log_cb → ftl_nv_cache_write_complete(io, true)
 * 호출 체인 (chunk open/close):
 *   ftl_nv_cache_chunk_open → on_chunk_open → ftl_p2l_log_acquire (slot 잡기)
 *   ftl_nv_cache_chunk_close → on_chunk_closed → ftl_p2l_log_release
 * 호출 체인 (복구):
 *   ftl_mngt 복구 단계 → recover_open_chunk → ftl_p2l_log_read 로 P2L 재구성
 *
 * === 타 모듈과의 연결 ===
 * - lib/ftl/ftl_p2l_log.c            : P2L 로그 풀/슬롯/IO/replay 구현
 * - lib/ftl/nvc/ftl_nvc_dev.c        : 디스크립터 등록 시스템
 * - lib/ftl/nvc/ftl_nvc_bdev_common.c: layout helper 공유
 * - lib/ftl/ftl_nv_cache.c           : write_complete/chunk_set_addr 등 코어 콜백
 * - lib/bdev/                        : spdk_bdev_writev_blocks API
 *
 * === 주요 함수/구조체 요약 ===
 * - init/deinit                       : P2L 로그 풀 생성/해제
 * - is_bdev_compatible                : md_size==0 인 일반 bdev만 허용
 * - on_chunk_open/closed              : 각 chunk에 P2L 로그 슬롯 바인딩
 * - write_io / write_io_cb / p2l_log_cb: 데이터+P2L 2단계 비동기 쓰기 체인
 * - process                           : poller에서 P2L flush 트리거
 * - recovery_chunk_*                  : 복구 시 P2L 로그 재생 절차
 * - setup_layout                      : P2L log 영역들을 superblock에 예약
 * - struct nvc_bdev_non_vss           : 디스크립터 (FTL_NV_CACHE_DEVICE_TYPE_REGISTER)
 */

#include "ftl_nvc_dev.h"           /* [한국어] NVC 디스크립터/ops 정의 */
#include "ftl_core.h"              /* [한국어] spdk_ftl_dev 본체 */
#include "ftl_layout.h"            /* [한국어] layout region 정의 */
#include "ftl_nv_cache.h"          /* [한국어] ftl_nv_cache_write_complete 등 코어 콜백 */
#include "mngt/ftl_mngt.h"         /* [한국어] 관리 절차 (복구) 헬퍼 */
#include "ftl_nvc_bdev_common.h"   /* [한국어] 공통 layout/region 함수 */

/* [한국어] 전방 선언 — write_io는 retry 콜백/p2l_log_cb 정의 순서 때문에 사전 선언 필요. */
static void write_io(struct ftl_io *io);
static void p2l_log_cb(struct ftl_io *io);

/*
 * [한국어]
 * init - non-VSS NVC 모델 초기화 (ops.init 콜백)
 *
 * @dev: FTL 디바이스
 * @return: 항상 0 (현재 ftl_p2l_log_init 실패도 무시 — 주의!)
 *
 * P2L 로그 풀(여러 chunk가 공유하는 메타데이터 append 로그 자원)을 만든다.
 * 주의: 현재 코드는 ftl_p2l_log_init 실패 시에도 0을 반환해 호출자가 실패를 인지하지
 * 못한다. 이는 이후 P2L 사용 시점에 실패하도록 의도한 것으로 보인다.
 *
 * 실행 컨텍스트: FTL 초기화 ftl_mngt 단계 (단일 스레드).
 *
 * 호출 체인:
 *   ftl_nv_cache_init → nvc_type->ops.init = init → ftl_p2l_log_init()
 */
static int
init(struct spdk_ftl_dev *dev)
{
	int rc;                                            /* [한국어] 하위 호출 결과 */

	rc = ftl_p2l_log_init(dev);                        /* [한국어] P2L 로그 풀 생성 */
	if (rc) {
		return 0;                                  /* [한국어] (의도적) 실패 무시, 0 반환 */
	}

	return 0;
}

/*
 * [한국어]
 * deinit - non-VSS NVC 모델 해제 (ops.deinit 콜백)
 *
 * 종료 시 init이 만든 P2L 로그 풀을 정리한다. 단일 스레드 종료 절차에서 호출.
 */
static void
deinit(struct spdk_ftl_dev *dev)
{
	ftl_p2l_log_deinit(dev);                           /* [한국어] P2L 로그 풀 해제 */
}

/*
 * [한국어]
 * is_bdev_compatible - 후보 bdev가 non-VSS 모델로 사용 가능한지 판정
 *
 * @dev:  FTL 디바이스
 * @bdev: 검사 대상 bdev
 * @return: md_size == 0(메타데이터 없음)이면 true
 *
 * "메타데이터 없음" = VSS 미지원 = 일반 NAND/SSD 라는 가정으로 분류한다.
 * VSS bdev은 따로 ftl_nvc_bdev_vss.c가 매칭한다 (등록 순서가 우선순위 결정).
 */
static bool
is_bdev_compatible(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	if (spdk_bdev_get_md_size(bdev) != 0) {            /* [한국어] 별도 메타 영역이 있는 bdev은 거부 */
		/* Bdev's metadata is invalid size */
		return false;
	}

	return true;                                       /* [한국어] md 없음 → non-VSS 모델로 사용 */
}

/*
 * [한국어]
 * on_chunk_open - chunk가 open될 때 P2L 로그 슬롯 바인딩
 *
 * @dev:   FTL 디바이스
 * @chunk: open 되는 chunk
 *
 * P2L 로그 풀에서 슬롯을 잡아 chunk에 매단다. 이 슬롯은 chunk 수명 동안 그 chunk의
 * 모든 사용자 쓰기에 대한 (LBA, addr) 메타데이터를 append-only로 기록한다.
 * chunk->md->p2l_log_type 에 슬롯의 타입(여러 P2L log region 중 어느 것)을 기록해
 * 종료/복구 시 어느 영역에서 P2L를 재생할지 판단할 수 있게 한다.
 *
 * 호출 체인:
 *   ftl_nv_cache_chunk_open() → ops.on_chunk_open → ftl_p2l_log_acquire()
 */
static void
on_chunk_open(struct spdk_ftl_dev *dev, struct ftl_nv_cache_chunk *chunk)
{
	assert(NULL == chunk->p2l_log);                    /* [한국어] open 시점에는 슬롯 없음이 정상 */
	/* [한국어] seq_id를 키로 P2L 풀에서 슬롯 획득. 완료 콜백은 p2l_log_cb 등록. */
	chunk->p2l_log = ftl_p2l_log_acquire(dev, chunk->md->seq_id, p2l_log_cb);
	chunk->md->p2l_log_type = ftl_p2l_log_type(chunk->p2l_log); /* [한국어] 메타에 타입 기록 (영속) */
}

/*
 * [한국어]
 * on_chunk_closed - chunk close 시 P2L 슬롯 반환
 *
 * @chunk: close된 chunk
 *
 * on_chunk_open이 잡은 슬롯을 풀로 돌려준다. chunk->p2l_log를 NULL로 만들어
 * 이후 잘못된 재사용을 막는다.
 */
static void
on_chunk_closed(struct spdk_ftl_dev *dev, struct ftl_nv_cache_chunk *chunk)
{
	assert(chunk->p2l_log);                            /* [한국어] close 시 반드시 슬롯이 있어야 함 */
	ftl_p2l_log_release(dev, chunk->p2l_log);          /* [한국어] 풀로 슬롯 반환 */
	chunk->p2l_log = NULL;                             /* [한국어] 재사용 방지 */
}

/*
 * [한국어]
 * p2l_log_cb - P2L 로그 append 완료 콜백 (마지막 단계)
 *
 * @io: P2L append가 끝난 사용자 IO
 *
 * write_io→write_io_cb→ftl_p2l_log_io 가 끝난 뒤 P2L 로그가 영속 매체에 안전히
 * 기록되었음을 알린다. 두 단계(데이터+메타)가 모두 끝난 시점이므로 사용자 완료를
 * 진정한 의미에서 보고할 수 있다.
 */
static void
p2l_log_cb(struct ftl_io *io)
{
	ftl_nv_cache_write_complete(io, true);             /* [한국어] 사용자에게 성공 통지 */
}

/*
 * [한국어]
 * write_io_cb - 데이터 블록 쓰기 완료 후 P2L 로그 단계로 진입
 *
 * @bdev_io: SPDK bdev가 돌려준 완료 IO 컨텍스트
 * @success: 쓰기 성공 여부
 * @ctx:     write_io에서 넘긴 ftl_io* (cb_arg로 위장)
 *
 * 데이터 단계가 끝나면 (1) 통계 업데이트 (2) bdev_io 해제 (3) 성공 시 P2L 로그에
 * (LBA, addr) append, 실패 시 즉시 사용자 실패 보고를 수행한다.
 *
 * 실행 컨텍스트: bdev 완료 콜백 — 해당 IO를 발사한 SPDK 스레드에서 실행.
 *
 * 호출 체인:
 *   spdk_bdev_writev_blocks 완료 → [이 함수]
 *     → 성공: ftl_p2l_log_io(log, io) → 추후 p2l_log_cb
 *     → 실패: ftl_nv_cache_write_complete(io, false)
 */
static void
write_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *ctx)
{
	struct ftl_io *io = ctx;                           /* [한국어] cb_arg를 ftl_io로 복원 */

	ftl_stats_bdev_io_completed(io->dev, FTL_STATS_TYPE_USER, bdev_io); /* [한국어] 사용자 IO 통계 갱신 */
	spdk_bdev_free_io(bdev_io);                        /* [한국어] bdev_io 컨텍스트 풀에 반환 */

	if (spdk_likely(success)) {                        /* [한국어] 정상 경로 — P2L 단계로 진입 */
		struct ftl_p2l_log *log = io->nv_cache_chunk->p2l_log; /* [한국어] chunk에 바인딩된 슬롯 */
		ftl_p2l_log_io(log, io);                   /* [한국어] (LBA, addr) append 비동기 시작 */
	} else {
		ftl_nv_cache_write_complete(io, false);    /* [한국어] 데이터 실패 — 즉시 사용자에 실패 통지 */
	}
}

/*
 * [한국어]
 * write_io_retry - bdev가 ENOMEM으로 거부했을 때 큐에서 깨어나 재시도
 *
 * @ctx: ftl_io* — io->bdev_io_wait.cb_arg로 등록한 값이 그대로 전달됨
 *
 * spdk_bdev_queue_io_wait가 자원 가용 시 이 콜백을 호출 → write_io 재실행.
 */
static void
write_io_retry(void *ctx)
{
	struct ftl_io *io = ctx;                           /* [한국어] cb_arg 복원 */

	write_io(io);                                      /* [한국어] 재시도 */
}

/*
 * [한국어]
 * write_io - 사용자 IO를 NVC에 비동기 쓰기 시작 (ops.write 콜백)
 *
 * @io: FTL 사용자 쓰기 IO
 *
 * 데이터를 NVC bdev에 spdk_bdev_writev_blocks로 비동기 발사한다.
 * NVC 내부 오프셋은 ftl_addr_to_nvc_offset(dev, io->addr)로 ftl 가상주소를 NVC 블록
 * 오프셋으로 변환한다. ENOMEM이면 wait queue에 등록 후 깨어날 때 재시도하고,
 * 그 외 오류는 abort 한다 (복구 불가능 가정).
 *
 * 실행 컨텍스트: FTL submit 경로의 스레드 (NVC 채널 소유 스레드).
 * 핫 패스 — 매 사용자 쓰기마다 호출된다.
 *
 * 호출 체인:
 *   nvc_type->ops.write = write_io
 *     → spdk_bdev_writev_blocks(... , write_io_cb, io)
 *     → 완료: write_io_cb → (성공) ftl_p2l_log_io → p2l_log_cb
 */
static void
write_io(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;                /* [한국어] FTL 디바이스 핸들 */
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;    /* [한국어] NVC 컨텍스트 */
	int rc;                                            /* [한국어] bdev 발사 결과 */

	/* [한국어] 데이터 블록 쓰기 발사. ftl_addr_to_nvc_offset = (addr - NVC base) 변환. */
	rc = spdk_bdev_writev_blocks(nv_cache->bdev_desc, nv_cache->cache_ioch,
				     io->iov, io->iov_cnt,
				     ftl_addr_to_nvc_offset(dev, io->addr), io->num_blocks,
				     write_io_cb, io);
	if (spdk_unlikely(rc)) {                           /* [한국어] 거의 발생하지 않음 (핫 패스 최적화) */
		if (rc == -ENOMEM) {                       /* [한국어] bdev_io 풀 고갈 — 큐 등록 후 재시도 */
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);
			io->bdev_io_wait.bdev = bdev;      /* [한국어] 어느 bdev에서 기다릴지 */
			io->bdev_io_wait.cb_fn = write_io_retry; /* [한국어] 깨울 때 호출할 콜백 */
			io->bdev_io_wait.cb_arg = io;      /* [한국어] 콜백 인자 = 이 io */
			spdk_bdev_queue_io_wait(bdev, nv_cache->cache_ioch, &io->bdev_io_wait);
		} else {
			ftl_abort();                       /* [한국어] 다른 오류는 복구 불가 → abort */
		}
	}
}

/*
 * [한국어]
 * process - 주기적 처리 (ops.process — poller에서 호출)
 *
 * @dev: FTL 디바이스
 *
 * P2L 로그에 누적된 entry를 영속화하기 위해 flush를 트리거한다. flush 자체는 비동기
 * 적으로 진행되며 완료 시 chunk별 슬롯의 cb (= p2l_log_cb)이 호출된다.
 */
static void
process(struct spdk_ftl_dev *dev)
{
	ftl_p2l_log_flush(dev);                            /* [한국어] 누적된 P2L 로그 영속화 트리거 */
}

/* [한국어] open chunk 복구용 ftl_mngt 컨텍스트.
 * 한 chunk를 복구하는 ftl_mngt 프로세스 인스턴스에 부착되며 P2L 재생 단계가
 * 어떤 chunk를 대상으로 하는지 식별한다. */
struct recovery_chunk_ctx {
	struct ftl_nv_cache_chunk *chunk;
	/* [한국어] 복구 대상 chunk 포인터.
	 * 설정자: recovery_chunk_init()에서 init_ctx로 전달받음.
	 * 읽는 자: recovery_chunk_recover_p2l_map() 등 단계 핸들러.
	 * 동기화: ftl_mngt 프로세스는 단일 스레드에서 순차 실행되므로 락 불필요. */
};

/*
 * [한국어]
 * recovery_chunk_recover_p2l_map_cb - P2L 재생 완료 콜백
 *
 * @cb_arg: ftl_mngt_process* (recovery_chunk_recover_p2l_map에서 등록)
 * @status: 0 성공, !=0 실패
 *
 * P2L 로그 전체 read가 끝난 시점에 호출되어 ftl_mngt 단계 전이를 결정한다.
 */
static void
recovery_chunk_recover_p2l_map_cb(void *cb_arg, int status)
{
	struct ftl_mngt_process *mngt = cb_arg;            /* [한국어] mngt 핸들 복원 */

	if (status) {
		ftl_mngt_fail_step(mngt);                  /* [한국어] 단계 실패 → 상위 절차 abort 경로 */
	} else {
		ftl_mngt_next_step(mngt);                  /* [한국어] 단계 성공 → 다음 단계로 진행 */
	}
}

/*
 * [한국어]
 * recovery_chunk_recover_p2l_map_read_cb - P2L 엔트리 단위 콜백
 *
 * @dev:    FTL 디바이스
 * @cb_arg: ftl_mngt_process*
 * @lba:    이 엔트리가 가리키는 사용자 LBA
 * @addr:   사용자 데이터가 저장된 NVC 주소 (ftl_addr)
 * @seq_id: 엔트리 시퀀스 ID
 * @return: 0 = 계속, 비0 = 중단
 *
 * P2L 로그를 한 엔트리씩 재생하면서 각 (LBA, addr) 매핑을 chunk 메모리 P2L 맵에
 * 다시 채워 넣는다.
 */
static int
recovery_chunk_recover_p2l_map_read_cb(struct spdk_ftl_dev *dev, void *cb_arg,
				       uint64_t lba, ftl_addr addr, uint64_t seq_id)
{
	struct ftl_mngt_process *mngt = cb_arg;            /* [한국어] mngt 핸들 복원 */
	struct recovery_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt); /* [한국어] 사용자 컨텍스트 */
	struct ftl_nv_cache_chunk *chunk = ctx->chunk;     /* [한국어] 대상 chunk */

	ftl_nv_cache_chunk_set_addr(chunk, lba, addr);     /* [한국어] (LBA, addr) 매핑을 in-memory P2L에 기록 */

	/* TODO We could stop scanning when getting all LBA within the chunk */
	return 0;                                          /* [한국어] 항상 계속 — 향후 조기 종료 최적화 여지 */
}


/*
 * [한국어]
 * recovery_chunk_recover_p2l_map - 단일 chunk의 P2L 재생 단계
 *
 * @dev:  FTL 디바이스
 * @mngt: 현재 단계의 mngt 핸들
 *
 * chunk->md에 기록되어 있던 p2l_log_type/seq_id로 P2L 로그를 비동기 read하면서
 * 엔트리 콜백으로 in-memory 맵을 재구성한다. 결과는 *_cb에서 단계 전이로 보고.
 */
static void
recovery_chunk_recover_p2l_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct recovery_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt); /* [한국어] 컨텍스트 획득 */
	struct ftl_nv_cache_chunk *chunk = ctx->chunk;     /* [한국어] 대상 chunk */
	int rc;                                            /* [한국어] 재생 시작 결과 */

	/* [한국어] P2L 로그 비동기 읽기 시작.
	 * 엔트리 콜백: recovery_chunk_recover_p2l_map_read_cb (각 LBA-addr 처리)
	 * 완료 콜백:   recovery_chunk_recover_p2l_map_cb     (단계 종료) */
	rc = ftl_p2l_log_read(dev, chunk->md->p2l_log_type, chunk->md->seq_id,
			      recovery_chunk_recover_p2l_map_cb, mngt,
			      recovery_chunk_recover_p2l_map_read_cb);

	if (rc) {
		ftl_mngt_fail_step(mngt);                  /* [한국어] 시작조차 실패 — 단계 실패 통지 */
	}
}

/*
 * [한국어]
 * recovery_chunk_init - 복구 mngt 프로세스 인스턴스의 init handler
 *
 * @dev:      FTL 디바이스
 * @mngt:     이 mngt 프로세스 핸들
 * @init_ctx: 호출자(recover_open_chunk)가 넘긴 chunk*
 * @return: 0 (실패 케이스 없음)
 */
static int
recovery_chunk_init(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
		    void *init_ctx)
{
	struct recovery_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt); /* [한국어] mngt가 할당한 빈 컨텍스트 */

	ctx->chunk = init_ctx;                             /* [한국어] 대상 chunk 저장 */
	return 0;
}

/* [한국어] open chunk 복구 mngt 프로세스 정의.
 * ctx_size로 mngt 프로세스 자체가 컨텍스트 메모리를 할당해 주고,
 * init_handler가 사용자 정보로 그 컨텍스트를 채운다.
 * steps 배열은 NULL terminator로 끝남 — 현재 단계는 단일 (P2L 재생). */
static const struct ftl_mngt_process_desc desc_chunk_recovery = {
	.name = "Recover open chunk",
	.ctx_size = sizeof(struct recovery_chunk_ctx),
	.init_handler = recovery_chunk_init,
	.steps = {
		{
			.name = "Recover chunk P2L map",
			.action = recovery_chunk_recover_p2l_map,
		},
		{}                                         /* [한국어] terminator */
	}
};

/*
 * [한국어]
 * recover_open_chunk - 외부에서 호출되는 open chunk 복구 진입점 (ops.recover_open_chunk)
 *
 * @dev:   FTL 디바이스
 * @mngt:  부모 mngt 프로세스 (FTL 전체 복구 흐름)
 * @chunk: 복구할 open chunk
 *
 * 부모 mngt 안에서 자식 mngt 프로세스 desc_chunk_recovery를 호출(call)하여
 * 위 단계를 수행한다. 자식이 끝나면 부모가 next_step으로 자동 진행.
 */
static void
recover_open_chunk(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
		   struct ftl_nv_cache_chunk *chunk)
{
	ftl_mngt_call_process(mngt, &desc_chunk_recovery, chunk); /* [한국어] 자식 mngt 호출 */
}

/*
 * [한국어]
 * setup_layout - non-VSS 모델이 필요한 P2L 로그 영역들을 layout에 예약 (ops.setup_layout)
 *
 * @dev: FTL 디바이스
 * @return: 0 성공, -1 실패 (region_create/open 중 하나라도 실패)
 *
 * non-VSS는 메타데이터를 P2L 로그에 별도 기록해야 하므로 여러 개의 P2L log region을
 * superblock에 예약한다 (P2L_LOG_IO_MIN ~ P2L_LOG_IO_MAX). 각 region은 한 chunk가
 * 1회 가득 차는 데 필요한 P2L 엔트리 수를 보관할 만큼의 블록을 가진다.
 *
 * 호출 체인:
 *   ftl_mngt 초기화 → ops.setup_layout = setup_layout
 *     → md_layout_ops.region_create / region_open (= ftl_nvc_bdev_common_*)
 */
static int
setup_layout(struct spdk_ftl_dev *dev)
{
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops; /* [한국어] 공통 layout ops 참조 */
	/* [한국어] 한 chunk 분량(=한 band의 블록 수)의 P2L 엔트리를 보관할 메타 블록 수 산출. */
	const uint64_t blocks = ftl_p2l_log_get_md_blocks_required(dev, 1, ftl_get_num_blocks_in_band(dev));
	enum ftl_layout_region_type region_type;           /* [한국어] 순회 변수 */

	/* [한국어] 여러 P2L 로그 슬롯을 region 단위로 예약 — 동시 open chunk 수만큼 필요. */
	for (region_type = FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MIN;
	     region_type <= FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MAX;
	     region_type++) {
		/* [한국어] 영역을 layout tracker에 예약 (현재 버전 사용). */
		if (md_ops->region_create(dev, region_type, FTL_P2L_LOG_VERSION_CURRENT, blocks)) {
			return -1;
		}

		/* [한국어] 예약된 영역을 dev->layout.region 슬롯에 채워 IO 시 즉시 사용 가능하게. */
		if (md_ops->region_open(dev, region_type, FTL_P2L_LOG_VERSION_CURRENT,
					FTL_BLOCK_SIZE, blocks,
					&dev->layout.region[region_type])) {
			return -1;
		}
	}

	return 0;
}

/* [한국어] non-VSS NVC 모델 정적 디스크립터 — 매크로로 자동 등록된다.
 * 이름 "bdev-non-vss"로 식별되며 is_bdev_compatible(md_size==0)을 통해 대상 bdev를 매칭. */
struct ftl_nv_cache_device_type nvc_bdev_non_vss = {
	.name = "bdev-non-vss",
	.features = {
	},
	.ops = {
		.init = init,
		.deinit = deinit,
		.on_chunk_open = on_chunk_open,
		.on_chunk_closed = on_chunk_closed,
		.is_bdev_compatible = is_bdev_compatible,
		.is_chunk_active = ftl_nvc_bdev_common_is_chunk_active, /* [한국어] 공통 구현 재사용 */
		.setup_layout = setup_layout,
		.md_layout_ops = {
			.region_create = ftl_nvc_bdev_common_region_create, /* [한국어] 공통 */
			.region_open = ftl_nvc_bdev_common_region_open,     /* [한국어] 공통 */
		},
		.process = process,
		.write = write_io,
		.recover_open_chunk = recover_open_chunk
	}
};
/* [한국어] 컨스트럭터에 등록 — main() 진입 전 자동 g_devs 추가. */
FTL_NV_CACHE_DEVICE_TYPE_REGISTER(nvc_bdev_non_vss)
