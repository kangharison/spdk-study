/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] vbdev_opal 구현 - TCG Opal SED 기반 가상 bdev (vbdev_opal.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVMe SED(Self-Encrypting Drive)에서 정의된 한 Locking Range를 SPDK bdev
 * 한 개로 노출하는 가상 bdev 모듈을 구현한다. 베이스 nvme bdev (예: Nvme0n1)에 대해
 * spdk_bdev_part 프레임워크를 사용해 [range_start, range_start+range_length) LBA 구간만
 * 보이는 partition bdev를 만들고, 그 LBA 구간을 SED Locking Range로 보호한다. 사용자는
 * RPC로 Locking Range를 만들고 잠금/해제하며, vbdev_opal은 모든 IO를 베이스 bdev로
 * 그대로 전달한다 (실제 보호는 SED 펌웨어가 LBA 범위 단위로 거부하거나 허용).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   - 생성: vbdev_opal_rpc → vbdev_opal_create()
 *           → spdk_bdev_part_base_construct_ext + spdk_bdev_part_construct
 *           → spdk_bdev_register (vbdev이 SPDK bdev 레이어에 노출)
 *   - I/O: 사용자 → spdk_bdev_*() → vbdev_opal_submit_request → spdk_bdev_part_submit_request
 *          → 베이스 bdev (nvme bdev) → lib/nvme → NVMe SQ/CQ → 디바이스
 *   - SED 잠금/해제: vbdev_opal_set_lock_state → spdk_opal_cmd_lock_unlock → SED Security Send
 *   - 핫리무브: 베이스 bdev 사라지면 vbdev_opal_base_bdev_hotremove_cb → 모든 vbdev 일괄 제거
 *
 * 데이터 경로(IO 발행) 자체는 SED를 거치지 않으며, SED는 별도의 보안 명령(Security Send/Receive,
 * NVMe opcode 0x81/0x82)으로만 제어된다. 사용자가 잠긴 LBA에 IO를 보내면 디바이스가
 * Status Code "Access Denied"로 거부.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/opal.h (lib/nvme/opal API), spdk/bdev_module.h (bdev 등록 + part 프레임워크),
 *         vbdev_opal.h (모듈 외부 API), spdk/log.h, spdk/string.h.
 * - 의존받음: vbdev_opal_rpc.c (사용자 RPC), bdev_nvme.c (nvme_ctrlr 라이프사이클 연동).
 * - 데이터 흐름: 사용자 IO → vbdev → spdk_bdev_part → nvme bdev → lib/nvme → 디바이스.
 * - 공유 전역: g_opal_vbdev (모든 opal_vbdev 리스트), g_opal_base (모든 part_base 리스트).
 *   둘 다 app 스레드에서만 변경.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct opal_vbdev: 한 개의 vbdev_opal 인스턴스 (베이스 + range_id + part_bdev).
 * - struct vbdev_opal_part_base: 한 베이스 nvme bdev에 대한 spdk_bdev_part_base 래퍼.
 *   같은 베이스에서 여러 vbdev_opal이 만들어지면 part_base는 공유.
 * - struct vbdev_opal_channel: spdk_bdev_part_channel 래퍼 (채널 컨텍스트).
 * - struct vbdev_opal_bdev_io: 채널/bdev_io 추적 + queue_io_wait 엔트리 (-ENOMEM 대비).
 * - vbdev_opal_create / _destruct: 생성/파괴 + SED Locking Range setup/erase.
 * - vbdev_opal_set_lock_state / _enable_new_user: SED 잠금 상태/User 권한 변경.
 * - vbdev_opal_submit_request / _vbdev_opal_submit_request: IO를 베이스 bdev로 위임.
 * - opal_vbdev_fn_table / opal_if: SPDK bdev 모듈 등록 정보.
 */

#include "spdk/opal.h"          /* [한국어] lib/nvme/opal 공개 API: spdk_opal_cmd_*. */
#include "spdk/bdev_module.h"   /* [한국어] spdk_bdev_part_*, SPDK_BDEV_MODULE_REGISTER 등. */
#include "vbdev_opal.h"          /* [한국어] 외부 API 선언. */
#include "spdk/log.h"            /* [한국어] SPDK_ERRLOG/DEBUGLOG/LOG_REGISTER_COMPONENT. */
#include "spdk/string.h"         /* [한국어] spdk_sprintf_alloc, strcasecmp 등. */

/* OPAL locking range only supports operations on nsid=1 for now */
/* [한국어] 현재 구현은 namespace 1에 대해서만 Locking Range를 만들 수 있다.
 * SED 펌웨어 자체는 다중 namespace를 지원할 수 있지만 SPDK 측 코드 한계. */
#define NSID_SUPPORTED		1

/*
 * [한국어]
 * struct opal_vbdev - 단일 Locking Range를 표현하는 vbdev 인스턴스.
 *
 * 같은 nvme_ctrlr 위에 여러 Locking Range가 만들어지면 각각에 대해 별도의 opal_vbdev가
 * g_opal_vbdev 리스트에 등록된다. spdk_bdev_part는 베이스 bdev의 LBA 부분 영역을 노출하는
 * SPDK 공통 메커니즘이며, 여기서는 [range_start, +range_length) 구간을 잘라낸다.
 */
struct opal_vbdev {
	char *name;
	/* [한국어] vbdev 이름 (예: "Nvme0n1r0" - r0은 Locking Range 0).
	 * 설정자: vbdev_opal_create()의 spdk_sprintf_alloc. 읽는 자: lookup, RPC 응답.
	 * 동기화: 변경은 app 스레드에서만. */

	struct nvme_ctrlr *nvme_ctrlr;
	/* [한국어] 이 Locking Range가 속한 NVMe 컨트롤러. opal_dev는 ctrlr 내부 필드. */

	struct spdk_opal_dev *opal_dev;
	/* [한국어] lib/nvme/opal이 만든 Opal 드라이버 핸들 (nvme_ctrlr->opal_dev 사본).
	 * 자주 사용해서 캐싱. */

	struct spdk_bdev_part *bdev_part;
	/* [한국어] 베이스 bdev 위에 만들어진 partition bdev (실제 SPDK bdev로 등록됨).
	 * spdk_bdev_part는 첫 필드가 spdk_bdev이므로 이 포인터를 spdk_bdev *로 캐스팅 가능. */

	uint8_t locking_range_id;
	/* [한국어] SED 내 Locking Range 인덱스 (0~7 등). */
	uint64_t range_start;
	/* [한국어] 보호 시작 LBA (베이스 bdev 기준 절대 LBA). */
	uint64_t range_length;
	/* [한국어] 보호 LBA 개수. */
	struct vbdev_opal_part_base *opal_base;
	/* [한국어] 베이스 nvme bdev에 대한 part_base 래퍼 (같은 베이스를 쓰는 여러 vbdev가 공유). */

	TAILQ_ENTRY(opal_vbdev) tailq;
	/* [한국어] g_opal_vbdev 리스트 노드. */
};

/* [한국어] 모든 vbdev_opal 인스턴스를 추적하는 전역 리스트.
 * 설정자: vbdev_opal_create/_delete (app 스레드).
 * 읽는 자: vbdev_opal_destruct, get_info, set_lock_state 등 모든 lookup. */
static TAILQ_HEAD(, opal_vbdev) g_opal_vbdev =
	TAILQ_HEAD_INITIALIZER(g_opal_vbdev);

/*
 * [한국어]
 * struct vbdev_opal_bdev_io - 한 IO 요청에 대한 vbdev_opal 측 추가 컨텍스트.
 *
 * spdk_bdev_io::driver_ctx에 임베드되며 (모듈 등록 시 get_ctx_size로 크기 보고),
 * -ENOMEM으로 발행이 실패했을 때 spdk_bdev_queue_io_wait에 등록해 재시도하기 위한 정보 보관.
 */
struct vbdev_opal_bdev_io {
	struct spdk_io_channel *ch;
	/* [한국어] 원래 IO를 받은 채널. resubmit 시 같은 채널로 재발행. */
	struct spdk_bdev_io *bdev_io;
	/* [한국어] 원래의 bdev_io 포인터 (resubmit 시 재사용). */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	/* [한국어] queue_io_wait에 등록되는 노드. cb_fn=vbdev_opal_resubmit_io. */
};

/*
 * [한국어]
 * struct vbdev_opal_channel - vbdev의 채널 컨텍스트 (스레드 단위).
 *
 * spdk_bdev_part_channel을 임베드해 베이스 bdev의 채널 핸들을 가진다.
 * spdk_io_channel ctx_buf 크기 = sizeof(struct vbdev_opal_channel).
 */
struct vbdev_opal_channel {
	struct spdk_bdev_part_channel part_ch;
	/* [한국어] 베이스 bdev에 대한 채널을 포함. 베이스 bdev의 io_channel을 자동 관리. */
};

/*
 * [한국어]
 * struct vbdev_opal_part_base - 한 베이스 bdev에 대한 part_base 래퍼.
 *
 * 같은 베이스 bdev에서 여러 vbdev_opal이 만들어질 수 있는데, spdk_bdev_part_base는
 * 베이스당 1개만 만들어야 하므로 g_opal_base에서 lookup해 공유한다.
 */
struct vbdev_opal_part_base {
	char *nvme_ctrlr_name;
	/* [한국어] 베이스 NVMe 컨트롤러 이름 (lookup용). */
	struct spdk_bdev_part_base *part_base;
	/* [한국어] SPDK가 관리하는 part_base 핸들. */
	SPDK_BDEV_PART_TAILQ part_tailq;
	/* [한국어] 이 베이스에 속한 모든 spdk_bdev_part 리스트. */
	TAILQ_ENTRY(vbdev_opal_part_base) tailq;
	/* [한국어] g_opal_base 리스트 노드. */
};

/* [한국어] 모든 part_base를 추적하는 전역 리스트. */
static TAILQ_HEAD(, vbdev_opal_part_base) g_opal_base = TAILQ_HEAD_INITIALIZER(g_opal_base);

/* [한국어] 내부 IO submit 헬퍼의 forward 선언 (queue_io에서 호출, submit_request에서도 호출). */
static void _vbdev_opal_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io);

