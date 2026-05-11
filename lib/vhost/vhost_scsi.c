/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK vhost-user-scsi 백엔드 구현 (vhost_scsi.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 virtio-scsi 사양(linux/virtio_scsi.h)을 따르는 vhost-user 백엔드를 구현한다.
 * 게스트 VM이 보내는 SCSI 명령을 virtqueue에서 꺼내어 SPDK SCSI 레이어(spdk_scsi_dev)로
 * 디스패치하고, 완료된 결과(VIRTIO_SCSI_S_OK 등 상태/sense 데이터)를 used ring에 기록한다.
 * 컨트롤 큐(Control Q, idx 0), 이벤트 큐(Event Q, idx 1), I/O 요청 큐(Request Q, idx 2..N)
 * 세 종류 큐를 처리하며, 한 컨트롤러는 최대 SPDK_VHOST_SCSI_CTRLR_MAX_DEVS(8)개의 SCSI target을
 * 노출할 수 있다(target = LUN 그룹). hotplug/hotremove/resize 이벤트도 이 파일에서 발행한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [게스트 VM] virtio-scsi driver
 *      ↓ (SCSI CDB를 virtqueue에 enqueue, kickfd write)
 *   [DPDK rte_vhost] kickfd → SPDK 콜백
 *      ↓
 *   [vhost_scsi.c::vdev_worker] (Request Q 폴러)
 *      → process_vq → process_scsi_task → process_request → task_data_setup
 *      → spdk_scsi_dev_queue_task (SPDK SCSI 레이어로 전달)
 *      ↓ (SCSI 실행 후 콜백)
 *   [vhost_scsi_task_cpl] → submit_completion → vhost_vq_used_ring_enqueue → callfd
 *      ↓
 *   [게스트 VM] IRQ로 완료 인지
 *
 * 호출 컨텍스트: 디바이스에 할당된 SPDK reactor thread (lcore 1개에 고정).
 * 폴러(vdev_worker, vdev_mgmt_worker)는 그 thread에서 무한 루프 도는 spdk_poller로 등록된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/scsi.h(SPDK SCSI 레이어), spdk/scsi_spec.h(SCSI 표준 정의),
 *   linux/virtio_scsi.h(virtio-scsi UAPI), vhost_internal.h(vhost 공통 인프라).
 * - 호출 대상: spdk_scsi_dev_queue_task / spdk_scsi_dev_construct_ext (SCSI 레이어),
 *   vhost_vq_avail_ring_get / vhost_vq_used_ring_enqueue (vhost.c 공통 헬퍼),
 *   rte_vhost_set_inflight_desc_split (DPDK rte_vhost).
 * - 데이터 흐름: virtio_scsi_cmd_req(게스트) → spdk_scsi_task(SPDK) → bdev I/O →
 *   spdk_scsi_task 완료 → virtio_scsi_cmd_resp(게스트).
 * - 공유 자료구조: spdk_vhost_scsi_dev(컨트롤러), spdk_vhost_scsi_session(세션),
 *   spdk_vhost_scsi_task(task), scsi_dev_state[8](target 슬롯).
 *
 * === 주요 함수/구조체 요약 ===
 * - vhost_scsi_start / vhost_scsi_stop: 세션 시작/종료 콜백 (poller 등록/해제).
 * - vdev_worker: Request Q(들) 폴링 — I/O 요청 처리의 핵심.
 * - vdev_mgmt_worker: Control Q + Event Q 처리 (TMF, hotplug 이벤트).
 * - process_vq → process_scsi_task → process_request: 게스트 요청 처리 파이프라인.
 * - task_data_setup: 게스트 desc chain → SCSI iovs/dxfer_dir 구성.
 * - eventq_enqueue: hotplug 이벤트를 Event Q에 enqueue (게스트에 알림).
 * - spdk_vhost_scsi_dev_construct / _no_start: virtio-scsi 컨트롤러 생성.
 * - spdk_vhost_scsi_dev_add_tgt / _remove_tgt: SCSI target(LUN 그룹) 동적 추가/제거.
 * - struct spdk_vhost_scsi_dev: 컨트롤러 — vdev + 8개 target 슬롯 + ref count.
 * - struct spdk_vhost_scsi_session: 세션 — vsession + svdev 역참조 + 세션별 target 사본 + poller들.
 * - struct spdk_vhost_scsi_task: 단일 SCSI 요청 — spdk_scsi_task + iovs + req/resp 포인터.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 C 라이브러리(stdint, stdbool 등) SPDK 통합 인클루드. */

#include <linux/virtio_scsi.h>
/* [한국어] virtio-scsi UAPI 정의 (struct virtio_scsi_cmd_req/resp, VIRTIO_SCSI_T_TMF 등).
 * 게스트가 보내는 요청과 호스트가 회신하는 응답의 메모리 레이아웃 표준. */

#include "spdk/env.h"
/* [한국어] SPDK 환경 추상화 — DPDK 기반 메모리 할당 등. */
#include "spdk/thread.h"
/* [한국어] spdk_thread / spdk_poller / spdk_thread_send_msg API. */
#include "spdk/scsi.h"
/* [한국어] SPDK SCSI 레이어 — spdk_scsi_dev, spdk_scsi_task, spdk_scsi_dev_queue_task 등. */
#include "spdk/scsi_spec.h"
/* [한국어] SCSI 표준 상수(SPDK_SCSI_STATUS_GOOD, SPDK_SPC_PROTOCOL_IDENTIFIER_SAS 등). */
#include "spdk/util.h"
/* [한국어] SPDK_CONTAINEROF, SPDK_COUNTOF 등 매크로. */
#include "spdk/likely.h"
/* [한국어] spdk_likely/spdk_unlikely 분기 힌트(HW 분기 예측 최적화). */

#include "spdk/vhost.h"
/* [한국어] vhost 공개 API (spdk_vhost_lock 등). */
#include "vhost_internal.h"
/* [한국어] vhost 내부 헤더 — spdk_vhost_dev/session/virtqueue + 헬퍼 함수. */

/* Features supported by SPDK VHOST lib. */
#define SPDK_VHOST_SCSI_FEATURES	(SPDK_VHOST_FEATURES | \
					(1ULL << VIRTIO_SCSI_F_INOUT) | \
					(1ULL << VIRTIO_SCSI_F_HOTPLUG) | \
					(1ULL << VIRTIO_SCSI_F_CHANGE ) | \
					(1ULL << VIRTIO_SCSI_F_T10_PI ))
/* [한국어] vhost-scsi가 게스트와 협상에서 광고하는 feature 비트맵.
 *  - SPDK_VHOST_FEATURES: 공통 vhost feature(VIRTIO_F_VERSION_1 등).
 *  - VIRTIO_SCSI_F_INOUT: 한 SCSI 요청에 read/write 데이터가 함께 있을 수 있음.
 *  - VIRTIO_SCSI_F_HOTPLUG: SCSI target hotplug/hotremove 이벤트 지원.
 *  - VIRTIO_SCSI_F_CHANGE: 디바이스 변경(LUN resize 등) 이벤트 지원.
 *  - VIRTIO_SCSI_F_T10_PI: T10 Protection Information 지원(현재는 광고만 함). */

/* Features that are specified in VIRTIO SCSI but currently not supported:
 * - Live migration not supported yet
 * - T10 PI
 */
#define SPDK_VHOST_SCSI_DISABLED_FEATURES	(SPDK_VHOST_DISABLED_FEATURES | \
						(1ULL << VIRTIO_SCSI_F_T10_PI ))
/* [한국어] 협상에서 비활성화하는 feature — 위에서 광고했지만 협상 시 disable.
 * T10 PI는 미구현이므로 게스트가 요청해도 거부. */

/* Vhost-user-scsi support protocol features */
#define SPDK_VHOST_SCSI_PROTOCOL_FEATURES	(1ULL << VHOST_USER_PROTOCOL_F_INFLIGHT_SHMFD)
/* [한국어] vhost-user 확장 프로토콜 feature.
 * INFLIGHT_SHMFD: 비정상 종료 후 재시작 시 inflight I/O를 공유 메모리(shmfd)로 보존하여 복구. */

#define MGMT_POLL_PERIOD_US (1000 * 5)
/* [한국어] 관리 큐(Control Q + Event Q) 폴링 주기(마이크로초) — 5ms.
 * I/O 큐(vdev_worker)는 무한 루프(주기 0)로 폴링하지만 관리 큐는 부담 줄이려 5ms 간격. */

#define VIRTIO_SCSI_CONTROLQ   0
/* [한국어] virtio-scsi의 Control Q 인덱스 — TMF(Task Management Function) 처리. */
#define VIRTIO_SCSI_EVENTQ   1
/* [한국어] virtio-scsi의 Event Q 인덱스 — hotplug/resize 이벤트 게스트로 통지. */
#define VIRTIO_SCSI_REQUESTQ   2
/* [한국어] 첫 번째 Request Q 인덱스 — I/O 요청 처리. multi-queue 시 [2..N] 사용. */

enum spdk_scsi_dev_vhost_status {
	/* Target ID is empty. */
	VHOST_SCSI_DEV_EMPTY,
	/* [한국어] target 슬롯이 비어 있음 — 새 target을 여기 추가 가능. */

	/* Target is still being added. */
	VHOST_SCSI_DEV_ADDING,
	/* [한국어] target 추가 진행 중 — foreach_session으로 모든 세션이 target을 인지하기까지 과도기. */

	/* Target ID occupied. */
	VHOST_SCSI_DEV_PRESENT,
	/* [한국어] target이 활성화되어 I/O 처리 가능 상태. */

	/* Target ID is occupied but removal is in progress. */
	VHOST_SCSI_DEV_REMOVING,
	/* [한국어] target 제거 진행 중 — pending I/O drain 대기. */

	/* In session - device (SCSI target) seen but removed. */
	VHOST_SCSI_DEV_REMOVED,
	/* [한국어] 세션 컨텍스트에서만 사용 — 디바이스에서 detach됐지만 hotremove sense 코드를 보내려고 보존. */
};

/** Context for a SCSI target in a vhost device */
struct spdk_scsi_dev_vhost_state {
	struct spdk_scsi_dev *dev;
	/* [한국어] SPDK SCSI 레이어의 target 객체.
	 * 설정자: spdk_vhost_scsi_dev_add_tgt → spdk_scsi_dev_construct_ext.
	 * 읽는 자: 모든 I/O/관리 경로가 SCSI 디스패치를 위해 참조.
	 * 동기화: 디바이스 thread 단일 접근(lock 불필요). NULL이면 빈 슬롯. */
	enum spdk_scsi_dev_vhost_status status;
	/* [한국어] 위 enum의 현재 상태 — target 라이프사이클 추적.
	 * 설정자: add_tgt/remove_tgt 경로에서 단계별 갱신. */
	spdk_vhost_event_fn remove_cb;
	/* [한국어] target 제거 완료 시 호출할 사용자 콜백 — RPC 비동기 응답 등에 사용.
	 * 설정자: spdk_vhost_scsi_dev_remove_tgt 호출 시점.
	 * 읽는 자: remove_scsi_tgt가 cleanup 마지막 단계에 호출. */
	void *remove_ctx;
	/* [한국어] remove_cb에 전달할 사용자 컨텍스트(보통 RPC request 핸들). */
};

struct spdk_vhost_scsi_dev {
	int ref;
	/* [한국어] 활성 target 참조 카운트.
	 * 설정자: add_tgt에서 +1, remove_scsi_tgt에서 --.
	 * 읽는 자: vhost_scsi_dev_remove가 0인지 검사 — 모든 target 제거 후에만 컨트롤러 해제. */
	bool registered;
	/* [한국어] true면 컨트롤러가 vhost-user listener로 등록되어 게스트 connect 가능.
	 * 설정자: vhost_scsi_dev_construct(delay=false) 또는 vhost_scsi_controller_start.
	 * 읽는 자: add_tgt가 활성 컨트롤러면 foreach_session으로 hotplug 이벤트 발행. */
	struct spdk_vhost_dev vdev;
	/* [한국어] vhost 공통 디바이스 구조체(이름/cpumask/소켓 경로 등).
	 * SPDK_CONTAINEROF로 vdev → spdk_vhost_scsi_dev 변환 가능. */
	struct spdk_scsi_dev_vhost_state scsi_dev_state[SPDK_VHOST_SCSI_CTRLR_MAX_DEVS];
	/* [한국어] 8개 target 슬롯 — 각 슬롯은 SPDK SCSI dev + 상태.
	 * 설정자: add_tgt/remove_tgt가 슬롯 변경.
	 * 읽는 자: I/O 처리 시 lun[1] 인덱스로 target 식별.
	 * 동기화: 디바이스 thread 단일 접근. */
};

/** Context for a SCSI target in a vhost session */
struct spdk_scsi_dev_session_state {
	struct spdk_scsi_dev *dev;
	/* [한국어] 이 세션이 인지하고 있는 target 포인터(컨트롤러의 dev 사본).
	 * 설정자: vhost_scsi_session_add_tgt가 컨트롤러로부터 복사.
	 * 읽는 자: I/O 핫패스가 LUN lookup 시 사용. NULL이면 이 세션은 해당 target에 I/O 못함. */
	enum spdk_scsi_dev_vhost_status status;
	/* [한국어] 세션 측 target 상태 — 컨트롤러 측과 별도로 추적해 hotremove 진행 중에도
	 * 게스트에 sense 코드를 보낼 수 있게 한다. */
};

struct spdk_vhost_scsi_session {
	struct spdk_vhost_session vsession;
	/* [한국어] vhost 세션 공통 구조체 — 게스트 메모리 매핑/virtqueue 배열/dpdk_sem 등.
	 * 항상 첫 멤버이므로 vsession ↔ scsi_session 캐스팅이 동일 주소. */

	struct spdk_vhost_scsi_dev *svdev;
	/* [한국어] 부모 컨트롤러 역참조 포인터.
	 * 설정자: 세션 생성 시 vhost.c가 vsession->vdev → svdev 변환해 저장.
	 * 읽는 자: 거의 모든 세션 핸들러가 컨트롤러 정보(target 슬롯 배열 등) 접근 시 사용. */
	/** Local copy of the device state */
	struct spdk_scsi_dev_session_state scsi_dev_state[SPDK_VHOST_SCSI_CTRLR_MAX_DEVS];
	/* [한국어] 컨트롤러의 target 상태를 세션 단위로 사본 보관 — 세션마다 hotplug 진행이 다를 수 있음.
	 * 설정자: vhost_scsi_session_add_tgt가 신규 target을 사본에 추가.
	 * 읽는 자: I/O 시 LUN lookup, mgmt poller가 hotremove drain 검사. */
	struct spdk_poller *requestq_poller;
	/* [한국어] vdev_worker(I/O 요청 큐 폴러)의 spdk_poller 핸들.
	 * 설정자: vhost_scsi_start에서 spdk_poller_register 결과.
	 * 해제: vhost_scsi_stop에서 spdk_poller_unregister. */
	struct spdk_poller *mgmt_poller;
	/* [한국어] vdev_mgmt_worker(Control + Event Q 폴러)의 핸들.
	 * 5ms(MGMT_POLL_PERIOD_US) 주기로 호출. */
	struct spdk_poller *stop_poller;
	/* [한국어] 세션 stop 시 task_cnt가 0이 될 때까지 대기하는 폴러 핸들.
	 * 설정자: vhost_scsi_stop의 poller 등록. */
};

struct spdk_vhost_scsi_task {
	struct spdk_scsi_task	scsi;
	/* [한국어] SPDK SCSI 레이어가 사용하는 task 구조체.
	 * spdk_scsi_dev_queue_task에 전달 — CDB/LUN/iovs/완료 콜백 보유.
	 * SPDK_CONTAINEROF로 scsi → spdk_vhost_scsi_task 변환. */
	struct iovec iovs[SPDK_VHOST_IOVS_MAX];
	/* [한국어] 게스트 데이터 버퍼들의 호스트 측 iovec 배열(GPA→VVA 변환된 결과).
	 * 설정자: task_data_setup이 desc chain 순회하며 채움.
	 * 읽는 자: spdk_scsi_dev_queue_task → bdev I/O 발급. */

	union {
		struct virtio_scsi_cmd_resp *resp;
		/* [한국어] 일반 SCSI 명령(REQUESTQ)의 응답 구조체 포인터.
		 * 설정자: task_data_setup이 응답 desc의 GPA → VVA 변환.
		 * 읽는 자: vhost_scsi_task_cpl이 status/sense/resid 채움. */
		struct virtio_scsi_ctrl_tmf_resp *tmf_resp;
		/* [한국어] Task Management Function 응답 구조체 포인터(CONTROLQ TMF용).
		 * resp/tmf_resp는 union — 큐 종류에 따라 둘 중 하나만 사용. */
	};

	struct spdk_vhost_scsi_session *svsession;
	/* [한국어] 이 task가 속한 세션 역참조. cb_fn에서 vsession 정보 획득. */
	struct spdk_scsi_dev *scsi_dev;
	/* [한국어] 이 task가 향할 SCSI target. vhost_scsi_task_init_target이 LUN 파싱 후 설정. */

