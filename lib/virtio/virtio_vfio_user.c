/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation. All rights reserved.
 */

/*
 * [한국어 설명] SPDK virtio vfio-user 트랜스포트 (virtio_vfio_user.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK virtio 이니시에이터의 **vfio-user 백엔드**를 구현한다.
 * vfio-user는 vfio의 유저스페이스 변형 — 커널 vfio-pci 드라이버 대신 UNIX 소켓 위에서
 * 같은 프로토콜(VFIO IOCTL)을 메시지로 주고받는다. 주로 SPDK target과 SPDK initiator를
 * 한 호스트 안에서 연결하거나, cloud-hypervisor 같은 VMM과 연결할 때 사용된다.
 * 본 파일은 다음 책임을 진다:
 *  1) UNIX 소켓 경로로 vfio-user 디바이스 setup (spdk_vfio_user_setup),
 *  2) PCI common cfg / device-specific cfg를 spdk_vfio_user_pci_bar_access(BAR R/W
 *     메시지)로 조작,
 *  3) 큐 메모리(vring)를 hugepage DMA 메모리로 할당하고 desc/avail/used 주소를 BAR
 *     access로 게시 (vfio-user는 IOVA = VA 모드로 동작),
 *  4) 큐 활성화/비활성화 — queue_enable BAR 슬롯에 R/W,
 *  5) PCI CMD 레지스터에 BUSMASTER + INTx-disable 비트를 set.
 * notify는 polled mode에서 불필요(no-op)하며, MMIO 대신 모든 접근이 소켓 메시지라
 * latency가 크다는 점에 주의 (init 경로에서만 사용).
 *
 * === 전체 아키텍처에서의 위치 ===
 * bdev_virtio_blk/scsi 모듈
 *   ↓ virtio_vfio_user_dev_init(path)
 * [이 파일: virtio_vfio_user.c — UNIX socket 위 vfio-user 프로토콜로 BAR 접근]
 *   ↓ spdk_vfio_user_pci_bar_access (lib/vfio_user/)
 * UNIX socket → vfio-user target (예: 다른 SPDK 프로세스)
 *   ↑ (virtio.c가 이 파일의 ops 콜백 호출)
 * 실행 컨텍스트: SPDK reactor 위 호스트 유저스페이스. polled-mode (notify는 의미 없음).
 *
 * === 타 모듈과의 연결 ===
 * - 상위 의존: virtio.c (vring 관리), bdev_virtio (init 경로)
 * - 하위 의존: spdk/vfio_user_pci.h (spdk_vfio_user_setup, spdk_vfio_user_pci_bar_access)
 * - 공유 자료구조:
 *     struct virtio_vfio_user_dev: 본 파일 사설 — vfio_device 핸들 + PCI cap 오프셋들
 *     vfio_device (불투명)        : lib/vfio_user/가 관리하는 소켓 + 메시지 컨텍스트
 * - 데이터 흐름: virtio.c 콜백 → bar_access(공통 cfg 영역 offset, R/W) → vfio-user 메시지
 *               → target이 emulated MMIO 처리 → 응답 메시지 → 함수 반환
 *
 * === 주요 함수/구조체 요약 ===
 *  - virtio_vfio_user_dev_init      : 진입점 — 소켓 setup + PCI CMD 활성화 + BAR4 레이아웃 하드코딩
 *  - virtio_vfio_user_setup_queue   : DMA 메모리 할당 + desc/avail/used 주소 BAR 게시 + queue_enable
 *  - virtio_vfio_user_{get,set}_features: VIRTIO_PCI_COMMON_GFSELECT + GF로 64-bit 피처 R/W
 *  - virtio_vfio_user_notify_queue  : no-op (polling)
 *  - struct virtio_vfio_user_dev    : 디바이스별 vfio handle + 사설 cap 오프셋 캐시
 */

#include "spdk/stdinc.h"        /* [한국어] 표준 라이브러리 추상화. */
#include "spdk/memory.h"        /* [한국어] VIRTIO_PCI_VRING_ALIGN, SPDK_MALLOC_DMA 등. */
#include "spdk/vfio_user_pci.h" /* [한국어] spdk_vfio_user_setup/release/pci_bar_access — UNIX 소켓 위 vfio-user 클라이언트. */

