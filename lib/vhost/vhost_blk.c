/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK vhost-blk(virtio-blk) target 구현 (vhost_blk.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 vhost-user 위에 동작하는 virtio-blk target의 구현이다.
 * 게스트(QEMU)가 보낸 virtio-blk 요청(read/write/flush/discard/write_zeroes/get_id)을
 * SPDK bdev I/O로 변환하여 처리한 뒤, used ring에 결과를 넣고 IRQ로 알린다.
 * split/packed virtqueue 양쪽을 모두 지원하며, vhost-user inflight 영역을 활용한
 * crash-recovery(reconnect 시 미완료 요청 재제출)도 처리한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (정상 I/O 경로):
 *   QEMU/게스트 → virtqueue avail ring → kickfd 트리거(또는 폴링)
 *     → vdev_worker / vdev_vq_worker (poller 콜백)
 *     → process_vq / process_packed_vq (큐 단위 처리)
 *     → process_blk_task / process_packed_blk_task (요청 단위)
 *     → blk_iovs_*_setup (desc 체인 → iov 변환)
 *     → virtio_blk_process_request (요청 타입 분기 → spdk_bdev_*v 호출)
 *     → bdev 백엔드 → I/O 완료 → blk_request_complete_cb
 *     → vhost_user_blk_request_finish → blk_task_enqueue → used ring + IRQ
 * 실행 컨텍스트: 디바이스의 SPDK thread (poller 또는 interrupt 콜백). single thread per session,
 * lockless 설계. ENOMEM 발생 시 spdk_bdev_queue_io_wait로 재시도.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk_bdev API (bdev_readv/writev/unmap/flush/write_zeroes 등),
 *   vhost_internal.h (virtqueue 헬퍼/세션 자료구조), <linux/virtio_blk.h> (virtio-blk 스펙).
 * - 등록: 이 파일은 transport "vhost_user_blk"의 ops를 SPDK_VIRTIO_BLK_TRANSPORT_REGISTER로 등록.
 * - RPC 진입점: spdk_vhost_blk_construct (vhost_create_blk_controller RPC).
 * - 데이터 흐름: 게스트 메모리(GPA) → vhost_gpa_to_vva → 호스트 IOV → bdev I/O.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_vhost_blk_dev: 컨트롤러 단위 상태 (bdev_desc, transport ops, readonly).
 * - struct spdk_vhost_blk_session: 세션 단위 상태 (poller, io_channel).
 * - struct spdk_vhost_user_blk_task: virtio-blk 요청 한 건 (blk_task + 큐/인덱스 메타).
 * - virtio_blk_process_request: 요청 타입 디스패처 (READ/WRITE/FLUSH/...).
 * - process_vq / process_packed_vq: 큐 폴링 루프.
 * - vhost_blk_start / vhost_blk_stop: 세션 시작/종료 콜백.
 * - alloc_vq_task_pool: 큐별 task 풀 할당.
 * - spdk_vhost_blk_construct: RPC가 호출하는 컨트롤러 생성 진입점.
 * - vhost_user_blk transport ops + SPDK_VIRTIO_BLK_TRANSPORT_REGISTER: 자동 등록.
 */

#include <linux/virtio_blk.h>
/* [한국어] virtio-blk 스펙 정의 (VIRTIO_BLK_T_IN/OUT/FLUSH/DISCARD, virtio_blk_outhdr 등). */

#include "spdk/env.h"
/* [한국어] spdk_zmalloc/spdk_free (DMA 가능 hugepage 메모리 할당). */
#include "spdk/bdev.h"
/* [한국어] spdk_bdev_readv/writev/flush/unmap/write_zeroes 등 bdev I/O API. */
#include "spdk/bdev_module.h"
/* [한국어] bdev module 측 헬퍼 — io_channel/desc 구조 접근. */
#include "spdk/thread.h"
/* [한국어] spdk_poller/spdk_thread/spdk_interrupt API. */
#include "spdk/likely.h"
/* [한국어] spdk_likely/spdk_unlikely (분기 예측 힌트). */
#include "spdk/string.h"
/* [한국어] spdk_strcpy_pad 등 — VIRTIO_BLK_T_GET_ID에서 사용. */
#include "spdk/util.h"
/* [한국어] SPDK_COUNTOF, SPDK_CONTAINEROF 매크로. */
#include "spdk/vhost.h"
/* [한국어] vhost 공개 API. */
#include "spdk/json.h"
/* [한국어] JSON 디코더 — RPC 인자 파싱. */

#include "vhost_internal.h"
/* [한국어] vhost 내부 자료구조/헬퍼 (vsession, virtqueue 헬퍼 등). */
#include <rte_version.h>
/* [한국어] DPDK 버전 매크로 — RTE_VERSION_NUM 비교로 API 호환 분기. */

/* Minimal set of features supported by every SPDK VHOST-BLK device */
#define SPDK_VHOST_BLK_FEATURES_BASE (SPDK_VHOST_FEATURES | \
		(1ULL << VIRTIO_BLK_F_SIZE_MAX) | (1ULL << VIRTIO_BLK_F_SEG_MAX) | \
		(1ULL << VIRTIO_BLK_F_GEOMETRY) | (1ULL << VIRTIO_BLK_F_BLK_SIZE) | \
		(1ULL << VIRTIO_BLK_F_TOPOLOGY) | (1ULL << VIRTIO_BLK_F_BARRIER)  | \
		(1ULL << VIRTIO_BLK_F_SCSI)     | (1ULL << VIRTIO_BLK_F_CONFIG_WCE) | \
		(1ULL << VIRTIO_BLK_F_MQ))
/* [한국어] vhost-blk가 광고하는 기본 feature 비트맵.
 *  - SPDK_VHOST_FEATURES: 공통 vhost feature.
 *  - VIRTIO_BLK_F_SIZE_MAX/SEG_MAX: 게스트가 알 수 있도록 최대 segment 크기/수 광고.
 *  - VIRTIO_BLK_F_BLK_SIZE: 블록 크기 노출.
 *  - VIRTIO_BLK_F_TOPOLOGY: 디스크 토폴로지(physical/optical block 등).
 *  - VIRTIO_BLK_F_MQ: multi-queue 지원.
 *  - GEOMETRY/BARRIER/SCSI/CONFIG_WCE는 협상 가능하지만 아래 DISABLED에서 끔. */

/* Not supported features */
#define SPDK_VHOST_BLK_DISABLED_FEATURES (SPDK_VHOST_DISABLED_FEATURES | \
		(1ULL << VIRTIO_BLK_F_GEOMETRY) | (1ULL << VIRTIO_BLK_F_CONFIG_WCE) | \
		(1ULL << VIRTIO_BLK_F_BARRIER)  | (1ULL << VIRTIO_BLK_F_SCSI))
/* [한국어] 협상에서 명시적으로 끄는 feature. SPDK는 SCSI passthrough/WCE 등을 지원하지 않음. */

/* Vhost-blk support protocol features */
#define SPDK_VHOST_BLK_PROTOCOL_FEATURES ((1ULL << VHOST_USER_PROTOCOL_F_CONFIG) | \
		(1ULL << VHOST_USER_PROTOCOL_F_INFLIGHT_SHMFD))
/* [한국어] vhost-user 확장 프로토콜 feature.
 *  - F_CONFIG: 게스트가 device-specific config 영역 read/write 가능.
 *  - F_INFLIGHT_SHMFD: inflight 메타를 공유 메모리에 저장 → reconnect 시 미완료 요청 복구. */

#define VIRTIO_BLK_DEFAULT_TRANSPORT "vhost_user_blk"
/* [한국어] vhost_create_blk_controller RPC에서 transport 미지정 시 사용할 기본 transport 이름. */

struct spdk_vhost_user_blk_task {
	struct spdk_vhost_blk_task blk_task;
	/* [한국어] 백엔드 공통 task 구조(헤더 정의) — 이 구조체의 첫 필드여야 컨테이너 캐스팅 안전.
	 * 설정자: blk_task_init이 iovcnt/status/used_len/payload_size 초기화.
	 * 읽는 자: virtio_blk_process_request가 iovs/payload_size 사용. */
	struct spdk_vhost_blk_session *bvsession;
	/* [한국어] 이 task가 속한 vhost-blk 세션 — 큐 풀 할당 시 설정.
	 * 읽는 자: blk_task_inc/dec_task_cnt 등 세션 단위 작업. */
	struct spdk_vhost_virtqueue *vq;
	/* [한국어] 이 task가 속한 virtqueue — 완료 시 used ring/packed enqueue 대상. */

	uint16_t req_idx;
	/* [한국어] 게스트 desc table에서 이 요청이 시작되는 인덱스(또는 packed ring의 ring 인덱스). */
	uint16_t num_descs;
	/* [한국어] desc chain의 길이(packed ring에서 사용). */
	uint16_t buffer_id;
	/* [한국어] packed ring에서 used flag와 함께 게스트로 돌려줄 buffer_id. */
	uint16_t inflight_head;
	/* [한국어] inflight 영역에서 이 task의 head 인덱스 — 완료 시 정리에 사용. */

	/* If set, the task is currently used for I/O processing. */
	bool used;
	/* [한국어] true면 이 task가 현재 처리 중 — 풀에서 같은 슬롯 재사용 방지.
	 * 설정자: blk_task_init = true, blk_task_finish = false.
	 * 읽는 자: process_blk_task 진입 시 검사 (이미 사용 중이면 즉시 enqueue로 fail). */
};

struct spdk_vhost_blk_dev {
	struct spdk_vhost_dev vdev;
	/* [한국어] 부모 디바이스 공통 구조 — 이 구조체의 첫 필드. SPDK_CONTAINEROF로 변환. */
	struct spdk_bdev *bdev;
	/* [한국어] 백엔드 bdev 포인터(read-only 속성 접근용). bdev_desc로부터 spdk_bdev_desc_get_bdev로 획득.
	 * NULL이면 hot-remove된 상태 — no_bdev_* 워커가 대체로 동작. */
	struct spdk_bdev_desc *bdev_desc;
	/* [한국어] bdev open descriptor — spdk_bdev_open_ext로 획득.
	 * I/O 시 spdk_bdev_*v 함수의 첫 인자로 전달. */
	const struct spdk_virtio_blk_transport_ops *ops;
	/* [한국어] transport별 콜백 (vhost_user_blk 또는 vfio_user_blk 등). */

	bool readonly;
	/* [한국어] true면 write/discard/write_zeroes 거부 — 게스트엔 VIRTIO_BLK_F_RO도 광고. */
};

struct spdk_vhost_blk_session {
	/* The parent session must be the very first field in this struct */
	struct spdk_vhost_session vsession;
	/* [한국어] 부모 세션 공통 구조 — 첫 필드여야 to_blk_session 캐스팅 안전.
	 * vsession_ctx_size = sizeof(blk_session) - sizeof(vsession)으로 백엔드 컨텍스트 크기 계산. */
	struct spdk_vhost_blk_dev *bvdev;
	/* [한국어] 부모 디바이스 포인터 — 세션 시작 시 캐싱. */
	struct spdk_poller *requestq_poller;
	/* [한국어] virtqueue 폴링용 SPDK poller — vdev_worker/no_bdev_vdev_worker를 무한 호출.
	 * 0us 주기 = 매 reactor 사이클 호출 (busy poll).
	 * 설정자: vhost_blk_start. 해제: vhost_blk_stop. */
	struct spdk_io_channel *io_channel;
	/* [한국어] 이 세션의 bdev I/O channel — 세션 thread에 묶인 채널.
	 * 설정자: vhost_blk_start (vhost_blk_get_io_channel). 해제: stop poller. */
	struct spdk_poller *stop_poller;
	/* [한국어] 종료 시퀀스 poller — task_cnt가 0이 될 때까지 기다리며 자원 정리. */
};

/* forward declaration */
static const struct spdk_vhost_dev_backend vhost_blk_device_backend;
/* [한국어] forward declaration — 아래에서 참조되는 백엔드 vtable. 정의는 파일 후반부. */

