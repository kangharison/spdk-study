/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Network block device layer
 */

/*
 * [한국어 설명] SPDK NBD(Network Block Device) 공개 API (nbd.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 의 임의의 bdev 를 Linux 커널의 NBD 드라이버(/dev/nbdN)로 노출시키는
 * 공개 API 를 정의한다. NBD 는 1990년대 후반부터 Linux 에 존재해온 전통적인 프로토콜로,
 * 커널 NBD 드라이버가 user-space 서버와 socket(보통 AF_UNIX socketpair) 으로 통신해
 * 블록 I/O 를 위임하는 구조이다. SPDK 는 이 socket pair 의 user-space 끝단에 앉아서
 * 커널이 보내는 NBD 요청 패킷을 받아 자신의 bdev 레이어로 변환·실행하고, 완료되면
 * NBD reply 를 다시 socket 으로 돌려보낸다.
 * 이를 통해 사용자는 SPDK 위에 만든 가상 디스크를 마치 실제 블록 디바이스인 것처럼
 * mkfs/mount/dd 등 표준 Linux 툴로 다룰 수 있다 (예: SPDK NVMe-oF 호스트로 받은 디스크를
 * /dev/nbd0 로 export 후 ext4 마운트). 같은 목적의 ublk(/dev/ublkbN) 모듈에 비해
 * 성능은 떨어지지만 (커널 NBD 는 io_uring 기반이 아니어서 syscall/문맥교환 오버헤드가 큼)
 * 거의 모든 Linux 배포판에서 추가 모듈 없이 즉시 사용 가능한 호환성 우위가 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택에서 NBD 는 "외부 노출 레이어" 에 속한다. 위로는 Linux 사용자/커널이
 * 표준 read(2)/write(2) 로 /dev/nbdN 을 사용하면, 커널 NBD 드라이버가 NBD 프로토콜
 * 패킷을 socket 에 써서 SPDK 프로세스로 보낸다. 아래로는 SPDK 의 spdk_bdev_* API 를
 * 호출해 실제 storage backend(NVMe / aio / lvol / null 등) 로 I/O 를 라우팅한다.
 *
 *    Linux app
 *       |  read(2) / write(2)
 *       v
 *    [Linux kernel NBD driver]   <— /dev/nbdN
 *       |  NBD request (socket)
 *       v
 *    [SPDK nbd subsystem (lib/nbd, this header)]
 *       |  spdk_bdev_readv / writev / unmap
 *       v
 *    [SPDK bdev layer]
 *       |
 *       v
 *    [SPDK bdev module (nvme / aio / lvol / ...)]
 *
 * 실행 컨텍스트는 모두 SPDK reactor 가 소유한 spdk_thread 위의 polled-mode 이며,
 * NBD 요청을 socket 에서 읽어들이는 것도 spdk_poller 로 구현된다 (lib/nbd/nbd.c).
 *
 * === 타 모듈과의 연결 ===
 * - spdk/bdev.h: spdk_bdev_open_ext / spdk_bdev_get_io_channel / spdk_bdev_readv 등을
 *   사용해 실제 I/O 를 backend bdev 에 위임한다. NBD 모듈은 자체 storage 를 갖지 않으며
 *   순수히 NBD 프로토콜 ↔ bdev 변환기 역할만 한다.
 * - Linux kernel <linux/nbd.h>: NBD_SET_SOCK / NBD_SET_BLKSIZE / NBD_DO_IT 등 ioctl 로
 *   /dev/nbdN 디바이스를 active 시킨다 (lib/nbd/nbd.c 가 이 ioctl 을 호출).
 * - spdk/json.h: spdk_nbd_write_config_json 으로 RPC subsystem 의 save_config 시
 *   현재 등록된 NBD 디스크들을 JSON 으로 직렬화한다.
 * - spdk/thread.h: 모든 콜백은 NBD 가 polling 되는 spdk_thread 컨텍스트에서 호출된다.
 * 데이터 흐름은 (kernel) NBD request → socket buffer → SPDK poller → bdev_io 변환 →
 * bdev module 처리 → completion cb → NBD reply 패킷 → socket → kernel 의 단방향 파이프
 * 두 줄로 구성된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_nbd_init / spdk_nbd_fini: NBD 서브시스템 전역 초기화/종료. SPDK app 시작 시
 *   subsystem framework 가 자동 호출.
 * - spdk_nbd_start: bdev 이름과 /dev/nbdX 경로를 받아 NBD 디스크를 등록한다. 비동기로
 *   socketpair 생성 → 커널 ioctl 호출 → polling 시작 후 cb_fn 호출.
 * - spdk_nbd_stop: 진행 중 I/O 를 안전히 마무리하고 NBD 디스크를 해제한다.
 * - spdk_nbd_get_path: 등록된 NBD 디스크의 /dev/nbdN 경로를 반환.
 * - spdk_nbd_disk: 한 NBD-노출 인스턴스를 표현하는 불투명 핸들. 내부적으로 socketpair fd,
 *   poller 핸들, 바인딩된 bdev_desc/io_channel 을 보유 (정의는 lib/nbd 내부 헤더).
 */

