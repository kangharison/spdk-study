/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

/*
 * [한국어 설명] AE4DMA 유저스페이스 드라이버 (ae4dma.c)
 *
 * === 파일의 역할 ===
 * AMD AE4DMA(Accelerated Engine 4 DMA, AMD EPYC/Pensando 계열의 임베디드 DMA
 * 엔진) PCIe 디바이스를 SPDK 유저스페이스에서 직접 제어하는 드라이버이다.
 * 호스트 커널을 우회하여 BAR0를 매핑하고, hugepage 기반 DMA 메모리에
 * 디스크립터 링을 만들고, 16개 H/W 큐를 활성화/플러시/폴링한다. 외부에는
 * `spdk_ae4dma_probe` / `spdk_ae4dma_attach` / `spdk_ae4dma_build_copy` /
 * `spdk_ae4dma_flush` / `spdk_ae4dma_process_events` / `spdk_ae4dma_detach`
 * 같은 호출만 노출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [상위 사용자 (예: ae4dma accel module 또는 데모 앱)]
 *      ↓ spdk_ae4dma_build_copy → flush → process_events
 *   [본 파일: AE4DMA 드라이버]
 *      ↓ spdk_pci_device_*  /  spdk_mmio_write_4  /  spdk_dma_zmalloc
 *   [SPDK env_dpdk + PCIe BAR0]
 *      ↓ MMIO write/read
 *   [HARDWARE: AE4DMA 엔진 (16 H/W queues)]
 * 실행 컨텍스트: 호스트 유저스페이스(SPDK reactor 스레드). 인터럽트 미사용.
 * 모든 완료 통보는 polling(`process_events`)으로 이뤄진다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `ae4dma_internal.h` / `ae4dma_spec.h` / `spdk/env.h`(spdk_dma_zmalloc/
 *   pci enumerate), `spdk/util.h`(ioviter), `spdk/memory.h`(spdk_vtophys),
 *   `spdk/log.h`.
 * - 의존자: 상위 SPDK accel ae4dma 모듈, 또는 ae4dma 예제 앱.
 * - 데이터 흐름: 사용자 가상 주소 src/dst → spdk_vtophys로 PA 변환 →
 *   `spdk_ae4dma_desc`에 hi/lo로 분할 적재 → write_index ++ → spdk_ae4dma_flush가
 *   MMIO write_idx 갱신 → H/W가 fetch → DMA 수행 → dw1.status 갱신 →
 *   `spdk_ae4dma_process_events`가 polling으로 발견하고 사용자 콜백 호출.
 * - 공유 자료구조: 글로벌 `g_ae4dma_driver`가 모든 attach된 채널(spdk_ae4dma_chan)
 *   을 TAILQ로 관리하며, mutex로 enumeration/detach 시 동시성을 보호.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_ae4dma_probe: PCI 열거 콜백을 호출자에게 노출하고 attach까지 수행.
 * - ae4dma_attach: 채널 객체 생성, PCI busmaster enable, channel_start 호출.
 * - ae4dma_channel_start: BAR 매핑, 16개 큐의 디스크립터 메모리 할당과 H/W 레지스터 초기화.
 * - spdk_ae4dma_build_copy: 사용자 iovec 페어를 다중 디스크립터로 변환해 ring에 적재.
 * - spdk_ae4dma_flush: write_idx MMIO 기록(=H/W에 fetch 트리거).
 * - spdk_ae4dma_process_events / ae4dma_process_channel_events: 폴링 루프에서 완료 디스크립터 회수.
 * - struct ae4dma_driver: 드라이버 글로벌 — 채널 리스트와 lock.
 */

/* [한국어] SPDK 표준 인클루드(stdint/stdbool 등). */
#include "spdk/stdinc.h"
/* [한국어] 본 드라이버 전용 내부 자료구조와 매크로 (큐 메타, upper/lower_32_bits 등). */
#include "ae4dma_internal.h"

/* [한국어] DPDK 기반 환경 추상 — PCI 열거/매핑, hugepage 기반 DMA 메모리 할당 등. */
#include "spdk/env.h"
/* [한국어] spdk_ioviter — 두 iovec 배열을 동시에 순회하며 매칭되는 (src,dst,len)을 산출. */
#include "spdk/util.h"
/* [한국어] spdk_vtophys — 가상→물리 주소 변환(hugepage 매핑 정보 사용). */
#include "spdk/memory.h"

/* [한국어] SPDK 로깅 매크로(SPDK_ERRLOG, SPDK_DEBUGLOG …). */
#include "spdk/log.h"

/* [한국어] DMA-able 메모리 정렬 — 디스크립터 영역(spdk_dma_zmalloc)을 32바이트 정렬로 받기 위함.
 *  AE4DMA 디스크립터는 32바이트(=8 dword)이므로 32바이트 정렬이면 디스크립터 경계에 깔끔히 맞음. */
#define ALIGN_DWORD 32

/*
 * [한국어] struct ae4dma_driver — 드라이버의 프로세스 글로벌 컨텍스트.
 *  attach된 모든 채널을 TAILQ로 관리하며, 등록/해제 시 mutex로 보호한다.
 *  애초에 SPDK가 lockless를 지향하지만, probe/detach는 어플리케이션 수명 동안
 *  드물게 일어나는 제어 경로라서 pthread_mutex로 단순 직렬화한다.
 */
struct ae4dma_driver {
	pthread_mutex_t	lock;
	/* [한국어] 채널 리스트(attached_chans) 보호용 mutex.
	 * 잠그는 자: spdk_ae4dma_probe가 enumeration 동안 보유, spdk_ae4dma_detach도 보유.
	 * 동기화: probe와 detach가 다른 스레드에서 동시 실행되어도 채널 리스트 일관성 유지. */

	TAILQ_HEAD(, spdk_ae4dma_chan) attached_chans;
	/* [한국어] 현재 attach되어 사용 중인 모든 AE4DMA 채널의 리스트.
	 * 설정자: enum_cb가 INSERT_TAIL, detach가 REMOVE.
	 * 읽는 자: enum_cb의 중복 체크 (이미 attached면 skip). */
};

/* [한국어] 드라이버 글로벌 인스턴스 — 정적 초기화로 프로세스 시작 시점에 즉시 사용 가능.
 *  PTHREAD_MUTEX_INITIALIZER는 글로벌 mutex의 정적 초기화 매크로. */
