/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * This is a module for test purpose which will simulate error cases for bdev.
 */

/*
 * [한국어 설명] error injection 가상 bdev 모듈 본체 (vbdev_error.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK error injection vbdev 모듈을 구현한다. 본 모듈은 base bdev 위에 한 층을
 * 덧대어, RPC로 설정된 규칙(io_type, error_type, count, queue depth, corrupt 옵션)에 따라
 * I/O를 인위적으로 실패/지연/손상시킨다. 주요 용도는 상위 레이어(블롭스토어, 파일시스템,
 * NVMe-oF target, 애플리케이션)의 에러 핸들링 경로를 검증하는 fault injection 테스트.
 * lib/bdev/part.c의 spdk_bdev_part / spdk_bdev_part_base 추상화를 사용해 단일 sub-bdev
 * (이름 패턴 "EE_<base_name>", offset=0, size=base->blockcnt 그대로)를 등록한다.
 * I/O마다 vbdev_error_get_error_type()이 atomic CAS로 error_num 카운터를 감소시켜 매칭
 * 여부를 결정하므로, 여러 채널(reactor)에서 동시에 들어오는 I/O 사이에서도 race 없이
 * 정확히 N개만 발동된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [bdev_error_create RPC] → vbdev_error_rpc.c → vbdev_error_create (이 파일)
 *                                                  → vbdev_error_config_add (전역 설정 추가)
 *                                                  → _vbdev_error_create
 *                                                       → spdk_bdev_part_base_construct_ext
 *                                                       → spdk_bdev_part_construct.
 *   [examine 시] base 등록 → vbdev_error_examine → _vbdev_error_create.
 *   [I/O 경로]
 *     submit: spdk_bdev_submit → bdev_fn_table.submit_request = vbdev_error_submit_request
 *             → vbdev_error_get_error_type (atomic CAS) → 결과에 따라
 *                 - VBDEV_IO_FAILURE → 즉시 FAILED 완료
 *                 - VBDEV_IO_NOMEM   → 즉시 NOMEM 완료(상위 레이어가 재시도)
 *                 - VBDEV_IO_PENDING → ch->pending_ios에 큐잉(완료 안 함 → reset 시 abort)
 *                 - VBDEV_IO_CORRUPT_DATA(WRITE) → 버퍼 변조 후 정상 발행
 *                 - VBDEV_IO_NO_ERROR → 정상 발행 (spdk_bdev_part_submit_request_ext)
 *     complete: base 완료 → vbdev_error_complete_request → READ + CORRUPT_DATA면 변조 →
 *               spdk_bdev_io_complete.
 *     reset: vbdev_error_reset → spdk_for_each_channel → 각 채널의 pending_ios abort.
 * 실행 컨텍스트: 등록/RPC는 메인 reactor, I/O는 채널별 reactor, 설정 갱신은 g_vbdev_error_mutex로
 *   I/O path와 동기화.
 *
 * === 타 모듈과의 연결 ===
 * - include: spdk/stdinc.h, spdk/rpc.h, spdk/util.h, spdk/endian.h, spdk/nvme_spec.h(미사용 가능),
 *   spdk/string.h, spdk/bdev_module.h, spdk/log.h, vbdev_error.h.
 * - 의존: lib/bdev/part.c, lib/bdev, vbdev_error_rpc.c(외부 진입점).
 * - 데이터 흐름: 클라이언트 → 본 모듈 (변조/실패/지연 가능) → base bdev → 응답.
 *
 * === 주요 함수/구조체 요약 ===
 * - `struct spdk_vbdev_error_config` : 한 base에 대한 설정(base_bdev 이름, uuid).
 * - `struct vbdev_error_info`        : per-io_type 규칙(error_type, error_num, qd, corrupt_*).
 * - `struct error_disk`              : 한 vbdev 인스턴스(part + 4종 io_type 규칙 벡터).
 * - `struct error_channel`           : per-thread 채널(in-flight 카운트 + pending I/O 큐).
 * - `vbdev_error_inject_error()`     : RPC가 호출하는 규칙 갱신(mutex 보호).
 * - `vbdev_error_submit_request()`   : I/O 진입점 — 규칙에 따라 분기.
 * - `vbdev_error_get_error_type()`   : 매칭 여부 판정(atomic CAS).
 * - `vbdev_error_corrupt_io_data()`  : iov 첫 위치에 corrupt_value를 XOR.
 */

/* [한국어] POSIX 표준 헤더 묶음 — strcmp/calloc/strdup 등. */
#include "spdk/stdinc.h"
/* [한국어] SPDK_RPC_REGISTER 등 RPC 매크로(이 파일 직접 등록은 없지만 헤더 의존). */
#include "spdk/rpc.h"
/* [한국어] SPDK_COUNTOF — error_vector 길이 계산에 사용. */
#include "spdk/util.h"
/* [한국어] 엔디안 매크로(현재 미사용). */
#include "spdk/endian.h"
/* [한국어] NVMe 스펙 정의(이 파일에서는 직접 사용 없음 — 향후 NVMe completion 시뮬레이션 대비). */
#include "spdk/nvme_spec.h"
/* [한국어] spdk_sprintf_alloc — vbdev 이름 생성에 사용("EE_<base>"). */
#include "spdk/string.h"

/* [한국어] bdev 모듈 작성에 필요한 핵심 API 묶음. */
#include "spdk/bdev_module.h"
/* [한국어] SPDK_ERRLOG 등. */
#include "spdk/log.h"

/* [한국어] error 모듈 공개 헤더 — RPC 핸들러 인터페이스 선언. */
#include "vbdev_error.h"