static void vhost_user_blk_request_finish(uint8_t status, struct spdk_vhost_blk_task *task,
		void *cb_arg);
/* [한국어] forward declaration — vhost-user 경로의 task 완료 콜백. */

/*
 * [한국어]
 * vhost_user_process_blk_request - vhost-user task를 공통 process_request로 위임하는 래퍼.
 *
 * @user_task: vhost-user 전용 task (blk_task와 큐 메타 포함).
 * @return: virtio_blk_process_request의 반환값 (0: 정상, -1: 실패).
 *
 * 공통 virtio_blk_process_request에 백엔드 io_channel, vdev, 완료 콜백을 결합해 호출.
 * 호출 컨텍스트: 디바이스 thread (poller 내부).
 *
 * 호출 체인: process_blk_task → vhost_user_process_blk_request → virtio_blk_process_request.
 */
static int
vhost_user_process_blk_request(struct spdk_vhost_user_blk_task *user_task)
{
	struct spdk_vhost_blk_session *bvsession = user_task->bvsession;
	/* [한국어] 세션 포인터 — io_channel 추출용. */
	struct spdk_vhost_dev *vdev = &bvsession->bvdev->vdev;
	/* [한국어] 디바이스 공통 구조 포인터 — process_request의 첫 인자. */

	return virtio_blk_process_request(vdev, bvsession->io_channel, &user_task->blk_task,
					  vhost_user_blk_request_finish, NULL);
	/* [한국어] 공통 처리 함수 호출. cb_arg=NULL — 완료 콜백은 task만으로 동작. */
}

/*
 * [한국어]
 * to_blk_dev - 디바이스 공통 vdev → vhost-blk dev 변환 헬퍼.
 *
 * @vdev: 공통 디바이스 구조 (NULL 허용).
 * @return: blk dev 포인터, 잘못된 백엔드면 NULL.
 *
 * vhost backend type이 BLK인지 검사 후 SPDK_CONTAINEROF로 캐스팅.
 * vdev가 scsi이거나 NULL이면 NULL 반환.
 */
static struct spdk_vhost_blk_dev *
to_blk_dev(struct spdk_vhost_dev *vdev)
{
	if (vdev == NULL) {
		/* [한국어] NULL 보호. */
		return NULL;
	}

	if (vdev->backend->type != VHOST_BACKEND_BLK) {
		/* [한국어] scsi 등 다른 백엔드면 에러 — 잘못된 변환 시도. */
		SPDK_ERRLOG("%s: not a vhost-blk device\n", vdev->name);
		return NULL;
	}

	return SPDK_CONTAINEROF(vdev, struct spdk_vhost_blk_dev, vdev);
	/* [한국어] vdev 필드의 오프셋을 빼서 부모 spdk_vhost_blk_dev 포인터 복원. */
}

/*
 * [한국어]
 * vhost_blk_get_bdev - 외부에서 vhost-blk 컨트롤러의 bdev 포인터 조회 (공개 API 구현).
 *
 * @vdev: blk 디바이스.
 * @return: bdev 포인터 (NULL 가능 — hot-remove된 경우).
 *
 * 이 포인터는 read-only 속성 조회만 허용 — 직접 I/O 발급은 bvdev->bdev_desc 사용.
 */
struct spdk_bdev *
vhost_blk_get_bdev(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);
	/* [한국어] blk dev 변환. */

	assert(bvdev != NULL);
	/* [한국어] 호출자가 blk vdev를 보장해야 함 — 디버그 빌드에서 즉시 abort. */

	return bvdev->bdev;
	/* [한국어] bdev 포인터 반환 (NULL일 수도 있음). */
}

/*
 * [한국어]
 * to_blk_session - 공통 vsession → vhost-blk 세션 변환.
 *
 * @vsession: 공통 세션.
 * @return: blk 세션 포인터.
 *
 * spdk_vhost_blk_session은 vsession을 첫 필드로 가지므로 단순 캐스팅으로 변환 가능.
 */
static struct spdk_vhost_blk_session *
to_blk_session(struct spdk_vhost_session *vsession)
{
	assert(vsession->vdev->backend->type == VHOST_BACKEND_BLK);
	/* [한국어] 백엔드 타입 검증 — scsi 세션을 잘못 캐스팅하지 못하도록. */
	return (struct spdk_vhost_blk_session *)vsession;
	/* [한국어] 첫 필드 일치 보장하에 직접 캐스팅. */
}

/*
 * [한국어]
 * blk_task_inc_task_cnt - 진행 중 task 카운터 증가 (인라인).
 *
 * stop poller가 task_cnt==0을 기다리므로 정확한 카운팅이 중요.
 */
static inline void
blk_task_inc_task_cnt(struct spdk_vhost_user_blk_task *task)
{
	task->bvsession->vsession.task_cnt++;
	/* [한국어] 세션 task_cnt +1. single thread이므로 atomic 불필요. */
}

/*
 * [한국어]
 * blk_task_dec_task_cnt - 진행 중 task 카운터 감소 (인라인).
 */
static inline void
blk_task_dec_task_cnt(struct spdk_vhost_user_blk_task *task)
{
	assert(task->bvsession->vsession.task_cnt > 0);
	/* [한국어] 음수 진입 방지 — 디버그 빌드 sanity check. */
	task->bvsession->vsession.task_cnt--;
	/* [한국어] 세션 task_cnt -1. */
}

/*
 * [한국어]
 * blk_task_finish - task 처리 완료 후 자원 반납.
 *
 * 카운터 감소 + used=false로 풀에 반환.
 * 호출 컨텍스트: 완료 콜백 (디바이스 thread).
 */
static void
blk_task_finish(struct spdk_vhost_user_blk_task *task)
{
	blk_task_dec_task_cnt(task);
	/* [한국어] 카운터 감소. */
	task->used = false;
	/* [한국어] 슬롯 사용 가능 표시 — 다음 같은 인덱스 요청이 진입할 수 있음. */
}

/*
 * [한국어]
 * blk_task_init - 새 요청 처리 시작 시 task 필드 초기화.
 *
 * iovcnt를 최대값으로 설정해 두어 setup 함수가 채워 넣을 수 있게 한다.
 */
static void
blk_task_init(struct spdk_vhost_user_blk_task *task)
{
	struct spdk_vhost_blk_task *blk_task = &task->blk_task;
	/* [한국어] 내부 blk_task 추출. */

	task->used = true;
	/* [한국어] 슬롯 사용 중 표시 — 동일 슬롯 재진입 차단. */
	blk_task->iovcnt = SPDK_COUNTOF(blk_task->iovs);
	/* [한국어] 최대 iov 슬롯 수로 초기화 — setup이 실제 사용 수로 줄임. */
	blk_task->status = NULL;
	/* [한국어] 응답 status 바이트 포인터 초기화 — 게스트 desc에서 추후 설정. */
	blk_task->used_len = 0;
	/* [한국어] used ring에 보고할 길이 0으로 초기화. */
	blk_task->payload_size = 0;
	/* [한국어] payload 크기 초기화. */
}

/*
 * [한국어]
 * blk_task_enqueue - 완료된 task를 used ring(또는 packed ring)에 enqueue.
 *
 * split/packed 분기 — split은 (req_idx, used_len), packed는 (buffer_id, num_descs, used_len, inflight_head).
 * 호출 컨텍스트: 완료 콜백.
 */
static void
blk_task_enqueue(struct spdk_vhost_user_blk_task *task)
{
	if (task->vq->packed.packed_ring) {
		/* [한국어] packed ring: buffer_id 기반 enqueue + inflight 정리. */
		vhost_vq_packed_ring_enqueue(&task->bvsession->vsession, task->vq,
					     task->num_descs,
					     task->buffer_id, task->blk_task.used_len,
					     task->inflight_head);
	} else {
		/* [한국어] split ring: head desc 인덱스 + used_len. */
		vhost_vq_used_ring_enqueue(&task->bvsession->vsession, task->vq,
					   task->req_idx, task->blk_task.used_len);
	}
}

/*
 * [한국어]
 * vhost_user_blk_request_finish - vhost-user 경로의 task 완료 콜백.
 *
 * @status: VIRTIO_BLK_S_OK/IOERR/UNSUPP.
 * @task: 백엔드 공통 task 포인터.
 * @cb_arg: 사용 안 함 (process_request에 NULL 전달됨).
 *
 * blk_task → user_blk_task로 캐스팅 후 used ring enqueue + 카운터/슬롯 정리.
 * IRQ는 폴러 루프 끝의 vhost_session_vq_used_signal에서 일괄 발사 — 여기선 ring만 채움.
 */
static void
vhost_user_blk_request_finish(uint8_t status, struct spdk_vhost_blk_task *task, void *cb_arg)
{
	struct spdk_vhost_user_blk_task *user_task;
	/* [한국어] user_blk_task 포인터 — 컨테이너 패턴 복원. */

	user_task = SPDK_CONTAINEROF(task, struct spdk_vhost_user_blk_task, blk_task);
	/* [한국어] blk_task 오프셋을 빼서 부모 user_blk_task 포인터 획득. */

	blk_task_enqueue(user_task);
	/* [한국어] used ring/packed ring에 결과 enqueue. */

	SPDK_DEBUGLOG(vhost_blk, "Finished task (%p) req_idx=%d\n status: %" PRIu8"\n",
		      user_task, user_task->req_idx, status);
	/* [한국어] 디버그 로그. */
	blk_task_finish(user_task);
	/* [한국어] task 슬롯 반환 + 카운터 감소. */
}

/*
 * [한국어]
 * blk_request_finish - 요청 처리 결과를 task->status에 기록 후 콜백 호출.
 *
 * @status: VIRTIO_BLK_S_*.
 * @task: 공통 blk task.
 *
 * status 바이트(게스트 메모리)에 결과 코드를 직접 쓰고, 등록된 cb를 호출.
 * cb는 vhost-user 경로에서는 vhost_user_blk_request_finish.
 */
static void
blk_request_finish(uint8_t status, struct spdk_vhost_blk_task *task)
{

	if (task->status) {
		/* [한국어] virtio-blk 응답 바이트가 매핑돼 있으면 게스트 메모리에 status 기록.
		 * volatile 포인터 — 컴파일러 캐싱 방지. */
		*task->status = status;
	}

	task->cb(status, task, task->cb_arg);
	/* [한국어] 등록된 완료 콜백 호출 — used ring enqueue 등을 수행. */
}

/*
 * Process task's descriptor chain and setup data related fields.
 * Return
 *   total size of supplied buffers
 *
 *   FIXME: Make this function return to rd_cnt and wr_cnt
 */
/*
 * [한국어]
 * blk_iovs_split_queue_setup - split virtqueue desc chain을 호스트 IOV 배열로 변환.
 *
 * @bvsession: blk 세션.
 * @vq: 대상 virtqueue.
 * @req_idx: head descriptor 인덱스.
 * @iovs: 출력 iov 배열 (호출자 제공).
 * @iovs_cnt: in: 배열 크기, out: 채워진 iov 수.
 * @length: 출력 — 전체 페이로드 길이(바이트).
 * @return: 0 성공, -1 실패 (잘못된 desc, 사이클, 최소 desc 수 미달 등).
 *
 * virtio split ring의 desc chain을 따라가면서 각 desc를 vhost_vring_desc_to_iov로
 * 호스트 IOV(GPA→VVA 변환)로 풀어준다. 무한 사이클 방어를 위해 desc_handled_cnt를
 * desc_table_size와 비교하며 순회한다.
 *
 * 반드시 2개 이상의 desc가 있어야 함:
 *   - 첫 desc: virtio_blk_outhdr (read-only) — 요청 헤더.
 *   - 마지막 desc: status 바이트 (write-able) — 응답.
 *   - 중간: 페이로드 (READ면 write-able, WRITE면 read-only).
 *
 * 호출 컨텍스트: 디바이스 thread (process_blk_task에서).
 */