/* [한국어] examine 콜백 forward (모듈 등록에 사용). */
static void vbdev_opal_examine(struct spdk_bdev *bdev);

/*
 * [한국어]
 * vbdev_opal_delete - opal_vbdev 메타 객체를 g_opal_vbdev에서 떼고 메모리 해제.
 *
 * @opal_bdev: 해제 대상.
 * @return: 없음.
 *
 * 주의: 이 함수는 spdk_bdev 자체를 unregister하지 않는다. part_bdev/spdk_bdev 해제는
 * vbdev_opal_destruct_bdev → spdk_bdev_unregister 경로에서 수행. 즉 이 함수는 메타 정리만.
 * 호출자: 정상 destruct, 에러 분기, hotremove cb, fini.
 */
static void
vbdev_opal_delete(struct opal_vbdev *opal_bdev)
{
	TAILQ_REMOVE(&g_opal_vbdev, opal_bdev, tailq);   /* [한국어] 전역 리스트에서 제거. */
	free(opal_bdev->name);                            /* [한국어] sprintf_alloc된 이름 해제. */
	free(opal_bdev);                                   /* [한국어] 구조체 자체 해제. */
	opal_bdev = NULL;                                  /* [한국어] (로컬 변수 nullify, 호출자에는 영향 없음). */
}

/*
 * [한국어]
 * vbdev_opal_clear - 모든 vbdev_opal 메타 객체를 일괄 정리 (fini 시 사용).
 *
 * TAILQ_FOREACH_SAFE로 순회 중 delete가 안전하게 동작.
 */
static void
vbdev_opal_clear(void)
{
	struct opal_vbdev *opal_bdev, *tmp;

	TAILQ_FOREACH_SAFE(opal_bdev, &g_opal_vbdev, tailq, tmp) {
		vbdev_opal_delete(opal_bdev);
	}
}

/*
 * [한국어]
 * vbdev_opal_init - 모듈 init 콜백. 현재는 미구현 (TODO).
 * @return: 0 항상 성공.
 * 호출자: SPDK bdev subsystem 초기화 단계.
 */
