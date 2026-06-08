/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Intel VMD(Volume Management Device) 드라이버 코어 (vmd.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Intel VMD(Volume Management Device)라는 특수한 PCIe 디바이스 뒤에 숨겨진
 * NVMe SSD들을 SPDK가 직접 접근할 수 있도록 만드는 유저스페이스 PCIe 루트 복합체
 * 에뮬레이터이다. VMD는 하나의 PCIe 엔드포인트처럼 호스트에 보이지만, 그 내부에는
 * 가상 루트 포트(root port)들과 그 아래에 연결된 다수의 NVMe SSD가 들어 있는 별도의
 * PCIe 도메인이 존재한다. 일반 PCI enumeration은 VMD 뒤의 디바이스를 볼 수 없으므로,
 * 이 파일은 VMD의 두 BAR(config BAR + memory BAR)를 직접 매핑하여 자체적으로 PCI
 * config 공간을 읽고(bus/device/function 순회), 각 디바이스에 BAR 주소를 직접 할당하며,
 * 브리지의 base/limit/secondary/subordinate 레지스터를 채우고, MSI-X 테이블을 재맵핑하고,
 * 핫플러그(hot-plug/hot-remove)를 처리한다. 즉 OS 커널이나 BIOS가 하던 PCIe 열거/자원
 * 할당 작업을 SPDK 유저스페이스가 VMD 도메인 내부에서 직접 수행하는 것이 핵심이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택에서 이 파일은 NVMe 드라이버(lib/nvme)보다 한 단계 아래, 즉 PCI
 * enumeration 계층에 위치한다. 진입점은 spdk_vmd_init()으로, DPDK/env_dpdk의
 * spdk_pci_enumerate()를 호출해 시스템의 모든 VMD 엔드포인트를 찾고(vmd_enum_cb),
 * 각 VMD에 대해 BAR를 매핑(vmd_domain_map_bars)한 뒤 vmd_enumerate_devices()로
 * 내부 PCIe 트리를 깊이우선(depth-first)으로 스캔한다. 발견된 NVMe SSD 각각에 대해
 * spdk_pci_hook_device()를 호출하여 가짜 spdk_pci_device(type="vmd")를 SPDK PCI
 * 서브시스템에 등록하면, 이후 lib/nvme의 NVMe PCIe 트랜스포트가 이 디바이스를 일반
 * NVMe SSD처럼 attach/probe할 수 있게 된다. 실행 컨텍스트는 전적으로 호스트
 * 유저스페이스이며, 대부분의 함수는 초기화/핫플러그 스캔 시점에 단일 스레드에서
 * 호출된다(아래 동기화 항목 참조).
 *
 * === 타 모듈과의 연결 ===
 * - 의존 대상(callee): env_dpdk의 spdk_pci_enumerate / spdk_pci_device_map_bar /
 *   spdk_pci_device_cfg_read(32) / spdk_pci_hook_device / spdk_pci_unhook_device /
 *   spdk_pci_addr_compare 등 PCI 추상화 API. lib/nvme의 spdk_pci_nvme_get_driver()로
 *   NVMe 드라이버 핸들을 얻어 hook한다. vmd_internal.h가 정의하는 자료구조
 *   (vmd_adapter / vmd_pci_bus / vmd_pci_device / vmd_hot_plug / pci_header 등)와
 *   PCI/PCIe 와이어 포맷 매크로(CONFIG_OFFSET_ADDR, BRIDGE_BASEREG 등)를 사용한다.
 * - 의존 주체(caller): spdk_vmd_init/fini/rescan/hotplug_monitor/pci_device_list/
 *   remove_device는 SPDK 애플리케이션 또는 RPC 핸들러가 호출하는 공개 API이다.
 *   vmd_attach_device/vmd_detach_device는 g_vmd_device_provider를 통해 SPDK PCI
 *   서브시스템에 등록되어, 사용자가 VMD 뒤의 디바이스를 attach/detach할 때 콜백된다.
 * - 데이터 흐름: 물리 하드웨어(VMD memory BAR가 매핑한 MMIO 영역)의 PCI config 공간을
 *   읽어 vmd_pci_device/vmd_pci_bus 트리(TAILQ 연결 리스트)로 구성하고, 그 트리를
 *   순회하며 BAR 주소(membar 윈도 내부)를 할당해 다시 하드웨어 config 공간에 기록한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_vmd_init(): 진입점. 모든 VMD를 enumerate하고 내부 NVMe를 SPDK에 hook.
 * - vmd_enum_cb(): VMD 하나가 발견될 때마다 호출되는 콜백 — BAR 매핑 후 스캔 시작.
 * - vmd_scan_single_bus(): 하나의 PCI 버스를 0~31 device 순회하며 깊이우선 재귀 스캔.
 * - vmd_assign_base_addrs(): 디바이스의 BAR 크기를 probe하고 membar에서 주소를 할당.
 * - vmd_allocate_base_addr() / vmd_hotplug_allocate_base_addr(): membar/핫플러그 슬롯
 *   메모리 윈도에서 size-aligned 주소를 first-fit으로 분배.
 * - spdk_vmd_hotplug_monitor(): slot status 비트를 폴링하여 hot-plug/remove를 감지.
 * - struct vmd_container: 시스템의 모든 VMD 어댑터를 담는 전역 컨테이너.
 */

#include "vmd_internal.h"   /* [한국어] VMD 전용 내부 자료구조(vmd_adapter/vmd_pci_bus/
				   vmd_pci_device/vmd_hot_plug/pci_header 등)와 PCI/PCIe
				   와이어 포맷 매크로(CONFIG_OFFSET_ADDR, BRIDGE_BASEREG,
				   VMD_UPPER_*_SIGNATURE, PCI_HEADER_TYPE_* 등) 정의 헤더. */

#include "spdk/stdinc.h"    /* [한국어] SPDK 표준 include 묶음(stdint/string/assert 등) —
				   calloc/free/memcmp/assert/TAILQ 매크로 사용을 위해 필요. */
#include "spdk/string.h"    /* [한국어] spdk_strerror()(errno→문자열) 사용 — hook 실패 등
				   에러 로깅 시 음수 rc를 사람이 읽을 메시지로 변환하기 위함. */
#include "spdk/likely.h"    /* [한국어] spdk_likely/unlikely 분기 예측 힌트 매크로 제공
				   (이 파일에서는 직접 쓰이지 않더라도 공통 include 규약). */

/* [한국어] PCIe Capability의 device_type 필드(4비트, 0~12) 값을 사람이 읽는 문자열로
 *          매핑하는 룩업 테이블. vmd_print_pci_info()가 SPDK_INFOLOG로 디바이스 종류를
 *          출력할 때 인덱싱한다. 인덱스는 PCI Express Capabilities Register의
 *          Device/Port Type 필드(PCIe Base Spec) 값과 1:1 대응한다 — 예: 4 = Root Port,
 *          5 = Upstream Port, 6 = Downstream Port. VMD 도메인 내부는 가상 Root Port와
 *          Switch Port들로 구성되므로 이 분류가 로그 가독성에 중요하다. */
static unsigned char *device_type[] = {
	"PCI Express Endpoint",
	"Legacy PCI Express Endpoint",
	"Reserved 1",
	"Reserved 2",
	"Root Port of PCI Express Root Complex",
	"Upstream Port of PCI Express Switch",
	"Downstream Port of PCI Express Switch",
	"PCI Express to PCI/PCI-X Bridge",
	"PCI/PCI-X to PCI Express Bridge",
	"Root Complex Integrated Endpoint",
	"Root Complex Event Collector",
	"Reserved Capability"
};

/*
 * Container for all VMD adapter probed in the system.
 */
/* [한국어] 시스템에서 발견된 모든 VMD 어댑터를 담는 컨테이너 구조체.
 *          spdk_pci_enumerate()가 VMD 엔드포인트를 하나씩 발견할 때마다
 *          vmd[count]에 채워지고 count가 증가한다. 전역 g_vmd_container 하나만 존재. */
struct vmd_container {
	uint32_t count;
	/* [한국어] 현재까지 발견·초기화된 VMD 어댑터 개수(vmd[] 배열의 유효 길이).
	 * 설정자: vmd_enum_cb()가 VMD 하나를 처리할 때마다 ++.
	 * 읽는 자: spdk_vmd_init 이후의 거의 모든 순회 함수(find_device/hotplug_monitor/
	 *   rescan/pci_device_list/attach_device)가 0..count-1 루프 상한으로 사용.
	 * 값 범위: 0 ~ MAX_VMD_SUPPORTED. 동기화: 초기화는 단일 enumerate 경로에서만
	 *   증가하므로 락 불필요(이후엔 읽기 전용으로 취급). */

	struct vmd_adapter vmd[MAX_VMD_SUPPORTED];
	/* [한국어] VMD 어댑터 객체의 정적 배열. 각 vmd_adapter는 하나의 물리 VMD 엔드포인트에
	 *   대응하며, 그 BAR 매핑/버스 트리(bus_list)/할당 커서(physical_addr 등)를 보유.
	 * 설정자: vmd_enum_cb()가 vmd_c->vmd[count]를 가리켜 채운다.
	 * 읽는 자: 위 순회 함수 전체. 값 범위: MAX_VMD_SUPPORTED개 슬롯(vmd_internal.h 정의).
	 * 동기화: 인덱스별 소유가 분리되어 enumerate 시점 단일 스레드 접근 — 락 불필요. */
};

/* [한국어] 시스템 전역 VMD 컨테이너 인스턴스. SPDK 프로세스당 하나만 존재하며,
 *          spdk_vmd_init()이 채우고 이후 모든 공개 API가 이 전역을 참조한다. */
static struct vmd_container g_vmd_container;
/* [한국어] 마지막 버스 스캔에서 발견한 엔드 디바이스(NVMe SSD 등 type-0 헤더) 누적 개수.
 *          vmd_scan_pcibus() 시작 시 0으로 리셋되고 vmd_init_end_device() 성공마다 증가하며,
 *          스캔 종료 후 INFOLOG로 출력하는 디버그용 카운터(기능에 영향 없음). */
static uint8_t g_end_device_count;

/*
 * [한국어]
 * vmd_is_valid_cfg_addr - 주어진 가상주소가 VMD config BAR 매핑 범위 안에 있는지 검사
 *
 * @bus: 검사 기준이 되는 VMD 버스(여기서 bus->vmd로 어댑터의 cfg_vaddr/cfgbar_size 접근)
 * @addr: 검사 대상 가상주소(보통 CONFIG_OFFSET_ADDR로 계산된 PCI config 공간 포인터)
 * @return: addr이 [cfg_vaddr, cfg_vaddr+cfgbar_size) 범위 내면 true, 아니면 false
 *
 * VMD의 PCI config 공간은 config BAR가 매핑한 MMIO 윈도 안에 bus/dev/fn 오프셋으로
 * 배치된다. 존재하지 않는 bus/dev에 대한 오프셋은 매핑 범위를 벗어날 수 있으므로,
 * 그 포인터를 역참조하기 전에 이 함수로 경계를 검사해 잘못된 MMIO 접근(SIGBUS 등)을
 * 방지한다. 호스트 유저스페이스, 스캔 컨텍스트의 단일 스레드에서 호출.
 *
 * 호출 체인:
 *   vmd_bus_device_present() → [vmd_is_valid_cfg_addr]
 */
static bool
vmd_is_valid_cfg_addr(struct vmd_pci_bus *bus, uint64_t addr)
{
	/* [한국어] addr이 config BAR 시작(cfg_vaddr) 이상이고 끝(cfg_vaddr+cfgbar_size) 미만일
	 *          때만 유효. 두 조건 모두 만족해야 매핑된 MMIO 안의 정당한 config 포인터. */
	return addr >= (uint64_t)bus->vmd->cfg_vaddr &&
	       addr < bus->vmd->cfgbar_size + (uint64_t)bus->vmd->cfg_vaddr;
}

/*
 * [한국어]
 * vmd_align_base_addrs - membar 할당 커서를 지정한 정렬 경계로 올림(round-up)
 *
 * @vmd: 할당 커서(physical_addr)와 남은 크기(current_addr_size)를 보유한 VMD 어댑터
 * @alignment: 정렬 경계(2의 거듭제곱, 예: ONE_MB). 이 값의 배수로 커서를 맞춘다.
 * @return: 없음(vmd->physical_addr / current_addr_size를 부작용으로 갱신)
 *
 * VMD memory BAR(membar) 윈도에서 디바이스 BAR 주소를 선형 할당할 때, 다음 할당이
 * alignment 경계에서 시작하도록 커서를 미리 정렬한다. PCI 브리지의 mem_base/limit는
 * 1MB 단위로만 표현 가능하므로(BRIDGE_BASEREG 16비트 << 16) 디바이스 BAR을 새로
 * 배치하기 전 커서를 1MB 등으로 정렬해 두는 것이 안전하다. 핫플러그 경로가 아닐 때만
 * 사용된다. 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_assign_base_addrs() → [vmd_align_base_addrs]
 */
static void
vmd_align_base_addrs(struct vmd_adapter *vmd, uint32_t alignment)
{
	uint32_t pad;   /* [한국어] 정렬 경계까지 끌어올리기 위해 버려야 할 패딩 바이트 수. */

	/*
	 *  Device is not in hot plug path, align the base address remaining from membar 1.
	 */
	/* [한국어] physical_addr이 이미 alignment 배수면 (addr & (align-1))==0 → 조정 불필요.
	 *          하위 비트가 남아 있으면(=정렬 안 됨) 패딩을 계산해 커서를 올린다.
	 *          alignment가 2의 거듭제곱이므로 (alignment-1)은 하위 비트 마스크가 된다. */
	if (vmd->physical_addr & (alignment - 1)) {
		/* [한국어] 다음 alignment 경계까지의 거리 = alignment - (현재 오프셋 잔여). */
		pad = alignment - (vmd->physical_addr & (alignment - 1));
		vmd->physical_addr += pad;       /* [한국어] 커서를 패딩만큼 전진(정렬 완료). */
		vmd->current_addr_size -= pad;   /* [한국어] 버린 패딩만큼 남은 윈도 크기 감소. */
	}
}

/*
 * [한국어]
 * vmd_device_is_enumerated - 이 루트 포트가 (이전에) 이미 enumerate 완료되었는지 판별
 *
 * @header: 검사할 PCI config 헤더(브리지 타입의 prefetch_base/limit_upper 필드를 봄)
 * @return: prefetch upper 레지스터에 VMD 시그니처가 둘 다 박혀 있으면 true
 *
 * SPDK(또는 이전 드라이버)는 루트 포트를 스캔 완료한 뒤 prefetch_base_upper/
 * prefetch_limit_upper에 VMD_UPPER_BASE/LIMIT_SIGNATURE라는 매직값을 써 둔다
 * (vmd_cache_scan_info 참조). 따라서 이 시그니처가 있으면 해당 도메인이 이미
 * 열거되었다는 뜻이고, 재스캔 시 base/limit 레지스터를 다시 리셋하지 않도록
 * 판단 근거로 쓰인다. 스캔/리셋 컨텍스트의 단일 스레드에서 호출.
 *
 * 호출 체인:
 *   vmd_update_scan_info()/vmd_reset_root_ports() → [vmd_device_is_enumerated]
 */
static bool
vmd_device_is_enumerated(volatile struct pci_header *header)
{
	/* [한국어] 두 prefetch upper 레지스터가 모두 SPDK가 심어 둔 매직 시그니처와 일치할
	 *          때만 "이미 열거됨"으로 간주. volatile 접근이므로 실제 MMIO config 읽기. */
	return header->one.prefetch_base_upper == VMD_UPPER_BASE_SIGNATURE &&
	       header->one.prefetch_limit_upper == VMD_UPPER_LIMIT_SIGNATURE;
}

/*
 * [한국어]
 * vmd_device_is_root_port - 주어진 PCI 디바이스가 Intel VMD 가상 루트 포트인지 판별
 *
 * @header: 검사할 PCI config 헤더(common.vendor_id / device_id 비교)
 * @return: Intel 벤더이면서 알려진 VMD 루트 포트 device_id 중 하나면 true
 *
 * VMD 도메인 내부의 최상위 가상 브리지는 "루트 포트"이며, 그 아래로 스위치/엔드포인트가
 * 매달린다. 루트 포트는 base/limit 레지스터 리셋, 스캔 완료 시그니처 기록 등 특별
 * 취급이 필요하므로 device_id 화이트리스트로 식별한다. 목록은 Skylake-X(SKX)와
 * Ice Lake(ICX) 세대의 루트 포트 A~D를 포함한다. 스캔 컨텍스트 단일 스레드.
 *
 * 호출 체인:
 *   vmd_update_scan_info()/vmd_cache_scan_info()/vmd_reset_root_ports() → [이 함수]
 */
static bool
vmd_device_is_root_port(volatile struct pci_header *header)
{
	/* [한국어] Intel 벤더(0x8086)이면서 SKX/ICX 세대 루트 포트 A~D device_id 8종 중
	 *          하나와 일치할 때만 루트 포트로 인정. OR로 묶인 8개가 화이트리스트. */
	return header->common.vendor_id == SPDK_PCI_VID_INTEL &&
	       /* [한국어] Skylake-X 세대 VMD 루트 포트 A/B/C/D 4종. */
	       (header->common.device_id == PCI_ROOT_PORT_A_INTEL_SKX ||
		header->common.device_id == PCI_ROOT_PORT_B_INTEL_SKX ||
		header->common.device_id == PCI_ROOT_PORT_C_INTEL_SKX ||
		header->common.device_id == PCI_ROOT_PORT_D_INTEL_SKX ||
		/* [한국어] Ice Lake 세대 VMD 루트 포트 A/B/C/D 4종. */
		header->common.device_id == PCI_ROOT_PORT_A_INTEL_ICX ||
		header->common.device_id == PCI_ROOT_PORT_B_INTEL_ICX ||
		header->common.device_id == PCI_ROOT_PORT_C_INTEL_ICX ||
		header->common.device_id == PCI_ROOT_PORT_D_INTEL_ICX);
}

/*
 * [한국어]
 * vmd_hotplug_coalesce_regions - 핫플러그 free 메모리 큐에서 인접한 영역들을 병합
 *
 * @hp: 핫플러그 슬롯의 메모리 관리자(free/unused/alloc 세 TAILQ를 보유)
 * @return: 없음(free_mem_queue를 부작용으로 정리)
 *
 * 핫플러그 슬롯의 메모리 윈도는 디바이스가 빠질 때마다 단편화된다. 주소 순으로 정렬된
 * free_mem_queue에서 "앞 영역 끝 == 뒷 영역 시작"인 인접 쌍을 찾아 하나로 합치고,
 * 비워진 디스크립터(pci_mem_mgr)는 재사용을 위해 unused_mem_queue로 되돌린다.
 * 한 번의 패스로 한 쌍만 병합하므로 더 병합할 게 없을 때까지 do-while로 반복한다.
 * 핫플러그 처리(단일 스레드) 컨텍스트에서만 호출되어 락 불필요.
 *
 * 호출 체인:
 *   vmd_hotplug_free_region() → [vmd_hotplug_coalesce_regions]
 */
static void
vmd_hotplug_coalesce_regions(struct vmd_hot_plug *hp)
{
	struct pci_mem_mgr *region, *prev;   /* [한국어] region: 현재 검사 영역, prev: 직전 영역. */

	do {
		prev = NULL;   /* [한국어] 매 패스 시작마다 직전 영역 추적을 초기화. */
		/* [한국어] 주소순 정렬된 free 큐를 훑어 prev의 끝과 region의 시작이 맞닿은
		 *          첫 쌍을 찾는다(=병합 후보). 찾으면 break하여 region에 후보가 남음. */
		TAILQ_FOREACH(region, &hp->free_mem_queue, tailq) {
			if (prev != NULL && (prev->addr + prev->size == region->addr)) {
				break;   /* [한국어] prev와 region이 인접 → 병합 대상 발견. */
			}

			prev = region;   /* [한국어] 인접 아님 → prev를 한 칸 전진. */
		}

		/* [한국어] region!=NULL이면 prev/region이 인접한 쌍 → 실제 병합 수행. */
		if (region != NULL) {
			prev->size += region->size;   /* [한국어] prev 영역을 region 크기만큼 확장. */
			/* [한국어] 흡수된 region 디스크립터를 free 큐에서 제거하고… */
			TAILQ_REMOVE(&hp->free_mem_queue, region, tailq);
			/* [한국어] …재사용 가능하도록 unused 큐로 반환(디스크립터 풀 환원). */
			TAILQ_INSERT_TAIL(&hp->unused_mem_queue, region, tailq);
		}
	} while (region != NULL);   /* [한국어] 이번 패스에서 병합이 있었으면 다시 한 번 검사. */
}

/*
 * [한국어]
 * vmd_hotplug_free_region - free된 메모리 영역을 주소 정렬 위치에 삽입하고 병합 수행
 *
 * @hp: 핫플러그 슬롯의 메모리 관리자
 * @region: free 큐로 되돌릴 메모리 영역 디스크립터(주소·크기 보유)
 * @return: 없음(free_mem_queue를 갱신하고 coalesce 호출)
 *
 * free_mem_queue를 항상 주소 오름차순으로 유지하기 위해, region->addr보다 큰 첫
 * 영역 직전에 삽입한다(정렬 삽입). 삽입 후 vmd_hotplug_coalesce_regions()로 인접
 * 영역을 병합해 단편화를 줄인다. assert로 BAR 윈도 범위와 정렬 불변식(앞 영역 끝 ≤
 * region 시작 ≤ region 끝 ≤ 뒤 영역 시작)을 검증한다. 핫플러그 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_hotplug_free_addr()/vmd_hotplug_allocate_base_addr() → [이 함수] → coalesce
 */
