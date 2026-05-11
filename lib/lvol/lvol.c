/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK Logical Volume Manager 핵심 구현 (lvol.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK Blobstore(BS) 위에 "Logical Volume(LVol)" 추상화를 구현하는 라이브러리
 * 코어이다. LVM 과 유사하게 한 블록 디바이스(bdev) 위에 N 개의 가변 크기 가상 디스크를
 * 생성/삭제/스냅샷/클론할 수 있게 해 주는 user-space, polled-mode, 비동기 콜백 모델 구현이다.
 * 한 LVS(Logical Volume Store) = 한 spdk_blob_store(BS 인스턴스 = 한 backend bdev) 이고,
 * 한 LVol = 한 spdk_blob 이다. 본 파일은 다음을 직접 구현한다:
 *   1) LVS 라이프사이클 (init / load / load_ext / unload / destroy / rename / grow / grow_live).
 *   2) LVol 라이프사이클 (create / open / close / destroy / rename / resize / set_read_only).
 *   3) 부모-자식 관계 (snapshot / clone / esnap_clone / inflate / decouple_parent /
 *      set_parent / set_external_parent / shallow_copy / iter_immediate_clones).
 *   4) External Snapshot(esnap) 관리: hot-plug 알림, "missing" 상태 추적
 *      (degraded_lvol_sets_tree), bs_dev_create 후크.
 *   5) 전역 LVS 등록/조회 (g_lvol_stores 연결리스트, get_by_uuid / get_by_names / is_degraded).
 *   6) Blobstore 에 외부 데이터 소스를 연결하기 위한 esnap_bs_dev_create 라우터 (lvs ←→ bs).
 * 모든 lvol 메타데이터(이름, UUID 등)는 blob xattr 로 저장되며, 한 LVS 의 super blob 은
 * lvs UUID/이름을 보관한다. 데이터 I/O(read/write)는 본 파일에 없고 spdk_blob_io_*  로
 * 위임된다 — 본 파일은 control plane(메타데이터/관계) 만 담당.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 스토리지 스택에서 lvol 라이브러리는 "blob 위 user-facing wrapper" 이며, module/bdev/
 * lvol/ (vbdev_lvol) 에 의해 다시 bdev 로 등록되어 fio/NVMe-oF/vhost/iscsi/ublk 등 모든
 * frontend 가 일반 bdev 처럼 사용한다.
 *
 *    [Frontend module: vhost-blk / NVMe-oF / fio_plugin / iscsi / ublk]
 *           |  spdk_bdev_open_ext / readv / writev
 *           v
 *    [bdev layer (lib/bdev)]
 *           |  bdev module dispatch
 *           v
 *    [bdev_lvol module (module/bdev/lvol/)]   <— lvol 을 bdev 로 등록하는 wrapper
 *           |  본 파일의 spdk_lvol_* / spdk_lvs_* API 호출
 *           v
 *    [lvol library (이 파일 + lvol_rpc.c)]   ← LVS metadata 관리, control plane
 *           |  spdk_blob_* / spdk_bs_*
 *           v
 *    [blobstore (lib/blob/)]                 ← LVS 1개 = 한 spdk_blob_store
 *           |  cluster I/O via bs_channel
 *           v
 *    [bs_dev abstraction (spdk_bdev_create_bs_dev)]
 *           |
 *           v
 *    [실제 backend bdev: NVMe / aio / malloc / raid …]
 *
 * 실행 컨텍스트: 모든 LVS 관련 비동기 콜백은 LVS 가 처음 init/load 된 spdk_thread (lvs->thread)
 * 에서 직렬 실행된다. 사용자(예: vbdev_lvol)는 lvs->thread 와 다른 thread 에서 LVS API 를
 * 호출하면 안 되며, 필요시 spdk_thread_send_msg() 로 owning thread 에 dispatch 해야 한다.
 * data plane(spdk_blob_io_*) 은 호출 thread 의 io_channel 에서 처리된다.
 *
 * === 타 모듈과의 연결 ===
 * - spdk_internal/lvolstore.h: 본 파일 전용 내부 자료구조 정의 (spdk_lvol_store, spdk_lvol,
 *   spdk_lvs_*_req, spdk_lvol_*_req 등). 본 파일에서만 internal 헤더를 통해 직접 접근.
 * - spdk/blob.h: 본 파일이 호출하는 모든 BS API (spdk_bs_init / load / unload / destroy /
 *   grow / grow_live / iter_first / iter_next / set_super / open_blob / create_blob /
 *   create_snapshot / create_clone / blob_inflate / blob_decouple_parent / blob_set_parent /
 *   blob_set_external_parent / blob_shallow_copy / blob_get_clones / blob_set_xattr /
 *   blob_get_xattr_value / blob_resize / blob_sync_md / blob_set_read_only / blob_close /
 *   blob_set_esnap_bs_dev / blob_is_degraded). 모든 IO/메타 작업이 BS 로 위임된다.
 * - spdk/blob_bdev.h: bs_dev abstraction 헤더 — spdk_bs_dev 타입.
 * - spdk/log.h: SPDK_ERRLOG / SPDK_NOTICELOG / SPDK_INFOLOG / SPDK_DEBUGLOG 매크로.
 * - spdk/string.h: 안전 문자열 함수.
 * - spdk/thread.h: spdk_get_thread / spdk_io_channel — owning thread 검증.
 * - spdk/tree.h: BSD-style RB tree 매크로 (RB_HEAD/INSERT/REMOVE/FIND/GENERATE_STATIC).
 *   degraded_lvol_sets_tree 구현에 사용.
 * - spdk/util.h: spdk_divide_round_up, SPDK_COUNTOF 등 유틸 매크로.
 * - sys/queue.h: TAILQ_HEAD/INSERT_TAIL/FOREACH/REMOVE — lvol/lvs 리스트 관리.
 * - module/bdev/lvol/ (vbdev_lvol): 외부 호출자. 본 파일의 API 를 통해 LVS 를 load/init 후
 *   각 LVol 을 bdev 로 등록한다.
 *
 * 공유 자료구조:
 *   - 전역 g_lvol_stores TAILQ + g_lvol_stores_mutex: 호스트 프로세스 내 모든 LVS 의 등록 리스트.
 *     이름 충돌 검출 / hotplug 알림 / get_by_uuid 조회의 진입점. 다중 thread 가 동시 read 가능.
 *   - lvs->lvols / lvs->pending_lvols / lvs->retry_open_lvols: LVS 단위 lvol 컬렉션. lvs->thread
 *     에서만 접근 — lock 없음.
 *   - lvs->degraded_lvol_sets_tree (RB tree): "missing esnap" 별로 동일한 esnap 을 기다리는
 *     lvol 들을 묶은 set 의 모음. lvs->thread 단독 접근 — lock 없음.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_lvs_init: 새 LVS 생성 (= spdk_bs_init + super blob 생성 + uuid/name 기록).
 * - spdk_lvs_load / load_ext: 디스크의 LVS 메타를 읽고 LVS 핸들 복원 (모든 lvol 도 enumerate).
 * - spdk_lvs_unload: blobstore unload + lvol 메모리 해제 (디스크 데이터는 보존).
 * - spdk_lvs_destroy: super blob 삭제 + bs_destroy (디스크 데이터 영구 파괴).
 * - spdk_lvs_rename: LVS 이름 변경 (super blob 의 "name" xattr 갱신, 두 단계 sync).
 * - spdk_lvs_grow / grow_live: backend bdev 가 커진 경우 BS 영역 확장.
 * - spdk_lvol_create / create_esnap_clone / create_snapshot / create_clone: LVol 생성 패밀리.
 * - spdk_lvol_open / close / destroy / rename / resize / set_read_only: LVol 라이프사이클.
 * - spdk_lvol_inflate / decouple_parent: 부모와의 의존성 해소 (CoW → 독립 cluster 보유).
 * - spdk_lvol_set_parent / set_external_parent / shallow_copy / iter_immediate_clones: 관계 변경/순회.
 * - spdk_lvs_esnap_missing_add / _remove / spdk_lvs_notify_hotplug: External Snapshot
 *   "degraded" 추적 및 hot-plug 처리.
 * - spdk_lvol_get_by_uuid / get_by_names / is_degraded / deletable: 조회/상태.
 * - struct spdk_lvs_degraded_lvol_set: "한 esnap 이 missing 상태일 때, 그것을 기다리는 lvol
 *   들의 집합"을 나타내는 RB tree 노드 + lvol TAILQ.
 * - struct lvs_esnap_hotplug_req: hotplug 콜백용 임시 컨텍스트.
 */

#include "spdk_internal/lvolstore.h"     /* [한국어] 본 파일 전용 내부 자료구조 (struct
                                          * spdk_lvol_store / spdk_lvol / spdk_lvs_req /
                                          * spdk_lvol_req / *_with_handle_req 등). 본 파일과
                                          * vbdev_lvol 에서만 사용되는 internal 정의. */
#include "spdk/log.h"                    /* [한국어] SPDK_ERRLOG / SPDK_NOTICELOG /
                                          * SPDK_INFOLOG / SPDK_DEBUGLOG 매크로. 본 파일은
                                          * 콜백 실패 경로 추적에 ERRLOG, 정상 흐름 추적에
                                          * INFOLOG 를 사용한다. */
#include "spdk/string.h"                 /* [한국어] 일부 SPDK 안전 문자열 함수 헤더 (직접
                                          * 사용은 적지만, 의존 헤더가 가져오는 정의 포함). */
#include "spdk/thread.h"                 /* [한국어] spdk_get_thread() — 콜백이 owning
                                          * thread 에서 실행되고 있는지 assert 검증하는 데
                                          * 사용. SPDK 의 thread/poller/message API. */
#include "spdk/blob_bdev.h"              /* [한국어] spdk_bs_dev 추상화. 호출자가 backend
                                          * bdev 위에 spdk_bdev_create_bs_dev() 로 만든
                                          * spdk_bs_dev * 를 lvs_init/load/grow 에 넘긴다. */
#include "spdk/tree.h"                   /* [한국어] BSD-style RB tree 매크로
                                          * (RB_HEAD/RB_ENTRY/RB_INSERT/RB_REMOVE/RB_FIND/
                                          * RB_GENERATE_STATIC). degraded_lvol_sets_tree
                                          * 구현에 사용 — esnap_id 키로 정렬된 set 트리. */
#include "spdk/util.h"                   /* [한국어] spdk_divide_round_up (size→cluster 수
                                          * 변환), SPDK_COUNTOF (배열 길이) 등 유틸. */

/* Default blob channel opts for lvol */
#define SPDK_LVOL_BLOB_OPTS_CHANNEL_OPS 512
                                          /* [한국어] LVS 의 blobstore 가 동시에 처리할 수
                                           * 있는 채널 단위 in-flight metadata operation 의
                                           * 기본 상한. spdk_bs_load/init 에 전달되는
                                           * spdk_bs_opts.max_channel_ops 값. 너무 작으면
                                           * 대규모 lvol 동시 작업 시 throttling, 너무 크면
                                           * 메모리 낭비. 512 는 일반 desktop/server SSD
                                           * 환경의 적정 기본값. */

#define LVOL_NAME "name"                  /* [한국어] lvol 이름을 blob xattr 로 저장할 때
                                           * 사용하는 키 문자열. 한 lvol blob 은 두 개의
                                           * xattr ("name", "uuid") 를 갖는다. 본 파일과
                                           * load 경로에서 같은 키를 참조하므로 매크로화. */

SPDK_LOG_REGISTER_COMPONENT(lvol)         /* [한국어] "lvol" 이라는 SPDK 로그 컴포넌트를 등록.
                                           * 사용자가 spdk_log_set_print_level() 또는 RPC
                                           * (log_set_flag) 로 본 컴포넌트의 INFOLOG/DEBUGLOG
                                           * 출력을 동적으로 켜고 끌 수 있다. SPDK_INFOLOG(lvol,
                                           * ...) 같은 호출은 이 컴포넌트가 enable 된 경우에만
                                           * 실제 출력. */

/*
 * [한국어] struct spdk_lvs_degraded_lvol_set
 *
 * "한 외부 스냅샷(esnap)이 현재 사용 불가능한 상태(missing)일 때, 그것을 부모로 갖는
 * lvol 들의 집합"을 나타내는 RB-tree 노드. 한 LVS 마다 lvs->degraded_lvol_sets_tree 가
 * 존재하며, 동일한 esnap_id 를 기다리는 lvol 들이 같은 노드 안의 TAILQ 에 묶인다.
 *
 * 라이프사이클:
 *   - spdk_lvs_esnap_missing_add() 가 새로운 esnap_id 에 대해 호출되면 여기에 set 노드를
 *     생성·삽입하고, 같은 esnap_id 가 이미 있으면 기존 노드의 lvols TAILQ 에 lvol 만 추가.
 *   - spdk_lvs_notify_hotplug() 가 호출되어 esnap 이 사용 가능해지면 해당 set 안의 모든
 *     lvol 에 대해 esnap_bs_dev_create() 를 다시 시도(hotplug)하고, 성공한 lvol 은 set 에서
 *     제거. set 이 비면 RB tree 에서 노드 자체도 제거.
 *
 * 동기화: 같은 LVS 의 lvs->thread 단일 thread 에서만 read/write — 별도 lock 없음.
 *         lvs 사이의 충돌은 spdk_lvs_notify_hotplug() 가 g_lvol_stores_mutex 로 보호.
 */
struct spdk_lvs_degraded_lvol_set {
	struct spdk_lvol_store			*lvol_store;
	/* [한국어] 이 set 이 속한 LVS 의 역참조 포인터.
	 * 설정자: spdk_lvs_esnap_missing_add() 에서 set 노드 생성 시 lvs 로 초기화.
	 * 읽는 자: lvs_esnap_degraded_hotplug() — 어느 LVS 에 속한 set 인지 알기 위해 사용
	 *          (특히 RB tree 에서 노드 제거 시 lvs->degraded_lvol_sets_tree 위치 파악).
	 * 값 범위: 유효한 LVS 포인터. set 의 라이프사이클이 LVS 의 라이프사이클 안에 있으므로
	 *          set 이 살아 있는 동안 항상 유효.
	 * 동기화: lvs->thread 단일 thread 에서만 접근. */

	const void				*esnap_id;
	/* [한국어] 이 set 이 기다리는 외부 스냅샷의 식별자 (불투명 바이트 배열).
	 * 설정자: spdk_lvs_esnap_missing_add() 가 calloc + memcpy 로 input id 의 사본을 보유.
	 *          set 노드가 자체 메모리를 소유하므로 호출자 영역의 esnap_id 가 free 되어도 안전.
	 * 읽는 자: lvs_esnap_name_cmp() (RB tree 비교 함수), spdk_lvs_notify_hotplug()
	 *          (hotplug id 와 일치 여부 검사), lvs_esnap_degraded_hotplug() (재시도용).
	 * 값 범위: id_len 바이트 길이의 임의 식별자. SPDK 의 esnap 의미에서는 보통 bdev UUID
	 *          문자열이지만, 사용자 정의 형태도 허용 (esnap_bs_dev_create 콜백이 해석).
	 * 동기화: lvs->thread 단일 thread 에서만 접근. set 노드와 동일한 라이프사이클. */

	uint32_t				id_len;
	/* [한국어] esnap_id 가 가리키는 바이트 길이.
	 * 설정자: spdk_lvs_esnap_missing_add() 에서 input id_len 으로 초기화 (memcpy 길이와 일치).
	 * 읽는 자: lvs_esnap_name_cmp() — 길이가 다르면 길이 자체로 정렬, 같으면 memcmp.
	 *          spdk_lvs_notify_hotplug() — find 키 구성에 사용.
	 * 값 범위: 1 이상의 정수. 일반적으로 UUID 문자열(36+1=37) 또는 그보다 짧은 식별자.
	 * 동기화: lvs->thread 단일 thread 에서만 접근. */

	TAILQ_HEAD(degraded_lvols, spdk_lvol)	lvols;
	/* [한국어] 이 esnap 을 기다리고 있는 lvol 들의 연결 리스트 (TAILQ).
	 * 설정자: lvs_degraded_lvol_set_add() 가 TAILQ_INSERT_TAIL 로 lvol 을 추가
	 *          (lvol->degraded_link 필드를 사용).
	 * 읽는 자: lvs_esnap_degraded_hotplug() 가 hotplug 시 전체 순회.
	 *          spdk_lvs_esnap_missing_remove() 가 lvol 을 제거하고, 비었는지 확인 후 set 노드도 제거.
	 * 값 범위: 비어 있지 않을 수도, 1개 이상일 수도 있음. set 노드는 lvol 이 최소 1개 있어야
	 *          존재해야 함이 invariant — 비면 즉시 RB tree 에서 제거되어야 한다.
	 * 동기화: lvs->thread 단일 thread 에서만 접근. */

	RB_ENTRY(spdk_lvs_degraded_lvol_set)	node;
	/* [한국어] RB tree (lvs->degraded_lvol_sets_tree) 의 노드 링크 필드 (BSD <sys/tree.h>).
	 * 설정자: RB_INSERT 매크로 (spdk_lvs_esnap_missing_add) 가 자동으로 채움.
	 * 읽는 자: RB_FIND/RB_REMOVE/RB_FOREACH 매크로 — 트리 순회/조회/삭제 시 자동 사용.
	 * 동기화: 트리는 lvs->thread 단일 thread 에서만 접근하므로 별도 lock 없음. */
};

static TAILQ_HEAD(, spdk_lvol_store) g_lvol_stores = TAILQ_HEAD_INITIALIZER(g_lvol_stores);
                                          /* [한국어] 호스트 프로세스 내 "현재 등록된 모든 LVS"
                                           * 의 전역 연결 리스트. spdk_lvs_init/load 가
                                           * add_lvs_to_list 로 추가, lvs_free 가 제거.
                                           * spdk_lvs_notify_hotplug / spdk_lvol_get_by_uuid /
                                           * spdk_lvol_get_by_names 등 LVS 간 검색의 진입점.
                                           * g_lvol_stores_mutex 로 보호 (다른 thread 의
                                           * vbdev examine 콜백이 동시에 read 가능). */
static pthread_mutex_t g_lvol_stores_mutex = PTHREAD_MUTEX_INITIALIZER;
                                          /* [한국어] g_lvol_stores 보호용 mutex. SPDK 는 보통
                                           * lockless 모델이지만 본 변수는 "전역" 이고 다른
                                           * spdk_thread 들이 동시에 examine/hotplug 콜백에서
                                           * 접근하므로 명시적 mutex 가 필요. critical section
                                           * 안에서는 blocking 호출(spdk_thread_send_msg 등)을
                                           * 하지 않고, 짧게 list traversal 만 한다. */

/* [한국어] 본 파일 내부에서 상호 참조되는 static 함수의 forward 선언 — 정의 전에 호출이
 *           등장할 수 있도록 컴파일러에 알려준다. */
static inline int lvs_opts_copy(const struct spdk_lvs_opts *src, struct spdk_lvs_opts *dst);
                                          /* [한국어] ABI-aware lvs_opts 복사 (opts_size 기반
                                           * 필드별 안전 복사). lvs_init/load 에서 사용. */
static int lvs_esnap_bs_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
				   const void *esnap_id, uint32_t id_len,
				   struct spdk_bs_dev **_bs_dev);
                                          /* [한국어] blobstore 가 esnap clone 을 open 할 때
                                           * 호출하는 콜백. lvs 의 사용자 콜백
                                           * (lvs->esnap_bs_dev_create) 으로 dispatch 한다. */
static struct spdk_lvol *lvs_get_lvol_by_blob_id(struct spdk_lvol_store *lvs, spdk_blob_id blob_id);
                                          /* [한국어] LVS 의 lvols TAILQ 에서 blob_id 로 lvol
                                           * 검색. esnap hotplug / destroy 경로의 보조 함수. */
static void lvs_degraded_lvol_set_add(struct spdk_lvs_degraded_lvol_set *degraded_set,
				      struct spdk_lvol *lvol);
                                          /* [한국어] degraded set 에 lvol 추가 (lvol->
                                           * degraded_set 갱신 + TAILQ_INSERT_TAIL). */
static void lvs_degraded_lvol_set_remove(struct spdk_lvs_degraded_lvol_set *degraded_set,
		struct spdk_lvol *lvol);
                                          /* [한국어] degraded set 에서 lvol 제거 (lvol->
                                           * degraded_set 을 NULL 로 + TAILQ_REMOVE). */

/*
 * [한국어]
 * add_lvs_to_list - LVS 를 전역 g_lvol_stores 리스트에 등록 (이름 충돌 검출 포함)
 *
 * @lvs: 등록 대상 LVS. 이미 lvs_alloc 으로 할당되고 lvs->name 이 채워져 있어야 함.
 * @return: 0 = 성공, -1 = 동일 이름 LVS 가 이미 존재 (EEXIST 의미).
 *
 * 동기/배경: SPDK 호스트 프로세스에 여러 LVS 가 공존할 수 있으며, 이름 기반 조회/충돌
 * 검출이 필요하다 (RPC 사용자가 같은 이름으로 두 번 만들지 못하게 막아야 한다).
 *
 * 동작:
 *   1) g_lvol_stores_mutex 획득.
 *   2) 기존 LVS 들의 name 과 비교 — 중복 시 unlock 후 -1 반환.
 *   3) 중복 없음: lvs->on_list=true, TAILQ_INSERT_TAIL.
 *   4) unlock 후 0 반환.
 *
 * 실행 컨텍스트: lvs_init / lvs_load 의 비동기 콜백 체인 안에서 lvs->thread 가 호출.
 *               g_lvol_stores 보호용 mutex 를 사용하므로 다른 thread 의 hotplug 호출과
 *               안전하게 race 한다.
 *
 * 호출 체인:
 *   spdk_lvs_init() / lvs_read_uuid() → [add_lvs_to_list]
 */
static int
add_lvs_to_list(struct spdk_lvol_store *lvs)
{
	struct spdk_lvol_store *tmp;                       /* [한국어] 리스트 순회용 임시 포인터. */

	pthread_mutex_lock(&g_lvol_stores_mutex);          /* [한국어] 전역 리스트 보호 — 다른
	                                                    * thread 의 examine/hotplug 와 race
	                                                    * 방지. critical section 안에서
	                                                    * blocking 호출 금지. */
	TAILQ_FOREACH(tmp, &g_lvol_stores, link) {         /* [한국어] 등록된 모든 LVS 순회. */
		if (!strncmp(lvs->name, tmp->name, SPDK_LVS_NAME_MAX)) {
		                                            /* [한국어] 이름 충돌 검출 — 정확
		                                             * 일치이면 거부. SPDK_LVS_NAME_MAX
		                                             * 까지 비교해 truncated 이름 위장
		                                             * 방지. */
			pthread_mutex_unlock(&g_lvol_stores_mutex);
			                                    /* [한국어] 일찍 unlock 후 에러. */
			return -1;                          /* [한국어] EEXIST 의미. 호출자는
			                                     * 즉시 LVS 정리 경로로 진입. */
		}
	}

	lvs->on_list = true;                               /* [한국어] "리스트 멤버십" 플래그 —
	                                                    * lvs_free 가 TAILQ_REMOVE 를 안전하게
	                                                    * 호출할 수 있도록 표식. 등록 실패한
	                                                    * LVS 를 정리할 때 잘못된 REMOVE 방지. */
	TAILQ_INSERT_TAIL(&g_lvol_stores, lvs, link);      /* [한국어] 전역 리스트 끝에 추가. */
	pthread_mutex_unlock(&g_lvol_stores_mutex);        /* [한국어] mutex 해제. */

	return 0;                                          /* [한국어] 성공. */
}

