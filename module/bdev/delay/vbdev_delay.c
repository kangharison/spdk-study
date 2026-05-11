/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] delay(지연 주입) 가상 bdev 모듈 본체 (vbdev_delay.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK delay vbdev 모듈을 구현한다. delay는 base bdev 위에 한 층을 덧대어 모든 I/O에
 * 사용자 지정 지연(평균/p99, READ/WRITE 별)을 주입하는 가상 디바이스로, NVMe-oF/iSCSI initiator
 * 측의 타임아웃·QoS 정책을 검증하는 데 사용된다. 구조는 passthru와 매우 비슷하지만, base 완료
 * 시점에 즉시 클라이언트에 응답하지 않고 채널별 4개 STAILQ(avg_read/avg_write/p99_read/p99_write)에
 * "completion_tick = now + 지연" 항목을 큐잉하여, per-channel 폴러(_delay_finish_io)가 매 tick마다
 * 만료된 I/O만 클라이언트에 완료한다.
 * P99 분류는 채널별 rand_seed 기반의 1/100 확률로 결정된다(`rand_r(&rand_seed) % 100 == 0`).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [bdev_delay_create RPC] → vbdev_delay_rpc.c → create_delay_disk (이 파일)
 *                                                  → vbdev_delay_insert_association (전역 매핑 등록)
 *                                                  → vbdev_delay_register
 *                                                       → spdk_bdev_open_ext
 *                                                       → spdk_io_device_register
 *                                                       → spdk_bdev_module_claim_bdev
 *                                                       → spdk_bdev_register.
 *   [examine 시] base 등록 → vbdev_delay_examine → vbdev_delay_register.
 *   [I/O 경로]
 *     submit: 클라이언트 → bdev 코어 → vbdev_delay_submit_request
 *             → io_type별 spdk_bdev_*_blocks_ext (cb=_delay_complete_io).
 *     base 완료: _delay_complete_io
 *                → 채널의 4개 STAILQ 중 하나에 (io_ctx, completion_tick) 큐잉.
 *     polling: 채널 폴러 _delay_finish_io
 *                → spdk_get_ticks() 비교로 만료된 항목을 spdk_bdev_io_complete.
 *     RESET: spdk_for_each_channel(vbdev_delay_reset_channel) — 각 채널의 큐를 abort.
 *     ABORT: vbdev_delay_abort — 4개 큐에서 검색 후 abort, 못 찾으면 base에 위임.
 *     update_latency: vbdev_delay_update_latency_value (RPC) — atomic 1-store로 갱신.
 * 실행 컨텍스트:
 *   - 등록/RPC: 메인 reactor.
 *   - I/O submit/complete/poller: 채널의 reactor (thread-affinity).
 *   - reset/abort: spdk_for_each_channel을 통해 모든 채널 thread를 순회.
 *
 * === 타 모듈과의 연결 ===
 * - include: spdk/stdinc.h, vbdev_delay.h, spdk/rpc.h, spdk/env.h, spdk/endian.h, spdk/string.h,
 *   spdk/thread.h(poller), spdk/util.h, spdk/bdev_module.h, spdk/log.h.
 * - 의존: lib/bdev (open/io API), lib/thread (channel/poller/send_msg).
 * - 데이터 흐름: 클라이언트 → delay vbdev (지연 큐) → base bdev → 응답.
 *
 * === 주요 함수/구조체 요약 ===
 * - `struct bdev_association`         : (vbdev_name, bdev_name, uuid, 4개 지연값) — 등록 대기 매핑.
 * - `struct vbdev_delay`              : 한 vbdev 인스턴스 — base + 4개 지연 ticks + open thread.
 * - `struct delay_bdev_io`            : per-IO 컨텍스트 — completion_tick, type, zcopy 보조.
 * - `struct delay_io_channel`         : per-thread 채널 — base_ch + 4개 STAILQ + 폴러 + rand_seed.
 * - `vbdev_delay_submit_request()`    : I/O 진입점 — io_type별 base API 호출 + p99 결정.
 * - `_delay_complete_io()`            : base 완료 시 호출 — 채널 큐에 적재.
 * - `_delay_finish_io()`              : 폴러 — 만료 I/O를 클라이언트에 완료.
 * - `vbdev_delay_update_latency_value()` : 가동 중 지연 파라미터 갱신.
 */

/* [한국어] POSIX 표준 헤더 모음. */
#include "spdk/stdinc.h"

/* [한국어] delay 모듈 공개 헤더 — RPC가 사용하는 함수 선언과 enum delay_io_type. */
#include "vbdev_delay.h"
/* [한국어] RPC 매크로(이 파일은 직접 등록 없음). */
#include "spdk/rpc.h"
/* [한국어] DPDK 환경(현재 미사용). */
#include "spdk/env.h"
/* [한국어] 엔디안 매크로(현재 미사용). */
#include "spdk/endian.h"
/* [한국어] spdk_uuid_parse 등. */
#include "spdk/string.h"
/* [한국어] spdk_get_thread, spdk_thread_send_msg, SPDK_POLLER_REGISTER. */
#include "spdk/thread.h"
/* [한국어] SPDK_CONTAINEROF 등 매크로. */
#include "spdk/util.h"

/* [한국어] bdev 모듈 작성 핵심 API. */
#include "spdk/bdev_module.h"
/* [한국어] 로깅 매크로. */
#include "spdk/log.h"

/* This namespace UUID was generated using uuid_generate() method. */
/* [한국어] delay vbdev이 자동으로 UUID를 생성할 때 사용하는 namespace UUID(SHA-1 시드).
 *           UUIDv5: ns + base->uuid → 결정적 vbdev UUID. */
#define BDEV_DELAY_NAMESPACE_UUID "4009b574-6430-4f1b-bc40-ace811091027"

/* [한국어] 모듈 콜백 전방 선언. */
static int vbdev_delay_init(void);
static int vbdev_delay_get_ctx_size(void);
static void vbdev_delay_examine(struct spdk_bdev *bdev);
static void vbdev_delay_finish(void);
static int vbdev_delay_config_json(struct spdk_json_write_ctx *w);

/* [한국어] delay 모듈 정의 — passthru와 거의 동일한 콜백 묶음. */
static struct spdk_bdev_module delay_if = {
	.name = "delay",
	.module_init = vbdev_delay_init,
	.get_ctx_size = vbdev_delay_get_ctx_size,
	.examine_config = vbdev_delay_examine,
	.module_fini = vbdev_delay_finish,
	.config_json = vbdev_delay_config_json
};

/* [한국어] 모듈 등록. */
SPDK_BDEV_MODULE_REGISTER(delay, &delay_if)

/* Associative list to be used in examine */
/* [한국어] (vbdev_name, bdev_name, 4개 지연, uuid) 매핑 — 등록 대기 + 활성 항목 모두 보관. */
struct bdev_association {
	char			*vbdev_name;
	/* [한국어] 새로 만들 vbdev 이름.
	 * 설정자: vbdev_delay_insert_association. 읽는 자: register/examine.
	 * 동기화: g_bdev_associations는 메인 thread에서만 조작. */

	char			*bdev_name;
	/* [한국어] base bdev 이름. */

	struct spdk_uuid	uuid;
	/* [한국어] vbdev에 부여할 UUID(0이면 자동 생성). */

	uint64_t		avg_read_latency;
	/* [한국어] 평균 읽기 지연(us). register 시 ticks로 변환되어 vbdev에 저장.
	 * 동기화: 매핑 단계에서는 단순 보관. */

	uint64_t		p99_read_latency;
	/* [한국어] p99 읽기 지연(us). */

	uint64_t		avg_write_latency;
	/* [한국어] 평균 쓰기 지연(us). */

	uint64_t		p99_write_latency;
	/* [한국어] p99 쓰기 지연(us). */

	TAILQ_ENTRY(bdev_association)	link;
	/* [한국어] g_bdev_associations 연결 노드. */
};
/* [한국어] 모든 delay 매핑의 전역 TAILQ. */
static TAILQ_HEAD(, bdev_association) g_bdev_associations = TAILQ_HEAD_INITIALIZER(
			g_bdev_associations);

/* List of virtual bdevs and associated info for each. */
/* [한국어] 한 delay vbdev 인스턴스의 컨텍스트 — delay_bdev.ctxt에 저장. */
struct vbdev_delay {
	struct spdk_bdev		*base_bdev; /* the thing we're attaching to */
	/* [한국어] base bdev 포인터.
	 * 설정자: register. 읽는 자: submit, dump, get_memory_domains. */

	struct spdk_bdev_desc		*base_desc; /* its descriptor we get from open */
	/* [한국어] base bdev 디스크립터 — I/O 발행에 사용.
	 * 동기화: open한 thread에서만 close. */

	struct spdk_bdev		delay_bdev;    /* the delay virtual bdev */
	/* [한국어] 본 vbdev 본체. spdk_bdev_register에 직접 전달. */

	uint64_t			average_read_latency_ticks; /* the average read delay */
	/* [한국어] 평균 읽기 지연(ticks 단위 — spdk_get_ticks_hz()/SPDK_SEC_TO_USEC * us).
	 * 설정자: register 초기값 + update_latency_value (RPC).
	 * 읽는 자: _delay_complete_io가 completion_tick 계산에 사용.
	 * 동기화: 단일 64비트 store는 x86에서 atomic — 별도 락 없음. */

	uint64_t			p99_read_latency_ticks; /* the p99 read delay */
	/* [한국어] p99 읽기 지연(ticks). */

	uint64_t			average_write_latency_ticks; /* the average write delay */
	/* [한국어] 평균 쓰기 지연(ticks). */

	uint64_t			p99_write_latency_ticks; /* the p99 write delay */
	/* [한국어] p99 쓰기 지연(ticks). */

	TAILQ_ENTRY(vbdev_delay)	link;
	/* [한국어] g_delay_nodes 연결 노드. */

	struct spdk_thread		*thread;    /* thread where base device is opened */
	/* [한국어] base를 open한 thread — close는 동일 thread에서 해야 함. */
};
/* [한국어] 모든 delay 인스턴스의 전역 TAILQ. */
static TAILQ_HEAD(, vbdev_delay) g_delay_nodes = TAILQ_HEAD_INITIALIZER(g_delay_nodes);

/* [한국어] per-IO 컨텍스트 — bdev_io->driver_ctx에 보관. */
struct delay_bdev_io {
	int status;
	/* [한국어] base I/O 결과 상태(SPDK_BDEV_IO_STATUS_*). 폴러가 클라이언트에 그대로 보고. */

	uint64_t completion_tick;
	/* [한국어] 클라이언트에 응답할 시점(ticks). spdk_get_ticks() >= completion_tick이면 완료.
	 * 설정자: _delay_complete_io. 읽는 자: _process_io_stailq. */

	enum delay_io_type type;
	/* [한국어] 본 I/O가 큐잉될 채널 — DELAY_AVG_READ/P99_READ/AVG_WRITE/P99_WRITE/NONE.
	 * NONE은 지연 없이 즉시 완료. */

	struct spdk_io_channel *ch;
	/* [한국어] 채널 보관 — 재시도(ENOMEM)와 _delay_complete_io에서 사용. */

	struct spdk_bdev_io_wait_entry bdev_io_wait;
	/* [한국어] ENOMEM 시 wait 큐 등록용. */

	struct spdk_bdev_io *zcopy_bdev_io;
	/* [한국어] ZCOPY는 start와 end가 분리된 두 단계 — start의 base bdev_io를 보관해 end 단계에서 사용.
	 * abort 시에는 spdk_bdev_zcopy_end로 명시적 종료(commit=false). */

	STAILQ_ENTRY(delay_bdev_io) link;
	/* [한국어] 4개 지연 큐 중 하나에 연결될 노드(현재 어느 큐에 있는지는 type으로 추적). */
};

/* [한국어] per-thread 채널 — base 채널 + 4개 지연 STAILQ + 폴러 + 랜덤 시드. */
struct delay_io_channel {
	struct spdk_io_channel	*base_ch; /* IO channel of base device */
	/* [한국어] base bdev 채널. submit_request가 모든 base API의 채널 인자로 사용. */

	STAILQ_HEAD(, delay_bdev_io) avg_read_io;
	/* [한국어] 평균 지연 적용된 READ 큐. FIFO 순서로 만료. */

	STAILQ_HEAD(, delay_bdev_io) p99_read_io;
	/* [한국어] p99 지연 적용 READ 큐. */

	STAILQ_HEAD(, delay_bdev_io) avg_write_io;
	/* [한국어] 평균 지연 WRITE 큐. */

	STAILQ_HEAD(, delay_bdev_io) p99_write_io;
	/* [한국어] p99 지연 WRITE 큐. */

	struct spdk_poller *io_poller;
	/* [한국어] 채널 등록 시 _delay_finish_io를 0us 주기로 등록 — 매 reactor 루프에서 호출.
	 * SPDK_POLLER_BUSY/IDLE 반환으로 reactor가 polling 빈도 조정. */

	unsigned int rand_seed;
	/* [한국어] p99 분류용 랜덤 시드 — submit 시 rand_r(&rand_seed) % 100 == 0 로 1% 확률 결정.
	 * 채널마다 독립이며 단일 thread만 사용하므로 락 불필요. */
};

/* [한국어] 전방 선언 — 재시도 콜백에서 호출. */
static void vbdev_delay_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);


