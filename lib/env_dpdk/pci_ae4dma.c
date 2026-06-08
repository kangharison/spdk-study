/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

/*
 * [한국어 설명] AMD AE4DMA DMA 엔진용 PCI 드라이버 등록 모듈 (pci_ae4dma.c)
 *
 * === 파일의 역할 ===
 * AMD의 AE4DMA(Async Engine 4 DMA) 가속기를 SPDK가 인식할 수 있도록 DPDK PCI 서브시스템에
 * 등록하는 매우 작은 글루(glue) 파일이다. 본 파일은 자체적인 디바이스 로직을 가지지 않으며,
 * 단지 (Vendor ID = AMD, Device ID = AE4DMA_3E/4E)를 갖는 PCI 디바이스가 발견되면 SPDK가
 * 그것을 "ae4dma" 드라이버 카테고리로 분류하도록 매칭 테이블만 제공한다. 실제 디바이스
 * 초기화·DMA 큐 관리는 별도의 ae4dma 라이브러리(module 또는 user code)가 spdk_pci_enumerate
 * 콜백을 통해 수행한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 초기화 흐름에서 spdk_env_init() → pci_env_init() → 본 파일이 등록한 드라이버 객체가
 * DPDK rte_pci_register()로 전달되어, 이후 rte_bus_scan()이 매칭 디바이스를 발견할 때
 * "이것은 ae4dma 디바이스다"라는 라벨을 부착한다. 즉 본 파일은 디바이스 enumeration 경로의
 * 시작점에 해당하며, 실행 컨텍스트는 메인 스레드(EAL 초기화 단계)이다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: env_internal.h(SPDK_PCI_DRIVER_REGISTER 매크로 제공), spdk/pci_ids.h(AMD VID와
 *   AE4DMA Device ID 매크로 정의).
 * - 본 파일에 의존: 상위 ae4dma user-space 드라이버(미공개 또는 별도 모듈)가
 *   spdk_pci_ae4dma_get_driver()를 호출하여 enumerate 시작점을 얻는다.
 * - 공유 자료구조: spdk_pci_id(vendor_id/device_id 매칭 테이블), spdk_pci_driver(드라이버 핸들).
 *
 * === 주요 함수/구조체 요약 ===
 * - ae4dma_driver_id[]: 본 드라이버가 매칭할 (VID, DID) 쌍 배열. sentinel(vendor_id = 0)로 종료.
 * - spdk_pci_ae4dma_get_driver(): 외부에서 ae4dma 드라이버 핸들을 얻기 위한 진입점.
 * - SPDK_PCI_DRIVER_REGISTER(ae4dma, ...): 컴파일/링크 시점에 글로벌 드라이버 레지스트리에
 *   본 드라이버를 추가하는 생성자 매크로(constructor attribute 사용).
 */

/* [한국어] env_dpdk 내부 헤더. SPDK_PCI_DRIVER_REGISTER 매크로와 spdk_pci_get_driver
 * 선언이 들어 있어 PCI 드라이버 등록·조회에 필수. */
#include "env_internal.h"

/* [한국어] SPDK가 알고 있는 모든 PCI vendor/device ID 매크로 모음.
 * 본 파일은 SPDK_PCI_VID_AMD, PCI_DEVICE_ID_AMD_AE4DMA_3E/4E를 가져다 쓴다. */
#include "spdk/pci_ids.h"

/* [한국어] AE4DMA 디바이스 ID 항목을 짧게 작성하기 위한 헬퍼 매크로.
 * VID는 항상 AMD이므로 매번 적지 않고 DEVICE_ID만 받아서 (VID, DID) 쌍을 만든다. */
#define SPDK_AE4DMA_PCI_DEVICE(DEVICE_ID) SPDK_PCI_DEVICE(SPDK_PCI_VID_AMD, DEVICE_ID)
/* [한국어] 이 드라이버가 매칭할 PCI 디바이스 ID 테이블.
 * 설정자: 컴파일 타임 정적 초기화.
 * 읽는 자: SPDK_PCI_DRIVER_REGISTER 매크로가 본 배열을 DPDK rte_pci_id 형태로 변환.
 * 값 범위: AE4DMA 3E와 4E 두 가지 변종을 포함하며, 마지막은 sentinel(vendor_id == 0). */
static struct spdk_pci_id ae4dma_driver_id[] = {
	{SPDK_AE4DMA_PCI_DEVICE(PCI_DEVICE_ID_AMD_AE4DMA_3E)}, /* [한국어] AMD AE4DMA 3E 변종 */
	{SPDK_AE4DMA_PCI_DEVICE(PCI_DEVICE_ID_AMD_AE4DMA_4E)}, /* [한국어] AMD AE4DMA 4E 변종 */
	{ .vendor_id = 0, /* sentinel */ }, /* [한국어] 배열 종료 표시. vendor_id == 0은 enumeration 루프 종료 조건. */
};

/*
 * [한국어]
 * spdk_pci_ae4dma_get_driver - "ae4dma" PCI 드라이버 핸들 반환
 *
 * @return: SPDK_PCI_DRIVER_REGISTER로 등록된 spdk_pci_driver* (없으면 NULL).
 *
 * 상위 ae4dma 모듈/유틸리티가 spdk_pci_enumerate() 또는 spdk_pci_device_attach()를 호출할 때
 * 첫 번째 인자로 넘길 드라이버 객체를 얻기 위한 진입점이다. 내부적으로 등록 시 사용한 이름
 * 문자열("ae4dma")로 글로벌 드라이버 리스트를 조회한다.
 * 실행 컨텍스트: 메인 스레드. PCI enumerate 시작 직전에 호출됨.
 *
 * 호출 체인: ae4dma user driver → spdk_pci_ae4dma_get_driver → spdk_pci_get_driver
 */
struct spdk_pci_driver *
spdk_pci_ae4dma_get_driver(void)
{
	/* [한국어] 등록 시 이름("ae4dma")으로 글로벌 드라이버 테이블을 조회.
	 * 일치하는 항목이 없다면 본 파일이 빌드에서 빠졌거나 등록 순서 오류이며 NULL이 반환된다. */
	return spdk_pci_get_driver("ae4dma");
}

/* [한국어] SPDK PCI 드라이버 등록 매크로. constructor attribute로 main 진입 전에 자동 실행되어
 * 이름 "ae4dma", 매칭 테이블 ae4dma_driver_id, 플래그 SPDK_PCI_DRIVER_NEED_MAPPING으로 등록한다.
 * SPDK_PCI_DRIVER_NEED_MAPPING: 디바이스 BAR을 mmap()으로 사용자 공간에 매핑해야 한다는 의미
 * (DMA를 위해 VFIO/uio 드라이버 바인딩이 필요). */
SPDK_PCI_DRIVER_REGISTER(ae4dma, ae4dma_driver_id, SPDK_PCI_DRIVER_NEED_MAPPING);