#include "spdk_internal/virtio.h" /* [한국어] virtio 공통 정의 (VIRTIO_PCI_COMMON_* 오프셋 매크로 포함). */

#include <linux/vfio.h>         /* [한국어] VFIO_PCI_CONFIG_REGION_INDEX, VFIO_PCI_BAR4_REGION_INDEX 등 region 인덱스. */

/*
 * [한국어]
 * struct virtio_vfio_user_dev - vfio-user 트랜스포트 사설 컨텍스트 (virtio_dev->ctx)
 *
 * vfio_device 핸들과 BAR4 내 virtio cap 4종의 오프셋을 보관. 현재는 cap 발견 대신
 * 하드코딩된 레이아웃을 사용 (TODO 주석 참조: 향후 vendor cap 순회로 대체 예정).
 */
struct virtio_vfio_user_dev {
	struct vfio_device	*ctx;
	/* [한국어] lib/vfio_user/가 관리하는 vfio-user 디바이스 핸들 (소켓 fd + 메시지 큐 포함).
	 * 설정자: virtio_vfio_user_dev_init에서 spdk_vfio_user_setup 결과.
	 * 읽는 자: 모든 BAR access 함수가 첫 인자로 전달.
	 * 동기화: lib/vfio_user 내부에서 처리 — 본 파일은 단순 패스스루. */

	char			path[PATH_MAX];
	/* [한국어] vfio-user UNIX 소켓 경로 — 디버그/로깅 용 보존.
	 * 설정자: dev_init에서 snprintf로 복사.
	 * 읽는 자: 현재 직접 사용처 없음 — 진단용 캡쳐. */

	uint32_t		pci_cap_region;
	/* [한국어] BAR region 인덱스 (보통 VFIO_PCI_BAR4_REGION_INDEX) — 모든 cap이 같은 BAR에.
	 * 설정자: dev_init에서 하드코딩 (BAR4).
	 * 읽는 자: 모든 bar_access 호출의 region 인자. */

	uint32_t		pci_cap_common_cfg_offset;
	/* [한국어] 이 BAR 내 common configuration 영역 시작 오프셋 (스펙 §4.1.4.3).
	 * 설정자: dev_init에서 0x0 하드코딩.
	 * 읽는 자: get/set_status, get/set_features, setup_queue 등이 + VIRTIO_PCI_COMMON_* 오프셋 가산. */

	uint32_t		pci_cap_common_cfg_length;
	/* [한국어] common cfg 영역 길이 (검증 용). 설정자: dev_init 하드코딩 0x1000. */

	uint32_t		pci_cap_device_specific_offset;
	/* [한국어] device-specific config 시작 오프셋 (예: virtio-blk capacity 위치).
	 * 설정자: dev_init 하드코딩 0x2000.
	 * 읽는 자: read/write_dev_config가 + 사용자 offset 가산. */

	uint32_t		pci_cap_device_specific_length;
	/* [한국어] device-specific 영역 길이. 설정자: dev_init 하드코딩 0x1000. */

	uint32_t		pci_cap_notifications_offset;
	/* [한국어] notify 영역 오프셋. polling mode에서 사용 안 됨 (notify_queue가 no-op).
	 * 설정자: dev_init 하드코딩 0x3000. */

	uint32_t		pci_cap_notifications_length;
	/* [한국어] notify 영역 길이. 설정자: dev_init 하드코딩 0x1000. */
};

/*
 * [한국어]
 * virtio_vfio_user_read_dev_config - device-specific config read (vfio-user BAR access)
 *
 * @vdev  : 대상
 * @offset: dev_cfg 시작 오프셋
 * @dst   : [out] 버퍼
 * @length: 바이트 수
 * @return: spdk_vfio_user_pci_bar_access 결과 (0 성공)
 *
 * vfio-user는 BAR access 메시지로 read를 구현 — UNIX 소켓 경유, latency 큼.
 *
 * 실행 컨텍스트: 호출 thread (보통 init 또는 RPC handler).
 *
 * 호출 체인:
 *   virtio_dev_read_dev_config → backend_ops->read_dev_cfg(=[virtio_vfio_user_read_dev_config])
 *   → spdk_vfio_user_pci_bar_access (소켓 메시지)
 */
