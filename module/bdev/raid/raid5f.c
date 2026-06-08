/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] RAID5 Forward (RAID5F) 모듈 — full-stripe write 전용 RAID5 (raid5f.c)
 *
 * === 파일의 역할 ===
 * 본 파일은 SPDK 의 bdev_raid 프레임워크 위에 RAID5 의 한 변형 "RAID5F" (Forward)
 * 를 구현한다. 일반 RAID5 와 달리, 본 모듈은 WRITE 가 항상 stripe 전체 (full stripe)
 * 단위로만 들어온다고 가정한다 — 이는 bdev 코어의 split_on_write_unit 기능과
 * write_unit_size = stripe_blocks 설정으로 강제된다. 덕분에 partial-stripe write 시
 * 발생하는 read-modify-write (RMW) 와 write-hole 문제를 회피할 수 있고, 각 WRITE 는
 * (N data + 1 parity) 청크를 N+1 베이스 디바이스에 병렬 발행하기만 하면 된다.
 * READ 는 일반 단일 청크 read (디바이스가 살아있을 때) 또는 reconstruct read
 * (디바이스가 죽었을 때 — 나머지 N개를 읽어 XOR 로 복원) 로 처리한다.
 * Parity = data chunk 들의 bitwise XOR — SPDK accel framework (spdk_accel_submit_xor)
 * 가 ISA-L 또는 SW fallback 으로 가속.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   application → spdk_bdev_read/write → lib/bdev (split: write_unit_size) →
 *   bdev_raid (level dispatcher) → [이 파일의 raid5f_submit_rw_request] →
 *   chunk 별 raid_bdev_writev/readv_blocks_ext → 베이스 bdev → SSD.
 * raid_bdev_module 의 vtable (level=RAID5F) 에 등록되어 bdev_raid 코어가
 * stripe 매핑 + I/O 분배의 RAID5F-specific 로직을 본 파일에 위임한다.
 * 본 파일은 bdev_raid.c (raid 코디네이터 코어, 다른 에이전트가 작업 중) 의
 * struct raid_bdev / raid_bdev_io / raid_base_bdev_info 위에서만 동작하며,
 * 그것들을 직접 수정하지는 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - bdev_raid.h: struct raid_bdev_module (vtable 등록), raid_bdev_io 의 핸들링
 *   헬퍼 (raid_bdev_io_complete_part / raid_bdev_writev_blocks_ext /
 *   raid_bdev_queue_io_wait / raid_bdev_io_init / raid_bdev_process_request) 사용.
 * - lib/accel (spdk/accel.h): spdk_accel_get_io_channel, spdk_accel_submit_xor —
 *   parity 계산용 비동기 XOR. ISA-L 가속이 있으면 SIMD (AVX2/AVX512) 사용.
 * - lib/util (spdk/util.h): spdk_ioviter (여러 iovec 를 한 번에 walk),
 *   SPDK_CONTAINEROF, spdk_u32log2 등.
 * - lib/env (spdk/env.h): spdk_dma_malloc/free — DPDK hugepage 메모리 (parity 버퍼).
 * 데이터 흐름:
 *   - WRITE: raid_io->iovs (stripe 전체 데이터) → raid5f_stripe_request_map_iovecs 가
 *     N개 data chunk 의 iov 로 분할 → XOR 로 parity chunk 채움 →
 *     N+1 베이스 bdev 에 병렬 writev → 모두 완료되면 raid_bdev_io_complete.
 *   - READ (정상): 단일 베이스 bdev readv. 빠른 경로.
 *   - READ (degraded): reconstruct_read → N-1 개의 정상 chunk + parity chunk readv →
 *     XOR 로 죽은 chunk 복원 → 사용자 iov 에 복사.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct stripe_request: 한 stripe 에 대한 진행 상태 (write 또는 reconstruct).
 *   N+1 개의 struct chunk (flexible array) 와 XOR 진행 상태 (xor.remaining,
 *   xor.cb 등) 를 포함. free pool 로 관리되어 RAID5F_MAX_STRIPES=32 개까지 동시 진행.
 * - struct chunk: 한 베이스 bdev 의 청크 — iov 배열 + md_buf. iovcnt 가 동적
 *   확장 가능 (realloc).
 * - struct raid5f_info: per-raid_bdev 정보 — stripe_blocks, total_stripes, buf_alignment.
 * - struct raid5f_io_channel: per-channel — free stripe_request 풀, accel io channel,
 *   xor retry queue.
 * - raid5f_submit_rw_request(): 메인 진입점. offset 으로 stripe_index/stripe_offset
 *   계산 후 READ/WRITE 분기.
 * - raid5f_submit_write_request(): full-stripe WRITE 처리. iovec 분할 → XOR → 병렬 발행.
 * - raid5f_submit_read_request(): READ 처리. 베이스 살아있으면 직접, 죽었으면 reconstruct.
 * - raid5f_xor_stripe(): SPDK accel framework 로 비동기 XOR 발행.
 * - raid5f_start() / raid5f_stop(): RAID 라이프사이클 콜백.
 * - raid5f_submit_process_request(): online 데이터 동기화 (디바이스 교체 후 rebuild 등) 처리.
 */

#include "bdev_raid.h"            /* [한국어] bdev_raid 프레임워크 — struct raid_bdev/raid_bdev_io/raid_bdev_module 등. */

#include "spdk/env.h"             /* [한국어] spdk_dma_malloc/free — DMA 적합 hugepage 메모리. */
#include "spdk/thread.h"          /* [한국어] spdk_io_channel/spdk_io_device — per-channel state. */
#include "spdk/string.h"          /* [한국어] spdk_strerror. */
#include "spdk/util.h"            /* [한국어] spdk_ioviter, SPDK_CONTAINEROF, spdk_min/max, spdk_u32log2 등. */
#include "spdk/likely.h"          /* [한국어] spdk_likely/spdk_unlikely — branch predictor 힌트. */
#include "spdk/log.h"             /* [한국어] SPDK_ERRLOG, SPDK_LOG_REGISTER_COMPONENT. */
#include "spdk/accel.h"           /* [한국어] accel framework — spdk_accel_submit_xor, ISA-L/SW XOR 가속. */

/* Maximum concurrent full stripe writes per io channel */
/* [한국어] 채널당 동시 진행 가능한 stripe 요청 (write/reconstruct 각각) 최대 개수.
 * 32 로 제한 — 더 많은 동시 stripe 가 필요하면 -ENOMEM 으로 거절되어 bdev 코어가
 * queue_io_wait 으로 재시도. 메모리 사용 (per-stripe = N+1 chunks * iov + parity buf) 을
 * 제한하기 위해 적절한 값. */
#define RAID5F_MAX_STRIPES 32

/* [한국어] 한 베이스 bdev (N data + 1 parity 중 하나) 에서 stripe 한 줄에 해당하는
 * 청크. 청크 크기 = strip_size * blocklen 바이트. iov 가 동적이라 사용자 IO 의
 * scatter-gather 와 일치하게 분할되어 들어간다. */
struct chunk {
	/* Corresponds to base_bdev index */
	uint8_t index;
	/* [한국어] 이 청크가 향하는 베이스 bdev 인덱스 (0..num_base_bdevs-1).
	 * 설정자: raid5f_stripe_request_alloc 가 chunks[] 배열 인덱스로 채움.
	 * 읽는 자: raid5f_chunk_submit 이 raid_bdev->base_bdev_info[index] lookup.
	 * 값 범위: [0, num_base_bdevs). 동기화: 한 stripe_request 단위로만 변경 — race 없음. */

	/* Array of iovecs */
	struct iovec *iovs;
	/* [한국어] 청크에 들어가는/나오는 데이터의 iovec 배열. 처음에는 4개로 할당,
	 * iovcnt 가 더 필요하면 realloc.
	 * 설정자: raid5f_stripe_request_alloc (초기 할당), raid5f_chunk_set_iovcnt (realloc),
	 *         raid5f_stripe_request_map_iovecs (iov_base/iov_len 채움).
	 * 읽는 자: raid_bdev_writev/readv_blocks_ext 가 베이스 bdev 호출 시.
	 * 동기화: 단일 IO. */

	/* Number of used iovecs */
	int iovcnt;
	/* [한국어] 현재 청크에서 사용 중인 iovec 개수. */

	/* Total number of available iovecs in the array */
	int iovcnt_max;
	/* [한국어] iovs 배열의 capacity. iovcnt > iovcnt_max 가 되면 realloc. */

	/* Pointer to buffer with I/O metadata */
	void *md_buf;
	/* [한국어] DIF/DIX 등의 metadata 버퍼 — 베이스 bdev 의 md 영역에 해당.
	 * write 시 사용자 raid_io->md_buf 의 일부 (또는 parity 의 경우 별도 버퍼).
	 * 설정자: raid5f_stripe_request_map_iovecs / raid5f_submit_reconstruct_read.
	 * 동기화: 단일 IO. */
};

struct stripe_request;
typedef void (*stripe_req_xor_cb)(struct stripe_request *stripe_req, int status);
/* [한국어] XOR 완료 콜백 시그니처 — XOR 가 끝나면 다음 단계(write submit 또는
 * reconstruct 후처리) 로 진행할 함수 포인터. */

/* [한국어] 한 stripe 단위의 진행 상태. type=WRITE 면 사용자 데이터를 N data chunk
 * 로 분할 + parity 계산 + N+1 chunk 병렬 발행. type=RECONSTRUCT 면 N-1 data + 1 parity
 * 읽어 XOR 로 죽은 chunk 복원.
 * 메모리 레이아웃: 본 구조체 + flexible array chunks[num_base_bdevs] 가 단일 calloc 으로
 * 할당된다. raid5f_chunk_stripe_req() 가 chunk 포인터에서 stripe_req 를 역으로 복원하는데,
 * chunks[] 가 구조체 끝에 있어 (chunk - chunk->index) 로 chunks[0] 얻고 CONTAINEROF 로 본체 추출. */
struct stripe_request {
	enum stripe_request_type {
		STRIPE_REQ_WRITE,
		/* [한국어] full-stripe WRITE 처리 중 — N data + parity 모두 발행하고 완료 대기. */
		STRIPE_REQ_RECONSTRUCT,
		/* [한국어] degraded READ 의 reconstruct — N-1 data + parity 를 읽어 죽은 chunk XOR 복원. */
	} type;
	/* [한국어] 본 요청의 종류. raid5f_stripe_request_alloc 시점에 결정되며, free 풀도
	 * write/reconstruct 별로 분리되어 있다. */

	struct raid5f_io_channel *r5ch;
	/* [한국어] 본 요청이 속한 channel — accel_ch 와 free 풀 위치 추적용.
	 * 설정자: raid5f_stripe_request_alloc. 읽는 자: 콜백 곳곳. */

	/* The associated raid_bdev_io */
	struct raid_bdev_io *raid_io;
	/* [한국어] 사용자가 보낸 원본 bdev_io 의 raid 래퍼.
	 * iovs/iovcnt/offset_blocks/num_blocks/md_buf 등 사용자 입력을 보관.
	 * 설정자: raid5f_stripe_request_init. 읽는 자: 모든 단계. */

	/* The stripe's index in the raid array. */
	uint64_t stripe_index;
	/* [한국어] 이 요청이 다루는 stripe 번호 — base_offset_blocks 계산에 사용.
	 * offset_blocks / stripe_blocks 로 계산. parity chunk 위치도 이 값의 함수. */

	/* The stripe's parity chunk */
	struct chunk *parity_chunk;
	/* [한국어] 이번 stripe 에서 parity 를 담는 chunk 의 포인터 (= &chunks[parity_idx]).
	 * rotating parity (RAID5 의 특징) — stripe_index 에 따라 parity 위치 회전.
	 * 설정자: raid5f_stripe_request_init (raid5f_stripe_parity_chunk_index 로 계산). */

	union {
		struct {
			/* Buffer for stripe parity */
			void *parity_buf;
			/* [한국어] parity 청크가 사용하는 DMA 버퍼 (strip_size * blocklen 바이트).
			 * spdk_dma_malloc 으로 alloc, raid5f_stripe_request_free 에서 free.
			 * WRITE 경로에서 XOR 결과가 여기 채워져 베이스 bdev 에 전송된다.
			 * 한 번 alloc 해서 stripe_request 의 평생 재사용 (free 풀에서 dequeue 될 때마다). */

			/* Buffer for stripe io metadata parity */
			void *parity_md_buf;
			/* [한국어] metadata (DIF/DIX) 의 parity 버퍼 — bdev 가 md_interleave=false 일 때만.
			 * 크기 = strip_size * md_len. */
		} write;
		/* [한국어] WRITE 전용 필드. */

		struct {
			/* Array of buffers for reading chunk data */
			void **chunk_buffers;
			/* [한국어] reconstruct 시 N-1 개 정상 chunk + parity chunk 의 데이터를 읽어둘
			 * 버퍼 포인터 배열. 크기 = N (data chunk 수) * strip_size * blocklen 의 buffer N개.
			 * 죽은 chunk 의 iov 는 사용자 raid_io->iovs 를 직접 가리키므로 따로 버퍼 불필요. */

			/* Array of buffers for reading chunk metadata */
			void **chunk_md_buffers;
			/* [한국어] reconstruct 시 metadata 용 버퍼 배열. md_interleave=false 면 alloc. */

			/* Chunk to reconstruct from parity */
			struct chunk *chunk;
			/* [한국어] 복원 대상 chunk 의 포인터 — 죽은(또는 빠진) 베이스 bdev 의 chunk. */

			/* Offset from chunk start */
			uint64_t chunk_offset;
			/* [한국어] chunk 내부의 read 시작 오프셋 (블록 단위). READ 가 strip 의 일부분만
			 * 요구할 때 사용. WRITE 는 항상 strip 전체이므로 0. */
		} reconstruct;
		/* [한국어] RECONSTRUCT 전용 필드. */
	};

	/* Array of iovec iterators for each chunk */
	struct spdk_ioviter *chunk_iov_iters;
	/* [한국어] spdk_ioviter — 여러 iovec 묶음을 동기적으로 walk 하기 위한 헬퍼.
	 * XOR 가 청크별 iov 를 동시에 진행하면서 공통 길이만큼씩 chunk_xor_buffers 에 채움.
	 * 크기 = SPDK_IOVITER_SIZE(num_base_bdevs). */

	/* Array of source buffer pointers for parity calculation */
	void **chunk_xor_buffers;
	/* [한국어] spdk_accel_submit_xor 에 넘기는 src 포인터 배열 (정확히는 dst 도 같은 배열의
	 * 마지막 슬롯). chunk_iov_iters 의 nextv 호출이 채움.
	 * 크기 = num_base_bdevs (data chunks + 1 dst slot). */

	/* Array of source buffer pointers for parity calculation of io metadata */
	void **chunk_xor_md_buffers;
	/* [한국어] metadata XOR 용 src 배열. data 와 md 의 src 가 분리된 이유는 md 가
	 * scatter-gather 가 아닌 단일 버퍼라서 iov_iter 불필요. */

	struct {
		size_t len;
		/* [한국어] 이번 XOR 호출에서 처리할 바이트 수 — ioviter_nextv 결과.
		 * src 들이 서로 다른 iov 경계를 가질 수 있어 매번 다른 값. */

		size_t remaining;
		/* [한국어] 남은 data XOR 바이트 수. 0 이 되면 data XOR 완료. */

		size_t remaining_md;
		/* [한국어] 남은 metadata XOR 바이트 수. data 와 md 는 동시 진행. */

		int status;
		/* [한국어] XOR 진행 중 발생한 첫 에러 (음수 errno). 0 이면 정상. */

		stripe_req_xor_cb cb;
		/* [한국어] 모든 XOR 가 끝났을 때 호출할 콜백. WRITE 면 raid5f_stripe_write_request_xor_done,
		 * reconstruct 면 raid5f_stripe_request_reconstruct_xor_done. */
	} xor;
	/* [한국어] XOR 진행 추적 상태 — 비동기 XOR 가 여러 번 나뉘어 호출될 수 있어 누적 필요. */

	TAILQ_ENTRY(stripe_request) link;
	/* [한국어] free 풀 또는 xor_retry_queue 연결. */

	/* Array of chunks corresponding to base_bdevs */
	struct chunk chunks[0];
	/* [한국어] flexible array — 실제 크기는 alloc 시 num_base_bdevs 만큼.
	 * 본 구조체와 함께 단일 calloc 으로 할당되어 cache locality 우수. */
};

/* [한국어] per-raid_bdev (RAID5F 인스턴스 1개당 1개) 영속 정보 객체.
 * raid5f_start() 가 calloc 으로 생성해 raid_bdev->module_private 에 매달고, 동시에
 * spdk_io_device_register() 의 io_device 인자로도 등록한다 — 즉 본 구조체 포인터가
 * 이 RAID5F 인스턴스의 "io_device 핸들" 이 되어, 채널 생성/조회 시 키로 쓰인다.
 * raid5f_stop() → raid5f_io_device_unregister_done() 에서 free.
 * 모든 필드는 start 시점에 한 번 계산되어 이후 read-only — 멀티 채널이 동시에 읽어도
 * 변경이 없으므로 락 불필요 (SPDK lockless 설계). */
struct raid5f_info {
	/* The parent raid bdev */
	struct raid_bdev *raid_bdev;
	/* [한국어] 이 RAID5F 가 속한 상위 raid_bdev (bdev_raid 코어 객체) 역참조.
	 * 설정자: raid5f_start() 가 raid_bdev 를 그대로 저장.
	 * 읽는 자: strip_size/num_base_bdevs/bdev.blocklen 등 RAID 기하 정보가 필요한 거의 모든 함수.
	 * 값 범위: 유효한 raid_bdev 포인터 (NULL 불가). 동기화: read-only 공유 — 락 불필요. */

