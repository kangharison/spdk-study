/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * This is a simple example of a virtual block device module that passes IO
 * down to a bdev (or bdevs) that its configured to attach to.
 */

/*
 * [한국어 설명] passthru(투과형) 가상 bdev 모듈 본체 (vbdev_passthru.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK passthru vbdev 모듈을 구현한다. passthru는 base bdev 위에 한 층을 덧대어
 * 모든 I/O를 그대로 base에 전달하기만 하는 가장 단순한 vbdev로, **vbdev 모듈을 새로 작성하려는
 * 개발자를 위한 정식 레퍼런스(canonical skeleton)** 역할을 한다. 따라서 split/error/delay와 달리
 * lib/bdev/part.c의 part 추상화를 사용하지 않고, bdev 자체를 직접 등록(spdk_bdev_register)하고
 * I/O 요청을 직접 spdk_bdev_*v_blocks_ext 등 공개 API로 base bdev에 발행한다.
 * 모든 I/O 타입(READ/WRITE/UNMAP/FLUSH/RESET/ZCOPY/ABORT/COPY/WRITE_ZEROES)을 명시적으로 처리하며,
 * extended IO opts(메모리 도메인, 메타데이터, DIF 검사 플래그)를 그대로 forwarding한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [bdev_passthru_create RPC] → vbdev_passthru_rpc.c → bdev_passthru_create_disk (이 파일)
 *                                                       → vbdev_passthru_insert_name (g_bdev_names 등록)
 *                                                       → vbdev_passthru_register
 *                                                            → spdk_bdev_open_ext (base 열기)
 *                                                            → spdk_io_device_register
 *                                                            → spdk_bdev_module_claim_bdev
 *                                                            → spdk_bdev_register (vbdev 등록).
 *   [examine 시] base 등록 → vbdev_passthru_examine → vbdev_passthru_register.
 *   [I/O 경로]
 *     submit: 클라이언트 → spdk_bdev_submit → bdev 코어 → vbdev_passthru_submit_request
 *             → io_type별 spdk_bdev_*v_blocks_ext (base에 새 bdev_io 발행, _pt_complete_io 콜백 지정).
 *     complete: base 완료 → _pt_complete_io → 원래 bdev_io를 같은 상태로 완료.
 *     ZCOPY는 별도 _pt_complete_zcopy_io 콜백(버퍼 포인터 forwarding).
 *
 * === 타 모듈과의 연결 ===
 * - include: spdk/stdinc.h, vbdev_passthru.h, spdk/rpc.h, spdk/env.h(미사용 가능), spdk/endian.h,
 *   spdk/string.h, spdk/thread.h, spdk/util.h, spdk/bdev_module.h, spdk/log.h.
 * - 의존: lib/bdev (open/register/io APIs), lib/thread (io_channel, send_msg).
 * - 데이터 흐름: 클라이언트 → passthru bdev_io → 새 bdev_io를 base에 발행 → base 완료 → passthru 완료.
 *
 * === 주요 함수/구조체 요약 ===
 * - `struct bdev_names`              : (vbdev_name, bdev_name, uuid) — 등록 대기 + 활성 매핑.
 * - `struct vbdev_passthru`          : 한 passthru vbdev 인스턴스 — base_bdev/desc + pt_bdev + open thread.
 * - `struct pt_io_channel`           : per-thread 채널 — base_ch 보관.
 * - `struct passthru_bdev_io`        : per-IO 컨텍스트 — 데모용 test 필드 + ENOMEM wait 엔트리.
 * - `vbdev_passthru_submit_request`  : I/O 진입점 — io_type별 base API 호출.
 * - `_pt_complete_io / _pt_complete_zcopy_io` : 비동기 완료 콜백.
 * - `vbdev_passthru_register`        : 실제 등록 본체.
 * - `bdev_passthru_create_disk / bdev_passthru_delete_disk` : RPC 외부 진입점.
 */

/* [한국어] POSIX 표준 헤더 모음. */
#include "spdk/stdinc.h"

/* [한국어] passthru 모듈 공개 헤더 — RPC가 사용하는 함수 선언. */
#include "vbdev_passthru.h"
/* [한국어] RPC 매크로(현재 파일에서 직접 등록은 없지만 헤더 의존성). */
#include "spdk/rpc.h"
/* [한국어] DPDK 환경 추상화(이 파일에서는 직접 사용 없음 — 호환성). */
#include "spdk/env.h"
/* [한국어] 엔디안 매크로(현재 미사용). */
#include "spdk/endian.h"
/* [한국어] spdk_uuid_parse, sprintf_alloc 등. */
#include "spdk/string.h"
/* [한국어] spdk_get_thread, spdk_thread_send_msg — 다른 thread에서 base close하기 위해 사용. */
#include "spdk/thread.h"
/* [한국어] SPDK_COUNTOF, SPDK_CONTAINEROF 등 매크로. */
#include "spdk/util.h"

/* [한국어] bdev 모듈 작성 핵심 API 묶음. */
#include "spdk/bdev_module.h"
/* [한국어] 로깅 매크로. */
#include "spdk/log.h"

/* This namespace UUID was generated using uuid_generate() method. */
/* [한국어] passthru가 자동으로 UUID를 생성할 때 사용하는 namespace UUID(SHA-1 시드).
 *           UUID v5: ns_uuid + base_bdev->uuid → 결정적 vbdev UUID 생성.
 *           재기동 후에도 동일 base에 대해서 동일 vbdev UUID가 나오도록 보장. */
#define BDEV_PASSTHRU_NAMESPACE_UUID "7e25812e-c8c0-4d3f-8599-16d790555b85"

/* [한국어] 모듈 콜백 전방 선언. */
static int vbdev_passthru_init(void);
static int vbdev_passthru_get_ctx_size(void);
static void vbdev_passthru_examine(struct spdk_bdev *bdev);
static void vbdev_passthru_finish(void);
static int vbdev_passthru_config_json(struct spdk_json_write_ctx *w);

/* [한국어] passthru 모듈 정의 — split/error와 비슷하지만 part API를 쓰지 않으므로 자체 register/I/O가 핵심. */
static struct spdk_bdev_module passthru_if = {
	.name = "passthru",                      /* [한국어] 모듈 이름. */
	.module_init = vbdev_passthru_init,      /* [한국어] init — noop. */
	.get_ctx_size = vbdev_passthru_get_ctx_size,  /* [한국어] driver_ctx 크기. */
	.examine_config = vbdev_passthru_examine,/* [한국어] base 등록 시 자동 활성화. */
	.module_fini = vbdev_passthru_finish,    /* [한국어] fini — g_bdev_names 정리. */
	.config_json = vbdev_passthru_config_json/* [한국어] save_config 직렬화. */
};

/* [한국어] 모듈 등록 — SPDK 부팅 시 자동 호출. */
SPDK_BDEV_MODULE_REGISTER(passthru, &passthru_if)

/* List of pt_bdev names and their base bdevs via configuration file.
 * Used so we can parse the conf once at init and use this list in examine().
 */
/* [한국어] (vbdev_name, bdev_name, uuid) 매핑. base가 아직 등록 전이어도 이 리스트에 보관해두고
 *           examine 시 매칭되면 자동으로 vbdev를 등록한다. */
struct bdev_names {
	char			*vbdev_name;
	/* [한국어] 새로 만들 vbdev 이름.
	 * 설정자: vbdev_passthru_insert_name. 읽는 자: vbdev_passthru_register/examine.
	 * 동기화: g_bdev_names는 메인 thread에서만 조작. */

