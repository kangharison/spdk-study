/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK Malloc bdev 모듈 구현 (bdev_malloc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 RAM(특히 DPDK가 관리하는 hugepage)을 백엔드로 하는 SPDK bdev 모듈을 구현한다.
 * 즉, 디스크 매체 없이 hugepage 위에 큰 블록 버퍼 한 덩어리를 잡아 두고, read/write를 그
 * 버퍼 영역에 대한 메모리 카피로 처리한다. 카피는 단순 memcpy가 아니라 SPDK의 accel framework
 * (DSA/DMA 엔진 또는 소프트웨어 fallback)을 통해 수행되어 zero-copy/오프로드 시나리오를
 * 지원한다. 주된 용도는 (1) 고성능 인메모리 디스크 — 매우 빠른 임시 저장소, (2) 상위
 * 스택(NVMe-oF target, vhost) 테스트의 일관성 있는 저지연 백엔드, (3) 데이터 무결성(DIF/DIX)
 * end-to-end 검증용. null bdev와 달리 실제로 write 데이터가 메모리에 보존되며, 다음 read에서
 * 그대로 회수된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [Application / NVMe-oF / vhost target / Test harness]
 *      ↓ spdk_bdev_read/write
 *   [bdev core (lib/bdev/)]
 *      ↓ fn_table->submit_request → bdev_malloc_submit_request
 *   [bdev_malloc (이 파일)]
 *      ↓ spdk_accel_submit_copy / spdk_accel_append_copy
 *   [SPDK Accel framework (lib/accel)]
 *      ↓ HW: Intel DSA / Intel IAA / IOAT / SW: memcpy
 *   [hugepage backed RAM (mdisk->malloc_buf, mdisk->malloc_md_buf)]
 *
 * 전체 흐름 (read 예):
 *   사용자 buffer ← spdk_accel copy ← mdisk->malloc_buf + offset (RAM)
 *
 * 전체 흐름 (write 예):
 *   mdisk->malloc_buf + offset (RAM) ← spdk_accel copy ← 사용자 buffer
 *
 * 코어 로컬 채널(malloc_channel)은 (a) accel framework의 io channel과 (b) "이번 tick에 완료될
 * 작업" 큐를 가진다. 즉시 완료 가능한 작업(RESET, FLUSH 등)은 큐에 넣어 다음 poller tick에
 * 일괄 spdk_bdev_io_complete를 호출하고, accel 작업은 accel 콜백이 직접 malloc_done을 호출.
 *
 * === 타 모듈과의 연결 ===
 * - lib/bdev/ : 모듈 등록, fn_table, spdk_bdev_io 라이프사이클.
 * - lib/accel/ : spdk_accel_submit_copy / spdk_accel_submit_fill / spdk_accel_append_copy /
 *   spdk_accel_sequence_finish — 메모리 카피와 zero-fill을 HW 엔진(DSA/IOAT)으로 오프로드.
 *   accel framework은 자체 io_channel을 제공하며 본 모듈은 그 채널을 그대로 빌려 쓴다.
 * - lib/util/ (DIF): spdk_dif_ctx_init, spdk_dif_generate, spdk_dif_verify, spdk_dix_generate/
 *   verify — interleaved(DIF)와 separated(DIX) 메타데이터 모두 처리. NVMe Base Spec §8.3.
 * - lib/env_dpdk/ (env): spdk_zmalloc(SPDK_MALLOC_DMA, NUMA 지정) — hugepage backed memory.
 *   2MiB 정렬로 할당해 DMA(예: DSA 엔진 또는 RDMA)와 직접 호환되도록 한다.
 * - lib/json/ : write_config_json 직렬화.
 * - lib/thread/ : spdk_io_device, spdk_get_io_channel, SPDK_POLLER_REGISTER, accel_channel 호출.
 * - module/bdev/malloc/bdev_malloc_rpc.c : RPC 핸들러가 본 파일의 create_malloc_disk /
 *   delete_malloc_disk를 호출.
 *
 * 데이터 흐름:
 *   [hugepage RAM] ←→ [accel HW/SW copy] ←→ [user iovec buffer]
 *
 * === 주요 함수/구조체 요약 ===
 * - struct malloc_disk : 디스크 인스턴스. spdk_bdev 임베드 + malloc_buf/malloc_md_buf 포인터.
 * - struct malloc_task : bdev_io당 컨텍스트(driver_ctx). num_outstanding과 status 보관.
 * - struct malloc_channel : 코어 로컬 채널. accel io channel + 완료 task 큐 + poller.
 * - bdev_malloc_readv / bdev_malloc_writev : 핵심 read/write 처리. accel sequence 사용.
 * - bdev_malloc_unmap / bdev_malloc_copy : zero-fill, 영역 복사.
 * - malloc_done : accel 완료 콜백. num_outstanding 감소 → 모두 끝나면 PI 검증/생성 후 완료.
 * - malloc_completion_poller : 즉시 완료 task를 다음 tick에 일괄 통보하는 poller.
 * - create_malloc_disk : RPC 핸들러가 호출하는 인스턴스 생성.
 * - malloc_disk_setup_pi : 생성 직후 hugepage 전체에 대해 초기 PI 태그를 미리 채워두는 함수.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 헤더 모음. */

#include "bdev_malloc.h"
/* [한국어] 같은 디렉토리의 공개 헤더. struct malloc_bdev_opts와 create_malloc_disk/
 * delete_malloc_disk 프로토타입을 노출. RPC 핸들러가 사용. */
#include "spdk/endian.h"
/* [한국어] 엔디언 변환 매크로. (현재 파일에서는 직접 사용 없으나 공통 헤더 의존성.) */
#include "spdk/env.h"
/* [한국어] DPDK 환경 추상화. spdk_zmalloc/spdk_free, SPDK_ENV_NUMA_ID_ANY,
 * SPDK_ENV_FOREACH_NUMA_ID, SPDK_MALLOC_DMA. hugepage 메모리 할당의 핵심. */
#include "spdk/accel.h"
/* [한국어] 메모리 가속(memcpy/zero-fill) 추상화. spdk_accel_submit_copy/fill,
 * spdk_accel_append_copy, spdk_accel_sequence_finish, spdk_accel_get_io_channel 등.
 * Intel DSA/IOAT 같은 HW 엔진을 사용하거나 SW fallback. */
#include "spdk/dma.h"
/* [한국어] DMA / memory_domain 관련. spdk_memory_domain_get_first/next — get_memory_domains
 * 콜백에서 사용. */
#include "spdk/likely.h"
/* [한국어] 분기 예측 매크로. */
#include "spdk/string.h"
/* [한국어] spdk_sprintf_alloc — Malloc%d 자동 이름 생성에 사용. */

#include "spdk/log.h"
/* [한국어] 로그 매크로(SPDK_DEBUGLOG/ERRLOG)와 SPDK_LOG_REGISTER_COMPONENT. */

/*
 * [한국어] struct malloc_disk
 *
 * Malloc bdev 인스턴스 1개당 1개. spdk_bdev를 임베드해 SPDK_CONTAINEROF로 역포인터 가능.
 * malloc_buf는 데이터(또는 인터리브 시 데이터+메타) hugepage 영역, malloc_md_buf는 분리 메타
 * 모드일 때만 사용되는 별도 메타데이터 영역.
 */
struct malloc_disk {
	struct spdk_bdev		disk;
	/* [한국어] bdev core가 인식하는 공개 메타데이터.
	 * 설정자: create_malloc_disk가 RPC 옵션으로부터 채움. 읽는 자: bdev core / 사용자.
	 * 동기화: 등록 후 read-only가 일반적. */

	void				*malloc_buf;
	/* [한국어] 데이터 백엔드 hugepage 영역 시작 주소. 크기 = num_blocks * blocklen.
	 * 인터리브 모드면 데이터+메타가 한 영역에 섞여 있고, 분리 모드면 데이터만.
	 * 설정자: create_malloc_disk의 spdk_zmalloc.
	 * 읽는 자: read/write/unmap/copy 처리에서 base + offset 계산.
	 * 값 범위: 비-NULL DMA 가능 hugepage 포인터.
	 * 동기화: 핫패스에서 같은 LBA에 동시 read/write 시 race는 사용자 책임(NVMe와 동일 의미). */

	void				*malloc_md_buf;
	/* [한국어] 분리 메타데이터(DIX) 영역. 인터리브 모드(md_interleave=true)거나 md_size=0이면 NULL.
	 * 크기 = num_blocks * md_len.
	 * 설정자: create_malloc_disk(분리 모드 전용 분기에서 spdk_zmalloc).
	 * 읽는 자: malloc_get_md_buf, _malloc_verify_pi(분리 모드 분기), bdev_malloc_readv/writev. */

	TAILQ_ENTRY(malloc_disk)	link;
	/* [한국어] g_malloc_disks 글로벌 리스트 노드. 단일 RPC 스레드에서만 갱신. */
};

/*
 * [한국어] struct malloc_task
 *
 * spdk_bdev_io 1개당 모듈 컨텍스트(driver_ctx). 한 bdev_io가 여러 accel 호출(데이터+메타)을
 * 사용할 수 있어 num_outstanding 카운터로 모두 끝났는지 추적한다.
 */
struct malloc_task {
	struct iovec			iov;
	/* [한국어] 모듈이 채우는 단일 iovec — accel API에 전달할 src/dst 표현.
	 * read 시: malloc_buf+offset (src). write 시: malloc_buf+offset (dst).
	 * 설정자/읽는 자: bdev_malloc_readv / writev. */

	int				num_outstanding;
	/* [한국어] 이 task가 발행한 accel 작업 중 아직 완료되지 않은 수. 데이터 전송 1 + (분리 메타면 +1).
	 * 설정자: 각 accel 호출 직전에 ++.
	 * 읽는 자: malloc_done이 -- 후 0이 되면 spdk_bdev_io_complete 호출.
	 * 동기화: 채널 reactor 안에서만 갱신되므로 atomic 불필요. */

	enum spdk_bdev_io_status	status;
	/* [한국어] task의 누적 상태. 첫 번째 실패가 sticky하게 기록된다.
	 * 설정자: malloc_done이 status에 따라 SUCCESS/FAILED/NOMEM 갱신.
	 * 읽는 자: malloc_done 마지막에 spdk_bdev_io_complete에 전달. */

	TAILQ_ENTRY(malloc_task)	tailq;
	/* [한국어] malloc_channel.completed_tasks 노드. 즉시 완료(RESET 등)된 task를 큐잉해
	 * 다음 poller tick에 일괄 통보. */
};

/*
 * [한국어] struct malloc_channel
 *
 * 코어 로컬 채널. accel framework의 io channel을 빌려 쓰고, "이번 tick에 완료될 task"의 큐와
 * poller를 가진다. 코어 로컬이므로 락 없이 접근 가능.
 */
struct malloc_channel {
	struct spdk_io_channel		*accel_channel;
	/* [한국어] accel framework의 io channel. spdk_accel_get_io_channel로 획득.
	 * accel 작업(spdk_accel_submit_copy 등)에 첫 인자로 전달.
	 * 설정자: malloc_create_channel_cb. 읽는 자: read/write/unmap/copy 처리 함수. */

	struct spdk_poller		*completion_poller;
	/* [한국어] completed_tasks 큐를 비우는 polling 콜백 핸들. SPDK_POLLER_REGISTER 결과.
	 * 설정자: malloc_create_channel_cb. 읽는 자: malloc_destroy_channel_cb의 unregister. */

	TAILQ_HEAD(, malloc_task)	completed_tasks;
	/* [한국어] 즉시 완료된 task가 다음 poller tick에 spdk_bdev_io_complete를 호출받기 위해
	 * 잠시 대기하는 큐. RESET/FLUSH/ZCOPY 같은 즉시 완료 케이스에 사용.
	 * 설정자: malloc_complete_task의 INSERT_TAIL.
	 * 읽는 자: malloc_completion_poller의 SWAP/REMOVE.
	 * 동기화: 채널 reactor 로컬. */
};

/*
 * [한국어]
 * _malloc_verify_pi - 주어진 iovec/메타에 대해 DIF/DIX(Protection Information)를 검증
 *
 * @bdev_io: I/O.
 * @iovs/@iovcnt: 검증할 데이터 iovec.
 * @md_buf: 분리 메타 모드일 때 메타 버퍼(인터리브 모드는 무시).
 * @return: 0 = OK, 음수 = 실패.
 *
 * DIF(인터리브) 또는 DIX(분리)에 따라 spdk_dif_verify / spdk_dix_verify 중 하나를 호출.
 * NVMe Base Spec §8.3 End-to-End Data Protection. memory_domain이 NULL일 때만 사용 가능
 * (CPU 직접 접근 가능한 버퍼에서만 검증 수행).
 */
static int
_malloc_verify_pi(struct spdk_bdev_io *bdev_io, struct iovec *iovs, int iovcnt,
		  void *md_buf)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_error err_blk;
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;

	assert(bdev_io->u.bdev.memory_domain == NULL);
	/* [한국어] memory_domain이 있는 경우는 데이터가 외부 메모리(예: GPU/SmartNIC)에 있어
	 * CPU 직접 검증 불가. 호출 전 바깥에서 보장해야 함. */
	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = bdev->dif_pi_format;
	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen,
			       bdev->md_len,
			       bdev->md_interleave,
			       bdev->dif_is_head_of_md,
			       bdev->dif_type,
			       bdev_io->u.bdev.dif_check_flags,
			       bdev_io->u.bdev.offset_blocks & 0xFFFFFFFF,
			       /* [한국어] 시작 LBA의 하위 32비트 → reftag 초기값. */
			       0xFFFF, 0, 0, 0, &dif_opts);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize DIF/DIX context\n");
		return rc;
	}

	if (spdk_bdev_is_md_interleaved(bdev)) {
		/* [한국어] 인터리브: 각 블록 끝(또는 앞)에 PI가 포함됨. spdk_dif_verify가 통합 처리. */
		rc = spdk_dif_verify(iovs,
				     iovcnt,
				     bdev_io->u.bdev.num_blocks,
				     &dif_ctx,
				     &err_blk);
	} else {
		/* [한국어] 분리(DIX): 데이터 iovec와 메타 iovec를 별도로 받아 PI를 검증. */
		struct iovec md_iov = {
			.iov_base	= md_buf,
			.iov_len	= bdev_io->u.bdev.num_blocks * bdev->md_len,
		};

		if (bdev_io->u.bdev.md_buf == NULL) {
			/* [한국어] 사용자가 메타 버퍼를 안 줬으면 검증 생략(상위가 PI를 사용 안 한다고 간주). */
			return 0;
		}

		rc = spdk_dix_verify(iovs,
				     iovcnt,
				     &md_iov,
				     bdev_io->u.bdev.num_blocks,
				     &dif_ctx,
				     &err_blk);
	}

	if (rc != 0) {
		/* [한국어] 첫 실패 블록의 위치/원인을 자세히 출력 — 디버깅에 유용. */
		SPDK_ERRLOG("DIF/DIX verify failed: lba %" PRIu64 ", num_blocks %" PRIu64 ", "
			    "err_type %u, expected %lu, actual %lu, err_offset %u\n",
			    bdev_io->u.bdev.offset_blocks,
			    bdev_io->u.bdev.num_blocks,
			    err_blk.err_type,
			    err_blk.expected,
			    err_blk.actual,
			    err_blk.err_offset);
	}

	return rc;
}

