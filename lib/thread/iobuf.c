/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK iobuf 공용 I/O 버퍼 풀 구현 (iobuf.c)
 *
 * === 파일의 역할 ===
 * SPDK 의 모든 서브시스템(bdev, NVMe-oF, iSCSI, blobstore 등)이 데이터 페이로드를
 * 담기 위해 사용하는 "공용 DMA 가능 버퍼 풀"을 한 곳에서 관리한다. 각 모듈은
 * spdk_iobuf_register_module() 로 자기 이름을 등록하고, 부팅 시 자기가 필요한
 * cache_size 와 pool_size 합산치를 알려주면, 본 파일은 이 모든 요구를 합산해
 * 두 종류(small / large)의 hugepage 메모리 영역을 단 한 번에 reserve 한다.
 * 동작 시점에는 spdk_iobuf_get() / spdk_iobuf_put() 으로 버퍼를 빌리고/돌려주며,
 * per-thread 캐시(스레드별 작은 ring) 와 글로벌 풀(spdk_ring) 사이에서 batch
 * 단위로 spillover 가 일어난다. 풀이 일시적으로 고갈되면 즉시 NULL 을 반환하고,
 * 호출자가 함께 넘긴 콜백(spdk_iobuf_entry)을 wait queue 에 매달아 다른 스레드가
 * put 할 때 깨우는 비동기 wait 모델을 따른다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택의 "데이터 버퍼 메모리" 레이어를 담당한다. spdk_thread_lib_init()
 * 이후, 각 SPDK 모듈이 부팅 단계에서 본 파일의 register_module() 을 호출하고,
 * 모든 등록이 끝난 뒤 application(예: spdk_app_start) 이 spdk_iobuf_initialize()
 * 를 호출해 hugepage(spdk_malloc) 와 lockless ring(spdk_ring) 을 한 번에 만든다.
 * 런타임에는 bdev/NVMe-oF 같은 상위 레이어가 자기 io_channel 을 생성할 때
 * spdk_iobuf_channel_init() 을 호출해 per-thread 캐시(spdk_iobuf_channel) 를
 * 만들고, 그 위에서 spdk_iobuf_get / spdk_iobuf_put 으로 버퍼를 사용한다.
 * 모든 함수는 호출자가 속한 SPDK thread(=lcore 고정 reactor) 컨텍스트에서 실행되며,
 * cross-thread 호출은 절대 일어나지 않는다 — 이것이 lockless 의 근거다.
 *
 * === 타 모듈과의 연결 ===
 *  - spdk/env.h            : spdk_malloc / spdk_free / spdk_mem_get_numa_id /
 *                            spdk_env_get_*_numa_id (DPDK hugepage / NUMA 추상화)
 *  - spdk/thread.h         : spdk_io_device_register / spdk_get_io_channel /
 *                            spdk_for_each_channel (per-thread 채널 인프라)
 *  - spdk/util.h           : spdk_min / spdk_max / SPDK_ALIGN_CEIL / TAILQ/STAILQ 매크로
 *  - spdk/likely.h         : spdk_unlikely (브랜치 힌트)
 *  - spdk/log.h            : SPDK_ERRLOG (런타임 에러 로깅)
 *  - 내부에서 쓰는 핵심 자료구조는 spdk/thread.h 의 다음 typedef 들이다:
 *      struct spdk_iobuf_buffer        : 풀에 들어가는 한 개의 버퍼 헤더 (free list 노드)
 *      struct spdk_iobuf_pool_cache    : per-thread per-NUMA 의 small/large 한 쪽 풀
 *      struct spdk_iobuf_node_cache    : 한 NUMA 노드에 대응하는 small+large 쌍
 *      struct spdk_iobuf_channel       : 모듈 하나가 받는 per-thread 핸들
 *      struct spdk_iobuf_entry         : 풀이 비었을 때 매다는 wait queue 엔트리
 *      struct spdk_iobuf_opts          : small/large pool_count, bufsize, NUMA 토글
 *      struct spdk_iobuf_module_stats  : 모듈별 cache/main/retry 카운터
 *  - 데이터 흐름:
 *      [모듈 init] --register_module--> g_iobuf.modules
 *      [app init]  --iobuf_initialize-> g_iobuf.node[*] (hugepage + ring 만들기)
 *      [thread init] --channel_init--> 글로벌 풀에서 N 개 dequeue → per-thread cache
 *      [I/O 시] iobuf_get → cache hit / miss(글로벌 batch dequeue) / wait queue 등록
 *      [I/O 완료] iobuf_put → waiter 깨우기 OR cache 환원 OR batch flush
 *
 * === 주요 함수/구조체 요약 ===
 *  - spdk_iobuf_initialize()       : 부팅 시 hugepage + lockless ring 한 번에 reserve
 *  - spdk_iobuf_finish()           : 종료 시 io_device unregister 후 모든 자원 해제
 *  - spdk_iobuf_set_opts/get_opts(): pool_count/bufsize/NUMA 옵션 조회·설정
 *  - spdk_iobuf_register_module()  : 모듈이 자기 이름을 g_iobuf.modules 에 등록
 *  - spdk_iobuf_channel_init/fini(): per-thread 캐시(spdk_iobuf_channel) 생성/해제
 *  - spdk_iobuf_get()              : 버퍼 한 개 획득 (cache hit → ring batch fetch → wait)
 *  - spdk_iobuf_put()              : 버퍼 반환 (waiter 즉시 깨움 → cache → batch flush)
 *  - spdk_iobuf_entry_abort()      : wait queue 에 매단 엔트리를 취소
 *  - spdk_iobuf_for_each_entry()   : 자기 모듈 소유의 wait queue 엔트리 순회
 *  - spdk_iobuf_get_stats()        : 모든 채널의 cache/main/retry 통계 비동기 수집
 *  핵심 전역: g_iobuf (옵션 + modules 리스트 + per-NUMA iobuf_node 배열).
 */

#include "spdk/env.h"      /* [한국어] DPDK hugepage / DMA 메모리 / NUMA 헬퍼 (spdk_malloc, spdk_mem_get_numa_id …) */
#include "spdk/util.h"     /* [한국어] SPDK_ALIGN_CEIL, spdk_min/max, TAILQ/STAILQ 매크로 — 큐와 정렬 산술용 */
#include "spdk/likely.h"   /* [한국어] spdk_likely / spdk_unlikely — hot path 브랜치 힌트(__builtin_expect) */
#include "spdk/log.h"      /* [한국어] SPDK_ERRLOG — 풀 고갈 / 옵션 검증 실패 등 런타임 에러 출력 */
#include "spdk/thread.h"   /* [한국어] io_device / io_channel / spdk_iobuf_* 공개 타입 정의 본체 */

/* [한국어] 풀 크기 하한선 — 너무 작으면 한 reactor 가 자기 cache 채우는 것조차 실패할 수 있어 강제. */
#define IOBUF_MIN_SMALL_POOL_SIZE	64
#define IOBUF_MIN_LARGE_POOL_SIZE	8
/* [한국어] 사용자가 set_opts 를 안 부르면 적용되는 기본 풀 크기(엔트리 개수). */
#define IOBUF_DEFAULT_SMALL_POOL_SIZE	8192
#define IOBUF_DEFAULT_LARGE_POOL_SIZE	1024
/* [한국어] 모든 버퍼는 4KB 정렬 — NVMe PRP / hugepage 정렬 요구를 만족시키기 위함. */
#define IOBUF_ALIGNMENT			4096
/* [한국어] 사용자가 줄여도 더 작아질 수 없는 한 버퍼의 최소 크기(바이트). */
#define IOBUF_MIN_SMALL_BUFSIZE		4096
#define IOBUF_MIN_LARGE_BUFSIZE		8192
/* [한국어] 기본 small 버퍼 크기 — 8KB. 일반적인 작은 I/O 페이로드 + 메타 헤더용. */
#define IOBUF_DEFAULT_SMALL_BUFSIZE	(8 * 1024)
/* 132k is a weird choice at first, but this needs to be large enough to accommodate
 * the default maximum size (128k) plus metadata everywhere. For code paths that
 * are explicitly configured, the math is instead done properly. This is only
 * for the default. */
/* [한국어] 기본 large 버퍼 크기 — 132KB. 128KB(SPDK 기본 max I/O 사이즈) + 메타데이터 마진을 모두 수용하기 위해 132KB 로 잡음. */
#define IOBUF_DEFAULT_LARGE_BUFSIZE	(132 * 1024)
/* [한국어] 한 reactor(io_channel) 위에 동시에 살아있을 수 있는 모듈별 채널의 최대 개수. */
#define IOBUF_MAX_CHANNELS		64
/* [한국어] populate / get / put 모두에서 글로벌 ring 을 한 번에 dequeue/enqueue 하는 배치 크기. */
#define IOBUF_POPULATE_BATCH_SIZE	64

/* [한국어] 풀에 들어가는 free buffer 의 헤더(spdk_iobuf_buffer)는 small 버퍼 한 개 안에 들어갈 수 있어야 한다.
 * 각 free buffer 는 메모리 첫 부분에 STAILQ 노드를 두고 그 뒤에 데이터가 오는 in-place 헤더 방식이기 때문. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_iobuf_buffer) <= IOBUF_MIN_SMALL_BUFSIZE,
		   "Invalid data offset");

/* [한국어] iobuf 모듈 전체가 한 번이라도 spdk_iobuf_initialize() 됐는지를 추적하는 플래그.
 * spdk_iobuf_finish() 가 두 번 불려도 안전하게 동작하도록 가드 역할 수행. */
static bool g_iobuf_is_initialized = false;

/*
 * [한국어]
 * struct iobuf_channel_node — 한 NUMA 노드 단위의 wait queue 묶음.
 *
 * 한 spdk_iobuf_channel(=한 모듈의 한 reactor 인스턴스) 이 자기에게 매달린 모든
 * waiter 들을 small/large 별로 보관할 수 있도록, 같은 reactor 위에 살아있는 다른
 * 모듈 채널들이 공유해서 쓰는 큐. iobuf_channel(아래) 안에 NUMA 개수만큼 배열로
 * 들어 있고, 각 spdk_iobuf_node_cache 의 .queue 포인터가 이쪽을 가리키도록 init.
 */
struct iobuf_channel_node {
	spdk_iobuf_entry_stailq_t	small_queue;
	/* [한국어] small 버퍼가 고갈됐을 때 매달리는 spdk_iobuf_entry STAILQ 헤드.
	 * 설정자: iobuf_channel_create_cb() 가 STAILQ_INIT 으로 초기화.
	 * 추가: spdk_iobuf_get() 이 풀이 비면 STAILQ_INSERT_TAIL 로 push.
	 * 제거: spdk_iobuf_put() 이 STAILQ_REMOVE_HEAD 로 pop 후 entry->cb_fn 호출.
	 * 동기화: 같은 reactor 의 모든 모듈 채널이 공유하지만, reactor 가 lcore 고정이라 락 불필요. */

	spdk_iobuf_entry_stailq_t	large_queue;
	/* [한국어] large 버퍼용 wait queue. 의미·동기화 모두 small_queue 와 동일. */
};

/*
 * [한국어]
 * struct iobuf_channel — io_device(g_iobuf) 위에 만들어지는 per-thread 컨텍스트.
 *
 * spdk_io_device_register(&g_iobuf, …) 로 등록한 io_device 의 ctx 영역(=
 * spdk_io_channel 의 hot ctx) 에 매핑된다. 한 reactor 에는 정확히 한 개의
 * iobuf_channel 이 존재하고, 그 위에서 같은 reactor 위의 여러 모듈이
 * spdk_iobuf_channel 을 들고 동거한다.
 */
