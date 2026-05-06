/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * IDXD driver public interface
 */

/*
 * [한국어 설명] SPDK IDXD(Intel Data Streaming/Analytics Accelerator) 사용자 공개 API (idxd.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK가 Intel SoC 내장 가속기(DSA: Data Streaming Accelerator,
 * IAA: In-memory Analytics Accelerator) 를 유저스페이스에서 직접 구동하기 위한
 * **공개 헤더**다. SPDK 응용은 이 헤더에 선언된 spdk_idxd_* 함수만으로
 * IDXD 디바이스의 초기화(probe/attach), I/O 채널(spdk_idxd_io_channel) 획득,
 * 메모리 복사/패턴 채우기/CRC32-C/비교/압축/해제/DIF 검사·삽입·제거·DIX 등
 * 다양한 가속 연산을 비동기로 발행할 수 있다.
 * 헤더의 함수들은 모두 prototype만 두고, 실제 구현은 lib/idxd/idxd.c 에 있다.
 * 또한 응용은 본 헤더가 노출한 콜백 시그니처(spdk_idxd_req_cb / spdk_idxd_attach_cb /
 * spdk_idxd_probe_cb) 를 구현하여 비동기 완료 통지와 디바이스 attach 정책을
 * 주입한다. 즉, 이 파일은 "유저 응용 ↔ SPDK IDXD 라이브러리"의 ABI/문법 계약을 정의한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK IDXD 스택은 다음과 같이 계층화된다.
 *   [응용/accel framework]
 *      ↓ spdk_idxd_submit_* (이 헤더의 API)
 *   [lib/idxd: SPDK 유저스페이스 IDXD 드라이버]   ← 본 헤더의 구현 위치
 *      ↓ MMIO portal write (ENQCMDS/MOVDIR64B 명령어로 64B 디스크립터 발행)
 *   [PCIe → IDXD 디바이스(DSA/IAA, Sapphire Rapids 이후 SoC 내장)]
 *      ↑ Completion Record(32B) 메모리 쓰기 + valid 비트 세트
 *   [poller: spdk_idxd_process_events()] ← 동일 spdk_thread에서 폴링
 * 이 헤더의 API는 모두 **단일 spdk_thread 컨텍스트**에서 호출되어야 한다.
 * 디스크립터 발행과 완료 폴링이 같은 코어/스레드에서 수행되므로 lockless 설계를
 * 유지할 수 있다. SPDK reactor 모델(1코어=1reactor=1유저스레드) 위에서 작동.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h(공통 표준 헤더), spdk/idxd_spec.h(IDXD 와이어 포맷:
 *   디스크립터·completion record·CAP 레지스터), spdk/dif.h(DIF/DIX 컨텍스트),
 *   spdk/env.h(spdk_pci_device 등 DPDK 추상화).
 * - 본 헤더의 함수를 호출하는 상위 모듈: module/accel/idxd/(SPDK accel framework
 *   가속 모듈), accel framework가 opcode→IDXD로 위임할 때 본 헤더의 submit_* 호출.
 *   직접 응용도 spdk_idxd_probe()로 디바이스를 attach하여 사용 가능.
 * - 본 헤더 구현이 호출하는 하위 모듈: lib/env_dpdk(PCIe 매핑), x86 ENQCMDS/
 *   MOVDIR64B 인라인 어셈블리, libpciaccess(/sys/bus/pci 스캔).
 * - 데이터 흐름: 응용 buffer(IOVA/DMA 가능) → idxd_hw_desc 작성 → MMIO portal write
 *   → 디바이스 처리 → completion record 갱신(메모리 쓰기) → process_events()가
 *   valid 비트 polling 후 spdk_idxd_req_cb 호출(arg, status).
 * - 공유 자료구조: struct idxd_hw_desc(스펙 정의, 디바이스가 직접 읽음),
 *   struct dsa_hw_comp_record/iaa_hw_comp_record(디바이스가 쓰고 SW가 읽음).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_idxd_probe / spdk_idxd_detach: PCI 스캔하여 IDXD 디바이스를 SPDK
 *   유저스페이스 드라이버에 부착/해제. probe_cb로 어떤 디바이스를 가져갈지 결정.
 * - spdk_idxd_set_config: 커널 모드(idxd kernel driver mediated) 또는 PF
 *   passthrough 모드를 선택. 첫 호출은 init 직전에 1회.
 * - spdk_idxd_get_channel / spdk_idxd_put_channel: 디바이스에서 spdk_thread별
 *   I/O 채널 획득/반환. 채널은 그 스레드에 고정(thread affinity).
 * - spdk_idxd_submit_{copy,fill,compare,crc32c,copy_crc32c,compress,decompress,
 *   dualcast,dif_check,dif_insert,dif_strip,dix_generate,raw_desc}: 각 가속 연산을
 *   디스크립터로 빌드 → portal write로 즉시 발행. 완료는 cb_fn 콜백.
 * - spdk_idxd_process_events: 채널의 완료 record를 polling. 호출자는 SPDK poller로
 *   주기 등록. 반환값은 처리한 완료 개수(반응형 backoff에 활용).
 * - 핵심 typedef: spdk_idxd_req_cb(완료 콜백), spdk_idxd_attach_cb(probe 결과
 *   디바이스 통지), spdk_idxd_probe_cb(부착 여부 결정).
 * - 핵심 opaque 핸들: struct spdk_idxd_device(PCI 디바이스 단위),
 *   struct spdk_idxd_io_channel(스레드 단위 디스크립터 ring + completion polling 상태).
 */

#ifndef SPDK_IDXD_H
#define SPDK_IDXD_H
/* [한국어] SPDK IDXD 공개 헤더 가드. 다중 include 시 중복 정의 방지. */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 stdinc 헤더 — <stdint.h>, <stdbool.h>, <unistd.h>, <pthread.h>,
 * <sys/uio.h>(struct iovec) 등을 일괄 포함. iovec/uint32_t 등이 본 헤더의 prototype에
 * 등장하므로 필요. */
#include "spdk/idxd_spec.h"
/* [한국어] IDXD 와이어 포맷 헤더 — struct idxd_hw_desc(64B 디스크립터),
 * dsa_hw_comp_record(완료 레코드), IDXD_FLAG_*(디바이스 플래그 비트마스크) 정의를 가져온다.
 * spdk_idxd_submit_raw_desc()의 인자 타입과 SPDK_IDXD_FLAG_NONTEMPORAL 매크로 의존. */