/*
 * [한국어]
 * malloc_verify_pi_io_buf - 사용자 iovec와 사용자 md_buf로 PI 검증
 *
 * write 경로: 사용자가 보낸 데이터의 PI를 검증해 매체(=hugepage)에 들어가기 전에 깨진 데이터를 차단.
 */
static int
malloc_verify_pi_io_buf(struct spdk_bdev_io *bdev_io)
{
	return _malloc_verify_pi(bdev_io,
				 bdev_io->u.bdev.iovs,
				 bdev_io->u.bdev.iovcnt,
				 bdev_io->u.bdev.md_buf);
}

/*
 * [한국어]
 * malloc_verify_pi_malloc_buf - hugepage 안의 데이터에 대해 PI 검증
 *
 * read after copy / write hide_metadata 경로 등에서 매체 측 데이터에 PI가 들어 있을 때 사용.
 */
static int
malloc_verify_pi_malloc_buf(struct spdk_bdev_io *bdev_io)
{
	struct iovec iov;
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct malloc_disk *mdisk = bdev->ctxt;
	uint64_t len, offset;

	len = bdev_io->u.bdev.num_blocks * bdev->blocklen;
	/* [한국어] 검증 영역 길이 (바이트). 인터리브 모드면 데이터+메타 모두 포함하는 합산 크기. */
	offset = bdev_io->u.bdev.offset_blocks * bdev->blocklen;
	/* [한국어] hugepage 시작에서의 바이트 오프셋. */

	iov.iov_base = mdisk->malloc_buf + offset;
	iov.iov_len = len;

	return _malloc_verify_pi(bdev_io, &iov, 1, NULL);
	/* [한국어] 인터리브 모드라 메타 버퍼는 NULL — _malloc_verify_pi가 내부에서 데이터 안의 PI 사용. */
}