static struct ae4dma_driver g_ae4dma_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.attached_chans = TAILQ_HEAD_INITIALIZER(g_ae4dma_driver.attached_chans),
};

/**
 * DMA engine capability flags
 */
/*
 * [한국어] enum spdk_ae4dma_dma_capability_flags — 엔진 capability 비트마스크.
 *  채널의 dma_capabilities 필드에 OR하여 사용. 현재는 COPY만 정의.
 */
enum spdk_ae4dma_dma_capability_flags {
	SPDK_AE4DMA_ENGINE_COPY_SUPPORTED       = 0x1, /**< The memory copy is supported */
	/* [한국어] 메모리 카피(memcpy류 DMA) 지원 비트. 모든 AE4DMA 엔진은 기본 지원.
	 * 미래에 fill/compress/crc32c 등이 추가되면 비트로 확장 가능. */
};

/* Mapping the PCI BAR */
/*
 * [한국어]
 * ae4dma_map_pci_bar - PCIe BAR0를 호스트 가상 주소 공간으로 매핑.
 *
 * @ae4dma: 채널 컨텍스트 (device 필드에 PCI 핸들이 미리 채워져 있어야 함).
 * @return: 0 성공 / -1 실패(BAR 매핑 실패).
 *
 * 동기/배경: AE4DMA의 모든 큐 레지스터는 BAR0 위에 매핑된 MMIO 영역에 있으므로,
 * 디바이스 사용 전 반드시 한 번 매핑해야 한다. SPDK는 이를 vfio-pci/uio_pci_generic을
 * 통한 user-space 매핑(`spdk_pci_device_map_bar`)으로 처리한다.
 *
 * 호출 체인: ae4dma_attach → ae4dma_channel_start → [ae4dma_map_pci_bar]
 *                                                      → spdk_pci_device_map_bar
 */
static int
ae4dma_map_pci_bar(struct spdk_ae4dma_chan *ae4dma)
{
	int rc;            /* [한국어] 매핑 호출의 반환 코드 저장. */
	void *addr;        /* [한국어] 매핑된 가상 주소(out 파라미터로 받음). */
	uint64_t phys_addr, size;
	/* [한국어] phys_addr/size는 SPDK API 시그니처상 받아야 하지만 본 함수에서는 사용하지 않음.
	 *  size는 본 함수에선 사용하지 않으나 디바이스 BAR 크기 디버그에 활용 가능. */

	/* [한국어] BAR0를 매핑 — vfio-pci 또는 uio 백엔드를 통해 user-space 가상 주소를 부여받음.
	 *  성공 시 addr이 채워지고, ae4dma->io_regs에 저장해 큐 레지스터 계산의 베이스로 사용. */
	rc = spdk_pci_device_map_bar(ae4dma->device, AE4DMA_PCIE_BAR, &addr, &phys_addr, &size);
	if (rc != 0 || addr == NULL) {
		/* [한국어] 매핑 실패 — vfio 권한, IOMMU 미설정, BAR 미존재 등이 원인일 수 있음. */
		SPDK_ERRLOG("pci_device_map_range failed with error code %d\n", rc);
		return -1;
	}

	/* [한국어] 채널에 매핑 결과 저장 — 이후 큐별 regs 포인터 계산은 io_regs + (i+1)*32 형태. */
	ae4dma->io_regs = addr;

	return 0;
}

/*
 * [한국어]
 * ae4dma_unmap_pci_bar - 매핑된 BAR을 해제.
 *
 * @ae4dma: 채널 컨텍스트.
 * @return: spdk_pci_device_unmap_bar의 반환값 (보통 0).
 *
 * 호출 체인: ae4dma_channel_destruct → [ae4dma_unmap_pci_bar]
 */
static int
ae4dma_unmap_pci_bar(struct spdk_ae4dma_chan *ae4dma)
{
	int rc = 0;
	/* [한국어] io_regs는 void*로 보관되어 있으므로 명시적으로 캐스팅. */
	void *addr = (void *)ae4dma->io_regs;

	/* [한국어] addr이 NULL이면 매핑 자체가 안 됐던 것이므로 unmap 호출 생략. */
	if (addr) {
		rc = spdk_pci_device_unmap_bar(ae4dma->device, 0, addr);
	}

	return rc;
}

/*
 * [한국어]
 * spdk_ae4dma_flush - 적재된 디스크립터들을 H/W에 알리기(=fetch 트리거).
 *
 * @ae4dma: 채널 컨텍스트.
 * @hwq_id: 대상 H/W 큐 인덱스 (0..15).
 *
 * 동기: prep_copy로 디스크립터 링과 호스트 측 write_index를 갱신해도, H/W는
 *  자기 MMIO 레지스터 write_idx가 변하기 전까지 새 디스크립터를 fetch하지 않는다.
 *  따라서 적재 후 본 함수로 MMIO write_idx를 갱신해 doorbell 효과를 만든다.
 *
 * 호출 컨텍스트: 사용자(SPDK reactor)가 build_copy 후 일괄 flush 호출.
 *
 * 호출 체인: 사용자 → [spdk_ae4dma_flush] → spdk_mmio_write_4
 */
void
spdk_ae4dma_flush(struct spdk_ae4dma_chan *ae4dma, int hwq_id)
{
	volatile uint32_t write_idx;
	/* [한국어] volatile은 컴파일러가 메모리 위치를 캐싱해 read를 최적화로 제거하지 못하도록 강제. */

	/* To flush the updated descs by incrementing the write_index of the queue */
	/* [한국어] 호스트 측 write_index 값을 읽어서 그대로 MMIO write_idx에 기록.
	 *  이렇게 두 단계(읽기+쓰기)로 분리하는 이유는 MMIO write에 직접 비트 연산을 피하고
	 *  값 추적을 용이하게 하기 위함. */
	write_idx = ae4dma->cmd_q[hwq_id].write_index;
	/* [한국어] spdk_mmio_write_4: 4바이트 정렬 쓰기 + write barrier 보장.
	 *  H/W는 이 write를 보고 [tail..write_idx) 구간의 디스크립터를 fetch 시작. */
	spdk_mmio_write_4(&ae4dma->cmd_q[hwq_id].regs->write_idx, write_idx);
}