/* Callback for unregistering the IO device. */
/*
 * [한국어]
 * _device_unregister_cb - io_device 해제 완료 시 호출 — vbdev 메모리 해제.
 *
 * 호출 체인: vbdev_delay_destruct → spdk_io_device_unregister → 모든 채널 close → [이 콜백].
 * 실행 컨텍스트: io device 메인 thread.
 */
static void
_device_unregister_cb(void *io_device)
{
	struct vbdev_delay *delay_node  = io_device;  /* [한국어] io_device 등록 시 등록한 vbdev_delay 자체. */

	/* Done with this delay_node. */
	free(delay_node->delay_bdev.name);  /* [한국어] strdup된 vbdev 이름 해제. */
	free(delay_node);                    /* [한국어] vbdev_delay 본체 해제. */
}

/*
 * [한국어]
 * _vbdev_delay_destruct - thread cross 시 send_msg 디스패치되는 close wrapper.
 *
 * 호출 체인: spdk_thread_send_msg → [이 함수] → spdk_bdev_close.
 */
static void
_vbdev_delay_destruct(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;  /* [한국어] send_msg ctx로 전달된 base 디스크립터. */

	spdk_bdev_close(desc);  /* [한국어] open한 thread에서만 안전한 호출 — wrapper 함수의 존재 이유. */
}

/*
 * [한국어]
 * vbdev_delay_destruct - bdev_fn_table.destruct — 소멸 시퀀스. passthru와 동일.
 *
 * 호출 체인: spdk_bdev_unregister → bdev 코어 → [이 함수] →
 *            (필요 시) send_msg → spdk_bdev_close → spdk_io_device_unregister → free.
 */
static int
vbdev_delay_destruct(void *ctx)
{
	struct vbdev_delay *delay_node = (struct vbdev_delay *)ctx;

	/* It is important to follow this exact sequence of steps for destroying
	 * a vbdev...
	 */

	TAILQ_REMOVE(&g_delay_nodes, delay_node, link);  /* [한국어] 1) 전역 리스트에서 분리. */

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(delay_node->base_bdev);  /* [한국어] 2) base claim 해제. */

	/* Close the underlying bdev on its same opened thread. */
	if (delay_node->thread && delay_node->thread != spdk_get_thread()) {
		/* [한국어] 다른 thread에서 destruct가 일어났으면 send_msg로 open thread에 위임. */
		spdk_thread_send_msg(delay_node->thread, _vbdev_delay_destruct, delay_node->base_desc);
	} else {
		/* [한국어] 동일 thread면 직접 close. */
		spdk_bdev_close(delay_node->base_desc);
	}

	/* Unregister the io_device. */
	/* [한국어] 4) io_device 비동기 해제 — 모든 채널이 닫히면 _device_unregister_cb이 free 수행. */
	spdk_io_device_unregister(delay_node, _device_unregister_cb);

	return 0;
}

/*
 * [한국어]
 * _process_io_stailq - 한 STAILQ에서 만료된 I/O를 클라이언트에 완료.
 *
 * @arg   : STAILQ 헤드. @ticks: 현재 tick.
 * @return: 완료 처리한 I/O 개수.
 *
 * STAILQ는 FIFO이므로 머리가 만료되지 않았으면 그 뒤도 만료되지 않음(같은 지연을 사용한 경우).
 * 단, update_latency로 지연이 동적 변경되면 일시적으로 비-FIFO 만료가 발생 가능 — 코멘트 참조.
 *
 * 호출 체인: _delay_finish_io → [이 함수] → spdk_bdev_io_complete.
 * 실행 컨텍스트: 채널 reactor.
 */
static int
_process_io_stailq(void *arg, uint64_t ticks)
{
	STAILQ_HEAD(, delay_bdev_io) *head = arg;  /* [한국어] 4개 STAILQ 중 하나의 헤드. */
	struct delay_bdev_io *io_ctx, *tmp;        /* [한국어] SAFE 순회용 — 본 루프가 노드를 제거하므로 tmp 필요. */
	int completions = 0;                        /* [한국어] 본 호출에서 완료시킨 I/O 수. 반환값으로 사용. */

	/* [한국어] 큐 머리부터 만료 시점을 검사. spdk_bdev_io_from_ctx는 driver_ctx로부터 bdev_io 컨테이너 복원. */
	STAILQ_FOREACH_SAFE(io_ctx, head, link, tmp) {
		if (io_ctx->completion_tick <= ticks) {
			/* [한국어] 만료 — 큐에서 분리하고 클라이언트에 완료 통지. */
			STAILQ_REMOVE(head, io_ctx, delay_bdev_io, link);
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io_ctx), io_ctx->status);
			completions++;
		} else {
			/* In the general case, I/O will become ready in an fifo order. When timeouts are dynamically
			 * changed, this is not necessarily the case. However, the normal behavior will be restored
			 * after the outstanding I/O at the time of the change have been completed.
			 * This essentially means that moving from a high to low latency creates a dam for the new I/O
			 * submitted after the latency change. This is considered desirable behavior for the use case where
			 * we are trying to trigger a pre-defined timeout on an initiator.
			 */
			/* [한국어] 머리가 미만료면 같은 큐 뒤도 미만료 — 일찍 종료하여 O(만료개수) 처리.
			 *           단 update_latency로 큐 중간 지연이 길어지는 일은 없으나 짧아지는 경우는
			 *           기존 high-latency I/O가 "댐" 역할 — 이는 의도된 동작(initiator 타임아웃 시뮬레이션). */
			break;
		}
	}

	return completions;
}

/*
 * [한국어]
 * _delay_finish_io - 채널 폴러 — 4개 지연 큐를 매 reactor 루프마다 점검.
 *
 * @arg   : delay_io_channel 포인터.
 * @return: SPDK_POLLER_BUSY(처리한 I/O 있음) / SPDK_POLLER_IDLE(없음). reactor가 폴링 빈도 결정.
 *
 * 호출 체인: SPDK_POLLER_REGISTER → reactor 루프 → [이 함수] → _process_io_stailq × 4.
 * 실행 컨텍스트: 채널 reactor.
 */
