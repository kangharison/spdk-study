/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] OCF(Open CAS Framework) vbdev 모듈 공개 헤더 (vbdev_ocf.h)
 *
 * === 파일의 역할 ===
 * Open CAS Framework(OCF)를 SPDK bdev 위에 적용해 "캐시 디바이스 + core 디바이스" 조합으로
 * 블록 캐싱(WT/WB/WA/PT 등 cache mode)을 제공하는 vbdev 모듈의 내부 정의/공개 API. OCF는
 * 별도 라이브러리(외부 서브모듈)로, 캐시 정책/메타데이터/리커버리/순차 분리 등을 담당하고,
 * 본 vbdev는 OCF의 plug-in 형태로 SPDK bdev 두 개(cache + core)를 OCF에 연결한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [JSON-RPC] → [vbdev_ocf_rpc.c]
 *     → [이 헤더 — vbdev_ocf_construct/delete/foreach]
 *         → [vbdev_ocf.c] (SPDK side 통합)
 *             ↓ libocf API
 *         [OCF library (ocf_cache, ocf_core, ocf_queue, ocf_volume)]
 *             ↓ volume callbacks → ocf/volume.c (이 디렉토리)
 *                 ↓ spdk_bdev_read/write
 *             [spdk_bdev cache + spdk_bdev core]
 *
 * === 타 모듈과의 연결 ===
 * - vbdev_ocf.c : 구현부.
 * - ocf/volume.c : OCF의 "volume" 인터페이스(I/O submit/event)를 SPDK bdev로 매핑.
 * - ocf/ctx.c : OCF 환경(thread/mempool/log/data buffer) 콜백 구현.
 * - ocf/data.c : OCF의 buffer "ctx_data_obj_alloc/free/seek" 구현.
 * - ocf/stats.c : OCF stats를 SPDK 통계로 변환.
 * - ocf/utils.c : 잡유틸.
 * - libocf : 외부 라이브러리.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct vbdev_ocf_qctx : OCF queue ↔ SPDK thread 매핑 컨텍스트.
 * - struct vbdev_ocf_state : 비동기 상태 머신 플래그 묶음.
 * - struct vbdev_ocf_config : OCF cache/attach/core config 묶음.
 * - struct vbdev_ocf_mngt_ctx : 비동기 management step machine 컨텍스트.
 * - struct vbdev_ocf_base : cache 또는 core 한 쪽의 디바이스 정보.
 * - struct vbdev_ocf : 노출되는 단일 vbdev (cache+core 페어 + OCF 핸들 + exp_bdev).
 * - vbdev_ocf_construct() : 생성 진입점.
 * - vbdev_ocf_get_by_name() / get_base_by_name() : 조회.
 * - vbdev_ocf_delete / delete_clean : 삭제(즉시 vs 클린-셧다운).
 * - vbdev_ocf_set_cache_mode / set_seqcutoff : 모드/임계값 변경.
 * - vbdev_ocf_foreach : 등록된 모든 OCF vbdev 순회.
 */

#ifndef SPDK_VBDEV_OCF_H
#define SPDK_VBDEV_OCF_H

#include <ocf/ocf.h>
/* [한국어] Open CAS Framework 라이브러리 헤더. ocf_cache/ocf_core/ocf_queue/ocf_volume 등 타입과
 * cache 관리/mngt 함수(ocf_mngt_*)를 제공. */

#include "spdk/bdev.h"
/* [한국어] spdk_bdev core API. */
#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈 작성 API. */

/* [한국어] OCF metadata에 저장 가능한 UUID/식별자의 최대 길이(바이트). vbdev_ocf.uuid에 사용. */
#define VBDEV_OCF_MD_MAX_LEN 4096

struct vbdev_ocf;
/* [한국어] forward declaration — 본 헤더 내 다른 구조체가 포인터로 참조. */

/* Context for OCF queue poller
 * Used for mapping SPDK threads to OCF queues */
/*
 * [한국어] struct vbdev_ocf_qctx
 *
 * OCF queue 1개 ↔ SPDK thread 1개를 1:1로 묶는 컨텍스트. OCF는 자체 queue 추상화를 가지고
 * 작업을 enqueue하면 vbdev 모듈이 SPDK poller로 drain해주는 모델이다. 각 SPDK reactor에서
 * 본 컨텍스트가 1개씩 만들어진다.
 */
