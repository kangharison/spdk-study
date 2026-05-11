/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Filesystem device internal APIs
 */

/*
 * [한국어 설명] fsdev(파일시스템 디바이스) 내부 헤더 (fsdev_internal.h)
 *
 * === 파일의 역할 ===
 * SPDK는 블록 디바이스 추상화(bdev) 외에, vhost-fs / vDPA-fs 등이 내려줄 파일시스템
 * 작업(open/read/write/lookup/setattr 등)을 모듈화한 "fsdev" 추상화를 제공한다.
 * bdev가 LBA-기반 블록 IO를 다룬다면, fsdev는 inode/path-기반 파일 시스템 호출을
 * 받아 백엔드(예: aio_fs, libfuse-기반 모듈, NFS proxy)에 위임한다. 본 헤더는
 * fsdev 코어(lib/fsdev/fsdev.c)와 IO 처리 경로(lib/fsdev/fsdev_io.c) 사이에서 공유하는
 * 내부 함수와 변환 매크로를 모은다. 외부 공개 API는 include/spdk/fsdev.h에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   vhost-user-fs front-end (QEMU 등)
 *     → SPDK vhost-fs (lib/vhost or virtio_fs)
 *       → fsdev 공개 API (spdk_fsdev_*)
 *         → fsdev 코어 (fsdev.c) — 채널·통계·IO 풀 관리
 *           → [본 헤더의 fsdev_io_submit] → fsdev module driver
 *             → 백엔드(예: aio_fs → posix open/read 등)
 * __io_ch_to_fsdev_ch 매크로는 SPDK io_channel 추상화에서 fsdev 전용 채널 ctx를 꺼낸다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/thread (io_channel), spdk/fsdev(공개 자료형 선언).
 * - 의존받음: lib/fsdev/fsdev.c, lib/fsdev/fsdev_io.c, fsdev module driver들.
 * - 데이터 흐름: 채널의 IO 풀에서 fsdev_channel_get_io()로 fsdev_io 객체를 빌려옴 →
 *   타입별 인자 채워서 fsdev_io_submit()으로 모듈에 전달 → 완료 콜백에서 풀에 반환.
 * - 공유 자료구조: struct spdk_fsdev_io (요청 객체), struct spdk_fsdev_channel
 *   (per-thread 채널 ctx — IO 풀, 진행 중 큐 등).
 *
 * === 주요 함수/구조체 요약 ===
 * - fsdev_io_submit             : 작성된 fsdev_io를 백엔드 모듈로 디스패치.
 * - fsdev_channel_get_io        : 채널의 IO 풀에서 fsdev_io 한 개 할당.
 * - __io_ch_to_fsdev_ch         : spdk_io_channel → spdk_fsdev_channel 변환 매크로.
 */

#ifndef SPDK_FSDEV_INT_H
#define SPDK_FSDEV_INT_H
/* [한국어] 다중 포함 가드. */

#include "spdk/thread.h"
/* [한국어] spdk_io_channel API — 채널 ctx 캐스팅 매크로에서 사용. */

/*
 * [한국어]
 * fsdev_io_submit - 채워진 fsdev_io를 fsdev 모듈에 디스패치(submit).
 *
 * @fsdev_io: 호출자가 모든 파라미터(타입, args, 콜백, ctx)를 채워서 넘긴 IO 객체.
 *
 * 코어는 통계 갱신(in_flight ++), QoS rate-limit 체크, 그리고 fsdev->fn_table->submit_request
 * 호출을 한다. submit_request는 동일 SPDK thread에서 동기적으로 시작하지만 완료는 비동기이므로
 * 호출 후 즉시 반환된다. 완료 시 fsdev_io_complete가 호출되며 사용자 콜백이 실행된다.
 *
 * 실행 컨텍스트: 채널을 소유한 SPDK thread (cross-thread 호출 금지).
 *
 * 호출 체인:
 *   spdk_fsdev_lookup/read/write 등 공개 API → [fsdev_io_submit] → module->submit_request
 */
void fsdev_io_submit(struct spdk_fsdev_io *fsdev_io);

/*
 * [한국어]
 * fsdev_channel_get_io - 채널 IO 풀에서 fsdev_io 객체를 한 개 꺼냄(없으면 NULL).
 *
 * @channel: 호출 thread 소유의 fsdev 채널 ctx.
 * @return : 할당된 fsdev_io 또는 NULL(고갈 시).
 *
 * fsdev_io는 spdk_mempool로 관리되며, 채널은 그 풀에서 빌려와 자기 채널 큐에 추가한다.
 * 풀 고갈 시 호출자는 일반적으로 nomem 큐로 backpressure하고 나중에 재시도해야 한다.
 *
 * 호출 체인:
 *   공개 API spdk_fsdev_* → [fsdev_channel_get_io] → spdk_mempool_get
 */
struct spdk_fsdev_io *fsdev_channel_get_io(struct spdk_fsdev_channel *channel);

#define __io_ch_to_fsdev_ch(io_ch)	((struct spdk_fsdev_channel *)spdk_io_channel_get_ctx(io_ch))
/* [한국어] spdk_io_channel*에서 우리가 등록한 fsdev 전용 ctx로 캐스팅하는 헬퍼.
 * SPDK io_channel은 모듈별 ctx를 채널 객체 뒤에 인라인으로 붙여 두므로
 * spdk_io_channel_get_ctx로 그 포인터를 얻고 fsdev 전용 타입으로 캐스팅한다.
 * 매크로로 둔 이유는 hot path에서 함수 호출 비용 없이 인라인되기 위함. */

#endif /* SPDK_FSDEV_INT_H */
/* [한국어] 다중 포함 가드 종결. */