	char			*bdev_name;
	/* [한국어] base bdev 이름. */

	struct spdk_uuid	uuid;
	/* [한국어] vbdev에 부여할 UUID(0이면 자동 생성). */

	TAILQ_ENTRY(bdev_names)	link;
	/* [한국어] g_bdev_names 연결 노드. */
};
/* [한국어] 모든 passthru 매핑의 전역 TAILQ. */
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

/* List of virtual bdevs and associated info for each. */
/* [한국어] 한 passthru vbdev 인스턴스의 컨텍스트 — pt_bdev.ctxt에 저장된다. */
struct vbdev_passthru {
	struct spdk_bdev		*base_bdev; /* the thing we're attaching to */
	/* [한국어] base bdev 포인터(spdk_bdev_desc_get_bdev로 얻음).
	 * 설정자: register. 읽는 자: submit_request, dump_info_json, get_memory_domains.
	 * 값 범위: NULL 아님(open 성공 후). 동기화: 단일 thread. */

	struct spdk_bdev_desc		*base_desc; /* its descriptor we get from open */
	/* [한국어] base bdev 디스크립터(open_ext 결과). I/O 발행 시 모든 spdk_bdev_*_blocks API의 첫 인자.
	 * 설정자: register의 spdk_bdev_open_ext. 읽는 자: submit_request, destruct.
	 * 동기화: open한 thread에서만 close해야 함(아래 thread 필드 참조). */

	struct spdk_bdev		pt_bdev;    /* the PT virtual bdev */
	/* [한국어] passthru가 노출하는 가상 bdev 본체. spdk_bdev_register에 직접 전달.
	 * 설정자: register가 name/blocklen/blockcnt 등 base에서 복제. 읽는 자: bdev 코어. */

	TAILQ_ENTRY(vbdev_passthru)	link;
	/* [한국어] g_pt_nodes 연결 노드. */

	struct spdk_thread		*thread;    /* thread where base device is opened */
	/* [한국어] base_desc를 open한 thread. close는 반드시 동일 thread에서 해야 하므로 보관.
	 * 설정자: register의 spdk_get_thread(). 읽는 자: destruct가 thread 일치 검사 후
	 *           일치 안 하면 spdk_thread_send_msg로 close 디스패치.
	 * 동기화: thread는 spdk_thread 객체이며 스스로 lifecycle 관리. */
};
/* [한국어] 모든 passthru 인스턴스의 전역 TAILQ. */
static TAILQ_HEAD(, vbdev_passthru) g_pt_nodes = TAILQ_HEAD_INITIALIZER(g_pt_nodes);

/* The pt vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 * If this vbdev needed to implement a poller or a queue for IO, this is where those things
 * would be defined. This passthru bdev doesn't actually need to allocate a channel, it could
 * simply pass back the channel of the bdev underneath it but for example purposes we will
 * present its own to the upper layers.
 */
/* [한국어] passthru 채널 — base bdev 채널을 보관. lib/bdev/io_channel이 자동 할당/해제. */
struct pt_io_channel {
	struct spdk_io_channel	*base_ch; /* IO channel of base device */
	/* [한국어] base bdev에 대한 IO 채널.
	 * 설정자: pt_bdev_ch_create_cb이 spdk_bdev_get_io_channel로 채움.
	 * 읽는 자: submit_request가 모든 base API 호출에 사용.
	 * 동기화: 채널은 thread-affinity로 동시 접근 없음. */
};

/* Just for fun, this pt_bdev module doesn't need it but this is essentially a per IO
 * context that we get handed by the bdev layer.
 */
/* [한국어] per-IO 컨텍스트 — bdev_io->driver_ctx에 저장. 본 모듈은 데모용 test 필드와
 *           ENOMEM 시 재시도용 wait 엔트리만 보관. */
struct passthru_bdev_io {
	uint8_t test;
	/* [한국어] 데모용 — submit에서 0x5a로 설정하고 complete에서 검증.
	 * driver_ctx가 정확히 보존되는지 확인하는 sanity check 목적. */

	/* bdev related */
	struct spdk_io_channel *ch;
	/* [한국어] 재시도 시 사용할 채널 보관. ENOMEM 시 vbdev_passthru_queue_io에서 사용. */

	/* for bdev_io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	/* [한국어] spdk_bdev_queue_io_wait 등록용. base가 자원 부족 해소 시 cb_fn 호출. */
};

/* [한국어] 전방 선언 — 재시도 콜백에서도 호출되므로 별도 분리. */
static void vbdev_passthru_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);


/* Callback for unregistering the IO device. */
/*
 * [한국어]
 * _device_unregister_cb - spdk_io_device_unregister 완료 시 호출되는 콜백.
 *
 * @io_device: 등록 시 전달한 vbdev_passthru 포인터.
 *
 * io_device 등록을 해제한 시점에 메모리 해제 — 모든 채널이 닫힌 뒤 안전하게 free 가능.
 *
 * 호출 체인: vbdev_passthru_destruct → spdk_io_device_unregister → 모든 채널 close → [이 콜백].
 * 실행 컨텍스트: io device 메인 thread.
 */
static void
_device_unregister_cb(void *io_device)
{
	struct vbdev_passthru *pt_node  = io_device;

	/* Done with this pt_node. */
	free(pt_node->pt_bdev.name);  /* [한국어] strdup된 vbdev 이름 해제. */
	free(pt_node);                /* [한국어] vbdev_passthru 본체 해제. */
}

/* Wrapper for the bdev close operation. */
/*
 * [한국어]
 * _vbdev_passthru_destruct - thread cross 시 send_msg로 디스패치되는 close wrapper.
 *
 * @ctx: spdk_bdev_desc 포인터.
 *
 * spdk_bdev_close는 open한 thread에서만 안전하게 호출 가능 → 다른 thread에서 destruct가 일어나면
 * send_msg로 open thread로 위임.
 *
 * 호출 체인: spdk_thread_send_msg → [이 함수] → spdk_bdev_close.
 * 실행 컨텍스트: open한 thread.
 */
static void
_vbdev_passthru_destruct(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}

/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next.
 */
/*
 * [한국어]
 * vbdev_passthru_destruct - bdev_fn_table.destruct — passthru vbdev 소멸 시퀀스.
 *
 * @ctx: vbdev_passthru 포인터.
 * @return: 0(spdk_io_device_unregister는 비동기이므로 0 반환).
 *
 * 시퀀스가 중요:
 *   1) g_pt_nodes에서 분리.
 *   2) base bdev claim 해제(다른 모듈이 attach 가능하게).
 *   3) base_desc close — 반드시 open한 thread에서.
 *   4) io_device unregister — 채널 모두 정리되면 _device_unregister_cb이 메모리 해제.
 *
 * 호출 체인: spdk_bdev_unregister → bdev 코어 → [이 함수] → spdk_io_device_unregister →
 *            (모든 채널 정리) → _device_unregister_cb → free.
 * 실행 컨텍스트: bdev 메인 thread (open thread와 다를 수 있음).
 */