	/** Number of bytes that were written. */
	uint32_t used_len;
	/* [한국어] used ring에 보고할 "device가 쓴 바이트 수" — 응답 구조체 크기 + 데이터 페이로드.
	 * 설정자: task_data_setup에서 sizeof(resp) + 데이터 길이로 계산.
	 * 읽는 자: submit_completion이 vhost_vq_used_ring_enqueue 호출 시 사용. */

	int req_idx;
	/* [한국어] avail ring에서 가져온 desc head 인덱스 — used ring enqueue 시 같은 값을 보고.
	 * 설정자: process_scsi_task에서 큐 task pool 인덱스로 함께 설정. */

	/* If set, the task is currently used for I/O processing. */
	bool used;
	/* [한국어] 현재 task pool 슬롯이 사용 중인지 표시.
	 * 설정자: scsi_task_init에서 true, vhost_scsi_task_free_cb에서 false.
	 * 읽는 자: process_scsi_task가 동일 인덱스 재사용 중인지 검사 — 게스트 오작동 방어. */

	struct spdk_vhost_virtqueue *vq;
	/* [한국어] 이 task가 속한 virtqueue — 완료 시 used ring enqueue 대상. */
};

/* [한국어] 아래는 backend vtable에서 참조되는 콜백들의 forward declaration —
 * 정의는 같은 파일 후반부에 위치한다. */
static int vhost_scsi_start(struct spdk_vhost_dev *vdev,
			    struct spdk_vhost_session *vsession, void *unused);
/* [한국어] 세션 시작 콜백 — 세션이 connect 되었을 때 poller 등록 등 셋업 수행. */
static int vhost_scsi_stop(struct spdk_vhost_dev *vdev,
			   struct spdk_vhost_session *vsession, void *unused);
/* [한국어] 세션 종료 콜백 — drain + poller 해제. */
static void vhost_scsi_dump_info_json(struct spdk_vhost_dev *vdev,
				      struct spdk_json_write_ctx *w);
/* [한국어] vhost_get_controllers RPC 응답에서 backend_specific 필드 채우는 콜백. */
static void vhost_scsi_write_config_json(struct spdk_vhost_dev *vdev,
		struct spdk_json_write_ctx *w);
/* [한국어] save_config 시 컨트롤러 재구성용 RPC 시퀀스 출력. */
static int vhost_scsi_dev_remove(struct spdk_vhost_dev *vdev);
/* [한국어] 컨트롤러 삭제 콜백 — 모든 target 강제 제거 후 vdev unregister. */
static int vhost_scsi_dev_param_changed(struct spdk_vhost_dev *vdev,
					unsigned scsi_tgt_num);
/* [한국어] target 파라미터 변경(LUN resize 등) 시 게스트에 PARAM_CHANGE 이벤트 발행. */
static int alloc_vq_task_pool(struct spdk_vhost_session *vsession, uint16_t qid);
/* [한국어] 큐 활성화 시 큐별 task pool(spdk_vhost_scsi_task 배열)을 할당. */

static const struct spdk_vhost_user_dev_backend spdk_vhost_scsi_user_device_backend = {
	.session_ctx_size = sizeof(struct spdk_vhost_scsi_session) - sizeof(struct spdk_vhost_session),
	/* [한국어] 세션 컨테이너 추가 크기 — vhost.c가 base 구조체 뒤에 이 크기만큼 더 할당 후 cast. */
	.start_session =  vhost_scsi_start,
	/* [한국어] 세션 시작 시 호출. */
	.stop_session = vhost_scsi_stop,
	/* [한국어] 세션 종료 시 호출. */
	.alloc_vq_tasks = alloc_vq_task_pool,
	/* [한국어] 큐 활성화 시 task pool 할당. */
};
/* [한국어] vhost-user 단계의 백엔드 콜백 vtable — vhost_dev_register에서 등록. */

static const struct spdk_vhost_dev_backend spdk_vhost_scsi_device_backend = {
	.type = VHOST_BACKEND_SCSI,
	/* [한국어] 백엔드 타입 — to_scsi_dev 등에서 분기 검증. */
	.dump_info_json = vhost_scsi_dump_info_json,
	/* [한국어] RPC 응답용 정보 출력. */
	.write_config_json = vhost_scsi_write_config_json,
	/* [한국어] save_config용. */
	.remove_device = vhost_scsi_dev_remove,
	/* [한국어] 컨트롤러 제거 콜백. */
	.set_coalescing = vhost_user_set_coalescing,
	/* [한국어] coalescing 설정 — vhost-user 공통 함수 그대로 사용. */
	.get_coalescing = vhost_user_get_coalescing,
	/* [한국어] coalescing 조회. */
};
/* [한국어] 백엔드 공통 vtable. */

/*
 * [한국어]
 * scsi_task_init - 한 task 슬롯을 새 요청 처리용으로 초기화.
 *
 * @task: 사용 가능한 task 슬롯 포인터.
 *
 * task pool에서 꺼낸 task의 scsi 필드를 0으로 초기화하고, resp/tmf_resp(union)을 NULL로 비운 뒤
 * used=true로 마크해 동일 인덱스 재사용을 차단.
 *
 * 호출 컨텍스트: 디바이스 thread (process_scsi_task에서 매 요청마다).
 */
static inline void
scsi_task_init(struct spdk_vhost_scsi_task *task)
{
	memset(&task->scsi, 0, sizeof(task->scsi));
	/* [한국어] SPDK SCSI task 내부를 0으로 클리어 — sense_data_len, status 등 모두 리셋. */
	/* Tmf_resp pointer and resp pointer are in a union.
	 * Here means task->tmf_resp = task->resp = NULL.
	 */
	task->resp = NULL;
	/* [한국어] union이므로 resp=NULL이 곧 tmf_resp=NULL. */
	task->used = true;
	/* [한국어] 슬롯 사용 중 표시 — 게스트가 같은 인덱스로 또 enqueue하면 검출. */
	task->used_len = 0;
	/* [한국어] used ring에 보고할 길이 초기화. */
}

/*
 * [한국어]
 * vhost_scsi_task_put - SPDK SCSI 레이어에 task 반환(완료 처리).
 *
 * @task: 완료된 task.
 *
 * spdk_scsi_task_put이 ref counting 후 0이 되면 vhost_scsi_task_free_cb를 호출 →
 * task_cnt-- 및 used=false 처리.
 */
static void
vhost_scsi_task_put(struct spdk_vhost_scsi_task *task)
{
	spdk_scsi_task_put(&task->scsi);
	/* [한국어] SCSI 레이어 ref 감소 — 0이 되면 free_fn(=vhost_scsi_task_free_cb)이 호출됨. */
}

/*
 * [한국어]
 * vhost_scsi_task_free_cb - SCSI task가 마지막으로 풀려날 때 SCSI 레이어가 호출하는 free 콜백.
 *
 * @scsi_task: 완료된 SCSI task(SPDK 레이어 관점).
 *
 * SPDK_CONTAINEROF로 vhost task로 변환 후, vsession의 task_cnt를 감소.
 * task_cnt가 0이 되면 stop poller가 cleanup을 진행할 수 있다.
 *
 * 호출 컨텍스트: 디바이스 thread (SCSI 레이어 완료 콜백).
 */
static void
vhost_scsi_task_free_cb(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_scsi_task *task = SPDK_CONTAINEROF(scsi_task, struct spdk_vhost_scsi_task, scsi);
	/* [한국어] scsi 멤버 주소 → vhost task 컨테이너 주소 복원. */
	struct spdk_vhost_session *vsession = &task->svsession->vsession;
	/* [한국어] 부모 세션 포인터. */

	assert(vsession->task_cnt > 0);
	/* [한국어] 음수로 빠지지 않도록 가드 — 버그 조기 발견. */
	vsession->task_cnt--;
	/* [한국어] 진행 중 task 카운터 감소 — 0이 되면 stop 가능. */
	task->used = false;
	/* [한국어] 슬롯 free 마킹 — 다음 요청에서 재사용 가능. */
}

/*
 * [한국어]
 * vhost_scsi_dev_unregister - 컨트롤러 unregister + 메모리 해제 (지연 실행 콜백).
 *
 * @arg1: spdk_vhost_scsi_dev*.
 *
 * remove_scsi_tgt가 vhost-user 락 보유 중에 호출될 수 있어 즉시 unregister 못함.
 * spdk_thread_send_msg로 다음 polling 사이클에 enqueue되어 락 풀린 뒤 안전하게 실행.
 */
static void
vhost_scsi_dev_unregister(void *arg1)
{
	struct spdk_vhost_scsi_dev *svdev = arg1;
	/* [한국어] arg → svdev 캐스팅. */

	if (vhost_dev_unregister(&svdev->vdev) == 0) {
		/* [한국어] 디바이스 unregister 성공 시(busy 아님) 메모리 해제. */
		free(svdev);
		/* [한국어] svdev 자체 free — 컨트롤러 리소스 완전 회수. */
	}
}

/*
 * [한국어]
 * remove_scsi_tgt - SCSI target을 컨트롤러에서 최종 제거.
 *
 * @svdev: 대상 컨트롤러.
 * @scsi_tgt_num: 제거할 target 번호 [0, 8).
 *
 * 모든 세션이 해당 target에서 detach된 후에 호출되며, SCSI dev 객체를 destruct하고
 * remove_cb(보통 RPC 응답)를 호출한다. target 제거 후 ref==0 + registered==false면
 * 컨트롤러 자체도 제거 큐에 enqueue.
 *
 * 호출 컨텍스트: 디바이스 thread, vhost-user 락 보유 중.
 *
 * 호출 체인:
 *   process_removed_devs/직접 → remove_scsi_tgt → spdk_scsi_dev_destruct → state->remove_cb
 */
static void
remove_scsi_tgt(struct spdk_vhost_scsi_dev *svdev,
		unsigned scsi_tgt_num)
{
	struct spdk_scsi_dev_vhost_state *state;
	/* [한국어] target 슬롯 메타. */
	struct spdk_scsi_dev *dev;
	/* [한국어] 제거할 SCSI dev 임시 저장. */

	state = &svdev->scsi_dev_state[scsi_tgt_num];
	/* [한국어] 슬롯 위치 획득. */
	dev = state->dev;
	/* [한국어] dev 보존 후 슬롯에서 분리 — destruct 호출 사이 다른 경로의 접근 방지. */
	state->dev = NULL;
	/* [한국어] 슬롯에서 dev 제거 — 이후 lookup이 NULL을 받음. */
	assert(state->status == VHOST_SCSI_DEV_REMOVING);
	/* [한국어] 상태 가드 — REMOVING 상태가 아니면 잘못된 호출. */
	state->status = VHOST_SCSI_DEV_EMPTY;
	/* [한국어] 슬롯 비움 표시 — 새 target 추가 가능. */
	spdk_scsi_dev_destruct(dev, NULL, NULL);
	/* [한국어] SPDK SCSI 레이어에서 dev 해제 (LUN/포트 등 정리). 콜백 NULL = 동기 완료 가정. */
	if (state->remove_cb) {
		/* [한국어] 사용자(보통 RPC)가 등록한 완료 콜백 호출. */
		state->remove_cb(&svdev->vdev, state->remove_ctx);
		/* [한국어] vdev + 사용자 컨텍스트 전달 — RPC 핸들러가 응답 발사. */
		state->remove_cb = NULL;
		/* [한국어] 중복 호출 방지. */
	}
	SPDK_INFOLOG(vhost, "removed target 'Target %u'\n", scsi_tgt_num);
	/* [한국어] 정보 로그. */

	if (--svdev->ref == 0 && svdev->registered == false) {
		/* [한국어] 활성 target ref가 0이 되고 컨트롤러도 unregister 요청 상태이면 컨트롤러 자체 제거. */
		/* `remove_scsi_tgt` is running under vhost-user lock, so we
		 * unregister the device in next poll.
		 */
		spdk_thread_send_msg(spdk_get_thread(), vhost_scsi_dev_unregister, svdev);
		/* [한국어] 락 풀린 뒤 안전하게 unregister 하도록 메시지 큐로 위임. */
	}
}

/*
 * [한국어]
 * vhost_scsi_dev_process_removed_cpl_cb - 모든 세션의 detach 처리 완료 후 컨트롤러 측 cleanup 콜백.
 *
 * @vdev: 컨트롤러 vdev.
 * @ctx: scsi_tgt_num을 uintptr_t로 인코딩한 값.
 *
 * vhost_user_dev_foreach_session의 완료 콜백 — 모든 세션 detach 후 호출.
 * 다른 경로에서 이미 제거됐을 수 있으므로 status를 다시 확인.
 */
static void
vhost_scsi_dev_process_removed_cpl_cb(struct spdk_vhost_dev *vdev, void *ctx)
{
	unsigned scsi_tgt_num = (unsigned)(uintptr_t)ctx;
	/* [한국어] 포인터로 인코딩된 정수 디코딩. */
	struct spdk_vhost_scsi_dev *svdev = SPDK_CONTAINEROF(vdev,
					    struct spdk_vhost_scsi_dev, vdev);
	/* [한국어] vdev → svdev 변환. */

	/* all sessions have already detached the device */
	if (svdev->scsi_dev_state[scsi_tgt_num].status != VHOST_SCSI_DEV_REMOVING) {
		/* device was already removed in the meantime */
		return;
		/* [한국어] 이미 제거됐으면 중복 처리 방지하고 리턴. */
	}

	remove_scsi_tgt(svdev, scsi_tgt_num);
	/* [한국어] 컨트롤러 측 최종 제거 실행. */
}

/*
 * [한국어]
 * vhost_scsi_session_process_removed - 한 세션이 target에서 detach됐는지 확인하는 foreach 콜백.
 *
 * @vdev: 컨트롤러 vdev.
 * @vsession: 검사할 세션.
 * @ctx: scsi_tgt_num을 uintptr_t로 인코딩.
 * @return: 0이면 계속 순회, -1이면 중단(아직 detach 안 된 세션 발견).
 *
 * 호출 컨텍스트: 각 세션의 lcore (foreach가 cross-thread 메시지로 분배).
 */
static int
vhost_scsi_session_process_removed(struct spdk_vhost_dev *vdev,
				   struct spdk_vhost_session *vsession, void *ctx)
{
	unsigned scsi_tgt_num = (unsigned)(uintptr_t)ctx;
	/* [한국어] target 번호 디코딩. */
	struct spdk_vhost_scsi_session *svsession = (struct spdk_vhost_scsi_session *)vsession;
	/* [한국어] base → derived 캐스팅. */
	struct spdk_scsi_dev_session_state *state = &svsession->scsi_dev_state[scsi_tgt_num];
	/* [한국어] 이 세션의 target 사본 슬롯. */

	if (state->dev != NULL) {
		/* there's still a session that references this device,
		 * so abort our foreach chain here. We'll be called
		 * again from this session's management poller after it
		 * is removed in there
		 */
		return -1;
		/* [한국어] 아직 detach 안 됐으므로 순회 중단 — 해당 세션이 mgmt poller에서 detach 후 다시 시도. */
	}

	return 0;
	/* [한국어] 이 세션은 이미 detach 완료 — 계속 다음 세션 검사. */
}

/*
 * [한국어]
 * process_removed_devs - 한 세션이 가진 target들 중 REMOVING 상태이고 pending I/O 없는 target detach.
 *
 * @svsession: 검사할 세션.
 *
 * 매 mgmt poller 사이클(vdev_mgmt_worker)마다 호출되어, REMOVING 상태이고
 * spdk_scsi_dev_has_pending_tasks가 false인 target은 io_channel 해제 후 슬롯에서 분리한다.
 * detach 후 다른 모든 세션도 detach 됐는지 foreach_session으로 확인.
 *
 * 호출 컨텍스트: 디바이스 thread (mgmt poller).
 */
static void
process_removed_devs(struct spdk_vhost_scsi_session *svsession)
{
	struct spdk_scsi_dev *dev;
	/* [한국어] 검사 중인 dev 임시 저장. */
	struct spdk_scsi_dev_session_state *state;
	/* [한국어] 슬롯 메타. */
	int i;
	/* [한국어] 슬롯 인덱스. */

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; ++i) {
		/* [한국어] 8개 슬롯 모두 검사. */
		state = &svsession->scsi_dev_state[i];
		dev = state->dev;

		if (dev && state->status == VHOST_SCSI_DEV_REMOVING &&
		    !spdk_scsi_dev_has_pending_tasks(dev, NULL)) {
			/* [한국어] REMOVING 상태이고 미완료 task 0개일 때만 detach 진행. */
			/* detach the device from this session */
			spdk_scsi_dev_free_io_channels(dev);
			/* [한국어] 세션 측 io_channel 해제 — 더 이상 이 세션에서 dev 접근 안 함. */
			state->dev = NULL;
			/* [한국어] 슬롯에서 dev 분리. */
			state->status = VHOST_SCSI_DEV_REMOVED;
			/* [한국어] 세션 상태를 REMOVED로 — 게스트에 hotremove sense 보낼 수 있도록 보존. */
			/* try to detach it globally */
			vhost_user_dev_foreach_session(&svsession->svdev->vdev,
						       vhost_scsi_session_process_removed,
						       vhost_scsi_dev_process_removed_cpl_cb,
						       (void *)(uintptr_t)i);
			/* [한국어] 모든 세션 순회로 글로벌 detach 완료 여부 확인 → 완료 시 컨트롤러 측 제거. */
		}
	}
}

