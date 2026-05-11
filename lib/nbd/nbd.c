/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] NBD(Network Block Device) target 코어 구현 (nbd.c)
 *
 * === 파일의 역할 ===
 * SPDK bdev를 Linux 커널의 /dev/nbdN으로 노출시키는 NBD 서버 측 구현이다. NBD는 본래
 * "원격 블록 디바이스를 TCP로 노출"하는 프로토콜이지만, 로컬 unix socketpair에 NBD
 * 프로토콜을 흘려도 정상 동작하므로 SPDK는 socketpair의 한쪽 끝을 커널 nbd 모듈에
 * NBD_SET_SOCK ioctl로 넘기고, 다른 한쪽에서 SPDK가 read/write/disc 요청을 받아 SPDK bdev로
 * 전달한다. 이를 통해 SPDK 유저스페이스 bdev(예: NVMe ZNS, lvol, malloc 등)가 표준 블록
 * 디바이스(/dev/nbd0)처럼 mkfs/mount/dd 가능해진다. 본 파일은 이 게이트웨이의 모든 단계
 * (start/stop, ioctl 시퀀스, 패킷 read/write, bdev_io 변환, 에러 처리)를 구현한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   사용자 fs op (mount/dd) → /dev/nbd0 [커널 nbd client]
 *     ↓ NBD 프로토콜 over socketpair
 *   SPDK NBD server (이 파일)
 *     - poller가 socketpair 한쪽을 read → nbd_request 헤더 + write payload 수신
 *     - bdev로 spdk_bdev_read/write 발행
 *     - 완료 시 nbd_reply + read payload write
 *   spdk_bdev_*** → bdev module → 백엔드 (NVMe, malloc, ...)
 *
 * === 타 모듈과의 연결 ===
 * - 의존: linux/nbd.h(NBD ioctl/구조체), spdk/bdev(spdk_bdev_io 발행), spdk/thread(poller),
 *   spdk/sock 또는 native socketpair(2), endianness 변환(spdk/endian.h).
 * - 의존받음: lib/nbd/nbd_rpc.c (RPC 핸들러가 spdk_nbd_start/stop 호출).
 * - 데이터 흐름: 커널 → socketpair fd → poller가 read → nbd_io 객체로 매핑 →
 *   spdk_bdev_read/write → 완료 콜백 → 응답 nbd_reply 송신.
 * - 동시성: 코어 thread는 단일 reactor에 고정. 커널과 socket을 통한 통신은 비동기,
 *   bdev I/O는 SPDK 비동기 모델로 처리.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_nbd_init/fini             : NBD 서브시스템 초기화/종료.
 * - spdk_nbd_start/spdk_nbd_stop   : 디스크 1개 기준 시작/정지(공개 API).
 * - nbd_disconnect / nbd_disk_*    : 내부 헬퍼 (RPC가 사용).
 * - nbd_poll                       : 메인 poller — recv/submit/xmit 4 단계 상태 머신을 진행.
 * - nbd_submit_bdev_io             : nbd_io를 spdk_bdev_io로 변환해 발행.
 * - struct nbd_io                  : 한 개 NBD 요청의 진행 상태(state/req/resp/offset).
 * - struct spdk_nbd_disk           : 한 NBD 디바이스 인스턴스의 모든 ctx (bdev, fd, pollers, queue).
 * - 상태 머신 enum nbd_io_state_t  : RECV_REQ → RECV_PAYLOAD → submit → XMIT_RESP → XMIT_PAYLOAD.
 */

#include "spdk/stdinc.h" /* [한국어] 표준 헤더 묶음. */
#include "spdk/string.h" /* [한국어] spdk_strerror 등. */

#include <linux/nbd.h> /* [한국어] 커널 NBD API: NBD_SET_SOCK/NBD_DO_IT/NBD_DISCONNECT 등 매크로 + nbd_request/reply 구조체. */

#include "spdk/nbd.h"        /* [한국어] 공개 spdk_nbd_start/stop API 시그니처. */
#include "nbd_internal.h"    /* [한국어] 같은 모듈 RPC와 공유하는 내부 함수 시그니처. */
#include "spdk/bdev.h"        /* [한국어] spdk_bdev_io_wait_entry 등 bdev I/O API. */
#include "spdk/endian.h"      /* [한국어] from_be32/64 — NBD 프로토콜은 big-endian. */
#include "spdk/env.h"         /* [한국어] DPDK 환경 (메모리 할당 등). */
#include "spdk/likely.h"      /* [한국어] likely/unlikely 분기 힌트. */
#include "spdk/log.h"         /* [한국어] 로깅 매크로. */
#include "spdk/util.h"        /* [한국어] 유틸 매크로(SPDK_COUNTOF, MIN 등). */
#include "spdk/thread.h"      /* [한국어] poller, thread 메시지. */

#include "spdk/queue.h" /* [한국어] BSD-style TAILQ 매크로. */

#define GET_IO_LOOP_COUNT		16
/* [한국어] nbd_poll 한 번에 socketpair에서 처리할 IO 최대 개수.
 * 너무 크면 다른 poller 기아, 작으면 throughput 손해 — 16은 경험적 균형. */

#define NBD_START_BUSY_WAITING_MS	1000
/* [한국어] start 중 커널이 nbd 디바이스 준비될 때까지 폴링 대기 최대 시간(ms). */

#define NBD_STOP_BUSY_WAITING_MS	10000
/* [한국어] stop 중 in-flight IO flush 대기 최대 시간(ms). 더 길어야 안전. */

#define NBD_BUSY_POLLING_INTERVAL_US	20000
/* [한국어] start/stop 폴링 간격 (us) — busy-wait 빈도 제어. */

#define NBD_IO_TIMEOUT_S		60
/* [한국어] 단일 NBD I/O 처리 타임아웃 — 커널 nbd timeout ioctl에 전달. */

enum nbd_io_state_t {
	/* [한국어] nbd_io 한 개의 진행 단계 — 4단계 상태 머신.
	 * 한 IO는 RECV_REQ → (write면 RECV_PAYLOAD →) submit → XMIT_RESP → (read면 XMIT_PAYLOAD →) 완료. */

	/* Receiving or ready to receive nbd request header */
	NBD_IO_RECV_REQ = 0,
	/* [한국어] 요청 헤더(28바이트 nbd_request) 수신 단계.
	 * offset 필드로 부분 수신 진행 추적 (TCP recv는 partial 가능). */

	/* Receiving write payload */
	NBD_IO_RECV_PAYLOAD,
	/* [한국어] WRITE 요청의 payload 수신 단계 (payload_size 바이트). */

	/* Transmitting or ready to transmit nbd response header */
	NBD_IO_XMIT_RESP,
	/* [한국어] bdev 완료 후 응답 헤더(16바이트 nbd_reply) 송신 단계. */

	/* Transmitting read payload */
	NBD_IO_XMIT_PAYLOAD,
	/* [한국어] READ 요청의 payload 송신 단계. 송신 끝나면 IO 완료, 풀로 반환. */
};

struct nbd_io {
	/* [한국어] 한 개 NBD 요청의 모든 컨텍스트.
	 * 객체는 풀에서 할당되고 상태 머신을 통해 RECV→XMIT 단계를 순회. */

	struct spdk_nbd_disk	*nbd;
	/* [한국어] back-pointer — 어느 NBD 디바이스 소속인지. fd/queue 접근에 사용. */

	enum nbd_io_state_t	state;
	/* [한국어] 현재 단계. nbd_poll이 dispatch에 사용. */

	void			*payload;
	/* [한국어] 데이터 버퍼 — write면 사용자가 보낸 데이터, read면 bdev가 채울 영역.
	 * spdk_dma_zmalloc으로 할당해 buf_align(bdev 정렬 요구)에 맞춤. */

	uint32_t		payload_size;
	/* [한국어] payload 바이트 수 (요청 길이). */

	struct nbd_request	req;
	/* [한국어] 수신된 NBD 요청 헤더 (magic, type, handle, from, len) — big-endian 그대로 저장 후 변환해 사용. */

	struct nbd_reply	resp;
	/* [한국어] 송신할 NBD 응답 헤더 (magic, error, handle). bdev 완료 시 채워짐. */

	/*
	 * Tracks current progress on reading/writing a request,
	 * response, or payload from the nbd socket.
	 */
	uint32_t		offset;
	/* [한국어] 현재 단계에서 이미 처리한 바이트 수 — partial recv/send 처리용.
	 * 단계 전환 시 0으로 리셋. */

	/* for bdev io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	/* [한국어] bdev_io 풀 고갈 시 호출자에게 알림 콜백을 등록하는 SPDK 표준 메커니즘.
	 * spdk_bdev_queue_io_wait가 사용. 풀 슬롯 생기면 cb_fn으로 다시 깨움. */

	TAILQ_ENTRY(nbd_io)	tailq;
	/* [한국어] disk의 received/executed/processing 큐 중 하나에 끼우는 링크.
	 * 어느 큐에 있느냐가 곧 이 IO의 단계 분류를 의미. */
};

struct spdk_nbd_disk {
	/* [한국어] 한 개 NBD 디바이스(SPDK ↔ /dev/nbdN 1대1) 인스턴스의 모든 상태.
	 * 모든 필드는 단일 reactor thread(보통 마스터)에서만 접근 — lock 불필요. */

	struct spdk_bdev	*bdev;
	/* [한국어] 백킹 SPDK bdev 핸들. NULL이면 미초기화. */

	struct spdk_bdev_desc	*bdev_desc;
	/* [한국어] bdev에 대한 open descriptor — IO 발행에 필요. */

	struct spdk_io_channel	*ch;
	/* [한국어] bdev IO를 보낼 SPDK io_channel — 본 thread 컨텍스트의 채널. */

	int			dev_fd;
	/* [한국어] /dev/nbdN을 직접 open한 fd — NBD_SET_SOCK / NBD_DO_IT 호출에 사용. */

	char			*nbd_path;
	/* [한국어] "/dev/nbdN" 경로 사본 (strdup). free 책임은 본 객체. */

	int			kernel_sp_fd;
	/* [한국어] socketpair의 커널 측 끝 fd. NBD_SET_SOCK으로 커널에 인계 후 SPDK가 close.
	 * 단, 인계 직전까지는 SPDK가 보유하므로 라이프타임 추적 필요. */

	int			spdk_sp_fd;
	/* [한국어] socketpair의 SPDK 측 끝 fd. poller가 read/write로 NBD 패킷 송수신. */

	struct spdk_poller	*nbd_poller;
	/* [한국어] 메인 polling 콜백 등록 핸들. 일반(non-interrupt) 모드에서 주기적으로 호출. */

	struct spdk_interrupt	*intr;
	/* [한국어] interrupt 모드 핸들 — fd readable 시 콜백. epoll 기반.
	 * SPDK는 polling/interrupt 두 모드를 지원, NBD는 둘 다 가능. */

	bool			interrupt_mode;
	/* [한국어] true면 interrupt 모드로 등록됨 (poller 대신 intr 사용). */

	uint32_t		buf_align;
	/* [한국어] payload 버퍼 정렬 요구 — bdev module이 요구하는 메모리 정렬(예: 512B). */

	struct spdk_poller	*retry_poller;
	/* [한국어] start 단계에서 커널이 준비될 때까지 재시도하는 poller. */

	int			retry_count;
	/* [한국어] start 재시도 횟수 카운터 — 한도 초과 시 실패 처리. */

	/* Synchronize nbd_start_kernel pthread and nbd_stop */
	bool			has_nbd_pthread;
	/* [한국어] NBD_DO_IT을 별도 pthread에서 실행 중인지 표식. stop 시 안전한 join 판정용. */

	struct nbd_io		*io_in_recv;
	/* [한국어] 현재 헤더/payload를 수신 중인 IO 한 개를 가리키는 포인터.
	 * NBD는 in-order이므로 동시에 수신 중인 IO는 최대 1개. */

	TAILQ_HEAD(, nbd_io)	received_io_list;
	/* [한국어] 헤더+payload 수신 완료, 아직 bdev에 submit하지 않은 IO 큐. */

	TAILQ_HEAD(, nbd_io)	executed_io_list;
	/* [한국어] bdev에 submit된 IO 큐 — 완료 콜백을 기다린다. */

	TAILQ_HEAD(, nbd_io)	processing_io_list;
	/* [한국어] bdev 완료 후 응답 송신을 기다리는 IO 큐. */

	bool			is_started;
	/* [한국어] start 시퀀스 완료 여부. 진짜 IO 흐름이 가능한 시점부터 true. */

	bool			is_closing;
	/* [한국어] disconnect 진행 중 표식 — 새 IO 수신 차단, in-flight 처리 후 정리. */

	/* count of nbd_io in spdk_nbd_disk */
	int			io_count;
	/* [한국어] 현재 살아 있는 nbd_io 수 — 자원 추적/정리 동기화에 사용. */

