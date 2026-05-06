/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

/** file
 * AE4DMA engine driver public interface
 */

/*
 * [한국어 설명] AMD AE4DMA(AMD EPYC 4th-Gen DMA) 엔진 공개 API 헤더 (ae4dma.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 AMD EPYC SoC 에 내장된 4세대 DMA 엔진(AE4DMA — AMD EPYC Encryption/Acceleration
 * Engine) 을 SPDK 유저스페이스 애플리케이션이 사용할 수 있도록 노출하는 공개 API 의 외부 계약
 * (probe / attach / detach / submit_copy / flush / process_events) 을 정의한다.
 * AE4DMA 는 CPU offload 메모리 복사·필 등을 PCIe MMIO 로 제어 가능한 가속기 형태로 제공하며,
 * SPDK 의 accel framework 에 한 모듈로 등록되어 spdk_accel_submit_copy() 등 상위 호출이
 * 이 디바이스로 라우팅될 때의 진입점 역할을 한다. Intel IDXD/DSA 의 AMD 측 대응물이라고 보면
 * 된다 — 하드웨어 큐(HWQ) 마다 descriptor ring 을 두고 doorbell(write_index) 을 증가시켜
 * 디스크립터를 제출하며, 완료는 read_index 의 진행을 폴링해서 감지한다.
 * 이 파일은 헤더이므로 코드 자체는 없고 “외부 호출자가 호출할 수 있는 함수 시그니처”와
 * “함수가 다루는 불투명 핸들 타입(struct spdk_ae4dma_chan)” 만 선언한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 전체 스택 관점에서 AE4DMA 는 “가속기 백엔드” 계층에 위치한다. 흐름은 다음과 같다:
 *   [Application]
 *     → spdk_accel_submit_copy()/submit_fill()    (lib/accel — 공통 가속기 추상화)
 *       → AE4DMA accel module(module/accel/ae4dma)이 ops 콜백으로 등록되어 있다면
 *         → 본 헤더의 spdk_ae4dma_build_copy() / spdk_ae4dma_flush() 호출
 *           → lib/ae4dma 가 PCIe MMIO 영역의 write_index 레지스터에 디스크립터 인덱스를 써서
 *             AE4DMA 하드웨어가 DMA 를 시작하도록 트리거
 *           → SPDK reactor 가 등록된 poller 에서 주기적으로 spdk_ae4dma_process_events()
 *             를 호출해 read_index 진행분을 확인하고 각 요청의 cb_fn(cb_arg, status) 호출
 * 즉 본 헤더는 “하드웨어 직접 제어 코드(lib/ae4dma)” 와 “상위 가속기 추상화(lib/accel)” 사이의
 * 경계 인터페이스를 정의한다. 실행 컨텍스트는 모두 SPDK 유저스페이스 reactor 스레드(코어에 고정된
 * polled-mode 스레드) 이며 인터럽트는 사용하지 않는다 — 폴링이 동기화의 기본이다.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/env.h: spdk_pci_device 정의를 가져오기 위해 의존. probe/attach 콜백이 PCI 디바이스
 *   포인터를 받기 때문이다 (PCI BDF, vendor/device id 조회는 env_dpdk 가 DPDK 위에서 수행).
 * - spdk/stdinc.h: 표준 정수형(uint32_t 등) 과 iovec 등을 사용하기 위해 의존.
 * - lib/ae4dma/ae4dma.c (구현체): 본 헤더에 선언된 API 의 실제 정의를 보유. MMIO 매핑·HWQ 관리·
 *   descriptor ring·doorbell 동작이 그쪽에 있다.
 * - module/accel/ae4dma/: SPDK accel framework 에 supports_opcode(COPY/FILL) 형태로
 *   본 API 를 등록하는 가속기 모듈. priority 비교를 통해 다른 백엔드(IDXD/IOAT/SW) 와 경쟁.
 * - include/spdk/pci_ids.h: SPDK_PCI_VID_AMD + SPDK_PCI_DID_AMD_AE4DMA_* 매크로로 매칭할
 *   PCI ID 가 정의됨. probe_cb 안에서 이를 참조해 attach 여부를 결정한다.
 * 데이터 흐름: 사용자 가상주소(src/dst iovec) → spdk_ae4dma_build_copy() 가 IOVA 변환 후
 * 디스크립터에 기록 → spdk_ae4dma_flush() 가 write_index 레지스터 갱신 → 하드웨어가 DMA 수행
 * → spdk_ae4dma_process_events() 가 read_index 진행 확인 후 cb_fn 호출 → 사용자 컨텍스트 복원.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_ae4dma_chan: 한 AE4DMA 채널을 표현하는 불투명 핸들. 사용자 코드는 포인터만
 *   다루고 내부 필드는 lib/ae4dma 에서만 접근. 일반적으로 한 PCI function 당 하나가 attach.
 * - spdk_ae4dma_probe(): 시스템에서 AE4DMA PCI 디바이스를 열거하고 probe_cb 가 true 를
 *   반환한 디바이스에 유저스페이스 드라이버를 attach. attach 가 끝나면 attach_cb 호출.
 * - spdk_ae4dma_detach(): 드라이버 분리 — MMIO 언맵, 디스크립터 메모리 해제, 채널 핸들 무효화.
 * - spdk_ae4dma_build_copy(): iovec 기반 메모리 복사 디스크립터를 채널의 ring 에 빌드.
 *   flush 가 호출되기 전까지는 하드웨어로 제출되지 않음(배칭 가능).
 * - spdk_ae4dma_flush(): write_index 를 증가시켜 빌드된 디스크립터들을 하드웨어에 한 번에 제출.
 * - spdk_ae4dma_process_events(): read_index 폴링으로 완료된 요청의 cb_fn 호출.
 * - spdk_ae4dma_req_cb / probe_cb / attach_cb: 비동기 완료/열거 콜백 시그니처 typedef.
 */