struct iobuf_channel {
	struct iobuf_channel_node	node[SPDK_CONFIG_MAX_NUMA_NODES];
	/* [한국어] NUMA 노드 개수만큼의 wait queue 슬롯.
	 * NUMA off 모드에서는 [0] 만 쓰고, on 모드에서는 IOBUF_FOREACH_NUMA_ID 로 모든 활성 노드를 순회.
	 * 같은 reactor 위 여러 모듈이 자기 spdk_iobuf_node_cache.queue 를 이 배열의 entry 로 link 한다. */

	struct spdk_iobuf_channel	*channels[IOBUF_MAX_CHANNELS];
	/* [한국어] 이 reactor 에서 살아있는 모듈 채널들의 약한 참조 배열(NULL 가능).
	 * 설정자: spdk_iobuf_channel_init() 이 첫 비어있는 슬롯에 자기 ch 포인터 저장.
	 * 해제: spdk_iobuf_channel_fini() 에서 자기 슬롯을 NULL 로 비움.
	 * 사용처: iobuf_get_channel_stats() 가 stats 수집할 때 모든 모듈 채널 순회 용도.
	 * 동기화: reactor 단일 스레드 보장이라 락 불필요. */
};

/*
 * [한국어]
 * struct iobuf_module — register_module() 로 등록된 모듈 메타데이터.
 *
 * g_iobuf.modules 라는 글로벌 TAILQ 에 한 노드씩 추가된다.
 * spdk_iobuf_channel 은 자기를 만든 모듈의 이름을 module 포인터로 들고 있어,
 * waiter 가 어느 모듈 소유인지 판별하는 데 쓰인다.
 */
struct iobuf_module {
	char				*name;
	/* [한국어] strdup 으로 복사된 모듈 이름 (소유권은 이 구조체).
	 * 설정자: register_module() 에서 strdup. 해제: unregister/finish 시 free. */

	TAILQ_ENTRY(iobuf_module)	tailq;
	/* [한국어] g_iobuf.modules 글로벌 리스트에 매달리기 위한 링크 노드. */
};

/*
 * [한국어]
 * struct iobuf_node — NUMA 노드 한 개에 대응하는 글로벌 풀 인스턴스.
 *
 * NUMA 가 활성화된 경우 노드 개수만큼 만들어지고, 비활성이면 [0] 한 개만 생성된다.
 * 각 노드는 small/large 두 개의 lockless MP/MC ring + 그 ring 을 채운 hugepage
 * 백킹 메모리(_pool_base) 를 갖는다.
 */
struct iobuf_node {
	struct spdk_ring		*small_pool;
	/* [한국어] small 크기 free buffer 들을 담는 lockless MP/MC ring(spdk_ring).
	 * 설정자: iobuf_node_initialize() 가 spdk_ring_create. 해제: iobuf_node_free() 가 spdk_ring_free.
	 * 동기화: MP/MC 모드라 어느 reactor 가 dequeue/enqueue 해도 안전 — 글로벌 풀이 진정한 공유 자원.
	 * 사용처: per-thread cache 가 비면 spdk_ring_dequeue 로 batch fetch, 너무 차면 spdk_ring_enqueue 로 환원. */

	struct spdk_ring		*large_pool;
	/* [한국어] large 크기 free buffer 용 동일한 ring. */

	void				*small_pool_base;
	/* [한국어] small_pool 안의 모든 버퍼가 슬라이스되어 나오는 hugepage 백킹 메모리의 시작.
	 * 설정자: iobuf_node_initialize() 가 spdk_malloc(SPDK_MALLOC_DMA, IOBUF_ALIGNMENT) 로 한 번에 reserve.
	 * 해제: iobuf_node_free() 가 spdk_free.
	 * 의미: 이 base + i*small_bufsize 가 i 번째 free buffer 의 시작 주소 → 한 번 잡으면 반환 시까지 고정. */

	void				*large_pool_base;
	/* [한국어] large_pool 의 백킹 hugepage 시작 주소. 의미는 small_pool_base 와 동일. */
};

/*
 * [한국어]
 * struct iobuf — iobuf 서브시스템의 단일 글로벌 상태.
 *
 * 정확히 한 개의 인스턴스 g_iobuf 만 존재한다 (싱글톤). spdk_io_device_register()
 * 의 io_device 키로도 자기 자신의 주소(&g_iobuf) 를 사용한다.
 */
struct iobuf {
	struct spdk_iobuf_opts		opts;
	/* [한국어] 풀 크기 / 버퍼 크기 / NUMA 활성 여부 등 사용자 튜닝 가능한 옵션.
	 * 설정자: spdk_iobuf_set_opts() (initialize 이전에만 의미 있음). 읽는 자: 거의 모든 함수. */

	TAILQ_HEAD(, iobuf_module)	modules;
	/* [한국어] 등록된 모든 iobuf_module 의 글로벌 리스트.
	 * 추가: spdk_iobuf_register_module(). 제거: spdk_iobuf_unregister_module() / iobuf_unregister_cb().
	 * 동기화: 등록은 init 단계(단일 스레드)에서만 일어난다고 가정하여 락 없음. */

	spdk_iobuf_finish_cb		finish_cb;
	/* [한국어] spdk_iobuf_finish() 가 받은 사용자 종료 콜백 — io_device unregister 가 모두 끝난 시점에 호출. */

	void				*finish_arg;
	/* [한국어] finish_cb 에 전달할 사용자 컨텍스트. */

	struct iobuf_node		node[SPDK_CONFIG_MAX_NUMA_NODES];
	/* [한국어] NUMA 노드 한 개당 한 개의 iobuf_node(=small+large 글로벌 풀). [0] 만 쓰는 모드/전 노드 모드 모두 지원. */
};

/*
 * [한국어]
 * IOBUF_FOREACH_NUMA_ID — NUMA 활성/비활성을 통합 처리하는 순회 매크로.
 *
 * NUMA on  : spdk_env_get_first_numa_id() ~ spdk_env_get_next_numa_id() 로 실제 활성 노드만 방문.
 * NUMA off : i = 0 → 다음 반복에서 i = INT32_MAX 로 즉시 종료 → [0] 한 번만 방문.
 *
 * 모든 함수가 이 매크로로 풀/캐시를 도므로, 호출자는 NUMA 분기 코드를 직접 쓸 필요가 없다.
 */
#define IOBUF_FOREACH_NUMA_ID(i)						\
	for (i = g_iobuf.opts.enable_numa ? spdk_env_get_first_numa_id() : 0;	\
	     i < INT32_MAX;							\
	     i = g_iobuf.opts.enable_numa ? spdk_env_get_next_numa_id(i) : INT32_MAX)

/* [한국어] 단일 글로벌 인스턴스. 기본 옵션 값으로 정적 초기화 — set_opts 미호출 시 그대로 사용. */
static struct iobuf g_iobuf = {
	.modules = TAILQ_HEAD_INITIALIZER(g_iobuf.modules), /* [한국어] modules 리스트 헤드를 빈 상태로 초기화. */
	.node = {},                                          /* [한국어] node 배열 전체 0 초기화 (small/large _pool 모두 NULL). */
	.opts = {
		.small_pool_count = IOBUF_DEFAULT_SMALL_POOL_SIZE, /* [한국어] 기본 8192 개 small 버퍼. */
		.large_pool_count = IOBUF_DEFAULT_LARGE_POOL_SIZE, /* [한국어] 기본 1024 개 large 버퍼. */
		.small_bufsize = IOBUF_DEFAULT_SMALL_BUFSIZE,      /* [한국어] 기본 8KB small 버퍼 크기. */
		.large_bufsize = IOBUF_DEFAULT_LARGE_BUFSIZE,      /* [한국어] 기본 132KB large 버퍼 크기 (128KB max I/O + meta). */
	},
};

/*
 * [한국어]
 * struct iobuf_get_stats_ctx — spdk_iobuf_get_stats() 의 비동기 컨텍스트.
 *
 * spdk_for_each_channel() 호출 시 각 reactor 로 hop 하면서 통계를 누적하기 위해
 * heap 에 잡아 들고 다닌다. 모든 reactor 순회가 끝나면 완료 콜백에서 free 된다.
 */
struct iobuf_get_stats_ctx {
	struct spdk_iobuf_module_stats	*modules;
	/* [한국어] 등록된 모듈 개수만큼 calloc 된 통계 누적 배열. cb_fn 호출 후 free. */

	uint32_t			num_modules;
	/* [한국어] modules 배열 길이 (=g_iobuf.modules 등록 개수 스냅샷). */

	spdk_iobuf_get_stats_cb		cb_fn;
	/* [한국어] 모든 reactor 순회가 끝났을 때 모듈별 통계와 함께 호출될 사용자 완료 콜백. */

	void				*cb_arg;
	/* [한국어] cb_fn 에 전달할 사용자 컨텍스트. */
};

/*
 * [한국어]
 * iobuf_channel_create_cb — io_device(g_iobuf) 에 새 io_channel 이 생길 때 호출.
 *
 * @io_device: 본 io_device 키(=&g_iobuf). 사용 안 함.
 * @ctx     : SPDK 가 채널 ctx 영역에 할당해 준 struct iobuf_channel.
 * @return  : 0 (실패 가능 경로 없음).
 *
 * spdk_io_channel_get_ctx() 의 backing buffer(=struct iobuf_channel) 가 호출 시점엔
 * 0-init 상태이므로, 이 콜백은 NUMA 노드별 wait queue STAILQ 헤드만 초기화한다.
 * 실행 컨텍스트: 처음 spdk_get_io_channel(&g_iobuf) 를 부르는 SPDK thread.
 *
 * 호출 체인:
 *   spdk_iobuf_channel_init() → spdk_get_io_channel(&g_iobuf) → [iobuf_channel_create_cb]
 */
static int
iobuf_channel_create_cb(void *io_device, void *ctx)
{
	struct iobuf_channel *ch = ctx;          /* [한국어] SPDK 가 zero-init 해 준 채널 ctx 캐스트 — 본 콜백이 채워야 할 대상. */
	struct iobuf_channel_node *node;         /* [한국어] NUMA 슬롯 별 wait queue 묶음 포인터. */
	int32_t i;                                /* [한국어] NUMA id (NUMA off 모드면 0 한 번만). */

	IOBUF_FOREACH_NUMA_ID(i) {               /* [한국어] 활성 NUMA 노드(또는 [0] 만)를 순회. */
		node = &ch->node[i];              /* [한국어] 이 NUMA 노드에 해당하는 wait queue 묶음 가져옴. */
		STAILQ_INIT(&node->small_queue);  /* [한국어] small 풀 고갈 시 매달릴 waiter 큐 초기화 (빈 STAILQ). */
		STAILQ_INIT(&node->large_queue);  /* [한국어] large 풀 고갈 시 매달릴 waiter 큐 초기화. */
	}

	return 0;                                /* [한국어] 채널 생성 실패 가능성 없음 — 항상 성공. */
}

/*
 * [한국어]
 * iobuf_channel_destroy_cb — io_channel 의 마지막 ref 가 풀릴 때 호출되는 클린업.
 *
 * @io_device: 본 io_device 키(=&g_iobuf). 미사용.
 * @ctx     : 정리할 struct iobuf_channel.
 *
 * 채널이 사라지는 시점엔 같은 reactor 위 모든 모듈이 spdk_iobuf_channel_fini() 까지
 * 끝난 상태여야 하므로, wait queue 가 비어있다고 단정한다 — 비어 있지 않다면 사용 흐름
 * 자체에 버그가 있다는 뜻이라 assert 로만 검증한다.
 *
 * 호출 체인:
 *   spdk_put_io_channel() (마지막 ref) → SPDK 채널 해제 경로 → [iobuf_channel_destroy_cb]
 */