static int
vbdev_passthru_destruct(void *ctx)
{
	struct vbdev_passthru *pt_node = (struct vbdev_passthru *)ctx;

	/* It is important to follow this exact sequence of steps for destroying
	 * a vbdev...
	 */

	TAILQ_REMOVE(&g_pt_nodes, pt_node, link);  /* [한국어] 1) 전역 리스트에서 분리. */

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(pt_node->base_bdev);  /* [한국어] 2) base의 exclusive claim 해제. */

	/* Close the underlying bdev on its same opened thread. */
	if (pt_node->thread && pt_node->thread != spdk_get_thread()) {
		/* [한국어] 3) 현재 thread가 open thread와 다르면 send_msg로 위임 — close는 open thread에서만 안전. */
		spdk_thread_send_msg(pt_node->thread, _vbdev_passthru_destruct, pt_node->base_desc);
	} else {
		/* [한국어] 동일 thread면 직접 close. */
		spdk_bdev_close(pt_node->base_desc);
	}

	/* Unregister the io_device. */
	spdk_io_device_unregister(pt_node, _device_unregister_cb);  /* [한국어] 4) 비동기 io_device 해제 — 콜백에서 free. */

	return 0;
}

/* Completion callback for IO that were issued from this bdev. The original bdev_io
 * is passed in as an arg so we'll complete that one with the appropriate status
 * and then free the one that this module issued.
 */
/*
 * [한국어]
 * _pt_complete_io - 일반 I/O 완료 콜백.
 *
 * @bdev_io: 우리가 base에 발행한 새 bdev_io. @success: 성공 여부. @cb_arg: 원래 bdev_io.
 *
 * passthru는 base에 새 bdev_io를 발행하므로 두 개의 bdev_io가 짝지어 존재한다:
 *   1) 클라이언트가 우리에게 보낸 'orig_io' — passthru bdev에 묶인 것.
 *   2) 우리가 base에 발행한 'bdev_io'    — base bdev에 묶인 것.
 * 본 콜백에서 base 결과 상태를 orig_io에 그대로 복사 후 새 bdev_io는 free.
 *
 * 호출 체인: spdk_bdev_*v_blocks_ext → base 완료 → [이 콜백] → spdk_bdev_io_complete_base_io_status.
 * 실행 컨텍스트: 채널 reactor.
 */
static void
_pt_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)orig_io->driver_ctx;

	/* We setup this value in the submission routine, just showing here that it is
	 * passed back to us.
	 */
	if (io_ctx->test != 0x5a) {
		/* [한국어] 데모 sanity check — driver_ctx가 보존되었음을 확인. 실제 모듈에서는 이 검증 불필요. */
		SPDK_ERRLOG("Error, original IO device_ctx is wrong! 0x%x\n",
			    io_ctx->test);
	}

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_request.
	 */
	/* [한국어] base의 완료 상태(NVMe SC, 일반 errno 등)를 orig_io에 그대로 전달. */
	spdk_bdev_io_complete_base_io_status(orig_io, bdev_io);

	spdk_bdev_free_io(bdev_io);  /* [한국어] 우리가 발행한 새 bdev_io 자원 해제. */
}

/*
 * [한국어]
 * _pt_complete_zcopy_io - ZCOPY I/O 완료 콜백.
 *
 * @bdev_io, @success, @cb_arg: 위와 같음.
 *
 * ZCOPY는 base가 직접 매핑한 버퍼를 클라이언트에 노출하므로, base의 iov[0]을 orig_io에
 * spdk_bdev_io_set_buf로 forwarding한 뒤 완료.
 *
 * 호출 체인: spdk_bdev_zcopy_start → base 완료 → [이 콜백] → spdk_bdev_io_set_buf + complete.
 * 실행 컨텍스트: 채널 reactor.
 */
static void
_pt_complete_zcopy_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)orig_io->driver_ctx;

	/* We setup this value in the submission routine, just showing here that it is
	 * passed back to us.
	 */
	if (io_ctx->test != 0x5a) {
		SPDK_ERRLOG("Error, original IO device_ctx is wrong! 0x%x\n",
			    io_ctx->test);
	}

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_request.
	 */
	/* [한국어] base가 노출한 버퍼 포인터를 클라이언트의 orig_io에 그대로 매핑. */
	spdk_bdev_io_set_buf(orig_io, bdev_io->u.bdev.iovs[0].iov_base, bdev_io->u.bdev.iovs[0].iov_len);
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

/*
 * [한국어]
 * vbdev_passthru_resubmit_io - bdev_io_wait 큐에서 깨어났을 때 재발행.
 *
 * 호출 체인: lib/bdev (자원 회복) → wait cb_fn → [이 함수] → vbdev_passthru_submit_request.
 * 실행 컨텍스트: 채널 reactor.
 */
static void
vbdev_passthru_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)bdev_io->driver_ctx;

	vbdev_passthru_submit_request(io_ctx->ch, bdev_io);  /* [한국어] 동일 채널/IO로 재발행. */
}

/*
 * [한국어]
 * vbdev_passthru_queue_io - ENOMEM 시 wait 큐에 등록.
 *
 * 호출 체인: submit_request 또는 pt_read_get_buf_cb (ENOMEM) → [이 함수].
 * 실행 컨텍스트: 채널 reactor.
 */
static void
vbdev_passthru_queue_io(struct spdk_bdev_io *bdev_io)
{
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)bdev_io->driver_ctx;
	struct pt_io_channel *pt_ch = spdk_io_channel_get_ctx(io_ctx->ch);
	int rc;

	/* [한국어] wait 엔트리에 bdev/cb_fn/cb_arg 채움. */
	io_ctx->bdev_io_wait.bdev = bdev_io->bdev;
	io_ctx->bdev_io_wait.cb_fn = vbdev_passthru_resubmit_io;
	io_ctx->bdev_io_wait.cb_arg = bdev_io;

	/* Queue the IO using the channel of the base device. */
	/* [한국어] base의 채널 wait 큐에 등록 — base bdev이 자원 회복 시 cb_fn 호출. */
	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, pt_ch->base_ch, &io_ctx->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vbdev_passthru_queue_io, rc=%d.\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * [한국어]
 * pt_init_ext_io_opts - extended IO opts 구조체를 base I/O 발행용으로 채움.
 *
 * @bdev_io: 원본. @opts: 출력 buffer.
 *
 * 메모리 도메인, 메타데이터 버퍼, DIF 검사 플래그를 그대로 forwarding.
 * dif_check_flags_exclude_mask는 'NOT' 마스크 — 검사하지 *않을* 플래그를 1로 표시.
 * 따라서 ~bdev_io->u.bdev.dif_check_flags가 정답.
 *
 * 호출 체인: pt_read_get_buf_cb / submit_request(WRITE) → [이 함수].
 */
static void
pt_init_ext_io_opts(struct spdk_bdev_io *bdev_io, struct spdk_bdev_ext_io_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->size = sizeof(*opts);                                       /* [한국어] 구조체 크기 — ABI 호환. */
	opts->memory_domain = bdev_io->u.bdev.memory_domain;              /* [한국어] DPDK 외 메모리 도메인(GPU 메모리 등). */
	opts->memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;      /* [한국어] 도메인별 컨텍스트. */
	opts->metadata = bdev_io->u.bdev.md_buf;                          /* [한국어] DIF 메타데이터 버퍼. */
	opts->dif_check_flags_exclude_mask = ~bdev_io->u.bdev.dif_check_flags;  /* [한국어] 비트 inverse — 검사 제외 플래그. */
}

/* Callback for getting a buf from the bdev pool in the event that the caller passed
 * in NULL, we need to own the buffer so it doesn't get freed by another vbdev module
 * beneath us before we're done with it. That won't happen in this example but it could
 * if this example were used as a template for something more complex.
 */
