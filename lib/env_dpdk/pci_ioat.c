/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Intel IOAT(I/O Acceleration Technology) PCI 드라이버 등록 모듈 (pci_ioat.c)
 *
 * === 파일의 역할 ===
 * Intel CPU 내장 DMA 엔진인 IOAT(Crystal Beach 시리즈)의 모든 세대(SNB, IVB, HSW, BWD, BDXDE,
 * BDX, SKX, ICX)에 해당하는 PCI 디바이스 ID들을 SPDK PCI 서브시스템에 등록한다. 본 파일은
 * 매칭 테이블만 제공하며, 실제 디스크립터 큐 운영은 lib/ioat에서 수행한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * spdk_env_init() → 본 파일의 SPDK_PCI_DRIVER_REGISTER constructor → DPDK rte_pci_register.
 * 실행 컨텍스트는 메인 스레드(EAL init 직후). lib/ioat이 spdk_pci_ioat_get_driver()로 enumerate.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: env_internal.h, spdk/pci_ids.h.
 * - 본 파일에 의존: lib/ioat, module/accel/ioat.
 * - 공유 자료구조: spdk_pci_id, spdk_pci_driver.
 *
 * === 주요 함수/구조체 요약 ===
 * - ioat_driver_id[]: 8세대×다중 변종의 거대 매칭 테이블(48여 개 항목).
 *   각 세대 약어: SNB=Sandy Bridge, IVB=Ivy Bridge, HSW=Haswell, BWD=Broadwell-DE,
 *   BDXDE=Broadwell-DE, BDX=Broadwell, SKX=Skylake-X, ICX=Ice Lake.
 * - spdk_pci_ioat_get_driver(): IOAT 드라이버 핸들 반환.
 * - SPDK_PCI_DRIVER_REGISTER(ioat, ...): 자동 등록.
 */

/* [한국어] env_dpdk 내부 헤더. 드라이버 등록 매크로 제공. */
#include "env_internal.h"

/* [한국어] Intel IOAT 디바이스 ID 매크로 모음. */
#include "spdk/pci_ids.h"

/* [한국어] Intel VID 고정 헬퍼 매크로. */
#define SPDK_IOAT_PCI_DEVICE(DEVICE_ID) SPDK_PCI_DEVICE(SPDK_PCI_VID_INTEL, DEVICE_ID)
/* [한국어] IOAT 매칭 테이블.
 * 설정자: 컴파일 타임 정적 초기화.
 * 읽는 자: SPDK_PCI_DRIVER_REGISTER 매크로.
 * 값 범위: Sandy Bridge ~ Ice Lake 세대의 IOAT 채널 번호별 PCI Function ID들. */
static struct spdk_pci_id ioat_driver_id[] = {
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB0)}, /* [한국어] Sandy Bridge IOAT 채널 0 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB1)}, /* [한국어] SNB 채널 1 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB2)}, /* [한국어] SNB 채널 2 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB3)}, /* [한국어] SNB 채널 3 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB4)}, /* [한국어] SNB 채널 4 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB5)}, /* [한국어] SNB 채널 5 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB6)}, /* [한국어] SNB 채널 6 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB7)}, /* [한국어] SNB 채널 7 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB8)}, /* [한국어] SNB 채널 8 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB0)}, /* [한국어] Ivy Bridge 채널 0 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB1)}, /* [한국어] IVB 채널 1 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB2)}, /* [한국어] IVB 채널 2 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB3)}, /* [한국어] IVB 채널 3 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB4)}, /* [한국어] IVB 채널 4 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB5)}, /* [한국어] IVB 채널 5 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB6)}, /* [한국어] IVB 채널 6 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB7)}, /* [한국어] IVB 채널 7 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB8)}, /* [한국어] IVB 채널 8 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB9)}, /* [한국어] IVB 채널 9 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW0)}, /* [한국어] Haswell 채널 0 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW1)}, /* [한국어] HSW 채널 1 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW2)}, /* [한국어] HSW 채널 2 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW3)}, /* [한국어] HSW 채널 3 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW4)}, /* [한국어] HSW 채널 4 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW5)}, /* [한국어] HSW 채널 5 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW6)}, /* [한국어] HSW 채널 6 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW7)}, /* [한국어] HSW 채널 7 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW8)}, /* [한국어] HSW 채널 8 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW9)}, /* [한국어] HSW 채널 9 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BWD0)}, /* [한국어] Broadwell 채널 0 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BWD1)}, /* [한국어] BWD 채널 1 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BWD2)}, /* [한국어] BWD 채널 2 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BWD3)}, /* [한국어] BWD 채널 3 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDXDE0)}, /* [한국어] Broadwell-DE 채널 0 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDXDE1)}, /* [한국어] BDXDE 채널 1 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDXDE2)}, /* [한국어] BDXDE 채널 2 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDXDE3)}, /* [한국어] BDXDE 채널 3 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX0)},   /* [한국어] Broadwell 채널 0 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX1)},   /* [한국어] BDX 채널 1 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX2)},   /* [한국어] BDX 채널 2 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX3)},   /* [한국어] BDX 채널 3 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX4)},   /* [한국어] BDX 채널 4 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX5)},   /* [한국어] BDX 채널 5 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX6)},   /* [한국어] BDX 채널 6 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX7)},   /* [한국어] BDX 채널 7 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX8)},   /* [한국어] BDX 채널 8 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX9)},   /* [한국어] BDX 채널 9 */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SKX)},    /* [한국어] Skylake-X IOAT */
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_ICX)},    /* [한국어] Ice Lake IOAT */
	{ .vendor_id = 0, /* sentinel */ }, /* [한국어] 배열 끝. */
};

/*
 * [한국어]
 * spdk_pci_ioat_get_driver - "ioat" PCI 드라이버 핸들 반환
 *
 * @return: 등록된 spdk_pci_driver*.
 *
 * lib/ioat이 IOAT DMA 엔진을 attach할 때 사용하는 진입점.
 * 실행 컨텍스트: 메인 스레드.
 *
 * 호출 체인: lib/ioat → spdk_pci_ioat_get_driver → spdk_pci_get_driver
 */
struct spdk_pci_driver *
spdk_pci_ioat_get_driver(void)
{
	/* [한국어] 등록 이름 "ioat"로 글로벌 드라이버 테이블 조회. */
	return spdk_pci_get_driver("ioat");
}

/* [한국어] IOAT 드라이버 자동 등록.
 * NEED_MAPPING: IOAT 디스크립터 ring 제어를 위한 BAR mmap 필요. */
SPDK_PCI_DRIVER_REGISTER(ioat, ioat_driver_id, SPDK_PCI_DRIVER_NEED_MAPPING);