	/* Number of data blocks in a stripe (without parity) */
	uint64_t stripe_blocks;
	/* [한국어] 한 stripe 의 데이터 블록 총개수 = strip_size * (데이터 청크 수 N).
	 * parity 청크는 제외 — 즉 사용자에게 노출되는 stripe 당 유효 용량.
	 * 설정자: raid5f_start() 에서 strip_size * raid5f_stripe_data_chunks_num() 으로 계산.
	 * 읽는 자: raid5f_submit_rw_request() 가 offset_blocks 를 이 값으로 나눠 stripe_index/offset 산출.
	 *         또한 bdev.write_unit_size 로도 설정되어 bdev 코어가 WRITE 를 이 단위로 split.
	 * 값 범위: strip_size * N (>=2). 동기화: read-only. */

	/* Number of stripes on this array */
	uint64_t total_stripes;
	/* [한국어] 이 어레이가 담는 stripe 총개수 = (베이스 bdev 중 최소 blockcnt) / strip_size.
	 * 설정자: raid5f_start(). 읽는 자: bdev.blockcnt = stripe_blocks * total_stripes 계산.
	 * 값 범위: >=0. 동기화: read-only. */

	/* Alignment for buffer allocation */
	size_t buf_alignment;
	/* [한국어] parity/reconstruct 버퍼 DMA alloc 시 사용할 정렬 바이트 수.
	 * 모든 베이스 bdev 의 buf_align 중 최댓값 — 가장 엄격한 요구를 만족시켜야 모든
	 * 베이스 bdev 에 DMA 가능.
	 * 설정자: raid5f_start() 가 spdk_bdev_get_buf_align() 의 max.
	 * 읽는 자: raid5f_stripe_request_alloc() 의 spdk_dma_malloc 정렬 인자.
	 * 값 범위: 2의 거듭제곱. 동기화: read-only. */

	/* block length bit shift for optimized calculation, only valid when no interleaved md */
	uint32_t blocklen_shift;
	/* [한국어] log2(blocklen) — md_buf 오프셋 계산 시 "블록 오프셋 = byte_offset >> shift"
	 * 로 나눗셈을 시프트로 대체하는 최적화 상수. interleaved md 가 아닐 때만 유효
	 * (md_interleave=true 면 0 으로 남고 사용되지 않음).
	 * 설정자: raid5f_start() 가 spdk_u32log2(blocklen).
	 * 읽는 자: raid5f_stripe_request_map_iovecs() 의 chunk->md_buf 오프셋 계산.
	 * 값 범위: 9(512B)~12(4KB) 등. 동기화: read-only. */
};

/* [한국어] per-channel (= per-spdk_thread / per-reactor) 상태 객체. SPDK 의 I/O 채널
 * 모델상, RAID5F 인스턴스 1개당 각 reactor 코어마다 본 구조체 1개가 만들어진다
 * (raid5f_ioch_create 가 채널 ctx_buf 로 초기화). 한 채널의 자료구조는 그 채널을 소유한
 * 단일 spdk_thread 에서만 접근되므로 free 풀/retry 큐 모두 락 없이 안전하다 — 이것이
 * SPDK lockless 설계의 핵심 (스레드 고정 = thread affinity).
 * 라이프사이클: spdk_get_io_channel(r5f_info) → raid5f_ioch_create → ... → raid5f_ioch_destroy. */
struct raid5f_io_channel {
	/* All available stripe requests on this channel */
	struct {
		TAILQ_HEAD(, stripe_request) write;
		/* [한국어] 재사용 가능한 WRITE 용 stripe_request free 풀.
		 * 채널 생성 시 RAID5F_MAX_STRIPES(32) 개를 미리 alloc 해 둠 — hot-path 에서
		 * malloc 회피.
		 * 설정자: ioch_create(채움), submit_write_request(dequeue), stripe_request_release(반환).
		 * 동기화: 채널 소유 스레드 단독 접근 — 락 불필요. */

		TAILQ_HEAD(, stripe_request) reconstruct;
		/* [한국어] 재사용 가능한 RECONSTRUCT 용 stripe_request free 풀 (역시 32개 사전 alloc).
		 * WRITE 와 분리한 이유: 두 타입의 버퍼 구성(parity_buf vs chunk_buffers)이 달라
		 * 풀을 공유하면 매번 재초기화가 필요하기 때문.
		 * 설정자: ioch_create / submit_reconstruct_read(dequeue) / stripe_request_release(반환).
		 * 동기화: 채널 소유 스레드 단독. */
	} free_stripe_requests;

	/* accel_fw channel */
	struct spdk_io_channel *accel_ch;
	/* [한국어] SPDK accel framework 의 I/O 채널 — spdk_accel_submit_xor 호출에 필요.
	 * accel 은 ISA-L(SIMD) 또는 SW 로 XOR 를 비동기 수행하며, 같은 reactor 스레드에서
	 * 처리되도록 채널 단위로 바인딩.
	 * 설정자: ioch_create 가 spdk_accel_get_io_channel(). 읽는 자: raid5f_xor_stripe_continue 등.
	 * 동기화: 채널 소유 스레드 단독. */

	/* For retrying xor if accel_ch runs out of resources */
	TAILQ_HEAD(, stripe_request) xor_retry_queue;
	/* [한국어] accel 채널이 -ENOMEM 을 반환했을 때 XOR 를 보류해 둘 재시도 큐.
	 * 다른 XOR 가 완료되면 raid5f_xor_stripe_done 이 이 큐에서 하나 꺼내 재시도 →
	 * 자원이 풀릴 때까지 자연스러운 backpressure 형성.
	 * 설정자: xor_stripe_continue/xor_stripe(INSERT), xor_stripe_done(REMOVE).
	 * 동기화: 채널 소유 스레드 단독. */

	/* For iterating over chunk iovecs during xor calculation */
	struct iovec **chunk_xor_iovs;
	/* [한국어] XOR 입력으로 넘길 각 청크의 iovec 배열 포인터들 (크기 num_base_bdevs).
	 * spdk_ioviter_firstv 에 넘겨 여러 청크를 동시에 walk 하기 위한 스크래치 영역.
	 * 채널 단위로 1개만 두고 매 XOR 마다 재사용 (한 채널은 직렬 처리이므로 안전).
	 * 설정자: ioch_create(alloc), raid5f_xor_stripe(매번 채움). 동기화: 채널 소유 스레드 단독. */

	size_t *chunk_xor_iovcnt;
	/* [한국어] chunk_xor_iovs 각 항목에 대응하는 iovcnt 배열 (크기 num_base_bdevs).
	 * 설정자: ioch_create(alloc), raid5f_xor_stripe(매번 채움).
	 * 읽는 자: spdk_ioviter_firstv. 동기화: 채널 소유 스레드 단독. */
};

/* [한국어] 청크 포인터 c 가 stripe_req 의 chunks[] 배열 범위 안에 있는지 검사.
 * 상한은 chunks + num_base_bdevs (= N data + 1 parity). 아래 FOR_EACH_* 루프의 종료 조건.
 * req->r5ch 를 통해 r5f_info 를 얻어 num_base_bdevs 를 조회한다. */
#define __CHUNK_IN_RANGE(req, c) \
	c < req->chunks + raid5f_ch_to_r5f_info(req->r5ch)->raid_bdev->num_base_bdevs

/* [한국어] from 부터 시작해 모든 청크(데이터+패리티)를 순회하는 for 루프 매크로.
 * 일부 함수가 이미 일부 청크를 처리한 뒤 나머지부터 재개할 때 사용 (submit_chunks). */
#define FOR_EACH_CHUNK_FROM(req, c, from) \
	for (c = from; __CHUNK_IN_RANGE(req, c); c++)

/* [한국어] chunks[0] 부터 모든 청크(데이터+패리티)를 순회. 가장 흔한 형태. */
#define FOR_EACH_CHUNK(req, c) \
	FOR_EACH_CHUNK_FROM(req, c, req->chunks)

/* [한국어] 청크 c 가 parity 청크면 다음 청크(c+1)로 건너뛰는 표현식.
 * RAID5 의 rotating parity 때문에 parity 위치가 stripe 마다 달라, 데이터 청크만
 * 골라 순회하려면 parity 인덱스를 동적으로 skip 해야 한다. */
#define __NEXT_DATA_CHUNK(req, c) \
	c == req->parity_chunk ? c+1 : c

/* [한국어] 데이터 청크만(패리티 제외) 순회하는 for 루프. parity 청크를 만나면 자동 skip.
 * map_iovecs(사용자 데이터를 데이터 청크에 분배)와 xor(데이터 청크를 src 로 모음)에 사용. */
#define FOR_EACH_DATA_CHUNK(req, c) \
	for (c = __NEXT_DATA_CHUNK(req, req->chunks); __CHUNK_IN_RANGE(req, c); \
	     c = __NEXT_DATA_CHUNK(req, c+1))

/*
 * [한국어]
 * raid5f_ch_to_r5f_info - raid5f_io_channel 에서 소속 raid5f_info 를 역으로 얻는다.
 *
 * @r5ch: per-channel 컨텍스트 (ctx_buf). spdk_io_channel 의 뒤에 붙어 있는 사용자 영역.
 * @return: 이 채널이 속한 RAID5F 인스턴스의 raid5f_info 포인터.
 *
 * SPDK io_channel 은 io_device(=여기서는 r5f_info)에 종속되며, 채널→디바이스 역참조가
 * spdk_io_channel_get_io_device 로 제공된다. r5ch(ctx_buf)에서 먼저 감싸는 spdk_io_channel
 * 을 복원(spdk_io_channel_from_ctx)한 뒤 그 io_device 를 꺼낸다.
 * 실행 컨텍스트: 채널 소유 스레드. 락 불필요(read-only 조회).
 *
 * 호출 체인:
 *   __CHUNK_IN_RANGE / raid5f_stripe_request_free 등 → [이 함수] → spdk_io_channel_*
 */
static inline struct raid5f_info *
raid5f_ch_to_r5f_info(struct raid5f_io_channel *r5ch)
{
	/* [한국어] ctx_buf → spdk_io_channel → io_device(r5f_info) 2단계 복원. */
	return spdk_io_channel_get_io_device(spdk_io_channel_from_ctx(r5ch));
}

/*
 * [한국어]
 * raid5f_chunk_stripe_req - chunk 포인터로부터 그것이 속한 stripe_request 본체를 복원한다.
 *
 * @chunk: chunks[] 배열의 한 원소 포인터 (bdev I/O 완료 콜백의 cb_arg 로 전달됨).
 * @return: 이 chunk 를 멤버로 갖는 stripe_request 포인터.
 *
 * 베이스 bdev I/O 의 완료 콜백(raid5f_chunk_complete_bdev_io)은 cb_arg 로 chunk 하나만
 * 받는다. 거기서 전체 stripe 상태에 접근하려면 본체를 복원해야 한다.
 * 동작: chunk->index 만큼 빼면 chunks[0] 주소가 되고, chunks[] 는 구조체 끝의 flexible
 * array 이므로 SPDK_CONTAINEROF 로 stripe_request 시작 주소를 역산한다.
 * 실행 컨텍스트: 완료 콜백 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_chunk_complete_bdev_io / raid5f_chunk_submit → [이 함수]
 */
static inline struct stripe_request *
raid5f_chunk_stripe_req(struct chunk *chunk)
{
	/* [한국어] (chunk - index) = chunks[0]; CONTAINEROF 로 stripe_request 본체 추출. */
	return SPDK_CONTAINEROF((chunk - chunk->index), struct stripe_request, chunks);
}

/*
 * [한국어]
 * raid5f_stripe_data_chunks_num - 한 stripe 의 데이터 청크 개수 N 을 반환한다.
 *
 * @raid_bdev: 상위 raid_bdev.
 * @return: 데이터 청크 수 = num_base_bdevs - 1 (= 동작에 필요한 최소 베이스 수).
 *
 * RAID5F 는 정확히 패리티 1개를 두므로 데이터 청크 수 = num_base_bdevs - 1 이며, 이는
 * bdev_raid 코어가 min_base_bdevs_operational 로 이미 계산해 둔 값과 같다. 의미를 명확히
 * 하기 위해 별도 헬퍼로 감쌌다.
 * 실행 컨텍스트: 모든 컨텍스트(순수 계산). 동기화: read-only.
 *
 * 호출 체인:
 *   xor / map_iovecs / alloc 등 다수 → [이 함수]
 */
static inline uint8_t
raid5f_stripe_data_chunks_num(const struct raid_bdev *raid_bdev)
{
	/* [한국어] N(데이터 청크 수) = 동작 최소 베이스 수 = num_base_bdevs - 1. */
	return raid_bdev->min_base_bdevs_operational;
}

/*
 * [한국어]
 * raid5f_stripe_parity_chunk_index - stripe_index 에 대한 패리티 청크의 배열 인덱스를 계산.
 *
 * @raid_bdev: 상위 raid_bdev.
 * @stripe_index: 대상 stripe 번호.
 * @return: chunks[] 배열 내 패리티 청크 인덱스 [0, num_base_bdevs).
 *
 * RAID5 는 핫스팟을 분산시키려 패리티를 stripe 마다 다른 디스크에 두는 "rotating parity"
 * 를 쓴다. 본 구현은 stripe_index 가 1 증가할 때마다 패리티 인덱스가 1 감소(회전)하도록
 * N - (stripe_index mod num_base_bdevs) 로 계산한다.
 * 실행 컨텍스트: 순수 계산. 동기화: read-only.
 *
 * 호출 체인:
 *   raid5f_stripe_request_init / raid5f_submit_read_request → [이 함수]
 */
static inline uint8_t
raid5f_stripe_parity_chunk_index(const struct raid_bdev *raid_bdev, uint64_t stripe_index)
{
	/* [한국어] N - (stripe_index % num_base_bdevs) — stripe 마다 패리티 위치를 회전. */
	return raid5f_stripe_data_chunks_num(raid_bdev) - stripe_index % raid_bdev->num_base_bdevs;
}

/*
 * [한국어]
 * raid5f_stripe_request_release - 다 쓴 stripe_request 를 해당 타입의 free 풀로 반환한다.
 *
 * @stripe_req: 처리가 끝난 (완료/실패 보고까지 마친) stripe 요청.
 * @return: 없음.
 *
 * stripe_request 는 채널 생성 시 미리 할당된 풀에서 빌려 쓰고 끝나면 돌려준다(객체 풀링)
 * — hot-path malloc/free 회피. type 에 따라 write/reconstruct 두 풀 중 맞는 곳으로 반환.
 * HEAD 삽입(LIFO)이라 방금 cache 에 올라온 객체를 곧바로 재사용해 cache 친화적.
 * 실행 컨텍스트: 완료 경로 — 채널 소유 스레드. 락 불필요.
 *
 * 호출 체인:
 *   chunk_write_complete / write_request_xor_done / reconstruct_xor_done 등 → [이 함수]
 */
static inline void
raid5f_stripe_request_release(struct stripe_request *stripe_req)
{
	/* [한국어] WRITE 가 압도적으로 흔하므로 spdk_likely 로 분기 예측 힌트. */
	if (spdk_likely(stripe_req->type == STRIPE_REQ_WRITE)) {
		/* [한국어] write free 풀의 머리에 반환(LIFO) — 직전 사용 객체 재활용. */
		TAILQ_INSERT_HEAD(&stripe_req->r5ch->free_stripe_requests.write, stripe_req, link);
	} else if (stripe_req->type == STRIPE_REQ_RECONSTRUCT) {
		/* [한국어] reconstruct free 풀로 반환. */
		TAILQ_INSERT_HEAD(&stripe_req->r5ch->free_stripe_requests.reconstruct, stripe_req, link);
	} else {
		/* [한국어] 알 수 없는 타입 — 논리 오류이므로 디버그 빌드에서 abort. */
		assert(false);
	}
}

/* [한국어] forward 선언 — xor_stripe_done 이 재시도 큐를 비울 때 호출하나 정의는 아래에 있다. */
static void raid5f_xor_stripe_retry(struct stripe_request *stripe_req);

/*
 * [한국어]
 * raid5f_xor_stripe_done - 한 stripe 의 모든 XOR 가 끝났을 때 최종 콜백을 부르고 재시도 큐를 진행.
 *
 * @stripe_req: XOR(데이터+md)가 모두 완료된 stripe 요청.
 * @return: 없음.
 *
 * 비동기 XOR 의 마지막 단계. 누적된 xor.status 가 에러면 로그를 남기고, 등록된 xor.cb
 * (WRITE: submit_chunks 진행 / RECONSTRUCT: 복원 후처리)를 호출한다. 그 후, 앞서 accel
 * 자원 부족(-ENOMEM)으로 xor_retry_queue 에 보류됐던 요청이 있으면 하나 꺼내 재개 →
 * 자원이 풀린 시점에 backpressure 를 자연스럽게 해소.
 * 실행 컨텍스트: accel 완료 콜백 — 채널 소유 스레드(같은 reactor). 재진입 없음.
 *
 * 호출 체인:
 *   raid5f_xor_stripe_cb/_md_cb → _raid5f_xor_stripe_cb → [이 함수] → xor.cb / xor_stripe_retry
 */