/* [한국어] 한 base bdev에 대한 error 설정 — 전역 g_error_config TAILQ에 저장. */
struct spdk_vbdev_error_config {
	char *base_bdev;
	/* [한국어] 대상 base bdev 이름(strdup 복제본).
	 * 설정자: vbdev_error_config_add. 읽는 자: examine, find_by_base_name.
	 * 동기화: g_error_config는 메인 thread + g_vbdev_error_mutex가 충돌 없이 보호. */

	struct spdk_uuid uuid;
	/* [한국어] 새 vbdev에 부여할 UUID(0이면 자동 생성).
	 * 설정자: vbdev_error_config_add가 spdk_uuid_copy로 채움.
	 * 읽는 자: examine 시 _vbdev_error_create에 전달. */

	TAILQ_ENTRY(spdk_vbdev_error_config) tailq;
	/* [한국어] g_error_config TAILQ 연결 노드. */
};

/* [한국어] 모든 error 설정의 전역 TAILQ. */
static TAILQ_HEAD(, spdk_vbdev_error_config) g_error_config
	= TAILQ_HEAD_INITIALIZER(g_error_config);

/* [한국어] per-io_type 규칙 슬롯. error_disk.error_vector[io_type]에 보관. */
struct vbdev_error_info {
	uint32_t			error_type;
	/* [한국어] 발동 시 어떤 종류 에러를 만들지(VBDEV_IO_FAILURE/PENDING/CORRUPT_DATA/NOMEM).
	 * 설정자: inject_error(mutex 보호). 읽는 자: get_error_type/submit_request.
	 * 동기화: error_num의 atomic CAS가 동기 시점 역할 — error_type은 그 후 일반 load. */

	uint32_t			error_num;
	/* [한국어] 남은 발동 횟수. 0이면 NO_ERROR.
	 * 설정자: inject_error(초기값) + get_error_type의 atomic CAS(감소).
	 * 읽는 자: get_error_type. 동기화: __atomic_compare_exchange_n으로 race-free. */

	uint64_t			error_qd;
	/* [한국어] 발동 조건 — 채널의 in-flight I/O 수가 이 값 이상일 때만 발동(부하 의존 시나리오).
	 * 0이면 항상 발동 가능. */

	uint64_t			corrupt_offset;
	/* [한국어] CORRUPT_DATA 시 변조할 바이트 오프셋(iov 시작에서). */

	uint8_t				corrupt_value;
	/* [한국어] 변조 시 XOR할 값. 0이면 변조가 일어나도 데이터가 바뀌지 않으므로 inject 단계에서 거부. */
};

/* [한국어] per-IO 컨텍스트 — bdev_io->driver_ctx에 보관. PENDING 큐잉용 link 포함. */
struct error_io {
	enum vbdev_error_type error_type;
	/* [한국어] 본 I/O가 받은 판정(submit 시 결정). complete 콜백에서 CORRUPT_DATA(READ) 처리에 참조. */

	TAILQ_ENTRY(error_io) link;
	/* [한국어] PENDING 시 channel.pending_ios TAILQ 연결 노드. */
};

/* Context for each error bdev */
/* [한국어] 한 error vbdev 인스턴스의 모든 상태를 담는 컨텍스트. bdev->ctxt에 저장된다. */
struct error_disk {
	struct spdk_bdev_part		part;
	/* [한국어] lib/bdev/part.c의 part 베이스 — bdev 본체와 등록 정보를 캡슐화.
	 * 첫 멤버이므로 (struct spdk_bdev_part*)와 (struct error_disk*) 캐스팅이 호환된다. */

	struct vbdev_error_info		error_vector[SPDK_BDEV_IO_TYPE_RESET];
	/* [한국어] io_type별 규칙. 인덱스는 SPDK_BDEV_IO_TYPE_*(READ/WRITE/UNMAP/FLUSH 등).
	 * 동기화: 갱신은 inject_error(mutex 보호), 읽기/감소는 atomic CAS. */
};

/* [한국어] per-thread I/O 채널. */
struct error_channel {
	struct spdk_bdev_part_channel	part_ch;
	/* [한국어] base bdev에 대한 part 공통 채널. */

	uint64_t			io_inflight;
	/* [한국어] 이 채널에 현재 진행 중인 I/O 수. error_qd 비교에 사용.
	 * 채널은 단일 thread에서만 만져지므로 일반 ++/--가 안전. */

	TAILQ_HEAD(, error_io)		pending_ios;
	/* [한국어] PENDING 판정으로 큐잉된 I/O. reset 시 abort 처리. */
};

/* [한국어] 설정/규칙 갱신 시 사용하는 글로벌 mutex.
 *           inject_error는 RPC poller에서 호출되지만, error_disk가 unregister 진행 중일 수 있어
 *           open_ext + part 검색 + 갱신 구간을 하나의 임계 구역으로 묶는다. */
static pthread_mutex_t g_vbdev_error_mutex = PTHREAD_MUTEX_INITIALIZER;

/* [한국어] 모든 error_disk(part)의 전역 TAILQ. inject_error의 검색 대상. */
static SPDK_BDEV_PART_TAILQ g_error_disks = TAILQ_HEAD_INITIALIZER(g_error_disks);

/* [한국어] 모듈 콜백 전방 선언. */
static int vbdev_error_init(void);
static void vbdev_error_fini(void);

static void vbdev_error_examine(struct spdk_bdev *bdev);
static int vbdev_error_config_json(struct spdk_json_write_ctx *w);

static int vbdev_error_config_add(const char *base_bdev_name, const struct spdk_uuid *uuid);
static int vbdev_error_config_remove(const char *base_bdev_name);

/*
 * [한국어]
 * vbdev_error_get_ctx_size - bdev_io.driver_ctx 영역 크기 알림.
 *
 * @return: sizeof(struct error_io). 호출 체인: bdev 모듈 init → [이 함수].
 */
