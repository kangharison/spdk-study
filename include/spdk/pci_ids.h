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
/* [한국어] include guard 매크로 SPDK_PCI_IDS.
 * 역할: 동일 번역 단위(translation unit) 안에서 본 헤더가 두 번 이상 전개되는 것을
 *   방지하여 매크로 중복 정의 경고/에러를 차단.
 * 설정자: 본 라인의 #define 만이 유일한 설정자.
 * 읽는 자: 같은 헤더의 끝 #endif 와, 본 헤더를 인클루드하는 모든 .c/.cpp/.h 파일
 *   (lib/env_dpdk/*, lib/nvme/*, lib/ioat/*, lib/vmd/*, module/bdev/virtio/* 등 다수).
 * 값 범위: 정의 여부만 의미가 있으며 값은 무관(빈 매크로). 사용 패턴은 #ifndef/#define/#endif 트리오.
 * 동기화: 전처리기 단계에서만 평가되므로 멀티스레드 동기화 무관.
 *   단, 동일 번역 단위 내 인클루드 순서와 무관하게 한 번만 전개되어야 하므로 SPDK
 *   공개 헤더의 표준 컨벤션을 따른다. */

#include "spdk/stdinc.h"
/* [한국어] #include 지시어 — SPDK 표준 인클루드 래퍼.
 * 역할: <stdint.h>, <stdbool.h>, <stddef.h>, <inttypes.h> 등 SPDK 공개 헤더가 공통으로
 *   요구하는 표준 C 헤더 묶음과 feature test macros (_GNU_SOURCE 등) 를 한 번에 끌어옴.
 * 설정자: include/spdk/stdinc.h 본문이 정의.
 * 읽는 자: 본 헤더에서는 함수 시그니처가 없어 표준 정수 타입에 직접 의존하지 않지만,
 *   본 헤더를 다시 인클루드하는 .c 파일이 동일한 표준 헤더 노출을 기대하므로 컨벤션상 포함.
 * 값 범위: 헤더 인클루드 — 매크로 값 없음.
 * 동기화: 전처리기 단계만 영향. 빌드 환경(POSIX feature test) 의 일관성을 보장하기 위함. */