static void
raid5f_xor_stripe_done(struct stripe_request *stripe_req)
{
	struct raid5f_io_channel *r5ch = stripe_req->r5ch;  /* [한국어] 재시도 큐 접근용 채널 캐시. */

	/* [한국어] XOR 누적 에러가 있으면 사람이 읽을 errno 문자열로 로그. */
	if (stripe_req->xor.status != 0) {
		SPDK_ERRLOG("stripe xor failed: %s\n", spdk_strerror(-stripe_req->xor.status));
	}

	/* [한국어] 등록된 완료 콜백 호출 — WRITE/RECONSTRUCT 후속 단계로 진입. */
	stripe_req->xor.cb(stripe_req, stripe_req->xor.status);

	/* [한국어] accel 자원 부족으로 보류된 XOR 가 있으면 지금 자원이 났을 수 있으니 재개. */
	if (!TAILQ_EMPTY(&r5ch->xor_retry_queue)) {
		stripe_req = TAILQ_FIRST(&r5ch->xor_retry_queue);          /* [한국어] 가장 오래 기다린(=HEAD) 요청 선택. */
		TAILQ_REMOVE(&r5ch->xor_retry_queue, stripe_req, link);    /* [한국어] 큐에서 제거. */
		raid5f_xor_stripe_retry(stripe_req);                      /* [한국어] data/md 중 어디서 멈췄는지에 따라 재개. */
	}
}

/* [한국어] forward 선언 — cb 들이 다음 XOR chunk 발행을 위해 호출하나 정의는 아래. */
static void raid5f_xor_stripe_continue(struct stripe_request *stripe_req);

/*
 * [한국어]
 * _raid5f_xor_stripe_cb - data/md XOR 콜백의 공통 후처리: 에러 누적 + 완료 판정.
 *
 * @stripe_req: 진행 중 stripe 요청.
 * @status: 이번 accel XOR 호출의 결과 (0 또는 음수 errno).
 * @return: 없음.
 *
 * data XOR(여러 조각으로 분할될 수 있음)와 md XOR 는 병렬로 진행되며, 둘 다 끝나야
 * (remaining + remaining_md == 0) 전체 완료다. 이 헬퍼는 두 콜백 경로의 마지막에서
 * 첫 에러를 xor.status 에 기록하고, 완료 조건을 만족하면 done 으로 넘긴다.
 * 실행 컨텍스트: accel 완료 콜백 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_xor_stripe_cb / raid5f_xor_stripe_md_cb → [이 함수] → raid5f_xor_stripe_done
 */
static void
_raid5f_xor_stripe_cb(struct stripe_request *stripe_req, int status)
{
	/* [한국어] 첫 에러만 보존(이후 에러로 덮어쓰지 않음) — 최초 실패 원인 추적. */
	if (status != 0) {
		stripe_req->xor.status = status;
	}

	/* [한국어] data·md 양쪽 잔량이 모두 0 이면 stripe 의 XOR 전체 완료. */
	if (stripe_req->xor.remaining + stripe_req->xor.remaining_md == 0) {
		raid5f_xor_stripe_done(stripe_req);
	}
}

/*
 * [한국어]
 * raid5f_xor_stripe_cb - data XOR 한 조각이 완료됐을 때의 accel 콜백.
 *
 * @_stripe_req: accel 에 cb_arg 로 넘겼던 stripe_request (void* → 캐스팅).
 * @status: accel XOR 결과.
 * @return: 없음.
 *
 * src iovec 들이 서로 다른 경계를 가질 수 있어 data XOR 는 ioviter 로 공통 길이만큼씩
 * 여러 번 나눠 처리된다. 이 콜백은 방금 처리한 len 만큼 remaining 을 깎고, 남았으면
 * 다음 공통 구간(nextv)을 구해 다음 XOR 조각을 발행한다. 그 뒤 공통 후처리로 완료를 점검.
 * 실행 컨텍스트: accel 완료 콜백 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   spdk_accel_submit_xor 완료 → [이 함수] → raid5f_xor_stripe_continue / _raid5f_xor_stripe_cb
 */
static void
raid5f_xor_stripe_cb(void *_stripe_req, int status)
{
	struct stripe_request *stripe_req = _stripe_req;  /* [한국어] accel cb_arg 복원. */

	stripe_req->xor.remaining -= stripe_req->xor.len;  /* [한국어] 방금 XOR 한 바이트만큼 잔량 차감. */

	/* [한국어] 아직 data 가 남았으면 다음 공통 구간을 잡아 추가 XOR 발행. */
	if (stripe_req->xor.remaining > 0) {
		/* [한국어] ioviter 를 한 칸 전진시켜 다음 공통 길이와 src 포인터들을 갱신. */
		stripe_req->xor.len = spdk_ioviter_nextv(stripe_req->chunk_iov_iters,
				      stripe_req->chunk_xor_buffers);
		raid5f_xor_stripe_continue(stripe_req);  /* [한국어] 갱신된 src/len 으로 다음 XOR 제출. */
	}

	/* [한국어] data·md 완료 여부 공통 점검 (방금 발행한 다음 조각과 무관히 호출 안전). */
	_raid5f_xor_stripe_cb(stripe_req, status);
}

/*
 * [한국어]
 * raid5f_xor_stripe_md_cb - metadata XOR 가 완료됐을 때의 accel 콜백.
 *
 * @_stripe_req: stripe_request (void* → 캐스팅).
 * @status: accel XOR 결과.
 * @return: 없음.
 *
 * md 버퍼는 scatter-gather 가 아닌 단일 연속 버퍼라 한 번의 XOR 로 끝난다. 따라서
 * 콜백 진입 즉시 remaining_md 를 0 으로 만들고 공통 후처리로 완료를 점검한다.
 * 실행 컨텍스트: accel 완료 콜백 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   spdk_accel_submit_xor(md) 완료 → [이 함수] → _raid5f_xor_stripe_cb
 */
static void
raid5f_xor_stripe_md_cb(void *_stripe_req, int status)
{
	struct stripe_request *stripe_req = _stripe_req;  /* [한국어] accel cb_arg 복원. */

	stripe_req->xor.remaining_md = 0;  /* [한국어] md 는 단일 버퍼 — 한 번에 전부 완료. */

	_raid5f_xor_stripe_cb(stripe_req, status);  /* [한국어] 완료 여부 공통 점검. */
}

/*
 * [한국어]
 * raid5f_xor_stripe_continue - 현재 ioviter 구간의 data XOR 한 조각을 accel 에 제출한다.
 *
 * @stripe_req: xor.len / chunk_xor_buffers 가 세팅된 stripe 요청.
 * @return: 없음 (결과는 콜백/에러 경로로 전달).
 *
 * chunk_xor_buffers 는 [0..n_src-1] 이 src(데이터 청크 포인터), [n_src] 가 dst(결과를 쓸
 * 패리티 또는 복원 대상 버퍼)로 배치돼 있다. accel framework 가 src 들을 모두 XOR 하여
 * dst 에 쓴다(ISA-L SIMD 또는 SW). -ENOMEM 이면 retry 큐로 보류(다른 XOR 완료 시 재개),
 * 그 외 에러면 즉시 done 으로 실패 전파.
 * 실행 컨텍스트: 채널 소유 스레드. accel 은 비동기 — 완료는 raid5f_xor_stripe_cb 로.
 *
 * 호출 체인:
 *   raid5f_xor_stripe / raid5f_xor_stripe_cb → [이 함수] → spdk_accel_submit_xor
 */
static void
raid5f_xor_stripe_continue(struct stripe_request *stripe_req)
{
	struct raid5f_io_channel *r5ch = stripe_req->r5ch;          /* [한국어] accel 채널/retry 큐 접근용. */
	struct raid_bdev_io *raid_io = stripe_req->raid_io;         /* [한국어] 상위 raid I/O. */
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;           /* [한국어] RAID 기하 정보. */
	uint8_t n_src = raid5f_stripe_data_chunks_num(raid_bdev);   /* [한국어] XOR src 개수 = 데이터 청크 수 N. */
	int ret;

	assert(stripe_req->xor.len > 0);  /* [한국어] 0 길이 XOR 발행은 논리 오류. */

	/* [한국어] src=chunk_xor_buffers[0..n_src-1], dst=chunk_xor_buffers[n_src], 길이=xor.len.
	 * 비동기 — 완료 시 raid5f_xor_stripe_cb(stripe_req) 호출. */
	ret = spdk_accel_submit_xor(r5ch->accel_ch, stripe_req->chunk_xor_buffers[n_src],
				    stripe_req->chunk_xor_buffers, n_src, stripe_req->xor.len,
				    raid5f_xor_stripe_cb, stripe_req);
	if (spdk_unlikely(ret)) {  /* [한국어] 제출 자체가 동기 실패한 경우. */
		if (ret == -ENOMEM) {
			/* [한국어] accel 자원 고갈 — retry 큐에 보류하고, 다른 XOR 완료 시 재개. */
			TAILQ_INSERT_HEAD(&r5ch->xor_retry_queue, stripe_req, link);
		} else {
			/* [한국어] 복구 불가 에러 — 상태 기록 후 즉시 완료 처리(실패 전파). */
			stripe_req->xor.status = ret;
			raid5f_xor_stripe_done(stripe_req);
		}
	}
}

/*
 * [한국어]
 * raid5f_xor_stripe - 한 stripe 의 패리티(또는 복원 데이터) XOR 계산을 시작한다.
 *
 * @stripe_req: 이미 각 청크의 iovs/iovcnt(및 md_buf)가 채워진 stripe 요청.
 * @cb: 모든 XOR 완료 시 호출할 콜백 (write: xor_done, reconstruct: reconstruct_xor_done).
 * @return: 없음 (결과는 cb 또는 에러 경로로 전달).
 *
 * 본 함수가 XOR 파이프라인의 진입점이다. WRITE 면 N개 데이터 청크를 XOR 해 패리티 청크에,
 * RECONSTRUCT 면 (정상 데이터 + 패리티) 청크들을 XOR 해 죽은 청크 버퍼에 결과를 쓴다.
 * dst 청크를 제외한 나머지 청크들의 iov 를 chunk_xor_iovs/iovcnt 에 모으고, 마지막 슬롯에
 * dst 를 배치한 뒤 spdk_ioviter_firstv 로 첫 공통 구간을 잡는다. xor.remaining 에 총 바이트
 * 수를 세팅하고, md 가 있으면 md XOR(단일 버퍼)도 병렬 발행, 이어서 data XOR 를 시작한다.
 * 실행 컨텍스트: 채널 소유 스레드. accel 은 비동기 — 같은 reactor 에서 완료 콜백.
 *
 * 호출 체인:
 *   raid5f_submit_write_request / raid5f_reconstruct_reads_completed_cb → [이 함수] →
 *   spdk_ioviter_firstv → spdk_accel_submit_xor(md) → raid5f_xor_stripe_continue(data)
 */
static void
raid5f_xor_stripe(struct stripe_request *stripe_req, stripe_req_xor_cb cb)
{
	struct raid5f_io_channel *r5ch = stripe_req->r5ch;   /* [한국어] accel 채널 + iov 스크래치 배열. */
	struct raid_bdev_io *raid_io = stripe_req->raid_io;  /* [한국어] 상위 raid I/O. */
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;    /* [한국어] RAID 기하 정보. */
	struct chunk *chunk;
	struct chunk *dest_chunk = NULL;  /* [한국어] XOR 결과를 쓸 대상 청크 (패리티 또는 복원 대상). */
	uint64_t num_blocks = 0;          /* [한국어] XOR 할 블록 수 (write=strip 전체, reconstruct=요청 블록). */
	uint8_t c;

	assert(cb != NULL);  /* [한국어] 완료 콜백은 필수. */

	/* [한국어] 타입별로 XOR 대상/길이 결정. WRITE 가 흔하므로 likely. */
	if (spdk_likely(stripe_req->type == STRIPE_REQ_WRITE)) {
		num_blocks = raid_bdev->strip_size;          /* [한국어] full-stripe write — strip 전체 길이. */
		dest_chunk = stripe_req->parity_chunk;       /* [한국어] 결과는 패리티 청크로. */
	} else if (stripe_req->type == STRIPE_REQ_RECONSTRUCT) {
		num_blocks = raid_io->num_blocks;            /* [한국어] reconstruct read — 요청한 블록 수만큼만. */
		dest_chunk = stripe_req->reconstruct.chunk;  /* [한국어] 결과는 복원 대상(죽은) 청크 버퍼로. */
	} else {
		assert(false);  /* [한국어] 알 수 없는 타입 — 논리 오류. */
	}

	/* [한국어] dst 를 제외한 src 청크들의 iov 를 0..c-1 슬롯에 모은다. */
	c = 0;
	FOR_EACH_CHUNK(stripe_req, chunk) {
		if (chunk == dest_chunk) {
			continue;  /* [한국어] dst 청크는 src 가 아니므로 건너뜀. */
		}
		r5ch->chunk_xor_iovs[c] = chunk->iovs;      /* [한국어] 이 src 청크의 iovec 배열. */
		r5ch->chunk_xor_iovcnt[c] = chunk->iovcnt;  /* [한국어] iovec 개수. */
		c++;
	}
	/* [한국어] 마지막 슬롯(c)에 dst 청크 배치 — ioviter 가 dst 도 같은 보폭으로 전진시켜
	 * chunk_xor_buffers[n_src] 에 dst 포인터를 채워주게 한다. */
	r5ch->chunk_xor_iovs[c] = dest_chunk->iovs;
	r5ch->chunk_xor_iovcnt[c] = dest_chunk->iovcnt;

	/* [한국어] ioviter 초기화 — num_base_bdevs 개 iov 묶음을 동시 walk, 첫 공통 길이 반환.
	 * 동시에 chunk_xor_buffers[]에 각 src/dst 의 현재 베이스 포인터를 채운다. */
	stripe_req->xor.len = spdk_ioviter_firstv(stripe_req->chunk_iov_iters,
			      raid_bdev->num_base_bdevs,
			      r5ch->chunk_xor_iovs,
			      r5ch->chunk_xor_iovcnt,
			      stripe_req->chunk_xor_buffers);
	stripe_req->xor.remaining = num_blocks * raid_bdev->bdev.blocklen;  /* [한국어] data XOR 총 바이트. */
	stripe_req->xor.status = 0;  /* [한국어] 에러 누적 상태 초기화. */
	stripe_req->xor.cb = cb;     /* [한국어] 완료 콜백 등록. */

	/* [한국어] metadata(DIF/DIX)가 있으면 md 도 XOR — data 와 병렬 진행. */
	if (raid_io->md_buf != NULL) {
		uint8_t n_src = raid5f_stripe_data_chunks_num(raid_bdev);  /* [한국어] md src 개수 = N. */
		uint64_t len = num_blocks * raid_bdev->bdev.md_len;        /* [한국어] md XOR 총 바이트. */
		int ret;

		stripe_req->xor.remaining_md = len;  /* [한국어] md 잔량 세팅 — md_cb 가 0 으로 만들 예정. */

		/* [한국어] dst 제외 src 청크들의 md_buf 포인터를 모은다 (md 는 단일 버퍼라 iovcnt 불필요). */
		c = 0;
		FOR_EACH_CHUNK(stripe_req, chunk) {
			if (chunk != dest_chunk) {
				stripe_req->chunk_xor_md_buffers[c] = chunk->md_buf;
				c++;
			}
		}

		/* [한국어] md XOR 발행 — dst=dest_chunk->md_buf. 완료 시 raid5f_xor_stripe_md_cb. */
		ret = spdk_accel_submit_xor(stripe_req->r5ch->accel_ch, dest_chunk->md_buf,
					    stripe_req->chunk_xor_md_buffers, n_src, len,
					    raid5f_xor_stripe_md_cb, stripe_req);
		if (spdk_unlikely(ret)) {  /* [한국어] md XOR 제출 동기 실패. */
			if (ret == -ENOMEM) {
				/* [한국어] 자원 고갈 — retry 큐로. 재개 시 raid5f_xor_stripe_retry 가
				 * remaining_md != 0 을 보고 본 함수를 처음부터 다시 호출한다. */
				TAILQ_INSERT_HEAD(&stripe_req->r5ch->xor_retry_queue, stripe_req, link);
			} else {
				stripe_req->xor.status = ret;       /* [한국어] 복구 불가 — 실패 기록. */
				raid5f_xor_stripe_done(stripe_req); /* [한국어] 즉시 완료(실패 전파). */
			}
			return;  /* [한국어] md 발행 실패 시 data XOR 는 시작하지 않고 종료. */
		}
	}

	/* [한국어] data XOR 첫 조각 발행 — 이후 조각은 raid5f_xor_stripe_cb 가 이어서 발행. */
	raid5f_xor_stripe_continue(stripe_req);
}

/*
 * [한국어]
 * raid5f_xor_stripe_retry - retry 큐에서 꺼낸 XOR 요청을 멈춘 지점에서 재개한다.
 *
 * @stripe_req: 앞서 -ENOMEM 으로 보류됐던 stripe 요청.
 * @return: 없음.
 *
 * 보류는 두 지점(md 발행 / data 발행)에서 일어날 수 있다. remaining_md 가 남아 있으면
 * md 발행 전이었다는 뜻이므로 raid5f_xor_stripe 전체를 다시 호출하고(중복 없이 안전한
 * 멱등 구성), 아니면 data XOR 만 이어 발행한다.
 * 실행 컨텍스트: raid5f_xor_stripe_done 에서 호출 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_xor_stripe_done → [이 함수] → raid5f_xor_stripe / raid5f_xor_stripe_continue
 */
static void
raid5f_xor_stripe_retry(struct stripe_request *stripe_req)
{
	/* [한국어] md 잔량이 있으면 md 발행 전에 막힌 것 — 전체 시작점부터 재개. */
	if (stripe_req->xor.remaining_md) {
		raid5f_xor_stripe(stripe_req, stripe_req->xor.cb);
	} else {
		/* [한국어] md 는 이미 끝났고 data XOR 발행에서 막힌 경우 — data 만 재개. */
		raid5f_xor_stripe_continue(stripe_req);
	}
}

