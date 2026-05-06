/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * I/OAT specification definitions
 */

/*
 * [한국어 설명] Intel I/OAT(I/O Acceleration Technology) 와이어 포맷 정의 헤더 (ioat_spec.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 Intel I/OAT DMA 엔진과 호스트 소프트웨어가 주고받는 “원시 와이어 포맷” —
 * 채널 MMIO 레지스터 레이아웃, 64바이트 descriptor 구조, 채널 명령/상태 비트, capability
 * 비트 등 — 을 비트 단위로 정확하게 정의한다. lib/ioat 의 구현부는 이 헤더를 포함해 raw
 * 메모리 위에 캐스팅·비트 조작·MMIO read/write 를 수행한다.
 * 본 파일은 “스펙 = 하드웨어 계약” 이므로 절대로 임의 변경되어서는 안 되며, 구조체 필드 순서·
 * 패딩·정렬은 인텔 IOAT 데이터시트(External Design Specification)의 와이어 정의를
 * 그대로 반영한다. SPDK_STATIC_ASSERT 로 sizeof(union spdk_ioat_hw_desc)==64 를 강제한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 본 헤더는 lib/ioat 와 module/accel/ioat 가 공통으로 의존하는 “HW spec layer” 다.
 *   [include/spdk/ioat.h]              ← 사용자 공개 API (논리 함수 시그니처)
 *           ↓ 사용
 *   [lib/ioat/ioat.c]                  ← API 구현체
 *           ↓ raw 메모리 캐스팅·MMIO write
 *   [include/spdk/ioat_spec.h] (THIS) ← 와이어 포맷
 *           ↓ 메모리/MMIO 매칭
 *   [I/OAT Hardware (PCIe DMA engine)]
 * 이 헤더에 정의된 비트 레이아웃은 IOAT v3.0 / v3.3 변종의 데이터시트에 정확히 대응하며,
 * 잘못된 비트 위치를 정의하면 하드웨어가 디스크립터를 오해해 silent corruption 또는 채널
 * halted 상태(CHANSTS_HALTED)가 발생한다.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/stdinc.h: uint*_t 정수 타입.
 * - spdk/assert.h: SPDK_STATIC_ASSERT 매크로 — 컴파일 타임 sizeof 검증으로 구조체 크기·
 *   레이아웃을 강제한다(특히 union spdk_ioat_hw_desc 가 64B 인지).
 * - lib/ioat/ioat.c, lib/ioat/ioat_internal.h: 본 헤더의 타입을 직접 사용하여 ring 슬롯
 *   포인터를 union spdk_ioat_hw_desc * 로 캐스팅, op 별로 dma/fill/xor 변종 union 멤버를
 *   골라 사용.
 * - test/unit/lib/ioat/, examples/ioat/: 본 헤더의 비트 정의를 사용해 테스트·예제 작성.
 * 데이터 흐름: 사용자 build_copy 호출 → lib/ioat 가 본 헤더의 spdk_ioat_dma_hw_desc 의
 * src_addr/dest_addr/control 비트를 채움 → spdk_ioat_flush 가 spdk_ioat_registers 의
 * dmacount 레지스터에 MMIO write → 하드웨어가 chainaddr 가리키는 디스크립터부터 next 체인을
 * 따라가며 DMA 실행 → 완료 시 chancmp 메모리에 last-completed descriptor 의 IOVA 기록.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_ioat_registers: 채널 MMIO 레지스터 0x00~0xAC 영역의 공식 레이아웃. packed.
 *   chancnt/xfercap/cbver/dmacapability(능력 비트), chanctrl(제어), chancmd(명령),
 *   dmacount(도어벨), chansts(상태), chainaddr(첫 디스크립터), chancmp(완료 기록 영역),
 *   chanerr/chanerrmask(에러).
 * - struct spdk_ioat_generic_hw_desc: op-agnostic 64B descriptor 일반형. op 비트로 분기.
 * - struct spdk_ioat_dma_hw_desc: op=COPY(0x00) 디스크립터.
 * - struct spdk_ioat_fill_hw_desc: op=FILL(0x01) 디스크립터(src 자리에 패턴 데이터 직접).
 * - struct spdk_ioat_xor_hw_desc / xor_ext: op=XOR/XOR_VAL 변종(RAID 가속).
 * - struct spdk_ioat_pq_hw_desc / pq_ext / pq_update: op=PQ(P/Q parity, RAID6).
 * - union spdk_ioat_hw_desc: 모든 변종을 64B 슬롯에 packing 하는 union — ring 의 한 슬롯.
 * - 매크로: SPDK_IOAT_OP_COPY/FILL/XOR/PQ 등 op 코드, CHANSTS_*/CHANCMD_* 상태/명령 비트,
 *   DMACAP_* capability 비트.
 */

/* [한국어] include guard. */
#ifndef SPDK_IOAT_SPEC_H
#define SPDK_IOAT_SPEC_H

/* [한국어] 표준 정수형(uint8/16/32/64_t) 사용을 위해 포함. 와이어 포맷이므로 정확한 비트 폭이
 * 필수. */
#include "spdk/stdinc.h"