/*
 * [한국어]
 * lvs_alloc - 빈 LVS 구조체 할당 및 초기 필드 세팅
 *
 * @return: 새로 할당된 spdk_lvol_store * 또는 NULL (메모리 부족 시).
 *
 * 동기: spdk_lvs_init / spdk_lvs_load / spdk_lvs_grow 모두 LVS 핸들 메모리를 먼저
 * 확보해야 하며, 그 안의 lvol 컬렉션과 RB tree 를 비어있는 상태로 시작해야 한다.
 *
 * 동작:
 *   1) calloc 으로 0-init 메모리 확보.
 *   2) lvols / pending_lvols / retry_open_lvols TAILQ 초기화.
 *   3) load_esnaps=false (BS 가 fully load 될 때까지 esnap dev 생성 보류).
 *   4) degraded_lvol_sets_tree 빈 RB tree.
 *   5) thread = 호출 thread (= 이후 모든 LVS 콜백이 실행될 owning thread).
 *
 * 실행 컨텍스트: 임의 spdk_thread (LVS 의 owning thread 가 됨).
 *
 * 호출 체인:
 *   spdk_lvs_init / lvs_load / spdk_lvs_grow → [lvs_alloc]
 */
static struct spdk_lvol_store *
lvs_alloc(void)
{
	struct spdk_lvol_store *lvs;                       /* [한국어] 새로 할당할 핸들. */

	lvs = calloc(1, sizeof(*lvs));                     /* [한국어] 0-init 할당 — 모든 필드를
	                                                    * NULL/0 으로. on_list=false 도 자동. */
	if (lvs == NULL) {                                 /* [한국어] 메모리 부족. */
		return NULL;                               /* [한국어] 호출자가 ENOMEM 처리. */
	}

	TAILQ_INIT(&lvs->lvols);                           /* [한국어] 활성(open 가능) lvol 컬렉션. */
	TAILQ_INIT(&lvs->pending_lvols);                   /* [한국어] 생성 진행 중 (blob create
	                                                    * 콜백 대기) lvol 컬렉션. 콜백 도달 시
	                                                    * lvols 로 이동. */
	TAILQ_INIT(&lvs->retry_open_lvols);                /* [한국어] esnap missing 등으로 open
	                                                    * 재시도가 필요한 lvol 컬렉션. */

	lvs->load_esnaps = false;                          /* [한국어] BS load 의 blob iteration
	                                                    * 단계에서 esnap bs_dev 를 만들지
	                                                    * 않도록 가드. iteration 종료 후 true. */
	RB_INIT(&lvs->degraded_lvol_sets_tree);            /* [한국어] missing esnap 추적 RB tree
	                                                    * 빈 상태로 초기화. */
	lvs->thread = spdk_get_thread();                   /* [한국어] 호출 thread 를 owning
	                                                    * thread 로 기록. 이후 모든 LVS
	                                                    * 콜백/조작은 이 thread 에서 호출되어야
	                                                    * 함을 assert(lvs->thread ==
	                                                    * spdk_get_thread()) 로 검증. */

	return lvs;                                        /* [한국어] 호출자에게 핸들 반환. */
}

/*
 * [한국어]
 * lvs_free - LVS 핸들 메모리 해제 (전역 리스트에서 제거 + invariant 검증)
 *
 * @lvs: 해제 대상. 이미 blobstore 는 unload/destroy 되어 있어야 하고, lvol 들도
 *       모두 free 되어 있어야 한다 (lvols TAILQ 비어 있음).
 *
 * 동기: LVS 라이프사이클 종료 시 메모리 회수. 전역 리스트에 등록되어 있다면 먼저 제거.
 *
 * 동작:
 *   1) g_lvol_stores_mutex 잠그고, on_list=true 면 TAILQ_REMOVE.
 *   2) degraded_lvol_sets_tree 비어 있어야 함을 assert (esnap 추적이 정리되지 않은 채
 *      LVS 를 free 하면 메모리 누수).
 *   3) free(lvs).
 *
 * 실행 컨텍스트: lvs->thread (owning thread) 또는 init/load 실패 정리 경로.
 */
static void
lvs_free(struct spdk_lvol_store *lvs)
{
	pthread_mutex_lock(&g_lvol_stores_mutex);          /* [한국어] 전역 리스트 보호. */
	if (lvs->on_list) {                                /* [한국어] add_lvs_to_list 로 등록된
	                                                    * 적이 있는가 — 등록 실패 시 false. */
		TAILQ_REMOVE(&g_lvol_stores, lvs, link);   /* [한국어] 등록되어 있다면 제거. */
	}
	pthread_mutex_unlock(&g_lvol_stores_mutex);        /* [한국어] mutex 해제. */

	assert(RB_EMPTY(&lvs->degraded_lvol_sets_tree));   /* [한국어] esnap missing 추적 트리는
	                                                    * 비어 있어야 한다. 비어있지 않다면
	                                                    * spdk_lvs_esnap_missing_remove() 가
	                                                    * 모든 lvol 에 대해 호출되지 않은
	                                                    * 버그 — 메모리 누수 위험. debug 빌드
	                                                    * 에서 즉시 abort 하여 발견. */

	free(lvs);                                         /* [한국어] 핸들 메모리 회수. */
}

/*
 * [한국어]
 * lvol_alloc - 새 LVol 핸들 할당 + UUID 생성 + pending 리스트에 등록
 *
 * @lvs: 소속 LVS.
 * @name: 사용자가 지정한 lvol 이름 (NULL-terminated). lvs 내부에서 unique 해야 함
 *        (lvs_verify_lvol_name 이 사전 검사).
 * @thin_provision: thin provisioning 여부 (참고만 — 실제 적용은 spdk_blob_opts 에서).
 *                  현 함수에서는 사용하지 않지만 인터페이스 일관성 유지를 위해 보존.
 * @clear_method: cluster 데이터 초기화 정책 (UNMAP / WRITE_ZEROES / NONE).
 * @return: 새 lvol 핸들 또는 NULL (메모리 부족).
 *
 * 동기: spdk_lvol_create / create_snapshot / create_clone / create_esnap_clone 모두
 * 동일한 형태로 lvol 핸들을 사전 생성하고 blobstore 에 blob create 를 dispatch 한다.
 * 이 함수가 그 공통 부분을 담당.
 *
 * 동작:
 *   1) calloc.
 *   2) lvol_store back-pointer + clear_method.
 *   3) name 복사 (snprintf 로 buffer overflow 방어).
 *   4) UUID 생성 (spdk_uuid_generate — random/time-based v4) + 36자리 lowercase 문자열.
 *   5) unique_id 도 같은 UUID 로 초기화 (load 시에는 lvs UUID + "_" + blob_id 형태가 될 수
 *      있음 — load_next_lvol 참고).
 *   6) pending_lvols TAILQ 에 추가 — blob 생성 콜백(lvol_create_open_cb) 도달 시
 *      여기에서 제거하고 lvols 로 이동. 같은 이름 중복 검사를 lvs_verify_lvol_name 이
 *      pending 까지 검사하므로, "동시에 같은 이름으로 create" 도 차단된다.
 *
 * 호출 체인:
 *   spdk_lvol_create / _snapshot / _clone / _esnap_clone → [lvol_alloc]
 *     → 추후 lvol_create_open_cb → TAILQ_INSERT_TAIL(&lvs->lvols, ...)
 */
static struct spdk_lvol *
lvol_alloc(struct spdk_lvol_store *lvs, const char *name, bool thin_provision,
	   enum lvol_clear_method clear_method)
{
	struct spdk_lvol *lvol;                            /* [한국어] 새 lvol 핸들. */

	lvol = calloc(1, sizeof(*lvol));                   /* [한국어] 0-init 할당. */
	if (lvol == NULL) {                                /* [한국어] 메모리 부족. */
		return NULL;                               /* [한국어] 호출자가 ENOMEM 처리. */
	}

	lvol->lvol_store = lvs;                            /* [한국어] 부모 LVS 역참조. */
	lvol->clear_method = (enum blob_clear_method)clear_method;
	                                                    /* [한국어] lvol_clear_method →
	                                                     * blob_clear_method 로 캐스트.
	                                                     * 두 enum 은 1:1 매핑되도록 동일한
	                                                     * 정수값을 갖는다 (헤더 ABI 약속). */
	snprintf(lvol->name, sizeof(lvol->name), "%s", name);
	                                                    /* [한국어] 이름 안전 복사 — 버퍼 크기
	                                                     * 초과 시 자동 truncate. lvol->name
	                                                     * 은 SPDK_LVOL_NAME_MAX 바이트. */
	spdk_uuid_generate(&lvol->uuid);                   /* [한국어] 무작위(v4) UUID 생성 — 새
	                                                    * lvol 마다 고유 식별자 부여. blob
	                                                    * xattr "uuid" 로 저장되어 영속화. */
	spdk_uuid_fmt_lower(lvol->uuid_str, sizeof(lvol->uuid_str), &lvol->uuid);
	                                                    /* [한국어] 36자리 lowercase 문자열 형태
	                                                     * (예: "550e8400-e29b-..."). */
	spdk_uuid_fmt_lower(lvol->unique_id, sizeof(lvol->uuid_str), &lvol->uuid);
	                                                    /* [한국어] unique_id 는 로깅/식별용
	                                                     * 문자열 — 새 lvol 의 경우 uuid_str 과
	                                                     * 동일. (load 시 uuid 가 손상된 lvol
	                                                     * 의 경우 lvs UUID + "_" + blob_id 형태
	                                                     * 로 fallback — load_next_lvol 참고.)
	                                                     * 두 번째 인자가 sizeof(uuid_str) 인
	                                                     * 것에 주의 — unique_id 는 더 큰
	                                                     * 버퍼이지만 UUID 만 쓰는 케이스이므로
	                                                     * 36+1 길이로 충분. */

	TAILQ_INSERT_TAIL(&lvs->pending_lvols, lvol, link);
	                                                    /* [한국어] 생성 in-flight 인 lvol 을
	                                                     * pending 큐에 등록. blob create 콜백
	                                                     * 도달 시 lvols 로 이동. lvs_verify_
	                                                     * lvol_name 이 pending 도 검사하므로
	                                                     * "동일 이름 동시 생성" 도 차단. */

	return lvol;                                       /* [한국어] 핸들 반환. */
}

/*
 * [한국어]
 * lvol_free - lvol 핸들 메모리 해제
 *
 * @lvol: 해제 대상. 이미 blob 은 close/delete 되어 있고, 어떤 리스트에서도 제거되어
 *        있어야 한다.
 *
 * 동기: lvol 의 라이프사이클 종료 시 메모리 회수. 단순 wrapper 이지만 메모리 회수
 * 위치를 한 함수로 통일해 향후 변경(예: per-CPU 풀)을 한 곳에서 처리할 수 있게 한다.
 *
 * 호출자: lvol_delete_blob_cb / lvol_create_open_cb (실패 경로) / spdk_lvs_unload /
 *         spdk_lvs_destroy / load_next_lvol (실패 경로).
 */
static void
lvol_free(struct spdk_lvol *lvol)
{
	free(lvol);                                        /* [한국어] 핸들 메모리 회수. */
}

/*
 * [한국어]
 * lvol_open_cb - spdk_lvol_open 의 blob open 완료 콜백
 *
 * @cb_arg: spdk_lvol_with_handle_req 포인터 (with_handle = handle 을 콜백에 같이 넘기는
 *           req 변종).
 * @blob: BS 가 open 한 blob 핸들 (lvolerrno != 0 일 때는 NULL).
 * @lvolerrno: 0 = 성공, 음수 errno = 실패.
 *
 * 동기: spdk_lvol_open 이 비동기 spdk_bs_open_blob_ext 를 호출했을 때 그 완료를 받아
 * lvol->blob 포인터와 ref_count 를 갱신하고 사용자 콜백에 결과 전달.
 *
 * 동작:
 *   - 성공: ref_count++, lvol->blob = blob.
 *   - 실패: 로그만, lvol 상태는 그대로 (이미 다른 곳에서 이전 blob 핸들이 보존될 수 있음).
 *   - 어떤 경우든 사용자 cb_fn(req->cb_arg, lvol, lvolerrno) 호출, req 해제.
 *
 * 실행 컨텍스트: BS 의 metadata thread (= lvs->thread). 사용자는 콜백 안에서 lvol 을
 *               즉시 사용할 수 있다.
 *
 * 호출 체인: spdk_lvol_open → spdk_bs_open_blob_ext → [lvol_open_cb] → 사용자 cb_fn.
 */
static void
lvol_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;    /* [한국어] req 컨텍스트 복원. */
	struct spdk_lvol *lvol = req->lvol;                /* [한국어] open 대상 lvol. */

	if (lvolerrno != 0) {                              /* [한국어] BS open 실패. */
		SPDK_INFOLOG(lvol, "Failed to open lvol %s\n", lvol->unique_id);
		                                            /* [한국어] INFOLOG — open 실패는
		                                             * 사용자 입장의 비정상이지만, 단
		                                             * 한번 시도한 결과이므로 ERRLOG
		                                             * 까지 가지는 않는다. */
		goto end;                                  /* [한국어] 콜백/free 단계로. */
	}

	lvol->ref_count++;                                 /* [한국어] open 성공 시 ref count 증가
	                                                    * — close 는 ref_count 가 0 이 될
	                                                    * 때만 실제 blob_close 를 호출한다. */
	lvol->blob = blob;                                 /* [한국어] open 된 blob 핸들 저장 —
	                                                    * 이후 read/write/resize/sync_md 등
	                                                    * 모든 IO 가 이 핸들을 통해 일어난다. */
end:
	req->cb_fn(req->cb_arg, lvol, lvolerrno);          /* [한국어] 사용자 콜백 호출 — 성공이든
	                                                    * 실패든 lvol 자체는 살아 있다 (생성된
	                                                    * 핸들이지 blob 의 라이프와 무관). */
	free(req);                                         /* [한국어] req 해제. */
}

/*
 * [한국어]
 * spdk_lvol_open - 기존 lvol 의 blob 을 open 하여 IO 가능 상태로 만든다 (공개 API)
 *
 * @lvol: open 대상 lvol (이미 lvs->lvols 에 등록된 핸들).
 * @cb_fn: 완료 콜백 (NULL 금지).
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동기: lvol 은 핸들 자체와 blob handle 이 분리된다. lvs_load 시에는 blob 을 일시 open
 * 하고 즉시 close 하므로 blob_id 만 보관된 상태가 되며, 사용자가 IO 를 보내려면 명시
 * 적으로 spdk_lvol_open 으로 blob 을 다시 open 해야 한다 (vbdev_lvol 이 bdev_open 시
 * 호출).
 *
 * 동작:
 *   - lvol 이 NULL: ENODEV.
 *   - action_in_progress: EBUSY (다른 destroy/close 가 진행 중).
 *   - 이미 open 상태(ref_count>0): ref_count 증가만 하고 즉시 콜백 (재진입 가능 = thin
 *     reopen ).
 *   - 첫 open: blob_open_opts 에 clear_method 만 세팅, spdk_bs_open_blob_ext 로 dispatch.
 *
 * 호출 체인:
 *   vbdev_lvol_open / RPC bdev_lvol_get_lvols → [spdk_lvol_open]
 *     → spdk_bs_open_blob_ext → lvol_open_cb → 사용자 cb_fn.
 */
void
spdk_lvol_open(struct spdk_lvol *lvol, spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;             /* [한국어] 비동기 콜백을 위한 req. */
	struct spdk_blob_open_opts opts;                   /* [한국어] BS open opt (clear_method). */

	assert(cb_fn != NULL);                             /* [한국어] 콜백은 필수 — 비동기 결과를
	                                                    * 받지 못하면 리소스 추적 불가. */

	if (lvol == NULL) {                                /* [한국어] NULL 핸들 가드. */
		SPDK_ERRLOG("lvol does not exist\n");
		cb_fn(cb_arg, NULL, -ENODEV);              /* [한국어] 실패 콜백 직접 호출. */
		return;
	}

	if (lvol->action_in_progress == true) {            /* [한국어] destroy/close 진행 중. */
		SPDK_ERRLOG("Cannot open lvol - operations on lvol pending\n");
		cb_fn(cb_arg, lvol, -EBUSY);
		return;
	}

	if (lvol->ref_count > 0) {                         /* [한국어] 이미 누군가 open 중 — fast
	                                                    * path: blob 은 이미 open 되어 있으므로
	                                                    * 재open 불필요, ref count 만 증가. */
		lvol->ref_count++;
		cb_fn(cb_arg, lvol, 0);                    /* [한국어] 즉시 성공 콜백 (동기 완료). */
		return;
	}

	req = calloc(1, sizeof(*req));                     /* [한국어] req 할당. */
	if (req == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for request structure\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;                                /* [한국어] 사용자 콜백 보관. */
	req->cb_arg = cb_arg;                              /* [한국어] 사용자 컨텍스트. */
	req->lvol = lvol;                                  /* [한국어] open 대상. */

	spdk_blob_open_opts_init(&opts, sizeof(opts));     /* [한국어] opts 기본값 채움 — ABI 안전
	                                                    * 초기화 (sizeof 전달로 향후 필드 추가
	                                                    * 시 forward-compatible). */
	opts.clear_method = lvol->clear_method;            /* [한국어] cluster 초기화 정책 전달 —
	                                                    * blob 가 처음 cluster 를 할당할 때
	                                                    * UNMAP 할지 0 으로 채울지 결정. */

	spdk_bs_open_blob_ext(lvol->lvol_store->blobstore, lvol->blob_id, &opts, lvol_open_cb, req);
	                                                    /* [한국어] BS 에 blob open 요청 —
	                                                     * 비동기. 완료 시 lvol_open_cb 가
	                                                     * lvol->blob 갱신. */
}

/*
 * [한국어]
 * bs_unload_with_error_cb - BS unload 완료를 받아 사용자에게 원래 에러로 통지하는 콜백
 *
 * @cb_arg: spdk_lvs_with_handle_req — 안에 lvserrno 가 이미 채워져 있다.
 * @lvolerrno: BS unload 자체의 결과 (보통 무시 — 원래 에러를 보고하기 위해 unload 만 한 것).
 *
 * 동기: lvs_load 도중에 super blob 손상/UUID 누락 등 치명적 에러를 만나면 BS 를 깔끔히
 * unload 한 뒤 사용자에게 원래 에러 코드를 돌려주어야 한다. 이 함수는 그 마지막 단계.
 */
static void
bs_unload_with_error_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	                                                    /* [한국어] req 캐스트. */

	req->cb_fn(req->cb_arg, NULL, req->lvserrno);      /* [한국어] 원래의 lvserrno (예: -EINVAL,
	                                                    * -EEXIST 등) 로 사용자 콜백 호출 —
	                                                    * lvol_store handle 은 NULL. unload 의
	                                                    * lvolerrno 자체는 무시. */
	free(req);                                         /* [한국어] req 해제. */
}

/*
 * [한국어]
 * load_next_lvol - LVS load 시 BS 의 모든 blob 을 순회하며 각 blob 을 lvol 핸들로 변환
 *
 * @cb_arg: spdk_lvs_with_handle_req.
 * @blob: BS iterator 가 현재 가리키는 blob (-ENOENT 일 때는 NULL).
 * @lvolerrno: 0 = 다음 blob, -ENOENT = iteration 완료, 음수 = 에러.
 *
 * 동기: spdk_bs_iter_first/iter_next 의 콜백. BS 안의 모든 blob 을 순회하며 각 blob 의
 * "name", "uuid" xattr 를 읽어 lvol 핸들 메모리에 채우고 lvs->lvols TAILQ 에 등록한다.
 * super blob 은 이미 처리되었으므로 skip. 손상된 blob 은 에러 표식 후 다음으로.
 *
 * 동작 단계:
 *   1) lvolerrno == -ENOENT (iteration 종료):
 *      - 누적 lvserrno == 0: lvs->load_esnaps = true (이제부터 esnap dev create 가능),
 *        사용자 콜백에 lvs 전달, req free. 정상 종료.
 *      - 누적 에러 발생: 모든 lvol 핸들 free, lvs free, BS unload 후 사용자에게 통지.
 *   2) lvolerrno < 0 (iteration 자체 에러): 에러 기록 후 invalid 라벨로 점프해 다음 blob 시도.
 *   3) blob_id == super_blob_id: 이미 메타로 처리됨 — skip.
 *   4) 일반 blob: lvol 핸들 calloc, blob_id/lvol_store 채움, "uuid"/"name" xattr 읽어 옮김.
 *      손상된 uuid 는 NULL UUID 로 두고 unique_id 를 lvs UUID + "_" + blob_id 로 fallback.
 *      name 손상은 치명적 — req->lvserrno = -EINVAL.
 *   5) lvols TAILQ 에 등록, lvs->lvol_count++, 다음 blob 으로.
 *
 * 실행 컨텍스트: lvs->thread (BS metadata thread). 본 함수는 자기 자신을 비동기적으로
 *               다시 trigger 하는 형태로 모든 blob 을 차례로 처리한다 (state-machine).
 *
 * 호출 체인:
 *   close_super_cb → spdk_bs_iter_first(load_next_lvol) → [load_next_lvol]
 *     → spdk_bs_iter_next(load_next_lvol) → [load_next_lvol] → ... → 종료시 사용자 cb_fn.
 */
