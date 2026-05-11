/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] fsdev(파일시스템 디바이스) 코어 프레임워크 (fsdev.c)
 *
 * === 파일의 역할 ===
 * SPDK는 블록 디바이스 추상화(bdev) 외에 파일 시스템 호출(open/lookup/read/write/setattr 등)을
 * 모듈화한 "fsdev" 추상화를 제공한다. 본 파일은 그 코어 — 즉 fsdev 디스크립터(spdk_fsdev_desc),
 * fsdev IO 객체(spdk_fsdev_io), fsdev IO 채널(spdk_fsdev_channel), 글로벌 매니저(g_fsdev_mgr),
 * 그리고 IO 풀(spdk_mempool) 등 라이프사이클 자체를 책임진다. 백엔드 구현(예: aio_fs, fuse-기반,
 * NFS proxy)은 이 코어 위에 모듈로 등록되어 vhost-fs/virtiofs 프런트엔드의 요청을 받아 처리한다.
 * 이 파일은 다음을 한다:
 *   1) fsdev 매니저 초기화/종료 (constructor + spdk_fsdev_initialize/finish).
 *   2) 모듈/fsdev 등록·해제 (spdk_fsdev_module_list_add/find, spdk_fsdev_register/unregister).
 *   3) per-thread mgmt 채널과 fsdev 채널 생성/파괴 (spdk_io_device 추상화 위에).
 *   4) fsdev_io 풀 관리 (per-thread cache + global mempool).
 *   5) IO 디스패치/완료 (fsdev_io_submit, spdk_fsdev_io_complete).
 *   6) descriptor open/close + hot-remove 통보.
 *   7) 글로벌 옵션 get/set (RPC 백엔드).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   vhost-user-fs / virtiofs 프런트엔드 (QEMU)
 *     → SPDK vhost-fs / virtiofs 트랜스포트
 *       → spdk_fsdev_open(name, ...) → spdk_fsdev_desc
 *       → spdk_fsdev_get_io_channel(desc) → spdk_io_channel(=spdk_fsdev_channel)
 *       → 공개 IO API(spdk_fsdev_lookup/read/write 등 — 이 파일이 아닌 fsdev_io.c 또는 헤더 inline)
 *           → fsdev_channel_get_io → fsdev_io_submit → fsdev->fn_table->submit_request
 *             → 모듈(aio/fuse/...) → 실제 파일시스템 호출
 *               → 모듈이 spdk_fsdev_io_complete 호출 → 사용자 cb_fn
 *
 * SPDK Reactor/Thread 모델: 모든 IO 흐름은 디스크립터/채널을 소유한 SPDK thread에서 직렬 실행되며,
 * cross-thread 작업은 spdk_thread_send_msg로 처리된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/thread(io_channel/spdk_thread/send_msg), spdk/env(spdk_mempool/spdk_spinlock),
 *   spdk/notify(이벤트 통보), spdk/fsdev_module(모듈 콜백 시그니처), fsdev_internal.h(내부 API).
 * - 의존받음: lib/vhost(vhost-fs/virtiofs 트랜스포트), module/fsdev/aio 등 백엔드 모듈,
 *   lib/fsdev/fsdev_rpc.c(옵션 RPC), lib/fsdev/fsdev_io.c(IO 헬퍼).
 * - 데이터 흐름: 부팅 시 g_fsdev_mgr 초기화 → 모듈 등록 → fsdev 인스턴스 등록 → 사용자 open/IO →
 *   해제 시 unregister/finish.
 * - 공유 자료구조: 전역 g_fsdev_mgr(매니저), g_fsdev_opts(옵션), 그리고 RB tree로 관리되는
 *   fsdev_name_tree(이름 → fsdev 매핑).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_fsdev_mgr               : 전역 매니저(IO 풀, 모듈/fsdev 리스트, RB tree, spinlock).
 * - struct spdk_fsdev_mgmt_channel      : per-thread 관리 채널(IO 풀의 thread-local cache).
 * - struct spdk_fsdev_shared_resource   : per-(io_device, thread) 공유 자원(같은 백엔드 채널 공유).
 * - struct spdk_fsdev_channel           : per-(fsdev, thread) IO 채널(io_outstanding, submitted 큐).
 * - struct spdk_fsdev_desc              : open 결과 디스크립터(이벤트 콜백, refcount, closed flag).
 * - spdk_fsdev_initialize / finish      : 부팅/종료 라이프사이클.
 * - spdk_fsdev_register / unregister    : fsdev 인스턴스 등록/해제.
 * - spdk_fsdev_open / close             : 디스크립터 라이프사이클.
 * - fsdev_channel_get_io / spdk_fsdev_free_io : IO 풀 빌림/반환(per-thread cache 우선).
 * - fsdev_io_submit / spdk_fsdev_io_complete  : IO 디스패치/완료.
 * - spdk_fsdev_subsystem_config_json    : 현재 설정을 JSON으로 출력(저장 형태).
 * - spdk_fsdev_get_opts / set_opts      : 글로벌 옵션 get/set (RPC 백엔드가 호출).
 */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 헤더 묶음 — size_t/uint*/bool/string.h 등 일괄 가시화. */
#include "spdk/fsdev.h"
/* [한국어] fsdev 공개 API 시그니처와 spdk_fsdev_opts/desc/io 구조체. */
#include "spdk/config.h"
/* [한국어] 빌드 시 결정되는 SPDK_CONFIG_* 매크로 — 조건부 컴파일 가시화. */
#include "spdk/env.h"
/* [한국어] DPDK 추상화 — spdk_mempool_*, spdk_spinlock_*, SPDK_ENV_NUMA_ID_ANY 등. */
#include "spdk/likely.h"
/* [한국어] spdk_likely/spdk_unlikely — 분기 예측 힌트 매크로. */
#include "spdk/queue.h"
/* [한국어] TAILQ_*/STAILQ_*/RB_* 매크로 (sys/queue.h 호환 + RB tree 확장). */
#include "spdk/util.h"
/* [한국어] SPDK_COUNTOF, offsetof 래퍼 등 유틸 매크로. */
#include "spdk/notify.h"
/* [한국어] spdk_notify_* — fsdev_register/unregister 이벤트를 가입자에게 비동기 통보하는 시스템. */
#include "spdk/fsdev_module.h"
/* [한국어] fsdev 백엔드 모듈이 구현해야 하는 콜백 시그니처와 spdk_fsdev_module 구조. */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG/NOTICELOG/DEBUGLOG 매크로. */
#include "spdk/string.h"
/* [한국어] spdk_sprintf_alloc 등 문자열 헬퍼. */
#include "fsdev_internal.h"
/* [한국어] 같은 라이브러리 내부 헤더 — fsdev_io_submit/fsdev_channel_get_io/__io_ch_to_fsdev_ch. */

#define SPDK_FSDEV_IO_POOL_SIZE (64 * 1024 - 1)
/* [한국어] 전역 fsdev_io 풀의 기본 슬롯 수. (64K - 1)은 DPDK ring buffer 크기로 가장 효율적인
 * 2^n - 1 패턴 — 슬롯 인덱싱 비트마스크가 단순해진다. 64K 슬롯이면 일반적인 multi-reactor 환경에서
 * 충분한 IO depth를 커버. */
#define SPDK_FSDEV_IO_CACHE_SIZE 256
/* [한국어] per-thread IO 캐시의 기본 크기 — 채널 생성 시 미리 풀에서 빼두는 슬롯 수.
 * 256 슬롯은 thread-local cache hit율을 높여 mempool global ring 접근을 줄임 (cmpxchg 회피). */

static struct spdk_fsdev_opts g_fsdev_opts = {
	/* [한국어] 전역 fsdev 코어 옵션 — RPC(fsdev_get/set_opts)와 spdk_fsdev_get/set_opts로 변경.
	 * 부팅 시점의 기본값으로 위 두 매크로가 사용됨. */
	.fsdev_io_pool_size = SPDK_FSDEV_IO_POOL_SIZE,
	/* [한국어] 글로벌 IO 풀 슬롯 수 — fsdev_io_pool 생성 시 사용. */
	.fsdev_io_cache_size = SPDK_FSDEV_IO_CACHE_SIZE,
	/* [한국어] per-thread cache 크기 — mgmt channel 생성 시 미리 빼두는 슬롯 수. */
};

TAILQ_HEAD(spdk_fsdev_list, spdk_fsdev);
/* [한국어] 등록된 fsdev 인스턴스의 TAILQ 타입 정의 — g_fsdev_mgr.fsdevs가 이 타입. */

RB_HEAD(fsdev_name_tree, spdk_fsdev_name);
/* [한국어] 이름 → fsdev 매핑을 빠르게 검색하기 위한 RB(red-black) tree 헤드 타입.
 * O(log n) lookup으로 fsdev 수가 늘어도 fsdev_get_by_name이 빠름. */

/*
 * [한국어]
 * fsdev_name_cmp - RB tree 비교 함수 — strcmp로 사전식 비교.
 *
 * @name1, @name2: 비교할 두 spdk_fsdev_name 노드(.name이 char*).
 * @return       : <0/0/>0 (strcmp 규약).
 *
 * RB_GENERATE_STATIC가 본 함수를 호출해 노드 배치를 결정.
 *
 * 호출 체인:
 *   RB_INSERT/RB_FIND → [fsdev_name_cmp] → strcmp
 */
static int
fsdev_name_cmp(struct spdk_fsdev_name *name1, struct spdk_fsdev_name *name2)
{
	return strcmp(name1->name, name2->name);
	/* [한국어] 이름 문자열 직접 비교 — 노드의 .name은 fsdev_name_add에서 strdup으로 채움. */
}

RB_GENERATE_STATIC(fsdev_name_tree, spdk_fsdev_name, node, fsdev_name_cmp);
/* [한국어] RB tree 매크로 인스턴스화 — RB_INSERT/RB_FIND/RB_REMOVE 등을 정적 함수로 생성.
 * 인자: tree-name, node-type, member-name(=node), cmp-fn. */

struct spdk_fsdev_mgr {
	/* [한국어] fsdev 코어의 단일 글로벌 매니저 인스턴스. spdk_io_device로도 등록되어
	 * per-thread mgmt 채널의 io_device 키 역할을 한다. */

	struct spdk_mempool *fsdev_io_pool;
	/* [한국어] 전역 fsdev_io 슬롯 풀 (DPDK rte_mempool 기반).
	 * 설정자: spdk_fsdev_initialize에서 spdk_mempool_create로 채움.
	 * 읽는 자: fsdev_channel_get_io(per-thread cache miss 시), spdk_fsdev_free_io.
	 * 값 범위: 유효한 mempool 핸들 또는 NULL(초기화 실패).
	 * 동기화: mempool 자체가 lock-free ring + per-core cache. */

	TAILQ_HEAD(fsdev_module_list, spdk_fsdev_module) fsdev_modules;
	/* [한국어] 등록된 fsdev 백엔드 모듈 리스트(예: aio_fs).
	 * 설정자: spdk_fsdev_module_list_add (보통 모듈의 SPDK_FSDEV_MODULE_REGISTER 매크로 → constructor).
	 * 읽는 자: 부팅(fsdev_modules_init), 종료(fsdev_module_fini_iter), config_json, get_max_ctx_size.
	 * 값 범위: 0개 이상의 spdk_fsdev_module 노드.
	 * 동기화: 부팅/종료 시점에만 변경(직렬) — 런타임 변경은 spinlock 보호 가정. */

	struct spdk_fsdev_list fsdevs;
	/* [한국어] 등록된 fsdev 인스턴스 TAILQ.
	 * 설정자: spdk_fsdev_register/unregister가 INSERT/REMOVE.
	 * 읽는 자: subsystem_config_json, fsdev_finish_unregister_fsdevs_iter.
	 * 값 범위: 0개 이상.
	 * 동기화: g_fsdev_mgr.spinlock으로 보호. */

	struct fsdev_name_tree fsdev_names;
	/* [한국어] 이름 → spdk_fsdev_name 매핑 RB tree — fsdev_get_by_name의 O(log n) lookup 백엔드.
	 * 설정자: fsdev_name_add (RB_INSERT), fsdev_name_del_unsafe (RB_REMOVE).
	 * 읽는 자: fsdev_get_by_name → RB_FIND.
	 * 동기화: g_fsdev_mgr.spinlock으로 보호. */

	bool init_complete;
	/* [한국어] spdk_fsdev_initialize가 콜백을 통보 완료했는지 flag.
	 * 설정자: fsdev_init_complete가 true, fsdev_mgr_unregister_cb가 false.
	 * 읽는 자: 진단/디버그 — 부팅 진행 단계 확인. */

	bool module_init_complete;
	/* [한국어] 모든 백엔드 모듈의 module_init이 끝났는지 flag.
	 * 설정자: spdk_fsdev_initialize 마지막에 true, fsdev_mgr_unregister_cb가 false.
	 * 읽는 자: fsdev_module_fini_iter가 false면 모듈 fini 스킵(불완전 초기화 정리). */

