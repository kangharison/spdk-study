/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * I/OAT DMA engine driver public interface
 */

/*
 * [한국어 설명] Intel I/OAT(I/O Acceleration Technology) DMA 엔진 공개 API 헤더 (ioat.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 Intel 서버 CPU 에 내장된 I/OAT(Crystal Beach 계열) DMA 엔진을 SPDK
 * 유저스페이스 코드가 사용할 수 있도록 노출하는 공개 API 의 외부 계약을 정의한다.
 * I/OAT 는 Sandy Bridge ~ Ice Lake 세대까지 인텔 Xeon 칩셋에 포함된 PCIe 노출형
 * DMA 가속기로서, CPU 코어의 memcpy 명령을 우회해 DMA 엔진이 메모리 to 메모리 복사·필
 * 작업을 수행하도록 함으로써 CPU 사이클을 절약한다(특히 큰 버퍼 복사). Sapphire Rapids
 * 이후로는 DSA(Data Streaming Accelerator, idxd) 가 후속 가속기로 들어왔으나, 구형
 * 시스템 호환을 위해 SPDK 는 I/OAT 모듈을 함께 유지한다.
 * 본 헤더는 probe/attach/detach 같은 디바이스 라이프사이클, build_copy/submit_copy/
 * build_fill/submit_fill 같은 요청 빌더, flush/process_events 같은 doorbell·완료 처리,
 * 그리고 spdk_ioat_dma_capability_flags 같은 기능 비트 enum 을 외부에 선언한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 가속기 스택에서 I/OAT 는 “채널-기반 PCIe DMA 백엔드”로 분류된다. 호출 흐름:
 *   [Application]
 *     → spdk_accel_submit_copy() / submit_fill() (lib/accel)
 *       → IOAT accel module(module/accel/ioat) 가 등록되어 있다면
 *         → 본 헤더의 spdk_ioat_submit_copy() / spdk_ioat_submit_fill() 호출
 *           → lib/ioat 가 ring 슬롯에 spdk_ioat_dma_hw_desc/spdk_ioat_fill_hw_desc
 *             기록 후 DMACOUNT 도어벨 MMIO write
 *           → I/OAT 하드웨어가 chained descriptor 를 따라가며 DMA 수행
 *           → reactor poller 가 spdk_ioat_process_events() 로 chancmp 메모리(완료
 *             기록 영역) 를 폴링하여 완료 디스크립터에 대해 cb_fn 호출
 * 실행 컨텍스트는 모두 SPDK 유저스페이스 스레드. polled mode 이므로 인터럽트는 사용하지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/env.h: spdk_pci_device 정의 의존(PCIe 디바이스 표현).
 * - spdk/stdinc.h: 표준 정수형/bool 사용.
 * - lib/ioat/ioat.c: 본 헤더의 함수 정의가 위치. ring/chain 관리, MMIO 매핑, descriptor
 *   생성·완료 처리.
 * - include/spdk/ioat_spec.h: 와이어 레벨 descriptor 포맷 (스펙 헤더 — 본 파일은 그 “API
 *   포장지”).
 * - module/accel/ioat/: SPDK accel framework 에 등록되는 모듈로, supports_opcode 로
 *   COPY/FILL 을 알리고 priority 를 지정해 다른 백엔드와 경쟁.
 * - include/spdk/pci_ids.h: SPDK_PCI_VID_INTEL + I/OAT device id 매크로 사용처.
 * 데이터 흐름: 사용자 가상주소(src/dst) → spdk_ioat_build_copy 가 IOVA 변환 →
 * descriptor 의 src_addr/dest_addr/size/control 비트 기록 → flush(DMACOUNT++) →
 * 하드웨어가 chainaddr 부터 DMA → 완료 시 chancmp 메모리에 last completed desc 주소 기록 →
 * process_events 가 그 메모리를 읽고 cb_fn 호출.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_ioat_chan: I/OAT 채널 1개를 표현하는 불투명 핸들. 한 PCI function 당 채널이
 *   복수 존재할 수 있고(과거 IOAT v3.x 는 채널 4~8개), 각 채널이 독립적인 ring/doorbell 보유.
 * - spdk_ioat_probe / detach: 디바이스 라이프사이클.
 * - spdk_ioat_build_copy / submit_copy / build_fill / submit_fill: 요청 빌더. build* 는
 *   ring 슬롯만 채우고, submit* 는 빌드+flush 까지 한 번에 수행.
 * - spdk_ioat_flush: 빌드된 디스크립터들을 한꺼번에 doorbell 로 제출(DMACOUNT 갱신).
 * - spdk_ioat_process_events: 완료 디스크립터의 cb_fn 호출.
 * - spdk_ioat_get_max_descriptors / get_dma_capabilities: 채널 능력 조회.
 * - spdk_ioat_dma_capability_flags enum: 채널이 COPY 만 지원하는지 FILL 도 지원하는지 등.
 */