static void
load_next_lvol(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;     /* [한국어] req 컨텍스트. */
	struct spdk_lvol_store *lvs = req->lvol_store;     /* [한국어] LVS 핸들. */
	struct spdk_blob_store *bs = lvs->blobstore;       /* [한국어] BS 핸들. */
	struct spdk_lvol *lvol, *tmp;                      /* [한국어] 새 lvol / 정리용 임시. */
	spdk_blob_id blob_id;                              /* [한국어] 현재 blob 의 64-bit ID. */
	const char *attr;                                  /* [한국어] xattr 값 포인터. */
	size_t value_len;                                  /* [한국어] xattr 값 길이. */
	int rc;                                            /* [한국어] 임시 반환 코드. */

	if (lvolerrno == -ENOENT) {                        /* [한국어] BS iteration 종료 신호. */
		/* Finished iterating */
		if (req->lvserrno == 0) {                  /* [한국어] 누적 에러 없음 — 정상. */
			lvs->load_esnaps = true;           /* [한국어] 이제부터 esnap clone 의
			                                    * bs_dev create 가 활성화된다 — load
			                                    * 도중의 임시 open 을 esnap 까지
			                                    * 끌어가지 않기 위한 게이트. */
			req->cb_fn(req->cb_arg, lvs, req->lvserrno);
			                                    /* [한국어] 사용자 콜백에 LVS 전달. */
			free(req);                         /* [한국어] req 해제. */
		} else {                                   /* [한국어] 도중에 에러 누적 — 전체 롤백. */
			TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
			                                    /* [한국어] 그동안 등록된 모든 lvol
			                                     * 정리. */
				TAILQ_REMOVE(&lvs->lvols, lvol, link);
				lvol_free(lvol);           /* [한국어] 핸들 메모리 회수. */
			}
			lvs_free(lvs);                     /* [한국어] LVS 핸들 정리. */
			spdk_bs_unload(bs, bs_unload_with_error_cb, req);
			                                    /* [한국어] BS 도 깔끔히 unload 후
			                                     * 사용자에게 원래 에러로 통지. */
		}
		return;
	} else if (lvolerrno < 0) {                        /* [한국어] iteration 자체의 에러. */
		SPDK_ERRLOG("Failed to fetch blobs list\n");
		req->lvserrno = lvolerrno;                 /* [한국어] 누적 에러로 기록. 종료
		                                            * 콜백에서 롤백 경로 진입. */
		goto invalid;                              /* [한국어] 다음 blob 시도. */
	}

	blob_id = spdk_blob_get_id(blob);                  /* [한국어] 64-bit blob 식별자. */

	if (blob_id == lvs->super_blob_id) {               /* [한국어] super blob 은 lvs 메타 자체
	                                                    * — load_super 단계에서 이미 처리. */
		SPDK_INFOLOG(lvol, "found superblob %"PRIu64"\n", (uint64_t)blob_id);
		spdk_bs_iter_next(bs, blob, load_next_lvol, req);
		                                            /* [한국어] super 는 skip 하고 다음. */
		return;
	}

	lvol = calloc(1, sizeof(*lvol));                   /* [한국어] 새 lvol 핸들. */
	if (!lvol) {
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		req->lvserrno = -ENOMEM;                   /* [한국어] 누적 에러. */
		goto invalid;
	}

	/*
	 * Do not store a reference to blob now because spdk_bs_iter_next() will close it.
	 * Storing blob_id for future lookups is fine.
	 */
	/* [한국어] 위 영문 주석: BS iterator 가 다음 호출 시 현재 blob 을 close 하므로 여기서
	 *           lvol->blob 으로 보관하면 dangling 이 된다. blob_id 만 저장하고 추후 사용자가
	 *           spdk_lvol_open 으로 open 해야 한다. */
	lvol->blob_id = blob_id;                           /* [한국어] 64-bit ID 저장 — open 시 키. */
	lvol->lvol_store = lvs;                            /* [한국어] 부모 역참조. */

	rc = spdk_blob_get_xattr_value(blob, "uuid", (const void **)&attr, &value_len);
	                                                    /* [한국어] blob 의 "uuid" xattr 추출. */
	if (rc != 0 || value_len != SPDK_UUID_STRING_LEN || attr[SPDK_UUID_STRING_LEN - 1] != '\0' ||
	    spdk_uuid_parse(&lvol->uuid, attr) != 0) {
	                                                    /* [한국어] xattr 누락 / 길이 오류 / null
	                                                     * 종결자 누락 / 파싱 실패 — 손상으로 판단. */
		SPDK_INFOLOG(lvol, "Missing or corrupt lvol uuid\n");
		spdk_uuid_set_null(&lvol->uuid);           /* [한국어] uuid 를 NULL UUID 로
		                                            * 마킹하고 unique_id 를 fallback. */
	}
	spdk_uuid_fmt_lower(lvol->uuid_str, sizeof(lvol->uuid_str), &lvol->uuid);
	                                                    /* [한국어] 36자 lowercase 문자열로 변환. */

	if (!spdk_uuid_is_null(&lvol->uuid)) {             /* [한국어] 정상 UUID — unique_id 도 같게. */
		snprintf(lvol->unique_id, sizeof(lvol->unique_id), "%s", lvol->uuid_str);
	} else {                                           /* [한국어] UUID 손상 fallback —
	                                                    * "<lvs_uuid>_<blob_id>" 형태로 임시
	                                                    * 식별자 부여. 로깅/RPC 표시에만 사용. */
		spdk_uuid_fmt_lower(lvol->unique_id, sizeof(lvol->unique_id), &lvol->lvol_store->uuid);
		value_len = strlen(lvol->unique_id);       /* [한국어] 현재 길이. */
		snprintf(lvol->unique_id + value_len, sizeof(lvol->unique_id) - value_len, "_%"PRIu64,
			 (uint64_t)blob_id);              /* [한국어] 뒤에 "_<blob_id>" 부착. */
	}

	rc = spdk_blob_get_xattr_value(blob, "name", (const void **)&attr, &value_len);
	                                                    /* [한국어] "name" xattr 추출. */
	if (rc != 0 || value_len > SPDK_LVOL_NAME_MAX) {   /* [한국어] 누락 또는 너무 김 — 치명적. */
		SPDK_ERRLOG("Cannot assign lvol name\n");
		lvol_free(lvol);                           /* [한국어] 방금 만든 핸들 정리. */
		req->lvserrno = -EINVAL;                   /* [한국어] 누적 에러. */
		goto invalid;
	}

	snprintf(lvol->name, sizeof(lvol->name), "%s", attr);
	                                                    /* [한국어] 이름 복사. */

	TAILQ_INSERT_TAIL(&lvs->lvols, lvol, link);        /* [한국어] LVS 의 lvol 컬렉션에 등록. */

	lvs->lvol_count++;                                 /* [한국어] 카운트 증가 — RPC 출력 등에 사용. */

	SPDK_INFOLOG(lvol, "added lvol %s (%s)\n", lvol->unique_id, lvol->uuid_str);

invalid:
	spdk_bs_iter_next(bs, blob, load_next_lvol, req);  /* [한국어] 다음 blob 으로 — 에러가
	                                                    * 있어도 일단 끝까지 순회하여 누적 에러
	                                                    * 와 함께 -ENOENT 도달 시 일괄 롤백. */
}

/*
 * [한국어]
 * close_super_cb - super blob close 후 본격적인 lvol iteration 을 시작하는 콜백
 *
 * @cb_arg: spdk_lvs_with_handle_req.
 * @lvolerrno: super blob close 의 결과.
 *
 * 동기: lvs_read_uuid 가 super blob 의 uuid/name 을 읽고 lvs 메타를 채운 뒤 이 콜백에서
 * super blob 을 close 한다. 정상적으로 close 되면 BS 의 blob iteration 을 시작해 모든
 * lvol blob 들을 메모리 핸들로 변환한다 (load_next_lvol).
 *
 * 호출 체인: lvs_read_uuid → spdk_blob_close → [close_super_cb] → spdk_bs_iter_first → load_next_lvol.
 */
static void
close_super_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;     /* [한국어] LVS 핸들. */
	struct spdk_blob_store *bs = lvs->blobstore;       /* [한국어] BS 핸들 (실패 unload 용). */

	if (lvolerrno != 0) {                              /* [한국어] super close 자체 실패. */
		SPDK_INFOLOG(lvol, "Could not close super blob\n");
		lvs_free(lvs);                             /* [한국어] LVS 핸들 정리. */
		req->lvserrno = -ENODEV;                   /* [한국어] 사용자에게 통지할 에러 코드. */
		spdk_bs_unload(bs, bs_unload_with_error_cb, req);
		                                            /* [한국어] BS 도 깔끔히 unload. */
		return;
	}

	/* Start loading lvols */
	/* [한국어] 위 영문 주석대로: 메타가 정상 → BS 의 모든 blob 을 순회하며 lvol 핸들 채움. */
	spdk_bs_iter_first(lvs->blobstore, load_next_lvol, req);
	                                                    /* [한국어] BS 의 첫 blob 부터 시작. */
}

/*
 * [한국어]
 * close_super_blob_with_error_cb - 에러 상황에서 super blob 을 close 한 뒤 BS 도 unload
 *
 * @cb_arg: spdk_lvs_with_handle_req — 이미 lvserrno 가 채워진 상태.
 * @lvolerrno: close 결과 (사용 안 함 — 어차피 에러 통지가 목적).
 *
 * 동기: lvs_read_uuid 가 uuid/name 검증에서 실패한 경우, 임시로 open 된 super blob 을
 * 우선 close 한 뒤 BS 도 unload 해야 한다. 이 함수가 그 cleanup 의 두 번째 단계.
 */
static void
close_super_blob_with_error_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;     /* [한국어] LVS 핸들. */
	struct spdk_blob_store *bs = lvs->blobstore;       /* [한국어] BS 핸들. */

	lvs_free(lvs);                                     /* [한국어] LVS 핸들 즉시 free. */

	spdk_bs_unload(bs, bs_unload_with_error_cb, req);  /* [한국어] BS unload 후 사용자 에러 통지. */
}

/*
 * [한국어]
 * lvs_read_uuid - super blob 이 open 된 뒤 그 안의 uuid/name xattr 을 읽어 lvs 메타에 반영
 *
 * @cb_arg: spdk_lvs_with_handle_req.
 * @blob: BS 가 open 한 super blob 핸들.
 * @lvolerrno: open 결과.
 *
 * 동기: BS 의 super blob 은 LVS 자체의 UUID 와 이름을 보존한다. load 시 이를 읽어
 * lvs->uuid / lvs->name 을 복원하고, 전역 g_lvol_stores 리스트에 등록한 뒤(중복 검출),
 * super_blob_id 를 저장하고 close 콜백 체인으로 진행한다.
 *
 * 동작 단계:
 *   1) open 자체 실패: 정리 경로 (BS unload).
 *   2) "uuid" xattr 검증/파싱 — 실패 시 close_super_blob_with_error_cb 로 -EINVAL.
 *   3) "name" xattr 검증/복사 — 실패 시 동일.
 *   4) add_lvs_to_list — 동일 이름 LVS 가 이미 있으면 -EEXIST 로 정리.
 *   5) lvs->super_blob_id 저장, super blob close → close_super_cb 로 점프.
 *
 * 실행 컨텍스트: lvs->thread.
 */
static void
lvs_read_uuid(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;     /* [한국어] LVS 핸들. */
	struct spdk_blob_store *bs = lvs->blobstore;       /* [한국어] BS 핸들. */
	const char *attr;                                  /* [한국어] xattr 값 포인터. */
	size_t value_len;                                  /* [한국어] xattr 값 길이. */
	int rc;                                            /* [한국어] 임시 반환 코드. */

	if (lvolerrno != 0) {                              /* [한국어] super open 실패. */
		SPDK_INFOLOG(lvol, "Could not open super blob\n");
		lvs_free(lvs);
		req->lvserrno = -ENODEV;
		spdk_bs_unload(bs, bs_unload_with_error_cb, req);
		return;
	}

	rc = spdk_blob_get_xattr_value(blob, "uuid", (const void **)&attr, &value_len);
	                                                    /* [한국어] super blob 의 "uuid" xattr. */
	if (rc != 0 || value_len != SPDK_UUID_STRING_LEN || attr[SPDK_UUID_STRING_LEN - 1] != '\0') {
	                                                    /* [한국어] xattr 누락 / 길이 불일치 /
	                                                     * null-terminator 누락 — 손상. */
		SPDK_INFOLOG(lvol, "degraded_set or incorrect UUID\n");
		req->lvserrno = -EINVAL;                   /* [한국어] 사용자 에러. */
		spdk_blob_close(blob, close_super_blob_with_error_cb, req);
		                                            /* [한국어] super 우선 close 후 BS unload. */
		return;
	}

	if (spdk_uuid_parse(&lvs->uuid, attr)) {           /* [한국어] 36자 문자열을 spdk_uuid 로
	                                                    * 파싱 — 형식 오류 시 0이 아닌 값. */
		SPDK_INFOLOG(lvol, "incorrect UUID '%s'\n", attr);
		req->lvserrno = -EINVAL;
		spdk_blob_close(blob, close_super_blob_with_error_cb, req);
		return;
	}

	rc = spdk_blob_get_xattr_value(blob, "name", (const void **)&attr, &value_len);
	                                                    /* [한국어] super blob 의 "name" xattr. */
	if (rc != 0 || value_len > SPDK_LVS_NAME_MAX) {    /* [한국어] 누락 또는 너무 김. */
		SPDK_INFOLOG(lvol, "degraded_set or invalid name\n");
		req->lvserrno = -EINVAL;
		spdk_blob_close(blob, close_super_blob_with_error_cb, req);
		return;
	}

	snprintf(lvs->name, sizeof(lvs->name), "%s", attr);
	                                                    /* [한국어] LVS 이름 복사 — 안전 truncate. */

	rc = add_lvs_to_list(lvs);                         /* [한국어] 전역 리스트에 등록 (중복 검사). */
	if (rc) {                                          /* [한국어] 같은 이름의 LVS 가 이미 있다. */
		SPDK_INFOLOG(lvol, "lvolstore with name %s already exists\n", lvs->name);
		req->lvserrno = -EEXIST;                   /* [한국어] 기존 LVS 와 충돌. */
		spdk_blob_close(blob, close_super_blob_with_error_cb, req);
		return;
	}

	lvs->super_blob_id = spdk_blob_get_id(blob);       /* [한국어] super blob 의 64-bit ID 보관 —
	                                                    * 이후 rename/destroy 가 다시 open 할 때 키. */

	spdk_blob_close(blob, close_super_cb, req);        /* [한국어] super 는 한 번 close 한 뒤
	                                                    * lvol blob iteration 시작. */
}

/*
 * [한국어]
 * lvs_open_super - super blob 의 ID 를 받아 실제 open 단계로 진입
 *
 * @cb_arg: spdk_lvs_with_handle_req.
 * @blobid: super blob 의 64-bit ID (BS 가 metadata region 에서 추출).
 * @lvolerrno: spdk_bs_get_super 의 결과 (ENOENT 면 super blob 자체가 없음).
 *
 * 호출 체인: spdk_bs_load → lvs_load_cb → spdk_bs_get_super → [lvs_open_super]
 *           → spdk_bs_open_blob → lvs_read_uuid.
 */
static void
lvs_open_super(void *cb_arg, spdk_blob_id blobid, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;     /* [한국어] LVS 핸들. */
	struct spdk_blob_store *bs = lvs->blobstore;       /* [한국어] BS 핸들. */

	if (lvolerrno != 0) {                              /* [한국어] super blob 자체가 없음 →
	                                                    * 이 BS 는 LVS 가 아니다. */
		SPDK_INFOLOG(lvol, "Super blob not found\n");
		lvs_free(lvs);
		req->lvserrno = -ENODEV;
		spdk_bs_unload(bs, bs_unload_with_error_cb, req);
		return;
	}

	spdk_bs_open_blob(bs, blobid, lvs_read_uuid, req); /* [한국어] super blob open → uuid/name 읽기. */
}

/*
 * [한국어]
 * lvs_load_cb - spdk_bs_load 의 완료 콜백 (BS 자체가 메모리에 로드됨)
 *
 * @cb_arg: spdk_lvs_with_handle_req.
 * @bs: 로드된 BS 핸들 (실패 시 NULL).
 * @lvolerrno: BS load 결과.
 *
 * 동기: BS 가 load 되면 super blob 의 ID 를 BS 에 물어봐야 한다. spdk_bs_get_super 의
 * 결과는 lvs_open_super 에서 처리.
 *
 * 호출 체인: lvs_load → spdk_bs_load → [lvs_load_cb] → spdk_bs_get_super → lvs_open_super.
 */
static void
lvs_load_cb(void *cb_arg, struct spdk_blob_store *bs, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;     /* [한국어] LVS 핸들. */

	if (lvolerrno != 0) {                              /* [한국어] BS load 자체 실패 (예: signature
	                                                    * 없음, IO 에러, format 미스매치). */
		req->cb_fn(req->cb_arg, NULL, lvolerrno);  /* [한국어] 사용자에게 즉시 통지. */
		lvs_free(lvs);                             /* [한국어] LVS 핸들 정리. */
		free(req);                                 /* [한국어] req 해제. */
		return;
	}

	lvs->blobstore = bs;                               /* [한국어] BS 포인터 저장. */
	lvs->bs_dev = req->bs_dev;                         /* [한국어] backend bs_dev 보존
	                                                    * (이후 unload/destroy 시 공유 자원
	                                                    * 정리에 필요할 수 있음). */

	spdk_bs_get_super(bs, lvs_open_super, req);        /* [한국어] super blob ID 추출 요청. */
}

/*
 * [한국어]
 * lvs_bs_opts_init - lvol 라이브러리가 사용하는 spdk_bs_opts 의 공통 기본값 세팅
 *
 * @opts: 채울 BS opts.
 *
 * 동기: lvol 코드는 BS 호출 전에 max_channel_ops 등을 일관되게 설정해야 한다. 이를
 * 한 곳에 모아두어 init/load/grow 가 동일한 정책으로 BS 를 다루도록 한다.
 *
 * 호출자: lvs_load / spdk_lvs_init / spdk_lvs_grow.
 */
static void
lvs_bs_opts_init(struct spdk_bs_opts *opts)
{
	spdk_bs_opts_init(opts, sizeof(*opts));            /* [한국어] BS 라이브러리의 기본값으로
	                                                    * 채움 — ABI 안전 초기화. */
	opts->max_channel_ops = SPDK_LVOL_BLOB_OPTS_CHANNEL_OPS;
	                                                    /* [한국어] lvol 의 큐 깊이 정책 (512). */
}

/*
 * [한국어]
 * lvs_load - LVS load 코어 (옵션 유무 두 진입점이 공유)
 *
 * @bs_dev: backend bdev 위에 spdk_bdev_create_bs_dev() 로 만든 BS device.
 * @_lvs_opts: 사용자 지정 lvs_opts (NULL 가능).
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동기: spdk_lvs_load 와 spdk_lvs_load_ext 가 동일한 본체를 공유하기 위해 분리된 internal
 * 함수. opts 가 주어지면 esnap_bs_dev_create 콜백 등 LVS 의 외부 의존성을 설정한다.
 *
 * 동작 단계:
 *   1) 인자 검증.
 *   2) lvs_opts 기본값 + opts_size 기반 안전 복사.
 *   3) req 와 LVS 핸들 calloc.
 *   4) BS opts 채우기 — bstype = "LVOLSTORE" (BS 의 magic 헤더 검증 키).
 *   5) esnap_bs_dev_create 가 사용자에게서 주어졌으면 lvs 에 저장하고 BS 에는
 *      lvs_esnap_bs_dev_create 라우터를 등록 (esnap_ctx = lvs).
 *   6) spdk_bs_load 호출 → lvs_load_cb 콜백 체인 시작.
 *
 * 실행 컨텍스트: 호출 thread = 향후 lvs->thread.
 *
 * 호출 체인:
 *   spdk_lvs_load / spdk_lvs_load_ext → [lvs_load] → spdk_bs_load → lvs_load_cb →
 *     spdk_bs_get_super → lvs_open_super → spdk_bs_open_blob → lvs_read_uuid →
 *       spdk_blob_close → close_super_cb → spdk_bs_iter_first → load_next_lvol(...) → 사용자 cb.
 */
static void
lvs_load(struct spdk_bs_dev *bs_dev, const struct spdk_lvs_opts *_lvs_opts,
	 spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_with_handle_req *req;              /* [한국어] 비동기 컨텍스트. */
	struct spdk_bs_opts bs_opts = {};                  /* [한국어] BS 로 전달할 opts. */
	struct spdk_lvs_opts lvs_opts;                     /* [한국어] lvs_opts 의 안전 복사본. */

	assert(cb_fn != NULL);                             /* [한국어] 콜백 필수. */

	if (bs_dev == NULL) {                              /* [한국어] BS 디바이스 가드. */
		SPDK_ERRLOG("Blobstore device does not exist\n");
		cb_fn(cb_arg, NULL, -ENODEV);
		return;
	}

	spdk_lvs_opts_init(&lvs_opts);                     /* [한국어] 기본값 채움. */
	if (_lvs_opts != NULL) {                           /* [한국어] 사용자 opts 가 있으면 안전 복사. */
		if (lvs_opts_copy(_lvs_opts, &lvs_opts) != 0) {
		                                            /* [한국어] opts_size 기반 ABI 검증. */
			SPDK_ERRLOG("Invalid options\n");
			cb_fn(cb_arg, NULL, -EINVAL);
			return;
		}
	}

	req = calloc(1, sizeof(*req));                     /* [한국어] req 할당. */
	if (req == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for request structure\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->lvol_store = lvs_alloc();                     /* [한국어] LVS 핸들 할당. owning thread
	                                                    * = 호출 thread. */
	if (req->lvol_store == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store\n");
		free(req);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;                                /* [한국어] 사용자 콜백 보관. */
	req->cb_arg = cb_arg;                              /* [한국어] 사용자 컨텍스트. */
	req->bs_dev = bs_dev;                              /* [한국어] backend bs_dev 보관. */

	lvs_bs_opts_init(&bs_opts);                        /* [한국어] BS opts 기본값 + 채널 깊이. */
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "LVOLSTORE");
	                                                    /* [한국어] BS magic header 검증용 type
	                                                     * 문자열. spdk_bs_load 가 디스크의
	                                                     * super block 의 bstype 와 비교 →
	                                                     * 일치하지 않으면 EINVAL. 다른 BS 사용자
	                                                     * (예: ftl) 와 충돌 방지. */

	if (lvs_opts.esnap_bs_dev_create != NULL) {        /* [한국어] 사용자가 esnap 지원 모드로
	                                                    * 호출했음 — vbdev_lvol 이 등록. */
		req->lvol_store->esnap_bs_dev_create = lvs_opts.esnap_bs_dev_create;
		                                            /* [한국어] 실제 사용자 콜백을 LVS 에 보관 —
		                                             * lvs_esnap_bs_dev_create 라우터가 dispatch. */
		bs_opts.esnap_bs_dev_create = lvs_esnap_bs_dev_create;
		                                            /* [한국어] BS 가 esnap clone 을 open 할 때
		                                             * 호출할 콜백을 lvol 라이브러리의
		                                             * 라우터로 등록. */
		bs_opts.esnap_ctx = req->lvol_store;       /* [한국어] 콜백의 첫 번째 인자(bs_ctx)로
		                                            * lvs 가 전달되도록. */
	}

	spdk_bs_load(bs_dev, &bs_opts, lvs_load_cb, req);  /* [한국어] BS load 시작 (디스크의
	                                                    * super block 검증, metadata 영역 매핑). */
}

/*
 * [한국어]
 * spdk_lvs_load - 기존 LVS 를 옵션 없이 로드 (공개 API, deprecated 아닌 단순화 형태)
 *
 * @bs_dev: backend bs_dev (호출자가 spdk_bdev_create_bs_dev 로 미리 생성).
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * esnap 미사용/단순 LVS 로드용. 옵션이 필요하면 spdk_lvs_load_ext 사용.
 */
void
spdk_lvs_load(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	lvs_load(bs_dev, NULL, cb_fn, cb_arg);             /* [한국어] 옵션 NULL 로 dispatch. */
}

/*
 * [한국어]
 * spdk_lvs_load_ext - 옵션을 동반한 LVS 로드 (공개 API)
 *
 * @bs_dev: backend bs_dev.
 * @opts: lvs_opts (esnap_bs_dev_create 등). opts_size 기반 ABI 안전.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * esnap 지원이나 cluster 정책 등을 명시할 때 사용. 호출자는 보통 vbdev_lvol.
 */
void
spdk_lvs_load_ext(struct spdk_bs_dev *bs_dev, const struct spdk_lvs_opts *opts,
		  spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	lvs_load(bs_dev, opts, cb_fn, cb_arg);             /* [한국어] 옵션 전달. */
}

/*
 * [한국어]
 * remove_bs_on_error_cb - 에러 정리 경로의 BS destroy 완료 콜백 (no-op)
 *
 * 에러로 LVS 생성을 포기할 때 BS 의 메타데이터까지 디스크에서 지운다. 이미 사용자에게
 * 에러 통지는 끝났으므로, BS destroy 의 결과는 더 이상 누구에게도 보고할 필요가 없다.
 */
static void
remove_bs_on_error_cb(void *cb_arg, int bserrno)
{
	/* [한국어] 의도적으로 비어 있음. BS destroy 의 결과는 더 이상 누구도 기다리지 않는다. */
}

/*
 * [한국어]
 * exit_error_lvs_req - LVS init 도중 super blob 생성 실패 시 통합 정리 경로
 *
 * @req: 진행 중이던 with_handle_req (사용자 cb 가 안에 들어 있음).
 * @lvs: 정리 대상 LVS.
 * @lvolerrno: 사용자에게 통지할 에러.
 *
 * 동기: super blob 생성 단계에서 어떤 콜백이든 실패하면 (1) 사용자에게 즉시 NULL 핸들 +
 * errno 로 통지, (2) 이미 spdk_bs_init 으로 디스크에 초기화된 BS 메타를 spdk_bs_destroy
 * 로 다시 지워서 disk 가 깨끗해지도록, (3) lvs/req 메모리 회수.
 */
static void
exit_error_lvs_req(struct spdk_lvs_with_handle_req *req, struct spdk_lvol_store *lvs, int lvolerrno)
{
	req->cb_fn(req->cb_arg, NULL, lvolerrno);          /* [한국어] 사용자 통지 (NULL handle). */
	spdk_bs_destroy(lvs->blobstore, remove_bs_on_error_cb, NULL);
	                                                    /* [한국어] 디스크에 기록된 BS 메타까지
	                                                     * 깨끗이 삭제 — 다음 시도가 깨끗하게
	                                                     * 시작될 수 있도록. */
	lvs_free(lvs);                                     /* [한국어] LVS 핸들 회수. */
	free(req);                                         /* [한국어] req 회수. */
}

/*
 * [한국어]
 * super_create_close_cb - super blob close 후 LVS init 의 사용자 콜백 호출 (정상 종료)
 *
 * @cb_arg: spdk_lvs_with_handle_req.
 * @lvolerrno: super close 결과.
 *
 * 호출 체인: ... → super_blob_set_cb → spdk_blob_close → [super_create_close_cb] → 사용자 cb_fn.
 */
static void
super_create_close_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno < 0) {                               /* [한국어] close 실패 (드물지만 IO 오류). */
		SPDK_ERRLOG("Lvol store init failed: could not close super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);   /* [한국어] BS destroy 까지 롤백. */
		return;
	}

	req->cb_fn(req->cb_arg, lvs, lvolerrno);           /* [한국어] 정상 — 사용자에게 LVS 핸들 전달. */
	free(req);                                         /* [한국어] req 해제. */
}