	struct spdk_spinlock spinlock;
	/* [한국어] 매니저 전역 spinlock — fsdevs/fsdev_names/모듈 리스트 접근 보호.
	 * SPDK spinlock은 spdk_thread 컨텍스트에서 안전한 lock 구현(reactor 정의된 sleep 회피).
	 * 설정자: _fsdev_init(constructor)가 init, 명시 destroy 없음(프로세스 종료까지 유지).
	 * 동기화: 본 락 자체. */
};

static struct spdk_fsdev_mgr g_fsdev_mgr = {
	/* [한국어] 단일 글로벌 매니저 인스턴스. 비-spinlock 필드는 컴파일 타임 정적 초기화. */
	.fsdev_modules = TAILQ_HEAD_INITIALIZER(g_fsdev_mgr.fsdev_modules),
	/* [한국어] 빈 모듈 리스트 — head가 자기 자신을 가리키도록. */
	.fsdevs = TAILQ_HEAD_INITIALIZER(g_fsdev_mgr.fsdevs),
	/* [한국어] 빈 fsdev 리스트. */
	.fsdev_names = RB_INITIALIZER(g_fsdev_mgr.fsdev_names),
	/* [한국어] 빈 RB tree. */
	.init_complete = false,
	/* [한국어] 부팅 전이므로 false. */
	.module_init_complete = false,
	/* [한국어] 동상. */
};

/*
 * [한국어]
 * _fsdev_init - 라이브러리 로드 시 매니저 spinlock 초기화.
 *
 * __attribute__((constructor))로 main 호출 전에 자동 실행. spinlock 정적 초기화는 SPDK API에
 * 일반적으로 불가능하므로 런타임 init이 필요 → constructor에서 1회 수행.
 *
 * 호출 체인:
 *   ld.so → __attribute__((constructor)) → [_fsdev_init] → spdk_spin_init
 */
static void
__attribute__((constructor))
_fsdev_init(void)
{
	spdk_spin_init(&g_fsdev_mgr.spinlock);
	/* [한국어] 매니저 spinlock 초기화 — 이후 모든 매니저 자료구조 접근에 사용. */
}


static spdk_fsdev_init_cb	g_init_cb_fn = NULL;
/* [한국어] spdk_fsdev_initialize 호출자가 등록한 완료 콜백 — 부팅 끝에 호출.
 * 설정자: spdk_fsdev_initialize, 읽는 자: fsdev_init_complete. 부팅 1회 한정. */
static void			*g_init_cb_arg = NULL;
/* [한국어] g_init_cb_fn에 전달할 사용자 ctx. */

static spdk_fsdev_fini_cb	g_fini_cb_fn = NULL;
/* [한국어] spdk_fsdev_finish 호출자가 등록한 완료 콜백 — 종료 끝에 호출. */
static void			*g_fini_cb_arg = NULL;
/* [한국어] g_fini_cb_fn에 전달할 사용자 ctx. */
static struct spdk_thread	*g_fini_thread = NULL;
/* [한국어] spdk_fsdev_finish를 호출한 thread 캡처 — 종료 콜백을 동일 thread로 라우팅하는 데 사용. */

struct spdk_fsdev_mgmt_channel {
	/* [한국어] per-SPDK-thread 관리 채널 컨텍스트. g_fsdev_mgr를 io_device로 등록할 때
	 * 채널 ctx로 사용된다. 한 thread당 하나, fsdev_io의 thread-local cache를 보유. */

	/*
	 * Each thread keeps a cache of fsdev_io - this allows
	 *  fsdev threads which are *not* DPDK threads to still
	 *  benefit from a per-thread fsdev_io cache.  Without
	 *  this, non-DPDK threads fetching from the mempool
	 *  incur a cmpxchg on get and put.
	 */
	fsdev_io_stailq_t per_thread_cache;
	/* [한국어] thread-local fsdev_io 캐시 STAILQ.
	 * 설정자: mgmt_channel_create(미리 cache_size만큼 채움), fsdev_channel_get_io(꺼내기), spdk_fsdev_free_io(반환).
	 * 읽는 자: 동상.
	 * 값 범위: 0..fsdev_io_cache_size 슬롯.
	 * 동기화: 채널은 thread 고정 — 별도 lock 없이 안전. cmpxchg 없는 단순 push/pop. */
	uint32_t	per_thread_cache_count;
	/* [한국어] per_thread_cache 현재 보유 슬롯 수. STAILQ는 기본적으로 길이 정보 없으므로 별도 카운터 유지.
	 * 설정자/읽는 자: 위와 동일. 동기화: thread 소유. */
	uint32_t	fsdev_io_cache_size;
	/* [한국어] 이 채널 한도(cache 최대치). 생성 시 g_fsdev_opts에서 복사 — 런타임 변경 안 함.
	 * 설정자: mgmt_channel_create. 동기화: immutable. */

	TAILQ_HEAD(, spdk_fsdev_shared_resource) shared_resources;
	/* [한국어] 같은 thread에서 동일 백엔드 io_device를 공유하는 fsdev들의 shared_resource 리스트.
	 * 설정자: fsdev_channel_create가 INSERT, fsdev_channel_destroy_resource가 REMOVE.
	 * 동기화: thread 소유. */
};

/*
 * Per-module (or per-io_device) data. Multiple fsdevs built on the same io_device
 * will queue here their IO that awaits retry. It makes it possible to retry sending
 * IO to one fsdev after IO from other fsdev completes.
 */
struct spdk_fsdev_shared_resource {
	/* [한국어] (백엔드 io_channel, thread) 단위로 공유되는 자원. 동일 백엔드(예: 한 디렉토리에 매핑된
	 * 여러 fsdev)는 같은 io_channel을 공유할 수 있고, 그 위에 ref-count로 라이프타임을 관리한다. */

	/* The fsdev management channel */
	struct spdk_fsdev_mgmt_channel *mgmt_ch;
	/* [한국어] 이 shared_resource를 보유한 mgmt 채널 포인터(역참조용).
	 * 설정자: fsdev_channel_create. 읽는 자: spdk_fsdev_free_io 등에서 mgmt_ch로 cache 접근.
	 * 동기화: 같은 thread 소유 → 안전. */

	/*
	 * Count of I/O submitted to fsdev module and waiting for completion.
	 * Incremented before submit_request() is called on an spdk_fsdev_io.
	 */
	uint64_t		io_outstanding;
	/* [한국어] 본 shared_resource 위로 발행되어 미완료 상태인 IO 개수.
	 * 설정자: fsdev_io_submit이 ++, spdk_fsdev_io_complete가 --.
	 * 읽는 자: ref==0일 때 0인지 검증 (assert).
	 * 동기화: thread 소유 — atomic 불필요. */

	/* I/O channel allocated by a fsdev module */
	struct spdk_io_channel	*shared_ch;
	/* [한국어] 백엔드 모듈이 발급한 io_channel — 같은 io_device를 쓰는 fsdev 채널들이 이 핸들을 공유.
	 * 설정자: fsdev_channel_create가 fsdev->fn_table->get_io_channel 결과로 채움.
	 * 읽는 자: 다른 fsdev 채널이 같은 io_channel 발견 시 이 shared_resource를 재사용(ref++). */

	uint32_t		ref;
	/* [한국어] 이 shared_resource를 사용 중인 fsdev_channel 개수.
	 * 0이 되면 mgmt_ch 리스트에서 제거 + free.
	 * 설정자/읽는 자: fsdev_channel_create/destroy_resource.
	 * 동기화: thread 소유. */

	TAILQ_ENTRY(spdk_fsdev_shared_resource) link;
	/* [한국어] mgmt_ch->shared_resources TAILQ 링크. */
};

struct spdk_fsdev_channel {
	/* [한국어] per-(fsdev, thread) IO 채널 컨텍스트. fsdev를 io_device로 등록할 때 채널 ctx 타입.
	 * 사용자 thread가 spdk_fsdev_get_io_channel을 호출하면 이 구조체가 thread별로 1개 생성된다. */

	struct spdk_fsdev	*fsdev;
	/* [한국어] 이 채널이 속한 fsdev 인스턴스.
	 * 설정자: fsdev_channel_create. 읽는 자: 채널 destroy 디버그/통계.
	 * 동기화: 채널 라이프타임 동안 immutable. */

	/* The channel for the underlying device */
	struct spdk_io_channel	*channel;
	/* [한국어] 백엔드 모듈(예: aio_fs)이 발급한 실제 IO 채널 — 본 채널의 IO submit 시 이 채널을 모듈에 전달.
	 * 설정자: fsdev_channel_create. 읽는 자: fsdev_io_submit. 동기화: thread 소유. */

	/* Per io_device per thread data */
	struct spdk_fsdev_shared_resource *shared_resource;
	/* [한국어] 같은 백엔드 io_channel을 쓰는 형제 fsdev 채널들과 공유하는 자원.
	 * 설정자: fsdev_channel_create. 동기화: thread 소유. */

	/*
	 * Count of I/O submitted to the underlying dev module through this channel
	 * and waiting for completion.
	 */
	uint64_t		io_outstanding;
	/* [한국어] 이 채널에서 발행되어 미완료인 IO 개수. 채널 destroy 시 0이어야 함.
	 * 설정자: fsdev_io_submit ++, spdk_fsdev_io_complete --.
	 * 동기화: thread 소유. */

	/*
	 * List of all submitted I/Os.
	 */
	fsdev_io_tailq_t	io_submitted;
	/* [한국어] 이 채널에서 발행되어 아직 완료되지 않은 IO 객체들의 TAILQ.
	 * 설정자: fsdev_io_submit이 INSERT_TAIL, fsdev_io_complete가 REMOVE.
	 * 읽는 자: 채널 destroy 시 비어있는지 assert, abort 경로 등.
	 * 동기화: thread 소유. */
};

struct spdk_fsdev_desc {
	/* [한국어] open이 반환하는 fsdev 디스크립터(=참조 핸들). 사용자 thread 소유.
	 * fsdev hot-remove 시 이벤트 콜백을 통보해 사용자가 close하도록 유도. */

	struct spdk_fsdev		*fsdev;
	/* [한국어] 가리키는 fsdev 인스턴스. open 후 close까지 immutable. */
	struct spdk_thread		*thread;
	/* [한국어] open을 호출한 thread — close/이벤트 콜백을 동일 thread로 라우팅.
	 * 설정자: fsdev_open. 읽는 자: spdk_fsdev_close(assert), _remove_notify(spdk_thread_send_msg). */
	struct {
		spdk_fsdev_event_cb_t event_fn;
		/* [한국어] hot-remove 등 이벤트 통보 콜백 — open 시 사용자가 등록. */
		void *ctx;
		/* [한국어] event_fn에 전달할 사용자 ctx. */
	}				callback;
	bool				closed;
	/* [한국어] spdk_fsdev_close가 진행 중/완료되었는지 flag.
	 * 설정자: fsdev_close. 읽는 자: _remove_notify가 in-flight 메시지와 close 간 race를 처리.
	 * 동기화: desc->spinlock. */
	struct spdk_spinlock		spinlock;
	/* [한국어] desc 내부 보호용 spinlock — closed/refs 동시 접근 직렬화. */
	uint32_t			refs;
	/* [한국어] in-flight remove notify 메시지 수 — 0이 되어야 desc free 가능.
	 * 설정자/읽는 자: fsdev_unregister_unsafe(++), _remove_notify(--), fsdev_close. */
	TAILQ_ENTRY(spdk_fsdev_desc)	link;
	/* [한국어] fsdev->internal.open_descs TAILQ 링크 — 한 fsdev에 열린 디스크립터들 추적. */
};

#define __fsdev_to_io_dev(fsdev)	(((char *)fsdev) + 1)
/* [한국어] fsdev 포인터를 spdk_io_device 키로 변환 — 의도적으로 +1로 시프트해
 * 같은 주소가 fsdev_mgr와 충돌하지 않게 함. spdk_io_device_register는 키만 식별하므로 안전. */
#define __fsdev_from_io_dev(io_dev)	((struct spdk_fsdev *)(((char *)io_dev) - 1))
/* [한국어] 위 변환의 역연산 — 콜백에서 io_device 키로부터 fsdev 복원. */
#define __io_ch_to_fsdev_mgmt_ch(io_ch)	((struct spdk_fsdev_mgmt_channel *)spdk_io_channel_get_ctx(io_ch))
/* [한국어] spdk_io_channel*에서 등록한 mgmt 채널 ctx로 캐스팅 — io_channel 객체 뒤에 ctx가 인라인. */