/*
 * [한국어]
 * eventq_enqueue - virtio-scsi Event Q에 이벤트 메시지 enqueue (게스트로 hotplug 등 통지).
 *
 * @svsession: 대상 세션.
 * @scsi_dev_num: 이벤트 발생 target 번호.
 * @event: 이벤트 종류 (VIRTIO_SCSI_T_TRANSPORT_RESET 등).
 * @reason: 사유 (VIRTIO_SCSI_EVT_RESET_RESCAN 등).
 *
 * Event Q의 avail ring에서 desc 1개를 꺼내 게스트 메모리에 virtio_scsi_event 구조를 작성한 뒤,
 * used ring에 enqueue하여 게스트가 IRQ로 인지하도록 한다. LUN 인코딩은 virtio-scsi 형식:
 * lun[0]=1(고정), lun[1]=target_id, lun[2..3]=LUN id(0).
 *
 * 호출 컨텍스트: 디바이스 thread (vhost_scsi_session_add_tgt 등).
 */
static void
eventq_enqueue(struct spdk_vhost_scsi_session *svsession, unsigned scsi_dev_num,
	       uint32_t event, uint32_t reason)
{
	struct spdk_vhost_session *vsession = &svsession->vsession;
	/* [한국어] base session 포인터. */
	struct spdk_vhost_virtqueue *vq;
	/* [한국어] 대상 큐(Event Q). */
	struct vring_desc *desc, *desc_table;
	/* [한국어] 가져온 descriptor와 그 desc table. */
	struct virtio_scsi_event *desc_ev;
	/* [한국어] desc가 가리키는 게스트 메모리에 매핑된 이벤트 구조체. */
	uint32_t desc_table_size, req_size = 0;
	/* [한국어] desc table 크기, used ring에 보고할 길이(에러시 0). */
	uint16_t req;
	/* [한국어] avail ring에서 받은 desc head 인덱스. */
	int rc;
	/* [한국어] 헬퍼 반환값. */

	assert(scsi_dev_num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS);
	/* [한국어] target 번호 범위 가드. */
	vq = &vsession->virtqueue[VIRTIO_SCSI_EVENTQ];
	/* [한국어] EventQ는 큐 인덱스 1 (고정). */

	if (vq->vring.desc == NULL || vhost_vq_avail_ring_get(vq, &req, 1) != 1) {
		/* [한국어] 큐 미초기화 또는 avail에 빈 슬롯 없음 — 게스트가 EventQ에 desc를 충분히 안 넣은 경우. */
		SPDK_ERRLOG("%s: failed to send virtio event (no avail ring entries?).\n",
			    vsession->name);
		return;
		/* [한국어] 이벤트 전달 실패 — 게스트는 사후 rescan 등으로 인지해야 함. */
	}

	rc = vhost_vq_get_desc(vsession, vq, req, &desc, &desc_table, &desc_table_size);
	/* [한국어] req 인덱스의 desc(체인 시작)와 desc table 획득. */
	if (rc != 0 || desc->len < sizeof(*desc_ev)) {
		/* [한국어] desc 무효 또는 길이가 이벤트 구조체보다 작음 → 폐기. */
		SPDK_ERRLOG("%s: invalid eventq descriptor at index %"PRIu16".\n",
			    vsession->name, req);
		goto out;
	}

	desc_ev = vhost_gpa_to_vva(vsession, desc->addr, sizeof(*desc_ev));
	/* [한국어] 게스트 GPA → 호스트 VVA 변환. NULL이면 매핑 영역 밖. */
	if (desc_ev == NULL) {
		SPDK_ERRLOG("%s: eventq descriptor at index %"PRIu16" points "
			    "to unmapped guest memory address %p.\n",
			    vsession->name, req, (void *)(uintptr_t)desc->addr);
		goto out;
	}

	desc_ev->event = event;
	/* [한국어] 이벤트 타입 기록 (게스트가 read). */
	desc_ev->lun[0] = 1;
	/* [한국어] virtio-scsi LUN 인코딩 byte 0: 항상 1 (스펙). */
	desc_ev->lun[1] = scsi_dev_num;
	/* [한국어] target id (lun[1]). */
	/* virtio LUN id 0 can refer either to the entire device
	 * or actual LUN 0 (the only supported by vhost for now)
	 */
	desc_ev->lun[2] = 0 >> 8;
	/* [한국어] LUN id 상위 바이트(=0). 항상 0인 것을 의도적으로 명시. */
	desc_ev->lun[3] = 0 & 0xFF;
	/* [한국어] LUN id 하위 바이트(=0). */
	/* virtio doesn't specify any strict format for LUN id (bytes 2 and 3)
	 * current implementation relies on linux kernel sources
	 */
	memset(&desc_ev->lun[4], 0, 4);
	/* [한국어] 나머지 4바이트(reserved) 0으로 클리어. */
	desc_ev->reason = reason;
	/* [한국어] 이벤트 사유 코드. */
	req_size = sizeof(*desc_ev);
	/* [한국어] 정상 작성 시 used에 보고할 길이. */

out:
	vhost_vq_used_ring_enqueue(vsession, vq, req, req_size);
	/* [한국어] used ring enqueue — 에러 경로면 req_size=0으로 desc만 회수. */
}

/*
 * [한국어]
 * submit_completion - 완료된 task를 used ring에 enqueue + task pool 반환.
 *
 * @task: 완료된 SCSI task.
 *
 * 두 가지 완료 콜백(vhost_scsi_task_cpl/_mgmt_cpl)에서 공통 호출.
 * vhost_vq_used_ring_enqueue가 used 슬롯을 채우고 used_idx 갱신 — 다음 used_signal에서 IRQ 발사.
 */
static void
submit_completion(struct spdk_vhost_scsi_task *task)
{
	struct spdk_vhost_session *vsession = &task->svsession->vsession;
	/* [한국어] base 세션. */

	vhost_vq_used_ring_enqueue(vsession, task->vq, task->req_idx,
				   task->used_len);
	/* [한국어] used ring에 (req_idx, used_len) 기록. */
	SPDK_DEBUGLOG(vhost_scsi, "Finished task (%p) req_idx=%d\n", task, task->req_idx);
	/* [한국어] 디버그 로그. */

	vhost_scsi_task_put(task);
	/* [한국어] task ref 감소 — 0 도달 시 free_cb 호출 → task_cnt--. */
}

/*
 * [한국어]
 * vhost_scsi_task_mgmt_cpl - TMF(Control Q) task 완료 콜백.
 *
 * @scsi_task: SPDK SCSI 레이어가 콜백 호출 시 전달하는 task.
 *
 * 일반 SCSI 명령과 달리 TMF는 status/sense를 응답에 채우지 않으므로 단순히 used ring에 enqueue만.
 *
 * 호출 컨텍스트: 디바이스 thread (SCSI 레이어 완료 콜백).
 */
static void
vhost_scsi_task_mgmt_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_scsi_task *task = SPDK_CONTAINEROF(scsi_task, struct spdk_vhost_scsi_task, scsi);
	/* [한국어] scsi → vhost task 변환. */

	submit_completion(task);
	/* [한국어] used ring enqueue + task put. */
}

/*
 * [한국어]
 * vhost_scsi_task_cpl - 일반 SCSI 명령(Request Q) task 완료 콜백.
 *
 * @scsi_task: 완료된 SCSI task.
 *
 * SCSI 결과(status/sense_data/data_transferred)를 virtio-scsi 응답 구조체에 채운 뒤
 * used ring에 enqueue. status가 GOOD이 아니면 sense 데이터도 복사.
 */
static void
vhost_scsi_task_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_scsi_task *task = SPDK_CONTAINEROF(scsi_task, struct spdk_vhost_scsi_task, scsi);
	/* [한국어] scsi → vhost task. */

	/* The SCSI task has completed.  Do final processing and then post
	   notification to the virtqueue's "used" ring.
	 */
	task->resp->status = task->scsi.status;
	/* [한국어] SCSI status를 virtio 응답에 복사. */

	if (task->scsi.status != SPDK_SCSI_STATUS_GOOD) {
		/* [한국어] 에러 상태 — sense 데이터도 응답에 포함. */
		memcpy(task->resp->sense, task->scsi.sense_data, task->scsi.sense_data_len);
		/* [한국어] sense byte 배열 복사. */
		task->resp->sense_len = task->scsi.sense_data_len;
		/* [한국어] sense 길이 기록. */
		SPDK_DEBUGLOG(vhost_scsi, "Task (%p) req_idx=%d failed - status=%u\n", task, task->req_idx,
			      task->scsi.status);
	}
	assert(task->scsi.transfer_len == task->scsi.length);
	/* [한국어] 요청 길이와 전달 길이 일치 가드 — 부분 전송은 별도 경로. */
	task->resp->resid = task->scsi.length - task->scsi.data_transferred;
	/* [한국어] residual count(요청-실제 전송) — 항상 0이 정상이지만 전송 실패 시 잔여 길이. */

	submit_completion(task);
	/* [한국어] used ring enqueue + put. */
}

/*
 * [한국어]
 * task_submit - 일반 SCSI 명령을 SPDK SCSI 레이어에 발급.
 *
 * @task: 처리할 task (resp/cdb/iovs 모두 셋업 완료된 상태).
 *
 * resp의 response 필드를 OK로 초기화한 뒤 spdk_scsi_dev_queue_task 호출.
 * 비동기 — 완료 시 vhost_scsi_task_cpl이 호출됨.
 */
static void
task_submit(struct spdk_vhost_scsi_task *task)
{
	task->resp->response = VIRTIO_SCSI_S_OK;
	/* [한국어] virtio-scsi response 코드 OK로 초기화 (실제 SCSI status는 별도 필드). */
	spdk_scsi_dev_queue_task(task->scsi_dev, &task->scsi);
	/* [한국어] SPDK SCSI 레이어로 발급 — LUN을 거쳐 bdev I/O로 변환됨. */
}

/*
 * [한국어]
 * mgmt_task_submit - TMF(Control Q) 명령을 SCSI 레이어에 발급.
 *
 * @task: TMF task.
 * @func: SCSI task function 종류 (SPDK_SCSI_TASK_FUNC_LUN_RESET 등).
 *
 * 일반 명령과 달리 spdk_scsi_dev_queue_mgmt_task로 별도 큐에 enqueue.
 */
static void
mgmt_task_submit(struct spdk_vhost_scsi_task *task, enum spdk_scsi_task_func func)
{
	task->tmf_resp->response = VIRTIO_SCSI_S_OK;
	/* [한국어] TMF response 초기화. */
	task->scsi.function = func;
	/* [한국어] LUN_RESET 등 TMF 함수 타입 지정. */
	spdk_scsi_dev_queue_mgmt_task(task->scsi_dev, &task->scsi);
	/* [한국어] SCSI 관리 큐로 발급 — 데이터 I/O와 분리해 빠르게 처리. */
}

/*
 * [한국어]
 * invalid_request - 잘못된 요청을 used ring에 빈 응답으로 회수.
 *
 * @task: 무효 요청 task.
 *
 * 게스트 desc가 손상됐거나 LUN이 없는 경우 호출. used_len은 task에 이미 설정된 값(0 또는 응답 크기) 사용.
 */
static void
invalid_request(struct spdk_vhost_scsi_task *task)
{
	struct spdk_vhost_session *vsession = &task->svsession->vsession;
	/* [한국어] base 세션. */

	vhost_vq_used_ring_enqueue(vsession, task->vq, task->req_idx,
				   task->used_len);
	/* [한국어] desc 회수 — 게스트가 본 used에 잘못된 응답이 보일 수 있음. */
	vhost_scsi_task_put(task);
	/* [한국어] task put — task_cnt--. */

	SPDK_DEBUGLOG(vhost_scsi, "Invalid request (status=%" PRIu8")\n",
		      task->resp ? task->resp->response : -1);
}

/*
 * [한국어]
 * vhost_scsi_task_init_target - virtio-scsi LUN 8바이트를 파싱해 task에 SCSI dev/LUN 매핑.
 *
 * @task: 대상 task.
 * @lun: virtio-scsi LUN 8바이트 (game LUN 인코딩).
 * @return: 0=정상, -1=완전히 잘못된 요청(즉시 폐기).
 *
 * virtio-scsi LUN 형식:
 *   lun[0]=1 (고정), lun[1]=target_id, lun[2..3]=LUN id, lun[4..7]=reserved.
 *
 * REMOVED 상태(hotremove 진행)이면 task_init은 성공시켜 hotremove sense를 회신할 수 있게 한다.
 * EMPTY(아예 없음)면 -1로 invalid 처리.
 */
static int
vhost_scsi_task_init_target(struct spdk_vhost_scsi_task *task, const __u8 *lun)
{
	struct spdk_vhost_scsi_session *svsession = task->svsession;
	/* [한국어] 세션. */
	struct spdk_scsi_dev_session_state *state;
	/* [한국어] target 슬롯 메타. */
	uint16_t lun_id = (((uint16_t)lun[2] << 8) | lun[3]) & 0x3FFF;
	/* [한국어] LUN id 추출 — 상위 2비트는 LUN id 종류 표시(스펙)이므로 마스크. */

	SPDK_LOGDUMP(vhost_scsi_queue, "LUN", lun, 8);
	/* [한국어] LUN raw 8바이트를 디버그 덤프. */

	/* First byte must be 1 and second is target */
	if (lun[0] != 1 || lun[1] >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		/* [한국어] 형식 검증 실패 — 게스트 측 SW 버그/공격. */
		return -1;
	}

	state = &svsession->scsi_dev_state[lun[1]];
	/* [한국어] target 슬롯 lookup. */
	task->scsi_dev = state->dev;
	/* [한국어] task에 SCSI dev 포인터 저장 (NULL일 수 있음 — 아래에서 처리). */
	if (state->dev == NULL || state->status != VHOST_SCSI_DEV_PRESENT) {
		/* If dev has been hotdetached, return 0 to allow sending
		 * additional hotremove event via sense codes.
		 */
		return state->status != VHOST_SCSI_DEV_EMPTY ? 0 : -1;
		/* [한국어] EMPTY: -1(폐기), REMOVED 등: 0(LUN NULL이지만 hotremove sense 응답 가능). */
	}

	task->scsi.target_port = spdk_scsi_dev_find_port_by_id(task->scsi_dev, 0);
	/* [한국어] target port id=0 lookup (vhost는 단일 port 사용). */
	task->scsi.lun = spdk_scsi_dev_get_lun(state->dev, lun_id);
	/* [한국어] LUN id로 LUN 객체 lookup — 없으면 NULL로 process_request가 NULL_LUN 응답. */
	return 0;
}

/*
 * [한국어]
 * process_ctrl_request - virtio-scsi Control Q에서 꺼낸 TMF/AN 요청 처리.
 *
 * @task: Control Q에서 만들어진 task.
 *
 * 처리 흐름:
 *   1) desc chain 첫 desc → 게스트 ctrl_req 매핑.
 *   2) LUN 파싱(target_init).
 *   3) 다음 desc → 응답 버퍼(virtio_scsi_ctrl_tmf_resp 등) 매핑.
 *   4) ctrl_req->type 분기:
 *      - VIRTIO_SCSI_T_TMF + LOGICAL_UNIT_RESET: SCSI mgmt task 발급(비동기, 완료 시 ctrl_cpl).
 *      - 미지원 TMF subtype: ABORTED 응답 + 즉시 회수.
 *      - VIRTIO_SCSI_T_AN_QUERY/SUBSCRIBE: ABORTED (현재 미지원).
 *      - 기타: 무시.
 */