/*
 * [한국어]
 * raid5f_stripe_request_chunk_write_complete - WRITE 경로에서 청크 1개 완료를 집계한다.
 *
 * @stripe_req: 진행 중 stripe 요청.
 * @status: 해당 베이스 bdev I/O 결과.
 * @return: 없음.
 *
 * full-stripe write 는 N+1 청크를 병렬 발행하고 모두 완료돼야 사용자 I/O 가 완료된다.
 * raid_bdev_io_complete_part(.., 1, ..)가 "1건 완료" 를 집계하고, 마지막 1건이면 true 를
 * 반환하므로 그때 stripe_request 를 풀로 반환한다.
 * 실행 컨텍스트: 베이스 bdev 완료 콜백 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_chunk_complete_bdev_io → [이 함수] → raid_bdev_io_complete_part / release
 */
static void
raid5f_stripe_request_chunk_write_complete(struct stripe_request *stripe_req,
		enum spdk_bdev_io_status status)
{
	/* [한국어] 청크 1건 완료 집계 — true 면 stripe 의 마지막 청크였으므로 풀로 반환. */
	if (raid_bdev_io_complete_part(stripe_req->raid_io, 1, status)) {
		raid5f_stripe_request_release(stripe_req);
	}
}

/*
 * [한국어]
 * raid5f_stripe_request_chunk_read_complete - RECONSTRUCT 경로에서 청크 1개 읽기 완료를 집계.
 *
 * @stripe_req: reconstruct 진행 중 stripe 요청.
 * @status: 해당 베이스 bdev read 결과.
 * @return: 없음.
 *
 * reconstruct 는 N개 청크를 읽고, 마지막 읽기 완료 시점에 raid_io->completion_cb
 * (raid5f_reconstruct_reads_completed_cb)가 호출되어 XOR 복원으로 이어진다. 따라서 여기서는
 * write 와 달리 release 하지 않고 part 집계만 한다 (release 는 XOR 완료 후 별도 경로에서).
 * 실행 컨텍스트: 베이스 bdev 완료 콜백 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_chunk_complete_bdev_io → [이 함수] → raid_bdev_io_complete_part
 */
static void
raid5f_stripe_request_chunk_read_complete(struct stripe_request *stripe_req,
		enum spdk_bdev_io_status status)
{
	struct raid_bdev_io *raid_io = stripe_req->raid_io;  /* [한국어] 집계 대상 raid I/O. */

	/* [한국어] read 1건 완료 집계. 마지막 1건이면 코어가 completion_cb 를 호출해 XOR 로 진행. */
	raid_bdev_io_complete_part(raid_io, 1, status);
}

/*
 * [한국어]
 * raid5f_chunk_complete_bdev_io - 베이스 bdev I/O 완료 콜백 (write/reconstruct 공통 진입).
 *
 * @bdev_io: 완료된 베이스 bdev I/O 객체 (반드시 free 해야 함).
 * @success: 성공 여부.
 * @cb_arg: 발행 시 넘긴 chunk 포인터.
 * @return: 없음.
 *
 * raid5f_chunk_submit 이 베이스 bdev 에 writev/readv 를 발행할 때 cb_arg=chunk 로 등록한
 * 콜백. chunk 로부터 stripe_req 를 복원하고, bdev_io 를 해제한 뒤 타입별 집계 함수로 분기.
 * 실행 컨텍스트: 베이스 bdev 의 완료 콜백 — 그 베이스 bdev 채널을 처리하는 reactor 스레드
 * (RAID5F 채널과 동일 reactor 에 고정되어 있어 cross-thread 문제 없음).
 *
 * 호출 체인:
 *   lib/bdev 완료 → [이 함수] → chunk_write_complete / chunk_read_complete
 */
static void
raid5f_chunk_complete_bdev_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct chunk *chunk = cb_arg;                                   /* [한국어] 발행 시 넘긴 청크. */
	struct stripe_request *stripe_req = raid5f_chunk_stripe_req(chunk);  /* [한국어] 청크→stripe 본체 복원. */
	enum spdk_bdev_io_status status = success ? SPDK_BDEV_IO_STATUS_SUCCESS :
					  SPDK_BDEV_IO_STATUS_FAILED;  /* [한국어] bool → bdev 상태 코드 변환. */

	spdk_bdev_free_io(bdev_io);  /* [한국어] 베이스 bdev_io 반납 — 누수 방지(필수). */

	/* [한국어] 타입별 완료 집계 분기. WRITE 가 흔하므로 likely. */
	if (spdk_likely(stripe_req->type == STRIPE_REQ_WRITE)) {
		raid5f_stripe_request_chunk_write_complete(stripe_req, status);
	} else if (stripe_req->type == STRIPE_REQ_RECONSTRUCT) {
		raid5f_stripe_request_chunk_read_complete(stripe_req, status);
	} else {
		assert(false);  /* [한국어] 알 수 없는 타입 — 논리 오류. */
	}
}

/* [한국어] forward 선언 — retry 콜백이 호출하나 정의는 아래에 있다. */
static void raid5f_stripe_request_submit_chunks(struct stripe_request *stripe_req);

/*
 * [한국어]
 * raid5f_chunk_submit_retry - 베이스 bdev 가 -ENOMEM 일 때 queue_io_wait 으로 등록된 재시도 콜백.
 *
 * @_raid_io: raid_bdev_io 포인터 (void* → 캐스팅).
 * @return: 없음.
 *
 * raid5f_chunk_submit 이 베이스 bdev I/O 를 -ENOMEM 으로 못 보냈을 때, 해당 bdev 의 io_wait
 * 큐에 본 콜백을 걸어 둔다. 베이스 bdev 에 자원이 나면 코어가 본 콜백을 호출하고, 멈춘
 * 청크 인덱스(base_bdev_io_submitted)부터 발행을 재개한다.
 * 실행 컨텍스트: 베이스 bdev io_wait 처리 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid_bdev_queue_io_wait → [이 함수] → raid5f_stripe_request_submit_chunks
 */
static void
raid5f_chunk_submit_retry(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;                          /* [한국어] raid I/O 복원. */
	struct stripe_request *stripe_req = raid_io->module_private;      /* [한국어] 진행 중 stripe 요청. */

	raid5f_stripe_request_submit_chunks(stripe_req);  /* [한국어] 멈춘 지점부터 청크 발행 재개. */
}

/*
 * [한국어]
 * raid5f_init_ext_io_opts - 베이스 bdev I/O 발행에 쓸 확장 옵션 구조체를 초기화한다.
 *
 * @opts: 출력 — 채울 ext_io_opts (호출자 스택 변수).
 * @raid_io: 사용자 raid I/O — memory_domain/md_buf 출처.
 * @return: 없음.
 *
 * SPDK bdev 의 *_ext I/O API 는 memory domain(예: RDMA/GPU 메모리), metadata 포인터 등
 * 추가 옵션을 ext_io_opts 로 받는다. ABI 호환을 위해 size 를 반드시 sizeof 로 채우고,
 * 사용자 I/O 의 memory domain 컨텍스트와 md_buf 를 그대로 전달한다.
 * 실행 컨텍스트: 발행 직전 — 채널 소유 스레드. 순수 메모리 초기화.
 *
 * 호출 체인:
 *   raid5f_chunk_submit / raid5f_submit_read_request / raid5f_process_submit_write → [이 함수]
 */
static inline void
raid5f_init_ext_io_opts(struct spdk_bdev_ext_io_opts *opts, struct raid_bdev_io *raid_io)
{
	memset(opts, 0, sizeof(*opts));                       /* [한국어] 전 필드 0 초기화(미설정 필드 안전값). */
	opts->size = sizeof(*opts);                           /* [한국어] ABI 버전 협상용 — 구조체 크기 명시. */
	opts->memory_domain = raid_io->memory_domain;         /* [한국어] 사용자 버퍼의 메모리 도메인(RDMA/GPU 등) 전파. */
	opts->memory_domain_ctx = raid_io->memory_domain_ctx; /* [한국어] 그 도메인의 컨텍스트 포인터. */
	opts->metadata = raid_io->md_buf;                     /* [한국어] DIF/DIX metadata 기본 버퍼(청크별로 덮어씀). */
}

/*
 * [한국어]
 * raid5f_chunk_submit - 한 청크를 대응 베이스 bdev 에 writev/readv 로 발행한다.
 *
 * @chunk: 발행할 청크 (chunk->index 로 베이스 bdev 식별).
 * @return: 0 또는 음수 errno (-ENOMEM 은 호출자가 backpressure 로 처리).
 *
 * stripe 의 각 청크를 실제 베이스 bdev I/O 로 바꾸는 핵심 함수. base_offset_blocks 는
 * stripe_index << strip_size_shift (= stripe_index * strip_size) 로 계산 — 모든 베이스
 * bdev 에서 같은 오프셋에 같은 stripe 가 놓이는 RAID5F 의 레이아웃 덕분이다.
 * WRITE: 죽은(채널 NULL) 베이스는 즉시 성공 집계(패리티/잉여 덕에 데이터 보존), 나머지는
 *        strip_size 만큼 writev. RECONSTRUCT: 복원 대상 청크 자신은 읽지 않고(곧 XOR 로 채움)
 *        성공 집계, 나머지는 num_blocks 만큼 readv (chunk_offset 가산).
 * 에러 경로: -ENOMEM 이면 io_wait 큐에 재시도 등록(backpressure). 그 외 에러면 아직 발행
 * 못한 나머지 I/O 를 모두 FAILED 로 묵시 집계하고 마지막이면 stripe_request 반환.
 * 실행 컨텍스트: 채널 소유 스레드. 완료는 비동기(raid5f_chunk_complete_bdev_io).
 *
 * 호출 체인:
 *   raid5f_stripe_request_submit_chunks → [이 함수] →
 *   raid_bdev_writev/readv_blocks_ext → 베이스 bdev
 */
static int
raid5f_chunk_submit(struct chunk *chunk)
{
	struct stripe_request *stripe_req = raid5f_chunk_stripe_req(chunk);  /* [한국어] 청크→stripe 본체. */
	struct raid_bdev_io *raid_io = stripe_req->raid_io;                  /* [한국어] 상위 raid I/O. */
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;                    /* [한국어] RAID 기하 정보. */
	struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[chunk->index]; /* [한국어] 대상 베이스 bdev 정보. */
	struct spdk_io_channel *base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch,
					  chunk->index);  /* [한국어] 그 베이스 bdev 의 채널 — NULL 이면 디바이스 죽음/미가용. */
	uint64_t base_offset_blocks = (stripe_req->stripe_index << raid_bdev->strip_size_shift); /* [한국어] stripe_index * strip_size = 베이스 내 시작 블록. */
	struct spdk_bdev_ext_io_opts io_opts;
	int ret;

	raid5f_init_ext_io_opts(&io_opts, raid_io);  /* [한국어] memory domain 등 공통 옵션 채움. */
	io_opts.metadata = chunk->md_buf;            /* [한국어] 이 청크 전용 md 버퍼로 덮어씀(청크마다 다름). */

	raid_io->base_bdev_io_submitted++;  /* [한국어] 발행 카운트 증가(완료 집계/에러 롤백 기준). */

	switch (stripe_req->type) {
	case STRIPE_REQ_WRITE:
		if (base_ch == NULL) {
			/* [한국어] 죽은 베이스 — 패리티 보호로 데이터는 살아있으니 이 청크는 성공으로 집계.
			 * (full-stripe write 이므로 나머지 N개로 복원 가능). */
			raid_bdev_io_complete_part(raid_io, 1, SPDK_BDEV_IO_STATUS_SUCCESS);
			return 0;
		}

		/* [한국어] strip 전체를 베이스 bdev 에 writev. 완료 시 raid5f_chunk_complete_bdev_io. */
		ret = raid_bdev_writev_blocks_ext(base_info, base_ch, chunk->iovs, chunk->iovcnt,
						  base_offset_blocks, raid_bdev->strip_size,
						  raid5f_chunk_complete_bdev_io, chunk, &io_opts);
		break;
	case STRIPE_REQ_RECONSTRUCT:
		if (chunk == stripe_req->reconstruct.chunk) {
			/* [한국어] 복원 대상 청크 자신은 읽지 않는다(죽었거나 곧 XOR 로 채워질 값). 성공 집계. */
			raid_bdev_io_complete_part(raid_io, 1, SPDK_BDEV_IO_STATUS_SUCCESS);
			return 0;
		}

		base_offset_blocks += stripe_req->reconstruct.chunk_offset;  /* [한국어] strip 일부만 읽을 때의 시작 오프셋 가산. */

		/* [한국어] 정상 청크(데이터+패리티)를 num_blocks 만큼 readv — XOR 입력으로 사용. */
		ret = raid_bdev_readv_blocks_ext(base_info, base_ch, chunk->iovs, chunk->iovcnt,
						 base_offset_blocks, raid_io->num_blocks,
						 raid5f_chunk_complete_bdev_io, chunk, &io_opts);
		break;
	default:
		assert(false);   /* [한국어] 알 수 없는 타입 — 논리 오류. */
		ret = -EINVAL;
		break;
	}

	if (spdk_unlikely(ret)) {  /* [한국어] 발행 동기 실패. */
		raid_io->base_bdev_io_submitted--;  /* [한국어] 카운트 롤백(실제로 안 나갔으므로). */
		if (ret == -ENOMEM) {
			/* [한국어] 베이스 bdev 자원 고갈 — io_wait 큐에 재시도 등록, 자원 나면 재개. */
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
						base_ch, raid5f_chunk_submit_retry);
		} else {
			/*
			 * Implicitly complete any I/Os not yet submitted as FAILED. If completing
			 * these means there are no more to complete for the stripe request, we can
			 * release the stripe request as well.
			 */
			/* [한국어] 복구 불가 에러 — 아직 발행 못한 나머지 청크 수를 계산해 한꺼번에
			 * FAILED 로 집계. submit_chunks 루프는 이 함수의 0 아닌 반환을 보고 멈춘다. */
			uint64_t base_bdev_io_not_submitted;

			if (stripe_req->type == STRIPE_REQ_WRITE) {
				/* [한국어] WRITE 는 N+1 개 전부가 대상 — 발행분을 뺀 나머지. */
				base_bdev_io_not_submitted = raid_bdev->num_base_bdevs -
							     raid_io->base_bdev_io_submitted;
			} else {
				/* [한국어] RECONSTRUCT 는 N개(복원 대상 제외)가 대상. */
				base_bdev_io_not_submitted = raid5f_stripe_data_chunks_num(raid_bdev) -
							     raid_io->base_bdev_io_submitted;
			}

			/* [한국어] 미발행분 일괄 FAILED 집계 — 마지막이면 stripe_request 풀로 반환. */
			if (raid_bdev_io_complete_part(raid_io, base_bdev_io_not_submitted,
						       SPDK_BDEV_IO_STATUS_FAILED)) {
				raid5f_stripe_request_release(stripe_req);
			}
		}
	}

	return ret;  /* [한국어] 0/에러 — submit_chunks 루프가 0 아니면 중단. */
}

/*
 * [한국어]
 * raid5f_chunk_set_iovcnt - 청크의 iovec 배열을 필요한 개수로 (필요 시 realloc 하여) 맞춘다.
 *
 * @chunk: 대상 청크.
 * @iovcnt: 필요한 iovec 개수.
 * @return: 0 또는 -ENOMEM (realloc 실패).
 *
 * 청크 iov 배열은 alloc 시 4개로 시작하고, 사용자 I/O 의 scatter-gather 가 더 많은 조각을
 * 요구하면 그때그때 realloc 해 용량(iovcnt_max)을 키운다. capacity 가 충분하면 realloc
 * 없이 iovcnt 만 갱신 — 풀에서 재사용되며 점차 최대 필요량으로 수렴해 amortized O(1).
 * 실행 컨텍스트: 발행 준비 단계 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_stripe_request_map_iovecs / raid5f_submit_reconstruct_read → [이 함수] → realloc
 */
static int
raid5f_chunk_set_iovcnt(struct chunk *chunk, int iovcnt)
{
	/* [한국어] 현재 capacity 보다 더 필요할 때만 realloc(축소는 하지 않음 — 재사용 대비). */
	if (iovcnt > chunk->iovcnt_max) {
		struct iovec *iovs = chunk->iovs;  /* [한국어] 기존 배열 보존(realloc 실패 시 원본 유지). */

		iovs = realloc(iovs, iovcnt * sizeof(*iovs));  /* [한국어] 새 크기로 재할당. */
		if (!iovs) {
			return -ENOMEM;  /* [한국어] 실패 — 원본 chunk->iovs 는 그대로 유효. */
		}
		chunk->iovs = iovs;            /* [한국어] 새 배열로 교체. */
		chunk->iovcnt_max = iovcnt;    /* [한국어] capacity 갱신. */
	}
	chunk->iovcnt = iovcnt;  /* [한국어] 실제 사용 개수 설정. */

	return 0;
}