/*
 * [한국어]
 * super_blob_set_cb - super blob 의 xattr sync 완료 콜백 (uuid/name 디스크에 기록됨)
 *
 * @cb_arg: spdk_lvs_with_handle_req.
 * @lvolerrno: sync_md 결과.
 *
 * 호출 체인: super_blob_init_cb → spdk_blob_sync_md → [super_blob_set_cb] → spdk_blob_close
 *           → super_create_close_cb.
 */
static void
super_blob_set_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob *blob = lvs->super_blob;          /* [한국어] super blob 핸들. */

	if (lvolerrno < 0) {                               /* [한국어] xattr 디스크 기록 실패. */
		SPDK_ERRLOG("Lvol store init failed: could not set uuid for super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	spdk_blob_close(blob, super_create_close_cb, req); /* [한국어] super blob close 후 종료 콜백. */
}

/*
 * [한국어]
 * super_blob_init_cb - super blob 이 set_super 로 등록된 직후, 그 안에 uuid/name xattr 기록
 *
 * @cb_arg: spdk_lvs_with_handle_req.
 * @lvolerrno: spdk_bs_set_super 결과.
 *
 * 동기: super blob 자체는 아무 데이터도 없는 빈 blob 이지만, "uuid"/"name" xattr 를
 * 보관하여 LVS 식별의 영속 메타가 된다. xattr 두 개를 set 하고 sync_md 로 디스크에
 * 기록한다.
 */
static void
super_blob_init_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob *blob = lvs->super_blob;
	char uuid[SPDK_UUID_STRING_LEN];                   /* [한국어] UUID 문자열 임시 버퍼. */

	if (lvolerrno < 0) {                               /* [한국어] set_super 실패. */
		SPDK_ERRLOG("Lvol store init failed: could not set super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &lvs->uuid);
	                                                    /* [한국어] LVS UUID 를 36자 문자열로. */

	spdk_blob_set_xattr(blob, "uuid", uuid, sizeof(uuid));
	                                                    /* [한국어] super blob 에 UUID 기록 — load
	                                                     * 시 lvs_read_uuid 가 이 키를 읽음. */
	spdk_blob_set_xattr(blob, "name", lvs->name, strnlen(lvs->name, SPDK_LVS_NAME_MAX) + 1);
	                                                    /* [한국어] LVS 이름 기록. +1 은 null
	                                                     * terminator 까지 보존하기 위함. */
	spdk_blob_sync_md(blob, super_blob_set_cb, req);   /* [한국어] xattr 변경을 디스크에 동기화. */
}

/*
 * [한국어]
 * super_blob_create_open_cb - 새로 만든 super blob 을 open 한 콜백 — set_super 호출
 *
 * @cb_arg: spdk_lvs_with_handle_req.
 * @blob: open 된 super blob.
 * @lvolerrno: open 결과.
 *
 * 동작: blob 핸들을 lvs->super_blob 에 저장하고, BS 에 "이 blob 을 super 로 등록" 알림.
 */
static void
super_blob_create_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno < 0) {                               /* [한국어] open 실패. */
		SPDK_ERRLOG("Lvol store init failed: could not open super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	lvs->super_blob = blob;                            /* [한국어] init 도중 사용할 핸들 보관. */
	lvs->super_blob_id = spdk_blob_get_id(blob);       /* [한국어] 64-bit ID 기억. */

	spdk_bs_set_super(lvs->blobstore, lvs->super_blob_id, super_blob_init_cb, req);
	                                                    /* [한국어] BS metadata 에 "super = blobid"
	                                                     * 기록. load 시 spdk_bs_get_super 가
	                                                     * 이 값을 반환. */
}

/*
 * [한국어]
 * super_blob_create_cb - spdk_bs_create_blob 의 콜백 — 새 super blob 을 open 으로 진행
 *
 * @cb_arg: spdk_lvs_with_handle_req.
 * @blobid: 갓 만들어진 blob 의 64-bit ID.
 * @lvolerrno: create 결과.
 */
static void
super_blob_create_cb(void *cb_arg, spdk_blob_id blobid, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs;

	if (lvolerrno < 0) {                               /* [한국어] create 실패. */
		SPDK_ERRLOG("Lvol store init failed: could not create super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	bs = req->lvol_store->blobstore;                   /* [한국어] BS 핸들. */

	spdk_bs_open_blob(bs, blobid, super_blob_create_open_cb, req);
	                                                    /* [한국어] 새로 만든 super blob open. */
}

/*
 * [한국어]
 * lvs_init_cb - spdk_bs_init 의 완료 콜백 — BS 가 디스크에 새로 포맷되었음
 *
 * @cb_arg: spdk_lvs_with_handle_req.
 * @bs: 새로 init 된 BS 핸들.
 * @lvserrno: bs_init 결과.
 *
 * 호출 체인: spdk_lvs_init → spdk_bs_init → [lvs_init_cb] → spdk_bs_create_blob →
 *           super_blob_create_cb → ... → 사용자 cb.
 */
static void
lvs_init_cb(void *cb_arg, struct spdk_blob_store *bs, int lvserrno)
{
	struct spdk_lvs_with_handle_req *lvs_req = cb_arg;
	struct spdk_lvol_store *lvs = lvs_req->lvol_store;

	if (lvserrno != 0) {                               /* [한국어] BS 포맷 실패 (IO 에러 등). */
		assert(bs == NULL);                        /* [한국어] 실패 시 BS 핸들은 항상 NULL. */
		lvs_req->cb_fn(lvs_req->cb_arg, NULL, lvserrno);
		                                            /* [한국어] 사용자 즉시 통지. */
		SPDK_ERRLOG("Lvol store init failed: could not initialize blobstore\n");
		lvs_free(lvs);                             /* [한국어] LVS 핸들 회수. */
		free(lvs_req);
		return;
	}

	assert(bs != NULL);                                /* [한국어] 성공 → BS 는 유효해야 함. */
	lvs->blobstore = bs;                               /* [한국어] BS 보관. */

	SPDK_INFOLOG(lvol, "Lvol store initialized\n");

	/* create super blob */
	/* [한국어] super blob 생성 — 빈 blob 이지만 uuid/name xattr 를 보관하는 LVS 메타 영역. */
	spdk_bs_create_blob(lvs->blobstore, super_blob_create_cb, lvs_req);
}

/*
 * [한국어]
 * spdk_lvs_opts_init - 사용자에게 노출되는 spdk_lvs_opts 의 기본값 채움 (공개 API)
 *
 * @o: 채울 opts 구조체.
 *
 * 동기: SPDK 공개 헤더의 opt 패턴 — 라이브러리가 기본값을 알고 있고, 사용자는 변경할
 * 필드만 덮어쓴다. ABI 변경에 대비해 opts_size 도 자동 채워진다.
 *
 * 기본값:
 *   - cluster_sz = 4MB (SPDK_LVS_OPTS_CLUSTER_SZ).
 *   - clear_method = UNMAP (NVMe DEALLOCATE — 가능 시 가장 빠른 데이터 초기화).
 *   - num_md_pages_per_cluster_ratio = 100 (= cluster 수와 동일한 메타 페이지 수).
 *   - opts_size = sizeof — 호출자 ABI 식별용.
 */
void
spdk_lvs_opts_init(struct spdk_lvs_opts *o)
{
	memset(o, 0, sizeof(*o));                          /* [한국어] 모든 필드 0으로 초기화 — 안전. */
	o->cluster_sz = SPDK_LVS_OPTS_CLUSTER_SZ;          /* [한국어] cluster size = 4MB. blobstore
	                                                    * 의 cluster 단위 — lvol 의 최소 할당 단위. */
	o->clear_method = LVS_CLEAR_WITH_UNMAP;            /* [한국어] cluster 회수 시 backend 에
	                                                    * UNMAP 명령. NVMe SSD 에서 가장 효율적. */
	o->num_md_pages_per_cluster_ratio = 100;           /* [한국어] cluster 100개당 metadata page
	                                                    * 100개 — 즉 cluster 와 동일 수의 md page.
	                                                    * blob 메타가 충분히 들어갈 정도로 큼. */
	o->opts_size = sizeof(*o);                         /* [한국어] ABI 식별. */
}

/*
 * [한국어]
 * lvs_opts_copy - opts_size 기반 ABI 안전 필드별 복사
 *
 * @src: 사용자 입력. opts_size 가 호출 시점의 라이브러리 ABI 와 일치하지 않을 수 있음
 *       (구버전 호출자 / 신버전 라이브러리 또는 그 반대).
 * @dst: 라이브러리 내부에서 사용할 opts. spdk_lvs_opts_init 으로 미리 초기화됨.
 * @return: 0 = 성공, -1 = 실패 (opts_size = 0).
 *
 * 동기: SPDK 의 ABI forward/backward 호환성 패턴. 호출자가 작성된 시점의 sizeof 값이
 * opts_size 로 들어오므로, 각 필드가 실제로 입력 buffer 안에 있는지(FIELD_OK)를 검사한
 * 뒤 안전하게 복사한다.
 *
 * SPDK_STATIC_ASSERT 가 있는 이유: 새 필드를 추가하면 sizeof 가 변하므로 assert 실패 →
 * 빌드 시점에 SET_FIELD 를 추가하라는 reminder 가 된다.
 */
static inline int
lvs_opts_copy(const struct spdk_lvs_opts *src, struct spdk_lvs_opts *dst)
{
	if (src->opts_size == 0) {                         /* [한국어] opts_size 0 = 호출자가
	                                                    * spdk_lvs_opts_init 을 호출하지 않은
	                                                    * 잘못된 사용. */
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return -1;
	}
#define FIELD_OK(field) \
        offsetof(struct spdk_lvs_opts, field) + sizeof(src->field) <= src->opts_size
        /* [한국어] 매크로: src 의 opts_size 안에 해당 field 의 끝까지 포함되는가? — 호출자
         *           ABI 가 그 필드를 알고 있는가의 판단. */

#define SET_FIELD(field) \
        if (FIELD_OK(field)) { \
                dst->field = src->field; \
        } \
        /* [한국어] 매크로: ABI 안에 있는 경우에만 복사. 모르는 필드는 dst 의 init 기본값 유지. */

	SET_FIELD(cluster_sz);                             /* [한국어] cluster 크기. */
	SET_FIELD(clear_method);                           /* [한국어] 데이터 영역 초기화 정책. */
	if (FIELD_OK(name)) {                              /* [한국어] name 은 배열이라 별도 처리. */
		memcpy(&dst->name, &src->name, sizeof(dst->name));
	}
	SET_FIELD(num_md_pages_per_cluster_ratio);         /* [한국어] metadata 페이지 비율. */
	SET_FIELD(opts_size);                              /* [한국어] 자기 자신 — 일관성. */
	SET_FIELD(esnap_bs_dev_create);                    /* [한국어] external snapshot 콜백. */
	SET_FIELD(md_page_size);                           /* [한국어] metadata page size (4KB 등). */

	dst->opts_size = src->opts_size;                   /* [한국어] dst 의 opts_size 를 호출자
	                                                    * 의 것으로 갱신 — 추후 라이브러리가
	                                                    * "어디까지 알고 있는 호출자였는지"
	                                                    * 다시 참조할 수 있도록. */

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	/* [한국어] 위 영문 reminder: 필드 추가 시 sizeof 가 바뀌므로 이 assert 가 깨진다. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_lvs_opts) == 92, "Incorrect size");
	                                                    /* [한국어] 빌드 타임 sanity — 새 필드가
	                                                     * 추가되면 92 가 갱신되어야 하고 위에
	                                                     * SET_FIELD 도 추가되어야 함을 강제. */

#undef FIELD_OK
#undef SET_FIELD

	return 0;                                          /* [한국어] 성공. */
}

/*
 * [한국어]
 * setup_lvs_opts - lvs_opts → bs_opts 매핑 (spdk_lvs_init 의 헬퍼)
 *
 * @bs_opts: 채울 BS opts.
 * @o: 사용자 lvs opts.
 * @total_clusters: backend 디바이스의 총 cluster 수 (블록 수 / cluster_sz).
 * @esnap_ctx: BS 가 esnap_bs_dev_create 콜백을 호출할 때 첫 인자로 사용 (= LVS 자체).
 *
 * 동기: lvs_opts 의 사용자 의미를 BS 가 이해하는 bs_opts 필드로 변환. md page 수는
 * cluster 수의 비율로 결정된다 (총 cluster 수 × 비율 / 100).
 */
static void
setup_lvs_opts(struct spdk_bs_opts *bs_opts, struct spdk_lvs_opts *o, uint32_t total_clusters,
	       void *esnap_ctx)
{
	assert(o != NULL);                                 /* [한국어] 내부 호출 invariant. */
	lvs_bs_opts_init(bs_opts);                         /* [한국어] BS opts 기본값 + lvol 채널 깊이. */
	bs_opts->cluster_sz = o->cluster_sz;               /* [한국어] cluster 크기 그대로 전달. */
	bs_opts->clear_method = (enum bs_clear_method)o->clear_method;
	                                                    /* [한국어] enum 캐스트 (1:1 매핑). */
	bs_opts->num_md_pages = (o->num_md_pages_per_cluster_ratio * total_clusters) / 100;
	                                                    /* [한국어] metadata page 수 = ratio% ×
	                                                     * total_clusters / 100. ratio=100 일
	                                                     * 때 cluster 수와 동일. */
	bs_opts->md_page_size = o->md_page_size;           /* [한국어] md page 크기 (보통 4KB). */
	bs_opts->esnap_bs_dev_create = o->esnap_bs_dev_create;
	                                                    /* [한국어] esnap 콜백 그대로 전달.
	                                                     * 단, 본 init 경로에서는 lvol 라우터
	                                                     * 를 거치지 않고 사용자 콜백을 직접
	                                                     * 등록 (init 시점에는 esnap clone 이
	                                                     * 없으니 immediate 호출도 없음). */
	bs_opts->esnap_ctx = esnap_ctx;                    /* [한국어] 콜백의 첫 인자 (= LVS 핸들). */
	snprintf(bs_opts->bstype.bstype, sizeof(bs_opts->bstype.bstype), "LVOLSTORE");
	                                                    /* [한국어] BS magic. */
}

/*
 * [한국어]
 * spdk_lvs_init - 새 LVS 를 backend bs_dev 위에 초기화 (공개 API)
 *
 * @bs_dev: backend bdev 위에 spdk_bdev_create_bs_dev 로 만든 BS 디바이스. 호출자가 소유.
 * @o: 사용자 lvs_opts (NULL 금지). 최소 name, opts_size 는 채워야 함.
 * @cb_fn: 완료 콜백 (성공 시 LVS 핸들 전달).
 * @cb_arg: 사용자 컨텍스트.
 * @return: 0 = 비동기 시작 성공, 음수 errno = 즉시 실패. 비동기 결과는 cb_fn 으로.
 *
 * 동기: 새로운 디스크에 LVS 를 만든다. 디스크의 기존 데이터는 BS init 에 의해 덮어쓰여짐
 * (super block + metadata region 포맷). 따라서 주의해서 호출해야 함.
 *
 * 동작 단계:
 *   1) 인자 검증 + opts 안전 복사.
 *   2) cluster_sz / blocklen 정수 배수 검증.
 *   3) total_clusters = blockcnt / (cluster_sz / blocklen) 계산.
 *   4) LVS 핸들 alloc, BS opts 매핑.
 *   5) 이름 길이 검증 (1 ~ MAX-1 자).
 *   6) UUID 생성, 이름 복사.
 *   7) 전역 리스트 등록 (이름 중복 검출 — EEXIST).
 *   8) req alloc + spdk_bs_init 호출 → 콜백 체인 시작.
 *
 * 호출 체인:
 *   사용자/RPC → [spdk_lvs_init] → spdk_bs_init → lvs_init_cb →
 *     spdk_bs_create_blob → super_blob_create_cb → spdk_bs_open_blob →
 *     super_blob_create_open_cb → spdk_bs_set_super → super_blob_init_cb →
 *     spdk_blob_set_xattr × 2 → spdk_blob_sync_md → super_blob_set_cb →
 *     spdk_blob_close → super_create_close_cb → 사용자 cb_fn.
 */
int
spdk_lvs_init(struct spdk_bs_dev *bs_dev, struct spdk_lvs_opts *o,
	      spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs;                       /* [한국어] 새 LVS 핸들. */
	struct spdk_lvs_with_handle_req *lvs_req;          /* [한국어] 비동기 컨텍스트. */
	struct spdk_bs_opts opts = {};                     /* [한국어] BS init opts. */
	struct spdk_lvs_opts lvs_opts;                     /* [한국어] 사용자 opts 의 안전 복사본. */
	uint32_t total_clusters;                           /* [한국어] 디바이스 안의 cluster 수. */
	int rc, len;                                       /* [한국어] 임시. */

	if (bs_dev == NULL) {                              /* [한국어] BS device 가드. */
		SPDK_ERRLOG("Blobstore device does not exist\n");
		return -ENODEV;
	}

	if (o == NULL) {                                   /* [한국어] init 은 opts 필수 (이름 등). */
		SPDK_ERRLOG("spdk_lvs_opts not specified\n");
		return -EINVAL;
	}

	spdk_lvs_opts_init(&lvs_opts);                     /* [한국어] 기본값. */
	if (lvs_opts_copy(o, &lvs_opts) != 0) {            /* [한국어] ABI 안전 복사. */
		SPDK_ERRLOG("spdk_lvs_opts invalid\n");
		return -EINVAL;
	}

	if (lvs_opts.cluster_sz < bs_dev->blocklen || (lvs_opts.cluster_sz % bs_dev->blocklen) != 0) {
	                                                    /* [한국어] cluster 가 backend block
	                                                     * 보다 작거나 정수배가 아니면 BS 가
	                                                     * 정렬 문제로 깨진다. */
		SPDK_ERRLOG("Cluster size %" PRIu32 " is smaller than blocklen %" PRIu32
			    "Or not an integral multiple\n", lvs_opts.cluster_sz, bs_dev->blocklen);
		return -EINVAL;
	}
	total_clusters = bs_dev->blockcnt / (lvs_opts.cluster_sz / bs_dev->blocklen);
	                                                    /* [한국어] cluster 수 = (blockcnt) /
	                                                     * (cluster 당 block 수). 메타 페이지 수
	                                                     * 계산에 사용. */

	lvs = lvs_alloc();                                 /* [한국어] LVS 핸들 alloc + thread 기록. */
	if (!lvs) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store base pointer\n");
		return -ENOMEM;
	}

	setup_lvs_opts(&opts, o, total_clusters, lvs);     /* [한국어] BS opts 채움 (esnap_ctx=lvs). */

	len = strnlen(lvs_opts.name, SPDK_LVS_NAME_MAX);   /* [한국어] 이름 길이 측정 (NULL 안전). */
	if (len == 0 || len == SPDK_LVS_NAME_MAX) {        /* [한국어] 0 (빈 이름) 또는 MAX (null
	                                                    * terminator 누락) 거부. */
		SPDK_ERRLOG("Name must be between 1 and %d characters\n", SPDK_LVS_NAME_MAX - 1);
		lvs_free(lvs);
		return -EINVAL;
	}

	spdk_uuid_generate(&lvs->uuid);                    /* [한국어] LVS UUID 생성. super blob 의
	                                                    * "uuid" xattr 로 영속화 — load 시 복원. */
	snprintf(lvs->name, sizeof(lvs->name), "%s", lvs_opts.name);
	                                                    /* [한국어] 이름 복사 (안전). */

	rc = add_lvs_to_list(lvs);                         /* [한국어] 전역 등록 (이름 중복 검사). */
	if (rc) {
		SPDK_ERRLOG("lvolstore with name %s already exists\n", lvs->name);
		lvs_free(lvs);
		return -EEXIST;
	}

	lvs_req = calloc(1, sizeof(*lvs_req));             /* [한국어] req 할당. */
	if (!lvs_req) {
		lvs_free(lvs);
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	assert(cb_fn != NULL);                             /* [한국어] 콜백 필수. */
	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;
	lvs_req->lvol_store = lvs;
	lvs->bs_dev = bs_dev;                              /* [한국어] backend bs_dev 보관. */

	SPDK_INFOLOG(lvol, "Initializing lvol store\n");
	spdk_bs_init(bs_dev, &opts, lvs_init_cb, lvs_req); /* [한국어] BS 포맷 시작 — 비동기. */

	return 0;                                          /* [한국어] 비동기 시작 성공. 결과는 cb_fn. */
}

/*
 * [한국어]
 * lvs_rename_cb - LVS rename 의 최종 단계: super blob close 콜백 + 메모리 이름 갱신
 *
 * @cb_arg: spdk_lvs_req — 안에 lvserrno (지금까지의 에러 누적), lvol_store, cb_fn 들어 있음.
 * @lvolerrno: super blob close 결과.
 *
 * 동작:
 *   - close 자체 실패 시 lvserrno 갱신.
 *   - lvserrno != 0: rename 실패. lvs->new_name 을 다시 lvs->name 으로 되돌림 (다음 rename
 *     시도가 깨끗하게 시작되도록).
 *   - lvserrno == 0: lvs->name = lvs->new_name 으로 갱신 (디스크는 이미 sync 됨).
 *   - 사용자 cb_fn 호출 + req 해제.
 */
static void
lvs_rename_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_req *req = cb_arg;

	if (lvolerrno != 0) {                              /* [한국어] close 자체 실패. */
		req->lvserrno = lvolerrno;
	}
	if (req->lvserrno != 0) {                          /* [한국어] rename 단계 어디선가 실패. */
		SPDK_ERRLOG("Lvol store rename operation failed\n");
		/* Lvs renaming failed, so we should 'clear' new_name.
		 * Otherwise it could cause a failure on the next attempt to change the name to 'new_name'  */
		/* [한국어] 위 영문 주석대로: new_name 을 그대로 두면 다음 rename 시도에서 자기
		 *           자신과 충돌 처럼 보이게 된다. 그래서 현재 이름으로 rollback. */
		snprintf(req->lvol_store->new_name,
			 sizeof(req->lvol_store->new_name),
			 "%s", req->lvol_store->name);
	} else {
		/* Update lvs name with new_name */
		/* [한국어] 디스크 sync 까지 끝났으므로 in-memory 이름도 새 이름으로 확정. */
		snprintf(req->lvol_store->name,
			 sizeof(req->lvol_store->name),
			 "%s", req->lvol_store->new_name);
	}

	req->cb_fn(req->cb_arg, req->lvserrno);            /* [한국어] 사용자 콜백. */
	free(req);
}

/*
 * [한국어]
 * lvs_rename_sync_cb - super blob 의 xattr sync_md 완료 콜백 → super blob close 진행
 *
 * @cb_arg: spdk_lvs_req.
 * @lvolerrno: sync 결과.
 *
 * 동작: sync 가 실패해도 super blob 은 어차피 close 해야 하므로(파일 핸들 누수 방지)
 * lvserrno 만 누적해 두고 close 단계로 진행.
 */
static void
lvs_rename_sync_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_req *req = cb_arg;
	struct spdk_blob *blob = req->lvol_store->super_blob;

	if (lvolerrno < 0) {                               /* [한국어] sync 실패. */
		req->lvserrno = lvolerrno;
	}

	spdk_blob_close(blob, lvs_rename_cb, req);         /* [한국어] super blob close. */
}

/*
 * [한국어]
 * lvs_rename_open_cb - super blob open 콜백 → "name" xattr 새 값으로 갱신 + sync
 *
 * @cb_arg: spdk_lvs_req.
 * @blob: open 된 super blob (lvolerrno != 0 이면 NULL).
 * @lvolerrno: open 결과.
 */
static void
lvs_rename_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_req *req = cb_arg;
	int rc;

	if (lvolerrno < 0) {                               /* [한국어] open 실패 — close 단계 skip
	                                                    * 하고 cb 직접 호출 (blob 핸들 없음). */
		lvs_rename_cb(cb_arg, lvolerrno);
		return;
	}

	rc = spdk_blob_set_xattr(blob, "name", req->lvol_store->new_name,
				 strlen(req->lvol_store->new_name) + 1);
	                                                    /* [한국어] super blob 의 "name" xattr
	                                                     * 를 new_name 으로 교체. +1 = null
	                                                     * terminator. */
	if (rc < 0) {                                      /* [한국어] set_xattr 실패 (메모리/검증). */
		req->lvserrno = rc;
		lvs_rename_sync_cb(req, rc);               /* [한국어] sync 단계 skip 하고 close 로. */
		return;
	}

	req->lvol_store->super_blob = blob;                /* [한국어] sync_cb 가 close 할 때 사용. */

	spdk_blob_sync_md(blob, lvs_rename_sync_cb, req);  /* [한국어] xattr 변경 디스크 동기화. */
}

/*
 * [한국어]
 * spdk_lvs_rename - LVS 이름 변경 (공개 API)
 *
 * @lvs: 대상 LVS.
 * @new_name: 새 이름 (NULL-terminated, SPDK_LVS_NAME_MAX 미만).
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동기: super blob 의 "name" xattr 를 갱신하여 영속화. 메모리상의 lvs->name 은 sync 가
 * 끝난 뒤에 갱신해야 한다 (도중에 crash 시 이전 이름이 디스크와 메모리 모두 일관).
 *
 * 동작:
 *   1) 이름이 동일하면 즉시 성공.
 *   2) 전역 LVS 리스트에서 동일 이름 또는 동일 new_name 충돌 검사 — EEXIST.
 *   3) lvs->new_name 에 임시로 새 이름 저장.
 *   4) super blob open → set_xattr → sync_md → close → 메모리 이름 갱신 → 사용자 cb.
 *
 * 호출 체인:
 *   사용자/RPC → [spdk_lvs_rename] → spdk_bs_open_blob → lvs_rename_open_cb →
 *     spdk_blob_set_xattr → spdk_blob_sync_md → lvs_rename_sync_cb → spdk_blob_close →
 *     lvs_rename_cb → 사용자 cb_fn.
 */
void
spdk_lvs_rename(struct spdk_lvol_store *lvs, const char *new_name,
		spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_req *req;
	struct spdk_lvol_store *tmp;

	/* Check if new name is current lvs name.
	 * If so, return success immediately */
	/* [한국어] 위 영문 주석: idempotent 처리 — 같은 이름으로 rename 호출 시 즉시 성공. */
	if (strncmp(lvs->name, new_name, SPDK_LVS_NAME_MAX) == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	/* Check if new or new_name is already used in other lvs */
	/* [한국어] 다른 LVS 의 현재 이름 또는 진행 중인 새 이름과 충돌 검사 — race 방지를 위해
	 *           in-flight rename 의 new_name 까지 본다. */
	pthread_mutex_lock(&g_lvol_stores_mutex);
	TAILQ_FOREACH(tmp, &g_lvol_stores, link) {
		if (!strncmp(new_name, tmp->name, SPDK_LVS_NAME_MAX) ||
		    !strncmp(new_name, tmp->new_name, SPDK_LVS_NAME_MAX)) {
			pthread_mutex_unlock(&g_lvol_stores_mutex);
			cb_fn(cb_arg, -EEXIST);
			return;
		}
	}
	pthread_mutex_unlock(&g_lvol_stores_mutex);

	req = calloc(1, sizeof(*req));                     /* [한국어] req 할당. */
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	snprintf(lvs->new_name, sizeof(lvs->new_name), "%s", new_name);
	                                                    /* [한국어] 진행 중 임시 보관 — 충돌 검사에
	                                                     * 의해 다른 thread 도 이를 본다. */
	req->lvol_store = lvs;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_bs_open_blob(lvs->blobstore, lvs->super_blob_id, lvs_rename_open_cb, req);
	                                                    /* [한국어] super blob 열기 → xattr 변경
	                                                     * 단계로 진입. */
}

/*
 * [한국어]
 * _lvs_unload_cb - spdk_bs_unload 의 완료 콜백 → 사용자 cb 호출 + req 해제
 */
static void
_lvs_unload_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_req *lvs_req = cb_arg;

	SPDK_INFOLOG(lvol, "Lvol store unloaded\n");
	assert(lvs_req->cb_fn != NULL);                    /* [한국어] cb_fn 은 필수 — invariant. */
	lvs_req->cb_fn(lvs_req->cb_arg, lvserrno);         /* [한국어] 사용자 통지. */
	free(lvs_req);                                     /* [한국어] req 해제. */
}

/*
 * [한국어]
 * spdk_lvs_unload - LVS 메모리/메타 핸들 unload (디스크 데이터는 보존, 공개 API)
 *
 * @lvs: 대상 LVS.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 * @return: 0 = 비동기 시작, 음수 errno = 즉시 실패.
 *
 * 동기: vbdev_lvol shutdown / 사용자 RPC 시 메모리에서 LVS 를 제거. 디스크의 LVS 메타와
 * 모든 lvol 데이터는 그대로 유지되어 다음 load 시 그대로 복원된다 (destroy 와 다름).
 *
 * 사전 조건: 모든 lvol 이 close (ref_count==0) 이고 어떤 액션도 진행 중이지 않음.
 *
 * 동작:
 *   1) 모든 lvol 점검 (busy/open lvol 있으면 EBUSY 즉시 반환).
 *   2) 모든 lvol 핸들 정리 (esnap missing 등록도 제거).
 *   3) BS unload + lvs_free.
 */
int
spdk_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		void *cb_arg)
{
	struct spdk_lvs_req *lvs_req;
	struct spdk_lvol *lvol, *tmp;

	if (lvs == NULL) {                                 /* [한국어] NULL 가드. */
		SPDK_ERRLOG("Lvol store is NULL\n");
		return -ENODEV;
	}

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) { /* [한국어] 모든 lvol 의 상태 검증 단계 1. */
		if (lvol->action_in_progress == true) {    /* [한국어] destroy/close 진행 중이면 안전
		                                            * 하게 unload 못함. */
			SPDK_ERRLOG("Cannot unload lvol store - operations on lvols pending\n");
			cb_fn(cb_arg, -EBUSY);
			return -EBUSY;
		} else if (lvol->ref_count != 0) {         /* [한국어] 누가 open 중. */
			SPDK_ERRLOG("Lvols still open on lvol store\n");
			cb_fn(cb_arg, -EBUSY);
			return -EBUSY;
		}
	}

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) { /* [한국어] 검증 통과 — 모든 핸들 회수. */
		spdk_lvs_esnap_missing_remove(lvol);       /* [한국어] degraded set 에 등록되어
		                                            * 있다면 제거 — 메모리 누수 방지. */
		TAILQ_REMOVE(&lvs->lvols, lvol, link);
		lvol_free(lvol);                           /* [한국어] lvol 핸들 free. */
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;

	SPDK_INFOLOG(lvol, "Unloading lvol store\n");
	spdk_bs_unload(lvs->blobstore, _lvs_unload_cb, lvs_req);
	                                                    /* [한국어] BS unload 비동기 시작 —
	                                                     * 디스크 메타는 그대로 유지. */
	lvs_free(lvs);                                     /* [한국어] LVS 핸들 즉시 free. BS unload
	                                                    * 콜백 안에서는 lvs 를 참조하지 않으므로
	                                                    * 안전. */

	return 0;
}