static void
iobuf_channel_destroy_cb(void *io_device, void *ctx)
{
	struct iobuf_channel *ch = ctx;                                        /* [한국어] 정리 대상 채널. */
	struct iobuf_channel_node *node __attribute__((unused));               /* [한국어] release 빌드에선 assert 만 쓰이므로 unused 경고 차단. */
	int32_t i;                                                              /* [한국어] NUMA 순회 인덱스. */

	IOBUF_FOREACH_NUMA_ID(i) {                                             /* [한국어] 모든 활성 NUMA 노드 순회. */
		node = &ch->node[i];                                            /* [한국어] 정리 검증 대상 wait queue 묶음. */
		assert(STAILQ_EMPTY(&node->small_queue));                       /* [한국어] small waiter 가 남아 있으면 누군가 fini 를 빼먹은 것 — 버그. */
		assert(STAILQ_EMPTY(&node->large_queue));                       /* [한국어] large 도 동일. */
	}
}

/*
 * [한국어]
 * iobuf_node_initialize — 한 NUMA 노드용 글로벌 풀(small + large) 만들기.
 *
 * @node    : 채워 넣을 g_iobuf.node[i] 슬롯.
 * @numa_id : 이 풀이 묶일 NUMA 노드 ID. NUMA off 모드면 SPDK_ENV_NUMA_ID_ANY 로 강제.
 * @return  : 0 성공, -ENOMEM ring/hugepage 할당 실패.
 *
 * 1) spdk_ring_create() 로 lockless MP/MC ring 생성 → 이게 곧 free buffer 색인.
 * 2) spdk_malloc(..., SPDK_MALLOC_DMA) 로 hugepage 한 덩어리를 reserve.
 * 3) base + i*bufsize 주소들을 ring 에 enqueue 해서 풀을 "초기 가득" 상태로 만든다.
 * 실패 경로에선 partial 상태를 모두 정리하고 에러 반환.
 *
 * 호출 체인:
 *   spdk_iobuf_initialize() → [iobuf_node_initialize] → spdk_ring_create / spdk_malloc / spdk_ring_enqueue
 */
static int
iobuf_node_initialize(struct iobuf_node *node, uint32_t numa_id)
{
	struct spdk_iobuf_opts *opts = &g_iobuf.opts; /* [한국어] 풀 크기/버퍼 크기 등 옵션 단축 참조. */
	struct spdk_iobuf_buffer *buf;                /* [한국어] free buffer 헤더 캐스트 임시 변수. */
	uint64_t i;                                    /* [한국어] 풀 채우기 루프 인덱스 (pool_count 까지). */
	int rc;                                        /* [한국어] 에러 코드. */

	if (!g_iobuf.opts.enable_numa) {              /* [한국어] NUMA off 모드라면 — */
		numa_id = SPDK_ENV_NUMA_ID_ANY;        /* [한국어] DPDK 에 "어느 NUMA 든 상관없음" 으로 메모리 요청. */
	}

	node->small_pool = spdk_ring_create(SPDK_RING_TYPE_MP_MC, opts->small_pool_count,
					    numa_id);
	/* [한국어] small free buffer 색인용 lockless MP/MC ring 생성 (DPDK rte_ring 위에 SPDK 가 얹은 추상화).
	 * 모든 reactor 가 enqueue/dequeue 가능. 크기는 옵션의 small_pool_count 만큼. */
	if (!node->small_pool) {
		SPDK_ERRLOG("Failed to create small iobuf pool\n"); /* [한국어] DPDK 차원 OOM/이름 충돌 등 — 진행 불가. */
		rc = -ENOMEM;
		goto error;                                          /* [한국어] partial 자원 정리하러 점프. */
	}

	node->small_pool_base = spdk_malloc(opts->small_bufsize * opts->small_pool_count, IOBUF_ALIGNMENT,
					    NULL, numa_id, SPDK_MALLOC_DMA);
	/* [한국어] small 풀의 모든 버퍼를 한 큰 hugepage 덩어리로 reserve.
	 * SPDK_MALLOC_DMA 플래그 → DMA 가능(IOMMU 매핑됨) hugepage 위에 잡힘 → NVMe SSD 에 직접 PRP 가능.
	 * 4KB 정렬은 NVMe 1.x 의 PRP/SGL 정렬 요구를 만족시키기 위함. */
	if (node->small_pool_base == NULL) {
		SPDK_ERRLOG("Unable to allocate requested small iobuf pool size\n"); /* [한국어] hugepage 부족 — 시스템 설정 문제. */
		rc = -ENOMEM;
		goto error;
	}

	node->large_pool = spdk_ring_create(SPDK_RING_TYPE_MP_MC, opts->large_pool_count,
					    numa_id);
	/* [한국어] large 풀용 동일한 ring 생성. */
	if (!node->large_pool) {
		SPDK_ERRLOG("Failed to create large iobuf pool\n");
		rc = -ENOMEM;
		goto error;
	}

	node->large_pool_base = spdk_malloc(opts->large_bufsize * opts->large_pool_count, IOBUF_ALIGNMENT,
					    NULL, numa_id, SPDK_MALLOC_DMA);
	/* [한국어] large 풀용 hugepage 백킹 메모리. 의미는 small_pool_base 와 동일. */
	if (node->large_pool_base == NULL) {
		SPDK_ERRLOG("Unable to allocate requested large iobuf pool size\n");
		rc = -ENOMEM;
		goto error;
	}

	for (i = 0; i < opts->small_pool_count; i++) { /* [한국어] small 풀을 "처음부터 가득 찬" 상태로 만들기 위해 모든 슬롯 enqueue. */
		buf = node->small_pool_base + i * opts->small_bufsize; /* [한국어] i 번째 버퍼의 시작 주소 = base + i*bufsize (선형 슬라이스). */
		spdk_ring_enqueue(node->small_pool, (void **)&buf, 1, NULL); /* [한국어] free 색인 ring 에 한 개 push (initial 이라 항상 성공). */
	}

	for (i = 0; i < opts->large_pool_count; i++) { /* [한국어] large 풀도 동일 방식으로 풀 가득 채움. */
		buf = node->large_pool_base + i * opts->large_bufsize;
		spdk_ring_enqueue(node->large_pool, (void **)&buf, 1, NULL);
	}

	return 0; /* [한국어] 이 NUMA 노드 풀 준비 완료. */

error:
	/* [한국어] 어디서든 실패하면 이 노드의 partial 자원을 모두 정리.
	 * spdk_free / spdk_ring_free 는 NULL 입력에 안전하므로 분기 없이 모두 호출 가능. */
	spdk_free(node->small_pool_base);
	spdk_ring_free(node->small_pool);
	spdk_free(node->large_pool_base);
	spdk_ring_free(node->large_pool);
	memset(node, 0, sizeof(*node)); /* [한국어] 호출자가 이 슬롯을 다시 unused 로 인식하도록 0 초기화. */

	return rc;
}

/*
 * [한국어]
 * iobuf_node_free — 한 NUMA 노드 글로벌 풀의 자원 모두 반환.
 *
 * @node: 정리할 g_iobuf.node[i] 슬롯.
 *
 * spdk_iobuf_finish() 경로 또는 initialize 실패 정리 경로에서 호출된다.
 * 풀에 들어 있는 free buffer 개수가 처음 초기화 시점과 다르면(=어딘가에서 leak),
 * 경고만 찍고 진행한다 — 함수의 책임은 "자원을 풀어주는 것".
 *
 * 호출 체인:
 *   iobuf_unregister_cb() / spdk_iobuf_initialize() 의 err 분기 → [iobuf_node_free]
 */
static void
iobuf_node_free(struct iobuf_node *node)
{
	if (node->small_pool == NULL) {
		/* This node didn't get allocated, so just return immediately. */
		/* [한국어] 이 노드는 한 번도 활성화되지 않았음 — NUMA off 모드의 빈 슬롯 등 — 그냥 반환. */
		return;
	}

	if (spdk_ring_count(node->small_pool) != g_iobuf.opts.small_pool_count) {
		/* [한국어] 종료 시점에 풀에 환원된 small 버퍼 개수가 init 시 채운 개수와 다르면 누수 경고.
		 * 일부 모듈이 spdk_iobuf_put 을 누락한 케이스 — 디버깅 단서로만 출력. */
		SPDK_ERRLOG("small iobuf pool count is %zu, expected %"PRIu64"\n",
			    spdk_ring_count(node->small_pool), g_iobuf.opts.small_pool_count);
	}

	if (spdk_ring_count(node->large_pool) != g_iobuf.opts.large_pool_count) {
		/* [한국어] large 풀에서도 동일한 leak 검사. */
		SPDK_ERRLOG("large iobuf pool count is %zu, expected %"PRIu64"\n",
			    spdk_ring_count(node->large_pool), g_iobuf.opts.large_pool_count);
	}

	spdk_free(node->small_pool_base); /* [한국어] hugepage 백킹 메모리 반환 (DPDK heap 으로). */
	node->small_pool_base = NULL;     /* [한국어] dangling pointer 방지 + iobuf_node_free 재호출 시 idempotent 보장. */
	spdk_ring_free(node->small_pool); /* [한국어] lockless ring 자체 해제. */
	node->small_pool = NULL;           /* [한국어] 다음 호출 시 위쪽 if 가드로 빠지게 함. */

	spdk_free(node->large_pool_base);
	node->large_pool_base = NULL;
	spdk_ring_free(node->large_pool);
	node->large_pool = NULL;
}

/*
 * [한국어]
 * spdk_iobuf_initialize — 부팅 시 모든 NUMA 노드의 글로벌 풀 초기화.
 *
 * @return: 0 성공, 음수 errno (어떤 NUMA 노드든 풀 만들기 실패 시).
 *
 * 호출 시점: spdk_app_start() 내부에서 모든 모듈이 register_module() 을 마친 직후.
 * 이 함수가 끝나야 비로소 spdk_iobuf_channel_init() / get / put 호출이 의미를 가진다.
 *
 * 1) bufsize 를 4KB 정렬로 올림 — NVMe DMA 정렬 요구.
 * 2) 활성 NUMA 노드 각각에 대해 iobuf_node_initialize() 로 ring + hugepage reserve.
 * 3) spdk_io_device_register(&g_iobuf, …) 로 io_channel 인프라 등록.
 *
 * 호출 체인:
 *   spdk_app_start() / 사용자 코드 → [spdk_iobuf_initialize] → iobuf_node_initialize / spdk_io_device_register
 */
int
spdk_iobuf_initialize(void)
{
	struct spdk_iobuf_opts *opts = &g_iobuf.opts; /* [한국어] 옵션 단축 참조. */
	struct iobuf_node *node;                       /* [한국어] 순회용 노드 포인터. */
	int32_t i;                                      /* [한국어] NUMA id. */
	int rc = 0;                                     /* [한국어] 결과 코드 (성공 0). */

	/* Round up to the nearest alignment so that each element remains aligned */
	opts->small_bufsize = SPDK_ALIGN_CEIL(opts->small_bufsize, IOBUF_ALIGNMENT);
	/* [한국어] 사용자 지정 small_bufsize 를 4KB 단위로 올림 — base + i*bufsize 가 항상 4KB 정렬되도록.
	 * 정렬 불일치 시 NVMe 컨트롤러가 PRP 위반으로 거부하므로 강제. */
	opts->large_bufsize = SPDK_ALIGN_CEIL(opts->large_bufsize, IOBUF_ALIGNMENT);
	/* [한국어] large_bufsize 도 동일하게 4KB 정렬. */

	IOBUF_FOREACH_NUMA_ID(i) {                     /* [한국어] 활성 NUMA 노드 각각에 대해 — */
		node = &g_iobuf.node[i];                /* [한국어] 해당 노드의 풀 슬롯. */
		rc = iobuf_node_initialize(node, i);    /* [한국어] ring + hugepage reserve. 실패 시 partial 정리 후 음수 반환. */
		if (rc) {
			goto err;                        /* [한국어] 한 노드라도 실패하면 이미 만든 모든 노드 풀어주고 실패. */
		}
	}

	spdk_io_device_register(&g_iobuf, iobuf_channel_create_cb, iobuf_channel_destroy_cb,
				sizeof(struct iobuf_channel), "iobuf");
	/* [한국어] g_iobuf 자체를 io_device 키로 등록 — 이후 spdk_get_io_channel(&g_iobuf) 호출 가능.
	 * 채널 ctx 크기는 sizeof(iobuf_channel), 이름 "iobuf" 는 디버깅용. */
	g_iobuf_is_initialized = true;                  /* [한국어] finish() 가 io_device_unregister 를 부를 수 있게 플래그 set. */

	return 0;

err:
	IOBUF_FOREACH_NUMA_ID(i) {                     /* [한국어] 실패 정리: 이미 만들어진 노드들도 모두 풀어줌 (idempotent 가드 있음). */
		node = &g_iobuf.node[i];
		iobuf_node_free(node);
	}
	return rc;
}