/*
 * [한국어]
 * pt_read_get_buf_cb - READ I/O를 위한 버퍼 확보 후 호출 → 실제 발행.
 *
 * @ch, @bdev_io, @success: 표준 시그니처.
 *
 * 호출 체인: spdk_bdev_io_get_buf → 풀 → [이 콜백] → spdk_bdev_readv_blocks_ext.
 * 실행 컨텍스트: 채널 reactor.
 */
static void
pt_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	/* [한국어] bdev->ctxt가 vbdev_passthru지만, 여기서는 bdev에서 컨테이너로 복원하는 패턴 시연. */
	struct vbdev_passthru *pt_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_passthru,
					 pt_bdev);
	struct pt_io_channel *pt_ch = spdk_io_channel_get_ctx(ch);
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)bdev_io->driver_ctx;
	struct spdk_bdev_ext_io_opts io_opts;
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);  /* [한국어] 버퍼 풀 고갈. */
		return;
	}

	pt_init_ext_io_opts(bdev_io, &io_opts);
	/* [한국어] base에 readv 발행. iovs/offset/num_blocks는 원본을 그대로 사용.
	 *           cb_arg=bdev_io로 콜백에서 원본 복원. */
	rc = spdk_bdev_readv_blocks_ext(pt_node->base_desc, pt_ch->base_ch, bdev_io->u.bdev.iovs,
					bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
					bdev_io->u.bdev.num_blocks, _pt_complete_io,
					bdev_io, &io_opts);
	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory, start to queue io for passthru.\n");
			io_ctx->ch = ch;                  /* [한국어] 재시도용 채널 보관. */
			vbdev_passthru_queue_io(bdev_io);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/* Called when someone above submits IO to this pt vbdev. We're simply passing it on here
 * via SPDK IO calls which in turn allocate another bdev IO and call our cpl callback provided
 * below along with the original bdev_io so that we can complete it once this IO completes.
 */
/*
 * [한국어]
 * vbdev_passthru_submit_request - bdev_fn_table.submit_request — I/O 진입점.
 *
 * @ch     : 채널.
 * @bdev_io: I/O.
 *
 * io_type별로 적절한 spdk_bdev_*_blocks_ext API를 호출해 base에 동일 I/O를 새로 발행한다.
 * 반환값:
 *   0       : 정상(완료는 비동기).
 *   -ENOMEM : base 자원 부족 → wait 큐에 등록.
 *   기타    : 즉시 FAILED 완료.
 *
 * 호출 체인: 클라이언트 → bdev 코어 → [이 함수] → spdk_bdev_*_blocks_ext → base.
 * 실행 컨텍스트: 채널 reactor.
 */
static void
vbdev_passthru_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_passthru *pt_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_passthru, pt_bdev);
	struct pt_io_channel *pt_ch = spdk_io_channel_get_ctx(ch);
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)bdev_io->driver_ctx;
	struct spdk_bdev_ext_io_opts io_opts;
	int rc = 0;

	/* Setup a per IO context value; we don't do anything with it in the vbdev other
	 * than confirm we get the same thing back in the completion callback just to
	 * demonstrate.
	 */
	io_ctx->test = 0x5a;  /* [한국어] driver_ctx 보존성 검증용 매직 값. */

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		/* [한국어] READ는 클라이언트 버퍼가 없을 수 있어 풀에서 확보 후 발행. */
		spdk_bdev_io_get_buf(bdev_io, pt_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* [한국어] WRITE는 클라이언트 버퍼 그대로 forwarding. ext opts는 메모리 도메인/DIF 등 복사. */
		pt_init_ext_io_opts(bdev_io, &io_opts);
		rc = spdk_bdev_writev_blocks_ext(pt_node->base_desc, pt_ch->base_ch, bdev_io->u.bdev.iovs,
						 bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
						 bdev_io->u.bdev.num_blocks, _pt_complete_io,
						 bdev_io, &io_opts);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		/* [한국어] WRITE_ZEROES는 데이터 버퍼 없이 디바이스 측에서 0으로 채움(NVMe Write Zeroes 명령). */
		rc = spdk_bdev_write_zeroes_blocks(pt_node->base_desc, pt_ch->base_ch,
						   bdev_io->u.bdev.offset_blocks,
						   bdev_io->u.bdev.num_blocks,
						   _pt_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		/* [한국어] UNMAP/discard — NVMe Dataset Management(Deallocate). */
		rc = spdk_bdev_unmap_blocks(pt_node->base_desc, pt_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _pt_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		/* [한국어] 캐시 플러시 — NVMe Flush 명령. */
		rc = spdk_bdev_flush_blocks(pt_node->base_desc, pt_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _pt_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		/* [한국어] 컨트롤러 reset — base bdev 모듈이 정의한 reset 절차 실행. */
		rc = spdk_bdev_reset(pt_node->base_desc, pt_ch->base_ch,
				     _pt_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		/* [한국어] ZCOPY — base가 직접 매핑한 버퍼 사용. populate=true면 READ 효과(데이터 매핑). */
		rc = spdk_bdev_zcopy_start(pt_node->base_desc, pt_ch->base_ch, NULL, 0,
					   bdev_io->u.bdev.offset_blocks,
					   bdev_io->u.bdev.num_blocks, bdev_io->u.bdev.zcopy.populate,
					   _pt_complete_zcopy_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		/* [한국어] 다른 bdev_io를 abort — bio_to_abort에 대상 I/O를 전달. */
		rc = spdk_bdev_abort(pt_node->base_desc, pt_ch->base_ch, bdev_io->u.abort.bio_to_abort,
				     _pt_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_COPY:
		/* [한국어] 디바이스 내부 COPY (NVMe 2.0 Copy 명령) — src→dst LBA를 디바이스가 처리. */
		rc = spdk_bdev_copy_blocks(pt_node->base_desc, pt_ch->base_ch,
					   bdev_io->u.bdev.offset_blocks,
					   bdev_io->u.bdev.copy.src_offset_blocks,
					   bdev_io->u.bdev.num_blocks,
					   _pt_complete_io, bdev_io);
		break;
	default:
		SPDK_ERRLOG("passthru: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory, start to queue io for passthru.\n");
			io_ctx->ch = ch;
			vbdev_passthru_queue_io(bdev_io);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/* We'll just call the base bdev and let it answer however if we were more
 * restrictive for some reason (or less) we could get the response back
 * and modify according to our purposes.
 */
/*
 * [한국어]
 * vbdev_passthru_io_type_supported - bdev_fn_table.io_type_supported — base에 위임.
 *
 * @ctx: vbdev_passthru. @io_type: 질의 대상.
 * @return: base가 지원하는지 그대로 반환.
 *
 * 호출 체인: bdev 코어가 새 I/O 진입 시 한 번 호출 → [이 함수] → spdk_bdev_io_type_supported.
 */
static bool
vbdev_passthru_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_passthru *pt_node = (struct vbdev_passthru *)ctx;

	return spdk_bdev_io_type_supported(pt_node->base_bdev, io_type);
}

/* We supplied this as an entry point for upper layers who want to communicate to this
 * bdev.  This is how they get a channel. We are passed the same context we provided when
 * we created our PT vbdev in examine() which, for this bdev, is the address of one of
 * our context nodes. From here we'll ask the SPDK channel code to fill out our channel
 * struct and we'll keep it in our PT node.
 */
/*
 * [한국어]
 * vbdev_passthru_get_io_channel - bdev_fn_table.get_io_channel — 클라이언트가 채널 요청 시 호출.
 *
 * @ctx: vbdev_passthru.
 * @return: spdk_io_channel 핸들.
 *
 * spdk_get_io_channel은 등록된 io_device(=pt_node)에 대한 새 채널을 할당(ch_create_cb 호출 포함).
 * 동일 thread에서 두 번째 호출 시에는 캐시된 채널 반환(reference count 증가).
 *
 * 호출 체인: 클라이언트 → spdk_bdev_get_io_channel → [이 함수] → spdk_get_io_channel.
 * 실행 컨텍스트: 클라이언트 thread.
 */
static struct spdk_io_channel *
vbdev_passthru_get_io_channel(void *ctx)
{
	struct vbdev_passthru *pt_node = (struct vbdev_passthru *)ctx;
	struct spdk_io_channel *pt_ch = NULL;

	/* The IO channel code will allocate a channel for us which consists of
	 * the SPDK channel structure plus the size of our pt_io_channel struct
	 * that we passed in when we registered our IO device. It will then call
	 * our channel create callback to populate any elements that we need to
	 * update.
	 */
	pt_ch = spdk_get_io_channel(pt_node);

	return pt_ch;
}

/* This is the output for bdev_get_bdevs() for this vbdev */
/*
 * [한국어]
 * vbdev_passthru_dump_info_json - bdev_get_bdevs 응답에 passthru 정보 추가.
 *
 * 응답: { "passthru": { "name": "PT0", "base_bdev_name": "Nvme0n1" } }
 */
static int
vbdev_passthru_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	/* [한국어] bdev_fn_table.dump_info_json 콜백 — ctx는 spdk_bdev_register 시 등록한 pt_bdev.ctxt. */
	struct vbdev_passthru *pt_node = (struct vbdev_passthru *)ctx;

	/* [한국어] "passthru" 키 시작 — bdev_get_bdevs 응답의 driver_specific 섹션에 추가됨. */
	spdk_json_write_name(w, "passthru");
	/* [한국어] 객체 시작 토큰 '{' 작성. */
	spdk_json_write_object_begin(w);
	/* [한국어] "name":"<vbdev 이름>" 키-값 쌍 작성. spdk_bdev_get_name은 bdev->name 반환. */
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&pt_node->pt_bdev));
	/* [한국어] "base_bdev_name":"<base 이름>" — 상위 도구가 위임 관계를 인지하도록. */
	spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(pt_node->base_bdev));
	/* [한국어] 객체 끝 토큰 '}' 작성. */
	spdk_json_write_object_end(w);

	return 0;  /* [한국어] 성공 항상 0. JSON 직렬화 자체가 실패하는 케이스는 writer 내부에서 처리. */
}

/* This is used to generate JSON that can configure this module to its current state. */
/*
 * [한국어]
 * vbdev_passthru_config_json - 모듈 레벨 config_json — 모든 pt_node를 RPC 호출 형태로 직렬화.
 */
static int
vbdev_passthru_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_passthru *pt_node;  /* [한국어] g_pt_nodes 순회용 임시 변수. */

	/* [한국어] 모든 활성 passthru 인스턴스를 RPC 호출 객체로 직렬화. save_config 시 호출됨. */
	TAILQ_FOREACH(pt_node, &g_pt_nodes, link) {
		/* [한국어] 현재 인스턴스의 UUID 조회 — auto-gen이면 base에서 파생된 결정적 값. */
		const struct spdk_uuid *uuid = spdk_bdev_get_uuid(&pt_node->pt_bdev);

		/* [한국어] 한 RPC 호출 객체 '{' 시작. */
		spdk_json_write_object_begin(w);
		/* [한국어] "method":"bdev_passthru_create" — 복원 시 호출할 RPC 메서드 이름. */
		spdk_json_write_named_string(w, "method", "bdev_passthru_create");
		/* [한국어] "params":{ 시작 — 메서드 인자 객체. */
		spdk_json_write_named_object_begin(w, "params");
		/* [한국어] params.base_bdev_name — 원본 base bdev 이름. */
		spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(pt_node->base_bdev));
		/* [한국어] params.name — vbdev 이름. */
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&pt_node->pt_bdev));
		if (!spdk_uuid_is_null(uuid)) {
			spdk_json_write_named_uuid(w, "uuid", uuid);  /* [한국어] 명시 UUID만 출력(자동 생성은 생략). */
		}
		/* [한국어] params 객체 종료. */
		spdk_json_write_object_end(w);
		/* [한국어] RPC 호출 객체 종료. */
		spdk_json_write_object_end(w);
	}
	return 0;  /* [한국어] 성공 항상 0. */
}