static int
blk_iovs_split_queue_setup(struct spdk_vhost_blk_session *bvsession,
			   struct spdk_vhost_virtqueue *vq,
			   uint16_t req_idx, struct iovec *iovs, uint16_t *iovs_cnt, uint32_t *length)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	/* [한국어] 공통 세션 추출. */
	struct spdk_vhost_dev *vdev = vsession->vdev;
	/* [한국어] 디바이스 — 에러 로그용. */
	struct vring_desc *desc, *desc_table;
	/* [한국어] 현재 desc 포인터, 그리고 desc 테이블(직접/indirect). */
	uint16_t out_cnt = 0, cnt = 0;
	/* [한국어] out_cnt: write-able desc 개수, cnt: 채운 iov 수. */
	uint32_t desc_table_size, len = 0;
	/* [한국어] desc_table 크기, 누적 페이로드 길이. */
	uint32_t desc_handled_cnt;
	/* [한국어] 처리한 desc 수 — 사이클 검출용. */
	int rc;
	/* [한국어] 결과 코드. */

	rc = vhost_vq_get_desc(vsession, vq, req_idx, &desc, &desc_table, &desc_table_size);
	/* [한국어] req_idx에서 첫 desc + desc_table 획득. INDIRECT 처리 포함. */
	if (rc != 0) {
		SPDK_ERRLOG("%s: invalid descriptor at index %"PRIu16".\n", vdev->name, req_idx);
		/* [한국어] 잘못된 인덱스 — 게스트 측 버그 또는 손상된 큐. */
		return -1;
	}

	desc_handled_cnt = 0;
	/* [한국어] 사이클 카운터 초기화. */
	while (1) {
		/*
		 * Maximum cnt reached?
		 * Should not happen if request is well formatted, otherwise this is a BUG.
		 */
		if (spdk_unlikely(cnt == *iovs_cnt)) {
			/* [한국어] iov 배열을 다 채웠는데 desc가 더 있다면 SPDK_VHOST_IOVS_MAX 초과 — 비정상.
			 * spdk_unlikely: 일반 정상 경로가 아님을 컴파일러에 힌트. */
			SPDK_DEBUGLOG(vhost_blk, "%s: max IOVs in request reached (req_idx = %"PRIu16").\n",
				      vsession->name, req_idx);
			return -1;
		}

		if (spdk_unlikely(vhost_vring_desc_to_iov(vsession, iovs, &cnt, desc))) {
			/* [한국어] desc → iov 변환 실패 (GPA 변환 실패 또는 영역 경계 초과 등). */
			SPDK_DEBUGLOG(vhost_blk, "%s: invalid descriptor %" PRIu16" (req_idx = %"PRIu16").\n",
				      vsession->name, req_idx, cnt);
			return -1;
		}

		len += desc->len;
		/* [한국어] 페이로드 길이 누적. */

		out_cnt += vhost_vring_desc_is_wr(desc);
		/* [한국어] write-able desc 개수 +1 (false면 +0). */

		rc = vhost_vring_desc_get_next(&desc, desc_table, desc_table_size);
		/* [한국어] 다음 desc로 이동. desc=NULL이면 chain 끝. */
		if (rc != 0) {
			/* [한국어] NEXT 비트는 있으나 다음 인덱스가 잘못된 경우. */
			SPDK_ERRLOG("%s: descriptor chain at index %"PRIu16" terminated unexpectedly.\n",
				    vsession->name, req_idx);
			return -1;
		} else if (desc == NULL) {
			/* [한국어] chain 끝 — 정상 종료. */
			break;
		}

		desc_handled_cnt++;
		/* [한국어] 처리한 desc 수 증가. */
		if (spdk_unlikely(desc_handled_cnt > desc_table_size)) {
			/* Break a cycle and report an error, if any. */
			/* [한국어] desc table 크기 초과 = NEXT 사이클 — 손상된 desc table. */
			SPDK_ERRLOG("%s: found a cycle in the descriptor chain: desc_table_size = %d, desc_handled_cnt = %d.\n",
				    vsession->name, desc_table_size, desc_handled_cnt);
			return -1;
		}
	}

	/*
	 * There must be least two descriptors.
	 * First contain request so it must be readable.
	 * Last descriptor contain buffer for response so it must be writable.
	 */
	if (spdk_unlikely(out_cnt == 0 || cnt < 2)) {
		/* [한국어] write-able desc가 1개도 없으면 status를 쓸 곳이 없음 — 잘못된 요청.
		 * cnt<2면 헤더만 있고 status가 없음 — 잘못된 요청. */
		return -1;
	}

	*length = len;
	/* [한국어] 출력 페이로드 길이. */
	*iovs_cnt = cnt;
	/* [한국어] 출력 채운 iov 수. */
	return 0;
}

/*
 * [한국어]
 * blk_iovs_packed_desc_setup - packed virtqueue desc chain (또는 indirect table)을 IOV로 변환.
 *
 * @vsession: vhost 세션.
 * @vq: 대상 virtqueue.
 * @req_idx: head 인덱스 (indirect 시 의미 없음).
 * @desc_table: indirect 테이블 포인터 (NULL이면 vq 본 테이블 사용).
 * @desc_table_size: desc 테이블 크기.
 * @iovs: 출력 iov 배열.
 * @iovs_cnt: in/out 카운트.
 * @length: 출력 길이.
 * @return: 0 성공, -EINVAL 실패.
 *
 * packed ring은 split과 달리 desc.flags의 NEXT/AVAIL/USED 비트로 chain을 따라간다.
 * indirect 모드면 desc_table[0]부터 시작, 아니면 vq->vring.desc_packed[req_idx]부터.
 */
static int
blk_iovs_packed_desc_setup(struct spdk_vhost_session *vsession,
			   struct spdk_vhost_virtqueue *vq, uint16_t req_idx,
			   struct vring_packed_desc *desc_table, uint16_t desc_table_size,
			   struct iovec *iovs, uint16_t *iovs_cnt, uint32_t *length)
{
	struct vring_packed_desc *desc;
	/* [한국어] 현재 desc 포인터. */
	uint16_t cnt = 0, out_cnt = 0;
	/* [한국어] 채운 iov 수, write-able desc 수. */
	uint32_t len = 0;
	/* [한국어] 누적 페이로드 길이. */

	if (desc_table == NULL) {
		/* [한국어] 본 테이블 사용 — vq의 packed desc 테이블에서 req_idx로 시작. */
		desc = &vq->vring.desc_packed[req_idx];
	} else {
		/* [한국어] indirect 테이블 사용 — 0번부터 시작. */
		req_idx = 0;
		desc = desc_table;
	}

	while (1) {
		/*
		 * Maximum cnt reached?
		 * Should not happen if request is well formatted, otherwise this is a BUG.
		 */
		if (spdk_unlikely(cnt == *iovs_cnt)) {
			/* [한국어] iov 배열 초과. */
			SPDK_ERRLOG("%s: max IOVs in request reached (req_idx = %"PRIu16").\n",
				    vsession->name, req_idx);
			return -EINVAL;
		}

		if (spdk_unlikely(vhost_vring_packed_desc_to_iov(vsession, iovs, &cnt, desc))) {
			/* [한국어] packed desc → iov 변환 실패. */
			SPDK_ERRLOG("%s: invalid descriptor %" PRIu16" (req_idx = %"PRIu16").\n",
				    vsession->name, req_idx, cnt);
			return -EINVAL;
		}

		len += desc->len;
		/* [한국어] 페이로드 길이 누적. */
		out_cnt += vhost_vring_packed_desc_is_wr(desc);
		/* [한국어] write-able desc 카운트. */

		/* desc is NULL means we reach the last desc of this request */
		vhost_vring_packed_desc_get_next(&desc, &req_idx, vq, desc_table, desc_table_size);
		/* [한국어] 다음 desc로 이동. NEXT 없으면 desc=NULL로 만든다. */
		if (desc == NULL) {
			/* [한국어] chain 끝. */
			break;
		}
	}

	/*
	 * There must be least two descriptors.
	 * First contain request so it must be readable.
	 * Last descriptor contain buffer for response so it must be writable.
	 */
	if (spdk_unlikely(out_cnt == 0 || cnt < 2)) {
		/* [한국어] split과 동일 — 최소 2개 desc + 1개 이상 write-able 필요. */
		return -EINVAL;
	}

	*length = len;
	/* [한국어] 출력. */
	*iovs_cnt = cnt;
	/* [한국어] 출력. */

	return 0;
}

/*
 * [한국어]
 * blk_iovs_packed_queue_setup - packed virtqueue에서 head desc를 가져와 setup으로 위임.
 *
 * vhost_vq_get_desc_packed로 desc + desc_table을 얻은 뒤 blk_iovs_packed_desc_setup 호출.
 */
static int
blk_iovs_packed_queue_setup(struct spdk_vhost_blk_session *bvsession,
			    struct spdk_vhost_virtqueue *vq, uint16_t req_idx,
			    struct iovec *iovs, uint16_t *iovs_cnt, uint32_t *length)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	/* [한국어] 공통 세션. */
	struct spdk_vhost_dev *vdev = vsession->vdev;
	/* [한국어] 에러 로그용 디바이스. */
	struct vring_packed_desc *desc = NULL, *desc_table;
	/* [한국어] 출력 desc + desc_table. */
	uint32_t desc_table_size;
	/* [한국어] desc table 크기. */
	int rc;
	/* [한국어] 결과. */

	rc = vhost_vq_get_desc_packed(vsession, vq, req_idx, &desc,
				      &desc_table, &desc_table_size);
	/* [한국어] head desc 획득. INDIRECT면 desc_table이 게스트 indirect 테이블, 아니면 NULL. */
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("%s: Invalid descriptor at index %"PRIu16".\n", vdev->name, req_idx);
		return rc;
	}

	return blk_iovs_packed_desc_setup(vsession, vq, req_idx, desc_table, desc_table_size,
					  iovs, iovs_cnt, length);
	/* [한국어] 실제 chain 처리는 별도 함수에 위임 — inflight 경로와 코드 공유. */
}

/*
 * [한국어]
 * blk_iovs_inflight_queue_setup - inflight 영역의 desc에서 IOV 변환 (재제출 경로).
 *
 * vhost-user reconnect 후 미완료 요청 복구 시 호출. inflight_desc에 indirect_addr가 있으면
 * 게스트 indirect 테이블로 위임, 아니면 inflight 영역 내 next 체인을 직접 따라간다.
 */
static int
blk_iovs_inflight_queue_setup(struct spdk_vhost_blk_session *bvsession,
			      struct spdk_vhost_virtqueue *vq, uint16_t req_idx,
			      struct iovec *iovs, uint16_t *iovs_cnt, uint32_t *length)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	/* [한국어] 공통 세션. */
	struct spdk_vhost_dev *vdev = vsession->vdev;
	/* [한국어] 에러 로그용. */
	spdk_vhost_inflight_desc *inflight_desc;
	/* [한국어] 현재 inflight desc. */
	struct vring_packed_desc *desc_table;
	/* [한국어] indirect 테이블 (있으면). */
	uint16_t out_cnt = 0, cnt = 0;
	/* [한국어] 카운터. */
	uint32_t desc_table_size, len = 0;
	/* [한국어] 테이블 크기, 누적 길이. */
	int rc = 0;
	/* [한국어] 결과. */

	rc = vhost_inflight_queue_get_desc(vsession, vq->vring_inflight.inflight_packed->desc,
					   req_idx, &inflight_desc, &desc_table, &desc_table_size);
	/* [한국어] inflight 영역에서 desc 획득. INDIRECT면 desc_table 채워줌. */
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("%s: Invalid descriptor at index %"PRIu16".\n", vdev->name, req_idx);
		return rc;
	}

	if (desc_table != NULL) {
		/* [한국어] indirect 테이블이 있다면 packed desc 경로로 위임. */
		return blk_iovs_packed_desc_setup(vsession, vq, req_idx, desc_table, desc_table_size,
						  iovs, iovs_cnt, length);
	}

	while (1) {
		/*
		 * Maximum cnt reached?
		 * Should not happen if request is well formatted, otherwise this is a BUG.
		 */
		if (spdk_unlikely(cnt == *iovs_cnt)) {
			/* [한국어] iov 초과. */
			SPDK_ERRLOG("%s: max IOVs in request reached (req_idx = %"PRIu16").\n",
				    vsession->name, req_idx);
			return -EINVAL;
		}

		if (spdk_unlikely(vhost_vring_inflight_desc_to_iov(vsession, iovs, &cnt, inflight_desc))) {
			/* [한국어] inflight desc → iov 변환 실패. */
			SPDK_ERRLOG("%s: invalid descriptor %" PRIu16" (req_idx = %"PRIu16").\n",
				    vsession->name, req_idx, cnt);
			return -EINVAL;
		}

		len += inflight_desc->len;
		/* [한국어] 길이 누적. */
		out_cnt += vhost_vring_inflight_desc_is_wr(inflight_desc);
		/* [한국어] write-able 카운트. */

		/* Without F_NEXT means it's the last desc */
		if ((inflight_desc->flags & VRING_DESC_F_NEXT) == 0) {
			/* [한국어] NEXT 비트 없음 = chain 끝. */
			break;
		}

		inflight_desc = &vq->vring_inflight.inflight_packed->desc[inflight_desc->next];
		/* [한국어] inflight 배열에서 next 인덱스로 이동. */
	}

	/*
	 * There must be least two descriptors.
	 * First contain request so it must be readable.
	 * Last descriptor contain buffer for response so it must be writable.
	 */
	if (spdk_unlikely(out_cnt == 0 || cnt < 2)) {
		/* [한국어] 최소 desc 검증. */
		return -EINVAL;
	}

	*length = len;
	/* [한국어] 출력. */
	*iovs_cnt = cnt;
	/* [한국어] 출력. */

	return 0;
}