static void
vmd_hotplug_free_region(struct vmd_hot_plug *hp, struct pci_mem_mgr *region)
{
	struct pci_mem_mgr *current, *prev = NULL;   /* [한국어] current: 삽입 위치 탐색 커서, prev: 그 직전. */

	/* [한국어] region 주소가 이 핫플러그 BAR 윈도 [start, start+size) 안에 있어야 함(디버그 검증). */
	assert(region->addr >= hp->bar.start && region->addr < hp->bar.start + hp->bar.size);

	/* [한국어] 주소순 정렬을 유지하려고 region->addr보다 큰 첫 영역(current)을 찾는다.
	 *          그 직전(prev)이 삽입 위치가 된다. */
	TAILQ_FOREACH(current, &hp->free_mem_queue, tailq) {
		if (current->addr > region->addr) {
			break;   /* [한국어] region이 들어갈 자리 발견(current 앞에 삽입). */
		}

		prev = current;   /* [한국어] 아직 region보다 작음 → 계속 전진. */
	}

	/* [한국어] prev가 있으면 그 뒤에, 없으면(=가장 작은 주소) 큐 맨 앞에 삽입. */
	if (prev != NULL) {
		/* [한국어] 정렬 불변식: prev의 끝이 region 시작을 넘지 않아야 겹치지 않음. */
		assert(prev->addr + prev->size <= region->addr);
		/* [한국어] region의 끝이 current 시작을 넘지 않아야 함(뒤 영역과도 비겹침). */
		assert(current == NULL || (region->addr + region->size <= current->addr));
		TAILQ_INSERT_AFTER(&hp->free_mem_queue, prev, region, tailq);   /* [한국어] prev 뒤에 정렬 삽입. */
	} else {
		TAILQ_INSERT_HEAD(&hp->free_mem_queue, region, tailq);   /* [한국어] 최소 주소 → 맨 앞 삽입. */
	}

	vmd_hotplug_coalesce_regions(hp);   /* [한국어] 삽입 직후 인접 free 영역 병합. */
}

/*
 * [한국어]
 * vmd_hotplug_free_addr - 특정 시작 주소로 alloc된 핫플러그 영역을 찾아 free로 반환
 *
 * @hp: 핫플러그 슬롯의 메모리 관리자
 * @addr: 해제할 영역의 시작 주소(이전에 allocate가 반환했던 값)
 * @return: 없음(alloc 큐에서 제거 후 free 큐로 환원)
 *
 * 디바이스가 핫리무브되어 BAR이 회수될 때, 그 BAR이 점유하던 영역을 alloc_mem_queue
 * 에서 주소로 검색해 제거하고 vmd_hotplug_free_region()으로 free 큐에 되돌린다.
 * vmd_dev_free()에서 디바이스 BAR마다 호출된다. 핫플러그/디바이스 해제 단일 스레드.
 *
 * 호출 체인:
 *   vmd_dev_free() → [vmd_hotplug_free_addr] → vmd_hotplug_free_region
 */
static void
vmd_hotplug_free_addr(struct vmd_hot_plug *hp, uint64_t addr)
{
	struct pci_mem_mgr *region;   /* [한국어] addr와 일치하는 alloc된 영역 디스크립터. */

	/* [한국어] alloc 큐를 훑어 시작 주소가 addr와 정확히 일치하는 영역을 찾는다. */
	TAILQ_FOREACH(region, &hp->alloc_mem_queue, tailq) {
		if (region->addr == addr) {
			break;   /* [한국어] 일치 영역 발견. */
		}
	}

	assert(region != NULL);   /* [한국어] alloc된 적 없는 주소를 free하려 하면 버그 → 검증. */
	TAILQ_REMOVE(&hp->alloc_mem_queue, region, tailq);   /* [한국어] alloc 큐에서 제거. */

	vmd_hotplug_free_region(hp, region);   /* [한국어] free 큐로 정렬 삽입 + 병합. */
}

/*
 * [한국어]
 * vmd_hotplug_allocate_base_addr - 핫플러그 슬롯 메모리 윈도에서 size만큼 first-fit 할당
 *
 * @hp: 핫플러그 슬롯의 메모리 관리자
 * @size: 할당할 메모리 윈도 크기(디바이스 BAR 크기)
 * @return: 할당된 영역의 시작 물리주소, 충분한 free 영역이 없으면 0
 *
 * free_mem_queue에서 size 이상인 첫 영역을 골라(first-fit) 그만큼 떼어 준다. 영역이
 * 요청보다 크면 나머지를 새 free 영역으로 쪼개 다시 free 큐에 넣고(쪼개는 데 쓸 빈
 * 디스크립터는 unused 큐에서 가져옴), 사용한 영역은 alloc_mem_queue로 옮긴다.
 * 핫플러그로 삽입된 디바이스의 BAR 주소 할당에 쓰인다. 단일 스레드 핫플러그 컨텍스트.
 *
 * 호출 체인:
 *   vmd_allocate_base_addr()/vmd_get_base_addr()/vmd_init_hotplug() → [이 함수]
 */
static uint64_t
vmd_hotplug_allocate_base_addr(struct vmd_hot_plug *hp, uint32_t size)
{
	struct pci_mem_mgr *region = NULL, *free_region;   /* [한국어] region: 선택된 free 영역, free_region: 잘려나간 나머지 보관용. */

	/* [한국어] free 큐에서 요청 size를 수용 가능한(size 이상) 첫 영역을 찾는다(first-fit). */
	TAILQ_FOREACH(region, &hp->free_mem_queue, tailq) {
		if (region->size >= size) {
			break;   /* [한국어] 수용 가능한 영역 발견. */
		}
	}

	/* [한국어] 끝까지 못 찾으면 단편화/소진으로 할당 실패 → 0 반환(호출자가 실패 처리). */
	if (region == NULL) {
		SPDK_INFOLOG(vmd, "Unable to find free hotplug memory region of size:"
			     "%"PRIx32"\n", size);
		return 0;
	}

	TAILQ_REMOVE(&hp->free_mem_queue, region, tailq);   /* [한국어] 선택 영역을 free 큐에서 분리. */
	/* [한국어] 영역이 요청보다 크면 남는 뒷부분을 별도 free 영역으로 쪼갠다. */
	if (size < region->size) {
		free_region = TAILQ_FIRST(&hp->unused_mem_queue);   /* [한국어] 나머지를 담을 빈 디스크립터 확보. */
		/* [한국어] 빈 디스크립터가 없으면 쪼갤 수 없음 → 나머지를 버리는 셈(경고만). */
		if (free_region == NULL) {
			SPDK_INFOLOG(vmd, "Unable to find unused descriptor to store the "
				     "free region of size: %"PRIu32"\n", region->size - size);
		} else {
			TAILQ_REMOVE(&hp->unused_mem_queue, free_region, tailq);   /* [한국어] unused 풀에서 디스크립터 꺼냄. */
			free_region->size = region->size - size;   /* [한국어] 나머지 크기 = 원래 - 요청. */
			free_region->addr = region->addr + size;    /* [한국어] 나머지 시작 = 원래 시작 + 요청 크기(뒷부분). */
			region->size = size;                        /* [한국어] 할당 영역은 정확히 요청 크기로 축소. */
			vmd_hotplug_free_region(hp, free_region);   /* [한국어] 나머지를 free 큐에 정렬 삽입(병합 포함). */
		}
	}

	TAILQ_INSERT_TAIL(&hp->alloc_mem_queue, region, tailq);   /* [한국어] 할당된 영역을 alloc 큐로 이동(추적). */

	return region->addr;   /* [한국어] 호출자에게 할당된 물리 시작주소 반환. */
}

/*
 *  Allocates an address from vmd membar for the input memory size
 *  vmdAdapter - vmd adapter object
 *  dev - vmd_pci_device to allocate a base address for.
 *  size - size of the memory window requested.
 *  Size must be an integral multiple of 2. Addresses are returned on the size boundary.
 *  Returns physical address within the VMD membar window, or 0x0 if cannot allocate window.
 *  Consider increasing the size of vmd membar if 0x0 is returned.
 */
/*
 * [한국어]
 * vmd_allocate_base_addr - VMD membar 윈도(또는 핫플러그 슬롯)에서 BAR 주소를 할당
 *
 * @vmd: 할당 커서(physical_addr/current_addr_size)를 보유한 VMD 어댑터
 * @dev: 주소를 할당받을 디바이스(핫플러그 슬롯 하위인지 판별용; NULL 가능)
 * @size: 요청 메모리 윈도 크기(2의 거듭제곱이어야 함)
 * @return: membar 윈도 내 물리 시작주소, 할당 불가 시 0
 *
 * 디바이스 BAR을 배치할 물리 주소를 membar 선형 윈도에서 떼어 준다. 단, dev가 핫플러그
 * 가능한 브리지(부모가 hotplug_capable) 하위라면 그 슬롯 전용 메모리 풀에서 할당한다
 * (vmd_hotplug_allocate_base_addr 위임). 일반 경로에서는 size 경계로 정렬(padding 계산)
 * 한 뒤 남은 윈도가 충분하면 커서를 전진시키며 주소를 반환한다. 반환 0은 membar 소진을
 * 뜻하므로 membar 크기를 늘려야 한다. 스캔 단일 스레드 컨텍스트(락 불필요).
 *
 * 호출 체인:
 *   vmd_assign_base_addrs()/vmd_init_hotplug() → [이 함수] → vmd_hotplug_allocate_base_addr
 */
static uint64_t
vmd_allocate_base_addr(struct vmd_adapter *vmd, struct vmd_pci_device *dev, uint32_t size)
{
	uint64_t base_address = 0, padding = 0;   /* [한국어] base_address: 반환할 주소, padding: 정렬용 패딩. */
	struct vmd_pci_bus *hp_bus;               /* [한국어] dev의 부모 버스(핫플러그 여부 판별용). */

	/* [한국어] size가 2의 거듭제곱이 아니면 거부. (~size+1)는 2의 보수(=최하위 1비트만 남김)이며,
	 *          이것이 size와 같으면 비트가 하나뿐 → 2의 거듭제곱. 아니면 즉시 0 반환. */
	if (size && ((size & (~size + 1)) != size)) {
		return base_address;
	}

	/*
	 *  If device is downstream of a hot plug port, allocate address from the
	 *  range dedicated for the hot plug slot. Search the list of addresses allocated to determine
	 *  if a free range exists that satisfy the input request.  If a free range cannot be found,
	 *  get a buffer from the  unused chunk. First fit algorithm, is used.
	 */
	/* [한국어] dev가 주어졌고 그 부모 브리지가 핫플러그 가능하면, membar가 아니라 그 슬롯
	 *          전용 메모리 풀에서 first-fit 할당해야 한다(핫플러그 영역은 미리 예약됨). */
	if (dev) {
		hp_bus = dev->parent;   /* [한국어] dev가 매달린 버스 = 그 버스의 self가 상위 브리지. */
		if (hp_bus && hp_bus->self && hp_bus->self->hotplug_capable) {
			return vmd_hotplug_allocate_base_addr(&hp_bus->self->hp, size);   /* [한국어] 핫플러그 풀에서 할당. */
		}
	}

	/* Ensure physical membar allocated is size aligned */
	/* [한국어] BAR은 자신의 크기 경계에 정렬돼야 함(PCI 규칙). 커서가 정렬 안 됐으면 패딩 계산. */
	if (vmd->physical_addr & (size - 1)) {
		padding = size - (vmd->physical_addr & (size - 1));   /* [한국어] 다음 size 경계까지 거리. */
	}

	/* Allocate from membar if enough memory is left */
	/* [한국어] 패딩 포함 요청을 수용할 만큼 윈도가 남아 있을 때만 할당 성공. */
	if (vmd->current_addr_size >= size + padding) {
		base_address = vmd->physical_addr + padding;        /* [한국어] 정렬된 위치를 반환 주소로. */
		vmd->physical_addr += size + padding;               /* [한국어] 커서를 패딩+크기만큼 전진. */
		vmd->current_addr_size -= size + padding;           /* [한국어] 남은 윈도 크기 차감. */
	}

	SPDK_INFOLOG(vmd, "allocated(size) %" PRIx64 " (%x)\n", base_address, size);   /* [한국어] 할당 결과 로깅. */

	return base_address;   /* [한국어] 성공 시 정렬된 물리주소, 실패 시 초기값 0. */
}

/*
 * [한국어]
 * vmd_is_end_device - 디바이스가 엔드포인트(type-0 헤더, 예: NVMe SSD)인지 판별
 *
 * @dev: 검사할 VMD PCI 디바이스(NULL/header NULL 안전 처리)
 * @return: 헤더 타입이 type-0(Normal)이면 true, 브리지면 false
 *
 * PCI config 헤더 타입의 하위 비트로 디바이스 종류를 구분한다. type-1(브리지)은 하위
 * 버스를 가지므로 재귀 스캔 대상이고, type-0(엔드포인트)은 실제 기능 디바이스다.
 * multi-function 비트(0x80)는 마스크 아웃하여 순수 헤더 타입만 비교한다. base/limit
 * 레지스터 갱신 여부 판단 등에 쓰인다. 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_assign_base_addrs() → [vmd_is_end_device]
 */
static bool
vmd_is_end_device(struct vmd_pci_device *dev)
{
	/* [한국어] dev와 header가 유효하고, multi-function 비트 제거 후 헤더타입이 Normal(0)이면
	 *          엔드포인트. ~PCI_MULTI_FUNCTION 마스크로 0x80 비트를 떼고 비교. */
	return (dev && dev->header) &&
	       ((dev->header->common.header_type & ~PCI_MULTI_FUNCTION) == PCI_HEADER_TYPE_NORMAL);
}

/*
 * [한국어]
 * vmd_update_base_limit_register - 엔드포인트 BAR 범위를 상위 모든 브리지의 mem_base/limit에 반영
 *
 * @dev: BAR이 할당된 디바이스(엔드포인트 또는 브리지)
 * @base: 디바이스 BAR의 시작에 해당하는 16비트 브리지 base 값(BRIDGE_BASEREG 인코딩)
 * @limit: 디바이스 BAR의 끝에 해당하는 16비트 브리지 limit 값
 * @return: 없음(상위 브리지들의 config mem_base/mem_limit를 부작용으로 갱신)
 *
 * PCI 브리지는 자기 하위의 모든 메모리 BAR을 포함하는 [mem_base, mem_limit] 윈도를
 * config에 가져야 라우팅이 동작한다. 새 엔드포인트 BAR이 할당되면, dev에서 루트
 * 방향으로 부모 브리지 체인을 거슬러 올라가며 각 브리지의 base를 더 낮게/limit를 더
 * 높게 확장한다(겹침 윈도 보장). 이미 스캔 완료된 도메인(scan_completed)에서는 건드리지
 * 않는다. 현재 32비트 메모리 공간만 다룬다. 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_assign_base_addrs() → [vmd_update_base_limit_register]
 */
static void
vmd_update_base_limit_register(struct vmd_pci_device *dev, uint16_t base, uint16_t limit)
{
	struct vmd_pci_bus *bus;        /* [한국어] 상위로 거슬러 올라가며 순회할 버스 커서. */
	struct vmd_pci_device *bridge;  /* [한국어] 현재 버스의 상위 브리지(self). */

	/* [한국어] base/limit 중 하나라도 0이면 유효한 BAR 윈도가 아님 → 갱신 생략. */
	if (base == 0 ||  limit == 0) {
		return;
	}

	/* [한국어] dev 자체가 브리지면 그 하위 버스(bus_object)에서, 엔드포인트면 부모 버스에서
	 *          상향 전파를 시작한다. */
	if (dev->header->common.header_type == PCI_HEADER_TYPE_BRIDGE) {
		bus = dev->bus_object;
	} else {
		bus = dev->parent;
	}

	bridge = bus->self;   /* [한국어] 시작 버스의 상위 브리지. */
	SPDK_INFOLOG(vmd, "base:limit = %x:%x\n", bridge->header->one.mem_base,
		     bridge->header->one.mem_limit);   /* [한국어] 갱신 전 현재 윈도 로깅. */

	/* [한국어] 도메인이 이미 스캔 완료(시그니처 존재)면 레지스터를 고정하고 손대지 않음. */
	if (dev->bus->vmd->scan_completed) {
		return;
	}

	/* [한국어] 루트 포트까지 부모 버스를 따라 올라가며 각 브리지 윈도를 확장한다. */
	while (bus && bus->self != NULL) {
		bridge = bus->self;   /* [한국어] 이 레벨의 브리지. */

		/* This is only for 32-bit memory space, need to revisit to support 64-bit */
		/* [한국어] 브리지 base가 새 BAR 시작보다 크면(윈도가 BAR을 못 덮음) base를 낮춘다. */
		if (bridge->header->one.mem_base > base) {
			bridge->header->one.mem_base = base;          /* [한국어] config에 새 base 기록(MMIO write). */
			base = bridge->header->one.mem_base;           /* [한국어] 즉시 read-back으로 전파값 갱신(posted write flush). */
		}

		/* [한국어] 브리지 limit가 새 BAR 끝보다 작으면 limit를 높여 윈도를 넓힌다. */
		if (bridge->header->one.mem_limit < limit) {
			bridge->header->one.mem_limit = limit;        /* [한국어] config에 새 limit 기록. */
			limit = bridge->header->one.mem_limit;         /* [한국어] read-back으로 상위 레벨 전파값 확정. */
		}

		bus = bus->parent;   /* [한국어] 한 레벨 위 버스로 이동(루트 방향). */
	}
}

/*
 * [한국어]
 * vmd_get_base_addr - (스캔 완료된 도메인에서) 디바이스 BAR의 기존/슬롯 주소를 조회
 *
 * @dev: 주소를 조회할 디바이스
 * @index: BAR 인덱스(브리지 경로에서만 사용)
 * @size: 핫플러그 슬롯 할당 시 요청 크기
 * @return: 해당 BAR에 사용할 물리 시작주소
 *
 * scan_completed 상태(예: 재스캔/핫플러그)에서 디바이스 BAR 주소를 결정한다. 디바이스가
 * 브리지면 이미 config에 적힌 BAR 값을 그대로(하위 4비트 플래그 제거) 사용한다.
 * 엔드포인트면 부모 브리지가 핫플러그 가능 슬롯이면 슬롯 풀에서 새로 할당하고, 아니면
 * 부모 브리지의 mem_base(<<16, 16비트→32비트 주소 복원)를 기준 주소로 쓴다. 스캔/핫플러그
 * 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_assign_base_addrs() → [vmd_get_base_addr] → vmd_hotplug_allocate_base_addr
 */
static uint64_t
vmd_get_base_addr(struct vmd_pci_device *dev, uint32_t index, uint32_t size)
{
	struct vmd_pci_bus *bus = dev->parent;   /* [한국어] dev가 매달린 버스(self가 상위 브리지). */

	/* [한국어] 브리지는 config의 BAR 값을 그대로 사용(하위 4비트는 BAR 타입 플래그라 마스크). */
	if (dev->header_type == PCI_HEADER_TYPE_BRIDGE) {
		return dev->header->zero.BAR[index] & ~0xf;
	} else {
		/* [한국어] 엔드포인트: 부모 브리지가 핫플러그 슬롯이면 슬롯 풀에서 size만큼 새 할당. */
		if (bus->self->hotplug_capable) {
			return vmd_hotplug_allocate_base_addr(&bus->self->hp, size);
		} else {
			/* [한국어] 일반 슬롯이면 부모 브리지 mem_base(16비트)를 32비트 주소로 복원(<<16). */
			return (uint64_t)bus->self->header->one.mem_base << 16;
		}
	}
}

/*
 * [한국어]
 * vmd_assign_base_addrs - 디바이스의 모든 BAR 크기를 probe하고 membar에서 주소를 할당·기록
 *
 * @dev: BAR을 할당할 VMD PCI 디바이스(엔드포인트 또는 브리지)
 * @return: 하나 이상의 BAR을 성공적으로 할당했으면 true, 아니면 false
 *
 * OS/BIOS가 했어야 할 PCI 자원 할당을 SPDK가 직접 수행하는 핵심 함수다. 각 BAR마다
 * "전부 1을 써 보고 읽어서 크기를 알아내는" 표준 PCI BAR sizing 절차를 수행한 뒤,
 * membar 윈도(또는 핫플러그 슬롯)에서 크기-정렬된 물리주소를 할당해 config BAR에 다시
 * 기록한다. 동시에 그 물리주소를 VMD가 매핑한 가상주소(mem_vaddr 기준 오프셋)로 환산해
 * dev->bar[i].vaddr에 보관한다. 64비트 prefetch BAR은 두 DWORD를 차지하므로 다음 BAR
 * 인덱스에 상위 32비트를 기록하고 i를 건너뛴다. 마지막에 디바이스 MEM/Bus Master를
 * 활성화하고, MSI-X 테이블 위치를 계산하며, 엔드포인트면 상위 브리지 윈도를 확장한다.
 * 스캔/attach 단일 스레드 컨텍스트(MMIO config 직접 접근).
 *
 * 호출 체인:
 *   vmd_init_end_device() → [vmd_assign_base_addrs] → vmd_allocate_base_addr/
 *   vmd_get_base_addr/vmd_update_base_limit_register
 */