/*
 * [한국어]
 * _lvs_destroy_cb - spdk_bs_destroy 의 최종 완료 콜백 — 사용자 cb 호출
 */
static void
_lvs_destroy_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_destroy_req *lvs_req = cb_arg;

	SPDK_INFOLOG(lvol, "Lvol store destroyed\n");
	assert(lvs_req->cb_fn != NULL);
	lvs_req->cb_fn(lvs_req->cb_arg, lvserrno);
	free(lvs_req);
}

/*
 * [한국어]
 * _lvs_destroy_super_cb - super blob 삭제 완료 콜백 → BS destroy 진행
 *
 * @cb_arg: spdk_lvs_destroy_req.
 * @bserrno: spdk_bs_delete_blob 결과 (현재 무시 — destroy 는 어쨌든 진행).
 *
 * 동기: super blob 을 먼저 삭제해 LVS 정체성을 무효화한 뒤 BS 자체를 destroy 하여
 * 디스크 메타까지 모두 지운다.
 */
static void
_lvs_destroy_super_cb(void *cb_arg, int bserrno)
{
	struct spdk_lvs_destroy_req *lvs_req = cb_arg;
	struct spdk_lvol_store *lvs = lvs_req->lvs;

	assert(lvs != NULL);                               /* [한국어] invariant. */

	SPDK_INFOLOG(lvol, "Destroying lvol store\n");
	spdk_bs_destroy(lvs->blobstore, _lvs_destroy_cb, lvs_req);
	                                                    /* [한국어] BS 메타까지 디스크에서 제거. */
	lvs_free(lvs);                                     /* [한국어] LVS 핸들 free. BS destroy
	                                                    * 콜백은 lvs 를 더 이상 참조하지 않음. */
}

/*
 * [한국어]
 * spdk_lvs_destroy - LVS 영구 삭제: 디스크 메타까지 모두 지운다 (공개 API)
 *
 * @lvs: 대상 LVS.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 * @return: 0 = 비동기 시작, 음수 errno = 즉시 실패.
 *
 * 사전 조건: 모든 lvol 이 close (ref_count==0) 이며 액션 없음.
 *
 * 동작:
 *   1) 모든 lvol busy/open 검증.
 *   2) 모든 lvol 핸들 free (단순 free — 어차피 디스크 데이터를 통째로 지우므로 esnap
 *      missing 등록 제거는 생략. lvs_free 의 invariant 를 만족하지 않을 수 있으나, 디스크
 *      자체가 사라지므로 의미 없음).
 *      ※ 주의: spdk_lvs_unload 와 달리 spdk_lvs_esnap_missing_remove 를 호출하지 않는다 —
 *        degraded_lvol_sets_tree 가 비어있지 않으면 lvs_free 의 assert 가 깨질 수 있다
 *        (해당 경로에서는 lvol 이 lvs->lvols 에 등록되어 있더라도 esnap missing 추적이
 *         보통 없는 상태에서만 destroy 가 안전하게 호출된다).
 *   3) super blob 삭제 → BS destroy → lvs_free → 사용자 cb.
 */
int
spdk_lvs_destroy(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_lvs_destroy_req *lvs_req;
	struct spdk_lvol *iter_lvol, *tmp;

	if (lvs == NULL) {
		SPDK_ERRLOG("Lvol store is NULL\n");
		return -ENODEV;
	}

	TAILQ_FOREACH_SAFE(iter_lvol, &lvs->lvols, link, tmp) {
		                                            /* [한국어] busy/open 검증. */
		if (iter_lvol->action_in_progress == true) {
			SPDK_ERRLOG("Cannot destroy lvol store - operations on lvols pending\n");
			cb_fn(cb_arg, -EBUSY);
			return -EBUSY;
		} else if (iter_lvol->ref_count != 0) {
			SPDK_ERRLOG("Lvols still open on lvol store\n");
			cb_fn(cb_arg, -EBUSY);
			return -EBUSY;
		}
	}

	TAILQ_FOREACH_SAFE(iter_lvol, &lvs->lvols, link, tmp) {
		free(iter_lvol);                           /* [한국어] 단순 free — 어차피 디스크 통째
		                                            * destroy. (TAILQ_REMOVE 도 생략 — 리스트 자체가
		                                            * 곧 free 될 lvs 에 종속.) */
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;
	lvs_req->lvs = lvs;

	SPDK_INFOLOG(lvol, "Deleting super blob\n");
	spdk_bs_delete_blob(lvs->blobstore, lvs->super_blob_id, _lvs_destroy_super_cb, lvs_req);
	                                                    /* [한국어] super blob 삭제 → 콜백 체인. */

	return 0;
}

/*
 * [한국어]
 * lvol_close_blob_cb - spdk_lvol_close 의 blob_close 완료 콜백
 *
 * @cb_arg: spdk_lvol_req.
 * @lvolerrno: blob_close 결과.
 *
 * 동작:
 *   - 성공: ref_count--, blob=NULL.
 *   - 실패: 로그만, 상태는 그대로.
 *   - 어떤 경우든 action_in_progress=false 해제, 사용자 cb 호출.
 */
static void
lvol_close_blob_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (lvolerrno < 0) {                               /* [한국어] close 실패. */
		SPDK_ERRLOG("Could not close blob on lvol\n");
		goto end;
	}

	lvol->ref_count--;                                 /* [한국어] 마지막 close → ref=0. */
	lvol->blob = NULL;                                 /* [한국어] blob 핸들 무효화 — 다음 open
	                                                    * 시 spdk_bs_open_blob_ext 다시 호출. */
	SPDK_INFOLOG(lvol, "Lvol %s closed\n", lvol->unique_id);

end:
	lvol->action_in_progress = false;                  /* [한국어] 다음 액션 허용. */
	req->cb_fn(req->cb_arg, lvolerrno);                /* [한국어] 사용자 cb. */
	free(req);
}

/*
 * [한국어]
 * spdk_lvol_deletable - 이 lvol 이 destroy 가능한지 확인 (clone 자식이 없는가)
 *
 * @lvol: 대상.
 * @return: true = clone 자식 없음 (즉시 destroy 가능), false = clone 이 있어 destroy 시 BUSY.
 *
 * 동기: snapshot 을 destroy 하려면 자식 clone 이 없어야 한다 (CoW 의존성). 사용자 RPC
 * (bdev_lvol_delete) 에서 친절한 에러를 위해 사전 조회.
 */
bool
spdk_lvol_deletable(struct spdk_lvol *lvol)
{
	size_t count = 0;                                  /* [한국어] count 만 받기 — id 배열은 NULL. */

	spdk_blob_get_clones(lvol->lvol_store->blobstore, lvol->blob_id, NULL, &count);
	                                                    /* [한국어] BS 에 immediate clone 수만
	                                                     * 질의 (-ENOMEM 으로 count 만 채워짐). */
	return (count == 0);
}

/*
 * [한국어]
 * lvol_delete_blob_cb - spdk_lvol_destroy 의 blob_delete 완료 콜백
 *
 * @cb_arg: spdk_lvol_req — req->clone_lvol 에 destroy 직전의 단일 자식이 있을 수 있음.
 * @lvolerrno: blob_delete 결과 (실패해도 forced removal — 메모리는 회수).
 *
 * 동기: blob 이 디스크에서 사라졌으므로 (또는 사라뜨릴 수 없어 강제 진행) lvol 핸들도
 * 메모리에서 제거. 추가로 esnap "missing" 추적의 인계 처리:
 *   - 이 lvol 이 degraded esnap clone 이고 자식 clone 이 1 개 있다면, 자식이 esnap clone
 *     자리를 물려받는다 (BS 가 부모-자식 chain 을 자동 재구성). 그러면 lvs_degraded_set 의
 *     해당 노드 안에서 lvol 을 빼고 자식을 넣어 추적을 유지한다.
 *   - 자식이 없다면 단순히 missing 추적 제거.
 */
static void
lvol_delete_blob_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;
	struct spdk_lvol *clone_lvol = req->clone_lvol;    /* [한국어] 단일 자식 clone (없으면 NULL). */

	if (lvolerrno < 0) {                               /* [한국어] 디스크 삭제 실패 — 메모리는
	                                                    * 어쨌든 회수. */
		SPDK_ERRLOG("Could not remove blob on lvol gracefully - forced removal\n");
	} else {
		SPDK_INFOLOG(lvol, "Lvol %s deleted\n", lvol->unique_id);
	}

	if (lvol->degraded_set != NULL) {                  /* [한국어] 이 lvol 이 degraded esnap
	                                                    * clone 으로 추적되고 있었음. */
		if (clone_lvol != NULL) {                  /* [한국어] 자식이 있어 자리 인계. */
			/*
			 * A degraded esnap clone that has a blob clone has been deleted. clone_lvol
			 * becomes an esnap clone and needs to be associated with the
			 * spdk_lvs_degraded_lvol_set.
			 */
			/* [한국어] 위 영문 주석: BS 가 자식의 부모를 esnap 으로 끌어올리므로,
			 *           자식이 새 esnap clone 이 된다. 따라서 degraded set 도 이전.
			 *           (BS 의 spdk_bs_delete_blob 의미: 자식이 있는 blob 의 cluster
			 *            소유권은 자식에게 이전되고 자식이 esnap clone 이 됨.) */
			struct spdk_lvs_degraded_lvol_set *degraded_set = lvol->degraded_set;

			lvs_degraded_lvol_set_remove(degraded_set, lvol);
			                                    /* [한국어] 이전 lvol 제거. */
			lvs_degraded_lvol_set_add(degraded_set, clone_lvol);
			                                    /* [한국어] 자식 등록. */
		} else {                                   /* [한국어] 자식 없음 → 단순 추적 해제. */
			spdk_lvs_esnap_missing_remove(lvol);
		}
	}

	TAILQ_REMOVE(&lvol->lvol_store->lvols, lvol, link);
	                                                    /* [한국어] 활성 리스트에서 제거. */
	lvol_free(lvol);                                   /* [한국어] 메모리 회수. */
	req->cb_fn(req->cb_arg, lvolerrno);                /* [한국어] 사용자 cb (디스크 삭제 결과). */
	free(req);
}

/*
 * [한국어]
 * lvol_create_open_cb - lvol 생성 후 첫 open 의 콜백 — 활성 리스트로 이동 + ref_count 시작
 *
 * @cb_arg: spdk_lvol_with_handle_req.
 * @blob: open 된 blob (lvolerrno != 0 이면 NULL).
 * @lvolerrno: open 결과.
 *
 * 동작:
 *   - pending_lvols 에서 제거 (생성 중 → 생성 완료).
 *   - 실패: lvol 핸들 free + 사용자에게 NULL 통지.
 *   - 성공: lvol->blob/blob_id 저장, lvols 에 등록, ref_count=1 (생성 즉시 open 상태 시작).
 */
static void
lvol_create_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	TAILQ_REMOVE(&req->lvol->lvol_store->pending_lvols, req->lvol, link);
	                                                    /* [한국어] pending → 처리 완료. */

	if (lvolerrno < 0) {                               /* [한국어] open 실패 — blob 도 정리 안된
	                                                    * 채일 수 있으나, lvol_free 만 — blob 은
	                                                    * 추후 BS 가 정리. */
		lvol_free(lvol);
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		free(req);
		return;
	}

	lvol->blob = blob;                                 /* [한국어] blob 핸들 보관. */
	lvol->blob_id = spdk_blob_get_id(blob);            /* [한국어] 64-bit ID 저장. */

	TAILQ_INSERT_TAIL(&lvol->lvol_store->lvols, lvol, link);
	                                                    /* [한국어] 활성 lvol 컬렉션에 등록. */

	lvol->ref_count++;                                 /* [한국어] 생성 = 첫 open. 사용자가 따로
	                                                    * spdk_lvol_open 을 호출하지 않아도
	                                                    * 즉시 IO 가능. */

	assert(req->cb_fn != NULL);
	req->cb_fn(req->cb_arg, req->lvol, lvolerrno);     /* [한국어] 사용자 통지. */
	free(req);
}

/*
 * [한국어]
 * lvol_create_cb - blob 생성 (또는 snapshot/clone 생성) 의 콜백 → 새 blob 을 open
 *
 * @cb_arg: spdk_lvol_with_handle_req.
 * @blobid: 갓 만들어진 blob 의 64-bit ID.
 * @lvolerrno: 생성 결과.
 *
 * 동기: 모든 lvol 생성 패밀리 (create / snapshot / clone / esnap_clone) 가 공통으로 사용.
 * 새 blob 을 즉시 open 하여 lvol->blob 핸들을 만들고 사용자에게 반환한다.
 *
 * 추가 처리: snapshot 생성 시 origlvol 이 degraded esnap clone 이었다면, 이제 그 origlvol
 * 은 일반 clone 이 되고 새 snapshot 이 새로운 esnap clone 이 된다 — degraded set 에서
 * 자리 교체.
 */
