/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL I/O 채널(ftl_io_channel) 라이프사이클 관리 (ftl_mngt_ioch.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 FTL이 호스트 I/O를 받기 위한 per-thread `ftl_io_channel` 객체의
 * 생성/등록/해제/제거를 담당한다. SPDK의 spdk_io_device 추상화 위에 FTL 고유 채널을
 * 얹어, 모든 spdk_thread가 자신만의 SQ(Submission Queue)/CQ(Completion Queue) ring과
 * map mempool, poller를 가지고 lockless로 I/O를 발행할 수 있도록 한다.
 * 또한 코어 스레드 측에서 등록된 모든 채널을 추적하기 위한 dev->ioch_queue 관리도
 * 본 파일이 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 컨텍스트: 본 파일의 step 함수(register/unregister, init/deinit)는 코어 스레드에서
 * ftl_mngt 파이프라인을 통해 호출. 그러나 io_channel_create_cb / destroy_cb는
 * 임의의 spdk_thread(어떤 reactor든)에서 호출되며, 새 채널을 코어 스레드의 ioch_queue에
 * 추가/제거할 때 cross-thread 안전성을 위해 spdk_thread_send_msg로 코어 스레드에 메시지
 * 전달한다(lockless 패턴).
 *   호출 체인 (startup):
 *     startup desc → ftl_mngt_register_io_device → spdk_io_device_register
 *                  → ftl_mngt_init_io_channel (코어 스레드의 ioch 획득)
 *                  → 호스트 측 spdk_get_io_channel(dev) 시 → io_channel_create_cb
 *                       → ftl_dev_register_channel (코어 스레드 메시지)
 *   호출 체인 (shutdown):
 *     shutdown desc → ftl_mngt_deinit_io_channel (코어 ioch put)
 *                   → ftl_mngt_unregister_io_device → spdk_io_device_unregister
 *                       → 모든 thread에서 io_channel_destroy_cb
 *                          → io_channel_unregister (코어 스레드 메시지) → 자원 해제
 *
 * === 타 모듈과의 연결 ===
 * - 의존: SPDK 코어 thread/io_device API (spdk_io_device_register/unregister,
 *   spdk_get_io_channel/spdk_put_io_channel, spdk_thread_send_msg, spdk_poller),
 *   spdk_ring (lockless SP/SC ring), ftl_mempool (DPDK 기반 메모리 풀),
 *   ftl_io_channel_poll (lib/ftl/ftl_io.c) — 채널 폴러 함수.
 * - 의존받음: ftl_mngt_startup.c, ftl_mngt_shutdown.c, 호스트 I/O 진입점
 *   (spdk_ftl_writev/readv 등은 spdk_get_io_channel로 ftl_io_channel을 획득해야 함).
 * - 데이터 흐름: 호스트 I/O는 cli thread에서 ftl_io_channel.sq에 push → 코어 스레드의
 *   ftl_io_channel_poll이 dequeue → 처리 후 ftl_io_channel.cq로 완료 push → cli thread가
 *   cq를 폴링해 콜백 호출.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ftl_io_channel_ctx : SPDK가 채널마다 할당하는 작은 wrapper(우리 ioch 포인터만 보유).
 * - ftl_io_channel_get_ctx()  : spdk_io_channel → ftl_io_channel 변환 헬퍼.
 * - ftl_dev_register_channel(): 새 채널을 dev->ioch_queue에 추가(코어 스레드에서만).
 * - io_channel_unregister()   : 채널 자원(ring, mempool, ioch 자체) 해제 + queue 제거.
 * - io_channel_create_cb()    : SPDK가 채널 생성 시 호출 — ring/mempool/poller 셋업.
 * - io_channel_destroy_cb()   : SPDK가 채널 제거 시 호출 — 폴러 정지 + 코어에 자원 해제 메시지.
 * - ftl_mngt_register_io_device()/unregister_io_device(): io_device 등록/해제 step.
 * - ftl_mngt_init_io_channel()/deinit_io_channel()      : 코어 스레드용 ioch 획득/반환 step.
 */

#include "spdk/thread.h"
/* [한국어] spdk_thread / spdk_thread_send_msg / spdk_get_io_channel /
 * spdk_io_device_register / spdk_poller 등 SPDK 스레드·IO 채널 API. */

#include "ftl_core.h"
/* [한국어] spdk_ftl_dev (dev->ioch_queue, dev->core_thread, dev->ioch 등). */
#include "ftl_mngt.h"
/* [한국어] ftl_mngt API. */
#include "ftl_mngt_steps.h"
/* [한국어] step 프로토타입. */
#include "ftl_band.h"
/* [한국어] band 자료구조 — ftl_io_channel이 band 메타에 접근하므로 포함. */

/*
 * [한국어]
 * struct ftl_io_channel_ctx - SPDK io_channel ctx에 우리 ioch 포인터를 보관하는 wrapper.
 *
 * 왜 wrapper가 필요한가: spdk_io_device_register에 등록된 ctx_size 만큼의 메모리는
 * SPDK가 채널 destroy 시 자동으로 해제하지만, 우리는 채널 destroy를 비동기로
 * 코어 스레드에 위임해서 ftl_io_channel 본체를 거기서 free해야 한다. 즉 SPDK ctx
 * 메모리 수명과 ftl_io_channel 수명이 다르므로 wrapper에 포인터만 둔다.
 */
struct ftl_io_channel_ctx {
	struct ftl_io_channel *ioch;
	/* [한국어] 실제 FTL I/O 채널 본체 포인터. ioch 자체는 calloc으로 별도 할당.
	 * 설정자: io_channel_create_cb (SPDK 콜백, 임의 thread).
	 * 읽는 자: ftl_io_channel_get_ctx (호스트 I/O 경로), io_channel_destroy_cb.
	 * 값 범위: 유효한 ftl_io_channel 포인터 (생성 성공 후), 또는 0 (생성 중 실패).
	 * 동기화: 채널 단일 thread 사용이라 별도 락 불필요. */
};

/*
 * [한국어]
 * ftl_io_channel_get_ctx - SPDK io_channel으로부터 ftl_io_channel 추출.
 *
 * @ioch: spdk_get_io_channel(dev)의 반환값.
 * @return: 해당 thread에서 사용하는 FTL I/O 채널 포인터.
 *
 * 왜 필요한가: 호스트 I/O 경로가 spdk_io_channel만 받았을 때 그 안에서 우리 ioch를
 * 꺼내쓰기 위함. SPDK ctx wrapper(ftl_io_channel_ctx)에서 한 번만 dereference.
 *
 * 실행 컨텍스트: 호스트 I/O 진입 thread (코어가 아닌 cli thread일 수도 있음).
 */
struct ftl_io_channel *
ftl_io_channel_get_ctx(struct spdk_io_channel *ioch)
{
	struct ftl_io_channel_ctx *ctx = spdk_io_channel_get_ctx(ioch);
	/* [한국어] SPDK가 채널 ctx 메모리에 우리 wrapper를 두었으므로 첫 인스턴스 가져옴. */
	return ctx->ioch;
	/* [한국어] wrapper에 보관된 실제 ftl_io_channel 포인터. */
}

/*
 * [한국어]
 * ftl_dev_register_channel - 새로 만든 채널을 코어 스레드의 ioch_queue에 등록.
 *
 * @ctx: ftl_io_channel 포인터 (메시지 인자로 전달).
 *
 * 왜 필요한가: dev->ioch_queue는 코어 스레드만 접근한다는 약속(lockless 설계)으로
 * 운영. 새 채널 등록은 임의 thread에서 발생하므로 spdk_thread_send_msg로 코어
 * 스레드에 위임해야 안전.
 *
 * 실행 컨텍스트: 항상 dev->core_thread (메시지 핸들러).
 */
static void
ftl_dev_register_channel(void *ctx)
{
	struct ftl_io_channel *ioch = ctx;
	struct spdk_ftl_dev *dev = ioch->dev;

	/* This only runs on the core thread, so it's safe to do this lockless */
	/* [한국어] 코어 스레드에서만 실행되므로 락 없이 안전하게 큐에 추가. */
	TAILQ_INSERT_TAIL(&dev->ioch_queue, ioch, entry);
	/* [한국어] dev->ioch_queue는 BSD-style 양방향 리스트(TAILQ).
	 * 코어 폴러가 이 큐를 순회해 모든 채널의 SQ에서 I/O를 회수. */
}

/*
 * [한국어]
 * io_channel_unregister - 채널 자원(ring, mempool, ioch 자체) 해제 + 큐 제거.
 *
 * @ctx: ftl_io_channel 포인터 (메시지 인자).
 *
 * 왜 필요한가: 채널 제거를 코어 스레드에서 처리하려면 destroy_cb가 직접 free하지 않고
 * 메시지로 코어에 위임해야 한다(ioch_queue가 lockless이기 때문).
 *
 * 실행 컨텍스트: dev->core_thread.
 */
static void
io_channel_unregister(void *ctx)
{
	struct ftl_io_channel *ioch = ctx;
	struct spdk_ftl_dev *dev = ioch->dev;

	TAILQ_REMOVE(&dev->ioch_queue, ioch, entry);
	/* [한국어] 코어 스레드의 ioch_queue에서 채널 제거 — 폴러가 더 이상 순회 대상 안 함. */

	spdk_ring_free(ioch->cq);
	/* [한국어] 완료 큐 ring 해제 — 안에 보류 항목이 있으면 손실되지만 destroy 시점은
	 * 호스트가 더 이상 I/O를 보내지 않는 시점이라야 함. */
	spdk_ring_free(ioch->sq);
	/* [한국어] 제출 큐 ring 해제. */
	ftl_mempool_destroy(ioch->map_pool);
	/* [한국어] map mempool 해제 — write I/O가 PBA 매핑을 모아두던 풀. */
	free(ioch);
	/* [한국어] ftl_io_channel 본체 해제 (calloc된 메모리). */
}

/*
 * [한국어]
 * io_channel_create_cb - SPDK가 spdk_get_io_channel(dev) 호출 시 임의 thread에서 호출.
 *
 * @io_device: spdk_io_device_register에 등록한 dev 포인터(= spdk_ftl_dev).
 * @ctx:       SPDK가 ctx_size만큼 할당한 메모리(= ftl_io_channel_ctx wrapper).
 * @return:    0=성공, 음수=실패 (실패 시 spdk_get_io_channel이 NULL 반환).
 *
 * 왜 필요한가: 채널을 처음 요구한 thread가 자기만의 ftl_io_channel(ring, mempool, poller)을
 * 만들도록 SPDK가 콜백을 호출. 본 함수는 그 모든 자원을 셋업.
 *
 * 동작:
 *  1) ftl_io_channel 본체 calloc.
 *  2) mempool name 생성 (디버그/통계용).
 *  3) ftl_mempool_create — 호스트 I/O마다 PBA 배열 보관용.
 *  4) spdk_ring_create — SP_SC(Single Producer, Single Consumer) ring으로 sq/cq.
 *  5) SPDK_POLLER_REGISTER — 본 thread에서 ftl_io_channel_poll을 무한 호출하는 폴러 등록.
 *  6) spdk_thread_send_msg(core_thread, ftl_dev_register_channel, ioch) — 큐 등록은 코어에 위임.
 *  7) wrapper에 ioch 포인터 저장.
 * 실패 시 goto chain으로 자원 역순 해제.
 *
 * 실행 컨텍스트: 임의 spdk_thread (호출자가 spdk_get_io_channel 호출한 thread).
 */
static int
io_channel_create_cb(void *io_device, void *ctx)
{
	struct spdk_ftl_dev *dev = io_device;
	struct ftl_io_channel_ctx *_ioch = ctx;
	struct ftl_io_channel *ioch;
	char mempool_name[32];
	int rc;

	FTL_NOTICELOG(dev, "FTL IO channel created on %s\n",
		      spdk_thread_get_name(spdk_get_thread()));
	/* [한국어] 채널을 만든 thread 이름을 로그(디버그용 — 어떤 reactor에 채널이 생겼는지). */

	/* This gets unregistered asynchronously with the device -
	 * we can't just use the ctx buffer passed by the thread library
	 */
	/* [한국어] SPDK ctx wrapper 메모리는 SPDK가 destroy 시 자동 해제하지만, 우리 ftl_io_channel
	 * 본체는 코어 스레드에서 비동기 free해야 하므로 별도 calloc으로 분리. */
	ioch = calloc(1, sizeof(*ioch));
	if (ioch == NULL) {
		FTL_ERRLOG(dev, "Failed to allocate IO channel\n");
		return -1;
	}

	rc = snprintf(mempool_name, sizeof(mempool_name), "ftl_io_%p", ioch);
	/* [한국어] mempool 디버그 이름 — 채널 포인터를 포함해 유일성 보장. */
	if (rc < 0 || rc >= (int)sizeof(mempool_name)) {
		/* [한국어] snprintf 오류 또는 버퍼 truncation — 가능성 낮으나 방어적 처리. */
		FTL_ERRLOG(dev, "Failed to create IO channel pool name\n");
		free(ioch);
		return -1;
	}

	ioch->dev = dev;
	/* [한국어] 백포인터 — destroy 시 어느 dev의 채널인지 식별. */

	ioch->map_pool = ftl_mempool_create(
				 dev->conf.user_io_pool_size,
				 sizeof(ftl_addr) * dev->xfer_size,
				 64,
				 SPDK_ENV_NUMA_ID_ANY);
	/* [한국어] 호스트 write 1건당 xfer_size개의 PBA(ftl_addr) 배열을 잡아두는 풀.
	 * - count: user_io_pool_size 만큼 동시 in-flight I/O 가능
	 * - element size: xfer_size * sizeof(ftl_addr)
	 * - alignment: 64B (cache line)
	 * - NUMA: ANY (생성 thread의 NUMA 노드 자동 선택). */
	if (!ioch->map_pool) {
		FTL_ERRLOG(dev, "Failed to create IO channel's  map IO pool\n");
		goto fail_io_pool;
	}

	ioch->cq = spdk_ring_create(SPDK_RING_TYPE_SP_SC, spdk_align64pow2(dev->conf.user_io_pool_size + 1),
				    SPDK_ENV_NUMA_ID_ANY);
	/* [한국어] 완료 큐 — 코어 스레드(producer)가 push, 호스트 thread(consumer)가 pop.
	 * SP_SC: Single Producer / Single Consumer = lockless 빠른 경로. 크기는 +1 후
	 * 다음 2^n으로 정렬 (DPDK ring 요구). */
	if (!ioch->cq) {
		FTL_ERRLOG(dev, "Failed to create IO channel completion queue\n");
		goto fail_io_pool;
	}

	ioch->sq = spdk_ring_create(SPDK_RING_TYPE_SP_SC, spdk_align64pow2(dev->conf.user_io_pool_size + 1),
				    SPDK_ENV_NUMA_ID_ANY);
	/* [한국어] 제출 큐 — 호스트 thread가 push, 코어 스레드가 pop. */
	if (!ioch->sq) {
		FTL_ERRLOG(dev, "Failed to create IO channel submission queue\n");
		goto fail_cq;
	}

	ioch->poller = SPDK_POLLER_REGISTER(ftl_io_channel_poll, ioch, 0);
	/* [한국어] 본 thread에 ftl_io_channel_poll 폴러 등록 — 0us 주기(매 reactor tick).
	 * 호스트 thread에서 cq를 폴링해 완료 콜백을 사용자에게 전달. */
	if (!ioch->poller) {
		FTL_ERRLOG(dev, "Failed to register IO channel poller\n");
		goto fail_sq;
	}

	spdk_thread_send_msg(dev->core_thread, ftl_dev_register_channel, ioch);
	/* [한국어] 코어 스레드에 "ioch_queue에 ioch 추가" 메시지 전송. lockless로 큐에 등록. */

	_ioch->ioch = ioch;
	/* [한국어] SPDK wrapper에 본체 포인터 보관 — 이후 ftl_io_channel_get_ctx로 재획득. */
	return 0;

fail_sq:
	spdk_ring_free(ioch->sq);
	/* [한국어] poller 등록 실패 — sq 해제 후 cq fall through. */
fail_cq:
	spdk_ring_free(ioch->cq);
	/* [한국어] sq 생성 실패 또는 위 fall through — cq 해제. */
fail_io_pool:
	ftl_mempool_destroy(ioch->map_pool);
	/* [한국어] map_pool 해제. */
	free(ioch);
	/* [한국어] ftl_io_channel 본체 해제. */

	return -1;
}

/*
 * [한국어]
 * io_channel_destroy_cb - SPDK가 spdk_put_io_channel 또는 io_device_unregister 시 호출.
 *
 * @io_device: dev 포인터.
 * @ctx:       ftl_io_channel_ctx wrapper.
 *
 * 왜 필요한가: 채널을 만든 thread에서 폴러를 정지하고, 본체 free는 코어 스레드에 메시지로
 * 위임. wrapper(ctx) 메모리는 본 함수 반환 후 SPDK가 자동 해제.
 *
 * 실행 컨텍스트: 채널 owner thread.
 */
static void
io_channel_destroy_cb(void *io_device, void *ctx)
{
	struct ftl_io_channel_ctx *_ioch = ctx;
	struct ftl_io_channel *ioch = _ioch->ioch;
	struct spdk_ftl_dev *dev = ioch->dev;

	FTL_NOTICELOG(dev, "FTL IO channel destroy on %s\n",
		      spdk_thread_get_name(spdk_get_thread()));
	/* [한국어] 어느 thread에서 채널이 사라지는지 로그(디버그용). */

	spdk_poller_unregister(&ioch->poller);
	/* [한국어] 본 thread에서 폴러 정지 — 폴러는 자기 thread에서만 unregister 가능. */
	spdk_thread_send_msg(ftl_get_core_thread(dev),
			     io_channel_unregister, ioch);
	/* [한국어] ring/mempool/ioch 본체 해제는 코어 스레드에서 처리하도록 메시지 전송.
	 * 이로써 ioch_queue 조작이 코어 스레드 안에서만 일어남(lockless 보장). */
}

/*
 * [한국어]
 * ftl_mngt_register_io_device - dev를 SPDK io_device로 등록하는 step.
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: 등록 전에는 어떤 thread도 spdk_get_io_channel(dev)을 호출할 수 없다.
 * startup 단계 중 ring/mempool 같은 리소스가 다 준비된 후 본 step을 실행해야 안전.
 *
 * 동작: dev->io_device_registered 플래그 set 후 spdk_io_device_register 호출.
 *      등록은 동기 — 즉시 next_step.
 */
void
ftl_mngt_register_io_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->io_device_registered = true;
	/* [한국어] 셧다운 시 unregister가 필요한지 판단하는 플래그. */

	spdk_io_device_register(dev, io_channel_create_cb,
				io_channel_destroy_cb,
				sizeof(struct ftl_io_channel_ctx),
				NULL);
	/* [한국어] dev를 io_device로 등록.
	 * - create_cb: 임의 thread에서 채널 만들 때 호출
	 * - destroy_cb: 채널 사라질 때 호출
	 * - ctx_size: 채널마다 할당될 wrapper 크기
	 * - name: NULL (디버그 이름 없음). */

	ftl_mngt_next_step(mngt);
	/* [한국어] 등록 동기 완료 — 다음 step. */
}