/*
 * [한국어]
 * malloc_unmap_write_zeroes_generate_pi - UNMAP/WRITE_ZEROES 후 hugepage에 PI 태그 재생성
 *
 * @bdev_io: I/O.
 * @return: 0 = OK, 음수 = 실패.
 *
 * UNMAP과 WRITE_ZEROES는 데이터를 0으로 만든다. DIF가 활성화된 디스크에서는 0 데이터에 대한
 * 합법적 PI(guard tag = 0의 CRC, reftag = LBA 등)를 다시 만들어 두어야 다음 read에서 검증
 * 통과가 가능하다. APPTAG/REFTAG는 IGNORE 플래그로 강제해 사용자 정의 값을 무시.
 */
static int
malloc_unmap_write_zeroes_generate_pi(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct malloc_disk *mdisk = bdev_io->bdev->ctxt;
	uint32_t block_size = bdev_io->bdev->blocklen;
	uint32_t dif_check_flags;
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	int rc;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = bdev->dif_pi_format;
	dif_check_flags = bdev->dif_check_flags | SPDK_DIF_CHECK_TYPE_REFTAG |
			  SPDK_DIF_FLAGS_APPTAG_CHECK;
	/* [한국어] 비트마스크에 reftag/apptag 검사 추가 — 다만 reftag/apptag 값은 IGNORE 처리. */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen,
			       bdev->md_len,
			       bdev->md_interleave,
			       bdev->dif_is_head_of_md,
			       bdev->dif_type,
			       dif_check_flags,
			       SPDK_DIF_REFTAG_IGNORE,
			       0xFFFF, SPDK_DIF_APPTAG_IGNORE,
			       0, 0, &dif_opts);
	if (rc != 0) {
		SPDK_ERRLOG("Initialization of DIF/DIX context failed\n");
		return rc;
	}

	if (bdev->md_interleave) {
		/* [한국어] 인터리브: 데이터 iovec 하나로 in-place에 PI 생성. */
		struct iovec iov = {
			.iov_base	= mdisk->malloc_buf + bdev_io->u.bdev.offset_blocks * block_size,
			.iov_len	= bdev_io->u.bdev.num_blocks * block_size,
		};

		rc = spdk_dif_generate(&iov, 1, bdev_io->u.bdev.num_blocks, &dif_ctx);
	} else {
		/* [한국어] 분리: 데이터 영역과 메타 영역에 각각 접근해 spdk_dix_generate 호출. */
		struct iovec iov = {
			.iov_base	= mdisk->malloc_buf + bdev_io->u.bdev.offset_blocks * block_size,
			.iov_len	= bdev_io->u.bdev.num_blocks * block_size,
		};

		struct iovec md_iov = {
			.iov_base	= mdisk->malloc_md_buf + bdev_io->u.bdev.offset_blocks * bdev->md_len,
			.iov_len	= bdev_io->u.bdev.num_blocks * bdev->md_len,
		};

		rc = spdk_dix_generate(&iov, 1, &md_iov, bdev_io->u.bdev.num_blocks, &dif_ctx);
	}

	if (rc != 0) {
		SPDK_ERRLOG("Formatting by DIF/DIX failed\n");
	}


	return rc;
}