static void
process_ctrl_request(struct spdk_vhost_scsi_task *task)
{
	struct spdk_vhost_session *vsession = &task->svsession->vsession;
	/* [한국어] base 세션. */
	struct vring_desc *desc, *desc_table;
	/* [한국어] desc 체인 순회용. */
	struct virtio_scsi_ctrl_tmf_req *ctrl_req;
	/* [한국어] TMF 요청 구조체 포인터(게스트 메모리 매핑). */
	struct virtio_scsi_ctrl_an_resp *an_resp;
	/* [한국어] Asynchronous Notification 응답 구조체 포인터. */
	uint32_t desc_table_size, used_len = 0;
	/* [한국어] desc table 크기, used에 보고할 길이(0=에러). */
	int rc;
	/* [한국어] 헬퍼 반환값. */

	spdk_scsi_task_construct(&task->scsi, vhost_scsi_task_mgmt_cpl, vhost_scsi_task_free_cb);
	/* [한국어] SPDK SCSI task 초기화 — 완료 콜백/free 콜백 등록. */
	rc = vhost_vq_get_desc(vsession, task->vq, task->req_idx, &desc, &desc_table,
			       &desc_table_size);
	/* [한국어] req_idx 위치의 desc(체인 head) 획득. */
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("%s: invalid controlq descriptor at index %d.\n",
			    vsession->name, task->req_idx);
		goto out;
	}

	ctrl_req = vhost_gpa_to_vva(vsession, desc->addr, sizeof(*ctrl_req));
	/* [한국어] 첫 desc(요청 buf) → ctrl_req 매핑. */
	if (ctrl_req == NULL) {
		SPDK_ERRLOG("%s: invalid task management request at index %d.\n",
			    vsession->name, task->req_idx);
		goto out;
	}

	SPDK_DEBUGLOG(vhost_scsi_queue,
		      "Processing controlq descriptor: desc %d/%p, desc_addr %p, len %d, flags %d, last_used_idx %d; kickfd %d; size %d\n",
		      task->req_idx, desc, (void *)desc->addr, desc->len, desc->flags, task->vq->last_used_idx,
		      task->vq->vring.kickfd, task->vq->vring.size);
	SPDK_LOGDUMP(vhost_scsi_queue, "Request descriptor", (uint8_t *)ctrl_req, desc->len);
	/* [한국어] 디버그 덤프 — vhost_scsi_queue 컴포넌트 활성화 시만. */

	vhost_scsi_task_init_target(task, ctrl_req->lun);
	/* [한국어] LUN 파싱 (실패해도 BAD_TARGET 응답 경로로 진입). */

	vhost_vring_desc_get_next(&desc, desc_table, desc_table_size);
	/* [한국어] 두 번째 desc(응답 buf)로 이동. */
	if (spdk_unlikely(desc == NULL)) {
		SPDK_ERRLOG("%s: no response descriptor for controlq request %d.\n",
			    vsession->name, task->req_idx);
		goto out;
	}

	/* Process the TMF request */
	switch (ctrl_req->type) {
	case VIRTIO_SCSI_T_TMF:
		/* [한국어] Task Management Function 요청. */
		task->tmf_resp = vhost_gpa_to_vva(vsession, desc->addr, sizeof(*task->tmf_resp));
		/* [한국어] TMF 응답 buf 매핑. */
		if (spdk_unlikely(desc->len < sizeof(struct virtio_scsi_ctrl_tmf_resp) || task->tmf_resp == NULL)) {
			SPDK_ERRLOG("%s: TMF response descriptor at index %d points to invalid guest memory region\n",
				    vsession->name, task->req_idx);
			goto out;
		}

		/* Check if we are processing a valid request */
		if (task->scsi_dev == NULL) {
			/* [한국어] target이 없거나 hotdetach됨 — BAD_TARGET 응답. */
			task->tmf_resp->response = VIRTIO_SCSI_S_BAD_TARGET;
			break;
		}

		switch (ctrl_req->subtype) {
		case VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET:
			/* Handle LUN reset */
			/* [한국어] LUN 단위 리셋 — 모든 진행 중 task 강제 abort. */
			SPDK_DEBUGLOG(vhost_scsi_queue, "%s: LUN reset\n", vsession->name);

			mgmt_task_submit(task, SPDK_SCSI_TASK_FUNC_LUN_RESET);
			/* [한국어] 비동기 발급 — 완료 시 mgmt_cpl 콜백에서 응답. */
			return;
		default:
			task->tmf_resp->response = VIRTIO_SCSI_S_ABORTED;
			/* [한국어] 미지원 TMF subtype — ABORTED 응답. */
			/* Unsupported command */
			SPDK_DEBUGLOG(vhost_scsi_queue, "%s: unsupported TMF command %x\n",
				      vsession->name, ctrl_req->subtype);
			break;
		}
		break;
	case VIRTIO_SCSI_T_AN_QUERY:
	case VIRTIO_SCSI_T_AN_SUBSCRIBE: {
		/* [한국어] Asynchronous Notification — 현재 미지원 (전부 ABORTED). */
		an_resp = vhost_gpa_to_vva(vsession, desc->addr, sizeof(*an_resp));
		if (spdk_unlikely(desc->len < sizeof(struct virtio_scsi_ctrl_an_resp) || an_resp == NULL)) {
			SPDK_WARNLOG("%s: asynchronous response descriptor points to invalid guest memory region\n",
				     vsession->name);
			goto out;
		}

		an_resp->response = VIRTIO_SCSI_S_ABORTED;
		/* [한국어] AN 미지원 — ABORTED. */
		break;
	}
	default:
		SPDK_DEBUGLOG(vhost_scsi_queue, "%s: Unsupported control command %x\n",
			      vsession->name, ctrl_req->type);
		/* [한국어] 알 수 없는 control command — 응답 길이 0으로 회수. */
		break;
	}

	used_len = sizeof(struct virtio_scsi_ctrl_tmf_resp);
	/* [한국어] 정상 처리 시 응답 길이. */
out:
	vhost_vq_used_ring_enqueue(vsession, task->vq, task->req_idx, used_len);
	/* [한국어] used ring enqueue. */
	vhost_scsi_task_put(task);
	/* [한국어] task 반환. */
}

/*
 * Process task's descriptor chain and setup data related fields.
 * Return
 *   -1 if request is invalid and must be aborted,
 *    0 if all data are set.
 */
/*
 * [한국어]
 * task_data_setup - virtio-scsi 요청의 desc chain을 파싱해 SPDK SCSI task의 iovs/dxfer_dir 셋업.
 *
 * @task: 처리할 SCSI task.
 * @req: 게스트 요청 구조체 포인터(out) — task_data_setup이 매핑.
 * @return: 0=성공, -1=무효(즉시 폐기).
 *
 * desc chain 형식:
 *   - FROM_DEV (READ):  [RD req] [WR resp] [WR buf0..bufN]
 *   - TO_DEV   (WRITE): [RD req] [RD buf0..bufN] [WR resp]
 *
 * 핵심 단계:
 *   1) 첫 desc → req 매핑 (반드시 read-only).
 *   2) 두 번째 desc의 WRITE 플래그로 방향 판단.
 *   3) FROM_DEV: resp가 두 번째, 데이터 buf는 그 뒤. TO_DEV: 데이터 buf가 먼저, resp는 마지막.
 *   4) 데이터 buf들을 vhost_vring_desc_to_iov로 task->iovs에 매핑 + 총 길이 누적.
 *
 * 호출 컨텍스트: 디바이스 thread.
 */
static int
task_data_setup(struct spdk_vhost_scsi_task *task,
		struct virtio_scsi_cmd_req **req)
{
	struct spdk_vhost_session *vsession = &task->svsession->vsession;
	/* [한국어] base 세션. */
	struct vring_desc *desc, *desc_table;
	/* [한국어] desc 체인 순회용. */
	struct iovec *iovs = task->iovs;
	/* [한국어] task의 iovs 배열 시작 주소(편의). */
	uint16_t iovcnt = 0;
	/* [한국어] iovs에 누적된 원소 수. */
	uint32_t desc_table_len, len = 0;
	/* [한국어] desc table 크기, 데이터 buf 총 길이. */
	int rc;
	/* [한국어] 헬퍼 반환값. */

	spdk_scsi_task_construct(&task->scsi, vhost_scsi_task_cpl, vhost_scsi_task_free_cb);
	/* [한국어] SCSI task 초기화 — 일반 명령용 완료 콜백 등록. */

	rc = vhost_vq_get_desc(vsession, task->vq, task->req_idx, &desc, &desc_table, &desc_table_len);
	/* [한국어] desc chain head 획득. */
	/* First descriptor must be readable */
	if (spdk_unlikely(rc != 0  || vhost_vring_desc_is_wr(desc) ||
			  desc->len < sizeof(struct virtio_scsi_cmd_req))) {
		/* [한국어] 첫 desc는 반드시 read-only이고 cmd_req 크기 이상이어야 함. */
		SPDK_WARNLOG("%s: invalid first request descriptor at index %"PRIu16".\n",
			     vsession->name, task->req_idx);
		goto invalid_task;
	}

	*req = vhost_gpa_to_vva(vsession, desc->addr, sizeof(**req));
	/* [한국어] 첫 desc → 요청 구조체로 매핑. */
	if (spdk_unlikely(*req == NULL)) {
		SPDK_WARNLOG("%s: request descriptor at index %d points to invalid guest memory region\n",
			     vsession->name, task->req_idx);
		goto invalid_task;
	}

	/* Each request must have at least 2 descriptors (e.g. request and response) */
	vhost_vring_desc_get_next(&desc, desc_table, desc_table_len);
	/* [한국어] 두 번째 desc로 이동. */
	if (desc == NULL) {
		SPDK_WARNLOG("%s: descriptor chain at index %d contains neither payload nor response buffer.\n",
			     vsession->name, task->req_idx);
		goto invalid_task;
	}
	task->scsi.dxfer_dir = vhost_vring_desc_is_wr(desc) ? SPDK_SCSI_DIR_FROM_DEV :
			       SPDK_SCSI_DIR_TO_DEV;
	/* [한국어] 두 번째 desc가 device-write이면 read 명령(FROM_DEV), 아니면 write(TO_DEV).
	 * READ는 응답 buf가 두 번째에 위치(WR), WRITE는 데이터 buf(RD) 다음에 응답(WR). */
	task->scsi.iovs = iovs;
	/* [한국어] SCSI 레이어가 사용할 iovs 포인터 설정. */

	if (task->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV) {
		/* [한국어] READ 경로: 응답 buf → 데이터 buf 순서. */
		/*
		 * FROM_DEV (READ): [RD_req][WR_resp][WR_buf0]...[WR_bufN]
		 */
		task->resp = vhost_gpa_to_vva(vsession, desc->addr, sizeof(*task->resp));
		/* [한국어] 두 번째 desc → 응답 buf 매핑. */
		if (spdk_unlikely(desc->len < sizeof(struct virtio_scsi_cmd_resp) || task->resp == NULL)) {
			SPDK_WARNLOG("%s: response descriptor at index %d points to invalid guest memory region\n",
				     vsession->name, task->req_idx);
			goto invalid_task;
		}
		rc = vhost_vring_desc_get_next(&desc, desc_table, desc_table_len);
		/* [한국어] 세 번째 desc(데이터 buf 시작)로 이동. */
		if (spdk_unlikely(rc != 0)) {
			SPDK_WARNLOG("%s: invalid descriptor chain at request index %d (descriptor id overflow?).\n",
				     vsession->name, task->req_idx);
			goto invalid_task;
		}

		if (desc == NULL) {
			/*
			 * TEST UNIT READY command and some others might not contain any payload and this is not an error.
			 */
			/* [한국어] TEST UNIT READY 등 페이로드 없는 명령 — 데이터 buf 0개로 정상 처리. */
			SPDK_DEBUGLOG(vhost_scsi_data,
				      "No payload descriptors for FROM DEV command req_idx=%"PRIu16".\n", task->req_idx);
			SPDK_LOGDUMP(vhost_scsi_data, "CDB=", (*req)->cdb, VIRTIO_SCSI_CDB_SIZE);
			task->used_len = sizeof(struct virtio_scsi_cmd_resp);
			/* [한국어] used에 응답 크기만 보고. */
			task->scsi.iovcnt = 1;
			/* [한국어] dummy iov 1개. */
			task->scsi.iovs[0].iov_len = 0;
			/* [한국어] 길이 0 — 데이터 전송 없음. */
			task->scsi.length = 0;
			/* [한국어] 요청 길이 0. */
			task->scsi.transfer_len = 0;
			/* [한국어] 전송 길이 0. */
			return 0;
		}

		/* All remaining descriptors are data. */
		while (desc) {
			/* [한국어] 모든 데이터 buf desc 순회. */
			if (spdk_unlikely(!vhost_vring_desc_is_wr(desc))) {
				/* [한국어] FROM_DEV에서는 데이터 buf도 device-write여야 함. RD면 게스트 측 버그. */
				SPDK_WARNLOG("%s: FROM DEV cmd: descriptor nr %" PRIu16" in payload chain is read only.\n",
					     vsession->name, iovcnt);
				goto invalid_task;
			}

			if (spdk_unlikely(vhost_vring_desc_to_iov(vsession, iovs, &iovcnt, desc))) {
				/* [한국어] desc → iovec 변환 실패(GPA 매핑 실패 등). */
				goto invalid_task;
			}
			len += desc->len;
			/* [한국어] 누적 데이터 길이. */

			rc = vhost_vring_desc_get_next(&desc, desc_table, desc_table_len);
			/* [한국어] 다음 desc 이동. NEXT 비트 없으면 desc=NULL로 루프 종료. */
			if (spdk_unlikely(rc != 0)) {
				SPDK_WARNLOG("%s: invalid payload in descriptor chain starting at index %d.\n",
					     vsession->name, task->req_idx);
				goto invalid_task;
			}
		}

		task->used_len = sizeof(struct virtio_scsi_cmd_resp) + len;
		/* [한국어] used 보고 길이 = 응답 + 페이로드. */
	} else {
		SPDK_DEBUGLOG(vhost_scsi_data, "TO DEV");
		/* [한국어] WRITE 경로: 데이터 buf → 응답 buf 순서. */
		/*
		 * TO_DEV (WRITE):[RD_req][RD_buf0]...[RD_bufN][WR_resp]
		 * No need to check descriptor WR flag as this is done while setting scsi.dxfer_dir.
		 */

		/* Process descriptors up to response. */
		while (!vhost_vring_desc_is_wr(desc)) {
			/* [한국어] WRITE 가능 desc(=응답)을 만날 때까지 데이터 buf로 간주. */
			if (spdk_unlikely(vhost_vring_desc_to_iov(vsession, iovs, &iovcnt, desc))) {
				goto invalid_task;
			}
			len += desc->len;

			vhost_vring_desc_get_next(&desc, desc_table, desc_table_len);
			if (spdk_unlikely(desc == NULL)) {
				/* [한국어] 마지막에 응답 desc가 없어야 정상이지만 NULL로 끝나면 chain 깨짐. */
				SPDK_WARNLOG("%s: TO_DEV cmd: no response descriptor.\n", vsession->name);
				goto invalid_task;
			}
		}

		task->resp = vhost_gpa_to_vva(vsession, desc->addr, sizeof(*task->resp));
		/* [한국어] 마지막 WR desc가 응답 buf. */
		if (spdk_unlikely(desc->len < sizeof(struct virtio_scsi_cmd_resp) || task->resp == NULL)) {
			SPDK_WARNLOG("%s: response descriptor at index %d points to invalid guest memory region\n",
				     vsession->name, task->req_idx);
			goto invalid_task;
		}

		task->used_len = sizeof(struct virtio_scsi_cmd_resp);
		/* [한국어] WRITE는 device가 데이터 영역에 쓰지 않으므로 used에 응답 크기만 보고. */
	}

	task->scsi.iovcnt = iovcnt;
	/* [한국어] iov 개수 반영. */
	task->scsi.length = len;
	/* [한국어] 데이터 길이 (요청 시점). */
	task->scsi.transfer_len = len;
	/* [한국어] 실제 전송 길이도 동일하게 시작 — 완료 시 data_transferred로 갱신. */
	return 0;

invalid_task:
	SPDK_DEBUGLOG(vhost_scsi_data, "%s: Invalid task at index %"PRIu16".\n",
		      vsession->name, task->req_idx);
	return -1;
	/* [한국어] -1 반환 → 호출자가 invalid_request로 폐기. */
}

/*
 * [한국어]
 * process_request - 일반 SCSI 명령(Request Q)의 task 셋업 + LUN 검증.
 *
 * @task: 처리할 task.
 * @return: 0=정상(SCSI 발급 가능), 1=NULL LUN 즉시 완료, -1=invalid 폐기.
 *
 * task_data_setup으로 desc chain → iovs 셋업 후 LUN 파싱 + CDB 매핑.
 * NULL LUN(target은 있지만 LUN id에 매핑된 LUN이 없음)이면 SCSI 레이어가 즉시 sense를 채워 반환.
 */
static int
process_request(struct spdk_vhost_scsi_task *task)
{
	struct virtio_scsi_cmd_req *req;
	/* [한국어] 게스트 요청 구조체 포인터(task_data_setup이 매핑). */
	int result;
	/* [한국어] 결과. */

	result = task_data_setup(task, &req);
	/* [한국어] desc chain 파싱 + iovs 셋업. */
	if (result) {
		return result;
		/* [한국어] -1이면 그대로 invalid 전파. */
	}

	result = vhost_scsi_task_init_target(task, req->lun);
	/* [한국어] LUN 8바이트 파싱 → target dev/LUN 객체 결정. */
	if (spdk_unlikely(result != 0)) {
		/* [한국어] LUN 형식 자체가 잘못됨 — BAD_TARGET 응답 후 폐기. */
		task->resp->response = VIRTIO_SCSI_S_BAD_TARGET;
		return -1;
	}

	task->scsi.cdb = req->cdb;
	/* [한국어] SCSI Command Descriptor Block 포인터(게스트 메모리)를 SCSI task에 그대로 전달.
	 * SPDK SCSI 레이어가 CDB를 파싱해 SCSI 명령 처리. */
	SPDK_LOGDUMP(vhost_scsi_data, "request CDB", req->cdb, VIRTIO_SCSI_CDB_SIZE);

	if (spdk_unlikely(task->scsi.lun == NULL)) {
		/* [한국어] target은 있지만 LUN id 매핑이 없음 — SCSI 레이어가 즉시 sense 응답. */
		spdk_scsi_task_process_null_lun(&task->scsi);
		task->resp->response = VIRTIO_SCSI_S_OK;
		/* [한국어] virtio response는 OK(SCSI 레벨에서는 sense로 에러 표현). */
		return 1;
		/* [한국어] 1=즉시 완료 (호출자가 vhost_scsi_task_cpl 호출). */
	}

	return 0;
	/* [한국어] 0=정상, 호출자가 task_submit. */
}