/*
 * [한국어]
 * blk_request_complete_cb - bdev I/O 완료 시 호출되는 SPDK bdev 콜백.
 *
 * @bdev_io: 완료된 bdev I/O.
 * @success: I/O 성공 여부.
 * @cb_arg: spdk_vhost_blk_task 포인터.
 *
 * spdk_bdev_*v 호출 시 등록한 콜백 — bdev 모듈이 I/O를 끝내면 디바이스 thread에서 호출.
 * bdev_io 자원 반납 후 blk_request_finish로 게스트에 status 통지.
 *
 * 호출 컨텍스트: 디바이스 thread (bdev 모듈에 의해 cross-thread send 처리됨).
 */
static void
blk_request_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_vhost_blk_task *task = cb_arg;
	/* [한국어] cb_arg는 task 포인터. */

	spdk_bdev_free_io(bdev_io);
	/* [한국어] bdev I/O 자원 반납 — bdev 풀로 회수. */
	blk_request_finish(success ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR, task);
	/* [한국어] 결과 status를 게스트 status 바이트에 기록 + 완료 콜백 호출. */
}

/*
 * [한국어]
 * blk_request_resubmit - bdev_io_wait 콜백 — ENOMEM 후 재시도 진입점.
 *
 * @arg: spdk_vhost_blk_task 포인터.
 *
 * spdk_bdev_queue_io_wait에 등록한 콜백. bdev 모듈이 자원 여유가 생기면 호출.
 * task의 저장된 vdev/ch/cb로 process_request를 다시 호출.
 *
 * 호출 컨텍스트: 디바이스 thread (bdev 내부 스케줄링).
 */
static void
blk_request_resubmit(void *arg)
{
	struct spdk_vhost_blk_task *task = arg;
	/* [한국어] task 복원. */
	int rc = 0;
	/* [한국어] 결과. */

	rc = virtio_blk_process_request(task->bdev_io_wait_vdev, task->bdev_io_wait_ch, task,
					task->cb, task->cb_arg);
	/* [한국어] 같은 인자로 process_request 재호출. */
	if (rc == 0) {
		SPDK_DEBUGLOG(vhost_blk, "====== Task %p resubmitted ======\n", task);
	} else {
		SPDK_DEBUGLOG(vhost_blk, "====== Task %p failed ======\n", task);
	}
}

/*
 * [한국어]
 * blk_request_queue_io - ENOMEM 발생 시 bdev_io_wait 큐에 등록.
 *
 * @vdev: 디바이스.
 * @ch: io channel.
 * @task: 대기시킬 task.
 *
 * 일시적 자원 부족(-ENOMEM)에 대해 bdev 측 wait 큐에 entry 등록 → 자원 회복 시 콜백 호출.
 * 등록 자체가 실패하면 IOERR로 종료.
 */
static inline void
blk_request_queue_io(struct spdk_vhost_dev *vdev, struct spdk_io_channel *ch,
		     struct spdk_vhost_blk_task *task)
{
	int rc;
	/* [한국어] 결과. */
	struct spdk_bdev *bdev = vhost_blk_get_bdev(vdev);
	/* [한국어] bdev 포인터 (queue_io_wait 호출에 필요). */

	task->bdev_io_wait.bdev = bdev;
	/* [한국어] wait entry 필드 설정 — 어느 bdev에 대해 대기인지. */
	task->bdev_io_wait.cb_fn = blk_request_resubmit;
	/* [한국어] 자원 회복 시 호출될 콜백. */
	task->bdev_io_wait.cb_arg = task;
	/* [한국어] 콜백에 전달할 task. */
	task->bdev_io_wait_ch = ch;
	/* [한국어] 재시도 시 사용할 io_channel 캐시. */
	task->bdev_io_wait_vdev = vdev;
	/* [한국어] 재시도 시 사용할 vdev 캐시. */

	rc = spdk_bdev_queue_io_wait(bdev, ch, &task->bdev_io_wait);
	/* [한국어] bdev wait 큐에 등록. */
	if (rc != 0) {
		/* [한국어] 등록 실패 — 곧장 IOERR 응답. */
		blk_request_finish(VIRTIO_BLK_S_IOERR, task);
	}
}

/*
 * [한국어]
 * virtio_blk_process_request - virtio-blk 요청 타입별 디스패처 (이 파일의 핵심).
 *
 * @vdev: blk 디바이스.
 * @ch: bdev I/O channel.
 * @task: 처리할 task (iovs/iovcnt/payload_size 채워져 있어야 함).
 * @cb: 완료 콜백 — task->cb에 저장됨.
 * @cb_arg: 콜백 인자 — task->cb_arg에 저장됨.
 * @return: 0 정상 발급, -1 동기 실패.
 *
 * 처리 단계:
 *   1) iovs[0] (virtio_blk_outhdr) 검증 → req.type 추출.
 *   2) iovs[last] (status 1바이트) 검증 → task->status 포인터 설정.
 *   3) 페이로드 길이 = payload_size - 헤더 - status.
 *   4) type 분기 (READ/WRITE/DISCARD/WRITE_ZEROES/FLUSH/GET_ID/...).
 *   5) 각 분기에서 spdk_bdev_*v 호출 + 완료 콜백 등록.
 *   6) ENOMEM이면 wait 큐에 등록, ENOTSUP이면 UNSUPP 상태로 종료.
 *
 * 호출 체인:
 *   process_blk_task → vhost_user_process_blk_request → virtio_blk_process_request
 *   → spdk_bdev_readv/writev/flush/unmap/write_zeroes → blk_request_complete_cb.
 */
int
virtio_blk_process_request(struct spdk_vhost_dev *vdev, struct spdk_io_channel *ch,
			   struct spdk_vhost_blk_task *task, virtio_blk_request_cb cb, void *cb_arg)
{
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);
	/* [한국어] blk dev로 변환. */
	struct virtio_blk_outhdr req;
	/* [한국어] virtio-blk 요청 헤더 (16바이트) — 게스트 메모리에서 stack으로 복사. */
	struct virtio_blk_discard_write_zeroes *desc;
	/* [한국어] DISCARD/WRITE_ZEROES의 페이로드 구조 — 게스트 메모리에 매핑된 영역 가리킴. */
	struct iovec *iov;
	/* [한국어] 임시 iov 포인터. */
	uint32_t type;
	/* [한국어] 요청 타입 (VIRTIO_BLK_T_*). */
	uint64_t flush_bytes;
	/* [한국어] FLUSH의 처리 범위(바이트). */
	uint32_t payload_len;
	/* [한국어] 데이터 페이로드 길이 (헤더/상태 제외). */
	uint16_t iovcnt;
	/* [한국어] 페이로드 iov 개수 (전체 - 2). */
	int rc;
	/* [한국어] bdev 호출 결과. */

	assert(bvdev != NULL);
	/* [한국어] blk dev 변환 보장. */

	task->cb = cb;
	/* [한국어] 완료 콜백 저장 (재제출 시도 사용). */
	task->cb_arg = cb_arg;
	/* [한국어] 콜백 인자 저장. */

	iov = &task->iovs[0];
	/* [한국어] 첫 iov = 요청 헤더. */
	if (spdk_unlikely(iov->iov_len != sizeof(req))) {
		/* [한국어] 헤더 크기 불일치 — 잘못된 요청 (게스트 측 버그). */
		SPDK_DEBUGLOG(vhost_blk,
			      "First descriptor size is %zu but expected %zu (task = %p).\n",
			      iov->iov_len, sizeof(req), task);
		blk_request_finish(VIRTIO_BLK_S_UNSUPP, task);
		/* [한국어] UNSUPP로 종료. */
		return -1;
	}

	/* Some SeaBIOS versions don't align the virtio_blk_outhdr on an 8-byte boundary, which
	 * triggers ubsan errors.  So copy this small 16-byte structure to the stack to workaround
	 * this problem.
	 */
	memcpy(&req, iov->iov_base, sizeof(req));
	/* [한국어] 정렬 문제 우회 — 게스트 측 헤더를 stack에 복사 후 사용. */

	iov = &task->iovs[task->iovcnt - 1];
	/* [한국어] 마지막 iov = status 1바이트. */
	if (spdk_unlikely(iov->iov_len != 1)) {
		/* [한국어] 상태 바이트 크기 불일치 — 잘못된 요청. */
		SPDK_DEBUGLOG(vhost_blk,
			      "Last descriptor size is %zu but expected %d (task = %p).\n",
			      iov->iov_len, 1, task);
		blk_request_finish(VIRTIO_BLK_S_UNSUPP, task);
		return -1;
	}

	payload_len = task->payload_size;
	/* [한국어] 전체 길이 시작. */
	task->status = iov->iov_base;
	/* [한국어] 게스트 status 바이트 포인터 저장 — 완료 시 여기로 결과 기록. */
	payload_len -= sizeof(req) + sizeof(*task->status);
	/* [한국어] 헤더 + status 제외 → 순수 페이로드 길이. */
	iovcnt = task->iovcnt - 2;
	/* [한국어] 페이로드 iov 개수 = 전체 - 2(헤더, status). */

	type = req.type;
	/* [한국어] 요청 타입 추출. */
#ifdef VIRTIO_BLK_T_BARRIER
	/* Don't care about barrier for now (as QEMU's virtio-blk do). */
	type &= ~VIRTIO_BLK_T_BARRIER;
	/* [한국어] BARRIER 비트 마스킹 — QEMU와 같이 무시 (모든 I/O가 바로 영구 저장된다고 가정). */