static bool
vmd_assign_base_addrs(struct vmd_pci_device *dev)
{
	uint16_t mem_base = 0, mem_limit = 0;   /* [한국어] 이 디바이스 BAR들을 덮는 브리지 윈도 base/limit(16비트). */
	unsigned char mem_attr = 0;             /* [한국어] BAR 하위 플래그(prefetch/64비트 타입 등) 보관. */
	int last;                               /* [한국어] 검사할 BAR 개수(엔드포인트 6, 브리지 2). */
	struct vmd_adapter *vmd = NULL;         /* [한국어] dev가 속한 VMD 어댑터(할당 커서 보유). */
	bool ret_val = false;                   /* [한국어] BAR 할당 성공 여부 누적(하나라도 성공하면 true). */
	uint32_t bar_value;                     /* [한국어] BAR 원래 값 임시 보관(sizing 중 복원용). */
	uint32_t table_offset;                  /* [한국어] MSI-X 테이블 오프셋(BIR+오프셋 인코딩). */

	/* [한국어] dev와 그 버스가 유효할 때만 어댑터 포인터 확보. */
	if (dev && dev->bus) {
		vmd = dev->bus->vmd;
	}

	/* [한국어] 어댑터가 없으면 할당할 membar도 없음 → 실패(0). */
	if (!vmd) {
		return 0;
	}

	vmd_align_base_addrs(vmd, ONE_MB);   /* [한국어] 새 디바이스 BAR 배치 전 커서를 1MB 경계로 정렬. */

	/* [한국어] 헤더 타입이 1(브리지)이면 BAR 2개, 0(엔드포인트)이면 6개를 순회. */
	last = dev->header_type ? 2 : 6;
	for (int i = 0; i < last; i++) {
		/* --- 표준 PCI BAR sizing: 원값 저장 → 전부1 기록 → 읽어 크기 파악 → 원값 복원 --- */
		bar_value = dev->header->zero.BAR[i];        /* [한국어] BAR 원래 값 백업. */
		dev->header->zero.BAR[i] = ~(0U);            /* [한국어] 전부 1을 써서 디바이스가 지원 크기 비트만 남기게 함. */
		dev->bar[i].size = dev->header->zero.BAR[i]; /* [한국어] read-back: 하위 비트들이 0인 마스크가 크기 정보. */
		dev->header->zero.BAR[i] = bar_value;        /* [한국어] BAR 원래 값 복원. */

		/* [한국어] 크기가 전부1(미구현)·0(미사용)이거나 BAR이 I/O 공간(bit0=1)이면 건너뜀(MEM BAR만 처리). */
		if (dev->bar[i].size == ~(0U) || dev->bar[i].size == 0  ||
		    dev->header->zero.BAR[i] & 1) {
			dev->bar[i].size = 0;
			continue;
		}
		mem_attr = dev->bar[i].size & PCI_BASE_ADDR_MASK;   /* [한국어] BAR 타입 플래그(prefetch/64비트) 추출. */
		/* [한국어] 마스크된 크기 비트를 2의 보수로 변환 → 실제 BAR 윈도 바이트 크기 산출. */
		dev->bar[i].size = TWOS_COMPLEMENT(dev->bar[i].size & PCI_BASE_ADDR_MASK);

		/* [한국어] 이미 스캔 완료된 도메인이면 기존/슬롯 주소 조회, 아니면 membar에서 새로 할당. */
		if (vmd->scan_completed) {
			dev->bar[i].start = vmd_get_base_addr(dev, i, dev->bar[i].size);
		} else {
			dev->bar[i].start = vmd_allocate_base_addr(vmd, dev, dev->bar[i].size);
		}

		dev->header->zero.BAR[i] = (uint32_t)dev->bar[i].start;   /* [한국어] 할당한 물리주소 하위32비트를 config BAR에 기록. */

		/* [한국어] 할당 실패(0)면 이 BAR 스킵. 64비트 BAR이었다면 상위 DWORD 슬롯도 함께 건너뜀. */
		if (!dev->bar[i].start) {
			if (mem_attr == (PCI_BAR_MEMORY_PREFETCH | PCI_BAR_MEMORY_TYPE_64)) {
				i++;
			}
			continue;
		}

		/* [한국어] 물리주소를 VMD가 매핑한 가상주소로 환산: mem_vaddr + (물리 - membar 시작). */
		dev->bar[i].vaddr = ((uint64_t)vmd->mem_vaddr + (dev->bar[i].start - vmd->membar));
		/* [한국어] 이 BAR을 덮는 브리지 limit = BAR 시작 + (크기-1)을 16비트 브리지 레지스터 형식으로. */
		mem_limit = BRIDGE_BASEREG(dev->header->zero.BAR[i]) +
			    BRIDGE_BASEREG(dev->bar[i].size - 1);
		/* [한국어] 첫 유효 BAR의 시작을 브리지 base로 채택(가장 낮은 주소). */
		if (!mem_base) {
			mem_base = BRIDGE_BASEREG(dev->header->zero.BAR[i]);
		}

		ret_val = true;   /* [한국어] 최소 하나의 BAR 할당 성공 표시. */

		/* [한국어] 64비트 prefetch BAR은 두 DWORD를 차지 → 다음 슬롯에 물리주소 상위32비트 기록 후 i++. */
		if (mem_attr == (PCI_BAR_MEMORY_PREFETCH | PCI_BAR_MEMORY_TYPE_64)) {
			i++;
			if (i < last) {
				dev->header->zero.BAR[i] = (uint32_t)(dev->bar[i].start >> PCI_DWORD_SHIFT);
			}
		}
	}

	/* Enable device MEM and bus mastering */
	/* [한국어] Command 레지스터에 MEM Space Enable + Bus Master Enable 비트를 세워 DMA/MMIO 허용. */
	dev->header->zero.command |= (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	/*
	 * Writes to the pci config space is posted write. To ensure transaction reaches its destination
	 * before another write is posed, an immediate read of the written value should be performed.
	 */
	/* [한국어] config 쓰기는 posted write이므로, 즉시 read-back으로 트랜잭션이 디바이스에
	 *          도달했음을 보장(다음 쓰기 전에 순서 강제). (void)cmd로 미사용 경고 억제. */
	{ uint16_t cmd = dev->header->zero.command; (void)cmd; }

	/* [한국어] MSI-X capability가 있고 BAR 할당이 성공했으면 MSI-X 테이블의 가상주소를 계산. */
	if (dev->msix_cap && ret_val) {
		table_offset = ((volatile struct pci_msix_cap *)dev->msix_cap)->msix_table_offset;   /* [한국어] BIR(하위3비트)+오프셋. */
		/* [한국어] table_offset 하위3비트가 가리키는 BAR이 매핑돼 있으면 테이블 포인터 산출. */
		if (dev->bar[table_offset & 0x3].vaddr) {
			/* [한국어] 테이블 주소 = 해당 BAR vaddr + (오프셋 하위3비트 제거, 8바이트 정렬: 0xfff8 마스크). */
			dev->msix_table = (volatile struct pci_msix_table_entry *)
					  (dev->bar[table_offset & 0x3].vaddr + (table_offset & 0xfff8));
		}
	}

	/* [한국어] 엔드포인트면 새 BAR 윈도를 상위 브리지 체인의 mem_base/limit에 전파(라우팅 보장). */
	if (ret_val && vmd_is_end_device(dev)) {
		vmd_update_base_limit_register(dev, mem_base, mem_limit);
	}

	return ret_val;   /* [한국어] BAR 할당 성공 여부 반환(호출자 init_end_device가 실패 시 디바이스 폐기). */
}

/*
 * [한국어]
 * vmd_get_device_capabilities - PCI capability 링크드 리스트를 순회하며 PCIe/MSI/MSI-X 캡 위치 캐싱
 *
 * @dev: capability를 탐색할 VMD PCI 디바이스(dev->header가 MMIO config를 가리킴)
 * @return: 없음(dev->pcie_cap / msi_cap / msix_cap / msix_table_size를 부작용으로 채움)
 *
 * PCI config 공간의 capability는 cap_pointer에서 시작해 각 헤더의 next 오프셋으로
 * 연결된 단일 연결 리스트다. 이 함수는 그 리스트를 따라가며 PCIe(0x10)/MSI(0x05)/
 * MSI-X(0x11) capability의 config 내 포인터를 dev 구조체에 캐싱한다. MSI-X는
 * message control의 table_size(0-based)에 +1 해 실제 벡터 수도 저장한다. 이후
 * hotplug 슬롯 제어(pcie_cap), 인터럽트 리맵(msix_cap) 등에 재사용된다. 스캔 단일 스레드.
 *
 * 호출 체인:
 *   vmd_read_config_space() → [vmd_get_device_capabilities]
 */
static void
vmd_get_device_capabilities(struct vmd_pci_device *dev)

{
	volatile uint8_t *config_space;                     /* [한국어] config 공간을 바이트 단위로 인덱싱할 베이스 포인터. */
	uint8_t capabilities_offset;                        /* [한국어] 현재 capability의 config 내 오프셋(바이트). */
	struct pci_capabilities_header *capabilities_hdr;   /* [한국어] 현재 capability 헤더(id + next). */

	config_space = (volatile uint8_t *)dev->header;     /* [한국어] config 베이스 = 헤더 시작(volatile=MMIO 직접 접근). */
	/* [한국어] Status 레지스터의 Capabilities List 비트가 0이면 capability 자체가 없음 → 종료. */
	if ((dev->header->common.status  & PCI_CAPABILITIES_LIST) == 0) {
		return;
	}

	capabilities_offset = dev->header->zero.cap_pointer;   /* [한국어] type-0 헤더의 cap 리스트 시작 포인터. */
	/* [한국어] 브리지(type-1)는 cap_pointer 위치가 다르므로 one.cap_pointer로 교체. */
	if (dev->header->common.header_type & PCI_HEADER_TYPE_BRIDGE) {
		capabilities_offset = dev->header->one.cap_pointer;
	}

	/* [한국어] next 오프셋이 0이 될 때까지 capability 리스트를 따라간다. */
	while (capabilities_offset > 0) {
		capabilities_hdr = (struct pci_capabilities_header *)
				   &config_space[capabilities_offset];   /* [한국어] 현재 오프셋의 capability 헤더. */
		switch (capabilities_hdr->capability_id) {
		case CAPABILITY_ID_PCI_EXPRESS:
			dev->pcie_cap = (volatile struct pci_express_cap *)(capabilities_hdr);   /* [한국어] PCIe 캡 캐싱(슬롯/링크 제어용). */
			break;

		case CAPABILITY_ID_MSI:
			dev->msi_cap = (volatile struct pci_msi_cap *)capabilities_hdr;   /* [한국어] MSI 캡 캐싱. */
			break;

		case CAPABILITY_ID_MSIX:
			dev->msix_cap = (volatile struct pci_msix_capability *)capabilities_hdr;   /* [한국어] MSI-X 캡 캐싱(리맵 대상). */
			dev->msix_table_size = dev->msix_cap->message_control.bit.table_size + 1;   /* [한국어] 벡터 수(0-based+1). */
			break;

		default:
			break;   /* [한국어] 관심 없는 capability는 무시. */
		}
		capabilities_offset = capabilities_hdr->next;   /* [한국어] 다음 capability로 이동(next 오프셋). */
	}
}

/*
 * [한국어]
 * vmd_get_enhanced_capabilities - PCIe extended(enhanced) capability 리스트에서 특정 ID 검색
 *
 * @dev: 검색 대상 디바이스
 * @capability_id: 찾을 extended capability ID(예: Device Serial Number)
 * @return: 해당 capability 헤더 포인터, 없으면 NULL
 *
 * PCIe extended capability는 config 공간 오프셋 0x100(EXTENDED_CAPABILITY_OFFSET)에서
 * 시작하는 별도 연결 리스트다(일반 capability와 다른 영역). next 오프셋을 따라가며
 * 원하는 capability_id를 찾는다. next가 0이거나 0x100 미만이면 리스트 종료/손상으로
 * 보고 중단한다. 주로 디바이스 시리얼 넘버 capability를 찾는 데 쓰인다. 스캔 단일 스레드.
 *
 * 호출 체인:
 *   vmd_read_config_space() → [vmd_get_enhanced_capabilities]
 */
static volatile struct pci_enhanced_capability_header *
vmd_get_enhanced_capabilities(struct vmd_pci_device *dev, uint16_t capability_id)
{
	uint8_t *data;                                  /* [한국어] config 공간 바이트 인덱싱 베이스. */
	uint16_t cap_offset = EXTENDED_CAPABILITY_OFFSET;   /* [한국어] 검색 시작 오프셋(0x100, extended cap 영역). */
	volatile struct pci_enhanced_capability_header *cap_hdr = NULL;   /* [한국어] 현재 extended cap 헤더. */

	data = (uint8_t *)dev->header;   /* [한국어] config 베이스. */
	/* [한국어] cap_offset이 0x100 이상인 동안 리스트를 순회(유효 extended cap 영역). */
	while (cap_offset >= EXTENDED_CAPABILITY_OFFSET) {
		cap_hdr = (volatile struct pci_enhanced_capability_header *) &data[cap_offset];   /* [한국어] 현재 헤더. */
		if (cap_hdr->capability_id == capability_id) {
			return cap_hdr;   /* [한국어] 원하는 ID 발견 → 포인터 반환. */
		}
		cap_offset = cap_hdr->next;   /* [한국어] 다음 extended cap 오프셋으로 이동. */
		/* [한국어] next가 0(끝)이거나 0x100 미만(손상/잘못된 포인터)이면 순회 중단. */
		if (cap_offset == 0 || cap_offset < EXTENDED_CAPABILITY_OFFSET) {
			break;
		}
	}

	return NULL;   /* [한국어] 해당 capability를 못 찾음. */
}

/*
 * [한국어]
 * vmd_read_config_space - 디바이스 config를 초기화: Bus Master/MEM 활성화 + capability 캐싱
 *
 * @dev: 초기화할 VMD PCI 디바이스
 * @return: 없음(command 레지스터 갱신 + dev->pcie/msi/msix/sn_cap 채움)
 *
 * 새로 발견한 디바이스의 config를 처음 만질 때 호출된다. Command 레지스터에 Bus Master
 * Enable + Memory Space Enable 비트를 세워 DMA/MMIO를 허용하고(posted write이므로 즉시
 * read-back으로 순서 보장), 일반 capability(PCIe/MSI/MSI-X)와 extended capability(시리얼
 * 넘버)를 찾아 dev 구조체에 캐싱한다. 스캔 단일 스레드 컨텍스트(MMIO config 직접 접근).
 *
 * 호출 체인:
 *   vmd_alloc_dev() → [vmd_read_config_space] → vmd_get_device_capabilities/vmd_get_enhanced_capabilities
 */
static void
vmd_read_config_space(struct vmd_pci_device *dev)
{
	/*
	 * Writes to the pci config space is posted weite. To ensure transaction reaches its destination
	 * before another write is posed, an immediate read of the written value should be performed.
	 */
	/* [한국어] Command에 Bus Master Enable + Memory Space Enable 비트 설정(DMA/MMIO 허용). */
	dev->header->common.command |= (BUS_MASTER_ENABLE | MEMORY_SPACE_ENABLE);
	/* [한국어] posted write 순서 보장을 위한 즉시 read-back(미사용 경고 억제). */
	{ uint16_t cmd = dev->header->common.command; (void)cmd; }

	vmd_get_device_capabilities(dev);   /* [한국어] PCIe/MSI/MSI-X 일반 capability 캐싱. */
	/* [한국어] extended capability에서 디바이스 시리얼 넘버 cap을 찾아 sn_cap에 저장(로깅용). */
	dev->sn_cap = (struct serial_number_capability *)vmd_get_enhanced_capabilities(dev,
			DEVICE_SERIAL_NUMBER_CAP_ID);
}

/*
 * [한국어]
 * vmd_update_scan_info - 첫 루트 포트를 만나 도메인이 이미 enumerate되었는지 한 번 판정
 *
 * @dev: 방금 alloc된 디바이스(브리지일 때만 의미)
 * @return: 없음(어댑터의 root_port_updated / scan_completed 플래그 설정)
 *
 * VMD 도메인을 처음 스캔할 때, 최초로 발견한 루트 포트를 보고 이 도메인이 (이전 드라이버
 * 등에 의해) 이미 열거된 상태인지 판단한다. 루트 포트의 prefetch upper 레지스터에 SPDK
 * 시그니처가 박혀 있으면(vmd_device_is_enumerated) scan_completed=1로 표시해, 이후 BAR
 * 할당이 새 할당(allocate) 대신 기존 값 조회(get_base_addr) 경로를 타게 한다. root_port_
 * updated 플래그로 이 판정을 도메인당 한 번만 수행한다. 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_alloc_dev() → [vmd_update_scan_info] → vmd_device_is_root_port/vmd_device_is_enumerated
 */
static void
vmd_update_scan_info(struct vmd_pci_device *dev)
{
	struct vmd_adapter *vmd_adapter = dev->bus->vmd;   /* [한국어] dev가 속한 VMD 어댑터. */

	/* [한국어] 이미 첫 루트 포트를 처리했다면 다시 판정하지 않음(도메인당 1회). */
	if (vmd_adapter->root_port_updated) {
		return;
	}

	/* [한국어] 엔드포인트(type-0)는 루트 포트가 아니므로 판정 대상 아님. */
	if (dev->header_type == PCI_HEADER_TYPE_NORMAL) {
		return;
	}

	/* [한국어] 이 브리지가 VMD 루트 포트면 판정 수행. */
	if (vmd_device_is_root_port(dev->header)) {
		vmd_adapter->root_port_updated = 1;   /* [한국어] 첫 루트 포트 처리 완료 표시. */
		SPDK_INFOLOG(vmd, "root_port_updated = %d\n",
			     vmd_adapter->root_port_updated);
		SPDK_INFOLOG(vmd, "upper:limit = %x : %x\n",
			     dev->header->one.prefetch_base_upper,
			     dev->header->one.prefetch_limit_upper);   /* [한국어] 시그니처 레지스터 현재값 로깅. */
		/* [한국어] prefetch upper에 SPDK 시그니처가 있으면 이미 열거된 도메인 → scan_completed. */
		if (vmd_device_is_enumerated(dev->header)) {
			vmd_adapter->scan_completed = 1;   /* [한국어] 이후 BAR은 재할당하지 않고 기존 값을 따른다. */
			SPDK_INFOLOG(vmd, "scan_completed = %d\n",
				     vmd_adapter->scan_completed);
		}
	}
}

/*
 * [한국어]
 * vmd_reset_base_limit_registers - 브리지의 base/limit/bus 번호 레지스터를 초기 상태로 클리어
 *
 * @header: 리셋할 브리지(type-1)의 PCI config 헤더
 * @return: 없음(config 레지스터들을 직접 0/초기값으로 기록)
 *
 * 이전 드라이버(예: Linux 커널)가 설정해 둔 stale한 브리지 윈도/버스 번호를 깨끗이
 * 지워, SPDK가 깊이우선 스캔을 시작하기 전 충돌(같은 secondary/subordinate 중복) 없이
 * 새로 배치할 수 있게 한다. mem_base는 limit보다 크게(0xfff0) 두어 "빈 윈도"로 만들고,
 * prefetch/io upper와 primary/secondary/subordinate 버스 번호를 모두 0으로 초기화한다.
 * 각 쓰기 직후 read-back(reg=...)으로 posted write 순서를 보장한다(reg는 unused).
 * 스캔 직전 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_alloc_dev()/vmd_reset_root_ports() → [vmd_reset_base_limit_registers]
 */
static void
vmd_reset_base_limit_registers(volatile struct pci_header *header)
{
	uint32_t reg __attribute__((unused));   /* [한국어] read-back 결과를 받는 더미(순서 보장용, 미사용 경고 억제). */

	/*
	 * Writes to the pci config space are posted writes.
	 * To ensure transaction reaches its destination
	 * before another write is posted, an immediate read
	 * of the written value should be performed.
	 */
	header->one.mem_base = 0xfff0;            /* [한국어] base를 limit보다 크게 → 빈(비활성) 메모리 윈도. */
	reg = header->one.mem_base;               /* [한국어] posted write flush(read-back). */
	header->one.mem_limit = 0x0;              /* [한국어] limit 0으로 윈도 폐쇄. */
	reg = header->one.mem_limit;
	header->one.prefetch_base = 0x0;          /* [한국어] prefetchable 메모리 윈도 base 클리어. */
	reg = header->one.prefetch_base;
	header->one.prefetch_limit = 0x0;         /* [한국어] prefetchable limit 클리어. */
	reg = header->one.prefetch_limit;
	header->one.prefetch_base_upper = 0x0;    /* [한국어] prefetch base 상위32비트 클리어(시그니처도 제거). */
	reg = header->one.prefetch_base_upper;
	header->one.prefetch_limit_upper = 0x0;   /* [한국어] prefetch limit 상위32비트 클리어. */
	reg = header->one.prefetch_limit_upper;
	header->one.io_base_upper = 0x0;          /* [한국어] I/O base 상위16비트 클리어. */
	reg = header->one.io_base_upper;
	header->one.io_limit_upper = 0x0;         /* [한국어] I/O limit 상위16비트 클리어. */
	reg = header->one.io_limit_upper;
	header->one.primary = 0;                  /* [한국어] primary 버스 번호 초기화(스캔이 새로 채움). */
	reg = header->one.primary;
	header->one.secondary = 0;                /* [한국어] secondary 버스 번호 초기화. */
	reg = header->one.secondary;
	header->one.subordinate = 0;              /* [한국어] subordinate 버스 번호 초기화. */
	reg = header->one.subordinate;
}

/*
 * [한국어]
 * vmd_init_hotplug - 핫플러그 가능 브리지에 전용 1MB 메모리 윈도와 free-list 관리자를 셋업
 *
 * @dev: 핫플러그 슬롯을 가진 브리지 디바이스(여기 dev->hp가 초기화됨)
 * @bus: 그 브리지의 하위 버스(bus->self == dev, bus->vmd로 어댑터 접근)
 * @return: 없음(dev->hp의 BAR 윈도와 free/unused/alloc 큐를 초기화)
 *
 * 핫플러그 슬롯에 나중에 삽입될 SSD의 BAR을 담을 1MB 예약 메모리 윈도를 미리 확보한다.
 * scan_completed가 아니면 membar에서 1MB를 새로 할당하고 브리지의 mem_base/limit에
 * 기록하며, 이미 열거된 도메인이면 브리지에 적힌 mem_base를 그대로 읽는다. 그 윈도를
 * pci_mem_mgr 디스크립터 배열(hp->mem[])로 관리: mem[0]이 전체 윈도를 나타내며 free
 * 큐에 들어가고, 나머지는 분할용 예비 디스크립터로 unused 큐에 들어간다. 이 자료구조가
 * vmd_hotplug_allocate/free_base_addr의 first-fit 풀이 된다. 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_scan_single_bus() → [vmd_init_hotplug] → vmd_allocate_base_addr
 */
static void
vmd_init_hotplug(struct vmd_pci_device *dev, struct vmd_pci_bus *bus)
{
	struct vmd_adapter *vmd = bus->vmd;        /* [한국어] 어댑터(membar 윈도/매핑 보유). */
	struct vmd_hot_plug *hp = &dev->hp;        /* [한국어] 초기화할 핫플러그 메모리 관리자. */
	size_t mem_id;                             /* [한국어] hp->mem[] 디스크립터 순회 인덱스. */

	dev->hotplug_capable = true;               /* [한국어] 이 브리지를 핫플러그 가능으로 표시. */
	hp->bar.size = 1 << 20;                    /* [한국어] 핫플러그 슬롯 예약 메모리 윈도 = 1MB. */

	/* [한국어] 아직 열거 안 됐으면 membar에서 1MB 새로 할당하고 브리지 윈도를 그 범위로 설정. */
	if (!vmd->scan_completed) {
		hp->bar.start = vmd_allocate_base_addr(vmd, NULL, hp->bar.size);   /* [한국어] membar에서 1MB 확보. */
		bus->self->header->one.mem_base = BRIDGE_BASEREG(hp->bar.start);   /* [한국어] 브리지 base = 윈도 시작. */
		bus->self->header->one.mem_limit =
			bus->self->header->one.mem_base + BRIDGE_BASEREG(hp->bar.size - 1);   /* [한국어] limit = base + (1MB-1). */
	} else {
		/* [한국어] 이미 열거된 도메인이면 브리지에 적힌 mem_base(16비트)를 32비트 주소로 복원. */
		hp->bar.start = (uint64_t)bus->self->header->one.mem_base << 16;
	}

	hp->bar.vaddr = (uint64_t)vmd->mem_vaddr + (hp->bar.start - vmd->membar);   /* [한국어] 윈도 시작의 가상주소 환산. */

	TAILQ_INIT(&hp->free_mem_queue);     /* [한국어] 가용 영역 큐 초기화. */
	TAILQ_INIT(&hp->unused_mem_queue);   /* [한국어] 분할용 예비 디스크립터 큐 초기화. */
	TAILQ_INIT(&hp->alloc_mem_queue);    /* [한국어] 할당 중 영역 큐 초기화. */

	hp->mem[0].size = hp->bar.size;      /* [한국어] 0번 디스크립터가 전체 1MB 윈도를 표현. */
	hp->mem[0].addr = hp->bar.start;     /* [한국어] 0번 디스크립터 시작주소 = 윈도 시작. */

	TAILQ_INSERT_TAIL(&hp->free_mem_queue, &hp->mem[0], tailq);   /* [한국어] 전체 윈도를 free 큐에 등록. */

	/* [한국어] 1번부터 끝까지는 빈 예비 디스크립터로 unused 큐에 넣어 분할 시 재사용. */
	for (mem_id = 1; mem_id < ADDR_ELEM_COUNT; ++mem_id) {
		TAILQ_INSERT_TAIL(&hp->unused_mem_queue, &hp->mem[mem_id], tailq);
	}

	SPDK_INFOLOG(vmd, "%s: mem_base:mem_limit = %x : %x\n", __func__,
		     bus->self->header->one.mem_base, bus->self->header->one.mem_limit);   /* [한국어] 설정 결과 로깅. */
}

/*
 * [한국어]
 * vmd_bus_device_present - 주어진 버스의 특정 device/function 슬롯에 디바이스가 존재하는지 검사
 *
 * @bus: 검사할 VMD 버스(config_bus_number로 config 오프셋 계산)
 * @devfn: device 번호(이 코드는 function 0만 다룸)
 * @return: 유효한 vendor_id를 읽으면 true(존재), 아니면 false(빈 슬롯)
 *
 * VMD config BAR 내에서 (bus, dev, fn) 좌표의 config 헤더 가상주소를 CONFIG_OFFSET_ADDR
 * 매크로로 계산하고, 먼저 그 주소가 매핑 범위 안인지 확인한 뒤 vendor_id를 읽는다.
 * vendor_id가 0xFFFF(미응답) 또는 0이면 빈 슬롯으로 판정한다. 빈 슬롯의 MMIO를 잘못
 * 역참조하면 머신 체크/SIGBUS가 날 수 있으므로 경계 검사가 선행된다. 스캔/핫플러그
 * 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_alloc_dev()/vmd_reset_root_ports()/vmd_bus_handle_hotremove() → [이 함수]
 */
static bool
vmd_bus_device_present(struct vmd_pci_bus *bus, uint32_t devfn)
{
	volatile struct pci_header *header;   /* [한국어] (bus,devfn,0,0) 좌표의 config 헤더 포인터. */

	/* [한국어] config BAR 시작 + (bus,dev,fn,reg) 오프셋으로 헤더 가상주소 계산. */
	header = (volatile struct pci_header *)(bus->vmd->cfg_vaddr +
						CONFIG_OFFSET_ADDR(bus->config_bus_number, devfn, 0, 0));
	/* [한국어] 계산된 주소가 매핑 범위를 벗어나면 잘못된 MMIO 접근 방지 위해 부재로 처리. */
	if (!vmd_is_valid_cfg_addr(bus, (uint64_t)header)) {
		return false;
	}

	/* [한국어] vendor_id가 0xFFFF(미응답) 또는 0이면 슬롯이 비어 있음. */
	if (header->common.vendor_id == PCI_INVALID_VENDORID || header->common.vendor_id == 0x0) {
		return false;
	}

	return true;   /* [한국어] 유효 vendor_id → 디바이스 존재. */
}

/*
 * [한국어]
 * vmd_alloc_dev - 버스의 특정 슬롯에 존재하는 디바이스를 위한 vmd_pci_device 객체 생성·초기화
 *
 * @bus: 디바이스가 매달릴 VMD 버스
 * @devfn: device 번호(0~31)
 * @return: 새로 할당·초기화된 vmd_pci_device, 슬롯이 비었거나 중복/실패면 NULL
 *
 * VMD config 공간에서 (bus, devfn) 슬롯의 헤더를 읽어 vmd_pci_device를 calloc하고 기본
 * 필드(header/vid/did/bus/parent/devfn/class/header_type)를 채운다. 같은 dev/fn이 이미
 * 등록돼 있거나 슬롯이 비어 있으면 NULL을 반환한다. 디바이스가 브리지면 스캔 완료 여부를
 * 판정(update_scan_info)하고, 아직 열거 전이라면 stale한 base/limit 레지스터를 리셋한다.
 * 마지막으로 config를 초기화(Bus Master/MEM 활성화 + capability 캐싱)한다. 스캔/attach
 * 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_scan_single_bus()/vmd_attach_device() → [vmd_alloc_dev] →
 *   vmd_update_scan_info/vmd_reset_base_limit_registers/vmd_read_config_space
 */
static struct vmd_pci_device *
vmd_alloc_dev(struct vmd_pci_bus *bus, uint32_t devfn)
{
	struct vmd_pci_device *dev = NULL;     /* [한국어] 새로 만들 디바이스 객체(중복 검사에도 재사용). */
	struct pci_header volatile *header;    /* [한국어] 슬롯의 config 헤더(MMIO). */
	uint8_t header_type;                   /* [한국어] 헤더 타입 원값(multi-function 비트 포함). */
	uint32_t rev_class;                    /* [한국어] revision+class code 묶음 레지스터. */

	/* Make sure we're not creating two devices on the same dev/fn */
	/* [한국어] 같은 devfn에 이미 디바이스가 있으면 중복 생성 방지 위해 NULL 반환. */
	TAILQ_FOREACH(dev, &bus->dev_list, tailq) {
		if (dev->devfn == devfn) {
			return NULL;
		}
	}

	/* [한국어] 슬롯이 비어 있으면(존재 검사 실패) 생성하지 않음. */
	if (!vmd_bus_device_present(bus, devfn)) {
		return NULL;
	}

	/* [한국어] 슬롯의 config 헤더 가상주소 계산(존재가 확인된 상태). */
	header = (struct pci_header * volatile)(bus->vmd->cfg_vaddr +
						CONFIG_OFFSET_ADDR(bus->config_bus_number, devfn, 0, 0));

	SPDK_INFOLOG(vmd, "PCI device found: %04x:%04x ***\n",
		     header->common.vendor_id, header->common.device_id);   /* [한국어] 발견된 디바이스 로깅. */

	dev = calloc(1, sizeof(*dev));   /* [한국어] 디바이스 객체 0-초기화 할당. */
	if (!dev) {
		return NULL;   /* [한국어] 메모리 부족 → 실패. */
	}

	dev->header = header;                         /* [한국어] config 헤더(MMIO) 포인터 보관. */
	dev->vid = dev->header->common.vendor_id;     /* [한국어] vendor id 캐싱. */
	dev->did = dev->header->common.device_id;     /* [한국어] device id 캐싱. */
	dev->bus = bus;                               /* [한국어] 소속 버스. */
	dev->parent = bus;                            /* [한국어] 부모 버스(주소 할당 시 핫플러그 판별에 사용). */
	dev->devfn = devfn;                           /* [한국어] device 번호. */
	header_type = dev->header->common.header_type;   /* [한국어] 헤더 타입 원값 읽기. */
	rev_class = dev->header->common.rev_class;       /* [한국어] revision/class 레지스터 읽기. */
	dev->class = rev_class >> 8;                  /* [한국어] 상위 24비트가 class code(하위 8비트 revision 제거). */
	dev->header_type = header_type & 0x7;         /* [한국어] 헤더 타입 하위 3비트만(0=Normal,1=Bridge). */

	/* [한국어] 브리지면 스캔 상태 판정 후, 아직 열거 전이면 stale 레지스터를 리셋. */
	if (header_type == PCI_HEADER_TYPE_BRIDGE) {
		vmd_update_scan_info(dev);   /* [한국어] 첫 루트 포트라면 scan_completed 판정. */
		if (!dev->bus->vmd->scan_completed) {
			vmd_reset_base_limit_registers(dev->header);   /* [한국어] 이전 드라이버가 남긴 설정 클리어. */
		}
	}

	vmd_read_config_space(dev);   /* [한국어] command 활성화 + capability 캐싱. */

	return dev;   /* [한국어] 초기화된 디바이스 반환. */
}

/*
 * [한국어]
 * vmd_create_new_bus - 브리지 하위에 새 PCI 버스 객체를 생성하고 브리지와 양방향 연결
 *
 * @parent: 새 버스의 부모 버스(도메인/vmd 상속원)
 * @bridge: 이 새 버스를 secondary로 갖는 브리지 디바이스
 * @bus_number: 새 버스에 부여할 논리 버스 번호
 * @return: 초기화된 vmd_pci_bus, 할당 실패 시 NULL
 *
 * 브리지 아래로 내려가며 재귀 스캔할 때, 그 브리지의 secondary 버스를 표현하는
 * vmd_pci_bus를 만든다. parent로부터 domain/vmd를 상속하고, config_bus_number를
 * (논리 버스 - vmd_bus.bus_start)로 계산하며(config BAR 오프셋 산출용), 브리지와
 * 새 버스를 self/subordinate로 상호 연결한다. 또한 브리지의 spdk_pci addr(bus/dev/
 * func/domain)도 채워 SPDK PCI 좌표로 노출한다. 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_scan_single_bus() → [vmd_create_new_bus]
 */
static struct vmd_pci_bus *
vmd_create_new_bus(struct vmd_pci_bus *parent, struct vmd_pci_device *bridge, uint8_t bus_number)
{
	struct vmd_pci_bus *new_bus;   /* [한국어] 생성할 새 버스 객체. */

	new_bus = calloc(1, sizeof(*new_bus));   /* [한국어] 0-초기화 할당. */
	if (!new_bus) {
		return NULL;   /* [한국어] 메모리 부족 → 실패. */
	}

	new_bus->parent = parent;                   /* [한국어] 부모 버스 연결. */
	new_bus->domain = parent->domain;           /* [한국어] 도메인 상속. */
	new_bus->bus_number = bus_number;           /* [한국어] 논리 버스 번호 설정. */
	new_bus->secondary_bus = new_bus->subordinate_bus = bus_number;   /* [한국어] 초기 secondary=subordinate=자신. */
	new_bus->self = bridge;                     /* [한국어] 이 버스의 상위 브리지. */
	new_bus->vmd = parent->vmd;                 /* [한국어] 어댑터 상속. */
	new_bus->config_bus_number = new_bus->bus_number - new_bus->vmd->vmd_bus.bus_start;   /* [한국어] config 오프셋용 0-base 버스 번호. */
	TAILQ_INIT(&new_bus->dev_list);             /* [한국어] 이 버스의 디바이스 리스트 초기화. */

	bridge->subordinate = new_bus;              /* [한국어] 브리지 → 하위 버스 역참조. */

	bridge->pci.addr.bus = new_bus->bus_number; /* [한국어] 브리지의 SPDK PCI 좌표(bus). */
	bridge->pci.addr.dev = bridge->devfn;       /* [한국어] dev = devfn. */
	bridge->pci.addr.func = 0;                  /* [한국어] VMD는 항상 function 0. */
	bridge->pci.addr.domain = parent->vmd->pci->addr.domain;   /* [한국어] domain은 VMD 엔드포인트 domain 상속. */

	return new_bus;   /* [한국어] 초기화된 새 버스 반환. */
}

/*
 * [한국어]
 * vmd_get_next_bus_number - 일반(비핫플러그) 브리지에 부여할 다음 논리 버스 번호 할당
 *
 * @vmd: 버스 번호 커서(next_bus_number)와 상한(max_pci_bus)을 보유한 어댑터
 * @return: 사용 가능한 버스 번호, 소진 시 0xff
 *
 * 깊이우선 스캔에서 새 브리지를 만날 때마다 그 secondary 버스에 부여할 번호를 단조
 * 증가 커서에서 하나 떼어 준다. 커서가 max_pci_bus에 도달하면 더 줄 번호가 없으므로
 * 0xff(무효)를 반환해 호출자가 스캔을 중단하게 한다. 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_scan_single_bus() → [vmd_get_next_bus_number]
 */
static uint8_t
vmd_get_next_bus_number(struct vmd_adapter *vmd)
{
	uint8_t bus = 0xff;   /* [한국어] 기본값 0xff = 할당 실패(버스 소진). */

	/* [한국어] 커서+1이 상한 미만일 때만 현재 커서를 내주고 커서를 전진. */
	if ((vmd->next_bus_number + 1) < vmd->max_pci_bus) {
		bus = vmd->next_bus_number;
		vmd->next_bus_number++;
	}

	return bus;   /* [한국어] 할당된 버스 번호 또는 0xff. */
}

/*
 * [한국어]
 * vmd_get_hotplug_bus_numbers - 핫플러그 슬롯에 미리 예약할 버스 번호 묶음 크기 반환
 *
 * @dev: 핫플러그 슬롯을 가진 브리지 디바이스
 * @return: 예약된 버스 개수(RESERVED_HOTPLUG_BUSES), 공간 부족 시 0xff
 *
 * 핫플러그로 나중에 삽입될 디바이스(특히 그 아래 또 브리지가 있을 수 있는 SSD)에게
 * 버스 번호를 동적으로 줄 수 있도록, 슬롯마다 RESERVED_HOTPLUG_BUSES개의 버스 번호를
 * 미리 비워 둔다. 남은 버스 범위가 부족하면 0xff. 어댑터의 next_bus_number 커서를
 * 그만큼 전진시킨다. 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_scan_single_bus() → [vmd_get_hotplug_bus_numbers]
 */
static uint8_t
vmd_get_hotplug_bus_numbers(struct vmd_pci_device *dev)
{
	uint8_t bus_number = 0xff;   /* [한국어] 기본 0xff = 예약 공간 부족. */

	/* [한국어] dev 체인이 유효하고 커서+예약수가 상한 미만이면 RESERVED_HOTPLUG_BUSES만큼 예약. */
	if (dev && dev->bus && dev->bus->vmd &&
	    ((dev->bus->vmd->next_bus_number + RESERVED_HOTPLUG_BUSES) < dev->bus->vmd->max_pci_bus)) {
		bus_number = RESERVED_HOTPLUG_BUSES;                       /* [한국어] 예약 개수 반환값. */
		dev->bus->vmd->next_bus_number += RESERVED_HOTPLUG_BUSES;  /* [한국어] 커서를 예약분만큼 전진. */
	}

	return bus_number;   /* [한국어] 예약 버스 개수 또는 0xff. */
}

/*
 * [한국어]
 * vmd_enable_msix - 디바이스 MSI-X를 정해진 시퀀스로 활성화(Function Mask 해제 포함)
 *
 * @dev: MSI-X capability를 가진 디바이스(dev->msix_cap)
 * @return: 없음(Message Control 레지스터를 단계적으로 기록)
 *
 * VMD는 하위 디바이스의 MSI-X를 자신의 벡터 0로 리맵하므로, 테이블 엔트리 설정 전후로
 * Message Control의 Function Mask(bit14)와 MSI-X Enable(bit15)을 특정 순서로 토글해야
 * 한다. 이 함수는 (1) Function Mask 설정 → (2) MSI-X Enable 설정 → (3) Function Mask
 * 해제 순으로 진행하며, 각 단계 후 read-back으로 posted write 순서를 보장한다. 스캔
 * 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_setup_msix() → [vmd_enable_msix]
 */
static void
vmd_enable_msix(struct vmd_pci_device *dev)
{
	volatile uint16_t control;   /* [한국어] Message Control 레지스터 임시값(read-back 포함). */

	control = dev->msix_cap->message_control.as_uint16_t | (1 << 14);   /* [한국어] Function Mask(bit14) 설정값 준비. */
	dev->msix_cap->message_control.as_uint16_t = control;               /* [한국어] Function Mask 설정 기록. */
	control = dev->msix_cap->message_control.as_uint16_t;               /* [한국어] read-back(순서 보장). */
	dev->msix_cap->message_control.as_uint16_t = (control | (1 << 15)); /* [한국어] MSI-X Enable(bit15) 설정. */
	control = dev->msix_cap->message_control.as_uint16_t;               /* [한국어] read-back. */
	control = control & ~(1 << 14);                                     /* [한국어] Function Mask 해제값 준비. */
	dev->msix_cap->message_control.as_uint16_t = control;              /* [한국어] Function Mask 해제 기록(인터럽트 전달 허용). */
	control = dev->msix_cap->message_control.as_uint16_t;              /* [한국어] read-back. */
}

/*
 * [한국어]
 * vmd_disable_msix - 디바이스 MSI-X를 정해진 시퀀스로 비활성화(Enable 비트 클리어)
 *
 * @dev: MSI-X capability를 가진 디바이스
 * @return: 없음(Message Control 레지스터를 단계적으로 기록)
 *
 * 테이블 엔트리를 안전하게 재구성하기 전에 인터럽트를 멈추기 위해, Function Mask를
 * 먼저 설정해 모든 벡터를 가린 뒤 MSI-X Enable(bit15)을 클리어한다. 각 단계 후
 * read-back으로 순서를 보장한다. vmd_setup_msix()가 테이블을 다시 채우기 전 호출한다.
 * 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_setup_msix() → [vmd_disable_msix]
 */
static void
vmd_disable_msix(struct vmd_pci_device *dev)
{
	volatile uint16_t control;   /* [한국어] Message Control 임시값. */

	control = dev->msix_cap->message_control.as_uint16_t | (1 << 14);   /* [한국어] Function Mask(bit14) 설정값 준비. */
	dev->msix_cap->message_control.as_uint16_t = control;               /* [한국어] 모든 벡터 마스킹. */
	control = dev->msix_cap->message_control.as_uint16_t & ~(1 << 15);  /* [한국어] MSI-X Enable(bit15) 클리어값 준비. */
	dev->msix_cap->message_control.as_uint16_t = control;              /* [한국어] MSI-X 비활성화 기록. */
	control = dev->msix_cap->message_control.as_uint16_t;              /* [한국어] read-back(순서 보장). */
}

/*
 * Set up MSI-X table entries for the port. Vmd MSIX vector 0 is used for
 * port interrupt, so vector 0 is mapped to all MSIX entries for the port.
 */
/*
 * [한국어]
 * vmd_setup_msix - 디바이스 MSI-X 테이블의 모든 엔트리를 마스킹하고 MSI-X를 재활성화
 *
 * @dev: MSI-X를 설정할 디바이스
 * @vmdEntry: VMD의 MSI-X 테이블 벡터 0 엔트리(현재는 유효성 검사에만 사용)
 * @return: 없음(dev->msix_table 엔트리들의 vector_control 설정 + enable)
 *
 * VMD 뒤의 디바이스 인터럽트는 모두 VMD의 단일 벡터 0으로 모인다. 따라서 디바이스 자체
 * 테이블 엔트리는 직접 시스템에 전달되지 않도록 vector_control bit0(Mask)를 1로 세워
 * 마스킹해 둔다. 안전하게 테이블을 수정하기 위해 먼저 MSI-X를 disable한 뒤, 테이블
 * 포인터가 유효하고 크기가 한도 이내일 때만 전 엔트리를 마스킹하고, 마지막에 다시
 * enable한다. 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_init_end_device() → [vmd_setup_msix] → vmd_disable_msix/vmd_enable_msix
 */
static void
vmd_setup_msix(struct vmd_pci_device *dev, volatile struct pci_msix_table_entry *vmdEntry)
{
	int entry;   /* [한국어] MSI-X 테이블 엔트리 순회 인덱스. */

	/* [한국어] dev/엔트리/MSI-X cap 중 하나라도 없으면 MSI-X 설정 불가 → 종료. */
	if (!dev || !vmdEntry || !dev->msix_cap) {
		return;
	}

	vmd_disable_msix(dev);   /* [한국어] 테이블 수정 전 인터럽트를 멈추기 위해 비활성화. */
	/* [한국어] 테이블 포인터가 없거나 크기가 한도를 초과하면 손대지 않음(안전). */
	if (dev->msix_table == NULL || dev->msix_table_size > MAX_MSIX_TABLE_SIZE) {
		return;
	}

	/* [한국어] 모든 엔트리의 vector_control bit0(Mask)을 1로 세워 디바이스 인터럽트 마스킹. */
	for (entry = 0; entry < dev->msix_table_size; ++entry) {
		dev->msix_table[entry].vector_control = 1;
	}
	vmd_enable_msix(dev);   /* [한국어] 설정 완료 후 MSI-X 재활성화. */
}

/*
 * [한국어]
 * vmd_bus_update_bridge_info - 새 브리지의 subordinate 버스 번호를 모든 상위 브리지에 전파
 *
 * @bridge: subordinate 값이 갱신된 하위 브리지
 * @return: 없음(상위 브리지들의 config subordinate 레지스터를 갱신)
 *
 * PCI 브리지의 subordinate 버스 번호는 자기 하위 트리의 최대 버스 번호여야 라우팅이
 * 동작한다. 깊이우선으로 새 버스를 추가하면 상위 브리지들의 subordinate도 그만큼 늘어나야
 * 하므로, parent_bridge 체인을 거슬러 올라가며 현재 subordinate보다 작은 브리지를 모두
 * 갱신한다. 각 단계 read-back으로 전파값을 확정한다. 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_scan_single_bus() → [vmd_bus_update_bridge_info]
 */
static void
vmd_bus_update_bridge_info(struct vmd_pci_device *bridge)
{
	/* Update the subordinate bus of all bridges above this bridge */
	volatile struct vmd_pci_device *dev = bridge;   /* [한국어] 상위로 거슬러 올라갈 커서. */
	uint8_t subordinate_bus;                        /* [한국어] 전파할 subordinate 버스 번호. */

	if (!dev) {
		return;   /* [한국어] 방어적 NULL 체크. */
	}
	subordinate_bus = bridge->header->one.subordinate;   /* [한국어] 시작 브리지의 subordinate. */
	/* [한국어] parent_bridge 체인을 따라 루트 방향으로 올라가며 갱신. */
	while (dev->parent_bridge != NULL) {
		dev = dev->parent_bridge;   /* [한국어] 한 레벨 위 브리지로. */
		/* [한국어] 상위 브리지 subordinate가 더 작으면 하위 트리를 못 덮음 → 키운다. */
		if (dev->header->one.subordinate < subordinate_bus) {
			dev->header->one.subordinate = subordinate_bus;   /* [한국어] config에 기록. */
			subordinate_bus = dev->header->one.subordinate;    /* [한국어] read-back으로 전파값 확정. */
		}
	}
}

/*
 * [한국어]
 * vmd_is_supported_device - 디바이스가 VMD가 후킹할 대상(NVMe Express 스토리지)인지 판별
 *
 * @dev: 검사할 디바이스
 * @return: class code가 PCI_CLASS_STORAGE_EXPRESS(NVMe)면 true
 *
 * VMD 뒤의 엔드포인트 중 SPDK NVMe 드라이버에 hook할 것은 NVMe Express SSD뿐이다.
 * class code(0x010802 등 NVMe)를 비교해 지원 대상만 골라낸다. 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_init_end_device() → [vmd_is_supported_device]
 */
static bool
vmd_is_supported_device(struct vmd_pci_device *dev)
{
	/* [한국어] class가 NVMe Express면 지원 대상. */
	return dev->class == PCI_CLASS_STORAGE_EXPRESS;
}

/*
 * [한국어]
 * vmd_dev_map_bar - SPDK PCI 추상화의 map_bar 콜백: 미리 계산된 BAR 가상/물리주소를 반환
 *
 * @pci_dev: SPDK가 보는 가짜 pci device(vmd_pci_device.pci에 임베드)
 * @bar: 매핑 요청 BAR 인덱스
 * @mapped_addr: [out] BAR의 가상주소
 * @phys_addr: [out] BAR의 물리주소
 * @size: [out] BAR 크기
 * @return: 항상 0(성공)
 *
 * 일반 PCI 디바이스라면 여기서 sysfs resource를 mmap하지만, VMD 뒤 디바이스의 BAR은
 * 이미 vmd_assign_base_addrs()에서 membar 윈도 내부 주소로 할당·매핑돼 있다. 따라서
 * 저장된 dev->bar[bar]의 vaddr/start/size를 그대로 돌려준다. lib/nvme의 PCIe 트랜스포트가
 * NVMe 레지스터 BAR을 매핑할 때 이 콜백이 호출된다. 호스트 유저스페이스, attach 시점.
 *
 * 호출 체인:
 *   spdk_pci_device_map_bar() → dev->pci.map_bar == [vmd_dev_map_bar]
 */
static int
vmd_dev_map_bar(struct spdk_pci_device *pci_dev, uint32_t bar,
		void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	struct vmd_pci_device *dev = SPDK_CONTAINEROF(pci_dev, struct vmd_pci_device, pci);   /* [한국어] 임베드된 pci에서 컨테이너 복원. */

	*size = dev->bar[bar].size;             /* [한국어] 저장된 BAR 크기 반환. */
	*phys_addr = dev->bar[bar].start;       /* [한국어] 저장된 BAR 물리주소 반환. */
	*mapped_addr = (void *)dev->bar[bar].vaddr;   /* [한국어] 저장된 BAR 가상주소 반환(이미 매핑됨). */

	return 0;   /* [한국어] 항상 성공. */
}

/*
 * [한국어]
 * vmd_dev_unmap_bar - SPDK PCI 추상화의 unmap_bar 콜백: VMD에서는 no-op
 *
 * @_dev: SPDK pci device(미사용)
 * @bar: BAR 인덱스(미사용)
 * @addr: 매핑 주소(미사용)
 * @return: 항상 0
 *
 * VMD BAR은 membar 윈도의 부분이라 개별 munmap 대상이 아니며(어댑터 BAR 매핑이 전체를
 * 관리), 따라서 언맵 시 할 일이 없다. 인터페이스 일관성을 위해 빈 콜백을 제공한다.
 * 호스트 유저스페이스, detach 시점.
 *
 * 호출 체인:
 *   spdk_pci_device_unmap_bar() → dev->pci.unmap_bar == [vmd_dev_unmap_bar]
 */
static int
vmd_dev_unmap_bar(struct spdk_pci_device *_dev, uint32_t bar, void *addr)
{
	return 0;   /* [한국어] 할 일 없음(membar 전체 매핑이 별도 관리). */
}

/*
 * [한국어]
 * vmd_dev_cfg_read - SPDK PCI 추상화의 cfg_read 콜백: VMD config 공간을 바이트 복사로 읽기
 *
 * @_dev: SPDK pci device(vmd_pci_device.pci)
 * @value: [out] 읽은 바이트를 담을 버퍼
 * @len: 읽을 바이트 수
 * @offset: config 공간 내 시작 오프셋
 * @return: 0 성공, 범위 초과 시 -1
 *
 * VMD 뒤 디바이스의 config는 커널 sysfs가 아니라 VMD config BAR이 매핑한 MMIO이므로,
 * dev->header(=config MMIO 베이스)에서 offset부터 len바이트를 직접 복사한다. NVMe
 * 드라이버나 사용자 코드가 config를 읽을 때 이 콜백을 탄다. volatile 접근으로 MMIO
 * 읽기를 강제한다. 호스트 유저스페이스, attach 이후.
 *
 * 호출 체인:
 *   spdk_pci_device_cfg_read*() → dev->pci.cfg_read == [vmd_dev_cfg_read]
 */
static int
vmd_dev_cfg_read(struct spdk_pci_device *_dev, void *value, uint32_t len,
		 uint32_t offset)
{
	struct vmd_pci_device *dev = SPDK_CONTAINEROF(_dev, struct vmd_pci_device, pci);   /* [한국어] 컨테이너 복원. */
	volatile uint8_t *src = (volatile uint8_t *)dev->header;   /* [한국어] config MMIO 소스 베이스. */
	uint8_t *dst = value;   /* [한국어] 사용자 버퍼. */
	size_t i;               /* [한국어] 복사 인덱스. */

	/* [한국어] 읽기 범위가 config 공간 크기를 넘으면 실패. */
	if (len + offset > PCI_MAX_CFG_SIZE) {
		return -1;
	}

	/* [한국어] offset부터 len바이트를 MMIO에서 바이트 단위로 복사. */
	for (i = 0; i < len; ++i) {
		dst[i] = src[offset + i];
	}

	return 0;   /* [한국어] 성공. */
}

/*
 * [한국어]
 * vmd_dev_cfg_write - SPDK PCI 추상화의 cfg_write 콜백: VMD config 공간에 바이트 복사로 쓰기
 *
 * @_dev: SPDK pci device(vmd_pci_device.pci)
 * @value: 기록할 바이트 버퍼
 * @len: 기록할 바이트 수
 * @offset: config 공간 내 시작 오프셋
 * @return: 0 성공, 범위 초과 시 -1
 *
 * cfg_read의 쓰기 버전. dev->header가 가리키는 config MMIO의 offset 위치에 len바이트를
 * 직접 기록한다(volatile=MMIO write). NVMe 드라이버가 Command 레지스터 등을 설정할 때
 * 이 콜백을 탄다. 호스트 유저스페이스, attach 이후.
 *
 * 호출 체인:
 *   spdk_pci_device_cfg_write*() → dev->pci.cfg_write == [vmd_dev_cfg_write]
 */
static int
vmd_dev_cfg_write(struct spdk_pci_device *_dev,  void *value,
		  uint32_t len, uint32_t offset)
{
	struct vmd_pci_device *dev = SPDK_CONTAINEROF(_dev, struct vmd_pci_device, pci);   /* [한국어] 컨테이너 복원. */
	volatile uint8_t *dst = (volatile uint8_t *)dev->header;   /* [한국어] config MMIO 목적지 베이스. */
	uint8_t *src = value;   /* [한국어] 사용자 데이터. */
	size_t i;               /* [한국어] 복사 인덱스. */

	/* [한국어] 쓰기 범위가 config 공간 크기를 넘으면 실패. */
	if ((len + offset) > PCI_MAX_CFG_SIZE) {
		return -1;
	}

	/* [한국어] offset부터 len바이트를 MMIO에 바이트 단위로 기록. */
	for (i = 0; i < len; ++i) {
		dst[offset + i] = src[i];
	}

	return 0;   /* [한국어] 성공. */
}

/*
 * [한국어]
 * vmd_dev_free - 디바이스가 점유한 핫플러그 메모리 영역을 반환하고 디바이스 객체 해제
 *
 * @dev: 해제할 VMD PCI 디바이스
 * @return: 없음(dev의 BAR 핫플러그 영역 반환 + free(dev))
 *
 * 디바이스가 핫플러그 가능한 버스 하위에 있었다면, 그 BAR들이 슬롯 메모리 풀에서 받은
 * 영역을 vmd_hotplug_free_addr()로 되돌려 다른 핫플러그 디바이스가 재사용하게 한다.
 * 그 후 객체 메모리를 free한다. 일반(비핫플러그) 디바이스는 membar 선형 할당이라 개별
 * 반환하지 않는다. 핫플러그/detach 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_dev_detach()/vmd_scan_single_bus(실패)/vmd_attach_device(실패) → [vmd_dev_free]
 */
static void
vmd_dev_free(struct vmd_pci_device *dev)
{
	struct vmd_pci_device *bus_device = dev->bus->self;   /* [한국어] 이 디바이스가 매달린 버스의 상위 브리지. */
	size_t i, num_bars = dev->header_type ? 2 : 6;        /* [한국어] BAR 개수(브리지 2, 엔드포인트 6). */

	/* Release the hotplug region if the device is under hotplug-capable bus */
	/* [한국어] 부모 브리지가 핫플러그 가능하면 각 BAR이 점유한 슬롯 메모리 영역을 반환. */
	if (bus_device && bus_device->hotplug_capable) {
		for (i = 0; i < num_bars; ++i) {
			if (dev->bar[i].start != 0) {   /* [한국어] 실제 할당된 BAR만 반환. */
				vmd_hotplug_free_addr(&bus_device->hp, dev->bar[i].start);   /* [한국어] 슬롯 풀로 반환. */
			}
		}
	}

	free(dev);   /* [한국어] 디바이스 객체 메모리 해제. */
}

/*
 * [한국어]
 * vmd_dev_detach - SPDK PCI 추상화의 detach 경로: hook 해제 + 버스 리스트에서 제거 + free
 *
 * @dev: 분리할 SPDK pci device(vmd_pci_device.pci)
 * @return: 없음
 *
 * 핫리무브나 사용자 요청으로 디바이스를 SPDK에서 떼어낼 때, spdk_pci_unhook_device()로
 * NVMe 드라이버와의 hook을 끊고, 소속 버스의 dev_list에서 제거한 뒤 vmd_dev_free()로
 * 자원을 회수한다. detach 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_remove_device() → [vmd_dev_detach] → spdk_pci_unhook_device/vmd_dev_free
 */
static void
vmd_dev_detach(struct spdk_pci_device *dev)
{
	struct vmd_pci_device *vmd_device = (struct vmd_pci_device *)dev;   /* [한국어] pci가 구조체 선두라 직접 캐스팅. */
	struct vmd_pci_bus *bus = vmd_device->bus;   /* [한국어] 소속 버스. */

	spdk_pci_unhook_device(dev);   /* [한국어] SPDK PCI 서브시스템에서 hook 해제(NVMe 드라이버와 분리). */
	TAILQ_REMOVE(&bus->dev_list, vmd_device, tailq);   /* [한국어] 버스 디바이스 리스트에서 제거. */

	vmd_dev_free(vmd_device);   /* [한국어] 핫플러그 영역 반환 + 객체 해제. */
}

/*
 * [한국어]
 * vmd_dev_init - vmd_pci_device의 SPDK pci 필드(주소/id/vtable 콜백 등)를 채워 노출 준비
 *
 * @dev: 초기화할 VMD PCI 디바이스
 * @return: 없음(dev->pci 구조체를 채움)
 *
 * VMD 뒤 디바이스를 SPDK가 일반 PCI 디바이스처럼 다룰 수 있도록, spdk_pci_device(.pci)에
 * BDF 주소(domain/bus/dev/func=0), NUMA id, vendor/device id, type="vmd", 그리고
 * map_bar/unmap_bar/cfg_read/cfg_write 콜백 함수 포인터를 설정한다. 또한 PCIe slot
 * control 레지스터를 캐싱해 둔다(핫플러그 비교 기준). 기본적으로 hotplug_capable은
 * false로 두며, 브리지인 경우 호출자(scan)가 이후 vmd_init_hotplug에서 true로 바꾼다.
 * 스캔/attach/리스트 노출 시점의 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_init_end_device()/vmd_scan_single_bus()/spdk_vmd_pci_device_list() → [vmd_dev_init]
 */
static void
vmd_dev_init(struct vmd_pci_device *dev)
{
	dev->pci.addr.domain = dev->bus->vmd->domain;   /* [한국어] SPDK PCI domain = VMD 합성 domain. */
	dev->pci.addr.bus = dev->bus->bus_number;       /* [한국어] 버스 번호. */
	dev->pci.addr.dev = dev->devfn;                 /* [한국어] device 번호. */
	dev->pci.addr.func = 0;                         /* [한국어] VMD는 항상 function 0. */
	dev->pci.numa_id = spdk_pci_device_get_numa_id(dev->bus->vmd->pci);   /* [한국어] VMD 엔드포인트의 NUMA 노드 상속. */
	dev->pci.id.vendor_id = dev->header->common.vendor_id;   /* [한국어] vendor id. */
	dev->pci.id.device_id = dev->header->common.device_id;   /* [한국어] device id. */
	dev->pci.type = "vmd";                          /* [한국어] 타입 태그(이후 detach/attach에서 검증에 사용). */
	dev->pci.map_bar = vmd_dev_map_bar;             /* [한국어] BAR 매핑 콜백 등록. */
	dev->pci.unmap_bar = vmd_dev_unmap_bar;         /* [한국어] BAR 언맵 콜백(no-op) 등록. */
	dev->pci.cfg_read = vmd_dev_cfg_read;           /* [한국어] config 읽기 콜백 등록. */
	dev->pci.cfg_write = vmd_dev_cfg_write;         /* [한국어] config 쓰기 콜백 등록. */
	dev->hotplug_capable = false;                   /* [한국어] 기본은 비핫플러그(브리지면 scan이 나중에 변경). */
	/* [한국어] PCIe cap이 있으면 slot control 레지스터를 캐싱(핫플러그 상태 비교 기준값). */
	if (dev->pcie_cap != NULL) {
		dev->cached_slot_control = dev->pcie_cap->slot_control;
	}
}

/*
 * [한국어]
 * vmd_init_end_device - 엔드포인트(NVMe SSD)를 BAR 할당·MSI-X 설정 후 SPDK NVMe 드라이버에 hook
 *
 * @dev: 초기화할 엔드포인트 디바이스
 * @return: 0 성공, -1 실패(BAR 할당 실패 또는 hook 실패)
 *
 * VMD 뒤에서 발견된 type-0 엔드포인트를 SPDK가 쓸 수 있게 만드는 마무리 단계다.
 * (1) vmd_assign_base_addrs로 BAR 주소를 membar에서 할당하고, (2) MSI-X 테이블을
 * 마스킹/설정하고, (3) spdk_pci_device 필드를 채운다(vmd_dev_init). 그 디바이스가 NVMe면
 * spdk_pci_nvme_get_driver()로 NVMe 드라이버를 얻어 spdk_pci_hook_device()로 후킹하여,
 * 이후 lib/nvme가 이 디바이스를 일반 NVMe SSD처럼 attach/probe할 수 있게 한다. 성공한
 * NVMe는 어댑터의 target[] 배열에 등록한다. 마지막으로 버스 dev_list에 추가한다. 스캔/
 * 핫플러그/attach 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_scan_single_bus()/vmd_attach_device() → [vmd_init_end_device] →
 *   vmd_assign_base_addrs/vmd_setup_msix/vmd_dev_init/spdk_pci_hook_device
 */
static int
vmd_init_end_device(struct vmd_pci_device *dev)
{
	struct vmd_pci_bus *bus = dev->bus;        /* [한국어] 디바이스가 매달린 버스. */
	struct vmd_adapter *vmd;                   /* [한국어] NVMe 등록 시 사용할 어댑터. */
	struct spdk_pci_driver *driver;            /* [한국어] hook할 NVMe 드라이버 핸들. */
	uint8_t bdf[32];                           /* [한국어] 로깅용 BDF 문자열 버퍼. */
	int rc;                                    /* [한국어] hook 결과 코드. */

	/* [한국어] BAR 주소 할당 실패면 디바이스를 쓸 수 없음 → 에러 후 실패 반환(호출자가 폐기). */
	if (!vmd_assign_base_addrs(dev)) {
		SPDK_ERRLOG("Failed to allocate BARs for device: %p\n", dev);
		return -1;
	}

	vmd_setup_msix(dev, &bus->vmd->msix_table[0]);   /* [한국어] MSI-X 테이블 마스킹/설정(인터럽트 VMD 벡터0로 수렴). */
	vmd_dev_init(dev);                               /* [한국어] spdk_pci_device 필드/콜백 채우기. */

	/* [한국어] NVMe Express 디바이스면 SPDK NVMe 드라이버에 hook하여 일반 NVMe처럼 노출. */
	if (vmd_is_supported_device(dev)) {
		spdk_pci_addr_fmt(bdf, sizeof(bdf), &dev->pci.addr);   /* [한국어] BDF 문자열 포맷(로깅). */
		SPDK_INFOLOG(vmd, "Initializing NVMe device at %s\n", bdf);
		dev->pci.parent = dev->bus->vmd->pci;   /* [한국어] 부모를 실제 VMD 엔드포인트로 지정(DMA/매핑 상속). */

		driver = spdk_pci_nvme_get_driver();   /* [한국어] SPDK NVMe PCIe 드라이버 핸들 획득. */
		assert(driver != NULL);                /* [한국어] NVMe 드라이버는 항상 등록돼 있어야 함. */
		rc = spdk_pci_hook_device(driver, &dev->pci);   /* [한국어] 가짜 pci device를 NVMe 드라이버에 후킹. */
		if (rc != 0) {
			SPDK_ERRLOG("Failed to hook device %s: %s\n", bdf, spdk_strerror(-rc));   /* [한국어] hook 실패 로깅. */
			return -1;   /* [한국어] 실패 반환(호출자가 디바이스 폐기). */
		}

		vmd = bus->vmd;                        /* [한국어] 어댑터. */
		vmd->target[vmd->nvme_count] = dev;    /* [한국어] NVMe 디바이스를 target 배열에 등록. */
		vmd->nvme_count++;                     /* [한국어] NVMe 개수 증가. */
	}

	/* Attach the device to the current bus and assign base addresses */
	TAILQ_INSERT_TAIL(&bus->dev_list, dev, tailq);   /* [한국어] 버스 디바이스 리스트에 추가(이후 순회 노출). */
	g_end_device_count++;   /* [한국어] 엔드 디바이스 카운터 증가(디버그 로깅용). */

	return 0;   /* [한국어] 성공. */
}

/*
 * Scans a single bus for all devices attached and return a count of
 * how many devices found. In the VMD topology, it is assume there are no multi-
 * function devices. Hence a bus(bridge) will not have multi function with both type
 * 0 and 1 header.
 *
 * The other option  for implementing this function is the bus is an int and
 * create a new device PciBridge. PciBridge would inherit from PciDevice with extra fields,
 * sub/pri/sec bus. The input becomes PciPort, bus number and parent_bridge.
 *
 * The bus number is scanned and if a device is found, based on the header_type, create
 * either PciBridge(1) or PciDevice(0).
 *
 * If a PciBridge, assign bus numbers and rescan new bus. The currently PciBridge being
 * scanned becomes the passed in parent_bridge with the new bus number.
 *
 * The linked list becomes list of pciBridges with PciDevices attached.
 *
 * Return count of how many devices found(type1 + type 0 header devices)
 */
/*
 * [한국어]
 * vmd_scan_single_bus - 하나의 PCI 버스를 device 0~31 순회하며 깊이우선으로 재귀 스캔
 *
 * @bus: 스캔할 버스
 * @parent_bridge: 이 버스 위의 부모 브리지(subordinate 전파 체인 구성용; 루트는 NULL)
 * @hotplug: true면 핫플러그 재스캔 모드(엔드포인트만 새로 초기화, 브리지는 건너뜀)
 * @return: 이 버스(및 그 하위 재귀)에서 발견·초기화한 디바이스 총 개수
 *
 * 이 파일의 PCIe enumeration 엔진 핵심이다. 각 device 슬롯에서 vmd_alloc_dev로
 * 디바이스를 만들고, 헤더 타입이 브리지면 새 버스 번호를 받아(new bus 생성) 그 하위를
 * 재귀 스캔하며, 슬롯이 핫플러그 가능하면 버스 번호 예약 + 핫플러그 메모리 관리자
 * 초기화를 수행한다. 엔드포인트면 vmd_init_end_device로 BAR 할당 + NVMe hook을 한다.
 * 깊이우선이므로 브리지를 만나면 그 하위를 끝까지 내려간 뒤 형제 슬롯으로 돌아온다.
 * Switch Upstream Port는 하나의 하위 버스만 가지므로 발견 즉시 반환(나머지 슬롯 생략).
 * 핫플러그 모드에서는 브리지를 재생성하지 않고(이미 트리 존재) 새 엔드포인트만 채운다.
 * 스캔/핫플러그 단일 스레드 컨텍스트(재귀 호출은 같은 스레드 내 스택).
 *
 * 호출 체인:
 *   vmd_scan_pcibus()/vmd_bus_handle_hotplug()/spdk_vmd_rescan() → [vmd_scan_single_bus]
 *     → vmd_alloc_dev/vmd_create_new_bus/vmd_init_hotplug/[재귀]/vmd_init_end_device
 */
static uint8_t
vmd_scan_single_bus(struct vmd_pci_bus *bus, struct vmd_pci_device *parent_bridge, bool hotplug)
{
	/* assuming only single function devices are on the bus */
	struct vmd_pci_device *new_dev;                          /* [한국어] 슬롯에서 새로 만든 디바이스. */
	union express_slot_capabilities_register slot_cap;       /* [한국어] PCIe slot capability(핫플러그 가능 여부 등). */
	struct vmd_pci_bus *new_bus;                             /* [한국어] 브리지 하위에 만들 새 버스. */
	uint8_t  device_number, dev_cnt = 0;                     /* [한국어] device 슬롯 인덱스 / 발견 개수 누적. */
	uint8_t new_bus_num;                                     /* [한국어] 브리지에 부여할 새 버스 번호. */
	int rc;                                                  /* [한국어] 엔드 디바이스 초기화 결과. */

	/* [한국어] VMD 토폴로지는 멀티펑션 없음 가정 → device 0~31, function 0만 순회. */
	for (device_number = 0; device_number < 32; device_number++) {
		new_dev = vmd_alloc_dev(bus, device_number);   /* [한국어] 슬롯에 디바이스가 있으면 객체 생성. */
		if (new_dev == NULL) {
			continue;   /* [한국어] 빈 슬롯/중복 → 다음 슬롯. */
		}

		/* [한국어] 헤더 타입이 브리지면 하위 버스를 만들어 재귀 스캔. */
		if (new_dev->header->common.header_type & PCI_HEADER_TYPE_BRIDGE) {
			/* [한국어] 핫플러그 재스캔 모드에서는 브리지를 재생성하지 않음(트리는 이미 존재). */
			if (hotplug) {
				free(new_dev);
				continue;
			}

			slot_cap.as_uint32_t = 0;   /* [한국어] slot capability 기본값 0. */
			/* [한국어] PCIe cap이 있으면 slot capability 레지스터 캐싱(핫플러그 판정용). */
			if (new_dev->pcie_cap != NULL) {
				slot_cap.as_uint32_t = new_dev->pcie_cap->slot_cap.as_uint32_t;
			}

			new_bus_num = vmd_get_next_bus_number(bus->vmd);   /* [한국어] 브리지 하위 버스 번호 할당. */
			if (new_bus_num == 0xff) {
				vmd_dev_free(new_dev);   /* [한국어] 버스 번호 소진 → 디바이스 폐기 후 현재까지 개수 반환. */
				return dev_cnt;
			}
			new_bus = vmd_create_new_bus(bus, new_dev, new_bus_num);   /* [한국어] 하위 버스 객체 생성. */
			if (!new_bus) {
				vmd_dev_free(new_dev);   /* [한국어] 버스 생성 실패 → 폐기 후 반환. */
				return dev_cnt;
			}
			new_bus->primary_bus = bus->secondary_bus;   /* [한국어] 새 버스의 primary = 현재 버스 secondary. */
			new_bus->self = new_dev;                     /* [한국어] 새 버스의 상위 브리지 = new_dev. */
			new_dev->bus_object = new_bus;               /* [한국어] 브리지 → 하위 버스 역참조. */

			/* [한국어] slot이 핫플러그 가능하고 slot이 구현돼 있으면 핫플러그용 버스 번호 예약. */
			if (slot_cap.bit_field.hotplug_capable && new_dev->pcie_cap != NULL &&
			    new_dev->pcie_cap->express_cap_register.bit_field.slot_implemented) {
				new_bus->hotplug_buses = vmd_get_hotplug_bus_numbers(new_dev);   /* [한국어] 예약 버스 개수. */
				new_bus->subordinate_bus += new_bus->hotplug_buses;             /* [한국어] subordinate를 예약분만큼 확장. */

				/* Attach hot plug instance if HP is supported */
				/* Hot inserted SSDs can be assigned port bus of sub-ordinate + 1 */
				SPDK_INFOLOG(vmd, "hotplug_capable/slot_implemented = "
					     "%x:%x\n", slot_cap.bit_field.hotplug_capable,
					     new_dev->pcie_cap->express_cap_register.bit_field.slot_implemented);
			}

			new_dev->parent_bridge = parent_bridge;   /* [한국어] subordinate 상향 전파 체인 연결. */
			new_dev->header->one.primary = new_bus->primary_bus;       /* [한국어] config primary 버스 기록. */
			new_dev->header->one.secondary = new_bus->secondary_bus;   /* [한국어] config secondary 버스 기록. */
			new_dev->header->one.subordinate = new_bus->subordinate_bus;   /* [한국어] config subordinate 버스 기록. */

			vmd_bus_update_bridge_info(new_dev);   /* [한국어] 상위 브리지들의 subordinate 전파 갱신. */
			TAILQ_INSERT_TAIL(&bus->vmd->bus_list, new_bus, tailq);   /* [한국어] 어댑터 전역 버스 리스트에 등록. */

			vmd_dev_init(new_dev);   /* [한국어] 브리지의 spdk_pci 필드 초기화. */
			dev_cnt++;               /* [한국어] 브리지도 발견 개수에 포함. */

			/* [한국어] 핫플러그 슬롯이면 전용 1MB 메모리 윈도/관리자를 셋업(나중 삽입 대비). */
			if (slot_cap.bit_field.hotplug_capable && new_dev->pcie_cap != NULL &&
			    new_dev->pcie_cap->express_cap_register.bit_field.slot_implemented) {
				vmd_init_hotplug(new_dev, new_bus);
			}

			dev_cnt += vmd_scan_single_bus(new_bus, new_dev, hotplug);   /* [한국어] 하위 버스 깊이우선 재귀 스캔. */
			/* [한국어] Switch Upstream Port는 하위 버스가 하나뿐 → 발견 즉시 형제 슬롯 생략하고 반환. */
			if (new_dev->pcie_cap != NULL) {
				if (new_dev->pcie_cap->express_cap_register.bit_field.device_type == SwitchUpstreamPort) {
					return dev_cnt;
				}
			}
		} else {
			/* [한국어] 엔드포인트면 BAR 할당 + NVMe hook 수행. */
			rc = vmd_init_end_device(new_dev);
			if (rc != 0) {
				vmd_dev_free(new_dev);   /* [한국어] 초기화 실패 → 디바이스 폐기(개수 미증가). */
			} else {
				dev_cnt++;   /* [한국어] 성공 → 개수 증가. */
			}
		}
	}

	return dev_cnt;   /* [한국어] 이 버스 서브트리에서 발견한 총 디바이스 수. */
}

/*
 * [한국어]
 * vmd_print_pci_info - 디바이스의 종류/BDF/BAR/브리지 정보/시리얼 넘버를 INFOLOG로 출력
 *
 * @dev: 정보를 출력할 디바이스(NULL 안전)
 * @return: 없음(SPDK_INFOLOG 출력만)
 *
 * 스캔 완료 후 트리 전체를 사람이 검토할 수 있도록 각 디바이스의 PCIe device type
 * (device_type[] 매핑), BDF 좌표, 엔드포인트면 BAR0 주소/가상주소, 브리지면 primary/
 * secondary/subordinate 및 핫플러그 가능 여부, 시리얼 넘버 capability가 있으면 8바이트
 * SN을 로깅한다. 기능에는 영향 없는 진단용. 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_scan_pcibus() → [vmd_print_pci_info]
 */
static void
vmd_print_pci_info(struct vmd_pci_device *dev)
{
	if (!dev) {
		return;   /* [한국어] NULL이면 아무것도 출력하지 않음. */
	}

	/* [한국어] PCIe cap이 있으면 device type 문자열까지 포함해 출력. */
	if (dev->pcie_cap != NULL) {
		SPDK_INFOLOG(vmd, "PCI DEVICE: [%04X:%04X] type(%x) : %s\n",
			     dev->header->common.vendor_id, dev->header->common.device_id,
			     dev->pcie_cap->express_cap_register.bit_field.device_type,
			     device_type[dev->pcie_cap->express_cap_register.bit_field.device_type]);   /* [한국어] vendor/device id + device type 분류 출력. */
	} else {
		/* [한국어] PCIe cap이 없으면 vendor/device id만 출력. */
		SPDK_INFOLOG(vmd, "PCI DEVICE: [%04X:%04X]\n",
			     dev->header->common.vendor_id, dev->header->common.device_id);
	}

	SPDK_INFOLOG(vmd, "\tDOMAIN:BDF: %04x:%02x:%02x:%x\n", dev->pci.addr.domain,
		     dev->pci.addr.bus, dev->pci.addr.dev, dev->pci.addr.func);   /* [한국어] SPDK PCI 좌표(BDF) 출력. */

	/* [한국어] 엔드포인트(비브리지)이고 버스가 있으면 BAR0 주소/가상주소 출력. */
	if (!(dev->header_type & PCI_HEADER_TYPE_BRIDGE) && dev->bus) {
		SPDK_INFOLOG(vmd, "\tbase addr: %x : %p\n",
			     dev->header->zero.BAR[0], (void *)dev->bar[0].vaddr);
	}

	/* [한국어] 브리지면 primary/secondary/subordinate 버스 및 슬롯/핫플러그 능력 출력. */
	if ((dev->header_type & PCI_HEADER_TYPE_BRIDGE)) {
		SPDK_INFOLOG(vmd, "\tPrimary = %d, Secondary = %d, Subordinate = %d\n",
			     dev->header->one.primary, dev->header->one.secondary, dev->header->one.subordinate);
		/* [한국어] PCIe slot이 구현돼 있으면 그 사실과 핫플러그 가능 여부 출력. */
		if (dev->pcie_cap && dev->pcie_cap->express_cap_register.bit_field.slot_implemented) {
			SPDK_INFOLOG(vmd, "\tSlot implemented on this device.\n");
			if (dev->pcie_cap->slot_cap.bit_field.hotplug_capable) {
				SPDK_INFOLOG(vmd, "Device has HOT-PLUG capable slot.\n");
			}
		}
	}

	/* [한국어] 시리얼 넘버 capability가 있으면 8바이트 SN을 빅엔디언 순으로 출력. */
	if (dev->sn_cap != NULL) {
		uint8_t *snLow = (uint8_t *)&dev->sn_cap->sn_low;   /* [한국어] SN 하위 4바이트 포인터. */
		uint8_t *snHi = (uint8_t *)&dev->sn_cap->sn_hi;     /* [한국어] SN 상위 4바이트 포인터. */

		SPDK_INFOLOG(vmd, "\tSN: %02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x\n",
			     snHi[3], snHi[2], snHi[1], snHi[0], snLow[3], snLow[2], snLow[1], snLow[0]);   /* [한국어] 8바이트 SN 출력. */
	}
}

/*
 * [한국어]
 * vmd_cache_scan_info - 루트 포트의 prefetch upper 레지스터에 "스캔 완료" 시그니처를 기록
 *
 * @dev: 시그니처를 기록할 디바이스(루트 포트일 때만 동작)
 * @return: 없음(루트 포트 config의 prefetch upper 두 레지스터 기록)
 *
 * 스캔이 끝난 뒤 루트 포트의 prefetch_base_upper/limit_upper에 VMD_UPPER_BASE/LIMIT_
 * SIGNATURE 매직값을 써 둔다. 이는 다음 번 드라이버 로드/재스캔 시 vmd_device_is_
 * enumerated()가 "이 도메인은 이미 열거됨"을 인식하는 마커가 된다. 엔드포인트나
 * 비루트 브리지에는 기록하지 않는다. 각 쓰기 후 read-back으로 posted write 순서를
 * 보장한다. 스캔 종료 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_scan_pcibus() → [vmd_cache_scan_info] → vmd_device_is_root_port
 */
static void
vmd_cache_scan_info(struct vmd_pci_device *dev)
{
	uint32_t reg __attribute__((unused));   /* [한국어] read-back용 더미(posted write 순서 보장). */

	/* [한국어] 엔드포인트(type-0)는 prefetch upper 레지스터가 없으므로 대상 아님. */
	if (dev->header_type == PCI_HEADER_TYPE_NORMAL) {
		return;
	}

	SPDK_INFOLOG(vmd, "vendor/device id:%x:%x\n", dev->header->common.vendor_id,
		     dev->header->common.device_id);   /* [한국어] 대상 디바이스 식별 로깅. */

	/* [한국어] 루트 포트에만 시그니처 기록(도메인 열거 완료 마커). */
	if (vmd_device_is_root_port(dev->header)) {
		dev->header->one.prefetch_base_upper = VMD_UPPER_BASE_SIGNATURE;   /* [한국어] base upper에 매직값 기록. */
		reg = dev->header->one.prefetch_base_upper;                        /* [한국어] read-back. */
		dev->header->one.prefetch_limit_upper = VMD_UPPER_LIMIT_SIGNATURE; /* [한국어] limit upper에 매직값 기록. */
		reg = dev->header->one.prefetch_limit_upper;                       /* [한국어] read-back. */

		SPDK_INFOLOG(vmd, "prefetch: %x:%x\n",
			     dev->header->one.prefetch_base_upper,
			     dev->header->one.prefetch_limit_upper);   /* [한국어] 기록 결과 로깅. */
	}
}

/*
 * [한국어]
 * vmd_reset_root_ports - 스캔 시작 전, 아직 열거되지 않은 루트 포트들의 stale 설정을 리셋
 *
 * @bus: 루트 버스(루트 포트들이 매달린 버스)
 * @return: 없음(미열거 루트 포트의 base/limit/bus 레지스터 클리어)
 *
 * SPDK 로드 전에 다른 드라이버(예: Linux 커널)가 루트 포트를 설정해 둔 stale한 값이
 * 남아 있을 수 있다. 깊이우선 스캔은 초기 루트 포트부터 차례로 진행하는데, 나중에 스캔될
 * 루트 포트가 stale 설정을 들고 있으면 두 브리지가 같은 secondary/subordinate를 갖는
 * 충돌이 생길 수 있다(이슈 #2413). 이를 막기 위해 스캔 전에 미열거 루트 포트들의 base/
 * limit/bus 레지스터를 미리 클리어한다. 이미 시그니처가 있는(열거된) 포트는 건드리지
 * 않는다. 스캔 직전 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_scan_pcibus() → [vmd_reset_root_ports] → vmd_reset_base_limit_registers
 */
static void
vmd_reset_root_ports(struct vmd_pci_bus *bus)
{
	volatile struct pci_header *header;   /* [한국어] 각 슬롯의 config 헤더. */
	uint32_t devfn;                       /* [한국어] device 번호 순회 인덱스. */

	/*
	 * The root ports might have been configured by some other driver (e.g.  Linux kernel) prior
	 * to loading the SPDK one, so we need to clear it.  We need to do it before starting the
	 * scanning process, as it's depth-first, so when initial root ports are scanned, the
	 * latter ones might still be using stale configuration.  This can lead to two bridges
	 * having the same secondary/subordinate bus configuration, which, of course, isn't correct.
	 * (Note: this fixed issue #2413.)
	 */
	/* [한국어] 루트 버스의 모든 device 슬롯을 순회. */
	for (devfn = 0; devfn < 32; ++devfn) {
		if (!vmd_bus_device_present(bus, devfn)) {
			continue;   /* [한국어] 빈 슬롯은 건너뜀. */
		}

		header = (volatile void *)(bus->vmd->cfg_vaddr +
					   CONFIG_OFFSET_ADDR(bus->config_bus_number, devfn, 0, 0));   /* [한국어] 슬롯 config 헤더. */
		/* [한국어] 루트 포트이면서 아직 열거 안 된(시그니처 없는) 것만 stale 레지스터 리셋. */
		if (vmd_device_is_root_port(header) && !vmd_device_is_enumerated(header)) {
			vmd_reset_base_limit_registers(header);
		}
	}
}

/*
 * [한국어]
 * vmd_scan_pcibus - VMD 루트 버스 스캔을 오케스트레이션하고 결과를 로깅
 *
 * @bus: 스캔할 루트 버스(vmd->vmd_bus)
 * @return: 발견·초기화된 디바이스 총 개수
 *
 * VMD 도메인 enumeration의 최상위 진입 절차다. (1) stale 루트 포트를 리셋하고, (2)
 * 카운터/버스 리스트를 초기화하며 next_bus_number 커서를 세팅하고, (3) vmd_scan_single_
 * bus()로 깊이우선 스캔을 시작한다. 스캔 후 발견된 모든 버스/디바이스를 순회하며 진단
 * 정보를 출력하고, 루트 포트에는 스캔 완료 시그니처를 기록(vmd_cache_scan_info)한다.
 * 스캔 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_enumerate_devices() → [vmd_scan_pcibus] → vmd_reset_root_ports/vmd_scan_single_bus
 */
static uint8_t
vmd_scan_pcibus(struct vmd_pci_bus *bus)
{
	struct vmd_pci_bus *bus_entry;     /* [한국어] 결과 출력 시 버스 순회 커서. */
	struct vmd_pci_device *dev;        /* [한국어] 결과 출력 시 디바이스 순회 커서. */
	uint8_t dev_cnt;                   /* [한국어] 발견 개수. */

	vmd_reset_root_ports(bus);   /* [한국어] 스캔 전 stale 루트 포트 설정 클리어. */

	g_end_device_count = 0;   /* [한국어] 엔드 디바이스 카운터 리셋. */
	TAILQ_INSERT_TAIL(&bus->vmd->bus_list, bus, tailq);   /* [한국어] 루트 버스를 전역 버스 리스트에 등록. */
	bus->vmd->next_bus_number = bus->bus_number + 1;      /* [한국어] 다음 할당 버스 번호 = 루트+1. */
	dev_cnt = vmd_scan_single_bus(bus, NULL, false);      /* [한국어] 루트부터 깊이우선 스캔 시작(부모 없음). */

	SPDK_INFOLOG(vmd, "VMD scan found %u devices\n", dev_cnt);   /* [한국어] 총 디바이스 수 로깅. */
	SPDK_INFOLOG(vmd, "VMD scan found %u END DEVICES\n", g_end_device_count);   /* [한국어] 엔드 디바이스 수 로깅. */

	SPDK_INFOLOG(vmd, "PCIe devices attached to VMD %04x:%02x:%02x:%x...\n",
		     bus->vmd->pci->addr.domain, bus->vmd->pci->addr.bus,
		     bus->vmd->pci->addr.dev, bus->vmd->pci->addr.func);   /* [한국어] VMD 엔드포인트 좌표 로깅. */

	/* [한국어] 발견된 모든 버스를 순회하며 진단 출력 + 루트 포트 시그니처 기록. */
	TAILQ_FOREACH(bus_entry, &bus->vmd->bus_list, tailq) {
		if (bus_entry->self != NULL) {
			vmd_print_pci_info(bus_entry->self);   /* [한국어] 버스의 상위 브리지 정보 출력. */
			vmd_cache_scan_info(bus_entry->self);  /* [한국어] 루트 포트면 스캔 완료 시그니처 기록. */
		}

		/* [한국어] 그 버스에 매달린 디바이스들 정보 출력. */
		TAILQ_FOREACH(dev, &bus_entry->dev_list, tailq) {
			vmd_print_pci_info(dev);
		}
	}

	return dev_cnt;   /* [한국어] 발견 개수 반환. */
}

/*
 * [한국어]
 * vmd_domain_map_bar - VMD 엔드포인트의 BAR을 매핑하고 진짜 물리주소를 config에서 보정
 *
 * @vmd: 대상 VMD 어댑터(vmd->pci가 실제 VMD 엔드포인트)
 * @bar: 매핑할 BAR 인덱스(0=config BAR, 2=memory BAR)
 * @vaddr: [out] 매핑된 가상주소
 * @paddr: [out] BAR의 실제 물리주소(IOVA 무관)
 * @size: [out] BAR 크기
 * @return: 0 성공, 음수 errno 실패
 *
 * VMD의 두 BAR을 SPDK PCI API로 mmap한다. 다만 spdk_pci_device_map_bar()가 돌려주는
 * 물리주소는 IOMMU/IOVA 설정에 따라 가상(IOVA)일 수 있다. VMD는 이 물리주소를 기준으로
 * 하위 디바이스의 base/limit 레지스터와 BAR을 채워야 하므로, 반드시 진짜 물리주소가
 * 필요하다. 따라서 config 공간의 BAR 레지스터를 직접 읽어(IOVA 설정과 무관) 하위 4비트
 * 플래그를 마스크한 실제 물리 베이스를 paddr에 채운다. 초기화 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_domain_map_bars() → [vmd_domain_map_bar] → spdk_pci_device_map_bar/cfg_read
 */
static int
vmd_domain_map_bar(struct vmd_adapter *vmd, uint32_t bar,
		   void **vaddr, uint64_t *paddr, uint64_t *size)
{
	uint64_t unused;   /* [한국어] map_bar가 주는 (IOVA일 수 있는) 물리주소 — 사용하지 않고 버림. */
	int rc;            /* [한국어] API 결과 코드. */

	rc = spdk_pci_device_map_bar(vmd->pci, bar, vaddr, &unused, size);   /* [한국어] BAR mmap → 가상주소/크기 획득. */
	if (rc != 0) {
		return rc;   /* [한국어] 매핑 실패 → 그대로 반환. */
	}

	/* Depending on the IOVA configuration, the physical address of the BAR returned by
	 * spdk_pci_device_map_bar() can be either an actual physical address or a virtual one (if
	 * IOMMU is enabled).  Since we do need an actual physical address to fill out the
	 * base/limit registers and the BARs of the devices behind the VMD, read the config space to
	 * get the correct address, regardless of IOVA configuration. */
	/* [한국어] config 공간의 BAR 레지스터를 직접 읽어 진짜 물리 베이스 확보(IOVA 무관). */
	rc = spdk_pci_device_cfg_read(vmd->pci, paddr, sizeof(*paddr),
				      PCI_BAR0_OFFSET + bar * PCI_BAR_SIZE);
	if (rc != 0) {
		return rc;   /* [한국어] config 읽기 실패 → 반환. */
	}

	*paddr &= PCI_BAR_MEMORY_ADDR_OFFSET;   /* [한국어] BAR 하위 플래그 비트 제거 → 정렬된 물리 베이스만 남김. */

	return 0;   /* [한국어] 성공. */
}

/*
 * [한국어]
 * vmd_domain_map_bars - VMD의 config BAR(0)과 memory BAR(2)를 매핑하고 할당 커서 초기화
 *
 * @vmd: 대상 VMD 어댑터
 * @return: 0 성공, 음수 errno 실패
 *
 * VMD 도메인 enumeration에 필요한 두 BAR을 매핑한다. BAR0(config BAR)은 하위 디바이스의
 * PCI config 공간 MMIO 윈도이고, BAR2(memory BAR=membar)는 하위 디바이스 BAR을 배치할
 * 메모리 윈도다. 매핑 성공 후 membar의 물리 시작/크기를 할당 커서(physical_addr/
 * current_addr_size)에 세팅해 이후 선형 BAR 할당이 가능하게 한다. 초기화 단일 스레드.
 *
 * 호출 체인:
 *   vmd_enum_cb() → [vmd_domain_map_bars] → vmd_domain_map_bar
 */
static int
vmd_domain_map_bars(struct vmd_adapter *vmd)
{
	int rc;   /* [한국어] 매핑 결과 코드. */

	rc = vmd_domain_map_bar(vmd, 0, (void **)&vmd->cfg_vaddr,
				&vmd->cfgbar, &vmd->cfgbar_size);   /* [한국어] BAR0 = config 공간 윈도 매핑. */
	if (rc != 0) {
		SPDK_ERRLOG("Failed to map config bar: %s\n", spdk_strerror(-rc));   /* [한국어] config BAR 매핑 실패. */
		return rc;
	}

	rc = vmd_domain_map_bar(vmd, 2, (void **)&vmd->mem_vaddr,
				&vmd->membar, &vmd->membar_size);   /* [한국어] BAR2 = membar(디바이스 BAR 배치용) 매핑. */
	if (rc != 0) {
		SPDK_ERRLOG("Failed to map memory bar: %s\n", spdk_strerror(-rc));   /* [한국어] memory BAR 매핑 실패. */
		return rc;
	}

	vmd->physical_addr = vmd->membar;            /* [한국어] 선형 할당 커서를 membar 시작으로 초기화. */
	vmd->current_addr_size = vmd->membar_size;   /* [한국어] 남은 윈도 크기를 membar 전체로 초기화. */

	return 0;   /* [한국어] 성공. */
}

/*
 * [한국어]
 * vmd_set_starting_bus_number - VMD config의 VMCAP/VMCONFIG를 읽어 버스 번호 범위 결정(ICX 전용)
 *
 * @vmd: 대상 VMD 어댑터
 * @bus_start: [out] 도메인 시작 버스 번호(0 또는 128)
 * @max_bus: [out] 도메인 최대 버스 번호(127 또는 255)
 * @return: 없음
 *
 * Ice Lake VMD는 도메인이 사용할 버스 번호 범위를 vendor-specific capability(VMCAP)와
 * 설정(VMCONFIG)으로 제약할 수 있다. VMCAP bit0(bus restriction capable)과 VMCONFIG
 * bits8-9(bus restriction)가 모두 활성이면 상위 절반(128~255)을, 아니면 하위 절반
 * (0~127)을 사용한다. 호스트의 다른 PCI 도메인과 버스 번호가 겹치지 않게 하기 위함이다.
 * 초기화 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_enumerate_devices() → [vmd_set_starting_bus_number]
 */
static void
vmd_set_starting_bus_number(struct vmd_adapter *vmd, uint8_t *bus_start,
			    uint8_t *max_bus)
{
	uint32_t vmd_cap = 0, vmd_config = 0;          /* [한국어] VMCAP/VMCONFIG 레지스터 값. */
	uint8_t bus_restrict_cap, bus_restrictions;    /* [한국어] 추출한 능력/설정 비트. */

	spdk_pci_device_cfg_read32(vmd->pci, &vmd_cap, PCI_VMD_VMCAP);        /* [한국어] VMCAP 읽기. */
	spdk_pci_device_cfg_read32(vmd->pci, &vmd_config, PCI_VMD_VMCONFIG);  /* [한국어] VMCONFIG 읽기. */

	bus_restrict_cap = vmd_cap & 0x1; /* bit 0 */                  /* [한국어] VMCAP bit0: 버스 제한 지원 여부. */
	bus_restrictions = (vmd_config >> 8) & 0x3; /* bits 8-9 */     /* [한국어] VMCONFIG bits8-9: 버스 제한 모드. */
	/* [한국어] 제한 지원+모드1이면 상위 절반(128~255), 아니면 하위 절반(0~127) 사용. */
	if ((bus_restrict_cap == 0x1) && (bus_restrictions == 0x1)) {
		*bus_start = 128;
		*max_bus = 255;
	} else {
		*bus_start = 0;
		*max_bus = 127;
	}
}

/*
 * [한국어]
 * vmd_enumerate_devices - VMD 루트 버스를 세대별 버스 번호 정책으로 초기화하고 스캔 시작
 *
 * @vmd: 대상 VMD 어댑터
 * @return: 발견된 디바이스 총 개수
 *
 * vmd_bus(루트 버스)를 어댑터/도메인과 연결하고, ICX 세대면 VMCAP/VMCONFIG로 버스 시작/
 * 최대 번호를 결정(set_starting_bus_number), 그 외 세대는 0~PCI_MAX_BUS_NUMBER를 쓴다.
 * 루트 버스의 primary/secondary/subordinate/bus_number를 시작 번호로 일치시킨 뒤
 * vmd_scan_pcibus()로 실제 enumeration을 트리거한다. 초기화 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_enum_cb() → [vmd_enumerate_devices] → vmd_scan_pcibus
 */
static int
vmd_enumerate_devices(struct vmd_adapter *vmd)
{
	uint8_t max_bus, bus_start;   /* [한국어] 도메인 버스 번호 범위. */

	vmd->vmd_bus.vmd = vmd;                          /* [한국어] 루트 버스 → 어댑터 역참조. */
	vmd->vmd_bus.domain = vmd->pci->addr.domain;     /* [한국어] 도메인 상속. */

	/* [한국어] Ice Lake VMD는 버스 제한 정책을 따름. */
	if (vmd->pci->id.device_id == PCI_DEVICE_ID_INTEL_VMD_ICX) {
		vmd_set_starting_bus_number(vmd, &bus_start, &max_bus);   /* [한국어] 시작/최대 버스 번호 결정. */
		vmd->vmd_bus.bus_start = bus_start;   /* [한국어] config 오프셋 기준이 되는 시작 버스. */
		vmd->vmd_bus.secondary_bus = vmd->vmd_bus.subordinate_bus = vmd->vmd_bus.bus_start;   /* [한국어] 루트 버스 secondary/subordinate 초기화. */
		vmd->vmd_bus.primary_bus = vmd->vmd_bus.bus_number = vmd->vmd_bus.bus_start;          /* [한국어] primary/bus_number 초기화. */
		vmd->max_pci_bus = max_bus;   /* [한국어] 버스 번호 상한. */
	} else {
		/* [한국어] 그 외 세대는 0부터 시작, 전체 버스 범위 사용. */
		vmd->vmd_bus.bus_start = 0;
		vmd->vmd_bus.secondary_bus = vmd->vmd_bus.subordinate_bus = 0;
		vmd->vmd_bus.primary_bus = vmd->vmd_bus.bus_number = 0;
		vmd->max_pci_bus = PCI_MAX_BUS_NUMBER;
	}

	return vmd_scan_pcibus(&vmd->vmd_bus);   /* [한국어] 루트 버스부터 enumeration 시작. */
}

/*
 * [한국어]
 * vmd_find_device - 전역 VMD 트리에서 주어진 PCI 주소와 일치하는 디바이스를 검색
 *
 * @addr: 찾을 SPDK PCI 주소(domain/bus/dev/func)
 * @return: 일치하는 vmd_pci_device(브리지 또는 엔드포인트), 없으면 NULL
 *
 * 모든 VMD 어댑터의 버스 리스트를 순회하며, 각 버스의 상위 브리지(self)와 그 버스에
 * 매달린 디바이스들의 pci.addr를 비교해 일치하는 것을 반환한다. attach/detach/remove RPC
 * 경로에서 주소로 디바이스를 찾는 데 쓰인다. 초기화 후 단일 스레드 접근(전역 트리는
 * 스캔 이후 읽기 위주).
 *
 * 호출 체인:
 *   spdk_vmd_remove_device()/vmd_detach_device() → [vmd_find_device]
 */
struct vmd_pci_device *
vmd_find_device(const struct spdk_pci_addr *addr)
{
	struct vmd_pci_bus *bus;     /* [한국어] 버스 순회 커서. */
	struct vmd_pci_device *dev;  /* [한국어] 디바이스 순회 커서. */
	uint32_t i;                  /* [한국어] VMD 어댑터 인덱스. */

	/* [한국어] 모든 VMD 어댑터를 순회. */
	for (i = 0; i < g_vmd_container.count; ++i) {
		/* [한국어] 어댑터의 모든 버스를 순회. */
		TAILQ_FOREACH(bus, &g_vmd_container.vmd[i].bus_list, tailq) {
			/* [한국어] 버스의 상위 브리지 주소가 일치하면 그 브리지 반환. */
			if (bus->self) {
				if (spdk_pci_addr_compare(&bus->self->pci.addr, addr) == 0) {
					return bus->self;
				}
			}

			/* [한국어] 버스에 매달린 디바이스 중 주소가 일치하는 것 반환. */
			TAILQ_FOREACH(dev, &bus->dev_list, tailq) {
				if (spdk_pci_addr_compare(&dev->pci.addr, addr) == 0) {
					return dev;
				}
			}
		}
	}

	return NULL;   /* [한국어] 못 찾음. */
}

/*
 * [한국어]
 * vmd_enum_cb - spdk_pci_enumerate가 VMD 엔드포인트를 발견할 때마다 호출되는 콜백
 *
 * @ctx: 콜백 컨텍스트(여기서는 &g_vmd_container)
 * @pci_dev: 발견된 실제 VMD PCI 엔드포인트
 * @return: 0 성공, -1 실패(BAR 매핑 실패)
 *
 * 시스템의 VMD 드라이버 매칭 디바이스마다 한 번씩 호출된다. 발견된 VMD에 대해 (1)
 * Command 레지스터에 Bus Master/Memory Enable(0x6) 비트를 세우고, (2) 컨테이너의 다음
 * 슬롯(vmd_c->vmd[count])을 채워 pci/도메인/버스리스트를 초기화하고, (3) 두 BAR을
 * 매핑한 뒤, (4) count를 증가시키고 vmd_enumerate_devices()로 내부 PCIe 트리 스캔을
 * 시작한다. 합성 domain은 (bus<<16)|(dev<<8)|func로 만들어 VMD를 고유 식별한다.
 * 초기화 단일 스레드 컨텍스트(spdk_pci_enumerate 내부에서 동기 호출).
 *
 * 호출 체인:
 *   spdk_vmd_init() → spdk_pci_enumerate() → [vmd_enum_cb] → vmd_domain_map_bars/vmd_enumerate_devices
 */
static int
vmd_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	uint32_t cmd_reg = 0;                              /* [한국어] Command 레지스터 값. */
	char bdf[32] = {0};                               /* [한국어] 로깅용 BDF 문자열. */
	struct vmd_container *vmd_c = ctx;                 /* [한국어] 전역 컨테이너(ctx). */
	struct vmd_adapter *vmd = &vmd_c->vmd[vmd_c->count];   /* [한국어] 이번에 채울 어댑터 슬롯. */

	spdk_pci_device_cfg_read32(pci_dev, &cmd_reg, 4);   /* [한국어] 현재 Command 레지스터 읽기(offset 4). */
	cmd_reg |= 0x6;                      /* PCI bus master/memory enable. */   /* [한국어] bit1(MEM)+bit2(Bus Master) 설정. */
	spdk_pci_device_cfg_write32(pci_dev, cmd_reg, 4);   /* [한국어] VMD 엔드포인트에 DMA/MMIO 허용 기록. */

	spdk_pci_addr_fmt(bdf, sizeof(bdf), &pci_dev->addr);   /* [한국어] BDF 문자열 포맷. */
	SPDK_INFOLOG(vmd, "Found a VMD[ %d ] at %s\n", vmd_c->count, bdf);   /* [한국어] 발견 로깅. */

	/* map vmd bars */
	vmd->pci = pci_dev;                  /* [한국어] 실제 VMD 엔드포인트 포인터 보관. */
	vmd->vmd_index = vmd_c->count;       /* [한국어] 컨테이너 내 인덱스. */
	vmd->domain = (pci_dev->addr.bus << 16) | (pci_dev->addr.dev << 8) | pci_dev->addr.func;   /* [한국어] 합성 domain 식별자. */
	TAILQ_INIT(&vmd->bus_list);          /* [한국어] 이 VMD의 버스 리스트 초기화. */

	/* [한국어] config/memory BAR 매핑 실패면 이 VMD는 사용 불가 → 실패 반환. */
	if (vmd_domain_map_bars(vmd) != 0) {
		return -1;
	}

	SPDK_INFOLOG(vmd, "vmd config bar(%p) vaddr(%p) size(%x)\n",
		     (void *)vmd->cfgbar, (void *)vmd->cfg_vaddr,
		     (uint32_t)vmd->cfgbar_size);   /* [한국어] config BAR 매핑 정보 로깅. */
	SPDK_INFOLOG(vmd, "vmd mem bar(%p) vaddr(%p) size(%x)\n",
		     (void *)vmd->membar, (void *)vmd->mem_vaddr,
		     (uint32_t)vmd->membar_size);   /* [한국어] memory BAR 매핑 정보 로깅. */

	vmd_c->count++;                  /* [한국어] 유효 VMD 개수 증가(다음 슬롯 준비). */
	vmd_enumerate_devices(vmd);      /* [한국어] 이 VMD 내부 PCIe 트리 enumeration 시작. */

	return 0;   /* [한국어] 성공. */
}