/*
 * [한국어]
 * raid5f_stripe_request_map_iovecs - 사용자 stripe 데이터를 N개 데이터 청크의 iovec 로 분할 매핑.
 *
 * @stripe_req: WRITE 용 stripe 요청 (raid_io 에 사용자 iov 가 들어 있음).
 * @return: 0 또는 음수 errno (-ENOMEM realloc 실패, -EINVAL 길이 불일치).
 *
 * full-stripe write 의 핵심 전처리. 사용자가 보낸 연속 논리 데이터(raid_io->iovs)를
 * strip_size 단위로 잘라 데이터 청크들의 iov 에 "복사 없이" 매핑한다 (iov_base 포인터만
 * 가리킴 — zero-copy). 사용자 iov 경계와 strip 경계가 어긋날 수 있으므로, raid_io 의 어느
 * iov/어느 오프셋까지 소비했는지(raid_io_iov_idx/raid_io_iov_offset/raid_io_offset)를
 * 추적하며 진행한다. 데이터 청크를 모두 채운 뒤, 패리티 청크는 미리 alloc 한 parity_buf
 * (XOR 결과가 채워질 곳)를 단일 iov 로 가리키게 한다.
 * md 가 있으면 각 청크의 md_buf 도 사용자 md_buf 에서 블록 오프셋(>> blocklen_shift)으로 계산.
 * 실행 컨텍스트: WRITE 발행 준비 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_submit_write_request → [이 함수] → raid5f_chunk_set_iovcnt
 */
static int
raid5f_stripe_request_map_iovecs(struct stripe_request *stripe_req)
{
	struct raid_bdev_io *raid_io = stripe_req->raid_io;          /* [한국어] 사용자 입력 iov 출처. */
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;            /* [한국어] RAID 기하 정보. */
	struct raid5f_info *r5f_info = raid_bdev->module_private;    /* [한국어] blocklen_shift 등 최적화 상수. */
	struct chunk *chunk;
	int raid_io_iov_idx = 0;       /* [한국어] 현재 소비 중인 사용자 iov 인덱스. */
	size_t raid_io_offset = 0;     /* [한국어] 전체 stripe 에서 지금까지 소비한 바이트(논리 오프셋). */
	size_t raid_io_iov_offset = 0; /* [한국어] 현재 iov 의 시작에 해당하는 누적 바이트(경계 추적). */
	int i;

	/* [한국어] 데이터 청크만 순회(패리티는 따로 처리). 각 청크에 strip_size 바이트씩 매핑. */
	FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
		int chunk_iovcnt = 0;  /* [한국어] 이 청크가 걸치는 사용자 iov 조각 수. */
		uint64_t len = raid_bdev->strip_size * raid_bdev->bdev.blocklen;  /* [한국어] 한 청크에 채울 바이트(strip 전체). */
		size_t off = raid_io_iov_offset;  /* [한국어] 몇 개의 사용자 iov 가 필요한지 미리 세기 위한 임시 누적. */
		int ret;

		/* [한국어] 이 청크 한 칸(len 바이트)을 덮는 데 필요한 사용자 iov 개수를 먼저 센다. */
		for (i = raid_io_iov_idx; i < raid_io->iovcnt; i++) {
			chunk_iovcnt++;
			off += raid_io->iovs[i].iov_len;
			if (off >= raid_io_offset + len) {
				break;  /* [한국어] 누적이 청크 끝을 덮었으면 충분. */
			}
		}

		assert(raid_io_iov_idx + chunk_iovcnt <= raid_io->iovcnt);  /* [한국어] 사용자 iov 범위 초과 금지. */

		ret = raid5f_chunk_set_iovcnt(chunk, chunk_iovcnt);  /* [한국어] 청크 iov 배열을 필요한 개수로 확장. */
		if (ret) {
			return ret;  /* [한국어] realloc 실패 전파. */
		}

		/* [한국어] md 가 있으면 이 청크의 md_buf 를 사용자 md_buf 에서 블록 오프셋으로 계산.
		 * (raid_io_offset >> blocklen_shift) = 지금까지 소비한 블록 수. */
		if (raid_io->md_buf != NULL) {
			chunk->md_buf = raid_io->md_buf +
					(raid_io_offset >> r5f_info->blocklen_shift) * raid_bdev->bdev.md_len;
		}

		/* [한국어] 실제 iov 채우기 — 사용자 iov 를 청크 iov 로 포인터/길이만 분할 매핑(zero-copy). */
		for (i = 0; i < chunk_iovcnt; i++) {
			struct iovec *chunk_iov = &chunk->iovs[i];                    /* [한국어] 채울 청크 iov. */
			const struct iovec *raid_io_iov = &raid_io->iovs[raid_io_iov_idx]; /* [한국어] 현재 사용자 iov. */
			size_t chunk_iov_offset = raid_io_offset - raid_io_iov_offset; /* [한국어] 현재 사용자 iov 내부 오프셋. */

			chunk_iov->iov_base = raid_io_iov->iov_base + chunk_iov_offset; /* [한국어] 사용자 버퍼 안쪽을 가리킴. */
			chunk_iov->iov_len = spdk_min(len, raid_io_iov->iov_len - chunk_iov_offset); /* [한국어] 남은 청크 길이와 이 iov 잔량 중 작은 값. */
			raid_io_offset += chunk_iov->iov_len;  /* [한국어] 전체 소비량 전진. */
			len -= chunk_iov->iov_len;             /* [한국어] 이 청크 남은 길이 감소. */

			/* [한국어] 현재 사용자 iov 를 다 소비했으면 다음 iov 로 진행 + 경계 갱신. */
			if (raid_io_offset >= raid_io_iov_offset + raid_io_iov->iov_len) {
				raid_io_iov_idx++;
				raid_io_iov_offset += raid_io_iov->iov_len;
			}
		}

		if (spdk_unlikely(len > 0)) {
			return -EINVAL;  /* [한국어] strip 을 다 못 채움 — 입력 길이 불일치(논리 오류). */
		}
	}

	/* [한국어] 패리티 청크는 미리 alloc 한 parity_buf 를 단일 iov 로 — XOR 가 여기에 결과를 쓴다. */
	stripe_req->parity_chunk->iovs[0].iov_base = stripe_req->write.parity_buf;
	stripe_req->parity_chunk->iovs[0].iov_len = raid_bdev->strip_size * raid_bdev->bdev.blocklen;
	stripe_req->parity_chunk->iovcnt = 1;
	stripe_req->parity_chunk->md_buf = stripe_req->write.parity_md_buf;  /* [한국어] 패리티 md 버퍼. */

	return 0;
}

/*
 * [한국어]
 * raid5f_stripe_request_submit_chunks - stripe 의 청크들을 (아직 안 보낸 것부터) 차례로 발행.
 *
 * @stripe_req: 발행할 stripe 요청.
 * @return: 없음.
 *
 * base_bdev_io_submitted 인덱스부터 시작해 각 청크를 raid5f_chunk_submit 으로 발행한다.
 * 이렇게 인덱스 기반으로 재개 가능하게 한 이유: 베이스 bdev 가 -ENOMEM 을 내면 chunk_submit
 * 이 0 아닌 값을 반환하고 루프가 멈추는데, io_wait 재시도(raid5f_chunk_submit_retry)가
 * 다시 이 함수를 불러 멈춘 인덱스부터 이어서 발행하기 때문이다.
 * 실행 컨텍스트: 채널 소유 스레드.
 *
 * 호출 체인:
 *   write_request_xor_done / submit_reconstruct_read / chunk_submit_retry → [이 함수] → raid5f_chunk_submit
 */
static void
raid5f_stripe_request_submit_chunks(struct stripe_request *stripe_req)
{
	struct raid_bdev_io *raid_io = stripe_req->raid_io;  /* [한국어] 진행 인덱스 출처. */
	struct chunk *start = &stripe_req->chunks[raid_io->base_bdev_io_submitted]; /* [한국어] 아직 안 보낸 첫 청크. */
	struct chunk *chunk;

	/* [한국어] start 부터 모든 청크 발행. chunk_submit 이 0 아니면(-ENOMEM 등) 즉시 중단 —
	 * 나머지는 재시도 콜백이 이어서 처리. */
	FOR_EACH_CHUNK_FROM(stripe_req, chunk, start) {
		if (spdk_unlikely(raid5f_chunk_submit(chunk) != 0)) {
			break;
		}
	}
}

/*
 * [한국어]
 * raid5f_stripe_request_init - stripe 요청의 공통 필드(raid_io/stripe_index/parity_chunk)를 세팅.
 *
 * @stripe_req: 풀에서 막 꺼낸 stripe 요청.
 * @raid_io: 처리할 사용자 raid I/O.
 * @stripe_index: 대상 stripe 번호.
 * @return: 없음.
 *
 * write/reconstruct 두 경로가 공유하는 초기화. rotating parity 때문에 stripe 마다 패리티
 * 위치가 달라, 여기서 raid5f_stripe_parity_chunk_index 로 이번 stripe 의 패리티 청크
 * 포인터를 미리 계산해 둔다.
 * 실행 컨텍스트: 발행 진입 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_submit_write_request / raid5f_submit_reconstruct_read → [이 함수]
 */
static inline void
raid5f_stripe_request_init(struct stripe_request *stripe_req, struct raid_bdev_io *raid_io,
			   uint64_t stripe_index)
{
	stripe_req->raid_io = raid_io;          /* [한국어] 사용자 I/O 연결. */
	stripe_req->stripe_index = stripe_index; /* [한국어] base_offset 계산에 사용. */
	/* [한국어] 이번 stripe 의 패리티 청크 포인터 — rotating parity 위치 계산. */
	stripe_req->parity_chunk = &stripe_req->chunks[raid5f_stripe_parity_chunk_index(raid_io->raid_bdev,
				   stripe_index)];
}

/*
 * [한국어]
 * raid5f_stripe_write_request_xor_done - WRITE 의 패리티 XOR 완료 후속 콜백.
 *
 * @stripe_req: XOR 가 끝난 WRITE stripe 요청.
 * @status: XOR 결과 (0=성공).
 * @return: 없음.
 *
 * 패리티 계산이 끝나면 비로소 N+1 청크를 베이스 bdev 에 발행할 수 있다. XOR 가 실패했으면
 * stripe 요청을 풀로 반환하고 사용자 I/O 를 FAILED 로 완료, 성공이면 청크 발행을 시작한다.
 * (패리티 청크의 베이스가 죽어 XOR 를 건너뛴 경우엔 status=0 으로 이 함수가 직접 불린다.)
 * 실행 컨텍스트: XOR 완료 콜백 또는 직접 호출 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_xor_stripe_done(xor.cb) / raid5f_submit_write_request → [이 함수] → submit_chunks / io_complete
 */
static void
raid5f_stripe_write_request_xor_done(struct stripe_request *stripe_req, int status)
{
	struct raid_bdev_io *raid_io = stripe_req->raid_io;  /* [한국어] 완료 보고 대상. */

	if (status != 0) {
		/* [한국어] 패리티 계산 실패 — stripe 반환 후 사용자 I/O 실패 완료. */
		raid5f_stripe_request_release(stripe_req);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	} else {
		/* [한국어] 패리티 준비 완료 — N+1 청크 병렬 발행 시작. */
		raid5f_stripe_request_submit_chunks(stripe_req);
	}
}

/*
 * [한국어]
 * raid5f_submit_write_request - full-stripe WRITE 처리 진입점.
 *
 * @raid_io: 사용자 WRITE I/O (offset/num_blocks 가 정확히 stripe 경계·크기여야 함).
 * @stripe_index: 대상 stripe 번호.
 * @return: 0 또는 음수 errno (-ENOMEM 풀 고갈, map_iovecs 에러).
 *
 * WRITE 흐름: 풀에서 write stripe_req 확보 → 공통 init → 사용자 데이터를 데이터 청크 iov
 * 로 분할 매핑 → 풀에서 제거 → 패리티 청크의 베이스가 살아있으면 XOR 계산 후 발행,
 * 죽었으면 패리티 계산을 건너뛰고 바로 데이터 청크만 발행(xor_done(.,0) 직접 호출).
 * 풀이 비었으면(-ENOMEM) 호출자(submit_rw_request)가 NOMEM 으로 완료 → 코어가 재시도.
 * 실행 컨텍스트: 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_submit_rw_request → [이 함수] → raid5f_xor_stripe / raid5f_stripe_write_request_xor_done
 */
static int
raid5f_submit_write_request(struct raid_bdev_io *raid_io, uint64_t stripe_index)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;  /* [한국어] RAID 기하 정보. */
	struct raid5f_io_channel *r5ch = raid_bdev_channel_get_module_ctx(raid_io->raid_ch); /* [한국어] per-channel 풀. */
	struct stripe_request *stripe_req;
	int ret;

	stripe_req = TAILQ_FIRST(&r5ch->free_stripe_requests.write);  /* [한국어] write 풀에서 하나 확보(아직 제거 안 함). */
	if (!stripe_req) {
		return -ENOMEM;  /* [한국어] 동시 진행 stripe 가 32개 다 찼음 — 코어가 재시도. */
	}

	raid5f_stripe_request_init(stripe_req, raid_io, stripe_index);  /* [한국어] 공통 필드 세팅(parity_chunk 계산 포함). */

	ret = raid5f_stripe_request_map_iovecs(stripe_req);  /* [한국어] 사용자 데이터를 데이터 청크 iov 로 분할. */
	if (spdk_unlikely(ret)) {
		return ret;  /* [한국어] 매핑 실패 — 아직 풀에서 제거 전이라 그냥 반환(객체 누수 없음). */
	}

	TAILQ_REMOVE(&r5ch->free_stripe_requests.write, stripe_req, link);  /* [한국어] 성공 확정 — 풀에서 제거. */

	raid_io->module_private = stripe_req;                       /* [한국어] 재시도 콜백이 stripe_req 복원하도록 저장. */
	raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs; /* [한국어] 완료 집계 목표치 = N+1 청크. */

	/* [한국어] 패리티 청크의 베이스 채널이 살아있으면 XOR 계산 필요, 없으면 건너뛰고 바로 발행. */
	if (raid_bdev_channel_get_base_channel(raid_io->raid_ch, stripe_req->parity_chunk->index) != NULL) {
		raid5f_xor_stripe(stripe_req, raid5f_stripe_write_request_xor_done);  /* [한국어] 패리티 XOR → 완료 시 발행. */
	} else {
		raid5f_stripe_write_request_xor_done(stripe_req, 0);  /* [한국어] 패리티 디스크 죽음 — XOR 불필요, 즉시 발행. */
	}

	return 0;
}

/*
 * [한국어]
 * raid5f_chunk_read_complete - 정상(non-degraded) READ 의 베이스 bdev 완료 콜백.
 *
 * @bdev_io: 완료된 베이스 read I/O (free 필요).
 * @success: 성공 여부.
 * @cb_arg: 사용자 raid_io.
 * @return: 없음.
 *
 * 정상 READ 는 단일 청크 readv 한 번이면 끝나므로(빠른 경로), 베이스 완료 즉시 사용자
 * I/O 를 그 결과로 완료한다. degraded 경로(reconstruct)와 달리 stripe_request 를 쓰지 않는다.
 * 실행 컨텍스트: 베이스 bdev 완료 콜백 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   lib/bdev 완료 → [이 함수] → raid_bdev_io_complete
 */
static void
raid5f_chunk_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;  /* [한국어] 완료 보고 대상. */

	spdk_bdev_free_io(bdev_io);  /* [한국어] 베이스 bdev_io 반납(필수). */

	/* [한국어] 단일 청크 읽기 결과를 그대로 사용자 I/O 완료로 전달. */
	raid_bdev_io_complete(raid_io, success ? SPDK_BDEV_IO_STATUS_SUCCESS :
			      SPDK_BDEV_IO_STATUS_FAILED);
}

/* [한국어] forward 선언 — io_wait 재시도 래퍼가 호출하나 정의는 아래. */
static void raid5f_submit_rw_request(struct raid_bdev_io *raid_io);

/*
 * [한국어]
 * _raid5f_submit_rw_request - submit_rw_request 의 void* 시그니처 어댑터(io_wait 재시도용).
 *
 * @_raid_io: raid_bdev_io (void* → 캐스팅).
 * @return: 없음.
 *
 * raid_bdev_queue_io_wait 는 void(*)(void*) 콜백을 요구하므로, 정상 READ 가 -ENOMEM 으로
 * 막혔을 때 재시도용 래퍼로 이 함수를 등록한다. 자원이 나면 코어가 호출 → 원 진입점 재실행.
 * 실행 컨텍스트: io_wait 처리 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid_bdev_queue_io_wait → [이 함수] → raid5f_submit_rw_request
 */
static void
_raid5f_submit_rw_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;  /* [한국어] raid I/O 복원. */

	raid5f_submit_rw_request(raid_io);  /* [한국어] 원 진입점 재실행. */
}

/*
 * [한국어]
 * raid5f_stripe_request_reconstruct_xor_done - degraded READ 의 복원 XOR 완료 후속 콜백.
 *
 * @stripe_req: 복원 XOR 가 끝난 reconstruct stripe 요청.
 * @status: XOR 결과 (0=성공).
 * @return: 없음.
 *
 * reconstruct read 의 마지막 단계. 정상 청크들을 XOR 해 죽은 청크 데이터를 사용자 iov 에
 * 직접 복원한 뒤, stripe 요청을 풀로 반환하고 사용자 READ 를 완료한다.
 * 실행 컨텍스트: XOR 완료 콜백 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_reconstruct_reads_completed_cb → raid5f_xor_stripe → ... → (xor.cb) [이 함수]
 */