static int
virtio_vfio_user_read_dev_config(struct virtio_dev *vdev, size_t offset,
				 void *dst, int length)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;  /* [한국어] vfio-user 컨텍스트. */

	SPDK_DEBUGLOG(virtio_vfio_user, "offset 0x%lx, length 0x%x\n", offset, length); /* [한국어] 디버그 로그. */
	return spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					     dev->pci_cap_device_specific_offset + offset,
					     length, dst, false);
	/* [한국어] BAR access — region/오프셋/길이/버퍼/is_write=false(read).
	 * 마지막 인자 false = read 방향 (소켓 메시지 후 dst에 결과 채움). */
}

/*
 * [한국어]
 * virtio_vfio_user_write_dev_config - device-specific config write
 *
 * @vdev  : 대상
 * @offset: 오프셋
 * @src   : 데이터
 * @length: 바이트 수
 * @return: bar_access 결과
 *
 * 호출 체인:
 *   virtio_dev_write_dev_config → backend_ops->write_dev_cfg(=[virtio_vfio_user_write_dev_config])
 */
static int
virtio_vfio_user_write_dev_config(struct virtio_dev *vdev, size_t offset,
				  const void *src, int length)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;

	SPDK_DEBUGLOG(virtio_vfio_user, "offset 0x%lx, length 0x%x\n", offset, length);
	return spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					     dev->pci_cap_device_specific_offset + offset,
					     length, (void *)src, true);
	/* [한국어] is_write=true — src 데이터를 디바이스에 write. const 캐스팅은 API 시그니처 호환. */
}

/*
 * [한국어]
 * virtio_vfio_user_get_status - 디바이스 status 비트 read (common cfg)
 *
 * @vdev: 대상
 * @return: status 비트 (실패 시 0)
 *
 * VIRTIO_PCI_COMMON_STATUS는 common cfg 내 status 레지스터의 오프셋 (스펙 §4.1.4.3).
 * 1바이트 read.
 *
 * 호출 체인:
 *   virtio_dev_get_status → backend_ops->get_status(=[virtio_vfio_user_get_status])
 */
static uint8_t
virtio_vfio_user_get_status(struct virtio_dev *vdev)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t offset;     /* [한국어] BAR 내 절대 오프셋. */
	uint8_t status = 0;  /* [한국어] read 결과 (실패 시 0 반환). */
	int rc;

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_STATUS; /* [한국어] common_cfg 시작 + status 오프셋. */
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 1, &status, false); /* [한국어] 1바이트 read. */
	if (rc) {
		SPDK_ERRLOG("Failed to get device status\n"); /* [한국어] 소켓 통신 실패. */
	}

	SPDK_DEBUGLOG(virtio_vfio_user, "device status %x\n", status);

	return status;
}

/*
 * [한국어]
 * virtio_vfio_user_set_status - 디바이스 status 비트 write
 *
 * @vdev  : 대상
 * @status: 새 status 값 (보통 누적 비트)
 *
 * 호출 체인:
 *   virtio_dev_set_status → backend_ops->set_status(=[virtio_vfio_user_set_status])
 */
static void
virtio_vfio_user_set_status(struct virtio_dev *vdev, uint8_t status)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t offset;
	int rc;

	SPDK_DEBUGLOG(virtio_vfio_user, "device status %x\n", status);

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_STATUS;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 1, &status, true); /* [한국어] 1바이트 write. */
	if (rc) {
		SPDK_ERRLOG("Failed to set device status\n");
	}
}

/*
 * [한국어]
 * virtio_vfio_user_get_features - 디바이스 지원 피처 64-bit read (DFSELECT + DF)
 *
 * @vdev: 대상
 * @return: 64-bit 피처 마스크
 *
 * virtio common_cfg는 32-bit 윈도. select=0이면 lo, select=1이면 hi.
 * 동작:
 *   1) DFSELECT = 0 → DF read → features_lo
 *   2) DFSELECT = 1 → DF read → features_hi
 *   3) ((hi << 32) | lo)
 * 각 단계가 독립 BAR access (소켓 메시지) — 4번의 R/W 메시지가 발생.
 *
 * 호출 체인:
 *   virtio_negotiate_features → backend_ops->get_features(=[virtio_vfio_user_get_features])
 */