/*
 * [한국어]
 * spdk_vmd_pci_device_list - 특정 VMD 주소 뒤의 모든 NVMe 디바이스 목록을 배열로 복사(공개 API)
 *
 * @vmd_addr: 대상 VMD 엔드포인트의 PCI 주소
 * @nvme_list: [out] 결과를 담을 spdk_pci_device 배열(호출자 제공)
 * @return: 채운 디바이스 개수, nvme_list가 NULL이면 -1
 *
 * 주어진 VMD를 컨테이너에서 찾아, 그 모든 버스의 디바이스를 순회하며 spdk_pci_device를
 * 사용자 배열에 복사한다. 아직 hook되지 않은(is_hooked=0) 디바이스는 이 시점에 vmd_dev_
 * init으로 SPDK PCI 필드를 채우고 마킹한다. SPDK 애플리케이션/RPC가 VMD 뒤 NVMe 목록을
 * 조회할 때 호출한다. 초기화 이후 단일 스레드 접근.
 *
 * 호출 체인:
 *   외부 SPDK 코드/RPC → [spdk_vmd_pci_device_list] → vmd_dev_init
 */
int
spdk_vmd_pci_device_list(struct spdk_pci_addr vmd_addr, struct spdk_pci_device *nvme_list)
{
	int cnt = 0;                  /* [한국어] 채운 디바이스 개수. */
	struct vmd_pci_bus *bus;      /* [한국어] 버스 순회 커서. */
	struct vmd_pci_device *dev;   /* [한국어] 디바이스 순회 커서. */
	uint32_t i;                   /* [한국어] VMD 인덱스. */

	/* [한국어] 출력 배열이 없으면 실패. */
	if (!nvme_list) {
		return -1;
	}

	/* [한국어] 모든 VMD를 순회하며 주소가 일치하는 것만 처리. */
	for (i = 0; i < g_vmd_container.count; ++i) {
		if (spdk_pci_addr_compare(&vmd_addr, &g_vmd_container.vmd[i].pci->addr) == 0) {
			/* [한국어] 그 VMD의 모든 버스/디바이스를 순회. */
			TAILQ_FOREACH(bus, &g_vmd_container.vmd[i].bus_list, tailq) {
				TAILQ_FOREACH(dev, &bus->dev_list, tailq) {
					nvme_list[cnt++] = dev->pci;   /* [한국어] spdk_pci_device를 출력 배열에 복사. */
					/* [한국어] 아직 init 안 됐으면 SPDK PCI 필드를 채우고 마킹. */
					if (!dev->is_hooked) {
						vmd_dev_init(dev);
						dev->is_hooked = 1;
					}
				}
			}
		}
	}

	return cnt;   /* [한국어] 채운 개수 반환. */
}

