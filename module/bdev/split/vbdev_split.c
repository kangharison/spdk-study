/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * This is a simple example of a virtual block device that takes a single
 * bdev and slices it into multiple smaller bdevs.
 */

/*
 * [한국어 설명] split 가상 bdev 모듈 본체 (vbdev_split.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK split vbdev 모듈을 구현한다. split은 단일 base bdev를 N개의 동일 크기
 * sub-bdev로 잘라(파티션) 노출하는 가장 단순한 형태의 가상 bdev이다. 본체 로직은 거의 모두
 * lib/bdev/part.c가 제공하는 spdk_bdev_part / spdk_bdev_part_base 추상화에 위임되며, 본 파일은
 * "설정 등록 → part_base 구성 → N개의 part 생성"이라는 얕은 어댑터 역할만 수행한다.
 * 또한 SPDK 모듈 시스템(SPDK_BDEV_MODULE_REGISTER)에 자신을 등록하여 examine 단계에 신규
 * 등록된 base bdev를 검사하고, g_split_config에 등록된 설정과 일치하면 자동으로 sub-bdev들을
 * 만든다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [bdev_split_create RPC] → vbdev_split_rpc.c → create_vbdev_split (이 파일)
 *                                                  → vbdev_split_add_config (전역 설정 추가)
 *                                                  → vbdev_split_create
 *                                                       → spdk_bdev_part_base_construct_ext
 *                                                       → spdk_bdev_part_construct (×N)
 *   [examine 시] base bdev 등록 시 lib/bdev → vbdev_split_examine → vbdev_split_create.
 *   [I/O 경로] 클라이언트 → spdk_bdev_submit → vbdev_split_submit_request
 *              → spdk_bdev_io_get_buf (READ만) → _vbdev_split_submit_request
 *              → spdk_bdev_part_submit_request → base bdev 모듈.
 * 실행 컨텍스트: bdev 등록/해제는 메인 reactor에서, I/O는 각 채널을 가진 reactor에서 실행.
 *
 * === 타 모듈과의 연결 ===
 * - include: vbdev_split.h(외부 RPC가 사용하는 선언), spdk/rpc.h, spdk/endian.h, spdk/string.h,
 *   spdk/thread.h(thread/poller), spdk/util.h, spdk/bdev_module.h(SPDK_BDEV_MODULE_REGISTER,
 *   spdk_bdev_part_*), spdk/log.h.
 * - 의존: lib/bdev/part.c(part_base/part API), lib/bdev(register/unregister/queue_io_wait).
 * - 의존하는 모듈: vbdev_split_rpc.c(외부 진입점), 모든 base bdev 모듈(NVMe, malloc, AIO 등).
 * - 데이터 흐름: bdev_io는 base bdev로부터 들어와 split의 채널을 거치며 offset을 보정한 뒤
 *   다시 base bdev로 흘러간다(part API가 자동 처리).
 *
 * === 주요 함수/구조체 요약 ===
 * - `struct spdk_vbdev_split_config`     : 한 base bdev에 대한 split 설정(전역 TAILQ).
 * - `struct vbdev_split_channel`         : per-thread I/O 채널(part_ch 포함).
 * - `struct vbdev_split_bdev_io`         : per-IO 컨텍스트(ENOMEM 시 큐잉용 wait 엔트리 포함).
 * - `vbdev_split_create()`               : 설정에 따라 N개 sub-bdev 등록.
 * - `vbdev_split_destruct_config()`      : sub-bdev들을 제거하고 설정도 free.
 * - `vbdev_split_examine()`              : SPDK 모듈 examine 콜백 — base 등록 시 자동 split.
 * - `vbdev_split_submit_request()`       : I/O dispatch 진입점(READ는 buf 확보 후, 그 외는 직접).
 * - `_vbdev_split_submit_request()`      : 실제 part API 호출. ENOMEM 시 wait 큐에 등록.
 * - `create_vbdev_split / vbdev_split_destruct` : RPC가 부르는 외부 진입점.
 */

/* [한국어] split 모듈의 공개 헤더 — RPC 핸들러가 사용하는 함수 선언을 포함. */
#include "vbdev_split.h"

/* [한국어] SPDK_RPC_REGISTER 등 RPC 매크로(이 파일에서는 직접 사용 안하지만 헤더 의존성). */
#include "spdk/rpc.h"
/* [한국어] 엔디안 변환 매크로 모음(현재 코드에서는 미사용이지만 유지). */
#include "spdk/endian.h"
/* [한국어] spdk_sprintf_alloc — sub-bdev 이름 생성에 사용. */
#include "spdk/string.h"
/* [한국어] spdk_thread/poller API. spdk_io_channel_get_ctx 등. */
#include "spdk/thread.h"
/* [한국어] SPDK_COUNTOF, offsetof 등 기본 매크로. */
#include "spdk/util.h"

/* [한국어] bdev 모듈 작성에 필요한 핵심 API: SPDK_BDEV_MODULE_REGISTER, spdk_bdev_part_*,
 *           spdk_bdev_register/unregister, struct spdk_bdev_fn_table 등. */
#include "spdk/bdev_module.h"
/* [한국어] SPDK_ERRLOG/DEBUGLOG 매크로. */
#include "spdk/log.h"

/* [한국어] 한 base bdev에 대한 split 구성 — 전역 g_split_config TAILQ에 저장된다.
 *           동일 base 이름에 대해 중복 등록은 -EEXIST로 거부된다. */
struct spdk_vbdev_split_config {
	char *base_bdev;
	/* [한국어] split 대상 base bdev 이름(strdup 복제본).
	 * 설정자: vbdev_split_add_config()가 strdup으로 채움.
	 * 읽는 자: vbdev_split_examine, vbdev_split_config_find_by_base_name.
	 * 값 범위: 비어있지 않은 문자열. NULL이면 add_config가 -ENOMEM 처리.
	 * 동기화: g_split_config 전체가 단일 thread(메인 reactor)에서만 조작되어 락 불필요. */