static uint64_t
virtio_vfio_user_get_features(struct virtio_dev *vdev)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t offset;
	uint32_t features_lo, features_hi, feature_select;  /* [한국어] 윈도 선택 + lo/hi 캡쳐. */
	int rc;

	feature_select = 0; /* [한국어] 하위 32 비트 윈도. */
	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_DFSELECT; /* [한국어] device feature select 레지스터. */
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &feature_select, true); /* [한국어] 4바이트 write — select=0 set. */
	if (rc) {
		SPDK_ERRLOG("Failed to set device feature select\n");
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_DF; /* [한국어] device feature 레지스터. */
	features_lo = 0;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &features_lo, false); /* [한국어] lo 32 비트 read. */
	if (rc) {
		SPDK_ERRLOG("Failed to get device feature low\n");
	}

	feature_select = 1; /* [한국어] 상위 32 비트 윈도. */
	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_DFSELECT;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &feature_select, true); /* [한국어] select=1 set. */
	if (rc) {
		SPDK_ERRLOG("Failed to set device feature select\n");
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_DF;
	features_hi = 0;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &features_hi, false); /* [한국어] hi 32 비트 read. */
	if (rc) {
		SPDK_ERRLOG("Failed to get device feature high\n");
	}

	SPDK_DEBUGLOG(virtio_vfio_user, "feature_hi 0x%x, feature_low 0x%x\n", features_hi, features_lo);

	return (((uint64_t)features_hi << 32) | ((uint64_t)features_lo)); /* [한국어] 64-bit 결합. */
}

/*
 * [한국어]
 * virtio_vfio_user_set_features - 협상 피처 64-bit write (GFSELECT + GF)
 *
 * @vdev    : 대상
 * @features: 협상 결과 마스크
 * @return: 0 성공 / 음수 errno (마지막 단계만 반환 — 중간 실패 시 조기 return)
 *
 * 동작 (DFSELECT/DF의 guest 버전):
 *   1) GFSELECT = 0 → GF write (lo)
 *   2) GFSELECT = 1 → GF write (hi)
 *
 * 호출 체인:
 *   virtio_negotiate_features → backend_ops->set_features(=[virtio_vfio_user_set_features])
 */
static int
virtio_vfio_user_set_features(struct virtio_dev *vdev, uint64_t features)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t offset;
	uint32_t features_lo, features_hi, feature_select;
	int rc;

	feature_select = 0;
	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_GFSELECT; /* [한국어] guest feature select. */
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &feature_select, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set Guest feature select\n");
		return rc;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_GF;
	features_lo = (uint32_t)features; /* [한국어] 64→32 자동 절단 = 하위 32. */
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &features_lo, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set Guest feature low\n");
		return rc;
	}

	feature_select = 1;
	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_GFSELECT;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &feature_select, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set Guest feature select\n");
		return rc;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_GF;
	features_hi = (uint32_t)(features >> 32); /* [한국어] 상위 32. */
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &features_hi, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set Guest feature high\n");
	}

	vdev->negotiated_features = features; /* [한국어] virtio.c가 EVENT_IDX 협상 결과 검사 시 참조. */
	SPDK_DEBUGLOG(virtio_vfio_user, "features 0x%"PRIx64"\n", features);

	return rc;
}

/*
 * [한국어]
 * virtio_vfio_user_destruct_dev - vfio-user 디바이스 release + 사설 ctx free
 *
 * @vdev: 대상
 *
 * 동작:
 *   1) spdk_vfio_user_release: 소켓 close + lib/vfio_user 컨텍스트 해제
 *   2) free(dev): 사설 컨텍스트 해제
 *
 * 호출 체인:
 *   virtio_dev_destruct → backend_ops->destruct_dev(=[virtio_vfio_user_destruct_dev])
 */
static void
virtio_vfio_user_destruct_dev(struct virtio_dev *vdev)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;

	if (dev) {
		spdk_vfio_user_release(dev->ctx); /* [한국어] vfio-user 핸들 해제 (소켓 close). */
		free(dev);                         /* [한국어] 사설 ctx free. */
	}
}

/*
 * [한국어]
 * virtio_vfio_user_get_queue_size - 큐 크기 query (Q_SELECT → Q_SIZE)
 *
 * @vdev    : 대상
 * @queue_id: 큐 인덱스
 * @return: 디바이스 보고 큐 크기 (실패 시 0)
 *
 * 동작:
 *   1) VIRTIO_PCI_COMMON_Q_SELECT = queue_id (큐 컨텍스트 전환)
 *   2) VIRTIO_PCI_COMMON_Q_SIZE read
 *
 * 호출 체인:
 *   virtio_init_queue → backend_ops->get_queue_size(=[virtio_vfio_user_get_queue_size])
 */
