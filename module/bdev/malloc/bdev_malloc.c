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

/*
 * [한국어]
 * malloc_done - accel 작업 완료 콜백 (read/write/unmap/copy 공통 종결자)
 *
 * @ref:    이 콜백에 결부된 malloc_task 포인터(callback context). bdev_malloc_readv 등이
 *          spdk_accel_submit_copy/append_copy 호출 시 직접 task를 전달했거나, accel sequence
 *          종결자 malloc_sequence_done이 본 함수를 호출하면서 전달.
 * @status: accel 작업의 결과. 0=성공, -ENOMEM=메모리 부족(상위 retry 신호),
 *          그 외 음수=영구 실패.
 *
 * 동작:
 *   1) 누적 status 갱신(첫 실패가 sticky). ENOMEM은 SUCCESS일 때만 NOMEM으로 격상.
 *   2) num_outstanding을 감소시키고 모두 끝났을 때만 다음 단계 진행.
 *   3) DIF/DIX가 활성화된 디스크인 경우 I/O 종류에 따라 PI 검증/생성을 한 번 더 수행:
 *      - READ: hide_metadata가 아닌 경우, 사용자 버퍼에 들어간 데이터/메타에 대해 PI 검증.
 *      - WRITE: hide_metadata인 경우, hugepage에 기록된 데이터/메타에 대해 PI 검증.
 *      - UNMAP/WRITE_ZEROES: 0으로 채워진 hugepage에 대해 PI 태그 재생성(legal PI).
 *   4) 최종적으로 spdk_bdev_io_complete를 호출해 bdev core에 통보 → 사용자 cb_fn 호출.
 *
 * 실행 컨텍스트: malloc_channel의 accel 콜백 컨텍스트(=채널 reactor 스레드). 락 없음.
 * 호출 체인:
 *   spdk_accel_* HW/SW 완료 → accel framework 콜백 → malloc_done
 *      → (DIF 후처리) → spdk_bdev_io_complete → 사용자 cb_fn
 */