#ifndef SPDK_NBD_H_                       /* [한국어] include guard 시작 — 이 헤더가 같은
                                          * 컴파일 유닛에 두 번 펼쳐지면 typedef/구조체
                                          * 중복 선언이 되므로, 매크로 정의 여부로 1회성을
                                          * 강제한다 (모든 SPDK 공개 헤더 공통 패턴). */
#define SPDK_NBD_H_                       /* [한국어] guard 매크로 정의: 두 번째 include
                                          * 부터는 #ifndef 가 거짓이 되어 본문 전체를 건너뛴다. */

#ifdef __cplusplus                        /* [한국어] C++ 컴파일러로 이 헤더를 포함했는지
                                          * 검사. SPDK 본체는 C 로 작성되어 있지만 사용자
                                          * 애플리케이션이 C++ 일 수 있으므로, 그 경우
                                          * 함수 심볼에 C linkage 를 강제해 name-mangling
                                          * 충돌을 막아야 한다. */
extern "C" {                              /* [한국어] extern "C" 블록 시작 — 아래 모든 선언
                                          * (typedef, 함수)이 C linkage 로 export 되어
                                          * SPDK 라이브러리 (.a / .so) 와 ABI 호환된다. */
#endif

struct spdk_bdev;                         /* [한국어] 전방 선언: SPDK bdev 객체 타입.
                                          * 실제 정의는 spdk/bdev.h 에 있지만, 이 헤더의
                                          * 어떤 함수도 spdk_bdev 의 내부 필드를 참조하지
                                          * 않으므로 (spdk_nbd_start 가 bdev_name 문자열로
                                          * 조회) 전방 선언만으로 충분하다. */
struct spdk_nbd_disk;                     /* [한국어] 전방 선언: NBD 디스크 인스턴스 타입.
                                          * 정의는 lib/nbd/nbd_internal.h 또는 nbd.c 내부에
                                          * 숨어 있어 사용자는 핸들(불투명 포인터)로만
                                          * 접근한다. 한 인스턴스 = (한 backend bdev,
                                          * 한 socketpair, 한 /dev/nbdN, 한 poller). */
struct spdk_json_write_ctx;               /* [한국어] 전방 선언: SPDK JSON 출력 컨텍스트.
                                          * spdk/json.h 에 정의되어 있고 RPC save_config
                                          * 경로에서 사용된다. 이 헤더는 단순 포인터로만
                                          * 받으므로 본 헤더에서 spdk/json.h 를 include 할
                                          * 필요가 없다 (헤더 의존성 최소화). */
typedef void (*spdk_nbd_fini_cb)(void *arg);
/* [한국어] NBD 서브시스템 종료 완료 콜백 타입.
 * - arg: spdk_nbd_fini() 호출 시 사용자가 넘긴 cb_arg 가 그대로 돌아옴.
 * 호출 시점: 모든 NBD 디스크가 안전히 정지되고 socket/poller 가 해제된 직후.
 * 호출 스레드: NBD 가 polling 되던 spdk_thread (subsystem 종료 reactor).
 * SPDK 의 비동기 완료 패턴(cb_fn(ctx, ...)) 표준 형태 중 "결과 코드 없는" 단순 통보형. */

/**
 * Initialize the network block device layer.
 *
 * \return 0 on success.
 */
/*
 * [한국어]
 * spdk_nbd_init - SPDK 프로세스 내 NBD 서브시스템을 1회 초기화한다.
 *
 * @return: 0 = 성공. 음수 errno = 실패 (TAILQ 헤드 초기화 실패 등은 거의 발생하지 않음).
 *
 * 이 함수는 SPDK app framework 이 subsystem dependency 그래프(spdk_nbd_subsystem)를
 * 따라 자동으로 호출한다. 사용자가 직접 부를 일은 거의 없고, JSON config 로
 * "subsystem": "nbd" 를 활성화하면 framework 이 호출한다. 내부적으로는 NBD disk 를
 * 추적하는 전역 TAILQ 와 mutex 를 초기화하며, 아직 어떤 /dev/nbdN 도 등록되지 않은
 * 상태로 끝난다. 실제 디스크는 spdk_nbd_start() 별도 호출로 만든다.
 *
 * 호출 체인:
 *   spdk_subsystem_init() → nbd subsystem init cb → spdk_nbd_init()
 */
int spdk_nbd_init(void);

/**
 * Stop and close all the running network block devices.
 *
 * \param cb_fn Callback to be always called.
 * \param cb_arg Passed to cb_fn.
 */
/*
 * [한국어]
 * spdk_nbd_fini - 등록된 모든 NBD 디스크를 정지하고 NBD 서브시스템을 해제한다.
 *
 * @cb_fn: 모든 정리 작업이 끝난 뒤 한 번 호출되는 완료 콜백. 성공/실패와 무관하게
 *         반드시 호출(always called)되므로 호출자는 이 cb 안에서 후속 종료 단계를
 *         이어가면 된다 (subsystem framework 의 next 호출 등).
 * @cb_arg: cb_fn 의 첫 인자로 그대로 전달되는 사용자 컨텍스트.
 *
 * 이 함수는 비동기이다. 등록된 NBD 디스크가 여러 개라면 각각의 socket close, poller
 * 해제, kernel ioctl(NBD_DISCONNECT/NBD_CLEAR_QUE) 호출을 차례로 실행하고, 마지막
 * 디스크의 정리가 끝났을 때 cb_fn 이 호출된다. 진행 중인 bdev I/O 는 자연스럽게
 * 완료를 기다린다 (forced abort 가 아님).
 *
 * 호출 체인:
 *   spdk_subsystem_fini() → nbd subsystem fini cb → spdk_nbd_fini(cb, arg)
 *     → (각 nbd_disk 별 stop) → cb_fn(arg)
 */
void spdk_nbd_fini(spdk_nbd_fini_cb cb_fn, void *cb_arg);

/**
 * Called when an NBD device has been started.
 * On success, rc is assigned 0; On failure, rc is assigned negated errno.
 */
/* [한국어] NBD 디스크 시작 완료 콜백 타입.
 * - cb_arg: spdk_nbd_start() 호출 시 사용자가 넘긴 컨텍스트.
 * - nbd: 성공 시 새로 만들어진 NBD 디스크 핸들 (실패 시 NULL 일 수 있음).
 * - rc: 0 = 성공, 음수 errno = 실패 (예: -ENODEV /dev/nbdN 사용 불가, -EBUSY 이미 사용 중,
 *   bdev_open_ext 실패 시 해당 errno 그대로 전파).
 * 호출 스레드: spdk_nbd_start 를 호출했던 spdk_thread.
 * SPDK 의 "with handle" 비동기 완료 패턴 — 핸들이 콜백 인자로 들어와 호출자가 후속
 * 작업(예: get_path 로 경로 출력)에 즉시 사용 가능하다. */
typedef void (*spdk_nbd_start_cb)(void *cb_arg, struct spdk_nbd_disk *nbd,
				  int rc);

/**
 * Start a network block device backed by the bdev.
 *
 * \param bdev_name Name of bdev exposed as a network block device.
 * \param nbd_path Path to the registered network block device.
 * \param cb_fn Callback to be always called.
 * \param cb_arg Passed to cb_fn.
 */
/*
 * [한국어]
 * spdk_nbd_start - 지정한 bdev 를 /dev/nbdX 로 노출시킨다.
 *
 * @bdev_name: backend 로 사용할 SPDK bdev 의 등록된 이름 (예: "Nvme0n1", "Malloc0").
 *             내부에서 spdk_bdev_get_by_name() 으로 조회 후 spdk_bdev_open_ext() 로 연다.
 * @nbd_path : 사용자에게 노출할 커널 NBD 디바이스 경로 (예: "/dev/nbd0"). 이 디바이스는
 *             modprobe nbd 로 미리 로드되어 있어야 하며, 이미 다른 프로세스가 사용 중이면
 *             -EBUSY.
 * @cb_fn    : 시작 완료 콜백 — 성공/실패 모두 호출됨.
 * @cb_arg   : cb_fn 에 그대로 전달.
 *
 * 동작 단계:
 *   1) bdev 조회 + open + io_channel 생성.
 *   2) socketpair(AF_UNIX, SOCK_STREAM) 생성 — 한 끝은 SPDK, 한 끝은 커널.
 *   3) /dev/nbdN 을 open 후 ioctl(NBD_SET_SOCK), NBD_SET_BLKSIZE, NBD_SET_SIZE_BLOCKS,
 *      NBD_SET_FLAGS 로 디바이스 속성 통보.
 *   4) 별도 worker thread/프로세스가 NBD_DO_IT ioctl 로 커널 측 I/O 루프 가동.
 *   5) SPDK 측 끝단 socket fd 를 spdk_poller 에 등록 → polling 루프에서 NBD request
 *      를 읽어 spdk_bdev_readv/writev/unmap 등으로 변환 후 submit.
 *   6) 모든 단계가 성공하면 cb_fn(arg, nbd, 0), 어느 단계든 실패 시 부분 정리 후
 *      cb_fn(arg, NULL, -errno).
 *
 * 실행 컨텍스트: 호출자는 SPDK reactor 위에 있어야 한다. 비동기로 여러 단계를 거치므로
 * 콜백이 호출될 때까지 핸들 사용 금지.
 *
 * 호출 체인:
 *   사용자/RPC handler → spdk_nbd_start() → (단계 1~5 비동기) → cb_fn(arg, nbd, rc)
 */
void spdk_nbd_start(const char *bdev_name, const char *nbd_path,
		    spdk_nbd_start_cb cb_fn, void *cb_arg);

/**
 * Stop the running network block device safely.
 *
 * \param nbd A pointer to the network block device to stop.
 *
 * \return 0 on success.
 */
/*
 * [한국어]
 * spdk_nbd_stop - 진행 중 I/O 를 마무리하고 NBD 디스크를 안전 종료한다.
 *
 * @nbd: spdk_nbd_start 의 콜백에서 받았거나 spdk_nbd_disk_find_by_nbd_path() 로 얻은 핸들.
 * @return: 0 = 정지 절차 정상 시작. (이 함수가 곧장 동기 정지를 보장하지는 않는다 —
 *          정지 자체는 reactor 에 작업이 큐잉된 뒤 polling 루프에서 진행된다.)
 *
 * 동작:
 *   1) poller 등록 해제로 더 이상 새 NBD request 를 받지 않게 한다.
 *   2) 현재 in-flight 인 bdev_io 가 모두 완료될 때까지 기다린다 (각 io 의 cb 에서
 *      ref count 감소 → 0 도달 시 해제 단계로 진입).
 *   3) ioctl(NBD_DISCONNECT) 후 NBD 측 socket fd close, /dev/nbdN close.
 *   4) bdev_close, io_channel_put, spdk_nbd_disk 구조체 free.
 *
 * 안전 종료가 핵심이다 — 강제 abort 가 아니므로 호출 시점에 큐잉되어 있던 read/write 는
 * 모두 정상 응답을 받고 종료된다.
 *
 * 호출 체인:
 *   사용자/RPC handler → spdk_nbd_stop() → (in-flight 대기) → 자원 해제 → 0 반환.
 */
int spdk_nbd_stop(struct spdk_nbd_disk *nbd);

/**
 * Get the local filesystem path used for the network block device.
 */
/*
 * [한국어]
 * spdk_nbd_get_path - NBD 디스크가 연결된 /dev/nbdN 경로를 반환한다.
 *
 * @nbd: 유효한 NBD 디스크 핸들.
 * @return: 디스크 시작 시 spdk_nbd_start 에 전달된 nbd_path 문자열을 그대로 반환
 *          (lifetime 은 nbd 핸들이 살아 있는 동안 유효). NULL 입력 시 동작은 정의되지
 *          않으므로 호출자가 유효성 검증 책임을 진다.
 *
 * 사용 예: RPC list_nbd_disks 응답 작성, 로깅, 사용자 화면 출력 등 read-only 정보 조회용.
 *
 * 호출 체인:
 *   RPC handler / config_json writer → spdk_nbd_get_path()
 */
const char *spdk_nbd_get_path(struct spdk_nbd_disk *nbd);

/**
 * Write NBD subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 */
/*
 * [한국어]
 * spdk_nbd_write_config_json - 현재 활성 NBD 디스크 목록을 JSON 으로 직렬화한다.
 *
 * @w: 호출자가 spdk_jsonrpc_begin_result 또는 spdk_json_write_begin 으로 만든 출력 컨텍스트.
 *
 * SPDK 의 save_config RPC 는 현재 실행 상태를 JSON 으로 덤프해 후속 부팅 시 그대로
 * 재현 가능하게 한다. 이 함수는 NBD 서브시스템 부분만 채우며, 등록된 모든 nbd_disk 에
 * 대해 한 개씩 다음 형태의 객체를 추가한다:
 *
 *   {
 *     "method": "nbd_start_disk",
 *     "params": { "bdev_name": "...", "nbd_device": "/dev/nbdN" }
 *   }
 *
 * 출력 형식은 RPC 입력 형식과 1:1 대응되어 그대로 다시 적용 가능 (round-trip).
 *
 * 호출 체인:
 *   spdk_subsystem_config_json() → nbd subsystem write_config_json cb → 이 함수
 */
void spdk_nbd_write_config_json(struct spdk_json_write_ctx *w);

#ifdef __cplusplus                        /* [한국어] C++ 사용자에 대한 extern "C" 블록 닫기 */
}
#endif

#endif                                    /* [한국어] include guard 종료 (#ifndef SPDK_NBD_H_) */