static uint16_t
virtio_vfio_user_get_queue_size(struct virtio_dev *vdev, uint16_t queue_id)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t offset;
	uint16_t qsize = 0;
	int rc;

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_SELECT; /* [한국어] queue select 레지스터. */
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &queue_id, true); /* [한국어] 2바이트 write — 대상 큐. */
	if (rc) {
		SPDK_ERRLOG("Failed to set queue select\n");
		return 0;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_SIZE;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &qsize, false); /* [한국어] 2바이트 read — 크기. */
	if (rc) {
		SPDK_ERRLOG("Failed to get queue size\n");
		return 0;
	}

	SPDK_DEBUGLOG(virtio_vfio_user, "queue %u, size %u\n", queue_id, qsize);

	return qsize;
}

/*
 * [한국어]
 * virtio_vfio_user_setup_queue - vring 메모리 할당 + IOVA 게시 + queue_enable
 *
 * @vdev: 대상
 * @vq  : 셋업할 virtqueue
 * @return: 0 성공, 음수 errno
 *
 * 차이점 vs PCI:
 *   - vfio-user는 IOVA = VA 모드(즉, 디바이스도 동일 VA 사용 — vtophys 불필요)
 *   - 모든 BAR write가 소켓 메시지 — 7번의 메시지 (select+desc lo/hi+avail lo/hi+used lo/hi)
 *     + notify_off read + queue_enable write
 *
 * 동작:
 *   1) hugepage DMA 메모리 할당 (VIRTIO_PCI_VRING_ALIGN 정렬)
 *   2) ring 영역 분할 계산 (desc/avail/used)
 *   3) Q_SELECT로 큐 선택
 *   4) Q_DESCLO/HI/AVAILLO/HI/USEDLO/HI에 32-bit 분할 write로 64-bit IOVA 게시
 *   5) Q_NOFF read (현재는 사용 안 함 — polling 모드)
 *   6) Q_ENABLE = 1로 큐 활성화
 *
 * 실행 컨텍스트: init 단일 thread.
 *
 * 호출 체인:
 *   virtio_init_queue → backend_ops->setup_queue(=[virtio_vfio_user_setup_queue])
 */