static void
malloc_done(void *ref, int status)
{
	struct malloc_task *task = (struct malloc_task *)ref;
	/* [한국어] 모듈이 driver_ctx로 심어둔 task 컨텍스트 복원 — bdev_io 1:1 대응. */
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(task);
	/* [한국어] task → bdev_io 역참조(spdk_bdev_io의 끝에 driver_ctx가 임베드되어 있음). */
	int rc;

	if (status != 0) {
		/* [한국어] accel 작업이 실패한 경우. */
		if (status == -ENOMEM) {
			/* [한국어] HW 자원 부족(예: DSA 큐 full) — 상위 bdev layer가 retry할 수 있도록
			 * NOMEM 상태로 격상(단, 이미 FAILED라면 그대로 둠 — sticky 우선). */
			if (task->status == SPDK_BDEV_IO_STATUS_SUCCESS) {
				task->status = SPDK_BDEV_IO_STATUS_NOMEM;
			}
		} else {
			/* [한국어] ENOMEM 외 모든 에러는 즉시 영구 실패로 마킹. */
			task->status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	if (--task->num_outstanding != 0) {
		/* [한국어] 데이터+메타 두 번 호출하는 케이스에서 두 콜백이 모두 들어와야 진행.
		 * 아직 남은 작업이 있으면 여기서 반환하고 마지막 콜백이 종결 처리를 담당. */
		return;
	}

	if (bdev_io->bdev->dif_type != SPDK_DIF_DISABLE &&
	    task->status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		/* [한국어] DIF 디스크이고 지금까지 누적 성공이면 I/O 후처리 PI 단계 수행.
		 * 실패한 경우엔 후처리해도 의미 없으므로 스킵. */
		switch (bdev_io->type) {
		case SPDK_BDEV_IO_TYPE_READ:
			/* [한국어] 읽기 후: 사용자에게 돌려줄 데이터의 PI 검증.
			 * hide_metadata 모드면 사용자에게 메타가 노출되지 않으므로 추가 검증 불필요. */
			if (!spdk_bdev_io_hide_metadata(bdev_io)) {
				rc = malloc_verify_pi_io_buf(bdev_io);
			} else {
				rc = 0;
			}
			break;
		case SPDK_BDEV_IO_TYPE_WRITE:
			/* [한국어] 쓰기 후: 사용자 데이터의 PI는 이미 submit 단계에서 검증되었으므로
			 * 추가 검증 불필요. 단, hide_metadata 모드(사용자가 메타 안 주는 모드)에서는
			 * malloc_buf에 매체측 PI가 새로 들어왔으므로 검증 한 번 더. */
			if (!spdk_bdev_io_hide_metadata(bdev_io)) {
				rc = 0;
			} else {
				rc = malloc_verify_pi_malloc_buf(bdev_io);
			}
			break;
		case SPDK_BDEV_IO_TYPE_UNMAP:
		case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
			/* [한국어] 데이터가 0으로 채워졌으므로 다음 read에서 PI 검증이 깨지지 않도록
			 * 0에 대응하는 합법적 PI 태그를 hugepage에 미리 재생성. */
			rc = malloc_unmap_write_zeroes_generate_pi(bdev_io);
			break;
		default:
			/* [한국어] RESET/FLUSH/ZCOPY/COPY/ABORT 등은 PI 후처리 불필요. */
			rc = 0;
			break;
		}

		if (rc != 0) {
			/* [한국어] PI 검증/생성에서 실패하면 최종 상태를 FAILED로 격상. */
			task->status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	assert(!bdev_io->u.bdev.accel_sequence || task->status == SPDK_BDEV_IO_STATUS_NOMEM);
	/* [한국어] accel sequence가 사용된 경로에서는 malloc_sequence_done이 sequence를 NULL로
	 * 비워야 정상. 비워지지 않았는데 여기까지 왔다면 NOMEM 케이스(시퀀스 abort 안 한 경우)뿐. */
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), task->status);
	/* [한국어] bdev core에 완료 통지 — bdev core가 사용자 cb_fn 호출 및 자원 회수 진행. */
}

/*
 * [한국어]
 * malloc_complete_task - 즉시 완료(synchronous) task를 채널 큐에 enqueue
 *
 * @task:   완료시킬 task.
 * @mch:    채널 컨텍스트(완료 큐 보유).
 * @status: 최종 상태(SUCCESS/FAILED/NOMEM).
 *
 * SPDK bdev 규약상 submit_request 안에서 동기적으로 spdk_bdev_io_complete를 호출하면
 * 재진입성과 스택 사용량 측면에서 위험하다(예: complete → 같은 사용자 콜백이 다시
 * spdk_bdev_io 발행). 따라서 즉시 완료해야 하는 RESET/FLUSH/ZCOPY/ABORT 등은 채널의
 * completed_tasks 큐에 넣어 다음 poller tick(malloc_completion_poller)에서 일괄 통보한다.
 *
 * 실행 컨텍스트: submit_request 컨텍스트(채널 reactor) — 채널 로컬이라 락 불필요.
 */
static void
malloc_complete_task(struct malloc_task *task, struct malloc_channel *mch,
		     enum spdk_bdev_io_status status)
{
	task->status = status;
	/* [한국어] 큐잉 단계에서 최종 상태를 미리 박아 둠 — poller는 그대로 complete만 호출. */
	TAILQ_INSERT_TAIL(&mch->completed_tasks, task, tailq);
	/* [한국어] FIFO로 큐 끝에 삽입 — poller가 SWAP으로 통째로 빼서 순서대로 처리. */
}

static TAILQ_HEAD(, malloc_disk) g_malloc_disks = TAILQ_HEAD_INITIALIZER(g_malloc_disks);
/* [한국어] 모든 Malloc bdev 인스턴스의 전역 리스트.
 * 설정자: create_malloc_disk(INSERT_TAIL), bdev_malloc_destruct(REMOVE).
 * 읽는 자: 디버깅/검사 시 순회. 단일 RPC 스레드(메인)에서만 수정되므로 락 불필요.
 * 부가 용도: io_device로도 등록되어 spdk_io_device_register/get_io_channel의 키 역할(주소 자체). */

int malloc_disk_count = 0;
/* [한국어] 이름 없는(auto-name) Malloc bdev에 "Malloc%d" 접미 인덱스 부여용 카운터.
 * bdev_malloc_initialize에서 0으로 리셋(서브모듈 재초기화 대비). 단일 스레드 갱신. */

static int bdev_malloc_initialize(void);
/* [한국어] forward decl — bdev_malloc 모듈 초기화 진입점(SPDK 모듈 시스템이 호출). */
static void bdev_malloc_deinitialize(void);
/* [한국어] forward decl — 종료 시 io_device 해제. */

/*
 * [한국어]
 * bdev_malloc_get_ctx_size - bdev_io에 임베드할 driver_ctx 크기 반환
 *
 * bdev core가 spdk_bdev_io를 할당할 때 모듈별 컨텍스트 크기를 미리 알아야 하므로 모듈
 * 등록 시 콜백으로 제공. 본 모듈은 malloc_task를 driver_ctx로 사용.
 */
static int
bdev_malloc_get_ctx_size(void)
{
	return sizeof(struct malloc_task);
	/* [한국어] bdev_io 끝에 malloc_task 크기만큼 자리 확보 — spdk_bdev_io_from_ctx로 역참조 가능. */
}

static struct spdk_bdev_module malloc_if = {
	/* [한국어] SPDK bdev 모듈 등록 디스크립터. 모듈 라이프사이클 콜백 모음. */
	.name = "malloc",
	/* [한국어] 모듈 식별 이름 — RPC bdev_malloc_create의 module 매칭 키, 로그 출력에도 사용. */
	.module_init = bdev_malloc_initialize,
	/* [한국어] SPDK 부팅 시 호출(env_init 직후). io_device 등록. */
	.module_fini = bdev_malloc_deinitialize,
	/* [한국어] SPDK 종료 시 호출. io_device 해제. */
	.get_ctx_size = bdev_malloc_get_ctx_size,
	/* [한국어] bdev_io 할당 시 driver_ctx 크기 반환 콜백. */

};

SPDK_BDEV_MODULE_REGISTER(malloc, &malloc_if)
/* [한국어] 컴파일 타임 모듈 등록 매크로 — __attribute__((constructor))로 등록되어
 * SPDK_BDEV_MODULE_INITIALIZE() 시 자동 발견됨. */

/*
 * [한국어]
 * malloc_disk_free - malloc_disk 인스턴스의 모든 자원 해제
 *
 * @malloc_disk: 해제할 디스크 (NULL 허용 — early exit).
 *
 * create_malloc_disk 도중 실패 롤백 경로와 bdev_malloc_destruct 정상 해제 경로 모두에서
 * 사용. spdk_free는 spdk_zmalloc과 짝이 되는 hugepage 해제(DPDK rte_free 기반).
 */
static void
malloc_disk_free(struct malloc_disk *malloc_disk)
{
	if (!malloc_disk) {
		/* [한국어] NULL 입력 방어 — calloc 실패 등 일부 호출 경로 단순화. */
		return;
	}

	free(malloc_disk->disk.name);
	/* [한국어] strdup 또는 spdk_sprintf_alloc로 할당한 일반 heap 메모리 해제. */
	spdk_free(malloc_disk->malloc_buf);
	/* [한국어] hugepage 영역 해제(DPDK rte_free). NULL이면 no-op이라 안전. */
	spdk_free(malloc_disk->malloc_md_buf);
	/* [한국어] 분리 메타 영역(있을 때만 비-NULL). 인터리브/md_size=0이면 NULL이라 no-op. */
	free(malloc_disk);
	/* [한국어] 구조체 자체 해제 — calloc로 할당했으므로 free. */
}

/*
 * [한국어]
 * bdev_malloc_destruct - fn_table.destruct 콜백, bdev 인스턴스 종료 시 호출
 *
 * @ctx: malloc_disk 포인터 (mdisk->disk.ctxt에 박혀 있음).
 *
 * 흐름: 사용자/RPC가 spdk_bdev_unregister_by_name → bdev core가 ref가 0이 되었을 때
 * 모듈의 destruct 콜백 호출. g_malloc_disks 리스트에서 빼고 모든 자원 해제.
 */
static int
bdev_malloc_destruct(void *ctx)
{
	struct malloc_disk *malloc_disk = ctx;
	/* [한국어] ctxt(void*) → malloc_disk 복원. create_malloc_disk에서 self pointer 저장됨. */

	TAILQ_REMOVE(&g_malloc_disks, malloc_disk, link);
	/* [한국어] 전역 리스트에서 제거 — 이후 다른 코드가 이 디스크를 순회로 발견하지 못함. */
	malloc_disk_free(malloc_disk);
	/* [한국어] hugepage 버퍼와 메타 버퍼, 이름, 구조체 자체 해제. */
	return 0;
	/* [한국어] 동기 해제 완료 — bdev core가 추가 처리 없이 unregister 콜백 호출 진행. */
}

/*
 * [한국어]
 * bdev_malloc_check_iov_len - 사용자 iovec 총합이 요청 바이트와 정확히 일치하는지 검증
 *
 * @iovs/@iovcnt: 사용자 iovec 배열.
 * @nbytes:       요청 데이터 길이 (num_blocks * blocklen).
 * @return: 0 = 길이 일치(OK), 비-0 = 일치하지 않음(에러).
 *
 * iovec 합이 nbytes보다 크거나 작으면 처리할 수 없으므로 사전 검사. 합이 작으면 루프
 * 종료 후 nbytes != 0가 남고, 합이 크면 중간에 nbytes < iovs[i].iov_len이 되어 0 반환.
 * 반환 값 의미가 직관과 반대(0=OK)이므로 호출부에서 `if (check_iov_len) { 실패 }` 패턴.
 */
static int
bdev_malloc_check_iov_len(struct iovec *iovs, int iovcnt, size_t nbytes)
{
	int i;

	for (i = 0; i < iovcnt; i++) {
		if (nbytes < iovs[i].iov_len) {
			/* [한국어] 누적 합이 요청보다 커지는 시점 — 이미 가용 데이터를 초과했으므로 비정상. */
			return 0;
		}

		nbytes -= iovs[i].iov_len;
		/* [한국어] 남은 필요 바이트에서 이번 iovec가 채우는 만큼 차감. */
	}

	return nbytes != 0;
	/* [한국어] 모든 iovec 소비 후 남은 nbytes==0이면 정확히 일치(0=OK). 그 외는 부족(비-0=에러). */
}

/*
 * [한국어]
 * malloc_get_md_len - I/O에 대응하는 분리 메타 영역 길이 계산 (바이트)
 *
 * 분리 메타 모드 전용: num_blocks * md_len. accel copy의 메타 단계 크기로 사용.
 */
static size_t
malloc_get_md_len(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->u.bdev.num_blocks * bdev_io->bdev->md_len;
}

/*
 * [한국어]
 * malloc_get_md_offset - 메타 영역 안에서의 시작 오프셋 계산 (바이트)
 *
 * 분리 메타 모드 전용: offset_blocks * md_len. malloc_md_buf 기준 오프셋.
 */
static uint64_t
malloc_get_md_offset(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->u.bdev.offset_blocks * bdev_io->bdev->md_len;
}

/*
 * [한국어]
 * malloc_get_md_buf - 분리 메타 영역에서 본 I/O가 차지하는 시작 주소
 *
 * mdisk->malloc_md_buf + (offset_blocks * md_len). 분리 메타 모드일 때만 호출되며,
 * 아닌 경우 assert로 잡힘. SPDK_CONTAINEROF로 bdev → malloc_disk 역참조.
 */
static void *
malloc_get_md_buf(struct spdk_bdev_io *bdev_io)
{
	struct malloc_disk *mdisk = SPDK_CONTAINEROF(bdev_io->bdev, struct malloc_disk, disk);
	/* [한국어] disk 필드가 임베드되어 있으므로 컨테이너 매크로로 malloc_disk 포인터 복원. */

	assert(spdk_bdev_is_md_separate(bdev_io->bdev));
	/* [한국어] 인터리브 모드라면 malloc_md_buf는 NULL이라 사용 불가 — 호출자가 분기 보장해야 함. */

	return (char *)mdisk->malloc_md_buf + malloc_get_md_offset(bdev_io);
	/* [한국어] char* 캐스팅 후 산술 — 메타 영역 시작에 LBA-비례 오프셋을 더한 결과. */
}

/*
 * [한국어]
 * malloc_sequence_fail - accel sequence 빌딩 단계에서 실패 시 처리
 *
 * @task:   진행 중이던 task.
 * @status: 실패 코드. -ENOMEM이면 sequence를 abort하지 않고 그대로 두어 bdev layer가 retry.
 *
 * spdk_accel_append_copy 등이 sequence 빌드 도중 실패한 경우 사용. ENOMEM 외의 에러는
 * 시퀀스를 폐기해야 누수가 없으며, ENOMEM은 bdev layer가 같은 sequence 포인터로
 * 재시도하기 때문에 abort 금지.
 */
static void
malloc_sequence_fail(struct malloc_task *task, int status)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(task);
	/* [한국어] task → bdev_io 역참조. accel_sequence 필드 정리를 위해 필요. */

	/* For ENOMEM, the IO will be retried by the bdev layer, so we don't abort the sequence */
	if (status != -ENOMEM) {
		/* [한국어] 영구 실패: 빌드된 sequence를 abort해 내부 step/resource 회수. */
		spdk_accel_sequence_abort(bdev_io->u.bdev.accel_sequence);
		bdev_io->u.bdev.accel_sequence = NULL;
		/* [한국어] bdev_io.accel_sequence를 비워야 상위 계층의 후속 호출이 재진입을 안 한다. */
	}

	malloc_done(task, status);
	/* [한국어] 공통 종결 함수에 위임 — num_outstanding 미증가 상태에서도 안전(첫 호출이 0으로 떨어짐). */
}

/*
 * [한국어]
 * malloc_sequence_done - accel sequence 전체 완료 콜백
 *
 * @ctx:    task 포인터.
 * @status: sequence 실행 결과. 0=성공, 음수=실패.
 *
 * spdk_accel_sequence_finish 호출 시 등록되는 종결자. sequence 실행이 끝나면(혹은 ENOMEM
 * 등으로 중단되면) 호출된다. ENOMEM은 bdev layer가 retry하지 않도록 -EFAULT로 변환 —
 * sequence는 이미 일부 step이 실행되었을 수 있으므로 안전한 retry가 불가능.
 */
static void
malloc_sequence_done(void *ctx, int status)
{
	struct malloc_task *task = ctx;
	/* [한국어] 등록 시 finish의 두 번째 인자로 task를 넘겼음. */
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(task);

	bdev_io->u.bdev.accel_sequence = NULL;
	/* [한국어] 시퀀스가 소비되었으므로 reset — malloc_done의 assert에서 검증되는 invariant. */
	/* Prevent bdev layer from retrying the request if the sequence failed with ENOMEM */
	malloc_done(task, status != -ENOMEM ? status : -EFAULT);
	/* [한국어] ENOMEM은 강제 EFAULT 변환: NOMEM 격상으로 인한 bdev layer retry를 차단해
	 * 절반 실행된 sequence가 두 번 실행되는 위험을 회피. */
}

/*
 * [한국어]
 * bdev_malloc_readv - READ 처리: hugepage → 사용자 iovec로 데이터(+분리 메타) 복사
 *
 * @mdisk:   대상 디스크 (malloc_buf/md_buf 보유).
 * @ch:      accel framework io_channel — accel HW/SW 엔진 액세스 핸들.
 * @task:    이번 I/O의 driver_ctx. num_outstanding/iov를 채워서 사용.
 * @bdev_io: 사용자 요청 객체.
 *
 * 동작:
 *   1) iovec 길이 sanity check — 합이 num_blocks*blocklen과 정확히 일치해야 함.
 *   2) 데이터 복사를 accel sequence에 append_copy로 추가:
 *      - 소스: malloc_buf+offset (NULL domain — 물리 직접 접근 가능).
 *      - 대상: 사용자 iovec (memory_domain이 있을 수 있음 — 예: GPU/SmartNIC 메모리).
 *   3) sequence_reverse: append 순서가 역순이라 사용자 buffer로 정방향 흐름 만들기 위해 reverse.
 *   4) sequence_finish로 실행 시작, 완료 시 malloc_sequence_done이 호출됨.
 *   5) 분리 메타 모드(md_buf != NULL)면 추가 메타 복사를 submit_copy로 발행. 메타는 NULL 도메인.
 *
 * 호출 체인:
 *   bdev_malloc_submit_request → _bdev_malloc_submit_request → bdev_malloc_readv
 *      → spdk_accel_append_copy → spdk_accel_sequence_finish
 *      → (HW DSA/IOAT or SW memcpy) → malloc_sequence_done → malloc_done → 사용자 cb
 */
static void
bdev_malloc_readv(struct malloc_disk *mdisk, struct spdk_io_channel *ch,
		  struct malloc_task *task, struct spdk_bdev_io *bdev_io)
{
	uint64_t len, offset;
	int res = 0;

	len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
	/* [한국어] 전송할 데이터 총 바이트 = 블록수 × (데이터+옵션메타 in 인터리브) 블록 크기. */
	offset = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;
	/* [한국어] hugepage 시작에서의 바이트 오프셋 = LBA × 블록 크기. */

	if (bdev_malloc_check_iov_len(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, len)) {
		/* [한국어] iovec 합 != len이면 사용자 버퍼가 맞지 않음 — 즉시 FAILED로 종결. */
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task),
				      SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	/* [한국어] 누적 상태 초기값 — 이후 malloc_done에서 sticky하게 갱신될 수 있음. */
	task->num_outstanding = 0;
	/* [한국어] 다음 단계에서 ++로 1 또는 2가 됨. 각 accel 호출이 콜백에서 -- 한다. */
	task->iov.iov_base = mdisk->malloc_buf + offset;
	/* [한국어] 소스 표현(매체 측). malloc_buf의 LBA 위치를 가리킴. */
	task->iov.iov_len = len;

	SPDK_DEBUGLOG(bdev_malloc, "read %zu bytes from offset %#" PRIx64 ", iovcnt=%d\n",
		      len, offset, bdev_io->u.bdev.iovcnt);
	/* [한국어] bdev_malloc 컴포넌트 활성 시에만 출력 — 핫패스 영향 최소화. */

	task->num_outstanding++;
	/* [한국어] 데이터 단계 제출 전 outstanding 증가. */
	res = spdk_accel_append_copy(&bdev_io->u.bdev.accel_sequence, ch,
				     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				     bdev_io->u.bdev.memory_domain,
				     bdev_io->u.bdev.memory_domain_ctx,
				     &task->iov, 1, NULL, NULL, NULL, NULL);
	/* [한국어] accel sequence에 copy step 추가:
	 *   dst = 사용자 iovec (memory_domain 가능 — 외부 메모리 도메인 변환은 accel framework 책임)
	 *   src = task->iov 단일 entry (malloc_buf+offset, NULL domain = 일반 RAM)
	 *   sequence는 in/out 파라미터 — 처음이면 NULL이 들어와 새로 할당, 이후엔 누적. */
	if (spdk_unlikely(res != 0)) {
		/* [한국어] sequence 빌드 실패(보통 ENOMEM) — abort 또는 retry 결정은 sequence_fail이 담당. */
		malloc_sequence_fail(task, res);
		return;
	}

	spdk_accel_sequence_reverse(bdev_io->u.bdev.accel_sequence);
	/* [한국어] append는 끝에 붙이는 방식이라 빌드된 순서는 (마지막 추가 → 처음 추가)의 역순.
	 * READ의 경우 누적된 다른 변환 step(예: crypto/DIF) 후 마지막에 복사가 와야 하므로 reverse. */
	spdk_accel_sequence_finish(bdev_io->u.bdev.accel_sequence, malloc_sequence_done, task);
	/* [한국어] 시퀀스 실행 트리거 — accel HW에 작업 큐잉. 완료 시 sequence_done 호출. */

	if (bdev_io->u.bdev.md_buf == NULL) {
		/* [한국어] 인터리브 모드 또는 사용자가 메타 요청 안 한 경우 — 메타 단계 skip. */
		return;
	}

	SPDK_DEBUGLOG(bdev_malloc, "read metadata %zu bytes from offset%#" PRIx64 "\n",
		      malloc_get_md_len(bdev_io), malloc_get_md_offset(bdev_io));

	task->num_outstanding++;
	/* [한국어] 메타 단계 제출 전 outstanding 추가 증가 → 데이터+메타 둘 다 끝나야 complete. */
	res = spdk_accel_submit_copy(ch, bdev_io->u.bdev.md_buf, malloc_get_md_buf(bdev_io),
				     malloc_get_md_len(bdev_io), malloc_done, task);
	/* [한국어] 메타는 sequence를 새로 만들지 않고 단순 submit_copy(즉시 1 step 발행) 사용.
	 * dst=사용자 md_buf, src=mdisk->malloc_md_buf+offset. 완료 시 malloc_done 호출. */
	if (res != 0) {
		/* [한국어] 동기 실패 케이스 — 콜백이 호출되지 않으므로 직접 malloc_done 호출해 정리. */
		malloc_done(task, res);
	}
}

/*
 * [한국어]
 * bdev_malloc_writev - WRITE 처리: 사용자 iovec → hugepage로 데이터(+분리 메타) 복사
 *
 * @mdisk:   대상 디스크.
 * @ch:      accel io_channel.
 * @task:    driver_ctx.
 * @bdev_io: 사용자 요청.
 *
 * READ와 대칭. 다른 점:
 *   - src/dst 방향 반대 (사용자 → hugepage).
 *   - WRITE의 경우 sequence_reverse 불필요 — 누적 step의 마지막에 copy가 와야 하므로
 *     append 순서 그대로 사용. (READ는 마지막 step 결과를 사용자에게 넘기는 흐름이라 reverse.)
 *
 * 호출 체인은 readv와 동일.
 */
static void
bdev_malloc_writev(struct malloc_disk *mdisk, struct spdk_io_channel *ch,
		   struct malloc_task *task, struct spdk_bdev_io *bdev_io)
{
	uint64_t len, offset;
	int res = 0;

	len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
	/* [한국어] 전송 바이트. */
	offset = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;
	/* [한국어] 매체 측 시작 오프셋. */

	if (bdev_malloc_check_iov_len(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, len)) {
		/* [한국어] iovec 길이 sanity 실패 시 즉시 FAILED. */
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task),
				      SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = 0;
	task->iov.iov_base = mdisk->malloc_buf + offset;
	/* [한국어] WRITE에서는 task->iov가 dst — 매체 측 위치. */
	task->iov.iov_len = len;

	SPDK_DEBUGLOG(bdev_malloc, "write %zu bytes to offset %#" PRIx64 ", iovcnt=%d\n",
		      len, offset, bdev_io->u.bdev.iovcnt);

	task->num_outstanding++;
	res = spdk_accel_append_copy(&bdev_io->u.bdev.accel_sequence, ch, &task->iov, 1, NULL, NULL,
				     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				     bdev_io->u.bdev.memory_domain,
				     bdev_io->u.bdev.memory_domain_ctx, NULL, NULL);
	/* [한국어] copy step 추가:
	 *   dst = task->iov (malloc_buf+offset, NULL domain = 일반 RAM)
	 *   src = 사용자 iovec (외부 memory_domain 가능)
	 *  READ와 위치가 정반대임에 주의. */
	if (spdk_unlikely(res != 0)) {
		malloc_sequence_fail(task, res);
		return;
	}

	spdk_accel_sequence_finish(bdev_io->u.bdev.accel_sequence, malloc_sequence_done, task);
	/* [한국어] WRITE는 reverse 불필요 — append 순서가 곧 실행 순서.
	 * 누적 변환(예: 사용자 측 crypto/DIF gen)이 있어도 그 결과를 마지막에 매체에 쓰면 되므로. */

	if (bdev_io->u.bdev.md_buf == NULL) {
		/* [한국어] 분리 메타가 없으면 메타 단계 skip. */
		return;
	}

	SPDK_DEBUGLOG(bdev_malloc, "write metadata %zu bytes to offset %#" PRIx64 "\n",
		      malloc_get_md_len(bdev_io), malloc_get_md_offset(bdev_io));

	task->num_outstanding++;
	res = spdk_accel_submit_copy(ch, malloc_get_md_buf(bdev_io), bdev_io->u.bdev.md_buf,
				     malloc_get_md_len(bdev_io), malloc_done, task);
	/* [한국어] 메타 복사: dst=mdisk->malloc_md_buf+offset, src=사용자 md_buf.
	 * 데이터 단계와 별개의 작업이므로 sequence가 아닌 submit_copy 사용. */
	if (res != 0) {
		/* [한국어] 동기 실패 — 콜백 미호출 → 직접 정리. */
		malloc_done(task, res);
	}
}

/*
 * [한국어]
 * bdev_malloc_unmap - UNMAP / WRITE_ZEROES 처리: hugepage 영역을 0으로 채움
 *
 * @mdisk:      대상 디스크.
 * @ch:         accel io_channel.
 * @task:       driver_ctx.
 * @offset:     매체 시작 오프셋 (바이트).
 * @byte_count: 0으로 채울 바이트 수.
 *
 * 실 매체(NVMe SSD)에서는 UNMAP과 WRITE_ZEROES가 의미가 다르지만, Malloc bdev에서는
 * 데이터를 0으로 만드는 것 외 의미가 없다(저장 매체가 RAM이므로 trim/dealloc 불가).
 * spdk_accel_submit_fill은 accel HW가 있으면 그를 통해 더 빠르게 0-fill 수행.
 *
 * 호출 체인:
 *   _bdev_malloc_submit_request (UNMAP/WRITE_ZEROES) → bdev_malloc_unmap
 *      → spdk_accel_submit_fill → (HW/SW fill) → malloc_done → 사용자 cb
 */
static int
bdev_malloc_unmap(struct malloc_disk *mdisk,
		  struct spdk_io_channel *ch,
		  struct malloc_task *task,
		  uint64_t offset,
		  uint64_t byte_count)
{
	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	/* [한국어] 누적 상태 초기값. */
	task->num_outstanding = 1;
	/* [한국어] fill 1회만 발행 — 메타 별도 처리는 후속 PI 재생성 단계에서. */

	return spdk_accel_submit_fill(ch, mdisk->malloc_buf + offset, 0,
				      byte_count, malloc_done, task);
	/* [한국어] dst=매체 영역, value=0, len=byte_count. 완료 시 malloc_done에서 DIF면
	 * malloc_unmap_write_zeroes_generate_pi가 PI 태그를 재구성. */
}

/*
 * [한국어]
 * bdev_malloc_copy - COPY 처리: 같은 디스크 내에서 src LBA → dst LBA 복사
 *
 * @mdisk:      대상 디스크 (src/dst 모두 이 디스크).
 * @ch:         accel io_channel.
 * @task:       driver_ctx.
 * @dst_offset: 대상 매체 오프셋(바이트).
 * @src_offset: 원본 매체 오프셋(바이트).
 * @len:        복사 바이트 수.
 *
 * SPDK_BDEV_IO_TYPE_COPY 처리. 단일 호출로 spdk_accel_submit_copy 발행 — accel HW가 있으면
 * 메모리 간 복사를 오프로드해 호스트 CPU 부담을 줄임(주로 NVMe Simple Copy 시뮬레이션 용도).
 * src/dst 영역 overlap의 결과는 spec상 정의되지 않으므로 호출자 책임.
 */
static void
bdev_malloc_copy(struct malloc_disk *mdisk, struct spdk_io_channel *ch,
		 struct malloc_task *task,
		 uint64_t dst_offset, uint64_t src_offset, size_t len)
{
	int64_t res = 0;
	void *dst = mdisk->malloc_buf + dst_offset;
	/* [한국어] 같은 hugepage 영역 내 dst 절대 주소. */
	void *src = mdisk->malloc_buf + src_offset;
	/* [한국어] 같은 hugepage 영역 내 src 절대 주소. */

	SPDK_DEBUGLOG(bdev_malloc, "Copy %zu bytes from offset %#" PRIx64 " to offset %#" PRIx64 "\n",
		      len, src_offset, dst_offset);

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = 1;
	/* [한국어] copy 1회만 발행. */

	res = spdk_accel_submit_copy(ch, dst, src, len, malloc_done, task);
	/* [한국어] 양쪽 모두 일반 RAM(NULL memory_domain). 완료 시 malloc_done 호출. */
	if (res != 0) {
		/* [한국어] 동기 실패(예: ENOMEM) — 콜백 미호출이므로 직접 정리. */
		malloc_done(task, res);
	}
}

/*
 * [한국어]
 * _bdev_malloc_submit_request - I/O 타입별 디스패치 (내부 구현체)
 *
 * @mch:     채널 컨텍스트(accel_channel, 완료 큐).
 * @bdev_io: 사용자 요청.
 * @return: 0=처리 시작/완료 큐잉 성공, -1=지원하지 않는 I/O 타입.
 *
 * bdev_io->type을 보고 적절한 처리 루틴으로 분기. READ/WRITE는 accel sequence로 처리,
 * RESET/FLUSH/ZCOPY/ABORT는 즉시 완료 큐로 enqueue. DIF 활성 시 적절한 PI 사전 검증.
 *
 * 실행 컨텍스트: 채널 reactor 스레드. 모든 자료구조는 채널 로컬 → 락 없음.
 *
 * 호출 체인:
 *   bdev core → fn_table.submit_request (bdev_malloc_submit_request)
 *      → _bdev_malloc_submit_request → 타입별 helper
 */
static int
_bdev_malloc_submit_request(struct malloc_channel *mch, struct spdk_bdev_io *bdev_io)
{
	struct malloc_task *task = (struct malloc_task *)bdev_io->driver_ctx;
	/* [한국어] bdev_io 끝에 임베드된 driver_ctx → malloc_task. */
	struct malloc_disk *disk = bdev_io->bdev->ctxt;
	/* [한국어] bdev → 모듈 컨텍스트 역참조. ctxt에 mdisk self-pointer를 넣어 둠. */
	uint32_t block_size = bdev_io->bdev->blocklen;
	/* [한국어] 인터리브 모드면 데이터+메타 합쳐진 크기. 분리 모드면 데이터만. */
	int rc;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.iovs[0].iov_base == NULL) {
			/* [한국어] "Buffered I/O 모드" — 사용자가 자신의 버퍼를 주지 않고 매체 측 버퍼를
			 * 그대로 빌려 쓰는 패턴(zero-copy READ). NULL이면 bdev core가 채워 달라 의미. */
			assert(bdev_io->u.bdev.iovcnt == 1);
			/* [한국어] zero-copy 모드는 단일 iovec만 가능. */
			assert(bdev_io->u.bdev.memory_domain == NULL);
			/* [한국어] 외부 메모리 도메인일 경우 직접 포인터 노출 불가 — accel 변환 필요. */
			bdev_io->u.bdev.iovs[0].iov_base =
				disk->malloc_buf + bdev_io->u.bdev.offset_blocks * block_size;
			/* [한국어] 매체 측 RAM 영역 시작 주소를 그대로 사용자 iovec에 박아 넘김(zero-copy). */
			bdev_io->u.bdev.iovs[0].iov_len = bdev_io->u.bdev.num_blocks * block_size;
			if (spdk_bdev_is_md_separate(bdev_io->bdev)) {
				/* [한국어] 분리 메타 모드면 메타 영역도 zero-copy로 노출. */
				spdk_bdev_io_set_md_buf(bdev_io, malloc_get_md_buf(bdev_io),
							malloc_get_md_len(bdev_io));
			}
			malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_SUCCESS);
			/* [한국어] 카피 없이 즉시 완료 — 다음 tick에 user cb 호출. */
			return 0;
		}

		if (bdev_io->bdev->dif_type != SPDK_DIF_DISABLE &&
		    spdk_bdev_io_hide_metadata(bdev_io)) {
			/* [한국어] hide_metadata READ + DIF: 매체에 있는 데이터의 PI를 먼저 검증해
			 * 깨진 데이터를 사용자에게 노출하지 않도록 함. (정상 모드는 read 끝난 뒤 검증.) */
			rc = malloc_verify_pi_malloc_buf(bdev_io);
			if (rc != 0) {
				malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_FAILED);
				/* [한국어] PI 검증 실패 → 즉시 FAILED 큐잉. */
				return 0;
			}
		}

		bdev_malloc_readv(disk, mch->accel_channel, task, bdev_io);
		/* [한국어] 정상 READ 경로 진입. */
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		if (bdev_io->bdev->dif_type != SPDK_DIF_DISABLE &&
		    !spdk_bdev_io_hide_metadata(bdev_io)) {
			/* [한국어] 사용자가 PI를 포함한 데이터를 보내는 모드 → 매체에 쓰기 전에 검증해서
			 * 깨진 데이터가 디스크에 남지 않도록 함. */
			rc = malloc_verify_pi_io_buf(bdev_io);
			if (rc != 0) {
				malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_FAILED);
				return 0;
			}
		}

		bdev_malloc_writev(disk, mch->accel_channel, task, bdev_io);
		/* [한국어] 정상 WRITE 경로 진입. */
		return 0;

	case SPDK_BDEV_IO_TYPE_RESET:
		/* [한국어] Malloc은 상태가 없는 RAM이라 reset은 즉시 성공으로 처리. */
		malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;

	case SPDK_BDEV_IO_TYPE_FLUSH:
		/* [한국어] write-back 캐시가 없으므로 flush도 즉시 성공. */
		malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		/* [한국어] UNMAP: 해당 영역을 0으로 채움(NVMe Deallocate의 read-back 0과 같은 효과). */
		return bdev_malloc_unmap(disk, mch->accel_channel, task,
					 bdev_io->u.bdev.offset_blocks * block_size,
					 bdev_io->u.bdev.num_blocks * block_size);

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		/* bdev_malloc_unmap is implemented with a call to mem_cpy_fill which zeroes out all of the requested bytes. */
		/* [한국어] WRITE_ZEROES도 UNMAP와 같은 0-fill로 처리(RAM 매체에서는 의미 동일). */
		return bdev_malloc_unmap(disk, mch->accel_channel, task,
					 bdev_io->u.bdev.offset_blocks * block_size,
					 bdev_io->u.bdev.num_blocks * block_size);

	case SPDK_BDEV_IO_TYPE_ZCOPY:
		if (bdev_io->u.bdev.zcopy.start) {
			/* [한국어] zcopy.start=true: 사용자에게 매체 영역의 가상 주소를 직접 노출하는 단계.
			 *   I/O 흐름: user calls bdev_zcopy_start → 모듈이 buf 포인터 전달 → user 직접 read/write
			 *           → user calls bdev_zcopy_end → 모듈은 commit/abort 처리. */
			void *buf;
			size_t len;

			buf = disk->malloc_buf + bdev_io->u.bdev.offset_blocks * block_size;
			/* [한국어] 노출할 매체 측 RAM 시작 주소. */
			len = bdev_io->u.bdev.num_blocks * block_size;
			spdk_bdev_io_set_buf(bdev_io, buf, len);
			/* [한국어] bdev_io에 buf/len 설정 — bdev core가 사용자 콜백에서 이 buf를 전달. */
			if (spdk_bdev_is_md_separate(bdev_io->bdev)) {
				/* [한국어] 분리 메타도 같이 노출. */
				spdk_bdev_io_set_md_buf(bdev_io, malloc_get_md_buf(bdev_io),
							malloc_get_md_len(bdev_io));
			}
		}
		/* [한국어] zcopy.start=false(=end)이면 매체에 직접 쓴 결과를 그대로 두면 끝 — 추가 작업 없음. */
		malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	case SPDK_BDEV_IO_TYPE_ABORT:
		/* [한국어] 본 모듈은 abort 미지원 — 항상 FAILED 통보(bdev core가 retry 안 함). */
		malloc_complete_task(task, mch, SPDK_BDEV_IO_STATUS_FAILED);
		return 0;
	case SPDK_BDEV_IO_TYPE_COPY:
		/* [한국어] 같은 디스크 내 영역 복사 — accel HW가 메모리 간 카피를 오프로드. */
		bdev_malloc_copy(disk, mch->accel_channel, task,
				 bdev_io->u.bdev.offset_blocks * block_size,
				 bdev_io->u.bdev.copy.src_offset_blocks * block_size,
				 bdev_io->u.bdev.num_blocks * block_size);
		return 0;

	default:
		/* [한국어] io_type_supported가 true를 반환한 타입만 와야 정상. -1 = 상위 wrapper에서 FAILED 큐잉. */
		return -1;
	}
	return 0;
}

