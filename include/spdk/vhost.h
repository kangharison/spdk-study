/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/**
 *  \file
 *  SPDK vhost
 */

/*
 * [한국어 설명] SPDK vhost-user target 공개 API (vhost.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 프로세스가 vhost-user 프로토콜의 "slave/target" 측이 되어
 * QEMU/CloudHypervisor 같은 가상화 게스트의 디스크를 user-space 에서 제공하기 위한
 * 공개 API 를 정의한다. vhost-user 는 원래 커널 vhost 인터페이스의 user-space 버전으로,
 * AF_UNIX 소켓을 통해 QEMU(master) 와 SPDK(slave) 가 다음을 협상한다: 게스트 메모리
 * 영역(memory region) 매핑, virtqueue base/size/kick/call eventfd, feature negotiation,
 * 그리고 packed/split virtqueue 모드 등. 협상이 끝나면 SPDK 는 게스트의 virtqueue 와
 * payload 메모리를 자기 주소공간에 직접 mmap 해 zero-copy 로 처리한다 — 이것이 일반
 * paravirt(virtio) 디스크에 비해 SPDK vhost 가 훨씬 빠른 핵심 이유이다.
 * 본 헤더는 두 가지 vdev 타입을 노출한다: vhost-blk(virtio-blk emulation, 단일 LUN)와
 * vhost-scsi(virtio-scsi emulation, 한 컨트롤러 당 다중 SCSI target/LUN). 또한 코어
 * 글로벌 함수 (init/fini/lock/find/coalescing) 와 controller 생성/삭제 헬퍼들을 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 외부 노출 레이어 중 "VM-facing" 경로에 해당한다. NBD/ublk 가 같은 호스트의
 * Linux kernel 을 통해 일반 블록 디바이스를 만든다면, vhost 는 같은 호스트에서 돌아가는
 * 가상머신(QEMU 등)에게 PCI virtio 디바이스로 디스크를 노출한다. 호스트 커널은
 * I/O 경로에 거의 개입하지 않고 (kick eventfd 하나만 통과), QEMU 도 control-plane 만
 * 담당한다. SPDK 는 게스트 가상 메모리 → 호스트 가상 메모리 매핑을 직접 들고 있어
 * virtqueue desc 를 따라 게스트 buffer 까지 한 번에 도달한다.
 *
 *    [Guest VM]
 *       virtio-blk / virtio-scsi driver  (Linux kernel in guest)
 *           |
 *           v   (virtqueue + shared memory)
 *       [QEMU vhost-user master]   <— control plane (eventfd, memory regions)
 *           |  AF_UNIX socket (socket_base_dir/<vdev_name>)
 *           v
 *    [SPDK vhost subsystem (lib/vhost, this header)]
 *       |  spdk_bdev_readv / writev / unmap / flush
 *       v
 *    [SPDK bdev layer]
 *       |
 *       v
 *    [SPDK bdev module (nvme / aio / lvol / ...)]
 *
 * 실행 컨텍스트: 한 vhost session(=게스트 1대의 한 virtio 디바이스)은 cpumask 로 지정된
 * 특정 spdk_thread 에 thread-affinity 로 배정되며, 그 thread 위의 spdk_poller 가
 * virtqueue 를 polling 한다. session 별 작업은 모두 그 한 thread 에서만 일어나므로
 * I/O 경로는 lockless 다. 단, vdev 라이프사이클(생성/제거)은 전역 mutex(spdk_vhost_lock)로
 * 보호된다.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/bdev.h: 모든 vhost-blk / vhost-scsi I/O 가 결국 spdk_bdev_* API 로 위임됨.
 * - spdk/scsi.h: vhost-scsi 가 SCSI target/LUN 추상화를 사용. spdk_scsi_dev 가 스크림 단위.
 * - spdk/cpuset.h: 각 vdev 의 spdk_thread 배치를 위한 CPU mask 표현.
 * - spdk/json.h: scsi_config_json / blk_config_json 으로 save_config 시 직렬화.
 * - spdk/thread.h: vhost session 별 spdk_thread 배정 / spdk_thread_send_msg 로 cross-thread
 *   요청 처리 (lock 보호 vdev 핸들 접근).
 * - rte_vhost (DPDK lib): 실제 vhost-user 프로토콜 파싱과 socket 처리는 DPDK 의
 *   librte_vhost 가 담당하며, SPDK 는 그 위의 ops 콜백을 채워 SCSI/blk 의미를 입힌다.
 * - QEMU/외부 master: AF_UNIX 소켓으로 통신. 소켓 경로는 spdk_vhost_set_socket_path 로
 *   설정한 디렉토리 + vdev name 으로 결정.
 * 데이터 흐름은 두 갈래: (1) control-plane: master 의 vhost-user message 가 socket 으로
 * 들어오면 rte_vhost 가 SPDK 콜백을 호출해 vdev 상태를 갱신하고, (2) data-plane: 게스트가
 * virtqueue 에 desc 를 추가하고 kick eventfd 를 쏘면, SPDK poller 가 desc 를 디큐해
 * spdk_bdev_io 로 변환·발사하고, 완료 시 used ring 에 기록 + call eventfd 로 인터럽트.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_vhost_set_socket_path: vhost socket 디렉토리 경로 설정 (init 전에 1회).
 * - spdk_vhost_blk_init / blk_fini / scsi_init / scsi_fini: 각각 vhost-blk / vhost-scsi
 *   서브시스템의 비동기 초기화·종료.
 * - spdk_vhost_dev_find / spdk_vhost_dev_next: 등록된 vdev 들을 이름/순회로 조회.
 * - spdk_vhost_lock / trylock / unlock: 전역 vdev mutex (라이프사이클 보호).
 * - spdk_vhost_blk_construct: 단일 bdev 를 virtio-blk 로 노출 (vhost_user_blk transport 등).
 * - spdk_vhost_scsi_dev_construct / construct_no_start: 빈 vhost-scsi 컨트롤러 생성.
 * - spdk_vhost_scsi_dev_add_tgt / get_tgt / remove_tgt: SCSI target slot 관리.
 * - spdk_vhost_set_coalescing / get_coalescing: 인터럽트 coalescing(IOPS 임계값 기반)
 *   설정 — 게스트 IRQ 폭주를 줄여 성능 향상.
 * - spdk_vhost_dev_remove: vdev 제거 (소켓에 active connection 이 없어야 함).
 * - spdk_vhost_dev: 모든 vdev 의 불투명 핸들 (실제 정의는 lib/vhost 내부).
 * - spdk_vhost_init_cb / fini_cb / event_fn: 비동기 콜백 타입들.
 */