/*
 * [한국어]
 * vmd_clear_hotplug_status - 핫플러그 슬롯/링크 status의 RW1C 비트를 클리어(이벤트 ack)
 *
 * @bus: 핫플러그 이벤트가 발생한 버스(bus->self가 슬롯 브리지)
 * @return: 없음(slot_status/link_status 레지스터에 write-back)
 *
 * PCIe slot/link status 레지스터의 변경 비트는 RW1C(Write-1-to-Clear)다. 읽은 값을 그대로
 * 다시 써서 set된 비트를 clear하여 핫플러그 이벤트를 ack한다. 그래야 다음 이벤트가
 * datalink_state_changed로 다시 감지된다. 각 write 후 read-back으로 순서를 보장한다.
 * 핫플러그 모니터(폴러) 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   spdk_vmd_hotplug_monitor() → [vmd_clear_hotplug_status]
 */
static void
vmd_clear_hotplug_status(struct vmd_pci_bus *bus)
{
	struct vmd_pci_device *device = bus->self;          /* [한국어] 슬롯 브리지 디바이스. */
	uint16_t status __attribute__((unused));            /* [한국어] read/write-back용 임시값. */

	status = device->pcie_cap->slot_status.as_uint16_t;   /* [한국어] slot status 읽기(set된 RW1C 비트 포함). */
	device->pcie_cap->slot_status.as_uint16_t = status;   /* [한국어] 같은 값을 써서 RW1C 비트 clear(이벤트 ack). */
	status = device->pcie_cap->slot_status.as_uint16_t;   /* [한국어] read-back. */

	status = device->pcie_cap->link_status.as_uint16_t;   /* [한국어] link status 읽기. */
	device->pcie_cap->link_status.as_uint16_t = status;   /* [한국어] link status RW1C clear. */
	status = device->pcie_cap->link_status.as_uint16_t;   /* [한국어] read-back. */
}