/*
 * [한국어]
 * bdev_malloc_submit_request - fn_table.submit_request 콜백 (외부 진입점)
 *
 * @ch:      이 채널의 spdk_io_channel.
 * @bdev_io: 사용자 요청.
 *
 * bdev core가 I/O를 모듈로 dispatch할 때 호출. spdk_io_channel_get_ctx로 채널 컨텍스트를
 * 꺼낸 뒤 _bdev_malloc_submit_request로 위임. 지원되지 않는 I/O 타입이면 FAILED로 큐잉.
 *
 * 실행 컨텍스트: 채널이 attach된 reactor 스레드.
 */
static void
bdev_malloc_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct malloc_channel *mch = spdk_io_channel_get_ctx(ch);
	/* [한국어] io_channel 뒤에 임베드된 malloc_channel 컨텍스트 추출 — accel_channel/완료 큐 접근. */

	if (_bdev_malloc_submit_request(mch, bdev_io) != 0) {
		/* [한국어] 지원 안 하는 타입 등 — FAILED로 큐잉해 다음 tick에 사용자 cb 호출. */
		malloc_complete_task((struct malloc_task *)bdev_io->driver_ctx, mch,
				     SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * [한국어]
 * bdev_malloc_io_type_supported - fn_table.io_type_supported 콜백
 *
 * bdev core가 사용자에게 노출할 수 있는 I/O 타입을 결정할 때 호출. Malloc은 READ/WRITE 외
 * FLUSH/RESET/UNMAP/WRITE_ZEROES/ZCOPY/ABORT/COPY를 지원(_bdev_malloc_submit_request의
 * switch와 정확히 동기되어야 함).
 */
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
		/* [한국어] 위 모든 타입은 _bdev_malloc_submit_request의 case에 매핑됨. */
		return true;

	default:
		/* [한국어] 그 외 타입(예: NVME_ADMIN, NVME_IO 같은 패스스루)은 미지원. */
		return false;
	}
}