/*
 * [한국어]
 * process_scsi_task - 한 큐의 한 요청(req_idx)을 처리하는 디스패치 함수.
 *
 * @vsession: 세션.
 * @vq: virtqueue.
 * @req_idx: avail ring에서 가져온 desc head 인덱스.
 *
 * 큐별 task pool에서 task를 빌려와 초기화한 뒤,
 *   - Control Q면 process_ctrl_request
 *   - Request Q면 process_request → task_submit/early-cpl/invalid 분기.
 *
 * 같은 인덱스가 used 되지 않은 채 두 번 들어오면(used==true) 게스트 측 버그로 간주하고 폐기.
 */
static void
process_scsi_task(struct spdk_vhost_session *vsession,
		  struct spdk_vhost_virtqueue *vq,
		  uint16_t req_idx)
{
	struct spdk_vhost_scsi_task *task;
	/* [한국어] task pool에서 가져올 슬롯. */
	int result;
	/* [한국어] process_request 결과. */

	task = &((struct spdk_vhost_scsi_task *)vq->tasks)[req_idx];
	/* [한국어] 큐의 task 풀에서 req_idx 위치 슬롯 획득(인덱스 = desc 인덱스 = task 인덱스). */
	if (spdk_unlikely(task->used)) {
		/* [한국어] 이미 사용 중인 슬롯 — 게스트가 used 회수 전에 또 enqueue함(버그). */
		SPDK_ERRLOG("%s: request with idx '%"PRIu16"' is already pending.\n",
			    vsession->name, req_idx);
		vhost_vq_used_ring_enqueue(vsession, vq, req_idx, 0);
		/* [한국어] desc만 회수하고 응답 길이 0. */
		return;
	}

	vsession->task_cnt++;
	/* [한국어] 진행 중 task 카운트 — stop이 0 도달까지 대기. */
	scsi_task_init(task);
	/* [한국어] 슬롯 클리어. */

	if (spdk_unlikely(vq->vring_idx == VIRTIO_SCSI_CONTROLQ)) {
		/* [한국어] Control Q는 별도 처리 (TMF/AN). */
		process_ctrl_request(task);
	} else {
		result = process_request(task);
		/* [한국어] 일반 Request Q. */
		if (likely(result == 0)) {
			/* [한국어] 정상 — SCSI 레이어로 발급. */
			task_submit(task);
			SPDK_DEBUGLOG(vhost_scsi, "====== Task %p req_idx %d submitted ======\n", task,
				      task->req_idx);
		} else if (result > 0) {
			/* [한국어] NULL LUN 등 즉시 완료 케이스. */
			vhost_scsi_task_cpl(&task->scsi);
			SPDK_DEBUGLOG(vhost_scsi, "====== Task %p req_idx %d finished early ======\n", task,
				      task->req_idx);
		} else {
			/* [한국어] invalid — used에 빈 응답으로 폐기. */
			invalid_request(task);
			SPDK_DEBUGLOG(vhost_scsi, "====== Task %p req_idx %d failed ======\n", task,
				      task->req_idx);
		}
	}
}

/*
 * [한국어]
 * submit_inflight_desc - 비정상 종료 후 재기동 시 inflight 영역에 남아있던 미완료 요청 재제출.
 *
 * @svsession: 세션.
 * @vq: 검사할 virtqueue.
 * @return: 재제출한 요청 수.
 *
 * vhost-user의 INFLIGHT_SHMFD feature 활용 — 공유 메모리에 미완료 desc 인덱스가 보존되어
 * SPDK 재기동 시 이를 그대로 다시 처리해 게스트가 stalled되지 않도록 한다.
 */
static int
submit_inflight_desc(struct spdk_vhost_scsi_session *svsession,
		     struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession;
	/* [한국어] base 세션. */
	spdk_vhost_resubmit_info *resubmit;
	/* [한국어] DPDK가 채워준 재제출 메타. */
	spdk_vhost_resubmit_desc *resubmit_list;
	/* [한국어] 재제출할 desc 인덱스 배열. */
	uint16_t req_idx;
	/* [한국어] 현재 재제출하는 인덱스. */
	int i, resubmit_cnt;
	/* [한국어] 루프 변수, 재제출 총 수. */

	resubmit = vq->vring_inflight.resubmit_inflight;
	/* [한국어] DPDK가 inflight 분석 후 채워준 정보. */
	if (spdk_likely(resubmit == NULL || resubmit->resubmit_list == NULL ||
			resubmit->resubmit_num == 0)) {
		/* [한국어] 정상 부팅(재제출 없음) — fast path 0 반환. */
		return 0;
	}

	resubmit_list = resubmit->resubmit_list;
	vsession = &svsession->vsession;

	for (i = resubmit->resubmit_num - 1; i >= 0; --i) {
		/* [한국어] 역순 처리 — 원래 enqueue 순서를 복원하기 위함. */
		req_idx = resubmit_list[i].index;
		SPDK_DEBUGLOG(vhost_scsi, "====== Start processing resubmit request idx %"PRIu16"======\n",
			      req_idx);

		if (spdk_unlikely(req_idx >= vq->vring.size)) {
			/* [한국어] 깨진 인덱스 — 폐기. */
			SPDK_ERRLOG("%s: request idx '%"PRIu16"' exceeds virtqueue size (%"PRIu16").\n",
				    vsession->name, req_idx, vq->vring.size);
			vhost_vq_used_ring_enqueue(vsession, vq, req_idx, 0);
			continue;
		}

		process_scsi_task(vsession, vq, req_idx);
		/* [한국어] 일반 처리 경로 재사용. */
	}
	resubmit_cnt = resubmit->resubmit_num;
	resubmit->resubmit_num = 0;
	/* [한국어] 한 번만 재제출하도록 카운터 리셋. */
	return resubmit_cnt;
}

/*
 * [한국어]
 * process_vq - 한 virtqueue를 한 폴링 사이클 처리.
 *
 * @svsession: 세션.
 * @vq: 처리할 큐.
 * @return: 처리한 요청 수(>0이면 BUSY 폴러).
 *
 * 흐름:
 *   1) inflight 재제출 처리.
 *   2) avail ring에서 한 번에 최대 32개 요청 가져오기.
 *   3) 각 요청을 process_scsi_task로 처리. inflight 메타에 마킹(rte_vhost_set_inflight_desc_split).
 */
static int
process_vq(struct spdk_vhost_scsi_session *svsession, struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = &svsession->vsession;
	/* [한국어] base 세션. */
	uint16_t reqs[32];
	/* [한국어] 한 사이클에 모을 요청 인덱스 배열(SPDK_VHOST_VQ_MAX_SUBMISSIONS). */
	uint16_t reqs_cnt, i;
	/* [한국어] 실제 모인 수, 루프 변수. */
	int resubmit_cnt;
	/* [한국어] inflight 재제출 수. */

	resubmit_cnt = submit_inflight_desc(svsession, vq);
	/* [한국어] 먼저 재제출 처리(있을 때만). */

	reqs_cnt = vhost_vq_avail_ring_get(vq, reqs, SPDK_COUNTOF(reqs));
	/* [한국어] avail ring에서 새 요청 인덱스를 한 번에 모음 — last_avail_idx 업데이트됨. */
	assert(reqs_cnt <= 32);
	/* [한국어] 헬퍼 보장 가드. */

	for (i = 0; i < reqs_cnt; i++) {
		SPDK_DEBUGLOG(vhost_scsi, "====== Starting processing request idx %"PRIu16"======\n",
			      reqs[i]);

		if (spdk_unlikely(reqs[i] >= vq->vring.size)) {
			/* [한국어] 깨진 인덱스 가드. */
			SPDK_ERRLOG("%s: request idx '%"PRIu16"' exceeds virtqueue size (%"PRIu16").\n",
				    vsession->name, reqs[i], vq->vring.size);
			vhost_vq_used_ring_enqueue(vsession, vq, reqs[i], 0);
			continue;
		}

		rte_vhost_set_inflight_desc_split(vsession->vid, vq->vring_idx, reqs[i]);
		/* [한국어] 이 desc가 inflight(처리 중)임을 공유 메모리에 마킹 — crash 후 재제출 키. */

		process_scsi_task(vsession, vq, reqs[i]);
		/* [한국어] 실제 요청 처리 디스패치. */
	}

	return reqs_cnt > 0 ? reqs_cnt : resubmit_cnt;
	/* [한국어] 새 요청 우선, 없으면 재제출 수 반환 — 0이면 IDLE 폴러. */
}

/*
 * [한국어]
 * vdev_mgmt_worker - 관리 큐(EventQ + ControlQ) 폴링 콜백 (5ms 주기).
 *
 * @arg: spdk_vhost_scsi_session*.
 * @return: SPDK_POLLER_BUSY/IDLE.
 *
 * 처리 흐름:
 *   1) process_removed_devs로 hotremove drain 검사.
 *   2) EventQ: used_signal로 보류된 IRQ 송신.
 *   3) ControlQ: process_vq로 TMF 처리.
 *
 * 호출 컨텍스트: 디바이스 thread (SPDK poller framework).
 */
static int
vdev_mgmt_worker(void *arg)
{
	struct spdk_vhost_scsi_session *svsession = arg;
	/* [한국어] 인자 캐스팅. */
	struct spdk_vhost_session *vsession = &svsession->vsession;
	/* [한국어] base 세션. */
	int rc = 0;
	/* [한국어] ControlQ 처리 결과. */

	process_removed_devs(svsession);
	/* [한국어] hotremove 진행 중 target 점검. */

	if (vsession->virtqueue[VIRTIO_SCSI_EVENTQ].vring.desc) {
		/* [한국어] EventQ가 활성화돼 있으면 보류된 used 알림. */
		vhost_vq_used_signal(vsession, &vsession->virtqueue[VIRTIO_SCSI_EVENTQ]);
	}

	if (vsession->virtqueue[VIRTIO_SCSI_CONTROLQ].vring.desc) {
		/* [한국어] ControlQ 처리. */
		rc = process_vq(svsession, &vsession->virtqueue[VIRTIO_SCSI_CONTROLQ]);
		vhost_vq_used_signal(vsession, &vsession->virtqueue[VIRTIO_SCSI_CONTROLQ]);
	}

	return rc > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
	/* [한국어] BUSY: 처리한 요청 있음, IDLE: 처리 없음 — SPDK reactor가 다음 폴러로 빠르게 이동. */
}

/*
 * [한국어]
 * vdev_worker - I/O 요청 큐(REQUESTQ ~ max_queues) 폴링 콜백.
 *
 * @arg: spdk_vhost_scsi_session*.
 * @return: SPDK_POLLER_BUSY/IDLE.
 *
 * Multi-queue 시 모든 Request Q를 순회 처리. 각 큐마다 used_signal로 IRQ 보류분 전송.
 * 무한 루프 폴링 (주기 0) — 최대 IOPS 위해 reactor가 가능한 한 자주 호출.
 */
static int
vdev_worker(void *arg)
{
	struct spdk_vhost_scsi_session *svsession = arg;
	/* [한국어] 인자 캐스팅. */
	struct spdk_vhost_session *vsession = &svsession->vsession;
	/* [한국어] base 세션. */
	uint32_t q_idx;
	/* [한국어] 큐 인덱스. */
	int rc = 0;
	/* [한국어] 처리 수. */

	for (q_idx = VIRTIO_SCSI_REQUESTQ; q_idx < vsession->max_queues; q_idx++) {
		/* [한국어] Request Q부터 max_queues-1까지 순회. */
		rc = process_vq(svsession, &vsession->virtqueue[q_idx]);
		/* [한국어] 한 큐 처리. */
		vhost_session_vq_used_signal(&vsession->virtqueue[q_idx]);
		/* [한국어] 큐별 IRQ 보류분 송신(coalescing 정책 따라 실제 송신 여부 결정). */
	}

	return rc > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
	/* [한국어] 마지막 큐 결과만 반환 — 일부 큐 BUSY/IDLE 혼재 시 약간의 손실 있으나 무방. */
}

/*
 * [한국어]
 * to_scsi_dev - vhost_dev → spdk_vhost_scsi_dev 안전 변환 헬퍼.
 *
 * @ctrlr: 변환할 vdev.
 * @return: scsi_dev 포인터 또는 NULL.
 *
 * 백엔드 타입 검증 — 실수로 blk 컨트롤러를 scsi 함수에 넘겨도 safely NULL 반환.
 */
static struct spdk_vhost_scsi_dev *
to_scsi_dev(struct spdk_vhost_dev *ctrlr)
{
	if (ctrlr == NULL) {
		return NULL;
	}

	if (ctrlr->backend->type != VHOST_BACKEND_SCSI) {
		/* [한국어] 백엔드 타입 mismatch — 잘못된 호출. */
		SPDK_ERRLOG("%s: not a vhost-scsi device.\n", ctrlr->name);
		return NULL;
	}

	return SPDK_CONTAINEROF(ctrlr, struct spdk_vhost_scsi_dev, vdev);
	/* [한국어] vdev 멤버 포함 컨테이너로 변환. */
}

/*
 * [한국어]
 * to_scsi_session - vhost_session → spdk_vhost_scsi_session 캐스팅.
 *
 * 첫 멤버가 vsession이므로 단순 cast.
 */
static struct spdk_vhost_scsi_session *
to_scsi_session(struct spdk_vhost_session *vsession)
{
	assert(vsession->vdev->backend->type == VHOST_BACKEND_SCSI);
	/* [한국어] 디버그 가드. */
	return (struct spdk_vhost_scsi_session *)vsession;
}

/*
 * [한국어]
 * vhost_scsi_controller_start - delay 모드로 만들어진 컨트롤러의 listener 활성화.
 *
 * @name: 컨트롤러 이름.
 * @return: 0=성공, -ENODEV=컨트롤러 없음.
 *
 * RPC vhost_start_scsi_controller에서 호출. delay 모드에서는 socket이 listen 상태가 아니므로
 * vhost_user_dev_start로 listen을 활성화한 뒤 registered=true로 마킹.
 *
 * 호출 컨텍스트: RPC thread.
 */
int
vhost_scsi_controller_start(const char *name)
{
	struct spdk_vhost_dev *vdev;
	/* [한국어] lookup 결과. */
	struct spdk_vhost_scsi_dev *svdev;
	/* [한국어] scsi 컨테이너. */
	int rc;
	/* [한국어] 결과. */

	spdk_vhost_lock();
	/* [한국어] 디렉토리 락. */
	vdev = spdk_vhost_dev_find(name);
	/* [한국어] 이름으로 lookup. */
	if (vdev == NULL) {
		spdk_vhost_unlock();
		return -ENODEV;
	}

	svdev = to_scsi_dev(vdev);
	/* [한국어] scsi 컨테이너 변환. */
	assert(svdev != NULL);

	if (svdev->registered == true) {
		/* already started, nothing to do */
		/* [한국어] 이미 시작된 상태 — idempotent 처리. */
		spdk_vhost_unlock();
		return 0;
	}

	rc = vhost_user_dev_start(vdev);
	/* [한국어] vhost-user 인프라에 listen 시작 요청 (UNIX 소켓 listen). */
	if (rc != 0) {
		spdk_vhost_unlock();
		return rc;
	}
	svdev->registered = true;
	/* [한국어] 활성 마킹. */

	spdk_vhost_unlock();
	return 0;
}

/*
 * [한국어]
 * vhost_scsi_dev_construct - virtio-scsi 컨트롤러 생성 내부 헬퍼 (start/no_start 공통).
 *
 * @name: 컨트롤러 이름.
 * @cpumask: 디바이스 thread cpumask.
 * @delay: true면 listen 미루고 등록만.
 * @return: 0/음수 errno.
 *
 * spdk_vhost_scsi_dev 할당 → feature 비트 설정 → vhost_dev_register로 등록.
 * 등록 성공 후 delay=false면 registered=true.
 */
static int
vhost_scsi_dev_construct(const char *name, const char *cpumask, bool delay)
{
	struct spdk_vhost_scsi_dev *svdev = calloc(1, sizeof(*svdev));
	/* [한국어] 0초기화 할당 — scsi_dev_state[]도 EMPTY(0)으로 시작. */
	int rc;

	if (svdev == NULL) {
		return -ENOMEM;
	}

	svdev->vdev.virtio_features = SPDK_VHOST_SCSI_FEATURES;
	/* [한국어] virtio-scsi 광고 feature 설정. */
	svdev->vdev.disabled_features = SPDK_VHOST_SCSI_DISABLED_FEATURES;
	/* [한국어] 비활성화할 feature. */
	svdev->vdev.protocol_features = SPDK_VHOST_SCSI_PROTOCOL_FEATURES;
	/* [한국어] vhost-user 확장 protocol feature (INFLIGHT_SHMFD). */

	rc = vhost_dev_register(&svdev->vdev, name, cpumask, NULL,
				&spdk_vhost_scsi_device_backend,
				&spdk_vhost_scsi_user_device_backend, delay);
	/* [한국어] vhost.c의 등록 함수 호출 — RB tree 삽입 + 소켓 등록(delay 옵션 따라). */
	if (rc) {
		free(svdev);
		return rc;
	}

	if (delay == false) {
		svdev->registered = true;
		/* [한국어] 즉시 listen 모드면 registered 마킹. */
	}

	return rc;
}