static int
vbdev_error_get_ctx_size(void)
{
	return sizeof(struct error_io);
}

/* [한국어] error 모듈 정의 — split과 유사하지만 examine/config_json만 등록. */
static struct spdk_bdev_module error_if = {
	.name = "error",                          /* [한국어] 모듈 이름. */
	.module_init = vbdev_error_init,          /* [한국어] init 콜백 — noop. */
	.module_fini = vbdev_error_fini,          /* [한국어] fini 콜백 — 설정 정리. */
	.examine_config = vbdev_error_examine,    /* [한국어] base 등록 시 자동 활성화. */
	.config_json = vbdev_error_config_json,   /* [한국어] save_config 직렬화. */
	.get_ctx_size = vbdev_error_get_ctx_size, /* [한국어] driver_ctx 크기. */

};

/* [한국어] 모듈 등록 매크로 — SPDK 시작 시 자동 호출. */
SPDK_BDEV_MODULE_REGISTER(error, &error_if)

/*
 * [한국어]
 * dummy_bdev_event_cb - inject_error에서 일시적으로 base를 open할 때의 noop 이벤트 콜백.
 *
 * spdk_bdev_open_ext가 콜백 NULL을 허용하지 않으므로 빈 함수 등록.
 */
static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

/*
 * [한국어]
 * vbdev_error_inject_error - 가동 중인 error vbdev에 새 규칙을 적용(또는 해제).
 *
 * @name: 대상 vbdev 이름.
 * @opts: 규칙(io_type, error_type, error_num, error_qd, corrupt_*).
 * @return: 0=성공, -EINVAL=잘못된 입력(예: corrupt_value=0), -ENODEV=대상 없음, -E*=open 실패.
 *
 * 동작:
 *   1) corrupt_value 검증(0은 XOR이 무의미하므로 거부).
 *   2) g_vbdev_error_mutex를 잡고 vbdev를 open_ext로 일시적으로 연다 — desc 수명 동안
 *      lib/bdev이 unregister를 막아준다(I/O path와의 race 방지).
 *   3) g_error_disks TAILQ에서 일치하는 part를 검색.
 *   4) opts.io_type 값에 따라:
 *      - 0xffffffff → 전체 io_type에 동일 규칙 일괄 적용.
 *      - 0          → 전체 io_type의 error_num을 0으로 (모든 규칙 해제).
 *      - 그 외       → 해당 io_type 슬롯에만 규칙 갱신.
 *   5) close + unlock.
 *
 * 호출 체인: rpc_bdev_error_inject_error → [이 함수].
 * 실행 컨텍스트: SPDK RPC poller. mutex로 inject 자체와 다른 inject/disk free 동시 호출을 직렬화.
 */
int
vbdev_error_inject_error(char *name, const struct vbdev_error_inject_opts *opts)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	struct spdk_bdev_part *part;
	struct error_disk *error_disk = NULL;
	uint32_t i;
	int rc = 0;

	if (opts->error_type == VBDEV_IO_CORRUPT_DATA) {
		if (opts->corrupt_value == 0) {
			/* If corrupt_value is 0, XOR cannot cause data corruption. */
			/* [한국어] 0과 XOR하면 값이 그대로 → 변조가 일어나지 않음. 잘못된 입력으로 거부. */
			SPDK_ERRLOG("corrupt_value should be non-zero.\n");
			return -EINVAL;
		}
	}

	pthread_mutex_lock(&g_vbdev_error_mutex);  /* [한국어] inject/검색/갱신 임계 구역 진입. */

	/* [한국어] vbdev를 read-only 핸들로 일시 open. 성공 시 lib/bdev이 unregister를 잠시 막음.
	 *           결과 desc는 검색용으로만 쓰고 즉시 close. */
	rc = spdk_bdev_open_ext(name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Could not open ErrorInjection bdev %s\n", name);
		pthread_mutex_unlock(&g_vbdev_error_mutex);
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);  /* [한국어] open된 bdev 포인터. */

	/* [한국어] g_error_disks에서 같은 bdev에 묶인 part 찾기. */
	TAILQ_FOREACH(part, &g_error_disks, tailq) {
		if (bdev == spdk_bdev_part_get_bdev(part)) {
			error_disk = (struct error_disk *)part;  /* [한국어] part가 첫 멤버이므로 캐스팅 안전. */
			break;
		}
	}

	if (error_disk == NULL) {
		/* [한국어] 같은 이름이지만 error 모듈이 만든 vbdev이 아님 — 다른 모듈 소속. */
		SPDK_ERRLOG("Could not find ErrorInjection bdev %s\n", name);
		rc = -ENODEV;
		goto exit;
	}

	if (0xffffffff == opts->io_type) {
		/* [한국어] "all" — 모든 io_type에 동일 규칙 적용. */
		for (i = 0; i < SPDK_COUNTOF(error_disk->error_vector); i++) {
			error_disk->error_vector[i].error_type = opts->error_type;
			error_disk->error_vector[i].error_num = opts->error_num;
			error_disk->error_vector[i].error_qd = opts->error_qd;
			error_disk->error_vector[i].corrupt_offset = opts->corrupt_offset;
			error_disk->error_vector[i].corrupt_value = opts->corrupt_value;
		}
	} else if (0 == opts->io_type) {
		/* [한국어] "clear" — 모든 io_type의 발동 카운터를 0으로 (실질적 규칙 해제). */
		for (i = 0; i < SPDK_COUNTOF(error_disk->error_vector); i++) {
			error_disk->error_vector[i].error_num = 0;
		}
	} else {
		/* [한국어] 특정 io_type만 갱신. */
		error_disk->error_vector[opts->io_type].error_type = opts->error_type;
		error_disk->error_vector[opts->io_type].error_num = opts->error_num;
		error_disk->error_vector[opts->io_type].error_qd = opts->error_qd;
		error_disk->error_vector[opts->io_type].corrupt_offset = opts->corrupt_offset;
		error_disk->error_vector[opts->io_type].corrupt_value = opts->corrupt_value;
	}

