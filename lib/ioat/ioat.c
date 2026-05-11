/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Intel I/OAT(I/O Acceleration Technology, "Crystal Beach") 유저스페이스 드라이버 (ioat.c)
 *
 * === 파일의 역할 ===
 * Intel IOAT는 IDXD/DSA의 전 세대(2010년대 초 Xeon에 처음 탑재)에 해당하는 비동기 DMA 엔진으로,
 * 호스트 메모리 간 copy / fill 연산을 CPU 대신 PCIe DMA 컨트롤러에 오프로드한다. 본 파일은
 * SPDK 환경에서 IOAT 채널을 mmap한 BAR0(MMIO) + DMA 버퍼 기반 환형 디스크립터 링으로 구동하는
 * 유저스페이스 드라이버 코어다. 디스크립터 prep(copy/fill/null), DMACOUNT 도어벨 쓰기로 제출,
 * comp_update DMA 영역 폴링으로 완료 처리, 그리고 채널 reset/start/destruct까지를 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * IOAT는 SPDK의 모듈 lib/copy_engine_ioat 등 상위 카피 엔진 모듈이 사용하며, 더 일반적으로는
 * "lib/ioat" 공개 API(spdk_ioat_*)를 직접 호출하는 사용자가 접근한다. IDXD가 등장한 이후로는
 * 사실상 legacy로 분류되지만, 구형 Xeon 시스템과의 호환을 위해 유지된다.
 *
 * 호출 체인 (제출 경로):
 *   사용자 → spdk_ioat_submit_copy(또는 fill)
 *     → spdk_ioat_build_copy/_fill (디스크립터 ring에 prep_copy/prep_fill 호출 반복)
 *     → ioat_prep_copy/fill (1개 슬롯 채우고 head++)
 *     → spdk_ioat_flush (마지막 디스크립터의 completion_update=1, dmacount 도어벨 쓰기)
 *     → 디바이스 DMA 진행, 완료 시 comp_update에 last completed descriptor의 phys_addr 기록
 *     → 사용자 → spdk_ioat_process_events
 *       → ioat_process_channel_events (comp_update 폴링, tail 이동, callback 호출)
 *
 * 호출 체인 (probe 경로):
 *   사용자 → spdk_ioat_probe
 *     → spdk_pci_enumerate(spdk_pci_ioat_get_driver(), ioat_enum_cb)
 *     → ioat_enum_cb (이미 attach된 디바이스 dedupe + 사용자 probe_cb 확인)
 *     → ioat_attach (PCI busmaster + ioat_channel_start)
 *     → ioat_channel_start (BAR map → version 검증 → max_xfer 계산 → comp_update 할당
 *                            → ring/hw_ring 할당 + 물리주소 체이닝 → reset_hw → CHANCMP/CHAINADDR 쓰기
 *                            → null 디스크립터로 채널 wakeup)
 *
 * 실행 컨텍스트: 호스트 유저스페이스. 한 채널은 한 스레드(보통 SPDK reactor)가 단독으로 사용 —
 *   ring head/tail이 lockless로 안전하기 위해 thread-affinity가 필수.
 *
 * === 타 모듈과의 연결 ===
 * 의존: lib/env_dpdk(spdk_pci_*, spdk_zmalloc DMA 메모리, spdk_vtophys), include/spdk/memory.h(spdk_mmio_*),
 *   ioat_internal.h(레지스터 layout, hw_desc union, spdk_ioat_chan 정의), POSIX pthread.
 * 데이터 흐름: 사용자 src/dst 가상주소 → spdk_vtophys로 IOVA(또는 PA)로 변환 → hw_desc.dma.{src,dest}_addr →
 *   디바이스 DMA가 직접 fetch & store. 완료는 comp_update DMA 영역에 last_completed의 phys_addr 기록.
 * 공유 자료구조: g_ioat_driver.attached_chans는 g_ioat_driver.lock으로 직렬 보호, ring/comp_update는 채널 소유 스레드 단독 접근.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ioat_driver: 전역 attached 채널 리스트 + 직렬화 mutex. probe/detach 직렬화에 사용.
 * - spdk_ioat_chan: 채널 핸들. ring(SW), hw_ring(DMA), head/tail/last_seen, regs(MMIO), comp_update(DMA) 등.
 * - ioat_channel_start: 채널 부팅 마스터 — BAR map, max_xfer 결정, ring 할당/체이닝, reset, null desc로 첫 wakeup.
 * - ioat_reset_hw: suspend → CHANERR clear → reset → 폴링 대기.
 * - ioat_process_channel_events: comp_update 폴링하며 tail 이동, 사용자 callback 호출. 핵심 완료 폴링 루프.
 * - ioat_prep_copy/fill/null: hw_ring[head]에 SPDK_IOAT_OP_COPY/FILL/COPY|null 디스크립터 작성.
 * - spdk_ioat_flush: 마지막 디스크립터에 completion_update=1을 set하고 DMACOUNT MMIO write —
 *   디바이스 도어벨 (한 번에 N개 디스크립터 처리 트리거).
 * - spdk_ioat_build/submit_copy/fill: 사용자 가상주소 범위를 vtophys로 한 청크씩 잘라 prep_copy를 반복.
 * - spdk_ioat_probe/detach: PCI 열거와 detach.
 * - is_ioat_active/idle/halted: chansts 비트 인터프리터 (header에 정의).
 */

#include "spdk/stdinc.h"     /* [한국어] POSIX 표준 헤더 일괄 포함 */

#include "ioat_internal.h"   /* [한국어] 레지스터 매크로, spdk_ioat_chan 정의, hw_desc union, is_ioat_*(상태 비트) */

#include "spdk/env.h"        /* [한국어] spdk_pci_*, spdk_zmalloc(DMA), spdk_vtophys, spdk_pci_enumerate */
#include "spdk/util.h"       /* [한국어] spdk_min 등 */
#include "spdk/memory.h"     /* [한국어] spdk_mmio_read_8/write_8 — MMIO 액세스 wrapper */

#include "spdk/log.h"        /* [한국어] SPDK_ERRLOG/SPDK_LOG_REGISTER_COMPONENT */

/*
 * [한국어]
 * struct ioat_driver — IOAT 드라이버 전역 상태.
 *
 * spdk_ioat_probe가 발견한 모든 채널을 attached_chans 리스트에 등록한다.
 * lock은 probe/detach 동안 리스트 변경을 직렬화 (probe가 보통 init 단계 한 번만 실행되므로
 * 핫패스 영향은 없음).
 */