static int
vbdev_opal_init(void)
{
	/* TODO */
	return 0;
}

/*
 * [한국어]
 * vbdev_opal_fini - 모듈 fini 콜백. 모든 메타 정리.
 * 호출자: SPDK bdev subsystem 종료 단계.
 */
static void
vbdev_opal_fini(void)
{
	vbdev_opal_clear();
}

/*
 * [한국어]
 * vbdev_opal_get_ctx_size - SPDK bdev 코어에 driver_ctx 크기를 알려주는 콜백.
 * @return: sizeof(struct vbdev_opal_bdev_io).
 *
 * spdk_bdev_io 끝에 이만큼의 공간이 자동 할당되어 driver_ctx로 노출된다.
 */
static int
vbdev_opal_get_ctx_size(void)
{
	return sizeof(struct vbdev_opal_bdev_io);
}

/* delete all the config of the same base bdev */
/*
 * [한국어]
 * vbdev_opal_delete_all_base_config - 한 베이스 bdev에 매달린 모든 opal_vbdev 메타 정리.
 *
 * 베이스가 핫리무브될 때 같은 nvme_ctrlr_name을 사용하는 vbdev들을 모두 제거.
 * 베이스 nvme bdev 이름 비교가 아니라 컨트롤러 이름 비교임에 주의 (멀티 namespace 대비).
 */
static void
vbdev_opal_delete_all_base_config(struct vbdev_opal_part_base *base)
{
	char *nvme_ctrlr_name = base->nvme_ctrlr_name;
	struct opal_vbdev *bdev, *tmp_bdev;

	TAILQ_FOREACH_SAFE(bdev, &g_opal_vbdev, tailq, tmp_bdev) {
		/* [한국어] 컨트롤러 이름이 일치하면 메타 객체 정리. */
		if (!strcmp(nvme_ctrlr_name, bdev->nvme_ctrlr->nbdev_ctrlr->name)) {
			vbdev_opal_delete(bdev);
		}
	}
}

/*
 * [한국어]
 * _vbdev_opal_destruct - spdk_bdev fn_table::destruct 콜백.
 *
 * @ctx: ctxt 포인터 (vbdev_opal_create에서 part_bdev로 설정).
 * @return: spdk_bdev_part_free 결과.
 *
 * spdk_bdev_unregister가 트리거하는 destruct 콜백으로, part_bdev 자체 해제만 담당.
 * 메타(opal_vbdev) 정리는 vbdev_opal_destruct_bdev에서 별도로 수행.
 */
static int
_vbdev_opal_destruct(void *ctx)
{
	struct spdk_bdev_part *part = ctx;

	return spdk_bdev_part_free(part);
}

/*
 * [한국어]
 * vbdev_opal_base_free - part_base 자체가 해제될 때 호출되는 콜백.
 *
 * spdk_bdev_part_base_construct_ext에 등록된 base_bdev_free_fn으로, 모든 part가
 * 정리되어 part_base가 free될 때 SPDK가 호출.
 */
static void
vbdev_opal_base_free(void *ctx)
{
	struct vbdev_opal_part_base *base = ctx;

	TAILQ_REMOVE(&g_opal_base, base, tailq);   /* [한국어] 전역 리스트에서 제거. */

	free(base->nvme_ctrlr_name);                 /* [한국어] strdup된 이름 해제. */
	free(base);
}

/*
 * [한국어]
 * vbdev_opal_resubmit_io - queue_io_wait의 cb_fn. ENOMEM 후 슬롯 확보 시 호출됨.
 *
 * 보관해뒀던 채널/bdev_io로 다시 _vbdev_opal_submit_request 호출.
 * 호출 컨텍스트: 베이스 bdev가 큐 슬롯을 비웠을 때 같은 SPDK 스레드.
 */
static void
vbdev_opal_resubmit_io(void *arg)
{
	struct vbdev_opal_bdev_io *io_ctx = (struct vbdev_opal_bdev_io *)arg;

	_vbdev_opal_submit_request(io_ctx->ch, io_ctx->bdev_io);
}

/*
 * [한국어]
 * vbdev_opal_queue_io - submit이 -ENOMEM으로 실패했을 때 IO를 wait queue에 등록.
 *
 * spdk_bdev_queue_io_wait는 베이스 bdev의 채널에 등록되어 다음에 슬롯이 비면
 * cb_fn(=resubmit_io)을 호출한다. 등록 자체도 실패하면 즉시 IO 실패 완료 보고.
 */