static int
_delay_finish_io(void *arg)
{
	struct delay_io_channel *delay_ch = arg;  /* [한국어] SPDK_POLLER_REGISTER 시 등록된 ctx — 채널 본체. */
	uint64_t ticks = spdk_get_ticks();   /* [한국어] 현재 TSC tick — DPDK rdtsc 기반. 호출 1회로 4개 큐 모두 처리. */
	int completions = 0;                 /* [한국어] 본 호출 동안 완료시킨 I/O 총합 — IDLE/BUSY 결정용. */

	/* [한국어] 4개 큐 모두 한 번씩 처리 — 우선순위 없이 순차적으로 만료 검사.
	 *           실제로는 각 큐가 같은 지연을 공유하므로 큐 간 만료 순서가 다를 수 있음. */
	completions += _process_io_stailq(&delay_ch->avg_read_io, ticks);
	completions += _process_io_stailq(&delay_ch->avg_write_io, ticks);
	completions += _process_io_stailq(&delay_ch->p99_read_io, ticks);
	completions += _process_io_stailq(&delay_ch->p99_write_io, ticks);

	/* [한국어] BUSY 반환 시 reactor는 본 poller가 일을 하고 있다고 판단해 polling 빈도 유지.
	 *           IDLE이 누적되면 power management/sleep 모드 진입(low power thread). */
	return completions == 0 ? SPDK_POLLER_IDLE : SPDK_POLLER_BUSY;
}

/* Completion callback for IO that were issued from this bdev. The original bdev_io
 * is passed in as an arg so we'll complete that one with the appropriate status
 * and then free the one that this module issued.
 */
/*
 * [한국어]
 * _delay_complete_io - base I/O 완료 시 호출되는 콜백 — 결과를 큐에 적재.
 *
 * @bdev_io: 우리가 base에 발행한 새 bdev_io. @success: 성공 여부. @cb_arg: 원래 bdev_io.
 *
 * 동작:
 *   1) base 결과 상태를 io_ctx->status에 보관(클라이언트에 그대로 회신할 값).
 *   2) ZCOPY start의 경우 zcopy_bdev_io를 보존(end 단계에서 spdk_bdev_zcopy_end에 사용).
 *      그 외에는 spdk_bdev_free_io로 base bdev_io 해제.
 *   3) io_ctx->type에 따라 4개 STAILQ 중 하나에 (completion_tick) 적재.
 *      DELAY_NONE이면 즉시 spdk_bdev_io_complete (지연 없음 — RESET/ABORT 같은 관리 I/O).
 *
 * 호출 체인: spdk_bdev_*_blocks_ext (cb=_delay_complete_io) → base 완료 → [이 콜백].
 * 실행 컨텍스트: 채널 reactor.
 */
static void
_delay_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;                                                   /* [한국어] submit 시 cb_arg로 전달한 원본 bdev_io 복원. */
	struct vbdev_delay *delay_node = SPDK_CONTAINEROF(orig_io->bdev, struct vbdev_delay, delay_bdev);  /* [한국어] vbdev → vbdev_delay 컨테이너 매크로(offsetof 기반). */
	struct delay_bdev_io *io_ctx = (struct delay_bdev_io *)orig_io->driver_ctx;              /* [한국어] driver_ctx에 저장된 per-IO 컨텍스트(ch, type 등). */
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(io_ctx->ch);                 /* [한국어] submit 단계에서 보관한 ch에서 채널 ctx 복원. */

	io_ctx->status = spdk_bdev_io_set_base_io_status(orig_io, bdev_io);  /* [한국어] base 상태를 orig에 set + 반환. */

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_ZCOPY && bdev_io->u.bdev.zcopy.start && success) {
		/* [한국어] ZCOPY start — base가 매핑한 핸들을 보관. end 단계에서 사용. */
		io_ctx->zcopy_bdev_io = bdev_io;
	} else {
		/* [한국어] 일반 I/O 또는 ZCOPY end — 우리가 발행한 base bdev_io를 해제. */
		assert(io_ctx->zcopy_bdev_io == NULL || io_ctx->zcopy_bdev_io == bdev_io);
		io_ctx->zcopy_bdev_io = NULL;
		spdk_bdev_free_io(bdev_io);
	}

	/* Put the I/O into the proper list for processing by the channel poller. */
	switch (io_ctx->type) {
	case DELAY_AVG_READ:
		/* [한국어] 만료 시각 = 현재 + 평균 READ 지연. avg_read_io 큐 끝에 추가. */
		io_ctx->completion_tick = spdk_get_ticks() + delay_node->average_read_latency_ticks;
		STAILQ_INSERT_TAIL(&delay_ch->avg_read_io, io_ctx, link);
		break;
	case DELAY_AVG_WRITE:
		io_ctx->completion_tick = spdk_get_ticks() + delay_node->average_write_latency_ticks;
		STAILQ_INSERT_TAIL(&delay_ch->avg_write_io, io_ctx, link);
		break;
	case DELAY_P99_READ:
		io_ctx->completion_tick = spdk_get_ticks() + delay_node->p99_read_latency_ticks;
		STAILQ_INSERT_TAIL(&delay_ch->p99_read_io, io_ctx, link);
		break;
	case DELAY_P99_WRITE:
		io_ctx->completion_tick = spdk_get_ticks() + delay_node->p99_write_latency_ticks;
		STAILQ_INSERT_TAIL(&delay_ch->p99_write_io, io_ctx, link);
		break;
	case DELAY_NONE:
	default:
		/* [한국어] 지연 없음(예: RESET) — 즉시 클라이언트에 완료. */
		spdk_bdev_io_complete(orig_io, io_ctx->status);
		break;
	}
}

/*
 * [한국어]
 * vbdev_delay_resubmit_io - bdev_io_wait에서 깨어나 재발행.
 */
static void
vbdev_delay_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;                   /* [한국어] wait 엔트리에 cb_arg로 저장한 bdev_io. */
	struct delay_bdev_io *io_ctx = (struct delay_bdev_io *)bdev_io->driver_ctx;  /* [한국어] 저장된 ch 복원용. */

	/* [한국어] 동일 channel + 동일 bdev_io로 submit_request 재진입. is_p99/type은 재계산되므로 결정성 없음(설계상 OK). */
	vbdev_delay_submit_request(io_ctx->ch, bdev_io);
}

/*
 * [한국어]
 * vbdev_delay_queue_io - ENOMEM 시 wait 큐 등록.
 */