/*
 * [한국어]
 * spdk_vhost_scsi_dev_construct - 외부 공개 API (즉시 listen).
 */
int
spdk_vhost_scsi_dev_construct(const char *name, const char *cpumask)
{
	return vhost_scsi_dev_construct(name, cpumask, false);
	/* [한국어] delay=false. */
}

/*
 * [한국어]
 * spdk_vhost_scsi_dev_construct_no_start - 외부 공개 API (delay 모드).
 *
 * target add 후 명시적으로 vhost_scsi_controller_start 호출하여 활성화.
 */
int
spdk_vhost_scsi_dev_construct_no_start(const char *name, const char *cpumask)
{
	return vhost_scsi_dev_construct(name, cpumask, true);
	/* [한국어] delay=true. */
}

/*
 * [한국어]
 * vhost_scsi_dev_remove - 컨트롤러 제거 백엔드 콜백.
 *
 * @vdev: 제거할 vdev.
 * @return: 0=완료, -EBUSY=비동기 작업 진행중(재시도 필요), 기타 errno.
 *
 * 모든 활성 target을 force-remove한 뒤 registered=false. ref가 0이면 즉시 unregister + free,
 * 아니면 마지막 target detach 시 vhost_scsi_dev_unregister가 호출됨.
 *
 * 호출 컨텍스트: RPC thread (vhost_delete_controller).
 */
static int
vhost_scsi_dev_remove(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_scsi_dev *svdev = to_scsi_dev(vdev);
	/* [한국어] scsi 컨테이너. */
	int rc = 0, i;
	/* [한국어] 결과, 루프 변수. */

	assert(svdev != NULL);

	if (vhost_user_dev_busy(vdev)) {
		/* [한국어] 진행 중 비동기 작업 있음 — RPC 측이 재시도. */
		return -EBUSY;
	}

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; ++i) {
		/* [한국어] 모든 target 슬롯 force-remove. */
		if (svdev->scsi_dev_state[i].dev) {
			rc = spdk_vhost_scsi_dev_remove_tgt(vdev, i, NULL, NULL);
			/* [한국어] cb=NULL — 비동기 완료 알림 없이 진행. */
			if (rc != 0) {
				SPDK_ERRLOG("%s: failed to force-remove target %d\n", vdev->name, i);
				return rc;
			}
		}
	}

	svdev->registered = false;
	/* [한국어] 더 이상 컨트롤러로 받지 않음 — vhost_scsi_dev_unregister 트리거 조건 일부. */

	if (svdev->ref == 0) {
		/* [한국어] 활성 target ref가 이미 0이면 즉시 unregister + free. */
		rc = vhost_dev_unregister(vdev);
		if (rc != 0) {
			return rc;
		}
		free(svdev);
	}
	/* [한국어] ref>0이면 마지막 target detach 시 remove_scsi_tgt 안에서 unregister. */

	return rc;
}

/*
 * [한국어]
 * spdk_vhost_scsi_dev_get_tgt - target 슬롯에서 SCSI dev 포인터 조회.
 *
 * @vdev: 컨트롤러.
 * @num: target 번호 [0, 8).
 * @return: PRESENT 상태인 SCSI dev 또는 NULL.
 *
 * RPC dump_info_json 등에서 target 정보 출력 시 사용.
 */
struct spdk_scsi_dev *
spdk_vhost_scsi_dev_get_tgt(struct spdk_vhost_dev *vdev, uint8_t num)
{
	struct spdk_vhost_scsi_dev *svdev;

	assert(num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS);
	/* [한국어] 범위 가드. */
	svdev = to_scsi_dev(vdev);
	assert(svdev != NULL);
	if (svdev->scsi_dev_state[num].status != VHOST_SCSI_DEV_PRESENT) {
		return NULL;
		/* [한국어] PRESENT 외 상태(EMPTY/ADDING/REMOVING/REMOVED)는 외부에 NULL로 가시. */
	}

	assert(svdev->scsi_dev_state[num].dev != NULL);
	return svdev->scsi_dev_state[num].dev;
}

/*
 * [한국어]
 * get_scsi_dev_num - LUN 객체로부터 그 LUN이 속한 target 슬롯 번호 역추적.
 *
 * @svdev: 컨트롤러.
 * @lun: SCSI 레이어에서 콜백으로 전달된 LUN 객체.
 * @return: target 슬롯 번호 [0, 8) 또는 8(=찾지 못함).
 *
 * SCSI 레이어가 hotremove/resize 콜백을 LUN 단위로 호출하므로, 이 LUN을 어느 target에서 노출했는지
 * 역으로 찾아야 한다. 선형 검색 — 슬롯 8개라 비용 무시.
 */
static unsigned
get_scsi_dev_num(const struct spdk_vhost_scsi_dev *svdev,
		 const struct spdk_scsi_lun *lun)
{
	const struct spdk_scsi_dev *scsi_dev;
	/* [한국어] LUN의 부모 SCSI dev. */
	unsigned scsi_dev_num;
	/* [한국어] 찾은 슬롯 번호. */

	assert(lun != NULL);
	assert(svdev != NULL);
	scsi_dev = spdk_scsi_lun_get_dev(lun);
	/* [한국어] LUN → SCSI dev 변환. */
	for (scsi_dev_num = 0; scsi_dev_num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; scsi_dev_num++) {
		/* [한국어] 8개 슬롯 선형 검색. */
		if (svdev->scsi_dev_state[scsi_dev_num].dev == scsi_dev) {
			break;
		}
	}

	return scsi_dev_num;
	/* [한국어] 못 찾으면 SPDK_VHOST_SCSI_CTRLR_MAX_DEVS(=8) 반환 — 호출자가 검사. */
}

/*
 * [한국어]
 * vhost_scsi_lun_resize - SCSI 레이어가 LUN 크기 변경 시 호출하는 콜백.
 *
 * @lun: 크기가 바뀐 LUN.
 * @arg: spdk_vhost_scsi_dev*.
 *
 * vhost_scsi_dev_param_changed로 PARAM_CHANGE 이벤트를 모든 세션에 전파.
 */
static void
vhost_scsi_lun_resize(const struct spdk_scsi_lun *lun, void *arg)
{
	struct spdk_vhost_scsi_dev *svdev = arg;
	/* [한국어] arg = spdk_scsi_dev_construct_ext에서 등록한 컨텍스트. */
	unsigned scsi_dev_num;

	scsi_dev_num = get_scsi_dev_num(svdev, lun);
	if (scsi_dev_num == SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		/* The entire device has been already removed. */
		/* [한국어] 이미 제거된 dev라면 콜백이 늦게 도착한 것 — 무시. */
		return;
	}

	vhost_scsi_dev_param_changed(&svdev->vdev, scsi_dev_num);
	/* [한국어] PARAM_CHANGE 이벤트 발행. */
}

/*
 * [한국어]
 * vhost_scsi_lun_hotremove - SCSI 레이어가 디바이스 제거(hotremove) 시 호출하는 콜백.
 *
 * @lun: 제거 중인 LUN.
 * @arg: spdk_vhost_scsi_dev*.
 *
 * 한 LUN이 사라지면 현재 vhost는 LUN 단위 분리를 지원하지 않으므로 target 전체를 제거.
 * (현재 구현은 1 target = 1 LUN 가정.)
 */
static void
vhost_scsi_lun_hotremove(const struct spdk_scsi_lun *lun, void *arg)
{
	struct spdk_vhost_scsi_dev *svdev = arg;
	unsigned scsi_dev_num;

	scsi_dev_num = get_scsi_dev_num(svdev, lun);
	if (scsi_dev_num == SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		/* The entire device has been already removed. */
		return;
	}

	/* remove entire device */
	spdk_vhost_scsi_dev_remove_tgt(&svdev->vdev, scsi_dev_num, NULL, NULL);
	/* [한국어] target 단위로 제거 시작 (cb=NULL, 비동기 진행). */
}

/*
 * [한국어]
 * vhost_scsi_dev_add_tgt_cpl_cb - 모든 세션에 target add 적용 후 컨트롤러 측 마무리 콜백.
 *
 * @vdev: 컨트롤러.
 * @ctx: scsi_tgt_num을 uintptr_t로 인코딩한 값.
 *
 * foreach_session의 완료 콜백 — 모든 세션이 target을 인지한 뒤 status를 PRESENT로 전환하고 ref++.
 *
 * 호출 컨텍스트: 디바이스 thread (vhost 관리 thread).
 */
static void
vhost_scsi_dev_add_tgt_cpl_cb(struct spdk_vhost_dev *vdev, void *ctx)
{
	unsigned scsi_tgt_num = (unsigned)(uintptr_t)ctx;
	/* [한국어] 인덱스 디코딩. */
	struct spdk_vhost_scsi_dev *svdev = SPDK_CONTAINEROF(vdev,
					    struct spdk_vhost_scsi_dev, vdev);
	/* [한국어] 컨테이너 변환. */
	struct spdk_scsi_dev_vhost_state *vhost_sdev;
	/* [한국어] target 슬롯. */

	vhost_sdev = &svdev->scsi_dev_state[scsi_tgt_num];

	/* All sessions have added the target */
	assert(vhost_sdev->status == VHOST_SCSI_DEV_ADDING);
	/* [한국어] add 시작 시 ADDING으로 마킹했으므로 여기서 PRESENT로 전환. */
	vhost_sdev->status = VHOST_SCSI_DEV_PRESENT;
	svdev->ref++;
	/* [한국어] 활성 target 카운트 증가 — 컨트롤러 unregister 조건에 영향. */
}

/*
 * [한국어]
 * vhost_scsi_session_add_tgt - target add를 한 세션에 적용하는 foreach 콜백.
 *
 * @vdev: 컨트롤러.
 * @vsession: 적용할 세션.
 * @ctx: scsi_tgt_num.
 * @return: 0(에러여도 다음 세션 진행).
 *
 * 세션의 사본 슬롯에 target dev 포인터/상태를 채우고 io_channel을 할당. hotplug 이벤트 발행.
 *
 * 호출 컨텍스트: 각 세션의 lcore.
 */
static int
vhost_scsi_session_add_tgt(struct spdk_vhost_dev *vdev,
			   struct spdk_vhost_session *vsession, void *ctx)
{
	unsigned scsi_tgt_num = (unsigned)(uintptr_t)ctx;
	/* [한국어] target 번호 디코딩. */
	struct spdk_vhost_scsi_session *svsession = (struct spdk_vhost_scsi_session *)vsession;
	/* [한국어] derived 캐스팅. */
	struct spdk_scsi_dev_session_state *session_sdev = &svsession->scsi_dev_state[scsi_tgt_num];
	/* [한국어] 세션의 사본 슬롯. */
	struct spdk_scsi_dev_vhost_state *vhost_sdev;
	/* [한국어] 컨트롤러 측 원본 슬롯. */
	int rc;
	/* [한국어] 결과. */

	if (!vsession->started || session_sdev->dev != NULL) {
		/* Nothing to do. */
		/* [한국어] 세션 미시작 또는 이미 추가됨 — 스킵. */
		return 0;
	}

	vhost_sdev = &svsession->svdev->scsi_dev_state[scsi_tgt_num];
	session_sdev->dev = vhost_sdev->dev;
	/* [한국어] 컨트롤러 측 dev를 세션 사본에 복사. */
	session_sdev->status = VHOST_SCSI_DEV_PRESENT;
	/* [한국어] 세션 측 상태 PRESENT. */

	rc = spdk_scsi_dev_allocate_io_channels(svsession->scsi_dev_state[scsi_tgt_num].dev);
	/* [한국어] 이 세션의 lcore에 SCSI dev의 LUN별 io_channel 할당 — bdev I/O 발급 기반 마련. */
	if (rc != 0) {
		SPDK_ERRLOG("%s: Couldn't allocate io channel for SCSI target %u.\n",
			    vsession->name, scsi_tgt_num);

		/* unset the SCSI target so that all I/O to it will be rejected */
		session_sdev->dev = NULL;
		/* [한국어] 이 세션은 target에 접근 불가 — I/O 시 BAD_TARGET 응답. */
		/* Set status to EMPTY so that we won't reply with SCSI hotremove
		 * sense codes - the device hasn't ever been added.
		 */
		session_sdev->status = VHOST_SCSI_DEV_EMPTY;
		/* [한국어] 한 번도 추가 안 됐던 것처럼 보이게 EMPTY로 — hotremove sense 회피. */

		/* Return with no error. We'll continue allocating io_channels for
		 * other sessions on this device in hopes they succeed. The sessions
		 * that failed to allocate io_channels simply won't be able to
		 * detect the SCSI target, nor do any I/O to it.
		 */
		return 0;
		/* [한국어] 0 반환 — 다른 세션에는 채널 할당 시도 계속. */
	}

	if (vhost_dev_has_feature(vsession, VIRTIO_SCSI_F_HOTPLUG)) {
		/* [한국어] 게스트가 HOTPLUG 지원하면 RESCAN 이벤트 발행. */
		eventq_enqueue(svsession, scsi_tgt_num,
			       VIRTIO_SCSI_T_TRANSPORT_RESET, VIRTIO_SCSI_EVT_RESET_RESCAN);
	} else {
		/* [한국어] 미지원 게스트는 재기동 후 rescan 필요 — 사용자에게 알림. */
		SPDK_NOTICELOG("%s: driver does not support hotplug. "
			       "Please restart it or perform a rescan.\n",
			       vsession->name);
	}

	return 0;
}

/*
 * [한국어]
 * spdk_vhost_scsi_dev_add_tgt - vhost-scsi 컨트롤러에 새 SCSI target 추가 (외부 API).
 *
 * @vdev: 대상 컨트롤러.
 * @scsi_tgt_num: target 번호 [0,8) 또는 -1(자동 배치).
 * @bdev_name: target에 연결할 bdev 이름.
 * @return: 부여된 target_num(>=0) 또는 음수 errno.
 *
 * 처리:
 *   1) target 번호 자동 배치 또는 검증.
 *   2) "Target N" 이름으로 spdk_scsi_dev 생성 (1 LUN per target).
 *   3) port id=0 추가.
 *   4) 컨트롤러가 등록 상태면 모든 세션에 add 이벤트 전파(foreach_session),
 *      delay 모드면 즉시 PRESENT로 마킹.
 *
 * 호출 컨텍스트: RPC thread (vhost_scsi_controller_add_target).
 */
int
spdk_vhost_scsi_dev_add_tgt(struct spdk_vhost_dev *vdev, int scsi_tgt_num,
			    const char *bdev_name)
{
	struct spdk_vhost_scsi_dev *svdev;
	/* [한국어] scsi 컨테이너. */
	struct spdk_scsi_dev_vhost_state *state;
	/* [한국어] 슬롯 메타. */
	char target_name[SPDK_SCSI_DEV_MAX_NAME];
	/* [한국어] target 이름 버퍼 ("Target 0" 등). */
	int lun_id_list[1];
	/* [한국어] LUN id 리스트(현재는 LUN 0 1개). */
	const char *bdev_names_list[1];
	/* [한국어] bdev 이름 리스트(현재 1개). */

	svdev = to_scsi_dev(vdev);
	if (!svdev) {
		SPDK_ERRLOG("Before adding a SCSI target, there should be a SCSI device.");
		return -EINVAL;
	}

	if (scsi_tgt_num < 0) {
		/* [한국어] 자동 배치 — 빈 슬롯 검색. */
		for (scsi_tgt_num = 0; scsi_tgt_num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; scsi_tgt_num++) {
			if (svdev->scsi_dev_state[scsi_tgt_num].dev == NULL) {
				break;
			}
		}

		if (scsi_tgt_num == SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
			/* [한국어] 8개 슬롯 모두 사용 중 — ENOSPC. */
			SPDK_ERRLOG("%s: all SCSI target slots are already in use.\n", vdev->name);
			return -ENOSPC;
		}
	} else {
		/* [한국어] 명시적 슬롯 — 범위 검증. */
		if (scsi_tgt_num >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
			SPDK_ERRLOG("%s: SCSI target number is too big (got %d, max %d), started from 0.\n",
				    vdev->name, scsi_tgt_num, SPDK_VHOST_SCSI_CTRLR_MAX_DEVS - 1);
			return -EINVAL;
		}
	}

	if (bdev_name == NULL) {
		/* [한국어] bdev 이름 필수. */
		SPDK_ERRLOG("No lun name specified\n");
		return -EINVAL;
	}

	state = &svdev->scsi_dev_state[scsi_tgt_num];
	if (state->dev != NULL) {
		/* [한국어] 슬롯 점유 중 — EEXIST. */
		SPDK_ERRLOG("%s: SCSI target %u already occupied\n", vdev->name, scsi_tgt_num);
		return -EEXIST;
	}

	/*
	 * At this stage only one LUN per target
	 */
	snprintf(target_name, sizeof(target_name), "Target %u", scsi_tgt_num);
	/* [한국어] SCSI target 이름 — "Target 0" 등. */
	lun_id_list[0] = 0;
	/* [한국어] LUN id = 0 (단일 LUN 가정). */
	bdev_names_list[0] = (char *)bdev_name;
	/* [한국어] bdev 이름 1개. */

	state->status = VHOST_SCSI_DEV_ADDING;
	/* [한국어] 추가 진행중 상태 — foreach_session 완료까지. */
	state->dev = spdk_scsi_dev_construct_ext(target_name, bdev_names_list, lun_id_list, 1,
			SPDK_SPC_PROTOCOL_IDENTIFIER_SAS,
			vhost_scsi_lun_resize, svdev,
			vhost_scsi_lun_hotremove, svdev);
	/* [한국어] SPDK SCSI 레이어에 SCSI dev 생성 요청. SAS 프로토콜 식별자.
	 * resize/hotremove 콜백 등록 — bdev 이벤트가 LUN 단위로 forward됨. */

	if (state->dev == NULL) {
		state->status = VHOST_SCSI_DEV_EMPTY;
		SPDK_ERRLOG("%s: couldn't create SCSI target %u using bdev '%s'\n",
			    vdev->name, scsi_tgt_num, bdev_name);
		return -EINVAL;
	}
	spdk_scsi_dev_add_port(state->dev, 0, "vhost");
	/* [한국어] target에 port id=0("vhost") 추가 — vhost는 단일 port 사용. */

	SPDK_INFOLOG(vhost, "%s: added SCSI target %u using bdev '%s'\n",
		     vdev->name, scsi_tgt_num, bdev_name);

	if (svdev->registered) {
		/* [한국어] 컨트롤러가 활성 listener면 기존 세션에 hotplug 전파. */
		vhost_user_dev_foreach_session(vdev, vhost_scsi_session_add_tgt,
					       vhost_scsi_dev_add_tgt_cpl_cb,
					       (void *)(uintptr_t)scsi_tgt_num);
	} else {
		/* [한국어] delay 모드 — 아직 세션 없음. 즉시 PRESENT로 마킹 + ref++. */
		state->status = VHOST_SCSI_DEV_PRESENT;
		svdev->ref++;
	}

	return scsi_tgt_num;
	/* [한국어] 부여된 target 번호 반환 (자동 배치 결과 포함). */
}

