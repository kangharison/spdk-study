/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

/*
 * [한국어 설명] AE4DMA 드라이버 내부 헤더 (ae4dma_internal.h)
 *
 * === 파일의 역할 ===
 * SPDK ae4dma 드라이버 내부에서만 공유되는 자료구조와 인라인 헬퍼를 정의한다.
 * 외부 공개 API(`include/spdk/ae4dma.h`)는 사용자에게 채널 핸들과
 * `spdk_ae4dma_*()` 함수만 노출하지만, 본 헤더는 채널 객체의 실제
 * 멤버(레지스터 매핑, 큐 메타데이터, 통계)를 그대로 드러내어 ae4dma.c가
 * 직접 접근할 수 있게 한다. 또한 32/64비트 분할 매크로(upper_32_bits/
 * lower_32_bits)와 큐 크기/full 검사 같은 인라인 유틸을 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK accel/ae4dma 스택에서 다음 위치를 차지한다:
 *   [accel framework] → [ae4dma accel module] → [ae4dma 드라이버 (lib/ae4dma)]
 *                                                     ↓ 본 헤더의 자료구조
 *                                                 [PCIe MMIO BAR — H/W]
 * 호스트 유저스페이스(SPDK reactor 스레드)에서 동작하며, hugepage 기반 DMA
 * 메모리와 PCIe BAR0를 직접 매핑해 사용한다. ae4dma_spec.h가 정의하는
 * H/W ABI(`spdk_ae4dma_desc`, `spdk_ae4dma_hwq_regs`)를 그대로 포인터로
 * 보유한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `spdk/stdinc.h`, `spdk/ae4dma.h`(콜백 타입 spdk_ae4dma_req_cb 등),
 *   `ae4dma_spec.h`(H/W 디스크립터/레지스터 구조), `spdk/queue.h`(TAILQ 매크로),
 *   `spdk/mmio.h`(spdk_mmio_read_4/write_4 — barrier가 들어간 MMIO 헬퍼).
 * - 본 헤더에 의존: `lib/ae4dma/ae4dma.c` (드라이버 구현체).
 * - 데이터 흐름: 사용자 콜백 정보(`callback_fn`/`callback_arg`)는
 *   호스트 측 그림자 링(`ae4dma_descriptor::ring`)에 저장되어 H/W 디스크립터
 *   완료 시점에 드라이버가 콜백으로 호출. 실제 DMA 디스크립터는
 *   `qbase_addr`에 hugepage로 매핑되어 H/W가 직접 접근.
 * - 공유 자료구조: `spdk_ae4dma_chan`은 외부 API 호출의 핸들이며,
 *   드라이버 글로벌 리스트 `g_ae4dma_driver.attached_chans`에 등록됨.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_ae4dma_chan : 한 AE4DMA 채널(=PCIe device 1대) 표현. 16개 H/W
 *   큐(cmd_q[])와 PCI 디바이스 핸들, MMIO 베이스를 보유.
 * - struct ae4dma_cmd_queue : 한 H/W 큐에 대한 호스트 측 메타. MMIO regs 포인터,
 *   디스크립터 링 베이스(qbase_addr), 호스트 그림자 ring(콜백 보관), tail/write_index 등.
 * - struct ae4dma_descriptor : H/W 디스크립터에 대응하는 호스트 측 그림자 — 사용자
 *   콜백(`callback_fn`, `callback_arg`) 보관용.
 * - upper_32_bits / lower_32_bits : 64비트 물리 주소를 H/W 디스크립터의
 *   src_lo/src_hi 등 32비트 필드로 분할하기 위한 비트 매크로.
 * - ae4dma_desc_cmdq_full : 큐가 가득 찼는지(여유 슬롯 4 미만) 검사.
 * - ae4dma_config_queues_per_device : 요청한 큐 수가 H/W 한도(16)를 넘는지 검증.
 */

#ifndef __AE4DMA_INTERNAL_H__
#define __AE4DMA_INTERNAL_H__

/* [한국어] SPDK 표준 인클루드(uint*_t/bool 등). */
#include "spdk/stdinc.h"
/* [한국어] 외부에 노출되는 ae4dma 공개 API (`spdk_ae4dma_chan` 전방 선언과
 *  콜백 타입 `spdk_ae4dma_req_cb` 등). */
#include "spdk/ae4dma.h"
/* [한국어] H/W 사양 정의(디스크립터/레지스터/매크로) — 이 헤더의 구조체가
 *  포인터로 참조한다. */
