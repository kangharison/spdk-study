/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK Blobstore 공개 API 헤더 (blob.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK Blobstore — NVMe SSD(또는 임의의 bdev) 위에서 직접 동작하는
 * "lightweight flat object store" — 의 사용자(컨슈머) 공개 API를 정의한다.
 * 일반 파일시스템(ext4/XFS 등)과 달리 POSIX 인터페이스(open/read/write)를 노출하지 않고,
 * "blob"이라 불리는 가변 길이 객체를 cluster 단위로 thin-provision 방식으로 할당하며
 * 모든 I/O를 비동기 콜백 모델(`cb_fn(cb_arg, ..., bserrno)`)로 처리한다.
 * 이 헤더에는 (1) blobstore 자체의 수명주기(init/load/grow/unload/destroy/dump),
 * (2) blob 수명주기(create/open/close/delete/resize), (3) 비동기 I/O(read/write/writev/readv,
 * unmap, write_zeroes), (4) 메타데이터 동기화(sync_md), (5) xattr(set/get/remove/iterate),
 * (6) 스냅샷·클론·external snapshot(ESnap), (7) iteration이 모두 한 번에 묶여 있다.
 * blobstore의 컨슈머는 SPDK Logical Volume(lvol), NVMe-oF target backing store,
 * RocksDB BlobFS, vhost-blk 등이 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 스토리지 스택에서 Blobstore는 bdev 레이어보다 한 단계 위, 그러나 lvol/BlobFS와 같은
 * "응용 추상화" 보다는 한 단계 아래에 위치한다. 호출 체인은 다음과 같다:
 *   [Application: lvol / BlobFS / vhost-blk]
 *      ↓ spdk_bs_*() / spdk_blob_io_*() (이 헤더가 정의)
 *   [Blobstore core: lib/blob/*.c]
 *      ↓ spdk_bs_dev->read/write/...  (구조체 spdk_bs_dev 인터페이스)
 *   [bs_dev 구현: bdev_bs_dev (lib/blob/zeroes.c, lib/blob_bdev/* 등)]
 *      ↓ spdk_bdev_*  (bdev API)
 *   [bdev module: bdev_nvme / bdev_aio / bdev_malloc ...]
 *      ↓
 *   [NVMe driver / kernel AIO / DRAM]
 * 실행 컨텍스트는 SPDK reactor(코어 1개 = SPDK thread 1개) 위에서 polled-mode로 돌며,
 * 메타데이터 작업은 모두 "metadata thread"(spdk_bs_init/spdk_bs_load 호출 스레드)에서만
 * 호출되어야 lock-free 설계가 유지된다. spdk_blob_io_* 류 데이터-경로 함수만이
 * I/O channel(스레드 로컬 자원)을 인자로 받아 다른 SPDK thread에서 호출 가능하다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존(아래 방향): spdk_bs_dev (bs_dev 어댑터, lib/blob_bdev에서 SPDK bdev 위에 구현)
 *   를 통해 실제 블록 I/O를 위임한다. spdk_bs_dev는 read/write/unmap/translate_lba/
 *   is_zeroes/is_range_valid/copy 등의 함수 포인터 vtable이며, 스냅샷 체인이
 *   recursive 구조(esnap → snapshot → clone)로 연결된다.
 * - 의존(위 방향): SPDK Logical Volume(lib/lvol)이 가장 큰 컨슈머이고, 각 lvol = blob 1개
 *   이며 lvolstore = blobstore 1개에 매핑된다. NVMe-oF subsystem이나 vhost-blk target도
 *   lvol을 통해 간접적으로 blobstore를 사용한다. RocksDB BlobFS는 blob을 파일로 매핑한다.
 * - 데이터 흐름: 사용자가 spdk_blob_io_write 호출 → blobstore가 blob의 cluster map(extent
 *   table)을 참조해 블롭의 io_unit offset을 백킹 bdev의 LBA로 변환 → spdk_bs_dev->writev
 *   호출 → bdev_io 생성 → 모듈(bdev_nvme 등)이 NVMe SQ에 명령 push → 완료 시 콜백 chain.
 * - 공유 자료구조: spdk_blob_store(슈퍼블록·메타페이지·cluster bitmap·used_md_pages bitmap·
 *   blobid → blob 매핑), spdk_blob(per-blob 메타데이터: id, page table, xattr table,
 *   parent_id/back_bs_dev for snapshot/clone, used_clusters for thin-prov),
 *   spdk_io_channel(per-thread I/O channel, 백킹 bdev의 channel과 1:1 페어).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_bs_init/load/unload/destroy/grow/dump : Blobstore 디스크 인스턴스의 수명주기.
 *   init은 빈 디바이스에 super block을 새로 쓰고, load는 기존 super block을 검증·복원한다.
 * - spdk_bs_create_blob[_ext], spdk_bs_open_blob[_ext], spdk_blob_close,
 *   spdk_bs_delete_blob : 단일 blob의 생성/오픈/종료/삭제. blobid는 64비트 정수.
 * - spdk_blob_io_read/write/readv/writev[_ext], unmap, write_zeroes : 비동기 데이터 I/O.
 *   I/O channel과 io_unit offset/length를 사용한다.
 * - spdk_blob_sync_md, spdk_blob_resize, spdk_blob_set_read_only : 메타데이터 변경 후
 *   sync_md로 영속화 (close에서 자동 sync 수행). resize는 cluster 단위 확장.
 * - spdk_bs_create_snapshot/clone, spdk_bs_inflate_blob, spdk_bs_blob_decouple_parent,
 *   spdk_bs_blob_set_parent/external_parent, spdk_blob_set_esnap_bs_dev :
 *   CoW 기반 snapshot/clone 및 ESnap(외부 snapshot) 트리 조작.
 * - spdk_bs_iter_first/next : 전체 blob 순회 이터레이터.
 * - spdk_blob_set/get/remove/get_first/get_next_xattr : extended attribute API.
 *   internal xattr(prefix '.')과 external xattr이 같은 인터페이스를 공유한다.
 * - spdk_bs_dev : blobstore가 백킹 디바이스를 추상화하는 vtable (read/write/translate_lba 등)
 * - spdk_blob_store : blobstore의 디스크 위 인스턴스 (불투명 핸들).
 * - spdk_blob : 단일 blob 핸들 (불투명).
 * - spdk_bs_opts / spdk_blob_opts / spdk_blob_open_opts : 각종 옵션 구조체.
 *   ABI 호환성을 위해 opts_size 필드를 사용해 caller가 컴파일된 시점의 구조체 크기를 전달.
 * - 콜백 타입: spdk_bs_op_complete(handle 없음), spdk_bs_op_with_handle_complete(bs 핸들),
 *   spdk_blob_op_with_id_complete(blobid), spdk_blob_op_with_handle_complete(blob 핸들).
 *   모든 콜백은 음수 errno로 실패를 표현한다(0 = 성공).
 */

/** \file
 * Blob Storage System
 *
 * The blob storage system, or the blobstore for short, is a low level
 * library for placing opaque blobs of data onto a storage device such
 * that scattered physical blocks on the storage device appear as a
 * single, contiguous storage region. These blobs are also persistent,
 * which means they are rediscoverable after reboot or power loss.
 *
 * The blobstore is designed to be very high performance, and thus has
 * a few general rules regarding thread safety to avoid taking locks
 * in the I/O path.  This is primarily done by only allowing most
 * functions to be called on the metadata thread.  The metadata thread is
 * the thread which called spdk_bs_init() or spdk_bs_load().
 *
 * Functions starting with the prefix "spdk_blob_io" are passed a channel
 * as an argument, and channels may only be used from the thread they were
 * created on. See \ref spdk_bs_alloc_io_channel.  These are the only
 * functions that may be called from a thread other than the metadata
 * thread.
 *
 * The blobstore returns errors using negated POSIX errno values, either
 * returned in the callback or as a return value. An errno value of 0 means
 * success.
 */

/* [한국어] 헤더 가드 시작 - 이 헤더가 여러 번 #include 되어도 한 번만 처리되도록 보호.
 *  SPDK 공개 헤더는 모두 SPDK_<NAME>_H 형식을 따른다. */
#ifndef SPDK_BLOB_H
#define SPDK_BLOB_H

/* [한국어] SPDK 표준 #include 래퍼 — size_t/uint64_t/FILE 등 C 표준 타입을 일괄 노출한다.
 *  SPDK는 시스템 헤더를 직접 포함하지 않고 stdinc.h 한 곳에 모아 OS 차이를 흡수한다. */
#include "spdk/stdinc.h"
/* [한국어] 컴파일 타임 어서션 매크로(SPDK_STATIC_ASSERT) 정의를 가져옴.
 *  아래에서 spdk_blob_ext_io_opts 등의 sizeof를 ABI 안정 크기로 검증하는 데 사용한다. */
#include "spdk/assert.h"

/* [한국어] C++ 코드에서 이 헤더를 #include 했을 때도 함수 심볼이 C linkage(이름 망글링 없음)
 *  로 보이도록 extern "C" 블록으로 감싼다 — SPDK는 C로 작성되었지만 C++ 사용자도 흔하다. */
#ifdef __cplusplus
extern "C" {
#endif

/* [한국어] blob 식별자(blob ID) 타입 — 64비트 부호 없는 정수.
 *  super block 또는 metadata page index에서 파생되며, 단일 blobstore 내에서 유일하다.
 *  사용자 코드는 이 ID로 spdk_bs_open_blob/delete_blob을 호출한다. */
typedef uint64_t spdk_blob_id;

/* [한국어] "유효하지 않은 blob ID" 센티넬 값(uint64_t의 최대값, 모든 비트 1).
 *  spdk_blob_get_parent_snapshot 등 blob ID를 반환하는 API가 "부모 없음" 등을 표현할 때 사용. */
#define SPDK_BLOBID_INVALID		(uint64_t)-1
/* [한국어] "이 blob은 외부(ESnap) 스냅샷의 클론" 임을 나타내는 특별 ID.
 *  ESnap clone은 부모를 자체 blobstore가 아닌 외부 bs_dev로 갖기 때문에 일반 ID로 표현 불가. */
#define SPDK_BLOBID_EXTERNAL_SNAPSHOT	(uint64_t)-2
/* [한국어] super block의 bstype 필드 길이(바이트) — 16바이트로 고정된 ASCII 문자열.
 *  컨슈머(lvol, BlobFS 등)가 이 blobstore의 "용도"를 식별하는 라벨로 쓴다. */
#define SPDK_BLOBSTORE_TYPE_LENGTH 16

/*
 * [한국어]
 * blob_clear_method - 개별 blob의 cluster를 해제(deallocate)/삭제할 때 백킹 디바이스에
 * 적용할 정리 방식. spdk_blob_opts.clear_method로 blob 생성 시 지정한다.
 *
 * 이 옵션이 필요한 이유: blob 데이터를 풀거나 삭제할 때 이전 데이터가 다른 blob에 leak되지
 * 않도록 보장해야 하는데, 디바이스에 따라 unmap이 더 빠를 수도, write zeroes가 더 안전할 수도
 * 있어 사용자 선택을 허용한다.
 */
enum blob_clear_method {
	BLOB_CLEAR_WITH_DEFAULT,
	/* [한국어] blobstore 단위(bs_clear_method)에 설정된 기본 동작을 따른다.
	 * 설정자: spdk_blob_opts_init이 기본값으로 사용.
	 * 읽는 자: blob 삭제·resize 축소·snapshot 동결 시 lib/blob 코어가 분기 판단.
	 * 값 의미: 명시적으로 BS 기본값을 따르겠다는 표현. */

	BLOB_CLEAR_WITH_NONE,
	/* [한국어] cluster를 해제하되 백킹 디바이스에는 아무 명령도 보내지 않음.
	 * 설정자: 사용자가 성능을 우선시하고, 이전 데이터 노출이 보안상 문제 없는 경우 선택.
	 * 읽는 자: lib/blob 코어가 unmap/write_zeroes 호출을 스킵.
	 * 값 의미: 가장 빠르지만 잔여 데이터가 그대로 남는다(보안 영향 가능). */

	BLOB_CLEAR_WITH_UNMAP,
	/* [한국어] 백킹 bs_dev의 unmap (NVMe Dataset Management Deallocate, SCSI UNMAP) 명령으로
	 *  cluster를 해제. 디바이스가 0 또는 unmapped 마커로 응답하도록 위임한다.
	 *  설정자: NVMe SSD가 deallocate를 지원하고 빠른 해제를 원할 때.
	 *  읽는 자: lib/blob 코어가 spdk_bs_dev->unmap을 호출.
	 *  값 의미: 디바이스가 이후 read에 0/이전값 중 무엇을 반환할지는 디바이스 구현에 따름. */

	BLOB_CLEAR_WITH_WRITE_ZEROES,
	/* [한국어] 명시적으로 0을 기록(NVMe Write Zeroes 명령 또는 SW emulation)해 cluster를 정리.
	 *  설정자: 보안상 잔여 데이터를 지워야 하거나, unmap 후 0 보장이 안 될 때.
	 *  읽는 자: lib/blob 코어가 spdk_bs_dev->write_zeroes 호출.
	 *  값 의미: 가장 안전하지만 디바이스가 패스트경로 zero를 지원하지 않으면 비싸다. */
};

/*
 * [한국어]
 * bs_clear_method - blobstore 전체(모든 blob의 기본값)에 적용되는 cluster 정리 방식.
 * spdk_bs_opts.clear_method로 blobstore init 시 지정한다.
 *
 * blob_clear_method와 같은 개념이지만 BS 전역 기본값을 결정하는 enum으로,
 * BLOB_CLEAR_WITH_DEFAULT를 갖는 blob은 이 값에 fallback한다.
 */
enum bs_clear_method {
	BS_CLEAR_WITH_UNMAP,
	/* [한국어] BS의 기본 정리 방식 = unmap.
	 * 설정자: spdk_bs_opts_init이 기본값으로 사용 (대부분의 NVMe SSD에서 적합).
	 * 읽는 자: BLOB_CLEAR_WITH_DEFAULT 인 blob의 정리 시 lib/blob이 참조.
	 * 값 의미: NVMe Deallocate 명령 사용. */

	BS_CLEAR_WITH_WRITE_ZEROES,
	/* [한국어] BS의 기본 정리 방식 = write zeroes.
	 * 설정자: 보안 요구가 있는 환경에서 사용자가 명시적으로 선택.
	 * 읽는 자: 위와 동일.
	 * 값 의미: 디바이스에 0을 기록 (SW fallback이 있으면 항상 동작). */

	BS_CLEAR_WITH_NONE,
	/* [한국어] BS의 기본 정리 방식 = 아무것도 안 함.
	 * 설정자: 성능 최우선이고 잔여 데이터 노출이 무방할 때.
	 * 읽는 자: 위와 동일.
	 * 값 의미: 가장 빠르지만 cluster 재할당 시 이전 데이터가 보일 수 있음. */
};

/* [한국어] forward declaration: blobstore의 디스크 위 인스턴스를 표현하는 불투명 구조체.
 *  실제 정의는 lib/blob/blobstore.h(내부 헤더)에 있으며, 사용자에게는 포인터로만 노출된다. */
struct spdk_blob_store;
/* [한국어] forward declaration: blobstore 백킹 블록 디바이스 어댑터(vtable).
 *  아래에서 본격적으로 정의되며, lib/blob_bdev이 SPDK bdev 위에 이 구조체를 만들어 전달한다. */
struct spdk_bs_dev;
/* [한국어] forward declaration: SPDK I/O channel(per-thread 자원).
 *  spdk/thread.h에 정의되어 있고, blob I/O는 호출자 스레드 소유의 채널을 인자로 받는다. */
struct spdk_io_channel;
/* [한국어] forward declaration: 단일 blob의 사용자 핸들.
 *  blob ID, cluster map, xattr 테이블 등 모두 내부에 캡슐화된 불투명 타입. */
struct spdk_blob;
/* [한국어] forward declaration: spdk_blob_get_xattr_names가 반환하는 xattr 이름 목록.
 *  내부적으로 names 배열과 count를 담고 있으며 spdk_xattr_names_free로 해제. */
struct spdk_xattr_names;

/**
 * Blobstore operation completion callback.
 *
 * \param cb_arg Callback argument.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_bs_op_complete - blobstore 단순 작업(unload/destroy/dump 등) 완료 콜백 시그니처.
 *
 * @cb_arg: 사용자가 작업 호출 시 함께 전달한 컨텍스트 포인터(예: 자체 상태 구조체).
 * @bserrno: 0 = 성공, 음수 = 실패한 POSIX errno (-EIO, -ENOMEM, -EINVAL 등).
 *
 * 호출 컨텍스트: 작업을 시작한 metadata thread (또는 작업이 진행된 thread)에서 polling
 * 루프 도중 호출된다. 따라서 cb_fn 안에서 spdk_thread_send_msg 없이 같은 thread의
 * 자원을 만질 수 있다. 단, blocking 호출은 reactor를 멈추므로 금지.
 */
typedef void (*spdk_bs_op_complete)(void *cb_arg, int bserrno);

/**
 * Blobstore operation completion callback with handle.
 *
 * \param cb_arg Callback argument.
 * \param bs Handle to a blobstore.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_bs_op_with_handle_complete - blobstore 핸들을 반환하는 작업의 완료 콜백.
 *
 * @cb_arg: 사용자 컨텍스트.
 * @bs: 새로 생성·로드된 blobstore의 핸들. bserrno != 0 이면 NULL이거나 무효이며 사용 금지.
 * @bserrno: 0 = 성공, 음수 errno = 실패.
 *
 * 사용처: spdk_bs_init / spdk_bs_load / spdk_bs_grow의 완료 콜백 — 호출자가 새로 만들어진
 * blobstore 핸들을 받아 이후 작업(create_blob 등)을 시작하는 출발점.
 */
typedef void (*spdk_bs_op_with_handle_complete)(void *cb_arg, struct spdk_blob_store *bs,
		int bserrno);

/**
 * Blob operation completion callback.
 *
 * \param cb_arg Callback argument.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_blob_op_complete - blob 단위 작업(write/read/sync_md/close/delete 등) 완료 콜백.
 *
 * @cb_arg: 사용자 컨텍스트.
 * @bserrno: 0 = 성공, 음수 errno = 실패. -EBUSY(resize 동시 실행), -EIO(디바이스 오류) 등.
 *
 * 호출 컨텍스트: I/O 작업이라면 그 채널을 만든 thread에서, 메타데이터 작업이라면
 * metadata thread에서 호출된다. SPDK reactor의 polling 루프 안.
 */
typedef void (*spdk_blob_op_complete)(void *cb_arg, int bserrno);

/**
 * Blob operation completion callback with blob ID.
 *
 * \param cb_arg Callback argument.
 * \param blobid Blob ID.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_blob_op_with_id_complete - 새로 만든 blob의 ID를 반환하는 작업 완료 콜백.
 *
 * @cb_arg: 사용자 컨텍스트.
 * @blobid: 새로 생성된 blob의 ID. bserrno != 0 이면 SPDK_BLOBID_INVALID가 들어올 수 있음.
 * @bserrno: 0 = 성공, 음수 errno = 실패.
 *
 * 사용처: spdk_bs_create_blob[_ext], spdk_bs_create_snapshot/clone, spdk_bs_get_super.
 */
typedef void (*spdk_blob_op_with_id_complete)(void *cb_arg, spdk_blob_id blobid, int bserrno);

/**
 * Blob operation completion callback with handle.
 *
 * \param cb_arg Callback argument.
 * \param blb Handle to a blob.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_blob_op_with_handle_complete - blob 핸들을 반환하는 작업 완료 콜백.
 *
 * @cb_arg: 사용자 컨텍스트.
 * @blb: 오픈된(또는 iter로 진행된) blob의 핸들. bserrno != 0이면 NULL.
 * @bserrno: 0 = 성공, 음수 errno = 실패. spdk_bs_iter_*는 -ENOENT로 순회 종료를 알림.
 *
 * 사용처: spdk_bs_open_blob[_ext], spdk_bs_iter_first/next, opts.iter_cb_fn.
 */
typedef void (*spdk_blob_op_with_handle_complete)(void *cb_arg, struct spdk_blob *blb, int bserrno);

/**
 * Blobstore device completion callback.
 *
 * \param channel I/O channel the operation was initiated on.
 * \param cb_arg Callback argument.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_bs_dev_cpl - bs_dev(백킹 디바이스 어댑터) 단계의 I/O 완료 콜백 시그니처.
 *
 * @channel: 작업이 제출된 I/O channel — 같은 채널에서 후속 작업을 chain할 때 사용 가능.
 * @cb_arg: 컨텍스트.
 * @bserrno: 0 = 성공, 음수 errno = 실패.
 *
 * 사용처: spdk_bs_dev->read/write/unmap/... 함수 포인터 호출 시 spdk_bs_dev_cb_args에
 * 들어가는 cb_fn으로, blobstore 코어가 bs_dev에 I/O를 위임하고 완료를 받을 때 호출.
 */
typedef void (*spdk_bs_dev_cpl)(struct spdk_io_channel *channel,
				void *cb_arg, int bserrno);

/**
 * Blob device open completion callback with blobstore device.
 *
 * \param cb_arg Callback argument.
 * \param bs_dev Blobstore device.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_blob_op_with_bs_dev - bs_dev 핸들을 반환하는 작업의 완료 콜백.
 *
 * @cb_arg: 사용자 컨텍스트.
 * @bs_dev: 비동기로 생성된 bs_dev 어댑터(예: ESnap 백엔드 오픈). bserrno != 0 이면 NULL.
 * @bserrno: 0 = 성공, 음수 errno = 실패.
 *
 * 사용처: lvol에서 ESnap 디바이스를 비동기로 오픈할 때 등 bs_dev 생성을 콜백으로 알리는 용도.
 */
typedef void (*spdk_blob_op_with_bs_dev)(void *cb_arg, struct spdk_bs_dev *bs_dev, int bserrno);

/**
 * External snapshot device open callback. As an esnap clone blob is loading, it uses this
 * callback registered with the blobstore to create the external snapshot device. The blobstore
 * consumer must set this while loading the blobstore if it intends to support external snapshots.
 *
 * If the blobstore consumer does not wish to load an external snapshot, it should set *bs_dev to
 * NULL and return 0.
 *
 * \param bs_ctx Context provided by the blobstore consumer via esnap_ctx member of struct
 * spdk_bs_opts.
 * \param blob_ctx Context provided to spdk_bs_open_ext() via esnap_ctx member of struct
 * spdk_bs_open_opts.
 * \param blob The blob that needs its external snapshot device.
 * \param esnap_id A copy of the esnap_id passed via blob_opts when creating the esnap clone.
 * \param id_size The size in bytes of the data referenced by esnap_id.
 * \param bs_dev When 0 is returned, the newly created blobstore device is returned by reference.
 *
 * \return 0 on success, else a negative errno.
 */
/*
 * [한국어]
 * spdk_bs_esnap_dev_create - "ESnap(External Snapshot)" 디바이스 생성 콜백 시그니처.
 *
 * ESnap이란? blobstore 외부의 어떤 데이터(다른 blob, 파일, RBD 이미지 등)를 read-only base
 * 로 두고 그 위에 thin-provisioned clone blob을 만드는 기능. CoW write 시점에 새 cluster를
 * blob에 할당하고, 미할당 cluster의 read는 esnap_id로 식별되는 외부 디바이스를 fallthrough.
 *
 * @bs_ctx: blobstore 단위 컨텍스트 (spdk_bs_opts.esnap_ctx).
 * @blob_ctx: blob 단위 컨텍스트 (spdk_bs_open_opts.esnap_ctx) — open 시점마다 바뀔 수 있음.
 * @blob: ESnap base가 필요한 blob 핸들 — esnap clone임이 검증된 상태로 전달됨.
 * @esnap_id: blob 메타데이터에 저장된 외부 snapshot 식별자 (불투명 바이트열, 컨슈머가 해석).
 * @id_size: esnap_id 길이 (바이트).
 * @bs_dev: [out] 생성된 bs_dev 포인터를 여기에 저장.
 * @return: 0 = 성공 (bs_dev에 유효 포인터 또는 NULL "no esnap"), 음수 errno = 실패.
 *
 * 호출 시점: spdk_bs_load 시 메타데이터 복원 중 ESnap clone을 만나면, 또는 사용자가
 * spdk_bs_open_blob[_ext]를 호출해 ESnap clone blob을 열 때. lvol 컨슈머는 이 콜백에서
 * lvol_esnap_dev_create_*를 호출해 bdev를 bs_dev로 감싼다.
 *
 * 호출 체인: spdk_bs_load → blob_load → 메타페이지 파싱 → esnap_id 발견 →
 *            opts.esnap_bs_dev_create() [이 콜백] → bs_dev 반환.
 */
typedef int (*spdk_bs_esnap_dev_create)(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
					const void *esnap_id, uint32_t id_size,
					struct spdk_bs_dev **bs_dev);

/**
 * Blob shallow copy status callback.
 *
 * \param copied_clusters Actual number of copied clusters by the shallow copy operation
 * \param cb_arg Callback argument.
 */
/*
 * [한국어]
 * spdk_blob_shallow_copy_status - shallow copy 진행 상황 보고용 콜백.
 *
 * @copied_clusters: 지금까지 복사된 cluster 개수 (누적값).
 * @cb_arg: 사용자 컨텍스트.
 *
 * shallow copy: blob의 "할당된 cluster만" 외부 bs_dev에 복사하는 작업.
 * spdk_bs_blob_shallow_copy 진행 중 cluster마다 호출되어 진행률 표시 등에 사용.
 * 호출 컨텍스트: copy를 진행 중인 channel의 thread.
 */
typedef void (*spdk_blob_shallow_copy_status)(uint64_t copied_clusters, void *cb_arg);

/*
 * [한국어]
 * spdk_bs_dev_cb_args - bs_dev 메소드 호출 시 완료 콜백 정보를 묶어 전달하는 보조 구조체.
 *
 * blobstore 코어가 spdk_bs_dev->read/write/...를 호출할 때 마지막 인자로 이 구조체의
 * 포인터를 전달하면 bs_dev 구현체가 작업 완료 시 cb_fn(channel, cb_arg, errno)를 호출한다.
 */
struct spdk_bs_dev_cb_args {
	spdk_bs_dev_cpl		cb_fn;
	/* [한국어] 작업 완료 시 호출될 콜백 함수 포인터.
	 * 설정자: blobstore 코어(lib/blob/blobstore.c)가 bs_dev에 I/O를 위임하기 직전 채움.
	 * 읽는 자: bs_dev 구현(예: bdev_blob.c의 bdev_blob_io_complete)이 호출.
	 * 값 범위: NULL 불가 (반드시 유효한 함수 포인터). */

	struct spdk_io_channel	*channel;
	/* [한국어] 작업이 제출된 I/O channel — bs_dev 구현이 channel 컨텍스트를 활용하거나
	 *  cb_fn에 그대로 넘겨주기 위한 핸들.
	 *  설정자: blobstore 코어. 보통 spdk_blob_io_*에 들어온 spdk_io_channel을 그대로 전달.
	 *  읽는 자: cb_fn 내부에서 후속 chain 작업이나 채널 통계 갱신에 사용.
	 *  값 범위: 호출 thread에 바인딩된 유효한 채널 (NULL 불가). */

	void			*cb_arg;
	/* [한국어] cb_fn에 전달될 사용자 정의 컨텍스트 포인터(주로 op tracking 구조체).
	 *  설정자: blobstore 코어가 자체 op 추적자(blob_request 등)를 가리키게 설정.
	 *  읽는 자: cb_fn에서 op를 진행/완료 처리할 때 사용.
	 *  값 범위: NULL 가능(콜백이 ctx를 안 쓸 때). */
};

/**
 * Structure with optional IO request parameters
 * The content of this structure must be valid until the IO request is completed
 */
/*
 * [한국어]
 * spdk_blob_ext_io_opts - blob 확장 I/O 옵션 (memory domain, user ctx).
 *
 * spdk_blob_io_writev_ext / readv_ext 호출 시에 메모리가 호스트가 아닌 다른 도메인
 * (RDMA NIC 메모리, GPU 메모리 등)에 있다는 사실을 blobstore에 알려주기 위한 메타정보.
 * memory domain 기반의 zero-copy DMA를 가능하게 한다.
 *
 * 주의: I/O 완료될 때까지 이 구조체와 안의 포인터가 모두 유효해야 한다(참조 보유).
 */
struct spdk_blob_ext_io_opts {
	/** Size of this structure in bytes */
	size_t size;
	/* [한국어] 호출자가 컴파일된 시점의 이 구조체 크기(바이트) — ABI 호환성 트릭.
	 * 설정자: 사용자 코드가 sizeof(struct spdk_blob_ext_io_opts) 또는 그 이하로 설정.
	 * 읽는 자: lib/blob 코어가 size를 보고 자신이 아는 필드만 읽도록 분기.
	 * 값 범위: ≤ 현재 라이브러리가 인식하는 sizeof. 더 작으면 그 만큼만 읽음. */

	/** Memory domain which describes payload in this IO request. */
	struct spdk_memory_domain *memory_domain;
	/* [한국어] 페이로드가 속한 memory domain 핸들 (NULL이면 기본 호스트 메모리).
	 * 설정자: 사용자 코드가 RDMA/GPU/PMR 도메인 핸들을 지정.
	 * 읽는 자: blobstore → bs_dev → bdev가 DMA 매핑 결정 시 참조.
	 * 값 범위: spdk_memory_domain_create*로 만든 유효 핸들 또는 NULL. */

	/** Context to be passed to memory domain operations */
	void *memory_domain_ctx;
	/* [한국어] memory_domain의 translate/pull/push 콜백에 함께 넘길 사용자 ctx.
	 * 설정자: 사용자 코드.
	 * 읽는 자: memory_domain 구현이 콜백 ctx로 받아 사용.
	 * 값 범위: 도메인 구현이 정의 (NULL 가능). */

	/** Optional user context */
	void *user_ctx;
	/* [한국어] 임의의 사용자 ctx — blobstore는 해석하지 않고 그대로 전파.
	 * 설정자: 사용자 코드(추적·디버깅용).
	 * 읽는 자: 사용자 코드(필요 시 cb_arg와 별도 채널로 데이터 전달).
	 * 값 범위: NULL 가능, 의미는 사용자가 정의. */
} __attribute__((packed));
/* [한국어] packed: 컴파일러 패딩 없이 정확한 32바이트 레이아웃을 강제하여 ABI 안정 보장. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_blob_ext_io_opts) == 32, "Incorrect size");
/* [한국어] 컴파일 타임 어서션 — 위 구조체가 정확히 32B 인지 확인.
 *  새 필드 추가 시 sizeof가 늘어나면 이 어서션이 깨지고, 명시적으로 32→64 등으로 버전 업해야 한다. */

/*
 * [한국어]
 * spdk_bs_dev - blobstore가 사용하는 백킹 블록 디바이스 추상화(vtable).
 *
 * 이 구조체는 blobstore 코어가 "어떤 블록 디바이스든 똑같이" 다루기 위한 인터페이스다.
 * 함수 포인터(read/write/unmap/translate_lba 등)와 디바이스 메트릭(blockcnt, blocklen,
 * phys_blocklen)으로 구성된다. 구현체로는:
 *   - lib/blob_bdev/blob_bdev.c → SPDK bdev를 bs_dev로 감싸는 어댑터 (가장 흔함)
 *   - lib/blob/zeroes.c → "all zeroes" 가상 디바이스 (thin-prov fallthrough용)
 *   - lvol의 ESnap 디바이스 어댑터 등
 *
 * 스냅샷 체인은 이 vtable을 재귀적으로 연결한다: blob A의 read가 cluster 미할당이면
 * back_bs_dev->read 호출 → 부모 snapshot의 bs_dev → ... → 최상위 ESnap 또는 zeroes_dev.
 */
struct spdk_bs_dev {
	/* Create a new channel which is a software construct that is used
	 * to submit I/O. */
	struct spdk_io_channel *(*create_channel)(struct spdk_bs_dev *dev);
	/* [한국어] 새 I/O channel을 생성. blob I/O 채널 할당 시 백킹 디바이스에서 짝이 되는
	 *  채널을 같이 받기 위해 호출된다.
	 *  설정자: bs_dev 구현체(예: bdev_blob_create_channel)가 함수 포인터를 채움.
	 *  읽는 자: spdk_bs_alloc_io_channel 경로에서 lib/blob 코어가 호출.
	 *  반환: 백킹 channel (NULL이면 채널 생성 실패).
	 *  실행 컨텍스트: 채널을 사용할 thread에서 호출되어야 thread affinity 유지. */

	/* Destroy a previously created channel */
	void (*destroy_channel)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel);
	/* [한국어] create_channel로 만든 채널을 해제.
	 *  설정자: bs_dev 구현체.
	 *  읽는 자: spdk_bs_free_io_channel 경로.
	 *  실행 컨텍스트: 채널을 만든 thread에서만 호출 (cross-thread 호출 금지). */

	/* Destroy this blobstore device.  Applications must not destroy the blobstore device,
	 *  rather the blobstore will destroy it using this function pointer once all
	 *  references to it during unload callback context have been completed.
	 */
	void (*destroy)(struct spdk_bs_dev *dev);
	/* [한국어] bs_dev 자체를 해제(메모리 free 포함).
	 *  설정자: bs_dev 구현체.
	 *  읽는 자: blobstore 코어가 unload/destroy 시 또는 ESnap base bs_dev 교체 시 호출.
	 *  주의: 사용자 코드는 직접 호출 금지 — 항상 blobstore 코어가 라이프사이클 관리. */

	void (*read)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		     uint64_t lba, uint32_t lba_count,
		     struct spdk_bs_dev_cb_args *cb_args);
	/* [한국어] 단일 버퍼(payload)에 lba부터 lba_count 만큼 비동기 read.
	 *  설정자: bs_dev 구현체.
	 *  읽는 자: lib/blob이 thin-prov fallthrough 또는 메타페이지 read에 사용.
	 *  단위: lba 와 lba_count 모두 blocklen 단위 — 호출자가 blob의 io_unit을 lba로 변환 후 호출. */

	void (*write)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		      uint64_t lba, uint32_t lba_count,
		      struct spdk_bs_dev_cb_args *cb_args);
	/* [한국어] 단일 버퍼 비동기 write. 위 read와 짝.
	 *  사용처: 메타페이지 영속화, blob write 시 백킹 디바이스에 실제 기록. */

	void (*readv)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      struct iovec *iov, int iovcnt,
		      uint64_t lba, uint32_t lba_count,
		      struct spdk_bs_dev_cb_args *cb_args);
	/* [한국어] iovec 배열로 분산-수집(scatter-gather) read.
	 *  iov: 사용자 버퍼들의 (base,len) 배열. iovcnt: 개수.
	 *  사용처: spdk_blob_io_readv → bs_dev 위임. NVMe SGL을 그대로 활용 가능. */

	void (*writev)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		       struct iovec *iov, int iovcnt,
		       uint64_t lba, uint32_t lba_count,
		       struct spdk_bs_dev_cb_args *cb_args);
	/* [한국어] iovec scatter-gather write (readv의 쌍).
	 *  사용처: spdk_blob_io_writev. */

	void (*readv_ext)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			  struct iovec *iov, int iovcnt,
			  uint64_t lba, uint32_t lba_count,
			  struct spdk_bs_dev_cb_args *cb_args,
			  struct spdk_blob_ext_io_opts *ext_io_opts);
	/* [한국어] readv + memory domain 등 확장 옵션 지원.
	 *  ext_io_opts: 호스트가 아닌 메모리 도메인(RDMA/GPU)에 있는 페이로드 처리.
	 *  사용처: spdk_blob_io_readv_ext. */

	void (*writev_ext)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			   struct iovec *iov, int iovcnt,
			   uint64_t lba, uint32_t lba_count,
			   struct spdk_bs_dev_cb_args *cb_args,
			   struct spdk_blob_ext_io_opts *ext_io_opts);
	/* [한국어] writev_ext — readv_ext의 쌍.
	 *  사용처: spdk_blob_io_writev_ext. */

	void (*flush)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      struct spdk_bs_dev_cb_args *cb_args);
	/* [한국어] 백킹 디바이스의 휘발성 캐시를 영속화 (NVMe Flush 명령).
	 *  사용처: spdk_bs_sync 또는 super block 갱신 후 영속화 보장 시.
	 *  의미: 이 호출이 완료되면 이전 모든 write가 디스크에 도달했다고 보장. */

	void (*write_zeroes)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			     uint64_t lba, uint64_t lba_count,
			     struct spdk_bs_dev_cb_args *cb_args);
	/* [한국어] LBA 범위에 0을 기록 (NVMe Write Zeroes 명령 또는 SW emulation).
	 *  사용처: clear_method == WRITE_ZEROES인 cluster 해제, blob 초기화 시. */

	void (*unmap)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      uint64_t lba, uint64_t lba_count,
		      struct spdk_bs_dev_cb_args *cb_args);
	/* [한국어] LBA 범위 unmap (NVMe Dataset Management Deallocate, SCSI UNMAP).
	 *  디바이스에 "이 영역은 이제 사용 안 함" 힌트를 주어 SSD GC 효율을 개선.
	 *  사용처: clear_method == UNMAP인 cluster 해제, spdk_blob_io_unmap. */

	struct spdk_bdev *(*get_base_bdev)(struct spdk_bs_dev *dev);
	/* [한국어] 이 bs_dev를 뒷받침하는 SPDK bdev를 얻는다 (bdev 어댑터 한정 — 다른 구현은 NULL).
	 *  설정자: bdev_blob.c가 함수 포인터 등록.
	 *  읽는 자: lvol/RPC 코드가 underlying bdev 정보(이름·UUID 등) 조회 시 호출.
	 *  값 범위: 유효한 spdk_bdev* 또는 NULL (zeroes_dev처럼 bdev가 없는 경우). */

	bool (*is_zeroes)(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count);
	/* [한국어] 해당 LBA 범위가 zeroes 디바이스 또는 동등한 영역인지 빠르게 판정.
	 *  설정자: bs_dev 구현체. zeroes_dev는 항상 true.
	 *  읽는 자: blobstore가 CoW write 시 read-then-write 대신 짧은 경로(0 채움)로 갈지 결정.
	 *  성능 의의: 불필요한 backing read를 회피. */

	/* Is the lba range we are looking for valid or not for this bs_dev. Used to
	 * check if we can safely reference the bs_dev during CoW or perhaps even
	 * during read. */
	bool (*is_range_valid)(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count);
	/* [한국어] 이 bs_dev에서 해당 LBA 범위가 실제로 매핑·접근 가능한지 검증.
	 *  ESnap clone 같이 부모 디바이스가 축소·degraded될 가능성이 있는 경우 안전 체크.
	 *  반환 false면 blobstore는 CoW를 포기하고 zeroes로 대체하거나 EIO 처리. */

	/* Translate blob lba to lba on the underlying bdev.
	 * This operation recurses down the whole chain of bs_dev's.
	 * Returns true and initializes value of base_lba on success.
	 * Returns false on failure.
	 * The function may fail when blob lba is not backed by the bdev lba.
	 * For example, when we eventually hit zeroes device in the chain.
	 */
	bool (*translate_lba)(struct spdk_bs_dev *dev, uint64_t lba, uint64_t *base_lba);
	/* [한국어] 가상 LBA를 실제 bdev LBA로 변환 (스냅샷 체인을 재귀 따라감).
	 *  성공 시 *base_lba에 최종 LBA 저장 후 true; 체인 끝이 zeroes_dev 등이면 false.
	 *  사용처: copy/peer-to-peer DMA 최적화 등에서 직접 LBA가 필요할 때. */

	void (*copy)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		     uint64_t dst_lba, uint64_t src_lba, uint64_t lba_count,
		     struct spdk_bs_dev_cb_args *cb_args);
	/* [한국어] 디바이스 내부 (NVMe Simple Copy) 또는 host-mediated 복사.
	 *  src_lba → dst_lba로 lba_count만큼 복사 (host buffer 거치지 않을 수 있음).
	 *  사용처: snapshot 생성 시 cluster 복제, shallow copy. */

	bool (*is_degraded)(struct spdk_bs_dev *dev);
	/* [한국어] 백킹 디바이스가 degraded 상태(부분 결함, 읽기 전용 등)인지 보고.
	 *  사용처: spdk_blob_is_degraded가 체인을 따라 누적 판정.
	 *  값 범위: true = I/O 불가 또는 위험, false = 정상. */

	uint64_t	blockcnt;
	/* [한국어] 디바이스의 총 블록 수 (block 단위).
	 *  설정자: bs_dev 구현체가 생성 시 설정 후 거의 불변.
	 *  읽는 자: blobstore가 cluster bitmap 크기 결정·범위 검증에 사용.
	 *  단위: blocklen 바이트 단위. */

	uint32_t	blocklen; /* In bytes */
	/* [한국어] 논리 블록 크기 (바이트) — 보통 512 또는 4096.
	 *  설정자: bs_dev 구현체.
	 *  읽는 자: blobstore가 lba 계산, cluster_sz/blocklen 정합성 검사에 사용.
	 *  주의: blocklen은 io_unit_size의 일부 — io_unit ≥ blocklen. */

	uint32_t        phys_blocklen; /* In bytes */
	/* [한국어] 물리 블록 크기 (바이트) — atomic write boundary 등 결정.
	 *  설정자: bs_dev 구현체. NVMe NPWG/NPWA 등에서 파생.
	 *  읽는 자: blobstore가 메타페이지 크기 정렬 결정 시 참조.
	 *  값 의미: blocklen 의 정수배. */
};