	TAILQ_ENTRY(spdk_nbd_disk)	tailq;
	/* [한국어] g_spdk_nbd.disk_head 전역 TAILQ에 자신을 끼우는 링크. */
};

struct spdk_nbd_disk_globals {
	/* [한국어] 모든 NBD 디스크를 모아 둔 전역 컨테이너.
	 * 단일 reactor에서만 변경되므로 lock 불필요. */

	TAILQ_HEAD(, spdk_nbd_disk)	disk_head;
	/* [한국어] 등록된 모든 spdk_nbd_disk 리스트. find/walk에 사용. */
};

static struct spdk_nbd_disk_globals g_spdk_nbd;
/* [한국어] 모듈 전역 상태 — init에서 head를 INIT, fini에서 비움. */
static spdk_nbd_fini_cb g_fini_cb_fn;
/* [한국어] fini 완료 시 호출할 사용자 콜백. */
static void *g_fini_cb_arg;
/* [한국어] g_fini_cb_fn에 전달할 ctx. */

static void _nbd_fini(void *arg1);
/* [한국어] 비동기 fini 진행자(자기 자신 메시지로 재호출). 전방 선언. */

static int nbd_submit_bdev_io(struct spdk_nbd_disk *nbd, struct nbd_io *io);
/* [한국어] 한 개 nbd_io를 spdk_bdev_io로 변환해 발행. 전방 선언. */
static int nbd_io_recv_internal(struct spdk_nbd_disk *nbd);
/* [한국어] socket에서 헤더/payload를 가능한 만큼 수신. 전방 선언. */

/*
 * [한국어]
 * spdk_nbd_init - NBD 모듈 전역 상태 초기화 (앱 시작 시 1회 호출).
 *
 * @return: 항상 0 (현재 실패 경로 없음).
 *
 * 호출 체인:
 *   subsystem init → [spdk_nbd_init] → TAILQ_INIT
 */
int
spdk_nbd_init(void)
{
	TAILQ_INIT(&g_spdk_nbd.disk_head); /* [한국어] 전역 디스크 리스트 헤드를 빈 상태로 초기화. */

	return 0;
}

/*
 * [한국어]
 * _nbd_fini - 모든 NBD 디스크를 안전하게 stop하고 fini 콜백을 트리거하는 비동기 진행자.
 *
 * @arg1: 현재 미사용 (메시지 시그니처 호환).
 *
 * 비동기 동작: 진행 중인 디스크를 spdk_nbd_stop으로 닫고, 디스크가 모두 사라질 때까지
 * spdk_thread_send_msg로 자기 자신을 다시 큐잉. 완전히 비면 g_fini_cb_fn을 호출.
 *
 * 호출 체인:
 *   spdk_nbd_fini → [_nbd_fini] (반복) → 모두 정리 후 사용자 콜백
 */
static void
_nbd_fini(void *arg1)
{
	struct spdk_nbd_disk *nbd, *nbd_tmp; /* [한국어] foreach_safe용 — 순회 중 제거 안전. */

	TAILQ_FOREACH_SAFE(nbd, &g_spdk_nbd.disk_head, tailq, nbd_tmp) {
		/* [한국어] 모든 디스크 walk — 아직 closing이 아니면 stop을 트리거. */
		if (!nbd->is_closing) {
			spdk_nbd_stop(nbd);
		}
	}

	/* Check if all nbds closed */
	if (!TAILQ_FIRST(&g_spdk_nbd.disk_head)) {
		/* [한국어] 모든 디스크가 사라졌으면 사용자 fini 콜백 호출 후 종료. */
		g_fini_cb_fn(g_fini_cb_arg);
	} else {
		/* [한국어] 아직 정리되지 않은 디스크가 있으면 자신을 다시 큐잉해 다음 사이클에 재시도.
		 * stop이 비동기이므로 즉시 끝나지 않을 수 있어 폴링 형태로 대기. */
		spdk_thread_send_msg(spdk_get_thread(),
				     _nbd_fini, NULL);
	}
}

/*
 * [한국어]
 * spdk_nbd_fini - NBD 모듈 종료 진입점 (사용자 fini 콜백 등록 + 비동기 정리 시작).
 *
 * @cb_fn : 정리 완료 시 호출할 콜백.
 * @cb_arg: 콜백 ctx.
 *
 * 호출 체인:
 *   subsystem fini → [spdk_nbd_fini] → _nbd_fini (비동기 루프)
 */
void
spdk_nbd_fini(spdk_nbd_fini_cb cb_fn, void *cb_arg)
{
	g_fini_cb_fn = cb_fn; /* [한국어] 사용자 콜백 저장. */
	g_fini_cb_arg = cb_arg; /* [한국어] 콜백 ctx 저장. */

	_nbd_fini(NULL); /* [한국어] 첫 회 진행자 호출 — 이후 자기 자신 메시지로 폴링. */
}

/*
 * [한국어]
 * nbd_disk_register - 새 디스크를 전역 리스트에 등록 (중복 nbd_path 거부).
 *
 * @nbd  : 등록할 디스크 (nbd_path 채워져 있어야 함).
 * @return: 0 성공, -EBUSY 중복.
 */
static int
nbd_disk_register(struct spdk_nbd_disk *nbd)
{
	/* Make sure nbd_path is not used in this SPDK app */
	if (nbd_disk_find_by_nbd_path(nbd->nbd_path)) {
		/* [한국어] 같은 /dev/nbdN을 우리가 이미 사용 중 — 충돌. */
		SPDK_NOTICELOG("%s is already exported\n", nbd->nbd_path);
		return -EBUSY;
	}

	TAILQ_INSERT_TAIL(&g_spdk_nbd.disk_head, nbd, tailq);
	/* [한국어] 전역 리스트 끝에 추가. 단일 thread 변경이라 락 불필요. */

	return 0;
}

/*
 * [한국어]
 * nbd_disk_unregister - 디스크를 전역 리스트에서 제거 (등록 안 됐으면 no-op).
 *
 * @nbd: 제거 대상.
 *
 * stop 경로가 등록 전에 호출되는 경우(start 도중 실패 등)도 안전하게 처리.
 */
static void
nbd_disk_unregister(struct spdk_nbd_disk *nbd)
{
	struct spdk_nbd_disk *nbd_idx, *nbd_tmp; /* [한국어] safe walker용 임시. */

	/*
	 * nbd disk may be stopped before registered.
	 * check whether it was registered.
	 */
	TAILQ_FOREACH_SAFE(nbd_idx, &g_spdk_nbd.disk_head, tailq, nbd_tmp) {
		if (nbd == nbd_idx) {
			/* [한국어] 일치하는 노드를 찾으면 제거 후 break. */
			TAILQ_REMOVE(&g_spdk_nbd.disk_head, nbd_idx, tailq);
			break;
		}
	}
}

/*
 * [한국어]
 * nbd_disk_find_by_nbd_path - 경로 문자열로 등록된 디스크 검색.
 *
 * @nbd_path: 검색 키.
 * @return  : 일치하는 디스크 또는 NULL.
 */
struct spdk_nbd_disk *
nbd_disk_find_by_nbd_path(const char *nbd_path)
{
	struct spdk_nbd_disk *nbd; /* [한국어] 순회용 임시. */

	/*
	 * check whether nbd has already been registered by nbd path.
	 */
	TAILQ_FOREACH(nbd, &g_spdk_nbd.disk_head, tailq) {
		if (!strcmp(nbd->nbd_path, nbd_path)) {
			/* [한국어] 정확히 동일 경로면 매치. */
			return nbd;
		}
	}

	return NULL; /* [한국어] 미발견. */
}

/*
 * [한국어]
 * nbd_disk_first - 등록된 디스크 리스트의 첫 노드.
 */
struct spdk_nbd_disk *nbd_disk_first(void)
{
	return TAILQ_FIRST(&g_spdk_nbd.disk_head);
	/* [한국어] 헤드 첫 원소 반환 — 비어 있으면 NULL. */
}

/*
 * [한국어]
 * nbd_disk_next - 주어진 노드의 다음 디스크.
 *
 * @prev: 현재 노드.
 */
struct spdk_nbd_disk *nbd_disk_next(struct spdk_nbd_disk *prev)
{
	return TAILQ_NEXT(prev, tailq);
	/* [한국어] TAILQ 매크로로 다음 노드 반환. */
}

/*
 * [한국어]
 * nbd_disk_get_nbd_path - 디스크의 /dev/nbdN 경로 반환.
 */
const char *
nbd_disk_get_nbd_path(struct spdk_nbd_disk *nbd)
{
	return nbd->nbd_path;
	/* [한국어] 내부 strdup된 경로 문자열 반환 — 라이프타임 디스크와 동일. */
}

/*
 * [한국어]
 * nbd_disk_get_bdev_name - 디스크의 백킹 bdev 이름 반환.
 */
const char *
nbd_disk_get_bdev_name(struct spdk_nbd_disk *nbd)
{
	return spdk_bdev_get_name(nbd->bdev);
	/* [한국어] bdev 모듈이 보관하는 이름 문자열 — bdev이 살아 있는 동안 유효. */
}

/*
 * [한국어]
 * spdk_nbd_write_config_json - 모든 NBD 디스크 설정을 JSON으로 직렬화 (RPC save_config용).
 *
 * @w: JSON write context (배열 모양으로 출력).
 *
 * 각 디스크는 nbd_start_disk RPC method 호출 형태로 기록되어, load_config 시 그대로 재현된다.
 */
void
spdk_nbd_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_nbd_disk *nbd; /* [한국어] 순회 변수. */

	spdk_json_write_array_begin(w); /* [한국어] '[' — 결과는 method 호출 객체의 배열. */

	TAILQ_FOREACH(nbd, &g_spdk_nbd.disk_head, tailq) {
		/* [한국어] 등록된 모든 디스크 walk. */
		spdk_json_write_object_begin(w); /* [한국어] '{' 시작 — 한 method 호출 객체. */

		spdk_json_write_named_string(w, "method", "nbd_start_disk");
		/* [한국어] "method": "nbd_start_disk" — load_config가 호출할 RPC 이름. */

		spdk_json_write_named_object_begin(w, "params"); /* [한국어] params 객체 시작. */
		spdk_json_write_named_string(w, "nbd_device",  nbd_disk_get_nbd_path(nbd));
		/* [한국어] params.nbd_device — 그대로 사용한 슬롯이 다음 부팅에서도 동일하게 적용되도록. */
		spdk_json_write_named_string(w, "bdev_name", nbd_disk_get_bdev_name(nbd));
		/* [한국어] params.bdev_name — 백킹 bdev 식별. */
		spdk_json_write_object_end(w); /* [한국어] params 종결. */

		spdk_json_write_object_end(w); /* [한국어] method call 객체 종결. */
	}

	spdk_json_write_array_end(w); /* [한국어] ']'. */
}

/*
 * [한국어]
 * nbd_disconnect - 커널에 NBD_DISCONNECT ioctl을 발행해 transmission 단계를 종료시킴.
 *
 * @nbd: 대상 디스크.
 *
 * 이 ioctl은 즉시 반환하며, 커널 nbd가 곧이어 NBD_CMD_DISC 타입의 IO를 SPDK 측에 보낸다.
 * SPDK는 그 메시지를 보고 정리 단계를 진행한다(소프트 단절 — 진행 중 IO를 안전하게 마무리).
 *
 * 호출 체인:
 *   nbd_rpc disconnect thread → [nbd_disconnect] → ioctl(NBD_DISCONNECT)
 */
void
nbd_disconnect(struct spdk_nbd_disk *nbd)
{
	/*
	 * nbd soft-disconnection to terminate transmission phase.
	 * After receiving this ioctl command, nbd kernel module will send
	 * a NBD_CMD_DISC type io to nbd server in order to inform server.
	 */
	ioctl(nbd->dev_fd, NBD_DISCONNECT);
	/* [한국어] /dev/nbdN fd에 NBD_DISCONNECT ioctl — 인자 없음. 반환값 무시(에러여도 정리 진행). */
}

/*
 * [한국어]
 * nbd_get_io - nbd_io 객체를 새로 할당해 카운터 증가.
 *
 * @nbd: 소속 디스크.
 * @return: 초기화된 nbd_io 또는 NULL(OOM).
 *
 * 응답 magic 필드를 미리 채워 둔다 — 응답 송신 시 매번 채울 필요 없음.
 */
static struct nbd_io *
nbd_get_io(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io; /* [한국어] 임시 포인터. */

	io = calloc(1, sizeof(*io)); /* [한국어] zero-init 동적 할당 — 모든 필드 0으로 시작. */
	if (!io) {
		return NULL; /* [한국어] OOM. */
	}

	io->nbd = nbd; /* [한국어] back-pointer 설정. */
	to_be32(&io->resp.magic, NBD_REPLY_MAGIC);
	/* [한국어] NBD 응답은 반드시 NBD_REPLY_MAGIC(0x67446698)로 시작 — big-endian 인코딩. */

	nbd->io_count++; /* [한국어] 살아 있는 IO 카운트 증가 (단일 thread이므로 비원자 OK). */

	return io;
}