/*
 * [한국어]
 * bdev_malloc_get_io_channel - fn_table.get_io_channel 콜백
 *
 * 채널 키로 g_malloc_disks 주소를 사용 — 모든 Malloc bdev가 같은 io_device를 공유하므로
 * 채널도 공유. spdk_get_io_channel가 thread-local 채널 풀에서 채널을 찾거나 새로 생성.
 */
static struct spdk_io_channel *
bdev_malloc_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_malloc_disks);
	/* [한국어] 채널 키 = g_malloc_disks의 주소. 모든 Malloc bdev가 단일 io_device를 공유. */
}

/*
 * [한국어]
 * bdev_malloc_write_json_config - fn_table.write_config_json 콜백
 *
 * SPDK config dump 시 호출되어 이 bdev를 재현할 RPC 호출의 JSON 표현을 출력. 결과는
 * bdev_malloc_create method를 부르는 JSON object — 그대로 spdk_tgt에 fed해 동일 상태 복원.
 */
static void
bdev_malloc_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	/* [한국어] 최상위 JSON object 시작. */

	spdk_json_write_named_string(w, "method", "bdev_malloc_create");
	/* [한국어] 재현할 RPC method 이름 — bdev_malloc_rpc.c에 등록된 핸들러 이름과 일치. */

	spdk_json_write_named_object_begin(w, "params");
	/* [한국어] RPC 파라미터 object 시작. */
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
	/* [한국어] 위 필드들이 모두 malloc_bdev_opts에 대응. 누락 시 재현 불가능한 차이 발생. */

	spdk_json_write_object_end(w);
	/* [한국어] params object 끝. */

	spdk_json_write_object_end(w);
	/* [한국어] 최상위 object 끝. */
}