#include "ae4dma_spec.h"
/* [한국어] TAILQ 매크로 — 드라이버 글로벌 리스트(attached_chans)에 채널을
 *  연결하기 위해 필요. */
#include "spdk/queue.h"
/* [한국어] spdk_mmio_read_4/write_4 같은 MMIO 액세스 헬퍼.
 *  본 헤더의 인라인 함수에서 직접 쓰진 않으나 전반에 걸쳐 사용됨. */
#include "spdk/mmio.h"
/**
 * upper_32_bits - return bits 32-63 of a number
 * @n: the number we're accessing
 */
/* [한국어] 64비트 정수의 상위 32비트를 추출. (n>>16)>>16 형태로 두 단계로 나눠
 *  쉬프트하는 이유는 일부 컴파일러/플랫폼에서 single >>32에 대한 경고를 피하기
 *  위함. ae4dma 디스크립터의 src_hi/dst_hi/qbase_hi에 PA(64) 상위를 넣을 때 사용. */
#define upper_32_bits(n) ((uint32_t)(((n) >> 16) >> 16))

/**
 * lower_32_bits - return bits 0-31 of a number
 * @n: the number we're accessing
 */
/* [한국어] 64비트 정수의 하위 32비트를 0xffffffff 마스크로 추출.
 *  src_lo/dst_lo/qbase_lo 채울 때 사용. */
#define lower_32_bits(n) ((uint32_t)((n) & 0xffffffff))

/* [한국어] AE4DMA 한 H/W 큐가 보유하는 디스크립터 슬롯 수(고정 32). 링 인덱스의
 *  wrap-around 분모로 쓰인다. write_idx 갱신 시 (idx+1) % 32 형태로 사용. */
#define AE4DMA_DESCRIPTORS_PER_CMDQ 32
/* [한국어] 한 디스크립터의 바이트 크기(=32). spec 헤더 SPDK_STATIC_ASSERT로 보장됨. */
#define AE4DMA_QUEUE_DESC_SIZE  sizeof(struct spdk_ae4dma_desc)
/* [한국어] 한 큐의 전체 디스크립터 영역 바이트 크기 계산: 32 * 디스크립터크기.
 *  단, 호출 시 인자 n에 디스크립터 크기를 넣어야 의미가 맞도록 설계되어 있어
 *  드라이버는 AE4DMA_QUEUE_SIZE(AE4DMA_QUEUE_DESC_SIZE) 형태로 사용한다. */
#define AE4DMA_QUEUE_SIZE(n)  (AE4DMA_DESCRIPTORS_PER_CMDQ * (n))

/*
 * [한국어] struct ae4dma_descriptor — H/W 디스크립터에 1:1 대응되는 호스트
 *  측 "그림자" 객체. H/W 디스크립터에는 콜백 정보를 담을 수 없으므로 별도의
 *  호스트 사이드 ring을 두고 같은 인덱스에 콜백을 보관해 두었다가, 완료 폴링
 *  시 동일 인덱스의 callback_fn을 호출한다.
 */
struct ae4dma_descriptor {
	spdk_ae4dma_req_cb	callback_fn;
	/* [한국어] 사용자 요청 완료 시 호출될 콜백.
	 * 설정자: `spdk_ae4dma_build_copy()`가 마지막 디스크립터에만 사용자 콜백을 설정.
	 * 읽는 자: `ae4dma_process_channel_events()`가 H/W 완료 후 호출.
	 * 값 범위: NULL(콜백 없음, 중간 디스크립터) 또는 유효 함수 포인터.
	 * 동기화: 한 디스크립터는 단일 스레드에서만 다뤄지므로 락 불필요. */

	void			*callback_arg;
	/* [한국어] callback_fn에 전달될 사용자 컨텍스트.
	 * 설정자: 사용자가 build_copy 호출 시 cb_arg로 넘긴 값.
	 * 읽는 자: process_channel_events가 callback_fn(arg, err_code)로 전달.
	 * 값 범위: 사용자 정의 포인터(NULL 가능). 동기화: 위와 동일. */
};

/*
 * [한국어] struct ae4dma_cmd_queue — H/W 큐(0..15) 한 개에 대한 호스트 측
 *  관리 컨텍스트. 큐의 MMIO 레지스터 핸들, DMA 메모리 주소, 호스트 그림자 ring,
 *  생산/소비 인덱스 등을 모두 담는다.
 */