exit:
	spdk_bdev_close(desc);                    /* [한국어] 임시 open 해제 — unregister 재허용. */
	pthread_mutex_unlock(&g_vbdev_error_mutex);
	return rc;
}

/*
 * [한국어]
 * vbdev_error_ch_abort_ios - 채널 단위로 pending_ios를 일괄 abort.
 *
 * @i: spdk_for_each_channel iterator(현재 채널 컨텍스트 보유).
 *
 * 호출 체인: vbdev_error_reset → spdk_for_each_channel → 각 채널에서 [이 함수] 실행.
 * 실행 컨텍스트: 각 채널의 reactor — 채널이 자신의 thread에서 I/O 큐를 안전하게 정리.
 */
static void
vbdev_error_ch_abort_ios(struct spdk_io_channel_iter *i)
{
	struct error_channel *ch = spdk_io_channel_get_ctx(spdk_io_channel_iter_get_channel(i));
	struct error_io *error_io, *tmp;

	/* [한국어] pending_ios의 각 항목을 ABORTED로 완료(상위 레이어가 재시도하지 않도록 명시). */
	TAILQ_FOREACH_SAFE(error_io, &ch->pending_ios, link, tmp) {
		TAILQ_REMOVE(&ch->pending_ios, error_io, link);
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(error_io), SPDK_BDEV_IO_STATUS_ABORTED);
	}

	spdk_for_each_channel_continue(i, 0);  /* [한국어] 다음 채널로 진행 신호. */
}

/*
 * [한국어]
 * vbdev_error_ch_abort_ios_done - 모든 채널의 abort가 완료된 뒤 reset I/O를 마무리.
 *
 * @i: iterator. @status: 0=정상, !=0=abort 도중 에러.
 *
 * 호출 체인: spdk_for_each_channel 마지막 → [이 함수] → spdk_bdev_io_complete(reset_io).
 * 실행 컨텍스트: spdk_for_each_channel를 시작한 thread.
 */
static void
vbdev_error_ch_abort_ios_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_bdev_io *reset_io = spdk_io_channel_iter_get_ctx(i);  /* [한국어] reset 자체 bdev_io. */

	if (status != 0) {
		SPDK_ERRLOG("Failed to abort pending I/Os on bdev %s, status = %d\n",
			    reset_io->bdev->name, status);
		spdk_bdev_io_complete(reset_io, SPDK_BDEV_IO_STATUS_FAILED);
	} else {
		spdk_bdev_io_complete(reset_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	}
}

/*
 * [한국어]
 * vbdev_error_reset - SPDK_BDEV_IO_TYPE_RESET 처리 — 모든 채널의 pending I/O abort.
 *
 * @error_disk: 대상 vbdev. @bdev_io: reset I/O.
 *
 * 호출 체인: vbdev_error_submit_request(RESET) → [이 함수] → spdk_for_each_channel.
 * 실행 컨텍스트: reset을 발행한 thread(보통 ctrlr/관리 thread).
 */
static void
vbdev_error_reset(struct error_disk *error_disk, struct spdk_bdev_io *bdev_io)
{
	spdk_for_each_channel(&error_disk->part, vbdev_error_ch_abort_ios, bdev_io,
			      vbdev_error_ch_abort_ios_done);
}

/*
 * [한국어]
 * vbdev_error_get_error_type - 한 I/O에 대해 발동 여부와 종류를 결정. atomic CAS로 race-free.
 *
 * @error_disk: 대상 vbdev. @ch: 채널(io_inflight 사용). @io_type: 본 I/O의 종류.
 * @return: 발동될 에러 종류(VBDEV_IO_*) 또는 VBDEV_IO_NO_ERROR.
 *
 * 동작:
 *   1) READ/WRITE/UNMAP/FLUSH 외 io_type은 NO_ERROR.
 *   2) ch->io_inflight < error_qd 면 NO_ERROR(부하 임계 미달).
 *   3) error_num을 atomic CAS로 1 감소 시도. 성공하면 발동, 0이면 NO_ERROR.
 *      여러 채널이 동시에 시도해도 정확히 N번만 발동되도록 race-free.
 *
 * 호출 체인: vbdev_error_submit_request → [이 함수].
 * 실행 컨텍스트: 채널 reactor.
 */
static uint32_t
vbdev_error_get_error_type(struct error_disk *error_disk, struct error_channel *ch,
			   uint32_t io_type)
{
	uint32_t error_num;
	struct vbdev_error_info *error_info;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		break;                            /* [한국어] 발동 가능한 io_type. */
	default:
		return VBDEV_IO_NO_ERROR;         /* [한국어] 그 외(예: RESET 등)는 발동 안 함. */
	}

	error_info = &error_disk->error_vector[io_type];

	if (ch->io_inflight < error_info->error_qd) {
		/* [한국어] 부하가 임계 미만 — 발동 보류. error_qd=0이면 항상 통과. */
		return VBDEV_IO_NO_ERROR;
	}

	error_num = error_info->error_num;
	do {
		if (error_num == 0) {
			/* [한국어] 더 발동할 횟수가 없음. */
			return VBDEV_IO_NO_ERROR;
		}
		/* [한국어] error_num을 (현재값 → 현재값-1)로 CAS 갱신. 다른 채널이 먼저 갱신했으면
		 *           error_num이 새 값으로 갱신되어 루프 재시도. RELAXED — 데이터 의존 없음. */
	} while (!__atomic_compare_exchange_n(&error_info->error_num,
					      &error_num, error_num - 1,
					      false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));

	return error_info->error_type;            /* [한국어] CAS 성공 — 발동 결정. */
}