/* [한국어] include guard — 동일 헤더의 중복 포함을 방지하기 위한 표준 패턴.
 * 매크로 SPDK_AE4DMA_H 가 정의되어 있지 않을 때만 본문이 컴파일되도록 한다. */
#ifndef SPDK_AE4DMA_H
#define SPDK_AE4DMA_H

/* [한국어] SPDK 의 표준 라이브러리 추상화 헤더. uint32_t/uint64_t/size_t/iovec 등의 표준
 * 정수·구조체와 strcmp/memcpy 같은 기본 함수 선언을 일괄 가져온다. SPDK 는 플랫폼별 헤더
 * 차이를 spdk/stdinc.h 에 모아 두므로 어떤 OS 빌드에서도 동일한 기본 타입을 보장한다. */
#include "spdk/stdinc.h"

/* [한국어] C++ 컴파일러로 이 헤더를 포함할 때도 C 링키지로 심볼이 처리되도록 extern "C" 로 감싼다.
 * SPDK 라이브러리는 C 로 빌드되므로, C++ 애플리케이션이 dlsym/링크할 때 네임 맹글링이 발생하면
 * 안 된다. __cplusplus 매크로는 C++ 컴파일러에서만 정의된다. */
#ifdef __cplusplus
extern "C" {
#endif

/* [한국어] SPDK 의 환경(env) 추상화 헤더. spdk_pci_device 구조체와 spdk_pci_addr 같은
 * PCI 관련 타입을 가져오기 위해 포함한다. AE4DMA 는 PCIe 디바이스이므로 probe_cb/attach_cb 가
 * 이 타입을 인자로 받는다. extern "C" 블록 내부에 둠으로써 env.h 가 C++ 에서도 C 링키지로 처리된다. */
#include "spdk/env.h"

/**
 * Opaque handle for a single AE4DMA channel returned by \ref spdk_ae4dma_probe().
 */
/* [한국어] 전방 선언 — struct spdk_ae4dma_chan 는 본 공개 헤더에서는 “포인터만 다루는 불투명 타입”.
 * 실제 정의(필드: MMIO base, HWQ array, descriptor ring, write/read index, callback table 등)는
 * lib/ae4dma 의 내부 헤더에 있고 사용자에게는 노출되지 않는다.
 * 설정자: spdk_ae4dma_probe() 가 attach 시 내부에서 할당해 attach_cb 의 ae4dma 인자로 전달.
 * 읽는 자: 사용자가 build_copy/flush/process_events/get_max_descriptors/detach 인자로 전달.
 * 값 범위: 유효한 채널 포인터 또는 NULL(에러 시). 한 번 detach 된 후에는 사용 금지(dangling).
 * 동기화: 한 채널은 “channel-level polling thread” 한 개에 affinity 가 묶이는 것이 권장되며,
 * 다른 reactor 에서 동시에 build/flush/process 를 호출하지 않는다. */
struct spdk_ae4dma_chan;

/**
 * Signature for callback function invoked when a request is completed.
 *
 * \param arg User-specified opaque value corresponding to cb_arg from the
 * request submission.
 */
/* [한국어] AE4DMA 요청 완료 콜백의 시그니처 typedef.
 * arg: build_copy 호출 시 전달한 cb_arg 가 그대로 돌아온다. 호출자가 자신의 컨텍스트 객체
 *   포인터를 저장해두는 용도(예: bdev_io 포인터)이다.
 * status: 0 = 성공, 음수 errno = 실패. AE4DMA 하드웨어가 보고한 에러 비트가 errno 형태로
 *   변환되어 전달된다.
 * 호출 컨텍스트: spdk_ae4dma_process_events() 를 호출한 reactor 스레드에서 동기적으로 실행.
 *   따라서 cb_fn 안에서 동일 채널의 build/submit 호출도 안전(같은 스레드).
 * 설정자: 사용자(상위 가속기 모듈)가 build_copy 의 cb_fn 인자로 등록.
 * 읽는 자: lib/ae4dma 가 완료 처리 시 호출. */
typedef void (*spdk_ae4dma_req_cb)(void *arg, int status);

/**
 * Callback for spdk_ae4dma_probe() enumeration.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_ae4dma_probe().
 * \param pci_dev PCI device that is being probed.
 *
 * \return true to attach to this device.
 */
/* [한국어] probe 단계에서 “이 디바이스에 attach 할 것인가?” 를 사용자에게 묻는 콜백 typedef.
 * cb_ctx: probe() 호출 시 사용자가 넘긴 임의의 컨텍스트 포인터(예: 필터링용 BDF 리스트).
 * pci_dev: 현재 발견된 PCI 디바이스. 사용자는 spdk_pci_device_get_addr() 등으로 BDF 를
 *   확인하거나 vendor/device id 를 비교해 attach 여부를 결정.
 * 반환: true 면 lib/ae4dma 가 해당 디바이스를 attach 하고 attach_cb 호출, false 면 스킵.
 * 호출 컨텍스트: spdk_ae4dma_probe() 호출 스레드에서 동기적으로 실행.
 * 설정자: 사용자가 probe() 의 probe_cb 인자로 등록. */
typedef bool (*spdk_ae4dma_probe_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev);

/**
 * Callback for spdk_ae4dma_probe() to report a device that has been attached to
 * the userspace AE4DMA driver.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_ae4dma_probe().
 * \param pci_dev PCI device that was attached to the driver.
 * \param ae4dma AE4DMA channel that was attached to the driver.
 */
/* [한국어] attach 가 끝난 시점에 사용자에게 채널 핸들을 전달하는 콜백 typedef.
 * cb_ctx: probe() 의 cb_ctx 가 그대로 전달.
 * pci_dev: attach 된 PCI 디바이스(probe_cb 와 동일 포인터).
 * ae4dma: 새로 할당된 spdk_ae4dma_chan 핸들 — 이후 build/flush/process_events/detach 에 사용.
 *   이 핸들의 소유권은 사용자에게 넘어가며, 사용자가 spdk_ae4dma_detach() 로 해제 책임을 진다.
 * 호출 컨텍스트: spdk_ae4dma_probe() 호출 스레드에서 동기적으로 실행.
 * 설정자: 사용자가 probe() 의 attach_cb 인자로 등록. */
typedef void (*spdk_ae4dma_attach_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev,
				      struct spdk_ae4dma_chan *ae4dma);

/**
 * Enumerate the AE4DMA devices attached to the system and attach the userspace
 * AE4DMA driver to them if desired.
 *
 * If called more than once, only devices that are not already attached to the
 * SPDK AE4DMA driver will be reported.
 *
 * To stop using the controller and release its associated resources, call
 * spdk_ae4dma_detach() with the ae4dma_channel instance returned by this function.
 *
 * \param cb_ctx Opaque value which will be passed back in cb_ctx parameter of
 * the callbacks.
 * \param probe_cb will be called once per AE4DMA device found in the system.
 * \param attach_cb will be called for devices for which probe_cb returned true
 * once the AE4DMA controller has been attached to the userspace driver.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_ae4dma_probe - 시스템의 AE4DMA PCI 디바이스를 열거하고 attach
 *
 * @param cb_ctx: 사용자 임의 컨텍스트 포인터. probe_cb/attach_cb 호출 시 그대로 되돌아옴.
 *   (예: 필터링 정책 객체, 컨테이너 등)
 * @param probe_cb: 발견된 디바이스 각각에 대해 호출되어 “attach 할지” 를 결정. NULL 이면
 *   기본 동작(모든 디바이스 attach) 일 가능성이 있으나 lib/ae4dma 의 정책에 따른다.
 * @param attach_cb: probe_cb 가 true 를 반환한 디바이스의 attach 가 완료된 후 호출되어
 *   사용자에게 새 채널 핸들을 전달.
 * @return: 0 = 성공(0개 이상 디바이스 attach), -1 = 실패(PCI 열거 자체 실패 등).
 *
 * 동기/배경:
 * SPDK 는 정적 링크가 아니라 “런타임에 발견된 디바이스에 attach” 하는 구조다. 사용자는
 * probe_cb 안에서 BDF/ID 를 보고 자신의 워크로드에 사용할 디바이스만 선택할 수 있다.
 * 두 번째 호출 시에는 이미 attach 된 디바이스는 다시 보고되지 않으므로, 후속 hot-plug 또는
 * 누락된 디바이스만 추가로 incremental attach 하는 데 쓰인다.
 *
 * 동작 과정:
 * 1) lib/ae4dma 가 env_dpdk(rte_pci) 를 통해 PCI bus 를 스캔.
 * 2) AMD vendor + AE4DMA device id 매칭되는 PCI function 을 발견.
 * 3) 각 디바이스에 대해 probe_cb 호출.
 * 4) probe_cb 가 true 면 BAR 매핑, MMIO 레지스터 초기화, descriptor ring 할당, HWQ 셋업.
 * 5) attach_cb 호출하여 채널 핸들 전달.
 *
 * 실행 컨텍스트: 사용자 호출 스레드(보통 spdk_app_start 콜백 안). reactor 진입 전 또는 후에
 *   호출 가능하지만, 호출 도중에는 다른 스레드가 동일 디바이스를 건드리면 안 된다.
 *
 * 호출 체인: [user main] → spdk_ae4dma_probe → lib/ae4dma 내부 PCI 스캔 → probe_cb/attach_cb
 *
 * 에러 경로: PCI 매핑 실패 시 -1 반환, 부분적으로 attach 된 디바이스는 내부에서 정리.
 */
int spdk_ae4dma_probe(void *cb_ctx, spdk_ae4dma_probe_cb probe_cb, spdk_ae4dma_attach_cb attach_cb);

/**
 * Detach specified device returned by spdk_ae4dma_probe() from the AE4DMA driver.
 *
 * \param ae4dma AE4DMA  channel to detach from the driver.
  */
/*
 * [한국어]
 * spdk_ae4dma_detach - 사용자 영역 AE4DMA 드라이버에서 채널을 분리하고 자원 해제
 *
 * @param ae4dma: probe 의 attach_cb 로 받은 채널 핸들. 호출 후에는 dangling 이 되므로
 *   다시 사용하지 말 것.
 * @return: 없음(void). 실패 처리는 내부 로깅에 위임.
 *
 * 동기/배경:
 * SPDK 종료 또는 hot-unplug 시 MMIO BAR 언맵, descriptor 메모리(hugepage) 해제, 채널 구조체
 *   free 가 필요. 이 함수가 그 마무리 역할을 한다.
 *
 * 동작 과정:
 * 1) 진행 중 요청 drain — 가능한 폴링 또는 hardware halt 로 idle 상태 보장.
 * 2) MMIO 영역 unmap.
 * 3) descriptor ring(hugepage backed) 메모리 free.
 * 4) 채널 구조체 free.
 *
 * 실행 컨텍스트: 사용자 호출 스레드. 동일 채널을 다른 스레드에서 동시에 process_events 중이면
 *   미정의 동작 — 사용자 책임.
 *
 * 호출 체인: [user shutdown] → spdk_ae4dma_detach → lib/ae4dma 내부 cleanup
 */
void spdk_ae4dma_detach(struct spdk_ae4dma_chan *ae4dma);

/**
 * Get the maximum number of descriptors supported by the library.
 *
 * \param chan AE4DMA channel
 *
 * \return maximum number of descriptors.
 */
/*
 * [한국어]
 * spdk_ae4dma_get_max_descriptors - 채널이 동시에 보유 가능한 최대 디스크립터 수 조회
 *
 * @param chan: 조회 대상 채널 핸들.
 * @return: 최대 디스크립터 수(uint32_t). 일반적으로 ring 크기 - 1(슬롯 1개는 head==tail
 *   감지 용도) 또는 라이브러리가 정한 상한.
 *
 * 동기/배경:
 * 사용자는 한 번에 build 한 디스크립터가 ring 을 넘지 않도록 backpressure 를 적용해야 하므로
 *   이 값을 알아야 한다. 상위 accel 모듈이 자체 큐 크기를 이 값에 맞춘다.
 *
 * 호출 체인: [accel module init] → spdk_ae4dma_get_max_descriptors
 */
uint32_t spdk_ae4dma_get_max_descriptors(struct spdk_ae4dma_chan *chan);

/**
 * Build a DMA engine memory copy request.
 *
 * This function will build the descriptor in the channel's ring.  The
 * caller must also explicitly call spdk_ae4dma_flush to submit the
 * descriptor, possibly after building additional descriptors.
 *
 * \param chan AE4DMA channel to build request.
 * \param hwq_id HW queue of the channel to be used for buiding descriptors.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param dst Destination virtual address.
 * \param src Source virtual address.
 * \param nbytes Number of bytes to copy.
 *
 * \return 0 on success, negative errno on failure.
 * the spdk_ae4dma_build_copy with iov feature
 */
/*
 * [한국어]
 * spdk_ae4dma_build_copy - 메모리 복사 디스크립터를 채널 ring 에 빌드 (제출은 flush 가 담당)
 *
 * @param ae4dma: 대상 채널 핸들.
 * @param hwq_id: 채널 내부의 어떤 HW Queue 에 디스크립터를 넣을지 지정. AE4DMA 는 한 채널이
 *   여러 HWQ 를 가질 수 있고 HWQ 별로 독립적인 doorbell/완료큐를 가진다.
 * @param cb_arg: 완료 시 cb_fn 의 arg 로 그대로 전달될 사용자 컨텍스트.
 * @param cb_fn: 완료 콜백.
 * @param diov, diovcnt: 목적지 iovec 배열과 그 길이. 흩어진 dst 영역으로의 scatter 를 지원.
 * @param siov, siovcnt: 소스 iovec 배열과 그 길이. 흩어진 src 영역에서의 gather 를 지원.
 * @return: 0 = 성공, 음수 errno = ring 가득 / 잘못된 인자 / IOVA 변환 실패 등.
 *
 * 동기/배경:
 * Build vs submit 분리는 “여러 디스크립터를 빌드한 뒤 한 번의 flush 로 doorbell write 횟수를
 * 줄이는 배칭 최적화” 를 위함. doorbell write 는 PCIe 트랜잭션이므로 비용이 큼.
 *
 * 동작 과정:
 * 1) 가상주소 → IOVA 변환(spdk_vtophys 또는 dpdk 매핑).
 * 2) iovec 길이 합 산출, 단일 디스크립터 한도 초과 시 chain 으로 분할.
 * 3) ring slot 에 op=COPY, src/dst, size 기록 + cb_fn/cb_arg 보관.
 * 4) write_index 레지스터는 아직 건드리지 않음 — flush 에서 갱신.
 *
 * 실행 컨텍스트: 채널이 affinity 된 스레드. 다른 스레드에서 동시 호출 금지.
 * 호출 체인: [accel ae4dma module] → spdk_ae4dma_build_copy → 사용자측 flush 호출
 * 에러 경로: ring full 이면 -ENOMEM 류 errno 반환, 사용자는 process_events 후 재시도.
 */
int
spdk_ae4dma_build_copy(struct spdk_ae4dma_chan *ae4dma, int hwq_id, void *cb_arg,
		       spdk_ae4dma_req_cb cb_fn,
		       struct iovec *diov, uint32_t diovcnt,
		       struct iovec *siov, uint32_t siovcnt);


/**
 * Flush previously built descriptors.
 *
 * Descriptors are flushed by incrementing the write_index register of the
 * command queue.
 *
 * This function increments the write_index register of the paticular queue
 * and flush the descriptor to hardware for further processing.
 *
 * \param chan AE4DMA channel details.
 * hwq_id HW queue of the particular channel to be flushed from.
 */
/*
 * [한국어]
 * spdk_ae4dma_flush - 빌드된 디스크립터를 doorbell 갱신으로 하드웨어에 제출
 *
 * @param chan: 대상 채널 핸들.
 * @param hwq_id: 디스크립터를 빌드한 HWQ 와 동일한 ID. flush 는 HWQ 단위로 수행된다.
 * @return: 없음(void).
 *
 * 동기/배경:
 * MMIO write 는 PCIe TLP 한 건이므로 디스크립터 N 개를 빌드한 뒤 N 번 doorbell 을 치는 대신
 * 1 번만 쳐서 latency 와 PCIe 대역폭을 절약하는 것이 목적. 이 함수가 그 “1번의 doorbell” 을
 * 책임진다.
 *
 * 동작 과정:
 * 1) 메모리 배리어 — 빌드된 디스크립터의 메모리 쓰기가 PCIe 측에서 보이도록 보장.
 * 2) write_index 레지스터에 새 tail 값을 MMIO write.
 * 3) 하드웨어가 read_index 와 비교해 새 디스크립터를 가져가 DMA 시작.
 *
 * 실행 컨텍스트: 채널 affinity 스레드. 호출 후 즉시 반환(논블록).
 * 호출 체인: [accel module] → spdk_ae4dma_build_copy(들) → spdk_ae4dma_flush
 */
void spdk_ae4dma_flush(struct spdk_ae4dma_chan *chan, int hwq_id);

/**
 * Check for completed requests on an AE4DMA channel.
 *
 * This function checks the read_index register to check for the completed
 * requests.
 *
 * \param chan AE4DMA channel to check for completions.
 * \param hwq_id HW queue on which the read_index needs to verified.
 *
 * \return number of events handled on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_ae4dma_process_events - read_index 폴링으로 완료된 요청 수만큼 cb_fn 호출
 *
 * @param chan: 대상 채널 핸들.
 * @param hwq_id: 폴링할 HWQ ID.
 * @return: 처리한 완료 이벤트 개수(>= 0), 음수 errno = 에러.
 *
 * 동기/배경:
 * polled-mode 의 핵심 함수. SPDK reactor 가 등록한 poller 가 본 함수를 주기적으로 호출하여
 * 인터럽트 없이 완료를 회수한다. 인터럽트를 쓰면 컨텍스트 스위치가 발생하므로 latency 가 커짐.
 *
 * 동작 과정:
 * 1) 하드웨어의 read_index 레지스터 읽기.
 * 2) 마지막으로 처리한 head_index 와 비교해 진행분 계산.
 * 3) 진행분에 해당하는 디스크립터들의 cb_fn(cb_arg, status) 를 차례로 호출.
 * 4) head_index 업데이트(다음 process_events 의 기준점).
 *
 * 실행 컨텍스트: 채널 affinity 스레드의 poller 콜백.
 * 호출 체인: [reactor poller] → spdk_ae4dma_process_events → 각 사용자 cb_fn
 * 에러 경로: 하드웨어 에러 비트 발견 시 cb_fn 의 status 에 errno 변환해 전달.
 */
int spdk_ae4dma_process_events(struct spdk_ae4dma_chan *chan, int hwq_id);

/* [한국어] extern "C" 블록의 종결. C++ 컴파일 시점에만 활성화되는 close brace. */
#ifdef __cplusplus
}
#endif

/* [한국어] include guard 종결 — SPDK_AE4DMA_H 매크로 영역의 끝. */
#endif