/* [한국어] include guard — 동일 헤더 중복 포함 방지. */
#ifndef SPDK_IOAT_H
#define SPDK_IOAT_H

/* [한국어] SPDK 표준 추상화. uint*_t / bool / size_t / errno 등을 일괄 가져온다.
 * 다양한 OS(linux/freebsd) 빌드에서도 동일한 기본 타입을 보장. */
#include "spdk/stdinc.h"

/* [한국어] C++ 컴파일 시 C 링키지를 보장 — 네임 맹글링 방지. SPDK 라이브러리는 C 로 빌드되며
 * C++ 사용자 코드도 동일 심볼을 링크할 수 있어야 한다. */
#ifdef __cplusplus
extern "C" {
#endif

/* [한국어] SPDK env 추상화 — spdk_pci_device 등을 가져옴. probe/attach 콜백이 PCI 디바이스
 * 포인터를 받기 위해 필요. */
#include "spdk/env.h"

/**
 * Opaque handle for a single I/OAT channel returned by \ref spdk_ioat_probe().
 */
/* [한국어] 불투명 핸들 전방 선언. 사용자 코드는 포인터만 다루고 내부 필드는 lib/ioat 만 접근.
 * 설정자: spdk_ioat_probe 내부에서 attach 시 할당, attach_cb 의 ioat 인자로 전달.
 * 읽는 자: 사용자가 build/submit/flush/process/detach 의 인자로 전달.
 * 값 범위: 유효한 채널 포인터. detach 후에는 dangling.
 * 동기화: 한 채널은 한 reactor 스레드에 affinity, 동시 호출 금지. */
struct spdk_ioat_chan;

/**
 * Signature for callback function invoked when a request is completed.
 *
 * \param arg User-specified opaque value corresponding to cb_arg from the
 * request submission.
 */
/* [한국어] I/OAT 완료 콜백 typedef. AE4DMA 와 다르게 status 인자가 없는 점에 주의 — I/OAT 는
 * 채널 단위 에러를 chanerr 레지스터에서 별도로 보고하는 모델이다. 따라서 개별 디스크립터 단위
 * 성공/실패가 아니라 “완료 시점에 호출되는 콜백” 의미다.
 * arg: build/submit 시 넘긴 cb_arg 가 그대로 들어옴.
 * 호출 컨텍스트: spdk_ioat_process_events() 를 호출한 스레드에서 동기적 실행. */
typedef void (*spdk_ioat_req_cb)(void *arg);

/**
 * Callback for spdk_ioat_probe() enumeration.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_ioat_probe().
 * \param pci_dev PCI device that is being probed.
 *
 * \return true to attach to this device.
 */
/* [한국어] probe 콜백 typedef — 디바이스 enum 시 attach 여부를 사용자에게 묻는다.
 * cb_ctx: 사용자 임의 컨텍스트.
 * pci_dev: 발견된 PCI 디바이스(spdk_pci_device_get_addr 등으로 BDF/ID 검사 가능).
 * 반환 true 면 lib/ioat 가 attach 진행, false 면 스킵.
 * 호출 컨텍스트: spdk_ioat_probe() 호출 스레드 동기 실행. */
typedef bool (*spdk_ioat_probe_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev);

/**
 * Callback for spdk_ioat_probe() to report a device that has been attached to
 * the userspace I/OAT driver.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_ioat_probe().
 * \param pci_dev PCI device that was attached to the driver.
 * \param ioat I/OAT channel that was attached to the driver.
 */
/* [한국어] attach 완료 통지 콜백 typedef.
 * cb_ctx: probe() 의 cb_ctx 가 전달.
 * pci_dev: attach 된 디바이스.
 * ioat: 새로 할당된 spdk_ioat_chan 핸들 — 사용자가 detach 까지 소유.
 * 호출 컨텍스트: probe() 호출 스레드 동기 실행. */
typedef void (*spdk_ioat_attach_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev,
				    struct spdk_ioat_chan *ioat);

/**
 * Enumerate the I/OAT devices attached to the system and attach the userspace
 * I/OAT driver to them if desired.
 *
 * If called more than once, only devices that are not already attached to the
 * SPDK I/OAT driver will be reported.
 *
 * To stop using the controller and release its associated resources, call
 * spdk_ioat_detach() with the ioat_channel instance returned by this function.
 *
 * \param cb_ctx Opaque value which will be passed back in cb_ctx parameter of
 * the callbacks.
 * \param probe_cb will be called once per I/OAT device found in the system.
 * \param attach_cb will be called for devices for which probe_cb returned true
 * once the I/OAT controller has been attached to the userspace driver.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_ioat_probe - 시스템의 I/OAT PCI 채널들을 enum 후 attach
 *
 * @param cb_ctx: 사용자 임의 컨텍스트. probe_cb/attach_cb 에 그대로 전달.
 * @param probe_cb: 발견된 채널마다 attach 여부를 결정하는 콜백.
 * @param attach_cb: attach 완료 시 사용자에게 채널 핸들 전달.
 * @return: 0 = 성공, -1 = PCI 열거 실패.
 *
 * 동기/배경:
 * I/OAT 는 한 PCI function 안에 여러 채널을 노출(channel count = chancnt 레지스터)하므로
 * probe 는 “채널 단위” 발견을 의미한다. 두 번째 호출은 새로 추가된(attach 안 된) 채널만 보고.
 *
 * 동작 과정:
 * 1) PCI bus 스캔 → Intel I/OAT vendor/device id 매칭.
 * 2) BAR 매핑 후 chancnt 읽어 채널 수 파악.
 * 3) 각 채널에 대해 probe_cb 호출.
 * 4) true 인 채널: ring 할당, descriptor chain 초기화, chancmp 영역 설정, attach_cb 호출.
 *
 * 실행 컨텍스트: 사용자 호출 스레드.
 * 호출 체인: [user main / accel module init] → spdk_ioat_probe → probe_cb/attach_cb
 * 에러 경로: PCI 매핑 실패는 -1, 부분 attach 는 내부 정리.
 */
int spdk_ioat_probe(void *cb_ctx, spdk_ioat_probe_cb probe_cb, spdk_ioat_attach_cb attach_cb);

/**
 * Detach specified device returned by spdk_ioat_probe() from the I/OAT driver.
 *
 * \param ioat I/OAT channel to detach from the driver.
  */
/*
 * [한국어]
 * spdk_ioat_detach - 채널을 드라이버에서 분리하고 자원 해제
 *
 * @param ioat: probe attach_cb 로 받은 채널 핸들. 호출 후 dangling.
 * @return: 없음.
 *
 * 동작 과정:
 * 1) 채널 SUSPEND 또는 RESET 명령(CHANCMD 레지스터) 으로 하드웨어 drain.
 * 2) ring(hugepage) 메모리 free.
 * 3) chancmp 메모리 free.
 * 4) MMIO 언매핑.
 *
 * 실행 컨텍스트: 사용자 호출 스레드. 동시에 process_events 중이면 미정의.
 */
void spdk_ioat_detach(struct spdk_ioat_chan *ioat);

/**
 * Get the maximum number of descriptors supported by the library.
 *
 * \param chan I/OAT channel
 *
 * \return maximum number of descriptors.
 */
/*
 * [한국어]
 * spdk_ioat_get_max_descriptors - 채널이 동시에 보유 가능한 최대 디스크립터 수 조회
 *
 * @param chan: 대상 채널.
 * @return: 최대 디스크립터 수. ring 크기에 종속되며, 사용자는 backpressure 의 기준으로 사용.
 *
 * 호출 체인: [accel ioat module init] → spdk_ioat_get_max_descriptors
 */
uint32_t spdk_ioat_get_max_descriptors(struct spdk_ioat_chan *chan);

/**
 * Build a DMA engine memory copy request.
 *
 * This function will build the descriptor in the channel's ring.  The
 * caller must also explicitly call spdk_ioat_flush to submit the
 * descriptor, possibly after building additional descriptors.
 *
 * \param chan I/OAT channel to build request.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param dst Destination virtual address.
 * \param src Source virtual address.
 * \param nbytes Number of bytes to copy.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_ioat_build_copy - 메모리 복사 디스크립터를 ring 에 빌드(submit 은 flush 가 담당)
 *
 * @param chan: 대상 채널.
 * @param cb_arg: 완료 콜백에 그대로 전달될 사용자 컨텍스트.
 * @param cb_fn: 완료 콜백.
 * @param dst, src: 가상주소(IOVA 변환은 내부에서 spdk_vtophys 사용).
 * @param nbytes: 복사 바이트 수. 단일 디스크립터 크기 한도(보통 IOAT v3.x 는 ~16MB)를
 *   초과하면 chain(next 포인터로 이어진 여러 디스크립터)으로 분할된다.
 * @return: 0 = 성공, 음수 errno = ring 가득/잘못된 인자/IOVA 변환 실패.
 *
 * 동기/배경:
 * I/OAT 는 한 디스크립터의 transfer_size 가 제한적이므로 큰 복사는 chain 으로 쪼개어 next
 * 필드를 채워두면 하드웨어가 한 번의 DMACOUNT 갱신만으로 자동 진행한다. build/submit 분리는
 * doorbell write 횟수를 줄이는 배칭 최적화.
 *
 * 동작 과정:
 * 1) src/dst 가상→IOVA 변환.
 * 2) ring 슬롯에 op=COPY, src_addr, dest_addr, size 기록.
 * 3) 큰 복사면 chain 으로 분할, 각 디스크립터의 next 필드를 다음 슬롯의 IOVA 로 설정.
 * 4) 마지막 디스크립터에 completion_update 비트 + cb_fn/cb_arg 보관.
 * 5) DMACOUNT 는 아직 건드리지 않음.
 *
 * 실행 컨텍스트: 채널 affinity 스레드. 동시 호출 금지.
 * 호출 체인: [accel ioat module] → spdk_ioat_build_copy → 사용자측 spdk_ioat_flush
 * 에러 경로: ring full -ENOMEM, 잘못된 NULL/0 -EINVAL.
 */
int spdk_ioat_build_copy(struct spdk_ioat_chan *chan,
			 void *cb_arg, spdk_ioat_req_cb cb_fn,
			 void *dst, const void *src, uint64_t nbytes);

/**
 * Build and submit a DMA engine memory copy request.
 *
 * This function will build the descriptor in the channel's ring and then
 * immediately submit it by writing the channel's doorbell.  Calling this
 * function does not require a subsequent call to spdk_ioat_flush.
 *
 * \param chan I/OAT channel to submit request.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param dst Destination virtual address.
 * \param src Source virtual address.
 * \param nbytes Number of bytes to copy.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_ioat_submit_copy - 메모리 복사 디스크립터 빌드 + 즉시 doorbell 제출
 *
 * @param chan, cb_arg, cb_fn, dst, src, nbytes: build_copy 와 동일.
 * @return: 0 = 성공, 음수 errno = 실패.
 *
 * 동기/배경:
 * 한 번에 하나만 보낼 때(또는 단순한 호출 패턴) 사용. 내부적으로 build_copy + flush 와 동치이나
 * 한 함수로 묶여 있어 호출자 코드가 간단해진다. 배칭이 필요하면 build_copy 를 여러 번 호출 후
 * flush 를 한 번 부르는 패턴을 쓴다.
 *
 * 실행 컨텍스트: 채널 affinity 스레드.
 * 호출 체인: [accel ioat module] → spdk_ioat_submit_copy → DMACOUNT MMIO write
 */
int spdk_ioat_submit_copy(struct spdk_ioat_chan *chan,
			  void *cb_arg, spdk_ioat_req_cb cb_fn,
			  void *dst, const void *src, uint64_t nbytes);

/**
 * Build a DMA engine memory fill request.
 *
 * This function will build the descriptor in the channel's ring.  The
 * caller must also explicitly call spdk_ioat_flush to submit the
 * descriptor, possibly after building additional descriptors.
 *
 * \param chan I/OAT channel to build request.
 * \param cb_arg Opaque value which will be passed back as the cb_arg parameter
 * in the completion callback.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param dst Destination virtual address.
 * \param fill_pattern Repeating eight-byte pattern to use for memory fill.
 * \param nbytes Number of bytes to fill.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_ioat_build_fill - 메모리 패턴 필 디스크립터를 ring 에 빌드
 *
 * @param chan: 대상 채널. 채널이 FILL 능력을 보유해야 한다(get_dma_capabilities 로 확인).
 * @param cb_arg, cb_fn: 완료 콜백 컨텍스트와 함수.
 * @param dst: 채울 영역의 가상주소.
 * @param fill_pattern: 8바이트 반복 패턴(예: 0x00, 0xFFFFFFFFFFFFFFFF, 0xDEADBEEF...).
 *   하드웨어는 이 패턴을 nbytes 만큼 반복 기록.
 * @param nbytes: 채울 바이트 수(패턴 8바이트 정렬 권장).
 * @return: 0 = 성공, 음수 errno.
 *
 * 동기/배경:
 * memset 류 워크로드(블록 영역 0 초기화 등)를 CPU 사이클 없이 수행. 디스크립터 op=FILL,
 * src 자리에 패턴 값을 직접 넣는 spdk_ioat_fill_hw_desc 포맷을 사용.
 *
 * 동작 과정:
 * 1) dst 가상→IOVA 변환.
 * 2) ring 슬롯에 op=FILL, src_data=fill_pattern, dest_addr, size 기록.
 * 3) 큰 영역이면 chain 분할.
 * 4) DMACOUNT 는 flush 에서.
 *
 * 실행 컨텍스트: 채널 affinity 스레드.
 */
int spdk_ioat_build_fill(struct spdk_ioat_chan *chan,
			 void *cb_arg, spdk_ioat_req_cb cb_fn,
			 void *dst, uint64_t fill_pattern, uint64_t nbytes);

/**
 * Build and submit a DMA engine memory fill request.
 *
 * This function will build the descriptor in the channel's ring and then
 * immediately submit it by writing the channel's doorbell.  Calling this
 * function does not require a subsequent call to spdk_ioat_flush.
 *
 * \param chan I/OAT channel to submit request.
 * \param cb_arg Opaque value which will be passed back as the cb_arg parameter
 * in the completion callback.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param dst Destination virtual address.
 * \param fill_pattern Repeating eight-byte pattern to use for memory fill.
 * \param nbytes Number of bytes to fill.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_ioat_submit_fill - fill 디스크립터 빌드 + 즉시 doorbell 제출
 *
 * @param: build_fill 과 동일.
 * @return: 0 = 성공, 음수 errno.
 *
 * 동기/배경:
 * 단발성 fill 요청에 적합. build_fill + flush 의 합성 함수.
 * 호출 체인: [accel ioat module] → spdk_ioat_submit_fill → DMACOUNT MMIO write
 */
int spdk_ioat_submit_fill(struct spdk_ioat_chan *chan,
			  void *cb_arg, spdk_ioat_req_cb cb_fn,
			  void *dst, uint64_t fill_pattern, uint64_t nbytes);

/**
 * Flush previously built descriptors.
 *
 * Descriptors are flushed by writing the channel's dmacount doorbell
 * register.  This function enables batching multiple descriptors followed by
 * a single doorbell write.
 *
 * \param chan I/OAT channel to flush.
 */
/*
 * [한국어]
 * spdk_ioat_flush - 빌드된 디스크립터들을 DMACOUNT 도어벨로 한 번에 제출
 *
 * @param chan: 대상 채널.
 * @return: 없음.
 *
 * 동기/배경:
 * MMIO write 비용을 줄이기 위해 build_copy/build_fill 을 여러 번 호출 후 flush 를 1번 호출.
 *
 * 동작 과정:
 * 1) 메모리 배리어로 ring 의 디스크립터 쓰기 가시화 보장.
 * 2) DMACOUNT 레지스터에 새 tail 값 MMIO write.
 * 3) 하드웨어가 chainaddr 부터 next 체인을 따라가며 DMA 수행.
 *
 * 실행 컨텍스트: 채널 affinity 스레드.
 */
void spdk_ioat_flush(struct spdk_ioat_chan *chan);

/**
 * Check for completed requests on an I/OAT channel.
 *
 * \param chan I/OAT channel to check for completions.
 *
 * \return number of events handled on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_ioat_process_events - 완료된 디스크립터의 cb_fn 호출 (polled-mode 핵심)
 *
 * @param chan: 대상 채널.
 * @return: 처리한 완료 이벤트 개수(>= 0), 음수 errno = 채널 에러.
 *
 * 동기/배경:
 * 인터럽트 없이 채널의 chancmp(완료 기록 메모리)와 chansts(상태 레지스터) 를 폴링하여
 * “하드웨어가 어디까지 진행했는가” 를 알아낸다. SPDK reactor 가 등록한 poller 가 본 함수를
 * 주기적으로 호출.
 *
 * 동작 과정:
 * 1) chancmp 의 last completed descriptor address 읽기.
 * 2) 채널 head 와 비교해 진행분 산출.
 * 3) 진행분 디스크립터의 cb_fn(cb_arg) 차례로 호출.
 * 4) 채널 head 갱신. chanerr 비트 발견 시 음수 errno 반환.
 *
 * 실행 컨텍스트: 채널 affinity 스레드의 poller 콜백.
 * 호출 체인: [reactor poller] → spdk_ioat_process_events → 각 사용자 cb_fn
 */
int spdk_ioat_process_events(struct spdk_ioat_chan *chan);

/**
 * DMA engine capability flags
 */
/* [한국어] DMA 엔진 기능 비트 enum.
 * 채널마다 하드웨어가 지원하는 op 가 다를 수 있어, 사용자는 spdk_ioat_get_dma_capabilities()
 *   로 비트마스크를 받은 뒤 자신이 사용하려는 op 의 비트가 켜져 있는지 확인해야 한다.
 * 설정자: lib/ioat 가 채널 attach 시 dmacapability 레지스터를 읽어 SPDK_IOAT_DMACAP_*
 *   비트를 본 enum 비트로 매핑.
 * 읽는 자: 사용자가 비트 AND 로 능력 확인. */
enum spdk_ioat_dma_capability_flags {
	SPDK_IOAT_ENGINE_COPY_SUPPORTED	= 0x1, /**< The memory copy is supported */
	/* [한국어] op=COPY(0x00) 디스크립터 발행 가능 — 모든 IOAT v3.x 채널이 기본 지원.
	 * 이 비트가 꺼져 있으면 build_copy/submit_copy 를 호출해도 -ENOTSUP 등이 반환된다. */

	SPDK_IOAT_ENGINE_FILL_SUPPORTED	= 0x2, /**< The memory fill is supported */
	/* [한국어] op=FILL(0x01) 디스크립터 발행 가능 — 일부 IOAT 변종은 BFILL(block fill)
	 * 능력 비트(SPDK_IOAT_DMACAP_BFILL)가 켜져야만 fill 을 지원한다. 이 비트가 꺼져 있으면
	 * spdk_ioat_build_fill/submit_fill 호출 시 실패. */
};

/**
 * Get the DMA engine capabilities.
 *
 * \param chan I/OAT channel to query.
 *
 * \return a combination of flags from spdk_ioat_dma_capability_flags().
 */
/*
 * [한국어]
 * spdk_ioat_get_dma_capabilities - 채널이 지원하는 op 의 비트마스크 조회
 *
 * @param chan: 대상 채널.
 * @return: spdk_ioat_dma_capability_flags 비트들의 OR 조합.
 *
 * 동기/배경:
 * accel framework 에 ioat 모듈을 등록할 때 “이 채널이 어떤 opcode 를 지원하는가” 를 알아야
 * supports_opcode 콜백이 올바른 답을 줄 수 있다. 라이브러리는 attach 시 dmacapability
 * MMIO 를 읽어 캐시.
 *
 * 호출 체인: [accel ioat module init] → spdk_ioat_get_dma_capabilities → supports_opcode
 *   매핑 테이블 구성
 */
uint32_t spdk_ioat_get_dma_capabilities(struct spdk_ioat_chan *chan);

/* [한국어] extern "C" 종결. */
#ifdef __cplusplus
}
#endif

/* [한국어] include guard 종결. */
#endif
