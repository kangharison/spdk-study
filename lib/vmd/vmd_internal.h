/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Intel VMD(Volume Management Device) 내부 헤더 (vmd_internal.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK가 Intel VMD를 통해 PCIe NVMe 디바이스를 enumerate/관리할 때 사용하는 내부
 * 자료구조를 정의한다. VMD는 Xeon CPU에 통합된 *PCIe 스위치 + endpoint hub*로, 일반적인
 * PCIe root complex와 달리 자기 BAR(membar/cfgbar) 안에 하위 PCIe 도메인을 노출한다.
 * SPDK는 VMD endpoint를 직접 사용자 공간에서 enumerate하여 NVMe 핫플러그/핫리무브 이벤트와
 * 슬롯 LED(IDENTIFY/FAULT/REBUILD)를 제어한다. 이 파일은 VMD 어댑터/버스/디바이스/핫플러그 슬롯
 * 등 enumerate 트리를 표현하는 자료구조의 정의와 vmd_find_device 헬퍼 선언을 포함한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   spdk_vmd_init()
 *     → vmd_enumerate_devices() (vmd.c)
 *         · cfgbar/membar mmap → 가상 root bus 생성(vmd_pci_bus)
 *         · BFS로 PCIe 트리 탐색하며 vmd_pci_device 생성/연결
 *         · 각 디바이스의 BAR 할당, 핫플러그 슬롯 초기화(vmd_hot_plug)
 *     → SPDK NVMe 드라이버는 일반 spdk_pci_device API를 통해 VMD 뒤 NVMe 사용
 *     → led.c: spdk_vmd_set/get_led_state로 슬롯 LED 제어 (이 헤더의 vmd_pci_device 사용)
 * 실행 컨텍스트: 호스트 유저스페이스. enumerate는 init 단계 1회, hotplug 처리는 백그라운드 polling.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/vmd.h        : 공개 API (spdk_vmd_init, spdk_vmd_set_led_state 등).
 * - spdk/env.h        : DPDK 추상화 - PCI BAR mmap, hugepage.
 * - spdk/util.h       : 유틸 (TAILQ 등).
 * - spdk/log.h        : 로깅.
 * - vmd_spec.h        : VMD 하드웨어 스펙 - PCI capability, slot control/status 비트 정의.
 * - lib/nvme         : VMD 뒤의 NVMe 디바이스를 일반 spdk_pci_device로 사용.
 * 데이터 흐름:
 *   사용자 (RPC: vmd_set_led_state) → led.c → vmd_pci_device.pcie_cap.slot_control → MMIO write → SES LED
 *   PCIe hotplug interrupt(MSI-X) → vmd.c poller → vmd_hot_plug 큐에서 이벤트 처리 → 새 디바이스 attach
 *
 * === 주요 함수/구조체 요약 ===
 * - struct vmd_adapter      : VMD 어댑터 1개(=Xeon CPU의 VMD 컨트롤러 1개) 컨텍스트.
 * - struct vmd_pci_bus      : VMD 도메인 내부의 PCIe 버스 1개.
 * - struct vmd_pci_device   : VMD 도메인 내부의 PCI 디바이스(브릿지/엔드포인트) 1개.
 * - struct vmd_hot_plug     : 핫플러그 가능 슬롯의 메타데이터(BAR, bus 번호 풀, mem 풀).
 * - struct pci_mem_mgr      : 핫플러그 시 BAR 재할당용 메모리 풀 엔트리.
 * - struct pci_bars         : PCI BAR 1개의 (vaddr, phys start, size) 트리플.
 * - vmd_find_device(addr)   : BDF로 vmd_pci_device 찾기 (led.c가 사용).
 */

#ifndef VMD_H
#define VMD_H

/* [한국어] SPDK 표준 C 라이브러리 - stdint/stdbool/string/unistd 등 묶음. */
#include "spdk/stdinc.h"
/* [한국어] VMD 공개 API - spdk_vmd_init, spdk_vmd_led_state enum 등. */
#include "spdk/vmd.h"
/* [한국어] DPDK 환경 추상화 - spdk_pci_device, BAR mmap, hugepage. */
#include "spdk/env.h"
/* [한국어] 유틸리티 (TAILQ 매크로, SPDK_CONTAINEROF 등). */
#include "spdk/util.h"
/* [한국어] 로깅 매크로(SPDK_ERRLOG 등). */
#include "spdk/log.h"
/* [한국어] VMD 하드웨어 스펙 - pci_header, pci_express_cap, slot_control/slot_status 비트필드 등. */
#include "vmd_spec.h"

/* [한국어] 전방 선언들 - 상호 참조하는 구조체들이라 forward declaration 필요. */
struct vmd_hot_plug;     /* 핫플러그 슬롯 메타. */
struct vmd_adapter;      /* VMD 어댑터(컨트롤러) 자체. */
struct vmd_pci_device;   /* VMD 내부 PCI 디바이스. */

/* [한국어] PCI BAR 1개의 가상/물리 주소와 크기를 묶은 트리플.
 * 디바이스마다 BAR을 6개 가질 수 있어 vmd_pci_device.bar[6]로 사용. */
struct pci_bars {
	uint64_t vaddr;
	/* [한국어] BAR을 mmap한 가상 주소 (호스트가 MMIO read/write에 사용).
	 * 설정자: vmd.c가 vmd_adapter.mem_vaddr + offset으로 계산.
	 * 읽는 자: 디바이스별 레지스터 접근 헬퍼.
	 * 값 범위: VMD membar 영역 내부의 가상주소. 0이면 미할당.
	 * 동기화: enumerate 단계 1회 set 후 read-only. */

	uint64_t start;
	/* [한국어] BAR의 물리(IOVA) 시작 주소 - PCI 디바이스가 보는 BAR.
	 * 설정자: vmd_assign_base_addrs()가 membar 풀에서 할당.
	 * 읽는 자: BAR 레지스터에 write할 값 계산.
	 * 값 범위: VMD membar 영역 내부의 물리주소.
	 * 동기화: enumerate 후 read-only. */

	uint32_t size;
	/* [한국어] BAR 크기(바이트) - PCI 디바이스가 BAR sizing으로 보고한 값.
	 * 설정자: vmd_get_pci_bar_size()가 BAR에 0xFFFFFFFF를 쓰고 다시 읽어 계산.
	 * 읽는 자: 메모리 매핑 길이 결정.
	 * 값 범위: 2^N (PCI 스펙). */
};

/* [한국어] VMD 도메인 내부의 PCIe 버스 1개를 표현하는 구조체.
 * 일반 PCI bus와 달리 VMD 어댑터의 cfg/mem 영역에서 직접 enumerate한 가상 트리이다. */
struct vmd_pci_bus {
	struct vmd_adapter *vmd;
	/* [한국어] 이 버스가 속한 VMD 어댑터(=root) 역참조.
	 * 설정자: vmd_create_new_bus()가 set.
	 * 읽는 자: 모든 enumerate 코드가 cfgbar/membar 접근 시 사용.
	 * 값 범위: 유효한 vmd_adapter 포인터.
	 * 동기화: 초기화 후 read-only. */

	struct vmd_pci_bus *parent;	/* parent bus that this bus is attached to(primary bus. */
	/* [한국어] 부모 버스(primary bus) - 트리 루트 방향.
	 * 설정자: BFS 탐색 시 부모 버스를 set.
	 * 읽는 자: BAR 할당 시 부모 도메인 정보 참조.
	 * 값 범위: 루트면 vmd->vmd_bus 자체, 아니면 상위 버스. */

	struct vmd_pci_device *self;		/* Pci device that describes this bus(bar, bus numbers, etc */
	/* [한국어] 이 버스를 노출하는 type-1 PCI 디바이스(=PCI-PCI 브릿지) 핸들.
	 * 설정자: vmd_create_new_bus()가 부모 브릿지 디바이스를 set.
	 * 읽는 자: secondary/subordinate bus 설정 시 self->header 사용.
	 * 값 범위: 루트 버스는 NULL, 그 외는 type-1 디바이스. */

	uint32_t  domain           : 8;
	/* [한국어] PCI 도메인 번호 - VMD 어댑터별로 고유한 식별자.
	 * 설정자: vmd_adapter.domain 값을 그대로 복사.
	 * 값 범위: 0..255. */

	uint32_t  hotplug_buses    : 10;
	/* [한국어] 이 버스에 핫플러그 reserved된 bus 번호 개수.
	 * 설정자: enumerate 시 핫플러그 정책에 따라 set.
	 * 값 범위: 0..1023. */

	uint32_t  is_added         : 1;
	/* [한국어] 이 버스가 SPDK PCI 시스템에 정식 등록되었는지.
	 * 설정자: 등록 완료 시 1. 값 범위: 0/1. */

	uint32_t  hp_event_queued  : 1;
	/* [한국어] 이 버스에서 핫플러그 이벤트가 hp_queue에 enqueued 되어 처리 대기 중인지.
	 * 설정자: 인터럽트/polling 핸들러가 set, 처리 완료 시 clear.
	 * 동기화: hp_queue lock 또는 동일 polling 스레드 단독 접근 가정. */

	uint32_t  rsv              : 12;
	/* [한국어] 비트필드 패딩 (32비트 정렬용). 사용 안 함. */

	uint32_t  bus_number      : 8;
	/* [한국어] 이 버스의 PCI bus number. */

	uint32_t  primary_bus     : 8;
	/* [한국어] type-1 브릿지의 primary bus number 필드 - 부모 버스 번호. */

	uint32_t  secondary_bus   : 8;
	/* [한국어] 브릿지의 secondary bus number - 이 버스 자기 번호. */

	uint32_t  subordinate_bus : 8;
	/* [한국어] 브릿지가 관리하는 가장 아래쪽 bus 번호 - 이 브릿지 하위의 모든 버스를 포함. */

	uint32_t  bus_start       : 8;
	/* [한국어] 핫플러그 reserved 영역의 시작 bus 번호. */

	uint32_t  config_bus_number : 8;
	/* [한국어] 이 버스의 config 공간 접근에 사용하는 bus 번호 - VMD config window에서 인덱싱. */

	TAILQ_HEAD(, vmd_pci_device) dev_list;	/* list of pci end device attached to this bus */
	/* [한국어] 이 버스에 직접 연결된 PCI 디바이스(브릿지/엔드포인트) 리스트.
	 * 설정자: vmd_dev_init()이 INSERT_TAIL.
	 * 읽는 자: enumerate/teardown/find_device 등 순회 코드.
	 * 동기화: enumerate 단계 후 read-only. hotplug 시 추가/삭제는 polling 스레드 단독. */

	TAILQ_ENTRY(vmd_pci_bus) tailq;		/* link for all buses found during scan */
	/* [한국어] vmd_adapter.bus_list 글로벌 리스트에서의 노드.
	 * 설정자: scan 단계가 INSERT_TAIL.
	 * 읽는 자: 전체 버스 순회 시 사용. */
};

/*
 * memory element for base address assignment and reuse
 */
/* [한국어] 핫플러그 슬롯 BAR 재할당용 메모리 블록 단위.
 * 핫플러그가 발생하면 새 디바이스의 BAR을 free_mem_queue에서 꺼내 할당하고, 제거 시 반환한다.
 * size 합 == hot_plug.bar.size이며 슬롯 BAR을 작은 청크로 나눠 동적 분배한다. */
struct pci_mem_mgr {
	uint32_t			size : 30;        /* size of memory element */
	/* [한국어] 메모리 청크 크기(바이트, 30비트 = 최대 1 GiB).
	 * 설정자: vmd_hp_bar_init()이 슬롯 BAR을 분할 시 set.
	 * 읽는 자: 디바이스 BAR 할당 시 사용. */

	uint32_t			in_use : 1;
	/* [한국어] 이 청크가 현재 사용 중인지(=alloc_mem_queue 소속) 또는 free 상태인지.
	 * 설정자: 할당/해제 시 1/0. 디버깅 가시성 보조 플래그. */

	uint32_t			rsv : 1;
	/* [한국어] 비트필드 패딩 - 32비트 정렬용. */

	uint64_t			addr;
	/* [한국어] 청크의 시작 물리 주소(IOVA).
	 * 설정자: 청크 분할 시 set, 이후 변경 없음.
	 * 읽는 자: BAR 레지스터에 write할 값 계산. */

	TAILQ_ENTRY(pci_mem_mgr)	tailq;
	/* [한국어] free_mem_queue/alloc_mem_queue/unused_mem_queue 중 하나의 노드.
	 * 설정자: 이동 시 TAILQ_REMOVE/INSERT.
	 * 읽는 자: 할당 정책 결정 시 순회. */
};

/* [한국어] 핫플러그 가능한 PCIe 슬롯 1개의 메타데이터 - 슬롯 BAR과 bus number 풀을 관리.
 * VMD는 enumerate 시 핫플러그 슬롯에 미리 BAR/bus 번호를 reserved해 두었다가, 디바이스 삽입 시 즉시 부여한다. */
struct vmd_hot_plug {
	uint32_t count  : 12;
	/* [한국어] 현재 슬롯에 attached된 디바이스 카운트. */

	uint32_t reserved_bus_count : 4;
	/* [한국어] 이 슬롯에 사전 예약된 bus 번호 개수. */

	uint32_t max_hotplug_bus_number : 8;
	/* [한국어] 이 슬롯이 사용 가능한 최대 bus 번호 - bus 번호 wrap 검사용. */

	uint32_t next_bus_number : 8;
	/* [한국어] 다음 hotplug 디바이스에 할당할 bus 번호 (round-robin 또는 sequential). */

	struct pci_bars bar;
	/* [한국어] 이 슬롯에 reserved된 메모리 BAR 영역 - 슬롯 안의 모든 디바이스가 공유.
	 * 설정자: enumerate 단계가 슬롯 capability 기반으로 할당. */

	union express_slot_status_register slot_status;
	/* [한국어] PCIe slot status 레지스터의 캐시(읽은 값).
	 * 설정자: hotplug 이벤트 처리 시 갱신.
	 * 읽는 자: presence detect, attention button 등 이벤트 분류. */

	struct pci_mem_mgr mem[ADDR_ELEM_COUNT];
	/* [한국어] 슬롯 BAR을 ADDR_ELEM_COUNT 청크로 미리 분할한 정적 풀 - 핫플러그 시 dynamic 할당.
	 * 설정자: vmd_hp_bar_init()이 분할.
	 * 읽는 자: free_mem_queue 등으로 가져가서 사용. */

	uint8_t bus_numbers[RESERVED_HOTPLUG_BUSES];
	/* [한국어] 슬롯에 reserved된 bus 번호 풀 - 디바이스 삽입 시 1개씩 사용.
	 * 설정자: enumerate 시 채움. */

	struct vmd_pci_bus *bus;
	/* [한국어] 이 슬롯이 매달린 PCIe 버스 역참조 - 슬롯 컨트롤러 디바이스의 bus 객체. */

	TAILQ_HEAD(, pci_mem_mgr) free_mem_queue;
	/* [한국어] free 상태 청크 큐 - 핫플러그 디바이스의 BAR 할당 시 여기서 가져감. */

	TAILQ_HEAD(, pci_mem_mgr) alloc_mem_queue;
	/* [한국어] 현재 사용 중인 청크 큐 - 디바이스 제거 시 여기서 free_mem_queue로 이동. */

	TAILQ_HEAD(, pci_mem_mgr) unused_mem_queue;
	/* [한국어] 사용한 적 없는 청크 큐 - free와 분리하여 fragment 감소 정책에 사용. */
};

/* [한국어] VMD 도메인 내부의 PCI 디바이스(브릿지 type-1 또는 엔드포인트 type-0) 핸들.
 * 일반 SPDK spdk_pci_device를 첫 멤버로 임베드하여, NVMe 드라이버 등은 일반 PCI 인터페이스로 접근 가능. */
struct vmd_pci_device {
	struct spdk_pci_device pci;
	/* [한국어] SPDK 공통 PCI 디바이스 추상화 - addr(BDF), config space 콜백 등 포함.
	 * NVMe 드라이버는 이 부분만 보고 디바이스를 다룬다.
	 * 설정자: vmd_dev_init()이 BDF/콜백 채움.
	 * 읽는 자: 외부 모듈 (spdk_pci_*). */

	struct pci_bars bar[6];
	/* [한국어] PCI 표준 BAR 6개. type-0는 0..5, type-1은 0..1 사용.
	 * 설정자: vmd_assign_base_addrs()가 enumerate 시 채움.
	 * 읽는 자: 디바이스 드라이버가 BAR mmap 결과 사용. */

	struct vmd_pci_device *parent_bridge;
	/* [한국어] 직접 부모 브릿지 디바이스 - 트리 구조 추적용. */

	struct vmd_pci_bus *bus, *parent;
	/* [한국어] bus = 이 디바이스가 매달린 버스, parent = 이 디바이스의 parent bus(보통 동일).
	 * led.c가 endpoint의 LED 제어 시 parent bridge를 따라 올라감. */

	struct vmd_pci_bus *bus_object;  /* bus tracks pci bus associated with this dev if type 1 dev. */
	/* [한국어] 이 디바이스가 type-1 브릿지인 경우, 자기가 관리하는 secondary bus 객체.
	 * type-0(엔드포인트)면 NULL. */

	struct vmd_pci_bus *subordinate;
	/* [한국어] 이 디바이스가 부모인 가장 깊은 subordinate bus(브릿지 chain의 끝). */

	volatile struct pci_header *header;
	/* [한국어] 이 디바이스의 PCI config header 영역(0..0x40) MMIO 매핑.
	 * 설정자: vmd_dev_init()이 cfgbar 내 BDF 오프셋으로 계산.
	 * 읽는 자: VID/DID/Class 등 표준 필드 접근.
	 * volatile: MMIO 캐시 회피. */

	volatile struct pci_express_cap *pcie_cap;
	/* [한국어] PCIe capability 레지스터 영역 (slot control/status, link control 등).
	 * 설정자: PCIe cap pointer 따라가며 set.
	 * 읽는 자: led.c가 slot_control 비트로 LED 제어. */

	volatile struct pci_msix_capability *msix_cap;
	/* [한국어] MSI-X capability 영역. 인터럽트 활성/벡터 수 제어. */

	volatile struct pci_msi_cap *msi_cap;
	/* [한국어] MSI capability 영역 (MSI-X 미지원 디바이스용 fallback). */

	volatile struct serial_number_capability *sn_cap;
	/* [한국어] PCIe serial number extended capability - 디바이스 고유 식별자(64-bit). */

	volatile struct pci_msix_table_entry *msix_table;
	/* [한국어] 디바이스의 MSI-X 테이블 - 각 벡터의 (addr, data, mask) 트리플. */

	TAILQ_ENTRY(vmd_pci_device) tailq;
	/* [한국어] vmd_pci_bus.dev_list TAILQ 노드 - 버스에 매달린 디바이스 리스트 연결. */

	uint32_t  class;
	/* [한국어] PCI Class code (24-bit) - mass storage/bridge 등 식별. */

	uint16_t  vid;
	/* [한국어] PCI Vendor ID. */

	uint16_t  did;
	/* [한국어] PCI Device ID. */

	uint16_t  pcie_flags, msix_table_size;
	/* [한국어] pcie_flags = PCIe capability의 device/port type 비트.
	 * msix_table_size = MSI-X 벡터 수 - 1. */

	uint32_t  devfn;
	/* [한국어] PCI device + function 번호 (5비트 dev + 3비트 func). */

	bool      hotplug_capable;
	/* [한국어] 이 디바이스 슬롯이 핫플러그 가능한지 - PCIe slot capability에서 추출. */

	uint32_t  header_type    : 1;
	/* [한국어] 0=type-0 endpoint, 1=type-1 bridge. PCI header 첫 바이트의 비트 7. */

	uint32_t  multifunction  : 1;
	/* [한국어] 멀티펑션 디바이스 여부 (function 0 외 추가 함수 존재). */

	uint32_t  hotplug_bridge : 1;
	/* [한국어] 이 브릿지가 핫플러그 슬롯을 노출하는지. */

	uint32_t  is_added       : 1;
	/* [한국어] SPDK PCI 시스템에 add_device로 등록되었는지. */

	uint32_t  is_hooked      : 1;
	/* [한국어] 디바이스의 인터럽트 라우팅이 VMD MSI-X로 hook 완료됐는지. */

	uint32_t  rsv1           : 12;
	/* [한국어] 비트필드 패딩. */

	uint32_t  target         : 16;
	/* [한국어] 어댑터 내 target 인덱스 (vmd_adapter.target[] 배열 인덱스). */

	struct vmd_hot_plug hp;
	/* [한국어] 이 디바이스가 핫플러그 브릿지인 경우의 슬롯 메타데이터. 일반 디바이스는 미사용. */

	/* Cached version of the slot_control register */
	union express_slot_control_register cached_slot_control;
	/* [한국어] PCIe slot_control 레지스터의 마지막 write 캐시 - led.c가 매번 MMIO read를 피하려고 SW 측 캐시 유지.
	 * 설정자: vmd_led_set_indicator_control()이 write 직후 다시 읽어 저장.
	 * 읽는 자: vmd_led_get_state()가 MMIO read 없이 LED 상태 조회. */
};

/*
 * The VMD adapter
 */
/* [한국어] VMD 어댑터 1개 - Xeon CPU에 통합된 VMD 컨트롤러 1개에 대응.
 * 어댑터 자체가 cfgbar(PCI config window)와 membar(자식 디바이스 BAR 영역)를 노출하며,
 * 이 안에서 별도의 가상 PCI 도메인이 enumerate된다. */
struct vmd_adapter {
	struct spdk_pci_device *pci;
	/* [한국어] VMD 어댑터를 노출하는 호스트 PCI 디바이스(상위 root complex가 보는 디바이스).
	 * 설정자: spdk_vmd_init()이 enumerate한 결과. */

	uint32_t domain;
	/* [한국어] VMD가 만든 가상 PCI 도메인 번호 (host 도메인과 별개).
	 * 값 범위: 보통 0x10000 이상의 값으로 host bus와 충돌 회피. */

	/* physical and virtual VMD bars */
	uint64_t cfgbar, cfgbar_size;
	/* [한국어] cfgbar = VMD config 공간 BAR의 물리 시작 주소 + 크기.
	 * config 공간은 BDF 인덱싱으로 4 KiB씩 차례로 노출되므로, BDF로 직접 오프셋 계산 가능. */

	uint64_t membar, membar_size;
	/* [한국어] membar = VMD가 자식 디바이스에 할당해주는 메모리 풀의 BAR 시작/크기.
	 * 자식 BAR은 이 풀에서 할당되어 host CPU에서 접근 가능. */

	volatile uint8_t *cfg_vaddr;
	/* [한국어] cfgbar mmap한 가상 주소 - BDF별 config space 접근에 사용. */

	volatile uint8_t *mem_vaddr;
	/* [한국어] membar mmap한 가상 주소 - 자식 BAR의 vaddr 계산에 사용. */

	volatile struct pci_msix_table_entry *msix_table;
	/* [한국어] VMD 자체의 MSI-X 테이블 - 인터럽트(특히 핫플러그)를 받는 데 사용. */

	uint32_t bar_sizes[6];
	/* [한국어] VMD 어댑터 자기 BAR 6개의 크기 캐시 - cfgbar/membar/msixbar 식별용. */

	uint64_t physical_addr;
	/* [한국어] 자식 BAR 할당 시 사용할 다음 물리 주소 커서 (membar 내부에서 sequential 할당). */

	uint32_t current_addr_size;
	/* [한국어] 현재 처리 중인 BAR의 sizing 결과 임시 저장. */

	uint32_t next_bus_number : 10;
	/* [한국어] 다음 enumerate 시 부여할 secondary bus 번호 (10비트=1024). */

	uint32_t max_pci_bus : 10;
	/* [한국어] 이 어댑터에서 사용 가능한 최대 bus 번호. */

	uint32_t root_port_updated : 1;
	/* [한국어] root port BAR window가 갱신됐는지 - 핫플러그 후 일관성 확인 용도. */

	uint32_t scan_completed : 1;
	/* [한국어] 초기 enumerate가 끝났는지 - 끝난 뒤에는 hotplug만 처리. */

	uint32_t rsv : 10;
	/* [한국어] 비트필드 패딩. */

	/* end devices attached to vmd adapters */
	struct vmd_pci_device *target[MAX_VMD_TARGET];
	/* [한국어] 어댑터에 attach된 endpoint 디바이스 배열 (target index로 접근).
	 * 설정자: enumerate 시 채움.
	 * 읽는 자: vmd_find_device 등이 BDF→target 매핑에 사용. */

	uint32_t  dev_count  : 16;
	/* [한국어] 현재 attached 디바이스 총 개수. */

	uint32_t  nvme_count : 8;
	/* [한국어] 그중 NVMe 디바이스 수 (Class code 0x010802 검사). */

	uint32_t  vmd_index  : 8;
	/* [한국어] 시스템 내 VMD 어댑터 인덱스 (0,1,2 …). */

	struct vmd_pci_bus vmd_bus;
	/* [한국어] 이 어댑터의 root bus 객체 - bus_list의 헤드 역할도 함. */

	TAILQ_HEAD(, vmd_pci_bus) bus_list;
	/* [한국어] 어댑터 내부의 모든 PCI 버스 리스트 (BFS 탐색 결과 누적).
	 * 설정자: scan 단계가 INSERT_TAIL.
	 * 읽는 자: enumerate/teardown/find_device 등 순회. */

	struct event_fifo *hp_queue;
	/* [한국어] 핫플러그 이벤트 큐 - 인터럽트 핸들러가 enqueue, polling 스레드가 dequeue 후 처리.
	 * 설정자: 어댑터 init 시 spdk_event_fifo로 할당.
	 * 읽는 자: vmd_hotplug_poller가 매 라운드 dequeue. */
};

/*
 * [한국어]
 * vmd_find_device - BDF 주소로 vmd_pci_device 핸들 찾기
 *
 * @addr: 찾을 PCI 주소 (domain/bus/dev/func 4-tuple).
 * @return: 일치하는 vmd_pci_device 포인터 또는 NULL(못 찾음).
 *
 * 왜 필요한가: led.c 등 외부 모듈은 spdk_pci_device * 만 받으므로, VMD 측 추가 메타(parent bridge,
 * pcie_cap 등)에 접근하려면 BDF로 vmd_pci_device를 역검색해야 한다. 이 함수는 모든 어댑터의
 * bus_list를 순회하며 매칭 디바이스를 반환한다.
 *
 * 실행 컨텍스트: led.c의 RPC 핸들러 등 다양한 스레드에서 호출 가능 - bus_list 순회는 read-only.
 * (단, 핫플러그 중 동시 변경에 대한 동기화는 별도 메커니즘 가정.)
 *
 * 호출 체인:
 *   spdk_vmd_set_led_state() → vmd_get_led_device() → [vmd_find_device] → 어댑터 bus_list 순회
 */
struct vmd_pci_device *vmd_find_device(const struct spdk_pci_addr *addr);

#endif /* VMD_H */