/*
 * [한국어]
 * fsdev_get_by_name - 이름으로 fsdev 인스턴스를 RB tree에서 O(log n) 검색.
 *
 * @fsdev_name: 찾을 이름.
 * @return    : spdk_fsdev* 또는 NULL.
 *
 * 호출 체인:
 *   spdk_fsdev_open → [fsdev_get_by_name] → RB_FIND
 */
static struct spdk_fsdev *
fsdev_get_by_name(const char *fsdev_name)
{
	struct spdk_fsdev_name find;
	/* [한국어] RB_FIND 키 — name 필드만 채워서 비교에 사용. */
	struct spdk_fsdev_name *res;
	/* [한국어] 검색 결과 노드 포인터. */

	find.name = (char *)fsdev_name;
	/* [한국어] const cast — 비교 함수에서 read-only로만 사용되므로 안전. */
	res = RB_FIND(fsdev_name_tree, &g_fsdev_mgr.fsdev_names, &find);
	/* [한국어] RB tree O(log n) 검색. */
	if (res != NULL) {
		return res->fsdev;
		/* [한국어] 매핑된 fsdev 반환. */
	}

	return NULL;
	/* [한국어] 미등록 이름. */
}

/*
 * [한국어]
 * fsdev_module_get_max_ctx_size - 등록된 모든 모듈의 get_ctx_size() 최댓값 반환.
 *
 * @return: 가장 큰 모듈 ctx 크기(0 이상).
 *
 * 모듈마다 fsdev_io 뒤에 모듈 전용 ctx를 인라인으로 두므로 풀 슬롯 크기를
 * sizeof(spdk_fsdev_io) + max_ctx_size 로 잡아야 모든 모듈을 수용할 수 있다.
 */
static int
fsdev_module_get_max_ctx_size(void)
{
	struct spdk_fsdev_module *fsdev_module;
	int max_fsdev_module_size = 0;
	/* [한국어] 누적 최댓값 누적기. */

	TAILQ_FOREACH(fsdev_module, &g_fsdev_mgr.fsdev_modules, internal.tailq) {
		/* [한국어] 모든 등록 모듈 순회 — 부팅 시점은 보통 직렬이므로 lock 없이도 안전. */
		if (fsdev_module->get_ctx_size && fsdev_module->get_ctx_size() > max_fsdev_module_size) {
			/* [한국어] 콜백을 제공한 모듈만 비교 — 미제공 모듈은 0으로 간주. */
			max_fsdev_module_size = fsdev_module->get_ctx_size();
		}
	}

	return max_fsdev_module_size;
}

/*
 * [한국어]
 * spdk_fsdev_subsystem_config_json - 현재 fsdev 서브시스템 설정을 JSON 배열로 출력.
 *
 * @w: spdk_json_write_ctx — config 저장(예: spdk_subsystem 보고서)에 쓰임.
 *
 * 출력 형식: 배열 안에 옵션 set RPC + 모듈 config + fsdev write_config_json들이 차례로 직렬화.
 * SPDK 부팅 시 config 파일을 재생산할 수 있도록 함.
 *
 * 호출 체인:
 *   서브시스템 config dumping → [spdk_fsdev_subsystem_config_json] → fsdev_module->config_json/write_config_json
 */
void
spdk_fsdev_subsystem_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_fsdev_module *fsdev_module;
	struct spdk_fsdev *fsdev;

	assert(w != NULL);
	/* [한국어] 호출 계약 — w 필수. */

	spdk_json_write_array_begin(w);
	/* [한국어] 전체 결과를 JSON 배열 '['로 시작 — 항목들을 순서대로 추가. */

	spdk_json_write_object_begin(w);
	/* [한국어] 첫 객체(=fsdev_set_opts RPC 호출 형태). */
	spdk_json_write_named_string(w, "method", "fsdev_set_opts");
	/* [한국어] "method": "fsdev_set_opts" — 사용자가 이 출력을 그대로 RPC로 재현 가능. */
	spdk_json_write_named_object_begin(w, "params");
	/* [한국어] "params": {  옵션 두 필드  }. */
	spdk_json_write_named_uint32(w, "fsdev_io_pool_size", g_fsdev_opts.fsdev_io_pool_size);
	/* [한국어] 현재 풀 크기 출력. */
	spdk_json_write_named_uint32(w, "fsdev_io_cache_size", g_fsdev_opts.fsdev_io_cache_size);
	/* [한국어] 현재 cache 크기 출력. */
	spdk_json_write_object_end(w); /* params */
	spdk_json_write_object_end(w);

	TAILQ_FOREACH(fsdev_module, &g_fsdev_mgr.fsdev_modules, internal.tailq) {
		/* [한국어] 각 모듈에 자체 config 출력 기회 부여. */
		if (fsdev_module->config_json) {
			fsdev_module->config_json(w);
			/* [한국어] 모듈이 제공한 콜백 호출 — 자기 형식의 RPC 호출 객체를 배열에 append. */
		}
	}

	spdk_spin_lock(&g_fsdev_mgr.spinlock);
	/* [한국어] fsdevs 리스트는 매니저 spinlock 보호. config dumping 도중 등록/해제와의 race 방지. */

	TAILQ_FOREACH(fsdev, &g_fsdev_mgr.fsdevs, internal.link) {
		/* [한국어] 등록된 모든 fsdev 인스턴스 순회. */
		if (fsdev->fn_table->write_config_json) {
			fsdev->fn_table->write_config_json(fsdev, w);
			/* [한국어] 모듈이 제공한 인스턴스 단위 config 출력. */
		}
	}

	spdk_spin_unlock(&g_fsdev_mgr.spinlock);
	spdk_json_write_array_end(w);
	/* [한국어] 배열 ']'로 종결. */
}

/*
 * [한국어]
 * fsdev_mgmt_channel_destroy - mgmt 채널 파괴 시 per-thread cache의 슬롯을 풀로 반환.
 *
 * @io_device: g_fsdev_mgr (사용 안 함 — io_device 콜백 시그니처 충족 목적).
 * @ctx_buf  : per-thread mgmt 채널 ctx 버퍼.
 *
 * spdk_io_device_unregister 또는 spdk_put_io_channel 시 SPDK가 호출.
 * 실행 컨텍스트: 채널 소유 thread.
 *
 * 호출 체인:
 *   spdk_put_io_channel(mgmt) → [fsdev_mgmt_channel_destroy] → spdk_mempool_put
 */
static void
fsdev_mgmt_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_fsdev_mgmt_channel *ch = ctx_buf;
	/* [한국어] io_channel 뒤에 인라인된 mgmt 채널 ctx로 캐스팅. */
	struct spdk_fsdev_io *fsdev_io;
	/* [한국어] 캐시에서 빼낼 슬롯 임시 변수. */

	if (!TAILQ_EMPTY(&ch->shared_resources)) {
		/* [한국어] 채널이 닫히는데 shared_resource가 남아 있으면 라이프사이클 버그.
		 * 정상 흐름이라면 fsdev_channel_destroy_resource가 모두 정리했어야 함. */
		SPDK_ERRLOG("Module channel list wasn't empty on mgmt channel free\n");
	}

	while (!STAILQ_EMPTY(&ch->per_thread_cache)) {
		/* [한국어] 캐시 비울 때까지 반복. */
		fsdev_io = STAILQ_FIRST(&ch->per_thread_cache);
		/* [한국어] head 슬롯을 꺼냄. */
		STAILQ_REMOVE_HEAD(&ch->per_thread_cache, internal.buf_link);
		/* [한국어] STAILQ에서 분리. */
		ch->per_thread_cache_count--;
		/* [한국어] 카운터 감소. */
		spdk_mempool_put(g_fsdev_mgr.fsdev_io_pool, (void *)fsdev_io);
		/* [한국어] 글로벌 풀로 슬롯 반환 — 다른 thread 채널에서 재사용 가능. */
	}

	assert(ch->per_thread_cache_count == 0);
	/* [한국어] 카운터-실 카운트 일치 확인. */
	return;
}

/*
 * [한국어]
 * fsdev_mgmt_channel_create - mgmt 채널 생성 시 per-thread cache를 미리 채워둠.
 *
 * @io_device: g_fsdev_mgr (사용 안 함).
 * @ctx_buf  : per-thread mgmt 채널 ctx 버퍼.
 * @return   : 0=성공, -1=풀 고갈로 cache 미채움.
 *
 * 부팅 직후 thread별로 처음 io_channel 획득 시 SPDK가 호출.
 * 실행 컨텍스트: 새로 mgmt 채널을 잡는 thread.
 *
 * 호출 체인:
 *   spdk_get_io_channel(mgmt) (미보유 thread) → [fsdev_mgmt_channel_create] → spdk_mempool_get
 */
static int
fsdev_mgmt_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_fsdev_mgmt_channel *ch = ctx_buf;
	/* [한국어] mgmt 채널 ctx로 캐스팅. */
	struct spdk_fsdev_io *fsdev_io;
	/* [한국어] 미리 빼낼 슬롯 임시. */
	uint32_t i;
	/* [한국어] cache 채움 루프 변수. */

	STAILQ_INIT(&ch->per_thread_cache);
	/* [한국어] cache STAILQ를 빈 상태로 초기화. */
	ch->fsdev_io_cache_size = g_fsdev_opts.fsdev_io_cache_size;
	/* [한국어] 글로벌 옵션 스냅샷 — 채널 라이프타임 동안 immutable. */

	/* Pre-populate fsdev_io cache to ensure this thread cannot be starved. */
	ch->per_thread_cache_count = 0;
	for (i = 0; i < ch->fsdev_io_cache_size; i++) {
		/* [한국어] cache_size만큼 미리 빼두어 starvation 방지(IO 시작 시 mempool global ring 접근 회피). */
		fsdev_io = spdk_mempool_get(g_fsdev_mgr.fsdev_io_pool);
		/* [한국어] 풀에서 슬롯 1개 가져옴. */
		if (fsdev_io == NULL) {
			/* [한국어] 풀 고갈 — 옵션 풀 크기를 키우라는 운영자 가이드 메시지. */
			SPDK_ERRLOG("You need to increase fsdev_io_pool_size using fsdev_set_options RPC.\n");
			assert(false);
			/* [한국어] 디버그 빌드 abort — 부팅 단계 실패는 즉시 노출하는 것이 안전. */
			fsdev_mgmt_channel_destroy(io_device, ctx_buf);
			/* [한국어] 그동안 빼낸 슬롯들을 풀로 반환해 누수 방지. */
			return -1;
		}
		ch->per_thread_cache_count++;
		/* [한국어] 카운터 ++. */
		STAILQ_INSERT_HEAD(&ch->per_thread_cache, fsdev_io, internal.buf_link);
		/* [한국어] cache HEAD에 push — LIFO 순서(가장 최근 것이 먼저 쓰여 캐시 라인 hot 유지). */
	}

	TAILQ_INIT(&ch->shared_resources);
	/* [한국어] 빈 shared_resources 리스트로 초기화. */
	return 0;
}

/*
 * [한국어]
 * fsdev_init_complete - 부팅 완료 시 사용자 콜백을 호출하고 글로벌 상태 갱신.
 *
 * @rc: 0=성공, 음수=실패.
 *
 * 호출 체인:
 *   spdk_fsdev_initialize / fsdev_init_failed → [fsdev_init_complete] → 사용자 init 콜백
 */
static void
fsdev_init_complete(int rc)
{
	spdk_fsdev_init_cb cb_fn = g_init_cb_fn;
	/* [한국어] 콜백 로컬 백업 — 콜백 안에서 새로운 init 호출 시 g_init_cb_fn 재사용 안전. */
	void *cb_arg = g_init_cb_arg;

	g_fsdev_mgr.init_complete = true;
	/* [한국어] 부팅 단계 완료 표시. */
	g_init_cb_fn = NULL;
	/* [한국어] 글로벌 슬롯 비움 — 다음 init 호출 시 재사용. */
	g_init_cb_arg = NULL;

	cb_fn(cb_arg, rc);
	/* [한국어] 사용자 init 콜백 호출. cb_fn은 호출자 thread에서 실행. */
}

/*
 * [한국어]
 * fsdev_init_failed - 비동기 컨텍스트에서 부팅 실패를 통보하기 위한 thunk.
 *
 * @cb_arg: 사용 안 함(spdk_thread_send_msg 시그니처 충족).
 *
 * 호출 체인:
 *   fsdev_modules_init 실패 → spdk_thread_send_msg(... fsdev_init_failed ...) → [fsdev_init_failed] → fsdev_init_complete(-1)
 */
static void
fsdev_init_failed(void *cb_arg)
{
	fsdev_init_complete(-1);
	/* [한국어] -1 코드로 실패 통보. */
}

/*
 * [한국어]
 * fsdev_modules_init - 등록된 모든 모듈의 module_init 콜백을 순차 호출.
 *
 * @return: 0=모두 성공, 첫 실패 코드.
 *
 * 첫 실패 시 즉시 중단하고 비동기로 fsdev_init_failed를 send_msg.
 *
 * 호출 체인:
 *   spdk_fsdev_initialize → [fsdev_modules_init] → module->module_init
 */