/*
 * [한국어]
 * ae4dma_prep_copy - 한 디스크립터(=하나의 src→dst PA 쌍, 길이 len)를
 *                   링에 적재. 콜백 정보는 이 함수에서 NULL로 두고
 *                   build_copy 종료 시 마지막 디스크립터에만 사용자 콜백을 매단다.
 *
 * @ae4dma: 채널.
 * @dst: 목적지 물리 주소 (이미 vtophys로 변환된 값).
 * @src: 소스 물리 주소.
 * @len: 전송 바이트 수 (max_xfer_size 이하 보장).
 * @hwq_index: 사용할 H/W 큐 인덱스.
 * @return: 적재된 호스트 측 그림자 디스크립터 포인터 / NULL 실패.
 *
 * 동작: write_index 위치의 ring/qbase_addr에 디스크립터 적재 → ring_buff_count++ →
 *       write_index를 (idx+1)%32로 wrap. 단, MMIO 트리거(=flush)는 호출하지 않는다.
 *
 * 호출 체인: spdk_ae4dma_build_copy → [ae4dma_prep_copy]
 */
static struct ae4dma_descriptor *
ae4dma_prep_copy(struct spdk_ae4dma_chan *ae4dma, uint64_t dst,
		 uint64_t src, uint32_t len, uint32_t hwq_index)
{
	struct ae4dma_descriptor *desc;        /* [한국어] 호스트 측 그림자 ring의 슬롯 포인터. */
	struct spdk_ae4dma_desc dma_desc;      /* [한국어] 작업용 H/W 디스크립터(스택 임시). */
	uint32_t hwq;                          /* [한국어] hwq_index의 단순 alias(코드 가독성). */
	uint32_t desc_index;                   /* [한국어] 적재 위치(write_index의 캐시). */

	/* [한국어] H/W 한도(max_xfer_size) 초과는 호출자가 막아야 함을 디버그 빌드에서 강제. */
	assert(len <= ae4dma->max_xfer_size);
	hwq = hwq_index;

	/* [한국어] 다음 적재 슬롯 인덱스 — 큐의 생산자 포인터. */
	desc_index = ae4dma->cmd_q[hwq].write_index;

	/* [한국어] 호스트 그림자 ring에서 해당 슬롯의 주소 — 콜백 보관 위치. */
	desc = &ae4dma->cmd_q[hwq].ring[desc_index];
	if (desc == NULL) {
		/* [한국어] 정상적으로는 ring이 channel_start에서 calloc된 상태라 NULL일 수 없음.
		 *  메모리 손상 등 비정상 상황에 대비한 방어 로깅. */
		SPDK_ERRLOG("desc at %d Q and %d ring is NULL\n", hwq, desc_index);
		return NULL;
	}

	/* [한국어] H/W 디스크립터 슬롯을 임시 변수로 가져와 필드별로 채움.
	 *  최종적으로 다시 qbase_addr[desc_index]에 대입(=H/W가 보는 메모리 위치)할 것임. */
	dma_desc = ae4dma->cmd_q[hwq].qbase_addr[desc_index];

	/* [한국어] 제어 비트(DWORD0 byte0) — 0(기본: stop/intr 등 아무것도 켜지 않음). */
	dma_desc.dw0.byte0 = 0;
	/* [한국어] 상태/에러/desc_id를 0으로 클리어 — H/W가 이후 처리하면서 갱신. */
	dma_desc.dw1.status = 0;
	dma_desc.dw1.err_code = 0;
	dma_desc.dw1.desc_id  = 0;
	/* [한국어] 전송 길이(바이트). 호출자가 보장한 ≤ max_xfer_size 값. */
	dma_desc.length = len;
	/* [한국어] 64비트 PA를 32비트 두 필드로 분할 — H/W ABI 요구. */
	dma_desc.src_hi = upper_32_bits(src);
	dma_desc.src_lo = lower_32_bits(src);
	dma_desc.dst_hi = upper_32_bits(dst);
	dma_desc.dst_lo = lower_32_bits(dst);

	/* [한국어] 임시 변수의 내용을 H/W가 보는 DMA 메모리 위치(=qbase_addr[idx])에 일괄 기록.
	 *  hugepage 위 메모리이므로 PA가 안정적이고 IOMMU 매핑도 유지된다. */
	ae4dma->cmd_q[hwq].qbase_addr[desc_index] = dma_desc;
	/* [한국어] in-flight 카운터 +1 — full 검사와 process_events 진입 시 종료 판정에 사용. */
	ae4dma->cmd_q[hwq].ring_buff_count++;

	/* [한국어] 다음 슬롯으로 wrap-around — 32 슬롯 환형 큐. */
	desc_index = (desc_index + 1) % (AE4DMA_DESCRIPTORS_PER_CMDQ);
	ae4dma->cmd_q[hwq].write_index = desc_index;

	/* [한국어] 호스트 그림자 디스크립터 포인터 반환 — 호출자가 콜백 정보를 매다는 데 사용. */
	return desc;
}


/*
 * [한국어]
 * spdk_ae4dma_build_copy - 사용자 (src iovec, dst iovec) 페어를 받아 H/W 디스크립터
 *                          링에 다중 디스크립터로 분해 적재.
 *
 * @ae4dma: 채널.
 * @hwq_id: 사용할 H/W 큐 인덱스(0..15). 어플리케이션이 워크로드 분산을 위해 직접 선택.
 * @cb_arg: 마지막 디스크립터에 매달릴 사용자 콜백 컨텍스트.
 * @cb_fn: 마지막 디스크립터 완료 시 호출될 사용자 콜백.
 * @diov, @diovcnt: 목적지 iovec 배열.
 * @siov, @siovcnt: 소스 iovec 배열.
 * @return: 0 성공 / 1 큐 가득참 / -EINVAL / -EFAULT(vtophys 실패) / -ENOMEM(슬롯 부족).
 *
 * 동작:
 *  1) `spdk_ioviter`로 src/dst iovec을 동시에 순회하며 매칭되는 (src,dst,len) 산출.
 *  2) 한 매칭 segment를 vtophys로 PA 변환. PA 페이지 경계로 segment가 더 잘게 쪼개질 수 있어
 *     루프 안에서 src_len/dst_len의 spdk_min을 한 디스크립터의 길이로 사용.
 *  3) ring 여유가 부족하면 즉시 1을 반환(부분 적재 후 호출자가 flush+process로 비울 수 있음).
 *  4) 모든 segment를 prep_copy로 적재하면서 마지막에 사용자 콜백을 단다.
 *  5) MMIO 트리거(flush)는 호출자가 별도로 spdk_ae4dma_flush를 불러야 한다.
 *
 * 호출 컨텍스트: 사용자 reactor 스레드. 단일 채널/큐는 단일 스레드에서만 사용 권장.
 *
 * 호출 체인: 사용자 → [spdk_ae4dma_build_copy] → spdk_ioviter / spdk_vtophys / ae4dma_prep_copy
 */