struct ae4dma_cmd_queue {
	volatile struct spdk_ae4dma_hwq_regs *regs;
	/* [한국어] 이 큐의 MMIO 레지스터 영역(BAR0의 (i+1) 슬롯)에 매핑된 포인터.
	 * 설정자: `ae4dma_channel_start`가 io_regs 베이스 + 큐오프셋으로 계산해 설정.
	 * 읽는 자: 큐 활성화/플러시/상태 조회 시 spdk_mmio_*로 접근.
	 * 값 범위: 유효한 MMIO 매핑 주소. volatile은 컴파일러 최적화에 의한 read 생략 방지.
	 * 동기화: MMIO 4바이트 단위 액세스는 PCIe ordering 모델에 따라 H/W가 보장. */

	/* Queue base address */
	struct spdk_ae4dma_desc *qbase_addr;
	/* [한국어] 큐의 디스크립터 링 베이스(가상 주소). hugepage 기반 DMA
	 * 가능 메모리(`spdk_dma_zmalloc`)로 할당되어 H/W가 PA를 통해 접근.
	 * 설정자: channel_start가 할당. 읽는 자: prep_copy가 [write_index]에 디스크립터 적재. */

	struct ae4dma_descriptor *ring;
	/* [한국어] 호스트 측 그림자 ring(콜백 정보 보관). qbase_addr와 인덱스가 1:1 매핑.
	 * 설정자: channel_start가 calloc으로 할당. 읽는 자: prep_copy가 콜백 저장,
	 *   process_channel_events가 호출. */

	uint64_t tail;
	/* [한국어] 호스트가 다음에 처리(완료 회수)할 디스크립터 인덱스(소비자 포인터).
	 * 설정자: process_channel_events가 한 디스크립터 처리 후 (tail+1)%32로 갱신.
	 * 읽는 자: 다음 폴링 사이클 진입 시. 값 범위: 0..31. */

	unsigned int queue_size;
	/* [한국어] 이 큐의 디스크립터 영역 바이트 크기(=32*32=1024 byte).
	 * 설정자: channel_start가 AE4DMA_QUEUE_SIZE(...)로 설정. */

	uint64_t qring_buffer_pa;
	/* [한국어] qbase_addr의 물리 주소(spdk_vtophys 결과).
	 * H/W에 큐 베이스를 알리기 위해 qbase_lo/qbase_hi 레지스터에 분할 기록. */

	uint64_t qdma_tail;
	/* [한국어] H/W에 보고된 큐 베이스 PA(qring_buffer_pa의 사본).
	 * 설정자: channel_start가 qring_buffer_pa로 초기화. */

	/* Queue Statistics */
	uint32_t write_index;
	/* [한국어] 호스트가 다음에 적재할 디스크립터 인덱스(생산자 포인터).
	 * 설정자: prep_copy가 디스크립터 추가 후 +1 (mod 32). flush 시 H/W의 write_idx 레지스터에 미러.
	 * 읽는 자: prep_copy(다음 슬롯 결정), flush(MMIO 기록 값). */

	uint32_t ring_buff_count;
	/* [한국어] 현재 큐에서 H/W에 의해 아직 소비되지 않은 디스크립터 수(in-flight).
	 * 설정자: prep_copy +1, process_channel_events -1. 읽는 자: full 검사.
	 * 값 범위: 0 ~ 32. AE4DMA_DESCRIPTORS_PER_CMDQ-4 이상이면 full로 간주. */
};

/*
 * [한국어] struct spdk_ae4dma_chan — AE4DMA 채널(PCIe device 1대) 핸들.
 *  외부에는 불투명하게 보이지만 내부에서는 16개 큐와 PCI 핸들, MMIO 베이스를 모두 담는다.
 *  spdk_ae4dma_probe → spdk_ae4dma_attach → 사용자에게 핸들 전달, 사용자는 이 핸들을
 *  build_copy/flush/process_events에 인자로 넘긴다.
 */
struct spdk_ae4dma_chan {
	/* Opaque handle to upper layer */
	struct    spdk_pci_device *device;
	/* [한국어] SPDK PCI 추상화 핸들. spdk_pci_enumerate가 발견한 디바이스.
	 * 설정자: ae4dma_attach. 읽는 자: 매핑 함수, BAR map/unmap, 설정공간 read/write.
	 * 동기화: 채널 lifetime 동안 불변. */