static void
malloc_done(void *ref, int status)
{
	struct malloc_task *task = (struct malloc_task *)ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(task);
	int rc;

	if (status != 0) {
		if (status == -ENOMEM) {
			if (task->status == SPDK_BDEV_IO_STATUS_SUCCESS) {
				task->status = SPDK_BDEV_IO_STATUS_NOMEM;
			}
		} else {
			task->status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	if (--task->num_outstanding != 0) {
		return;
	}

	if (bdev_io->bdev->dif_type != SPDK_DIF_DISABLE &&
	    task->status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		switch (bdev_io->type) {
		case SPDK_BDEV_IO_TYPE_READ:
			if (!spdk_bdev_io_hide_metadata(bdev_io)) {
				rc = malloc_verify_pi_io_buf(bdev_io);
			} else {
				rc = 0;
			}
			break;
		case SPDK_BDEV_IO_TYPE_WRITE:
			if (!spdk_bdev_io_hide_metadata(bdev_io)) {
				rc = 0;
			} else {
				rc = malloc_verify_pi_malloc_buf(bdev_io);
			}
			break;
		case SPDK_BDEV_IO_TYPE_UNMAP:
		case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
			rc = malloc_unmap_write_zeroes_generate_pi(bdev_io);
			break;
		default:
			rc = 0;
			break;
		}

		if (rc != 0) {
			task->status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	assert(!bdev_io->u.bdev.accel_sequence || task->status == SPDK_BDEV_IO_STATUS_NOMEM);
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), task->status);
}

static void
malloc_complete_task(struct malloc_task *task, struct malloc_channel *mch,
		     enum spdk_bdev_io_status status)
{
	task->status = status;
	TAILQ_INSERT_TAIL(&mch->completed_tasks, task, tailq);
}

static TAILQ_HEAD(, malloc_disk) g_malloc_disks = TAILQ_HEAD_INITIALIZER(g_malloc_disks);

int malloc_disk_count = 0;

static int bdev_malloc_initialize(void);
static void bdev_malloc_deinitialize(void);

static int
bdev_malloc_get_ctx_size(void)
{
	return sizeof(struct malloc_task);
}

static struct spdk_bdev_module malloc_if = {
	.name = "malloc",
	.module_init = bdev_malloc_initialize,
	.module_fini = bdev_malloc_deinitialize,
	.get_ctx_size = bdev_malloc_get_ctx_size,

};

SPDK_BDEV_MODULE_REGISTER(malloc, &malloc_if)

static void
malloc_disk_free(struct malloc_disk *malloc_disk)
{
	if (!malloc_disk) {
		return;
	}

	free(malloc_disk->disk.name);
	spdk_free(malloc_disk->malloc_buf);
	spdk_free(malloc_disk->malloc_md_buf);
	free(malloc_disk);
}

static int
bdev_malloc_destruct(void *ctx)
{
	struct malloc_disk *malloc_disk = ctx;

	TAILQ_REMOVE(&g_malloc_disks, malloc_disk, link);
	malloc_disk_free(malloc_disk);
	return 0;
}

static int
bdev_malloc_check_iov_len(struct iovec *iovs, int iovcnt, size_t nbytes)
{
	int i;

	for (i = 0; i < iovcnt; i++) {
		if (nbytes < iovs[i].iov_len) {
			return 0;
		}

		nbytes -= iovs[i].iov_len;
	}

	return nbytes != 0;
}

static size_t
malloc_get_md_len(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->u.bdev.num_blocks * bdev_io->bdev->md_len;
}

static uint64_t
malloc_get_md_offset(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->u.bdev.offset_blocks * bdev_io->bdev->md_len;
}

static void *
malloc_get_md_buf(struct spdk_bdev_io *bdev_io)
{
	struct malloc_disk *mdisk = SPDK_CONTAINEROF(bdev_io->bdev, struct malloc_disk, disk);

	assert(spdk_bdev_is_md_separate(bdev_io->bdev));

	return (char *)mdisk->malloc_md_buf + malloc_get_md_offset(bdev_io);
}

static void
malloc_sequence_fail(struct malloc_task *task, int status)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(task);

	/* For ENOMEM, the IO will be retried by the bdev layer, so we don't abort the sequence */
	if (status != -ENOMEM) {
		spdk_accel_sequence_abort(bdev_io->u.bdev.accel_sequence);
		bdev_io->u.bdev.accel_sequence = NULL;
	}

	malloc_done(task, status);
}

static void
malloc_sequence_done(void *ctx, int status)
{
	struct malloc_task *task = ctx;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(task);

	bdev_io->u.bdev.accel_sequence = NULL;
	/* Prevent bdev layer from retrying the request if the sequence failed with ENOMEM */
	malloc_done(task, status != -ENOMEM ? status : -EFAULT);
}

static void
bdev_malloc_readv(struct malloc_disk *mdisk, struct spdk_io_channel *ch,
		  struct malloc_task *task, struct spdk_bdev_io *bdev_io)
{
	uint64_t len, offset;
	int res = 0;

	len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
	offset = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;

	if (bdev_malloc_check_iov_len(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, len)) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task),
				      SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = 0;
	task->iov.iov_base = mdisk->malloc_buf + offset;
	task->iov.iov_len = len;

	SPDK_DEBUGLOG(bdev_malloc, "read %zu bytes from offset %#" PRIx64 ", iovcnt=%d\n",
		      len, offset, bdev_io->u.bdev.iovcnt);

	task->num_outstanding++;
	res = spdk_accel_append_copy(&bdev_io->u.bdev.accel_sequence, ch,
				     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				     bdev_io->u.bdev.memory_domain,
				     bdev_io->u.bdev.memory_domain_ctx,
				     &task->iov, 1, NULL, NULL, NULL, NULL);
	if (spdk_unlikely(res != 0)) {
		malloc_sequence_fail(task, res);
		return;
	}

	spdk_accel_sequence_reverse(bdev_io->u.bdev.accel_sequence);
	spdk_accel_sequence_finish(bdev_io->u.bdev.accel_sequence, malloc_sequence_done, task);

	if (bdev_io->u.bdev.md_buf == NULL) {
		return;
	}

	SPDK_DEBUGLOG(bdev_malloc, "read metadata %zu bytes from offset%#" PRIx64 "\n",
		      malloc_get_md_len(bdev_io), malloc_get_md_offset(bdev_io));

	task->num_outstanding++;
	res = spdk_accel_submit_copy(ch, bdev_io->u.bdev.md_buf, malloc_get_md_buf(bdev_io),
				     malloc_get_md_len(bdev_io), malloc_done, task);
	if (res != 0) {
		malloc_done(task, res);
	}
}