/*
 * [한국어]
 * vmd_bus_handle_hotplug - 핫플러그 삽입 감지 후 새 디바이스가 나타날 때까지 버스를 재스캔
 *
 * @bus: 삽입 이벤트가 발생한 버스
 * @return: 없음(새 디바이스를 init_end_device까지 처리)
 *
 * SSD가 슬롯에 삽입되면 링크 업/디바이스 준비까지 시간이 걸린다. 따라서 최대 20회(매회
 * 200ms 대기) vmd_scan_single_bus(hotplug=true)를 반복하여 새 디바이스가 나타나면 즉시
 * 처리하고 종료한다. 시간 내 못 찾으면 에러를 로깅한다. spdk_delay_us는 폴러 컨텍스트
 * 에서의 busy 지연이다. 핫플러그 모니터 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   spdk_vmd_hotplug_monitor() → [vmd_bus_handle_hotplug] → vmd_scan_single_bus
 */
static void
vmd_bus_handle_hotplug(struct vmd_pci_bus *bus)
{
	uint8_t num_devices, sleep_count;   /* [한국어] 발견 개수 / 재시도 카운터. */

	/* [한국어] 새 디바이스가 보일 때까지 최대 20회 재스캔(매회 200ms 대기). */
	for (sleep_count = 0; sleep_count < 20; ++sleep_count) {
		/* Scan until a new device is found */
		num_devices = vmd_scan_single_bus(bus, bus->self, true);   /* [한국어] 핫플러그 모드로 버스 재스캔. */
		if (num_devices > 0) {
			break;   /* [한국어] 새 디바이스 발견 → 종료. */
		}

		spdk_delay_us(200000);   /* [한국어] 링크/디바이스 준비 대기(200ms). */
	}

	/* [한국어] 20회 동안 못 찾으면 타임아웃 에러 로깅. */
	if (num_devices == 0) {
		SPDK_ERRLOG("Timed out while scanning for hotplugged devices\n");
	}
}

