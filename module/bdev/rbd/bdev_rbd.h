/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Ceph RBD bdev 모듈 공개 API 헤더 (bdev_rbd.h)
 *
 * === 파일의 역할 ===
 * Ceph RBD(RADOS Block Device)를 백엔드로 사용하는 bdev 모듈의 공개 API. RBD는 Ceph 분산
 * 스토리지가 제공하는 block-level 객체로, librbd/librados를 통해 사용자 공간에서 직접 접근 가능.
 * 본 모듈은 RADOS cluster 핸들 등록, RBD pool/image 매핑을 통해 임의의 Ceph 이미지를 spdk_bdev로
 * 노출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [JSON-RPC] → [bdev_rbd_rpc.c] → [이 헤더] → [bdev_rbd.c]
 *                                                ↓ librbd
 *                                                ↓ librados
 *                                                [Ceph OSD cluster]
 *
 * === 타 모듈과의 연결 ===
 * - bdev_rbd.c : 구현부.
 * - librbd / librados : Ceph 클라이언트 라이브러리.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct cluster_register_info : RADOS cluster 등록 정보.
 * - bdev_rbd_create / delete / resize : RBD bdev 라이프사이클.
 * - bdev_rbd_register_cluster / unregister_cluster / get_clusters_info : RADOS cluster 관리.
 */

#ifndef SPDK_BDEV_RBD_H
#define SPDK_BDEV_RBD_H

#include "spdk/stdinc.h"
/* [한국어] 표준 헤더 모음. */

#include "spdk/bdev.h"
/* [한국어] spdk_bdev 타입. */
#include "spdk/rpc.h"
/* [한국어] spdk_jsonrpc_request 타입 (RPC 응답 작성에 사용). */

/*
 * [한국어] struct cluster_register_info
 *
 * RADOS cluster를 SPDK에 등록하기 위한 정보 묶음. 다수의 RBD bdev가 같은 cluster를 공유 가능.
 */
struct cluster_register_info {
	char *name;
	/* [한국어] cluster 등록 이름. bdev_rbd_create에서 cluster_name으로 참조. */
	char *user_id;
	/* [한국어] CEPH user(예: "client.admin"). NULL이면 default. */
	char **config_param;
	/* [한국어] key=value 형식의 추가 RADOS config (NULL-terminated 배열). NULL 가능. */
	char *config_file;
	/* [한국어] ceph.conf 경로. NULL이면 default. */
	char *key_file;
	/* [한국어] keyring 파일 경로. NULL 가능. */
	char *core_mask;
	/* [한국어] librados 콜백을 실행할 코어 마스크 (RADOS worker thread affinity). */
};

/*
 * [한국어]
 * bdev_rbd_free_config - bdev_rbd_dup_config로 만든 config 배열 해제.
 */
void bdev_rbd_free_config(char **config);

/*
 * [한국어]
 * bdev_rbd_dup_config - NULL-terminated 문자열 배열을 깊은 복사.
 * @return: 새 배열 (free는 bdev_rbd_free_config로).
 */
char **bdev_rbd_dup_config(const char *const *config);

/*
 * [한국어] typedef spdk_delete_rbd_complete - RBD 삭제 완료 콜백 시그니처.
 */
typedef void (*spdk_delete_rbd_complete)(void *cb_arg, int bdeverrno);

/*
 * [한국어]
 * bdev_rbd_create - RBD image를 bdev로 등록
 *
 * @bdev: [out] 생성된 bdev 포인터.
 * @name: bdev 이름.
 * @user_id: Ceph user.
 * @pool_name: RADOS pool 이름.
 * @config: 추가 config (NULL 가능).
 * @rbd_name: RBD image 이름.
 * @block_size: 블록 크기(바이트).
 * @cluster_name: 사용할 등록된 cluster 이름(NULL이면 임시 cluster 생성).
 * @uuid: bdev UUID(NULL이면 자동).
 * @read_only: true면 read-only로 open.
 * @return: 0 성공, 음수 -errno.
 */
int bdev_rbd_create(struct spdk_bdev **bdev, const char *name, const char *user_id,
		    const char *pool_name,
		    const char *const *config,
		    const char *rbd_name, uint32_t block_size, const char *cluster_name,
		    const struct spdk_uuid *uuid, bool read_only);
/**
 * Delete rbd bdev.
 *
 * \param name Name of rbd bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
/*
 * [한국어] bdev_rbd_delete - 이름 기반 비동기 unregister.
 */
void bdev_rbd_delete(const char *name, spdk_delete_rbd_complete cb_fn,
		     void *cb_arg);

/**
 * Resize rbd bdev.
 *
 * \param bdev Name of rbd bdev.
 * \param new_size_in_mb The new size in MiB for this bdev.
 */
/*
 * [한국어] bdev_rbd_resize - RBD image 크기 변경 (librbd 통해 OSD에 반영) + blockcnt 업데이트.
 */
int bdev_rbd_resize(const char *name, const uint64_t new_size_in_mb);

/**
 * Create a Rados cluster.
 *
 * \param info the info to register the Rados cluster object
 */
/*
 * [한국어] bdev_rbd_register_cluster - RADOS cluster handle 생성 + 모듈 글로벌 리스트 등록.
 */
int bdev_rbd_register_cluster(struct cluster_register_info *info);

/**
 * Delete a registered cluster.
 *
 * \param name the name of the cluster to be deleted.
 */
/*
 * [한국어] bdev_rbd_unregister_cluster - 등록된 RADOS cluster 해제 (사용 중이면 -EBUSY).
 */
int bdev_rbd_unregister_cluster(const char *name);

/**
 * Show the cluster info of a given name. If given name is empty,
 * the info of every registered cluster name will be showed.
 *
 * \param request the json request.
 * \param name the name of the cluster.
 */
/*
 * [한국어] bdev_rbd_get_clusters_info - 등록된 cluster 정보 JSON 응답.
 * name==NULL이면 전체, 아니면 해당 cluster만.
 */
int bdev_rbd_get_clusters_info(struct spdk_jsonrpc_request *request, const char *name);

#endif /* SPDK_BDEV_RBD_H */