static void
vbdev_delay_queue_io(struct spdk_bdev_io *bdev_io)
{
	struct delay_bdev_io *io_ctx = (struct delay_bdev_io *)bdev_io->driver_ctx;  /* [한국어] driver_ctx 내부의 wait 엔트리 사용. */
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(io_ctx->ch);     /* [한국어] base 채널 추출용. */
	int rc;                                                                       /* [한국어] queue_io_wait 결과 — 0이면 큐잉 성공. */

	/* [한국어] wait 엔트리 채우기 — base가 자원 회복 시 cb_fn(cb_arg) 호출하도록 약속. */
	io_ctx->bdev_io_wait.bdev = bdev_io->bdev;                /* [한국어] 어느 bdev에 대한 대기인지 명시. */
	io_ctx->bdev_io_wait.cb_fn = vbdev_delay_resubmit_io;     /* [한국어] 자원 회복 시 호출될 콜백. */
	io_ctx->bdev_io_wait.cb_arg = bdev_io;                    /* [한국어] 콜백 인자 — 재발행할 bdev_io. */

	/* [한국어] base 채널의 wait 큐에 등록. base bdev 모듈은 자원 회복 시 큐를 비우며 cb_fn을 호출. */
	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, delay_ch->base_ch, &io_ctx->bdev_io_wait);
	if (rc != 0) {
		/* [한국어] 큐잉 자체가 실패한 비정상 케이스 — 클라이언트에 즉시 실패 통지. */
		SPDK_ERRLOG("Queue io failed in vbdev_delay_queue_io, rc=%d.\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * [한국어]
 * delay_init_ext_io_opts - extended IO opts 초기화. passthru 버전과 거의 동일하나 DIF 플래그 미포함.
 */
static void
delay_init_ext_io_opts(struct spdk_bdev_io *bdev_io, struct spdk_bdev_ext_io_opts *opts)
{
	memset(opts, 0, sizeof(*opts));                                /* [한국어] 모든 옵션 0으로 초기화 — 이후 명시 필드만 채움. */
	opts->size = sizeof(*opts);                                    /* [한국어] ABI 호환을 위한 size 필드(구버전 SPDK 호환). */
	opts->memory_domain = bdev_io->u.bdev.memory_domain;           /* [한국어] DPDK 외 메모리 도메인(GPU 등)이면 그대로 forwarding. */
	opts->memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;   /* [한국어] 도메인별 보조 컨텍스트. */
	opts->metadata = bdev_io->u.bdev.md_buf;                       /* [한국어] DIF/DIX 메타 버퍼 포인터. passthru와 달리 dif_check_flags는 미설정. */
}

/*
 * [한국어]
 * delay_read_get_buf_cb - READ 버퍼 확보 후 base에 readv 발행.
 *
 * 호출 체인: spdk_bdev_io_get_buf → 풀 → [이 콜백] → spdk_bdev_readv_blocks_ext.
 */
static void
delay_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct vbdev_delay *delay_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_delay,
					 delay_bdev);                                /* [한국어] vbdev → vbdev_delay 복원. */
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(ch);                 /* [한국어] 채널 ctx 추출. */
	struct spdk_bdev_ext_io_opts io_opts;                                            /* [한국어] base에 전달할 확장 옵션. */
	int rc;                                                                          /* [한국어] readv 결과 — 0/ENOMEM/기타. */

	if (!success) {
		/* [한국어] 버퍼 풀 고갈 — read 진행 불가, 클라이언트에 즉시 FAILED 통지. */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	delay_init_ext_io_opts(bdev_io, &io_opts);                                       /* [한국어] 메모리 도메인/메타 옵션 채움. */
	/* [한국어] base에 readv 발행. iovs/offset/num은 원본 그대로. cb=_delay_complete_io로 완료 시 큐 적재.
	 *           cb_arg=bdev_io로 콜백에서 orig_io 복원. */
	rc = spdk_bdev_readv_blocks_ext(delay_node->base_desc, delay_ch->base_ch, bdev_io->u.bdev.iovs,
					bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
					bdev_io->u.bdev.num_blocks, _delay_complete_io,
					bdev_io, &io_opts);

	if (rc == -ENOMEM) {
		/* [한국어] base 자원 부족 — wait 큐에 등록 후 재시도 대기. */
		SPDK_ERRLOG("No memory, start to queue io for delay.\n");
		vbdev_delay_queue_io(bdev_io);
	} else if (rc != 0) {
		/* [한국어] 기타 에러(예: -EINVAL) — 회복 불가, 즉시 FAILED. */
		SPDK_ERRLOG("ERROR on bdev_io submission!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * [한국어]
 * vbdev_delay_reset_dev - 모든 채널 abort 완료 후 base에 reset 발행.
 *
 * @i     : iterator (ctx로 reset bdev_io 보관). @status: spdk_for_each_channel 결과(미사용).
 *
 * 호출 체인: spdk_for_each_channel(reset_channel, reset_dev) → 모든 채널 abort 완료 → [이 함수]
 *            → spdk_bdev_reset(base) → _delay_complete_io.
 * 실행 컨텍스트: spdk_for_each_channel을 시작한 thread.
 */
static void
vbdev_delay_reset_dev(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_bdev_io *bdev_io = spdk_io_channel_iter_get_ctx(i);             /* [한국어] for_each_channel 시작 시 ctx로 등록된 reset bdev_io. */
	struct delay_bdev_io *io_ctx = (struct delay_bdev_io *)bdev_io->driver_ctx; /* [한국어] driver_ctx → io_ctx. */
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(io_ctx->ch);    /* [한국어] reset을 트리거한 채널의 base 채널 사용. */
	struct vbdev_delay *delay_node = spdk_io_channel_iter_get_io_device(i);     /* [한국어] iterator가 등록된 io_device(vbdev_delay). */
	int rc;                                                                      /* [한국어] reset 결과. */

	/* [한국어] 모든 채널 abort가 끝났으므로 이제 base에 실제 reset 발행 — 완료 시 _delay_complete_io로 라우팅. */
	rc = spdk_bdev_reset(delay_node->base_desc, delay_ch->base_ch,
			     _delay_complete_io, bdev_io);

	if (rc == -ENOMEM) {
		/* [한국어] reset도 base 자원 부족할 수 있음 — 재시도 대기. */
		SPDK_ERRLOG("No memory, start to queue io for delay.\n");
		vbdev_delay_queue_io(bdev_io);
	} else if (rc != 0) {
		/* [한국어] 회복 불가 에러 — reset 자체 실패 통지. */
		SPDK_ERRLOG("ERROR on bdev_io submission!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * [한국어]
 * abort_zcopy_io - ZCOPY end의 abort 완료 콜백 — 단순 free.
 *
 * 호출 체인: _abort_all_delayed_io / abort_delayed_io → spdk_bdev_zcopy_end(commit=false) → [이 콜백].
 */
static void
abort_zcopy_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	/* [한국어] zcopy_end(commit=false) 완료 시 호출. abort 경로에서는 결과를 검사할 필요가 없음 —
	 *           base bdev_io만 해제하면 충분(원본 bdev_io는 호출자가 따로 ABORTED 처리). */
	spdk_bdev_free_io(bdev_io);
}

/*
 * [한국어]
 * _abort_all_delayed_io - 한 STAILQ의 모든 항목을 abort 처리.
 *
 * @arg: STAILQ 헤드.
 *
 * ZCOPY가 진행 중이었으면 spdk_bdev_zcopy_end로 명시적 종료(commit=false → 변경 폐기).
 * 호출 체인: vbdev_delay_reset_channel → [이 함수].
 */
static void
_abort_all_delayed_io(void *arg)
{
	STAILQ_HEAD(, delay_bdev_io) *head = arg;  /* [한국어] 4개 STAILQ 중 하나의 헤드. */
	struct delay_bdev_io *io_ctx, *tmp;        /* [한국어] SAFE 순회 — 본 루프가 노드 제거. */

	/* [한국어] 큐에 남은 모든 I/O를 ABORTED 상태로 즉시 완료 — reset 경로의 cleanup. */
	STAILQ_FOREACH_SAFE(io_ctx, head, link, tmp) {
		STAILQ_REMOVE(head, io_ctx, delay_bdev_io, link);  /* [한국어] 큐에서 분리. */
		if (io_ctx->zcopy_bdev_io != NULL) {
			/* [한국어] 진행 중인 ZCOPY는 commit=false로 종료해 base 자원 회수. */
			spdk_bdev_zcopy_end(io_ctx->zcopy_bdev_io, false, abort_zcopy_io, NULL);
		}
		/* [한국어] 클라이언트에 ABORTED 상태로 완료 통지(io_ctx->status는 무시). */
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io_ctx), SPDK_BDEV_IO_STATUS_ABORTED);
	}
}

/*
 * [한국어]
 * vbdev_delay_reset_channel - 한 채널의 4개 지연 큐를 모두 abort.
 *
 * 호출 체인: vbdev_delay_submit_request(RESET) → spdk_for_each_channel → [이 함수].
 * 실행 컨텍스트: 각 채널 reactor.
 */
static void
vbdev_delay_reset_channel(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);  /* [한국어] 현재 순회 중인 채널 핸들. */
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(ch);   /* [한국어] 채널 ctx — 4개 STAILQ 접근용. */

	/* [한국어] 채널 내 모든 지연 큐 비우기 — 본 함수는 spdk_for_each_channel로 각 reactor에서 실행된다. */
	_abort_all_delayed_io(&delay_ch->avg_read_io);
	_abort_all_delayed_io(&delay_ch->avg_write_io);
	_abort_all_delayed_io(&delay_ch->p99_read_io);
	_abort_all_delayed_io(&delay_ch->p99_write_io);

	spdk_for_each_channel_continue(i, 0);  /* [한국어] 다음 채널 진행. 마지막 채널 이후 cpl_fn=vbdev_delay_reset_dev 호출. */
}

/*
 * [한국어]
 * abort_delayed_io - 한 STAILQ에서 특정 I/O를 검색해 abort.
 *
 * @_head: 큐 헤드. @bio_to_abort: 대상 I/O.
 * @return: true=찾아서 abort, false=없음.
 *
 * 호출 체인: vbdev_delay_abort → [이 함수].
 */
static bool
abort_delayed_io(void *_head, struct spdk_bdev_io *bio_to_abort)
{
	STAILQ_HEAD(, delay_bdev_io) *head = _head;                                              /* [한국어] 검색할 큐 헤드. */
	struct delay_bdev_io *io_ctx_to_abort = (struct delay_bdev_io *)bio_to_abort->driver_ctx; /* [한국어] 대상 I/O의 driver_ctx — 큐 노드와 비교용. */
	struct delay_bdev_io *io_ctx;                                                            /* [한국어] 순회용. */

	/* [한국어] 포인터 직접 비교 — driver_ctx는 bdev_io에 내장된 메모리이므로 주소가 유일 식별자. */
	STAILQ_FOREACH(io_ctx, head, link) {
		if (io_ctx == io_ctx_to_abort) {
			/* [한국어] 일치 — 큐에서 분리 + ZCOPY 정리 + ABORTED 완료. */
			STAILQ_REMOVE(head, io_ctx_to_abort, delay_bdev_io, link);
			if (io_ctx->zcopy_bdev_io != NULL) {
				/* [한국어] ZCOPY 진행 중이었으면 commit=false로 종료 — 변경분 폐기. */
				spdk_bdev_zcopy_end(io_ctx->zcopy_bdev_io, false, abort_zcopy_io, NULL);
			}
			spdk_bdev_io_complete(bio_to_abort, SPDK_BDEV_IO_STATUS_ABORTED);  /* [한국어] 클라이언트에 즉시 ABORTED 통지. */
			return true;  /* [한국어] 찾았음 — 호출자가 SUCCESS 응답하도록 시그널. */
		}
	}

	return false;  /* [한국어] 본 큐에 없음 — 다음 큐 검색 또는 base에 위임. */
}

/*
 * [한국어]
 * vbdev_delay_abort - SPDK_BDEV_IO_TYPE_ABORT 처리 — 4개 지연 큐 검색 후 base에 위임.
 *
 * @return: 0=찾아서 abort 또는 base abort 발행, 음수=base abort 실패.
 *
 * 호출 체인: vbdev_delay_submit_request(ABORT) → [이 함수].
 */
static int
vbdev_delay_abort(struct vbdev_delay *delay_node, struct delay_io_channel *delay_ch,
		  struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_io *bio_to_abort = bdev_io->u.abort.bio_to_abort;  /* [한국어] ABORT 명령의 대상 I/O — abort_io 유니온 멤버. */

	/* [한국어] 4개 지연 큐 순서대로 검색 — short-circuit OR. */
	if (abort_delayed_io(&delay_ch->avg_read_io, bio_to_abort) ||
	    abort_delayed_io(&delay_ch->avg_write_io, bio_to_abort) ||
	    abort_delayed_io(&delay_ch->p99_read_io, bio_to_abort) ||
	    abort_delayed_io(&delay_ch->p99_write_io, bio_to_abort)) {
		/* [한국어] 우리 큐에서 찾았으면 abort 자체는 즉시 SUCCESS. */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	}

	/* [한국어] 우리 큐에 없으면 base에서 진행 중일 가능성 — base에 abort 위임. */
	return spdk_bdev_abort(delay_node->base_desc, delay_ch->base_ch, bio_to_abort,
			       _delay_complete_io, bdev_io);
}

/*
 * [한국어]
 * vbdev_delay_submit_request - bdev_fn_table.submit_request — I/O 진입점.
 *
 * @ch, @bdev_io: 표준.
 *
 * 동작:
 *   1) is_p99 결정 — 1/100 확률로 true.
 *   2) io_ctx 초기 설정(ch, type=NONE, zcopy_bdev_io=NULL).
 *   3) io_type별로:
 *      - READ : type=p99?P99_READ:AVG_READ, get_buf 후 readv.
 *      - WRITE: type=p99?P99_WRITE:AVG_WRITE, writev.
 *      - WRITE_ZEROES/UNMAP/FLUSH: 지연 적용 안 됨(type=NONE 그대로) → 즉시 완료 경로.
 *      - RESET: spdk_for_each_channel로 모든 채널 abort 후 base reset.
 *      - ABORT: vbdev_delay_abort.
 *      - ZCOPY: start/end로 분기. commit/populate에 따라 type 설정.
 *
 * 호출 체인: 클라이언트 → bdev 코어 → [이 함수] → spdk_bdev_*_blocks_ext → base.
 * 실행 컨텍스트: 채널 reactor.
 */
static void
vbdev_delay_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_delay *delay_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_delay, delay_bdev);  /* [한국어] bdev → vbdev_delay 컨테이너. */
	struct delay_io_channel *delay_ch = spdk_io_channel_get_ctx(ch);                                   /* [한국어] 채널 ctx — STAILQ/base_ch 접근. */
	struct delay_bdev_io *io_ctx = (struct delay_bdev_io *)bdev_io->driver_ctx;                        /* [한국어] driver_ctx → per-IO 컨텍스트. */
	struct spdk_bdev_ext_io_opts io_opts;
	int rc = 0;
	bool is_p99;

	/* [한국어] 1/100 확률로 p99 트리거 — rand_r은 thread-safe하지만 시드는 채널 단위 독립. */
	is_p99 = rand_r(&delay_ch->rand_seed) % 100 == 0 ? true : false;

	io_ctx->ch = ch;                /* [한국어] 콜백/재시도용 채널 보관. */
	io_ctx->type = DELAY_NONE;      /* [한국어] 기본값 — 지연 없음. */
	if (bdev_io->type != SPDK_BDEV_IO_TYPE_ZCOPY || bdev_io->u.bdev.zcopy.start) {
		/* [한국어] ZCOPY end가 아닌 경우(start 또는 일반) — zcopy_bdev_io 슬롯 초기화.
		 *           ZCOPY end는 start에서 보관한 zcopy_bdev_io를 그대로 써야 하므로 건드리지 않음. */
		io_ctx->zcopy_bdev_io = NULL;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		/* [한국어] READ — 1% 확률로 P99, 나머지는 AVG. base 발행은 get_buf_cb에서. */
		io_ctx->type = is_p99 ? DELAY_P99_READ : DELAY_AVG_READ;
		/* [한국어] 클라이언트 버퍼 없으면 풀에서 확보 후 delay_read_get_buf_cb 호출 (총 바이트 = num_blocks * blocklen). */
		spdk_bdev_io_get_buf(bdev_io, delay_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* [한국어] WRITE — type 분류 후 즉시 base에 writev. 버퍼는 클라이언트 소유 그대로 forwarding. */
		io_ctx->type = is_p99 ? DELAY_P99_WRITE : DELAY_AVG_WRITE;
		delay_init_ext_io_opts(bdev_io, &io_opts);  /* [한국어] 메모리 도메인/메타 옵션 채움. */
		rc = spdk_bdev_writev_blocks_ext(delay_node->base_desc, delay_ch->base_ch, bdev_io->u.bdev.iovs,
						 bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
						 bdev_io->u.bdev.num_blocks, _delay_complete_io,
						 bdev_io, &io_opts);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		/* [한국어] 메타 명령 — 지연 없이 base에 그대로 forwarding(type=NONE). */
		rc = spdk_bdev_write_zeroes_blocks(delay_node->base_desc, delay_ch->base_ch,
						   bdev_io->u.bdev.offset_blocks,
						   bdev_io->u.bdev.num_blocks,
						   _delay_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(delay_node->base_desc, delay_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _delay_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(delay_node->base_desc, delay_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _delay_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		/* During reset, the generic bdev layer aborts all new I/Os and queues all new resets.
		 * Hence we can simply abort all I/Os delayed to complete.
		 */
		/* [한국어] reset 중에는 bdev 코어가 신규 I/O를 차단하고 reset을 직렬화 — 안전하게 큐 abort.
		 *           각 채널에서 큐 정리 후 마지막에 base reset 발행. */
		spdk_for_each_channel(delay_node, vbdev_delay_reset_channel, bdev_io,
				      vbdev_delay_reset_dev);
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		rc = vbdev_delay_abort(delay_node, delay_ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		/* [한국어] ZCOPY는 commit(=write)/populate(=read)에 따라 type 결정. */
		if (bdev_io->u.bdev.zcopy.commit) {
			io_ctx->type = is_p99 ? DELAY_P99_WRITE : DELAY_AVG_WRITE;
		} else if (bdev_io->u.bdev.zcopy.populate) {
			io_ctx->type = is_p99 ? DELAY_P99_READ : DELAY_AVG_READ;
		}
		if (bdev_io->u.bdev.zcopy.start) {
			/* [한국어] ZCOPY start — base가 매핑한 핸들을 받아 zcopy_bdev_io에 저장(complete 콜백에서). */
			rc = spdk_bdev_zcopy_start(delay_node->base_desc, delay_ch->base_ch,
						   bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						   bdev_io->u.bdev.offset_blocks,
						   bdev_io->u.bdev.num_blocks,
						   bdev_io->u.bdev.zcopy.populate,
						   _delay_complete_io, bdev_io);
		} else {
			/* [한국어] ZCOPY end — start에서 보관한 핸들로 종료. commit=true면 데이터 반영. */
			rc = spdk_bdev_zcopy_end(io_ctx->zcopy_bdev_io, bdev_io->u.bdev.zcopy.commit,
						 _delay_complete_io, bdev_io);
		}
		break;
	default:
		SPDK_ERRLOG("delay: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc == -ENOMEM) {
		/* [한국어] base 자원 부족 — 재시도 큐로. switch 이후 통합 처리. */
		SPDK_ERRLOG("No memory, start to queue io for delay.\n");
		vbdev_delay_queue_io(bdev_io);
	} else if (rc != 0) {
		/* [한국어] 회복 불가 에러 — 즉시 FAILED. */
		SPDK_ERRLOG("ERROR on bdev_io submission!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * [한국어]
 * vbdev_delay_io_type_supported - base에 위임. 지연 모듈은 base가 지원하는 모든 I/O 타입 지원.
 */
static bool
vbdev_delay_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_delay *delay_node = (struct vbdev_delay *)ctx;  /* [한국어] register 시 등록한 ctxt. */

	/* [한국어] base bdev에 직접 질의 — delay 모듈은 데이터 변형 없이 어떤 I/O 타입이든 통과시킴. */
	return spdk_bdev_io_type_supported(delay_node->base_bdev, io_type);
}

/*
 * [한국어]
 * vbdev_delay_get_io_channel - 채널 요청 시 spdk_get_io_channel.
 */
static struct spdk_io_channel *
vbdev_delay_get_io_channel(void *ctx)
{
	struct vbdev_delay *delay_node = (struct vbdev_delay *)ctx;  /* [한국어] register 시 등록한 ctxt. */
	struct spdk_io_channel *delay_ch = NULL;                     /* [한국어] 반환할 채널 — 실패 시 NULL. */

	/* [한국어] 등록된 io_device(=delay_node)에 대한 새 채널을 요청. 채널이 처음 생성되는 thread면
	 *           delay_bdev_ch_create_cb가 동기 호출되어 STAILQ/poller/base_ch 초기화 수행. */
	delay_ch = spdk_get_io_channel(delay_node);

	return delay_ch;
}

/*
 * [한국어]
 * _delay_write_conf_values - dump_info_json / config_json에서 공통 사용하는 필드 출력.
 *
 * ticks → us 역변환: ticks * SPDK_SEC_TO_USEC / spdk_get_ticks_hz().
 *
 * 호출 체인: dump_info_json / config_json → [이 함수].
 */
static void
_delay_write_conf_values(struct vbdev_delay *delay_node, struct spdk_json_write_ctx *w)
{
	struct spdk_uuid *uuid = &delay_node->delay_bdev.uuid;  /* [한국어] vbdev UUID 포인터. */

	/* [한국어] "name":"<vbdev 이름>" — 클라이언트 식별자. */
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&delay_node->delay_bdev));
	/* [한국어] "base_bdev_name":"<base 이름>" — 위임 대상. */
	spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(delay_node->base_bdev));
	if (!spdk_uuid_is_null(uuid)) {
		/* [한국어] 명시 UUID만 출력 — 자동 생성은 base에서 파생되므로 직렬화 불필요. */
		spdk_json_write_named_uuid(w, "uuid", uuid);
	}
	/* [한국어] 4개 지연을 ticks → us로 역변환하여 출력.
	 *           ticks * SEC_TO_USEC / ticks_hz = us 단위 정수. 단순 산술 — 정밀도 손실 가능하지만 us 단위로 충분. */
	spdk_json_write_named_int64(w, "avg_read_latency",
				    delay_node->average_read_latency_ticks * SPDK_SEC_TO_USEC / spdk_get_ticks_hz());
	spdk_json_write_named_int64(w, "p99_read_latency",
				    delay_node->p99_read_latency_ticks * SPDK_SEC_TO_USEC / spdk_get_ticks_hz());
	spdk_json_write_named_int64(w, "avg_write_latency",
				    delay_node->average_write_latency_ticks * SPDK_SEC_TO_USEC / spdk_get_ticks_hz());
	spdk_json_write_named_int64(w, "p99_write_latency",
				    delay_node->p99_write_latency_ticks * SPDK_SEC_TO_USEC / spdk_get_ticks_hz());
}

/*
 * [한국어]
 * vbdev_delay_dump_info_json - bdev_get_bdevs 응답.
 */
static int
vbdev_delay_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_delay *delay_node = (struct vbdev_delay *)ctx;  /* [한국어] register 시 ctxt. */

	spdk_json_write_name(w, "delay");                    /* [한국어] "delay" 키 — bdev_get_bdevs 응답의 driver_specific. */
	spdk_json_write_object_begin(w);                     /* [한국어] '{' 시작. */
	_delay_write_conf_values(delay_node, w);             /* [한국어] name/base/uuid/4개 지연 출력. */
	spdk_json_write_object_end(w);                       /* [한국어] '}' 종료. */

	return 0;  /* [한국어] 성공 항상 0. */
}

/* This is used to generate JSON that can configure this module to its current state. */
/*
 * [한국어]
 * vbdev_delay_config_json - 모듈 레벨 config_json — 모든 delay_node를 RPC 호출 형태로 직렬화.
 */
static int
vbdev_delay_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_delay *delay_node;  /* [한국어] 순회용. */

	/* [한국어] 모든 활성 delay 인스턴스를 RPC 호출 객체로 직렬화 — save_config 시 호출. */
	TAILQ_FOREACH(delay_node, &g_delay_nodes, link) {
		spdk_json_write_object_begin(w);                                       /* [한국어] 한 RPC 호출 '{' 시작. */
		spdk_json_write_named_string(w, "method", "bdev_delay_create");        /* [한국어] 복원 시 호출할 메서드. */
		spdk_json_write_named_object_begin(w, "params");                       /* [한국어] "params":{ */
		_delay_write_conf_values(delay_node, w);                               /* [한국어] 동일 필드 세트로 출력. */
		spdk_json_write_object_end(w);                                         /* [한국어] params 종료. */
		spdk_json_write_object_end(w);                                         /* [한국어] RPC 호출 객체 종료. */
	}
	return 0;
}

/* We provide this callback for the SPDK channel code to create a channel using
 * the channel struct we provided in our module get_io_channel() entry point. Here
 * we get and save off an underlying base channel of the device below us so that
 * we can communicate with the base bdev on a per channel basis.  If we needed
 * our own poller for this vbdev, we'd register it here.
 */
/*
 * [한국어]
 * delay_bdev_ch_create_cb - 채널 생성 콜백 — STAILQ/poller/base 채널/시드 초기화.
 *
 * 호출 체인: spdk_get_io_channel → 등록된 콜백 → [이 함수] → SPDK_POLLER_REGISTER + spdk_bdev_get_io_channel.
 * 실행 컨텍스트: 채널을 요청한 thread.
 */
static int
delay_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct delay_io_channel *delay_ch = ctx_buf;       /* [한국어] spdk_io_device_register 시 size로 지정한 영역. */
	struct vbdev_delay *delay_node = io_device;        /* [한국어] register 시 등록한 io_device. */

	/* [한국어] 4개 지연 큐 헤드 초기화. STAILQ는 FIFO 단방향 — 만료 검사 시 머리에서 꺼냄. */
	STAILQ_INIT(&delay_ch->avg_read_io);
	STAILQ_INIT(&delay_ch->p99_read_io);
	STAILQ_INIT(&delay_ch->avg_write_io);
	STAILQ_INIT(&delay_ch->p99_write_io);

	/* [한국어] 폴러 등록 — 주기 0us는 매 reactor 루프에서 호출(busy-poll 모드).
	 *           SPDK_POLLER_REGISTER 매크로는 spdk_poller_register(_delay_finish_io, delay_ch, 0)로 확장.
	 *           반환된 핸들은 destroy_cb의 unregister에 사용. */
	delay_ch->io_poller = SPDK_POLLER_REGISTER(_delay_finish_io, delay_ch, 0);
	delay_ch->base_ch = spdk_bdev_get_io_channel(delay_node->base_desc);  /* [한국어] base 채널 획득. */
	delay_ch->rand_seed = time(NULL);  /* [한국어] p99 결정용 시드 — 채널마다 독립. */

	return 0;  /* [한국어] 성공 항상 0. */
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created. If this bdev used its own poller, we'd unregister it here.
 */
/*
 * [한국어]
 * delay_bdev_ch_destroy_cb - 채널 해제 — 폴러 unregister + base 채널 반납.
 */
static void
delay_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct delay_io_channel *delay_ch = ctx_buf;  /* [한국어] 해제 대상 채널 ctx. */

	/* [한국어] 폴러 등록 해제 — 다음 reactor 루프부터 _delay_finish_io 미호출. unregister는 즉시 동기.
	 *           이중 포인터 인자로 호출 후 *poller=NULL이 되어 부분 정리 안전. */
	spdk_poller_unregister(&delay_ch->io_poller);
	/* [한국어] base 채널 반납 — refcount-- 후 0이면 base 채널 destroy. */
	spdk_put_io_channel(delay_ch->base_ch);
}

/* Create the delay association from the bdev and vbdev name and insert
 * on the global list. */
/*
 * [한국어]
 * vbdev_delay_insert_association - g_bdev_associations에 매핑 추가.
 *
 * 호출 체인: create_delay_disk → [이 함수].
 */
static int
vbdev_delay_insert_association(const char *bdev_name, const char *vbdev_name,
			       struct spdk_uuid *uuid,
			       uint64_t avg_read_latency, uint64_t p99_read_latency,
			       uint64_t avg_write_latency, uint64_t p99_write_latency)
{
	struct bdev_association *assoc;  /* [한국어] 순회/할당 공용 포인터. */

	/* [한국어] 중복 검사 — vbdev_name은 SPDK 전역에서 유일해야 함. */
	TAILQ_FOREACH(assoc, &g_bdev_associations, link) {
		if (strcmp(vbdev_name, assoc->vbdev_name) == 0) {
			SPDK_ERRLOG("delay bdev %s already exists\n", vbdev_name);
			return -EEXIST;
		}
	}

	/* [한국어] 0-init 할당 — uuid가 미지정(NULL UUID)일 때 spdk_uuid_is_null이 true 반환하도록. */
	assoc = calloc(1, sizeof(struct bdev_association));
	if (!assoc) {
		SPDK_ERRLOG("could not allocate bdev_association\n");
		return -ENOMEM;
	}

	/* [한국어] base bdev 이름 strdup — RPC 입력 수명이 짧을 수 있음. */
	assoc->bdev_name = strdup(bdev_name);
	if (!assoc->bdev_name) {
		SPDK_ERRLOG("could not allocate assoc->bdev_name\n");
		free(assoc);  /* [한국어] 부분 할당 누수 방지. */
		return -ENOMEM;
	}

	/* [한국어] vbdev 이름 strdup. */
	assoc->vbdev_name = strdup(vbdev_name);
	if (!assoc->vbdev_name) {
		SPDK_ERRLOG("could not allocate assoc->vbdev_name\n");
		free(assoc->bdev_name);  /* [한국어] 역순 해제. */
		free(assoc);
		return -ENOMEM;
	}

	/* [한국어] 4개 지연 값(us)을 그대로 보관 — register 시 ticks로 변환된다. */
	assoc->avg_read_latency = avg_read_latency;
	assoc->p99_read_latency = p99_read_latency;
	assoc->avg_write_latency = avg_write_latency;
	assoc->p99_write_latency = p99_write_latency;
	/* [한국어] UUID 복사 — NULL UUID도 그대로 보관(register에서 분기 처리). */
	spdk_uuid_copy(&assoc->uuid, uuid);

	/* [한국어] 전역 리스트 꼬리에 추가. */
	TAILQ_INSERT_TAIL(&g_bdev_associations, assoc, link);

	return 0;
}

/*
 * [한국어]
 * vbdev_delay_update_latency_value - 가동 중 vbdev의 지연 파라미터 갱신.
 *
 * @delay_name, @latency_us, @type: RPC 입력.
 * @return: 0=성공, -ENODEV=이름 없음, -EINVAL=잘못된 type.
 *
 * us → ticks 변환: latency_us * (ticks_hz / SEC_TO_USEC). 64-bit store는 x86에서 atomic이므로
 * 별도 락 없이 안전.
 *
 * 호출 체인: rpc_bdev_delay_update_latency → [이 함수].
 * 실행 컨텍스트: SPDK RPC poller.
 */
int
vbdev_delay_update_latency_value(char *delay_name, uint64_t latency_us, enum delay_io_type type)
{
	struct vbdev_delay *delay_node;  /* [한국어] 검색 결과 보관. */
	uint64_t ticks_mhz = spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;  /* [한국어] 1us 당 tick 수. */

	/* [한국어] 이름으로 g_delay_nodes 검색. */
	TAILQ_FOREACH(delay_node, &g_delay_nodes, link) {
		if (strcmp(delay_node->delay_bdev.name, delay_name) == 0) {
			break;  /* [한국어] 매칭 — 이후 update. */
		}
	}

	if (delay_node == NULL) {
		return -ENODEV;  /* [한국어] 매칭 없음. */
	}

	/* [한국어] type별로 해당 ticks 필드만 atomic 갱신. 다른 thread의 _delay_complete_io가 동시에 read해도
	 *           64-bit 정렬된 store는 x86에서 단일 명령으로 원자적 — read-modify-write가 아니므로 안전. */
	switch (type) {
	case DELAY_AVG_READ:
		delay_node->average_read_latency_ticks = ticks_mhz * latency_us;  /* [한국어] 64bit atomic store. */
		break;
	case DELAY_AVG_WRITE:
		delay_node->average_write_latency_ticks = ticks_mhz * latency_us;
		break;
	case DELAY_P99_READ:
		delay_node->p99_read_latency_ticks = ticks_mhz * latency_us;
		break;
	case DELAY_P99_WRITE:
		delay_node->p99_write_latency_ticks = ticks_mhz * latency_us;
		break;
	default:
		return -EINVAL;  /* [한국어] DELAY_NONE 등 잘못된 type. */
	}

	return 0;  /* [한국어] 갱신 성공. */
}

/*
 * [한국어]
 * vbdev_delay_init - 모듈 init — noop.
 */
static int
vbdev_delay_init(void)
{
	/* Not allowing for .ini style configuration. */
	/* [한국어] 전역 상태(g_bdev_associations/g_delay_nodes)는 TAILQ_HEAD_INITIALIZER로 컴파일 타임
	 *           초기화되므로 런타임 init 불필요. .ini 스타일 설정도 미지원(RPC 전용). */
	return 0;
}

/*
 * [한국어]
 * vbdev_delay_finish - 모듈 fini — g_bdev_associations 정리.
 */
static void
vbdev_delay_finish(void)
{
	struct bdev_association *assoc;  /* [한국어] head 반복 추출용 임시 포인터. */

	/* [한국어] 모든 매핑을 head부터 순차 제거 — 모든 vbdev은 이미 destruct로 해제된 후 호출. */
	while ((assoc = TAILQ_FIRST(&g_bdev_associations))) {
		TAILQ_REMOVE(&g_bdev_associations, assoc, link);  /* [한국어] 리스트에서 분리. */
		free(assoc->bdev_name);                            /* [한국어] strdup 메모리. */
		free(assoc->vbdev_name);                           /* [한국어] strdup 메모리. */
		free(assoc);                                       /* [한국어] 노드 본체. */
	}
}

/*
 * [한국어]
 * vbdev_delay_get_ctx_size - driver_ctx 크기 알림. @return: sizeof(struct delay_bdev_io).
 */
static int
vbdev_delay_get_ctx_size(void)
{
	/* [한국어] 매 bdev_io에 함께 할당될 driver_ctx 크기 — 본 모듈의 per-IO 컨텍스트.
	 *           status/completion_tick/type/ch/bdev_io_wait/zcopy_bdev_io/link를 모두 보관. */
	return sizeof(struct delay_bdev_io);
}

/*
 * [한국어]
 * vbdev_delay_write_config_json - per-bdev config 없음.
 */
static void
vbdev_delay_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No config per bdev needed */
	/* [한국어] per-bdev 옵션이 별도로 노출되지 않아 noop. 모듈 레벨 config_json이 모든 정보를 포함. */
}

/*
 * [한국어]
 * vbdev_delay_get_memory_domains - base에 위임 — 데이터 변형 없으므로 모든 base 도메인 지원.
 */
static int
vbdev_delay_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct vbdev_delay *delay_node = (struct vbdev_delay *)ctx;  /* [한국어] register 시 ctxt. */

	/* Delay bdev doesn't work with data buffers, so it supports any memory domain used by base_bdev */
	/* [한국어] delay는 데이터 버퍼를 읽지/쓰지 않으므로 base가 지원하는 메모리 도메인을 그대로 광고. */
	return spdk_bdev_get_memory_domains(delay_node->base_bdev, domains, array_size);
}

/* When we register our bdev this is how we specify our entry points. */
/* [한국어] delay vbdev의 fn_table — bdev 코어가 dispatch 시 참조. */
static const struct spdk_bdev_fn_table vbdev_delay_fn_table = {
	.destruct		= vbdev_delay_destruct,
	.submit_request		= vbdev_delay_submit_request,
	.io_type_supported	= vbdev_delay_io_type_supported,
	.get_io_channel		= vbdev_delay_get_io_channel,
	.dump_info_json		= vbdev_delay_dump_info_json,
	.write_config_json	= vbdev_delay_write_config_json,
	.get_memory_domains	= vbdev_delay_get_memory_domains,
};

/*
 * [한국어]
 * vbdev_delay_base_bdev_hotremove_cb - base hot-remove 시 본 base 위 모든 delay vbdev unregister.
 */
static void
vbdev_delay_base_bdev_hotremove_cb(struct spdk_bdev *bdev_find)
{
	struct vbdev_delay *delay_node, *tmp;  /* [한국어] SAFE 순회 — unregister가 노드 제거 가능. */

	/* [한국어] 동일 base를 공유하는 모든 delay 인스턴스에 대해 비동기 unregister 발행. */
	TAILQ_FOREACH_SAFE(delay_node, &g_delay_nodes, link, tmp) {
		if (bdev_find == delay_node->base_bdev) {
			spdk_bdev_unregister(&delay_node->delay_bdev, NULL, NULL);  /* [한국어] cb 없이 비동기 시작. */
		}
	}
}

/* Called when the underlying base bdev triggers asynchronous event such as bdev removal. */
/*
 * [한국어]
 * vbdev_delay_base_bdev_event_cb - base 이벤트 라우팅 — 현재는 REMOVE만 처리.
 */
static void
vbdev_delay_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			       void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		/* [한국어] hot-remove — 모든 자식 delay vbdev을 즉시 unregister해 상위에 EIO 통지. */
		vbdev_delay_base_bdev_hotremove_cb(bdev);
		break;
	default:
		/* [한국어] RESIZE/MEDIA_MGMT 등은 현재 미지원 — 로그만 남김. */
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/* Create and register the delay vbdev if we find it in our list of bdev names.
 * This can be called either by the examine path or RPC method.
 */
/*
 * [한국어]
 * vbdev_delay_register - g_bdev_associations 매칭 항목에 대해 vbdev 등록.
 *
 * 시퀀스(passthru와 거의 동일):
 *   1) delay_node 0-init 할당 + name strdup.
 *   2) base open_ext (write 권한 true).
 *   3) base 메타 복제(blocklen/blockcnt/dif 등).
 *   4) ctxt/fn_table/module 설정.
 *   5) 4개 지연 ticks 계산(us → ticks).
 *   6) UUID 결정(자동 생성 또는 사용자 지정).
 *   7) spdk_io_device_register.
 *   8) thread 보관.
 *   9) claim_bdev → register.
 *  10) 실패 시 error_close 라벨에서 정리.
 *
 * 참고: passthru는 g_pt_nodes에 INSERT한 후 io_device_register하지만, delay는 register 성공 후
 * INSERT한다(에러 처리 단순화).
 */
static int
vbdev_delay_register(const char *bdev_name)
{
	struct bdev_association *assoc;
	struct vbdev_delay *delay_node;
	struct spdk_bdev *bdev;
	uint64_t ticks_mhz = spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;  /* [한국어] 1us 당 tick 수. */
	struct spdk_uuid ns_uuid;
	int rc = 0;

	spdk_uuid_parse(&ns_uuid, BDEV_DELAY_NAMESPACE_UUID);  /* [한국어] 문자열 namespace UUID → 16바이트 바이너리. */

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the delay_node & bdev accordingly.
	 */
	/* [한국어] 모든 매핑을 순회하며 bdev_name이 일치하는 항목에 대해 vbdev 생성. */
	TAILQ_FOREACH(assoc, &g_bdev_associations, link) {
		if (strcmp(assoc->bdev_name, bdev_name) != 0) {
			continue;  /* [한국어] 다른 base에 대한 매핑은 skip. */
		}

		/* [한국어] 0-init 할당 — UUID/포인터 필드가 안전한 초기값을 갖도록. */
		delay_node = calloc(1, sizeof(struct vbdev_delay));
		if (!delay_node) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate delay_node\n");
			break;  /* [한국어] 루프 종료, 호출자에게 ENOMEM 보고. */
		}
		/* [한국어] vbdev 이름 strdup — bdev->name은 자체 lifecycle. */
		delay_node->delay_bdev.name = strdup(assoc->vbdev_name);
		if (!delay_node->delay_bdev.name) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate delay_bdev name\n");
			free(delay_node);  /* [한국어] 부분 할당 회수. */
			break;
		}
		delay_node->delay_bdev.product_name = "delay";  /* [한국어] product_name은 정적 문자열로 충분. */

		/* The base bdev that we're attaching to. */
		/* [한국어] write 권한 true로 open — claim_bdev 위해 필수. event_cb로 hot-remove 등 알림 수신. */
		rc = spdk_bdev_open_ext(bdev_name, true, vbdev_delay_base_bdev_event_cb,
					NULL, &delay_node->base_desc);
		if (rc) {
			if (rc != -ENODEV) {
				SPDK_ERRLOG("could not open bdev %s\n", bdev_name);
			}
			/* [한국어] open 실패 — 부분 자원 정리 후 종료. ENODEV는 정상(매핑은 이미 g_bdev_associations에 보관). */
			free(delay_node->delay_bdev.name);
			free(delay_node);
			break;
		}

		bdev = spdk_bdev_desc_get_bdev(delay_node->base_desc);  /* [한국어] desc → bdev 포인터. */
		delay_node->base_bdev = bdev;                            /* [한국어] base 포인터 보관. */

		/* [한국어] base 메타 복제 — 클라이언트는 delay vbdev을 base와 동일한 디스크처럼 본다. */
		delay_node->delay_bdev.write_cache = bdev->write_cache;              /* [한국어] write cache 지원 여부. */
		delay_node->delay_bdev.required_alignment = bdev->required_alignment;/* [한국어] DMA 정렬 요구사항. */
		delay_node->delay_bdev.optimal_io_boundary = bdev->optimal_io_boundary; /* [한국어] 최적 I/O 경계(NVMe NOIOB). */
		delay_node->delay_bdev.blocklen = bdev->blocklen;                    /* [한국어] 블록 크기(512/4096 등). */
		delay_node->delay_bdev.blockcnt = bdev->blockcnt;                    /* [한국어] 총 블록 수. */

		delay_node->delay_bdev.md_interleave = bdev->md_interleave;          /* [한국어] DIF/DIX 메타데이터 interleave 여부. */
		delay_node->delay_bdev.md_len = bdev->md_len;                        /* [한국어] 메타 길이. */
		delay_node->delay_bdev.dif_type = bdev->dif_type;                    /* [한국어] DIF 타입(0/1/2/3). */
		delay_node->delay_bdev.dif_is_head_of_md = bdev->dif_is_head_of_md;  /* [한국어] DIF가 메타 헤더에 있는지. */
		delay_node->delay_bdev.dif_check_flags = bdev->dif_check_flags;      /* [한국어] DIF 검사 플래그. */
		delay_node->delay_bdev.dif_pi_format = bdev->dif_pi_format;          /* [한국어] DIF PI 포맷. */

		delay_node->delay_bdev.ctxt = delay_node;                             /* [한국어] fn_table 콜백의 ctx 인자. */
		delay_node->delay_bdev.fn_table = &vbdev_delay_fn_table;              /* [한국어] dispatch 테이블 연결. */
		delay_node->delay_bdev.module = &delay_if;                            /* [한국어] 모듈 식별자 — claim 시 사용. */

		delay_node->delay_bdev.numa = bdev->numa;                             /* [한국어] NUMA 정보 — affinity 결정. */

		/* Store the number of ticks you need to add to get the I/O expiration time. */
		/* [한국어] us → ticks 사전 변환 — submit/complete 마다 변환 비용 절감. */
		delay_node->average_read_latency_ticks = ticks_mhz * assoc->avg_read_latency;
		delay_node->p99_read_latency_ticks = ticks_mhz * assoc->p99_read_latency;
		delay_node->average_write_latency_ticks = ticks_mhz * assoc->avg_write_latency;
		delay_node->p99_write_latency_ticks = ticks_mhz * assoc->p99_write_latency;

		if (spdk_uuid_is_null(&assoc->uuid)) {
			/* Generate UUID based on namespace UUID + base bdev UUID */
			/* [한국어] UUIDv5 — 동일 base에 대해 결정적 UUID. */
			rc = spdk_uuid_generate_sha1(&delay_node->delay_bdev.uuid, &ns_uuid,
						     (const char *)&bdev->uuid, sizeof(struct spdk_uuid));
			if (rc) {
				/* [한국어] SHA1 실패는 드물지만 — 부분 자원 정리 후 종료. */
				spdk_bdev_close(delay_node->base_desc);
				free(delay_node->delay_bdev.name);
				free(delay_node);
				break;
			}
		} else {
			/* [한국어] 사용자가 명시한 UUID를 그대로 사용. */
			spdk_uuid_copy(&delay_node->delay_bdev.uuid, &assoc->uuid);
		}

		/* [한국어] io_device 등록 — delay_io_channel 크기 명시. spdk_get_io_channel이 자동 할당.
		 *           ctx_buf로 channel ctx 영역이 함께 할당되어 create_cb에 전달됨. */
		spdk_io_device_register(delay_node, delay_bdev_ch_create_cb, delay_bdev_ch_destroy_cb,
					sizeof(struct delay_io_channel),
					assoc->vbdev_name);

		/* Save the thread where the base device is opened */
		delay_node->thread = spdk_get_thread();  /* [한국어] close를 동일 thread에서 수행하기 위해 보관. */

		/* [한국어] base에 대한 exclusive claim — 다른 모듈이 attach 못 하게. */
		rc = spdk_bdev_module_claim_bdev(bdev, delay_node->base_desc, delay_node->delay_bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", bdev_name);
			goto error_close;  /* [한국어] 통합 cleanup 경로. */
		}

		/* [한국어] 마지막 단계 — vbdev을 bdev 레이어에 노출. 이 시점부터 외부 사용 가능. */
		rc = spdk_bdev_register(&delay_node->delay_bdev);
		if (rc) {
			SPDK_ERRLOG("could not register delay_bdev\n");
			spdk_bdev_module_release_bdev(delay_node->base_bdev);  /* [한국어] claim 해제. */
			goto error_close;
		}

		/* [한국어] 성공 — 전역 리스트에 추가. passthru와 달리 register 성공 후 INSERT한다. */
		TAILQ_INSERT_TAIL(&g_delay_nodes, delay_node, link);
	}

	return rc;