#ifdef __cplusplus
extern "C" {
#endif
/* [한국어] C++ 컴파일러용 링키지 가드 시작.
 * 역할: 본 헤더를 C++ 코드(예: bdev_virtio 의 일부 외부 SDK 통합) 가 인클루드할 때
 *   매크로/심볼이 C name mangling 으로 처리되도록 강제.
 * 설정자: __cplusplus 매크로(C++ 컴파일러가 자동 정의) 가 진입 조건.
 * 읽는 자: 동일 헤더 끝부분의 짝(closing brace) 과 본 헤더를 C++ 에서 인클루드하는 모든 코드.
 * 값 범위: 정의 여부만 영향. 본 헤더는 매크로만 노출하므로 직접적인 mangling 영향은 없으나,
 *   향후 함수 prototype 이 추가될 때를 대비한 컨벤션.
 * 동기화: 전처리기 단계만 영향, 멀티스레드 무관. */

#define SPDK_PCI_ANY_ID			0xffff
/* [한국어] PCI Vendor ID / Device ID 매칭의 와일드카드 sentinel.
 * 역할: DPDK rte_pci_id (id_table 엔트리) 의 vendor_id 또는 device_id 필드에 들어가
 *   "이 필드는 매칭에 사용하지 않음(any)" 을 표현.
 * 설정자: 본 헤더의 #define 만이 유일한 설정자(컴파일 타임 상수).
 * 읽는 자: lib/env_dpdk/pci_nvme.c 가 NVMe id_table 작성 시 vendor_id/device_id 모두에
 *   본 값을 채워 클래스 코드(SPDK_PCI_CLASS_NVME) 만으로 매칭. DPDK rte_pci_register()
 *   가 sysfs 스캔 결과와 비교할 때 본 sentinel 을 보고 wildcard 로 처리.
 * 값 범위: 16-bit 0xFFFF 고정. PCI 스펙상 VID 0xFFFF 는 reserved/invalid 이므로 실 디바이스가
 *   가질 수 없어 sentinel 로 충돌 없음. PCIe config space 의 offset 0x00-0x01(VID),
 *   offset 0x02-0x03(DID) 모두에 동일한 sentinel 의미.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define SPDK_PCI_VID_INTEL		0x8086
/* [한국어] Intel Corporation 의 PCI Vendor ID(VID).
 * 역할: PCIe config space offset 0x00-0x01 의 Vendor ID 필드 값으로, PCI 디바이스 제조사가
 *   Intel 임을 식별하는 16-bit 상수. "8086" 은 Intel 8086 CPU 모델명에서 유래한 Intel 의
 *   시그니처 VID(PCI-SIG 가 Intel 에 할당).
 * 설정자: 본 헤더의 #define 이 유일.
 * 읽는 자: lib/env_dpdk/pci_nvme.c (Intel NVMe quirk 매칭), pci_ioat.c (Intel IOAT
 *   id_table), pci_vmd.c (VMD), pci_dsa.c, pci_iaa.c, pci_idxd.c (Intel 가속기들),
 *   lib/nvme/nvme_quirks.c (NVME_INTEL_QUIRK_* 매핑 — Intel P3700/P4500 등의 stripe size
 *   워크어라운드 적용 키).
 * 값 범위: 16-bit 0x8086 고정. PCI-SIG 공식 할당.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define SPDK_PCI_VID_AMD		0x1022
/* [한국어] Advanced Micro Devices(AMD) 의 PCI VID.
 * 역할: PCIe config offset 0x00-0x01 가 0x1022 인 디바이스를 AMD 제품으로 식별.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/ae4dma, module/accel/ae4dma 가 PCI_DEVICE_ID_AMD_AE4DMA_* 와 짝지어
 *   AMD EPYC 의 PSP 파생 DMA 엔진 식별. 또한 일부 AMD 시스템에서의 IOMMU/PCIe 토폴로지
 *   quirks 분기에도 참조될 수 있음.
 * 값 범위: 16-bit 0x1022 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define SPDK_PCI_VID_MEMBLAZE		0x1c5f
/* [한국어] Memblaze (중국 PBlaze NVMe SSD 제조사) 의 PCI VID.
 * 역할: PCIe config offset 0x00-0x01 가 0x1c5f 인 디바이스를 Memblaze 제품으로 식별.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/nvme/nvme_quirks.c — 일부 Memblaze 디바이스가 vendor-specific identify
 *   동작이나 비표준 admin 명령 응답을 보여 호환성 플래그(quirk bitmap) 부여.
 * 값 범위: 16-bit 0x1c5f 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define SPDK_PCI_VID_SAMSUNG		0x144d
/* [한국어] Samsung Electronics 의 PCI VID.
 * 역할: PCIe config offset 0x00-0x01 가 0x144d 인 디바이스를 Samsung 제품으로 식별.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/nvme/nvme_quirks.c — Samsung NVMe SSD (PM9A3, PM1733, PM1735, 980 PRO 등)
 *   에 대한 NVME_QUIRK_NO_LOG_PAGES, NVME_QUIRK_DELAY_AFTER_QUEUE_ALLOC 등 vendor 특화
 *   quirk 매핑 키.
 * 값 범위: 16-bit 0x144d 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define SPDK_PCI_VID_VIRTUALBOX		0x80ee
/* [한국어] Oracle VirtualBox 가상화 솔루션의 PCI VID.
 * 역할: VirtualBox VM 이 게스트에 노출하는 가상 디바이스(가상 NVMe 등) 의 vendor 식별.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: NVMe quirk 테이블 — VirtualBox 가 호스트보다 약한 NVMe semantics 를 흉내내므로
 *   특정 quirk 가 필요할 때 매칭. SPDK 를 VirtualBox VM 안에서 학습/테스트 환경 구축 시 활용.
 * 값 범위: 16-bit 0x80ee 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define SPDK_PCI_VID_VIRTIO		0x1af4
/* [한국어] virtio (Red Hat / QEMU) PCI transport 의 표준 VID.
 * 역할: virtio 1.0 spec §4.1.2 에서 모든 virtio PCI 디바이스(virtio-blk, virtio-scsi,
 *   virtio-net, virtio-fs 등) 가 동일하게 사용하도록 규정한 단일 vendor 식별자. PCIe
 *   config offset 0x00-0x01 가 0x1af4 이면 virtio 디바이스로 간주.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/virtio/virtio_pci.c 와 module/bdev/virtio_blk, module/bdev/virtio_scsi 가
 *   게스트 측에서 본 VID 와 PCI_DEVICE_ID_VIRTIO_* 의 조합으로 virtio 디바이스를 매치.
 * 값 범위: 16-bit 0x1af4 고정. (참고: legacy DID 1000h~103Fh, modern DID 1040h~107Fh)
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define SPDK_PCI_VID_CNEXLABS		0x1d1d
/* [한국어] CNEX Labs 의 PCI VID. Open-Channel SSD(OCSSD) 컨트롤러 제조사.
 * 역할: PCIe config offset 0x00-0x01 가 0x1d1d 인 디바이스를 CNEX Labs OCSSD 컨트롤러로 식별.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/nvme/nvme_quirks.c 가 본 VID 와 매치되면 NVME_QUIRK_OCSSD 플래그를 세워
 *   해당 컨트롤러를 NVMe 표준 명령 셋이 아닌 OCSSD 1.2/2.0 명령 셋(include/spdk/nvme_ocssd.h)
 *   으로 다루도록 드라이버 분기. OCSSD 는 host-managed FTL 을 위한 vendor-specific 명령
 *   (vector chunk read/write, get/set chunk info) 을 사용.
 * 값 범위: 16-bit 0x1d1d 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define SPDK_PCI_VID_VMWARE		0x15ad
/* [한국어] VMware Inc. 의 PCI VID.
 * 역할: VMware ESXi / Workstation / Fusion 가상 머신이 게스트에 노출하는 가상 NVMe / SCSI
 *   /paravirt 디바이스 vendor 식별.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/nvme/nvme_quirks.c 가 일부 VMware 가상 NVMe 의 응답 quirk 매칭에 사용.
 *   SPDK 를 VMware 게스트 환경에서 구동할 때 디바이스 enumeration 시 매치.
 * 값 범위: 16-bit 0x15ad 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define SPDK_PCI_VID_REDHAT		0x1b36
/* [한국어] Red Hat 의 PCI VID.
 * 역할: QEMU 가 노출하는 가상 NVMe 컨트롤러(예: 0x1b36:0x0010) 와 일부 Red Hat 가상
 *   SCSI/RNG 디바이스의 vendor 식별. virtio 가 아닌 "Red Hat 자체 가상 디바이스" 군이
 *   본 VID 를 사용.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: SPDK 의 NVMe driver 가 QEMU/KVM 게스트에서 테스트될 때 본 VID 를 가진 가상
 *   NVMe 컨트롤러를 SPDK_PCI_CLASS_NVME 클래스 매칭으로 attach.
 * 값 범위: 16-bit 0x1b36 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define SPDK_PCI_VID_NUTANIX		0x4e58
/* [한국어] Nutanix Inc. 의 PCI VID.
 * 역할: Nutanix AHV(Acropolis Hypervisor) 가 게스트에 노출하는 가상 디바이스 vendor 식별.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/nvme/nvme_quirks.c — Nutanix 환경에서의 NVMe 가상화 quirk 매칭. SPDK 를
 *   Nutanix 게스트 안에서 사용할 때 디바이스 인식.
 * 값 범위: 16-bit 0x4e58 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define SPDK_PCI_VID_HUAWEI		0x19e5
/* [한국어] Huawei Technologies 의 PCI VID.
 * 역할: Huawei 자체 NVMe SSD (예: ES3000 v3/v5) 와 일부 Huawei FusionCompute 가상화 환경
 *   디바이스의 vendor 식별.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/nvme/nvme_quirks.c — Huawei 디바이스에 대한 vendor-specific quirk 매칭.
 * 값 범위: 16-bit 0x19e5 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define SPDK_PCI_VID_MICROSOFT		0x1414
/* [한국어] Microsoft Corporation 의 PCI VID.
 * 역할: Hyper-V 가상 NVMe 컨트롤러 / Azure NVMe Direct (가상화 패스스루 NVMe) 디바이스의
 *   vendor 식별. Azure VM 에서 NVMe local SSD 가 본 VID 로 노출되는 경우가 있음.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/nvme/nvme_quirks.c — Microsoft 디바이스에 대한 quirk 매칭. SPDK 를 Azure VM
 *   에서 사용할 때 NVMe 바인딩 시 매치.
 * 값 범위: 16-bit 0x1414 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define SPDK_PCI_VID_MICRON		0x1344
/* [한국어] Micron Technology 의 PCI VID.
 * 역할: Micron 자체 NVMe SSD (9300, 7400, 7450 시리즈 등) 의 vendor 식별. Crucial 브랜드의
 *   소비자 SSD 도 같은 VID 를 공유.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/nvme/nvme_quirks.c — Micron 디바이스의 vendor-specific quirk 매칭(예:
 *   특정 NVMe admin 명령 타임아웃, identify 응답 layout quirk).
 * 값 범위: 16-bit 0x1344 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */

#define SPDK_PCI_CLASS_ANY_ID		0xffffff
/* [한국어] PCI Class Code 매칭의 와일드카드 sentinel.
 * 역할: DPDK rte_pci_id 의 class_id 필드(24-bit) 에 들어가 "어떤 클래스든 매치" 를 표현.
 *   id_table 에서 vendor/device 로만 식별하고 클래스 코드는 무시할 때 사용.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: 특정 VID/DID 조합으로만 매칭하는 드라이버(예: IOAT, VMD, virtio) 의 id_table
 *   엔트리에서 class_id 필드에 본 sentinel 을 채움. DPDK EAL 이 sysfs 의
 *   /sys/bus/pci/devices/*/class 와 비교할 때 본 값을 보고 wildcard 로 처리.
 * 값 범위: 24-bit 0xFFFFFF 고정. PCIe config space offset 0x09(prog-if), 0x0A(subclass),
 *   0x0B(base class) 의 packed 표현 — 모든 비트가 1 이므로 실제 디바이스가 가질 수 없음.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
/**
 * PCI class code for NVMe devices.
 *
 * Base class code 01h: mass storage
 * Subclass code 08h: non-volatile memory
 * Programming interface 02h: NVM Express
 */
#define SPDK_PCI_CLASS_NVME		0x010802
/* [한국어] NVMe 디바이스를 식별하는 24-bit packed PCI Class Code.
 * 역할: PCIe config space 의 class code 필드(offset 0x09 prog-if, 0x0A subclass,
 *   0x0B base class) 가 본 값과 일치하는 디바이스를 NVMe 컨트롤러로 분류.
 *   구성 비트: [base=0x01 mass storage][sub=0x08 NVM][prog-if=0x02 NVM Express]
 *   → 0x010802 (PCI Base Specification class code table, NVMe over PCIe transport spec 강제).
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/env_dpdk/pci_nvme.c 의 NVMe driver id_table — vendor/device 는
 *   SPDK_PCI_ANY_ID 로 두고 본 클래스 코드만으로 매치하여 신규 NVMe SSD(DID 가 새로
 *   부여됨) 도 코드 변경 없이 자동 인식. lib/nvme/nvme_pcie.c 가 attach 시 추가 검증.
 * 값 범위: 24-bit 0x010802 고정. NVMe spec 이 PCIe transport 컨트롤러에 강제하는 값으로
 *   다른 값일 경우 NVMe 컨트롤러로 보지 않음.
 * 동기화: 컴파일 타임 상수, 동기화 무관. attach 후의 NVMe 명령 동기화는 별도 lib/nvme 레벨 책임. */

#define PCI_DEVICE_ID_INTEL_DSA		0x0b25
/* [한국어] Intel DSA (Data Streaming Accelerator) 의 PCI Device ID(DID).
 * 역할: PCIe config offset 0x02-0x03 가 0x0b25 인 Intel 디바이스를 DSA 가속기로 식별.
 *   DSA 는 Sapphire Rapids(SPR) 이후 Xeon Scalable CPU 통합 가속기로, 메모리 복사 /
 *   DIF(Data Integrity Field) 검증 / CRC / fill 등을 CPU 코어 외부에서 비동기 수행.
 *   shared work queue(SWQ)/dedicated work queue(DWQ) 모델로 동작.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/env_dpdk/pci_dsa.c 와 lib/idxd 가 본 DID + SPDK_PCI_VID_INTEL(0x8086)
 *   조합으로 DSA 디바이스를 발견. module/accel/dsa 가 spdk_accel API 의 백엔드로 사용.
 * 값 범위: 16-bit 0x0b25 고정. SPR 세대 한정 — 차세대에서 새 DID 추가 가능.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define PCI_DEVICE_ID_INTEL_IAA		0x0cfe
/* [한국어] Intel IAA (In-memory Analytics Accelerator) 의 PCI DID.
 * 역할: PCIe config offset 0x02-0x03 가 0x0cfe 인 Intel 디바이스를 IAA 가속기로 식별.
 *   IAA 는 SPR 통합 가속기로, Deflate(zlib) 압축/해제 / Huffman 코딩을 가속하여 SPDK
 *   reduce/compress bdev 가 lib/idxd 를 통해 사용.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/env_dpdk/pci_iaa.c (또는 통합된 idxd 등록), lib/idxd 가 본 DID + Intel VID
 *   조합으로 IAA 발견. module/accel/iaa 가 spdk_accel 백엔드로 등록.
 * 값 범위: 16-bit 0x0cfe 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */

#define PCI_DEVICE_ID_INTEL_IOAT_SNB0	0x3c20
/* [한국어] Intel IOAT DMA 엔진(I/O Acceleration Technology) — Sandy Bridge-EP 채널 0.
 * 역할: PCIe config offset 0x02-0x03 가 0x3c20 인 Intel 디바이스를 SNB-EP 의 IOAT 첫
 *   DMA 채널로 식별. 8086:3c2x 군 전체는 Xeon E5 v1 통합 IOAT(Crystal Beach 3.0) 의
 *   메인 DMA 채널 8 개(SNB0~SNB7) + 보조 채널 2 개(SNB8/SNB9, PCI Function 6/7).
 *   각 채널은 독립된 PCI Function 으로 노출되어 병렬 DMA 가능.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/env_dpdk/pci_ioat.c 의 IOAT id_table, lib/ioat/ioat.c 가 attach 후
 *   채널별 ring/doorbell 매핑하여 spdk_ioat_submit_copy() 등으로 DMA 실행.
 * 값 범위: 16-bit 0x3c20 고정.
 * 동기화: 컴파일 타임 상수. 채널별 ring 동시성은 lib/ioat 레벨 책임. */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB1	0x3c21
/* [한국어] Intel IOAT — Sandy Bridge-EP 채널 1.
 * 역할: SNB-EP IOAT 의 두 번째 DMA 채널 식별. SNB0 와 동일 IOAT 블록의 다른 PCI Function.
 * 설정자/읽는 자/값 범위/동기화: SNB0 와 동일 (DID 만 0x3c21).  */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB2	0x3c22
/* [한국어] Intel IOAT — Sandy Bridge-EP 채널 2.
 * 역할: SNB-EP IOAT 의 세 번째 DMA 채널 식별.
 * 설정자/읽는 자/값 범위/동기화: SNB0 와 동일 (DID 만 0x3c22). */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB3	0x3c23
/* [한국어] Intel IOAT — Sandy Bridge-EP 채널 3.
 * 역할: SNB-EP IOAT 의 네 번째 DMA 채널 식별.
 * 설정자/읽는 자/값 범위/동기화: SNB0 와 동일 (DID 만 0x3c23). */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB4	0x3c24
/* [한국어] Intel IOAT — Sandy Bridge-EP 채널 4.
 * 역할: SNB-EP IOAT 의 다섯 번째 DMA 채널 식별.
 * 설정자/읽는 자/값 범위/동기화: SNB0 와 동일 (DID 만 0x3c24). */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB5	0x3c25
/* [한국어] Intel IOAT — Sandy Bridge-EP 채널 5.
 * 역할: SNB-EP IOAT 의 여섯 번째 DMA 채널 식별.
 * 설정자/읽는 자/값 범위/동기화: SNB0 와 동일 (DID 만 0x3c25). */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB6	0x3c26
/* [한국어] Intel IOAT — Sandy Bridge-EP 채널 6.
 * 역할: SNB-EP IOAT 의 일곱 번째 DMA 채널 식별.
 * 설정자/읽는 자/값 범위/동기화: SNB0 와 동일 (DID 만 0x3c26). */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB7	0x3c27
/* [한국어] Intel IOAT — Sandy Bridge-EP 채널 7 (메인 채널 마지막).
 * 역할: SNB-EP IOAT 의 여덟 번째이자 마지막 메인 DMA 채널 식별.
 * 설정자/읽는 자/값 범위/동기화: SNB0 와 동일 (DID 만 0x3c27). */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB8	0x3c2e
/* [한국어] Intel IOAT — Sandy Bridge-EP 보조 채널 8 (PCI Function 6).
 * 역할: SNB-EP IOAT 의 보조 DMA 채널 — 메인 8 채널과는 별도 PCI Function 으로 노출.
 *   Intel IOAT 사양상 Crystal Beach 의 일부 ChannelGroup 이 별도 Function 으로 분리됨.
 * 설정자/읽는 자/값 범위/동기화: SNB0 와 동일 (DID 만 0x3c2e). */
#define PCI_DEVICE_ID_INTEL_IOAT_SNB9	0x3c2f
/* [한국어] Intel IOAT — Sandy Bridge-EP 보조 채널 9 (PCI Function 7).
 * 역할: SNB-EP IOAT 의 마지막 보조 DMA 채널. SNB0~SNB9 의 10 개 DID 가 lib/env_dpdk/
 *   pci_ioat.c 의 IOAT id_table 에 일괄 등록되어 lib/ioat 가 발견 시 자동 attach 한다.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/env_dpdk/pci_ioat.c, lib/ioat/ioat.c.
 * 값 범위: 16-bit 0x3c2f 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */

#define PCI_DEVICE_ID_INTEL_IOAT_IVB0	0x0e20
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 0 (Xeon E5 v2 통합 IOAT, Crystal Beach 3.1).
 * 역할: PCIe config offset 0x02-0x03 가 0x0e20 인 Intel 디바이스를 IVB-EP IOAT 의 첫
 *   DMA 채널로 식별. 8086:0e2x 군 전체는 채널 0~7 + 보조 8/9 (PCI Function 6/7).
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/env_dpdk/pci_ioat.c id_table, lib/ioat/ioat.c attach 경로.
 * 값 범위: 16-bit 0x0e20 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB1	0x0e21
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 1.
 * 역할/설정자/읽는 자/값 범위/동기화: IVB0 와 동일 (DID 만 0x0e21). */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB2	0x0e22
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 2.
 * 역할/설정자/읽는 자/값 범위/동기화: IVB0 와 동일 (DID 만 0x0e22). */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB3	0x0e23
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 3.
 * 역할/설정자/읽는 자/값 범위/동기화: IVB0 와 동일 (DID 만 0x0e23). */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB4	0x0e24
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 4.
 * 역할/설정자/읽는 자/값 범위/동기화: IVB0 와 동일 (DID 만 0x0e24). */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB5	0x0e25
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 5.
 * 역할/설정자/읽는 자/값 범위/동기화: IVB0 와 동일 (DID 만 0x0e25). */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB6	0x0e26
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 6.
 * 역할/설정자/읽는 자/값 범위/동기화: IVB0 와 동일 (DID 만 0x0e26). */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB7	0x0e27
/* [한국어] Intel IOAT — Ivy Bridge-EP 채널 7 (메인 채널 마지막).
 * 역할/설정자/읽는 자/값 범위/동기화: IVB0 와 동일 (DID 만 0x0e27). */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB8	0x0e2e
/* [한국어] Intel IOAT — Ivy Bridge-EP 보조 채널 8 (PCI Function 6).
 * 역할/설정자/읽는 자/값 범위/동기화: IVB0 와 동일 (DID 만 0x0e2e). */
#define PCI_DEVICE_ID_INTEL_IOAT_IVB9	0x0e2f
/* [한국어] Intel IOAT — Ivy Bridge-EP 보조 채널 9 (PCI Function 7).
 * 역할/설정자/읽는 자/값 범위/동기화: IVB0 와 동일 (DID 만 0x0e2f). */

#define PCI_DEVICE_ID_INTEL_IOAT_HSW0	0x2f20
/* [한국어] Intel IOAT — Haswell-EP 채널 0 (Xeon E5 v3 통합 IOAT, Crystal Beach 3.1+).
 * 역할: PCIe config offset 0x02-0x03 가 0x2f20 인 Intel 디바이스를 HSW-EP IOAT 첫
 *   DMA 채널로 식별. 8086:2f2x 군 전체는 채널 0~7 + 보조 8/9 (10 PCI Function).
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/env_dpdk/pci_ioat.c id_table, lib/ioat/ioat.c.
 * 값 범위: 16-bit 0x2f20 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW1	0x2f21
/* [한국어] Intel IOAT — Haswell-EP 채널 1. 역할/설정자/읽는 자/값/동기화: HSW0 와 동일 (DID 0x2f21). */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW2	0x2f22
/* [한국어] Intel IOAT — Haswell-EP 채널 2. 역할/설정자/읽는 자/값/동기화: HSW0 와 동일 (DID 0x2f22). */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW3	0x2f23
/* [한국어] Intel IOAT — Haswell-EP 채널 3. 역할/설정자/읽는 자/값/동기화: HSW0 와 동일 (DID 0x2f23). */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW4	0x2f24
/* [한국어] Intel IOAT — Haswell-EP 채널 4. 역할/설정자/읽는 자/값/동기화: HSW0 와 동일 (DID 0x2f24). */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW5	0x2f25
/* [한국어] Intel IOAT — Haswell-EP 채널 5. 역할/설정자/읽는 자/값/동기화: HSW0 와 동일 (DID 0x2f25). */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW6	0x2f26
/* [한국어] Intel IOAT — Haswell-EP 채널 6. 역할/설정자/읽는 자/값/동기화: HSW0 와 동일 (DID 0x2f26). */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW7	0x2f27
/* [한국어] Intel IOAT — Haswell-EP 채널 7 (메인 채널 마지막).
 * 역할/설정자/읽는 자/값/동기화: HSW0 와 동일 (DID 0x2f27). */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW8	0x2f2e
/* [한국어] Intel IOAT — Haswell-EP 보조 채널 8 (PCI Function 6).
 * 역할/설정자/읽는 자/값/동기화: HSW0 와 동일 (DID 0x2f2e). */
#define PCI_DEVICE_ID_INTEL_IOAT_HSW9	0x2f2f
/* [한국어] Intel IOAT — Haswell-EP 보조 채널 9 (PCI Function 7).
 * 역할/설정자/읽는 자/값/동기화: HSW0 와 동일 (DID 0x2f2f). */

#define PCI_DEVICE_ID_INTEL_IOAT_BWD0	0x0C50
/* [한국어] Intel IOAT — Bay Trail / Avoton (Atom C2000 시리즈, 코드네임 BWD=Briarwood) 채널 0.
 * 역할: PCIe config offset 0x02-0x03 가 0x0C50 인 Intel 디바이스를 BWD IOAT 첫 DMA 채널로
 *   식별. 데스크탑/임베디드 IOAT 변종으로 채널 수가 4 개로 적음. 8086:0C5x 패밀리.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/env_dpdk/pci_ioat.c id_table, lib/ioat/ioat.c.
 * 값 범위: 16-bit 0x0C50 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define PCI_DEVICE_ID_INTEL_IOAT_BWD1	0x0C51
/* [한국어] Intel IOAT — Bay Trail/Briarwood 채널 1.
 * 역할/설정자/읽는 자/값/동기화: BWD0 와 동일 (DID 0x0C51). */
#define PCI_DEVICE_ID_INTEL_IOAT_BWD2	0x0C52
/* [한국어] Intel IOAT — Bay Trail/Briarwood 채널 2.
 * 역할/설정자/읽는 자/값/동기화: BWD0 와 동일 (DID 0x0C52). */
#define PCI_DEVICE_ID_INTEL_IOAT_BWD3	0x0C53
/* [한국어] Intel IOAT — Bay Trail/Briarwood 채널 3 (마지막).
 * 역할/설정자/읽는 자/값/동기화: BWD0 와 동일 (DID 0x0C53). */

#define PCI_DEVICE_ID_INTEL_IOAT_BDXDE0	0x6f50
/* [한국어] Intel IOAT — Broadwell-DE (Xeon D-1500 시리즈, SoC-class Xeon) 채널 0.
 * 역할: PCIe config offset 0x02-0x03 가 0x6f50 인 Intel 디바이스를 BDX-DE IOAT 첫 채널로
 *   식별. 8086:6f5x 패밀리, 채널 4 개로 축소된 SoC 변종.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/env_dpdk/pci_ioat.c id_table, lib/ioat/ioat.c.
 * 값 범위: 16-bit 0x6f50 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDXDE1	0x6f51
/* [한국어] Intel IOAT — Broadwell-DE 채널 1.
 * 역할/설정자/읽는 자/값/동기화: BDXDE0 와 동일 (DID 0x6f51). */
#define PCI_DEVICE_ID_INTEL_IOAT_BDXDE2	0x6f52
/* [한국어] Intel IOAT — Broadwell-DE 채널 2.
 * 역할/설정자/읽는 자/값/동기화: BDXDE0 와 동일 (DID 0x6f52). */
#define PCI_DEVICE_ID_INTEL_IOAT_BDXDE3	0x6f53
/* [한국어] Intel IOAT — Broadwell-DE 채널 3 (마지막).
 * 역할/설정자/읽는 자/값/동기화: BDXDE0 와 동일 (DID 0x6f53). */

#define PCI_DEVICE_ID_INTEL_IOAT_BDX0	0x6f20
/* [한국어] Intel IOAT — Broadwell-EP (Xeon E5 v4) 채널 0.
 * 역할: PCIe config offset 0x02-0x03 가 0x6f20 인 Intel 디바이스를 BDX-EP IOAT 첫 채널로
 *   식별. 8086:6f2x 패밀리, 채널 0~7 + 보조 8/9 의 10 PCI Function.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/env_dpdk/pci_ioat.c id_table, lib/ioat/ioat.c.
 * 값 범위: 16-bit 0x6f20 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX1	0x6f21
/* [한국어] Intel IOAT — Broadwell-EP 채널 1.
 * 역할/설정자/읽는 자/값/동기화: BDX0 와 동일 (DID 0x6f21). */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX2	0x6f22
/* [한국어] Intel IOAT — Broadwell-EP 채널 2.
 * 역할/설정자/읽는 자/값/동기화: BDX0 와 동일 (DID 0x6f22). */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX3	0x6f23
/* [한국어] Intel IOAT — Broadwell-EP 채널 3.
 * 역할/설정자/읽는 자/값/동기화: BDX0 와 동일 (DID 0x6f23). */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX4	0x6f24
/* [한국어] Intel IOAT — Broadwell-EP 채널 4.
 * 역할/설정자/읽는 자/값/동기화: BDX0 와 동일 (DID 0x6f24). */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX5	0x6f25
/* [한국어] Intel IOAT — Broadwell-EP 채널 5.
 * 역할/설정자/읽는 자/값/동기화: BDX0 와 동일 (DID 0x6f25). */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX6	0x6f26
/* [한국어] Intel IOAT — Broadwell-EP 채널 6.
 * 역할/설정자/읽는 자/값/동기화: BDX0 와 동일 (DID 0x6f26). */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX7	0x6f27
/* [한국어] Intel IOAT — Broadwell-EP 채널 7 (메인 채널 마지막).
 * 역할/설정자/읽는 자/값/동기화: BDX0 와 동일 (DID 0x6f27). */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX8	0x6f2e
/* [한국어] Intel IOAT — Broadwell-EP 보조 채널 8 (PCI Function 6).
 * 역할/설정자/읽는 자/값/동기화: BDX0 와 동일 (DID 0x6f2e). */
#define PCI_DEVICE_ID_INTEL_IOAT_BDX9	0x6f2f
/* [한국어] Intel IOAT — Broadwell-EP 보조 채널 9 (PCI Function 7).
 * 역할/설정자/읽는 자/값/동기화: BDX0 와 동일 (DID 0x6f2f). */

#define PCI_DEVICE_ID_INTEL_IOAT_SKX	0x2021
/* [한국어] Intel IOAT — Skylake-SP (Xeon Scalable 1세대) DID.
 * 역할: PCIe config offset 0x02-0x03 가 0x2021 인 Intel 디바이스를 SKX 통합 IOAT 로 식별.
 *   SKX 부터 IOAT 가 Crystal Beach 3.2 로 갱신되며 모든 채널이 단일 DID 0x2021 로 표현되고,
 *   채널 구분은 PCI Function 단위로 분기.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/env_dpdk/pci_ioat.c 의 IOAT id_table, lib/ioat/ioat.c.
 * 값 범위: 16-bit 0x2021 고정. PCI 도메인 안에 동일 DID 디바이스가 다수 존재할 수 있음(서로 다른 BDF).
 * 동기화: 컴파일 타임 상수, 동기화 무관. */

#define PCI_DEVICE_ID_INTEL_IOAT_ICX	0x0b00
/* [한국어] Intel IOAT — Ice Lake-SP (Xeon Scalable 3세대) DID.
 * 역할: PCIe config offset 0x02-0x03 가 0x0b00 인 Intel 디바이스를 ICX 통합 IOAT 로 식별.
 *   ICX 에서 IOAT 가 Crystal Beach 3.3 로 진화했고 단일 DID 0x0b00 사용. Sapphire Rapids
 *   이후로는 IOAT 가 DSA(0x0b25) 로 대체되어 신규 IOAT DID 가 추가되지 않음.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/env_dpdk/pci_ioat.c id_table, lib/ioat/ioat.c.
 * 값 범위: 16-bit 0x0b00 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */

#define PCI_DEVICE_ID_VIRTIO_BLK_LEGACY	0x1001
/* [한국어] virtio-blk PCI legacy(=virtio 0.95 / transitional) 디바이스 DID.
 * 역할: PCIe config offset 0x00-0x01=0x1af4 (SPDK_PCI_VID_VIRTIO) 와 짝. offset 0x02-0x03 가
 *   0x1001 인 디바이스를 transitional virtio-blk 으로 식별. transitional 디바이스는 legacy
 *   I/O port 와 modern MMIO config 모두를 노출하여 구형 게스트와의 호환성을 유지.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/virtio/virtio_pci.c 와 module/bdev/virtio_blk 가 본 DID 와 SPDK_PCI_VID_VIRTIO
 *   조합으로 매치. QEMU 의 `-device virtio-blk-pci` 가 transitional 모드에서 본 DID 노출.
 * 값 범위: 16-bit 0x1001 고정. virtio 1.0 spec §4.1.2 의 transitional ID 범위(0x1000~0x103F)
 *   중 blk 슬롯.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define PCI_DEVICE_ID_VIRTIO_SCSI_LEGACY 0x1004
/* [한국어] virtio-scsi PCI legacy(transitional) 디바이스 DID.
 * 역할: VID 0x1af4 + DID 0x1004 인 디바이스를 transitional virtio-scsi 컨트롤러로 식별.
 *   virtio-scsi 는 다중 LUN/태그 큐를 지원하는 SCSI host bus adapter 가상 디바이스.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: module/bdev/virtio_scsi 와 lib/virtio 가 vhost-user-scsi 게스트에서 본 DID 를 매치.
 * 값 범위: 16-bit 0x1004 고정. transitional 범위(0x1000~0x103F) 중 scsi 슬롯.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define PCI_DEVICE_ID_VIRTIO_BLK_MODERN	0x1042
/* [한국어] virtio-blk PCI modern(=virtio 1.0+ non-transitional) DID.
 * 역할: VID 0x1af4 + DID 0x1042 인 디바이스를 modern virtio-blk 으로 식별. modern
 *   디바이스는 legacy I/O port 가 없고 MMIO config(virtio_pci_common_cfg/notify_cfg/
 *   isr_cfg/device_cfg) 만 노출하여 PCIe 표준에 정합.
 *   ID 규칙: transitional ID(0x1001) + 0x40 = 0x1042 (virtio 1.0 spec §4.1.2.1).
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/virtio/virtio_pci.c 와 module/bdev/virtio_blk 가 OVMF/UEFI 게스트에서 매치.
 * 값 범위: 16-bit 0x1042 고정. modern 범위(0x1040~0x107F) 의 blk 슬롯.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define PCI_DEVICE_ID_VIRTIO_SCSI_MODERN 0x1048
/* [한국어] virtio-scsi PCI modern DID.
 * 역할: VID 0x1af4 + DID 0x1048 인 디바이스를 modern virtio-scsi 로 식별. transitional
 *   ID(0x1004) + 0x40 = 0x1048 규칙 적용. modern 디바이스는 SR-IOV / message-signaled
 *   interrupt(MSI-X) capability 를 표준 방식으로 노출.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: module/bdev/virtio_scsi, lib/virtio.
 * 값 범위: 16-bit 0x1048 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define PCI_DEVICE_ID_VIRTIO_FS		0x105A
/* [한국어] virtio-fs (DAX-shared 파일시스템 transport) PCI DID.
 * 역할: VID 0x1af4 + DID 0x105A 인 디바이스를 virtio-fs 로 식별. virtiofsd 데몬 또는
 *   SPDK fsdev 가 호스트 디렉토리를 게스트에 공유할 때 사용하는 modern virtio 디바이스.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/fsdev 와 module/fsdev/* 가 게스트에서 본 DID 매치 시 virtio-fs frontend 로 attach.
 * 값 범위: 16-bit 0x105A 고정. modern 범위(0x1040~0x107F) 의 fs 슬롯.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */

#define PCI_DEVICE_ID_VIRTIO_VHOST_USER 0x1017
/* [한국어] virtio vhost-user transport 의 transitional PCI DID.
 * 역할: SPDK 의 vhost target(lib/vhost) 이 호스트에서 vhost-user 백엔드를 노출할 때
 *   짝이 되는 게스트 측 식별자. transitional 범위(0x1000~0x103F) 의 vhost-user 슬롯.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/virtio/virtio_user.c (게스트 측 virtio-user driver 시뮬레이션 시),
 *   lib/vhost (호스트 측 vhost-user backend 식별).
 * 값 범위: 16-bit 0x1017 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */

#define PCI_DEVICE_ID_INTEL_VMD_SKX	0x201d
/* [한국어] Intel VMD (Volume Management Device) — Skylake-SP DID.
 * 역할: PCIe config offset 0x02-0x03 가 0x201d 인 Intel 디바이스를 SKX VMD 컨트롤러로 식별.
 *   VMD 는 CPU 내부의 "PCIe 도메인 격리" 컨트롤러로, 그 하위에 NVMe SSD 들이 직접 연결되어
 *   OS 가 NVMe 를 직접 보지 못하고 VMD 디바이스만 본다. NVMe 핫플러그/LED 표시/RAID 통합용.
 *   VMD 는 자체 BAR(MEMBAR1=Config Space, MEMBAR2=MEM, MEMBAR3=High Mem) 을 통해 하위
 *   가상 PCIe 트리에 접근하게 한다.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/vmd/vmd.c id_table — 매치 시 BAR 매핑 후 spdk_vmd_init() 가 하위 가상
 *   PCIe 트리를 enumerate 하여 NVMe 컨트롤러 발견. lib/env_dpdk/pci_vmd.c 가 DPDK 와 통합.
 * 값 범위: 16-bit 0x201d 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. attach 후 VMD 하위 PCI 트리 탐색은 lib/vmd 의 락 책임. */
#define PCI_DEVICE_ID_INTEL_VMD_ICX	0x28c0
/* [한국어] Intel VMD — Ice Lake-SP DID.
 * 역할: PCIe config offset 0x02-0x03 가 0x28c0 인 Intel 디바이스를 ICX VMD 컨트롤러로 식별.
 *   SKX 와 동일 메커니즘이나 하드웨어 세대 갱신 — 확장 BAR, 더 많은 root port 슬롯, 향상된
 *   인터럽트 라우팅을 가짐.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/vmd/vmd.c id_table 에 SKX 와 함께 등록되어 동일 코드 경로로 attach.
 * 값 범위: 16-bit 0x28c0 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */

#define PCI_ROOT_PORT_A_INTEL_SKX	0x2030
/* [한국어] Intel SKX VMD 가 노출하는 가상 PCIe Root Port A (총 4 개 중 첫 번째).
 * 역할: VMD 컨트롤러 하위에서 가상 PCIe 트리의 root port 디바이스를 식별하는 DID.
 *   VMD 는 자신의 MEMBAR 안에 가상 PCIe 도메인을 만들고, 그 도메인의 root complex 가
 *   본 root port 들을 노출하여 그 아래로 NVMe SSD 들이 enumerate 된다.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/vmd/vmd.c — VMD 하위 토폴로지를 traverse 할 때 root port 디바이스를
 *   식별하는 키로 사용. 일반 NVMe id_table 에는 들어가지 않으며 VMD 전용.
 * 값 범위: 16-bit 0x2030 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define PCI_ROOT_PORT_B_INTEL_SKX	0x2031
/* [한국어] Intel SKX VMD root port B.
 * 역할/설정자/읽는 자/값/동기화: PCI_ROOT_PORT_A_INTEL_SKX 와 동일 (DID 0x2031). */
#define PCI_ROOT_PORT_C_INTEL_SKX	0x2032
/* [한국어] Intel SKX VMD root port C.
 * 역할/설정자/읽는 자/값/동기화: PCI_ROOT_PORT_A_INTEL_SKX 와 동일 (DID 0x2032). */
#define PCI_ROOT_PORT_D_INTEL_SKX	0x2033
/* [한국어] Intel SKX VMD root port D (마지막).
 * 역할: VMD 한 개당 보통 4 개 root port (A~D) 를 노출하여 그 아래로 PCIe lane 을 분배.
 *   각 root port 아래에 직결 NVMe SSD 가 배치되어 VMD 가 핫플러그/LED 를 통합 관리.
 * 설정자/읽는 자/값/동기화: PCI_ROOT_PORT_A_INTEL_SKX 와 동일 (DID 0x2033). */
#define PCI_ROOT_PORT_A_INTEL_ICX	0x347a
/* [한국어] Intel ICX VMD root port A.
 * 역할: ICX 세대의 VMD 가상 root port. ICX 는 SKX 의 0x2030~0x2033 대역을 0x347a~0x347d
 *   로 옮긴 형태. lib/vmd/vmd.c 가 ICX VMD 하위에서 root port 식별 시 매치.
 * 설정자/읽는 자/값/동기화: PCI_ROOT_PORT_A_INTEL_SKX 와 동일 (DID 0x347a). */
#define PCI_ROOT_PORT_B_INTEL_ICX	0x347b
/* [한국어] Intel ICX VMD root port B.
 * 역할/설정자/읽는 자/값/동기화: PCI_ROOT_PORT_A_INTEL_ICX 와 동일 (DID 0x347b). */
#define PCI_ROOT_PORT_C_INTEL_ICX	0x347c
/* [한국어] Intel ICX VMD root port C.
 * 역할/설정자/읽는 자/값/동기화: PCI_ROOT_PORT_A_INTEL_ICX 와 동일 (DID 0x347c). */
#define PCI_ROOT_PORT_D_INTEL_ICX	0x347d
/* [한국어] Intel ICX VMD root port D (마지막).
 * 역할/설정자/읽는 자/값/동기화: PCI_ROOT_PORT_A_INTEL_ICX 와 동일 (DID 0x347d). */

#define PCI_DEVICE_ID_AMD_AE4DMA_3E 0x14dc
/* [한국어] AMD AE4DMA (AMD EPYC PSP 기반 DMA 엔진) DID — 변종 1 (Genoa/Bergamo 시기).
 * 역할: PCIe config offset 0x00-0x01=0x1022 (SPDK_PCI_VID_AMD) + offset 0x02-0x03=0x14dc
 *   인 디바이스를 AMD AE4DMA 가속기로 식별. AE4DMA 는 AMD EPYC 의 Platform Security
 *   Processor(PSP) 에서 파생된 DMA 엔진으로, Intel IOAT/DSA 와 유사하게 메모리 복사 등을
 *   가속. PCIe Function 단위로 채널을 분리.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/ae4dma 와 module/accel/ae4dma — 본 DID + AMD VID 매치 시 attach 하여
 *   spdk_accel API 의 백엔드로 등록 (lib/idxd 와 유사 패턴).
 * 값 범위: 16-bit 0x14dc 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */
#define PCI_DEVICE_ID_AMD_AE4DMA_4E 0x149b
/* [한국어] AMD AE4DMA DID — 변종 2 (이후 EPYC 세대).
 * 역할: AMD VID 0x1022 + 본 DID 인 디바이스를 후속 세대 AE4DMA 로 식별. 변종 1(0x14dc)
 *   과 동일 드라이버(lib/ae4dma) 에서 함께 매치되어 PCIe Function 단위로 채널 enumerate.
 * 설정자: 본 헤더의 #define.
 * 읽는 자: lib/ae4dma, module/accel/ae4dma id_table.
 * 값 범위: 16-bit 0x149b 고정.
 * 동기화: 컴파일 타임 상수, 동기화 무관. */

#ifdef __cplusplus
}
#endif
/* [한국어] C++ 링키지 가드 종료.
 * 역할: 위의 extern "C" { 와 짝이 되는 닫는 중괄호로, C++ 컴파일러에서만 전개되어
 *   본 헤더 영역의 심볼이 C linkage 로 처리되도록 마감.
 * 설정자: __cplusplus 매크로 정의 시 활성화.
 * 읽는 자: C++ 컴파일러의 link 단계.
 * 값 범위: 정의 여부만 영향.
 * 동기화: 전처리기 단계만 영향. */

#endif /* SPDK_PCI_IDS */
/* [한국어] include guard 종료 (#ifndef SPDK_PCI_IDS 와 짝).
 * 역할: 본 헤더가 정의한 매크로 군(SPDK_PCI_VID_*, SPDK_PCI_CLASS_*, PCI_DEVICE_ID_*) 이
 *   한 번역 단위에서 한 번만 전개되도록 보장하여 중복 정의 경고/에러를 차단.
 * 설정자/읽는 자: 본 헤더 시작부의 #ifndef SPDK_PCI_IDS / #define SPDK_PCI_IDS 와 짝.
 * 값 범위: 정의 여부만 영향. SPDK 의 거의 모든 PCI 관련 모듈(lib/env_dpdk, lib/nvme,
 *   lib/ioat, lib/vmd, lib/ae4dma, module/bdev/virtio 등) 이 본 헤더를 직간접 인클루드.
 * 동기화: 전처리기 단계만 영향, 멀티스레드 무관. */