/*
 * [한국어]
 * nbd_put_io - nbd_io 자원 해제.
 *
 * @nbd: 소속 디스크.
 * @io : 해제 대상.
 */
static void
nbd_put_io(struct spdk_nbd_disk *nbd, struct nbd_io *io)
{
	if (io->payload) {
		/* [한국어] DMA 정렬된 payload 버퍼는 spdk_dma_zmalloc으로 할당했으므로 spdk_free로 해제. */
		spdk_free(io->payload);
	}
	free(io); /* [한국어] nbd_io 본체 해제 (calloc과 짝). */

	nbd->io_count--; /* [한국어] 살아 있는 IO 카운트 감소. */
}

/*
 * Check whether received nbd_io are all executed,
 * and put back executed nbd_io instead of transmitting them
 *
 * \return 1 there is still some nbd_io under executing
 *         0 all nbd_io gotten are freed.
 */
/*
 * [한국어]
 * nbd_cleanup_io - stop 경로에서 잔여 IO를 모두 회수.
 *
 * @nbd : 대상 디스크.
 * @return: 1 = 아직 처리 중인 IO 있음(재시도), 0 = 모두 정리됨.
 *
 * 단계:
 *  1) socket에서 더 이상 read할 게 없을 때까지 잔여 명령 흡수(헤더라도 받아 정리).
 *  2) 수신 도중이던 IO가 있으면 즉시 free.
 *  3) bdev에 떠 있는 IO가 남아 있으면 1을 반환해 호출자가 다시 시도하게 함.
 */
static int
nbd_cleanup_io(struct spdk_nbd_disk *nbd)
{
	/* Try to read the remaining nbd commands in the socket */
	while (nbd_io_recv_internal(nbd) > 0);
	/* [한국어] socket에 남아 있는 NBD 패킷 모두 비움. EAGAIN/0 반환되면 종료. */

	/* free io_in_recv */
	if (nbd->io_in_recv != NULL) {
		/* [한국어] partial 수신 중이던 IO는 의미 없으므로 즉시 폐기. */
		nbd_put_io(nbd, nbd->io_in_recv);
		nbd->io_in_recv = NULL;
	}

	/*
	 * Some nbd_io may be under executing in bdev.
	 * Wait for their done operation.
	 */
	if (nbd->io_count != 0) {
		/* [한국어] bdev 또는 송신 큐에 남은 IO가 있으면 정리 미완료. */
		return 1;
	}

	return 0; /* [한국어] 모든 IO 회수 완료. */
}

/*
 * [한국어]
 * _nbd_stop - spdk_nbd_disk 한 개의 모든 자원을 해제하는 실제 종료 루틴.
 *
 * @arg   : 대상 spdk_nbd_disk* (poller 등록 시그니처 호환을 위해 void*).
 * @return: 항상 0 또는 SPDK_POLLER_BUSY (재시도 모드일 때).
 *
 * 이 함수는 두 가지 모드로 호출된다:
 *  (1) spdk_nbd_stop이 정상 흐름에서 직접 호출 — 한 번만 실행되어 자원 해제 완료.
 *  (2) NBD_DO_IT pthread가 아직 살아 있으면 retry_poller로 자기 자신을 주기 호출 —
 *      pthread 종료(NBD_DO_IT ioctl 반환)를 기다린 뒤 정리를 마무리한다.
 *
 * 단계:
 *  1) nbd_poller / intr 등 fd 이벤트 소스 해제 (더 이상 새 IO를 polling하지 않게).
 *  2) socketpair 양 끝(spdk_sp_fd, kernel_sp_fd) close — 커널이 NBD_DO_IT에서 빠져나오게.
 *  3) NBD_DO_IT pthread가 아직 살아 있으면 retry_poller 등록 후 반환 (재진입 대기).
 *  4) /dev/nbdN fd가 살아 있으면 NBD_CLEAR_QUE + NBD_CLEAR_SOCK로 커널 큐 비우고 close.
 *  5) 경로/io_channel/bdev descriptor 순으로 해제.
 *  6) 전역 리스트에서 unregister 후 nbd 본체 free.
 *
 * 실행 컨텍스트: 본 디스크 소속 SPDK reactor thread.
 *
 * 호출 체인:
 *   spdk_nbd_stop → [_nbd_stop] → (필요시 자기 자신을 retry_poller로 재호출)
 *   nbd_poll(에러 검출) → [_nbd_stop]
 *   nbd_enable_kernel(start 실패) → [_nbd_stop]
 */
static int
_nbd_stop(void *arg)
{
	struct spdk_nbd_disk *nbd = arg; /* [한국어] poller 시그니처에서 nbd 복원. */

	if (nbd->nbd_poller) {
		/* [한국어] 메인 IO poller 해제 — 이후 nbd_poll 호출 중단. */
		spdk_poller_unregister(&nbd->nbd_poller);
	}

	if (nbd->intr) {
		/* [한국어] interrupt 모드 핸들 해제 — epoll에서 fd 제거. */
		spdk_interrupt_unregister(&nbd->intr);
	}

	if (nbd->spdk_sp_fd >= 0) {
		/* [한국어] SPDK 측 socketpair fd close.
		 * 커널 측에서 보면 peer가 닫혔으므로 NBD_DO_IT ioctl이 곧 반환된다. */
		close(nbd->spdk_sp_fd);
		nbd->spdk_sp_fd = -1; /* [한국어] 더블 close 방지를 위한 invalid 마킹. */
	}

	if (nbd->kernel_sp_fd >= 0) {
		/* [한국어] 커널에 인계되기 전이면 우리가 들고 있는 fd를 close.
		 * 이미 NBD_SET_SOCK으로 인계된 후라면 dup된 커널 측 fd는 별도이므로 영향 없음. */
		close(nbd->kernel_sp_fd);
		nbd->kernel_sp_fd = -1;
	}

	/* Continue the stop procedure after the exit of nbd_start_kernel pthread */
	if (nbd->has_nbd_pthread) {
		/* [한국어] NBD_DO_IT을 들고 있는 pthread가 아직 살아 있는 경우 — 자원 정리를 미뤄야 한다.
		 * 위에서 spdk_sp_fd를 close했으므로 곧 ioctl이 풀려 pthread가 has_nbd_pthread=false로 끄게 된다. */
		if (nbd->retry_poller == NULL) {
			/* [한국어] 첫 호출: retry_poller를 NBD_BUSY_POLLING_INTERVAL_US 주기로 등록.
			 * 만료 횟수는 NBD_STOP_BUSY_WAITING_MS / interval = 약 500회 (10초/20ms). */
			nbd->retry_count = NBD_STOP_BUSY_WAITING_MS * 1000ULL / NBD_BUSY_POLLING_INTERVAL_US;
			nbd->retry_poller = SPDK_POLLER_REGISTER(_nbd_stop, nbd,
					    NBD_BUSY_POLLING_INTERVAL_US);
			return SPDK_POLLER_BUSY; /* [한국어] poller 본인이 BUSY로 보고 — 다음 사이클에 재진입. */
		}

		if (nbd->retry_count-- > 0) {
			/* [한국어] 카운트 남았으면 다음 인터벌까지 재시도 — pthread가 종료되길 기다림. */
			return SPDK_POLLER_BUSY;
		}

		SPDK_ERRLOG("Failed to wait for returning of NBD_DO_IT ioctl.\n");
		/* [한국어] 한도 초과 — 실패 로그 후 강제 자원 해제 진행 (leak 위험 감수). */
	}

	if (nbd->retry_poller) {
		/* [한국어] retry_poller가 있으면 해제 — pthread 종료 확인 후 정상 경로 진입. */
		spdk_poller_unregister(&nbd->retry_poller);
	}

	if (nbd->dev_fd >= 0) {
		/* Clear nbd device only if it is occupied by SPDK app */
		if (nbd->nbd_path && nbd_disk_find_by_nbd_path(nbd->nbd_path)) {
			/* [한국어] 우리가 등록한 디스크가 맞으면 커널 측에 정리 ioctl 발행.
			 * NBD_CLEAR_QUE: 커널 nbd 요청 큐 비움.
			 * NBD_CLEAR_SOCK: 인계받은 socket fd 분리 (다른 프로세스가 다시 NBD_SET_SOCK 가능). */
			ioctl(nbd->dev_fd, NBD_CLEAR_QUE);
			ioctl(nbd->dev_fd, NBD_CLEAR_SOCK);
		}
		close(nbd->dev_fd); /* [한국어] /dev/nbdN fd close — 마지막 참조이므로 커널 nbd가 idle 상태로. */
	}

	if (nbd->nbd_path) {
		/* [한국어] strdup된 경로 문자열 해제. */
		free(nbd->nbd_path);
	}

	if (nbd->ch) {
		/* [한국어] bdev IO 채널 반환 — bdev 모듈에서 채널 RC 감소. */
		spdk_put_io_channel(nbd->ch);
		nbd->ch = NULL;
	}

	if (nbd->bdev_desc) {
		/* [한국어] bdev 오픈 디스크립터 close — bdev 측의 reader RC 감소. */
		spdk_bdev_close(nbd->bdev_desc);
		nbd->bdev_desc = NULL;
	}

	nbd_disk_unregister(nbd); /* [한국어] 전역 TAILQ에서 제거 (등록 안 됐어도 안전한 no-op). */

	free(nbd); /* [한국어] nbd 본체 해제 — 이후 nbd 포인터 dangling. */

	return 0; /* [한국어] 정상 종료. */
}

/*
 * [한국어]
 * spdk_nbd_stop - NBD 디스크 종료 진입점 (공개 API).
 *
 * @nbd   : 종료할 디스크 (NULL 허용 — no-op).
 * @return: 0 = 즉시 정리 완료, 1 = 비동기 정리 진행 중(호출자가 다시 호출).
 *
 * 흐름:
 *  - 닫는 중 표식(is_closing=true) → poller가 새 IO 받지 않음.
 *  - 아직 start가 끝나지 않았으면 1을 반환해 호출자가 나중에 다시 호출하게 함
 *    (start 비동기 단계가 자기 자신의 정리 경로로 자연스럽게 진입).
 *  - 잔여 IO를 nbd_cleanup_io로 회수, 모두 비면 _nbd_stop으로 자원 해제.
 *
 * 호출 체인:
 *   _nbd_fini → [spdk_nbd_stop] → nbd_cleanup_io / _nbd_stop
 *   nbd_poll(closing 감지) → [spdk_nbd_stop]
 */
int
spdk_nbd_stop(struct spdk_nbd_disk *nbd)
{
	int rc = 0; /* [한국어] 반환 코드 — 0 정리 완료, 1 미완료. */

	if (nbd == NULL) {
		/* [한국어] 방어적 NULL 체크 — _nbd_fini의 walker가 도중에 free된 항목을 건드릴 가능성 대비. */
		return rc;
	}

	nbd->is_closing = true; /* [한국어] 새 nbd_io 수신 차단 — recv path가 이 플래그를 검사. */

	/* if nbd is not started, it will continue to call nbd stop later */
	if (!nbd->is_started) {
		/* [한국어] start가 아직 완료되지 않았으면 안전하게 정리할 시점이 아님. 호출자가 재시도해야 함. */
		return 1;
	}

	/*
	 * Stop action should be called only after all nbd_io are executed.
	 */

	rc = nbd_cleanup_io(nbd); /* [한국어] socket 잔여 패킷 흡수 + in-flight bdev IO 추적. */
	if (!rc) {
		/* [한국어] 모든 IO가 회수되었으면 즉시 정리. */
		_nbd_stop(nbd);
	}

	return rc;
}

/*
 * [한국어]
 * nbd_socket_rw - non-blocking socketpair에서 read/write 한 번 시도하고 진행 바이트 수 반환.
 *
 * @fd     : 대상 fd (보통 nbd->spdk_sp_fd).
 * @buf    : 버퍼 포인터.
 * @length : 시도할 바이트 수.
 * @read_op: true면 read, false면 write.
 * @return : > 0 진행 바이트 수, 0 = EAGAIN(지금은 진행 불가), 음수 = -errno(연결 종료/실패).
 *
 * SPDK reactor는 polling 모델이므로 socket은 SOCK_NONBLOCK으로 설정되어 있다. 따라서
 * EAGAIN(실제로는 EWOULDBLOCK과 동치)은 정상 케이스 — 그냥 다음 polling 사이클에 다시 시도.
 * read=0(EOF)은 peer가 socket을 닫은 상태(예: 커널 nbd 모듈이 NBD_DO_IT에서 빠져나옴)이며
 * NBD에서는 곧 -EIO로 매핑해 정리 경로로 진입한다.
 *
 * 실행 컨텍스트: 본 디스크 소속 reactor thread.
 */