/* We provide this callback for the SPDK channel code to create a channel using
 * the channel struct we provided in our module get_io_channel() entry point. Here
 * we get and save off an underlying base channel of the device below us so that
 * we can communicate with the base bdev on a per channel basis.  If we needed
 * our own poller for this vbdev, we'd register it here.
 */
/*
 * [한국어]
 * pt_bdev_ch_create_cb - io_device 채널 생성 콜백 — base 채널 보관.
 *
 * @io_device: vbdev_passthru. @ctx_buf: 새 pt_io_channel.
 * @return: 0(성공).
 *
 * 호출 체인: spdk_get_io_channel → 등록된 콜백 → [이 함수] → spdk_bdev_get_io_channel(base).
 * 실행 컨텍스트: 채널을 요청한 thread.
 */
static int
pt_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct pt_io_channel *pt_ch = ctx_buf;
	struct vbdev_passthru *pt_node = io_device;

	pt_ch->base_ch = spdk_bdev_get_io_channel(pt_node->base_desc);  /* [한국어] base 채널 획득 (refcount++). */

	return 0;
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created. If this bdev used its own poller, we'd unregister it here.
 */
/*
 * [한국어]
 * pt_bdev_ch_destroy_cb - 채널 해제 콜백 — base 채널 반납.
 */
static void
pt_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct pt_io_channel *pt_ch = ctx_buf;

	spdk_put_io_channel(pt_ch->base_ch);  /* [한국어] base 채널 refcount--. 0이면 채널 destroy. */
}

/* Create the passthru association from the bdev and vbdev name and insert
 * on the global list. */
/*
 * [한국어]
 * vbdev_passthru_insert_name - g_bdev_names에 (vbdev_name, bdev_name, uuid) 매핑 추가.
 *
 * @bdev_name, @vbdev_name, @uuid: RPC 입력.
 * @return: 0=성공, -EEXIST=중복, -ENOMEM=할당 실패.
 *
 * vbdev_name 기준 중복 검사 — bdev_name은 동일해도 다른 vbdev_name으로 여러 passthru 생성 가능.
 *
 * 호출 체인: bdev_passthru_create_disk → [이 함수].
 */