static int
fsdev_modules_init(void)
{
	struct spdk_fsdev_module *module;
	int rc = 0;

	TAILQ_FOREACH(module, &g_fsdev_mgr.fsdev_modules, internal.tailq) {
		/* [한국어] 등록 순서대로 module_init 호출. */
		rc = module->module_init();
		/* [한국어] 모듈별 초기화 — 일반적으로 동기, 일부는 비동기 처리 가능. */
		if (rc != 0) {
			/* [한국어] 한 모듈이라도 실패 시 부팅 전체 실패. 비동기 컨텍스트로 통보. */
			spdk_thread_send_msg(spdk_get_thread(), fsdev_init_failed, module);
			return rc;
		}
	}

	return 0;
}

/*
 * [한국어]
 * spdk_fsdev_initialize - fsdev 서브시스템 부팅 — 풀 생성 + 모듈 init + io_device 등록.
 *
 * @cb_fn : 부팅 완료 통보 콜백 (필수).
 * @cb_arg: 콜백에 전달할 ctx.
 *
 * 비동기 종료 시점에 cb_fn(cb_arg, 0/-1) 호출.
 * 실행 컨텍스트: 마스터 thread / SPDK app 시작 시퀀스.
 *
 * 호출 체인:
 *   spdk_subsystem init → [spdk_fsdev_initialize] → spdk_mempool_create → spdk_io_device_register → fsdev_modules_init → fsdev_init_complete
 */
void
spdk_fsdev_initialize(spdk_fsdev_init_cb cb_fn, void *cb_arg)
{
	int rc = 0;
	char mempool_name[32];
	/* [한국어] 풀 이름 합성 임시 — 동일 호스트에서 여러 SPDK 프로세스 공존 시 PID로 식별. */

	assert(cb_fn != NULL);
	/* [한국어] 호출 계약 — 콜백 필수. */

	g_init_cb_fn = cb_fn;
	g_init_cb_arg = cb_arg;
	/* [한국어] 글로벌 슬롯에 보관 — fsdev_init_complete가 사용. */

	spdk_notify_type_register("fsdev_register");
	/* [한국어] 알림 타입 등록 — 외부 가입자가 fsdev 등록 이벤트를 구독 가능. */
	spdk_notify_type_register("fsdev_unregister");

	snprintf(mempool_name, sizeof(mempool_name), "fsdev_io_%d", getpid());
	/* [한국어] "fsdev_io_<PID>" 형태로 풀 이름 합성 — DPDK는 동일 이름 풀이 두 번 못 만들어지므로 PID 포함. */

	g_fsdev_mgr.fsdev_io_pool = spdk_mempool_create(mempool_name,
				    g_fsdev_opts.fsdev_io_pool_size,
				    sizeof(struct spdk_fsdev_io) +
				    fsdev_module_get_max_ctx_size(),
				    0,
				    SPDK_ENV_NUMA_ID_ANY);
	/* [한국어] 글로벌 풀 생성 — 슬롯 크기 = fsdev_io + 가장 큰 모듈 ctx (모든 모듈을 수용).
	 * cache_size=0 — DPDK rte_mempool 자체 cache는 끄고 위 fsdev mgmt 채널 cache로 대체.
	 * NUMA_ID_ANY — DPDK가 적절한 노드 선택. */

	if (g_fsdev_mgr.fsdev_io_pool == NULL) {
		/* [한국어] hugepage/메모리 부족 — 부팅 실패. */
		SPDK_ERRLOG("Could not allocate spdk_fsdev_io pool\n");
		fsdev_init_complete(-1);
		return;
	}

	spdk_io_device_register(&g_fsdev_mgr, fsdev_mgmt_channel_create,
				fsdev_mgmt_channel_destroy,
				sizeof(struct spdk_fsdev_mgmt_channel),
				"fsdev_mgr");
	/* [한국어] 매니저 자체를 io_device로 등록 — thread별 mgmt 채널 ctx를 SPDK가 관리. */

	rc = fsdev_modules_init();
	/* [한국어] 모듈 init 트리거 — 실패 시 fsdev_init_failed가 비동기 send_msg로 -1 통보 후 본 함수는 return. */
	g_fsdev_mgr.module_init_complete = true;
	/* [한국어] 모듈 init 단계 완료 마킹. 실패해도 true (cleanup 경로 분기 단서). */
	if (rc != 0) {
		/* [한국어] 동기 실패 코드 잡혔으면 여기서 종료. fsdev_init_failed가 콜백 처리. */
		SPDK_ERRLOG("fsdev modules init failed\n");
		return;
	}

	fsdev_init_complete(0);
	/* [한국어] 정상 부팅 완료 통보. */
}

/*
 * [한국어]
 * fsdev_mgr_unregister_cb - 매니저 io_device unregister 콜백 — 풀 free + 사용자 fini 콜백 호출.
 *
 * @io_device: 사용 안 함.
 *
 * spdk_io_device_unregister가 모든 thread에서 mgmt 채널 destroy를 마치면 호출. 풀에 슬롯이 모두
 * 반환된 상태여야 정상 — 누수 시 ERRLOG.
 */
static void
fsdev_mgr_unregister_cb(void *io_device)
{
	spdk_fsdev_fini_cb cb_fn = g_fini_cb_fn;
	/* [한국어] fini 콜백 로컬 백업. */

	if (g_fsdev_mgr.fsdev_io_pool) {
		if (spdk_mempool_count(g_fsdev_mgr.fsdev_io_pool) != g_fsdev_opts.fsdev_io_pool_size) {
			/* [한국어] 풀 카운트가 원래 크기와 다르면 IO 슬롯 누수 — 운영자 가시성. */
			SPDK_ERRLOG("fsdev IO pool count is %zu but should be %u\n",
				    spdk_mempool_count(g_fsdev_mgr.fsdev_io_pool),
				    g_fsdev_opts.fsdev_io_pool_size);
		}

		spdk_mempool_free(g_fsdev_mgr.fsdev_io_pool);
		/* [한국어] 풀 해제 — hugepage 영역 반환. */
	}

	cb_fn(g_fini_cb_arg);
	/* [한국어] 사용자 fini 콜백 호출. */
	g_fini_cb_fn = NULL;
	g_fini_cb_arg = NULL;
	/* [한국어] 글로벌 슬롯 비움. */
	g_fsdev_mgr.init_complete = false;
	g_fsdev_mgr.module_init_complete = false;
	/* [한국어] 부팅 플래그 리셋 — 재부팅 가능. */
}

/*
 * [한국어]
 * fsdev_module_fini_iter - 등록된 모듈을 역순으로 module_fini 호출 (마지막 등록부터 해제).
 *
 * @arg: 사용 안 함(send_msg 시그니처 충족).
 *
 * 모든 모듈 fini 후 spdk_io_device_unregister(매니저)로 풀 정리/사용자 콜백 트리거.
 */
static void
fsdev_module_fini_iter(void *arg)
{
	struct spdk_fsdev_module *fsdev_module;

	/* FIXME: Handling initialization failures is broken now,
	 * so we won't even try cleaning up after successfully
	 * initialized modules. if module_init_complete is false,
	 * just call spdk_fsdev_mgr_unregister_cb
	 */
	if (!g_fsdev_mgr.module_init_complete) {
		/* [한국어] 모듈 init이 끝나지 않은 상태에서의 종료 — 안전을 위해 fini 스킵하고 곧장 cleanup 콜백.
		 * (FIXME: 부분 초기화 정리 구현이 미흡하다는 영어 주석 그대로 — 보강 시 본 분기 정교화 필요.) */
		fsdev_mgr_unregister_cb(NULL);
		return;
	}

	/* Start iterating from the last touched module */
	fsdev_module = TAILQ_LAST(&g_fsdev_mgr.fsdev_modules, fsdev_module_list);
	/* [한국어] 등록 역순으로 fini — 마지막에 init된 모듈부터 해제(LIFO). */
	while (fsdev_module) {
		if (fsdev_module->module_fini) {
			fsdev_module->module_fini();
			/* [한국어] 모듈별 정리 콜백 호출. */
		}

		fsdev_module = TAILQ_PREV(fsdev_module, fsdev_module_list,
					  internal.tailq);
		/* [한국어] 한 칸 앞으로 — 처음에 도달하면 PREV가 NULL. */
	}

	spdk_io_device_unregister(&g_fsdev_mgr, fsdev_mgr_unregister_cb);
	/* [한국어] 매니저 io_device 해제 — 모든 thread mgmt 채널 destroy 후 콜백 발화. */
}

/*
 * [한국어]
 * fsdev_finish_unregister_fsdevs_iter - fsdev 인스턴스를 한 개씩 unregister하며 진행.
 *
 * @cb_arg     : 직전 unregister 대상 fsdev (또는 NULL — 첫 호출).
 * @fsdeverrno : 직전 unregister 결과(0=성공, 음수=에러).
 *
 * 비동기 콜백 체인 — 한 fsdev이 정리되면 다음 fsdev로 진행, 더 없으면 fsdev_module_fini_iter 단계로.
 */
static void
fsdev_finish_unregister_fsdevs_iter(void *cb_arg, int fsdeverrno)
{
	struct spdk_fsdev *fsdev = cb_arg;

	if (fsdeverrno && fsdev) {
		/* [한국어] 직전 unregister가 실패한 경우 — 일관성 위해 강제로 리스트에서 빼고 다음 fsdev로 진행.
		 * 실패한 fsdev은 free되지 않으므로 잠재적 누수이지만 진행 자체는 멈추지 않음. */
		SPDK_WARNLOG("Unable to unregister fsdev '%s' during spdk_fsdev_finish()\n",
			     fsdev->name);

		/*
		 * Since the call to spdk_fsdev_unregister() failed, we have no way to free this
		 *  fsdev; try to continue by manually removing this fsdev from the list and continue
		 *  with the next fsdev in the list.
		 */
		TAILQ_REMOVE(&g_fsdev_mgr.fsdevs, fsdev, internal.link);
	}

	fsdev = TAILQ_FIRST(&g_fsdev_mgr.fsdevs);
	/* [한국어] 다음(=리스트의 첫) fsdev 가져옴. */
	if (!fsdev) {
		/* [한국어] 모든 fsdev 정리 완료 → 모듈 fini 단계로 이동. */
		SPDK_DEBUGLOG(fsdev, "Done unregistering fsdevs\n");
		/*
		 * Fsdev module finish need to be deferred as we might be in the middle of some context
		 * that will use this fsdev (or private fsdev driver ctx data)
		 * after returning.
		 */
		spdk_thread_send_msg(spdk_get_thread(), fsdev_module_fini_iter, NULL);
		/* [한국어] 동기 호출 시 콜백이 fsdev 자료구조를 아직 참조 중일 수 있어 send_msg로 deferred 실행. */
		return;
	}

	SPDK_DEBUGLOG(fsdev, "Unregistering fsdev '%s'\n", fsdev->name);
	spdk_fsdev_unregister(fsdev, fsdev_finish_unregister_fsdevs_iter, fsdev);
	/* [한국어] 다음 fsdev unregister 트리거 — 완료 시 본 함수가 다시 호출되어 체인 진행. */
	return;
}

/*
 * [한국어]
 * spdk_fsdev_finish - fsdev 서브시스템 종료 — 모든 fsdev unregister + 모듈 fini + 풀 free.
 *
 * @cb_fn : 종료 완료 콜백 (필수).
 * @cb_arg: ctx.
 *
 * 비동기 — 호출 후 즉시 반환, 콜백은 모든 정리 완료 시점에 호출 thread로 라우팅.
 *
 * 호출 체인:
 *   spdk_subsystem fini → [spdk_fsdev_finish] → fsdev_finish_unregister_fsdevs_iter → fsdev_module_fini_iter → spdk_io_device_unregister → fsdev_mgr_unregister_cb
 */
void
spdk_fsdev_finish(spdk_fsdev_fini_cb cb_fn, void *cb_arg)
{
	assert(cb_fn != NULL);
	g_fini_thread = spdk_get_thread();
	/* [한국어] 호출 thread 캡처 — 종료 콜백 라우팅에 사용. */
	g_fini_cb_fn = cb_fn;
	g_fini_cb_arg = cb_arg;
	fsdev_finish_unregister_fsdevs_iter(NULL, 0);
	/* [한국어] 첫 fsdev unregister부터 시작 — NULL/0은 "직전 결과 없음" 의미. */
}

/*
 * [한국어]
 * fsdev_channel_get_io - per-thread cache 우선 → 글로벌 풀 fallback으로 fsdev_io 1개 반환.
 *
 * @channel: 호출 thread 소유 fsdev 채널.
 * @return : fsdev_io* 또는 NULL (풀 고갈).
 *
 * Hot path — cache hit 시 lock-free single-thread STAILQ pop. Miss 시 mempool ring 접근.
 *
 * 호출 체인:
 *   공개 fsdev IO API → [fsdev_channel_get_io] → spdk_mempool_get
 */
