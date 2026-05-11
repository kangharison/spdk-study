/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

/**
 * AE4DMA specification definitions
 */

/*
 * [한국어 설명] AMD AE4DMA(엔진) 하드웨어 사양 헤더 (ae4dma_spec.h)
 *
 * === 파일의 역할 ===
 * AMD AE4DMA(Accelerated Engine 4 DMA, AMD Pensando/EPYC 계열의 임베디드
 * DMA 엔진) 하드웨어가 노출하는 PCIe MMIO 레지스터 레이아웃과 디스크립터
 * 포맷을 비트 정확하게 기술하는 사양 헤더이다. SPDK ae4dma 드라이버
 * (`lib/ae4dma/ae4dma.c`)는 이 헤더에 정의된 구조체를 그대로 디바이스
 * 메모리에 매핑/기록하여 큐를 구성하고, 디스크립터를 SQ에 적재해 DMA를 트리거하며,
 * 완료 상태(status, err_code)를 디스크립터의 dw1에서 읽어 호스트에 보고한다.
 * 본 파일은 순수 데이터 구조 선언만 담고 있으며 함수 정의는 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 다음 SPDK accel/ae4dma 스택에서 가장 하단 — 하드웨어 ABI 계층에 해당한다:
 *   [bdev / 사용자 앱]
 *      ↓ spdk_accel_submit_*()
 *   [accel framework (lib/accel/accel.c)]
 *      ↓ submit_tasks
 *   [ae4dma accel module (module/accel/ae4dma)]
 *      ↓ spdk_ae4dma_build_copy / flush
 *   [ae4dma 드라이버 (lib/ae4dma/ae4dma.c)]
 *      ↓ MMIO 쓰기 / 디스크립터 링 갱신
 *   [HARDWARE: AE4DMA 엔진 (이 헤더의 메모리 레이아웃을 사용)]
 * 실행 컨텍스트는 모두 호스트 유저스페이스(SPDK reactor 스레드)이며 커널
 * 우회로 PCIe BAR을 직접 매핑해 접근한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존하는 헤더: `spdk/stdinc.h`(uint*_t), `spdk/assert.h`(SPDK_STATIC_ASSERT).
 * - 본 헤더에 의존: `lib/ae4dma/ae4dma_internal.h`(드라이버 내부 타입 정의),
 *   `lib/ae4dma/ae4dma.c`(드라이버 구현), 추후 ae4dma accel 모듈.
 * - 데이터 흐름: 드라이버가 `spdk_ae4dma_desc` 32바이트 디스크립터를 hugepage
 *   기반 DMA 가능 메모리(`spdk_dma_zmalloc`)에 적재 → MMIO `write_idx`를 갱신해
 *   엔진에 신호 → 엔진이 큐 베이스(qbase_hi/lo)에서 디스크립터를 읽어 DMA 수행 →
 *   완료 후 `dw1.status`를 AE4DMA_DMA_DESC_COMPLETED로 갱신.
 * - 공유 자료구조: `spdk_ae4dma_hwq_regs`는 PCIe BAR0 위에 매핑되는 MMIO 레지스터
 *   영역의 모양 그대로(packed/aligned) 정의되며, `spdk_ae4dma_chan::cmd_q[i].regs`로
 *   포인터화되어 직접 read/write된다.
 *
 * === 주요 함수/구조체 요약 ===
 * (이 헤더는 함수 없이 자료구조와 매크로 상수만 정의)
 * - struct spdk_ae4dma_desc : 32바이트 H/W 디스크립터(8 dword). src/dst 64비트
 *   물리 주소를 hi/lo 32bit로 분할 저장하며 길이/제어/상태 필드를 포함.
 * - struct spdk_ae4dma_desc_dword0 / dword1 : 디스크립터 첫 두 dword의 비트
 *   필드 분해. byte0=control bits, dw1.status=완료 상태, dw1.err_code=에러 코드.
 * - struct spdk_ae4dma_hwq_regs : 큐별 MMIO 레지스터 32바이트. control_reg(enable),
 *   status_reg, max_idx/read_idx/write_idx, intr_status_reg, qbase_lo/hi.
 * - enum spdk_ae4dma_dma_status : 디스크립터 처리 상태(SUBMITTED/COMPLETED/ERROR 등).
 * - enum spdk_ae4dma_hwqueue_status : 하드웨어 큐 상태(EMPTY/FULL/NOT_EMPTY).
 * - 매크로: AE4DMA_MAX_HW_QUEUES(16개 큐), 각 디스크립터 word의 비트 정의.
 */