	unsigned split_count;
	/* [한국어] 만들 sub-bdev 개수(클램프되어 max_split_count 초과 시 자동 축소).
	 * 설정자: add_config. 읽는 자: vbdev_split_create의 루프.
	 * 값 범위: 1 이상. 동기화: 동일. */

	uint64_t split_size_mb;
	/* [한국어] 각 sub-bdev 크기(MiB). 0이면 자동 균등 분할.
	 * 값 범위: 0 또는 base 블록 크기의 배수. 0 처리는 vbdev_split_create에서. */

	SPDK_BDEV_PART_TAILQ splits;
	/* [한국어] 이 설정으로 만들어진 sub-bdev(spdk_bdev_part)들의 TAILQ 헤드.
	 * 설정자: vbdev_split_create의 TAILQ_INIT + spdk_bdev_part_construct.
	 * 읽는 자: hot-remove 시 spdk_bdev_part_base_hotremove의 인자로 전달.
	 * 동기화: 단일 thread. */

	struct spdk_bdev_part_base *split_base;
	/* [한국어] lib/bdev/part.c가 제공하는 base wrapper. base bdev open과 모든 part의 공통 정보 보관.
	 * 설정자: vbdev_split_create가 spdk_bdev_part_base_construct_ext로 생성.
	 * 읽는 자: hot-remove, destruct, get_part_base.
	 * 값 범위: 등록 후 NULL 아님; 등록 전/실패 시 NULL.
	 * 동기화: 단일 thread. */

	TAILQ_ENTRY(spdk_vbdev_split_config) tailq;
	/* [한국어] g_split_config TAILQ 연결 노드.
	 * 설정자: add_config의 TAILQ_INSERT_TAIL.
	 * 읽는 자: vbdev_split_clear_config, find_by_base_name, config_json 등 모든 순회. */
};

/* [한국어] 모든 split 설정을 담는 전역 TAILQ. 단일 thread에서만 접근하여 락 불필요. */
static TAILQ_HEAD(, spdk_vbdev_split_config) g_split_config = TAILQ_HEAD_INITIALIZER(
			g_split_config);

/* [한국어] split의 per-thread I/O 채널. 각 reactor가 자신만의 인스턴스를 가진다.
 *           spdk_bdev_part_channel이 base bdev에 대한 채널을 캡슐화. */
struct vbdev_split_channel {
	struct spdk_bdev_part_channel	part_ch;
	/* [한국어] base bdev에 대한 part 공통 채널 — get_io_channel은 이 안의 base_ch를 사용.
	 * 설정자: lib/bdev/part.c가 채널 생성 시 자동 채움(spdk_bdev_part_base의 등록 사이즈 기반).
	 * 읽는 자: _vbdev_split_submit_request가 part_ch를 통해 base에 I/O 발행.
	 * 동기화: 채널은 thread-affinity로 동시 접근 없음. */
};

/* [한국어] per-IO 컨텍스트 — bdev_io->driver_ctx에 저장. ENOMEM 발생 시 재시도 큐잉에 사용. */
struct vbdev_split_bdev_io {
	struct spdk_io_channel *ch;
	/* [한국어] 이 I/O가 속한 채널. 재시도 시 동일 채널에서 재발행해야 함.
	 * 설정자: ENOMEM 시 _vbdev_split_submit_request가 io_ctx->ch에 보관.
	 * 읽는 자: vbdev_split_resubmit_io. 동기화: 채널은 단일 thread. */

	struct spdk_bdev_io *bdev_io;
	/* [한국어] 재시도할 bdev_io 자체.
	 * 설정자/읽는 자: 위와 같음. 동기화: 동일. */

	/* for bdev_io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	/* [한국어] spdk_bdev_queue_io_wait에 등록할 wait 엔트리.
	 * 설정자: vbdev_split_queue_io. 읽는 자: lib/bdev이 ENOMEM 해소 후 cb_fn 호출. */
};

/* [한국어] 전방 선언 — 설정 free(아래에서 정의). */
static void vbdev_split_del_config(struct spdk_vbdev_split_config *cfg);

/* [한국어] SPDK 모듈 인터페이스가 요구하는 콜백들의 전방 선언. 등록 테이블 split_if에서 사용. */
static int vbdev_split_init(void);
static void vbdev_split_fini(void);
static void vbdev_split_examine(struct spdk_bdev *bdev);
static int vbdev_split_config_json(struct spdk_json_write_ctx *w);
static int vbdev_split_get_ctx_size(void);

/* [한국어] 내부 I/O 발행 함수 전방 선언. wait 콜백에서도 호출되므로 분리. */
static void _vbdev_split_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io);

/* [한국어] SPDK_BDEV_MODULE_REGISTER에 넘길 모듈 정의 — name과 콜백 묶음. */
static struct spdk_bdev_module split_if = {
	.name = "split",                          /* [한국어] 모듈 이름. config_json/RPC에서 method 식별. */
	.module_init = vbdev_split_init,          /* [한국어] 모듈 init 시 호출 — 현재는 noop. */
	.module_fini = vbdev_split_fini,          /* [한국어] 모듈 fini 시 호출 — 모든 설정 해제. */
	.get_ctx_size = vbdev_split_get_ctx_size, /* [한국어] per-IO 컨텍스트 크기 알림 — bdev_io에 driver_ctx로 보관 가능. */
	.examine_config = vbdev_split_examine,    /* [한국어] 새 base bdev 등록 시 호출 — split 조건이면 자동 등록. */
	.config_json = vbdev_split_config_json,   /* [한국어] save_config 시 현재 설정을 JSON으로 직렬화. */
};