#include "spdk/dif.h"
/* [한국어] SPDK DIF(Data Integrity Field) 컨텍스트 정의 — struct spdk_dif_ctx.
 * dif_check/insert/strip/dix_generate API의 인자로 사용되며, NVMe T10 DIF
 * (8B PI: Guard/AppTag/RefTag) 의 시드/마스크/블록 크기 등을 캡슐화한다. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++에서 include할 때 C 링크 규약을 강제 — name mangling 방지. */
#endif

#include "spdk/env.h"
/* [한국어] SPDK 환경 추상화 — 여기서는 spdk_pci_device 타입 정의를 가져온다.
 * spdk_idxd_probe_cb 콜백 시그니처에 spdk_pci_device * 인자가 등장하므로 필요.
 * extern "C" 안쪽에 위치 — 이 헤더가 C++에 노출하는 타입도 C 링크로 묶기 위함. */

/* The following flags control the behavior of I/O operations to IDXD. These flags
 * are often mapped to DSA specification values to ensure they have a unique value,
 * but do not necessarily correspond 1:1 with the hardware-defined flags.
 */
/* [한국어] 아래 SPDK_IDXD_FLAG_* 매크로들은 spdk_idxd_submit_*() 의 flags 인자에
 * OR로 전달되어 개별 연산의 동작을 제어한다. 일부는 DSA 스펙의 IDXD_FLAG_* 비트와
 * 동일한 비트를 사용하나, "의미"가 항상 1:1이 아님에 주의 — SPDK는 추상화 계층에서
 * 의미를 재해석한 뒤 디스크립터의 실제 hardware flag 비트로 변환한다. */

/* This flag indicates that IDXD should bypass the CPU cache for write operations,
 * landing the output directly into main memory. This is considered a hint, rather
 * than a guarantee.
 *
 * Note: While the value here is defined to be IDXD_FLAG_CACHE_CONTROL, this is only
 * to ensure that the flag has a unique value. The meaning here is the reverse of
 * IDXD_FLAG_CACHE_CONTROL - i.e. not specifying a flag writes data into CPU cache
 * because writing to cache is a more sensible default behavior.
 */
#define SPDK_IDXD_FLAG_NONTEMPORAL IDXD_FLAG_CACHE_CONTROL
/* [한국어] "비-시간적(non-temporal) 쓰기" 힌트 — 결과를 CPU L1/L2/LLC를 우회하여
 * 메인 메모리에 직접 기록하도록 디바이스에 요청한다. 대용량 streaming write에서
 * 캐시 오염을 막아 다른 핫 데이터를 보호하기 위해 사용.
 * 값은 IDXD_FLAG_CACHE_CONTROL 비트(idxd_spec.h: 1<<8)와 동일하지만, SPDK 의미는
 * 하드웨어 비트의 "쓸 때 캐시 컨트롤" 의미를 반전한 것이다 — flag가 "없으면" 캐시에
 * 쓰는 것이 기본(일반적 OS/응용 기대), "있으면" 캐시 우회.
 * 설정자: spdk_idxd_submit_*() 호출자.
 * 읽는 자: lib/idxd 내부에서 디스크립터 빌드 시 IDXD_FLAG_CACHE_CONTROL로 변환.
 * 동기화: per-call 인자이므로 동시성 이슈 없음. */

/**
 * Opaque handle for a single IDXD channel.
 */
struct spdk_idxd_io_channel;
/* [한국어] 단일 IDXD I/O 채널의 불투명 핸들(전방 선언).
 * - 한 채널은 단일 spdk_thread 컨텍스트에 고정되어, 그 스레드 전용 디스크립터 ring과
 *   completion record array, in-flight 추적을 캡슐화한다.
 * - 설정자: spdk_idxd_get_channel()이 디바이스에서 채널을 할당.
 * - 읽는 자: 모든 spdk_idxd_submit_*() 와 spdk_idxd_process_events() 의 첫 인자.
 * - 값 범위: get_channel 반환값 (NULL이면 자원 부족/오류).
 * - 동기화: 한 채널은 한 스레드에서만 다뤄야 하므로 별도 락 불필요(lockless).
 * - 실제 구조 정의는 lib/idxd/idxd_internal.h. 응용은 포인터로만 다룬다. */

/**
 * Opaque handle for a single IDXD device.
 */
struct spdk_idxd_device;
/* [한국어] 단일 IDXD PCI 디바이스(DSA/IAA 한 PCI function)의 불투명 핸들.
 * - 디바이스 단위 자원(WQ 구성, MMIO 매핑, group/engine 토폴로지) 캡슐화.
 * - 설정자: spdk_idxd_probe()가 attach_cb로 응용에 전달.
 * - 읽는 자: spdk_idxd_get_socket(), spdk_idxd_get_channel(), spdk_idxd_detach().
 * - 값 범위: NULL 불가(probe 후), detach 후에는 무효.
 * - 동기화: 한 디바이스는 여러 스레드에서 채널을 받아갈 수 있어 디바이스 단위 자원
 *   할당(채널 ID, WQ 슬롯)에는 디바이스 내부 락이 사용됨. 응용은 신경 쓰지 않아도 됨. */

/**
 * Get the socket that this device is on.
 *
 * \param idxd device to query
 * \return socket number.
 */
/*
 * [한국어]
 * spdk_idxd_get_socket - 해당 IDXD 디바이스가 부착된 NUMA 노드(소켓) 번호 조회.
 *
 * @idxd: probe로 attach 받은 디바이스 핸들. NULL이면 동작 보장 안 됨.
 * @return: NUMA 소켓 번호(0..N-1) 또는 SPDK_ENV_SOCKET_ID_ANY(매칭 안 될 때).
 *
 * 응용/accel framework는 IDXD 디바이스를 사용할 spdk_thread를 같은 NUMA 노드에 배치하여
 * cross-socket QPI/UPI 트래픽을 줄인다. 이 함수는 lib/env_dpdk가 PCI BDF→NUMA를
 * 매핑한 결과를 그대로 반환한다.
 *
 * 호출 컨텍스트: 임의 스레드. 단순 읽기이므로 thread affinity 제약 없음.
 *
 * 호출 체인:
 *   응용/accel scheduler → spdk_idxd_get_socket() → idxd->socket_id 반환
 */
uint32_t spdk_idxd_get_socket(struct spdk_idxd_device *idxd);

