/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * PCI device ID list
 */

/*
 * [한국어 설명] SPDK 전역 PCI Vendor / Device / Class ID 매크로 모음 (pci_ids.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 가 PCI 버스를 통해 인식·바인딩·식별해야 하는 모든 디바이스의
 * Vendor ID(VID) / Device ID(DID) / Class Code 를 컴파일 타임 상수 매크로로 모아둔
 * 단일 source-of-truth 헤더이다. SPDK 는 커널 NVMe / Linux UIO 드라이버를 우회하고
 * 유저스페이스에서 vfio-pci / uio_pci_generic 으로 디바이스를 직접 잡기 때문에,
 * 어떤 PCI 디바이스를 "SPDK 가 다룰 수 있는 NVMe / IOAT / VMD / virtio-blk / virtio-scsi /
 * AMD AE4DMA / Intel DSA / Intel IAA 디바이스" 로 분류할 것인지 호스트 부팅 시점에
 * 정적으로 알 수 있어야 하며, 본 헤더가 그 카탈로그 역할을 한다.
 * 또한 일부 NVMe 디바이스는 표준 NVMe 동작에서 벗어나는 호환성 quirk(예: stripe size,
 * Open-Channel SSD, vendor-specific identify) 이 필요한데, 이 quirk lookup 키도 본
 * 헤더의 VID/DID 매크로를 그대로 사용한다 (lib/nvme/nvme_quirks.c).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 본 헤더는 SPDK 의 가장 하부 레이어인 PCI 디바이스 enumeration / probe 경로의 입력값을
 * 제공한다. 호출(=참조) 체인:
 *   [SPDK 앱 시작] spdk_env_init()
 *     → [lib/env_dpdk/pci.c] DPDK rte_pci_register() 로 드라이버별 id_table 등록
 *         · NVMe driver: { vid=PCI_ANY_ID, did=PCI_ANY_ID, class=SPDK_PCI_CLASS_NVME }
 *         · IOAT driver: 본 헤더의 PCI_DEVICE_ID_INTEL_IOAT_* 전체 나열
 *         · VMD driver:  PCI_DEVICE_ID_INTEL_VMD_*
 *         · virtio:      PCI_DEVICE_ID_VIRTIO_*
 *         · DSA/IAA:     PCI_DEVICE_ID_INTEL_DSA / IAA
 *         · AMD AE4DMA:  PCI_DEVICE_ID_AMD_AE4DMA_*
 *     → DPDK EAL 이 sysfs (/sys/bus/pci/devices) 를 스캔하며 일치하는 디바이스를
 *        해당 SPDK 드라이버로 attach
 *     → [lib/nvme/nvme_quirks.c] NVMe attach 시 nvme_get_quirks(pci_id) 가 본 헤더의
 *        VID/DID 와 매치되는 quirk 마스크(NVME_INTEL_QUIRK_*, NVME_QUIRK_OCSSD,
 *        NVME_QUIRK_IDENTIFY_CNS 등) 를 반환
 * 모든 매크로는 컴파일 타임 상수이며, 실행 컨텍스트는 SPDK env init phase (초기화)
 * 또는 attach callback (보통 메인 reactor) 이다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(이 헤더가 사용하는 것):
 *   - <spdk/stdinc.h>: 표준 정수 타입(직접 등장하지 않지만 SPDK 헤더 컨벤션상 포함).
 * 역의존(이 헤더를 사용하는 것):
 *   - lib/env_dpdk/pci.c, pci_event.c — PCI driver registration / hotplug.
 *   - lib/env_dpdk/pci_nvme.c, pci_ioat.c, pci_virtio.c, pci_vmd.c, pci_dsa.c,
 *     pci_iaa.c, pci_idxd.c — 각 디바이스 클래스별 id_table.
 *   - lib/nvme/nvme_quirks.c, nvme_pcie.c — NVMe 디바이스의 vendor 별 quirk 매핑.
 *   - lib/ioat/ioat.c — Intel IOAT(I/O Acceleration Technology) DMA 엔진 식별.
 *   - lib/vmd/vmd.c — Intel VMD(Volume Management Device) 식별.
 *   - module/bdev/virtio/* — virtio-blk / virtio-scsi 식별.
 *   - examples/nvme/identify, app/spdk_nvme_perf 등 — VID 기반 출력 표시.
 * 데이터 흐름: PCI sysfs 의 /vendor /device /class 파일 → DPDK EAL 의 rte_pci_device →
 *   본 헤더 매크로와 비교 → 일치 시 해당 SPDK 드라이버 probe 콜백 호출.
 *
 * === 주요 함수/구조체 요약 ===
 * 본 파일은 함수/구조체 정의 없이 매크로만 노출한다. 매크로 군은 다음 4 개 카테고리:
 *   1) 와일드카드 / 상수: SPDK_PCI_ANY_ID(0xffff), SPDK_PCI_CLASS_ANY_ID(0xffffff).
 *      DPDK 의 RTE_PCI_ANY_ID 와 동치로, "vid 무관/did 무관" id_table 엔트리 작성에 사용.
 *   2) Vendor ID (SPDK_PCI_VID_*): Intel(0x8086), AMD(0x1022), Samsung(0x144d),
 *      CNEX Labs(0x1d1d, OCSSD 컨트롤러 제조사), VirtualBox(0x80ee), virtio(0x1af4),
 *      VMware(0x15ad), Red Hat(0x1b36, QEMU 가상 디바이스), Nutanix, Huawei, Microsoft,
 *      Micron, Memblaze 등.
 *   3) Class Code: SPDK_PCI_CLASS_NVME(0x010802) — PCI 스펙 base 01h(mass storage)
 *      / sub 08h(NVM) / prog-if 02h(NVMe) 의 packed 24-bit 표현.
 *   4) Device ID 카탈로그:
 *      · Intel IOAT (Sandy Bridge, Ivy Bridge, Haswell, Broadwell, Bay Trail/BWD,
 *        Skylake, Ice Lake): PCI_DEVICE_ID_INTEL_IOAT_*
 *      · Intel VMD: VMD_SKX(0x201d), VMD_ICX(0x28c0). VMD root port: 0x2030~33, 0x347a~7d.
 *      · Intel 가속기: DSA(0x0b25 — Data Streaming Accelerator), IAA(0x0cfe — In-Memory
 *        Analytics Accelerator).
 *      · virtio-blk/scsi/fs (legacy 1.0 / modern 1.1): 0x1001/1004/1042/1048/0x105A.
 *      · virtio vhost-user transitional: 0x1017.
 *      · AMD AE4DMA: 0x14dc / 0x149b.
 *
 * 약어 풀이:
 *   - VID/DID: PCI Vendor ID / Device ID (PCIe spec §6.2.1, config space offset 0x00/0x02).
 *   - Class Code: PCIe config space offset 0x09(prog-if), 0x0a(subclass), 0x0b(base class).
 *   - IOAT: Intel I/O Acceleration Technology (CPU 통합 DMA 엔진, lib/ioat).
 *   - VMD: Intel Volume Management Device (CPU 내부에서 NVMe 핫플러그/RAID 를 위한
 *     PCIe 도메인 격리 컨트롤러).
 *   - DSA / IAA: Intel 가속기. SPDK 는 lib/idxd 를 통해 사용 (memcpy/compress 가속).
 *   - OCSSD: Open-Channel SSD. CNEX Labs 가 1d1d:* 디바이스로 첫 상용화.
 */

