/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] bdev 내부 헬퍼 선언 (bdev_internal.h)
 *
 * === 파일의 역할 ===
 * bdev 레이어의 **공개되지 않은** 내부 헬퍼 함수들의 프로토타입과 상수를
 * 모아둔 헤더. lib/bdev/ 내의 여러 소스 파일(bdev.c, bdev_rpc.c, bdev_zone.c,
 * part.c 등)이 서로 공유하는 구현 세부 함수는 공개 bdev.h에 노출하지 않고
 * 이 헤더로만 선언한다. 외부 모듈·애플리케이션은 이 헤더를 포함하지 않아야
 * 한다.
 *
 * 주요 내용:
 *   - ZERO_BUFFER_SIZE: write_zeroes fallback용 내부 제로 버퍼 크기(1MB)
 *   - bdev_channel_get_io(): 채널별 bdev_io 풀에서 엔트리 획득
 *   - bdev_io_init(): bdev_io 구조체 기본 필드 초기화
 *   - bdev_io_submit(): bdev_io를 실제 모듈 경로로 밀어넣는 내부 제출 함수
 *   - bdev_alloc_io_stat / bdev_free_io_stat: per-device 통계 구조 할당·해제
 *   - bdev_reset_device_stat(): 통계 리셋 (모드별)
 *
 * === 전체 아키텍처에서의 위치 ===
 * bdev 코어(lib/bdev) 전용. spdk_bdev_read() 같은 공개 API가 내부에서 이
 * 헤더의 함수들을 경유해 bdev_io를 준비·제출하고, 채널 단위 통계를 갱신한다.
 * 실행 컨텍스트: 각 함수는 해당 bdev의 I/O 채널 소유 스레드에서 호출되는
 * 것이 원칙. 크로스 스레드 호출 시 spdk_thread_send_msg를 통한 우회 필요.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/bdev.h — 공개 타입(spdk_bdev, spdk_bdev_io 등)
 * 의존하는 모듈(파일 기준): lib/bdev/bdev.c, bdev_rpc.c, bdev_zone.c, part.c.
 * 공유 자료구조: spdk_bdev, spdk_bdev_io, spdk_bdev_channel (opaque 전방선언).
 *
 * === 주요 함수/구조체 요약 ===
 * 위 내용 참조. 모두 구현 내부 경로 전용이며 ABI 안정성 보장 대상 아님.
 */

#ifndef SPDK_BDEV_INTERNAL_H     /* [한국어] include 가드 시작 */
#define SPDK_BDEV_INTERNAL_H

#include "spdk/bdev.h"           /* [한국어] 공개 타입 확보 — spdk_bdev_io_completion_cb 등 콜백 시그니처 정의 */

#define ZERO_BUFFER_SIZE	0x100000
                                 /* [한국어] write_zeroes 폴백 시 호스트 메모리에 할당하는 제로 버퍼 크기 = 1 MiB
                                  *  - 장치가 WRITE ZEROES 커맨드를 지원하지 않거나 영역 매핑이 제한적일 때, bdev 코어가 이 크기로 제로 버퍼를 만들고 일반 write 커맨드를 반복 발행
                                  *  - 1 MiB는 성능·메모리 절충치. 너무 작으면 커맨드 수 증가로 오버헤드, 너무 크면 DMA 버퍼 pinning 비용 증가 */

struct spdk_bdev;                /* [한국어] opaque — bdev 레이어가 등록·관리하는 블록 디바이스 표현 */
struct spdk_bdev_io;             /* [한국어] opaque — 단일 I/O 요청 객체 (read/write/unmap 등) */
struct spdk_bdev_channel;        /* [한국어] opaque — 스레드 소유 bdev 채널 (I/O 컨텍스트·통계 보관) */

struct spdk_bdev_io *bdev_channel_get_io(struct spdk_bdev_channel *channel);
/*
 * [한국어]
 * bdev_channel_get_io - 채널별 bdev_io 풀에서 사용 가능한 엔트리 하나 획득
 *
 * @channel: I/O 컨텍스트가 속한 채널 (thread-affinity 준수)
 * @return: 사용 가능한 bdev_io 포인터. 풀 고갈 시 NULL
 *
 * 내부: 채널 내부 per-thread 프리리스트(mempool)에서 원소 pop. 락 없음(thread-local).
 * 호출 체인: spdk_bdev_read/write 등 공개 API → 이 함수 → bdev_io_init → 모듈 제출 콜백
 */

void bdev_io_init(struct spdk_bdev_io *bdev_io, struct spdk_bdev *bdev, void *cb_arg,
		  spdk_bdev_io_completion_cb cb);
/*
 * [한국어]
 * bdev_io_init - bdev_io 기본 필드 초기화
 *
 * @bdev_io: 획득된 bdev_io 슬롯
 * @bdev:    타겟 bdev 포인터
 * @cb_arg:  완료 콜백에 전달할 사용자 컨텍스트
 * @cb:      완료 콜백 함수 포인터
 *
 * 설정 내용: 타겟 bdev, 콜백, 상태 초기값, 내부 리스트 링크 제거 등.
 * 실행 컨텍스트: 채널 소유 스레드.
 */

void bdev_io_submit(struct spdk_bdev_io *bdev_io);
/*
 * [한국어]
 * bdev_io_submit - 준비된 bdev_io를 bdev 모듈 계층으로 제출
 *
 * - 큐에 여유가 있으면 모듈의 submit_request 콜백 즉시 호출
 * - in-flight 한계 초과 시 nomem 큐에 적재, 완료 이벤트에서 drain
 *
 * 호출 체인: 공개 API(spdk_bdev_read 등) → bdev_io_submit → 모듈 submit_request → NVMe/aio/uring/...
 */

struct spdk_bdev_io_stat *bdev_alloc_io_stat(bool io_error_stat);
void bdev_free_io_stat(struct spdk_bdev_io_stat *stat);
/*
 * [한국어]
 * bdev_alloc_io_stat / bdev_free_io_stat - per-device I/O 통계 구조 할당·해제
 *
 * @io_error_stat: true면 에러 타입별 카운터 배열까지 포함 (크기 증가)
 * 사용처: bdev 등록 시 1회 할당, 등록 해제 시 free.
 */

enum spdk_bdev_reset_stat_mode;  /* [한국어] 전방선언 — 실제 enum 정의는 spdk/bdev.h에 위치. 이 헤더는 타입 사용만 하므로 전방선언으로 충분 */

typedef void (*bdev_reset_device_stat_cb)(struct spdk_bdev *bdev, void *cb_arg, int rc);
                                 /* [한국어] 통계 리셋 비동기 완료 콜백
                                  *  @rc: 0 성공, 음수 errno 실패 */

void bdev_reset_device_stat(struct spdk_bdev *bdev, enum spdk_bdev_reset_stat_mode mode,
			    bdev_reset_device_stat_cb cb, void *cb_arg);
/*
 * [한국어]
 * bdev_reset_device_stat - bdev 전체 채널의 통계를 리셋 (mode에 따라 total/all/max)
 *
 * 모든 채널을 순회하며 각 채널 스레드에 송신(spdk_thread_send_msg)한 뒤 전부 집계되면 cb 호출. 비동기.
 */

#endif /* SPDK_BDEV_INTERNAL_H */ /* [한국어] include 가드 종료 */