static void
vbdev_opal_queue_io(struct vbdev_opal_bdev_io *io_ctx)
{
	struct vbdev_opal_channel *ch = spdk_io_channel_get_ctx(io_ctx->ch);   /* [한국어] 채널 컨텍스트 얻기. */
	int rc;

	/* [한국어] wait 엔트리 초기화: 어느 bdev에 대한 대기인지, 깨울 때 호출할 cb_fn. */
	io_ctx->bdev_io_wait.bdev = io_ctx->bdev_io->bdev;
	io_ctx->bdev_io_wait.cb_fn = vbdev_opal_resubmit_io;
	io_ctx->bdev_io_wait.cb_arg = io_ctx;

	/* [한국어] 베이스 채널의 wait queue에 등록. base_ch는 spdk_bdev_part_channel이 보관한 베이스 채널. */
	rc = spdk_bdev_queue_io_wait(io_ctx->bdev_io->bdev, ch->part_ch.base_ch, &io_ctx->bdev_io_wait);

	if (rc != 0) {
		/* [한국어] 등록도 실패하면 더 이상 회복 불가 → IO 실패 보고. */
		SPDK_ERRLOG("Queue io failed in vbdev_opal_queue_io: %d\n", rc);
		spdk_bdev_io_complete(io_ctx->bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * [한국어]
 * _vbdev_opal_submit_request - 실제로 베이스 bdev에 IO를 위임하는 내부 함수.
 *
 * @_ch: vbdev_opal 채널.
 * @bdev_io: 사용자 IO 요청.
 *
 * spdk_bdev_part_submit_request가 LBA 오프셋 보정(베이스 기준 + range_start)을 자동 처리.
 * 반환값별 처리:
 *   - 0: 성공적으로 베이스에 enqueue됨.
 *   - -ENOMEM: 베이스 큐 만석 → queue_io로 대기열에 등록.
 *   - 기타: IO 실패 완료.
 */
static void
_vbdev_opal_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_opal_channel *ch = spdk_io_channel_get_ctx(_ch);   /* [한국어] 채널 ctx. */
	struct vbdev_opal_bdev_io *io_ctx = (struct vbdev_opal_bdev_io *)bdev_io->driver_ctx;   /* [한국어] driver_ctx 임베드. */
	int rc;

	/* [한국어] 베이스 bdev로 IO 발행 위임 (오프셋/길이는 part 프레임워크가 보정). */
	rc = spdk_bdev_part_submit_request(&ch->part_ch, bdev_io);
	if (rc) {
		if (rc == -ENOMEM) {
			/* [한국어] ENOMEM은 일시적 큐 만석 → wait queue 대기 후 재시도. */
			SPDK_DEBUGLOG(vbdev_opal, "opal: no memory, queue io.\n");
			io_ctx->ch = _ch;          /* [한국어] resubmit 시 사용할 채널 보관. */
			io_ctx->bdev_io = bdev_io; /* [한국어] resubmit 시 사용할 bdev_io 보관. */
			vbdev_opal_queue_io(io_ctx);
		} else {
			/* [한국어] 다른 에러는 회복 불가 → 실패 보고. */
			SPDK_ERRLOG("opal: error on io submission, rc=%d.\n", rc);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/*
 * [한국어]
 * vbdev_opal_io_get_buf_cb - READ에 대해 버퍼 할당이 완료된 후 호출되는 콜백.
 *
 * Read IO는 bdev 코어가 destination 버퍼를 할당/준비한 뒤 이 콜백을 호출. 성공이면
 * 실제 submit으로 진행, 실패면 IO 실패.
 */
static void
vbdev_opal_io_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	_vbdev_opal_submit_request(ch, bdev_io);
}

/*
 * [한국어]
 * vbdev_opal_submit_request - bdev fn_table::submit_request 진입점.
 *
 * @ch: vbdev 채널, @bdev_io: 사용자 IO.
 *
 * READ는 bdev 코어의 get_buf 인프라를 거쳐 destination 버퍼 준비 후 발행 (제로 카피 위해).
 * 그 외(write/flush/unmap 등)는 즉시 베이스로 전달.
 * 호출 컨텍스트: 사용자가 spdk_bdev_*() 호출한 그 SPDK 스레드.
 */
static void
vbdev_opal_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		/* [한국어] Read는 버퍼 준비 후 콜백으로 진행. num_blocks*blocklen만큼 필요. */
		spdk_bdev_io_get_buf(bdev_io, vbdev_opal_io_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	default:
		/* [한국어] 그 외 IO 타입은 즉시 위임. */
		_vbdev_opal_submit_request(ch, bdev_io);
		break;
	}
}

/*
 * [한국어]
 * vbdev_opal_get_info_from_bdev - vbdev 이름으로 Locking Range 메타데이터 조회.
 *
 * vbdev_opal.h §2 참조. SED에 Get-Range를 발급해 캐시를 갱신한 뒤, 캐시에서 info를 반환.
 * 호출 컨텍스트: app 스레드 (RPC 핸들러 경유).
 */
struct spdk_opal_locking_range_info *
vbdev_opal_get_info_from_bdev(const char *opal_bdev_name, const char *password)
{
	struct opal_vbdev *vbdev;
	struct nvme_ctrlr *nvme_ctrlr;
	int locking_range_id;
	int rc;

	/* [한국어] 1단계: 이름으로 vbdev lookup. */
	TAILQ_FOREACH(vbdev, &g_opal_vbdev, tailq) {
		if (strcmp(vbdev->name, opal_bdev_name) == 0) {
			break;
		}
	}

	if (vbdev == NULL) {
		SPDK_ERRLOG("%s not found\n", opal_bdev_name);
		return NULL;
	}

	/* [한국어] 2단계: 컨트롤러 유효성 검증 (제거 진행 중일 수 있음). */
	nvme_ctrlr = vbdev->nvme_ctrlr;
	if (nvme_ctrlr == NULL) {
		SPDK_ERRLOG("can't find nvme_ctrlr of %s\n", vbdev->name);
		return NULL;
	}

	/* [한국어] 3단계: SED에 Get-Range 명령 (Admin1 인증 필요). */
	locking_range_id = vbdev->locking_range_id;
	rc = spdk_opal_cmd_get_locking_range_info(nvme_ctrlr->opal_dev, password,
			OPAL_ADMIN1, locking_range_id);
	if (rc) {
		SPDK_ERRLOG("Get locking range info error: %d\n", rc);
		return NULL;
	}

	/* [한국어] 4단계: 캐시된 info 반환 (호출자는 read-only로 사용). */
	return spdk_opal_get_locking_range_info(nvme_ctrlr->opal_dev, locking_range_id);
}

/*
 * [한국어]
 * vbdev_opal_dump_info_json - bdev_get_bdevs RPC 시 vbdev_opal 정보 dump 콜백.
 *
 * 출력: {"opal": {"base_bdev": "...", "offset_blocks": N}}.
 */
static int
vbdev_opal_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct spdk_bdev_part *part = ctx;
	struct spdk_bdev *base_bdev = spdk_bdev_part_get_base_bdev(part);   /* [한국어] 베이스 bdev 참조. */
	uint64_t offset = spdk_bdev_part_get_offset_blocks(part);            /* [한국어] LBA 시작 오프셋. */

	spdk_json_write_named_object_begin(w, "opal");

	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(base_bdev));
	spdk_json_write_named_uint64(w, "offset_blocks", offset);

	spdk_json_write_object_end(w);

	return 0;
}

/*
 * [한국어]
 * vbdev_opal_base_bdev_hotremove_cb - 베이스 nvme bdev가 핫리무브될 때 호출되는 콜백.
 *
 * SPDK part 프레임워크가 모든 part_bdev를 hotremove하고, 이어서 우리 측 메타도 일괄 정리.
 * 예: NVMe SSD가 PCIe에서 빠지면 lib/nvme이 nvme bdev 제거 → 이 콜백.
 */
static void
vbdev_opal_base_bdev_hotremove_cb(void *_part_base)
{
	struct spdk_bdev_part_base *part_base = _part_base;
	struct vbdev_opal_part_base *base = spdk_bdev_part_base_get_ctx(part_base);

	/* [한국어] 1단계: SPDK part 프레임워크에 hotremove 위임 (각 part_bdev unregister 트리거). */
	spdk_bdev_part_base_hotremove(part_base, spdk_bdev_part_base_get_tailq(part_base));
	/* [한국어] 2단계: 우리 측 opal_vbdev 메타 일괄 정리. */
	vbdev_opal_delete_all_base_config(base);
}

/*
 * [한국어]
 * vbdev_opal_io_type_supported - bdev fn_table::io_type_supported 콜백.
 *
 * Locking Range는 LBA 범위만 가지고 있으므로 베이스가 지원하는 IO type을 그대로 따라간다.
 */
static bool
vbdev_opal_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct spdk_bdev_part *part = ctx;
	struct spdk_bdev *base_bdev = spdk_bdev_part_get_base_bdev(part);

	return spdk_bdev_io_type_supported(base_bdev, io_type);
}