error_close:
	/* [한국어] register 실패 시 cleanup — bdev_close + io_device_unregister + free.
	 *           io_device_unregister에 NULL을 줘 즉시 동기 해제(아직 채널 없음). */
	spdk_bdev_close(delay_node->base_desc);
	spdk_io_device_unregister(delay_node, NULL);
	free(delay_node->delay_bdev.name);
	free(delay_node);
	return rc;
}

/*
 * [한국어]
 * create_delay_disk - RPC 외부 진입점.
 *
 * @bdev_name, @vbdev_name, @uuid, 4개 지연: 입력. @return: 0=성공/base 미존재, 음수=errno.
 *
 * 입력 검증:
 *   - p99 < avg → -EINVAL (p99는 평균보다 길어야 의미가 있음).
 * base 미존재(-ENODEV)는 0으로 보정 — examine 시 자동 활성화.
 *
 * 호출 체인: rpc_bdev_delay_create → [이 함수] → vbdev_delay_insert_association + vbdev_delay_register.
 * 실행 컨텍스트: SPDK RPC poller.
 */
int
create_delay_disk(const char *bdev_name, const char *vbdev_name, struct spdk_uuid *uuid,
		  uint64_t avg_read_latency,
		  uint64_t p99_read_latency, uint64_t avg_write_latency, uint64_t p99_write_latency)
{
	int rc = 0;  /* [한국어] 단계별 결과 누적. */

	/* [한국어] 의미적 검증 — p99는 평균 이상이어야 함. 아니면 통계적으로 무효한 설정. */
	if (p99_read_latency < avg_read_latency || p99_write_latency < avg_write_latency) {
		SPDK_ERRLOG("Unable to create a delay bdev where p99 latency is less than average latency.\n");
		return -EINVAL;
	}

	/* [한국어] 1단계: 매핑 등록. base가 아직 없어도 보관됨. */
	rc = vbdev_delay_insert_association(bdev_name, vbdev_name, uuid, avg_read_latency, p99_read_latency,
					    avg_write_latency, p99_write_latency);
	if (rc) {
		return rc;  /* [한국어] EEXIST/ENOMEM 그대로 전달. */
	}

	/* [한국어] 2단계: 즉시 vbdev 등록 시도. */
	rc = vbdev_delay_register(bdev_name);
	if (rc == -ENODEV) {
		/* This is not an error, we tracked the name above and it still
		 * may show up later.
		 */
		/* [한국어] base 부재는 정상 — examine에서 처리. 클라이언트에는 성공으로 보고. */
		SPDK_NOTICELOG("vbdev creation deferred pending base bdev arrival\n");
		rc = 0;
	}

	return rc;
}