/*
 * [한국어]
 * iobuf_unregister_cb — io_device unregister 가 모두 끝난 시점에 SPDK 가 호출하는 콜백.
 *
 * @io_device: &g_iobuf. 미사용.
 *
 * 1) 모든 등록 모듈을 free.
 * 2) 모든 NUMA 노드 풀(ring + hugepage) 해제.
 * 3) spdk_iobuf_finish() 가 들고 있던 사용자 종료 콜백을 마지막으로 호출 — 이때서야
 *    "iobuf 가 완전히 종료됐다" 가 보장된다.
 *
 * 호출 체인:
 *   spdk_iobuf_finish() → spdk_io_device_unregister(…, iobuf_unregister_cb)
 *     → 모든 채널 정리 끝나면 → [iobuf_unregister_cb] → 사용자 finish_cb
 */
static void
iobuf_unregister_cb(void *io_device)
{
	struct iobuf_module *module; /* [한국어] 모듈 리스트 순회용. */
	struct iobuf_node *node;     /* [한국어] NUMA 노드 풀 정리용. */
	int32_t i;                    /* [한국어] NUMA id. */

	while (!TAILQ_EMPTY(&g_iobuf.modules)) {                 /* [한국어] 등록된 모든 모듈을 — */
		module = TAILQ_FIRST(&g_iobuf.modules);           /* [한국어] 리스트 head 부터 — */
		TAILQ_REMOVE(&g_iobuf.modules, module, tailq);    /* [한국어] 리스트에서 떼고 — */
		free(module->name);                                /* [한국어] strdup 한 이름 해제. */
		free(module);                                       /* [한국어] 노드 자체 해제. */
	}

	IOBUF_FOREACH_NUMA_ID(i) {                              /* [한국어] 모든 NUMA 풀(ring + hugepage) 반환. */
		node = &g_iobuf.node[i];
		iobuf_node_free(node);
	}

	if (g_iobuf.finish_cb != NULL) {                        /* [한국어] 사용자가 finish 콜백을 등록해 뒀다면 — */
		g_iobuf.finish_cb(g_iobuf.finish_arg);          /* [한국어] 이제 모두 정리됐으니 호출 — 이 시점에 application 도 다음 단계로. */
	}
}

/*
 * [한국어]
 * spdk_iobuf_finish — iobuf 서브시스템 종료를 시작한다.
 *
 * @cb_fn : 모든 정리 후 호출될 사용자 완료 콜백.
 * @cb_arg: cb_fn 컨텍스트.
 *
 * spdk_io_device_unregister() 가 모든 reactor 위 채널을 destroy 한 뒤
 * iobuf_unregister_cb 를 부르는 비동기 흐름이라, 이 함수는 즉시 반환한다.
 * iobuf 가 한 번도 init 안 됐으면 cb_fn 만 그 자리에서 호출하고 리턴.
 *
 * 호출 체인:
 *   spdk_app_stop() / 사용자 종료 코드 → [spdk_iobuf_finish] → spdk_io_device_unregister
 *     → (각 reactor) iobuf_channel_destroy_cb → iobuf_unregister_cb → cb_fn
 */
void
spdk_iobuf_finish(spdk_iobuf_finish_cb cb_fn, void *cb_arg)
{
	if (!g_iobuf_is_initialized) {           /* [한국어] init 안 된 상태라면 풀 자원이 없으니 — */
		cb_fn(cb_arg);                    /* [한국어] 그 자리에서 사용자 콜백만 호출하고 리턴 (멱등성 보장). */
		return;
	}

	g_iobuf_is_initialized = false;          /* [한국어] 이중 finish 방지. */
	g_iobuf.finish_cb = cb_fn;                /* [한국어] iobuf_unregister_cb 가 마지막에 부를 수 있게 보관. */
	g_iobuf.finish_arg = cb_arg;

	spdk_io_device_unregister(&g_iobuf, iobuf_unregister_cb);
	/* [한국어] io_device 해제 시작. 이건 비동기 — 모든 reactor 의 채널 destroy 가 끝난 뒤에야 unregister_cb 가 불린다. */
}

/*
 * [한국어]
 * spdk_iobuf_set_opts — 옵션을 사용자 값으로 갱신.
 *
 * @opts: 사용자가 채워 준 spdk_iobuf_opts (opts_size 로 ABI 호환 처리).
 * @return: 0 성공, -1 / -EINVAL 검증 실패.
 *
 * spdk_iobuf_initialize() 호출 이전에만 의미가 있다 — initialize 후엔 이미 풀이 만들어졌다.
 * opts_size 는 SPDK ABI 의 forward/backward 호환을 위한 트릭: 새 필드가 추가돼도
 * 옛 호출자의 작은 opts 를 받아 일부 필드만 갱신할 수 있다.
 *
 * 호출 체인:
 *   사용자 init 코드 / RPC 핸들러 → [spdk_iobuf_set_opts]
 */
int
spdk_iobuf_set_opts(const struct spdk_iobuf_opts *opts)
{
	if (!opts) {                                                 /* [한국어] NULL 입력 방어 — 흔한 실수 잡기. */
		SPDK_ERRLOG("opts cannot be NULL\n");
		return -1;
	}

	if (!opts->opts_size) {                                      /* [한국어] opts_size 0 이면 어디까지 채웠는지 알 수 없음 — 거부. */
		SPDK_ERRLOG("opts_size inside opts cannot be zero value\n");
		return -1;
	}

	if (opts->small_pool_count < IOBUF_MIN_SMALL_POOL_SIZE) {    /* [한국어] small 풀이 너무 작으면 channel cache 채우기조차 실패할 수 있음. */
		SPDK_ERRLOG("small_pool_count must be at least %" PRIu32 "\n",
			    IOBUF_MIN_SMALL_POOL_SIZE);
		return -EINVAL;
	}
	if (opts->large_pool_count < IOBUF_MIN_LARGE_POOL_SIZE) {    /* [한국어] large 풀도 마찬가지로 하한 검사. */
		SPDK_ERRLOG("large_pool_count must be at least %" PRIu32 "\n",
			    IOBUF_MIN_LARGE_POOL_SIZE);
		return -EINVAL;
	}

	if (opts->small_bufsize < IOBUF_MIN_SMALL_BUFSIZE) {         /* [한국어] small 버퍼 한 개 크기가 너무 작으면 헤더(spdk_iobuf_buffer)도 못 들어감. */
		SPDK_ERRLOG("small_bufsize must be at least %" PRIu32 "\n",
			    IOBUF_MIN_SMALL_BUFSIZE);
		return -EINVAL;
	}

	if (opts->large_bufsize < IOBUF_MIN_LARGE_BUFSIZE) {         /* [한국어] large 버퍼 하한 검사. */
		SPDK_ERRLOG("large_bufsize must be at least %" PRIu32 "\n",
			    IOBUF_MIN_LARGE_BUFSIZE);
		return -EINVAL;
	}

	if (opts->enable_numa &&
	    spdk_env_get_last_numa_id() >= SPDK_CONFIG_MAX_NUMA_NODES) {
		/* [한국어] NUMA 활성을 켜고 싶지만 시스템의 마지막 NUMA id 가 컴파일타임 상한을 넘어가면 거부.
		 * iobuf_channel.node[] / iobuf_node[] 배열이 컴파일타임 크기라 빌드 옵션을 다시 설정해야 함. */
		SPDK_ERRLOG("max NUMA ID %" PRIu32 " cannot be supported with "
			    "SPDK_CONFIG_MAX_NUMA_NODES %" PRIu32 "\n",
			    spdk_env_get_last_numa_id(), SPDK_CONFIG_MAX_NUMA_NODES);
		SPDK_ERRLOG("Re-configure with --max-numa-nodes=%" PRIu32 "\n",
			    spdk_env_get_last_numa_id() + 1);
		return -EINVAL;
	}

#define SET_FIELD(field) \
        if (offsetof(struct spdk_iobuf_opts, field) + sizeof(opts->field) <= opts->opts_size) { \
                g_iobuf.opts.field = opts->field; \
        } \
	/* [한국어] opts_size 가 해당 필드 끝까지 포함하는 경우에만 복사 — 옛 호출자/신 호출자 ABI 호환. */

	SET_FIELD(small_pool_count); /* [한국어] small 풀 엔트리 개수 갱신. */
	SET_FIELD(large_pool_count); /* [한국어] large 풀 엔트리 개수 갱신. */
	SET_FIELD(small_bufsize);    /* [한국어] small 버퍼 한 개 크기 갱신. */
	SET_FIELD(large_bufsize);    /* [한국어] large 버퍼 한 개 크기 갱신. */
	SET_FIELD(enable_numa);      /* [한국어] NUMA 분리 활성 여부 갱신. */

	g_iobuf.opts.opts_size = opts->opts_size; /* [한국어] 다음 get_opts 호출에서 어디까지 유효한지 알 수 있게 저장. */

#undef SET_FIELD

	return 0;
}

/*
 * [한국어]
 * spdk_iobuf_get_opts — 현재 적용 중인 옵션을 사용자 버퍼에 복사.
 *
 * @opts     : 사용자 출력 버퍼.
 * @opts_size: 사용자 버퍼 크기 (sizeof 로 채워야 정확).
 *
 * set_opts 와 마찬가지로 opts_size 기반 ABI 호환 트릭을 쓴다.
 * 끝의 SPDK_STATIC_ASSERT 는 새 필드 추가 시 SET_FIELD 매크로 호출도 같이
 * 추가해야 함을 컴파일러에게 강제하는 안전장치.
 */