struct ioat_driver {
	pthread_mutex_t			lock;
	/* [한국어] attached_chans 리스트 변경 보호.
	 * 설정자: spdk_ioat_probe(insert), spdk_ioat_detach(remove).
	 * I/O 핫패스에서는 잡지 않음. */

	TAILQ_HEAD(, spdk_ioat_chan)	attached_chans;
	/* [한국어] 현재 attach 상태인 모든 IOAT 채널의 글로벌 리스트.
	 * 동일 디바이스를 두 번 attach하지 않도록 ioat_enum_cb에서 dedupe 검사에 사용. */
};

/* [한국어] 전역 드라이버 상태 — static initializer로 lock=PTHREAD_MUTEX_INITIALIZER, list=비어있음 */
static struct ioat_driver g_ioat_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.attached_chans = TAILQ_HEAD_INITIALIZER(g_ioat_driver.attached_chans),
};

/*
 * [한국어]
 * ioat_get_chansts — CHANSTS 레지스터 8B 읽기 (channel status).
 * 디스크립터 완료 정보 + 상태(active/idle/halted) 비트를 인코딩한 64-bit 값 반환.
 * 마스크 SPDK_IOAT_CHANSTS_COMPLETED_DESCRIPTOR_MASK로 last completed 디스크립터의 phys_addr 추출 가능.
 */
static uint64_t
ioat_get_chansts(struct spdk_ioat_chan *ioat)
{
	return spdk_mmio_read_8(&ioat->regs->chansts);
}

/*
 * [한국어]
 * ioat_write_chancmp — CHANCMP 레지스터에 comp_update DMA 버퍼의 물리주소 8B 기록.
 *
 * 디바이스는 디스크립터 완료 시 이 주소로 last completed의 phys_addr을 DMA write한다.
 * 채널 시작 시 한 번만 호출.
 */
static void
ioat_write_chancmp(struct spdk_ioat_chan *ioat, uint64_t addr)
{
	spdk_mmio_write_8(&ioat->regs->chancmp, addr);
}

/*
 * [한국어]
 * ioat_write_chainaddr — CHAINADDR 레지스터에 ring의 첫 디스크립터 phys_addr 기록.
 * 디바이스는 이 주소부터 hw_desc.next 체인을 따라가며 디스크립터를 fetch한다.
 * 채널 시작 시 한 번만 호출.
 */
static void
ioat_write_chainaddr(struct spdk_ioat_chan *ioat, uint64_t addr)
{
	spdk_mmio_write_8(&ioat->regs->chainaddr, addr);
}

/*
 * [한국어]
 * ioat_suspend — CHANCMD에 SUSPEND 비트 쓰기. 디바이스가 진행 중인 디스크립터 처리를 멈춘다.
 * reset 전에 항상 suspend 후 idle 상태로 전이시킨다 (IOAT spec 권장 시퀀스).
 */
static inline void
ioat_suspend(struct spdk_ioat_chan *ioat)
{
	ioat->regs->chancmd = SPDK_IOAT_CHANCMD_SUSPEND;  /* [한국어] 1B write — IOAT의 chancmd는 8-bit 레지스터 */
}

/*
 * [한국어]
 * ioat_reset — CHANCMD에 RESET 비트 쓰기. 채널을 초기 상태로 되돌린다.
 */
static inline void
ioat_reset(struct spdk_ioat_chan *ioat)
{
	ioat->regs->chancmd = SPDK_IOAT_CHANCMD_RESET;
}

/*
 * [한국어]
 * ioat_reset_pending — RESET 비트가 아직 set되어 있는지 확인.
 * 디바이스가 reset을 처리하면 비트가 자동으로 클리어된다. 폴링 루프에서 사용.
 */
static inline uint32_t
ioat_reset_pending(struct spdk_ioat_chan *ioat)
{
	uint8_t cmd;

	cmd = ioat->regs->chancmd;                                 /* [한국어] 현재 chancmd 비트맵 read */
	return (cmd & SPDK_IOAT_CHANCMD_RESET) == SPDK_IOAT_CHANCMD_RESET;  /* [한국어] RESET 비트가 켜져 있으면 아직 진행 중 */
}

/*
 * [한국어]
 * ioat_map_pci_bar — IOAT BAR0(MMIO 레지스터) 매핑.
 *
 * IOAT는 BAR0 하나만 사용 (IDXD와 달리 portal BAR이 별도로 없음 — 디스크립터는 호스트 메모리 ring에 두고
 * 도어벨만 MMIO로 트리거).
 */
static int
ioat_map_pci_bar(struct spdk_ioat_chan *ioat)
{
	int regs_bar, rc;
	void *addr;
	uint64_t phys_addr, size;     /* [한국어] DPDK가 채워주지만 본 함수에서 사용 안 함 */

	regs_bar = 0;                  /* [한국어] BAR 0 = IOAT 레지스터 */
	rc = spdk_pci_device_map_bar(ioat->device, regs_bar, &addr, &phys_addr, &size);
	if (rc != 0 || addr == NULL) {
		SPDK_ERRLOG("pci_device_map_range failed with error code %d\n",
			    rc);
		return -1;
	}

	ioat->regs = (volatile struct spdk_ioat_registers *)addr;
	/* [한국어] volatile — 컴파일러가 레지스터 read/write를 캐시/리오더링하지 않게.
	 * spdk_mmio_* 래퍼와 함께 사용하면 더 안전하지만, 이 코드는 직접 ioat->regs->chancmd 등 접근도 함. */

	return 0;
}

/*
 * [한국어]
 * ioat_unmap_pci_bar — BAR0 매핑 해제. destruct 경로에서 호출.
 */
static int
ioat_unmap_pci_bar(struct spdk_ioat_chan *ioat)
{
	int rc = 0;
	void *addr = (void *)ioat->regs;  /* [한국어] volatile 제거하여 비-볼라타일 인자에 전달 */

	if (addr) {
		rc = spdk_pci_device_unmap_bar(ioat->device, 0, addr);
	}
	return rc;
}


/*
 * [한국어]
 * ioat_get_active — 현재 ring에 미완료 디스크립터 수.
 * head는 다음에 작성할 슬롯 인덱스, tail은 다음에 회수할 슬롯 인덱스.
 * (head - tail) & mask = 활성 디스크립터 수 (mod 2^ring_size_order 산술).
 */
static inline uint32_t
ioat_get_active(struct spdk_ioat_chan *ioat)
{
	return (ioat->head - ioat->tail) & ((1 << ioat->ring_size_order) - 1);
}

/*
 * [한국어]
 * ioat_get_ring_space — ring에서 새 디스크립터를 prep할 수 있는 빈 슬롯 수.
 * "-1"은 head==tail로 비어있음/꽉참 모호성을 회피하는 sentinel slot 1개를 항상 비워두기 위함.
 */