#ifndef SPDK_VHOST_H                      /* [한국어] include guard 시작 — 다중 include 시
                                          * 함수/typedef 중복 선언 방지. */
#define SPDK_VHOST_H                      /* [한국어] guard 매크로 정의 — 두 번째 include 부터
                                          * 본문이 통째로 건너뛰어짐. */

#include "spdk/stdinc.h"                  /* [한국어] SPDK 표준 헤더 묶음 — stdint, stdbool,
                                          * stdio, stdlib, string 등 자주 쓰이는 C stdlib 을
                                          * 한 번에 가져온다 (uint32_t / size_t / bool 등 사용). */

#include "spdk/cpuset.h"                  /* [한국어] struct spdk_cpuset 정의를 가져옴 —
                                          * spdk_vhost_dev_get_cpumask() 의 반환 타입.
                                          * SPDK 의 비트맵 기반 CPU 마스크 추상화. */
#include "spdk/json.h"                    /* [한국어] struct spdk_json_write_ctx / spdk_json_val
                                          * 정의를 가져옴 — config_json / vhost_blk_construct
                                          * 의 params 인자 타입. */
#include "spdk/thread.h"                  /* [한국어] SPDK thread 모델 헤더 — vhost session 의
                                          * thread affinity / send_msg 등을 사용하는 호출자가
                                          * 함께 include 하는 것이 일반적이라 편의 차원에서 노출. */

#ifdef __cplusplus                        /* [한국어] C++ 컴파일러 사용 시 함수 심볼을 C linkage
                                          * 로 export — name-mangling 충돌 방지. */