struct scsi_tgt_hotplug_ctx {
	unsigned scsi_tgt_num;
	/* [한국어] 처리 중인 target 번호. */
	bool async_fini;
	/* [한국어] true면 활성 세션이 있어 비동기 처리 — 완료는 mgmt poller가 함.
	 * false면 활성 세션 0개라 cpl_cb에서 즉시 remove_scsi_tgt 호출. */
};

/*
 * [한국어]
 * vhost_scsi_dev_remove_tgt_cpl_cb - target 제거 foreach 완료 콜백.
 *
 * @vdev: 컨트롤러.
 * @_ctx: scsi_tgt_hotplug_ctx*.
 *
 * async_fini=false (활성 세션 없음)면 즉시 remove_scsi_tgt 호출.
 * async_fini=true면 세션의 mgmt poller가 drain 후 처리하므로 여기서는 ctx free만.
 */
static void
vhost_scsi_dev_remove_tgt_cpl_cb(struct spdk_vhost_dev *vdev, void *_ctx)
{
	struct scsi_tgt_hotplug_ctx *ctx = _ctx;
	struct spdk_vhost_scsi_dev *svdev = SPDK_CONTAINEROF(vdev,
					    struct spdk_vhost_scsi_dev, vdev);
	/* [한국어] 컨테이너 변환. */

	if (!ctx->async_fini) {
		/* there aren't any active sessions, so remove the dev and exit */
		/* [한국어] 활성 세션 없음 — 즉시 컨트롤러 측 정리. */
		remove_scsi_tgt(svdev, ctx->scsi_tgt_num);
	}

	free(ctx);
	/* [한국어] 컨텍스트 해제 (async일 때도 여기서 free — drain 진행 상태는 status로 추적). */
}

/*
 * [한국어]
 * vhost_scsi_session_remove_tgt - target 제거 foreach 콜백 (각 세션에 적용).
 *
 * @vdev: 컨트롤러.
 * @vsession: 적용할 세션.
 * @_ctx: scsi_tgt_hotplug_ctx*.
 * @return: 0.
 *
 * 세션 측 상태를 REMOVING으로 마킹 + RESET_REMOVED 이벤트 발행.
 * 실제 detach는 세션 mgmt poller(process_removed_devs)가 pending I/O 0이 될 때 수행.
 */
static int
vhost_scsi_session_remove_tgt(struct spdk_vhost_dev *vdev,
			      struct spdk_vhost_session *vsession, void *_ctx)
{
	struct scsi_tgt_hotplug_ctx *ctx = _ctx;
	unsigned scsi_tgt_num = ctx->scsi_tgt_num;
	struct spdk_vhost_scsi_session *svsession = (struct spdk_vhost_scsi_session *)vsession;
	struct spdk_scsi_dev_session_state *state = &svsession->scsi_dev_state[scsi_tgt_num];

	if (!vsession->started || state->dev == NULL) {
		/* Nothing to do */
		/* [한국어] 미시작/이미 분리됨. */
		return 0;
	}

	/* Mark the target for removal */
	assert(state->status == VHOST_SCSI_DEV_PRESENT);
	state->status = VHOST_SCSI_DEV_REMOVING;
	/* [한국어] 세션 측 상태 REMOVING — 이후 process_removed_devs가 처리. */

	/* Send a hotremove virtio event */
	if (vhost_dev_has_feature(vsession, VIRTIO_SCSI_F_HOTPLUG)) {
		eventq_enqueue(svsession, scsi_tgt_num,
			       VIRTIO_SCSI_T_TRANSPORT_RESET, VIRTIO_SCSI_EVT_RESET_REMOVED);
		/* [한국어] 게스트에 디바이스 제거 알림 — virtio-scsi 스펙 RESET_REMOVED. */
	}

	/* Wait for the session's management poller to remove the target after
	 * all its pending I/O has finished.
	 */
	ctx->async_fini = true;
	/* [한국어] 활성 세션 발견 표시 — cpl_cb가 즉시 remove하지 않게. */
	return 0;
}

/*
 * [한국어]
 * spdk_vhost_scsi_dev_remove_tgt - SCSI target 제거 외부 API.
 *
 * @vdev: 컨트롤러.
 * @scsi_tgt_num: 제거할 target 번호.
 * @cb_fn: 제거 완료 시 호출할 콜백 (RPC 응답 등).
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0=시작 성공(비동기), 음수 errno=즉시 실패.
 *
 * 비동기 — foreach_session으로 세션별 detach 시작 후 즉시 리턴.
 * cb_fn은 마지막 detach가 끝났을 때 remove_scsi_tgt에서 호출됨.
 */
int
spdk_vhost_scsi_dev_remove_tgt(struct spdk_vhost_dev *vdev, unsigned scsi_tgt_num,
			       spdk_vhost_event_fn cb_fn, void *cb_arg)
{
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_scsi_dev_vhost_state *scsi_dev_state;
	struct scsi_tgt_hotplug_ctx *ctx;

	if (scsi_tgt_num >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		SPDK_ERRLOG("%s: invalid SCSI target number %d\n", vdev->name, scsi_tgt_num);
		return -EINVAL;
	}

	svdev = to_scsi_dev(vdev);
	if (!svdev) {
		SPDK_ERRLOG("An invalid SCSI device that removing from a SCSI target.");
		return -EINVAL;
	}

	scsi_dev_state = &svdev->scsi_dev_state[scsi_tgt_num];

	if (scsi_dev_state->status != VHOST_SCSI_DEV_PRESENT) {
		/* [한국어] 추가 중/제거 중/이미 제거됨 — 다른 작업 진행 중이므로 EBUSY. */
		return -EBUSY;
	}

	if (scsi_dev_state->dev == NULL || scsi_dev_state->status == VHOST_SCSI_DEV_ADDING) {
		SPDK_ERRLOG("%s: SCSI target %u is not occupied\n", vdev->name, scsi_tgt_num);
		return -ENODEV;
	}

	assert(scsi_dev_state->status != VHOST_SCSI_DEV_EMPTY);
	ctx = calloc(1, sizeof(*ctx));
	/* [한국어] 비동기 컨텍스트 할당. */
	if (ctx == NULL) {
		SPDK_ERRLOG("calloc failed\n");
		return -ENOMEM;
	}

	ctx->scsi_tgt_num = scsi_tgt_num;
	ctx->async_fini = false;
	/* [한국어] 기본은 동기 종료(활성 세션 없음 가정). foreach 콜백이 발견하면 true로 변경. */

	scsi_dev_state->remove_cb = cb_fn;
	/* [한국어] 사용자 완료 콜백 저장 (remove_scsi_tgt가 호출). */
	scsi_dev_state->remove_ctx = cb_arg;
	scsi_dev_state->status = VHOST_SCSI_DEV_REMOVING;
	/* [한국어] 컨트롤러 측 상태 REMOVING. */

	vhost_user_dev_foreach_session(vdev, vhost_scsi_session_remove_tgt,
				       vhost_scsi_dev_remove_tgt_cpl_cb, ctx);
	/* [한국어] 모든 세션에 detach 전파 시작. */
	return 0;
}

/*
 * [한국어]
 * vhost_scsi_session_param_changed - LUN 파라미터 변경(resize 등) 이벤트 foreach 콜백.
 *
 * 게스트가 VIRTIO_SCSI_F_CHANGE를 협상했으면 EventQ에 PARAM_CHANGE 이벤트 enqueue.
 * reason은 SCSI sense (asc 0x2A, ascq 0x09 = "CAPACITY DATA HAS CHANGED").
 */
static int
vhost_scsi_session_param_changed(struct spdk_vhost_dev *vdev,
				 struct spdk_vhost_session *vsession, void *ctx)
{
	unsigned scsi_tgt_num = (unsigned)(uintptr_t)ctx;
	struct spdk_vhost_scsi_session *svsession = (struct spdk_vhost_scsi_session *)vsession;
	struct spdk_scsi_dev_session_state *state = &svsession->scsi_dev_state[scsi_tgt_num];

	if (!vsession->started || state->dev == NULL) {
		/* Nothing to do */
		return 0;
	}

	/* Send a parameter change virtio event */
	if (vhost_dev_has_feature(vsession, VIRTIO_SCSI_F_CHANGE)) {
		/*
		 * virtio 1.0 spec says:
		 * By sending this event, the device signals a change in the configuration
		 * parameters of a logical unit, for example the capacity or cache mode.
		 * event is set to VIRTIO_SCSI_T_PARAM_CHANGE. lun addresses a logical unit
		 * in the SCSI host. The same event SHOULD also be reported as a unit
		 * attention condition. reason contains the additional sense code and
		 * additional sense code qualifier, respectively in bits 0…7 and 8…15.
		 * Note: For example, a change in * capacity will be reported as asc
		 * 0x2a, ascq 0x09 (CAPACITY DATA HAS CHANGED).
		 */
		eventq_enqueue(svsession, scsi_tgt_num, VIRTIO_SCSI_T_PARAM_CHANGE, 0x2a | (0x09 << 8));
		/* [한국어] reason 인코딩: lower 8비트=asc(0x2A), upper 8비트=ascq(0x09). */
	}

	return 0;
}

/*
 * [한국어]
 * vhost_scsi_dev_param_changed - target 파라미터 변경 진입점.
 *
 * @vdev: 컨트롤러.
 * @scsi_tgt_num: 영향 받은 target.
 * @return: 0=성공.
 *
 * vhost_scsi_lun_resize 콜백에서 호출. foreach_session으로 모든 세션에 PARAM_CHANGE 이벤트 발행.
 */
static int
vhost_scsi_dev_param_changed(struct spdk_vhost_dev *vdev, unsigned scsi_tgt_num)
{
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_scsi_dev_vhost_state *scsi_dev_state;

	if (scsi_tgt_num >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		SPDK_ERRLOG("%s: invalid SCSI target number %d\n", vdev->name, scsi_tgt_num);
		return -EINVAL;
	}

	svdev = to_scsi_dev(vdev);
	if (!svdev) {
		SPDK_ERRLOG("An invalid SCSI device that removing from a SCSI target.");
		return -EINVAL;
	}

	scsi_dev_state = &svdev->scsi_dev_state[scsi_tgt_num];

	if (scsi_dev_state->status != VHOST_SCSI_DEV_PRESENT) {
		return -EBUSY;
	}

	if (scsi_dev_state->dev == NULL || scsi_dev_state->status == VHOST_SCSI_DEV_ADDING) {
		SPDK_ERRLOG("%s: SCSI target %u is not occupied\n", vdev->name, scsi_tgt_num);
		return -ENODEV;
	}

	assert(scsi_dev_state->status != VHOST_SCSI_DEV_EMPTY);

	vhost_user_dev_foreach_session(vdev, vhost_scsi_session_param_changed,
				       NULL, (void *)(uintptr_t)scsi_tgt_num);
	/* [한국어] 모든 세션에 param 변경 전파. */
	return 0;
}

/*
 * [한국어]
 * free_task_pool - 세션의 모든 큐 task pool 해제.
 *
 * @svsession: 정리할 세션.
 *
 * 세션 stop 시 destroy_session_poller_cb에서 호출. 큐별 spdk_zmalloc 영역을 spdk_free로 회수.
 */
static void
free_task_pool(struct spdk_vhost_scsi_session *svsession)
{
	struct spdk_vhost_session *vsession = &svsession->vsession;
	struct spdk_vhost_virtqueue *vq;
	uint16_t i;

	for (i = 0; i < vsession->max_queues; i++) {
		/* [한국어] 활성 큐 모두 순회. */
		vq = &vsession->virtqueue[i];
		if (vq->tasks == NULL) {
			continue;
			/* [한국어] 할당 안 된 큐는 스킵. */
		}

		spdk_free(vq->tasks);
		/* [한국어] DMA 메모리 해제 (spdk_zmalloc 짝). */
		vq->tasks = NULL;
		/* [한국어] 이중 free 방지. */
	}
}

/*
 * [한국어]
 * alloc_vq_task_pool - 큐 활성화 콜백 — 큐별 task 배열 할당.
 *
 * @vsession: 세션.
 * @qid: 큐 인덱스.
 * @return: 0=성공, -1=실패, -EINVAL=qid 범위 초과.
 *
 * 큐 size만큼의 task 배열을 DMA 가능한 hugepage(SPDK_MALLOC_DMA)에 할당하고
 * 각 task에 svsession/vq/req_idx를 미리 채워 둔다 — 핫 패스에서 매번 채우는 비용 제거.
 *
 * 호출 컨텍스트: 디바이스 thread (큐 enable 시).
 */