static int
vbdev_passthru_insert_name(const char *bdev_name, const char *vbdev_name,
			   const struct spdk_uuid *uuid)
{
	struct bdev_names *name;  /* [한국어] 순회/할당 공용 포인터. */

	/* [한국어] 중복 검사 — 동일 vbdev_name이 이미 있으면 EEXIST. */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(vbdev_name, name->vbdev_name) == 0) {
			SPDK_ERRLOG("passthru bdev %s already exists\n", vbdev_name);
			return -EEXIST;  /* [한국어] 호출자(RPC)가 사용자에게 그대로 전달. */
		}
	}

	/* [한국어] calloc — 0-init 보장으로 uuid 미설정 시에도 spdk_uuid_is_null 사용 가능. */
	name = calloc(1, sizeof(struct bdev_names));
	if (!name) {
		SPDK_ERRLOG("could not allocate bdev_names\n");
		return -ENOMEM;
	}

	/* [한국어] base bdev 이름 복사 — RPC 입력이 임시일 수 있으므로 강제 strdup. */
	name->bdev_name = strdup(bdev_name);
	if (!name->bdev_name) {
		SPDK_ERRLOG("could not allocate name->bdev_name\n");
		free(name);  /* [한국어] 부분 할당 누수 방지. */
		return -ENOMEM;
	}

	/* [한국어] vbdev 이름 복사 — 동일 이유. */
	name->vbdev_name = strdup(vbdev_name);
	if (!name->vbdev_name) {
		SPDK_ERRLOG("could not allocate name->vbdev_name\n");
		free(name->bdev_name);  /* [한국어] 역순으로 해제. */
		free(name);
		return -ENOMEM;
	}

	/* [한국어] UUID 복사 — NULL UUID(자동 생성 모드)도 그대로 보관 → register에서 분기. */
	spdk_uuid_copy(&name->uuid, uuid);
	/* [한국어] 전역 리스트 꼬리에 추가 — examine 시 순회됨. */
	TAILQ_INSERT_TAIL(&g_bdev_names, name, link);

	return 0;  /* [한국어] 등록 완료. */
}

/* On init, just perform bdev module specific initialization. */
/*
 * [한국어]
 * vbdev_passthru_init - 모듈 초기화 — noop. 호출 체인: SPDK 부팅 → [이 함수].
 */
static int
vbdev_passthru_init(void)
{
	/* [한국어] passthru는 전역 상태(g_bdev_names/g_pt_nodes)를 TAILQ_HEAD_INITIALIZER로
	 *           컴파일 타임 초기화하므로 런타임 init이 필요 없다. 비교: delay 모듈은
	 *           spdk_poller_register를 위해 채널마다 초기화가 필요. */
	return 0;
}

/* Called when the entire module is being torn down. */
/*
 * [한국어]
 * vbdev_passthru_finish - 모듈 fini — g_bdev_names의 모든 매핑 free.
 */
static void
vbdev_passthru_finish(void)
{
	struct bdev_names *name;  /* [한국어] head를 반복적으로 꺼낼 임시 포인터. */

	/* [한국어] 모든 매핑을 head부터 순차 제거 — 모든 vbdev은 이미 destruct로 해제된 후 호출됨. */
	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		TAILQ_REMOVE(&g_bdev_names, name, link);  /* [한국어] 리스트에서 분리. */
		free(name->bdev_name);                    /* [한국어] strdup 메모리 해제. */
		free(name->vbdev_name);                   /* [한국어] strdup 메모리 해제. */
		free(name);                               /* [한국어] 노드 자체 해제. */
	}
}

/* During init we'll be asked how much memory we'd like passed to us
 * in bev_io structures as context. Here's where we specify how
 * much context we want per IO.
 */
/*
 * [한국어]
 * vbdev_passthru_get_ctx_size - driver_ctx 크기 알림. @return: sizeof(struct passthru_bdev_io).
 */
static int
vbdev_passthru_get_ctx_size(void)
{
	/* [한국어] bdev 코어는 모든 bdev_io에 driver_ctx 영역을 함께 할당한다. 본 모듈은 매 I/O마다
	 *           passthru_bdev_io(test 마커 + bdev_io_wait + 채널)를 보관해야 하므로 그 크기를 반환. */
	return sizeof(struct passthru_bdev_io);
}

/* Where vbdev_passthru_config_json() is used to generate per module JSON config data, this
 * function is called to output any per bdev specific methods. For the PT module, there are
 * none.
 */
/*
 * [한국어]
 * vbdev_passthru_write_config_json - per-bdev config (없음).
 */
static void
vbdev_passthru_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No config per bdev needed */
	/* [한국어] per-bdev 옵션이 없어 noop. delay 모듈과 달리 passthru는 추가 파라미터(latency 등)가
	 *           없으므로 모듈 레벨 config_json만으로 충분 — 위 vbdev_passthru_config_json 참조. */
}

/*
 * [한국어]
 * vbdev_passthru_get_memory_domains - 본 vbdev이 지원하는 메모리 도메인 목록을 base에 위임.
 *
 * passthru는 데이터 버퍼를 변형하지 않으므로 base가 지원하는 도메인을 그대로 노출.
 * 호출 체인: 상위 레이어 → bdev_fn_table.get_memory_domains → [이 함수] → spdk_bdev_get_memory_domains.
 */
static int
vbdev_passthru_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct vbdev_passthru *pt_node = (struct vbdev_passthru *)ctx;

	/* Passthru bdev doesn't work with data buffers, so it supports any memory domain used by base_bdev */
	return spdk_bdev_get_memory_domains(pt_node->base_bdev, domains, array_size);
}

/* When we register our bdev this is how we specify our entry points. */
/* [한국어] passthru의 fn_table — bdev 코어가 메타데이터/I/O dispatch에 사용. */
static const struct spdk_bdev_fn_table vbdev_passthru_fn_table = {
	.destruct		= vbdev_passthru_destruct,
	.submit_request		= vbdev_passthru_submit_request,
	.io_type_supported	= vbdev_passthru_io_type_supported,
	.get_io_channel		= vbdev_passthru_get_io_channel,
	.dump_info_json		= vbdev_passthru_dump_info_json,
	.write_config_json	= vbdev_passthru_write_config_json,
	.get_memory_domains	= vbdev_passthru_get_memory_domains,
};

/*
 * [한국어]
 * vbdev_passthru_base_bdev_hotremove_cb - hot-remove 시 본 base를 가진 모든 passthru를 unregister.
 *
 * 호출 체인: hot-remove → vbdev_passthru_base_bdev_event_cb(REMOVE) → [이 함수] → spdk_bdev_unregister.
 */
static void
vbdev_passthru_base_bdev_hotremove_cb(struct spdk_bdev *bdev_find)
{
	struct vbdev_passthru *pt_node, *tmp;  /* [한국어] SAFE 순회 — 본 루프 안에서 unregister가 노드를 제거할 수 있음. */

	/* [한국어] 동일 base를 공유하는 모든 passthru 인스턴스에 대해 unregister 발행.
	 *           unregister가 비동기이므로 spdk_bdev_unregister 호출 후 destruct가 나중에 실행됨. */
	TAILQ_FOREACH_SAFE(pt_node, &g_pt_nodes, link, tmp) {
		if (bdev_find == pt_node->base_bdev) {
			/* [한국어] 비동기 unregister 시작 — cb_fn=NULL은 결과 통지 불필요. */
			spdk_bdev_unregister(&pt_node->pt_bdev, NULL, NULL);
		}
	}
}

/* Called when the underlying base bdev triggers asynchronous event such as bdev removal. */
/*
 * [한국어]
 * vbdev_passthru_base_bdev_event_cb - base bdev 이벤트(예: REMOVE) 콜백.
 *
 * spdk_bdev_open_ext에 등록되어 base 측의 비동기 이벤트를 본 모듈로 전달.
 *
 * 호출 체인: lib/bdev (이벤트) → [이 함수] → 이벤트별 처리.
 */