/*
 * [한국어]
 * vbdev_error_corrupt_io_data - iov 시퀀스의 corrupt_offset 위치를 corrupt_value로 XOR.
 *
 * @bdev_io: 대상 I/O. @corrupt_offset: iov 시작 누적 오프셋. @corrupt_value: XOR 값.
 *
 * iov가 분할되어 있을 수 있으므로 누적 오프셋이 들어가는 첫 iov를 찾아 1바이트 XOR.
 * NULL 버퍼는 noop. WRITE에서는 submit 전, READ에서는 complete 후에 호출되어 변조 효과 발생.
 *
 * 호출 체인: vbdev_error_submit_request(WRITE+CORRUPT) / vbdev_error_complete_request(READ+CORRUPT).
 */
static void
vbdev_error_corrupt_io_data(struct spdk_bdev_io *bdev_io, uint64_t corrupt_offset,
			    uint8_t corrupt_value)
{
	uint8_t *buf;
	int i;

	if (bdev_io->u.bdev.iovs == NULL || bdev_io->u.bdev.iovs[0].iov_base == NULL) {
		/* [한국어] 버퍼 미할당 — 변조 대상 없음. */
		return;
	}

	for (i = 0; i < bdev_io->u.bdev.iovcnt; i++) {
		if (bdev_io->u.bdev.iovs[i].iov_len > corrupt_offset) {
			/* [한국어] 이 iov 안에 오프셋이 들어옴 — 해당 바이트 XOR. */
			buf = (uint8_t *)bdev_io->u.bdev.iovs[i].iov_base;

			buf[corrupt_offset] ^= corrupt_value;
			break;
		}

		/* [한국어] 이 iov 길이만큼 차감하고 다음 iov로 진행. */
		corrupt_offset -= bdev_io->u.bdev.iovs[i].iov_len;
	}
}

/*
 * [한국어]
 * vbdev_error_complete_request - base bdev I/O 완료 후 호출되는 콜백.
 *
 * @bdev_io: 완료된 I/O. @success: base 결과(true=성공). @cb_arg: 미사용.
 *
 * 동작:
 *   1) io_inflight 감소.
 *   2) READ + 성공 + CORRUPT_DATA 판정이었으면 결과 버퍼를 변조(클라이언트가 잘못된 데이터를 받음).
 *   3) 클라이언트에게 최종 완료 통지.
 *
 * 호출 체인: spdk_bdev_part_submit_request_ext → base 완료 → [이 콜백] → spdk_bdev_io_complete.
 * 실행 컨텍스트: base 완료가 일어나는 채널의 reactor.
 */
static void
vbdev_error_complete_request(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct error_io *error_io = (struct error_io *)bdev_io->driver_ctx;       /* [한국어] per-IO ctx. */
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	struct error_disk *error_disk = bdev_io->bdev->ctxt;                      /* [한국어] vbdev 컨텍스트. */
	struct error_channel *ch = spdk_io_channel_get_ctx(spdk_bdev_io_get_io_channel(bdev_io));

	assert(ch->io_inflight > 0);  /* [한국어] submit에서 ++ 했으므로 양수여야 함. */
	ch->io_inflight--;            /* [한국어] in-flight 카운터 감소. */

	if (success && bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		if (error_io->error_type == VBDEV_IO_CORRUPT_DATA) {
			/* [한국어] READ 결과 버퍼 변조 — 클라이언트는 손상된 데이터를 받게 됨. */
			vbdev_error_corrupt_io_data(bdev_io,
						    error_disk->error_vector[bdev_io->type].corrupt_offset,
						    error_disk->error_vector[bdev_io->type].corrupt_value);
		}
	}

	spdk_bdev_io_complete(bdev_io, status);
}

/*
 * [한국어]
 * vbdev_error_submit_request - bdev_fn_table.submit_request — I/O 진입점.
 *
 * @_ch    : 채널.
 * @bdev_io: I/O.
 *
 * 동작:
 *   - RESET → vbdev_error_reset (채널 abort).
 *   - 그 외 → get_error_type으로 판정 후:
 *       FAILURE → 즉시 FAILED 완료.
 *       NOMEM   → 즉시 NOMEM 완료(상위가 재시도 큐잉).
 *       PENDING → ch->pending_ios에 큐잉(완료 보류 — reset에서 abort).
 *       CORRUPT_DATA + WRITE → 버퍼 변조 후 정상 발행 (fallthrough).
 *       NO_ERROR → 정상 발행 (io_inflight++ 후 part API).
 *
 * 호출 체인: 클라이언트 → bdev 코어 → [이 함수] → spdk_bdev_part_submit_request_ext → base.
 * 실행 컨텍스트: 채널 reactor.
 */
