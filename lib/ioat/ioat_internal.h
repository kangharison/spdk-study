/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] IOAT(Intel I/O Acceleration Technology) DMA 엔진 내부 헤더 (ioat_internal.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK가 Intel IOAT(Crystal Beach) DMA 가속 엔진을 유저스페이스에서 polled-mode로
 * 구동하기 위한 내부 자료구조를 정의한다. IOAT은 Xeon CPU에 내장된 구세대 DMA copy/fill
 * 엔진(I/OAT)으로 메모리-메모리 복사·메모리 채우기를 호스트 CPU 부담 없이 비동기로 수행한다.
 * 이 파일에서는 채널 객체(spdk_ioat_chan)와 디스크립터 슬롯(ioat_descriptor),
 * 그리고 채널 상태 레지스터를 해석하는 헬퍼들을 외부에 노출하지 않는 구현 측면에서 선언한다.
 * spdk/ioat_spec.h의 하드웨어 정의와 spdk/ioat.h의 공개 API를 잇는 다리 역할이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [SPDK accel module / 사용자 앱]
 *      → spdk_ioat_probe()/spdk_ioat_submit_copy()/spdk_ioat_process_events() (공개 API in spdk/ioat.h)
 *      → ioat.c 내부 구현 (이 헤더의 자료구조 사용)
 *      → spdk_ioat_registers (MMIO BAR0 매핑)
 *      → IOAT 하드웨어 채널
 * 실행 컨텍스트: 호스트 유저스페이스. SPDK 환경 초기화 후 PCI BAR을 매핑한 메모리 공간을
 * 통해 디바이스와 통신한다. 채널은 일반적으로 단일 SPDK thread/reactor에 고정되어 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/ioat.h: 공개 API (probe/attach/submit/process_events 콜백 시그니처).
 * - spdk/ioat_spec.h: 하드웨어 레지스터 비트 필드 및 디스크립터 포맷 정의(union spdk_ioat_hw_desc 등).
 * - spdk/queue.h: TAILQ 매크로 (전체 attached 채널 리스트 관리).
 * - spdk/mmio.h: spdk_mmio_read_8/spdk_mmio_write_8 등 MMIO 접근 헬퍼.
 * 데이터 흐름: 사용자가 spdk_ioat_submit_copy()를 부르면 ring(=hw_ring)의 head 위치에
 * 디스크립터를 채우고 doorbell(dmacount) 레지스터를 갱신한다. 하드웨어가 작업을 끝내면
 * comp_update 메모리에 마지막 완료 디스크립터의 물리 주소를 기록하고, polling 함수는
 * 이를 비교하여 ring->callback_fn을 호출한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_ioat_chan: PCI 디바이스 1개 = 채널 1개에 대응되는 런타임 컨텍스트.
 * - struct ioat_descriptor: 소프트웨어 측 디스크립터 슬롯(물리 주소 + 콜백 정보).
 * - is_ioat_active/idle/halted/suspended: CHANSTS 레지스터의 상태 비트 해석 헬퍼.
 *   (실제 함수 구현 및 ring 회전, doorbell 동작은 ioat.c 참조)
 */

#ifndef __IOAT_INTERNAL_H__
#define __IOAT_INTERNAL_H__

/* [한국어] SPDK 표준 C 라이브러리 래퍼 - 플랫폼 차이를 흡수한 stdint/stdbool/string 등을 묶음. */
#include "spdk/stdinc.h"

/* [한국어] IOAT 공개 API (spdk_ioat_probe_cb, spdk_ioat_req_cb 등 콜백 시그니처). */
#include "spdk/ioat.h"
/* [한국어] IOAT 하드웨어 스펙: 레지스터 레이아웃, 디스크립터 union(spdk_ioat_hw_desc), opcode 상수. */
#include "spdk/ioat_spec.h"
/* [한국어] BSD-스타일 TAILQ/STAILQ 매크로 - attached_chans 리스트 등 lockless 자료구조에 사용. */
#include "spdk/queue.h"
/* [한국어] MMIO 메모리 매핑 영역에 대한 read/write 래퍼 - 컴파일러 재배치/최적화 방지. */
#include "spdk/mmio.h"

/* [한국어] 기본 ring 크기를 2^15(=32768)개의 디스크립터로 설정.
 * IOAT은 ring 크기가 2^N여야 하며, 이 값이 클수록 in-flight 작업을 더 많이 큐잉할 수 있다.
 * 32K는 일반 NVMe queue depth(보통 1K) 대비 매우 넉넉한 값으로, IOAT의 single-ring 설계를 보완한다. */
#define IOAT_DEFAULT_ORDER			15

/* [한국어] 소프트웨어 측 ring 슬롯 - 하드웨어 디스크립터(spdk_ioat_hw_desc)와 1:1 짝을 이룸.
 * 하드웨어 디스크립터는 hw_ring[i]에, 소프트웨어 메타데이터(콜백, 물리 주소)는 ring[i]에 저장. */
struct ioat_descriptor {
	uint64_t		phys_addr;
	/* [한국어] 이 슬롯이 가리키는 하드웨어 디스크립터(hw_ring[i])의 물리 주소(IOVA).
	 * 설정자: ioat_channel_start()가 spdk_vtophys()로 한 번 계산해 저장 (slot 별 고정).
	 * 읽는 자: ioat_process_channel_events()가 comp_update 값과 비교하여 완료 위치 식별.
	 * 값 범위: hugepage 영역의 물리 주소 (DMA 가능 영역). 0이면 미초기화 상태.
	 * 동기화: 채널은 단일 SPDK 스레드 사용이 권장되므로 별도 락 없음. */

	spdk_ioat_req_cb	callback_fn;
	/* [한국어] 이 디스크립터의 작업이 끝났을 때 호출할 사용자 콜백.
	 * 설정자: spdk_ioat_build_copy()/build_fill() 마지막 단계에서 last_desc에만 설정 (한 작업 그룹의 끝).
	 * 읽는 자: ioat_process_channel_events()가 완료 시 callback_fn(callback_arg) 호출.
	 * 값 범위: NULL(중간 디스크립터) 또는 사용자 함수 포인터. NULL이면 콜백 없이 자원만 회수.
	 * 동기화: poll 호출 스레드(채널 소유 SPDK 스레드)에서 직렬 호출. */

	void			*callback_arg;
	/* [한국어] callback_fn 호출 시 전달되는 사용자 컨텍스트 포인터.
	 * 설정자: spdk_ioat_build_copy()/submit_*() 호출 시 사용자가 지정.
	 * 읽는 자: ioat_process_channel_events()가 콜백 호출 시 사용.
	 * 값 범위: 임의의 사용자 포인터(NULL 허용).
	 * 동기화: 콜백과 동일 (poll 스레드 단일 접근). */
};

/* [한국어] IOAT 채널: PCI 디바이스(=DMA 채널) 1개에 대응되는 런타임 핸들.
 * 한 채널은 자체 ring과 doorbell을 가지며, 동시에 단일 SPDK 스레드/reactor 소유로 사용된다. */
/* One of these per allocated PCI device. */
struct spdk_ioat_chan {
	/* Opaque handle to upper layer */
	struct spdk_pci_device		*device;
	/* [한국어] DPDK PCI 추상화로 얻은 디바이스 핸들 - BAR 매핑/언매핑/config 접근에 사용.
	 * 설정자: ioat_attach()가 spdk_pci_enumerate() 콜백에서 받은 포인터를 저장.
	 * 읽는 자: ioat_map_pci_bar()/spdk_pci_device_cfg_read32() 등 PCI 작업.
	 * 값 범위: 유효한 spdk_pci_device 포인터.
	 * 동기화: 디바이스 등록 시 g_ioat_driver.lock으로 보호, 이후 채널 단위로만 접근. */

	uint64_t            max_xfer_size;
	/* [한국어] 디스크립터 1개로 전송 가능한 최대 바이트 수 (XFERCAP 레지스터 기반).
	 * 설정자: ioat_channel_start()가 regs->xfercap을 읽어 1 << xfercap으로 계산
	 *         (xfercap=0이면 4 GB로 해석).
	 * 읽는 자: spdk_ioat_build_copy()/build_fill()이 큰 전송을 max_xfer_size 단위로 분할할 때 사용.
	 * 값 범위: 4 KB 이상 ~ 4 GB. IOAT 스펙상 xfercap의 최소값은 12.
	 * 동기화: 초기화 후 read-only. */

	volatile struct spdk_ioat_registers *regs;
	/* [한국어] PCI BAR0를 mmap한 MMIO 레지스터 영역 포인터.
	 * 설정자: ioat_map_pci_bar()가 spdk_pci_device_map_bar(BAR0)으로 매핑.
	 * 읽는 자: 거의 모든 함수가 spdk_mmio_read_*/write_*로 channel 제어/상태 조회 시 사용.
	 * 값 범위: 유효한 mmap 주소. volatile은 컴파일러 캐시 회피용.
	 * 동기화: MMIO는 단일 채널 스레드에서만 접근 - 락 불필요. */

	volatile uint64_t   *comp_update;
	/* [한국어] DMA 완료 통보 메모리 - HW가 마지막으로 완료한 디스크립터의 물리 주소를 여기에 기록.
	 * 설정자: ioat_channel_start()가 spdk_zmalloc(DMA)로 hugepage 메모리 할당,
	 *         CHANCMP 레지스터에 그 물리 주소를 등록(ioat_write_chancmp).
	 * 읽는 자: ioat_process_channel_events()가 매 polling마다 *comp_update를 읽어 완료 추적.
	 * 값 범위: SPDK_IOAT_CHANCMP_ALIGN(8 byte) 정렬된 hugepage 주소. 하위 비트는 상태 플래그.
	 * 동기화: HW가 비동기 쓰기, SW가 polling 읽기 - volatile + DMA-coherent로 OK. */

	uint32_t            head;
	/* [한국어] 다음 디스크립터를 채울 ring 인덱스 (소프트웨어 producer).
	 * 설정자: ioat_submit_single()가 매 prep_copy/prep_fill 호출 후 1 증가.
	 * 읽는 자: ioat_get_active()/spdk_ioat_flush()가 doorbell 갱신용으로 사용.
	 * 값 범위: 무한 증가 (랩어라운드는 ring_size_order 마스크로 처리). 부호 없는 32비트 차이 사용.
	 * 동기화: 채널 소유 스레드 단독 갱신. */

	uint32_t            tail;
	/* [한국어] 다음에 회수할 디스크립터의 ring 인덱스 (소프트웨어 consumer).
	 * 설정자: ioat_process_channel_events()가 완료된 디스크립터 1개당 1 증가.
	 * 읽는 자: ioat_get_active()/get_ring_space()가 ring 잔여 공간 계산 시 사용.
	 * 값 범위: head를 절대 추월 못하며, head - tail이 in-flight 개수.
	 * 동기화: 채널 소유 스레드 단독 갱신. */

	uint32_t            ring_size_order;
	/* [한국어] ring 크기의 2의 지수 - 실제 슬롯 수는 (1 << ring_size_order).
	 * 설정자: ioat_channel_start()가 IOAT_DEFAULT_ORDER(15) 그대로 설정 (현재 동적 변경 없음).
	 * 읽는 자: ioat_get_ring_index()/get_active()/get_ring_space() 등이 마스크 계산에 사용.
	 * 값 범위: 일반적으로 15 (32K 슬롯). 하드웨어 한계 내에서만 변경 가능.
	 * 동기화: 초기화 후 read-only. */

	uint64_t            last_seen;
	/* [한국어] 마지막 polling에서 보았던 완료 디스크립터의 물리 주소.
	 * 설정자: ioat_process_channel_events()가 한 라운드를 마치고 갱신.
	 * 읽는 자: 같은 함수가 새 *comp_update와 비교해 "정말 새 완료가 있는지" 빠르게 판정.
	 * 값 범위: ring 슬롯 중 하나의 phys_addr 또는 0 (초기 상태).
	 * 동기화: poll 스레드 단독 접근. */

	struct ioat_descriptor		*ring;
	/* [한국어] 소프트웨어 측 디스크립터 메타데이터 배열 (calloc 할당, 호스트 메모리).
	 * 설정자: ioat_channel_start()가 num_descriptors 크기로 calloc.
	 * 읽는 자: ioat_get_ring_entry()/process_channel_events() 등이 인덱스로 접근.
	 * 값 범위: ring[0..(1<<order)-1]. 각 엔트리에 phys_addr/cb 등 저장.
	 * 동기화: 단일 채널 스레드 접근. */

	union spdk_ioat_hw_desc		*hw_ring;
	/* [한국어] 하드웨어 디스크립터 ring (hugepage DMA 메모리, 64-byte 정렬).
	 * 설정자: ioat_channel_start()가 spdk_zmalloc(SPDK_MALLOC_DMA)로 할당, 각 슬롯의 next를 다음 슬롯의
	 *         물리 주소로 연결하여 단방향 링크드 링 구성.
	 * 읽는 자: prep_copy/prep_fill/prep_null이 실제 명령어 필드를 채움. HW가 DMA로 읽음.
	 * 값 범위: hw_ring[0..(1<<order)-1]. 각 엔트리는 64 byte.
	 * 동기화: SW write/HW read - SW가 doorbell 갱신 전에 _spdk_wmb 등 메모리 배리어가 필요할 수 있음. */

	uint32_t			dma_capabilities;
	/* [한국어] 이 채널이 지원하는 DMA 연산 종류 비트마스크.
	 * 설정자: ioat_channel_start()가 항상 SPDK_IOAT_ENGINE_COPY_SUPPORTED를 켜고,
	 *         DMACAP에 BFILL이 있으면 SPDK_IOAT_ENGINE_FILL_SUPPORTED 추가.
	 * 읽는 자: spdk_ioat_get_dma_capabilities() 및 spdk_ioat_build_fill()이 fill 가능 여부 확인.
	 * 값 범위: 비트 OR (COPY, FILL 등).
	 * 동기화: 초기화 후 read-only. */

	/* tailq entry for attached_chans */
	TAILQ_ENTRY(spdk_ioat_chan)	tailq;
	/* [한국어] g_ioat_driver.attached_chans 리스트 연결 노드.
	 * 설정자: ioat_enum_cb()가 INSERT_TAIL로 등록, spdk_ioat_detach()가 REMOVE.
	 * 읽는 자: ioat_enum_cb()가 중복 attach 방지 목적으로 순회.
	 * 값 범위: 리스트의 prev/next 포인터.
	 * 동기화: g_ioat_driver.lock으로 보호 (전역 리스트). */
};

/*
 * [한국어]
 * is_ioat_active - CHANSTS 레지스터 값이 ACTIVE 상태인지 검사
 *
 * @status: ioat_get_chansts()가 spdk_mmio_read_8로 읽은 64-bit CHANSTS 값 (하위 3비트가 status).
 * @return: ACTIVE면 1(=true), 아니면 0.
 *
 * 채널 상태는 IDLE/PREFETCH/SUSPENDED/HALTED/ARMED 등 여러 단계가 있으며,
 * ACTIVE는 HW가 디스크립터를 처리 중인 상태이다. SPDK_IOAT_CHANSTS_STATUS는 status 비트필드 마스크.
 * 호출 컨텍스트: ioat_reset_hw() 등 채널 정지/재시작 절차에서 polling 시 사용.
 *
 * 호출 체인:
 *   ioat_reset_hw → [is_ioat_active] (status 폴링)
 */
static inline uint32_t
is_ioat_active(uint64_t status)
{
	/* [한국어] 하위 status 비트만 추출(SPDK_IOAT_CHANSTS_STATUS 마스크)하여 ACTIVE 상수와 동등 비교. */
	return (status & SPDK_IOAT_CHANSTS_STATUS) == SPDK_IOAT_CHANSTS_ACTIVE;
}

/*
 * [한국어]
 * is_ioat_idle - CHANSTS가 IDLE(아무 작업 없음) 상태인지 검사
 *
 * @status: CHANSTS 레지스터의 64-bit raw 값.
 * @return: IDLE이면 1, 아니면 0.
 *
 * 채널이 IDLE 상태여야 새로운 디스크립터 체인의 처음을 안전하게 셋업할 수 있다.
 * ioat_channel_start()의 마지막 단계에서 NULL 디스크립터 처리 후 IDLE 상태로 진입했는지 확인하는 데 사용.
 *
 * 호출 체인:
 *   ioat_channel_start/ioat_reset_hw → [is_ioat_idle]
 */
static inline uint32_t
is_ioat_idle(uint64_t status)
{
	/* [한국어] status 비트가 IDLE 상수와 일치하는지 검사. */
	return (status & SPDK_IOAT_CHANSTS_STATUS) == SPDK_IOAT_CHANSTS_IDLE;
}

/*
 * [한국어]
 * is_ioat_halted - CHANSTS가 HALTED(에러로 멈춤) 상태인지 검사
 *
 * @status: CHANSTS 64-bit 값.
 * @return: HALTED면 1.
 *
 * HALTED는 DMA 에러로 채널이 자동 정지된 상태이며, 복구를 위해 RESET이 필요하다.
 * ioat_process_channel_events()가 매 polling 라운드 시작 시 점검한다.
 *
 * 호출 체인:
 *   ioat_process_channel_events → [is_ioat_halted] → 에러 로깅 후 -1 반환
 */
static inline uint32_t
is_ioat_halted(uint64_t status)
{
	/* [한국어] HALTED 비트 패턴과 일치 여부 검사 - 일치하면 채널 에러 상태로 판단. */
	return (status & SPDK_IOAT_CHANSTS_STATUS) == SPDK_IOAT_CHANSTS_HALTED;
}

/*
 * [한국어]
 * is_ioat_suspended - CHANSTS가 SUSPENDED(SUSPEND 명령으로 일시 중단) 상태인지 검사
 *
 * @status: CHANSTS 64-bit 값.
 * @return: SUSPENDED면 1.
 *
 * SUSPEND 명령(ioat_suspend) 후 RESET 전에 SUSPENDED로 진입했는지 확인하는 데 사용.
 * 현재 ioat.c에서는 직접 호출 위치는 없지만 디버깅/확장용으로 헤더에 노출되어 있다.
 *
 * 호출 체인:
 *   (확장 코드/디버깅) → [is_ioat_suspended]
 */
static inline uint32_t
is_ioat_suspended(uint64_t status)
{
	/* [한국어] SUSPENDED 상태 패턴과 일치 여부 - SUSPEND 명령 후 안정화 확인용. */
	return (status & SPDK_IOAT_CHANSTS_STATUS) == SPDK_IOAT_CHANSTS_SUSPENDED;
}

#endif /* __IOAT_INTERNAL_H__ */
