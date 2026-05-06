/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK UBlk(userspace block driver) 공개 API (ublk.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 의 bdev 를 Linux kernel 의 ublk(userspace block driver) 인터페이스
 * 위에서 일반 블록 디바이스(/dev/ublkbN)로 노출시키는 공개 API 를 정의한다.
 * ublk 는 Linux 5.20(2022) 부터 본격 도입된 차세대 user-space block backend 로,
 * io_uring 기반의 명령 큐(IORING_OP_URING_CMD)를 사용해 커널 ↔ 유저스페이스 사이에
 * I/O 패킷을 전달한다. 전통적인 NBD/loopback/tcmu 보다 syscall 횟수가 적고 zero-copy
 * 성격이 강해 SPDK 의 polled-mode 와 결합 시 매우 높은 IOPS/낮은 지연을 달성한다.
 * 이 헤더는 공개 API 표면이 매우 작아 init/fini/config_json 만 노출한다 — 실제 ublk
 * target 생성/제거(ublk_create_target, ublk_start_disk 등)는 RPC layer 또는
 * lib/ublk 내부 헤더(ublk_internal.h)를 통해 호출된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택에서 ublk 는 NBD 와 동일한 "외부 블록 노출" 레이어에 자리한다.
 * 위로는 Linux kernel ublk_drv 가 /dev/ublkbN 을 만들어 일반 사용자에게 표준 블록
 * 디바이스로 보이게 하고, 그 사이의 I/O 위임은 io_uring command-queue 로 SPDK 에 전달된다.
 * 아래로는 SPDK 의 spdk_bdev_* API 를 호출해 storage backend 로 라우팅한다.
 *
 *    Linux app
 *       |  read(2)/write(2)/io_uring
 *       v
 *    [Linux kernel ublk_drv]   <— /dev/ublkbN
 *       |  io_uring URING_CMD (UBLK_IO_FETCH_REQ / UBLK_IO_COMMIT_AND_FETCH_REQ)
 *       v
 *    [SPDK ublk subsystem (lib/ublk, this header)]
 *       |  spdk_bdev_readv / writev / unmap / flush
 *       v
 *    [SPDK bdev layer]
 *       |
 *       v
 *    [SPDK bdev module (nvme / aio / lvol / ...)]
 *
 * 실행 컨텍스트: 한 ublk target 은 특정 spdk_thread 에 thread-affinity 로 묶여
 * polling 된다. io_uring SQE/CQE 처리도 그 스레드에서만 일어나므로 lockless 가 가능하다.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/bdev.h: 실제 I/O 위임. ublk 는 자체 storage 가 없고 NBD 와 마찬가지로
 *   "프로토콜 변환기" 다. ublk request → bdev_io 변환 후 spdk_bdev_readv 등 호출.
 * - Linux kernel ublk_drv (drivers/block/ublk_drv.c): /dev/ublk-controlN 으로 제어
 *   채널을, /dev/ublkbN 으로 데이터 채널을 만든다. SPDK 는 io_uring command 으로 둘 다
 *   조작한다 (UBLK_CMD_ADD_DEV / UBLK_CMD_START_DEV 등).
 * - liburing (linux header <liburing.h>): SPDK 가 직접 사용하지는 않고 자체적으로
 *   io_uring syscall 을 wrapping 한다 (lib/ublk/ublk.c).
 * - spdk/json.h: spdk_ublk_write_config_json 으로 RPC save_config 시 현재 ublk
 *   target/disk 구성을 JSON 으로 직렬화.
 * - spdk/thread.h: 모든 ublk poller 와 콜백은 ublk 가 등록된 spdk_thread 에서 실행.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_ublk_init: SPDK 프로세스 내 ublk 서브시스템 1회 초기화 (subsystem framework
 *   이 자동 호출). 전역 TAILQ/뮤텍스 준비.
 * - spdk_ublk_fini: 모든 ublk target/disk 를 정지하고 io_uring fd 를 close. 비동기.
 * - spdk_ublk_write_config_json: 현재 등록된 ublk target/disk 를 JSON 으로 덤프해
 *   round-trip 가능한 config 를 만든다.
 * - spdk_ublk_fini_cb: 종료 완료 콜백 타입.
 * 추가 API(ublk_create_target / ublk_destroy_target / ublk_start_disk / ublk_stop_disk)는
 * 이 공개 헤더에는 없고 lib/ublk 내부 헤더에서 RPC 핸들러용으로 노출된다.
 */

#ifndef SPDK_UBLK_H_                      /* [한국어] include guard 시작 — 헤더 중복 펼침 시
                                          * typedef 충돌이 일어나지 않도록 한다. SPDK 공개
                                          * 헤더 공통 형식이며, 매크로 이름은 파일 경로를
                                          * 대문자로 변환한 형태(SPDK_UBLK_H_)를 따른다. */
#define SPDK_UBLK_H_                      /* [한국어] guard 매크로 정의 — 두 번째 include 부터
                                          * 본문이 통째로 건너뛰어진다. */

#include "spdk/json.h"                    /* [한국어] spdk_json_write_ctx 정의를 위해 include.
                                          * spdk_ublk_write_config_json 의 인자 타입이 완전한
                                          * 정의를 요구하지 않지만(포인터로만 사용), 사용자
                                          * 편의를 위해 SPDK 는 관습적으로 JSON 출력 함수가
                                          * 있는 헤더에서 spdk/json.h 를 그대로 노출한다. */

#ifdef __cplusplus                        /* [한국어] C++ 사용자 보호: 함수 심볼이 C linkage 로
                                          * 노출되도록 extern "C" 블록을 연다. SPDK 본체는 C 다. */
extern "C" {
#endif

typedef void (*spdk_ublk_fini_cb)(void *arg);
/* [한국어] ublk 서브시스템 종료 완료 콜백 타입.
 * - arg: spdk_ublk_fini() 호출 시 사용자가 넘긴 cb_arg 가 그대로 돌아옴.
 * 호출 시점: 모든 ublk target/disk 정지 + io_uring fd close + 자료구조 해제 직후.
 * 호출 스레드: ublk 가 polling 되던 spdk_thread 또는 fini 가 dispatch 된 thread.
 * 결과 코드를 받지 않는 단순 통보형(spdk_subsystem_fini 가 항상 호출됨을 보장하는 패턴). */

/**
 * Initialize the ublk library.
 */
/*
 * [한국어]
 * spdk_ublk_init - SPDK 프로세스 내 ublk 서브시스템을 1회 초기화한다.
 *
 * @return: 없음 (void). 실패 시 내부적으로 SPDK_ERRLOG 만 남기고 후속 ublk_create_target
 *          호출이 실패하게 된다 — 이 init 자체는 자료구조 준비만 하므로 "실패 가능 경로"가
 *          극히 적다.
 *
 * SPDK app framework 의 subsystem 종속성 그래프(spdk_ublk_subsystem)를 통해 자동 호출되며,
 * 사용자가 직접 부르는 일은 거의 없다. 내부적으로는 ublk_target/ublk_disk 들을 추적할
 * 전역 TAILQ 와 mutex, 그리고 io_uring 셋업을 위한 thread-local 상태 슬롯을 준비한다.
 * 이 시점에는 아직 어떤 /dev/ublkbN 도 존재하지 않는다 — 실제 디바이스는 RPC
 * (ublk_create_target → ublk_start_disk)로 만든다.
 *
 * 호출 체인:
 *   spdk_subsystem_init() → ublk subsystem init cb → spdk_ublk_init()
 */
void spdk_ublk_init(void);

/**
 * Stop the ublk layer and close all running ublk block devices.
 *
 * \param cb_fn Callback to be always called.
 * \param cb_arg Passed to cb_fn.
 * \return 0 on success.
 */
/*
 * [한국어]
 * spdk_ublk_fini - 모든 ublk target/disk 를 정지하고 ublk 서브시스템을 해제한다.
 *
 * @cb_fn: 모든 정리가 끝난 뒤 한 번 호출되는 완료 콜백. 항상 호출되므로(error path 포함)
 *         호출자는 cb 안에서 후속 종료(예: 다음 subsystem fini 진행)를 이어가면 된다.
 * @cb_arg: cb_fn 의 첫 인자로 그대로 전달되는 사용자 컨텍스트.
 * @return: 0 = 정지 절차 정상 시작 (실제 정지는 비동기). 음수 errno 시 cb_fn 미호출.
 *
 * 동작 단계 (비동기 chain):
 *   1) 등록된 ublk_disk 각각에 대해 ublk_stop_disk 를 발사 — io_uring 으로
 *      UBLK_CMD_STOP_DEV / UBLK_CMD_DEL_DEV 명령을 커널에 보낸다.
 *   2) 진행 중 io_uring CQE 가 모두 회수될 때까지 기다린다 (in-flight bdev_io 자연 완료).
 *   3) 각 ublk_target 의 io_uring fd close, spdk_thread 에 등록된 poller 해제.
 *   4) 마지막 disk 정리가 끝났을 때 cb_fn(arg) 호출.
 *
 * 호출 체인:
 *   spdk_subsystem_fini() → ublk subsystem fini cb → spdk_ublk_fini(cb, arg)
 *     → (ublk_disk 별 stop) → cb_fn(arg)
 */
int spdk_ublk_fini(spdk_ublk_fini_cb cb_fn, void *cb_arg);

/**
 * Write UBLK subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 */
/*
 * [한국어]
 * spdk_ublk_write_config_json - 현재 활성 ublk target/disk 구성을 JSON 으로 직렬화한다.
 *
 * @w: 호출자가 spdk_jsonrpc_begin_result 등으로 만든 JSON 출력 컨텍스트.
 *
 * SPDK save_config RPC 의 ublk 파트를 채운다. 현재 등록된 모든 ublk target/disk 에
 * 대해 다음과 같은 객체들을 출력한다 (단순화):
 *   { "method": "ublk_create_target", "params": { "cpumask": "0xF", ... } }
 *   { "method": "ublk_start_disk", "params": { "bdev_name": "...", "ublk_id": N, ... } }
 * 출력 형식은 RPC 입력 스키마와 1:1 대응되므로 round-trip(save → restore) 이 가능하다.
 *
 * 호출 체인:
 *   spdk_subsystem_config_json() → ublk subsystem write_config_json cb → 이 함수
 */
void spdk_ublk_write_config_json(struct spdk_json_write_ctx *w);

#ifdef __cplusplus                        /* [한국어] C++ extern "C" 블록 닫기 */
}
#endif

#endif /* SPDK_UBLK_H_ */                 /* [한국어] include guard 종료 */