/*
 * [한국어]
 * vmd_remove_device - 디바이스를 제거 예정으로 표시하고, 미attach 상태면 즉시 detach
 *
 * @device: 제거할 디바이스
 * @return: 없음
 *
 * 디바이스 제거는 NVMe 드라이버가 아직 그 디바이스를 사용 중(attached)일 수 있으므로,
 * 우선 pending_removal 플래그만 세운다. 드라이버가 attach하지 않은 상태라면 안전하게
 * 바로 detach한다. attached 상태면 드라이버가 사용을 마치고 detach 콜백을 호출할 때
 * 실제 제거가 일어난다(지연 제거). 핫리무브/remove RPC 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   vmd_bus_handle_hotremove()/spdk_vmd_remove_device()/vmd_detach_device() → [이 함수] → vmd_dev_detach
 */
static void
vmd_remove_device(struct vmd_pci_device *device)
{
	device->pci.internal.pending_removal = true;   /* [한국어] 제거 예정 마킹(드라이버가 detach 시 확인). */

	/* If the device isn't attached, remove it immediately */
	/* [한국어] 드라이버가 attach 안 했으면 안전하게 즉시 detach. */
	if (!device->pci.internal.attached) {
		vmd_dev_detach(&device->pci);
	}
}

/*
 * [한국어]
 * vmd_bus_handle_hotremove - 버스에서 사라진 디바이스를 감지해 제거 처리
 *
 * @bus: 핫리무브 이벤트가 발생한 버스
 * @return: 없음
 *
 * 버스의 디바이스 리스트를 순회하며 더 이상 config에 존재하지 않는(vmd_bus_device_present
 * 실패) 디바이스를 찾아 vmd_remove_device로 제거한다. TAILQ_FOREACH_SAFE를 쓰는 이유는
 * 순회 중 리스트에서 노드가 제거(detach)될 수 있기 때문이다. 핫플러그 모니터 단일 스레드.
 *
 * 호출 체인:
 *   spdk_vmd_hotplug_monitor() → [vmd_bus_handle_hotremove] → vmd_remove_device
 */