static inline uint32_t
ioat_get_ring_space(struct spdk_ioat_chan *ioat)
{
	return (1 << ioat->ring_size_order) - ioat_get_active(ioat) - 1;
}

/*
 * [한국어]
 * ioat_get_ring_index — index를 ring 크기에 맞춰 wrap (modulo).
 * ring_size_order가 power-of-two이므로 mask AND 한 번으로 끝남.
 */
static uint32_t
ioat_get_ring_index(struct spdk_ioat_chan *ioat, uint32_t index)
{
	return index & ((1 << ioat->ring_size_order) - 1);
}

/*
 * [한국어]
 * ioat_get_ring_entry — index에 해당하는 SW(ioat_descriptor)와 HW(spdk_ioat_hw_desc) 슬롯 포인터를 반환.
 *
 * SW ring(ioat->ring): callback_fn/arg, phys_addr 등 메타데이터 (호스트 메모리, 디바이스가 보지 않음).
 * HW ring(ioat->hw_ring): src/dst/size/control_raw/next 등 디바이스가 직접 fetch하는 64B DMA 영역.
 * 두 ring은 같은 인덱스를 공유.
 */
static void
ioat_get_ring_entry(struct spdk_ioat_chan *ioat, uint32_t index,
		    struct ioat_descriptor **desc,
		    union spdk_ioat_hw_desc **hw_desc)
{
	uint32_t i = ioat_get_ring_index(ioat, index);

	*desc = &ioat->ring[i];
	*hw_desc = &ioat->hw_ring[i];
}

/*
 * [한국어]
 * ioat_submit_single — head 인덱스 1 증가. 실제 도어벨은 spdk_ioat_flush가 별도로 한 번에.
 * prep_copy/fill/null이 디스크립터를 채운 직후 호출.
 */
static void
ioat_submit_single(struct spdk_ioat_chan *ioat)
{
	ioat->head++;
}

/*
 * [한국어]
 * spdk_ioat_flush — 누적된 prep된 디스크립터들을 디바이스에 한 번에 알린다.
 *
 * 1) 마지막 디스크립터(=head-1)의 control.completion_update=1로 set —
 *    디바이스가 이 디스크립터까지 완료한 시점에 comp_update DMA 영역을 갱신하도록 지시.
 * 2) regs->dmacount = head — 디바이스 도어벨. dmacount는 16-bit MMIO 레지스터.
 *    디바이스는 (마지막에 본 dmacount, 현재 dmacount) 사이의 디스크립터를 수행하기 시작.
 *
 * 이 batched flush 패턴 덕분에 prep N개 후 flush 한 번 = 도어벨 1번 = MMIO 1번으로 효율적.
 */
void
spdk_ioat_flush(struct spdk_ioat_chan *ioat)
{
	uint32_t index = ioat_get_ring_index(ioat, ioat->head - 1);  /* [한국어] 마지막으로 prep된 슬롯 */
	union spdk_ioat_hw_desc *hw_desc;

	hw_desc = &ioat->hw_ring[index];
	hw_desc->dma.u.control.completion_update = 1;  /* [한국어] 이 디스크립터 완료 시 comp_update DMA 갱신 트리거 */
	ioat->regs->dmacount = (uint16_t)ioat->head;   /* [한국어] 도어벨 — 디바이스가 head까지의 디스크립터를 처리 */
}

/*
 * [한국어]
 * ioat_prep_null — 길이 0의 dummy 디스크립터를 ring에 prep.
 *
 * 채널 start 직후 디바이스를 idle 상태로 보내기 위한 wakeup 트릭.
 * dma.u.control.null=1 set + size=8(최소값) + src/dst=0. 디바이스는 "복사 작업 자체는 skip하고
 * completion만 보고"하는 동작을 수행 → 다음 정상 디스크립터를 받을 준비를 함.
 */
static struct ioat_descriptor *
ioat_prep_null(struct spdk_ioat_chan *ioat)
{
	struct ioat_descriptor *desc;
	union spdk_ioat_hw_desc *hw_desc;

	if (ioat_get_ring_space(ioat) < 1) {  /* [한국어] ring이 꽉참 — flow control 위반 */
		return NULL;
	}

	ioat_get_ring_entry(ioat, ioat->head, &desc, &hw_desc);

	hw_desc->dma.u.control_raw = 0;                  /* [한국어] control 비트필드 초기화 (null 외 모든 비트 0) */
	hw_desc->dma.u.control.op = SPDK_IOAT_OP_COPY;   /* [한국어] opcode = COPY (null이지만 base op은 copy) */
	hw_desc->dma.u.control.null = 1;                 /* [한국어] null 비트 — 실제 데이터 전송은 안 함 */

	hw_desc->dma.size = 8;                           /* [한국어] 최소 크기 — 디바이스가 0 길이는 거부할 수 있음 */
	hw_desc->dma.src_addr = 0;
	hw_desc->dma.dest_addr = 0;

	desc->callback_fn = NULL;                        /* [한국어] 사용자 콜백 없음 — 내부 wakeup 용도 */
	desc->callback_arg = NULL;

	ioat_submit_single(ioat);                        /* [한국어] head++ — 호출자는 별도로 flush 호출 */

	return desc;
}

/*
 * [한국어]
 * ioat_prep_copy — copy 디스크립터 1개를 ring에 prep.
 *
 * @dst: 목적지 물리(또는 IOVA) 주소.
 * @src: 출발 물리주소.
 * @len: 복사 바이트 수 (max_xfer_size 이하).
 *
 * 호출자(spdk_ioat_build_copy)가 이미 vtophys 변환을 끝내고 페이지 경계로 잘라서 호출하므로
 * 본 함수는 단순히 ring slot을 채우고 head++만 한다.
 */
static struct ioat_descriptor *
ioat_prep_copy(struct spdk_ioat_chan *ioat, uint64_t dst,
	       uint64_t src, uint32_t len)
{
	struct ioat_descriptor *desc;
	union spdk_ioat_hw_desc *hw_desc;

	assert(len <= ioat->max_xfer_size);  /* [한국어] 디바이스 한계 초과 방지 — 호출자가 보장해야 함 */

	if (ioat_get_ring_space(ioat) < 1) {
		return NULL;                  /* [한국어] ring full — 호출자는 flush 후 재시도 또는 backpressure */
	}

	ioat_get_ring_entry(ioat, ioat->head, &desc, &hw_desc);

	hw_desc->dma.u.control_raw = 0;                  /* [한국어] control 필드 클리어 */
	hw_desc->dma.u.control.op = SPDK_IOAT_OP_COPY;   /* [한국어] opcode COPY */

	hw_desc->dma.size = len;
	hw_desc->dma.src_addr = src;
	hw_desc->dma.dest_addr = dst;

	desc->callback_fn = NULL;     /* [한국어] 마지막 디스크립터에서만 build_copy가 콜백 설정 */
	desc->callback_arg = NULL;

	ioat_submit_single(ioat);

	return desc;
}