struct vbdev_ocf_qctx {
	/* OCF queue. Contains OCF requests */
	struct ocf_queue            *queue;
	/* [한국어] OCF에서 만든 queue 핸들. OCF가 작업을 push하면 poller가 ocf_queue_run으로 처리. */

	/* Poller for OCF queue. Runs OCF requests */
	struct spdk_poller          *poller;
	/* [한국어] OCF queue를 drain하는 SPDK poller. spdk_poller_register로 등록. */

	/* Reference to parent vbdev */
	struct vbdev_ocf            *vbdev;
	/* [한국어] 자신이 속한 vbdev_ocf. */

	/* Base devices channels */
	struct spdk_io_channel      *cache_ch;
	/* [한국어] cache base bdev의 IO channel. OCF가 cache로 I/O 발사 시 사용. */
	struct spdk_io_channel      *core_ch;
	/* [한국어] core base bdev의 IO channel. */

	/* If true, we have to free this context on queue stop */
	bool allocated;
	/* [한국어] true면 OCF stop 콜백에서 본 컨텍스트를 free해야 함을 표시. */

	/* Link to per-bdev list of queue contexts */
	TAILQ_ENTRY(vbdev_ocf_qctx)  tailq;
	/* [한국어] vbdev_ocf::cache_ctx의 qctx 리스트 노드. */
};

/* Important states */
/*
 * [한국어] struct vbdev_ocf_state
 * vbdev 비동기 라이프사이클의 진행 단계를 표시하는 플래그 묶음.
 */
struct vbdev_ocf_state {
	/* From the moment when clean delete started */
	bool                         doing_clean_delete;
	/* [한국어] clean delete (cache flush 후 unregister) 진행 중. */
	/* From the moment when finish started */
	bool                         doing_finish;
	/* [한국어] 일반 finish (stop) 절차 진행 중. */
	/* From the moment when reset IO received, until it is completed */
	bool                         doing_reset;
	/* [한국어] RESET 처리 중 — 신규 I/O를 일시 차단. */
	/* From the moment when exp_bdev is registered */
	bool                         started;
	/* [한국어] exp_bdev가 bdev core에 등록 완료. */
	/* From the moment when register path started */
	bool                         starting;
	/* [한국어] 시작 중(등록 대기). */
	/* Status of last attempt for stopping this device */
	int                          stop_status;
	/* [한국어] 마지막 stop 시도의 결과 errno (0 = 성공). */
};

/*
 * OCF cache configuration options
 */
/*
 * [한국어] struct vbdev_ocf_config
 * OCF에 cache를 attach/load할 때 전달하는 3가지 OCF 옵션 구조체와 load 모드 플래그.
 */
struct vbdev_ocf_config {
	/* Initial cache configuration  */
	struct ocf_mngt_cache_config        cache;
	/* [한국어] OCF cache 인스턴스 옵션 (mode, name, cache_line_size 등). */

	/* Cache device config */
	struct ocf_mngt_cache_attach_config attach;
	/* [한국어] cache 디바이스 attach 시 옵션 (force, discard_on_start 등). */

	/* Core initial config */
	struct ocf_mngt_core_config         core;
	/* [한국어] core 디바이스 등록 옵션. */

	/* Load flag, if set to true, then we will try load cache instance from disk,
	 * otherwise we will create new cache on that disk */
	bool                                loadq;
	/* [한국어] true면 디스크에 있는 기존 cache metadata 로드(보존), false면 새 cache 생성(파괴). */
};

/* Types for management operations */
/*
 * [한국어] typedef vbdev_ocf_mngt_fn / vbdev_ocf_mngt_callback
 * 비동기 management 절차의 step 함수 타입과 최종 콜백 타입.
 */
typedef void (*vbdev_ocf_mngt_fn)(struct vbdev_ocf *);
typedef void (*vbdev_ocf_mngt_callback)(int, struct vbdev_ocf *, void *);

/* Context for asynchronous management operations
 * Single management operation usually contains a list of sub procedures,
 * this structure handles sharing between those sub procedures */