static int64_t
nbd_socket_rw(int fd, void *buf, size_t length, bool read_op)
{
	ssize_t rc; /* [한국어] read/write 반환값 임시. */

	if (read_op) {
		/* [한국어] kernel→spdk 방향: NBD 요청 헤더/payload 수신. */
		rc = read(fd, buf, length);
	} else {
		/* [한국어] spdk→kernel 방향: NBD 응답 헤더/payload 송신. */
		rc = write(fd, buf, length);
	}

	if (rc == 0) {
		/* [한국어] read EOF — peer(커널) 측이 close. 연결 종료로 보고 -EIO 보고. */
		return -EIO;
	} else if (rc == -1) {
		if (errno != EAGAIN) {
			/* [한국어] EAGAIN 외의 에러 (ECONNRESET, EBADF 등)는 종료 트리거. */
			return -errno;
		}
		return 0; /* [한국어] EAGAIN — non-blocking에서 정상. 0 진행으로 보고. */
	} else {
		return rc; /* [한국어] 진행 바이트 수. partial 가능 — 호출자가 offset 누적. */
	}
}

/*
 * [한국어]
 * nbd_io_done - bdev I/O 완료 콜백. nbd_io를 processing_io_list → executed_io_list로 이동.
 *
 * @bdev_io: 완료된 spdk_bdev_io (NULL = bdev 발행 실패로 합성된 완료).
 * @success: bdev 모듈 보고 결과 (true 성공).
 * @cb_arg : 짝이 되는 nbd_io*.
 *
 * 동작:
 *  1) NBD 응답 error 필드를 0(성공) 또는 EIO(실패)로 채운다 (big-endian).
 *  2) 응답 handle을 요청 handle 그대로 복사 — NBD 프로토콜은 client가 이 값을 IO 식별에 사용.
 *  3) interrupt 모드이고 executed_io_list가 비어 있던 상태에서 새 항목이 추가된다면,
 *     poller에 EVENT_OUT을 활성화해 nbd_io_xmit이 호출되도록 한다.
 *  4) processing → executed 큐로 이동, bdev_io 자원 해제.
 *
 * 실행 컨텍스트: bdev 모듈이 부르는 콜백 → SPDK 비동기 모델상 본 디스크의 thread.
 *
 * 호출 체인:
 *   spdk_bdev_read/write/flush/unmap 완료 → [nbd_io_done] → 송신 큐로 이동
 *   nbd_submit_bdev_io 실패 시 합성 호출 → [nbd_io_done(NULL, false, io)]
 */
static void
nbd_io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct nbd_io	*io = cb_arg; /* [한국어] 콜백 ctx에서 짝 nbd_io 복원. */
	struct spdk_nbd_disk *nbd = io->nbd; /* [한국어] 큐 조작에 필요한 디스크 핸들. */

	if (success) {
		/* [한국어] 성공 — error=0 (이미 zero-init이지만 명시). big-endian이지만 0은 동일. */
		io->resp.error = 0;
	} else {
		/* [한국어] 실패 — NBD 프로토콜 표준 errno EIO를 big-endian으로 인코딩. */
		to_be32(&io->resp.error, EIO);
	}

	memcpy(&io->resp.handle, &io->req.handle, sizeof(io->resp.handle));
	/* [한국어] NBD client(커널)가 발행한 handle을 그대로 응답에 복사 — 응답을 어느 요청과 짝지을지 식별. */

	/* When there begins to have executed_io, enable socket writable notice in order to
	 * get it processed in nbd_io_xmit
	 */
	if (nbd->interrupt_mode && TAILQ_EMPTY(&nbd->executed_io_list)) {
		/* [한국어] interrupt 모드: 송신 가능해질 때까지 EPOLLOUT 알림이 필요.
		 * 이전엔 빈 큐였으므로 OUT 이벤트를 비활성화했었다 — 지금 첫 항목이 들어가니 다시 켠다. */
		spdk_interrupt_set_event_types(nbd->intr, SPDK_INTERRUPT_EVENT_IN | SPDK_INTERRUPT_EVENT_OUT);
	}

	TAILQ_REMOVE(&nbd->processing_io_list, io, tailq); /* [한국어] processing(=in-flight) 큐에서 제거. */
	TAILQ_INSERT_TAIL(&nbd->executed_io_list, io, tailq); /* [한국어] 송신 대기 큐에 끼움 — 다음 nbd_io_xmit이 처리. */

	if (bdev_io != NULL) {
		/* [한국어] 정상 완료된 bdev_io는 풀로 반환. NULL이면 발행 실패 합성 호출이라 free 불필요. */
		spdk_bdev_free_io(bdev_io);
	}
}

/*
 * [한국어]
 * nbd_resubmit_io - bdev_io 풀이 다시 가용해졌을 때 spdk_bdev_queue_io_wait가 호출하는 콜백.
 *
 * @arg: 대기 중이던 nbd_io*.
 *
 * 단순히 nbd_submit_bdev_io를 다시 호출 — 풀이 한 슬롯이라도 가용하므로 보통 성공한다.
 * 실패 시 INFO 로그만 남기고 nbd_submit_bdev_io 내부에서 nbd_io_done(NULL,false,...) 처리됨.
 *
 * 실행 컨텍스트: bdev 모듈의 io_wait 통보 → 본 디스크 thread.
 */
static void
nbd_resubmit_io(void *arg)
{
	struct nbd_io *io = (struct nbd_io *)arg; /* [한국어] 대기 등록 시 보관한 nbd_io 복원. */
	struct spdk_nbd_disk *nbd = io->nbd; /* [한국어] 디스크 백포인터. */
	int rc = 0; /* [한국어] 결과 저장 — 0이면 발행 OK(완료는 비동기). */

	rc = nbd_submit_bdev_io(nbd, io); /* [한국어] 다시 발행 시도. 내부적으로 풀 부족 시 또 큐잉. */
	if (rc) {
		/* [한국어] nbd_submit_bdev_io는 보통 0만 반환(실패는 nbd_io_done 합성 처리)하지만
		 * 방어적 로깅. type은 big-endian이므로 from_be32로 디코드. */
		SPDK_INFOLOG(nbd, "nbd: io resubmit for dev %s , io_type %d, returned %d.\n",
			     nbd_disk_get_bdev_name(nbd), from_be32(&io->req.type), rc);
	}
}

/*
 * [한국어]
 * nbd_queue_io - bdev_io 풀이 고갈된 상황에서 spdk_bdev_queue_io_wait로 대기 등록.
 *
 * @io: 발행에 실패한(ENOMEM) nbd_io.
 *
 * SPDK bdev_io 풀(코어 mempool)은 capacity가 정해져 있어 일시적으로 비어 있을 수 있다.
 * 그 경우 호출자가 풀 가용 시 callback을 받게 등록 가능 — 본 함수가 그 등록을 수행.
 * 등록 자체가 실패하면(예: 디스크 종료 중) 합성 EIO 완료로 처리해 응답 경로로 흘려보낸다.
 */
static void
nbd_queue_io(struct nbd_io *io)
{
	int rc; /* [한국어] queue_io_wait 결과. */
	struct spdk_bdev *bdev = io->nbd->bdev; /* [한국어] 대기 등록 대상 bdev. */

	io->bdev_io_wait.bdev = bdev; /* [한국어] 어느 bdev 풀을 기다리는지. */
	io->bdev_io_wait.cb_fn = nbd_resubmit_io; /* [한국어] 가용 시 재발행 콜백. */
	io->bdev_io_wait.cb_arg = io; /* [한국어] 콜백에 전달할 ctx — 자기 자신. */

	rc = spdk_bdev_queue_io_wait(bdev, io->nbd->ch, &io->bdev_io_wait);
	/* [한국어] bdev 모듈에 wait_entry 등록. 풀 슬롯이 반환되면 cb_fn이 호출됨. */
	if (rc != 0) {
		/* [한국어] 등록 자체 실패(rare, 예: 채널 비유효) — 합성 EIO 완료로 응답 경로로 보냄. */
		SPDK_ERRLOG("Queue io failed in nbd_queue_io, rc=%d.\n", rc);
		nbd_io_done(NULL, false, io);
	}
}

/*
 * [한국어]
 * nbd_submit_bdev_io - 한 개 nbd_io를 type에 맞는 spdk_bdev_* 호출로 변환해 발행.
 *
 * @nbd : 디스크.
 * @io  : 발행할 nbd_io (req.type/from/len/payload 모두 채워져 있어야 함).
 * @return: 0(언제나 0 — 실패 경로도 nbd_io_done 합성으로 흡수).
 *
 * NBD 명령 매핑:
 *  - NBD_CMD_READ  → spdk_bdev_read
 *  - NBD_CMD_WRITE → spdk_bdev_write
 *  - NBD_CMD_FLUSH → spdk_bdev_flush (전체 디바이스, FLAG_SEND_FLUSH가 컴파일 시 정의된 경우)
 *  - NBD_CMD_TRIM  → spdk_bdev_unmap (FLAG_SEND_TRIM 정의된 경우)
 *  - 그 외          → 실패로 처리 (NBD에서 비표준 또는 지원 안 함)
 *
 * 풀 고갈(-ENOMEM) 시 nbd_queue_io로 풀 가용까지 대기. 그 외 실패는 합성 EIO 완료.
 *
 * 실행 컨텍스트: 본 디스크 reactor thread.
 *
 * 호출 체인:
 *   nbd_io_exec → [nbd_submit_bdev_io] → spdk_bdev_read/write/flush/unmap → bdev module
 *   nbd_resubmit_io → [nbd_submit_bdev_io]
 */
static int
nbd_submit_bdev_io(struct spdk_nbd_disk *nbd, struct nbd_io *io)
{
	struct spdk_bdev_desc *desc = nbd->bdev_desc; /* [한국어] bdev open descriptor. */
	struct spdk_io_channel *ch = nbd->ch; /* [한국어] 본 thread의 IO 채널. */
	int rc = 0; /* [한국어] bdev 발행 결과. */

	switch (from_be32(&io->req.type)) {
	/* [한국어] req.type은 big-endian — from_be32로 호스트 endian 정수로 디코드. */
	case NBD_CMD_READ:
		/* [한국어] READ: bdev로부터 payload만큼 읽어 io->payload에 채움.
		 * from(64비트 오프셋, big-endian) → from_be64. 완료 시 nbd_io_done 콜. */
		rc = spdk_bdev_read(desc, ch, io->payload, from_be64(&io->req.from),
				    io->payload_size, nbd_io_done, io);
		break;
	case NBD_CMD_WRITE:
		/* [한국어] WRITE: 사용자가 socket으로 보낸 payload를 bdev에 기록. */
		rc = spdk_bdev_write(desc, ch, io->payload, from_be64(&io->req.from),
				     io->payload_size, nbd_io_done, io);
		break;
#ifdef NBD_FLAG_SEND_FLUSH
	case NBD_CMD_FLUSH:
		/* [한국어] FLUSH: 디바이스 전 영역(0 ~ blocks*block_size)을 flush. NBD는 부분 flush 미지원. */
		rc = spdk_bdev_flush(desc, ch, 0,
				     spdk_bdev_get_num_blocks(nbd->bdev) * spdk_bdev_get_block_size(nbd->bdev),
				     nbd_io_done, io);
		break;
#endif
#ifdef NBD_FLAG_SEND_TRIM
	case NBD_CMD_TRIM:
		/* [한국어] TRIM: discard/unmap 명령. len은 32비트 big-endian. */
		rc = spdk_bdev_unmap(desc, ch, from_be64(&io->req.from),
				     from_be32(&io->req.len), nbd_io_done, io);
		break;
#endif
	default:
		/* [한국어] 알 수 없는/미지원 NBD 명령 — 실패로 표시해 아래 합성 완료 경로로. */
		rc = -1;
	}

	if (rc < 0) {
		if (rc == -ENOMEM) {
			/* [한국어] bdev_io 풀 고갈 — wait_entry 등록 후 추후 재시도. */
			SPDK_INFOLOG(nbd, "No memory, start to queue io.\n");
			nbd_queue_io(io);
		} else {
			/* [한국어] 그 외 영구적 실패 — 즉시 EIO 응답 경로로. */
			SPDK_ERRLOG("nbd io failed in nbd_queue_io, rc=%d.\n", rc);
			nbd_io_done(NULL, false, io);
		}
	}

	return 0; /* [한국어] 항상 0 — 실패도 비동기 콜백으로 처리되므로 caller에 동기 에러 전달 안 함. */
}

/*
 * [한국어]
 * nbd_io_exec - received_io_list에 있는 모든 IO를 bdev에 발행 (received → processing 큐 이동).
 *
 * @nbd   : 디스크.
 * @return: 발행한 IO 개수, 음수면 발행 중 실패.
 *
 * 단일 thread 흐름이라 발행 중 콜백이 끼어들 일은 없다. processing 큐로 옮긴 뒤 bdev 호출 —
 * 호출 안에서 즉시 동기 실패 시 nbd_io_done이 합성 호출되어 다시 executed_io_list로 이동한다.
 */