/*
 * [한국어]
 * unregister_cb - spdk_io_device_unregister가 모든 채널 destroy 후 호출하는 콜백.
 *
 * @io_device: dev 포인터.
 *
 * 왜 필요한가: unregister는 비동기 — 모든 thread의 destroy_cb가 끝나야 본 cb 호출.
 * 그 시점에 mngt를 next_step으로 진행해야 셧다운 파이프라인이 다음으로 갈 수 있다.
 *
 * 실행 컨텍스트: SPDK가 콜백을 호출하는 thread (보통 unregister를 호출한 thread).
 */
static void
unregister_cb(void *io_device)
{
	struct spdk_ftl_dev *dev = io_device;
	struct ftl_mngt_process *mngt = dev->unregister_process;
	/* [한국어] unregister 시작 시 dev에 보관해뒀던 mngt 핸들 복원. */

	dev->io_device_registered = false;
	/* [한국어] 더 이상 io_device 아님 — 향후 register 호출이 안전하도록 플래그 리셋. */
	dev->unregister_process = NULL;
	/* [한국어] 사용 끝난 mngt 백업 핸들 NULL로. */

	ftl_mngt_next_step(mngt);
	/* [한국어] 셧다운 다음 step으로 진행. */
}

/*
 * [한국어]
 * ftl_mngt_unregister_io_device - io_device 등록을 비동기 해제하는 step.
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: 셧다운 시 모든 채널을 정리한 뒤 io_device 등록 자체를 해제해야 다음
 * 누가 같은 dev를 다시 io_device로 등록 가능. 이미 등록 안 된 상태(skip)이면 step 건너뜀.
 */