#ifndef SPDK_AE4DMA_SPEC_H
#define SPDK_AE4DMA_SPEC_H

/* [한국어] uint8/16/32_t 등 고정 폭 정수 타입을 사용하기 위해 포함.
 *  H/W ABI 정의에서는 폭 명시가 필수이므로 SPDK 표준 헤더로 통일. */
#include "spdk/stdinc.h"
/* [한국어] SPDK_STATIC_ASSERT 매크로 — 컴파일 타임에 디스크립터 크기를 32바이트로
 *  강제하기 위해 필요 (HW ABI 보장). */
#include "spdk/assert.h"

#ifdef __cplusplus
/* [한국어] C++에서 본 헤더를 인클루드해도 C 링크가 유지되도록 extern "C" 보호. */
extern "C" {
#endif

/*
 * An AE4DMA engine has 16 DMA queues. Each queue supports 32 descriptors
 */

/* [한국어] AE4DMA 엔진 1대당 최대 H/W 큐 개수(스펙으로 16개로 고정).
 *  큐 인덱스는 0~15. cmd_q[] 배열의 길이로 사용된다. */
#define AE4DMA_MAX_HW_QUEUES		16
/* [한국어] 큐 인덱스 시작값(0) — 디스크립터 링 초기화 시 베이스 인덱스. */
#define AE4DMA_QUEUE_START_INDEX	0
/* [한국어] control_reg.control_raw에 쓸 값. queue_enable 비트 1만 set.
 *  드라이버가 채널 시작 시 큐를 활성화하기 위해 MMIO 기록. */
#define AE4DMA_CMD_QUEUE_ENABLE	0x1

/** Common to all queues */
/* [한국어] 모든 큐에 공통으로 적용되는 설정 레지스터의 BAR 내 오프셋(0x00).
 *  `q_per_eng`(엔진당 활성 큐 수)를 여기에 기록한다. */
#define AE4DMA_COMMON_CONFIG_OFFSET 0x00
/* [한국어] AE4DMA가 노출하는 PCIe BAR 인덱스(BAR0). spdk_pci_device_map_bar의 인자. */
#define AE4DMA_PCIE_BAR 0

/* Descriptor status */
/*
 * [한국어] enum spdk_ae4dma_dma_status — 디스크립터의 dw1.status 필드 값 정의.
 *  하드웨어가 디스크립터를 처리해 가는 상태 머신을 표현한다.
 */
enum spdk_ae4dma_dma_status {
	AE4DMA_DMA_DESC_SUBMITTED = 0,
	/* [한국어] 호스트가 디스크립터를 적재하고 write_idx를 갱신한 직후 상태.
	 * 설정자: 호스트 드라이버(`ae4dma_prep_copy`)가 0으로 클리어.
	 * 읽는 자: `ae4dma_process_channel_events`가 이 값을 보면 아직 처리 중이라 판단해 break.
	 * 동기화: H/W가 비동기로 1/2/3/4로 갱신 — MMIO 메모리 가시성에 의존. */

	AE4DMA_DMA_DESC_VALIDATED = 1,
	/* [한국어] 엔진이 디스크립터 필드를 검증한 단계 (중간 상태).
	 * 설정자: H/W. 읽는 자: 거의 사용 안 함. */

	AE4DMA_DMA_DESC_PROCESSED = 2,
	/* [한국어] 엔진이 데이터 전송을 시작/처리 중인 단계 (중간 상태).
	 * 설정자: H/W. */

	AE4DMA_DMA_DESC_COMPLETED = 3,
	/* [한국어] 정상 완료. 호스트가 콜백을 호출할 수 있는 상태.
	 * 설정자: H/W. 읽는 자: process_channel_events가 != COMPLETED면 에러로 판정. */

	AE4DMA_DMA_DESC_ERROR = 4,
	/* [한국어] 처리 중 에러 발생. dw1.err_code에 상세 사유.
	 * 설정자: H/W. 읽는 자: 드라이버가 SPDK_ERRLOG로 보고. */
};

/* HW Queue status */
/*
 * [한국어] enum spdk_ae4dma_hwqueue_status — status_reg.queue_status 비트필드 값.
 *  큐의 현재 상태(빈/꽉찬/비어있지않음)를 보고한다.
 */
enum spdk_ae4dma_hwqueue_status {
	AE4DMA_HWQUEUE_EMPTY = 0,
	/* [한국어] 큐에 처리할 디스크립터가 없음. 새 작업을 적재해야 함.
	 * 설정자: H/W. 읽는 자: 드라이버 디버그/검증. */

	AE4DMA_HWQUEUE_FULL = 1,
	/* [한국어] 큐가 꽉 차서 더 이상 디스크립터를 받을 수 없음.
	 * 호스트는 ring_buff_count 검사로 사전에 차단. */

	AE4DMA_HWQUEUE_NOT_EMPTY = 4
	/* [한국어] 큐에 처리 중/처리 대기 디스크립터가 존재함. 일반 동작 상태. */
};

/*
 * descriptor for AE4DMA commands
 * 8 32-bit words:
 * word 0: source memory type; destination memory type ; control bits
 * word 1: desc_id; error code; status
 * word 2: length
 * word 3: reserved
 * word 4: upper 32 bits of source pointer
 * word 5: low 32 bits of source pointer
 * word 6: upper 32 bits of destination pointer
 * word 7: low 32 bits of destination pointer
 */

/* AE4DMA Descriptor - DWORD0 - Controls bits: Reserved for future use */
/* [한국어] DWORD0 비트 0 — Stop-on-completion: 이 디스크립터 완료 시 큐 정지. */
#define AE4DMA_DWORD0_STOP_ON_COMPLETION	BIT(0)
/* [한국어] DWORD0 비트 1 — 완료 시 인터럽트 발생(현 SPDK 드라이버는 polling이라 사용 X). */
#define AE4DMA_DWORD0_INTERRUPT_ON_COMPLETION	BIT(1)
/* [한국어] DWORD0 비트 3 — 메시지(다중 디스크립터 묶음)의 시작 표기. */
#define AE4DMA_DWORD0_START_OF_MESSAGE		BIT(3)
/* [한국어] DWORD0 비트 4 — 메시지의 끝 표기. */
#define AE4DMA_DWORD0_END_OF_MESSAGE		BIT(4)
/* [한국어] DWORD0 비트 5..4 — 목적지 메모리 타입(host memory vs IO memory) 마스크. */
#define AE4DMA_DWORD0_DESTINATION_MEMORY_TYPE	GENMASK(5, 4)
/* [한국어] DWORD0 비트 7..6 — 소스 메모리 타입 마스크. */
#define AE4DMA_DWORD0_SOURCE_MEMEORY_TYPE	GENMASK(7, 6)

/* [한국어] dst가 일반 호스트 메모리일 때 (default 0). */
#define AE4DMA_DWORD0_DESTINATION_MEMORY_TYPE_MEMORY	0x0
/* [한국어] dst가 IO 메모리(MMIO)일 때 (비트4 set). */
#define AE4DMA_DWORD0_DESTINATION_MEMORY_TYPE_IOMEMORY	(1<<4)
/* [한국어] src가 일반 호스트 메모리일 때 (default). */
#define AE4DMA_DWORD0_SOURCE_MEMEORY_TYPE_MEMORY	0x0
/* [한국어] src가 IO 메모리일 때 (비트6 set). */
#define AE4DMA_DWORD0_SOURCE_MEMEORY_TYPE_IOMEMORY	(1<<6)

/*
 * [한국어] 디스크립터 첫 32비트(DWORD0)의 비트 분해.
 *  byte0=제어 비트(stop/intr/SOM/EOM 등), byte1=메모리 타입,
 *  timestamp=타임스탬프(엔진이 기록하는 16비트 카운터).
 *  현재 드라이버는 byte0만 0으로 클리어해 사용한다.
 */
struct spdk_ae4dma_desc_dword0 {
	uint8_t	byte0;
	/* [한국어] DWORD0의 하위 8비트 — 제어 비트 그룹 (STOP_ON_COMPLETION 등).
	 * 설정자: 호스트(`ae4dma_prep_copy`)가 0으로 클리어.
	 * 읽는 자: H/W가 디스크립터 처리 시 검사.
	 * 값 범위: 위의 AE4DMA_DWORD0_* 비트 OR 조합.
	 * 동기화: 디스크립터는 호스트→디바이스 단방향 쓰기, 한 디스크립터당 한 호스트 스레드. */

	uint8_t	byte1;
	/* [한국어] DWORD0의 비트 8..15 — src/dst 메모리 타입 인코딩 영역.
	 * 설정자/읽는 자/동기화: byte0과 동일. 현 구현은 0(=둘 다 호스트 메모리) 사용. */

	uint16_t timestamp;
	/* [한국어] DWORD0의 상위 16비트 — H/W가 기록하는 디스크립터 처리 시점.
	 * 설정자: H/W. 읽는 자: 디버그/추적용 (현 SPDK 드라이버는 미참조).
	 * 값 범위: 16비트 카운터. */
};

/*
 * [한국어] 디스크립터 두 번째 dword(DWORD1)의 비트 분해.
 *  status는 처리 상태, err_code는 실패 시 사유, desc_id는 디스크립터 식별자.
 */
struct spdk_ae4dma_desc_dword1 {
	uint8_t	status;
	/* [한국어] 디스크립터 처리 상태 (enum spdk_ae4dma_dma_status).
	 * 설정자: 호스트가 0(SUBMITTED)으로 클리어 → 이후 H/W가 갱신.
	 * 읽는 자: process_channel_events가 polling으로 확인.
	 * 값 범위: 0..4. 동기화: MMIO/메모리 가시성으로 동기 (volatile read). */

	uint8_t	err_code;
	/* [한국어] 실패 시 H/W 보고 에러 코드.
	 * 설정자: 호스트 0 클리어 후 H/W 갱신. 읽는 자: 드라이버가 로그/콜백에 전달.
	 * 값 범위: H/W 정의 코드 (스펙 문서 참조). */

	uint16_t desc_id;
	/* [한국어] 디스크립터 식별자 — 큐 내에서 의미 있는 식별 번호.
	 * 설정자: 호스트가 0으로 적재하거나 H/W가 갱신 (벤더에 따라 동작 상이).
	 * 현 SPDK 드라이버는 0으로 클리어. */
};

/*
 * [한국어] struct spdk_ae4dma_desc — AE4DMA 하드웨어 디스크립터(고정 32바이트).
 *  hugepage 기반 DMA 메모리 위에 32바이트 정렬로 배열되며, H/W는 cmd queue
 *  base 주소(qbase_hi:qbase_lo)에서 (write_idx % 32) 오프셋으로 읽는다.
 */
struct spdk_ae4dma_desc {
	struct spdk_ae4dma_desc_dword0 dw0;
	/* [한국어] DWORD0(제어/메모리 타입/타임스탬프). 위 분해 참고. */

	struct spdk_ae4dma_desc_dword1 dw1;
	/* [한국어] DWORD1(상태/에러/desc_id). 위 분해 참고. */

	uint32_t length;
	/* [한국어] 전송 바이트 길이. 한 디스크립터 단위로 H/W가 처리 가능한 최대 길이.
	 * 설정자: `ae4dma_prep_copy`가 사용자 요청 길이(seg_len)로 채움.
	 * 읽는 자: H/W. 값 범위: 1..max_xfer_size(드라이버에서 1ULL<<32로 설정). */

	uint32_t reserved;
	/* [한국어] 미래 확장을 위한 예약 영역(0으로 두는 것이 권장). */

	uint32_t src_lo;
	/* [한국어] 소스 물리 주소의 하위 32비트.
	 * 설정자: lower_32_bits(spdk_vtophys(src)). 읽는 자: H/W. */

	uint32_t src_hi;
	/* [한국어] 소스 물리 주소의 상위 32비트(64비트 PA 지원).
	 * 설정자: upper_32_bits(spdk_vtophys(src)). 읽는 자: H/W. */

	uint32_t dst_lo;
	/* [한국어] 목적지 물리 주소의 하위 32비트. 설정자/읽는 자: src와 동일. */

	uint32_t dst_hi;
	/* [한국어] 목적지 물리 주소의 상위 32비트. */
};
/* [한국어] 컴파일 타임 검증 — 구조체 크기가 정확히 32바이트(=8 dword)임을
 *  보장. H/W ABI가 깨지면 빌드 실패. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_ae4dma_desc) == 32, "incorrect ae4dma_hw_desc layout");

/*
 * Registers for each queue :4 bytes length
 * Effective address : offset + reg
 */

/*
 * [한국어] struct spdk_ae4dma_hwq_regs — 한 큐(0..15)의 MMIO 레지스터 묶음.
 *  PCIe BAR0의 (i+1) 슬롯에 매핑되는 32바이트 영역 (8 dword). packed/aligned로
 *  H/W 레이아웃과 1:1 일치하도록 강제한다. 모든 접근은 `spdk_mmio_read_4 /
 *  spdk_mmio_write_4` 같은 정렬된 32비트 MMIO 헬퍼를 사용해야 함.
 */
struct spdk_ae4dma_hwq_regs {
	union {
		uint32_t control_raw;
		/* [한국어] control_reg를 32비트 그대로 읽기/쓰기 위한 alias.
		 * 드라이버는 이 alias로 AE4DMA_CMD_QUEUE_ENABLE(=0x1)을 기록한다. */
		struct {
			uint32_t queue_enable: 1;
			/* [한국어] 비트0 — 큐 활성화 플래그. 1 쓰면 H/W가 디스크립터를 fetch 시작. */
			uint32_t reserved_internal: 31;
			/* [한국어] 비트1..31 — 미래 확장용 예약. */
		} control;
	} control_reg;
	/* [한국어] 큐 활성/비활성 제어 레지스터(32비트).
	 * 설정자: `ae4dma_channel_start`가 채널 시작 시 1로 기록.
	 * 읽는 자: H/W. */

	union {
		uint32_t status_raw;
		/* [한국어] status를 32비트로 raw read 하기 위한 alias. */
		struct {
			uint32_t reserved0: 1;
			/* [한국어] 비트0 — 예약. */
			uint32_t queue_status: 2; /* 0–empty, 1–full, 2–stopped, 3–error , 4–Not Empty */
			/* [한국어] 비트1..2 — 큐 상태 (위 enum spdk_ae4dma_hwqueue_status).
			 * NOTE: 주석상 4까지 표기되지만 비트필드 폭이 2라 0..3만 표현 가능 — H/W 문서/구현
			 * 차이일 수 있음. */
			uint32_t reserved1: 21;
			/* [한국어] 비트3..23 — 예약. */
			uint32_t interrupt_type: 4;
			/* [한국어] 비트24..27 — 인터럽트 타입(현재 polling 모드라 미사용). */
			uint32_t reserved2: 4;
			/* [한국어] 비트28..31 — 예약. */
		} status;
	} status_reg;
	/* [한국어] 큐 상태 레지스터.
	 * 설정자: H/W. 읽는 자: 진단/디버그용으로 드라이버가 read 가능. */

	uint32_t max_idx;
	/* [한국어] 큐의 최대 인덱스(=한 큐의 디스크립터 개수, AE4DMA_DESCRIPTORS_PER_CMDQ=32).
	 * 설정자: `ae4dma_channel_start`가 32 기록. 읽는 자: H/W가 wrap-around 기준. */

	uint32_t read_idx;
	/* [한국어] 엔진이 다음에 읽을 디스크립터 인덱스(H/W가 갱신, host는 read만).
	 * 호스트의 tail 추적과 동기화에 사용. */

	uint32_t write_idx;
	/* [한국어] 호스트가 다음에 쓸 디스크립터 인덱스(host가 갱신, H/W가 trigger 신호로 사용).
	 * `spdk_ae4dma_flush()`가 이 레지스터를 MMIO write 하면 엔진이 fetch 시작. */

	union {
		uint32_t intr_status_raw;
		/* [한국어] 인터럽트 상태 raw — 1 쓰기로 W1C(write-1-to-clear) 가능. */
		struct {
			uint32_t intr_status: 1;
			/* [한국어] 비트0 — 인터럽트 보류 여부. 드라이버는 disable로 0x1을 W1C. */
			uint32_t reserved: 31;
			/* [한국어] 예약. */
		} intr_status;
	} intr_status_reg;
	/* [한국어] 인터럽트 상태/제어. SPDK polling 모드에서는 0x1 W1C로 비활성화. */

	uint32_t qbase_lo;
	/* [한국어] 큐 디스크립터 링의 DMA 베이스 물리 주소(하위 32비트).
	 * 설정자: `ae4dma_channel_start`가 spdk_vtophys(qbase_addr)의 lower_32_bits 기록. */

	uint32_t qbase_hi;
	/* [한국어] 큐 디스크립터 링의 DMA 베이스 물리 주소(상위 32비트). */

} __attribute__((packed)) __attribute__((aligned));
/* [한국어] packed: 패딩 없이 H/W 메모리맵과 1:1 일치 강제.
 *  aligned: 컴파일러 기본 정렬을 H/W 요구에 맞게 부여. */


#ifdef __cplusplus
}
#endif

#endif /* SPDK_AE4DMA_SPEC_H */