/*
 * [한국어]
 * bdev_malloc_get_memory_domains - fn_table.get_memory_domains 콜백
 *
 * @ctx:        mdisk.
 * @domains:    출력 배열(NULL이면 카운트만 반환).
 * @array_size: domains 배열 크기.
 * @return: 지원하는 메모리 도메인 총 개수.
 *
 * 본 모듈이 직접 다룰 수 있는 외부 메모리 도메인(예: GPU/SmartNIC 메모리)을 알리는 콜백.
 * Malloc은 hugepage RAM이 백엔드라 accel framework이 변환만 해주면 사실상 모든 도메인을
 * 지원할 수 있으므로 시스템에 등록된 모든 도메인을 반환. 단, DIF 활성화 시에는 CPU 직접
 * 검증이 필요하므로 외부 도메인 지원 불가 → 0 반환.
 */
static int
bdev_malloc_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct malloc_disk *malloc_disk = ctx;
	struct spdk_memory_domain *domain;
	int num_domains = 0;

	if (malloc_disk->disk.dif_type != SPDK_DIF_DISABLE) {
		/* [한국어] DIF 활성화 → PI 검증 시 데이터를 CPU가 직접 읽어야 함 → 외부 메모리 도메인 불가.
		 * 0 반환 = 어떤 도메인도 지원 안 함 (사용자 버퍼는 일반 RAM이어야 함). */
		return 0;
	}

	/* Report support for every memory domain */
	for (domain = spdk_memory_domain_get_first(NULL); domain != NULL;
	     domain = spdk_memory_domain_get_next(domain, NULL)) {
		/* [한국어] dma.h가 제공하는 도메인 iterator로 시스템 전체 순회. NULL=type 필터 없음. */
		if (domains != NULL && num_domains < array_size) {
			/* [한국어] 호출자가 카운트만 원하면(NULL) 채우지 않고 num_domains만 누적. */
			domains[num_domains] = domain;
		}
		num_domains++;
	}

	return num_domains;
	/* [한국어] 전체 도메인 수 반환. 호출자가 두 번 호출(첫 호출은 카운트, 두 번째는 배열 채우기)할 수 있음. */
}

