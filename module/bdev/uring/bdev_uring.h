/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] io_uring bdev 모듈 공개 API 헤더 (bdev_uring.h)
 *
 * === 파일의 역할 ===
 * io_uring 기반 bdev 모듈의 외부 인터페이스를 선언한다. io_uring은 Linux 5.1+가 제공하는
 * 차세대 비동기 I/O 인터페이스로, 사용자 공간 ring buffer를 통해 SQE(Submission Queue Entry)를
 * 커널에 전달하고 CQE(Completion Queue Entry)를 회수한다. libaio 대비 syscall 횟수가 낮고
 * Polled-mode / IORING_SETUP_SQPOLL 같은 다양한 모드를 지원해 SPDK의 polled-mode 모델과 잘 맞는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [JSON-RPC] → [bdev_uring_rpc.c] → [이 헤더] → [bdev_uring.c → io_uring_setup/io_uring_enter]
 *     → [Linux kernel io_uring]
 *
 * === 타 모듈과의 연결 ===
 * - bdev_uring.c : 구현부.
 * - bdev_uring_rpc.c : "bdev_uring_create/delete/rescan" JSON-RPC 핸들러.
 * - liburing : 사용자 공간 io_uring 헬퍼 라이브러리.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct bdev_uring_opts : 생성 옵션.
 * - create_uring_bdev() : open(O_DIRECT) + spdk_bdev_register.
 * - delete_uring_bdev() : 비동기 unregister.
 * - bdev_uring_rescan() : 디바이스 크기 재측정.
 */

#ifndef SPDK_BDEV_URING_H
#define SPDK_BDEV_URING_H

#include "spdk/stdinc.h"
/* [한국어] 표준 헤더 모음. */

#include "spdk/queue.h"
/* [한국어] TAILQ/LIST 매크로. 모듈 글로벌 리스트가 본 헤더 사용처에서 필요. */
#include "spdk/bdev.h"
/* [한국어] spdk_bdev / spdk_uuid 타입. */

#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈 작성 API. */

/*
 * [한국어] typedef spdk_delete_uring_complete
 * uring bdev 삭제 완료 콜백 시그니처.
 */
typedef void (*spdk_delete_uring_complete)(void *cb_arg, int bdeverrno);

/*
 * [한국어] struct bdev_uring_opts
 * uring bdev 생성 옵션. AIO와 유사하지만 fallocate/readonly/nowait 옵션은 없음.
 */
struct bdev_uring_opts {
	const char *name;
	/* [한국어] bdev 등록 이름. */
	const char *filename;
	/* [한국어] 백엔드 파일/디바이스 경로. */
	uint32_t block_size;
	/* [한국어] 블록 크기(0이면 자동 감지). */
	struct spdk_uuid uuid;
	/* [한국어] UUID(zero이면 자동 생성). */
};

/*
 * [한국어]
 * create_uring_bdev - uring bdev 생성 및 등록
 *
 * @opts: 생성 옵션.
 * @return: 성공 시 spdk_bdev 포인터, 실패 시 NULL.
 */
struct spdk_bdev *create_uring_bdev(const struct bdev_uring_opts *opts);

/*
 * [한국어]
 * delete_uring_bdev - 비동기 삭제. unregister 완료 후 cb_fn 호출.
 */
void delete_uring_bdev(const char *name, spdk_delete_uring_complete cb_fn, void *cb_arg);

/*
 * [한국어]
 * bdev_uring_rescan - 백엔드 크기 변경 반영.
 * @return: 0 성공, 음수 -errno.
 */
int bdev_uring_rescan(const char *name);

#endif /* SPDK_BDEV_URING_H */