extern "C" {
#endif

/**
 * Callback for spdk_vhost_blk|scsi_init().
 *
 * \param rc 0 on success, negative errno on failure
 */
/* [한국어] vhost-blk / vhost-scsi 서브시스템 초기화 완료 콜백 타입.
 * - rc: 0 = 성공, 음수 errno = 실패 (소켓 디렉토리 접근 불가, rte_vhost 등록 실패 등).
 * 호출 시점: rte_vhost 등록 + 내부 자료구조 준비가 모두 끝난 직후.
 * 호출 스레드: subsystem framework 이 init 을 발사한 spdk_thread.
 * SPDK subsystem framework 은 이 cb 호출을 받고서야 다음 subsystem 으로 진행한다. */
typedef void (*spdk_vhost_init_cb)(int rc);

/** Callback for spdk_vhost_blk|scsi_fini(). */
/* [한국어] vhost-blk / vhost-scsi 서브시스템 종료 완료 콜백 타입.
 * - 인자/반환값 없음 — 단순 통보형. (spdk_subsystem_fini 가 항상 호출 보장하는 패턴.)
 * 호출 시점: 모든 vdev 제거 + 소켓 unlink + rte_vhost cleanup 완료 직후. */
typedef void (*spdk_vhost_fini_cb)(void);

/**
 * Set the path to the directory where vhost sockets will be created.
 *
 * This function must be called before spdk_vhost_init().
 *
 * \param basename Path to vhost socket directory
 *
 * \return 0 on success, negative errno on error.
 */
/*
 * [한국어]
 * spdk_vhost_set_socket_path - 모든 vhost-user 소켓이 만들어질 베이스 디렉토리 설정.
 *
 * @basename: 절대 경로 디렉토리 (예: "/var/tmp"). 각 vdev 의 소켓은 <basename>/<vdev_name>
 *            형태로 생성된다. 디렉토리는 미리 존재해야 하며 SPDK 프로세스가 쓰기 권한을
 *            가져야 한다.
 * @return: 0 = 성공. 음수 errno (예: -ENAMETOOLONG, -EINVAL) = 실패.
 *
 * 반드시 spdk_vhost_blk_init / spdk_vhost_scsi_init 보다 먼저 호출해야 한다 — init 단계에서
 * basename 이 캡처되어 이후 모든 controller 생성에 사용되기 때문. SPDK app 시작 시 JSON
 * config 의 vhost subsystem 의 "socket_path" 옵션이 자동으로 이 함수를 호출한다.
 *
 * 호출 체인:
 *   사용자/RPC handler → spdk_vhost_set_socket_path(basename) → 내부 g_vhost_user_dev_dirname
 *   설정.
 */
int spdk_vhost_set_socket_path(const char *basename);

/**
 * Init vhost environment.
 *
 * \param init_cb Function to be called when the initialization is complete.
 */
/*
 * [한국어]
 * spdk_vhost_scsi_init - vhost-scsi 서브시스템 비동기 초기화.
 *
 * @init_cb: 초기화 완료 시 한 번 호출. rc=0 성공, rc<0 실패.
 *
 * SPDK app framework 이 vhost subsystem dependency 처리를 통해 자동 호출하며,
 * 사용자가 직접 호출하는 일은 거의 없다. 내부적으로 vhost-scsi 의 rte_vhost 콜백
 * 테이블을 등록하고, 전역 vdev TAILQ 슬롯을 준비한다. 이 시점 이후 vhost_scsi_dev_construct
 * 가 가능하다.
 *
 * 호출 체인:
 *   spdk_subsystem_init() → vhost_scsi subsystem init cb → spdk_vhost_scsi_init(cb)
 *     → 비동기 setup → cb(rc)
 */
void spdk_vhost_scsi_init(spdk_vhost_init_cb init_cb);

/**
 * Clean up the environment of vhost.
 *
 * \param fini_cb Function to be called when the cleanup is complete.
 */
/*
 * [한국어]
 * spdk_vhost_scsi_fini - vhost-scsi 서브시스템 비동기 종료.
 *
 * @fini_cb: 모든 vhost-scsi vdev 가 제거되고 cleanup 이 끝난 뒤 한 번 호출.
 *
 * 등록된 모든 vhost-scsi controller 에 대해 dev_remove 를 차례로 발사하고, in-flight I/O 가
 * 정리된 뒤 fini_cb 를 호출한다. spdk_vhost_dev_remove 와 달리 이 함수는 active connection
 * 여부를 따지지 않고 강제 종료에 가깝다 (소켓 닫기, 게스트는 IO 에러를 보게 됨).
 *
 * 호출 체인:
 *   spdk_subsystem_fini() → vhost_scsi subsystem fini cb → spdk_vhost_scsi_fini(cb)
 *     → 비동기 정리 → cb()
 */
void spdk_vhost_scsi_fini(spdk_vhost_fini_cb fini_cb);

/**
 * Write vhost subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 */
/*
 * [한국어]
 * spdk_vhost_scsi_config_json - 등록된 vhost-scsi controller 들을 JSON 으로 직렬화.
 *
 * @w: JSON 출력 컨텍스트 (호출자가 begin_result 등으로 시작).
 *
 * save_config RPC 의 vhost-scsi 부분을 채운다. 각 controller 에 대해
 *   { "method": "vhost_create_scsi_controller", "params": { "ctrlr": "...", "cpumask": "..." } }
 * 와 controller 별 attached target 마다
 *   { "method": "vhost_scsi_controller_add_target", "params": { ... } }
 * 를 출력해 round-trip(save → restore) 가능하게 한다.
 *
 * 호출 체인:
 *   spdk_subsystem_config_json() → vhost_scsi config_json cb → 이 함수
 */
void spdk_vhost_scsi_config_json(struct spdk_json_write_ctx *w);

/**
 * Init vhost environment.
 *
 * \param init_cb Function to be called when the initialization is complete.
 */
/*
 * [한국어]
 * spdk_vhost_blk_init - vhost-blk 서브시스템 비동기 초기화.
 *
 * @init_cb: 초기화 완료 시 호출 (rc=0 성공, 음수 실패).
 *
 * 동작은 spdk_vhost_scsi_init 과 동일한 패턴 — vhost-blk 용 rte_vhost 콜백 등록 +
 * 전역 자료구조 준비. SPDK app framework 의 subsystem 그래프가 자동 호출한다.
 *
 * 호출 체인:
 *   spdk_subsystem_init() → vhost_blk subsystem init cb → spdk_vhost_blk_init(cb)
 *     → 비동기 setup → cb(rc)
 */
void spdk_vhost_blk_init(spdk_vhost_init_cb init_cb);

/**
 * Clean up the environment of vhost.
 *
 * \param fini_cb Function to be called when the cleanup is complete.
 */
/*
 * [한국어]
 * spdk_vhost_blk_fini - vhost-blk 서브시스템 비동기 종료.
 *
 * @fini_cb: 모든 vhost-blk vdev 가 정리된 뒤 한 번 호출.
 *
 * 등록된 vhost-blk controller 들을 차례로 제거하고, 게스트 측 socket 을 닫는다.
 * 진행 중 bdev_io 는 자연 완료를 기다린다.
 *
 * 호출 체인:
 *   spdk_subsystem_fini() → vhost_blk subsystem fini cb → spdk_vhost_blk_fini(cb)
 */
void spdk_vhost_blk_fini(spdk_vhost_fini_cb fini_cb);

/**
 * Write vhost subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 */
/*
 * [한국어]
 * spdk_vhost_blk_config_json - 등록된 vhost-blk controller 들을 JSON 으로 직렬화.
 *
 * @w: JSON 출력 컨텍스트.
 *
 * 각 vhost-blk vdev 에 대해
 *   { "method": "vhost_create_blk_controller", "params": { "ctrlr": "...", "dev_name": "...",
 *     "cpumask": "...", "transport": "vhost_user_blk", "readonly": ..., "packed_ring": ... } }
 * 를 출력. round-trip 가능하게 함.
 *
 * 호출 체인:
 *   spdk_subsystem_config_json() → vhost_blk config_json cb → 이 함수
 */
void spdk_vhost_blk_config_json(struct spdk_json_write_ctx *w);

/**
 * Deinit vhost application. This is called once by SPDK app layer.
 */
/*
 * [한국어]
 * spdk_vhost_shutdown_cb - SPDK app 종료 시 vhost 강제 정리.
 *
 * @return: 없음. SPDK app framework 의 shutdown hook 으로 등록되어, 정상 fini 흐름이
 *          끝나지 못한 경우(SIGTERM 등) 한 번 호출된다. 모든 active session 의 socket close
 *          + lockless 데이터 영역 free 등 best-effort 정리만 수행.
 *
 * 호출 체인:
 *   SPDK app shutdown 경로 → spdk_vhost_shutdown_cb()
 */
void spdk_vhost_shutdown_cb(void);

/**
 * SPDK vhost device (vdev).  An equivalent of Virtio device.
 * Both virtio-blk and virtio-scsi devices are represented by this
 * struct. For virtio-scsi a single vhost device (also called SCSI
 * controller) may contain multiple SCSI targets (devices), each of
 * which may contain multiple logical units (SCSI LUNs). For now
 * only one LUN per target is available.
 *
 * All vdev-changing functions operate directly on this object.
 * Note that \c spdk_vhost_dev cannot be acquired. This object is
 * only accessible as a callback parameter via \c
 * spdk_vhost_call_external_event and it's derivatives. This ensures
 * that all access to the vdev is piped through a single,
 * thread-safe API.
 */
struct spdk_vhost_dev;
/* [한국어] vhost vdev (가상 virtio 디바이스) 의 불투명 핸들 타입.
 * 정의는 lib/vhost/vhost_internal.h 에 숨어 있고 사용자는 포인터로만 접근한다.
 * 한 인스턴스는 다음을 보유한다: 이름(=소켓 파일명), cpumask(=배정된 spdk_thread),
 * backend bdev_desc / io_channel, virtqueue 메타데이터, rte_vhost session 식별자,
 * coalescing 파라미터, vhost-blk 또는 vhost-scsi 별 추가 상태.
 * 라이프사이클: blk_construct / scsi_dev_construct → 사용 → dev_remove.
 * 참고: 이 객체는 acquire/refcount 패턴이 아니다 — 외부에서 접근하려면 항상
 * spdk_vhost_call_external_event 같은 thread-safe wrapper 를 통해야 한다 (또는 본 헤더의
 * 콜백 인자로 받은 포인터 한정 사용). */

/**
 * Lock the global vhost mutex synchronizing all the vhost device accesses.
 */
/*
 * [한국어]
 * spdk_vhost_lock - vdev TAILQ 보호 전역 mutex 를 잠근다.
 *
 * vhost vdev 라이프사이클(추가/삭제/이름조회)은 단일 mutex 로 직렬화된다 (data-plane I/O
 * 는 thread-affinity 라 lockless 지만 control-plane 은 cross-thread 호출이 가능). 이 함수는
 * 블로킹 lock — 이미 다른 thread 가 잡고 있으면 풀릴 때까지 대기. spdk_thread context 에서
 * 호출하면 reactor 가 멈출 수 있으므로 반드시 짧게 잡고 풀어야 한다.
 *
 * 호출 체인:
 *   RPC handler / 외부 controller → spdk_vhost_lock() → vdev_find/next 호출 → unlock()
 */
void spdk_vhost_lock(void);

/**
 * Lock the global vhost mutex synchronizing all the vhost device accesses.
 *
 * \return 0 if the mutex could be locked immediately, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_vhost_trylock - vdev mutex 비차단 시도.
 *
 * @return: 0 = 즉시 잠금 성공. 음수 errno (보통 -EBUSY) = 다른 thread 보유 중.
 *
 * spdk_thread reactor 안에서 reactor 가 멈추면 안 되는 경로(예: I/O poller 내부)에서
 * 사용한다. 실패 시 호출자가 작업을 message queue 로 deferral 해야 한다.
 *
 * 호출 체인:
 *   poller / I/O fast path → spdk_vhost_trylock() (실패하면 spdk_thread_send_msg 로 재시도)
 */
int spdk_vhost_trylock(void);

/**
 * Unlock the global vhost mutex.
 */
/*
 * [한국어]
 * spdk_vhost_unlock - vdev mutex 해제.
 *
 * spdk_vhost_lock / trylock 의 짝. 잠그지 않고 unlock 호출 시 동작 미정의이므로 호출자가
 * lock/unlock 짝맞춤 책임을 진다.
 *
 * 호출 체인:
 *   spdk_vhost_lock() / trylock() … 짧은 critical section … spdk_vhost_unlock()
 */
void spdk_vhost_unlock(void);

/**
 * Find a vhost device by name.
 *
 * \return vhost device or NULL
 */
/*
 * [한국어]
 * spdk_vhost_dev_find - 이름으로 등록된 vdev 를 찾는다.
 *
 * @return: 일치하는 vdev 핸들 또는 NULL (없을 때).
 *
 * 호출자가 spdk_vhost_lock 을 잡고 있어야 안전하다 — 잠그지 않으면 동시 dev_remove 에 의해
 * 반환된 포인터가 dangling 이 될 수 있다. 비교는 vdev->name == 인자 문자열 (대소문자 구분).
 *
 * 호출 체인:
 *   RPC vhost_get_controllers → spdk_vhost_lock → spdk_vhost_dev_find(name) → ... → unlock
 */
struct spdk_vhost_dev *spdk_vhost_dev_find(const char *name);

/**
 * Get the next vhost device. If there's no more devices to iterate
 * through, NULL will be returned.
 *
 * \param vdev vhost device. If NULL, this function will return the
 * very first device.
 * \return vdev vhost device or NULL
 */
/*
 * [한국어]
 * spdk_vhost_dev_next - vdev TAILQ 순회용 헬퍼.
 *
 * @vdev: 현재 순회 위치. NULL 이면 첫 vdev 부터 시작.
 * @return: 다음 vdev 또는 NULL (끝).
 *
 * for (vdev = spdk_vhost_dev_next(NULL); vdev; vdev = spdk_vhost_dev_next(vdev)) 패턴.
 * 호출자가 spdk_vhost_lock 을 보유 중이어야 안전. 순회 중 add/remove 가 일어나지 않도록
 * lock 안에서만 사용.
 *
 * 호출 체인:
 *   RPC list / config_json → spdk_vhost_lock() → 반복 dev_next() → unlock()
 */
struct spdk_vhost_dev *spdk_vhost_dev_next(struct spdk_vhost_dev *vdev);

/**
 * Synchronized vhost event used for user callbacks.
 *
 * \param vdev vhost device.
 * \param arg user-provided parameter.
 *
 * \return 0 on success, -1 on failure.
 */
/* [한국어] vhost 이벤트 콜백 타입 (spdk_vhost_scsi_dev_remove_tgt 등에서 사용).
 * - vdev: 이벤트 대상 vhost vdev. cb 호출 시점에 lock 이 잡힌 상태로 들어옴.
 * - arg: 호출자가 cb 등록 시 넘긴 사용자 컨텍스트.
 * - return: 0 = 성공, -1 = 실패. SPDK 내부에서 retry/abort 결정에 사용.
 * 호출 스레드: vdev 가 배정된 spdk_thread (data-plane 과 같은 스레드). */
typedef int (*spdk_vhost_event_fn)(struct spdk_vhost_dev *vdev, void *arg);

/**
 * Get the name of the vhost device.  This is equal to the filename
 * of socket file. The name is constant throughout the lifetime of
 * a vdev.
 *
 * \param vdev vhost device.
 *
 * \return name of the vdev.
 */
/*
 * [한국어]
 * spdk_vhost_dev_get_name - vdev 의 이름 (= 소켓 파일명) 을 반환.
 *
 * @vdev: 유효한 vdev 핸들 (NULL 금지).
 * @return: 내부 vdev->name 의 const 포인터. lifetime 은 vdev 와 동일.
 *
 * 이름은 construct 시 고정되며 변경 불가 — 소켓 파일명에 포함되기 때문. RPC 응답이나 로깅에
 * 사용된다. 잠금 없이 호출 가능 (이름 자체는 const 처럼 다뤄짐).
 */
const char *spdk_vhost_dev_get_name(struct spdk_vhost_dev *vdev);

/**
 * Get cpuset of the vhost device.  The cpuset is constant throughout the lifetime
 * of a vdev. It is a subset of SPDK app cpuset vhost was started with.
 *
 * \param vdev vhost device.
 *
 * \return cpuset of the vdev.
 */
/*
 * [한국어]
 * spdk_vhost_dev_get_cpumask - vdev 가 배정된 spdk_thread 의 CPU 마스크를 반환.
 *
 * @vdev: 유효한 vdev 핸들.
 * @return: const struct spdk_cpuset 포인터. lifetime = vdev. 변경 금지.
 *
 * 이 마스크는 SPDK app 의 전체 cpumask 의 부분집합이다. construct 시 사용자가 지정한
 * cpumask 인자(예: "0xF" → CPU 0,1,2,3) 가 반영되며, 그 중 하나의 CPU 위 spdk_thread 에
 * 데이터 경로가 고정된다.
 *
 * 호출 체인:
 *   RPC list / 모니터링 도구 → spdk_vhost_dev_get_cpumask(vdev) → cpuset 변환/출력
 */
const struct spdk_cpuset *spdk_vhost_dev_get_cpumask(struct spdk_vhost_dev *vdev);

/**
 * By default, events are generated when asked, but for high queue depth and
 * high IOPS this prove to be inefficient both for guest kernel that have to
 * handle a lot more IO completions and for SPDK vhost that need to make more
 * syscalls. If enabled, limit amount of events (IRQs) sent to initiator by SPDK
 * vhost effectively coalescing couple of completions. This of course introduce
 * IO latency penalty proportional to event delay time.
 *
 * Actual events delay time when is calculated according to below formula:
 * if (delay_base == 0 || IOPS < iops_threshold) {
 *   delay = 0;
 * } else if (IOPS < iops_threshold) {
 *   delay = delay_base * (iops - iops_threshold) / iops_threshold;
 * }
 *
 * \param vdev vhost device.
 * \param delay_base_us Base delay time in microseconds. If 0, coalescing is disabled.
 * \param iops_threshold IOPS threshold when coalescing is activated.
 */
/*
 * [한국어]
 * spdk_vhost_set_coalescing - vdev 별 인터럽트 coalescing 정책 설정.
 *
 * @vdev: 유효한 vdev 핸들.
 * @delay_base_us: coalescing 기본 지연 (μs). 0 이면 coalescing 비활성 (모든 완료에 대해 즉시
 *                 call eventfd 발사).
 * @iops_threshold: 이 IOPS 임계값을 넘기면 coalescing 활성. 임계 미만이면 즉시 통보.
 * @return: 0 = 성공, 음수 errno = 실패 (예: 잘못된 vdev 타입).
 *
 * 게스트 측 IRQ 폭주를 줄여 게스트 CPU 부하 + SPDK 의 eventfd write syscall 횟수를 모두
 * 절감한다. 트레이드오프: I/O latency 가 delay 만큼 증가.
 *
 * 호출 체인:
 *   RPC vhost_controller_set_coalescing → spdk_vhost_lock → 이 함수 → unlock
 */
int spdk_vhost_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			      uint32_t iops_threshold);