#endif

	switch (type) {
	case VIRTIO_BLK_T_IN:
	case VIRTIO_BLK_T_OUT:
		/* [한국어] 일반 read/write 경로. */
		if (spdk_unlikely(payload_len == 0 || (payload_len & (512 - 1)) != 0)) {
			/* [한국어] 페이로드가 0이거나 512B 배수 아님 — 잘못된 요청. */
			SPDK_ERRLOG("%s - passed IO buffer is not multiple of 512b (task = %p).\n",
				    type ? "WRITE" : "READ", task);
			blk_request_finish(VIRTIO_BLK_S_UNSUPP, task);
			return -1;
		}

		if (type == VIRTIO_BLK_T_IN) {
			/* [한국어] READ 경로. */
			task->used_len = payload_len + sizeof(*task->status);
			/* [한국어] used_len = 게스트로 보고할 길이 (페이로드 + status 1바이트). */
			rc = spdk_bdev_readv(bvdev->bdev_desc, ch,
					     &task->iovs[1], iovcnt, req.sector * 512,
					     payload_len, blk_request_complete_cb, task);
			/* [한국어] bdev에 readv 발급. iovs[1..iovcnt+1)에 데이터 채우게 됨.
			 * 시작 LBA = req.sector*512 (sector는 512B 단위). */
		} else if (!bvdev->readonly) {
			/* [한국어] WRITE 경로 (read-only 디바이스가 아닐 때만). */
			task->used_len = sizeof(*task->status);
			/* [한국어] write는 게스트로 돌려줄 데이터 없음 — status 1바이트만. */
			rc = spdk_bdev_writev(bvdev->bdev_desc, ch,
					      &task->iovs[1], iovcnt, req.sector * 512,
					      payload_len, blk_request_complete_cb, task);
			/* [한국어] writev 발급. */
		} else {
			SPDK_DEBUGLOG(vhost_blk, "Device is in read-only mode!\n");
			/* [한국어] read-only 디바이스에 write 시도 — 거부. */
			rc = -1;
		}

		if (rc) {
			if (rc == -ENOMEM) {
				/* [한국어] bdev 자원 부족 — wait 큐에 등록해 나중에 재시도. */
				SPDK_DEBUGLOG(vhost_blk, "No memory, start to queue io.\n");
				blk_request_queue_io(vdev, ch, task);
			} else {
				/* [한국어] 그 외 에러 — IOERR로 종료. */
				blk_request_finish(VIRTIO_BLK_S_IOERR, task);
				return -1;
			}
		}
		break;
	case VIRTIO_BLK_T_DISCARD:
		/* [한국어] DISCARD (UNMAP) — 영역을 미사용 표시 (TRIM). */
		desc = task->iovs[1].iov_base;
		/* [한국어] 페이로드 영역 = discard 명령 구조. */
		if (payload_len != sizeof(*desc)) {
			/* [한국어] 페이로드 크기 불일치. */
			SPDK_NOTICELOG("Invalid discard payload size: %u\n", payload_len);
			blk_request_finish(VIRTIO_BLK_S_IOERR, task);
			return -1;
		}

		if (desc->flags & VIRTIO_BLK_WRITE_ZEROES_FLAG_UNMAP) {
			/* [한국어] UNMAP 플래그는 WRITE_ZEROES에서만 유효 — DISCARD에선 거부. */
			SPDK_ERRLOG("UNMAP flag is only used for WRITE ZEROES command\n");
			blk_request_finish(VIRTIO_BLK_S_UNSUPP, task);
			return -1;
		}

		rc = spdk_bdev_unmap(bvdev->bdev_desc, ch,
				     desc->sector * 512, desc->num_sectors * 512,
				     blk_request_complete_cb, task);
		/* [한국어] unmap 발급. */
		if (rc) {
			if (rc == -ENOMEM) {
				SPDK_DEBUGLOG(vhost_blk, "No memory, start to queue io.\n");
				blk_request_queue_io(vdev, ch, task);
			} else {
				blk_request_finish(VIRTIO_BLK_S_IOERR, task);
				return -1;
			}
		}
		break;
	case VIRTIO_BLK_T_WRITE_ZEROES:
		/* [한국어] 영역을 0으로 채우기 (실제로 0 데이터를 쓰거나 TRIM으로 처리). */
		desc = task->iovs[1].iov_base;
		/* [한국어] 페이로드 = 명령 구조. */
		if (payload_len != sizeof(*desc)) {
			SPDK_NOTICELOG("Invalid write zeroes payload size: %u\n", payload_len);
			blk_request_finish(VIRTIO_BLK_S_IOERR, task);
			return -1;
		}

		/* Unmap this range, SPDK doesn't support it, kernel will enable this flag by default
		 * without checking unmap feature is negotiated or not, the flag isn't mandatory, so
		 * just print a warning.
		 */
		if (desc->flags & VIRTIO_BLK_WRITE_ZEROES_FLAG_UNMAP) {
			/* [한국어] kernel virtio-blk 드라이버는 종종 UNMAP을 함께 요청 — 무시 + 경고만. */
			SPDK_WARNLOG("Ignore the unmap flag for WRITE ZEROES from %"PRIx64", len %"PRIx64"\n",
				     (uint64_t)desc->sector * 512, (uint64_t)desc->num_sectors * 512);
		}

		rc = spdk_bdev_write_zeroes(bvdev->bdev_desc, ch,
					    desc->sector * 512, desc->num_sectors * 512,
					    blk_request_complete_cb, task);
		/* [한국어] write_zeroes 발급. */
		if (rc) {
			if (rc == -ENOMEM) {
				SPDK_DEBUGLOG(vhost_blk, "No memory, start to queue io.\n");
				blk_request_queue_io(vdev, ch, task);
			} else {
				blk_request_finish(VIRTIO_BLK_S_IOERR, task);
				return -1;
			}
		}
		break;
	case VIRTIO_BLK_T_FLUSH:
		/* [한국어] 캐시 flush — 모든 미동기화 write를 영구 저장. */
		flush_bytes = spdk_bdev_get_num_blocks(bvdev->bdev) * spdk_bdev_get_block_size(bvdev->bdev);
		/* [한국어] 전체 디바이스 크기를 flush 범위로 — virtio-blk는 항상 전체 flush. */
		if (req.sector != 0) {
			/* [한국어] virtio-blk 스펙: flush의 sector는 0이어야 함. */
			SPDK_NOTICELOG("sector must be zero for flush command\n");
			blk_request_finish(VIRTIO_BLK_S_IOERR, task);
			return -1;
		}
		rc = spdk_bdev_flush(bvdev->bdev_desc, ch,
				     0, flush_bytes,
				     blk_request_complete_cb, task);
		/* [한국어] bdev flush 발급. */
		if (rc) {
			if (rc == -ENOMEM) {
				SPDK_DEBUGLOG(vhost_blk, "No memory, start to queue io.\n");
				blk_request_queue_io(vdev, ch, task);
			} else if (rc == -ENOTSUP) {
				/* [한국어] bdev가 flush 미지원 (예: malloc bdev) — UNSUPP 응답. */
				blk_request_finish(VIRTIO_BLK_S_UNSUPP, task);
				return -1;
			} else {
				blk_request_finish(VIRTIO_BLK_S_IOERR, task);
				return -1;
			}
		}
		break;
	case VIRTIO_BLK_T_GET_ID:
		/* [한국어] 디바이스 ID(시리얼) 반환 — 게스트 페이로드에 디바이스 이름을 채운다. */
		if (!iovcnt || !payload_len) {
			/* [한국어] 페이로드 없음 — 잘못된 요청. */
			blk_request_finish(VIRTIO_BLK_S_UNSUPP, task);
			return -1;
		}
		task->used_len = spdk_min((size_t)VIRTIO_BLK_ID_BYTES, task->iovs[1].iov_len);
		/* [한국어] 출력 길이 = min(스펙 정의 ID 크기, 게스트 버퍼 크기). */
		spdk_strcpy_pad(task->iovs[1].iov_base, spdk_bdev_get_name(bvdev->bdev),
				task->used_len, ' ');
		/* [한국어] 게스트 버퍼에 bdev 이름을 패딩으로 복사. */
		blk_request_finish(VIRTIO_BLK_S_OK, task);
		/* [한국어] 동기 완료 — bdev I/O 발급 없음. */
		break;
	default:
		/* [한국어] 미지원 타입. */
		SPDK_DEBUGLOG(vhost_blk, "Not supported request type '%"PRIu32"'.\n", type);
		blk_request_finish(VIRTIO_BLK_S_UNSUPP, task);
		return -1;
	}

	return 0;
	/* [한국어] 정상 발급 (비동기 진행 중) 또는 동기 완료. */
}

static void
process_blk_task(struct spdk_vhost_virtqueue *vq, uint16_t req_idx)
{
	struct spdk_vhost_user_blk_task *task;
	struct spdk_vhost_blk_task *blk_task;
	int rc;

	assert(vq->packed.packed_ring == false);

	task = &((struct spdk_vhost_user_blk_task *)vq->tasks)[req_idx];
	blk_task = &task->blk_task;
	if (spdk_unlikely(task->used)) {
		SPDK_ERRLOG("%s: request with idx '%"PRIu16"' is already pending.\n",
			    task->bvsession->vsession.name, req_idx);
		blk_task->used_len = 0;
		blk_task_enqueue(task);
		return;
	}

	blk_task_inc_task_cnt(task);

	blk_task_init(task);

	rc = blk_iovs_split_queue_setup(task->bvsession, vq, task->req_idx,
					blk_task->iovs, &blk_task->iovcnt, &blk_task->payload_size);

	if (rc) {
		SPDK_DEBUGLOG(vhost_blk, "Invalid request (req_idx = %"PRIu16").\n", task->req_idx);
		/* Only READ and WRITE are supported for now. */
		vhost_user_blk_request_finish(VIRTIO_BLK_S_UNSUPP, blk_task, NULL);
		return;
	}

	if (vhost_user_process_blk_request(task) == 0) {
		SPDK_DEBUGLOG(vhost_blk, "====== Task %p req_idx %d submitted ======\n", task,
			      req_idx);
	} else {
		SPDK_ERRLOG("====== Task %p req_idx %d failed ======\n", task, req_idx);
	}
}

static void
process_packed_blk_task(struct spdk_vhost_virtqueue *vq, uint16_t req_idx)
{
	struct spdk_vhost_user_blk_task *task;
	struct spdk_vhost_blk_task *blk_task;
	uint16_t task_idx = req_idx, num_descs;
	int rc;

	assert(vq->packed.packed_ring);

	/* Packed ring used the buffer_id as the task_idx to get task struct.
	 * In kernel driver, it uses the vq->free_head to set the buffer_id so the value
	 * must be in the range of 0 ~ vring.size. The free_head value must be unique
	 * in the outstanding requests.
	 * We can't use the req_idx as the task_idx because the desc can be reused in
	 * the next phase even when it's not completed in the previous phase. For example,
	 * At phase 0, last_used_idx was 2 and desc0 was not completed.Then after moving
	 * phase 1, last_avail_idx is updated to 1. In this case, req_idx can not be used
	 * as task_idx because we will know task[0]->used is true at phase 1.
	 * The split queue is quite different, the desc would insert into the free list when
	 * device completes the request, the driver gets the desc from the free list which
	 * ensures the req_idx is unique in the outstanding requests.
	 */
	task_idx = vhost_vring_packed_desc_get_buffer_id(vq, req_idx, &num_descs);

	task = &((struct spdk_vhost_user_blk_task *)vq->tasks)[task_idx];
	blk_task = &task->blk_task;
	if (spdk_unlikely(task->used)) {
		SPDK_ERRLOG("%s: request with idx '%"PRIu16"' is already pending.\n",
			    task->bvsession->vsession.name, task_idx);
		blk_task->used_len = 0;
		blk_task_enqueue(task);
		return;
	}

	task->req_idx = req_idx;
	task->num_descs = num_descs;
	task->buffer_id = task_idx;

	rte_vhost_set_inflight_desc_packed(task->bvsession->vsession.vid, vq->vring_idx,
					   req_idx, (req_idx + num_descs - 1) % vq->vring.size,
					   &task->inflight_head);

	blk_task_inc_task_cnt(task);

	blk_task_init(task);

	rc = blk_iovs_packed_queue_setup(task->bvsession, vq, task->req_idx, blk_task->iovs,
					 &blk_task->iovcnt,
					 &blk_task->payload_size);
	if (rc) {
		SPDK_DEBUGLOG(vhost_blk, "Invalid request (req_idx = %"PRIu16").\n", task->req_idx);
		/* Only READ and WRITE are supported for now. */
		vhost_user_blk_request_finish(VIRTIO_BLK_S_UNSUPP, blk_task, NULL);
		return;
	}

	if (vhost_user_process_blk_request(task) == 0) {
		SPDK_DEBUGLOG(vhost_blk, "====== Task %p req_idx %d submitted ======\n", task,
			      task_idx);
	} else {
		SPDK_ERRLOG("====== Task %p req_idx %d failed ======\n", task, task_idx);
	}
}