static int
nbd_io_exec(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io, *io_tmp; /* [한국어] FOREACH_SAFE — 순회 중 큐 이동 안전성 보장. */
	int io_count = 0; /* [한국어] 발행한 총 IO 수. */
	int ret = 0; /* [한국어] 발행 결과 저장. */

	TAILQ_FOREACH_SAFE(io, &nbd->received_io_list, tailq, io_tmp) {
		/* [한국어] 수신 완료된 모든 IO를 순서대로 처리. */
		TAILQ_REMOVE(&nbd->received_io_list, io, tailq); /* [한국어] received 큐에서 분리. */
		TAILQ_INSERT_TAIL(&nbd->processing_io_list, io, tailq); /* [한국어] in-flight 큐에 추가. */
		ret = nbd_submit_bdev_io(nbd, io); /* [한국어] bdev 호출 — 거의 항상 0 반환. */
		if (ret < 0) {
			/* [한국어] 비정상적 실패 — 즉시 caller에 보고 (poller가 connection close 처리). */
			return ret;
		}

		io_count++; /* [한국어] 발행 성공 카운트. */
	}

	return io_count;
}

/*
 * [한국어]
 * nbd_io_recv_internal - socket에서 NBD 요청 헤더 + (write일 경우) payload를 한 번 시도해 수신.
 *
 * @nbd   : 디스크.
 * @return: 진행한 바이트 수, 0 = EAGAIN, 음수 = -errno(연결 종료/에러).
 *
 * 핵심 동작:
 *  1) io_in_recv가 NULL이면 새 nbd_io 할당 (NBD는 in-order이므로 동시에 한 개만 진행).
 *  2) 상태가 RECV_REQ면 sizeof(struct nbd_request)=28바이트를 partial-aware 수신.
 *     - offset 바이트만큼 진행. 완전 수신되면 magic 검사 → type 분기.
 *     - NBD_CMD_DISC: 정상 단절 신호 — is_closing=true 표식, 본 IO 폐기.
 *     - WRITE/READ : payload_size 결정, WRITE면 RECV_PAYLOAD로 전이, 그 외(READ/FLUSH/TRIM)는
 *                    payload 없이 RECV 단계 종료 → received_io_list에 enqueue.
 *  3) 상태가 RECV_PAYLOAD면 payload_size 만큼 추가 수신, 완전 수신 시 received_io_list에 enqueue.
 *
 * 종료 중(closing)이거나 아직 시작 미완(started=false)일 때 enqueue 위치가 다르다 — 이런 IO는
 * bdev에 보내지 않고 즉시 합성 EIO 처리해 응답 경로로 흘려보낸다(disconnect 직전 IO 등).
 *
 * 실행 컨텍스트: 본 디스크 reactor thread.
 *
 * 호출 체인:
 *   nbd_io_recv → [nbd_io_recv_internal] → nbd_socket_rw / nbd_get_io / nbd_io_done
 *   nbd_cleanup_io → [nbd_io_recv_internal] (drain 용도)
 */