/*
 * [한국어]
 * spdk_bs_type - blobstore의 사용자 정의 타입 라벨 (16바이트 ASCII).
 *
 * 컨슈머가 blobstore가 어떤 용도로 만들어졌는지(예: "LVOLSTORE", "BLOBFS")를 표시해두면,
 * 다른 컨슈머가 잘못 load해서 메타데이터를 깨먹는 사고를 막을 수 있다.
 * spdk_bs_init/grow 시 opts.bstype에 채워지고 super block에 영속화된다.
 */
struct spdk_bs_type {
	char bstype[SPDK_BLOBSTORE_TYPE_LENGTH];
	/* [한국어] 16바이트 ASCII 라벨 (널 종료 또는 패딩 0).
	 * 설정자: spdk_bs_set_bstype 또는 spdk_bs_opts.bstype.
	 * 읽는 자: spdk_bs_load 시 컨슈머가 spdk_bs_get_bstype으로 가져와 검증.
	 * 값 의미: 비교는 strncmp 또는 memcmp로 16바이트 정확 일치. */
};

/*
 * [한국어]
 * spdk_bs_opts - blobstore init/load/grow 시 사용하는 옵션 구조체.
 *
 * 사용 패턴:
 *   struct spdk_bs_opts opts;
 *   spdk_bs_opts_init(&opts, sizeof(opts));   // 기본값 채움
 *   opts.cluster_sz = 4 * 1024 * 1024;        // 필요 필드 변경
 *   spdk_bs_init(dev, &opts, cb, ctx);
 *
 * opts_size 필드를 사용한 ABI versioning에 주의 — 라이브러리가 새 필드를 추가하더라도
 * 옛 사용자 코드가 깨지지 않게 하기 위함.
 */