/* [한국어] 위 split_if를 SPDK 모듈 레지스트리에 등록. SPDK 시작 시 자동으로 module_init이 호출됨. */
SPDK_BDEV_MODULE_REGISTER(split, &split_if)

/*
 * [한국어]
 * vbdev_split_base_free - spdk_bdev_part_base의 destroy 콜백.
 *
 * @ctx: spdk_bdev_part_base에 등록 시 전달한 context(struct spdk_vbdev_split_config*).
 * @return: 없음.
 *
 * part_base가 모든 part의 unregister가 끝나고 free되는 시점에 lib/bdev/part.c에서 호출된다.
 * 본 함수는 split 설정 자체를 g_split_config에서 제거하고 free한다.
 *
 * 호출 체인: lib/bdev/part.c (모든 part unregister 완료) → [이 콜백] → vbdev_split_del_config.
 * 실행 컨텍스트: bdev 메인 thread.
 */
static void
vbdev_split_base_free(void *ctx)
{
	struct spdk_vbdev_split_config *cfg = ctx;  /* [한국어] context를 split config로 캐스팅. */

	vbdev_split_del_config(cfg);                /* [한국어] g_split_config에서 제거하고 free. */
}

/*
 * [한국어]
 * _vbdev_split_destruct - spdk_bdev_fn_table.destruct 콜백.
 *
 * @ctx: bdev_io->bdev->ctxt — 즉 spdk_bdev_part 포인터.
 * @return: spdk_bdev_part_free 결과(0=성공/완전 free, 1=대기 필요).
 *
 * 한 sub-bdev가 unregister될 때 호출되어 part 객체를 정리한다. 마지막 part가 free되면
 * 자동으로 part_base의 base_free 콜백이 트리거된다.
 *
 * 호출 체인: spdk_bdev_unregister → bdev_destroy → [이 콜백] → spdk_bdev_part_free.
 * 실행 컨텍스트: bdev 메인 thread.
 */
static int
_vbdev_split_destruct(void *ctx)
{
	struct spdk_bdev_part *part = ctx;  /* [한국어] context를 part로 캐스팅. */

	return spdk_bdev_part_free(part);   /* [한국어] part 메모리 해제(또는 비동기 대기 코드 반환). */
}

/*
 * [한국어]
 * vbdev_split_base_bdev_hotremove_cb - base bdev이 사라졌을 때 호출되는 콜백.
 *
 * @_part_base: spdk_bdev_part_base*.
 * @return: 없음.
 *
 * 디바이스 hot-remove(예: NVMe SSD 제거)가 발생하면 base bdev가 사라지므로, 그 위의 모든
 * sub-bdev도 unregister해야 한다. 본 콜백은 spdk_bdev_part_base_hotremove로 splits TAILQ의
 * 모든 part를 일괄 정리한다.
 *
 * 호출 체인: 디바이스 핫리무브 → lib/bdev → part_base 콜백 → [이 함수].
 * 실행 컨텍스트: bdev 메인 thread.
 */
static void
vbdev_split_base_bdev_hotremove_cb(void *_part_base)
{
	struct spdk_bdev_part_base *part_base = _part_base;
	struct spdk_vbdev_split_config *cfg = spdk_bdev_part_base_get_ctx(part_base);  /* [한국어] part_base에 등록한 cfg 컨텍스트 복원. */

	spdk_bdev_part_base_hotremove(part_base, &cfg->splits);  /* [한국어] 이 base에 묶인 모든 sub-bdev unregister. */
}

/*
 * [한국어]
 * vbdev_split_resubmit_io - bdev_io_wait 큐에서 깨어났을 때 재발행하는 콜백.
 *
 * @arg: vbdev_split_bdev_io 포인터(이전에 저장된 ch와 bdev_io 보유).
 * @return: 없음.
 *
 * 호출 체인: lib/bdev이 ENOMEM 해소 → wait 엔트리의 cb_fn → [이 함수]
 *                                                          → _vbdev_split_submit_request.
 * 실행 컨텍스트: 채널이 속한 reactor.
 */
static void
vbdev_split_resubmit_io(void *arg)
{
	struct vbdev_split_bdev_io *split_io = (struct vbdev_split_bdev_io *)arg;  /* [한국어] context 복원. */

	_vbdev_split_submit_request(split_io->ch, split_io->bdev_io);  /* [한국어] 동일 채널/IO로 재발행. */
}

/*
 * [한국어]
 * vbdev_split_queue_io - ENOMEM 시 spdk_bdev_queue_io_wait로 재시도 큐에 등록.
 *
 * @split_io: per-IO 컨텍스트(ch와 bdev_io를 이미 보관해둔 상태).
 * @return  : 없음(실패 시 즉시 FAILED 완료).
 *
 * 호출 체인: _vbdev_split_submit_request(rc==-ENOMEM) → [이 함수].
 * 실행 컨텍스트: 채널 thread.
 */