/**
 * Signature for callback function invoked when a request is completed.
 *
 * \param arg User-specified opaque value corresponding to cb_arg from the
 *            request submission.
 * \param status 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_req_cb - IDXD 비동기 요청 완료 콜백 시그니처.
 *
 * @arg: submit_*() 호출 시 응용이 넘긴 cb_arg 그대로 전달. 응용이 자기 컨텍스트
 *       (예: spdk_bdev_io 포인터)를 복원하는 데 사용.
 * @status: 0=성공, 음수=−errno 또는 매핑된 SPDK/POSIX 에러. 비트 단위 의미가 아님.
 *
 * 호출 컨텍스트: 항상 spdk_idxd_process_events() 가 실행 중인 스레드(=요청을 발행한
 * 스레드와 동일). 즉, 응용은 콜백 안에서 또 다른 SPDK API를 호출해도 lockless 가정이
 * 유지된다. 단, 콜백 안에서 같은 채널에 대해 또 submit_*()를 호출하면 reentrancy
 * 주의 — process_events 한 번에 여러 완료를 처리할 수 있도록 라이브러리는 안전 처리.
 *
 * SPDK accel framework는 이 콜백에서 다시 spdk_accel_task의 다음 단계를 진행하거나
 * 상위 spdk_bdev_io를 완료시킨다.
 *
 * 호출 체인:
 *   spdk_idxd_process_events() → (디스크립터별) cb_fn(cb_arg, status)
 */
typedef void (*spdk_idxd_req_cb)(void *arg, int status);

/**
 * Callback for spdk_idxd_probe() to report a device that has been attached to
 * the userspace IDXD driver.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_idxd_probe().
 * \param idxd IDXD device that was attached to the driver.
 */
/*
 * [한국어]
 * spdk_idxd_attach_cb - probe 과정에서 부착(attach)에 성공한 디바이스를 응용에게 통지.
 *
 * @cb_ctx: spdk_idxd_probe()에 전달했던 사용자 컨텍스트. (예: 디바이스 리스트 헤드)
 * @idxd: 새로 attach된 디바이스 핸들. 응용은 이 포인터를 보관하여 이후
 *        get_channel/detach 등에 사용.
 *
 * 호출 컨텍스트: spdk_idxd_probe() 내부에서 동기적으로 호출됨 — probe와 동일 스레드.
 * 응용은 이 콜백 안에서 즉시 채널을 획득하지 않고, 디바이스 핸들만 보관하는 것이 권장.
 *
 * 호출 체인:
 *   응용 → spdk_idxd_probe() → (PCI 스캔 후 각 디바이스에 대해) attach_cb(cb_ctx, idxd)
 */
typedef void (*spdk_idxd_attach_cb)(void *cb_ctx, struct spdk_idxd_device *idxd);

/**
 * Callback for spdk_idxd_probe() to report a device that has been found.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_idxd_probe().
 * \param dev PCI device that is in question.
 * \return true if the caller wants the device, false if not..
 */
/*
 * [한국어]
 * spdk_idxd_probe_cb - 발견된 PCI 디바이스를 SPDK가 가져갈지 응용에게 묻는 콜백.
 *
 * @cb_ctx: probe()에 전달한 사용자 컨텍스트.
 * @dev: PCI scan으로 발견된 디바이스(spdk_pci_device, env.h). VID/DID/BDF 등 조회 가능.
 * @return: true=이 디바이스를 attach하라, false=skip.
 *
 * 보통 응용은 IDXD_PCI_VID(0x8086) 와 device id로 DSA/IAA를 식별 후 정책(NUMA, ID 화이트
 * 리스트 등)에 따라 true/false 결정한다. false 반환 시 해당 디바이스에 대해서는
 * attach_cb가 호출되지 않는다.
 *
 * 호출 컨텍스트: probe() 내부, PCI 스캔 루프에서 동기적으로 호출됨.
 *
 * 호출 체인:
 *   응용 → spdk_idxd_probe() → (각 PCI 후보) probe_cb → true면 attach_cb
 */
typedef bool (*spdk_idxd_probe_cb)(void *cb_ctx, struct spdk_pci_device *dev);

/**
 * Enumerate the IDXD devices attached to the system and attach the userspace
 * IDXD driver to them if desired.
 *
 * If called more than once, only devices that are not already attached to the
 * SPDK IDXD driver will be reported.
 *
 * To stop using the controller and release its associated resources, call
 * spdk_idxd_detach() with the idxd_channel instance returned by this function.
 *
 * \param cb_ctx Opaque value which will be passed back in cb_ctx parameter of
 *               the callbacks.
 * \param probe_cb callback to determine if the device being probe should be attached.
 * \param attach_cb will be called for devices for which probe_cb returned true.
 *                  once the IDXD controller has been attached to the userspace driver.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_idxd_probe - 시스템에 부착된 모든 IDXD 디바이스를 PCI 스캔하여 SPDK 유저
 * 스페이스 드라이버로 부착(VFIO/UIO/idxd kernel driver mediated).
 *
 * @cb_ctx: probe_cb / attach_cb 양쪽에 전달될 사용자 컨텍스트.
 * @attach_cb: probe_cb가 true를 반환한 후, 디바이스가 실제로 SPDK 드라이버에
 *             초기화·매핑된 시점에 호출됨. 응용은 디바이스 포인터를 여기서 받아 보관.
 * @probe_cb: 발견된 각 PCI 후보에 대해 attach 여부 결정. NULL이면 라이브러리 기본
 *            정책 사용(IDXD VID/DID 일치 시 true).
 * @return: 0=성공(0개 발견도 성공), -1=치명적 오류(PCI 스캔 실패 등).
 *
 * 멱등성: 두 번 이상 호출하면 이미 attach된 디바이스는 다시 통지하지 않는다.
 * 응용은 새로 hotplug된 디바이스를 주기적으로 가져오기 위해 반복 호출 가능.
 *
 * 해제: detach() 사용. attach_cb 안에서 보관한 idxd 핸들을 detach()에 전달.
 *
 * 호출 컨텍스트: 응용 초기화 단계(보통 main thread / spdk_app_start 직후),
 * I/O 경로에서는 호출 금지.
 *
 * 호출 체인:
 *   응용 → spdk_idxd_probe() → lib/idxd:idxd_probe_internal() → libpciaccess
 *     → 각 후보에 대해 probe_cb → 통과 시 PF/VFIO 매핑 → attach_cb
 */
