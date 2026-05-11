/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2010-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK virtio 코어 (virtio.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK virtio 이니시에이터(initiator)의 **트랜스포트-비의존(transport-agnostic) 코어**를
 * 구현한다. 즉, virtio 디바이스가 PCI(QEMU 또는 다른 SPDK 프로세스), vfio-user, vhost-user 중
 * 무엇으로 노출되든 공통적으로 수행되는 다음 동작을 담는다:
 *  1) virtqueue 메모리(vring) 할당/초기화/해제,
 *  2) descriptor chain 할당/반환(free list 관리),
 *  3) avail/used 링을 통한 요청 제출(virtqueue_req_*) 및 완료 회수(virtio_recv_pkts),
 *  4) virtio device feature negotiation, status 시퀀스(ACK→DRIVER→FEATURES_OK→DRIVER_OK).
 * 트랜스포트별로 달라지는 부분(MMIO read/write, BAR 매핑, 통지 방식)은
 * `struct virtio_dev_ops` 콜백을 통해 위임한다. virtio.c 자체는 이 ops 테이블을 호출만 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK virtio-blk / virtio-scsi bdev 모듈
 *   ↓ (virtqueue_req_start → virtqueue_req_add_iovs → virtqueue_req_flush)
 * [이 파일: virtio core - 공통 vring 관리]
 *   ↓ (backend_ops 콜백)
 * virtio_pci.c / virtio_vfio_user.c / virtio_vhost_user.c (트랜스포트 백엔드)
 *   ↓ (MMIO BAR write / vfio-user socket / vhost-user UNIX socket)
 * QEMU/cloud-hypervisor/SPDK target (백엔드 디바이스)
 * 실행 컨텍스트: 호스트 유저스페이스, polled-mode SPDK reactor 위에서 실행.
 * 큐 단위로 `owner_thread`가 부여되어 한 큐는 단일 SPDK thread에서만 다뤄진다 (lockless 보장).
 *
 * === 타 모듈과의 연결 ===
 * - 상위 의존: bdev_virtio_blk.c, bdev_virtio_scsi.c (`module/bdev/virtio/`)가
 *   virtqueue_req_*, virtio_recv_pkts, virtio_dev_acquire_queue 등을 호출한다.
 * - 하위 의존: backend_ops를 통해 트랜스포트 구현(virtio_pci.c 등)에 위임.
 * - 공유 자료구조:
 *     struct virtio_dev    : 디바이스 상태(피처, 큐 배열, 백엔드 ops)
 *     struct virtqueue     : 단일 vring의 상태 + descriptor 자유리스트
 *     struct vring         : virtio 스펙의 desc/avail/used 3개 링 (공유 메모리)
 *     struct vring_desc    : 단일 디스크립터 (addr/len/flags/next)
 *     struct vq_desc_extra : SPDK 측이 보관하는 디스크립터 메타(콜백 cookie, ndescs)
 * - 데이터 흐름: bdev_io → iovs → vring_desc 체인 → avail->ring[] →
 *               (백엔드 처리) → used->ring[] → virtio_recv_pkts → bdev_io 완료.
 *
 * === 주요 함수/구조체 요약 ===
 *  - virtio_dev_construct() : virtio_dev 기본 필드 초기화 (이름, mutex, ops 바인딩)
 *  - virtio_dev_reset()     : 디바이스 리셋 + status 시퀀스(ACK→DRIVER) + feature negotiation
 *  - virtio_dev_start()     : 큐 메모리 할당(virtio_alloc_queues) + DRIVER_OK 설정
 *  - virtqueue_req_start()  : 새 요청을 위한 descriptor head 예약(자유리스트에서 1칸 확보)
 *  - virtqueue_req_add_iovs(): iovec 배열을 디스크립터 체인으로 변환 (DMA 분할 포함)
 *  - virtqueue_req_flush()  : avail->idx 게시 + (필요 시) 백엔드 통지(notify)
 *  - virtio_recv_pkts()     : used 링에서 완료된 요청들의 cookie를 회수
 *  - vq_ring_free_chain()   : 사용 후 디스크립터 체인을 자유리스트 tail에 반환
 *  - virtio_dev_acquire/release_queue: 한 큐를 한 SPDK thread에 락-안전하게 귀속
 *  - virtio_negotiate_features(): VIRTIO_RING_F_EVENT_IDX 등 피처 협상 후 FEATURES_OK 게시
 */

#include "spdk/stdinc.h"     /* [한국어] SPDK 표준 stdint/stdbool 등 표준 라이브러리 래퍼. 빌드 환경(POSIX/Windows) 차이를 흡수한다. */

#include "spdk/env.h"        /* [한국어] DPDK EAL 추상화 - spdk_vtophys(가상→물리 주소), spdk_zmalloc 등 hugepage DMA 메모리 API. */
#include "spdk/util.h"       /* [한국어] spdk_u32_is_pow2(2의 거듭제곱 검사), SPDK_ALIGN_CEIL 등 스칼라 유틸 매크로. */
#include "spdk/barrier.h"    /* [한국어] SMP 메모리 배리어(spdk_smp_mb/rmb/wmb). vring의 producer/consumer 가시성 보장에 필수. */

#include "spdk_internal/virtio.h" /* [한국어] virtio 내부 구조체/상수: struct virtio_dev, virtqueue, vring, VIRTIO_PCI_* 등. */

/* We use SMP memory barrier variants as all virtio_pci devices
 * are purely virtual. All MMIO is executed on a CPU core, so
 * there's no need to do full MMIO synchronization.
 */
/* [한국어] virtio 디바이스는 가상화된 백엔드(QEMU/SPDK target)와 통신하므로,
 * 실제 PCIe MMIO 디바이스용 풀 메모리 배리어(예: spdk_mb)가 아니라
 * SMP CPU-CPU 가시성만 보장하면 충분하다. 이 매크로들은 그 사실을 명시적으로 코드화한 것이다.
 * - virtio_mb : full barrier (모든 read/write 순서 보장)
 * - virtio_rmb: read barrier (used->idx 읽기 전 used 링 데이터 가시성 보장)
 * - virtio_wmb: write barrier (avail->idx 게시 전 avail 링 갱신 가시성 보장) */
#define virtio_mb()	spdk_smp_mb()    /* [한국어] avail/used 양방향 동시 가시성이 필요할 때 사용. */
#define virtio_rmb()	spdk_smp_rmb()   /* [한국어] virtio_recv_pkts에서 used->idx 읽기 후 used->ring 읽기 사이의 RAR 순서 보장. */
#define virtio_wmb()	spdk_smp_wmb()   /* [한국어] finish_req에서 avail->ring 쓰기 후 avail->idx 게시 사이의 WAW 순서 보장. */

/* Chain all the descriptors in the ring with an END */
/*
 * [한국어]
 * vring_desc_init - 디스크립터 배열을 단일 자유리스트(free list)로 연결
 *
 * @dp: vring->desc 배열 시작 포인터 (n개의 vring_desc 연속 메모리)
 * @n : 디스크립터 개수 (큐 크기)
 * @return: 없음
 *
 * 큐 초기화 시점에 모든 디스크립터를 i → i+1 → ... → n-1 → CHAIN_END 형태의 단일
 * 연결 리스트로 만들어 vq_desc_head_idx=0, vq_desc_tail_idx=n-1, vq_free_cnt=n
 * 상태로 두기 위한 보조 함수다. 이후 요청이 들어올 때마다 head에서 디스크립터를
 * 하나씩 떼어 사용하고, 완료 후 vq_ring_free_chain()이 tail에 다시 붙인다.
 *
 * 실행 컨텍스트: virtio_init_vring()에서 단일 스레드 초기화 경로로만 호출. 동시성 없음.
 *
 * 호출 체인:
 *   virtio_init_queue → virtio_init_vring → [vring_desc_init]
 */
static inline void
vring_desc_init(struct vring_desc *dp, uint16_t n)
{
	uint16_t i;  /* [한국어] 디스크립터 인덱스 순회 변수. uint16_t는 virtio 스펙상 큐 크기 상한(32768)을 충분히 표현. */

	for (i = 0; i < n - 1; i++) {     /* [한국어] 마지막 디스크립터를 제외한 모든 디스크립터에 대해 next를 다음 인덱스로 설정. */
		dp[i].next = (uint16_t)(i + 1); /* [한국어] i번 디스크립터의 next를 i+1로 — 자유리스트 노드 연결. */
	}
	dp[i].next = VQ_RING_DESC_CHAIN_END; /* [한국어] 마지막(i=n-1) 디스크립터는 체인 종료 마커(0xFFFF 또는 유사값)로 표시 — 자유리스트 끝. */
}

/*
 * [한국어]
 * virtio_init_vring - virtqueue의 vring 메모리(공유 영역) 초기화
 *
 * @vq: 초기화할 virtqueue. 호출 전 vq_ring_virt_mem(vring 메모리 가상주소),
 *      vq_ring_size, vq_nentries, vdev가 설정되어 있어야 한다.
 * @return: 없음
 *
 * vring은 백엔드(QEMU/SPDK target)와 공유되는 메모리이며, 다음 3개 영역으로 구성된다:
 *   - desc[N]   : 디스크립터 테이블 (이니시에이터→백엔드 방향: 데이터 위치 명시)
 *   - avail     : 이니시에이터가 백엔드에게 "처리해줘"라고 게시하는 큐 (idx + ring[])
 *   - used      : 백엔드가 이니시에이터에게 "처리 끝"이라고 게시하는 큐 (idx + ring[])
 * 본 함수는 메모리를 0으로 클리어한 뒤 vring_init()으로 desc/avail/used 포인터를 배치하고,
 * SPDK 측 인덱스(vq_used_cons_idx 등)와 자유리스트, 그리고 디바이스 인터럽트 억제 설정을 한다.
 * 백엔드가 인터럽트로 깨어나는 비용을 피하고 폴링만으로 동작시키기 위해
 * VRING_AVAIL_F_NO_INTERRUPT 또는 EVENT_IDX 트릭을 사용한다.
 *
 * 실행 컨텍스트: virtio_init_queue()의 단일 초기화 경로. 큐가 still empty이며 백엔드는 아직 보지 못함.
 *
 * 호출 체인:
 *   virtio_dev_start → virtio_alloc_queues → virtio_init_queue → [virtio_init_vring]
 */