static void
process_packed_inflight_blk_task(struct spdk_vhost_virtqueue *vq,
				 uint16_t req_idx)
{
	spdk_vhost_inflight_desc *desc_array = vq->vring_inflight.inflight_packed->desc;
	spdk_vhost_inflight_desc *desc = &desc_array[req_idx];
	struct spdk_vhost_user_blk_task *task;
	struct spdk_vhost_blk_task *blk_task;
	uint16_t task_idx, num_descs;
	int rc;

	task_idx = desc_array[desc->last].id;
	num_descs = desc->num;
	/* In packed ring reconnection, we use the last_used_idx as the
	 * initial value. So when we process the inflight descs we still
	 * need to update the available ring index.
	 */
	vq->last_avail_idx += num_descs;
	if (vq->last_avail_idx >= vq->vring.size) {
		vq->last_avail_idx -= vq->vring.size;
		vq->packed.avail_phase = !vq->packed.avail_phase;
	}

	task = &((struct spdk_vhost_user_blk_task *)vq->tasks)[task_idx];
	blk_task = &task->blk_task;
	if (spdk_unlikely(task->used)) {
		SPDK_ERRLOG("%s: request with idx '%"PRIu16"' is already pending.\n",
			    task->bvsession->vsession.name, task_idx);
		blk_task->used_len = 0;
		blk_task_enqueue(task);
		return;
	}

	task->req_idx = req_idx;
	task->num_descs = num_descs;
	task->buffer_id = task_idx;
	/* It's for cleaning inflight entries */
	task->inflight_head = req_idx;

	blk_task_inc_task_cnt(task);

	blk_task_init(task);

	rc = blk_iovs_inflight_queue_setup(task->bvsession, vq, task->req_idx, blk_task->iovs,
					   &blk_task->iovcnt,
					   &blk_task->payload_size);
	if (rc) {
		SPDK_DEBUGLOG(vhost_blk, "Invalid request (req_idx = %"PRIu16").\n", task->req_idx);
		/* Only READ and WRITE are supported for now. */
		vhost_user_blk_request_finish(VIRTIO_BLK_S_UNSUPP, blk_task, NULL);
		return;
	}

	if (vhost_user_process_blk_request(task) == 0) {
		SPDK_DEBUGLOG(vhost_blk, "====== Task %p req_idx %d submitted ======\n", task,
			      task_idx);
	} else {
		SPDK_ERRLOG("====== Task %p req_idx %d failed ======\n", task, task_idx);
	}
}

static int
submit_inflight_desc(struct spdk_vhost_blk_session *bvsession,
		     struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession;
	spdk_vhost_resubmit_info *resubmit;
	spdk_vhost_resubmit_desc *resubmit_list;
	uint16_t req_idx;
	int i, resubmit_cnt;

	resubmit = vq->vring_inflight.resubmit_inflight;
	if (spdk_likely(resubmit == NULL || resubmit->resubmit_list == NULL ||
			resubmit->resubmit_num == 0)) {
		return 0;
	}

	resubmit_list = resubmit->resubmit_list;
	vsession = &bvsession->vsession;

	for (i = resubmit->resubmit_num - 1; i >= 0; --i) {
		req_idx = resubmit_list[i].index;
		SPDK_DEBUGLOG(vhost_blk, "====== Start processing resubmit request idx %"PRIu16"======\n",
			      req_idx);

		if (spdk_unlikely(req_idx >= vq->vring.size)) {
			SPDK_ERRLOG("%s: request idx '%"PRIu16"' exceeds virtqueue size (%"PRIu16").\n",
				    vsession->name, req_idx, vq->vring.size);
			vhost_vq_used_ring_enqueue(vsession, vq, req_idx, 0);
			continue;
		}

		if (vq->packed.packed_ring) {
			process_packed_inflight_blk_task(vq, req_idx);
		} else {
			process_blk_task(vq, req_idx);
		}
	}
	resubmit_cnt = resubmit->resubmit_num;
	resubmit->resubmit_num = 0;
	return resubmit_cnt;
}

static int
process_vq(struct spdk_vhost_blk_session *bvsession, struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	uint16_t reqs[SPDK_VHOST_VQ_MAX_SUBMISSIONS];
	uint16_t reqs_cnt, i;
	int resubmit_cnt = 0;

	resubmit_cnt = submit_inflight_desc(bvsession, vq);

	reqs_cnt = vhost_vq_avail_ring_get(vq, reqs, SPDK_COUNTOF(reqs));
	if (!reqs_cnt) {
		return resubmit_cnt;
	}

	for (i = 0; i < reqs_cnt; i++) {
		SPDK_DEBUGLOG(vhost_blk, "====== Starting processing request idx %"PRIu16"======\n",
			      reqs[i]);

		if (spdk_unlikely(reqs[i] >= vq->vring.size)) {
			SPDK_ERRLOG("%s: request idx '%"PRIu16"' exceeds virtqueue size (%"PRIu16").\n",
				    vsession->name, reqs[i], vq->vring.size);
			vhost_vq_used_ring_enqueue(vsession, vq, reqs[i], 0);
			continue;
		}

		rte_vhost_set_inflight_desc_split(vsession->vid, vq->vring_idx, reqs[i]);

		process_blk_task(vq, reqs[i]);
	}

	return reqs_cnt;
}

static int
process_packed_vq(struct spdk_vhost_blk_session *bvsession, struct spdk_vhost_virtqueue *vq)
{
	uint16_t i = 0;
	uint16_t count = 0;
	int resubmit_cnt = 0;

	resubmit_cnt = submit_inflight_desc(bvsession, vq);

	while (i++ < SPDK_VHOST_VQ_MAX_SUBMISSIONS &&
	       vhost_vq_packed_ring_is_avail(vq)) {
		SPDK_DEBUGLOG(vhost_blk, "====== Starting processing request idx %"PRIu16"======\n",
			      vq->last_avail_idx);
		count++;
		process_packed_blk_task(vq, vq->last_avail_idx);
	}

	return count > 0 ? count : resubmit_cnt;
}

static int
_vdev_vq_worker(struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = vq->vsession;
	struct spdk_vhost_blk_session *bvsession = to_blk_session(vsession);
	bool packed_ring;
	int rc = 0;

	packed_ring = vq->packed.packed_ring;
	if (packed_ring) {
		rc = process_packed_vq(bvsession, vq);
	} else {
		rc = process_vq(bvsession, vq);
	}

	vhost_session_vq_used_signal(vq);

	return rc;

}

static int
vdev_vq_worker(void *arg)
{
	struct spdk_vhost_virtqueue *vq = arg;

	return _vdev_vq_worker(vq);
}

static int
vdev_worker(void *arg)
{
	struct spdk_vhost_blk_session *bvsession = arg;
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	uint16_t q_idx;
	int rc = 0;

	for (q_idx = 0; q_idx < vsession->max_queues; q_idx++) {
		rc += _vdev_vq_worker(&vsession->virtqueue[q_idx]);
	}

	return rc > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
no_bdev_process_vq(struct spdk_vhost_blk_session *bvsession, struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct iovec iovs[SPDK_VHOST_IOVS_MAX];
	uint32_t length;
	uint16_t iovcnt, req_idx;

	if (vhost_vq_avail_ring_get(vq, &req_idx, 1) != 1) {
		return;
	}

	iovcnt = SPDK_COUNTOF(iovs);
	if (blk_iovs_split_queue_setup(bvsession, vq, req_idx, iovs, &iovcnt, &length) == 0) {
		*(volatile uint8_t *)iovs[iovcnt - 1].iov_base = VIRTIO_BLK_S_IOERR;
		SPDK_DEBUGLOG(vhost_blk_data, "Aborting request %" PRIu16"\n", req_idx);
	}

	vhost_vq_used_ring_enqueue(vsession, vq, req_idx, 0);
}

static void
no_bdev_process_packed_vq(struct spdk_vhost_blk_session *bvsession, struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct spdk_vhost_user_blk_task *task;
	struct spdk_vhost_blk_task *blk_task;
	uint32_t length;
	uint16_t req_idx = vq->last_avail_idx;
	uint16_t task_idx, num_descs;

	if (!vhost_vq_packed_ring_is_avail(vq)) {
		return;
	}

	task_idx = vhost_vring_packed_desc_get_buffer_id(vq, req_idx, &num_descs);
	task = &((struct spdk_vhost_user_blk_task *)vq->tasks)[task_idx];
	blk_task = &task->blk_task;
	if (spdk_unlikely(task->used)) {
		SPDK_ERRLOG("%s: request with idx '%"PRIu16"' is already pending.\n",
			    vsession->name, req_idx);
		vhost_vq_packed_ring_enqueue(vsession, vq, num_descs,
					     task->buffer_id, blk_task->used_len,
					     task->inflight_head);
		return;
	}

	task->req_idx = req_idx;
	task->num_descs = num_descs;
	task->buffer_id = task_idx;
	blk_task_init(task);

	if (blk_iovs_packed_queue_setup(bvsession, vq, task->req_idx, blk_task->iovs, &blk_task->iovcnt,
					&length)) {
		*(volatile uint8_t *)(blk_task->iovs[blk_task->iovcnt - 1].iov_base) = VIRTIO_BLK_S_IOERR;
		SPDK_DEBUGLOG(vhost_blk_data, "Aborting request %" PRIu16"\n", req_idx);
	}

	task->used = false;
	vhost_vq_packed_ring_enqueue(vsession, vq, num_descs,
				     task->buffer_id, blk_task->used_len,
				     task->inflight_head);
}

static int
_no_bdev_vdev_vq_worker(struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = vq->vsession;
	struct spdk_vhost_blk_session *bvsession = to_blk_session(vsession);
	bool packed_ring;

	packed_ring = vq->packed.packed_ring;
	if (packed_ring) {
		no_bdev_process_packed_vq(bvsession, vq);
	} else {
		no_bdev_process_vq(bvsession, vq);
	}

	vhost_session_vq_used_signal(vq);

	if (vsession->task_cnt == 0 && bvsession->io_channel) {
		vhost_blk_put_io_channel(bvsession->io_channel);
		bvsession->io_channel = NULL;
	}

	return SPDK_POLLER_BUSY;
}

static int
no_bdev_vdev_vq_worker(void *arg)
{
	struct spdk_vhost_virtqueue *vq = arg;

	return _no_bdev_vdev_vq_worker(vq);
}

static int
no_bdev_vdev_worker(void *arg)
{
	struct spdk_vhost_blk_session *bvsession = arg;
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	uint16_t q_idx;

	for (q_idx = 0; q_idx < vsession->max_queues; q_idx++) {
		_no_bdev_vdev_vq_worker(&vsession->virtqueue[q_idx]);
	}

	return SPDK_POLLER_BUSY;
}

static void
vhost_blk_session_unregister_interrupts(struct spdk_vhost_blk_session *bvsession)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct spdk_vhost_virtqueue *vq;
	int i;

	SPDK_DEBUGLOG(vhost_blk, "unregister virtqueues interrupt\n");
	for (i = 0; i < vsession->max_queues; i++) {
		vq = &vsession->virtqueue[i];
		if (vq->intr == NULL) {
			break;
		}

		SPDK_DEBUGLOG(vhost_blk, "unregister vq[%d]'s kickfd is %d\n",
			      i, vq->vring.kickfd);
		spdk_interrupt_unregister(&vq->intr);
	}
}

static void
_vhost_blk_vq_register_interrupt(void *arg)
{
	struct spdk_vhost_virtqueue *vq = arg;
	struct spdk_vhost_session *vsession = vq->vsession;
	struct spdk_vhost_blk_dev *bvdev =  to_blk_dev(vsession->vdev);

	assert(bvdev != NULL);

	if (bvdev->bdev) {
		vq->intr = spdk_interrupt_register(vq->vring.kickfd, vdev_vq_worker, vq, "vdev_vq_worker");
	} else {
		vq->intr = spdk_interrupt_register(vq->vring.kickfd, no_bdev_vdev_vq_worker, vq,
						   "no_bdev_vdev_vq_worker");
	}

	if (vq->intr == NULL) {
		SPDK_ERRLOG("Fail to register req notifier handler.\n");
		assert(false);
	}
}