int spdk_idxd_probe(void *cb_ctx, spdk_idxd_attach_cb attach_cb,
		    spdk_idxd_probe_cb probe_cb);

/**
 * Detach specified device returned by spdk_idxd_probe() from the IDXD driver.
 *
 * \param idxd IDXD device to detach from the driver.
  */
/*
 * [한국어]
 * spdk_idxd_detach - 지정 IDXD 디바이스를 SPDK 드라이버에서 분리하고 자원 해제.
 *
 * @idxd: probe의 attach_cb로 받았던 디바이스 핸들. NULL 호출 시 동작 미정의.
 * @return: 없음(void).
 *
 * 사전 조건: 이 디바이스의 모든 I/O 채널이 spdk_idxd_put_channel()로 반환되어 있어야 함.
 * in-flight 디스크립터가 있는 채널이 남아 있으면 디바이스 reset이 hang할 수 있다.
 *
 * 동작: 디바이스 disable command(IDXD_DISABLE_DEV) → WQ 비활성화 → MMIO 언매핑 →
 * 메모리 해제. 이후 idxd 포인터는 무효(use-after-free 금지).
 *
 * 호출 컨텍스트: 응용 종료 단계. I/O hot path에서 호출 금지.
 *
 * 호출 체인:
 *   응용 → spdk_idxd_detach() → 디바이스 disable cmd → MMIO 언매핑 → free
 */
void spdk_idxd_detach(struct spdk_idxd_device *idxd);

/**
 * Sets the IDXD configuration.
 *
 * \param kernel_mode true if using kernel driver.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_set_config - SPDK가 IDXD를 사용할 모드를 설정(전역).
 *
 * @kernel_mode: true=커널의 idxd 드라이버가 mediate(WQ 슬라이스를 /dev/dsa* 캐릭터
 *               디바이스로 노출, ENQCMDS로 user-mode 발행)하는 모드. false=SPDK가
 *               PF 자체를 직접 PF Passthrough로 점유(VFIO no-IOMMU 또는 UIO).
 * @return: 0=성공, 음수=−errno(이미 init 후 호출한 경우 -EPERM 등).
 *
 * 멱등성/순서: spdk_idxd_probe() 이전에 1회 호출. probe 이후의 호출은 무시되거나
 * 오류. 전역 상태이므로 한 프로세스 안에서는 한 번만 결정한다.
 *
 * 두 모드의 차이:
 * - kernel_mode=true: 동일 호스트의 다른 process(QEMU 등)와 IDXD 디바이스를 공유 가능.
 *   SVM(Shared Virtual Memory)/PASID 활용. 디스크립터는 ENQCMDS(SWQ 안전 발행)로 발행.
 * - kernel_mode=false: SPDK가 디바이스를 독점, MOVDIR64B(DWQ 전용)도 사용 가능.
 *   더 낮은 발행 latency, 단 다른 process와 공존 불가.
 *
 * 호출 체인:
 *   응용 → spdk_idxd_set_config() → lib/idxd 전역 g_kernel_mode 갱신
 */
int spdk_idxd_set_config(bool kernel_mode);

/**
 * Build and submit an idxd memory copy request.
 *
 * This function will build the copy descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param diov Destination iovec.
 * \param diovcnt Number of elements in diov.
 * \param siov Source iovec.
 * \param siovcnt Number of elements in siov.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 *               the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_submit_copy - DSA Memory Move(opcode IDXD_OPCODE_MEMMOVE=0x03) 비동기 발행.
 *
 * @chan: 호출 스레드의 IDXD I/O 채널. 디스크립터 ring과 completion record array 보유.
 * @diov / @diovcnt: 목적지 iovec 배열과 길이. 다수의 segment면 라이브러리가 batch
 *                   디스크립터(IDXD_OPCODE_BATCH=0x01)로 묶거나 segment마다 sub-desc 발행.
 * @siov / @siovcnt: 소스 iovec. 총 바이트 수는 diov 합과 같아야 함.
 * @flags: SPDK_IDXD_FLAG_NONTEMPORAL 등. 라이브러리가 디스크립터 flags 필드(IDXD_FLAG_*)로
 *         번역하여 기록.
 * @cb_fn / @cb_arg: 완료 시 호출되는 콜백과 사용자 컨텍스트.
 * @return: 0=발행 성공(완료는 비동기), 음수=−errno(자원 부족 -ENOMEM, ring full -EBUSY 등).
 *
 * 동작 단계:
 *   1) ring에서 빈 디스크립터 슬롯 + completion record 슬롯 확보.
 *   2) idxd_hw_desc 작성: opcode=MEMMOVE, src/dst 주소(IOVA), xfer_size, completion_addr.
 *   3) MMIO portal 주소(BAR2 + WQ portal offset)에 ENQCMDS(SWQ)/MOVDIR64B(DWQ)로 64B 발행.
 *   4) 응용 컨텍스트(cb_fn/cb_arg)는 채널 내부 in-flight 테이블에 저장.
 *
 * 완료: 디바이스가 dsa_hw_comp_record.status에 비-zero(0x01=PASS) 기록 → 다음
 * spdk_idxd_process_events() 호출 시 폴링되어 cb_fn 실행.
 *
 * 호출 컨텍스트: 채널을 소유한 spdk_thread 만 (cross-thread 호출 금지).
 *
 * 호출 체인:
 *   응용/accel module → spdk_idxd_submit_copy() → lib/idxd:idxd_submit_copy()
 *     → ENQCMDS/MOVDIR64B → IDXD 디바이스
 *     ↑ (poll) spdk_idxd_process_events() → cb_fn(cb_arg, status)
 */
