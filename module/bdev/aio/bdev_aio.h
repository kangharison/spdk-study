/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] AIO bdev 모듈의 공개 API 헤더 (bdev_aio.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 AIO bdev 모듈(bdev_aio.c)이 외부(주로 RPC 핸들러 bdev_aio_rpc.c)에 노출하는
 * 최소 인터페이스를 선언한다. AIO bdev란 일반 파일이나 블록 디바이스(/dev/sdX, 루프 파일 등)를
 * 리눅스 libaio(또는 FreeBSD POSIX AIO) 위에 얹어 SPDK bdev로 노출시키는 모듈이다.
 * 본 헤더가 제공하는 API는 (1) bdev 인스턴스 생성 create_aio_bdev, (2) 삭제 bdev_aio_delete,
 * (3) 크기/속성 재조사 bdev_aio_rescan 세 가지이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 헤더는 SPDK bdev 계층의 "백엔드 모듈"인 AIO 모듈의 thin facade이다.
 *   [JSON-RPC client]
 *     → [bdev_aio_rpc.c]
 *         → [이 헤더] create_aio_bdev / bdev_aio_delete / bdev_aio_rescan
 *             → [bdev_aio.c] 실제 spdk_bdev_register, open(O_DIRECT), io_setup 등 수행
 * 본 헤더의 함수는 RPC/관리 스레드(app thread)에서 호출되며 그 함수들 내부에서
 * spdk_io_device_register / spdk_bdev_register 같은 thread-affine 동작을 수행한다.
 *
 * === 타 모듈과의 연결 ===
 * - bdev_aio.c : 이 헤더의 함수를 정의하는 실제 구현부.
 * - bdev_aio_rpc.c : "bdev_aio_create" / "bdev_aio_delete" / "bdev_aio_rescan" JSON-RPC
 *   핸들러가 본 헤더의 함수를 호출.
 * - include/spdk/bdev.h : spdk_uuid 타입 사용. bdev 레이어 공개 API를 함께 사용한다.
 * - include/spdk/stdinc.h : 표준 헤더 일괄 포함 (bool, uint32_t 등).
 *
 * === 주요 함수/구조체 요약 ===
 * - typedef delete_aio_bdev_complete : 삭제 완료 콜백 시그니처.
 * - create_aio_bdev() : 파일/디바이스 열고 spdk_bdev_register까지 수행.
 * - bdev_aio_delete() : 이름으로 lookup 후 spdk_bdev_unregister(비동기).
 * - bdev_aio_rescan() : 백엔드 사이즈가 바뀐 경우 bdev 크기 정보 갱신.
 */

#ifndef SPDK_BDEV_AIO_H
#define SPDK_BDEV_AIO_H

#include "spdk/stdinc.h"
/* [한국어] 표준 헤더 모음. bool/uint32_t 등 본 헤더가 노출하는 타입이 필요. */
#include "spdk/bdev.h"
/* [한국어] bdev 공개 API 헤더. struct spdk_uuid 정의를 사용. */

/*
 * [한국어] typedef delete_aio_bdev_complete
 *
 * AIO bdev 삭제 완료 콜백의 함수 시그니처.
 * - 설정자: RPC 핸들러가 bdev_aio_delete에 전달.
 * - 호출자: bdev_aio_delete의 내부에서 spdk_bdev_unregister가 끝나면 호출.
 * - 값 범위: cb_arg는 임의 포인터(주로 RPC ctx), bdeverrno는 0(성공)/음수 -errno(실패).
 * - 호출 컨텍스트: app thread(unregister 콜백 컨텍스트).
 */
typedef void (*delete_aio_bdev_complete)(void *cb_arg, int bdeverrno);

/*
 * [한국어]
 * create_aio_bdev - 파일/디바이스를 AIO bdev로 등록 (RPC 백엔드 진입점)
 *
 * @name: bdev 등록 시 사용할 고유 이름.
 * @filename: 백엔드 파일/디바이스 경로(예: "/dev/sdb", "/var/tmp/aiofile").
 * @block_size: 블록 크기(바이트). 0이면 자동 감지 (블록 디바이스의 논리 블록 크기).
 * @readonly: true면 O_RDONLY로 open + write 차단.
 * @falloc: true면 UNMAP/WRITE_ZEROES를 fallocate(2)로 매핑 (Linux+fs 지원 필요).
 * @uuid: 사용자가 명시한 UUID(NULL이면 자동 생성).
 * @nowait: true면 io_submit에 RWF_NOWAIT 사용 (블록 디바이스 한정).
 * @return: 0 성공, 음수 -errno 실패.
 *
 * 호출 컨텍스트: RPC/app thread.
 */
int create_aio_bdev(const char *name, const char *filename, uint32_t block_size, bool readonly,
		    bool falloc, const struct spdk_uuid *uuid, bool nowait);

/*
 * [한국어]
 * bdev_aio_rescan - 백엔드 크기 변화를 bdev 크기 정보에 반영
 *
 * @name: 대상 bdev 이름.
 * @return: 0 성공, 음수 -errno.
 *
 * 블록 디바이스가 외부에서 크기가 바뀐 경우(예: 핫 추가/리사이즈) bdev 레이어의 blockcnt를
 * spdk_fd_get_size로 다시 측정해 spdk_bdev_notify_blockcnt_change로 전파한다.
 */
int bdev_aio_rescan(const char *name);

/*
 * [한국어]
 * bdev_aio_delete - AIO bdev 인스턴스를 비동기 삭제
 *
 * @name: 대상 bdev 이름.
 * @cb_fn: 삭제 완료(또는 실패) 시 호출될 콜백. NULL 가능.
 * @cb_arg: 콜백 인자.
 *
 * 내부적으로 spdk_bdev_open_ext/unregister를 호출. unregister는 모든 채널 destroy 후
 * destruct_cb를 거쳐 fd close와 메모리 free까지 진행. 콜백은 app thread에서 호출됨.
 */
void bdev_aio_delete(const char *name, delete_aio_bdev_complete cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_AIO_H */