/*
 * [한국어]
 * bdev_malloc_accel_sequence_supported - fn_table.accel_sequence_supported 콜백
 *
 * accel sequence는 상위 계층(예: bdev_crypto, bdev_dif)이 변환 step을 누적해 모듈에
 * 전달하는 기법. Malloc은 READ/WRITE만 sequence와 같이 처리할 수 있고(데이터 copy step
 * 하나를 sequence 끝/시작에 append), 그 외 타입(UNMAP/COPY 등)은 sequence 결합 불가.
 */
static bool
bdev_malloc_accel_sequence_supported(void *ctx, enum spdk_bdev_io_type type)
{
	switch (type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* [한국어] readv/writev가 spdk_accel_append_copy로 sequence 끝에 step 추가하도록 구현됨. */
		return true;
	default:
		return false;
	}
}

static const struct spdk_bdev_fn_table malloc_fn_table = {
	/* [한국어] Malloc bdev가 bdev core에 제공하는 함수 테이블 — 각 bdev 인스턴스의
	 * disk.fn_table로 등록되어 bdev core가 콜백으로 호출. */
	.destruct			= bdev_malloc_destruct,
	/* [한국어] unregister 시 호출 — 인스턴스 자원 해제. */
	.submit_request			= bdev_malloc_submit_request,
	/* [한국어] I/O 제출 콜백 — 핫패스 진입점. */
	.io_type_supported		= bdev_malloc_io_type_supported,
	/* [한국어] 지원 I/O 타입 광고. */
	.get_io_channel			= bdev_malloc_get_io_channel,
	/* [한국어] 채널 획득 — bdev core가 spdk_bdev_get_io_channel 호출 시 위임. */
	.write_config_json		= bdev_malloc_write_json_config,
	/* [한국어] 구성 직렬화. */
	.get_memory_domains		= bdev_malloc_get_memory_domains,
	/* [한국어] 지원 메모리 도메인 광고. */
	.accel_sequence_supported	= bdev_malloc_accel_sequence_supported,
	/* [한국어] accel sequence 지원 여부 광고. */
};

/*
 * [한국어]
 * malloc_disk_setup_pi - 디스크 생성 직후 hugepage 전체에 초기 PI 태그를 채워두는 함수
 *
 * @mdisk: 새로 만든 디스크.
 * @return: 0=OK, 음수=실패.
 *
 * DIF가 활성화된 디스크는 모든 블록에 합법적 PI(guard/reftag/apptag)가 있어야 read에서
 * 검증을 통과한다. spdk_zmalloc로 받은 영역은 모두 0이라 PI 검사를 통과 못 하므로 생성
 * 직후 전체 디스크에 대해 spdk_dif_generate(인터리브) 또는 spdk_dix_generate(분리)를
 * 한 번 돌려 초기 태그를 박아 둔다. APPTAG/REFTAG는 IGNORE 플래그로 무시.
 *
 * 실행 컨텍스트: create_malloc_disk(RPC 스레드 동기 호출). 한 번만 실행.
 */
static int
malloc_disk_setup_pi(struct malloc_disk *mdisk)
{
	struct spdk_bdev *bdev = &mdisk->disk;
	/* [한국어] 임베드된 bdev 메타. blocklen/md_len 등 PI 컨텍스트 초기화에 필요. */
	struct spdk_dif_ctx dif_ctx;
	struct iovec iov, md_iov;
	uint32_t dif_check_flags;
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	/* [한국어] ABI 안정성을 위한 sentinel 크기 — dif_pi_format까지 포함된 길이. */
	dif_opts.dif_pi_format = bdev->dif_pi_format;
	/* [한국어] PI 포맷 (16비트 CRC, 32비트 CRC, 64비트 CRC 등 NVMe 2.0 PI format). */
	/* Set APPTAG|REFTAG_IGNORE to PI fields after creation of malloc bdev */
	dif_check_flags = bdev->dif_check_flags | SPDK_DIF_CHECK_TYPE_REFTAG |
			  SPDK_DIF_FLAGS_APPTAG_CHECK;
	/* [한국어] 초기 생성 단계에서는 REFTAG/APPTAG 모두 IGNORE로 채워 둠 → 사용자 검증 단계에서는
	 * guard만 검사하면 통과. flag에 CHECK 비트를 켜는 것은 dif_ctx_init이 정상 동작하기 위함. */
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
	/* [한국어] PI 컨텍스트 초기화. REFTAG 값은 IGNORE 센티넬, APPTAG 값도 IGNORE. */
	if (rc != 0) {
		SPDK_ERRLOG("Initialization of DIF/DIX context failed\n");
		return rc;
	}

	iov.iov_base = mdisk->malloc_buf;
	iov.iov_len = bdev->blockcnt * bdev->blocklen;
	/* [한국어] 디스크 전체 영역을 단일 iovec로 표현 — 한 번에 통째로 PI 생성. */

	if (mdisk->disk.md_interleave) {
		/* [한국어] 인터리브 모드: 각 블록 끝에 PI가 박힘. dif_generate가 in-place 갱신. */
		rc = spdk_dif_generate(&iov, 1, bdev->blockcnt, &dif_ctx);
	} else {
		/* [한국어] 분리 모드(DIX): 데이터/메타 두 영역을 따로 입력. */
		md_iov.iov_base = mdisk->malloc_md_buf;
		md_iov.iov_len = bdev->blockcnt * bdev->md_len;

		rc = spdk_dix_generate(&iov, 1, &md_iov, bdev->blockcnt, &dif_ctx);
	}

	if (rc != 0) {
		SPDK_ERRLOG("Formatting by DIF/DIX failed\n");
	}

	return rc;
	/* [한국어] 호출자(create_malloc_disk)가 실패 시 디스크 자원 롤백. */
}

/*
 * [한국어]
 * create_malloc_disk - Malloc bdev 인스턴스 생성 (RPC 핸들러가 호출하는 핵심 함수)
 *
 * @bdev: [out] 생성된 spdk_bdev 포인터를 채울 곳.
 * @opts: 사용자 옵션 (num_blocks, block_size, md_size, dif_type, numa_id 등).
 * @return: 0=성공, 음수=실패(-EINVAL/-ENOMEM 등). 실패 시 *bdev는 미정의.
 *
 * 흐름:
 *   1) 옵션 검증 (블록 수>0, 512 align, NUMA 존재성, md_size 허용 값).
 *   2) 인터리브 모드면 block_size에 md_size 합산.
 *   3) malloc_disk 구조체 calloc.
 *   4) spdk_zmalloc(SPDK_MALLOC_DMA, 2MiB align)로 hugepage 데이터 영역 확보.
 *   5) 분리 메타 모드면 메타 영역도 별도 확보.
 *   6) 이름(자동/사용자 지정) 설정, product_name/blocklen/md_len 등 disk 필드 채움.
 *   7) DIF 활성 시 malloc_disk_setup_pi로 초기 PI 태그 채움.
 *   8) optimal_io_boundary, uuid, numa_id 옵션 반영.
 *   9) fn_table/module/ctxt 셋업 후 spdk_bdev_register로 bdev core에 등록.
 *  10) g_malloc_disks 리스트에 추가.
 *
 * 실패 시 각 단계마다 malloc_disk_free로 안전 롤백 — calloc 직후라 NULL 필드는 spdk_free
 * no-op으로 처리되어 누수 없음.
 *
 * 실행 컨텍스트: RPC 스레드(주로 메인 reactor). spdk_bdev_register는 thread-safe.
 *
 * 호출 체인: bdev_malloc_rpc.c::rpc_bdev_malloc_create → create_malloc_disk → spdk_bdev_register
 */