static int
virtio_vfio_user_setup_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t desc_addr, avail_addr, used_addr, offset; /* [한국어] vring 영역 주소들 + 진행 오프셋. */
	uint32_t addr_lo, addr_hi;                          /* [한국어] 32-bit 분할 write 임시 변수. */
	uint16_t notify_off, queue_enable;                  /* [한국어] notify_off(미사용), queue_enable=1 set용. */
	void *queue_mem;
	int rc;

	queue_mem = spdk_zmalloc(vq->vq_ring_size, VIRTIO_PCI_VRING_ALIGN, NULL,
				 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	/* [한국어] DMA-pinned hugepage에서 큐 메모리 할당. VIRTIO_PCI_VRING_ALIGN 정렬. */
	if (queue_mem == NULL) {
		return -ENOMEM;
	}

	/* vfio-user address translation uses `IOVA=VA` mode */
	/* [한국어] vfio-user는 DMA 매핑 시 host VA = IOVA로 등록 — 디바이스가 같은 VA를 그대로 사용.
	 * 따라서 vtophys 불필요, VA를 IOVA로 그대로 사용. */
	vq->vq_ring_mem = (uint64_t)(uintptr_t)queue_mem; /* [한국어] IOVA = VA. */
	vq->vq_ring_virt_mem = queue_mem;                 /* [한국어] SPDK가 사용할 VA (동일). */

	desc_addr = vq->vq_ring_mem;
	avail_addr = desc_addr + vq->vq_nentries * sizeof(struct vring_desc);
	used_addr = (avail_addr + offsetof(struct vring_avail, ring[vq->vq_nentries])
		     + VIRTIO_PCI_VRING_ALIGN - 1) & ~(VIRTIO_PCI_VRING_ALIGN - 1);
	/* [한국어] desc → avail → (정렬) → used 레이아웃 — virtio 스펙 §2.6. */

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_SELECT;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &vq->vq_queue_index, true); /* [한국어] 2바이트 write — 셋업 대상 큐 선택. */
	if (rc) {
		SPDK_ERRLOG("Failed to set queue select\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_DESCLO;
	addr_lo = (uint32_t)desc_addr;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &addr_lo, true); /* [한국어] desc lo 32-bit. */
	if (rc) {
		SPDK_ERRLOG("Failed to set desc addr low\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_DESCHI;
	addr_hi = (uint32_t)(desc_addr >> 32);
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &addr_hi, true); /* [한국어] desc hi 32-bit. */
	if (rc) {
		SPDK_ERRLOG("Failed to set desc addr high\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_AVAILLO;
	addr_lo = (uint32_t)avail_addr;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &addr_lo, true); /* [한국어] avail lo. */
	if (rc) {
		SPDK_ERRLOG("Failed to set avail addr low\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_AVAILHI;
	addr_hi = (uint32_t)(avail_addr >> 32);
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &addr_hi, true); /* [한국어] avail hi. */
	if (rc) {
		SPDK_ERRLOG("Failed to set avail addr high\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_USEDLO;
	addr_lo = (uint32_t)used_addr;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &addr_lo, true); /* [한국어] used lo. */
	if (rc) {
		SPDK_ERRLOG("Failed to set used addr low\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_USEDHI;
	addr_hi = (uint32_t)(used_addr >> 32);
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &addr_hi, true); /* [한국어] used hi. */
	if (rc) {
		SPDK_ERRLOG("Failed to set used addr high\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_NOFF;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &notify_off, false); /* [한국어] notify_off read — 본 트랜스포트는 polling이라 미사용이지만 프로토콜 완결성. */
	if (rc) {
		SPDK_ERRLOG("Failed to get queue notify off\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_ENABLE;
	queue_enable = 1;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &queue_enable, true); /* [한국어] 큐 활성화 — 디바이스가 폴링 시작. */
	if (rc) {
		SPDK_ERRLOG("Failed to enable queue %u\n", vq->vq_queue_index);
		goto err;
	}

	SPDK_DEBUGLOG(virtio_vfio_user, "queue %"PRIu16" addresses:\n", vq->vq_queue_index);
	SPDK_DEBUGLOG(virtio_vfio_user, "\t desc_addr: %" PRIx64 "\n", desc_addr);
	SPDK_DEBUGLOG(virtio_vfio_user, "\t aval_addr: %" PRIx64 "\n", avail_addr);
	SPDK_DEBUGLOG(virtio_vfio_user, "\t used_addr: %" PRIx64 "\n", used_addr);

	return 0;
err:
	spdk_free(queue_mem); /* [한국어] 부분 성공 롤백 — 메모리 해제. */
	return rc;
}

/*
 * [한국어]
 * virtio_vfio_user_del_queue - 큐 비활성화 + 메모리 해제
 *
 * @vdev: 대상
 * @vq  : 해제 큐
 *
 * 동작:
 *   1) Q_SELECT로 큐 선택
 *   2) Q_ENABLE = 0으로 비활성
 *   3) hugepage 해제
 * (TODO: desc/avail/used 주소 클리어 — 현재 미구현, 디바이스가 disable 시 자동 처리 가정)
 *
 * 호출 체인:
 *   virtio_free_queues → backend_ops->del_queue(=[virtio_vfio_user_del_queue])
 */
static void
virtio_vfio_user_del_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t offset;
	uint16_t queue_enable = 0;  /* [한국어] 비활성 값. */
	int rc;

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_SELECT;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &vq->vq_queue_index, true); /* [한국어] 큐 선택. */
	if (rc) {
		SPDK_ERRLOG("Failed to select queue %u\n", vq->vq_queue_index);
		spdk_free(vq->vq_ring_virt_mem); /* [한국어] 그래도 메모리는 해제. */
		return;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_ENABLE;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &queue_enable, true); /* [한국어] enable=0. */
	if (rc) {
		SPDK_ERRLOG("Failed to enable queue %u\n", vq->vq_queue_index); /* [한국어] 메시지 텍스트는 잘못됐지만 의미는 disable 실패. */
	}

	spdk_free(vq->vq_ring_virt_mem); /* [한국어] hugepage 해제. */
	/* TODO: clear desc/avail/used address */
	/* [한국어] 향후 보강: queue_desc/avail/used를 0으로 명시적 클리어. */
}

/*
 * [한국어]
 * virtio_vfio_user_notify_queue - no-op (polling 모드 = doorbell 불필요)
 *
 * @vdev: 미사용
 * @vq  : 미사용
 *
 * vfio-user target은 polling으로 새 avail entry를 발견하므로 통지 메시지가 필요 없다.
 * 인터럽트 모드를 향후 지원하면 여기에 notify BAR write를 추가.
 *
 * 호출 체인:
 *   virtqueue_req_flush → backend_ops->notify_queue(=[virtio_vfio_user_notify_queue]) (no-op)
 */
static void
virtio_vfio_user_notify_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	/* we're running in polling mode, no need to write doorbells */
	/* [한국어] polled mode — 백엔드 target도 폴링 — doorbell 메시지 비용 회피. */
}

/*
 * [한국어]
 * virtio_vfio_user_ops - vfio-user 트랜스포트 콜백 테이블
 *
 * dump_json_info / write_json_config는 미구현 (RPC dump 시 NULL 호출 → segfault 가능 —
 * 사용 시 추가 보강 필요).
 */
static const struct virtio_dev_ops virtio_vfio_user_ops = {
	.read_dev_cfg	= virtio_vfio_user_read_dev_config,   /* [한국어] dev cfg read. */
	.write_dev_cfg	= virtio_vfio_user_write_dev_config,  /* [한국어] dev cfg write. */
	.get_status	= virtio_vfio_user_get_status,        /* [한국어] status read. */
	.set_status	= virtio_vfio_user_set_status,        /* [한국어] status write. */
	.get_features	= virtio_vfio_user_get_features,      /* [한국어] 피처 64-bit read. */
	.set_features	= virtio_vfio_user_set_features,      /* [한국어] 피처 64-bit write. */
	.destruct_dev	= virtio_vfio_user_destruct_dev,      /* [한국어] release + free. */
	.get_queue_size	= virtio_vfio_user_get_queue_size,    /* [한국어] 큐 크기 query. */
	.setup_queue	= virtio_vfio_user_setup_queue,       /* [한국어] vring 셋업 + enable. */
	.del_queue	= virtio_vfio_user_del_queue,         /* [한국어] disable + free. */
	.notify_queue	= virtio_vfio_user_notify_queue       /* [한국어] no-op (polling). */
};

/*
 * [한국어]
 * virtio_vfio_user_dev_init - vfio-user UNIX 소켓 setup + PCI CMD 활성화 + cap 레이아웃 설정
 *
 * @vdev: 호출자가 할당한 virtio_dev
 * @name: 디바이스 이름
 * @path: vfio-user UNIX 소켓 경로
 * @return: 0 성공, 음수 errno
 *
 * 동작:
 *   1) name/path 유효성 검사 (access(F_OK))
 *   2) virtio_vfio_user_dev calloc
 *   3) virtio_dev_construct로 vdev 기본 초기화 (ops 바인딩)
 *   4) spdk_vfio_user_setup으로 소켓 연결 + 디바이스 핸들 획득
 *   5) PCI CMD 레지스터(config space offset 4)에 BUSMASTER + INTx-disable 비트(0x404) set
 *      - BUSMASTER(0x4): DMA 가능
 *      - INTx-disable(0x400): 폴링 모드라 INTx 차단
 *   6) BAR4 cap 레이아웃 하드코딩 (TODO 주석: 향후 vendor cap 순회로 대체)
 *
 * 실행 컨텍스트: 디바이스 attach 경로 — 단일 thread.
 *
 * 호출 체인:
 *   bdev_virtio_vfio_user_create / RPC → [virtio_vfio_user_dev_init] →
 *      virtio_dev_construct + spdk_vfio_user_setup
 */
int
virtio_vfio_user_dev_init(struct virtio_dev *vdev, const char *name, const char *path)
{
	struct virtio_vfio_user_dev *dev;  /* [한국어] 새 사설 컨텍스트. */
	uint16_t cmd_reg;                   /* [한국어] PCI CMD 레지스터 값. */
	int rc;

	if (name == NULL) {
		SPDK_ERRLOG("No name given for controller: %s\n", path);
		return -EINVAL;
	}

	rc = access(path, F_OK); /* [한국어] 소켓 파일 존재 확인 — 미존재 시 즉시 실패. */
	if (rc != 0) {
		SPDK_ERRLOG("Access path %s failed\n", path);
		return -EACCES;
	}

	dev = calloc(1, sizeof(*dev)); /* [한국어] 0 초기화 — path 등 모든 필드 0. */
	if (dev == NULL) {
		return -ENOMEM;
	}

	rc = virtio_dev_construct(vdev, name, &virtio_vfio_user_ops, dev); /* [한국어] vdev에 ops + ctx 바인딩. */
	if (rc != 0) {
		SPDK_ERRLOG("Failed to init device: %s\n", path);
		free(dev);
		return rc;
	}

	snprintf(dev->path, PATH_MAX, "%s", path); /* [한국어] 경로 보존 (디버그용). */
	dev->ctx = spdk_vfio_user_setup(path);     /* [한국어] UNIX 소켓 연결 + vfio-user 핸들 획득. */
	if (!dev->ctx) {
		SPDK_ERRLOG("Error to setup %s as vfio device\n", path);
		virtio_dev_destruct(vdev); /* [한국어] construct 부분 롤백 — destruct가 ops->destruct_dev 호출 → free(dev). */
		return -EINVAL;
	}

	/* Enable PCI busmaster and disable INTx */
	/* [한국어] PCI CMD 레지스터를 read-modify-write 시퀀스로 갱신:
	 * - 비트 2 (BUSMASTER): 디바이스가 DMA initiator 역할 가능
	 * - 비트 10 (INTx-disable): 인터럽트 비활성 (polling 모드)
	 * 0x404 = (1<<10) | (1<<2). */
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2,
					   &cmd_reg, false);
	/* [한국어] PCI config space offset 4 (CMD 레지스터)에서 2바이트 read. */
	if (rc != 0) {
		SPDK_ERRLOG("Read PCI CMD REG failed\n");
		virtio_dev_destruct(vdev);
		return rc;
	}
	cmd_reg |= 0x404; /* [한국어] BUSMASTER + INTx-disable OR. */
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2,
					   &cmd_reg, true); /* [한국어] write back. */
	if (rc != 0) {
		SPDK_ERRLOG("Write PCI CMD REG failed\n");
		virtio_dev_destruct(vdev);
		return rc;
	}

	/* TODO: we cat get virtio device PCI common space layout via
	 * iterating vendor capabilities in PCI Configuration space,
	 * while here we use hardcoded layout first, this feature can
	 * be added in future.
	 *
	 * vfio-user emulated virtio device layout in Target:
	 *
	 * region 1: MSI-X Table
	 * region 2: MSI-X PBA
	 * region 4: virtio modern memory 64bits BAR
	 *     Common configuration          0x0    - 0x1000
	 *     ISR access                    0x1000 - 0x2000
	 *     Device specific configuration 0x2000 - 0x3000
	 *     Notifications                 0x3000 - 0x4000
	 */
	/* [한국어] TODO: PCI vendor capability list 순회로 동적 발견 (virtio_pci.c처럼) — 현재는 SPDK target과
	 * 합의된 고정 레이아웃 사용. BAR4(64-bit) 안에 4개 영역이 1KB 단위로 배치되어 있다. */
	dev->pci_cap_region = VFIO_PCI_BAR4_REGION_INDEX;       /* [한국어] BAR4 사용. */
	dev->pci_cap_common_cfg_offset = 0x0;                    /* [한국어] common cfg는 BAR4 처음. */
	dev->pci_cap_common_cfg_length = 0x1000;                 /* [한국어] 4KB. */
	dev->pci_cap_device_specific_offset = 0x2000;            /* [한국어] device cfg는 BAR4 + 8KB. */
	dev->pci_cap_device_specific_length = 0x1000;            /* [한국어] 4KB. */
	dev->pci_cap_notifications_offset = 0x3000;              /* [한국어] notify는 BAR4 + 12KB (미사용). */
	dev->pci_cap_notifications_length = 0x1000;              /* [한국어] 4KB. */

	return 0;
}

/* [한국어] SPDK 로그 컴포넌트 등록 — `--logflag virtio_vfio_user`로 활성화. */
SPDK_LOG_REGISTER_COMPONENT(virtio_vfio_user)