/*
 * [한국어] vbdev_opal의 spdk_bdev fn_table.
 * spdk_bdev 코어가 destruct/submit/io_type_supported/dump_info_json을 콜백으로 호출.
 * write_config_json은 NULL → vbdev_opal은 save_config 비대상.
 */
static struct spdk_bdev_fn_table opal_vbdev_fn_table = {
	.destruct           = _vbdev_opal_destruct,
	.submit_request     = vbdev_opal_submit_request,
	.io_type_supported  = vbdev_opal_io_type_supported,
	.dump_info_json     = vbdev_opal_dump_info_json,
	.write_config_json  = NULL,
};

/*
 * [한국어] vbdev_opal SPDK bdev 모듈 등록 정보.
 * SPDK_BDEV_MODULE_REGISTER 매크로가 컴파일 타임에 모듈 리스트에 자동 등록.
 */
static struct spdk_bdev_module opal_if = {
	.name           = "opal",                 /* [한국어] 모듈 이름. */
	.module_init    = vbdev_opal_init,        /* [한국어] subsystem init 시 호출. */
	.module_fini    = vbdev_opal_fini,        /* [한국어] subsystem fini 시 호출. */
	.get_ctx_size   = vbdev_opal_get_ctx_size,/* [한국어] driver_ctx 크기. */
	.examine_config = vbdev_opal_examine,     /* [한국어] 새 베이스 bdev 등장 시 호출 (현재 no-op). */
	.config_json    = NULL,                   /* [한국어] save_config 비대상. */
};

SPDK_BDEV_MODULE_REGISTER(opal, &opal_if)
/* [한국어] ↑ SPDK bdev 모듈 시스템에 "opal" 모듈을 등록. 부팅 시 module_init 호출. */

/*
 * [한국어]
 * vbdev_opal_create - 새 vbdev_opal 인스턴스 생성. 자세한 의미는 vbdev_opal.h §2 참조.
 *
 * 동작 단계 요약:
 *   1) 인자 검증 (nsid, ctrlr 존재, opal 지원).
 *   2) opal_bdev 메타 객체 할당 + 필드 채우기.
 *   3) 베이스 nvme bdev lookup.
 *   4) 같은 베이스에 대한 part_base가 이미 있는지 확인 → 없으면 새로 construct.
 *   5) part_bdev 할당 후 g_opal_vbdev에 등록.
 *   6) 이름 생성 ("<base>r<range_id>", 예: "Nvme0n1r0").
 *   7) SED Locking Range 메타데이터 설정 (Setup-Range).
 *   8) spdk_bdev_part_construct 호출 → SPDK bdev로 등록.
 *   9) 초기 상태로 RWLOCK (잠금) 설정 → 사용자가 명시적으로 unlock하기 전엔 IO 차단.
 *
 * 에러 발생 시 err: 라벨로 가서 메타와 part_bdev를 정리. (단, 부분 단계 실패 시
 * 일관성에 주의 - 코드 흐름상 4단계 후 5단계 실패하면 part_base는 살아남고 메타만 정리됨.)
 */