struct spdk_fsdev_io *
fsdev_channel_get_io(struct spdk_fsdev_channel *channel)
{
	struct spdk_fsdev_mgmt_channel *ch = channel->shared_resource->mgmt_ch;
	/* [한국어] 같은 thread의 mgmt 채널로 진입 — cache 보유자. */
	struct spdk_fsdev_io *fsdev_io;

	if (ch->per_thread_cache_count > 0) {
		/* [한국어] cache hit — 무경합 단일 thread 단위 pop. */
		fsdev_io = STAILQ_FIRST(&ch->per_thread_cache);
		STAILQ_REMOVE_HEAD(&ch->per_thread_cache, internal.buf_link);
		ch->per_thread_cache_count--;
	} else {
		/* [한국어] cache miss — 글로벌 mempool에서 1개 가져옴(ring + per-core cache 경유). */
		fsdev_io = spdk_mempool_get(g_fsdev_mgr.fsdev_io_pool);
	}

	return fsdev_io;
}

/*
 * [한국어]
 * spdk_fsdev_free_io - fsdev_io를 thread cache로 반환(여유 있으면) 또는 글로벌 풀로 반환.
 *
 * @fsdev_io: 사용 끝난 IO 객체.
 *
 * IO 완료 콜백 후 호출 — Hot path. cache 가득 차면 글로벌 풀로 overflow.
 *
 * 호출 체인:
 *   사용자 IO 완료 cb_fn → [spdk_fsdev_free_io] → spdk_mempool_put / STAILQ_INSERT_HEAD
 */
void
spdk_fsdev_free_io(struct spdk_fsdev_io *fsdev_io)
{
	struct spdk_fsdev_mgmt_channel *ch;

	assert(fsdev_io != NULL);
	/* [한국어] NULL 인자는 호출 버그. */

	ch = fsdev_io->internal.ch->shared_resource->mgmt_ch;
	/* [한국어] 이 IO의 thread mgmt 채널 진입. */

	if (ch->per_thread_cache_count < ch->fsdev_io_cache_size) {
		/* [한국어] cache 여유 → 다시 cache로 push (LIFO로 hot 캐시라인 유지). */
		ch->per_thread_cache_count++;
		STAILQ_INSERT_HEAD(&ch->per_thread_cache, fsdev_io, internal.buf_link);
	} else {
		/* [한국어] cache 가득 → 글로벌 풀로 반환. */
		spdk_mempool_put(g_fsdev_mgr.fsdev_io_pool, (void *)fsdev_io);
	}
}

/*
 * [한국어]
 * fsdev_io_submit - 채워진 fsdev_io를 백엔드 모듈로 디스패치.
 *
 * @fsdev_io: 사용자가 모든 인자(타입/args/cb_fn/cb_arg)를 채워서 넘긴 IO 객체.
 *
 * io_outstanding++ → submitted 큐 enqueue → 모듈 submit_request 호출.
 * in_submit_request 플래그를 켰다 끄는 이유: 모듈이 동기 완료(즉시 spdk_fsdev_io_complete 호출)하는 경우
 * fsdev_io_complete가 send_msg로 deferred 처리해 무한 재귀(submit→complete→submit→…)를 방지하기 위함.
 *
 * 실행 컨텍스트: 채널 소유 thread.
 *
 * 호출 체인:
 *   공개 spdk_fsdev_* API → [fsdev_io_submit] → fsdev->fn_table->submit_request → 모듈 처리
 */
void
fsdev_io_submit(struct spdk_fsdev_io *fsdev_io)
{
	struct spdk_fsdev *fsdev = fsdev_io->fsdev;
	struct spdk_fsdev_channel *ch = fsdev_io->internal.ch;
	struct spdk_fsdev_shared_resource *shared_resource = ch->shared_resource;

	TAILQ_INSERT_TAIL(&ch->io_submitted, fsdev_io, internal.ch_link);
	/* [한국어] 채널의 in-flight 큐 끝에 추가 — 추적/abort 경로에서 사용. */

	ch->io_outstanding++;
	/* [한국어] 채널 단위 미완료 IO 카운트 증가. */
	shared_resource->io_outstanding++;
	/* [한국어] (io_device, thread) 공유 단위 미완료 카운트 증가. */
	fsdev_io->internal.in_submit_request = true;
	/* [한국어] 동기 완료 시 fsdev_io_complete가 deferred 모드로 동작하도록 표시. */
	fsdev->fn_table->submit_request(ch->channel, fsdev_io);
	/* [한국어] 모듈에 위임 — 비동기 시작이 일반적. 동기 완료 시 내부 in_submit_request로 deferred. */
	fsdev_io->internal.in_submit_request = false;
	/* [한국어] submit_request 반환 후 플래그 해제. 비동기 완료 케이스라면 이 시점에 IO는 in-flight. */
}

/*
 * [한국어]
 * fsdev_channel_destroy_resource - 채널의 백엔드 io_channel 반환 + shared_resource refcount 처리.
 *
 * @ch: 파괴 중인 fsdev 채널.
 *
 * ref==0이면 mgmt_ch 리스트에서 제거 + free + mgmt 채널의 io_channel도 put.
 */
static void
fsdev_channel_destroy_resource(struct spdk_fsdev_channel *ch)
{
	struct spdk_fsdev_shared_resource *shared_resource;

	spdk_put_io_channel(ch->channel);
	/* [한국어] 백엔드 io_channel 사용 종료 통보 — 마지막 reference면 모듈이 해제. */

	shared_resource = ch->shared_resource;

	assert(TAILQ_EMPTY(&ch->io_submitted));
	/* [한국어] 채널 destroy 시점엔 모든 IO가 완료되어 있어야 함. */
	assert(ch->io_outstanding == 0);
	/* [한국어] 카운터도 0이어야 함. */
	assert(shared_resource->ref > 0);
	/* [한국어] 본 채널이 ref 1을 보유 중. */
	shared_resource->ref--;
	if (shared_resource->ref == 0) {
		/* [한국어] 마지막 fsdev 채널이 떠나는 경우 — shared_resource 자체 정리. */
		assert(shared_resource->io_outstanding == 0);
		TAILQ_REMOVE(&shared_resource->mgmt_ch->shared_resources, shared_resource, link);
		spdk_put_io_channel(spdk_io_channel_from_ctx(shared_resource->mgmt_ch));
		/* [한국어] mgmt 채널 reference 한 개 반환 — 본 채널 생성 시 잡았던 짝. */
		free(shared_resource);
	}
}

/*
 * [한국어]
 * fsdev_desc_free - 디스크립터 spinlock 파괴 + 메모리 free.
 *
 * @desc: 더 이상 참조 없는 디스크립터.
 */
static void
fsdev_desc_free(struct spdk_fsdev_desc *desc)
{
	spdk_spin_destroy(&desc->spinlock);
	/* [한국어] spinlock 파괴 — alloc 시 init한 짝. */
	free(desc);
}


/*
 * [한국어]
 * fsdev_channel_create - fsdev IO 채널 생성 콜백.
 *
 * @io_device: __fsdev_to_io_dev로 인코딩된 fsdev 키.
 * @ctx_buf  : 새로 만들어진 fsdev_channel ctx.
 * @return   : 0=성공, -1=실패.
 *
 * 모듈 io_channel 발급 + mgmt 채널 reference 획득 + 같은 io_channel 공유하는 형제 발견 시 ref++.
 * 실행 컨텍스트: 새 채널을 잡는 thread.
 *
 * 호출 체인:
 *   spdk_get_io_channel(fsdev) → SPDK 채널 매니저 → [fsdev_channel_create]
 */
static int
fsdev_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_fsdev		*fsdev = __fsdev_from_io_dev(io_device);
	/* [한국어] 인코딩된 키 복원. */
	struct spdk_fsdev_channel	*ch = ctx_buf;
	struct spdk_io_channel		*mgmt_io_ch;
	struct spdk_fsdev_mgmt_channel	*mgmt_ch;
	struct spdk_fsdev_shared_resource *shared_resource;

	ch->fsdev = fsdev;
	ch->channel = fsdev->fn_table->get_io_channel(fsdev->ctxt);
	/* [한국어] 모듈에 백엔드 io_channel 발급 요청 — 모듈이 자체 thread-local ctx 준비. */
	if (!ch->channel) {
		return -1;
	}

	mgmt_io_ch = spdk_get_io_channel(&g_fsdev_mgr);
	/* [한국어] 같은 thread의 mgmt 채널 reference 획득 — IO 풀 cache 사용 채널. */
	if (!mgmt_io_ch) {
		spdk_put_io_channel(ch->channel);
		return -1;
	}

	mgmt_ch = __io_ch_to_fsdev_mgmt_ch(mgmt_io_ch);
	/* [한국어] mgmt ctx로 캐스팅. */
	TAILQ_FOREACH(shared_resource, &mgmt_ch->shared_resources, link) {
		/* [한국어] 같은 thread의 같은 백엔드 io_channel을 쓰는 형제 fsdev이 이미 있는지 검색. */
		if (shared_resource->shared_ch == ch->channel) {
			/* [한국어] 발견 — 새로 만들지 않고 ref++로 공유. */
			spdk_put_io_channel(mgmt_io_ch);
			/* [한국어] 위에서 잡은 mgmt reference는 형제가 이미 1개 잡고 있으므로 즉시 반환. */
			shared_resource->ref++;
			break;
		}
	}

	if (shared_resource == NULL) {
		/* [한국어] 형제 없음 — 새 shared_resource 생성. */
		shared_resource = calloc(1, sizeof(*shared_resource));
		if (shared_resource == NULL) {
			spdk_put_io_channel(ch->channel);
			spdk_put_io_channel(mgmt_io_ch);
			return -1;
		}

		shared_resource->mgmt_ch = mgmt_ch;
		shared_resource->io_outstanding = 0;
		shared_resource->shared_ch = ch->channel;
		shared_resource->ref = 1;
		TAILQ_INSERT_TAIL(&mgmt_ch->shared_resources, shared_resource, link);
		/* [한국어] mgmt 채널의 형제 리스트에 등록. */
	}

	ch->io_outstanding = 0;
	ch->shared_resource = shared_resource;
	TAILQ_INIT(&ch->io_submitted);
	/* [한국어] in-flight IO 추적 큐 빈 상태로. */
	return 0;
}

/*
 * [한국어]
 * fsdev_channel_destroy - fsdev IO 채널 파괴 콜백.
 *
 * @io_device: 사용 안 함.
 * @ctx_buf  : 파괴 중인 fsdev_channel ctx.
 *
 * spdk_put_io_channel 마지막 reference 시 SPDK가 호출.
 */
static void
fsdev_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_fsdev_channel *ch = ctx_buf;

	SPDK_DEBUGLOG(fsdev, "Destroying channel %p for fsdev %s on thread %p\n",
		      ch, ch->fsdev->name,
		      spdk_get_thread());
	fsdev_channel_destroy_resource(ch);
	/* [한국어] 백엔드 io_channel + shared_resource 정리. */
}

/*
 * If the name already exists in the global fsdev name tree, RB_INSERT() returns a pointer
 * to it. Hence we do not have to call fsdev_get_by_name() when using this function.
 */
/*
 * [한국어]
 * fsdev_name_add - 이름 노드를 RB tree에 등록.
 *
 * @fsdev_name: 임베드된 spdk_fsdev_name 노드.
 * @fsdev    : 매핑 대상 fsdev.
 * @name     : 이름 문자열(strdup으로 복사됨).
 * @return   : 0=성공, -ENOMEM, -EEXIST(중복).
 *
 * RB_INSERT가 중복 노드 반환 시 -EEXIST. 별도 fsdev_get_by_name 검사 불필요.
 */
static int
fsdev_name_add(struct spdk_fsdev_name *fsdev_name, struct spdk_fsdev *fsdev, const char *name)
{
	struct spdk_fsdev_name *tmp;

	fsdev_name->name = strdup(name);
	/* [한국어] 이름을 자체 소유 버퍼로 복사 — fsdev_name_del_unsafe에서 free. */
	if (fsdev_name->name == NULL) {
		SPDK_ERRLOG("Unable to allocate fsdev name\n");
		return -ENOMEM;
	}

	fsdev_name->fsdev = fsdev;

	spdk_spin_lock(&g_fsdev_mgr.spinlock);
	/* [한국어] RB tree 갱신은 매니저 spinlock 보호 하에. */
	tmp = RB_INSERT(fsdev_name_tree, &g_fsdev_mgr.fsdev_names, fsdev_name);
	spdk_spin_unlock(&g_fsdev_mgr.spinlock);
	if (tmp != NULL) {
		/* [한국어] 동일 이름이 이미 존재 — 충돌. */
		SPDK_ERRLOG("Fsdev name %s already exists\n", name);
		free(fsdev_name->name);
		return -EEXIST;
	}

	return 0;
}