/*
 * [한국어]
 * ioat_prep_fill — fill 디스크립터 1개 prep. 8B 패턴을 dst에 len 바이트만큼 반복 채움.
 *
 * dma_capabilities에 SPDK_IOAT_ENGINE_FILL_SUPPORTED 비트가 있을 때만 사용 가능 (모든 IOAT가 지원하지는 않음).
 */
static struct ioat_descriptor *
ioat_prep_fill(struct spdk_ioat_chan *ioat, uint64_t dst,
	       uint64_t fill_pattern, uint32_t len)
{
	struct ioat_descriptor *desc;
	union spdk_ioat_hw_desc *hw_desc;

	assert(len <= ioat->max_xfer_size);

	if (ioat_get_ring_space(ioat) < 1) {
		return NULL;
	}

	ioat_get_ring_entry(ioat, ioat->head, &desc, &hw_desc);

	hw_desc->fill.u.control_raw = 0;                  /* [한국어] fill 디스크립터의 control_raw — copy와 union으로 같은 필드 */
	hw_desc->fill.u.control.op = SPDK_IOAT_OP_FILL;   /* [한국어] opcode FILL */

	hw_desc->fill.size = len;
	hw_desc->fill.src_data = fill_pattern;            /* [한국어] 8B 패턴 — 디바이스가 dst에 반복 기록 */
	hw_desc->fill.dest_addr = dst;

	desc->callback_fn = NULL;
	desc->callback_arg = NULL;

	ioat_submit_single(ioat);

	return desc;
}

/*
 * [한국어]
 * ioat_reset_hw — 채널 HW reset 마스터 시퀀스.
 *
 * 단계 (IOAT spec 권장):
 *   1) 현재 active/idle이면 SUSPEND 후 idle 대기 (최대 20ms).
 *   2) CHANERR(write-1-to-clear)로 누적된 에러 비트 클리어.
 *   3) IOAT v3.0 미만이면 PCI config의 internal channel error 레지스터도 클리어.
 *   4) RESET 명령.
 *   5) reset_pending 폴링 (최대 20ms).
 *
 * 호출자: ioat_channel_start. ring 셋업 후 정상 출발선으로 돌리기 위해 한 번 수행.
 */
static int
ioat_reset_hw(struct spdk_ioat_chan *ioat)
{
	int timeout;
	uint64_t status;
	uint32_t chanerr;
	int rc;

	status = ioat_get_chansts(ioat);
	if (is_ioat_active(status) || is_ioat_idle(status)) {
		ioat_suspend(ioat);  /* [한국어] active/idle 상태면 먼저 suspend로 보낸다 (halt 상태에서는 suspend 의미 없음) */
	}

	timeout = 20; /* in milliseconds */
	while (is_ioat_active(status) || is_ioat_idle(status)) {
		spdk_delay_us(1000);  /* [한국어] 1ms 슬립 — busy poll보다 부드러움 */
		timeout--;
		if (timeout == 0) {
			SPDK_ERRLOG("timed out waiting for suspend\n");
			return -1;
		}
		status = ioat_get_chansts(ioat);
	}

	/*
	 * Clear any outstanding errors.
	 * CHANERR is write-1-to-clear, so write the current CHANERR bits back to reset everything.
	 */
	chanerr = ioat->regs->chanerr;        /* [한국어] 현재 set된 에러 비트맵 읽기 */
	ioat->regs->chanerr = chanerr;        /* [한국어] 같은 비트를 쓰면 W1C 동작으로 클리어됨 */

	if (ioat->regs->cbver < SPDK_IOAT_VER_3_3) {
		/* [한국어] 구형(<3.3) 디바이스는 PCI config space에 별도의 internal error 레지스터를 가짐 */
		rc = spdk_pci_device_cfg_read32(ioat->device, &chanerr,
						SPDK_IOAT_PCI_CHANERR_INT_OFFSET);
		if (rc) {
			SPDK_ERRLOG("failed to read the internal channel error register\n");
			return -1;
		}

		spdk_pci_device_cfg_write32(ioat->device, chanerr,
					    SPDK_IOAT_PCI_CHANERR_INT_OFFSET);
		/* [한국어] write-back으로 W1C 클리어 */
	}

	ioat_reset(ioat);   /* [한국어] RESET 명령 발행 */

	timeout = 20;
	while (ioat_reset_pending(ioat)) {
		spdk_delay_us(1000);
		timeout--;
		if (timeout == 0) {
			SPDK_ERRLOG("timed out waiting for reset\n");
			return -1;
		}
	}

	return 0;
}

/*
 * [한국어]
 * ioat_process_channel_events — 완료된 디스크립터를 회수하고 사용자 콜백 호출.
 *
 * @return: 처리한 이벤트 수. 채널 halt 시 -1.
 *
 * 동작:
 *   1) head==tail이면 작업 없음.
 *   2) comp_update DMA 영역에서 last completed phys_addr 추출 (status & MASK).
 *   3) 채널이 halted 상태면 chanerr 로그 + 에러 반환.
 *   4) last_seen과 같으면 새 완료 없음.
 *   5) tail부터 시작해 desc->phys_addr이 last_completed와 같아질 때까지 회수 루프 —
 *      각 desc의 callback_fn(callback_arg) 호출, tail++.
 *   6) last_seen 갱신.
 *
 * 폴링 모델: 인터럽트 사용 안 함. 호출자(SPDK reactor)가 주기적으로 spdk_ioat_process_events를 호출.
 * lockless: 채널 소유 스레드만 head/tail/last_seen에 접근하므로 동기화 불필요.
 */