int
vbdev_opal_create(const char *nvme_ctrlr_name, uint32_t nsid, uint8_t locking_range_id,
		  uint64_t range_start, uint64_t range_length, const char *password)
{
	int rc;                                              /* [한국어] 단계별 결과 코드. */
	char *opal_vbdev_name;                                /* [한국어] 새 bdev 이름. */
	char *base_bdev_name;                                  /* [한국어] 베이스 nvme bdev 이름. */
	struct nvme_ctrlr *nvme_ctrlr;                        /* [한국어] 컨트롤러 lookup 결과. */
	struct opal_vbdev *opal_bdev;                         /* [한국어] 메타 객체. */
	struct vbdev_opal_part_base *opal_part_base = NULL;   /* [한국어] 베이스 part_base (재사용 또는 신규). */
	struct spdk_bdev_part *part_bdev;                     /* [한국어] SPDK part bdev 객체. */
	struct nvme_bdev *nvme_bdev;                          /* [한국어] 베이스 nvme bdev. */
	struct nvme_ns *nvme_ns;                              /* [한국어] 해당 namespace 객체. */

	/* [한국어] 1단계: nsid 제한 검증. 현재 namespace 1만 허용. */
	if (nsid != NSID_SUPPORTED) {
		SPDK_ERRLOG("nsid %d not supported", nsid);
		return -EINVAL;
	}

	/* [한국어] 2단계: 컨트롤러 lookup. */
	nvme_ctrlr = nvme_ctrlr_get_by_name(nvme_ctrlr_name);
	if (!nvme_ctrlr) {
		SPDK_ERRLOG("get nvme ctrlr failed\n");
		return -ENODEV;
	}

	/* [한국어] 3단계: SED 지원 여부 확인. attach 시 SED 감지된 경우만 opal_dev 채워져 있음. */
	if (!nvme_ctrlr->opal_dev) {
		SPDK_ERRLOG("Opal not supported\n");
		return -ENOTSUP;
	}

	/* [한국어] 4단계: 메타 객체 할당. */
	opal_bdev = calloc(1, sizeof(struct opal_vbdev));
	if (!opal_bdev) {
		SPDK_ERRLOG("allocation for opal_bdev failed\n");
		return -ENOMEM;
	}

	/* [한국어] 5단계: 메타 필드 채우기. */
	opal_bdev->locking_range_id = locking_range_id;
	opal_bdev->range_start = range_start;
	opal_bdev->range_length = range_length;

	opal_bdev->nvme_ctrlr = nvme_ctrlr;
	opal_bdev->opal_dev = nvme_ctrlr->opal_dev;

	/* [한국어] 6단계: namespace 객체 lookup → 베이스 nvme bdev 추출. */
	nvme_ns = nvme_ctrlr_get_ns(nvme_ctrlr, nsid);
	if (nvme_ns == NULL) {
		free(opal_bdev);
		return -ENODEV;
	}

	nvme_bdev = nvme_ns->bdev;
	assert(nvme_bdev != NULL);
	base_bdev_name = nvme_bdev->disk.name;   /* [한국어] 예: "Nvme0n1". */

	/* traverse base list to see if part_base is already create for this base bdev */
	/* [한국어] 7단계: 같은 베이스의 part_base가 이미 있나? 있으면 재사용. */
	TAILQ_FOREACH(opal_part_base, &g_opal_base, tailq) {
		if (!strcmp(spdk_bdev_part_base_get_bdev_name(opal_part_base->part_base), base_bdev_name)) {
			break;
		}
	}

	/* If there is not a corresponding opal_part_base, a new opal_part_base will be created.
	   For each new part_base, there will be one tailq to store all the parts of this base */
	/* [한국어] 8단계: part_base가 없으면 새로 construct. */
	if (opal_part_base == NULL) {
		opal_part_base = calloc(1, sizeof(*opal_part_base));
		if (opal_part_base == NULL) {
			SPDK_ERRLOG("Could not allocate opal_part_base\n");
			free(opal_bdev);
			return -ENOMEM;
		}
		TAILQ_INIT(&opal_part_base->part_tailq);   /* [한국어] 이 베이스의 part 리스트 초기화. */

		/* [한국어] spdk_bdev_part_base_construct_ext: 베이스 bdev를 점유하고 part 프레임워크 활성화.
		 * - hotremove_cb: 베이스 사라질 때 호출.
		 * - opal_if: 우리 모듈 식별자.
		 * - fn_table: 모든 part_bdev에 적용될 콜백 묶음.
		 * - part_tailq: 이 베이스에 만들어질 part_bdev들 추적.
		 * - base_free_fn: part_base 자체 free될 때.
		 * - sizeof(channel): 채널 컨텍스트 크기. */
		rc = spdk_bdev_part_base_construct_ext(base_bdev_name,
						       vbdev_opal_base_bdev_hotremove_cb, &opal_if,
						       &opal_vbdev_fn_table, &opal_part_base->part_tailq,
						       vbdev_opal_base_free, opal_part_base,
						       sizeof(struct vbdev_opal_channel), NULL, NULL,
						       &opal_part_base->part_base);
		if (rc != 0) {
			if (rc != -ENODEV) {
				SPDK_ERRLOG("Could not allocate part_base\n");
			}
			free(opal_bdev);
			free(opal_part_base);
			return rc;
		}
		opal_part_base->nvme_ctrlr_name = strdup(nvme_ctrlr_name);
		if (opal_part_base->nvme_ctrlr_name == NULL) {
			free(opal_bdev);
			spdk_bdev_part_base_free(opal_part_base->part_base);
			return -ENOMEM;
		}

		TAILQ_INSERT_TAIL(&g_opal_base, opal_part_base, tailq);
	}
	assert(opal_part_base != NULL);
	opal_bdev->opal_base = opal_part_base;   /* [한국어] 메타에 part_base 역참조 보관. */

	/* [한국어] 9단계: spdk_bdev_part 객체 할당. */
	part_bdev = calloc(1, sizeof(struct spdk_bdev_part));
	if (!part_bdev) {
		SPDK_ERRLOG("Could not allocate part_bdev\n");
		free(opal_bdev);
		return -ENOMEM;
	}

	/* [한국어] 10단계: 전역 리스트에 메타 등록 (이후 단계 실패 시 err: 라벨로 정리). */
	TAILQ_INSERT_TAIL(&g_opal_vbdev, opal_bdev, tailq);
	/* [한국어] 11단계: bdev 이름 생성. 형식: "<base>r<range_id>" (예: nvme0n1r1). */
	opal_vbdev_name = spdk_sprintf_alloc("%sr%" PRIu8, base_bdev_name,
					     opal_bdev->locking_range_id);  /* e.g.: nvme0n1r1 */
	if (opal_vbdev_name == NULL) {
		SPDK_ERRLOG("Could not allocate opal_vbdev_name\n");
		rc = -ENOMEM;
		goto err;
	}

	opal_bdev->name = opal_vbdev_name;
	/* [한국어] 12단계: SED Locking Range 메타데이터 설정.
	 * NVMe Security Send 명령으로 SED 펌웨어에 [range_start, +range_length) 등록. */
	rc = spdk_opal_cmd_setup_locking_range(opal_bdev->opal_dev, OPAL_ADMIN1,
					       opal_bdev->locking_range_id, opal_bdev->range_start,
					       opal_bdev->range_length, password);
	if (rc) {
		SPDK_ERRLOG("Error construct %s\n", opal_vbdev_name);
		goto err;
	}

	/* [한국어] 13단계: spdk_bdev_part_construct → 진짜 SPDK bdev로 등록.
	 * 인자 의미: part 객체, part_base, 이름, 시작 LBA, 크기 (블록), product_name. */
	rc = spdk_bdev_part_construct(part_bdev, opal_bdev->opal_base->part_base, opal_vbdev_name,
				      opal_bdev->range_start, opal_bdev->range_length, "Opal locking range");
	if (rc) {
		SPDK_ERRLOG("Could not allocate bdev part\n");
		goto err;
	}

	/* lock this bdev initially */
	/* [한국어] 14단계: 안전을 위해 초기 상태는 RWLOCK (read+write 잠금).
	 * 사용자가 set_lock_state로 명시적 unlock해야 IO 가능. */
	rc = spdk_opal_cmd_lock_unlock(opal_bdev->opal_dev, OPAL_ADMIN1, OPAL_RWLOCK, locking_range_id,
				       password);
	if (rc) {
		SPDK_ERRLOG("Error lock %s\n", opal_vbdev_name);
		goto err;
	}

	opal_bdev->bdev_part = part_bdev;   /* [한국어] 메타에 part_bdev 역참조 보관. */
	return 0;

err:
	/* [한국어] 에러 분기: 메타 정리 + part_bdev free. (part_base는 유지 - 이미 성공한 자원).
	 * 주의: spdk_bdev_part_construct 이후에 실패하면 spdk_bdev_part_free가 필요할 수도 있는데
	 * 현 구현은 단순 free만 한다. 호출자는 이를 인지하고 사용. */
	vbdev_opal_delete(opal_bdev);
	free(part_bdev);
	return rc;
}