int
spdk_ae4dma_build_copy(struct spdk_ae4dma_chan *ae4dma, int hwq_id, void *cb_arg,
		       spdk_ae4dma_req_cb cb_fn,
		       struct iovec *diov, uint32_t diovcnt,
		       struct iovec *siov, uint32_t siovcnt)
{
	struct ae4dma_descriptor        *cb_desc = NULL;
	/* [한국어] 가장 최근에 적재된 그림자 디스크립터 — 마지막 segment에 콜백 부착 시 사용. */
	struct ae4dma_descriptor        *last_desc = NULL;
	/* [한국어] "디스크립터를 한 개라도 적재했는가"를 가리키는 표식.
	 *  build_copy 호출이 모두 zero-length였다면 NULL로 남아 콜백 부착을 건너뜀. */
	struct spdk_ioviter iter;          /* [한국어] iovec 동시 순회 상태. */
	uint64_t        pdst_addr, psrc_addr;
	/* [한국어] vtophys로 변환한 src/dst 물리 주소(64비트). */
	void *src, *dst;
	/* [한국어] 현재 segment의 가상 주소(가변). 한 segment 내에서도 PA 페이지 경계에 따라 분할. */
	uint64_t len, seg_len;
	/* [한국어] iovec 매칭 길이(len)와 페이지 경계 분할 후의 실제 디스크립터 길이(seg_len). */

	/* [한국어] 입력 sanity 검사. NULL 채널/iov 포인터는 즉시 거부. */
	if (!ae4dma || !diov || !siov) {
		return -EINVAL;
	}

	/* [한국어] siov ↔ diov를 매칭 길이로 동시 순회.
	 *  spdk_ioviter는 src/dst iovec의 길이가 다른 경우 더 짧은 쪽을 기준으로 잘라 매칭한다. */
	for (len = spdk_ioviter_first(&iter, siov, siovcnt, diov, diovcnt, &src, &dst);
	     len > 0;
	     len = spdk_ioviter_next(&iter, &src, &dst)) {

		uint64_t remain = len;
		/* [한국어] 현재 매칭 segment에서 아직 적재하지 못한 바이트 수. */
		while (remain > 0) {
			uint64_t src_len = remain;
			uint64_t dst_len = remain;
			/* [한국어] vtophys 호출 시 in-out 파라미터 — "여기까지는 연속 PA 보장"을 알려준다. */

			/* [한국어] 가상→물리 주소 변환. 동일 가상 주소라도 페이지 경계에서 PA가 끊길 수 있어
			 *  src_len/dst_len이 호출 전 remain보다 작게 줄어들 수 있다. */
			psrc_addr = spdk_vtophys(src, &src_len);
			pdst_addr = spdk_vtophys(dst, &dst_len);

			if (psrc_addr == SPDK_VTOPHYS_ERROR || pdst_addr == SPDK_VTOPHYS_ERROR) {
				/* [한국어] hugepage가 아닌 메모리이거나 매핑 누락 — DMA 불가. */
				SPDK_ERRLOG("Error: vtophys translation failed\n");
				return -EFAULT;
			}

			/* [한국어] src/dst 모두 연속 PA 길이를 만족하는 만큼만 한 디스크립터로 적재. */
			seg_len = spdk_min(src_len, dst_len);
			if (seg_len == 0) {
				/* [한국어] 정상적으로 발생하지 않아야 함(가상 주소가 곧바로 페이지 경계인 등 비정상). */
				SPDK_ERRLOG("Zero segment length during iov copy\n");
				return -EINVAL;
			}

			/* [한국어] 큐 여유 검사 — full(여유<4)이면 호출자에게 1 반환(소프트 백프레셔). */
			if (ae4dma->cmd_q[hwq_id].ring_buff_count >= (AE4DMA_DESCRIPTORS_PER_CMDQ - 4)) {

				SPDK_ERRLOG("Descriptor ring is full\n");
				return 1;
			}

			/* [한국어] 한 디스크립터를 적재. cb_desc는 호스트 그림자 슬롯의 포인터. */
			cb_desc = ae4dma_prep_copy(ae4dma, pdst_addr, psrc_addr, seg_len, hwq_id);
			if (!cb_desc) {
				/* [한국어] prep_copy가 NULL을 돌려준 비정상 — 메모리 손상 가능성. */
				SPDK_ERRLOG("Error: Out of descriptors\n");
				return -ENOMEM;
			}

			/* [한국어] 중간 디스크립터에는 콜백을 달지 않는다(=NULL).
			 *  process_events가 NULL인 경우 호출 스킵. 마지막 디스크립터에서만 사용자 콜백을 단다. */
			cb_desc->callback_fn = NULL;
			cb_desc->callback_arg = NULL;

			/* [한국어] 마지막 디스크립터 추적. 한 build_copy의 끝에서 cb_desc에 cb_fn 부착. */
			last_desc = cb_desc;

			/* [한국어] 가상/실제 진행 — 다음 segment를 위해 src/dst 포인터를 seg_len만큼 전진. */
			src = (char *)src + seg_len;
			dst = (char *)dst + seg_len;
			remain -= seg_len;
		}
	}
	/* assign user callback to final segment of iov batch */
	/* [한국어] 적재한 디스크립터가 한 개라도 있으면 마지막 슬롯에 사용자 콜백을 부착.
	 *  cb_desc는 가장 최근 prep_copy의 반환값이므로 last_desc와 동일하다. */
	if (last_desc) {
		cb_desc->callback_fn = cb_fn;
		cb_desc->callback_arg = cb_arg;
	}

	return 0;
}