static void
bdev_malloc_writev(struct malloc_disk *mdisk, struct spdk_io_channel *ch,
		   struct malloc_task *task, struct spdk_bdev_io *bdev_io)
{
	uint64_t len, offset;
	int res = 0;

	len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
	offset = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;

	if (bdev_malloc_check_iov_len(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, len)) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task),
				      SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = 0;
	task->iov.iov_base = mdisk->malloc_buf + offset;
	task->iov.iov_len = len;

	SPDK_DEBUGLOG(bdev_malloc, "write %zu bytes to offset %#" PRIx64 ", iovcnt=%d\n",
		      len, offset, bdev_io->u.bdev.iovcnt);

	task->num_outstanding++;
	res = spdk_accel_append_copy(&bdev_io->u.bdev.accel_sequence, ch, &task->iov, 1, NULL, NULL,
				     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				     bdev_io->u.bdev.memory_domain,
				     bdev_io->u.bdev.memory_domain_ctx, NULL, NULL);
	if (spdk_unlikely(res != 0)) {
		malloc_sequence_fail(task, res);
		return;
	}

	spdk_accel_sequence_finish(bdev_io->u.bdev.accel_sequence, malloc_sequence_done, task);

	if (bdev_io->u.bdev.md_buf == NULL) {
		return;
	}

	SPDK_DEBUGLOG(bdev_malloc, "write metadata %zu bytes to offset %#" PRIx64 "\n",
		      malloc_get_md_len(bdev_io), malloc_get_md_offset(bdev_io));

	task->num_outstanding++;
	res = spdk_accel_submit_copy(ch, malloc_get_md_buf(bdev_io), bdev_io->u.bdev.md_buf,
				     malloc_get_md_len(bdev_io), malloc_done, task);
	if (res != 0) {
		malloc_done(task, res);
	}
}