/*
 * [한국어]
 * vbdev_opal_destruct_bdev - opal_vbdev 인스턴스의 part bdev를 unregister하고 메타 정리.
 *
 * range_start와 part 오프셋이 일치할 때만 unregister하는데, 이는 part_base가 공유되는
 * 환경에서 같은 메타 객체가 의도한 part를 가리키는지 sanity check.
 */
static void
vbdev_opal_destruct_bdev(struct opal_vbdev *opal_bdev)
{
	struct spdk_bdev_part *part = opal_bdev->bdev_part;

	assert(opal_bdev->opal_base != NULL);
	assert(part != NULL);

	/* [한국어] 일치 검증: 메타 range_start == part 실제 오프셋. */
	if (opal_bdev->range_start == spdk_bdev_part_get_offset_blocks(part)) {
		/* [한국어] spdk_bdev_unregister: 모든 채널/descriptor를 정리하고 fn_table::destruct 호출.
		 * cb=NULL이므로 비동기 완료를 기다리지 않음. */
		spdk_bdev_unregister(spdk_bdev_part_get_bdev(part), NULL, NULL);
	}
	vbdev_opal_delete(opal_bdev);
}

/*
 * [한국어]
 * vbdev_opal_destruct - vbdev_opal.h §2 참조. SED Secure Erase + Locking Range 초기화 + bdev unregister.
 *
 * 동작 단계:
 *   1) 이름으로 vbdev lookup.
 *   2) Secure Erase Locking Range: SED가 해당 범위의 데이터 키(MEK)를 재생성 → 기존 데이터 영구 삭제.
 *   3) Locking Range 메타데이터를 (start=0, length=0)으로 리셋.
 *   4) info 캐시 free.
 *   5) bdev unregister + 메타 정리.
 */
int
vbdev_opal_destruct(const char *bdev_name, const char *password)
{
	struct nvme_ctrlr *nvme_ctrlr;
	int locking_range_id;
	int rc;
	struct opal_vbdev *opal_bdev;

	/* [한국어] 1단계: 이름으로 lookup. */
	TAILQ_FOREACH(opal_bdev, &g_opal_vbdev, tailq) {
		if (strcmp(opal_bdev->name, bdev_name) == 0) {
			break;
		}
	}

	if (opal_bdev == NULL) {
		SPDK_ERRLOG("%s not found\n", bdev_name);
		rc = -ENODEV;
		goto err;
	}

	locking_range_id = opal_bdev->locking_range_id;

	/* [한국어] 2단계: 컨트롤러 유효성. */
	nvme_ctrlr = opal_bdev->nvme_ctrlr;
	if (nvme_ctrlr == NULL) {
		SPDK_ERRLOG("can't find nvme_ctrlr of %s\n", bdev_name);
		return -ENODEV;
	}

	/* secure erase locking range */
	/* [한국어] 3단계: Secure Erase. SED가 MEK를 재생성해 기존 데이터를 복호 불가능하게 만든다.
	 * Sanitize CryptoErase와 유사한 효과. 사용자 데이터 영구 손실. */
	rc = spdk_opal_cmd_secure_erase_locking_range(nvme_ctrlr->opal_dev, OPAL_ADMIN1, locking_range_id,
			password);
	if (rc) {
		SPDK_ERRLOG("opal erase locking range failed\n");
		goto err;
	}

	/* reset the locking range to 0 */
	/* [한국어] 4단계: Locking Range를 (0,0)로 리셋해 같은 ID를 재사용 가능 상태로. */
	rc = spdk_opal_cmd_setup_locking_range(nvme_ctrlr->opal_dev, OPAL_ADMIN1, locking_range_id, 0,
					       0, password);
	if (rc) {
		SPDK_ERRLOG("opal reset locking range failed\n");
		goto err;
	}

	/* [한국어] 5단계: lib/nvme/opal이 보관하던 info 캐시 해제. */
	spdk_opal_free_locking_range_info(opal_bdev->opal_dev, locking_range_id);
	/* [한국어] 6단계: bdev unregister + 메타 정리. */
	vbdev_opal_destruct_bdev(opal_bdev);
	return 0;

err:
	return rc;
}

/*
 * [한국어]
 * vbdev_opal_examine - bdev 모듈 examine 콜백. 새 베이스 bdev 등장 시 SPDK가 호출.
 *
 * 현재는 자동 detect 미구현 (TODO). examine_done만 호출해 다음 모듈로 진행 신호.
 * 호출자: SPDK bdev 코어가 새 bdev 등록 후 모든 모듈에 대해 순차 호출.
 */
static void
vbdev_opal_examine(struct spdk_bdev *bdev)
{
	/* TODO */
	spdk_bdev_module_examine_done(&opal_if);
}