struct spdk_bs_opts {
	/** Size of cluster in bytes. Must be multiple of 4KiB page size. */
	uint32_t cluster_sz;
	/* [한국어] cluster 크기 (바이트) — blob 할당의 단위. 보통 1MB ~ 4MB.
	 * 설정자: 사용자 코드(기본값은 spdk_bs_opts_init이 채움).
	 * 읽는 자: lib/blob 코어가 cluster bitmap 및 io_unit ↔ cluster 변환에 사용.
	 * 값 범위: 4KiB 페이지 크기의 배수, blocklen의 배수, 보통 1MB 이상.
	 * 영향: 작을수록 fragmentation 적지만 메타데이터 오버헤드 증가. */

	/** Count of the number of pages reserved for metadata */
	uint32_t num_md_pages;
	/* [한국어] 메타데이터 페이지(MD page) 예약 개수.
	 * 설정자: 사용자 또는 init이 디바이스 크기에서 자동 산출.
	 * 읽는 자: lib/blob이 super block 직후 영역에 num_md_pages × md_page_size 만큼 메타 영역 할당.
	 * 값 의미: 동시 보존 가능한 blob 메타페이지 수의 상한 — 부족하면 새 blob 생성 시 -ENOMEM. */

	/** Maximum simultaneous metadata operations */
	uint32_t max_md_ops;
	/* [한국어] 동시에 in-flight 가능한 메타데이터 작업 개수의 상한.
	 * 설정자: 사용자 또는 기본값.
	 * 읽는 자: lib/blob이 메타 op pool 크기로 사용 (resize/sync_md/create 등).
	 * 영향: 크면 동시성 ↑ 메모리 ↑, 작으면 큐잉 발생. */

	/** Maximum simultaneous operations per channel */
	uint32_t max_channel_ops;
	/* [한국어] I/O channel 당 동시 in-flight 가능한 데이터 op 개수.
	 * 설정자: 사용자.
	 * 읽는 자: spdk_bs_alloc_io_channel이 channel 내부 request pool 크기 결정.
	 * 영향: NVMe queue depth와 매칭되어야 throughput 최적. */

	/** Clear method */
	enum bs_clear_method  clear_method;
	/* [한국어] cluster 해제 시 백킹 디바이스 정리 방식 (UNMAP/WRITE_ZEROES/NONE).
	 * 설정자: 사용자.
	 * 읽는 자: cluster 반환 경로에서 lib/blob 코어가 분기.
	 * 값 의미: 위 enum bs_clear_method 참조. */

	/** Blobstore type */
	struct spdk_bs_type bstype;
	/* [한국어] blobstore 컨슈머 식별 라벨 (16바이트).
	 * 설정자: 컨슈머가 자기 시그니처로 채움 (예: "LVOLSTORE").
	 * 읽는 자: super block에 영속화되어 load 시 컨슈머가 검증.
	 * 값 의미: 잘못된 컨슈머가 잘못된 blobstore 열어 깨먹는 사고 방지. */

	/** Metadata page size */
	uint32_t md_page_size;
	/* [한국어] 메타데이터 페이지 크기 (바이트) — 보통 4096.
	 * 설정자: 사용자 (init 시) 또는 load 시 super block에서 복원.
	 * 읽는 자: 메타페이지 read/write 시 lib/blob 코어가 사용.
	 * 값 범위: blocklen의 배수, 보통 4KiB. */