/*
 * [한국어]
 * ae4dma_process_channel_events - 한 H/W 큐에 대해 완료된 디스크립터를 폴링으로 회수,
 *                                 콜백 호출, tail 전진.
 *
 * @ae4dma: 채널.
 * @hwq_id: 처리할 큐.
 * @return: 이번 호출에서 처리된 디스크립터 수(>=0).
 *
 * 동작:
 *  - cmd_q->tail부터 ring_buff_count 만큼 순회하며 dw1.status를 본다.
 *  - SUBMITTED 상태이면 즉시 중단(아직 H/W 처리 전 — 후속 디스크립터들은 더더욱 안 끝남).
 *  - COMPLETED면 사용자 콜백 호출 (status=0).
 *  - 그 외(에러)는 SPDK_ERRLOG와 함께 콜백을 err_code로 호출.
 *
 * 동기화: H/W가 dw1.status를 비동기로 갱신하므로 hw_desc는 volatile 포인터로 읽는다.
 *  같은 큐를 여러 스레드에서 동시에 폴링하면 안 된다(드라이버는 큐별 단일 스레드 가정).
 *
 * 호출 체인: spdk_ae4dma_process_events → [ae4dma_process_channel_events]
 */
static int
ae4dma_process_channel_events(struct spdk_ae4dma_chan *ae4dma, int hwq_id)
{
	volatile struct spdk_ae4dma_desc *hw_desc;
	/* [한국어] H/W가 갱신하는 디스크립터 영역을 가리키는 포인터. volatile로 컴파일러가
	 *  status read를 캐시하지 못하게 한다(폴링 루프의 정확성 보장). */
	struct ae4dma_cmd_queue *cmd_q;
	/* [한국어] 대상 큐 메타에 대한 alias. */
	uint32_t events_count = 0;
	/* [한국어] 이번 호출에서 처리한 디스크립터 수. 호출자에게 반환. */
	volatile uint32_t tail;
	/* [한국어] 큐의 소비자 인덱스(로컬 사본). volatile은 보수적 안전장치. */
	volatile uint32_t desc_status, desc_err_code;;
	/* [한국어] 디스크립터에서 읽은 상태/에러 코드의 임시 저장. */
	uint64_t sub_desc_cnt;
	/* [한국어] 처리할 in-flight 디스크립터 수의 스냅샷. 루프에서 감소시키며 종료 조건. */

	cmd_q = &ae4dma->cmd_q[hwq_id];

	/* [한국어] 이번 폴링 사이클 시작 시점의 tail. 처리 후 한꺼번에 cmd_q->tail에 반영. */
	tail = cmd_q->tail;

	/* To process all the submitted descriptors for the HW queue */

	/* [한국어] 처리해야 할 디스크립터 수의 시작 스냅샷.
	 *  중간에 ring_buff_count는 -- 되지만 sub_desc_cnt는 별도 카운터. */
	sub_desc_cnt = cmd_q->ring_buff_count;
	while (sub_desc_cnt) {
		desc_status = 0;
		desc_err_code = 0;
		/* [한국어] 다음 처리 슬롯의 H/W 디스크립터에 대한 포인터. */
		hw_desc = &cmd_q->qbase_addr[tail];

		/* [한국어] H/W가 갱신했는지 여부를 보기 위한 status 읽기. volatile 보장. */
		desc_status = hw_desc->dw1.status;

		if (desc_status == AE4DMA_DMA_DESC_SUBMITTED) {
			/* [한국어] 아직 미처리 — 큐는 in-order 가정이므로 뒤 디스크립터들도 다 미처리.
			 *  더 이상 폴링할 의미 없으므로 루프 종료. */
			break;
		}

		if (desc_status != AE4DMA_DMA_DESC_COMPLETED) {
			/* [한국어] 정상 완료가 아닌 상태(=에러 또는 중간 단계가 정체) — 에러 코드 저장 후 로그. */
			desc_err_code = hw_desc->dw1.err_code;
			SPDK_ERRLOG("Desc error code : %d\n", hw_desc->dw1.err_code);
		}

		/* [한국어] in-flight 카운터 일관성 점검 후 -1. 음수가 되면 큐 관리 버그. */
		assert(cmd_q->ring_buff_count > 0);
		cmd_q->ring_buff_count--;

		/* [한국어] 사용자 콜백이 매달려 있는 슬롯이면 호출(=한 build_copy의 마지막 디스크립터).
		 *  중간 디스크립터들은 콜백 NULL이라 호출 생략. */
		if (cmd_q->ring[tail].callback_fn) {
			cmd_q->ring[tail].callback_fn(cmd_q->ring[tail].callback_arg, desc_err_code);
		}

		events_count++;                                      /* [한국어] 처리 카운트 +1. */
		tail = (tail + 1) % AE4DMA_DESCRIPTORS_PER_CMDQ;     /* [한국어] 다음 슬롯으로 wrap-around. */
		sub_desc_cnt--;                                      /* [한국어] 처리할 슬롯 수 감소. */
	}
	/* [한국어] 누적된 tail을 한 번에 갱신. 다음 폴링 사이클의 시작점이 됨. */
	cmd_q->tail = tail;

	return events_count;
}

/*
 * [한국어]
 * ae4dma_channel_destruct - 채널 시작 시 할당된 모든 자원(BAR 매핑, 디스크립터 메모리,
 *                            그림자 ring) 해제.
 *
 * @hwqueues: 해제할 큐 수(보통 AE4DMA_MAX_HW_QUEUES=16).
 * @ae4dma: 채널.
 *
 * 호출 체인: ae4dma_attach 실패 경로 / spdk_ae4dma_detach → [ae4dma_channel_destruct]
 */
static void
ae4dma_channel_destruct(uint8_t hwqueues, struct spdk_ae4dma_chan *ae4dma)
{
	int i;

	/* [한국어] BAR 매핑 해제 — io_regs가 NULL이어도 unmap_pci_bar가 안전하게 처리. */
	ae4dma_unmap_pci_bar(ae4dma);

	for (i = 0; i < hwqueues; i++) {
		/* [한국어] DMA 가능 메모리(hugepage)로 받은 디스크립터 영역 해제. spdk_free는 NULL-safe. */
		spdk_free(ae4dma->cmd_q[i].qbase_addr);
		if (ae4dma->cmd_q[i].ring) {
			/* [한국어] 호스트 그림자 ring은 일반 calloc — free로 해제. NULL이면 할당 자체 안 된 상태. */
			free(ae4dma->cmd_q[i].ring);
		}
	}
}