static int
alloc_vq_task_pool(struct spdk_vhost_session *vsession, uint16_t qid)
{
	struct spdk_vhost_scsi_session *svsession = to_scsi_session(vsession);
	/* [한국어] derived 캐스팅. */
	struct spdk_vhost_virtqueue *vq;
	/* [한국어] 대상 큐. */
	struct spdk_vhost_scsi_task *task;
	/* [한국어] 채우는 중 task 포인터. */
	uint32_t task_cnt;
	/* [한국어] 할당할 task 수 = 큐 size. */
	uint32_t j;
	/* [한국어] 루프 변수. */

	if (qid >= SPDK_VHOST_MAX_VQUEUES) {
		return -EINVAL;
	}

	vq = &vsession->virtqueue[qid];
	if (vq->vring.desc == NULL) {
		/* [한국어] 큐 미초기화 — 할당 불필요. */
		return 0;
	}

	task_cnt = vq->vring.size;
	/* [한국어] 게스트가 협상한 큐 size. */
	if (task_cnt > SPDK_VHOST_MAX_VQ_SIZE) {
		/* sanity check */
		/* [한국어] 게스트가 보낸 비정상적으로 큰 size 방어. */
		SPDK_ERRLOG("%s: virtqueue %"PRIu16" is too big. (size = %"PRIu32", max = %"PRIu32")\n",
			    vsession->name, qid, task_cnt, SPDK_VHOST_MAX_VQ_SIZE);
		return -1;
	}
	vq->tasks = spdk_zmalloc(sizeof(struct spdk_vhost_scsi_task) * task_cnt,
				 SPDK_CACHE_LINE_SIZE, NULL,
				 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	/* [한국어] DMA 가능 hugepage에 task 배열 할당.
	 * SPDK_CACHE_LINE_SIZE 정렬, NUMA 무관(LCORE_ID_ANY), DMA 플래그 — bdev I/O가 직접 참조 가능. */
	if (vq->tasks == NULL) {
		SPDK_ERRLOG("%s: failed to allocate %"PRIu32" tasks for virtqueue %"PRIu16"\n",
			    vsession->name, task_cnt, qid);
		return -1;
	}

	for (j = 0; j < task_cnt; j++) {
		/* [한국어] 각 task에 정적 정보 미리 기록. */
		task = &((struct spdk_vhost_scsi_task *)vq->tasks)[j];
		task->svsession = svsession;
		/* [한국어] 부모 세션. */
		task->vq = vq;
		/* [한국어] 부모 큐. */
		task->req_idx = j;
		/* [한국어] task 인덱스 = desc 인덱스로 사용. */
	}

	return 0;
}

/*
 * [한국어]
 * vhost_scsi_start - 세션 start 콜백 — backend->start_session.
 *
 * @vdev: 컨트롤러.
 * @vsession: 시작할 세션.
 * @unused: 미사용.
 * @return: 0=성공, -EINPROGRESS=이미 시작중, -1=큐 검증 실패.
 *
 * 게스트가 vhost-user 협상을 마치고 큐 enable 메시지까지 보낸 시점에 호출됨.
 * 단계:
 *   1) 이미 진행 중인지 확인 (idempotent).
 *   2) Request Q 인덱스 범위 검증.
 *   3) 컨트롤러의 모든 PRESENT target을 세션 사본에 복사 + io_channel 할당.
 *   4) requestq_poller(0us, 무한 폴) + mgmt_poller(5ms) 등록.
 *
 * 호출 컨텍스트: 디바이스 thread (rte_vhost_user.c가 cross-thread 메시지로 디스패치).
 */
static int
vhost_scsi_start(struct spdk_vhost_dev *vdev,
		 struct spdk_vhost_session *vsession, void *unused)
{
	struct spdk_vhost_scsi_session *svsession = to_scsi_session(vsession);
	/* [한국어] derived 캐스팅. */
	struct spdk_vhost_scsi_dev *svdev;
	/* [한국어] 컨트롤러. */
	struct spdk_scsi_dev_vhost_state *state;
	/* [한국어] 컨트롤러 측 슬롯. */
	uint32_t i;
	/* [한국어] 루프 변수. */
	int rc;
	/* [한국어] io_channel 할당 결과. */

	/* return if start is already in progress */
	if (svsession->requestq_poller) {
		/* [한국어] poller가 이미 등록됨 — 진행중. */
		SPDK_INFOLOG(vhost, "%s: start in progress\n", vsession->name);
		return -EINPROGRESS;
	}

	/* validate all I/O queues are in a contiguous index range */
	if (vsession->max_queues < VIRTIO_SCSI_REQUESTQ + 1) {
		/* [한국어] 최소 ControlQ + EventQ + RequestQ 0번 = 3개 필요. */
		SPDK_INFOLOG(vhost, "%s: max_queues %u, no I/O queues\n", vsession->name, vsession->max_queues);
		return -1;
	}
	for (i = VIRTIO_SCSI_REQUESTQ; i < vsession->max_queues; i++) {
		/* [한국어] Request Q들이 모두 협상돼 있어야 함. */
		if (vsession->virtqueue[i].vring.desc == NULL) {
			SPDK_ERRLOG("%s: queue %"PRIu32" is empty\n", vsession->name, i);
			return -1;
		}
	}

	svdev = to_scsi_dev(vsession->vdev);
	assert(svdev != NULL);
	svsession->svdev = svdev;
	/* [한국어] 세션에서 컨트롤러로의 역참조 저장 — 이후 핫패스에서 lookup 비용 제거. */

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
		/* [한국어] 컨트롤러의 모든 target 슬롯을 세션 사본에 복사. */
		state = &svdev->scsi_dev_state[i];
		if (state->dev == NULL || state->status == VHOST_SCSI_DEV_REMOVING) {
			/* [한국어] 빈 슬롯 또는 제거 중인 target은 스킵. */
			continue;
		}

		assert(svsession->scsi_dev_state[i].status == VHOST_SCSI_DEV_EMPTY);
		/* [한국어] 새 세션의 사본은 EMPTY로 시작해야 함 (calloc 결과). */
		svsession->scsi_dev_state[i].dev = state->dev;
		svsession->scsi_dev_state[i].status = VHOST_SCSI_DEV_PRESENT;
		rc = spdk_scsi_dev_allocate_io_channels(state->dev);
		/* [한국어] 이 세션 lcore에 SCSI dev의 LUN별 io_channel 생성 — bdev 발급 통로. */
		if (rc != 0) {
			SPDK_ERRLOG("%s: failed to alloc io_channel for SCSI target %"PRIu32"\n",
				    vsession->name, i);
			/* unset the SCSI target so that all I/O to it will be rejected */
			svsession->scsi_dev_state[i].dev = NULL;
			/* set EMPTY state so that we won't reply with SCSI hotremove
			 * sense codes - the device hasn't ever been added.
			 */
			svsession->scsi_dev_state[i].status = VHOST_SCSI_DEV_EMPTY;
			/* [한국어] 채널 실패 시 — 이 세션은 해당 target에 접근 불가. EMPTY로 마킹해 hotremove sense 회피. */
			continue;
		}
	}
	SPDK_INFOLOG(vhost, "%s: started poller on lcore %d\n",
		     vsession->name, spdk_env_get_current_core());

	svsession->requestq_poller = SPDK_POLLER_REGISTER(vdev_worker, svsession, 0);
	/* [한국어] 주기 0us = 무한 루프 폴 — 최대 IOPS 위해. */
	svsession->mgmt_poller = SPDK_POLLER_REGISTER(vdev_mgmt_worker, svsession,
				 MGMT_POLL_PERIOD_US);
	/* [한국어] 5ms 주기 — Control/Event Q는 부담 줄이기 위해 느리게. */
	return 0;
}

/*
 * [한국어]
 * destroy_session_poller_cb - 세션 stop 중 task drain 대기 + 최종 정리 폴러.
 *
 * @arg: spdk_vhost_scsi_session*.
 * @return: SPDK_POLLER_BUSY.
 *
 * 1ms 주기로 호출되며 task_cnt가 0이 되고 user_dev->lock 획득 가능할 때까지 대기.
 * 타임아웃 도달 시 -ETIMEDOUT으로 강제 종료.
 *
 * task_cnt==0 도달 후:
 *   1) 모든 큐 used_signal — 보류된 IRQ 송신.
 *   2) 모든 target io_channel 해제 + dev=NULL.
 *   3) REMOVING 상태였던 target은 글로벌 detach foreach.
 *   4) free_task_pool로 task 메모리 해제.
 *   5) vhost_user_session_stop_done(0)으로 DPDK pthread 깨움.
 *
 * 호출 컨텍스트: 디바이스 thread (poller).
 */
static int
destroy_session_poller_cb(void *arg)
{
	struct spdk_vhost_scsi_session *svsession = arg;
	/* [한국어] 인자 캐스팅. */
	struct spdk_vhost_session *vsession = &svsession->vsession;
	/* [한국어] base 세션. */
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vsession->vdev);
	/* [한국어] 디바이스 vhost-user 컨텍스트(lock 보유). */
	struct spdk_scsi_dev_session_state *state;
	/* [한국어] target 슬롯. */
	uint32_t i;
	/* [한국어] 루프 변수. */

	if (vsession->task_cnt > 0 || (pthread_mutex_trylock(&user_dev->lock) != 0)) {
		/* [한국어] task가 남아있거나 다른 컨텍스트가 lock 보유 중 — 다음 사이클에 재시도. */
		assert(vsession->stop_retry_count > 0);
		vsession->stop_retry_count--;
		if (vsession->stop_retry_count == 0) {
			/* [한국어] 타임아웃 도달 — 강제 종료. */
			SPDK_ERRLOG("%s: Timedout when destroy session (task_cnt %d)\n", vsession->name,
				    vsession->task_cnt);
			spdk_poller_unregister(&svsession->stop_poller);
			vhost_user_session_stop_done(vsession, -ETIMEDOUT);
			/* [한국어] DPDK pthread에 -ETIMEDOUT 응답. */
		}

		return SPDK_POLLER_BUSY;
	}

	for (i = 0; i < vsession->max_queues; i++) {
		/* [한국어] 모든 큐의 보류된 IRQ 송신 — 게스트가 마지막 응답을 받도록. */
		vhost_vq_used_signal(vsession, &vsession->virtqueue[i]);
	}

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
		/* [한국어] 모든 target 슬롯 정리. */
		enum spdk_scsi_dev_vhost_status prev_status;
		/* [한국어] 이전 상태 보존(아래 분기에 사용). */

		state = &svsession->scsi_dev_state[i];
		/* clear the REMOVED status so that we won't send hotremove events anymore */
		prev_status = state->status;
		state->status = VHOST_SCSI_DEV_EMPTY;
		/* [한국어] 어쨌든 EMPTY로 — 더 이상 이벤트 발행 안 함. */
		if (state->dev == NULL) {
			continue;
		}

		spdk_scsi_dev_free_io_channels(state->dev);
		/* [한국어] io_channel 해제 — bdev 통로 닫기. */

		state->dev = NULL;
		/* [한국어] dev 포인터 분리. */

		if (prev_status == VHOST_SCSI_DEV_REMOVING) {
			/* try to detach it globally */
			/* [한국어] hotremove 진행 중이었던 target은 글로벌 detach 시도. */
			pthread_mutex_unlock(&user_dev->lock);
			/* [한국어] foreach_session이 다른 lock 잡을 수 있어 잠깐 풀어둠. */
			vhost_user_dev_foreach_session(vsession->vdev,
						       vhost_scsi_session_process_removed,
						       vhost_scsi_dev_process_removed_cpl_cb,
						       (void *)(uintptr_t)i);
			pthread_mutex_lock(&user_dev->lock);
			/* [한국어] 다시 lock 획득. */
		}
	}

	SPDK_INFOLOG(vhost, "%s: stopping poller on lcore %d\n",
		     vsession->name, spdk_env_get_current_core());

	free_task_pool(svsession);
	/* [한국어] 모든 큐 task 메모리 해제. */

	spdk_poller_unregister(&svsession->stop_poller);
	/* [한국어] stop_poller 자체 unregister — 더 이상 호출 안 됨. */
	vhost_user_session_stop_done(vsession, 0);
	/* [한국어] DPDK pthread에 0(성공) 응답 — sem_post 후 dpdk_response 0. */

	pthread_mutex_unlock(&user_dev->lock);
	/* [한국어] lock 해제. */
	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * vhost_scsi_stop - 세션 stop 콜백 — backend->stop_session.
 *
 * @vdev: 컨트롤러.
 * @vsession: 종료할 세션.
 * @return: 0=stop 시작 성공, -EINPROGRESS=이미 stop 중.
 *
 * 1) requestq_poller / mgmt_poller 즉시 unregister — 새 요청 수신 중단.
 * 2) stop_poller 등록 — 1ms마다 task_cnt 감소 대기 후 최종 정리.
 *
 * 비동기 — 완료는 destroy_session_poller_cb에서 vhost_user_session_stop_done 호출.
 */
static int
vhost_scsi_stop(struct spdk_vhost_dev *vdev,
		struct spdk_vhost_session *vsession, void *unused)
{
	struct spdk_vhost_scsi_session *svsession = to_scsi_session(vsession);
	/* [한국어] derived 캐스팅. */

	/* return if stop is already in progress */
	if (svsession->stop_poller) {
		return -EINPROGRESS;
	}

	/* Stop receiving new I/O requests */
	spdk_poller_unregister(&svsession->requestq_poller);
	/* [한국어] I/O 폴러 즉시 제거 — 새 요청 처리 중단. */

	/* Stop receiving controlq requests, also stop processing the
	 * asynchronous hotremove events. All the remaining events
	 * will be finalized by the stop_poller below.
	 */
	spdk_poller_unregister(&svsession->mgmt_poller);
	/* [한국어] 관리 폴러도 제거 — Control/Event Q 처리 중단. */

	svsession->vsession.stop_retry_count = (SPDK_VHOST_SESSION_STOP_RETRY_TIMEOUT_IN_SEC * 1000 *
						1000) / SPDK_VHOST_SESSION_STOP_RETRY_PERIOD_IN_US;
	/* [한국어] 재시도 횟수 = 타임아웃(초→us) / 주기(us). 보통 4초/1ms = 4000회. */

	/* Wait for all pending I/Os to complete, then process all the
	 * remaining hotremove events one last time.
	 */
	svsession->stop_poller = SPDK_POLLER_REGISTER(destroy_session_poller_cb,
				 svsession, SPDK_VHOST_SESSION_STOP_RETRY_PERIOD_IN_US);
	/* [한국어] 1ms 주기 stop poller 등록. */

	return 0;
}

/*
 * [한국어]
 * vhost_scsi_dump_info_json - vhost_get_controllers RPC 응답에 backend_specific 필드 채우는 콜백.
 *
 * @vdev: 컨트롤러.
 * @w: JSON writer.
 *
 * 출력 스키마: { "scsi": [ { "scsi_dev_num", "id", "target_name", "luns": [...] } ] }
 */
static void
vhost_scsi_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_scsi_dev *sdev;
	/* [한국어] target dev 임시. */
	struct spdk_scsi_lun *lun;
	/* [한국어] LUN 순회 변수. */
	uint32_t dev_idx;
	/* [한국어] target 슬롯 인덱스. */

	assert(vdev != NULL);
	spdk_json_write_named_array_begin(w, "scsi");
	/* [한국어] "scsi": [ — target 배열 시작. */
	for (dev_idx = 0; dev_idx < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; dev_idx++) {
		sdev = spdk_vhost_scsi_dev_get_tgt(vdev, dev_idx);
		if (!sdev) {
			continue;
			/* [한국어] PRESENT 아닌 슬롯은 출력에서 제외. */
		}

		spdk_json_write_object_begin(w);

		spdk_json_write_named_uint32(w, "scsi_dev_num", dev_idx);
		/* [한국어] target 슬롯 번호. */

		spdk_json_write_named_uint32(w, "id", spdk_scsi_dev_get_id(sdev));
		/* [한국어] SPDK SCSI dev 내부 id. */

		spdk_json_write_named_string(w, "target_name", spdk_scsi_dev_get_name(sdev));
		/* [한국어] "Target N" 형식 이름. */

		spdk_json_write_named_array_begin(w, "luns");
		/* [한국어] LUN 배열. */

		for (lun = spdk_scsi_dev_get_first_lun(sdev); lun != NULL;
		     lun = spdk_scsi_dev_get_next_lun(lun)) {
			/* [한국어] target에 속한 모든 LUN 순회 (현재는 1개). */
			spdk_json_write_object_begin(w);

			spdk_json_write_named_int32(w, "id", spdk_scsi_lun_get_id(lun));
			/* [한국어] LUN id. */

			spdk_json_write_named_string(w, "bdev_name", spdk_scsi_lun_get_bdev_name(lun));
			/* [한국어] 백엔드 bdev 이름. */

			spdk_json_write_object_end(w);
		}

		spdk_json_write_array_end(w);
		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
}

/*
 * [한국어]
 * vhost_scsi_write_config_json - save_config 시 호출 — 컨트롤러 재구성 RPC 시퀀스 출력.
 *
 * @vdev: 컨트롤러.
 * @w: JSON writer.
 *
 * 출력:
 *   1) vhost_create_scsi_controller(delay=true) RPC.
 *   2) 각 target에 대해 vhost_scsi_controller_add_target RPC.
 *   3) vhost_start_scsi_controller RPC.
 *
 * SPDK 재기동 시 이 시퀀스를 그대로 실행하면 동일 컨트롤러가 복원된다.
 */
static void
vhost_scsi_write_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_scsi_dev *scsi_dev;
	struct spdk_scsi_lun *lun;
	uint32_t i;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "vhost_create_scsi_controller");
	/* [한국어] 첫 번째 RPC: 컨트롤러 생성 (delay=true로 만들어 add_target까지 한 후 start). */

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "ctrlr", vdev->name);
	spdk_json_write_named_string(w, "cpumask",
				     spdk_cpuset_fmt(spdk_thread_get_cpumask(vdev->thread)));
	/* [한국어] 디바이스 thread cpumask. */
	spdk_json_write_named_bool(w, "delay", true);
	/* [한국어] delay=true로 listen 미루기 — target 추가 후 명시 start. */
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
		scsi_dev = spdk_vhost_scsi_dev_get_tgt(vdev, i);
		if (scsi_dev == NULL) {
			continue;
		}

		lun = spdk_scsi_dev_get_lun(scsi_dev, 0);
		/* [한국어] LUN 0(현재는 단일 LUN per target). */
		assert(lun != NULL);

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "vhost_scsi_controller_add_target");
		/* [한국어] 두 번째 RPC: 각 target 추가. */

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "ctrlr", vdev->name);
		spdk_json_write_named_uint32(w, "scsi_target_num", i);

		spdk_json_write_named_string(w, "bdev_name", spdk_scsi_lun_get_bdev_name(lun));
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "vhost_start_scsi_controller");
	/* [한국어] 마지막 RPC: 컨트롤러 활성화 (listen 시작). */

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "ctrlr", vdev->name);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

SPDK_LOG_REGISTER_COMPONENT(vhost_scsi)
/* [한국어] "vhost_scsi" 로그 컴포넌트 — 일반 디버그 로그. */
SPDK_LOG_REGISTER_COMPONENT(vhost_scsi_queue)
/* [한국어] "vhost_scsi_queue" — 큐/desc 처리 디버그. */
SPDK_LOG_REGISTER_COMPONENT(vhost_scsi_data)
/* [한국어] "vhost_scsi_data" — 데이터 페이로드 덤프 디버그. */