static void
vbdev_error_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct error_io *error_io = (struct error_io *)bdev_io->driver_ctx;
	struct error_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct error_disk *error_disk = bdev_io->bdev->ctxt;
	int rc;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_RESET) {
		/* [한국어] RESET은 별도 경로 — 모든 채널 abort. */
		vbdev_error_reset(error_disk, bdev_io);
		return;
	}

	/* [한국어] 본 I/O에 대한 발동 여부 판정. atomic CAS로 race-free. */
	error_io->error_type = vbdev_error_get_error_type(error_disk, ch, bdev_io->type);

	switch (error_io->error_type) {
	case VBDEV_IO_FAILURE:
		/* [한국어] 즉시 실패 응답. base에 발행하지 않음. */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	case VBDEV_IO_NOMEM:
		/* [한국어] -ENOMEM 응답. 상위 레이어가 wait 큐에 등록 후 재발행하는 경로 검증. */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		break;
	case VBDEV_IO_PENDING:
		/* [한국어] 응답 보류 — pending_ios에 큐잉. 타임아웃/reset 시 abort. */
		TAILQ_INSERT_TAIL(&ch->pending_ios, error_io, link);
		break;
	case VBDEV_IO_CORRUPT_DATA:
		if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
			/* [한국어] WRITE는 발행 전 사용자 버퍼를 변조 — 디스크에 잘못된 데이터가 기록됨. */
			vbdev_error_corrupt_io_data(bdev_io,
						    error_disk->error_vector[bdev_io->type].corrupt_offset,
						    error_disk->error_vector[bdev_io->type].corrupt_value);
		}
	/* fallthrough */
	case VBDEV_IO_NO_ERROR:
		/* [한국어] 정상 발행 경로(또는 CORRUPT_DATA 후 fallthrough). */
		ch->io_inflight++;
		rc = spdk_bdev_part_submit_request_ext(&ch->part_ch, bdev_io,
						       vbdev_error_complete_request);

		if (rc) {
			SPDK_ERRLOG("bdev_error: submit request failed, rc=%d\n", rc);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			ch->io_inflight--;  /* [한국어] 발행 실패 — ++ 한 것 되돌림. */
		}
		break;
	default:
		assert(false);  /* [한국어] 모든 enum 값을 처리했으므로 도달 불가. */
		break;
	}
}

/*
 * [한국어]
 * vbdev_error_destruct - bdev_fn_table.destruct — 한 vbdev unregister 시 호출.
 *
 * @ctx: error_disk 포인터(bdev->ctxt).
 * @return: spdk_bdev_part_free 결과.
 *
 * config_remove로 g_error_config에서 설정도 제거 — 같은 base를 다시 만들 수 있도록.
 *
 * 호출 체인: spdk_bdev_unregister → [이 함수] → vbdev_error_config_remove + spdk_bdev_part_free.
 * 실행 컨텍스트: bdev 메인 thread.
 */
static int
vbdev_error_destruct(void *ctx)
{
	struct error_disk *error_disk = ctx;
	struct spdk_bdev *base_bdev = spdk_bdev_part_get_base_bdev(&error_disk->part);
	int rc;

	rc = vbdev_error_config_remove(base_bdev->name);
	if (rc != 0) {
		SPDK_ERRLOG("vbdev_error_config_remove() failed\n");
	}

	return spdk_bdev_part_free(&error_disk->part);  /* [한국어] part 메모리 해제. */
}

/*
 * [한국어]
 * vbdev_error_dump_info_json - bdev_get_bdevs 응답에 error_disk 정보 추가.
 *
 * @ctx: error_disk. @w: writer. @return: 0.
 *
 * 응답: { "error_disk": { "base_bdev": "Nvme0n1" } }
 *
 * 호출 체인: bdev_get_bdevs → [이 함수]. 실행 컨텍스트: RPC poller.
 */
static int
vbdev_error_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct error_disk *error_disk = ctx;
	struct spdk_bdev *base_bdev = spdk_bdev_part_get_base_bdev(&error_disk->part);

	spdk_json_write_named_object_begin(w, "error_disk");

	spdk_json_write_named_string(w, "base_bdev", base_bdev->name);

	spdk_json_write_object_end(w);

	return 0;
}

/*
 * [한국어]
 * vbdev_error_write_config_json - per-bdev config_json (없음 — 모듈 레벨에서 처리).
 */
static void
vbdev_error_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No config per bdev. */
}


/* [한국어] error vbdev의 fn_table — bdev 코어가 dispatch 시 참조. */
static struct spdk_bdev_fn_table vbdev_error_fn_table = {
	.destruct		= vbdev_error_destruct,
	.submit_request		= vbdev_error_submit_request,
	.dump_info_json		= vbdev_error_dump_info_json,
	.write_config_json	= vbdev_error_write_config_json
};

/*
 * [한국어]
 * vbdev_error_base_bdev_hotremove_cb - base bdev hot-remove 콜백.
 *
 * 호출 체인: 디바이스 제거 → lib/bdev → [이 함수] → spdk_bdev_part_base_hotremove.
 * 실행 컨텍스트: bdev 메인 thread.
 */
static void
vbdev_error_base_bdev_hotremove_cb(void *_part_base)
{
	struct spdk_bdev_part_base *part_base = _part_base;

	spdk_bdev_part_base_hotremove(part_base, &g_error_disks);  /* [한국어] 본 base에 묶인 vbdev 일괄 제거. */
}

/*
 * [한국어]
 * vbdev_error_ch_create_cb - 채널 생성 시 lib/bdev이 호출하는 콜백 — 채널 컨텍스트 초기화.
 *
 * @io_device: 등록된 io device(part). @ctx_buf: 새 channel 메모리.
 * @return: 0.
 *
 * 호출 체인: spdk_get_io_channel → 등록된 콜백 → [이 함수].
 * 실행 컨텍스트: 채널을 요청한 thread.
 */
static int
vbdev_error_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct error_channel *ch = ctx_buf;

	ch->io_inflight = 0;            /* [한국어] in-flight 카운터 0 시작. */
	TAILQ_INIT(&ch->pending_ios);   /* [한국어] PENDING 큐 초기화. */

	return 0;
}

/*
 * [한국어]
 * vbdev_error_ch_destroy_cb - 채널 해제 시 호출 — 본 모듈은 추가 정리 없음.
 *
 * pending_ios가 남아있더라도 reset에서 모두 abort되었으므로 비어 있는 상태.
 */