static int
bdev_malloc_unmap(struct malloc_disk *mdisk,
		  struct spdk_io_channel *ch,
		  struct malloc_task *task,
		  uint64_t offset,
		  uint64_t byte_count)
{
	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = 1;

	return spdk_accel_submit_fill(ch, mdisk->malloc_buf + offset, 0,
				      byte_count, malloc_done, task);
}

static void
bdev_malloc_copy(struct malloc_disk *mdisk, struct spdk_io_channel *ch,
		 struct malloc_task *task,
		 uint64_t dst_offset, uint64_t src_offset, size_t len)
{
	int64_t res = 0;
	void *dst = mdisk->malloc_buf + dst_offset;
	void *src = mdisk->malloc_buf + src_offset;

	SPDK_DEBUGLOG(bdev_malloc, "Copy %zu bytes from offset %#" PRIx64 " to offset %#" PRIx64 "\n",
		      len, src_offset, dst_offset);

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = 1;

	res = spdk_accel_submit_copy(ch, dst, src, len, malloc_done, task);
	if (res != 0) {
		malloc_done(task, res);
	}
}

static int
_bdev_malloc_submit_request(struct malloc_channel *mch, struct spdk_bdev_io *bdev_io)
{
	struct malloc_task *task = (struct malloc_task *)bdev_io->driver_ctx;
	struct malloc_disk *disk = bdev_io->bdev->ctxt;
	uint32_t block_size = bdev_io->bdev->blocklen;
	int rc;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.iovs[0].iov_base == NULL) {
			assert(bdev_io->u.bdev.iovcnt == 1);
			assert(bdev_io->u.bdev.memory_domain == NULL);
			bdev_io->u.bdev.iovs[0].iov_base =
				disk->malloc_buf + bdev_io->u.bdev.offset_blocks * block_size;
			bdev_io->u.bdev.iovs[0].iov_len = bdev_io->u.bdev.num_blocks * block_size;
			if (spdk_bdev_is_md_separate(bdev_io->bdev)) {
				spdk_bdev_io_set_md_buf(bdev_io, malloc_get_md_buf(bdev_io),
							malloc_get_md_len(bdev_io));
			}
			malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_SUCCESS);
			return 0;
		}

		if (bdev_io->bdev->dif_type != SPDK_DIF_DISABLE &&
		    spdk_bdev_io_hide_metadata(bdev_io)) {
			rc = malloc_verify_pi_malloc_buf(bdev_io);
			if (rc != 0) {
				malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_FAILED);
				return 0;
			}
		}

		bdev_malloc_readv(disk, mch->accel_channel, task, bdev_io);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		if (bdev_io->bdev->dif_type != SPDK_DIF_DISABLE &&
		    !spdk_bdev_io_hide_metadata(bdev_io)) {
			rc = malloc_verify_pi_io_buf(bdev_io);
			if (rc != 0) {
				malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_FAILED);
				return 0;
			}
		}

		bdev_malloc_writev(disk, mch->accel_channel, task, bdev_io);
		return 0;

	case SPDK_BDEV_IO_TYPE_RESET:
		malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;

	case SPDK_BDEV_IO_TYPE_FLUSH:
		malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		return bdev_malloc_unmap(disk, mch->accel_channel, task,
					 bdev_io->u.bdev.offset_blocks * block_size,
					 bdev_io->u.bdev.num_blocks * block_size);

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		/* bdev_malloc_unmap is implemented with a call to mem_cpy_fill which zeroes out all of the requested bytes. */
		return bdev_malloc_unmap(disk, mch->accel_channel, task,
					 bdev_io->u.bdev.offset_blocks * block_size,
					 bdev_io->u.bdev.num_blocks * block_size);

	case SPDK_BDEV_IO_TYPE_ZCOPY:
		if (bdev_io->u.bdev.zcopy.start) {
			void *buf;
			size_t len;

			buf = disk->malloc_buf + bdev_io->u.bdev.offset_blocks * block_size;
			len = bdev_io->u.bdev.num_blocks * block_size;
			spdk_bdev_io_set_buf(bdev_io, buf, len);
			if (spdk_bdev_is_md_separate(bdev_io->bdev)) {
				spdk_bdev_io_set_md_buf(bdev_io, malloc_get_md_buf(bdev_io),
							malloc_get_md_len(bdev_io));
			}
		}
		malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	case SPDK_BDEV_IO_TYPE_ABORT:
		malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_FAILED);
		return 0;
	case SPDK_BDEV_IO_TYPE_COPY:
		bdev_malloc_copy(disk, mch->accel_channel, task,
				 bdev_io->u.bdev.offset_blocks * block_size,
				 bdev_io->u.bdev.copy.src_offset_blocks * block_size,
				 bdev_io->u.bdev.num_blocks * block_size);
		return 0;

	default:
		return -1;
	}
	return 0;
}

static void
bdev_malloc_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct malloc_channel *mch = spdk_io_channel_get_ctx(ch);

	if (_bdev_malloc_submit_request(mch, bdev_io) != 0) {
		malloc_complete_task((struct malloc_task *)bdev_io->driver_ctx, mch,
				     SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_malloc_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_ZCOPY:
	case SPDK_BDEV_IO_TYPE_ABORT:
	case SPDK_BDEV_IO_TYPE_COPY:
		return true;

	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_malloc_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_malloc_disks);
}