int spdk_idxd_submit_copy(struct spdk_idxd_io_channel *chan,
			  struct iovec *diov, uint32_t diovcnt,
			  struct iovec *siov, uint32_t siovcnt,
			  int flags, spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit an idxd dualcast request.
 *
 * This function will build the dual cast descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param dst1 First destination virtual address (must be 4K aligned).
 * \param dst2 Second destination virtual address (must be 4K aligned).
 * \param src Source virtual address.
 * \param nbytes Number of bytes to copy.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 *               the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_submit_dualcast - DSA Dualcast(opcode 0x05) 비동기 발행.
 * 한 번의 read로 src를 읽고 두 개의 destination(dst1, dst2)에 동시에 기록.
 *
 * @chan: 채널 핸들.
 * @dst1: 첫 번째 목적지 가상 주소. **반드시 4KB 정렬** (DSA 스펙 요구: dualcast는 두
 *        목적지의 4K 페이지 alignment가 일치해야 한다).
 * @dst2: 두 번째 목적지. 마찬가지로 4KB 정렬 필요.
 * @src: 소스 가상 주소(특정 alignment 요구 없음).
 * @nbytes: 복사 길이. max_xfer_size(보통 2GB) 이하.
 * @flags: SPDK_IDXD_FLAG_NONTEMPORAL 등.
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0=성공, 음수=−errno(정렬 위반 -EINVAL 등).
 *
 * 사용처: replication, cache-tier write-through, memcpy x2 합치기.
 * Dualcast는 메모리 read를 1회만 수행해 메모리 대역폭을 절반으로 줄인다.
 *
 * 호출 체인:
 *   응용 → spdk_idxd_submit_dualcast() → DSA dualcast desc → portal
 */
int spdk_idxd_submit_dualcast(struct spdk_idxd_io_channel *chan,
			      void *dst1, void *dst2, const void *src, uint64_t nbytes, int flags,
			      spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory compare request.
 *
 * This function will build the compare descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param siov1 First source iovec.
 * \param siov1cnt Number of elements in siov1.
 * \param siov2 Second source iovec.
 * \param siov2cnt Number of elements in siov2.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 *               the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_submit_compare - DSA Compare(opcode 0x09) 비동기 발행.
 * 두 메모리 영역을 바이트 단위 비교, 첫 mismatch 위치를 completion record에 기록.
 *
 * @chan: 채널 핸들.
 * @siov1 / @siov1cnt: 첫 번째 소스 iovec과 길이.
 * @siov2 / @siov2cnt: 두 번째 소스 iovec과 길이. 총 바이트 수는 siov1과 같아야 함.
 * @flags: 비교 동작 제어(현재 SPDK 추상화에서 의미 있는 플래그는 적음).
 * @cb_fn / @cb_arg: 완료 콜백. status=0이면 일치, status=-EILSEQ 등이면 불일치.
 * @return: 0=발행 성공, 음수=−errno.
 *
 * 사용처: dedup, replication 검증, RAID scrubbing.
 * 결과(일치/불일치 + 첫 불일치 오프셋)는 dsa_hw_comp_record.bytes_completed로 노출되며,
 * SPDK는 이를 status로 변환해 콜백에 전달한다.
 *
 * 호출 체인:
 *   응용 → spdk_idxd_submit_compare() → DSA compare desc → portal
 */
int spdk_idxd_submit_compare(struct spdk_idxd_io_channel *chan,
			     struct iovec *siov1, size_t siov1cnt,
			     struct iovec *siov2, size_t siov2cnt,
			     int flags, spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit a idxd memory fill request.
 *
 * This function will build the fill descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param diov Destination iovec.
 * \param diovcnt Number of elements in diov.
 * \param fill_pattern Repeating eight-byte pattern to use for memory fill.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the cb_arg parameter
 *               in the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_submit_fill - DSA Memory Fill(opcode 0x04) 비동기 발행. memset 가속.
 *
 * @chan: 채널 핸들.
 * @diov / @diovcnt: 채울 목적지 iovec.
 * @fill_pattern: 8바이트 반복 패턴(uint64_t). 0이면 zeroing(흔한 use case).
 *                디스크립터 src_addr 자리(union의 pattern 멤버)에 저장됨.
 * @flags: NONTEMPORAL 등.
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0=발행 성공, 음수=−errno.
 *
 * 사용처: NVMe write zeroes 가속, 버퍼 초기화, blob 영역 초기화.
 * memset 대비 장점: CPU 코어 점유 없이 IDXD 엔진이 백그라운드 처리.
 *
 * 호출 체인:
 *   응용 → spdk_idxd_submit_fill() → DSA fill desc(pattern in src union) → portal
 */
int spdk_idxd_submit_fill(struct spdk_idxd_io_channel *chan,
			  struct iovec *diov, size_t diovcnt,
			  uint64_t fill_pattern, int flags, spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory CRC32-C request.
 *
 * This function will build the CRC-32C descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param siov Source iovec.
 * \param siovcnt Number of elements in siov.
 * \param seed Four byte CRC-32C seed value.
 * \param crc_dst Resulting calculation.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the cb_arg parameter
 *               in the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_submit_crc32c - DSA CRC32-C 생성(opcode 0x10) 비동기 발행.
 * NVMe end-to-end data protection / iSCSI CRC / NVMe-oF TCP digest 가속에 사용.
 *
 * @chan: 채널 핸들.
 * @siov / @siovcnt: 입력 데이터 iovec.
 * @seed: CRC32-C(Castagnoli, 다항식 0x1EDC6F41) 초기값. 청크별 누적 시 이전 결과를 시드로.
 * @crc_dst: 완료 시 결과를 저장할 uint32_t 포인터. 응용 메모리. 라이브러리는 이 포인터를
 *           in-flight 테이블에 보관해두었다가 completion 처리 시 dsa_hw_comp_record.crc32c_val을
 *           복사한다. (즉, 콜백 호출 시점에 *crc_dst 가 유효)
 * @flags: IDXD_CLEAR_CRC_FLAGS 같은 시드 처리 옵션.
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0=발행 성공, 음수=−errno.
 *
 * 호출 체인:
 *   응용 → spdk_idxd_submit_crc32c() → DSA CRCGEN desc → portal
 *   ↑ process_events → *crc_dst = comp.crc32c_val → cb_fn
 */
int spdk_idxd_submit_crc32c(struct spdk_idxd_io_channel *chan,
			    struct iovec *siov, size_t siovcnt,
			    uint32_t seed, uint32_t *crc_dst, int flags,
			    spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit a copy combined with CRC32-C request.
 *
 * This function will build the descriptor for copy plus CRC32-C and then immediately
 * submit by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param diov Destination iovec.
 * \param diovcnt Number of elements in diov.
 * \param siov Source iovec.
 * \param siovcnt Number of elements in siov.
 * \param seed Four byte CRC-32C seed value.
 * \param crc_dst Resulting calculation.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the cb_arg parameter
 *               in the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_submit_copy_crc32c - DSA Copy+CRC32-C(opcode 0x11) 비동기 발행.
 * 한 번의 디스크립터로 메모리 복사와 CRC 계산을 동시에 수행 — bandwidth 절감.
 *
 * @chan / @diov / @diovcnt / @siov / @siovcnt: copy와 동일.
 * @seed / @crc_dst: crc32c와 동일.
 * @flags: IDXD_FLAG_CRC_READ_CRC_SEED(저장된 seed에서 이어서) 등.
 * @cb_fn / @cb_arg: 완료 콜백. 콜백 시점에 dst에 데이터가 쓰여 있고 *crc_dst에 CRC가 들어 있음.
 * @return: 0=발행 성공, 음수=−errno.
 *
 * 사용처: NVMe read 경로에서 SSD→host buffer 복사 + CRC 검증 동시 수행.
 *
 * 호출 체인:
 *   응용 → spdk_idxd_submit_copy_crc32c() → DSA COPY_CRC desc → portal
 */
int spdk_idxd_submit_copy_crc32c(struct spdk_idxd_io_channel *chan,
				 struct iovec *diov, size_t diovcnt,
				 struct iovec *siov, size_t siovcnt,
				 uint32_t seed, uint32_t *crc_dst, int flags,
				 spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit an IAA memory compress request.
 *
 * This function will build the compress descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param dst Destination to write the compressed data to.
 * \param nbytes Length in bytes. The dst buffer should be large enough to hold the compressed data.
 * \param siov Source iovec.
 * \param siovcnt Number of elements in siov.
 * \param output_size The size of the compressed data.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 *               the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_submit_compress - IAA DEFLATE 압축(opcode IAX_OPCODE_COMPRESS=0x43) 발행.
 * **IAA(Intel In-memory Analytics Accelerator) 전용** — DSA 디바이스에서는 미지원.
 * 채널이 IAA 디바이스 위에 있어야 함(채널 획득 시 디바이스 종류로 결정).
 *
 * @chan: IAA 채널 핸들.
 * @dst: 압축 결과 출력 버퍼(연속). 압축 데이터가 들어갈 충분한 공간 필요(보통 src 크기와 동일).
 * @nbytes: dst 버퍼 크기 한계. 디스크립터의 max_dst_size로 사용. 초과 시 IAA_COMP_OUTBUF_OVERFLOW.
 * @siov / @siovcnt: 입력 iovec(원본 데이터).
 * @output_size: 완료 시 실제 압축 크기를 저장할 uint32_t 포인터. iaa_hw_comp_record.output_size
 *               에서 복사. 콜백 시점에 유효.
 * @flags: IAA_COMP_FLAGS(FLUSH_OUTPUT|APPEND_EOB) 등.
 * @cb_fn / @cb_arg: 완료 콜백. status=0 성공, IAA_COMP_OUTBUF_OVERFLOW=음수 매핑.
 * @return: 0=발행 성공, 음수=−errno.
 *
 * 사용처: 압축 bdev module(reduce), IAA 가속 압축.
 *
 * 호출 체인:
 *   응용/reduce → spdk_idxd_submit_compress() → IAA compress desc(AECS 첨부) → portal
 *   ↑ process_events → *output_size = comp.output_size → cb_fn
 */
int spdk_idxd_submit_compress(struct spdk_idxd_io_channel *chan,
			      void *dst, uint64_t nbytes,
			      struct iovec *siov, uint32_t siovcnt, uint32_t *output_size,
			      int flags, spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit an IAA memory decompress request.
 *
 * This function will build the decompress descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param diov Destination iovec. diov with diovcnt must be large enough to hold decompressed data.
 * \param diovcnt Number of elements in diov for decompress buffer.
 * \param siov Source iovec.
 * \param siovcnt Number of elements in siov.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 *               the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_submit_decompress - IAA DEFLATE 압축 해제(opcode IAX_OPCODE_DECOMPRESS=0x42) 발행.
 *
 * @chan: IAA 채널 핸들.
 * @diov / @diovcnt: 압축 해제 결과 출력 iovec. 충분한 크기 필요(미리 알 수 없으면 최대값으로 추정).
 * @siov / @siovcnt: 압축된 입력 iovec.
 * @flags: IAA_DECOMP_FLAGS(ENABLE|FLUSH_OUTPUT|CHECK_FOR_EOB|STOP_ON_EOB) 등.
 * @cb_fn / @cb_arg: 완료 콜백. 압축 해제 크기는 콜백 시점에 응용이 별도 경로로 알아야 함
 *                    (상위 reduce module은 chunk header에 원본 크기를 저장해 둠).
 * @return: 0=발행 성공, 음수=−errno.
 *
 * 호출 체인:
 *   응용/reduce → spdk_idxd_submit_decompress() → IAA decompress desc → portal
 */
int spdk_idxd_submit_decompress(struct spdk_idxd_io_channel *chan,
				struct iovec *diov, uint32_t diovcnt,
				struct iovec *siov, uint32_t siovcnt,
				int flags, spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit a DIF check request.
 *
 * This function will build the DIF check descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param siov Source iovec.
 * \param siovcnt Number of elements in siov.
 * \param num_blocks Total number of blocks to process.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to check.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the cb_arg parameter
 *               in the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_submit_dif_check - DSA DIF Check(opcode IDXD_OPCODE_DIF_CHECK=0x12) 발행.
 * NVMe T10 DIF/PI(8B Protection Information: 2B Guard + 2B AppTag + 4B RefTag)를
 * 데이터 블록 끝에 첨부된 형식(=NVMe extended LBA)으로 검증.
 *
 * @chan: DSA 채널.
 * @siov / @siovcnt: 검사할 데이터 iovec. 각 블록은 {data || PI(8B)} 형식.
 * @num_blocks: 처리할 블록 수. 디스크립터의 xfer_size = num_blocks * (block_size + 8) 가 됨.
 * @ctx: spdk/dif.h의 spdk_dif_ctx — 블록 크기, AppTag/RefTag 시드, guard 다항식,
 *       reftag 증분 정책 등을 캡슐화. 라이브러리가 ctx를 dif_chk 디스크립터 필드로 변환.
 * @flags: SPDK_IDXD_FLAG_*.
 * @cb_fn / @cb_arg: 완료 콜백. status=0 성공, status<0 (예: -EILSEQ) 무결성 오류 —
 *                    completion record dif_chk_ref_tag/app_tag로 어느 블록에서 실패했는지 식별.
 * @return: 0=발행 성공, 음수=−errno.
 *
 * 사용처: NVMe 읽기 경로에서 host로 데이터를 넘기기 전에 PI 검사.
 *
 * 호출 체인:
 *   응용/bdev_nvme → spdk_idxd_submit_dif_check() → DSA DIF_CHECK desc → portal
 */
int spdk_idxd_submit_dif_check(struct spdk_idxd_io_channel *chan,
			       struct iovec *siov, size_t siovcnt,
			       uint32_t num_blocks, const struct spdk_dif_ctx *ctx, int flags,
			       spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit a DIF insert request.
 *
 * This function will build the DIF insert descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param diov Destination iovec.
 * \param diovcnt Number of elements in diov.
 * \param siov Source iovec.
 * \param siovcnt Number of elements in siov.
 * \param num_blocks Total number of blocks to process.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to insert.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the cb_arg parameter
 *               in the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_submit_dif_insert - DSA DIF Insert(opcode 0x13) 발행.
 * 메타데이터 없이 들어온 source data에 PI(8B)를 계산·생성하여 destination에 extended LBA
 * 형식으로 기록. 즉, "raw 데이터 → DIF 보호 데이터" 변환.
 *
 * @chan: DSA 채널.
 * @diov / @diovcnt: 출력. 각 블록 = {data || PI(8B)}. 크기 = num_blocks * (block_size + 8).
 * @siov / @siovcnt: 입력. 각 블록 = data만(PI 없음). 크기 = num_blocks * block_size.
 * @num_blocks: 블록 수.
 * @ctx: AppTag/RefTag 시드. 라이브러리가 dif_ins 디스크립터 필드로 변환.
 * @flags: 동작 옵션.
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0=발행 성공, 음수=−errno.
 *
 * 사용처: NVMe 쓰기 경로에서 host data → SSD에 기록하기 전 PI 첨부.
 *
 * 호출 체인:
 *   응용 → spdk_idxd_submit_dif_insert() → DSA DIF_INSERT desc → portal
 */
int spdk_idxd_submit_dif_insert(struct spdk_idxd_io_channel *chan,
				struct iovec *diov, size_t diovcnt,
				struct iovec *siov, size_t siovcnt,
				uint32_t num_blocks, const struct spdk_dif_ctx *ctx, int flags,
				spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit a DIF strip request.
 *
 * This function will build the DIF strip descriptor and then immediately submit
 * by writing to the proper device portal. The transfer size must be a multiple
 * of the source block size plus metadata for each source block. The number
 * of bytes written to the destination is the transfer size minus metadata
 * for each source block. The source and destination data can be scattered across
 * several different buffers, but each source buffer has to be a multiple of
 * a block size and each destination buffer has to be a multiple of a data block size
 * excluding metadata. Moreover, the length of each element in the source array (siov)
 * must be exactly the same as the corresponding element in the destination array (diov)
 * excluding metadata size.
 *
 * \param chan IDXD channel to submit request.
 * \param diov Destination iovec.
 * \param diovcnt Number of elements in diov.
 * \param siov Source iovec.
 * \param siovcnt Number of elements in siov.
 * \param num_blocks Total number of blocks to process.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the cb_arg parameter
 *               in the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_submit_dif_strip - DSA DIF Strip(opcode 0x14) 발행.
 * 보호된 데이터(extended LBA: data + PI(8B))를 검증한 뒤 PI를 제거하여 raw data만 destination에 기록.
 *
 * @chan: DSA 채널.
 * @diov / @diovcnt: 출력 (data만, PI 제거). 각 element 크기 = (corresponding siov element 크기 - 블록당 8B).
 * @siov / @siovcnt: 입력 (data + PI). 각 element는 block_size+8 의 정수배.
 * @num_blocks: 블록 수. transfer_size = num_blocks * (block_size + 8).
 * @ctx: DIF 컨텍스트(검증 기준 시드).
 * @flags: 동작 옵션.
 * @cb_fn / @cb_arg: 완료 콜백. 검증 실패 시 status<0.
 * @return: 0=발행 성공, 음수=−errno.
 *
 * 제약 (스펙 + SPDK 추가 제약):
 *   - 각 source element 크기 = block_size+8 의 정수배.
 *   - 각 dest element 크기 = (대응 source 크기) - (블록 수 * 8).
 *   - source와 destination iovec 인덱스가 1:1 대응되어야 함.
 *
 * 사용처: NVMe 읽기 경로에서 SSD→host buffer 전달 시 PI 검증+제거.
 *
 * 호출 체인:
 *   응용 → spdk_idxd_submit_dif_strip() → DSA DIF_STRIP desc → portal
 */
int spdk_idxd_submit_dif_strip(struct spdk_idxd_io_channel *chan,
			       struct iovec *diov, size_t diovcnt,
			       struct iovec *siov, size_t siovcnt,
			       uint32_t num_blocks, const struct spdk_dif_ctx *ctx, int flags,
			       spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit DIX Generate request.
 *
 * This function will build DIX Generate descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit the request.
 * \param siov Source iovecs.
 * \param siovcnt Number of elements in siov.
 * \param mdiov Metadata iovec for generated protection information.
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIX context. Contains the DIX configuration values, including the reference
 *	Application Tag and initial value of the Reference Tag to insert.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called upon request completion.
 * \param cb_arg Opaque value which will be passed as a parameter to the cb_fn.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_submit_dix_generate - DSA DIX Generate(opcode 0x17) 발행.
 * DIX(Data Integrity eXtension): data와 메타데이터(PI)가 분리된 buffer로 저장되는 형식.
 * NVMe metadata pointer separate buffer + DIF combination에 해당.
 *
 * @chan: DSA 채널.
 * @siov / @siovcnt: 데이터 iovec(data only, PI 없음).
 * @mdiov: 메타데이터 iovec(생성된 PI를 저장할 별도 버퍼). 크기 = num_blocks * 8B.
 * @num_blocks: 블록 수.
 * @ctx: DIX 컨텍스트(시드).
 * @flags: 동작 옵션.
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0=발행 성공, 음수=−errno.
 *
 * 사용처: NVMe metadata pointer 분리형(PRP1->data, MPTR->metadata)에서 PI 생성.
 *
 * 호출 체인:
 *   응용 → spdk_idxd_submit_dix_generate() → DSA DIX_GEN desc → portal
 */
int spdk_idxd_submit_dix_generate(struct spdk_idxd_io_channel *chan, struct iovec *siov,
				  size_t siovcnt, struct iovec *mdiov, uint32_t num_blocks,
				  const struct spdk_dif_ctx *ctx, int flags,
				  spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit an IDXD raw request.
 *
 * This function will process the supplied descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param desc properly formatted IDXD descriptor.  Memory addresses should be physical.
 *             The completion address will be filled in by the lower level library.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 *               the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_idxd_submit_raw_desc - 응용이 직접 빌드한 idxd_hw_desc을 그대로 발행.
 * 표준 submit_*() 가 다루지 않는 opcode(BATCH, DRAIN, SCAN/EXTRACT 등 IAA 분석 opcode,
 * 새로운 opcode 등) 를 사용할 때 사용하는 escape hatch.
 *
 * @chan: 채널 핸들.
 * @desc: 응용이 채워둔 64B 디스크립터. opcode/flags/src/dst/xfer_size/op_specific 필드 모두 응용 책임.
 *        **메모리 주소는 물리(IOVA) 주소여야 함** — 가상 주소 → IOVA 변환은 spdk_vtophys().
 *        completion_addr와 completion_record의 cb_fn 매핑은 라이브러리가 채움(덮어쓸 수 있음).
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0=발행 성공, 음수=−errno.
 *
 * 위험: 응용이 잘못된 opcode/flags 조합을 넣으면 디바이스가 INVALID_FLAGS / BAD_OPCODE
 * status로 실패하거나 SW err 인터럽트를 일으킨다. 디바이스 reset이 필요할 수도 있음.
 *
 * 사용처: 실험적 가속, batch descriptor 직접 제어, 진단/디버그.
 *
 * 호출 체인:
 *   고급 응용 → spdk_idxd_submit_raw_desc() → portal write
 */
int spdk_idxd_submit_raw_desc(struct spdk_idxd_io_channel *chan,
			      struct idxd_hw_desc *desc,
			      spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Check for completed requests on an IDXD channel.
 *
 * \param chan IDXD channel to check for completions.
 * \return number of operations completed.
 */
/*
 * [한국어]
 * spdk_idxd_process_events - 채널의 completion record 배열을 polling, 완료된 디스크립터의
 * 콜백을 모두 호출하고 슬롯을 ring으로 반환.
 *
 * @chan: 채널 핸들.
 * @return: 이번 호출에서 처리한 완료 개수(>=0). 0 반환은 SPDK poller에서 backoff 신호로 활용.
 *
 * 동작:
 *   1) 채널의 in-flight 슬롯 인덱스를 head→tail로 순회.
 *   2) 각 슬롯의 completion record(volatile uint8_t status) 를 읽는다.
 *      - 0 = 미완료 → 더 이상 진행하지 않고 반환(완료는 in-order 가정 — IDXD는 큐 순서대로 완료 가능).
 *      - 0x01(PASS) = 정상 완료 → cb_fn(cb_arg, 0) 호출.
 *      - 그 외 = 오류 코드 → SPDK errno로 변환 후 cb_fn(cb_arg, -errno).
 *   3) status를 0으로 reset, 슬롯을 free pool로 반환.
 *
 * 호출 컨텍스트: 채널 소유 spdk_thread. SPDK poller로 등록되어 reactor loop에서
 * 주기적으로 호출됨(보통 매 루프마다).
 *
 * 동기화: lockless — 채널은 단일 스레드 소유이므로 ring head/tail에 락 불필요.
 * completion record는 디바이스가 쓰고 SW가 읽는 단일 produce/consume 패턴.
 * volatile + 메모리 배리어(콜백 안에서)로 ordering 보장.
 *
 * 호출 체인:
 *   reactor → poller → spdk_idxd_process_events() → 각 완료마다 cb_fn(cb_arg, status)
 */
int spdk_idxd_process_events(struct spdk_idxd_io_channel *chan);

/**
 * Returns an IDXD channel for a given IDXD device.
 *
 * \param idxd IDXD device to get a channel for.
 * \return pointer to an IDXD channel.
 */
/*
 * [한국어]
 * spdk_idxd_get_channel - 디바이스에서 호출 스레드 전용 I/O 채널 할당.
 *
 * @idxd: 대상 디바이스 핸들.
 * @return: 새로 할당된 채널, NULL=자원 부족(WQ 슬롯 소진 / 메모리 부족).
 *
 * 동작:
 *   - 디바이스에서 가용한 WQ 슬롯 분배(group/engine round-robin 또는 explicit 매핑).
 *   - 채널 전용 디스크립터 ring(보통 64~512 entries) + completion record array 할당.
 *     completion record는 8B aligned가 필수(스펙 요구).
 *   - in-flight 추적 자료구조(slot ↔ {cb_fn, cb_arg, *crc_dst, *output_size}) 초기화.
 *
 * 호출 컨텍스트: 채널을 사용할 spdk_thread에서 호출 — 그 스레드에 affinity 부여.
 * 다른 스레드에서 받은 채널을 사용하면 lockless 가정이 깨짐.
 *
 * 호출 체인:
 *   응용/accel module → spdk_idxd_get_channel() → 채널 할당 → 응용에 반환
 */
struct spdk_idxd_io_channel *spdk_idxd_get_channel(struct spdk_idxd_device *idxd);

/**
 * Free an IDXD channel.
 *
 * \param chan IDXD channel to free.
 */
/*
 * [한국어]
 * spdk_idxd_put_channel - 채널을 디바이스로 반환하고 자원 해제.
 *
 * @chan: 반환할 채널. NULL이면 no-op (구현에 따라 다를 수 있음).
 * @return: 없음.
 *
 * 사전 조건: 이 채널의 in-flight 디스크립터가 모두 완료되어 있어야 한다(없으면
 * leak 또는 디바이스 hang). 응용은 보통 cleanup 단계에서 process_events()를
 * 한참 돌려 drain 후 호출.
 *
 * 동작: WQ 슬롯 반환, ring/completion 메모리 해제, 디바이스의 채널 카운터 감소.
 *
 * 호출 컨텍스트: 채널 소유 스레드. 다른 스레드에서 호출 금지.
 *
 * 호출 체인:
 *   응용 cleanup → spdk_idxd_put_channel() → 디바이스 채널 카운터/WQ 슬롯 반환
 */
void spdk_idxd_put_channel(struct spdk_idxd_io_channel *chan);

#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 종료 — C++에서 include한 경우. */
#endif

#endif
/* [한국어] SPDK_IDXD_H 가드 종료. */