static int
vhost_blk_vq_enable(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *vq)
{
	if (spdk_interrupt_mode_is_enabled()) {
		spdk_thread_send_msg(vsession->vdev->thread, _vhost_blk_vq_register_interrupt, vq);
	}

	return 0;
}

static int
vhost_blk_session_register_no_bdev_interrupts(struct spdk_vhost_blk_session *bvsession)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct spdk_vhost_virtqueue *vq = NULL;
	int i;

	SPDK_DEBUGLOG(vhost_blk, "Register virtqueues interrupt\n");
	for (i = 0; i < vsession->max_queues; i++) {
		vq = &vsession->virtqueue[i];
		SPDK_DEBUGLOG(vhost_blk, "Register vq[%d]'s kickfd is %d\n",
			      i, vq->vring.kickfd);
		vq->intr = spdk_interrupt_register(vq->vring.kickfd, no_bdev_vdev_vq_worker, vq,
						   "no_bdev_vdev_vq_worker");
		if (vq->intr == NULL) {
			goto err;
		}

	}

	return 0;

err:
	vhost_blk_session_unregister_interrupts(bvsession);
	return -1;
}

static void
vhost_blk_poller_set_interrupt_mode(struct spdk_poller *poller, void *cb_arg, bool interrupt_mode)
{
	struct spdk_vhost_blk_session *bvsession = cb_arg;

	vhost_user_session_set_interrupt_mode(&bvsession->vsession, interrupt_mode);
}

static void
bdev_event_cpl_cb(struct spdk_vhost_dev *vdev, void *ctx)
{
	enum spdk_bdev_event_type type = (enum spdk_bdev_event_type)(uintptr_t)ctx;
	struct spdk_vhost_blk_dev *bvdev;

	if (type == SPDK_BDEV_EVENT_REMOVE) {
		/* All sessions have been notified, time to close the bdev */
		bvdev = to_blk_dev(vdev);
		assert(bvdev != NULL);
		spdk_bdev_close(bvdev->bdev_desc);
		bvdev->bdev_desc = NULL;
		bvdev->bdev = NULL;
	}
}

static int
vhost_session_bdev_resize_cb(struct spdk_vhost_dev *vdev,
			     struct spdk_vhost_session *vsession,
			     void *ctx)
{
	SPDK_NOTICELOG("bdev send slave msg to vid(%d)\n", vsession->vid);
#if RTE_VERSION >= RTE_VERSION_NUM(23, 03, 0, 0)
	rte_vhost_backend_config_change(vsession->vid, false);
#else
	rte_vhost_slave_config_change(vsession->vid, false);
#endif

	return 0;
}

static void
vhost_user_blk_resize_cb(struct spdk_vhost_dev *vdev, bdev_event_cb_complete cb, void *cb_arg)
{
	vhost_user_dev_foreach_session(vdev, vhost_session_bdev_resize_cb,
				       cb, cb_arg);
}

static int
vhost_user_session_bdev_remove_cb(struct spdk_vhost_dev *vdev,
				  struct spdk_vhost_session *vsession,
				  void *ctx)
{
	struct spdk_vhost_blk_session *bvsession;
	int rc;

	bvsession = to_blk_session(vsession);
	if (bvsession->requestq_poller) {
		spdk_poller_unregister(&bvsession->requestq_poller);
		if (spdk_interrupt_mode_is_enabled()) {
			vhost_blk_session_unregister_interrupts(bvsession);
			rc = vhost_blk_session_register_no_bdev_interrupts(bvsession);
			if (rc) {
				SPDK_ERRLOG("%s: Interrupt register failed\n", vsession->name);
				return rc;
			}
		}

		bvsession->requestq_poller = SPDK_POLLER_REGISTER(no_bdev_vdev_worker, bvsession, 0);
		spdk_poller_register_interrupt(bvsession->requestq_poller, vhost_blk_poller_set_interrupt_mode,
					       bvsession);
	}

	return 0;
}

static void
vhost_user_bdev_remove_cb(struct spdk_vhost_dev *vdev, bdev_event_cb_complete cb, void *cb_arg)
{
	SPDK_WARNLOG("%s: hot-removing bdev - all further requests will fail.\n",
		     vdev->name);

	vhost_user_dev_foreach_session(vdev, vhost_user_session_bdev_remove_cb,
				       cb, cb_arg);
}

static void
vhost_user_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_vhost_dev *vdev,
			 bdev_event_cb_complete cb, void *cb_arg)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		vhost_user_bdev_remove_cb(vdev, cb, cb_arg);
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		vhost_user_blk_resize_cb(vdev, cb, cb_arg);
		break;
	default:
		assert(false);
		return;
	}
}

static void
bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
	      void *event_ctx)
{
	struct spdk_vhost_dev *vdev = (struct spdk_vhost_dev *)event_ctx;
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);

	assert(bvdev != NULL);

	SPDK_DEBUGLOG(vhost_blk, "Bdev event: type %d, name %s\n",
		      type,
		      bdev->name);

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
	case SPDK_BDEV_EVENT_RESIZE:
		bvdev->ops->bdev_event(type, vdev, bdev_event_cpl_cb, (void *)type);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

static void
free_task_pool(struct spdk_vhost_blk_session *bvsession)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct spdk_vhost_virtqueue *vq;
	uint16_t i;

	for (i = 0; i < vsession->max_queues; i++) {
		vq = &vsession->virtqueue[i];
		if (vq->tasks == NULL) {
			continue;
		}

		spdk_free(vq->tasks);
		vq->tasks = NULL;
	}
}

static int
alloc_vq_task_pool(struct spdk_vhost_session *vsession, uint16_t qid)
{
	struct spdk_vhost_blk_session *bvsession = to_blk_session(vsession);
	struct spdk_vhost_virtqueue *vq;
	struct spdk_vhost_user_blk_task *task;
	uint32_t task_cnt;
	uint32_t j;

	if (qid >= SPDK_VHOST_MAX_VQUEUES) {
		return -EINVAL;
	}

	vq = &vsession->virtqueue[qid];
	if (vq->vring.desc == NULL) {
		return 0;
	}

	task_cnt = vq->vring.size;
	if (task_cnt > SPDK_VHOST_MAX_VQ_SIZE) {
		/* sanity check */
		SPDK_ERRLOG("%s: virtqueue %"PRIu16" is too big. (size = %"PRIu32", max = %"PRIu32")\n",
			    vsession->name, qid, task_cnt, SPDK_VHOST_MAX_VQ_SIZE);
		return -1;
	}
	vq->tasks = spdk_zmalloc(sizeof(struct spdk_vhost_user_blk_task) * task_cnt,
				 SPDK_CACHE_LINE_SIZE, NULL,
				 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (vq->tasks == NULL) {
		SPDK_ERRLOG("%s: failed to allocate %"PRIu32" tasks for virtqueue %"PRIu16"\n",
			    vsession->name, task_cnt, qid);
		return -1;
	}

	for (j = 0; j < task_cnt; j++) {
		task = &((struct spdk_vhost_user_blk_task *)vq->tasks)[j];
		task->bvsession = bvsession;
		task->req_idx = j;
		task->vq = vq;
	}

	return 0;
}

static int
vhost_blk_start(struct spdk_vhost_dev *vdev,
		struct spdk_vhost_session *vsession, void *unused)
{
	struct spdk_vhost_blk_session *bvsession = to_blk_session(vsession);
	struct spdk_vhost_blk_dev *bvdev;
	int i;

	/* return if start is already in progress */
	if (bvsession->requestq_poller) {
		SPDK_INFOLOG(vhost, "%s: start in progress\n", vsession->name);
		return -EINPROGRESS;
	}

	/* validate all I/O queues are in a contiguous index range */
	for (i = 0; i < vsession->max_queues; i++) {
		/* vring.desc and vring.desc_packed are in a union struct
		 * so q->vring.desc can replace q->vring.desc_packed.
		 */
		if (vsession->virtqueue[i].vring.desc == NULL) {
			SPDK_ERRLOG("%s: queue %"PRIu32" is empty\n", vsession->name, i);
			return -1;
		}
	}

	bvdev = to_blk_dev(vdev);
	assert(bvdev != NULL);
	bvsession->bvdev = bvdev;

	if (bvdev->bdev) {
		bvsession->io_channel = vhost_blk_get_io_channel(vdev);
		if (!bvsession->io_channel) {
			free_task_pool(bvsession);
			SPDK_ERRLOG("%s: I/O channel allocation failed\n", vsession->name);
			return -1;
		}
	}

	if (bvdev->bdev) {
		bvsession->requestq_poller = SPDK_POLLER_REGISTER(vdev_worker, bvsession, 0);
	} else {
		bvsession->requestq_poller = SPDK_POLLER_REGISTER(no_bdev_vdev_worker, bvsession, 0);
	}
	SPDK_INFOLOG(vhost, "%s: started poller on lcore %d\n",
		     vsession->name, spdk_env_get_current_core());

	spdk_poller_register_interrupt(bvsession->requestq_poller, vhost_blk_poller_set_interrupt_mode,
				       bvsession);

	return 0;
}

static int
destroy_session_poller_cb(void *arg)
{
	struct spdk_vhost_blk_session *bvsession = arg;
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vsession->vdev);
	int i;

	if (vsession->task_cnt > 0 || (pthread_mutex_trylock(&user_dev->lock) != 0)) {
		assert(vsession->stop_retry_count > 0);
		vsession->stop_retry_count--;
		if (vsession->stop_retry_count == 0) {
			SPDK_ERRLOG("%s: Timedout when destroy session (task_cnt %d)\n", vsession->name,
				    vsession->task_cnt);
			spdk_poller_unregister(&bvsession->stop_poller);
			vhost_user_session_stop_done(vsession, -ETIMEDOUT);
		}

		return SPDK_POLLER_BUSY;
	}

	for (i = 0; i < vsession->max_queues; i++) {
		vsession->virtqueue[i].next_event_time = 0;
		vhost_vq_used_signal(vsession, &vsession->virtqueue[i]);
	}

	SPDK_INFOLOG(vhost, "%s: stopping poller on lcore %d\n",
		     vsession->name, spdk_env_get_current_core());

	if (bvsession->io_channel) {
		vhost_blk_put_io_channel(bvsession->io_channel);
		bvsession->io_channel = NULL;
	}

	free_task_pool(bvsession);
	spdk_poller_unregister(&bvsession->stop_poller);
	vhost_user_session_stop_done(vsession, 0);

	pthread_mutex_unlock(&user_dev->lock);
	return SPDK_POLLER_BUSY;
}

static int
vhost_blk_stop(struct spdk_vhost_dev *vdev,
	       struct spdk_vhost_session *vsession, void *unused)
{
	struct spdk_vhost_blk_session *bvsession = to_blk_session(vsession);

	/* return if stop is already in progress */
	if (bvsession->stop_poller) {
		return -EINPROGRESS;
	}

	spdk_poller_unregister(&bvsession->requestq_poller);
	vhost_blk_session_unregister_interrupts(bvsession);

	bvsession->vsession.stop_retry_count = (SPDK_VHOST_SESSION_STOP_RETRY_TIMEOUT_IN_SEC * 1000 *
						1000) / SPDK_VHOST_SESSION_STOP_RETRY_PERIOD_IN_US;
	bvsession->stop_poller = SPDK_POLLER_REGISTER(destroy_session_poller_cb,
				 bvsession, SPDK_VHOST_SESSION_STOP_RETRY_PERIOD_IN_US);
	return 0;
}