static void
bdev_malloc_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_malloc_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint64(w, "num_blocks", bdev->blockcnt);
	spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	spdk_json_write_named_uint32(w, "physical_block_size", bdev->phys_blocklen);
	spdk_json_write_named_uuid(w, "uuid", &bdev->uuid);
	spdk_json_write_named_uint32(w, "optimal_io_boundary", bdev->optimal_io_boundary);
	spdk_json_write_named_uint32(w, "md_size", bdev->md_len);
	spdk_json_write_named_uint32(w, "dif_type", bdev->dif_type);
	spdk_json_write_named_bool(w, "dif_is_head_of_md", bdev->dif_is_head_of_md);
	spdk_json_write_named_uint32(w, "dif_pi_format", bdev->dif_pi_format);
	spdk_json_write_named_int32(w, "numa_id", spdk_bdev_get_numa_id(bdev));

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static int
bdev_malloc_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct malloc_disk *malloc_disk = ctx;
	struct spdk_memory_domain *domain;
	int num_domains = 0;

	if (malloc_disk->disk.dif_type != SPDK_DIF_DISABLE) {
		return 0;
	}

	/* Report support for every memory domain */
	for (domain = spdk_memory_domain_get_first(NULL); domain != NULL;
	     domain = spdk_memory_domain_get_next(domain, NULL)) {
		if (domains != NULL && num_domains < array_size) {
			domains[num_domains] = domain;
		}
		num_domains++;
	}

	return num_domains;
}

static bool
bdev_malloc_accel_sequence_supported(void *ctx, enum spdk_bdev_io_type type)
{
	switch (type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;
	default:
		return false;
	}
}

static const struct spdk_bdev_fn_table malloc_fn_table = {
	.destruct			= bdev_malloc_destruct,
	.submit_request			= bdev_malloc_submit_request,
	.io_type_supported		= bdev_malloc_io_type_supported,
	.get_io_channel			= bdev_malloc_get_io_channel,
	.write_config_json		= bdev_malloc_write_json_config,
	.get_memory_domains		= bdev_malloc_get_memory_domains,
	.accel_sequence_supported	= bdev_malloc_accel_sequence_supported,
};

static int
malloc_disk_setup_pi(struct malloc_disk *mdisk)
{
	struct spdk_bdev *bdev = &mdisk->disk;
	struct spdk_dif_ctx dif_ctx;
	struct iovec iov, md_iov;
	uint32_t dif_check_flags;
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = bdev->dif_pi_format;
	/* Set APPTAG|REFTAG_IGNORE to PI fields after creation of malloc bdev */
	dif_check_flags = bdev->dif_check_flags | SPDK_DIF_CHECK_TYPE_REFTAG |
			  SPDK_DIF_FLAGS_APPTAG_CHECK;
	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen,
			       bdev->md_len,
			       bdev->md_interleave,
			       bdev->dif_is_head_of_md,
			       bdev->dif_type,
			       dif_check_flags,
			       SPDK_DIF_REFTAG_IGNORE,
			       0xFFFF, SPDK_DIF_APPTAG_IGNORE,
			       0, 0, &dif_opts);
	if (rc != 0) {
		SPDK_ERRLOG("Initialization of DIF/DIX context failed\n");
		return rc;
	}

	iov.iov_base = mdisk->malloc_buf;
	iov.iov_len = bdev->blockcnt * bdev->blocklen;

	if (mdisk->disk.md_interleave) {
		rc = spdk_dif_generate(&iov, 1, bdev->blockcnt, &dif_ctx);
	} else {
		md_iov.iov_base = mdisk->malloc_md_buf;
		md_iov.iov_len = bdev->blockcnt * bdev->md_len;

		rc = spdk_dix_generate(&iov, 1, &md_iov, bdev->blockcnt, &dif_ctx);
	}

	if (rc != 0) {
		SPDK_ERRLOG("Formatting by DIF/DIX failed\n");
	}

	return rc;
}