static void
lvol_create_cb(void *cb_arg, spdk_blob_id blobid, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;
	struct spdk_blob_store *bs;
	struct spdk_blob_open_opts opts;

	if (lvolerrno < 0) {                               /* [한국어] 생성 실패. */
		TAILQ_REMOVE(&req->lvol->lvol_store->pending_lvols, req->lvol, link);
		                                            /* [한국어] pending 제거. */
		lvol_free(req->lvol);                      /* [한국어] 핸들 회수. */
		assert(req->cb_fn != NULL);
		req->cb_fn(req->cb_arg, NULL, lvolerrno);  /* [한국어] 사용자 NULL 통지. */
		free(req);
		return;
	}

	spdk_blob_open_opts_init(&opts, sizeof(opts));     /* [한국어] open opts 기본값. */
	opts.clear_method = req->lvol->clear_method;       /* [한국어] cluster 초기화 정책. */
	/*
	 * If the lvol that is being created is an esnap clone, the blobstore needs to be able to
	 * pass the lvol to the esnap_bs_dev_create callback. In order for that to happen, we need
	 * to pass it here.
	 *
	 * This does set ensap_ctx in cases where it's not needed, but we don't know that it's not
	 * needed until after the blob is open. When the blob is not an esnap clone, a reference to
	 * the value stored in opts.esnap_ctx is not retained by the blobstore.
	 */
	/* [한국어] 위 영문 주석: BS 의 esnap_bs_dev_create 콜백이 호출될 때 두 번째 인자로 lvol 이
	 *           필요하다 (lvs_esnap_bs_dev_create 라우터가 사용자 콜백에 lvol 을 전달). 이를 위해
	 *           opts.esnap_ctx 에 lvol 을 넣는다. esnap clone 이 아닐 때도 unconditionally
	 *           세팅하지만, 그런 경우 BS 가 이 값을 보존하지 않으므로 메모리 누수/dangling 위험
	 *           없음. */
	opts.esnap_ctx = req->lvol;                        /* [한국어] 콜백 컨텍스트 = 새 lvol. */
	bs = req->lvol->lvol_store->blobstore;

	if (req->origlvol != NULL && req->origlvol->degraded_set != NULL) {
	                                                    /* [한국어] snapshot 생성 시 origlvol 이
	                                                     * degraded esnap clone 이었음. */
		/*
		 * A snapshot was created from a degraded esnap clone. The new snapshot is now a
		 * degraded esnap clone. The previous clone is now a regular clone of a blob. Update
		 * the set of directly-related clones to the missing external snapshot.
		 */
		/* [한국어] 위 영문 주석: BS 의 snapshot 의미상 origlvol → snapshot → esnap 의
		 *           체인이 만들어졌다. 따라서 esnap 을 직접 의존하는 lvol 은 이제 origlvol 이
		 *           아니라 새 snapshot 이다. 추적 set 도 자리를 옮긴다. */
		struct spdk_lvs_degraded_lvol_set *degraded_set = req->origlvol->degraded_set;

		lvs_degraded_lvol_set_remove(degraded_set, req->origlvol);
		lvs_degraded_lvol_set_add(degraded_set, req->lvol);
	}

	spdk_bs_open_blob_ext(bs, blobid, &opts, lvol_create_open_cb, req);
	                                                    /* [한국어] 새 blob 을 즉시 open 하여
	                                                     * 사용자가 IO 가능 상태로 만든다. */
}

/*
 * [한국어]
 * lvol_get_xattr_value - blob xattr 의 lazy value getter (BS 콜백)
 *
 * @xattr_ctx: spdk_lvol * (lvol_create 시 xattrs.ctx 로 전달).
 * @name: xattr 이름.
 * @value: out — 값 포인터.
 * @value_len: out — 값 길이.
 *
 * 동기: BS 가 새 blob 의 xattr 를 쓸 때 값 자체를 외부에서 가져오는 게터 콜백. lvol 핸들
 * 의 in-memory 필드(name, uuid_str)를 그대로 노출한다.
 */
static void
lvol_get_xattr_value(void *xattr_ctx, const char *name,
		     const void **value, size_t *value_len)
{
	struct spdk_lvol *lvol = xattr_ctx;

	if (!strcmp(LVOL_NAME, name)) {                    /* [한국어] "name" 요청. */
		*value = lvol->name;
		*value_len = SPDK_LVOL_NAME_MAX;
		return;
	}
	if (!strcmp("uuid", name)) {                       /* [한국어] "uuid" 요청. */
		*value = lvol->uuid_str;
		*value_len = sizeof(lvol->uuid_str);
		return;
	}
	*value = NULL;                                     /* [한국어] 알 수 없는 키 — empty. */
	*value_len = 0;
}

/*
 * [한국어]
 * lvs_verify_lvol_name - 신규 lvol 이름의 유효성 + 중복 검사
 *
 * @lvs: 대상 LVS.
 * @name: 사용자 지정 이름.
 * @return: 0 = OK, -EINVAL = 형식 오류, -EEXIST = 중복.
 *
 * 동기: lvol 생성 패밀리 함수 모두가 동일한 검사를 해야 하므로 공통화.
 *
 * 검사 항목:
 *   1) NULL/빈 문자열 거부.
 *   2) MAX 길이 = null terminator 누락 → 거부.
 *   3) 활성 lvols TAILQ + pending_lvols TAILQ 에서 동일 이름 검색.
 */
static int
lvs_verify_lvol_name(struct spdk_lvol_store *lvs, const char *name)
{
	struct spdk_lvol *tmp;

	if (name == NULL || strnlen(name, SPDK_LVOL_NAME_MAX) == 0) {
		SPDK_INFOLOG(lvol, "lvol name not provided.\n");
		return -EINVAL;
	}

	if (strnlen(name, SPDK_LVOL_NAME_MAX) == SPDK_LVOL_NAME_MAX) {
	                                                    /* [한국어] MAX 까지 \0 없음 → 잘림 의심. */
		SPDK_ERRLOG("Name has no null terminator.\n");
		return -EINVAL;
	}

	TAILQ_FOREACH(tmp, &lvs->lvols, link) {            /* [한국어] 활성 lvol 중 같은 이름 있나. */
		if (!strncmp(name, tmp->name, SPDK_LVOL_NAME_MAX)) {
			SPDK_ERRLOG("lvol with name %s already exists\n", name);
			return -EEXIST;
		}
	}

	TAILQ_FOREACH(tmp, &lvs->pending_lvols, link) {    /* [한국어] 동시에 생성 중인 lvol 도 검사
	                                                    * — race 방지. */
		if (!strncmp(name, tmp->name, SPDK_LVOL_NAME_MAX)) {
			SPDK_ERRLOG("lvol with name %s is being already created\n", name);
			return -EEXIST;
		}
	}

	return 0;
}

/*
 * [한국어]
 * spdk_lvol_create - 새 LVol 생성 (공개 API)
 *
 * @lvs: 소속 LVS.
 * @name: 새 lvol 이름.
 * @sz: 바이트 단위 크기. cluster 단위로 round-up.
 * @thin_provision: true = thin (lazy 할당), false = thick (전체 cluster 즉시 할당).
 * @clear_method: cluster 회수 시 데이터 영역 처리 정책.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 * @return: 0 = 비동기 시작, 음수 errno = 즉시 실패.
 *
 * 동작:
 *   1) lvs/이름 검증.
 *   2) req + lvol 핸들 alloc (lvol_alloc 이 pending_lvols 에 등록).
 *   3) blob_opts 채움: thin/thick, num_clusters, xattrs(name+uuid).
 *   4) spdk_bs_create_blob_ext 시작 → lvol_create_cb → spdk_bs_open_blob_ext →
 *      lvol_create_open_cb → 사용자 cb_fn.
 */
int
spdk_lvol_create(struct spdk_lvol_store *lvs, const char *name, uint64_t sz,
		 bool thin_provision, enum lvol_clear_method clear_method, spdk_lvol_op_with_handle_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_store *bs;
	struct spdk_lvol *lvol;
	struct spdk_blob_opts opts;
	char *xattr_names[] = {LVOL_NAME, "uuid"};         /* [한국어] blob 에 저장할 xattr 키 목록. */
	int rc;

	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		return -EINVAL;
	}

	rc = lvs_verify_lvol_name(lvs, name);              /* [한국어] 이름 형식 + 중복 검사. */
	if (rc < 0) {
		return rc;
	}

	bs = lvs->blobstore;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		return -ENOMEM;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	lvol = lvol_alloc(lvs, name, thin_provision, clear_method);
	                                                    /* [한국어] lvol 핸들 + UUID 생성 +
	                                                     * pending_lvols 등록. */
	if (!lvol) {
		free(req);
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		return -ENOMEM;
	}

	req->lvol = lvol;
	spdk_blob_opts_init(&opts, sizeof(opts));          /* [한국어] blob_opts 기본값. */
	opts.thin_provision = thin_provision;              /* [한국어] thin/thick 결정. */
	opts.num_clusters = spdk_divide_round_up(sz, spdk_bs_get_cluster_size(bs));
	                                                    /* [한국어] cluster 단위로 ceil — 사용자
	                                                     * 가 요청한 sz 이상이 되도록. */
	opts.clear_method = lvol->clear_method;            /* [한국어] cluster 클리어 정책. */
	opts.xattrs.count = SPDK_COUNTOF(xattr_names);     /* [한국어] xattr 2개 (name, uuid). */
	opts.xattrs.names = xattr_names;
	opts.xattrs.ctx = lvol;                            /* [한국어] getter 의 첫 인자. */
	opts.xattrs.get_value = lvol_get_xattr_value;      /* [한국어] lazy getter — 위에 정의. */

	spdk_bs_create_blob_ext(lvs->blobstore, &opts, lvol_create_cb, req);
	                                                    /* [한국어] BS 에 blob 생성 시작 — 비동기. */

	return 0;
}

/*
 * [한국어]
 * spdk_lvol_create_esnap_clone - 외부 read-only 데이터 소스를 부모로 하는 esnap clone 생성
 *
 * @esnap_id: 외부 스냅샷 식별자 (보통 backing bdev UUID 문자열).
 * @id_len: esnap_id 바이트 길이.
 * @size_bytes: 새 lvol 크기 (cluster 정수배여야 함).
 * @lvs: 소속 LVS.
 * @clone_name: 새 lvol 이름.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동기: external snapshot — LVS 외부의 read-only bdev 를 backing 으로 하는 thin lvol.
 * 사용자가 미리 만들어 둔 read-only 이미지(예: golden VM image)를 가리키면, 새 lvol 은
 * 그 위에 CoW write 만 누적한다. 부팅 디스크 등에 유용.
 *
 * 동작:
 *   1) 인자 검증, cluster 정수배 검사.
 *   2) lvol 핸들 alloc (thin=true 강제).
 *   3) blob_opts 에 esnap_id/_len 설정 → BS 가 이 blob 을 esnap clone 으로 인식.
 *   4) BS create_blob_ext → 콜백 체인 (일반 create 과 동일).
 */
int
spdk_lvol_create_esnap_clone(const void *esnap_id, uint32_t id_len, uint64_t size_bytes,
			     struct spdk_lvol_store *lvs, const char *clone_name,
			     spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_store *bs;
	struct spdk_lvol *lvol;
	struct spdk_blob_opts opts;
	uint64_t cluster_sz;
	char *xattr_names[] = {LVOL_NAME, "uuid"};
	int rc;

	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		return -EINVAL;
	}

	rc = lvs_verify_lvol_name(lvs, clone_name);        /* [한국어] 이름 검증. */
	if (rc < 0) {
		return rc;
	}

	bs = lvs->blobstore;

	cluster_sz = spdk_bs_get_cluster_size(bs);         /* [한국어] BS 의 cluster 크기. */
	if ((size_bytes % cluster_sz) != 0) {              /* [한국어] esnap clone 은 정확히 cluster
	                                                    * 정수배여야 한다 — backing snapshot 의
	                                                    * cluster 경계와 맞춰져야 BS 의 cluster
	                                                    * dispatch 가 정상 동작. */
		SPDK_ERRLOG("Cannot create '%s/%s': size %" PRIu64 " is not an integer multiple of "
			    "cluster size %" PRIu64 "\n", lvs->name, clone_name, size_bytes,
			    cluster_sz);
		return -EINVAL;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		return -ENOMEM;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	lvol = lvol_alloc(lvs, clone_name, true, LVOL_CLEAR_WITH_DEFAULT);
	                                                    /* [한국어] thin=true 고정 (esnap clone
	                                                     * 의미상 thin 만 가능). DEFAULT clear. */
	if (!lvol) {
		free(req);
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		return -ENOMEM;
	}
	req->lvol = lvol;

	spdk_blob_opts_init(&opts, sizeof(opts));
	opts.esnap_id = esnap_id;                          /* [한국어] BS 가 이 blob 을 esnap clone
	                                                    * 으로 식별하는 키. blob xattr 에 저장. */
	opts.esnap_id_len = id_len;
	opts.thin_provision = true;                        /* [한국어] esnap clone 은 정의상 thin. */
	opts.num_clusters = spdk_divide_round_up(size_bytes, cluster_sz);
	                                                    /* [한국어] cluster 수 (이미 정수배). */
	opts.clear_method = lvol->clear_method;
	opts.xattrs.count = SPDK_COUNTOF(xattr_names);
	opts.xattrs.names = xattr_names;
	opts.xattrs.ctx = lvol;
	opts.xattrs.get_value = lvol_get_xattr_value;

	spdk_bs_create_blob_ext(lvs->blobstore, &opts, lvol_create_cb, req);
	                                                    /* [한국어] BS 에 esnap clone blob 생성 요청.
	                                                     * BS 는 esnap_bs_dev_create 콜백을 통해
	                                                     * 외부 backing 을 알아보고 cluster
	                                                     * dispatch 를 backing 으로 라우팅. */

	return 0;
}

/*
 * [한국어]
 * spdk_lvol_create_snapshot - 기존 lvol 의 read-only snapshot 생성 (공개 API)
 *
 * @origlvol: 원본 lvol — 이후 read-write 가능 상태로 유지되며, 새 snapshot 이 그것의
 *           "부모" 가 된다 (BS 의 CoW snapshot 의미: snapshot 은 read-only, original 은
 *           snapshot 을 부모로 갖는 thin clone 으로 변환).
 * @snapshot_name: 새 snapshot 의 이름.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동기: 시점 (point-in-time) 백업 / 복제 / 빠른 데이터 분기. snapshot 자체는 읽기 전용
 * 이며, original 이 이후 변경하는 cluster 만 새로 할당된다 (CoW).
 *
 * 호출 체인:
 *   사용자 → [spdk_lvol_create_snapshot] → spdk_bs_create_snapshot → lvol_create_cb →
 *     spdk_bs_open_blob_ext → lvol_create_open_cb → 사용자 cb (with snapshot lvol).
 */