void
ftl_mngt_unregister_io_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (dev->io_device_registered) {
		/* [한국어] 등록되어 있으면 비동기 unregister 시작. */
		dev->unregister_process = mngt;
		/* [한국어] 비동기 cb에서 next_step할 mngt 핸들 임시 보관. */
		spdk_io_device_unregister(dev, unregister_cb);
		/* [한국어] SPDK가 모든 thread에서 destroy_cb를 트리거 후 마지막에 unregister_cb 호출. */
	} else {
		/* [한국어] 이미 unregister됨 — 그냥 skip. */
		ftl_mngt_skip_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_init_io_channel - 코어 스레드용 ftl_io_channel을 dev->ioch에 획득.
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: 코어 스레드도 다른 thread처럼 자기만의 채널을 가져야 GC/wbuf flush
 * 등 내부 잡이 ftl_io_channel.sq/cq 인프라를 사용할 수 있다. spdk_get_io_channel은
 * 처음 호출이면 io_channel_create_cb를 동기 호출.
 *
 * 동작: spdk_get_io_channel(dev) → 코어 스레드의 ftl_io_channel 본체 생성됨 →
 *      dev->ioch에 보관. 실패 시 fail_step.
 *
 * 실행 컨텍스트: 코어 스레드(현재 step 실행 thread).
 */
void
ftl_mngt_init_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->ioch = spdk_get_io_channel(dev);
	/* [한국어] 코어 스레드용 ioch 획득(create_cb 동기 호출). */
	if (!dev->ioch) {
		/* [한국어] 채널 생성 실패(메모리 부족 등). 롤백 트리거. */
		FTL_ERRLOG(dev, "Unable to get IO channel for core thread");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
	/* [한국어] 채널 OK — 다음 step. */
}

/*
 * [한국어]
 * ftl_mngt_deinit_io_channel - 코어 스레드의 ioch 반환.
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 동작: dev->ioch != NULL이면 spdk_put_io_channel — refcount가 0되면 destroy_cb 호출되어
 *      자원 해제 메시지가 코어로 전송. NULL이면 그냥 skip.
 *      (실제 자원 해제는 비동기지만 본 step은 즉시 next로 진행 — 이후 다른 step과 race 없음.)
 */
void
ftl_mngt_deinit_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (dev->ioch) {
		spdk_put_io_channel(dev->ioch);
		/* [한국어] 코어 ioch 참조 해제 — 마지막 참조였으면 destroy_cb 트리거. */
		dev->ioch = NULL;
		/* [한국어] dangling pointer 방지. */
	}

	ftl_mngt_next_step(mngt);
	/* [한국어] 다음 step 진행. */
}