static void
raid5f_stripe_request_reconstruct_xor_done(struct stripe_request *stripe_req, int status)
{
	struct raid_bdev_io *raid_io = stripe_req->raid_io;  /* [한국어] 완료 보고 대상. */

	raid5f_stripe_request_release(stripe_req);  /* [한국어] 복원 끝 — stripe 요청 풀로 반환. */

	/* [한국어] XOR 성공 여부를 사용자 READ 완료 상태로 전달. */
	raid_bdev_io_complete(raid_io,
			      status == 0 ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

/*
 * [한국어]
 * raid5f_reconstruct_reads_completed_cb - reconstruct 용 N개 청크 읽기가 모두 끝났을 때의 코어 콜백.
 *
 * @raid_io: reconstruct 진행 중 raid I/O.
 * @status: 누적 읽기 결과.
 * @return: 없음.
 *
 * raid_io->completion_cb 로 등록되어, 베이스 청크 읽기들이 모두 완료(raid_bdev_io_complete_part
 * 의 마지막 집계)되는 시점에 코어가 호출한다. 읽기가 성공했으면 XOR 로 죽은 청크를 복원하고,
 * 하나라도 실패했으면 XOR 를 건너뛰고 등록된 xor.cb 에 -EIO 로 실패를 전달한다.
 * completion_cb 를 NULL 로 비워 재진입을 막는다(코어가 다시 부르지 않도록).
 * 실행 컨텍스트: 마지막 베이스 read 완료 시점 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid_bdev_io_complete_part(마지막) → completion_cb → [이 함수] → raid5f_xor_stripe / xor.cb
 */
static void
raid5f_reconstruct_reads_completed_cb(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	struct stripe_request *stripe_req = raid_io->module_private;  /* [한국어] 진행 중 reconstruct 요청. */

	raid_io->completion_cb = NULL;  /* [한국어] 1회성 콜백 — 비워 재진입 방지. */

	/* [한국어] 읽기 중 하나라도 실패하면 복원 불가 — XOR 생략하고 실패 콜백. */
	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		stripe_req->xor.cb(stripe_req, -EIO);
		return;
	}

	/* [한국어] 모든 읽기 성공 — XOR 로 죽은 청크 복원 후 xor.cb 호출. */
	raid5f_xor_stripe(stripe_req, stripe_req->xor.cb);
}

/*
 * [한국어]
 * raid5f_submit_reconstruct_read - degraded READ (또는 process rebuild) 의 reconstruct 발행.
 *
 * @raid_io: 사용자 READ (또는 process 의 합성) I/O.
 * @stripe_index: 대상 stripe.
 * @chunk_idx: 복원할(죽은/대상) 청크의 배열 인덱스.
 * @chunk_offset: strip 내부 읽기 시작 오프셋(블록).
 * @cb: XOR 복원 완료 시 호출할 콜백 (일반 READ vs process write 분기).
 * @return: 0 또는 음수 errno (-ENOMEM 풀 고갈 또는 iov realloc 실패).
 *
 * 죽은 청크를 나머지 N개(정상 데이터 + 패리티)의 XOR 로 복원하는 경로. 복원 대상 청크의
 * iov 는 사용자 raid_io->iovs 를 그대로 복사해 가리키게 하여, XOR 결과가 사용자 버퍼에
 * 바로 쓰이게 한다(zero-copy 복원). 나머지 청크들은 미리 alloc 한 reconstruct.chunk_buffers
 * 를 단일 iov 로 가리켜 거기로 데이터를 읽는다. completion_cb 를 걸어, N개 읽기가 모두
 * 끝나면 reconstruct_reads_completed_cb → XOR 로 이어지게 한다.
 * 실행 컨텍스트: 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_submit_read_request / raid5f_submit_process_request → [이 함수] →
 *   submit_chunks → (모두 완료) reconstruct_reads_completed_cb → raid5f_xor_stripe
 */
static int
raid5f_submit_reconstruct_read(struct raid_bdev_io *raid_io, uint64_t stripe_index,
			       uint8_t chunk_idx, uint64_t chunk_offset, stripe_req_xor_cb cb)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;  /* [한국어] RAID 기하 정보. */
	struct raid5f_io_channel *r5ch = raid_bdev_channel_get_module_ctx(raid_io->raid_ch); /* [한국어] reconstruct 풀. */
	void *raid_io_md = raid_io->md_buf;  /* [한국어] 사용자 md 버퍼(있으면 복원 대상 청크에 직접 매핑). */
	struct stripe_request *stripe_req;
	struct chunk *chunk;
	int buf_idx;  /* [한국어] reconstruct.chunk_buffers 의 다음 사용 인덱스(복원 대상 제외 청크에 1:1). */

	assert(cb != NULL);  /* [한국어] 완료 콜백 필수. */

	stripe_req = TAILQ_FIRST(&r5ch->free_stripe_requests.reconstruct);  /* [한국어] reconstruct 풀에서 확보. */
	if (!stripe_req) {
		return -ENOMEM;  /* [한국어] 풀 고갈 — 코어가 재시도. */
	}

	raid5f_stripe_request_init(stripe_req, raid_io, stripe_index);  /* [한국어] 공통 필드 세팅. */

	stripe_req->reconstruct.chunk = &stripe_req->chunks[chunk_idx];  /* [한국어] 복원 대상 청크 지정. */
	stripe_req->reconstruct.chunk_offset = chunk_offset;             /* [한국어] strip 내부 시작 오프셋. */
	stripe_req->xor.cb = cb;                                         /* [한국어] 복원 완료 콜백 미리 저장. */
	buf_idx = 0;

	/* [한국어] 모든 청크 순회하며 iov 구성: 복원 대상은 사용자 버퍼, 나머지는 읽기 버퍼. */
	FOR_EACH_CHUNK(stripe_req, chunk) {
		if (chunk == stripe_req->reconstruct.chunk) {
			/* [한국어] 복원 대상 청크 — 사용자 iov 를 그대로 복사해 XOR 결과가 사용자 버퍼로. */
			int i;
			int ret;

			ret = raid5f_chunk_set_iovcnt(chunk, raid_io->iovcnt);  /* [한국어] 사용자 iov 개수만큼 확장. */
			if (ret) {
				return ret;  /* [한국어] realloc 실패 — 아직 풀 제거 전이라 누수 없음. */
			}

			for (i = 0; i < raid_io->iovcnt; i++) {
				chunk->iovs[i] = raid_io->iovs[i];  /* [한국어] 사용자 iov 그대로 복사(포인터 공유). */
			}

			chunk->md_buf = raid_io_md;  /* [한국어] md 도 사용자 버퍼로 — 복원 결과가 바로 들어감. */
		} else {
			/* [한국어] 읽어올 정상 청크 — 전용 읽기 버퍼를 단일 iov 로 가리킴. */
			struct iovec *iov = &chunk->iovs[0];

			iov->iov_base = stripe_req->reconstruct.chunk_buffers[buf_idx]; /* [한국어] 미리 alloc 한 DMA 버퍼. */
			iov->iov_len = raid_io->num_blocks * raid_bdev->bdev.blocklen;  /* [한국어] 읽을 바이트 수. */
			chunk->iovcnt = 1;

			if (raid_io_md) {
				chunk->md_buf = stripe_req->reconstruct.chunk_md_buffers[buf_idx]; /* [한국어] md 읽기 버퍼. */
			}

			buf_idx++;  /* [한국어] 다음 정상 청크용 버퍼로 진행. */
		}
	}

	raid_io->module_private = stripe_req;                        /* [한국어] 콜백들이 stripe_req 복원하도록 저장. */
	raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs;  /* [한국어] 완료 집계 목표(복원 대상 포함해 N+1, 대상은 즉시 성공 집계). */
	raid_io->completion_cb = raid5f_reconstruct_reads_completed_cb; /* [한국어] 모든 읽기 완료 시 XOR 로 이어줄 코어 콜백. */

	TAILQ_REMOVE(&r5ch->free_stripe_requests.reconstruct, stripe_req, link);  /* [한국어] 성공 확정 — 풀에서 제거. */

	raid5f_stripe_request_submit_chunks(stripe_req);  /* [한국어] 정상 청크들 readv 발행 시작. */

	return 0;
}

/*
 * [한국어]
 * raid5f_submit_read_request - READ 처리: 정상 디스크면 직접 readv, 죽었으면 reconstruct.
 *
 * @raid_io: 사용자 READ I/O.
 * @stripe_index: 대상 stripe.
 * @stripe_offset: stripe 내부 오프셋(블록) — 어느 데이터 청크/내부 위치인지 결정.
 * @return: 0 또는 음수 errno.
 *
 * READ 는 항상 한 strip 이내(submit_rw_request 의 assert)이므로 단 하나의 데이터 청크에서
 * 온다. stripe_offset 를 strip 단위로 나눠 데이터 청크 인덱스(chunk_data_idx)를 구하고,
 * rotating parity 위치(p_idx)를 건너뛰어 실제 베이스 인덱스(chunk_idx)로 보정한다.
 * 해당 베이스 채널이 살아있으면 그 디스크에서 직접 readv(빠른 경로), 죽었으면(NULL)
 * reconstruct read 로 위임한다. -ENOMEM 이면 io_wait 으로 전체 재시도.
 * 실행 컨텍스트: 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_submit_rw_request → [이 함수] → raid_bdev_readv_blocks_ext / raid5f_submit_reconstruct_read
 */
static int
raid5f_submit_read_request(struct raid_bdev_io *raid_io, uint64_t stripe_index,
			   uint64_t stripe_offset)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;  /* [한국어] RAID 기하 정보. */
	uint8_t chunk_data_idx = stripe_offset >> raid_bdev->strip_size_shift;  /* [한국어] 데이터 청크 번호(패리티 무시한 순서). */
	uint8_t p_idx = raid5f_stripe_parity_chunk_index(raid_bdev, stripe_index);  /* [한국어] 이번 stripe 의 패리티 청크 인덱스. */
	uint8_t chunk_idx = chunk_data_idx < p_idx ? chunk_data_idx : chunk_data_idx + 1;  /* [한국어] 패리티 위치를 건너뛴 실제 베이스 인덱스. */
	struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[chunk_idx];  /* [한국어] 대상 베이스 bdev. */
	struct spdk_io_channel *base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch, chunk_idx); /* [한국어] 채널 — NULL 이면 죽음. */
	uint64_t chunk_offset = stripe_offset - (chunk_data_idx << raid_bdev->strip_size_shift);  /* [한국어] strip 내부 오프셋(블록). */
	uint64_t base_offset_blocks = (stripe_index << raid_bdev->strip_size_shift) + chunk_offset; /* [한국어] 베이스 내 절대 블록 오프셋. */
	struct spdk_bdev_ext_io_opts io_opts;
	int ret;

	raid5f_init_ext_io_opts(&io_opts, raid_io);  /* [한국어] memory domain/md 옵션 채움. */
	if (base_ch == NULL) {
		/* [한국어] 대상 디스크가 죽음 — 나머지 N개로 reconstruct 복원. */
		return raid5f_submit_reconstruct_read(raid_io, stripe_index, chunk_idx, chunk_offset,
						      raid5f_stripe_request_reconstruct_xor_done);
	}

	/* [한국어] 정상 경로 — 해당 디스크에서 직접 readv(가장 빠른 경로, XOR 불필요). */
	ret = raid_bdev_readv_blocks_ext(base_info, base_ch, raid_io->iovs, raid_io->iovcnt,
					 base_offset_blocks, raid_io->num_blocks,
					 raid5f_chunk_read_complete, raid_io, &io_opts);
	if (spdk_unlikely(ret == -ENOMEM)) {
		/* [한국어] 베이스 자원 고갈 — io_wait 으로 전체 READ 재시도 등록. */
		raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _raid5f_submit_rw_request);
		return 0;  /* [한국어] 재시도 예약했으므로 호출자에 에러 전파 안 함. */
	}

	return ret;
}

/*
 * [한국어]
 * raid5f_submit_rw_request - RAID5F 모듈의 R/W 메인 진입점(vtable .submit_rw_request).
 *
 * @raid_io: bdev_raid 코어가 split·매핑을 마쳐 넘긴 사용자 I/O.
 * @return: 없음 (에러는 raid_bdev_io_complete 로 보고).
 *
 * bdev_raid 코어가 레벨 디스패처에서 본 함수를 호출한다. offset_blocks 를 stripe_blocks
 * 로 나눠 stripe_index/stripe_offset 를 구하고 READ/WRITE 로 분기한다. WRITE 는 코어가
 * write_unit_size=stripe_blocks 로 split 해 주므로 항상 stripe 경계·전체 크기임을 assert
 * 로 보증(full-stripe write 전제). 하위 함수가 -ENOMEM 을 반환하면 NOMEM 상태로 완료해
 * 코어가 나중에 재시도하도록 한다.
 * 실행 컨텍스트: 채널 소유 스레드(이 raid_io 가 바인딩된 reactor).
 *
 * 호출 체인:
 *   bdev_raid 코어(레벨 디스패처) / _raid5f_submit_rw_request(재시도) → [이 함수] →
 *   raid5f_submit_read_request / raid5f_submit_write_request
 */