static void
virtio_init_vring(struct virtqueue *vq)
{
	int size = vq->vq_nentries;            /* [한국어] 큐 크기(디스크립터 개수). 항상 2의 거듭제곱(가상화 스펙 요구). */
	struct vring *vr = &vq->vq_ring;       /* [한국어] vring 구조체 별칭 — desc/avail/used 포인터를 담는다. */
	uint8_t *ring_mem = vq->vq_ring_virt_mem; /* [한국어] vring 메모리의 가상 주소(이니시에이터 측). 백엔드는 vq_ring_mem(IOVA)으로 본다. */

	/*
	 * Reinitialise since virtio port might have been stopped and restarted
	 */
	memset(ring_mem, 0, vq->vq_ring_size); /* [한국어] 재시작 케이스 대비, vring 전체를 0으로 초기화 — 잔여 디스크립터/idx 제거. */
	vring_init(vr, size, ring_mem, VIRTIO_PCI_VRING_ALIGN); /* [한국어] desc/avail/used 영역의 시작 포인터를 정렬에 맞춰 계산해 vr 구조체에 채움. */
	vq->vq_used_cons_idx = 0;              /* [한국어] 이니시에이터가 다음에 읽어야 할 used 링 위치. 초기에는 백엔드가 아직 아무것도 게시하지 않음. */
	vq->vq_desc_head_idx = 0;              /* [한국어] 자유리스트 헤드 = 0번 디스크립터부터 사용. */
	vq->vq_avail_idx = 0;                  /* [한국어] 다음에 게시할 avail 위치. avail->idx와 누적값을 분리해 관리. */
	vq->vq_desc_tail_idx = (uint16_t)(vq->vq_nentries - 1); /* [한국어] 자유리스트 꼬리 = 마지막 디스크립터(반환받은 체인을 여기 뒤에 붙임). */
	vq->vq_free_cnt = vq->vq_nentries;     /* [한국어] 자유 디스크립터 개수 = 전체. 새 요청은 이 카운트에서 차감. */
	vq->req_start = VQ_RING_DESC_CHAIN_END; /* [한국어] 현재 작성 중인 요청 체인의 시작. END = "작성 중 요청 없음". */
	vq->req_end = VQ_RING_DESC_CHAIN_END;  /* [한국어] 현재 작성 중인 요청 체인의 끝. END = 동일. */
	vq->reqs_finished = 0;                 /* [한국어] flush 시 EVENT_IDX 트릭에 사용할, 마지막 flush 이후 완료된 요청 수. */
	memset(vq->vq_descx, 0, sizeof(struct vq_desc_extra) * vq->vq_nentries); /* [한국어] 각 디스크립터의 SPDK 메타(cookie, ndescs)를 0으로. */

	vring_desc_init(vr->desc, size); /* [한국어] desc 배열을 단일 자유리스트로 체인. (위 함수 참조) */

	/* Tell the backend not to interrupt us.
	 * If F_EVENT_IDX is negotiated, we will always set incredibly high
	 * used event idx, so that we will practically never receive an
	 * interrupt. See virtqueue_req_flush()
	 */
	/* [한국어] SPDK는 polled-mode이므로 백엔드가 인터럽트(또는 시그널)를 보낼 필요가 없다.
	 * 두 가지 경로가 있다:
	 *  1) EVENT_IDX 피처 협상 성공 시: used_event = UINT16_MAX로 두면 백엔드는 used.idx가
	 *     UINT16_MAX 도달 직전에만 신호하므로 사실상 절대 신호하지 않는다.
	 *  2) 미협상 시: avail->flags에 NO_INTERRUPT 비트를 켜서 백엔드가 신호 자체를 억제. */
	if (vq->vdev->negotiated_features & (1ULL << VIRTIO_RING_F_EVENT_IDX)) { /* [한국어] EVENT_IDX 피처가 협상되었는지 확인. */
		vring_used_event(&vq->vq_ring) = UINT16_MAX; /* [한국어] used_event 슬롯을 최대값으로 — 백엔드 통지 임계값을 사실상 무한대로. */
	} else {
		vq->vq_ring.avail->flags |= VRING_AVAIL_F_NO_INTERRUPT; /* [한국어] avail.flags에 NO_INTERRUPT — 백엔드가 인터럽트 송신 억제. */
	}
}

/*
 * [한국어]
 * virtio_init_queue - 단일 virtqueue 인덱스에 대해 메모리 할당 + 백엔드 셋업 + vring 초기화
 *
 * @dev            : 대상 virtio 디바이스
 * @vtpci_queue_idx: 큐 인덱스 (0..max_queues-1)
 * @return: 0 성공, 음수 errno (-EINVAL: 큐 미존재/비-2의-거듭제곱, -ENOMEM: 메모리 부족)
 *
 * 처리 단계:
 *   1) 백엔드에 query: 이 인덱스의 큐 크기(디바이스가 결정) 확인
 *   2) virtqueue 구조체 + descx 메타 배열을 cache-line 정렬로 할당
 *   3) vring 메모리(desc/avail/used) 크기 계산
 *   4) backend_ops->setup_queue로 트랜스포트별 메모리 할당(예: hugepage DMA) 위임
 *   5) virtio_init_vring으로 vring을 사용 가능한 상태로 초기화
 * setup_queue 콜백은 vq->vq_ring_virt_mem과 vq->vq_ring_mem(IOVA)을 채우는 책임이 있다.
 *
 * 실행 컨텍스트: 디바이스 초기화 시 단일 스레드. virtio_dev_acquire_queue 이전이므로 owner_thread = NULL.
 *
 * 호출 체인:
 *   virtio_dev_start → virtio_alloc_queues → [virtio_init_queue] → backend_ops->setup_queue
 */
static int
virtio_init_queue(struct virtio_dev *dev, uint16_t vtpci_queue_idx)
{
	unsigned int vq_size, size;  /* [한국어] vq_size: 큐 길이(디스크립터 수), size: 메모리 할당 바이트 수. */
	struct virtqueue *vq;        /* [한국어] 새로 할당할 virtqueue 포인터. */
	int rc;                       /* [한국어] 반환 코드. */

	SPDK_DEBUGLOG(virtio_dev, "setting up queue: %"PRIu16"\n", vtpci_queue_idx); /* [한국어] 디버그: 어떤 큐를 셋업하는지 로깅. */

	/*
	 * Read the virtqueue size from the Queue Size field
	 * Always power of 2 and if 0 virtqueue does not exist
	 */
	vq_size = virtio_dev_backend_ops(dev)->get_queue_size(dev, vtpci_queue_idx); /* [한국어] 백엔드(PCI 등)에 큐 크기 질의. virtio 스펙: queue_size 레지스터. */
	SPDK_DEBUGLOG(virtio_dev, "vq_size: %u\n", vq_size); /* [한국어] 디버그: 백엔드가 보고한 큐 크기. */
	if (vq_size == 0) {                                  /* [한국어] 0이면 해당 인덱스의 큐가 디바이스에 존재하지 않음. */
		SPDK_ERRLOG("virtqueue %"PRIu16" does not exist\n", vtpci_queue_idx); /* [한국어] 비정상 상황 — 호출자가 잘못된 인덱스 요청. */
		return -EINVAL;                              /* [한국어] EINVAL로 호출 체인 반환. */
	}

	if (!spdk_u32_is_pow2(vq_size)) { /* [한국어] virtio 스펙 요구사항: 큐 크기는 2의 거듭제곱이어야 (mod = and 마스크 사용을 위해). */
		SPDK_ERRLOG("virtqueue %"PRIu16" size (%u) is not powerof 2\n",
			    vtpci_queue_idx, vq_size); /* [한국어] 백엔드가 비표준값 보고 — 진행 불가. */
		return -EINVAL;
	}

	size = sizeof(*vq) + vq_size * sizeof(struct vq_desc_extra); /* [한국어] virtqueue 구조체 + 모든 디스크립터별 메타(descx) 가변 길이. */

	if (posix_memalign((void **)&vq, SPDK_CACHE_LINE_SIZE, size)) { /* [한국어] 캐시라인(보통 64B) 정렬 — false sharing 방지 + DMA 친화. */
		SPDK_ERRLOG("can not allocate vq\n");        /* [한국어] 시스템 메모리 부족. */
		return -ENOMEM;
	}
	memset(vq, 0, size);            /* [한국어] posix_memalign은 0 초기화 보장 X — 명시적 클리어로 모든 필드를 안전한 기본값으로. */
	dev->vqs[vtpci_queue_idx] = vq; /* [한국어] 디바이스 큐 배열에 등록 — 이후 backend setup_queue가 vdev->vqs로 인덱싱 가능. */

	vq->vdev = dev;                       /* [한국어] 백포인터 — vq에서 디바이스 ops 접근 시 사용. */
	vq->vq_queue_index = vtpci_queue_idx; /* [한국어] 자기 자신의 인덱스를 캐싱(notify 시 백엔드에 송신할 ID). */
	vq->vq_nentries = vq_size;            /* [한국어] vring 슬롯 수 캐싱 — modular wrap-around 마스크에 사용 (idx & (n-1)). */

	/*
	 * Reserve a memzone for vring elements
	 */
	size = vring_size(vq_size, VIRTIO_PCI_VRING_ALIGN); /* [한국어] virtio 스펙 공식: desc[N] + avail + used 영역 총합 (정렬 포함) 계산. */
	vq->vq_ring_size = SPDK_ALIGN_CEIL(size, VIRTIO_PCI_VRING_ALIGN); /* [한국어] 페이지/정렬 경계에 맞춰 올림 — 백엔드 BAR 매핑 호환. */
	SPDK_DEBUGLOG(virtio_dev, "vring_size: %u, rounded_vring_size: %u\n",
		      size, vq->vq_ring_size); /* [한국어] 정렬 전/후 크기 디버그. */

	vq->owner_thread = NULL; /* [한국어] 큐는 미할당 상태로 시작. virtio_dev_acquire_queue 호출 시 spdk_thread가 부여됨. */

	rc = virtio_dev_backend_ops(dev)->setup_queue(dev, vq); /* [한국어] 트랜스포트별 셋업: hugepage 메모리 할당 + BAR/MMIO에 desc/avail/used 주소 게시. */
	if (rc < 0) {
		SPDK_ERRLOG("setup_queue failed\n"); /* [한국어] 백엔드 셋업 실패 — 메모리/트랜스포트 에러. */
		free(vq);                          /* [한국어] 부분 할당 자원 회수. */
		dev->vqs[vtpci_queue_idx] = NULL;  /* [한국어] 디바이스 배열에서도 제거 — 후속 cleanup이 NULL 체크로 스킵. */
		return rc;
	}

	SPDK_DEBUGLOG(virtio_dev, "vq->vq_ring_mem:      0x%" PRIx64 "\n",
		      vq->vq_ring_mem); /* [한국어] 디버그: 백엔드에게 보일 IOVA(또는 PFN). */
	SPDK_DEBUGLOG(virtio_dev, "vq->vq_ring_virt_mem: 0x%" PRIx64 "\n",
		      (uint64_t)(uintptr_t)vq->vq_ring_virt_mem); /* [한국어] 디버그: 이니시에이터가 보는 가상 주소. */

	virtio_init_vring(vq); /* [한국어] 메모리 0 클리어 + desc/avail/used 포인터 배치 + 자유리스트 초기화 + 인터럽트 억제. */
	return 0;              /* [한국어] 성공. */
}