/*
 * [한국어]
 * fsdev_name_del_unsafe - 이름 노드를 RB tree에서 제거 (호출자가 매니저 spinlock 보유 가정).
 *
 * @fsdev_name: 제거할 노드.
 *
 * "_unsafe" 접미사: lock을 호출자가 이미 잡고 있어야 함.
 */
static void
fsdev_name_del_unsafe(struct spdk_fsdev_name *fsdev_name)
{
	RB_REMOVE(fsdev_name_tree, &g_fsdev_mgr.fsdev_names, fsdev_name);
	/* [한국어] tree에서 분리. */
	free(fsdev_name->name);
	/* [한국어] strdup된 이름 해제. */
}

/*
 * [한국어]
 * spdk_fsdev_get_io_channel - 디스크립터로부터 IO 채널을 획득.
 *
 * @desc  : open이 반환한 디스크립터.
 * @return: spdk_io_channel* (실제 ctx는 spdk_fsdev_channel).
 *
 * 호출 체인:
 *   사용자 → [spdk_fsdev_get_io_channel] → spdk_get_io_channel → fsdev_channel_create
 */
struct spdk_io_channel *
spdk_fsdev_get_io_channel(struct spdk_fsdev_desc *desc)
{
	return spdk_get_io_channel(__fsdev_to_io_dev(spdk_fsdev_desc_get_fsdev(desc)));
	/* [한국어] desc → fsdev → io_device 키 → io_channel. */
}

/*
 * [한국어]
 * spdk_fsdev_set_opts - 글로벌 옵션 갱신 (RPC 백엔드).
 *
 * @opts  : 사용자가 채운 옵션. opts_size가 0이면 거부.
 * @return: 0=성공, -EINVAL.
 *
 * pool_size가 thread별 cache_size 합보다 작으면 부팅 시 풀 고갈 가능 → 거부. SET_FIELD 매크로로
 * ABI 호환(미래 필드 추가 시 작은 opts_size로 호출되어도 안전) 보장.
 */
int
spdk_fsdev_set_opts(const struct spdk_fsdev_opts *opts)
{
	uint32_t min_pool_size;

	if (!opts) {
		SPDK_ERRLOG("opts cannot be NULL\n");
		return -EINVAL;
	}

	if (!opts->opts_size) {
		/* [한국어] opts_size=0은 ABI 식별 불가 — 거부. */
		SPDK_ERRLOG("opts_size inside opts cannot be zero value\n");
		return -EINVAL;
	}

	/*
	 * Add 1 to the thread count to account for the extra mgmt_ch that gets created during subsystem
	 *  initialization.  A second mgmt_ch will be created on the same thread when the application starts
	 *  but before the deferred put_io_channel event is executed for the first mgmt_ch.
	 */
	min_pool_size = opts->fsdev_io_cache_size * (spdk_thread_get_count() + 1);
	/* [한국어] 모든 thread가 cache를 가득 채울 때 필요한 슬롯 합계. +1은 부팅 단계의 임시 추가 mgmt 채널 보정. */
	if (opts->fsdev_io_pool_size < min_pool_size) {
		/* [한국어] 풀이 너무 작아 starvation 가능 — 명시 거부 + 권장값 가이드. */
		SPDK_ERRLOG("fsdev_io_pool_size %" PRIu32 " is not compatible with bdev_io_cache_size %" PRIu32
			    " and %" PRIu32 " threads\n", opts->fsdev_io_pool_size, opts->fsdev_io_cache_size,
			    spdk_thread_get_count());
		SPDK_ERRLOG("fsdev_io_pool_size must be at least %" PRIu32 "\n", min_pool_size);
		return -EINVAL;
	}

#define SET_FIELD(field) \
        if (offsetof(struct spdk_fsdev_opts, field) + sizeof(opts->field) <= opts->opts_size) { \
                g_fsdev_opts.field = opts->field; \
        } \
	/* [한국어] 사용자 opts_size가 해당 필드까지 포함하는 경우에만 복사 — 옛 ABI 호출자 호환. */

	SET_FIELD(fsdev_io_pool_size);
	/* [한국어] 풀 크기 복사 (조건부). */
	SET_FIELD(fsdev_io_cache_size);
	/* [한국어] 캐시 크기 복사 (조건부). */

	g_fsdev_opts.opts_size = opts->opts_size;
	/* [한국어] 사용자 ABI 버전 기록. */

#undef SET_FIELD

	return 0;
}

/*
 * [한국어]
 * spdk_fsdev_get_opts - 현재 글로벌 옵션을 사용자 버퍼로 복사.
 *
 * @opts     : 출력 버퍼.
 * @opts_size: 사용자 버퍼 크기 (ABI 호환 키).
 * @return   : 0=성공, -EINVAL.
 */
int
spdk_fsdev_get_opts(struct spdk_fsdev_opts *opts, size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return -EINVAL;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return -EINVAL;
	}

	opts->opts_size = opts_size;
	/* [한국어] 사용자가 인지하는 ABI 크기 기록. */

#define SET_FIELD(field) \
	if (offsetof(struct spdk_fsdev_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = g_fsdev_opts.field; \
	}
	/* [한국어] 사용자 버퍼 크기가 필드를 수용할 때만 복사 — 미래 필드 추가 호환. */

	SET_FIELD(fsdev_io_pool_size);
	SET_FIELD(fsdev_io_cache_size);

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_fsdev_opts) == 12, "Incorrect size");
	/* [한국어] ABI 가드 — 구조체 크기가 의도와 다르면 컴파일 실패. 새 필드 추가 시 이 값과 SET_FIELD 갱신 필수. */

#undef SET_FIELD
	return 0;
}

/*
 * [한국어]
 * spdk_fsdev_get_memory_domains - fsdev이 지원하는 메모리 도메인 목록 조회 위임.
 *
 * @fsdev     : 대상 fsdev (NULL이면 -EINVAL).
 * @domains   : 출력 배열 (NULL 허용 시 모듈에 따라 카운트만 반환).
 * @array_size: 배열 크기.
 * @return    : 모듈 콜백 결과(>=0=도메인 수, <0=에러). 콜백 미제공 시 0.
 *
 * 메모리 도메인은 RDMA/GPU 등 source/sink 메모리의 위치 식별 — bdev/fsdev이 지원하는 도메인을 통해
 * zero-copy DMA 가능 여부를 결정.
 */
int
spdk_fsdev_get_memory_domains(struct spdk_fsdev *fsdev, struct spdk_memory_domain **domains,
			      int array_size)
{
	if (!fsdev) {
		return -EINVAL;
	}

	if (fsdev->fn_table->get_memory_domains) {
		return fsdev->fn_table->get_memory_domains(fsdev->ctxt, domains, array_size);
		/* [한국어] 모듈이 도메인 조회 콜백 제공 시 위임. */
	}

	return 0;
	/* [한국어] 모듈이 미제공 → 도메인 0개. */
}

/*
 * [한국어]
 * spdk_fsdev_get_module_name - fsdev이 속한 모듈 이름 반환.
 *
 * @fsdev: 대상.
 * @return: 모듈 이름 문자열 포인터(불변).
 */
const char *
spdk_fsdev_get_module_name(const struct spdk_fsdev *fsdev)
{
	return fsdev->module->name;
}

/*
 * [한국어]
 * spdk_fsdev_get_name - fsdev 이름 반환.
 */
const char *
spdk_fsdev_get_name(const struct spdk_fsdev *fsdev)
{
	return fsdev->name;
}

/*
 * [한국어]
 * fsdev_io_complete - IO 객체에 등록된 사용자 cb_fn을 호출(직접 또는 deferred).
 *
 * @ctx: spdk_fsdev_io* (send_msg 시그니처 충족용 void*).
 *
 * 모듈이 submit_request 안에서 동기 완료한 경우(in_submit_request=true) → 무한 재귀 회피를 위해
 * spdk_thread_send_msg로 deferred 실행. 그 외엔 즉시 cb_fn 호출.
 * 호출 thread는 IO 채널의 thread여야 함(assert로 검증).
 */
static inline void
fsdev_io_complete(void *ctx)
{
	struct spdk_fsdev_io *fsdev_io = ctx;
	struct spdk_fsdev_channel *fsdev_ch = fsdev_io->internal.ch;

	if (spdk_unlikely(fsdev_io->internal.in_submit_request)) {
		/*
		 * Defer completion to avoid potential infinite recursion if the
		 * user's completion callback issues a new I/O.
		 */
		spdk_thread_send_msg(spdk_fsdev_io_get_thread(fsdev_io),
				     fsdev_io_complete, fsdev_io);
		/* [한국어] deferred 실행 — submit이 끝난 다음 reactor 메인 루프에서 자기 자신을 다시 호출. */
		return;
	}

	TAILQ_REMOVE(&fsdev_ch->io_submitted, fsdev_io, internal.ch_link);
	/* [한국어] in-flight 큐에서 제거 — IO가 완료되었음을 채널 상태에 반영. */

	assert(fsdev_io->internal.cb_fn != NULL);
	/* [한국어] 사용자 cb_fn 등록 의무 — 미등록은 호출 버그. */
	assert(spdk_get_thread() == spdk_fsdev_io_get_thread(fsdev_io));
	/* [한국어] thread affinity 검증 — 채널 소유 thread가 아니면 데이터 race 위험. */
	fsdev_io->internal.cb_fn(fsdev_io, fsdev_io->internal.cb_arg);
	/* [한국어] 사용자 콜백 호출 — 이 안에서 spdk_fsdev_free_io 호출이 일반적. */
}


/*
 * [한국어]
 * spdk_fsdev_io_complete - 모듈이 IO 완료를 코어에 통보.
 *
 * @fsdev_io: 완료된 IO.
 * @status  : 0(성공) 또는 음수(에러).
 *
 * 모듈이 비동기 처리 완료 시 호출. status<=0 검증, 카운터 감소 후 fsdev_io_complete 호출.
 *
 * 호출 체인:
 *   모듈 백엔드 완료 → [spdk_fsdev_io_complete] → fsdev_io_complete → 사용자 cb_fn
 */
void
spdk_fsdev_io_complete(struct spdk_fsdev_io *fsdev_io, int status)
{
	struct spdk_fsdev_channel *fsdev_ch = fsdev_io->internal.ch;
	struct spdk_fsdev_shared_resource *shared_resource = fsdev_ch->shared_resource;

	assert(status <= 0);
	/* [한국어] status는 0(성공) 또는 음수(errno) 규약. */
	fsdev_io->internal.status = status;
	/* [한국어] IO 객체에 결과 코드 기록 — 사용자 cb_fn이 spdk_fsdev_io_get_status로 조회. */
	assert(fsdev_ch->io_outstanding > 0);
	assert(shared_resource->io_outstanding > 0);
	/* [한국어] 카운터 일관성 — submit 시 ++, complete 시 -- 짝맞춤. */
	fsdev_ch->io_outstanding--;
	shared_resource->io_outstanding--;
	fsdev_io_complete(fsdev_io);
	/* [한국어] 콜백 디스패치(직접 또는 deferred). */
}

/*
 * [한국어]
 * spdk_fsdev_io_get_thread - IO 객체가 묶인 thread 반환.
 */
struct spdk_thread *
spdk_fsdev_io_get_thread(struct spdk_fsdev_io *fsdev_io)
{
	return spdk_io_channel_get_thread(fsdev_io->internal.ch->channel);
	/* [한국어] 백엔드 io_channel을 통해 thread 정보 추출. */
}

/*
 * [한국어]
 * spdk_fsdev_io_get_io_channel - IO 객체의 백엔드 io_channel 반환.
 */
struct spdk_io_channel *
spdk_fsdev_io_get_io_channel(struct spdk_fsdev_io *fsdev_io)
{
	return fsdev_io->internal.ch->channel;
}

/*
 * [한국어]
 * fsdev_register - fsdev 인스턴스 등록 (이름 검증 + io_device 등록 + 리스트 추가).
 *
 * @fsdev: 모듈이 채워서 넘긴 fsdev 객체.
 * @return: 0=성공, 음수=에러.
 *
 * 호출 체인:
 *   spdk_fsdev_register → [fsdev_register] → fsdev_name_add + spdk_io_device_register
 */