#ifndef SPDK_PCI_IDS
#define SPDK_PCI_IDS
/* [한국어] include guard. SPDK 빌드 시 본 헤더가 lib/env_dpdk, lib/nvme, lib/ioat 등
 * 다수 모듈에서 동시 인클루드되므로 중복 정의 방지 필수. */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 인클루드 래퍼. 본 헤더는 함수 시그니처가 없어 표준 정수 타입에
 * 직접 의존하지 않지만, SPDK 공개 헤더 컨벤션상 stdinc.h 를 가장 먼저 끌어와 빌드
 * 환경 차이(_GNU_SOURCE, _DEFAULT_SOURCE feature test macros) 를 일관되게 적용한다. */

#ifdef __cplusplus
extern "C" {
#endif
/* [한국어] C++ 사용자 코드(예: bdev_virtio backend 의 일부, 외부 SDK 통합)에서 본
 * 헤더를 인클루드할 때 C 링키지를 강제. 매크로 정의에는 직접 영향 없지만, 동일
 * 헤더 안에 함수 prototype 이 추가되어도 호환되도록 SPDK 컨벤션을 따른다. */

#define SPDK_PCI_ANY_ID			0xffff
/* [한국어] PCI VID/DID 매칭에서 "wildcard(아무 값이나 매치)" 로 쓰이는 sentinel.
 * DPDK rte_pci_id 테이블의 vendor_id/device_id 필드에 들어가 "이 vid 또는 이 did 는
 * 신경 쓰지 않는다" 를 표현. 사용처: lib/env_dpdk/pci_nvme.c 의 NVMe id_table —
 * { .vendor_id = SPDK_PCI_ANY_ID, .device_id = SPDK_PCI_ANY_ID,
 *   .class = SPDK_PCI_CLASS_NVME } 처럼 클래스로만 NVMe 를 잡을 때. 0xffff 는 PCI 스펙상
 * "Vendor ID 0xFFFF" 가 reserved/invalid 라 sentinel 로 안전. */
#define SPDK_PCI_VID_INTEL		0x8086
/* [한국어] Intel Corporation 의 PCI Vendor ID. "8086" 은 8086 CPU 에서 유래한 Intel
 * 의 시그니처 VID. 사용처: NVMe quirk 테이블(Intel P3700/P4500 등의 stripe size
 * 워크어라운드), IOAT/VMD/DSA/IAA 식별, lib/nvme/nvme_quirks.c 의 NVME_INTEL_QUIRK_*. */
#define SPDK_PCI_VID_AMD		0x1022
/* [한국어] Advanced Micro Devices VID. SPDK 에서는 주로 AMD AE4DMA(AMD EPYC PSP
 * 파생 DMA 엔진) 식별과 AMD 시스템에서의 IOMMU/PCIe 토폴로지 quirks 에 사용. */
#define SPDK_PCI_VID_MEMBLAZE		0x1c5f
/* [한국어] Memblaze (PBlaze NVMe SSD 제조사) VID. NVMe quirk lookup 에 사용 —
 * 일부 Memblaze 디바이스가 vendor-specific identify 동작을 보여 lib/nvme/nvme_quirks.c
 * 에서 호환성 플래그를 부여. */
#define SPDK_PCI_VID_SAMSUNG		0x144d
/* [한국어] Samsung Electronics VID. Samsung NVMe SSD (PM9A3, PM1733 등) 식별과
 * Samsung 특정 NVMe quirks (NVME_QUIRK_NO_LOG_PAGES 등) lookup 에 사용. */
#define SPDK_PCI_VID_VIRTUALBOX		0x80ee
/* [한국어] Oracle VirtualBox 가상 머신의 가상 디바이스 VID. SPDK 를 VirtualBox VM
 * 안에서 테스트할 때 가상 NVMe 컨트롤러를 식별하기 위한 용도. */
#define SPDK_PCI_VID_VIRTIO		0x1af4
/* [한국어] virtio (Red Hat / QEMU) 가상 디바이스 transport VID. SPDK 의 module/bdev/virtio
 * 와 lib/virtio 가 게스트 측에서 virtio-blk/virtio-scsi PCI 디바이스를 잡을 때 매치
 * 키로 사용. virtio 1.0 spec §4.1.2 에서 모든 virtio PCI 디바이스가 0x1af4 를 사용하도록 규정. */
#define SPDK_PCI_VID_CNEXLABS		0x1d1d
/* [한국어] CNEX Labs VID. Open-Channel SSD (OCSSD) 컨트롤러 제조사로, SPDK 의
 * lib/nvme/nvme_quirks.c 에서 NVME_QUIRK_OCSSD 비트와 매핑되어 본 컨트롤러를 NVMe
 * 표준 동작이 아닌 OCSSD 1.2/2.0 명령 셋(spdk/nvme_ocssd.h) 으로 다루도록 분기시키는
 * 핵심 식별자. */
#define SPDK_PCI_VID_VMWARE		0x15ad
/* [한국어] VMware VID. VMware ESXi/Workstation 가상 머신의 가상 NVMe/SCSI 디바이스
 * 식별. SPDK 를 VMware 게스트 환경에서 구동할 때 사용. */
#define SPDK_PCI_VID_REDHAT		0x1b36
/* [한국어] Red Hat VID. QEMU 가 노출하는 가상 NVMe 컨트롤러(0x1b36:0x0010) 와 가상
 * SCSI 등의 식별에 사용. SPDK 를 QEMU/KVM 게스트에서 테스트할 때 매우 자주 매치된다. */
#define SPDK_PCI_VID_NUTANIX		0x4e58
/* [한국어] Nutanix VID. Nutanix AHV(Acropolis Hypervisor) 의 가상 디바이스 식별 —
 * SPDK 를 Nutanix 환경의 게스트에서 사용할 때를 대비. */
#define SPDK_PCI_VID_HUAWEI		0x19e5
/* [한국어] Huawei VID. Huawei 자체 NVMe SSD (ES3000 등) 와 일부 Huawei 가상화 환경
 * 디바이스 식별. NVMe quirks 테이블 매칭에 사용. */
#define SPDK_PCI_VID_MICROSOFT		0x1414
/* [한국어] Microsoft VID. Hyper-V 가상 NVMe 컨트롤러 / Azure NVMe Direct 디바이스
 * 식별. SPDK 를 Azure VM 의 NVMe local SSD 에 바인딩할 때 매치된다. */
#define SPDK_PCI_VID_MICRON		0x1344
/* [한국어] Micron Technology VID. Micron NVMe SSD (9300, 7400 시리즈 등) 식별과
 * Micron 특정 quirk lookup 에 사용. */

#define SPDK_PCI_CLASS_ANY_ID		0xffffff
/* [한국어] PCI Class Code 매칭의 와일드카드. 24-bit class 필드 전부를 1 로 채워
 * "어떤 클래스든 매치" 를 표현. id_table 에서 vendor/device 만 보고 클래스는 무관하게
 * 잡을 때 사용 (예: 특정 VID 의 모든 디바이스). */
/**
 * PCI class code for NVMe devices.
 *
 * Base class code 01h: mass storage
 * Subclass code 08h: non-volatile memory
 * Programming interface 02h: NVM Express
 */
#define SPDK_PCI_CLASS_NVME		0x010802
/* [한국어] NVMe 디바이스를 식별하는 24-bit packed PCI Class Code.
 * 구성: [base=0x01 mass storage] [sub=0x08 NVM] [prog-if=0x02 NVM Express]
 *  = 0x010802 (PCI BASE specification 의 class code table 참조).
 * 사용처: lib/env_dpdk/pci_nvme.c 의 NVMe driver id_table 에서 vendor/device 를
 *   wildcard(SPDK_PCI_ANY_ID) 로 두고 본 클래스 코드만으로 모든 NVMe 컨트롤러를
 *   포괄 매치. 이 덕분에 SPDK 는 새로 출시되는 NVMe SSD (DID 가 새로 부여됨) 도
 *   별도 코드 변경 없이 자동으로 인식한다.
 * 동의: NVMe 컨트롤러는 PCIe config space 의 class code 가 반드시 0x010802 여야 한다는
 *   NVMe over PCIe transport spec 의 요구사항을 그대로 따른다. */

#define PCI_DEVICE_ID_INTEL_DSA		0x0b25
/* [한국어] Intel DSA (Data Streaming Accelerator) DID. Sapphire Rapids 이후 Xeon CPU
 * 통합 가속기로, 메모리 복사 / DIF 검증 / CRC 등을 CPU 코어 외부에서 수행. SPDK 는
 * lib/idxd 를 통해 본 디바이스를 잡고 module/accel/dsa 가 spdk_accel API 백엔드로 사용. */
#define PCI_DEVICE_ID_INTEL_IAA		0x0cfe
/* [한국어] Intel IAA (In-memory Analytics Accelerator) DID. Sapphire Rapids 통합
 * 가속기로, Deflate(zlib) 압축/해제 / Huffman 코딩을 가속. SPDK 의 압축 bdev 와
 * spdk_accel 가 lib/idxd 를 통해 사용. */

#define PCI_DEVICE_ID_INTEL_IOAT_SNB0	0x3c20
/* [한국어] Intel IOAT DMA 엔진 — Sandy Bridge-EP 채널 0. 8086:3c2x 군은 Sandy Bridge
 * Xeon E5 v1 통합 IOAT(=Crystal Beach 3.0) 의 DMA 채널 8 개(SNB0~7) + 보조 2 개(SNB8/9). */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB1	0x3c21
/* [한국어] Intel IOAT DMA — Sandy Bridge-EP 채널 1. (SNB0 동일 패밀리, 채널 인덱스만 다름) */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB2	0x3c22
/* [한국어] Intel IOAT DMA — Sandy Bridge-EP 채널 2. */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB3	0x3c23
/* [한국어] Intel IOAT DMA — Sandy Bridge-EP 채널 3. */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB4	0x3c24
/* [한국어] Intel IOAT DMA — Sandy Bridge-EP 채널 4. */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB5	0x3c25
/* [한국어] Intel IOAT DMA — Sandy Bridge-EP 채널 5. */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB6	0x3c26
/* [한국어] Intel IOAT DMA — Sandy Bridge-EP 채널 6. */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB7	0x3c27
/* [한국어] Intel IOAT DMA — Sandy Bridge-EP 채널 7. */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB8	0x3c2e
/* [한국어] Intel IOAT DMA — Sandy Bridge-EP 보조 채널 8 (PCI Function 6). */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB9	0x3c2f
/* [한국어] Intel IOAT DMA — Sandy Bridge-EP 보조 채널 9 (PCI Function 7).
 * SNB 그룹 전체는 lib/env_dpdk/pci_ioat.c 의 IOAT id_table 에 모두 등록되어 lib/ioat
 * 가 발견 시 자동 attach 한다. */

#define PCI_DEVICE_ID_INTEL_IOAT_IVB0	0x0e20
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 0 (Xeon E5 v2). 8086:0e2x 패밀리. */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB1	0x0e21
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 1. */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB2	0x0e22
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 2. */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB3	0x0e23
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 3. */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB4	0x0e24
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 4. */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB5	0x0e25
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 5. */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB6	0x0e26
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 6. */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB7	0x0e27
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 7. */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB8	0x0e2e
/* [한국어] Intel IOAT — Ivy Bridge-EP 보조 채널 8. */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB9	0x0e2f
/* [한국어] Intel IOAT — Ivy Bridge-EP 보조 채널 9. */

#define PCI_DEVICE_ID_INTEL_IOAT_HSW0	0x2f20
/* [한국어] Intel IOAT — Haswell-EP 채널 0 (Xeon E5 v3). 8086:2f2x 패밀리. */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW1	0x2f21
/* [한국어] Intel IOAT — Haswell-EP 채널 1. */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW2	0x2f22
/* [한국어] Intel IOAT — Haswell-EP 채널 2. */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW3	0x2f23
/* [한국어] Intel IOAT — Haswell-EP 채널 3. */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW4	0x2f24
/* [한국어] Intel IOAT — Haswell-EP 채널 4. */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW5	0x2f25
/* [한국어] Intel IOAT — Haswell-EP 채널 5. */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW6	0x2f26
/* [한국어] Intel IOAT — Haswell-EP 채널 6. */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW7	0x2f27
/* [한국어] Intel IOAT — Haswell-EP 채널 7. */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW8	0x2f2e
/* [한국어] Intel IOAT — Haswell-EP 보조 채널 8. */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW9	0x2f2f
/* [한국어] Intel IOAT — Haswell-EP 보조 채널 9. */

#define PCI_DEVICE_ID_INTEL_IOAT_BWD0	0x0C50
/* [한국어] Intel IOAT — Bay Trail / Avoton (Atom C2000 시리즈, 코드네임 BWD/Briarwood)
 * 채널 0. 데스크탑/임베디드 IOAT 변종으로 채널 수가 4 개로 적다. 8086:0C5x. */
#define PCI_DEVICE_ID_INTEL_IOAT_BWD1	0x0C51
/* [한국어] Intel IOAT — Bay Trail/Briarwood 채널 1. */
#define PCI_DEVICE_ID_INTEL_IOAT_BWD2	0x0C52
/* [한국어] Intel IOAT — Bay Trail/Briarwood 채널 2. */
#define PCI_DEVICE_ID_INTEL_IOAT_BWD3	0x0C53
/* [한국어] Intel IOAT — Bay Trail/Briarwood 채널 3. */

#define PCI_DEVICE_ID_INTEL_IOAT_BDXDE0	0x6f50
/* [한국어] Intel IOAT — Broadwell-DE (Xeon D-1500 시리즈) 채널 0. 8086:6f5x.
 * BDX-DE 는 SoC-class Xeon 으로 IOAT 채널 4 개. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDXDE1	0x6f51
/* [한국어] Intel IOAT — Broadwell-DE 채널 1. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDXDE2	0x6f52
/* [한국어] Intel IOAT — Broadwell-DE 채널 2. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDXDE3	0x6f53
/* [한국어] Intel IOAT — Broadwell-DE 채널 3. */

#define PCI_DEVICE_ID_INTEL_IOAT_BDX0	0x6f20
/* [한국어] Intel IOAT — Broadwell-EP (Xeon E5 v4) 채널 0. 8086:6f2x. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX1	0x6f21
/* [한국어] Intel IOAT — Broadwell-EP 채널 1. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX2	0x6f22
/* [한국어] Intel IOAT — Broadwell-EP 채널 2. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX3	0x6f23
/* [한국어] Intel IOAT — Broadwell-EP 채널 3. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX4	0x6f24
/* [한국어] Intel IOAT — Broadwell-EP 채널 4. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX5	0x6f25
/* [한국어] Intel IOAT — Broadwell-EP 채널 5. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX6	0x6f26
/* [한국어] Intel IOAT — Broadwell-EP 채널 6. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX7	0x6f27
/* [한국어] Intel IOAT — Broadwell-EP 채널 7. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX8	0x6f2e
/* [한국어] Intel IOAT — Broadwell-EP 보조 채널 8. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX9	0x6f2f
/* [한국어] Intel IOAT — Broadwell-EP 보조 채널 9. */

#define PCI_DEVICE_ID_INTEL_IOAT_SKX	0x2021
/* [한국어] Intel IOAT — Skylake-SP (Xeon Scalable 1세대) DID. SKX 부터 IOAT 가
 * Crystal Beach 3.2 로 갱신되며 단일 DID 0x2021 로 표현 (채널은 PCI Function 단위로 분기).
 * 사용처: lib/env_dpdk/pci_ioat.c id_table. */

#define PCI_DEVICE_ID_INTEL_IOAT_ICX	0x0b00
/* [한국어] Intel IOAT — Ice Lake-SP (Xeon Scalable 3세대) DID. ICX 부터 IOAT 가
 * Crystal Beach 3.3 로 진화했고 0x0b00 단일 DID. Sapphire Rapids 이후로는 IOAT 가
 * DSA 로 대체되어 신규 DID 가 추가되지 않는다 (DSA 는 위의 0x0b25). */

#define PCI_DEVICE_ID_VIRTIO_BLK_LEGACY	0x1001
/* [한국어] virtio-blk PCI legacy(=virtio 0.95 / transitional) 디바이스 DID. VID 0x1af4
 * 와 짝. QEMU 의 -device virtio-blk-pci 가 transitional 모드에서 노출. SPDK 의
 * module/bdev/virtio_blk 가 매치. virtio 1.0 spec §4.1.2 의 transitional ID 범위
 * 0x1000~0x103f 중 blk 슬롯. */
#define PCI_DEVICE_ID_VIRTIO_SCSI_LEGACY 0x1004
/* [한국어] virtio-scsi PCI legacy 디바이스 DID. VID 0x1af4 와 짝. SPDK 의
 * module/bdev/virtio_scsi 와 lib/virtio 가 vhost-user-scsi 게스트에서 매치. */
#define PCI_DEVICE_ID_VIRTIO_BLK_MODERN	0x1042
/* [한국어] virtio-blk PCI modern(=virtio 1.0+ non-transitional) DID. transitional ID
 * (0x1001) + 0x40 규칙으로 0x1042. modern device 는 legacy I/O port 가 아닌 MMIO
 * config 만 노출. SPDK 가 OVMF/UEFI 게스트에서 주로 매치. */
#define PCI_DEVICE_ID_VIRTIO_SCSI_MODERN 0x1048
/* [한국어] virtio-scsi PCI modern DID. transitional 0x1004 + 0x40. */
#define PCI_DEVICE_ID_VIRTIO_FS		0x105A
/* [한국어] virtio-fs (DAX 기반 공유 파일시스템) PCI DID. SPDK 의 fsdev (lib/fsdev)
 * 와 module/fsdev 가 게스트에서 virtio-fs 디바이스를 잡을 때 사용. */

#define PCI_DEVICE_ID_VIRTIO_VHOST_USER 0x1017
/* [한국어] virtio vhost-user transport 의 transitional PCI DID. SPDK 의 vhost target
 * (lib/vhost) 이 노출하는 vhost-user backend 와 짝이 되는 게스트 측 식별자. */

#define PCI_DEVICE_ID_INTEL_VMD_SKX	0x201d
/* [한국어] Intel VMD (Volume Management Device) — Skylake-SP DID. VMD 는 CPU 내부의
 * "PCIe 도메인 격리" 컨트롤러로, 그 하위에 NVMe SSD 들이 직접 연결되어 OS 가 NVMe 를
 * 직접 보지 못하고 VMD 만 본다. 사용처: lib/vmd/vmd.c 가 본 DID 를 매치하면 BAR 을
 * 매핑하고 VMD 하위 가상 PCIe 트리를 enumerate 하여 NVMe 를 발견. NVMe 핫플러그·LED·
 * RAID 통합용. */
#define PCI_DEVICE_ID_INTEL_VMD_ICX	0x28c0
/* [한국어] Intel VMD — Ice Lake-SP DID. SKX 와 동일 메커니즘이나 하드웨어 세대 갱신
 * (확장 BAR, 더 많은 root port 슬롯). lib/vmd/vmd.c id_table 에 SKX 와 함께 등록. */

#define PCI_ROOT_PORT_A_INTEL_SKX	0x2030
/* [한국어] Intel SKX VMD 가 노출하는 가상 PCIe Root Port A (총 4 개 중 첫 번째).
 * lib/vmd/vmd.c 가 VMD 하위 토폴로지를 traverse 할 때 root port 디바이스를 식별하기
 * 위한 키. 일반 NVMe id_table 에는 들어가지 않으며 VMD 전용. */
#define PCI_ROOT_PORT_B_INTEL_SKX	0x2031
/* [한국어] Intel SKX VMD root port B. */
#define PCI_ROOT_PORT_C_INTEL_SKX	0x2032
/* [한국어] Intel SKX VMD root port C. */
#define PCI_ROOT_PORT_D_INTEL_SKX	0x2033
/* [한국어] Intel SKX VMD root port D (마지막). VMD 한 개당 보통 4 개 root port 가 있어
 * 그 아래로 PCIe lane 을 분배. */
#define PCI_ROOT_PORT_A_INTEL_ICX	0x347a
/* [한국어] Intel ICX VMD root port A. ICX 는 SKX 의 0x2030~33 대역을 0x347a~7d 로 옮김. */
#define PCI_ROOT_PORT_B_INTEL_ICX	0x347b
/* [한국어] Intel ICX VMD root port B. */
#define PCI_ROOT_PORT_C_INTEL_ICX	0x347c
/* [한국어] Intel ICX VMD root port C. */
#define PCI_ROOT_PORT_D_INTEL_ICX	0x347d
/* [한국어] Intel ICX VMD root port D. */

#define PCI_DEVICE_ID_AMD_AE4DMA_3E 0x14dc
/* [한국어] AMD AE4DMA (AMD EPYC 의 PSP 기반 DMA 엔진) DID — 변종 1 (Genoa/Bergamo 시기).
 * SPDK 의 lib/ae4dma 와 module/accel/ae4dma 가 본 DID 를 매치하여 lib/idxd 와 유사한
 * spdk_accel 백엔드로 사용. AMD VID 0x1022 와 짝. */
#define PCI_DEVICE_ID_AMD_AE4DMA_4E 0x149b
/* [한국어] AMD AE4DMA DID — 변종 2 (이후 EPYC 세대). 0x14dc 와 동일 드라이버에서
 * 함께 매치되어 PCIe Function 단위로 채널 enumerate. */

#ifdef __cplusplus
}
#endif
/* [한국어] 위쪽 extern "C" { 와 짝이 되는 닫는 중괄호. C++ 컴파일러에서만 활성화. */

#endif /* SPDK_PCI_IDS */
/* [한국어] include guard 종료. 본 헤더가 정의한 매크로 군이 한 번역 단위에 한 번만
 * 들어가도록 보장. SPDK 의 거의 모든 PCI 관련 모듈이 본 헤더를 인클루드하므로
 * 중복 정의 충돌을 피하기 위해 필수. */