/*
 * [한국어]
 * delete_delay_disk - RPC 외부 진입점. 비동기 unregister 시작.
 *
 * unregister 시작 성공 시 g_bdev_associations에서도 해당 매핑 제거 — 같은 base가 재등록될 때
 * 자동 재생성 방지(사용자가 명시적으로 다시 create RPC를 호출해야 함).
 *
 * 호출 체인: rpc_bdev_delay_delete → [이 함수].
 */
void
delete_delay_disk(const char *vbdev_name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct bdev_association *assoc;  /* [한국어] 매핑 검색 결과. */
	int rc;                           /* [한국어] unregister 즉시 결과. */

	/* [한국어] 비동기 unregister 시작 — 실제 destruct는 콜백에서 완료된다. */
	rc = spdk_bdev_unregister_by_name(vbdev_name, &delay_if, cb_fn, cb_arg);
	if (rc == 0) {
		/* [한국어] g_bdev_associations에서도 매핑 제거 — 같은 base가 재등록되어도 자동 재생성 방지. */
		TAILQ_FOREACH(assoc, &g_bdev_associations, link) {
			if (strcmp(assoc->vbdev_name, vbdev_name) == 0) {
				TAILQ_REMOVE(&g_bdev_associations, assoc, link);  /* [한국어] 분리. */
				free(assoc->bdev_name);                            /* [한국어] strdup 해제. */
				free(assoc->vbdev_name);                           /* [한국어] strdup 해제. */
				free(assoc);                                       /* [한국어] 노드 해제. */
				break;                                             /* [한국어] vbdev_name 유일. */
			}
		}
	} else {
		cb_fn(cb_arg, rc);  /* [한국어] 즉시 실패 — cb_fn에 에러 코드 전달, 비동기 경로 안 탐. */
	}
}

/*
 * [한국어]
 * vbdev_delay_examine - 새 base bdev 등록 시 자동 활성화.
 */
static void
vbdev_delay_examine(struct spdk_bdev *bdev)
{
	/* [한국어] 새로 등록된 base 이름으로 vbdev 생성 시도. 매핑이 없으면 register 내부 no-op. */
	vbdev_delay_register(bdev->name);

	spdk_bdev_module_examine_done(&delay_if);  /* [한국어] examine 완료 알림 — 다음 모듈에 control 전달. */
}

/* [한국어] "vbdev_delay" 디버그 컴포넌트 등록. */
SPDK_LOG_REGISTER_COMPONENT(vbdev_delay)