int
create_malloc_disk(struct spdk_bdev **bdev, const struct malloc_bdev_opts *opts)
{
	struct malloc_disk *mdisk;
	uint32_t block_size;
	int32_t numa_id;
	int rc;

	assert(opts != NULL);

	if (opts->num_blocks == 0) {
		SPDK_ERRLOG("Disk num_blocks must be greater than 0");
		return -EINVAL;
	}

	if (opts->block_size % 512) {
		SPDK_ERRLOG("Data block size must be 512 bytes aligned\n");
		return -EINVAL;
	}

	if (opts->physical_block_size % 512) {
		SPDK_ERRLOG("Physical block must be 512 bytes aligned\n");
		return -EINVAL;
	}

	if (opts->numa_id != SPDK_ENV_NUMA_ID_ANY) {
		/* Verify if requested NUMA node ID is present on the system. */
		SPDK_ENV_FOREACH_NUMA_ID(numa_id) {
			if (numa_id == opts->numa_id) {
				break;
			}
		}
		if (numa_id == INT32_MAX) {
			SPDK_ERRLOG("NUMA node ID %d not present on the system\n", opts->numa_id);
			return -EINVAL;
		}
	}

	switch (opts->md_size) {
	case 0:
	case 8:
	case 16:
	case 32:
	case 64:
	case 128:
		break;
	default:
		SPDK_ERRLOG("metadata size %u is not supported\n", opts->md_size);
		return -EINVAL;
	}

	if (opts->md_interleave) {
		block_size = opts->block_size + opts->md_size;
	} else {
		block_size = opts->block_size;
	}

	mdisk = calloc(1, sizeof(*mdisk));
	if (!mdisk) {
		SPDK_ERRLOG("mdisk calloc() failed\n");
		return -ENOMEM;
	}

	/*
	 * Allocate the large backend memory buffer from pinned memory.
	 */
	mdisk->malloc_buf = spdk_zmalloc(opts->num_blocks * block_size, 2 * 1024 * 1024, NULL,
					 opts->numa_id, SPDK_MALLOC_DMA);
	if (!mdisk->malloc_buf) {
		SPDK_ERRLOG("malloc_buf spdk_zmalloc() failed\n");
		malloc_disk_free(mdisk);
		return -ENOMEM;
	}

	if (!opts->md_interleave && opts->md_size != 0) {
		mdisk->malloc_md_buf = spdk_zmalloc(opts->num_blocks * opts->md_size, 2 * 1024 * 1024, NULL,
						    opts->numa_id, SPDK_MALLOC_DMA);
		if (!mdisk->malloc_md_buf) {
			SPDK_ERRLOG("malloc_md_buf spdk_zmalloc() failed\n");
			malloc_disk_free(mdisk);
			return -ENOMEM;
		}
	}

	if (opts->name) {
		mdisk->disk.name = strdup(opts->name);
	} else {
		/* Auto-generate a name */
		mdisk->disk.name = spdk_sprintf_alloc("Malloc%d", malloc_disk_count);
		malloc_disk_count++;
	}
	if (!mdisk->disk.name) {
		malloc_disk_free(mdisk);
		return -ENOMEM;
	}
	mdisk->disk.product_name = "Malloc disk";

	mdisk->disk.write_cache = 1;
	mdisk->disk.blocklen = block_size;
	mdisk->disk.phys_blocklen = opts->physical_block_size;
	mdisk->disk.blockcnt = opts->num_blocks;
	mdisk->disk.md_len = opts->md_size;
	mdisk->disk.md_interleave = opts->md_interleave;
	mdisk->disk.dif_type = opts->dif_type;
	mdisk->disk.dif_is_head_of_md = opts->dif_is_head_of_md;
	/* Current block device layer API does not propagate
	 * any DIF related information from user. So, we can
	 * not generate or verify Application Tag.
	 */
	switch (opts->dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		mdisk->disk.dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK |
					      SPDK_DIF_FLAGS_REFTAG_CHECK;
		break;
	case SPDK_DIF_TYPE3:
		mdisk->disk.dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK;
		break;
	case SPDK_DIF_DISABLE:
		break;
	}
	mdisk->disk.dif_pi_format = opts->dif_pi_format;

	if (opts->dif_type != SPDK_DIF_DISABLE) {
		rc = malloc_disk_setup_pi(mdisk);
		if (rc) {
			SPDK_ERRLOG("Failed to set up protection information.\n");
			malloc_disk_free(mdisk);
			return rc;
		}
	}

	if (opts->optimal_io_boundary) {
		mdisk->disk.optimal_io_boundary = opts->optimal_io_boundary;
		mdisk->disk.split_on_optimal_io_boundary = true;
	}
	if (!spdk_uuid_is_null(&opts->uuid)) {
		spdk_uuid_copy(&mdisk->disk.uuid, &opts->uuid);
	}

	mdisk->disk.numa.id_valid = 1;
	mdisk->disk.numa.id = opts->numa_id;

	mdisk->disk.max_copy = 0;
	mdisk->disk.ctxt = mdisk;
	mdisk->disk.fn_table = &malloc_fn_table;
	mdisk->disk.module = &malloc_if;

	rc = spdk_bdev_register(&mdisk->disk);
	if (rc) {
		malloc_disk_free(mdisk);
		return rc;
	}

	*bdev = &(mdisk->disk);

	TAILQ_INSERT_TAIL(&g_malloc_disks, mdisk, link);
	SPDK_DEBUGLOG(bdev_malloc, "Bdev:%s created on NUMA node ID: %d\n",
		      spdk_bdev_get_name(*bdev), spdk_bdev_get_numa_id(*bdev));

	return rc;
}

void
delete_malloc_disk(const char *name, spdk_delete_malloc_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_bdev_unregister_by_name(name, &malloc_if, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);
	}
}

static int
malloc_completion_poller(void *ctx)
{
	struct malloc_channel *ch = ctx;
	struct malloc_task *task;
	TAILQ_HEAD(, malloc_task) completed_tasks;
	uint32_t num_completions = 0;

	TAILQ_INIT(&completed_tasks);
	TAILQ_SWAP(&completed_tasks, &ch->completed_tasks, malloc_task, tailq);

	while (!TAILQ_EMPTY(&completed_tasks)) {
		task = TAILQ_FIRST(&completed_tasks);
		TAILQ_REMOVE(&completed_tasks, task, tailq);
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), task->status);
		num_completions++;
	}

	return num_completions > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
malloc_create_channel_cb(void *io_device, void *ctx)
{
	struct malloc_channel *ch = ctx;

	ch->accel_channel = spdk_accel_get_io_channel();
	if (!ch->accel_channel) {
		SPDK_ERRLOG("Failed to get accel framework's IO channel\n");
		return -ENOMEM;
	}

	ch->completion_poller = SPDK_POLLER_REGISTER(malloc_completion_poller, ch, 0);
	if (!ch->completion_poller) {
		SPDK_ERRLOG("Failed to register malloc completion poller\n");
		spdk_put_io_channel(ch->accel_channel);
		return -ENOMEM;
	}

	TAILQ_INIT(&ch->completed_tasks);

	return 0;
}

static void
malloc_destroy_channel_cb(void *io_device, void *ctx)
{
	struct malloc_channel *ch = ctx;

	assert(TAILQ_EMPTY(&ch->completed_tasks));

	spdk_put_io_channel(ch->accel_channel);
	spdk_poller_unregister(&ch->completion_poller);
}

static int
bdev_malloc_initialize(void)
{
	/* This needs to be reset for each reinitialization of submodules.
	 * Otherwise after enough devices or reinitializations the value gets too high.
	 * TODO: Make malloc bdev name mandatory and remove this counter. */
	malloc_disk_count = 0;

	spdk_io_device_register(&g_malloc_disks, malloc_create_channel_cb,
				malloc_destroy_channel_cb, sizeof(struct malloc_channel),
				"bdev_malloc");

	return 0;
}

static void
bdev_malloc_deinitialize(void)
{
	spdk_io_device_unregister(&g_malloc_disks, NULL);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_malloc)
