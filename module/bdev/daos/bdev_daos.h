/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) croit GmbH.
 *   All rights reserved.
 */

/*
 * [한국어 설명] DAOS bdev 모듈 공개 API 헤더 (bdev_daos.h)
 *
 * === 파일의 역할 ===
 * DAOS(Distributed Asynchronous Object Storage)를 백엔드로 사용하는 bdev 모듈의 공개 API.
 * DAOS는 Intel이 주도한 차세대 분산 객체 스토리지로, OCPI(Optane PMem) + NVMe를 분산 배치하고
 * libdaos/libdfs API를 통해 객체 단위 I/O를 제공한다. 본 모듈은 DAOS의 pool/container/object를
 * spdk_bdev로 노출해 SPDK 스택이 DAOS를 일반 블록 디바이스처럼 사용 가능하게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [JSON-RPC] → [bdev_daos_rpc.c] → [이 헤더] → [bdev_daos.c]
 *                                                ↓ libdaos
 *                                                [DAOS pool/container/object]
 *
 * === 타 모듈과의 연결 ===
 * - bdev_daos.c : 구현부 (daos_init/connect/object_open, daos_event_init 등).
 * - libdaos / libdfs : DAOS C API.
 *
 * === 주요 함수/구조체 요약 ===
 * - create_bdev_daos() : DAOS pool/container/object 핸들을 만들고 spdk_bdev_register.
 * - delete_bdev_daos() : 비동기 unregister.
 * - bdev_daos_resize() : 사이즈 조정.
 */

#ifndef SPDK_BDEV_DAOS_H
#define SPDK_BDEV_DAOS_H

#include "spdk/stdinc.h"
/* [한국어] 표준 헤더 모음. */
#include "spdk/bdev.h"
/* [한국어] spdk_bdev, spdk_uuid 타입. */
#include "spdk/bdev_module.h"
/* [한국어] spdk_bdev_unregister_cb 타입. */

/*
 * [한국어]
 * create_bdev_daos - DAOS object를 bdev로 등록
 *
 * @bdev: [out] 생성된 bdev 포인터.
 * @name: bdev 이름.
 * @uuid: bdev UUID (NULL이면 자동).
 * @pool: DAOS pool label/UUID.
 * @cont: DAOS container label/UUID.
 * @oclass: DAOS object class 이름 (예: "SX", "RP_2GX").
 * @num_blocks: 디바이스 블록 수.
 * @block_size: 블록 크기(바이트).
 * @return: 0 성공, 음수 -errno.
 */
int create_bdev_daos(struct spdk_bdev **bdev, const char *name, const struct spdk_uuid *uuid,
		     const char *pool, const char *cont, const char *oclass,
		     uint64_t num_blocks, uint32_t block_size);

/*
 * [한국어] delete_bdev_daos - 비동기 unregister.
 */
void delete_bdev_daos(const char *bdev_name, spdk_bdev_unregister_cb cb_fn, void *cb_arg);

/**
 * Resize DAOS bdev.
 *
 * \param bdev_name Name of DAOS bdev.
 * \param new_size_in_mb The new size in MiB for this bdev
 */
/*
 * [한국어] bdev_daos_resize - DAOS bdev 블록 개수 변경.
 */
int bdev_daos_resize(const char *bdev_name, const uint64_t new_size_in_mb);

#endif /* SPDK_BDEV_DAOS_H */