static int
nbd_io_recv_internal(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io; /* [한국어] 현재 수신 중인 IO. */
	int ret = 0; /* [한국어] socket_rw 임시 결과. */
	int received = 0; /* [한국어] 누적 수신 바이트 수. */

	if (nbd->io_in_recv == NULL) {
		/* [한국어] 새 요청 시작 — 풀에서 nbd_io 할당. NBD는 한 번에 하나씩만 수신. */
		nbd->io_in_recv = nbd_get_io(nbd);
		if (!nbd->io_in_recv) {
			return -ENOMEM; /* [한국어] OOM — caller가 connection close 처리. */
		}
	}

	io = nbd->io_in_recv; /* [한국어] 진행 중 IO 참조. */

	if (io->state == NBD_IO_RECV_REQ) {
		/* [한국어] 1단계: nbd_request 헤더 수신. partial 가능 — io->offset 누적. */
		ret = nbd_socket_rw(nbd->spdk_sp_fd, (char *)&io->req + io->offset,
				    sizeof(io->req) - io->offset, true);
		/* [한국어] req 구조체의 offset 위치부터 남은 바이트만큼 read 시도. */
		if (ret < 0) {
			/* [한국어] 영구 에러 — IO 폐기 후 에러 전파. */
			nbd_put_io(nbd, io);
			nbd->io_in_recv = NULL;
			return ret;
		}

		io->offset += ret; /* [한국어] partial recv 진행을 누적. */
		received = ret;

		/* request is fully received */
		if (io->offset == sizeof(io->req)) {
			/* [한국어] 헤더 28바이트 완전 수신. payload 단계 진입 준비. */
			io->offset = 0; /* [한국어] 다음 단계용으로 offset 리셋. */

			/* req magic check */
			if (from_be32(&io->req.magic) != NBD_REQUEST_MAGIC) {
				/* [한국어] NBD_REQUEST_MAGIC = 0x25609513. 매직 불일치는 프로토콜 동기 손실 — 폐기. */
				SPDK_ERRLOG("invalid request magic\n");
				nbd_put_io(nbd, io);
				nbd->io_in_recv = NULL;
				return -EINVAL;
			}

			if (from_be32(&io->req.type) == NBD_CMD_DISC) {
				/* [한국어] 커널 nbd가 disconnect 통보 (NBD_DISCONNECT ioctl 대응 IO).
				 * 응답 없이 즉시 IO 폐기, is_closing 설정해 추가 IO 차단. */
				nbd->is_closing = true;
				nbd->io_in_recv = NULL;
				if (nbd->interrupt_mode && TAILQ_EMPTY(&nbd->executed_io_list)) {
					/* [한국어] interrupt 모드에서 빠르게 종료 진행을 위해 OUT 이벤트 켜
					 * nbd_poll(→ nbd_io_xmit)을 적어도 한 번 더 호출되게 한다. */
					spdk_interrupt_set_event_types(nbd->intr, SPDK_INTERRUPT_EVENT_IN | SPDK_INTERRUPT_EVENT_OUT);
				}
				nbd_put_io(nbd, io); /* [한국어] DISC IO 자체는 응답 안 보내고 폐기. */
				/* After receiving NBD_CMD_DISC, nbd will not receive any new commands */
				return received;
			}

			/* io except read/write should ignore payload */
			if (from_be32(&io->req.type) == NBD_CMD_WRITE ||
			    from_be32(&io->req.type) == NBD_CMD_READ) {
				/* [한국어] READ/WRITE만 payload를 가짐. len(big-endian)을 호스트 32비트 정수로. */
				io->payload_size = from_be32(&io->req.len);
			} else {
				/* [한국어] FLUSH/TRIM 등은 payload 없음. */
				io->payload_size = 0;
			}

			/* io payload allocate */
			if (io->payload_size) {
				/* [한국어] DMA-able 메모리로 payload 할당.
				 *  - buf_align: bdev 모듈 요구 정렬 (보통 512B 또는 4KB).
				 *  - SPDK_ENV_LCORE_ID_ANY: NUMA 임의 — DMA 가능한 hugepage에서 할당.
				 *  - SPDK_MALLOC_DMA: hugepage 기반 DMA 영역 강제. */
				io->payload = spdk_malloc(io->payload_size, nbd->buf_align, NULL,
							  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
				if (io->payload == NULL) {
					/* [한국어] 큰 IO에서 hugepage 부족 가능 — 즉시 ENOMEM 반환해 종료. */
					SPDK_ERRLOG("could not allocate io->payload of size %d\n", io->payload_size);
					nbd_put_io(nbd, io);
					nbd->io_in_recv = NULL;
					return -ENOMEM;
				}
			} else {
				io->payload = NULL; /* [한국어] payload 없는 명령 — NULL로 명시. */
			}

			/* next io step */
			if (from_be32(&io->req.type) == NBD_CMD_WRITE) {
				/* [한국어] WRITE는 client→server payload 수신 단계가 추가됨. */
				io->state = NBD_IO_RECV_PAYLOAD;
			} else {
				/* [한국어] READ/FLUSH/TRIM은 헤더만 받으면 즉시 발행 가능. */
				io->state = NBD_IO_XMIT_RESP;
				if (spdk_likely((!nbd->is_closing) && nbd->is_started)) {
					/* [한국어] 정상 흐름 — bdev 발행 대기 큐로. */
					TAILQ_INSERT_TAIL(&nbd->received_io_list, io, tailq);
				} else {
					/* [한국어] 비정상(closing/未시작) — 즉시 EIO 응답 경로로 우회.
					 * processing 큐에 잠시 넣었다가 nbd_io_done이 executed로 이동시킴. */
					TAILQ_INSERT_TAIL(&nbd->processing_io_list, io, tailq);
					nbd_io_done(NULL, false, io);
				}
				nbd->io_in_recv = NULL; /* [한국어] 다음 IO 수신 슬롯 비움. */
			}
		}
	}

	if (io->state == NBD_IO_RECV_PAYLOAD) {
		/* [한국어] 2단계: WRITE payload 수신 — io->payload + offset 위치에 partial 누적. */
		ret = nbd_socket_rw(nbd->spdk_sp_fd, io->payload + io->offset, io->payload_size - io->offset, true);
		if (ret < 0) {
			/* [한국어] 송신 중 에러 — IO 폐기. */
			nbd_put_io(nbd, io);
			nbd->io_in_recv = NULL;
			return ret;
		}

		io->offset += ret; /* [한국어] payload partial 진행. */
		received += ret; /* [한국어] 누적 수신 바이트 갱신. */

		/* request payload is fully received */
		if (io->offset == io->payload_size) {
			/* [한국어] payload 전체 수신 완료 — 응답 단계 준비. */
			io->offset = 0;
			io->state = NBD_IO_XMIT_RESP;
			if (spdk_likely((!nbd->is_closing) && nbd->is_started)) {
				/* [한국어] 정상 — bdev 발행 큐로. */
				TAILQ_INSERT_TAIL(&nbd->received_io_list, io, tailq);
			} else {
				/* [한국어] closing/未시작 상태 — bdev에 보내지 않고 EIO 응답. */
				TAILQ_INSERT_TAIL(&nbd->processing_io_list, io, tailq);
				nbd_io_done(NULL, false, io);
			}
			nbd->io_in_recv = NULL; /* [한국어] 다음 IO 수신 슬롯 해제. */
		}

	}

	return received; /* [한국어] 누적 진행 바이트 수 반환. */
}

/*
 * [한국어]
 * nbd_io_recv - 한 번의 polling 사이클에서 최대 GET_IO_LOOP_COUNT개까지 IO를 수신.
 *
 * @nbd   : 디스크.
 * @return: 누적 수신 바이트 수, 음수 = 에러.
 *
 * 다른 디스크/poller의 starvation을 막기 위해 GET_IO_LOOP_COUNT(16)으로 한도. 진행 도중
 * NBD_CMD_DISC가 수신되어 is_closing이 true가 되면 즉시 break.
 *
 * 호출 체인:
 *   nbd_poll → _nbd_poll → [nbd_io_recv] → nbd_io_recv_internal
 */
static int
nbd_io_recv(struct spdk_nbd_disk *nbd)
{
	int i, rc, ret = 0; /* [한국어] 루프 변수, 임시 결과, 누적 진행. */

	/*
	 * nbd server should not accept request after closing command
	 */
	if (nbd->is_closing) {
		/* [한국어] 종료 진행 중이면 더 이상 신규 IO를 받지 않는다. cleanup 경로는 별도. */
		return 0;
	}

	for (i = 0; i < GET_IO_LOOP_COUNT; i++) {
		/* [한국어] 한 사이클에 처리할 최대 IO 수 제한 — 다른 poller에 CPU 양보. */
		rc = nbd_io_recv_internal(nbd);
		if (rc < 0) {
			/* [한국어] 영구 실패 — caller에게 즉시 보고. */
			return rc;
		}
		ret += rc; /* [한국어] 누적 진행. */
		if (nbd->is_closing) {
			/* [한국어] 도중에 NBD_CMD_DISC 수신 등으로 종료 진입 — 루프 종료. */
			break;
		}
	}

	return ret;
}

/*
 * [한국어]
 * nbd_io_xmit_internal - executed_io_list의 첫 IO에 대해 응답 헤더(+READ payload) 송신을 1회 시도.
 *
 * @nbd   : 디스크.
 * @return: > 0 송신 바이트 수, 0 = EAGAIN, 음수 = 에러.
 *
 * 단계:
 *  1) IO를 executed_io_list 앞에서 제거(완료 가정).
 *  2) state=XMIT_RESP면 16바이트 nbd_reply 송신.
 *     - 완전 송신되고 READ + error=0이면 XMIT_PAYLOAD로 전이, 아니면 IO put_io로 종료.
 *  3) state=XMIT_PAYLOAD면 io->payload를 socket으로 송신.
 *  4) partial이거나 EAGAIN/에러면 head로 다시 끼워 넣어 다음 사이클 재시도.
 *
 * "선 제거 후 완료 시 누락" 패턴이 아니라 "선 제거 후 미완 시 head 재삽입"인 이유:
 * scan-build의 use-after-free 오탐 회피 (코멘트 원본 그대로).
 */
static int
nbd_io_xmit_internal(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io; /* [한국어] 송신 대상. */
	int ret = 0; /* [한국어] socket_rw 임시. */
	int sent = 0; /* [한국어] 누적 송신 바이트 수. */

	io = TAILQ_FIRST(&nbd->executed_io_list); /* [한국어] FIFO — 가장 먼저 완료된 IO부터 응답. */
	if (io == NULL) {
		return 0; /* [한국어] 송신할 게 없음. */
	}

	/* Remove IO from list now assuming it will be completed.  It will be inserted
	 *  back to the head if it cannot be completed.  This approach is specifically
	 *  taken to work around a scan-build use-after-free mischaracterization.
	 */
	TAILQ_REMOVE(&nbd->executed_io_list, io, tailq);
	/* [한국어] 큐에서 분리 — 완료 시 곧 nbd_put_io로 free, 미완료 시 reinsert 라벨에서 head로 복귀. */

	/* resp error and handler are already set in io_done */

	if (io->state == NBD_IO_XMIT_RESP) {
		/* [한국어] 1단계: nbd_reply (16바이트) 송신. magic/error/handle은 nbd_io_done에서 채워졌다. */
		ret = nbd_socket_rw(nbd->spdk_sp_fd, (char *)&io->resp + io->offset,
				    sizeof(io->resp) - io->offset, false);
		if (ret <= 0) {
			/* [한국어] 0(EAGAIN) 또는 에러 — 다음 사이클로 미루기 위해 reinsert. */
			goto reinsert;
		}

		io->offset += ret; /* [한국어] partial 진행 누적. */
		sent = ret;

		/* response is fully transmitted */
		if (io->offset == sizeof(io->resp)) {
			io->offset = 0; /* [한국어] payload 단계용 offset 리셋. */

			/* transmit payload only when NBD_CMD_READ with no resp error */
			if (from_be32(&io->req.type) != NBD_CMD_READ || io->resp.error != 0) {
				/* [한국어] READ가 아니거나 에러면 응답 본문 없음 — IO 종료.
				 *  (WRITE 응답은 헤더만, READ는 헤더+payload, 그 외도 헤더만.)
				 *  io->resp.error는 big-endian이지만 0 검사이므로 변환 불필요. */
				nbd_put_io(nbd, io);
				return 0;
			} else {
				/* [한국어] READ 성공 — payload(데이터) 송신 단계. */
				io->state = NBD_IO_XMIT_PAYLOAD;
			}
		}
	}

	if (io->state == NBD_IO_XMIT_PAYLOAD) {
		/* [한국어] 2단계: READ 데이터 payload 송신. partial-aware. */
		ret = nbd_socket_rw(nbd->spdk_sp_fd, io->payload + io->offset, io->payload_size - io->offset,
				    false);
		if (ret <= 0) {
			/* [한국어] EAGAIN 또는 에러 — head로 복귀 후 종료. */
			goto reinsert;
		}

		io->offset += ret; /* [한국어] partial 진행 누적. */
		sent += ret;

		/* read payload is fully transmitted */
		if (io->offset == io->payload_size) {
			/* [한국어] 데이터 송신 완료 — IO 자원 해제 후 정상 종료. */
			nbd_put_io(nbd, io);
			return sent;
		}
	}

reinsert:
	/* [한국어] 미완료(partial/EAGAIN/error) IO는 head로 복귀해 순서 보존. */
	TAILQ_INSERT_HEAD(&nbd->executed_io_list, io, tailq);
	return ret < 0 ? ret : sent; /* [한국어] 에러는 음수, 그 외는 진행 바이트 수. */
}

/*
 * [한국어]
 * nbd_io_xmit - 송신 큐가 빌 때까지 nbd_io_xmit_internal을 반복 호출.
 *
 * @nbd   : 디스크.
 * @return: 누적 송신 바이트 수, 음수 = 에러.
 *
 * 큐가 모두 비면 interrupt 모드일 경우 EPOLLOUT 이벤트를 다시 끈다(불필요한 wake-up 절감).
 *
 * 호출 체인:
 *   _nbd_poll → [nbd_io_xmit] → nbd_io_xmit_internal → nbd_socket_rw
 */
static int
nbd_io_xmit(struct spdk_nbd_disk *nbd)
{
	int ret = 0; /* [한국어] 누적 진행 바이트. */
	int rc; /* [한국어] internal 반환값 임시. */

	while (!TAILQ_EMPTY(&nbd->executed_io_list)) {
		/* [한국어] 송신 큐가 비거나 EAGAIN(internal이 0 반환 후 reinsert)이 될 때까지. */
		rc = nbd_io_xmit_internal(nbd);
		if (rc < 0) {
			/* [한국어] 영구 에러 — 즉시 보고. */
			return rc;
		}

		ret += rc;
		/* [한국어] rc=0(EAGAIN with reinsert)인 경우 internal이 큐에 같은 항목을 head로 복귀시켰으므로
		 *  TAILQ_EMPTY는 false지만 내용물 변화 없음 → 무한루프 위험.
		 *  실제로는 nbd_io_xmit_internal이 partial이면 진행 바이트>0을 반환하고, EAGAIN이면 0 반환 후
		 *  caller가 멈춰야 한다. 본 루프에서 0 반환을 break하지 않는 것은 잠재적 이슈로 보이나,
		 *  실제로는 socket이 풀린 상태에서만 0이 반환되므로 곧 다음 internal 호출이 진행한다는 가정. */
	}

	/* When there begins to have no executed_io, disable socket writable notice */
	if (nbd->interrupt_mode) {
		/* [한국어] 큐 비었으니 EPOLLOUT 끔 — 다음 nbd_io_done에서 다시 켤 때까지 wake-up 회피. */
		spdk_interrupt_set_event_types(nbd->intr, SPDK_INTERRUPT_EVENT_IN);
	}

	return ret;
}

/**
 * Poll an NBD instance.
 *
 * \return 0 on success or negated errno values on error (e.g. connection closed).
 */
/*
 * [한국어]
 * _nbd_poll - 한 NBD 인스턴스의 한 polling 사이클: xmit → recv → exec 순.
 *
 * @nbd   : 디스크.
 * @return: 진행 바이트 수 합계 (>=0), 음수 = 에러.
 *
 * 순서 의의:
 *  - xmit이 먼저 — 응답 큐를 비워 socket 버퍼를 해제 (커널 측이 read 가능하도록).
 *  - recv: 새 IO 헤더/payload를 받아 received 큐로.
 *  - exec: received 큐의 IO를 bdev에 발행 → processing 큐로 이동.
 *  완료는 비동기 콜백 nbd_io_done이 다시 executed 큐로 가져온다.
 */
static int
_nbd_poll(struct spdk_nbd_disk *nbd)
{
	int received, sent, executed; /* [한국어] 각 단계 진행량. */

	/* transmit executed io first */
	sent = nbd_io_xmit(nbd); /* [한국어] 응답 송신 단계 — socket 버퍼 비워 backpressure 완화. */
	if (sent < 0) {
		return sent; /* [한국어] socket write 영구 에러 — 연결 종료 처리. */
	}

	received = nbd_io_recv(nbd); /* [한국어] 새 요청 수신 단계 — 최대 16개. */
	if (received < 0) {
		return received; /* [한국어] socket read 영구 에러 — 연결 종료 처리. */
	}

	executed = nbd_io_exec(nbd); /* [한국어] 받은 IO를 bdev로 발행 — 비동기 완료. */
	if (executed < 0) {
		return executed;
	}

	return sent + received + executed; /* [한국어] 진행이 있었는지(>0) caller에 보고. */
}

/*
 * [한국어]
 * nbd_poll - SPDK poller 콜백 (poller 또는 interrupt 모드 공통 진입점).
 *
 * @arg   : spdk_nbd_disk*.
 * @return: SPDK_POLLER_IDLE = 진행 없음, SPDK_POLLER_BUSY = 진행 있음 (CPU stat에 반영).
 *
 * _nbd_poll이 음수 반환 시 = 연결 종료 — 즉시 _nbd_stop 호출해 자원 해제.
 * is_closing && io_count==0면 정식 종료 진입 (spdk_nbd_stop 호출).
 *
 * 실행 컨텍스트: 본 디스크 reactor thread (polling 또는 epoll wake).
 *
 * 호출 체인:
 *   reactor poll loop → [nbd_poll] → _nbd_poll (xmit/recv/exec)
 *   reactor poll loop → [nbd_poll] (closing 확인) → spdk_nbd_stop → _nbd_stop
 */
static int
nbd_poll(void *arg)
{
	struct spdk_nbd_disk *nbd = arg; /* [한국어] poller arg 복원. */
	int rc; /* [한국어] _nbd_poll 결과. */

	rc = _nbd_poll(nbd); /* [한국어] 한 사이클 진행. */
	if (rc < 0) {
		/* [한국어] socket 에러/EOF — 연결 종료로 보고 즉시 자원 해제. */
		SPDK_INFOLOG(nbd, "nbd_poll() returned %s (%d); closing connection\n",
			     spdk_strerror(-rc), rc);
		_nbd_stop(nbd);
		return SPDK_POLLER_IDLE;
	}
	if (nbd->is_closing && nbd->io_count == 0) {
		/* [한국어] 종료 신호 수신 + 잔여 IO 없음 — 정식 종료 시퀀스 시작. */
		spdk_nbd_stop(nbd);
	}

	return rc == 0 ? SPDK_POLLER_IDLE : SPDK_POLLER_BUSY;
	/* [한국어] 진행 없으면 IDLE — reactor가 다른 poller 우선. */
}

struct spdk_nbd_start_ctx {
	/* [한국어] spdk_nbd_start의 비동기 진행 ctx — start 완료까지 살아 있다.
	 * NBD_SET_SOCK 재시도, kernel pthread, complete 콜백을 가로지르는 컨텍스트. */

	struct spdk_nbd_disk	*nbd;
	/* [한국어] 생성 중인 디스크 — start 전 과정에서 ctx와 짝.
	 * 설정자: spdk_nbd_start. 읽는 자: nbd_enable_kernel/nbd_start_continue/nbd_start_kernel/nbd_start_complete.
	 * 라이프타임: ctx free 직전까지. 종료 후 nbd는 nbd_poller가 소유.
	 * 동기화: 단일 thread 흐름 (nbd_start_kernel만 별도 pthread, 그 안에서는 nbd 필드를 거의 변경 안 함). */

	spdk_nbd_start_cb	cb_fn;
	/* [한국어] 시작 완료/실패 시 호출할 사용자 콜백 (signature: (cb_arg, nbd, rc)).
	 * NULL 가능 — 그 경우 결과 통보 없음. */

	void			*cb_arg;
	/* [한국어] cb_fn에 전달할 사용자 ctx (예: rpc_nbd_start_disk*). */

	struct spdk_thread	*thread;
	/* [한국어] start를 호출한 SPDK thread. nbd_start_kernel(별도 pthread)에서 이 thread로
	 * spdk_thread_send_msg를 보내 nbd_start_complete를 깔끔히 본 thread에서 실행. */
};

/*
 * [한국어]
 * nbd_start_complete - start 완료를 본 SPDK thread에서 마무리하는 message handler.
 *
 * @arg: spdk_nbd_start_ctx*.
 *
 * NBD_DO_IT을 호출한 직후 본 메시지가 본 thread에 전달되어, 사용자 콜백을 호출하고 ctx를 free.
 * is_started=true는 여기서 세팅 — 이전엔 stop이 와도 1을 반환해 재시도 처리.
 *
 * 실행 컨텍스트: spdk_thread_send_msg에 의해 본 디스크 thread로 라우팅됨.
 */
static void
nbd_start_complete(void *arg)
{
	struct spdk_nbd_start_ctx *ctx = arg; /* [한국어] message arg 복원. */

	if (ctx->cb_fn) {
		/* [한국어] 사용자 콜백 호출 (nbd 핸들 + rc=0). RPC가 응답을 만들어 보내는 경로. */
		ctx->cb_fn(ctx->cb_arg, ctx->nbd, 0);
	}

	/* nbd will possibly receive stop command while initing */
	ctx->nbd->is_started = true;
	/* [한국어] 이제 정식 IO 흐름이 가능. spdk_nbd_stop이 1 대신 0으로 동작 가능. */

	free(ctx); /* [한국어] ctx 자원 해제 — 이후 ctx 포인터 dangling. */
}

/*
 * [한국어]
 * nbd_start_kernel - NBD_DO_IT ioctl을 들고 있는 OS pthread의 진입 함수.
 *
 * @arg   : spdk_nbd_start_ctx*.
 * @return: NULL (pthread_exit).
 *
 * NBD_DO_IT은 커널 nbd 모듈의 메인 루프에 진입하는 ioctl로, socket pair가 닫힐 때까지
 * 호출자(이 pthread)가 블록된다. SPDK reactor 안에서 호출하면 polling 정지하므로 별도
 * pthread로 분리 + spdk_unaffinitize_thread()로 reactor CPU 점유 회피.
 *
 * 동작:
 *  1) thread affinity 해제.
 *  2) 본 thread로 nbd_start_complete 메시지 송신 — DO_IT 진입 전 마지막 안전한 시점.
 *  3) NBD_DO_IT ioctl 진입 (블록).
 *  4) ioctl 반환되면 has_nbd_pthread=false 설정 후 pthread 종료.
 *
 * 실행 컨텍스트: detached OS pthread (SPDK reactor 외).
 */
static void *
nbd_start_kernel(void *arg)
{
	struct spdk_nbd_start_ctx *ctx = arg; /* [한국어] pthread arg 복원. */
	struct spdk_nbd_disk *nbd = ctx->nbd; /* [한국어] 사용 편의를 위한 별칭. */

	spdk_unaffinitize_thread();
	/* [한국어] DPDK가 박아둔 CPU affinity 해제 — 이 pthread가 reactor 코어를 점유하지 않게 한다. */

	/* Send a message to complete the start context - this is the
	 * latest point we can do it, since the NBD_DO_IT ioctl will
	 * block in the kernel.
	 */
	spdk_thread_send_msg(ctx->thread, nbd_start_complete, ctx);
	/* [한국어] complete 처리는 SPDK thread에서 — start 호출자에게 응답 송신을 그 thread에서 깔끔히 수행. */

	/* This will block in the kernel until we close the spdk_sp_fd. */
	ioctl(nbd->dev_fd, NBD_DO_IT);
	/* [한국어] 커널 nbd 클라이언트 메인 루프 진입 — socketpair peer가 close될 때까지 블록.
	 *  반환값/에러 무시 — 종료 트리거는 항상 외부(close)이므로 errno 의미가 작다. */

	nbd->has_nbd_pthread = false;
	/* [한국어] _nbd_stop의 retry_poller가 pthread 종료를 인지할 수 있는 플래그 해제.
	 *  (단일 32-bit bool 쓰기 — 단일 작성자 + 폴링 reader이므로 메모리 배리어 불필요한 SPDK 관행.) */

	pthread_exit(NULL); /* [한국어] detached pthread 자원 자동 회수. */
}

/*
 * [한국어]
 * nbd_bdev_hot_remove - 백킹 bdev이 hot-remove된 경우 NBD 디스크 측 정리 진입.
 *
 * @nbd: 영향 받는 디스크.
 *
 * bdev이 갑자기 사라지면 더 이상 IO 발행이 불가하므로 즉시 closing 상태로 만들고 cleanup.
 * received 큐에 남아 있던 IO도 즉시 EIO로 합성 응답해 NBD client(커널)에 보고.
 */
static void
nbd_bdev_hot_remove(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io, *io_tmp; /* [한국어] FOREACH_SAFE 변수. */

	nbd->is_closing = true; /* [한국어] 신규 IO 수신 차단. */
	nbd_cleanup_io(nbd); /* [한국어] socket 잔여 패킷 흡수 + 일부 큐 정리 시도. */

	TAILQ_FOREACH_SAFE(io, &nbd->received_io_list, tailq, io_tmp) {
		/* [한국어] bdev에 발행되기 직전인 IO들 — 더 이상 갈 곳이 없으니 즉시 EIO 합성 응답. */
		TAILQ_REMOVE(&nbd->received_io_list, io, tailq);
		nbd_io_done(NULL, false, io); /* [한국어] processing→executed 순서를 건너뛰고 직접 executed로. */
	}
}

/*
 * [한국어]
 * nbd_bdev_event_cb - bdev 이벤트 통보 콜백 (open_ext에 등록).
 *
 * @type     : bdev 이벤트 종류.
 * @bdev     : 이벤트 발생 bdev.
 * @event_ctx: open_ext에 등록한 ctx — 본 NBD 디스크.
 *
 * 현재는 SPDK_BDEV_EVENT_REMOVE만 처리. 그 외는 NOTICELOG로 무시.
 */
static void
nbd_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		  void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		/* [한국어] bdev 사라짐 — NBD 디스크 hot-remove 흐름. */
		nbd_bdev_hot_remove(event_ctx);
		break;
	default:
		/* [한국어] resize 등 미지원 이벤트 — 알림 로그만. */
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/*
 * [한국어]
 * nbd_poller_set_interrupt_mode - poller가 interrupt 모드로 전환될 때 호출되는 hook.
 *
 * @poller       : 본 디스크의 메인 poller.
 * @cb_arg       : spdk_nbd_disk*.
 * @interrupt_mode: true면 epoll 기반 wake-up, false면 일반 polling.
 *
 * 이 콜백은 디스크의 interrupt_mode 플래그를 동기화 — nbd_io_done/recv_internal/xmit이
 * EPOLLOUT 토글 여부를 결정할 때 이 플래그를 본다.
 */
static void
nbd_poller_set_interrupt_mode(struct spdk_poller *poller, void *cb_arg, bool interrupt_mode)
{
	struct spdk_nbd_disk *nbd = cb_arg; /* [한국어] hook arg 복원. */

	nbd->interrupt_mode = interrupt_mode; /* [한국어] 모드 플래그 동기화. */
}

/*
 * [한국어]
 * nbd_start_continue - NBD_SET_SOCK 성공 후 나머지 ioctl + pthread + poller 등록을 진행.
 *
 * @ctx: start 진행 ctx (nbd, cb_fn, cb_arg, thread).
 *
 * 단계:
 *  1) NBD_SET_BLKSIZE: 커널이 사용할 논리 블록 크기 (bdev block_size 그대로 — 보통 512/4096).
 *  2) NBD_SET_SIZE_BLOCKS: 디바이스 총 블록 수 → mkfs/df 등이 보는 디바이스 크기 결정.
 *  3) NBD_SET_TIMEOUT: 단일 IO 타임아웃 (60초). 미정의 시 호환 NOTICELOG만.
 *  4) NBD_SET_FLAGS: SEND_FLUSH/SEND_TRIM 활성 (bdev이 지원하는 경우).
 *  5) NBD_DO_IT을 들고 있을 별도 pthread 생성 + detach.
 *  6) interrupt 모드 / polling 모드 둘 다 nbd_poll을 등록 — interrupt면 socket fd에 epoll 부착.
 *
 * 실패 시 _nbd_stop으로 자원 회수 + 사용자 콜백에 음수 rc 보고.
 *
 * 호출 체인:
 *   nbd_enable_kernel(NBD_SET_SOCK 성공) → [nbd_start_continue]
 *     → ioctl(NBD_SET_BLKSIZE/SIZE_BLOCKS/TIMEOUT/FLAGS)
 *     → pthread_create(nbd_start_kernel) (커널 NBD_DO_IT 진입)
 *     → SPDK_POLLER_REGISTER(nbd_poll)
 */
static void
nbd_start_continue(struct spdk_nbd_start_ctx *ctx)
{
	int		rc; /* [한국어] ioctl/pthread 결과 임시. */
	pthread_t	tid; /* [한국어] 생성된 pthread id (detach 후 미사용). */
	unsigned long	nbd_flags = 0; /* [한국어] NBD_SET_FLAGS에 OR-결합할 플래그 비트. */

	rc = ioctl(ctx->nbd->dev_fd, NBD_SET_BLKSIZE, spdk_bdev_get_block_size(ctx->nbd->bdev));
	/* [한국어] 커널 nbd에 논리 블록 크기 알림. bdev이 4K이면 4096을 전달 — 잘못된 값이면 mkfs가 실패. */
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_BLKSIZE) failed: %s\n", spdk_strerror(errno));
		rc = -errno; /* [한국어] errno → 음수 errno로 변환. */
		goto err;
	}

	rc = ioctl(ctx->nbd->dev_fd, NBD_SET_SIZE_BLOCKS, spdk_bdev_get_num_blocks(ctx->nbd->bdev));
	/* [한국어] 총 블록 수 알림. block_size × num_blocks = 디바이스 용량. */
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_SIZE_BLOCKS) failed: %s\n", spdk_strerror(errno));
		rc = -errno;
		goto err;
	}

