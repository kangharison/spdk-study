/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Intel VMD(Volume Management Device) PCI 드라이버 등록 모듈 (pci_vmd.c)
 *
 * === 파일의 역할 ===
 * Intel VMD는 CPU 내부의 PCIe 루트 컴플렉스를 별도 도메인으로 격리해 그 아래의 NVMe들을
 * 하나의 부모 디바이스로 묶어 핫플러그/LED 관리를 단순화하는 기능이다. 본 파일은 VMD 컨트롤러
 * (SKX/ICX 변종) 자체를 SPDK가 PCI 디바이스로 인식할 수 있도록 매칭 테이블을 등록한다.
 * 실제 VMD 도메인 enumerate(VMD 아래의 NVMe 발견)는 lib/vmd 라이브러리에서 수행한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * spdk_env_init() 단계에서 본 드라이버가 자동 등록되고, lib/vmd가
 * spdk_pci_vmd_get_driver()를 통해 enumerate를 시작한다. VMD가 활성화된 시스템에서는
 * NVMe 디바이스가 직접 보이지 않고 VMD를 통해 노출되므로 NVMe 드라이버보다 먼저 초기화돼야 한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: env_internal.h, spdk/pci_ids.h.
 * - 본 파일에 의존: lib/vmd, module/bdev/nvme(VMD 활성 시).
 * - 공유 자료구조: spdk_pci_id, spdk_pci_driver.
 *
 * === 주요 함수/구조체 요약 ===
 * - vmd_pci_driver_id[]: Intel VMD SKX/ICX 두 변종 ID 테이블.
 * - spdk_pci_vmd_get_driver(): vmd 드라이버 핸들 반환.
 * - SPDK_PCI_DRIVER_REGISTER(vmd, ..., NEED_MAPPING | WC_ACTIVATE): 자동 등록.
 */

/* [한국어] env_dpdk 내부 헤더. 드라이버 등록 매크로 제공. */
#include "env_internal.h"

/* [한국어] Intel VMD vendor/device ID 매크로. */
#include "spdk/pci_ids.h"

/* [한국어] Intel VMD 매칭 테이블.
 * 설정자: 컴파일 타임 정적 초기화. 읽는 자: SPDK_PCI_DRIVER_REGISTER.
 * 값 범위: Skylake-X(SKX), Ice Lake(ICX) 변종. 마지막은 sentinel. */
static struct spdk_pci_id vmd_pci_driver_id[] = {
	{ SPDK_PCI_DEVICE(SPDK_PCI_VID_INTEL, PCI_DEVICE_ID_INTEL_VMD_SKX) }, /* [한국어] Skylake-X VMD */
	{ SPDK_PCI_DEVICE(SPDK_PCI_VID_INTEL, PCI_DEVICE_ID_INTEL_VMD_ICX) }, /* [한국어] Ice Lake VMD */
	{ .vendor_id = 0, /* sentinel */ }, /* [한국어] 종료 표시. */
};

/*
 * [한국어]
 * spdk_pci_vmd_get_driver - "vmd" PCI 드라이버 핸들 반환
 *
 * @return: 등록된 spdk_pci_driver*.
 *
 * lib/vmd가 VMD 컨트롤러를 attach할 때 사용하는 진입점.
 * 실행 컨텍스트: 메인 스레드.
 *
 * 호출 체인: lib/vmd → spdk_pci_vmd_get_driver → spdk_pci_get_driver
 */
struct spdk_pci_driver *
spdk_pci_vmd_get_driver(void)
{
	/* [한국어] 등록 이름 "vmd"로 글로벌 드라이버 테이블 조회. */
	return spdk_pci_get_driver("vmd");
}

/* [한국어] VMD 드라이버 자동 등록.
 * NEED_MAPPING: VMD config BAR을 사용자공간에 매핑하여 enumerate.
 * WC_ACTIVATE: Write-Combining 매핑(MMIO write coalescing). */
SPDK_PCI_DRIVER_REGISTER(vmd, vmd_pci_driver_id,
			 SPDK_PCI_DRIVER_NEED_MAPPING | SPDK_PCI_DRIVER_WC_ACTIVATE);