/*
 * [한국어]
 * ae4dma_channel_start - 채널의 모든 자원(BAR 매핑, 디스크립터 메모리, 큐 레지스터) 초기화.
 *
 * @hw_queues: 사용할 큐 수(이 값이 H/W 한도 16을 넘으면 16으로 클램프).
 * @ae4dma: 새로 할당된 채널 객체(device 필드만 채워진 상태).
 * @return: 0 성공 / -1, -ENOMEM, -EFAULT 실패.
 *
 * 동작 단계:
 *  1) 입력 큐 수 sanity check 후 q_per_eng 결정.
 *  2) BAR0 매핑(`ae4dma_map_pci_bar`).
 *  3) capability/transfer size 설정.
 *  4) common config 레지스터에 q_per_eng 기록(=H/W에 활성 큐 수 통보).
 *  5) 각 큐별로:
 *     a) regs 포인터 계산(io_regs 베이스 + (i+1) 슬롯).
 *     b) hugepage 기반 디스크립터 메모리 할당 + vtophys로 PA 획득.
 *     c) max_idx/queue_enable/intr_status 레지스터 초기화.
 *     d) qbase_lo/qbase_hi에 PA 분할 기록(=H/W가 큐 베이스를 알게 됨).
 *     e) 호스트 그림자 ring(callback 보관) calloc.
 *
 * 호출 체인: ae4dma_attach → [ae4dma_channel_start] → spdk_dma_zmalloc/spdk_vtophys/spdk_mmio_write_4
 */
static int
ae4dma_channel_start(uint8_t hw_queues, struct spdk_ae4dma_chan *ae4dma)
{
	uint32_t i;
	void *ae4dma_mmio_base_addr;
	/* [한국어] BAR0의 매핑된 가상 주소를 byte pointer로 다루기 위한 alias. */
	struct ae4dma_cmd_queue *cmd_q;
	uint32_t dma_queue_base_addr_low, dma_queue_base_addr_hi;
	/* [한국어] 디스크립터 베이스 PA를 32비트 두 조각으로 분할해 MMIO에 기록. */
	uint32_t q_per_eng;
	/* [한국어] 이 엔진에서 실제로 활성화할 큐 수. H/W 한도(16)로 캡 처리. */
	uint64_t size;

	/* [한국어] 요청한 큐 수가 한도 이내면 그대로 사용, 초과면 16으로 캡. */
	if (!ae4dma_config_queues_per_device(hw_queues)) {
		q_per_eng = hw_queues;
	} else {
		q_per_eng = AE4DMA_MAX_HW_QUEUES;
	}

	/* [한국어] BAR0 매핑 — 실패 시 즉시 종료(자원 해제는 호출자가 destruct로 일괄 처리). */
	if (ae4dma_map_pci_bar(ae4dma) != 0) {
		SPDK_ERRLOG("ae4dma_map_pci_bar() failed\n");
		return -1;
	}
	/* [한국어] uint8_t* 캐스팅으로 byte 단위 주소 산술이 안전해짐(공통 config 오프셋 가산용). */
	ae4dma_mmio_base_addr = (uint8_t *)ae4dma->io_regs;

	/* Always support DMA copy */
	/* [한국어] 모든 AE4DMA가 기본 지원하는 copy 비트 설정. 미래 확장 시 추가. */
	ae4dma->dma_capabilities = SPDK_AE4DMA_ENGINE_COPY_SUPPORTED;
	/* [한국어] 한 디스크립터의 최대 길이 — 4GiB (uint32 length 필드의 표현 가능 최대치). */
	ae4dma->max_xfer_size = 1ULL << 32;

	/* Set the number of HW queues for this AE4DMA engine. */
	/* [한국어] 공통 설정 레지스터(BAR offset 0)에 활성 큐 수 기록.
	 *  H/W가 이 값을 보고 내부 자원을 그만큼 활성화한다. */
	spdk_mmio_write_4((ae4dma_mmio_base_addr + AE4DMA_COMMON_CONFIG_OFFSET), q_per_eng);
	/* [한국어] 다시 read해 보아 H/W가 실제 인정한 큐 수를 받는다(쓴 값과 다를 수 있음). */
	q_per_eng = spdk_mmio_read_4((ae4dma_mmio_base_addr + AE4DMA_COMMON_CONFIG_OFFSET));

	/* Filling up cmd_q; there would be 'n' cmd_q's for 'n' q_per_eng */
	for (i = 0; i < q_per_eng; i++) {

		/* AE4DMA queue initialization */

		/* Current cmd_q details (total 16) */
		cmd_q = &ae4dma->cmd_q[i];
		/* [한국어] 큐 활성 카운트 +1 — 종료/실패 경로에서 정리할 큐 수의 단서. */
		ae4dma->cmd_q_count++;

		/* Initialize queue's HW registers (8 dwords: 32 bytes(0x20)) */
		/* [한국어] 큐 i의 레지스터 영역은 BAR0의 (i+1)번째 32바이트 슬롯에 위치.
		 *  슬롯 0은 공통 config 영역(common config). 따라서 +1. */
		cmd_q->regs = (volatile struct spdk_ae4dma_hwq_regs *)ae4dma->io_regs + (i + 1);

		/* Queue_size: 32*sizeof(struct ae4dmadma_desc) */
		/* [한국어] 디스크립터 영역의 총 바이트 크기 = 32 슬롯 × 32B = 1024B. */
		cmd_q->queue_size = AE4DMA_QUEUE_SIZE(AE4DMA_QUEUE_DESC_SIZE);
		size = cmd_q->queue_size;

		/* DMA'ble desc address - for each cmd_q  */
		/* [한국어] hugepage 기반 DMA 가능한 메모리 할당. zmalloc은 영역을 0으로 초기화.
		 *  ALIGN_DWORD(=32)로 정렬해 디스크립터 경계와 어긋나지 않게 한다. */
		cmd_q->qbase_addr = spdk_dma_zmalloc(AE4DMA_DESCRIPTORS_PER_CMDQ * sizeof(struct spdk_ae4dma_desc),
						     ALIGN_DWORD, NULL);

		if (cmd_q->qbase_addr == NULL) {
			SPDK_ERRLOG(" Failed to get desc address\n");
			return -ENOMEM;
		}

		/* [한국어] 가상→물리 변환. H/W에 베이스 주소를 알려주려면 PA 필요. */
		cmd_q->qring_buffer_pa = spdk_vtophys(cmd_q->qbase_addr, &size);

		if (cmd_q->qring_buffer_pa == SPDK_VTOPHYS_ERROR) {
			/* [한국어] hugepage가 아닌 메모리이거나 매핑 실패 시 — 비정상. */
			SPDK_ERRLOG("Failed to translate descriptor %u to physical address\n", i);
			return -EFAULT;
		}

		/* Max Index (cmd queue length) */
		/* [한국어] H/W에 큐 길이(=32) 통보. wrap-around 분모로 사용됨. */
		spdk_mmio_write_4(&cmd_q->regs->max_idx, AE4DMA_DESCRIPTORS_PER_CMDQ);

		/* Queue Enable */
		/* [한국어] control_raw에 1을 써서 큐 활성화. 이후 H/W가 디스크립터 fetch 가능 상태. */
		spdk_mmio_write_4(&cmd_q->regs->control_reg.control_raw, AE4DMA_CMD_QUEUE_ENABLE);

		/* Disabling the interrupt */
		/* [한국어] intr_status에 1 W1C — pending 인터럽트 클리어 + 활성화 비트가 있다면 끔.
		 *  SPDK는 polled-mode이므로 인터럽트가 필요 없음. */
		spdk_mmio_write_4(&cmd_q->regs->intr_status_reg.intr_status_raw, 0x1);

		/* [한국어] 호스트 측 write_index를 H/W의 현재 write_idx로 동기화(0 또는 H/W 초기값). */
		cmd_q->write_index = spdk_mmio_read_4(&cmd_q->regs->write_idx);

		/* [한국어] 마찬가지로 tail은 H/W의 read_idx로 초기화. */
		cmd_q->tail = spdk_mmio_read_4(&cmd_q->regs->read_idx);
		cmd_q->ring_buff_count = 0;

		/* Update the device registers with queue addresses. */
		/* [한국어] qdma_tail은 PA 사본 — qbase_lo/hi에 분할 기록할 원본 값. */
		cmd_q->qdma_tail = cmd_q->qring_buffer_pa;

		/* [한국어] 큐 베이스 PA의 하위 32비트 → qbase_lo. */
		dma_queue_base_addr_low = lower_32_bits(cmd_q->qdma_tail);
		spdk_mmio_write_4(&cmd_q->regs->qbase_lo, (uint32_t)dma_queue_base_addr_low);

		/* [한국어] 상위 32비트 → qbase_hi. 이 두 레지스터를 모두 쓴 후에야 H/W가 디스크립터 영역 위치를 알 수 있음. */
		dma_queue_base_addr_hi = upper_32_bits(cmd_q->qdma_tail);
		spdk_mmio_write_4(&cmd_q->regs->qbase_hi, (uint32_t)dma_queue_base_addr_hi);

		/* [한국어] 호스트 그림자 ring 할당 — 콜백 정보 32 슬롯 분량. */
		cmd_q->ring = calloc(AE4DMA_DESCRIPTORS_PER_CMDQ, sizeof(struct ae4dma_descriptor));
		if (!cmd_q->ring) {
			return -ENOMEM;
		}
	}

	if (ae4dma->cmd_q_count == 0) {
		/* [한국어] 한 개의 큐도 활성화되지 않은 비정상 상태 — H/W 사용 불가. */
		SPDK_ERRLOG("Error in enabling HW queues.No HW queues available\n");
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * ae4dma_attach - 발견된 PCI 디바이스에 대해 채널 객체를 만들고 초기화.
 *
 * @hw_queues: 활성화 요청 큐 수.
 * @device: SPDK PCI 추상화로 받은 디바이스 핸들.
 * @return: 새 채널 / NULL 실패.
 *
 * 동작:
 *  1) 채널 객체 calloc.
 *  2) PCI configuration space에서 Command 레지스터(offset 4)를 읽어 BusMaster 비트(0x4) 켜기.
 *     이게 빠지면 PCIe DMA 자체가 동작 불가.
 *  3) channel_start 호출.
 *
 * 호출 체인: ae4dma_enum_cb → [ae4dma_attach] → ae4dma_channel_start
 */