static void
raid5f_submit_rw_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;             /* [한국어] RAID 기하 정보. */
	struct raid5f_info *r5f_info = raid_bdev->module_private;     /* [한국어] stripe_blocks 등. */
	uint64_t stripe_index = raid_io->offset_blocks / r5f_info->stripe_blocks;  /* [한국어] 몇 번째 stripe 인지. */
	uint64_t stripe_offset = raid_io->offset_blocks % r5f_info->stripe_blocks; /* [한국어] stripe 내부 오프셋. */
	int ret;

	switch (raid_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		assert(raid_io->num_blocks <= raid_bdev->strip_size);  /* [한국어] READ 는 한 strip 이내(코어 split 보증). */
		ret = raid5f_submit_read_request(raid_io, stripe_index, stripe_offset);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		assert(stripe_offset == 0);                                   /* [한국어] WRITE 는 stripe 경계에서 시작. */
		assert(raid_io->num_blocks == r5f_info->stripe_blocks);       /* [한국어] WRITE 는 정확히 full-stripe 크기. */
		ret = raid5f_submit_write_request(raid_io, stripe_index);
		break;
	default:
		ret = -EINVAL;  /* [한국어] RAID5F 는 R/W 외 타입 미지원. */
		break;
	}

	if (spdk_unlikely(ret)) {
		/* [한국어] -ENOMEM 은 NOMEM(코어 재시도), 그 외는 FAILED 로 완료. */
		raid_bdev_io_complete(raid_io, ret == -ENOMEM ? SPDK_BDEV_IO_STATUS_NOMEM :
				      SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * [한국어]
 * raid5f_stripe_request_free - stripe 요청과 그 부속 버퍼/배열을 모두 해제한다.
 *
 * @stripe_req: 해제할 stripe 요청 (alloc 의 부분 실패 경로에서도 호출되므로 NULL 필드 안전).
 * @return: 없음.
 *
 * raid5f_stripe_request_alloc 의 역연산. 각 청크 iov 배열을 free 하고, 타입별로 WRITE 의
 * parity 버퍼들 또는 RECONSTRUCT 의 chunk 읽기 버퍼 배열들을 (spdk_dma_free 로) 해제한다.
 * DMA 버퍼는 hugepage 기반이라 일반 free 가 아닌 spdk_dma_free 를 써야 한다. 마지막으로
 * XOR 스크래치 배열과 본체를 free. 채널 파괴 시점 또는 alloc 실패 롤백(goto err)에서 호출.
 * 실행 컨텍스트: 채널 파괴/생성 — 채널 소유 스레드(hot-path 아님).
 *
 * 호출 체인:
 *   raid5f_ioch_destroy / raid5f_stripe_request_alloc(err) → [이 함수] → spdk_dma_free / free
 */
static void
raid5f_stripe_request_free(struct stripe_request *stripe_req)
{
	struct chunk *chunk;

	/* [한국어] 모든 청크의 iov 배열 해제(일반 realloc/calloc 으로 만든 것이라 free). */
	FOR_EACH_CHUNK(stripe_req, chunk) {
		free(chunk->iovs);
	}

	if (stripe_req->type == STRIPE_REQ_WRITE) {
		/* [한국어] WRITE 의 패리티 데이터/메타데이터 DMA 버퍼 해제(hugepage → spdk_dma_free). */
		spdk_dma_free(stripe_req->write.parity_buf);
		spdk_dma_free(stripe_req->write.parity_md_buf);
	} else if (stripe_req->type == STRIPE_REQ_RECONSTRUCT) {
		struct raid5f_info *r5f_info = raid5f_ch_to_r5f_info(stripe_req->r5ch);  /* [한국어] N 조회용. */
		struct raid_bdev *raid_bdev = r5f_info->raid_bdev;
		uint8_t i;

		/* [한국어] reconstruct 의 청크 데이터 읽기 버퍼 N개 + 포인터 배열 해제. */
		if (stripe_req->reconstruct.chunk_buffers) {
			for (i = 0; i < raid5f_stripe_data_chunks_num(raid_bdev); i++) {
				spdk_dma_free(stripe_req->reconstruct.chunk_buffers[i]);
			}
			free(stripe_req->reconstruct.chunk_buffers);
		}

		/* [한국어] reconstruct 의 청크 md 읽기 버퍼 N개 + 포인터 배열 해제(md 있을 때만 alloc 됨). */
		if (stripe_req->reconstruct.chunk_md_buffers) {
			for (i = 0; i < raid5f_stripe_data_chunks_num(raid_bdev); i++) {
				spdk_dma_free(stripe_req->reconstruct.chunk_md_buffers[i]);
			}
			free(stripe_req->reconstruct.chunk_md_buffers);
		}
	} else {
		assert(false);  /* [한국어] 알 수 없는 타입 — 논리 오류. */
	}

	free(stripe_req->chunk_xor_buffers);     /* [한국어] XOR src/dst 포인터 배열. */
	free(stripe_req->chunk_xor_md_buffers);  /* [한국어] md XOR src 포인터 배열. */
	free(stripe_req->chunk_iov_iters);       /* [한국어] ioviter 상태 버퍼. */

	free(stripe_req);  /* [한국어] 본체(+flexible array chunks[]) 해제 — 단일 calloc 이었음. */
}

/*
 * [한국어]
 * raid5f_stripe_request_alloc - stripe 요청 1개와 그 부속 버퍼/배열을 전부 할당한다.
 *
 * @r5ch: 소속 채널 (raid_bdev 기하 정보 조회 + 소유권 연결).
 * @type: STRIPE_REQ_WRITE 또는 STRIPE_REQ_RECONSTRUCT — 버퍼 구성이 달라짐.
 * @return: 초기화된 stripe_request 또는 NULL(할당 실패 — 부분 할당분은 내부에서 정리).
 *
 * 채널 생성 시 RAID5F_MAX_STRIPES 개를 미리 만들어 free 풀에 채우는 풀링 전략의 생성자.
 * 본체+chunks[] 를 단일 calloc 으로 잡고(cache locality), 각 청크에 초기 iov 4개를 할당.
 * WRITE 면 패리티 데이터/md DMA 버퍼를, RECONSTRUCT 면 N개 청크 읽기 데이터/md DMA 버퍼를
 * spdk_dma_malloc(hugepage, buf_alignment 정렬)로 확보. 마지막으로 XOR 에 쓸 ioviter/포인터
 * 배열들을 할당. 어느 단계든 실패하면 goto err → raid5f_stripe_request_free 로 일괄 정리.
 * 실행 컨텍스트: 채널 생성 — 채널 소유 스레드(hot-path 아님).
 *
 * 호출 체인:
 *   raid5f_ioch_create → [이 함수] → spdk_dma_malloc / calloc / malloc
 */
static struct stripe_request *
raid5f_stripe_request_alloc(struct raid5f_io_channel *r5ch, enum stripe_request_type type)
{
	struct raid5f_info *r5f_info = raid5f_ch_to_r5f_info(r5ch);  /* [한국어] 기하/정렬 정보. */
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;
	uint32_t raid_io_md_size = raid_bdev->bdev.md_interleave ? 0 : raid_bdev->bdev.md_len; /* [한국어] 분리 md 크기(interleave 면 0=별도 버퍼 불필요). */
	struct stripe_request *stripe_req;
	struct chunk *chunk;
	size_t chunk_len;

	/* [한국어] 본체 + chunks[num_base_bdevs] 를 한 번에 calloc — 0 초기화 + cache locality. */
	stripe_req = calloc(1, sizeof(*stripe_req) + sizeof(*chunk) * raid_bdev->num_base_bdevs);
	if (!stripe_req) {
		return NULL;
	}

	stripe_req->r5ch = r5ch;   /* [한국어] 소속 채널 연결. */
	stripe_req->type = type;   /* [한국어] 타입 고정(free 풀 분리 기준). */

	/* [한국어] 각 청크 초기화: index 부여 + iov 배열 초기 4개 할당. */
	FOR_EACH_CHUNK(stripe_req, chunk) {
		chunk->index = chunk - stripe_req->chunks;  /* [한국어] chunks[] 내 자기 인덱스(=베이스 bdev 인덱스). */
		chunk->iovcnt_max = 4;                       /* [한국어] 초기 capacity 4(필요 시 realloc). */
		chunk->iovs = calloc(chunk->iovcnt_max, sizeof(chunk->iovs[0]));
		if (!chunk->iovs) {
			goto err;  /* [한국어] 실패 — 지금까지 할당분 free 로 정리. */
		}
	}

	chunk_len = raid_bdev->strip_size * raid_bdev->bdev.blocklen;  /* [한국어] 한 청크(=1 strip) 바이트 크기. */

	if (type == STRIPE_REQ_WRITE) {
		/* [한국어] 패리티 데이터 버퍼 — XOR 결과가 여기 채워져 베이스 bdev 로 전송. */
		stripe_req->write.parity_buf = spdk_dma_malloc(chunk_len, r5f_info->buf_alignment, NULL);
		if (!stripe_req->write.parity_buf) {
			goto err;
		}

		/* [한국어] 분리 md 가 있으면 패리티 md 버퍼도 확보. */
		if (raid_io_md_size != 0) {
			stripe_req->write.parity_md_buf = spdk_dma_malloc(raid_bdev->strip_size * raid_io_md_size,
							  r5f_info->buf_alignment, NULL);
			if (!stripe_req->write.parity_md_buf) {
				goto err;
			}
		}
	} else if (type == STRIPE_REQ_RECONSTRUCT) {
		uint8_t n = raid5f_stripe_data_chunks_num(raid_bdev);  /* [한국어] 읽어둘 정상 청크 수 N. */
		void *buf;
		uint8_t i;

		/* [한국어] N개 청크 읽기 데이터 버퍼 포인터 배열. */
		stripe_req->reconstruct.chunk_buffers = calloc(n, sizeof(void *));
		if (!stripe_req->reconstruct.chunk_buffers) {
			goto err;
		}

		/* [한국어] 각 정상 청크용 DMA 읽기 버퍼(1 strip 크기) 확보. */
		for (i = 0; i < n; i++) {
			buf = spdk_dma_malloc(chunk_len, r5f_info->buf_alignment, NULL);
			if (!buf) {
				goto err;
			}
			stripe_req->reconstruct.chunk_buffers[i] = buf;
		}

		/* [한국어] 분리 md 가 있으면 md 읽기 버퍼들도 동일하게 확보. */
		if (raid_io_md_size != 0) {
			stripe_req->reconstruct.chunk_md_buffers = calloc(n, sizeof(void *));
			if (!stripe_req->reconstruct.chunk_md_buffers) {
				goto err;
			}

			for (i = 0; i < n; i++) {
				buf = spdk_dma_malloc(raid_bdev->strip_size * raid_io_md_size, r5f_info->buf_alignment, NULL);
				if (!buf) {
					goto err;
				}
				stripe_req->reconstruct.chunk_md_buffers[i] = buf;
			}
		}
	} else {
		assert(false);   /* [한국어] 알 수 없는 타입. */
		return NULL;
	}

	/* [한국어] ioviter 상태 버퍼 — num_base_bdevs 개 iov 묶음을 동시 walk 하기 위함. */
	stripe_req->chunk_iov_iters = malloc(SPDK_IOVITER_SIZE(raid_bdev->num_base_bdevs));
	if (!stripe_req->chunk_iov_iters) {
		goto err;
	}

	/* [한국어] XOR src/dst 포인터 배열(N data + 1 dst slot = num_base_bdevs). */
	stripe_req->chunk_xor_buffers = calloc(raid_bdev->num_base_bdevs,
					       sizeof(stripe_req->chunk_xor_buffers[0]));
	if (!stripe_req->chunk_xor_buffers) {
		goto err;
	}

	/* [한국어] md XOR src 포인터 배열(N개 — dst 는 dest_chunk->md_buf 직접 사용). */
	stripe_req->chunk_xor_md_buffers = calloc(raid5f_stripe_data_chunks_num(raid_bdev),
					   sizeof(stripe_req->chunk_xor_md_buffers[0]));
	if (!stripe_req->chunk_xor_md_buffers) {
		goto err;
	}

	return stripe_req;
err:
	/* [한국어] 부분 할당분 일괄 정리 후 NULL 반환(free 가 NULL 필드 안전 처리). */
	raid5f_stripe_request_free(stripe_req);
	return NULL;
}

/*
 * [한국어]
 * raid5f_ioch_destroy - per-channel 자원 해제 콜백 (spdk_io_device_register 의 destroy_cb).
 *
 * @io_device: r5f_info (io_device 핸들). 여기선 거의 안 씀.
 * @ctx_buf: 파괴할 raid5f_io_channel.
 * @return: 없음.
 *
 * 채널이 사라질 때(마지막 참조 put) 코어가 호출한다. retry 큐는 비어 있어야 하며(진행 중인
 * XOR 가 없어야 함 — assert), 두 free 풀의 stripe_request 들을 모두 해제하고, accel 채널을
 * 반납(put), XOR 스크래치 배열을 free 한다. ioch_create 의 실패 롤백 경로에서도 호출되므로
 * 부분 초기화 상태(NULL 필드)에도 안전하도록 작성됐다.
 * 실행 컨텍스트: 채널 파괴 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   spdk_put_io_channel(마지막) / raid5f_ioch_create(err) → [이 함수] → raid5f_stripe_request_free
 */
static void
raid5f_ioch_destroy(void *io_device, void *ctx_buf)
{
	struct raid5f_io_channel *r5ch = ctx_buf;  /* [한국어] 파괴 대상 채널. */
	struct stripe_request *stripe_req;

	assert(TAILQ_EMPTY(&r5ch->xor_retry_queue));  /* [한국어] 진행 중 XOR 가 남아 있으면 안 됨(데이터 유실 위험). */

	/* [한국어] write free 풀 비우기 — 각 요청과 부속 버퍼 해제. */
	while ((stripe_req = TAILQ_FIRST(&r5ch->free_stripe_requests.write))) {
		TAILQ_REMOVE(&r5ch->free_stripe_requests.write, stripe_req, link);
		raid5f_stripe_request_free(stripe_req);
	}

	/* [한국어] reconstruct free 풀 비우기. */
	while ((stripe_req = TAILQ_FIRST(&r5ch->free_stripe_requests.reconstruct))) {
		TAILQ_REMOVE(&r5ch->free_stripe_requests.reconstruct, stripe_req, link);
		raid5f_stripe_request_free(stripe_req);
	}

	/* [한국어] accel 채널 반납(획득됐을 때만 — 부분 초기화 안전). */
	if (r5ch->accel_ch) {
		spdk_put_io_channel(r5ch->accel_ch);
	}

	free(r5ch->chunk_xor_iovs);    /* [한국어] XOR iov 스크래치 배열. */
	free(r5ch->chunk_xor_iovcnt);  /* [한국어] XOR iovcnt 스크래치 배열. */
}

/*
 * [한국어]
 * raid5f_ioch_create - per-channel 자원 초기화 콜백 (spdk_io_device_register 의 create_cb).
 *
 * @io_device: r5f_info (io_device 핸들) — 기하 정보 출처.
 * @ctx_buf: 초기화할 raid5f_io_channel (코어가 sizeof 만큼 미리 잡아 줌).
 * @return: 0 또는 -ENOMEM.
 *
 * spdk_get_io_channel(r5f_info) 가 reactor 마다 처음 불릴 때 코어가 호출한다. 두 free 풀과
 * retry 큐를 초기화하고, write/reconstruct 각각 RAID5F_MAX_STRIPES(32)개의 stripe_request 를
 * 미리 할당해 풀에 채운다(hot-path malloc 회피). accel 채널을 획득하고 XOR 스크래치 배열을
 * 할당한다. 어느 단계든 실패하면 ioch_destroy 로 정리 후 -ENOMEM 반환.
 * 실행 컨텍스트: 채널 생성 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   spdk_get_io_channel(처음) → [이 함수] → raid5f_stripe_request_alloc / spdk_accel_get_io_channel
 */
static int
raid5f_ioch_create(void *io_device, void *ctx_buf)
{
	struct raid5f_io_channel *r5ch = ctx_buf;            /* [한국어] 초기화할 채널. */
	struct raid5f_info *r5f_info = io_device;            /* [한국어] io_device = r5f_info. */
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;   /* [한국어] num_base_bdevs 등. */
	struct stripe_request *stripe_req;
	int i;

	TAILQ_INIT(&r5ch->free_stripe_requests.write);        /* [한국어] write free 풀 초기화. */
	TAILQ_INIT(&r5ch->free_stripe_requests.reconstruct);  /* [한국어] reconstruct free 풀 초기화. */
	TAILQ_INIT(&r5ch->xor_retry_queue);                   /* [한국어] XOR 재시도 큐 초기화. */

	/* [한국어] write 용 stripe_request 32개 사전 할당. */
	for (i = 0; i < RAID5F_MAX_STRIPES; i++) {
		stripe_req = raid5f_stripe_request_alloc(r5ch, STRIPE_REQ_WRITE);
		if (!stripe_req) {
			goto err;
		}

		TAILQ_INSERT_HEAD(&r5ch->free_stripe_requests.write, stripe_req, link);
	}

	/* [한국어] reconstruct 용 stripe_request 32개 사전 할당. */
	for (i = 0; i < RAID5F_MAX_STRIPES; i++) {
		stripe_req = raid5f_stripe_request_alloc(r5ch, STRIPE_REQ_RECONSTRUCT);
		if (!stripe_req) {
			goto err;
		}

		TAILQ_INSERT_HEAD(&r5ch->free_stripe_requests.reconstruct, stripe_req, link);
	}

	/* [한국어] accel framework 채널 획득 — XOR 발행에 필수. */
	r5ch->accel_ch = spdk_accel_get_io_channel();
	if (!r5ch->accel_ch) {
		SPDK_ERRLOG("Failed to get accel framework's IO channel\n");
		goto err;
	}

	/* [한국어] XOR 시 청크별 iov 를 모을 스크래치 배열. */
	r5ch->chunk_xor_iovs = calloc(raid_bdev->num_base_bdevs, sizeof(*r5ch->chunk_xor_iovs));
	if (!r5ch->chunk_xor_iovs) {
		goto err;
	}

	/* [한국어] 위 배열에 대응하는 iovcnt 스크래치 배열. */
	r5ch->chunk_xor_iovcnt = calloc(raid_bdev->num_base_bdevs, sizeof(*r5ch->chunk_xor_iovcnt));
	if (!r5ch->chunk_xor_iovcnt) {
		goto err;
	}

	return 0;
err:
	/* [한국어] 부분 초기화분을 destroy 로 일괄 정리 후 실패 반환. */
	SPDK_ERRLOG("Failed to initialize io channel\n");
	raid5f_ioch_destroy(r5f_info, r5ch);
	return -ENOMEM;
}

/*
 * [한국어]
 * raid5f_start - RAID5F 인스턴스 시작 콜백 (vtable .start) — 기하 계산 + io_device 등록.
 *
 * @raid_bdev: 코어가 베이스 bdev 들을 모아 준비한 raid_bdev.
 * @return: 0 또는 -ENOMEM.
 *
 * RAID5F 라이프사이클의 시작점. raid5f_info 를 만들어 module_private 에 매단다. 모든 베이스
 * bdev 중 최소 blockcnt 를 구해 strip_size 배수로 내림(모든 디스크가 같은 stripe 수를 갖도록)
 * 하고, 각 베이스의 data_size 를 그 값으로 통일한다. stripe_blocks(=strip_size*N), total_stripes,
 * 버퍼 정렬, blocklen_shift 를 계산하고, 사용자에게 보일 bdev 의 blockcnt/optimal_io_boundary/
 * write_unit_size 와 split 플래그를 세팅한다 — 특히 split_on_write_unit + write_unit_size=
 * stripe_blocks 가 "WRITE 는 항상 full-stripe" 라는 RAID5F 의 전제를 코어 차원에서 강제한다.
 * 마지막으로 r5f_info 를 io_device 로 등록해 채널 생성 경로를 활성화.
 * 실행 컨텍스트: RAID 구성 시점 — 관리 스레드(app thread).
 *
 * 호출 체인:
 *   bdev_raid 코어(configure) → [이 함수] → spdk_io_device_register(create/destroy 콜백 연결)
 */
static int
raid5f_start(struct raid_bdev *raid_bdev)
{
	uint64_t min_blockcnt = UINT64_MAX;  /* [한국어] 베이스 중 최소 용량(블록) — 모든 디스크 공통 stripe 수 결정. */
	uint64_t base_bdev_data_size;
	struct raid_base_bdev_info *base_info;
	struct spdk_bdev *base_bdev;
	struct raid5f_info *r5f_info;
	size_t alignment = 0;  /* [한국어] 모든 베이스 중 가장 엄격한 버퍼 정렬. */

	r5f_info = calloc(1, sizeof(*r5f_info));  /* [한국어] per-raid 정보 객체 생성. */
	if (!r5f_info) {
		SPDK_ERRLOG("Failed to allocate r5f_info\n");
		return -ENOMEM;
	}
	r5f_info->raid_bdev = raid_bdev;  /* [한국어] 역참조 연결. */

	/* [한국어] 모든 베이스 bdev 순회: 최소 용량 + 최대 정렬 요구 수집. */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		min_blockcnt = spdk_min(min_blockcnt, base_info->data_size);
		if (base_info->desc) {  /* [한국어] 살아있는 베이스만 정렬 조회(죽은 디스크는 desc=NULL). */
			base_bdev = spdk_bdev_desc_get_bdev(base_info->desc);
			alignment = spdk_max(alignment, spdk_bdev_get_buf_align(base_bdev));
		}
	}

	base_bdev_data_size = (min_blockcnt / raid_bdev->strip_size) * raid_bdev->strip_size;  /* [한국어] strip 배수로 내림(남는 블록 버림). */

	/* [한국어] 모든 베이스의 유효 data_size 를 통일(정합성). */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->data_size = base_bdev_data_size;
	}

	r5f_info->total_stripes = min_blockcnt / raid_bdev->strip_size;  /* [한국어] stripe 총수. */
	r5f_info->stripe_blocks = raid_bdev->strip_size * raid5f_stripe_data_chunks_num(raid_bdev); /* [한국어] stripe 당 데이터 블록 = strip*N. */
	r5f_info->buf_alignment = alignment;  /* [한국어] DMA 버퍼 정렬 저장. */
	if (!raid_bdev->bdev.md_interleave) {
		r5f_info->blocklen_shift = spdk_u32log2(raid_bdev->bdev.blocklen);  /* [한국어] md 오프셋 시프트 최적화 상수. */
	}

	raid_bdev->bdev.blockcnt = r5f_info->stripe_blocks * r5f_info->total_stripes;  /* [한국어] 사용자에게 보일 총 용량. */
	raid_bdev->bdev.optimal_io_boundary = raid_bdev->strip_size;          /* [한국어] strip 경계가 최적 I/O 경계. */
	raid_bdev->bdev.split_on_optimal_io_boundary = true;                  /* [한국어] 코어가 strip 경계로 split(READ 한 strip 보장). */
	raid_bdev->bdev.write_unit_size = r5f_info->stripe_blocks;            /* [한국어] WRITE 단위 = full stripe. */
	raid_bdev->bdev.split_on_write_unit = true;                          /* [한국어] WRITE 를 stripe 단위로 split → full-stripe write 강제(RMW/write-hole 회피). */

	raid_bdev->module_private = r5f_info;  /* [한국어] 이후 모든 콜백이 r5f_info 접근하도록 매닮. */

	/* [한국어] r5f_info 를 io_device 로 등록 — 채널당 raid5f_io_channel 생성/파괴 콜백 연결. */
	spdk_io_device_register(r5f_info, raid5f_ioch_create, raid5f_ioch_destroy,
				sizeof(struct raid5f_io_channel), NULL);

	return 0;
}

