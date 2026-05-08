/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] Blobstore 내부 자료구조와 인라인 변환 함수 헤더 (blobstore.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK Blobstore의 모든 내부 자료구조 — 메모리 구조체(spdk_blob,
 * spdk_blob_store, spdk_bs_channel, spdk_blob_mut_data 등)와 디스크 구조체
 * (super block, metadata page, 모든 metadata descriptor) — 를 정의하고, blob 좌표계와
 * 디바이스 LBA 사이의 변환 인라인 함수를 제공한다. lib/blob의 모든 .c 파일이 이 헤더를
 * 포함하며, 외부에는 노출되지 않는 internal API이다. 메모리 구조체는 blob 라이프사이클을
 * 관리하고, 디스크 구조체는 메타페이지 직렬화 포맷 (#pragma pack(1)으로 비트 정확)을
 * 정의한다. 매크로 상수들(SPDK_BLOB_OPTS_*, SPDK_MD_DESCRIPTOR_TYPE_*)은 default 옵션과
 * descriptor 타입 ID를 정의한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 / 의존:
 *   사용자 → spdk/blob.h (공개 API) → lib/blob/blobstore.c가 이 헤더를 include
 *     → struct spdk_blob 등을 직접 다룸
 *     → request.c/blob_bs_dev.c/zeroes.c도 이 헤더를 공유
 *   디스크 → bdev_*->read → spdk_blob_md_page (#pragma pack) → in-memory spdk_blob 변환
 * 실행 컨텍스트: 호스트 유저스페이스, blob 소유 SPDK thread (메타는 md_thread).
 * polled-mode lockless: 채널은 thread-affined, used_lock(spdk_spinlock)만 멀티스레드 보호.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/blob.h: 공개 spdk_blob_id, spdk_blob_op_complete 등 사용자 API.
 * - spdk/queue.h: TAILQ_* / RB_* 매크로 (open_blobs RB tree, snapshots TAILQ 등).
 * - spdk/tree.h: RB tree (red-black) 매크로 — open_blobs 정렬 + O(log n) 조회.
 * - spdk/thread.h: spdk_io_channel, spdk_thread, spdk_spinlock.
 * - spdk/assert.h: SPDK_STATIC_ASSERT — 디스크 구조체 크기 컴파일 타임 검증.
 * - spdk/util.h: SPDK_SIZEOF_MEMBER, spdk_align64pow2 등 유틸 매크로.
 * - request.h: spdk_bs_request_set, spdk_bs_cpl 등 요청 객체 — bs_channel의 풀에 들어감.
 * - blob_bs_dev.c: bs_create_blob_bs_dev, struct spdk_blob_bs_dev 정의를 사용.
 * - zeroes.c: bs_create_zeroes_dev, blob_backed_with_zeroes_dev.
 * 데이터 흐름 (in-memory ↔ on-disk):
 *   load: bdev read → spdk_blob_md_page (디스크) → descriptor 파싱 → spdk_blob (메모리)
 *   sync: spdk_blob (메모리) → descriptor 직렬화 → spdk_blob_md_page → bdev write
 *
 * === 주요 함수/구조체 요약 ===
 * 메모리 구조체:
 * - struct spdk_blob: blob의 in-memory 표현. clean/active dual copy로 트랜잭션 sync.
 * - struct spdk_blob_store: blobstore 전체 상태 (메타 위치, 클러스터 풀, 채널 등).
 * - struct spdk_bs_channel: per-thread I/O 채널 컨텍스트, request set 풀 보유.
 * - struct spdk_blob_mut_data: blob의 mutable·persistent 데이터 (clusters/pages 배열).
 * - struct spdk_xattr: 단일 xattr 항목 (name/value).
 * - struct spdk_blob_list: snapshot의 자식(clone) 리스트 — 트리 구조.
 * - struct spdk_blob_bs_dev: blob_bs_dev.c 어댑터의 메모리 객체.
 *
 * 디스크 구조체 (#pragma pack(1)으로 비트 정확):
 * - struct spdk_bs_super_block: 슈퍼블록 (4KiB) — 시그니처/버전/메타 위치/크기 등.
 * - struct spdk_blob_md_page: 4KiB 메타페이지 — descriptor 체인 + CRC + next 포인터.
 * - struct spdk_bs_md_mask: 사용중 페이지/클러스터/blobid 비트맵 직렬화.
 * - struct spdk_blob_md_descriptor*: 다양한 descriptor 타입 (XATTR, EXTENT_RLE,
 *   EXTENT_TABLE, EXTENT_PAGE, FLAGS).
 *
 * 인라인 변환 함수 (단위 변환 — 클러스터/페이지/io_unit/LBA/byte 사이):
 * - bs_byte_to_lba / bs_dev_byte_to_lba / bs_page_to_lba / bs_md_page_to_lba
 * - bs_dev_io_unit_to_lba / bs_cluster_to_io_unit / bs_io_unit_to_cluster
 * - bs_cluster_to_lba / bs_lba_to_cluster / bs_io_unit_to_back_dev_lba
 * - bs_blob_io_unit_to_lba: blob 좌표 → 디스크 LBA (clusters[] 인덱싱).
 * - bs_num_io_units_to_cluster_boundary / bs_io_unit_to_cluster_start /
 *   bs_io_unit_to_cluster_number: split 알고리즘에 사용.
 * - bs_io_unit_is_allocated: thin blob의 클러스터 할당 여부.
 * - bs_blobid_to_page / bs_page_to_blobid: blob_id ↔ 메타페이지 인덱스 변환.
 *
 * 핵심 매크로:
 * - SPDK_BS_VERSION = 3 (현재 디스크 포맷 버전)
 * - SPDK_BS_PAGE_SIZE = 0x1000 (4KiB) — 메타페이지 단위
 * - SPDK_BS_SUPER_BLOCK_SIG = "SPDKBLOB" (8바이트 매직 시그니처)
 * - SPDK_MD_DESCRIPTOR_TYPE_*: 메타 descriptor 타입 ID (PADDING/XATTR/FLAGS/RLE/TABLE/PAGE/INTERNAL).
 * - SPDK_BLOB_*: blob flag 비트 (THIN_PROV, INTERNAL_XATTR, EXTENT_TABLE, EXTERNAL_SNAPSHOT, READ_ONLY).
 */

#ifndef SPDK_BLOBSTORE_H
#define SPDK_BLOBSTORE_H
/* [한국어] 헤더 가드 — 다중 include 시 중복 정의 방지. */

#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT — 디스크 구조체 크기 컴파일 타임 검증에 사용. */
#include "spdk/blob.h"
/* [한국어] 공개 blob API — spdk_blob_id, blob_clear_method, spdk_bs_type 등. */
#include "spdk/queue.h"
/* [한국어] TAILQ_* / RB_* 매크로 — pending_persists, snapshots, open_blobs 등 컨테이너. */
#include "spdk/util.h"
/* [한국어] SPDK_SIZEOF_MEMBER, spdk_align64pow2 등 유틸 매크로 — 디스크 포맷 크기 계산에 사용. */
#include "spdk/tree.h"
/* [한국어] RB(red-black) tree 매크로 — open_blobs 정렬 + O(log n) 조회. */
#include "spdk/thread.h"
/* [한국어] spdk_io_channel, spdk_thread, spdk_spinlock — channel/lock 정의. */

#include "request.h"
/* [한국어] spdk_bs_request_set, spdk_bs_cpl — bs_channel의 reqs 풀에 들어가는 객체 정의. */

/* In Memory Data Structures
 *
 * The following data structures exist only in memory.
 */

#define SPDK_BLOB_OPTS_CLUSTER_SZ (1024 * 1024)
/* [한국어] 기본 클러스터 크기 = 1MiB. 사용자가 spdk_bs_init_opts로 변경 가능 (보통 4MiB 등).
 * 클러스터 = blobstore의 할당 단위 (메타페이지 4KiB와 별개). */
#define SPDK_BLOB_OPTS_NUM_MD_PAGES UINT32_MAX
/* [한국어] 기본 메타페이지 수 — UINT32_MAX는 "디스크 크기에 맞춰 최대로 잡으라"는 마커.
 * 실제 값은 디스크 크기와 cluster_sz로부터 spdk_bs_init이 계산. */
#define SPDK_BLOB_OPTS_MAX_MD_OPS 32
/* [한국어] 메타데이터 채널의 동시 진행 가능 op 수 (request set 풀 크기 기본값). */
#define SPDK_BLOB_OPTS_DEFAULT_CHANNEL_OPS 512
/* [한국어] 일반 I/O 채널의 동시 진행 가능 op 수 — 한 채널이 이만큼의 set을 풀에 보유. */
#define SPDK_BLOB_BLOBID_HIGH_BIT (1ULL << 32)
/* [한국어] blob_id 상위 32비트 마커 — 하위 32비트가 page_idx, 상위 비트는 항상 1로 설정해
 * "blob_id == page_idx"로 잘못 가정하는 코드 버그를 컴파일 후 노출시킴 (방어적 설계). */

/*
 * [한국어]
 * struct spdk_xattr — 단일 확장 속성(extended attribute) 항목.
 * blob에 키-값 쌍 메타데이터를 부착하기 위한 자료구조. xattrs(공개) / xattrs_internal(내부)
 * 두 종류의 TAILQ에 들어간다.
 */
struct spdk_xattr {
	uint32_t	index;
	/* [한국어] 메타페이지 내 descriptor 인덱스 (직렬화 시 위치).
	 * 설정자: blob 메타 직렬화/역직렬화 코드 (blob_serialize/blob_parse).
	 * 읽는 자: 동일. 값 범위: 0 이상의 인덱스. 동기화: blob 단일 thread 점유. */
	uint16_t	value_len;
	/* [한국어] value 바이트 길이 (16비트 → 최대 64KiB - 1).
	 * 설정자: 사용자가 spdk_blob_set_xattr 호출 시. 읽는 자: get_xattr/직렬화.
	 * 값 범위: 0 ~ UINT16_MAX. 동기화: blob 단일 thread. */
	char		*name;
	/* [한국어] xattr 이름 (NUL-terminated 문자열). 별도 strdup으로 할당된 메모리.
	 * 설정자: set_xattr이 strdup. 읽는 자: get_xattr/직렬화. 값 범위: NULL 불가.
	 * 동기화: blob 단일 thread. blob 해제 시 free. */
	void		*value;
	/* [한국어] xattr 값 — value_len 바이트의 임의 바이너리 데이터, malloc/memcpy 보관.
	 * 설정자: set_xattr. 읽는 자: get_xattr/직렬화. 값 범위: NULL 불가 (value_len이 0이면 빈 buffer 가능).
	 * 동기화: blob 단일 thread. */
	TAILQ_ENTRY(spdk_xattr)	link;
	/* [한국어] xattrs / xattrs_internal TAILQ에 매달리는 링크.
	 * 설정자/읽는 자: TAILQ_INSERT/REMOVE 호출하는 모든 xattr 조작 코드.
	 * 값 범위: TAILQ에 매달려 있을 때만 유효. 동기화: blob 단일 thread → 락 불필요. */
};

/* The mutable part of the blob data that is sync'd to
 * disk. The data in here is both mutable and persistent.
 */
/*
 * [한국어]
 * struct spdk_blob_mut_data — blob의 mutable·persistent 데이터 (디스크에 sync되는 부분).
 * spdk_blob 안에 clean/active 두 복사본이 있어 sync 트랜잭션 중 atomic하게 교체 가능 —
 * 디스크 쓰기 도중 crash가 나도 이전 클린 상태로 복구할 수 있다.
 */
struct spdk_blob_mut_data {
	/* Number of data clusters in the blob */
	uint64_t	num_clusters;
	/* [한국어] blob의 데이터 클러스터 수 — blob 크기를 결정.
	 * 설정자: spdk_blob_resize, blob_create, blob_load.
	 * 읽는 자: 모든 LBA 변환 함수, sync 직렬화 코드.
	 * 값 범위: 0 ~ blobstore total clusters.
	 * 동기화: blob 단일 thread (md_thread for sync). */

	/* Array LBAs that are the beginning of a cluster, in
	 * the order they appear in the blob.
	 */
	uint64_t	*clusters;
	/* [한국어] blob 좌표 순서대로 각 클러스터의 시작 LBA를 담은 배열.
	 * thin blob에서 미할당 슬롯은 0 — bs_io_unit_is_allocated가 이를 체크.
	 * 설정자: cluster 할당/해제 코드 (blob_insert_cluster, blob_remove_cluster).
	 * 읽는 자: bs_blob_io_unit_to_lba 등 LBA 변환 함수들.
	 * 값 범위: 각 원소는 0 (미할당) 또는 유효 LBA. 배열 크기 = cluster_array_size.
	 * 동기화: blob 단일 thread (active) / md_thread (clean sync). */

	/* The size of the clusters array. This is greater than or
	 * equal to 'num_clusters'.
	 */
	size_t		cluster_array_size;
	/* [한국어] clusters 배열 capacity (실제 할당된 원소 수). 보통 num_clusters와 같지만
	 * 미리 더 크게 잡아두어 reserve 효과를 노릴 때 num_clusters > 일 수 있음.
	 * 설정자: realloc/할당 코드. 읽는 자: 배열 접근 시 경계 체크.
	 * 값 범위: >= num_clusters. 동기화: blob 단일 thread. */

	/* The number of allocated clusters in the clusters array */
	uint64_t	num_allocated_clusters;
	/* [한국어] clusters 배열 중 실제 LBA가 할당된(0이 아닌) 원소 수.
	 * thin blob에서 사용된 클러스터 수 추적, 통계/디버깅에 사용.
	 * 설정자: insert/remove cluster 코드. 읽는 자: spdk_blob_get_num_allocated_clusters API.
	 * 값 범위: 0 ~ num_clusters. 동기화: blob 단일 thread. */

	/* Number of extent pages */
	uint64_t	num_extent_pages;
	/* [한국어] EXTENT_TABLE 사용 blob의 extent page 수. RLE 사용 blob은 0.
	 * 설정자: extent_table 직렬화 시. 읽는 자: extent_pages 배열 접근 시 경계.
	 * 값 범위: 0 (RLE 사용) 또는 양수. 동기화: blob 단일 thread. */

	/* Array of page offsets into the metadata region,
	 * containing extents. Can contain entries for not yet
	 * allocated pages. */
	uint32_t	*extent_pages;
	/* [한국어] EXTENT_TABLE 모드의 extent page 인덱스 배열 (메타 영역 page offset).
	 * 0이면 미할당 — lazy allocation. 큰 blob의 클러스터 매핑을 분산 저장.
	 * 설정자: extent_table 갱신 코드. 읽는 자: extent page 로드/저장 코드.
	 * 값 범위: 0 또는 유효 page 인덱스. 동기화: blob 단일 thread. */

	/* The size of the extent page array. This is greater than or
	 * equal to 'num_extent_pages'. */
	size_t		extent_pages_array_size;
	/* [한국어] extent_pages 배열 capacity.
	 * 설정자/읽는 자: 위 패턴과 동일.
	 * 값 범위: >= num_extent_pages. 동기화: blob 단일 thread. */

	/* Number of metadata pages */
	uint32_t	num_pages;
	/* [한국어] 이 blob이 사용하는 메타페이지 수 (체인 길이).
	 * 설정자: 메타 직렬화/역직렬화. 읽는 자: pages 배열 접근.
	 * 값 범위: >= 1 (최소 첫 메타페이지). 동기화: blob 단일 thread. */

	/* Array of page offsets into the metadata region, in
	 * the order of the metadata page sequence.
	 */
	uint32_t	*pages;
	/* [한국어] 이 blob의 메타페이지 인덱스 배열 (체인 순서대로).
	 * 첫 원소 = blob_id 하위 32비트 = bs_blobid_to_page(id).
	 * 설정자: 메타 할당 코드. 읽는 자: 메타 read/write 시 LBA 계산.
	 * 값 범위: 각 원소는 유효 메타페이지 인덱스 (0 ~ md_len-1).
	 * 동기화: blob 단일 thread. */
};

/*
 * [한국어]
 * enum spdk_blob_state — blob의 메모리/디스크 동기화 상태.
 * blob_sync 트랜잭션 중간 상태 추적 + 잘못된 시점 sync 시도 방지.
 */
enum spdk_blob_state {
	/* The blob in-memory version does not match the on-disk
	 * version.
	 */
	SPDK_BLOB_STATE_DIRTY,
	/* [한국어] DIRTY: 메모리 변경이 있지만 디스크에 반영되지 않음.
	 * 설정자: spdk_blob_set_xattr, spdk_blob_resize 등 메모리 수정 함수.
	 * 읽는 자: spdk_blob_sync_md가 dirty면 디스크 write 발행.
	 * 값 범위: enum 셋 중 하나. 동기화: blob 단일 thread 점유. */

	/* The blob in memory version of the blob matches the on disk
	 * version.
	 */
	SPDK_BLOB_STATE_CLEAN,
	/* [한국어] CLEAN: 메모리 = 디스크. 추가 sync 불필요.
	 * 설정자: blob_persist_complete (sync 완료 시), blob_load_complete.
	 * 읽는 자: sync 함수가 클린이면 즉시 콜백 호출 (no-op).
	 * 값 범위: enum 셋. 동기화: blob 단일 thread. */

	/* The in-memory state being synchronized with the on-disk
	 * blob state. */
	SPDK_BLOB_STATE_LOADING,
	/* [한국어] LOADING: 디스크 → 메모리 로드 중간 상태 (또는 sync 진행 중).
	 * 이 상태에서 신규 작업이 들어오면 큐잉 또는 거부.
	 * 설정자: spdk_bs_open_blob 시작 시. 읽는 자: 작업 가능 여부 판정.
	 * 값 범위: enum 셋. 동기화: blob 단일 thread. */
};

TAILQ_HEAD(spdk_xattr_tailq, spdk_xattr);
/* [한국어] xattr 헤드 타입 정의 — spdk_blob의 xattrs/xattrs_internal 필드가 이 타입. */

/*
 * [한국어]
 * struct spdk_blob_list — snapshot의 자식(clone) 트리 노드.
 * 한 snapshot에 여러 clone이 매달릴 수 있고, 각 clone도 다시 snapshot이 되어 자식을 가질 수 있다.
 * blobstore 전체에서는 spdk_blob_store::snapshots 리스트가 root snapshots를 관리.
 */
struct spdk_blob_list {
	spdk_blob_id id;
	/* [한국어] 이 노드가 표현하는 blob의 ID.
	 * 설정자: snapshot/clone 관계 구축 코드. 읽는 자: lookup 함수들.
	 * 값 범위: 유효 blob_id. 동기화: blobstore 메타 thread (md_thread). */
	size_t clone_count;
	/* [한국어] 직속 자식(clone) 수.
	 * 설정자: clone 추가/삭제 코드. 읽는 자: 통계/CLI.
	 * 값 범위: 0 이상. 동기화: md_thread. */
	TAILQ_HEAD(, spdk_blob_list) clones;
	/* [한국어] 이 snapshot의 자식 clone들 (각 자식도 spdk_blob_list 노드).
	 * 설정자/읽는 자: TAILQ_INSERT/REMOVE를 호출하는 clone 관리 코드.
	 * 값 범위: 0개 이상의 자식. 동기화: md_thread. */
	TAILQ_ENTRY(spdk_blob_list) link;
	/* [한국어] 부모의 clones 리스트 또는 blobstore의 snapshots 리스트에 매달리는 링크.
	 * 설정자/읽는 자: TAILQ_INSERT/REMOVE. 동기화: md_thread. */
};

/*
 * [한국어]
 * struct spdk_blob — blob의 in-memory 표현. blobstore의 가장 핵심적인 객체.
 * dual mut_data(clean/active)로 트랜잭션 sync을 안전하게 수행.
 * RB tree(open_blobs)에 매달려 O(log n) 조회 가능.
 * 동기화: 각 blob은 자기 소유 thread에서 다뤄지며 (open_ref 카운트로 lifetime 보호),
 * 메타 sync는 md_thread에서 수행되므로 thread 간 메시지 전달이 필요할 수 있다.
 */
struct spdk_blob {
	struct spdk_blob_store *bs;
	/* [한국어] 이 blob이 속한 blobstore.
	 * 설정자: spdk_bs_open_blob/create_blob. 읽는 자: 거의 모든 blob 함수가 bs->dev/io_units_per_cluster 등 접근.
	 * 값 범위: 유효 blobstore 포인터 (NULL 불가).
	 * 동기화: lifetime은 blob 사용 중 보장됨. */

	uint32_t	open_ref;
	/* [한국어] 사용자 open 카운트. 0이 되면 blob이 메모리에서 해제될 수 있음.
	 * 설정자: spdk_bs_open_blob에서 ++, spdk_blob_close에서 --.
	 * 읽는 자: blob 라이프사이클 관리 코드.
	 * 값 범위: 0 이상. 동기화: blob 단일 thread (open/close가 같은 thread에서 일어남 가정). */

	spdk_blob_id	id;
	/* [한국어] 이 blob의 고유 ID. 하위 32비트 = 첫 메타페이지 인덱스, 상위 비트 = HIGH_BIT 마커.
	 * 설정자: blob 생성 시 1회. 읽는 자: 모든 lookup/직렬화.
	 * 값 범위: 유효 blob_id, SPDK_BLOBID_INVALID 아님.
	 * 동기화: 불변 — 생성 후 변경 불가. */

	spdk_blob_id	parent_id;
	/* [한국어] 부모(snapshot) blob의 ID. snapshot/clone 관계가 없으면 SPDK_BLOBID_INVALID.
	 * 설정자: snapshot/clone 생성 코드. 읽는 자: parent traversal, esnap 등.
	 * 값 범위: 유효 blob_id 또는 INVALID.
	 * 동기화: 라이프사이클 이벤트(snapshot/clone)에서만 변경 — md_thread. */

	enum spdk_blob_state		state;
	/* [한국어] DIRTY/CLEAN/LOADING 상태.
	 * 설정자: 메모리 수정/디스크 sync/load 코드.
	 * 읽는 자: sync 함수가 dirty 여부 검사.
	 * 값 범위: enum spdk_blob_state. 동기화: blob 단일 thread. */

	/* Two copies of the mutable data. One is a version
	 * that matches the last known data on disk (clean).
	 * The other (active) is the current data. Syncing
	 * a blob makes the clean match the active.
	 */
	struct spdk_blob_mut_data	clean;
	/* [한국어] 디스크와 일치하는 직전 클린 복사본. crash 시 이 상태로 복구 가능.
	 * 설정자: blob_persist_complete (sync 성공 시 active를 clean으로 복사).
	 * 읽는 자: rollback 경로 (sync 실패 시 active를 clean으로 복원).
	 * 값 범위: spdk_blob_mut_data. 동기화: md_thread만 변경. */
	struct spdk_blob_mut_data	active;
	/* [한국어] 현재(활성) 메모리 상태. 사용자 수정은 여기 반영.
	 * 설정자: 모든 blob 메모리 수정 함수 (resize/insert_cluster/etc).
	 * 읽는 자: I/O 경로의 LBA 변환 등 거의 모든 곳.
	 * 값 범위: spdk_blob_mut_data. 동기화: blob 단일 thread. */

	bool		invalid;
	/* [한국어] blob이 invalid_flags 때문에 사용 불가 상태.
	 * 설정자: blob_load 시 알 수 없는 invalid_flag 발견하면 true.
	 * 읽는 자: spdk_bs_open_blob이 invalid면 거부.
	 * 값 범위: true/false. 동기화: 로드 시점 1회 결정 — 변경 없음. */
	bool		data_ro;
	/* [한국어] blob의 데이터가 read-only인지 (snapshot, READ_ONLY flag 등).
	 * 설정자: 생성/load. 읽는 자: write 경로가 거부 판정.
	 * 값 범위: true/false. 동기화: 라이프사이클 이벤트에서만 변경. */
	bool		md_ro;
	/* [한국어] 메타데이터(xattr/flags 등)가 read-only인지.
	 * 설정자: 생성/load. 읽는 자: set_xattr, sync_md 등.
	 * 값 범위: true/false. 동기화: 위와 같음. */

	uint64_t	invalid_flags;
	/* [한국어] 디스크에서 읽은 invalid_flags 비트맵 — 알 수 없는 플래그 있으면 invalid=true.
	 * SPDK_BLOB_INVALID_FLAGS_MASK로 알려진 플래그 검사.
	 * 설정자: blob_load. 읽는 자: 검증 로직, sync 시 다시 직렬화.
	 * 값 범위: SPDK_BLOB_THIN_PROV/INTERNAL_XATTR/EXTENT_TABLE/EXTERNAL_SNAPSHOT 비트 조합.
	 * 동기화: load 후 변경되지 않음. */
	uint64_t	data_ro_flags;
	/* [한국어] data read-only를 강제하는 플래그 비트맵 (예: SPDK_BLOB_READ_ONLY).
	 * 설정자/읽는 자: 위와 같음. 동기화: 위와 같음. */
	uint64_t	md_ro_flags;
	/* [한국어] md read-only 강제 플래그 (예: clear_method 비트들).
	 * 설정자/읽는 자: 위와 같음. 동기화: 위와 같음. */

	struct spdk_bs_dev *back_bs_dev;
	/* [한국어] backing 디바이스 — thin blob의 미할당 영역 read 위임처.
	 * NULL = thick blob (모두 자체 할당), &g_zeroes_bs_dev = zero-backed thin blob,
	 * 다른 spdk_blob_bs_dev = snapshot/clone 관계.
	 * 설정자: 생성/snapshot 설정 시. 읽는 자: read 경로의 미할당 영역 처리.
	 * 값 범위: NULL 또는 유효 bs_dev. 동기화: 라이프사이클 이벤트에서만 변경. */

	/* TODO: The xattrs are mutable, but we don't want to be
	 * copying them unnecessarily. Figure this out.
	 */
	struct spdk_xattr_tailq xattrs;
	/* [한국어] 사용자 xattr TAILQ — spdk_blob_set/get_xattr API로 접근.
	 * 설정자: set_xattr이 INSERT, remove_xattr이 REMOVE. 읽는 자: get_xattr/직렬화.
	 * 값 범위: 0개 이상의 spdk_xattr 노드. 동기화: blob 단일 thread. */
	struct spdk_xattr_tailq xattrs_internal;
	/* [한국어] 내부용 xattr — SNAP/EXTSNAP 같은 SPDK 내부 키 보관 (사용자에게 비노출).
	 * 설정자: 내부 코드만 (snapshot 마커 등). 읽는 자: lvol/snapshot 인지 판정.
	 * 값 범위: 0개 이상. 동기화: md_thread. */

	RB_ENTRY(spdk_blob) link;
	/* [한국어] spdk_blob_store::open_blobs RB tree에 매달리는 링크.
	 * 설정자: open 시 RB_INSERT, close 시 RB_REMOVE.
	 * 읽는 자: id로 lookup 시 RB_FIND.
	 * 값 범위: open 상태에서만 유효. 동기화: md_thread (open_blobs 조작). */

	uint32_t frozen_refcnt;
	/* [한국어] blob freeze 카운트 — snapshot 생성 등 동안 새 I/O를 차단하기 위한 게이트.
	 * 설정자: blob_freeze_io에서 ++, blob_unfreeze_io에서 --.
	 * 읽는 자: I/O submit 코드가 0이 아니면 큐잉.
	 * 값 범위: 0 이상. 동기화: blob 단일 thread (다만 cross-thread freeze는 메시지로 전달). */
	bool locked_operation_in_progress;
	/* [한국어] resize/snapshot 등 mutex처럼 단일 사용자만 진행해야 하는 op 진행 중인지 플래그.
	 * 설정자: 해당 op 시작 시 true, 종료 시 false. 읽는 자: 동시 op 거부.
	 * 값 범위: true/false. 동기화: blob 단일 thread. */
	enum blob_clear_method clear_method;
	/* [한국어] 클러스터 해제 시 정책 (DEFAULT/NONE/UNMAP/WRITE_ZEROES).
	 * 설정자: blob 생성 옵션. 읽는 자: cluster 해제 코드.
	 * 값 범위: enum. 동기화: 생성 시 결정 — 변경 없음. */
	bool extent_rle_found;
	/* [한국어] 메타 로드 시 EXTENT_RLE descriptor가 발견되었는지.
	 * 설정자: blob_parse 시. 읽는 자: 디스크 포맷 호환성 판정.
	 * 값 범위: true/false. 동기화: load 시 결정. */
	bool extent_table_found;
	/* [한국어] EXTENT_TABLE descriptor 발견 여부.
	 * 설정자/읽는 자: 위와 같음. 값 범위: true/false. 동기화: 위와 같음. */
	bool use_extent_table;
	/* [한국어] EXTENT_TABLE 모드를 사용할지 (RLE 대신). 큰 blob에 더 효율적.
	 * 설정자: 생성 옵션 또는 load 시. 읽는 자: 직렬화 코드의 분기.
	 * 값 범위: true/false. 동기화: 생성 시 결정. */

	/* A list of pending metadata pending_persists */
	TAILQ_HEAD(, spdk_blob_persist_ctx) pending_persists;
	/* [한국어] 대기 중인 메타 persist 컨텍스트 큐 — sync 호출이 진행 중일 때 추가 sync는 여기 대기.
	 * 설정자/읽는 자: blob_sync_md 코드. 값 범위: 0개 이상. 동기화: md_thread. */
	TAILQ_HEAD(, spdk_blob_persist_ctx) persists_to_complete;
	/* [한국어] 완료 대기 중인 persist 컨텍스트 — 진행 중인 sync가 끝나면 일괄 콜백 호출 대상.
	 * 설정자/읽는 자: blob_sync_md 완료 처리. 동기화: md_thread. */

	/* Number of data clusters retrieved from extent table,
	 * that many have to be read from extent pages. */
	uint64_t	remaining_clusters_in_et;
	/* [한국어] EXTENT_TABLE 모드에서 아직 extent page 로드를 완료하지 않은 클러스터 수.
	 * load 진행 추적용 카운터 — 0이 되면 모든 extent page 로딩 완료.
	 * 설정자: blob_load 시작/진행. 읽는 자: load 완료 판정.
	 * 값 범위: 0 ~ num_clusters. 동기화: load 중인 thread (보통 md_thread). */
};

/*
 * [한국어]
 * struct spdk_blob_store — blobstore 인스턴스 전체 상태.
 * 메타 영역 위치, 클러스터 풀, 채널, 모든 open blob 등을 관리. 전역적으로 lockless 설계가
 * 목표지만 클러스터/메타페이지 비트맵은 멀티 thread 접근이 가능하므로 used_lock(spinlock) 사용.
 * md_thread에 메타 작업이 affined되어 있어 대부분의 메타 변경은 lockless 처리.
 */
struct spdk_blob_store {
	uint64_t			md_start; /* Offset from beginning of disk, in pages */
	/* [한국어] 메타 영역 시작 page 오프셋 (디스크 초반에서). 슈퍼블록 → used masks → md_start 순으로 배치.
	 * 설정자: spdk_bs_init/load. 읽는 자: bs_md_page_to_lba 등.
	 * 값 범위: 항상 양수 (슈퍼블록 + 마스크 영역 다음).
	 * 동기화: 초기화 후 불변 — 락 불필요. */
	uint32_t			md_len; /* Count, in pages */
	/* [한국어] 메타 영역 page 수 — 디스크 크기와 cluster_sz로부터 계산.
	 * 설정자: init 시. 읽는 자: 메타 인덱스 경계 검사.
	 * 값 범위: > 0. 동기화: 초기화 후 불변. */
	uint32_t                        md_page_size; /* Metadata page size */
	/* [한국어] 메타페이지 한 개 크기 (보통 4KiB = SPDK_BS_PAGE_SIZE).
	 * 설정자: init/load. 읽는 자: page → LBA 변환. 값 범위: 4096 (현재 고정).
	 * 동기화: 초기화 후 불변. */

	struct spdk_io_channel		*md_channel;
	/* [한국어] 메타데이터 전용 I/O 채널 — md_thread에서만 사용.
	 * 설정자: bs_init/load. 읽는 자: 메타 sync 코드.
	 * 값 범위: 유효 채널 (NULL 불가 in 정상 상태).
	 * 동기화: md_thread만 사용 → 락 불필요. */
	uint32_t			max_channel_ops;
	/* [한국어] 일반 채널의 최대 동시 op 수 — bs_alloc_io_channel 시 풀 크기 결정.
	 * 설정자: 사용자 옵션. 읽는 자: 채널 초기화.
	 * 값 범위: > 0. 동기화: 초기화 후 불변. */

	struct spdk_thread		*md_thread;
	/* [한국어] 메타데이터 작업이 affined된 SPDK thread.
	 * 설정자: bs_init/load. 읽는 자: cross-thread 메타 작업 시 spdk_thread_send_msg로 디스패치.
	 * 값 범위: 유효 thread. 동기화: 라이프사이클 동안 불변. */

	struct spdk_bs_dev		*dev;
	/* [한국어] backing 블록 디바이스 — bdev_nvme/aio/malloc 등이 spdk_bs_dev로 노출됨.
	 * 설정자: bs_init/load. 읽는 자: 모든 디스크 I/O. 값 범위: 유효 bs_dev.
	 * 동기화: 라이프사이클 동안 불변. */

	struct spdk_bit_array		*used_md_pages;		/* Protected by used_lock */
	/* [한국어] 사용 중인 메타페이지 비트맵 (1bit/page).
	 * 설정자/읽는 자: 메타페이지 할당/해제 코드.
	 * 값 범위: md_len개 비트. 동기화: used_lock 보호. */
	struct spdk_bit_pool		*used_clusters;		/* Protected by used_lock */
	/* [한국어] 사용 중 클러스터 비트풀 — 빈 비트 검색을 빠르게 하는 자료구조.
	 * 설정자/읽는 자: 클러스터 할당/해제. 값 범위: total_clusters 비트.
	 * 동기화: used_lock 보호. */
	struct spdk_bit_array		*used_blobids;
	/* [한국어] 사용 중인 blob_id 비트맵 — 새 blob_id 할당 시 충돌 방지.
	 * 설정자/읽는 자: blob 생성/삭제. 값 범위: blob_id space 비트맵.
	 * 동기화: md_thread 단일 thread. */
	struct spdk_bit_array		*open_blobids;
	/* [한국어] 현재 메모리에 로드된 blob_id 비트맵 (open_blobs RB tree와 함께 사용).
	 * 설정자/읽는 자: open/close. 동기화: md_thread. */

	struct spdk_spinlock		used_lock;
	/* [한국어] used_md_pages, used_clusters, num_free_clusters를 보호하는 스핀락.
	 * 멀티 thread에서 클러스터 할당이 일어날 수 있어 필요. SPDK spinlock은 polled-mode 친화적.
	 * 설정자: spdk_spin_init at bs_init. 읽는 자: 위 비트맵/카운터 갱신 코드.
	 * 동기화: 자기 자신이 동기화 메커니즘. */

	uint32_t			cluster_sz;
	/* [한국어] 클러스터 크기(바이트). 모든 변환 공식의 기반.
	 * 설정자: init. 값 범위: 4KiB의 배수, 보통 1MiB~4MiB. 동기화: 불변. */
	uint64_t			total_clusters;
	/* [한국어] 전체 클러스터 수 (메타 포함). 설정자: init. 동기화: 불변. */
	uint64_t			total_data_clusters;
	/* [한국어] 데이터로 사용 가능한 클러스터 수 (메타 영역 제외).
	 * 설정자: init. 읽는 자: spdk_bs_get_cluster_count API. 동기화: 불변. */
	uint64_t			num_free_clusters;	/* Protected by used_lock */
	/* [한국어] 현재 빈 데이터 클러스터 수.
	 * 설정자: 클러스터 할당/해제. 읽는 자: spdk_bs_free_cluster_count API.
	 * 동기화: used_lock 보호. */
	uint64_t			pages_per_cluster;
	/* [한국어] 클러스터당 메타페이지 수 — cluster_sz / md_page_size.
	 * 설정자: init. 읽는 자: 변환 함수들. 동기화: 불변. */
	uint64_t			io_units_per_cluster;
	/* [한국어] 클러스터당 io_unit 수 — cluster_sz / io_unit_size.
	 * 설정자: init. 읽는 자: 거의 모든 변환 함수. 동기화: 불변. */
	uint8_t				pages_per_cluster_shift;
	/* [한국어] pages_per_cluster가 2의 거듭제곱일 때 shift 값(log2). 0이면 거듭제곱 아님 → 나눗셈 사용.
	 * 빠른 비트시프트로 변환을 가속. 설정자: init. 동기화: 불변. */
	uint8_t				io_units_per_cluster_shift;
	/* [한국어] io_units_per_cluster의 log2 (또는 0 = 거듭제곱 아님).
	 * 설정자: init. 읽는 자: bs_blob_io_unit_to_lba 등 빠른 경로. 동기화: 불변. */
	uint32_t			io_unit_size;
	/* [한국어] io_unit 크기(바이트) — 보통 디바이스 blocklen과 같거나 그 배수.
	 * blob의 read/write 단위. 설정자: init. 동기화: 불변. */

	spdk_blob_id			super_blob;
	/* [한국어] 사용자가 지정한 "슈퍼" blob ID — blobstore 내 특별한 blob을 가리킴 (없으면 INVALID).
	 * 설정자: spdk_bs_set_super. 읽는 자: spdk_bs_get_super. 동기화: md_thread. */
	struct spdk_bs_type		bstype;
	/* [한국어] blobstore 타입 식별자 (16바이트 문자열) — 슈퍼블록에 저장.
	 * 설정자: init/load. 읽는 자: load 시 검증. 동기화: 불변. */

	struct spdk_bs_cpl		unload_cpl;
	/* [한국어] unload 진행 중인 사용자 콜백 — esnap 채널 destroy 완료 대기 시 사용.
	 * 설정자: spdk_bs_unload. 읽는 자: 채널 정리 완료 콜백. 동기화: md_thread. */
	int				unload_err;
	/* [한국어] unload 누적 에러. 설정자/읽는 자: unload 코드. 동기화: md_thread. */

	RB_HEAD(spdk_blob_tree, spdk_blob) open_blobs;
	/* [한국어] 현재 open된 blob들의 RB tree (id 정렬). lookup O(log n).
	 * 설정자/읽는 자: open/close. 동기화: md_thread. */
	TAILQ_HEAD(, spdk_blob_list)	snapshots;
	/* [한국어] root snapshot 리스트 — 자식 clones는 각 snapshot 안의 TAILQ에.
	 * 설정자/읽는 자: snapshot/clone 관리. 동기화: md_thread. */

	bool				clean;
	/* [한국어] 마지막 unmount가 정상이었는지 (super_block.clean 비트와 동기화).
	 * load 시 이 값이 false면 dirty shutdown으로 간주, 메타 검증/복구 모드.
	 * 설정자: load(super 읽음), unmount(true 기록). 읽는 자: 진단/복구.
	 * 값 범위: true/false. 동기화: load/unmount 시 1회. */

	spdk_bs_esnap_dev_create	esnap_bs_dev_create;
	/* [한국어] 외부 snapshot용 bs_dev 생성 콜백 — 사용자가 등록.
	 * 설정자: spdk_bs_load 옵션. 읽는 자: esnap clone 로드 시 호출.
	 * 값 범위: NULL(esnap 미사용) 또는 사용자 함수. 동기화: 불변(등록 후). */
	void				*esnap_ctx;
	/* [한국어] esnap 콜백에 전달될 사용자 컨텍스트.
	 * 설정자: 사용자. 읽는 자: 콜백 호출 시 함께 전달. 값 범위: NULL 허용.
	 * 동기화: 불변. */

	/* If external snapshot channels are being destroyed while
	 * the blobstore is unloaded, the unload is deferred until
	 * after the channel destruction completes.
	 */
	uint32_t			esnap_channels_unloading;
	/* [한국어] 현재 destroy 진행 중인 esnap 채널 수. 0이 되면 unload 진행 가능.
	 * 설정자: 채널 destroy 시작/완료 시 ++/--.
	 * 읽는 자: unload 코드가 이 카운트가 0이 될 때까지 대기.
	 * 값 범위: 0 이상. 동기화: md_thread. */
	spdk_bs_op_complete		esnap_unload_cb_fn;
	/* [한국어] 채널 destroy 완료 후 호출할 unload 콜백.
	 * 설정자: spdk_bs_unload. 읽는 자: 채널 destroy 완료 핸들러. 동기화: md_thread. */
	void				*esnap_unload_cb_arg;
	/* [한국어] unload 콜백 인자.
	 * 설정자/읽는 자: 위와 같음. 동기화: md_thread. */
};

/*
 * [한국어]
 * struct spdk_bs_channel — per-thread blobstore I/O 채널 컨텍스트.
 * 사용자가 spdk_bs_alloc_io_channel을 호출하면 spdk_io_channel 끝에 이 ctx가 따라붙는다.
 * 채널은 thread-affined이므로 모든 필드는 단일 thread 점유 → lockless.
 * request set 풀(req_mem)을 미리 통째로 할당해두고 reqs TAILQ에 free list로 노출.
 */
struct spdk_bs_channel {
	struct spdk_bs_request_set	*req_mem;
	/* [한국어] 풀의 메모리 백킹 — calloc(max_channel_ops, sizeof(set))로 할당된 큰 배열의 시작.
	 * 채널 destroy 시 free 대상.
	 * 설정자: 채널 생성. 읽는 자: 채널 destroy. 동기화: 채널 thread. */
	TAILQ_HEAD(, spdk_bs_request_set) reqs;
	/* [한국어] free request set 리스트 — req_mem 안의 set들이 사용 중이 아닐 때 여기 매달림.
	 * 사용 시작: TAILQ_REMOVE(첫 set), 사용 종료: TAILQ_INSERT_TAIL.
	 * 설정자/읽는 자: bs_sequence_start, bs_request_set_complete 등.
	 * 값 범위: 0 ~ max_channel_ops 개. 동기화: 채널 thread → lockless. */

	struct spdk_blob_store		*bs;
	/* [한국어] 이 채널이 속한 blobstore 포인터 — 편의 접근용.
	 * 설정자: 채널 생성. 읽는 자: I/O 처리 코드. 동기화: 라이프사이클 동안 불변. */

	struct spdk_bs_dev		*dev;
	/* [한국어] 채널이 사용할 main bs_dev — 보통 bs->dev와 동일.
	 * 설정자: 채널 생성. 읽는 자: bs_sequence_*_dev. 동기화: 불변. */
	struct spdk_io_channel		*dev_channel;
	/* [한국어] bs_dev 백엔드(bdev)에 발행할 io_channel — bdev_module이 사용.
	 * 설정자: 채널 생성 시 bs_dev->create_channel. 읽는 자: bs_dev->read/write 호출.
	 * 동기화: 채널 thread. */

	/* This page is only used during insert of a new cluster. */
	struct spdk_blob_md_page	*new_cluster_page;
	/* [한국어] 새 클러스터 추가 시 메타페이지 작성에 쓰일 임시 버퍼 (4KiB).
	 * 채널당 1개 미리 할당 — 매번 alloc/free하지 않기 위함.
	 * 설정자: 채널 생성 시 spdk_zmalloc. 읽는 자: blob_insert_cluster 코드.
	 * 동기화: 채널 thread, op 직렬 처리 → 동시 사용 없음. */

	TAILQ_HEAD(, spdk_bs_request_set) need_cluster_alloc;
	/* [한국어] 클러스터 할당 대기 중인 request set 큐 — thin blob write가 클러스터 할당을
	 * 비동기로 기다릴 때 여기 매달리고, 할당 완료 후 dequeue되어 실행.
	 * 설정자/읽는 자: blob_request_submit_op_split의 thin write 분기.
	 * 동기화: 채널 thread. */
	TAILQ_HEAD(, spdk_bs_request_set) queued_io;
	/* [한국어] 일시적으로 미뤄진 I/O 큐 (예: blob freeze 동안 새 I/O 큐잉).
	 * 설정자/읽는 자: freeze/unfreeze 코드. 동기화: 채널 thread. */

	/* This page is only used during release of an existing cluster. */
	struct spdk_blob_md_page        *release_cluster_page;
	/* [한국어] 클러스터 해제 시 메타페이지 갱신용 임시 버퍼.
	 * 설정자: 채널 생성. 읽는 자: blob_remove_cluster. 동기화: 채널 thread. */

	RB_HEAD(blob_esnap_channel_tree, blob_esnap_channel) esnap_channels;
	/* [한국어] esnap blob별 외부 디바이스 채널 RB tree (blob_id → channel).
	 * lazy 할당: esnap blob에 처음 I/O가 일어날 때 채널 생성, 트리에 삽입.
	 * 설정자/읽는 자: blob_esnap_get_io_channel.
	 * 값 범위: 0개 이상의 esnap 채널 노드. 동기화: 채널 thread. */
};

/** operation type */
/*
 * [한국어]
 * enum spdk_blob_op_type — user_op이 표현할 수 있는 사용자 I/O 종류.
 * bs_user_op_alloc/execute에서 switch 분기 키로 사용.
 */
enum spdk_blob_op_type {
	SPDK_BLOB_WRITE,
	/* [한국어] 단일 buffer write. */
	SPDK_BLOB_READ,
	/* [한국어] 단일 buffer read. */
	SPDK_BLOB_UNMAP,
	/* [한국어] UNMAP/Deallocate (NVMe DSM). */
	SPDK_BLOB_WRITE_ZEROES,
	/* [한국어] WRITE_ZEROES (NVMe opcode 0x08). */
	SPDK_BLOB_WRITEV,
	/* [한국어] vectored write (iovec 배열). */
	SPDK_BLOB_READV,
	/* [한국어] vectored read (iovec 배열). */
};

/* back bs_dev */

#define BLOB_SNAPSHOT "SNAP"
/* [한국어] 내부 xattr 키 — 이 blob이 snapshot임을 표시. */
#define SNAPSHOT_IN_PROGRESS "SNAPTMP"
/* [한국어] snapshot 생성 진행 중 마커 — crash recovery 시 미완료 snapshot 식별. */
#define SNAPSHOT_PENDING_REMOVAL "SNAPRM"
/* [한국어] 삭제 진행 중 snapshot 마커 — crash recovery 시 미완료 삭제 정리. */
#define BLOB_EXTERNAL_SNAPSHOT_ID "EXTSNAP"
/* [한국어] esnap blob 내부 xattr 키 — 외부 snapshot 식별자(uuid 등) 저장. */

/*
 * [한국어]
 * struct spdk_blob_bs_dev — blob을 spdk_bs_dev로 감싼 어댑터.
 * 첫 멤버가 spdk_bs_dev이므로 두 타입 간 캐스팅이 안전 (C 표준 첫 멤버 호환성).
 * blob_bs_dev.c의 모든 함수가 이 구조체를 사용.
 */
struct spdk_blob_bs_dev {
	struct spdk_bs_dev bs_dev;
	/* [한국어] spdk_bs_dev 인터페이스 본체 — 함수 포인터 테이블 + blockcnt/blocklen.
	 * 첫 멤버여야 (struct spdk_blob_bs_dev *)와 (struct spdk_bs_dev *) 캐스팅이 안전.
	 * 설정자: bs_create_blob_bs_dev. 읽는 자: 모든 bs_dev 사용 코드. 동기화: 라이프사이클 동안 불변. */
	struct spdk_blob *blob;
	/* [한국어] backing으로 사용되는 blob 포인터 (보통 snapshot/parent).
	 * 설정자: bs_create_blob_bs_dev. 읽는 자: blob_bs_dev_read/translate/destroy 등.
	 * 값 범위: 유효 blob (open 상태). 동기화: blob의 open_ref가 유지되는 동안 유효. */
};

/* On-Disk Data Structures
 *
 * The following data structures exist on disk.
 */
/* [한국어] 이하 #pragma pack(1)으로 패딩 없이 정의 — 디스크 직렬화 포맷이 비트 정확하도록.
 * 이 영역의 구조체 크기는 SPDK_STATIC_ASSERT로 컴파일 타임 검증된다. */
#define SPDK_BS_INITIAL_VERSION 1
/* [한국어] blobstore 디스크 포맷 초기 버전 (v1). 호환성 검사 시 사용. */
#define SPDK_BS_VERSION 3 /* current version */
/* [한국어] 현재 포맷 버전. 새 blobstore 생성 시 슈퍼블록에 기록.
 * v3 = EXTENT_TABLE/EXTENT_PAGE descriptor 도입 후. */

#pragma pack(push, 1)
/* [한국어] 패킹 1바이트 정렬로 전환 — 이후 디스크 구조체 정의에서 컴파일러가 자동 패딩 추가 금지.
 * pop은 #pragma pack(pop)으로. */

#define SPDK_MD_MASK_TYPE_USED_PAGES 0
/* [한국어] used_md_pages 비트맵 마커. */
#define SPDK_MD_MASK_TYPE_USED_CLUSTERS 1
/* [한국어] used_clusters 비트맵 마커. */
#define SPDK_MD_MASK_TYPE_USED_BLOBIDS 2
/* [한국어] used_blobids 비트맵 마커. */

/*
 * [한국어]
 * struct spdk_bs_md_mask — used_pages/clusters/blobids 비트맵의 디스크 직렬화 포맷.
 * 슈퍼블록에 위치 정보(used_*_mask_start/len)가 적혀 있고, 그 영역에 이 구조체들이 저장됨.
 */
struct spdk_bs_md_mask {
	uint8_t		type;
	/* [한국어] 비트맵 종류 식별자 — SPDK_MD_MASK_TYPE_*.
	 * 설정자: load 시 디스크에서 읽음, init 시 기록. 읽는 자: 검증.
	 * 값 범위: 0/1/2. 동기화: 디스크 직렬화 — load/save 시점만. */
	uint32_t	length; /* In bits */
	/* [한국어] 비트 수 (mask 배열의 비트 길이).
	 * 설정자/읽는 자: 위와 같음. 값 범위: 0 ~ total_clusters/pages. */
	uint8_t		mask[0];
	/* [한국어] flexible array — 실제 비트맵 데이터. ceil(length/8) 바이트.
	 * 설정자/읽는 자: 직렬화/역직렬화. 동기화: load/save 시점만. */
};

#define SPDK_MD_DESCRIPTOR_TYPE_PADDING 0
/* [한국어] 메타페이지 잔여 영역 패딩 descriptor — 의미 없음, 끝까지 채우기용. */
#define SPDK_MD_DESCRIPTOR_TYPE_XATTR 2
/* [한국어] 사용자 xattr descriptor. */
#define SPDK_MD_DESCRIPTOR_TYPE_FLAGS 3
/* [한국어] blob의 invalid/data_ro/md_ro flags descriptor. */
#define SPDK_MD_DESCRIPTOR_TYPE_XATTR_INTERNAL 4
/* [한국어] 내부용 xattr (SNAP/EXTSNAP 등) — 사용자에게 비노출. */

/* Following descriptors define cluster layout in a blob.
 * EXTENT_RLE cannot be present in blobs metadata,
 * at the same time as EXTENT_TABLE and EXTENT_PAGE descriptors. */

/* EXTENT_RLE descriptor holds an array of LBA that points to
 * beginning of allocated clusters. The array is run-length encoded,
 * with 0's being unallocated clusters. It is part of serialized
 * metadata chain for a blob. */
#define SPDK_MD_DESCRIPTOR_TYPE_EXTENT_RLE 1
/* [한국어] EXTENT_RLE: 클러스터 LBA 배열을 RLE 인코딩 (작은 blob에 효율적). */
/* EXTENT_TABLE descriptor holds array of md page offsets that
 * point to pages with EXTENT_PAGE descriptor. The 0's in the array
 * are run-length encoded, non-zero values are unallocated pages.
 * It is part of serialized metadata chain for a blob. */
#define SPDK_MD_DESCRIPTOR_TYPE_EXTENT_TABLE 5
/* [한국어] EXTENT_TABLE: 큰 blob을 위한 두-단계 인덱스 (table → extent_page → cluster). */
/* EXTENT_PAGE descriptor holds an array of LBAs that point to
 * beginning of allocated clusters. The array is run-length encoded,
 * with 0's being unallocated clusters. It is NOT part of
 * serialized metadata chain for a blob. */
#define SPDK_MD_DESCRIPTOR_TYPE_EXTENT_PAGE 6
/* [한국어] EXTENT_PAGE: EXTENT_TABLE이 가리키는 별도 메타페이지. blob 체인에 없음. */

/*
 * [한국어]
 * struct spdk_blob_md_descriptor_xattr — XATTR descriptor 포맷.
 * descriptor 체인의 한 항목으로 메타페이지에 직렬화됨.
 */
struct spdk_blob_md_descriptor_xattr {
	uint8_t		type;
	/* [한국어] descriptor 타입 — SPDK_MD_DESCRIPTOR_TYPE_XATTR(2) 또는 _XATTR_INTERNAL(4).
	 * 설정자/읽는 자: 직렬화/파싱. 값 범위: 2 또는 4. 동기화: 직렬화 시점. */
	uint32_t	length;
	/* [한국어] 이 descriptor 전체 길이 (헤더 + name + value, type/length 필드 제외 본문 길이).
	 * 설정자/읽는 자: 파서가 다음 descriptor로 넘어가는 데 사용.
	 * 값 범위: 0 < length < 메타페이지 잔여. */

	uint16_t	name_length;
	/* [한국어] xattr 이름 길이 (바이트). 값 범위: 0 ~ UINT16_MAX. */
	uint16_t	value_length;
	/* [한국어] xattr 값 길이 (바이트). 값 범위: 0 ~ UINT16_MAX. */

	char		name[0];
	/* String name immediately followed by string value. */
	/* [한국어] flexible array — name 문자열 그 뒤로 곧장 value 바이너리.
	 * 즉 메모리 레이아웃: [type][length][name_len][value_len][name 바이트][value 바이트].
	 * value의 시작 주소 = name + name_length. */
};

/*
 * [한국어]
 * struct spdk_blob_md_descriptor_extent_rle — EXTENT_RLE descriptor 포맷.
 * 작은 blob의 클러스터 매핑을 RLE로 직렬화. extents[]는 (cluster_idx, length) 페어 배열.
 * length 단위는 클러스터, cluster_idx=0이면 미할당 run을 표현.
 */
struct spdk_blob_md_descriptor_extent_rle {
	uint8_t		type;
	/* [한국어] descriptor 타입 = SPDK_MD_DESCRIPTOR_TYPE_EXTENT_RLE(1).
	 * 설정자/읽는 자: 직렬화/파싱. 값 범위: 1 고정. */
	uint32_t	length;
	/* [한국어] 본문 바이트 길이. 설정자/읽는 자: 파서. */

	struct {
		uint32_t	cluster_idx;
		/* [한국어] 클러스터 시작 인덱스 (디스크 좌표). 0이면 미할당 run.
		 * 설정자/읽는 자: 직렬화. 값 범위: 0 또는 유효 인덱스. */
		uint32_t	length; /* In units of clusters */
		/* [한국어] 이 run의 클러스터 수.
		 * 설정자/읽는 자: 직렬화. 값 범위: > 0. */
	} extents[0];
	/* [한국어] flexible array — descriptor 길이로부터 추출되는 extent run 리스트.
	 * 설정자/읽는 자: 메타 직렬화/파싱. 동기화: 디스크 I/O 시점만. */
};

/*
 * [한국어]
 * struct spdk_blob_md_descriptor_extent_table — EXTENT_TABLE descriptor 포맷.
 * 큰 blob에서 사용. 각 extent_page[i]는 별도 EXTENT_PAGE descriptor가 들어있는 메타페이지를 가리킴.
 */
struct spdk_blob_md_descriptor_extent_table {
	uint8_t		type;
	/* [한국어] descriptor 타입 = 5 (EXTENT_TABLE). */
	uint32_t	length;
	/* [한국어] 본문 길이. */

	/* Number of data clusters in the blob */
	uint64_t	num_clusters;
	/* [한국어] blob의 총 클러스터 수 (load 시 검증).
	 * 설정자/읽는 자: 직렬화/파싱. 값 범위: 0 이상. */

	struct {
		uint32_t	page_idx;
		/* [한국어] EXTENT_PAGE descriptor가 있는 메타페이지 인덱스. 0 = 미할당.
		 * 설정자/읽는 자: 직렬화/파싱. 값 범위: 0 또는 유효 page_idx. */
		uint32_t	num_pages; /* In units of pages */
		/* [한국어] 이 run의 page 개수 (RLE: 0 run 인코딩).
		 * 설정자/읽는 자: 위와 같음. 값 범위: > 0. */
	} extent_page[0];
	/* [한국어] flexible array — extent page 매핑 run 배열. */
};

/*
 * [한국어]
 * struct spdk_blob_md_descriptor_extent_page — EXTENT_PAGE descriptor 포맷.
 * EXTENT_TABLE이 가리키는 별도 메타페이지에 들어 있음 (blob 체인에 없음).
 * cluster_idx[]는 RLE — 0은 미할당 run.
 */
struct spdk_blob_md_descriptor_extent_page {
	uint8_t		type;
	/* [한국어] descriptor 타입 = 6 (EXTENT_PAGE). */
	uint32_t	length;
	/* [한국어] 본문 길이. */

	/* First cluster index in this extent page */
	uint32_t	start_cluster_idx;
	/* [한국어] 이 페이지가 담당하는 첫 클러스터의 blob 좌표.
	 * 설정자/읽는 자: 파싱 시 위치 식별. */

	uint32_t	cluster_idx[0];
	/* [한국어] flexible array — 클러스터 LBA 인덱스 배열 (RLE 인코딩).
	 * 설정자/읽는 자: 직렬화/파싱. */
};

#define SPDK_BLOB_THIN_PROV		(1ULL << 0)
/* [한국어] thin-provisioned blob — 미할당 영역은 backing(zeroes/snapshot)으로 위임. */
#define SPDK_BLOB_INTERNAL_XATTR	(1ULL << 1)
/* [한국어] 내부 xattr 사용 — XATTR_INTERNAL descriptor 존재. */
#define SPDK_BLOB_EXTENT_TABLE		(1ULL << 2)
/* [한국어] EXTENT_TABLE 모드 사용 (큰 blob). */
#define SPDK_BLOB_EXTERNAL_SNAPSHOT	(1ULL << 3)
/* [한국어] 외부 snapshot blob (esnap). */
#define SPDK_BLOB_INVALID_FLAGS_MASK	(SPDK_BLOB_THIN_PROV | SPDK_BLOB_INTERNAL_XATTR | \
					 SPDK_BLOB_EXTENT_TABLE | SPDK_BLOB_EXTERNAL_SNAPSHOT)
/* [한국어] 알려진 invalid flags 비트 마스크 — 디스크에 이 마스크 외 비트가 켜져 있으면
 * 이 SPDK 빌드가 모르는 기능이므로 blob을 invalid로 처리. */

#define SPDK_BLOB_READ_ONLY (1ULL << 0)
/* [한국어] data read-only flag (예: snapshot). */
#define SPDK_BLOB_DATA_RO_FLAGS_MASK	SPDK_BLOB_READ_ONLY
/* [한국어] 알려진 data_ro flags 마스크. 외부 비트 → blob을 data_ro 모드로만 open 허용. */

#define SPDK_BLOB_CLEAR_METHOD_SHIFT 0
/* [한국어] clear_method 비트 시작 위치. */
#define SPDK_BLOB_CLEAR_METHOD (3ULL << SPDK_BLOB_CLEAR_METHOD_SHIFT)
/* [한국어] clear_method 마스크 (2비트 = 4가지 정책: DEFAULT/NONE/UNMAP/WRITE_ZEROES). */
#define SPDK_BLOB_MD_RO_FLAGS_MASK	SPDK_BLOB_CLEAR_METHOD
/* [한국어] 알려진 md_ro flags 마스크. */

/*
 * [한국어]
 * struct spdk_blob_md_descriptor_flags — FLAGS descriptor 포맷.
 * blob의 invalid/data_ro/md_ro flags를 메타페이지에 직렬화.
 * load 시 알려지지 않은 비트가 있으면 invalid 또는 read-only 모드로 처리.
 */
struct spdk_blob_md_descriptor_flags {
	uint8_t		type;
	/* [한국어] descriptor 타입 = SPDK_MD_DESCRIPTOR_TYPE_FLAGS(3). */
	uint32_t	length;
	/* [한국어] 본문 길이. */

	/*
	 * If a flag in invalid_flags is set that the application is not aware of,
	 *  it will not allow the blob to be opened.
	 */
	uint64_t	invalid_flags;
	/* [한국어] 알려지지 않은 비트가 켜져 있으면 blob open 거부.
	 * 설정자: 직렬화 시 spdk_blob.invalid_flags 그대로. 읽는 자: load 시 검증.
	 * 값 범위: SPDK_BLOB_INVALID_FLAGS_MASK 비트 조합. 동기화: 디스크 직렬화. */

	/*
	 * If a flag in data_ro_flags is set that the application is not aware of,
	 *  allow the blob to be opened in data_read_only and md_read_only mode.
	 */
	uint64_t	data_ro_flags;
	/* [한국어] 알려지지 않은 비트 → data_ro + md_ro 모드만 허용 (읽기 가능, 쓰기 거부).
	 * 설정자/읽는 자/값 범위: 위 패턴과 동일. 동기화: 디스크. */

	/*
	 * If a flag in md_ro_flags is set the application is not aware of,
	 *  allow the blob to be opened in md_read_only mode.
	 */
	uint64_t	md_ro_flags;
	/* [한국어] 알려지지 않은 비트 → md_ro 모드만 허용 (메타 변경 거부).
	 * 설정자/읽는 자/값 범위: 위와 같음. */
};

/*
 * [한국어]
 * struct spdk_blob_md_descriptor — 모든 descriptor의 공통 헤더(type + length).
 * 파서가 각 descriptor 시작에서 이 두 필드만 보고 다음으로 점프할 때 사용.
 */
struct spdk_blob_md_descriptor {
	uint8_t		type;
	/* [한국어] descriptor 타입 (SPDK_MD_DESCRIPTOR_TYPE_*). */
	uint32_t	length;
	/* [한국어] 본문 길이 — 파서가 다음 descriptor 위치 계산에 사용. */
};

#define SPDK_INVALID_MD_PAGE UINT32_MAX
/* [한국어] 메타페이지 인덱스 sentinel — "유효 페이지 없음" 의미. */

/*
 * [한국어]
 * struct spdk_blob_md_page — 4KiB 메타페이지 디스크 포맷.
 * blob 메타데이터는 이 페이지들의 체인 형태(next로 연결)로 저장된다.
 * 첫 페이지의 인덱스 = blob_id 하위 32비트.
 */
struct spdk_blob_md_page {
	spdk_blob_id     id;
	/* [한국어] 이 페이지가 속한 blob의 ID — 검증용 (corruption 감지).
	 * 설정자: 직렬화 시 blob->id. 읽는 자: load 시 검증.
	 * 값 범위: 유효 blob_id. */

	uint32_t	sequence_num;
	/* [한국어] 메타페이지 체인 내 순번 (0부터 시작) — 잘못된 순서로 읽혔는지 검증.
	 * 설정자: 직렬화. 읽는 자: load 시 검증. 값 범위: 0 ~ num_pages-1. */
	uint32_t	reserved0;
	/* [한국어] 예약 — 향후 확장용, 현재는 0으로 유지. */

	/* Descriptors here */
	uint8_t		descriptors[4072];
	/* [한국어] descriptor 체인이 들어가는 본문 영역 (4072 = 4096 - 24바이트 헤더/꼬리).
	 * descriptor들이 차례로 packing되며, 끝에 PADDING(0)으로 채움.
	 * 설정자: 직렬화 코드. 읽는 자: 파서. */

	uint32_t	next;
	/* [한국어] 다음 메타페이지 인덱스 — SPDK_INVALID_MD_PAGE면 체인 끝.
	 * 설정자: 직렬화 시 다음 페이지 결정 후 기록. 읽는 자: 체인 traversal. */
	uint32_t	crc;
	/* [한국어] CRC32 — 페이지 무결성 검증 (id ~ next까지 계산). corruption 감지.
	 * 설정자: 직렬화 마지막 단계. 읽는 자: load 시 검증. */
};
#define SPDK_BS_PAGE_SIZE 0x1000
/* [한국어] 메타페이지 크기 = 4096바이트 (4KiB). */
SPDK_STATIC_ASSERT(SPDK_BS_PAGE_SIZE == sizeof(struct spdk_blob_md_page), "Invalid md page size");
/* [한국어] 컴파일 타임 검증 — pack(1)이 제대로 적용되어 정확히 4KiB가 되었는지 확인.
 * 어긋나면 빌드 실패 → 디스크 포맷 호환성 깨짐을 조기에 발견. */

#define SPDK_BS_MAX_DESC_SIZE SPDK_SIZEOF_MEMBER(struct spdk_blob_md_page, descriptors)
/* [한국어] descriptor 영역 최대 크기 = 4072 (sizeof(descriptors)). */

/* Maximum number of extents a single Extent Page can fit.
 * For an SPDK_BS_PAGE_SIZE of 4K SPDK_EXTENTS_PER_EP would be 512. */
#define SPDK_EXTENTS_PER_EP_MAX ((SPDK_BS_MAX_DESC_SIZE - sizeof(struct spdk_blob_md_descriptor_extent_page)) / sizeof(uint32_t))
/* [한국어] 한 EXTENT_PAGE에 들어갈 수 있는 최대 extent 수 (이론상). */
#define SPDK_EXTENTS_PER_EP (spdk_align64pow2(SPDK_EXTENTS_PER_EP_MAX + 1) >> 1u)
/* [한국어] 실제 사용 값 — 가장 가까운 2의 거듭제곱으로 round down (shift 연산을 위해 거듭제곱 사용).
 * 4KiB 페이지 기준 보통 512가 됨. */

#define SPDK_BS_SUPER_BLOCK_SIG "SPDKBLOB"
/* [한국어] 슈퍼블록 매직 문자열 (8바이트) — load 시 첫 검증. SPDK blobstore임을 식별. */

/*
 * [한국어]
 * struct spdk_bs_super_block — 슈퍼블록 디스크 포맷 (4KiB).
 * 디스크 첫 페이지(LBA 0)에 위치. blobstore 식별 + 메타 영역 위치 정보 + 검증 데이터.
 * load 시 가장 먼저 읽히며, signature/version/crc로 1차 검증을 통과해야 다음 단계 진행.
 */
struct spdk_bs_super_block {
	uint8_t		signature[8];
	/* [한국어] 매직 시그니처 — "SPDKBLOB" (NUL 없음). load 시 일치하지 않으면 거부.
	 * 설정자: bs_init. 읽는 자: bs_load 1차 검증. 값 범위: 고정 문자열. */
	uint32_t	version;
	/* [한국어] 디스크 포맷 버전 (현재 SPDK_BS_VERSION=3).
	 * 설정자: bs_init. 읽는 자: bs_load. 값 범위: 1 ~ 현재 버전. */
	uint32_t	length;
	/* [한국어] 슈퍼블록 본문 길이 (보통 sizeof(spdk_bs_super_block)).
	 * 설정자/읽는 자: 위와 같음. 값 범위: 0x1000 (4KiB) 기대. */
	uint32_t	clean; /* If there was a clean shutdown, this is 1. */
	/* [한국어] clean shutdown 플래그 — 1이면 정상 종료, 0이면 dirty shutdown(crash).
	 * 설정자: unmount 시 1, mount 시 0으로 갱신 후 디스크에 즉시 sync.
	 * 읽는 자: load 시 dirty 여부 판정 → 복구 모드 진입. 값 범위: 0/1. */
	spdk_blob_id	super_blob;
	/* [한국어] 사용자가 지정한 "슈퍼" blob ID (없으면 SPDK_BLOBID_INVALID).
	 * 설정자: spdk_bs_set_super. 읽는 자: spdk_bs_get_super. */

	uint32_t	cluster_size; /* In bytes */
	/* [한국어] 클러스터 크기 (바이트). 설정자: bs_init. 읽는 자: load 후 in-memory에 복사. */

	uint32_t	used_page_mask_start; /* Offset from beginning of disk, in pages */
	/* [한국어] used_md_pages 비트맵의 디스크 page 시작 오프셋. 설정자: init. 읽는 자: load. */
	uint32_t	used_page_mask_len; /* Count, in pages */
	/* [한국어] used_md_pages 비트맵의 page 수. */

	uint32_t	used_cluster_mask_start; /* Offset from beginning of disk, in pages */
	/* [한국어] used_clusters 비트맵 시작 오프셋. */
	uint32_t	used_cluster_mask_len; /* Count, in pages */
	/* [한국어] used_clusters 비트맵 page 수. */

	uint32_t	md_start; /* Offset from beginning of disk, in pages */
	/* [한국어] 메타페이지 영역 시작 오프셋. spdk_blob_store::md_start의 디스크 표현. */
	uint32_t	md_len; /* Count, in pages */
	/* [한국어] 메타페이지 영역 크기. spdk_blob_store::md_len의 디스크 표현. */

	struct spdk_bs_type	bstype; /* blobstore type */
	/* [한국어] blobstore 타입 (16바이트 문자열) — 사용자 정의 식별자.
	 * 설정자: init 옵션. 읽는 자: load 시 일치 검증 (옵션). */

	uint32_t	used_blobid_mask_start; /* Offset from beginning of disk, in pages */
	/* [한국어] used_blobids 비트맵 시작 오프셋. */
	uint32_t	used_blobid_mask_len; /* Count, in pages */
	/* [한국어] used_blobids 비트맵 page 수. */

	uint64_t	size; /* size of blobstore in bytes */
	/* [한국어] blobstore 전체 크기 (바이트). 설정자: init. 읽는 자: 디바이스 크기 검증. */
	uint32_t	io_unit_size; /* Size of io unit in bytes */
	/* [한국어] io_unit 크기 (바이트). 보통 디바이스 blocklen 또는 그 배수. */

	uint32_t        md_page_size; /* Size in bytes */
	/* [한국어] 메타페이지 크기 (보통 4KiB).
	 * 설정자: init. 읽는 자: load 후 in-memory에 복사. */
	uint8_t		reserved[3996];
	/* [한국어] 예약 영역 — 향후 확장용. 슈퍼블록을 정확히 4KiB로 맞추기 위한 padding 겸용.
	 * 합계: signature(8) + version+length+clean(12) + super_blob(8) + cluster_size(4) +
	 *       마스크 4쌍(32) + md(8) + bstype(16) + blobid_mask(8) + size(8) + io_unit(4) +
	 *       md_page_size(4) + reserved(3996) + crc(4) = 4096. */

	uint32_t	crc;
	/* [한국어] CRC32 — signature부터 reserved 끝까지 검증. corruption 시 거부.
	 * 설정자: init/sync 마지막 단계. 읽는 자: load 시 검증. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_bs_super_block) == 0x1000, "Invalid super block size");
/* [한국어] 컴파일 타임 검증 — 슈퍼블록이 정확히 4KiB(0x1000)인지. 디스크 포맷 호환성 보호. */

#pragma pack(pop)
/* [한국어] 패킹 설정 복원 — 이후 일반 정렬 적용. 디스크 구조체 정의 끝. */

/*
 * [한국어]
 * 함수 선언 — 다른 .c 파일(blob_bs_dev.c, zeroes.c, blobstore.c)에 정의된 함수의 선언.
 */
struct spdk_bs_dev *bs_create_zeroes_dev(void);
/* [한국어] zeroes.c 정의 — 전역 zero-backing dev 싱글톤 반환. */
struct spdk_bs_dev *bs_create_blob_bs_dev(struct spdk_blob *blob);
/* [한국어] blob_bs_dev.c 정의 — blob을 backing dev로 노출하는 어댑터 생성. */
struct spdk_io_channel *blob_esnap_get_io_channel(struct spdk_io_channel *ch,
		struct spdk_blob *blob);
/* [한국어] blobstore.c 정의 — esnap blob의 외부 디바이스 채널 lazy 획득.
 * @ch: 사용자 채널, @blob: esnap blob. 반환: 외부 디바이스용 채널 또는 NULL(실패). */
bool blob_backed_with_zeroes_dev(struct spdk_blob *blob);
/* [한국어] zeroes.c 정의 — blob의 backing이 zeroes 싱글톤인지 식별. */

/* Unit Conversions
 *
 * The blobstore works with several different units:
 * - Byte: Self explanatory
 * - LBA: The logical blocks on the backing storage device.
 * - Page: The read/write units of blobs and metadata. This is
 *         an offset into a blob in units of 4KiB.
 * - Cluster Index: The disk is broken into a sequential list of
 *		    clusters. This is the offset from the beginning.
 *
 * NOTE: These conversions all act on simple magnitudes, not with any sort
 *        of knowledge about the blobs themselves. For instance, converting
 *        a page to an lba with the conversion function below simply converts
 *        a number of pages to an equivalent number of lbas, but that
 *        lba certainly isn't the right lba that corresponds to a page offset
 *        for a particular blob.
 */
/*
 * [한국어]
 * 단위 변환 인라인 함수들 — 모두 정렬 가정을 assert로 검증.
 * 단위 종류:
 *   - byte: 일반 바이트 카운트
 *   - LBA: 디스크의 논리 블록 (blocklen 단위)
 *   - page: 4KiB 메타페이지 단위
 *   - io_unit: blob의 read/write 단위 (보통 4KiB 또는 blocklen)
 *   - cluster: blob 할당 단위 (cluster_sz 단위)
 * 이 함수들은 "특정 blob의 어떤 영역이 어디로 매핑되는가"는 모르고, 단순히 단위 환산만 한다.
 * blob 좌표계를 디스크 LBA로 매핑하려면 bs_blob_io_unit_to_lba를 사용해야 한다.
 */

/*
 * [한국어]
 * bs_byte_to_lba - 바이트 길이를 디바이스 LBA 수로 변환.
 *
 * @bs: blobstore. @length: 바이트 (blocklen에 정렬되어야 함). @return: LBA 수.
 */
static inline uint64_t
bs_byte_to_lba(struct spdk_blob_store *bs, uint64_t length)
{
	assert(length % bs->dev->blocklen == 0);
	/* [한국어] 정렬 검증 — 미정렬이면 partial LBA 발생 = 의미 모호 → assert. */

	return length / bs->dev->blocklen;
	/* [한국어] 단순 나눗셈으로 LBA 수 계산. */
}

/*
 * [한국어]
 * bs_dev_byte_to_lba - 임의 bs_dev 기준 바이트 → LBA 변환.
 *
 * @bs_dev: 디바이스. @length: 바이트. @return: LBA 수.
 * bs_byte_to_lba와 다른 점: blobstore 대신 임의 bs_dev의 blocklen을 사용 (back_bs_dev 등).
 */
static inline uint64_t
bs_dev_byte_to_lba(struct spdk_bs_dev *bs_dev, uint64_t length)
{
	assert(length % bs_dev->blocklen == 0);
	/* [한국어] 디바이스 blocklen 정렬 검증. */

	return length / bs_dev->blocklen;
	/* [한국어] LBA 수 계산. */
}

/*
 * [한국어]
 * bs_page_to_lba - 메타페이지 인덱스 → 디바이스 LBA 변환.
 *
 * @bs: blobstore. @page: 메타페이지 인덱스. @return: 시작 LBA.
 * 페이지 크기(4KiB)와 디바이스 blocklen 차이가 있을 수 있으므로 단순히 곱-나눗셈.
 */
static inline uint64_t
bs_page_to_lba(struct spdk_blob_store *bs, uint64_t page)
{
	return page * bs->md_page_size / bs->dev->blocklen;
	/* [한국어] page 수 × 페이지 크기 ÷ blocklen = LBA. 정수 나눗셈 — 정렬 가정. */
}

/*
 * [한국어]
 * bs_md_page_to_lba - 메타 영역 내 page 인덱스 → 디스크 LBA.
 *
 * @bs: blobstore. @page: 메타 영역 내 0-기반 인덱스. @return: 디스크 시작 LBA.
 * md_start 오프셋을 더해 절대 page로 변환 후 LBA로.
 */
static inline uint64_t
bs_md_page_to_lba(struct spdk_blob_store *bs, uint32_t page)
{
	assert(page < bs->md_len);
	/* [한국어] 메타 영역 경계 검증. */
	return bs_page_to_lba(bs, page + bs->md_start);
	/* [한국어] 절대 page 인덱스(메타 시작 + 상대 인덱스)를 LBA로. */
}

/*
 * [한국어]
 * bs_dev_io_unit_to_lba - 임의 bs_dev 기준 io_unit → LBA.
 *
 * @blob: blob (io_unit_size 추출용). @bs_dev: 변환 대상 디바이스. @io_unit: io_unit 인덱스.
 * @return: 디바이스 좌표계 LBA.
 */
static inline uint64_t
bs_dev_io_unit_to_lba(struct spdk_blob *blob, struct spdk_bs_dev *bs_dev, uint64_t io_unit)
{
	return io_unit * blob->bs->io_unit_size / bs_dev->blocklen;
	/* [한국어] io_unit 수 × io_unit_size ÷ blocklen = LBA. */
}

/*
 * [한국어]
 * bs_cluster_to_io_unit - 클러스터 인덱스 → io_unit 인덱스.
 */
static inline uint64_t
bs_cluster_to_io_unit(struct spdk_blob_store *bs, uint32_t cluster)
{
	return (uint64_t)cluster * bs->io_units_per_cluster;
	/* [한국어] uint64 캐스팅으로 32비트 곱셈 오버플로 방지. */
}

/*
 * [한국어]
 * bs_io_unit_to_cluster - io_unit 인덱스 → 클러스터 인덱스.
 *
 * io_unit이 클러스터 경계에 정렬되어 있어야 함.
 */
static inline uint32_t
bs_io_unit_to_cluster(struct spdk_blob_store *bs, uint64_t io_unit)
{
	assert(io_unit % bs->io_units_per_cluster == 0);
	/* [한국어] 클러스터 정렬 검증. */

	return io_unit / bs->io_units_per_cluster;
	/* [한국어] 정수 나눗셈으로 클러스터 인덱스. */
}

/*
 * [한국어]
 * bs_cluster_to_lba - 클러스터 인덱스 → 디스크 LBA.
 *
 * 클러스터 = (cluster_sz / blocklen) LBA 묶음.
 */
static inline uint64_t
bs_cluster_to_lba(struct spdk_blob_store *bs, uint32_t cluster)
{
	assert(bs->cluster_sz / bs->dev->blocklen > 0);
	/* [한국어] 정수 나눗셈이 0이 되지 않도록 검증 (cluster_sz가 blocklen 이상이어야 함). */

	return (uint64_t)cluster * (bs->cluster_sz / bs->dev->blocklen);
	/* [한국어] cluster × LBA-per-cluster. */
}

/*
 * [한국어]
 * bs_lba_to_cluster - 디스크 LBA → 클러스터 인덱스.
 *
 * LBA가 클러스터 경계에 정렬되어 있어야 함.
 */
static inline uint32_t
bs_lba_to_cluster(struct spdk_blob_store *bs, uint64_t lba)
{
	assert(lba % (bs->cluster_sz / bs->dev->blocklen) == 0);
	/* [한국어] 클러스터 LBA 경계 정렬 검증. */

	return lba / (bs->cluster_sz / bs->dev->blocklen);
	/* [한국어] 정수 나눗셈으로 클러스터 인덱스. */
}

/*
 * [한국어]
 * bs_io_unit_to_back_dev_lba - blob의 io_unit → backing 디바이스 LBA로 변환.
 *
 * blob의 io_unit_size와 back_bs_dev의 blocklen 비율로 환산.
 * 사용처: blob_bs_dev_read 등에서 자식 좌표 → 부모 디바이스 좌표 변환.
 */
static inline uint64_t
bs_io_unit_to_back_dev_lba(struct spdk_blob *blob, uint64_t io_unit)
{
	return io_unit * (blob->bs->io_unit_size / blob->back_bs_dev->blocklen);
	/* [한국어] 두 디바이스의 비율을 곱해 좌표계 변환. */
}

/*
 * [한국어]
 * bs_cluster_to_extent_table_id - 클러스터 번호 → 그것을 담당하는 extent_table 슬롯 인덱스.
 *
 * EXTENT_TABLE 모드에서 한 슬롯이 SPDK_EXTENTS_PER_EP 개의 클러스터를 담당.
 */
static inline uint64_t
bs_cluster_to_extent_table_id(uint64_t cluster_num)
{
	return cluster_num / SPDK_EXTENTS_PER_EP;
	/* [한국어] 클러스터 번호를 페이지당 extent 수로 나눠 슬롯 인덱스 산출. */
}

/*
 * [한국어]
 * bs_cluster_to_extent_page - 클러스터를 담당하는 extent_pages 배열 슬롯 포인터 반환.
 *
 * @blob: EXTENT_TABLE 모드 blob (use_extent_table=true)
 * @cluster_num: 클러스터 번호
 * @return: blob->active.extent_pages[i] 슬롯 포인터 — 호출자가 read/write 가능.
 */
static inline uint32_t *
bs_cluster_to_extent_page(struct spdk_blob *blob, uint64_t cluster_num)
{
	uint64_t extent_table_id = bs_cluster_to_extent_table_id(cluster_num);
	/* [한국어] 클러스터 → 슬롯 인덱스 변환. */

	assert(blob->use_extent_table);
	/* [한국어] 호출자는 EXTENT_TABLE 모드여야 함 (RLE 모드면 사용 불가). */
	assert(extent_table_id < blob->active.extent_pages_array_size);
	/* [한국어] 배열 경계 검증. */

	return &blob->active.extent_pages[extent_table_id];
	/* [한국어] 슬롯 주소 반환 — 호출자가 lazy allocation 등에 사용. */
}

/*
 * [한국어]
 * bs_io_units_per_cluster - blob의 클러스터당 io_unit 수 단순 접근자.
 */
static inline uint64_t
bs_io_units_per_cluster(struct spdk_blob *blob)
{
	return blob->bs->io_units_per_cluster;
	/* [한국어] blobstore 전역값 반환 — 빠른 접근을 위한 wrapper. */
}

/* End basic conversions */

/*
 * [한국어]
 * bs_blobid_to_page - blob_id → 첫 메타페이지 인덱스.
 *
 * blob_id의 하위 32비트가 첫 메타페이지 인덱스 (디스크 좌표).
 */
static inline uint64_t
bs_blobid_to_page(spdk_blob_id id)
{
	return id & 0xFFFFFFFF;
	/* [한국어] 하위 32비트 마스크 — 상위 32비트는 HIGH_BIT 마커이므로 제거. */
}

/* The blob id is a 64 bit number. The lower 32 bits are the page_idx. The upper
 * 32 bits are not currently used. Stick a 1 there just to catch bugs where the
 * code assumes blob id == page_idx.
 */
/*
 * [한국어]
 * bs_page_to_blobid - 메타페이지 인덱스 → blob_id.
 *
 * 상위 32비트에 SPDK_BLOB_BLOBID_HIGH_BIT(=1<<32) 마커를 OR해서 "blob_id != page_idx"임을
 * 강제. page_idx가 UINT32_MAX 초과면 SPDK_BLOBID_INVALID 반환.
 */
static inline spdk_blob_id
bs_page_to_blobid(uint64_t page_idx)
{
	if (page_idx > UINT32_MAX) {
		/* [한국어] page 인덱스 오버플로 — 유효한 blob_id 생성 불가. */
		return SPDK_BLOBID_INVALID;
	}
	return SPDK_BLOB_BLOBID_HIGH_BIT | page_idx;
	/* [한국어] HIGH_BIT(상위 32비트) | page_idx(하위 32비트) = blob_id.
	 * 코드에서 무심코 blob_id를 page로 바로 사용하면 비정상 page 인덱스가 되어 버그 노출. */
}

/* Given an io unit offset into a blob, look up the LBA for the
 * start of that io unit.
 */
/*
 * [한국어]
 * bs_blob_io_unit_to_lba - blob 내 io_unit 오프셋 → 디스크 LBA.
 *
 * @blob: 대상 blob. @io_unit: blob 좌표계 io_unit 인덱스.
 * @return: 디스크 LBA. thin blob의 미할당 클러스터면 0 반환 (호출자가 처리).
 *
 * 동작:
 *   1) io_unit이 어느 클러스터에 속하는지 결정 (shift 또는 나눗셈).
 *   2) blob->active.clusters[]에서 해당 클러스터의 디스크 시작 LBA 조회.
 *   3) 0이면 미할당 → 0 반환.
 *   4) 0이 아니면 클러스터 시작 + 클러스터 내 오프셋 = 최종 LBA.
 *
 * shift 사용 이유: io_units_per_cluster가 2의 거듭제곱이면 shift 연산이 나눗셈보다 빠름.
 * 핫 패스이므로 가능한 모든 곳에서 shift 사용.
 */
static inline uint64_t
bs_blob_io_unit_to_lba(struct spdk_blob *blob, uint64_t io_unit)
{
	uint64_t	lba;
	/* [한국어] 결과 LBA. */
	uint8_t		shift;
	/* [한국어] io_units_per_cluster의 log2 (0이면 거듭제곱 아님). */
	uint64_t	io_units_per_cluster = blob->bs->io_units_per_cluster;
	/* [한국어] 캐시. */

	shift = blob->bs->io_units_per_cluster_shift;
	/* [한국어] shift 캐시. */
	assert(io_unit < blob->active.num_clusters * io_units_per_cluster);
	/* [한국어] blob 경계 검증 — 잘못된 io_unit이면 즉시 abort. */
	if (shift != 0) {
		/* [한국어] 거듭제곱이면 shift 연산으로 클러스터 인덱스 — 빠른 경로. */
		lba = blob->active.clusters[io_unit >> shift];
	} else {
		/* [한국어] 거듭제곱이 아니면 일반 나눗셈. */
		lba = blob->active.clusters[io_unit / io_units_per_cluster];
	}
	if (lba == 0) {
		/* [한국어] 미할당 클러스터 (thin blob에서만 가능) — 0 반환으로 호출자에게 신호. */
		return 0;
	} else {
		/* [한국어] 클러스터 시작 LBA + 클러스터 내 오프셋 = 최종 LBA.
		 * 클러스터 내 오프셋 = io_unit % io_units_per_cluster. */
		return lba + io_unit % io_units_per_cluster;
	}
}

/* Given an io_unit offset into a blob, look up the number of io_units until the
 * next cluster boundary.
 */
/*
 * [한국어]
 * bs_num_io_units_to_cluster_boundary - 현재 io_unit에서 다음 클러스터 경계까지 거리.
 *
 * 사용처: I/O split 알고리즘 — 한 op이 여러 클러스터에 걸치면 클러스터별로 쪼갠다.
 */
static inline uint32_t
bs_num_io_units_to_cluster_boundary(struct spdk_blob *blob, uint64_t io_unit)
{
	uint64_t	io_units_per_cluster;
	/* [한국어] 캐시. */

	io_units_per_cluster = bs_io_units_per_cluster(blob);
	/* [한국어] blob의 클러스터당 io_unit 수. */

	return io_units_per_cluster - (io_unit % io_units_per_cluster);
	/* [한국어] 다음 경계까지 = 한 클러스터 - 현재 위치의 클러스터 내 오프셋. */
}

/* Given an io_unit offset into a blob, look up the number of io_unit into blob to beginning of current cluster */
/*
 * [한국어]
 * bs_io_unit_to_cluster_start - 현재 io_unit이 속한 클러스터의 첫 io_unit 인덱스.
 *
 * 사용처: 클러스터 단위 작업 (CoW 등)에서 시작점 정렬.
 */
static inline uint64_t
bs_io_unit_to_cluster_start(struct spdk_blob *blob, uint64_t io_unit)
{
	uint64_t	io_units_per_cluster = blob->bs->io_units_per_cluster;
	/* [한국어] 캐시. */

	return io_unit - (io_unit % io_units_per_cluster);
	/* [한국어] 클러스터 내 오프셋만큼 빼면 클러스터 시작 io_unit. */
}

/* Given an io_unit offset into a blob, look up the number of pages into blob to beginning of current cluster */
/*
 * [한국어]
 * bs_io_unit_to_cluster_number - io_unit → 그것이 속한 클러스터 인덱스.
 *
 * shift 사용으로 빠른 경로 제공.
 */
static inline uint32_t
bs_io_unit_to_cluster_number(struct spdk_blob *blob, uint64_t io_unit)
{
	uint64_t	io_units_per_cluster = blob->bs->io_units_per_cluster;
	/* [한국어] 캐시. */
	uint8_t		shift = blob->bs->io_units_per_cluster_shift;
	/* [한국어] shift (0이면 거듭제곱 아님). */

	if (shift != 0) {
		/* [한국어] shift 사용 빠른 경로. */
		return io_unit >> shift;
	} else {
		/* [한국어] 일반 나눗셈. */
		return io_unit / io_units_per_cluster;
	}
}

/* Given an io unit offset into a blob, look up if it is from allocated cluster. */
/*
 * [한국어]
 * bs_io_unit_is_allocated - 해당 io_unit이 속한 클러스터가 할당되어 있는지 (thin blob).
 *
 * @return: true=할당됨(실제 LBA 존재), false=미할당(backing으로 위임 필요).
 */
static inline bool
bs_io_unit_is_allocated(struct spdk_blob *blob, uint64_t io_unit)
{
	uint64_t lba = bs_blob_io_unit_to_lba(blob, io_unit);
	/* [한국어] 변환 시도 — 미할당이면 0 반환. */

	if (lba == 0) {
		/* [한국어] 미할당 — 이 경우는 thin blob에서만 정당하므로 assert로 검증.
		 * thick blob에서 0 LBA가 나오면 메타 corruption. */
		assert(spdk_blob_is_thin_provisioned(blob));
		return false;
	} else {
		/* [한국어] 유효 LBA → 할당된 상태. */
		return true;
	}
}

#endif
/* [한국어] 헤더 가드 종료. */