static void
vmd_bus_handle_hotremove(struct vmd_pci_bus *bus)
{
	struct vmd_pci_device *device, *tmpdev;   /* [한국어] 순회 커서 / 안전 순회용 다음 노드 보관. */

	/* [한국어] SAFE 순회: 순회 중 device가 detach되어 리스트에서 빠질 수 있으므로 tmpdev로 보호. */
	TAILQ_FOREACH_SAFE(device, &bus->dev_list, tailq, tmpdev) {
		/* [한국어] config에 더 이상 없으면 물리적으로 빠진 것 → 제거. */
		if (!vmd_bus_device_present(bus, device->devfn)) {
			vmd_remove_device(device);
		}
	}
}

/*
 * [한국어]
 * spdk_vmd_hotplug_monitor - 모든 VMD 슬롯의 status를 폴링해 hot-plug/remove를 처리(공개 API)
 *
 * @return: 이번 호출에서 처리한 핫플러그 이벤트 수
 *
 * VMD는 인터럽트가 VMD 벡터 0로 수렴하므로, SPDK는 이 함수를 주기적으로(poller에서)
 * 호출해 각 핫플러그 슬롯 브리지의 PCIe slot_status.datalink_state_changed 비트를
 * 폴링한다. 변경이 감지되면 link_status.datalink_layer_active로 삽입/제거를 구분해
 * vmd_bus_handle_hotplug 또는 vmd_bus_handle_hotremove를 호출하고, 마지막에 RW1C
 * status 비트를 클리어해 이벤트를 ack한다. SPDK 애플리케이션의 폴러가 호출하는 단일
 * 스레드 컨텍스트.
 *
 * 호출 체인:
 *   외부 SPDK poller → [spdk_vmd_hotplug_monitor] → vmd_bus_handle_hotplug/hotremove/clear_status
 */
int
spdk_vmd_hotplug_monitor(void)
{
	struct vmd_pci_bus *bus;        /* [한국어] 버스 순회 커서. */
	struct vmd_pci_device *device;  /* [한국어] 슬롯 브리지 디바이스. */
	int num_hotplugs = 0;           /* [한국어] 처리한 이벤트 수. */
	uint32_t i;                     /* [한국어] VMD 인덱스. */

	/* [한국어] 모든 VMD의 모든 버스를 순회. */
	for (i = 0; i < g_vmd_container.count; ++i) {
		TAILQ_FOREACH(bus, &g_vmd_container.vmd[i].bus_list, tailq) {
			device = bus->self;   /* [한국어] 버스의 상위 브리지. */
			/* [한국어] 브리지가 없거나 핫플러그 비대상이면 건너뜀. */
			if (device == NULL || !device->hotplug_capable) {
				continue;
			}

			/* [한국어] datalink_state_changed 비트가 안 섰으면 이벤트 없음 → 다음 버스. */
			if (device->pcie_cap->slot_status.bit_field.datalink_state_changed != 1) {
				continue;
			}

			/* [한국어] 링크가 active면 삽입(hotplug), 아니면 제거(hotremove)로 판단. */
			if (device->pcie_cap->link_status.bit_field.datalink_layer_active == 1) {
				SPDK_INFOLOG(vmd, "Device hotplug detected on bus "
					     "%"PRIu32"\n", bus->bus_number);
				vmd_bus_handle_hotplug(bus);   /* [한국어] 새 디바이스 재스캔/초기화. */
			} else {
				SPDK_INFOLOG(vmd, "Device hotremove detected on bus "
					     "%"PRIu32"\n", bus->bus_number);
				vmd_bus_handle_hotremove(bus);   /* [한국어] 사라진 디바이스 제거. */
			}

			vmd_clear_hotplug_status(bus);   /* [한국어] RW1C status 비트 클리어(이벤트 ack). */
			num_hotplugs++;                  /* [한국어] 처리 이벤트 수 증가. */
		}
	}

	return num_hotplugs;   /* [한국어] 처리한 이벤트 수 반환. */
}

/*
 * [한국어]
 * spdk_vmd_remove_device - 주소로 지정한 VMD 뒤 디바이스를 제거(공개 API)
 *
 * @addr: 제거할 디바이스의 PCI 주소
 * @return: 0 성공, -ENODEV(못 찾음)
 *
 * 사용자/RPC가 명시적으로 VMD 뒤 디바이스를 제거할 때 호출한다. vmd_find_device로 찾아
 * type이 "vmd"임을 검증한 뒤 vmd_remove_device로 제거(또는 지연 제거)한다. 단일 스레드.
 *
 * 호출 체인:
 *   외부 SPDK 코드/RPC → [spdk_vmd_remove_device] → vmd_find_device/vmd_remove_device
 */
int
spdk_vmd_remove_device(const struct spdk_pci_addr *addr)
{
	struct vmd_pci_device *device;   /* [한국어] 찾은 디바이스. */

	device = vmd_find_device(addr);   /* [한국어] 주소로 디바이스 검색. */
	if (device == NULL) {
		return -ENODEV;   /* [한국어] 못 찾으면 ENODEV. */
	}

	assert(strcmp(spdk_pci_device_get_type(&device->pci), "vmd") == 0);   /* [한국어] VMD 디바이스인지 검증. */
	vmd_remove_device(device);   /* [한국어] 제거(즉시 또는 지연). */

	return 0;   /* [한국어] 성공. */
}

/*
 * [한국어]
 * spdk_vmd_rescan - 모든 VMD 버스를 핫플러그 모드로 재스캔해 새 디바이스를 흡수(공개 API)
 *
 * @return: 이번 재스캔에서 새로 발견·초기화한 디바이스 총 개수
 *
 * 폴링 기반 핫플러그가 아닌, 사용자가 명시적으로 트리거하는 전체 재스캔이다. 모든 VMD의
 * 모든 버스에 대해 vmd_scan_single_bus(hotplug=true)를 호출해 새로 삽입된 엔드포인트를
 * 찾아 초기화한다(기존 브리지 트리는 보존). 단일 스레드.
 *
 * 호출 체인:
 *   외부 SPDK 코드/RPC → [spdk_vmd_rescan] → vmd_scan_single_bus
 */
int
spdk_vmd_rescan(void)
{
	struct vmd_pci_bus *bus;   /* [한국어] 버스 순회 커서. */
	uint32_t i;                /* [한국어] VMD 인덱스. */
	int rc = 0;                /* [한국어] 발견 개수 누적. */

	/* [한국어] 모든 VMD의 모든 버스를 핫플러그 모드로 재스캔. */
	for (i = 0; i < g_vmd_container.count; ++i) {
		TAILQ_FOREACH(bus, &g_vmd_container.vmd[i].bus_list, tailq) {
			rc += vmd_scan_single_bus(bus, bus->self, true);
		}
	}

	return rc;   /* [한국어] 새로 발견한 디바이스 총 개수. */
}

/*
 * [한국어]
 * vmd_attach_device - PCI provider attach 콜백: 주소로 지정한 VMD 뒤 엔드포인트를 attach
 *
 * @addr: attach할 디바이스의 PCI 주소
 * @return: 0 성공, -ENODEV(미존재/브리지/초기화 실패)
 *
 * SPDK PCI 서브시스템이 g_vmd_device_provider를 통해 호출하는 콜백이다. 주소의 domain/
 * bus로 해당 VMD와 버스를 찾고, addr->dev 슬롯에서 디바이스를 새로 alloc해 초기화한다.
 * VMD는 항상 function 0이므로 func!=0이면 거부하며, 브리지는 attach 대상이 아니므로
 * (엔드포인트만 허용) 거부한다. 성공하면 vmd_init_end_device로 NVMe hook까지 완료한다.
 * 사용자/RPC 기반 attach 단일 스레드 컨텍스트.
 *
 * 호출 체인:
 *   SPDK PCI subsystem → g_vmd_device_provider.attach_cb == [vmd_attach_device] →
 *   vmd_alloc_dev/vmd_init_end_device
 */
static int
vmd_attach_device(const struct spdk_pci_addr *addr)
{
	struct vmd_pci_bus *bus;       /* [한국어] 대상 버스. */
	struct vmd_adapter *vmd;       /* [한국어] 대상 VMD 어댑터. */
	struct vmd_pci_device *dev;    /* [한국어] 새로 만들 디바이스. */
	uint32_t i;                    /* [한국어] VMD 인덱스. */
	int rc;                        /* [한국어] 초기화 결과. */

	/* VMD always sets function to zero */
	/* [한국어] VMD 뒤 디바이스는 항상 function 0 → 그 외 func는 거부. */
	if (addr->func != 0) {
		return -ENODEV;
	}

	/* [한국어] domain이 일치하는 VMD를 찾는다. */
	for (i = 0; i < g_vmd_container.count; ++i) {
		vmd = &g_vmd_container.vmd[i];
		if (vmd->domain != addr->domain) {
			continue;
		}

		/* [한국어] 그 VMD에서 bus 번호가 일치하는 버스를 찾는다. */
		TAILQ_FOREACH(bus, &vmd->bus_list, tailq) {
			if (bus->bus_number != addr->bus) {
				continue;
			}

			dev = vmd_alloc_dev(bus, addr->dev);   /* [한국어] 해당 슬롯의 디바이스 생성. */
			if (dev == NULL) {
				return -ENODEV;   /* [한국어] 슬롯이 비었거나 중복 → 실패. */
			}

			/* Only allow attaching endpoint devices */
			/* [한국어] 브리지는 attach 대상이 아님 → 폐기 후 거부. */
			if (dev->header->common.header_type & PCI_HEADER_TYPE_BRIDGE) {
				free(dev);
				return -ENODEV;
			}

			rc = vmd_init_end_device(dev);   /* [한국어] BAR 할당 + NVMe hook. */
			if (rc != 0) {
				free(dev);            /* [한국어] 초기화 실패 → 폐기. */
				return -ENODEV;
			}

			return 0;   /* [한국어] attach 성공. */
		}
	}

	return -ENODEV;   /* [한국어] 일치하는 VMD/버스를 못 찾음. */
}

/*
 * [한국어]
 * vmd_detach_device - PCI provider detach 콜백: 디바이스를 제거 처리로 위임
 *
 * @pci_dev: detach할 SPDK pci device(vmd_pci_device.pci)
 * @return: 없음
 *
 * SPDK PCI 서브시스템이 detach 시 호출하는 콜백이다. 컨테이너를 복원해 type/존재를
 * 검증한 뒤 vmd_remove_device로 제거(즉시 또는 지연)를 위임한다. detach 단일 스레드.
 *
 * 호출 체인:
 *   SPDK PCI subsystem → g_vmd_device_provider.detach_cb == [vmd_detach_device] → vmd_remove_device
 */
static void
vmd_detach_device(struct spdk_pci_device *pci_dev)
{
	struct vmd_pci_device *dev = SPDK_CONTAINEROF(pci_dev, struct vmd_pci_device, pci);   /* [한국어] 컨테이너 복원. */

	assert(strcmp(spdk_pci_device_get_type(pci_dev), "vmd") == 0);   /* [한국어] VMD 디바이스인지 검증. */
	assert(vmd_find_device(&pci_dev->addr) != NULL);                 /* [한국어] 트리에 실제 존재하는지 검증. */

	vmd_remove_device(dev);   /* [한국어] 제거 처리 위임. */
}

/* [한국어] VMD를 SPDK PCI 서브시스템에 "디바이스 제공자(provider)"로 등록하는 디스크립터.
 *          이를 통해 SPDK가 "vmd" 타입 디바이스의 attach/detach를 이 파일의 콜백으로 위임. */
static struct spdk_pci_device_provider g_vmd_device_provider = {
	.name = "vmd",                    /* [한국어] 제공자 이름(디바이스 type 태그와 일치). */
	.attach_cb = vmd_attach_device,   /* [한국어] attach 요청 콜백. */
	.detach_cb = vmd_detach_device,   /* [한국어] detach 요청 콜백. */
};

/* [한국어] constructor 시점에 g_vmd_device_provider를 SPDK PCI 서브시스템에 자동 등록하는 매크로.
 *          main 진입 전에 실행되어, 런타임에 별도 등록 코드 없이 provider가 활성화된다. */
SPDK_PCI_REGISTER_DEVICE_PROVIDER(vmd, &g_vmd_device_provider);

/*
 * [한국어]
 * spdk_vmd_init - 시스템의 모든 VMD를 enumerate하여 뒤의 NVMe를 SPDK에 노출(공개 진입점)
 *
 * @return: spdk_pci_enumerate의 반환값(0 성공, 음수 실패)
 *
 * 이 파일의 최상위 진입점이다. spdk_pci_vmd_get_driver()로 VMD 드라이버를 얻어
 * spdk_pci_enumerate()를 호출하면, 매칭되는 각 VMD 엔드포인트마다 vmd_enum_cb가 호출되어
 * BAR 매핑 + 내부 PCIe 트리 스캔 + NVMe hook이 수행된다. 결과는 전역 g_vmd_container에
 * 누적된다. SPDK 애플리케이션 초기화 단계의 단일 스레드에서 호출.
 *
 * 호출 체인:
 *   외부 SPDK 초기화 → [spdk_vmd_init] → spdk_pci_enumerate → vmd_enum_cb
 */
int
spdk_vmd_init(void)
{
	/* [한국어] VMD 드라이버 매칭 디바이스를 모두 열거하며 각각 vmd_enum_cb로 처리. */
	return spdk_pci_enumerate(spdk_pci_vmd_get_driver(), vmd_enum_cb, &g_vmd_container);
}

/*
 * [한국어]
 * spdk_vmd_fini - 열거했던 모든 VMD 엔드포인트를 detach하여 정리(공개 종료점)
 *
 * @return: 없음
 *
 * spdk_vmd_init의 짝으로, 컨테이너에 등록된 각 VMD 엔드포인트를 spdk_pci_device_detach로
 * 분리한다. 애플리케이션 종료 단계의 단일 스레드에서 호출.
 *
 * 호출 체인:
 *   외부 SPDK 종료 → [spdk_vmd_fini] → spdk_pci_device_detach
 */
void
spdk_vmd_fini(void)
{
	uint32_t i;   /* [한국어] VMD 인덱스. */

	/* [한국어] 등록된 모든 VMD 엔드포인트를 차례로 detach. */
	for (i = 0; i < g_vmd_container.count; ++i) {
		spdk_pci_device_detach(g_vmd_container.vmd[i].pci);
	}
}

/* [한국어] 이 파일의 로그 컴포넌트 "vmd"를 등록하는 매크로. SPDK_INFOLOG(vmd, ...) 등이
 *          이 컴포넌트 이름으로 필터링/출력되도록 constructor 시점에 등록한다. */
SPDK_LOG_REGISTER_COMPONENT(vmd)
