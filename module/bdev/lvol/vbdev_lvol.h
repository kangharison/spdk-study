/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] LVOL(Logical Volume) bdev 모듈 공개 API 헤더 (vbdev_lvol.h)
 *
 * === 파일의 역할 ===
 * SPDK Blobstore(lib/blob) 위에 구축된 Logical Volume 매니저의 bdev 어댑터 공개 API.
 * Lvol(논리 볼륨)은 thin-provisioning, snapshot, clone, resize, read-only, external snapshot
 * (esnap), shallow copy 등 풍부한 기능을 가지는 SPDK 고수준 스토리지 추상화이다. 본 헤더는
 * "lvol을 spdk_bdev로 노출"하는 vbdev 레이어의 진입점을 선언한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [JSON-RPC] → [vbdev_lvol_rpc.c]
 *     → [이 헤더 — vbdev_lvs_create / vbdev_lvol_create / snapshot/clone/resize/destroy]
 *         → [vbdev_lvol.c]
 *             → [lib/lvol]
 *                 → [lib/blob (Blobstore)]
 *                     → [base bdev (NVMe / AIO / Malloc...)]
 *
 * lvol_store(lvs)는 base bdev 1개 위에 한 번 만들고, 그 위에 다수의 lvol을 생성 가능.
 * 각 lvol은 spdk_blob 1개와 1:1 매핑되며 vbdev로 노출된다.
 *
 * === 타 모듈과의 연결 ===
 * - vbdev_lvol.c : 구현부.
 * - lib/lvol/* : Logical Volume 코어 (lvol/lvolstore의 metadata, snapshot tree 관리).
 * - lib/blob/* : 백엔드 객체 저장소 (clusters, allocations).
 * - include/spdk/blob_bdev.h : bs_dev abstraction — 일반 bdev를 blobstore가 쓰는 형태로 wrap.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct lvol_store_bdev : (lvolstore, base bdev) 쌍을 모듈 글로벌 리스트에 보관.
 * - struct lvol_bdev : 단일 lvol을 spdk_bdev로 노출하는 어댑터.
 * - vbdev_lvs_create / vbdev_lvs_unload / vbdev_lvs_destruct : lvolstore 라이프사이클.
 * - vbdev_lvol_create / _snapshot / _clone / _bdev_clone / _resize / _destroy : lvol 라이프사이클.
 * - vbdev_lvol_set_read_only / set_external_parent / rename / shallow_copy : 고급 연산.
 * - vbdev_get_lvol_store_by_uuid/name : 조회 helper.
 */

#ifndef SPDK_VBDEV_LVOL_H
#define SPDK_VBDEV_LVOL_H

#include "spdk/lvol.h"
/* [한국어] Logical Volume 공개 API (spdk_lvol_store, spdk_lvol, opts, lvs_clear_method 등). */
#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈 작성 API. */
#include "spdk/blob_bdev.h"
/* [한국어] spdk_bs_dev — Blobstore가 base bdev 위에서 동작하기 위한 디바이스 어댑터. */

#include "spdk_internal/lvolstore.h"
/* [한국어] 내부 lvolstore 정의(spdk_lvol_store, spdk_lvol의 내부 필드 접근용). */

/*
 * [한국어] struct lvol_store_bdev
 *
 * lvolstore + base bdev + 진행 중 요청 묶음. 모듈 전역 리스트(g_spdk_lvol_pairs)에 등록되어
 * RPC가 lvolstore를 이름/UUID로 조회 가능하게 한다.
 */
struct lvol_store_bdev {
	struct spdk_lvol_store	*lvs;
	/* [한국어] lib/lvol이 관리하는 lvolstore 핸들. UUID/name/cluster_sz 등 메타데이터 보유.
	 * 설정자: vbdev_lvs_create의 콜백. 동기화: app thread에서만 갱신. */

	struct spdk_bdev	*bdev;
	/* [한국어] lvolstore가 올라가 있는 base bdev (NVMe/AIO/Malloc 등). */

	struct spdk_lvs_req	*req;
	/* [한국어] 진행 중인 lvolstore 작업의 요청 객체 — create/load/destroy 중 1건만 유효.
	 * 작업 완료 시 NULL로 회복. */

	bool			removal_in_progress;
	/* [한국어] base bdev hot-remove가 시작되면 true. 신규 lvol 작업 거절에 사용. */

	TAILQ_ENTRY(lvol_store_bdev)	lvol_stores;
	/* [한국어] g_spdk_lvol_pairs 글로벌 리스트 노드. */
};