static void
vbdev_split_queue_io(struct vbdev_split_bdev_io *split_io)
{
	struct vbdev_split_channel *ch = spdk_io_channel_get_ctx(split_io->ch);  /* [한국어] 채널의 split 컨텍스트. */
	int rc;

	/* [한국어] wait 엔트리를 채워 lib/bdev에 등록 — base bdev이 자원 회복 시 cb_fn 호출. */
	split_io->bdev_io_wait.bdev = split_io->bdev_io->bdev;
	split_io->bdev_io_wait.cb_fn = vbdev_split_resubmit_io;
	split_io->bdev_io_wait.cb_arg = split_io;

	/* [한국어] base 채널의 wait 큐에 등록. base_ch는 part_ch 내부에서 part API가 만든 base 채널. */
	rc = spdk_bdev_queue_io_wait(split_io->bdev_io->bdev,
				     ch->part_ch.base_ch, &split_io->bdev_io_wait);
	if (rc != 0) {
		/* [한국어] wait 등록 자체가 실패하면 더 이상 손쓸 수 없어 즉시 실패 완료. */
		SPDK_ERRLOG("Queue io failed in vbdev_split_queue_io, rc=%d\n", rc);
		spdk_bdev_io_complete(split_io->bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * [한국어]
 * _vbdev_split_submit_request - 실제 part API로 I/O를 발행하고 ENOMEM 처리.
 *
 * @_ch    : split 채널(spdk_io_channel).
 * @bdev_io: 발행할 bdev I/O.
 * @return : 없음.
 *
 * spdk_bdev_part_submit_request가 알아서 offset 보정 + base bdev로 forwarding 처리한다.
 * 반환값:
 *   0       : 정상 진입(완료는 비동기).
 *   -ENOMEM : base bdev이 일시 자원 부족 — wait 큐에 등록 후 재시도.
 *   기타 음수: 즉시 FAILED 완료.
 *
 * 호출 체인:
 *   상위: vbdev_split_submit_request (READ는 get_buf 후), vbdev_split_resubmit_io.
 *   하위: spdk_bdev_part_submit_request → 내부에서 base bdev의 submit_request.
 * 실행 컨텍스트: 채널이 속한 reactor.
 */
static void
_vbdev_split_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_split_channel *ch = spdk_io_channel_get_ctx(_ch);                            /* [한국어] split 채널 ctx. */
	struct vbdev_split_bdev_io *io_ctx = (struct vbdev_split_bdev_io *)bdev_io->driver_ctx;   /* [한국어] per-IO ctx — get_ctx_size로 확보된 영역. */
	int rc;

	rc = spdk_bdev_part_submit_request(&ch->part_ch, bdev_io);  /* [한국어] part API에 위임 — offset 보정 후 base에 발행. */
	if (rc) {
		if (rc == -ENOMEM) {
			/* [한국어] base bdev에 자원 부족 — io_ctx에 ch/bdev_io를 보관하고 wait 등록. */
			SPDK_DEBUGLOG(vbdev_split, "split: no memory, queue io.\n");
			io_ctx->ch = _ch;
			io_ctx->bdev_io = bdev_io;
			vbdev_split_queue_io(io_ctx);
		} else {
			/* [한국어] 기타 에러 — 즉시 실패 완료. */
			SPDK_ERRLOG("split: error on io submission, rc=%d.\n", rc);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/*
 * [한국어]
 * vbdev_split_get_buf_cb - READ I/O를 위한 버퍼가 확보된 뒤 호출되는 콜백.
 *
 * @ch     : 채널.
 * @bdev_io: 버퍼가 채워진 I/O.
 * @success: 버퍼 확보 성공 여부.
 * @return : 없음.
 *
 * READ는 클라이언트가 버퍼를 제공하지 않으면 SPDK가 풀에서 할당해야 하므로 get_buf를 거친다.
 * 버퍼 확보 후 실제 발행 단계인 _vbdev_split_submit_request로 진입.
 *
 * 호출 체인: spdk_bdev_io_get_buf → buf 풀 → [이 콜백] → _vbdev_split_submit_request.
 * 실행 컨텍스트: 채널 thread.
 */
static void
vbdev_split_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	if (!success) {
		/* [한국어] 버퍼 풀 고갈 등으로 실패 — 즉시 FAILED 완료. */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	_vbdev_split_submit_request(ch, bdev_io);  /* [한국어] 정상 발행 진행. */
}

/*
 * [한국어]
 * vbdev_split_submit_request - bdev_fn_table.submit_request 콜백 — 모든 sub-bdev I/O의 진입점.
 *
 * @_ch    : 채널.
 * @bdev_io: 발행할 I/O.
 * @return : 없음(완료는 비동기).
 *
 * READ 타입만 별도 처리: 버퍼가 없을 수 있으므로 spdk_bdev_io_get_buf로 확보 후 실제 발행.
 * 그 외(WRITE/UNMAP/FLUSH 등)는 곧장 _vbdev_split_submit_request 호출.
 *
 * 호출 체인: 클라이언트 → spdk_bdev_submit → bdev 코어 → [이 함수] →
 *            (READ) get_buf → vbdev_split_get_buf_cb → _vbdev_split_submit_request
 *            (그 외) _vbdev_split_submit_request 직행.
 * 실행 컨텍스트: 채널 thread.
 */
static void
vbdev_split_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		/* [한국어] READ는 buf가 없을 수 있어 풀에서 확보. 길이는 num_blocks * blocklen. */
		spdk_bdev_io_get_buf(bdev_io, vbdev_split_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	default:
		/* [한국어] 비-READ는 직접 발행. WRITE는 클라이언트 버퍼를 그대로 쓴다. */
		_vbdev_split_submit_request(_ch, bdev_io);
		break;
	}
}

/*
 * [한국어]
 * vbdev_split_dump_info_json - bdev_fn_table.dump_info_json — bdev_get_bdevs 응답에 split 정보 추가.
 *
 * @ctx: spdk_bdev_part 포인터.
 * @w  : JSON writer.
 * @return: 0(성공).
 *
 * 응답 예시: { "split": { "base_bdev": "Nvme0n1", "offset_blocks": 0 } }
 *
 * 호출 체인: bdev_get_bdevs RPC → 각 bdev의 dump_info_json → [이 함수]. 컨텍스트: RPC poller.
 */
static int
vbdev_split_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct spdk_bdev_part *part = ctx;                                       /* [한국어] context를 part로. */
	struct spdk_bdev *split_base_bdev = spdk_bdev_part_get_base_bdev(part);  /* [한국어] base bdev 추출. */
	uint64_t offset_blocks = spdk_bdev_part_get_offset_blocks(part);         /* [한국어] base 내 시작 블록 오프셋. */

	spdk_json_write_named_object_begin(w, "split");                          /* [한국어] "split" 객체 시작. */

	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(split_base_bdev));  /* [한국어] base 이름 출력. */
	spdk_json_write_named_uint64(w, "offset_blocks", offset_blocks);                    /* [한국어] 오프셋 출력. */

	spdk_json_write_object_end(w);                                           /* [한국어] 객체 종료. */

	return 0;
}

/*
 * [한국어]
 * vbdev_split_write_config_json - bdev_fn_table.write_config_json (per-bdev) — split은 per-bdev 설정 없음.
 *
 * @bdev: 대상. @w: writer. @return: 없음.
 *
 * split의 설정은 base 단위로만 의미가 있으므로(=g_split_config 항목) 개별 sub-bdev는 별도 출력 없음.
 * 모듈 레벨의 vbdev_split_config_json이 base 단위로 설정을 직렬화한다.
 */
static void
vbdev_split_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No config per bdev needed */
}

/* [한국어] 각 sub-bdev에 대한 fn_table — bdev 코어가 I/O dispatch와 메타데이터 콜백에 사용. */
static struct spdk_bdev_fn_table vbdev_split_fn_table = {
	.destruct		= _vbdev_split_destruct,         /* [한국어] unregister 시 part 해제. */
	.submit_request		= vbdev_split_submit_request,    /* [한국어] I/O 진입점. */
	.dump_info_json		= vbdev_split_dump_info_json,    /* [한국어] bdev_get_bdevs 응답 채움. */
	.write_config_json	= vbdev_split_write_config_json  /* [한국어] per-bdev 설정 출력(없음). */
};

/*
 * [한국어]
 * vbdev_split_create - 한 split 설정에 따라 N개의 sub-bdev를 등록한다.
 *
 * @cfg   : 사전에 vbdev_split_add_config로 등록된 설정.
 * @return: 0=성공, -EINVAL=split_size 정렬 오류, -ENOMEM=할당 실패, -ENODEV=base 없음.
 *
 * 동작:
 *   1) splits TAILQ 초기화 후 part_base 구성(base bdev open 포함).
 *   2) split_size_blocks 계산 (split_size_mb 지정 시 블록 단위 변환, 아니면 base/N).
 *   3) split_count를 max_split_count로 클램프.
 *   4) i=0..split_count-1 루프:
 *      - spdk_bdev_part 메모리 할당.
 *      - "<base>p<i>" 형식의 이름 생성.
 *      - spdk_bdev_part_construct로 등록(offset_blocks씩 증가).
 *   5) 실패 시 part_base hotremove + free로 정리.
 *
 * 호출 체인:
 *   상위: create_vbdev_split (RPC), vbdev_split_examine (자동).
 *   하위: spdk_bdev_part_base_construct_ext, spdk_bdev_part_construct.
 * 실행 컨텍스트: bdev 메인 thread.
 */
static int
vbdev_split_create(struct spdk_vbdev_split_config *cfg)
{
	uint64_t split_size_blocks, offset_blocks;
	uint64_t split_count, max_split_count;
	uint64_t mb = 1024 * 1024;             /* [한국어] 1 MiB(MB가 아니라 MiB로 동작). */
	uint64_t i;
	int rc;
	char *name;
	struct spdk_bdev *base_bdev;
	struct bdev_part_tailq *split_base_tailq;

	assert(cfg->split_count > 0);          /* [한국어] add_config에서 0 거부했으므로 invariant. */

	TAILQ_INIT(&cfg->splits);              /* [한국어] sub-bdev TAILQ 초기화. */
	/* [한국어] part_base 구성 — 내부에서 base bdev을 open하고 hotremove cb 등록.
	 *           ctx로 cfg를 보관해두어 hotremove cb 등에서 다시 cfg를 복원할 수 있다. */
	rc = spdk_bdev_part_base_construct_ext(cfg->base_bdev,
					       vbdev_split_base_bdev_hotremove_cb,
					       &split_if, &vbdev_split_fn_table,
					       &cfg->splits, vbdev_split_base_free, cfg,
					       sizeof(struct vbdev_split_channel),
					       NULL, NULL, &cfg->split_base);
	if (rc != 0) {
		if (rc != -ENODEV) {
			/* [한국어] -ENODEV는 base가 아직 등록 안됨 — 예상 가능한 케이스(나중 examine에서 재시도). */
			SPDK_ERRLOG("Cannot construct bdev part base\n");
		}
		return rc;
	}

	base_bdev = spdk_bdev_part_base_get_bdev(cfg->split_base);  /* [한국어] open된 base bdev 포인터. */

	if (cfg->split_size_mb) {
		/* [한국어] 사용자가 명시적으로 split_size_mb를 지정한 경우 — base block size로 정렬 검사. */
		if (((cfg->split_size_mb * mb) % base_bdev->blocklen) != 0) {
			SPDK_ERRLOG("Split size %" PRIu64 " MB is not possible with block size "
				    "%" PRIu32 "\n",
				    cfg->split_size_mb, base_bdev->blocklen);
			rc = -EINVAL;
			goto err;
		}
		/* [한국어] MiB → 블록 수 변환. */
		split_size_blocks = (cfg->split_size_mb * mb) / base_bdev->blocklen;
		SPDK_DEBUGLOG(vbdev_split, "Split size %" PRIu64 " MB specified by user\n",
			      cfg->split_size_mb);
	} else {
		/* [한국어] 자동 균등 분할 — 끝부분 잔여 블록은 버려진다(블록 단위 정수 나눗셈). */
		split_size_blocks = base_bdev->blockcnt / cfg->split_count;
		SPDK_DEBUGLOG(vbdev_split, "Split size not specified by user\n");
	}

	max_split_count = base_bdev->blockcnt / split_size_blocks;  /* [한국어] base 용량으로 가능한 최대 split 개수. */
	split_count = cfg->split_count;
	if (split_count > max_split_count) {
		/* [한국어] 사용자 요청이 base 용량을 초과 → 가능한 만큼만 만든다(경고 후 클램프). */
		SPDK_WARNLOG("Split count %" PRIu64 " is greater than maximum possible split count "
			     "%" PRIu64 " - clamping\n", split_count, max_split_count);
		split_count = max_split_count;
	}

	SPDK_DEBUGLOG(vbdev_split, "base_bdev: %s split_count: %" PRIu64
		      " split_size_blocks: %" PRIu64 "\n",
		      cfg->base_bdev, split_count, split_size_blocks);

	offset_blocks = 0;                                    /* [한국어] 첫 sub-bdev의 시작 오프셋(블록). */
	for (i = 0; i < split_count; i++) {
		struct spdk_bdev_part *d;

		d = calloc(1, sizeof(*d));                    /* [한국어] part 객체 0-init 할당. */
		if (d == NULL) {
			SPDK_ERRLOG("could not allocate bdev part\n");
			rc = -ENOMEM;
			goto err;
		}

		/* [한국어] sub-bdev 이름 생성 — "<base>p<i>" (예: Nvme0n1p0, Nvme0n1p1, ...). */
		name = spdk_sprintf_alloc("%sp%" PRIu64, cfg->base_bdev, i);
		if (!name) {
			SPDK_ERRLOG("could not allocate name\n");
			free(d);
			rc = -ENOMEM;
			goto err;
		}

		/* [한국어] part 등록 — base 내 [offset, offset+size) 구간을 새 bdev로 노출.
		 *           실패 시 part_construct가 자체적으로 name을 free한다(아래 free name은 정상 케이스). */
		rc = spdk_bdev_part_construct(d, cfg->split_base, name, offset_blocks, split_size_blocks,
					      "Split Disk");
		free(name);                                   /* [한국어] 정상 케이스에서 part_construct가 name을 strdup하므로 우리는 free. */
		if (rc) {
			SPDK_ERRLOG("could not construct bdev part\n");
			/* spdk_bdev_part_construct will free name if it fails */
			free(d);
			rc = -ENOMEM;
			goto err;
		}

		offset_blocks += split_size_blocks;           /* [한국어] 다음 sub-bdev의 시작 오프셋으로 이동. */
	}

	return 0;
err:
	/* [한국어] 어느 단계든 실패 시: 이미 등록된 part들을 hotremove로 정리하고 part_base 해제. */
	split_base_tailq = spdk_bdev_part_base_get_tailq(cfg->split_base);
	spdk_bdev_part_base_hotremove(cfg->split_base, split_base_tailq);
	spdk_bdev_part_base_free(cfg->split_base);
	return rc;
}

/*
 * [한국어]
 * vbdev_split_del_config - g_split_config에서 cfg를 떼어내고 메모리 해제.
 *
 * @cfg: 제거 대상. @return: 없음.
 *
 * 호출 체인: vbdev_split_base_free / vbdev_split_destruct_config(part_base 없을 때).
 * 실행 컨텍스트: bdev 메인 thread.
 */
static void
vbdev_split_del_config(struct spdk_vbdev_split_config *cfg)
{
	TAILQ_REMOVE(&g_split_config, cfg, tailq);  /* [한국어] 전역 리스트에서 분리. */
	free(cfg->base_bdev);                       /* [한국어] strdup된 base 이름 해제. */
	free(cfg);                                  /* [한국어] cfg 자체 해제. */
}

/*
 * [한국어]
 * vbdev_split_destruct_config - 한 cfg에 묶인 모든 sub-bdev를 제거하고 cfg 해제 트리거.
 *
 * @cfg: 제거 대상.
 *
 * 두 경로:
 *   1) part_base 등록 상태 → hotremove로 모든 part unregister 시작 → 비동기 완료 시
 *      base_free 콜백이 호출되어 cfg 해제.
 *   2) part_base 미등록 상태(base bdev 부재로 create가 -ENODEV였던 경우) → 직접 del_config.
 *
 * 호출 체인: vbdev_split_destruct (RPC) / vbdev_split_clear_config (모듈 fini).
 * 실행 컨텍스트: bdev 메인 thread.
 */
static void
vbdev_split_destruct_config(struct spdk_vbdev_split_config *cfg)
{
	struct bdev_part_tailq *split_base_tailq;

	if (cfg->split_base != NULL) {
		/* [한국어] 정상 등록된 경우 — sub-bdev들을 hotremove. cfg 해제는 base_free에서. */
		split_base_tailq = spdk_bdev_part_base_get_tailq(cfg->split_base);
		spdk_bdev_part_base_hotremove(cfg->split_base, split_base_tailq);
	} else {
		/* [한국어] base bdev 없이 설정만 등록된 경우 — 즉시 cfg 해제. */
		vbdev_split_del_config(cfg);
	}
}

/*
 * [한국어]
 * vbdev_split_clear_config - 모듈 fini 시 모든 split 설정을 정리.
 *
 * 호출 체인: vbdev_split_fini → [이 함수] → vbdev_split_destruct_config(각 cfg).
 * 실행 컨텍스트: bdev 메인 thread (모듈 종료 시).
 */
static void
vbdev_split_clear_config(void)
{
	struct spdk_vbdev_split_config *cfg, *tmp_cfg;

	/* [한국어] _SAFE 변형 사용 — 순회 중 TAILQ_REMOVE가 일어나도 안전. */
	TAILQ_FOREACH_SAFE(cfg, &g_split_config, tailq, tmp_cfg) {
		vbdev_split_destruct_config(cfg);
	}
}

/*
 * [한국어]
 * vbdev_split_config_find_by_base_name - base 이름으로 g_split_config 검색.
 *
 * @base_bdev_name: 비교 대상 이름. @return: 일치하는 cfg 또는 NULL.
 *
 * 호출 체인: vbdev_split_examine, create_vbdev_split, vbdev_split_destruct, get_part_base.
 * 실행 컨텍스트: bdev 메인 thread.
 */
static struct spdk_vbdev_split_config *
vbdev_split_config_find_by_base_name(const char *base_bdev_name)
{
	struct spdk_vbdev_split_config *cfg;

	TAILQ_FOREACH(cfg, &g_split_config, tailq) {
		if (strcmp(cfg->base_bdev, base_bdev_name) == 0) {  /* [한국어] strcmp로 정확 일치. */
			return cfg;
		}
	}

	return NULL;  /* [한국어] 못 찾음. */
}

/*
 * [한국어]
 * vbdev_split_add_config - 새 split 설정을 g_split_config에 추가.
 *
 * @base_bdev_name: 대상 base bdev 이름. @split_count: 만들 sub-bdev 수. @split_size: 각 크기(MiB).
 * @config: 결과 cfg를 받을 out 포인터(NULL 허용).
 * @return: 0=성공, -EINVAL=잘못된 입력, -EEXIST=중복, -ENOMEM=할당 실패.
 *
 * base bdev이 아직 없어도 설정은 등록된다(나중 examine에서 자동 활성화).
 *
 * 호출 체인: create_vbdev_split → [이 함수].
 * 실행 컨텍스트: bdev 메인 thread.
 */
static int
vbdev_split_add_config(const char *base_bdev_name, unsigned split_count, uint64_t split_size,
		       struct spdk_vbdev_split_config **config)
{
	struct spdk_vbdev_split_config *cfg;
	assert(base_bdev_name);  /* [한국어] caller 책임 — RPC 디코딩 후 NULL 아님 보장. */

	if (base_bdev_name == NULL) {
		/* [한국어] 방어적 — assert가 풀린 빌드에서도 안전. */
		SPDK_ERRLOG("Split bdev config: no base bdev provided.");
		return -EINVAL;
	}

	if (split_count == 0) {
		SPDK_ERRLOG("Split bdev config: split_count can't be 0.");
		return -EINVAL;
	}

	/* Check if we already have 'base_bdev_name' registered in config */
	/* [한국어] 동일 base에 대한 중복 등록 거부. */
	cfg = vbdev_split_config_find_by_base_name(base_bdev_name);
	if (cfg) {
		SPDK_ERRLOG("Split bdev config for base bdev '%s' already exist.", base_bdev_name);
		return -EEXIST;
	}

	cfg = calloc(1, sizeof(*cfg));  /* [한국어] 0-init으로 모든 포인터/카운트 초기화. */
	if (!cfg) {
		SPDK_ERRLOG("calloc(): Out of memory");
		return -ENOMEM;
	}

	cfg->base_bdev = strdup(base_bdev_name);  /* [한국어] caller 문자열 수명과 분리해 자체 보관. */
	if (!cfg->base_bdev) {
		SPDK_ERRLOG("strdup(): Out of memory");
		free(cfg);
		return -ENOMEM;
	}

	cfg->split_count = split_count;            /* [한국어] 사용자 지정 N. */
	cfg->split_size_mb = split_size;           /* [한국어] 사용자 지정 크기(또는 0). */
	TAILQ_INSERT_TAIL(&g_split_config, cfg, tailq);  /* [한국어] 전역 리스트에 추가. */
	if (config) {
		*config = cfg;                     /* [한국어] caller에게 cfg 포인터 반환(옵션). */
	}

	return 0;
}

/*
 * [한국어]
 * vbdev_split_init - 모듈 초기화 콜백 — 현재는 noop(전역 TAILQ는 정적 초기화).
 *
 * @return: 0(성공). 호출 체인: SPDK 부팅 → bdev 모듈 init → [이 함수]. 실행 컨텍스트: 메인 thread.
 */
static int
vbdev_split_init(void)
{
	return 0;
}

/*
 * [한국어]
 * vbdev_split_fini - 모듈 종료 콜백 — 모든 cfg 정리.
 *
 * 호출 체인: SPDK 종료 → bdev 모듈 fini → [이 함수] → vbdev_split_clear_config.
 * 실행 컨텍스트: 메인 thread.
 */
static void
vbdev_split_fini(void)
{
	vbdev_split_clear_config();
}

/*
 * [한국어]
 * vbdev_split_examine - 새 base bdev 등록 시 자동으로 split을 시도하는 콜백.
 *
 * @bdev: 새로 등록된 bdev. @return: 없음.
 *
 * 동작: 이름이 g_split_config에 등록되어 있으면 vbdev_split_create로 sub-bdev들을 만든다.
 * 그 외에는 spdk_bdev_module_examine_done만 호출하여 examine을 종료.
 *
 * 호출 체인: lib/bdev (새 bdev 등록 시) → 모든 모듈의 examine_config → [이 함수]
 *                                                                       → vbdev_split_create.
 * 실행 컨텍스트: bdev 메인 thread.
 */
static void
vbdev_split_examine(struct spdk_bdev *bdev)
{
	struct spdk_vbdev_split_config *cfg = vbdev_split_config_find_by_base_name(bdev->name);

	if (cfg != NULL) {
		assert(cfg->split_base == NULL);  /* [한국어] 설정 등록 후 처음 등장한 base여야 정상. */

		if (vbdev_split_create(cfg)) {
			SPDK_ERRLOG("could not split bdev %s\n", bdev->name);
		}
	}
	spdk_bdev_module_examine_done(&split_if);  /* [한국어] examine 단계 완료 알림(등록 진행 재개). */
}

/*
 * [한국어]
 * vbdev_split_config_json - 모듈 레벨 config_json — 모든 split 설정을 JSON 배열로 출력.
 *
 * @w: writer. @return: 0(성공).
 *
 * save_config RPC 또는 framework_get_config 시 호출되어 현재 등록된 모든 split 설정을
 * 재현 가능한 RPC 배열로 직렬화한다.
 *
 * 호출 체인: bdev 코어의 save_config → 각 모듈의 config_json → [이 함수].
 * 실행 컨텍스트: RPC poller(메인 reactor).
 */
static int
vbdev_split_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_vbdev_split_config *cfg;

	TAILQ_FOREACH(cfg, &g_split_config, tailq) {
		spdk_json_write_object_begin(w);                           /* [한국어] 객체 시작. */

		spdk_json_write_named_string(w, "method", "bdev_split_create");  /* [한국어] RPC 메서드 이름. */

		spdk_json_write_named_object_begin(w, "params");           /* [한국어] params 객체 시작. */
		spdk_json_write_named_string(w, "base_bdev", cfg->base_bdev);
		spdk_json_write_named_uint32(w, "split_count", cfg->split_count);
		spdk_json_write_named_uint64(w, "split_size_mb", cfg->split_size_mb);
		spdk_json_write_object_end(w);                             /* [한국어] params 종료. */

		spdk_json_write_object_end(w);                             /* [한국어] 객체 종료. */
	}

	return 0;
}