static void
vbdev_error_ch_destroy_cb(void *io_device, void *ctx_buf)
{
}

/*
 * [한국어]
 * _vbdev_error_create - 실제 vbdev 등록 본체.
 *
 * @base_bdev_name: base bdev 이름.
 * @uuid          : 부여할 UUID(NULL/0이면 자동).
 * @return        : 0=성공, -ENODEV=base 없음, -ENOMEM=할당 실패, 기타 음수=등록 실패.
 *
 * 동작:
 *   1) part_base 구성(base open, hotremove cb 등록, channel size 지정).
 *   2) error_disk 메모리 할당.
 *   3) 이름 "EE_<base>" 생성.
 *   4) UUID 지정 시 spdk_uuid_copy.
 *   5) part_construct(offset 0, 길이=base 전체).
 *
 * 호출 체인: vbdev_error_create / vbdev_error_examine → [이 함수].
 * 실행 컨텍스트: bdev 메인 thread.
 */
static int
_vbdev_error_create(const char *base_bdev_name, const struct spdk_uuid *uuid)
{
	struct spdk_bdev_part_base *base = NULL;
	struct error_disk *disk = NULL;
	struct spdk_bdev *base_bdev, *bdev;
	char *name;
	int rc;

	/* [한국어] part_base 구성 — channel size = sizeof(error_channel)로 lib/bdev이 채널 메모리 자동 할당.
	 *           ch_create_cb / ch_destroy_cb 등록. ctx 인자(NULL)는 base_free 콜백이 없어 미사용. */
	rc = spdk_bdev_part_base_construct_ext(base_bdev_name,
					       vbdev_error_base_bdev_hotremove_cb,
					       &error_if, &vbdev_error_fn_table, &g_error_disks,
					       NULL, NULL, sizeof(struct error_channel),
					       vbdev_error_ch_create_cb, vbdev_error_ch_destroy_cb,
					       &base);
	if (rc != 0) {
		if (rc != -ENODEV) {
			SPDK_ERRLOG("could not construct part base for bdev %s\n", base_bdev_name);
		}
		return rc;
	}

	base_bdev = spdk_bdev_part_base_get_bdev(base);  /* [한국어] open된 base bdev 포인터. */

	disk = calloc(1, sizeof(*disk));                 /* [한국어] error_disk 0-init 할당(error_vector도 0으로). */
	if (!disk) {
		SPDK_ERRLOG("Memory allocation failure\n");
		spdk_bdev_part_base_free(base);
		return -ENOMEM;
	}

	name = spdk_sprintf_alloc("EE_%s", base_bdev_name);  /* [한국어] vbdev 이름 생성("EE_" prefix). */
	if (!name) {
		SPDK_ERRLOG("name allocation failure\n");
		spdk_bdev_part_base_free(base);
		free(disk);
		return -ENOMEM;
	}

	if (!spdk_uuid_is_null(uuid)) {
		/* [한국어] UUID가 지정되어 있으면 part_construct 전에 미리 설정.
		 *           part_construct가 자체적으로 UUID를 생성하기 전에 우리 값을 보존. */
		bdev = spdk_bdev_part_get_bdev(&disk->part);
		spdk_uuid_copy(&bdev->uuid, uuid);
	}

	/* [한국어] part 등록 — offset=0, 길이=base_bdev 전체(즉 1:1 매핑). */
	rc = spdk_bdev_part_construct(&disk->part, base, name, 0, base_bdev->blockcnt,
				      "Error Injection Disk");
	free(name);
	if (rc) {
		SPDK_ERRLOG("could not construct part for bdev %s\n", base_bdev_name);
		/* spdk_bdev_part_construct will free name on failure */
		spdk_bdev_part_base_free(base);
		free(disk);
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * vbdev_error_create - RPC가 호출하는 외부 진입점. 설정 등록 + 즉시 생성 시도.
 *
 * @base_bdev_name, @uuid: 입력. @return: 0=성공(또는 base 미존재), 음수=errno.
 *
 * 호출 체인: rpc_bdev_error_create → [이 함수] → vbdev_error_config_add + _vbdev_error_create.
 * 실행 컨텍스트: SPDK RPC poller.
 */
int
vbdev_error_create(const char *base_bdev_name, const struct spdk_uuid *uuid)
{
	int rc;

	rc = vbdev_error_config_add(base_bdev_name, uuid);
	if (rc != 0) {
		SPDK_ERRLOG("Adding config for ErrorInjection bdev %s failed (rc=%d)\n",
			    base_bdev_name, rc);
		return rc;
	}

	rc = _vbdev_error_create(base_bdev_name, uuid);
	if (rc == -ENODEV) {
		/* [한국어] base가 없는 것은 정상 시나리오 — examine 시 자동 생성. */
		rc = 0;
	} else if (rc != 0) {
		/* [한국어] 다른 실패 — 등록한 설정도 롤백. */
		vbdev_error_config_remove(base_bdev_name);
		SPDK_ERRLOG("Could not create ErrorInjection bdev %s (rc=%d)\n",
			    base_bdev_name, rc);
	}

	return rc;
}

/*
 * [한국어]
 * vbdev_error_delete - RPC가 호출하는 외부 진입점. 비동기 unregister 시작.
 *
 * @error_vbdev_name: 삭제 대상. @cb_fn/@cb_arg: 완료 콜백과 인자.
 *
 * spdk_bdev_unregister_by_name이 음수 반환 시(이름 없음 등) cb_fn을 직접 호출.
 *
 * 호출 체인: rpc_bdev_error_delete → [이 함수] → spdk_bdev_unregister_by_name → 콜백.
 * 실행 컨텍스트: SPDK RPC poller.
 */
void
vbdev_error_delete(const char *error_vbdev_name, spdk_delete_error_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_bdev_unregister_by_name(error_vbdev_name, &error_if, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);  /* [한국어] 즉시 실패 — 비동기 경로 안 탐. 직접 콜백 호출. */
	}
}

/*
 * [한국어]
 * vbdev_error_clear_config - 모든 설정 정리(모듈 fini용).
 *
 * 호출 체인: vbdev_error_fini → [이 함수]. 실행 컨텍스트: 메인 thread (모듈 종료).
 */
static void
vbdev_error_clear_config(void)
{
	struct spdk_vbdev_error_config *cfg;

	while ((cfg = TAILQ_FIRST(&g_error_config))) {
		TAILQ_REMOVE(&g_error_config, cfg, tailq);
		free(cfg->base_bdev);
		free(cfg);
	}
}

/*
 * [한국어]
 * vbdev_error_config_find_by_base_name - base 이름으로 g_error_config 검색.
 *
 * @base_bdev_name: 비교 대상. @return: cfg 또는 NULL.
 */
static struct spdk_vbdev_error_config *
vbdev_error_config_find_by_base_name(const char *base_bdev_name)
{
	struct spdk_vbdev_error_config *cfg;

	TAILQ_FOREACH(cfg, &g_error_config, tailq) {
		if (strcmp(cfg->base_bdev, base_bdev_name) == 0) {
			return cfg;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * vbdev_error_config_add - 새 설정을 g_error_config에 추가.
 *
 * @base_bdev_name, @uuid: 입력.
 * @return: 0=성공, -EEXIST=중복, -ENOMEM=할당 실패.
 *
 * 호출 체인: vbdev_error_create → [이 함수]. 실행 컨텍스트: RPC poller.
 */
static int
vbdev_error_config_add(const char *base_bdev_name, const struct spdk_uuid *uuid)
{
	struct spdk_vbdev_error_config *cfg;

	cfg = vbdev_error_config_find_by_base_name(base_bdev_name);
	if (cfg) {
		SPDK_ERRLOG("vbdev_error_config for bdev %s already exists\n",
			    base_bdev_name);
		return -EEXIST;
	}

	cfg = calloc(1, sizeof(*cfg));
	if (!cfg) {
		SPDK_ERRLOG("calloc() failed for vbdev_error_config\n");
		return -ENOMEM;
	}

	cfg->base_bdev = strdup(base_bdev_name);
	if (!cfg->base_bdev) {
		free(cfg);
		SPDK_ERRLOG("strdup() failed for base_bdev_name\n");
		return -ENOMEM;
	}

	spdk_uuid_copy(&cfg->uuid, uuid);                 /* [한국어] UUID 보관(NULL/0이면 그대로 0). */
	TAILQ_INSERT_TAIL(&g_error_config, cfg, tailq);

	return 0;
}

/*
 * [한국어]
 * vbdev_error_config_remove - g_error_config에서 base 이름의 설정을 제거.
 *
 * @return: 0=성공, -ENOENT=없음.
 *
 * 호출 체인: vbdev_error_create(rollback), vbdev_error_destruct → [이 함수].
 * 실행 컨텍스트: RPC poller 또는 bdev 메인 thread.
 */
static int
vbdev_error_config_remove(const char *base_bdev_name)
{
	struct spdk_vbdev_error_config *cfg;

	cfg = vbdev_error_config_find_by_base_name(base_bdev_name);
	if (!cfg) {
		return -ENOENT;
	}

	TAILQ_REMOVE(&g_error_config, cfg, tailq);
	free(cfg->base_bdev);
	free(cfg);
	return 0;
}

/*
 * [한국어]
 * vbdev_error_init - 모듈 초기화 콜백 — noop(전역 TAILQ는 정적 초기화).
 */
static int
vbdev_error_init(void)
{
	return 0;
}

/*
 * [한국어]
 * vbdev_error_fini - 모듈 종료 콜백 — 모든 설정 정리.
 */
static void
vbdev_error_fini(void)
{
	vbdev_error_clear_config();
}

/*
 * [한국어]
 * vbdev_error_examine - base bdev 등록 시 자동으로 vbdev 생성 시도.
 *
 * @bdev: 새로 등록된 bdev.
 *
 * 호출 체인: lib/bdev (새 bdev 등록) → 모든 모듈 examine_config → [이 함수]
 *                                                                 → _vbdev_error_create.
 * 실행 컨텍스트: bdev 메인 thread.
 */
static void
vbdev_error_examine(struct spdk_bdev *bdev)
{
	struct spdk_vbdev_error_config *cfg;
	int rc;

	cfg = vbdev_error_config_find_by_base_name(bdev->name);
	if (cfg != NULL) {
		rc = _vbdev_error_create(bdev->name, &cfg->uuid);
		if (rc != 0) {
			SPDK_ERRLOG("could not create error vbdev for bdev %s at examine\n",
				    bdev->name);
		}
	}

	spdk_bdev_module_examine_done(&error_if);  /* [한국어] examine 완료 알림. */
}

/*
 * [한국어]
 * vbdev_error_config_json - 모듈 레벨 config_json — 모든 error 설정을 RPC 호출 형태로 직렬화.
 *
 * 호출 체인: save_config → 각 모듈 config_json → [이 함수].
 */
static int
vbdev_error_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_vbdev_error_config *cfg;

	TAILQ_FOREACH(cfg, &g_error_config, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "method", "bdev_error_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_name", cfg->base_bdev);
		if (!spdk_uuid_is_null(&cfg->uuid)) {
			/* [한국어] UUID가 명시 지정된 경우만 출력 — 자동 생성 UUID는 재현성을 위해 생략. */
			spdk_json_write_named_uuid(w, "uuid", &cfg->uuid);
		}
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	return 0;
}