/*
 * [한국어] struct lvol_bdev
 *
 * 단일 lvol을 spdk_bdev로 노출하기 위한 어댑터. spdk_bdev를 임베드(첫 번째 멤버)하고
 * lvol 핸들 + 소속 lvol_store_bdev 포인터를 추가로 가진다.
 */
struct lvol_bdev {
	struct spdk_bdev	bdev;
	/* [한국어] bdev core에 노출되는 메타데이터. read/write/unmap은 fn_table을 통해 본 모듈로 전달.
	 * 첫 번째 멤버이므로 lvol_bdev* ↔ spdk_bdev* 캐스팅 가능. */
	struct spdk_lvol	*lvol;
	/* [한국어] 백엔드 lvol 핸들. spdk_blob/메타데이터 접근. */
	struct lvol_store_bdev	*lvs_bdev;
	/* [한국어] 이 lvol이 속한 lvolstore 컨텍스트. lvolstore 단위 동작에 사용. */
};

/*
 * [한국어]
 * vbdev_lvs_create - base bdev 위에 lvolstore를 생성
 *
 * @base_bdev_name: lvolstore가 올라갈 base bdev 이름.
 * @name: lvolstore 이름.
 * @cluster_sz: cluster 크기(바이트). thin provisioning 단위.
 * @clear_method: 클러스터 할당 시 지움 방식(NONE/UNMAP/WRITE_ZEROES).
 * @num_md_pages_per_cluster_ratio: 메타데이터 페이지 비율.
 * @cb_fn / @cb_arg: 생성 완료 콜백.
 * @return: 0 성공, 음수 -errno.
 */
int vbdev_lvs_create(const char *base_bdev_name, const char *name, uint32_t cluster_sz,
		     enum lvs_clear_method clear_method, uint32_t num_md_pages_per_cluster_ratio,
		     spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg);

/*
 * [한국어]
 * vbdev_lvs_create_ext - vbdev_lvs_create 확장판 (md_page_size 추가 지정).
 * 그 외 인자는 위와 동일.
 */
int vbdev_lvs_create_ext(const char *base_bdev_name, const char *name, uint32_t cluster_sz,
			 enum lvs_clear_method clear_method, uint32_t num_md_pages_per_cluster_ratio,
			 uint32_t md_page_size, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg);

/*
 * [한국어]
 * vbdev_lvs_destruct - lvolstore와 모든 자식 lvol을 destroy (영구 삭제 — 메타도 지움).
 */
void vbdev_lvs_destruct(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg);

/*
 * [한국어]
 * vbdev_lvs_unload - 메타데이터는 보존하고 메모리 상의 lvolstore만 해제 (재로딩 가능).
 */
void vbdev_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg);

/*
 * [한국어]
 * vbdev_lvol_create - lvolstore 안에 새 lvol 생성
 *
 * @lvs: 부모 lvolstore.
 * @name: lvol 이름.
 * @sz: 크기(바이트). cluster 크기 배수로 round-up.
 * @thin_provisioned: true면 thin (할당된 cluster만 차지), false면 즉시 모두 할당.
 * @clear_method: 새 cluster 지움 방식.
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0 성공, 음수 -errno.
 */
int vbdev_lvol_create(struct spdk_lvol_store *lvs, const char *name, uint64_t sz,
		      bool thin_provisioned, enum lvol_clear_method clear_method,
		      spdk_lvol_op_with_handle_complete cb_fn,
		      void *cb_arg);

/*
 * [한국어]
 * vbdev_lvol_create_snapshot - 기존 lvol의 시점 snapshot 생성 (CoW base).
 */