/*
 * [한국어]
 * vbdev_opal_set_lock_state - vbdev_opal.h §2 참조. 잠금 상태 변경.
 *
 * lock_state 문자열 매핑:
 *   - "READWRITE": read+write 모두 허용 (잠금 해제).
 *   - "READONLY":  read만 허용, write 거부.
 *   - "RWLOCK":    read+write 모두 거부 (완전 잠금).
 * 입력 비교는 대소문자 무시 (strcasecmp).
 */
int
vbdev_opal_set_lock_state(const char *bdev_name, uint16_t user_id, const char *password,
			  const char *lock_state)
{
	struct nvme_ctrlr *nvme_ctrlr;
	int locking_range_id;
	int rc;
	enum spdk_opal_lock_state state_flag;
	struct opal_vbdev *opal_bdev;

	/* [한국어] 1단계: 이름으로 vbdev lookup. */
	TAILQ_FOREACH(opal_bdev, &g_opal_vbdev, tailq) {
		if (strcmp(opal_bdev->name, bdev_name) == 0) {
			break;
		}
	}

	if (opal_bdev == NULL) {
		SPDK_ERRLOG("%s not found\n", bdev_name);
		return -ENODEV;
	}

	/* [한국어] 2단계: 컨트롤러 검증. */
	nvme_ctrlr = opal_bdev->nvme_ctrlr;
	if (nvme_ctrlr == NULL) {
		SPDK_ERRLOG("can't find nvme_ctrlr of %s\n", opal_bdev->name);
		return -ENODEV;
	}

	/* [한국어] 3단계: 문자열 → enum 변환. */
	if (strcasecmp(lock_state, "READWRITE") == 0) {
		state_flag = OPAL_READWRITE;
	} else if (strcasecmp(lock_state, "READONLY") == 0) {
		state_flag = OPAL_READONLY;
	} else if (strcasecmp(lock_state, "RWLOCK") == 0) {
		state_flag = OPAL_RWLOCK;
	} else {
		SPDK_ERRLOG("Invalid OPAL lock state input\n");
		return -EINVAL;
	}

	/* [한국어] 4단계: SED에 잠금/해제 명령 발급 (user_id로 인증). */
	locking_range_id = opal_bdev->locking_range_id;
	rc = spdk_opal_cmd_lock_unlock(nvme_ctrlr->opal_dev, user_id, state_flag, locking_range_id,
				       password);
	if (rc) {
		SPDK_ERRLOG("%s lock/unlock failure: %d\n", bdev_name, rc);
	}

	return rc;
}

/*
 * [한국어]
 * vbdev_opal_enable_new_user - vbdev_opal.h §2 참조. 새 User 활성화 + 권한 부여.
 *
 * 동작 단계 (모두 Admin 인증으로 수행):
 *   1) Enable User: SED 내 User Authority 활성화.
 *   2) Set Password: 그 User의 신규 패스워드 설정 (true=admin이 user 비밀번호 설정).
 *   3) Add User to Locking Range (READONLY): User에게 read 권한.
 *   4) Add User to Locking Range (READWRITE): User에게 write 권한.
 * 한 단계라도 실패하면 즉시 반환 (이전 단계 롤백 없음 - 운영자가 수동 정리해야 할 수도).
 */
int
vbdev_opal_enable_new_user(const char *bdev_name, const char *admin_password, uint16_t user_id,
			   const char *user_password)
{
	struct nvme_ctrlr *nvme_ctrlr;
	int locking_range_id;
	int rc;
	struct opal_vbdev *opal_bdev;

	/* [한국어] 1단계: vbdev lookup. */
	TAILQ_FOREACH(opal_bdev, &g_opal_vbdev, tailq) {
		if (strcmp(opal_bdev->name, bdev_name) == 0) {
			break;
		}
	}

	if (opal_bdev == NULL) {
		SPDK_ERRLOG("%s not found\n", bdev_name);
		return -ENODEV;
	}

	/* [한국어] 2단계: 컨트롤러 검증. */
	nvme_ctrlr = opal_bdev->nvme_ctrlr;
	if (nvme_ctrlr == NULL) {
		SPDK_ERRLOG("can't find nvme_ctrlr of %s\n", opal_bdev->name);
		return -ENODEV;
	}

	/* [한국어] 3단계: User Authority enable. */
	rc = spdk_opal_cmd_enable_user(nvme_ctrlr->opal_dev, user_id, admin_password);
	if (rc) {
		SPDK_ERRLOG("%s enable user error: %d\n", bdev_name, rc);
		return rc;
	}

	/* [한국어] 4단계: 새 패스워드 설정. 마지막 인자 true = admin이 user 비밀번호를 설정 (위임 모드). */
	rc = spdk_opal_cmd_set_new_passwd(nvme_ctrlr->opal_dev, user_id, user_password, admin_password,
					  true);
	if (rc) {
		SPDK_ERRLOG("%s set user password error: %d\n", bdev_name, rc);
		return rc;
	}

	/* [한국어] 5단계: Locking Range ACL에 User 추가 (READONLY 권한). */
	locking_range_id = opal_bdev->locking_range_id;
	rc = spdk_opal_cmd_add_user_to_locking_range(nvme_ctrlr->opal_dev, user_id, locking_range_id,
			OPAL_READONLY, admin_password);
	if (rc) {
		SPDK_ERRLOG("%s add user READONLY priority error: %d\n", bdev_name, rc);
		return rc;
	}

	/* [한국어] 6단계: 같은 User에게 READWRITE 권한도 부여 (read+write 모두 가능). */
	rc = spdk_opal_cmd_add_user_to_locking_range(nvme_ctrlr->opal_dev, user_id, locking_range_id,
			OPAL_READWRITE, admin_password);
	if (rc) {
		SPDK_ERRLOG("%s add user READWRITE priority error: %d\n", bdev_name, rc);
		return rc;
	}

	return 0;
}

/* [한국어] SPDK 디버그 로그 컴포넌트 등록 ("vbdev_opal" 카테고리 활성화/비활성화 가능). */
SPDK_LOG_REGISTER_COMPONENT(vbdev_opal)
