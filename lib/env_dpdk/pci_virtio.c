/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Virtio-SCSI / Virtio-Blk PCI 드라이버 등록 모듈 (pci_virtio.c)
 *
 * === 파일의 역할 ===
 * QEMU/KVM 게스트에서 보이는 virtio PCI 디바이스(virtio-scsi, virtio-blk)를 SPDK가 인식하도록
 * 매칭 테이블을 등록하는 글루 파일이다. modern(1.0+)과 legacy(0.95) 양쪽 디바이스 ID를 모두
 * 등록한다. 실제 virtio 큐 관리·디스크립터 처리는 module/bdev/virtio 측에서 수행한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 게스트에서 SPDK를 돌리는 경우 spdk_env_init() 단계에서 본 드라이버가 자동 등록되고, 이후
 * SPDK virtio bdev 모듈이 spdk_pci_virtio_get_driver()로 enumerate를 시작한다. 호스트에서
 * vhost-user target을 돌릴 때는 본 파일이 무관하다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: env_internal.h, spdk/pci_ids.h.
 * - 본 파일에 의존: module/bdev/virtio, module/bdev/virtio_blk.
 * - 공유 자료구조: spdk_pci_id, spdk_pci_driver.
 *
 * === 주요 함수/구조체 요약 ===
 * - virtio_pci_driver_id[]: virtio-scsi/blk × modern/legacy 4가지 ID 테이블.
 * - spdk_pci_virtio_get_driver(): virtio 드라이버 핸들 반환.
 * - SPDK_PCI_DRIVER_REGISTER(virtio, ..., NEED_MAPPING | WC_ACTIVATE): 자동 등록 매크로.
 *   WC_ACTIVATE: Write-Combining 매핑 활성화(virtio notify register write 시 throughput 향상).
 */

/* [한국어] env_dpdk 내부 헤더. 드라이버 등록 매크로/조회 함수를 제공. */
#include "env_internal.h"

/* [한국어] virtio vendor ID 및 디바이스 ID 매크로. */
#include "spdk/pci_ids.h"

/* [한국어] virtio PCI 디바이스 매칭 테이블.
 * 설정자: 컴파일 타임 정적 초기화. 읽는 자: SPDK_PCI_DRIVER_REGISTER.
 * 값 범위: SCSI/BLK × MODERN/LEGACY 4가지. virtio 1.0 이상은 MODERN, 그 이전은 LEGACY. */
static struct spdk_pci_id virtio_pci_driver_id[] = {
	{ SPDK_PCI_DEVICE(SPDK_PCI_VID_VIRTIO, PCI_DEVICE_ID_VIRTIO_SCSI_MODERN) }, /* [한국어] virtio-scsi 1.0+ */
	{ SPDK_PCI_DEVICE(SPDK_PCI_VID_VIRTIO, PCI_DEVICE_ID_VIRTIO_BLK_MODERN) },  /* [한국어] virtio-blk 1.0+ */
	{ SPDK_PCI_DEVICE(SPDK_PCI_VID_VIRTIO, PCI_DEVICE_ID_VIRTIO_SCSI_LEGACY) }, /* [한국어] virtio-scsi 0.95 호환 */
	{ SPDK_PCI_DEVICE(SPDK_PCI_VID_VIRTIO, PCI_DEVICE_ID_VIRTIO_BLK_LEGACY) },  /* [한국어] virtio-blk 0.95 호환 */
	{ .vendor_id = 0, /* sentinel */ }, /* [한국어] 종료 표시. */
};

/*
 * [한국어]
 * spdk_pci_virtio_get_driver - "virtio" PCI 드라이버 핸들 반환
 *
 * @return: 등록된 spdk_pci_driver*.
 *
 * module/bdev/virtio가 게스트 안의 virtio 디바이스를 attach할 때 사용하는 진입점.
 * 실행 컨텍스트: 메인 스레드.
 *
 * 호출 체인: virtio bdev module → spdk_pci_virtio_get_driver → spdk_pci_get_driver
 */
struct spdk_pci_driver *
spdk_pci_virtio_get_driver(void)
{
	/* [한국어] 등록 시 사용한 이름 "virtio"로 글로벌 테이블에서 조회. */
	return spdk_pci_get_driver("virtio");
}

/* [한국어] virtio 드라이버 자동 등록.
 * SPDK_PCI_DRIVER_NEED_MAPPING: BAR mmap 필요.
 * SPDK_PCI_DRIVER_WC_ACTIVATE: BAR을 Write-Combining 캐시 정책으로 매핑하여 doorbell write
 * 같은 store 연산을 묶어 PCIe write throughput을 높인다. */
SPDK_PCI_DRIVER_REGISTER(virtio, virtio_pci_driver_id,
			 SPDK_PCI_DRIVER_NEED_MAPPING | SPDK_PCI_DRIVER_WC_ACTIVATE);