void
spdk_iobuf_get_opts(struct spdk_iobuf_opts *opts, size_t opts_size)
{
	if (!opts) {                                  /* [한국어] NULL 출력 버퍼 방어. */
		SPDK_ERRLOG("opts should not be NULL\n");
		return;
	}

	if (!opts_size) {                              /* [한국어] 0 크기 방어 — 어디까지 쓸지 알 수 없음. */
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return;
	}

	opts->opts_size = opts_size;                   /* [한국어] 호출자가 받은 버퍼 크기로 출력 구조체의 ABI 식별자 표시. */

#define SET_FIELD(field) \
	if (offsetof(struct spdk_iobuf_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = g_iobuf.opts.field; \
	} \
	/* [한국어] 사용자 버퍼가 이 필드 끝까지 포함할 수 있을 때만 복사 — set_opts 와 동일한 호환 처리. */

	SET_FIELD(small_pool_count); /* [한국어] 현재 small 풀 크기. */
	SET_FIELD(large_pool_count); /* [한국어] 현재 large 풀 크기. */
	SET_FIELD(small_bufsize);    /* [한국어] 현재 small 버퍼 크기. */
	SET_FIELD(large_bufsize);    /* [한국어] 현재 large 버퍼 크기. */
	SET_FIELD(enable_numa);      /* [한국어] NUMA 활성 여부. */

#undef SET_FIELD

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	/* [한국어] 새 필드를 추가하면 sizeof 가 바뀌어 이 assert 가 깨진다 — 그때 위 SET_FIELD 도 같이 업데이트하라는 강제. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_iobuf_opts) == 40, "Incorrect size");
}

/*
 * [한국어]
 * iobuf_channel_node_init — 한 모듈의 한 NUMA 슬롯에 대한 cache 메타데이터 초기화.
 *
 * @ch              : 모듈이 받는 spdk_iobuf_channel.
 * @iobuf_ch        : 본 reactor 의 io_channel ctx (=struct iobuf_channel).
 * @numa_id         : 초기화할 NUMA 슬롯 id.
 * @small_cache_size: 사용자가 요청한 per-thread small 캐시 목표 크기.
 * @large_cache_size: 사용자가 요청한 per-thread large 캐시 목표 크기.
 *
 * 이 함수는 메타데이터(포인터/크기/카운터) 만 채우고, 실제 버퍼 dequeue 는
 * 뒤에 오는 iobuf_channel_node_populate() 가 한다.
 *
 * 호출 체인:
 *   spdk_iobuf_channel_init() → [iobuf_channel_node_init]
 */
static void
iobuf_channel_node_init(struct spdk_iobuf_channel *ch, struct iobuf_channel *iobuf_ch,
			int32_t numa_id, uint32_t small_cache_size, uint32_t large_cache_size)
{
	struct iobuf_node *node = &g_iobuf.node[numa_id];          /* [한국어] 이 NUMA 슬롯의 글로벌 풀 — cache miss 시 fetch 대상. */
	struct spdk_iobuf_node_cache *cache = &ch->cache[numa_id]; /* [한국어] 채울 채널 측 cache 슬롯. */
	struct iobuf_channel_node *ch_node = &iobuf_ch->node[numa_id]; /* [한국어] reactor 가 공유하는 wait queue 묶음. */

	cache->small.queue = &ch_node->small_queue; /* [한국어] cache 측 .queue 가 reactor 공유 wait queue 를 가리키도록 link. */
	cache->large.queue = &ch_node->large_queue; /* [한국어] large 도 동일 link. */
	cache->small.pool = node->small_pool;        /* [한국어] cache miss 시 fetch 할 글로벌 small ring 핸들. */
	cache->large.pool = node->large_pool;        /* [한국어] 동일하게 large ring 핸들. */
	cache->small.bufsize = g_iobuf.opts.small_bufsize; /* [한국어] get/put 에서 length → small/large 분류 기준. */
	cache->large.bufsize = g_iobuf.opts.large_bufsize; /* [한국어] 동일. */
	cache->small.cache_size = small_cache_size;        /* [한국어] 사용자가 요청한 per-thread cache 목표 크기. */
	cache->large.cache_size = large_cache_size;        /* [한국어] 동일. */
	cache->small.cache_count = 0;                       /* [한국어] 아직 populate 안 했으니 0. populate 가 끝나면 cache_size 와 일치. */
	cache->large.cache_count = 0;

	STAILQ_INIT(&cache->small.cache); /* [한국어] free buffer 캐시 STAILQ 초기화. populate 가 채울 예정. */
	STAILQ_INIT(&cache->large.cache);
}

/*
 * [한국어]
 * iobuf_channel_node_populate — 글로벌 풀에서 small/large 각각 cache_size 만큼 dequeue 해 채우기.
 *
 * @ch     : 채울 채널.
 * @name   : 모듈 이름 (에러 로깅용).
 * @numa_id: 처리할 NUMA 슬롯.
 * @return : 0 성공, -ENOMEM 글로벌 풀이 부족해 cache 채우기 실패.
 *
 * IOBUF_POPULATE_BATCH_SIZE 단위로 spdk_ring_dequeue 를 호출해 채우다가, 한 번이라도
 * 요청 개수보다 적게 dequeue 되면 풀이 정말 부족하다는 뜻이라 즉시 실패 반환.
 *
 * 호출 체인:
 *   spdk_iobuf_channel_init() → [iobuf_channel_node_populate]
 *                                → spdk_ring_dequeue (batch)
 */
static int
iobuf_channel_node_populate(struct spdk_iobuf_channel *ch, const char *name, int32_t numa_id)
{
	struct iobuf_node *node = &g_iobuf.node[numa_id];           /* [한국어] 이 NUMA 의 글로벌 풀(ring) 핸들. */
	struct spdk_iobuf_node_cache *cache = &ch->cache[numa_id];  /* [한국어] 채울 채널 측 cache. */
	uint32_t small_cache_size = cache->small.cache_size;        /* [한국어] 채워야 할 small 목표 (init 단계에서 저장됨). */
	uint32_t large_cache_size = cache->large.cache_size;        /* [한국어] large 목표. */
	void *bufs[IOBUF_POPULATE_BATCH_SIZE];                       /* [한국어] 한 번 dequeue 에 받을 임시 배치 배열. */
	uint32_t i, remaining, count, dequeued;                      /* [한국어] 루프 변수들. */

	remaining = small_cache_size;                                /* [한국어] 아직 채워야 할 small 개수. */
	while (remaining > 0) {
		count = spdk_min(remaining, IOBUF_POPULATE_BATCH_SIZE); /* [한국어] 이번 batch 에서 시도할 개수 — 남은 양과 BATCH 중 작은 값. */
		dequeued = spdk_ring_dequeue(node->small_pool, bufs, count);
		/* [한국어] 글로벌 small ring 에서 batch 만큼 lockless dequeue.
		 * 풀이 부족하면 dequeued < count 가 될 수 있다. */
		for (i = 0; i < dequeued; ++i) {
			STAILQ_INSERT_TAIL(&cache->small.cache, (struct spdk_iobuf_buffer *)bufs[i], stailq);
			/* [한국어] dequeue 한 버퍼들을 채널 cache STAILQ 에 차례로 push. INSERT_TAIL 로 NUMA-local 우선순위 유지. */
		}

		cache->small.cache_count += dequeued;                /* [한국어] 채널 cache 카운터 증가. */
		remaining -= dequeued;                                /* [한국어] 남은 목표 감소. */
		if (dequeued != count) {                              /* [한국어] 요청보다 적게 받았다는 건 풀이 진짜 부족하다는 뜻. */
			SPDK_ERRLOG("Failed to populate '%s' iobuf small buffer cache at %d/%d entries. "
				    "You may need to increase spdk_iobuf_opts.small_pool_count (%"PRIu64")\n",
				    name, cache->small.cache_count, small_cache_size, g_iobuf.opts.small_pool_count);
			SPDK_ERRLOG("See scripts/calc-iobuf.py for guidance on how to calculate "
				    "this value.\n");
			return -ENOMEM;                                /* [한국어] 호출자가 channel_init 을 실패시키고 fini 로 정리하도록 알림. */
		}
	}

	assert(remaining == 0);                                       /* [한국어] 위 루프가 정상 종료했으면 반드시 0. */
	remaining = large_cache_size;                                 /* [한국어] 동일한 알고리즘을 large 에 대해 반복. */
	while (remaining > 0) {
		count = spdk_min(remaining, IOBUF_POPULATE_BATCH_SIZE);
		dequeued = spdk_ring_dequeue(node->large_pool, bufs, count); /* [한국어] 글로벌 large ring 에서 batch dequeue. */
		for (i = 0; i < dequeued; ++i) {
			STAILQ_INSERT_TAIL(&cache->large.cache, (struct spdk_iobuf_buffer *)bufs[i], stailq);
		}

		cache->large.cache_count += dequeued;
		remaining -= dequeued;
		if (dequeued != count) {
			SPDK_ERRLOG("Failed to populate '%s' iobuf large buffer cache at %d/%d entries. "
				    "You may need to increase spdk_iobuf_opts.large_pool_count (%"PRIu64")\n",
				    name, cache->large.cache_count, large_cache_size, g_iobuf.opts.large_pool_count);
			SPDK_ERRLOG("See scripts/calc-iobuf.py for guidance on how to calculate "
				    "this value.\n");
			return -ENOMEM;
		}
	}

	assert(remaining == 0);                                       /* [한국어] large 도 정상 종료 검증. */
	return 0;
}

/*
 * [한국어]
 * spdk_iobuf_channel_init — 모듈이 자기 reactor 위에 per-thread 캐시를 만든다.
 *
 * @ch              : 모듈이 들고 있을 채널 핸들 (보통 모듈 내부 구조체에 임베드).
 * @name            : register_module 시 등록한 모듈 이름.
 * @small_cache_size: 이 채널이 들고 있을 small 캐시 개수.
 * @large_cache_size: 이 채널이 들고 있을 large 캐시 개수.
 * @return          : 0 성공, -ENODEV 모듈 미등록, -ENOMEM 풀 부족 / 채널 슬롯 풀.
 *
 * 1) 등록된 모듈인지 검증.
 * 2) reactor 의 io_channel 을 잡고 ctx(iobuf_channel) 안의 빈 슬롯에 자기 등록.
 * 3) NUMA 슬롯 별 cache 메타 init → 글로벌 ring 에서 batch dequeue 해 cache populate.
 * 실행 컨텍스트: 호출자 SPDK thread (=lcore 고정 reactor). 락 없음.
 *
 * 호출 체인:
 *   bdev/NVMe-oF/iSCSI module init → [spdk_iobuf_channel_init]
 *     → spdk_get_io_channel(&g_iobuf) → iobuf_channel_create_cb → populate
 */
int
spdk_iobuf_channel_init(struct spdk_iobuf_channel *ch, const char *name,
			uint32_t small_cache_size, uint32_t large_cache_size)
{
	struct spdk_io_channel *ioch;     /* [한국어] reactor 의 io_channel 객체. */
	struct iobuf_channel *iobuf_ch;   /* [한국어] io_channel 안에 살아있는 ctx — reactor 공유 wait queue. */
	struct iobuf_module *module;      /* [한국어] 등록된 모듈 검색 결과. */
	uint32_t i;                        /* [한국어] 채널 슬롯 / 일반 인덱스. */
	int32_t numa_id;                   /* [한국어] NUMA 순회 변수 (음수형 SPDK 표현 가능). */
	int rc;                            /* [한국어] 결과 코드. */

	TAILQ_FOREACH(module, &g_iobuf.modules, tailq) {       /* [한국어] g_iobuf.modules 에서 이름 일치하는 모듈 검색. */
		if (strcmp(name, module->name) == 0) {
			break;                                  /* [한국어] 발견 — module 포인터 그대로 둠. */
		}
	}

	if (module == NULL) {                                   /* [한국어] register_module 을 안 한 모듈은 채널을 만들 수 없음. */
		SPDK_ERRLOG("Couldn't find iobuf module: '%s'\n", name);
		return -ENODEV;
	}

	ioch = spdk_get_io_channel(&g_iobuf);                  /* [한국어] reactor 의 iobuf io_channel 획득 — 처음이면 create_cb 자동 호출. */
	if (ioch == NULL) {
		SPDK_ERRLOG("Couldn't get iobuf IO channel\n");
		return -ENOMEM;
	}

	iobuf_ch = spdk_io_channel_get_ctx(ioch);              /* [한국어] io_channel ctx 영역(=struct iobuf_channel) 추출. */

	for (i = 0; i < IOBUF_MAX_CHANNELS; ++i) {             /* [한국어] reactor 위 채널 약한 참조 슬롯 첫 번째 빈 칸을 찾는다. */
		if (iobuf_ch->channels[i] == NULL) {
			iobuf_ch->channels[i] = ch;             /* [한국어] 자기 자신을 등록 — stats 수집 시 이 배열로 모든 채널 발견. */
			break;
		}
	}

	if (i == IOBUF_MAX_CHANNELS) {                         /* [한국어] 모든 슬롯이 가득 찼다 — 한 reactor 에 너무 많은 모듈 채널. */
		SPDK_ERRLOG("Max number of iobuf channels (%" PRIu32 ") exceeded.\n", i);
		rc = -ENOMEM;
		goto error;
	}

	ch->parent = ioch;                                     /* [한국어] put 시 spdk_put_io_channel 호출하기 위한 부모 io_channel 보관. */
	ch->module = module;                                   /* [한국어] waiter 가 어느 모듈 소속인지 식별하는 키. */

	IOBUF_FOREACH_NUMA_ID(numa_id) {                       /* [한국어] 활성 NUMA 슬롯 각각에 대해 cache 메타 초기화 (포인터/크기). */
		iobuf_channel_node_init(ch, iobuf_ch, numa_id,
					small_cache_size, large_cache_size);
	}

	IOBUF_FOREACH_NUMA_ID(numa_id) {                       /* [한국어] 메타 초기화가 끝난 후 실제 cache populate (글로벌 풀 → cache). */
		rc = iobuf_channel_node_populate(ch, name, numa_id);
		if (rc) {
			goto error;                              /* [한국어] populate 실패 — 이미 채워둔 부분도 fini 가 환원. */
		}
	}

	return 0;
error:
	spdk_iobuf_channel_fini(ch);                            /* [한국어] partial 자원(cache 일부 + 슬롯) 모두 정리하고 실패 반환. */

	return rc;
}

/*
 * [한국어]
 * iobuf_channel_node_fini — 한 NUMA 슬롯의 cache 환원 + waiter 무결성 검증.
 *
 * @ch     : 정리할 채널.
 * @numa_id: 처리할 NUMA 슬롯.
 *
 * 1) wait queue 에 남은 엔트리 중 자기 모듈 소유가 있으면 버그(assert).
 * 2) cache STAILQ 에 남은 free buffer 들을 모두 글로벌 ring 으로 환원.
 * 실행 컨텍스트: 호출자 reactor 단일 스레드.
 *
 * 호출 체인:
 *   spdk_iobuf_channel_fini() → [iobuf_channel_node_fini]
 *                              → spdk_ring_enqueue (cache 환원)
 */
static void
iobuf_channel_node_fini(struct spdk_iobuf_channel *ch, int32_t numa_id)
{
	struct spdk_iobuf_node_cache *cache = &ch->cache[numa_id];   /* [한국어] 정리 대상 cache 슬롯. */
	struct iobuf_node *node = &g_iobuf.node[numa_id];             /* [한국어] cache 를 환원할 글로벌 ring. */
	struct spdk_iobuf_entry *entry __attribute__((unused));       /* [한국어] release 빌드의 unused 경고 차단. */
	struct spdk_iobuf_buffer *buf;                                /* [한국어] cache pop 임시 변수. */

	/* Make sure none of the wait queue entries are coming from this module */
	STAILQ_FOREACH(entry, cache->small.queue, stailq) {           /* [한국어] reactor 공유 small wait queue 의 모든 엔트리 — */
		assert(entry->module != ch->module);                  /* [한국어] 내 모듈 소유면 spdk_iobuf_entry_abort 누락 — 버그. */
	}
	STAILQ_FOREACH(entry, cache->large.queue, stailq) {           /* [한국어] large 도 동일 검증. */
		assert(entry->module != ch->module);
	}

	/* Release cached buffers back to the pool */
	while (!STAILQ_EMPTY(&cache->small.cache)) {                  /* [한국어] cache 에 남아있는 모든 small free buffer 를 — */
		buf = STAILQ_FIRST(&cache->small.cache);              /* [한국어] head 부터 pop — */
		STAILQ_REMOVE_HEAD(&cache->small.cache, stailq);
		spdk_ring_enqueue(node->small_pool, (void **)&buf, 1, NULL); /* [한국어] 글로벌 ring 에 환원 (다른 reactor 가 받을 수 있게). */
		cache->small.cache_count--;
	}
	while (!STAILQ_EMPTY(&cache->large.cache)) {                  /* [한국어] large 도 동일하게 환원. */
		buf = STAILQ_FIRST(&cache->large.cache);
		STAILQ_REMOVE_HEAD(&cache->large.cache, stailq);
		spdk_ring_enqueue(node->large_pool, (void **)&buf, 1, NULL);
		cache->large.cache_count--;
	}

	assert(cache->small.cache_count == 0);                         /* [한국어] 모두 환원했으면 카운트 0. */
	assert(cache->large.cache_count == 0);
}

/*
 * [한국어]
 * spdk_iobuf_channel_fini — 모듈이 자기 채널을 종료한다.
 *
 * @ch: 종료할 채널.
 *
 * NUMA 슬롯 별 cache 환원 → reactor ctx 의 channels[] 슬롯 비움 →
 * spdk_put_io_channel 으로 io_channel ref 감소(마지막이면 destroy_cb 트리거).
 *
 * 호출 체인:
 *   bdev/NVMe-oF module deinit → [spdk_iobuf_channel_fini] → spdk_put_io_channel
 */
void
spdk_iobuf_channel_fini(struct spdk_iobuf_channel *ch)
{
	struct iobuf_channel *iobuf_ch; /* [한국어] reactor io_channel ctx 추출용. */
	uint32_t i;                      /* [한국어] 슬롯 인덱스. */

	IOBUF_FOREACH_NUMA_ID(i) {       /* [한국어] 모든 NUMA 슬롯의 cache 를 글로벌 풀로 환원. */
		iobuf_channel_node_fini(ch, i);
	}

	iobuf_ch = spdk_io_channel_get_ctx(ch->parent);          /* [한국어] reactor 공유 ctx 획득. */
	for (i = 0; i < IOBUF_MAX_CHANNELS; ++i) {               /* [한국어] 자기 약한 참조 슬롯 찾아서 비움. */
		if (iobuf_ch->channels[i] == ch) {
			iobuf_ch->channels[i] = NULL;
			break;
		}
	}

	spdk_put_io_channel(ch->parent);                          /* [한국어] io_channel ref 감소 — 마지막이면 destroy_cb 트리거. */
	ch->parent = NULL;                                         /* [한국어] dangling 방지. */
}

/*
 * [한국어]
 * spdk_iobuf_register_module — 모듈 이름을 글로벌 리스트에 등록.
 *
 * @name  : 등록할 모듈 이름 (strdup 으로 복사됨).
 * @return: 0 성공, -EEXIST 중복, -ENOMEM 메모리 부족.
 *
 * 모듈은 부팅 시 한 번만 등록한다고 가정하므로 락 없이 안전.
 * 등록 후 channel_init 에서 이 이름으로 모듈을 찾아 ch->module 에 연결한다.
 *
 * 호출 체인:
 *   각 모듈 init (예: bdev_register_module 등가) → [spdk_iobuf_register_module]
 */
int
spdk_iobuf_register_module(const char *name)
{
	struct iobuf_module *module; /* [한국어] 검색/생성할 모듈 노드. */

	TAILQ_FOREACH(module, &g_iobuf.modules, tailq) {  /* [한국어] 같은 이름이 이미 있는지 선형 검색. */
		if (strcmp(name, module->name) == 0) {
			return -EEXIST;                    /* [한국어] 중복 등록 거부 — 호출자 버그. */
		}
	}

	module = calloc(1, sizeof(*module));               /* [한국어] 모듈 노드를 0-init heap 에 잡음. */
	if (module == NULL) {
		return -ENOMEM;
	}

	module->name = strdup(name);                       /* [한국어] 호출자 문자열 수명에 의존하지 않도록 자체 복사. */
	if (module->name == NULL) {
		free(module);                              /* [한국어] strdup 실패 — 노드도 같이 풀고 실패. */
		return -ENOMEM;
	}

	TAILQ_INSERT_TAIL(&g_iobuf.modules, module, tailq); /* [한국어] 글로벌 리스트 끝에 등록. 락 없음(부팅 단일 스레드 가정). */

	return 0;
}

/*
 * [한국어]
 * spdk_iobuf_unregister_module — 등록 해제 (이름으로 검색해 free).
 *
 * @name  : 해제할 모듈 이름.
 * @return: 0 성공, -ENOENT 미등록.
 *
 * 보통 spdk_iobuf_finish 가 일괄 정리하지만, 모듈이 동적으로 빠질 수 있는 경우 직접 호출.
 */
int
spdk_iobuf_unregister_module(const char *name)
{
	struct iobuf_module *module; /* [한국어] 검색 변수. */

	TAILQ_FOREACH(module, &g_iobuf.modules, tailq) {     /* [한국어] 이름 일치 선형 검색. */
		if (strcmp(name, module->name) == 0) {
			TAILQ_REMOVE(&g_iobuf.modules, module, tailq); /* [한국어] 리스트에서 떼어내고 — */
			free(module->name);                            /* [한국어] strdup 한 이름 해제. */
			free(module);                                   /* [한국어] 노드 자체 해제. */
			return 0;
		}
	}

	return -ENOENT;                                       /* [한국어] 등록된 적 없는 이름. */
}

/*
 * [한국어]
 * iobuf_pool_for_each_entry — 한 풀(small or large)의 wait queue 를 자기 모듈 한정 순회.
 *
 * @ch    : 순회 주체 채널 (자기 모듈 식별 키 ch->module).
 * @pool  : 순회 대상 (small or large pool_cache).
 * @cb_fn : 각 매칭 엔트리에 대해 호출할 콜백.
 * @cb_ctx: 콜백 컨텍스트.
 * @return: 0 성공 / cb_fn 이 0 이외 반환하면 즉시 그 값으로 종료.
 *
 * STAILQ_FOREACH_SAFE 를 쓰는 이유: cb_fn 이 entry 를 abort/제거할 가능성 있음.
 *
 * 호출 체인:
 *   spdk_iobuf_for_each_entry() → [iobuf_pool_for_each_entry] → cb_fn
 */
static int
iobuf_pool_for_each_entry(struct spdk_iobuf_channel *ch, struct spdk_iobuf_pool_cache *pool,
			  spdk_iobuf_for_each_entry_fn cb_fn, void *cb_ctx)
{
	struct spdk_iobuf_entry *entry, *tmp; /* [한국어] tmp 는 next-step 보관용 — cb_fn 이 entry 를 떼도 안전하게 다음 진행. */
	int rc;                                /* [한국어] 콜백 결과. */

	STAILQ_FOREACH_SAFE(entry, pool->queue, stailq, tmp) { /* [한국어] reactor 공유 wait queue 를 SAFE 모드로 순회. */
		/* We only want to iterate over the entries requested by the module which owns ch */
		if (entry->module != ch->module) {              /* [한국어] 다른 모듈 소유 엔트리는 건너뜀 — 자기 모듈만 책임. */
			continue;
		}

		rc = cb_fn(ch, entry, cb_ctx);                  /* [한국어] 자기 모듈 엔트리만 콜백에 전달 (예: 강제 abort, 통계 등). */
		if (rc != 0) {
			return rc;                               /* [한국어] non-zero 는 "더 이상 순회 말고 끝내라" 신호. */
		}
	}

	return 0;
}

/*
 * [한국어]
 * spdk_iobuf_for_each_entry — 모든 NUMA 슬롯의 small/large wait queue 를 자기 모듈 한정 순회.
 *
 * 모듈이 종료 시 "내 wait queue 엔트리들 모두 강제 종료" 같은 흐름에 사용.
 */
int
spdk_iobuf_for_each_entry(struct spdk_iobuf_channel *ch,
			  spdk_iobuf_for_each_entry_fn cb_fn, void *cb_ctx)
{
	struct spdk_iobuf_node_cache *cache; /* [한국어] 순회 중 cache 슬롯 포인터. */
	uint32_t i;                           /* [한국어] NUMA id. */
	int rc;                                /* [한국어] 콜백 결과. */

	IOBUF_FOREACH_NUMA_ID(i) {            /* [한국어] 모든 활성 NUMA 슬롯에 대해 — */
		cache = &ch->cache[i];

		rc = iobuf_pool_for_each_entry(ch, &cache->small, cb_fn, cb_ctx); /* [한국어] small wait queue 순회. */
		if (rc != 0) {
			return rc;
		}
		rc = iobuf_pool_for_each_entry(ch, &cache->large, cb_fn, cb_ctx); /* [한국어] large wait queue 순회. */
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

/*
 * [한국어]
 * iobuf_entry_abort_node — 한 NUMA 슬롯의 wait queue 에서 특정 entry 를 떼어낸다.
 *
 * @ch     : 채널 (small/large 분류용 cache 보유).
 * @numa_id: 순회할 NUMA 슬롯.
 * @entry  : 떼어낼 대상 (포인터 비교).
 * @len    : 원래 요청 길이 — small/large 어느 큐에 매달렸는지 결정.
 * @return : true 발견·제거, false 못 찾음.
 *
 * 호출자가 더 이상 콜백을 받고 싶지 않을 때(예: I/O 가 다른 경로로 취소됨) 사용.
 *
 * 호출 체인:
 *   spdk_iobuf_entry_abort() → [iobuf_entry_abort_node]
 */
static bool
iobuf_entry_abort_node(struct spdk_iobuf_channel *ch, int32_t numa_id,
		       struct spdk_iobuf_entry *entry, uint64_t len)
{
	struct spdk_iobuf_node_cache *cache; /* [한국어] 이 NUMA 슬롯의 cache. */
	struct spdk_iobuf_pool_cache *pool;  /* [한국어] small or large 중 선택된 풀. */
	struct spdk_iobuf_entry *e;           /* [한국어] STAILQ 순회용. */

	cache = &ch->cache[numa_id];

	if (len <= cache->small.bufsize) {    /* [한국어] 요청 길이가 small 이내면 small wait queue 에 매달려 있었음. */
		pool = &cache->small;
	} else {
		assert(len <= cache->large.bufsize); /* [한국어] large 보다 큰 요청은 처음부터 거부됐어야 함 — 호출자 버그 검증. */
		pool = &cache->large;
	}

	STAILQ_FOREACH(e, pool->queue, stailq) { /* [한국어] 해당 wait queue 선형 검색 (보통 짧으니 OK). */
		if (e == entry) {                /* [한국어] 포인터 동일성 비교 — 호출자가 같은 entry 객체를 넘겼음. */
			STAILQ_REMOVE(pool->queue, entry, spdk_iobuf_entry, stailq); /* [한국어] 큐에서 제거. */
			return true;
		}
	}

	return false; /* [한국어] 이미 디스패치됐거나 처음부터 들어가 있지 않음. */
}

/*
 * [한국어]
 * spdk_iobuf_entry_abort — 모든 NUMA 슬롯에서 entry 떼어내기 시도.
 */
void
spdk_iobuf_entry_abort(struct spdk_iobuf_channel *ch, struct spdk_iobuf_entry *entry,
		       uint64_t len)
{
	uint32_t i; /* [한국어] NUMA id. */

	IOBUF_FOREACH_NUMA_ID(i) {                    /* [한국어] 어느 NUMA 슬롯 큐에 매달렸는지 모르므로 전부 시도. */
		iobuf_entry_abort_node(ch, i, entry, len);
	}
}

/* [한국어] get/put 에서 글로벌 ring 과 cache 사이를 한 번에 옮기는 batch 크기. */
#define IOBUF_BATCH_SIZE 32

/*
 * [한국어]
 * spdk_iobuf_get — DMA 가능한 데이터 버퍼 한 개 획득 (hot path).
 *
 * @ch    : 호출 모듈의 채널.
 * @len   : 필요한 버퍼 길이 — small/large 분류 기준.
 * @entry : 풀이 비어있을 때 매달릴 wait 엔트리. NULL 이면 wait 없이 NULL 즉시 반환.
 * @cb_fn : 풀에 자리가 생겼을 때 호출될 콜백 (entry 와 함께 사용).
 * @return: 데이터 버퍼 포인터 / NULL (즉시 만족 불가 — wait 등록됨 또는 빈 entry).
 *
 * 동작:
 *   1) per-thread cache STAILQ head pop → cache hit (pool->stats.cache++).
 *   2) cache 비면 글로벌 ring 에서 batch dequeue (pool->stats.main++).
 *      한 번에 IOBUF_BATCH_SIZE 또는 cache_size 만큼 가져와 일부는 cache 에 보관.
 *   3) 글로벌 ring 도 비면 entry 가 있으면 wait queue 에 push 하고 NULL.
 *      entry 가 NULL 이면 그냥 NULL.
 *
 * 실행 컨텍스트: 채널의 부모 io_channel 의 SPDK thread 안에서만 호출 가능 (assert).
 * lockless 근거: cache 는 per-thread, 글로벌 ring 만 spdk_ring(MP/MC) 으로 lockless.
 *
 * 호출 체인:
 *   bdev/NVMe-oF I/O 경로 → [spdk_iobuf_get] → STAILQ pop / spdk_ring_dequeue / wait queue push
 */
void *
spdk_iobuf_get(struct spdk_iobuf_channel *ch, uint64_t len,
	       struct spdk_iobuf_entry *entry, spdk_iobuf_get_cb cb_fn)
{
	struct spdk_iobuf_node_cache *cache;  /* [한국어] 사용할 cache 슬롯. */
	struct spdk_iobuf_pool_cache *pool;   /* [한국어] small/large 중 선택. */
	void *buf;                             /* [한국어] 반환할 데이터 버퍼. */

	cache = &ch->cache[0];
	/* [한국어] get 경로는 NUMA-aware 분류를 하지 않고 NUMA 0 슬롯만 사용 (간소화).
	 * put 시점엔 buf 의 실제 NUMA 를 spdk_mem_get_numa_id 로 얻어 정확한 슬롯에 환원한다. */

	assert(spdk_io_channel_get_thread(ch->parent) == spdk_get_thread());
	/* [한국어] thread affinity 검증 — 채널의 소유 SPDK thread 가 아닌 곳에서 부르면 lockless 가정 깨짐.
	 * 이게 SPDK 가 공유 자료구조에 락 없이 동작하는 핵심 근거. */
	if (len <= cache->small.bufsize) {     /* [한국어] 요청 길이가 small 이내면 small 풀 사용. */
		pool = &cache->small;
	} else {
		assert(len <= cache->large.bufsize); /* [한국어] large 보다 크면 호출자 버그 — large_bufsize 가 사실상 max I/O 한계. */
		pool = &cache->large;
	}

	buf = (void *)STAILQ_FIRST(&pool->cache); /* [한국어] cache hit 후보 — 가장 최근 환원된 head 를 살펴봄. */
	if (buf) {                                  /* [한국어] cache hit 경로 — 락 없이 STAILQ pop 만 하면 됨. */
		STAILQ_REMOVE_HEAD(&pool->cache, stailq);
		assert(pool->cache_count > 0);
		pool->cache_count--;
		pool->stats.cache++;                /* [한국어] cache hit 카운터 (DPDK Magazine 패턴의 빠른 경로). */
	} else {
		/* [한국어] cache miss — 글로벌 ring 에서 batch 로 끌어올린다. */
		struct spdk_iobuf_buffer *bufs[IOBUF_BATCH_SIZE]; /* [한국어] 한 번에 받을 임시 배치. */
		size_t sz, i;

		/* If we're going to dequeue, we may as well dequeue a batch. */
		sz = spdk_ring_dequeue(pool->pool, (void **)bufs, spdk_min(IOBUF_BATCH_SIZE,
				       spdk_max(pool->cache_size, 1)));
		/* [한국어] 글로벌 lockless ring 에서 한 번에 batch dequeue.
		 * batch 크기는 IOBUF_BATCH_SIZE 와 cache_size 중 작은 값(최소 1) — cache_size 0 인 일회성 채널도 동작. */
		if (sz == 0) {
			/* [한국어] 글로벌 풀도 비었음 — wait queue 모델로 fallback. */
			if (entry) {
				STAILQ_INSERT_TAIL(pool->queue, entry, stailq); /* [한국어] reactor 공유 wait queue 에 매달기 (FIFO). */
				entry->module = ch->module;                      /* [한국어] put 시 모듈 단위 라우팅을 위해 소유자 표시. */
				entry->cb_fn = cb_fn;                             /* [한국어] 깨어날 때 호출될 사용자 콜백. */
				pool->stats.retry++;                              /* [한국어] retry 카운터 — 풀 압박 상태 모니터링용. */
			}

			return NULL;                                              /* [한국어] 즉시 만족 불가 — 호출자는 비동기 콜백 또는 entry NULL 시 직접 처리. */
		}

		pool->stats.main++;
		/* [한국어] 글로벌 풀에서 fetch 한 횟수 카운터 (cache 미스 → main 풀 hit).
		 * 이상적인 워크로드는 stats.cache >> stats.main 이며, main 비율이 높으면 cache_size 부족. */
		for (i = 0; i < (sz - 1); i++) {
			STAILQ_INSERT_HEAD(&pool->cache, bufs[i], stailq);
			/* [한국어] 가져온 batch 중 마지막 한 개 제외하고 모두 cache 에 push.
			 * INSERT_HEAD 로 LIFO — 캐시 친화적(가장 최근 사용한 메모리 라인 다시 사용). */
			pool->cache_count++;
		}

		/* The last one is the one we'll return */
		buf = bufs[i];                          /* [한국어] 마지막 한 개를 반환 — 별도 cache push 없이 호출자가 즉시 사용. */
	}

	return (char *)buf;                            /* [한국어] 호출자에게 데이터 영역 시작 주소 반환 (헤더 영역도 같은 메모리지만 호출자는 데이터로만 사용). */
}

/*
 * [한국어]
 * spdk_iobuf_put — 사용 끝난 버퍼를 풀에 반환 (hot path).
 *
 * @ch : 호출 모듈의 채널.
 * @buf: get() 으로 받은 버퍼 포인터 (또는 다른 reactor 의 NUMA 슬롯에서 온 것).
 * @len: 원래 요청한 길이 (small/large 분류용).
 *
 * 동작:
 *   1) NUMA on 모드면 spdk_mem_get_numa_id 로 buf 의 실제 NUMA id 식별 → 그 슬롯에 환원.
 *   2) wait queue 에 대기자 있으면 즉시 깨워서 buf 를 직접 넘김 (글로벌 ring 우회 = fast path).
 *   3) 없으면 cache 에 push, cache 가 cache_size + IOBUF_BATCH_SIZE 를 넘으면 batch 만큼 글로벌 ring 으로 flush.
 *
 * 실행 컨텍스트: 호출자 SPDK thread 안. waiter 가 같은 thread 에 매달린 경우, 깨우면서 콜백 안에서 즉시 사용.
 *
 * 호출 체인:
 *   bdev/NVMe-oF I/O 완료 → [spdk_iobuf_put] → entry->cb_fn (waiter) 또는 cache push / spdk_ring_enqueue
 */
void
spdk_iobuf_put(struct spdk_iobuf_channel *ch, void *buf, uint64_t len)
{
	struct spdk_iobuf_entry *entry;        /* [한국어] 깨울 wait 엔트리. */
	struct spdk_iobuf_buffer *iobuf_buf;   /* [한국어] cache 에 매달기 위한 buffer 헤더 캐스트. */
	struct spdk_iobuf_node_cache *cache;   /* [한국어] 환원할 cache 슬롯. */
	struct spdk_iobuf_pool_cache *pool;    /* [한국어] small or large 중 선택. */
	uint32_t numa_id;                       /* [한국어] buf 가 속한 NUMA 노드 id. */
	size_t sz;                              /* [한국어] flush batch 크기. */

	if (g_iobuf.opts.enable_numa) {        /* [한국어] NUMA 활성: 버퍼의 실제 NUMA 를 알아내 정확한 슬롯에 환원. */
		numa_id = spdk_mem_get_numa_id(buf, NULL);
		/* [한국어] DPDK heap 에 등록된 페이지의 NUMA id 반환 — buf 가 어느 NUMA 노드 hugepage 에서 왔는지 식별.
		 * 이것이 가능한 이유: spdk_malloc 시 페이지마다 NUMA 정보가 등록돼 있음. */
	} else {
		numa_id = 0;                    /* [한국어] NUMA off: 항상 [0] 슬롯. */
	}

	cache = &ch->cache[numa_id];

	assert(spdk_io_channel_get_thread(ch->parent) == spdk_get_thread());
	/* [한국어] put 도 채널의 소유 SPDK thread 안에서만 — lockless 가정 검증. */
	if (len <= cache->small.bufsize) {     /* [한국어] small/large 분류 (get 시 했던 것과 동일). */
		pool = &cache->small;
	} else {
		pool = &cache->large;
	}

	if (STAILQ_EMPTY(pool->queue)) {       /* [한국어] wait 중인 모듈이 없는 일반 경로. */
		if (pool->cache_size == 0) {    /* [한국어] cache 사용을 끄고 채널을 만들었다면 — */
			spdk_ring_enqueue(pool->pool, (void **)&buf, 1, NULL);
			/* [한국어] cache 우회하고 곧장 글로벌 ring 으로 환원. */
			return;
		}

		iobuf_buf = (struct spdk_iobuf_buffer *)buf;
		/* [한국어] buf 첫 부분에 STAILQ 노드를 두기 위한 캐스트 — in-place 헤더. */

		STAILQ_INSERT_HEAD(&pool->cache, iobuf_buf, stailq); /* [한국어] cache LIFO push — 다음 get 이 같은 라인을 다시 가져가도록. */
		pool->cache_count++;

		/* The cache size may exceed the configured amount. We always dequeue from the
		 * central pool in batches of known size, so wait until at least a batch
		 * has been returned to actually return the buffers to the central pool. */
		/* [한국어] cache_size 가 한 번 초과해도 즉시 flush 하지 않고, 한 batch 만큼 더 쌓일 때까지 기다린다.
		 * → 이유: 글로벌 ring 은 항상 batch 단위로 dequeue 되므로, 같은 batch 단위로 enqueue 하는 것이
		 *   ring slot 효율과 cacheline contention 양쪽에서 유리. */
		sz = spdk_min(IOBUF_BATCH_SIZE, pool->cache_size); /* [한국어] flush 할 batch 크기 (BATCH 와 cache_size 중 작은 값). */
		if (pool->cache_count >= pool->cache_size + sz) {   /* [한국어] cache_size + batch 한 개 이상 쌓였으면 flush 시점. */
			struct spdk_iobuf_buffer *bufs[IOBUF_BATCH_SIZE]; /* [한국어] flush 임시 배치. */
			size_t i;

			for (i = 0; i < sz; i++) {                  /* [한국어] cache head 에서 sz 개 pop. */
				bufs[i] = STAILQ_FIRST(&pool->cache);
				STAILQ_REMOVE_HEAD(&pool->cache, stailq);
				assert(pool->cache_count > 0);
				pool->cache_count--;
			}

			spdk_ring_enqueue(pool->pool, (void **)bufs, sz, NULL);
			/* [한국어] 글로벌 lockless ring 으로 batch 환원 — 다른 reactor 가 즉시 가져갈 수 있음. */
		}
	} else {
		/* [한국어] wait 중인 엔트리가 있다 — buf 를 글로벌 ring/cache 거치지 않고 곧바로 깨우며 전달 (가장 빠른 경로). */
		entry = STAILQ_FIRST(pool->queue);                  /* [한국어] FIFO head — 가장 오래 기다린 엔트리. */
		STAILQ_REMOVE_HEAD(pool->queue, stailq);
		entry->cb_fn(entry, buf);                            /* [한국어] 사용자 콜백 호출 — 이 호출이 같은 thread 에서 buf 를 즉시 다시 사용 가능. */
		if (spdk_unlikely(entry == STAILQ_LAST(pool->queue, spdk_iobuf_entry, stailq))) {
			/* [한국어] (드문 케이스) 깨운 엔트리가 큐의 last 와 같다 — cb_fn 안에서 같은 entry 를 다시 enqueue 한 경우.
			 * 그대로 두면 head/tail 무결성이 깨질 수 있어 head 위치로 옮겨 큐 일관성을 회복. */
			STAILQ_REMOVE(pool->queue, entry, spdk_iobuf_entry, stailq);
			STAILQ_INSERT_HEAD(pool->queue, entry, stailq);
		}
	}
}

/*
 * [한국어]
 * iobuf_get_channel_stats_done — spdk_for_each_channel 의 완료 콜백.
 *
 * @iter  : 순회 컨텍스트.
 * @status: spdk_for_each_channel 결과 (보통 0).
 *
 * 모든 reactor 순회가 끝난 시점에 ctx->modules 통계를 사용자 cb_fn 에 전달하고 메모리 해제.
 * 실행 컨텍스트: spdk_for_each_channel 호출 시 지정된 시작 thread (=호출자 thread).
 *
 * 호출 체인:
 *   spdk_iobuf_get_stats() → spdk_for_each_channel(…, iobuf_get_channel_stats_done)
 *     → 모든 reactor 통과 후 → [iobuf_get_channel_stats_done] → 사용자 cb_fn
 */
static void
iobuf_get_channel_stats_done(struct spdk_io_channel_iter *iter, int status)
{
	struct iobuf_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(iter); /* [한국어] heap 컨텍스트 추출. */

	ctx->cb_fn(ctx->modules, ctx->num_modules, ctx->cb_arg); /* [한국어] 누적된 모듈별 통계와 함께 사용자 콜백 호출. */
	free(ctx->modules);                                       /* [한국어] 모듈 통계 배열 해제. */
	free(ctx);                                                 /* [한국어] 컨텍스트 본체 해제. */
}

/*
 * [한국어]
 * iobuf_get_channel_stats — 한 reactor 위에서 stats 를 누적하는 per-channel 콜백.
 *
 * @iter: 순회 컨텍스트 (cb_arg 로 iobuf_get_stats_ctx 보유).
 *
 * 1) 이 reactor 의 iobuf_channel ctx 에서 모든 살아있는 모듈 채널을 순회.
 * 2) 각 채널의 모든 NUMA 슬롯 small/large stats 를 모듈 이름 매칭으로 누적.
 * 3) spdk_for_each_channel_continue 로 다음 reactor 로 hop.
 *
 * 실행 컨텍스트: 매 호출마다 다른 reactor — 각 reactor 의 cache stats 를 그 reactor 가 직접 읽는다 (lockless).
 *
 * 호출 체인:
 *   spdk_for_each_channel → 각 reactor 마다 [iobuf_get_channel_stats]
 */
static void
iobuf_get_channel_stats(struct spdk_io_channel_iter *iter)
{
	struct iobuf_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(iter);   /* [한국어] heap 컨텍스트. */
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(iter);     /* [한국어] 현재 hop 한 reactor 의 io_channel. */
	struct iobuf_channel *iobuf_ch = spdk_io_channel_get_ctx(ch);            /* [한국어] reactor 공유 ctx — 모듈 채널 약한 참조 보관. */
	struct spdk_iobuf_channel *channel;                                      /* [한국어] 모듈 채널 포인터. */
	struct iobuf_module *module;                                             /* [한국어] 채널 소유 모듈. */
	struct spdk_iobuf_module_stats *it;                                      /* [한국어] 누적 대상 모듈 통계. */
	uint32_t i, j;                                                            /* [한국어] 모듈 / 채널 슬롯 인덱스. */

	for (i = 0; i < ctx->num_modules; ++i) {                                 /* [한국어] 등록된 모든 모듈에 대해 — */
		for (j = 0; j < IOBUF_MAX_CHANNELS; ++j) {                       /* [한국어] 이 reactor 에 살아있는 모든 모듈 채널 슬롯 — */
			channel = iobuf_ch->channels[j];
			if (channel == NULL) {                                    /* [한국어] 빈 슬롯은 건너뜀. */
				continue;
			}

			it = &ctx->modules[i];                                    /* [한국어] 현재 모듈 인덱스의 통계. */
			module = (struct iobuf_module *)channel->module;
			if (strcmp(it->module, module->name) == 0) {              /* [한국어] 이름 매칭으로 같은 모듈 채널 식별. */
				struct spdk_iobuf_pool_cache *cache;
				uint32_t i;                                        /* [한국어] 외부 i 가리는 의도적 shadowing — NUMA 순회 전용. */

				IOBUF_FOREACH_NUMA_ID(i) {                          /* [한국어] 모든 NUMA 슬롯의 stats 를 합산 — */
					cache = &channel->cache[i].small;
					it->small_pool.cache += cache->stats.cache;  /* [한국어] cache hit 누적. */
					it->small_pool.main += cache->stats.main;     /* [한국어] 글로벌 ring fetch 누적. */
					it->small_pool.retry += cache->stats.retry;   /* [한국어] wait queue 등록(=풀 고갈) 누적. */

					cache = &channel->cache[i].large;
					it->large_pool.cache += cache->stats.cache;
					it->large_pool.main += cache->stats.main;
					it->large_pool.retry += cache->stats.retry;
				}
				break;                                              /* [한국어] 한 reactor 에는 같은 모듈 채널이 최대 1 개라 가정 — 발견 시 다음 모듈로. */
			}
		}
	}

	spdk_for_each_channel_continue(iter, 0);
	/* [한국어] 다음 reactor 로 hop 시작 — 모든 reactor 가 끝나면 iobuf_get_channel_stats_done 자동 호출. */
}

/*
 * [한국어]
 * spdk_iobuf_get_stats — 모든 reactor 의 모듈별 cache/main/retry 통계를 비동기로 수집.
 *
 * @cb_fn : 모든 통계가 모이면 호출될 사용자 콜백.
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0 성공, -ENOMEM 컨텍스트 할당 실패.
 *
 * spdk_for_each_channel 인프라를 사용해 reactor 별로 hop 하면서 누적한다.
 * 본 함수는 즉시 반환하고 결과는 cb_fn 에서 도달.
 *
 * 호출 체인:
 *   사용자 RPC / 모니터링 → [spdk_iobuf_get_stats] → spdk_for_each_channel → … → cb_fn
 */
int
spdk_iobuf_get_stats(spdk_iobuf_get_stats_cb cb_fn, void *cb_arg)
{
	struct iobuf_module *module;     /* [한국어] 모듈 순회. */
	struct iobuf_get_stats_ctx *ctx; /* [한국어] 비동기 컨텍스트. */
	uint32_t i;                       /* [한국어] 모듈 인덱스. */

	ctx = calloc(1, sizeof(*ctx));    /* [한국어] heap 컨텍스트 — 비동기 흐름 동안 살아있어야 함. */
	if (ctx == NULL) {
		return -ENOMEM;
	}

	TAILQ_FOREACH(module, &g_iobuf.modules, tailq) {                          /* [한국어] 등록된 모듈 개수 카운트. */
		++ctx->num_modules;
	}

	ctx->modules = calloc(ctx->num_modules, sizeof(struct spdk_iobuf_module_stats)); /* [한국어] 모듈 개수만큼 통계 배열 0-init. */
	if (ctx->modules == NULL) {
		free(ctx);
		return -ENOMEM;
	}

	i = 0;
	TAILQ_FOREACH(module, &g_iobuf.modules, tailq) {                          /* [한국어] 모듈 이름을 통계 슬롯에 미리 채워둠 — get_channel_stats 가 strcmp 매칭에 사용. */
		ctx->modules[i].module = module->name;
		++i;
	}

	ctx->cb_fn = cb_fn;       /* [한국어] 사용자 완료 콜백 보관. */
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(&g_iobuf, iobuf_get_channel_stats, ctx,
			      iobuf_get_channel_stats_done);
	/* [한국어] iobuf io_device 의 모든 reactor 위 io_channel 을 순회.
	 * 매 reactor 에서 iobuf_get_channel_stats 가 호출되어 stats 누적,
	 * 모두 끝나면 iobuf_get_channel_stats_done 으로 사용자 cb_fn 호출 + 메모리 해제. */
	return 0;
}