/*
 * [한국어]
 * virtio_free_queues - 디바이스의 모든 virtqueue를 트랜스포트 차원에서 해제하고 메모리 반환
 *
 * @dev: 대상 virtio 디바이스
 * @return: 없음
 *
 * 디바이스 정지(virtio_dev_stop)나 알로케이션 실패 롤백 시 호출. dev->vqs[] 배열의 각 요소에
 * 대해 backend_ops->del_queue로 vring 메모리(예: hugepage)와 백엔드 측 등록을 풀고,
 * SPDK 메타 구조체(virtqueue + descx)도 free 한다. dev->vqs 배열 자체도 해제하고 NULL로 설정.
 *
 * 실행 컨텍스트: 단일 스레드(디바이스 정지 경로). 큐는 이미 owner_thread = NULL이거나
 * 종료 단계라 동시 접근 없음.
 *
 * 호출 체인:
 *   virtio_dev_stop / virtio_alloc_queues(에러) → [virtio_free_queues] → backend_ops->del_queue
 */
static void
virtio_free_queues(struct virtio_dev *dev)
{
	uint16_t nr_vq = dev->max_queues; /* [한국어] 해제 대상 큐 개수 — alloc 시 기록한 max_queues. */
	struct virtqueue *vq;             /* [한국어] 순회용 virtqueue 포인터. */
	uint16_t i;                       /* [한국어] 인덱스 변수. */

	if (dev->vqs == NULL) {           /* [한국어] alloc 자체가 실패했거나 이미 해제된 디바이스 — no-op. */
		return;
	}

	for (i = 0; i < nr_vq; i++) {     /* [한국어] 모든 큐 순회. */
		vq = dev->vqs[i];
		if (!vq) {                /* [한국어] init 도중 실패한 큐는 NULL일 수 있음 — 안전하게 스킵. */
			continue;
		}

		virtio_dev_backend_ops(dev)->del_queue(dev, vq); /* [한국어] 트랜스포트별 해제: BAR 등록 해제 + vring 메모리 free. */

		free(vq);                 /* [한국어] virtqueue + descx 메타 구조체 해제. */
		dev->vqs[i] = NULL;       /* [한국어] 배열 슬롯도 NULL로 — 더블 free 방지. */
	}

	free(dev->vqs);                   /* [한국어] 큐 포인터 배열 자체도 해제. */
	dev->vqs = NULL;                  /* [한국어] 더블 free 방지 + 재초기화 가능 상태. */
}

/*
 * [한국어]
 * virtio_alloc_queues - 디바이스의 모든 큐를 일괄 초기화 (virtio_dev_start의 핵심 단계)
 *
 * @dev          : 대상 virtio 디바이스
 * @max_queues   : 디바이스가 노출할 총 큐 개수 (control queue 포함)
 * @fixed_vq_num : 처음 N개 큐는 control용으로 고정(예: virtio-scsi의 ctrl/event 큐)
 * @return: 0 성공, 음수 errno
 *
 * dev->vqs 배열을 calloc 후 0..max_queues-1까지 virtio_init_queue를 호출해 큐를 셋업한다.
 * 중간 실패 시 이미 만든 큐는 virtio_free_queues로 일괄 롤백한다. fixed_queues_num은
 * 상위 bdev 모듈이 어떤 큐를 데이터 큐로 분배할지 판단할 때 사용한다.
 *
 * 실행 컨텍스트: virtio_dev_start 호출 경로의 단일 스레드.
 *
 * 호출 체인:
 *   virtio_dev_start → [virtio_alloc_queues] → virtio_init_queue × N
 */
static int
virtio_alloc_queues(struct virtio_dev *dev, uint16_t max_queues, uint16_t fixed_vq_num)
{
	uint16_t i;  /* [한국어] 큐 인덱스 카운터. */
	int ret;     /* [한국어] virtio_init_queue 결과 저장. */

	if (max_queues == 0) {
		/* perfectly fine to have a device with no virtqueues. */
		/* [한국어] 일부 디바이스는 0큐 (예: 단순 config-only)도 가능 — 정상 경로로 처리. */
		return 0;
	}

	assert(dev->vqs == NULL); /* [한국어] 중복 alloc 방지 — 호출자는 이전 free 후 호출해야 함. */
	dev->vqs = calloc(1, sizeof(struct virtqueue *) * max_queues); /* [한국어] 포인터 배열만 할당(개별 vq는 init_queue에서). calloc → 모든 슬롯 NULL 초기화. */
	if (!dev->vqs) {
		SPDK_ERRLOG("failed to allocate %"PRIu16" vqs\n", max_queues); /* [한국어] 시스템 메모리 부족. */
		return -ENOMEM;
	}

	for (i = 0; i < max_queues; i++) {       /* [한국어] 모든 큐 순차 초기화. */
		ret = virtio_init_queue(dev, i); /* [한국어] 개별 큐 셋업 — 백엔드 setup_queue + vring init. */
		if (ret < 0) {
			virtio_free_queues(dev); /* [한국어] 부분 성공 롤백 — 이미 init된 큐만 free 됨(NULL 슬롯은 스킵). */
			return ret;              /* [한국어] 첫 실패 코드를 그대로 호출자에 반환. */
		}
	}

	dev->max_queues = max_queues;          /* [한국어] 성공적으로 모든 큐 init 완료 — 디바이스 메타에 기록. */
	dev->fixed_queues_num = fixed_vq_num;  /* [한국어] 컨트롤/이벤트 등 고정 큐 개수 — bdev_io 분배 시 참고. */
	return 0;
}

/**
 * Negotiate virtio features. For virtio_user this will also set
 * dev->modern flag if VIRTIO_F_VERSION_1 flag is negotiated.
 */
/*
 * [한국어]
 * virtio_negotiate_features - virtio 피처 협상 (드라이버↔디바이스 교집합 결정)
 *
 * @dev         : 대상 virtio 디바이스
 * @req_features: 드라이버(SPDK)가 원하는 피처 마스크 (예: VIRTIO_F_VERSION_1, RING_F_EVENT_IDX, etc.)
 * @return: 0 성공, 음수 errno
 *
 * virtio 1.x 협상 시퀀스:
 *   1) ACKNOWLEDGE 비트 set (호출자 책임 — virtio_dev_reset에서)
 *   2) DRIVER 비트 set (호출자 책임)
 *   3) get_features로 디바이스가 지원하는 피처 풀 획득
 *   4) (driver_wants ∩ device_supports)를 set_features로 디바이스에 통보
 *   5) FEATURES_OK 비트 set
 *   6) 디바이스가 FEATURES_OK 비트를 그대로 유지하는지 read-back 검증
 * 6단계에서 비트가 클리어되면 협상 거절(예: 드라이버가 의존성 누락 피처 셋) — 진행 불가.
 *
 * 실행 컨텍스트: virtio_dev_reset 경로의 단일 스레드.
 *
 * 호출 체인:
 *   virtio_dev_reset → [virtio_negotiate_features] → backend_ops->{get_features,set_features,set_status,get_status}
 */