	/** Callback function to invoke for each blob. */
	spdk_blob_op_with_handle_complete iter_cb_fn;
	/* [한국어] load 시 발견된 각 blob에 대해 호출될 콜백 (선택적).
	 * 설정자: 사용자 (NULL이면 일반 load).
	 * 읽는 자: lib/blob의 load 경로가 메타 복원 후 각 blob을 열어 이 콜백 호출.
	 * 사용처: 컨슈머가 load 도중 blob 별 초기화(이름 등록 등)를 수행. */

	/** Argument passed to iter_cb_fn for each blob. */
	void *iter_cb_arg;
	/* [한국어] iter_cb_fn에 함께 전달될 사용자 컨텍스트.
	 * 설정자: 사용자.
	 * 읽는 자: iter_cb_fn 내부에서만 사용.
	 * 값 범위: NULL 가능. */

	/**
	 * The size of spdk_bs_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * After that, new added fields should be put in the end of the struct.
	 */
	size_t opts_size;
	/* [한국어] caller가 컴파일된 시점의 sizeof(struct spdk_bs_opts) — ABI 호환 핵심.
	 * 설정자: 사용자(spdk_bs_opts_init이 자동으로 sizeof로 설정).
	 * 읽는 자: lib/blob이 opts_size를 보고 자기보다 작은 구조체를 받았을 때 그만큼만 읽고
	 *           나머지는 기본값으로 채움.
	 * 영향: 새 필드는 반드시 구조체 "끝"에 추가되어야 이 메커니즘이 동작. */

	/** Force recovery during import. This is a uint64_t for padding reasons, treated as a bool. */
	uint64_t force_recover;
	/* [한국어] load 시 강제로 recovery 절차를 실행하라는 플래그(0/1).
	 * 설정자: 사용자가 비정상 종료 후 강제 복구 시 1로 설정.
	 * 읽는 자: lib/blob load 경로가 super block의 clean shutdown 비트와 무관하게 복구 진입.
	 * 타입 이유: 정렬·패딩 안정성을 위해 bool 대신 uint64_t. */

	/**
	 * External snapshot creation callback to register with the blobstore.
	 */
	spdk_bs_esnap_dev_create esnap_bs_dev_create;
	/* [한국어] ESnap clone blob을 열거나 load할 때 외부 base bs_dev를 만드는 콜백.
	 * 설정자: ESnap을 지원하는 컨슈머(lvol)가 자기 콜백으로 등록.
	 * 읽는 자: lib/blob 코어가 esnap_id가 있는 blob을 열 때 호출.
	 * NULL 의미: ESnap 미지원 — esnap clone 발견 시 load 실패. */

	/**
	 * Context to pass with esnap_bs_dev_create.
	 */
	void *esnap_ctx;
	/* [한국어] esnap_bs_dev_create의 첫 인자 bs_ctx로 전달될 컨텍스트.
	 * 설정자: 컨슈머 (보통 자기 lvolstore 핸들 등).
	 * 읽는 자: 콜백 안에서 컨슈머 자체 상태 접근.
	 * 값 범위: 컨슈머가 정의 (NULL 가능). */
} __attribute__((packed));
/* [한국어] packed: 패딩 제거로 정확히 88바이트 ABI 강제. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_bs_opts) == 88, "Incorrect size");
/* [한국어] 컴파일 타임 어서션: 구조체 변경 시 sizeof가 바뀌면 알아채고 의도적으로 갱신하도록. */

/**
 * Initialize a spdk_bs_opts structure to the default blobstore option values.
 *
 * \param opts The spdk_bs_opts structure to be initialized.
 * \param opts_size The opts_size must be the size of spdk_bs_opts structure.
 */
/*
 * [한국어]
 * spdk_bs_opts_init - spdk_bs_opts를 라이브러리 기본값으로 초기화한다.
 *
 * @opts: 초기화 대상 구조체 포인터 (호출자가 스택/힙에 할당).
 * @opts_size: caller가 인식하는 sizeof(struct spdk_bs_opts) — 보통 sizeof(opts).
 *
 * 사용자 코드가 항상 호출해야 하는 표준 진입점. 라이브러리가 모든 필드를 안전한 기본값
 * (cluster_sz=1MB, num_md_pages=자동, clear_method=UNMAP 등)으로 채워준 후 사용자가
 * 변경하고 싶은 필드만 덮어쓴다. opts_size를 함께 받음으로써 caller가 컴파일된 헤더가
 * 라이브러리 헤더보다 작더라도 안전하게 호출 가능 (ABI 호환).
 *
 * 호출 컨텍스트: 임의의 thread (단순 메모리 초기화).
 * 호출 체인: [사용자] → spdk_bs_opts_init → memset/필드 채움.
 */
void spdk_bs_opts_init(struct spdk_bs_opts *opts, size_t opts_size);

/**
 * Load a blobstore from the given device.
 *
 * \param dev Blobstore block device.
 * \param opts The structure which contains the option values for the blobstore.
 * \param cb_fn Called when the loading is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_load - 기존 blobstore를 디스크에서 비동기 로드한다.
 *
 * @dev: 백킹 bs_dev (load 후 blobstore가 소유권을 가짐 — 사용자가 destroy 금지).
 * @opts: 옵션 (bstype 검증, esnap_bs_dev_create 등록 등). NULL이면 기본값.
 * @cb_fn: 완료 콜백 (성공 시 bs 핸들과 bserrno=0).
 * @cb_arg: cb_fn 사용자 컨텍스트.
 *
 * 동작 단계:
 *  1) super block(LBA 0) read → 매직넘버, version, bstype 검증.
 *  2) used_md_pages bitmap, used_clusters bitmap 복원.
 *  3) 각 메타페이지를 순회하며 blob 메타데이터 인덱스 빌드.
 *  4) opts.iter_cb_fn 등록 시 각 blob에 대해 호출.
 *  5) ESnap clone 발견 시 opts.esnap_bs_dev_create 호출해 base bs_dev 생성.
 *  6) cb_fn(cb_arg, bs, 0) 호출.
 *
 * 에러 경로: 매직 mismatch → -EILSEQ, force_recover=0 인데 dirty → -EIO,
 * 메타 inconsistency → -EUCLEAN. 실패 시 bs는 NULL.
 *
 * 실행 컨텍스트: 호출 thread가 metadata thread가 된다 — 이후 모든 메타 op는 이 thread에서.
 */