void
spdk_lvol_create_snapshot(struct spdk_lvol *origlvol, const char *snapshot_name,
			  spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *newlvol;
	struct spdk_blob *origblob;
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_xattr_opts snapshot_xattrs;
	char *xattr_names[] = {LVOL_NAME, "uuid"};
	int rc;

	if (origlvol == NULL) {
		SPDK_INFOLOG(lvol, "Lvol not provided.\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	origblob = origlvol->blob;                         /* [한국어] 원본 blob 핸들. */
	lvs = origlvol->lvol_store;
	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	rc = lvs_verify_lvol_name(lvs, snapshot_name);
	if (rc < 0) {
		cb_fn(cb_arg, NULL, rc);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	newlvol = lvol_alloc(origlvol->lvol_store, snapshot_name, true,
			     (enum lvol_clear_method)origlvol->clear_method);
	                                                    /* [한국어] snapshot 도 lvol 핸들 하나 —
	                                                     * thin=true 의미상 새 cluster 가 따로
	                                                     * 잡히지 않음 (read-only). */
	if (!newlvol) {
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		free(req);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	snapshot_xattrs.count = SPDK_COUNTOF(xattr_names);
	snapshot_xattrs.ctx = newlvol;
	snapshot_xattrs.names = xattr_names;
	snapshot_xattrs.get_value = lvol_get_xattr_value;
	req->lvol = newlvol;
	req->origlvol = origlvol;                          /* [한국어] origlvol 도 보존 — colvol_create_cb
	                                                    * 가 degraded set 자리 교체에 사용. */
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_bs_create_snapshot(lvs->blobstore, spdk_blob_get_id(origblob), &snapshot_xattrs,
				lvol_create_cb, req);
	                                                    /* [한국어] BS 의 snapshot API — 원본의
	                                                     * cluster 소유권을 새 snapshot 으로
	                                                     * 옮기고, 원본은 snapshot 을 부모로 갖는
	                                                     * thin clone 이 된다 (BS 가 자동). */
}

/*
 * [한국어]
 * spdk_lvol_create_clone - 기존 (snapshot) lvol 을 부모로 하는 read-write thin clone 생성 (공개 API)
 *
 * @origlvol: 부모 (보통 read-only snapshot).
 * @clone_name: 새 clone 의 이름.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동기: snapshot 으로부터 분기된 새 RW 볼륨. 부모와 cluster 를 공유하다가 write 시 CoW.
 *
 * 호출 체인:
 *   사용자 → [spdk_lvol_create_clone] → spdk_bs_create_clone → lvol_create_cb → ... → 사용자 cb.
 */
void
spdk_lvol_create_clone(struct spdk_lvol *origlvol, const char *clone_name,
		       spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *newlvol;
	struct spdk_lvol_with_handle_req *req;
	struct spdk_lvol_store *lvs;
	struct spdk_blob *origblob;
	struct spdk_blob_xattr_opts clone_xattrs;
	char *xattr_names[] = {LVOL_NAME, "uuid"};
	int rc;

	if (origlvol == NULL) {
		SPDK_INFOLOG(lvol, "Lvol not provided.\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	origblob = origlvol->blob;                         /* [한국어] 부모 blob 핸들. */
	lvs = origlvol->lvol_store;
	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	rc = lvs_verify_lvol_name(lvs, clone_name);
	if (rc < 0) {
		cb_fn(cb_arg, NULL, rc);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	newlvol = lvol_alloc(lvs, clone_name, true, (enum lvol_clear_method)origlvol->clear_method);
	                                                    /* [한국어] 새 clone (thin). */
	if (!newlvol) {
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		free(req);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	clone_xattrs.count = SPDK_COUNTOF(xattr_names);
	clone_xattrs.ctx = newlvol;                        /* [한국어] xattr getter 컨텍스트. */
	clone_xattrs.names = xattr_names;
	clone_xattrs.get_value = lvol_get_xattr_value;
	req->lvol = newlvol;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_bs_create_clone(lvs->blobstore, spdk_blob_get_id(origblob), &clone_xattrs,
			     lvol_create_cb,
			     req);
	                                                    /* [한국어] BS 의 clone API — 새 thin
	                                                     * blob 을 부모(origblob)로 삼아 생성. */
}

/*
 * [한국어]
 * lvol_resize_done - resize 의 sync_md 완료 콜백 → 사용자 cb 호출
 */
static void
lvol_resize_done(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	req->cb_fn(req->cb_arg,  lvolerrno);
	free(req);
}

/*
 * [한국어]
 * lvol_blob_resize_cb - blob_resize 완료 → 메타 sync 진행
 */
static void
lvol_blob_resize_cb(void *cb_arg, int bserrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (bserrno != 0) {                                /* [한국어] resize 실패. */
		req->cb_fn(req->cb_arg, bserrno);
		free(req);
		return;
	}

	spdk_blob_sync_md(lvol->blob, lvol_resize_done, req);
	                                                    /* [한국어] 메타 디스크 동기화. */
}

/*
 * [한국어]
 * spdk_lvol_resize - lvol 크기 변경 (확장/축소, 공개 API — internal lvolstore.h 선언)
 *
 * @lvol: 대상.
 * @sz: 새 크기 (바이트). cluster 단위로 round-up.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 */
void
spdk_lvol_resize(struct spdk_lvol *lvol, uint64_t sz,
		 spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_blob *blob = lvol->blob;
	struct spdk_lvol_store *lvs = lvol->lvol_store;
	struct spdk_lvol_req *req;
	uint64_t new_clusters = spdk_divide_round_up(sz, spdk_bs_get_cluster_size(lvs->blobstore));
	                                                    /* [한국어] 새 cluster 수. */

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;

	spdk_blob_resize(blob, new_clusters, lvol_blob_resize_cb, req);
	                                                    /* [한국어] BS 에 cluster 수 변경 요청. */
}

/*
 * [한국어]
 * lvol_set_read_only_cb - sync_md 완료 콜백
 */
static void
lvol_set_read_only_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

/*
 * [한국어]
 * spdk_lvol_set_read_only - lvol 을 read-only 로 영구 전환 (internal lvolstore.h 선언)
 *
 * @lvol: 대상.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동작: blob 의 read_only 플래그 set + sync_md 로 디스크 영속화. 한 번 설정하면 되돌릴 수 없다.
 */
void
spdk_lvol_set_read_only(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_blob_set_read_only(lvol->blob);               /* [한국어] in-memory 플래그 (동기). */
	spdk_blob_sync_md(lvol->blob, lvol_set_read_only_cb, req);
	                                                    /* [한국어] 디스크에 영속화. */
}

/*
 * [한국어]
 * lvol_rename_cb - lvol rename 의 sync_md 완료 콜백 → 메모리 이름 갱신
 *
 * 디스크 sync 가 끝났으므로 in-memory lvol->name 도 새 이름으로 확정. 실패 시 메모리는
 * 그대로 — 디스크의 xattr 도 기존 값일 가능성이 높음 (sync 가 실패한 경우).
 */
static void
lvol_rename_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	if (lvolerrno != 0) {                              /* [한국어] sync 실패. */
		SPDK_ERRLOG("Lvol rename operation failed\n");
	} else {
		snprintf(req->lvol->name, sizeof(req->lvol->name), "%s", req->name);
		                                            /* [한국어] 메모리 이름 갱신. */
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

/*
 * [한국어]
 * spdk_lvol_rename - lvol 이름 변경 (공개 API)
 *
 * @lvol: 대상.
 * @new_name: 새 이름.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동작:
 *   1) 이름 동일하면 즉시 성공 (idempotent).
 *   2) 같은 LVS 안에서 동일 이름 충돌 검사.
 *   3) blob 의 "name" xattr 설정 (in-memory) → sync_md → 메모리 이름 갱신.
 */
void
spdk_lvol_rename(struct spdk_lvol *lvol, const char *new_name,
		 spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *tmp;
	struct spdk_blob *blob = lvol->blob;
	struct spdk_lvol_req *req;
	int rc;

	/* Check if new name is current lvol name.
	 * If so, return success immediately */
	/* [한국어] idempotent: 같은 이름이면 즉시 성공. */
	if (strncmp(lvol->name, new_name, SPDK_LVOL_NAME_MAX) == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	/* Check if lvol with 'new_name' already exists in lvolstore */
	/* [한국어] 같은 LVS 안의 다른 lvol 이름과 충돌 검사. (lvs 간 중복은 허용.) */
	TAILQ_FOREACH(tmp, &lvol->lvol_store->lvols, link) {
		if (strncmp(tmp->name, new_name, SPDK_LVOL_NAME_MAX) == 0) {
			SPDK_ERRLOG("Lvol %s already exists in lvol store %s\n", new_name, lvol->lvol_store->name);
			cb_fn(cb_arg, -EEXIST);
			return;
		}
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;
	snprintf(req->name, sizeof(req->name), "%s", new_name);
	                                                    /* [한국어] sync 후 메모리 이름 갱신용. */

	rc = spdk_blob_set_xattr(blob, "name", new_name, strlen(new_name) + 1);
	                                                    /* [한국어] xattr in-memory 변경. +1 = \0. */
	if (rc < 0) {
		free(req);
		cb_fn(cb_arg, rc);
		return;
	}

	spdk_blob_sync_md(blob, lvol_rename_cb, req);      /* [한국어] 디스크 영속화. */
}

/*
 * [한국어]
 * spdk_lvol_destroy - lvol 영구 삭제 (공개 API). blob 까지 디스크에서 제거.
 *
 * @lvol: 대상. ref_count == 0 이어야 함 (open 상태에서 destroy 금지).
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동작:
 *   1) NULL/closed 상태/ref count 검증.
 *   2) blob_get_clones 로 자식 수 확인:
 *      - 0개: 단순 삭제.
 *      - 1개: req->clone_lvol 에 자식 lvol 핸들 보관 (delete 콜백에서 esnap 자리 인계용).
 *      - 2개 이상 (-ENOMEM): EBUSY 반환 (snapshot 자식이 여러개면 destroy 불가).
 *   3) action_in_progress=true, spdk_bs_delete_blob → lvol_delete_blob_cb.
 */
void
spdk_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	struct spdk_blob_store *bs;
	struct spdk_lvol_store	*lvs;
	spdk_blob_id	clone_id;                          /* [한국어] 단일 자식 clone 의 blob_id. */
	size_t		count = 1;                         /* [한국어] get_clones in/out — 1로 초기화하여
	                                                    * 0 (clone 없음) 도 받을 수 있도록. */
	int		rc;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	lvs = lvol->lvol_store;

	if (lvol->ref_count != 0) {                        /* [한국어] 누가 open 중이면 destroy 금지. */
		SPDK_ERRLOG("Cannot destroy lvol %s because it is still open\n", lvol->unique_id);
		cb_fn(cb_arg, -EBUSY);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;
	bs = lvol->lvol_store->blobstore;

	rc = spdk_blob_get_clones(lvs->blobstore, lvol->blob_id, &clone_id, &count);
	                                                    /* [한국어] 자식 수 + (가능하다면) blob_id
	                                                     * 1개까지 받기. count 입력은 buffer 크기,
	                                                     * 출력은 실제 자식 수. */
	if (rc == 0 && count == 1) {                       /* [한국어] 자식 1개 — 자리 인계 필요. */
		req->clone_lvol = lvs_get_lvol_by_blob_id(lvs, clone_id);
		                                            /* [한국어] 자식 lvol 핸들 lookup. delete 콜백
		                                             * 에서 degraded set 자리 교체에 사용. */
	} else if (rc == -ENOMEM) {                        /* [한국어] buffer 부족 = 자식 ≥ 2 — destroy
	                                                    * 불가 (snapshot 자식 여러 개의 부모는 못 지움). */
		SPDK_INFOLOG(lvol, "lvol %s: cannot destroy: has %" PRIu64 " clones\n",
			     lvol->unique_id, count);
		free(req);
		assert(count > 1);
		cb_fn(cb_arg, -EBUSY);
		return;
	}

	lvol->action_in_progress = true;                   /* [한국어] open/close 등 다른 액션 차단. */

	spdk_bs_delete_blob(bs, lvol->blob_id, lvol_delete_blob_cb, req);
	                                                    /* [한국어] BS 에서 blob 영구 삭제 — 비동기. */
}

/*
 * [한국어]
 * spdk_lvol_close - lvol 의 blob 핸들 close (ref_count 감소, 0이 되면 실제 close, 공개 API)
 *
 * @lvol: 대상.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동작:
 *   - lvol == NULL: ENODEV.
 *   - ref_count > 1: ref_count-- 만 하고 즉시 콜백 (다른 사용자가 아직 open 중).
 *   - ref_count == 0: EINVAL (이미 close 상태인데 또 close).
 *   - ref_count == 1 (마지막 close): action_in_progress=true 후 spdk_blob_close → 콜백에서
 *     blob=NULL, ref_count=0.
 */
void
spdk_lvol_close(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	if (lvol->ref_count > 1) {                         /* [한국어] 다수 사용자 — ref count 만 감소. */
		lvol->ref_count--;
		cb_fn(cb_arg, 0);
		return;
	} else if (lvol->ref_count == 0) {                 /* [한국어] 이미 close — 사용자 버그. */
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;

	lvol->action_in_progress = true;                   /* [한국어] 다른 액션 차단. */

	spdk_blob_close(lvol->blob, lvol_close_blob_cb, req);
	                                                    /* [한국어] BS 에 close 요청 — 비동기. */
}

/*
 * [한국어]
 * spdk_lvol_get_io_channel - lvol 용 IO 채널 할당 (호출 thread 의 BS 채널, 공개 API)
 *
 * @lvol: 대상.
 * @return: 새 spdk_io_channel * 또는 NULL.
 *
 * 동기: spdk_blob_io_read/write 가 IO 채널을 요구한다. IO 는 호출 thread 의 채널에서
 * 처리되므로 thread 마다 한 번씩 alloc 해서 보관 (vbdev_lvol 의 io_channel 콜백).
 */
struct spdk_io_channel *
spdk_lvol_get_io_channel(struct spdk_lvol *lvol)
{
	return spdk_bs_alloc_io_channel(lvol->lvol_store->blobstore);
	                                                    /* [한국어] BS 의 채널 alloc 위임. */
}

/*
 * [한국어]
 * lvol_inflate_cb - inflate / decouple_parent 의 공통 완료 콜백
 *
 * 두 작업 모두 임시로 채널을 alloc 했으므로 free 하고 사용자에게 결과 통지.
 */
static void
lvol_inflate_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	spdk_bs_free_io_channel(req->channel);             /* [한국어] 임시 채널 해제. */

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not inflate lvol\n");
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

/*
 * [한국어]
 * spdk_lvol_inflate - thin lvol 을 thick 로 변환 (모든 cluster 를 자체 보유, 공개 API)
 *
 * @lvol: 대상.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동기: 부모로부터 빌리던 cluster 를 모두 자체 cluster 로 복사 (실데이터를 자기 영역에
 * 배치). 결과적으로 부모 의존성이 사라지므로 부모를 destroy 할 수 있게 된다. 단점:
 * 모든 cluster 를 alloc 하므로 디스크 사용량이 늘어나고 시간이 걸린다.
 *
 * 내부 동작: spdk_bs_inflate_blob 이 cluster 를 한 개씩 read (부모) → write (자기) 한다.
 */
void
spdk_lvol_inflate(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	spdk_blob_id blob_id;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("Lvol does not exist\n");
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->channel = spdk_bs_alloc_io_channel(lvol->lvol_store->blobstore);
	                                                    /* [한국어] 임시 IO 채널 — 작업 완료 후 free. */
	if (req->channel == NULL) {
		SPDK_ERRLOG("Cannot alloc io channel for lvol inflate request\n");
		free(req);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	blob_id = spdk_blob_get_id(lvol->blob);
	spdk_bs_inflate_blob(lvol->lvol_store->blobstore, req->channel, blob_id, lvol_inflate_cb,
			     req);
	                                                    /* [한국어] BS 에 inflate 요청 — cluster
	                                                     * 단위로 read+write 반복하는 비동기 작업. */
}

/*
 * [한국어]
 * spdk_lvol_decouple_parent - lvol 의 부모와의 의존성만 해소 (공개 API)
 *
 * @lvol: 대상.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동기: inflate 와 비슷하지만, 이미 사용자 자신이 write 한 cluster 만 보유 중이고 (부모
 * 의존 cluster 는 아직 부모로부터 빌리는) 상태에서 "부모 의존 cluster 만 자기 영역으로
 * 옮긴다". 새로 alloc 되는 cluster 는 이미 사용 중이던 것보다 적을 수 있어 inflate
 * 보다 빠르고 디스크 사용량 증가도 적다.
 *
 * 내부: spdk_bs_blob_decouple_parent 가 부모 영역만 cluster 단위로 복사.
 */
void
spdk_lvol_decouple_parent(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	spdk_blob_id blob_id;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("Lvol does not exist\n");
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->channel = spdk_bs_alloc_io_channel(lvol->lvol_store->blobstore);
	                                                    /* [한국어] 임시 IO 채널. */
	if (req->channel == NULL) {
		SPDK_ERRLOG("Cannot alloc io channel for lvol inflate request\n");
		free(req);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	blob_id = spdk_blob_get_id(lvol->blob);
	spdk_bs_blob_decouple_parent(lvol->lvol_store->blobstore, req->channel, blob_id,
				     lvol_inflate_cb, req);
	                                                    /* [한국어] BS 에 decouple 요청. */
}

/*
 * [한국어]
 * lvs_grow_live_cb - spdk_lvs_grow_live 의 BS 콜백 — 사용자 cb 호출 + req 해제
 */
static void
lvs_grow_live_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_req *req = (struct spdk_lvs_req *)cb_arg;

	if (req->cb_fn) {                                  /* [한국어] cb_fn 은 NULL 가능 (공개 API
	                                                    * 가 NULL 허용). */
		req->cb_fn(req->cb_arg, lvolerrno);
	}
	free(req);
	return;
}

/*
 * [한국어]
 * spdk_lvs_grow_live - 이미 load 된 LVS 의 BS 영역을 backend 디바이스 크기에 맞게 확장 (공개 API)
 *
 * @lvs: 대상 LVS (이미 load 된 상태).
 * @cb_fn: 완료 콜백 (NULL 가능).
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동기: backend bdev 가 hot-resize 되거나 사용자가 명시적으로 BS 사용 가능 영역을 늘릴
 * 때 호출. spdk_lvs_grow 와 달리 LVS 를 unload/reload 하지 않는다 (live = online).
 */
void
spdk_lvs_grow_live(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_req *req;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for request structure\n");
		if (cb_fn) {
			cb_fn(cb_arg, -ENOMEM);
		}
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol_store = lvs;

	spdk_bs_grow_live(lvs->blobstore, lvs_grow_live_cb, req);
	                                                    /* [한국어] BS 의 live grow API 위임. */
}

/*
 * [한국어]
 * spdk_lvs_grow - LVS 를 새로 load 하면서 BS 사용 가능 영역 확장 (공개 API)
 *
 * @bs_dev: backend bs_dev (커진 backend 디바이스 위에서 만들어진 새 bs_dev).
 * @cb_fn: 완료 콜백.
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동기: 사용자가 backend 디바이스를 키운 뒤 LVS 를 unload-reload 하면서 동시에 새
 * 영역까지 cluster bitmap 등을 확장하고 싶을 때. spdk_lvs_load 의 변형 — spdk_bs_grow
 * 는 디스크의 super block 을 갱신하면서 load 한다.
 *
 * 호출 체인:
 *   사용자 → [spdk_lvs_grow] → spdk_bs_grow → lvs_load_cb → ... (이후 lvs_load 와 동일).
 */
void
spdk_lvs_grow(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_with_handle_req *req;
	struct spdk_bs_opts opts = {};

	assert(cb_fn != NULL);

	if (bs_dev == NULL) {
		SPDK_ERRLOG("Blobstore device does not exist\n");
		cb_fn(cb_arg, NULL, -ENODEV);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for request structure\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->lvol_store = lvs_alloc();                     /* [한국어] 새 LVS 핸들. */
	if (req->lvol_store == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store\n");
		free(req);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->bs_dev = bs_dev;

	lvs_bs_opts_init(&opts);                           /* [한국어] BS opts 기본값. */
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "LVOLSTORE");
	                                                    /* [한국어] magic 검증 type. */

	spdk_bs_grow(bs_dev, &opts, lvs_load_cb, req);     /* [한국어] BS grow + load — 콜백은
	                                                    * 일반 load 경로(lvs_load_cb)를 재사용. */
}

/*
 * [한국어]
 * lvs_get_lvol_by_blob_id - LVS 의 lvols TAILQ 에서 blob_id 에 해당하는 lvol 검색
 *
 * @lvs: 대상.
 * @blob_id: 찾을 blob 의 64-bit ID.
 * @return: lvol * 또는 NULL.
 *
 * 동기: BS 가 esnap_bs_dev_create 콜백을 호출할 때 (또는 destroy 의 자식 인계 처리 등)
 * blob 핸들만 알고 있는 경우 lvol 핸들을 역으로 찾을 필요가 있다.
 *
 * 실행 컨텍스트: lvs->thread (단일 thread — lock 없이 안전).
 */
static struct spdk_lvol *
lvs_get_lvol_by_blob_id(struct spdk_lvol_store *lvs, spdk_blob_id blob_id)
{
	struct spdk_lvol *lvol;

	TAILQ_FOREACH(lvol, &lvs->lvols, link) {           /* [한국어] 단순 선형 검색. lvol 수가
	                                                    * 일반적으로 수십~수천이므로 충분. */
		if (lvol->blob_id == blob_id) {
			return lvol;
		}
	}
	return NULL;                                       /* [한국어] super blob 등 lvol 이 아닌 blob. */
}

/*
 * [한국어]
 * lvs_esnap_bs_dev_create - BS 가 esnap clone 을 open 할 때 호출하는 라우터 콜백
 *
 * @bs_ctx: BS 에 등록된 컨텍스트 (= LVS *). NULL 가능.
 * @blob_ctx: blob open 시 opts.esnap_ctx 로 넘겨진 값 (= lvol *). NULL 가능.
 * @blob: 대상 blob.
 * @esnap_id: blob 의 esnap_id xattr.
 * @id_len: 길이.
 * @bs_dev: out — backing bs_dev. NULL 로 두면 BS 는 backing 없이 open (load 단계용).
 * @return: 0 = 성공, 음수 errno = 실패.
 *
 * 동기: lvol 라이브러리는 사용자(vbdev_lvol)가 등록한 esnap_bs_dev_create 콜백에
 * dispatch 한다. 그 전에 두 가지 특수 처리가 필요:
 *   1) load 단계 (lvs->load_esnaps == false): BS iteration 으로 모든 blob 을 잠시 open
 *      한다. 이때 esnap clone 까지 backing 을 매번 만드는 것은 낭비이고 race 조건도
 *      유발할 수 있으므로, *bs_dev = NULL 을 주어 BS 가 backing 없이 진행하게 한다.
 *      load 가 끝나면 lvs->load_esnaps = true 가 되어 이후 사용자가 명시적 open 하면
 *      이 라우터가 사용자 콜백을 호출.
 *   2) 사용자가 spdk_bs_blob_open (esnap_ctx 없음) 으로 직접 열거나, BS 내부 활동으로
 *      자발적 open 이 일어나면 blob_ctx (= lvol) 가 NULL 일 수 있다. 이 경우
 *      lvs_get_lvol_by_blob_id 로 lvol 을 lookup.
 */
static int
lvs_esnap_bs_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
			const void *esnap_id, uint32_t id_len,
			struct spdk_bs_dev **bs_dev)
{
	struct spdk_lvol_store	*lvs = bs_ctx;             /* [한국어] BS context = LVS. */
	struct spdk_lvol	*lvol = blob_ctx;          /* [한국어] blob open ctx = lvol (또는 NULL). */
	spdk_blob_id		blob_id = spdk_blob_get_id(blob);

	if (lvs == NULL) {                                 /* [한국어] BS 가 ctx 없이 호출 — lvol 에서
	                                                    * lvs 추출. */
		if (lvol == NULL || lvol->lvol_store == NULL) {
			SPDK_ERRLOG("Blob 0x%" PRIx64 ": no lvs context nor lvol context\n",
				    blob_id);
			return -EINVAL;
		}
		lvs = lvol->lvol_store;
	}

	/*
	 * When spdk_lvs_load() is called, it iterates through all blobs in its blobstore building
	 * up a list of lvols (lvs->lvols). During this initial iteration, each blob is opened,
	 * passed to load_next_lvol(), then closed. There is no need to open the external snapshot
	 * during this phase. Once the blobstore is loaded, lvs->load_esnaps is set to true so that
	 * future lvol opens cause the external snapshot to be loaded.
	 */
	/* [한국어] 위 영문 주석 요약: load 단계의 짧은 open/close 에서는 esnap backing 생성을 skip. */
	if (!lvs->load_esnaps) {                           /* [한국어] load 단계의 임시 open. */
		*bs_dev = NULL;                            /* [한국어] BS 는 backing 없이 진행. */
		return 0;
	}

	if (lvol == NULL) {                                /* [한국어] 사용자가 open_ext 가 아닌 open
	                                                    * 으로 직접 열었거나 BS 자체 활동. */
		spdk_blob_id blob_id = spdk_blob_get_id(blob);
		                                            /* [한국어] (위 변수 가림 — 안전한 재선언.) */

		/*
		 * If spdk_bs_blob_open() is used instead of spdk_bs_blob_open_ext() the lvol will
		 * not have been passed in. The same is true if the open happens spontaneously due
		 * to blobstore activity.
		 */
		/* [한국어] 위 영문 주석대로: blob_id 로 lvol 을 lookup. */
		lvol = lvs_get_lvol_by_blob_id(lvs, blob_id);
		if (lvol == NULL) {
			SPDK_ERRLOG("lvstore %s: no lvol for blob 0x%" PRIx64 "\n",
				    lvs->name, blob_id);
			return -ENODEV;
		}
	}

	return lvs->esnap_bs_dev_create(lvs, lvol, blob, esnap_id, id_len, bs_dev);
	                                                    /* [한국어] 사용자 콜백(vbdev_lvol_esnap_
	                                                     * dev_create 등) 으로 dispatch. 사용자가
	                                                     * 외부 bdev 를 spdk_bs_dev 로 wrap 해
	                                                     * 반환하면 BS 가 backing 으로 사용. */
}

/*
 * The theory of missing external snapshots
 *
 * The lvs->esnap_bs_dev_create() callback may be unable to create an external snapshot bs_dev when
 * it is called. This can happen, for instance, as when the device containing the lvolstore is
 * examined prior to spdk_bdev_register() being called on a bdev that acts as an external snapshot.
 * In such a case, the esnap_bs_dev_create() callback will call spdk_lvs_esnap_missing_add().
 *
 * Missing external snapshots are tracked in a per-lvolstore tree, lvs->degraded_lvol_sets_tree.
 * Each tree node (struct spdk_lvs_degraded_lvol_set) contains a tailq of lvols that are missing
 * that particular external snapshot.
 *
 * When a potential missing snapshot becomes available, spdk_lvs_notify_hotplug() may be called to
 * notify this library that it is available. It will then iterate through the active lvolstores and
 * search each lvs->degraded_lvol_sets_tree for a set of degraded lvols that are missing an external
 * snapshot matching the id passed in the notification. The lvols in the tailq on each matching tree
 * node are then asked to create an external snapshot bs_dev using the esnap_bs_dev_create()
 * callback that the consumer registered with the lvolstore. If lvs->esnap_bs_dev_create() returns
 * 0, the lvol is removed from the spdk_lvs_degraded_lvol_set's lvol tailq. When this tailq becomes
 * empty, the degraded lvol set node for this missing external snapshot is removed.
 */
/* [한국어] 위 영문 블록 요약: external snapshot (esnap) 의 backing bdev 가 lvol 보다 늦게
 *           등장할 수 있다 (예: vbdev examine 순서 문제). 그동안 backing 이 없는 lvol 을
 *           "degraded" 로 추적해야 backing 이 hotplug 되었을 때 즉시 정상화할 수 있다.
 *
 *           자료구조: 한 LVS 마다 lvs->degraded_lvol_sets_tree (RB tree). 트리 노드는
 *           "한 esnap_id 를 기다리는 lvol 들의 set". 한 노드 안에 같은 esnap 을 기다리는
 *           lvol TAILQ.
 *
 *           시점:
 *             - lvol open 도중 esnap_bs_dev_create 가 backing 못 만들면
 *               spdk_lvs_esnap_missing_add 호출.
 *             - vbdev_lvol 의 examine 콜백에서 새 bdev 등장 알림 → spdk_lvs_notify_hotplug.
 *               해당 esnap_id 와 매칭되는 set 의 모든 lvol 에 대해 backing 재시도. 성공한
 *               lvol 은 set 에서 빠지고, set 이 비면 트리 노드도 제거.
 */

/*
 * [한국어]
 * lvs_esnap_name_cmp - degraded set RB tree 의 비교 함수
 *
 * @m1, @m2: 두 set 노드.
 * @return: 음수 = m1 < m2, 0 = 같음, 양수 = m1 > m2.
 *
 * 정렬 키: (id_len, esnap_id 바이트열). 길이가 다르면 길이로 단조 비교, 같으면 memcmp.
 */
static int
lvs_esnap_name_cmp(struct spdk_lvs_degraded_lvol_set *m1, struct spdk_lvs_degraded_lvol_set *m2)
{
	if (m1->id_len == m2->id_len) {                    /* [한국어] 같은 길이 — 바이트 비교. */
		return memcmp(m1->esnap_id, m2->esnap_id, m1->id_len);
	}
	return (m1->id_len > m2->id_len) ? 1 : -1;         /* [한국어] 길이가 다르면 길이로. */
}

RB_GENERATE_STATIC(degraded_lvol_sets_tree, spdk_lvs_degraded_lvol_set, node, lvs_esnap_name_cmp)
/* [한국어] BSD <sys/tree.h> 의 매크로 — degraded_lvol_sets_tree 라는 이름의 RB tree
 *           구현 함수들 (insert/remove/find 등) 을 file-static 으로 자동 생성. 위에서
 *           정의된 lvs_esnap_name_cmp 를 비교 함수로 사용. */

/*
 * [한국어]
 * lvs_degraded_lvol_set_add - lvol 을 degraded set 의 lvol TAILQ 에 추가
 *
 * @degraded_set: 대상 set.
 * @lvol: 추가할 lvol.
 *
 * 동기: spdk_lvs_esnap_missing_add 또는 lvol_create_cb (snapshot 인계) 에서 호출.
 * lvol->degraded_set 역참조와 TAILQ 등록을 함께 처리.
 */
static void
lvs_degraded_lvol_set_add(struct spdk_lvs_degraded_lvol_set *degraded_set, struct spdk_lvol *lvol)
{
	assert(lvol->lvol_store->thread == spdk_get_thread());
	                                                    /* [한국어] lvs->thread 단일 thread 검증. */

	lvol->degraded_set = degraded_set;                 /* [한국어] 역참조 — destroy 시 자리 인계
	                                                    * 처리에 사용. */
	TAILQ_INSERT_TAIL(&degraded_set->lvols, lvol, degraded_link);
}

/*
 * [한국어]
 * lvs_degraded_lvol_set_remove - lvol 을 degraded set 의 lvol TAILQ 에서 제거
 *
 * @degraded_set: 대상 set.
 * @lvol: 제거할 lvol.
 *
 * 주의: 호출자는 set 이 비었는지(TAILQ_EMPTY) 직접 확인하고 필요하다면 트리 노드도
 * 제거해야 한다 — 이 함수는 lvol 만 떼어낸다.
 */
static void
lvs_degraded_lvol_set_remove(struct spdk_lvs_degraded_lvol_set *degraded_set,
			     struct spdk_lvol *lvol)
{
	assert(lvol->lvol_store->thread == spdk_get_thread());

	lvol->degraded_set = NULL;                         /* [한국어] 역참조 해제. */
	TAILQ_REMOVE(&degraded_set->lvols, lvol, degraded_link);
	/* degraded_set->lvols may be empty. Caller should check if not immediately adding a new
	 * lvol. */
	/* [한국어] 위 영문 주석 강조: 비었는지는 호출자 책임. */
}

/*
 * Record in lvs->degraded_lvol_sets_tree that a bdev of the specified name is needed by the
 * specified lvol.
 */
/*
 * [한국어]
 * spdk_lvs_esnap_missing_add - "이 lvol 이 esnap_id 를 기다린다" 는 사실을 LVS 추적 트리에 기록
 *
 * @lvs: 대상 LVS.
 * @lvol: 기다리는 lvol.
 * @esnap_id: 누락된 외부 스냅샷 식별자.
 * @id_len: 길이.
 * @return: 0 = OK, -ENOMEM = 메모리 부족.
 *
 * 동기: vbdev_lvol 의 esnap_bs_dev_create 콜백이 backing 을 만들 수 없을 때 (예: 해당 backing
 * bdev 가 아직 등록되지 않음) 이 함수를 호출. 추후 hotplug 알림이 오면 일괄 재시도.
 *
 * 동작:
 *   1) 트리에 동일 esnap_id set 이 있는지 RB_FIND.
 *   2) 없으면 새 노드 calloc + esnap_id 사본 + lvols TAILQ_INIT + RB_INSERT.
 *   3) 노드의 lvols 에 lvol 추가.
 *
 * 실행 컨텍스트: lvs->thread (단일 thread).
 */
int
spdk_lvs_esnap_missing_add(struct spdk_lvol_store *lvs, struct spdk_lvol *lvol,
			   const void *esnap_id, uint32_t id_len)
{
	struct spdk_lvs_degraded_lvol_set find, *degraded_set;
	                                                    /* [한국어] find = RB_FIND 를 위한 stack
	                                                     * 임시 키. degraded_set = 결과/새로 만든 노드. */

	assert(lvs->thread == spdk_get_thread());

	find.esnap_id = esnap_id;                          /* [한국어] 검색 키 esnap_id. */
	find.id_len = id_len;
	degraded_set = RB_FIND(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree, &find);
	if (degraded_set == NULL) {                        /* [한국어] 새 esnap_id — 노드 생성. */
		degraded_set = calloc(1, sizeof(*degraded_set));
		if (degraded_set == NULL) {
			SPDK_ERRLOG("lvol %s: cannot create degraded_set node: out of memory\n",
				    lvol->unique_id);
			return -ENOMEM;
		}
		degraded_set->esnap_id = calloc(1, id_len); /* [한국어] esnap_id 의 사본 — 호출자
		                                             * buffer 가 free 되어도 안전. */
		if (degraded_set->esnap_id == NULL) {
			free(degraded_set);
			SPDK_ERRLOG("lvol %s: cannot create degraded_set node: out of memory\n",
				    lvol->unique_id);
			return -ENOMEM;
		}
		memcpy((void *)degraded_set->esnap_id, esnap_id, id_len);
		                                            /* [한국어] (void *) 캐스트 — esnap_id 는
		                                             * const void * 이지만 set 노드 자체는 사본을
		                                             * 소유하므로 쓰기 가능. */
		degraded_set->id_len = id_len;
		degraded_set->lvol_store = lvs;
		TAILQ_INIT(&degraded_set->lvols);          /* [한국어] 빈 lvol TAILQ. */
		RB_INSERT(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree, degraded_set);
		                                            /* [한국어] 트리에 등록. */
	}

	lvs_degraded_lvol_set_add(degraded_set, lvol);     /* [한국어] lvol 을 set 에 추가. */

	return 0;
}

/*
 * Remove the record of the specified lvol needing a degraded_set bdev.
 */
/*
 * [한국어]
 * spdk_lvs_esnap_missing_remove - lvol 의 "missing" 추적 해제 (set 이 비면 트리 노드도 제거)
 *
 * @lvol: 대상 lvol.
 *
 * 동작:
 *   1) lvol->degraded_set 이 NULL 이면 추적 안 됨 — no-op.
 *   2) 아니면 set 의 TAILQ 에서 제거.
 *   3) set 이 비었으면 RB_REMOVE 후 esnap_id 사본 + 노드 free (라이프사이클 종료).
 *
 * 호출자: spdk_lvs_unload (모든 lvol), lvol_delete_blob_cb (자식 없는 destroy),
 *         사용자 콜백 (esnap 이 정상 backing 으로 전환된 경우는 lvs_esnap_degraded_hotplug
 *         가 직접 처리).
 */
void
spdk_lvs_esnap_missing_remove(struct spdk_lvol *lvol)
{
	struct spdk_lvol_store		*lvs = lvol->lvol_store;
	struct spdk_lvs_degraded_lvol_set	*degraded_set = lvol->degraded_set;

	assert(lvs->thread == spdk_get_thread());

	if (degraded_set == NULL) {                        /* [한국어] 추적되지 않는 lvol — 무시. */
		return;
	}

	lvs_degraded_lvol_set_remove(degraded_set, lvol);  /* [한국어] TAILQ 에서 떼어냄. */

	if (!TAILQ_EMPTY(&degraded_set->lvols)) {          /* [한국어] 다른 lvol 이 같은 esnap 을
	                                                    * 기다리고 있으면 set 노드는 유지. */
		return;
	}

	RB_REMOVE(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree, degraded_set);
	                                                    /* [한국어] 트리에서 제거. */

	free((char *)degraded_set->esnap_id);              /* [한국어] esnap_id 사본 free. */
	free(degraded_set);                                /* [한국어] 노드 free. */
}

/*
 * [한국어]
 * struct lvs_esnap_hotplug_req - hotplug 시 사용자 콜백 호출에 사용할 임시 컨텍스트
 */
struct lvs_esnap_hotplug_req {
	struct spdk_lvol			*lvol;
	/* [한국어] hotplug 대상 lvol. lvs_esnap_degraded_hotplug 가 set 의 lvol 을
	 * 순회하며 각 lvol 마다 하나씩 alloc.
	 * 설정자: lvs_esnap_degraded_hotplug.
	 * 읽는 자: lvs_esnap_hotplug_done — 사용자에게 어느 lvol 의 결과인지 통지. */

	spdk_lvol_op_with_handle_complete	cb_fn;
	/* [한국어] 사용자 hotplug 콜백 (spdk_lvs_notify_hotplug 호출자가 등록).
	 * 설정자: lvs_esnap_degraded_hotplug 가 호출자 cb_fn 그대로 복사.
	 * 읽는 자: lvs_esnap_hotplug_done. 호출 시점 = blob_set_esnap_bs_dev 완료. */

	void					*cb_arg;
	/* [한국어] cb_fn 의 첫 인자 — 사용자 컨텍스트. */
};

/*
 * [한국어]
 * lvs_esnap_hotplug_done - blob_set_esnap_bs_dev 의 완료 콜백 → 사용자에게 lvol+결과 통지
 *
 * @cb_arg: lvs_esnap_hotplug_req.
 * @bserrno: blob 에 새 backing 을 연결한 결과.
 */
static void
lvs_esnap_hotplug_done(void *cb_arg, int bserrno)
{
	struct lvs_esnap_hotplug_req *req = cb_arg;
	struct spdk_lvol	*lvol = req->lvol;
	struct spdk_lvol_store	*lvs = lvol->lvol_store;

	if (bserrno != 0) {                                /* [한국어] backing 연결 실패. */
		SPDK_ERRLOG("lvol %s/%s: failed to hotplug blob_bdev due to error %d\n",
			    lvs->name, lvol->name, bserrno);
	}
	req->cb_fn(req->cb_arg, lvol, bserrno);            /* [한국어] 사용자 통지. */
	free(req);
}

/*
 * [한국어]
 * lvs_esnap_degraded_hotplug - 한 degraded set 안의 모든 lvol 에 대해 backing 재시도
 *
 * @degraded_set: 대상 set.
 * @cb_fn: 사용자 콜백 (각 lvol 마다 한 번씩 호출됨).
 * @cb_arg: 사용자 컨텍스트.
 *
 * 동기: 새 backing bdev 가 등장 — 그동안 그것을 기다리던 모든 lvol 의 blob 에 backing
 * bs_dev 를 연결한다 (spdk_blob_set_esnap_bs_dev). 각 lvol 마다 사용자 콜백이 1번 호출.
 *
 * 까다로운 점: lvs->esnap_bs_dev_create 가 또다시 spdk_lvs_esnap_missing_add 를 부를 수
 * 있어서 (예: 같은 backing 이지만 검증 단계에서 또 실패) 우리가 순회 중인 TAILQ 가
 * 변경될 수 있다. 이 함수는 그 가능성을 안전하게 다루는 패턴을 사용:
 *   1) 시작 시점에 last_missing = TAILQ_LAST 기억.
 *   2) 한 lvol 처리할 때 먼저 TAILQ_REMOVE → 콜백이 missing_add 를 호출하면 TAIL 에 다시
 *      추가될 수 있지만, last_missing 까지만 처리하므로 무한 루프 방지.
 *   3) 처리 후 set 이 비었으면 트리에서 set 자체도 제거.
 *
 * 실행 컨텍스트: lvs->thread 단일 thread. 동시에 다른 hotplug 가 동일 set 에 들어올 수 없음.
 */
static void
lvs_esnap_degraded_hotplug(struct spdk_lvs_degraded_lvol_set *degraded_set,
			   spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store	*lvs = degraded_set->lvol_store;
	struct spdk_lvol	*lvol, *tmp, *last_missing;
	struct spdk_bs_dev	*bs_dev;
	const void		*esnap_id = degraded_set->esnap_id;
	uint32_t		id_len = degraded_set->id_len;
	struct lvs_esnap_hotplug_req *req;
	int			rc;

	assert(lvs->thread == spdk_get_thread());

	/*
	 * When lvs->esnap_bs_bdev_create() tries to load an external snapshot, it can encounter
	 * errors that lead it to calling spdk_lvs_esnap_missing_add(). This function needs to be
	 * sure that such modifications do not lead to degraded_set->lvols tailqs or references
	 * to memory that this function will free.
	 *
	 * While this function is running, no other thread can add items to degraded_set->lvols. If
	 * the list is mutated, it must have been done by this function or something in its call
	 * graph running on this thread.
	 */
	/* [한국어] 위 영문 주석: 콜백이 missing_add 를 호출해 같은 set 에 lvol 을 다시 넣어도
	 *           안전해야 한다. last_missing 마커로 종료점을 fixed snapshot 처럼 잡는다.
	 *           다른 thread 에서의 변경 가능성은 lvs->thread 단독 접근 가정으로 배제. */

	/* Remember the last lvol on the list. Iteration will stop once it has been processed. */
	/* [한국어] 시작 시점의 마지막 lvol 을 기억 — 그 이후 추가된 것은 모두 콜백 도중 (혹은
	 *           실패 재삽입) 이므로 같은 hotplug 라운드에서 또 처리하지 않는다. */
	last_missing = TAILQ_LAST(&degraded_set->lvols, degraded_lvols);

	TAILQ_FOREACH_SAFE(lvol, &degraded_set->lvols, degraded_link, tmp) {
		                                            /* [한국어] _SAFE 변종 — 순회 중 REMOVE 가
		                                             * 안전하도록. */
		req = calloc(1, sizeof(*req));
		if (req == NULL) {
			SPDK_ERRLOG("lvol %s: failed to create esnap bs_dev: out of memory\n",
				    lvol->unique_id);
			cb_fn(cb_arg, lvol, -ENOMEM);
			/* The next one likely won't succeed either, but keep going so that all the
			 * failed hotplugs are logged.
			 */
			/* [한국어] 위 영문 주석대로: 어차피 다음도 ENOMEM 이지만 모든 실패를
			 *           로그에 남기기 위해 계속 진행. */
			goto next;
		}

		/*
		 * Remove the lvol from the tailq so that tailq corruption is avoided if
		 * lvs->esnap_bs_dev_create() calls spdk_lvs_esnap_missing_add(lvol).
		 */
		/* [한국어] 위 영문 주석: 콜백이 같은 lvol 에 대해 missing_add 를 다시 호출해도
		 *           안전하도록 미리 떼어둔다. */
		TAILQ_REMOVE(&degraded_set->lvols, lvol, degraded_link);
		lvol->degraded_set = NULL;

		bs_dev = NULL;
		rc = lvs->esnap_bs_dev_create(lvs, lvol, lvol->blob, esnap_id, id_len, &bs_dev);
		                                            /* [한국어] 사용자 콜백 (vbdev_lvol) — 실제
		                                             * backing bdev 를 spdk_bs_dev 로 wrap. */
		if (rc != 0) {                             /* [한국어] 여전히 실패 — set 으로 다시 복귀. */
			SPDK_ERRLOG("lvol %s: failed to create esnap bs_dev: error %d\n",
				    lvol->unique_id, rc);
			lvol->degraded_set = degraded_set;
			TAILQ_INSERT_TAIL(&degraded_set->lvols, lvol, degraded_link);
			                                    /* [한국어] 다시 set 끝에 등록 — last_missing
			                                     * 다음에 위치하므로 이 라운드에서 또
			                                     * 시도되지는 않는다. */
			cb_fn(cb_arg, lvol, rc);           /* [한국어] 사용자 통지. */
			free(req);
			goto next;
		}

		req->lvol = lvol;
		req->cb_fn = cb_fn;
		req->cb_arg = cb_arg;
		spdk_blob_set_esnap_bs_dev(lvol->blob, bs_dev, lvs_esnap_hotplug_done, req);
		                                            /* [한국어] BS 의 in-memory blob 에 새 backing
		                                             * 연결 — 비동기 메타 sync 후 hotplug_done
		                                             * 에서 사용자 통지. */

next:
		if (lvol == last_missing) {
			/*
			 * Anything after last_missing was added due to some problem encountered
			 * while trying to create the esnap bs_dev.
			 */
			/* [한국어] last_missing 에 도달 — 그 이후 추가된 항목은 본 라운드에서
			 *           처리하지 않는다 (다음 hotplug 통지 시 재시도). */
			break;
		}
	}

	if (TAILQ_EMPTY(&degraded_set->lvols)) {           /* [한국어] 모든 lvol 이 정상화됨 — set 노드
	                                                    * 도 트리에서 제거 + free. */
		RB_REMOVE(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree, degraded_set);
		free((void *)degraded_set->esnap_id);
		free(degraded_set);
	}
}

/*
 * Notify each lvstore created on this thread that is missing a bdev by the specified name or uuid
 * that the bdev now exists.
 */
/*
 * [한국어]
 * spdk_lvs_notify_hotplug - 외부 backing bdev 가 등장했음을 lvol 라이브러리에 알림 (공개 API)
 *
 * @esnap_id: 새로 등장한 backing 의 식별자 (보통 bdev UUID 문자열).
 * @id_len: 길이.
 * @cb_fn: 각 lvol hotplug 결과 콜백.
 * @cb_arg: 사용자 컨텍스트.
 * @return: true = 매칭되는 LVS 가 하나 이상 있어 hotplug 시작, false = 매칭 없음.
 *
 * 동기: vbdev_lvol 의 examine_config 에서 새 bdev 가 등장할 때마다 호출. 모든 LVS 의
 * degraded_lvol_sets_tree 를 검색해 esnap_id 일치하는 set 이 있으면 hotplug.
 *
 * thread 검증: vbdev_lvol 은 보통 app thread 에서 examine 콜백을 호출한다. LVS 의
 * lvs->thread 와 호출 thread 가 다르면 — wrong thread — discard. 정상 시나리오에서는
 * RPC, examine_disk, examine_config 모두 app thread 에서 일어남.
 */
bool
spdk_lvs_notify_hotplug(const void *esnap_id, uint32_t id_len,
			spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_degraded_lvol_set *found;
	struct spdk_lvs_degraded_lvol_set find = { 0 };    /* [한국어] RB_FIND 키. */
	struct spdk_lvol_store	*lvs;
	struct spdk_thread	*thread = spdk_get_thread();
	bool			ret = false;

	find.esnap_id = esnap_id;
	find.id_len = id_len;

	pthread_mutex_lock(&g_lvol_stores_mutex);          /* [한국어] 전역 LVS 리스트 보호. */
	TAILQ_FOREACH(lvs, &g_lvol_stores, link) {         /* [한국어] 모든 LVS 검사. */
		if (thread != lvs->thread) {               /* [한국어] thread 일치 검사. */
			/*
			 * It is expected that this is called from vbdev_lvol's examine_config()
			 * callback. The lvstore was likely loaded do a creation happening as a
			 * result of an RPC call or opening of an existing lvstore via
			 * examine_disk() callback. RPC calls, examine_disk(), and examine_config()
			 * should all be happening only on the app thread. The "wrong thread"
			 * condition will only happen when an application is doing something weird.
			 */
			/* [한국어] 위 영문 주석: 정상 사용에서는 절대 발생하지 않아야 한다. */
			SPDK_NOTICELOG("Discarded examine for lvstore %s: wrong thread\n",
				       lvs->name);
			continue;
		}

		found = RB_FIND(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree, &find);
		                                            /* [한국어] 같은 esnap_id set 이 있는가. */
		if (found == NULL) {
			continue;                          /* [한국어] 이 LVS 는 해당 esnap 안 기다림. */
		}

		ret = true;                                /* [한국어] 적어도 한 LVS 에서 hotplug 발생. */
		lvs_esnap_degraded_hotplug(found, cb_fn, cb_arg);
		                                            /* [한국어] 해당 set 의 모든 lvol 처리. */
	}
	pthread_mutex_unlock(&g_lvol_stores_mutex);

	return ret;
}

int
spdk_lvol_iter_immediate_clones(struct spdk_lvol *lvol, spdk_lvol_iter_cb cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs = lvol->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;
	struct spdk_lvol *clone;
	spdk_blob_id *ids;
	size_t id_cnt = 0;
	size_t i;
	int rc;

	rc = spdk_blob_get_clones(bs, lvol->blob_id, NULL, &id_cnt);
	if (rc != -ENOMEM) {
		/* -ENOMEM says id_cnt is valid, no other errors should be returned. */
		assert(rc == 0);
		return rc;
	}

	ids = calloc(id_cnt, sizeof(*ids));
	if (ids == NULL) {
		SPDK_ERRLOG("lvol %s: out of memory while iterating clones\n", lvol->unique_id);
		return -ENOMEM;
	}

	rc = spdk_blob_get_clones(bs, lvol->blob_id, ids, &id_cnt);
	if (rc != 0) {
		SPDK_ERRLOG("lvol %s: unable to get clone blob IDs: %d\n", lvol->unique_id, rc);
		free(ids);
		return rc;
	}

	for (i = 0; i < id_cnt; i++) {
		clone = lvs_get_lvol_by_blob_id(lvs, ids[i]);
		if (clone == NULL) {
			SPDK_NOTICELOG("lvol %s: unable to find clone lvol with blob id 0x%"
				       PRIx64 "\n", lvol->unique_id, ids[i]);
			continue;
		}
		rc = cb_fn(cb_arg, clone);
		if (rc != 0) {
			SPDK_DEBUGLOG(lvol, "lvol %s: iteration stopped when lvol %s (blob 0x%"
				      PRIx64 ") returned %d\n", lvol->unique_id, clone->unique_id,
				      ids[i], rc);
			break;
		}
	}

	free(ids);
	return rc;
}

struct spdk_lvol *
spdk_lvol_get_by_uuid(const struct spdk_uuid *uuid)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;

	pthread_mutex_lock(&g_lvol_stores_mutex);

	TAILQ_FOREACH(lvs, &g_lvol_stores, link) {
		TAILQ_FOREACH(lvol, &lvs->lvols, link) {
			if (spdk_uuid_compare(uuid, &lvol->uuid) == 0) {
				pthread_mutex_unlock(&g_lvol_stores_mutex);
				return lvol;
			}
		}
	}

	pthread_mutex_unlock(&g_lvol_stores_mutex);
	return NULL;
}

struct spdk_lvol *
spdk_lvol_get_by_names(const char *lvs_name, const char *lvol_name)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;

	pthread_mutex_lock(&g_lvol_stores_mutex);

	TAILQ_FOREACH(lvs, &g_lvol_stores, link) {
		if (strcmp(lvs_name, lvs->name) != 0) {
			continue;
		}
		TAILQ_FOREACH(lvol, &lvs->lvols, link) {
			if (strcmp(lvol_name, lvol->name) == 0) {
				pthread_mutex_unlock(&g_lvol_stores_mutex);
				return lvol;
			}
		}
	}

	pthread_mutex_unlock(&g_lvol_stores_mutex);
	return NULL;
}

bool
spdk_lvol_is_degraded(const struct spdk_lvol *lvol)
{
	struct spdk_blob *blob = lvol->blob;

	if (blob == NULL) {
		return true;
	}
	return spdk_blob_is_degraded(blob);
}

static void
lvol_shallow_copy_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_copy_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	spdk_bs_free_io_channel(req->channel);

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not make a shallow copy of lvol %s, error %d\n", lvol->unique_id, lvolerrno);
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

int
spdk_lvol_shallow_copy(struct spdk_lvol *lvol, struct spdk_bs_dev *ext_dev,
		       spdk_blob_shallow_copy_status status_cb_fn, void *status_cb_arg,
		       spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_copy_req *req;
	spdk_blob_id blob_id;
	int rc;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol must not be NULL\n");
		return -EINVAL;
	}

	assert(lvol->lvol_store->thread == spdk_get_thread());

	if (ext_dev == NULL) {
		SPDK_ERRLOG("lvol %s shallow copy, ext_dev must not be NULL\n", lvol->unique_id);
		return -EINVAL;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("lvol %s shallow copy, cannot alloc memory for lvol request\n", lvol->unique_id);
		return -ENOMEM;
	}

	req->lvol = lvol;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->channel = spdk_bs_alloc_io_channel(lvol->lvol_store->blobstore);
	if (req->channel == NULL) {
		SPDK_ERRLOG("lvol %s shallow copy, cannot alloc io channel for lvol request\n", lvol->unique_id);
		free(req);
		return -ENOMEM;
	}

	blob_id = spdk_blob_get_id(lvol->blob);

	rc = spdk_bs_blob_shallow_copy(lvol->lvol_store->blobstore, req->channel, blob_id, ext_dev,
				       status_cb_fn, status_cb_arg, lvol_shallow_copy_cb, req);

	if (rc < 0) {
		SPDK_ERRLOG("Could not make a shallow copy of lvol %s\n", lvol->unique_id);
		spdk_bs_free_io_channel(req->channel);
		free(req);
	}

	return rc;
}

static void
lvol_set_parent_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("could not set parent of lvol %s, error %d\n", req->lvol->name, lvolerrno);
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

void
spdk_lvol_set_parent(struct spdk_lvol *lvol, struct spdk_lvol *snapshot,
		     spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	spdk_blob_id blob_id, snapshot_id;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol must not be NULL\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	if (snapshot == NULL) {
		SPDK_ERRLOG("snapshot must not be NULL\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->lvol = lvol;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	blob_id = spdk_blob_get_id(lvol->blob);
	snapshot_id = spdk_blob_get_id(snapshot->blob);

	spdk_bs_blob_set_parent(lvol->lvol_store->blobstore, blob_id, snapshot_id,
				lvol_set_parent_cb, req);
}

static void
lvol_set_external_parent_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_bs_dev_req *req = cb_arg;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("could not set external parent of lvol %s, error %d\n", req->lvol->name, lvolerrno);
		req->bs_dev->destroy(req->bs_dev);
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

void
spdk_lvol_set_external_parent(struct spdk_lvol *lvol, const void *esnap_id, uint32_t esnap_id_len,
			      spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_bs_dev_req *req;
	struct spdk_bs_dev *bs_dev;
	spdk_blob_id blob_id;
	int rc;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol must not be NULL\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	if (esnap_id == NULL) {
		SPDK_ERRLOG("snapshot must not be NULL\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	if (esnap_id_len == sizeof(lvol->uuid_str) &&
	    memcmp(esnap_id, lvol->uuid_str, esnap_id_len) == 0) {
		SPDK_ERRLOG("lvol %s and esnap have the same UUID\n", lvol->name);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	rc = lvs_esnap_bs_dev_create(lvol->lvol_store, lvol, lvol->blob, esnap_id, esnap_id_len, &bs_dev);
	if (rc < 0) {
		cb_fn(cb_arg, rc);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->lvol = lvol;
	req->bs_dev = bs_dev;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	blob_id = spdk_blob_get_id(lvol->blob);

	spdk_bs_blob_set_external_parent(lvol->lvol_store->blobstore, blob_id, bs_dev, esnap_id,
					 esnap_id_len, lvol_set_external_parent_cb, req);
}