static struct spdk_ae4dma_chan *
ae4dma_attach(uint8_t hw_queues, struct spdk_pci_device *device)
{
	struct spdk_ae4dma_chan *ae4dma;
	uint32_t cmd_reg;

	/* [한국어] 채널 객체 영확장 zero-init. 실패 시 NULL 반환. */
	ae4dma = calloc(1, sizeof(struct spdk_ae4dma_chan));
	if (ae4dma == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for ae4dma device.\n");
		return NULL;
	}

	/* Enable PCI busmaster. */
	/* [한국어] PCI Configuration Header offset 4 — Command 레지스터.
	 *  비트 2(0x4) = Bus Master Enable. 1로 set해야 PCIe DMA 마스터로 동작. */
	spdk_pci_device_cfg_read32(device, &cmd_reg, 4);
	cmd_reg |= 0x4;
	spdk_pci_device_cfg_write32(device, cmd_reg, 4);

	/* [한국어] 채널 객체에 PCI 핸들 저장 — 이후 BAR map/unmap, cfg 액세스 인자로 사용. */
	ae4dma->device = device;

	/* [한국어] 큐 자원 초기화. 실패 시 destruct로 부분 자원 정리 후 free. */
	if (ae4dma_channel_start(hw_queues, ae4dma) != 0) {
		ae4dma_channel_destruct(hw_queues, ae4dma);
		free(ae4dma);
		return NULL;
	}

	return ae4dma;
}

/*
 * [한국어] struct ae4dma_enum_ctx — PCI 열거 콜백에 전달되는 컨텍스트 묶음.
 *  spdk_pci_enumerate는 콜백에 단일 void* ctx만 전달하므로 multiple 사용자 콜백을
 *  하나의 구조체로 묶어 넘긴다.
 */
struct ae4dma_enum_ctx {
	spdk_ae4dma_probe_cb probe_cb;
	/* [한국어] 사용자 정의 probe 필터. true 반환 시 attach 진행. 설정자: spdk_ae4dma_probe. */
	spdk_ae4dma_attach_cb attach_cb;
	/* [한국어] attach 성공 시 사용자에게 새 채널을 알리기 위한 콜백. */
	void *cb_ctx;
	/* [한국어] 사용자 컨텍스트(사용자가 spdk_ae4dma_probe에 넘긴 cb_ctx). */
};