static int
virtio_negotiate_features(struct virtio_dev *dev, uint64_t req_features)
{
	uint64_t host_features = virtio_dev_backend_ops(dev)->get_features(dev); /* [한국어] 디바이스가 지원하는 피처 풀 — 백엔드 MMIO 또는 vhost-user 응답으로 획득. */
	int rc;                                                                   /* [한국어] set_features 결과. */

	SPDK_DEBUGLOG(virtio_dev, "guest features = %" PRIx64 "\n", req_features);   /* [한국어] 드라이버가 원하는 피처. */
	SPDK_DEBUGLOG(virtio_dev, "device features = %" PRIx64 "\n", host_features); /* [한국어] 디바이스가 지원하는 피처. */

	rc = virtio_dev_backend_ops(dev)->set_features(dev, req_features & host_features); /* [한국어] 교집합을 디바이스에 통보 — 협상 결과 픽스. */
	if (rc != 0) {
		SPDK_ERRLOG("failed to negotiate device features.\n"); /* [한국어] 트랜스포트 통신 실패. */
		return rc;
	}

	SPDK_DEBUGLOG(virtio_dev, "negotiated features = %" PRIx64 "\n",
		      dev->negotiated_features); /* [한국어] set_features 콜백이 dev->negotiated_features를 채웠음. */

	virtio_dev_set_status(dev, VIRTIO_CONFIG_S_FEATURES_OK); /* [한국어] FEATURES_OK 비트 set — "협상 끝, 검증해줘"를 디바이스에 알림. */
	if (!(virtio_dev_get_status(dev) & VIRTIO_CONFIG_S_FEATURES_OK)) { /* [한국어] read-back: 디바이스가 비트를 유지하는지 확인. */
		SPDK_ERRLOG("failed to set FEATURES_OK status!\n");
		/* either the device failed, or we offered some features that
		 * depend on other, not offered features.
		 */
		/* [한국어] 디바이스가 비트를 클리어했다 = 우리 피처 셋이 일관성 없거나 디바이스가 거부. */
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * virtio_dev_construct - virtio_dev 기본 골격 초기화 (이름/뮤텍스/ops 바인딩)
 *
 * @vdev: 호출자가 할당한 virtio_dev 구조체 (전형적으로 트랜스포트별 init이 calloc 후 호출)
 * @name: 디바이스 이름 (디버깅/RPC 표기용, strdup으로 복제)
 * @ops : 트랜스포트별 콜백 테이블 (modern_ops / legacy_ops / vfio-user / vhost-user)
 * @ctx : 트랜스포트별 컨텍스트 (예: virtio_pci.c의 struct virtio_hw*)
 * @return: 0 성공, -ENOMEM/-pthread_error
 *
 * 메모리 할당이 실패할 수 있는 자원(이름, 뮤텍스)을 안전 순서로 초기화.
 * 큐는 아직 만들지 않으며, virtio_dev_start에서 별도로 처리.
 *
 * 실행 컨텍스트: 트랜스포트 attach/probe 경로의 단일 스레드.
 *
 * 호출 체인:
 *   virtio_pci_dev_init / virtio_vfio_user_dev_init / virtio_user_dev_init → [virtio_dev_construct]
 */
int
virtio_dev_construct(struct virtio_dev *vdev, const char *name,
		     const struct virtio_dev_ops *ops, void *ctx)
{
	int rc;  /* [한국어] mutex_init 결과. */

	vdev->name = strdup(name);  /* [한국어] 호출자 문자열 수명에 의존하지 않도록 복제 — destruct에서 free. */
	if (vdev->name == NULL) {
		return -ENOMEM;     /* [한국어] strdup 실패 = 메모리 부족. */
	}

	rc = pthread_mutex_init(&vdev->mutex, NULL); /* [한국어] 큐 acquire/release 시 사용할 뮤텍스. cross-thread 큐 부여 직렬화. */
	if (rc != 0) {
		free(vdev->name);  /* [한국어] 부분 성공 롤백 — strdup 결과 회수. */
		return -rc;        /* [한국어] pthread는 양수 errno 반환 — 음수로 변환. */
	}

	vdev->backend_ops = ops; /* [한국어] 트랜스포트 콜백 테이블 바인딩 — 이후 모든 백엔드 호출 진입점. */
	vdev->ctx = ctx;         /* [한국어] 트랜스포트 사설 컨텍스트 포인터. */

	return 0;
}

/*
 * [한국어]
 * virtio_dev_reset - 디바이스 리셋 후 ACK→DRIVER 시퀀스 + 피처 협상
 *
 * @dev         : 대상 디바이스
 * @req_features: 드라이버가 요청할 피처 마스크 (VIRTIO_F_VERSION_1은 강제로 OR됨 — modern 강제)
 * @return: 0 성공, -EIO(상태 비트 미세트), 기타 협상 에러
 *
 * virtio 1.x 표준 초기화 시퀀스:
 *   1) virtio_dev_stop: 기존 RUN 상태면 RESET 비트 set + 큐 해제
 *   2) ACKNOWLEDGE: 드라이버가 디바이스를 인식했음을 통지
 *   3) DRIVER     : 드라이버가 어떻게 다룰지 안다는 통지
 *   4) feature negotiate (FEATURES_OK까지 포함)
 * 호출자는 이후 virtio_dev_start로 큐 만들고 DRIVER_OK까지 진행한다.
 *
 * 실행 컨텍스트: 디바이스 attach/probe 경로의 단일 스레드.
 *
 * 호출 체인:
 *   bdev_virtio_attach → [virtio_dev_reset] → virtio_dev_stop / virtio_negotiate_features
 */
int
virtio_dev_reset(struct virtio_dev *dev, uint64_t req_features)
{
	req_features |= (1ULL << VIRTIO_F_VERSION_1); /* [한국어] SPDK virtio 이니시에이터는 modern(virtio 1.x)만 지원 — VERSION_1 강제. */

	virtio_dev_stop(dev); /* [한국어] 기존 상태가 있다면 RESET 비트로 디바이스 정지하고 큐 메모리 회수. */

	virtio_dev_set_status(dev, VIRTIO_CONFIG_S_ACKNOWLEDGE); /* [한국어] 드라이버 인식 단계 — virtio 스펙 §3.1.1. */
	if (!(virtio_dev_get_status(dev) & VIRTIO_CONFIG_S_ACKNOWLEDGE)) { /* [한국어] read-back으로 비트 set 검증. */
		SPDK_ERRLOG("Failed to set VIRTIO_CONFIG_S_ACKNOWLEDGE status.\n");
		return -EIO;
	}

	virtio_dev_set_status(dev, VIRTIO_CONFIG_S_DRIVER); /* [한국어] 드라이버 활성 단계 — 디바이스가 드라이버 진행을 기다림을 의미. */
	if (!(virtio_dev_get_status(dev) & VIRTIO_CONFIG_S_DRIVER)) {
		SPDK_ERRLOG("Failed to set VIRTIO_CONFIG_S_DRIVER status.\n");
		return -EIO;
	}

	return virtio_negotiate_features(dev, req_features); /* [한국어] 피처 협상 진행 — 성공 시 FEATURES_OK까지 set. */
}

/*
 * [한국어]
 * virtio_dev_start - 큐 메모리 할당 + DRIVER_OK 게시 (디바이스 사용 가능 진입)
 *
 * @vdev            : 대상 디바이스 (이미 reset/협상 완료)
 * @max_queues      : 노출할 총 큐 개수
 * @fixed_queue_num : 컨트롤/이벤트 등 데이터가 아닌 큐 개수
 * @return: 0 성공, 음수 errno (-1: DRIVER_OK 미세트), -ENOMEM(큐 alloc 실패)
 *
 * virtio_dev_reset 직후 호출하는 마지막 진입 단계. virtio_alloc_queues로 모든 큐를
 * 셋업한 뒤 DRIVER_OK 비트를 set하면 디바이스가 본격적으로 avail 링을 처리하기 시작한다.
 *
 * 실행 컨텍스트: 단일 스레드 (attach 경로).
 *
 * 호출 체인:
 *   bdev_virtio_attach → virtio_dev_reset → [virtio_dev_start] → virtio_alloc_queues → virtio_init_queue × N
 */
int
virtio_dev_start(struct virtio_dev *vdev, uint16_t max_queues, uint16_t fixed_queue_num)
{
	int ret;  /* [한국어] 단계별 결과. */

	ret = virtio_alloc_queues(vdev, max_queues, fixed_queue_num); /* [한국어] 모든 큐 vring 메모리 할당 + 백엔드 등록. */
	if (ret < 0) {
		return ret;
	}

	virtio_dev_set_status(vdev, VIRTIO_CONFIG_S_DRIVER_OK); /* [한국어] DRIVER_OK = 디바이스 본격 동작 시작 신호. */
	if (!(virtio_dev_get_status(vdev) & VIRTIO_CONFIG_S_DRIVER_OK)) { /* [한국어] read-back으로 디바이스가 비트 유지 확인. */
		SPDK_ERRLOG("Failed to set VIRTIO_CONFIG_S_DRIVER_OK status.\n");
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * virtio_dev_destruct - virtio_dev 자원 최종 해제 (트랜스포트 ctx + 뮤텍스 + 이름)
 *
 * @dev: 대상 디바이스
 * @return: 없음
 *
 * 호출 전 virtio_dev_stop으로 큐가 해제되어 있어야 한다. 트랜스포트별 destruct_dev 콜백이
 * 트랜스포트 컨텍스트(예: PCI BAR unmap, vfio-user release)를 처리하고, 본 함수가 공통 자원을 해제.
 *
 * 실행 컨텍스트: 디바이스 detach 경로의 단일 스레드.
 *
 * 호출 체인:
 *   bdev_virtio_detach → virtio_dev_stop → [virtio_dev_destruct] → backend_ops->destruct_dev
 */
void
virtio_dev_destruct(struct virtio_dev *dev)
{
	virtio_dev_backend_ops(dev)->destruct_dev(dev); /* [한국어] 트랜스포트별 자원 회수 (BAR unmap, fd close 등). */
	pthread_mutex_destroy(&dev->mutex);             /* [한국어] 뮤텍스 해제. */
	free(dev->name);                                /* [한국어] strdup으로 복제한 이름 회수. */
}

/*
 * [한국어]
 * vq_ring_free_chain - 사용 후 디스크립터 체인을 자유리스트 tail에 반환
 *
 * @vq      : 대상 virtqueue
 * @desc_idx: 반환할 체인의 head 인덱스 (used.ring[].id에서 얻음)
 * @return: 없음
 *
 * 백엔드가 used 링에 반환한 체인을 SPDK가 다시 자유리스트에 붙이는 함수.
 * 동작:
 *   1) descx[head].ndescs 만큼 vq_free_cnt 복원
 *   2) NEXT 플래그를 따라가며 체인 끝(desc_idx_last) 탐색 (INDIRECT 디스크립터 제외)
 *   3) ndescs를 0으로 — descx 메타도 클리어
 *   4) 자유리스트 tail에 이 체인을 append (혹은 비어 있던 자유리스트라면 head로 설정)
 *   5) 체인 끝의 next를 CHAIN_END로 마무리 (자유리스트 종단 갱신)
 *
 * 실행 컨텍스트: 큐 owner_thread 단일 스레드(virtio_recv_pkts 경로). lockless.
 *
 * 호출 체인:
 *   virtio_recv_pkts → virtqueue_dequeue_burst_rx → [vq_ring_free_chain]
 *   virtqueue_req_abort → [vq_ring_free_chain]
 */
static void
vq_ring_free_chain(struct virtqueue *vq, uint16_t desc_idx)
{
	struct vring_desc *dp, *dp_tail;     /* [한국어] dp: 현재 노드 포인터, dp_tail: 자유리스트 마지막 노드. */
	struct vq_desc_extra *dxp;           /* [한국어] descx 메타(ndescs/cookie). */
	uint16_t desc_idx_last = desc_idx;   /* [한국어] 체인 끝 인덱스 추적 — append 위치 결정에 사용. */

	dp  = &vq->vq_ring.desc[desc_idx];   /* [한국어] head 디스크립터 시작. */
	dxp = &vq->vq_descx[desc_idx];       /* [한국어] head의 SPDK 메타. ndescs = 이 체인의 디스크립터 총 수. */
	vq->vq_free_cnt = (uint16_t)(vq->vq_free_cnt + dxp->ndescs); /* [한국어] 자유 카운트 복원 — 다음 req_start가 사용 가능. */
	if ((dp->flags & VRING_DESC_F_INDIRECT) == 0) { /* [한국어] INDIRECT면 next는 보조 테이블 인덱스이므로 follow 금지. */
		while (dp->flags & VRING_DESC_F_NEXT) { /* [한국어] NEXT 플래그가 있는 동안 체인 따라가기. */
			desc_idx_last = dp->next;       /* [한국어] 마지막 인덱스 갱신. */
			dp = &vq->vq_ring.desc[dp->next]; /* [한국어] 다음 노드로 이동. */
		}
	}
	dxp->ndescs = 0;  /* [한국어] head의 메타 클리어 — 다음 사용 전 깨끗한 상태. */

	/*
	 * We must append the existing free chain, if any, to the end of
	 * newly freed chain. If the virtqueue was completely used, then
	 * head would be VQ_RING_DESC_CHAIN_END (ASSERTed above).
	 */
	/* [한국어] 자유리스트 갱신 전략:
	 * - tail이 END(=자유리스트 비어 있음): head를 새 체인의 시작으로 설정.
	 * - tail이 유효: 기존 tail의 next를 새 체인 head로 연결 (append). */
	if (vq->vq_desc_tail_idx == VQ_RING_DESC_CHAIN_END) {
		vq->vq_desc_head_idx = desc_idx;        /* [한국어] 자유리스트가 비어 있었으니 head = 새 체인 시작. */
	} else {
		dp_tail = &vq->vq_ring.desc[vq->vq_desc_tail_idx]; /* [한국어] 기존 tail 디스크립터. */
		dp_tail->next = desc_idx;               /* [한국어] tail.next = 새 체인 head — 연결. */
	}

	vq->vq_desc_tail_idx = desc_idx_last;           /* [한국어] tail 갱신: 새로 붙인 체인의 끝. */
	dp->next = VQ_RING_DESC_CHAIN_END;              /* [한국어] 자유리스트 끝 표시 — 다음 free 시 검색이 여기서 멈춤. */
}

/*
 * [한국어]
 * virtqueue_dequeue_burst_rx - used 링에서 num개 완료 항목을 가져와 cookie/length 추출
 *
 * @vq      : 대상 virtqueue
 * @rx_pkts : [out] 완료된 요청들의 cookie 포인터 배열 (호출자가 할당)
 * @len     : [out] 각 완료의 used.len (디바이스가 기록한 처리 바이트 수)
 * @num     : 가져올 최대 개수 (호출자 선검증, used 링에 그만큼 있다고 가정)
 * @return: 실제 가져온 개수 (cookie NULL을 만나면 그 직전까지)
 *
 * 동작:
 *   1) used_cons_idx 마스크해서 used.ring 슬롯 인덱스 계산 (& (n-1)는 mod n과 동일, 2의 거듭제곱이라 가능)
 *   2) used_elem.id에서 디스크립터 head 인덱스, used_elem.len에서 처리 바이트 추출
 *   3) descx[head].cookie를 회수 → 상위 모듈(bdev_virtio)이 bdev_io를 복원
 *   4) vq_ring_free_chain으로 디스크립터 체인 자유리스트로 반환
 *   5) used_cons_idx 증가 (다음 회수 위치)
 * cookie가 NULL이면 사용자 미지정 또는 이중 완료 — 경고 후 break.
 *
 * 실행 컨텍스트: 큐 owner_thread 단일 스레드(폴링).
 *
 * 호출 체인:
 *   virtio_recv_pkts → [virtqueue_dequeue_burst_rx] → vq_ring_free_chain
 */
static uint16_t
virtqueue_dequeue_burst_rx(struct virtqueue *vq, void **rx_pkts,
			   uint32_t *len, uint16_t num)
{
	struct vring_used_elem *uep;  /* [한국어] used 링의 한 항목 (id + len). */
	void *cookie;                 /* [한국어] descx에 저장된 사용자 컨텍스트(보통 bdev_io 등). */
	uint16_t used_idx, desc_idx;  /* [한국어] used 링 슬롯 인덱스, 디스크립터 head 인덱스. */
	uint16_t i;                   /* [한국어] 회수 카운터. */

	/*  Caller does the check */
	for (i = 0; i < num ; i++) {
		used_idx = (uint16_t)(vq->vq_used_cons_idx & (vq->vq_nentries - 1)); /* [한국어] 큐 크기가 2의 거듭제곱이므로 mod 대신 AND 마스크 — 빠름. */
		uep = &vq->vq_ring.used->ring[used_idx];   /* [한국어] 백엔드가 게시한 완료 항목 위치. */
		desc_idx = (uint16_t) uep->id;             /* [한국어] 완료된 요청의 디스크립터 head 인덱스. */
		len[i] = uep->len;                         /* [한국어] 디바이스가 기록한 처리 바이트 수 (read 응답 길이 등). */
		cookie = vq->vq_descx[desc_idx].cookie;    /* [한국어] req_start에서 저장한 상위 컨텍스트(bdev_io 등) 회수. */

		if (spdk_unlikely(cookie == NULL)) {       /* [한국어] cookie 부재 = 비정상(이중 완료/미지정). __builtin_expect로 정상 경로 최적화. */
			SPDK_WARNLOG("vring descriptor with no mbuf cookie at %"PRIu16"\n",
				     vq->vq_used_cons_idx);
			break;                              /* [한국어] 이상 항목에서 멈춰 호출자에게 부분 결과 반환. */
		}

		__builtin_prefetch(cookie);                 /* [한국어] cookie 데이터(bdev_io 등) 프리페치 — 캐시 미스 비용 숨김. */

		rx_pkts[i]  = cookie;                       /* [한국어] 상위 호출자에게 cookie 전달. */
		vq->vq_used_cons_idx++;                     /* [한국어] 다음 폴링 위치로 진행. */
		vq_ring_free_chain(vq, desc_idx);           /* [한국어] 디스크립터 체인을 자유리스트에 반환. */
		vq->vq_descx[desc_idx].cookie = NULL;       /* [한국어] cookie 클리어 — 다음 사용 전 누수/오용 방지. */
	}

	return i;
}

/*
 * [한국어]
 * finish_req - 현재 작성 중인 디스크립터 체인을 avail 링에 게시(하위 단계)
 *
 * @vq: 대상 virtqueue
 * @return: 없음
 *
 * virtqueue_req_start로 시작해 virtqueue_req_add_iovs로 채운 체인의 마지막을 마감한다:
 *   1) 체인 끝 디스크립터의 NEXT 플래그 클리어 (마지막 디스크립터임을 표시)
 *   2) avail->ring[avail_idx]에 체인 head 인덱스 기록
 *   3) 메모리 배리어(virtio_wmb) 후 avail->idx 증가 — 백엔드가 새 게시를 인지
 *   4) reqs_finished 카운터 증가 (flush 시 EVENT_IDX 트릭에 사용)
 *
 * 메모리 배리어가 중요한 이유: 백엔드가 avail->idx를 먼저 보고 idx-1까지의 ring[]을
 * 읽으므로, ring[] 쓰기가 idx 갱신 이전에 가시화돼야 한다 (TSO에서도 컴파일러 reorder 차단).
 *
 * 실행 컨텍스트: 큐 owner_thread 단일 스레드.
 *
 * 호출 체인:
 *   virtqueue_req_start (이전 요청이 미flush일 때) / virtqueue_req_flush → [finish_req]
 */
static void
finish_req(struct virtqueue *vq)
{
	struct vring_desc *desc;  /* [한국어] 체인의 마지막 디스크립터. */
	uint16_t avail_idx;       /* [한국어] avail.ring 슬롯 인덱스. */

	desc = &vq->vq_ring.desc[vq->req_end];     /* [한국어] req_end는 add_iovs가 마지막에 채운 디스크립터 인덱스. */
	desc->flags &= ~VRING_DESC_F_NEXT;         /* [한국어] NEXT 클리어 — 백엔드가 여기서 체인 끝임을 인식. */

	/*
	 * Place the head of the descriptor chain into the next slot and make
	 * it usable to the host. The chain is made available now rather than
	 * deferring to virtqueue_req_flush() in the hopes that if the host is
	 * currently running on another CPU, we can keep it processing the new
	 * descriptor.
	 */
	/* [한국어] 멀티코어 백엔드가 다른 CPU에서 폴링 중일 수 있으므로,
	 * flush를 기다리지 않고 즉시 avail 게시 — 백엔드 파이프라이닝 향상. */
	avail_idx = (uint16_t)(vq->vq_avail_idx & (vq->vq_nentries - 1)); /* [한국어] avail.ring 슬롯 (mod n). */
	vq->vq_ring.avail->ring[avail_idx] = vq->req_start; /* [한국어] avail에 체인 head 게시 — 백엔드는 desc[head]부터 읽음. */
	vq->vq_avail_idx++;                                /* [한국어] SPDK 측 누적 카운트 — wrap 모듈러 분리 관리. */
	vq->req_end = VQ_RING_DESC_CHAIN_END;              /* [한국어] 작성 중 요청 마감 표시. */
	virtio_wmb();                                      /* [한국어] avail.ring[] 쓰기 가시성 → avail.idx 갱신 이전에 보장. */
	vq->vq_ring.avail->idx = vq->vq_avail_idx;          /* [한국어] 백엔드가 모니터링하는 진짜 avail 인덱스 게시. */
	vq->reqs_finished++;                                /* [한국어] EVENT_IDX 트릭에서 사용 — 마지막 flush 이후 게시 수. */
}

/*
 * [한국어]
 * virtqueue_req_start - 새 요청 작성 시작 (자유리스트에서 head 예약)
 *
 * @vq    : 대상 virtqueue
 * @cookie: 상위(bdev_io 등) 컨텍스트 — 완료 시 dequeue_burst_rx가 회수
 * @iovcnt: 이 요청에 사용할 iovec 개수 (주의: DMA 주소 분할 시 더 많이 쓸 수 있어 2배 예약 검사)
 * @return: 0 성공, -ENOMEM(자유 디스크립터 부족), -EINVAL(iovcnt가 큐 크기 초과)
 *
 * 동작:
 *   1) 디스크립터 부족 검사: iovec 1개당 최대 2개의 디스크립터(주소 경계 분할)가 필요할 수 있음
 *   2) 이전 작성 중인 미flush 요청이 있다면 finish_req로 즉시 게시 — 큐를 깨끗이
 *   3) 자유리스트 head를 req_start로 예약, descx에 cookie 저장
 * 이후 호출자는 virtqueue_req_add_iovs로 데이터 디스크립터를 추가하고
 * 마지막에 virtqueue_req_flush를 호출해야 한다.
 *
 * 실행 컨텍스트: 큐 owner_thread 단일 스레드.
 *
 * 호출 체인:
 *   bdev_virtio_blk/scsi submit_request → [virtqueue_req_start]
 */
int
virtqueue_req_start(struct virtqueue *vq, void *cookie, int iovcnt)
{
	struct vq_desc_extra *dxp;  /* [한국어] head 디스크립터의 SPDK 메타. */

	/* Reserve enough entries to handle iov split */
	/* [한국어] 같은 iovec이라도 DMA 주소가 hugepage 경계를 가로지르면 2개의 디스크립터로 쪼개야 한다.
	 * 따라서 최악의 경우 2*iovcnt 디스크립터가 필요. */
	if (2 * iovcnt > vq->vq_free_cnt) {
		return iovcnt > vq->vq_nentries ? -EINVAL : -ENOMEM;
		/* [한국어] iovcnt가 큐 전체 크기를 넘으면 영구 불가 → EINVAL. 아니면 일시적 자원 부족 → ENOMEM. */
	}

	if (vq->req_end != VQ_RING_DESC_CHAIN_END) { /* [한국어] 이전 요청이 add 중간에 멈춰 있다면 — 그것을 즉시 게시. */
		finish_req(vq);                       /* [한국어] avail 게시 후 req_end = END로. */
	}

	vq->req_start = vq->vq_desc_head_idx;    /* [한국어] 자유리스트 head를 새 요청의 시작으로 예약. */
	dxp = &vq->vq_descx[vq->req_start];      /* [한국어] head의 메타 슬롯. */
	dxp->cookie = cookie;                    /* [한국어] 완료 시 회수할 상위 컨텍스트 저장. */
	dxp->ndescs = 0;                         /* [한국어] add_iovs가 호출될 때마다 증가. */

	return 0;
}

/*
 * [한국어]
 * virtqueue_req_flush - 현재 요청 게시(finish_req) + 메모리 배리어 + 백엔드 통지(필요 시)
 *
 * @vq: 대상 virtqueue
 * @return: 없음
 *
 * 동작:
 *   1) 작성 중 요청을 finish_req로 avail 링에 게시
 *   2) full 메모리 배리어로 게시 가시성 확보 후 used_event 갱신
 *   3) EVENT_IDX 협상 시: used_event 임계값 갱신 + vring_need_event 검사로 통지 필요 여부 판단
 *      미협상 시: VRING_USED_F_NO_NOTIFY 플래그 검사
 *   4) 통지 필요 시 backend_ops->notify_queue 호출 (PCI MMIO doorbell 또는 vhost-user kick fd)
 *
 * 실행 컨텍스트: 큐 owner_thread 단일 스레드.
 *
 * 호출 체인:
 *   bdev_virtio submit (bdev_io 들 add 후 마지막) → [virtqueue_req_flush] → backend_ops->notify_queue
 */
void
virtqueue_req_flush(struct virtqueue *vq)
{
	uint16_t reqs_finished;  /* [한국어] 마지막 flush 이후 누적 게시 수 — EVENT_IDX 임계값 비교용. */

	if (vq->req_end == VQ_RING_DESC_CHAIN_END) {
		/* no non-empty requests have been started */
		/* [한국어] 작성 중 요청 없음 — 플러시할 게 없음. */
		return;
	}

	finish_req(vq);   /* [한국어] avail->ring + avail->idx 게시. */
	virtio_mb();      /* [한국어] full barrier — used_event 읽기/쓰기와 avail 게시 사이의 일관성. */

	reqs_finished = vq->reqs_finished; /* [한국어] 누적 게시 수 캡처. */
	vq->reqs_finished = 0;             /* [한국어] 다음 flush 사이클 위해 리셋. */

	if (vq->vdev->negotiated_features & (1ULL << VIRTIO_RING_F_EVENT_IDX)) { /* [한국어] EVENT_IDX 피처 협상된 경우 (modern virtio). */
		/* Set used event idx to a value the device will never reach.
		 * This effectively disables interrupts.
		 */
		/* [한국어] used_event = (cons - n - 1)로 설정 — wrap 고려해도 백엔드가 사실상 도달 불가 → 인터럽트 영구 억제. */
		vring_used_event(&vq->vq_ring) = vq->vq_used_cons_idx - vq->vq_nentries - 1;

		/* [한국어] 백엔드 측 avail_event를 보고 통지가 필요한지 검사.
		 * vring_need_event(event_idx, new_idx, old_idx)는 [old_idx, new_idx) 구간에 event_idx가 포함되는지 판단.
		 * 포함되지 않으면 백엔드가 이 게시를 보고도 "아직 통지 임계 미도달"로 처리 → 통지 생략 가능. */
		if (!vring_need_event(vring_avail_event(&vq->vq_ring),
				      vq->vq_avail_idx,
				      vq->vq_avail_idx - reqs_finished)) {
			return;  /* [한국어] 통지 불필요 — 백엔드는 폴링으로 발견할 것. */
		}
	} else if (vq->vq_ring.used->flags & VRING_USED_F_NO_NOTIFY) { /* [한국어] 미협상: 백엔드가 NO_NOTIFY 플래그 set이면 통지 생략. */
		return;
	}

	virtio_dev_backend_ops(vq->vdev)->notify_queue(vq->vdev, vq); /* [한국어] PCI: queue_notify MMIO write. vhost-user: kick fd write. vfio-user: no-op (폴링). */
	SPDK_DEBUGLOG(virtio_dev, "Notified backend after xmit\n");   /* [한국어] 디버그: 통지 발생 기록. */
}

/*
 * [한국어]
 * virtqueue_req_abort - 작성 중인 요청을 게시하지 않고 폐기(자유리스트로 반환)
 *
 * @vq: 대상 virtqueue
 * @return: 없음
 *
 * req_start 후 add_iovs 도중 에러가 나서 요청을 보내지 않기로 한 경우 호출.
 * 동작:
 *   1) req_start = END면 시작도 안 한 상태 — no-op
 *   2) 마지막 디스크립터 NEXT 클리어 (cleanly 종결)
 *   3) vq_ring_free_chain으로 디스크립터들 자유리스트에 반환
 *   4) req_start = END 표시
 *
 * 실행 컨텍스트: 큐 owner_thread 단일 스레드.
 *
 * 호출 체인:
 *   bdev_virtio (add_iovs 실패) → [virtqueue_req_abort] → vq_ring_free_chain
 */
void
virtqueue_req_abort(struct virtqueue *vq)
{
	struct vring_desc *desc;  /* [한국어] 마지막 디스크립터. */

	if (vq->req_start == VQ_RING_DESC_CHAIN_END) {
		/* no requests have been started */
		/* [한국어] 시작 자체가 안 된 상태 — 폐기할 것 없음. */
		return;
	}

	desc = &vq->vq_ring.desc[vq->req_end];   /* [한국어] 마지막 디스크립터 위치. */
	desc->flags &= ~VRING_DESC_F_NEXT;       /* [한국어] NEXT 클리어 — 자유리스트 반환 시 깨끗한 상태. */

	vq_ring_free_chain(vq, vq->req_start);   /* [한국어] req_start부터 시작하는 체인 전체를 자유리스트 tail에 반환. */
	vq->req_start = VQ_RING_DESC_CHAIN_END;  /* [한국어] 작성 중 표시 클리어. */
}

/*
 * [한국어]
 * virtqueue_req_add_iovs - iovec 배열을 디스크립터 체인으로 변환해 추가
 *
 * @vq       : 대상 virtqueue (req_start 호출 후여야 함)
 * @iovs     : 추가할 iovec 배열
 * @iovcnt   : iovec 개수
 * @desc_type: 디스크립터 방향 — SPDK_VIRTIO_DESC_RO(읽기, 백엔드→이니시에이터)
 *             또는 SPDK_VIRTIO_DESC_WR(쓰기, 이니시에이터→백엔드)
 *
 * 한 iovec이 hugepage 경계를 가로질러 물리적으로 분할되면 spdk_vtophys가 한 번에 변환할 수
 * 없으므로 여러 디스크립터로 쪼갠다(while 내부 루프). HW 디바이스(is_hw=1)는 IOVA 주소가
 * 필요하므로 spdk_vtophys 사용, 가상 디바이스(vhost-user)는 VA를 그대로 desc.addr에 적재.
 * 모든 디스크립터는 NEXT 플래그를 켜둔 상태로 두고, finish_req에서 마지막 NEXT만 클리어.
 *
 * 실행 컨텍스트: 큐 owner_thread 단일 스레드. req_start와 req_flush 사이에서 호출.
 *
 * 호출 체인:
 *   bdev_virtio submit_request → virtqueue_req_start → [virtqueue_req_add_iovs] (1회 이상) → virtqueue_req_flush
 */
void
virtqueue_req_add_iovs(struct virtqueue *vq, struct iovec *iovs, uint16_t iovcnt,
		       enum spdk_virtio_desc_type desc_type)
{
	struct vring_desc *desc;       /* [한국어] 현재 채울 디스크립터. */
	struct vq_desc_extra *dxp;     /* [한국어] head 디스크립터의 메타. */
	uint16_t i, prev_head, new_head; /* [한국어] iovec 카운터, 직전 디스크립터, 다음 빈 디스크립터. */
	uint64_t processed_length, iovec_length, current_length; /* [한국어] iovec 분할 진행 길이/현재 segment 길이. */
	void *current_base;            /* [한국어] iovec 내부 진행 포인터. */
	uint16_t used_desc_count = 0;  /* [한국어] 이번 호출에서 사용한 디스크립터 수 — descx ndescs에 누적. */

	assert(vq->req_start != VQ_RING_DESC_CHAIN_END); /* [한국어] req_start가 호출되었어야 함. */
	assert(iovcnt <= vq->vq_free_cnt);                /* [한국어] req_start에서 이미 검증했지만 이중 안전장치. */

	/* TODO use indirect descriptors if iovcnt is high enough
	 * or the caller specifies SPDK_VIRTIO_DESC_F_INDIRECT
	 */
	/* [한국어] TODO: iovcnt가 많거나 INDIRECT 명시 시 INDIRECT 디스크립터 사용 가능 — 현재 미구현. */

	prev_head = vq->req_end;            /* [한국어] 이전 add 호출이 남긴 마지막 디스크립터 (체인 연결 시작점). */
	new_head = vq->vq_desc_head_idx;    /* [한국어] 자유리스트 head — 여기서 떼어내며 사용. */
	for (i = 0; i < iovcnt; ++i) {
		processed_length = 0;             /* [한국어] 이 iovec에서 처리한 길이. */
		iovec_length = iovs[i].iov_len;   /* [한국어] 이 iovec의 전체 길이. */
		current_base = iovs[i].iov_base;  /* [한국어] 진행 시 증가시킬 가상 주소. */

		while (processed_length < iovec_length) { /* [한국어] DMA 분할: hugepage 경계마다 분리되므로 루프. */
			desc = &vq->vq_ring.desc[new_head]; /* [한국어] 자유리스트에서 가져온 디스크립터. */
			current_length = iovec_length - processed_length; /* [한국어] 남은 길이 — vtophys가 줄여줄 수 있음. */

			if (!vq->vdev->is_hw) {       /* [한국어] vhost-user처럼 VA를 그대로 사용하는 트랜스포트. */
				desc->addr  = (uintptr_t)current_base;
			} else {                       /* [한국어] PCI/vfio-user 같은 HW형 — IOVA(=물리주소 또는 IOMMU 주소) 필요. */
				desc->addr = spdk_vtophys(current_base, &current_length);
				/* [한국어] vtophys가 hugepage 경계까지의 연속 길이를 current_length에 갱신 — 자동 분할. */
			}

			desc->len = current_length;   /* [한국어] 이번 segment 길이. */
			/* always set NEXT flag. unset it on the last descriptor
			 * in the request-ending function.
			 */
			/* [한국어] desc_type(R/W) | NEXT — finish_req에서만 마지막 NEXT 클리어. */
			desc->flags = desc_type | VRING_DESC_F_NEXT;

			prev_head = new_head;          /* [한국어] 직전 = 현재 — 마지막 디스크립터 추적. */
			new_head = desc->next;         /* [한국어] 자유리스트 다음 노드로 진행. */
			used_desc_count++;             /* [한국어] 사용 카운트 증가. */

			processed_length += current_length; /* [한국어] 처리 길이 누적. */
			current_base += current_length;     /* [한국어] base 포인터 진행. */
		}
	}

	dxp = &vq->vq_descx[vq->req_start];  /* [한국어] head 메타. */
	dxp->ndescs += used_desc_count;       /* [한국어] free_chain이 한 번에 회수할 디스크립터 수 누적. */

	vq->req_end = prev_head;              /* [한국어] 마지막 디스크립터 인덱스 — finish_req가 NEXT 클리어할 위치. */
	vq->vq_desc_head_idx = new_head;      /* [한국어] 자유리스트 head를 사용한 만큼 진행. */
	vq->vq_free_cnt = (uint16_t)(vq->vq_free_cnt - used_desc_count); /* [한국어] 자유 카운트 차감. */
	if (vq->vq_desc_head_idx == VQ_RING_DESC_CHAIN_END) { /* [한국어] 자유리스트 소진 — 디스크립터 전부 사용 중. */
		assert(vq->vq_free_cnt == 0);              /* [한국어] 일관성 검증. */
		vq->vq_desc_tail_idx = VQ_RING_DESC_CHAIN_END; /* [한국어] tail도 END로 표시 — 다음 free_chain이 head/tail 둘 다 갱신. */
	}
}

/* [한국어] 캐시라인 한 줄에 몇 개의 디스크립터가 들어가는지 — recv 시 캐시라인 정렬 효과를 위한 임계값. */
#define DESC_PER_CACHELINE (SPDK_CACHE_LINE_SIZE / sizeof(struct vring_desc))
/*
 * [한국어]
 * virtio_recv_pkts - used 링에서 완료된 요청 cookie를 회수 (폴링)
 *
 * @vq     : 대상 virtqueue
 * @io     : [out] 완료된 cookie 배열 (호출자 할당)
 * @len    : [out] 각 완료의 used.len
 * @nb_pkts: 가져올 최대 개수 (호출자 버퍼 크기)
 * @return: 실제 가져온 개수 (0 ~ nb_pkts)
 *
 * 동작:
 *   1) 백엔드의 used->idx 읽고 SPDK 측 vq_used_cons_idx와 차분으로 가용 완료 수 계산
 *   2) read 배리어(virtio_rmb)로 used.ring 데이터 가시성 확보
 *   3) 캐시라인 정렬: 가능한 경우 회수 개수를 캐시라인 경계에 맞춰 줄임 — 다음 폴링에서
 *      좋은 정렬 상태 유지 (likely 분기로 주 최적 경로 표시)
 *   4) virtqueue_dequeue_burst_rx에 위임
 *
 * 실행 컨텍스트: 큐 owner_thread의 SPDK poller — 폴링 루프에서 주기적 호출.
 *
 * 호출 체인:
 *   spdk_poller (bdev_virtio) → [virtio_recv_pkts] → virtqueue_dequeue_burst_rx
 */
uint16_t
virtio_recv_pkts(struct virtqueue *vq, void **io, uint32_t *len, uint16_t nb_pkts)
{
	uint16_t nb_used, num;  /* [한국어] 백엔드가 게시한 미회수 완료 수 / 실제 회수할 개수. */

	nb_used = vq->vq_ring.used->idx - vq->vq_used_cons_idx; /* [한국어] uint16 wrap-around 안전 차분 — 백엔드와 SPDK 측 인덱스 차이. */
	virtio_rmb(); /* [한국어] used.idx 읽기 → used.ring[] 읽기 사이의 RAR 순서 보장. */

	num = (uint16_t)(spdk_likely(nb_used <= nb_pkts) ? nb_used : nb_pkts); /* [한국어] 가용량과 호출자 버퍼 중 작은 쪽. likely=대개 가용량이 작음. */
	if (spdk_likely(num > DESC_PER_CACHELINE)) { /* [한국어] 충분히 많을 때만 정렬 트림 — 적을 때는 그대로 회수. */
		num = num - ((vq->vq_used_cons_idx + num) % DESC_PER_CACHELINE); /* [한국어] 회수 끝점을 캐시라인 경계에 맞춤 — 다음 폴링 효율. */
	}

	return virtqueue_dequeue_burst_rx(vq, io, len, num); /* [한국어] 실제 회수 위임. */
}

/*
 * [한국어]
 * virtio_dev_acquire_queue - 특정 인덱스의 큐를 호출 스레드에 귀속 (단독 사용 권한 획득)
 *
 * @vdev : 대상 디바이스
 * @index: 큐 인덱스
 * @return: 0 성공, -1(인덱스 초과/이미 점유됨)
 *
 * SPDK virtio는 큐 단위 lockless를 위해 한 큐를 한 SPDK thread에만 부여한다.
 * 본 함수가 vdev->mutex 아래서 owner_thread를 spdk_get_thread()로 set한다.
 * 이미 점유된 큐는 거절하여 호출자(예: bdev_virtio)가 다른 큐를 시도하도록 유도.
 *
 * 실행 컨텍스트: bdev_virtio가 channel 생성 시 호출. SPDK thread context.
 *
 * 호출 체인:
 *   bdev_virtio create_channel → [virtio_dev_acquire_queue]
 */
int
virtio_dev_acquire_queue(struct virtio_dev *vdev, uint16_t index)
{
	struct virtqueue *vq = NULL;  /* [한국어] 대상 큐 포인터. */

	if (index >= vdev->max_queues) {  /* [한국어] 경계 검사 — 잘못된 인덱스. */
		SPDK_ERRLOG("requested vq index %"PRIu16" exceeds max queue count %"PRIu16".\n",
			    index, vdev->max_queues);
		return -1;
	}

	pthread_mutex_lock(&vdev->mutex); /* [한국어] cross-thread 점유 검사를 위해 mutex 진입. */
	vq = vdev->vqs[index];
	if (vq == NULL || vq->owner_thread != NULL) { /* [한국어] 큐 미존재 또는 이미 점유 — 실패. */
		pthread_mutex_unlock(&vdev->mutex);
		return -1;
	}

	vq->owner_thread = spdk_get_thread(); /* [한국어] 호출 스레드를 owner로 등록 — 이후 acquire/release/get_thread는 모두 이 thread 기준. */
	pthread_mutex_unlock(&vdev->mutex);
	return 0;
}

/*
 * [한국어]
 * virtio_dev_find_and_acquire_queue - start_index부터 free 큐를 찾아 점유
 *
 * @vdev       : 대상 디바이스
 * @start_index: 검색 시작 인덱스 (예: fixed_queues_num — 컨트롤 큐는 건너뜀)
 * @return: 점유한 큐 인덱스(>=0), -1(가용 큐 없음)
 *
 * acquire_queue의 자동 검색 버전 — bdev_virtio가 새 channel 만들 때 어떤 큐를 쓸지 모를 때 사용.
 * mutex 아래에서 첫 번째 free 큐를 찾아 owner_thread로 set.
 *
 * 실행 컨텍스트: bdev_virtio create_channel. SPDK thread context.
 *
 * 호출 체인:
 *   bdev_virtio create_channel → [virtio_dev_find_and_acquire_queue]
 */
int32_t
virtio_dev_find_and_acquire_queue(struct virtio_dev *vdev, uint16_t start_index)
{
	struct virtqueue *vq = NULL;  /* [한국어] 발견한 free 큐. */
	uint16_t i;                   /* [한국어] 검색 인덱스. */

	pthread_mutex_lock(&vdev->mutex); /* [한국어] 검색+점유를 원자적으로. */
	for (i = start_index; i < vdev->max_queues; ++i) {
		vq = vdev->vqs[i];
		if (vq != NULL && vq->owner_thread == NULL) { /* [한국어] 존재하고 미점유면 후보. */
			break;
		}
	}

	if (vq == NULL || i == vdev->max_queues) { /* [한국어] 전부 점유 또는 큐 없음. */
		SPDK_ERRLOG("no more unused virtio queues with idx >= %"PRIu16".\n", start_index);
		pthread_mutex_unlock(&vdev->mutex);
		return -1;
	}

	vq->owner_thread = spdk_get_thread(); /* [한국어] 점유. */
	pthread_mutex_unlock(&vdev->mutex);
	return i;  /* [한국어] 점유 성공한 인덱스 반환 — 호출자가 채널 컨텍스트에 저장. */
}

/*
 * [한국어]
 * virtio_dev_queue_get_thread - 큐의 현재 owner thread 조회
 *
 * @vdev : 대상 디바이스
 * @index: 큐 인덱스
 * @return: spdk_thread* (NULL이면 미점유)
 *
 * 인덱스 경계 위반은 abort() — 복구 불가능 버그로 간주.
 *
 * 실행 컨텍스트: 모든 SPDK thread 가능. mutex로 보호.
 *
 * 호출 체인:
 *   virtio_dev_queue_is_acquired / 진단 코드 → [virtio_dev_queue_get_thread]
 */
struct spdk_thread *
virtio_dev_queue_get_thread(struct virtio_dev *vdev, uint16_t index)
{
	struct spdk_thread *thread = NULL;  /* [한국어] 반환 thread. */

	if (index >= vdev->max_queues) {
		SPDK_ERRLOG("given vq index %"PRIu16" exceeds max queue count %"PRIu16"\n",
			    index, vdev->max_queues);
		abort(); /* This is not recoverable */
		/* [한국어] 인덱스 검증은 호출자 책임 — 위반 시 즉시 종료(silent corruption 방지). */
	}

	pthread_mutex_lock(&vdev->mutex); /* [한국어] owner_thread 읽기 직렬화. */
	thread = vdev->vqs[index]->owner_thread;
	pthread_mutex_unlock(&vdev->mutex);

	return thread;
}

/*
 * [한국어]
 * virtio_dev_queue_is_acquired - 큐가 어떤 thread에 점유되었는지 boolean 조회
 *
 * @vdev : 대상 디바이스
 * @index: 큐 인덱스
 * @return: true=점유됨, false=free
 *
 * 단순 wrapper — get_thread != NULL.
 *
 * 호출 체인:
 *   bdev_virtio (큐 가용성 검사) → [virtio_dev_queue_is_acquired] → virtio_dev_queue_get_thread
 */
bool
virtio_dev_queue_is_acquired(struct virtio_dev *vdev, uint16_t index)
{
	return virtio_dev_queue_get_thread(vdev, index) != NULL; /* [한국어] thread != NULL이면 점유. */
}

/*
 * [한국어]
 * virtio_dev_release_queue - 점유한 큐 반환 (owner_thread = NULL)
 *
 * @vdev : 대상 디바이스
 * @index: 큐 인덱스
 * @return: 없음
 *
 * 호출자는 자신이 점유한 큐를 반환해야 한다 (assert로 검증).
 * 인덱스 경계 위반은 에러 로그 후 no-op (반환은 best-effort).
 *
 * 실행 컨텍스트: 큐의 owner_thread (assert로 강제).
 *
 * 호출 체인:
 *   bdev_virtio destroy_channel → [virtio_dev_release_queue]
 */
void
virtio_dev_release_queue(struct virtio_dev *vdev, uint16_t index)
{
	struct virtqueue *vq = NULL;  /* [한국어] 대상 큐. */

	if (index >= vdev->max_queues) {
		SPDK_ERRLOG("given vq index %"PRIu16" exceeds max queue count %"PRIu16".\n",
			    index, vdev->max_queues);
		return;  /* [한국어] release는 abort 대신 로그+return — destroy 경로의 안정성 우선. */
	}

	pthread_mutex_lock(&vdev->mutex);
	vq = vdev->vqs[index];
	if (vq == NULL) {  /* [한국어] 이미 해제된 큐. */
		SPDK_ERRLOG("virtqueue at index %"PRIu16" is not initialized.\n", index);
		pthread_mutex_unlock(&vdev->mutex);
		return;
	}

	assert(vq->owner_thread == spdk_get_thread()); /* [한국어] 호출 스레드가 점유자여야 함 — 다른 스레드의 release 금지. */
	vq->owner_thread = NULL;  /* [한국어] 반환. */
	pthread_mutex_unlock(&vdev->mutex);
}

/*
 * [한국어]
 * virtio_dev_read_dev_config - 디바이스 사설 config 영역 read (트랜스포트 위임)
 *
 * @dev   : 대상 디바이스
 * @offset: dev_cfg 시작으로부터 오프셋 (바이트)
 * @dst   : [out] 읽어올 버퍼
 * @length: 바이트 수
 * @return: 트랜스포트별 코드
 *
 * 디바이스별 PCI common cfg 외의 device-specific 영역 (예: virtio-blk의 capacity, virtio-scsi의
 * num_queues 등). modern_read_dev_config는 generation 카운터로 일관성 검증한다.
 *
 * 호출 체인:
 *   bdev_virtio (디바이스 query) → [virtio_dev_read_dev_config] → backend_ops->read_dev_cfg
 */
int
virtio_dev_read_dev_config(struct virtio_dev *dev, size_t offset,
			   void *dst, int length)
{
	return virtio_dev_backend_ops(dev)->read_dev_cfg(dev, offset, dst, length); /* [한국어] PCI/vfio-user/vhost-user별 구현으로 위임. */
}

/*
 * [한국어]
 * virtio_dev_write_dev_config - 디바이스 사설 config 영역 write (트랜스포트 위임)
 *
 * @dev   : 대상 디바이스
 * @offset: dev_cfg 시작 오프셋
 * @src   : 쓸 데이터
 * @length: 바이트 수
 * @return: 트랜스포트별 코드
 *
 * 호출 체인:
 *   bdev_virtio (디바이스 설정) → [virtio_dev_write_dev_config] → backend_ops->write_dev_cfg
 */
int
virtio_dev_write_dev_config(struct virtio_dev *dev, size_t offset,
			    const void *src, int length)
{
	return virtio_dev_backend_ops(dev)->write_dev_cfg(dev, offset, src, length); /* [한국어] 트랜스포트별 위임. */
}

/*
 * [한국어]
 * virtio_dev_stop - 디바이스 리셋 + 큐 메모리 해제
 *
 * @dev: 대상 디바이스
 * @return: 없음
 *
 * 동작:
 *   1) status를 VIRTIO_CONFIG_S_RESET(0)로 set — 디바이스가 모든 처리 중단 후 초기 상태로 돌아감
 *   2) get_status read-back — 디바이스가 RESET 인식했음을 확인하는 flush 효과
 *   3) virtio_free_queues로 vring 메모리 회수
 * 이후 virtio_dev_reset/start로 재시작 가능 또는 destruct로 종료.
 *
 * 실행 컨텍스트: detach/reset 경로의 단일 스레드. 모든 큐의 polling은 중단되어 있어야 함.
 *
 * 호출 체인:
 *   virtio_dev_reset / virtio_dev_destruct(직접 호출 X, 호출자 책임) → [virtio_dev_stop]
 */
void
virtio_dev_stop(struct virtio_dev *dev)
{
	virtio_dev_backend_ops(dev)->set_status(dev, VIRTIO_CONFIG_S_RESET); /* [한국어] status=0 — virtio 스펙: RESET. */
	/* flush status write */
	virtio_dev_backend_ops(dev)->get_status(dev); /* [한국어] read-back으로 write 반영 보장 (특히 PCI MMIO에서 posted write 강제 flush). */
	virtio_free_queues(dev);  /* [한국어] 모든 vring 메모리 회수. */
}

/*
 * [한국어]
 * virtio_dev_set_status - status 비트 추가 set (기존 비트 OR)
 *
 * @dev   : 대상 디바이스
 * @status: 추가할 비트 마스크 (또는 RESET이면 단독 0 set)
 * @return: 없음
 *
 * virtio status 레지스터는 ACK→DRIVER→FEATURES_OK→DRIVER_OK 순서로 비트가 누적된다.
 * 본 함수는 RESET 외의 경우 기존 status를 read하고 OR한 뒤 write — 누적 set을 보장.
 * RESET의 경우 단독 0 write로 모든 비트 클리어.
 *
 * 호출 체인:
 *   virtio_dev_reset / virtio_dev_start / virtio_negotiate_features → [virtio_dev_set_status] → backend_ops->set_status
 */
void
virtio_dev_set_status(struct virtio_dev *dev, uint8_t status)
{
	if (status != VIRTIO_CONFIG_S_RESET) {  /* [한국어] RESET이면 누적 X — 모든 비트 클리어 의도. */
		status |= virtio_dev_backend_ops(dev)->get_status(dev); /* [한국어] 기존 비트와 OR — 단일 비트 추가 시맨틱. */
	}

	virtio_dev_backend_ops(dev)->set_status(dev, status); /* [한국어] 트랜스포트로 실제 write. */
}

/*
 * [한국어]
 * virtio_dev_get_status - 현재 status 비트 read
 *
 * @dev: 대상 디바이스
 * @return: status 비트 (ACK | DRIVER | FEATURES_OK | DRIVER_OK | NEEDS_RESET | FAILED 조합)
 *
 * 호출 체인:
 *   virtio_dev_set_status / virtio_dev_reset / virtio_negotiate_features → [virtio_dev_get_status] → backend_ops->get_status
 */
uint8_t
virtio_dev_get_status(struct virtio_dev *dev)
{
	return virtio_dev_backend_ops(dev)->get_status(dev); /* [한국어] 트랜스포트별 read 위임. */
}

/*
 * [한국어]
 * virtio_dev_backend_ops - 트랜스포트 ops 테이블 접근자
 *
 * @dev: 대상 디바이스
 * @return: virtio_dev_ops 포인터 (NULL 불가, construct 시 set)
 *
 * 모든 트랜스포트 호출의 단일 진입점 — 디버깅 시 hookable.
 */
const struct virtio_dev_ops *
virtio_dev_backend_ops(struct virtio_dev *dev)
{
	return dev->backend_ops; /* [한국어] construct에서 바인딩한 ops 반환. */
}

/*
 * [한국어]
 * virtio_dev_dump_json_info - 디바이스 상태를 JSON으로 덤프 (RPC 응답)
 *
 * @hw: 대상 디바이스 (실제 구조는 virtio_dev*)
 * @w : SPDK JSON writer 컨텍스트
 *
 * SPDK RPC (예: bdev_virtio_get_devices)에서 호출되어 디바이스 정보를 반환.
 * 공통 정보(vq_count, vq_size) 후 트랜스포트별 dump_json_info(예: PCI 주소)로 위임.
 *
 * 실행 컨텍스트: SPDK RPC handler thread (보통 main thread).
 *
 * 호출 체인:
 *   spdk_rpc handler (bdev_virtio_*) → [virtio_dev_dump_json_info] → backend_ops->dump_json_info
 */
void
virtio_dev_dump_json_info(struct virtio_dev *hw, struct spdk_json_write_ctx *w)
{
	spdk_json_write_named_object_begin(w, "virtio"); /* [한국어] "virtio" 키의 객체 시작. */

	spdk_json_write_named_uint32(w, "vq_count", hw->max_queues); /* [한국어] 큐 개수 노출. */

	spdk_json_write_named_uint32(w, "vq_size",
				     virtio_dev_backend_ops(hw)->get_queue_size(hw, 0)); /* [한국어] 0번 큐 크기 (대표값). */

	virtio_dev_backend_ops(hw)->dump_json_info(hw, w); /* [한국어] 트랜스포트별 정보(예: PCI BDF) 추가. */

	spdk_json_write_object_end(w); /* [한국어] "virtio" 객체 종료. */
}

/* [한국어] SPDK 로그 컴포넌트 등록 — `--logflag virtio_dev`로 SPDK_DEBUGLOG 활성화 가능. */
SPDK_LOG_REGISTER_COMPONENT(virtio_dev)