/**
 * Get coalescing parameters.
 *
 * \see spdk_vhost_set_coalescing
 *
 * \param vdev vhost device.
 * \param delay_base_us Optional pointer to store base delay time.
 * \param iops_threshold Optional pointer to store IOPS threshold.
 */
/*
 * [한국어]
 * spdk_vhost_get_coalescing - 현재 vdev 의 coalescing 파라미터를 읽는다.
 *
 * @vdev: 유효한 vdev 핸들.
 * @delay_base_us: NULL 가능. NULL 아니면 *delay_base_us 에 현재 지연(μs)을 저장.
 * @iops_threshold: NULL 가능. NULL 아니면 *iops_threshold 에 현재 임계값을 저장.
 *
 * 둘 다 NULL 호출 시 사실상 no-op (검증용). 짝맞춤 setter 는 spdk_vhost_set_coalescing.
 *
 * 호출 체인:
 *   RPC config_json / 모니터링 → spdk_vhost_get_coalescing(vdev, &a, &b) → 출력
 */
void spdk_vhost_get_coalescing(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			       uint32_t *iops_threshold);

/**
 * Construct an empty vhost SCSI device.  This will create a
 * Unix domain socket together with a vhost-user slave server waiting
 * for a connection on this socket. Creating the vdev does not
 * start any I/O pollers and does not hog the CPU. I/O processing
 * starts after receiving proper message on the created socket.
 * See QEMU's vhost-user documentation for details.
 * All physical devices have to be separately attached to this
 * vdev via \c spdk_vhost_scsi_dev_add_tgt().
 *
 * This function is thread-safe.
 *
 * \param name name of the vhost device. The name will also be used
 * for socket name, which is exactly \c socket_base_dir/name
 * \param cpumask string containing cpumask in hex. The leading *0x*
 * is allowed but not required. The mask itself can be constructed as:
 * ((1 << cpu0) | (1 << cpu1) | ... | (1 << cpuN)).
 *
 * \return 0 on success, negative errno on error.
 */