/*
 * [한국어]
 * ae4dma_enum_cb - spdk_pci_enumerate가 발견한 디바이스마다 호출되는 콜백.
 *                  중복 attach 방지, probe 필터, attach, 사용자 콜백 호출 순으로 처리.
 *
 * @ctx: ae4dma_enum_ctx (probe/attach 콜백과 사용자 ctx 묶음).
 * @pci_dev: 발견된 PCI 디바이스 핸들.
 * @return: 0 성공/스킵, -1 attach 실패.
 *
 * 호출 체인: spdk_ae4dma_probe → spdk_pci_enumerate → [ae4dma_enum_cb] → ae4dma_attach
 */
static int
ae4dma_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct ae4dma_enum_ctx *enum_ctx = ctx;
	struct spdk_ae4dma_chan *ae4dma;

	/* Verify that this device is not already attached */
	/* [한국어] 동일 PCI 디바이스가 이미 attached_chans에 있으면 중복 attach 방지(0 반환). */
	TAILQ_FOREACH(ae4dma, &g_ae4dma_driver.attached_chans, tailq) {
		/*
		 * NOTE: This assumes that the PCI abstraction layer will use the same device handle
		 *  across enumerations; we could compare by BDF instead if this is not true.
		 */
		if (pci_dev == ae4dma->device) {
			return 0;
		}
	}

	/* [한국어] 사용자 probe 필터 — true이면 attach 진행, false면 skip. */
	if (enum_ctx->probe_cb(enum_ctx->cb_ctx, pci_dev)) {
		/*
		 * Since AE4DMA init is relatively quick, just perform the full init during probing.
		 *  If this turns out to be a bottleneck later, this can be changed to work like
		 *  NVMe with a list of devices to initialize in parallel.
		 */
		/* [한국어] 모든 큐(16)를 활성화한 채로 attach. NVMe와 달리 작은 셋업이라 직렬 실행. */
		ae4dma = ae4dma_attach(AE4DMA_MAX_HW_QUEUES, pci_dev);
		if (ae4dma == NULL) {
			SPDK_ERRLOG("ae4dma_attach() failed\n");
			return -1;
		}

		/* [한국어] 글로벌 채널 리스트에 등록 — 이후 detach까지 유지. */
		TAILQ_INSERT_TAIL(&g_ae4dma_driver.attached_chans, ae4dma, tailq);

		/* [한국어] 사용자에게 새 채널을 알림(=사용자가 자기 데이터구조에 보관). */
		enum_ctx->attach_cb(enum_ctx->cb_ctx, pci_dev, ae4dma);
	}

	return 0;
}

/*
 * [한국어]
 * spdk_ae4dma_probe - 시스템의 모든 AE4DMA PCI 디바이스를 열거하고 사용자가 원하는 것을 attach.
 *
 * @cb_ctx: 사용자 컨텍스트(probe_cb/attach_cb에 그대로 전달).
 * @probe_cb: 각 디바이스에 대해 attach 여부를 결정하는 사용자 콜백.
 * @attach_cb: attach 성공 시 호출되는 콜백.
 * @return: spdk_pci_enumerate 결과 (0 성공 / 음수 실패).
 *
 * 동기화: g_ae4dma_driver.lock으로 enumeration 전체를 직렬화 — 동시에 두 호출자가
 *  attach를 시도해도 attached_chans 리스트가 깨지지 않게 한다.
 */
int
spdk_ae4dma_probe(void *cb_ctx, spdk_ae4dma_probe_cb probe_cb, spdk_ae4dma_attach_cb attach_cb)
{
	int rc;
	struct ae4dma_enum_ctx enum_ctx;

	/* [한국어] 글로벌 lock — enumerate가 끝날 때까지 다른 attach/detach를 직렬화. */
	pthread_mutex_lock(&g_ae4dma_driver.lock);

	enum_ctx.probe_cb = probe_cb;
	enum_ctx.attach_cb = attach_cb;
	enum_ctx.cb_ctx = cb_ctx;

	/* [한국어] AE4DMA 전용 PCI driver descriptor를 받아 spdk_pci_enumerate에 전달.
	 *  enumerate는 vid/pid 매칭되는 디바이스를 찾아 콜백 호출. */
	rc = spdk_pci_enumerate(spdk_pci_ae4dma_get_driver(), ae4dma_enum_cb, &enum_ctx);

	pthread_mutex_unlock(&g_ae4dma_driver.lock);

	return rc;
}

/*
 * [한국어]
 * spdk_ae4dma_detach - 사용을 마친 채널을 글로벌 리스트에서 빼고 자원 해제.
 *
 * @ae4dma: 해제 대상 채널.
 *
 * 사전 조건: 이 채널을 더 이상 어떤 스레드도 폴링/적재하지 않아야 함(상위 책임).
 */
void
spdk_ae4dma_detach(struct spdk_ae4dma_chan *ae4dma)
{
	struct ae4dma_driver *driver = &g_ae4dma_driver;

	/* ae4dma should be in the free list (not registered to a thread)
	 * when calling ae4dma_detach().
	 */
	/* [한국어] 글로벌 lock으로 리스트에서 안전하게 제거. */
	pthread_mutex_lock(&driver->lock);
	TAILQ_REMOVE(&driver->attached_chans, ae4dma, tailq);
	pthread_mutex_unlock(&driver->lock);

	/* [한국어] 큐별 자원 해제 후 채널 객체 free. lock 밖에서 수행해도 안전(이미 리스트 분리). */
	ae4dma_channel_destruct(AE4DMA_MAX_HW_QUEUES, ae4dma);
	free(ae4dma);
}

/*
 * [한국어]
 * spdk_ae4dma_process_events - 사용자가 호출하는 polling 진입점. 한 큐의 완료 디스크립터를 회수.
 *
 * @ae4dma: 채널.
 * @hwq_id: 폴링할 큐.
 * @return: 처리한 디스크립터 수.
 *
 * SPDK reactor의 poller 콜백에서 주기적으로 호출되는 것이 일반적인 사용 패턴.
 * (예: SPDK_POLLER_REGISTER로 등록한 콜백 안에서 호출)
 *
 * 호출 체인: 사용자 poller → [spdk_ae4dma_process_events] → ae4dma_process_channel_events
 */
int
spdk_ae4dma_process_events(struct spdk_ae4dma_chan *ae4dma, int hwq_id)
{
	return ae4dma_process_channel_events(ae4dma, hwq_id);
}
