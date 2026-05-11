/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK Blobstore 코어 구현 (blobstore.c)
 *
 * === 파일의 역할 ===
 * SPDK Blobstore — "blob"(가변 크기 객체)을 cluster 단위로 묶어 보관하는 객체 저장소 —
 * 의 핵심 로직 전부가 이 한 파일에 들어 있다. 기능적으로 다음을 모두 책임진다:
 *   1) 저장소 lifecycle: spdk_bs_init / load / unload / destroy / grow (super block,
 *      메타데이터 페이지, used_md/clusters/blobids 비트맵 등 영속 자료구조 관리).
 *   2) Blob lifecycle:   create / open / close / delete / resize / sync / set_super.
 *   3) Blob I/O:        read / write / readv / writev / unmap / write_zeroes
 *      (thick provision / thin provision / COW / snapshot / clone / esnap 경로 분기).
 *   4) Snapshot/Clone/Esnap: create_snapshot / create_clone / inflate / decouple_parent /
 *      shallow_copy / set_parent / set_external_parent.
 *   5) Extended attributes(xattr) — 내부/외부 attr 모두.
 *   6) Iteration: spdk_bs_iter_first / spdk_bs_iter_next 로 모든 blob 순회.
 *   7) 비동기 작업 시퀀스/배치(spdk_bs_sequence_t / spdk_bs_batch_t)를 통한 callback chain.
 *   8) Cluster 할당/해제, md page 할당/해제, used_* 비트맵 갱신과 superblock 영속화.
 *
 * 영속 레이아웃 개요 (bs_dev 위에서):
 *   [super block (1 page)]
 *   [used_md   bitmap]   - 어떤 md page가 사용 중인지
 *   [used_clusters bitmap] - 어떤 cluster가 사용 중인지
 *   [used_blobids bitmap]  - 어떤 blob id가 사용 중인지
 *   [md pages …]          - 각 blob의 descriptor chain (XATTR, EXTENT_RLE, EXTENT_PAGE, FLAGS)
 *   [cluster data …]      - 실제 사용자 데이터 (io_unit/cluster 단위)
 *
 * === 전체 아키텍처에서의 위치 ===
 * Blobstore는 SPDK 스토리지 스택의 "객체 저장소" 레이어이며 위/아래로 다음과 연결된다:
 *
 *   [lvol / vbdev_lvol / blobfs / 사용자 앱]
 *        ↓ public API (include/spdk/blob.h)
 *   [blobstore.c (이 파일) — 메타·IO·callback chain]
 *        ↓ blob_bs_dev I/O (request_set/sequence/batch)
 *   [bs_dev abstraction (struct spdk_bs_dev)]
 *        ↓
 *   [bdev / aio / malloc / nvme bs_dev 구현 (module/blob/bdev/blob_bdev.c 등)]
 *        ↓
 *   [bdev layer → NVMe 드라이버 → 물리 매체]
 *
 * SPDK 스레드 모델: blobstore는 "md_thread"(메타 갱신 전담)와 "user thread"(IO 발사) 사이의
 * 협동을 메시지 패스(spdk_thread_send_msg)로 구현한다. cluster insert/free 같은 메타 변경은
 * 항상 md_thread로 우회되며, IO 핫 패스는 user 스레드에서 가능한 한 자체 처리한다.
 *
 * === 타 모듈과의 연결 ===
 * - include/spdk/blob.h          : 공개 API (spdk_bs_*, spdk_blob_*)
 * - lib/blob/blobstore.h         : 내부 자료구조 (struct spdk_blob, spdk_blob_store, etc.)
 * - lib/blob/request.c           : spdk_bs_sequence_t / spdk_bs_batch_t / request_set
 * - lib/blob/blob_bs_dev.c       : blob을 backing device로 노출 (snapshot 체인)
 * - lib/blob/zeroes.c            : zero-fill bs_dev (없는 cluster read 처리)
 * - module/blob/bdev/blob_bdev.c : 일반 bdev을 bs_dev로 감싸는 어댑터
 * - include/spdk_internal/trace_defs.h : tracepoint id (OWNER_BLOB 등)
 *
 * 데이터 흐름 (사용자 write 한 건 기준):
 *   spdk_blob_io_write → blob_request_submit_op → blob_request_submit_op_split
 *     → COW가 필요한 cluster면 bs_allocate_and_copy_cluster (md_thread로 RPC)
 *         → bs_user_op_queue/resume + spdk_bs_sequence_*
 *     → spdk_bs_sequence_*_writev(seq, io_unit_lba, …) → bs_dev->writev
 *     → completion → callback chain → user cb_fn
 *
 * === 주요 함수/구조체 요약 ===
 * Lifecycle:
 *   - spdk_bs_init            : super 새로 만들고 비트맵·메타 초기화 (전형적 흐름)
 *   - spdk_bs_load            : 디스크 super 읽고 used_md/clusters/blobids 복원
 *   - spdk_bs_unload          : 더티 영속화 후 정리
 *   - spdk_bs_destroy         : super 무효화 후 해제 (load 불가능 상태로)
 *   - spdk_bs_grow_live / spdk_bs_grow : 백엔드 확장 반영
 *
 * Blob lifecycle:
 *   - spdk_bs_create_blob(_ext) : 빈 blob 할당 + md 초기화
 *   - spdk_bs_open_blob(_ext)   : id로 blob을 열어 핸들 획득
 *   - spdk_blob_close           : ref 감소, 0이면 메타 sync 후 자원 회수
 *   - spdk_bs_delete_blob       : 의존성 해제 후 영구 삭제
 *   - spdk_blob_resize          : cluster 수 변경 (thin: 가상 크기만, thick: 즉시 할당)
 *   - spdk_blob_sync_md         : 인메모리 메타 → 디스크
 *
 * Snapshot/Clone:
 *   - spdk_bs_create_snapshot   : blob을 read-only 스냅샷으로 분기
 *   - spdk_bs_create_clone      : 스냅샷에서 새 thin clone 생성
 *   - spdk_bs_inflate_blob      : thin → thick (모든 unallocated cluster를 채움)
 *   - spdk_bs_blob_decouple_parent : 부모와의 의존 끊기 (parent의 데이터를 모두 복사)
 *   - spdk_bs_blob_shallow_copy  : 활용 중인 cluster만 다른 bs_dev로 복사
 *   - spdk_bs_blob_set_parent / _set_external_parent : 부모 재지정 (esnap 포함)
 *
 * I/O:
 *   - spdk_blob_io_read/write    : 1 io 한 건
 *   - spdk_blob_io_readv/writev  : iovec 다건
 *   - spdk_blob_io_readv_ext/writev_ext : ext_opts(메타) 동반
 *   - spdk_blob_io_unmap / write_zeroes : 영역 무효화 / 0 채움
 *
 * 내부 콜백 사이클의 대표 자료구조:
 *   - struct spdk_blob_store     : 저장소 전체 상태 (super, 비트맵, md_thread, blob 리스트)
 *   - struct spdk_blob           : 단일 blob 메타 (id, ref, active/clean, cluster 배열)
 *   - struct spdk_blob_md_page   : 디스크 영속 메타 페이지 (descriptor chain)
 *   - struct spdk_bs_channel     : per-thread IO 채널 (bs_dev channel + queued ops)
 *   - struct spdk_blob_persist_ctx, spdk_blob_load_ctx, spdk_bs_load_ctx : 비동기 단계 컨텍스트
 *   - struct spdk_clone_snapshot_ctx, delete_snapshot_ctx, set_parent_ctx 등 : 작업별 ctx
 *
 * Esnap (External snapshot):
 *   - 외부 bs_dev을 부모로 갖는 clone — 부모는 blobstore에 없을 수 있음 (read-only)
 *   - blob_esnap_channel: 스레드/blob 단위 IO 채널 캐시 (RB tree)
 *   - 관련 함수: blob_esnap_destroy_bs_dev_channels, spdk_blob_set_esnap_bs_dev …
 */

#include "spdk/stdinc.h"          /* [한국어] 표준 라이브러리 묶음 (string, stdint, …) */

#include "spdk/blob.h"            /* [한국어] 공개 Blobstore API 정의 */
#include "spdk/crc32.h"           /* [한국어] md page CRC 검증/생성 */
#include "spdk/env.h"             /* [한국어] DPDK 추상화 — DMA-가능 메모리 할당 (spdk_zmalloc 등) */
#include "spdk/queue.h"           /* [한국어] TAILQ_x/RB_x 매크로 — blob list, esnap tree 등 */
#include "spdk/thread.h"          /* [한국어] spdk_thread/poller/send_msg — md_thread 우회에 사용 */
#include "spdk/bit_array.h"       /* [한국어] 비트 배열 — used_md/used_blobids 비트맵 */
#include "spdk/bit_pool.h"        /* [한국어] 비트 풀 — used_clusters 풀 (할당/회수) */
#include "spdk/likely.h"          /* [한국어] spdk_likely/unlikely (브랜치 힌트) */
#include "spdk/util.h"            /* [한국어] SPDK_CONTAINEROF, alignment 매크로 등 */
#include "spdk/string.h"          /* [한국어] 문자열 헬퍼 (spdk_strerror 등) */
#include "spdk/trace.h"           /* [한국어] tracepoint 기록 — TRACE_BLOB_REQ_*等 */

#include "spdk_internal/assert.h" /* [한국어] SPDK 내부 assert (release 모드에서도 유지) */
#include "spdk_internal/trace_defs.h" /* [한국어] OWNER_BLOB / TRACE_BLOB_* id 정의 */
#include "spdk/log.h"             /* [한국어] SPDK 로그 매크로 */

#include "blobstore.h"            /* [한국어] 내부 구조체/매크로 — spdk_blob_store 등 */

#define BLOB_CRC32C_INITIAL    0xffffffffUL  /* [한국어] md page CRC32C 초기값 (RFC 3720 표준) */

/* [한국어] 다음 forward declaration 묶음은 콜백 체인이 서로를 참조해야 하므로
 * 정의 순서와 무관하게 미리 선언해 둔다. 모두 같은 파일 내 static 함수다. */
static int bs_register_md_thread(struct spdk_blob_store *bs);     /* [한국어] md_thread 핸들 캐시 */
static int bs_unregister_md_thread(struct spdk_blob_store *bs);   /* [한국어] 위 해제 */
static void blob_close_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno); /* [한국어] close 시퀀스 완료 콜백 */
/* [한국어] cluster 할당/해제는 메타 변경이라 항상 md_thread에서만 수행한다 (lockless 보장).
 * IO 스레드가 새 cluster가 필요해지면 spdk_thread_send_msg로 md_thread에 위임한다. */
static void blob_insert_cluster_on_md_thread(struct spdk_blob *blob, uint32_t cluster_num,
		uint64_t cluster, uint32_t extent, struct spdk_blob_md_page *page,
		spdk_blob_op_complete cb_fn, void *cb_arg);
static void blob_free_cluster_on_md_thread(struct spdk_blob *blob, uint32_t cluster_num,
		uint32_t extent_page, struct spdk_blob_md_page *page, spdk_blob_op_complete cb_fn, void *cb_arg);

/* [한국어] xattr 핵심 헬퍼 — internal/external 구분으로 두 종류의 xattr 모두 처리.
 * internal=true는 사용자에게 보이지 않는 SPDK 내부 메타 (snapshot 부모 id 등). */
static int blob_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
			  uint16_t value_len, bool internal);
static int blob_get_xattr_value(struct spdk_blob *blob, const char *name,
				const void **value, size_t *value_len, bool internal);
static int blob_remove_xattr(struct spdk_blob *blob, const char *name, bool internal);

/* [한국어] EXTENT_PAGE 단위(많은 cluster 매핑) 메타를 디스크에 쓰는 헬퍼. */
static void blob_write_extent_page(struct spdk_blob *blob, uint32_t extent, uint64_t cluster_num,
				   struct spdk_blob_md_page *page, spdk_blob_op_complete cb_fn, void *cb_arg);
/* [한국어] blob에 진행 중인 IO를 잠시 멈춰 메타 변경(snapshot 분기 등)을 안전히 수행. */
static void blob_freeze_io(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg);

/* [한국어] shallow copy 루프 — 다음 할당된 cluster를 찾아 대상 bs_dev로 복사 진행. */
static void bs_shallow_copy_cluster_find_next(void *cb_arg);

/*
 * External snapshots require a channel per thread per esnap bdev.  The tree
 * is populated lazily as blob IOs are handled by the back_bs_dev. When this
 * channel is destroyed, all the channels in the tree are destroyed.
 */

/* [한국어] Esnap 채널 캐시 노드.
 * 외부 snapshot(다른 bs_dev이 부모)을 가진 blob은 IO마다 부모 bs_dev에 채널이 필요한데,
 * 매번 만들면 비싸므로 (thread, blob_id) 단위로 한 번 만든 채널을 RB tree에 캐시한다. */
struct blob_esnap_channel {
	RB_ENTRY(blob_esnap_channel)	node;
	/* [한국어] RB tree 링크 — blob_esnap_channel_tree에 매달림. */

	spdk_blob_id			blob_id;
	/* [한국어] 키 — 어느 esnap clone blob에 대한 채널인지 식별. */

	struct spdk_io_channel		*channel;
	/* [한국어] 부모 bs_dev에 연결된 io channel. */
};

/* [한국어] esnap 채널 관련 정적 함수들. RB tree 비교자/파괴자/콜백 구현은 아래쪽 정의 참조. */
static int blob_esnap_channel_compare(struct blob_esnap_channel *c1, struct blob_esnap_channel *c2);
static void blob_esnap_destroy_bs_dev_channels(struct spdk_blob *blob, bool abort_io,
		spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);
static void blob_esnap_destroy_bs_channel(struct spdk_bs_channel *ch);
static void blob_set_back_bs_dev_frozen(void *_ctx, int bserrno);
/* [한국어] esnap 채널 tree의 RB 매크로 코드 생성 (insert/find/remove 등). */
RB_GENERATE_STATIC(blob_esnap_channel_tree, blob_esnap_channel, node, blob_esnap_channel_compare)

/*
 * [한국어]
 * blob_is_esnap_clone - blob이 외부 snapshot(esnap)의 clone 인지 검사 (inline)
 *
 * @blob: 검사 대상 (NULL 금지)
 * @return: invalid_flags에 SPDK_BLOB_EXTERNAL_SNAPSHOT 비트가 켜져 있으면 true
 *
 * Esnap clone은 부모가 다른 bs_dev(예: 별도 lvol/snapshot 외부 데이터)에 있으므로
 * read 시 부모 bs_dev로 분기한다. invalid_flags = "예전 SPDK가 알 수 없으면 깨어 있는
 * blob을 망가뜨릴 수 있으므로 무시하지 말고 거부하라"는 영속 플래그.
 */
static inline bool
blob_is_esnap_clone(const struct spdk_blob *blob)
{
	assert(blob != NULL);                              /* [한국어] NULL 방어 (호출자 계약) */
	return !!(blob->invalid_flags & SPDK_BLOB_EXTERNAL_SNAPSHOT); /* [한국어] !! 로 0/1 정규화 */
}

/*
 * [한국어]
 * blob_id_cmp - blob id 비교자 (RB tree key 함수)
 *
 * @return: -1/0/+1 (id1 < id2 / 동일 / id1 > id2)
 *
 * spdk_blob_tree (열린 blob들을 id로 정렬해 빠르게 찾는 RB tree) 의 비교 함수.
 */
static int
blob_id_cmp(struct spdk_blob *blob1, struct spdk_blob *blob2)
{
	assert(blob1 != NULL && blob2 != NULL);
	return (blob1->id < blob2->id ? -1 : blob1->id > blob2->id);
}

/* [한국어] 열린 blob의 RB tree (key=blob->id, link=spdk_blob::link).
 * RB_GENERATE_STATIC: insert/find/remove 함수를 컴파일 타임에 생성. */
RB_GENERATE_STATIC(spdk_blob_tree, spdk_blob, link, blob_id_cmp);

/*
 * [한국어]
 * blob_verify_md_op - 메타 변경 작업이 md_thread에서 호출되는지 단언
 *
 * 모든 메타 변경 함수의 진입부에서 호출되어 thread affinity를 강제한다.
 * Lockless 메타 자료구조의 핵심 전제 — md_thread만 메타를 수정한다.
 */
static void
blob_verify_md_op(struct spdk_blob *blob)
{
	assert(blob != NULL);
	assert(spdk_get_thread() == blob->bs->md_thread); /* [한국어] 현재 스레드가 md_thread인지 검증 */
	assert(blob->state != SPDK_BLOB_STATE_LOADING);   /* [한국어] LOADING 중에는 메타 조작 금지 */
}

/*
 * [한국어]
 * bs_get_snapshot_entry - 주어진 blobid에 해당하는 snapshot 항목 검색
 *
 * @bs:     blobstore
 * @blobid: snapshot 후보 blob id
 * @return: 매칭되는 snapshot list 항목, 없으면 NULL
 *
 * snapshots 리스트는 "스냅샷 blob → 그를 부모로 가진 clone들"을 매다는 자료구조.
 * 새 clone 등록 / clone 삭제 시 부모 검색에 사용.
 */
static struct spdk_blob_list *
bs_get_snapshot_entry(struct spdk_blob_store *bs, spdk_blob_id blobid)
{
	struct spdk_blob_list *snapshot_entry = NULL;

	TAILQ_FOREACH(snapshot_entry, &bs->snapshots, link) {
		if (snapshot_entry->id == blobid) {
			break;
		}
	}

	return snapshot_entry;
}

/*
 * [한국어]
 * bs_claim_md_page - md page 한 장을 used 비트맵에 표시 (할당 마킹)
 *
 * used_lock(spin) 보호 하에서 호출되어야 함. 동일 페이지 이중 할당은 assert로 보호.
 */
static void
bs_claim_md_page(struct spdk_blob_store *bs, uint32_t page)
{
	assert(spdk_spin_held(&bs->used_lock));            /* [한국어] 호출자가 used_lock을 잡고 있어야 함 */
	assert(page < spdk_bit_array_capacity(bs->used_md_pages)); /* [한국어] 인덱스 범위 검증 */
	assert(spdk_bit_array_get(bs->used_md_pages, page) == false); /* [한국어] 이중 할당 금지 */

	spdk_bit_array_set(bs->used_md_pages, page);       /* [한국어] used 비트 ON */
}

/*
 * [한국어]
 * bs_release_md_page - md page 사용 해제 (used 비트 OFF)
 */
static void
bs_release_md_page(struct spdk_blob_store *bs, uint32_t page)
{
	assert(spdk_spin_held(&bs->used_lock));            /* [한국어] 락 보유 검증 */
	assert(page < spdk_bit_array_capacity(bs->used_md_pages));
	assert(spdk_bit_array_get(bs->used_md_pages, page) == true); /* [한국어] 이미 비어 있으면 버그 */

	spdk_bit_array_clear(bs->used_md_pages, page);     /* [한국어] used 비트 OFF */
}

/*
 * [한국어]
 * bs_claim_cluster - 자유 cluster 한 개를 비트풀에서 빼서 반환
 *
 * @return: 할당된 cluster 인덱스, 자원 없으면 UINT32_MAX
 *
 * cluster는 blob의 데이터 단위(여러 io_unit/page를 묶은 큰 블록 — 보통 1 MiB).
 * spdk_bit_pool은 bit_array의 효율적인 wrapper로 first-fit 검색을 제공.
 */
static uint32_t
bs_claim_cluster(struct spdk_blob_store *bs)
{
	uint32_t cluster_num;                              /* [한국어] 할당 결과 */

	assert(spdk_spin_held(&bs->used_lock));

	cluster_num = spdk_bit_pool_allocate_bit(bs->used_clusters); /* [한국어] 자유 비트 1개 획득 */
	if (cluster_num == UINT32_MAX) {
		return UINT32_MAX;                         /* [한국어] 가용 cluster 없음 */
	}

	SPDK_DEBUGLOG(blob, "Claiming cluster %u\n", cluster_num);
	bs->num_free_clusters--;                           /* [한국어] 카운터 동기화 */

	return cluster_num;
}

/*
 * [한국어]
 * bs_release_cluster - cluster 사용 해제 (비트풀에 반환)
 */
static void
bs_release_cluster(struct spdk_blob_store *bs, uint32_t cluster_num)
{
	assert(spdk_spin_held(&bs->used_lock));
	assert(cluster_num < spdk_bit_pool_capacity(bs->used_clusters));
	assert(spdk_bit_pool_is_allocated(bs->used_clusters, cluster_num) == true); /* [한국어] 이미 풀린 비트 다시 풀기 금지 */
	assert(bs->num_free_clusters < bs->total_clusters); /* [한국어] 음수 방지 */

	SPDK_DEBUGLOG(blob, "Releasing cluster %u\n", cluster_num);

	spdk_bit_pool_free_bit(bs->used_clusters, cluster_num); /* [한국어] 비트 OFF */
	bs->num_free_clusters++;                           /* [한국어] 카운터 동기화 */
}

/*
 * [한국어]
 * blob_insert_cluster - blob의 logical cluster 슬롯에 physical cluster LBA 매핑
 *
 * @blob:        대상 blob (md_thread 소유)
 * @cluster_num: blob 내 logical cluster 번호 (0..num_clusters-1)
 * @cluster:     bs 단위의 physical cluster 인덱스
 * @return: 0 성공, -EEXIST 이미 매핑된 슬롯
 *
 * thin provision일 때 빈 cluster_num 슬롯에 처음으로 데이터가 쓰이면 호출되어
 * "이 logical cluster는 이제 이 physical cluster"라는 매핑을 만든다.
 */
static int
blob_insert_cluster(struct spdk_blob *blob, uint32_t cluster_num, uint64_t cluster)
{
	uint64_t *cluster_lba = &blob->active.clusters[cluster_num]; /* [한국어] active 매핑 슬롯 */

	blob_verify_md_op(blob);                           /* [한국어] md_thread 검증 */

	if (*cluster_lba != 0) {
		return -EEXIST;                            /* [한국어] 이미 매핑됨 — 호출자 버그 */
	}

	*cluster_lba = bs_cluster_to_lba(blob->bs, cluster); /* [한국어] cluster 인덱스 → LBA 변환 */
	blob->active.num_allocated_clusters++;             /* [한국어] 할당 cluster 수 증가 */

	return 0;
}

/*
 * [한국어]
 * bs_allocate_cluster - 새 cluster 한 개와 (필요 시) extent page 한 개를 한 번에 할당
 *
 * @blob:               대상 blob (md_thread)
 * @cluster_num:        blob 내 logical cluster 번호
 * @cluster:            (out) 할당된 physical cluster 인덱스
 * @lowest_free_md_page:(in/out) 검색 시작 위치 / 결과 페이지 번호
 * @update_map:         true면 즉시 blob 매핑까지 갱신 (false면 caller가 나중에)
 * @return: 0 성공, -ENOSPC 자원 부족
 *
 * use_extent_table=true 인 blob은 cluster 매핑을 EXTENT_PAGE로 외부화하는 새 포맷이며,
 * 이 경우 cluster를 할당할 때 그 cluster를 가리킬 extent page도 같이 할당한다.
 * 실패 시 이미 잡은 cluster를 풀어 atomicity를 보장한다.
 *
 * 호출 체인:
 *   blob_persist / blob_insert_cluster_msg → bs_allocate_cluster
 *     → bs_claim_cluster + (선택) bs_claim_md_page + (선택) blob_insert_cluster
 */
static int
bs_allocate_cluster(struct spdk_blob *blob, uint32_t cluster_num,
		    uint64_t *cluster, uint32_t *lowest_free_md_page, bool update_map)
{
	uint32_t *extent_page = 0;                         /* [한국어] extent_page 슬롯 포인터 */

	assert(spdk_spin_held(&blob->bs->used_lock));      /* [한국어] used_lock 보유 검증 */

	*cluster = bs_claim_cluster(blob->bs);             /* [한국어] 자유 cluster 1개 획득 */
	if (*cluster == UINT32_MAX) {
		/* No more free clusters. Cannot satisfy the request */
		return -ENOSPC;
	}

	if (blob->use_extent_table) {                      /* [한국어] 새 EXTENT_TABLE 포맷 분기 */
		extent_page = bs_cluster_to_extent_page(blob, cluster_num); /* [한국어] 이 cluster를 가리킬 extent page 슬롯 */
		if (*extent_page == 0) {                   /* [한국어] 아직 extent page 미할당 */
			/* Extent page shall never occupy md_page so start the search from 1 */
			if (*lowest_free_md_page == 0) {
				*lowest_free_md_page = 1;  /* [한국어] page 0은 super 영역이라 제외 */
			}
			/* No extent_page is allocated for the cluster */
			/* [한국어] used_md_pages에서 lowest_free 인덱스부터 빈 슬롯 검색. */
			*lowest_free_md_page = spdk_bit_array_find_first_clear(blob->bs->used_md_pages,
					       *lowest_free_md_page);
			if (*lowest_free_md_page == UINT32_MAX) {
				/* No more free md pages. Cannot satisfy the request */
				bs_release_cluster(blob->bs, *cluster); /* [한국어] cluster 롤백 */
				return -ENOSPC;
			}
			bs_claim_md_page(blob->bs, *lowest_free_md_page); /* [한국어] extent page 자리 마킹 */
		}
	}

	SPDK_DEBUGLOG(blob, "Claiming cluster %" PRIu64 " for blob 0x%" PRIx64 "\n", *cluster,
		      blob->id);

	if (update_map) {                                  /* [한국어] 즉시 blob 매핑 반영 옵션 */
		blob_insert_cluster(blob, cluster_num, *cluster);
		if (blob->use_extent_table && *extent_page == 0) {
			*extent_page = *lowest_free_md_page; /* [한국어] extent page 슬롯에 페이지 번호 기록 */
		}
	}

	return 0;
}

/*
 * [한국어]
 * blob_xattrs_init - xattr 옵션 구조체를 빈 상태로 초기화
 *
 * spdk_blob_opts_init이 호출 — opts.xattrs 의 모든 필드를 0/NULL로 세팅.
 */
static void
blob_xattrs_init(struct spdk_blob_xattr_opts *xattrs)
{
	xattrs->count = 0;
	xattrs->names = NULL;
	xattrs->ctx = NULL;
	xattrs->get_value = NULL;
}

/*
 * [한국어]
 * spdk_blob_opts_init - blob 생성 옵션 구조체를 안전한 디폴트로 초기화 (공개 API)
 *
 * @opts:      초기화할 옵션 객체
 * @opts_size: 호출자가 인식하는 구조체 크기 (forward-compat)
 *
 * SPDK는 외부 ABI 호환성을 위해 "구조체 크기"를 함께 받는 패턴을 쓴다.
 * - 호출자가 아는 크기(opts_size)만큼 0으로 초기화하고 새 필드는 호출자가 모르는 자리에
 *   있을 수 있으므로 SET_FIELD 매크로(FIELD_OK 검사)로 부분적으로 채운다.
 * 이 패턴 덕분에 SDK 업데이트 시 기존 호출자 바이너리가 깨지지 않는다.
 */
void
spdk_blob_opts_init(struct spdk_blob_opts *opts, size_t opts_size)
{
	if (!opts) {                                       /* [한국어] NULL 검사 */
		SPDK_ERRLOG("opts should not be NULL\n");
		return;
	}

	if (!opts_size) {                                  /* [한국어] 0 크기는 의미 없음 */
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return;
	}

	memset(opts, 0, opts_size);                        /* [한국어] 호출자 인식 범위 zero-init */
	opts->opts_size = opts_size;                       /* [한국어] 라이브러리가 어느 부분 신뢰할지 기록 */

#define FIELD_OK(field) \
        offsetof(struct spdk_blob_opts, field) + sizeof(opts->field) <= opts_size

#define SET_FIELD(field, value) \
        if (FIELD_OK(field)) { \
                opts->field = value; \
        } \

	SET_FIELD(num_clusters, 0);
	SET_FIELD(thin_provision, false);
	SET_FIELD(clear_method, BLOB_CLEAR_WITH_DEFAULT);

	if (FIELD_OK(xattrs)) {
		blob_xattrs_init(&opts->xattrs);
	}

	SET_FIELD(use_extent_table, true);

#undef FIELD_OK
#undef SET_FIELD
}

/*
 * [한국어]
 * spdk_blob_open_opts_init - blob open 옵션 구조체 디폴트 초기화 (공개 API)
 *
 * spdk_blob_opts_init과 동일한 forward-compat 패턴. open 시에는 보통 clear_method 만
 * 의미가 있다.
 */
void
spdk_blob_open_opts_init(struct spdk_blob_open_opts *opts, size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return;
	}

	memset(opts, 0, opts_size);
	opts->opts_size = opts_size;

#define FIELD_OK(field) \
        offsetof(struct spdk_blob_open_opts, field) + sizeof(opts->field) <= opts_size

#define SET_FIELD(field, value) \
        if (FIELD_OK(field)) { \
                opts->field = value; \
        } \

	SET_FIELD(clear_method, BLOB_CLEAR_WITH_DEFAULT);

#undef FIELD_OK
#undef SET_FILED
}

static struct spdk_blob *
blob_alloc(struct spdk_blob_store *bs, spdk_blob_id id)
{
	struct spdk_blob *blob;

	blob = calloc(1, sizeof(*blob));
	if (!blob) {
		return NULL;
	}

	blob->id = id;
	blob->bs = bs;

	blob->parent_id = SPDK_BLOBID_INVALID;

	blob->state = SPDK_BLOB_STATE_DIRTY;
	blob->extent_rle_found = false;
	blob->extent_table_found = false;
	blob->active.num_pages = 1;
	blob->active.pages = calloc(1, sizeof(*blob->active.pages));
	if (!blob->active.pages) {
		free(blob);
		return NULL;
	}

	blob->active.pages[0] = bs_blobid_to_page(id);

	TAILQ_INIT(&blob->xattrs);
	TAILQ_INIT(&blob->xattrs_internal);
	TAILQ_INIT(&blob->pending_persists);
	TAILQ_INIT(&blob->persists_to_complete);

	return blob;
}

static void
xattrs_free(struct spdk_xattr_tailq *xattrs)
{
	struct spdk_xattr	*xattr, *xattr_tmp;

	TAILQ_FOREACH_SAFE(xattr, xattrs, link, xattr_tmp) {
		TAILQ_REMOVE(xattrs, xattr, link);
		free(xattr->name);
		free(xattr->value);
		free(xattr);
	}
}

static void
blob_unref_back_bs_dev(struct spdk_blob *blob)
{
	blob->back_bs_dev->destroy(blob->back_bs_dev);
	blob->back_bs_dev = NULL;
}

static void
blob_free(struct spdk_blob *blob)
{
	assert(blob != NULL);
	assert(TAILQ_EMPTY(&blob->pending_persists));
	assert(TAILQ_EMPTY(&blob->persists_to_complete));

	free(blob->active.extent_pages);
	free(blob->clean.extent_pages);
	free(blob->active.clusters);
	free(blob->clean.clusters);
	free(blob->active.pages);
	free(blob->clean.pages);

	xattrs_free(&blob->xattrs);
	xattrs_free(&blob->xattrs_internal);

	if (blob->back_bs_dev) {
		blob_unref_back_bs_dev(blob);
	}

	free(blob);
}

static void
blob_back_bs_destroy_esnap_done(void *ctx, struct spdk_blob *blob, int bserrno)
{
	struct spdk_bs_dev	*bs_dev = ctx;

	if (bserrno != 0) {
		/*
		 * This is probably due to a memory allocation failure when creating the
		 * blob_esnap_destroy_ctx before iterating threads.
		 */
		SPDK_ERRLOG("blob 0x%" PRIx64 ": Unable to destroy bs dev channels: error %d\n",
			    blob->id, bserrno);
		assert(false);
	}

	if (bs_dev == NULL) {
		/*
		 * This check exists to make scanbuild happy.
		 *
		 * blob->back_bs_dev for an esnap is NULL during the first iteration of blobs while
		 * the blobstore is being loaded. It could also be NULL if there was an error
		 * opening the esnap device. In each of these cases, no channels could have been
		 * created because back_bs_dev->create_channel() would have led to a NULL pointer
		 * deref.
		 */
		assert(false);
		return;
	}

	SPDK_DEBUGLOG(blob_esnap, "blob 0x%" PRIx64 ": calling destroy on back_bs_dev\n", blob->id);
	bs_dev->destroy(bs_dev);
}

static void
blob_back_bs_destroy(struct spdk_blob *blob)
{
	SPDK_DEBUGLOG(blob_esnap, "blob 0x%" PRIx64 ": preparing to destroy back_bs_dev\n",
		      blob->id);

	blob_esnap_destroy_bs_dev_channels(blob, false, blob_back_bs_destroy_esnap_done,
					   blob->back_bs_dev);
	blob->back_bs_dev = NULL;
}

struct blob_parent {
	union {
		struct {
			spdk_blob_id id;
			struct spdk_blob *blob;
		} snapshot;

		struct {
			void *id;
			uint32_t id_len;
			struct spdk_bs_dev *back_bs_dev;
		} esnap;
	} u;
};

typedef int (*set_parent_refs_cb)(struct spdk_blob *blob, struct blob_parent *parent);

struct set_bs_dev_ctx {
	struct spdk_blob	*blob;
	struct spdk_bs_dev	*back_bs_dev;

	/*
	 * This callback is used during a set parent operation to change the references
	 * to the parent of the blob.
	 */
	set_parent_refs_cb	parent_refs_cb_fn;
	struct blob_parent	*parent_refs_cb_arg;

	spdk_blob_op_complete	cb_fn;
	void			*cb_arg;
	int			bserrno;
};

static void
blob_set_back_bs_dev(struct spdk_blob *blob, struct spdk_bs_dev *back_bs_dev,
		     set_parent_refs_cb parent_refs_cb_fn, struct blob_parent *parent_refs_cb_arg,
		     spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct set_bs_dev_ctx	*ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("blob 0x%" PRIx64 ": out of memory while setting back_bs_dev\n",
			    blob->id);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->parent_refs_cb_fn = parent_refs_cb_fn;
	ctx->parent_refs_cb_arg = parent_refs_cb_arg;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->back_bs_dev = back_bs_dev;
	ctx->blob = blob;

	blob_freeze_io(blob, blob_set_back_bs_dev_frozen, ctx);
}

struct freeze_io_ctx {
	struct spdk_bs_cpl cpl;
	struct spdk_blob *blob;
};

static void
blob_io_sync(struct spdk_io_channel_iter *i)
{
	spdk_for_each_channel_continue(i, 0);
}

static void
blob_execute_queued_io(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_bs_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct freeze_io_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_bs_request_set	*set;
	struct spdk_bs_user_op_args	*args;
	spdk_bs_user_op_t *op, *tmp;

	TAILQ_FOREACH_SAFE(op, &ch->queued_io, link, tmp) {
		set = (struct spdk_bs_request_set *)op;
		args = &set->u.user_op;

		if (args->blob == ctx->blob) {
			TAILQ_REMOVE(&ch->queued_io, op, link);
			bs_user_op_execute(op);
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
blob_io_cpl(struct spdk_io_channel_iter *i, int status)
{
	struct freeze_io_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	ctx->cpl.u.blob_basic.cb_fn(ctx->cpl.u.blob_basic.cb_arg, 0);

	free(ctx);
}

static void
blob_freeze_io(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct freeze_io_ctx *ctx;

	blob_verify_md_op(blob);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	ctx->cpl.u.blob_basic.cb_fn = cb_fn;
	ctx->cpl.u.blob_basic.cb_arg = cb_arg;
	ctx->blob = blob;

	/* Freeze I/O on blob */
	blob->frozen_refcnt++;

	spdk_for_each_channel(blob->bs, blob_io_sync, ctx, blob_io_cpl);
}

static void
blob_unfreeze_io(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct freeze_io_ctx *ctx;

	blob_verify_md_op(blob);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	ctx->cpl.u.blob_basic.cb_fn = cb_fn;
	ctx->cpl.u.blob_basic.cb_arg = cb_arg;
	ctx->blob = blob;

	assert(blob->frozen_refcnt > 0);

	blob->frozen_refcnt--;

	spdk_for_each_channel(blob->bs, blob_execute_queued_io, ctx, blob_io_cpl);
}

static int
blob_mark_clean(struct spdk_blob *blob)
{
	uint32_t *extent_pages = NULL;
	uint64_t *clusters = NULL;
	uint32_t *pages = NULL;

	assert(blob != NULL);

	if (blob->active.num_extent_pages) {
		assert(blob->active.extent_pages);
		extent_pages = calloc(blob->active.num_extent_pages, sizeof(*blob->active.extent_pages));
		if (!extent_pages) {
			return -ENOMEM;
		}
		memcpy(extent_pages, blob->active.extent_pages,
		       blob->active.num_extent_pages * sizeof(*extent_pages));
	}

	if (blob->active.num_clusters) {
		assert(blob->active.clusters);
		clusters = calloc(blob->active.num_clusters, sizeof(*blob->active.clusters));
		if (!clusters) {
			free(extent_pages);
			return -ENOMEM;
		}
		memcpy(clusters, blob->active.clusters, blob->active.num_clusters * sizeof(*blob->active.clusters));
	}

	if (blob->active.num_pages) {
		assert(blob->active.pages);
		pages = calloc(blob->active.num_pages, sizeof(*blob->active.pages));
		if (!pages) {
			free(extent_pages);
			free(clusters);
			return -ENOMEM;
		}
		memcpy(pages, blob->active.pages, blob->active.num_pages * sizeof(*blob->active.pages));
	}

	free(blob->clean.extent_pages);
	free(blob->clean.clusters);
	free(blob->clean.pages);

	blob->clean.num_extent_pages = blob->active.num_extent_pages;
	blob->clean.extent_pages = blob->active.extent_pages;
	blob->clean.num_clusters = blob->active.num_clusters;
	blob->clean.clusters = blob->active.clusters;
	blob->clean.num_allocated_clusters = blob->active.num_allocated_clusters;
	blob->clean.num_pages = blob->active.num_pages;
	blob->clean.pages = blob->active.pages;

	blob->active.extent_pages = extent_pages;
	blob->active.clusters = clusters;
	blob->active.pages = pages;

	/* If the metadata was dirtied again while the metadata was being written to disk,
	 *  we do not want to revert the DIRTY state back to CLEAN here.
	 */
	if (blob->state == SPDK_BLOB_STATE_LOADING) {
		blob->state = SPDK_BLOB_STATE_CLEAN;
	}

	return 0;
}

static int
blob_deserialize_xattr(struct spdk_blob *blob,
		       struct spdk_blob_md_descriptor_xattr *desc_xattr, bool internal)
{
	struct spdk_xattr                       *xattr;

	if (desc_xattr->length != sizeof(desc_xattr->name_length) +
	    sizeof(desc_xattr->value_length) +
	    desc_xattr->name_length + desc_xattr->value_length) {
		return -EINVAL;
	}

	xattr = calloc(1, sizeof(*xattr));
	if (xattr == NULL) {
		return -ENOMEM;
	}

	xattr->name = malloc(desc_xattr->name_length + 1);
	if (xattr->name == NULL) {
		free(xattr);
		return -ENOMEM;
	}

	xattr->value = malloc(desc_xattr->value_length);
	if (xattr->value == NULL) {
		free(xattr->name);
		free(xattr);
		return -ENOMEM;
	}

	memcpy(xattr->name, desc_xattr->name, desc_xattr->name_length);
	xattr->name[desc_xattr->name_length] = '\0';
	xattr->value_len = desc_xattr->value_length;
	memcpy(xattr->value,
	       (void *)((uintptr_t)desc_xattr->name + desc_xattr->name_length),
	       desc_xattr->value_length);

	TAILQ_INSERT_TAIL(internal ? &blob->xattrs_internal : &blob->xattrs, xattr, link);

	return 0;
}


static int
blob_parse_page(const struct spdk_blob_md_page *page, struct spdk_blob *blob)
{
	struct spdk_blob_md_descriptor *desc;
	size_t	cur_desc = 0;
	void *tmp;

	desc = (struct spdk_blob_md_descriptor *)page->descriptors;
	while (cur_desc < sizeof(page->descriptors)) {
		if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_PADDING) {
			if (desc->length == 0) {
				/* If padding and length are 0, this terminates the page */
				break;
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_FLAGS) {
			struct spdk_blob_md_descriptor_flags	*desc_flags;

			desc_flags = (struct spdk_blob_md_descriptor_flags *)desc;

			if (desc_flags->length != sizeof(*desc_flags) - sizeof(*desc)) {
				return -EINVAL;
			}

			if ((desc_flags->invalid_flags | SPDK_BLOB_INVALID_FLAGS_MASK) !=
			    SPDK_BLOB_INVALID_FLAGS_MASK) {
				return -EINVAL;
			}

			if ((desc_flags->data_ro_flags | SPDK_BLOB_DATA_RO_FLAGS_MASK) !=
			    SPDK_BLOB_DATA_RO_FLAGS_MASK) {
				blob->data_ro = true;
				blob->md_ro = true;
			}

			if ((desc_flags->md_ro_flags | SPDK_BLOB_MD_RO_FLAGS_MASK) !=
			    SPDK_BLOB_MD_RO_FLAGS_MASK) {
				blob->md_ro = true;
			}

			if ((desc_flags->data_ro_flags & SPDK_BLOB_READ_ONLY)) {
				blob->data_ro = true;
				blob->md_ro = true;
			}

			blob->invalid_flags = desc_flags->invalid_flags;
			blob->data_ro_flags = desc_flags->data_ro_flags;
			blob->md_ro_flags = desc_flags->md_ro_flags;

		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_RLE) {
			struct spdk_blob_md_descriptor_extent_rle	*desc_extent_rle;
			unsigned int				i, j;
			unsigned int				cluster_count = blob->active.num_clusters;

			if (blob->extent_table_found) {
				/* Extent Table already present in the md,
				 * both descriptors should never be at the same time. */
				return -EINVAL;
			}
			blob->extent_rle_found = true;

			desc_extent_rle = (struct spdk_blob_md_descriptor_extent_rle *)desc;

			if (desc_extent_rle->length == 0 ||
			    (desc_extent_rle->length % sizeof(desc_extent_rle->extents[0]) != 0)) {
				return -EINVAL;
			}

			for (i = 0; i < desc_extent_rle->length / sizeof(desc_extent_rle->extents[0]); i++) {
				for (j = 0; j < desc_extent_rle->extents[i].length; j++) {
					if (desc_extent_rle->extents[i].cluster_idx != 0) {
						if (!spdk_bit_pool_is_allocated(blob->bs->used_clusters,
										desc_extent_rle->extents[i].cluster_idx + j)) {
							return -EINVAL;
						}
					}
					cluster_count++;
				}
			}

			if (cluster_count == 0) {
				return -EINVAL;
			}
			tmp = realloc(blob->active.clusters, cluster_count * sizeof(*blob->active.clusters));
			if (tmp == NULL) {
				return -ENOMEM;
			}
			blob->active.clusters = tmp;
			blob->active.cluster_array_size = cluster_count;

			for (i = 0; i < desc_extent_rle->length / sizeof(desc_extent_rle->extents[0]); i++) {
				for (j = 0; j < desc_extent_rle->extents[i].length; j++) {
					if (desc_extent_rle->extents[i].cluster_idx != 0) {
						blob->active.clusters[blob->active.num_clusters++] = bs_cluster_to_lba(blob->bs,
								desc_extent_rle->extents[i].cluster_idx + j);
						blob->active.num_allocated_clusters++;
					} else if (spdk_blob_is_thin_provisioned(blob)) {
						blob->active.clusters[blob->active.num_clusters++] = 0;
					} else {
						return -EINVAL;
					}
				}
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_TABLE) {
			struct spdk_blob_md_descriptor_extent_table *desc_extent_table;
			uint32_t num_extent_pages = blob->active.num_extent_pages;
			uint32_t i, j;
			size_t extent_pages_length;

			desc_extent_table = (struct spdk_blob_md_descriptor_extent_table *)desc;
			extent_pages_length = desc_extent_table->length - sizeof(desc_extent_table->num_clusters);

			if (blob->extent_rle_found) {
				/* This means that Extent RLE is present in MD,
				 * both should never be at the same time. */
				return -EINVAL;
			} else if (blob->extent_table_found &&
				   desc_extent_table->num_clusters != blob->remaining_clusters_in_et) {
				/* Number of clusters in this ET does not match number
				 * from previously read EXTENT_TABLE. */
				return -EINVAL;
			}

			if (desc_extent_table->length == 0 ||
			    (extent_pages_length % sizeof(desc_extent_table->extent_page[0]) != 0)) {
				return -EINVAL;
			}

			blob->extent_table_found = true;

			for (i = 0; i < extent_pages_length / sizeof(desc_extent_table->extent_page[0]); i++) {
				num_extent_pages += desc_extent_table->extent_page[i].num_pages;
			}

			if (num_extent_pages > 0) {
				tmp = realloc(blob->active.extent_pages, num_extent_pages * sizeof(uint32_t));
				if (tmp == NULL) {
					return -ENOMEM;
				}
				blob->active.extent_pages = tmp;
			}
			blob->active.extent_pages_array_size = num_extent_pages;

			blob->remaining_clusters_in_et = desc_extent_table->num_clusters;

			/* Extent table entries contain md page numbers for extent pages.
			 * Zeroes represent unallocated extent pages, those are run-length-encoded.
			 */
			for (i = 0; i < extent_pages_length / sizeof(desc_extent_table->extent_page[0]); i++) {
				if (desc_extent_table->extent_page[i].page_idx != 0) {
					assert(desc_extent_table->extent_page[i].num_pages == 1);
					blob->active.extent_pages[blob->active.num_extent_pages++] =
						desc_extent_table->extent_page[i].page_idx;
				} else if (spdk_blob_is_thin_provisioned(blob)) {
					for (j = 0; j < desc_extent_table->extent_page[i].num_pages; j++) {
						blob->active.extent_pages[blob->active.num_extent_pages++] = 0;
					}
				} else {
					return -EINVAL;
				}
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_PAGE) {
			struct spdk_blob_md_descriptor_extent_page	*desc_extent;
			unsigned int					i;
			unsigned int					cluster_count = 0;
			size_t						cluster_idx_length;

			if (blob->extent_rle_found) {
				/* This means that Extent RLE is present in MD,
				 * both should never be at the same time. */
				return -EINVAL;
			}

			desc_extent = (struct spdk_blob_md_descriptor_extent_page *)desc;
			cluster_idx_length = desc_extent->length - sizeof(desc_extent->start_cluster_idx);

			if (desc_extent->length <= sizeof(desc_extent->start_cluster_idx) ||
			    (cluster_idx_length % sizeof(desc_extent->cluster_idx[0]) != 0)) {
				return -EINVAL;
			}

			for (i = 0; i < cluster_idx_length / sizeof(desc_extent->cluster_idx[0]); i++) {
				if (desc_extent->cluster_idx[i] != 0) {
					if (!spdk_bit_pool_is_allocated(blob->bs->used_clusters, desc_extent->cluster_idx[i])) {
						return -EINVAL;
					}
				}
				cluster_count++;
			}

			if (cluster_count == 0) {
				return -EINVAL;
			}

			/* When reading extent pages sequentially starting cluster idx should match
			 * current size of a blob.
			 * If changed to batch reading, this check shall be removed. */
			if (desc_extent->start_cluster_idx != blob->active.num_clusters) {
				return -EINVAL;
			}

			tmp = realloc(blob->active.clusters,
				      (cluster_count + blob->active.num_clusters) * sizeof(*blob->active.clusters));
			if (tmp == NULL) {
				return -ENOMEM;
			}
			blob->active.clusters = tmp;
			blob->active.cluster_array_size = (cluster_count + blob->active.num_clusters);

			for (i = 0; i < cluster_idx_length / sizeof(desc_extent->cluster_idx[0]); i++) {
				if (desc_extent->cluster_idx[i] != 0) {
					blob->active.clusters[blob->active.num_clusters++] = bs_cluster_to_lba(blob->bs,
							desc_extent->cluster_idx[i]);
					blob->active.num_allocated_clusters++;
				} else if (spdk_blob_is_thin_provisioned(blob)) {
					blob->active.clusters[blob->active.num_clusters++] = 0;
				} else {
					return -EINVAL;
				}
			}
			assert(desc_extent->start_cluster_idx + cluster_count == blob->active.num_clusters);
			assert(blob->remaining_clusters_in_et >= cluster_count);
			blob->remaining_clusters_in_et -= cluster_count;
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR) {
			int rc;

			rc = blob_deserialize_xattr(blob,
						    (struct spdk_blob_md_descriptor_xattr *) desc, false);
			if (rc != 0) {
				return rc;
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR_INTERNAL) {
			int rc;

			rc = blob_deserialize_xattr(blob,
						    (struct spdk_blob_md_descriptor_xattr *) desc, true);
			if (rc != 0) {
				return rc;
			}
		} else {
			/* Unrecognized descriptor type.  Do not fail - just continue to the
			 *  next descriptor.  If this descriptor is associated with some feature
			 *  defined in a newer version of blobstore, that version of blobstore
			 *  should create and set an associated feature flag to specify if this
			 *  blob can be loaded or not.
			 */
		}

		/* Advance to the next descriptor */
		cur_desc += sizeof(*desc) + desc->length;
		if (cur_desc + sizeof(*desc) > sizeof(page->descriptors)) {
			break;
		}
		desc = (struct spdk_blob_md_descriptor *)((uintptr_t)page->descriptors + cur_desc);
	}

	return 0;
}

static bool bs_load_cur_extent_page_valid(struct spdk_blob_md_page *page);

static int
blob_parse_extent_page(struct spdk_blob_md_page *extent_page, struct spdk_blob *blob)
{
	assert(blob != NULL);
	assert(blob->state == SPDK_BLOB_STATE_LOADING);

	if (bs_load_cur_extent_page_valid(extent_page) == false) {
		return -ENOENT;
	}

	return blob_parse_page(extent_page, blob);
}

static int
blob_parse(const struct spdk_blob_md_page *pages, uint32_t page_count,
	   struct spdk_blob *blob)
{
	const struct spdk_blob_md_page *page;
	uint32_t i;
	int rc;
	void *tmp;

	assert(page_count > 0);
	assert(pages[0].sequence_num == 0);
	assert(blob != NULL);
	assert(blob->state == SPDK_BLOB_STATE_LOADING);
	assert(blob->active.clusters == NULL);

	/* The blobid provided doesn't match what's in the MD, this can
	 * happen for example if a bogus blobid is passed in through open.
	 */
	if (blob->id != pages[0].id) {
		SPDK_ERRLOG("Blobid (0x%" PRIx64 ") doesn't match what's in metadata "
			    "(0x%" PRIx64 ")\n", blob->id, pages[0].id);
		return -ENOENT;
	}

	tmp = realloc(blob->active.pages, page_count * sizeof(*blob->active.pages));
	if (!tmp) {
		return -ENOMEM;
	}
	blob->active.pages = tmp;

	blob->active.pages[0] = pages[0].id;

	for (i = 1; i < page_count; i++) {
		assert(spdk_bit_array_get(blob->bs->used_md_pages, pages[i - 1].next));
		blob->active.pages[i] = pages[i - 1].next;
	}
	blob->active.num_pages = page_count;

	for (i = 0; i < page_count; i++) {
		page = &pages[i];

		assert(page->id == blob->id);
		assert(page->sequence_num == i);

		rc = blob_parse_page(page, blob);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int
blob_serialize_add_page(const struct spdk_blob *blob,
			struct spdk_blob_md_page **pages,
			uint32_t *page_count,
			struct spdk_blob_md_page **last_page)
{
	struct spdk_blob_md_page *page, *tmp_pages;

	assert(pages != NULL);
	assert(page_count != NULL);

	*last_page = NULL;
	if (*page_count == 0) {
		assert(*pages == NULL);
		*pages = spdk_malloc(blob->bs->md_page_size, 0,
				     NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (*pages == NULL) {
			return -ENOMEM;
		}
		*page_count = 1;
	} else {
		assert(*pages != NULL);
		tmp_pages = spdk_realloc(*pages, blob->bs->md_page_size * (*page_count + 1), 0);
		if (tmp_pages == NULL) {
			return -ENOMEM;
		}
		(*page_count)++;
		*pages = tmp_pages;
	}

	page = &(*pages)[*page_count - 1];
	memset(page, 0, sizeof(*page));
	page->id = blob->id;
	page->sequence_num = *page_count - 1;
	page->next = SPDK_INVALID_MD_PAGE;
	*last_page = page;

	return 0;
}

/* Transform the in-memory representation 'xattr' into an on-disk xattr descriptor.
 * Update required_sz on both success and failure.
 *
 */
static int
blob_serialize_xattr(const struct spdk_xattr *xattr,
		     uint8_t *buf, size_t buf_sz,
		     size_t *required_sz, bool internal)
{
	struct spdk_blob_md_descriptor_xattr	*desc;

	*required_sz = sizeof(struct spdk_blob_md_descriptor_xattr) +
		       strlen(xattr->name) +
		       xattr->value_len;

	if (buf_sz < *required_sz) {
		return -1;
	}

	desc = (struct spdk_blob_md_descriptor_xattr *)buf;

	desc->type = internal ? SPDK_MD_DESCRIPTOR_TYPE_XATTR_INTERNAL : SPDK_MD_DESCRIPTOR_TYPE_XATTR;
	desc->length = sizeof(desc->name_length) +
		       sizeof(desc->value_length) +
		       strlen(xattr->name) +
		       xattr->value_len;
	desc->name_length = strlen(xattr->name);
	desc->value_length = xattr->value_len;

	memcpy(desc->name, xattr->name, desc->name_length);
	memcpy((void *)((uintptr_t)desc->name + desc->name_length),
	       xattr->value,
	       desc->value_length);

	return 0;
}

static void
blob_serialize_extent_table_entry(const struct spdk_blob *blob,
				  uint64_t start_ep, uint64_t *next_ep,
				  uint8_t **buf, size_t *remaining_sz)
{
	struct spdk_blob_md_descriptor_extent_table *desc;
	size_t cur_sz;
	uint64_t i, et_idx;
	uint32_t extent_page, ep_len;

	/* The buffer must have room for at least num_clusters entry */
	cur_sz = sizeof(struct spdk_blob_md_descriptor) + sizeof(desc->num_clusters);
	if (*remaining_sz < cur_sz) {
		*next_ep = start_ep;
		return;
	}

	desc = (struct spdk_blob_md_descriptor_extent_table *)*buf;
	desc->type = SPDK_MD_DESCRIPTOR_TYPE_EXTENT_TABLE;

	desc->num_clusters = blob->active.num_clusters;

	ep_len = 1;
	et_idx = 0;
	for (i = start_ep; i < blob->active.num_extent_pages; i++) {
		if (*remaining_sz < cur_sz  + sizeof(desc->extent_page[0])) {
			/* If we ran out of buffer space, return */
			break;
		}

		extent_page = blob->active.extent_pages[i];
		/* Verify that next extent_page is unallocated */
		if (extent_page == 0 &&
		    (i + 1 < blob->active.num_extent_pages && blob->active.extent_pages[i + 1] == 0)) {
			ep_len++;
			continue;
		}
		desc->extent_page[et_idx].page_idx = extent_page;
		desc->extent_page[et_idx].num_pages = ep_len;
		et_idx++;

		ep_len = 1;
		cur_sz += sizeof(desc->extent_page[et_idx]);
	}
	*next_ep = i;

	desc->length = sizeof(desc->num_clusters) + sizeof(desc->extent_page[0]) * et_idx;
	*remaining_sz -= sizeof(struct spdk_blob_md_descriptor) + desc->length;
	*buf += sizeof(struct spdk_blob_md_descriptor) + desc->length;
}

static int
blob_serialize_extent_table(const struct spdk_blob *blob,
			    struct spdk_blob_md_page **pages,
			    struct spdk_blob_md_page *cur_page,
			    uint32_t *page_count, uint8_t **buf,
			    size_t *remaining_sz)
{
	uint64_t				last_extent_page;
	int					rc;

	last_extent_page = 0;
	/* At least single extent table entry has to be always persisted.
	 * Such case occurs with num_extent_pages == 0. */
	while (last_extent_page <= blob->active.num_extent_pages) {
		blob_serialize_extent_table_entry(blob, last_extent_page, &last_extent_page, buf,
						  remaining_sz);

		if (last_extent_page == blob->active.num_extent_pages) {
			break;
		}

		rc = blob_serialize_add_page(blob, pages, page_count, &cur_page);
		if (rc < 0) {
			return rc;
		}

		*buf = (uint8_t *)cur_page->descriptors;
		*remaining_sz = sizeof(cur_page->descriptors);
	}

	return 0;
}

static void
blob_serialize_extent_rle(const struct spdk_blob *blob,
			  uint64_t start_cluster, uint64_t *next_cluster,
			  uint8_t **buf, size_t *buf_sz)
{
	struct spdk_blob_md_descriptor_extent_rle *desc_extent_rle;
	size_t cur_sz;
	uint64_t i, extent_idx;
	uint64_t lba, lba_per_cluster, lba_count;

	/* The buffer must have room for at least one extent */
	cur_sz = sizeof(struct spdk_blob_md_descriptor) + sizeof(desc_extent_rle->extents[0]);
	if (*buf_sz < cur_sz) {
		*next_cluster = start_cluster;
		return;
	}

	desc_extent_rle = (struct spdk_blob_md_descriptor_extent_rle *)*buf;
	desc_extent_rle->type = SPDK_MD_DESCRIPTOR_TYPE_EXTENT_RLE;

	lba_per_cluster = bs_cluster_to_lba(blob->bs, 1);
	/* Assert for scan-build false positive */
	assert(lba_per_cluster > 0);

	lba = blob->active.clusters[start_cluster];
	lba_count = lba_per_cluster;
	extent_idx = 0;
	for (i = start_cluster + 1; i < blob->active.num_clusters; i++) {
		if ((lba + lba_count) == blob->active.clusters[i] && lba != 0) {
			/* Run-length encode sequential non-zero LBA */
			lba_count += lba_per_cluster;
			continue;
		} else if (lba == 0 && blob->active.clusters[i] == 0) {
			/* Run-length encode unallocated clusters */
			lba_count += lba_per_cluster;
			continue;
		}
		desc_extent_rle->extents[extent_idx].cluster_idx = lba / lba_per_cluster;
		desc_extent_rle->extents[extent_idx].length = lba_count / lba_per_cluster;
		extent_idx++;

		cur_sz += sizeof(desc_extent_rle->extents[extent_idx]);

		if (*buf_sz < cur_sz) {
			/* If we ran out of buffer space, return */
			*next_cluster = i;
			break;
		}

		lba = blob->active.clusters[i];
		lba_count = lba_per_cluster;
	}

	if (*buf_sz >= cur_sz) {
		desc_extent_rle->extents[extent_idx].cluster_idx = lba / lba_per_cluster;
		desc_extent_rle->extents[extent_idx].length = lba_count / lba_per_cluster;
		extent_idx++;

		*next_cluster = blob->active.num_clusters;
	}

	desc_extent_rle->length = sizeof(desc_extent_rle->extents[0]) * extent_idx;
	*buf_sz -= sizeof(struct spdk_blob_md_descriptor) + desc_extent_rle->length;
	*buf += sizeof(struct spdk_blob_md_descriptor) + desc_extent_rle->length;
}

static int
blob_serialize_extents_rle(const struct spdk_blob *blob,
			   struct spdk_blob_md_page **pages,
			   struct spdk_blob_md_page *cur_page,
			   uint32_t *page_count, uint8_t **buf,
			   size_t *remaining_sz)
{
	uint64_t				last_cluster;
	int					rc;

	last_cluster = 0;
	while (last_cluster < blob->active.num_clusters) {
		blob_serialize_extent_rle(blob, last_cluster, &last_cluster, buf, remaining_sz);

		if (last_cluster == blob->active.num_clusters) {
			break;
		}

		rc = blob_serialize_add_page(blob, pages, page_count, &cur_page);
		if (rc < 0) {
			return rc;
		}

		*buf = (uint8_t *)cur_page->descriptors;
		*remaining_sz = sizeof(cur_page->descriptors);
	}

	return 0;
}

static void
blob_serialize_extent_page(const struct spdk_blob *blob,
			   uint64_t cluster, struct spdk_blob_md_page *page)
{
	struct spdk_blob_md_descriptor_extent_page *desc_extent;
	uint64_t i, extent_idx;
	uint64_t lba, lba_per_cluster;
	uint64_t start_cluster_idx = (cluster / SPDK_EXTENTS_PER_EP) * SPDK_EXTENTS_PER_EP;

	desc_extent = (struct spdk_blob_md_descriptor_extent_page *) page->descriptors;
	desc_extent->type = SPDK_MD_DESCRIPTOR_TYPE_EXTENT_PAGE;

	lba_per_cluster = bs_cluster_to_lba(blob->bs, 1);

	desc_extent->start_cluster_idx = start_cluster_idx;
	extent_idx = 0;
	for (i = start_cluster_idx; i < blob->active.num_clusters; i++) {
		lba = blob->active.clusters[i];
		desc_extent->cluster_idx[extent_idx++] = lba / lba_per_cluster;
		if (extent_idx >= SPDK_EXTENTS_PER_EP) {
			break;
		}
	}
	desc_extent->length = sizeof(desc_extent->start_cluster_idx) +
			      sizeof(desc_extent->cluster_idx[0]) * extent_idx;
}

static void
blob_serialize_flags(const struct spdk_blob *blob,
		     uint8_t *buf, size_t *buf_sz)
{
	struct spdk_blob_md_descriptor_flags *desc;

	/*
	 * Flags get serialized first, so we should always have room for the flags
	 *  descriptor.
	 */
	assert(*buf_sz >= sizeof(*desc));

	desc = (struct spdk_blob_md_descriptor_flags *)buf;
	desc->type = SPDK_MD_DESCRIPTOR_TYPE_FLAGS;
	desc->length = sizeof(*desc) - sizeof(struct spdk_blob_md_descriptor);
	desc->invalid_flags = blob->invalid_flags;
	desc->data_ro_flags = blob->data_ro_flags;
	desc->md_ro_flags = blob->md_ro_flags;

	*buf_sz -= sizeof(*desc);
}

static int
blob_serialize_xattrs(const struct spdk_blob *blob,
		      const struct spdk_xattr_tailq *xattrs, bool internal,
		      struct spdk_blob_md_page **pages,
		      struct spdk_blob_md_page *cur_page,
		      uint32_t *page_count, uint8_t **buf,
		      size_t *remaining_sz)
{
	const struct spdk_xattr	*xattr;
	int	rc;

	TAILQ_FOREACH(xattr, xattrs, link) {
		size_t required_sz = 0;

		rc = blob_serialize_xattr(xattr,
					  *buf, *remaining_sz,
					  &required_sz, internal);
		if (rc < 0) {
			/* Need to add a new page to the chain */
			rc = blob_serialize_add_page(blob, pages, page_count,
						     &cur_page);
			if (rc < 0) {
				spdk_free(*pages);
				*pages = NULL;
				*page_count = 0;
				return rc;
			}

			*buf = (uint8_t *)cur_page->descriptors;
			*remaining_sz = sizeof(cur_page->descriptors);

			/* Try again */
			required_sz = 0;
			rc = blob_serialize_xattr(xattr,
						  *buf, *remaining_sz,
						  &required_sz, internal);

			if (rc < 0) {
				spdk_free(*pages);
				*pages = NULL;
				*page_count = 0;
				return rc;
			}
		}

		*remaining_sz -= required_sz;
		*buf += required_sz;
	}

	return 0;
}

static int
blob_serialize(const struct spdk_blob *blob, struct spdk_blob_md_page **pages,
	       uint32_t *page_count)
{
	struct spdk_blob_md_page		*cur_page;
	int					rc;
	uint8_t					*buf;
	size_t					remaining_sz;

	assert(pages != NULL);
	assert(page_count != NULL);
	assert(blob != NULL);
	assert(blob->state == SPDK_BLOB_STATE_DIRTY);

	*pages = NULL;
	*page_count = 0;

	/* A blob always has at least 1 page, even if it has no descriptors */
	rc = blob_serialize_add_page(blob, pages, page_count, &cur_page);
	if (rc < 0) {
		return rc;
	}

	buf = (uint8_t *)cur_page->descriptors;
	remaining_sz = sizeof(cur_page->descriptors);

	/* Serialize flags */
	blob_serialize_flags(blob, buf, &remaining_sz);
	buf += sizeof(struct spdk_blob_md_descriptor_flags);

	/* Serialize xattrs */
	rc = blob_serialize_xattrs(blob, &blob->xattrs, false,
				   pages, cur_page, page_count, &buf, &remaining_sz);
	if (rc < 0) {
		return rc;
	}

	/* Serialize internal xattrs */
	rc = blob_serialize_xattrs(blob, &blob->xattrs_internal, true,
				   pages, cur_page, page_count, &buf, &remaining_sz);
	if (rc < 0) {
		return rc;
	}

	if (blob->use_extent_table) {
		/* Serialize extent table */
		rc = blob_serialize_extent_table(blob, pages, cur_page, page_count, &buf, &remaining_sz);
	} else {
		/* Serialize extents */
		rc = blob_serialize_extents_rle(blob, pages, cur_page, page_count, &buf, &remaining_sz);
	}

	return rc;
}

struct spdk_blob_load_ctx {
	struct spdk_blob		*blob;

	struct spdk_blob_md_page	*pages;
	uint32_t			num_pages;
	uint32_t			next_extent_page;
	spdk_bs_sequence_t	        *seq;

	spdk_bs_sequence_cpl		cb_fn;
	void				*cb_arg;
};

static uint32_t
blob_md_page_calc_crc(void *page)
{
	uint32_t		crc;

	crc = BLOB_CRC32C_INITIAL;
	crc = spdk_crc32c_update(page, SPDK_BS_PAGE_SIZE - 4, crc);
	crc ^= BLOB_CRC32C_INITIAL;

	return crc;

}

static void
blob_load_final(struct spdk_blob_load_ctx *ctx, int bserrno)
{
	struct spdk_blob		*blob = ctx->blob;

	if (bserrno == 0) {
		blob_mark_clean(blob);
	}

	ctx->cb_fn(ctx->seq, ctx->cb_arg, bserrno);

	/* Free the memory */
	spdk_free(ctx->pages);
	free(ctx);
}

static void
blob_load_snapshot_cpl(void *cb_arg, struct spdk_blob *snapshot, int bserrno)
{
	struct spdk_blob_load_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;

	if (bserrno == 0) {
		blob->back_bs_dev = bs_create_blob_bs_dev(snapshot);
		if (blob->back_bs_dev == NULL) {
			bserrno = -ENOMEM;
		}
	}
	if (bserrno != 0) {
		SPDK_ERRLOG("Snapshot fail\n");
	}

	blob_load_final(ctx, bserrno);
}

static void blob_update_clear_method(struct spdk_blob *blob);

static int
blob_load_esnap(struct spdk_blob *blob, void *blob_ctx)
{
	struct spdk_blob_store *bs = blob->bs;
	struct spdk_bs_dev *bs_dev = NULL;
	const void *esnap_id = NULL;
	size_t id_len = 0;
	int rc;

	if (bs->esnap_bs_dev_create == NULL) {
		SPDK_NOTICELOG("blob 0x%" PRIx64 " is an esnap clone but the blobstore was opened "
			       "without support for esnap clones\n", blob->id);
		return -ENOTSUP;
	}
	assert(blob->back_bs_dev == NULL);

	rc = blob_get_xattr_value(blob, BLOB_EXTERNAL_SNAPSHOT_ID, &esnap_id, &id_len, true);
	if (rc != 0) {
		SPDK_ERRLOG("blob 0x%" PRIx64 " is an esnap clone but has no esnap ID\n", blob->id);
		return -EINVAL;
	}
	assert(id_len > 0 && id_len < UINT32_MAX);

	SPDK_INFOLOG(blob, "Creating external snapshot device\n");

	rc = bs->esnap_bs_dev_create(bs->esnap_ctx, blob_ctx, blob, esnap_id, (uint32_t)id_len,
				     &bs_dev);
	if (rc != 0) {
		SPDK_DEBUGLOG(blob_esnap, "blob 0x%" PRIx64 ": failed to load back_bs_dev "
			      "with error %d\n", blob->id, rc);
		return rc;
	}

	/*
	 * Note: bs_dev might be NULL if the consumer chose to not open the external snapshot.
	 * This especially might happen during spdk_bs_load() iteration.
	 */
	if (bs_dev != NULL) {
		SPDK_DEBUGLOG(blob_esnap, "blob 0x%" PRIx64 ": loaded back_bs_dev\n", blob->id);
		if ((bs->io_unit_size % bs_dev->blocklen) != 0) {
			SPDK_NOTICELOG("blob 0x%" PRIx64 " external snapshot device block size %u "
				       "is not compatible with blobstore block size %u\n",
				       blob->id, bs_dev->blocklen, bs->io_unit_size);
			bs_dev->destroy(bs_dev);
			return -EINVAL;
		}
	}

	blob->back_bs_dev = bs_dev;
	blob->parent_id = SPDK_BLOBID_EXTERNAL_SNAPSHOT;

	return 0;
}

static void
blob_load_backing_dev(spdk_bs_sequence_t *seq, void *cb_arg)
{
	struct spdk_blob_load_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	const void			*value;
	size_t				len;
	int				rc;

	if (blob_is_esnap_clone(blob)) {
		rc = blob_load_esnap(blob, seq->cpl.u.blob_handle.esnap_ctx);
		blob_load_final(ctx, rc);
		return;
	}

	if (spdk_blob_is_thin_provisioned(blob)) {
		rc = blob_get_xattr_value(blob, BLOB_SNAPSHOT, &value, &len, true);
		if (rc == 0) {
			if (len != sizeof(spdk_blob_id)) {
				blob_load_final(ctx, -EINVAL);
				return;
			}
			/* open snapshot blob and continue in the callback function */
			blob->parent_id = *(spdk_blob_id *)value;
			spdk_bs_open_blob(blob->bs, blob->parent_id,
					  blob_load_snapshot_cpl, ctx);
			return;
		} else {
			/* add zeroes_dev for thin provisioned blob */
			blob->back_bs_dev = bs_create_zeroes_dev();
		}
	} else {
		/* standard blob */
		blob->back_bs_dev = NULL;
	}
	blob_load_final(ctx, 0);
}

static void
blob_load_cpl_extents_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_load_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_md_page	*page;
	uint64_t			i;
	uint32_t			crc;
	uint64_t			lba;
	void				*tmp;
	uint64_t			sz;

	if (bserrno) {
		SPDK_ERRLOG("Extent page read failed: %d\n", bserrno);
		blob_load_final(ctx, bserrno);
		return;
	}

	if (ctx->pages == NULL) {
		/* First iteration of this function, allocate buffer for single EXTENT_PAGE */
		ctx->pages = spdk_zmalloc(blob->bs->md_page_size, 0,
					  NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (!ctx->pages) {
			blob_load_final(ctx, -ENOMEM);
			return;
		}
		ctx->num_pages = 1;
		ctx->next_extent_page = 0;
	} else {
		page = &ctx->pages[0];
		crc = blob_md_page_calc_crc(page);
		if (crc != page->crc) {
			blob_load_final(ctx, -EINVAL);
			return;
		}

		if (page->next != SPDK_INVALID_MD_PAGE) {
			blob_load_final(ctx, -EINVAL);
			return;
		}

		bserrno = blob_parse_extent_page(page, blob);
		if (bserrno) {
			blob_load_final(ctx, bserrno);
			return;
		}
	}

	for (i = ctx->next_extent_page; i < blob->active.num_extent_pages; i++) {
		if (blob->active.extent_pages[i] != 0) {
			/* Extent page was allocated, read and parse it. */
			lba = bs_md_page_to_lba(blob->bs, blob->active.extent_pages[i]);
			ctx->next_extent_page = i + 1;

			bs_sequence_read_dev(seq, &ctx->pages[0], lba,
					     bs_byte_to_lba(blob->bs, blob->bs->md_page_size),
					     blob_load_cpl_extents_cpl, ctx);
			return;
		} else {
			/* Thin provisioned blobs can point to unallocated extent pages.
			 * In this case blob size should be increased by up to the amount left in remaining_clusters_in_et. */

			sz = spdk_min(blob->remaining_clusters_in_et, SPDK_EXTENTS_PER_EP);
			blob->active.num_clusters += sz;
			blob->remaining_clusters_in_et -= sz;

			assert(spdk_blob_is_thin_provisioned(blob));
			assert(i + 1 < blob->active.num_extent_pages || blob->remaining_clusters_in_et == 0);

			tmp = realloc(blob->active.clusters, blob->active.num_clusters * sizeof(*blob->active.clusters));
			if (tmp == NULL) {
				blob_load_final(ctx, -ENOMEM);
				return;
			}
			memset(tmp + sizeof(*blob->active.clusters) * blob->active.cluster_array_size, 0,
			       sizeof(*blob->active.clusters) * (blob->active.num_clusters - blob->active.cluster_array_size));
			blob->active.clusters = tmp;
			blob->active.cluster_array_size = blob->active.num_clusters;
		}
	}

	blob_load_backing_dev(seq, ctx);
}

static void
blob_load_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_load_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_md_page	*page;
	int				rc;
	uint32_t			crc;
	uint32_t			current_page;

	if (ctx->num_pages == 1) {
		current_page = bs_blobid_to_page(blob->id);
	} else {
		assert(ctx->num_pages != 0);
		page = &ctx->pages[ctx->num_pages - 2];
		current_page = page->next;
	}

	if (bserrno) {
		SPDK_ERRLOG("Metadata page %d read failed for blobid 0x%" PRIx64 ": %d\n",
			    current_page, blob->id, bserrno);
		blob_load_final(ctx, bserrno);
		return;
	}

	page = &ctx->pages[ctx->num_pages - 1];
	crc = blob_md_page_calc_crc(page);
	if (crc != page->crc) {
		SPDK_ERRLOG("Metadata page %d crc mismatch for blobid 0x%" PRIx64 "\n",
			    current_page, blob->id);
		blob_load_final(ctx, -EINVAL);
		return;
	}

	if (page->next != SPDK_INVALID_MD_PAGE) {
		struct spdk_blob_md_page *tmp_pages;
		uint32_t next_page = page->next;
		uint64_t next_lba = bs_md_page_to_lba(blob->bs, next_page);

		/* Read the next page */
		tmp_pages = spdk_realloc(ctx->pages, (sizeof(*page) * (ctx->num_pages + 1)), 0);
		if (tmp_pages == NULL) {
			blob_load_final(ctx, -ENOMEM);
			return;
		}
		ctx->num_pages++;
		ctx->pages = tmp_pages;

		bs_sequence_read_dev(seq, &ctx->pages[ctx->num_pages - 1],
				     next_lba,
				     bs_byte_to_lba(blob->bs, sizeof(*page)),
				     blob_load_cpl, ctx);
		return;
	}

	/* Parse the pages */
	rc = blob_parse(ctx->pages, ctx->num_pages, blob);
	if (rc) {
		blob_load_final(ctx, rc);
		return;
	}

	if (blob->extent_table_found == true) {
		/* If EXTENT_TABLE was found, that means support for it should be enabled. */
		assert(blob->extent_rle_found == false);
		blob->use_extent_table = true;
	} else {
		/* If EXTENT_RLE or no extent_* descriptor was found disable support
		 * for extent table. No extent_* descriptors means that blob has length of 0
		 * and no extent_rle descriptors were persisted for it.
		 * EXTENT_TABLE if used, is always present in metadata regardless of length. */
		blob->use_extent_table = false;
	}

	/* Check the clear_method stored in metadata vs what may have been passed
	 * via spdk_bs_open_blob_ext() and update accordingly.
	 */
	blob_update_clear_method(blob);

	spdk_free(ctx->pages);
	ctx->pages = NULL;

	if (blob->extent_table_found) {
		blob_load_cpl_extents_cpl(seq, ctx, 0);
	} else {
		blob_load_backing_dev(seq, ctx);
	}
}

/* Load a blob from disk given a blobid */
static void
blob_load(spdk_bs_sequence_t *seq, struct spdk_blob *blob,
	  spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_blob_load_ctx *ctx;
	struct spdk_blob_store *bs;
	uint32_t page_num;
	uint64_t lba;

	blob_verify_md_op(blob);

	bs = blob->bs;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(seq, cb_arg, -ENOMEM);
		return;
	}

	ctx->blob = blob;
	ctx->pages = spdk_realloc(ctx->pages, bs->md_page_size, 0);
	if (!ctx->pages) {
		free(ctx);
		cb_fn(seq, cb_arg, -ENOMEM);
		return;
	}
	ctx->num_pages = 1;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->seq = seq;

	page_num = bs_blobid_to_page(blob->id);
	lba = bs_md_page_to_lba(blob->bs, page_num);

	blob->state = SPDK_BLOB_STATE_LOADING;

	bs_sequence_read_dev(seq, &ctx->pages[0], lba,
			     bs_byte_to_lba(bs, bs->md_page_size),
			     blob_load_cpl, ctx);
}

struct spdk_blob_persist_ctx {
	struct spdk_blob		*blob;

	struct spdk_blob_md_page	*pages;
	uint32_t			next_extent_page;
	struct spdk_blob_md_page	*extent_page;

	spdk_bs_sequence_t		*seq;
	spdk_bs_sequence_cpl		cb_fn;
	void				*cb_arg;
	TAILQ_ENTRY(spdk_blob_persist_ctx) link;
};

static void
bs_batch_clear_dev(struct spdk_blob *blob, spdk_bs_batch_t *batch, uint64_t lba,
		   uint64_t lba_count)
{
	switch (blob->clear_method) {
	case BLOB_CLEAR_WITH_DEFAULT:
	case BLOB_CLEAR_WITH_UNMAP:
		bs_batch_unmap_dev(batch, lba, lba_count);
		break;
	case BLOB_CLEAR_WITH_WRITE_ZEROES:
		bs_batch_write_zeroes_dev(batch, lba, lba_count);
		break;
	case BLOB_CLEAR_WITH_NONE:
	default:
		break;
	}
}

static int
bs_super_validate(struct spdk_bs_super_block *super, struct spdk_blob_store *bs)
{
	uint32_t	crc;
	static const char zeros[SPDK_BLOBSTORE_TYPE_LENGTH];

	if (super->version > SPDK_BS_VERSION ||
	    super->version < SPDK_BS_INITIAL_VERSION) {
		return -EILSEQ;
	}

	if (memcmp(super->signature, SPDK_BS_SUPER_BLOCK_SIG,
		   sizeof(super->signature)) != 0) {
		return -EILSEQ;
	}

	crc = blob_md_page_calc_crc(super);
	if (crc != super->crc) {
		return -EILSEQ;
	}

	if (memcmp(&bs->bstype, &super->bstype, SPDK_BLOBSTORE_TYPE_LENGTH) == 0) {
		SPDK_DEBUGLOG(blob, "Bstype matched - loading blobstore\n");
	} else if (memcmp(&bs->bstype, zeros, SPDK_BLOBSTORE_TYPE_LENGTH) == 0) {
		SPDK_DEBUGLOG(blob, "Bstype wildcard used - loading blobstore regardless bstype\n");
	} else {
		SPDK_DEBUGLOG(blob, "Unexpected bstype\n");
		SPDK_LOGDUMP(blob, "Expected:", bs->bstype.bstype, SPDK_BLOBSTORE_TYPE_LENGTH);
		SPDK_LOGDUMP(blob, "Found:", super->bstype.bstype, SPDK_BLOBSTORE_TYPE_LENGTH);
		return -ENXIO;
	}

	if (super->size > bs->dev->blockcnt * bs->dev->blocklen) {
		SPDK_NOTICELOG("Size mismatch, dev size: %" PRIu64 ", blobstore size: %" PRIu64 "\n",
			       bs->dev->blockcnt * bs->dev->blocklen, super->size);
		return -EILSEQ;
	}

	return 0;
}

static void bs_mark_dirty(spdk_bs_sequence_t *seq, struct spdk_blob_store *bs,
			  spdk_bs_sequence_cpl cb_fn, void *cb_arg);

static void
blob_persist_complete_cb(void *arg)
{
	struct spdk_blob_persist_ctx *ctx = arg;

	/* Call user callback */
	ctx->cb_fn(ctx->seq, ctx->cb_arg, 0);

	/* Free the memory */
	spdk_free(ctx->pages);
	free(ctx);
}

static void blob_persist_start(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno);

static void
blob_persist_complete(spdk_bs_sequence_t *seq, struct spdk_blob_persist_ctx *ctx, int bserrno)
{
	struct spdk_blob_persist_ctx	*next_persist, *tmp;
	struct spdk_blob		*blob = ctx->blob;

	if (bserrno == 0) {
		blob_mark_clean(blob);
	}

	assert(ctx == TAILQ_FIRST(&blob->persists_to_complete));

	/* Complete all persists that were pending when the current persist started */
	TAILQ_FOREACH_SAFE(next_persist, &blob->persists_to_complete, link, tmp) {
		TAILQ_REMOVE(&blob->persists_to_complete, next_persist, link);
		spdk_thread_send_msg(spdk_get_thread(), blob_persist_complete_cb, next_persist);
	}

	if (TAILQ_EMPTY(&blob->pending_persists)) {
		return;
	}

	/* Queue up all pending persists for completion and start blob persist with first one */
	TAILQ_SWAP(&blob->persists_to_complete, &blob->pending_persists, spdk_blob_persist_ctx, link);
	next_persist = TAILQ_FIRST(&blob->persists_to_complete);

	blob->state = SPDK_BLOB_STATE_DIRTY;
	bs_mark_dirty(seq, blob->bs, blob_persist_start, next_persist);
}

static void
blob_persist_clear_extents_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	size_t				i;

	if (bserrno != 0) {
		blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	spdk_spin_lock(&bs->used_lock);

	/* Release all extent_pages that were truncated */
	for (i = blob->active.num_extent_pages; i < blob->active.extent_pages_array_size; i++) {
		/* Nothing to release if it was not allocated */
		if (blob->active.extent_pages[i] != 0) {
			bs_release_md_page(bs, blob->active.extent_pages[i]);
		}
	}

	spdk_spin_unlock(&bs->used_lock);

	if (blob->active.num_extent_pages == 0) {
		free(blob->active.extent_pages);
		blob->active.extent_pages = NULL;
		blob->active.extent_pages_array_size = 0;
	} else if (blob->active.num_extent_pages != blob->active.extent_pages_array_size) {
#ifndef __clang_analyzer__
		void *tmp;

		/* scan-build really can't figure reallocs, workaround it */
		tmp = realloc(blob->active.extent_pages, sizeof(uint32_t) * blob->active.num_extent_pages);
		assert(tmp != NULL);
		blob->active.extent_pages = tmp;
#endif
		blob->active.extent_pages_array_size = blob->active.num_extent_pages;
	}

	blob_persist_complete(seq, ctx, bserrno);
}

static void
blob_persist_clear_extents(spdk_bs_sequence_t *seq, struct spdk_blob_persist_ctx *ctx)
{
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	size_t				i;
	uint64_t                        lba;
	uint64_t                        lba_count;
	spdk_bs_batch_t                 *batch;

	batch = bs_sequence_to_batch(seq, blob_persist_clear_extents_cpl, ctx);
	lba_count = bs_byte_to_lba(bs, bs->md_page_size);

	/* Clear all extent_pages that were truncated */
	for (i = blob->active.num_extent_pages; i < blob->active.extent_pages_array_size; i++) {
		/* Nothing to clear if it was not allocated */
		if (blob->active.extent_pages[i] != 0) {
			lba = bs_md_page_to_lba(bs, blob->active.extent_pages[i]);
			bs_batch_write_zeroes_dev(batch, lba, lba_count);
		}
	}

	bs_batch_close(batch);
}

static void
blob_persist_clear_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	size_t				i;

	if (bserrno != 0) {
		blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	spdk_spin_lock(&bs->used_lock);
	/* Release all clusters that were truncated */
	for (i = blob->active.num_clusters; i < blob->active.cluster_array_size; i++) {
		uint32_t cluster_num = bs_lba_to_cluster(bs, blob->active.clusters[i]);

		/* Nothing to release if it was not allocated */
		if (blob->active.clusters[i] != 0) {
			bs_release_cluster(bs, cluster_num);
		}
	}
	spdk_spin_unlock(&bs->used_lock);

	if (blob->active.num_clusters == 0) {
		free(blob->active.clusters);
		blob->active.clusters = NULL;
		blob->active.cluster_array_size = 0;
	} else if (blob->active.num_clusters != blob->active.cluster_array_size) {
#ifndef __clang_analyzer__
		void *tmp;

		/* scan-build really can't figure reallocs, workaround it */
		tmp = realloc(blob->active.clusters, sizeof(*blob->active.clusters) * blob->active.num_clusters);
		assert(tmp != NULL);
		blob->active.clusters = tmp;

#endif
		blob->active.cluster_array_size = blob->active.num_clusters;
	}

	/* Move on to clearing extent pages */
	blob_persist_clear_extents(seq, ctx);
}

static int
lba_cmp(const void *a, const void *b)
{
	uint64_t ua = *(const uint64_t *)a;
	uint64_t ub = *(const uint64_t *)b;

	if (ua < ub) {
		return -1;
	}
	if (ua > ub) {
		return 1;
	}
	return 0;
}

static void
blob_persist_clear_clusters(spdk_bs_sequence_t *seq, struct spdk_blob_persist_ctx *ctx)
{
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	spdk_bs_batch_t			*batch;
	size_t				i;
	uint64_t			lba;
	uint64_t			lba_count;

	/* Clusters don't move around in blobs. The list shrinks or grows
	 * at the end, but no changes ever occur in the middle of the list.
	 */

	batch = bs_sequence_to_batch(seq, blob_persist_clear_clusters_cpl, ctx);

	/* Clear all clusters that were truncated */
	lba = 0;
	lba_count = 0;

	if (blob->active.cluster_array_size > blob->active.num_clusters) {
		qsort(&blob->active.clusters[blob->active.num_clusters],
		      blob->active.cluster_array_size - blob->active.num_clusters, sizeof(uint64_t), lba_cmp);
	}
	for (i = blob->active.num_clusters; i < blob->active.cluster_array_size; i++) {
		uint64_t next_lba = blob->active.clusters[i];
		uint64_t next_lba_count = bs_cluster_to_lba(bs, 1);

		if (next_lba > 0 && (lba + lba_count) == next_lba) {
			/* This cluster is contiguous with the previous one. */
			lba_count += next_lba_count;
			continue;
		} else if (next_lba == 0) {
			continue;
		}

		/* This cluster is not contiguous with the previous one. */

		/* If a run of LBAs previously existing, clear them now */
		if (lba_count > 0) {
			bs_batch_clear_dev(ctx->blob, batch, lba, lba_count);
		}

		/* Start building the next batch */
		lba = next_lba;
		if (next_lba > 0) {
			lba_count = next_lba_count;
		} else {
			lba_count = 0;
		}
	}

	/* If we ended with a contiguous set of LBAs, clear them now */
	if (lba_count > 0) {
		bs_batch_clear_dev(ctx->blob, batch, lba, lba_count);
	}

	bs_batch_close(batch);
}

static void
blob_persist_zero_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	size_t				i;

	if (bserrno != 0) {
		blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	spdk_spin_lock(&bs->used_lock);

	/* This loop starts at 1 because the first page is special and handled
	 * below. The pages (except the first) are never written in place,
	 * so any pages in the clean list must be zeroed.
	 */
	for (i = 1; i < blob->clean.num_pages; i++) {
		bs_release_md_page(bs, blob->clean.pages[i]);
	}

	if (blob->active.num_pages == 0) {
		uint32_t page_num;

		page_num = bs_blobid_to_page(blob->id);
		bs_release_md_page(bs, page_num);
	}

	spdk_spin_unlock(&bs->used_lock);

	/* Move on to clearing clusters */
	blob_persist_clear_clusters(seq, ctx);
}

static void
blob_persist_zero_pages(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	uint64_t			lba;
	uint64_t			lba_count;
	spdk_bs_batch_t			*batch;
	size_t				i;

	if (bserrno != 0) {
		blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	batch = bs_sequence_to_batch(seq, blob_persist_zero_pages_cpl, ctx);

	lba_count = bs_byte_to_lba(bs, bs->md_page_size);

	/* This loop starts at 1 because the first page is special and handled
	 * below. The pages (except the first) are never written in place,
	 * so any pages in the clean list must be zeroed.
	 */
	for (i = 1; i < blob->clean.num_pages; i++) {
		lba = bs_md_page_to_lba(bs, blob->clean.pages[i]);

		bs_batch_write_zeroes_dev(batch, lba, lba_count);
	}

	/* The first page will only be zeroed if this is a delete. */
	if (blob->active.num_pages == 0) {
		uint32_t page_num;

		/* The first page in the metadata goes where the blobid indicates */
		page_num = bs_blobid_to_page(blob->id);
		lba = bs_md_page_to_lba(bs, page_num);

		bs_batch_write_zeroes_dev(batch, lba, lba_count);
	}

	bs_batch_close(batch);
}

static void
blob_persist_write_page_root(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	uint64_t			lba;
	uint32_t			lba_count;
	struct spdk_blob_md_page	*page;

	if (bserrno != 0) {
		blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	if (blob->active.num_pages == 0) {
		/* Move on to the next step */
		blob_persist_zero_pages(seq, ctx, 0);
		return;
	}

	lba_count = bs_byte_to_lba(bs, bs->md_page_size);

	page = &ctx->pages[0];
	/* The first page in the metadata goes where the blobid indicates */
	lba = bs_md_page_to_lba(bs, bs_blobid_to_page(blob->id));

	bs_sequence_write_dev(seq, page, lba, lba_count,
			      blob_persist_zero_pages, ctx);
}

static void
blob_persist_write_page_chain(spdk_bs_sequence_t *seq, struct spdk_blob_persist_ctx *ctx)
{
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	uint64_t			lba;
	uint32_t			lba_count;
	struct spdk_blob_md_page	*page;
	spdk_bs_batch_t			*batch;
	size_t				i;

	/* Clusters don't move around in blobs. The list shrinks or grows
	 * at the end, but no changes ever occur in the middle of the list.
	 */

	lba_count = bs_byte_to_lba(bs, sizeof(*page));

	batch = bs_sequence_to_batch(seq, blob_persist_write_page_root, ctx);

	/* This starts at 1. The root page is not written until
	 * all of the others are finished
	 */
	for (i = 1; i < blob->active.num_pages; i++) {
		page = &ctx->pages[i];
		assert(page->sequence_num == i);

		lba = bs_md_page_to_lba(bs, blob->active.pages[i]);

		bs_batch_write_dev(batch, page, lba, lba_count);
	}

	bs_batch_close(batch);
}

static int
blob_resize(struct spdk_blob *blob, uint64_t sz)
{
	uint64_t	i;
	uint64_t	*tmp;
	uint64_t	cluster;
	uint32_t	lfmd; /*  lowest free md page */
	uint64_t	num_clusters;
	uint32_t	*ep_tmp;
	uint64_t	new_num_ep = 0, current_num_ep = 0;
	struct spdk_blob_store *bs;
	int		rc;

	bs = blob->bs;

	blob_verify_md_op(blob);

	if (blob->active.num_clusters == sz) {
		return 0;
	}

	if (blob->active.num_clusters < blob->active.cluster_array_size) {
		/* If this blob was resized to be larger, then smaller, then
		 * larger without syncing, then the cluster array already
		 * contains spare assigned clusters we can use.
		 */
		num_clusters = spdk_min(blob->active.cluster_array_size,
					sz);
	} else {
		num_clusters = blob->active.num_clusters;
	}

	if (blob->use_extent_table) {
		/* Round up since every cluster beyond current Extent Table size,
		 * requires new extent page. */
		new_num_ep = spdk_divide_round_up(sz, SPDK_EXTENTS_PER_EP);
		current_num_ep = spdk_divide_round_up(num_clusters, SPDK_EXTENTS_PER_EP);
	}

	assert(!spdk_spin_held(&bs->used_lock));

	/* Check first that we have enough clusters and md pages before we start claiming them.
	 * bs->used_lock is held to ensure that clusters we think are free are still free when we go
	 * to claim them later in this function.
	 */
	if (sz > num_clusters && spdk_blob_is_thin_provisioned(blob) == false) {
		spdk_spin_lock(&bs->used_lock);
		if ((sz - num_clusters) > bs->num_free_clusters) {
			rc = -ENOSPC;
			goto out;
		}
		lfmd = 0;
		for (i = current_num_ep; i < new_num_ep ; i++) {
			lfmd = spdk_bit_array_find_first_clear(blob->bs->used_md_pages, lfmd);
			if (lfmd == UINT32_MAX) {
				/* No more free md pages. Cannot satisfy the request */
				rc = -ENOSPC;
				goto out;
			}
		}
	}

	if (sz > num_clusters) {
		/* Expand the cluster array if necessary.
		 * We only shrink the array when persisting.
		 */
		tmp = realloc(blob->active.clusters, sizeof(*blob->active.clusters) * sz);
		if (sz > 0 && tmp == NULL) {
			rc = -ENOMEM;
			goto out;
		}
		memset(tmp + blob->active.cluster_array_size, 0,
		       sizeof(*blob->active.clusters) * (sz - blob->active.cluster_array_size));
		blob->active.clusters = tmp;
		blob->active.cluster_array_size = sz;

		/* Expand the extents table, only if enough clusters were added */
		if (new_num_ep > current_num_ep && blob->use_extent_table) {
			ep_tmp = realloc(blob->active.extent_pages, sizeof(*blob->active.extent_pages) * new_num_ep);
			if (new_num_ep > 0 && ep_tmp == NULL) {
				rc = -ENOMEM;
				goto out;
			}
			memset(ep_tmp + blob->active.extent_pages_array_size, 0,
			       sizeof(*blob->active.extent_pages) * (new_num_ep - blob->active.extent_pages_array_size));
			blob->active.extent_pages = ep_tmp;
			blob->active.extent_pages_array_size = new_num_ep;
		}
	}

	blob->state = SPDK_BLOB_STATE_DIRTY;

	if (spdk_blob_is_thin_provisioned(blob) == false) {
		cluster = 0;
		lfmd = 0;
		for (i = num_clusters; i < sz; i++) {
			bs_allocate_cluster(blob, i, &cluster, &lfmd, true);
			/* Do not increment lfmd here.  lfmd will get updated
			 * to the md_page allocated (if any) when a new extent
			 * page is needed.  Just pass that value again,
			 * bs_allocate_cluster will just start at that index
			 * to find the next free md_page when needed.
			 */
		}
	}

	/* If we are shrinking the blob, we must adjust num_allocated_clusters */
	for (i = sz; i < num_clusters; i++) {
		if (blob->active.clusters[i] != 0) {
			blob->active.num_allocated_clusters--;
		}
	}

	blob->active.num_clusters = sz;
	blob->active.num_extent_pages = new_num_ep;

	rc = 0;
out:
	if (spdk_spin_held(&bs->used_lock)) {
		spdk_spin_unlock(&bs->used_lock);
	}

	return rc;
}

static void
blob_persist_generate_new_md(struct spdk_blob_persist_ctx *ctx)
{
	spdk_bs_sequence_t *seq = ctx->seq;
	struct spdk_blob *blob = ctx->blob;
	struct spdk_blob_store *bs = blob->bs;
	uint64_t i;
	uint32_t page_num;
	void *tmp;
	int rc;

	/* Generate the new metadata */
	rc = blob_serialize(blob, &ctx->pages, &blob->active.num_pages);
	if (rc < 0) {
		blob_persist_complete(seq, ctx, rc);
		return;
	}

	assert(blob->active.num_pages >= 1);

	/* Resize the cache of page indices */
	tmp = realloc(blob->active.pages, blob->active.num_pages * sizeof(*blob->active.pages));
	if (!tmp) {
		blob_persist_complete(seq, ctx, -ENOMEM);
		return;
	}
	blob->active.pages = tmp;

	/* Assign this metadata to pages. This requires two passes - one to verify that there are
	 * enough pages and a second to actually claim them. The used_lock is held across
	 * both passes to ensure things don't change in the middle.
	 */
	spdk_spin_lock(&bs->used_lock);
	page_num = 0;
	/* Note that this loop starts at one. The first page location is fixed by the blobid. */
	for (i = 1; i < blob->active.num_pages; i++) {
		page_num = spdk_bit_array_find_first_clear(bs->used_md_pages, page_num);
		if (page_num == UINT32_MAX) {
			spdk_spin_unlock(&bs->used_lock);
			blob_persist_complete(seq, ctx, -ENOMEM);
			return;
		}
		page_num++;
	}

	page_num = 0;
	blob->active.pages[0] = bs_blobid_to_page(blob->id);
	for (i = 1; i < blob->active.num_pages; i++) {
		page_num = spdk_bit_array_find_first_clear(bs->used_md_pages, page_num);
		ctx->pages[i - 1].next = page_num;
		/* Now that previous metadata page is complete, calculate the crc for it. */
		ctx->pages[i - 1].crc = blob_md_page_calc_crc(&ctx->pages[i - 1]);
		blob->active.pages[i] = page_num;
		bs_claim_md_page(bs, page_num);
		SPDK_DEBUGLOG(blob, "Claiming page %u for blob 0x%" PRIx64 "\n", page_num,
			      blob->id);
		page_num++;
	}
	spdk_spin_unlock(&bs->used_lock);
	ctx->pages[i - 1].crc = blob_md_page_calc_crc(&ctx->pages[i - 1]);
	/* Start writing the metadata from last page to first */
	blob->state = SPDK_BLOB_STATE_CLEAN;
	blob_persist_write_page_chain(seq, ctx);
}

static void
blob_persist_write_extent_pages(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	size_t				i;
	uint32_t			extent_page_id;
	uint32_t                        page_count = 0;
	int				rc;

	if (ctx->extent_page != NULL) {
		spdk_free(ctx->extent_page);
		ctx->extent_page = NULL;
	}

	if (bserrno != 0) {
		blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	/* Only write out Extent Pages when blob was resized. */
	for (i = ctx->next_extent_page; i < blob->active.extent_pages_array_size; i++) {
		extent_page_id = blob->active.extent_pages[i];
		if (extent_page_id == 0) {
			/* No Extent Page to persist */
			assert(spdk_blob_is_thin_provisioned(blob));
			continue;
		}
		assert(spdk_bit_array_get(blob->bs->used_md_pages, extent_page_id));
		ctx->next_extent_page = i + 1;
		rc = blob_serialize_add_page(ctx->blob, &ctx->extent_page, &page_count, &ctx->extent_page);
		if (rc < 0) {
			blob_persist_complete(seq, ctx, rc);
			return;
		}

		blob->state = SPDK_BLOB_STATE_DIRTY;
		blob_serialize_extent_page(blob, i * SPDK_EXTENTS_PER_EP, ctx->extent_page);

		ctx->extent_page->crc = blob_md_page_calc_crc(ctx->extent_page);

		bs_sequence_write_dev(seq, ctx->extent_page, bs_md_page_to_lba(blob->bs, extent_page_id),
				      bs_byte_to_lba(blob->bs, blob->bs->md_page_size),
				      blob_persist_write_extent_pages, ctx);
		return;
	}

	blob_persist_generate_new_md(ctx);
}

static void
blob_persist_start(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx *ctx = cb_arg;
	struct spdk_blob *blob = ctx->blob;

	if (bserrno != 0) {
		blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	if (blob->active.num_pages == 0) {
		/* This is the signal that the blob should be deleted.
		 * Immediately jump to the clean up routine. */
		assert(blob->clean.num_pages > 0);
		blob->state = SPDK_BLOB_STATE_CLEAN;
		blob_persist_zero_pages(seq, ctx, 0);
		return;

	}

	if (blob->clean.num_clusters < blob->active.num_clusters) {
		/* Blob was resized up */
		assert(blob->clean.num_extent_pages <= blob->active.num_extent_pages);
		ctx->next_extent_page = spdk_max(1, blob->clean.num_extent_pages) - 1;
	} else if (blob->active.num_clusters < blob->active.cluster_array_size) {
		/* Blob was resized down */
		assert(blob->clean.num_extent_pages >= blob->active.num_extent_pages);
		ctx->next_extent_page = spdk_max(1, blob->active.num_extent_pages) - 1;
	} else {
		/* No change in size occurred */
		blob_persist_generate_new_md(ctx);
		return;
	}

	blob_persist_write_extent_pages(seq, ctx, 0);
}

struct spdk_bs_mark_dirty {
	struct spdk_blob_store		*bs;
	struct spdk_bs_super_block	*super;
	spdk_bs_sequence_cpl		cb_fn;
	void				*cb_arg;
};

static void
bs_mark_dirty_write_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_mark_dirty *ctx = cb_arg;

	if (bserrno == 0) {
		ctx->bs->clean = 0;
	}

	ctx->cb_fn(seq, ctx->cb_arg, bserrno);

	spdk_free(ctx->super);
	free(ctx);
}

static void bs_write_super(spdk_bs_sequence_t *seq, struct spdk_blob_store *bs,
			   struct spdk_bs_super_block *super, spdk_bs_sequence_cpl cb_fn, void *cb_arg);


static void
bs_mark_dirty_write(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_mark_dirty *ctx = cb_arg;
	int rc;

	if (bserrno != 0) {
		bs_mark_dirty_write_cpl(seq, ctx, bserrno);
		return;
	}

	rc = bs_super_validate(ctx->super, ctx->bs);
	if (rc != 0) {
		bs_mark_dirty_write_cpl(seq, ctx, rc);
		return;
	}

	ctx->super->clean = 0;
	if (ctx->super->size == 0) {
		ctx->super->size = ctx->bs->dev->blockcnt * ctx->bs->dev->blocklen;
	}

	bs_write_super(seq, ctx->bs, ctx->super, bs_mark_dirty_write_cpl, ctx);
}

static void
bs_mark_dirty(spdk_bs_sequence_t *seq, struct spdk_blob_store *bs,
	      spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_mark_dirty *ctx;

	/* Blobstore is already marked dirty */
	if (bs->clean == 0) {
		cb_fn(seq, cb_arg, 0);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(seq, cb_arg, -ENOMEM);
		return;
	}
	ctx->bs = bs;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	ctx->super = spdk_zmalloc(sizeof(*ctx->super), 0x1000, NULL,
				  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->super) {
		free(ctx);
		cb_fn(seq, cb_arg, -ENOMEM);
		return;
	}

	bs_sequence_read_dev(seq, ctx->super, bs_page_to_lba(bs, 0),
			     bs_byte_to_lba(bs, sizeof(*ctx->super)),
			     bs_mark_dirty_write, ctx);
}

/* Write a blob to disk */
static void
blob_persist(spdk_bs_sequence_t *seq, struct spdk_blob *blob,
	     spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_blob_persist_ctx *ctx;

	blob_verify_md_op(blob);

	if (blob->state == SPDK_BLOB_STATE_CLEAN && TAILQ_EMPTY(&blob->persists_to_complete)) {
		cb_fn(seq, cb_arg, 0);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(seq, cb_arg, -ENOMEM);
		return;
	}
	ctx->blob = blob;
	ctx->seq = seq;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	/* Multiple blob persists can affect one another, via blob->state or
	 * blob mutable data changes. To prevent it, queue up the persists. */
	if (!TAILQ_EMPTY(&blob->persists_to_complete)) {
		TAILQ_INSERT_TAIL(&blob->pending_persists, ctx, link);
		return;
	}
	TAILQ_INSERT_HEAD(&blob->persists_to_complete, ctx, link);

	bs_mark_dirty(seq, blob->bs, blob_persist_start, ctx);
}

struct spdk_blob_copy_cluster_ctx {
	struct spdk_blob *blob;
	uint8_t *buf;
	uint64_t io_unit;
	uint64_t new_cluster;
	uint32_t new_extent_page;
	spdk_bs_sequence_t *seq;
	struct spdk_blob_md_page *new_cluster_page;
};

struct spdk_blob_free_cluster_ctx {
	struct spdk_blob *blob;
	uint64_t page;
	struct spdk_blob_md_page *md_page;
	uint64_t cluster_num;
	uint32_t extent_page;
	spdk_bs_sequence_t *seq;
};

static void
blob_allocate_and_copy_cluster_cpl(void *cb_arg, int bserrno)
{
	struct spdk_blob_copy_cluster_ctx *ctx = cb_arg;
	struct spdk_bs_request_set *set = (struct spdk_bs_request_set *)ctx->seq;
	TAILQ_HEAD(, spdk_bs_request_set) requests;
	spdk_bs_user_op_t *op;

	TAILQ_INIT(&requests);
	TAILQ_SWAP(&set->channel->need_cluster_alloc, &requests, spdk_bs_request_set, link);

	while (!TAILQ_EMPTY(&requests)) {
		op = TAILQ_FIRST(&requests);
		TAILQ_REMOVE(&requests, op, link);
		if (bserrno == 0) {
			bs_user_op_execute(op);
		} else {
			bs_user_op_abort(op, bserrno);
		}
	}

	spdk_free(ctx->buf);
	free(ctx);
}

static void
blob_free_cluster_cpl(void *cb_arg, int bserrno)
{
	struct spdk_blob_free_cluster_ctx *ctx = cb_arg;
	spdk_bs_sequence_t *seq = ctx->seq;

	bs_sequence_finish(seq, bserrno);

	free(ctx);
}

static void
blob_insert_cluster_revert(struct spdk_blob_copy_cluster_ctx *ctx)
{
	spdk_spin_lock(&ctx->blob->bs->used_lock);
	bs_release_cluster(ctx->blob->bs, ctx->new_cluster);
	if (ctx->new_extent_page != 0) {
		bs_release_md_page(ctx->blob->bs, ctx->new_extent_page);
	}
	spdk_spin_unlock(&ctx->blob->bs->used_lock);
}

static void
blob_insert_cluster_clear_cpl(void *cb_arg, int bserrno)
{
	struct spdk_blob_copy_cluster_ctx *ctx = cb_arg;

	if (bserrno) {
		SPDK_WARNLOG("Failed to clear cluster: %d\n", bserrno);
	}

	blob_insert_cluster_revert(ctx);
	bs_sequence_finish(ctx->seq, bserrno);
}

static void
blob_insert_cluster_clear(struct spdk_blob_copy_cluster_ctx *ctx)
{
	struct spdk_bs_cpl cpl;
	spdk_bs_batch_t *batch;
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(ctx->seq->channel);

	/*
	 * We allocated a cluster and we copied data to it. But now, we realized that we don't need
	 * this cluster and we want to release it. We must ensure that we clear the data on this
	 * cluster.
	 * The cluster may later be re-allocated by a thick-provisioned blob for example. When
	 * reading from this thick-provisioned blob before writing data, we should read zeroes.
	 */

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = blob_insert_cluster_clear_cpl;
	cpl.u.blob_basic.cb_arg = ctx;

	batch = bs_batch_open(ch, &cpl, ctx->blob);
	if (!batch) {
		blob_insert_cluster_clear_cpl(ctx, -ENOMEM);
		return;
	}

	bs_batch_clear_dev(ctx->blob, batch, bs_cluster_to_lba(ctx->blob->bs, ctx->new_cluster),
			   bs_cluster_to_lba(ctx->blob->bs, 1));
	bs_batch_close(batch);
}

static void
blob_insert_cluster_cpl(void *cb_arg, int bserrno)
{
	struct spdk_blob_copy_cluster_ctx *ctx = cb_arg;

	if (bserrno) {
		if (bserrno == -EEXIST) {
			/* The metadata insert failed because another thread
			 * allocated the cluster first. Clear and free our cluster
			 * but continue without error. */
			blob_insert_cluster_clear(ctx);
			return;
		}

		blob_insert_cluster_revert(ctx);
	}

	bs_sequence_finish(ctx->seq, bserrno);
}

static void
blob_write_copy_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_copy_cluster_ctx *ctx = cb_arg;
	uint32_t cluster_number;

	if (bserrno) {
		/* The write failed, so jump to the final completion handler */
		bs_sequence_finish(seq, bserrno);
		return;
	}

	cluster_number = bs_io_unit_to_cluster(ctx->blob->bs, ctx->io_unit);

	blob_insert_cluster_on_md_thread(ctx->blob, cluster_number, ctx->new_cluster,
					 ctx->new_extent_page, ctx->new_cluster_page, blob_insert_cluster_cpl, ctx);
}

static void
blob_write_copy(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_copy_cluster_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		/* The read failed, so jump to the final completion handler */
		bs_sequence_finish(seq, bserrno);
		return;
	}

	/* Write whole cluster */
	bs_sequence_write_dev(seq, ctx->buf,
			      bs_cluster_to_lba(ctx->blob->bs, ctx->new_cluster),
			      bs_cluster_to_lba(ctx->blob->bs, 1),
			      blob_write_copy_cpl, ctx);
}

static bool
blob_can_copy(struct spdk_blob *blob, uint64_t cluster_start_io_unit, uint64_t *base_lba)
{
	uint64_t lba = bs_dev_io_unit_to_lba(blob, blob->back_bs_dev, cluster_start_io_unit);

	return (!blob_is_esnap_clone(blob) && blob->bs->dev->copy != NULL) &&
	       blob->back_bs_dev->translate_lba(blob->back_bs_dev, lba, base_lba);
}

static void
blob_copy(struct spdk_blob_copy_cluster_ctx *ctx, spdk_bs_user_op_t *op, uint64_t src_lba)
{
	struct spdk_blob *blob = ctx->blob;
	uint64_t lba_count = bs_dev_byte_to_lba(blob->back_bs_dev, blob->bs->cluster_sz);

	bs_sequence_copy_dev(ctx->seq,
			     bs_cluster_to_lba(blob->bs, ctx->new_cluster),
			     src_lba,
			     lba_count,
			     blob_write_copy_cpl, ctx);
}

static void
bs_allocate_and_copy_cluster(struct spdk_blob *blob,
			     struct spdk_io_channel *_ch,
			     uint64_t io_unit, spdk_bs_user_op_t *op)
{
	struct spdk_bs_cpl cpl;
	struct spdk_bs_channel *ch;
	struct spdk_blob_copy_cluster_ctx *ctx;
	uint64_t cluster_start_io_unit;
	uint32_t cluster_number;
	bool is_zeroes;
	bool can_copy;
	bool is_valid_range;
	uint64_t copy_src_lba;
	int rc;

	ch = spdk_io_channel_get_ctx(_ch);

	if (!TAILQ_EMPTY(&ch->need_cluster_alloc)) {
		/* There are already operations pending. Queue this user op
		 * and return because it will be re-executed when the outstanding
		 * cluster allocation completes. */
		TAILQ_INSERT_TAIL(&ch->need_cluster_alloc, op, link);
		return;
	}

	/* Round the io_unit offset down to the first io_unit in the cluster */
	cluster_start_io_unit = bs_io_unit_to_cluster_start(blob, io_unit);

	/* Calculate which index in the metadata cluster array the corresponding
	 * cluster is supposed to be at. */
	cluster_number = bs_io_unit_to_cluster_number(blob, io_unit);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		bs_user_op_abort(op, -ENOMEM);
		return;
	}

	assert(blob->bs->cluster_sz % blob->back_bs_dev->blocklen == 0);

	ctx->blob = blob;
	ctx->io_unit = cluster_start_io_unit;
	ctx->new_cluster_page = ch->new_cluster_page;

	/* Check if the cluster that we intend to do CoW for is valid for
	 * the backing dev. For zeroes backing dev, it'll be always valid.
	 * For other backing dev e.g. a snapshot, it could be invalid if
	 * the blob has been resized after snapshot was taken. */
	is_valid_range = blob->back_bs_dev->is_range_valid(blob->back_bs_dev,
			 bs_dev_io_unit_to_lba(blob, blob->back_bs_dev, cluster_start_io_unit),
			 bs_dev_byte_to_lba(blob->back_bs_dev, blob->bs->cluster_sz));

	can_copy = is_valid_range && blob_can_copy(blob, cluster_start_io_unit, &copy_src_lba);

	is_zeroes = is_valid_range && blob->back_bs_dev->is_zeroes(blob->back_bs_dev,
			bs_dev_io_unit_to_lba(blob, blob->back_bs_dev, cluster_start_io_unit),
			bs_dev_byte_to_lba(blob->back_bs_dev, blob->bs->cluster_sz));
	if (blob->parent_id != SPDK_BLOBID_INVALID && !is_zeroes && !can_copy) {
		ctx->buf = spdk_malloc(blob->bs->cluster_sz, blob->back_bs_dev->blocklen,
				       NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (!ctx->buf) {
			SPDK_ERRLOG("DMA allocation for cluster of size = %" PRIu32 " failed.\n",
				    blob->bs->cluster_sz);
			free(ctx);
			bs_user_op_abort(op, -ENOMEM);
			return;
		}
	}

	spdk_spin_lock(&blob->bs->used_lock);
	rc = bs_allocate_cluster(blob, cluster_number, &ctx->new_cluster, &ctx->new_extent_page,
				 false);
	spdk_spin_unlock(&blob->bs->used_lock);
	if (rc != 0) {
		spdk_free(ctx->buf);
		free(ctx);
		bs_user_op_abort(op, rc);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = blob_allocate_and_copy_cluster_cpl;
	cpl.u.blob_basic.cb_arg = ctx;

	ctx->seq = bs_sequence_start_blob(_ch, &cpl, blob);
	if (!ctx->seq) {
		spdk_spin_lock(&blob->bs->used_lock);
		bs_release_cluster(blob->bs, ctx->new_cluster);
		spdk_spin_unlock(&blob->bs->used_lock);
		spdk_free(ctx->buf);
		free(ctx);
		bs_user_op_abort(op, -ENOMEM);
		return;
	}

	/* Queue the user op to block other incoming operations */
	TAILQ_INSERT_TAIL(&ch->need_cluster_alloc, op, link);

	if (blob->parent_id != SPDK_BLOBID_INVALID && !is_zeroes) {
		if (can_copy) {
			blob_copy(ctx, op, copy_src_lba);
		} else {
			/* Read cluster from backing device */
			bs_sequence_read_bs_dev(ctx->seq, blob->back_bs_dev, ctx->buf,
						bs_dev_io_unit_to_lba(blob, blob->back_bs_dev, cluster_start_io_unit),
						bs_dev_byte_to_lba(blob->back_bs_dev, blob->bs->cluster_sz),
						blob_write_copy, ctx);
		}

	} else {
		blob_insert_cluster_on_md_thread(ctx->blob, cluster_number, ctx->new_cluster,
						 ctx->new_extent_page, ctx->new_cluster_page, blob_insert_cluster_cpl, ctx);
	}
}

static inline bool
blob_calculate_lba_and_lba_count(struct spdk_blob *blob, uint64_t io_unit, uint64_t length,
				 uint64_t *lba,	uint64_t *lba_count)
{
	*lba_count = length;

	if (!bs_io_unit_is_allocated(blob, io_unit)) {
		assert(blob->back_bs_dev != NULL);
		*lba = bs_io_unit_to_back_dev_lba(blob, io_unit);
		*lba_count = bs_io_unit_to_back_dev_lba(blob, *lba_count);
		return false;
	} else {
		*lba = bs_blob_io_unit_to_lba(blob, io_unit);
		return true;
	}
}

struct op_split_ctx {
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	uint64_t io_unit_offset;
	uint64_t io_units_remaining;
	void *curr_payload;
	enum spdk_blob_op_type op_type;
	spdk_bs_sequence_t *seq;
	bool in_submit_ctx;
	bool completed_in_submit_ctx;
	bool done;
};

static void
blob_request_submit_op_split_next(void *cb_arg, int bserrno)
{
	struct op_split_ctx	*ctx = cb_arg;
	struct spdk_blob	*blob = ctx->blob;
	struct spdk_io_channel	*ch = ctx->channel;
	enum spdk_blob_op_type	op_type = ctx->op_type;
	uint8_t			*buf;
	uint64_t		offset;
	uint64_t		length;
	uint64_t		op_length;

	if (bserrno != 0 || ctx->io_units_remaining == 0) {
		bs_sequence_finish(ctx->seq, bserrno);
		if (ctx->in_submit_ctx) {
			/* Defer freeing of the ctx object, since it will be
			 * accessed when this unwinds back to the submission
			 * context.
			 */
			ctx->done = true;
		} else {
			free(ctx);
		}
		return;
	}

	if (ctx->in_submit_ctx) {
		/* If this split operation completed in the context
		 * of its submission, mark the flag and return immediately
		 * to avoid recursion.
		 */
		ctx->completed_in_submit_ctx = true;
		return;
	}

	while (true) {
		ctx->completed_in_submit_ctx = false;

		offset = ctx->io_unit_offset;
		length = ctx->io_units_remaining;
		buf = ctx->curr_payload;
		op_length = spdk_min(length, bs_num_io_units_to_cluster_boundary(blob,
				     offset));

		/* Update length and payload for next operation */
		ctx->io_units_remaining -= op_length;
		ctx->io_unit_offset += op_length;
		if (op_type == SPDK_BLOB_WRITE || op_type == SPDK_BLOB_READ) {
			ctx->curr_payload += op_length * blob->bs->io_unit_size;
		}

		assert(!ctx->in_submit_ctx);
		ctx->in_submit_ctx = true;

		switch (op_type) {
		case SPDK_BLOB_READ:
			spdk_blob_io_read(blob, ch, buf, offset, op_length,
					  blob_request_submit_op_split_next, ctx);
			break;
		case SPDK_BLOB_WRITE:
			spdk_blob_io_write(blob, ch, buf, offset, op_length,
					   blob_request_submit_op_split_next, ctx);
			break;
		case SPDK_BLOB_UNMAP:
			spdk_blob_io_unmap(blob, ch, offset, op_length,
					   blob_request_submit_op_split_next, ctx);
			break;
		case SPDK_BLOB_WRITE_ZEROES:
			spdk_blob_io_write_zeroes(blob, ch, offset, op_length,
						  blob_request_submit_op_split_next, ctx);
			break;
		case SPDK_BLOB_READV:
		case SPDK_BLOB_WRITEV:
			SPDK_ERRLOG("readv/write not valid\n");
			bs_sequence_finish(ctx->seq, -EINVAL);
			free(ctx);
			return;
		}

#ifndef __clang_analyzer__
		/* scan-build reports a false positive around accessing the ctx here. It
		 * forms a path that recursively calls this function, but then says
		 * "assuming ctx->in_submit_ctx is false", when that isn't possible.
		 * This path does free(ctx), returns to here, and reports a use-after-free
		 * bug.  Wrapping this bit of code so that scan-build doesn't see it
		 * works around the scan-build bug.
		 */
		assert(ctx->in_submit_ctx);
		ctx->in_submit_ctx = false;

		/* If the operation completed immediately, loop back and submit the
		 * next operation.  Otherwise we can return and the next split
		 * operation will get submitted when this current operation is
		 * later completed asynchronously.
		 */
		if (ctx->completed_in_submit_ctx) {
			continue;
		} else if (ctx->done) {
			free(ctx);
		}
#endif
		break;
	}
}

static void
blob_request_submit_op_split(struct spdk_io_channel *ch, struct spdk_blob *blob,
			     void *payload, uint64_t offset, uint64_t length,
			     spdk_blob_op_complete cb_fn, void *cb_arg, enum spdk_blob_op_type op_type)
{
	struct op_split_ctx *ctx;
	spdk_bs_sequence_t *seq;
	struct spdk_bs_cpl cpl;

	assert(blob != NULL);

	ctx = calloc(1, sizeof(struct op_split_ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = bs_sequence_start_blob(ch, &cpl, blob);
	if (!seq) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->blob = blob;
	ctx->channel = ch;
	ctx->curr_payload = payload;
	ctx->io_unit_offset = offset;
	ctx->io_units_remaining = length;
	ctx->op_type = op_type;
	ctx->seq = seq;

	blob_request_submit_op_split_next(ctx, 0);
}

/*
 * [한국어]
 * spdk_free_cluster_unmap_complete - cluster unmap IO 완료 콜백 (static)
 *
 * unmap이 성공해서 backing dev에 cluster 영역이 invalid화된 다음, md_thread로
 * 메시지를 보내 메타에서도 그 cluster를 free 시킨다 (used_clusters 비트 OFF).
 * 비트맵 갱신을 IO 스레드가 직접 하지 않는 이유: 메타 자료구조는 md_thread 단독 소유.
 *
 * (이름은 공개 prefix이지만 static — 과거 호환성 잔재로 보임.)
 */
static void
spdk_free_cluster_unmap_complete(void *cb_arg, int bserrno)
{
	struct spdk_blob_free_cluster_ctx *ctx = cb_arg;

	if (bserrno) {
		bs_sequence_finish(ctx->seq, bserrno);
		free(ctx);
		return;
	}

	blob_free_cluster_on_md_thread(ctx->blob, ctx->cluster_num,
				       ctx->extent_page, ctx->md_page, blob_free_cluster_cpl, ctx);
}

static void
blob_request_submit_op_single(struct spdk_io_channel *_ch, struct spdk_blob *blob,
			      void *payload, uint64_t offset, uint64_t length,
			      spdk_blob_op_complete cb_fn, void *cb_arg, enum spdk_blob_op_type op_type)
{
	struct spdk_bs_cpl cpl;
	uint64_t lba;
	uint64_t lba_count;
	bool is_allocated;

	assert(blob != NULL);

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	if (blob->frozen_refcnt) {
		/* This blob I/O is frozen */
		spdk_bs_user_op_t *op;
		struct spdk_bs_channel *bs_channel = spdk_io_channel_get_ctx(_ch);

		op = bs_user_op_alloc(_ch, &cpl, op_type, blob, payload, 0, offset, length);
		if (!op) {
			cb_fn(cb_arg, -ENOMEM);
			return;
		}

		TAILQ_INSERT_TAIL(&bs_channel->queued_io, op, link);

		return;
	}

	is_allocated = blob_calculate_lba_and_lba_count(blob, offset, length, &lba, &lba_count);

	switch (op_type) {
	case SPDK_BLOB_READ: {
		spdk_bs_batch_t *batch;

		batch = bs_batch_open(_ch, &cpl, blob);
		if (!batch) {
			cb_fn(cb_arg, -ENOMEM);
			return;
		}

		if (is_allocated) {
			/* Read from the blob */
			bs_batch_read_dev(batch, payload, lba, lba_count);
		} else {
			/* Read from the backing block device */
			bs_batch_read_bs_dev(batch, blob->back_bs_dev, payload, lba, lba_count);
		}

		bs_batch_close(batch);
		break;
	}
	case SPDK_BLOB_WRITE:
	case SPDK_BLOB_WRITE_ZEROES: {
		if (is_allocated) {
			/* Write to the blob */
			spdk_bs_batch_t *batch;

			if (lba_count == 0) {
				cb_fn(cb_arg, 0);
				return;
			}

			batch = bs_batch_open(_ch, &cpl, blob);
			if (!batch) {
				cb_fn(cb_arg, -ENOMEM);
				return;
			}

			if (op_type == SPDK_BLOB_WRITE) {
				bs_batch_write_dev(batch, payload, lba, lba_count);
			} else {
				bs_batch_write_zeroes_dev(batch, lba, lba_count);
			}

			bs_batch_close(batch);
		} else {
			/* Queue this operation and allocate the cluster */
			spdk_bs_user_op_t *op;

			op = bs_user_op_alloc(_ch, &cpl, op_type, blob, payload, 0, offset, length);
			if (!op) {
				cb_fn(cb_arg, -ENOMEM);
				return;
			}

			bs_allocate_and_copy_cluster(blob, _ch, offset, op);
		}
		break;
	}
	case SPDK_BLOB_UNMAP: {
		struct spdk_blob_free_cluster_ctx *ctx = NULL;
		spdk_bs_batch_t *batch;

		/* if aligned with cluster release cluster,
		 * but if blob locked_operation_in_progress has been set
		 * because of some specical cases, such as doing inflate,
		 * we should skip the operation of release cluster.
		 */
		if (spdk_blob_is_thin_provisioned(blob) && is_allocated &&
		    (!blob->locked_operation_in_progress) &&
		    blob_backed_with_zeroes_dev(blob) &&
		    bs_io_units_per_cluster(blob) == length) {
			struct spdk_bs_channel *bs_channel = spdk_io_channel_get_ctx(_ch);
			uint64_t cluster_start_page;
			uint32_t cluster_number;

			assert(offset % bs_io_units_per_cluster(blob) == 0);

			/* Round the io_unit offset down to the first page in the cluster */
			cluster_start_page = bs_io_unit_to_cluster_start(blob, offset);

			/* Calculate which index in the metadata cluster array the corresponding
			 * cluster is supposed to be at. */
			cluster_number = bs_io_unit_to_cluster_number(blob, offset);

			ctx = calloc(1, sizeof(*ctx));
			if (!ctx) {
				cb_fn(cb_arg, -ENOMEM);
				return;
			}
			/* When freeing a cluster the flow should be (in order):
			 * 1. Unmap the underlying area (so if the cluster is reclaimed in the future, it won't leak
			 * old data)
			 * 2. Once the unmap completes (to avoid any races with incoming writes that may claim the
			 * cluster), update and sync metadata freeing the cluster
			 * 3. Once metadata update is done, complete the user unmap request
			 */
			ctx->blob = blob;
			ctx->page = cluster_start_page;
			ctx->cluster_num = cluster_number;
			ctx->md_page = bs_channel->release_cluster_page;
			ctx->seq = bs_sequence_start_bs(_ch, &cpl);
			if (!ctx->seq) {
				free(ctx);
				cb_fn(cb_arg, -ENOMEM);
				return;
			}

			if (blob->use_extent_table) {
				ctx->extent_page = *bs_cluster_to_extent_page(blob, cluster_number);
			}

			cpl.u.blob_basic.cb_fn = spdk_free_cluster_unmap_complete;
			cpl.u.blob_basic.cb_arg = ctx;
		}

		batch = bs_batch_open(_ch, &cpl, blob);
		if (!batch) {
			if (ctx != NULL) {
				assert(ctx->seq != NULL);
				/* Finish the sequence allocated for metadata update */
				bs_sequence_finish(ctx->seq, -ENOMEM);
			} else {
				cb_fn(cb_arg, -ENOMEM);
			}
			free(ctx);
			return;
		}

		if (is_allocated) {
			bs_batch_unmap_dev(batch, lba, lba_count);
		}

		bs_batch_close(batch);
		break;
	}
	case SPDK_BLOB_READV:
	case SPDK_BLOB_WRITEV:
		SPDK_ERRLOG("readv/write not valid\n");
		cb_fn(cb_arg, -EINVAL);
		break;
	}
}

static void
blob_request_submit_op(struct spdk_blob *blob, struct spdk_io_channel *_channel,
		       void *payload, uint64_t offset, uint64_t length,
		       spdk_blob_op_complete cb_fn, void *cb_arg, enum spdk_blob_op_type op_type)
{
	assert(blob != NULL);

	if (blob->data_ro && op_type != SPDK_BLOB_READ) {
		cb_fn(cb_arg, -EPERM);
		return;
	}

	if (length == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	if (offset + length > bs_cluster_to_lba(blob->bs, blob->active.num_clusters)) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}
	if (length <= bs_num_io_units_to_cluster_boundary(blob, offset)) {
		blob_request_submit_op_single(_channel, blob, payload, offset, length,
					      cb_fn, cb_arg, op_type);
	} else {
		blob_request_submit_op_split(_channel, blob, payload, offset, length,
					     cb_fn, cb_arg, op_type);
	}
}

struct rw_iov_ctx {
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	spdk_blob_op_complete cb_fn;
	void *cb_arg;
	bool read;
	int iovcnt;
	struct iovec *orig_iov;
	uint64_t io_unit_offset;
	uint64_t io_units_remaining;
	uint64_t io_units_done;
	struct spdk_blob_ext_io_opts *ext_io_opts;
	struct iovec iov[0];
};

static void
rw_iov_done(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	assert(cb_arg == NULL);
	bs_sequence_finish(seq, bserrno);
}

static void
rw_iov_split_next(void *cb_arg, int bserrno)
{
	struct rw_iov_ctx *ctx = cb_arg;
	struct spdk_blob *blob = ctx->blob;
	struct iovec *iov, *orig_iov;
	int iovcnt;
	size_t orig_iovoff;
	uint64_t io_units_count, io_units_to_boundary, io_unit_offset;
	uint64_t byte_count;

	if (bserrno != 0 || ctx->io_units_remaining == 0) {
		ctx->cb_fn(ctx->cb_arg, bserrno);
		free(ctx);
		return;
	}

	io_unit_offset = ctx->io_unit_offset;
	io_units_to_boundary = bs_num_io_units_to_cluster_boundary(blob, io_unit_offset);
	io_units_count = spdk_min(ctx->io_units_remaining, io_units_to_boundary);
	/*
	 * Get index and offset into the original iov array for our current position in the I/O sequence.
	 *  byte_count will keep track of how many bytes remaining until orig_iov and orig_iovoff will
	 *  point to the current position in the I/O sequence.
	 */
	byte_count = ctx->io_units_done * blob->bs->io_unit_size;
	orig_iov = &ctx->orig_iov[0];
	orig_iovoff = 0;
	while (byte_count > 0) {
		if (byte_count >= orig_iov->iov_len) {
			byte_count -= orig_iov->iov_len;
			orig_iov++;
		} else {
			orig_iovoff = byte_count;
			byte_count = 0;
		}
	}

	/*
	 * Build an iov array for the next I/O in the sequence.  byte_count will keep track of how many
	 *  bytes of this next I/O remain to be accounted for in the new iov array.
	 */
	byte_count = io_units_count * blob->bs->io_unit_size;
	iov = &ctx->iov[0];
	iovcnt = 0;
	while (byte_count > 0) {
		assert(iovcnt < ctx->iovcnt);
		iov->iov_len = spdk_min(byte_count, orig_iov->iov_len - orig_iovoff);
		iov->iov_base = orig_iov->iov_base + orig_iovoff;
		byte_count -= iov->iov_len;
		orig_iovoff = 0;
		orig_iov++;
		iov++;
		iovcnt++;
	}

	ctx->io_unit_offset += io_units_count;
	ctx->io_units_remaining -= io_units_count;
	ctx->io_units_done += io_units_count;
	iov = &ctx->iov[0];

	if (ctx->read) {
		spdk_blob_io_readv_ext(ctx->blob, ctx->channel, iov, iovcnt, io_unit_offset,
				       io_units_count, rw_iov_split_next, ctx, ctx->ext_io_opts);
	} else {
		spdk_blob_io_writev_ext(ctx->blob, ctx->channel, iov, iovcnt, io_unit_offset,
					io_units_count, rw_iov_split_next, ctx, ctx->ext_io_opts);
	}
}

static void
blob_request_submit_rw_iov(struct spdk_blob *blob, struct spdk_io_channel *_channel,
			   struct iovec *iov, int iovcnt,
			   uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg, bool read,
			   struct spdk_blob_ext_io_opts *ext_io_opts)
{
	struct spdk_bs_cpl	cpl;

	assert(blob != NULL);

	if (!read && blob->data_ro) {
		cb_fn(cb_arg, -EPERM);
		return;
	}

	if (length == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	if (offset + length > bs_cluster_to_lba(blob->bs, blob->active.num_clusters)) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	/*
	 * For now, we implement readv/writev using a sequence (instead of a batch) to account for having
	 *  to split a request that spans a cluster boundary.  For I/O that do not span a cluster boundary,
	 *  there will be no noticeable difference compared to using a batch.  For I/O that do span a cluster
	 *  boundary, the target LBAs (after blob offset to LBA translation) may not be contiguous, so we need
	 *  to allocate a separate iov array and split the I/O such that none of the resulting
	 *  smaller I/O cross a cluster boundary.  These smaller I/O will be issued in sequence (not in parallel)
	 *  but since this case happens very infrequently, any performance impact will be negligible.
	 *
	 * This could be optimized in the future to allocate a big enough iov array to account for all of the iovs
	 *  for all of the smaller I/Os, pre-build all of the iov arrays for the smaller I/Os, then issue them
	 *  in a batch.  That would also require creating an intermediate spdk_bs_cpl that would get called
	 *  when the batch was completed, to allow for freeing the memory for the iov arrays.
	 */
	if (spdk_likely(length <= bs_num_io_units_to_cluster_boundary(blob, offset))) {
		uint64_t lba_count;
		uint64_t lba;
		bool is_allocated;

		cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
		cpl.u.blob_basic.cb_fn = cb_fn;
		cpl.u.blob_basic.cb_arg = cb_arg;

		if (blob->frozen_refcnt) {
			/* This blob I/O is frozen */
			enum spdk_blob_op_type op_type;
			spdk_bs_user_op_t *op;
			struct spdk_bs_channel *bs_channel = spdk_io_channel_get_ctx(_channel);

			op_type = read ? SPDK_BLOB_READV : SPDK_BLOB_WRITEV;
			op = bs_user_op_alloc(_channel, &cpl, op_type, blob, iov, iovcnt, offset, length);
			if (!op) {
				cb_fn(cb_arg, -ENOMEM);
				return;
			}

			TAILQ_INSERT_TAIL(&bs_channel->queued_io, op, link);

			return;
		}

		is_allocated = blob_calculate_lba_and_lba_count(blob, offset, length, &lba, &lba_count);

		if (read) {
			spdk_bs_sequence_t *seq;

			seq = bs_sequence_start_blob(_channel, &cpl, blob);
			if (!seq) {
				cb_fn(cb_arg, -ENOMEM);
				return;
			}

			seq->ext_io_opts = ext_io_opts;

			if (is_allocated) {
				bs_sequence_readv_dev(seq, iov, iovcnt, lba, lba_count, rw_iov_done, NULL);
			} else {
				bs_sequence_readv_bs_dev(seq, blob->back_bs_dev, iov, iovcnt, lba, lba_count,
							 rw_iov_done, NULL);
			}
		} else {
			if (is_allocated) {
				spdk_bs_sequence_t *seq;

				seq = bs_sequence_start_blob(_channel, &cpl, blob);
				if (!seq) {
					cb_fn(cb_arg, -ENOMEM);
					return;
				}

				seq->ext_io_opts = ext_io_opts;

				bs_sequence_writev_dev(seq, iov, iovcnt, lba, lba_count, rw_iov_done, NULL);
			} else {
				/* Queue this operation and allocate the cluster */
				spdk_bs_user_op_t *op;

				op = bs_user_op_alloc(_channel, &cpl, SPDK_BLOB_WRITEV, blob, iov, iovcnt, offset,
						      length);
				if (!op) {
					cb_fn(cb_arg, -ENOMEM);
					return;
				}

				op->ext_io_opts = ext_io_opts;

				bs_allocate_and_copy_cluster(blob, _channel, offset, op);
			}
		}
	} else {
		struct rw_iov_ctx *ctx;

		ctx = calloc(1, sizeof(struct rw_iov_ctx) + iovcnt * sizeof(struct iovec));
		if (ctx == NULL) {
			cb_fn(cb_arg, -ENOMEM);
			return;
		}

		ctx->blob = blob;
		ctx->channel = _channel;
		ctx->cb_fn = cb_fn;
		ctx->cb_arg = cb_arg;
		ctx->read = read;
		ctx->orig_iov = iov;
		ctx->iovcnt = iovcnt;
		ctx->io_unit_offset = offset;
		ctx->io_units_remaining = length;
		ctx->io_units_done = 0;
		ctx->ext_io_opts = ext_io_opts;

		rw_iov_split_next(ctx, 0);
	}
}

static struct spdk_blob *
blob_lookup(struct spdk_blob_store *bs, spdk_blob_id blobid)
{
	struct spdk_blob find;

	if (spdk_bit_array_get(bs->open_blobids, blobid) == 0) {
		return NULL;
	}

	find.id = blobid;
	return RB_FIND(spdk_blob_tree, &bs->open_blobs, &find);
}

static void
blob_get_snapshot_and_clone_entries(struct spdk_blob *blob,
				    struct spdk_blob_list **snapshot_entry, struct spdk_blob_list **clone_entry)
{
	assert(blob != NULL);
	*snapshot_entry = NULL;
	*clone_entry = NULL;

	if (blob->parent_id == SPDK_BLOBID_INVALID) {
		return;
	}

	TAILQ_FOREACH(*snapshot_entry, &blob->bs->snapshots, link) {
		if ((*snapshot_entry)->id == blob->parent_id) {
			break;
		}
	}

	if (*snapshot_entry != NULL) {
		TAILQ_FOREACH(*clone_entry, &(*snapshot_entry)->clones, link) {
			if ((*clone_entry)->id == blob->id) {
				break;
			}
		}

		assert(*clone_entry != NULL);
	}
}

static int
bs_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_blob_store		*bs = io_device;
	struct spdk_bs_channel		*channel = ctx_buf;
	struct spdk_bs_dev		*dev;
	uint32_t			max_ops = bs->max_channel_ops;
	uint32_t			i;

	dev = bs->dev;

	channel->req_mem = calloc(max_ops, sizeof(struct spdk_bs_request_set));
	if (!channel->req_mem) {
		return -1;
	}

	TAILQ_INIT(&channel->reqs);

	for (i = 0; i < max_ops; i++) {
		TAILQ_INSERT_TAIL(&channel->reqs, &channel->req_mem[i], link);
	}

	channel->bs = bs;
	channel->dev = dev;
	channel->dev_channel = dev->create_channel(dev);

	if (!channel->dev_channel) {
		SPDK_ERRLOG("Failed to create device channel.\n");
		free(channel->req_mem);
		return -1;
	}

	channel->new_cluster_page = spdk_zmalloc(bs->md_page_size, 0, NULL, SPDK_ENV_NUMA_ID_ANY,
				    SPDK_MALLOC_DMA);
	if (!channel->new_cluster_page) {
		SPDK_ERRLOG("Failed to allocate new cluster page\n");
		free(channel->req_mem);
		channel->dev->destroy_channel(channel->dev, channel->dev_channel);
		return -1;
	}

	channel->release_cluster_page = spdk_zmalloc(bs->md_page_size, 0, NULL, SPDK_ENV_NUMA_ID_ANY,
					SPDK_MALLOC_DMA);
	if (!channel->release_cluster_page) {
		SPDK_ERRLOG("Failed to allocate release cluster page\n");
		spdk_free(channel->new_cluster_page);
		free(channel->req_mem);
		channel->dev->destroy_channel(channel->dev, channel->dev_channel);
		return -1;
	}

	TAILQ_INIT(&channel->need_cluster_alloc);
	TAILQ_INIT(&channel->queued_io);
	RB_INIT(&channel->esnap_channels);

	return 0;
}

static void
bs_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_bs_channel *channel = ctx_buf;
	spdk_bs_user_op_t *op;

	while (!TAILQ_EMPTY(&channel->need_cluster_alloc)) {
		op = TAILQ_FIRST(&channel->need_cluster_alloc);
		TAILQ_REMOVE(&channel->need_cluster_alloc, op, link);
		bs_user_op_abort(op, -EIO);
	}

	while (!TAILQ_EMPTY(&channel->queued_io)) {
		op = TAILQ_FIRST(&channel->queued_io);
		TAILQ_REMOVE(&channel->queued_io, op, link);
		bs_user_op_abort(op, -EIO);
	}

	blob_esnap_destroy_bs_channel(channel);

	free(channel->req_mem);
	spdk_free(channel->new_cluster_page);
	spdk_free(channel->release_cluster_page);
	channel->dev->destroy_channel(channel->dev, channel->dev_channel);
}

static void
bs_dev_destroy(void *io_device)
{
	struct spdk_blob_store *bs = io_device;
	struct spdk_blob	*blob, *blob_tmp;

	bs->dev->destroy(bs->dev);

	RB_FOREACH_SAFE(blob, spdk_blob_tree, &bs->open_blobs, blob_tmp) {
		RB_REMOVE(spdk_blob_tree, &bs->open_blobs, blob);
		spdk_bit_array_clear(bs->open_blobids, blob->id);
		blob_free(blob);
	}

	spdk_spin_destroy(&bs->used_lock);

	spdk_bit_array_free(&bs->open_blobids);
	spdk_bit_array_free(&bs->used_blobids);
	spdk_bit_array_free(&bs->used_md_pages);
	spdk_bit_pool_free(&bs->used_clusters);
	/*
	 * If this function is called for any reason except a successful unload,
	 * the unload_cpl type will be NONE and this will be a nop.
	 */
	bs_call_cpl(&bs->unload_cpl, bs->unload_err);

	free(bs);
}

static int
bs_blob_list_add(struct spdk_blob *blob)
{
	spdk_blob_id snapshot_id;
	struct spdk_blob_list *snapshot_entry = NULL;
	struct spdk_blob_list *clone_entry = NULL;

	assert(blob != NULL);

	snapshot_id = blob->parent_id;
	if (snapshot_id == SPDK_BLOBID_INVALID ||
	    snapshot_id == SPDK_BLOBID_EXTERNAL_SNAPSHOT) {
		return 0;
	}

	snapshot_entry = bs_get_snapshot_entry(blob->bs, snapshot_id);
	if (snapshot_entry == NULL) {
		/* Snapshot not found */
		snapshot_entry = calloc(1, sizeof(struct spdk_blob_list));
		if (snapshot_entry == NULL) {
			return -ENOMEM;
		}
		snapshot_entry->id = snapshot_id;
		TAILQ_INIT(&snapshot_entry->clones);
		TAILQ_INSERT_TAIL(&blob->bs->snapshots, snapshot_entry, link);
	} else {
		TAILQ_FOREACH(clone_entry, &snapshot_entry->clones, link) {
			if (clone_entry->id == blob->id) {
				break;
			}
		}
	}

	if (clone_entry == NULL) {
		/* Clone not found */
		clone_entry = calloc(1, sizeof(struct spdk_blob_list));
		if (clone_entry == NULL) {
			return -ENOMEM;
		}
		clone_entry->id = blob->id;
		TAILQ_INIT(&clone_entry->clones);
		TAILQ_INSERT_TAIL(&snapshot_entry->clones, clone_entry, link);
		snapshot_entry->clone_count++;
	}

	return 0;
}

static void
bs_blob_list_remove(struct spdk_blob *blob)
{
	struct spdk_blob_list *snapshot_entry = NULL;
	struct spdk_blob_list *clone_entry = NULL;

	blob_get_snapshot_and_clone_entries(blob, &snapshot_entry, &clone_entry);

	if (snapshot_entry == NULL) {
		return;
	}

	blob->parent_id = SPDK_BLOBID_INVALID;
	TAILQ_REMOVE(&snapshot_entry->clones, clone_entry, link);
	free(clone_entry);

	snapshot_entry->clone_count--;
}

static int
bs_blob_list_free(struct spdk_blob_store *bs)
{
	struct spdk_blob_list *snapshot_entry;
	struct spdk_blob_list *snapshot_entry_tmp;
	struct spdk_blob_list *clone_entry;
	struct spdk_blob_list *clone_entry_tmp;

	TAILQ_FOREACH_SAFE(snapshot_entry, &bs->snapshots, link, snapshot_entry_tmp) {
		TAILQ_FOREACH_SAFE(clone_entry, &snapshot_entry->clones, link, clone_entry_tmp) {
			TAILQ_REMOVE(&snapshot_entry->clones, clone_entry, link);
			free(clone_entry);
		}
		TAILQ_REMOVE(&bs->snapshots, snapshot_entry, link);
		free(snapshot_entry);
	}

	return 0;
}

static void
bs_free(struct spdk_blob_store *bs)
{
	bs_blob_list_free(bs);

	bs_unregister_md_thread(bs);
	spdk_io_device_unregister(bs, bs_dev_destroy);
}

/*
 * [한국어]
 * spdk_bs_opts_init - blobstore init/load/grow 옵션 구조체를 디폴트로 초기화 (공개 API)
 *
 * @opts:      초기화할 옵션
 * @opts_size: 호출자가 인식하는 구조체 크기 (forward-compat)
 *
 * 디폴트 값:
 *   cluster_sz = 1 MiB, num_md_pages = SPDK_BLOB_OPTS_NUM_MD_PAGES,
 *   max_md_ops/max_channel_ops 각각 SPDK_BLOB_OPTS_*, clear_method = UNMAP,
 *   bstype = 모두 0, force_recover = false, esnap_bs_dev_create = NULL.
 */
void
spdk_bs_opts_init(struct spdk_bs_opts *opts, size_t opts_size)
{

	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return;
	}

	memset(opts, 0, opts_size);
	opts->opts_size = opts_size;

#define FIELD_OK(field) \
	offsetof(struct spdk_bs_opts, field) + sizeof(opts->field) <= opts_size

#define SET_FIELD(field, value) \
	if (FIELD_OK(field)) { \
		opts->field = value; \
	} \

	SET_FIELD(cluster_sz, SPDK_BLOB_OPTS_CLUSTER_SZ);
	SET_FIELD(num_md_pages, SPDK_BLOB_OPTS_NUM_MD_PAGES);
	SET_FIELD(max_md_ops, SPDK_BLOB_OPTS_NUM_MD_PAGES);
	SET_FIELD(max_channel_ops, SPDK_BLOB_OPTS_DEFAULT_CHANNEL_OPS);
	SET_FIELD(clear_method,  BS_CLEAR_WITH_UNMAP);

	if (FIELD_OK(bstype)) {
		memset(&opts->bstype, 0, sizeof(opts->bstype));
	}

	SET_FIELD(iter_cb_fn, NULL);
	SET_FIELD(iter_cb_arg, NULL);
	SET_FIELD(force_recover, false);
	SET_FIELD(esnap_bs_dev_create, NULL);
	SET_FIELD(esnap_ctx, NULL);

#undef FIELD_OK
#undef SET_FIELD
}

static int
bs_opts_verify(struct spdk_bs_opts *opts)
{
	if (opts->cluster_sz == 0 || opts->num_md_pages == 0 || opts->max_md_ops == 0 ||
	    opts->max_channel_ops == 0) {
		SPDK_ERRLOG("Blobstore options cannot be set to 0\n");
		return -1;
	}

	if ((opts->cluster_sz % SPDK_BS_PAGE_SIZE) != 0) {
		SPDK_ERRLOG("Cluster size %" PRIu32 " is not an integral multiple of blocklen %" PRIu32"\n",
			    opts->cluster_sz, SPDK_BS_PAGE_SIZE);
		return -1;
	}

	return 0;
}

/* START spdk_bs_load */

/* spdk_bs_load_ctx is used for init, load, unload and dump code paths. */

struct spdk_bs_load_ctx {
	struct spdk_blob_store		*bs;
	struct spdk_bs_super_block	*super;

	struct spdk_bs_md_mask		*mask;
	bool				in_page_chain;
	uint32_t			page_index;
	uint32_t			cur_page;
	struct spdk_blob_md_page	*page;

	uint64_t			num_extent_pages;
	uint32_t			*extent_page_num;
	struct spdk_blob_md_page	*extent_pages;
	struct spdk_bit_array		*used_clusters;

	spdk_bs_sequence_t			*seq;
	spdk_blob_op_with_handle_complete	iter_cb_fn;
	void					*iter_cb_arg;
	struct spdk_blob			*blob;
	spdk_blob_id				blobid;

	bool					force_recover;

	/* These fields are used in the spdk_bs_dump path. */
	bool					dumping;
	FILE					*fp;
	spdk_bs_dump_print_xattr		print_xattr_fn;
	char					xattr_name[4096];
};

static void
bs_init_per_cluster_fields(struct spdk_blob_store *bs)
{
	bs->pages_per_cluster = bs->cluster_sz / bs->md_page_size;
	if (spdk_u32_is_pow2(bs->pages_per_cluster)) {
		bs->pages_per_cluster_shift = spdk_u32log2(bs->pages_per_cluster);
	}
	bs->io_units_per_cluster = bs->cluster_sz / bs->io_unit_size;
	if (spdk_u32_is_pow2(bs->io_units_per_cluster)) {
		bs->io_units_per_cluster_shift = spdk_u32log2(bs->io_units_per_cluster);
	}
}

static int
bs_alloc(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts, struct spdk_blob_store **_bs,
	 struct spdk_bs_load_ctx **_ctx)
{
	struct spdk_blob_store	*bs;
	struct spdk_bs_load_ctx	*ctx;
	uint64_t dev_size;
	uint32_t md_page_size;
	int rc;

	dev_size = dev->blocklen * dev->blockcnt;
	if (dev_size < opts->cluster_sz) {
		/* Device size cannot be smaller than cluster size of blobstore */
		SPDK_INFOLOG(blob, "Device size %" PRIu64 " is smaller than cluster size %" PRIu32 "\n",
			     dev_size, opts->cluster_sz);
		return -ENOSPC;
	}

	md_page_size = spdk_max(spdk_max(dev->phys_blocklen, SPDK_BS_PAGE_SIZE),
				opts->md_page_size);
	if (opts->cluster_sz < md_page_size) {
		/* Cluster size cannot be smaller than page size */
		SPDK_ERRLOG("Cluster size %" PRIu32 " is smaller than page size %d\n",
			    opts->cluster_sz, md_page_size);
		return -EINVAL;
	}
	bs = calloc(1, sizeof(struct spdk_blob_store));
	if (!bs) {
		return -ENOMEM;
	}

	ctx = calloc(1, sizeof(struct spdk_bs_load_ctx));
	if (!ctx) {
		free(bs);
		return -ENOMEM;
	}

	ctx->bs = bs;
	ctx->iter_cb_fn = opts->iter_cb_fn;
	ctx->iter_cb_arg = opts->iter_cb_arg;
	ctx->force_recover = opts->force_recover;

	ctx->super = spdk_zmalloc(sizeof(*ctx->super), 0x1000, NULL,
				  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->super) {
		free(ctx);
		free(bs);
		return -ENOMEM;
	}

	RB_INIT(&bs->open_blobs);
	TAILQ_INIT(&bs->snapshots);
	bs->dev = dev;
	bs->md_page_size = md_page_size;
	bs->md_thread = spdk_get_thread();
	assert(bs->md_thread != NULL);

	/*
	 * Do not use bs_lba_to_cluster() here since blockcnt may not be an
	 *  even multiple of the cluster size.
	 */
	bs->cluster_sz = opts->cluster_sz;
	bs->total_clusters = dev->blockcnt / (bs->cluster_sz / dev->blocklen);
	ctx->used_clusters = spdk_bit_array_create(bs->total_clusters);
	if (!ctx->used_clusters) {
		spdk_free(ctx->super);
		free(ctx);
		free(bs);
		return -ENOMEM;
	}

	bs->num_free_clusters = bs->total_clusters;
	bs->io_unit_size = dev->blocklen;
	bs_init_per_cluster_fields(bs);

	bs->max_channel_ops = opts->max_channel_ops;
	bs->super_blob = SPDK_BLOBID_INVALID;
	memcpy(&bs->bstype, &opts->bstype, sizeof(opts->bstype));
	bs->esnap_bs_dev_create = opts->esnap_bs_dev_create;
	bs->esnap_ctx = opts->esnap_ctx;

	/* The metadata is assumed to be at least 1 page */
	bs->used_md_pages = spdk_bit_array_create(1);
	bs->used_blobids = spdk_bit_array_create(0);
	bs->open_blobids = spdk_bit_array_create(0);

	spdk_spin_init(&bs->used_lock);

	spdk_io_device_register(bs, bs_channel_create, bs_channel_destroy,
				sizeof(struct spdk_bs_channel), "blobstore");
	rc = bs_register_md_thread(bs);
	if (rc == -1) {
		spdk_io_device_unregister(bs, NULL);
		spdk_spin_destroy(&bs->used_lock);
		spdk_bit_array_free(&bs->open_blobids);
		spdk_bit_array_free(&bs->used_blobids);
		spdk_bit_array_free(&bs->used_md_pages);
		spdk_bit_array_free(&ctx->used_clusters);
		spdk_free(ctx->super);
		free(ctx);
		free(bs);
		/* FIXME: this is a lie but don't know how to get a proper error code here */
		return -ENOMEM;
	}

	*_ctx = ctx;
	*_bs = bs;
	return 0;
}

static void
bs_write_super(spdk_bs_sequence_t *seq, struct spdk_blob_store *bs,
	       struct spdk_bs_super_block *super, spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	/* Update the values in the super block */
	super->super_blob = bs->super_blob;
	memcpy(&super->bstype, &bs->bstype, sizeof(bs->bstype));
	super->crc = blob_md_page_calc_crc(super);
	bs_sequence_write_dev(seq, super, bs_page_to_lba(bs, 0),
			      bs_byte_to_lba(bs, sizeof(*super)),
			      cb_fn, cb_arg);
}

static void
bs_write_used_clusters(spdk_bs_sequence_t *seq, void *arg, spdk_bs_sequence_cpl cb_fn)
{
	struct spdk_bs_load_ctx	*ctx = arg;
	uint64_t	mask_size, lba, lba_count;

	/* Write out the used clusters mask */
	mask_size = ctx->super->used_cluster_mask_len * ctx->bs->md_page_size;
	ctx->mask = spdk_zmalloc(mask_size, 0x1000, NULL,
				 SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->mask) {
		cb_fn(seq, arg, -ENOMEM);
		return;
	}

	ctx->mask->type = SPDK_MD_MASK_TYPE_USED_CLUSTERS;
	ctx->mask->length = ctx->bs->total_clusters;
	/* We could get here through the normal unload path, or through dirty
	 * shutdown recovery.  For the normal unload path, we use the mask from
	 * the bit pool.  For dirty shutdown recovery, we don't have a bit pool yet -
	 * only the bit array from the load ctx.
	 */
	if (ctx->bs->used_clusters) {
		assert(ctx->mask->length == spdk_bit_pool_capacity(ctx->bs->used_clusters));
		spdk_bit_pool_store_mask(ctx->bs->used_clusters, ctx->mask->mask);
	} else {
		assert(ctx->mask->length == spdk_bit_array_capacity(ctx->used_clusters));
		spdk_bit_array_store_mask(ctx->used_clusters, ctx->mask->mask);
	}
	lba = bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_start);
	lba_count = bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_len);
	bs_sequence_write_dev(seq, ctx->mask, lba, lba_count, cb_fn, arg);
}

static void
bs_write_used_md(spdk_bs_sequence_t *seq, void *arg, spdk_bs_sequence_cpl cb_fn)
{
	struct spdk_bs_load_ctx	*ctx = arg;
	uint64_t	mask_size, lba, lba_count;

	mask_size = ctx->super->used_page_mask_len * ctx->bs->md_page_size;
	ctx->mask = spdk_zmalloc(mask_size, 0x1000, NULL,
				 SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->mask) {
		cb_fn(seq, arg, -ENOMEM);
		return;
	}

	ctx->mask->type = SPDK_MD_MASK_TYPE_USED_PAGES;
	ctx->mask->length = ctx->super->md_len;
	assert(ctx->mask->length == spdk_bit_array_capacity(ctx->bs->used_md_pages));

	spdk_bit_array_store_mask(ctx->bs->used_md_pages, ctx->mask->mask);
	lba = bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_start);
	lba_count = bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_len);
	bs_sequence_write_dev(seq, ctx->mask, lba, lba_count, cb_fn, arg);
}

static void
bs_write_used_blobids(spdk_bs_sequence_t *seq, void *arg, spdk_bs_sequence_cpl cb_fn)
{
	struct spdk_bs_load_ctx	*ctx = arg;
	uint64_t	mask_size, lba, lba_count;

	if (ctx->super->used_blobid_mask_len == 0) {
		/*
		 * This is a pre-v3 on-disk format where the blobid mask does not get
		 *  written to disk.
		 */
		cb_fn(seq, arg, 0);
		return;
	}

	mask_size = ctx->super->used_blobid_mask_len * ctx->bs->md_page_size;
	ctx->mask = spdk_zmalloc(mask_size, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY,
				 SPDK_MALLOC_DMA);
	if (!ctx->mask) {
		cb_fn(seq, arg, -ENOMEM);
		return;
	}

	ctx->mask->type = SPDK_MD_MASK_TYPE_USED_BLOBIDS;
	ctx->mask->length = ctx->super->md_len;
	assert(ctx->mask->length == spdk_bit_array_capacity(ctx->bs->used_blobids));

	spdk_bit_array_store_mask(ctx->bs->used_blobids, ctx->mask->mask);
	lba = bs_page_to_lba(ctx->bs, ctx->super->used_blobid_mask_start);
	lba_count = bs_page_to_lba(ctx->bs, ctx->super->used_blobid_mask_len);
	bs_sequence_write_dev(seq, ctx->mask, lba, lba_count, cb_fn, arg);
}

static void
blob_set_thin_provision(struct spdk_blob *blob)
{
	blob_verify_md_op(blob);
	blob->invalid_flags |= SPDK_BLOB_THIN_PROV;
	blob->state = SPDK_BLOB_STATE_DIRTY;
}

static void
blob_set_clear_method(struct spdk_blob *blob, enum blob_clear_method clear_method)
{
	blob_verify_md_op(blob);
	blob->clear_method = clear_method;
	blob->md_ro_flags |= (clear_method << SPDK_BLOB_CLEAR_METHOD_SHIFT);
	blob->state = SPDK_BLOB_STATE_DIRTY;
}

static void bs_load_iter(void *arg, struct spdk_blob *blob, int bserrno);

static void
bs_delete_corrupted_blob_cpl(void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	spdk_blob_id id;
	int64_t page_num;

	/* Iterate to next blob (we can't use spdk_bs_iter_next function as our
	 * last blob has been removed */
	page_num = bs_blobid_to_page(ctx->blobid);
	page_num++;
	page_num = spdk_bit_array_find_first_set(ctx->bs->used_blobids, page_num);
	if (page_num >= spdk_bit_array_capacity(ctx->bs->used_blobids)) {
		bs_load_iter(ctx, NULL, -ENOENT);
		return;
	}

	id = bs_page_to_blobid(page_num);

	spdk_bs_open_blob(ctx->bs, id, bs_load_iter, ctx);
}

static void
bs_delete_corrupted_close_cb(void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("Failed to close corrupted blob\n");
		spdk_bs_iter_next(ctx->bs, ctx->blob, bs_load_iter, ctx);
		return;
	}

	spdk_bs_delete_blob(ctx->bs, ctx->blobid, bs_delete_corrupted_blob_cpl, ctx);
}

static void
bs_delete_corrupted_blob(void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint64_t i;

	if (bserrno != 0) {
		SPDK_ERRLOG("Failed to close clone of a corrupted blob\n");
		spdk_bs_iter_next(ctx->bs, ctx->blob, bs_load_iter, ctx);
		return;
	}

	/* Snapshot and clone have the same copy of cluster map and extent pages
	 * at this point. Let's clear both for snapshot now,
	 * so that it won't be cleared for clone later when we remove snapshot.
	 * Also set thin provision to pass data corruption check */
	for (i = 0; i < ctx->blob->active.num_clusters; i++) {
		ctx->blob->active.clusters[i] = 0;
	}
	for (i = 0; i < ctx->blob->active.num_extent_pages; i++) {
		ctx->blob->active.extent_pages[i] = 0;
	}

	ctx->blob->active.num_allocated_clusters = 0;

	ctx->blob->md_ro = false;

	blob_set_thin_provision(ctx->blob);

	ctx->blobid = ctx->blob->id;

	spdk_blob_close(ctx->blob, bs_delete_corrupted_close_cb, ctx);
}

static void
bs_update_corrupted_blob(void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("Failed to close clone of a corrupted blob\n");
		spdk_bs_iter_next(ctx->bs, ctx->blob, bs_load_iter, ctx);
		return;
	}

	ctx->blob->md_ro = false;
	blob_remove_xattr(ctx->blob, SNAPSHOT_PENDING_REMOVAL, true);
	blob_remove_xattr(ctx->blob, SNAPSHOT_IN_PROGRESS, true);
	spdk_blob_set_read_only(ctx->blob);

	if (ctx->iter_cb_fn) {
		ctx->iter_cb_fn(ctx->iter_cb_arg, ctx->blob, 0);
	}
	bs_blob_list_add(ctx->blob);

	spdk_bs_iter_next(ctx->bs, ctx->blob, bs_load_iter, ctx);
}

static void
bs_examine_clone(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("Failed to open clone of a corrupted blob\n");
		spdk_bs_iter_next(ctx->bs, ctx->blob, bs_load_iter, ctx);
		return;
	}

	if (blob->parent_id == ctx->blob->id) {
		/* Power failure occurred before updating clone (snapshot delete case)
		 * or after updating clone (creating snapshot case) - keep snapshot */
		spdk_blob_close(blob, bs_update_corrupted_blob, ctx);
	} else {
		/* Power failure occurred after updating clone (snapshot delete case)
		 * or before updating clone (creating snapshot case) - remove snapshot */
		spdk_blob_close(blob, bs_delete_corrupted_blob, ctx);
	}
}

static void
bs_load_iter(void *arg, struct spdk_blob *blob, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = arg;
	const void *value;
	size_t len;
	int rc = 0;

	if (bserrno == 0) {
		/* Examine blob if it is corrupted after power failure. Fix
		 * the ones that can be fixed and remove any other corrupted
		 * ones. If it is not corrupted just process it */
		rc = blob_get_xattr_value(blob, SNAPSHOT_PENDING_REMOVAL, &value, &len, true);
		if (rc != 0) {
			rc = blob_get_xattr_value(blob, SNAPSHOT_IN_PROGRESS, &value, &len, true);
			if (rc != 0) {
				/* Not corrupted - process it and continue with iterating through blobs */
				if (ctx->iter_cb_fn) {
					ctx->iter_cb_fn(ctx->iter_cb_arg, blob, 0);
				}
				bs_blob_list_add(blob);
				spdk_bs_iter_next(ctx->bs, blob, bs_load_iter, ctx);
				return;
			}

		}

		assert(len == sizeof(spdk_blob_id));

		ctx->blob = blob;

		/* Open clone to check if we are able to fix this blob or should we remove it */
		spdk_bs_open_blob(ctx->bs, *(spdk_blob_id *)value, bs_examine_clone, ctx);
		return;
	} else if (bserrno == -ENOENT) {
		bserrno = 0;
	} else {
		/*
		 * This case needs to be looked at further.  Same problem
		 *  exists with applications that rely on explicit blob
		 *  iteration.  We should just skip the blob that failed
		 *  to load and continue on to the next one.
		 */
		SPDK_ERRLOG("Error in iterating blobs\n");
	}

	ctx->iter_cb_fn = NULL;

	spdk_free(ctx->super);
	bs_sequence_finish(ctx->seq, bserrno);
	free(ctx);
}

static void bs_dump_read_md_page(spdk_bs_sequence_t *seq, void *cb_arg);

static void
bs_load_complete(struct spdk_bs_load_ctx *ctx)
{
	ctx->bs->used_clusters = spdk_bit_pool_create_from_array(ctx->used_clusters);
	if (ctx->dumping) {
		bs_dump_read_md_page(ctx->seq, ctx);
		return;
	}
	spdk_bs_iter_first(ctx->bs, bs_load_iter, ctx);
}

static void
bs_load_ctx_fail(struct spdk_bs_load_ctx *ctx, int bserrno)
{
	assert(bserrno != 0);

	spdk_free(ctx->mask);
	spdk_free(ctx->super);
	bs_sequence_finish(ctx->seq, bserrno);
	bs_free(ctx->bs);
	spdk_bit_array_free(&ctx->used_clusters);
	free(ctx);
}

static void
bs_load_used_blobids_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	int rc;

	/* The type must be correct */
	assert(ctx->mask->type == SPDK_MD_MASK_TYPE_USED_BLOBIDS);

	/* The length of the mask (in bits) must not be greater than
	 * the length of the buffer (converted to bits) */
	assert(ctx->mask->length <= (ctx->super->used_blobid_mask_len * ctx->super->md_page_size * 8));

	/* The length of the mask must be exactly equal to the size
	 * (in pages) of the metadata region */
	assert(ctx->mask->length == ctx->super->md_len);

	rc = spdk_bit_array_resize(&ctx->bs->used_blobids, ctx->mask->length);
	if (rc < 0) {
		bs_load_ctx_fail(ctx, rc);
		return;
	}

	spdk_bit_array_load_mask(ctx->bs->used_blobids, ctx->mask->mask);
	spdk_free(ctx->mask);

	bs_load_complete(ctx);
}

static void
bs_load_used_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint64_t		lba, lba_count, mask_size;
	int			rc;

	if (bserrno != 0) {
		bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	/* The type must be correct */
	assert(ctx->mask->type == SPDK_MD_MASK_TYPE_USED_CLUSTERS);
	/* The length of the mask (in bits) must not be greater than the length of the buffer (converted to bits) */
	assert(ctx->mask->length <= (ctx->super->used_cluster_mask_len * sizeof(
					     struct spdk_blob_md_page) * 8));
	/*
	 * The length of the mask must be equal to or larger than the total number of clusters. It may be
	 * larger than the total number of clusters due to a failure spdk_bs_grow.
	 */
	assert(ctx->mask->length >= ctx->bs->total_clusters);
	if (ctx->mask->length > ctx->bs->total_clusters) {
		SPDK_WARNLOG("Shrink the used_custers mask length to total_clusters");
		ctx->mask->length = ctx->bs->total_clusters;
	}

	rc = spdk_bit_array_resize(&ctx->used_clusters, ctx->mask->length);
	if (rc < 0) {
		spdk_free(ctx->mask);
		bs_load_ctx_fail(ctx, rc);
		return;
	}

	spdk_bit_array_load_mask(ctx->used_clusters, ctx->mask->mask);
	ctx->bs->num_free_clusters = spdk_bit_array_count_clear(ctx->used_clusters);
	assert(ctx->bs->num_free_clusters <= ctx->bs->total_clusters);

	spdk_free(ctx->mask);

	/* Read the used blobids mask */
	mask_size = ctx->super->used_blobid_mask_len * ctx->super->md_page_size;
	ctx->mask = spdk_zmalloc(mask_size, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY,
				 SPDK_MALLOC_DMA);
	if (!ctx->mask) {
		bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}
	lba = bs_page_to_lba(ctx->bs, ctx->super->used_blobid_mask_start);
	lba_count = bs_page_to_lba(ctx->bs, ctx->super->used_blobid_mask_len);
	bs_sequence_read_dev(seq, ctx->mask, lba, lba_count,
			     bs_load_used_blobids_cpl, ctx);
}

static void
bs_load_used_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint64_t		lba, lba_count, mask_size;
	int			rc;

	if (bserrno != 0) {
		bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	/* The type must be correct */
	assert(ctx->mask->type == SPDK_MD_MASK_TYPE_USED_PAGES);
	/* The length of the mask (in bits) must not be greater than the length of the buffer (converted to bits) */
	assert(ctx->mask->length <= (ctx->super->used_page_mask_len * ctx->super->md_page_size *
				     8));
	/* The length of the mask must be exactly equal to the size (in pages) of the metadata region */
	if (ctx->mask->length != ctx->super->md_len) {
		SPDK_ERRLOG("mismatched md_len in used_pages mask: "
			    "mask->length=%" PRIu32 " super->md_len=%" PRIu32 "\n",
			    ctx->mask->length, ctx->super->md_len);
		assert(false);
	}

	rc = spdk_bit_array_resize(&ctx->bs->used_md_pages, ctx->mask->length);
	if (rc < 0) {
		bs_load_ctx_fail(ctx, rc);
		return;
	}

	spdk_bit_array_load_mask(ctx->bs->used_md_pages, ctx->mask->mask);
	spdk_free(ctx->mask);

	/* Read the used clusters mask */
	mask_size = ctx->super->used_cluster_mask_len * ctx->super->md_page_size;
	ctx->mask = spdk_zmalloc(mask_size, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY,
				 SPDK_MALLOC_DMA);
	if (!ctx->mask) {
		bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}
	lba = bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_start);
	lba_count = bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_len);
	bs_sequence_read_dev(seq, ctx->mask, lba, lba_count,
			     bs_load_used_clusters_cpl, ctx);
}

static void
bs_load_read_used_pages(struct spdk_bs_load_ctx *ctx)
{
	uint64_t lba, lba_count, mask_size;

	/* Read the used pages mask */
	mask_size = ctx->super->used_page_mask_len * ctx->super->md_page_size;
	ctx->mask = spdk_zmalloc(mask_size, 0x1000, NULL,
				 SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->mask) {
		bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	lba = bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_start);
	lba_count = bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_len);
	bs_sequence_read_dev(ctx->seq, ctx->mask, lba, lba_count,
			     bs_load_used_pages_cpl, ctx);
}

static int
bs_load_replay_md_parse_page(struct spdk_bs_load_ctx *ctx, struct spdk_blob_md_page *page)
{
	struct spdk_blob_store *bs = ctx->bs;
	struct spdk_blob_md_descriptor *desc;
	size_t	cur_desc = 0;

	desc = (struct spdk_blob_md_descriptor *)page->descriptors;
	while (cur_desc < sizeof(page->descriptors)) {
		if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_PADDING) {
			if (desc->length == 0) {
				/* If padding and length are 0, this terminates the page */
				break;
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_RLE) {
			struct spdk_blob_md_descriptor_extent_rle	*desc_extent_rle;
			unsigned int				i, j;
			unsigned int				cluster_count = 0;
			uint32_t				cluster_idx;

			desc_extent_rle = (struct spdk_blob_md_descriptor_extent_rle *)desc;

			for (i = 0; i < desc_extent_rle->length / sizeof(desc_extent_rle->extents[0]); i++) {
				for (j = 0; j < desc_extent_rle->extents[i].length; j++) {
					cluster_idx = desc_extent_rle->extents[i].cluster_idx;
					/*
					 * cluster_idx = 0 means an unallocated cluster - don't mark that
					 * in the used cluster map.
					 */
					if (cluster_idx != 0) {
						SPDK_NOTICELOG("Recover: cluster %" PRIu32 "\n", cluster_idx + j);
						spdk_bit_array_set(ctx->used_clusters, cluster_idx + j);
						if (bs->num_free_clusters == 0) {
							return -ENOSPC;
						}
						bs->num_free_clusters--;
					}
					cluster_count++;
				}
			}
			if (cluster_count == 0) {
				return -EINVAL;
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_PAGE) {
			struct spdk_blob_md_descriptor_extent_page	*desc_extent;
			uint32_t					i;
			uint32_t					cluster_count = 0;
			uint32_t					cluster_idx;
			size_t						cluster_idx_length;

			desc_extent = (struct spdk_blob_md_descriptor_extent_page *)desc;
			cluster_idx_length = desc_extent->length - sizeof(desc_extent->start_cluster_idx);

			if (desc_extent->length <= sizeof(desc_extent->start_cluster_idx) ||
			    (cluster_idx_length % sizeof(desc_extent->cluster_idx[0]) != 0)) {
				return -EINVAL;
			}

			for (i = 0; i < cluster_idx_length / sizeof(desc_extent->cluster_idx[0]); i++) {
				cluster_idx = desc_extent->cluster_idx[i];
				/*
				 * cluster_idx = 0 means an unallocated cluster - don't mark that
				 * in the used cluster map.
				 */
				if (cluster_idx != 0) {
					spdk_bit_array_set(ctx->used_clusters, cluster_idx);
					if (bs->num_free_clusters == 0) {
						return -ENOSPC;
					}
					bs->num_free_clusters--;
				}
				cluster_count++;
			}

			if (cluster_count == 0) {
				return -EINVAL;
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR) {
			/* Skip this item */
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR_INTERNAL) {
			/* Skip this item */
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_FLAGS) {
			/* Skip this item */
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_TABLE) {
			struct spdk_blob_md_descriptor_extent_table *desc_extent_table;
			uint32_t num_extent_pages = ctx->num_extent_pages;
			uint32_t i;
			size_t extent_pages_length;
			void *tmp;

			desc_extent_table = (struct spdk_blob_md_descriptor_extent_table *)desc;
			extent_pages_length = desc_extent_table->length - sizeof(desc_extent_table->num_clusters);

			if (desc_extent_table->length == 0 ||
			    (extent_pages_length % sizeof(desc_extent_table->extent_page[0]) != 0)) {
				return -EINVAL;
			}

			for (i = 0; i < extent_pages_length / sizeof(desc_extent_table->extent_page[0]); i++) {
				if (desc_extent_table->extent_page[i].page_idx != 0) {
					if (desc_extent_table->extent_page[i].num_pages != 1) {
						return -EINVAL;
					}
					num_extent_pages += 1;
				}
			}

			if (num_extent_pages > 0) {
				tmp = realloc(ctx->extent_page_num, num_extent_pages * sizeof(uint32_t));
				if (tmp == NULL) {
					return -ENOMEM;
				}
				ctx->extent_page_num = tmp;

				/* Extent table entries contain md page numbers for extent pages.
				 * Zeroes represent unallocated extent pages, those are run-length-encoded.
				 */
				for (i = 0; i < extent_pages_length / sizeof(desc_extent_table->extent_page[0]); i++) {
					if (desc_extent_table->extent_page[i].page_idx != 0) {
						ctx->extent_page_num[ctx->num_extent_pages] = desc_extent_table->extent_page[i].page_idx;
						ctx->num_extent_pages += 1;
					}
				}
			}
		} else {
			/* Error */
			return -EINVAL;
		}
		/* Advance to the next descriptor */
		cur_desc += sizeof(*desc) + desc->length;
		if (cur_desc + sizeof(*desc) > sizeof(page->descriptors)) {
			break;
		}
		desc = (struct spdk_blob_md_descriptor *)((uintptr_t)page->descriptors + cur_desc);
	}
	return 0;
}

static bool
bs_load_cur_extent_page_valid(struct spdk_blob_md_page *page)
{
	uint32_t crc;
	struct spdk_blob_md_descriptor *desc = (struct spdk_blob_md_descriptor *)page->descriptors;
	size_t desc_len;

	crc = blob_md_page_calc_crc(page);
	if (crc != page->crc) {
		return false;
	}

	/* Extent page should always be of sequence num 0. */
	if (page->sequence_num != 0) {
		return false;
	}

	/* Descriptor type must be EXTENT_PAGE. */
	if (desc->type != SPDK_MD_DESCRIPTOR_TYPE_EXTENT_PAGE) {
		return false;
	}

	/* Descriptor length cannot exceed the page. */
	desc_len = sizeof(*desc) + desc->length;
	if (desc_len > sizeof(page->descriptors)) {
		return false;
	}

	/* It has to be the only descriptor in the page. */
	if (desc_len + sizeof(*desc) <= sizeof(page->descriptors)) {
		desc = (struct spdk_blob_md_descriptor *)((uintptr_t)page->descriptors + desc_len);
		if (desc->length != 0) {
			return false;
		}
	}

	return true;
}

static bool
bs_load_cur_md_page_valid(struct spdk_bs_load_ctx *ctx)
{
	uint32_t crc;
	struct spdk_blob_md_page *page = ctx->page;

	crc = blob_md_page_calc_crc(page);
	if (crc != page->crc) {
		return false;
	}

	/* First page of a sequence should match the blobid. */
	if (page->sequence_num == 0 &&
	    bs_page_to_blobid(ctx->cur_page) != page->id) {
		return false;
	}
	assert(bs_load_cur_extent_page_valid(page) == false);

	return true;
}

static void bs_load_replay_cur_md_page(struct spdk_bs_load_ctx *ctx);

static void
bs_load_write_used_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_free(ctx->mask);
	ctx->mask = NULL;

	if (bserrno != 0) {
		bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	bs_load_complete(ctx);
}

static void
bs_load_write_used_blobids_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_free(ctx->mask);
	ctx->mask = NULL;

	if (bserrno != 0) {
		bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	bs_write_used_clusters(seq, ctx, bs_load_write_used_clusters_cpl);
}

static void
bs_load_write_used_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_free(ctx->mask);
	ctx->mask = NULL;

	if (bserrno != 0) {
		bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	bs_write_used_blobids(seq, ctx, bs_load_write_used_blobids_cpl);
}

static void
bs_load_write_used_md(struct spdk_bs_load_ctx *ctx)
{
	bs_write_used_md(ctx->seq, ctx, bs_load_write_used_pages_cpl);
}

static void
bs_load_replay_md_chain_cpl(struct spdk_bs_load_ctx *ctx)
{
	uint64_t num_md_clusters;
	uint64_t i;

	ctx->in_page_chain = false;

	do {
		ctx->page_index++;
	} while (spdk_bit_array_get(ctx->bs->used_md_pages, ctx->page_index) == true);

	if (ctx->page_index < ctx->super->md_len) {
		ctx->cur_page = ctx->page_index;
		bs_load_replay_cur_md_page(ctx);
	} else {
		/* Claim all of the clusters used by the metadata */
		num_md_clusters = spdk_divide_round_up(
					  ctx->super->md_start + ctx->super->md_len, ctx->bs->pages_per_cluster);
		for (i = 0; i < num_md_clusters; i++) {
			spdk_bit_array_set(ctx->used_clusters, i);
		}
		ctx->bs->num_free_clusters -= num_md_clusters;
		spdk_free(ctx->page);
		bs_load_write_used_md(ctx);
	}
}

static void
bs_load_replay_extent_page_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint32_t page_num;
	uint64_t i;

	if (bserrno != 0) {
		spdk_free(ctx->extent_pages);
		bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	for (i = 0; i < ctx->num_extent_pages; i++) {
		/* Extent pages are only read when present within in chain md.
		 * Integrity of md is not right if that page was not a valid extent page. */
		if (bs_load_cur_extent_page_valid(&ctx->extent_pages[i]) != true) {
			spdk_free(ctx->extent_pages);
			bs_load_ctx_fail(ctx, -EILSEQ);
			return;
		}

		page_num = ctx->extent_page_num[i];
		spdk_bit_array_set(ctx->bs->used_md_pages, page_num);
		if (bs_load_replay_md_parse_page(ctx, &ctx->extent_pages[i])) {
			spdk_free(ctx->extent_pages);
			bs_load_ctx_fail(ctx, -EILSEQ);
			return;
		}
	}

	spdk_free(ctx->extent_pages);
	free(ctx->extent_page_num);
	ctx->extent_page_num = NULL;
	ctx->num_extent_pages = 0;

	bs_load_replay_md_chain_cpl(ctx);
}

static void
bs_load_replay_extent_pages(struct spdk_bs_load_ctx *ctx)
{
	spdk_bs_batch_t *batch;
	uint32_t page;
	uint64_t lba;
	uint64_t i;

	ctx->extent_pages = spdk_zmalloc(ctx->super->md_page_size * ctx->num_extent_pages, 0,
					 NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->extent_pages) {
		bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	batch = bs_sequence_to_batch(ctx->seq, bs_load_replay_extent_page_cpl, ctx);

	for (i = 0; i < ctx->num_extent_pages; i++) {
		page = ctx->extent_page_num[i];
		assert(page < ctx->super->md_len);
		lba = bs_md_page_to_lba(ctx->bs, page);
		bs_batch_read_dev(batch, &ctx->extent_pages[i], lba,
				  bs_byte_to_lba(ctx->bs, ctx->super->md_page_size));
	}

	bs_batch_close(batch);
}

static void
bs_load_replay_md_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint32_t page_num;
	struct spdk_blob_md_page *page;

	if (bserrno != 0) {
		bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	page_num = ctx->cur_page;
	page = ctx->page;
	if (bs_load_cur_md_page_valid(ctx) == true) {
		if (page->sequence_num == 0 || ctx->in_page_chain == true) {
			spdk_spin_lock(&ctx->bs->used_lock);
			bs_claim_md_page(ctx->bs, page_num);
			spdk_spin_unlock(&ctx->bs->used_lock);
			if (page->sequence_num == 0) {
				SPDK_NOTICELOG("Recover: blob 0x%" PRIx32 "\n", page_num);
				spdk_bit_array_set(ctx->bs->used_blobids, page_num);
			}
			if (bs_load_replay_md_parse_page(ctx, page)) {
				bs_load_ctx_fail(ctx, -EILSEQ);
				return;
			}
			if (page->next != SPDK_INVALID_MD_PAGE) {
				ctx->in_page_chain = true;
				ctx->cur_page = page->next;
				bs_load_replay_cur_md_page(ctx);
				return;
			}
			if (ctx->num_extent_pages != 0) {
				bs_load_replay_extent_pages(ctx);
				return;
			}
		}
	}
	bs_load_replay_md_chain_cpl(ctx);
}

static void
bs_load_replay_cur_md_page(struct spdk_bs_load_ctx *ctx)
{
	uint64_t lba;

	assert(ctx->cur_page < ctx->super->md_len);
	lba = bs_md_page_to_lba(ctx->bs, ctx->cur_page);
	bs_sequence_read_dev(ctx->seq, ctx->page, lba,
			     bs_byte_to_lba(ctx->bs, ctx->super->md_page_size),
			     bs_load_replay_md_cpl, ctx);
}

static void
bs_load_replay_md(struct spdk_bs_load_ctx *ctx)
{
	ctx->page_index = 0;
	ctx->cur_page = 0;
	ctx->page = spdk_zmalloc(ctx->bs->md_page_size, 0,
				 NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->page) {
		bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}
	bs_load_replay_cur_md_page(ctx);
}

static void
bs_recover(struct spdk_bs_load_ctx *ctx)
{
	int		rc;

	SPDK_NOTICELOG("Performing recovery on blobstore\n");
	rc = spdk_bit_array_resize(&ctx->bs->used_md_pages, ctx->super->md_len);
	if (rc < 0) {
		bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	rc = spdk_bit_array_resize(&ctx->bs->used_blobids, ctx->super->md_len);
	if (rc < 0) {
		bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	rc = spdk_bit_array_resize(&ctx->used_clusters, ctx->bs->total_clusters);
	if (rc < 0) {
		bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	rc = spdk_bit_array_resize(&ctx->bs->open_blobids, ctx->super->md_len);
	if (rc < 0) {
		bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	ctx->bs->num_free_clusters = ctx->bs->total_clusters;
	bs_load_replay_md(ctx);
}

static int
bs_parse_super(struct spdk_bs_load_ctx *ctx)
{
	int rc;

	if (ctx->super->size == 0) {
		ctx->super->size = ctx->bs->dev->blockcnt * ctx->bs->dev->blocklen;
	}

	if (ctx->super->io_unit_size == 0) {
		ctx->super->io_unit_size = SPDK_BS_PAGE_SIZE;
	}
	if (ctx->super->md_page_size == 0) {
		ctx->super->md_page_size = SPDK_BS_PAGE_SIZE;
	}

	ctx->bs->clean = 1;
	ctx->bs->cluster_sz = ctx->super->cluster_size;
	ctx->bs->total_clusters = ctx->super->size / ctx->super->cluster_size;
	ctx->bs->io_unit_size = ctx->super->io_unit_size;
	ctx->bs->md_page_size = ctx->super->md_page_size;
	bs_init_per_cluster_fields(ctx->bs);
	rc = spdk_bit_array_resize(&ctx->used_clusters, ctx->bs->total_clusters);
	if (rc < 0) {
		return -ENOMEM;
	}
	ctx->bs->md_start = ctx->super->md_start;
	ctx->bs->md_len = ctx->super->md_len;
	rc = spdk_bit_array_resize(&ctx->bs->open_blobids, ctx->bs->md_len);
	if (rc < 0) {
		return -ENOMEM;
	}

	ctx->bs->total_data_clusters = ctx->bs->total_clusters - spdk_divide_round_up(
					       ctx->bs->md_start + ctx->bs->md_len, ctx->bs->pages_per_cluster);
	ctx->bs->super_blob = ctx->super->super_blob;
	memcpy(&ctx->bs->bstype, &ctx->super->bstype, sizeof(ctx->super->bstype));

	return 0;
}

static void
bs_load_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	int rc;

	rc = bs_super_validate(ctx->super, ctx->bs);
	if (rc != 0) {
		bs_load_ctx_fail(ctx, rc);
		return;
	}

	rc = bs_parse_super(ctx);
	if (rc < 0) {
		bs_load_ctx_fail(ctx, rc);
		return;
	}

	if (ctx->super->used_blobid_mask_len == 0 || ctx->super->clean == 0 || ctx->force_recover) {
		bs_recover(ctx);
	} else {
		bs_load_read_used_pages(ctx);
	}
}

static inline int
bs_opts_copy(struct spdk_bs_opts *src, struct spdk_bs_opts *dst)
{

	if (!src->opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return -1;
	}

#define FIELD_OK(field) \
        offsetof(struct spdk_bs_opts, field) + sizeof(src->field) <= src->opts_size

#define SET_FIELD(field) \
        if (FIELD_OK(field)) { \
                dst->field = src->field; \
        } \

	SET_FIELD(cluster_sz);
	SET_FIELD(num_md_pages);
	SET_FIELD(max_md_ops);
	SET_FIELD(max_channel_ops);
	SET_FIELD(clear_method);

	if (FIELD_OK(bstype)) {
		memcpy(&dst->bstype, &src->bstype, sizeof(dst->bstype));
	}
	SET_FIELD(md_page_size);
	SET_FIELD(iter_cb_fn);
	SET_FIELD(iter_cb_arg);
	SET_FIELD(force_recover);
	SET_FIELD(esnap_bs_dev_create);
	SET_FIELD(esnap_ctx);

	dst->opts_size = src->opts_size;

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_bs_opts) == 88, "Incorrect size");

#undef FIELD_OK
#undef SET_FIELD

	return 0;
}

/*
 * [한국어]
 * spdk_bs_load - 기존 blobstore가 들어 있는 bs_dev를 읽어 in-memory 핸들 복원 (공개 API)
 *
 * @dev:    blobstore가 이미 기록된 backing bs_dev
 * @o:      옵션 (NULL=디폴트, max_md_ops/max_channel_ops 등)
 * @cb_fn:  완료 콜백 (cb_arg, bs, bserrno)
 *
 * 동작 단계:
 * 1) phys_blocklen / blocklen 정합성 검사
 * 2) 옵션 복사·검증 (max_md_ops != 0, max_channel_ops != 0)
 * 3) bs_alloc — 빈 spdk_blob_store 본체 할당
 * 4) bs_sequence_start_bs로 시퀀스 시작
 * 5) bs_sequence_read_dev로 super block(page 0) 읽기 시작 → bs_load_super_cpl로 콜백 체인:
 *    - super 검증 (signature/CRC/version)
 *    - clean=0이거나 force_recover면 replay 모드 → md page 전체 read 후 used 비트맵 재구성
 *    - clean=1이면 used_md/cluster/blobid mask들을 직접 read해 비트맵 즉시 복원
 *    - 마지막에 super.clean=0 재기록(다음 unload 때 1로 다시 mark)
 *    - cb_fn(cb_arg, bs, 0) 호출
 *
 * Esnap 지원: 옵션의 esnap_bs_dev_create 콜백이 등록되어 있으면 esnap clone을 만났을 때
 * 부모 bs_dev를 만들어 달라는 요청을 한다.
 *
 * 호출 체인:
 *   사용자 → spdk_bs_load → bs_alloc → bs_sequence_start_bs → bs_sequence_read_dev(super)
 *     → bs_load_super_cpl → (clean ? bs_load_replay_md : 직접 mask 읽기) → cb_fn
 */
void
spdk_bs_load(struct spdk_bs_dev *dev, struct spdk_bs_opts *o,
	     spdk_bs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_store	*bs;                       /* [한국어] 복원할 blobstore 핸들 */
	struct spdk_bs_cpl	cpl;                       /* [한국어] 시퀀스 완료 통보 */
	struct spdk_bs_load_ctx *ctx;                      /* [한국어] load 콜백 체인 컨텍스트 */
	struct spdk_bs_opts	opts = {};                 /* [한국어] 정규화된 옵션 */
	int err;

	SPDK_DEBUGLOG(blob, "Loading blobstore from dev %p\n", dev);

	if ((dev->phys_blocklen % dev->blocklen) != 0) {   /* [한국어] block length 정합성 */
		SPDK_DEBUGLOG(blob, "unsupported dev block length of %d\n", dev->blocklen);
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	spdk_bs_opts_init(&opts, sizeof(opts));            /* [한국어] 디폴트 채움 */
	if (o) {
		if (bs_opts_copy(o, &opts)) {
			dev->destroy(dev);
			cb_fn(cb_arg, NULL, -EINVAL);
			return;
		}
	}

	if (opts.max_md_ops == 0 || opts.max_channel_ops == 0) { /* [한국어] 0이면 큐 사이즈 비정상 */
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	err = bs_alloc(dev, &opts, &bs, &ctx);             /* [한국어] bs/super/ctx 할당 */
	if (err) {
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, err);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_HANDLE;             /* [한국어] 완료 시 bs 핸들을 반환 */
	cpl.u.bs_handle.cb_fn = cb_fn;
	cpl.u.bs_handle.cb_arg = cb_arg;
	cpl.u.bs_handle.bs = bs;

	ctx->seq = bs_sequence_start_bs(bs->md_channel, &cpl); /* [한국어] 시퀀스 시작 */
	if (!ctx->seq) {
		spdk_free(ctx->super);
		free(ctx);
		bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	/* Read the super block */
	/* [한국어] super block(page 0)을 디스크에서 읽어 와 bs_load_super_cpl 콜백으로 검증 시작. */
	bs_sequence_read_dev(ctx->seq, ctx->super, bs_page_to_lba(bs, 0),
			     bs_byte_to_lba(bs, sizeof(*ctx->super)),
			     bs_load_super_cpl, ctx);
}

/* END spdk_bs_load */

/* START spdk_bs_dump */

static void
bs_dump_finish(spdk_bs_sequence_t *seq, struct spdk_bs_load_ctx *ctx, int bserrno)
{
	spdk_free(ctx->super);

	/*
	 * We need to defer calling bs_call_cpl() until after
	 * dev destruction, so tuck these away for later use.
	 */
	ctx->bs->unload_err = bserrno;
	memcpy(&ctx->bs->unload_cpl, &seq->cpl, sizeof(struct spdk_bs_cpl));
	seq->cpl.type = SPDK_BS_CPL_TYPE_NONE;

	bs_sequence_finish(seq, 0);
	bs_free(ctx->bs);
	free(ctx);
}

static void
bs_dump_print_xattr(struct spdk_bs_load_ctx *ctx, struct spdk_blob_md_descriptor *desc)
{
	struct spdk_blob_md_descriptor_xattr *desc_xattr;
	uint32_t i;
	const char *type;

	desc_xattr = (struct spdk_blob_md_descriptor_xattr *)desc;

	if (desc_xattr->length !=
	    sizeof(desc_xattr->name_length) + sizeof(desc_xattr->value_length) +
	    desc_xattr->name_length + desc_xattr->value_length) {
	}

	memcpy(ctx->xattr_name, desc_xattr->name, desc_xattr->name_length);
	ctx->xattr_name[desc_xattr->name_length] = '\0';
	if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR) {
		type = "XATTR";
	} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR_INTERNAL) {
		type = "XATTR_INTERNAL";
	} else {
		assert(false);
		type = "XATTR_?";
	}
	fprintf(ctx->fp, "%s: name = \"%s\"\n", type, ctx->xattr_name);
	fprintf(ctx->fp, "       value = \"");
	ctx->print_xattr_fn(ctx->fp, ctx->super->bstype.bstype, ctx->xattr_name,
			    (void *)((uintptr_t)desc_xattr->name + desc_xattr->name_length),
			    desc_xattr->value_length);
	fprintf(ctx->fp, "\"\n");
	for (i = 0; i < desc_xattr->value_length; i++) {
		if (i % 16 == 0) {
			fprintf(ctx->fp, "               ");
		}
		fprintf(ctx->fp, "%02" PRIx8 " ", *((uint8_t *)desc_xattr->name + desc_xattr->name_length + i));
		if ((i + 1) % 16 == 0) {
			fprintf(ctx->fp, "\n");
		}
	}
	if (i % 16 != 0) {
		fprintf(ctx->fp, "\n");
	}
}

struct type_flag_desc {
	uint64_t mask;
	uint64_t val;
	const char *name;
};

static void
bs_dump_print_type_bits(struct spdk_bs_load_ctx *ctx, uint64_t flags,
			struct type_flag_desc *desc, size_t numflags)
{
	uint64_t covered = 0;
	size_t i;

	for (i = 0; i < numflags; i++) {
		if ((desc[i].mask & flags) != desc[i].val) {
			continue;
		}
		fprintf(ctx->fp, "\t\t 0x%016" PRIx64 " %s", desc[i].val, desc[i].name);
		if (desc[i].mask != desc[i].val) {
			fprintf(ctx->fp, " (mask 0x%" PRIx64 " value 0x%" PRIx64 ")",
				desc[i].mask, desc[i].val);
		}
		fprintf(ctx->fp, "\n");
		covered |= desc[i].mask;
	}
	if ((flags & ~covered) != 0) {
		fprintf(ctx->fp, "\t\t 0x%016" PRIx64 " Unknown\n", flags & ~covered);
	}
}

static void
bs_dump_print_type_flags(struct spdk_bs_load_ctx *ctx, struct spdk_blob_md_descriptor *desc)
{
	struct spdk_blob_md_descriptor_flags *type_desc;
#define ADD_FLAG(f) { f, f, #f }
#define ADD_MASK_VAL(m, v) { m, v, #v }
	static struct type_flag_desc invalid[] = {
		ADD_FLAG(SPDK_BLOB_THIN_PROV),
		ADD_FLAG(SPDK_BLOB_INTERNAL_XATTR),
		ADD_FLAG(SPDK_BLOB_EXTENT_TABLE),
	};
	static struct type_flag_desc data_ro[] = {
		ADD_FLAG(SPDK_BLOB_READ_ONLY),
	};
	static struct type_flag_desc md_ro[] = {
		ADD_MASK_VAL(SPDK_BLOB_MD_RO_FLAGS_MASK, BLOB_CLEAR_WITH_DEFAULT),
		ADD_MASK_VAL(SPDK_BLOB_MD_RO_FLAGS_MASK, BLOB_CLEAR_WITH_NONE),
		ADD_MASK_VAL(SPDK_BLOB_MD_RO_FLAGS_MASK, BLOB_CLEAR_WITH_UNMAP),
		ADD_MASK_VAL(SPDK_BLOB_MD_RO_FLAGS_MASK, BLOB_CLEAR_WITH_WRITE_ZEROES),
	};
#undef ADD_FLAG
#undef ADD_MASK_VAL

	type_desc = (struct spdk_blob_md_descriptor_flags *)desc;
	fprintf(ctx->fp, "Flags:\n");
	fprintf(ctx->fp, "\tinvalid: 0x%016" PRIx64 "\n", type_desc->invalid_flags);
	bs_dump_print_type_bits(ctx, type_desc->invalid_flags, invalid,
				SPDK_COUNTOF(invalid));
	fprintf(ctx->fp, "\tdata_ro: 0x%016" PRIx64 "\n", type_desc->data_ro_flags);
	bs_dump_print_type_bits(ctx, type_desc->data_ro_flags, data_ro,
				SPDK_COUNTOF(data_ro));
	fprintf(ctx->fp, "\t  md_ro: 0x%016" PRIx64 "\n", type_desc->md_ro_flags);
	bs_dump_print_type_bits(ctx, type_desc->md_ro_flags, md_ro,
				SPDK_COUNTOF(md_ro));
}

static void
bs_dump_print_extent_table(struct spdk_bs_load_ctx *ctx, struct spdk_blob_md_descriptor *desc)
{
	struct spdk_blob_md_descriptor_extent_table *et_desc;
	uint64_t num_extent_pages;
	uint32_t et_idx;

	et_desc = (struct spdk_blob_md_descriptor_extent_table *)desc;
	num_extent_pages = (et_desc->length - sizeof(et_desc->num_clusters)) /
			   sizeof(et_desc->extent_page[0]);

	fprintf(ctx->fp, "Extent table:\n");
	for (et_idx = 0; et_idx < num_extent_pages; et_idx++) {
		if (et_desc->extent_page[et_idx].page_idx == 0) {
			/* Zeroes represent unallocated extent pages. */
			continue;
		}
		fprintf(ctx->fp, "\tExtent page: %5" PRIu32 " length %3" PRIu32
			" at LBA %" PRIu64 "\n", et_desc->extent_page[et_idx].page_idx,
			et_desc->extent_page[et_idx].num_pages,
			bs_md_page_to_lba(ctx->bs, et_desc->extent_page[et_idx].page_idx));
	}
}

static void
bs_dump_print_md_page(struct spdk_bs_load_ctx *ctx)
{
	uint32_t page_idx = ctx->cur_page;
	struct spdk_blob_md_page *page = ctx->page;
	struct spdk_blob_md_descriptor *desc;
	size_t cur_desc = 0;
	uint32_t crc;

	fprintf(ctx->fp, "=========\n");
	fprintf(ctx->fp, "Metadata Page Index: %" PRIu32 " (0x%" PRIx32 ")\n", page_idx, page_idx);
	fprintf(ctx->fp, "Start LBA: %" PRIu64 "\n", bs_md_page_to_lba(ctx->bs, page_idx));
	fprintf(ctx->fp, "Blob ID: 0x%" PRIx64 "\n", page->id);
	fprintf(ctx->fp, "Sequence: %" PRIu32 "\n", page->sequence_num);
	if (page->next == SPDK_INVALID_MD_PAGE) {
		fprintf(ctx->fp, "Next: None\n");
	} else {
		fprintf(ctx->fp, "Next: %" PRIu32 "\n", page->next);
	}
	fprintf(ctx->fp, "In used bit array%s:", ctx->super->clean ? "" : " (not clean: dubious)");
	if (spdk_bit_array_get(ctx->bs->used_md_pages, page_idx)) {
		fprintf(ctx->fp, " md");
	}
	if (spdk_bit_array_get(ctx->bs->used_blobids, page_idx)) {
		fprintf(ctx->fp, " blob");
	}
	fprintf(ctx->fp, "\n");

	crc = blob_md_page_calc_crc(page);
	fprintf(ctx->fp, "CRC: 0x%" PRIx32 " (%s)\n", page->crc, crc == page->crc ? "OK" : "Mismatch");

	desc = (struct spdk_blob_md_descriptor *)page->descriptors;
	while (cur_desc < sizeof(page->descriptors)) {
		if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_PADDING) {
			if (desc->length == 0) {
				/* If padding and length are 0, this terminates the page */
				break;
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_RLE) {
			struct spdk_blob_md_descriptor_extent_rle	*desc_extent_rle;
			unsigned int				i;

			desc_extent_rle = (struct spdk_blob_md_descriptor_extent_rle *)desc;

			for (i = 0; i < desc_extent_rle->length / sizeof(desc_extent_rle->extents[0]); i++) {
				if (desc_extent_rle->extents[i].cluster_idx != 0) {
					fprintf(ctx->fp, "Allocated Extent - Start: %" PRIu32,
						desc_extent_rle->extents[i].cluster_idx);
				} else {
					fprintf(ctx->fp, "Unallocated Extent - ");
				}
				fprintf(ctx->fp, " Length: %" PRIu32, desc_extent_rle->extents[i].length);
				fprintf(ctx->fp, "\n");
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_PAGE) {
			struct spdk_blob_md_descriptor_extent_page	*desc_extent;
			unsigned int					i;

			desc_extent = (struct spdk_blob_md_descriptor_extent_page *)desc;

			for (i = 0;
			     i < (desc_extent->length - sizeof(desc_extent->start_cluster_idx)) / sizeof(
				     desc_extent->cluster_idx[0]); i++) {
				if (desc_extent->cluster_idx[i] != 0) {
					fprintf(ctx->fp, "Allocated Extent - Start: %" PRIu32,
						desc_extent->cluster_idx[i]);
				} else {
					fprintf(ctx->fp, "Unallocated Extent");
				}
				fprintf(ctx->fp, "\n");
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR) {
			bs_dump_print_xattr(ctx, desc);
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR_INTERNAL) {
			bs_dump_print_xattr(ctx, desc);
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_FLAGS) {
			bs_dump_print_type_flags(ctx, desc);
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_TABLE) {
			bs_dump_print_extent_table(ctx, desc);
		} else {
			/* Error */
			fprintf(ctx->fp, "Unknown descriptor type %" PRIu8 "\n", desc->type);
		}
		/* Advance to the next descriptor */
		cur_desc += sizeof(*desc) + desc->length;
		if (cur_desc + sizeof(*desc) > sizeof(page->descriptors)) {
			break;
		}
		desc = (struct spdk_blob_md_descriptor *)((uintptr_t)page->descriptors + cur_desc);
	}
}

static void
bs_dump_read_md_page_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		bs_dump_finish(seq, ctx, bserrno);
		return;
	}

	if (ctx->page->id != 0) {
		bs_dump_print_md_page(ctx);
	}

	ctx->cur_page++;

	if (ctx->cur_page < ctx->super->md_len) {
		bs_dump_read_md_page(seq, ctx);
	} else {
		spdk_free(ctx->page);
		bs_dump_finish(seq, ctx, 0);
	}
}

static void
bs_dump_read_md_page(spdk_bs_sequence_t *seq, void *cb_arg)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint64_t lba;

	assert(ctx->cur_page < ctx->super->md_len);
	lba = bs_page_to_lba(ctx->bs, ctx->super->md_start + ctx->cur_page);
	bs_sequence_read_dev(seq, ctx->page, lba,
			     bs_byte_to_lba(ctx->bs, ctx->super->md_page_size),
			     bs_dump_read_md_page_cpl, ctx);
}

static void
bs_dump_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	int rc;

	fprintf(ctx->fp, "Signature: \"%.8s\" ", ctx->super->signature);
	if (memcmp(ctx->super->signature, SPDK_BS_SUPER_BLOCK_SIG,
		   sizeof(ctx->super->signature)) != 0) {
		fprintf(ctx->fp, "(Mismatch)\n");
		bs_dump_finish(seq, ctx, bserrno);
		return;
	} else {
		fprintf(ctx->fp, "(OK)\n");
	}
	fprintf(ctx->fp, "Version: %" PRIu32 "\n", ctx->super->version);
	fprintf(ctx->fp, "CRC: 0x%x (%s)\n", ctx->super->crc,
		(ctx->super->crc == blob_md_page_calc_crc(ctx->super)) ? "OK" : "Mismatch");
	fprintf(ctx->fp, "Blobstore Type: %.*s\n", SPDK_BLOBSTORE_TYPE_LENGTH, ctx->super->bstype.bstype);
	fprintf(ctx->fp, "Cluster Size: %" PRIu32 "\n", ctx->super->cluster_size);
	fprintf(ctx->fp, "Super Blob ID: ");
	if (ctx->super->super_blob == SPDK_BLOBID_INVALID) {
		fprintf(ctx->fp, "(None)\n");
	} else {
		fprintf(ctx->fp, "0x%" PRIx64 "\n", ctx->super->super_blob);
	}
	fprintf(ctx->fp, "Clean: %" PRIu32 "\n", ctx->super->clean);
	fprintf(ctx->fp, "Used Metadata Page Mask Start: %" PRIu32 "\n", ctx->super->used_page_mask_start);
	fprintf(ctx->fp, "Used Metadata Page Mask Length: %" PRIu32 "\n", ctx->super->used_page_mask_len);
	fprintf(ctx->fp, "Used Cluster Mask Start: %" PRIu32 "\n", ctx->super->used_cluster_mask_start);
	fprintf(ctx->fp, "Used Cluster Mask Length: %" PRIu32 "\n", ctx->super->used_cluster_mask_len);
	fprintf(ctx->fp, "Used Blob ID Mask Start: %" PRIu32 "\n", ctx->super->used_blobid_mask_start);
	fprintf(ctx->fp, "Used Blob ID Mask Length: %" PRIu32 "\n", ctx->super->used_blobid_mask_len);
	fprintf(ctx->fp, "Metadata Start: %" PRIu32 "\n", ctx->super->md_start);
	fprintf(ctx->fp, "Metadata Length: %" PRIu32 "\n", ctx->super->md_len);

	ctx->cur_page = 0;
	ctx->page = spdk_zmalloc(ctx->super->md_page_size, 0,
				 NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->page) {
		bs_dump_finish(seq, ctx, -ENOMEM);
		return;
	}

	rc = bs_parse_super(ctx);
	if (rc < 0) {
		bs_load_ctx_fail(ctx, rc);
		return;
	}

	bs_load_read_used_pages(ctx);
}

/*
 * [한국어]
 * spdk_bs_dump - bs_dev에 들어 있는 blobstore의 메타를 텍스트로 덤프 (공개 API)
 *
 * @dev:           읽을 backing bs_dev (load와 마찬가지로 소유권 인계)
 * @fp:            출력할 FILE*
 * @print_xattr_fn:사용자 xattr 출력 콜백 (NULL 가능)
 *
 * 디버깅/포렌식 도구 — super block, used 비트맵, 모든 md page descriptor를 사람이 읽을
 * 수 있는 형식으로 출력한다. 실제 IO는 발생하지 않으며 (read만), bs는 fully load되지
 * 않는다 (dumping 플래그가 true).
 */
void
spdk_bs_dump(struct spdk_bs_dev *dev, FILE *fp, spdk_bs_dump_print_xattr print_xattr_fn,
	     spdk_bs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_store	*bs;
	struct spdk_bs_cpl	cpl;
	struct spdk_bs_load_ctx *ctx;
	struct spdk_bs_opts	opts = {};
	int err;

	SPDK_DEBUGLOG(blob, "Dumping blobstore from dev %p\n", dev);

	spdk_bs_opts_init(&opts, sizeof(opts));

	err = bs_alloc(dev, &opts, &bs, &ctx);
	if (err) {
		dev->destroy(dev);
		cb_fn(cb_arg, err);
		return;
	}

	ctx->dumping = true;
	ctx->fp = fp;
	ctx->print_xattr_fn = print_xattr_fn;

	cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	cpl.u.bs_basic.cb_fn = cb_fn;
	cpl.u.bs_basic.cb_arg = cb_arg;

	ctx->seq = bs_sequence_start_bs(bs->md_channel, &cpl);
	if (!ctx->seq) {
		spdk_free(ctx->super);
		free(ctx);
		bs_free(bs);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* Read the super block */
	bs_sequence_read_dev(ctx->seq, ctx->super, bs_page_to_lba(bs, 0),
			     bs_byte_to_lba(bs, sizeof(*ctx->super)),
			     bs_dump_super_cpl, ctx);
}

/* END spdk_bs_dump */

/* START spdk_bs_init */

static void
bs_init_persist_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;

	ctx->bs->used_clusters = spdk_bit_pool_create_from_array(ctx->used_clusters);
	spdk_free(ctx->super);
	free(ctx);

	bs_sequence_finish(seq, bserrno);
}

static void
bs_init_trim_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;

	/* Write super block */
	bs_sequence_write_dev(seq, ctx->super, bs_page_to_lba(ctx->bs, 0),
			      bs_byte_to_lba(ctx->bs, sizeof(*ctx->super)),
			      bs_init_persist_super_cpl, ctx);
}

/*
 * [한국어]
 * spdk_bs_init - 빈 bs_dev 위에 새 blobstore를 만들어 핸들을 비동기 반환 (공개 API)
 *
 * @dev:    아래에 깔린 bs_dev (수명은 unload까지 blobstore가 인계받음)
 * @o:      옵션 (NULL이면 디폴트). cluster_size, io_unit_size 등.
 * @cb_fn:  완료 콜백 — (cb_arg, struct spdk_blob_store *bs, int bserrno)
 * @cb_arg: 콜백 인자
 *
 * 동작 단계:
 * 1) phys_blocklen / blocklen 정합성 검사
 * 2) 옵션 복사·검증 (bs_opts_copy / bs_opts_verify)
 * 3) bs_alloc — spdk_blob_store 본체와 super page 컨텍스트 할당
 * 4) used_md / used_blobids / open_blobids 비트 배열을 md_len 크기로 확장
 * 5) super block 영속 필드 채움 (signature, version, sizes, bstype …)
 * 6) 메타 영역 layout 계산:
 *    - super(1 page) + used_page_mask + used_cluster_mask + used_blobid_mask + md_pages
 *    - 향후 grow를 대비해 used_cluster_mask_len의 최댓값(= md_len 기준)으로 예약
 * 7) 메타 영역에 해당하는 cluster를 used 비트맵에 미리 점유
 * 8) bs_sequence_start_bs로 비동기 시퀀스 시작:
 *    - batch.write_zeroes(0, num_md_lba)        : 메타 영역 zero
 *    - clear_method 따라 데이터 영역 trim/zero/none
 *    - 완료 시 bs_init_trim_cpl → super write → bs_init_persist_super_cpl
 *    - 마지막에 ctx->bs를 콜백으로 반환
 *
 * 실행 컨텍스트: 호출 스레드 — 이 함수는 동기적으로 시작만 하고 콜백은 md 채널 스레드에서.
 * 모든 실패 경로에서 dev->destroy(dev) 호출로 backing dev 소유권을 넘겨받았음을 처리.
 *
 * 호출 체인:
 *   사용자 → spdk_bs_init
 *     → bs_alloc → bs_sequence_start_bs → batch.write_zeroes → bs_init_trim_cpl
 *     → bs_sequence_write_dev(super) → bs_init_persist_super_cpl
 *     → spdk_bit_pool_create_from_array(used_clusters) + cb_fn(..., bs, 0)
 */
void
spdk_bs_init(struct spdk_bs_dev *dev, struct spdk_bs_opts *o,
	     spdk_bs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_load_ctx *ctx;                      /* [한국어] init 콜백 체인 컨텍스트 */
	struct spdk_blob_store	*bs;                       /* [한국어] 새로 만들 blobstore 핸들 */
	struct spdk_bs_cpl	cpl;                       /* [한국어] sequence 완료 통보 */
	spdk_bs_sequence_t	*seq;                      /* [한국어] 비동기 시퀀스 */
	spdk_bs_batch_t		*batch;                    /* [한국어] 메타 zero/trim 배치 */
	uint64_t		num_md_lba;                /* [한국어] 메타 전체 LBA 수 */
	uint64_t		num_md_pages;              /* [한국어] 메타 영역 page 수 누적 */
	uint64_t		num_md_clusters;           /* [한국어] 메타 영역 cluster 수 */
	uint64_t		max_used_cluster_mask_len; /* [한국어] grow 대비 mask 최대 길이 */
	uint32_t		i;
	struct spdk_bs_opts	opts = {};                 /* [한국어] 정규화된 옵션 사본 */
	int			rc;
	uint64_t		lba, lba_count;            /* [한국어] 데이터 영역 trim/zero 범위 */

	SPDK_DEBUGLOG(blob, "Initializing blobstore on dev %p\n", dev);
	if ((dev->phys_blocklen % dev->blocklen) != 0) {   /* [한국어] phys_blocklen이 logical의 배수여야 함 */
		SPDK_ERRLOG("unsupported dev block length of %d\n",
			    dev->blocklen);
		dev->destroy(dev);                         /* [한국어] 실패 시 backing dev 정리 */
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	spdk_bs_opts_init(&opts, sizeof(opts));            /* [한국어] 디폴트 채움 */
	if (o) {
		if (bs_opts_copy(o, &opts)) {              /* [한국어] forward-compat 복사 */
			dev->destroy(dev);
			cb_fn(cb_arg, NULL, -EINVAL);
			return;
		}
	}

	if (bs_opts_verify(&opts) != 0) {                  /* [한국어] 옵션 무결성 검증 */
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	rc = bs_alloc(dev, &opts, &bs, &ctx);              /* [한국어] bs/ctx/super 메모리 할당 */
	if (rc) {
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, rc);
		return;
	}

	/* [한국어] num_md_pages 디폴트는 cluster당 1 페이지 — 단순한 over-alloc 휴리스틱. */
	if (opts.num_md_pages == SPDK_BLOB_OPTS_NUM_MD_PAGES) {
		/* By default, allocate 1 page per cluster.
		 * Technically, this over-allocates metadata
		 * because more metadata will reduce the number
		 * of usable clusters. This can be addressed with
		 * more complex math in the future.
		 */
		bs->md_len = bs->total_clusters;
	} else {
		bs->md_len = opts.num_md_pages;
	}
	rc = spdk_bit_array_resize(&bs->used_md_pages, bs->md_len); /* [한국어] used_md 비트맵 사이즈 */
	if (rc < 0) {
		spdk_free(ctx->super);
		free(ctx);
		bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	rc = spdk_bit_array_resize(&bs->used_blobids, bs->md_len); /* [한국어] used_blobid 비트맵 사이즈 */
	if (rc < 0) {
		spdk_free(ctx->super);
		free(ctx);
		bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	rc = spdk_bit_array_resize(&bs->open_blobids, bs->md_len); /* [한국어] open 중인 blobid 추적용 */
	if (rc < 0) {
		spdk_free(ctx->super);
		free(ctx);
		bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	/* [한국어] super block 영속 필드 채움 — 이 모든 값이 디스크에 그대로 기록된다. */
	memcpy(ctx->super->signature, SPDK_BS_SUPER_BLOCK_SIG,
	       sizeof(ctx->super->signature));            /* [한국어] "SPDKBLOB" 매직 시그니처 */
	ctx->super->version = SPDK_BS_VERSION;             /* [한국어] 디스크 포맷 버전 */
	ctx->super->length = sizeof(*ctx->super);
	ctx->super->super_blob = bs->super_blob;           /* [한국어] super blob id (없으면 INVALID) */
	ctx->super->clean = 0;                             /* [한국어] 마운트 동안 dirty=0이면 unload 시 1로 */
	ctx->super->cluster_size = bs->cluster_sz;
	ctx->super->io_unit_size = bs->io_unit_size;
	ctx->super->md_page_size = bs->md_page_size;
	memcpy(&ctx->super->bstype, &bs->bstype, sizeof(bs->bstype)); /* [한국어] 사용자 식별 태그 */

	/* Calculate how many pages the metadata consumes at the front
	 * of the disk.
	 */

	/* The super block uses 1 page */
	num_md_pages = 1;                                  /* [한국어] super는 page 0 */

	/* The used_md_pages mask requires 1 bit per metadata page, rounded
	 * up to the nearest page, plus a header.
	 */
	/* [한국어] used_md mask 영역: header + ceil(md_len/8) 바이트 → page 단위 올림. */
	ctx->super->used_page_mask_start = num_md_pages;
	ctx->super->used_page_mask_len = spdk_divide_round_up(sizeof(struct spdk_bs_md_mask) +
					 spdk_divide_round_up(bs->md_len, 8),
					 ctx->super->md_page_size);
	num_md_pages += ctx->super->used_page_mask_len;

	/* The used_clusters mask requires 1 bit per cluster, rounded
	 * up to the nearest page, plus a header.
	 */
	/* [한국어] used_clusters mask: cluster당 1비트. */
	ctx->super->used_cluster_mask_start = num_md_pages;
	ctx->super->used_cluster_mask_len = spdk_divide_round_up(sizeof(struct spdk_bs_md_mask) +
					    spdk_divide_round_up(bs->total_clusters, 8),
					    ctx->super->md_page_size);
	/* The blobstore might be extended, then the used_cluster bitmap will need more space.
	 * Here we calculate the max clusters we can support according to the
	 * num_md_pages (bs->md_len).
	 */
	/* [한국어] grow 시 cluster 수가 md_len까지 늘어날 수 있다고 가정하고 mask 영역을 미리 예약. */
	max_used_cluster_mask_len = spdk_divide_round_up(sizeof(struct spdk_bs_md_mask) +
				    spdk_divide_round_up(bs->md_len, 8),
				    ctx->super->md_page_size);
	max_used_cluster_mask_len = spdk_max(max_used_cluster_mask_len,
					     ctx->super->used_cluster_mask_len);
	num_md_pages += max_used_cluster_mask_len;

	/* The used_blobids mask requires 1 bit per metadata page, rounded
	 * up to the nearest page, plus a header.
	 */
	/* [한국어] used_blobids mask: blob id 후보당 1비트(=md page 하나당). */
	ctx->super->used_blobid_mask_start = num_md_pages;
	ctx->super->used_blobid_mask_len = spdk_divide_round_up(sizeof(struct spdk_bs_md_mask) +
					   spdk_divide_round_up(bs->md_len, 8),
					   ctx->super->md_page_size);
	num_md_pages += ctx->super->used_blobid_mask_len;

	/* The metadata region size was chosen above */
	/* [한국어] 마지막으로 실제 md page 영역 (각 blob의 descriptor chain 보관). */
	ctx->super->md_start = bs->md_start = num_md_pages;
	ctx->super->md_len = bs->md_len;
	num_md_pages += bs->md_len;

	num_md_lba = bs_page_to_lba(bs, num_md_pages);     /* [한국어] page → LBA 변환 (zero/trim 범위) */

	ctx->super->size = dev->blockcnt * dev->blocklen;  /* [한국어] 전체 용량 (bytes) */

	ctx->super->crc = blob_md_page_calc_crc(ctx->super); /* [한국어] super CRC32C 계산 */

	num_md_clusters = spdk_divide_round_up(num_md_pages, bs->pages_per_cluster); /* [한국어] 메타가 차지할 cluster 수 */
	if (num_md_clusters > bs->total_clusters) {        /* [한국어] 메타가 디스크보다 크면 거부 */
		SPDK_ERRLOG("Blobstore metadata cannot use more clusters than is available, "
			    "please decrease number of pages reserved for metadata "
			    "or increase cluster size.\n");
		spdk_free(ctx->super);
		spdk_bit_array_free(&ctx->used_clusters);
		free(ctx);
		bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}
	/* Claim all of the clusters used by the metadata */
	/* [한국어] 메타 영역에 해당하는 cluster들을 처음부터 used로 마킹. */
	for (i = 0; i < num_md_clusters; i++) {
		spdk_bit_array_set(ctx->used_clusters, i);
	}

	bs->num_free_clusters -= num_md_clusters;          /* [한국어] 사용 가능 cluster 수 차감 */
	bs->total_data_clusters = bs->num_free_clusters;   /* [한국어] 데이터에 쓸 수 있는 총합 */

	cpl.type = SPDK_BS_CPL_TYPE_BS_HANDLE;             /* [한국어] 완료 시 bs 핸들을 사용자에게 전달 */
	cpl.u.bs_handle.cb_fn = cb_fn;
	cpl.u.bs_handle.cb_arg = cb_arg;
	cpl.u.bs_handle.bs = bs;

	seq = bs_sequence_start_bs(bs->md_channel, &cpl);  /* [한국어] 비동기 시퀀스 시작 */
	if (!seq) {
		spdk_free(ctx->super);
		free(ctx);
		bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	batch = bs_sequence_to_batch(seq, bs_init_trim_cpl, ctx); /* [한국어] 배치로 zero/trim 묶음 발사 */

	/* Clear metadata space */
	bs_batch_write_zeroes_dev(batch, 0, num_md_lba);   /* [한국어] 메타 영역 0으로 초기화 (확정성 보장) */

	lba = num_md_lba;                                  /* [한국어] 데이터 영역 시작 */
	lba_count = ctx->bs->dev->blockcnt - lba;          /* [한국어] 데이터 영역 길이 */
	switch (opts.clear_method) {                       /* [한국어] 사용자가 요청한 clear 정책 */
	case BS_CLEAR_WITH_UNMAP:
		/* Trim data clusters */
		bs_batch_unmap_dev(batch, lba, lba_count); /* [한국어] NVMe Deallocate / SATA TRIM */
		break;
	case BS_CLEAR_WITH_WRITE_ZEROES:
		/* Write_zeroes to data clusters */
		bs_batch_write_zeroes_dev(batch, lba, lba_count); /* [한국어] 명시적 0 쓰기 */
		break;
	case BS_CLEAR_WITH_NONE:
	default:
		break;                                     /* [한국어] 아무 것도 하지 않음 (기존 데이터 유지) */
	}

	bs_batch_close(batch);                             /* [한국어] 배치 종료 → 모두 끝나면 bs_init_trim_cpl */
}

/* END spdk_bs_init */

/* START spdk_bs_destroy */

static void
bs_destroy_trim_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	struct spdk_blob_store *bs = ctx->bs;

	free(ctx);

	if (bserrno != 0) {
		bs_sequence_finish(seq, bserrno);
		return;
	}

	/*
	 * We need to defer calling bs_call_cpl() until after
	 * dev destruction, so tuck these away for later use.
	 */
	bs->unload_err = bserrno;
	memcpy(&bs->unload_cpl, &seq->cpl, sizeof(struct spdk_bs_cpl));
	seq->cpl.type = SPDK_BS_CPL_TYPE_NONE;
	bs_sequence_finish(seq, bserrno);

	bs_free(bs);
}

/*
 * [한국어]
 * spdk_bs_destroy - blobstore를 영구 파괴 (super block 0으로 덮어쓰기) (공개 API)
 *
 * @bs:     파괴할 blobstore
 * @cb_fn:  완료 콜백
 *
 * spdk_bs_unload와 달리 영속 데이터를 보존하지 않는다 — super block을 0으로 덮어써
 * 다음 spdk_bs_load 가 실패하도록 하고 in-memory 자원을 해제한다.
 *
 * 사전 조건: 모든 blob close 필요. open이면 -EBUSY 반환.
 */
void
spdk_bs_destroy(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn,
		void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;
	struct spdk_bs_load_ctx *ctx;

	SPDK_DEBUGLOG(blob, "Destroying blobstore\n");

	if (!RB_EMPTY(&bs->open_blobs)) {                  /* [한국어] open blob이 남아 있으면 거부 */
		SPDK_ERRLOG("Blobstore still has open blobs\n");
		cb_fn(cb_arg, -EBUSY);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	cpl.u.bs_basic.cb_fn = cb_fn;
	cpl.u.bs_basic.cb_arg = cb_arg;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bs = bs;

	seq = bs_sequence_start_bs(bs->md_channel, &cpl);
	if (!seq) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* Write zeroes to the super block */
	/* [한국어] super 영역에 0 쓰기 — 시그니처가 깨져 다음 load가 실패한다.
	 * 완료 시 bs_destroy_trim_cpl이 bs_free까지 호출해 in-memory도 정리. */
	bs_sequence_write_zeroes_dev(seq,
				     bs_page_to_lba(bs, 0),
				     bs_byte_to_lba(bs, sizeof(struct spdk_bs_super_block)),
				     bs_destroy_trim_cpl, ctx);
}

/* END spdk_bs_destroy */

/* START spdk_bs_unload */

static void
bs_unload_finish(struct spdk_bs_load_ctx *ctx, int bserrno)
{
	spdk_bs_sequence_t *seq = ctx->seq;
	struct spdk_blob_store *bs = ctx->bs;

	spdk_free(ctx->super);
	free(ctx);

	/*
	 * Exception for EIO is made for hot-remove cases where the underlying
	 * block device is no longer available.
	 */
	if (bserrno != 0 && bserrno != -EIO) {
		bs_sequence_finish(seq, bserrno);
		return;
	}

	/*
	 * We need to defer calling bs_call_cpl() until after
	 * dev destruction, so tuck these away for later use.
	 */
	bs->unload_err = bserrno;
	memcpy(&bs->unload_cpl, &seq->cpl, sizeof(struct spdk_bs_cpl));
	seq->cpl.type = SPDK_BS_CPL_TYPE_NONE;
	bs_sequence_finish(seq, bserrno);

	bs_free(bs);
}

static void
bs_unload_write_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	bs_unload_finish(ctx, bserrno);
}

static void
bs_unload_write_used_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_free(ctx->mask);
	ctx->mask = NULL;

	if (bserrno != 0) {
		bs_unload_finish(ctx, bserrno);
		return;
	}

	ctx->super->clean = 1;

	bs_write_super(seq, ctx->bs, ctx->super, bs_unload_write_super_cpl, ctx);
}

static void
bs_unload_write_used_blobids_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_free(ctx->mask);
	ctx->mask = NULL;

	if (bserrno != 0) {
		bs_unload_finish(ctx, bserrno);
		return;
	}

	bs_write_used_clusters(seq, ctx, bs_unload_write_used_clusters_cpl);
}

static void
bs_unload_write_used_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_free(ctx->mask);
	ctx->mask = NULL;

	if (bserrno != 0) {
		bs_unload_finish(ctx, bserrno);
		return;
	}

	bs_write_used_blobids(seq, ctx, bs_unload_write_used_blobids_cpl);
}

static void
bs_unload_read_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;
	int rc;

	if (bserrno != 0) {
		bs_unload_finish(ctx, bserrno);
		return;
	}

	rc = bs_super_validate(ctx->super, ctx->bs);
	if (rc != 0) {
		bs_unload_finish(ctx, rc);
		return;
	}

	bs_write_used_md(seq, cb_arg, bs_unload_write_used_pages_cpl);
}

/*
 * [한국어]
 * spdk_bs_unload - blobstore를 깨끗하게 영속화하고 메모리 자원 해제 (공개 API)
 *
 * @bs:     unload할 blobstore (호출 후에는 사용 금지)
 * @cb_fn:  완료 콜백 (cb_arg, bserrno)
 *
 * 사전 조건:
 * - 모든 blob이 close된 상태여야 함 (open_blobs RB tree empty). 안 그러면 -EBUSY.
 * - esnap 채널 정리가 진행 중이면 정리 완료 후 자동 재시도하도록 콜백을 저장하고 반환.
 *
 * 동작 단계:
 * 1) ctx + DMA-가능 super 버퍼 할당
 * 2) bs_sequence_start_bs로 시퀀스 시작
 * 3) super를 다시 read (bs_unload_read_super_cpl)
 * 4) bs_super_validate 후 비트맵 영속화 체인:
 *    used_md → used_blobids → used_clusters → super.clean=1 재기록 → bs_free
 *
 * Esnap 처리: bs->esnap_channels_unloading > 0 이면 채널 파괴가 끝날 때까지 대기.
 * 그 사이에 두 번 호출되면 -EBUSY로 거부.
 */
void
spdk_bs_unload(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	struct spdk_bs_load_ctx *ctx;

	SPDK_DEBUGLOG(blob, "Syncing blobstore\n");

	/*
	 * If external snapshot channels are being destroyed while the blobstore is unloaded, the
	 * unload is deferred until after the channel destruction completes.
	 */
	if (bs->esnap_channels_unloading != 0) {           /* [한국어] esnap 채널 정리 중 — unload 지연 */
		if (bs->esnap_unload_cb_fn != NULL) {      /* [한국어] 이미 한 번 deferred 됐으면 거부 */
			SPDK_ERRLOG("Blobstore unload in progress\n");
			cb_fn(cb_arg, -EBUSY);
			return;
		}
		SPDK_DEBUGLOG(blob_esnap, "Blobstore unload deferred: %" PRIu32
			      " esnap clones are unloading\n", bs->esnap_channels_unloading);
		bs->esnap_unload_cb_fn = cb_fn;            /* [한국어] 정리 완료 시 자동 호출되도록 보관 */
		bs->esnap_unload_cb_arg = cb_arg;
		return;
	}
	if (bs->esnap_unload_cb_fn != NULL) {              /* [한국어] deferred 호출이 다시 진입한 경우 정리 */
		SPDK_DEBUGLOG(blob_esnap, "Blobstore deferred unload progressing\n");
		assert(bs->esnap_unload_cb_fn == cb_fn);
		assert(bs->esnap_unload_cb_arg == cb_arg);
		bs->esnap_unload_cb_fn = NULL;
		bs->esnap_unload_cb_arg = NULL;
	}

	if (!RB_EMPTY(&bs->open_blobs)) {                  /* [한국어] 열린 blob 있으면 거부 — 사용자 정리 의무 */
		SPDK_ERRLOG("Blobstore still has open blobs\n");
		cb_fn(cb_arg, -EBUSY);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));                     /* [한국어] 콜백 체인 컨텍스트 */
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bs = bs;

	/* [한국어] super 페이지 — DMA 가능 메모리(zmalloc with 4KB align). */
	ctx->super = spdk_zmalloc(sizeof(*ctx->super), 0x1000, NULL,
				  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->super) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;              /* [한국어] basic = 단순 errno만 반환 */
	cpl.u.bs_basic.cb_fn = cb_fn;
	cpl.u.bs_basic.cb_arg = cb_arg;

	ctx->seq = bs_sequence_start_bs(bs->md_channel, &cpl);
	if (!ctx->seq) {
		spdk_free(ctx->super);
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* Read super block */
	/* [한국어] 현재 디스크 super를 읽어 와 검증 후 비트맵들을 차례로 기록한다. */
	bs_sequence_read_dev(ctx->seq, ctx->super, bs_page_to_lba(bs, 0),
			     bs_byte_to_lba(bs, sizeof(*ctx->super)),
			     bs_unload_read_super_cpl, ctx);
}

/* END spdk_bs_unload */

/* START spdk_bs_set_super */

struct spdk_bs_set_super_ctx {
	struct spdk_blob_store		*bs;
	struct spdk_bs_super_block	*super;
};

static void
bs_set_super_write_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_set_super_ctx	*ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("Unable to write to super block of blobstore\n");
	}

	spdk_free(ctx->super);

	bs_sequence_finish(seq, bserrno);

	free(ctx);
}

static void
bs_set_super_read_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_set_super_ctx	*ctx = cb_arg;
	int rc;

	if (bserrno != 0) {
		SPDK_ERRLOG("Unable to read super block of blobstore\n");
		spdk_free(ctx->super);
		bs_sequence_finish(seq, bserrno);
		free(ctx);
		return;
	}

	rc = bs_super_validate(ctx->super, ctx->bs);
	if (rc != 0) {
		SPDK_ERRLOG("Not a valid super block\n");
		spdk_free(ctx->super);
		bs_sequence_finish(seq, rc);
		free(ctx);
		return;
	}

	bs_write_super(seq, ctx->bs, ctx->super, bs_set_super_write_cpl, ctx);
}

/*
 * [한국어]
 * spdk_bs_set_super - 사용자 진입점 blob을 super_blob으로 등록 (공개 API)
 *
 * @blobid: 진입점이 될 blob id (이미 존재해야 함)
 *
 * 디스크 super block에 super_blob 필드를 갱신해 다음 load 시에도 보존된다.
 * 동작: super read → in-memory super 갱신 → super write → 콜백.
 */
void
spdk_bs_set_super(struct spdk_blob_store *bs, spdk_blob_id blobid,
		  spdk_bs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl		cpl;
	spdk_bs_sequence_t		*seq;
	struct spdk_bs_set_super_ctx	*ctx;

	SPDK_DEBUGLOG(blob, "Setting super blob id on blobstore\n");

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bs = bs;

	ctx->super = spdk_zmalloc(sizeof(*ctx->super), 0x1000, NULL,
				  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->super) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	cpl.u.bs_basic.cb_fn = cb_fn;
	cpl.u.bs_basic.cb_arg = cb_arg;

	seq = bs_sequence_start_bs(bs->md_channel, &cpl);
	if (!seq) {
		spdk_free(ctx->super);
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	bs->super_blob = blobid;

	/* Read super block */
	bs_sequence_read_dev(seq, ctx->super, bs_page_to_lba(bs, 0),
			     bs_byte_to_lba(bs, sizeof(*ctx->super)),
			     bs_set_super_read_cpl, ctx);
}

/* END spdk_bs_set_super */

/*
 * [한국어]
 * spdk_bs_get_super - 등록된 super blob의 id를 즉시 반환 (공개 API)
 *
 * super blob은 사용자가 "이 blobstore의 진입점"으로 지정해 둔 특별한 blob.
 * spdk_bs_set_super로 미리 등록되지 않았으면 -ENOENT.
 */
void
spdk_bs_get_super(struct spdk_blob_store *bs,
		  spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	if (bs->super_blob == SPDK_BLOBID_INVALID) {
		cb_fn(cb_arg, SPDK_BLOBID_INVALID, -ENOENT);
	} else {
		cb_fn(cb_arg, bs->super_blob, 0);
	}
}

/* [한국어] cluster 크기 (bytes) — 동기 단순 getter. */
uint64_t
spdk_bs_get_cluster_size(struct spdk_blob_store *bs)
{
	return bs->cluster_sz;
}

/* [한국어] md page 크기 (bytes) — 보통 4 KiB. */
uint64_t
spdk_bs_get_page_size(struct spdk_blob_store *bs)
{
	return bs->md_page_size;
}

/*
 * [한국어]
 * spdk_bs_get_max_growable_size - grow 시 도달 가능한 최대 크기(bytes) 계산 (공개 API)
 *
 * spdk_bs_init 시점에 used_cluster_mask 영역을 max_used_cluster_mask_len 만큼 미리
 * 예약했으므로, 그 mask가 표현 가능한 cluster 수의 상한을 역산해 돌려준다.
 */
uint64_t
spdk_bs_get_max_growable_size(struct spdk_blob_store *bs)
{
	uint64_t max_used_cluster_mask, max_number_of_clusters;

	/* Calculate maximum number of pages reserved for used_cluster_mask,
	 * This is immutable.
	 */
	max_used_cluster_mask = spdk_divide_round_up(sizeof(struct spdk_bs_md_mask) +
				spdk_divide_round_up(bs->md_len, 8),
				spdk_bs_get_page_size(bs));
	/* In used_cluster_mask, It takes 1 bit to track a cluster. */
	max_number_of_clusters = ((max_used_cluster_mask * spdk_bs_get_page_size(bs))
				  - sizeof(struct spdk_bs_md_mask)) * 8;

	return max_number_of_clusters * bs->cluster_sz;
}

/* [한국어] io_unit 크기 (bytes) — 가장 작은 IO 정렬 단위. */
uint64_t
spdk_bs_get_io_unit_size(struct spdk_blob_store *bs)
{
	return bs->io_unit_size;
}

/* [한국어] 현재 free cluster 수 (실시간). 사용자 IO와 race 가능 — 단순 통계 용도. */
uint64_t
spdk_bs_free_cluster_count(struct spdk_blob_store *bs)
{
	return bs->num_free_clusters;
}

/* [한국어] 데이터에 사용 가능한 총 cluster 수 (메타 영역 제외). */
uint64_t
spdk_bs_total_data_cluster_count(struct spdk_blob_store *bs)
{
	return bs->total_data_clusters;
}

/*
 * [한국어]
 * bs_register_md_thread - 메타 IO 전용 채널을 잡고 md_thread 사용 준비 완료 (static)
 *
 * spdk_bs_init/load의 콜백 체인 후반에 호출되어 bs->md_channel을 채운다.
 * 채널은 thread-local이라 호출 시점의 thread가 곧 md_thread가 된다.
 */
static int
bs_register_md_thread(struct spdk_blob_store *bs)
{
	bs->md_channel = spdk_get_io_channel(bs);
	if (!bs->md_channel) {
		SPDK_ERRLOG("Failed to get IO channel.\n");
		return -1;
	}

	return 0;
}

/* [한국어] bs_register_md_thread의 역동작 — md 채널 ref 감소. */
static int
bs_unregister_md_thread(struct spdk_blob_store *bs)
{
	spdk_put_io_channel(bs->md_channel);

	return 0;
}

/* [한국어] blob의 id 반환 — 단순 getter (lockless 안전). */
spdk_blob_id
spdk_blob_get_id(struct spdk_blob *blob)
{
	assert(blob != NULL);

	return blob->id;
}

/*
 * [한국어]
 * spdk_blob_get_num_io_units - blob의 논리 크기를 io_unit 단위로 반환 (공개 API)
 *
 * 사용자에게 "이 blob의 size를 io_unit 단위로" 알리는 함수.
 * thin blob에서도 active.num_clusters 기반 — 실제 할당량이 아니라 가상 크기.
 */
uint64_t
spdk_blob_get_num_io_units(struct spdk_blob *blob)
{
	assert(blob != NULL);

	return bs_cluster_to_io_unit(blob->bs, blob->active.num_clusters);
}

/* [한국어] blob의 가상 cluster 수 (resize 가능). */
uint64_t
spdk_blob_get_num_clusters(struct spdk_blob *blob)
{
	assert(blob != NULL);

	return blob->active.num_clusters;
}

/* [한국어] thin blob에서 실제로 cluster가 할당된 수 (가상 num_clusters와 다름). */
uint64_t
spdk_blob_get_num_allocated_clusters(struct spdk_blob *blob)
{
	assert(blob != NULL);

	return blob->active.num_allocated_clusters;
}

/*
 * [한국어]
 * blob_find_io_unit - offset 이후 처음으로 is_allocated 상태가 매칭되는 io_unit 찾기 (static)
 *
 * thin blob의 cluster 단위로 점프하며 검색 — 할당/미할당은 cluster 단위로 결정되므로
 * cluster boundary 단위로 건너뛰면 충분하다.
 */
static uint64_t
blob_find_io_unit(struct spdk_blob *blob, uint64_t offset, bool is_allocated)
{
	uint64_t blob_io_unit_num = spdk_blob_get_num_io_units(blob);

	while (offset < blob_io_unit_num) {
		if (bs_io_unit_is_allocated(blob, offset) == is_allocated) {
			return offset;
		}

		offset += bs_num_io_units_to_cluster_boundary(blob, offset); /* [한국어] 다음 cluster 경계로 점프 */
	}

	return UINT64_MAX;                                 /* [한국어] 끝까지 못 찾음 */
}

/*
 * [한국어]
 * spdk_blob_get_next_allocated_io_unit - thin blob에서 다음 할당된 io_unit 찾기 (공개 API)
 *
 * shallow_copy/dump 등에서 "건너뛸 수 있는 hole"을 빠르게 탐색할 때 사용.
 */
uint64_t
spdk_blob_get_next_allocated_io_unit(struct spdk_blob *blob, uint64_t offset)
{
	return blob_find_io_unit(blob, offset, true);
}

/*
 * [한국어]
 * spdk_blob_get_next_unallocated_io_unit - 다음 미할당 io_unit 찾기 (공개 API)
 */
uint64_t
spdk_blob_get_next_unallocated_io_unit(struct spdk_blob *blob, uint64_t offset)
{
	return blob_find_io_unit(blob, offset, false);
}

/* START spdk_bs_create_blob */

static void
bs_create_blob_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;
	uint32_t page_idx = bs_blobid_to_page(blob->id);

	if (bserrno != 0) {
		spdk_spin_lock(&blob->bs->used_lock);
		spdk_bit_array_clear(blob->bs->used_blobids, page_idx);
		bs_release_md_page(blob->bs, page_idx);
		spdk_spin_unlock(&blob->bs->used_lock);
	}

	blob_free(blob);

	bs_sequence_finish(seq, bserrno);
}

static int
blob_set_xattrs(struct spdk_blob *blob, const struct spdk_blob_xattr_opts *xattrs,
		bool internal)
{
	uint64_t i;
	size_t value_len = 0;
	int rc;
	const void *value = NULL;
	if (xattrs->count > 0 && xattrs->get_value == NULL) {
		return -EINVAL;
	}
	for (i = 0; i < xattrs->count; i++) {
		xattrs->get_value(xattrs->ctx, xattrs->names[i], &value, &value_len);
		if (value == NULL || value_len == 0) {
			return -EINVAL;
		}
		rc = blob_set_xattr(blob, xattrs->names[i], value, value_len, internal);
		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}

static void
blob_opts_copy(const struct spdk_blob_opts *src, struct spdk_blob_opts *dst)
{
#define FIELD_OK(field) \
        offsetof(struct spdk_blob_opts, field) + sizeof(src->field) <= src->opts_size

#define SET_FIELD(field) \
        if (FIELD_OK(field)) { \
                dst->field = src->field; \
        } \

	SET_FIELD(num_clusters);
	SET_FIELD(thin_provision);
	SET_FIELD(clear_method);

	if (FIELD_OK(xattrs)) {
		memcpy(&dst->xattrs, &src->xattrs, sizeof(src->xattrs));
	}

	SET_FIELD(use_extent_table);
	SET_FIELD(esnap_id);
	SET_FIELD(esnap_id_len);

	dst->opts_size = src->opts_size;

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_blob_opts) == 80, "Incorrect size");

#undef FIELD_OK
#undef SET_FIELD
}

static void
bs_create_blob(struct spdk_blob_store *bs,
	       const struct spdk_blob_opts *opts,
	       const struct spdk_blob_xattr_opts *internal_xattrs,
	       spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	struct spdk_blob	*blob;
	uint32_t		page_idx;
	struct spdk_bs_cpl	cpl;
	struct spdk_blob_opts	opts_local;
	struct spdk_blob_xattr_opts internal_xattrs_default;
	spdk_bs_sequence_t	*seq;
	spdk_blob_id		id;
	int rc;

	assert(spdk_get_thread() == bs->md_thread);

	spdk_spin_lock(&bs->used_lock);
	page_idx = spdk_bit_array_find_first_clear(bs->used_md_pages, 0);
	if (page_idx == UINT32_MAX) {
		spdk_spin_unlock(&bs->used_lock);
		cb_fn(cb_arg, 0, -ENOMEM);
		return;
	}
	spdk_bit_array_set(bs->used_blobids, page_idx);
	bs_claim_md_page(bs, page_idx);
	spdk_spin_unlock(&bs->used_lock);

	id = bs_page_to_blobid(page_idx);

	SPDK_DEBUGLOG(blob, "Creating blob with id 0x%" PRIx64 " at page %u\n", id, page_idx);

	spdk_blob_opts_init(&opts_local, sizeof(opts_local));
	if (opts) {
		blob_opts_copy(opts, &opts_local);
	}

	blob = blob_alloc(bs, id);
	if (!blob) {
		rc = -ENOMEM;
		goto error;
	}

	blob->use_extent_table = opts_local.use_extent_table;
	if (blob->use_extent_table) {
		blob->invalid_flags |= SPDK_BLOB_EXTENT_TABLE;
	}

	if (!internal_xattrs) {
		blob_xattrs_init(&internal_xattrs_default);
		internal_xattrs = &internal_xattrs_default;
	}

	rc = blob_set_xattrs(blob, &opts_local.xattrs, false);
	if (rc < 0) {
		goto error;
	}

	rc = blob_set_xattrs(blob, internal_xattrs, true);
	if (rc < 0) {
		goto error;
	}

	if (opts_local.thin_provision) {
		blob_set_thin_provision(blob);
	}

	blob_set_clear_method(blob, opts_local.clear_method);

	if (opts_local.esnap_id != NULL) {
		if (opts_local.esnap_id_len > UINT16_MAX) {
			SPDK_ERRLOG("esnap id length %" PRIu64 "is too long\n",
				    opts_local.esnap_id_len);
			rc = -EINVAL;
			goto error;

		}
		blob_set_thin_provision(blob);
		blob->invalid_flags |= SPDK_BLOB_EXTERNAL_SNAPSHOT;
		rc = blob_set_xattr(blob, BLOB_EXTERNAL_SNAPSHOT_ID,
				    opts_local.esnap_id, opts_local.esnap_id_len, true);
		if (rc != 0) {
			goto error;
		}
	}

	rc = blob_resize(blob, opts_local.num_clusters);
	if (rc < 0) {
		goto error;
	}
	cpl.type = SPDK_BS_CPL_TYPE_BLOBID;
	cpl.u.blobid.cb_fn = cb_fn;
	cpl.u.blobid.cb_arg = cb_arg;
	cpl.u.blobid.blobid = blob->id;

	seq = bs_sequence_start_bs(bs->md_channel, &cpl);
	if (!seq) {
		rc = -ENOMEM;
		goto error;
	}

	blob_persist(seq, blob, bs_create_blob_cpl, blob);
	return;

error:
	SPDK_ERRLOG("Failed to create blob: %s, size in clusters/size: %lu (clusters)\n",
		    spdk_strerror(rc), opts_local.num_clusters);
	if (blob != NULL) {
		blob_free(blob);
	}
	spdk_spin_lock(&bs->used_lock);
	spdk_bit_array_clear(bs->used_blobids, page_idx);
	bs_release_md_page(bs, page_idx);
	spdk_spin_unlock(&bs->used_lock);
	cb_fn(cb_arg, 0, rc);
}

/*
 * [한국어]
 * spdk_bs_create_blob - 옵션 없이 빈 blob 생성 (공개 API)
 *
 * 디폴트 옵션(0 cluster, thick=false, use_extent_table=true)으로 새 blob을 만든다.
 * 비동기 콜백으로 spdk_blob_id를 돌려준다.
 */
void
spdk_bs_create_blob(struct spdk_blob_store *bs,
		    spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	bs_create_blob(bs, NULL, NULL, cb_fn, cb_arg);     /* [한국어] opts/xattrs NULL → bs_create_blob 디폴트 사용 */
}

/*
 * [한국어]
 * spdk_bs_create_blob_ext - 사용자 옵션과 함께 blob 생성 (공개 API)
 *
 * @opts: num_clusters, thin_provision, clear_method, xattrs 등 사용자 정의 옵션
 *
 * 내부 xattr는 사용하지 않는 외부 호출자 전용 진입점. snapshot/clone 같은 SPDK 내부
 * 흐름은 bs_create_blob을 internal_xattrs와 함께 직접 호출한다.
 */
void
spdk_bs_create_blob_ext(struct spdk_blob_store *bs, const struct spdk_blob_opts *opts,
			spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	bs_create_blob(bs, opts, NULL, cb_fn, cb_arg);     /* [한국어] internal_xattrs는 NULL */
}

/* END spdk_bs_create_blob */

/* START blob_cleanup */

struct spdk_clone_snapshot_ctx {
	struct spdk_bs_cpl      cpl;
	int bserrno;
	bool frozen;

	struct spdk_io_channel *channel;

	/* Current cluster for inflate operation */
	uint64_t cluster;

	/* For inflation force allocation of all unallocated clusters and remove
	 * thin-provisioning. Otherwise only decouple parent and keep clone thin. */
	bool allocate_all;

	struct {
		spdk_blob_id id;
		struct spdk_blob *blob;
		bool md_ro;
	} original;
	struct {
		spdk_blob_id id;
		struct spdk_blob *blob;
	} new;

	/* xattrs specified for snapshot/clones only. They have no impact on
	 * the original blobs xattrs. */
	const struct spdk_blob_xattr_opts *xattrs;
};

static void
bs_clone_snapshot_cleanup_finish(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = cb_arg;
	struct spdk_bs_cpl *cpl = &ctx->cpl;

	if (bserrno != 0) {
		if (ctx->bserrno != 0) {
			SPDK_ERRLOG("Cleanup error %d\n", bserrno);
		} else {
			ctx->bserrno = bserrno;
		}
	}

	switch (cpl->type) {
	case SPDK_BS_CPL_TYPE_BLOBID:
		cpl->u.blobid.cb_fn(cpl->u.blobid.cb_arg, cpl->u.blobid.blobid, ctx->bserrno);
		break;
	case SPDK_BS_CPL_TYPE_BLOB_BASIC:
		cpl->u.blob_basic.cb_fn(cpl->u.blob_basic.cb_arg, ctx->bserrno);
		break;
	default:
		SPDK_UNREACHABLE();
		break;
	}

	free(ctx);
}

static void
bs_snapshot_unfreeze_cpl(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;

	if (bserrno != 0) {
		if (ctx->bserrno != 0) {
			SPDK_ERRLOG("Unfreeze error %d\n", bserrno);
		} else {
			ctx->bserrno = bserrno;
		}
	}

	ctx->original.id = origblob->id;
	origblob->locked_operation_in_progress = false;

	/* Revert md_ro to original state */
	origblob->md_ro = ctx->original.md_ro;

	spdk_blob_close(origblob, bs_clone_snapshot_cleanup_finish, ctx);
}

static void
bs_clone_snapshot_origblob_cleanup(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;

	if (bserrno != 0) {
		if (ctx->bserrno != 0) {
			SPDK_ERRLOG("Cleanup error %d\n", bserrno);
		} else {
			ctx->bserrno = bserrno;
		}
	}

	if (ctx->frozen) {
		/* Unfreeze any outstanding I/O */
		blob_unfreeze_io(origblob, bs_snapshot_unfreeze_cpl, ctx);
	} else {
		bs_snapshot_unfreeze_cpl(ctx, 0);
	}

}

static void
bs_clone_snapshot_newblob_cleanup(struct spdk_clone_snapshot_ctx *ctx, int bserrno)
{
	struct spdk_blob *newblob = ctx->new.blob;

	if (bserrno != 0) {
		if (ctx->bserrno != 0) {
			SPDK_ERRLOG("Cleanup error %d\n", bserrno);
		} else {
			ctx->bserrno = bserrno;
		}
	}

	ctx->new.id = newblob->id;
	spdk_blob_close(newblob, bs_clone_snapshot_origblob_cleanup, ctx);
}

/* END blob_cleanup */

/* START spdk_bs_create_snapshot */

static void
bs_snapshot_swap_cluster_maps(struct spdk_blob *blob1, struct spdk_blob *blob2)
{
	uint64_t *cluster_temp;
	uint64_t num_allocated_clusters_temp;
	uint32_t *extent_page_temp;

	cluster_temp = blob1->active.clusters;
	blob1->active.clusters = blob2->active.clusters;
	blob2->active.clusters = cluster_temp;

	num_allocated_clusters_temp = blob1->active.num_allocated_clusters;
	blob1->active.num_allocated_clusters = blob2->active.num_allocated_clusters;
	blob2->active.num_allocated_clusters = num_allocated_clusters_temp;

	extent_page_temp = blob1->active.extent_pages;
	blob1->active.extent_pages = blob2->active.extent_pages;
	blob2->active.extent_pages = extent_page_temp;
}

/* Copies an internal xattr */
static int
bs_snapshot_copy_xattr(struct spdk_blob *toblob, struct spdk_blob *fromblob, const char *name)
{
	const void	*val = NULL;
	size_t		len;
	int		bserrno;

	bserrno = blob_get_xattr_value(fromblob, name, &val, &len, true);
	if (bserrno != 0) {
		SPDK_ERRLOG("blob 0x%" PRIx64 " missing %s XATTR\n", fromblob->id, name);
		return bserrno;
	}

	bserrno = blob_set_xattr(toblob, name, val, len, true);
	if (bserrno != 0) {
		SPDK_ERRLOG("could not set %s XATTR on blob 0x%" PRIx64 "\n",
			    name, toblob->id);
		return bserrno;
	}
	return 0;
}

static void
bs_snapshot_origblob_sync_cpl(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;
	struct spdk_blob *newblob = ctx->new.blob;

	if (bserrno != 0) {
		bs_snapshot_swap_cluster_maps(newblob, origblob);
		if (blob_is_esnap_clone(newblob)) {
			bs_snapshot_copy_xattr(origblob, newblob, BLOB_EXTERNAL_SNAPSHOT_ID);
			origblob->invalid_flags |= SPDK_BLOB_EXTERNAL_SNAPSHOT;
		}
		bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	/* Remove metadata descriptor SNAPSHOT_IN_PROGRESS */
	bserrno = blob_remove_xattr(newblob, SNAPSHOT_IN_PROGRESS, true);
	if (bserrno != 0) {
		bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	bs_blob_list_add(ctx->original.blob);

	spdk_blob_set_read_only(newblob);

	/* sync snapshot metadata */
	spdk_blob_sync_md(newblob, bs_clone_snapshot_origblob_cleanup, ctx);
}

static void
bs_snapshot_newblob_sync_cpl(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;
	struct spdk_blob *newblob = ctx->new.blob;

	if (bserrno != 0) {
		/* return cluster map back to original */
		bs_snapshot_swap_cluster_maps(newblob, origblob);

		/* Newblob md sync failed. Valid clusters are only present in origblob.
		 * Since I/O is frozen on origblob, not changes to zeroed out cluster map should have occurred.
		 * Newblob needs to be reverted to thin_provisioned state at creation to properly close. */
		blob_set_thin_provision(newblob);
		assert(spdk_mem_all_zero(newblob->active.clusters,
					 newblob->active.num_clusters * sizeof(*newblob->active.clusters)));
		assert(spdk_mem_all_zero(newblob->active.extent_pages,
					 newblob->active.num_extent_pages * sizeof(*newblob->active.extent_pages)));

		bs_clone_snapshot_newblob_cleanup(ctx, bserrno);
		return;
	}

	/* Set internal xattr for snapshot id */
	bserrno = blob_set_xattr(origblob, BLOB_SNAPSHOT, &newblob->id, sizeof(spdk_blob_id), true);
	if (bserrno != 0) {
		/* return cluster map back to original */
		bs_snapshot_swap_cluster_maps(newblob, origblob);
		blob_set_thin_provision(newblob);
		bs_clone_snapshot_newblob_cleanup(ctx, bserrno);
		return;
	}

	/* Create new back_bs_dev for snapshot */
	origblob->back_bs_dev = bs_create_blob_bs_dev(newblob);
	if (origblob->back_bs_dev == NULL) {
		/* return cluster map back to original */
		bs_snapshot_swap_cluster_maps(newblob, origblob);
		blob_set_thin_provision(newblob);
		bs_clone_snapshot_newblob_cleanup(ctx, -EINVAL);
		return;
	}

	/* Remove the xattr that references an external snapshot */
	if (blob_is_esnap_clone(origblob)) {
		origblob->invalid_flags &= ~SPDK_BLOB_EXTERNAL_SNAPSHOT;
		bserrno = blob_remove_xattr(origblob, BLOB_EXTERNAL_SNAPSHOT_ID, true);
		if (bserrno != 0) {
			if (bserrno == -ENOENT) {
				SPDK_ERRLOG("blob 0x%" PRIx64 " has no " BLOB_EXTERNAL_SNAPSHOT_ID
					    " xattr to remove\n", origblob->id);
				assert(false);
			} else {
				/* return cluster map back to original */
				bs_snapshot_swap_cluster_maps(newblob, origblob);
				blob_set_thin_provision(newblob);
				bs_clone_snapshot_newblob_cleanup(ctx, bserrno);
				return;
			}
		}
	}

	bs_blob_list_remove(origblob);
	origblob->parent_id = newblob->id;
	/* set clone blob as thin provisioned */
	blob_set_thin_provision(origblob);

	bs_blob_list_add(newblob);

	/* sync clone metadata */
	spdk_blob_sync_md(origblob, bs_snapshot_origblob_sync_cpl, ctx);
}

static void
bs_snapshot_freeze_cpl(void *cb_arg, int rc)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;
	struct spdk_blob *newblob = ctx->new.blob;
	int bserrno;

	if (rc != 0) {
		bs_clone_snapshot_newblob_cleanup(ctx, rc);
		return;
	}

	ctx->frozen = true;

	if (blob_is_esnap_clone(origblob)) {
		/* Clean up any channels associated with the original blob id because future IO will
		 * perform IO using the snapshot blob_id.
		 */
		blob_esnap_destroy_bs_dev_channels(origblob, false, NULL, NULL);
	}
	if (newblob->back_bs_dev) {
		blob_back_bs_destroy(newblob);
	}
	/* set new back_bs_dev for snapshot */
	newblob->back_bs_dev = origblob->back_bs_dev;
	/* Set invalid flags from origblob */
	newblob->invalid_flags = origblob->invalid_flags;

	/* inherit parent from original blob if set */
	newblob->parent_id = origblob->parent_id;
	switch (origblob->parent_id) {
	case SPDK_BLOBID_EXTERNAL_SNAPSHOT:
		bserrno = bs_snapshot_copy_xattr(newblob, origblob, BLOB_EXTERNAL_SNAPSHOT_ID);
		if (bserrno != 0) {
			bs_clone_snapshot_newblob_cleanup(ctx, bserrno);
			return;
		}
		break;
	case SPDK_BLOBID_INVALID:
		break;
	default:
		/* Set internal xattr for snapshot id */
		bserrno = blob_set_xattr(newblob, BLOB_SNAPSHOT,
					 &origblob->parent_id, sizeof(spdk_blob_id), true);
		if (bserrno != 0) {
			bs_clone_snapshot_newblob_cleanup(ctx, bserrno);
			return;
		}
	}

	/* swap cluster maps */
	bs_snapshot_swap_cluster_maps(newblob, origblob);

	/* Set the clear method on the new blob to match the original. */
	blob_set_clear_method(newblob, origblob->clear_method);

	/* sync snapshot metadata */
	spdk_blob_sync_md(newblob, bs_snapshot_newblob_sync_cpl, ctx);
}

static void
bs_snapshot_newblob_open_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;
	struct spdk_blob *newblob = _blob;

	if (bserrno != 0) {
		bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	ctx->new.blob = newblob;
	assert(spdk_blob_is_thin_provisioned(newblob));
	assert(spdk_mem_all_zero(newblob->active.clusters,
				 newblob->active.num_clusters * sizeof(*newblob->active.clusters)));
	assert(spdk_mem_all_zero(newblob->active.extent_pages,
				 newblob->active.num_extent_pages * sizeof(*newblob->active.extent_pages)));

	blob_freeze_io(origblob, bs_snapshot_freeze_cpl, ctx);
}

static void
bs_snapshot_newblob_create_cpl(void *cb_arg, spdk_blob_id blobid, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;

	if (bserrno != 0) {
		bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	ctx->new.id = blobid;
	ctx->cpl.u.blobid.blobid = blobid;

	spdk_bs_open_blob(origblob->bs, ctx->new.id, bs_snapshot_newblob_open_cpl, ctx);
}


static void
bs_xattr_snapshot(void *arg, const char *name,
		  const void **value, size_t *value_len)
{
	assert(strncmp(name, SNAPSHOT_IN_PROGRESS, sizeof(SNAPSHOT_IN_PROGRESS)) == 0);

	struct spdk_blob *blob = (struct spdk_blob *)arg;
	*value = &blob->id;
	*value_len = sizeof(blob->id);
}

static void
bs_snapshot_origblob_open_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob_opts opts;
	struct spdk_blob_xattr_opts internal_xattrs;
	char *xattrs_names[] = { SNAPSHOT_IN_PROGRESS };

	if (bserrno != 0) {
		bs_clone_snapshot_cleanup_finish(ctx, bserrno);
		return;
	}

	ctx->original.blob = _blob;

	if (_blob->data_ro || _blob->md_ro) {
		SPDK_DEBUGLOG(blob, "Cannot create snapshot from read only blob with id 0x%"
			      PRIx64 "\n", _blob->id);
		ctx->bserrno = -EINVAL;
		spdk_blob_close(_blob, bs_clone_snapshot_cleanup_finish, ctx);
		return;
	}

	if (_blob->locked_operation_in_progress) {
		SPDK_DEBUGLOG(blob, "Cannot create snapshot - another operation in progress\n");
		ctx->bserrno = -EBUSY;
		spdk_blob_close(_blob, bs_clone_snapshot_cleanup_finish, ctx);
		return;
	}

	_blob->locked_operation_in_progress = true;

	spdk_blob_opts_init(&opts, sizeof(opts));
	blob_xattrs_init(&internal_xattrs);

	/* Change the size of new blob to the same as in original blob,
	 * but do not allocate clusters */
	opts.thin_provision = true;
	opts.num_clusters = spdk_blob_get_num_clusters(_blob);
	opts.use_extent_table = _blob->use_extent_table;

	/* If there are any xattrs specified for snapshot, set them now */
	if (ctx->xattrs) {
		memcpy(&opts.xattrs, ctx->xattrs, sizeof(*ctx->xattrs));
	}
	/* Set internal xattr SNAPSHOT_IN_PROGRESS */
	internal_xattrs.count = 1;
	internal_xattrs.ctx = _blob;
	internal_xattrs.names = xattrs_names;
	internal_xattrs.get_value = bs_xattr_snapshot;

	bs_create_blob(_blob->bs, &opts, &internal_xattrs,
		       bs_snapshot_newblob_create_cpl, ctx);
}

void
/*
 * [한국어]
 * spdk_bs_create_snapshot - 기존 blob의 read-only 스냅샷을 생성 (공개 API)
 *
 * @bs:               blobstore
 * @blobid:           스냅샷의 원본이 될 blob (data_ro/md_ro=false 여야 함)
 * @snapshot_xattrs:  새 스냅샷에 부여할 사용자 xattr (옵션)
 * @cb_fn:            완료 콜백 (cb_arg, new_snap_blobid, bserrno)
 *
 * 흐름 (단순화):
 * 1) origblob open → bs_snapshot_origblob_open_cpl
 * 2) 임시 SNAPSHOT_IN_PROGRESS xattr 부여 → 새 thin blob(snapshot) 생성
 * 3) origblob의 IO를 freeze
 * 4) cluster 매핑을 swap → origblob은 비어 있고 newblob이 데이터 보유
 * 5) origblob의 부모를 newblob으로 변경, origblob에 BLOB_SNAPSHOT 내부 xattr 등록
 * 6) 두 blob 모두 메타 sync 후 freeze 해제 → 사용자에게 new_snap_blobid 반환
 *
 * Snapshot의 핵심: 기존 blob의 데이터를 "이름만 바꿔" 새 blob에 옮기고, 원본은
 * 새 blob을 부모로 가진 thin clone이 된다 (사용자에게는 데이터 그대로 보임).
 */
void
spdk_bs_create_snapshot(struct spdk_blob_store *bs, spdk_blob_id blobid,
			const struct spdk_blob_xattr_opts *snapshot_xattrs,
			spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	struct spdk_clone_snapshot_ctx *ctx = calloc(1, sizeof(*ctx)); /* [한국어] 콜백 체인 컨텍스트 */

	if (!ctx) {
		cb_fn(cb_arg, SPDK_BLOBID_INVALID, -ENOMEM);
		return;
	}
	ctx->cpl.type = SPDK_BS_CPL_TYPE_BLOBID;
	ctx->cpl.u.blobid.cb_fn = cb_fn;
	ctx->cpl.u.blobid.cb_arg = cb_arg;
	ctx->cpl.u.blobid.blobid = SPDK_BLOBID_INVALID;
	ctx->bserrno = 0;
	ctx->frozen = false;
	ctx->original.id = blobid;
	ctx->xattrs = snapshot_xattrs;

	spdk_bs_open_blob(bs, ctx->original.id, bs_snapshot_origblob_open_cpl, ctx); /* [한국어] origblob open으로 체인 시작 */
}
/* END spdk_bs_create_snapshot */

/* START spdk_bs_create_clone */

static void
bs_xattr_clone(void *arg, const char *name,
	       const void **value, size_t *value_len)
{
	assert(strncmp(name, BLOB_SNAPSHOT, sizeof(BLOB_SNAPSHOT)) == 0);

	struct spdk_blob *blob = (struct spdk_blob *)arg;
	*value = &blob->id;
	*value_len = sizeof(blob->id);
}

static void
bs_clone_newblob_open_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *clone = _blob;

	ctx->new.blob = clone;
	bs_blob_list_add(clone);

	spdk_blob_close(clone, bs_clone_snapshot_origblob_cleanup, ctx);
}

static void
bs_clone_newblob_create_cpl(void *cb_arg, spdk_blob_id blobid, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;

	ctx->cpl.u.blobid.blobid = blobid;
	spdk_bs_open_blob(ctx->original.blob->bs, blobid, bs_clone_newblob_open_cpl, ctx);
}

static void
bs_clone_origblob_open_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	struct spdk_clone_snapshot_ctx	*ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob_opts		opts;
	struct spdk_blob_xattr_opts internal_xattrs;
	char *xattr_names[] = { BLOB_SNAPSHOT };

	if (bserrno != 0) {
		bs_clone_snapshot_cleanup_finish(ctx, bserrno);
		return;
	}

	ctx->original.blob = _blob;
	ctx->original.md_ro = _blob->md_ro;

	if (!_blob->data_ro || !_blob->md_ro) {
		SPDK_DEBUGLOG(blob, "Clone not from read-only blob\n");
		ctx->bserrno = -EINVAL;
		spdk_blob_close(_blob, bs_clone_snapshot_cleanup_finish, ctx);
		return;
	}

	if (_blob->locked_operation_in_progress) {
		SPDK_DEBUGLOG(blob, "Cannot create clone - another operation in progress\n");
		ctx->bserrno = -EBUSY;
		spdk_blob_close(_blob, bs_clone_snapshot_cleanup_finish, ctx);
		return;
	}

	_blob->locked_operation_in_progress = true;

	spdk_blob_opts_init(&opts, sizeof(opts));
	blob_xattrs_init(&internal_xattrs);

	opts.thin_provision = true;
	opts.num_clusters = spdk_blob_get_num_clusters(_blob);
	opts.use_extent_table = _blob->use_extent_table;
	if (ctx->xattrs) {
		memcpy(&opts.xattrs, ctx->xattrs, sizeof(*ctx->xattrs));
	}

	/* Set internal xattr BLOB_SNAPSHOT */
	internal_xattrs.count = 1;
	internal_xattrs.ctx = _blob;
	internal_xattrs.names = xattr_names;
	internal_xattrs.get_value = bs_xattr_clone;

	bs_create_blob(_blob->bs, &opts, &internal_xattrs,
		       bs_clone_newblob_create_cpl, ctx);
}

/*
 * [한국어]
 * spdk_bs_create_clone - read-only snapshot에서 새 thin clone 생성 (공개 API)
 *
 * @bs:           blobstore
 * @blobid:       부모가 될 snapshot blob (data_ro && md_ro 여야 함)
 * @clone_xattrs: 새 clone에 부여할 사용자 xattr (옵션)
 * @cb_fn:        완료 콜백 (cb_arg, new_clone_blobid, bserrno)
 *
 * 흐름:
 * 1) snapshot 부모 blob을 open → bs_clone_origblob_open_cpl
 * 2) 부모와 같은 num_clusters / use_extent_table을 가진 thin blob 생성 (BLOB_SNAPSHOT 내부 xattr로 부모 id 기록)
 * 3) clone 생성 후 부모 snapshot의 clone 리스트에 등록 → close
 *
 * Read 시 미할당 cluster는 부모 snapshot에서 가져온다 (back_bs_dev = blob_bs_dev(parent)).
 */
void
spdk_bs_create_clone(struct spdk_blob_store *bs, spdk_blob_id blobid,
		     const struct spdk_blob_xattr_opts *clone_xattrs,
		     spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	struct spdk_clone_snapshot_ctx	*ctx = calloc(1, sizeof(*ctx));

	if (!ctx) {
		cb_fn(cb_arg, SPDK_BLOBID_INVALID, -ENOMEM);
		return;
	}

	ctx->cpl.type = SPDK_BS_CPL_TYPE_BLOBID;
	ctx->cpl.u.blobid.cb_fn = cb_fn;
	ctx->cpl.u.blobid.cb_arg = cb_arg;
	ctx->cpl.u.blobid.blobid = SPDK_BLOBID_INVALID;
	ctx->bserrno = 0;
	ctx->xattrs = clone_xattrs;
	ctx->original.id = blobid;

	spdk_bs_open_blob(bs, ctx->original.id, bs_clone_origblob_open_cpl, ctx); /* [한국어] 부모 open으로 체인 시작 */
}

/* END spdk_bs_create_clone */

/* START spdk_bs_inflate_blob */

static void
bs_inflate_blob_set_parent_cpl(void *cb_arg, struct spdk_blob *_parent, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *_blob = ctx->original.blob;

	if (bserrno != 0) {
		bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	/* Temporarily override md_ro flag for MD modification */
	_blob->md_ro = false;

	bserrno = blob_set_xattr(_blob, BLOB_SNAPSHOT, &_parent->id, sizeof(spdk_blob_id), true);
	if (bserrno != 0) {
		bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	assert(_parent != NULL);

	bs_blob_list_remove(_blob);
	_blob->parent_id = _parent->id;

	blob_back_bs_destroy(_blob);
	_blob->back_bs_dev = bs_create_blob_bs_dev(_parent);
	bs_blob_list_add(_blob);

	spdk_blob_sync_md(_blob, bs_clone_snapshot_origblob_cleanup, ctx);
}

static void
bs_inflate_blob_done(struct spdk_clone_snapshot_ctx *ctx)
{
	struct spdk_blob *_blob = ctx->original.blob;
	struct spdk_blob *_parent;

	if (ctx->allocate_all) {
		/* remove thin provisioning */
		bs_blob_list_remove(_blob);
		if (_blob->parent_id == SPDK_BLOBID_EXTERNAL_SNAPSHOT) {
			blob_remove_xattr(_blob, BLOB_EXTERNAL_SNAPSHOT_ID, true);
			_blob->invalid_flags &= ~SPDK_BLOB_EXTERNAL_SNAPSHOT;
		} else {
			blob_remove_xattr(_blob, BLOB_SNAPSHOT, true);
		}
		_blob->invalid_flags = _blob->invalid_flags & ~SPDK_BLOB_THIN_PROV;
		blob_back_bs_destroy(_blob);
		_blob->parent_id = SPDK_BLOBID_INVALID;
	} else {
		/* For now, esnap clones always have allocate_all set. */
		assert(!blob_is_esnap_clone(_blob));

		_parent = ((struct spdk_blob_bs_dev *)(_blob->back_bs_dev))->blob;
		if (_parent->parent_id != SPDK_BLOBID_INVALID) {
			/* We must change the parent of the inflated blob */
			spdk_bs_open_blob(_blob->bs, _parent->parent_id,
					  bs_inflate_blob_set_parent_cpl, ctx);
			return;
		}

		bs_blob_list_remove(_blob);
		_blob->parent_id = SPDK_BLOBID_INVALID;
		blob_back_bs_destroy(_blob);
		_blob->back_bs_dev = bs_create_zeroes_dev();
	}

	/* Temporarily override md_ro flag for MD modification */
	_blob->md_ro = false;
	blob_remove_xattr(_blob, BLOB_SNAPSHOT, true);
	_blob->state = SPDK_BLOB_STATE_DIRTY;

	spdk_blob_sync_md(_blob, bs_clone_snapshot_origblob_cleanup, ctx);
}

/* Check if cluster needs allocation */
static inline bool
bs_cluster_needs_allocation(struct spdk_blob *blob, uint64_t cluster, bool allocate_all)
{
	struct spdk_blob_bs_dev *b;

	assert(blob != NULL);

	if (blob->active.clusters[cluster] != 0) {
		/* Cluster is already allocated */
		return false;
	}

	if (blob->parent_id == SPDK_BLOBID_INVALID) {
		/* Blob have no parent blob */
		return allocate_all;
	}

	if (blob->parent_id == SPDK_BLOBID_EXTERNAL_SNAPSHOT) {
		return true;
	}

	b = (struct spdk_blob_bs_dev *)blob->back_bs_dev;
	return (allocate_all || b->blob->active.clusters[cluster] != 0);
}

static void
bs_inflate_blob_touch_next(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *_blob = ctx->original.blob;
	struct spdk_bs_cpl cpl;
	spdk_bs_user_op_t *op;
	uint64_t offset;

	if (bserrno != 0) {
		bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	for (; ctx->cluster < _blob->active.num_clusters; ctx->cluster++) {
		if (bs_cluster_needs_allocation(_blob, ctx->cluster, ctx->allocate_all)) {
			break;
		}
	}

	if (ctx->cluster < _blob->active.num_clusters) {
		offset = bs_cluster_to_lba(_blob->bs, ctx->cluster);

		/* We may safely increment a cluster before copying */
		ctx->cluster++;

		/* Use a dummy 0B read as a context for cluster copy */
		cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
		cpl.u.blob_basic.cb_fn = bs_inflate_blob_touch_next;
		cpl.u.blob_basic.cb_arg = ctx;

		op = bs_user_op_alloc(ctx->channel, &cpl, SPDK_BLOB_READ, _blob,
				      NULL, 0, offset, 0);
		if (!op) {
			bs_clone_snapshot_origblob_cleanup(ctx, -ENOMEM);
			return;
		}

		bs_allocate_and_copy_cluster(_blob, ctx->channel, offset, op);
	} else {
		bs_inflate_blob_done(ctx);
	}
}

static void
bs_inflate_blob_open_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	uint64_t clusters_needed;
	uint64_t i;

	if (bserrno != 0) {
		bs_clone_snapshot_cleanup_finish(ctx, bserrno);
		return;
	}

	ctx->original.blob = _blob;
	ctx->original.md_ro = _blob->md_ro;

	if (_blob->locked_operation_in_progress) {
		SPDK_DEBUGLOG(blob, "Cannot inflate blob - another operation in progress\n");
		ctx->bserrno = -EBUSY;
		spdk_blob_close(_blob, bs_clone_snapshot_cleanup_finish, ctx);
		return;
	}

	_blob->locked_operation_in_progress = true;

	switch (_blob->parent_id) {
	case SPDK_BLOBID_INVALID:
		if (!ctx->allocate_all) {
			/* This blob has no parent, so we cannot decouple it. */
			SPDK_ERRLOG("Cannot decouple parent of blob with no parent.\n");
			bs_clone_snapshot_origblob_cleanup(ctx, -EINVAL);
			return;
		}
		break;
	case SPDK_BLOBID_EXTERNAL_SNAPSHOT:
		/*
		 * It would be better to rely on back_bs_dev->is_zeroes(), to determine which
		 * clusters require allocation. Until there is a blobstore consumer that
		 * uses esnaps with an spdk_bs_dev that implements a useful is_zeroes() it is not
		 * worth the effort.
		 */
		ctx->allocate_all = true;
		break;
	default:
		break;
	}

	if (spdk_blob_is_thin_provisioned(_blob) == false) {
		/* This is not thin provisioned blob. No need to inflate. */
		bs_clone_snapshot_origblob_cleanup(ctx, 0);
		return;
	}

	/* Do two passes - one to verify that we can obtain enough clusters
	 * and another to actually claim them.
	 */
	clusters_needed = 0;
	for (i = 0; i < _blob->active.num_clusters; i++) {
		if (bs_cluster_needs_allocation(_blob, i, ctx->allocate_all)) {
			clusters_needed++;
		}
	}

	if (clusters_needed > _blob->bs->num_free_clusters) {
		/* Not enough free clusters. Cannot satisfy the request. */
		bs_clone_snapshot_origblob_cleanup(ctx, -ENOSPC);
		return;
	}

	ctx->cluster = 0;
	bs_inflate_blob_touch_next(ctx, 0);
}

static void
bs_inflate_blob(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
		spdk_blob_id blobid, bool allocate_all, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_clone_snapshot_ctx *ctx = calloc(1, sizeof(*ctx));

	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	ctx->cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	ctx->cpl.u.bs_basic.cb_fn = cb_fn;
	ctx->cpl.u.bs_basic.cb_arg = cb_arg;
	ctx->bserrno = 0;
	ctx->original.id = blobid;
	ctx->channel = channel;
	ctx->allocate_all = allocate_all;

	spdk_bs_open_blob(bs, ctx->original.id, bs_inflate_blob_open_cpl, ctx);
}

/*
 * [한국어]
 * spdk_bs_inflate_blob - thin blob을 thick으로 변환 (모든 cluster 채움) (공개 API)
 *
 * @bs:      blobstore
 * @channel: IO channel (할당된 cluster의 zero/copy IO 발사용)
 * @blobid:  변환할 thin blob
 * @cb_fn:   완료 콜백
 *
 * allocate_all=true: 미할당 cluster를 모두 새로 잡고, 부모에서 데이터를 복사 (또는 zero).
 * 결과적으로 부모와의 의존이 사라져 BLOB_SNAPSHOT xattr이 제거되고 parent_id=INVALID로.
 */
void
spdk_bs_inflate_blob(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
		     spdk_blob_id blobid, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	bs_inflate_blob(bs, channel, blobid, true, cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_bs_blob_decouple_parent - clone과 직속 부모 사이의 의존 끊기 (공개 API)
 *
 * @bs/channel/blobid/cb_fn/cb_arg: inflate와 동일
 *
 * allocate_all=false: 부모에 의존하던 cluster만 복사해 자기 cluster로 만들고, 부모를
 * "할아버지"(grandparent)로 갱신한다 (부모는 더 이상 필요 없어 삭제 가능 상태가 됨).
 */
void
spdk_bs_blob_decouple_parent(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
			     spdk_blob_id blobid, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	bs_inflate_blob(bs, channel, blobid, false, cb_fn, cb_arg);
}
/* END spdk_bs_inflate_blob */

/* START spdk_bs_blob_shallow_copy */

struct shallow_copy_ctx {
	struct spdk_bs_cpl cpl;
	int bserrno;

	/* Blob source for copy */
	struct spdk_blob_store *bs;
	spdk_blob_id blobid;
	struct spdk_blob *blob;
	struct spdk_io_channel *blob_channel;

	/* Destination device for copy */
	struct spdk_bs_dev *ext_dev;
	struct spdk_io_channel *ext_channel;

	/* Current cluster for copy operation */
	uint64_t cluster;

	/* Buffer for blob reading */
	uint8_t *read_buff;

	/* Struct for external device writing */
	struct spdk_bs_dev_cb_args ext_args;

	/* Actual number of copied clusters */
	uint64_t copied_clusters_count;

	/* Status callback for updates about the ongoing operation */
	spdk_blob_shallow_copy_status status_cb;

	/* Argument passed to function status_cb */
	void *status_cb_arg;
};

static void
bs_shallow_copy_cleanup_finish(void *cb_arg, int bserrno)
{
	struct shallow_copy_ctx *ctx = cb_arg;
	struct spdk_bs_cpl *cpl = &ctx->cpl;

	if (bserrno != 0) {
		SPDK_ERRLOG("blob 0x%" PRIx64 " shallow copy, cleanup error %d\n", ctx->blob->id, bserrno);
		ctx->bserrno = bserrno;
	}

	ctx->ext_dev->destroy_channel(ctx->ext_dev, ctx->ext_channel);
	spdk_free(ctx->read_buff);

	cpl->u.blob_basic.cb_fn(cpl->u.blob_basic.cb_arg, ctx->bserrno);

	free(ctx);
}

static void
bs_shallow_copy_bdev_write_cpl(struct spdk_io_channel *channel, void *cb_arg, int bserrno)
{
	struct shallow_copy_ctx *ctx = cb_arg;
	struct spdk_blob *_blob = ctx->blob;

	if (bserrno != 0) {
		SPDK_ERRLOG("blob 0x%" PRIx64 " shallow copy, ext dev write error %d\n", ctx->blob->id, bserrno);
		ctx->bserrno = bserrno;
		_blob->locked_operation_in_progress = false;
		spdk_blob_close(_blob, bs_shallow_copy_cleanup_finish, ctx);
		return;
	}

	ctx->cluster++;
	if (ctx->status_cb) {
		ctx->copied_clusters_count++;
		ctx->status_cb(ctx->copied_clusters_count, ctx->status_cb_arg);
	}

	bs_shallow_copy_cluster_find_next(ctx);
}

static void
bs_shallow_copy_blob_read_cpl(void *cb_arg, int bserrno)
{
	struct shallow_copy_ctx *ctx = cb_arg;
	struct spdk_bs_dev *ext_dev = ctx->ext_dev;
	struct spdk_blob *_blob = ctx->blob;

	if (bserrno != 0) {
		SPDK_ERRLOG("blob 0x%" PRIx64 " shallow copy, blob read error %d\n", ctx->blob->id, bserrno);
		ctx->bserrno = bserrno;
		_blob->locked_operation_in_progress = false;
		spdk_blob_close(_blob, bs_shallow_copy_cleanup_finish, ctx);
		return;
	}

	ctx->ext_args.channel = ctx->ext_channel;
	ctx->ext_args.cb_fn = bs_shallow_copy_bdev_write_cpl;
	ctx->ext_args.cb_arg = ctx;

	ext_dev->write(ext_dev, ctx->ext_channel, ctx->read_buff,
		       bs_cluster_to_lba(_blob->bs, ctx->cluster),
		       bs_dev_byte_to_lba(_blob->bs->dev, _blob->bs->cluster_sz),
		       &ctx->ext_args);
}

static void
bs_shallow_copy_cluster_find_next(void *cb_arg)
{
	struct shallow_copy_ctx *ctx = cb_arg;
	struct spdk_blob *_blob = ctx->blob;

	while (ctx->cluster < _blob->active.num_clusters) {
		if (_blob->active.clusters[ctx->cluster] != 0) {
			break;
		}

		ctx->cluster++;
	}

	if (ctx->cluster < _blob->active.num_clusters) {
		blob_request_submit_op_single(ctx->blob_channel, _blob, ctx->read_buff,
					      bs_cluster_to_lba(_blob->bs, ctx->cluster),
					      bs_dev_byte_to_lba(_blob->bs->dev, _blob->bs->cluster_sz),
					      bs_shallow_copy_blob_read_cpl, ctx, SPDK_BLOB_READ);
	} else {
		_blob->locked_operation_in_progress = false;
		spdk_blob_close(_blob, bs_shallow_copy_cleanup_finish, ctx);
	}
}

static void
bs_shallow_copy_blob_open_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	struct shallow_copy_ctx *ctx = cb_arg;
	struct spdk_bs_dev *ext_dev = ctx->ext_dev;
	uint32_t blob_block_size;
	uint64_t blob_total_size;

	if (bserrno != 0) {
		SPDK_ERRLOG("Shallow copy blob open error %d\n", bserrno);
		ctx->bserrno = bserrno;
		bs_shallow_copy_cleanup_finish(ctx, 0);
		return;
	}

	if (!spdk_blob_is_read_only(_blob)) {
		SPDK_ERRLOG("blob 0x%" PRIx64 " shallow copy, blob must be read only\n", _blob->id);
		ctx->bserrno = -EPERM;
		spdk_blob_close(_blob, bs_shallow_copy_cleanup_finish, ctx);
		return;
	}

	blob_block_size = _blob->bs->dev->blocklen;
	blob_total_size = spdk_blob_get_num_clusters(_blob) * spdk_bs_get_cluster_size(_blob->bs);

	if (blob_total_size > ext_dev->blockcnt * ext_dev->blocklen) {
		SPDK_ERRLOG("blob 0x%" PRIx64 " shallow copy, external device must have at least blob size\n",
			    _blob->id);
		ctx->bserrno = -EINVAL;
		spdk_blob_close(_blob, bs_shallow_copy_cleanup_finish, ctx);
		return;
	}

	if (blob_block_size % ext_dev->blocklen != 0) {
		SPDK_ERRLOG("blob 0x%" PRIx64 " shallow copy, external device block size is not compatible with \
blobstore block size\n", _blob->id);
		ctx->bserrno = -EINVAL;
		spdk_blob_close(_blob, bs_shallow_copy_cleanup_finish, ctx);
		return;
	}

	ctx->blob = _blob;

	if (_blob->locked_operation_in_progress) {
		SPDK_DEBUGLOG(blob, "blob 0x%" PRIx64 " shallow copy - another operation in progress\n", _blob->id);
		ctx->bserrno = -EBUSY;
		spdk_blob_close(_blob, bs_shallow_copy_cleanup_finish, ctx);
		return;
	}

	_blob->locked_operation_in_progress = true;

	ctx->cluster = 0;
	bs_shallow_copy_cluster_find_next(ctx);
}

/*
 * [한국어]
 * spdk_bs_blob_shallow_copy - blob의 "할당된" cluster만 외부 bs_dev로 복사 (공개 API)
 *
 * @blob_id:        복사 원본 blob
 * @ext_dev:        목적지 bs_dev (외부, blobstore 외부 매체일 수 있음)
 * @status_cb_fn:   진행 상황 보고 콜백 (선택) — (ctx, copied_clusters, total_clusters)
 * @cb_fn:          최종 완료 콜백
 *
 * 할당되지 않은 cluster는 건너뛰며, 할당된 cluster만 read 후 ext_dev에 write 한다.
 * 이렇게 하면 thin blob의 실제 데이터만 효율적으로 외부로 export 가능 (예: 백업 시).
 *
 * 동작:
 * 1) 컨텍스트와 cluster 크기의 read_buff 할당
 * 2) ext_dev에 채널 생성
 * 3) blob open → bs_shallow_copy_blob_open_cpl
 * 4) bs_shallow_copy_cluster_find_next → 다음 할당 cluster를 찾아 read → write 반복
 */
int
spdk_bs_blob_shallow_copy(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
			  spdk_blob_id blobid, struct spdk_bs_dev *ext_dev,
			  spdk_blob_shallow_copy_status status_cb_fn, void *status_cb_arg,
			  spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct shallow_copy_ctx *ctx;
	struct spdk_io_channel *ext_channel;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->bs = bs;
	ctx->blobid = blobid;
	ctx->cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	ctx->cpl.u.bs_basic.cb_fn = cb_fn;
	ctx->cpl.u.bs_basic.cb_arg = cb_arg;
	ctx->bserrno = 0;
	ctx->blob_channel = channel;
	ctx->status_cb = status_cb_fn;
	ctx->status_cb_arg = status_cb_arg;
	ctx->read_buff = spdk_malloc(bs->cluster_sz, bs->dev->blocklen, NULL,
				     SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->read_buff) {
		free(ctx);
		return -ENOMEM;
	}

	ext_channel = ext_dev->create_channel(ext_dev);
	if (!ext_channel) {
		spdk_free(ctx->read_buff);
		free(ctx);
		return -ENOMEM;
	}
	ctx->ext_dev = ext_dev;
	ctx->ext_channel = ext_channel;

	spdk_bs_open_blob(ctx->bs, ctx->blobid, bs_shallow_copy_blob_open_cpl, ctx);

	return 0;
}
/* END spdk_bs_blob_shallow_copy */

/* START spdk_bs_blob_set_parent */

struct set_parent_ctx {
	struct spdk_blob_store *bs;
	int			bserrno;
	spdk_bs_op_complete	cb_fn;
	void			*cb_arg;

	struct spdk_blob	*blob;
	bool			blob_md_ro;

	struct blob_parent	parent;
};

static void
bs_set_parent_cleanup_finish(void *cb_arg, int bserrno)
{
	struct set_parent_ctx *ctx = cb_arg;

	assert(ctx != NULL);

	if (bserrno != 0) {
		SPDK_ERRLOG("blob set parent finish error %d\n", bserrno);
		if (ctx->bserrno == 0) {
			ctx->bserrno = bserrno;
		}
	}

	ctx->cb_fn(ctx->cb_arg, ctx->bserrno);

	free(ctx);
}

static void
bs_set_parent_close_snapshot(void *cb_arg, int bserrno)
{
	struct set_parent_ctx *ctx = cb_arg;

	if (ctx->bserrno != 0) {
		spdk_blob_close(ctx->parent.u.snapshot.blob, bs_set_parent_cleanup_finish, ctx);
		return;
	}

	if (bserrno != 0) {
		SPDK_ERRLOG("blob close error %d\n", bserrno);
		ctx->bserrno = bserrno;
	}

	bs_set_parent_cleanup_finish(ctx, ctx->bserrno);
}

static void
bs_set_parent_close_blob(void *cb_arg, int bserrno)
{
	struct set_parent_ctx *ctx = cb_arg;
	struct spdk_blob *blob = ctx->blob;
	struct spdk_blob *snapshot = ctx->parent.u.snapshot.blob;

	if (bserrno != 0 && ctx->bserrno == 0) {
		SPDK_ERRLOG("error %d in metadata sync\n", bserrno);
		ctx->bserrno = bserrno;
	}

	/* Revert md_ro to original state */
	blob->md_ro = ctx->blob_md_ro;

	blob->locked_operation_in_progress = false;
	snapshot->locked_operation_in_progress = false;

	spdk_blob_close(blob, bs_set_parent_close_snapshot, ctx);
}

static void
bs_set_parent_set_back_bs_dev_done(void *cb_arg, int bserrno)
{
	struct set_parent_ctx *ctx = cb_arg;
	struct spdk_blob *blob = ctx->blob;

	if (bserrno != 0) {
		SPDK_ERRLOG("error %d setting back_bs_dev\n", bserrno);
		ctx->bserrno = bserrno;
		bs_set_parent_close_blob(ctx, bserrno);
		return;
	}

	spdk_blob_sync_md(blob, bs_set_parent_close_blob, ctx);
}

static int
bs_set_parent_refs(struct spdk_blob *blob, struct blob_parent *parent)
{
	int rc;

	bs_blob_list_remove(blob);

	rc = blob_set_xattr(blob, BLOB_SNAPSHOT, &parent->u.snapshot.id, sizeof(spdk_blob_id), true);
	if (rc != 0) {
		SPDK_ERRLOG("error %d setting snapshot xattr\n", rc);
		return rc;
	}
	blob->parent_id = parent->u.snapshot.id;

	if (blob_is_esnap_clone(blob)) {
		/* Remove the xattr that references the external snapshot */
		blob->invalid_flags &= ~SPDK_BLOB_EXTERNAL_SNAPSHOT;
		blob_remove_xattr(blob, BLOB_EXTERNAL_SNAPSHOT_ID, true);
	}

	bs_blob_list_add(blob);

	return 0;
}

static void
bs_set_parent_snapshot_open_cpl(void *cb_arg, struct spdk_blob *snapshot, int bserrno)
{
	struct set_parent_ctx *ctx = cb_arg;
	struct spdk_blob *blob = ctx->blob;
	struct spdk_bs_dev *back_bs_dev;

	if (bserrno != 0) {
		SPDK_ERRLOG("snapshot open error %d\n", bserrno);
		ctx->bserrno = bserrno;
		spdk_blob_close(blob, bs_set_parent_cleanup_finish, ctx);
		return;
	}

	ctx->parent.u.snapshot.blob = snapshot;
	ctx->parent.u.snapshot.id = snapshot->id;

	if (!spdk_blob_is_snapshot(snapshot)) {
		SPDK_ERRLOG("parent blob is not a snapshot\n");
		ctx->bserrno = -EINVAL;
		spdk_blob_close(blob, bs_set_parent_close_snapshot, ctx);
		return;
	}

	if (blob->active.num_clusters != snapshot->active.num_clusters) {
		SPDK_ERRLOG("parent blob has a number of clusters different from child's ones\n");
		ctx->bserrno = -EINVAL;
		spdk_blob_close(blob, bs_set_parent_close_snapshot, ctx);
		return;
	}

	if (blob->locked_operation_in_progress || snapshot->locked_operation_in_progress) {
		SPDK_ERRLOG("cannot set parent of blob, another operation in progress\n");
		ctx->bserrno = -EBUSY;
		spdk_blob_close(blob, bs_set_parent_close_snapshot, ctx);
		return;
	}

	blob->locked_operation_in_progress = true;
	snapshot->locked_operation_in_progress = true;

	/* Temporarily override md_ro flag for MD modification */
	blob->md_ro = false;

	back_bs_dev = bs_create_blob_bs_dev(snapshot);

	blob_set_back_bs_dev(blob, back_bs_dev, bs_set_parent_refs, &ctx->parent,
			     bs_set_parent_set_back_bs_dev_done,
			     ctx);
}

static void
bs_set_parent_blob_open_cpl(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct set_parent_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("blob open error %d\n", bserrno);
		ctx->bserrno = bserrno;
		bs_set_parent_cleanup_finish(ctx, 0);
		return;
	}

	if (!spdk_blob_is_thin_provisioned(blob)) {
		SPDK_ERRLOG("blob is not thin-provisioned\n");
		ctx->bserrno = -EINVAL;
		spdk_blob_close(blob, bs_set_parent_cleanup_finish, ctx);
		return;
	}

	ctx->blob = blob;
	ctx->blob_md_ro = blob->md_ro;

	spdk_bs_open_blob(ctx->bs, ctx->parent.u.snapshot.id, bs_set_parent_snapshot_open_cpl, ctx);
}

/*
 * [한국어]
 * spdk_bs_blob_set_parent - 기존 blob의 부모를 다른 snapshot으로 재지정 (공개 API)
 *
 * @blob_id:     thin blob (대상)
 * @snapshot_id: 새 부모 snapshot
 *
 * 사전 조건: blob은 thin이어야 하고, blob_id != snapshot_id, 이미 같은 부모면 -EEXIST.
 * 흐름: blob open → snapshot open → 부모 list 갱신 + back_bs_dev 교체 → sync_md.
 */
void
spdk_bs_blob_set_parent(struct spdk_blob_store *bs, spdk_blob_id blob_id,
			spdk_blob_id snapshot_id, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct set_parent_ctx *ctx;

	if (snapshot_id == SPDK_BLOBID_INVALID) {
		SPDK_ERRLOG("snapshot id not valid\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	if (blob_id == snapshot_id) {
		SPDK_ERRLOG("blob id and snapshot id cannot be the same\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	if (spdk_blob_get_parent_snapshot(bs, blob_id) == snapshot_id) {
		SPDK_NOTICELOG("snapshot is already the parent of blob\n");
		cb_fn(cb_arg, -EEXIST);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bs = bs;
	ctx->parent.u.snapshot.id = snapshot_id;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->bserrno = 0;

	spdk_bs_open_blob(bs, blob_id, bs_set_parent_blob_open_cpl, ctx);
}
/* END spdk_bs_blob_set_parent */

/* START spdk_bs_blob_set_external_parent */

static void
bs_set_external_parent_cleanup_finish(void *cb_arg, int bserrno)
{
	struct set_parent_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("blob set external parent finish error %d\n", bserrno);
		if (ctx->bserrno == 0) {
			ctx->bserrno = bserrno;
		}
	}

	ctx->cb_fn(ctx->cb_arg, ctx->bserrno);

	free(ctx->parent.u.esnap.id);
	free(ctx);
}

static void
bs_set_external_parent_close_blob(void *cb_arg, int bserrno)
{
	struct set_parent_ctx *ctx = cb_arg;
	struct spdk_blob *blob = ctx->blob;

	if (bserrno != 0 && ctx->bserrno == 0) {
		SPDK_ERRLOG("error %d in metadata sync\n", bserrno);
		ctx->bserrno = bserrno;
	}

	/* Revert md_ro to original state */
	blob->md_ro = ctx->blob_md_ro;

	blob->locked_operation_in_progress = false;

	spdk_blob_close(blob, bs_set_external_parent_cleanup_finish, ctx);
}

static void
bs_set_external_parent_unfrozen(void *cb_arg, int bserrno)
{
	struct set_parent_ctx *ctx = cb_arg;
	struct spdk_blob *blob = ctx->blob;

	if (bserrno != 0) {
		SPDK_ERRLOG("error %d setting back_bs_dev\n", bserrno);
		ctx->bserrno = bserrno;
		bs_set_external_parent_close_blob(ctx, bserrno);
		return;
	}

	spdk_blob_sync_md(blob, bs_set_external_parent_close_blob, ctx);
}

static int
bs_set_external_parent_refs(struct spdk_blob *blob, struct blob_parent *parent)
{
	int rc;

	bs_blob_list_remove(blob);

	if (spdk_blob_is_clone(blob)) {
		/* Remove the xattr that references the snapshot */
		blob->parent_id = SPDK_BLOBID_INVALID;
		blob_remove_xattr(blob, BLOB_SNAPSHOT, true);
	}

	rc = blob_set_xattr(blob, BLOB_EXTERNAL_SNAPSHOT_ID, parent->u.esnap.id,
			    parent->u.esnap.id_len, true);
	if (rc != 0) {
		SPDK_ERRLOG("error %d setting external snapshot xattr\n", rc);
		return rc;
	}
	blob->invalid_flags |= SPDK_BLOB_EXTERNAL_SNAPSHOT;
	blob->parent_id = SPDK_BLOBID_EXTERNAL_SNAPSHOT;

	bs_blob_list_add(blob);

	return 0;
}

static void
bs_set_external_parent_blob_open_cpl(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct set_parent_ctx *ctx = cb_arg;
	const void *esnap_id;
	size_t esnap_id_len;
	int rc;

	if (bserrno != 0) {
		SPDK_ERRLOG("blob open error %d\n", bserrno);
		ctx->bserrno = bserrno;
		bs_set_parent_cleanup_finish(ctx, 0);
		return;
	}

	ctx->blob = blob;
	ctx->blob_md_ro = blob->md_ro;

	rc = spdk_blob_get_esnap_id(blob, &esnap_id, &esnap_id_len);
	if (rc == 0 && esnap_id != NULL && esnap_id_len == ctx->parent.u.esnap.id_len &&
	    memcmp(esnap_id, ctx->parent.u.esnap.id, esnap_id_len) == 0) {
		SPDK_ERRLOG("external snapshot is already the parent of blob\n");
		ctx->bserrno = -EEXIST;
		goto error;
	}

	if (!spdk_blob_is_thin_provisioned(blob)) {
		SPDK_ERRLOG("blob is not thin-provisioned\n");
		ctx->bserrno = -EINVAL;
		goto error;
	}

	if (blob->locked_operation_in_progress) {
		SPDK_ERRLOG("cannot set external parent of blob, another operation in progress\n");
		ctx->bserrno = -EBUSY;
		goto error;
	}

	blob->locked_operation_in_progress = true;

	/* Temporarily override md_ro flag for MD modification */
	blob->md_ro = false;

	blob_set_back_bs_dev(blob, ctx->parent.u.esnap.back_bs_dev, bs_set_external_parent_refs,
			     &ctx->parent, bs_set_external_parent_unfrozen, ctx);
	return;

error:
	spdk_blob_close(blob, bs_set_external_parent_cleanup_finish, ctx);
}

/*
 * [한국어]
 * spdk_bs_blob_set_external_parent - blob에 외부 snapshot(esnap)을 부모로 지정 (공개 API)
 *
 * @blob_id:       대상 blob (thin)
 * @esnap_bs_dev:  새 부모가 될 외부 bs_dev (사이즈는 cluster_sz의 배수여야 함)
 * @esnap_id:      외부 부모를 식별하는 사용자 정의 id (디스크 xattr로 영속화)
 * @esnap_id_len:  esnap_id 길이
 *
 * blob의 invalid_flags에 EXTERNAL_SNAPSHOT 비트를 켜고, BLOB_EXTERNAL_SNAPSHOT_ID 내부
 * xattr로 식별 정보를 저장. back_bs_dev는 esnap_bs_dev로 교체.
 *
 * 활용: 다른 blobstore의 snapshot 또는 readonly 외부 데이터를 이 blob의 부모로 만들어
 * thin clone처럼 활용 (export/import 시나리오).
 */
void
spdk_bs_blob_set_external_parent(struct spdk_blob_store *bs, spdk_blob_id blob_id,
				 struct spdk_bs_dev *esnap_bs_dev, const void *esnap_id,
				 uint32_t esnap_id_len, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct set_parent_ctx *ctx;
	uint64_t esnap_dev_size, cluster_sz;

	if (sizeof(blob_id) == esnap_id_len && memcmp(&blob_id, esnap_id, sizeof(blob_id)) == 0) {
		SPDK_ERRLOG("blob id and external snapshot id cannot be the same\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	esnap_dev_size = esnap_bs_dev->blockcnt * esnap_bs_dev->blocklen;
	cluster_sz = spdk_bs_get_cluster_size(bs);
	if ((esnap_dev_size % cluster_sz) != 0) {
		SPDK_ERRLOG("Esnap device size %" PRIu64 " is not an integer multiple of "
			    "cluster size %" PRIu64 "\n", esnap_dev_size, cluster_sz);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->parent.u.esnap.id = calloc(1, esnap_id_len);
	if (!ctx->parent.u.esnap.id) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bs = bs;
	ctx->parent.u.esnap.back_bs_dev = esnap_bs_dev;
	memcpy(ctx->parent.u.esnap.id, esnap_id, esnap_id_len);
	ctx->parent.u.esnap.id_len = esnap_id_len;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->bserrno = 0;

	spdk_bs_open_blob(bs, blob_id, bs_set_external_parent_blob_open_cpl, ctx);
}
/* END spdk_bs_blob_set_external_parent */

/* START spdk_blob_resize */
struct spdk_bs_resize_ctx {
	spdk_blob_op_complete cb_fn;
	void *cb_arg;
	struct spdk_blob *blob;
	uint64_t sz;
	int rc;
};

static void
bs_resize_unfreeze_cpl(void *cb_arg, int rc)
{
	struct spdk_bs_resize_ctx *ctx = (struct spdk_bs_resize_ctx *)cb_arg;

	if (rc != 0) {
		SPDK_ERRLOG("Unfreeze failed, rc=%d\n", rc);
	}

	if (ctx->rc != 0) {
		SPDK_ERRLOG("Unfreeze failed, ctx->rc=%d\n", ctx->rc);
		rc = ctx->rc;
	}

	ctx->blob->locked_operation_in_progress = false;

	ctx->cb_fn(ctx->cb_arg, rc);
	free(ctx);
}

static void
bs_resize_freeze_cpl(void *cb_arg, int rc)
{
	struct spdk_bs_resize_ctx *ctx = (struct spdk_bs_resize_ctx *)cb_arg;

	if (rc != 0) {
		ctx->blob->locked_operation_in_progress = false;
		ctx->cb_fn(ctx->cb_arg, rc);
		free(ctx);
		return;
	}

	ctx->rc = blob_resize(ctx->blob, ctx->sz);

	blob_unfreeze_io(ctx->blob, bs_resize_unfreeze_cpl, ctx);
}

/*
 * [한국어]
 * spdk_blob_resize - blob의 cluster 수를 변경 (확장 또는 축소) (공개 API)
 *
 * @blob:  대상 blob
 * @sz:    새 cluster 수 (0이면 빈 blob)
 * @cb_fn: 완료 콜백
 *
 * 동작:
 * 1) md_ro면 -EPERM, 변경 없으면 즉시 0 반환, 다른 작업 진행 중이면 -EBUSY
 * 2) blob_freeze_io로 진행 중인 IO 모두 멈춤
 * 3) blob_resize로 클러스터 배열 확장/축소 (thin: 매핑 슬롯만 늘림, thick: 즉시 cluster 할당)
 * 4) 완료 후 blob_unfreeze_io → 사용자 콜백
 *
 * 주의: 새 크기는 메모리에만 반영 — 디스크 영속화는 spdk_blob_sync_md를 따로 호출해야 한다.
 */
void
spdk_blob_resize(struct spdk_blob *blob, uint64_t sz, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_resize_ctx *ctx;

	blob_verify_md_op(blob);                           /* [한국어] md_thread 강제 */

	SPDK_DEBUGLOG(blob, "Resizing blob 0x%" PRIx64 " to %" PRIu64 " clusters\n", blob->id, sz);

	if (blob->md_ro) {                                 /* [한국어] read-only blob은 변경 거부 */
		cb_fn(cb_arg, -EPERM);
		return;
	}

	if (sz == blob->active.num_clusters) {             /* [한국어] 동일 크기면 no-op */
		cb_fn(cb_arg, 0);
		return;
	}

	if (blob->locked_operation_in_progress) {          /* [한국어] 동시 작업 충돌 회피 */
		cb_fn(cb_arg, -EBUSY);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	blob->locked_operation_in_progress = true;         /* [한국어] resize 동안 다른 op 차단 */
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->blob = blob;
	ctx->sz = sz;
	blob_freeze_io(blob, bs_resize_freeze_cpl, ctx);   /* [한국어] IO freeze → bs_resize_freeze_cpl에서 실제 resize */
}

/* END spdk_blob_resize */


/* START spdk_bs_delete_blob */

static void
bs_delete_close_cpl(void *cb_arg, int bserrno)
{
	spdk_bs_sequence_t *seq = cb_arg;

	bs_sequence_finish(seq, bserrno);
}

static void
bs_delete_persist_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;

	if (bserrno != 0) {
		/*
		 * We already removed this blob from the blobstore tailq, so
		 *  we need to free it here since this is the last reference
		 *  to it.
		 */
		blob_free(blob);
		bs_delete_close_cpl(seq, bserrno);
		return;
	}

	/*
	 * This will immediately decrement the ref_count and call
	 *  the completion routine since the metadata state is clean.
	 *  By calling spdk_blob_close, we reduce the number of call
	 *  points into code that touches the blob->open_ref count
	 *  and the blobstore's blob list.
	 */
	spdk_blob_close(blob, bs_delete_close_cpl, seq);
}

struct delete_snapshot_ctx {
	struct spdk_blob_list *parent_snapshot_entry;
	struct spdk_blob *snapshot;
	struct spdk_blob_md_page *page;
	bool snapshot_md_ro;
	struct spdk_blob *clone;
	bool clone_md_ro;
	spdk_blob_op_with_handle_complete cb_fn;
	void *cb_arg;
	int bserrno;
	uint32_t next_extent_page;
};

static void
delete_blob_cleanup_finish(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("Snapshot cleanup error %d\n", bserrno);
	}

	assert(ctx != NULL);

	if (bserrno != 0 && ctx->bserrno == 0) {
		ctx->bserrno = bserrno;
	}

	ctx->cb_fn(ctx->cb_arg, ctx->snapshot, ctx->bserrno);
	spdk_free(ctx->page);
	free(ctx);
}

static void
delete_snapshot_cleanup_snapshot(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		ctx->bserrno = bserrno;
		SPDK_ERRLOG("Clone cleanup error %d\n", bserrno);
	}

	if (ctx->bserrno != 0) {
		assert(blob_lookup(ctx->snapshot->bs, ctx->snapshot->id) == NULL);
		RB_INSERT(spdk_blob_tree, &ctx->snapshot->bs->open_blobs, ctx->snapshot);
		spdk_bit_array_set(ctx->snapshot->bs->open_blobids, ctx->snapshot->id);
	}

	ctx->snapshot->locked_operation_in_progress = false;
	ctx->snapshot->md_ro = ctx->snapshot_md_ro;

	spdk_blob_close(ctx->snapshot, delete_blob_cleanup_finish, ctx);
}

static void
delete_snapshot_cleanup_clone(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;

	ctx->clone->locked_operation_in_progress = false;
	ctx->clone->md_ro = ctx->clone_md_ro;

	spdk_blob_close(ctx->clone, delete_snapshot_cleanup_snapshot, ctx);
}

static void
delete_snapshot_unfreeze_cpl(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;

	if (bserrno) {
		ctx->bserrno = bserrno;
		delete_snapshot_cleanup_clone(ctx, 0);
		return;
	}

	ctx->clone->locked_operation_in_progress = false;
	spdk_blob_close(ctx->clone, delete_blob_cleanup_finish, ctx);
}

static void
delete_snapshot_sync_snapshot_cpl(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;
	struct spdk_blob_list *parent_snapshot_entry = NULL;
	struct spdk_blob_list *snapshot_entry = NULL;
	struct spdk_blob_list *clone_entry = NULL;
	struct spdk_blob_list *snapshot_clone_entry = NULL;

	if (bserrno) {
		SPDK_ERRLOG("Failed to sync MD on blob\n");
		ctx->bserrno = bserrno;
		delete_snapshot_cleanup_clone(ctx, 0);
		return;
	}

	/* Get snapshot entry for the snapshot we want to remove */
	snapshot_entry = bs_get_snapshot_entry(ctx->snapshot->bs, ctx->snapshot->id);

	assert(snapshot_entry != NULL);

	/* Remove clone entry in this snapshot (at this point there can be only one clone) */
	clone_entry = TAILQ_FIRST(&snapshot_entry->clones);
	assert(clone_entry != NULL);
	TAILQ_REMOVE(&snapshot_entry->clones, clone_entry, link);
	snapshot_entry->clone_count--;
	assert(TAILQ_EMPTY(&snapshot_entry->clones));

	switch (ctx->snapshot->parent_id) {
	case SPDK_BLOBID_INVALID:
	case SPDK_BLOBID_EXTERNAL_SNAPSHOT:
		/* No parent snapshot - just remove clone entry */
		free(clone_entry);
		break;
	default:
		/* This snapshot is at the same time a clone of another snapshot - we need to
		 * update parent snapshot (remove current clone, add new one inherited from
		 * the snapshot that is being removed) */

		/* Get snapshot entry for parent snapshot and clone entry within that snapshot for
		 * snapshot that we are removing */
		blob_get_snapshot_and_clone_entries(ctx->snapshot, &parent_snapshot_entry,
						    &snapshot_clone_entry);

		/* Switch clone entry in parent snapshot */
		TAILQ_INSERT_TAIL(&parent_snapshot_entry->clones, clone_entry, link);
		TAILQ_REMOVE(&parent_snapshot_entry->clones, snapshot_clone_entry, link);
		free(snapshot_clone_entry);
	}

	/* Restore md_ro flags */
	ctx->clone->md_ro = ctx->clone_md_ro;
	ctx->snapshot->md_ro = ctx->snapshot_md_ro;

	blob_unfreeze_io(ctx->clone, delete_snapshot_unfreeze_cpl, ctx);
}

static void
delete_snapshot_sync_clone_cpl(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;
	uint64_t i;

	ctx->snapshot->md_ro = false;

	if (bserrno) {
		SPDK_ERRLOG("Failed to sync MD on clone\n");
		ctx->bserrno = bserrno;

		/* Restore snapshot to previous state */
		bserrno = blob_remove_xattr(ctx->snapshot, SNAPSHOT_PENDING_REMOVAL, true);
		if (bserrno != 0) {
			delete_snapshot_cleanup_clone(ctx, bserrno);
			return;
		}

		spdk_blob_sync_md(ctx->snapshot, delete_snapshot_cleanup_clone, ctx);
		return;
	}

	/* Clear cluster map entries for snapshot */
	for (i = 0; i < ctx->snapshot->active.num_clusters && i < ctx->clone->active.num_clusters; i++) {
		if (ctx->clone->active.clusters[i] == ctx->snapshot->active.clusters[i]) {
			if (ctx->snapshot->active.clusters[i] != 0) {
				ctx->snapshot->active.num_allocated_clusters--;
			}
			ctx->snapshot->active.clusters[i] = 0;
		}
	}
	for (i = 0; i < ctx->snapshot->active.num_extent_pages &&
	     i < ctx->clone->active.num_extent_pages; i++) {
		if (ctx->clone->active.extent_pages[i] == ctx->snapshot->active.extent_pages[i]) {
			ctx->snapshot->active.extent_pages[i] = 0;
		}
	}

	blob_set_thin_provision(ctx->snapshot);
	ctx->snapshot->state = SPDK_BLOB_STATE_DIRTY;

	if (ctx->parent_snapshot_entry != NULL) {
		ctx->snapshot->back_bs_dev = NULL;
	}

	spdk_blob_sync_md(ctx->snapshot, delete_snapshot_sync_snapshot_cpl, ctx);
}

static void
delete_snapshot_update_extent_pages_cpl(struct delete_snapshot_ctx *ctx)
{
	int bserrno;

	/* Delete old backing bs_dev from clone (related to snapshot that will be removed) */
	blob_back_bs_destroy(ctx->clone);

	/* Set/remove snapshot xattr and switch parent ID and backing bs_dev on clone... */
	if (ctx->snapshot->parent_id == SPDK_BLOBID_EXTERNAL_SNAPSHOT) {
		bserrno = bs_snapshot_copy_xattr(ctx->clone, ctx->snapshot,
						 BLOB_EXTERNAL_SNAPSHOT_ID);
		if (bserrno != 0) {
			ctx->bserrno = bserrno;

			/* Restore snapshot to previous state */
			bserrno = blob_remove_xattr(ctx->snapshot, SNAPSHOT_PENDING_REMOVAL, true);
			if (bserrno != 0) {
				delete_snapshot_cleanup_clone(ctx, bserrno);
				return;
			}

			spdk_blob_sync_md(ctx->snapshot, delete_snapshot_cleanup_clone, ctx);
			return;
		}
		ctx->clone->parent_id = SPDK_BLOBID_EXTERNAL_SNAPSHOT;
		ctx->clone->back_bs_dev = ctx->snapshot->back_bs_dev;
		/* Do not delete the external snapshot along with this snapshot */
		ctx->snapshot->back_bs_dev = NULL;
		ctx->clone->invalid_flags |= SPDK_BLOB_EXTERNAL_SNAPSHOT;
	} else if (ctx->parent_snapshot_entry != NULL) {
		/* ...to parent snapshot */
		ctx->clone->parent_id = ctx->parent_snapshot_entry->id;
		ctx->clone->back_bs_dev = ctx->snapshot->back_bs_dev;
		blob_set_xattr(ctx->clone, BLOB_SNAPSHOT, &ctx->parent_snapshot_entry->id,
			       sizeof(spdk_blob_id),
			       true);
	} else {
		/* ...to blobid invalid and zeroes dev */
		ctx->clone->parent_id = SPDK_BLOBID_INVALID;
		ctx->clone->back_bs_dev = bs_create_zeroes_dev();
		blob_remove_xattr(ctx->clone, BLOB_SNAPSHOT, true);
	}

	spdk_blob_sync_md(ctx->clone, delete_snapshot_sync_clone_cpl, ctx);
}

static void
delete_snapshot_update_extent_pages(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;
	uint32_t *extent_page;
	uint64_t i;

	for (i = ctx->next_extent_page; i < ctx->snapshot->active.num_extent_pages &&
	     i < ctx->clone->active.num_extent_pages; i++) {
		if (ctx->snapshot->active.extent_pages[i] == 0) {
			/* No extent page to use from snapshot */
			continue;
		}

		extent_page = &ctx->clone->active.extent_pages[i];
		if (*extent_page == 0) {
			/* Copy extent page from snapshot when clone did not have a matching one */
			*extent_page = ctx->snapshot->active.extent_pages[i];
			continue;
		}

		/* Clone and snapshot both contain partially filled matching extent pages.
		 * Update the clone extent page in place with cluster map containing the mix of both. */
		ctx->next_extent_page = i + 1;

		blob_write_extent_page(ctx->clone, *extent_page, i * SPDK_EXTENTS_PER_EP, ctx->page,
				       delete_snapshot_update_extent_pages, ctx);
		return;
	}
	delete_snapshot_update_extent_pages_cpl(ctx);
}

static void
delete_snapshot_sync_snapshot_xattr_cpl(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;
	uint64_t i;

	/* Temporarily override md_ro flag for clone for MD modification */
	ctx->clone_md_ro = ctx->clone->md_ro;
	ctx->clone->md_ro = false;

	if (bserrno) {
		SPDK_ERRLOG("Failed to sync MD with xattr on blob\n");
		ctx->bserrno = bserrno;
		delete_snapshot_cleanup_clone(ctx, 0);
		return;
	}

	/* Copy snapshot map to clone map (only unallocated clusters in clone) */
	for (i = 0; i < ctx->snapshot->active.num_clusters && i < ctx->clone->active.num_clusters; i++) {
		if (ctx->clone->active.clusters[i] == 0) {
			ctx->clone->active.clusters[i] = ctx->snapshot->active.clusters[i];
			if (ctx->clone->active.clusters[i] != 0) {
				ctx->clone->active.num_allocated_clusters++;
			}
		}
	}
	ctx->next_extent_page = 0;
	delete_snapshot_update_extent_pages(ctx, 0);
}

static void
delete_snapshot_esnap_channels_destroyed_cb(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("blob 0x%" PRIx64 ": failed to destroy esnap channels: %d\n",
			    blob->id, bserrno);
		/* That error should not stop us from syncing metadata. */
	}

	spdk_blob_sync_md(ctx->snapshot, delete_snapshot_sync_snapshot_xattr_cpl, ctx);
}

static void
delete_snapshot_freeze_io_cb(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;

	if (bserrno) {
		SPDK_ERRLOG("Failed to freeze I/O on clone\n");
		ctx->bserrno = bserrno;
		delete_snapshot_cleanup_clone(ctx, 0);
		return;
	}

	/* Temporarily override md_ro flag for snapshot for MD modification */
	ctx->snapshot_md_ro = ctx->snapshot->md_ro;
	ctx->snapshot->md_ro = false;

	/* Mark blob as pending for removal for power failure safety, use clone id for recovery */
	ctx->bserrno = blob_set_xattr(ctx->snapshot, SNAPSHOT_PENDING_REMOVAL, &ctx->clone->id,
				      sizeof(spdk_blob_id), true);
	if (ctx->bserrno != 0) {
		delete_snapshot_cleanup_clone(ctx, 0);
		return;
	}

	if (blob_is_esnap_clone(ctx->snapshot)) {
		blob_esnap_destroy_bs_dev_channels(ctx->snapshot, false,
						   delete_snapshot_esnap_channels_destroyed_cb,
						   ctx);
		return;
	}

	spdk_blob_sync_md(ctx->snapshot, delete_snapshot_sync_snapshot_xattr_cpl, ctx);
}

static void
delete_snapshot_open_clone_cb(void *cb_arg, struct spdk_blob *clone, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;

	if (bserrno) {
		SPDK_ERRLOG("Failed to open clone\n");
		ctx->bserrno = bserrno;
		delete_snapshot_cleanup_snapshot(ctx, 0);
		return;
	}

	ctx->clone = clone;

	if (clone->locked_operation_in_progress) {
		SPDK_DEBUGLOG(blob, "Cannot remove blob - another operation in progress on its clone\n");
		ctx->bserrno = -EBUSY;
		spdk_blob_close(ctx->clone, delete_snapshot_cleanup_snapshot, ctx);
		return;
	}

	clone->locked_operation_in_progress = true;

	blob_freeze_io(clone, delete_snapshot_freeze_io_cb, ctx);
}

static void
update_clone_on_snapshot_deletion(struct spdk_blob *snapshot, struct delete_snapshot_ctx *ctx)
{
	struct spdk_blob_list *snapshot_entry = NULL;
	struct spdk_blob_list *clone_entry = NULL;
	struct spdk_blob_list *snapshot_clone_entry = NULL;

	/* Get snapshot entry for the snapshot we want to remove */
	snapshot_entry = bs_get_snapshot_entry(snapshot->bs, snapshot->id);

	assert(snapshot_entry != NULL);

	/* Get clone of the snapshot (at this point there can be only one clone) */
	clone_entry = TAILQ_FIRST(&snapshot_entry->clones);
	assert(snapshot_entry->clone_count == 1);
	assert(clone_entry != NULL);

	/* Get snapshot entry for parent snapshot and clone entry within that snapshot for
	 * snapshot that we are removing */
	blob_get_snapshot_and_clone_entries(snapshot, &ctx->parent_snapshot_entry,
					    &snapshot_clone_entry);

	spdk_bs_open_blob(snapshot->bs, clone_entry->id, delete_snapshot_open_clone_cb, ctx);
}

static void
bs_delete_blob_finish(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	spdk_bs_sequence_t *seq = cb_arg;
	struct spdk_blob_list *snapshot_entry = NULL;
	uint32_t page_num;

	if (bserrno) {
		SPDK_ERRLOG("Failed to remove blob\n");
		bs_sequence_finish(seq, bserrno);
		return;
	}

	/* Remove snapshot from the list */
	snapshot_entry = bs_get_snapshot_entry(blob->bs, blob->id);
	if (snapshot_entry != NULL) {
		TAILQ_REMOVE(&blob->bs->snapshots, snapshot_entry, link);
		free(snapshot_entry);
	}

	page_num = bs_blobid_to_page(blob->id);
	spdk_bit_array_clear(blob->bs->used_blobids, page_num);
	blob->state = SPDK_BLOB_STATE_DIRTY;
	blob->active.num_pages = 0;
	blob_resize(blob, 0);

	blob_persist(seq, blob, bs_delete_persist_cpl, blob);
}

static int
bs_is_blob_deletable(struct spdk_blob *blob, bool *update_clone)
{
	struct spdk_blob_list *snapshot_entry = NULL;
	struct spdk_blob_list *clone_entry = NULL;
	struct spdk_blob *clone = NULL;
	bool has_one_clone = false;

	/* Check if this is a snapshot with clones */
	snapshot_entry = bs_get_snapshot_entry(blob->bs, blob->id);
	if (snapshot_entry != NULL) {
		if (snapshot_entry->clone_count > 1) {
			SPDK_ERRLOG("Cannot remove snapshot with more than one clone\n");
			return -EBUSY;
		} else if (snapshot_entry->clone_count == 1) {
			has_one_clone = true;
		}
	}

	/* Check if someone has this blob open (besides this delete context):
	 * - open_ref = 1 - only this context opened blob, so it is ok to remove it
	 * - open_ref <= 2 && has_one_clone = true - clone is holding snapshot
	 *	and that is ok, because we will update it accordingly */
	if (blob->open_ref <= 2 && has_one_clone) {
		clone_entry = TAILQ_FIRST(&snapshot_entry->clones);
		assert(clone_entry != NULL);
		clone = blob_lookup(blob->bs, clone_entry->id);

		if (blob->open_ref == 2 && clone == NULL) {
			/* Clone is closed and someone else opened this blob */
			SPDK_ERRLOG("Cannot remove snapshot because it is open\n");
			return -EBUSY;
		}

		*update_clone = true;
		return 0;
	}

	if (blob->open_ref > 1) {
		SPDK_ERRLOG("Cannot remove snapshot because it is open\n");
		return -EBUSY;
	}

	assert(has_one_clone == false);
	*update_clone = false;
	return 0;
}

static void
bs_delete_enomem_close_cpl(void *cb_arg, int bserrno)
{
	spdk_bs_sequence_t *seq = cb_arg;

	bs_sequence_finish(seq, -ENOMEM);
}

static void
bs_delete_open_cpl(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	spdk_bs_sequence_t *seq = cb_arg;
	struct delete_snapshot_ctx *ctx;
	bool update_clone = false;

	if (bserrno != 0) {
		bs_sequence_finish(seq, bserrno);
		return;
	}

	blob_verify_md_op(blob);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_blob_close(blob, bs_delete_enomem_close_cpl, seq);
		return;
	}

	ctx->snapshot = blob;
	ctx->cb_fn = bs_delete_blob_finish;
	ctx->cb_arg = seq;

	/* Check if blob can be removed and if it is a snapshot with clone on top of it */
	ctx->bserrno = bs_is_blob_deletable(blob, &update_clone);
	if (ctx->bserrno) {
		spdk_blob_close(blob, delete_blob_cleanup_finish, ctx);
		return;
	}

	if (blob->locked_operation_in_progress) {
		SPDK_DEBUGLOG(blob, "Cannot remove blob - another operation in progress\n");
		ctx->bserrno = -EBUSY;
		spdk_blob_close(blob, delete_blob_cleanup_finish, ctx);
		return;
	}

	blob->locked_operation_in_progress = true;

	/*
	 * Remove the blob from the blob_store list now, to ensure it does not
	 *  get returned after this point by blob_lookup().
	 */
	spdk_bit_array_clear(blob->bs->open_blobids, blob->id);
	RB_REMOVE(spdk_blob_tree, &blob->bs->open_blobs, blob);

	if (update_clone) {
		ctx->page = spdk_zmalloc(blob->bs->md_page_size, 0, NULL, SPDK_ENV_NUMA_ID_ANY,
					 SPDK_MALLOC_DMA);
		if (!ctx->page) {
			ctx->bserrno = -ENOMEM;
			spdk_blob_close(blob, delete_blob_cleanup_finish, ctx);
			return;
		}
		/* This blob is a snapshot with active clone - update clone first */
		update_clone_on_snapshot_deletion(blob, ctx);
	} else {
		/* This blob does not have any clones - just remove it */
		bs_blob_list_remove(blob);
		bs_delete_blob_finish(seq, blob, 0);
		free(ctx);
	}
}

/*
 * [한국어]
 * spdk_bs_delete_blob - blob을 영구 삭제 (공개 API)
 *
 * @bs:     blobstore
 * @blobid: 삭제할 blob (다른 곳에서 열려 있으면 -EBUSY)
 * @cb_fn:  완료 콜백
 *
 * 삭제 흐름:
 * 1) bs_open_blob → bs_delete_open_cpl
 * 2) bs_is_blob_deletable: snapshot이고 clone이 있는지 → 있다면 clone들을 부모로 갱신
 * 3) cluster/extent_page를 모두 free, used_blobids/md_pages 비트 OFF
 * 4) blob_persist로 디스크 메타 정리 → 사용자 콜백
 *
 * 실행 컨텍스트: md_thread (assert로 강제).
 */
void
spdk_bs_delete_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		    spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;

	SPDK_DEBUGLOG(blob, "Deleting blob 0x%" PRIx64 "\n", blobid);

	assert(spdk_get_thread() == bs->md_thread);        /* [한국어] md_thread 강제 */

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = bs_sequence_start_bs(bs->md_channel, &cpl);
	if (!seq) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	spdk_bs_open_blob(bs, blobid, bs_delete_open_cpl, seq); /* [한국어] open 후 bs_delete_open_cpl로 진행 */
}

/* END spdk_bs_delete_blob */

/* START spdk_bs_open_blob */

static void
bs_open_blob_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;
	struct spdk_blob *existing;

	if (bserrno != 0) {
		blob_free(blob);
		seq->cpl.u.blob_handle.blob = NULL;
		bs_sequence_finish(seq, bserrno);
		return;
	}

	existing = blob_lookup(blob->bs, blob->id);
	if (existing) {
		blob_free(blob);
		existing->open_ref++;
		seq->cpl.u.blob_handle.blob = existing;
		bs_sequence_finish(seq, 0);
		return;
	}

	blob->open_ref++;

	spdk_bit_array_set(blob->bs->open_blobids, blob->id);
	RB_INSERT(spdk_blob_tree, &blob->bs->open_blobs, blob);

	bs_sequence_finish(seq, bserrno);
}

static inline void
blob_open_opts_copy(const struct spdk_blob_open_opts *src, struct spdk_blob_open_opts *dst)
{
#define FIELD_OK(field) \
        offsetof(struct spdk_blob_open_opts, field) + sizeof(src->field) <= src->opts_size

#define SET_FIELD(field) \
        if (FIELD_OK(field)) { \
                dst->field = src->field; \
        } \

	SET_FIELD(clear_method);
	SET_FIELD(esnap_ctx);

	dst->opts_size = src->opts_size;

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_blob_open_opts) == 24, "Incorrect size");

#undef FIELD_OK
#undef SET_FIELD
}

static void
bs_open_blob(struct spdk_blob_store *bs,
	     spdk_blob_id blobid,
	     struct spdk_blob_open_opts *opts,
	     spdk_blob_op_with_handle_complete cb_fn,
	     void *cb_arg)
{
	struct spdk_blob		*blob;
	struct spdk_bs_cpl		cpl;
	struct spdk_blob_open_opts	opts_local;
	spdk_bs_sequence_t		*seq;
	uint32_t			page_num;

	SPDK_DEBUGLOG(blob, "Opening blob 0x%" PRIx64 "\n", blobid);
	assert(spdk_get_thread() == bs->md_thread);

	page_num = bs_blobid_to_page(blobid);
	if (spdk_bit_array_get(bs->used_blobids, page_num) == false) {
		/* Invalid blobid */
		cb_fn(cb_arg, NULL, -ENOENT);
		return;
	}

	blob = blob_lookup(bs, blobid);
	if (blob) {
		blob->open_ref++;
		cb_fn(cb_arg, blob, 0);
		return;
	}

	blob = blob_alloc(bs, blobid);
	if (!blob) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	spdk_blob_open_opts_init(&opts_local, sizeof(opts_local));
	if (opts) {
		blob_open_opts_copy(opts, &opts_local);
	}

	blob->clear_method = opts_local.clear_method;

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_HANDLE;
	cpl.u.blob_handle.cb_fn = cb_fn;
	cpl.u.blob_handle.cb_arg = cb_arg;
	cpl.u.blob_handle.blob = blob;
	cpl.u.blob_handle.esnap_ctx = opts_local.esnap_ctx;

	seq = bs_sequence_start_bs(bs->md_channel, &cpl);
	if (!seq) {
		blob_free(blob);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	blob_load(seq, blob, bs_open_blob_cpl, blob);
}

/*
 * [한국어]
 * spdk_bs_open_blob - blobid로 blob을 열어 핸들 획득 (공개 API)
 *
 * @bs:     blobstore
 * @blobid: 열고자 하는 blob id (used_blobids에 등록되어 있어야 함)
 * @cb_fn:  완료 콜백 (cb_arg, struct spdk_blob*, bserrno)
 *
 * 이미 열려 있는 blob이면 ref count만 증가시키고 즉시 콜백 (open이 idempotent).
 * 처음 여는 경우엔 blob_alloc → blob_load(메타 read) → bs_open_blob_cpl 체인으로
 * RB tree에 등록한다.
 *
 * 실행 컨텍스트: md_thread (assert로 강제).
 */
void
spdk_bs_open_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		  spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	bs_open_blob(bs, blobid, NULL, cb_fn, cb_arg);     /* [한국어] opts NULL = 디폴트 */
}

/*
 * [한국어]
 * spdk_bs_open_blob_ext - 옵션과 함께 blob 열기 (공개 API)
 *
 * @opts: clear_method, esnap_ctx 등. esnap clone을 열 때는 esnap_ctx로 부모 bs_dev 매핑.
 */
void
spdk_bs_open_blob_ext(struct spdk_blob_store *bs, spdk_blob_id blobid,
		      struct spdk_blob_open_opts *opts, spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	bs_open_blob(bs, blobid, opts, cb_fn, cb_arg);
}

/* END spdk_bs_open_blob */

/* START spdk_blob_set_read_only */
/*
 * [한국어]
 * spdk_blob_set_read_only - blob을 read-only 플래그로 표시 (공개 API)
 *
 * 이후 sync_md 호출에서 디스크에 영속화되며, sync 완료 콜백(blob_sync_md_cpl)에서
 * data_ro=true 가 적용된다. set 후엔 write 시도가 실패한다.
 */
int
spdk_blob_set_read_only(struct spdk_blob *blob)
{
	blob_verify_md_op(blob);

	blob->data_ro_flags |= SPDK_BLOB_READ_ONLY;       /* [한국어] 영속 플래그 비트 ON */

	blob->state = SPDK_BLOB_STATE_DIRTY;               /* [한국어] sync 대상 */
	return 0;
}
/* END spdk_blob_set_read_only */

/* START spdk_blob_sync_md */

static void
blob_sync_md_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;

	if (bserrno == 0 && (blob->data_ro_flags & SPDK_BLOB_READ_ONLY)) {
		blob->data_ro = true;
		blob->md_ro = true;
	}

	bs_sequence_finish(seq, bserrno);
}

static void
blob_sync_md(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = bs_sequence_start_bs(blob->bs->md_channel, &cpl);
	if (!seq) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	blob_persist(seq, blob, blob_sync_md_cpl, blob);
}

/*
 * [한국어]
 * spdk_blob_sync_md - blob의 in-memory 메타를 디스크에 영속화 (공개 API)
 *
 * @blob:  대상 blob
 * @cb_fn: 완료 콜백
 *
 * resize/xattr 변경 등 메타 변경 후에 호출되어 변경 내용을 superblock의 md page에 기록한다.
 * md_ro blob은 sync 자체가 의미 없음 — 즉시 0 반환.
 *
 * 내부적으로 blob_persist를 호출 — 이는 dirty/clean 상태 차이를 비교해 변경된 페이지만
 * 디스크에 쓰는 핵심 영속화 함수.
 */
void
spdk_blob_sync_md(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	blob_verify_md_op(blob);

	SPDK_DEBUGLOG(blob, "Syncing blob 0x%" PRIx64 "\n", blob->id);

	if (blob->md_ro) {
		assert(blob->state == SPDK_BLOB_STATE_CLEAN);
		cb_fn(cb_arg, 0);
		return;
	}

	blob_sync_md(blob, cb_fn, cb_arg);                 /* [한국어] 실제 비동기 sync 시작 */
}

/* END spdk_blob_sync_md */

struct spdk_blob_cluster_op_ctx {
	struct spdk_thread	*thread;
	struct spdk_blob	*blob;
	uint32_t		cluster_num;	/* cluster index in blob */
	uint32_t		cluster;	/* cluster on disk */
	uint32_t		extent_page;	/* extent page on disk */
	struct spdk_blob_md_page *page; /* preallocated extent page */
	int			rc;
	spdk_blob_op_complete	cb_fn;
	void			*cb_arg;
};

static void
blob_op_cluster_msg_cpl(void *arg)
{
	struct spdk_blob_cluster_op_ctx *ctx = arg;

	ctx->cb_fn(ctx->cb_arg, ctx->rc);
	free(ctx);
}

static void
blob_op_cluster_msg_cb(void *arg, int bserrno)
{
	struct spdk_blob_cluster_op_ctx *ctx = arg;

	ctx->rc = bserrno;
	spdk_thread_send_msg(ctx->thread, blob_op_cluster_msg_cpl, ctx);
}

static void
blob_insert_new_ep_cb(void *arg, int bserrno)
{
	struct spdk_blob_cluster_op_ctx *ctx = arg;
	uint32_t *extent_page;

	extent_page = bs_cluster_to_extent_page(ctx->blob, ctx->cluster_num);
	*extent_page = ctx->extent_page;
	ctx->blob->state = SPDK_BLOB_STATE_DIRTY;
	blob_sync_md(ctx->blob, blob_op_cluster_msg_cb, ctx);
}

struct spdk_blob_write_extent_page_ctx {
	struct spdk_blob_store		*bs;

	uint32_t			extent;
	struct spdk_blob_md_page	*page;
};

static void
blob_free_cluster_msg_cb(void *arg, int bserrno)
{
	struct spdk_blob_cluster_op_ctx *ctx = arg;

	spdk_spin_lock(&ctx->blob->bs->used_lock);
	bs_release_cluster(ctx->blob->bs, ctx->cluster);
	spdk_spin_unlock(&ctx->blob->bs->used_lock);

	ctx->rc = bserrno;
	spdk_thread_send_msg(ctx->thread, blob_op_cluster_msg_cpl, ctx);
}

static void
blob_free_cluster_update_ep_cb(void *arg, int bserrno)
{
	struct spdk_blob_cluster_op_ctx *ctx = arg;

	if (bserrno != 0 || ctx->blob->bs->clean == 0) {
		blob_free_cluster_msg_cb(ctx, bserrno);
		return;
	}

	ctx->blob->state = SPDK_BLOB_STATE_DIRTY;
	blob_sync_md(ctx->blob, blob_free_cluster_msg_cb, ctx);
}

static void
blob_persist_extent_page_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_write_extent_page_ctx *ctx = cb_arg;

	free(ctx);
	bs_sequence_finish(seq, bserrno);
}

static void
blob_write_extent_page_ready(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_write_extent_page_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		blob_persist_extent_page_cpl(seq, ctx, bserrno);
		return;
	}
	bs_sequence_write_dev(seq, ctx->page, bs_md_page_to_lba(ctx->bs, ctx->extent),
			      bs_byte_to_lba(ctx->bs, ctx->bs->md_page_size),
			      blob_persist_extent_page_cpl, ctx);
}

static void
blob_write_extent_page(struct spdk_blob *blob, uint32_t extent, uint64_t cluster_num,
		       struct spdk_blob_md_page *page, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_write_extent_page_ctx	*ctx;
	spdk_bs_sequence_t			*seq;
	struct spdk_bs_cpl			cpl;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	ctx->bs = blob->bs;
	ctx->extent = extent;
	ctx->page = page;
	memset(ctx->page, 0, blob->bs->md_page_size);

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = bs_sequence_start_bs(blob->bs->md_channel, &cpl);
	if (!seq) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	assert(page);
	page->next = SPDK_INVALID_MD_PAGE;
	page->id = blob->id;
	page->sequence_num = 0;

	blob_serialize_extent_page(blob, cluster_num, page);

	page->crc = blob_md_page_calc_crc(page);

	assert(spdk_bit_array_get(blob->bs->used_md_pages, extent) == true);

	bs_mark_dirty(seq, blob->bs, blob_write_extent_page_ready, ctx);
}

static void
blob_insert_cluster_msg(void *arg)
{
	struct spdk_blob_cluster_op_ctx *ctx = arg;
	uint32_t *extent_page;

	ctx->rc = blob_insert_cluster(ctx->blob, ctx->cluster_num, ctx->cluster);
	if (ctx->rc != 0) {
		spdk_thread_send_msg(ctx->thread, blob_op_cluster_msg_cpl, ctx);
		return;
	}

	if (ctx->blob->use_extent_table == false) {
		/* Extent table is not used, proceed with sync of md that will only use extents_rle. */
		ctx->blob->state = SPDK_BLOB_STATE_DIRTY;
		blob_sync_md(ctx->blob, blob_op_cluster_msg_cb, ctx);
		return;
	}

	extent_page = bs_cluster_to_extent_page(ctx->blob, ctx->cluster_num);
	if (*extent_page == 0) {
		/* Extent page requires allocation.
		 * It was already claimed in the used_md_pages map and placed in ctx. */
		assert(ctx->extent_page != 0);
		assert(spdk_bit_array_get(ctx->blob->bs->used_md_pages, ctx->extent_page) == true);
		blob_write_extent_page(ctx->blob, ctx->extent_page, ctx->cluster_num, ctx->page,
				       blob_insert_new_ep_cb, ctx);
	} else {
		/* It is possible for original thread to allocate extent page for
		 * different cluster in the same extent page. In such case proceed with
		 * updating the existing extent page, but release the additional one. */
		if (ctx->extent_page != 0) {
			spdk_spin_lock(&ctx->blob->bs->used_lock);
			assert(spdk_bit_array_get(ctx->blob->bs->used_md_pages, ctx->extent_page) == true);
			bs_release_md_page(ctx->blob->bs, ctx->extent_page);
			spdk_spin_unlock(&ctx->blob->bs->used_lock);
			ctx->extent_page = 0;
		}
		/* Extent page already allocated.
		 * Every cluster allocation, requires just an update of single extent page. */
		blob_write_extent_page(ctx->blob, *extent_page, ctx->cluster_num, ctx->page,
				       blob_op_cluster_msg_cb, ctx);
	}
}

static void
blob_insert_cluster_on_md_thread(struct spdk_blob *blob, uint32_t cluster_num,
				 uint64_t cluster, uint32_t extent_page, struct spdk_blob_md_page *page,
				 spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_cluster_op_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->thread = spdk_get_thread();
	ctx->blob = blob;
	ctx->cluster_num = cluster_num;
	ctx->cluster = cluster;
	ctx->extent_page = extent_page;
	ctx->page = page;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_thread_send_msg(blob->bs->md_thread, blob_insert_cluster_msg, ctx);
}

static void
blob_free_cluster_msg(void *arg)
{
	struct spdk_blob_cluster_op_ctx *ctx = arg;
	uint32_t *extent_page;

	ctx->cluster = bs_lba_to_cluster(ctx->blob->bs, ctx->blob->active.clusters[ctx->cluster_num]);

	/* There were concurrent unmaps to the same cluster, only release the cluster on the first one */
	if (ctx->cluster == 0) {
		blob_op_cluster_msg_cb(ctx, 0);
		return;
	}

	ctx->blob->active.clusters[ctx->cluster_num] = 0;
	if (ctx->cluster != 0) {
		ctx->blob->active.num_allocated_clusters--;
	}

	if (ctx->blob->use_extent_table == false) {
		/* Extent table is not used, proceed with sync of md that will only use extents_rle. */
		spdk_spin_lock(&ctx->blob->bs->used_lock);
		bs_release_cluster(ctx->blob->bs, ctx->cluster);
		spdk_spin_unlock(&ctx->blob->bs->used_lock);
		ctx->blob->state = SPDK_BLOB_STATE_DIRTY;
		blob_sync_md(ctx->blob, blob_op_cluster_msg_cb, ctx);
		return;
	}

	extent_page = bs_cluster_to_extent_page(ctx->blob, ctx->cluster_num);

	/* There shouldn't be parallel release operations on same cluster */
	assert(*extent_page == ctx->extent_page);

	blob_write_extent_page(ctx->blob, *extent_page, ctx->cluster_num, ctx->page,
			       blob_free_cluster_update_ep_cb, ctx);
}


static void
blob_free_cluster_on_md_thread(struct spdk_blob *blob, uint32_t cluster_num, uint32_t extent_page,
			       struct spdk_blob_md_page *page, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_cluster_op_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->thread = spdk_get_thread();
	ctx->blob = blob;
	ctx->cluster_num = cluster_num;
	ctx->extent_page = extent_page;
	ctx->page = page;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_thread_send_msg(blob->bs->md_thread, blob_free_cluster_msg, ctx);
}

/* START spdk_blob_close */

static void
blob_close_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;

	if (bserrno == 0) {
		blob->open_ref--;
		if (blob->open_ref == 0) {
			/*
			 * Blobs with active.num_pages == 0 are deleted blobs.
			 *  these blobs are removed from the blob_store list
			 *  when the deletion process starts - so don't try to
			 *  remove them again.
			 */
			if (blob->active.num_pages > 0) {
				spdk_bit_array_clear(blob->bs->open_blobids, blob->id);
				RB_REMOVE(spdk_blob_tree, &blob->bs->open_blobs, blob);
			}
			blob_free(blob);
		}
	}

	bs_sequence_finish(seq, bserrno);
}

static void
blob_close_esnap_done(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	spdk_bs_sequence_t	*seq = cb_arg;

	if (bserrno != 0) {
		SPDK_DEBUGLOG(blob_esnap, "blob 0x%" PRIx64 ": close failed with error %d\n",
			      blob->id, bserrno);
		bs_sequence_finish(seq, bserrno);
		return;
	}

	SPDK_DEBUGLOG(blob_esnap, "blob 0x%" PRIx64 ": closed, syncing metadata on thread %s\n",
		      blob->id, spdk_thread_get_name(spdk_get_thread()));

	/* Sync metadata */
	blob_persist(seq, blob, blob_close_cpl, blob);
}

/*
 * [한국어]
 * spdk_blob_close - blob ref 감소 / 마지막 닫기 시 메타 sync 후 자원 해제 (공개 API)
 *
 * @blob:   닫을 blob 핸들 (open_ref > 0이어야 함)
 * @cb_fn:  완료 콜백
 *
 * - open_ref == 0 → -EBADF (이미 닫힌 핸들).
 * - 마지막 close (이후 ref 0) + esnap clone:
 *     먼저 esnap 채널들을 모두 파괴한 뒤 (blob_esnap_destroy_bs_dev_channels) 메타 sync.
 * - 그 외: 곧바로 blob_persist → blob_close_cpl → ref 감소·필요 시 RB tree에서 제거.
 *
 * 실행 컨텍스트: md_thread (blob_verify_md_op로 강제).
 *
 * 호출 체인:
 *   spdk_blob_close → bs_sequence_start_bs
 *     → (esnap?) blob_esnap_destroy_bs_dev_channels → blob_close_esnap_done → blob_persist
 *     → blob_close_cpl (ref 감소, RB remove)
 */
void
spdk_blob_close(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;

	blob_verify_md_op(blob);                           /* [한국어] md_thread 강제 */

	SPDK_DEBUGLOG(blob, "Closing blob 0x%" PRIx64 "\n", blob->id);

	if (blob->open_ref == 0) {                         /* [한국어] 이미 닫힌 blob — bad fd */
		cb_fn(cb_arg, -EBADF);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = bs_sequence_start_bs(blob->bs->md_channel, &cpl);
	if (!seq) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* [한국어] 마지막 ref + esnap → 채널 파괴 후 sync. esnap 채널이 아직 살아 있으면
	 * blob 메타가 disk에 가기 전에 backing dev IO가 진행 중일 수 있어 위험. */
	if (blob->open_ref == 1 && blob_is_esnap_clone(blob)) {
		blob_esnap_destroy_bs_dev_channels(blob, false, blob_close_esnap_done, seq);
		return;
	}

	/* Sync metadata */
	blob_persist(seq, blob, blob_close_cpl, blob);     /* [한국어] 일반 close 경로 */
}

/* END spdk_blob_close */

/*
 * [한국어]
 * spdk_bs_alloc_io_channel - blobstore에 대한 IO 채널 획득 (공개 API)
 *
 * 내부적으로 spdk_get_io_channel(bs) — SPDK io channel 인프라가 thread-local 캐시에서
 * 채널을 만들어 준다. 채널은 thread 단위로 1개씩 caching된다.
 */
struct spdk_io_channel *spdk_bs_alloc_io_channel(struct spdk_blob_store *bs)
{
	return spdk_get_io_channel(bs);
}

/*
 * [한국어]
 * spdk_bs_free_io_channel - 위에서 잡은 IO 채널 해제 (공개 API)
 *
 * 채널이 esnap 채널 캐시(RB tree)를 가지고 있을 수 있으므로 먼저 그것들을 모두 파괴한 후
 * spdk_put_io_channel로 ref 감소.
 */
void
spdk_bs_free_io_channel(struct spdk_io_channel *channel)
{
	blob_esnap_destroy_bs_channel(spdk_io_channel_get_ctx(channel)); /* [한국어] esnap 채널 정리 */
	spdk_put_io_channel(channel);                      /* [한국어] ref 감소 (0이면 진짜 해제) */
}

/*
 * [한국어]
 * spdk_blob_io_unmap - blob 영역 [offset, offset+length) io_unit를 invalid화 (공개 API)
 *
 * @offset/length: io_unit 단위 (cluster 아님 — io_unit은 블록 단위 가장 작은 IO).
 *
 * 경계 cluster를 통째로 unmap할 수 있으면 cluster를 free 시키며 그 외 영역은 backing
 * dev에 unmap (NVMe Deallocate / SCSI UNMAP)을 위임한다.
 */
void
spdk_blob_io_unmap(struct spdk_blob *blob, struct spdk_io_channel *channel,
		   uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	blob_request_submit_op(blob, channel, NULL, offset, length, cb_fn, cb_arg,
			       SPDK_BLOB_UNMAP);
}

/*
 * [한국어]
 * spdk_blob_io_write_zeroes - blob 영역 [offset, offset+length)을 0으로 채움 (공개 API)
 *
 * write_zeroes는 NVMe Write Zeroes 명령(payload를 보내지 않는 효율적 방법)으로 처리.
 * thin blob의 경우 cluster가 아직 할당되지 않은 영역은 그대로 두고, 할당된 영역만 0 처리.
 */
void
spdk_blob_io_write_zeroes(struct spdk_blob *blob, struct spdk_io_channel *channel,
			  uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	blob_request_submit_op(blob, channel, NULL, offset, length, cb_fn, cb_arg,
			       SPDK_BLOB_WRITE_ZEROES);
}

/*
 * [한국어]
 * spdk_blob_io_write - 단일 payload 버퍼 → blob의 [offset, length) 영역에 비동기 쓰기
 *
 * @payload: DMA-가능 버퍼 (io_unit 정렬). 사용자가 spdk_dma_zmalloc 등으로 미리 마련해야 함.
 * @offset/length: io_unit 단위.
 *
 * 동작 단계 (blob_request_submit_op 내부):
 * 1) IO를 cluster 경계로 split 가능하면 split.
 * 2) cluster가 비어 있고 thin이면 새 cluster 할당 (md_thread로 우회).
 * 3) snapshot/clone이라면 COW: 부모에서 cluster 전체를 read 후 새 cluster에 merge write.
 * 4) backing bs_dev->writev 발사 → 완료 시 사용자 cb_fn.
 */
void
spdk_blob_io_write(struct spdk_blob *blob, struct spdk_io_channel *channel,
		   void *payload, uint64_t offset, uint64_t length,
		   spdk_blob_op_complete cb_fn, void *cb_arg)
{
	blob_request_submit_op(blob, channel, payload, offset, length, cb_fn, cb_arg,
			       SPDK_BLOB_WRITE);
}

/*
 * [한국어]
 * spdk_blob_io_read - blob의 [offset, length) 영역을 payload로 읽기 (공개 API)
 *
 * thin blob에서 매핑되지 않은 영역은:
 *   - 부모 snapshot이 있으면 부모 bs_dev에서 read
 *   - 부모도 없거나 esnap이고 부모 없음 → zero-fill (zeroes_bs_dev가 자동 처리)
 */
void
spdk_blob_io_read(struct spdk_blob *blob, struct spdk_io_channel *channel,
		  void *payload, uint64_t offset, uint64_t length,
		  spdk_blob_op_complete cb_fn, void *cb_arg)
{
	blob_request_submit_op(blob, channel, payload, offset, length, cb_fn, cb_arg,
			       SPDK_BLOB_READ);
}

/*
 * [한국어]
 * spdk_blob_io_writev - iovec(scatter) 입력으로 blob 쓰기 (공개 API)
 *
 * blob_request_submit_rw_iov 가 read=false 로 호출되어 split/COW/매핑 처리를 거친다.
 */
void
spdk_blob_io_writev(struct spdk_blob *blob, struct spdk_io_channel *channel,
		    struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
		    spdk_blob_op_complete cb_fn, void *cb_arg)
{
	blob_request_submit_rw_iov(blob, channel, iov, iovcnt, offset, length, cb_fn, cb_arg, false, NULL);
}

/*
 * [한국어]
 * spdk_blob_io_readv - iovec(gather) 입력으로 blob 읽기 (공개 API)
 *
 * read=true 분기 — payload split 없는 직접 매핑이 가능하면 곧장 backing dev->readv.
 */
void
spdk_blob_io_readv(struct spdk_blob *blob, struct spdk_io_channel *channel,
		   struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
		   spdk_blob_op_complete cb_fn, void *cb_arg)
{
	blob_request_submit_rw_iov(blob, channel, iov, iovcnt, offset, length, cb_fn, cb_arg, true, NULL);
}

/*
 * [한국어]
 * spdk_blob_io_writev_ext - 메타옵션 동반 writev (공개 API)
 *
 * @io_opts: ext_io_opts(메타데이터/encryption ctx 등). 일부 backing bs_dev이 사용한다.
 */
void
spdk_blob_io_writev_ext(struct spdk_blob *blob, struct spdk_io_channel *channel,
			struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			spdk_blob_op_complete cb_fn, void *cb_arg, struct spdk_blob_ext_io_opts *io_opts)
{
	blob_request_submit_rw_iov(blob, channel, iov, iovcnt, offset, length, cb_fn, cb_arg, false,
				   io_opts);
}

/*
 * [한국어]
 * spdk_blob_io_readv_ext - 메타옵션 동반 readv (공개 API)
 */
void
spdk_blob_io_readv_ext(struct spdk_blob *blob, struct spdk_io_channel *channel,
		       struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
		       spdk_blob_op_complete cb_fn, void *cb_arg, struct spdk_blob_ext_io_opts *io_opts)
{
	blob_request_submit_rw_iov(blob, channel, iov, iovcnt, offset, length, cb_fn, cb_arg, true,
				   io_opts);
}

struct spdk_bs_iter_ctx {
	int64_t page_num;
	struct spdk_blob_store *bs;

	spdk_blob_op_with_handle_complete cb_fn;
	void *cb_arg;
};

static void
bs_iter_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	struct spdk_bs_iter_ctx *ctx = cb_arg;
	struct spdk_blob_store *bs = ctx->bs;
	spdk_blob_id id;

	if (bserrno == 0) {
		ctx->cb_fn(ctx->cb_arg, _blob, bserrno);
		free(ctx);
		return;
	}

	ctx->page_num++;
	ctx->page_num = spdk_bit_array_find_first_set(bs->used_blobids, ctx->page_num);
	if (ctx->page_num >= spdk_bit_array_capacity(bs->used_blobids)) {
		ctx->cb_fn(ctx->cb_arg, NULL, -ENOENT);
		free(ctx);
		return;
	}

	id = bs_page_to_blobid(ctx->page_num);

	spdk_bs_open_blob(bs, id, bs_iter_cpl, ctx);
}

/*
 * [한국어]
 * spdk_bs_iter_first - blobstore의 첫 번째 blob을 열어 콜백으로 반환 (공개 API)
 *
 * @bs:    blobstore
 * @cb_fn: 각 blob에 대해 호출 — (cb_arg, blob, 0=정상). 더 없으면 (NULL, -ENOENT).
 *
 * 사용 패턴 (모든 blob 순회):
 *   spdk_bs_iter_first(bs, my_cb, NULL);
 *   void my_cb(void *_, struct spdk_blob *b, int err) {
 *     if (err) return;
 *     // b 사용 후
 *     spdk_bs_iter_next(bs, b, my_cb, NULL);
 *   }
 *
 * 동작: page_num=-1 부터 시작해 used_blobids 비트맵에서 다음 set bit를 찾아 그 blob을 open.
 */
void
spdk_bs_iter_first(struct spdk_blob_store *bs,
		   spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_iter_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	ctx->page_num = -1;                                /* [한국어] iter_cpl이 +1 증가시켜 0부터 검색 */
	ctx->bs = bs;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	bs_iter_cpl(ctx, NULL, -1);                        /* [한국어] err=-1로 초기 진입 → 다음 검색 분기 */
}

/*
 * [한국어]
 * bs_iter_close_cpl - 이전 blob close 후 다음 검색 시작 (static helper)
 */
static void
bs_iter_close_cpl(void *cb_arg, int bserrno)
{
	struct spdk_bs_iter_ctx *ctx = cb_arg;

	bs_iter_cpl(ctx, NULL, -1);                        /* [한국어] err=-1 = "다음으로" 신호 */
}

/*
 * [한국어]
 * spdk_bs_iter_next - 현재 blob을 close하고 다음 blob을 열어 콜백 (공개 API)
 *
 * @blob:  현재 사용 중이던 blob (close됨)
 *
 * 호출 시 현재 blob의 페이지 번호를 기준으로 used_blobids에서 다음 set bit를 찾아
 * 새 ctx로 콜백 체인을 이어간다. 핵심은 "iter는 동시에 한 blob만 잡는다"는 protocol.
 */
void
spdk_bs_iter_next(struct spdk_blob_store *bs, struct spdk_blob *blob,
		  spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_iter_ctx *ctx;

	assert(blob != NULL);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	ctx->page_num = bs_blobid_to_page(blob->id);
	ctx->bs = bs;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	/* Close the existing blob */
	spdk_blob_close(blob, bs_iter_close_cpl, ctx);     /* [한국어] close 완료 후 다음 검색 */
}

static int
blob_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
	       uint16_t value_len, bool internal)
{
	struct spdk_xattr_tailq *xattrs;
	struct spdk_xattr	*xattr;
	size_t			desc_size;
	void			*tmp;

	blob_verify_md_op(blob);

	if (blob->md_ro) {
		return -EPERM;
	}

	desc_size = sizeof(struct spdk_blob_md_descriptor_xattr) + strlen(name) + value_len;
	if (desc_size > SPDK_BS_MAX_DESC_SIZE) {
		SPDK_DEBUGLOG(blob, "Xattr '%s' of size %zu does not fix into single page %zu\n", name,
			      desc_size, SPDK_BS_MAX_DESC_SIZE);
		return -ENOMEM;
	}

	if (internal) {
		xattrs = &blob->xattrs_internal;
		blob->invalid_flags |= SPDK_BLOB_INTERNAL_XATTR;
	} else {
		xattrs = &blob->xattrs;
	}

	TAILQ_FOREACH(xattr, xattrs, link) {
		if (!strcmp(name, xattr->name)) {
			tmp = malloc(value_len);
			if (!tmp) {
				return -ENOMEM;
			}

			free(xattr->value);
			xattr->value_len = value_len;
			xattr->value = tmp;
			memcpy(xattr->value, value, value_len);

			blob->state = SPDK_BLOB_STATE_DIRTY;

			return 0;
		}
	}

	xattr = calloc(1, sizeof(*xattr));
	if (!xattr) {
		return -ENOMEM;
	}

	xattr->name = strdup(name);
	if (!xattr->name) {
		free(xattr);
		return -ENOMEM;
	}

	xattr->value_len = value_len;
	xattr->value = malloc(value_len);
	if (!xattr->value) {
		free(xattr->name);
		free(xattr);
		return -ENOMEM;
	}
	memcpy(xattr->value, value, value_len);
	TAILQ_INSERT_TAIL(xattrs, xattr, link);

	blob->state = SPDK_BLOB_STATE_DIRTY;

	return 0;
}

/*
 * [한국어]
 * spdk_blob_set_xattr - 사용자 xattr 설정 (또는 갱신) (공개 API)
 *
 * @name:      xattr 이름 (NULL 종료 문자열, 디스크에 그대로 저장됨)
 * @value:     값 버퍼 (내부에서 복사됨)
 * @value_len: 값 길이 (uint16_t — 디스크 메타 포맷 한계)
 *
 * blob의 state를 DIRTY로 만들고 in-memory xattr 리스트에 삽입. 영속화는 따로 sync_md 필요.
 */
int
spdk_blob_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
		    uint16_t value_len)
{
	return blob_set_xattr(blob, name, value, value_len, false);
}

static int
blob_remove_xattr(struct spdk_blob *blob, const char *name, bool internal)
{
	struct spdk_xattr_tailq *xattrs;
	struct spdk_xattr	*xattr;

	blob_verify_md_op(blob);

	if (blob->md_ro) {
		return -EPERM;
	}
	xattrs = internal ? &blob->xattrs_internal : &blob->xattrs;

	TAILQ_FOREACH(xattr, xattrs, link) {
		if (!strcmp(name, xattr->name)) {
			TAILQ_REMOVE(xattrs, xattr, link);
			free(xattr->value);
			free(xattr->name);
			free(xattr);

			if (internal && TAILQ_EMPTY(&blob->xattrs_internal)) {
				blob->invalid_flags &= ~SPDK_BLOB_INTERNAL_XATTR;
			}
			blob->state = SPDK_BLOB_STATE_DIRTY;

			return 0;
		}
	}

	return -ENOENT;
}

/*
 * [한국어]
 * spdk_blob_remove_xattr - 사용자 xattr 제거 (공개 API)
 *
 * 내부 xattr (SPDK 자체 메타)은 제거 불가 — internal=false 고정.
 */
int
spdk_blob_remove_xattr(struct spdk_blob *blob, const char *name)
{
	return blob_remove_xattr(blob, name, false);
}

static int
blob_get_xattr_value(struct spdk_blob *blob, const char *name,
		     const void **value, size_t *value_len, bool internal)
{
	struct spdk_xattr	*xattr;
	struct spdk_xattr_tailq *xattrs;

	xattrs = internal ? &blob->xattrs_internal : &blob->xattrs;

	TAILQ_FOREACH(xattr, xattrs, link) {
		if (!strcmp(name, xattr->name)) {
			*value = xattr->value;
			*value_len = xattr->value_len;
			return 0;
		}
	}
	return -ENOENT;
}

/*
 * [한국어]
 * spdk_blob_get_xattr_value - 사용자 xattr 값 조회 (공개 API)
 *
 * @value:     (out) 내부 버퍼 포인터 (free 금지). blob 수명 동안 유효.
 * @value_len: (out) 값 길이
 */
int
spdk_blob_get_xattr_value(struct spdk_blob *blob, const char *name,
			  const void **value, size_t *value_len)
{
	blob_verify_md_op(blob);

	return blob_get_xattr_value(blob, name, value, value_len, false);
}

struct spdk_xattr_names {
	uint32_t	count;
	const char	*names[0];
};

static int
blob_get_xattr_names(struct spdk_xattr_tailq *xattrs, struct spdk_xattr_names **names)
{
	struct spdk_xattr	*xattr;
	int			count = 0;

	TAILQ_FOREACH(xattr, xattrs, link) {
		count++;
	}

	*names = calloc(1, sizeof(struct spdk_xattr_names) + count * sizeof(char *));
	if (*names == NULL) {
		return -ENOMEM;
	}

	TAILQ_FOREACH(xattr, xattrs, link) {
		(*names)->names[(*names)->count++] = xattr->name;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_blob_get_xattr_names - blob의 사용자 xattr 이름 목록 획득 (공개 API)
 *
 * @names: (out) 새로 할당된 spdk_xattr_names — 사용 후 spdk_xattr_names_free로 해제 필수.
 *
 * 동적 할당된 names 배열은 blob의 xattr 포인터를 그대로 가리키므로 blob이 close되지
 * 않은 동안에만 유효.
 */
int
spdk_blob_get_xattr_names(struct spdk_blob *blob, struct spdk_xattr_names **names)
{
	blob_verify_md_op(blob);

	return blob_get_xattr_names(&blob->xattrs, names);
}

/* [한국어] xattr 이름 배열의 항목 수 반환 (공개 API). */
uint32_t
spdk_xattr_names_get_count(struct spdk_xattr_names *names)
{
	assert(names != NULL);

	return names->count;
}

/* [한국어] index번째 이름 반환 (범위 초과 시 NULL) (공개 API). */
const char *
spdk_xattr_names_get_name(struct spdk_xattr_names *names, uint32_t index)
{
	if (index >= names->count) {
		return NULL;
	}

	return names->names[index];
}

/* [한국어] spdk_blob_get_xattr_names 결과 해제 (공개 API). */
void
spdk_xattr_names_free(struct spdk_xattr_names *names)
{
	free(names);
}

/* [한국어] blobstore 식별 태그(bstype) 반환 — init 시 지정한 16바이트 사용자 식별자. */
struct spdk_bs_type
spdk_bs_get_bstype(struct spdk_blob_store *bs)
{
	return bs->bstype;
}

/* [한국어] bstype 변경 — 다음 unload 시 super에 영속화됨. */
void
spdk_bs_set_bstype(struct spdk_blob_store *bs, struct spdk_bs_type bstype)
{
	memcpy(&bs->bstype, &bstype, sizeof(bstype));
}

/*
 * [한국어]
 * spdk_blob_is_read_only - 데이터 또는 메타 RO 여부 (공개 API)
 *
 * snapshot blob은 둘 다 true. read-only로 set 된 blob도 true.
 */
bool
spdk_blob_is_read_only(struct spdk_blob *blob)
{
	assert(blob != NULL);
	return (blob->data_ro || blob->md_ro);
}

/*
 * [한국어]
 * spdk_blob_is_snapshot - blob이 snapshot 인지 검사 (공개 API)
 *
 * snapshots 리스트에 등록되어 있으면 snapshot. 등록은 spdk_bs_create_snapshot 흐름에서.
 */
bool
spdk_blob_is_snapshot(struct spdk_blob *blob)
{
	struct spdk_blob_list *snapshot_entry;

	assert(blob != NULL);

	snapshot_entry = bs_get_snapshot_entry(blob->bs, blob->id);
	if (snapshot_entry == NULL) {
		return false;
	}

	return true;
}

/*
 * [한국어]
 * spdk_blob_is_clone - blob이 다른 blob의 clone (esnap 제외) 인지 (공개 API)
 *
 * parent_id가 유효하고 EXTERNAL_SNAPSHOT이 아니면 일반 clone. clone은 항상 thin이다.
 */
bool
spdk_blob_is_clone(struct spdk_blob *blob)
{
	assert(blob != NULL);

	if (blob->parent_id != SPDK_BLOBID_INVALID &&
	    blob->parent_id != SPDK_BLOBID_EXTERNAL_SNAPSHOT) {
		assert(spdk_blob_is_thin_provisioned(blob));
		return true;
	}

	return false;
}

/* [한국어] thin provision 플래그 검사 — 미할당 cluster가 zero/parent에서 채워짐. */
bool
spdk_blob_is_thin_provisioned(struct spdk_blob *blob)
{
	assert(blob != NULL);
	return !!(blob->invalid_flags & SPDK_BLOB_THIN_PROV);
}

/* [한국어] esnap clone(외부 snapshot이 부모) 여부 — invalid_flags의 EXTERNAL_SNAPSHOT 비트. */
bool
spdk_blob_is_esnap_clone(const struct spdk_blob *blob)
{
	return blob_is_esnap_clone(blob);
}

static void
blob_update_clear_method(struct spdk_blob *blob)
{
	enum blob_clear_method stored_cm;

	assert(blob != NULL);

	/* If BLOB_CLEAR_WITH_DEFAULT was passed in, use the setting stored
	 * in metadata previously.  If something other than the default was
	 * specified, ignore stored value and used what was passed in.
	 */
	stored_cm = ((blob->md_ro_flags & SPDK_BLOB_CLEAR_METHOD) >> SPDK_BLOB_CLEAR_METHOD_SHIFT);

	if (blob->clear_method == BLOB_CLEAR_WITH_DEFAULT) {
		blob->clear_method = stored_cm;
	} else if (blob->clear_method != stored_cm) {
		SPDK_WARNLOG("Using passed in clear method 0x%x instead of stored value of 0x%x\n",
			     blob->clear_method, stored_cm);
	}
}

/*
 * [한국어]
 * spdk_blob_get_parent_snapshot - blob_id의 직속 snapshot 부모 id를 반환 (공개 API)
 *
 * snapshots 리스트 전체를 순회하며 각 snapshot의 clones 리스트에서 blob_id를 찾는다.
 * 부모가 없거나 esnap clone이면 SPDK_BLOBID_INVALID 반환.
 */
spdk_blob_id
spdk_blob_get_parent_snapshot(struct spdk_blob_store *bs, spdk_blob_id blob_id)
{
	struct spdk_blob_list *snapshot_entry = NULL;
	struct spdk_blob_list *clone_entry = NULL;

	TAILQ_FOREACH(snapshot_entry, &bs->snapshots, link) {
		TAILQ_FOREACH(clone_entry, &snapshot_entry->clones, link) {
			if (clone_entry->id == blob_id) {
				return snapshot_entry->id;
			}
		}
	}

	return SPDK_BLOBID_INVALID;
}

/*
 * [한국어]
 * spdk_blob_get_clones - 주어진 snapshot의 자식 clone id 목록 반환 (공개 API)
 *
 * @ids:   (in/out) 사용자 제공 버퍼
 * @count: (in/out) in=버퍼 크기, out=실제 clone 수
 * @return: 0 성공, -ENOMEM 버퍼 부족 (이때 *count에 필요한 크기 채움)
 *
 * 첫 호출에서 ids=NULL/count=0으로 호출해 필요한 크기를 받은 뒤 두 번째 호출로
 * 채우는 패턴을 권장.
 */
int
spdk_blob_get_clones(struct spdk_blob_store *bs, spdk_blob_id blobid, spdk_blob_id *ids,
		     size_t *count)
{
	struct spdk_blob_list *snapshot_entry, *clone_entry;
	size_t n;

	snapshot_entry = bs_get_snapshot_entry(bs, blobid);
	if (snapshot_entry == NULL) {
		*count = 0;
		return 0;
	}

	if (ids == NULL || *count < snapshot_entry->clone_count) {
		*count = snapshot_entry->clone_count;
		return -ENOMEM;
	}
	*count = snapshot_entry->clone_count;

	n = 0;
	TAILQ_FOREACH(clone_entry, &snapshot_entry->clones, link) {
		ids[n++] = clone_entry->id;
	}

	return 0;
}

static void
bs_load_grow_continue(struct spdk_bs_load_ctx *ctx)
{
	int rc;

	if (ctx->super->size == 0) {
		ctx->super->size = ctx->bs->dev->blockcnt * ctx->bs->dev->blocklen;
	}

	if (ctx->super->io_unit_size == 0) {
		ctx->super->io_unit_size = SPDK_BS_PAGE_SIZE;
	}
	if (ctx->super->md_page_size == 0) {
		ctx->super->md_page_size = SPDK_BS_PAGE_SIZE;
	}

	/* Parse the super block */
	ctx->bs->clean = 1;
	ctx->bs->cluster_sz = ctx->super->cluster_size;
	ctx->bs->total_clusters = ctx->super->size / ctx->super->cluster_size;
	ctx->bs->md_page_size = ctx->super->md_page_size;
	ctx->bs->io_unit_size = ctx->super->io_unit_size;
	bs_init_per_cluster_fields(ctx->bs);
	rc = spdk_bit_array_resize(&ctx->used_clusters, ctx->bs->total_clusters);
	if (rc < 0) {
		bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}
	ctx->bs->md_start = ctx->super->md_start;
	ctx->bs->md_len = ctx->super->md_len;
	rc = spdk_bit_array_resize(&ctx->bs->open_blobids, ctx->bs->md_len);
	if (rc < 0) {
		bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	ctx->bs->total_data_clusters = ctx->bs->total_clusters - spdk_divide_round_up(
					       ctx->bs->md_start + ctx->bs->md_len, ctx->bs->pages_per_cluster);
	ctx->bs->super_blob = ctx->super->super_blob;
	memcpy(&ctx->bs->bstype, &ctx->super->bstype, sizeof(ctx->super->bstype));

	if (ctx->super->used_blobid_mask_len == 0 || ctx->super->clean == 0) {
		SPDK_ERRLOG("Can not grow an unclean blobstore, please load it normally to clean it.\n");
		bs_load_ctx_fail(ctx, -EIO);
		return;
	} else {
		bs_load_read_used_pages(ctx);
	}
}

static void
bs_load_grow_super_write_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	if (bserrno != 0) {
		bs_load_ctx_fail(ctx, bserrno);
		return;
	}
	bs_load_grow_continue(ctx);
}

static void
bs_load_grow_used_clusters_write_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	if (bserrno != 0) {
		bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	spdk_free(ctx->mask);

	bs_sequence_write_dev(ctx->seq, ctx->super, bs_page_to_lba(ctx->bs, 0),
			      bs_byte_to_lba(ctx->bs, sizeof(*ctx->super)),
			      bs_load_grow_super_write_cpl, ctx);
}

static void
bs_load_grow_used_clusters_read_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint64_t		lba, lba_count;
	uint64_t		dev_size;
	uint64_t		total_clusters;

	if (bserrno != 0) {
		bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	/* The type must be correct */
	assert(ctx->mask->type == SPDK_MD_MASK_TYPE_USED_CLUSTERS);
	/* The length of the mask (in bits) must not be greater than the length of the buffer (converted to bits) */
	assert(ctx->mask->length <= (ctx->super->used_cluster_mask_len * sizeof(
					     struct spdk_blob_md_page) * 8));
	dev_size = ctx->bs->dev->blockcnt * ctx->bs->dev->blocklen;
	total_clusters = dev_size / ctx->super->cluster_size;
	ctx->mask->length = total_clusters;

	lba = bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_start);
	lba_count = bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_len);
	bs_sequence_write_dev(ctx->seq, ctx->mask, lba, lba_count,
			      bs_load_grow_used_clusters_write_cpl, ctx);
}

static void
bs_load_try_to_grow(struct spdk_bs_load_ctx *ctx)
{
	uint64_t dev_size, total_clusters, used_cluster_mask_len, max_used_cluster_mask;
	uint64_t lba, lba_count, mask_size;

	dev_size = ctx->bs->dev->blockcnt * ctx->bs->dev->blocklen;
	total_clusters = dev_size / ctx->super->cluster_size;
	used_cluster_mask_len = spdk_divide_round_up(sizeof(struct spdk_bs_md_mask) +
				spdk_divide_round_up(total_clusters, 8),
				ctx->super->md_page_size);
	max_used_cluster_mask = ctx->super->used_blobid_mask_start - ctx->super->used_cluster_mask_start;
	/* No necessary to grow or no space to grow */
	if (ctx->super->size >= dev_size || used_cluster_mask_len > max_used_cluster_mask) {
		SPDK_DEBUGLOG(blob, "No grow\n");
		bs_load_grow_continue(ctx);
		return;
	}

	SPDK_DEBUGLOG(blob, "Resize blobstore\n");

	ctx->super->size = dev_size;
	ctx->super->used_cluster_mask_len = used_cluster_mask_len;
	ctx->super->crc = blob_md_page_calc_crc(ctx->super);

	mask_size = used_cluster_mask_len * ctx->super->md_page_size;
	ctx->mask = spdk_zmalloc(mask_size, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY,
				 SPDK_MALLOC_DMA);
	if (!ctx->mask) {
		bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}
	lba = bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_start);
	lba_count = bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_len);
	bs_sequence_read_dev(ctx->seq, ctx->mask, lba, lba_count,
			     bs_load_grow_used_clusters_read_cpl, ctx);
}

static void
bs_grow_load_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	int rc;

	rc = bs_super_validate(ctx->super, ctx->bs);
	if (rc != 0) {
		bs_load_ctx_fail(ctx, rc);
		return;
	}

	bs_load_try_to_grow(ctx);
}

struct spdk_bs_grow_ctx {
	struct spdk_blob_store		*bs;
	struct spdk_bs_super_block	*super;

	struct spdk_bit_pool		*new_used_clusters;
	struct spdk_bs_md_mask		*new_used_clusters_mask;

	spdk_bs_sequence_t		*seq;
};

static void
bs_grow_live_done(struct spdk_bs_grow_ctx *ctx, int bserrno)
{
	if (bserrno != 0) {
		spdk_bit_pool_free(&ctx->new_used_clusters);
	}

	bs_sequence_finish(ctx->seq, bserrno);
	free(ctx->new_used_clusters_mask);
	spdk_free(ctx->super);
	free(ctx);
}

static void
bs_grow_live_super_write_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_grow_ctx	*ctx = cb_arg;
	struct spdk_blob_store *bs = ctx->bs;
	uint64_t total_clusters;

	if (bserrno != 0) {
		bs_grow_live_done(ctx, bserrno);
		return;
	}

	/*
	 * Blobstore is not clean until unload, for now only the super block is up to date.
	 * This is similar to state right after blobstore init, when bs_write_used_md() didn't
	 * yet execute.
	 * When cleanly unloaded, the used md pages will be written out.
	 * In case of unclean shutdown, loading blobstore will go through recovery path correctly
	 * filling out the used_clusters with new size and writing it out.
	 */
	bs->clean = 0;

	/* Reverting the super->size past this point is complex, avoid any error paths
	 * that require to do so. */
	spdk_spin_lock(&bs->used_lock);

	total_clusters = ctx->super->size / ctx->super->cluster_size;

	assert(total_clusters >= spdk_bit_pool_capacity(bs->used_clusters));
	spdk_bit_pool_store_mask(bs->used_clusters, ctx->new_used_clusters_mask);

	assert(total_clusters == spdk_bit_pool_capacity(ctx->new_used_clusters));
	spdk_bit_pool_load_mask(ctx->new_used_clusters, ctx->new_used_clusters_mask);

	spdk_bit_pool_free(&bs->used_clusters);
	bs->used_clusters = ctx->new_used_clusters;

	bs->total_clusters = total_clusters;
	bs->total_data_clusters = bs->total_clusters - spdk_divide_round_up(
					  bs->md_start + bs->md_len, bs->pages_per_cluster);

	bs->num_free_clusters = spdk_bit_pool_count_free(bs->used_clusters);
	assert(ctx->bs->num_free_clusters <= ctx->bs->total_clusters);
	spdk_spin_unlock(&bs->used_lock);

	bs_grow_live_done(ctx, 0);
}

static void
bs_grow_live_load_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_grow_ctx *ctx = cb_arg;
	uint64_t dev_size, total_clusters, used_cluster_mask_len, max_used_cluster_mask;
	int rc;

	if (bserrno != 0) {
		bs_grow_live_done(ctx, bserrno);
		return;
	}

	rc = bs_super_validate(ctx->super, ctx->bs);
	if (rc != 0) {
		bs_grow_live_done(ctx, rc);
		return;
	}

	dev_size = ctx->bs->dev->blockcnt * ctx->bs->dev->blocklen;
	total_clusters = dev_size / ctx->super->cluster_size;
	used_cluster_mask_len = spdk_divide_round_up(sizeof(struct spdk_bs_md_mask) +
				spdk_divide_round_up(total_clusters, 8),
				ctx->super->md_page_size);
	max_used_cluster_mask = ctx->super->used_blobid_mask_start - ctx->super->used_cluster_mask_start;
	/* Only checking dev_size. Since it can change, but total_clusters remain the same. */
	if (dev_size == ctx->super->size) {
		SPDK_DEBUGLOG(blob, "No need to grow blobstore\n");
		bs_grow_live_done(ctx, 0);
		return;
	}
	/*
	 * Blobstore cannot be shrunk, so check before if:
	 * - new size of the device is smaller than size in super_block
	 * - new total number of clusters is smaller than used_clusters bit_pool
	 * - there is enough space in metadata for used_cluster_mask to be written out
	 */
	if (dev_size < ctx->super->size ||
	    total_clusters < spdk_bit_pool_capacity(ctx->bs->used_clusters) ||
	    used_cluster_mask_len > max_used_cluster_mask) {
		SPDK_DEBUGLOG(blob, "No space to grow blobstore\n");
		bs_grow_live_done(ctx, -ENOSPC);
		return;
	}

	SPDK_DEBUGLOG(blob, "Resizing blobstore\n");

	ctx->new_used_clusters_mask = calloc(1, total_clusters);
	if (!ctx->new_used_clusters_mask) {
		bs_grow_live_done(ctx, -ENOMEM);
		return;
	}
	ctx->new_used_clusters = spdk_bit_pool_create(total_clusters);
	if (!ctx->new_used_clusters) {
		bs_grow_live_done(ctx, -ENOMEM);
		return;
	}

	ctx->super->clean = 0;
	ctx->super->size = dev_size;
	ctx->super->used_cluster_mask_len = used_cluster_mask_len;
	bs_write_super(seq, ctx->bs, ctx->super, bs_grow_live_super_write_cpl, ctx);
}

/*
 * [한국어]
 * spdk_bs_grow_live - 마운트된 blobstore의 backing dev 확장을 즉시 반영 (공개 API)
 *
 * @bs:    이미 load된 blobstore (재시작 없이 그 자리에서 cluster 확장)
 * @cb_fn: 완료 콜백
 *
 * 사용 사례: lvol/bdev 사이즈가 늘어났을 때 blobstore가 새 cluster를 인식하게 한다.
 * 동작:
 * 1) super를 다시 read
 * 2) 새 dev_size 기반 used_cluster_mask 길이 재계산 (init 시 max 영역 안에 있어야 함)
 * 3) super clean=0 표시 후 super write → spdk_bs_grow_live_super_write_cpl 체인
 * 4) used_cluster bit_pool/bit_array를 확장 → 새 cluster들을 free로 마킹 → super write
 *
 * 실행 컨텍스트: md_thread (assert).
 */
void
spdk_bs_grow_live(struct spdk_blob_store *bs,
		  spdk_bs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	struct spdk_bs_grow_ctx *ctx;

	assert(spdk_get_thread() == bs->md_thread);

	SPDK_DEBUGLOG(blob, "Growing blobstore on dev %p\n", bs->dev);

	cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	cpl.u.bs_basic.cb_fn = cb_fn;
	cpl.u.bs_basic.cb_arg = cb_arg;

	ctx = calloc(1, sizeof(struct spdk_bs_grow_ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	ctx->bs = bs;

	ctx->super = spdk_zmalloc(sizeof(*ctx->super), 0x1000, NULL,
				  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->super) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->seq = bs_sequence_start_bs(bs->md_channel, &cpl);
	if (!ctx->seq) {
		spdk_free(ctx->super);
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* Read the super block */
	bs_sequence_read_dev(ctx->seq, ctx->super, bs_page_to_lba(bs, 0),
			     bs_byte_to_lba(bs, sizeof(*ctx->super)),
			     bs_grow_live_load_super_cpl, ctx);
}

/*
 * [한국어]
 * spdk_bs_grow - load + grow를 한 번에 (offline grow) (공개 API)
 *
 * @dev: 새 크기로 늘어난 backing bs_dev (이전엔 load된 적 없거나 unload된 상태여야 함)
 *
 * spdk_bs_load와 거의 같지만, super read 후 grow 처리(used_cluster mask 확장)를 거쳐
 * 새 cluster들을 사용 가능하게 만든 뒤 사용자에게 핸들 반환. 재시작 직후의 grow
 * 시나리오에 적합.
 */
void
spdk_bs_grow(struct spdk_bs_dev *dev, struct spdk_bs_opts *o,
	     spdk_bs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_store	*bs;
	struct spdk_bs_cpl	cpl;
	struct spdk_bs_load_ctx *ctx;
	struct spdk_bs_opts	opts = {};
	int err;

	SPDK_DEBUGLOG(blob, "Loading blobstore from dev %p\n", dev);

	if ((dev->phys_blocklen % dev->blocklen) != 0) {
		SPDK_DEBUGLOG(blob, "unsupported dev block length of %d\n", dev->blocklen);
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	spdk_bs_opts_init(&opts, sizeof(opts));
	if (o) {
		if (bs_opts_copy(o, &opts)) {
			dev->destroy(dev);
			cb_fn(cb_arg, NULL, -EINVAL);
			return;
		}
	}

	if (opts.max_md_ops == 0 || opts.max_channel_ops == 0) {
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	err = bs_alloc(dev, &opts, &bs, &ctx);
	if (err) {
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, err);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_HANDLE;
	cpl.u.bs_handle.cb_fn = cb_fn;
	cpl.u.bs_handle.cb_arg = cb_arg;
	cpl.u.bs_handle.bs = bs;

	ctx->seq = bs_sequence_start_bs(bs->md_channel, &cpl);
	if (!ctx->seq) {
		spdk_free(ctx->super);
		free(ctx);
		bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	/* Read the super block */
	bs_sequence_read_dev(ctx->seq, ctx->super, bs_page_to_lba(bs, 0),
			     bs_byte_to_lba(bs, sizeof(*ctx->super)),
			     bs_grow_load_super_cpl, ctx);
}

/*
 * [한국어]
 * spdk_blob_get_esnap_id - esnap clone의 외부 snapshot id를 반환 (공개 API)
 *
 * @id:  (out) BLOB_EXTERNAL_SNAPSHOT_ID 내부 xattr 값 포인터 (free 금지)
 * @len: (out) 값 길이
 * @return: 0 성공, -EINVAL esnap clone이 아님
 *
 * id의 의미는 사용자(esnap_bs_dev_create 콜백)가 정의 — 보통 부모 lvol id 등.
 */
int
spdk_blob_get_esnap_id(struct spdk_blob *blob, const void **id, size_t *len)
{
	if (!blob_is_esnap_clone(blob)) {
		return -EINVAL;
	}

	return blob_get_xattr_value(blob, BLOB_EXTERNAL_SNAPSHOT_ID, id, len, true);
}

struct spdk_io_channel *
blob_esnap_get_io_channel(struct spdk_io_channel *ch, struct spdk_blob *blob)
{
	struct spdk_bs_channel		*bs_channel = spdk_io_channel_get_ctx(ch);
	struct spdk_bs_dev		*bs_dev = blob->back_bs_dev;
	struct blob_esnap_channel	find = {};
	struct blob_esnap_channel	*esnap_channel, *existing;

	find.blob_id = blob->id;
	esnap_channel = RB_FIND(blob_esnap_channel_tree, &bs_channel->esnap_channels, &find);
	if (spdk_likely(esnap_channel != NULL)) {
		SPDK_DEBUGLOG(blob_esnap, "blob 0x%" PRIx64 ": using cached channel on thread %s\n",
			      blob->id, spdk_thread_get_name(spdk_get_thread()));
		return esnap_channel->channel;
	}

	SPDK_DEBUGLOG(blob_esnap, "blob 0x%" PRIx64 ": allocating channel on thread %s\n",
		      blob->id, spdk_thread_get_name(spdk_get_thread()));

	esnap_channel = calloc(1, sizeof(*esnap_channel));
	if (esnap_channel == NULL) {
		SPDK_NOTICELOG("blob 0x%" PRIx64 " channel allocation failed: no memory\n",
			       find.blob_id);
		return NULL;
	}
	esnap_channel->channel = bs_dev->create_channel(bs_dev);
	if (esnap_channel->channel == NULL) {
		SPDK_NOTICELOG("blob 0x%" PRIx64 " back channel allocation failed\n", blob->id);
		free(esnap_channel);
		return NULL;
	}
	esnap_channel->blob_id = find.blob_id;
	existing = RB_INSERT(blob_esnap_channel_tree, &bs_channel->esnap_channels, esnap_channel);
	if (spdk_unlikely(existing != NULL)) {
		/*
		 * This should be unreachable: all modifications to this tree happen on this thread.
		 */
		SPDK_ERRLOG("blob 0x%" PRIx64 "lost race to allocate a channel\n", find.blob_id);
		assert(false);

		bs_dev->destroy_channel(bs_dev, esnap_channel->channel);
		free(esnap_channel);

		return existing->channel;
	}

	return esnap_channel->channel;
}

static int
blob_esnap_channel_compare(struct blob_esnap_channel *c1, struct blob_esnap_channel *c2)
{
	return (c1->blob_id < c2->blob_id ? -1 : c1->blob_id > c2->blob_id);
}

struct blob_esnap_destroy_ctx {
	spdk_blob_op_with_handle_complete	cb_fn;
	void					*cb_arg;
	struct spdk_blob			*blob;
	struct spdk_bs_dev			*back_bs_dev;
	bool					abort_io;
};

static void
blob_esnap_destroy_channels_done(struct spdk_io_channel_iter *i, int status)
{
	struct blob_esnap_destroy_ctx	*ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;

	SPDK_DEBUGLOG(blob_esnap, "blob 0x%" PRIx64 ": done destroying channels for this blob\n",
		      blob->id);

	if (ctx->cb_fn != NULL) {
		ctx->cb_fn(ctx->cb_arg, blob, status);
	}
	free(ctx);

	bs->esnap_channels_unloading--;
	if (bs->esnap_channels_unloading == 0 && bs->esnap_unload_cb_fn != NULL) {
		spdk_bs_unload(bs, bs->esnap_unload_cb_fn, bs->esnap_unload_cb_arg);
	}
}

static void
blob_esnap_destroy_one_channel(struct spdk_io_channel_iter *i)
{
	struct blob_esnap_destroy_ctx	*ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_bs_dev		*bs_dev = ctx->back_bs_dev;
	struct spdk_io_channel		*channel = spdk_io_channel_iter_get_channel(i);
	struct spdk_bs_channel		*bs_channel = spdk_io_channel_get_ctx(channel);
	struct blob_esnap_channel	*esnap_channel;
	struct blob_esnap_channel	find = {};

	assert(spdk_get_thread() == spdk_io_channel_get_thread(channel));

	find.blob_id = blob->id;
	esnap_channel = RB_FIND(blob_esnap_channel_tree, &bs_channel->esnap_channels, &find);
	if (esnap_channel != NULL) {
		SPDK_DEBUGLOG(blob_esnap, "blob 0x%" PRIx64 ": destroying channel on thread %s\n",
			      blob->id, spdk_thread_get_name(spdk_get_thread()));
		RB_REMOVE(blob_esnap_channel_tree, &bs_channel->esnap_channels, esnap_channel);

		if (ctx->abort_io) {
			spdk_bs_user_op_t *op, *tmp;

			TAILQ_FOREACH_SAFE(op, &bs_channel->queued_io, link, tmp) {
				if (op->back_channel == esnap_channel->channel) {
					TAILQ_REMOVE(&bs_channel->queued_io, op, link);
					bs_user_op_abort(op, -EIO);
				}
			}
		}

		bs_dev->destroy_channel(bs_dev, esnap_channel->channel);
		free(esnap_channel);
	}

	spdk_for_each_channel_continue(i, 0);
}

/*
 * Destroy the channels for a specific blob on each thread with a blobstore channel. This should be
 * used when closing an esnap clone blob and after decoupling from the parent.
 */
static void
blob_esnap_destroy_bs_dev_channels(struct spdk_blob *blob, bool abort_io,
				   spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct blob_esnap_destroy_ctx	*ctx;

	if (!blob_is_esnap_clone(blob) || blob->back_bs_dev == NULL) {
		if (cb_fn != NULL) {
			cb_fn(cb_arg, blob, 0);
		}
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		if (cb_fn != NULL) {
			cb_fn(cb_arg, blob, -ENOMEM);
		}
		return;
	}
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->blob = blob;
	ctx->back_bs_dev = blob->back_bs_dev;
	ctx->abort_io = abort_io;

	SPDK_DEBUGLOG(blob_esnap, "blob 0x%" PRIx64 ": destroying channels for this blob\n",
		      blob->id);

	blob->bs->esnap_channels_unloading++;
	spdk_for_each_channel(blob->bs, blob_esnap_destroy_one_channel, ctx,
			      blob_esnap_destroy_channels_done);
}

/*
 * Destroy all bs_dev channels on a specific blobstore channel. This should be used when a
 * bs_channel is destroyed.
 */
static void
blob_esnap_destroy_bs_channel(struct spdk_bs_channel *ch)
{
	struct blob_esnap_channel *esnap_channel, *esnap_channel_tmp;

	assert(spdk_get_thread() == spdk_io_channel_get_thread(spdk_io_channel_from_ctx(ch)));

	SPDK_DEBUGLOG(blob_esnap, "destroying channels on thread %s\n",
		      spdk_thread_get_name(spdk_get_thread()));
	RB_FOREACH_SAFE(esnap_channel, blob_esnap_channel_tree, &ch->esnap_channels,
			esnap_channel_tmp) {
		SPDK_DEBUGLOG(blob_esnap, "blob 0x%" PRIx64
			      ": destroying one channel in thread %s\n",
			      esnap_channel->blob_id, spdk_thread_get_name(spdk_get_thread()));
		RB_REMOVE(blob_esnap_channel_tree, &ch->esnap_channels, esnap_channel);
		spdk_put_io_channel(esnap_channel->channel);
		free(esnap_channel);
	}
	SPDK_DEBUGLOG(blob_esnap, "done destroying channels on thread %s\n",
		      spdk_thread_get_name(spdk_get_thread()));
}

static void
blob_set_back_bs_dev_done(void *_ctx, int bserrno)
{
	struct set_bs_dev_ctx	*ctx = _ctx;

	if (bserrno != 0) {
		/* Even though the unfreeze failed, the update may have succeed. */
		SPDK_ERRLOG("blob 0x%" PRIx64 ": unfreeze failed with error %d\n", ctx->blob->id,
			    bserrno);
	}
	ctx->cb_fn(ctx->cb_arg, ctx->bserrno);
	free(ctx);
}

static void
blob_frozen_set_back_bs_dev(void *_ctx, struct spdk_blob *blob, int bserrno)
{
	struct set_bs_dev_ctx	*ctx = _ctx;
	int rc;

	if (bserrno != 0) {
		SPDK_ERRLOG("blob 0x%" PRIx64 ": failed to release old back_bs_dev with error %d\n",
			    blob->id, bserrno);
		ctx->bserrno = bserrno;
		blob_unfreeze_io(blob, blob_set_back_bs_dev_done, ctx);
		return;
	}

	if (blob->back_bs_dev != NULL) {
		blob_unref_back_bs_dev(blob);
	}

	if (ctx->parent_refs_cb_fn) {
		rc = ctx->parent_refs_cb_fn(blob, ctx->parent_refs_cb_arg);
		if (rc != 0) {
			ctx->bserrno = rc;
			blob_unfreeze_io(blob, blob_set_back_bs_dev_done, ctx);
			return;
		}
	}

	SPDK_NOTICELOG("blob 0x%" PRIx64 ": hotplugged back_bs_dev\n", blob->id);
	blob->back_bs_dev = ctx->back_bs_dev;
	ctx->bserrno = 0;

	blob_unfreeze_io(blob, blob_set_back_bs_dev_done, ctx);
}

static void
blob_set_back_bs_dev_frozen(void *_ctx, int bserrno)
{
	struct set_bs_dev_ctx	*ctx = _ctx;
	struct spdk_blob	*blob = ctx->blob;

	if (bserrno != 0) {
		SPDK_ERRLOG("blob 0x%" PRIx64 ": failed to freeze with error %d\n", blob->id,
			    bserrno);
		ctx->cb_fn(ctx->cb_arg, bserrno);
		free(ctx);
		return;
	}

	/*
	 * This does not prevent future reads from the esnap device because any future IO will
	 * lazily create a new esnap IO channel.
	 */
	blob_esnap_destroy_bs_dev_channels(blob, true, blob_frozen_set_back_bs_dev, ctx);
}

/*
 * [한국어]
 * spdk_blob_set_esnap_bs_dev - esnap clone의 backing bs_dev를 hot-swap (공개 API)
 *
 * @back_bs_dev: 새 부모 bs_dev (소유권 인계)
 *
 * esnap clone이 아닌 blob에는 -EINVAL. 동작:
 * 1) blob freeze (진행 중 IO 정지)
 * 2) 기존 esnap 채널을 모두 파괴 (abort_io=true — 진행 중 read는 실패 처리)
 * 3) 옛 back_bs_dev 해제 → 새 것으로 교체
 * 4) blob unfreeze
 *
 * 사용 사례: 부모 lvol/vbdev이 다른 위치로 이동했을 때 인플레이트 없이 backing 교체.
 */
void
spdk_blob_set_esnap_bs_dev(struct spdk_blob *blob, struct spdk_bs_dev *back_bs_dev,
			   spdk_blob_op_complete cb_fn, void *cb_arg)
{
	if (!blob_is_esnap_clone(blob)) {
		SPDK_ERRLOG("blob 0x%" PRIx64 ": not an esnap clone\n", blob->id);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	blob_set_back_bs_dev(blob, back_bs_dev, NULL, NULL, cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_blob_get_esnap_bs_dev - 현재 esnap clone의 backing bs_dev 포인터 반환 (공개 API)
 *
 * 단순 getter — esnap clone이 아니면 NULL.
 */
struct spdk_bs_dev *
spdk_blob_get_esnap_bs_dev(const struct spdk_blob *blob)
{
	if (!blob_is_esnap_clone(blob)) {
		SPDK_ERRLOG("blob 0x%" PRIx64 ": not an esnap clone\n", blob->id);
		return NULL;
	}

	return blob->back_bs_dev;
}

/*
 * [한국어]
 * spdk_blob_is_degraded - blob 또는 그 backing dev가 degraded 상태인지 (공개 API)
 *
 * - 자체 bs_dev->is_degraded() == true 면 degraded
 * - back_bs_dev가 있고 그쪽이 degraded면 degraded
 *
 * 사용 사례: 사용자 모니터링 / RPC 응답 — 운영 중인 blob 상태 진단.
 */
bool
spdk_blob_is_degraded(const struct spdk_blob *blob)
{
	if (blob->bs->dev->is_degraded != NULL && blob->bs->dev->is_degraded(blob->bs->dev)) {
		return true;
	}
	if (blob->back_bs_dev == NULL || blob->back_bs_dev->is_degraded == NULL) {
		return false;
	}

	return blob->back_bs_dev->is_degraded(blob->back_bs_dev);
}

SPDK_LOG_REGISTER_COMPONENT(blob)
SPDK_LOG_REGISTER_COMPONENT(blob_esnap)

static void
blob_trace(void)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"BLOB_REQ_SET_START", TRACE_BLOB_REQ_SET_START,
			OWNER_TYPE_NONE, OBJECT_BLOB_CB_ARG, 1,
			{
				{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 }
			}
		},
		{
			"BLOB_REQ_SET_COMPLETE", TRACE_BLOB_REQ_SET_COMPLETE,
			OWNER_TYPE_NONE, OBJECT_BLOB_CB_ARG, 0,
			{
				{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 }
			}
		},
	};

	spdk_trace_register_object(OBJECT_BLOB_CB_ARG, 'a');
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_START, OBJECT_BLOB_CB_ARG, 1);
	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_DONE, OBJECT_BLOB_CB_ARG, 0);
}
SPDK_TRACE_REGISTER_FN(blob_trace, "blob", TRACE_GROUP_BLOB)