/*
 * [한국어]
 * spdk_vhost_scsi_dev_construct - 빈 vhost-scsi 컨트롤러 (PCI virtio-scsi) 생성.
 *
 * @name: vdev 이름 = 소켓 파일명. 디렉토리는 spdk_vhost_set_socket_path 로 설정된 base.
 * @cpumask: hex 문자열로 표현된 CPU 마스크 (예: "0x3" → CPU 0,1). 이 mask 의 한 CPU 위
 *           spdk_thread 가 data-plane 을 polling.
 * @return: 0 = 성공, 음수 errno = 실패 (이름 중복 -EEXIST, mask 무효 -EINVAL 등).
 *
 * 동작:
 *   1) AF_UNIX socket(<base>/<name>) 생성 + listen.
 *   2) rte_vhost 에 콜백 등록 (게스트가 connect 하면 협상 시작).
 *   3) vdev 객체를 전역 TAILQ 에 등록.
 *   4) 즉시 반환 — 아직 어떤 SCSI target 도 없음. add_tgt 으로 LUN 부착 필요.
 *
 * SCSI 의미상 한 컨트롤러에는 여러 target slot 이 있고 (SPDK_VHOST_SCSI_CTRLR_MAX_DEVS
 * 만큼), 각 slot 에 1개 LUN(LUN0)을 둘 수 있다. 게스트는 lsscsi 등으로 여러 디스크를 본다.
 *
 * 호출 체인:
 *   RPC vhost_create_scsi_controller → spdk_vhost_lock → 이 함수 → unlock
 */