void spdk_bs_load(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts,
		  spdk_bs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Grow a blobstore to fill the underlying device
 * Cannot be used on loaded blobstore.
 *
 * \param dev Blobstore block device.
 * \param opts The structure which contains the option values for the blobstore.
 * \param cb_fn Called when the loading is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_grow - 백킹 디바이스 확장에 맞춰 blobstore를 확장 (offline 버전).
 *
 * @dev: 백킹 bs_dev (확장된 디바이스).
 * @opts: 옵션.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 사용처: 백킹 bdev(LV, RAID 등)이 커진 후 blobstore의 cluster 영역을 새 크기까지 확장.
 * 단, 이 함수는 "loaded되지 않은" blobstore에만 사용 가능 — 즉 load 없이 직접 grow 후
 * 다시 load하는 형태. live grow는 spdk_bs_grow_live를 쓴다.
 *
 * 동작: super block 읽음 → 새 디바이스 크기 검증 → cluster bitmap 확장 → super block 갱신.
 * 호출 컨텍스트: load와 동일 — metadata thread.
 */
void spdk_bs_grow(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts,
		  spdk_bs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Grow a blobstore to fill the underlying device.
 * Can be used on loaded blobstore, even with opened blobs.
 *
 * \param bs blobstore to grow.
 * \param cb_fn Called when the growing is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_grow_live - 라이브 blobstore를 확장 (opened blob이 있어도 가능).
 *
 * @bs: 이미 load된 blobstore.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * spdk_bs_grow와 차이: load 상태에서 사용 가능 — blob들이 열려 있어도 안전.
 * 동작: 백킹 bs_dev->blockcnt가 커졌음을 감지 → cluster bitmap 확장 → super block 갱신.
 * 사용처: lvolstore가 underlying bdev resize 이벤트를 받으면 자동 호출.
 * 실행 컨텍스트: metadata thread.
 */
void spdk_bs_grow_live(struct spdk_blob_store *bs,
		       spdk_bs_op_complete cb_fn, void *cb_arg);

/**
 * Initialize a blobstore on the given device.
 *
 * \param dev Blobstore block device.
 * \param opts The structure which contains the option values for the blobstore.
 * \param cb_fn Called when the initialization is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_init - 빈(또는 기존 데이터를 무시할) bs_dev에 새 blobstore를 비동기 초기화한다.
 *
 * @dev: 백킹 bs_dev (init 후 blobstore가 소유).
 * @opts: 옵션 (cluster_sz, num_md_pages, bstype 등).
 * @cb_fn: 완료 콜백 (bs 핸들 반환).
 * @cb_arg: 컨텍스트.
 *
 * 동작 단계:
 *  1) opts에 따라 super block 작성 (매직, version, cluster_sz, num_md_pages, bstype).
 *  2) 메타데이터 영역 0으로 클리어 (clear_method 적용).
 *  3) cluster bitmap 초기화 (메타 영역만 점유).
 *  4) cb_fn 호출.
 *
 * 호출자 책임: dev에 기존 데이터가 있으면 손실됨 — 호출 전 백업 또는 빈 디바이스 보장.
 * 실행 컨텍스트: 호출 thread = metadata thread.
 */
void spdk_bs_init(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts,
		  spdk_bs_op_with_handle_complete cb_fn, void *cb_arg);

/*
 * [한국어]
 * spdk_bs_dump_print_xattr - dump 시 사용자 정의 xattr 값을 사람이 읽을 수 있게 출력하는 콜백.
 *
 * @fp: dump 출력 대상 FILE.
 * @bstype: 이 blobstore의 bstype 라벨 — 컨슈머가 자기 xattr 식별에 사용.
 * @name: xattr 이름.
 * @value: xattr 값(불투명 바이트열).
 * @value_length: 값 길이.
 *
 * blobstore 코어는 xattr 값의 의미를 모르므로 컨슈머(lvol 등)가 이 콜백을 등록해
 * "lvol_uuid", "lvol_name" 등 자기 xattr을 적절히 디코딩해 출력.
 */
typedef void (*spdk_bs_dump_print_xattr)(FILE *fp, const char *bstype, const char *name,
		const void *value, size_t value_length);

/**
 * Dump a blobstore's metadata to a given FILE in human-readable format.
 *
 * \param dev Blobstore block device.
 * \param fp FILE pointer to dump the metadata contents.
 * \param print_xattr_fn Callback function to interpret external xattrs.
 * \param cb_fn Called when the dump is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_dump - blobstore 메타데이터를 사람이 읽을 수 있는 텍스트로 fp에 dump.
 *
 * @dev: 검사 대상 백킹 bs_dev.
 * @fp: 출력 파일(보통 stdout 또는 로그 파일).
 * @print_xattr_fn: 사용자 정의 xattr 디코더 콜백 (NULL이면 hex dump).
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작: super block, used bitmap, 각 blob의 메타페이지(blob ID, cluster map, xattr table)을
 * 텍스트로 출력. 디버깅·포렌식 용도. 도구 spdk_blobstore_inspect, blob_cli가 활용.
 *
 * 주의: dump는 read-only 작업이지만 bs_dev를 internal하게 잠시 사용하므로 동시 init/load
 * 와는 충돌. 보통 단독 dump 도구로 호출.
 */
void spdk_bs_dump(struct spdk_bs_dev *dev, FILE *fp, spdk_bs_dump_print_xattr print_xattr_fn,
		  spdk_bs_op_complete cb_fn, void *cb_arg);
/**
 * Destroy the blobstore.
 *
 * It will destroy the blobstore by zeroing the super block.
 *
 * \param bs blobstore to destroy.
 * \param cb_fn Called when the destruction is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_destroy - blobstore를 영구 파괴 (super block을 0으로 덮음).
 *
 * @bs: 대상 blobstore.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작: super block에 0을 기록 → 다음 load 시 매직 mismatch로 빈 디바이스로 인식.
 * 메모리 인스턴스도 unload와 동일하게 정리. 사용자 데이터(blob 클러스터 데이터)는
 * 명시적으로 지우지 않음 — 필요 시 호출자가 사전에 unmap.
 *
 * 실행 컨텍스트: metadata thread.
 * 호출 후: bs 포인터는 더 이상 유효하지 않음.
 */
void spdk_bs_destroy(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn,
		     void *cb_arg);

/**
 * Unload the blobstore.
 *
 * It will flush all volatile data to disk and clean up the blobstore from memory.
 * When disk is no longer present the volatile data will not be persisted.
 *
 * On success or -EIO the blobstore pointer is no longer valid.
 *
 * \param bs blobstore to unload.
 * \param cb_fn Called when the unloading is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_unload - 휘발성 데이터를 모두 디스크에 flush 후 blobstore 메모리 인스턴스 해제.
 *
 * @bs: 대상.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작 단계:
 *  1) 모든 dirty 메타페이지 sync.
 *  2) super block에 clean shutdown 비트 기록 (다음 load 시 force_recover=0이라도 OK).
 *  3) bs_dev->destroy 호출 (어댑터 해제).
 *  4) blob/메모리 free.
 *
 * 사전조건: 모든 blob이 close되어 있어야 함. open된 blob이 있으면 -EBUSY.
 * 디스크가 사라진 상태(EIO)에서도 메모리는 해제됨 — 그 경우 dirty data는 영구 손실.
 * 실행 컨텍스트: metadata thread.
 */
void spdk_bs_unload(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn, void *cb_arg);

/**
 * Set a super blob on the given blobstore.
 *
 * This will be retrievable immediately after spdk_bs_load() on the next initialization.
 *
 * \param bs blobstore.
 * \param blobid The id of the blob which will be set as the super blob.
 * \param cb_fn Called when the setting is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_set_super - "super blob" 포인터를 super block에 기록.
 *
 * @bs: 대상.
 * @blobid: super로 지정할 기존 blob ID.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * super blob: 컨슈머가 "이 blob부터 메타데이터 트리가 시작된다"는 진입점을 표시하기 위한
 * 지정 blob. 다음 load 시 spdk_bs_get_super로 즉시 조회 가능.
 * 사용 예: BlobFS의 root metadata blob, lvol의 lvolstore metadata blob.
 *
 * 동작: super block의 super_blob 필드 갱신 → 영속화.
 * 실행 컨텍스트: metadata thread.
 */
void spdk_bs_set_super(struct spdk_blob_store *bs, spdk_blob_id blobid,
		       spdk_bs_op_complete cb_fn, void *cb_arg);

/**
 * Get the super blob. The obtained blob id will be passed to the callback function.
 *
 * \param bs blobstore.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_get_super - 등록된 super blob의 ID를 콜백으로 반환.
 *
 * @bs: 대상.
 * @cb_fn: 완료 콜백 (blobid + bserrno).
 * @cb_arg: 컨텍스트.
 *
 * super blob 미설정 시 cb_fn에 -ENOENT가 전달됨.
 * 실행 컨텍스트: metadata thread (그러나 단순 메모리 조회라 사실상 즉시 콜백).
 */
void spdk_bs_get_super(struct spdk_blob_store *bs,
		       spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/**
 * Get the cluster size in bytes.
 *
 * \param bs blobstore to query.
 *
 * \return cluster size.
 */
/*
 * [한국어]
 * spdk_bs_get_cluster_size - blobstore의 cluster 크기 (바이트) 조회 (동기).
 *
 * @bs: 대상.
 * @return: cluster_sz (보통 1MB ~ 4MB).
 *
 * cluster: blob 할당 단위. blob의 num_clusters × cluster_sz = blob 용량.
 * 사용처: blob resize 인자 산정, IO 정렬 결정.
 * 호출 컨텍스트: 임의 thread (단순 atomic load).
 */
uint64_t spdk_bs_get_cluster_size(struct spdk_blob_store *bs);

/**
 * Get the metadata page size in bytes.
 *
 * \param bs blobstore to query.
 *
 * \return page size.
 */
/*
 * [한국어]
 * spdk_bs_get_page_size - 메타데이터 페이지 크기 (바이트) 조회.
 *
 * @bs: 대상.
 * @return: md_page_size (보통 4096).
 *
 * 메타페이지는 blob의 cluster map, xattr 등이 저장되는 단위.
 * 호출 컨텍스트: 임의 thread.
 */
uint64_t spdk_bs_get_page_size(struct spdk_blob_store *bs);

/**
 * Get the maximum growable size of blobstore, in bytes.
 *
 * \param bs blobstore to query.
 *
 * \return the maximum growable size in bytes
 */
/*
 * [한국어]
 * spdk_bs_get_max_growable_size - 이 blobstore가 최대로 확장 가능한 크기 (바이트).
 *
 * @bs: 대상.
 * @return: cluster bitmap의 비트 수 한계로 결정되는 상한.
 *
 * super block에 예약된 cluster bitmap 크기로 결정됨 — 이 한계를 넘으면 grow 불가.
 * 사용처: lvol/관리툴이 사용자에게 grow 가능 여부 표시.
 */
uint64_t
spdk_bs_get_max_growable_size(struct spdk_blob_store *bs);

/**
 * Get the io unit size in bytes.
 *
 * \param bs blobstore to query.
 *
 * \return io unit size.
 */
/*
 * [한국어]
 * spdk_bs_get_io_unit_size - io_unit 크기 (바이트) 조회.
 *
 * @bs: 대상.
 * @return: io_unit_size (보통 blocklen과 같거나 그 배수).
 *
 * io_unit: blob_io_read/write의 offset/length 단위. cluster ⊃ pages ⊃ io_units 계층 구조.
 * 사용처: 사용자 코드가 read/write offset/length 계산.
 */
uint64_t spdk_bs_get_io_unit_size(struct spdk_blob_store *bs);

/**
 * Get the number of free clusters.
 *
 * \param bs blobstore to query.
 *
 * \return the number of free clusters.
 */
/*
 * [한국어]
 * spdk_bs_free_cluster_count - 미할당(=사용 가능) cluster 수 조회.
 *
 * @bs: 대상.
 * @return: 자유 cluster 개수 — × cluster_sz 가 사용 가능 용량.
 *
 * 동시 cluster 할당/해제 중에는 약간 stale할 수 있으나 lock-free 카운터.
 * 사용처: 잔여 용량 모니터링, RPC bdev_get_iostat 등에서 노출.
 */
uint64_t spdk_bs_free_cluster_count(struct spdk_blob_store *bs);

/**
 * Get the total number of clusters accessible by user.
 *
 * \param bs blobstore to query.
 *
 * \return the total number of clusters accessible by user.
 */
/*
 * [한국어]
 * spdk_bs_total_data_cluster_count - 사용자 접근 가능한 총 data cluster 수.
 *
 * @bs: 대상.
 * @return: 메타데이터 영역을 제외한 data cluster 총 개수.
 *
 * total - free = 현재 할당된 cluster 수.
 * 사용처: 용량 보고, 모니터링.
 */
uint64_t spdk_bs_total_data_cluster_count(struct spdk_blob_store *bs);

/**
 * Get the blob id.
 *
 * \param blob Blob struct to query.
 *
 * \return blob id.
 */
/*
 * [한국어]
 * spdk_blob_get_id - 열린 blob 핸들에서 blob ID 조회.
 *
 * @blob: 대상.
 * @return: 64비트 blob ID.
 *
 * 사용처: blob 핸들만 갖고 있을 때 ID로 변환 (예: snapshot 메타에 부모 ID 기록).
 * 호출 컨텍스트: 임의 thread (필드 read).
 */
spdk_blob_id spdk_blob_get_id(struct spdk_blob *blob);

/**
 * Get the number of io_units allocated to the blob.
 *
 * \param blob Blob struct to query.
 *
 * \return the number of io_units.
 */
/*
 * [한국어]
 * spdk_blob_get_num_io_units - blob의 (논리) io_unit 수.
 *
 * @blob: 대상.
 * @return: blob 크기를 io_unit 단위로 표현한 값.
 *
 * num_clusters × (cluster_sz / io_unit_size). thin-prov여도 논리 크기 기준.
 */
uint64_t spdk_blob_get_num_io_units(struct spdk_blob *blob);

/**
 * Get the number of clusters in the blob.
 *
 * This value represents the size of the blob in number of clusters.
 *
 * \param blob Blob struct to query.
 *
 * \return the number of clusters.
 */
/*
 * [한국어]
 * spdk_blob_get_num_clusters - blob의 (논리) cluster 수 = 크기.
 *
 * @blob: 대상.
 * @return: 논리 cluster 수.
 *
 * thin-prov인 경우 일부는 미할당 — get_num_allocated_clusters와 다를 수 있다.
 */
uint64_t spdk_blob_get_num_clusters(struct spdk_blob *blob);

/**
 * Get the number of allocated clusters to the blob.
 *
 * In case of a thin-provisioned blob, this value is less than or equal
 * to the number of clusters in the blob, otherwise they are equal.
 *
 * \param blob Blob struct to query.
 *
 * \return the number of clusters actually allocated to the blob.
 */
/*
 * [한국어]
 * spdk_blob_get_num_allocated_clusters - 실제로 백킹 디바이스에 할당된 cluster 수.
 *
 * @blob: 대상.
 * @return: thin-prov blob의 경우 num_clusters 이하, 그렇지 않으면 동일.
 *
 * 사용처: thin-prov 사용량 모니터링. allocated × cluster_sz = 실제 disk usage.
 */
uint64_t spdk_blob_get_num_allocated_clusters(struct spdk_blob *blob);

/**
 * Get next allocated io_unit
 *
 * Starting at 'offset' io_units into the blob, returns the offset of
 * the first allocated io unit found.
 * If 'offset' points to an allocated io_unit, same offset is returned.
 *
 * \param blob Blob struct to query.
 * \param offset Offset is in io units from the beginning of the blob.
 *
 * \return offset in io_units or UINT64_MAX if no allocated io_unit found
 */
/*
 * [한국어]
 * spdk_blob_get_next_allocated_io_unit - offset 이후 첫 번째 "할당된" io_unit 위치.
 *
 * @blob: 대상.
 * @offset: 검색 시작 io_unit offset.
 * @return: 첫 할당된 io_unit offset 또는 UINT64_MAX(없음).
 *
 * thin-prov blob에서 sparse 영역을 스킵하며 데이터가 있는 부분만 처리할 때 사용.
 * 사용처: dd-like 도구, shallow copy의 cluster 스캔.
 */
uint64_t spdk_blob_get_next_allocated_io_unit(struct spdk_blob *blob, uint64_t offset);

/**
 * Get next unallocated io_unit
 *
 * Starting at 'offset' io_units into the blob, returns the offset of
 * the first unallocated io unit found.
 * If 'offset' points to an unallocated io_unit, same offset is returned.
 *
 * \param blob Blob struct to query.
 * \param offset Offset is in io units from the beginning of the blob.
 *
 * \return offset in io_units or UINT64_MAX if only allocated io_unit found
 */
/*
 * [한국어]
 * spdk_blob_get_next_unallocated_io_unit - offset 이후 첫 미할당 io_unit 위치.
 *
 * @blob: 대상.
 * @offset: 검색 시작.
 * @return: 첫 미할당 io_unit offset 또는 UINT64_MAX(전부 할당됨).
 *
 * 위 함수의 반대 — 데이터가 없는 hole을 찾을 때.
 * 두 함수를 번갈아 사용해 (allocated, unallocated) 구간 페어를 효율적으로 열거 가능.
 */
uint64_t spdk_blob_get_next_unallocated_io_unit(struct spdk_blob *blob, uint64_t offset);

/*
 * [한국어]
 * spdk_blob_xattr_opts - blob 생성 시점에 함께 기록될 xattr 묶음 옵션.
 *
 * spdk_blob_opts.xattrs 또는 spdk_bs_create_snapshot/clone에 직접 전달되어 새 blob의
 * 메타페이지에 처음부터 xattr이 영속화되도록 한다.
 */
struct spdk_blob_xattr_opts {
	/* Number of attributes */
	size_t	count;
	/* [한국어] xattr 개수.
	 * 설정자: 사용자.
	 * 읽는 자: lib/blob 코어가 names 배열의 유효 길이로 사용.
	 * 값 범위: 0 가능 (xattr 없음). */

	/* Array of attribute names. Caller should free this array after use. */
	char	**names;
	/* [한국어] xattr 이름 문자열 배열 (count개의 NUL-terminated C string).
	 * 설정자: 사용자가 strdup 등으로 할당.
	 * 읽는 자: lib/blob이 각 이름에 대해 get_value 콜백으로 값 조회.
	 * 메모리: 배열과 각 문자열 모두 호출자가 소유 — 작업 완료 후 호출자가 free. */

	/* User context passed to get_xattr_value function */
	void	*ctx;
	/* [한국어] get_value 콜백에 함께 전달될 사용자 컨텍스트.
	 * 설정자: 사용자.
	 * 읽는 자: get_value 내부.
	 * 값 범위: NULL 가능. */

	/* Callback that will return value for each attribute name. */
	void	(*get_value)(void *xattr_ctx, const char *name,
			     const void **value, size_t *value_len);
	/* [한국어] 이름별로 xattr 값을 반환하는 콜백.
	 * 인자: xattr_ctx(=ctx), name, [out] value/value_len.
	 * 호출 시점: blob 생성 중 lib/blob이 각 이름을 순회하며 호출.
	 * 의의: 값 메모리 소유권을 호출자가 유지하면서 lazy하게 노출 — 큰 값이 한꺼번에 복제되는 것을 방지. */
};

/*
 * [한국어]
 * spdk_blob_opts - 새 blob 생성 시 사용되는 옵션.
 *
 * spdk_bs_create_blob_ext에 전달. spdk_blob_opts_init으로 기본값 채운 후 변경.
 */
struct spdk_blob_opts {
	uint64_t  num_clusters;
	/* [한국어] 새 blob의 초기 cluster 개수 (논리 크기).
	 * 설정자: 사용자.
	 * 읽는 자: lib/blob이 cluster bitmap 할당(thick) 또는 메타에 size 기록(thin).
	 * 값 범위: 1 이상 (0이면 빈 blob — esnap_id 동반 시 가능). */

	bool	thin_provision;
	/* [한국어] thin-provisioning 플래그.
	 * 설정자: 사용자.
	 * 읽는 자: lib/blob이 cluster를 즉시 할당할지(thick=false), 데이터 write 시점에 lazy 할당할지(thin=true) 결정.
	 * 영향: thin이면 free_cluster 즉시 감소 안 함, 첫 write에 cluster 할당. */

	enum blob_clear_method clear_method;
	/* [한국어] 이 blob 단위 cluster 정리 방식 (BS 기본값을 override).
	 * 설정자: 사용자.
	 * 읽는 자: lib/blob의 해제·resize 축소 경로.
	 * 값 의미: enum blob_clear_method 참조. */

	struct spdk_blob_xattr_opts xattrs;
	/* [한국어] 생성과 동시에 기록할 xattr 묶음.
	 * 설정자: 사용자 (count=0 이면 xattr 없음).
	 * 읽는 자: lib/blob이 첫 메타페이지 영속화 시 xattr 직렬화.
	 * 사용처: lvol이 lvol_uuid/lvol_name을 처음부터 등록. */

	/** Enable separate extent pages in metadata */
	bool use_extent_table;
	/* [한국어] 큰 blob을 위해 cluster map을 별도 extent 페이지로 분리할지 여부.
	 * 설정자: 사용자(또는 기본 true 권장 — 큰 blob에 효율적).
	 * 읽는 자: lib/blob이 메타페이지 레이아웃 결정.
	 * 영향: false면 base 페이지 하나에 cluster map → 큰 blob에서 메타 압박; true면 extent 페이지 별도 chain. */

	/**
	 * The size of spdk_blob_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
	/* [한국어] caller 컴파일 시점의 sizeof — ABI 호환 핵심 (spdk_bs_opts와 동일 메커니즘). */

	/**
	 * If set, create an esnap clone. The memory referenced by esnap_id will be copied into the
	 * blob's metadata and can be retrieved with spdk_blob_get_esnap_id(), typically from an
	 * esnap_bs_dev_create() callback.
	 * See struct_bs_opts.
	 *
	 * When esnap_id is specified, num_clusters should be specified. If it is not, the blob will
	 * have no capacity until spdk_blob_resize() is called.
	 */
	const void *esnap_id;
	/* [한국어] ESnap base 식별자 — 이 blob을 ESnap clone으로 만든다.
	 * 설정자: 사용자(NULL이면 일반 blob).
	 * 읽는 자: lib/blob이 메타페이지에 복사 영속화 → 이후 esnap_bs_dev_create로 base 매핑.
	 * 메모리: lib/blob이 내부로 복사하므로 호출 후 호출자는 free 가능.
	 * 형식: 컨슈머 정의 — 보통 외부 bdev UUID 또는 snapshot identifier. */

	/**
	 * The size of data referenced by esnap_id, in bytes.
	 */
	uint64_t esnap_id_len;
	/* [한국어] esnap_id의 바이트 길이.
	 * 설정자: 사용자.
	 * 읽는 자: lib/blob이 메타페이지에 함께 영속화.
	 * 값 범위: esnap_id != NULL일 때만 유효 (>0). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_blob_opts) == 80, "Incorrect size");
/* [한국어] ABI 안정성: sizeof = 80바이트 어서션. 새 필드 추가 시 의도적으로 갱신. */

/**
 * Initialize a spdk_blob_opts structure to the default blob option values.
 *
 * \param opts spdk_blob_opts structure to initialize.
 * \param opts_size It must be the size of spdk_blob_opts structure.
 */
/*
 * [한국어]
 * spdk_blob_opts_init - spdk_blob_opts를 라이브러리 기본값으로 초기화.
 *
 * @opts: 대상.
 * @opts_size: caller 측 sizeof(struct spdk_blob_opts).
 *
 * 기본: num_clusters=0, thin_provision=false, clear_method=DEFAULT, xattrs.count=0,
 * use_extent_table=true (대부분 true가 권장), esnap_id=NULL.
 *
 * 사용 패턴: spdk_bs_opts_init과 동일.
 * 호출 컨텍스트: 임의 thread.
 */
void spdk_blob_opts_init(struct spdk_blob_opts *opts, size_t opts_size);

/**
 * Create a new blob with options on the given blobstore. The new blob id will
 * be passed to the callback function.
 *
 * \param bs blobstore.
 * \param opts The structure which contains the option values for the new blob.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_create_blob_ext - 옵션을 명시해 새 blob 생성.
 *
 * @bs: 대상.
 * @opts: blob 옵션 (NULL이면 기본).
 * @cb_fn: 완료 콜백 (blob ID 반환).
 * @cb_arg: 컨텍스트.
 *
 * 동작 단계:
 *  1) used_md_pages bitmap에서 새 메타페이지 슬롯 할당 → 이 슬롯 인덱스가 blob ID.
 *  2) opts.thin_provision=false면 num_clusters 만큼 cluster bitmap에서 할당.
 *  3) opts.xattrs를 메타페이지에 직렬화.
 *  4) 메타페이지 영속화(write) → 완료 시 cb_fn 호출.
 *
 * 에러: -ENOMEM(메타페이지 부족), -ENOSPC(cluster 부족, thick prov), -EINVAL(opts 오류).
 * 실행 컨텍스트: metadata thread.
 */
void spdk_bs_create_blob_ext(struct spdk_blob_store *bs, const struct spdk_blob_opts *opts,
			     spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/**
 * Create a new blob with default option values on the given blobstore.
 * The new blob id will be passed to the callback function.
 *
 * \param bs blobstore.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_create_blob - 기본 옵션으로 빈 blob 생성 (편의 함수).
 *
 * @bs: 대상.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * num_clusters=0, thin_provision=false인 빈 blob 생성. 이후 spdk_blob_resize로 확장.
 * spdk_bs_create_blob_ext의 단순화 wrapper.
 */
void spdk_bs_create_blob(struct spdk_blob_store *bs,
			 spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/**
 * Create a read-only snapshot of specified blob with provided options.
 * This will automatically sync specified blob.
 *
 * When operation is done, original blob is converted to the thin-provisioned
 * blob with a newly created read-only snapshot set as a backing blob.
 * Structure snapshot_xattrs as well as anything it references (like e.g. names
 * array) must be valid until the completion is called.
 *
 * \param bs blobstore.
 * \param blobid Id of the source blob used to create a snapshot.
 * \param snapshot_xattrs xattrs specified for snapshot.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_create_snapshot - 기존 blob을 read-only snapshot으로 동결하고 원본을 thin clone으로 변환.
 *
 * @bs: 대상.
 * @blobid: 스냅샷의 원본 blob ID (열려 있어도 무방).
 * @snapshot_xattrs: 스냅샷에 등록할 xattr (NULL 가능).
 * @cb_fn: 완료 콜백 (새 스냅샷의 blob ID 반환).
 * @cb_arg: 컨텍스트.
 *
 * CoW 동작 단계:
 *  1) 원본 blob의 dirty 메타 sync.
 *  2) 새 read-only blob 생성 — 원본의 cluster map을 그대로 복사 (실제 데이터는 공유).
 *  3) 원본 blob을 thin-provisioned로 전환하고 parent_blob = new_snapshot으로 설정.
 *  4) 원본의 cluster map은 비워짐 → 향후 write 시 CoW 발동(부모 cluster를 복사 후 자기에게 할당).
 *  5) cb_fn에 스냅샷 ID 전달.
 *
 * 결과: 원본은 변경 가능한 thin clone, 스냅샷은 영구 read-only.
 * 사용처: lvol snapshot, VM disk versioning.
 * 메모리: snapshot_xattrs는 cb_fn 호출 전까지 유효해야 함.
 * 실행 컨텍스트: metadata thread.
 */
void spdk_bs_create_snapshot(struct spdk_blob_store *bs, spdk_blob_id blobid,
			     const struct spdk_blob_xattr_opts *snapshot_xattrs,
			     spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/**
 * Create a clone of specified read-only blob.
 *
 * Structure clone_xattrs as well as anything it references (like e.g. names
 * array) must be valid until the completion is called.
 *
 * \param bs blobstore.
 * \param blobid Id of the read only blob used as a snapshot for new clone.
 * \param clone_xattrs xattrs specified for clone.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_create_clone - read-only snapshot에서 새 thin clone 생성.
 *
 * @bs: 대상.
 * @blobid: 부모로 사용할 read-only blob ID (반드시 read_only).
 * @clone_xattrs: 클론에 등록할 xattr (NULL 가능).
 * @cb_fn: 완료 콜백 (새 클론 ID 반환).
 * @cb_arg: 컨텍스트.
 *
 * 동작:
 *  1) 새 thin-provisioned blob 생성 (cluster map 비어있음).
 *  2) parent_blob = blobid 설정.
 *  3) 클론 read = cluster 미할당 시 부모로 fallthrough; write = CoW 후 자기에게 할당.
 *
 * snapshot에서 여러 클론이 가능 — 한 부모, 여러 자식.
 * 사용처: VM template로 여러 인스턴스 생성, dev/test branching.
 * 실행 컨텍스트: metadata thread.
 */
void spdk_bs_create_clone(struct spdk_blob_store *bs, spdk_blob_id blobid,
			  const struct spdk_blob_xattr_opts *clone_xattrs,
			  spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/**
 * Provide table with blob id's of clones are dependent on specified snapshot.
 *
 * Ids array should be allocated and the count parameter set to the number of
 * id's it can store, before calling this function.
 *
 * If ids is NULL or count parameter is not sufficient to handle ids of all
 * clones, -ENOMEM error is returned and count parameter is updated to the
 * total number of clones.
 *
 * \param bs blobstore.
 * \param blobid Snapshots blob id.
 * \param ids Array of the clone ids or NULL to get required size in count.
 * \param count Size of ids. After call it is updated to the number of clones.
 *
 * \return -ENOMEM if count is not sufficient to store all clones.
 */
/*
 * [한국어]
 * spdk_blob_get_clones - 스냅샷에 의존하는 모든 클론들의 ID 목록을 동기적으로 조회.
 *
 * @bs: 대상.
 * @blobid: 스냅샷 blob ID.
 * @ids: [out] 클론 ID 저장 배열 (호출자 할당) 또는 NULL.
 * @count: [in/out] 입력 시 ids 배열 크기, 출력 시 실제 클론 수.
 * @return: 0 = 성공, -ENOMEM = 배열이 작거나 ids=NULL (count는 필요 크기로 갱신됨).
 *
 * 사용 패턴 (2-pass):
 *   size_t n = 0;
 *   spdk_blob_get_clones(bs, snap_id, NULL, &n);   // n에 필요 크기
 *   spdk_blob_id *arr = malloc(n*sizeof(spdk_blob_id));
 *   spdk_blob_get_clones(bs, snap_id, arr, &n);    // 채움
 *
 * 사용처: 스냅샷 삭제 안전성 검사 (의존 클론이 있으면 삭제 금지).
 * 동기 함수 — metadata thread에서 호출.
 */
int spdk_blob_get_clones(struct spdk_blob_store *bs, spdk_blob_id blobid, spdk_blob_id *ids,
			 size_t *count);

/**
 * Get the blob id for the parent snapshot of this blob.
 *
 * \param bs blobstore.
 * \param blobid Blob id.
 *
 * \return blob id of parent blob or SPDK_BLOBID_INVALID if have no parent
 */
/*
 * [한국어]
 * spdk_blob_get_parent_snapshot - 이 blob의 부모 스냅샷 ID 조회 (동기).
 *
 * @bs: 대상.
 * @blobid: 자식 blob ID.
 * @return: 부모 blob ID 또는 SPDK_BLOBID_INVALID(부모 없음 또는 ESnap clone).
 *
 * ESnap clone은 SPDK_BLOBID_EXTERNAL_SNAPSHOT가 아니라 SPDK_BLOBID_INVALID 반환 —
 * ESnap 여부는 spdk_blob_is_esnap_clone으로 별도 판정.
 * 사용처: snapshot tree 탐색, dependency 검증.
 */
spdk_blob_id spdk_blob_get_parent_snapshot(struct spdk_blob_store *bs, spdk_blob_id blobid);

/**
 * Get the id used to access the esnap clone's parent.
 *
 * \param blob The clone's blob.
 * \param id On successful return, *id will reference memory that has the same life as blob.
 * \param len On successful return *len will be the size of id in bytes.
 *
 * \return 0 on success
 * \return -EINVAL if blob is not an esnap clone.
 */
/*
 * [한국어]
 * spdk_blob_get_esnap_id - ESnap clone의 외부 base 식별자 조회 (동기).
 *
 * @blob: 대상 (ESnap clone이어야 함).
 * @id: [out] 식별자 메모리 포인터 (blob 내부 메모리 — 호출자 free 금지, blob 살아 있는 동안 유효).
 * @len: [out] id 길이 (바이트).
 * @return: 0 = 성공, -EINVAL = ESnap clone이 아님.
 *
 * 컨슈머가 esnap_bs_dev_create 콜백 안에서 자기 외부 base를 식별할 때 또는 dump 시 호출.
 * 메모리 수명: blob이 close될 때까지 유효 — 그 이후 dangling pointer 됨.
 */
int spdk_blob_get_esnap_id(struct spdk_blob *blob, const void **id, size_t *len);

/**
 * Check if blob is read only.
 *
 * \param blob Blob.
 *
 * \return true if blob is read only.
 */
/*
 * [한국어]
 * spdk_blob_is_read_only - blob의 read-only 플래그 조회.
 *
 * @blob: 대상.
 * @return: true = read-only (write 호출 시 -EPERM), false = 쓰기 가능.
 *
 * 스냅샷은 항상 true. 일반 blob도 spdk_blob_set_read_only로 변경 가능.
 */
bool spdk_blob_is_read_only(struct spdk_blob *blob);

/**
 * Check if blob is a snapshot.
 *
 * \param blob Blob.
 *
 * \return true if blob is a snapshot.
 */
/*
 * [한국어]
 * spdk_blob_is_snapshot - 이 blob이 다른 blob의 스냅샷으로 사용되고 있는지.
 *
 * @blob: 대상.
 * @return: true = 다른 blob의 부모(=스냅샷)이거나 의존자가 있는 read-only blob.
 *
 * 사용처: 스냅샷 삭제 가드, lvol UI 분류.
 */
bool spdk_blob_is_snapshot(struct spdk_blob *blob);

/**
 * Check if blob is a clone of a blob.
 *
 * Clones of external snapshots will return false. See spdk_blob_is_esnap_clone.
 *
 * \param blob Blob.
 *
 * \return true if blob is a clone of a blob.
 */
/*
 * [한국어]
 * spdk_blob_is_clone - 이 blob이 (다른 blob 스냅샷의) 클론인지.
 *
 * @blob: 대상.
 * @return: true = 부모가 또 다른 blob; false = 일반 blob 또는 ESnap clone.
 *
 * ESnap clone은 별도 — spdk_blob_is_esnap_clone 사용. 두 함수가 동시에 false면 일반 blob.
 */
bool spdk_blob_is_clone(struct spdk_blob *blob);

/**
 * Check if blob is thin-provisioned.
 *
 * \param blob Blob.
 *
 * \return true if blob is thin-provisioned.
 */
/*
 * [한국어]
 * spdk_blob_is_thin_provisioned - thin-prov 여부 조회.
 *
 * @blob: 대상.
 * @return: true = thin (cluster lazy 할당), false = thick.
 *
 * 사용처: 사용량 모니터링, 인플레이션 결정.
 */
bool spdk_blob_is_thin_provisioned(struct spdk_blob *blob);

/**
 * Check if blob is a clone of an external snapshot.
 *
 * \param blob Blob.
 *
 * \return true if blob is a clone of an external bdev.
 */
/*
 * [한국어]
 * spdk_blob_is_esnap_clone - ESnap(외부 base) clone 여부.
 *
 * @blob: 대상 (const).
 * @return: true = base가 외부 bs_dev (다른 bdev/파일 등); false = 일반 blob/일반 clone.
 *
 * ESnap clone은 spdk_bs_open_blob 시 esnap_bs_dev_create 콜백이 base를 만들어준다.
 */
bool spdk_blob_is_esnap_clone(const struct spdk_blob *blob);

/**
 * Delete an existing blob from the given blobstore.
 *
 * \param bs blobstore.
 * \param blobid The id of the blob to delete.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_delete_blob - blob 영구 삭제.
 *
 * @bs: 대상.
 * @blobid: 삭제할 blob ID.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작:
 *  1) blob의 모든 cluster를 free (clear_method에 따라 unmap/write_zeroes/none).
 *  2) 메타페이지 슬롯 해제 (used_md_pages bitmap clear).
 *  3) super block 또는 메타 영속화.
 *  4) cb_fn 호출.
 *
 * 사전조건: blob이 close 상태여야 하고, 다른 blob의 부모(스냅샷)이면 안 됨 → -EBUSY.
 * 실행 컨텍스트: metadata thread.
 */
void spdk_bs_delete_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
			 spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Allocate all clusters in this blob. Data for allocated clusters is copied
 * from backing blob(s) if they exist.
 *
 * This call removes all dependencies on any backing blobs.
 *
 * \param bs blobstore.
 * \param channel IO channel used to inflate blob.
 * \param blobid The id of the blob to inflate.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_inflate_blob - thin/clone blob을 thick 변환 — 모든 cluster를 채우고 의존성 제거.
 *
 * @bs: 대상.
 * @channel: I/O 작업용 채널 (호출 thread 소유).
 * @blobid: inflate 대상 blob ID.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작:
 *  1) blob의 모든 cluster를 순회.
 *  2) 미할당 cluster는 부모(snapshot/ESnap)에서 read 후 자기에게 write (allocation 동반).
 *  3) 부모 의존성 제거 → parent_blob = INVALID.
 *  4) cb_fn 호출.
 *
 * 사용처: 스냅샷에서 분리 독립 blob을 만들고 싶을 때 (예: VM 템플릿에서 영구 사본 생성).
 * 비용: cluster 수 × cluster_sz 만큼의 read+write — 매우 비쌀 수 있음.
 * 실행 컨텍스트: I/O channel thread + metadata thread (혼합) — lib/blob 내부에서 조율.
 */
void spdk_bs_inflate_blob(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
			  spdk_blob_id blobid, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Remove dependency on parent blob.
 *
 * This call allocates and copies data for any clusters that are allocated in
 * the parent blob, and decouples parent updating dependencies of blob to
 * its ancestor.
 *
 * If blob have no parent -EINVAL error is reported.
 *
 * \param bs blobstore.
 * \param channel IO channel used to inflate blob.
 * \param blobid The id of the blob.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_blob_decouple_parent - 부모 한 단계만 분리 (조부모는 유지).
 *
 * @bs: 대상.
 * @channel: I/O 채널.
 * @blobid: 대상 blob ID.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * inflate vs decouple_parent: inflate는 모든 조상 의존성 제거(완전 thick), decouple은
 * 직속 부모에서만 분리하고 그 부모의 부모(조부모)가 새 부모가 됨. 부모 스냅샷을 삭제하고
 * 싶을 때 후손들을 조부모로 reroute하는 데 사용.
 *
 * 에러: blob이 clone이 아니면 -EINVAL.
 * 실행 컨텍스트: inflate와 동일.
 */
void spdk_bs_blob_decouple_parent(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
				  spdk_blob_id blobid, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Perform a shallow copy of a blob to a blobstore device.
 *
 * This makes a shallow copy from a blob to a blobstore device.
 * Only clusters allocated to the blob will be written on the device.
 * Blob must be read only and blob size must be less or equal than device size.
 * Blobstore block size must be a multiple of device block size.

 * \param bs Blobstore
 * \param channel IO channel used to copy the blob.
 * \param blobid The id of the blob.
 * \param ext_dev The device to copy on
 * \param status_cb_fn Called repeatedly during operation with status updates
 * \param status_cb_arg Argument passed to function status_cb_fn.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 *
 * \return 0 if operation starts correctly, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_bs_blob_shallow_copy - blob의 "할당된 cluster만" 외부 bs_dev에 복사.
 *
 * @bs: 대상.
 * @channel: I/O 채널.
 * @blobid: 소스 blob ID (반드시 read-only).
 * @ext_dev: 복사 대상 외부 bs_dev (사용자 제공, 크기 ≥ blob 크기).
 * @status_cb_fn: 진행 상황 콜백 (cluster 단위 누적값) — NULL 가능.
 * @status_cb_arg: status 콜백 컨텍스트.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 완료 컨텍스트.
 * @return: 0 = 시작 성공, 음수 errno = 시작 실패 (사전조건 위반 등).
 *
 * 동작:
 *  1) blob의 cluster bitmap을 순회.
 *  2) 각 할당된 cluster의 데이터를 read 후 ext_dev에 같은 offset으로 write.
 *  3) 미할당 cluster는 skip (데이터 없음).
 *  4) cluster마다 status_cb_fn 호출.
 *
 * 용도: 스냅샷을 외부 백업 디바이스로 추출, ESnap 마이그레이션.
 * 사전조건: blob read-only, blocklen 정합 (BS blocklen이 ext_dev blocklen의 배수).
 * 실행 컨텍스트: channel thread.
 */
int spdk_bs_blob_shallow_copy(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
			      spdk_blob_id blobid, struct spdk_bs_dev *ext_dev,
			      spdk_blob_shallow_copy_status status_cb_fn, void *status_cb_arg,
			      spdk_blob_op_complete cb_fn, void *cb_arg);


/**
 * Set a snapshot as the parent of a blob
 *
 * This call set a snapshot as the parent of a blob, making the blob a clone of this snapshot.
 * The previous parent of the blob, if any, can be another snapshot or an external snapshot; if
 * the blob is not a clone, it must be thin-provisioned.
 * Blob and parent snapshot must have the same size.
 *
 * \param bs blobstore.
 * \param blob_id The id of the blob.
 * \param snapshot_id The id of the parent snapshot.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_blob_set_parent - blob의 부모를 (다른) 스냅샷으로 변경/설정.
 *
 * @bs: 대상.
 * @blob_id: 자식 blob ID (thin이거나 이미 clone).
 * @snapshot_id: 새 부모 스냅샷 ID (반드시 read-only, 크기 동일).
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작: 기존 parent_blob을 새 snapshot_id로 교체. 자식 blob의 미할당 cluster read는
 * 이제부터 새 부모로 fallthrough.
 * 사전조건: blob과 부모 크기 동일, blob이 thin이거나 이미 clone.
 * 사용처: re-parenting (snapshot 트리 재구성), backup 복원.
 */
void spdk_bs_blob_set_parent(struct spdk_blob_store *bs, spdk_blob_id blob_id,
			     spdk_blob_id snapshot_id, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Set an external snapshot as the parent of a blob
 *
 * This call set an external snapshot as the parent of a blob, making the blob a clone of this
 * external snapshot.
 * The previous parent of the blob, if any, can be another external snapshot or a snapshot; if
 * the blob is not a clone, it must be thin-provisioned.
 *
 * \param bs blobstore.
 * \param blob_id The id of the blob.
 * \param esnap_bs_dev The new blobstore device to use as an external snapshot.
 * \param esnap_id The identifier of the external snapshot.
 * \param esnap_id_len The length of esnap_id, in bytes.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_blob_set_external_parent - blob의 부모를 외부 ESnap 디바이스로 설정.
 *
 * @bs: 대상.
 * @blob_id: 자식 blob ID.
 * @esnap_bs_dev: 새 외부 base bs_dev (사용자 제공).
 * @esnap_id: 외부 base 식별자(메타에 영속화) — lib/blob이 내부 복사.
 * @esnap_id_len: esnap_id 바이트 길이.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작: blob을 ESnap clone으로 전환 또는 다른 ESnap으로 교체. blob 메타에 esnap_id를
 * 기록해 다음 load 시 esnap_bs_dev_create 콜백으로 자동 base 매핑되도록 한다.
 *
 * 활용 시나리오: 외부 backup 이미지로부터 thin clone 생성 (마이그레이션).
 */
void spdk_bs_blob_set_external_parent(struct spdk_blob_store *bs, spdk_blob_id blob_id,
				      struct spdk_bs_dev *esnap_bs_dev, const void *esnap_id,
				      uint32_t esnap_id_len, spdk_blob_op_complete cb_fn, void *cb_arg);


/*
 * [한국어]
 * spdk_blob_open_opts - 기존 blob을 open할 때의 옵션.
 *
 * spdk_bs_open_blob_ext에 전달.
 */
struct spdk_blob_open_opts {
	enum blob_clear_method  clear_method;
	/* [한국어] open된 blob에 대한 cluster 정리 방식 override.
	 * 설정자: 사용자.
	 * 읽는 자: lib/blob의 해제 경로.
	 * 값 의미: enum blob_clear_method 참조. */

	/**
	 * The size of spdk_blob_open_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
	/* [한국어] caller 컴파일 시점 sizeof — ABI 호환. spdk_blob_open_opts_init이 자동 채움. */

	/**
	 * Blob context to be passed to any call of bs->external_bs_dev_create() that is triggered
	 * by this open call.
	 */
	void *esnap_ctx;
	/* [한국어] 이 open 호출이 ESnap base 생성을 트리거할 때 esnap_bs_dev_create의 blob_ctx로 전달.
	 *  설정자: 사용자(ESnap clone일 때만 의미 있음).
	 *  읽는 자: esnap_bs_dev_create 콜백 내부.
	 *  bs_ctx와 분리된 이유: open마다 다른 컨텍스트(다른 호출자, 다른 lvol 인스턴스)를 줄 수 있도록. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_blob_open_opts) == 24, "Incorrect size");
/* [한국어] ABI sizeof 어서션 — 24바이트 고정. */

/**
 * Initialize a spdk_blob_open_opts structure to the default blob option values.
 *
 * \param opts spdk_blob_open_opts structure to initialize.
 * \param opts_size It mus be the size of struct spdk_blob_open_opts.
 */
/*
 * [한국어]
 * spdk_blob_open_opts_init - spdk_blob_open_opts를 라이브러리 기본값으로 초기화.
 *
 * @opts: 대상.
 * @opts_size: caller sizeof.
 *
 * 기본: clear_method=DEFAULT, esnap_ctx=NULL.
 * 호출 컨텍스트: 임의 thread.
 */
void spdk_blob_open_opts_init(struct spdk_blob_open_opts *opts, size_t opts_size);

/**
 * Open a blob from the given blobstore.
 *
 * \param bs blobstore.
 * \param blobid The id of the blob to open.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_open_blob - 기본 옵션으로 blob open (편의 함수).
 *
 * @bs: 대상.
 * @blobid: open할 blob ID.
 * @cb_fn: 완료 콜백 (blob 핸들 반환).
 * @cb_arg: 컨텍스트.
 *
 * 동작: 메타페이지 read → blob 구조체 in-memory 복원 → cb_fn에 핸들 전달.
 * ESnap clone이면 esnap_bs_dev_create 자동 호출.
 * 다중 open: 같은 blob을 여러 번 open 가능 — 마지막 close 때 sync.
 * 실행 컨텍스트: metadata thread.
 */
void spdk_bs_open_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		       spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Open a blob from the given blobstore with additional options.
 *
 * \param bs blobstore.
 * \param blobid The id of the blob to open.
 * \param opts The structure which contains the option values for the blob.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_open_blob_ext - 옵션과 함께 blob open.
 *
 * @bs: 대상.
 * @blobid: open할 blob ID.
 * @opts: open 옵션 (esnap_ctx 등).
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * spdk_bs_open_blob의 extended 버전 — ESnap clone open 시 esnap_ctx 전달이 가능해서
 * 같은 blobstore에서도 open마다 다른 컨텍스트로 base 디바이스를 만들 수 있다.
 */
void spdk_bs_open_blob_ext(struct spdk_blob_store *bs, spdk_blob_id blobid,
			   struct spdk_blob_open_opts *opts, spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Resize a blob to 'sz' clusters. These changes are not persisted to disk until
 * spdk_bs_md_sync_blob() is called.
 * If called before previous resize finish, it will fail with errno -EBUSY
 *
 * \param blob Blob to resize.
 * \param sz The new number of clusters.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 *
 */
/*
 * [한국어]
 * spdk_blob_resize - blob의 크기를 sz cluster로 변경 (확장 또는 축소).
 *
 * @blob: 대상 (반드시 open 상태).
 * @sz: 새 cluster 수.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작:
 *  - 확장: cluster bitmap에서 추가 cluster 할당(thick) 또는 num_clusters만 갱신(thin).
 *  - 축소: 잘려나가는 cluster를 clear_method에 따라 정리 후 free.
 *
 * 영속화: 이 함수는 in-memory만 변경 — spdk_blob_sync_md를 별도 호출해야 디스크에 반영.
 * 동시 resize 금지: 진행 중 두 번째 resize 호출 시 -EBUSY.
 * 에러: -ENOSPC(thick prov, cluster 부족), -EBUSY.
 * 실행 컨텍스트: metadata thread.
 */
void spdk_blob_resize(struct spdk_blob *blob, uint64_t sz, spdk_blob_op_complete cb_fn,
		      void *cb_arg);

/**
 * Set blob as read only.
 *
 * These changes do not take effect until spdk_blob_sync_md() is called.
 *
 * \param blob Blob to set.
 */
/*
 * [한국어]
 * spdk_blob_set_read_only - blob을 read-only로 표시 (동기 in-memory만).
 *
 * @blob: 대상.
 * @return: 0 = 성공, 음수 errno = 실패 (현재 구현에선 항상 0).
 *
 * 영속화: spdk_blob_sync_md 호출 시점에 디스크에 반영.
 * 사용처: snapshot 자동 변환 또는 사용자가 명시적 보호.
 * 실행 컨텍스트: metadata thread.
 */
int spdk_blob_set_read_only(struct spdk_blob *blob);

/**
 * Sync a blob.
 *
 * Make a blob persistent. This applies to open, resize, set xattr, and remove
 * xattr. These operations will not be persistent until the blob has been synced.
 *
 * \param blob Blob to sync.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_blob_sync_md - blob의 dirty 메타데이터를 디스크에 영속화.
 *
 * @blob: 대상.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 무엇이 dirty가 되는가: resize, set/remove_xattr, set_read_only 등은 모두 in-memory만
 * 변경. 이 함수가 호출되어야 메타페이지가 백킹 디바이스에 write되고, 백킹 flush까지
 * 마쳐 영속화된다. 같은 blob에 대해 동시 sync는 lib/blob 내부에서 직렬화.
 *
 * 호출하지 않으면? 비정상 종료 시 마지막 sync 시점으로 롤백됨.
 * 실행 컨텍스트: metadata thread.
 */
void spdk_blob_sync_md(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Close a blob. This will automatically sync.
 *
 * \param blob Blob to close.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_blob_close - blob 핸들 닫기 (sync 자동 수행).
 *
 * @blob: 대상.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작:
 *  1) 마지막 reference면 dirty 메타 sync.
 *  2) in-memory blob 구조 해제.
 *  3) cb_fn 호출 — 이 시점부터 blob 포인터 무효.
 *
 * 다중 open된 blob은 ref count 감소만 — 마지막 close에서 sync.
 * 실행 컨텍스트: metadata thread.
 */
void spdk_blob_close(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Allocate an I/O channel for the given blobstore.
 *
 * \param bs blobstore.
 * \return a pointer to the allocated I/O channel.
 */
/*
 * [한국어]
 * spdk_bs_alloc_io_channel - blobstore용 I/O channel 할당 (스레드 로컬 자원).
 *
 * @bs: 대상.
 * @return: 새 채널 포인터 또는 NULL (메모리 부족).
 *
 * 동작:
 *  1) SPDK 공통 io_channel 메커니즘으로 channel 할당.
 *  2) 백킹 bs_dev->create_channel 호출 → 두 채널을 연결.
 *  3) channel 내부 request pool, completion polling 설정.
 *
 * 스레드 어피니티: 호출 thread에 바인딩된다 — 그 thread에서만 사용해야 한다.
 * 다른 thread로 넘기려면 spdk_thread_send_msg로 메시지 보내야 함.
 * 실행 컨텍스트: 임의 thread (반드시 SPDK thread).
 */
struct spdk_io_channel *spdk_bs_alloc_io_channel(struct spdk_blob_store *bs);

/**
 * Free the I/O channel.
 *
 * \param channel I/O channel to free.
 */
/*
 * [한국어]
 * spdk_bs_free_io_channel - I/O channel 해제.
 *
 * @channel: 대상.
 *
 * 동작: bs_dev->destroy_channel 호출 → channel 메모리 해제.
 * 사전조건: 이 채널에서 진행 중인 I/O가 없어야 함 (호출자 책임).
 * 실행 컨텍스트: 채널을 만든 thread에서만.
 */
void spdk_bs_free_io_channel(struct spdk_io_channel *channel);

/**
 * Write data to a blob.
 *
 * \param blob Blob to write.
 * \param channel The I/O channel used to submit requests.
 * \param payload The specified buffer which should contain the data to be written.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_blob_io_write - blob에 비동기 단일 버퍼 write.
 *
 * @blob: 대상 (open, !read_only).
 * @channel: 호출 thread의 I/O channel.
 * @payload: 사용자 데이터 버퍼 (DMA-able 메모리, spdk_dma_malloc 권장).
 * @offset: blob 시작부터 io_unit 단위 offset.
 * @length: 길이 (io_unit).
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작 단계:
 *  1) blob의 cluster map을 보고 (offset → cluster) 매핑.
 *  2) thin이고 미할당 cluster면 cluster 새로 할당 (CoW from parent).
 *  3) 백킹 bs_dev->write 호출.
 *  4) 완료 시 cb_fn(cb_arg, errno) 호출 — 같은 channel thread에서.
 *
 * 메모리: payload는 cb_fn 호출까지 유효해야 함 (DMA 진행 중).
 * 실행 컨텍스트: channel을 만든 thread (cross-thread 호출 금지).
 */
void spdk_blob_io_write(struct spdk_blob *blob, struct spdk_io_channel *channel,
			void *payload, uint64_t offset, uint64_t length,
			spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Read data from a blob.
 *
 * \param blob Blob to read.
 * \param channel The I/O channel used to submit requests.
 * \param payload The specified buffer which will store the obtained data.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_blob_io_read - blob에서 비동기 단일 버퍼 read.
 *
 * @blob: 대상.
 * @channel: 호출 thread의 채널.
 * @payload: 결과를 저장할 버퍼 (DMA-able).
 * @offset: io_unit offset.
 * @length: io_unit 길이.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작:
 *  1) cluster map 조회.
 *  2) 할당된 cluster면 백킹 bs_dev->read.
 *  3) 미할당이면 부모(snapshot/ESnap)로 fallthrough → 재귀 read.
 *  4) 부모도 없으면 0으로 채움 (zeroes_dev).
 *  5) cb_fn 호출.
 *
 * 메모리: payload 유효성 cb_fn 호출까지 보장.
 * 실행 컨텍스트: channel thread.
 */