void vbdev_lvol_create_snapshot(struct spdk_lvol *lvol, const char *snapshot_name,
				spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

/*
 * [한국어]
 * vbdev_lvol_create_clone - snapshot으로부터 새 writable clone 생성.
 */
void vbdev_lvol_create_clone(struct spdk_lvol *lvol, const char *clone_name,
			     spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

/*
 * [한국어]
 * vbdev_lvol_create_bdev_clone - 외부 bdev(esnap)를 부모로 한 clone 생성.
 */
void vbdev_lvol_create_bdev_clone(const char *esnap_uuid,
				  struct spdk_lvol_store *lvs, const char *clone_name,
				  spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * \brief Change size of lvol
 * \param lvol Handle to lvol
 * \param sz Size of lvol to change
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 * \return error
 */
/*
 * [한국어] vbdev_lvol_resize - lvol 크기 변경. spdk_bdev_notify_blockcnt_change 자동.
 */
void vbdev_lvol_resize(struct spdk_lvol *lvol, uint64_t sz, spdk_lvol_op_complete cb_fn,
		       void *cb_arg);

/**
 * \brief Mark lvol as read only
 * \param lvol Handle to lvol
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
/*
 * [한국어] vbdev_lvol_set_read_only - lvol을 read-only로 변경 (메타데이터에 영구 반영).
 */
void vbdev_lvol_set_read_only(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg);

/*
 * [한국어] vbdev_lvol_rename - lvol 이름 변경(메타데이터 갱신 + bdev 이름 alias 갱신).
 */
void vbdev_lvol_rename(struct spdk_lvol *lvol, const char *new_lvol_name,
		       spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Destroy a logical volume
 * \param lvol Handle to lvol
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
/*
 * [한국어] vbdev_lvol_destroy - lvol 삭제 + blob 해제 + cluster 반납.
 */
void vbdev_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * \brief Renames given lvolstore.
 *
 * \param lvs Pointer to lvolstore
 * \param new_name New name of lvs
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
/*
 * [한국어] vbdev_lvs_rename - lvolstore 이름 변경.
 */
void vbdev_lvs_rename(struct spdk_lvol_store *lvs, const char *new_lvs_name,
		      spdk_lvs_op_complete cb_fn, void *cb_arg);

/**
 * \brief Search for handle lvolstore
 * \param uuid_str UUID of lvolstore
 * \return Handle to spdk_lvol_store or NULL if not found.
 */
/*
 * [한국어] vbdev_get_lvol_store_by_uuid - UUID 문자열로 lvolstore 조회.
 */
struct spdk_lvol_store *vbdev_get_lvol_store_by_uuid(const char *uuid_str);

/**
 * \brief Search for handle to lvolstore
 * \param name name of lvolstore
 * \return Handle to spdk_lvol_store or NULL if not found.
 */
/*
 * [한국어] vbdev_get_lvol_store_by_name - 이름으로 lvolstore 조회.
 */
struct spdk_lvol_store *vbdev_get_lvol_store_by_name(const char *name);

/**
 * \brief Search for handle to lvol_store_bdev
 * \param lvs handle to lvolstore
 * \return Handle to lvol_store_bdev or NULL if not found.
 */
/*
 * [한국어] vbdev_get_lvs_bdev_by_lvs - lvolstore → lvol_store_bdev 역포인터.
 */
struct lvol_store_bdev *vbdev_get_lvs_bdev_by_lvs(struct spdk_lvol_store *lvs);

/*
 * [한국어] vbdev_lvol_get_from_bdev - spdk_bdev → spdk_lvol 역포인터(lvol_bdev::bdev 임베드 활용).
 */
struct spdk_lvol *vbdev_lvol_get_from_bdev(struct spdk_bdev *bdev);

/*
 * [한국어] vbdev_lvol_esnap_dev_create - external snapshot(esnap)의 bs_dev 어댑터 생성 콜백.
 * Blobstore가 esnap 부모를 열어야 할 때 등록된 콜백으로 호출되어 외부 bdev를 wrap한 bs_dev를 반환.
 */
int vbdev_lvol_esnap_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
				const void *esnap_id, uint32_t id_len,
				struct spdk_bs_dev **_bs_dev);

/**
 * \brief Make a shallow copy of lvol over a bdev
 *
 * \param lvol Handle to lvol
 * \param bdev_name Name of the bdev to copy on
 * \param status_cb_fn Called repeatedly during operation with status updates
 * \param status_cb_arg Argument passed to function status_cb_fn.
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 *
 * \return 0 if operation starts correctly, negative errno on failure.
 */
/*
 * [한국어] vbdev_lvol_shallow_copy - 할당된 cluster만 외부 bdev로 복사 (sparse-aware).
 */
int vbdev_lvol_shallow_copy(struct spdk_lvol *lvol, const char *bdev_name,
			    spdk_blob_shallow_copy_status status_cb_fn, void *status_cb_arg,
			    spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * \brief Set an external snapshot as the parent of a lvol.
 *
 * \param lvol Handle to lvol
 * \param esnap_name Name of the bdev that acts as external snapshot
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
/*
 * [한국어] vbdev_lvol_set_external_parent - 외부 bdev를 esnap parent로 설정.
 * 이후 lvol에 read가 들어와도 할당 없는 cluster는 esnap parent로 redirect.
 */
void vbdev_lvol_set_external_parent(struct spdk_lvol *lvol, const char *esnap_name,
				    spdk_lvol_op_complete cb_fn, void *cb_arg);

#endif /* SPDK_VBDEV_LVOL_H */