#ifdef NBD_SET_TIMEOUT
	rc = ioctl(ctx->nbd->dev_fd, NBD_SET_TIMEOUT, NBD_IO_TIMEOUT_S);
	/* [한국어] 단일 IO 타임아웃 60초 — 이 시간 내 응답 없으면 커널이 IO를 EIO로 fail. */
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_TIMEOUT) failed: %s\n", spdk_strerror(errno));
		rc = -errno;
		goto err;
	}
#else
	/* [한국어] 구버전 커널은 NBD_SET_TIMEOUT 미지원 — 알림 로그만. */
	SPDK_NOTICELOG("ioctl(NBD_SET_TIMEOUT) is not supported.\n");
#endif

#ifdef NBD_FLAG_SEND_FLUSH
	if (spdk_bdev_io_type_supported(ctx->nbd->bdev, SPDK_BDEV_IO_TYPE_FLUSH)) {
		/* [한국어] bdev이 flush 지원하면 NBD_FLAG_SEND_FLUSH 비트 추가 — 커널이 NBD_CMD_FLUSH 발행. */
		nbd_flags |= NBD_FLAG_SEND_FLUSH;
	}
#endif
#ifdef NBD_FLAG_SEND_TRIM
	if (spdk_bdev_io_type_supported(ctx->nbd->bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		/* [한국어] bdev이 unmap/trim 지원하면 NBD_FLAG_SEND_TRIM 비트 추가. */
		nbd_flags |= NBD_FLAG_SEND_TRIM;
	}
#endif

	if (nbd_flags) {
		rc = ioctl(ctx->nbd->dev_fd, NBD_SET_FLAGS, nbd_flags);
		/* [한국어] 결합된 플래그를 한 번에 설정 — 비어 있으면 호출 생략. */
		if (rc == -1) {
			SPDK_ERRLOG("ioctl(NBD_SET_FLAGS, 0x%lx) failed: %s\n", nbd_flags, spdk_strerror(errno));
			rc = -errno;
			goto err;
		}
	}

	ctx->nbd->has_nbd_pthread = true;
	/* [한국어] pthread 생성 직전에 표식 — _nbd_stop에서 retry 대기 진입 판단에 사용.
	 *  생성 실패 시 다시 false로 클리어. */
	rc = pthread_create(&tid, NULL, nbd_start_kernel, ctx);
	/* [한국어] OS pthread 생성 — NBD_DO_IT 블록 호출을 격리. ctx를 그대로 인자로 넘김. */
	if (rc != 0) {
		ctx->nbd->has_nbd_pthread = false; /* [한국어] 실패 시 표식 원복. */
		SPDK_ERRLOG("could not create thread: %s\n", spdk_strerror(rc));
		rc = -rc; /* [한국어] pthread는 양수 errno 반환 — 음수로 변환. */
		goto err;
	}

	rc = pthread_detach(tid);
	/* [한국어] detached 모드 — 종료 시 자동 자원 회수, join 불필요. */
	if (rc != 0) {
		SPDK_ERRLOG("could not detach thread for nbd kernel: %s\n", spdk_strerror(rc));
		rc = -rc;
		goto err;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		/* [한국어] SPDK app이 interrupt 모드면 socket fd에 epoll 콜백 등록 — wake-on-IO로 동작. */
		ctx->nbd->intr = SPDK_INTERRUPT_REGISTER(ctx->nbd->spdk_sp_fd, nbd_poll, ctx->nbd);
	}

	ctx->nbd->nbd_poller = SPDK_POLLER_REGISTER(nbd_poll, ctx->nbd, 0);
	/* [한국어] 메인 polling 콜백 등록 — period 0 = 매 reactor tick마다 호출(busy poll). */
	spdk_poller_register_interrupt(ctx->nbd->nbd_poller, nbd_poller_set_interrupt_mode, ctx->nbd);
	/* [한국어] 모드 전환 hook 등록 — interrupt_mode 플래그 동기화 책임. */
	return;

err:
	/* [한국어] 모든 실패 경로 — nbd 자원 회수 후 사용자 콜백에 에러 보고. */
	_nbd_stop(ctx->nbd);
	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, NULL, rc); /* [한국어] nbd=NULL, rc<0 — RPC 측 에러 응답으로 매핑. */
	}
	free(ctx);
}