static int
ioat_process_channel_events(struct spdk_ioat_chan *ioat)
{
	struct ioat_descriptor *desc;
	uint64_t status, completed_descriptor, hw_desc_phys_addr, events_count = 0;
	uint32_t tail;

	if (ioat->head == ioat->tail) {
		return 0;  /* [한국어] 진행 중인 디스크립터 없음 — 빠른 패스 */
	}

	status = *ioat->comp_update;  /* [한국어] DMA 영역에서 디바이스가 갱신한 status read.
				       * comp_update는 spdk_zmalloc(SPDK_MALLOC_DMA)로 hugepage에 할당됨. */
	completed_descriptor = status & SPDK_IOAT_CHANSTS_COMPLETED_DESCRIPTOR_MASK;
	/* [한국어] 64-bit status 중 phys_addr 부분 마스킹 — IOAT spec: 상위 비트는 채널 상태, 하위는 last completed 주소 */

	if (is_ioat_halted(status)) {
		SPDK_ERRLOG("Channel halted (%x)\n", ioat->regs->chanerr);
		return -1;
	}

	if (completed_descriptor == ioat->last_seen) {
		return 0;  /* [한국어] 마지막으로 본 위치와 동일 — 새 완료 없음 */
	}

	do {
		tail = ioat_get_ring_index(ioat, ioat->tail);
		desc = &ioat->ring[tail];

		if (desc->callback_fn) {
			desc->callback_fn(desc->callback_arg);
			/* [한국어] 사용자 콜백 호출 — build_copy/fill에서 마지막 디스크립터에만 set됨.
			 * 콜백은 채널 소유 스레드에서 실행되므로 추가 락 없이 자료구조 접근 가능. */
		}

		hw_desc_phys_addr = desc->phys_addr;  /* [한국어] 이 SW desc에 대응하는 HW desc의 phys_addr 보관 */
		ioat->tail++;                         /* [한국어] tail 진행 — ring slot 회수 */
		events_count++;
	} while (hw_desc_phys_addr != completed_descriptor);
	/* [한국어] 디바이스가 last_completed로 보고한 phys_addr까지 도달하면 종료.
	 * 디바이스는 디스크립터를 in-order로 처리하므로 안전. */

	ioat->last_seen = hw_desc_phys_addr;  /* [한국어] 다음 폴링에서 같은 위치 재처리 방지 */

	return events_count;
}

/*
 * [한국어]
 * ioat_channel_destruct — 채널 자원 해제. ring 메모리, hw_ring DMA, comp_update DMA, BAR 매핑 모두 해제.
 *
 * 호출자: spdk_ioat_detach. 이미 attached 리스트에서 제거된 후 호출되어 동시성 문제 없음.
 */
static void
ioat_channel_destruct(struct spdk_ioat_chan *ioat)
{
	ioat_unmap_pci_bar(ioat);   /* [한국어] BAR0 unmap */

	if (ioat->ring) {
		free(ioat->ring);   /* [한국어] SW ring — 일반 malloc 영역 */
	}

	if (ioat->hw_ring) {
		spdk_free(ioat->hw_ring);  /* [한국어] HW ring — DMA 가능 hugepage 영역 (spdk_zmalloc과 짝) */
	}

	if (ioat->comp_update) {
		spdk_free((void *)ioat->comp_update);  /* [한국어] completion DMA 영역 — DMA 가능 */
		ioat->comp_update = NULL;
	}
}

/*
 * [한국어]
 * spdk_ioat_get_max_descriptors — 채널의 ring 슬롯 수 반환 (=2^ring_size_order).
 * 호출자가 backpressure 한계를 알고 싶을 때 사용. 실제 가용 슬롯은 -1 sentinel을 빼야 함.
 */
uint32_t
spdk_ioat_get_max_descriptors(struct spdk_ioat_chan *ioat)
{
	return 1 << ioat->ring_size_order;
}

/*
 * [한국어]
 * ioat_channel_start — 채널 부팅 마스터 절차.
 *
 * 단계:
 *   1) BAR0 매핑.
 *   2) version 검증 (>= 3.0). 본 드라이버는 ioat v3 계열 이후만 지원.
 *   3) dma_capability에서 fill 지원 여부 확인.
 *   4) xfercap 레지스터로 max_xfer_size 결정. 0 → 4GB, [12,31] → 1<<xfercap.
 *   5) comp_update DMA 영역 할당 + IOVA 변환.
 *   6) ring(SW), hw_ring(DMA, 64B 정렬) 할당.
 *   7) 각 hw_desc의 next 필드를 다음 슬롯의 phys_addr로 체이닝 — IOAT는 next 필드를 따라 진행.
 *      특이점: i번 슬롯의 phys_addr을 (i-1)번 hw_desc의 next에 기록 (mod ring_size_order로 wrap).
 *   8) ioat_reset_hw로 채널 정리.
 *   9) chanctrl: ANY_ERR_ABORT_EN — 어떤 에러든 발생 시 즉시 abort.
 *   10) chancmp/chainaddr 레지스터에 comp_update phys_addr/ring[0] phys_addr 기록.
 *   11) null 디스크립터 prep + flush로 채널 wakeup. idle 상태 폴링 (최대 10ms).
 *   12) idle이면 process_channel_events로 null의 완료를 회수. 아니면 에러.
 */