int
create_malloc_disk(struct spdk_bdev **bdev, const struct malloc_bdev_opts *opts)
{
	struct malloc_disk *mdisk;
	uint32_t block_size;
	int32_t numa_id;
	int rc;

	assert(opts != NULL);

	if (opts->num_blocks == 0) {
		/* [한국어] 0 블록 디스크는 무의미 — 즉시 EINVAL. */
		SPDK_ERRLOG("Disk num_blocks must be greater than 0");
		return -EINVAL;
	}

	if (opts->block_size % 512) {
		/* [한국어] NVMe/SCSI 호환을 위해 블록 크기는 512배수 강제. */
		SPDK_ERRLOG("Data block size must be 512 bytes aligned\n");
		return -EINVAL;
	}

	if (opts->physical_block_size % 512) {
		/* [한국어] 물리 블록 크기(논리 vs 물리 분리 — 4K/512e 시뮬레이션용)도 512 align. */
		SPDK_ERRLOG("Physical block must be 512 bytes aligned\n");
		return -EINVAL;
	}

	if (opts->numa_id != SPDK_ENV_NUMA_ID_ANY) {
		/* Verify if requested NUMA node ID is present on the system. */
		/* [한국어] 사용자가 특정 NUMA를 요청했으면 실제 그 NUMA가 시스템에 있는지 확인. */
		SPDK_ENV_FOREACH_NUMA_ID(numa_id) {
			/* [한국어] DPDK가 발견한 모든 NUMA 노드를 순회. break로 발견 시 즉시 탈출. */
			if (numa_id == opts->numa_id) {
				break;
			}
		}
		if (numa_id == INT32_MAX) {
			/* [한국어] 매크로 종료 sentinel — 일치하는 NUMA 없음 = 무효한 옵션. */
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
		/* [한국어] NVMe spec이 정의한 메타 크기 값들 — 그 외는 검증/생성 코드가 가정하지 않음. */
		break;
	default:
		SPDK_ERRLOG("metadata size %u is not supported\n", opts->md_size);
		return -EINVAL;
	}

	if (opts->md_interleave) {
		/* [한국어] 인터리브: 디스크의 실제 블록 크기는 데이터+메타 합 — bdev core가 인식하는 blocklen. */
		block_size = opts->block_size + opts->md_size;
	} else {
		/* [한국어] 분리 모드: bdev core의 blocklen은 데이터만, 메타는 별도 영역. */
		block_size = opts->block_size;
	}

	mdisk = calloc(1, sizeof(*mdisk));
	/* [한국어] 일반 heap에서 malloc_disk 자체 할당. 0으로 초기화되어 실패 롤백 시 NULL 안전. */
	if (!mdisk) {
		SPDK_ERRLOG("mdisk calloc() failed\n");
		return -ENOMEM;
	}

	/*
	 * Allocate the large backend memory buffer from pinned memory.
	 */
	mdisk->malloc_buf = spdk_zmalloc(opts->num_blocks * block_size, 2 * 1024 * 1024, NULL,
					 opts->numa_id, SPDK_MALLOC_DMA);
	/* [한국어] 데이터 영역: hugepage 기반 핀된 메모리(DPDK rte_malloc), 2MiB 정렬로 DMA 친화적.
	 *   size: num_blocks * block_size
	 *   align: 2MiB (hugepage 경계 — DSA/IOAT 페이지 정렬 요구 충족)
	 *   phys_addr: NULL (가상 주소만 사용)
	 *   numa: 사용자 옵션 (ANY면 DPDK가 자유 선택)
	 *   flag: SPDK_MALLOC_DMA = DMA 가능 영역(IOMMU 매핑 등록)
	 * 실패 시 mdisk 롤백 후 ENOMEM. */
	if (!mdisk->malloc_buf) {
		SPDK_ERRLOG("malloc_buf spdk_zmalloc() failed\n");
		malloc_disk_free(mdisk);
		return -ENOMEM;
	}

	if (!opts->md_interleave && opts->md_size != 0) {
		/* [한국어] 분리 메타 모드(DIX): 메타 영역을 별도 hugepage로 확보. */
		mdisk->malloc_md_buf = spdk_zmalloc(opts->num_blocks * opts->md_size, 2 * 1024 * 1024, NULL,
						    opts->numa_id, SPDK_MALLOC_DMA);
		/* [한국어] 데이터 영역과 동일한 정책(2MiB align, DMA, NUMA 일치). */
		if (!mdisk->malloc_md_buf) {
			SPDK_ERRLOG("malloc_md_buf spdk_zmalloc() failed\n");
			malloc_disk_free(mdisk);
			return -ENOMEM;
		}
	}

	if (opts->name) {
		/* [한국어] 사용자 지정 이름 — strdup으로 디스크가 소유하는 사본. */
		mdisk->disk.name = strdup(opts->name);
	} else {
		/* Auto-generate a name */
		/* [한국어] 이름 미지정 — "Malloc%d" 형식으로 자동 부여. */
		mdisk->disk.name = spdk_sprintf_alloc("Malloc%d", malloc_disk_count);
		malloc_disk_count++;
	}
	if (!mdisk->disk.name) {
		/* [한국어] strdup/sprintf_alloc 실패 — 메모리 부족. 자원 정리. */
		malloc_disk_free(mdisk);
		return -ENOMEM;
	}
	mdisk->disk.product_name = "Malloc disk";
	/* [한국어] NVMe Identify의 model number 자리 — 사용자에게 노출되는 제품명 문자열. */

	mdisk->disk.write_cache = 1;
	/* [한국어] 캐시 모델 표시 — RAM이라 이미 휘발성이지만 write-cache=1로 알려 user가 flush 호출 가능. */
	mdisk->disk.blocklen = block_size;
	/* [한국어] bdev core가 인식하는 블록 크기. 인터리브면 data+meta. */
	mdisk->disk.phys_blocklen = opts->physical_block_size;
	/* [한국어] NVMe IDENT의 NPDA/AWUN과 유사 — 논리 블록 < 물리 블록 시 정렬 권고. */
	mdisk->disk.blockcnt = opts->num_blocks;
	/* [한국어] 총 LBA 수 (= 총 hugepage 크기 / blocklen). */
	mdisk->disk.md_len = opts->md_size;
	/* [한국어] 블록당 메타 크기 (0=없음). */
	mdisk->disk.md_interleave = opts->md_interleave;
	/* [한국어] 인터리브/분리 플래그. */
	mdisk->disk.dif_type = opts->dif_type;
	/* [한국어] NVMe Type1/2/3 또는 DISABLE. */
	mdisk->disk.dif_is_head_of_md = opts->dif_is_head_of_md;
	/* [한국어] PI 필드가 메타의 앞쪽인지 뒤쪽인지 — NVMe spec PIF 옵션. */
	/* Current block device layer API does not propagate
	 * any DIF related information from user. So, we can
	 * not generate or verify Application Tag.
	 */
	switch (opts->dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		/* [한국어] Type1/2: GUARD(CRC) + REFTAG(LBA) 검사. APPTAG는 API 미지원으로 생략. */
		mdisk->disk.dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK |
					      SPDK_DIF_FLAGS_REFTAG_CHECK;
		break;
	case SPDK_DIF_TYPE3:
		/* [한국어] Type3: GUARD만 검사 (reftag/apptag는 사용 안 함). */
		mdisk->disk.dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK;
		break;
	case SPDK_DIF_DISABLE:
		/* [한국어] 검사 비활성 — flag 0 유지. */
		break;
	}
	mdisk->disk.dif_pi_format = opts->dif_pi_format;
	/* [한국어] CRC16/CRC32/CRC64 포맷 선택 — NVMe 2.0 PIF. */

	if (opts->dif_type != SPDK_DIF_DISABLE) {
		/* [한국어] DIF 활성 시 hugepage 전체에 초기 PI 박아 두기 — 안 하면 read 검증 실패. */
		rc = malloc_disk_setup_pi(mdisk);
		if (rc) {
			SPDK_ERRLOG("Failed to set up protection information.\n");
			malloc_disk_free(mdisk);
			return rc;
		}
	}

	if (opts->optimal_io_boundary) {
		/* [한국어] 사용자 정의 정렬 경계 — bdev core가 큰 I/O를 이 경계로 자동 split. */
		mdisk->disk.optimal_io_boundary = opts->optimal_io_boundary;
		mdisk->disk.split_on_optimal_io_boundary = true;
	}
	if (!spdk_uuid_is_null(&opts->uuid)) {
		/* [한국어] 사용자가 UUID를 지정한 경우 그대로 복사. 미지정 시 bdev_register가 자동 부여. */
		spdk_uuid_copy(&mdisk->disk.uuid, &opts->uuid);
	}

	mdisk->disk.numa.id_valid = 1;
	mdisk->disk.numa.id = opts->numa_id;
	/* [한국어] 디스크의 NUMA affinity 광고 — 상위 사용자가 같은 NUMA 코어에서 I/O 발행하면 더 빠름. */

	mdisk->disk.max_copy = 0;
	/* [한국어] COPY I/O 한 번에 처리할 수 있는 최대 블록 수 — 0=무제한. */
	mdisk->disk.ctxt = mdisk;
	/* [한국어] fn_table 콜백이 받는 ctx로 self-pointer를 박아 둠 → 모듈 함수에서 mdisk 역참조. */
	mdisk->disk.fn_table = &malloc_fn_table;
	/* [한국어] 함수 디스패치 테이블 — bdev core가 destruct/submit/io_channel 등 호출. */
	mdisk->disk.module = &malloc_if;
	/* [한국어] 어느 모듈이 소유한 bdev인지 — unregister_by_name에서 매칭. */

	rc = spdk_bdev_register(&mdisk->disk);
	/* [한국어] bdev core에 디스크 등록. 이후 사용자가 spdk_bdev_open* 가능해짐.
	 * 등록은 동기 — 내부적으로 spdk_thread send_msg를 사용하지만 함수 자체는 호출 직후 반환.
	 * 등록 실패 시 자원 정리. */
	if (rc) {
		malloc_disk_free(mdisk);
		return rc;
	}

	*bdev = &(mdisk->disk);
	/* [한국어] 호출자(RPC 핸들러)에게 출력 포인터 전달 — 응답에 이름 포함. */

	TAILQ_INSERT_TAIL(&g_malloc_disks, mdisk, link);
	/* [한국어] 전역 리스트에 추가 — destruct 시 REMOVE와 짝. */
	SPDK_DEBUGLOG(bdev_malloc, "Bdev:%s created on NUMA node ID: %d\n",
		      spdk_bdev_get_name(*bdev), spdk_bdev_get_numa_id(*bdev));

	return rc;
	/* [한국어] 정상 종료 시 rc=0 (spdk_bdev_register의 반환값). */
}

/*
 * [한국어]
 * delete_malloc_disk - Malloc bdev 삭제 (RPC 핸들러 진입점)
 *
 * @name:   삭제할 bdev 이름.
 * @cb_fn:  완료 콜백 (성공/실패 모두 호출).
 * @cb_arg: cb_fn에 전달할 사용자 컨텍스트.
 *
 * spdk_bdev_unregister_by_name이 비동기로 진행 — open된 디스크의 모든 채널을 정리하고
 * destruct까지 완료된 후에 cb_fn 호출. by_name 호출 자체가 즉시 실패한 경우(이름 매칭
 * 없음 등)는 동기로 cb_fn(rc) 호출해 RPC가 응답할 수 있게 함.
 *
 * 호출 체인: bdev_malloc_rpc.c::rpc_bdev_malloc_delete → delete_malloc_disk →
 *   spdk_bdev_unregister_by_name → (정리 진행) → bdev_malloc_destruct → cb_fn
 */
void
delete_malloc_disk(const char *name, spdk_delete_malloc_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_bdev_unregister_by_name(name, &malloc_if, cb_fn, cb_arg);
	/* [한국어] 모듈 디스크립터를 함께 넘겨 이름이 같지만 다른 모듈인 bdev는 매칭에서 제외. */
	if (rc != 0) {
		/* [한국어] 동기 실패 — 콜백이 호출되지 않으므로 여기서 직접 호출. 비-0이면 매칭 실패 등. */
		cb_fn(cb_arg, rc);
	}
}

/*
 * [한국어]
 * malloc_completion_poller - 즉시 완료 큐를 비우는 poller 콜백
 *
 * @ctx: malloc_channel.
 * @return: SPDK_POLLER_BUSY=일했음 / IDLE=한 일 없음 — reactor 통계용.
 *
 * RESET/FLUSH/ZCOPY/ABORT 같은 즉시 완료 case가 채널의 completed_tasks에 enqueue되면
 * 다음 reactor tick에서 본 poller가 깨어나 일괄 spdk_bdev_io_complete 호출한다. 큐를
 * 임시 리스트로 swap한 뒤 처리하므로, 처리 중에 새로운 enqueue가 와도 race가 없다.
 *
 * 실행 컨텍스트: 채널 reactor (코어 로컬). 채널과 같은 스레드에 등록된 poller라 락 없음.
 */
static int
malloc_completion_poller(void *ctx)
{
	struct malloc_channel *ch = ctx;
	struct malloc_task *task;
	TAILQ_HEAD(, malloc_task) completed_tasks;
	/* [한국어] 채널 큐에서 빼낸 task들을 잠시 담아둘 로컬 리스트. */
	uint32_t num_completions = 0;

	TAILQ_INIT(&completed_tasks);
	/* [한국어] 로컬 헤더 초기화. */
	TAILQ_SWAP(&completed_tasks, &ch->completed_tasks, malloc_task, tailq);
	/* [한국어] 두 리스트를 atomic 스왑(매크로 — single thread 컨텍스트라 명목상 atomic).
	 * 채널 큐는 비워지고 로컬 리스트가 작업 집합을 인계받음. 이후 enqueue는 새로운 사이클로 분리. */

	while (!TAILQ_EMPTY(&completed_tasks)) {
		task = TAILQ_FIRST(&completed_tasks);
		TAILQ_REMOVE(&completed_tasks, task, tailq);
		/* [한국어] 큐 머리에서 하나씩 빼서 처리 — FIFO 순서. */
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), task->status);
		/* [한국어] bdev core에 완료 통지 — 사용자 cb_fn 호출 진행. complete 안에서 또 다른
		 * submit이 와도 새로운 task는 ch->completed_tasks(SWAP 후 비어 있음)에 들어가
		 * 다음 tick에 처리되므로 재진입 안전. */
		num_completions++;
	}

	return num_completions > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
	/* [한국어] 한 번이라도 처리했으면 BUSY로 알려 reactor가 통계/sleep 결정에 활용. */
}

