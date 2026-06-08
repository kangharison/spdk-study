/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

/*
 * [한국어 설명] xNVMe bdev 모듈 공개 API 헤더 (bdev_xnvme.h)
 *
 * === 파일의 역할 ===
 * libxnvme(https://xnvme.io)을 백엔드로 사용하는 bdev 모듈의 공개 API. xNVMe는 OS/하드웨어에
 * 독립적인 NVMe 액세스 라이브러리로, 동일한 코드가 Linux io_uring(_cmd) / libaio / SPDK NVMe
 * driver / Windows IOCP / 사용자 공간 spdk-fuse 등 다양한 백엔드(="io_mechanism")로 동작한다.
 * 본 모듈을 통하면 io_mechanism 옵션 하나로 어떤 백엔드를 쓸지 결정해 동일한 bdev 인터페이스로
 * 노출할 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [JSON-RPC] → [bdev_xnvme_rpc.c] → [이 헤더] → [bdev_xnvme.c]
 *                                                  ↓ libxnvme
 *                                          [io_uring_cmd / libaio / SPDK / IOCP / ...]
 *
 * === 타 모듈과의 연결 ===
 * - bdev_xnvme.c : 구현부.
 * - libxnvme : 외부 라이브러리.
 *
 * === 주요 함수/구조체 요약 ===
 * - create_xnvme_bdev() : xnvme device open + spdk_bdev_register.
 * - delete_xnvme_bdev() : 비동기 unregister.
 */

#ifndef SPDK_BDEV_XNVME_H
#define SPDK_BDEV_XNVME_H

#include "spdk/stdinc.h"
/* [한국어] 표준 헤더 모음. */

#include "spdk/queue.h"
/* [한국어] TAILQ/LIST 매크로. */
#include "spdk/bdev.h"
/* [한국어] spdk_bdev. */

#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈 작성 API. spdk_bdev_unregister_cb 타입. */

/*
 * [한국어]
 * create_xnvme_bdev - xNVMe device를 열어 bdev로 등록
 *
 * @name: bdev 이름.
 * @filename: xnvme URI (예: "/dev/nvme0n1", "0000:00:04.0:nsid=1").
 * @io_mechanism: xnvme 백엔드 ("io_uring", "io_uring_cmd", "libaio", "spdk" 등).
 * @conserve_cpu: true면 xnvme가 cpu 절약 모드(인터럽트/슬립) 사용, false면 polled.
 * @return: 성공 시 spdk_bdev 포인터, 실패 시 NULL.
 */
struct spdk_bdev *create_xnvme_bdev(const char *name, const char *filename,
				    const char *io_mechanism, bool conserve_cpu);

/*
 * [한국어] delete_xnvme_bdev - 비동기 unregister.
 */
void delete_xnvme_bdev(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_XNVME_H */