int spdk_vhost_scsi_dev_construct(const char *name, const char *cpumask);

/**
 * Create an empty vhost SCSI device like \c spdk_vhost_scsi_dev_construct
 * but do not start the controller.
 *
 * This function is thread-safe.
 *
 * \param name name of the vhost device. The name will also be used
 * for socket name, which is exactly \c socket_base_dir/name
 * \param cpumask string containing cpumask in hex. The leading *0x*
 * is allowed but not required. The mask itself can be constructed as:
 * ((1 << cpu0) | (1 << cpu1) | ... | (1 << cpuN)).
 *
 * \return 0 on success, negative errno on error.
 */
/*
 * [한국어]
 * spdk_vhost_scsi_dev_construct_no_start - 컨트롤러를 만들되 시작하지 않는 변형.
 *
 * @name, @cpumask: spdk_vhost_scsi_dev_construct 와 동일.
 * @return: 0 = 성공, 음수 errno.
 *
 * spdk_vhost_scsi_dev_construct 와 거의 같지만 rte_vhost 의 listen 시작을 보류한다.
 * 사용 시나리오: 여러 target 을 add_tgt 으로 미리 부착해 둔 뒤 명시적 start 호출로
 * 한 번에 게스트에게 보이게 하고 싶을 때. (target hot-attach 시 게스트가 N 번 재인식하지
 * 않게 하기 위함.) 별도의 start API 와 짝을 이룬다.
 *
 * 호출 체인:
 *   RPC handler → spdk_vhost_lock → 이 함수 (+ add_tgt 반복) → start API → unlock
 */
