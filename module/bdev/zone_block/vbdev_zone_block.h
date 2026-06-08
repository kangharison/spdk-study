/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] zone_block 가상 bdev 공개 API 헤더 (vbdev_zone_block.h)
 *
 * === 파일의 역할 ===
 * 일반 블록 bdev 위에 Zoned Block Device(ZNS-like) 의미론을 에뮬레이션해주는 vbdev 모듈의
 * 공개 API. ZNS(NVMe Zoned Namespace)는 디스크를 다수의 zone으로 분할해 sequential write만
 * 허용하는 모델로, 본 모듈은 일반 bdev 위에 zone capacity/optimal open zones를 정의해
 * APP_OPEN/CLOSE/RESET 등 ZNS 명령을 시뮬레이션한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [JSON-RPC] → [vbdev_zone_block_rpc.c] → [이 헤더] → [vbdev_zone_block.c]
 *                                                        ↓ spdk_bdev_open_ext
 *                                                    [base bdev (e.g. NVMe / Malloc)]
 *
 * === 타 모듈과의 연결 ===
 * - vbdev_zone_block.c : 구현부.
 * - include/spdk/bdev_zone.h : zone bdev 공통 인터페이스 정의.
 *
 * === 주요 함수/구조체 요약 ===
 * - vbdev_zone_block_create() : zone_capacity, optimal_open_zones를 지정해 zoned vbdev 생성.
 * - vbdev_zone_block_delete() : 이름 기반 비동기 unregister.
 */

#ifndef SPDK_VBDEV_ZONE_BLOCK_H
#define SPDK_VBDEV_ZONE_BLOCK_H

#include "spdk/stdinc.h"
/* [한국어] 표준 헤더 모음. */

#include "spdk/bdev.h"
/* [한국어] spdk_bdev_unregister_cb 타입. */
#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈 작성 API. */

/*
 * [한국어]
 * vbdev_zone_block_create - base bdev 위에 zoned vbdev 생성
 *
 * @bdev_name: 기반이 될 base bdev 이름.
 * @vbdev_name: 새로 생성할 zoned vbdev 이름.
 * @zone_capacity: 한 zone의 사용 가능 LBA 수 (zone size와 다를 수 있음 — ZNS 스펙).
 * @optimal_open_zones: 동시에 열어둘 권장 zone 개수 — bdev_zone_info에 노출됨.
 * @return: 0 성공, 음수 -errno.
 */
int vbdev_zone_block_create(const char *bdev_name, const char *vbdev_name,
			    uint64_t zone_capacity, uint64_t optimal_open_zones);

/*
 * [한국어]
 * vbdev_zone_block_delete - zoned vbdev 비동기 unregister.
 * @name: 대상 vbdev 이름.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 인자.
 */
void vbdev_zone_block_delete(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg);

#endif /* SPDK_VBDEV_ZONE_BLOCK_H */