/* [한국어] C++ 컴파일 시 C 링키지 보장. */
#ifdef __cplusplus
extern "C" {
#endif

/* [한국어] SPDK 의 컴파일 타임 어서션 매크로 SPDK_STATIC_ASSERT 를 사용하기 위해 포함.
 * 본 파일은 union spdk_ioat_hw_desc 가 정확히 64B 인지를 컴파일 시 검증한다. 만약 누군가
 * 구조체 필드를 추가/제거해 크기가 깨지면 빌드 자체가 실패하도록 만들어 silent ABI 깨짐을 방지. */
#include "spdk/assert.h"

/* [한국어] PCI configuration space 의 채널 인터럽트 영역 오프셋 — IOAT 의 PCIe MSI/INTx
 * 설정 시 사용. 0x180 은 데이터시트 정의. lib/ioat 가 PCI config write 시 참조. */
#define SPDK_IOAT_PCI_CHANERR_INT_OFFSET	0x180

/* [한국어] 인터럽트 컨트롤러의 master interrupt enable 비트. SPDK 는 polled-mode 라
 * 인터럽트를 끄거나 무시하지만, 일부 초기화 경로에서 master enable 을 설정해야 채널이
 * 정상 동작하므로 정의해 둔다. */
#define SPDK_IOAT_INTRCTRL_MASTER_INT_EN	0x01

/* [한국어] cbver(Crystal Beach version) 레지스터에 매칭되는 IOAT 하드웨어 버전 코드.
 * v3.0 = 0x30 — Sandy Bridge ~ Haswell 세대. */
#define SPDK_IOAT_VER_3_0                0x30
/* [한국어] v3.3 = 0x33 — Broadwell ~ Skylake / 일부 Ice Lake 세대. 일부 능력(DIF, BFILL 등)
 * 추가. lib/ioat 가 버전을 확인하여 워크어라운드 / 기능 분기 결정. */
#define SPDK_IOAT_VER_3_3                0x33

/* DMA Channel Registers */
/* [한국어] 이하 SPDK_IOAT_CHANCTRL_* 는 chanctrl(0x80) 레지스터 비트마스크.
 * lib/ioat 가 채널을 활성/리셋·snoop·인터럽트 동작을 설정할 때 OR 연산으로 합성해 MMIO write. */

/* [한국어] 채널 우선순위 비트필드 마스크(15:12). 여러 채널이 메모리 컨트롤러 자원을 두고 경쟁할
 * 때 우선순위를 설정하는 데 사용. 0=lowest, 0xF=highest. */
#define SPDK_IOAT_CHANCTRL_CHANNEL_PRIORITY_MASK	0xF000

/* [한국어] DCA(Direct Cache Access) 완료 기록 활성. 완료 메모리 쓰기를 캐시로 직접 routing
 * 하여 호스트 폴링이 캐시 히트하도록 함. */
#define SPDK_IOAT_CHANCTRL_COMPL_DCA_EN		0x0200

/* [한국어] 채널 사용 중 비트. 1 = 다른 드라이버(커널 등)가 이미 점유. SPDK 가 attach 시 이
 * 비트를 검사하여 충돌 여부 판단. */
#define SPDK_IOAT_CHANCTRL_CHANNEL_IN_USE		0x0100

/* [한국어] 디스크립터 자체에 대한 PCIe snoop 제어 — 디스크립터 메모리는 코히런트 영역이므로
 * 보통 켜둔다(snoop 활성). */
#define SPDK_IOAT_CHANCTRL_DESCRIPTOR_ADDR_SNOOP_CONTROL	0x0020

/* [한국어] 에러 발생 시 인터럽트 enable. SPDK 는 polled-mode 라 보통 끄지만, 디버깅 빌드에서
 * 사용 가능. */
#define SPDK_IOAT_CHANCTRL_ERR_INT_EN		0x0010

/* [한국어] 임의 에러 발생 시 채널 자동 abort. 한 디스크립터 에러로 인해 chain 전체가 멈춰
 * 후속 처리를 명확히 하도록 함. */
#define SPDK_IOAT_CHANCTRL_ANY_ERR_ABORT_EN		0x0008

/* [한국어] 에러 발생 시에도 completion 메모리에 결과를 기록 — 호스트가 폴링으로 에러를 인지
 * 가능. polled-mode 에서 권장. */
#define SPDK_IOAT_CHANCTRL_ERR_COMPLETION_EN		0x0004

/* [한국어] 인터럽트 재무장(rearm) 비트 — 인터럽트 모드에서 한 번의 인터럽트 후 다음 이벤트를
 * 받으려면 필요. polled-mode 무관. */
#define SPDK_IOAT_CHANCTRL_INT_REARM			0x0001

/* DMA Channel Capabilities */
/* [한국어] 이하 SPDK_IOAT_DMACAP_* 는 dmacapability(0x10) 레지스터의 비트들 — 채널이
 * 지원하는 op 종류를 알린다. lib/ioat 가 attach 시 이 레지스터를 읽어 supports_opcode
 * 매핑 테이블을 구성한다. */

/* [한국어] PB(Page Break) 능력 — src/dst page 경계 자동 분할 지원. */
#define	SPDK_IOAT_DMACAP_PB		(1 << 0)

/* [한국어] DCA(Direct Cache Access) 능력 — 완료 기록을 호스트 캐시에 직접 라이트. */
#define	SPDK_IOAT_DMACAP_DCA		(1 << 4)

/* [한국어] BFILL(Block Fill) 능력 — op=FILL(0x01) 가능 여부. 비트가 꺼지면 fill 사용 불가. */
#define	SPDK_IOAT_DMACAP_BFILL		(1 << 6)

/* [한국어] XOR 능력 — op=XOR(0x87)/XOR_VAL(0x88) 사용 가능 여부. RAID5 가속. */
#define	SPDK_IOAT_DMACAP_XOR		(1 << 8)

/* [한국어] PQ 능력 — op=PQ(0x89)/PQ_VAL(0x8a)/PQ_UP(0x8b) 사용 가능 여부. RAID6 가속. */
#define	SPDK_IOAT_DMACAP_PQ		(1 << 9)

/* [한국어] DMA + DIF(Data Integrity Field) 가속 능력 — T10-DIF 프로텍션 정보를 DMA 와
 * 함께 처리. NVMe PI 와 연관. */
#define	SPDK_IOAT_DMACAP_DMA_DIF	(1 << 10)

/* [한국어] I/OAT 채널 MMIO 레지스터 영역(BAR 의 채널별 페이지)의 와이어 포맷.
 * packed + aligned 속성으로 컴파일러가 패딩을 추가하지 않도록 강제 — 실제 PCIe MMIO 와
 * 1:1 대응되어야 한다.
 * 설정자: lib/ioat 가 MMIO read/write 시 본 구조체로 캐스팅된 포인터로 접근.
 * 읽는 자: 동일.
 * 동기화: MMIO 영역은 PCIe 트랜잭션 단위로 ordering 이 보장되며, 호스트에서는 volatile
 * 캐스팅 또는 spdk_mmio_write_*() 헬퍼로 접근. */
struct spdk_ioat_registers {
	uint8_t		chancnt;
	/* [한국어] 채널 개수(읽기 전용) — 한 IOAT 디바이스에 노출된 채널 수.
	 * 설정자: 하드웨어 reset 시점에 자동 채워짐.
	 * 읽는 자: spdk_ioat_probe 가 attach 진행 시 채널 수 enumeration. */

	uint8_t		xfercap;
	/* [한국어] 단일 디스크립터의 transfer capability — 한 디스크립터가 옮길 수 있는 최대 바이트
	 * 수의 log2 인코딩(예: 0x14 = 1MB, 0x20 = 4MB ...). lib/ioat 가 큰 복사 분할 단위로 사용. */

	uint8_t		genctrl;
	/* [한국어] 일반 제어 레지스터 — 칩 reset 등 글로벌 동작. 잘 사용되지 않음. */

	uint8_t		intrctrl;
	/* [한국어] 인터럽트 컨트롤 — SPDK_IOAT_INTRCTRL_MASTER_INT_EN 비트 등을 제어. polled-mode
	 * 에서는 master enable 만 켜고 무시. */

	uint32_t	attnstatus;
	/* [한국어] attention status — 채널들 중 인터럽트가 펜딩된 채널 비트맵(채널당 1비트). polled
	 * 에서는 무시. */

	uint8_t		cbver;		/* 0x08 */
	/* [한국어] Crystal Beach version 코드(0x30=v3.0, 0x33=v3.3). lib/ioat 가 attach 시 이를
	 * 읽어 SPDK_IOAT_VER_3_0/3_3 과 비교, 워크어라운드 분기. */

	uint8_t		reserved4[0x3]; /* 0x09 */
	/* [한국어] reserved 패딩 — 다음 16비트 정렬을 위해 3바이트. 절대 read/write 금지. */

	uint16_t	intrdelay;	/* 0x0C */
	/* [한국어] 인터럽트 지연(merge) 설정 — coalescing 미세조정. polled-mode 에서는 의미 없음. */

	uint16_t	cs_status;	/* 0x0E */
	/* [한국어] 컨트롤러 상태 비트(글로벌). */

	uint32_t	dmacapability;	/* 0x10 */
	/* [한국어] 채널이 지원하는 op 비트마스크 — SPDK_IOAT_DMACAP_* 비트들의 OR.
	 * 설정자: 하드웨어 reset 시 자동 채움.
	 * 읽는 자: lib/ioat 가 attach 시 1회 읽어 캐시. */

	uint8_t		reserved5[0x6C]; /* 0x14 */
	/* [한국어] reserved 영역(0x14~0x7F) — 채널 제어 레지스터 시작 0x80 까지의 갭. */

	uint16_t	chanctrl;	/* 0x80 */
	/* [한국어] 채널 컨트롤 — SPDK_IOAT_CHANCTRL_* 비트 OR. attach 시 적절한 비트로 초기화. */

	uint8_t		reserved6[0x2];	/* 0x82 */
	/* [한국어] reserved 패딩(0x82~0x83). */

	uint8_t		chancmd;	/* 0x84 */
	/* [한국어] 채널 명령 — SPDK_IOAT_CHANCMD_RESET/SUSPEND. detach/error-recovery 시 사용. */

	uint8_t		reserved3[1];	/* 0x85 */
	/* [한국어] reserved 패딩 1바이트. */

	uint16_t	dmacount;	/* 0x86 */
	/* [한국어] DMACOUNT 도어벨 — 호스트가 새 디스크립터를 추가한 뒤 이 값에 “총 디스크립터 수
	 * delta” 를 더해 MMIO write 하면 하드웨어가 진행 시작.
	 * 설정자: spdk_ioat_flush 또는 submit_* 가 갱신.
	 * 읽는 자: 하드웨어. 호스트는 read 하지 않음(write-only 도어벨 모델). */

	uint64_t	chansts;	/* 0x88 */
	/* [한국어] 채널 상태 — 하위 3비트 SPDK_IOAT_CHANSTS_STATUS(ACTIVE/IDLE/SUSPENDED/
	 * HALTED/ARMED), 그리고 last-completed descriptor 의 IOVA(상위 비트, 64B 정렬이라
	 * 하위 6비트는 status).
	 * 설정자: 하드웨어가 진행 상황에 따라 갱신.
	 * 읽는 자: spdk_ioat_process_events 가 폴링. */

	uint64_t	chainaddr;	/* 0x90 */
	/* [한국어] 채널이 다음에 가져갈 디스크립터의 IOVA(첫 디스크립터). attach 시 ring 의 헤드 IOVA
	 * 로 1회 설정. 이후 하드웨어가 next 포인터로 자동 이동. */

	uint64_t	chancmp;	/* 0x98 */
	/* [한국어] 완료 기록 메모리의 IOVA(8바이트 정렬 필수) — 하드웨어가 완료된 디스크립터의 주소를
	 * 이 메모리에 쓴다. 호스트는 chancmp 가 가리키는 메모리를 폴링하여 진행 확인.
	 * 설정자: lib/ioat 가 attach 시 spdk_zmalloc 한 8바이트 영역의 IOVA 를 한 번 기록.
	 * 읽는 자: 하드웨어가 진행 시 갱신, 호스트가 process_events 에서 read. */

	uint8_t		reserved2[0x8];	/* 0xA0 */
	/* [한국어] reserved 패딩 8바이트. */

	uint32_t	chanerr;	/* 0xA8 */
	/* [한국어] 에러 비트 — 하드웨어가 디스크립터 처리 중 만난 에러 종류(주소 정렬 위반, 미지정 op,
	 * 메모리 fault 등) 가 비트별로 set. 1 을 write 하면 클리어(W1C).
	 * 설정자: 하드웨어가 set, lib/ioat 가 W1C 로 클리어.
	 * 읽는 자: process_events 가 에러 검출 시 read. */

	uint32_t	chanerrmask;	/* 0xAC */
	/* [한국어] chanerr 비트별 mask — 1 이면 해당 에러 원인을 무시(인터럽트 안 발생). polled
	 * 에서는 보통 디폴트. */
} __attribute__((packed)) __attribute__((aligned));

/* [한국어] chancmd 레지스터에 쓸 명령 비트들. */

/* [한국어] CHANCMD_RESET — 채널 강제 리셋(in-flight 디스크립터 abort, 상태 초기화).
 * lib/ioat 가 detach 또는 hard-error 복구 시 사용. */
#define SPDK_IOAT_CHANCMD_RESET			0x20

/* [한국어] CHANCMD_SUSPEND — 채널 일시 정지(현재 디스크립터까지 처리 후 IDLE). 우아한 종료. */
#define SPDK_IOAT_CHANCMD_SUSPEND		0x04

/* [한국어] chansts 의 status 필드 마스크(하위 3비트). */
#define SPDK_IOAT_CHANSTS_STATUS		0x7ULL

/* [한국어] status=0: ACTIVE — 디스크립터 처리 중. */
#define SPDK_IOAT_CHANSTS_ACTIVE		0x0
/* [한국어] status=1: IDLE — 처리할 디스크립터 없음(ring 비었거나 dmacount 미증가). */
#define SPDK_IOAT_CHANSTS_IDLE			0x1
/* [한국어] status=2: SUSPENDED — CHANCMD_SUSPEND 후 멈춘 상태. resume 가능. */
#define SPDK_IOAT_CHANSTS_SUSPENDED		0x2
/* [한국어] status=3: HALTED — 에러로 멈춤. chanerr 확인 + RESET 필요. */
#define SPDK_IOAT_CHANSTS_HALTED		0x3
/* [한국어] status=4: ARMED — 인터럽트 모드에서의 무장 상태. polled 무관. */
#define SPDK_IOAT_CHANSTS_ARMED			0x4

/* [한국어] chansts 의 unaffiliated error 비트 — 특정 디스크립터가 아니라 채널 자체 문제. */
#define SPDK_IOAT_CHANSTS_UNAFFILIATED_ERROR	0x8ULL

/* [한국어] chansts 의 soft error 비트 — recoverable 에러. */
#define SPDK_IOAT_CHANSTS_SOFT_ERROR		0x10ULL

/* [한국어] chansts 의 상위 비트는 last completed descriptor 의 IOVA(64B 정렬이라 하위 6비트는
 * status/error). 이 마스크로 상위만 추출. */
#define SPDK_IOAT_CHANSTS_COMPLETED_DESCRIPTOR_MASK	(~0x3FULL)

/* [한국어] chancmp(완료 기록 메모리)는 64-bit(8B) 정렬 필수 — 하드웨어 요구사항. lib/ioat 가
 * spdk_zmalloc(align=8) 로 할당. */
#define SPDK_IOAT_CHANCMP_ALIGN			8	/* CHANCMP address must be 64-bit aligned */

/* [한국어] 일반(generic) 64B 디스크립터 — op 종류를 모를 때 공통 헤더만 보고 op 비트로 분기할 때
 * 사용. lib/ioat 가 ring 슬롯의 op 를 먼저 읽어 dma/fill/xor union 멤버를 선택.
 * 64바이트 고정 — 모든 IOAT descriptor 의 공통 사이즈. */
struct spdk_ioat_generic_hw_desc {
	uint32_t size;
	/* [한국어] 전송 바이트 수(가장 일반적), 또는 op 별 사이즈 의미. xfercap 한도 이내.
	 * 설정자: lib/ioat 빌더, 읽는 자: 하드웨어. */

	union {
		uint32_t control_raw;
		/* [한국어] control 비트필드 전체를 32비트로 한 번에 read/write 하는 별칭. lib/ioat 가
		 * memset/memcpy 로 빠르게 초기화할 때 사용. */

		struct {
			uint32_t int_enable: 1;
			/* [한국어] 이 디스크립터 완료 시 인터럽트 발생 여부. polled-mode 는 0. */

			uint32_t src_snoop_disable: 1;
			/* [한국어] 1=소스 PCIe snoop 비활성화(논코히런트 메모리 가정). 일반 hugepage 는 코히런트라 0. */

			uint32_t dest_snoop_disable: 1;
			/* [한국어] 1=목적지 PCIe snoop 비활성화. */

			uint32_t completion_update: 1;
			/* [한국어] 1=완료 시 chancmp 메모리에 이 디스크립터 IOVA 기록. chain 의 마지막에만 보통 1. */

			uint32_t fence: 1;
			/* [한국어] 1=이 디스크립터가 완료될 때까지 후속 디스크립터 시작 금지(직렬화 배리어). */

			uint32_t reserved2: 1;
			/* [한국어] reserved — 0 으로 둘 것. */

			uint32_t src_page_break: 1;
			/* [한국어] 소스 영역에 페이지 경계 break 가 있음을 알림(주소 정렬 처리). */

			uint32_t dest_page_break: 1;
			/* [한국어] 목적지 영역의 페이지 경계 break. */

			uint32_t bundle: 1;
			/* [한국어] bundle 비트 — 일부 op(XOR/PQ)에서 chain 묶음 표시. */

			uint32_t dest_dca: 1;
			/* [한국어] 1=DCA 로 목적지 데이터를 캐시에 직접 push(낮은 latency 후속 read 용). */

			uint32_t hint: 1;
			/* [한국어] hardware prefetch hint 비트. */

			uint32_t reserved: 13;
			/* [한국어] reserved 13비트. */

			uint32_t op: 8;
			/* [한국어] opcode — 0x00=COPY, 0x01=FILL, 0x87=XOR, 0x88=XOR_VAL, 0x89=PQ,
			 * 0x8a=PQ_VAL, 0x8b=PQ_UP. lib/ioat 가 op 를 먼저 보고 union 멤버를 선택. */
		} control;
	} u;

	uint64_t src_addr;
	/* [한국어] 소스 IOVA. op 가 FILL 이면 이 자리에 패턴 데이터(8바이트)가 들어가는 변종 구조체
	 * (spdk_ioat_fill_hw_desc) 가 사용된다. */

	uint64_t dest_addr;
	/* [한국어] 목적지 IOVA. */

	uint64_t next;
	/* [한국어] 다음 디스크립터의 IOVA(64B 정렬). chain 의 끝이면 0. 큰 transfer 분할 시 핵심. */

	uint64_t op_specific[4];
	/* [한국어] op 별 추가 페이로드 4 워드 — XOR 의 추가 src, PQ 의 P/Q 주소 등이 들어간다. */
};

/* [한국어] op=COPY(0x00) 전용 64B 디스크립터. spdk_ioat_dma_hw_desc 는 generic 과 동일
 * 64B 영역을 다른 비트필드 이름으로 해석한다. user1/user2 는 호스트 소프트웨어가 임의 활용
 * 가능한 메타데이터 슬롯(예: cb_arg/cb_fn 인덱스 보관). */
struct spdk_ioat_dma_hw_desc {
	uint32_t size;
	/* [한국어] 복사할 바이트 수. xfercap 의 단일 디스크립터 한도 초과 시 chain 분할 필요. */

	union {
		uint32_t control_raw;
		/* [한국어] control 비트필드 32비트 별칭. */

		struct {
			uint32_t int_enable: 1;
			/* [한국어] 인터럽트 발생 비트(polled 0). */

			uint32_t src_snoop_disable: 1;
			/* [한국어] 소스 snoop disable. */

			uint32_t dest_snoop_disable: 1;
			/* [한국어] 목적지 snoop disable. */

			uint32_t completion_update: 1;
			/* [한국어] 완료 기록 활성. chain 마지막에만 1. */

			uint32_t fence: 1;
			/* [한국어] 직렬화 배리어. */

			uint32_t null: 1;
			/* [한국어] 1=null 디스크립터 — 실제 데이터 이동 없이 chain 위치 유지/패딩 용. */

			uint32_t src_page_break: 1;
			/* [한국어] 소스 페이지 경계 표시. */

			uint32_t dest_page_break: 1;
			/* [한국어] 목적지 페이지 경계 표시. */

			uint32_t bundle: 1;
			/* [한국어] bundle 비트 — COPY 에서는 거의 사용 안 함. */

			uint32_t dest_dca: 1;
			/* [한국어] 목적지 DCA 활성. */

			uint32_t hint: 1;
			/* [한국어] 프리페치 힌트. */

			uint32_t reserved: 13;
			/* [한국어] reserved 13비트. */

#define SPDK_IOAT_OP_COPY 0x00
			/* [한국어] op=COPY 코드(0x00). build_copy 시 op 필드에 이 값 기록. */
			uint32_t op: 8;
			/* [한국어] opcode 필드 — COPY 는 항상 0x00. */
		} control;
	} u;

	uint64_t src_addr;
	/* [한국어] 소스 IOVA. */

	uint64_t dest_addr;
	/* [한국어] 목적지 IOVA. */

	uint64_t next;
	/* [한국어] 다음 chain 디스크립터 IOVA. */

	uint64_t reserved;
	/* [한국어] reserved 8B. */

	uint64_t reserved2;
	/* [한국어] reserved 8B. */

	uint64_t user1;
	/* [한국어] 호스트 임의 사용 메타데이터 1(예: cb_arg 인덱스 등). 하드웨어는 무시. */

	uint64_t user2;
	/* [한국어] 호스트 임의 사용 메타데이터 2. */
};

/* [한국어] op=FILL(0x01) 전용 디스크립터. src 자리에 fill_pattern 8바이트가 직접 들어가는 점이
 * COPY 와 다르다. */
struct spdk_ioat_fill_hw_desc {
	uint32_t size;
	/* [한국어] 채울 바이트 수(8B 정렬 권장). */

	union {
		uint32_t control_raw;
		/* [한국어] 32비트 별칭. */

		struct {
			uint32_t int_enable: 1;
			/* [한국어] 인터럽트 발생. polled 0. */

			uint32_t reserved: 1;
			/* [한국어] reserved 1비트. */

			uint32_t dest_snoop_disable: 1;
			/* [한국어] 목적지 snoop disable. FILL 은 소스가 패턴 데이터라 src snoop 무관. */

			uint32_t completion_update: 1;
			/* [한국어] 완료 기록 활성. */

			uint32_t fence: 1;
			/* [한국어] 직렬화 배리어. */

			uint32_t reserved2: 2;
			/* [한국어] reserved 2비트. */

			uint32_t dest_page_break: 1;
			/* [한국어] 목적지 페이지 경계 표시. */

			uint32_t bundle: 1;
			/* [한국어] bundle 비트. */

			uint32_t reserved3: 15;
			/* [한국어] reserved 15비트. */

#define SPDK_IOAT_OP_FILL 0x01
			/* [한국어] op=FILL 코드(0x01). build_fill 시 기록. */
			uint32_t op: 8;
			/* [한국어] opcode — FILL 은 항상 0x01. */
		} control;
	} u;

	uint64_t src_data;
	/* [한국어] 8바이트 fill 패턴(IOVA 가 아니라 raw 데이터!). 하드웨어가 이 8바이트를 size 만큼
	 * 반복 기록. */

	uint64_t dest_addr;
	/* [한국어] 목적지 IOVA. */

	uint64_t next;
	/* [한국어] chain next IOVA. */

	uint64_t reserved;
	/* [한국어] reserved 8B. */

	uint64_t next_dest_addr;
	/* [한국어] chain 의 다음 fill 목적지 — chain 으로 큰 영역을 fill 할 때 사용. */

	uint64_t user1;
	/* [한국어] 호스트 메타데이터 1. */

	uint64_t user2;
	/* [한국어] 호스트 메타데이터 2. */
};

/* [한국어] op=XOR(0x87)/XOR_VAL(0x88) 디스크립터 — RAID5 parity 연산.
 * src_addr ~ src_addr5 의 5개 소스를 XOR 해 dest_addr 에 기록(또는 검증). 6개 이상 소스는
 * spdk_ioat_xor_ext_hw_desc 로 확장. */
struct spdk_ioat_xor_hw_desc {
	uint32_t size;
	/* [한국어] XOR 단위 바이트 수. */

	union {
		uint32_t control_raw;
		/* [한국어] 32비트 별칭. */

		struct {
			uint32_t int_enable: 1;
			/* [한국어] 인터럽트. */
			uint32_t src_snoop_disable: 1;
			/* [한국어] 소스 snoop disable. */
			uint32_t dest_snoop_disable: 1;
			/* [한국어] 목적지 snoop disable. */
			uint32_t completion_update: 1;
			/* [한국어] 완료 기록 활성. */
			uint32_t fence: 1;
			/* [한국어] 배리어. */
			uint32_t src_count: 3;
			/* [한국어] 활성 소스 개수(2~5). 5 미만이면 나머지 src_addrN 무시. */
			uint32_t bundle: 1;
			/* [한국어] bundle 비트. */
			uint32_t dest_dca: 1;
			/* [한국어] 목적지 DCA. */
			uint32_t hint: 1;
			/* [한국어] 프리페치 힌트. */
			uint32_t reserved: 13;
			/* [한국어] reserved. */

#define SPDK_IOAT_OP_XOR 0x87
			/* [한국어] op=XOR(0x87) — 결과를 dest 에 기록(parity 생성). */
#define SPDK_IOAT_OP_XOR_VAL 0x88
			/* [한국어] op=XOR_VAL(0x88) — XOR 결과가 0 인지 검증(parity 검증). */
			uint32_t op: 8;
			/* [한국어] opcode. */
		} control;
	} u;

	uint64_t src_addr;
	/* [한국어] 소스 1 IOVA. */
	uint64_t dest_addr;
	/* [한국어] 목적지 IOVA. XOR_VAL 의 경우 검증 결과가 dest 자리에 기록될 수도 있음. */
	uint64_t next;
	/* [한국어] chain next. */
	uint64_t src_addr2;
	/* [한국어] 소스 2 IOVA. */
	uint64_t src_addr3;
	/* [한국어] 소스 3 IOVA. */
	uint64_t src_addr4;
	/* [한국어] 소스 4 IOVA. */
	uint64_t src_addr5;
	/* [한국어] 소스 5 IOVA. src_count 에 따라 활성/무시. */
};

/* [한국어] XOR 확장 디스크립터(64B chain 슬롯 1개 추가) — 6,7,8 번째 소스 주소 보유. */
struct spdk_ioat_xor_ext_hw_desc {
	uint64_t src_addr6;
	/* [한국어] 소스 6 IOVA. */
	uint64_t src_addr7;
	/* [한국어] 소스 7 IOVA. */
	uint64_t src_addr8;
	/* [한국어] 소스 8 IOVA. */
	uint64_t next;
	/* [한국어] chain next IOVA(이 ext 자체도 chain 의 한 슬롯). */
	uint64_t reserved[4];
	/* [한국어] reserved 32B 패딩(64B 정렬). */
};

/* [한국어] op=PQ(0x89)/PQ_VAL(0x8a) 디스크립터 — RAID6 P/Q parity 생성·검증.
 * 5개 소스 + Galois Field 계수(coef[8]) + p_addr/q_addr 두 출력. */
struct spdk_ioat_pq_hw_desc {
	uint32_t size;
	/* [한국어] 처리 바이트 수. */

	union {
		uint32_t control_raw;
		/* [한국어] 32비트 별칭. */

		struct {
			uint32_t int_enable: 1;
			/* [한국어] 인터럽트. */
			uint32_t src_snoop_disable: 1;
			/* [한국어] 소스 snoop disable. */
			uint32_t dest_snoop_disable: 1;
			/* [한국어] 목적지 snoop disable. */
			uint32_t completion_update: 1;
			/* [한국어] 완료 기록 활성. */
			uint32_t fence: 1;
			/* [한국어] 배리어. */
			uint32_t src_count: 3;
			/* [한국어] 활성 소스 개수. */
			uint32_t bundle: 1;
			/* [한국어] bundle 비트. */
			uint32_t dest_dca: 1;
			/* [한국어] DCA. */
			uint32_t hint: 1;
			/* [한국어] 프리페치 힌트. */
			uint32_t p_disable: 1;
			/* [한국어] 1=P parity 출력 비활성(Q 만 생성). */
			uint32_t q_disable: 1;
			/* [한국어] 1=Q parity 출력 비활성(P 만 생성). */
			uint32_t reserved: 11;
			/* [한국어] reserved. */

#define SPDK_IOAT_OP_PQ 0x89
			/* [한국어] op=PQ(0x89) — P,Q parity 생성. */
#define SPDK_IOAT_OP_PQ_VAL 0x8a
			/* [한국어] op=PQ_VAL(0x8a) — P,Q 검증. */
			uint32_t op: 8;
			/* [한국어] opcode. */
		} control;
	} u;

	uint64_t src_addr;
	/* [한국어] 소스 1 IOVA. */
	uint64_t p_addr;
	/* [한국어] P parity 출력 IOVA. */
	uint64_t next;
	/* [한국어] chain next. */
	uint64_t src_addr2;
	/* [한국어] 소스 2. */
	uint64_t src_addr3;
	/* [한국어] 소스 3. */
	uint8_t  coef[8];
	/* [한국어] Galois 계수 8개 — Q parity 생성에 사용되는 RS 코드 계수.
	 * 설정자: 호스트 소프트웨어. 읽는 자: 하드웨어 GF 곱셈기. */
	uint64_t q_addr;
	/* [한국어] Q parity 출력 IOVA. q_disable=1 이면 무시. */
};

/* [한국어] PQ 확장 디스크립터 — 4~8번째 소스 IOVA. */
struct spdk_ioat_pq_ext_hw_desc {
	uint64_t src_addr4;
	/* [한국어] 소스 4. */
	uint64_t src_addr5;
	/* [한국어] 소스 5. */
	uint64_t src_addr6;
	/* [한국어] 소스 6. */
	uint64_t next;
	/* [한국어] chain next. */
	uint64_t src_addr7;
	/* [한국어] 소스 7. */
	uint64_t src_addr8;
	/* [한국어] 소스 8. */
	uint64_t reserved[2];
	/* [한국어] reserved 16B 패딩. */
};

/* [한국어] op=PQ_UP(0x8b) 디스크립터 — 기존 P/Q 를 점진적 갱신(파티션 1개만 변경됐을 때
 * 전체 재계산 대신 차분 업데이트). */
struct spdk_ioat_pq_update_hw_desc {
	uint32_t size;
	/* [한국어] 갱신 바이트 수. */

	union {
		uint32_t control_raw;
		/* [한국어] 32비트 별칭. */

		struct {
			uint32_t int_enable: 1;
			/* [한국어] 인터럽트. */
			uint32_t src_snoop_disable: 1;
			/* [한국어] 소스 snoop disable. */
			uint32_t dest_snoop_disable: 1;
			/* [한국어] 목적지 snoop disable. */
			uint32_t completion_update: 1;
			/* [한국어] 완료 기록 활성. */
			uint32_t fence: 1;
			/* [한국어] 배리어. */
			uint32_t src_cnt: 3;
			/* [한국어] 소스 개수. */
			uint32_t bundle: 1;
			/* [한국어] bundle. */
			uint32_t dest_dca: 1;
			/* [한국어] DCA. */
			uint32_t hint: 1;
			/* [한국어] 프리페치. */
			uint32_t p_disable: 1;
			/* [한국어] P 비활성. */
			uint32_t q_disable: 1;
			/* [한국어] Q 비활성. */
			uint32_t reserved: 3;
			/* [한국어] reserved. */
			uint32_t coef: 8;
			/* [한국어] 단일 Galois 계수(차분 업데이트는 한 소스만 바뀌므로 1개로 충분). */

#define SPDK_IOAT_OP_PQ_UP 0x8b
			/* [한국어] op=PQ_UP(0x8b) — 점진적 P/Q 갱신. */
			uint32_t op: 8;
			/* [한국어] opcode. */
		} control;
	} u;

	uint64_t src_addr;
	/* [한국어] 새로운 소스 데이터 IOVA. */
	uint64_t p_addr;
	/* [한국어] P parity 출력 IOVA(in-place 갱신). */
	uint64_t next;
	/* [한국어] chain next. */
	uint64_t src_addr2;
	/* [한국어] 두 번째 소스(이전 데이터 등). */
	uint64_t p_src;
	/* [한국어] 기존 P parity 입력 IOVA(차분 계산용). */
	uint64_t q_src;
	/* [한국어] 기존 Q parity 입력 IOVA. */
	uint64_t q_addr;
	/* [한국어] Q parity 출력 IOVA. */
};

/* [한국어] raw view — 64B 디스크립터를 8개의 uint64_t 워드 배열로 보는 별칭. memcpy/배열
 * 인덱스로 빠르게 zero-clear 하거나 디버깅 시 hex dump 할 때 사용. */
struct spdk_ioat_raw_hw_desc {
	uint64_t field[8];
	/* [한국어] 8 워드 = 64 바이트.
	 * 설정자: lib/ioat 가 ring 슬롯을 0으로 클리어하는 fast-path 에서 사용.
	 * 읽는 자: 디버깅/덤프. */
};

/* [한국어] union spdk_ioat_hw_desc — 64B 슬롯 한 개를 모든 op 변종 중 하나로 해석할 수 있게
 * 하는 union. ring 의 한 슬롯은 항상 이 union 크기(64B). lib/ioat 는 op 코드를 보고 어느
 * 멤버를 읽을지 결정.
 * 설정자: lib/ioat 빌더가 op 별 멤버에 데이터 기록.
 * 읽는 자: 하드웨어가 raw 64B 메모리 영역으로 읽음.
 * 동기화: ring 슬롯 단위 — 빌더와 하드웨어가 서로 다른 슬롯에 접근하므로 슬롯 간 충돌은 없으나,
 * 빌더가 슬롯을 채운 뒤 dmacount 갱신 전에 메모리 배리어 필요. */
union spdk_ioat_hw_desc {
	struct spdk_ioat_raw_hw_desc raw;
	/* [한국어] raw 8 워드 뷰. */
	struct spdk_ioat_generic_hw_desc generic;
	/* [한국어] op-agnostic 일반 뷰. op 분기 전 공통 헤더 접근에 사용. */
	struct spdk_ioat_dma_hw_desc dma;
	/* [한국어] op=COPY 변종. */
	struct spdk_ioat_fill_hw_desc fill;
	/* [한국어] op=FILL 변종. */
	struct spdk_ioat_xor_hw_desc xor_desc;
	/* [한국어] op=XOR/XOR_VAL 변종. (xor 는 C++ 키워드라 멤버명 xor_desc 사용) */
	struct spdk_ioat_xor_ext_hw_desc xor_ext;
	/* [한국어] XOR 확장(소스 6~8) 변종. */
	struct spdk_ioat_pq_hw_desc pq;
	/* [한국어] op=PQ/PQ_VAL 변종. */
	struct spdk_ioat_pq_ext_hw_desc pq_ext;
	/* [한국어] PQ 확장(소스 4~8) 변종. */
	struct spdk_ioat_pq_update_hw_desc pq_update;
	/* [한국어] op=PQ_UP 변종. */
};
/* [한국어] 컴파일 타임 어서션 — union 의 sizeof 가 정확히 64 인지 검증.
 * 만약 누군가 변종 구조체에 필드를 추가하다 64B 를 초과하면 build 실패하여 silent ABI 깨짐 방지.
 * 64B 는 캐시라인 1줄 + IOAT 하드웨어가 가정한 디스크립터 크기. */
SPDK_STATIC_ASSERT(sizeof(union spdk_ioat_hw_desc) == 64, "incorrect spdk_ioat_hw_desc layout");

/* [한국어] extern "C" 종결. */
#ifdef __cplusplus
}
#endif

/* [한국어] include guard 종결. */
#endif /* SPDK_IOAT_SPEC_H */