static void
vbdev_passthru_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				  void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		/* [한국어] hot-remove(예: NVMe surprise removal, AIO/loop 디바이스 unlink 등) —
		 *           본 모듈은 즉시 모든 자식 vbdev을 unregister해서 상위 클라이언트가 오류를 받도록. */
		vbdev_passthru_base_bdev_hotremove_cb(bdev);
		break;
	default:
		/* [한국어] RESIZE/MEDIA_MGMT 등 다른 이벤트는 현재 미지원 — 로그만 남김. */
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/* Create and register the passthru vbdev if we find it in our list of bdev names.
 * This can be called either by the examine path or RPC method.
 */
/*
 * [한국어]
 * vbdev_passthru_register - g_bdev_names를 순회하며 bdev_name이 일치하는 매핑에 대해 vbdev 등록.
 *
 * @bdev_name: 새로 등장한 base bdev 이름(or RPC에서 지정된 이름).
 * @return: 0=성공(매칭 없으면 0), -ENODEV=base 미존재, -ENOMEM=할당 실패, 기타=등록 실패.
 *
 * 시퀀스(매칭된 매핑마다):
 *   1) pt_node 0-init 할당.
 *   2) pt_bdev.name strdup(vbdev_name).
 *   3) base bdev open_ext (write 권한 true — claim 위해 필요).
 *   4) UUID 결정: 사용자 지정 UUID가 있으면 그대로, 없으면 namespace_uuid + base->uuid 의 SHA-1.
 *   5) base에서 메타데이터 복제(blocklen/blockcnt/dif/numa 등).
 *   6) ctxt/fn_table/module 설정.
 *   7) g_pt_nodes 추가.
 *   8) spdk_io_device_register (채널 size 명시).
 *   9) spdk_bdev_module_claim_bdev — base를 본 모듈이 독점 사용 표시.
 *  10) spdk_bdev_register — vbdev을 bdev 레이어에 노출.
 *
 * 호출 체인:
 *   상위: bdev_passthru_create_disk (RPC), vbdev_passthru_examine (자동).
 *   하위: spdk_bdev_open_ext, spdk_io_device_register, spdk_bdev_module_claim_bdev, spdk_bdev_register.
 * 실행 컨텍스트: bdev 메인 thread.
 */
static int
vbdev_passthru_register(const char *bdev_name)
{
	struct bdev_names *name;
	struct vbdev_passthru *pt_node;
	struct spdk_bdev *bdev;
	struct spdk_uuid ns_uuid;
	int rc = 0;

	spdk_uuid_parse(&ns_uuid, BDEV_PASSTHRU_NAMESPACE_UUID);  /* [한국어] 매크로 문자열 → 16바이트 UUID. */

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the pt_node & bdev accordingly.
	 */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->bdev_name, bdev_name) != 0) {
			continue;  /* [한국어] 다른 base에 대한 매핑은 skip. */
		}

		SPDK_NOTICELOG("Match on %s\n", bdev_name);
		pt_node = calloc(1, sizeof(struct vbdev_passthru));
		if (!pt_node) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate pt_node\n");
			break;
		}

		pt_node->pt_bdev.name = strdup(name->vbdev_name);
		if (!pt_node->pt_bdev.name) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate pt_bdev name\n");
			free(pt_node);
			break;
		}
		pt_node->pt_bdev.product_name = "passthru";  /* [한국어] product_name은 정적 문자열로 충분. */

		/* The base bdev that we're attaching to. */
		/* [한국어] write 권한 true로 open — claim_bdev에 필요. event_cb로 hot-remove 받음. */
		rc = spdk_bdev_open_ext(bdev_name, true, vbdev_passthru_base_bdev_event_cb,
					NULL, &pt_node->base_desc);
		if (rc) {
			if (rc != -ENODEV) {
				SPDK_ERRLOG("could not open bdev %s\n", bdev_name);
			}
			free(pt_node->pt_bdev.name);
			free(pt_node);
			break;
		}
		SPDK_NOTICELOG("base bdev opened\n");

		bdev = spdk_bdev_desc_get_bdev(pt_node->base_desc);  /* [한국어] desc → bdev 포인터. */
		pt_node->base_bdev = bdev;

		if (!spdk_uuid_is_null(&name->uuid)) {
			/* Use the configured UUID */
			spdk_uuid_copy(&pt_node->pt_bdev.uuid, &name->uuid);
		} else {
			/* Generate UUID based on namespace UUID + base bdev UUID. */
			/* [한국어] UUIDv5 — 동일 base에 대해 결정적 UUID 생성(재기동 후에도 동일). */
			rc = spdk_uuid_generate_sha1(&pt_node->pt_bdev.uuid, &ns_uuid,
						     (const char *)&pt_node->base_bdev->uuid, sizeof(struct spdk_uuid));
			if (rc) {
				SPDK_ERRLOG("Unable to generate new UUID for passthru bdev\n");
				spdk_bdev_close(pt_node->base_desc);
				free(pt_node->pt_bdev.name);
				free(pt_node);
				break;
			}
		}

		/* Copy some properties from the underlying base bdev. */
		/* [한국어] base의 메타데이터를 그대로 노출(투과). */
		pt_node->pt_bdev.write_cache = bdev->write_cache;             /* [한국어] write cache 지원 여부. */
		pt_node->pt_bdev.required_alignment = bdev->required_alignment;  /* [한국어] DMA 정렬 요구사항. */
		pt_node->pt_bdev.optimal_io_boundary = bdev->optimal_io_boundary;/* [한국어] 최적 I/O 경계(NVMe NOIOB). */
		pt_node->pt_bdev.blocklen = bdev->blocklen;                   /* [한국어] 블록 크기(보통 512/4096). */
		pt_node->pt_bdev.blockcnt = bdev->blockcnt;                   /* [한국어] 총 블록 수. */

		pt_node->pt_bdev.md_interleave = bdev->md_interleave;         /* [한국어] DIF/DIX 메타데이터 interleave. */
		pt_node->pt_bdev.md_len = bdev->md_len;                       /* [한국어] 메타 길이. */
		pt_node->pt_bdev.dif_type = bdev->dif_type;                   /* [한국어] DIF 타입(0/1/2/3). */
		pt_node->pt_bdev.dif_is_head_of_md = bdev->dif_is_head_of_md; /* [한국어] DIF가 메타 헤더에 있는지. */
		pt_node->pt_bdev.dif_check_flags = bdev->dif_check_flags;     /* [한국어] DIF 검사 플래그. */
		pt_node->pt_bdev.dif_pi_format = bdev->dif_pi_format;         /* [한국어] DIF PI 포맷. */

		pt_node->pt_bdev.numa = bdev->numa;                           /* [한국어] NUMA 정보 — affinity 결정. */

		/* This is the context that is passed to us when the bdev
		 * layer calls in so we'll save our pt_bdev node here.
		 */
		pt_node->pt_bdev.ctxt = pt_node;                              /* [한국어] fn_table 콜백의 ctx 인자. */
		pt_node->pt_bdev.fn_table = &vbdev_passthru_fn_table;
		pt_node->pt_bdev.module = &passthru_if;
		TAILQ_INSERT_TAIL(&g_pt_nodes, pt_node, link);

		/* [한국어] io_device 등록 — pt_io_channel 크기 지정. spdk_get_io_channel이 자동 할당. */
		spdk_io_device_register(pt_node, pt_bdev_ch_create_cb, pt_bdev_ch_destroy_cb,
					sizeof(struct pt_io_channel),
					name->vbdev_name);
		SPDK_NOTICELOG("io_device created at: %p\n", pt_node);

		/* Save the thread where the base device is opened */
		pt_node->thread = spdk_get_thread();  /* [한국어] close를 동일 thread에서 하기 위해 보관. */

		/* [한국어] base bdev에 대한 exclusive claim — 다른 모듈이 attach 못 하게. */
		rc = spdk_bdev_module_claim_bdev(bdev, pt_node->base_desc, pt_node->pt_bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", bdev_name);
			spdk_bdev_close(pt_node->base_desc);
			TAILQ_REMOVE(&g_pt_nodes, pt_node, link);
			spdk_io_device_unregister(pt_node, NULL);
			free(pt_node->pt_bdev.name);
			free(pt_node);
			break;
		}
		SPDK_NOTICELOG("bdev claimed\n");

		/* [한국어] 마지막 단계 — vbdev을 bdev 레이어에 노출. 이 시점부터 다른 모듈/클라이언트가 사용 가능. */
		rc = spdk_bdev_register(&pt_node->pt_bdev);
		if (rc) {
			SPDK_ERRLOG("could not register pt_bdev\n");
			spdk_bdev_module_release_bdev(&pt_node->pt_bdev);
			spdk_bdev_close(pt_node->base_desc);
			TAILQ_REMOVE(&g_pt_nodes, pt_node, link);
			spdk_io_device_unregister(pt_node, NULL);
			free(pt_node->pt_bdev.name);
			free(pt_node);
			break;
		}
		SPDK_NOTICELOG("pt_bdev registered\n");
		SPDK_NOTICELOG("created pt_bdev for: %s\n", name->vbdev_name);
	}

	return rc;
}