static void
vhost_blk_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_blk_dev *bvdev;

	bvdev = to_blk_dev(vdev);
	assert(bvdev != NULL);

	spdk_json_write_named_object_begin(w, "block");

	spdk_json_write_named_bool(w, "readonly", bvdev->readonly);

	spdk_json_write_name(w, "bdev");
	if (bvdev->bdev) {
		spdk_json_write_string(w, spdk_bdev_get_name(bvdev->bdev));
	} else {
		spdk_json_write_null(w);
	}
	spdk_json_write_named_string(w, "transport", bvdev->ops->name);

	spdk_json_write_object_end(w);
}

static void
vhost_blk_write_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_blk_dev *bvdev;

	bvdev = to_blk_dev(vdev);
	assert(bvdev != NULL);

	if (!bvdev->bdev) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "vhost_create_blk_controller");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "ctrlr", vdev->name);
	spdk_json_write_named_string(w, "dev_name", spdk_bdev_get_name(bvdev->bdev));
	spdk_json_write_named_string(w, "cpumask",
				     spdk_cpuset_fmt(spdk_thread_get_cpumask(vdev->thread)));
	spdk_json_write_named_bool(w, "readonly", bvdev->readonly);
	spdk_json_write_named_string(w, "transport", bvdev->ops->name);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static int vhost_blk_destroy(struct spdk_vhost_dev *dev);

static int
vhost_blk_get_config(struct spdk_vhost_dev *vdev, uint8_t *config,
		     uint32_t len)
{
	struct virtio_blk_config blkcfg;
	struct spdk_bdev *bdev;
	uint32_t blk_size;
	uint64_t blkcnt;

	memset(&blkcfg, 0, sizeof(blkcfg));
	bdev = vhost_blk_get_bdev(vdev);
	if (bdev == NULL) {
		/* We can't just return -1 here as this GET_CONFIG message might
		 * be caused by a QEMU VM reboot. Returning -1 will indicate an
		 * error to QEMU, who might then decide to terminate itself.
		 * We don't want that. A simple reboot shouldn't break the system.
		 *
		 * Presenting a block device with block size 0 and block count 0
		 * doesn't cause any problems on QEMU side and the virtio-pci
		 * device is even still available inside the VM, but there will
		 * be no block device created for it - the kernel drivers will
		 * silently reject it.
		 */
		blk_size = 0;
		blkcnt = 0;
	} else {
		blk_size = spdk_bdev_get_block_size(bdev);
		blkcnt = spdk_bdev_get_num_blocks(bdev);
		if (spdk_bdev_get_buf_align(bdev) > 1) {
			blkcfg.size_max = SPDK_BDEV_LARGE_BUF_MAX_SIZE;
			blkcfg.seg_max = spdk_min(SPDK_VHOST_IOVS_MAX - 2 - 1, SPDK_BDEV_IO_NUM_CHILD_IOV - 2 - 1);
		} else {
			blkcfg.size_max = 131072;
			/*  -2 for REQ and RESP and -1 for region boundary splitting */
			blkcfg.seg_max = SPDK_VHOST_IOVS_MAX - 2 - 1;
		}
	}

	blkcfg.blk_size = blk_size;
	/* minimum I/O size in blocks */
	blkcfg.min_io_size = 1;
	/* expressed in 512 Bytes sectors */
	blkcfg.capacity = (blkcnt * blk_size) / 512;
	/* QEMU can overwrite this value when started */
	blkcfg.num_queues = SPDK_VHOST_MAX_VQUEUES;

	if (bdev && spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		/* 16MiB, expressed in 512 Bytes */
		blkcfg.max_discard_sectors = 32768;
		blkcfg.max_discard_seg = 1;
		blkcfg.discard_sector_alignment = blk_size / 512;
	}
	if (bdev && spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
		blkcfg.max_write_zeroes_sectors = 32768;
		blkcfg.max_write_zeroes_seg = 1;
	}

	memcpy(config, &blkcfg, spdk_min(len, sizeof(blkcfg)));

	return 0;
}

static int
vhost_blk_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			 uint32_t iops_threshold)
{
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);

	assert(bvdev != NULL);

	return bvdev->ops->set_coalescing(vdev, delay_base_us, iops_threshold);
}

static void
vhost_blk_get_coalescing(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			 uint32_t *iops_threshold)
{
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);

	assert(bvdev != NULL);

	bvdev->ops->get_coalescing(vdev, delay_base_us, iops_threshold);
}

static const struct spdk_vhost_user_dev_backend vhost_blk_user_device_backend = {
	.session_ctx_size = sizeof(struct spdk_vhost_blk_session) - sizeof(struct spdk_vhost_session),
	.start_session =  vhost_blk_start,
	.stop_session = vhost_blk_stop,
	.alloc_vq_tasks = alloc_vq_task_pool,
	.enable_vq = vhost_blk_vq_enable,
};

static const struct spdk_vhost_dev_backend vhost_blk_device_backend = {
	.type = VHOST_BACKEND_BLK,
	.vhost_get_config = vhost_blk_get_config,
	.dump_info_json = vhost_blk_dump_info_json,
	.write_config_json = vhost_blk_write_config_json,
	.remove_device = vhost_blk_destroy,
	.set_coalescing = vhost_blk_set_coalescing,
	.get_coalescing = vhost_blk_get_coalescing,
};

int
virtio_blk_construct_ctrlr(struct spdk_vhost_dev *vdev, const char *address,
			   struct spdk_cpuset *cpumask, const struct spdk_json_val *params,
			   const struct spdk_vhost_user_dev_backend *user_backend)
{
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);

	assert(bvdev != NULL);

	return bvdev->ops->create_ctrlr(vdev, cpumask, address, params, (void *)user_backend);
}

int
spdk_vhost_blk_construct(const char *name, const char *cpumask, const char *dev_name,
			 const char *transport, const struct spdk_json_val *params)
{
	struct spdk_vhost_blk_dev *bvdev = NULL;
	struct spdk_vhost_dev *vdev;
	struct spdk_bdev *bdev;
	const char *transport_name = VIRTIO_BLK_DEFAULT_TRANSPORT;
	int ret = 0;

	bvdev = calloc(1, sizeof(*bvdev));
	if (bvdev == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	if (transport != NULL) {
		transport_name = transport;
	}

	bvdev->ops = virtio_blk_get_transport_ops(transport_name);
	if (!bvdev->ops) {
		ret = -EINVAL;
		SPDK_ERRLOG("Transport type '%s' unavailable.\n", transport_name);
		goto out;
	}

	ret = spdk_bdev_open_ext(dev_name, true, bdev_event_cb, bvdev, &bvdev->bdev_desc);
	if (ret != 0) {
		SPDK_ERRLOG("%s: could not open bdev '%s', error=%d\n",
			    name, dev_name, ret);
		goto out;
	}
	bdev = spdk_bdev_desc_get_bdev(bvdev->bdev_desc);

	vdev = &bvdev->vdev;
	vdev->virtio_features = SPDK_VHOST_BLK_FEATURES_BASE;
	vdev->disabled_features = SPDK_VHOST_BLK_DISABLED_FEATURES;
	vdev->protocol_features = SPDK_VHOST_BLK_PROTOCOL_FEATURES;

	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		vdev->virtio_features |= (1ULL << VIRTIO_BLK_F_DISCARD);
	}
	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
		vdev->virtio_features |= (1ULL << VIRTIO_BLK_F_WRITE_ZEROES);
	}

	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH)) {
		vdev->virtio_features |= (1ULL << VIRTIO_BLK_F_FLUSH);
	}

	bvdev->bdev = bdev;
	bvdev->readonly = false;
	ret = vhost_dev_register(vdev, name, cpumask, params, &vhost_blk_device_backend,
				 &vhost_blk_user_device_backend, false);
	if (ret != 0) {
		spdk_bdev_close(bvdev->bdev_desc);
		goto out;
	}

	SPDK_INFOLOG(vhost, "%s: using bdev '%s'\n", name, dev_name);
out:
	if (ret != 0 && bvdev) {
		free(bvdev);
	}
	return ret;
}

int
virtio_blk_destroy_ctrlr(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);

	assert(bvdev != NULL);

	return bvdev->ops->destroy_ctrlr(vdev);
}

static int
vhost_blk_destroy(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);
	int rc;

	assert(bvdev != NULL);

	rc = vhost_dev_unregister(&bvdev->vdev);
	if (rc != 0) {
		return rc;
	}

	if (bvdev->bdev_desc) {
		spdk_bdev_close(bvdev->bdev_desc);
		bvdev->bdev_desc = NULL;
	}
	bvdev->bdev = NULL;

	free(bvdev);
	return 0;
}

struct spdk_io_channel *
vhost_blk_get_io_channel(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);

	assert(bvdev != NULL);

	return spdk_bdev_get_io_channel(bvdev->bdev_desc);
}

void
vhost_blk_put_io_channel(struct spdk_io_channel *ch)
{
	spdk_put_io_channel(ch);
}

static struct spdk_virtio_blk_transport *
vhost_user_blk_create(const struct spdk_json_val *params)
{
	int ret;
	struct spdk_virtio_blk_transport *vhost_user_blk;

	vhost_user_blk = calloc(1, sizeof(*vhost_user_blk));
	if (!vhost_user_blk) {
		return NULL;
	}

	ret = vhost_user_init();
	if (ret != 0) {
		free(vhost_user_blk);
		return NULL;
	}

	return vhost_user_blk;
}

static int
vhost_user_blk_destroy(struct spdk_virtio_blk_transport *transport,
		       spdk_vhost_fini_cb cb_fn)
{
	vhost_user_fini(cb_fn);
	free(transport);
	return 0;
}

struct rpc_vhost_blk {
	bool readonly;
	bool packed_ring;
};

static const struct spdk_json_object_decoder rpc_construct_vhost_blk[] = {
	{"readonly", offsetof(struct rpc_vhost_blk, readonly), spdk_json_decode_bool, true},
	{"packed_ring", offsetof(struct rpc_vhost_blk, packed_ring), spdk_json_decode_bool, true},
};

static int
vhost_user_blk_create_ctrlr(struct spdk_vhost_dev *vdev, struct spdk_cpuset *cpumask,
			    const char *address, const struct spdk_json_val *params, void *custom_opts)
{
	struct rpc_vhost_blk req = {0};
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);

	assert(bvdev != NULL);

	if (spdk_json_decode_object_relaxed(params, rpc_construct_vhost_blk,
					    SPDK_COUNTOF(rpc_construct_vhost_blk),
					    &req)) {
		SPDK_DEBUGLOG(vhost_blk, "spdk_json_decode_object failed\n");
		return -EINVAL;
	}

	if (req.packed_ring) {
		vdev->virtio_features |= (uint64_t)req.packed_ring << VIRTIO_F_RING_PACKED;
	}
	if (req.readonly) {
		vdev->virtio_features |= (1ULL << VIRTIO_BLK_F_RO);
		bvdev->readonly = req.readonly;
	}

	return vhost_user_dev_create(vdev, address, cpumask, custom_opts, false);
}

static int
vhost_user_blk_destroy_ctrlr(struct spdk_vhost_dev *vdev)
{
	return vhost_user_dev_unregister(vdev);
}

static void
vhost_user_blk_dump_opts(struct spdk_virtio_blk_transport *transport, struct spdk_json_write_ctx *w)
{
	assert(w != NULL);

	spdk_json_write_named_string(w, "name", transport->ops->name);
}

static const struct spdk_virtio_blk_transport_ops vhost_user_blk = {
	.name = "vhost_user_blk",

	.dump_opts = vhost_user_blk_dump_opts,

	.create = vhost_user_blk_create,
	.destroy = vhost_user_blk_destroy,

	.create_ctrlr = vhost_user_blk_create_ctrlr,
	.destroy_ctrlr = vhost_user_blk_destroy_ctrlr,

	.bdev_event = vhost_user_bdev_event_cb,
	.set_coalescing = vhost_user_set_coalescing,
	.get_coalescing = vhost_user_get_coalescing,
};

SPDK_VIRTIO_BLK_TRANSPORT_REGISTER(vhost_user_blk, &vhost_user_blk);

SPDK_LOG_REGISTER_COMPONENT(vhost_blk)
SPDK_LOG_REGISTER_COMPONENT(vhost_blk_data)