/*
 * [한국어]
 * nbd_enable_kernel - NBD_SET_SOCK ioctl로 socketpair의 커널 측 fd를 커널 nbd 모듈에 인계.
 *
 * @arg   : spdk_nbd_start_ctx*.
 * @return: SPDK_POLLER_BUSY (poller로 재등록되거나 일회성 호출).
 *
 * NBD_SET_SOCK은 다른 사용자가 같은 /dev/nbdN을 잡고 있을 때 EBUSY를 반환할 수 있다.
 * 이는 race(다른 RPC가 동시에 점유 시도) 또는 직전 사용자의 정리 지연 때문이다. 이 경우
 * NBD_START_BUSY_WAITING_MS(1초) 동안 NBD_BUSY_POLLING_INTERVAL_US(20ms) 주기로 재시도.
 *
 * 첫 호출은 spdk_nbd_start가 직접 부른다(retry_poller=NULL인 분기). 이후 재시도는 poller
 * 콜백으로 자기 자신을 호출. 성공 시 nbd_start_continue로 넘어간다.
 *
 * 실행 컨텍스트: 본 디스크 reactor thread.
 *
 * 호출 체인:
 *   spdk_nbd_start → [nbd_enable_kernel] → ioctl(NBD_SET_SOCK)
 *     성공: → nbd_start_continue
 *     EBUSY: poller 등록 후 재시도 → 한도 초과 시 _nbd_stop + 사용자 콜백 에러
 */
static int
nbd_enable_kernel(void *arg)
{
	struct spdk_nbd_start_ctx *ctx = arg; /* [한국어] poller arg 복원. */
	int rc; /* [한국어] ioctl 결과. */

	/* Declare device setup by this process */
	rc = ioctl(ctx->nbd->dev_fd, NBD_SET_SOCK, ctx->nbd->kernel_sp_fd);
	/* [한국어] /dev/nbdN fd에 socketpair의 커널 측 fd를 인계 — 이후 커널은 이 fd로 NBD 패킷 송수신.
	 *  주의: 인계 후에도 우리가 보유한 kernel_sp_fd 자체는 close해도 됨 (커널이 dup해 보관). */

	if (rc) {
		if (errno == EBUSY) {
			/* [한국어] 다른 사용자가 이미 이 슬롯을 점유 중 — 짧은 시간 재시도. */
			if (ctx->nbd->retry_poller == NULL) {
				/* [한국어] 첫 EBUSY: retry 카운터 + poller 신규 등록. */
				ctx->nbd->retry_count = NBD_START_BUSY_WAITING_MS * 1000ULL / NBD_BUSY_POLLING_INTERVAL_US;
				/* [한국어] 1000ms × 1000us/ms ÷ 20000us = 50회. */
				ctx->nbd->retry_poller = SPDK_POLLER_REGISTER(nbd_enable_kernel, ctx,
							 NBD_BUSY_POLLING_INTERVAL_US);
				return SPDK_POLLER_BUSY;
			} else if (ctx->nbd->retry_count-- > 0) {
				/* Repeatedly unregister and register retry poller to avoid scan-build error */
				/* [한국어] 매 시도마다 poller를 unregister/register하는 이유는 scan-build의
				 *  거짓 양성(false-positive)을 회피하기 위함이라고 원본 코멘트가 설명한다. */
				spdk_poller_unregister(&ctx->nbd->retry_poller);
				ctx->nbd->retry_poller = SPDK_POLLER_REGISTER(nbd_enable_kernel, ctx,
							 NBD_BUSY_POLLING_INTERVAL_US);
				return SPDK_POLLER_BUSY;
			}
			/* [한국어] retry_count 소진 — 아래 영구 실패 경로로 떨어진다. */
		}

		SPDK_ERRLOG("ioctl(NBD_SET_SOCK) failed: %s\n", spdk_strerror(errno));
		if (ctx->nbd->retry_poller) {
			/* [한국어] 진행 중이던 retry poller 정리. */
			spdk_poller_unregister(&ctx->nbd->retry_poller);
		}

		_nbd_stop(ctx->nbd); /* [한국어] start 실패 — 부분 자원 정리. */

		if (ctx->cb_fn) {
			ctx->cb_fn(ctx->cb_arg, NULL, -errno);
			/* [한국어] 사용자 콜백에 -errno 보고 — RPC 측에서 -EBUSY 등으로 매핑됨. */
		}

		free(ctx); /* [한국어] ctx free — 이후 ctx dangling. */
		return SPDK_POLLER_BUSY;
	}

	if (ctx->nbd->retry_poller) {
		/* [한국어] 재시도 끝에 성공한 경우 — retry poller 해제. */
		spdk_poller_unregister(&ctx->nbd->retry_poller);
	}

	nbd_start_continue(ctx); /* [한국어] 후속 ioctl + pthread + main poller 등록 단계로 진행. */

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * spdk_nbd_start - 공개 API: 지정된 bdev를 /dev/nbdN으로 비동기적으로 노출.
 *
 * @bdev_name: 백킹 SPDK bdev 이름 (예: "Nvme0n1", "Malloc0").
 * @nbd_path : 사용할 NBD 디바이스 경로 ("/dev/nbdN"). 호출 전에 가용성 확인이 끝나 있어야 함.
 * @cb_fn    : 시작 완료/실패 콜백 (signature: (cb_arg, nbd_or_NULL, rc)). NULL 가능.
 * @cb_arg   : cb_fn에 전달할 ctx.
 *
 * 단계:
 *  1) spdk_nbd_disk + start_ctx 동적 할당.
 *  2) 모든 fd를 -1로 초기화 — err 경로에서 안전한 close 분기.
 *  3) bdev open_ext + io_channel 획득 + 정렬 결정.
 *  4) AF_UNIX socketpair(NONBLOCK) — sp[0]=SPDK 측, sp[1]=커널 측.
 *  5) nbd_path strdup + 큐 초기화 + 전역 리스트 등록 (중복 검사).
 *  6) /dev/nbdN을 O_RDWR|O_DIRECT로 open — 이후 NBD ioctl 발행 가능.
 *  7) nbd_enable_kernel 호출 → NBD_SET_SOCK → ... → 메인 poller 등록.
 *
 * 실패 시 ctx free + nbd가 부분적으로 만들어졌다면 _nbd_stop으로 회수, 사용자 콜백에 에러 보고.
 *
 * 실행 컨텍스트: RPC 핸들러가 호출 — 보통 SPDK 마스터 reactor thread.
 *
 * 호출 체인:
 *   rpc_nbd_start_disk → [spdk_nbd_start]
 *     → spdk_bdev_open_ext + spdk_bdev_get_io_channel
 *     → socketpair(2)
 *     → open("/dev/nbdN", O_RDWR|O_DIRECT)
 *     → nbd_enable_kernel → ioctl(NBD_SET_SOCK) → nbd_start_continue → ...
 */
void
spdk_nbd_start(const char *bdev_name, const char *nbd_path,
	       spdk_nbd_start_cb cb_fn, void *cb_arg)
{
	struct spdk_nbd_start_ctx	*ctx = NULL; /* [한국어] start 진행 ctx — 콜백까지 살아 있음. */
	struct spdk_nbd_disk		*nbd = NULL; /* [한국어] 새로 만드는 디스크. */
	struct spdk_bdev		*bdev; /* [한국어] open_ext으로 얻은 bdev 객체. */
	int				rc; /* [한국어] 에러 누적용. */
	int				sp[2]; /* [한국어] socketpair 결과 fd 쌍. */

	nbd = calloc(1, sizeof(*nbd)); /* [한국어] zero-init 동적 할당. */
	if (nbd == NULL) {
		rc = -ENOMEM;
		goto err;
	}

	nbd->dev_fd = -1; /* [한국어] open 전 invalid 마킹 — err 경로에서 close 분기 보호. */
	nbd->spdk_sp_fd = -1;
	nbd->kernel_sp_fd = -1;

	ctx = calloc(1, sizeof(*ctx)); /* [한국어] start 진행 ctx 할당. */
	if (ctx == NULL) {
		rc = -ENOMEM;
		goto err;
	}

	ctx->nbd = nbd;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->thread = spdk_get_thread();
	/* [한국어] 현재 SPDK thread를 기억 — nbd_start_kernel(별도 pthread)이 send_msg로 돌려 보낼 대상. */

	rc = spdk_bdev_open_ext(bdev_name, true, nbd_bdev_event_cb, nbd, &nbd->bdev_desc);
	/* [한국어] 백킹 bdev open. 두 번째 인자 true = write 권한.
	 *  이벤트 콜백 nbd_bdev_event_cb가 hot-remove 등을 본 디스크에 통보. */
	if (rc != 0) {
		SPDK_ERRLOG("could not open bdev %s, error=%d\n", bdev_name, rc);
		goto err;
	}

	bdev = spdk_bdev_desc_get_bdev(nbd->bdev_desc); /* [한국어] descriptor에서 bdev 객체 추출. */
	nbd->bdev = bdev;

	nbd->ch = spdk_bdev_get_io_channel(nbd->bdev_desc);
	/* [한국어] 본 thread 컨텍스트의 IO 채널 획득 — bdev 모듈이 thread별로 별도 채널 발급. */
	nbd->buf_align = spdk_max(spdk_bdev_get_buf_align(bdev), 64);
	/* [한국어] bdev 요구 정렬 vs 64B 중 큰 값 — 최소 캐시라인 정렬 보장. */

	rc = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sp);
	/* [한국어] AF_UNIX 스트림 + NONBLOCK socketpair 생성.
	 *  AF_UNIX: 같은 머신 내, 빠른 IPC. SOCK_STREAM: NBD 프로토콜은 byte-stream.
	 *  NONBLOCK: poller 모델 호환 — read/write가 즉시 EAGAIN 가능. */
	if (rc != 0) {
		SPDK_ERRLOG("socketpair failed\n");
		rc = -errno;
		goto err;
	}

	nbd->spdk_sp_fd = sp[0]; /* [한국어] 우리가 보유할 끝. */
	nbd->kernel_sp_fd = sp[1]; /* [한국어] NBD_SET_SOCK으로 커널에 인계할 끝. */
	nbd->nbd_path = strdup(nbd_path); /* [한국어] 경로 사본 — RPC ctx 라이프타임과 분리. */
	if (!nbd->nbd_path) {
		SPDK_ERRLOG("strdup allocation failure\n");
		rc = -ENOMEM;
		goto err;
	}

	TAILQ_INIT(&nbd->received_io_list); /* [한국어] 헤더+payload 수신 완료 큐 초기화. */
	TAILQ_INIT(&nbd->executed_io_list); /* [한국어] bdev 완료, 송신 대기 큐 초기화. */
	TAILQ_INIT(&nbd->processing_io_list); /* [한국어] bdev에 in-flight 큐 초기화. */

	/* Add nbd_disk to the end of disk list */
	rc = nbd_disk_register(ctx->nbd);
	/* [한국어] 전역 리스트에 등록 (중복 nbd_path는 EBUSY로 거부). */
	if (rc != 0) {
		goto err;
	}

	nbd->dev_fd = open(nbd_path, O_RDWR | O_DIRECT);
	/* [한국어] /dev/nbdN을 RDWR로 open. O_DIRECT는 page cache 우회 — NBD ioctl 발행에 필요한 fd.
	 *  실제 IO는 socketpair로 흐르므로 O_DIRECT의 의미는 약하지만 관습. */
	if (nbd->dev_fd == -1) {
		SPDK_ERRLOG("open(\"%s\") failed: %s\n", nbd_path, spdk_strerror(errno));
		rc = -errno;
		goto err;
	}

	SPDK_INFOLOG(nbd, "Enabling kernel access to bdev %s via %s\n",
		     bdev_name, nbd_path);

	nbd_enable_kernel(ctx);
	/* [한국어] NBD_SET_SOCK 시작 — 이후 진행은 비동기. ctx는 콜백 체인에서 free 책임. */
	return;

err:
	/* [한국어] 에러 경로 — ctx/nbd 부분 자원 회수 후 사용자 콜백에 보고. */
	free(ctx);
	if (nbd) {
		_nbd_stop(nbd); /* [한국어] 부분 초기화된 nbd도 안전하게 정리. */
	}

	if (cb_fn) {
		cb_fn(cb_arg, NULL, rc);
	}
}

/*
 * [한국어]
 * spdk_nbd_get_path - 공개 API: 디스크의 /dev/nbdN 경로 반환.
 *
 * @nbd   : 대상 디스크.
 * @return: "/dev/nbdN" 문자열 (디스크 살아있는 동안 유효).
 *
 * RPC nbd_start_disk가 자동 할당된 경로를 클라이언트에 응답하는 데 사용.
 */
const char *
spdk_nbd_get_path(struct spdk_nbd_disk *nbd)
{
	return nbd->nbd_path; /* [한국어] strdup된 경로 직접 반환 (caller가 free 금지). */
}

SPDK_LOG_REGISTER_COMPONENT(nbd)
/* [한국어] SPDK 로깅 컴포넌트 "nbd" 등록 — SPDK_INFOLOG(nbd, ...) 등이 활성/비활성 토글 가능. */