/*
 * [한국어]
 * raid5f_io_device_unregister_done - io_device 등록 해제 완료 콜백.
 *
 * @io_device: r5f_info.
 * @return: 없음.
 *
 * spdk_io_device_unregister 는 모든 채널이 정리될 때까지 비동기로 기다린 뒤 이 콜백을
 * 호출한다. 여기서 코어에 RAID 모듈 정지 완료를 통지(raid_bdev_module_stop_done)하고
 * r5f_info 를 해제한다. start 의 calloc 과 짝.
 * 실행 컨텍스트: io_device 정리 완료 시점 — 관리 스레드.
 *
 * 호출 체인:
 *   spdk_io_device_unregister(완료) → [이 함수] → raid_bdev_module_stop_done / free
 */
static void
raid5f_io_device_unregister_done(void *io_device)
{
	struct raid5f_info *r5f_info = io_device;  /* [한국어] 해제 대상. */

	raid_bdev_module_stop_done(r5f_info->raid_bdev);  /* [한국어] 코어에 정지 완료 통지. */

	free(r5f_info);  /* [한국어] per-raid 정보 해제. */
}

/*
 * [한국어]
 * raid5f_stop - RAID5F 인스턴스 정지 콜백 (vtable .stop).
 *
 * @raid_bdev: 정지할 raid_bdev.
 * @return: false — 정지가 비동기로 진행 중이라는 의미(코어는 완료 콜백을 기다림).
 *
 * io_device 등록을 해제 요청한다. 실제 정리는 모든 채널이 닫힌 뒤 비동기로 일어나며,
 * 그 완료는 raid5f_io_device_unregister_done 에서 처리된다. 따라서 false 를 반환해
 * "아직 정지 안 끝남, 완료 콜백 기다려라" 를 코어에 알린다.
 * 실행 컨텍스트: RAID 해체 시점 — 관리 스레드.
 *
 * 호출 체인:
 *   bdev_raid 코어 → [이 함수] → spdk_io_device_unregister
 */
static bool
raid5f_stop(struct raid_bdev *raid_bdev)
{
	struct raid5f_info *r5f_info = raid_bdev->module_private;  /* [한국어] 등록된 io_device. */

	spdk_io_device_unregister(r5f_info, raid5f_io_device_unregister_done);  /* [한국어] 비동기 해제 시작. */

	return false;  /* [한국어] 비동기 — 완료는 unregister_done 에서 통지. */
}

/*
 * [한국어]
 * raid5f_get_io_channel - RAID5F 의 I/O 채널을 얻는 콜백 (vtable .get_io_channel).
 *
 * @raid_bdev: 대상 raid_bdev.
 * @return: 이 RAID5F 인스턴스의 spdk_io_channel (현재 reactor 용).
 *
 * 사용자가 bdev 를 open 하고 채널을 요청할 때 코어가 호출한다. r5f_info(io_device) 에 대해
 * spdk_get_io_channel 을 호출하면, 현재 reactor 에 채널이 없으면 raid5f_ioch_create 가
 * 불려 새로 만들고 있으면 refcnt 만 증가시킨다.
 * 실행 컨텍스트: 채널 요청 스레드(요청한 reactor).
 *
 * 호출 체인:
 *   bdev_raid 코어 → [이 함수] → spdk_get_io_channel → (필요 시) raid5f_ioch_create
 */
static struct spdk_io_channel *
raid5f_get_io_channel(struct raid_bdev *raid_bdev)
{
	struct raid5f_info *r5f_info = raid_bdev->module_private;  /* [한국어] io_device 핸들. */

	return spdk_get_io_channel(r5f_info);  /* [한국어] 현재 reactor 의 채널 획득(없으면 생성). */
}

/*
 * [한국어]
 * raid5f_process_write_completed - rebuild 시 복원 데이터를 교체 디스크에 쓴 뒤의 완료 콜백.
 *
 * @bdev_io: 완료된 베이스 write I/O (free 필요).
 * @success: 성공 여부.
 * @cb_arg: raid_bdev_process_request.
 * @return: 없음.
 *
 * raid_bdev 의 background process(디스크 교체 후 데이터 동기화/rebuild) 경로의 한 단계.
 * 복원한 strip 을 교체된 타깃 디스크에 기록한 결과를 코어에 보고한다(process_request_complete).
 * 실행 컨텍스트: 베이스 bdev 완료 콜백 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   lib/bdev 완료 → [이 함수] → raid_bdev_process_request_complete
 */
static void
raid5f_process_write_completed(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_process_request *process_req = cb_arg;  /* [한국어] background process 요청. */

	spdk_bdev_free_io(bdev_io);  /* [한국어] 베이스 bdev_io 반납. */

	raid_bdev_process_request_complete(process_req, success ? 0 : -EIO);  /* [한국어] 결과를 코어에 보고. */
}

/* [한국어] forward 선언 — io_wait 재시도 래퍼가 호출하나 정의는 아래. */
static void raid5f_process_submit_write(struct raid_bdev_process_request *process_req);

/*
 * [한국어]
 * _raid5f_process_submit_write - process_submit_write 의 void* 시그니처 어댑터(io_wait 재시도).
 *
 * @ctx: raid_bdev_process_request (void* → 캐스팅).
 * @return: 없음.
 *
 * 교체 디스크 write 가 -ENOMEM 으로 막혔을 때 io_wait 콜백으로 등록되는 래퍼.
 * 실행 컨텍스트: io_wait 처리 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid_bdev_queue_io_wait → [이 함수] → raid5f_process_submit_write
 */
static void
_raid5f_process_submit_write(void *ctx)
{
	struct raid_bdev_process_request *process_req = ctx;  /* [한국어] process 요청 복원. */

	raid5f_process_submit_write(process_req);  /* [한국어] 재실행. */
}

/*
 * [한국어]
 * raid5f_process_submit_write - rebuild 로 복원한 strip 을 교체 타깃 디스크에 기록한다.
 *
 * @process_req: 복원 데이터(raid_io->iovs)와 타깃(교체된 디스크) 정보를 담은 요청.
 * @return: 없음 (결과는 완료 콜백/코어로 보고).
 *
 * reconstruct read 로 복원한 한 strip 분량의 데이터를, 새로 들어온(교체된) 베이스 디스크의
 * 해당 stripe 위치에 writev 한다. -ENOMEM 이면 io_wait 으로 재시도, 그 외 에러면 process
 * 요청을 에러로 완료. 이로써 죽었다 교체된 디스크의 데이터가 패리티/잉여로부터 재건된다.
 * 실행 컨텍스트: 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_process_stripe_request_reconstruct_xor_done → [이 함수] → raid_bdev_writev_blocks_ext
 */
static void
raid5f_process_submit_write(struct raid_bdev_process_request *process_req)
{
	struct raid_bdev_io *raid_io = &process_req->raid_io;          /* [한국어] process 내장 raid I/O. */
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;             /* [한국어] RAID 기하 정보. */
	struct raid5f_info *r5f_info = raid_bdev->module_private;     /* [한국어] stripe_blocks. */
	uint64_t stripe_index = process_req->offset_blocks / r5f_info->stripe_blocks;  /* [한국어] 대상 stripe. */
	struct spdk_bdev_ext_io_opts io_opts;
	int ret;

	raid5f_init_ext_io_opts(&io_opts, raid_io);  /* [한국어] memory domain/md 옵션. */
	/* [한국어] 복원 데이터를 타깃 디스크의 stripe 위치(stripe_index*strip_size)에 strip 전체만큼 기록. */
	ret = raid_bdev_writev_blocks_ext(process_req->target, process_req->target_ch,
					  raid_io->iovs, raid_io->iovcnt,
					  stripe_index << raid_bdev->strip_size_shift, raid_bdev->strip_size,
					  raid5f_process_write_completed, process_req, &io_opts);
	if (spdk_unlikely(ret != 0)) {
		if (ret == -ENOMEM) {
			/* [한국어] 타깃 자원 고갈 — io_wait 으로 write 재시도. */
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(process_req->target->desc),
						process_req->target_ch, _raid5f_process_submit_write);
		} else {
			raid_bdev_process_request_complete(process_req, ret);  /* [한국어] 복구 불가 — 에러 보고. */
		}
	}
}

/*
 * [한국어]
 * raid5f_process_stripe_request_reconstruct_xor_done - rebuild 의 복원 XOR 완료 후속 콜백.
 *
 * @stripe_req: 복원 XOR 가 끝난 reconstruct stripe 요청.
 * @status: XOR 결과.
 * @return: 없음.
 *
 * 일반 READ 의 reconstruct_xor_done 과 달리, 여기서는 복원 결과를 사용자에게 반환하지 않고
 * 교체 디스크에 다시 기록(process_submit_write)하는 점이 다르다. raid_io 가 process_request
 * 안에 내장돼 있어 SPDK_CONTAINEROF 로 바깥 process_req 를 복원한다.
 * 실행 컨텍스트: XOR 완료 콜백 — 채널 소유 스레드.
 *
 * 호출 체인:
 *   raid5f_xor_stripe(xor.cb) → [이 함수] → raid5f_process_submit_write
 */
static void
raid5f_process_stripe_request_reconstruct_xor_done(struct stripe_request *stripe_req, int status)
{
	struct raid_bdev_io *raid_io = stripe_req->raid_io;  /* [한국어] 내장 raid I/O. */
	struct raid_bdev_process_request *process_req = SPDK_CONTAINEROF(raid_io,
			struct raid_bdev_process_request, raid_io);  /* [한국어] 바깥 process 요청 복원. */

	raid5f_stripe_request_release(stripe_req);  /* [한국어] 복원 끝 — stripe 요청 반환. */

	if (status != 0) {
		raid_bdev_process_request_complete(process_req, status);  /* [한국어] 복원 실패 — 에러 보고. */
		return;
	}

	raid5f_process_submit_write(process_req);  /* [한국어] 복원 성공 — 교체 디스크에 기록. */
}

/*
 * [한국어]
 * raid5f_submit_process_request - 디스크 교체 후 rebuild(데이터 동기화) 한 stripe 분 처리 (vtable .submit_process_request).
 *
 * @process_req: 코어 background process 가 넘긴 요청(offset/num_blocks/target).
 * @raid_ch: 처리할 RAID 채널.
 * @return: 처리한 블록 수(>0) 또는 음수 errno. 진행 불가 시 0.
 *
 * 죽었다 교체된 디스크의 데이터를 다른 디스크들로부터 재건하는 경로. 교체 타깃의 청크
 * 인덱스(chunk_idx)를 구하고, 그 청크를 "복원 대상" 으로 삼아 reconstruct read 를 발행한다
 * (raid5f_process_stripe_request_reconstruct_xor_done 콜백으로 → XOR → 타깃에 write).
 * full stripe 단위로만 처리하므로, 남은 블록이 한 stripe 미만이면 0(이번엔 처리 안 함) 반환.
 * 반환한 stripe_blocks 만큼 코어가 다음 오프셋으로 진행한다.
 * 실행 컨텍스트: background process — 채널 소유 스레드.
 *
 * 호출 체인:
 *   bdev_raid 코어(process) → [이 함수] → raid5f_submit_reconstruct_read
 */
static int
raid5f_submit_process_request(struct raid_bdev_process_request *process_req,
			      struct raid_bdev_io_channel *raid_ch)
{
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(raid_ch);       /* [한국어] 채널 핸들 복원. */
	struct raid_bdev *raid_bdev = spdk_io_channel_get_io_device(ch);      /* [한국어] 이 채널의 raid_bdev. */
	struct raid5f_info *r5f_info = raid_bdev->module_private;             /* [한국어] stripe_blocks. */
	struct raid_bdev_io *raid_io = &process_req->raid_io;                /* [한국어] 합성할 내장 raid I/O. */
	uint8_t chunk_idx = raid_bdev_base_bdev_slot(process_req->target);    /* [한국어] 교체 타깃의 청크/슬롯 인덱스 = 복원 대상. */
	uint64_t stripe_index = process_req->offset_blocks / r5f_info->stripe_blocks;  /* [한국어] 대상 stripe. */
	struct iovec *iov;
	int ret;

	assert((process_req->offset_blocks % r5f_info->stripe_blocks) == 0);  /* [한국어] stripe 경계에서만 처리. */

	if (process_req->num_blocks < r5f_info->stripe_blocks) {
		return 0;  /* [한국어] 한 stripe 분이 안 되면 이번엔 건너뜀(다음 라운드에서). */
	}

	iov = &process_req->iov;                                            /* [한국어] 복원 데이터를 담을 단일 iov. */
	iov->iov_len = raid_bdev->strip_size * raid_bdev->bdev.blocklen;    /* [한국어] strip 한 칸 크기. */
	/* [한국어] 내장 raid_io 를 READ 타입으로 합성 — reconstruct read 가 이 iov 에 복원 데이터를 채움. */
	raid_bdev_io_init(raid_io, raid_ch, SPDK_BDEV_IO_TYPE_READ,
			  process_req->offset_blocks, raid_bdev->strip_size,
			  iov, 1, process_req->md_buf, NULL, NULL);

	/* [한국어] 교체 타깃 청크를 복원 대상으로 reconstruct read 발행. 완료 시 위 XOR done 콜백. */
	ret = raid5f_submit_reconstruct_read(raid_io, stripe_index, chunk_idx, 0,
					     raid5f_process_stripe_request_reconstruct_xor_done);
	if (spdk_likely(ret == 0)) {
		return r5f_info->stripe_blocks;  /* [한국어] 한 stripe 진행했음 — 코어가 다음 오프셋으로. */
	} else if (ret < 0) {
		return ret;  /* [한국어] 에러(-ENOMEM 등) 전파. */
	} else {
		return -EINVAL;  /* [한국어] 양수 반환은 정의되지 않음 — 방어적으로 에러. */
	}
}

/* [한국어] RAID5F 레벨의 모듈 vtable — bdev_raid 코어가 level=RAID5F 인 raid_bdev 의
 * 라이프사이클/I/O 를 본 파일의 콜백들에 위임하도록 등록하는 디스패치 테이블.
 * 각 멤버는 코어가 적절한 시점에 호출하는 함수 포인터다. */
static struct raid_bdev_module g_raid5f_module = {
	.level = RAID5F,
	/* [한국어] 이 vtable 이 담당하는 RAID 레벨 식별자(코어의 레벨→모듈 lookup 키). */

	.base_bdevs_min = 3,
	/* [한국어] 최소 베이스 bdev 수 = 3 (데이터 2 + 패리티 1). RAID5 는 패리티 1개를 위해
	 * 최소 3개의 디스크가 필요. */

	.base_bdevs_constraint = {CONSTRAINT_MAX_BASE_BDEVS_REMOVED, 1},
	/* [한국어] 결함 허용 제약: 최대 1개 베이스 bdev 가 빠져도(removed) 동작 가능.
	 * 패리티 1개 → 단일 디스크 장애까지 reconstruct 로 견딘다. */

	.start = raid5f_start,
	/* [한국어] 인스턴스 시작 콜백 — 기하 계산 + io_device 등록. */

	.stop = raid5f_stop,
	/* [한국어] 인스턴스 정지 콜백 — io_device 비동기 해제(false 반환). */

	.submit_rw_request = raid5f_submit_rw_request,
	/* [한국어] R/W I/O 진입점 — stripe 매핑 + READ/WRITE 분기. */

	.get_io_channel = raid5f_get_io_channel,
	/* [한국어] per-channel 획득 콜백. */

	.submit_process_request = raid5f_submit_process_request,
	/* [한국어] background rebuild(디스크 교체 후 동기화) 한 stripe 처리 콜백. */
};
/* [한국어] constructor 시점에 g_raid5f_module 을 bdev_raid 코어의 모듈 리스트에 자동 등록.
 * main() 진입 전에 실행되어, RAID5F 타입 raid_bdev 생성 시 코어가 이 모듈을 찾을 수 있게 함. */
RAID_MODULE_REGISTER(&g_raid5f_module)

/* [한국어] "bdev_raid5f" 디버그 로그 컴포넌트 등록 — SPDK_DEBUGLOG(bdev_raid5f, ...) 활성화
 * 토글 및 로그 플래그 리스트 노출. */
SPDK_LOG_REGISTER_COMPONENT(bdev_raid5f)