/*
 * [한국어] struct vbdev_ocf_mngt_ctx
 * 비동기 management 절차의 상태 머신 컨텍스트. step 배열을 순차 실행하며 진행.
 */
struct vbdev_ocf_mngt_ctx {
	/* Pointer to function that is currently being executed
	 * It gets incremented on each step until it dereferences to NULL */
	vbdev_ocf_mngt_fn                  *current_step;
	/* [한국어] step 함수 배열을 가리키는 포인터. ++로 다음 step 진행, NULL 만나면 완료. */

	/* Function that gets invoked by poller on each iteration */
	vbdev_ocf_mngt_fn                   poller_fn;
	/* [한국어] step이 polling 대기를 요청한 경우 매 iteration마다 호출되는 함수. */
	/* Poller timeout time stamp - when the poller should stop with error */
	uint64_t                            timeout_ts;
	/* [한국어] poller가 timeout 처리해야 할 절대 시각(tick). */

	/* Status of management operation */
	int                                 status;
	/* [한국어] 현재 management 절차의 결과 status (0 = 진행 중/성공, 음수 = 에러). */

	/* External callback and its argument */
	vbdev_ocf_mngt_callback             cb;
	/* [한국어] 절차 완료 시 호출할 외부 콜백. */
	void                               *cb_arg;
	/* [한국어] 외부 콜백 인자. */
};

/* Base device info */
/*
 * [한국어] struct vbdev_ocf_base
 * cache 또는 core 한 쪽의 base bdev 정보 묶음.
 */
struct vbdev_ocf_base {
	/* OCF internal name */
	char                        *name;
	/* [한국어] OCF에 등록되는 이름 (= base bdev 이름). */

	/* True if this is a caching device */
	bool                         is_cache;
	/* [한국어] true면 cache 슬롯, false면 core 슬롯. */

	/* Connected SPDK block device */
	struct spdk_bdev            *bdev;
	/* [한국어] open된 base bdev 포인터. */

	/* SPDK device io handle */
	struct spdk_bdev_desc       *desc;
	/* [한국어] spdk_bdev_open_ext의 결과 descriptor (write_claim 포함). */

	/* True if SPDK bdev has been claimed and opened for writing */
	bool                         attached;
	/* [한국어] open + claim 성공 여부. */

	/* Channel for cleaner operations */
	struct spdk_io_channel      *management_channel;
	/* [한국어] cleaner thread(메타데이터 청소)가 사용하는 IO 채널. */

	/* Reference to main vbdev */
	struct vbdev_ocf            *parent;
	/* [한국어] 본 base가 속한 vbdev_ocf 역포인터. */

	/* thread where base device is opened */
	struct spdk_thread	    *thread;
	/* [한국어] open이 일어난 SPDK thread (close도 같은 thread에서 해야 함). */
};

/*
 * The main information provider
 * It's also registered as io_device
 */
/*
 * [한국어] struct vbdev_ocf
 *
 * OCF vbdev 인스턴스. cache + core 쌍, OCF 핸들, 노출 spdk_bdev, 비동기 상태/management 컨텍스트
 * 모두 보유. io_device로 등록되어 reactor당 IO 채널이 생성됨.
 */
struct vbdev_ocf {
	/* Exposed unique name */
	char                        *name;
	/* [한국어] 외부에 노출되는 vbdev 이름. */

	/* Base bdevs */
	struct vbdev_ocf_base        cache;
	/* [한국어] cache slot. */
	struct vbdev_ocf_base        core;
	/* [한국어] core slot. */

	/* Base bdevs OCF objects */
	ocf_cache_t                  ocf_cache;
	/* [한국어] OCF에 등록된 cache 핸들. */
	ocf_core_t                   ocf_core;
	/* [한국어] OCF에 등록된 core 핸들. */

	/* Parameters */
	struct vbdev_ocf_config      cfg;
	/* [한국어] config 묶음. */
	struct vbdev_ocf_state       state;
	/* [한국어] 상태 머신 플래그. */

	/* Management context */
	struct vbdev_ocf_mngt_ctx    mngt_ctx;
	/* [한국어] 현재 진행 중인 management 절차의 컨텍스트. */