/*
 * [한국어]
 * create_vbdev_split - RPC가 호출하는 외부 진입점. 설정 추가 후 즉시 생성 시도.
 *
 * @base_bdev_name, @split_count, @split_size_mb: RPC 입력.
 * @return: 0=성공(또는 base 미존재), 음수=errno.
 *
 * base bdev이 없을 때(-ENODEV)는 0으로 보정 — 설정만 등록되고 추후 examine에서 자동 활성화.
 *
 * 호출 체인: rpc_bdev_split_create → [이 함수] → vbdev_split_add_config + vbdev_split_create.
 * 실행 컨텍스트: SPDK RPC poller.
 */
int
create_vbdev_split(const char *base_bdev_name, unsigned split_count, uint64_t split_size_mb)
{
	int rc;
	struct spdk_vbdev_split_config *cfg;

	rc = vbdev_split_add_config(base_bdev_name, split_count, split_size_mb, &cfg);
	if (rc) {
		return rc;
	}

	rc = vbdev_split_create(cfg);
	if (rc == -ENODEV) {
		/* It is ok if base bdev does not exist yet. */
		/* [한국어] base가 아직 없는 것은 합법적 시나리오 — 설정 등록은 유지. */
		rc = 0;
	}

	return rc;
}

/*
 * [한국어]
 * vbdev_split_destruct - RPC가 호출하는 외부 진입점. 이름의 split 설정을 제거.
 *
 * @base_bdev_name: 대상.
 * @return: 0=성공, -ENOENT=설정 없음.
 *
 * 호출 체인: rpc_bdev_split_delete → [이 함수] → vbdev_split_destruct_config.
 * 실행 컨텍스트: SPDK RPC poller.
 */