/* Create the passthru disk from the given bdev and vbdev name. */
/*
 * [한국어]
 * bdev_passthru_create_disk - RPC가 호출하는 외부 진입점.
 *
 * @bdev_name, @vbdev_name, @uuid: RPC 입력.
 * @return: 0=성공(또는 base 미존재), 음수=errno.
 *
 * base가 없을 때(-ENODEV)는 0으로 보정 — 매핑은 등록되어 추후 examine 시 자동 활성화.
 *
 * 호출 체인: rpc_bdev_passthru_create → [이 함수] → vbdev_passthru_insert_name + vbdev_passthru_register.
 * 실행 컨텍스트: SPDK RPC poller.
 */
int
bdev_passthru_create_disk(const char *bdev_name, const char *vbdev_name,
			  const struct spdk_uuid *uuid)
{
	int rc;  /* [한국어] 단계별 결과 누적용. */

	/* Insert the bdev name into our global name list even if it doesn't exist yet,
	 * it may show up soon...
	 */
	/* [한국어] 1단계: 매핑 등록. base가 아직 없어도 등록해 두면 추후 examine 시 자동 활성화. */
	rc = vbdev_passthru_insert_name(bdev_name, vbdev_name, uuid);
	if (rc) {
		return rc;  /* [한국어] EEXIST/ENOMEM는 그대로 클라이언트에 전달. */
	}

	/* [한국어] 2단계: 즉시 vbdev 등록 시도. base가 이미 있으면 그 자리에서 활성화. */
	rc = vbdev_passthru_register(bdev_name);
	if (rc == -ENODEV) {
		/* This is not an error, we tracked the name above and it still
		 * may show up later.
		 */
		/* [한국어] base 부재는 정상 케이스 — 매핑은 보관됐고 examine 콜백에서 처리될 것. */
		SPDK_NOTICELOG("vbdev creation deferred pending base bdev arrival\n");
		rc = 0;
	}

	return rc;
}

/*
 * [한국어]
 * bdev_passthru_delete_disk - RPC가 호출하는 외부 진입점. 비동기 unregister 시작.
 *
 * @bdev_name: 삭제 대상 vbdev 이름. @cb_fn/@cb_arg: 완료 콜백.
 *
 * unregister가 즉시 시작 가능했으면 0 반환 → 매핑도 g_bdev_names에서 제거(같은 base가 다시
 * examine될 때 재생성되지 않도록). 즉시 실패면 cb_fn에 에러 전달.
 *
 * 호출 체인: rpc_bdev_passthru_delete → [이 함수] → spdk_bdev_unregister_by_name.
 * 실행 컨텍스트: SPDK RPC poller.
 */
void
bdev_passthru_delete_disk(const char *bdev_name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct bdev_names *name;  /* [한국어] g_bdev_names 매칭 노드 포인터. */
	int rc;                   /* [한국어] unregister 즉시 결과. */

	/* Some cleanup happens in the destruct callback. */
	/* [한국어] bdev 코어에 unregister 요청 — 동기 단계는 큐잉/검증, 실제 destruct는 비동기.
	 *           rc==0이면 unregister가 큐잉됐고, cb_fn은 destruct 완료 시 호출됨. */
	rc = spdk_bdev_unregister_by_name(bdev_name, &passthru_if, cb_fn, cb_arg);
	if (rc == 0) {
		/* Remove the association (vbdev, bdev) from g_bdev_names. This is required so that the
		 * vbdev does not get re-created if the same bdev is constructed at some other time,
		 * unless the underlying bdev was hot-removed.
		 */
		/* [한국어] g_bdev_names에서도 매핑 제거 — 사용자가 명시적으로 삭제했으므로
		 *           이후 동일 base가 다시 examine되어도 자동 재생성되면 안 됨. */
		TAILQ_FOREACH(name, &g_bdev_names, link) {
			if (strcmp(name->vbdev_name, bdev_name) == 0) {
				TAILQ_REMOVE(&g_bdev_names, name, link);  /* [한국어] 리스트에서 분리. */
				free(name->bdev_name);                    /* [한국어] base 이름 strdup 해제. */
				free(name->vbdev_name);                   /* [한국어] vbdev 이름 strdup 해제. */
				free(name);                               /* [한국어] 노드 자체 해제. */
				break;                                    /* [한국어] vbdev_name은 유일 — 첫 매칭에서 종료. */
			}
		}
	} else {
		cb_fn(cb_arg, rc);  /* [한국어] 즉시 실패 — 비동기 경로 안 탐. */
	}
}

/* Because we specified this function in our pt bdev function table when we
 * registered our pt bdev, we'll get this call anytime a new bdev shows up.
 * Here we need to decide if we care about it and if so what to do. We
 * parsed the config file at init so we check the new bdev against the list
 * we built up at that time and if the user configured us to attach to this
 * bdev, here's where we do it.
 */
/*
 * [한국어]
 * vbdev_passthru_examine - 새 base bdev 등록 시 자동 활성화.
 *
 * 호출 체인: lib/bdev (새 bdev 등록) → 모든 모듈 examine_config → [이 함수] → vbdev_passthru_register.
 * 실행 컨텍스트: bdev 메인 thread.
 */
static void
vbdev_passthru_examine(struct spdk_bdev *bdev)
{
	/* [한국어] 새로 나타난 bdev 이름으로 register 시도. 매칭 없으면 register 내부에서 no-op.
	 *           matching된 매핑이 있다면 즉시 vbdev이 생성/노출됨. */
	vbdev_passthru_register(bdev->name);

	spdk_bdev_module_examine_done(&passthru_if);  /* [한국어] examine 완료 알림 — 다음 모듈로 control 넘김. */
}

/* [한국어] "vbdev_passthru" 디버그 컴포넌트 등록. */
SPDK_LOG_REGISTER_COMPONENT(vbdev_passthru)