	uint64_t  max_xfer_size;
	/* [한국어] 한 번에 H/W가 처리 가능한 최대 전송 바이트(현 구현은 1ULL<<32).
	 * 설정자: channel_start. 읽는 자: prep_copy의 assert로 검증. */

	/* I/O area used for device communication */
	void *io_regs;
	/* [한국어] PCIe BAR0의 매핑된 가상 주소 베이스. 큐별 regs는 이 베이스에서 오프셋 계산.
	 * 설정자: ae4dma_map_pci_bar (spdk_pci_device_map_bar 결과).
	 * 읽는 자: channel_start가 큐별 레지스터 주소 계산 시. */

	struct ae4dma_cmd_queue cmd_q[AE4DMA_MAX_HW_QUEUES];
	/* [한국어] 16개 H/W 큐 각각에 대한 호스트 측 메타.
	 * 설정자: channel_start가 q_per_eng만큼 순차 초기화.
	 * 읽는 자: build_copy/flush/process_events에서 hwq_id로 인덱싱. */

	unsigned int cmd_q_count;
	/* [한국어] 실제 활성화된 큐 개수 (H/W에 기록된 q_per_eng 값).
	 * 설정자: channel_start가 큐를 하나 초기화할 때마다 +1.
	 * 읽는 자: detach 시 모든 큐 해제. */

	uint32_t	dma_capabilities;
	/* [한국어] 이 엔진이 지원하는 DMA 기능 비트마스크
	 * (현 정의: SPDK_AE4DMA_ENGINE_COPY_SUPPORTED=0x1).
	 * 설정자: channel_start. 읽는 자: 모듈 코드(상위)에서 fill 등 미지원 op 차단 가능. */

	/* tailq entry for attached_chans */
	TAILQ_ENTRY(spdk_ae4dma_chan)	tailq;
	/* [한국어] g_ae4dma_driver.attached_chans 리스트의 엔트리.
	 * 설정자: ae4dma_enum_cb가 INSERT_TAIL. 읽는 자: probe 중복 체크/detach. */
};

/* This function verifies if the command queue is full */
/*
 * [한국어]
 * ae4dma_desc_cmdq_full - 명령 큐가 거의 가득 찼는지(=새 디스크립터를 더 받을
 *                         여유가 4 슬롯 미만) 검사.
 *
 * @count: 현재 in-flight 디스크립터 수(ring_buff_count). 0..32.
 * @return: true=가득참(여유 4 미만, 적재 거부 권장), false=적재 가능.
 *
 * 디자인 노트: H/W 동작상 마지막 몇 슬롯을 비워둬야 안정적인 처리가 가능하므로
 * 32-4 = 28을 임계로 사용. 실제 코드(spdk_ae4dma_build_copy)도 동일 임계 사용.
 *
 * 호출 컨텍스트: 채널 처리 스레드(보통 reactor) 내. 동시성 없음.
 */
static inline bool
ae4dma_desc_cmdq_full(uint8_t count)
{
	if (count >= (AE4DMA_DESCRIPTORS_PER_CMDQ - 4)) {
		/* [한국어] 여유 슬롯이 4 미만 — 큐가 거의 차서 추가 적재 시 H/W가 처리 못 할 위험. */
		return true;
	} else {
		/* [한국어] 적재 가능 — 일반 경로. */
		return false;
	}
}

/* This function verifies the number of queues that can be configured for a ae4dma device */
/*
 * [한국어]
 * ae4dma_config_queues_per_device - 요청된 큐 수가 H/W 한도(16)를 초과하는지 검증.
 *
 * @num_hw_queues: 호출자가 원하는 큐 수.
 * @return: false = 한도 이내(설정 가능), true = 한도 초과(설정 불가).
 *  주의: 반환 의미가 "OK 여부"가 아니라 "초과 여부"임 — channel_start에서
 *  if(!ae4dma_config_queues_per_device(...))로 분기.
 */
static inline bool
ae4dma_config_queues_per_device(uint8_t num_hw_queues)
{
	if (num_hw_queues <= AE4DMA_MAX_HW_QUEUES) {
		/* [한국어] 16개 이하 — 설정 가능 → false 반환(=초과 아님). */
		return false;
	} else {
		/* [한국어] 16 초과 — 잘못된 입력 → true 반환. */
		return true;
	}
}

#endif /* __AE4DMA_INTERNAL_H__ */