static int
ioat_channel_start(struct spdk_ioat_chan *ioat)
{
	uint8_t xfercap, version;
	uint64_t status = 0;
	int i, num_descriptors;
	uint64_t comp_update_bus_addr = 0;
	uint64_t phys_addr;

	if (ioat_map_pci_bar(ioat) != 0) {
		SPDK_ERRLOG("ioat_map_pci_bar() failed\n");
		return -1;
	}

	version = ioat->regs->cbver;             /* [한국어] CB version 레지스터 — 상위 4비트=major, 하위=minor */
	if (version < SPDK_IOAT_VER_3_0) {
		SPDK_ERRLOG(" unsupported IOAT version %u.%u\n",
			    version >> 4, version & 0xF);
		return -1;
	}

	/* Always support DMA copy */
	ioat->dma_capabilities = SPDK_IOAT_ENGINE_COPY_SUPPORTED;  /* [한국어] copy는 모든 IOAT가 지원 */
	if (ioat->regs->dmacapability & SPDK_IOAT_DMACAP_BFILL) {
		ioat->dma_capabilities |= SPDK_IOAT_ENGINE_FILL_SUPPORTED;  /* [한국어] block fill 지원 비트 */
	}
	xfercap = ioat->regs->xfercap;

	/* Only bits [4:0] are valid. */
	xfercap &= 0x1f;
	if (xfercap == 0) {
		/* 0 means 4 GB max transfer size. */
		ioat->max_xfer_size = 1ULL << 32;  /* [한국어] xfercap=0은 특수값 — 최대 4GB까지 단일 디스크립터로 가능 */
	} else if (xfercap < 12) {
		/* XFERCAP must be at least 12 (4 KB) according to the spec. */
		SPDK_ERRLOG("invalid XFERCAP value %u\n", xfercap);
		return -1;
	} else {
		ioat->max_xfer_size = 1U << xfercap;  /* [한국어] 일반 케이스: 2^xfercap 바이트 */
	}

	ioat->comp_update = spdk_zmalloc(sizeof(*ioat->comp_update), SPDK_IOAT_CHANCMP_ALIGN,
					 NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	/* [한국어] DMA 가능 영역에 completion 슬롯 할당. SPDK_IOAT_CHANCMP_ALIGN(=64B) 정렬은 spec 요구. */
	if (ioat->comp_update == NULL) {
		return -1;
	}

	comp_update_bus_addr = spdk_vtophys((void *)ioat->comp_update, NULL);
	/* [한국어] comp_update의 IOVA(또는 PA) 변환. NULL은 length 갱신 받지 않음을 의미. */
	if (comp_update_bus_addr == SPDK_VTOPHYS_ERROR) {
		return -1;
	}

	ioat->ring_size_order = IOAT_DEFAULT_ORDER;  /* [한국어] 기본 ring 크기 = 2^IOAT_DEFAULT_ORDER (보통 14 = 16384 슬롯) */

	num_descriptors = 1 << ioat->ring_size_order;

	ioat->ring = calloc(num_descriptors, sizeof(struct ioat_descriptor));
	/* [한국어] SW ring — 일반 메모리, callback 메타데이터 보관 */
	if (!ioat->ring) {
		return -1;
	}

	ioat->hw_ring = spdk_zmalloc(num_descriptors * sizeof(union spdk_ioat_hw_desc), 64,
				     NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	/* [한국어] HW ring — DMA 가능, 64B 정렬 (디스크립터가 64B 단위이므로 cache line 맞춤). */
	if (!ioat->hw_ring) {
		return -1;
	}

	for (i = 0; i < num_descriptors; i++) {
		phys_addr = spdk_vtophys(&ioat->hw_ring[i], NULL);
		if (phys_addr == SPDK_VTOPHYS_ERROR) {
			SPDK_ERRLOG("Failed to translate descriptor %u to physical address\n", i);
			return -1;
		}

		ioat->ring[i].phys_addr = phys_addr;
		/* [한국어] SW desc에 자기 HW desc의 phys_addr 캐시 — process_channel_events에서 빠른 비교용 */
		ioat->hw_ring[ioat_get_ring_index(ioat, i - 1)].generic.next = phys_addr;
		/* [한국어] (i-1)번 디스크립터의 next 필드를 i의 phys_addr로 set — 환형 체이닝.
		 * i=0일 때 (i-1)는 마지막 슬롯으로 wrap → 환형 ring 형성. 디바이스는 chainaddr부터 시작해
		 * next 필드를 따라가며 디스크립터를 fetch. */
	}

	ioat->head = 0;
	ioat->tail = 0;
	ioat->last_seen = 0;

	ioat_reset_hw(ioat);  /* [한국어] HW reset — 깨끗한 출발 */

	ioat->regs->chanctrl = SPDK_IOAT_CHANCTRL_ANY_ERR_ABORT_EN;
	/* [한국어] 채널 제어: 어떤 에러든 발생 즉시 abort & halt — 자가 격리. process_channel_events가 halted 감지. */
	ioat_write_chancmp(ioat, comp_update_bus_addr);  /* [한국어] 디바이스에 completion DMA 주소 알림 */
	ioat_write_chainaddr(ioat, ioat->ring[0].phys_addr);  /* [한국어] 디스크립터 체인 시작 주소 */

	ioat_prep_null(ioat);     /* [한국어] dummy 디스크립터로 채널 wakeup */
	spdk_ioat_flush(ioat);    /* [한국어] 도어벨 — 디바이스가 처리 시작 */

	i = 100;
	while (i-- > 0) {
		spdk_delay_us(100);          /* [한국어] 100us 폴링 슬립 */
		status = ioat_get_chansts(ioat);
		if (is_ioat_idle(status)) {
			break;               /* [한국어] null 처리 후 idle 도달 — 정상 */
		}
	}

	if (is_ioat_idle(status)) {
		ioat_process_channel_events(ioat);  /* [한국어] null의 완료를 회수해 ring 깨끗하게 */
	} else {
		SPDK_ERRLOG("could not start channel: status = %p\n error = %#x\n",
			    (void *)status, ioat->regs->chanerr);
		return -1;
	}

	return 0;
}

/* Caller must hold g_ioat_driver.lock */
/*
 * [한국어]
 * ioat_attach — PCI 디바이스를 spdk_ioat_chan으로 래핑 후 채널 부팅.
 *
 * 1) spdk_ioat_chan 할당.
 * 2) PCI command register bit 2 (busmaster) set — DMA 가능하게.
 * 3) ioat->device 보관.
 * 4) ioat_channel_start 호출 — 실패 시 destruct + free 후 NULL 반환.
 */
static struct spdk_ioat_chan *
ioat_attach(struct spdk_pci_device *device)
{
	struct spdk_ioat_chan *ioat;
	uint32_t cmd_reg;

	ioat = calloc(1, sizeof(struct spdk_ioat_chan));
	if (ioat == NULL) {
		return NULL;
	}

	/* Enable PCI busmaster. */
	spdk_pci_device_cfg_read32(device, &cmd_reg, 4);  /* [한국어] PCI cfg offset 4 = command register */
	cmd_reg |= 0x4;                                   /* [한국어] bit 2 = busmaster — 디바이스가 호스트 메모리에 DMA 가능 */
	spdk_pci_device_cfg_write32(device, cmd_reg, 4);

	ioat->device = device;

	if (ioat_channel_start(ioat) != 0) {
		ioat_channel_destruct(ioat);
		free(ioat);
		return NULL;
	}

	return ioat;
}

/*
 * [한국어]
 * struct ioat_enum_ctx — spdk_pci_enumerate 콜백에 전달되는 컨텍스트.
 * 사용자 probe_cb/attach_cb와 ctx를 묶음.
 */
struct ioat_enum_ctx {
	spdk_ioat_probe_cb probe_cb;
	/* [한국어] 사용자 결정 콜백. true 반환 시 attach 진행. */

	spdk_ioat_attach_cb attach_cb;
	/* [한국어] attach 성공 시 사용자에게 채널 핸들 전달. */

	void *cb_ctx;
	/* [한국어] 사용자 컨텍스트. */
};

/* This function must only be called while holding g_ioat_driver.lock */
/*
 * [한국어]
 * ioat_enum_cb — spdk_pci_enumerate가 디바이스 하나를 발견할 때마다 호출.
 *
 * 흐름:
 *   1) attached_chans를 순회하며 dedupe — 이미 attach된 디바이스면 0 반환(스킵).
 *   2) 사용자 probe_cb가 true면 ioat_attach → attached_chans에 insert → 사용자 attach_cb 호출.
 *
 * NOTE: PCI 추상화 계층이 enumeration 간에 동일 device handle을 사용한다고 가정.
 *       그렇지 않다면 BDF(domain:bus:dev.func)로 비교해야 함.
 */
static int
ioat_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct ioat_enum_ctx *enum_ctx = ctx;
	struct spdk_ioat_chan *ioat;

	/* Verify that this device is not already attached */
	TAILQ_FOREACH(ioat, &g_ioat_driver.attached_chans, tailq) {
		/*
		 * NOTE: This assumes that the PCI abstraction layer will use the same device handle
		 *  across enumerations; we could compare by BDF instead if this is not true.
		 */
		if (pci_dev == ioat->device) {
			return 0;  /* [한국어] 이미 attached — 두 번 잡지 않도록 0(skip) 반환 */
		}
	}

	if (enum_ctx->probe_cb(enum_ctx->cb_ctx, pci_dev)) {
		/*
		 * Since I/OAT init is relatively quick, just perform the full init during probing.
		 *  If this turns out to be a bottleneck later, this can be changed to work like
		 *  NVMe with a list of devices to initialize in parallel.
		 */
		/* [한국어] IOAT init은 ms 수준이므로 enumerate 동안 동기 attach. NVMe처럼 병렬화는 미지원. */
		ioat = ioat_attach(pci_dev);
		if (ioat == NULL) {
			SPDK_ERRLOG("ioat_attach() failed\n");
			return -1;
		}

		TAILQ_INSERT_TAIL(&g_ioat_driver.attached_chans, ioat, tailq);  /* [한국어] 글로벌 리스트에 등록 */

		enum_ctx->attach_cb(enum_ctx->cb_ctx, pci_dev, ioat);            /* [한국어] 사용자에게 채널 핸들 전달 */
	}

	return 0;
}

/*
 * [한국어]
 * spdk_ioat_probe — 공개 API. 모든 IOAT PCI 디바이스를 enumerate하여 attach.
 *
 * @cb_ctx, @probe_cb, @attach_cb: 사용자 콜백 + 컨텍스트.
 * @return: spdk_pci_enumerate 반환값.
 *
 * g_ioat_driver.lock으로 직렬화. 내부에서 ioat_enum_cb가 호출될 때마다 이미 lock 보유 상태.
 */
int
spdk_ioat_probe(void *cb_ctx, spdk_ioat_probe_cb probe_cb, spdk_ioat_attach_cb attach_cb)
{
	int rc;
	struct ioat_enum_ctx enum_ctx;

	pthread_mutex_lock(&g_ioat_driver.lock);

	enum_ctx.probe_cb = probe_cb;
	enum_ctx.attach_cb = attach_cb;
	enum_ctx.cb_ctx = cb_ctx;

	rc = spdk_pci_enumerate(spdk_pci_ioat_get_driver(), ioat_enum_cb, &enum_ctx);

	pthread_mutex_unlock(&g_ioat_driver.lock);

	return rc;
}

/*
 * [한국어]
 * spdk_ioat_detach — 공개 API. 채널을 attached 리스트에서 제거하고 자원 해제.
 *
 * 호출 컨텍스트: 채널이 더 이상 사용되지 않을 때. ring에 outstanding 디스크립터가 없어야 안전 (호출자 책임).
 */
void
spdk_ioat_detach(struct spdk_ioat_chan *ioat)
{
	struct ioat_driver	*driver = &g_ioat_driver;

	/* ioat should be in the free list (not registered to a thread)
	 * when calling ioat_detach().
	 */
	pthread_mutex_lock(&driver->lock);
	TAILQ_REMOVE(&driver->attached_chans, ioat, tailq);   /* [한국어] 리스트에서 제거 — 다른 enumerate가 이 디바이스 다시 보지 않도록 */
	pthread_mutex_unlock(&driver->lock);

	ioat_channel_destruct(ioat);  /* [한국어] BAR unmap, ring/comp_update free */
	free(ioat);
}

/*
 * [한국어]
 * spdk_ioat_build_copy — 사용자 가상주소 범위(dst, src)의 nbytes를 IOAT ring에 prep.
 *
 * 단일 디스크립터로 처리할 수 없는 큰 범위(또는 페이지 분할이 일어나는 범위)를
 * vtophys로 한 청크씩 잘라 ioat_prep_copy를 반복 호출한다.
 *
 * @cb_arg, @cb_fn: 모든 디스크립터의 마지막에 한 번 호출될 사용자 콜백.
 * @return: 0 성공, -EINVAL(vtophys 실패), -ENOMEM(ring full).
 *
 * 동작:
 *   1) orig_head 보관 — ring full 시 롤백 위해.
 *   2) remaining > 0 동안:
 *      a) src/dst를 각각 vtophys (dst_len/src_len에 연속 매핑된 길이 반환).
 *      b) op_size = min(dst_len, src_len, max_xfer_size).
 *      c) ioat_prep_copy(pdst, psrc, op_size).
 *      d) remaining=0이거나 ring full(last_desc==NULL)이면 종료.
 *   3) nbytes==0이면 null 디스크립터 prep — copy 0바이트는 의미 없으므로 사용자 콜백만 트리거.
 *   4) 마지막 디스크립터에 callback 설정. ring 부족이면 head 롤백 후 -ENOMEM.
 *
 * 주의: 호출자는 단일 스레드(채널 소유)에서만 호출해야 함 (ring head 동시성).
 */
int
spdk_ioat_build_copy(struct spdk_ioat_chan *ioat, void *cb_arg, spdk_ioat_req_cb cb_fn,
		     void *dst, const void *src, uint64_t nbytes)
{
	struct ioat_descriptor	*last_desc = NULL;
	uint64_t	remaining, op_size;
	uint64_t	vdst, vsrc;
	uint64_t	pdst_addr, psrc_addr, dst_len, src_len;
	uint32_t	orig_head;

	if (!ioat) {
		return -EINVAL;
	}

	orig_head = ioat->head;          /* [한국어] ring 부족 시 롤백할 head 위치 */

	vdst = (uint64_t)dst;             /* [한국어] 가상주소를 정수로 — 산술 편의 */
	vsrc = (uint64_t)src;

	remaining = nbytes;
	while (remaining) {
		src_len = dst_len = remaining;  /* [한국어] vtophys 입력값 — 디바이스/매핑이 더 작은 길이를 반환할 수 있음 */

		psrc_addr = spdk_vtophys((void *)vsrc, &src_len);
		/* [한국어] src 가상주소를 IOVA(또는 PA)로 변환. src_len에 연속 매핑된 실제 길이 반환.
		 * 페이지 경계나 IOMMU 매핑 단위에서 잘릴 수 있음. */
		if (psrc_addr == SPDK_VTOPHYS_ERROR) {
			return -EINVAL;
		}
		pdst_addr = spdk_vtophys((void *)vdst, &dst_len);
		if (pdst_addr == SPDK_VTOPHYS_ERROR) {
			return -EINVAL;
		}

		op_size = spdk_min(dst_len, src_len);     /* [한국어] 양쪽 모두에서 연속 매핑된 만큼만 처리 가능 */
		op_size = spdk_min(op_size, ioat->max_xfer_size);  /* [한국어] 디바이스 단일 디스크립터 한계 */
		remaining -= op_size;

		last_desc = ioat_prep_copy(ioat, pdst_addr, psrc_addr, op_size);

		if (remaining == 0 || last_desc == NULL) {
			break;  /* [한국어] 모두 처리했거나 ring 부족 — 후자는 아래 -ENOMEM 처리 */
		}

		vsrc += op_size;     /* [한국어] 다음 청크로 진행 */
		vdst += op_size;

	}
	/* Issue null descriptor for null transfer */
	if (nbytes == 0) {
		last_desc = ioat_prep_null(ioat);  /* [한국어] 0바이트 copy는 사용자 콜백만 의미 — null로 처리 */
	}

	if (last_desc) {
		last_desc->callback_fn = cb_fn;     /* [한국어] 마지막 디스크립터에만 콜백 — 디바이스가 in-order 처리하므로 마지막 완료 = 전체 완료 */
		last_desc->callback_arg = cb_arg;
	} else {
		/*
		 * Ran out of descriptors in the ring - reset head to leave things as they were
		 * in case we managed to fill out any descriptors.
		 */
		/* [한국어] ring 부족 — prep된 디스크립터가 있어도 commit하지 않은 상태이므로 head를 원위치.
		 * 디바이스는 dmacount 도어벨이 없는 한 새 디스크립터를 fetch하지 않음. */
		ioat->head = orig_head;
		return -ENOMEM;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_ioat_submit_copy — build_copy 후 즉시 flush. 가장 일반적인 사용 경로.
 */
int
spdk_ioat_submit_copy(struct spdk_ioat_chan *ioat, void *cb_arg, spdk_ioat_req_cb cb_fn,
		      void *dst, const void *src, uint64_t nbytes)
{
	int rc;

	rc = spdk_ioat_build_copy(ioat, cb_arg, cb_fn, dst, src, nbytes);
	if (rc != 0) {
		return rc;
	}

	spdk_ioat_flush(ioat);  /* [한국어] 도어벨 — 디바이스가 ring 처리 시작 */
	return 0;
}

/*
 * [한국어]
 * spdk_ioat_build_fill — fill (8B 패턴 반복) 디스크립터를 nbytes만큼 ring에 prep.
 *
 * src가 없으므로 dst 단일 vtophys만 수행. dma_capabilities가 FILL을 지원하지 않으면 -1 반환.
 */
int
spdk_ioat_build_fill(struct spdk_ioat_chan *ioat, void *cb_arg, spdk_ioat_req_cb cb_fn,
		     void *dst, uint64_t fill_pattern, uint64_t nbytes)
{
	struct ioat_descriptor	*last_desc = NULL;
	uint64_t	remaining, op_size;
	uint64_t	vdst;
	uint64_t	pdst_addr, dst_len;
	uint32_t	orig_head;

	if (!ioat) {
		return -EINVAL;
	}

	if (!(ioat->dma_capabilities & SPDK_IOAT_ENGINE_FILL_SUPPORTED)) {
		SPDK_ERRLOG("Channel does not support memory fill\n");
		return -1;  /* [한국어] FILL 미지원 디바이스 */
	}

	orig_head = ioat->head;

	vdst = (uint64_t)dst;
	remaining = nbytes;

	while (remaining) {
		dst_len = remaining;
		pdst_addr = spdk_vtophys((void *)vdst, &dst_len);
		if (pdst_addr == SPDK_VTOPHYS_ERROR) {
			return -EINVAL;
		}

		op_size = spdk_min(dst_len, ioat->max_xfer_size);
		remaining -= op_size;

		last_desc = ioat_prep_fill(ioat, pdst_addr, fill_pattern, op_size);

		if (remaining == 0 || last_desc == NULL) {
			break;
		}

		vdst += op_size;
	}

	if (last_desc) {
		last_desc->callback_fn = cb_fn;
		last_desc->callback_arg = cb_arg;
	} else {
		/*
		 * Ran out of descriptors in the ring - reset head to leave things as they were
		 * in case we managed to fill out any descriptors.
		 */
		ioat->head = orig_head;
		return -ENOMEM;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_ioat_submit_fill — build_fill + flush.
 */
int
spdk_ioat_submit_fill(struct spdk_ioat_chan *ioat, void *cb_arg, spdk_ioat_req_cb cb_fn,
		      void *dst, uint64_t fill_pattern, uint64_t nbytes)
{
	int rc;

	rc = spdk_ioat_build_fill(ioat, cb_arg, cb_fn, dst, fill_pattern, nbytes);
	if (rc != 0) {
		return rc;
	}

	spdk_ioat_flush(ioat);
	return 0;
}

/*
 * [한국어]
 * spdk_ioat_get_dma_capabilities — 채널이 지원하는 DMA op 비트맵.
 * SPDK_IOAT_ENGINE_COPY_SUPPORTED, SPDK_IOAT_ENGINE_FILL_SUPPORTED 등.
 */
uint32_t
spdk_ioat_get_dma_capabilities(struct spdk_ioat_chan *ioat)
{
	if (!ioat) {
		return 0;
	}
	return ioat->dma_capabilities;
}

/*
 * [한국어]
 * spdk_ioat_process_events — 공개 폴링 진입점. SPDK reactor가 주기적으로 호출.
 * 내부적으로 ioat_process_channel_events 호출 — 완료된 디스크립터 회수 + 콜백 실행.
 */
int
spdk_ioat_process_events(struct spdk_ioat_chan *ioat)
{
	return ioat_process_channel_events(ioat);
}

/* [한국어] SPDK 로그 컴포넌트 등록 — SPDK_DEBUGLOG(ioat, ...)가 활성화되도록 "ioat" 이름의 컴포넌트 생성 */
SPDK_LOG_REGISTER_COMPONENT(ioat)