int spdk_vhost_scsi_dev_construct_no_start(const char *name, const char *cpumask);

/**
 * Construct and attach new SCSI target to the vhost SCSI device
 * on given (unoccupied) slot.  The device will be created with a single
 * LUN0 associated with given SPDK bdev. Currently only one LUN per
 * device is supported.
 *
 * If the vhost SCSI device has an active connection and has negotiated
 * \c VIRTIO_SCSI_F_HOTPLUG feature,  the new SCSI target should be
 * automatically detected by the other side.
 *
 * \param vdev vhost SCSI device.
 * \param scsi_tgt_num slot to attach to or negative value to use first free.
 * \param bdev_name name of the SPDK bdev to associate with SCSI LUN0.
 *
 * \return value >= 0 on success - the SCSI target ID, negative errno code:
 * -EINVAL - one of the arguments is invalid:
 *   - vdev is not vhost SCSI device
 *   - SCSI target ID is out of range
 *   - bdev name is NULL
 *   - can't create SCSI LUN because of other errors e.g.: bdev does not exist
 * -ENOSPC - scsi_tgt_num is -1 and maximum targets in vhost SCSI device reached
 * -EEXIST - SCSI target ID already exists
 */
/*
 * [한국어]
 * spdk_vhost_scsi_dev_add_tgt - vhost-scsi 컨트롤러에 SCSI target(LUN0=bdev) 부착.
 *
 * @vdev: 대상 vhost-scsi vdev (vhost-blk 면 -EINVAL).
 * @scsi_tgt_num: target slot 번호 [0..MAX-1] 또는 -1 (자동 할당).
 * @bdev_name: SCSI LUN0 으로 노출할 SPDK bdev 의 이름.
 * @return: >=0 = 할당된 target ID, 음수 = 에러 (-EINVAL/-ENOSPC/-EEXIST 등).
 *
 * 게스트가 이미 연결돼 있고 VIRTIO_SCSI_F_HOTPLUG 협상이 끝났다면 게스트 SCSI mid-layer 가
 * 자동으로 new target 을 발견 (lsscsi --rescan 불필요). 협상 안 됐다면 다음 disconnect/
 * reconnect 시 보이게 된다.
 *
 * 동작:
 *   1) bdev_name 으로 backend bdev 조회 + open + io_channel.
 *   2) 빈 slot 을 찾거나 지정된 slot 검증.
 *   3) spdk_scsi_dev 생성 + LUN0 등록 + vdev 의 target 배열에 저장.
 *   4) hot-plug 협상 시 게스트에 notify.
 *
 * 호출 체인:
 *   RPC vhost_scsi_controller_add_target → spdk_vhost_lock → 이 함수 → unlock
 */
int spdk_vhost_scsi_dev_add_tgt(struct spdk_vhost_dev *vdev, int scsi_tgt_num,
				const char *bdev_name);

/**
 * Get SCSI target from vhost SCSI device on given slot. Max
 * number of available slots is defined by.
 * \c SPDK_VHOST_SCSI_CTRLR_MAX_DEVS.
 *
 * \param vdev vhost SCSI device.
 * \param num slot id.
 *
 * \return SCSI device on given slot or NULL.
 */
/*
 * [한국어]
 * spdk_vhost_scsi_dev_get_tgt - 지정 slot 의 SCSI target 핸들 반환.
 *
 * @vdev: vhost-scsi vdev.
 * @num: slot 번호 [0..SPDK_VHOST_SCSI_CTRLR_MAX_DEVS-1].
 * @return: spdk_scsi_dev * 또는 NULL (slot 비어 있음).
 *
 * 호출자는 spdk_vhost_lock 을 잡고 있어야 한다 — 동시 remove_tgt 와 경쟁 회피.
 *
 * 호출 체인:
 *   RPC list / config_json → spdk_vhost_lock → spdk_vhost_scsi_dev_get_tgt(vdev, n) → unlock
 */
struct spdk_scsi_dev *spdk_vhost_scsi_dev_get_tgt(struct spdk_vhost_dev *vdev, uint8_t num);

/**
 * Detach and destruct SCSI target from a vhost SCSI device.
 *
 * The device will be deleted after all pending I/O is finished.
 * If the driver supports VIRTIO_SCSI_F_HOTPLUG, then a hotremove
 * notification will be sent.
 *
 * \param vdev vhost SCSI device
 * \param scsi_tgt_num slot id to delete target from
 * \param cb_fn callback to be fired once target has been successfully
 * deleted. The first parameter of callback function is the vhost SCSI
 * device, the second is user provided argument *cb_arg*.
 * \param cb_arg parameter to be passed to *cb_fn*.
 *
 * \return 0 on success, negative errno on error.
 */
/*
 * [한국어]
 * spdk_vhost_scsi_dev_remove_tgt - 지정 slot 의 SCSI target 분리/해제 (안전 비동기).
 *
 * @vdev: vhost-scsi vdev.
 * @scsi_tgt_num: 제거할 slot 번호.
 * @cb_fn: 제거 완료 후 호출되는 spdk_vhost_event_fn (vdev, cb_arg) → 0/−1.
 * @cb_arg: cb_fn 의 두 번째 인자.
 * @return: 0 = 제거 절차 정상 시작, 음수 errno = 즉시 거부 (slot 비어 있음, vdev 무효 등).
 *
 * 동작:
 *   1) target 의 모든 in-flight bdev_io 가 끝날 때까지 대기 (poller 루프 안에서 ref==0
 *      도달 감시).
 *   2) VIRTIO_SCSI_F_HOTPLUG 가 협상돼 있으면 게스트에 hotremove notification 발사.
 *   3) spdk_scsi_dev 해제, slot NULL 화.
 *   4) cb_fn(vdev, cb_arg) 호출 (vdev_thread 컨텍스트).
 *
 * 호출 체인:
 *   RPC vhost_scsi_controller_remove_target → spdk_vhost_lock → 이 함수 →
 *   (in-flight 대기) → cb_fn(vdev, arg) → unlock 은 호출자 책임 / 또는 cb 내부에서.
 */