static int
fsdev_register(struct spdk_fsdev *fsdev)
{
	char *fsdev_name;
	int ret;

	assert(fsdev->module != NULL);
	/* [한국어] 모듈 연결 의무. */

	if (!fsdev->name) {
		SPDK_ERRLOG("Fsdev name is NULL\n");
		return -EINVAL;
	}

	if (!strlen(fsdev->name)) {
		SPDK_ERRLOG("Fsdev name must not be an empty string\n");
		return -EINVAL;
	}

	/* Users often register their own I/O devices using the fsdev name. In
	 * order to avoid conflicts, prepend fsdev_. */
	fsdev_name = spdk_sprintf_alloc("fsdev_%s", fsdev->name);
	/* [한국어] io_device 등록 시 사용할 이름은 "fsdev_<name>" — 사용자 io_device와 이름 충돌 회피. */
	if (!fsdev_name) {
		SPDK_ERRLOG("Unable to allocate memory for internal fsdev name.\n");
		return -ENOMEM;
	}

	fsdev->internal.status = SPDK_FSDEV_STATUS_READY;
	/* [한국어] 등록 직후엔 READY — open 가능 상태. */
	TAILQ_INIT(&fsdev->internal.open_descs);
	/* [한국어] 빈 디스크립터 리스트로 초기화. */

	ret = fsdev_name_add(&fsdev->internal.fsdev_name, fsdev, fsdev->name);
	/* [한국어] RB tree 등록 — 동일 이름이 이미 있으면 -EEXIST. */
	if (ret != 0) {
		free(fsdev_name);
		return ret;
	}

	spdk_io_device_register(__fsdev_to_io_dev(fsdev),
				fsdev_channel_create, fsdev_channel_destroy,
				sizeof(struct spdk_fsdev_channel),
				fsdev_name);
	/* [한국어] fsdev 자체를 io_device로 등록 — thread별 fsdev_channel을 SPDK가 관리. */

	free(fsdev_name);
	/* [한국어] spdk_io_device_register가 이름을 자체 복사하므로 호출자 버퍼는 free 가능. */

	spdk_spin_init(&fsdev->internal.spinlock);
	/* [한국어] fsdev 내부 spinlock 초기화 — open_descs/status 보호용. */

	SPDK_DEBUGLOG(fsdev, "Inserting fsdev %s into list\n", fsdev->name);
	TAILQ_INSERT_TAIL(&g_fsdev_mgr.fsdevs, fsdev, internal.link);
	/* [한국어] 매니저 fsdev 리스트 끝에 추가. (호출자가 spinlock 잡고 호출하는 것을 권장 — 공개 래퍼는 그렇게 함.) */
	return 0;
}

/*
 * [한국어]
 * fsdev_destroy_cb - 모든 thread의 fsdev_channel destroy 후 SPDK가 호출하는 최종 콜백.
 *
 * @io_device: 인코딩된 fsdev 키.
 *
 * 모듈의 destruct 콜백 호출 + 사용자 unregister cb 호출.
 */
static void
fsdev_destroy_cb(void *io_device)
{
	int			rc;
	struct spdk_fsdev	*fsdev;
	spdk_fsdev_unregister_cb cb_fn;
	void			*cb_arg;

	fsdev = __fsdev_from_io_dev(io_device);
	cb_fn = fsdev->internal.unregister_cb;
	cb_arg = fsdev->internal.unregister_ctx;

	spdk_spin_destroy(&fsdev->internal.spinlock);
	/* [한국어] register 시 init한 spinlock 짝 destroy. */

	rc = fsdev->fn_table->destruct(fsdev->ctxt);
	/* [한국어] 모듈에 자원 해제 요청 — 0/양수 반환은 동기/비동기 완료 신호.
	 * (양수면 모듈이 비동기 destruct 후 spdk_fsdev_destruct_done 호출 책임을 가짐.) */
	if (rc < 0) {
		SPDK_ERRLOG("destruct failed\n");
	}
	if (rc <= 0 && cb_fn != NULL) {
		/* [한국어] 동기 완료(0/음수)인 경우만 즉시 사용자 콜백 호출. 양수면 deferred. */
		cb_fn(cb_arg, rc);
	}
}

/*
 * [한국어]
 * spdk_fsdev_destruct_done - 모듈이 비동기 destruct 완료를 통보(unregister 콜백 트리거).
 *
 * @fsdev    : 대상 fsdev.
 * @fsdeverrno: 결과(0=성공, 음수=에러).
 *
 * 호출 체인:
 *   모듈 비동기 destruct 완료 → [spdk_fsdev_destruct_done] → 사용자 unregister_cb
 */
void
spdk_fsdev_destruct_done(struct spdk_fsdev *fsdev, int fsdeverrno)
{
	if (fsdev->internal.unregister_cb != NULL) {
		fsdev->internal.unregister_cb(fsdev->internal.unregister_ctx, fsdeverrno);
	}
}

/*
 * [한국어]
 * _remove_notify - hot-remove 이벤트를 디스크립터의 thread로 라우팅해 사용자 event_fn 호출.
 *
 * @arg: spdk_fsdev_desc*.
 *
 * fsdev_unregister_unsafe가 send_msg로 본 함수를 디스패치 — race 방지(콜백 안에서 close되어도 안전).
 */
static void
_remove_notify(void *arg)
{
	struct spdk_fsdev_desc *desc = arg;

	spdk_spin_lock(&desc->spinlock);
	desc->refs--;
	/* [한국어] 메시지 in-flight 카운트 감소 — fsdev_unregister_unsafe에서 ++ 짝. */

	if (!desc->closed) {
		spdk_spin_unlock(&desc->spinlock);
		desc->callback.event_fn(SPDK_FSDEV_EVENT_REMOVE, desc->fsdev, desc->callback.ctx);
		/* [한국어] 사용자에게 hot-remove 통보 — 사용자는 보통 spdk_fsdev_close 호출. */
		return;
	} else if (0 == desc->refs) {
		/* This descriptor was closed after this remove_notify message was sent.
		 * spdk_fsdev_close() could not free the descriptor since this message was
		 * in flight, so we free it now using fsdev_desc_free().
		 */
		/* [한국어] 본 메시지 in-flight 도중 사용자가 close했고 refs도 0 — desc 마지막 정리 책임이 본 함수. */
		spdk_spin_unlock(&desc->spinlock);
		fsdev_desc_free(desc);
		return;
	}
	spdk_spin_unlock(&desc->spinlock);
	/* [한국어] closed지만 다른 in-flight 메시지가 있는 경우 — 그 메시지가 마지막이 되어 free. */
}

/* Must be called while holding g_fsdev_mgr.mutex and fsdev->internal.spinlock.
 * returns: 0 - fsdev removed and ready to be destructed.
 *          -EBUSY - fsdev can't be destructed yet.  */
/*
 * [한국어]
 * fsdev_unregister_unsafe - fsdev 해제 핵심 로직 (락 보유 가정).
 *
 * @fsdev: 해제 대상.
 * @return: 0=즉시 destruct 가능, -EBUSY=열린 desc 존재(나중에 마지막 close 시 다시 시도).
 *
 * 열린 디스크립터들에 hot-remove 이벤트를 보내고, 모두 닫혀 있으면 RB tree/리스트에서 제거.
 */
static int
fsdev_unregister_unsafe(struct spdk_fsdev *fsdev)
{
	struct spdk_fsdev_desc	*desc, *tmp;
	int			rc = 0;

	/* Notify each descriptor about hotremoval */
	TAILQ_FOREACH_SAFE(desc, &fsdev->internal.open_descs, link, tmp) {
		/* [한국어] 모든 열린 디스크립터에 hot-remove 통보 큐잉. */
		rc = -EBUSY;
		/* [한국어] 열린 desc가 하나라도 있으면 즉시 destruct 불가 — close 완료 후 재시도. */
		spdk_spin_lock(&desc->spinlock);
		/*
		 * Defer invocation of the event_cb to a separate message that will
		 *  run later on its thread.  This ensures this context unwinds and
		 *  we don't recursively unregister this fsdev again if the event_cb
		 *  immediately closes its descriptor.
		 */
		desc->refs++;
		/* [한국어] in-flight 메시지 카운트 ++ — _remove_notify 도착 시 --. */
		spdk_thread_send_msg(desc->thread, _remove_notify, desc);
		/* [한국어] desc 소유 thread로 메시지 — 사용자 콜백은 그 thread에서 실행. */
		spdk_spin_unlock(&desc->spinlock);
	}

	/* If there are no descriptors, proceed removing the fsdev */
	if (rc == 0) {
		/* [한국어] 열린 desc 없음 — 즉시 정리. */
		TAILQ_REMOVE(&g_fsdev_mgr.fsdevs, fsdev, internal.link);
		SPDK_DEBUGLOG(fsdev, "Removing fsdev %s from list done\n", fsdev->name);
		fsdev_name_del_unsafe(&fsdev->internal.fsdev_name);
		spdk_notify_send("fsdev_unregister", spdk_fsdev_get_name(fsdev));
		/* [한국어] 알림 가입자에게 비동기 통보. */
	}

	return rc;
}

/*
 * [한국어]
 * fsdev_unregister - fsdev 해제 1단계 — 상태를 REMOVING으로 바꾸고 unregister_unsafe 호출.
 *
 * @fsdev : 해제 대상.
 * @_ctx  : 사용 안 함(콜백 시그니처).
 * @status: 사용 안 함.
 *
 * 모든 열린 desc가 닫혀 있는 경우(rc==0)만 spdk_io_device_unregister 진행.
 */
static void
fsdev_unregister(struct spdk_fsdev *fsdev, void *_ctx, int status)
{
	int rc;

	spdk_spin_lock(&g_fsdev_mgr.spinlock);
	spdk_spin_lock(&fsdev->internal.spinlock);
	/*
	 * Set the status to REMOVING after completing to abort channels. Otherwise,
	 * the last spdk_fsdev_close() may call spdk_io_device_unregister() while
	 * spdk_fsdev_for_each_channel() is executed and spdk_io_device_unregister()
	 * may fail.
	 */
	fsdev->internal.status = SPDK_FSDEV_STATUS_REMOVING;
	/* [한국어] 새로운 open 차단 + 마지막 close가 unregister 마무리할 수 있도록 표시. */
	rc = fsdev_unregister_unsafe(fsdev);
	spdk_spin_unlock(&fsdev->internal.spinlock);
	spdk_spin_unlock(&g_fsdev_mgr.spinlock);

	if (rc == 0) {
		/* [한국어] 즉시 destruct 가능 — io_device unregister 진행. 콜백은 fsdev_destroy_cb. */
		spdk_io_device_unregister(__fsdev_to_io_dev(fsdev), fsdev_destroy_cb);
	}
}

/*
 * [한국어]
 * spdk_fsdev_unregister - fsdev 비동기 해제 진입점.
 *
 * @fsdev : 해제 대상.
 * @cb_fn : 해제 완료 콜백.
 * @cb_arg: ctx.
 *
 * 호출 체인:
 *   사용자/finish_iter → [spdk_fsdev_unregister] → fsdev_unregister → fsdev_destroy_cb → cb_fn
 */
void
spdk_fsdev_unregister(struct spdk_fsdev *fsdev, spdk_fsdev_unregister_cb cb_fn, void *cb_arg)
{
	struct spdk_thread	*thread;

	SPDK_DEBUGLOG(fsdev, "Removing fsdev %s from list\n", fsdev->name);

	thread = spdk_get_thread();
	if (!thread) {
		/* The user called this from a non-SPDK thread. */
		/* [한국어] SPDK thread가 아니면 send_msg 라우팅 불가 — 즉시 -ENOTSUP 응답. */
		if (cb_fn != NULL) {
			cb_fn(cb_arg, -ENOTSUP);
		}
		return;
	}

	spdk_spin_lock(&g_fsdev_mgr.spinlock);
	if (fsdev->internal.status == SPDK_FSDEV_STATUS_UNREGISTERING ||
	    fsdev->internal.status == SPDK_FSDEV_STATUS_REMOVING) {
		/* [한국어] 이미 unregister 진행 중 — 중복 요청 거부. */
		spdk_spin_unlock(&g_fsdev_mgr.spinlock);
		if (cb_fn) {
			cb_fn(cb_arg, -EBUSY);
		}
		return;
	}

	spdk_spin_lock(&fsdev->internal.spinlock);
	fsdev->internal.status = SPDK_FSDEV_STATUS_UNREGISTERING;
	/* [한국어] 새로운 open 차단. */
	fsdev->internal.unregister_cb = cb_fn;
	fsdev->internal.unregister_ctx = cb_arg;
	spdk_spin_unlock(&fsdev->internal.spinlock);
	spdk_spin_unlock(&g_fsdev_mgr.spinlock);

	/* @todo: bdev aborts IOs on all channels here. */
	fsdev_unregister(fsdev, fsdev, 0);
	/* [한국어] 실제 해제 로직 진입. */
}

/*
 * [한국어]
 * _tmp_fsdev_event_cb - spdk_fsdev_unregister_by_name이 임시 open할 때 등록하는 더미 이벤트 핸들러.
 *
 * @type, @fsdev, @ctx: 이벤트 정보.
 *
 * 임시 desc는 by_name 함수 내에서 즉시 close되므로 정상 흐름에서 호출되지 않음.
 */
