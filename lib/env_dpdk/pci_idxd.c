/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Intel IDXD(DSA/IAA) 가속기용 PCI 드라이버 등록 모듈 (pci_idxd.c)
 *
 * === 파일의 역할 ===
 * Intel IDXD 패밀리(Data Streaming Accelerator = DSA, In-Memory Analytics Accelerator = IAA)
 * 디바이스를 SPDK가 알아볼 수 있도록 PCI 매칭 테이블만 등록하는 글루 파일이다. 실제 IDXD
 * 큐(work queue) 관리·디스크립터 제출은 module/accel/idxd 또는 lib/idxd 측에서 수행한다.
 * 본 파일은 (VID = Intel, DID = DSA/IAA)인 디바이스가 발견되면 SPDK가 "idxd" 카테고리로
 * 라벨을 붙이게 만든다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * spdk_env_init() → SPDK_PCI_DRIVER_REGISTER constructor → DPDK rte_pci_register 경로의
 * 일부이며, 실행 컨텍스트는 EAL 초기화 직후 메인 스레드이다. SPDK 가속(accel) 프레임워크
 * 측에서 idxd 모듈을 로드할 때 spdk_pci_idxd_get_driver()를 호출해 enumerate를 시작한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: env_internal.h(드라이버 등록 매크로), spdk/pci_ids.h(Intel VID/DSA·IAA DID).
 * - 본 파일에 의존: lib/idxd 또는 module/accel/idxd.
 * - 공유 자료구조: spdk_pci_id 테이블, spdk_pci_driver 핸들.
 *
 * === 주요 함수/구조체 요약 ===
 * - idxd_driver_id[]: Intel DSA + IAA 두 디바이스 ID를 갖는 매칭 테이블.
 * - spdk_pci_idxd_get_driver(): idxd 드라이버 핸들 반환 진입점.
 * - SPDK_PCI_DRIVER_REGISTER(idxd, ...): 자동 등록 매크로.
 */

/* [한국어] env_dpdk 내부 헤더. SPDK_PCI_DRIVER_REGISTER, spdk_pci_get_driver를 제공한다. */
#include "env_internal.h"

/* [한국어] PCI VID/DID 매크로 정의. Intel과 DSA/IAA 디바이스 ID 사용. */
#include "spdk/pci_ids.h"

/* [한국어] Intel VID 고정 + DEVICE_ID만 가변으로 받는 헬퍼 매크로. */
#define SPDK_IDXD_PCI_DEVICE(DEVICE_ID) SPDK_PCI_DEVICE(SPDK_PCI_VID_INTEL, DEVICE_ID)
/* [한국어] 본 드라이버가 매칭할 디바이스 ID 배열.
 * 설정자: 컴파일 타임 정적 초기화. 읽는 자: SPDK_PCI_DRIVER_REGISTER 매크로.
 * 값 범위: Intel DSA와 IAA 두 종, 마지막은 sentinel. */
static struct spdk_pci_id idxd_driver_id[] = {
	{SPDK_IDXD_PCI_DEVICE(PCI_DEVICE_ID_INTEL_DSA)}, /* [한국어] Intel Data Streaming Accelerator */
	{SPDK_IDXD_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IAA)}, /* [한국어] Intel In-Memory Analytics Accelerator */
	{ .vendor_id = 0, /* sentinel */ }, /* [한국어] 배열 끝 표시. */
};

/*
 * [한국어]
 * spdk_pci_idxd_get_driver - "idxd" PCI 드라이버 핸들 반환
 *
 * @return: 등록된 spdk_pci_driver*. 본 파일이 빌드에 포함되지 않았다면 NULL.
 *
 * lib/idxd 또는 module/accel/idxd가 디바이스 enumerate를 시작할 때 호출한다.
 * 실행 컨텍스트: 메인 스레드.
 *
 * 호출 체인: idxd accel module → spdk_pci_idxd_get_driver → spdk_pci_get_driver
 */
struct spdk_pci_driver *
spdk_pci_idxd_get_driver(void)
{
	/* [한국어] 등록 이름 "idxd"로 글로벌 드라이버 테이블 조회. */
	return spdk_pci_get_driver("idxd");
}

/* [한국어] 컴파일 타임 자동 드라이버 등록. SPDK_PCI_DRIVER_NEED_MAPPING:
 * IDXD 디바이스는 portal MMIO 영역에 직접 워크 디스크립터를 써넣어야 하므로 BAR mmap이 필수. */
SPDK_PCI_DRIVER_REGISTER(idxd, idxd_driver_id, SPDK_PCI_DRIVER_NEED_MAPPING);