	/* Cache context */
	struct vbdev_ocf_cache_ctx  *cache_ctx;
	/* [한국어] cache 인스턴스별 보조 컨텍스트 (qctx 리스트 등). */

	/* Status of flushing operation */
	struct {
		bool in_progress;
		/* [한국어] flush가 진행 중인지. */
		int status;
		/* [한국어] flush 결과 status. */
	} flush;

	/* Exposed SPDK bdev. Registered in bdev layer */
	struct spdk_bdev             exp_bdev;
	/* [한국어] 외부로 노출되는 spdk_bdev. spdk_bdev_register 대상. */

	/* OCF uuid for core device of this vbdev */
	char uuid[VBDEV_OCF_MD_MAX_LEN];
	/* [한국어] core 디바이스의 OCF UUID (메타데이터에 저장되어 재부팅 후 재연결 식별자). */

	/* Link to global list of this type structures */
	TAILQ_ENTRY(vbdev_ocf)       tailq;
	/* [한국어] 모듈 글로벌 리스트 노드. */
};

/*
 * [한국어] vbdev_ocf_construct - OCF vbdev 생성 진입점.
 * @vbdev_name: 노출 이름.
 * @cache_mode_name: "wt"/"wb"/"wa"/"wo"/"pt" 등 cache mode 문자열.
 * @cache_line_size: cache line 크기(바이트).
 * @cache_name: cache base bdev 이름.
 * @core_name: core base bdev 이름.
 * @loadq: true면 기존 cache metadata 로드 시도.
 * @cb: 완료 콜백 (status, vbdev, cb_arg).
 * @cb_arg: 콜백 인자.
 */
void vbdev_ocf_construct(
	const char *vbdev_name,
	const char *cache_mode_name,
	const uint64_t cache_line_size,
	const char *cache_name,
	const char *core_name,
	bool loadq,
	void (*cb)(int, struct vbdev_ocf *, void *),
	void *cb_arg);

/* If vbdev is online, return its object */
/* [한국어] vbdev_ocf_get_by_name - 이름으로 online 상태의 vbdev_ocf 조회. */
struct vbdev_ocf *vbdev_ocf_get_by_name(const char *name);

/* Return matching base if parent vbdev is online */
/* [한국어] vbdev_ocf_get_base_by_name - 이름으로 base(cache 또는 core) 조회. */
struct vbdev_ocf_base *vbdev_ocf_get_base_by_name(const char *name);

/* Stop OCF cache and unregister SPDK bdev */
/* [한국어] vbdev_ocf_delete - 즉시 stop + unregister (cache flush 비강제). */
int vbdev_ocf_delete(struct vbdev_ocf *vbdev, void (*cb)(void *, int), void *cb_arg);

/* [한국어] vbdev_ocf_delete_clean - clean delete (cache flush 후 stop + unregister). */
int vbdev_ocf_delete_clean(struct vbdev_ocf *vbdev, void (*cb)(void *, int), void *cb_arg);

/* Set new cache mode on OCF cache */
/* [한국어] vbdev_ocf_set_cache_mode - 동작 중 cache mode 변경. */
void vbdev_ocf_set_cache_mode(
	struct vbdev_ocf *vbdev,
	const char *cache_mode_name,
	void (*cb)(int, struct vbdev_ocf *, void *),
	void *cb_arg);

/* Set sequential cutoff parameters on OCF cache */
/* [한국어] vbdev_ocf_set_seqcutoff - 순차 cutoff 정책 설정. promotion_count: cutoff 발동 임계 횟수. */
void vbdev_ocf_set_seqcutoff(
	struct vbdev_ocf *vbdev,
	const char *policy_name,
	uint32_t threshold,
	uint32_t promotion_count,
	void (*cb)(int, void *),
	void *cb_arg);

/* [한국어] typedef vbdev_ocf_foreach_fn - foreach 콜백 시그니처. */
typedef void (*vbdev_ocf_foreach_fn)(struct vbdev_ocf *, void *);

/* Execute fn for each OCF device that is online or waits for base devices */
/* [한국어] vbdev_ocf_foreach - 모든 OCF vbdev(online 또는 base 대기 중)에 fn 실행. */
void vbdev_ocf_foreach(vbdev_ocf_foreach_fn fn, void *ctx);

#endif