static void
_tmp_fsdev_event_cb(enum spdk_fsdev_event_type type, struct spdk_fsdev *fsdev, void *ctx)
{
	SPDK_NOTICELOG("Unexpected fsdev event type: %d\n", type);
}

/*
 * [한국어]
 * spdk_fsdev_unregister_by_name - 이름과 모듈로 fsdev을 검색해 unregister.
 *
 * @fsdev_name: 대상 이름.
 * @module    : 등록 시 사용한 모듈 (이중 검증 — 다른 모듈이 같은 이름으로 등록한 경우 거부).
 * @cb_fn, cb_arg: 완료 콜백.
 * @return    : 0=성공, 음수=에러.
 */
int
spdk_fsdev_unregister_by_name(const char *fsdev_name, struct spdk_fsdev_module *module,
			      spdk_fsdev_unregister_cb cb_fn, void *cb_arg)
{
	struct spdk_fsdev_desc *desc;
	struct spdk_fsdev *fsdev;
	int rc;

	rc = spdk_fsdev_open(fsdev_name, _tmp_fsdev_event_cb, NULL, &desc);
	/* [한국어] 임시 desc로 fsdev 식별 — 동시에 다른 사용자가 unregister 못하게 reference 확보 효과. */
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open fsdev with name: %s\n", fsdev_name);
		return rc;
	}

	fsdev = spdk_fsdev_desc_get_fsdev(desc);

	if (fsdev->module != module) {
		/* [한국어] 모듈 불일치 — 잘못된 호출자. */
		spdk_fsdev_close(desc);
		SPDK_ERRLOG("Fsdev %s was not registered by the specified module.\n",
			    fsdev_name);
		return -ENODEV;
	}

	spdk_fsdev_unregister(fsdev, cb_fn, cb_arg);
	/* [한국어] 비동기 unregister 시작. */
	spdk_fsdev_close(desc);
	/* [한국어] 임시 desc 즉시 close — REMOVING 상태이므로 마지막 close가 unregister 완료 트리거. */

	return 0;
}

/*
 * [한국어]
 * fsdev_open - desc를 fsdev에 연결(open_descs 리스트 등록).
 *
 * @fsdev: 대상.
 * @desc : 새 디스크립터.
 * @return: 0=성공, -ENOTSUP/-ENODEV.
 */
static int
fsdev_open(struct spdk_fsdev *fsdev, struct spdk_fsdev_desc *desc)
{
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (!thread) {
		/* [한국어] non-SPDK thread에서 호출 — 이벤트 라우팅 불가. */
		SPDK_ERRLOG("Cannot open fsdev from non-SPDK thread.\n");
		return -ENOTSUP;
	}

	SPDK_DEBUGLOG(fsdev, "Opening descriptor %p for fsdev %s on thread %p\n",
		      desc, fsdev->name, spdk_get_thread());

	desc->fsdev = fsdev;
	desc->thread = thread;
	/* [한국어] 콜백 라우팅 키. */

	spdk_spin_lock(&fsdev->internal.spinlock);
	if (fsdev->internal.status == SPDK_FSDEV_STATUS_UNREGISTERING ||
	    fsdev->internal.status == SPDK_FSDEV_STATUS_REMOVING) {
		/* [한국어] 이미 해제 중인 fsdev은 open 거부. */
		spdk_spin_unlock(&fsdev->internal.spinlock);
		return -ENODEV;
	}

	TAILQ_INSERT_TAIL(&fsdev->internal.open_descs, desc, link);
	/* [한국어] 열린 디스크립터 추적 — hot-remove 시 이 리스트로 통보. */
	spdk_spin_unlock(&fsdev->internal.spinlock);
	return 0;
}

/*
 * [한국어]
 * fsdev_desc_alloc - 디스크립터 메모리 할당 + spinlock init + 콜백 등록.
 *
 * @fsdev    : 대상(현 시점엔 사용 안 함 — 시그니처 호환).
 * @event_cb : 이벤트 콜백.
 * @event_ctx: 콜백 ctx.
 * @_desc    : 출력.
 * @return   : 0=성공, -ENOMEM.
 */
static int
fsdev_desc_alloc(struct spdk_fsdev *fsdev, spdk_fsdev_event_cb_t event_cb, void *event_ctx,
		 struct spdk_fsdev_desc **_desc)
{
	struct spdk_fsdev_desc *desc;

	desc = calloc(1, sizeof(*desc));
	/* [한국어] zero-init 할당 — closed=false/refs=0 보장. */
	if (desc == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for fsdev descriptor\n");
		return -ENOMEM;
	}

	desc->callback.event_fn = event_cb;
	desc->callback.ctx = event_ctx;
	spdk_spin_init(&desc->spinlock);
	/* [한국어] desc 내부 spinlock 초기화 — closed/refs 보호. */
	*_desc = desc;
	return 0;
}

/*
 * [한국어]
 * spdk_fsdev_open - fsdev 디스크립터 생성(공개 API).
 *
 * @fsdev_name : 대상 이름.
 * @event_cb   : 이벤트 콜백 (필수).
 * @event_ctx  : 콜백 ctx.
 * @_desc      : 출력.
 * @return     : 0=성공, 음수=에러.
 *
 * 호출 체인:
 *   사용자 → [spdk_fsdev_open] → fsdev_get_by_name + fsdev_desc_alloc + fsdev_open
 */
int
spdk_fsdev_open(const char *fsdev_name, spdk_fsdev_event_cb_t event_cb, void *event_ctx,
		struct spdk_fsdev_desc **_desc)
{
	struct spdk_fsdev_desc *desc;
	struct spdk_fsdev *fsdev;
	int rc;

	if (event_cb == NULL) {
		SPDK_ERRLOG("Missing event callback function\n");
		return -EINVAL;
	}

	spdk_spin_lock(&g_fsdev_mgr.spinlock);
	/* [한국어] 매니저 락 — fsdev 검색 + open 동안 unregister와의 race 방지. */

	fsdev = fsdev_get_by_name(fsdev_name);
	if (fsdev == NULL) {
		SPDK_NOTICELOG("Currently unable to find fsdev with name: %s\n", fsdev_name);
		spdk_spin_unlock(&g_fsdev_mgr.spinlock);
		return -ENODEV;
	}

	rc = fsdev_desc_alloc(fsdev, event_cb, event_ctx, &desc);
	if (rc != 0) {
		spdk_spin_unlock(&g_fsdev_mgr.spinlock);
		return rc;
	}

	rc = fsdev_open(fsdev, desc);
	/* [한국어] desc를 fsdev에 연결. 이미 해제 중이면 -ENODEV. */
	if (rc != 0) {
		fsdev_desc_free(desc);
		desc = NULL;
	}

	*_desc = desc;
	spdk_spin_unlock(&g_fsdev_mgr.spinlock);
	return rc;
}

/*
 * [한국어]
 * fsdev_close - 디스크립터 분리 + (마지막 close + REMOVING 상태면) unregister 마무리.
 *
 * @fsdev: 대상.
 * @desc : 닫을 디스크립터.
 *
 * REMOVING 상태에서 마지막 desc가 닫히면 fsdev_unregister_unsafe로 io_device unregister 트리거.
 */
static void
fsdev_close(struct spdk_fsdev *fsdev, struct spdk_fsdev_desc *desc)
{
	int rc;

	spdk_spin_lock(&fsdev->internal.spinlock);
	spdk_spin_lock(&desc->spinlock);

	TAILQ_REMOVE(&fsdev->internal.open_descs, desc, link);
	/* [한국어] open_descs에서 분리. */
	desc->closed = true;
	/* [한국어] 이후 _remove_notify가 도착해도 정상 close된 상태로 인지. */
	if (0 == desc->refs) {
		/* [한국어] in-flight 메시지 없음 → 즉시 free 안전. */
		spdk_spin_unlock(&desc->spinlock);
		fsdev_desc_free(desc);
	} else {
		/* [한국어] in-flight 있음 — 마지막 _remove_notify가 free 책임. */
		spdk_spin_unlock(&desc->spinlock);
	}

	if (fsdev->internal.status == SPDK_FSDEV_STATUS_REMOVING &&
	    TAILQ_EMPTY(&fsdev->internal.open_descs)) {
		/* [한국어] hot-remove 진행 중 + 마지막 desc 닫힘 → unregister 마무리. */
		rc = fsdev_unregister_unsafe(fsdev);
		spdk_spin_unlock(&fsdev->internal.spinlock);

		if (rc == 0) {
			spdk_io_device_unregister(__fsdev_to_io_dev(fsdev), fsdev_destroy_cb);
		}
	} else {
		spdk_spin_unlock(&fsdev->internal.spinlock);
	}
}

/*
 * [한국어]
 * spdk_fsdev_close - 디스크립터 닫기(공개 API).
 *
 * @desc: 닫을 디스크립터.
 *
 * 호출 thread는 open한 thread여야 함(assert).
 */
void
spdk_fsdev_close(struct spdk_fsdev_desc *desc)
{
	struct spdk_fsdev *fsdev = spdk_fsdev_desc_get_fsdev(desc);

	SPDK_DEBUGLOG(fsdev, "Closing descriptor %p for fsdev %s on thread %p\n",
		      desc, fsdev->name, spdk_get_thread());
	assert(desc->thread == spdk_get_thread());
	/* [한국어] thread affinity 검증 — 다른 thread에서 close하면 spinlock/메시지 race. */
	spdk_spin_lock(&g_fsdev_mgr.spinlock);
	fsdev_close(fsdev, desc);
	spdk_spin_unlock(&g_fsdev_mgr.spinlock);
}

/*
 * [한국어]
 * spdk_fsdev_register - 모듈이 fsdev 인스턴스를 코어에 등록(공개 API).
 *
 * @fsdev : 모듈이 채운 객체.
 * @return: 0=성공, 음수=에러.
 */
int
spdk_fsdev_register(struct spdk_fsdev *fsdev)
{
	int rc;

	rc = fsdev_register(fsdev);
	if (rc != 0) {
		return rc;
	}

	spdk_notify_send("fsdev_register", spdk_fsdev_get_name(fsdev));
	/* [한국어] 가입자에게 새로운 fsdev 등록 통보 (비동기). */
	return rc;
}

/*
 * [한국어]
 * spdk_fsdev_desc_get_fsdev - 디스크립터에서 fsdev 인스턴스 추출.
 */
struct spdk_fsdev *
spdk_fsdev_desc_get_fsdev(struct spdk_fsdev_desc *desc)
{
	assert(desc != NULL);
	return desc->fsdev;
}

/*
 * [한국어]
 * spdk_fsdev_module_list_add - 모듈을 매니저 모듈 리스트에 등록.
 *
 * @fsdev_module: 모듈 객체.
 *
 * 보통 모듈의 SPDK_FSDEV_MODULE_REGISTER 매크로가 만든 constructor에서 호출.
 * 동일 이름이 이미 등록되어 있으면 abort(부팅 단계의 명백한 버그).
 */
void
spdk_fsdev_module_list_add(struct spdk_fsdev_module *fsdev_module)
{

	if (spdk_fsdev_module_list_find(fsdev_module->name)) {
		SPDK_ERRLOG("ERROR: module '%s' already registered.\n", fsdev_module->name);
		assert(false);
	}

	TAILQ_INSERT_TAIL(&g_fsdev_mgr.fsdev_modules, fsdev_module, internal.tailq);
	/* [한국어] 모듈 리스트에 추가 — 부팅/종료 시 이 순서대로 init/역순으로 fini. */
}

/*
 * [한국어]
 * spdk_fsdev_module_list_find - 이름으로 모듈 검색(선형).
 *
 * @name : 모듈 이름.
 * @return: 모듈 객체 또는 NULL.
 *
 * 모듈 수는 적어 선형 검색으로 충분. fsdev 인스턴스는 RB tree.
 */
struct spdk_fsdev_module *
spdk_fsdev_module_list_find(const char *name)
{
	struct spdk_fsdev_module *fsdev_module;

	TAILQ_FOREACH(fsdev_module, &g_fsdev_mgr.fsdev_modules, internal.tailq) {
		if (strcmp(name, fsdev_module->name) == 0) {
			break;
			/* [한국어] 일치 — fsdev_module 그대로 반환. */
		}
	}

	return fsdev_module;
	/* [한국어] 미발견 시 fsdev_module은 TAILQ_FOREACH 종료 후 NULL. */
}

SPDK_LOG_REGISTER_COMPONENT(fsdev)
/* [한국어] "fsdev" 로그 컴포넌트 등록 — SPDK_DEBUGLOG(fsdev, ...)이 활성화되도록 디버그 카테고리 등록.
 * 매크로는 .init_array 등록자를 만들어 부팅 시 자동 등록. */