void spdk_blob_io_read(struct spdk_blob *blob, struct spdk_io_channel *channel,
		       void *payload, uint64_t offset, uint64_t length,
		       spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Write the data described by 'iov' to 'length' io_units beginning at 'offset' io_units
 * into the blob.
 *
 * \param blob Blob to write.
 * \param channel I/O channel used to submit requests.
 * \param iov The pointer points to an array of iovec structures.
 * \param iovcnt The number of buffers.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_blob_io_writev - blob에 scatter-gather 비동기 write (iovec 배열).
 *
 * @blob: 대상.
 * @channel: I/O 채널.
 * @iov: iovec 배열 (각 (iov_base, iov_len)).
 * @iovcnt: iovec 개수.
 * @offset: io_unit offset.
 * @length: io_unit 길이 (∑iov_len과 일치해야 함).
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * NVMe SGL을 그대로 활용 가능 — 분산된 사용자 버퍼를 zero-copy로 SSD에 전송.
 * 메모리: iov 배열 자체와 각 base 모두 cb_fn까지 유효 필요.
 * 실행 컨텍스트: channel thread.
 */
void spdk_blob_io_writev(struct spdk_blob *blob, struct spdk_io_channel *channel,
			 struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			 spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Read 'length' io_units starting at 'offset' io_units into the blob into the memory
 * described by 'iov'.
 *
 * \param blob Blob to read.
 * \param channel I/O channel used to submit requests.
 * \param iov The pointer points to an array of iovec structures.
 * \param iovcnt The number of buffers.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_blob_io_readv - blob에서 scatter-gather 비동기 read.
 *
 * @blob: 대상.
 * @channel: 채널.
 * @iov: 결과를 채울 iovec 배열.
 * @iovcnt: 개수.
 * @offset/length: io_unit 단위.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * writev의 read 버전 — fallthrough 동작은 spdk_blob_io_read와 동일.
 */
void spdk_blob_io_readv(struct spdk_blob *blob, struct spdk_io_channel *channel,
			struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Write the data described by 'iov' to 'length' io_units beginning at 'offset' io_units
 * into the blob. Accepts extended IO request options
 *
 * \param blob Blob to write.
 * \param channel I/O channel used to submit requests.
 * \param iov The pointer points to an array of iovec structures.
 * \param iovcnt The number of buffers.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 * \param io_opts Optional extended IO request options
 */
/*
 * [한국어]
 * spdk_blob_io_writev_ext - writev + memory domain 등 확장 옵션.
 *
 * @blob, @channel, @iov, @iovcnt, @offset, @length, @cb_fn, @cb_arg: spdk_blob_io_writev와 동일.
 * @io_opts: 확장 옵션 (memory_domain 등) — NULL 가능.
 *
 * memory_domain 사용 시 호스트가 아닌 RDMA NIC/GPU 메모리에 있는 페이로드를 zero-copy 처리.
 * io_opts는 cb_fn까지 유효해야 함.
 */
void spdk_blob_io_writev_ext(struct spdk_blob *blob, struct spdk_io_channel *channel,
			     struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			     spdk_blob_op_complete cb_fn, void *cb_arg,
			     struct spdk_blob_ext_io_opts *io_opts);

/**
 * Read 'length' io_units starting at 'offset' io_units into the blob into the memory
 * described by 'iov'. Accepts extended IO request options
 *
 * \param blob Blob to read.
 * \param channel I/O channel used to submit requests.
 * \param iov The pointer points to an array of iovec structures.
 * \param iovcnt The number of buffers.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 * \param io_opts Optional extended IO request options
 */
/*
 * [한국어]
 * spdk_blob_io_readv_ext - readv + 확장 옵션 (memory domain 등).
 *
 * 시그니처는 writev_ext의 read 버전. memory domain 활용은 동일.
 */
void spdk_blob_io_readv_ext(struct spdk_blob *blob, struct spdk_io_channel *channel,
			    struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			    spdk_blob_op_complete cb_fn, void *cb_arg,
			    struct spdk_blob_ext_io_opts *io_opts);

/**
 * Unmap 'length' io_units beginning at 'offset' io_units on the blob as unused. Unmapped
 * io_units may allow the underlying storage media to behave more efficiently.
 *
 * \param blob Blob to unmap.
 * \param channel I/O channel used to submit requests.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of unmap area in io_units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_blob_io_unmap - blob의 [offset, offset+length) 범위를 unused로 표시.
 *
 * @blob: 대상.
 * @channel: 채널.
 * @offset/length: io_unit.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작: cluster 단위로 정렬되면 cluster 자체를 free (thin-prov로 회귀); 정렬 안 되면
 * 백킹 디바이스에 unmap/write_zeroes 명령. SSD GC 효율 ↑.
 * 사용처: TRIM 패스스루, 사용자 명시적 sparse 처리.
 * 실행 컨텍스트: channel thread.
 */
void spdk_blob_io_unmap(struct spdk_blob *blob, struct spdk_io_channel *channel,
			uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Write zeros into area of a blob.
 *
 * \param blob Blob to write.
 * \param channel I/O channel used to submit requests.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_blob_io_write_zeroes - blob의 [offset, offset+length) 영역에 0 기록.
 *
 * @blob: 대상.
 * @channel: 채널.
 * @offset/length: io_unit.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작: 백킹 bs_dev->write_zeroes 호출 (NVMe Write Zeroes 또는 SW emulation).
 * thin-prov 면 새 cluster 할당 후 zero (또는 unmap-and-zero).
 * 사용처: 디스크 셔드(파괴) 후 빠른 0 채움.
 */
void spdk_blob_io_write_zeroes(struct spdk_blob *blob, struct spdk_io_channel *channel,
			       uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Get the first blob of the blobstore. The obtained blob will be passed to
 * the callback function.
 *
 * The user's cb_fn will be called with rc == -ENOENT when the iteration is
 * complete.
 *
 * When the user's cb_fn is called with rc == 0, the associated blob is open.
 * This means that the cb_fn may not attempt to unload the blobstore.  It
 * must complete the iteration before attempting to unload.
 *
 * \param bs blobstore to traverse.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_iter_first - blobstore의 첫 blob을 open한 상태로 cb_fn에 전달 (이터레이션 시작).
 *
 * @bs: 대상.
 * @cb_fn: 완료 콜백 (성공 시 blob 핸들 + 0; 빈 BS면 NULL + -ENOENT).
 * @cb_arg: 컨텍스트.
 *
 * 동작: used_md_pages bitmap을 스캔해 첫 유효 메타페이지의 blob을 spdk_bs_open_blob로 오픈.
 * 사용 패턴: cb_fn 안에서 처리 후 spdk_bs_iter_next로 chain. iter 동안 unload 금지.
 * 실행 컨텍스트: metadata thread.
 */
void spdk_bs_iter_first(struct spdk_blob_store *bs,
			spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Get the next blob by using the current blob. The obtained blob will be passed
 * to the callback function.
 *
 * The user's cb_fn will be called with rc == -ENOENT when the iteration is
 * complete.
 *
 * When the user's cb_fn is called with rc == 0, the associated blob is open.
 * This means that the cb_fn may not attempt to unload the blobstore.  It
 * must complete the iteration before attempting to unload.
 *
 * \param bs blobstore to traverse.
 * \param blob The current blob.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_bs_iter_next - 현재 blob 다음의 blob을 open해 cb_fn에 전달.
 *
 * @bs: 대상.
 * @blob: 현재 blob 핸들 (이 함수가 close하고 다음으로 진행).
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작:
 *  1) 현재 blob close.
 *  2) used_md_pages 비트맵에서 다음 유효 슬롯 찾기.
 *  3) 다음 blob open.
 *  4) cb_fn 호출 (없으면 -ENOENT).
 *
 * 사용처: load 시 iter_cb_fn으로 컨슈머가 모든 blob 등록할 때, 또는 dump 도구.
 */
void spdk_bs_iter_next(struct spdk_blob_store *bs, struct spdk_blob *blob,
		       spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Set an extended attribute for the given blob.
 *
 * \param blob Blob to set attribute.
 * \param name Name of the extended attribute.
 * \param value Value of the extended attribute.
 * \param value_len Length of the value.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_blob_set_xattr - blob에 extended attribute 추가/변경 (동기 in-memory만).
 *
 * @blob: 대상.
 * @name: xattr 이름 (NUL-terminated). 이름이 '.'으로 시작하면 internal xattr (스냅샷
 *        부모 ID 같이 lib/blob 내부 용도) — 사용자 정의는 일반 이름 사용 권장.
 * @value: 값 (불투명 바이트열).
 * @value_len: 값 길이 (uint16_t 한계 → 65535바이트 이하).
 * @return: 0 = 성공, -1 = 실패.
 *
 * 영속화: spdk_blob_sync_md 호출 시 디스크 반영. close에서도 자동 sync.
 * internal vs external: '.' prefix는 lib/blob 코어가 예약. 일반 사용자는 회피.
 * 실행 컨텍스트: metadata thread.
 */
int spdk_blob_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
			uint16_t value_len);

/**
 * Remove the extended attribute from the given blob.
 *
 * \param blob Blob to remove attribute.
 * \param name Name of the extended attribute.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_blob_remove_xattr - blob에서 xattr 제거 (동기 in-memory).
 *
 * @blob: 대상.
 * @name: 제거할 xattr 이름.
 * @return: 0 = 성공, 음수 errno = 실패 (-ENOENT면 이름 없음).
 *
 * 영속화: spdk_blob_sync_md 후. close 자동 sync.
 */
int spdk_blob_remove_xattr(struct spdk_blob *blob, const char *name);

/**
 * Get the value of the specified extended attribute. The obtained value and its
 * size will be stored in value and value_len.
 *
 * \param blob Blob to query.
 * \param name Name of the extended attribute.
 * \param value Parameter as output.
 * \param value_len Parameter as output.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_blob_get_xattr_value - 이름으로 xattr 값 조회 (동기).
 *
 * @blob: 대상.
 * @name: xattr 이름.
 * @value: [out] 값 포인터 (blob 내부 메모리, free 금지, blob open 동안 유효).
 * @value_len: [out] 값 길이.
 * @return: 0 = 성공, -ENOENT = 이름 없음.
 *
 * 메모리 수명: blob close될 때까지만 유효. 더 오래 보존하려면 호출자가 복사.
 */
int spdk_blob_get_xattr_value(struct spdk_blob *blob, const char *name,
			      const void **value, size_t *value_len);

/**
 * Iterate through all extended attributes of the blob. Get the names of all extended
 * attributes that will be stored in names.
 *
 * \param blob Blob to query.
 * \param names Parameter as output.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_blob_get_xattr_names - blob의 모든 xattr 이름 목록 동기 조회.
 *
 * @blob: 대상.
 * @names: [out] spdk_xattr_names 포인터 (호출자가 spdk_xattr_names_free로 해제 필요).
 * @return: 0 = 성공, 음수 errno = 실패.
 *
 * 사용 패턴:
 *   struct spdk_xattr_names *names;
 *   spdk_blob_get_xattr_names(blob, &names);
 *   for (i=0; i<spdk_xattr_names_get_count(names); i++) {
 *       const char *name = spdk_xattr_names_get_name(names, i);
 *       spdk_blob_get_xattr_value(blob, name, ...);
 *   }
 *   spdk_xattr_names_free(names);
 *
 * 메모리: names는 호출자 소유 — 반드시 free.
 */
int spdk_blob_get_xattr_names(struct spdk_blob *blob, struct spdk_xattr_names **names);

/**
 * Get the number of extended attributes.
 *
 * \param names Names of total extended attributes of the blob.
 *
 * \return the number of extended attributes.
 */
/*
 * [한국어]
 * spdk_xattr_names_get_count - xattr 이름 목록의 개수.
 *
 * @names: spdk_blob_get_xattr_names가 반환한 핸들.
 * @return: xattr 개수.
 *
 * 단순 getter. 호출 컨텍스트: 임의 thread.
 */
uint32_t spdk_xattr_names_get_count(struct spdk_xattr_names *names);

/**
 * Get the attribute name specified by the index.
 *
 * \param names Names of total extended attributes of the blob.
 * \param index Index position of the specified attribute.
 *
 * \return attribute name.
 */
/*
 * [한국어]
 * spdk_xattr_names_get_name - 인덱스에 해당하는 xattr 이름 반환.
 *
 * @names: 핸들.
 * @index: 0..count-1 범위.
 * @return: NUL-terminated 이름 (names가 살아 있는 동안 유효, free 금지).
 *
 * index ≥ count 시 동작은 정의되지 않음 — 호출자가 가드.
 */
const char *spdk_xattr_names_get_name(struct spdk_xattr_names *names, uint32_t index);

/**
 * Free the attribute names.
 *
 * \param names Names of total extended attributes of the blob.
 */
/*
 * [한국어]
 * spdk_xattr_names_free - spdk_blob_get_xattr_names로 받은 핸들 해제.
 *
 * @names: 해제할 핸들. NULL 허용 (no-op).
 *
 * 호출 컨텍스트: 임의 thread.
 */
void spdk_xattr_names_free(struct spdk_xattr_names *names);

/**
 * Get blobstore type of the given device.
 *
 * \param bs blobstore to query.
 *
 * \return blobstore type.
 */
/*
 * [한국어]
 * spdk_bs_get_bstype - blobstore의 bstype 라벨 조회 (동기).
 *
 * @bs: 대상.
 * @return: spdk_bs_type 값 (16바이트, 사본).
 *
 * 컨슈머가 자기 시그니처 검증에 사용 (예: lvol이 "LVOLSTORE"인지 확인).
 */
struct spdk_bs_type spdk_bs_get_bstype(struct spdk_blob_store *bs);

/**
 * Set blobstore type to the given device.
 *
 * \param bs blobstore to set to.
 * \param bstype Type label to set.
 */
/*
 * [한국어]
 * spdk_bs_set_bstype - blobstore의 bstype 라벨 설정 (in-memory; 다음 sync 시 영속화).
 *
 * @bs: 대상.
 * @bstype: 새 라벨 (16바이트).
 *
 * 사용처: 컨슈머가 init 후 명시적으로 자기 시그니처를 박아둘 때.
 * 영속화: super block은 다음 메타 sync 시 갱신.
 * 실행 컨텍스트: metadata thread.
 */
void spdk_bs_set_bstype(struct spdk_blob_store *bs, struct spdk_bs_type bstype);

/**
 * Replace the existing external snapshot device.
 *
 * \param blob The blob that is getting a new external snapshot device.
 * \param back_bs_dev The new blobstore device to use as an external snapshot.
 * \param cb_fn Callback to be called when complete.
 * \param cb_arg Callback argument used with cb_fn.
 */
/*
 * [한국어]
 * spdk_blob_set_esnap_bs_dev - ESnap clone의 base bs_dev를 다른 디바이스로 교체.
 *
 * @blob: 대상 (반드시 ESnap clone).
 * @back_bs_dev: 새 외부 base bs_dev.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 컨텍스트.
 *
 * 동작: 기존 base의 destroy 호출 → 새 base 연결. esnap_id는 변하지 않음 — 같은 id를
 * 다른 백엔드로 매핑하는 시나리오 (예: 백업본 교체, base 디바이스 마이그레이션).
 *
 * 실행 컨텍스트: metadata thread.
 */
void spdk_blob_set_esnap_bs_dev(struct spdk_blob *blob, struct spdk_bs_dev *back_bs_dev,
				spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Get the existing external snapshot device
 *
 * \param blob A blob that is an esnap clone
 *
 * \return NULL if the blob is not an esnap clone, else the current external snapshot device.
 */
/*
 * [한국어]
 * spdk_blob_get_esnap_bs_dev - 현재 ESnap base bs_dev 반환.
 *
 * @blob: 대상 (const).
 * @return: ESnap clone이면 base bs_dev 포인터, 아니면 NULL.
 *
 * 메모리: 반환된 포인터는 lib/blob 소유 — 호출자가 destroy 금지.
 * 사용처: 컨슈머가 base 식별자/통계 조회.
 */
struct spdk_bs_dev *spdk_blob_get_esnap_bs_dev(const struct spdk_blob *blob);

/**
 * Determine if the blob is degraded. A degraded blob cannot perform IO.
 *
 * \param blob A blob
 *
 * \return true if the blob or any snapshots upon which it depends are degraded, else false.
 */
/*
 * [한국어]
 * spdk_blob_is_degraded - blob 또는 그 의존 체인이 degraded 인지 판정.
 *
 * @blob: 대상 (const).
 * @return: true = blob 자체 또는 snapshot 체인의 어느 한 단계라도 degraded.
 *
 * 동작: bs_dev->is_degraded를 체인 따라 호출하며 OR. ESnap base 디바이스가 사라지거나
 * 부분 결함 시 true. true면 사용자는 I/O를 시작하지 말아야 함 (호출 시 -EIO).
 * 사용처: lvol UI 상태 표시, 헬스체크.
 */
bool spdk_blob_is_degraded(const struct spdk_blob *blob);

/* [한국어] C++ extern "C" 블록 닫기. */
#ifdef __cplusplus
}
#endif

/* [한국어] 헤더 가드 끝. */
#endif /* SPDK_BLOB_H_ */