/*
 * [한국어]
 * malloc_create_channel_cb - io_device의 채널 생성 콜백
 *
 * @io_device: g_malloc_disks 주소 (등록 시 키).
 * @ctx:       채널 컨텍스트 — sizeof(malloc_channel) 만큼 spdk_thread가 잡아 줌.
 *
 * 채널이 처음 attach되는 시점에 호출 (코어별 1회). accel framework의 io_channel을 빌리고
 * 완료 poller를 등록. 실패 시 ENOMEM 반환 → spdk_get_io_channel이 NULL 리턴.
 *
 * 실행 컨텍스트: 채널이 사용될 reactor 스레드.
 */
static int
malloc_create_channel_cb(void *io_device, void *ctx)
{
	struct malloc_channel *ch = ctx;
	/* [한국어] io_channel 끝에 임베드된 malloc_channel 컨텍스트. */

	ch->accel_channel = spdk_accel_get_io_channel();
	/* [한국어] accel framework의 동일 코어 채널 획득 — submit_copy/fill 호출에 사용. */
	if (!ch->accel_channel) {
		SPDK_ERRLOG("Failed to get accel framework's IO channel\n");
		return -ENOMEM;
	}

	ch->completion_poller = SPDK_POLLER_REGISTER(malloc_completion_poller, ch, 0);
	/* [한국어] 완료 처리 poller 등록. period=0 = 매 tick마다 실행 (idle 시 reactor가 백오프). */
	if (!ch->completion_poller) {
		SPDK_ERRLOG("Failed to register malloc completion poller\n");
		spdk_put_io_channel(ch->accel_channel);
		/* [한국어] 부분 성공 롤백: accel channel 해제. */
		return -ENOMEM;
	}

	TAILQ_INIT(&ch->completed_tasks);
	/* [한국어] 완료 큐 헤더 초기화 — 이후 malloc_complete_task가 INSERT_TAIL. */

	return 0;
}

/*
 * [한국어]
 * malloc_destroy_channel_cb - io_device 채널 파괴 콜백
 *
 * 채널 ref count가 0이 될 때 호출. accel channel 반환과 poller 해제.
 */
static void
malloc_destroy_channel_cb(void *io_device, void *ctx)
{
	struct malloc_channel *ch = ctx;

	assert(TAILQ_EMPTY(&ch->completed_tasks));
	/* [한국어] 모든 inflight I/O가 끝난 뒤 destroy되어야 정상 — 큐에 남아 있으면 누수. */

	spdk_put_io_channel(ch->accel_channel);
	/* [한국어] accel framework 채널 ref 반환. 마지막이면 accel destroy_channel_cb 호출됨. */
	spdk_poller_unregister(&ch->completion_poller);
	/* [한국어] 완료 poller 해제 — 인자는 포인터 주소(매크로가 *ch->... = NULL 처리). */
}

/*
 * [한국어]
 * bdev_malloc_initialize - 모듈 초기화 (SPDK_BDEV_MODULE_REGISTER가 등록한 module_init)
 *
 * SPDK 부팅 시 bdev 서브시스템이 모든 모듈의 module_init을 호출. 본 함수는 글로벌
 * io_device 등록 — 모든 Malloc bdev가 이 io_device를 공유.
 */
static int
bdev_malloc_initialize(void)
{
	/* This needs to be reset for each reinitialization of submodules.
	 * Otherwise after enough devices or reinitializations the value gets too high.
	 * TODO: Make malloc bdev name mandatory and remove this counter. */
	/* [한국어] 동적 reload 시에도 Malloc0부터 시작하도록 카운터 리셋. */
	malloc_disk_count = 0;

	spdk_io_device_register(&g_malloc_disks, malloc_create_channel_cb,
				malloc_destroy_channel_cb, sizeof(struct malloc_channel),
				"bdev_malloc");
	/* [한국어] io_device 등록:
	 *   key:       &g_malloc_disks 주소 (글로벌 unique).
	 *   create_cb: 채널 attach 시 호출.
	 *   destroy_cb: 채널 detach 시 호출.
	 *   size:      malloc_channel 임베드 크기.
	 *   name:      디버깅용 식별자. */

	return 0;
}

/*
 * [한국어]
 * bdev_malloc_deinitialize - 모듈 종료 (SPDK_BDEV_MODULE_REGISTER의 module_fini)
 *
 * SPDK 종료 시 호출. io_device 해제 — 모든 채널이 정리된 후에 한 번 실행.
 */
static void
bdev_malloc_deinitialize(void)
{
	spdk_io_device_unregister(&g_malloc_disks, NULL);
	/* [한국어] NULL = unregister 완료 콜백 없음(동기 의미). */
}

SPDK_LOG_REGISTER_COMPONENT(bdev_malloc)
/* [한국어] 로그 컴포넌트 등록 매크로 — `spdk_log_set_flag("bdev_malloc")`로 DEBUGLOG 활성화 가능.
 * SPDK_DEBUGLOG(bdev_malloc, ...)와 짝. */