int
vbdev_split_destruct(const char *base_bdev_name)
{
	struct spdk_vbdev_split_config *cfg = vbdev_split_config_find_by_base_name(base_bdev_name);

	if (!cfg) {
		SPDK_ERRLOG("Split configuration for '%s' not found\n", base_bdev_name);
		return -ENOENT;
	}

	vbdev_split_destruct_config(cfg);
	return 0;
}

/*
 * [한국어]
 * vbdev_split_get_part_base - RPC 응답 빌드용 — base bdev에 묶인 part_base를 조회.
 *
 * @bdev: base bdev. @return: 일치하는 part_base 또는 NULL.
 *
 * vbdev_split_rpc.c의 create 응답에서 sub-bdev 이름 배열을 만들 때 사용.
 *
 * 호출 체인: rpc_bdev_split_create → [이 함수].
 * 실행 컨텍스트: SPDK RPC poller.
 */
struct spdk_bdev_part_base *
vbdev_split_get_part_base(struct spdk_bdev *bdev)
{
	struct spdk_vbdev_split_config *cfg;

	cfg = vbdev_split_config_find_by_base_name(spdk_bdev_get_name(bdev));

	if (cfg == NULL) {
		return NULL;
	}

	return cfg->split_base;
}

/*
 * During init we'll be asked how much memory we'd like passed to us
 * in bev_io structures as context. Here's where we specify how
 * much context we want per IO.
 */
/*
 * [한국어]
 * vbdev_split_get_ctx_size - 모듈 레벨 콜백 — bdev_io.driver_ctx 크기 알림.
 *
 * @return: sizeof(struct vbdev_split_bdev_io).
 *
 * SPDK는 모든 bdev_io에 driver_ctx 영역을 미리 할당하는데, 모듈마다 필요한 크기가 다르다.
 * split은 ENOMEM 시 재시도용 컨텍스트가 필요해 위 구조체 크기만큼 요청.
 *
 * 호출 체인: bdev 모듈 init → [이 함수]. 실행 컨텍스트: 메인 thread.
 */
static int
vbdev_split_get_ctx_size(void)
{
	return sizeof(struct vbdev_split_bdev_io);
}

/* [한국어] "vbdev_split" 디버그 로그 컴포넌트 등록. SPDK_DEBUGLOG(vbdev_split, ...)에 사용. */
SPDK_LOG_REGISTER_COMPONENT(vbdev_split)