int spdk_vhost_scsi_dev_remove_tgt(struct spdk_vhost_dev *vdev, unsigned scsi_tgt_num,
				   spdk_vhost_event_fn cb_fn, void *cb_arg);

/**
 * Construct a vhost blk device.  This will create a Unix domain
 * socket together with a vhost-user slave server waiting for a
 * connection on this socket. Creating the vdev does not start
 * any I/O pollers and does not hog the CPU. I/O processing starts
 * after receiving proper message on the created socket.
 * See QEMU's vhost-user documentation for details. Vhost blk
 * device is tightly associated with given SPDK bdev. Given
 * bdev can not be changed, unless it has been hotremoved. This
 * would result in all I/O failing with virtio \c VIRTIO_BLK_S_IOERR
 * error code.
 *
 * This function is thread-safe.
 *
 * \param name name of the vhost blk device. The name will also be
 * used for socket name, which is exactly \c socket_base_dir/name
 * \param cpumask string containing cpumask in hex. The leading *0x*
 * is allowed but not required. The mask itself can be constructed as:
 * ((1 << cpu0) | (1 << cpu1) | ... | (1 << cpuN)).
 * \param dev_name bdev name to associate with this vhost device
 * \param transport virtio blk transport name (default: vhost_user_blk)
 * \param params JSON value object containing variables:
 * readonly if set, all writes to the device will fail with
 * \c VIRTIO_BLK_S_IOERR error code.
 * packed_ring this controller supports packed ring if set.
 *
 * \return 0 on success, negative errno on error.
 */
/*
 * [한국어]
 * spdk_vhost_blk_construct - vhost-blk 컨트롤러 (PCI virtio-blk) 생성.
 *
 * @name: vdev 이름 = 소켓 파일명. <socket_base>/<name> 으로 listen.
 * @cpumask: hex 문자열 CPU 마스크. data-plane 이 묶일 thread 후보.
 * @dev_name: backend SPDK bdev 이름. 한 vhost-blk 는 정확히 하나의 bdev 와 결합되며
 *            중간에 교체 불가 (hotremove 시 모든 I/O 가 VIRTIO_BLK_S_IOERR 로 실패).
 * @transport: virtio-blk transport 이름. 기본 "vhost_user_blk". (vhost-user-blk 외에
 *             vhost-vsock-blk 등 미래 transport 확장 여지를 위한 인자.)
 * @params: 추가 옵션 JSON 객체 — readonly: bool (true 면 게스트 write 모두 IOERR),
 *          packed_ring: bool (virtio 1.1 packed virtqueue 지원 협상).
 * @return: 0 = 성공, 음수 errno = 실패.
 *
 * vhost-blk 는 SCSI mid-layer 가 없는 대신 단일 디스크/단일 큐(또는 multi-queue) 시멘틱으로
 * 게스트에 보인다. 동작은 scsi_dev_construct 와 마찬가지로 socket listen 후 게스트 connect
 * 를 기다리는 구조 — 즉시 I/O poller 가 돌지 않아 CPU 를 점유하지 않는다.
 *
 * 호출 체인:
 *   RPC vhost_create_blk_controller → spdk_vhost_lock → 이 함수 → unlock
 */
int spdk_vhost_blk_construct(const char *name, const char *cpumask, const char *dev_name,
			     const char *transport, const struct spdk_json_val *params);

/**
 * Remove a vhost device. The device must not have any open connections on it's socket.
 *
 * \param vdev vhost blk device.
 *
 * \return 0 on success, negative errno on error.
 */
/*
 * [한국어]
 * spdk_vhost_dev_remove - vhost vdev (blk 또는 scsi) 를 제거하고 소켓을 닫는다.
 *
 * @vdev: 제거할 vdev 핸들.
 * @return: 0 = 성공, 음수 errno = 실패. 흔한 실패: -EBUSY (게스트가 아직 connected 임).
 *
 * 안전 제거를 위해 active connection 이 없어야 한다 — 즉 게스트가 detach (QEMU 측에서
 * device_del 등) 한 뒤에 호출해야 한다. 그렇지 않으면 -EBUSY 반환. 이 함수는 vhost-blk/scsi
 * 양쪽에 공통으로 적용되며, 내부에서 vdev 타입을 보고 알맞은 destructor 를 호출한다.
 *
 * 동작:
 *   1) 게스트 connection 검사 (있으면 -EBUSY).
 *   2) socket close + unlink.
 *   3) data-plane poller 해제, backend bdev close, io_channel put.
 *   4) 전역 TAILQ 에서 제거 + 메모리 free.
 *
 * 호출 체인:
 *   RPC vhost_delete_controller → spdk_vhost_lock → 이 함수 → unlock
 */
int spdk_vhost_dev_remove(struct spdk_vhost_dev *vdev);

#ifdef __cplusplus                        /* [한국어] C++ extern "C" 블록 닫기 */
}
#endif

#endif /* SPDK_VHOST_H */                 /* [한국어] include guard 종료 */
