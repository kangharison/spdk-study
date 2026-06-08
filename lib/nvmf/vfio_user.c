/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2019-2022, Nutanix Inc. All rights reserved.
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe over vfio-user transport
 */

/*
 * [한국어 설명] SPDK NVMe-oF vfio-user Target Transport (vfio_user.c)
 *
 * === 파일의 역할 ===
 * SPDK NVMe-oF target 의 **vfio-user transport 구현**. RDMA/TCP 와 달리 네트워크 전송
 * 이 아닌 **동일 호스트 안의 VM (또는 다른 프로세스) 에 가상 NVMe PCIe 디바이스를 노출**
 * 한다. 호스트는 libvfio-user 가 만든 Unix socket 으로 VM 의 vfio 요청 (BAR access,
 * IRQ trigger, DMA mapping) 을 받아 SPDK bdev 으로 dispatch.
 *
 * 사용 사례:
 *   - QEMU + libvfio-user: VM 안의 NVMe 디바이스가 실제로는 SPDK bdev 으로 routing.
 *   - kata-containers / lightweight VM 에 storage backend 제공.
 *   - hardware emulation 없이 software 로 NVMe spec 준수.
 *
 * === 전체 아키텍처에서의 위치 ===
 *  [VM Guest OS] → /dev/nvme0 (가상)
 *    → vfio-pci kernel driver (guest)
 *    → vfio-user UNIX socket (host)
 *    → libvfio-user (host process)
 *    → [본 transport] BAR read/write / IRQ / DMA map handlers
 *    → NVMe spec 처리 (CC/CSTS/SQE/CQE) — Linux NVMe 드라이버를 software 로 emulate
 *    → spdk_nvmf_request_exec
 *    → bdev layer
 *    → SPDK bdev module (실제 storage)
 *
 * 핵심 차이 (vs RDMA/TCP):
 *   - capsule 전송 안 함 — 호스트가 NVMe BAR 를 직접 emulate (vfu_pci_init + BAR handler).
 *   - SQ/CQ 가 host shared memory (DMA mapped) — VM 이 SQE write 후 doorbell write 시
 *     vfio-user 이벤트로 호스트에게 통지.
 *   - shadow doorbell + eventidx 패턴으로 doorbell trap 최소화 (NVMe 1.3+ optimization).
 *   - "PCIe device emulation" 이므로 모든 NVMe register (CC, CSTS, AQA, ASQ, ACQ) 를
 *     소프트웨어로 처리.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: vfio-user/libvfio-user.h (외부 라이브러리 — libvfio-user), nvmf_internal.h,
 *   transport.h (NVMe-oF transport vtable 인터페이스).
 * - 의존하는 모듈: lib/nvmf/transport.c (transport list registration).
 * - 데이터 흐름: VM SQE write → vfu_run_ctx polling → BAR write handler 가 doorbell
 *   감지 → SPDK 가 host shared SQ 에서 SQE fetch → bdev exec → CQE 를 host shared CQ
 *   write → IRQ trigger.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct nvmf_vfio_user_req     : 1개 NVMe request (SQE) 의 SPDK 측 컨테이너.
 * - struct nvmf_vfio_user_sq      : Submission Queue (admin/IO 모두 동일 구조체).
 * - struct nvmf_vfio_user_cq      : Completion Queue.
 * - struct nvmf_vfio_user_ctrlr   : 1개 VM 의 NVMe controller emulation 컨텍스트.
 * - struct nvmf_vfio_user_endpoint: vfu_ctx + 소켓 + ctrlr 묶음 (1 endpoint = 1 VM).
 * - struct nvmf_vfio_user_transport : 전체 transport (모든 endpoints).
 * - vfio_user_handle_bar/cc/sq/cq_*: PCIe register emulation 핸들러.
 * - shadow doorbell + eventidx: VM exit 최소화 최적화.
 *
 * === Shadow Doorbell + eventidx 패턴 ===
 *  매 SQE doorbell write 마다 VM exit 발생하면 IOPS 한계. NVMe 1.3 이 도입한 Shadow
 *  Doorbell Buffer 를 host shared memory 에 두고, VM 이 write 시 eventidx 비교로 실제
 *  통지가 필요한지 결정 → 대부분 통지 skip (batching). NVMF_VFIO_USER_EVENTIDX_POLL
 *  값으로 SPDK polling 모드 진입.
 *
 * === 핵심 상수 ===
 *  - NVMF_VFIO_USER_DEFAULT_MAX_QUEUE_DEPTH = 256
 *  - NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR = 512 (NVMe spec 5.21.1.7 한도)
 *  - NVME_DOORBELLS_OFFSET = 0x1000 (BAR0 의 doorbell 시작)
 *  - BAR0 size = 0x1000 + 2 * MAX_QPAIRS * DOORBELL_SIZE (page align)
 *  - BAR4 size = MSI-X table (NVMF_VFIO_USER_MSIX_NUM × 16, page align)
 *  - BAR5 size = MSI-X PBA (PBA bit per vector, page align)
 */

#include <sys/param.h>                              /* [한국어] BSD 호환 매크로 (MIN/MAX 등) - vfio-user가 의존 */

#include <vfio-user/libvfio-user.h>                 /* [한국어] libvfio-user 메인 API - vfu_ctx, vfu_pci_init, BAR handler 등록 */
#include <vfio-user/pci_defs.h>                     /* [한국어] PCI config space 상수 (PCI_VENDOR_ID 등) - NVMe PCIe device emulation */

#include "spdk/barrier.h"                           /* [한국어] spdk_mb/wmb/rmb - shadow doorbell read/write 순서 보장 */
#include "spdk/stdinc.h"                            /* [한국어] 표준 라이브러리 묶음 */
#include "spdk/assert.h"                            /* [한국어] SPDK_STATIC_ASSERT - BAR 크기/큐 한도 컴파일 타임 검사 */
#include "spdk/thread.h"                            /* [한국어] spdk_thread/poller - vfu_run_ctx 폴링용 */
#include "spdk/nvmf_transport.h"                    /* [한국어] spdk_nvmf_transport_ops - 본 파일이 채워 등록할 vtable */
#include "spdk/sock.h"                              /* [한국어] (libvfio-user가 Unix socket 사용 - 직접 호출하지 않으나 의존성) */
#include "spdk/string.h"                            /* [한국어] spdk_strerror 등 */
#include "spdk/util.h"                              /* [한국어] SPDK_COUNTOF, SPDK_ALIGN_CEIL 등 */
#include "spdk/log.h"                               /* [한국어] SPDK_ERRLOG/DEBUGLOG */

#include "transport.h"                              /* [한국어] nvmf_transport_* dispatch thunk */

#include "nvmf_internal.h"                          /* [한국어] NVMe-oF 코어 내부 타입 */

/* [한국어] SWAP(x,y) - typeof를 사용한 GCC 확장 매크로로 두 변수 값 교환.
 * BAR write handler가 큐 포인터 교체 등에서 사용. typeof로 임시 변수 자동 추론. */
#define SWAP(x, y)                  \
	do                          \
	{                           \
		typeof(x) _tmp = x; \
		x = y;              \
		y = _tmp;           \
	} while (0)

#define NVMF_VFIO_USER_DEFAULT_MAX_QUEUE_DEPTH 256  /* [한국어] I/O 큐 기본 깊이 (entries) - NVMe spec 권장 1024 미만 */
#define NVMF_VFIO_USER_DEFAULT_AQ_DEPTH 32          /* [한국어] Admin 큐 깊이 - admin은 적은 명령으로 충분 */
#define NVMF_VFIO_USER_DEFAULT_MAX_IO_SIZE ((NVMF_REQ_MAX_BUFFERS - 1) << SHIFT_4KB) /* [한국어] 단일 NVMe 명령 최대 데이터 크기 - (버퍼 수 - PRP2용 1) * 4KB */
#define NVMF_VFIO_USER_DEFAULT_IO_UNIT_SIZE NVMF_VFIO_USER_DEFAULT_MAX_IO_SIZE /* [한국어] bdev IO unit 기본 크기 - vfio-user는 분할 불필요하므로 max와 동일 */

#define NVME_DOORBELLS_OFFSET	0x1000              /* [한국어] NVMe BAR0 내 SQ/CQ doorbell 시작 오프셋 - NVMe spec 5.3 (Submission/Completion Queue Tail/Head Doorbell) */
#define NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT 2 /* [한국어] shadow doorbell + eventidx 두 버퍼 (NVMe 1.3 Doorbell Buffer Config) */
#define NVMF_VFIO_USER_SET_EVENTIDX_MAX_ATTEMPTS 3  /* [한국어] eventidx 업데이트 재시도 한도 - race 발생 시 3번까지 재시도 */
#define NVMF_VFIO_USER_EVENTIDX_POLL UINT32_MAX     /* [한국어] eventidx 값이 이 sentinel이면 SPDK가 polling mode로 진입 - 통지 없이 폴링 */

#define NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR 512     /* [한국어] 컨트롤러당 최대 qpair 수 - NVMe spec 5.21.1.7 (Identify Controller, MAXNQ) 한도 */
#define NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR (NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR / 4) /* [한국어] 기본값 128 - 메모리 절약 */

/* NVMe spec 1.4, section 5.21.1.7 */
SPDK_STATIC_ASSERT(NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR >= 2 &&
		   NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR <= SPDK_NVME_MAX_IO_QUEUES,
		   "bad number of queues");

/*
 * NVMe driver reads 4096 bytes, which is the extended PCI configuration space
 * available on PCI-X 2.0 and PCI Express buses
 */
#define NVME_REG_CFG_SIZE       0x1000

/*
 * Doorbells must be page aligned so that they can memory mapped.
 *
 * TODO does the NVMe spec also require this? Document it.
 */
#define NVMF_VFIO_USER_DOORBELLS_SIZE \
	SPDK_ALIGN_CEIL( \
		(NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR * 2 * SPDK_NVME_DOORBELL_REGISTER_SIZE), \
		0x1000)
#define NVME_REG_BAR0_SIZE (NVME_DOORBELLS_OFFSET + NVMF_VFIO_USER_DOORBELLS_SIZE)

/*
 * TODO check the PCI spec whether BAR4 and BAR5 really have to be at least one
 * page and a multiple of page size (maybe QEMU also needs this?). Document all
 * this.
 */

#define NVMF_VFIO_USER_MSIX_NUM MAX(CHAR_BIT, NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR)

#define NVMF_VFIO_USER_MSIX_TABLE_BIR (4)
#define NVMF_VFIO_USER_BAR4_SIZE SPDK_ALIGN_CEIL((NVMF_VFIO_USER_MSIX_NUM * 16), 0x1000)
SPDK_STATIC_ASSERT(NVMF_VFIO_USER_BAR4_SIZE > 0, "Incorrect size");

/*
 * TODO according to the PCI spec we need one bit per vector, document the
 * relevant section.
 */
#define NVMF_VFIO_USER_MSIX_PBA_BIR (5)
#define NVMF_VFIO_USER_BAR5_SIZE SPDK_ALIGN_CEIL((NVMF_VFIO_USER_MSIX_NUM / CHAR_BIT), 0x1000)
SPDK_STATIC_ASSERT(NVMF_VFIO_USER_BAR5_SIZE > 0, "Incorrect size");
struct nvmf_vfio_user_req;

typedef int (*nvmf_vfio_user_req_cb_fn)(struct nvmf_vfio_user_req *req, void *cb_arg);

/* 1 more for PRP2 list itself */
#define NVMF_VFIO_USER_MAX_IOVECS	(NVMF_REQ_MAX_BUFFERS + 1)

/*
 * [한국어] enum nvmf_vfio_user_req_state - request 의 단순 2-state.
 *
 * RDMA 의 16-state 와 달리 vfio-user 는 capsule 전송 없이 host shared memory 만 사용 →
 * state machine 단순. FREE = 풀에 있음, EXECUTING = bdev I/O in-flight.
 * 데이터 이동은 DMA mapped iov 로 직접 (별도 RDMA READ/WRITE 없음).
 */
enum nvmf_vfio_user_req_state {
	VFIO_USER_REQUEST_STATE_FREE = 0,
	VFIO_USER_REQUEST_STATE_EXECUTING,
};

/*
 * [한국어] struct nvmf_vfio_user_req - 1개 NVMe request (SQE) 의 SPDK 측 컨테이너.
 *
 * RDMA 의 spdk_nvmf_rdma_request 와 같은 역할이지만 RDMA WR 대신 dma_sg_t (libvfio-user
 * 의 DMA scatter-gather 구조) 사용. iov 는 VM 의 host shared memory 를 직접 가리킴
 * (vfu_addr_to_sgl 로 변환).
 *
 * 필드:
 *  - req: 공통 NVMe-oF request.
 *  - rsp/cmd: NVMe completion/command 사본 (request 내부 보관 — VM 메모리 disturbance 회피).
 *  - state: FREE/EXECUTING 단순 상태.
 *  - cb_fn/cb_arg: bdev 완료 후 후처리 콜백 (e.g. prop_set_cc 후 CC 변경 처리).
 *  - cc: prop_set_cc 직전 CC 값 (rollback / diff 처리용).
 *  - link: free list 또는 EXECUTING 추적 TAILQ.
 *  - iov[MAX]: bdev 가 사용할 scatter-gather (VM 메모리 매핑).
 *  - iovcnt: 유효 iov 수.
 *  - sg[]: 가변 길이 — MAX_IOVECS 만큼의 dma_sg_t (libvfio-user 의 DMA descriptor).
 *
 * 가변 길이 sg[] 이유: dma_sg_t 크기는 libvfio-user 버전 의존이라 sizeof 컴파일 타임에
 * 안 됨 → calloc 시 sizeof(req) + max_iovecs * dma_sg_size 로 할당.
 */
struct nvmf_vfio_user_req  {
	struct spdk_nvmf_request		req;
	struct spdk_nvme_cpl			rsp;
	struct spdk_nvme_cmd			cmd;

	enum nvmf_vfio_user_req_state		state;
	nvmf_vfio_user_req_cb_fn		cb_fn;
	void					*cb_arg;

	/* old CC before prop_set_cc fabric command */
	union spdk_nvme_cc_register		cc;

	TAILQ_ENTRY(nvmf_vfio_user_req)		link;

	struct iovec				iov[NVMF_VFIO_USER_MAX_IOVECS];
	uint8_t					iovcnt;

	/* NVMF_VFIO_USER_MAX_IOVECS worth of dma_sg_t. */
	uint8_t					sg[];
};

#define MAP_R			(0)
#define MAP_RW			(1 << 0)
#define MAP_INITIALIZE		(1 << 1)
#define MAP_QUIET		(1 << 2)

/*
 * Mapping of an NVMe queue.
 *
 * This holds the information tracking a local process mapping of an NVMe queue
 * shared by the client.
 */
/*
 * [한국어] struct nvme_q_mapping - VM 의 NVMe 큐 (SQ 또는 CQ) 메모리 매핑 정보.
 *
 * VM 이 Create IO SQ/CQ admin 명령으로 큐 메모리의 PRP1 (host 측 IOVA) 를 등록하면,
 * SPDK 가 vfu_addr_to_sgl 로 그 IOVA 를 자기 가상주소로 매핑 → iov 에 저장.
 * sg 는 unmap 시 필요 (vfu_unmap_sg). prp1/len 은 진단 + remap 시 활용.
 */
struct nvme_q_mapping {
	/* iov of local process mapping. */
	struct iovec iov;
	/* Stored sg, needed for unmap. */
	dma_sg_t *sg;
	/* Client PRP of queue. */
	uint64_t prp1;
	/* Total length in bytes. */
	uint64_t len;
};

/*
 * [한국어] enum nvmf_vfio_user_sq_state - Submission Queue 상태.
 *
 *  - UNUSED:   슬롯 미사용 (Create IO SQ 전).
 *  - CREATED:  Create 완료, 아직 connect 안 됨.
 *  - DELETED:  Delete 받았지만 in-flight 있어 회수 대기.
 *  - ACTIVE:   정상 동작 (poller 가 doorbell polling 중).
 *  - INACTIVE: PCIe reset / disable 등으로 일시 비활성 (resume 가능).
 */
enum nvmf_vfio_user_sq_state {
	VFIO_USER_SQ_UNUSED = 0,
	VFIO_USER_SQ_CREATED,
	VFIO_USER_SQ_DELETED,
	VFIO_USER_SQ_ACTIVE,
	VFIO_USER_SQ_INACTIVE
};

/*
 * [한국어] enum nvmf_vfio_user_cq_state - Completion Queue 상태. SQ 와 유사하나 더 단순
 * (CQ 는 ACTIVE/INACTIVE 구분 불필요 — SQ 의 짝이 결정).
 */
enum nvmf_vfio_user_cq_state {
	VFIO_USER_CQ_UNUSED = 0,
	VFIO_USER_CQ_CREATED,
	VFIO_USER_CQ_DELETED,
};

/*
 * [한국어] enum nvmf_vfio_user_ctrlr_state - controller emulation 의 5-state 머신.
 *
 * VM live migration / PCI reset / memory map 변경 같은 "critical section" 을 안전하게
 * 처리하기 위한 quiesce → pause → resume 패턴.
 *
 * 흐름:
 *   CREATING → RUNNING (정상 동작)
 *   RUNNING → PAUSING (libvfio-user 가 quiesce 요청)
 *   PAUSING → PAUSED (NVMf subsystem pause 완료 — 이 상태에서 PCI reset/memory remap 안전)
 *   PAUSED → RESUMING (변경 끝, NVMf resume 요청 보냄)
 *   RESUMING → RUNNING (resume 완료, 정상 복귀)
 *
 * live migration 용도: destination VM 으로 controller 상태 복원 후 resume.
 */
enum nvmf_vfio_user_ctrlr_state {
	VFIO_USER_CTRLR_CREATING = 0,
	VFIO_USER_CTRLR_RUNNING,
	/* Quiesce requested by libvfio-user */
	VFIO_USER_CTRLR_PAUSING,
	/* NVMf subsystem is paused, it's safe to do PCI reset, memory register,
	 * memory unregister, etc in this state.
	 */
	VFIO_USER_CTRLR_PAUSED,
	/*
	 * Implies that the NVMf subsystem is paused. Device will be unquiesced (PCI
	 * reset, memory register and unregister, controller in destination VM has
	 * been restored).  NVMf subsystem resume has been requested.
	 */
	VFIO_USER_CTRLR_RESUMING,
};

/*
 * [한국어] struct nvmf_vfio_user_sq - vfio-user 의 Submission Queue.
 *
 * NVMe SQ 의 host 측 emulation 컨테이너. qid=0 은 admin SQ, qid>=1 은 IO SQ.
 *
 * 핵심 필드:
 *  - qpair: 공통 NVMe-oF qpair (poll_group 에 등록).
 *  - ctrlr: 소속 controller emulation 컨텍스트.
 *  - qid/size: NVMe queue identifier + entry 수 (Create IO SQ 시 결정).
 *  - mapping: VM 의 SQ 메모리 host 측 매핑 (SQE 가 여기 들어옴).
 *  - head: SPDK 가 다음에 읽을 SQE 인덱스 (consumer pointer).
 *  - dbl_tailp: VM 이 write 하는 doorbell — VM 이 SQE 채우고 tail 증가시키면 SPDK 가 비교.
 *  - need_rearm: shadow doorbell eventidx 재설정 필요 (poll → notify 전환).
 *  - cqid: 짝이 되는 CQ id (multiple SQ → 1 CQ 가능).
 *  - post_create_io_sq_completion: Create IO SQ 직후엔 true (응답 CQE 발행), live migration
 *    reconnect 시엔 false (응답 보내지 않음).
 *  - free_reqs: 이 SQ 의 free request 풀.
 *  - link/tailq: poll_group 의 sqs 리스트 + connected SQ 리스트.
 */
struct nvmf_vfio_user_sq {
	struct spdk_nvmf_qpair			qpair;
	struct spdk_nvmf_transport_poll_group	*group;
	struct nvmf_vfio_user_ctrlr		*ctrlr;

	uint32_t				qid;
	/* Number of entries in queue. */
	uint32_t				size;
	struct nvme_q_mapping			mapping;
	enum nvmf_vfio_user_sq_state		sq_state;

	uint32_t				head;
	volatile uint32_t			*dbl_tailp;
	/* [한국어] doorbell tail pointer — VM 이 write, SPDK 가 read. volatile 로 캐시 차단. */

	/* Whether a shadow doorbell eventidx needs setting. */
	bool					need_rearm;
	/* [한국어] shadow doorbell 사용 시 — poll 한 번 끝나면 eventidx 재설정해 다음 doorbell
	 * write 가 통지로 wake up 되게 함. polled-mode/notify-mode 전환 트리거. */

	/* multiple SQs can be mapped to the same CQ */
	uint16_t				cqid;

	/* handle_queue_connect_rsp() can be used both for CREATE IO SQ response
	 * and SQ re-connect response in the destination VM, for the prior case,
	 * we will post a NVMe completion to VM, we will not set this flag when
	 * re-connecting SQs in the destination VM.
	 */
	bool					post_create_io_sq_completion;
	/* Copy of Create IO SQ command, this field is used together with
	 * `post_create_io_sq_completion` flag.
	 */
	struct spdk_nvme_cmd			create_io_sq_cmd;

	struct vfio_user_delete_sq_ctx		*delete_ctx;

	/* Currently unallocated reqs. */
	TAILQ_HEAD(, nvmf_vfio_user_req)	free_reqs;
	/* Poll group entry */
	TAILQ_ENTRY(nvmf_vfio_user_sq)		link;
	/* Connected SQ entry */
	TAILQ_ENTRY(nvmf_vfio_user_sq)		tailq;
};

/*
 * [한국어] struct nvmf_vfio_user_cq - vfio-user Completion Queue.
 *
 *  - cq_ref: 이 CQ 를 가리키는 SQ 수 (multiple SQ → 1 CQ 시 ref counting, Delete IO CQ 전에 0 확인).
 *  - tail: SPDK 가 다음에 write 할 CQE 인덱스 (producer pointer).
 *  - dbl_headp: VM 이 write 하는 CQ head doorbell (CQE 소비 통지).
 *  - phase: NVMe phase bit — 매 wrap 마다 토글 (VM 이 새 CQE vs 이전 wrap CQE 구분).
 *  - iv: Interrupt Vector (MSI-X 인덱스). VM 의 ISR 가 이 vector 에 등록됨.
 *  - ien: Interrupt Enable — CQE write 시 IRQ trigger 할지 결정.
 *  - nr_outstanding: 이 CQ 로 완료될 in-flight IO 수 (Delete CQ 전 drain 판정).
 *  - last_head / last_trigger_irq_tail: IRQ coalescing 결정용 — 직전 IRQ trigger 시점.
 */
struct nvmf_vfio_user_cq {
	struct spdk_nvmf_transport_poll_group	*group;
	int					cq_ref;

	uint32_t				qid;
	/* Number of entries in queue. */
	uint32_t				size;
	struct nvme_q_mapping			mapping;
	enum nvmf_vfio_user_cq_state		cq_state;

	uint32_t				tail;
	volatile uint32_t			*dbl_headp;

	bool					phase;

	uint16_t				iv;
	bool					ien;

	/* Number of outstanding IOs that will complete in this queue. */
	size_t					nr_outstanding;

	uint32_t				last_head;
	uint32_t				last_trigger_irq_tail;
};

struct nvmf_vfio_user_poll_group {
	struct spdk_nvmf_transport_poll_group	group;
	TAILQ_ENTRY(nvmf_vfio_user_poll_group)	link;
	TAILQ_HEAD(, nvmf_vfio_user_sq)		sqs;
	struct spdk_interrupt			*intr;
	int					intr_fd;
	struct {

		/*
		 * ctrlr_intr and ctrlr_kicks will be zero for all other poll
		 * groups. However, they can be zero even for the poll group
		 * the controller belongs are if no vfio-user message has been
		 * received or the controller hasn't been kicked yet.
		 */

		/*
		 * Number of times vfio_user_ctrlr_intr() has run:
		 * vfio-user file descriptor has been ready or explicitly
		 * kicked (see below).
		 */
		uint64_t ctrlr_intr;

		/*
		 * Kicks to the controller by ctrlr_kick().
		 * ctrlr_intr - ctrlr_kicks is the number of times the
		 * vfio-user poll file descriptor has been ready.
		 */
		uint64_t ctrlr_kicks;

		/*
		 * Number of times this poll group was kicked.
		 */
		uint64_t pg_kicks;

		/*
		 * How many times we won the race arming an SQ.
		 */
		uint64_t won;

		/*
		 * How many times we lost the race arming an SQ
		 */
		uint64_t lost;

		/*
		 * How many requests we processed in total each time we lost
		 * the rearm race.
		 */
		uint64_t lost_count;

		/*
		 * Number of attempts we attempted to rearm all the SQs in the
		 * poll group.
		 */
		uint64_t rearms;

		/*
		 * Number of times we had to apply flow control to this SQ.
		 */
		uint64_t cq_full;

		uint64_t pg_process_count;
		uint64_t intr;
		uint64_t polls;
		uint64_t polls_spurious;
		uint64_t poll_reqs;
		uint64_t poll_reqs_squared;
		uint64_t cqh_admin_writes;
		uint64_t cqh_io_writes;
	} stats;

	/* Whether this PG needs kicking to wake up again. */
	bool need_kick;
};

/*
 * [한국어] struct nvmf_vfio_user_shadow_doorbells - NVMe 1.3 Shadow Doorbell Buffer 자원.
 *
 * NVMe spec § 4.10 — VM 이 매 doorbell write 마다 BAR exit 하면 IOPS 한계. shadow
 * doorbell 을 host shared memory 에 두면 VM 은 그곳에만 write → SPDK 가 polling.
 * eventidx 와 비교해 "통지 필요" 한 경우만 실제 BAR doorbell 트리거.
 *
 *  - shadow_doorbells: VM 이 write 하는 영역 (host 메모리, DMA mapped).
 *  - eventidxs: SPDK 가 write 하는 영역 — "내가 마지막 본 인덱스" 표시.
 *    VM 이 shadow_doorbell write 후 eventidxs 와 비교해 통지 필요성 판단.
 *  - sgs/iovs: DMA mapping 정보 (unmap 시 필요).
 */
struct nvmf_vfio_user_shadow_doorbells {
	volatile uint32_t			*shadow_doorbells;
	volatile uint32_t			*eventidxs;
	dma_sg_t				*sgs;
	struct iovec				*iovs;
};

/*
 * [한국어] struct nvmf_vfio_user_ctrlr - 1 VM 의 NVMe controller emulation 컨텍스트.
 *
 * VM 이 vfio-user socket 으로 연결 (CONNECT capsule 도 없이 PCI probe) 시 본 객체 생성.
 * controller 의 NVMe register (CC/CSTS/AQA 등) 상태 + 모든 SQ/CQ + bar0 doorbell + shadow
 * doorbell + interrupt context 보유.
 *
 * 핵심 필드:
 *  - endpoint: 소속 endpoint (1 endpoint = 1 VM = 1 socket file).
 *  - connected_sqs: ACTIVE SQ list (poll_group 에 등록된 것들).
 *  - state: 5-state machine (CREATING → RUNNING ↔ PAUSING → PAUSED → RESUMING).
 *  - thread/vfu_ctx_poller/intr: reactor 스레드 + vfu_run_ctx poller + interrupt source.
 *  - queued_quiesce: PAUSING 중 추가 quiesce 요청 대기 표시.
 *  - reset_shn/disconnect: CC.SHN / disable 처리 플래그.
 *  - cntlid: NVMe Controller Identifier (Identify Controller 응답에 들어감).
 *  - ctrlr: 공통 NVMe-oF controller (lib/nvmf/ctrlr.c).
 *  - sqs[512]/cqs[512]: qid → sq/cq 매핑. NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR = 512.
 *  - bar0_doorbells: BAR0 의 doorbell 영역 (offset 0x1000+).
 *  - sdbl: shadow doorbell buffer 자원 (Set Features 0x7F = Doorbell Buffer Config 후).
 *  - shadow_doorbell_buffer/eventidx_buffer: live migration 시 destination 에 전달할 PRP.
 *  - adaptive_irqs_enabled: VM 측 ISR 의 적응형 batching 기능 활성.
 */
struct nvmf_vfio_user_ctrlr {
	struct nvmf_vfio_user_endpoint		*endpoint;
	struct nvmf_vfio_user_transport		*transport;

	/* Connected SQs list */
	TAILQ_HEAD(, nvmf_vfio_user_sq)		connected_sqs;
	enum nvmf_vfio_user_ctrlr_state		state;

	struct spdk_thread			*thread;
	struct spdk_poller			*vfu_ctx_poller;
	struct spdk_interrupt			*intr;
	int					intr_fd;

	bool					queued_quiesce;

	bool					reset_shn;
	bool					disconnect;

	uint16_t				cntlid;
	struct spdk_nvmf_ctrlr			*ctrlr;

	struct nvmf_vfio_user_sq		*sqs[NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR];
	struct nvmf_vfio_user_cq		*cqs[NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR];

	TAILQ_ENTRY(nvmf_vfio_user_ctrlr)	link;

	volatile uint32_t			*bar0_doorbells;
	struct nvmf_vfio_user_shadow_doorbells	*sdbl;
	/*
	 * Shadow doorbells PRPs to provide during the stop-and-copy state.
	 */
	uint64_t				shadow_doorbell_buffer;
	uint64_t				eventidx_buffer;

	bool					adaptive_irqs_enabled;
};

/*
 * [한국어] struct nvmf_vfio_user_endpoint
 *
 * 1 endpoint = 1 socket file = 1 VM. listen 시점에 socket 생성 + vfu_ctx 초기화 + accept
 * poller 등록. VM 이 connect 하면 ctrlr 객체 생성 + bar0/MSI-X 초기화.
 *
 * 핵심 필드:
 *  - vfu_ctx: libvfio-user context (모든 vfu_* API 의 첫 인자).
 *  - accept_poller/accept_thread: accept 전용 thread (controller 의 reactor 와 별개).
 *  - interrupt_mode: poll vs interrupt mode 선택.
 *  - msix: MSI-X capability pointer (pci_config_space 내).
 *  - pci_config_space: PCI config space 256B (vendor/device ID, BAR address 등).
 *  - devmem_fd: BAR0 memory 의 backing fd (mappable BAR 인 경우).
 *  - bar0/bar0_doorbells: BAR0 의 host 측 가상주소 (VM 이 mmap 한 영역과 같은 물리 페이지).
 *  - trid: transport id (UNIX socket path).
 *  - subsystem: 이 endpoint 가 노출하는 NVMf subsystem.
 *  - ctrlr: 현재 연결된 controller (1 endpoint 당 1 controller 한도 — multi-controller
 *    subsystem 시 향후 확장).
 *  - lock: pthread_mutex (accept thread 와 controller thread 사이 동기화).
 *  - need_async_destroy/need_relisten: controller destroy 후 다음 connect 받을 준비.
 */
struct nvmf_vfio_user_endpoint {
	struct nvmf_vfio_user_transport		*transport;
	vfu_ctx_t				*vfu_ctx;
	struct spdk_poller			*accept_poller;
	struct spdk_thread			*accept_thread;
	bool					interrupt_mode;
	struct msixcap				*msix;
	vfu_pci_config_space_t			*pci_config_space;
	int					devmem_fd;
	int					accept_intr_fd;
	struct spdk_interrupt			*accept_intr;

	volatile uint32_t			*bar0;
	volatile uint32_t			*bar0_doorbells;

	struct spdk_nvme_transport_id		trid;
	struct spdk_nvmf_subsystem		*subsystem;

	/* Controller is associated with an active socket connection,
	 * the lifecycle of the controller is same as the VM.
	 * Currently we only support one active connection, as the NVMe
	 * specification defines, we may support multiple controllers in
	 * future, so that it can support e.g: RESERVATION.
	 */
	struct nvmf_vfio_user_ctrlr		*ctrlr;
	pthread_mutex_t				lock;

	bool					need_async_destroy;
	/* Start the accept poller again after destroying the controller */
	bool					need_relisten;

	TAILQ_ENTRY(nvmf_vfio_user_endpoint)	link;
};

/*
 * [한국어] struct nvmf_vfio_user_transport_opts - transport 설정 5 옵션.
 *
 *  - disable_mappable_bar0: BAR0 mmap 비활성 — 모든 read/write 가 BAR handler 거침 (디버깅용).
 *  - disable_adaptive_irq: IRQ coalescing 휴리스틱 끔 — 매 CQE 마다 IRQ trigger.
 *  - disable_shadow_doorbells: NVMe 1.3 shadow doorbell 안 씀 — 매 doorbell 마다 VM exit.
 *  - disable_compare: NVMe Compare command 미지원으로 advertise.
 *  - enable_intr_mode_sq_spreading: interrupt mode 에서 SQ 를 여러 poll_group 에 분산.
 */
struct nvmf_vfio_user_transport_opts {
	bool					disable_mappable_bar0;
	bool					disable_adaptive_irq;
	bool					disable_shadow_doorbells;
	bool					disable_compare;
	bool					enable_intr_mode_sq_spreading;
};

/* Endpoint in vfio-user is associated with a socket file, which
 * is the representative of a PCI endpoint.
 */
struct nvmf_vfio_user_endpoint {
	struct nvmf_vfio_user_transport		*transport;
	vfu_ctx_t				*vfu_ctx;
	struct spdk_poller			*accept_poller;
	struct spdk_thread			*accept_thread;
	bool					interrupt_mode;
	struct msixcap				*msix;
	vfu_pci_config_space_t			*pci_config_space;
	int					devmem_fd;
	int					accept_intr_fd;
	struct spdk_interrupt			*accept_intr;

	volatile uint32_t			*bar0;
	volatile uint32_t			*bar0_doorbells;

	struct spdk_nvme_transport_id		trid;
	struct spdk_nvmf_subsystem		*subsystem;

	/* Controller is associated with an active socket connection,
	 * the lifecycle of the controller is same as the VM.
	 * Currently we only support one active connection, as the NVMe
	 * specification defines, we may support multiple controllers in
	 * future, so that it can support e.g: RESERVATION.
	 */
	struct nvmf_vfio_user_ctrlr		*ctrlr;
	pthread_mutex_t				lock;

	bool					need_async_destroy;
	/* Start the accept poller again after destroying the controller */
	bool					need_relisten;

	TAILQ_ENTRY(nvmf_vfio_user_endpoint)	link;
};

/*
 * [한국어] struct nvmf_vfio_user_transport
 *
 * 전체 vfio-user transport 인스턴스. nvmf_create_transport(type="VFIOUSER") 시 1개 생성 →
 * 모든 endpoints + poll_groups 보유. RDMA 의 nvmf_rdma_transport 와 같은 역할.
 *
 *  - transport: 공통 NVMe-oF transport (vtable 포함).
 *  - transport_opts: 위의 5 옵션 값.
 *  - intr_mode_supported: vfio-user 라이브러리가 interrupt mode 지원하는지 (capability check).
 *  - endpoints: 모든 endpoints (= 모든 VM connection points).
 *  - poll_groups + next_pg: round-robin poll_group 할당 cursor (새 SQ 에 PG 분배).
 *  - lock/pg_lock: endpoints / poll_groups 리스트 보호.
 */
struct nvmf_vfio_user_transport {
	struct spdk_nvmf_transport		transport;
	struct nvmf_vfio_user_transport_opts    transport_opts;
	bool					intr_mode_supported;
	pthread_mutex_t				lock;
	TAILQ_HEAD(, nvmf_vfio_user_endpoint)	endpoints;

	pthread_mutex_t				pg_lock;
	TAILQ_HEAD(, nvmf_vfio_user_poll_group)	poll_groups;
	struct nvmf_vfio_user_poll_group	*next_pg;
};

/*
 * function prototypes
 */
static void nvmf_vfio_user_req_free(struct spdk_nvmf_request *req);

static struct nvmf_vfio_user_req *get_nvmf_vfio_user_req(struct nvmf_vfio_user_sq *sq);

/*
 * Local process virtual address of a queue.
 */
/*
 * [한국어]
 * q_addr - SQ/CQ 큐 메모리의 호스트 로컬 프로세스 가상주소(VVA) 반환.
 *
 * @mapping: 큐(SQ 또는 CQ)의 DMA 매핑 디스크립터. VM 게스트가 지정한 큐 base
 *   주소(GPA)를 map_one()/vfu_addr_to_sgl()로 호스트 VA 에 매핑한 결과 iov 를 담는다.
 * @return: 큐 엔트리 배열의 시작 호스트 가상주소. 미매핑 시 NULL(iov_base 가 0).
 *
 * 동기/배경: vfio-user 에서 VM 의 큐는 게스트 물리주소(GPA/IOVA) 공간에 있고,
 *   호스트(SPDK)는 이를 vfio-user SGL 변환으로 자신의 VA 공간에 매핑해 둔다. SQE 를
 *   읽거나 CQE 를 쓸 때마다 이 헬퍼로 매핑된 로컬 포인터를 얻는다.
 * 동작: iov.iov_base 한 필드를 그대로 반환하는 1줄 접근자.
 * 실행 컨텍스트: 큐를 소유한 poll group 스레드의 hot-path (SQE/CQE 접근 직전). 락 없음.
 * 호출자: handle_cmd_req, post_completion 등 SQ/CQ 접근 코드.
 * 호출 체인: (SQE/CQE 접근 코드) → [q_addr] → mapping->iov.iov_base
 */
static inline void *
q_addr(struct nvme_q_mapping *mapping)
{
	return mapping->iov.iov_base;  /* [한국어] map_one()이 채운 호스트 VA — 큐 엔트리 배열 시작. */
}

/*
 * [한국어]
 * queue_index - (qid, is_cq) 를 doorbell 배열 내 슬롯 인덱스로 변환.
 *
 * @qid: 큐 ID (0=admin, 1..N=I/O). SQ/CQ 가 같은 qid 를 공유한다.
 * @is_cq: 이 doorbell 이 CQ head 인지(true) SQ tail 인지(false). bool → 0/1 로 사용.
 * @return: BAR0 doorbell 영역(또는 shadow doorbell 버퍼) 내의 uint32 슬롯 인덱스.
 *
 * 동기/배경: NVMe 레지스터 레이아웃에서 doorbell 들은 qid 순으로 SQ tail, CQ head 가
 *   번갈아 배치된다(SQ0TDBL, CQ0HDBL, SQ1TDBL, CQ1HDBL, ...). 즉 슬롯 = qid*2 + is_cq.
 *   stride 가 4바이트라고 가정한 인덱스이며 실제 바이트 오프셋은 별도 stride 곱에서 처리.
 * 동작: qid*2 + is_cq 산술 한 줄.
 * 실행 컨텍스트: doorbell 포인터 계산 hot-path. 락 없음(순수 함수).
 * 호출자: vfio_user_ctrlr_switch_doorbells, copy_doorbells, init_sq/cq 등.
 * 호출 체인: (doorbell 포인터 계산) → [queue_index] → 정수 인덱스
 */
static inline int
queue_index(uint16_t qid, bool is_cq)
{
	return (qid * 2) + is_cq;  /* [한국어] SQ/CQ 교차 배치 — qid 당 2슬롯, is_cq 가 +1 오프셋. */
}

/*
 * [한국어]
 * sq_headp - SQ 의 내부 head 카운터 주소 반환 (호스트가 소비한 SQ 위치).
 *
 * @sq: 대상 submission queue. NULL 불가(assert).
 * @return: sq->head 의 volatile 포인터. head 는 호스트가 다음에 읽을 SQE 인덱스.
 *
 * 동기/배경: NVMe SQ 는 tail(VM 이 doorbell 로 알림) - head(호스트가 소비) 환형 큐.
 *   head 는 SPDK 측이 SQE 를 하나 처리할 때마다 sq_head_advance()로 증가시킨다.
 *   volatile 로 두는 이유: 같은 스레드 내에서도 CQE 의 sqhd 필드로 VM 에 보고되므로
 *   컴파일러가 캐싱·재배열하지 못하게 한다.
 * 동작: &sq->head 반환.
 * 실행 컨텍스트: SQ 를 소유한 poll group 스레드. 단일 소유라 락 불필요.
 * 호출자: sq_head_advance, post_completion(CQE 의 sqhd 채울 때) 등.
 * 호출 체인: (SQ 소비/완료 보고) → [sq_headp] → &sq->head
 */
static inline volatile uint32_t *
sq_headp(struct nvmf_vfio_user_sq *sq)
{
	assert(sq != NULL);   /* [한국어] hot-path 안전성 가드 — 디버그 빌드에서만 활성. */
	return &sq->head;     /* [한국어] 호스트가 소비한 SQ 위치(다음에 읽을 SQE 인덱스). */
}

/*
 * [한국어]
 * sq_dbl_tailp - SQ tail doorbell 슬롯 포인터 반환 (VM 이 기록하는 tail).
 *
 * @sq: 대상 submission queue. NULL 불가(assert).
 * @return: sq->dbl_tailp. BAR0 doorbell 또는 shadow doorbell 버퍼 내 이 SQ 의 tail 슬롯.
 *
 * 동기/배경: VM 이 새 명령을 큐에 넣으면 SQ tail doorbell 에 새 tail 값을 기록한다.
 *   호스트는 이 슬롯을 읽어 tail 변화를 감지(polled-mode) 또는 vfio-user 메시지로
 *   통지받는다. switch_doorbells() 가 BAR0/shadow 중 활성 영역으로 이 포인터를 갱신한다.
 * 동작: sq->dbl_tailp 반환(이미 슬롯을 가리키도록 설정된 캐시 포인터).
 * 실행 컨텍스트: SQ poll group 스레드 hot-path. dbl_tailp 갱신은 doorbell switch 시에만.
 * 호출자: SQ 처리 루프(consume_cmds 등)가 tail 을 읽을 때.
 * 호출 체인: (SQ tail 폴링) → [sq_dbl_tailp] → sq->dbl_tailp
 */
static inline volatile uint32_t *
sq_dbl_tailp(struct nvmf_vfio_user_sq *sq)
{
	assert(sq != NULL);     /* [한국어] NULL 방어 가드. */
	return sq->dbl_tailp;   /* [한국어] switch_doorbells()가 BAR0/shadow 영역으로 설정해 둔 tail 슬롯. */
}

/*
 * [한국어]
 * cq_dbl_headp - CQ head doorbell 슬롯 포인터 반환 (VM 이 기록하는 head).
 *
 * @cq: 대상 completion queue. NULL 불가(assert).
 * @return: cq->dbl_headp. VM 이 CQE 를 소비한 뒤 갱신하는 CQ head doorbell 슬롯.
 *
 * 동기/배경: 호스트가 CQE 를 쓰고 VM 에 인터럽트를 보내면, VM 은 CQE 를 처리한 뒤
 *   CQ head doorbell 에 새 head 를 기록한다. 호스트는 이 값으로 CQ 가 가득 찼는지
 *   (host tail 이 guest head 를 추월하지 않도록) 판단한다.
 * 동작: cq->dbl_headp 반환.
 * 실행 컨텍스트: CQ 를 소유한 poll group 스레드. dbl_headp 도 switch 시에만 갱신.
 * 호출자: CQ full 검사(cq_is_full 등), post_completion.
 * 호출 체인: (CQ 공간 검사) → [cq_dbl_headp] → cq->dbl_headp
 */
static inline volatile uint32_t *
cq_dbl_headp(struct nvmf_vfio_user_cq *cq)
{
	assert(cq != NULL);     /* [한국어] NULL 방어 가드. */
	return cq->dbl_headp;   /* [한국어] VM 이 CQE 소비 후 기록하는 head — 호스트의 CQ full 판단 기준. */
}

/*
 * [한국어]
 * cq_tailp - CQ 의 내부 tail 카운터 주소 반환 (호스트가 다음 CQE 를 쓸 위치).
 *
 * @cq: 대상 completion queue. NULL 불가(assert).
 * @return: &cq->tail. 호스트가 다음 CQE 를 기록할 CQ 인덱스.
 *
 * 동기/배경: 호스트는 완료마다 cq->tail 위치에 CQE 를 쓰고 cq_tail_advance()로
 *   tail 을 전진시킨다. tail 이 size 에 도달하면 0 으로 wrap 하면서 phase bit 를 토글한다
 *   (NVMe spec §4.6 — VM 이 새 CQE 를 phase 변화로 인식). volatile: 같은 스레드에서도
 *   메모리에 즉시 반영되도록.
 * 동작: &cq->tail 반환.
 * 실행 컨텍스트: CQ poll group 스레드. 단일 소유라 락 불필요.
 * 호출자: cq_tail_advance, post_completion(CQE 기록 위치 계산).
 * 호출 체인: (CQE 기록) → [cq_tailp] → &cq->tail
 */
static inline volatile uint32_t *
cq_tailp(struct nvmf_vfio_user_cq *cq)
{
	assert(cq != NULL);   /* [한국어] NULL 방어 가드. */
	return &cq->tail;     /* [한국어] 호스트가 다음 CQE 를 쓸 위치(완료마다 전진). */
}

/*
 * [한국어]
 * sq_head_advance - SQE 하나를 소비한 뒤 SQ head 를 1 전진(환형 wrap 포함).
 *
 * @sq: head 를 전진시킬 submission queue.
 *
 * 동기/배경: 호스트가 SQ 에서 명령 하나를 꺼내 처리할 때마다 head 를 1 올린다.
 *   head 는 CQE 의 sqhd 필드로 VM 에 보고되어 VM 이 SQ 공간 회수를 판단하게 한다.
 *   tail 과 달리 head 는 호스트만 갱신하므로 락 없이 안전(단일 소유 스레드).
 * 동작 단계:
 *   1) 현재 head 가 size 미만인지 검증(assert — 환형 불변식).
 *   2) head++.
 *   3) size 에 도달했으면 0 으로 wrap(환형 큐). spdk_unlikely: wrap 은 드문 경계.
 * 실행 컨텍스트: SQ 를 소유한 poll group 스레드 hot-path. 락 없음.
 * 호출자: handle_cmd_req/consume_cmd 가 SQE 하나를 dispatch 한 직후.
 * 호출 체인: (SQE dispatch) → [sq_head_advance] → sq_headp
 */
static inline void
sq_head_advance(struct nvmf_vfio_user_sq *sq)
{
	assert(sq != NULL);   /* [한국어] NULL 방어 가드. */

	assert(*sq_headp(sq) < sq->size);   /* [한국어] 환형 불변식: head 는 항상 [0,size) 범위. */
	(*sq_headp(sq))++;                  /* [한국어] SQE 하나 소비 → head 1 전진. */

	/* [한국어] head 가 큐 끝에 도달하면 환형 wrap. spdk_unlikely: size 경계는 드묾 → 분기 예측 힌트. */
	if (spdk_unlikely(*sq_headp(sq) == sq->size)) {
		*sq_headp(sq) = 0;   /* [한국어] 환형 큐 시작으로 되감기. SQ 는 phase 개념 없음(CQ 만 phase 사용). */
	}
}

/*
 * [한국어]
 * cq_tail_advance - CQE 하나를 쓴 뒤 CQ tail 을 1 전진(wrap 시 phase 토글).
 *
 * @cq: tail 을 전진시킬 completion queue.
 *
 * 동기/배경: 호스트가 CQE 하나를 기록한 직후 tail 을 올린다. CQ 가 한 바퀴 돌면
 *   (tail==size) 0 으로 wrap 하면서 phase bit 를 반전한다. VM 은 CQE 의 phase 비트가
 *   기대값과 같아질 때 "새 완료"로 인식하므로(NVMe spec §4.6 — round-robin phase),
 *   wrap 마다 phase 를 토글하는 것이 정확성의 핵심이다.
 * 동작 단계:
 *   1) tail < size 환형 불변식 검증.
 *   2) tail++.
 *   3) size 도달 시 tail=0 + phase 반전.
 * 실행 컨텍스트: CQ 를 소유한 poll group 스레드 hot-path. 단일 소유 → 락 없음.
 * 호출자: post_completion 이 CQE 를 쓴 직후.
 * 호출 체인: (CQE 기록) → [cq_tail_advance] → cq_tailp
 */
static inline void
cq_tail_advance(struct nvmf_vfio_user_cq *cq)
{
	assert(cq != NULL);   /* [한국어] NULL 방어 가드. */

	assert(*cq_tailp(cq) < cq->size);   /* [한국어] 환형 불변식: tail 은 항상 [0,size). */
	(*cq_tailp(cq))++;                  /* [한국어] CQE 하나 기록 → tail 1 전진. */

	/* [한국어] tail 이 큐 끝에 도달하면 wrap + phase 토글. spdk_unlikely: 경계는 드묾. */
	if (spdk_unlikely(*cq_tailp(cq) == cq->size)) {
		*cq_tailp(cq) = 0;          /* [한국어] 환형 시작으로 되감기. */
		cq->phase = !cq->phase;     /* [한국어] NVMe spec §4.6 — wrap 마다 phase 반전해 VM 이 새 CQE 를 구분. */
	}
}

/*
 * [한국어]
 * io_q_exists - 주어진 (qid, is_cq) I/O 큐가 현재 살아있는(생성·미삭제) 상태인지 판정.
 *
 * @vu_ctrlr: 대상 vfio-user 컨트롤러. NULL 불가(assert).
 * @qid: 검사할 큐 ID. admin 큐(0)와 한도 초과는 무조건 false.
 * @is_cq: CQ 검사(true) / SQ 검사(false).
 * @return: 해당 큐가 존재하고 DELETED/UNUSED 상태가 아니면 true, 아니면 false.
 *
 * 동기/배경: Create/Delete I/O Queue admin 명령 처리 시, 참조하는 짝 큐가 실제로
 *   존재하는지(예: CQ 를 가리키는 SQ 생성 요청에서 그 CQ 가 있는지) 검증해야 한다.
 *   배열 슬롯이 NULL 이거나 상태가 DELETED/UNUSED 면 "없음"으로 취급한다.
 * 동작 단계:
 *   1) qid==0(admin) 또는 한도(>=MAX_QPAIRS) 면 즉시 false — I/O 큐가 아님.
 *   2) is_cq 면 cqs[qid] 슬롯과 cq_state 검사, 아니면 sqs[qid] 슬롯과 sq_state 검사.
 * 실행 컨텍스트: admin 명령 처리 스레드(컨트롤러 소유 poll group). 락 없음(단일 소유).
 * 호출자: handle_create_io_q, handle_del_io_q, handle_create_io_cq 등 admin 핸들러.
 * 에러 경로: 존재하지 않으면 호출자가 Invalid Queue Identifier 등 상태코드로 응답.
 * 호출 체인: (admin 큐 관리 명령) → [io_q_exists] → cqs/sqs 배열 + 상태 enum
 */
static bool
io_q_exists(struct nvmf_vfio_user_ctrlr *vu_ctrlr, const uint16_t qid, const bool is_cq)
{
	assert(vu_ctrlr != NULL);   /* [한국어] NULL 방어 가드. */

	/* [한국어] qid 0 은 admin 큐(I/O 큐가 아님), 한도 이상은 배열 범위 밖 → 둘 다 "I/O 큐 없음". */
	if (qid == 0 || qid >= NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR) {
		return false;
	}

	if (is_cq) {   /* [한국어] CQ 존재 여부 검사 분기. */
		/* [한국어] CQ 슬롯이 아직 비어 있으면 생성된 적 없음 → false. */
		if (vu_ctrlr->cqs[qid] == NULL) {
			return false;
		}

		/* [한국어] 슬롯은 있으나 상태가 DELETED(삭제됨)/UNUSED(예약만)면 "없음"으로 취급.
		 * 그 외(LIVE 등)면 true → 짝 SQ 생성 등에서 유효한 참조로 인정. */
		return (vu_ctrlr->cqs[qid]->cq_state != VFIO_USER_CQ_DELETED &&
			vu_ctrlr->cqs[qid]->cq_state != VFIO_USER_CQ_UNUSED);
	}

	/* [한국어] SQ 슬롯이 비어 있으면 생성된 적 없음 → false. */
	if (vu_ctrlr->sqs[qid] == NULL) {
		return false;
	}

	/* [한국어] SQ 도 DELETED/UNUSED 가 아니어야 "존재"로 인정. */
	return (vu_ctrlr->sqs[qid]->sq_state != VFIO_USER_SQ_DELETED &&
		vu_ctrlr->sqs[qid]->sq_state != VFIO_USER_SQ_UNUSED);
}

/*
 * [한국어]
 * endpoint_id - endpoint 의 식별 문자열(전송 주소 = 소켓 경로) 반환.
 *
 * @endpoint: 대상 endpoint(= VM 1대의 가상 NVMe 디바이스). NULL 가정 안 함.
 * @return: endpoint->trid.traddr — vfio-user 소켓 경로 문자열(예: /var/run/...).
 *
 * 동기/배경: 로그/디버그 메시지에서 어느 VM 인지 사람이 읽을 수 있게 식별하기 위해
 *   소켓 경로를 ID 로 사용한다. traddr 은 listen 시 trid 에서 복사된 고정 문자열.
 * 동작: trid.traddr 필드 반환(1줄 접근자).
 * 실행 컨텍스트: 어느 스레드든 호출 가능(read-only 고정 문자열). 락 없음.
 * 호출자: ctrlr_id, destroy_endpoint 등 모든 로그 출력부.
 * 호출 체인: (로그 출력) → [endpoint_id] → endpoint->trid.traddr
 */
static char *
endpoint_id(struct nvmf_vfio_user_endpoint *endpoint)
{
	return endpoint->trid.traddr;   /* [한국어] vfio-user 소켓 경로 = 이 VM 디바이스의 사람이 읽는 ID. */
}

/*
 * [한국어]
 * ctrlr_id - 컨트롤러의 식별 문자열 반환(NULL-safe 래퍼).
 *
 * @ctrlr: 대상 vfio-user 컨트롤러. NULL 또는 endpoint 미연결 상태도 허용.
 * @return: 연결된 endpoint 의 ID 문자열, 아직 endpoint 가 없으면 "Null Ctrlr".
 *
 * 동기/배경: 컨트롤러 생성 초기 단계 등 endpoint 가 아직 연결되지 않은 상태에서도
 *   로그 매크로(SPDK_ERRLOG 등)가 ctrlr_id(x) 를 안전하게 부를 수 있도록 NULL 가드를 둔다.
 * 동작 단계: ctrlr 또는 endpoint 가 NULL 이면 상수 문자열, 아니면 endpoint_id 위임.
 * 실행 컨텍스트: 모든 스레드(read-only). 락 없음.
 * 호출자: 파일 전반의 거의 모든 로그 출력부.
 * 호출 체인: (로그 출력) → [ctrlr_id] → endpoint_id
 */
static char *
ctrlr_id(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	/* [한국어] 컨트롤러나 endpoint 가 아직 없으면(생성 초기/실패 경로) 안전한 더미 문자열 반환. */
	if (!ctrlr || !ctrlr->endpoint) {
		return "Null Ctrlr";
	}

	return endpoint_id(ctrlr->endpoint);   /* [한국어] endpoint 가 있으면 그 소켓 경로 ID 사용. */
}

/* Return the poll group for the admin queue of the controller. */
/*
 * [한국어]
 * ctrlr_to_poll_group - 컨트롤러의 admin 큐가 속한 vfio-user poll group 반환.
 *
 * @vu_ctrlr: 대상 컨트롤러. sqs[0](admin SQ)가 유효해야 한다.
 * @return: admin SQ 가 등록된 nvmf_vfio_user_poll_group 포인터.
 *
 * 동기/배경: 컨트롤러를 깨우거나(ctrlr_kick) 인터럽트를 보낼 때, 컨트롤러를 "대표"하는
 *   poll group 이 필요하다. SPDK 에서 admin 큐(qid 0)는 항상 컨트롤러를 생성한 poll
 *   group 에 머무르므로, admin SQ 의 generic group 으로부터 vfio-user poll group 을
 *   container_of(SPDK_CONTAINEROF)로 역참조하면 그것이 컨트롤러의 대표 group 이 된다.
 * 동작: sqs[0]->group(generic spdk_nvmf_transport_poll_group)을 감싼 vfio-user 구조체 복원.
 * 실행 컨텍스트: 주로 컨트롤러 소유 스레드. group 멤버는 생성 후 고정이라 락 불필요.
 * 호출자: ctrlr_kick.
 * 호출 체인: (컨트롤러 kick) → [ctrlr_to_poll_group] → SPDK_CONTAINEROF
 */
static inline struct nvmf_vfio_user_poll_group *
ctrlr_to_poll_group(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	/* [한국어] admin SQ 의 generic group 멤버 주소 → 이를 포함하는 vfio-user poll group 으로 역산.
	 * group 은 vfio-user poll group 의 첫 멤버가 아닐 수 있어 CONTAINEROF 가 필요. */
	return SPDK_CONTAINEROF(vu_ctrlr->sqs[0]->group,
				struct nvmf_vfio_user_poll_group,
				group);
}

/*
 * [한국어]
 * sq_to_poll_group - 임의 SQ 가 속한 vfio-user poll group 반환.
 *
 * @sq: 대상 submission queue. sq->group 이 설정되어 있어야 한다.
 * @return: 이 SQ 가 등록된 nvmf_vfio_user_poll_group 포인터.
 *
 * 동기/배경: SQ 처리·완료·kick 시 그 SQ 가 어느 poll group(따라서 어느 스레드)에
 *   바인딩되었는지 알아야 한다. SQ 의 generic group 멤버에서 vfio-user poll group 으로
 *   역참조한다. ctrlr_to_poll_group 과 달리 admin 전용이 아니라 모든 SQ 에 일반화된 버전.
 * 동작: sq->group 을 감싼 vfio-user poll group 복원.
 * 실행 컨텍스트: SQ 를 소유한 poll group 스레드. group 은 add 후 고정 → 락 없음.
 * 호출자: SQ kick/통계/처리 코드.
 * 호출 체인: (SQ 처리) → [sq_to_poll_group] → SPDK_CONTAINEROF
 */
static inline struct nvmf_vfio_user_poll_group *
sq_to_poll_group(struct nvmf_vfio_user_sq *sq)
{
	/* [한국어] SQ 의 generic group 멤버 → 이를 포함하는 vfio-user poll group 으로 역참조. */
	return SPDK_CONTAINEROF(sq->group, struct nvmf_vfio_user_poll_group,
				group);
}

/*
 * [한국어]
 * poll_group_to_thread - vfio-user poll group 이 실행되는 spdk_thread 반환.
 *
 * @vu_pg: 대상 vfio-user poll group.
 * @return: 이 poll group 이 바인딩된 spdk_thread*(= reactor 코어 1개에 고정된 논리 스레드).
 *
 * 동기/배경: cross-thread 작업(spdk_thread_send_msg)을 보낼 대상 스레드를 알아야 한다.
 *   SPDK poll group 은 생성 시 한 spdk_thread 에 고정(affinity)되며, 그 스레드에서만
 *   해당 group 의 큐를 처리한다(lockless 설계의 근거). 이 헬퍼가 그 스레드를 꺼낸다.
 * 동작: 중첩 group->group->thread 접근(vfio-user group → generic transport group → spdk_thread).
 * 실행 컨텍스트: 모든 스레드(read-only 포인터). 락 없음.
 * 호출자: ctrlr_kick(spdk_thread_send_msg 대상 결정).
 * 호출 체인: (cross-thread kick) → [poll_group_to_thread] → spdk_thread_send_msg
 */
static inline struct spdk_thread *
poll_group_to_thread(struct nvmf_vfio_user_poll_group *vu_pg)
{
	return vu_pg->group.group->thread;   /* [한국어] poll group 이 고정된 reactor 스레드 — 메시지 전달 대상. */
}

/*
 * [한국어]
 * index_to_sg_t - dma_sg_t 가변 크기 배열에서 i 번째 원소 포인터 계산.
 *
 * @arr: dma_sg_t 배열의 시작 주소(calloc 으로 dma_sg_size()*N 만큼 할당).
 * @i: 원하는 원소 인덱스.
 * @return: arr 의 i 번째 dma_sg_t 포인터.
 *
 * 동기/배경: libvfio-user 의 dma_sg_t 는 ABI 상 크기가 런타임에 dma_sg_size()로만
 *   알 수 있는 불투명 타입이라 일반 배열 인덱싱(arr[i])이 불가능하다. 따라서 바이트
 *   단위로 i*dma_sg_size() 만큼 수동 포인터 산술을 해야 한다(컴파일 타임 sizeof 불가).
 * 동작: (uintptr_t)arr + i*dma_sg_size() 를 dma_sg_t* 로 캐스팅.
 * 실행 컨텍스트: shadow doorbell/SGL 매핑 코드. 락 없음(순수 산술).
 * 호출자: map_sdbl, unmap_sdbl 등 다중 SGL 세그먼트를 다루는 코드.
 * 호출 체인: (SGL 배열 접근) → [index_to_sg_t] → 바이트 오프셋 포인터
 */
static dma_sg_t *
index_to_sg_t(void *arr, size_t i)
{
	/* [한국어] dma_sg_t 는 불투명 타입(sizeof 불가) → dma_sg_size() 로 stride 를 얻어 수동 산술. */
	return (dma_sg_t *)((uintptr_t)arr + i * dma_sg_size());
}

/*
 * [한국어]
 * in_interrupt_mode - 현재 transport 가 인터럽트 모드로 동작 중인지 판정.
 *
 * @vu_transport: 대상 vfio-user transport.
 * @return: SPDK 전역 인터럽트 모드가 켜져 있고 이 transport 가 인터럽트를 지원하면 true.
 *
 * 동기/배경: vfio-user 는 기본 polled-mode(무한 폴링)이지만, BAR0 비매핑 설정 시
 *   eventfd/epoll 기반 인터럽트 모드로도 동작할 수 있다. 두 모드는 doorbell 처리,
 *   poller 등록, kick 방식이 달라 분기마다 이 판정이 필요하다. 전역 모드 ON + transport
 *   capability(intr_mode_supported, create 시 disable_mappable_bar0 로 결정) 둘 다 충족해야 함.
 * 동작: 전역 spdk_interrupt_mode_is_enabled() && vu_transport->intr_mode_supported.
 * 실행 컨텍스트: 모든 스레드(read-only 플래그). 락 없음.
 * 호출자: poll group/SQ/doorbell 처리 코드의 모드 분기 다수.
 * 호출 체인: (모드 분기) → [in_interrupt_mode] → 전역 플래그 + transport 플래그
 */
static inline bool
in_interrupt_mode(struct nvmf_vfio_user_transport *vu_transport)
{
	/* [한국어] 전역 인터럽트 모드 ON 그리고 이 transport 가 인터럽트 지원(=BAR0 비매핑)일 때만 true. */
	return spdk_interrupt_mode_is_enabled() &&
	       vu_transport->intr_mode_supported;
}

static int vfio_user_ctrlr_intr(void *ctx);   /* [한국어] 전방 선언 — 아래 _intr_msg 가 먼저 참조하므로 필요. */

/*
 * [한국어]
 * vfio_user_ctrlr_intr_msg - spdk_thread_send_msg 로 cross-thread 전달되는 인터럽트 래퍼.
 *
 * @ctx: vfio_user_ctrlr_intr 에 그대로 넘길 컨텍스트(컨트롤러 포인터).
 *
 * 동기/배경: 다른 코어/스레드가 컨트롤러를 깨워야 할 때 직접 vfio_user_ctrlr_intr 를
 *   부를 수 없다(그 함수는 컨트롤러 소유 스레드에서만 안전 — lockless 단일 소유 모델).
 *   대신 spdk_thread_send_msg 로 이 래퍼를 대상 스레드에 큐잉하면, 대상 스레드의 메시지
 *   처리 루프가 자기 컨텍스트에서 vfio_user_ctrlr_intr 를 호출하게 된다.
 * 동작: vfio_user_ctrlr_intr(ctx) 단순 위임(반환값 무시 — 메시지 콜백 시그니처는 void).
 * 실행 컨텍스트: 대상 poll group 스레드의 메시지 처리 루프(send_msg 디스패치 시점).
 * 호출자: ctrlr_kick 이 spdk_thread_send_msg 의 콜백으로 등록.
 * 호출 체인: spdk_thread_send_msg → [vfio_user_ctrlr_intr_msg] → vfio_user_ctrlr_intr
 */
static void
vfio_user_ctrlr_intr_msg(void *ctx)
{
	vfio_user_ctrlr_intr(ctx);   /* [한국어] 대상 스레드 컨텍스트에서 실제 인터럽트 처리 위임. 반환값은 메시지 콜백상 무시. */
}

/*
 * Kick (force a wakeup) of all poll groups for this controller.
 * vfio_user_ctrlr_intr() itself arranges for kicking other poll groups if
 * needed.
 */
/*
 * [한국어]
 * ctrlr_kick - 컨트롤러의 대표(admin) poll group 스레드를 강제로 깨운다.
 *
 * @vu_ctrlr: 깨울 대상 컨트롤러.
 *
 * 동기/배경: 인터럽트 모드에서 poll group 은 평소 epoll 에서 잠들어 있다. 상태 변화
 *   (새 doorbell, 마이그레이션, 큐 생성 등)를 즉시 반영하려면 그 group 을 깨워야 한다.
 *   현재 스레드와 대상 group 스레드가 다를 수 있으므로 직접 호출 대신 send_msg 로 전달한다
 *   (cross-thread 안전성 — 단일 소유 모델 유지). 깨어난 vfio_user_ctrlr_intr 가 필요하면
 *   다른 poll group 들도 연쇄적으로 깨운다.
 * 동작 단계:
 *   1) 디버그 로그 + ctrlr_to_poll_group 으로 대표 group 획득.
 *   2) 통계(ctrlr_kicks) 증가.
 *   3) spdk_thread_send_msg 로 그 group 의 스레드에 _intr_msg 큐잉.
 * 실행 컨텍스트: 임의 스레드에서 호출 가능. 실제 처리는 대상 group 스레드로 위임.
 * 호출자: doorbell/마이그레이션/큐 변경 등 컨트롤러 상태 변경 코드.
 * 호출 체인: (상태 변경) → [ctrlr_kick] → spdk_thread_send_msg → vfio_user_ctrlr_intr_msg
 */
static void
ctrlr_kick(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	struct nvmf_vfio_user_poll_group *vu_ctrlr_group;   /* [한국어] 컨트롤러의 대표(admin) poll group. */

	SPDK_DEBUGLOG(vfio_user_db, "%s: kicked\n", ctrlr_id(vu_ctrlr));   /* [한국어] kick 발생 추적 로그. */

	vu_ctrlr_group = ctrlr_to_poll_group(vu_ctrlr);   /* [한국어] admin SQ 기준으로 대표 group 찾기. */

	vu_ctrlr_group->stats.ctrlr_kicks++;   /* [한국어] 컨트롤러 kick 횟수 통계(성능 분석/관찰용). */

	/* [한국어] 대표 group 의 스레드로 인터럽트 처리를 cross-thread 전달. 직접 호출하지 않는 이유:
	 * vfio_user_ctrlr_intr 는 그 group 스레드 컨텍스트에서만 lockless 안전하기 때문. */
	spdk_thread_send_msg(poll_group_to_thread(vu_ctrlr_group),
			     vfio_user_ctrlr_intr_msg, vu_ctrlr);
}

/*
 * Force a wake-up for this particular poll group and its contained SQs.
 */
/*
 * [한국어]
 * poll_group_kick - 특정 poll group(과 그 안의 SQ들)을 eventfd 로 즉시 깨운다.
 *
 * @vu_group: 깨울 대상 poll group. 호출 전에 need_kick 이 true 여야 한다(assert).
 *
 * 동기/배경: 인터럽트 모드에서 poll group 스레드는 자신의 epoll set 에 등록된 intr_fd
 *   (eventfd)에서 블록한다. 다른 코드가 이 group 에 처리할 일이 생겼다고 표시(need_kick)
 *   했을 때, eventfd 에 8바이트를 써서 epoll 을 깨운다. ctrlr_kick 과 달리 send_msg 가
 *   아니라 직접 eventfd_write 인 이유: 같은 스레드/직접 fd 깨우기 경로이기 때문.
 * 동작 단계:
 *   1) 통계 증가, need_kick 불변식 검증.
 *   2) need_kick=false 로 리셋(중복 kick 방지).
 *   3) eventfd_write(intr_fd, 1) — epoll 깨우기.
 * 실행 컨텍스트: 해당 group 의 처리/통지 경로. eventfd 는 thread-safe.
 * 호출자: vfio_user_ctrlr_intr 등 group 단위로 처리를 트리거하는 코드.
 * 호출 체인: (group 처리 트리거) → [poll_group_kick] → eventfd_write
 */
static void
poll_group_kick(struct nvmf_vfio_user_poll_group *vu_group)
{
	vu_group->stats.pg_kicks++;          /* [한국어] poll group kick 횟수 통계. */
	assert(vu_group->need_kick);         /* [한국어] 불변식: kick 요청 표시가 켜져 있을 때만 깨운다. */
	vu_group->need_kick = false;         /* [한국어] 깨운 즉시 플래그 클리어 → 동일 kick 중복 방지. */
	eventfd_write(vu_group->intr_fd, 1); /* [한국어] eventfd 에 1 기록 → epoll_wait 가 즉시 반환되어 group 폴링 재개. */
}

/*
 * Make the given DMA address and length available (locally mapped) via iov.
 */
/*
 * [한국어]
 * map_one - 게스트 DMA 주소 구간(IOVA)을 호스트 로컬 VA(iov)로 매핑.
 *
 * @ctx: libvfio-user 컨텍스트(이 endpoint 의 DMA 매핑 테이블 보유).
 * @addr: 매핑할 게스트 IOVA(=GPA) 시작 주소.
 * @len: 매핑할 길이(바이트).
 * @sg: 변환 결과 SGL 디스크립터를 받을 dma_sg_t(호출자 소유 — 나중에 vfu_sgl_put 에 사용).
 * @iov: 결과 호스트 VA/길이를 받을 iovec(호출자 소유).
 * @flags: MAP_RW(쓰기 허용), MAP_QUIET(실패 로그 억제) 비트 조합.
 * @return: 매핑된 호스트 가상주소(iov->iov_base), 실패 시 NULL.
 *
 * 동기/배경: vfio-user 에서 VM 의 메모리는 게스트 IOVA 공간에 있고, 호스트가 SQE/CQE/
 *   데이터 버퍼/shadow doorbell 등에 접근하려면 먼저 자기 VA 로 변환해야 한다. 이 변환은
 *   2단계: (1) vfu_addr_to_sgl 로 IOVA→SGL, (2) vfu_sgl_get 으로 SGL→실제 VA(iovec).
 *   단일 세그먼트(1)만 허용 — 연속 매핑이 안 되면(여러 세그먼트 필요) 실패 처리한다.
 * 동작 단계:
 *   1) flags 로 prot(R 또는 RW) 결정.
 *   2) vfu_addr_to_sgl(maxsg=1) — IOVA 가 단일 연속 세그먼트로 변환 가능한지.
 *   3) 실패 시 -1(권한/미매핑) vs 그 외(N 세그먼트 필요) 구분 로그(MAP_QUIET 면 억제).
 *   4) vfu_sgl_get 으로 실제 호스트 VA 채움.
 * 실행 컨텍스트: I/O hot-path(데이터 매핑) 및 큐 설정 경로. ctx 의 DMA 테이블은
 *   libvfio-user 내부에서 보호.
 * 호출자: map_sdbl, nvme_cmd_map_prps/sgls 의 gpa_to_vva 콜백, 큐 매핑 코드 등.
 * 에러 경로: 어느 단계든 실패 시 NULL 반환 → 호출자가 -EINVAL/-ERANGE 로 변환.
 * 호출 체인: (메모리 매핑) → [map_one] → vfu_addr_to_sgl → vfu_sgl_get
 */
static void *
map_one(vfu_ctx_t *ctx, uint64_t addr, uint64_t len, dma_sg_t *sg,
	struct iovec *iov, int32_t flags)
{
	int prot = PROT_READ;   /* [한국어] 기본은 읽기 전용 매핑(예: SQE, PRP list, SGL 세그먼트는 read-only). */
	int ret;                /* [한국어] libvfio-user 호출 반환 코드. */

	if (flags & MAP_RW) {
		prot |= PROT_WRITE;   /* [한국어] 쓰기 필요(데이터 read 명령의 목적지, CQE, shadow doorbell 등)면 쓰기 권한 추가. */
	}

	assert(ctx != NULL);   /* [한국어] 입력 포인터 NULL 방어 가드 3종. */
	assert(sg != NULL);
	assert(iov != NULL);

	/* [한국어] 1단계: 게스트 IOVA[addr,addr+len) 를 단일 SGL 세그먼트(maxsg=1)로 변환.
	 * libvfio-user 가 이 endpoint 의 DMA 등록 영역과 대조해 권한·연속성을 검사. */
	ret = vfu_addr_to_sgl(ctx, (void *)(uintptr_t)addr, len, sg, 1, prot);
	if (ret < 0) {
		if (ret == -1) {   /* [한국어] -1 = 권한 위반 또는 미매핑 영역(접근 자체 불가). */
			if (!(flags & MAP_QUIET)) {   /* [한국어] MAP_QUIET 면 정상적 탐색 실패라 로그 억제. */
				SPDK_ERRLOG("failed to translate IOVA [%#lx, %#lx) (prot=%d) to local VA: %m\n",
					    addr, addr + len, prot);
			}
		} else {
			/* [한국어] ret<-1: 구간이 비연속이라 여러 세그먼트가 필요(-(ret+1) 개) — 단일 매핑 정책상 실패. */
			SPDK_ERRLOG("failed to translate IOVA [%#lx, %#lx) (prot=%d) to local VA: %d segments needed\n",
				    addr, addr + len, prot, -(ret + 1));
		}
		return NULL;   /* [한국어] 변환 실패 → NULL(호출자가 에러코드로 변환). */
	}

	/* [한국어] 2단계: 위에서 얻은 SGL(sg)을 실제 호스트 VA(iov->iov_base)로 채운다.
	 * 이 매핑은 나중에 vfu_sgl_put(ctx, sg, iov, 1) 로 반드시 해제해야 함. */
	ret = vfu_sgl_get(ctx, sg, iov, 1, 0);
	if (ret != 0) {
		SPDK_ERRLOG("failed to get iovec for IOVA [%#lx, %#lx): %m\n",
			    addr, addr + len);
		return NULL;   /* [한국어] iovec 획득 실패 → NULL. */
	}

	assert(iov->iov_base != NULL);   /* [한국어] 성공 시 호스트 VA 가 반드시 채워져 있어야 함. */
	return iov->iov_base;            /* [한국어] 호스트 로컬 가상주소 반환 — 이제 SQE/데이터 직접 접근 가능. */
}

/*
 * [한국어]
 * nvme_cmd_map_prps - NVMe PRP(Physical Region Page) 디스크립터를 호스트 iovec 배열로 변환.
 *
 * @prv: gpa_to_vva 콜백에 그대로 전달되는 컨텍스트(보통 컨트롤러/endpoint).
 * @cmd: PRP 를 담은 NVMe 명령. dptr.prp.prp1/prp2 사용.
 * @iovs: 변환 결과를 채울 iovec 배열(호출자 소유, 최대 max_iovcnt 개).
 * @max_iovcnt: iovs 배열 용량. 초과하면 -ERANGE.
 * @len: 전송할 총 데이터 길이(바이트).
 * @mps: Memory Page Size(컨트롤러 페이지 크기, NVMe CC.MPS — 보통 4KB).
 * @gpa_to_vva: 게스트 IOVA → 호스트 VA 변환 콜백(보통 map_one 래퍼).
 * @return: 채운 iovec 개수(>0), 실패 시 음수 errno(-EINVAL/-ERANGE).
 *
 * 동기/배경: NVMe 데이터 포인터는 PRP 또는 SGL 방식. PRP 는 페이지 단위 물리주소 목록으로
 *   전송 버퍼를 기술한다(NVMe spec §4.3). vfio-user 타깃은 이를 호스트가 직접 읽고/쓸 수
 *   있는 iovec 로 풀어야 bdev I/O 로 넘길 수 있다. PRP1 은 페이지 내 오프셋이 허용되고,
 *   PRP2 는 (남은 데이터가 1페이지면) 두 번째 버퍼, (1페이지 초과면) PRP list 의 주소다.
 * 동작 단계:
 *   1) prp1 을 매핑하되, 첫 페이지의 잔여 길이(residue)만큼만(페이지 내 오프셋 허용).
 *   2) 남은 길이가 없으면 PRP1 단독(iovcnt=1).
 *   3) 남은 길이 <= mps 면 PRP2 가 직접 두 번째 버퍼(iovcnt=2).
 *   4) 그 이상이면 PRP2 가 PRP list(연속 64bit 주소 배열) — 각 엔트리를 페이지 단위 매핑.
 * 실행 컨텍스트: I/O 명령 처리 hot-path(handle_cmd_req). 컨트롤러 소유 스레드.
 * 호출자: nvme_map_cmd(psdt==PRP 분기).
 * 에러 경로: 매핑 실패/용량 초과 시 음수 반환 → 호출자가 명령을 에러 완료시킴.
 * 호출 체인: nvme_map_cmd → [nvme_cmd_map_prps] → gpa_to_vva(map_one)
 */
static int
nvme_cmd_map_prps(void *prv, struct spdk_nvme_cmd *cmd, struct iovec *iovs,
		  uint32_t max_iovcnt, uint32_t len, size_t mps,
		  void *(*gpa_to_vva)(void *prv, uint64_t addr, uint64_t len, uint32_t flags))
{
	uint64_t prp1, prp2;       /* [한국어] NVMe 명령의 두 PRP 엔트리(게스트 물리주소). */
	void *vva;                 /* [한국어] gpa_to_vva 가 돌려준 호스트 가상주소. */
	uint32_t i;                /* [한국어] PRP list 순회 인덱스. */
	uint32_t residue_len, nents;  /* [한국어] 첫 페이지 잔여 길이 / PRP list 엔트리 수. */
	uint64_t *prp_list;        /* [한국어] PRP list(64bit 주소 배열)의 호스트 포인터. */
	uint32_t iovcnt;           /* [한국어] 최종 채운 iovec 개수(반환값). */

	assert(max_iovcnt > 0);   /* [한국어] 최소 1개 iovec 공간은 보장돼야 함. */

	prp1 = cmd->dptr.prp.prp1;   /* [한국어] 첫 PRP — 첫 버퍼(페이지 내 오프셋 허용). */
	prp2 = cmd->dptr.prp.prp2;   /* [한국어] 둘째 PRP — 두 번째 버퍼 또는 PRP list 주소. */

	/* PRP1 may started with unaligned page address */
	/* [한국어] PRP1 은 페이지 경계에 정렬 안 될 수 있음 → 첫 페이지에서 쓸 수 있는 잔여 길이 계산.
	 * residue = (페이지 크기) - (페이지 내 오프셋). NVMe spec §4.3: PRP1 만 offset 허용. */
	residue_len = mps - (prp1 % mps);
	residue_len = spdk_min(len, residue_len);   /* [한국어] 전체 길이가 더 짧으면 그만큼만. */

	vva = gpa_to_vva(prv, prp1, residue_len, MAP_RW);   /* [한국어] 첫 버퍼를 호스트 VA 로 매핑(RW — 데이터 read/write 양용). */
	if (spdk_unlikely(vva == NULL)) {
		SPDK_ERRLOG("GPA to VVA failed\n");
		return -EINVAL;   /* [한국어] 첫 버퍼 매핑 실패 → 즉시 에러. */
	}
	len -= residue_len;   /* [한국어] 첫 버퍼로 처리한 만큼 남은 길이 차감. */
	if (len && max_iovcnt < 2) {   /* [한국어] 아직 남았는데 iovec 공간이 1개뿐이면 담을 곳 없음. */
		SPDK_ERRLOG("Too many page entries, at least two iovs are required\n");
		return -ERANGE;
	}
	iovs[0].iov_base = vva;          /* [한국어] iovec[0] = 첫 버퍼 호스트 VA. */
	iovs[0].iov_len = residue_len;   /* [한국어] iovec[0] 길이 = 첫 페이지 잔여 길이. */

	if (len) {   /* [한국어] 첫 버퍼로 다 못 담았으면 PRP2 처리 분기. */
		if (spdk_unlikely(prp2 == 0)) {   /* [한국어] 데이터가 남았는데 PRP2 가 0 이면 스펙 위반. */
			SPDK_ERRLOG("no PRP2, %d remaining\n", len);
			return -EINVAL;
		}

		if (len <= mps) {
			/* 2 PRP used */
			/* [한국어] 남은 데이터가 1페이지 이하 → PRP2 가 직접 두 번째 버퍼(PRP list 아님). */
			iovcnt = 2;
			vva = gpa_to_vva(prv, prp2, len, MAP_RW);   /* [한국어] 두 번째 버퍼 매핑. */
			if (spdk_unlikely(vva == NULL)) {
				SPDK_ERRLOG("no VVA for %#" PRIx64 ", len%#x\n",
					    prp2, len);
				return -EINVAL;
			}
			iovs[1].iov_base = vva;   /* [한국어] iovec[1] = 두 번째 버퍼. */
			iovs[1].iov_len = len;    /* [한국어] 나머지 전부. */
		} else {
			/* PRP list used */
			/* [한국어] 남은 데이터가 1페이지 초과 → PRP2 는 PRP list(64bit 주소 배열) 주소.
			 * 필요한 엔트리 수 = ceil(len/mps). 각 엔트리가 페이지 1개를 가리킴. */
			nents = (len + mps - 1) / mps;
			if (spdk_unlikely(nents + 1 > max_iovcnt)) {   /* [한국어] (첫 버퍼 1 + nents) 가 용량 초과면 거부. */
				SPDK_ERRLOG("Too many page entries\n");
				return -ERANGE;
			}

			/* [한국어] PRP list 자체를 read-only 로 매핑(컨트롤러가 주소만 읽음). */
			vva = gpa_to_vva(prv, prp2, nents * sizeof(*prp_list), MAP_R);
			if (spdk_unlikely(vva == NULL)) {
				SPDK_ERRLOG("no VVA for %#" PRIx64 ", nents=%#x\n",
					    prp2, nents);
				return -EINVAL;
			}
			prp_list = vva;   /* [한국어] 호스트에서 본 PRP list 배열 시작. */
			i = 0;
			while (len != 0) {   /* [한국어] 남은 데이터를 페이지 단위로 모두 매핑할 때까지. */
				residue_len = spdk_min(len, mps);   /* [한국어] 이번 페이지에 담을 길이(마지막은 부분 페이지). */
				/* [한국어] PRP list 의 i 번째 페이지 주소를 호스트 VA 로 매핑(2번째 이후는 4KB 정렬 전제). */
				vva = gpa_to_vva(prv, prp_list[i], residue_len, MAP_RW);
				if (spdk_unlikely(vva == NULL)) {
					SPDK_ERRLOG("no VVA for %#" PRIx64 ", residue_len=%#x\n",
						    prp_list[i], residue_len);
					return -EINVAL;
				}
				iovs[i + 1].iov_base = vva;          /* [한국어] iovec[i+1](+1: [0]은 첫 버퍼). */
				iovs[i + 1].iov_len = residue_len;
				len -= residue_len;   /* [한국어] 처리한 만큼 차감. */
				i++;
			}
			iovcnt = i + 1;   /* [한국어] 첫 버퍼 1 + PRP list 페이지 i 개. */
		}
	} else {
		/* 1 PRP used */
		iovcnt = 1;   /* [한국어] 첫 버퍼 하나로 전부 처리됨. */
	}

	assert(iovcnt <= max_iovcnt);   /* [한국어] 채운 개수가 용량을 넘지 않음을 보장. */
	return iovcnt;                  /* [한국어] 호출자에게 iovec 개수 반환. */
}

/*
 * [한국어]
 * nvme_cmd_map_sgls_data - SGL Data Block 디스크립터 배열을 호스트 iovec 로 변환.
 *
 * @prv: gpa_to_vva 콜백 컨텍스트.
 * @sgls: SGL Data Block 디스크립터 배열(이미 호스트 VA 로 매핑된 세그먼트 내부).
 * @num_sgls: 변환할 디스크립터 개수.
 * @iovs: 결과 iovec 배열.
 * @max_iovcnt: iovs 용량.
 * @gpa_to_vva: 게스트 IOVA → 호스트 VA 변환 콜백.
 * @return: 변환한 개수(=num_sgls), 실패 시 음수 errno.
 *
 * 동기/배경: SGL(Scatter-Gather List, NVMe spec §4.4)의 한 세그먼트 안에 들어 있는
 *   Data Block 디스크립터들을 각각 호스트 버퍼로 매핑하는 하위 헬퍼. 각 디스크립터는
 *   반드시 DATA_BLOCK 타입이어야 한다(세그먼트/마지막 세그먼트 타입은 상위에서 처리).
 * 동작 단계: num_sgls > max_iovcnt 면 -ERANGE; 각 항목 타입 검증 후 address/length 매핑.
 * 실행 컨텍스트: I/O hot-path. 컨트롤러 소유 스레드.
 * 호출자: nvme_cmd_map_sgls(세그먼트의 data block 들을 일괄 매핑).
 * 호출 체인: nvme_cmd_map_sgls → [nvme_cmd_map_sgls_data] → gpa_to_vva
 */
static int
nvme_cmd_map_sgls_data(void *prv, struct spdk_nvme_sgl_descriptor *sgls, uint32_t num_sgls,
		       struct iovec *iovs, uint32_t max_iovcnt,
		       void *(*gpa_to_vva)(void *prv, uint64_t addr, uint64_t len, uint32_t flags))
{
	uint32_t i;     /* [한국어] SGL 디스크립터 순회 인덱스. */
	void *vva;      /* [한국어] 매핑된 호스트 VA. */

	if (spdk_unlikely(max_iovcnt < num_sgls)) {   /* [한국어] iovec 공간이 디스크립터 수보다 적으면 담을 수 없음. */
		return -ERANGE;
	}

	for (i = 0; i < num_sgls; i++) {   /* [한국어] 각 SGL Data Block 을 호스트 버퍼로 변환. */
		if (spdk_unlikely(sgls[i].unkeyed.type != SPDK_NVME_SGL_TYPE_DATA_BLOCK)) {   /* [한국어] 여기선 Data Block 만 유효. */
			SPDK_ERRLOG("Invalid SGL type %u\n", sgls[i].unkeyed.type);
			return -EINVAL;
		}
		/* [한국어] 디스크립터의 게스트 주소/길이를 호스트 VA 로 매핑(RW). */
		vva = gpa_to_vva(prv, sgls[i].address, sgls[i].unkeyed.length, MAP_RW);
		if (spdk_unlikely(vva == NULL)) {
			SPDK_ERRLOG("GPA to VVA failed\n");
			return -EINVAL;
		}
		iovs[i].iov_base = vva;                       /* [한국어] iovec[i] base. */
		iovs[i].iov_len = sgls[i].unkeyed.length;     /* [한국어] iovec[i] 길이. */
	}

	return num_sgls;   /* [한국어] 변환한 디스크립터 개수 반환. */
}

/*
 * [한국어]
 * nvme_cmd_map_sgls - NVMe SGL 디스크립터 체인을 호스트 iovec 배열로 변환.
 *
 * @prv: gpa_to_vva 콜백 컨텍스트.
 * @cmd: SGL 을 담은 NVMe 명령(dptr.sgl1 이 첫 디스크립터).
 * @iovs: 결과 iovec 배열.
 * @max_iovcnt: iovs 용량.
 * @len: 총 전송 길이(단일 Data Block 검증에만 사용).
 * @mps: 페이지 크기(SGL 경로에서는 미사용 — 시그니처 통일용).
 * @gpa_to_vva: 게스트 IOVA → 호스트 VA 콜백.
 * @return: 채운 iovec 개수, 실패 시 음수 errno.
 *
 * 동기/배경: SGL(NVMe spec §4.4)은 PRP 와 달리 디스크립터 체인 구조다. sgl1 이
 *   (a) 단일 Data Block 이면 버퍼 하나, (b) Segment/Last Segment 면 디스크립터들이 모인
 *   세그먼트를 가리킨다. 세그먼트를 따라가며 Data Block 들을 매핑하고, 마지막이 또 다른
 *   세그먼트를 가리키면 그 마지막 디스크립터로 이동해 체인을 계속 따라간다.
 * 동작 단계:
 *   1) sgl1 이 Data Block 이면 단일 매핑 후 1 반환(빠른 경로).
 *   2) 무한 루프: 현재 sgl 이 Segment/Last Segment 인지 검증 → 세그먼트 매핑(read-only).
 *   3) 세그먼트의 마지막 디스크립터가 Data Block 이면 전체를 매핑하고 종료.
 *   4) 마지막이 또 세그먼트면 앞부분만 매핑 후 last 로 이동해 체인 추적 계속.
 * 실행 컨텍스트: I/O hot-path. 컨트롤러 소유 스레드.
 * 호출자: nvme_map_cmd(psdt != PRP 분기).
 * 호출 체인: nvme_map_cmd → [nvme_cmd_map_sgls] → nvme_cmd_map_sgls_data → gpa_to_vva
 */
static int
nvme_cmd_map_sgls(void *prv, struct spdk_nvme_cmd *cmd, struct iovec *iovs, uint32_t max_iovcnt,
		  uint32_t len, size_t mps,
		  void *(*gpa_to_vva)(void *prv, uint64_t addr, uint64_t len, uint32_t flags))
{
	struct spdk_nvme_sgl_descriptor *sgl, *last_sgl;   /* [한국어] 현재 디스크립터 / 세그먼트의 마지막 디스크립터. */
	uint32_t num_sgls, seg_len;                        /* [한국어] 세그먼트 내 디스크립터 수 / 세그먼트 바이트 길이. */
	void *vva;                                         /* [한국어] 매핑된 호스트 VA. */
	int ret;                                           /* [한국어] 하위 매핑 호출 반환값. */
	uint32_t total_iovcnt = 0;                         /* [한국어] 지금까지 누적된 iovec 개수. */

	/* SGL cases */
	sgl = &cmd->dptr.sgl1;   /* [한국어] SGL 체인의 시작 = 명령의 sgl1. */

	/* only one SGL segment */
	/* [한국어] 빠른 경로: sgl1 자체가 단일 Data Block 이면 버퍼 하나만 매핑하고 끝. */
	if (sgl->unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK) {
		assert(max_iovcnt > 0);
		vva = gpa_to_vva(prv, sgl->address, sgl->unkeyed.length, MAP_RW);   /* [한국어] 데이터 버퍼 매핑. */
		if (spdk_unlikely(vva == NULL)) {
			SPDK_ERRLOG("GPA to VVA failed\n");
			return -EINVAL;
		}
		iovs[0].iov_base = vva;                   /* [한국어] iovec[0] = 유일한 데이터 버퍼. */
		iovs[0].iov_len = sgl->unkeyed.length;
		assert(sgl->unkeyed.length == len);       /* [한국어] 단일 버퍼 길이는 전체 전송 길이와 같아야 함. */

		return 1;
	}

	for (;;) {   /* [한국어] 세그먼트 체인 추적 루프(마지막 디스크립터가 Data Block 이 될 때까지). */
		/* [한국어] sgl1 또는 체인 노드는 Segment / Last Segment 타입이어야 유효. */
		if (spdk_unlikely((sgl->unkeyed.type != SPDK_NVME_SGL_TYPE_SEGMENT) &&
				  (sgl->unkeyed.type != SPDK_NVME_SGL_TYPE_LAST_SEGMENT))) {
			SPDK_ERRLOG("Invalid SGL type %u\n", sgl->unkeyed.type);
			return -EINVAL;
		}

		seg_len = sgl->unkeyed.length;   /* [한국어] 이 세그먼트의 바이트 길이. */
		/* [한국어] 세그먼트 길이는 디스크립터 크기의 정수배여야 함(디스크립터 배열이므로). */
		if (spdk_unlikely(seg_len % sizeof(struct spdk_nvme_sgl_descriptor))) {
			SPDK_ERRLOG("Invalid SGL segment len %u\n", seg_len);
			return -EINVAL;
		}

		num_sgls = seg_len / sizeof(struct spdk_nvme_sgl_descriptor);   /* [한국어] 세그먼트 내 디스크립터 개수. */
		/* [한국어] 세그먼트(디스크립터 배열) 자체를 read-only 로 매핑(컨트롤러가 디스크립터만 읽음). */
		vva = gpa_to_vva(prv, sgl->address, sgl->unkeyed.length, MAP_R);
		if (spdk_unlikely(vva == NULL)) {
			SPDK_ERRLOG("GPA to VVA failed\n");
			return -EINVAL;
		}

		/* sgl point to the first segment */
		sgl = (struct spdk_nvme_sgl_descriptor *)vva;   /* [한국어] sgl 을 매핑된 세그먼트 첫 디스크립터로 갱신. */
		last_sgl = &sgl[num_sgls - 1];                  /* [한국어] 세그먼트의 마지막 디스크립터(체인 계속 여부 판단). */

		/* we are done */
		/* [한국어] 마지막 디스크립터가 Data Block 이면 이 세그먼트가 체인의 끝 — 전체를 매핑하고 반환. */
		if (last_sgl->unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK) {
			/* map whole sgl list */
			ret = nvme_cmd_map_sgls_data(prv, sgl, num_sgls, &iovs[total_iovcnt],
						     max_iovcnt - total_iovcnt, gpa_to_vva);
			if (spdk_unlikely(ret < 0)) {
				return ret;
			}
			total_iovcnt += ret;   /* [한국어] 누적 iovec 수 갱신. */

			return total_iovcnt;
		}

		/* [한국어] 마지막이 또 다른 세그먼트면, 앞쪽 (num_sgls-1) 개 Data Block 만 먼저 매핑. */
		if (num_sgls > 1) {
			/* map whole sgl exclude last_sgl */
			ret = nvme_cmd_map_sgls_data(prv, sgl, num_sgls - 1, &iovs[total_iovcnt],
						     max_iovcnt - total_iovcnt, gpa_to_vva);
			if (spdk_unlikely(ret < 0)) {
				return ret;
			}
			total_iovcnt += ret;
		}

		/* move to next level's segments */
		sgl = last_sgl;   /* [한국어] 마지막 디스크립터(다음 세그먼트 포인터)로 이동해 체인 추적 계속. */
	}

	return 0;   /* [한국어] 도달 불가(루프는 return 으로만 탈출) — 컴파일러 만족용. */
}

/*
 * [한국어]
 * nvme_map_cmd - NVMe 명령의 데이터 포인터(PRP 또는 SGL)를 호스트 iovec 로 매핑하는 디스패처.
 *
 * @prv: gpa_to_vva 콜백 컨텍스트.
 * @cmd: 매핑 대상 NVMe 명령. cmd->psdt 로 PRP/SGL 구분.
 * @iovs: 결과 iovec 배열.
 * @max_iovcnt: iovs 용량.
 * @len: 총 전송 길이.
 * @mps: 페이지 크기.
 * @gpa_to_vva: 게스트 IOVA → 호스트 VA 변환 콜백.
 * @return: 채운 iovec 개수, 실패 시 음수 errno.
 *
 * 동기/배경: NVMe 명령의 데이터 영역은 PSDT(PRP or SGL for Data Transfer) 필드로 PRP 방식
 *   인지 SGL 방식인지 정해진다(NVMe spec §4.3/§4.4, command Dword0 PSDT). 이 함수는 그
 *   필드만 보고 적절한 매핑 함수로 분기하는 얇은 디스패처다. 결과 iovec 는 이후 bdev I/O
 *   의 입출력 버퍼 목록으로 사용된다.
 * 동작 단계: psdt==PRP 면 nvme_cmd_map_prps, 아니면 nvme_cmd_map_sgls 위임.
 * 실행 컨텍스트: I/O 명령 처리 hot-path. 컨트롤러 소유 스레드.
 * 호출자: handle_cmd_req 등 게스트 명령의 데이터 버퍼를 풀어야 하는 코드.
 * 호출 체인: handle_cmd_req → [nvme_map_cmd] → nvme_cmd_map_prps / nvme_cmd_map_sgls
 */
static int
nvme_map_cmd(void *prv, struct spdk_nvme_cmd *cmd, struct iovec *iovs, uint32_t max_iovcnt,
	     uint32_t len, size_t mps,
	     void *(*gpa_to_vva)(void *prv, uint64_t addr, uint64_t len, uint32_t flags))
{
	/* [한국어] PSDT 필드가 PRP(0)면 PRP 매핑 경로. NVMe spec §4.3. */
	if (cmd->psdt == SPDK_NVME_PSDT_PRP) {
		return nvme_cmd_map_prps(prv, cmd, iovs, max_iovcnt, len, mps, gpa_to_vva);
	}

	/* [한국어] 그 외(SGL_DATA_BLOCK_META 등)면 SGL 매핑 경로. NVMe spec §4.4. */
	return nvme_cmd_map_sgls(prv, cmd, iovs, max_iovcnt, len, mps, gpa_to_vva);
}

/*
 * For each queue, update the location of its doorbell to the correct location:
 * either our own BAR0, or the guest's configured shadow doorbell area.
 *
 * The Admin queue (qid: 0) does not ever use shadow doorbells.
 */
/*
 * [한국어]
 * vfio_user_ctrlr_switch_doorbells - 모든 I/O 큐의 doorbell 포인터를 BAR0↔shadow 로 전환.
 *
 * @ctrlr: 대상 컨트롤러.
 * @shadow: true 면 게스트가 등록한 shadow doorbell 버퍼로, false 면 호스트 BAR0 doorbell 로.
 *
 * 동기/배경: NVMe 1.3 Doorbell Buffer Config(shadow doorbell) 기능을 켜고 끌 때, 각 SQ/CQ
 *   의 doorbell 접근 포인터(dbl_tailp/dbl_headp)를 새 메모리 영역으로 일괄 갱신해야 한다.
 *   shadow doorbell 은 게스트 메모리에 있는 doorbell mirror 로, MMIO trap 없이 호스트가
 *   값을 폴링할 수 있게 해 성능을 높인다. admin 큐(qid 0)는 shadow 를 쓰지 않으므로 i=1 부터.
 * 동작 단계:
 *   1) shadow 여부로 사용할 doorbell 베이스 결정(sdbl->shadow_doorbells vs bar0_doorbells).
 *   2) i=1..MAX 의 각 SQ/CQ 에 대해 queue_index 로 슬롯 포인터 재설정.
 *   3) SQ 는 shadow 로 전환 시 need_rearm=true(아래 첫 폴링에서 eventidx 재무장 필요).
 * 실행 컨텍스트: 컨트롤러 소유 poll group 스레드(doorbell 일관성 보장).
 * 호출자: shadow doorbell 활성/비활성/마이그레이션 처리 코드.
 * 호출 체인: (shadow 전환) → [vfio_user_ctrlr_switch_doorbells] → queue_index
 */
static void
vfio_user_ctrlr_switch_doorbells(struct nvmf_vfio_user_ctrlr *ctrlr, bool shadow)
{
	/* [한국어] 전환 후 사용할 doorbell 베이스: shadow 면 게스트 mirror, 아니면 호스트 BAR0. */
	volatile uint32_t *doorbells = shadow ? ctrlr->sdbl->shadow_doorbells :
				       ctrlr->bar0_doorbells;

	assert(doorbells != NULL);   /* [한국어] 선택된 베이스가 반드시 매핑되어 있어야 함. */

	/* [한국어] admin(qid 0)은 shadow 미사용 → i=1 부터 I/O 큐만 순회. */
	for (size_t i = 1; i < NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR; i++) {
		struct nvmf_vfio_user_sq *sq = ctrlr->sqs[i];   /* [한국어] i 번째 SQ(없을 수 있음). */
		struct nvmf_vfio_user_cq *cq = ctrlr->cqs[i];   /* [한국어] i 번째 CQ(없을 수 있음). */

		if (sq != NULL) {
			/* [한국어] SQ tail doorbell 포인터를 새 베이스의 (qid,false) 슬롯으로 재설정. */
			sq->dbl_tailp = doorbells + queue_index(sq->qid, false);

			/* [한국어] shadow 로 전환 시 eventidx 재무장 필요 표시(다음 폴링에서 set_sq_eventidx).
			 * BAR0 로 전환(shadow=false)이면 false → 재무장 불필요. */
			ctrlr->sqs[i]->need_rearm = shadow;
		}

		if (cq != NULL) {
			/* [한국어] CQ head doorbell 포인터를 (qid,true) 슬롯으로 재설정. CQ 는 rearm 개념 없음. */
			cq->dbl_headp = doorbells + queue_index(cq->qid, true);
		}
	}
}

/*
 * [한국어]
 * unmap_sdbl - shadow doorbell 버퍼(2개)의 vfio-user SGL 매핑을 해제.
 *
 * @vfu_ctx: libvfio-user 컨텍스트.
 * @sdbl: 해제할 shadow doorbell 구조체(매핑된 iovs/sgs 보유).
 *
 * 동기/배경: shadow doorbell 은 게스트 메모리의 두 버퍼(doorbell mirror PRP1, eventidx PRP2)
 *   를 호스트 VA 로 매핑한 것이다. 비활성/재설정/파괴 시 이 매핑을 vfu_sgl_put 으로 되돌려야
 *   한다(map_one 의 역연산). map_one 한 만큼만(iov_len 이 0 이 아닌 것만) 풀어준다.
 * 동작 단계: iovs/sgs 중 하나라도 NULL 이면 매핑된 적 없음 → 즉시 반환; 각 버퍼에 대해
 *   index_to_sg_t 로 sg 를 얻어 vfu_sgl_put.
 * 실행 컨텍스트: 컨트롤러 소유 스레드(파괴/전환 경로).
 * 호출자: free_sdbl.
 * 호출 체인: free_sdbl → [unmap_sdbl] → vfu_sgl_put
 */
static void
unmap_sdbl(vfu_ctx_t *vfu_ctx, struct nvmf_vfio_user_shadow_doorbells *sdbl)
{
	assert(vfu_ctx != NULL);   /* [한국어] 입력 NULL 방어. */
	assert(sdbl != NULL);

	/*
	 * An allocation error would result in only one of the two being
	 * non-NULL.  If that is the case, no memory should have been mapped.
	 */
	/* [한국어] alloc 실패로 둘 중 하나만 NULL 인 경우엔 매핑이 일어나지 않았으므로 풀 것도 없음. */
	if (sdbl->iovs == NULL || sdbl->sgs == NULL) {
		return;
	}

	/* [한국어] 두 버퍼(doorbell, eventidx)를 순회하며 매핑 해제. */
	for (size_t i = 0; i < NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT; ++i) {
		struct iovec *iov;
		dma_sg_t *sg;

		if (!sdbl->iovs[i].iov_len) {   /* [한국어] 길이 0 = 이 버퍼는 매핑된 적 없음 → 건너뜀. */
			continue;
		}

		sg = index_to_sg_t(sdbl->sgs, i);   /* [한국어] i 번째 dma_sg_t(불투명 타입 수동 인덱싱). */
		iov = sdbl->iovs + i;               /* [한국어] i 번째 iovec. */

		vfu_sgl_put(vfu_ctx, sg, iov, 1);   /* [한국어] map_one 의 역연산 — 호스트 VA 매핑 반환. */
	}
}

/*
 * [한국어]
 * free_sdbl - shadow doorbell 구조체와 그 보조 배열을 매핑 해제 후 해제.
 *
 * @vfu_ctx: libvfio-user 컨텍스트.
 * @sdbl: 해제할 shadow doorbell 구조체. NULL 허용(no-op).
 *
 * 동기/배경: map_sdbl 의 완전한 역연산. 먼저 게스트 버퍼 매핑을 풀고(unmap_sdbl),
 *   그 다음 호스트가 직접 calloc 한 보조 배열(sgs/iovs)과 구조체 자체를 free 한다.
 *   shadow_doorbells/eventidxs 포인터는 매핑(게스트 메모리)이지 alloc 이 아니므로 free 금지.
 * 동작 단계: NULL 가드 → unmap_sdbl → free(sgs)/free(iovs)/free(sdbl).
 * 실행 컨텍스트: 컨트롤러 소유 스레드.
 * 호출자: map_sdbl 의 에러 경로, shadow doorbell 교체/컨트롤러 파괴 경로.
 * 호출 체인: (shadow 해제) → [free_sdbl] → unmap_sdbl → free
 */
static void
free_sdbl(vfu_ctx_t *vfu_ctx, struct nvmf_vfio_user_shadow_doorbells *sdbl)
{
	if (sdbl == NULL) {   /* [한국어] NULL 이면 할 일 없음(map_sdbl 초기 실패 등). */
		return;
	}

	unmap_sdbl(vfu_ctx, sdbl);   /* [한국어] 먼저 게스트 버퍼 매핑부터 해제. */

	/*
	 * sdbl->shadow_doorbells and sdbl->eventidxs were mapped,
	 * not allocated, so don't free() them.
	 */
	/* [한국어] shadow_doorbells/eventidxs 는 게스트 메모리 매핑 포인터라 free 하지 않는다
	 * (free 하면 게스트 메모리를 호스트 힙으로 오인해 손상). 호스트가 alloc 한 건 sgs/iovs 뿐. */
	free(sdbl->sgs);    /* [한국어] dma_sg_t 배열 해제. */
	free(sdbl->iovs);   /* [한국어] iovec 배열 해제. */
	free(sdbl);         /* [한국어] 구조체 자체 해제. */
}

/*
 * [한국어]
 * map_sdbl - 게스트가 지정한 shadow doorbell/eventidx 두 버퍼를 호스트 VA 로 매핑.
 *
 * @vfu_ctx: libvfio-user 컨텍스트.
 * @prp1: shadow doorbell 버퍼(게스트가 doorbell 값을 쓰는 mirror)의 게스트 주소.
 * @prp2: eventidx 버퍼(호스트가 깨우기 임계값을 쓰는 영역)의 게스트 주소.
 * @len: 각 버퍼의 길이(보통 doorbell 영역 전체 크기).
 * @return: 매핑 완료된 nvmf_vfio_user_shadow_doorbells*, 실패 시 NULL.
 *
 * 동기/배경: NVMe 1.3 Doorbell Buffer Config(opcode 0x7C) admin 명령은 게스트가 두 버퍼를
 *   PRP1/PRP2 로 제공한다 — (1) shadow doorbell: 게스트가 doorbell 값을 MMIO 대신 여기에 써
 *   호스트가 폴링, (2) eventidx: 호스트가 "이 값에 도달하면 깨워달라"는 임계값을 게스트에 알림.
 *   이 함수가 두 버퍼를 map_one 으로 호스트 VA 에 고정해 둔다.
 * 동작 단계:
 *   1) sdbl/sgs/iovs 할당(calloc).
 *   2) PRP1(shadow doorbell)을 RW 로 map_one.
 *   3) PRP2(eventidx)를 RW 로 map_one(index 1 슬롯).
 *   4) iovs[0]/iovs[1].iov_base 를 shadow_doorbells/eventidxs 포인터로 캐싱.
 * 실행 컨텍스트: 컨트롤러 소유 스레드(Doorbell Buffer Config 명령 처리).
 * 호출자: Doorbell Buffer Config admin 명령 핸들러.
 * 에러 경로: 어느 단계든 실패 시 err: 로 점프 → free_sdbl 후 NULL.
 * 호출 체인: (Doorbell Buffer Config) → [map_sdbl] → map_one → free_sdbl(에러 시)
 */
static struct nvmf_vfio_user_shadow_doorbells *
map_sdbl(vfu_ctx_t *vfu_ctx, uint64_t prp1, uint64_t prp2, size_t len)
{
	struct nvmf_vfio_user_shadow_doorbells *sdbl = NULL;   /* [한국어] 생성할 shadow doorbell 구조체. */
	dma_sg_t *sg2 = NULL;   /* [한국어] eventidx 버퍼용 두 번째 SGL 디스크립터 포인터. */
	void *p;                /* [한국어] map_one 결과 호스트 VA(성공/실패 판정용). */

	assert(vfu_ctx != NULL);   /* [한국어] 컨텍스트 NULL 방어. */

	sdbl = calloc(1, sizeof(*sdbl));   /* [한국어] 구조체 zero-alloc. */
	if (sdbl == NULL) {
		goto err;   /* [한국어] OOM → 정리 후 NULL. */
	}

	/* [한국어] 두 버퍼(doorbell, eventidx)용 dma_sg_t 배열과 iovec 배열 할당.
	 * dma_sg_size()는 불투명 타입 크기라 런타임에 곱해야 함. */
	sdbl->sgs = calloc(NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT, dma_sg_size());
	sdbl->iovs = calloc(NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT, sizeof(*sdbl->iovs));
	if (sdbl->sgs == NULL || sdbl->iovs == NULL) {
		goto err;   /* [한국어] 둘 중 하나라도 실패 → free_sdbl 이 부분 상태도 안전 정리. */
	}

	/* Map shadow doorbell buffer (PRP1). */
	/* [한국어] 첫 버퍼(shadow doorbell mirror)를 호스트 VA 로 매핑. 게스트가 여기에 doorbell
	 * 값을 쓰고 호스트가 폴링 → RW 매핑(호스트가 copy_doorbells 시 쓰기도 함). */
	p = map_one(vfu_ctx, prp1, len, sdbl->sgs, sdbl->iovs, MAP_RW);

	if (p == NULL) {
		goto err;   /* [한국어] 첫 버퍼 매핑 실패. */
	}

	/*
	 * Map eventidx buffer (PRP2).
	 * Should only be written to by the controller.
	 */
	/* [한국어] 두 번째 버퍼(eventidx) — 원칙적으로 호스트(컨트롤러)만 쓴다. sgs 배열의 1번
	 * 슬롯을 사용하므로 index_to_sg_t 로 두 번째 dma_sg_t 포인터를 얻는다. */

	sg2 = index_to_sg_t(sdbl->sgs, 1);

	/* [한국어] eventidx 버퍼를 호스트 VA 로 매핑(iovs 의 1번 슬롯에 결과 저장). */
	p = map_one(vfu_ctx, prp2, len, sg2, sdbl->iovs + 1, MAP_RW);

	if (p == NULL) {
		goto err;   /* [한국어] 두 번째 버퍼 매핑 실패. */
	}

	/* [한국어] 매핑된 두 호스트 VA 를 타입드 포인터로 캐싱 — 이후 빠른 접근용. */
	sdbl->shadow_doorbells = (uint32_t *)sdbl->iovs[0].iov_base;   /* [한국어] 게스트 doorbell mirror. */
	sdbl->eventidxs = (uint32_t *)sdbl->iovs[1].iov_base;          /* [한국어] 깨우기 임계값 버퍼. */

	return sdbl;   /* [한국어] 매핑 완료된 shadow doorbell 구조체 반환. */

err:
	free_sdbl(vfu_ctx, sdbl);   /* [한국어] 부분 할당/매핑을 안전하게 일괄 정리(NULL 가드 내장). */
	return NULL;
}

/*
 * Copy doorbells from one buffer to the other, during switches between BAR0
 * doorbells and shadow doorbells.
 */
/*
 * [한국어]
 * copy_doorbells - BAR0↔shadow 전환 시 한 doorbell 버퍼의 값을 다른 버퍼로 복사.
 *
 * @ctrlr: 대상 컨트롤러(어느 큐가 존재하는지 판단).
 * @from: 복사 원본 doorbell 버퍼(volatile — 게스트/MMIO 가 동시 수정 가능).
 * @to: 복사 대상 doorbell 버퍼(volatile).
 *
 * 동기/배경: shadow doorbell 을 켜거나(또는 마이그레이션) BAR0 로 되돌릴 때, 기존 버퍼에
 *   누적된 doorbell 값을 새 버퍼로 옮겨야 큐 상태(tail/head)의 연속성이 깨지지 않는다.
 *   memcpy 를 못 쓰는 이유: from/to 가 volatile 이라 컴파일러가 접근을 최적화·재배열하면
 *   안 되는데 memcpy 는 그 의미를 보존하지 않기 때문(반드시 워드 단위 volatile 접근).
 * 동작 단계: 존재하는 SQ/CQ 의 (qid, false/true) 슬롯만 골라 from→to 로 한 워드씩 복사.
 * 실행 컨텍스트: 컨트롤러 소유 스레드(doorbell 전환 경로).
 * 호출자: shadow doorbell 활성/비활성 전환, 마이그레이션 복원 코드.
 * 호출 체인: (doorbell 전환) → [copy_doorbells] → queue_index
 */
static void
copy_doorbells(struct nvmf_vfio_user_ctrlr *ctrlr,
	       const volatile uint32_t *from, volatile uint32_t *to)
{
	assert(ctrlr != NULL);   /* [한국어] 입력 NULL 방어 3종. */
	assert(from != NULL);
	assert(to != NULL);

	SPDK_DEBUGLOG(vfio_user_db,
		      "%s: migrating shadow doorbells from %p to %p\n",
		      ctrlr_id(ctrlr), from, to);   /* [한국어] 어느 버퍼에서 어디로 옮기는지 추적 로그. */

	/* Can't use memcpy because it doesn't respect volatile semantics. */
	/* [한국어] volatile 의미 보존을 위해 워드 단위 수동 복사. 존재하는 큐의 슬롯만 옮긴다. */
	for (size_t i = 0; i < NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR; ++i) {
		if (ctrlr->sqs[i] != NULL) {
			/* [한국어] i 번째 SQ tail doorbell 슬롯 복사((qid,false)). */
			to[queue_index(i, false)] = from[queue_index(i, false)];
		}

		if (ctrlr->cqs[i] != NULL) {
			/* [한국어] i 번째 CQ head doorbell 슬롯 복사((qid,true)). */
			to[queue_index(i, true)] = from[queue_index(i, true)];
		}
	}
}

/*
 * [한국어]
 * fail_ctrlr - 컨트롤러를 치명적 오류(Controller Fatal Status) 상태로 전환.
 *
 * @vu_ctrlr: 실패시킬 컨트롤러. ctrlr(generic spdk_nvmf_ctrlr) 가 유효해야 함.
 *
 * 동기/배경: 복구 불가능한 오류(예: 잘못된 doorbell, DMA 매핑 실패 등)가 발생하면 NVMe
 *   컨트롤러를 CSTS.CFS=1(Controller Fatal Status)로 표시해 게스트 드라이버가 컨트롤러를
 *   리셋하도록 유도한다(NVMe spec §3.1.6 CSTS). 이미 CFS 가 1 이면 로그를 중복 출력하지 않는다.
 * 동작 단계: 현재 레지스터를 읽어 CFS==0(아직 정상)일 때만 에러 로그; nvmf_ctrlr_set_fatal_status 호출.
 * 실행 컨텍스트: 컨트롤러 소유 스레드(오류 감지 지점).
 * 호출자: doorbell/큐/매핑 처리 중 복구 불가 오류를 만난 모든 경로.
 * 호출 체인: (치명 오류) → [fail_ctrlr] → nvmf_ctrlr_set_fatal_status
 */
static void
fail_ctrlr(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	const struct spdk_nvmf_registers *regs;   /* [한국어] 현재 NVMe 컨트롤러 레지스터 스냅샷(CSTS 확인용). */

	assert(vu_ctrlr != NULL);          /* [한국어] 입력 NULL 방어. */
	assert(vu_ctrlr->ctrlr != NULL);   /* [한국어] generic ctrlr 연결 확인. */

	regs = spdk_nvmf_ctrlr_get_regs(vu_ctrlr->ctrlr);   /* [한국어] NVMe-oF 코어에서 컨트롤러 레지스터 획득. */
	if (regs->csts.bits.cfs == 0) {   /* [한국어] 아직 CFS 가 안 켜졌을 때만(중복 로그 방지). */
		SPDK_ERRLOG(":%s failing controller\n", ctrlr_id(vu_ctrlr));
	}

	/* [한국어] CSTS.CFS=1 설정 — 게스트가 컨트롤러를 리셋하도록 유도하는 표준 NVMe 신호. */
	nvmf_ctrlr_set_fatal_status(vu_ctrlr->ctrlr);
}

/*
 * [한국어]
 * ctrlr_interrupt_enabled - 게스트가 인터럽트(INTx 또는 MSI-X)를 활성화했는지 판정.
 *
 * @vu_ctrlr: 대상 컨트롤러. endpoint(PCI config space, MSI-X cap)가 유효해야 함.
 * @return: 게스트가 인터럽트 전달을 받을 준비가 되어 있으면 true.
 *
 * 동기/배경: 완료 후 게스트에 인터럽트를 보낼지(vfu_irq_trigger) 결정하려면, 게스트가
 *   PCI 레벨에서 인터럽트를 켰는지 확인해야 한다. 판정 기준 2가지: (1) PCI Command 레지스터의
 *   Interrupt Disable 비트(cmd.id)가 0 이면 legacy INTx 가능, (2) MSI-X Message Control 의
 *   MSI-X Enable(mxc.mxe)이 1 이면 MSI-X 가능. 둘 중 하나라도 만족하면 인터럽트 전달 가능.
 * 동작: PCI config space 와 MSI-X cap 를 읽어 (!cmd.id || mxc.mxe) 평가.
 * 실행 컨텍스트: 완료 처리 hot-path(post_completion 직전). config space 는 게스트가
 *   드물게 갱신 — 동일 스레드에서 읽기.
 * 호출자: 완료 후 인터럽트 발행을 결정하는 코드(post_completion 등).
 * 호출 체인: (완료 후 인터럽트 결정) → [ctrlr_interrupt_enabled] → PCI/MSI-X 레지스터
 */
static inline bool
ctrlr_interrupt_enabled(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	assert(vu_ctrlr != NULL);             /* [한국어] 입력 NULL 방어. */
	assert(vu_ctrlr->endpoint != NULL);   /* [한국어] endpoint(PCI config/MSI-X) 연결 확인. */

	/* [한국어] libvfio-user 가 관리하는 게스트 PCI config space 포인터. */
	vfu_pci_config_space_t *pci = vu_ctrlr->endpoint->pci_config_space;

	/* [한국어] INTx 가능(!Interrupt Disable) 또는 MSI-X Enable 중 하나라도 켜졌으면 인터럽트 전달 가능.
	 * cmd.id: PCI Command 의 Interrupt Disable 비트, mxc.mxe: MSI-X Message Control 의 Enable 비트. */
	return (!pci->hdr.cmd.id || vu_ctrlr->endpoint->msix->mxc.mxe);
}

/*
 * [한국어]
 * nvmf_vfio_user_destroy_endpoint - 1개 endpoint (= 1 VM 가상 NVMe 디바이스) 의 모든 자원 해제.
 *
 * @endpoint: 해제할 endpoint. TAILQ 에서 이미 제거된 상태여야 한다 (이중 참조 방지).
 *
 * 동기/배경: endpoint 는 vfu_ctx (libvfio-user 컨텍스트) + devmem 공유메모리 fd +
 *   BAR0 mmap + accept poller/interrupt + 소켓을 묶은 단위이다. transport 종료나
 *   stop_listen 시 이 모든 자원을 누수 없이 되돌려야 한다.
 * 동작 단계:
 *   1) accept_intr/accept_poller 등록 해제 — 더 이상 새 VM 연결을 받지 않도록.
 *   2) BAR0 mmap 해제 (devmem 위에 매핑된 doorbell 영역).
 *   3) devmem_fd / vfu_ctx / endpoint->lock 정리.
 * 실행 컨텍스트: transport 를 소유한 main thread (단일 호출). vfu_ctx 폴링 스레드가
 *   이미 멈춘 상태에서만 안전하다.
 * 호출자: nvmf_vfio_user_destroy (transport 종료), nvmf_vfio_user_stop_listen
 *   (listen 중단), nvmf_vfio_user_listen 의 에러 cleanup 경로.
 * 에러 경로: void 반환 — 부분적으로 NULL 인 필드는 if 가드로 건너뛴다.
 *
 * 호출 체인:
 *   nvmf_vfio_user_destroy / _stop_listen → [nvmf_vfio_user_destroy_endpoint]
 *     → spdk_interrupt_unregister / munmap / close / vfu_destroy_ctx / free
 */
static void
nvmf_vfio_user_destroy_endpoint(struct nvmf_vfio_user_endpoint *endpoint)
{
	/* [한국어] 어떤 endpoint 를 정리하는지 디버그 로그 (endpoint_id = 소켓 경로 문자열). */
	SPDK_DEBUGLOG(nvmf_vfio, "destroy endpoint %s\n", endpoint_id(endpoint));

	/* [한국어] interrupt 모드일 때 등록한 accept fd interrupt 해제 — VM 연결 수락 중단. */
	spdk_interrupt_unregister(&endpoint->accept_intr);
	/* [한국어] polling 모드일 때 등록한 accept poller 해제 (위 interrupt 와 배타적 사용). */
	spdk_poller_unregister(&endpoint->accept_poller);

	/* [한국어] BAR0 가 mmap 되어 있으면 (mappable BAR0 또는 doorbell 영역) 해제. */
	if (endpoint->bar0) {
		/* [한국어] devmem fd 위에 매핑한 BAR0 영역 (doorbell 포함) 을 munmap.
		 * NVME_REG_BAR0_SIZE = 0x1000 + 2*MAX_QPAIRS*DOORBELL_SIZE (page align). */
		munmap((void *)endpoint->bar0, NVME_REG_BAR0_SIZE);
		endpoint->bar0 = NULL;            /* [한국어] dangling 포인터 방지. */
		endpoint->bar0_doorbells = NULL;  /* [한국어] BAR0 내부를 가리키던 doorbell 포인터도 무효화. */
	}

	/* [한국어] devmem 공유메모리 (shm) fd 가 열려 있으면 닫는다 (fd>0 가 유효 fd). */
	if (endpoint->devmem_fd > 0) {
		close(endpoint->devmem_fd);  /* [한국어] shm_open 으로 만든 fd 반환 — 커널 shm 객체 unlink 는 별도. */
	}

	/* [한국어] libvfio-user 컨텍스트가 생성되었으면 파괴 — 소켓 close + 내부 자원 해제. */
	if (endpoint->vfu_ctx) {
		vfu_destroy_ctx(endpoint->vfu_ctx);  /* [한국어] libvfio-user 가 소켓 fd, BAR 등록 정보 등을 정리. */
	}

	/* [한국어] endpoint 단위 mutex 파괴 — 이 endpoint 에 대한 cross-thread 접근 보호용이었음. */
	pthread_mutex_destroy(&endpoint->lock);
	free(endpoint);  /* [한국어] endpoint 구조체 자체 해제. */
}

/* called when process exits */
/*
 * [한국어]
 * nvmf_vfio_user_destroy - vfio-user transport 전체를 파괴 (.destroy vtable 콜백).
 *
 * @transport: 파괴할 NVMe-oF transport 객체. nvmf_vfio_user_create 가 만든
 *   nvmf_vfio_user_transport.transport 멤버를 가리킨다 (SPDK_CONTAINEROF 로 역참조).
 * @cb_fn: 파괴 완료를 NVMe-oF 코어에 알리는 비동기 완료 콜백. NULL 가능.
 * @cb_arg: cb_fn 에 그대로 전달되는 사용자 컨텍스트.
 *
 * 동기/배경: NVMe-oF target 종료(프로세스 exit) 시 transport list 의 각 transport 에
 *   대해 코어가 이 .destroy 콜백을 호출한다. vfio-user 는 transport 아래에 여러
 *   endpoint (VM 마다 1개) 를 거느리므로, 모든 endpoint 를 먼저 정리한 뒤 transport
 *   자체를 free 해야 누수가 없다.
 * 동작 단계:
 *   1) container_of 로 vu_transport 복원.
 *   2) transport 레벨 두 mutex (endpoints 보호 lock, poll_groups 보호 pg_lock) 파괴.
 *   3) endpoints TAILQ 를 안전하게(_SAFE) 순회하며 각 endpoint 제거 + 파괴.
 *   4) vu_transport free 후 완료 콜백 호출.
 * 실행 컨텍스트: NVMe-oF 코어의 종료 경로 (main/init thread). 이 시점에는 모든
 *   poll group 과 qpair 가 이미 정리되어 있다고 가정 (코어가 보장) — 그래서 동기적
 *   파괴가 안전하고 cb_fn 도 즉시(동기) 호출한다.
 * 호출자: spdk_nvmf_transport_destroy → vtable .destroy 디스패치.
 * 에러 경로: 항상 성공 — 부분 NULL 자원은 destroy_endpoint 내부 가드로 처리.
 *
 * 호출 체인:
 *   spdk_nvmf_transport_destroy → [nvmf_vfio_user_destroy]
 *     → nvmf_vfio_user_destroy_endpoint (endpoint 마다) → cb_fn(cb_arg)
 */
static void
nvmf_vfio_user_destroy(struct spdk_nvmf_transport *transport,
		       spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct nvmf_vfio_user_transport *vu_transport;        /* [한국어] container_of 로 복원할 실제 transport. */
	struct nvmf_vfio_user_endpoint *endpoint, *tmp;       /* [한국어] TAILQ_FOREACH_SAFE 용 현재/다음 노드. */

	SPDK_DEBUGLOG(nvmf_vfio, "destroy transport\n");  /* [한국어] transport 파괴 진입 로그. */

	/* [한국어] 코어가 넘긴 generic transport 포인터를 vfio-user 전용 구조체로 역참조.
	 * transport 는 nvmf_vfio_user_transport 의 첫 멤버가 아닐 수 있어 CONTAINEROF 필요. */
	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);

	/* [한국어] endpoints TAILQ 를 보호하던 transport 레벨 mutex 파괴 (이제 동시 접근 없음). */
	pthread_mutex_destroy(&vu_transport->lock);
	/* [한국어] poll_groups TAILQ 를 보호하던 별도 mutex 파괴 (SQ 분산 정책에서 사용). */
	pthread_mutex_destroy(&vu_transport->pg_lock);

	/* [한국어] 모든 endpoint 를 순회하며 정리. _SAFE 변형은 endpoint 가 루프 안에서
	 * free 되어도 tmp 에 다음 노드를 미리 저장해 두므로 안전. */
	TAILQ_FOREACH_SAFE(endpoint, &vu_transport->endpoints, link, tmp) {
		/* [한국어] 리스트에서 먼저 떼어낸 뒤 (다른 코드가 못 보게) 파괴. */
		TAILQ_REMOVE(&vu_transport->endpoints, endpoint, link);
		nvmf_vfio_user_destroy_endpoint(endpoint);  /* [한국어] vfu_ctx/BAR/소켓 등 endpoint 자원 일괄 해제. */
	}

	free(vu_transport);  /* [한국어] transport 컨테이너 구조체 자체 해제. */

	/* [한국어] 코어가 완료 콜백을 넘겼으면 동기적으로 호출 — 파괴가 끝났음을 통지. */
	if (cb_fn) {
		cb_fn(cb_arg);  /* [한국어] 같은 thread 에서 즉시 실행 (async 대기 없음). */
	}
}

/* [한국어] vfio-user transport 의 transport_specific JSON 옵션 디코더 테이블.
 * nvmf_create_transport RPC 의 "params.<name>" JSON 객체를 nvmf_vfio_user_transport
 * 의 transport_opts 필드들로 매핑한다. 각 엔트리는 {JSON 키, 구조체 내 오프셋,
 * 디코더 함수, optional 여부}. 마지막 true 는 "optional" 이므로 키가 없어도 에러 아님.
 * 읽는 자: nvmf_vfio_user_create() 가 spdk_json_decode_object_relaxed 로 사용.
 * 동기화: const read-only 전역 테이블 — 락 불필요. */
static const struct spdk_json_object_decoder vfio_user_transport_opts_decoder[] = {
	{
		/* [한국어] mappable BAR0 비활성 — true 면 BAR0 를 VM 에 직접 mmap 시키지 않고
		 * doorbell write 마다 vfio-user 메시지로 trap (interrupt 모드 전제 조건). */
		"disable_mappable_bar0",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.disable_mappable_bar0),
		spdk_json_decode_bool, true
	},
	{
		/* [한국어] adaptive IRQ coalescing 비활성 — IRQ 합치기 휴리스틱을 끈다.
		 * interrupt 모드에서는 SQ poller 보장이 없어 자동으로 강제 비활성됨. */
		"disable_adaptive_irq",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.disable_adaptive_irq),
		spdk_json_decode_bool, true
	},
	{
		/* [한국어] shadow doorbell 비활성 — NVMe 1.3 Doorbell Buffer Config 최적화를 끈다.
		 * mappable BAR0 가 켜져 있으면 의미 없으므로 자동 true 로 강제됨. */
		"disable_shadow_doorbells",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.disable_shadow_doorbells),
		spdk_json_decode_bool, true
	},
	{
		/* [한국어] Compare 명령 비활성 — deprecated 옵션 (아래 DEPRECATION_REGISTER 참조). */
		"disable_compare",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.disable_compare),
		spdk_json_decode_bool, true
	},
	{
		/* [한국어] interrupt 모드에서 SQ 들을 여러 poll group 에 분산(spreading) 시킬지.
		 * 단일 poll group 병목을 줄이기 위한 옵션. */
		"enable_intr_mode_sq_spreading",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.enable_intr_mode_sq_spreading),
		spdk_json_decode_bool, true
	},
};

/* [한국어] disable_compare 옵션의 deprecation 등록 — v26.05 부터 제거 예정.
 * 사용 시 24시간마다 1회 경고 로그를 띄우도록 SPDK deprecation 프레임워크에 등록.
 * nvmf_vfio_user_create() 가 옵션이 켜져 있으면 SPDK_LOG_DEPRECATED(disable_compare) 호출. */
SPDK_LOG_DEPRECATION_REGISTER(disable_compare, "", "v26.05", SPDK_LOG_DEPRECATION_EVERY_24H);

/*
 * [한국어]
 * nvmf_vfio_user_create - vfio-user transport 객체를 생성·초기화 (.create vtable 콜백).
 *
 * @opts: NVMe-oF 코어가 채운 공통 transport 옵션 (max_qpairs_per_ctrlr, max_io_size 등)
 *   + transport_specific (이 transport 전용 JSON 옵션 블록).
 * @return: 성공 시 spdk_nvmf_transport* (vu_transport->transport 멤버 주소),
 *   실패 시 NULL. 코어는 NULL 이면 transport 생성을 포기한다.
 *
 * 동기/배경: nvmf_create_transport RPC 또는 config replay 시 코어가 trtype="vfio_user"
 *   에 대해 이 .create 를 호출한다. 여기서 transport 컨테이너 alloc + 두 mutex 초기화 +
 *   endpoints/poll_groups 리스트 초기화 + JSON 전용 옵션 디코딩 + interrupt 모드/shadow
 *   doorbell 가능 여부 결정을 수행한다.
 * 동작 단계:
 *   1) max_qpairs_per_ctrlr 한도 검증 (NVMe spec 5.21.1.7).
 *   2) vu_transport zero-alloc + lock/pg_lock 초기화 + 두 TAILQ INIT.
 *   3) transport_specific JSON 을 옵션 디코더로 파싱 (relaxed = 미지 키 허용).
 *   4) disable_mappable_bar0 로부터 intr_mode_supported 도출, shadow doorbell 정합화.
 *   5) interrupt 모드면 capability 검증 + adaptive IRQ 강제 비활성.
 * 실행 컨텍스트: NVMe-oF 코어의 init thread (단일). polling 시작 전이라 동기화 단순.
 * 호출자: spdk_nvmf_transport_create → vtable .create 디스패치.
 * 에러 경로: err: (mutex 실패 시 free 만), cleanup: (디코드/검증 실패 시 두 mutex
 *   destroy + free) — 둘 다 NULL 반환.
 *
 * 호출 체인:
 *   spdk_nvmf_transport_create → [nvmf_vfio_user_create]
 *     → calloc / pthread_mutex_init / spdk_json_decode_object_relaxed
 */
static struct spdk_nvmf_transport *
nvmf_vfio_user_create(struct spdk_nvmf_transport_opts *opts)
{
	struct nvmf_vfio_user_transport *vu_transport;  /* [한국어] 생성할 transport 컨테이너. */
	int err;                                        /* [한국어] mutex_init/JSON decode 반환 코드. */

	/* [한국어] 요청된 컨트롤러당 qpair 수가 하드 한도(512)를 넘으면 거부. NVMe spec 5.21.1.7. */
	if (opts->max_qpairs_per_ctrlr > NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR) {
		SPDK_ERRLOG("Invalid max_qpairs_per_ctrlr=%d, supported max_qpairs_per_ctrlr=%d\n",
			    opts->max_qpairs_per_ctrlr, NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR);
		return NULL;  /* [한국어] 한도 초과 — 코어에 생성 실패 통지. */
	}

	/* [한국어] transport 컨테이너 zero-alloc (calloc 으로 모든 필드 0 초기화). */
	vu_transport = calloc(1, sizeof(*vu_transport));
	if (vu_transport == NULL) {
		SPDK_ERRLOG("Transport alloc fail: %m\n");  /* [한국어] %m = errno 문자열 (OOM 등). */
		return NULL;
	}

	/* [한국어] endpoints TAILQ 를 보호할 transport 레벨 mutex 초기화. */
	err = pthread_mutex_init(&vu_transport->lock, NULL);
	if (err != 0) {
		SPDK_ERRLOG("Pthread initialisation failed (%d)\n", err);
		goto err;  /* [한국어] mutex 미초기화 상태 — free 만 하고 반환. */
	}
	TAILQ_INIT(&vu_transport->endpoints);  /* [한국어] VM 별 endpoint 리스트 빈 상태로 시작. */

	/* [한국어] poll_groups TAILQ 를 보호할 별도 mutex 초기화 (SQ spreading 정책에서 사용). */
	err = pthread_mutex_init(&vu_transport->pg_lock, NULL);
	if (err != 0) {
		pthread_mutex_destroy(&vu_transport->lock);  /* [한국어] 앞서 만든 lock 은 되돌린다. */
		SPDK_ERRLOG("Pthread initialisation failed (%d)\n", err);
		goto err;
	}
	TAILQ_INIT(&vu_transport->poll_groups);  /* [한국어] poll group 리스트 빈 상태로 시작. */

	/* [한국어] transport_specific JSON 이 있으면 전용 옵션을 디코드. relaxed 변형은
	 * 디코더 테이블에 없는 키가 있어도 에러를 내지 않는다 (다른 transport 와 공유 JSON 대비). */
	if (opts->transport_specific != NULL &&
	    spdk_json_decode_object_relaxed(opts->transport_specific, vfio_user_transport_opts_decoder,
					    SPDK_COUNTOF(vfio_user_transport_opts_decoder),
					    vu_transport)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		goto cleanup;  /* [한국어] JSON 파싱 실패 — 두 mutex 까지 정리 후 반환. */
	}

	/* [한국어] deprecated 옵션 disable_compare 가 켜졌으면 rate-limit 경고 로그. */
	if (vu_transport->transport_opts.disable_compare) {
		SPDK_LOG_DEPRECATED(disable_compare);  /* [한국어] 24시간마다 1회 경고 (위 REGISTER 참조). */
	}

	/*
	 * To support interrupt mode, the transport must be configured with
	 * mappable BAR0 disabled: we need a vfio-user message to wake us up
	 * when a client writes new doorbell values to BAR0, via the
	 * libvfio-user socket fd.
	 */
	/* [한국어] interrupt 모드는 "BAR0 비매핑" 일 때만 가능. doorbell write 를 VM 이 직접
	 * mmap 한 BAR0 에 쓰면 호스트가 깨어날 방법이 없기 때문 — vfio-user 소켓 메시지로
	 * trap 해야 epoll/eventfd 로 호스트를 깨울 수 있다. 따라서 가능 여부 = disable_mappable_bar0. */
	vu_transport->intr_mode_supported =
		vu_transport->transport_opts.disable_mappable_bar0;

	/*
	 * If BAR0 is mappable, it doesn't make sense to support shadow
	 * doorbells, so explicitly turn it off.
	 */
	/* [한국어] BAR0 가 매핑 가능하면 VM 이 doorbell 을 직접 쓰므로 shadow doorbell
	 * (host shared memory 의 doorbell mirror) 가 무의미 → 강제로 비활성. */
	if (!vu_transport->transport_opts.disable_mappable_bar0) {
		vu_transport->transport_opts.disable_shadow_doorbells = true;
	}

	/* [한국어] 이 프로세스가 interrupt 모드(폴링 대신 fd 기반 wakeup)로 동작하는지 질의. */
	if (spdk_interrupt_mode_is_enabled()) {
		/* [한국어] interrupt 모드인데 BAR0 매핑이 켜져 있으면 doorbell wakeup 불가 → 실패. */
		if (!vu_transport->intr_mode_supported) {
			SPDK_ERRLOG("interrupt mode not supported\n");
			goto cleanup;
		}

		/*
		 * If we are in interrupt mode, we cannot support adaptive IRQs,
		 * as there is no guarantee the SQ poller will run subsequently
		 * to send pending IRQs.
		 */
		/* [한국어] interrupt 모드에서는 SQ poller 가 뒤이어 실행된다는 보장이 없어
		 * pending IRQ 를 모았다가 나중에 보내는 adaptive IRQ 가 불가능 → 강제 비활성. */
		vu_transport->transport_opts.disable_adaptive_irq = true;
	}

	/* [한국어] 최종 결정된 세 옵션을 디버그 로그로 남긴다 (운영 진단용). */
	SPDK_DEBUGLOG(nvmf_vfio, "vfio_user transport: disable_mappable_bar0=%d\n",
		      vu_transport->transport_opts.disable_mappable_bar0);
	SPDK_DEBUGLOG(nvmf_vfio, "vfio_user transport: disable_adaptive_irq=%d\n",
		      vu_transport->transport_opts.disable_adaptive_irq);
	SPDK_DEBUGLOG(nvmf_vfio, "vfio_user transport: disable_shadow_doorbells=%d\n",
		      vu_transport->transport_opts.disable_shadow_doorbells);

	/* [한국어] 성공 — 코어에 generic transport 포인터(컨테이너의 transport 멤버)를 반환.
	 * 코어는 이 포인터를 transport list 에 등록하고 이후 vtable 콜백 호출 시 넘긴다. */
	return &vu_transport->transport;

cleanup:
	/* [한국어] JSON 디코드/검증 실패 경로 — 두 mutex 모두 파괴 후 free 로 fall-through. */
	pthread_mutex_destroy(&vu_transport->lock);
	pthread_mutex_destroy(&vu_transport->pg_lock);
err:
	/* [한국어] mutex 초기화 실패 경로 진입점 — 컨테이너만 free 하고 NULL 반환. */
	free(vu_transport);
	return NULL;
}

/*
 * [한국어]
 * max_queue_size - controller 가 지원하는 SQ/CQ 최대 엔트리 수 반환.
 *
 * @vu_ctrlr: 쿼리할 controller.
 * @return: NVMe CAP.MQES (0-based) + 1 = 실제 최대 큐 깊이.
 *
 * 동기/배경: NVMe spec § 3.1.1 CAP.MQES (Maximum Queue Entries Supported, 0-based).
 *   Create IO SQ/CQ 에서 qsize 검증 기준값, shadow doorbell page 크기 계산 기준으로 사용.
 * 동작: vcprop.cap.bits.mqes (컨트롤러 CAP 레지스터에서 NVMf 코어가 설정한 값) 에 +1.
 * 실행 컨텍스트: admin 명령 처리 경로 (controller thread).
 * 호출자: handle_create_io_sq, handle_create_io_cq, handle_doorbell_buffer_config 등.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   handle_create_io_sq → [max_queue_size]
 */
static uint32_t
max_queue_size(struct nvmf_vfio_user_ctrlr const *vu_ctrlr)
{
	assert(vu_ctrlr != NULL);
	assert(vu_ctrlr->ctrlr != NULL);

	/* [한국어] CAP.MQES 는 0-based 값 → 실제 엔트리 수는 +1. */
	return vu_ctrlr->ctrlr->vcprop.cap.bits.mqes + 1;
}

/*
 * [한국어]
 * doorbell_stride - controller 의 NVMe doorbell stride(DSTRD) 값 반환.
 *
 * @vu_ctrlr: 쿼리할 controller.
 * @return: CAP.DSTRD 값 (0 = stride 4B, 1 = 8B, ...; NVMe spec § 3.1.1).
 *
 * 동기/배경: NVMe spec § 3.1.1 — doorbell 간 간격 = (4 << DSTRD) bytes.
 *   NVMf 구현은 DSTRD=0 을 advertise (최소 stride). shadow doorbell 페이지 크기 계산:
 *   (4 << dstrd) * MAX_QPAIRS * 2 ≤ PAGE_SIZE 여야 shadow buffer 1 페이지에 들어감.
 * 동작: vcprop.cap.bits.dstrd 직접 반환.
 * 실행 컨텍스트: admin 명령 처리 경로 (controller thread).
 * 호출자: handle_doorbell_buffer_config, queue_index 관련 검증 코드.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   handle_doorbell_buffer_config → [doorbell_stride]
 */
static uint32_t
doorbell_stride(const struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	assert(vu_ctrlr != NULL);
	assert(vu_ctrlr->ctrlr != NULL);

	/* [한국어] NVMf 는 DSTRD=0 advertise — doorbell 간격 4B. */
	return vu_ctrlr->ctrlr->vcprop.cap.bits.dstrd;
}

/*
 * [한국어]
 * memory_page_size - VM 이 CC.MPS 로 설정한 memory page 크기(byte) 반환.
 *
 * @vu_ctrlr: 쿼리할 controller.
 * @return: page size in bytes (최소 4096 = 1<<12).
 *
 * 동기/배경: NVMe spec § 3.1.5 CC.MPS(Memory Page Size) — VM 이 CC.EN 전에 설정.
 *   MPS=0 → 4KB, MPS=1 → 8KB, ... MPS+12 = page shift.
 *   shadow doorbell buffer, eventidx buffer 는 각 1 page → 이 크기가 DMA 매핑 단위.
 * 동작: cc.bits.mps + 12 = page shift, 1<<shift = size.
 * 실행 컨텍스트: admin 명령 처리 경로.
 * 호출자: handle_doorbell_buffer_config (page alignment 검증 + DMA 매핑 크기 결정).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   handle_doorbell_buffer_config → [memory_page_size]
 */
static uintptr_t
memory_page_size(const struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	/* [한국어] CC.MPS + 12 = page shift (NVMe spec § 3.1.5). */
	uint32_t memory_page_shift = vu_ctrlr->ctrlr->vcprop.cc.bits.mps + 12;
	return 1ul << memory_page_shift;
}

/*
 * [한국어]
 * memory_page_mask - VM 의 page alignment mask 반환 (page 내 오프셋 클리어용).
 *
 * @ctrlr: 쿼리할 controller.
 * @return: ~(page_size - 1) — 비트 AND 로 page base 주소 추출.
 *
 * 동기/배경: shadow doorbell buffer / eventidx buffer PRP 가 page-aligned 인지
 *   검증할 때 사용. (prp & ~mask) == 0 이면 정렬됨.
 * 동작: memory_page_size 의 보수 - 1.
 * 실행 컨텍스트: admin 명령 처리.
 * 호출자: handle_doorbell_buffer_config (PRP 정렬 검증).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   handle_doorbell_buffer_config → [memory_page_mask] → memory_page_size
 */
static uintptr_t
memory_page_mask(const struct nvmf_vfio_user_ctrlr *ctrlr)
{
	/* [한국어] page boundary mask — PRP & ~mask == 0 이면 page-aligned. */
	return ~(memory_page_size(ctrlr) - 1);
}

/*
 * [한국어]
 * map_q - NVMe 큐(SQ 또는 CQ) 메모리를 VM IOVA → host VA 로 매핑.
 *
 * @vu_ctrlr: DMA 매핑을 수행할 controller (vfu_ctx 보유).
 * @mapping: 매핑 결과를 저장할 nvme_q_mapping 구조체 (prp1/len 미리 채워져 있음).
 * @flags: MAP_R(읽기), MAP_RW(읽쓰기), MAP_INITIALIZE(memset 0), MAP_QUIET(에러 로그 억제).
 * @return: 0 성공, -EFAULT 매핑 실패.
 *
 * 동기/배경: VM 이 Create IO SQ/CQ 명령으로 큐 메모리 GPA(PRP1)를 알려주면,
 *   SPDK 가 그 GPA 를 vfu_addr_to_sgl + vfu_sgl_get 으로 host 가상주소로 변환한다.
 *   이 host VA 로 SQE/CQE 를 직접 읽고 쓴다 (DMA 없이 메모리 접근).
 * 동작 단계:
 *   1. map_one(vfu_ctx, prp1, len, sg, iov, flags) — 단일 DMA 페이지 변환.
 *   2. MAP_INITIALIZE 플래그 있으면 큐 메모리를 0 으로 초기화 (SQE 잔여 제거).
 * 실행 컨텍스트: admin 명령 처리 경로 (controller thread).
 * 호출자: asq_setup, acq_setup, handle_create_io_sq, handle_create_io_cq, memory_region_add_cb.
 * 에러 경로: map_one 실패 → -EFAULT (DMA 테이블에 없는 GPA 등).
 *
 * 호출 체인:
 *   handle_create_io_sq → [map_q] → map_one → vfu_sgl_get
 */
static int
map_q(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvme_q_mapping *mapping,
      uint32_t flags)
{
	void *ret;

	assert(mapping->len != 0);
	assert(q_addr(mapping) == NULL);  /* [한국어] 이미 매핑됐으면 재진입 버그 — 방지. */

	/* [한국어] GPA(mapping->prp1) → host VA 변환 + dma_sg 저장. */
	ret = map_one(vu_ctrlr->endpoint->vfu_ctx, mapping->prp1, mapping->len,
		      mapping->sg, &mapping->iov, flags);
	if (ret == NULL) {
		return -EFAULT;  /* [한국어] GPA 가 DMA 테이블에 없거나 권한 불일치. */
	}

	/* [한국어] MAP_INITIALIZE: SQE/CQE 잔여 없이 깨끗하게 시작 — 특히 CQ 는 phase 비트 오염 방지. */
	if (flags & MAP_INITIALIZE) {
		memset(q_addr(mapping), 0, mapping->len);
	}

	return 0;
}

/*
 * [한국어]
 * unmap_q - NVMe 큐 메모리의 host VA 매핑을 해제 (DMA 언매핑).
 *
 * @vu_ctrlr: vfu_ctx 보유 controller.
 * @mapping: 해제할 매핑 정보 (sg, iov 보유).
 *
 * 동기/배경: SQ/CQ 가 삭제(delete) 되거나 controller 가 reset 될 때 매핑을 해제해야
 *   한다. libvfio-user 의 vfu_sgl_put 이 DMA reference count 를 감소시키고, 더이상
 *   이 GPA 영역에 접근하지 않음을 libvfio-user 에 통보한다.
 * 동작: iov_base 가 유효(NULL 아님)한 경우에만 vfu_sgl_put 호출 후 iov_base=NULL 으로
 *   클리어 (이중 해제 방지 센티넬).
 * 실행 컨텍스트: admin 명령 처리 / controller destroy 경로.
 * 호출자: delete_sq_done, delete_cq_done, free_qp, disable_ctrlr, memory_region_remove_cb.
 * 에러 경로: 없음 (매핑 없으면 no-op).
 *
 * 호출 체인:
 *   delete_sq_done → [unmap_q] → vfu_sgl_put
 */
static inline void
unmap_q(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvme_q_mapping *mapping)
{
	/* [한국어] iov_base == NULL 이면 이미 해제됐거나 미매핑 상태 — no-op. */
	if (q_addr(mapping) != NULL) {
		/* [한국어] DMA reference 해제 — libvfio-user 가 GPA 영역 접근 추적 종료. */
		vfu_sgl_put(vu_ctrlr->endpoint->vfu_ctx, mapping->sg,
			    &mapping->iov, 1);
		mapping->iov.iov_base = NULL;  /* [한국어] 이중 해제 방지 센티넬. */
	}
}

/*
 * [한국어]
 * asq_setup - Admin Submission Queue (SQ0) 를 NVMe CC.EN 시점에 초기화.
 *
 * @ctrlr: controller.
 * @return: 0 성공, -EFAULT 매핑 실패.
 *
 * 동기/배경: VM 이 CC.EN=1 write(enable_ctrlr 호출) 시 NVMe spec § 3.5.3 에 따라
 *   AQA/ASQ/ACQ 레지스터를 통해 admin queue 위치와 크기를 알려준다.
 *   본 함수가 ASQ 레지스터에서 GPA/크기를 읽어 DMA 매핑한다.
 * 동작 단계:
 *   1. AQA.ASQS (0-based) + 1 = SQ 크기.
 *   2. ASQ 레지스터 = admin SQ 메모리 GPA (PRP1).
 *   3. map_q(MAP_INITIALIZE) — 매핑 + zero-init (잔여 SQE 제거).
 *   4. admin SQ 는 shadow doorbell 미사용 — bar0_doorbells[0] 직접 참조.
 *   5. doorbell tail 을 0 으로 클리어.
 * 주의: ASQ==0 도 유효한 GPA 일 수 있으므로 NULL 검사 불가.
 * 실행 컨텍스트: enable_ctrlr 콜백 경로 (controller thread).
 * 호출자: enable_ctrlr.
 * 에러 경로: map_q 실패 → -EFAULT.
 *
 * 호출 체인:
 *   enable_ctrlr → [asq_setup] → map_q → map_one → vfu_sgl_get
 */
static int
asq_setup(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_sq *sq;
	const struct spdk_nvmf_registers *regs;
	int ret;

	assert(ctrlr != NULL);

	sq = ctrlr->sqs[0];  /* [한국어] admin SQ 는 항상 sqs[0]. */

	assert(sq != NULL);
	assert(q_addr(&sq->mapping) == NULL);  /* [한국어] 아직 매핑 안 됐어야 함. */
	/* XXX ctrlr->asq == 0 is a valid memory address */

	regs = spdk_nvmf_ctrlr_get_regs(ctrlr->ctrlr);  /* [한국어] NVMe register space (AQA/ASQ/ACQ 포함). */
	sq->qid = 0;  /* [한국어] admin SQ 는 qid=0. */
	sq->size = regs->aqa.bits.asqs + 1;  /* [한국어] AQA.ASQS 는 0-based → +1. */
	sq->mapping.prp1 = regs->asq;  /* [한국어] ASQ 레지스터 = admin SQ 의 GPA. */
	sq->mapping.len = sq->size * sizeof(struct spdk_nvme_cmd);  /* [한국어] SQE 크기 × 엔트리 수. */
	*sq_headp(sq) = 0;  /* [한국어] SPDK 측 head pointer 초기화. */
	sq->cqid = 0;  /* [한국어] admin SQ 는 admin CQ(0)와 짝. */

	/* [한국어] admin SQ GPA → host VA 매핑 + zero-init (잔여 SQE 제거). */
	ret = map_q(ctrlr, &sq->mapping, MAP_INITIALIZE);
	if (ret) {
		return ret;
	}

	/* The Admin queue (qid: 0) does not ever use shadow doorbells. */
	/* [한국어] admin SQ 는 shadow doorbell 미사용 — BAR0 doorbell [0] 직접 참조. */
	sq->dbl_tailp = ctrlr->bar0_doorbells + queue_index(0, false);

	*sq_dbl_tailp(sq) = 0;  /* [한국어] VM 측 tail doorbell 초기화. */

	return 0;
}

/*
 * Updates eventidx to set an SQ into interrupt or polling mode.
 *
 * Returns false if the current SQ tail does not match the SQ head, as
 * this means that the host has submitted more items to the queue while we were
 * not looking - or during the event index update. In that case, we must retry,
 * or otherwise make sure we are going to wake up again.
 */
/*
 * [한국어]
 * set_sq_eventidx - shadow doorbell 의 eventidx 를 갱신해 SQ 를 notify 모드로 전환.
 *
 * @sq: eventidx 를 재설정할 SQ (need_rearm == true 인 상태로 진입).
 * @return: true = 재무장 성공(레이스 없음), false = 레이스 발생(재시도 필요).
 *
 * 동기/배경: shadow doorbell 패턴(NVMe 1.3) 에서 VM 은 eventidx 와 자신의 tail 을 비교해
 *   "SPDK 에게 알림이 필요한지" 판단한다. SPDK 가 eventidx 를 갱신하지 않으면 VM 은 항상
 *   알림을 보내고, 갱신하면 "내가 마지막 본 tail 이후부터" 알림이 온다. 문제는 eventidx
 *   쓰기와 tail 재조회 사이에 VM 이 tail 을 갱신하면 알림이 영원히 안 올 수 있는 레이스다.
 *
 * 레이스 안전 알고리즘 (set_sq_eventidx 의 핵심):
 *   1. 현재 tail 읽기 (old_tail).
 *   2. eventidx = old_tail 기록.
 *   3. ★ 메모리 배리어(spdk_mb) — eventidx 쓰기가 tail 재조회 전에 완료되도록.
 *      VM 은 tail 쓰기 후 wmb() 해야 하고, SPDK 는 eventidx 쓰기 후 rmb() 해야
 *      상호 가시성 보장 (store-release / load-acquire 쌍).
 *   4. tail 재조회 (new_tail).
 *   5. new_tail == head 이면 레이스 없음 — true 반환.
 *   6. new_tail != head 이면 VM 이 사이에 tail 을 갱신 — false 반환(호출자가 재폴링+재시도).
 *
 * polled 모드: eventidx = NVMF_VFIO_USER_EVENTIDX_POLL (UINT32_MAX) — "항상 poll, 알림 불필요".
 *
 * 실행 컨텍스트: vfio_user_sq_rearm 에서 호출 (interrupt 모드 전용, polled 모드에서도 호출되나 즉시 반환).
 * 호출자: vfio_user_sq_rearm (최대 NVMF_VFIO_USER_SET_EVENTIDX_MAX_ATTEMPTS 번 시도).
 * 에러 경로: 없음 (bool 반환 — 호출자가 재시도 루프).
 *
 * 호출 체인:
 *   vfio_user_poll_group_rearm → vfio_user_sq_rearm → [set_sq_eventidx]
 */
static bool
set_sq_eventidx(struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_ctrlr *ctrlr;
	volatile uint32_t *sq_tail_eidx;
	uint32_t old_tail, new_tail;

	assert(sq != NULL);
	assert(sq->ctrlr != NULL);
	assert(sq->ctrlr->sdbl != NULL);
	assert(sq->need_rearm);
	assert(sq->qid != 0);

	ctrlr = sq->ctrlr;

	SPDK_DEBUGLOG(vfio_user_db, "%s: updating eventidx of sqid:%u\n",
		      ctrlr_id(ctrlr), sq->qid);

	sq_tail_eidx = ctrlr->sdbl->eventidxs + queue_index(sq->qid, false);

	assert(ctrlr->endpoint != NULL);

	if (!ctrlr->endpoint->interrupt_mode) {
		/* No synchronisation necessary. */
		*sq_tail_eidx = NVMF_VFIO_USER_EVENTIDX_POLL;
		return true;
	}

	old_tail = *sq_dbl_tailp(sq);
	*sq_tail_eidx = old_tail;

	/*
	 * Ensure that the event index is updated before re-reading the tail
	 * doorbell. If it's not, then the host might race us and update the
	 * tail after the second read but before the event index is written, so
	 * it won't write to BAR0 and we'll miss the update.
	 *
	 * The driver should provide similar ordering with an mb().
	 */
	spdk_mb();

	/*
	 * Check if the host has updated the tail doorbell after we've read it
	 * for the first time, but before the event index was written. If that's
	 * the case, then we've lost the race and we need to update the event
	 * index again (after polling the queue, since the host won't write to
	 * BAR0).
	 */
	new_tail = *sq_dbl_tailp(sq);

	/*
	 * We might poll the queue straight after this function returns if the
	 * tail has been updated, so we need to ensure that any changes to the
	 * queue will be visible to us if the doorbell has been updated.
	 *
	 * The driver should provide similar ordering with a wmb() to ensure
	 * that the queue is written before it updates the tail doorbell.
	 */
	spdk_rmb();

	SPDK_DEBUGLOG(vfio_user_db, "%s: sqid:%u, old_tail=%u, new_tail=%u, "
		      "sq_head=%u\n", ctrlr_id(ctrlr), sq->qid, old_tail,
		      new_tail, *sq_headp(sq));

	if (new_tail == *sq_headp(sq)) {
		sq->need_rearm = false;
		return true;
	}

	/*
	 * We've lost the race: the tail was updated since we last polled,
	 * including if it happened within this routine.
	 *
	 * The caller should retry after polling (think of this as a cmpxchg
	 * loop); if we go to sleep while the SQ is not empty, then we won't
	 * process the remaining events.
	 */
	return false;
}

static int nvmf_vfio_user_sq_poll(struct nvmf_vfio_user_sq *sq);

/*
 * Arrange for an SQ to interrupt us if written. Returns non-zero if we
 * processed some SQ entries.
 */
/*
 * [한국어]
 * vfio_user_sq_rearm - 단일 SQ 의 shadow doorbell eventidx 재무장 (레이스 재시도 포함).
 *
 * @ctrlr: controller.
 * @sq: 재무장할 SQ (need_rearm == true 인 상태).
 * @vu_group: 통계 갱신 및 need_kick 설정 대상 PG.
 * @return: 이 과정에서 처리한 SQE 수 (>0 = 새 I/O 발견, 0 = 조용).
 *
 * 동기/배경: interrupt 모드에서 SPDK 가 sleep 하기 전에 set_sq_eventidx 레이스를 이겨야
 *   한다. 한 번에 안 되면 SQE 를 처리(sq_poll)하고 다시 시도(레이스 재시도 루프).
 *   최대 NVMF_VFIO_USER_SET_EVENTIDX_MAX_ATTEMPTS 번 시도 후에도 못 이기면 need_kick=true
 *   세팅 → 강제 wake-up 보장.
 *
 * 동작 단계 (루프):
 *   1. set_sq_eventidx — 성공(레이스 없음) 시 won++ + return count.
 *   2. 실패(레이스) 시 nvmf_vfio_user_sq_poll 로 SQE 처리(레이스 해소).
 *   3. MAX_ATTEMPTS 초과 → need_kick=true + lost/lost_count 통계 + return.
 *
 * 실행 컨텍스트: PG 의 reactor thread (interrupt 모드 전용).
 * 호출자: vfio_user_poll_group_rearm.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   vfio_user_poll_group_rearm → [vfio_user_sq_rearm]
 *     → set_sq_eventidx (레이스 경쟁)
 *     → nvmf_vfio_user_sq_poll (레이스 발생 시 SQE 처리)
 */
static int
vfio_user_sq_rearm(struct nvmf_vfio_user_ctrlr *ctrlr,
		   struct nvmf_vfio_user_sq *sq,
		   struct nvmf_vfio_user_poll_group *vu_group)
{
	int count = 0;
	size_t i;

	assert(sq->need_rearm);

	for (i = 0; i < NVMF_VFIO_USER_SET_EVENTIDX_MAX_ATTEMPTS; i++) {
		int ret;

		if (set_sq_eventidx(sq)) {
			/* We won the race and set eventidx; done. */
			vu_group->stats.won++;
			return count;
		}

		ret = nvmf_vfio_user_sq_poll(sq);

		count += (ret < 0) ? 1 : ret;
	}

	/*
	 * We couldn't arrange an eventidx guaranteed to cause a BAR0 write, as
	 * we raced with the producer too many times; force ourselves to wake up
	 * instead. We'll process all queues at that point.
	 */
	vu_group->need_kick = true;

	SPDK_DEBUGLOG(vfio_user_db,
		      "%s: set_sq_eventidx() lost the race %zu times\n",
		      ctrlr_id(ctrlr), i);

	vu_group->stats.lost++;
	vu_group->stats.lost_count += count;

	return count;
}

/*
 * We're in interrupt mode, and potentially about to go to sleep. We need to
 * make sure any further I/O submissions are guaranteed to wake us up: for
 * shadow doorbells that means we may need to go through set_sq_eventidx() for
 * every SQ that needs re-arming.
 *
 * Returns non-zero if we processed something.
 */
/*
 * [한국어]
 * vfio_user_poll_group_rearm - PG 의 모든 ACTIVE SQ 에 대해 eventidx 재무장.
 *
 * @vu_group: 재무장할 PG.
 * @return: 이 과정에서 처리한 총 SQE 수.
 *
 * 동기/배경: interrupt 모드에서 SPDK 가 sleep 하기 전에, 이 PG 의 모든 need_rearm SQ 에
 *   대해 eventidx 를 재설정해 다음 doorbell write 가 wake-up 을 트리거하게 해야 한다.
 *   rearm 을 빼먹으면 VM 이 doorbell write 를 해도 SPDK 가 깨어나지 않아 I/O 가 멈춘다.
 * 동작 단계:
 *   1. rearms 통계 증가.
 *   2. PG 의 sqs TAILQ 를 순회하며 ACTIVE + size>0 + need_rearm 인 SQ 마다 vfio_user_sq_rearm.
 *   3. need_kick 이 세워졌으면 poll_group_kick 으로 강제 wakeup.
 * 실행 컨텍스트: PG 소유 reactor (interrupt 모드 전용).
 * 호출자: vfio_user_poll_group_process (interrupt wakeup 후), vfio_user_ctrlr_set_intr_mode (mode 전환 시).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   vfio_user_poll_group_process → [vfio_user_poll_group_rearm]
 *     → vfio_user_sq_rearm (SQ별) → set_sq_eventidx
 */
static int
vfio_user_poll_group_rearm(struct nvmf_vfio_user_poll_group *vu_group)
{
	struct nvmf_vfio_user_sq *sq;
	int count = 0;

	vu_group->stats.rearms++;

	TAILQ_FOREACH(sq, &vu_group->sqs, link) {
		if (spdk_unlikely(sq->sq_state != VFIO_USER_SQ_ACTIVE || !sq->size)) {
			continue;
		}

		if (sq->need_rearm) {
			count += vfio_user_sq_rearm(sq->ctrlr, sq, vu_group);
		}
	}

	if (vu_group->need_kick) {
		poll_group_kick(vu_group);
	}

	return count;
}

/*
 * [한국어]
 * acq_setup - Admin Completion Queue (CQ0) 를 NVMe CC.EN 시점에 초기화.
 *
 * @ctrlr: controller.
 * @return: 0 성공, -EFAULT 매핑 실패.
 *
 * 동기/배경: VM 이 CC.EN=1(enable_ctrlr) 시 ACQ 레지스터에 admin CQ GPA/크기를 알려준다.
 *   본 함수가 ACQ 레지스터에서 읽어 DMA 매핑하고, phase/ien/nr_outstanding 을 초기화한다.
 *   asq_setup 과 쌍이 되며, enable_ctrlr 에서 acq → asq 순서로 호출된다.
 * 동작 단계:
 *   1. AQA.ACQS (0-based) + 1 = CQ 크기.
 *   2. ACQ 레지스터 = admin CQ GPA (PRP1).
 *   3. phase = true (초기 phase bit, NVMe spec § 4.6.3 — wrap 때마다 토글).
 *   4. ien = true (admin CQ 는 항상 interrupt enabled).
 *   5. nr_outstanding = 0 (in-flight 없음).
 *   6. map_q(MAP_RW|MAP_INITIALIZE) — 읽쓰기 매핑 + zero-init (CQE 잔여 제거).
 *   7. admin CQ 는 shadow doorbell 미사용 — bar0_doorbells[1] 참조.
 * 실행 컨텍스트: enable_ctrlr 콜백 경로 (controller thread).
 * 호출자: enable_ctrlr.
 * 에러 경로: map_q 실패 → -EFAULT.
 *
 * 호출 체인:
 *   enable_ctrlr → [acq_setup] → map_q → map_one → vfu_sgl_get
 */
static int
acq_setup(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_cq *cq;
	const struct spdk_nvmf_registers *regs;
	int ret;

	assert(ctrlr != NULL);

	cq = ctrlr->cqs[0];  /* [한국어] admin CQ 는 항상 cqs[0]. */

	assert(cq != NULL);

	assert(q_addr(&cq->mapping) == NULL);  /* [한국어] 미매핑 상태여야 함. */

	regs = spdk_nvmf_ctrlr_get_regs(ctrlr->ctrlr);  /* [한국어] AQA/ACQ 레지스터 포함 NVMe reg space. */
	assert(regs != NULL);
	cq->qid = 0;  /* [한국어] admin CQ 는 qid=0. */
	cq->size = regs->aqa.bits.acqs + 1;  /* [한국어] AQA.ACQS 0-based → +1. */
	cq->mapping.prp1 = regs->acq;  /* [한국어] ACQ 레지스터 = admin CQ 의 GPA. */
	cq->mapping.len = cq->size * sizeof(struct spdk_nvme_cpl);  /* [한국어] CQE 크기 × 엔트리 수. */
	*cq_tailp(cq) = 0;  /* [한국어] SPDK 측 tail pointer 초기화. */
	cq->ien = true;   /* [한국어] admin CQ 는 항상 interrupt enabled. */
	cq->phase = true;  /* [한국어] 초기 phase bit = 1 (첫 CQE 부터 phase=1 로 채움). */
	cq->nr_outstanding = 0;  /* [한국어] in-flight 없음. */

	/* [한국어] admin CQ GPA → host VA 읽쓰기 매핑 + zero-init (phase 비트 오염 방지). */
	ret = map_q(ctrlr, &cq->mapping, MAP_RW | MAP_INITIALIZE);
	if (ret) {
		return ret;
	}

	/* The Admin queue (qid: 0) does not ever use shadow doorbells. */
	/* [한국어] admin CQ 는 shadow doorbell 미사용 — BAR0 doorbell [1] (CQ0 head) 직접 참조. */
	cq->dbl_headp = ctrlr->bar0_doorbells + queue_index(0, true);

	*cq_dbl_headp(cq) = 0;  /* [한국어] VM 측 CQ head doorbell 초기화. */

	return 0;
}

/*
 * [한국어]
 * _map_one - nvme_map_cmd 에서 개별 PRP/SGL 주소를 host VA 로 변환하는 콜백.
 *
 * @prv: nvmf_request 포인터 (prv 인자로 넘겨받음 — sg/iov 누적 저장소).
 * @addr: 변환할 VM 의 GPA (PRP 또는 SGL element 주소).
 * @len: 이 segment 의 byte 길이.
 * @flags: MAP_R/MAP_RW 등.
 * @return: host 가상주소(성공) 또는 NULL(매핑 실패).
 *
 * 동기/배경: nvme_map_cmd 는 PRP1/PRP2 list, SGL chain 을 파싱하며, 각 page/segment 마다
 *   이 콜백을 호출해 GPA → host VA 변환을 위임한다. 변환된 iov 는 bdev I/O 의
 *   scatter-gather list 로 쓰인다.
 * 동작 단계:
 *   1. prv(req) → vu_req → sq → vfu_ctx 역참조.
 *   2. iovcnt 인덱스로 sg 슬롯 선택 (index_to_sg_t).
 *   3. map_one(vfu_ctx, addr, len, sg, iov) — GPA → host VA 변환.
 *   4. 성공 시 vu_req->iovcnt 증가.
 * 실행 컨텍스트: PRP/SGL 파싱 경로 (controller thread).
 * 호출자: nvme_map_cmd (PRP/SGL element 마다 콜백).
 * 에러 경로: map_one 실패 → NULL 반환 (nvme_map_cmd 가 에러 처리).
 *
 * 호출 체인:
 *   vfio_user_map_cmd → nvme_map_cmd → [_map_one] → map_one → vfu_sgl_get
 */
static void *
_map_one(void *prv, uint64_t addr, uint64_t len, uint32_t flags)
{
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)prv;  /* [한국어] nvme_map_cmd 가 prv 로 전달. */
	struct spdk_nvmf_qpair *qpair;
	struct nvmf_vfio_user_req *vu_req;
	struct nvmf_vfio_user_sq *sq;
	void *ret;

	assert(req != NULL);
	qpair = req->qpair;  /* [한국어] 이 요청이 속한 qpair → SQ → controller → vfu_ctx 경로. */
	vu_req = SPDK_CONTAINEROF(req, struct nvmf_vfio_user_req, req);  /* [한국어] req → vu_req (sg/iov 저장소). */
	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);  /* [한국어] qpair → SQ. */

	assert(vu_req->iovcnt < NVMF_VFIO_USER_MAX_IOVECS);  /* [한국어] iov 슬롯 한도 초과 방지. */
	/* [한국어] GPA → host VA: sg 슬롯 = index_to_sg_t(vu_req->sg, iovcnt), iov = &vu_req->iov[iovcnt]. */
	ret = map_one(sq->ctrlr->endpoint->vfu_ctx, addr, len,
		      index_to_sg_t(vu_req->sg, vu_req->iovcnt),
		      &vu_req->iov[vu_req->iovcnt], flags);
	if (spdk_likely(ret != NULL)) {
		vu_req->iovcnt++;  /* [한국어] 성공 시 다음 슬롯으로. */
	}
	return ret;
}

/*
 * [한국어]
 * vfio_user_map_cmd - NVMe SQE 의 PRP/SGL 을 host iov 로 변환 (상위 래퍼).
 *
 * @ctrlr: controller (현재 미사용 — 서명 일관성).
 * @req: nvmf_request (cmd 가 채워진 상태; sg/iov 결과 저장).
 * @iov: 결과 iov 배열 (req->iov 와 같은 포인터).
 * @length: 전체 데이터 길이 (byte).
 * @return: 생성된 iov 수 (>0 성공, <0 에러).
 *
 * 동기/배경: PRP1/PRP2 (NVMe 전통 방식) 또는 SGL (NVMe 1.2+) 을 파싱해
 *   host VA iov scatter-gather list 로 변환한다. 실제 파싱은 nvme_map_cmd 에 위임하고
 *   개별 주소 변환은 _map_one 콜백으로 처리한다.
 * 동작: nvme_map_cmd(req, cmd, iov, max_bufs=NVMF_REQ_MAX_BUFFERS, length, page_size=4096, _map_one).
 * 실행 컨텍스트: map_admin_cmd_req / map_io_cmd_req 경로 (controller thread).
 * 호출자: map_admin_cmd_req, map_io_cmd_req.
 * 에러 경로: nvme_map_cmd 내부 또는 _map_one 실패 → 음수 반환.
 *
 * 호출 체인:
 *   map_admin_cmd_req / map_io_cmd_req → [vfio_user_map_cmd] → nvme_map_cmd → _map_one
 */
static int
vfio_user_map_cmd(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvmf_request *req,
		  struct iovec *iov, uint32_t length)
{
	/* Map PRP list to from Guest physical memory to
	 * virtual memory address.
	 */
	/* [한국어] 4096 = 최소 페이지 크기 (PRP list 경계). NVMF_REQ_MAX_BUFFERS = iov 배열 최대 크기. */
	return nvme_map_cmd(req, &req->cmd->nvme_cmd, iov, NVMF_REQ_MAX_BUFFERS,
			    length, 4096, _map_one);
}

static int handle_cmd_req(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd,
			  struct nvmf_vfio_user_sq *sq);

/*
 * [한국어]
 * cq_free_slots - CQ 의 현재 사용 가능한 CQE 슬롯 수 반환 (cached last_head 기준).
 *
 * @cq: 쿼리할 CQ.
 * @return: 남은 free CQE 슬롯 수 (항상 ≥ 0; post_completion 직전에는 ≥ 1 이어야 함).
 *
 * 동기/배경: CQ 는 원형 큐 — SPDK 가 tail 을 전진(producer), VM 이 head 를 전진(consumer).
 *   SPDK 가 post_completion 하기 전에 남은 공간을 확인해 CQ overflow 를 방지한다.
 *   last_head 는 VM 의 CQ head doorbell 의 캐시 값 (읽기 비용 절약). cq_is_full 이
 *   overflow 직전에만 실제 doorbell 을 다시 읽는다.
 * 동작:
 *   - tail == last_head: 완전히 비어있음 → cq->size 슬롯 모두 사용 가능.
 *   - tail > last_head: tail 이 head 보다 앞 → size - (tail - head).
 *   - tail < last_head: wrap-around → head - tail.
 *   - -1: CQE post 후 tail 이 head 와 같아지면 "가득 찬 것처럼 보임" 방지 (원형 큐 관례).
 * 실행 컨텍스트: handle_sq_tdbl_write (SQE 처리 전 flow control 체크).
 * 호출자: cq_is_full, handle_sq_tdbl_write.
 * 에러 경로: 없음 (assert 로 free_slots > 0 검증).
 *
 * 호출 체인:
 *   handle_sq_tdbl_write → cq_is_full → [cq_free_slots]
 */
static uint32_t
cq_free_slots(struct nvmf_vfio_user_cq *cq)
{
	uint32_t free_slots;

	assert(cq != NULL);

	/* [한국어] tail == last_head: ring 이 완전히 비어있음. */
	if (cq->tail == cq->last_head) {
		free_slots = cq->size;
	} else if (cq->tail > cq->last_head) {
		/* [한국어] tail 이 head 보다 앞 (wrap-around 없음): size - gap. */
		free_slots = cq->size - (cq->tail - cq->last_head);
	} else {
		/* [한국어] tail < head: wrap-around 발생 — head - tail = 나머지 공간. */
		free_slots = cq->last_head - cq->tail;
	}
	assert(free_slots > 0);  /* [한국어] 원형 큐 관례: 최소 1슬롯은 항상 "비어 보임". */

	/* [한국어] -1: CQE 1개 post 후 tail==head 가 되면 "full" 로 잘못 판정 방지 (sentinel). */
	return free_slots - 1;
}

/*
 * Since reading the head doorbell is relatively expensive, we use the cached
 * value, so we only have to read it for real if it appears that we are full.
 */
/*
 * [한국어]
 * cq_is_full - CQ 가 가득 찼는지 확인 (cached last_head 우선, overflow 직전만 doorbell 재조회).
 *
 * @cq: 확인할 CQ.
 * @return: true = 남은 슬롯 없음(flow control 필요), false = 여유 있음.
 *
 * 동기/배경: CQ head doorbell (VM 이 write) 를 매 SQE 마다 읽으면 비용이 높다.
 *   캐시값(last_head)으로 먼저 확인하고, "꽉 찬 것 같다"고 판단될 때만 실제 doorbell read.
 *   이 두 단계 전략으로 head doorbell read 를 최소화한다.
 * 동작:
 *   1. cq_free_slots (last_head 캐시 기반) — 0이면 overhead 감수하고 doorbell 실제 read.
 *   2. last_head 갱신 + 재계산 → 그래도 0이면 true (실제로 꽉 참).
 * 실행 컨텍스트: handle_sq_tdbl_write 의 flow control 경로.
 * 호출자: handle_sq_tdbl_write (SQE 처리 전).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   handle_sq_tdbl_write → [cq_is_full] → cq_free_slots (→ *cq_dbl_headp if full)
 */
static inline bool
cq_is_full(struct nvmf_vfio_user_cq *cq)
{
	uint32_t free_cq_slots;

	assert(cq != NULL);

	free_cq_slots = cq_free_slots(cq);

	if (spdk_unlikely(free_cq_slots == 0)) {
		cq->last_head = *cq_dbl_headp(cq);
		free_cq_slots = cq_free_slots(cq);
	}

	return free_cq_slots == 0;
}

/*
 * Posts a CQE in the completion queue.
 *
 * @ctrlr: the vfio-user controller
 * @cq: the completion queue
 * @cdw0: cdw0 as reported by NVMf
 * @sqid: submission queue ID
 * @cid: command identifier in NVMe command
 * @sc: the NVMe CQE status code
 * @sct: the NVMe CQE status code type
 */
/*
 * [한국어]
 * post_completion - ★ CQE 1개를 VM 의 CQ 에 write + 필요 시 IRQ trigger.
 *
 * @ctrlr: controller.
 * @cq: 대상 CQ.
 * @cdw0: NVMe completion dword 0 (명령마다 의미 다름 — 보통 0, Create IO Q 응답에선 NSID 등).
 * @sqid/cid: 응답 짝이 되는 SQ id + command id.
 * @sc/sct: status code / status code type (0/0 = 성공).
 * @return: 0 성공, -1 CQ full.
 *
 * IO/admin 완료 시 본 함수가 CQE 16B 를 VM 의 CQ 메모리에 직접 write (DMA mapped iov →
 * memory store). VM 의 NVMe driver 는 CQ poll 또는 IRQ 로 받음.
 *
 * 단계:
 *   1. CQ unmapped (lazy) 면 no-op — race condition.
 *   2. admin CQ 면 thread affinity 검증 (admin queue 는 ctrlr thread 전용).
 *   3. CQ full 검사 — flow control 은 SQ tail doorbell side 에서 처리하므로 여기 도달 시 버그.
 *   4. CQE 16B 채우기:
 *      - cpl->sqhd: SPDK 의 SQ head — VM 이 받아서 SQ 가 얼마나 비었는지 확인.
 *      - sqid/cid/cdw0/status — NVMe spec § 4.4 cqe format.
 *      - phase bit: cq->phase (wrap 마다 토글). VM 이 phase 로 새 CQE vs stale 구분.
 *   5. nr_outstanding-- (Delete CQ 전 drain 판정용).
 *   6. wmb + cq_tail_advance: CQE write 가 VM 에 보인 후 tail 갱신.
 *   7. IRQ trigger (조건):
 *      - admin CQ 또는 adaptive_irqs 비활성 (= 매 CQE 마다 IRQ).
 *      - cq->ien (Interrupt Enable, Create IO CQ 시 결정).
 *      - controller-level interrupt enable.
 *      → vfu_irq_trigger 가 libvfio-user 통해 MSI-X 발행.
 *
 * adaptive_irqs: IO CQ 의 IRQ coalescing — handle_suppressed_irq 가 호스트가 활동 안 보이면
 * 그때만 IRQ trigger. CPU 사용량 절감.
 *
 * 호출 체인:
 *   handle_cmd_rsp (bdev 완료 후) → [본 함수]
 *     → CQ memory write (host shared)
 *     → vfu_irq_trigger → libvfio-user → VM IRQ delivery
 */
static int
post_completion(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_cq *cq,
		uint32_t cdw0, uint16_t sqid, uint16_t cid, uint16_t sc, uint16_t sct)
{
	struct spdk_nvme_status cpl_status = { 0 };
	struct spdk_nvme_cpl *cpl;
	int err;

	assert(ctrlr != NULL);

	/* [한국어] CQ 가 아직 매핑 안 됐거나 (Create IO CQ 전) lazy unmap 상태면 no-op. */
	if (spdk_unlikely(cq == NULL || q_addr(&cq->mapping) == NULL)) {
		return 0;
	}

	/* [한국어] admin CQ (qid=0) 는 ctrlr thread 에서만 — admin command 처리의 thread affinity 보장. */
	if (cq->qid == 0) {
		assert(spdk_get_thread() == cq->group->group->thread);
	}

	/*
	 * As per NVMe Base spec 3.3.1.2.1, we are supposed to implement CQ flow
	 * control: that is, we should handle running out of free CQ slots.
	 *
	 * Instead, we implement this by applying flow control on the submission
	 * side: see handle_sq_tdbl_write().
	 */
	/* [한국어] CQ full = 버그 — SQ 측 flow control 이 inflight 가 CQ 크기 넘지 않게 제한해야 함.
	 * 도달 시 spec 위반 또는 race. */
	if (cq_is_full(cq)) {
		SPDK_ERRLOG("%s: cqid:%d full (tail=%d, head=%d)\n",
			    ctrlr_id(ctrlr), cq->qid, *cq_tailp(cq),
			    *cq_dbl_headp(cq));
		return -1;
	}

	/* [한국어] CQE slot pointer = CQ base + tail index. VM 메모리에 직접 write. */
	cpl = ((struct spdk_nvme_cpl *)q_addr(&cq->mapping)) + *cq_tailp(cq);

	assert(ctrlr->sqs[sqid] != NULL);
	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: request complete sqid:%d cid=%d status=%#x "
		      "sqhead=%d cq tail=%d\n", ctrlr_id(ctrlr), sqid, cid, sc,
		      *sq_headp(ctrlr->sqs[sqid]), *cq_tailp(cq));

	/* [한국어] CQE 16B 채우기 (NVMe spec § 4.4). */
	cpl->sqhd = *sq_headp(ctrlr->sqs[sqid]);   /* VM 측이 SQ 의 빈 공간 추적 */
	cpl->sqid = sqid;
	cpl->cid = cid;
	cpl->cdw0 = cdw0;

	/*
	 * This is a bitfield: instead of setting the individual bits we need
	 * directly in cpl->status, which would cause a read-modify-write cycle,
	 * we'll avoid reading from the CPL altogether by filling in a local
	 * cpl_status variable, then writing the whole thing.
	 */
	/* [한국어] status 는 비트필드 — VM 메모리에 RMW 시 PCIe 왕복 1회 추가.
	 * local 변수에 조립 후 단일 store 로 회피. */
	cpl_status.sct = sct;
	cpl_status.sc = sc;
	cpl_status.p = cq->phase;     /* phase bit — wrap 마다 토글 */
	cpl->status = cpl_status;

	cq->nr_outstanding--;

	/* Ensure the Completion Queue Entry is visible. */
	/* [한국어] wmb: CQE write 가 VM 에 보인 후 tail 갱신 순서 — 그 반대면 VM 이 빈 슬롯에서
	 * stale 데이터 read. */
	spdk_wmb();
	cq_tail_advance(cq);

	/* [한국어] IRQ trigger 조건:
	 * - admin CQ (= qid 0) 는 항상 trigger (호스트가 polling 안 하는 게 일반적).
	 * - adaptive_irqs 비활성이면 매 CQE trigger (정확성 우선).
	 * - cq->ien: Create IO CQ 의 IEN bit (호스트가 IRQ 활성화 요청).
	 * - controller-level interrupt enable.
	 * 위 모두 만족 시 vfu_irq_trigger → libvfio-user → MSI-X 발행 → VM IRQ. */
	if ((cq->qid == 0 || !ctrlr->adaptive_irqs_enabled) &&
	    cq->ien && ctrlr_interrupt_enabled(ctrlr)) {
		err = vfu_irq_trigger(ctrlr->endpoint->vfu_ctx, cq->iv);
		if (err != 0) {
			SPDK_ERRLOG("%s: failed to trigger interrupt: %m\n",
				    ctrlr_id(ctrlr));
			return err;
		}
	}

	return 0;
}

/*
 * [한국어]
 * free_sq_reqs - SQ 의 free_reqs 풀에 있는 모든 요청 객체를 해제.
 *
 * @sq: 요청 풀을 정리할 SQ.
 *
 * 동기/배경: SQ 가 삭제(delete_sq_done) 또는 초기화 실패 시(alloc_sq_reqs 에러 경로)
 *   free_reqs TAILQ 에 있는 vu_req 객체들을 모두 free 한다. EXECUTING 상태의 요청은 이미
 *   outstanding TAILQ 에 있어 여기에 없으므로 double-free 없음.
 * 동작: free_reqs 가 빌 때까지 FIRST+REMOVE+free 반복.
 * 실행 컨텍스트: SQ destroy 경로 (controller thread, 단일 스레드).
 * 호출자: delete_sq_done, free_qp, alloc_sq_reqs (에러 경로).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   delete_sq_done → [free_sq_reqs] → free(vu_req)
 */
static void
free_sq_reqs(struct nvmf_vfio_user_sq *sq)
{
	while (!TAILQ_EMPTY(&sq->free_reqs)) {
		struct nvmf_vfio_user_req *vu_req = TAILQ_FIRST(&sq->free_reqs);  /* [한국어] 첫 요청 꺼냄. */
		TAILQ_REMOVE(&sq->free_reqs, vu_req, link);  /* [한국어] 리스트에서 제거. */
		free(vu_req);  /* [한국어] alloc_sq_reqs 에서 calloc 한 메모리 반환. */
	}
}

/*
 * [한국어]
 * delete_cq_done - CQ 자원 정리 및 상태를 DELETED 로 전이.
 *
 * @ctrlr: controller.
 * @cq: 정리할 CQ (cq_ref == 0 이어야 함).
 *
 * 동기/배경: Delete IO CQ admin 명령이 성공하거나, controller reset/disconnect 시
 *   마지막 SQ 가 제거돼 cq_ref 가 0이 되면 CQ 를 해제한다. cq_ref > 0 에서 호출하면
 *   아직 binding 된 SQ 가 남은 것 — assert 로 방지.
 * 동작: unmap_q → size=0 → cq_state=DELETED → group=NULL → nr_outstanding=0.
 * 실행 컨텍스트: admin 명령 처리 또는 disconnect 경로 (controller thread).
 * 호출자: handle_del_io_q (CQ delete 성공), delete_sq_done (reset/disconnect 시 last SQ).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   handle_del_io_q → [delete_cq_done] → unmap_q
 */
static void
delete_cq_done(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_cq *cq)
{
	assert(cq->cq_ref == 0);  /* [한국어] binding SQ 가 모두 삭제됐어야 함. */
	unmap_q(ctrlr, &cq->mapping);  /* [한국어] CQ 메모리 DMA 매핑 해제. */
	cq->size = 0;  /* [한국어] size=0 으로 SPDK 가 이 CQ 를 미사용으로 인식. */
	cq->cq_state = VFIO_USER_CQ_DELETED;  /* [한국어] 상태를 DELETED 로 전이. */
	cq->group = NULL;  /* [한국어] PG 참조 클리어 (get_optimal_poll_group 의 cq->group 조건). */
	cq->nr_outstanding = 0;  /* [한국어] in-flight 카운터 초기화. */
}

/* Deletes a SQ, if this SQ is the last user of the associated CQ
 * and the controller is being shut down/reset or vfio-user client disconnects,
 * then the CQ is also deleted.
 */
/*
 * [한국어]
 * delete_sq_done - SQ 자원 정리 및 상태 전이, reset/disconnect 시 관련 CQ 도 함께 정리.
 *
 * @vu_ctrlr: controller.
 * @sq: 정리할 SQ.
 *
 * 동기/배경: qpair disconnect 완료 후(nvmf_vfio_user_close_qpair) 또는 Delete IO SQ 처리 후
 *   호출된다. SQ 의 매핑, free_reqs 를 해제하고 sq_state=DELETED 로 전이한다. 단, controller
 *   reset 이나 disconnect 시(reset_shn || disconnect 플래그)에는 VM 이 Delete IO SQ/CQ 를
 *   명시적으로 보내지 않으므로 이 함수가 CQ 도 함께 정리한다(cq_ref 감소 후 0이면 delete).
 * 동작: unmap_q + free_sq_reqs + sq_state=DELETED → reset/disconnect 이면 cq_ref-- + delete_cq_done.
 * 실행 컨텍스트: controller thread (endpoint->lock 보유 상태).
 * 호출자: nvmf_vfio_user_close_qpair.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   nvmf_vfio_user_close_qpair → [delete_sq_done] → unmap_q / free_sq_reqs → (reset 시) delete_cq_done
 */
static void
delete_sq_done(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_cq *cq;
	uint16_t cqid;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: delete sqid:%d=%p done\n", ctrlr_id(vu_ctrlr),
		      sq->qid, sq);

	/* Free SQ resources */
	unmap_q(vu_ctrlr, &sq->mapping);

	free_sq_reqs(sq);

	sq->size = 0;

	sq->sq_state = VFIO_USER_SQ_DELETED;

	/* Controller RESET and SHUTDOWN are special cases,
	 * VM may not send DELETE IO SQ/CQ commands, NVMf library
	 * will disconnect IO queue pairs.
	 */
	if (vu_ctrlr->reset_shn || vu_ctrlr->disconnect) {
		cqid = sq->cqid;
		cq = vu_ctrlr->cqs[cqid];

		SPDK_DEBUGLOG(nvmf_vfio, "%s: try to delete cqid:%u=%p\n", ctrlr_id(vu_ctrlr),
			      cq->qid, cq);

		assert(cq->cq_ref > 0);
		if (--cq->cq_ref == 0) {
			delete_cq_done(vu_ctrlr, cq);
		}
	}
}

/*
 * [한국="]
 * free_qp - controller 의 SQ[qid] + CQ[qid] 를 완전히 해제 (메모리까지).
 *
 * @ctrlr: controller (NULL 이면 no-op).
 * @qid: 해제할 queue id.
 *
 * 동기/배경: controller 생성 실패(nvmf_vfio_user_create_ctrlr 에러 경로) 또는 free_ctrlr 에서
 *   qid 별 SQ/CQ 객체를 완전히 해제한다. delete_sq_done 은 매핑/reqs 만 정리하지만,
 *   free_qp 는 sg 버퍼와 SQ/CQ 객체 자체까지 free 하고 ctrlr->sqs/cqs[qid]=NULL 한다.
 * 동작: SQ 있으면 unmap_q + free_sq_reqs + free(sg) + free(sq) → ctrlr->sqs[qid]=NULL.
 *   CQ 있으면 unmap_q + free(sg) + free(cq) → ctrlr->cqs[qid]=NULL.
 * 실행 컨텍스트: controller 생성 실패 경로 또는 free_ctrlr (단일 스레드).
 * 호출자: nvmf_vfio_user_create_ctrlr (에러 경로), free_ctrlr.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   free_ctrlr → [free_qp] → unmap_q + free_sq_reqs + free
 */
static void
free_qp(struct nvmf_vfio_user_ctrlr *ctrlr, uint16_t qid)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;

	if (ctrlr == NULL) {  /* [한국어] ctrlr 없으면 no-op (생성 실패 초기 경로). */
		return;
	}

	sq = ctrlr->sqs[qid];
	if (sq) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s: Free sqid:%u\n", ctrlr_id(ctrlr), qid);
		unmap_q(ctrlr, &sq->mapping);

		free_sq_reqs(sq);

		free(sq->mapping.sg);
		free(sq);
		ctrlr->sqs[qid] = NULL;
	}

	cq = ctrlr->cqs[qid];
	if (cq) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s: Free cqid:%u\n", ctrlr_id(ctrlr), qid);
		unmap_q(ctrlr, &cq->mapping);
		free(cq->mapping.sg);
		free(cq);
		ctrlr->cqs[qid] = NULL;
	}
}

/*
 * [한국어]
 * init_sq - SQ 객체를 calloc 해 controller 의 sqs[id] 슬롯에 등록.
 *
 * @ctrlr: controller.
 * @transport: generic nvmf transport (qpair.transport 필드에 저장 — Connect 경로에서 필요).
 * @id: SQ id (0 = admin, 1..MAX = IO).
 * @return: 0 성공, -ENOMEM 메모리 부족.
 *
 * 동기/배경: SQ 는 사용 전(Create IO SQ 또는 ctrlr 생성 시 admin SQ 초기화)에 한 번만
 *   calloc 된다. 이후 map_q 로 메모리 매핑, alloc_sq_reqs 로 요청 풀이 채워진다.
 *   sg 버퍼(dma_sg_size() 크기)는 map_q 내부에서 vfu_sgl_get 에 전달되는 DMA descriptor.
 * 동작: SQ calloc → sg calloc → qid/qpair.qid/qpair.transport/ctrlr 초기화 →
 *   ctrlr->sqs[id] 등록 → free_reqs TAILQ_INIT.
 * 실행 컨텍스트: admin 명령 처리 또는 ctrlr 생성 초기화.
 * 호출자: nvmf_vfio_user_create_ctrlr (admin SQ용), handle_create_io_sq (IO SQ용).
 * 에러 경로: calloc 실패 → -ENOMEM (롤백은 호출자).
 *
 * 호출 체인:
 *   handle_create_io_sq → [init_sq] → calloc × 2 → ctrlr->sqs[id] 등록
 */
static int
init_sq(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvmf_transport *transport,
	const uint16_t id)
{
	struct nvmf_vfio_user_sq *sq;

	assert(ctrlr != NULL);
	assert(transport != NULL);
	assert(ctrlr->sqs[id] == NULL);  /* [한국어] 중복 초기화 방지. */

	sq = calloc(1, sizeof(*sq));
	if (sq == NULL) {
		return -ENOMEM;
	}
	sq->mapping.sg = calloc(1, dma_sg_size());
	if (sq->mapping.sg == NULL) {
		free(sq);
		return -ENOMEM;
	}

	sq->qid = id;
	sq->qpair.qid = id;
	sq->qpair.transport = transport;
	sq->ctrlr = ctrlr;
	ctrlr->sqs[id] = sq;

	TAILQ_INIT(&sq->free_reqs);

	return 0;
}

/*
 * [한국어]
 * init_cq - CQ 객체를 calloc 해 controller 의 cqs[id] 슬롯에 등록.
 *
 * @vu_ctrlr: controller.
 * @id: CQ id (0 = admin, 1..MAX = IO).
 * @return: 0 성공, -ENOMEM 메모리 부족.
 *
 * 동기/배경: init_sq 와 대칭. CQ 는 Create IO CQ 명령 처리(handle_create_io_cq) 또는
 *   controller 생성 시 admin CQ(id=0) 초기화에 호출된다. 이후 acq_setup 또는
 *   handle_create_io_cq 가 매핑/크기를 완성한다.
 * 동작: CQ calloc → sg calloc → qid 초기화 → cqs[id] 등록.
 * 실행 컨텍스트: admin 명령 처리 또는 ctrlr 생성 초기화.
 * 호출자: nvmf_vfio_user_create_ctrlr (admin CQ용), handle_create_io_cq (IO CQ용).
 * 에러 경로: calloc 실패 → -ENOMEM.
 *
 * 호출 체인:
 *   handle_create_io_cq → [init_cq] → calloc × 2 → cqs[id] 등록
 */
static int
init_cq(struct nvmf_vfio_user_ctrlr *vu_ctrlr, const uint16_t id)
{
	struct nvmf_vfio_user_cq *cq;

	assert(vu_ctrlr != NULL);
	assert(vu_ctrlr->cqs[id] == NULL);  /* [한국어] 중복 초기화 방지. */

	cq = calloc(1, sizeof(*cq));
	if (cq == NULL) {
		return -ENOMEM;
	}
	cq->mapping.sg = calloc(1, dma_sg_size());
	if (cq->mapping.sg == NULL) {
		free(cq);
		return -ENOMEM;
	}

	cq->qid = id;
	vu_ctrlr->cqs[id] = cq;

	return 0;
}

/*
 * [한국어]
 * alloc_sq_reqs - SQ 의 요청(vu_req) 풀을 sq->size 개만큼 calloc 해 free_reqs 에 등록.
 *
 * @vu_ctrlr: controller (현재 미사용 — 서명 일관성).
 * @sq: 요청 풀을 채울 SQ (sq->size 가 미리 설정돼 있어야 함).
 * @return: 0 성공, -ENOMEM 메모리 부족.
 *
 * 동기/배경: vfio-user 는 hot path 에서 malloc 을 피하려고 SQ 생성 시 요청 객체를
 *   미리 할당한다. 요청 1개의 크기 = sizeof(nvmf_vfio_user_req) + dma_sg_size() × MAX_IOVECS.
 *   dma_sg_size() 는 libvfio-user 버전에 따라 다르므로 sizeof 대신 런타임 계산.
 * 동작: sq->size 번 calloc → req/rsp/cmd/qpair 초기화 → free_reqs 에 TAIL INSERT.
 *   실패 시 err: 지금까지 할당된 것 모두 free(free_reqs FOREACH_SAFE).
 * 실행 컨텍스트: SQ 생성 경로 (handle_create_io_sq 또는 ctrlr 생성 시).
 * 호출자: handle_create_io_sq, nvmf_vfio_user_create_ctrlr (admin SQ 용).
 * 에러 경로: calloc 실패 → err 레이블 → TAILQ_FOREACH_SAFE free → -ENOMEM.
 *
 * 호출 체인:
 *   handle_create_io_sq → [alloc_sq_reqs] → calloc(sq->size) → TAILQ_INSERT(free_reqs)
 */
static int
alloc_sq_reqs(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_req *vu_req, *tmp;
	size_t req_size;
	uint32_t i;

	req_size = sizeof(struct nvmf_vfio_user_req) +
		   (dma_sg_size() * NVMF_VFIO_USER_MAX_IOVECS);

	for (i = 0; i < sq->size; i++) {
		struct spdk_nvmf_request *req;

		vu_req = calloc(1, req_size);
		if (vu_req == NULL) {
			goto err;
		}

		req = &vu_req->req;
		req->qpair = &sq->qpair;
		req->rsp = (union nvmf_c2h_msg *)&vu_req->rsp;
		req->cmd = (union nvmf_h2c_msg *)&vu_req->cmd;
		req->stripped_data = NULL;

		TAILQ_INSERT_TAIL(&sq->free_reqs, vu_req, link);
	}

	return 0;

err:
	TAILQ_FOREACH_SAFE(vu_req, &sq->free_reqs, link, tmp) {
		free(vu_req);
	}
	return -ENOMEM;
}

/*
 * [한국어]
 * ctrlr_doorbell_ptr - 현재 활성화된 doorbell 배열(shadow 또는 BAR0)의 시작 포인터 반환.
 *
 * @ctrlr: 쿼리할 controller.
 * @return: shadow doorbell 활성 시 sdbl->shadow_doorbells, 그 외 bar0_doorbells.
 *
 * 동기/배경: shadow doorbell 활성 시(Doorbell Buffer Config 완료 후) VM 이 SQ tail을
 *   bar0_doorbells 대신 shadow_doorbells 에 write 한다. SQ 의 dbl_tailp 를 설정하거나
 *   eventidx 계산을 위한 base pointer 선택에 사용.
 * 동작: sdbl != NULL 이면 shadow_doorbells, 아니면 bar0_doorbells.
 * 실행 컨텍스트: admin 명령 처리 경로 (handle_create_io_sq).
 * 호출자: handle_create_io_sq (SQ dbl_tailp 설정).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   handle_create_io_sq → [ctrlr_doorbell_ptr]
 */
static volatile uint32_t *
ctrlr_doorbell_ptr(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	/* [한국어] shadow doorbell 활성: VM 이 sdbl->shadow_doorbells 에 doorbell write.
	 * 비활성: VM 이 BAR0 doorbell(bar0_doorbells)에 write (BAR exit 발생). */
	return ctrlr->sdbl != NULL ?
	       ctrlr->sdbl->shadow_doorbells :
	       ctrlr->bar0_doorbells;
}

/*
 * [한국어]
 * handle_create_io_sq - ★ Create I/O Submission Queue admin 명령 (opcode 0x01) 처리.
 *
 * @ctrlr: controller.
 * @cmd: SQE (cdw10/11 에 qid, qsize, cqid, pc, qprio 비트필드).
 * @sct: out — SCT (status code type).
 * @return: SC (status code). 0 = 성공.
 *
 * VM 이 Create IO SQ 발행 시 본 함수가 처리:
 *   1. cdw10/11 비트필드 추출 (qid, cqid, qsize+1).
 *   2. sqs[qid] 슬롯 미할당 시 init_sq 로 SQ 객체 생성.
 *   3. cqid 검증 (0=admin, max_qpairs 초과 금지).
 *   4. 짝이 되는 CQ 가 먼저 Create 됐는지 검증 (NVMe spec 강제 순서).
 *   5. pc=1 (Physically Contiguous) 만 지원 — PRP list 의 분산 큐 미지원.
 *   6. SQ 메모리 매핑 (PRP1 = VM 의 SQ host IOVA → vfu_addr_to_sgl 로 host VA 변환).
 *   7. alloc_sq_reqs: qsize 개 vu_req 객체 calloc + free_reqs 풀에 등록.
 *   8. cq_ref++ (multiple SQ → 1 CQ 일 때 Delete CQ 차단).
 *   9. sq_state = CREATED, doorbell pointer 세팅.
 *  10. shadow doorbell 사용 시 eventidx 초기화 (poll/notify race 회피).
 *
 * race 조건 — !set_sq_eventidx:
 *   eventidx 세팅 전에 VM 이 이미 doorbell write 했으면 통지 손실 가능 → fail_ctrlr.
 *
 * 호출 체인:
 *   handle_cmd_req (admin SQE) → map_admin_cmd_req → [본 함수]
 *     → init_sq → map_q → alloc_sq_reqs → cq_ref++ → set_sq_eventidx (shadow doorbell 시)
 */
static uint16_t
handle_create_io_sq(struct nvmf_vfio_user_ctrlr *ctrlr,
		    struct spdk_nvme_cmd *cmd, uint16_t *sct)
{
	struct nvmf_vfio_user_transport *vu_transport = ctrlr->transport;
	struct nvmf_vfio_user_sq *sq;
	uint32_t qsize;
	uint16_t cqid;
	uint16_t qid;
	int err;

	/* [한국어] NVMe spec § 5.5: Create I/O SQ 의 cdw10/11 비트필드. qsize 는 0-based 라서 +1. */
	qid = cmd->cdw10_bits.create_io_q.qid;
	cqid = cmd->cdw11_bits.create_io_sq.cqid;
	qsize = cmd->cdw10_bits.create_io_q.qsize + 1;

	/* [한국어] SQ 슬롯 lazy 생성 — 처음 사용 시점에 init. admin queue (qid=0) 의 transport 재사용. */
	if (ctrlr->sqs[qid] == NULL) {
		err = init_sq(ctrlr, ctrlr->sqs[0]->qpair.transport, qid);
		if (err != 0) {
			*sct = SPDK_NVME_SCT_GENERIC;
			return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}

	/* [한국어] cqid 검증 — admin CQ (0) 는 IO SQ 에 못 쓰고, max 초과 금지. */
	if (cqid == 0 || cqid >= vu_transport->transport.opts.max_qpairs_per_ctrlr) {
		SPDK_ERRLOG("%s: invalid cqid:%u\n", ctrlr_id(ctrlr), cqid);
		*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		return SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
	}

	/* CQ must be created before SQ. */
	/* [한국어] NVMe spec § 5.5.1: SQ 는 자신이 binding 할 CQ 가 이미 존재해야 함. */
	if (!io_q_exists(ctrlr, cqid, true)) {
		SPDK_ERRLOG("%s: cqid:%u does not exist\n", ctrlr_id(ctrlr), cqid);
		*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		return SPDK_NVME_SC_COMPLETION_QUEUE_INVALID;
	}

	/* [한국어] PC=1 (Physically Contiguous) 만 지원. PC=0 면 PRP list 형식인데 vfio-user 미구현. */
	if (cmd->cdw11_bits.create_io_sq.pc != 0x1) {
		SPDK_ERRLOG("%s: non-PC SQ not supported\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INVALID_FIELD;
	}

	sq = ctrlr->sqs[qid];
	sq->size = qsize;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: sqid:%d cqid:%d\n", ctrlr_id(ctrlr),
		      qid, cqid);

	sq->mapping.prp1 = cmd->dptr.prp.prp1;
	sq->mapping.len = sq->size * sizeof(struct spdk_nvme_cmd);

	err = map_q(ctrlr, &sq->mapping, MAP_INITIALIZE);
	if (err) {
		SPDK_ERRLOG("%s: failed to map I/O queue: %m\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s: mapped sqid:%d IOVA=%#lx vaddr=%p\n",
		      ctrlr_id(ctrlr), qid, cmd->dptr.prp.prp1,
		      q_addr(&sq->mapping));

	err = alloc_sq_reqs(ctrlr, sq);
	if (err < 0) {
		SPDK_ERRLOG("%s: failed to allocate SQ requests: %m\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	sq->cqid = cqid;
	ctrlr->cqs[sq->cqid]->cq_ref++;
	sq->sq_state = VFIO_USER_SQ_CREATED;
	*sq_headp(sq) = 0;

	sq->dbl_tailp = ctrlr_doorbell_ptr(ctrlr) + queue_index(qid, false);

	/*
	 * We should always reset the doorbells.
	 *
	 * The Specification prohibits the controller from writing to the shadow
	 * doorbell buffer, however older versions of the Linux NVMe driver
	 * don't reset the shadow doorbell buffer after a Queue-Level or
	 * Controller-Level reset, which means that we're left with garbage
	 * doorbell values.
	 */
	*sq_dbl_tailp(sq) = 0;

	if (ctrlr->sdbl != NULL) {
		sq->need_rearm = true;

		if (!set_sq_eventidx(sq)) {
			SPDK_ERRLOG("%s: host updated SQ tail doorbell before "
				    "sqid:%hu was initialized\n",
				    ctrlr_id(ctrlr), qid);
			fail_ctrlr(ctrlr);
			*sct = SPDK_NVME_SCT_GENERIC;
			return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}

	/*
	 * Create our new I/O qpair. This asynchronously invokes, on a suitable
	 * poll group, the nvmf_vfio_user_poll_group_add() callback, which will
	 * call spdk_nvmf_request_exec() with a generated fabrics
	 * connect command. This command is then eventually completed via
	 * handle_queue_connect_rsp().
	 */
	sq->create_io_sq_cmd = *cmd;
	sq->post_create_io_sq_completion = true;

	spdk_nvmf_tgt_new_qpair(ctrlr->transport->transport.tgt,
				&sq->qpair);

	*sct = SPDK_NVME_SCT_GENERIC;
	return SPDK_NVME_SC_SUCCESS;
}

/*
 * [한국어]
 * handle_create_io_cq - ★ Create I/O Completion Queue admin (opcode 0x05) 처리.
 *
 * @ctrlr: controller.
 * @cmd: SQE (cdw10/11 = qid, qsize+1, pc, iv, ien).
 * @sct: out — SCT.
 * @return: SC.
 *
 * NVMe spec § 5.5.2. VM 의 Create IO CQ admin 처리. SQ 와의 차이:
 *   - iv (Interrupt Vector): MSI-X table 의 index (0..MSIX_NUM-1).
 *   - ien (Interrupt Enable): 이 CQ 의 CQE write 시 IRQ trigger 여부.
 *   - phase=true 초기화: 첫 wrap 전 expected phase.
 *   - cq_state = CREATED → SQ 가 binding 가능.
 *
 * 호출 체인:
 *   handle_cmd_req → consume_admin_cmd → [본 함수] → init_cq → map_q
 *     → cq_state=CREATED → 이후 handle_create_io_sq 가 cq_ref++
 */
static uint16_t
handle_create_io_cq(struct nvmf_vfio_user_ctrlr *ctrlr,
		    struct spdk_nvme_cmd *cmd, uint16_t *sct)
{
	struct nvmf_vfio_user_cq *cq;
	uint32_t qsize;
	uint16_t qid;
	int err;

	qid = cmd->cdw10_bits.create_io_q.qid;
	qsize = cmd->cdw10_bits.create_io_q.qsize + 1;

	/* [한국어] CQ 슬롯 lazy init (Create IO SQ 와 같은 패턴). */
	if (ctrlr->cqs[qid] == NULL) {
		err = init_cq(ctrlr, qid);
		if (err != 0) {
			*sct = SPDK_NVME_SCT_GENERIC;
			return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}

	/* [한국어] PC=1 only (SQ 와 동일 — PRP list 형식 미지원). */
	if (cmd->cdw11_bits.create_io_cq.pc != 0x1) {
		SPDK_ERRLOG("%s: non-PC CQ not supported\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INVALID_FIELD;
	}

	/* [한국어] IV 검증 — MSI-X vector index 가 advertise 한 NUM 범위 안. */
	if (cmd->cdw11_bits.create_io_cq.iv > NVMF_VFIO_USER_MSIX_NUM - 1) {
		SPDK_ERRLOG("%s: IV is too big\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		return SPDK_NVME_SC_INVALID_INTERRUPT_VECTOR;
	}

	cq = ctrlr->cqs[qid];
	cq->size = qsize;

	cq->mapping.prp1 = cmd->dptr.prp.prp1;
	cq->mapping.len = cq->size * sizeof(struct spdk_nvme_cpl);

	cq->dbl_headp = ctrlr_doorbell_ptr(ctrlr) + queue_index(qid, true);

	err = map_q(ctrlr, &cq->mapping, MAP_RW | MAP_INITIALIZE);
	if (err) {
		SPDK_ERRLOG("%s: failed to map I/O queue: %m\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s: mapped cqid:%u IOVA=%#lx vaddr=%p\n",
		      ctrlr_id(ctrlr), qid, cmd->dptr.prp.prp1,
		      q_addr(&cq->mapping));

	cq->ien = cmd->cdw11_bits.create_io_cq.ien;
	cq->iv = cmd->cdw11_bits.create_io_cq.iv;
	cq->phase = true;
	cq->cq_state = VFIO_USER_CQ_CREATED;

	*cq_tailp(cq) = 0;

	/*
	 * We should always reset the doorbells.
	 *
	 * The Specification prohibits the controller from writing to the shadow
	 * doorbell buffer, however older versions of the Linux NVMe driver
	 * don't reset the shadow doorbell buffer after a Queue-Level or
	 * Controller-Level reset, which means that we're left with garbage
	 * doorbell values.
	 */
	*cq_dbl_headp(cq) = 0;

	*sct = SPDK_NVME_SCT_GENERIC;
	return SPDK_NVME_SC_SUCCESS;
}

/*
 * Creates a completion or submission I/O queue. Returns 0 on success, -errno
 * on error.
 */
/*
 * [한국어]
 * handle_create_io_q - Create I/O SQ/CQ admin wrapper — 공통 검증 + SQ/CQ 분기.
 *
 * @ctrlr: controller.
 * @cmd: Create IO SQ/CQ SQE.
 * @is_cq: true = CQ (opcode 0x05), false = SQ (opcode 0x01).
 * @return: 0 = response 발행 완료.
 *
 * consume_admin_cmd 가 호출. SQ 와 CQ 양쪽이 공유하는 검증 (qid 범위, 중복 검사, qsize)
 * 을 wrapper 에서 처리한 뒤 handle_create_io_sq / handle_create_io_cq 로 분기.
 *
 * 공통 검증:
 *   1. qid == 0 또는 max_qpairs 초과 — INVALID_QUEUE_IDENTIFIER.
 *   2. 같은 qid 가 이미 존재 — INVALID_QUEUE_IDENTIFIER.
 *   3. qsize == 1 (NVMe spec: 2 이상) 또는 max_queue_size 초과 — INVALID_QUEUE_SIZE.
 *
 * SQ 성공 분기의 특이점: SQ Create 가 성공하면 spdk_nvmf_tgt_new_qpair 가 호출되어
 * poll_group_add 가 비동기로 진행 → CQE 발행은 handle_queue_connect_rsp 가 처리
 * (post_create_io_sq_completion 플래그). 따라서 본 함수는 0 반환 (CQE 안 보냄).
 * CQ 는 즉시 완료라 fall-through post_completion.
 *
 * 호출 체인:
 *   consume_admin_cmd (Create IO SQ/CQ) → [본 함수] → 공통 검증 →
 *     CQ: handle_create_io_cq → post_completion
 *     SQ: handle_create_io_sq → (성공) 비동기 CQE / (실패) post_completion
 */
static int
handle_create_io_q(struct nvmf_vfio_user_ctrlr *ctrlr,
		   struct spdk_nvme_cmd *cmd, const bool is_cq)
{
	struct nvmf_vfio_user_transport *vu_transport = ctrlr->transport;
	uint16_t sct = SPDK_NVME_SCT_GENERIC;
	uint16_t sc = SPDK_NVME_SC_SUCCESS;
	uint32_t qsize;
	uint16_t qid;

	assert(ctrlr != NULL);
	assert(cmd != NULL);

	qid = cmd->cdw10_bits.create_io_q.qid;
	/* [한국어] qid=0 은 admin 전용. max_qpairs 초과 = transport opts 한계. */
	if (qid == 0 || qid >= vu_transport->transport.opts.max_qpairs_per_ctrlr) {
		SPDK_ERRLOG("%s: invalid qid=%d, max=%d\n", ctrlr_id(ctrlr),
			    qid, vu_transport->transport.opts.max_qpairs_per_ctrlr);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		goto out;
	}

	/* [한국어] 중복 create 검사 — VM 의 spec 위반 또는 우리 측 stale state. */
	if (io_q_exists(ctrlr, qid, is_cq)) {
		SPDK_ERRLOG("%s: %cqid:%d already exists\n", ctrlr_id(ctrlr),
			    is_cq ? 'c' : 's', qid);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		goto out;
	}

	/* [한국어] qsize 0-based → +1. qsize=1 (raw=0) = NVMe spec 위반. max_queue_size 는
	 * 컨트롤러 MQES (Max Queue Entries Supported) 한도. */
	qsize = cmd->cdw10_bits.create_io_q.qsize + 1;
	if (qsize == 1 || qsize > max_queue_size(ctrlr)) {
		SPDK_ERRLOG("%s: invalid I/O queue size %u\n", ctrlr_id(ctrlr), qsize);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_SIZE;
		goto out;
	}

	if (is_cq) {
		sc = handle_create_io_cq(ctrlr, cmd, &sct);
	} else {
		sc = handle_create_io_sq(ctrlr, cmd, &sct);

		/* [한국어] SQ 성공 시 비동기 CQE — spdk_nvmf_tgt_new_qpair → poll_group_add →
		 * handle_queue_connect_rsp 가 post_create_io_sq_completion 플래그 보고 CQE 발행.
		 * 본 함수가 post_completion 하면 중복 CQE → 잘못된 cid 응답 발생. */
		if (sct == SPDK_NVME_SCT_GENERIC &&
		    sc == SPDK_NVME_SC_SUCCESS) {
			/* Completion posted asynchronously. */
			return 0;
		}
	}

out:
	return post_completion(ctrlr, ctrlr->cqs[0], 0, 0, cmd->cid, sc, sct);
}

/* For ADMIN I/O DELETE SUBMISSION QUEUE the NVMf library will disconnect and free
 * queue pair, so save the command id and controller in a context.
 */
/* [한국어] struct vfio_user_delete_sq_ctx - Delete IO SQ 비동기 완료를 위한 컨텍스트.
 *
 * 배경: Delete IO SQ 명령 처리 시 qpair_disconnect 가 비동기로 진행되므로, 원래 명령의
 *   cid(command id)와 controller 포인터를 보관해 qpair 종료 콜백(vfio_user_qpair_delete_cb)
 *   이 admin CQE 를 올바른 cid 로 post 할 수 있게 한다.
 */
struct vfio_user_delete_sq_ctx {
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	/* [한국어] 이 Delete IO SQ 를 처리할 controller.
	 * 설정자: handle_del_io_q (SQ delete 시 calloc + 초기화).
	 * 읽는 자: vfio_user_qpair_delete_cb (admin CQ 를 통해 CQE post 위해).
	 * 값 범위: 유효한 controller 포인터. SQ 가 close 될 때까지 살아 있어야 함.
	 * 동기화: SQ delete 는 단일 스레드(controller thread)에서만 발생 — 락 불필요. */

	uint16_t cid;
	/* [한국어] Delete IO SQ 명령의 command id.
	 * 설정자: handle_del_io_q (cmd->cid 를 복사).
	 * 읽는 자: vfio_user_qpair_delete_cb (post_completion 의 cid 인자로 사용).
	 * 값 범위: 0~0xFFFF (NVMe spec의 CID 필드).
	 * 동기화: 이 컨텍스트를 보는 스레드가 1개 — 별도 락 불필요. */
};

/*
 * [한국어]
 * vfio_user_qpair_delete_cb - Delete IO SQ 의 qpair disconnect 완료 후 admin CQE 게시.
 *
 * @cb_arg: vfio_user_delete_sq_ctx 포인터 (cid/controller 보유).
 *
 * 동기/배경: Delete IO SQ 는 qpair_disconnect 이후 drain 이 완료돼야 CQE 를 post 할 수 있다.
 *   그런데 CQE 는 admin CQ 를 소유한 thread 에서만 안전하게 post 되므로, thread 가 다르면
 *   spdk_thread_send_msg 로 재귀 전송해 admin CQ 소유 thread 에서 실행되게 한다.
 * 동작: admin CQ 소유 thread 확인 → 다르면 재귀 메시지 → 같으면 post_completion + free(ctx).
 * 실행 컨텍스트: controller thread → admin CQ 소유 thread (thread 간 전달 가능).
 * 호출자: nvmf_vfio_user_close_qpair (del_ctx 있을 때).
 * 에러 경로: 없음 (post_completion 자체도 assert 로 보호).
 *
 * 호출 체인:
 *   nvmf_vfio_user_close_qpair → [vfio_user_qpair_delete_cb]
 *     → spdk_thread_send_msg (재귀, 다른 thread) 또는 post_completion + free (같은 thread)
 */
static void
vfio_user_qpair_delete_cb(void *cb_arg)
{
	struct vfio_user_delete_sq_ctx *ctx = cb_arg;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = ctx->vu_ctrlr;
	struct nvmf_vfio_user_cq *admin_cq = vu_ctrlr->cqs[0];

	assert(admin_cq != NULL);
	assert(admin_cq->group != NULL);
	assert(admin_cq->group->group->thread != NULL);
	if (admin_cq->group->group->thread != spdk_get_thread()) {
		spdk_thread_send_msg(admin_cq->group->group->thread,
				     vfio_user_qpair_delete_cb,
				     cb_arg);
	} else {
		post_completion(vu_ctrlr, admin_cq, 0, 0,
				ctx->cid,
				SPDK_NVME_SC_SUCCESS, SPDK_NVME_SCT_GENERIC);
		free(ctx);
	}
}

/*
 * Deletes a completion or submission I/O queue.
 */
/*
 * [한국어]
 * handle_del_io_q - Delete I/O SQ (opcode 0x00) 또는 CQ (opcode 0x04) admin 처리.
 *
 * @ctrlr: controller.
 * @cmd: SQE (cdw10 의 lower 16-bit = qid).
 * @is_cq: true = Delete IO CQ, false = Delete IO SQ.
 * @return: 0 = response 발행 완료 (성공/에러 모두 CQE post 됨).
 *
 * NVMe spec § 5.6 / § 5.4. SQ vs CQ 처리 비대칭:
 *
 *   CQ delete:
 *     - cq_ref > 0 이면 INVALID_QUEUE_DELETION (SQ 가 먼저 delete 되어야 함).
 *     - delete_cq_done: unmap_q + size=0 + cq_state=DELETED + nr_outstanding=0.
 *     - 즉시 CQE post.
 *
 *   SQ delete:
 *     - delete_ctx 동적 할당 (qpair_disconnect 가 비동기라 cmd 정보 보존 필요).
 *     - sq_state=DELETED 마킹 + cq_ref-- (CQ 가 이제 delete 가능).
 *     - spdk_nvmf_qpair_disconnect → 비동기 drain → delete_sq_done 콜백에서 CQE post.
 *     - 본 함수는 0 반환 (CQE post 는 콜백에서).
 *
 * Deletion 의 비대칭 이유:
 *   - SQ 는 in-flight 가 있을 수 있어 drain 후에야 delete 완료 통지.
 *   - CQ 는 SQ 가 모두 delete 된 후 (cq_ref=0) 호출되므로 즉시 완료 가능.
 *
 * 호출 체인:
 *   handle_cmd_req → consume_admin_cmd → [본 함수]
 *     → CQ: delete_cq_done + post_completion (즉시)
 *     → SQ: qpair_disconnect → drain → delete_sq_done → post_completion (비동기)
 */
static int
handle_del_io_q(struct nvmf_vfio_user_ctrlr *ctrlr,
		struct spdk_nvme_cmd *cmd, const bool is_cq)
{
	uint16_t sct = SPDK_NVME_SCT_GENERIC;
	uint16_t sc = SPDK_NVME_SC_SUCCESS;
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: delete I/O %cqid:%d\n",
		      ctrlr_id(ctrlr), is_cq ? 'c' : 's',
		      cmd->cdw10_bits.delete_io_q.qid);

	/* [한국어] 존재 여부 검증 — 없는 queue 에 delete 시도하면 spec 위반. */
	if (!io_q_exists(ctrlr, cmd->cdw10_bits.delete_io_q.qid, is_cq)) {
		SPDK_ERRLOG("%s: I/O %cqid:%d does not exist\n", ctrlr_id(ctrlr),
			    is_cq ? 'c' : 's', cmd->cdw10_bits.delete_io_q.qid);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		goto out;
	}

	if (is_cq) {
		cq = ctrlr->cqs[cmd->cdw10_bits.delete_io_q.qid];
		/* [한국어] cq_ref > 0 = 이 CQ 에 binding 된 SQ 가 아직 있음.
		 * NVMe spec § 5.4: SQ delete 먼저 해야 함. INVALID_QUEUE_DELETION SC. */
		if (cq->cq_ref) {
			SPDK_ERRLOG("%s: the associated SQ must be deleted first\n", ctrlr_id(ctrlr));
			sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			sc = SPDK_NVME_SC_INVALID_QUEUE_DELETION;
			goto out;
		}
		/* [한국어] CQ 는 즉시 unmap + state 변경 가능 (in-flight 없음 보장). */
		delete_cq_done(ctrlr, cq);
	} else {
		/*
		 * Deletion of the CQ is only deferred to delete_sq_done() on
		 * VM reboot or CC.EN change, so we have to delete it in all
		 * other cases.
		 */
		sq = ctrlr->sqs[cmd->cdw10_bits.delete_io_q.qid];
		/* [한국어] delete_ctx 동적 할당 — qpair_disconnect 가 비동기라서 cb_arg 로
		 * cmd 정보 (cid) 보존 필요. delete_sq_done 콜백이 free + CQE post. */
		sq->delete_ctx = calloc(1, sizeof(*sq->delete_ctx));
		if (!sq->delete_ctx) {
			sct = SPDK_NVME_SCT_GENERIC;
			sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto out;
		}
		sq->delete_ctx->vu_ctrlr = ctrlr;
		sq->delete_ctx->cid = cmd->cid;
		sq->sq_state = VFIO_USER_SQ_DELETED;
		assert(ctrlr->cqs[sq->cqid]->cq_ref);
		/* [한국어] cq_ref-- — 이 SQ 가 차지하던 ref 해제. 마지막이면 CQ 도 delete 가능. */
		ctrlr->cqs[sq->cqid]->cq_ref--;

		/* [한국어] 비동기 disconnect 시작 — drain 끝나면 delete_sq_done 이 CQE post. */
		spdk_nvmf_qpair_disconnect(&sq->qpair);
		return 0;
	}

out:
	/* [한국어] CQ delete 또는 SQ 에러 경로 — admin CQ (cqs[0]) 에 즉시 응답 post. */
	return post_completion(ctrlr, ctrlr->cqs[0], 0, 0, cmd->cid, sc, sct);
}

/*
 * Configures Shadow Doorbells.
 */
/*
 * [한국어]
 * handle_doorbell_buffer_config - ★ NVMe 1.3 Doorbell Buffer Config admin (opcode 0x7C) 처리.
 *
 * @ctrlr: controller.
 * @cmd: SQE (PRP1 = shadow doorbell buffer GPA, PRP2 = eventidx buffer GPA).
 * @return: 0 (성공/실패 모두 CQE post 됨).
 *
 * VM 의 Linux NVMe driver 가 부팅 시 발행. shadow doorbell + eventidx 두 page (각 4KB)
 * 의 host GPA 를 controller 에 등록 → 이후 doorbell write 가 BAR exit 없이 shared
 * memory 에 직접 갈 수 있음 (VM exit 최소화).
 *
 * 검증:
 *   1. dstrd 검사: (4 << dstrd) * MAX_QPAIRS <= page_size 여야 모든 doorbell 이 한 페이지 안.
 *   2. PSDT=PRP (SGL 모드는 unsupported).
 *   3. PRP1 != PRP2 (두 page 가 다른 영역이어야 함).
 *   4. PRP1, PRP2 모두 page_mask 정렬 (4KB align).
 *
 * 매핑:
 *   - map_sdbl: PRP1, PRP2 의 GPA → host VA 변환 + dma_sg 보존.
 *   - sdbl->iovs[0] = shadow doorbell page, iovs[1] = eventidx page.
 *
 * CQ head doorbell 은 polling mode (eventidx = UINT32_MAX = NEVER_NOTIFY) 로 설정:
 *   - SPDK 는 항상 CQ head doorbell 보고 CQ slot 회수 (호스트가 갱신).
 *   - SQ tail doorbell 만 interrupt mode (호스트가 notify 필요 시).
 *
 * SWAP 트릭: 새 sdbl 설치 + 기존 sdbl 안전 cleanup. 동시에 활성화 + 회수.
 *
 * shadow_doorbell_buffer / eventidx_buffer 필드 보존: live migration source 가 destination
 * 에 전달해야 새 controller 가 같은 shared memory 사용 가능.
 *
 * 호출 체인:
 *   handle_cmd_req → consume_admin_cmd (opcode 0x7C) → [본 함수]
 *     → map_sdbl → sdbl 활성화 → CQE post (success/failure)
 */
static int
handle_doorbell_buffer_config(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd)
{
	struct nvmf_vfio_user_shadow_doorbells *sdbl = NULL;
	uint32_t dstrd;
	uintptr_t page_size, page_mask;
	uint64_t prp1, prp2;
	uint16_t sct = SPDK_NVME_SCT_GENERIC;
	uint16_t sc = SPDK_NVME_SC_INVALID_FIELD;

	assert(ctrlr != NULL);
	assert(ctrlr->endpoint != NULL);
	assert(cmd != NULL);

	dstrd = doorbell_stride(ctrlr);
	page_size = memory_page_size(ctrlr);
	page_mask = memory_page_mask(ctrlr);

	/* FIXME: we don't check doorbell stride when setting queue doorbells. */
	/* [한국어] 모든 doorbell (max_qpairs * 2 * stride) 이 한 페이지 안에 들어가야 매핑 가능. */
	if ((4u << dstrd) * NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR > page_size) {
		SPDK_ERRLOG("%s: doorbells do not fit in a single host page",
			    ctrlr_id(ctrlr));

		goto out;
	}

	/* Verify guest physical addresses passed as PRPs. */
	/* [한국어] PSDT=PRP only (SGL 모드 미지원 — NVMe 1.3 spec 이 PRP 만 허용). */
	if (cmd->psdt != SPDK_NVME_PSDT_PRP) {
		SPDK_ERRLOG("%s: received Doorbell Buffer Config without PRPs",
			    ctrlr_id(ctrlr));

		goto out;
	}

	prp1 = cmd->dptr.prp.prp1;
	prp2 = cmd->dptr.prp.prp2;

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: configuring shadow doorbells with PRP1=%#lx and PRP2=%#lx (GPAs)\n",
		      ctrlr_id(ctrlr), prp1, prp2);

	/* [한국어] PRP1 ≠ PRP2 (두 page 다른 영역) + 둘 다 page_size 정렬 — NVMe spec § 5.7. */
	if (prp1 == prp2
	    || prp1 != (prp1 & page_mask)
	    || prp2 != (prp2 & page_mask)) {
		SPDK_ERRLOG("%s: invalid shadow doorbell GPAs\n",
			    ctrlr_id(ctrlr));

		goto out;
	}

	/* Map guest physical addresses to our virtual address space. */
	/* [한국어] GPA → host VA 매핑. sdbl->iovs[0] = shadow, iovs[1] = eventidx. */
	sdbl = map_sdbl(ctrlr->endpoint->vfu_ctx, prp1, prp2, page_size);
	if (sdbl == NULL) {
		SPDK_ERRLOG("%s: failed to map shadow doorbell buffers\n",
			    ctrlr_id(ctrlr));

		goto out;
	}

	/* [한국어] live migration 용 PRP 보존 — destination 이 같은 shared memory 사용. */
	ctrlr->shadow_doorbell_buffer = prp1;
	ctrlr->eventidx_buffer = prp2;

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: mapped shadow doorbell buffers [%p, %p) and [%p, %p)\n",
		      ctrlr_id(ctrlr),
		      sdbl->iovs[0].iov_base,
		      sdbl->iovs[0].iov_base + sdbl->iovs[0].iov_len,
		      sdbl->iovs[1].iov_base,
		      sdbl->iovs[1].iov_base + sdbl->iovs[1].iov_len);


	/*
	 * Set all possible CQ head doorbells to polling mode now, such that we
	 * don't have to worry about it later if the host creates more queues.
	 *
	 * We only ever want interrupts for writes to the SQ tail doorbells
	 * (which are initialised in set_ctrlr_intr_mode() below).
	 */
	for (uint16_t i = 0; i < NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR; ++i) {
		sdbl->eventidxs[queue_index(i, true)] = NVMF_VFIO_USER_EVENTIDX_POLL;
	}

	/* Update controller. */
	SWAP(ctrlr->sdbl, sdbl);

	/*
	 * Copy doorbells from either the previous shadow doorbell buffer or the
	 * BAR0 doorbells and make I/O queue doorbells point to the new buffer.
	 *
	 * This needs to account for older versions of the Linux NVMe driver,
	 * which don't clear out the buffer after a controller reset.
	 */
	copy_doorbells(ctrlr, sdbl != NULL ?
		       sdbl->shadow_doorbells : ctrlr->bar0_doorbells,
		       ctrlr->sdbl->shadow_doorbells);

	vfio_user_ctrlr_switch_doorbells(ctrlr, true);

	ctrlr_kick(ctrlr);

	sc = SPDK_NVME_SC_SUCCESS;

out:
	/*
	 * Unmap existing buffers, in case Doorbell Buffer Config was sent
	 * more than once (pointless, but not prohibited by the spec), or
	 * in case of an error.
	 *
	 * If this is the first time Doorbell Buffer Config was processed,
	 * then we've just swapped a NULL from ctrlr->sdbl into sdbl, so
	 * free_sdbl() becomes a noop.
	 */
	free_sdbl(ctrlr->endpoint->vfu_ctx, sdbl);

	return post_completion(ctrlr, ctrlr->cqs[0], 0, 0, cmd->cid, sc, sct);
}

/*
 * [한국어]
 * consume_admin_cmd - ★ admin SQE 1개의 opcode 디스패처 (vfio-user 가 직접 처리 vs 위임).
 *
 * @ctrlr: controller.
 * @cmd: admin SQE.
 * @return: 0 = 응답 처리 완료, 음수 = 에러.
 *
 * 일부 admin 명령은 vfio-user 가 직접 emulate 해야 함 (queue 관리, doorbell config) —
 * 이런 명령은 공통 NVMe-oF layer 가 의미 없음. 그 외는 일반 nvmf_request 경로로 위임.
 *
 * 분기:
 *   1. fuse != 0: Fused admin 미지원 → INVALID_FIELD CQE.
 *   2. **CREATE_IO_CQ/SQ (0x05/0x01)**: handle_create_io_q wrapper — vfio-user 가 직접
 *      매핑 + ref counting 처리. ctrlr layer 가 알 수 없는 일.
 *   3. **DELETE_IO_SQ/CQ (0x00/0x04)**: handle_del_io_q — drain 처리 필요.
 *   4. **DOORBELL_BUFFER_CONFIG (0x7C)**: shadow doorbell 활성. opt 으로 비활성 시 fallthrough.
 *   5. **기본**: handle_cmd_req → 공통 NVMe-oF dispatch (Identify/Get Log/Set Features 등).
 *      = vfio-user 가 처리 안 하고 lib/nvmf/ctrlr.c 의 admin handler 가 처리.
 */
/* Returns 0 on success and -errno on error. */
static int
consume_admin_cmd(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd)
{
	assert(ctrlr != NULL);
	assert(cmd != NULL);

	/* [한국어] Fused command (FUSE 2-bit ≠ 0) — admin 에서는 NVMe spec 상 미지원. */
	if (cmd->fuse != 0) {
		/* Fused admin commands are not supported. */
		return post_completion(ctrlr, ctrlr->cqs[0], 0, 0, cmd->cid,
				       SPDK_NVME_SC_INVALID_FIELD,
				       SPDK_NVME_SCT_GENERIC);
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_CREATE_IO_CQ:
	case SPDK_NVME_OPC_CREATE_IO_SQ:
		/* [한국어] Queue 생성은 vfio-user 가 직접 — VM 메모리 매핑 + sqs/cqs 배열 등록. */
		return handle_create_io_q(ctrlr, cmd,
					  cmd->opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	case SPDK_NVME_OPC_DELETE_IO_SQ:
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		/* [한국어] Queue 삭제도 vfio-user 가 직접 — drain + cq_ref 관리. */
		return handle_del_io_q(ctrlr, cmd,
				       cmd->opc == SPDK_NVME_OPC_DELETE_IO_CQ);
	case SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG:
		SPDK_NOTICELOG("%s: requested shadow doorbells (supported: %d)\n",
			       ctrlr_id(ctrlr),
			       !ctrlr->transport->transport_opts.disable_shadow_doorbells);
		/* [한국어] shadow doorbell 옵션 활성 시만 처리. 비활성 시 fallthrough → 일반 path
		 * (handle_cmd_req → ctrlr layer 가 INVALID_OPCODE 응답). */
		if (!ctrlr->transport->transport_opts.disable_shadow_doorbells) {
			return handle_doorbell_buffer_config(ctrlr, cmd);
		}
	/* FALLTHROUGH */
	default:
		/* [한국어] 그 외 모든 admin 명령 — 공통 NVMe-oF dispatch (Identify/Set Features 등). */
		return handle_cmd_req(ctrlr, cmd, ctrlr->sqs[0]);
	}
}

/*
 * [한국어]
 * handle_cmd_rsp - bdev/admin I/O 완료 후 DMA 매핑 해제 + CQE 게시 콜백.
 *
 * @vu_req: 완료된 vfio-user 요청 (req.rsp 에 응답 채워짐).
 * @cb_arg: SQ 포인터 (cb_arg 로 등록).
 * @return: post_completion 결과 (0 성공, 음수 에러).
 *
 * 동기/배경: handle_cmd_req 에서 vu_req->cb_fn = handle_cmd_rsp 로 등록되어,
 *   bdev I/O 완료 시 nvmf_vfio_user_req_complete → cb_fn 경로로 호출된다.
 *   DMA 매핑(vu_req->iov)을 해제하고, 코어 응답(req.rsp)에서 CQE 를 구성해 게시한다.
 * 동작:
 *   1. iovcnt > 0 이면 vfu_sgl_put 으로 모든 DMA 매핑 해제.
 *   2. post_completion 으로 CQE write + MSI-X trigger.
 * 실행 컨텍스트: SQ 소유 thread (bdev 완료 경로).
 * 호출자: nvmf_vfio_user_req_complete (cb_fn 콜백 경유).
 * 에러 경로: post_completion 실패(CQ 쓰기 오류) → 음수 반환 → fail_ctrlr.
 *
 * 호출 체인:
 *   bdev 완료 → spdk_nvmf_request_complete → nvmf_vfio_user_req_complete
 *     → [handle_cmd_rsp] → vfu_sgl_put + post_completion
 */
static int
handle_cmd_rsp(struct nvmf_vfio_user_req *vu_req, void *cb_arg)
{
	struct nvmf_vfio_user_sq *sq = cb_arg;            /* [한국어] 등록 시 cb_arg = SQ. */
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = sq->ctrlr; /* [한국어] SQ → controller. */
	uint16_t sqid, cqid;

	assert(sq != NULL);
	assert(vu_req != NULL);
	assert(vu_ctrlr != NULL);

	/* [한국어] DMA 매핑 해제 — bdev 가 iov 로 접근 완료 후 libvfio-user reference 반환. */
	if (spdk_likely(vu_req->iovcnt)) {
		vfu_sgl_put(vu_ctrlr->endpoint->vfu_ctx,
			    index_to_sg_t(vu_req->sg, 0),
			    vu_req->iov, vu_req->iovcnt);
	}
	sqid = sq->qid;   /* [한국어] CQE 의 SQID 필드 (어느 SQ 의 명령이었나). */
	cqid = sq->cqid;  /* [한국어] 완료를 보낼 CQ id. */

	/* [한국어] 코어 응답에서 CQE 구성 + VM CQ 에 write + MSI-X interrupt trigger. */
	return post_completion(vu_ctrlr, vu_ctrlr->cqs[cqid],
			       vu_req->req.rsp->nvme_cpl.cdw0,
			       sqid,
			       vu_req->req.cmd->nvme_cmd.cid,
			       vu_req->req.rsp->nvme_cpl.status.sc,
			       vu_req->req.rsp->nvme_cpl.status.sct);
}

/*
 * [한국어]
 * consume_cmd - SQE 1개를 admin 또는 IO 처리 경로로 분기.
 *
 * @ctrlr: controller.
 * @sq: SQE 가 도착한 SQ.
 * @cmd: 처리할 NVMe SQE.
 * @return: 0 성공, 음수 에러.
 *
 * 동기/배경: handle_sq_tdbl_write 가 SQE 를 fetch 한 뒤 호출한다. admin SQ(qid=0) 이면
 *   consume_admin_cmd 로, IO SQ 이면 handle_cmd_req 로 분기해 단일 dispatch 진입점 역할.
 * 동작: nvmf_qpair_is_admin_queue 로 qid=0 검사 → 분기.
 * 실행 컨텍스트: controller thread (handle_sq_tdbl_write 의 SQE 처리 루프).
 * 호출자: handle_sq_tdbl_write.
 * 에러 경로: 없음 (하위 함수가 처리).
 *
 * 호출 체인:
 *   handle_sq_tdbl_write → [consume_cmd]
 *     → consume_admin_cmd (admin SQ) 또는 handle_cmd_req (IO SQ)
 */
static int
consume_cmd(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq,
	    struct spdk_nvme_cmd *cmd)
{
	assert(sq != NULL);
	/* [한국어] admin SQ (qid=0) 이면 Create/Delete Queue, Identify, Set/Get Features 등 admin 분기. */
	if (spdk_unlikely(nvmf_qpair_is_admin_queue(&sq->qpair))) {
		return consume_admin_cmd(ctrlr, cmd);
	}

	/* [한국어] IO SQ — bdev Read/Write/Flush/DSM/Compare 등. */
	return handle_cmd_req(ctrlr, cmd, sq);
}

/*
 * [한국어]
 * handle_sq_tdbl_write - ★ SQ doorbell 갱신 시 head..new_tail 구간의 SQE 일괄 처리.
 *
 * @ctrlr: controller.
 * @new_tail: VM 이 write 한 새 tail 값 (doorbell 또는 shadow_doorbell 에서 read).
 * @sq: 대상 SQ.
 * @return: 처리한 SQE 수 (>=0) 또는 음수 errno.
 *
 * sq_poll 이 새 tail 발견 시 호출. 핵심 loop: head != new_tail 동안:
 *   1. CQ free slots 검사 (flow control):
 *      - free_cq_slots <= nr_outstanding 이면 SQ 처리 중단 (cq full 회피).
 *      - VM 의 CQ head 갱신 (last_head 재조회) → free 확보됐는지 retry.
 *      - 그래도 부족하면 break + cq_full 통계 + interrupt mode 에선 need_kick.
 *   2. SQE pointer = queue[head] (host shared memory direct read).
 *   3. nr_outstanding++ + sq_head_advance (CQE 의 SQHD 필드 정확성 보장).
 *   4. consume_cmd → handle_cmd_req (IO) 또는 consume_admin_cmd (admin).
 *
 * Flow control 중요성: VM (Linux NVMe driver) 가 가끔 free CQ slot 보다 많이 enqueue —
 * SPDK 가 spec 따라 CQ overflow 방어. NVMe spec § 3.3.1.2.1: host 가 CQ 비우는 속도가
 * 충분해야 하지만 backpressure 메커니즘이 약함.
 *
 * sq_head_advance 가 consume_cmd 전인 이유: handle_cmd_req → post_completion 이 사용할
 * CQE 의 sqhd 필드가 "다음 SQE 의 head" 여야 함 (NVMe spec § 4.6).
 *
 * shadow doorbell + need_rearm: shadow doorbell 사용 시 매 처리 후 eventidx 재설정 필요
 * (다음 doorbell write 가 통지 트리거되게).
 *
 * 호출 체인:
 *   sq_poll → [본 함수] → consume_cmd → handle_cmd_req → request_exec
 */
/* Returns the number of commands processed, or a negative value on error. */
static int
handle_sq_tdbl_write(struct nvmf_vfio_user_ctrlr *ctrlr, const uint32_t new_tail,
		     struct nvmf_vfio_user_sq *sq)
{
	struct spdk_nvme_cmd *queue;
	struct nvmf_vfio_user_cq *cq = ctrlr->cqs[sq->cqid];
	int count = 0;
	uint32_t free_cq_slots;

	assert(ctrlr != NULL);
	assert(sq != NULL);

	/* [한국어] shadow doorbell 활성 + IO SQ — eventidx 재설정 필요 (admin SQ 는 항상 polled). */
	if (ctrlr->sdbl != NULL && sq->qid != 0) {
		/*
		 * Submission queue index has moved past the event index, so it
		 * needs to be re-armed before we go to sleep.
		 */
		sq->need_rearm = true;
	}

	/* [한국어] flow control 초기값 + SQ memory pointer. */
	free_cq_slots = cq_free_slots(cq);
	queue = q_addr(&sq->mapping);
	while (*sq_headp(sq) != new_tail) {
		int err;
		struct spdk_nvme_cmd *cmd;

		/*
		 * At least the Linux nvme driver can submit more requests than
		 * our current view of the available free CQ slots, although it
		 * is not clear exactly why or how; it is relatively rare even
		 * under high load.
		 *
		 * As we need to make sure we have free CQ slots (see
		 * post_completion()), we implement flow control here: if the
		 * number of currently outstanding requests for this SQ would
		 * use all the available CQ slots, then we cannot submit this
		 * new request.
		 *
		 * Instead we back off until the driver has informed us that CQ
		 * slots are available.
		 */
		if ((free_cq_slots-- <= cq->nr_outstanding)) {
			struct nvmf_vfio_user_poll_group *vu_group;
			cq->last_head = *cq_dbl_headp(cq);

			free_cq_slots = cq_free_slots(cq);
			if (free_cq_slots > cq->nr_outstanding) {
				continue;
			}

			vu_group = sq_to_poll_group(sq);

			vu_group->stats.cq_full++;

			/*
			 * There are no free CQ slots, so stop processing
			 * submissions for this SQ until "a later time". In
			 * interrupt mode, we need to kick ourselves, so that we
			 * are guaranteed to wake up and come back here.
			 */
			if (in_interrupt_mode(ctrlr->transport)) {
				vu_group->need_kick = true;
			}
			break;
		}

		cmd = &queue[*sq_headp(sq)];
		count++;

		cq->nr_outstanding++;

		/*
		 * SQHD must contain the new head pointer, so we must increase
		 * it before we generate a completion.
		 */
		sq_head_advance(sq);

		err = consume_cmd(ctrlr, sq, cmd);
		if (spdk_unlikely(err != 0)) {
			return err;
		}
	}

	return count;
}

/* Checks whether endpoint is connected from the same process */
/*
 * [한국어]
 * is_peer_same_process - vfio-user socket 의 peer 가 같은 프로세스인지 확인.
 *
 * @endpoint: 확인할 endpoint.
 * @return: true = 같은 프로세스, false = 다른 프로세스 또는 endpoint NULL.
 *
 * 동기/배경: memory_region_add_cb 에서 spdk_mem_register 가 필요한지 결정할 때 사용한다.
 *   같은 프로세스(예: unit test, 로컬 시뮬레이션)이면 메모리가 이미 공유되어 있어
 *   register 가 불필요하고 중복 시 오류가 될 수 있다.
 *   실제 VM(다른 프로세스)에서 오면 DMA 매핑 자원으로 SPDK 에 등록해야 한다.
 * 동작: vfu_get_poll_fd 로 socket fd 얻어 getsockopt(SO_PEERCRED) 로 peer pid 확인 →
 *   getpid() 와 비교.
 * 실행 컨텍스트: memory_region_add_cb (vfu_run_ctx 콜백 경로).
 * 호출자: memory_region_add_cb.
 * 에러 경로: getsockopt 실패 → false (보수적 처리 = 다른 프로세스로 가정).
 *
 * 호출 체인:
 *   memory_region_add_cb → [is_peer_same_process] → getsockopt(SO_PEERCRED)
 */
static bool
is_peer_same_process(struct nvmf_vfio_user_endpoint *endpoint)
{
	struct ucred ucred;  /* [한국어] SO_PEERCRED 응답 — peer 의 pid/uid/gid. */
	socklen_t ucredlen = sizeof(ucred);

	if (endpoint == NULL) {
		return false;  /* [한국어] endpoint 없으면 보수적으로 false. */
	}

	if (getsockopt(vfu_get_poll_fd(endpoint->vfu_ctx), SOL_SOCKET, SO_PEERCRED, &ucred,
		       &ucredlen) < 0) {
		SPDK_ERRLOG("getsockopt(SO_PEERCRED): %s\n", strerror(errno));
		return false;
	}

	return ucred.pid == getpid();
}

/*
 * [한국어]
 * memory_region_add_cb - VM 이 host memory region 등록 시 libvfio-user 콜백.
 *
 * @vfu_ctx: libvfio-user context.
 * @info: 추가된 DMA 영역 정보 (iova/vaddr/len/prot).
 *
 * VM 이 부팅 시 자기 RAM 을 vfio-pci 통해 DMA 가능 영역으로 등록 → libvfio-user 가 본 콜백
 * 호출 → SPDK 가 그 영역을:
 *   1. spdk_mem_register 로 SPDK 메모리 풀에 추가 (DMA 매핑 IOVA = host VA).
 *   2. INACTIVE 상태로 있던 SQ/CQ 중 이 영역에 속하는 것 재매핑 → ACTIVE 전이.
 *
 * 2MB 정렬 검증: SPDK 의 memory map 은 2MB 단위 (hugepage) — non-aligned 영역 등록 거부.
 *
 * is_peer_same_process: vfio-user client/server 가 같은 process 면 메모리 share 자체가
 * 같은 mmap → register 불필요. 다른 process (보통 VM) 일 때만 register.
 *
 * INACTIVE → ACTIVE 재매핑: VM 이 reboot 등으로 memory layout 바꾸면 이전 SQ/CQ 매핑이
 * 무효화돼 unmap_q + INACTIVE 마킹된 상태. 새 region 추가 시 그 안에 SQ/CQ 가 다시
 * 들어오면 map_q 로 활성화. shared CQ 는 한 번만 매핑 (q_addr 검사).
 *
 * 호출 체인:
 *   VM DMA register → vfio-user message → libvfio-user → [본 함수]
 *     → spdk_mem_register + SQ/CQ remap → INACTIVE → ACTIVE
 */
static void
memory_region_add_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr;
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;
	void *map_start, *map_end;
	int ret;

	/*
	 * We're not interested in any DMA regions that aren't mappable (we don't
	 * support clients that don't share their memory).
	 */
	/* [한국어] vaddr=NULL = client 가 memory share 미지원 (SPDK 가 자기 VA 로 못 봄). skip. */
	if (!info->vaddr) {
		return;
	}

	map_start = info->mapping.iov_base;
	map_end = info->mapping.iov_base + info->mapping.iov_len;

	/* [한국어] 2MB 정렬 강제 — SPDK memory map 의 단위. non-aligned 거부. */
	if (((uintptr_t)info->mapping.iov_base & MASK_2MB) ||
	    (info->mapping.iov_len & MASK_2MB)) {
		SPDK_DEBUGLOG(nvmf_vfio, "Invalid memory region vaddr %p, IOVA %p-%p\n",
			      info->vaddr, map_start, map_end);
		return;
	}

	assert(endpoint != NULL);
	/* [한국어] controller 없으면 = 아직 connect 안 됨 — DMA register 만 처리, SQ/CQ remap 무관. */
	if (endpoint->ctrlr == NULL) {
		return;
	}
	ctrlr = endpoint->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: map IOVA %p-%p\n", endpoint_id(endpoint),
		      map_start, map_end);

	/* VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE are enabled when registering to VFIO, here we also
	 * check the protection bits before registering. When vfio client and server are run in same process
	 * there is no need to register the same memory again.
	 */
	if (info->prot == (PROT_WRITE | PROT_READ) && !is_peer_same_process(endpoint)) {
		ret = spdk_mem_register(info->mapping.iov_base, info->mapping.iov_len);
		if (ret) {
			SPDK_ERRLOG("Memory region register %p-%p failed, ret=%d\n",
				    map_start, map_end, ret);
		}
	}

	pthread_mutex_lock(&endpoint->lock);
	TAILQ_FOREACH(sq, &ctrlr->connected_sqs, tailq) {
		if (sq->sq_state != VFIO_USER_SQ_INACTIVE) {
			continue;
		}

		cq = ctrlr->cqs[sq->cqid];

		/* For shared CQ case, we will use q_addr() to avoid mapping CQ multiple times */
		if (cq->size && q_addr(&cq->mapping) == NULL) {
			ret = map_q(ctrlr, &cq->mapping, MAP_RW | MAP_QUIET);
			if (ret) {
				SPDK_DEBUGLOG(nvmf_vfio, "Memory isn't ready to remap cqid:%d %#lx-%#lx\n",
					      cq->qid, cq->mapping.prp1,
					      cq->mapping.prp1 + cq->mapping.len);
				continue;
			}
		}

		if (sq->size) {
			ret = map_q(ctrlr, &sq->mapping, MAP_R | MAP_QUIET);
			if (ret) {
				SPDK_DEBUGLOG(nvmf_vfio, "Memory isn't ready to remap sqid:%d %#lx-%#lx\n",
					      sq->qid, sq->mapping.prp1,
					      sq->mapping.prp1 + sq->mapping.len);
				continue;
			}
		}
		sq->sq_state = VFIO_USER_SQ_ACTIVE;
		SPDK_DEBUGLOG(nvmf_vfio, "Remap sqid:%u successfully\n", sq->qid);
	}
	pthread_mutex_unlock(&endpoint->lock);
}

/*
 * [한국어]
 * memory_region_remove_cb - VM 이 host memory region 제거 시 libvfio-user 콜백.
 *
 * @vfu_ctx: libvfio-user context.
 * @info: 제거되는 DMA 영역 정보.
 *
 * VM 의 memory hot-unplug 또는 reboot 으로 영역이 무효화될 때 호출. SPDK 가 그 영역에
 * 매핑됐던 SQ/CQ 를 unmap_q + INACTIVE 마킹 → 다음 add 시 재매핑 가능.
 *
 * Shadow doorbell 특수 처리: shadow doorbell buffer 가 제거되는 region 에 속하면
 * 1) bar0_doorbells 로 fallback 전환 (copy_doorbells 로 누적값 보존)
 * 2) free_sdbl 로 자원 해제
 * → poll 모드로 복귀 (shadow doorbell 미사용 상태).
 *
 * spdk_mem_unregister: add_cb 와 대칭. same-process 면 skip.
 *
 * 호출 체인:
 *   VM DMA unregister → libvfio-user → [본 함수]
 *     → unmap_q (SQ/CQ) + sq_state=INACTIVE
 *     → shadow doorbell 영역이면 fallback + free_sdbl
 *     → spdk_mem_unregister
 */
static void
memory_region_remove_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;
	void *map_start, *map_end;
	int ret = 0;

	if (!info->vaddr) {
		return;
	}

	map_start = info->mapping.iov_base;
	map_end = info->mapping.iov_base + info->mapping.iov_len;

	if (((uintptr_t)info->mapping.iov_base & MASK_2MB) ||
	    (info->mapping.iov_len & MASK_2MB)) {
		SPDK_DEBUGLOG(nvmf_vfio, "Invalid memory region vaddr %p, IOVA %p-%p\n",
			      info->vaddr, map_start, map_end);
		return;
	}

	assert(endpoint != NULL);
	SPDK_DEBUGLOG(nvmf_vfio, "%s: unmap IOVA %p-%p\n", endpoint_id(endpoint),
		      map_start, map_end);

	if (endpoint->ctrlr != NULL) {
		struct nvmf_vfio_user_ctrlr *ctrlr;
		ctrlr = endpoint->ctrlr;

		pthread_mutex_lock(&endpoint->lock);
		TAILQ_FOREACH(sq, &ctrlr->connected_sqs, tailq) {
			if (q_addr(&sq->mapping) >= map_start && q_addr(&sq->mapping) <= map_end) {
				unmap_q(ctrlr, &sq->mapping);
				sq->sq_state = VFIO_USER_SQ_INACTIVE;
			}

			cq = ctrlr->cqs[sq->cqid];
			if (q_addr(&cq->mapping) >= map_start && q_addr(&cq->mapping) <= map_end) {
				unmap_q(ctrlr, &cq->mapping);
			}
		}

		if (ctrlr->sdbl != NULL) {
			size_t i;

			for (i = 0; i < NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT; i++) {
				const void *const iov_base = ctrlr->sdbl->iovs[i].iov_base;

				if (iov_base >= map_start && iov_base < map_end) {
					copy_doorbells(ctrlr,
						       ctrlr->sdbl->shadow_doorbells,
						       ctrlr->bar0_doorbells);
					vfio_user_ctrlr_switch_doorbells(ctrlr, false);
					free_sdbl(endpoint->vfu_ctx, ctrlr->sdbl);
					ctrlr->sdbl = NULL;
					break;
				}
			}
		}

		pthread_mutex_unlock(&endpoint->lock);
	}

	if (info->prot == (PROT_WRITE | PROT_READ) && !is_peer_same_process(endpoint)) {
		ret = spdk_mem_unregister(info->mapping.iov_base, info->mapping.iov_len);
		if (ret) {
			SPDK_ERRLOG("Memory region unregister %p-%p failed, ret=%d\n",
				    map_start, map_end, ret);
		}
	}
}

/* Used to initiate a controller-level reset or a controller shutdown. */
/*
 * [한국어]
 * disable_ctrlr - controller 를 비활성(reset 또는 shutdown)으로 전환.
 *
 * @vu_ctrlr: 비활성화할 controller.
 *
 * 동기/배경: VM 이 CC.EN=0 write(disable) 또는 CC.SHN 으로 shutdown 을 요청하면
 *   nvmf_vfio_user_prop_req_rsp_set → disable_ctrlr 이 호출된다. NVMe spec § 3.5.4:
 *   controller disable 시 admin queue 를 unmap 하고 shadow doorbell 도 해제한다.
 *   IO qpair 들은 별도 disconnect 경로로 처리되고, admin queue 는 여기서 즉시 정리.
 * 동작:
 *   1. admin SQ/CQ unmap.
 *   2. admin SQ size=0 + head=0 + state=INACTIVE.
 *   3. admin CQ size=0 + tail=0.
 *   4. AER(Asynchronous Event Request) 응답 드롭 (reset/shutdown 시 AER 불필요).
 *   5. shadow doorbell 해제 (switch_doorbells → free_sdbl).
 * 실행 컨텍스트: nvmf_vfio_user_prop_req_rsp_set 콜백 경로 (controller thread).
 * 호출자: nvmf_vfio_user_prop_req_rsp_set (CC.EN=0 또는 CC.SHN 감지 시).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   nvmf_vfio_user_prop_req_rsp_set → [disable_ctrlr]
 *     → unmap_q × 2 / spdk_nvmf_ctrlr_abort_aer / free_sdbl
 */
static void
disable_ctrlr(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	SPDK_NOTICELOG("%s: disabling controller\n", ctrlr_id(vu_ctrlr));

	/* Unmap Admin queue. */

	assert(vu_ctrlr->sqs[0] != NULL);
	assert(vu_ctrlr->cqs[0] != NULL);

	unmap_q(vu_ctrlr, &vu_ctrlr->sqs[0]->mapping);  /* [한국어] admin SQ DMA 매핑 해제. */
	unmap_q(vu_ctrlr, &vu_ctrlr->cqs[0]->mapping);  /* [한국어] admin CQ DMA 매핑 해제. */

	vu_ctrlr->sqs[0]->size = 0;  /* [한국어] admin SQ 크기 0 (미활성 표시). */
	*sq_headp(vu_ctrlr->sqs[0]) = 0;  /* [한국어] head pointer 초기화. */

	vu_ctrlr->sqs[0]->sq_state = VFIO_USER_SQ_INACTIVE;

	vu_ctrlr->cqs[0]->size = 0;
	*cq_tailp(vu_ctrlr->cqs[0]) = 0;

	/*
	 * For PCIe controller reset or shutdown, we will drop all AER
	 * responses.
	 */
	spdk_nvmf_ctrlr_abort_aer(vu_ctrlr->ctrlr);

	/* Free the shadow doorbell buffer. */
	vfio_user_ctrlr_switch_doorbells(vu_ctrlr, false);
	free_sdbl(vu_ctrlr->endpoint->vfu_ctx, vu_ctrlr->sdbl);
	vu_ctrlr->sdbl = NULL;
}

/* Used to re-enable the controller after a controller-level reset. */
/*
 * [한국어]
 * enable_ctrlr - controller reset 후 CC.EN=1 로 재활성화.
 *
 * @vu_ctrlr: 재활성화할 controller.
 * @return: 0 성공, 음수 에러(acq_setup 또는 asq_setup 실패).
 *
 * 동기/배경: VM 이 disable 후 다시 CC.EN=1 write 시(controller reset 완료 후 재init)
 *   nvmf_vfio_user_prop_req_rsp_set → enable_ctrlr 이 호출된다. NVMe spec § 3.5.3:
 *   CC.EN=1 → AQA/ASQ/ACQ 레지스터가 유효해지므로 admin queue 를 재매핑한다.
 * 동작: acq_setup → asq_setup → sqs[0]->sq_state=ACTIVE.
 * 실행 컨텍스트: nvmf_vfio_user_prop_req_rsp_set 콜백 경로 (controller thread).
 * 호출자: nvmf_vfio_user_prop_req_rsp_set (CC.EN 0→1 전환 감지 시).
 * 에러 경로: acq/asq_setup 실패 → 음수 반환 → 호출자가 fail_ctrlr.
 *
 * 호출 체인:
 *   nvmf_vfio_user_prop_req_rsp_set → [enable_ctrlr] → acq_setup → asq_setup
 */
static int
enable_ctrlr(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	int err;

	assert(vu_ctrlr != NULL);

	SPDK_NOTICELOG("%s: enabling controller\n", ctrlr_id(vu_ctrlr));

	err = acq_setup(vu_ctrlr);
	if (err != 0) {
		return err;
	}

	err = asq_setup(vu_ctrlr);
	if (err != 0) {
		return err;
	}

	vu_ctrlr->sqs[0]->sq_state = VFIO_USER_SQ_ACTIVE;

	return 0;
}

/*
 * [한국어]
 * nvmf_vfio_user_prop_req_rsp_set - Fabric Property Set 완료 콜백 — CC 레지스터 변경 후처리.
 *
 * @req: 완료된 Property Set 요청 (req.cmd 에 CC write 값 보유).
 * @sq: admin SQ (ctrlr 역참조에 사용).
 * @return: 0 성공, enable_ctrlr 실패 시 음수.
 *
 * 동기/배경: vfio_user_property_access 가 BAR0 의 CC write 를 Fabric Property Set capsule 로
 *   변환해 코어에 dispatch 하면, 코어가 처리 후 이 콜백을 호출한다. 코어는 CC 의 새 값을
 *   자신의 상태에 반영했으므로, vfio-user transport 가 CC 변경을 감지해 enable/disable 을
 *   수행해야 한다. req->cc 에 저장된 "이전 CC" 와 새 CC 를 XOR 해 변경 비트를 찾는다.
 * 동작:
 *   - CC.EN 0→1: enable_ctrlr (admin SQ/CQ 재설정, state=ACTIVE).
 *   - CC.EN 1→0 또는 CC.SHN 설정: disable_ctrlr (admin Q unmap, AER 드롭, sdbl 해제).
 *   - CC.EN/SHN 외 필드 변경 (CC.MPS 등): 처리 없음 (코어가 관리).
 * 실행 컨텍스트: admin SQ 소유 thread (Property Set 완료 콜백).
 * 호출자: nvmf_vfio_user_prop_req_rsp.
 * 에러 경로: enable_ctrlr 실패 → 음수 반환 → fail_ctrlr 트리거.
 *
 * 호출 체인:
 *   (CC write) vfio_user_property_access → Fabric Property Set → (완료) nvmf_vfio_user_prop_req_rsp
 *     → [nvmf_vfio_user_prop_req_rsp_set] → enable_ctrlr 또는 disable_ctrlr
 */
static int
nvmf_vfio_user_prop_req_rsp_set(struct nvmf_vfio_user_req *req,
				struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	union spdk_nvme_cc_register cc, diff;  /* [한국어] 새 CC 값 / XOR 차이(변경된 비트). */

	assert(req->req.cmd->prop_set_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET);
	assert(sq->ctrlr != NULL);
	vu_ctrlr = sq->ctrlr;

	if (req->req.cmd->prop_set_cmd.ofst != offsetof(struct spdk_nvme_registers, cc)) {
		return 0;
	}

	cc.raw = req->req.cmd->prop_set_cmd.value.u64;
	diff.raw = cc.raw ^ req->cc.raw;

	if (diff.bits.en) {
		if (cc.bits.en) {
			int ret = enable_ctrlr(vu_ctrlr);
			if (ret) {
				SPDK_ERRLOG("%s: failed to enable ctrlr\n", ctrlr_id(vu_ctrlr));
				return ret;
			}
			vu_ctrlr->reset_shn = false;
		} else {
			vu_ctrlr->reset_shn = true;
		}
	}

	if (diff.bits.shn) {
		if (cc.bits.shn == SPDK_NVME_SHN_NORMAL || cc.bits.shn == SPDK_NVME_SHN_ABRUPT) {
			vu_ctrlr->reset_shn = true;
		}
	}

	if (vu_ctrlr->reset_shn) {
		disable_ctrlr(vu_ctrlr);
	}
	return 0;
}

/*
 * [한국어]
 * nvmf_vfio_user_prop_req_rsp - Fabric Property Get/Set 완료 통합 콜백.
 *
 * @req: 완료된 Property Get 또는 Set 요청.
 * @cb_arg: admin SQ 포인터.
 * @return: 0 성공, 음수 실패.
 *
 * 동기/배경: vfio_user_property_access 가 BAR0 NVMe register read/write 를 Fabric
 *   Property Get/Set 으로 변환해 코어에 dispatch 하면, 코어가 처리 후 이 콜백 호출.
 *   Get 이면 코어 응답값을 BAR 접근 buffer 에 복사(호출자가 읽게), Set 이면 후처리.
 * 동작:
 *   - Property Get(fctype=GET): req.rsp->prop_get_rsp.value.u64 를 req.iov[0] 에 복사.
 *   - Property Set(fctype=SET): nvmf_vfio_user_prop_req_rsp_set 으로 위임 (CC 변경 처리).
 * 실행 컨텍스트: admin SQ 소유 thread.
 * 호출자: nvmf_vfio_user_req_complete (cb_fn 경유).
 * 에러 경로: Set 콜백 실패 시 음수 반환.
 *
 * 호출 체인:
 *   vfio_user_property_access → req dispatch → (완료) nvmf_vfio_user_req_complete
 *     → [nvmf_vfio_user_prop_req_rsp] → (Set) nvmf_vfio_user_prop_req_rsp_set
 */
static int
nvmf_vfio_user_prop_req_rsp(struct nvmf_vfio_user_req *req, void *cb_arg)
{
	struct nvmf_vfio_user_sq *sq = cb_arg;  /* [한국어] cb_arg = admin SQ. */

	assert(sq != NULL);
	assert(req != NULL);

	if (req->req.cmd->prop_get_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET) {
		assert(sq->ctrlr != NULL);
		assert(req != NULL);

		memcpy(req->req.iov[0].iov_base,
		       &req->req.rsp->prop_get_rsp.value.u64,
		       req->req.length);
		return 0;
	}

	return nvmf_vfio_user_prop_req_rsp_set(req, sq);
}

/*
 * Handles a write at offset 0x1000 or more; this is the non-mapped path when a
 * doorbell is written via access_bar0_fn().
 *
 * DSTRD is set to fixed value 0 for NVMf.
 *
 */
/*
 * [한국어]
 * handle_dbl_access - BAR0 0x1000+ 영역 (doorbell) write 처리. mappable BAR 미사용 시 path.
 *
 * @ctrlr: controller.
 * @buf: doorbell 값 (4B).
 * @count: 항상 4 (single doorbell).
 * @pos: BAR0 offset (>= 0x1000).
 * @is_write: doorbell 은 write-only — read 는 EPERM.
 * @return: 0 성공, -1 errno 설정.
 *
 * mappable BAR mmap 사용 시 VM 의 doorbell write 가 BAR exit 없이 그냥 page write 됨 →
 * 본 함수 호출되지 않고 SPDK 가 polling. mappable 비활성 시 매 doorbell write 가 VFIO 통해
 * 본 함수 호출 (slow path, 디버깅 / shadow doorbell 미지원 호스트용).
 *
 * 단계:
 *   1. 입력 검증 (write only, 4B, dword aligned).
 *   2. pos → array index 변환 ((pos - 0x1000) >> 2):
 *      - 짝수 = SQ tail doorbell (qid = index/2)
 *      - 홀수 = CQ head doorbell (qid = (index-1)/2)
 *   3. doorbell 값을 bar0_doorbells[index] 에 write — SPDK polling 이 이를 본다.
 *   4. shadow doorbell 활성 시 same write 도 shadow 에 mirror.
 *
 * DSTRD=0 의미: doorbell stride 0 = 인접 doorbell 사이 4B (최소). NVMe spec § 3.1.13.
 * NVMf 는 항상 DSTRD=0 으로 advertise.
 */
static int
handle_dbl_access(struct nvmf_vfio_user_ctrlr *ctrlr, uint32_t *buf,
		  const size_t count, loff_t pos, const bool is_write)
{
	struct nvmf_vfio_user_poll_group *group;

	assert(ctrlr != NULL);
	assert(buf != NULL);

	/* [한국어] doorbell 은 NVMe spec § 3.1.13 상 write-only — host 가 read 하면 spec 위반. */
	if (spdk_unlikely(!is_write)) {
		SPDK_WARNLOG("%s: host tried to read BAR0 doorbell %#lx\n",
			     ctrlr_id(ctrlr), pos);
		errno = EPERM;
		return -1;
	}

	/* [한국어] doorbell 은 단일 dword (4B) write — 다른 크기는 spec 위반. */
	if (spdk_unlikely(count != sizeof(uint32_t))) {
		SPDK_ERRLOG("%s: bad doorbell buffer size %ld\n",
			    ctrlr_id(ctrlr), count);
		errno = EINVAL;
		return -1;
	}

	pos -= NVME_DOORBELLS_OFFSET;

	/* pos must be dword aligned */
	/* [한국어] dword 정렬 검증 — non-aligned write 는 정의되지 않음. */
	if (spdk_unlikely((pos & 0x3) != 0)) {
		SPDK_ERRLOG("%s: bad doorbell offset %#lx\n", ctrlr_id(ctrlr), pos);
		errno = EINVAL;
		return -1;
	}

	/* convert byte offset to array index */
	/* [한국어] byte → dword index 변환. 짝수 = SQ tail, 홀수 = CQ head (NVMe doorbell layout). */
	pos >>= 2;

	if (spdk_unlikely(pos >= NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR * 2)) {
		SPDK_ERRLOG("%s: bad doorbell index %#lx\n", ctrlr_id(ctrlr), pos);
		errno = EINVAL;
		return -1;
	}

	ctrlr->bar0_doorbells[pos] = *buf;
	spdk_wmb();

	group = ctrlr_to_poll_group(ctrlr);
	if (pos == 1) {
		group->stats.cqh_admin_writes++;
	} else if (pos & 1) {
		group->stats.cqh_io_writes++;
	}

	SPDK_DEBUGLOG(vfio_user_db, "%s: updating BAR0 doorbell %s:%ld to %u\n",
		      ctrlr_id(ctrlr), (pos & 1) ? "cqid" : "sqid",
		      pos / 2, *buf);


	return 0;
}

/*
 * [한국어]
 * vfio_user_property_access - BAR0 0x00~0x0FFF NVMe register read/write 처리.
 *
 * @vu_ctrlr: controller.
 * @buf: data buffer (4B or 8B).
 * @count: 4 또는 8 (CAP/ASQ/ACQ 등 64-bit register).
 * @pos: register offset (CC=0x14, CSTS=0x1C, AQA=0x24, ASQ=0x28, ACQ=0x30, ...).
 * @is_write: true = VM 이 register write.
 * @return: 처리 바이트 수 또는 -1.
 *
 * ★ 핵심 트릭 ★: vfio-user 는 별도 capsule transport 없지만, NVMe-oF 공통 layer 의
 * NVMe register access 는 모두 Fabric Property Get/Set capsule 로 라우팅돼 있음.
 * 본 함수가 BAR access 를 in-memory Fabric Property capsule 로 변환해 spdk_nvmf_request_exec
 * 으로 dispatch — RDMA/TCP/vfio-user 가 같은 ctrlr layer 코드 공유.
 *
 * CC write 의 의미:
 *   - CC.EN: 0→1 = controller enable (admin SQ/CQ 활성), 1→0 = disable.
 *   - CC.SHN: 1=normal shutdown, 2=abrupt → CSTS.SHST 변환.
 *   - 이런 transition 은 lib/nvmf/ctrlr.c 의 nvmf_property_set_cc 에서 처리.
 *
 * req->cc 보존: prop_set_cc 처리 후 cb_fn 이 "before vs after CC" 비교해 enable/disable
 * transition 감지하는 데 사용 (vfio_user 만의 특수 필드).
 *
 * 호출 체인:
 *   access_bar0_fn → (pos < 0x1000) → [본 함수]
 *     → Fabric Property Get/Set capsule build
 *     → spdk_nvmf_request_exec → lib/nvmf/ctrlr.c nvmf_property_*
 *     → nvmf_vfio_user_prop_req_rsp 콜백 → access_bar0_fn 반환
 */
static size_t
vfio_user_property_access(struct nvmf_vfio_user_ctrlr *vu_ctrlr,
			  char *buf, size_t count, loff_t pos,
			  bool is_write)
{
	struct nvmf_vfio_user_req *req;
	const struct spdk_nvmf_registers *regs;

	/* [한국어] 4B (CC/CSTS 등) 또는 8B (CAP/ASQ/ACQ) 만 — 다른 크기는 NVMe spec 위반. */
	if ((count != 4) && (count != 8)) {
		errno = EINVAL;
		return -1;
	}

	/* Construct a Fabric Property Get/Set command and send it */
	/* [한국어] admin SQ 의 free request 풀에서 1개 빌림. property 처리는 항상 admin SQ. */
	req = get_nvmf_vfio_user_req(vu_ctrlr->sqs[0]);
	if (req == NULL) {
		errno = ENOBUFS;
		return -1;
	}
	regs = spdk_nvmf_ctrlr_get_regs(vu_ctrlr->ctrlr);
	/* [한국어] prop_set_cc 처리 후 cb_fn 이 "before vs after CC" 비교에 사용. */
	req->cc.raw = regs->cc.raw;

	req->cb_fn = nvmf_vfio_user_prop_req_rsp;
	req->cb_arg = vu_ctrlr->sqs[0];
	req->req.cmd->prop_set_cmd.opcode = SPDK_NVME_OPC_FABRIC;   /* 0x7F */
	req->req.cmd->prop_set_cmd.cid = 0;
	/* [한국어] attrib.size: 0 = 4B property, 1 = 8B property. NVMe-oF spec § 6.2. */
	if (count == 4) {
		req->req.cmd->prop_set_cmd.attrib.size = 0;
	} else {
		req->req.cmd->prop_set_cmd.attrib.size = 1;
	}
	req->req.cmd->prop_set_cmd.ofst = pos;
	if (is_write) {
		/* [한국어] Fabric Property Set fctype 1 = SET. value 필드에 write 데이터. */
		req->req.cmd->prop_set_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET;
		if (req->req.cmd->prop_set_cmd.attrib.size) {
			req->req.cmd->prop_set_cmd.value.u64 = *(uint64_t *)buf;
		} else {
			req->req.cmd->prop_set_cmd.value.u32.high = 0;
			req->req.cmd->prop_set_cmd.value.u32.low = *(uint32_t *)buf;
		}
	} else {
		/* [한국어] Property Get fctype 4 = GET. 응답이 prop_get_rsp.value 에 옴. */
		req->req.cmd->prop_get_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET;
	}
	req->req.length = count;
	/* [한국어] single-iov 설정 — buf 가 응답 받을 위치 (read) 또는 데이터 source (write). */
	SPDK_IOV_ONE(req->req.iov, &req->req.iovcnt, buf, req->req.length);

	/* [한국어] 공통 NVMe-oF dispatch — ctrlr.c 가 BAR 종류별 처리. */
	spdk_nvmf_request_exec(&req->req);

	return count;
}

/*
 * [한국어]
 * access_bar0_fn - VM 의 BAR0 read/write 트래핑 핸들러.
 *
 * @vfu_ctx: libvfio-user context.
 * @buf: 데이터 buffer (read 면 채울 곳, write 면 데이터 source).
 * @count: 바이트 수.
 * @pos: BAR0 내 offset (0x00 ~ NVME_REG_BAR0_SIZE).
 * @is_write: true = VM 이 write, false = VM 이 read.
 * @return: 처리한 바이트 수 (양수) 또는 -1 (errno 설정).
 *
 * libvfio-user 가 VM 의 BAR0 access 를 trap 해 본 함수 호출. NVMe register space:
 *   - 0x00 ~ 0x0FFF: NVMe register (CAP/VS/INTMS/INTMC/CC/CSTS/AQA/ASQ/ACQ/CMBLOC/CMBSZ ...)
 *   - 0x1000 ~ : doorbell (SQ tail / CQ head 짝, qid 별)
 *
 * 분기:
 *   1. pos >= 0x1000 → doorbell 영역 → handle_dbl_access (qid 별 SQ tail or CQ head 업데이트).
 *   2. pos < 0x1000  → register 영역 → vfio_user_property_access
 *      (CC write 면 enable/disable 처리, AQA/ASQ/ACQ write 면 admin queue 셋업).
 *
 * mappable BAR 의 trap 회피: BAR0 을 mmap 한 VM 은 doorbell write 가 BAR exit 없이 그냥
 * page write — 본 함수 호출되지 않음. disable_mappable_bar0=true 또는 shadow doorbell
 * 미사용 시 본 함수가 매 doorbell write 마다 호출됨 (성능 저하).
 *
 * 호출 체인:
 *   VM BAR0 read/write → vfio-pci → vfio-user socket → libvfio-user → [본 함수]
 *     → handle_dbl_access (doorbell) 또는 vfio_user_property_access (register)
 */
static ssize_t
access_bar0_fn(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t pos,
	       bool is_write)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr;
	int ret;

	ctrlr = endpoint->ctrlr;
	if (spdk_unlikely(endpoint->need_async_destroy || !ctrlr)) {
		errno = EIO;
		return -1;
	}

	/* [한국어] 0x1000 부터 doorbell 영역 — qid 별로 SQ tail (짝수 offset), CQ head (홀수). */
	if (pos >= NVME_DOORBELLS_OFFSET) {
		/*
		 * The fact that the doorbells can be memory mapped doesn't mean
		 * that the client (VFIO in QEMU) is obliged to memory map them,
		 * it might still elect to access them via regular read/write;
		 * we might also have had disable_mappable_bar0 set.
		 */
		ret = handle_dbl_access(ctrlr, (uint32_t *)buf, count,
					pos, is_write);
		if (ret == 0) {
			return count;
		}
		return ret;
	}

	/* [한국어] 0x00~0x0FFF: NVMe register space (CAP/CC/CSTS/AQA/ASQ/ACQ 등) — property access. */
	return vfio_user_property_access(ctrlr, buf, count, pos, is_write);
}

/*
 * [한국어]
 * access_pci_config - PCI config space read/write libvfio-user 콜백.
 *
 * @vfu_ctx: libvfio-user context (endpoint 역참조에 사용).
 * @buf: 읽은 데이터를 채울 버퍼 (read) 또는 쓸 데이터 소스 (write).
 * @count: 접근 바이트 수.
 * @offset: PCI config space 내 오프셋.
 * @is_write: true = write (현재 미지원), false = read.
 * @return: 성공 시 count, 에러 시 -1 (errno 설정).
 *
 * 동기/배경: VM 이 PCI config space 에 접근(read/write)하면 libvfio-user 가 본 콜백 호출.
 *   vfio-user 에서 PCI config space 는 SPDK 가 관리하는 256B 영역(endpoint->pci_config_space).
 *   init_pci_config_space 가 초기화 후, Vendor/Device ID, BAR, capability chain 등 VM 이
 *   probe 시 읽는 정보를 포함한다. Write 는 현재 미지원(PC BAR write 등은 별도 처리).
 * 동작: is_write → -EINVAL; count+offset > NVME_REG_CFG_SIZE → -ERANGE;
 *   read → memcpy(buf, pci_config_space+offset, count).
 * 실행 컨텍스트: vfu_run_ctx 콜백 경로 (ctrlr thread).
 * 호출자: libvfio-user (vfu_setup_region 으로 등록된 CFG region 핸들러).
 * 에러 경로: write/범위 초과 → 음수.
 *
 * 호출 체인:
 *   VM PCI config access → libvfio-user → vfu_run_ctx → [access_pci_config]
 */
static ssize_t
access_pci_config(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t offset,
		  bool is_write)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);  /* [한국어] vfu_ctx → endpoint. */

	if (is_write) {
		SPDK_ERRLOG("%s: write %#lx-%#lx not supported\n",
			    endpoint_id(endpoint), offset, offset + count);
		errno = EINVAL;
		return -1;
	}

	if (offset + count > NVME_REG_CFG_SIZE) {
		SPDK_ERRLOG("%s: access past end of extended PCI configuration space, want=%ld+%ld, max=%d\n",
			    endpoint_id(endpoint), offset, count,
			    NVME_REG_CFG_SIZE);
		errno = ERANGE;
		return -1;
	}

	memcpy(buf, ((unsigned char *)endpoint->pci_config_space) + offset, count);

	return count;
}

/*
 * [한국어]
 * vfio_user_log - libvfio-user 의 로그 메시지를 SPDK 로그로 라우팅하는 콜백.
 *
 * @vfu_ctx: libvfio-user context (endpoint 역참조에 사용).
 * @level: syslog 수준(LOG_DEBUG/INFO/NOTICE/WARNING/ERR).
 * @msg: 로그 메시지 문자열.
 *
 * 동기/배경: vfu_setup_log 로 등록. libvfio-user 내부 오류/디버그 메시지를 SPDK 의
 *   통합 로그 프레임워크로 전달해 SPDK 로그 레벨 설정으로 제어할 수 있게 한다.
 * 동작: syslog 수준을 SPDK 로그 함수로 매핑.
 * 실행 컨텍스트: libvfio-user 가 호출하는 스레드 (주로 ctrlr thread).
 * 호출자: libvfio-user 내부 로그 경로.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   libvfio-user internals → [vfio_user_log] → SPDK_DEBUGLOG/INFOLOG/...
 */
static void
vfio_user_log(vfu_ctx_t *vfu_ctx, int level, char const *msg)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);  /* [한국어] context → endpoint. */

	/* [한국어] syslog 수준 내림차순 — level이 높을수록 덜 심각(LOG_DEBUG=7, LOG_ERR=3). */
	if (level >= LOG_DEBUG) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s: %s\n", endpoint_id(endpoint), msg);
	} else if (level >= LOG_INFO) {
		SPDK_INFOLOG(nvmf_vfio, "%s: %s\n", endpoint_id(endpoint), msg);
	} else if (level >= LOG_NOTICE) {
		SPDK_NOTICELOG("%s: %s\n", endpoint_id(endpoint), msg);
	} else if (level >= LOG_WARNING) {
		SPDK_WARNLOG("%s: %s\n", endpoint_id(endpoint), msg);
	} else {
		SPDK_ERRLOG("%s: %s\n", endpoint_id(endpoint), msg);
	}
}

/*
 * [한국어]
 * vfio_user_get_log_level - libvfio-user 에 전달할 현재 로그 수준(syslog 레벨) 반환.
 *
 * @return: LOG_DEBUG~LOG_ERR 중 하나 (syslog 레벨).
 *
 * 동기/배경: vfu_setup_log 의 두 번째 인자로 사용. SPDK 의 현재 로그 레벨을 syslog 레벨로
 *   변환해 libvfio-user 가 그 수준 이상의 메시지만 vfio_user_log 로 전달하게 한다.
 * 동작: nvmf_vfio debug flag 활성 시 LOG_DEBUG, 아니면 SPDK 레벨을 syslog 로 변환.
 * 실행 컨텍스트: vfu_setup_log 호출 시점(초기화).
 * 호출자: vfio_user_dev_info_fill.
 * 에러 경로: spdk_log_to_syslog_level < 0 → LOG_ERR (가장 제한적).
 *
 * 호출 체인:
 *   vfio_user_dev_info_fill → vfu_setup_log(vfio_user_log, [vfio_user_get_log_level])
 */
static int
vfio_user_get_log_level(void)
{
	int level;

	/* [한국어] nvmf_vfio 디버그 플래그 활성 시 libvfio-user 에도 DEBUG 수준 요청. */
	if (SPDK_DEBUGLOG_FLAG_ENABLED("nvmf_vfio")) {
		return LOG_DEBUG;
	}

	/* [한국어] SPDK 로그 레벨을 syslog 레벨로 변환. */
	level = spdk_log_to_syslog_level(spdk_log_get_level());
	if (level < 0) {
		return LOG_ERR;  /* [한국어] 변환 실패 시 가장 제한적인 수준. */
	}

	return level;
}

/*
 * [한국어]
 * init_pci_config_space - PCI config space 의 BAR/인터럽트 기본값 초기화.
 *
 * @p: libvfio-user 가 할당한 PCI config space 구조체 (256B).
 *
 * 동기/배경: vfu_pci_init 이후 SPDK 가 PCI config space 헤더의 일부를 직접 초기화한다.
 *   BAR0/BAR1 (MLBAR/MUBAR = NVMe spec 의 64-bit memory BAR 쌍)을 0 으로 설정하고,
 *   INTx (레거시 인터럽트 핀) 을 INTx#A 로 설정한다.
 *   이 값은 VM PCI probe 시 VM 이 읽는 물리적 register 값에 영향.
 *   Vendor/Device ID 는 vfu_pci_set_id 가 별도로 설정.
 * 동작: bars[0~1]=0 (64-bit BAR), bars[3~5]=0 (vendor-specific BARs), intr.ipin=0x1 (INTA).
 * 실행 컨텍스트: vfio_user_dev_info_fill (endpoint 생성 시 1회).
 * 호출자: nvmf_vfio_user_listen (endpoint setup 경로).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   nvmf_vfio_user_listen → vfio_user_dev_info_fill → vfu_pci_init → [init_pci_config_space]
 */
static void
init_pci_config_space(vfu_pci_config_space_t *p)
{
	/* MLBAR */
	/* [한국어] BAR0(MLBAR) = NVMe 64-bit memory BAR 의 하위 32-bit — 0으로 초기화 (libvfio-user 가 관리). */
	p->hdr.bars[0].raw = 0x0;
	/* MUBAR */
	p->hdr.bars[1].raw = 0x0;

	/* vendor specific, let's set them to zero for now */
	p->hdr.bars[3].raw = 0x0;
	p->hdr.bars[4].raw = 0x0;
	p->hdr.bars[5].raw = 0x0;

	/* enable INTx */
	p->hdr.intr.ipin = 0x1;
}

/* [한국어] struct ctrlr_quiesce_ctx - controller quiesce 진행 상태를 전달하는 컨텍스트.
 *
 * 5-state FSM 의 PAUSING 전이 중 여러 PG(reactor)를 순차적으로 quiesce 하고,
 * 마지막으로 NVMf subsystem 을 pause 하는 과정에서 PG 커서와 상태를 유지한다.
 */
struct ctrlr_quiesce_ctx {
	struct nvmf_vfio_user_endpoint *endpoint;
	/* [한국어] quiesce 를 진행 중인 controller 의 endpoint.
	 * 설정자: ctrlr_quiesce (calloc + 초기화).
	 * 읽는 자: vfio_user_quiesce_pg, vfio_user_quiesce_done 이 endpoint->ctrlr 로 controller 접근.
	 * 값 범위: 유효한 endpoint 포인터. quiesce 완료 전까지 살아 있음.
	 * 동기화: 메시지 체인으로 순차 전달 — 단일 스레드씩 접근. */

	struct nvmf_vfio_user_poll_group *group;
	/* [한국어] 다음에 quiesce 메시지를 보낼 PG 커서.
	 * 설정자: ctrlr_quiesce (TAILQ_FIRST 로 초기화), vfio_user_quiesce_pg (TAILQ_NEXT 로 전진).
	 * 읽는 자: vfio_user_quiesce_pg (NULL 이면 모든 PG 완료 → subsystem pause 진행).
	 * 값 범위: 유효한 PG 포인터 또는 NULL (완료 표시).
	 * 동기화: 메시지 체인으로 순차 — 동시 접근 없음. */

	int status;
	/* [한국어] NVMf subsystem pause 의 결과 상태 (0 = 성공).
	 * 설정자: vfio_user_pause_done (subsystem_pause 콜백 status 인자).
	 * 읽는 자: vfio_user_quiesce_done (vfu_device_quiesced 에 전달).
	 * 값 범위: 0 성공, 음수 에러.
	 * 동기화: 단일 스레드 체인에서만 쓰고 읽음. */
};

static void ctrlr_quiesce(struct nvmf_vfio_user_ctrlr *vu_ctrlr);

/*
 * [한국어]
 * _vfio_user_endpoint_resume_done_msg - NVMf subsystem resume 완료 후 controller thread 에서 실행.
 *
 * @ctx: nvmf_vfio_user_endpoint.
 *
 * 동기/배경: vfio_user_endpoint_resume_done 은 NVMf 코어의 임의 thread 에서 호출될 수 있어
 *   직접 ctrlr 상태를 변경하면 race 가 발생한다. spdk_thread_send_msg 로 controller thread
 *   에 메시지를 보내고 본 함수가 그 thread 에서 안전하게 state=RUNNING 전이를 수행한다.
 * 동작:
 *   - queued_quiesce == false: state=RUNNING, interrupt 모드면 ctrlr_kick 으로 quiesce 중
 *     놓친 SQE 를 처리.
 *   - queued_quiesce == true: resume 완료 직후 또 다른 quiesce 요청이 쌓임 → ctrlr_quiesce.
 * 실행 컨텍스트: controller thread (메시지 핸들러).
 * 호출자: spdk_thread_send_msg (vfio_user_endpoint_resume_done 이 큐잉).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   vfio_user_endpoint_resume_done → spdk_thread_send_msg → [_vfio_user_endpoint_resume_done_msg]
 *     → state=RUNNING 또는 ctrlr_quiesce(재진입)
 */
static void
_vfio_user_endpoint_resume_done_msg(void *ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = ctx;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;

	if (!vu_ctrlr) {
		return;
	}

	if (!vu_ctrlr->queued_quiesce) {
		vu_ctrlr->state = VFIO_USER_CTRLR_RUNNING;

		/*
		 * We might have ignored new SQ entries while we were quiesced:
		 * kick ourselves so we'll definitely check again while in
		 * VFIO_USER_CTRLR_RUNNING state.
		 */
		if (in_interrupt_mode(endpoint->transport)) {
			ctrlr_kick(vu_ctrlr);
		}
		return;
	}


	/*
	 * Basically, once we call `vfu_device_quiesced` the device is
	 * unquiesced from libvfio-user's perspective so from the moment
	 * `vfio_user_quiesce_done` returns libvfio-user might quiesce the device
	 * again. However, because the NVMf subsystem is an asynchronous
	 * operation, this quiesce might come _before_ the NVMf subsystem has
	 * been resumed, so in the callback of `spdk_nvmf_subsystem_resume` we
	 * need to check whether a quiesce was requested.
	 */
	SPDK_DEBUGLOG(nvmf_vfio, "%s has queued quiesce event, quiesce again\n",
		      ctrlr_id(vu_ctrlr));
	ctrlr_quiesce(vu_ctrlr);
}

/*
 * [한국어]
 * vfio_user_endpoint_resume_done - spdk_nvmf_subsystem_resume 완료 콜백.
 *
 * @subsystem: resume 된 NVMf subsystem (미사용).
 * @cb_arg: endpoint 포인터.
 * @status: resume 결과 (0 = 성공).
 *
 * 동기/배경: vfio_user_quiesce_done 이 subsystem_resume 을 요청하면, 완료 시 이 콜백이
 *   호출된다. NVMf 코어 thread 에서 호출될 수 있으므로 controller thread 로 메시지 위임.
 * 동작: spdk_thread_send_msg(ctrlr->thread, _vfio_user_endpoint_resume_done_msg).
 * 실행 컨텍스트: NVMf 코어 thread (임의).
 * 호출자: spdk_nvmf_subsystem_resume 완료 시 콜백.
 * 에러 경로: vu_ctrlr == NULL 이면 early return (ctrlr 이미 destroy 됨).
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_resume → (완료) [vfio_user_endpoint_resume_done]
 *     → spdk_thread_send_msg → _vfio_user_endpoint_resume_done_msg
 */
static void
vfio_user_endpoint_resume_done(struct spdk_nvmf_subsystem *subsystem,
			       void *cb_arg, int status)
{
	struct nvmf_vfio_user_endpoint *endpoint = cb_arg;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "%s resumed done with status %d\n", endpoint_id(endpoint), status);

	if (!vu_ctrlr) {
		return;  /* [한국어] controller 이미 destroy — 할 일 없음. */
	}

	/* [한국어] controller thread 로 상태 전이 위임(다른 thread 에서 ctrlr 상태 변경 방지). */
	spdk_thread_send_msg(vu_ctrlr->thread, _vfio_user_endpoint_resume_done_msg, endpoint);
}

/*
 * [한국어]
 * vfio_user_quiesce_done - NVMf subsystem pause 완료 후 quiesced 상태 알림 + resume 시작.
 *
 * @ctx: ctrlr_quiesce_ctx (status 포함).
 *
 * 동기/배경: vfio_user_pause_done 이 모든 PG quiesce + subsystem pause 완료를 확인하고
 *   controller thread 로 메시지를 보내면 본 함수가 실행된다. 여기서:
 *   1. state=PAUSED 전이.
 *   2. vfu_device_quiesced — libvfio-user 에 "device 가 안전하게 멈췄음" 통보
 *      (이 시점부터 live migration, memory remap 등이 안전).
 *   3. queued_quiesce 클리어.
 *   4. state=RESUMING + spdk_nvmf_subsystem_resume 요청.
 * 주의: vfu_device_quiesced 반환 직후 libvfio-user 가 또 quiesce 를 요청할 수 있어
 *   vfio_user_endpoint_resume_done 에서 queued_quiesce 재확인 필요.
 * 실행 컨텍스트: controller thread (spdk_thread_send_msg 로 전달됨).
 * 호출자: spdk_thread_send_msg (vfio_user_pause_done 이 큐잉).
 * 에러 경로: subsystem_resume 실패 → state=PAUSED 복원.
 *
 * 호출 체인:
 *   vfio_user_pause_done → spdk_thread_send_msg → [vfio_user_quiesce_done]
 *     → vfu_device_quiesced → spdk_nvmf_subsystem_resume → vfio_user_endpoint_resume_done
 */
static void
vfio_user_quiesce_done(void *ctx)
{
	struct ctrlr_quiesce_ctx *quiesce_ctx = ctx;
	struct nvmf_vfio_user_endpoint *endpoint = quiesce_ctx->endpoint;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;
	int ret;

	if (!vu_ctrlr) {
		free(quiesce_ctx);
		return;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s device quiesced\n", ctrlr_id(vu_ctrlr));

	assert(vu_ctrlr->state == VFIO_USER_CTRLR_PAUSING);
	vu_ctrlr->state = VFIO_USER_CTRLR_PAUSED;
	vfu_device_quiesced(endpoint->vfu_ctx, quiesce_ctx->status);
	vu_ctrlr->queued_quiesce = false;
	free(quiesce_ctx);

	SPDK_DEBUGLOG(nvmf_vfio, "%s start to resume\n", ctrlr_id(vu_ctrlr));
	vu_ctrlr->state = VFIO_USER_CTRLR_RESUMING;
	ret = spdk_nvmf_subsystem_resume((struct spdk_nvmf_subsystem *)endpoint->subsystem,
					 vfio_user_endpoint_resume_done, endpoint);
	if (ret < 0) {
		vu_ctrlr->state = VFIO_USER_CTRLR_PAUSED;
		SPDK_ERRLOG("%s: failed to resume, ret=%d\n", endpoint_id(endpoint), ret);
	}
}

/*
 * [한국어]
 * vfio_user_pause_done - spdk_nvmf_subsystem_pause 완료 콜백.
 *
 * @subsystem: pause 된 NVMf subsystem (미사용).
 * @ctx: ctrlr_quiesce_ctx.
 * @status: pause 결과 (0 = 성공).
 *
 * 동기/배경: vfio_user_quiesce_pg 가 모든 PG 를 순회한 뒤 마지막에
 *   spdk_nvmf_subsystem_pause(GLOBAL_NS_TAG) 를 호출하면, 완료 시 이 콜백이 호출된다.
 *   subsystem 코어 thread 에서 호출되므로 controller thread 로 위임.
 * 동작: status → quiesce_ctx->status 저장 → controller thread 로 vfio_user_quiesce_done 전달.
 * 실행 컨텍스트: NVMf 코어 thread (임의, non-controller thread 가능).
 * 호출자: spdk_nvmf_subsystem_pause 완료 콜백.
 * 에러 경로: vu_ctrlr == NULL (controller destroy됨) → quiesce_ctx 해제 후 early return.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_pause → (완료) [vfio_user_pause_done]
 *     → spdk_thread_send_msg → vfio_user_quiesce_done
 */
static void
vfio_user_pause_done(struct spdk_nvmf_subsystem *subsystem,
		     void *ctx, int status)
{
	struct ctrlr_quiesce_ctx *quiesce_ctx = ctx;    /* [한국어] quiesce 상태 컨텍스트. */
	struct nvmf_vfio_user_endpoint *endpoint = quiesce_ctx->endpoint;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;

	if (!vu_ctrlr) {                                /* [한국어] controller 이미 destroy — 컨텍스트만 해제. */
		free(quiesce_ctx);
		return;
	}

	quiesce_ctx->status = status;                   /* [한국어] pause 결과를 vfio_user_quiesce_done 으로 전달. */

	SPDK_DEBUGLOG(nvmf_vfio, "%s pause done with status %d\n",
		      ctrlr_id(vu_ctrlr), status);

	/* [한국어] controller thread 에서 quiesce_done 을 실행 — state 변경은 controller thread 에서만. */
	spdk_thread_send_msg(vu_ctrlr->thread,
			     vfio_user_quiesce_done, ctx);
}

/*
 * Ensure that, for this PG, we've stopped running in nvmf_vfio_user_sq_poll();
 * we've already set ctrlr->state, so we won't process new entries, but we need
 * to ensure that this PG is quiesced. This only works because there's no
 * callback context set up between polling the SQ and spdk_nvmf_request_exec().
 *
 * Once we've walked all PGs, we need to pause any submitted I/O via
 * spdk_nvmf_subsystem_pause(SPDK_NVME_GLOBAL_NS_TAG).
 *
 * [한국어]
 * vfio_user_quiesce_pg - quiesce 체인에서 각 PG 를 순차적으로 quiesce 하는 핸들러.
 *
 * @ctx: ctrlr_quiesce_ctx (quiesce_ctx->group 커서가 현재 처리할 PG).
 *
 * 동기/배경: ctrlr_quiesce 는 transport->poll_groups 리스트의 첫 PG 에 이 함수를
 *   send_msg 한다. 각 PG 는 자신의 thread 에서 본 함수를 실행해 "더 이상 sq_poll 를
 *   처리하지 않음"을 보장한 뒤 TAILQ_NEXT 로 다음 PG 에 릴레이한다.
 *   (ctrlr->state == VFIO_USER_CTRLR_PAUSING 이므로 sq_poll 의 새 처리는 이미 차단됨.)
 *   모든 PG 를 순회하면 subsystem_pause(GLOBAL_NS_TAG) 로 진행 중인 NVMf I/O 를 멈춘다.
 * 동작:
 *   1. quiesce_ctx->group = TAILQ_NEXT (커서 전진).
 *   2. 다음 PG 있으면 해당 PG thread 로 재귀 send_msg.
 *   3. 모든 PG 완료 → spdk_nvmf_subsystem_pause 요청 (vfio_user_pause_done 콜백).
 * 실행 컨텍스트: 각 PG 의 thread (각 PG 마다 한 번씩, 순차).
 * 호출자: ctrlr_quiesce(첫 PG), 이전 vfio_user_quiesce_pg(다음 PG).
 * 에러 경로: vu_ctrlr == NULL → quiesce_ctx free + return;
 *   subsystem_pause 실패 → state=RUNNING + fail_ctrlr + quiesce_ctx free.
 *
 * 호출 체인:
 *   ctrlr_quiesce → spdk_thread_send_msg → [vfio_user_quiesce_pg]
 *     → (재귀) spdk_thread_send_msg → [vfio_user_quiesce_pg] ...
 *     → spdk_nvmf_subsystem_pause → vfio_user_pause_done
 */
static void
vfio_user_quiesce_pg(void *ctx)
{
	struct ctrlr_quiesce_ctx *quiesce_ctx = ctx;
	struct nvmf_vfio_user_endpoint *endpoint = quiesce_ctx->endpoint;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;
	struct nvmf_vfio_user_poll_group *vu_group = quiesce_ctx->group; /* [한국어] 현재 quiesce 대상 PG. */
	struct spdk_nvmf_subsystem *subsystem = endpoint->subsystem;
	int ret;

	SPDK_DEBUGLOG(nvmf_vfio, "quiesced pg:%p\n", vu_group);

	if (!vu_ctrlr) {                                /* [한국어] controller 이미 사라짐 — quiesce 불필요. */
		free(quiesce_ctx);
		return;
	}

	/* [한국어] 커서를 다음 PG 로 전진. NULL 이면 모든 PG 완료를 의미. */
	quiesce_ctx->group = TAILQ_NEXT(vu_group, link);
	if (quiesce_ctx->group != NULL)  {
		/* [한국어] 다음 PG thread 로 quiesce 릴레이 (각 PG 는 자신의 thread 에서 보장). */
		spdk_thread_send_msg(poll_group_to_thread(quiesce_ctx->group),
				     vfio_user_quiesce_pg, quiesce_ctx);
		return;
	}

	/* [한국어] 모든 PG 완료 → subsystem 전체 I/O pause (진행 중인 bdev I/O 완료 대기). */
	ret = spdk_nvmf_subsystem_pause(subsystem, SPDK_NVME_GLOBAL_NS_TAG,
					vfio_user_pause_done, quiesce_ctx);
	if (ret < 0) {
		SPDK_ERRLOG("%s: failed to pause, ret=%d\n",
			    endpoint_id(endpoint), ret);
		vu_ctrlr->state = VFIO_USER_CTRLR_RUNNING;  /* [한국어] quiesce 실패 → RUNNING 복원. */
		fail_ctrlr(vu_ctrlr);                        /* [한국어] controller 를 fatal error 상태로 전환. */
		free(quiesce_ctx);
	}
}

/*
 * [한국어]
 * ctrlr_quiesce - controller quiesce 시퀀스 진입점. RUNNING → PAUSING 전이 후
 *   PG 체인 순회를 시작한다.
 *
 * @vu_ctrlr: quiesce 할 controller.
 *
 * 동기/배경: vfio_user_dev_quiesce_cb 가 libvfio-user 의 요청(live migration 등)에 응답해
 *   이 함수를 호출한다. 또는 _vfio_user_endpoint_resume_done_msg 가 queued_quiesce 를
 *   발견했을 때도 호출된다. RUNNING 상태에서만 유효하다.
 * 동작:
 *   1. state = PAUSING.
 *   2. ctrlr_quiesce_ctx 할당 (endpoint, group 커서 = TAILQ_FIRST).
 *   3. 첫 PG thread 로 vfio_user_quiesce_pg send_msg.
 * 이후 체인: vfio_user_quiesce_pg → … → vfio_user_pause_done → vfio_user_quiesce_done
 *   → vfu_device_quiesced → subsystem_resume → vfio_user_endpoint_resume_done.
 * 실행 컨텍스트: controller thread.
 * 호출자: vfio_user_dev_quiesce_cb, _vfio_user_endpoint_resume_done_msg.
 * 에러 경로: calloc 실패 → assert(false) — OOM 은 치명적 버그로 처리.
 *
 * 호출 체인:
 *   vfio_user_dev_quiesce_cb → [ctrlr_quiesce]
 *     → spdk_thread_send_msg → vfio_user_quiesce_pg → …
 */
static void
ctrlr_quiesce(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	struct ctrlr_quiesce_ctx *quiesce_ctx;

	vu_ctrlr->state = VFIO_USER_CTRLR_PAUSING;         /* [한국어] RUNNING → PAUSING 전이. */

	quiesce_ctx = calloc(1, sizeof(*quiesce_ctx));      /* [한국어] quiesce 체인 전달용 컨텍스트 할당. */
	if (!quiesce_ctx) {
		SPDK_ERRLOG("Failed to allocate subsystem pause context\n");
		assert(false);  /* [한국어] OOM 은 치명적 버그로 처리 — 복구 경로 없음. */
		return;
	}

	quiesce_ctx->endpoint = vu_ctrlr->endpoint;         /* [한국어] endpoint 는 quiesce 내내 유효. */
	quiesce_ctx->status = 0;                            /* [한국어] 초기 상태(성공). pause_done 에서 갱신. */
	/* [한국어] PG 체인 커서를 첫 PG 로 초기화. poll_groups 리스트는 transport lifetime 동안 고정. */
	quiesce_ctx->group = TAILQ_FIRST(&vu_ctrlr->transport->poll_groups);

	/* [한국어] 첫 PG 의 thread 에 quiesce 체인 시작 메시지 전송. */
	spdk_thread_send_msg(poll_group_to_thread(quiesce_ctx->group),
			     vfio_user_quiesce_pg, quiesce_ctx);
}

/*
 * [한국어]
 * vfio_user_dev_quiesce_cb - libvfio-user 가 device quiesce 를 요청할 때 호출되는 콜백.
 *
 * @vfu_ctx: libvfio-user context.
 * @return: 0 = 이미 quiesced (동기 완료), -1 + errno=EBUSY = 비동기 quiesce 진행 중.
 *
 * 동기/배경: libvfio-user 는 live migration, dirty page tracking, memory remapping 등의
 *   작업 전에 device 가 안전하게 멈춰야 함을 보장하기 위해 이 콜백을 호출한다.
 *   콜백이 -1/EBUSY 를 반환하면 libvfio-user 는 기다리다가 vfu_device_quiesced 호출을
 *   기다린다. 0 을 반환하면 즉시 quiesced 로 간주한다.
 * 동작:
 *   1. controller 없음 / NVMf ctrlr 없음 → 0 (이미 quiesced).
 *   2. CC.EN=0 또는 CSTS.RDY=0 또는 SHST=COMPLETE → 0 (장치가 이미 조용함).
 *   3. state=PAUSED → 0.
 *   4. state=RUNNING → ctrlr_quiesce 시작 → -EBUSY.
 *   5. state=RESUMING → queued_quiesce=true (resume 완료 후 재시도) → -EBUSY.
 * 실행 컨텍스트: controller thread (vfu_ctx poller 와 동일 thread — 경쟁 없음).
 * 호출자: libvfio-user (vfu_run_ctx 처리 중).
 * 에러 경로: PAUSING 상태에서는 assert(false) — 이미 진행 중이면 재진입 불가.
 *
 * 호출 체인:
 *   libvfio-user (vfu_run_ctx) → [vfio_user_dev_quiesce_cb]
 *     → ctrlr_quiesce → vfio_user_quiesce_pg → … → vfu_device_quiesced
 */
static int
vfio_user_dev_quiesce_cb(vfu_ctx_t *vfu_ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct spdk_nvmf_subsystem *subsystem = endpoint->subsystem;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;

	if (!vu_ctrlr) {
		return 0;  /* [한국어] controller 없음 — 이미 quiesced 상태. */
	}

	/* NVMf library will destruct controller when no
	 * connected queue pairs.
	 * [한국어] NVMf subsystem 에 controller 가 등록돼 있지 않으면 quiesce 불필요.
	 */
	if (!nvmf_subsystem_get_ctrlr(subsystem, vu_ctrlr->cntlid)) {
		return 0;  /* [한국어] controller 이미 소멸 — quiesced 간주. */
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s starts to quiesce\n", ctrlr_id(vu_ctrlr));

	/* There is no race condition here as device quiesce callback
	 * and nvmf_prop_set_cc() are running in the same thread context.
	 * [한국어] 이 콜백과 nvmf_prop_set_cc() 는 같은 thread 에서 실행 — 경쟁 없음.
	 */
	if (!vu_ctrlr->ctrlr->vcprop.cc.bits.en) {
		return 0;  /* [한국어] CC.EN=0 — controller 비활성 상태, quiesce 불필요. */
	} else if (!vu_ctrlr->ctrlr->vcprop.csts.bits.rdy) {
		return 0;  /* [한국어] CSTS.RDY=0 — controller 아직 준비 안 됨. */
	} else if (vu_ctrlr->ctrlr->vcprop.csts.bits.shst == SPDK_NVME_SHST_COMPLETE) {
		return 0;  /* [한국어] shutdown 완료 — 이미 quiesced. */
	}

	switch (vu_ctrlr->state) {
	case VFIO_USER_CTRLR_PAUSED:
		return 0;  /* [한국어] 이미 PAUSED — 즉시 완료. */
	case VFIO_USER_CTRLR_RUNNING:
		/* [한국어] RUNNING → quiesce 시퀀스 시작 (비동기, -EBUSY 반환). */
		ctrlr_quiesce(vu_ctrlr);
		break;
	case VFIO_USER_CTRLR_RESUMING:
		/* [한국어] RESUMING 중 quiesce 요청 — resume 완료 후 재진입하도록 플래그 설정. */
		vu_ctrlr->queued_quiesce = true;
		SPDK_DEBUGLOG(nvmf_vfio, "%s is busy to quiesce, current state %u\n", ctrlr_id(vu_ctrlr),
			      vu_ctrlr->state);
		break;
	default:
		/* [한국어] PAUSING 상태에서 재진입은 버그 — assert 로 감지. */
		assert(vu_ctrlr->state != VFIO_USER_CTRLR_PAUSING);
		break;
	}

	/* [한국어] EBUSY: 비동기 quiesce 진행 중 — libvfio-user 가 vfu_device_quiesced 를 기다림. */
	errno = EBUSY;
	return -1;
}

/*
 * If we are about to close the connection, we need to unregister the interrupt,
 * as the library will subsequently close the file descriptor we registered.
 */
static int
vfio_user_device_reset(vfu_ctx_t *vfu_ctx, vfu_reset_type_t type)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr = endpoint->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "Device reset type %u\n", type);

	if (type == VFU_RESET_LOST_CONN) {
		if (ctrlr != NULL) {
			spdk_interrupt_unregister(&ctrlr->intr);
			ctrlr->intr_fd = -1;
		}
		return 0;
	}

	/* FIXME: LOST_CONN case ? */
	if (ctrlr->sdbl != NULL) {
		vfio_user_ctrlr_switch_doorbells(ctrlr, false);
		free_sdbl(vfu_ctx, ctrlr->sdbl);
		ctrlr->sdbl = NULL;
	}

	/* FIXME: much more needed here. */

	return 0;
}

static int
vfio_user_dev_info_fill(struct nvmf_vfio_user_transport *vu_transport,
			struct nvmf_vfio_user_endpoint *endpoint)
{
	int ret;
	ssize_t cap_offset;
	vfu_ctx_t *vfu_ctx = endpoint->vfu_ctx;

	struct pmcap pmcap = { .hdr.id = PCI_CAP_ID_PM, .pmcs.nsfrst = 0x1 };
	struct pxcap pxcap = {
		.hdr.id = PCI_CAP_ID_EXP,
		.pxcaps.ver = 0x2,
		.pxdcap = {.rer = 0x1, .flrc = 0x1},
		.pxdcap2.ctds = 0x1
	};

	struct msixcap msixcap = {
		.hdr.id = PCI_CAP_ID_MSIX,
		.mxc.ts = NVMF_VFIO_USER_MSIX_NUM - 1,
		.mtab = {.tbir = NVMF_VFIO_USER_MSIX_TABLE_BIR, .to = 0x0},
		.mpba = {.pbir = NVMF_VFIO_USER_MSIX_PBA_BIR, .pbao = 0x0}
	};

	struct iovec sparse_mmap[] = {
		{
			.iov_base = (void *)NVME_DOORBELLS_OFFSET,
			.iov_len = NVMF_VFIO_USER_DOORBELLS_SIZE,
		},
	};

	ret = vfu_pci_init(vfu_ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to initialize PCI\n", vfu_ctx);
		return ret;
	}
	vfu_pci_set_id(vfu_ctx, SPDK_PCI_VID_NUTANIX, 0x0001, SPDK_PCI_VID_NUTANIX, 0);
	/*
	 * 0x02, controller uses the NVM Express programming interface
	 * 0x08, non-volatile memory controller
	 * 0x01, mass storage controller
	 */
	vfu_pci_set_class(vfu_ctx, 0x01, 0x08, 0x02);

	cap_offset = vfu_pci_add_capability(vfu_ctx, 0, 0, &pmcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add pmcap\n", vfu_ctx);
		return ret;
	}

	cap_offset = vfu_pci_add_capability(vfu_ctx, 0, 0, &pxcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add pxcap\n", vfu_ctx);
		return ret;
	}

	cap_offset = vfu_pci_add_capability(vfu_ctx, 0, 0, &msixcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add msixcap\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_CFG_REGION_IDX, NVME_REG_CFG_SIZE,
			       access_pci_config, VFU_REGION_FLAG_RW, NULL, 0, -1, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup cfg\n", vfu_ctx);
		return ret;
	}

	if (vu_transport->transport_opts.disable_mappable_bar0) {
		ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX, NVME_REG_BAR0_SIZE,
				       access_bar0_fn, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM,
				       NULL, 0, -1, 0);
	} else {
		ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX, NVME_REG_BAR0_SIZE,
				       access_bar0_fn, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM,
				       sparse_mmap, 1, endpoint->devmem_fd, 0);
	}

	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup bar 0\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR4_REGION_IDX, NVMF_VFIO_USER_BAR4_SIZE,
			       NULL, VFU_REGION_FLAG_RW, NULL, 0, -1, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup bar 4\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR5_REGION_IDX, NVMF_VFIO_USER_BAR5_SIZE,
			       NULL, VFU_REGION_FLAG_RW, NULL, 0, -1, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup bar 5\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_device_dma(vfu_ctx, memory_region_add_cb, memory_region_remove_cb);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup dma callback\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_device_reset_cb(vfu_ctx, vfio_user_device_reset);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup reset callback\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_INTX_IRQ, 1);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup INTX\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_MSIX_IRQ, NVMF_VFIO_USER_MSIX_NUM);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup MSIX\n", vfu_ctx);
		return ret;
	}

	vfu_setup_device_quiesce_cb(vfu_ctx, vfio_user_dev_quiesce_cb);

	ret = vfu_realize_ctx(vfu_ctx);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to realize\n", vfu_ctx);
		return ret;
	}

	endpoint->pci_config_space = vfu_pci_get_config_space(endpoint->vfu_ctx);
	assert(endpoint->pci_config_space != NULL);
	init_pci_config_space(endpoint->pci_config_space);

	assert(cap_offset != 0);
	endpoint->msix = (struct msixcap *)((uint8_t *)endpoint->pci_config_space + cap_offset);

	return 0;
}

static int nvmf_vfio_user_accept(void *ctx);

/*
 * Register an "accept" poller: this is polling for incoming vfio-user socket
 * connections (on the listening socket).
 *
 * We need to do this on first listening, and also after destroying a
 * controller, so we can accept another connection.
 */
static int
vfio_user_register_accept_poller(struct nvmf_vfio_user_endpoint *endpoint)
{
	uint64_t poll_rate_us = endpoint->transport->transport.opts.acceptor_poll_rate;

	SPDK_DEBUGLOG(nvmf_vfio, "registering accept poller\n");

	endpoint->accept_poller = SPDK_POLLER_REGISTER(nvmf_vfio_user_accept,
				  endpoint, poll_rate_us);

	if (!endpoint->accept_poller) {
		return -1;
	}

	endpoint->accept_thread = spdk_get_thread();
	endpoint->need_relisten = false;

	if (!spdk_interrupt_mode_is_enabled()) {
		return 0;
	}

	endpoint->accept_intr_fd = vfu_get_poll_fd(endpoint->vfu_ctx);
	assert(endpoint->accept_intr_fd != -1);

	endpoint->accept_intr = SPDK_INTERRUPT_REGISTER(endpoint->accept_intr_fd,
				nvmf_vfio_user_accept, endpoint);

	assert(endpoint->accept_intr != NULL);

	spdk_poller_register_interrupt(endpoint->accept_poller, NULL, NULL);
	return 0;
}

static void
_vfio_user_relisten(void *ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = ctx;

	vfio_user_register_accept_poller(endpoint);
}

/*
 * [한국어]
 * _free_ctrlr - controller 자원 최종 해제 (controller thread 위에서 실행).
 *
 * @ctx: ctrlr.
 *
 * free_ctrlr 가 spdk_thread_exec_msg 로 controller thread 에 dispatch — 이유:
 * vfu_ctx_poller 와 intr 가 controller thread 에 등록돼 있어 같은 thread 에서만 unregister 가능.
 *
 * 단계:
 *   1. shadow doorbell 자원 해제 (free_sdbl — sdbl=NULL 도 안전).
 *   2. intr / vfu_ctx_poller unregister.
 *   3. ctrlr free.
 *   4. **endpoint relisten 결정**:
 *      - need_async_destroy: endpoint 도 함께 destroy (RPC 로 endpoint 자체 삭제 요청한 경우).
 *      - need_relisten: accept_poller 재등록 → 새 VM connect 대기 (VM 만 재기동한 경우).
 *      - 둘 다 false: endpoint 살아있되 next connection 안 받음 (?? — 보통은 둘 중 하나).
 *
 * relisten 은 accept_thread 에 send_msg — accept_poller 가 그 thread 소속이므로 그곳에서 등록.
 */
static void
_free_ctrlr(void *ctx)
{
	struct nvmf_vfio_user_ctrlr *ctrlr = ctx;
	struct nvmf_vfio_user_endpoint *endpoint = ctrlr->endpoint;

	free_sdbl(endpoint->vfu_ctx, ctrlr->sdbl);

	spdk_interrupt_unregister(&ctrlr->intr);
	ctrlr->intr_fd = -1;
	spdk_poller_unregister(&ctrlr->vfu_ctx_poller);

	free(ctrlr);

	/* [한국어] endpoint 정리 분기 — async_destroy 우선 (RPC delete 요청), relisten (VM 재기동 대비). */
	if (endpoint->need_async_destroy) {
		nvmf_vfio_user_destroy_endpoint(endpoint);
	} else if (endpoint->need_relisten) {
		spdk_thread_send_msg(endpoint->accept_thread,
				     _vfio_user_relisten, endpoint);
	}
}

/*
 * [한국어]
 * free_ctrlr - controller destruction 진입점. 모든 SQ/CQ free 후 thread dispatch.
 *
 * @ctrlr: controller.
 *
 * VM 종료 / disconnect / RPC delete 시 호출. 모든 qpair 해제 → controller thread 로
 * dispatch 해서 _free_ctrlr 실행 (다른 thread 에서 호출됐을 수 있음).
 *
 * thread fallback: ctrlr->thread 미설정 (controller 생성 도중 실패) 시 caller thread 사용.
 * spdk_thread_exec_msg: 같은 thread 면 inline 실행, 다른 thread 면 message queue.
 */
static void
free_ctrlr(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct spdk_thread *thread;
	int i;

	assert(ctrlr != NULL);
	/* [한국어] thread 미설정 fallback — controller construction 실패 시 caller thread 사용. */
	thread = ctrlr->thread ? ctrlr->thread : spdk_get_thread();

	SPDK_DEBUGLOG(nvmf_vfio, "free %s\n", ctrlr_id(ctrlr));

	/* [한국어] 모든 qpair slot (max 512) 순회 해제 — 사용 안 한 slot 은 free_qp 가 no-op. */
	for (i = 0; i < NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR; i++) {
		free_qp(ctrlr, i);
	}

	/* [한국어] controller thread 로 dispatch — vfu_ctx_poller/intr unregister 안전성. */
	spdk_thread_exec_msg(thread, _free_ctrlr, ctrlr);
}

/*
 * [한국어]
 * nvmf_vfio_user_create_ctrlr - VM connect 직후 controller emulation 컨텍스트 생성.
 *
 * @transport: vfio-user transport.
 * @endpoint: VM 이 연결된 endpoint.
 * @return: 0 성공, 음수 errno.
 *
 * nvmf_vfio_user_accept 가 vfu_attach_ctx 성공 후 호출. RDMA 의 nvmf_rdma_connect 와
 * 비슷하지만 vfio-user 는 capsule 없이 PCI probe 로 connect — 본 함수가 NVMe register
 * 상태 + admin queue 자원 일괄 셋업.
 *
 * 단계:
 *   1. ctrlr 객체 calloc.
 *   2. cntlid 생성 (subsystem 단위 unique — RDMA/TCP 와 같은 target 에 vfio-user 가 같이
 *      있을 때 cntlid 충돌 회피).
 *   3. intr_fd = -1 sentinel, adaptive_irqs (transport opt 기반).
 *   4. ★ admin SQ/CQ (qid=0) 생성 + AQ depth = 32, free_reqs 풀 calloc.
 *      VM 의 NVMe driver 가 부팅 시 CC.EN write 전까진 admin queue 없이 register access 만 하므로
 *      admin queue 만 미리 준비.
 *   5. endpoint->ctrlr 연결 + spdk_nvmf_tgt_new_qpair 로 상위 NVMe-oF 레이어에 알림 →
 *      poll_group_add → 이후 admin command 처리 가능.
 *
 * 호출 체인:
 *   nvmf_vfio_user_accept → [본 함수]
 *     → init_sq/cq (admin) → alloc_sq_reqs → endpoint->ctrlr=ctrlr
 *     → spdk_nvmf_tgt_new_qpair → poll_group_add → vfu_ctx_poller 등록
 */
static int
nvmf_vfio_user_create_ctrlr(struct nvmf_vfio_user_transport *transport,
			    struct nvmf_vfio_user_endpoint *endpoint)
{
	struct nvmf_vfio_user_ctrlr *ctrlr;
	int err = 0;

	SPDK_DEBUGLOG(nvmf_vfio, "%s\n", endpoint_id(endpoint));

	/* First, construct a vfio-user CUSTOM transport controller */
	ctrlr = calloc(1, sizeof(*ctrlr));
	if (ctrlr == NULL) {
		err = -ENOMEM;
		goto out;
	}
	/*
	 * We can only support one connection for now, but generate a unique cntlid in case vfio-user
	 * transport is used together with RDMA or TCP transports in the same target
	 */
	/* [한국어] cntlid 생성 — subsystem 안에서 unique. 같은 target 에 RDMA/TCP/vfio-user 가
	 * 공존할 때 cntlid 가 겹치지 않게 함. NVMe spec: 0 reserved, 1..N controllers. */
	ctrlr->cntlid = nvmf_subsystem_gen_cntlid(endpoint->subsystem);
	ctrlr->intr_fd = -1;
	ctrlr->transport = transport;
	ctrlr->endpoint = endpoint;
	/* [한국어] BAR0 doorbell 영역 포인터 endpoint 에서 상속 — VM 이 BAR0 에 mmap 한 같은 페이지. */
	ctrlr->bar0_doorbells = endpoint->bar0_doorbells;
	TAILQ_INIT(&ctrlr->connected_sqs);

	/* [한국어] adaptive_irqs 옵션 — disable_adaptive_irq 면 IO CQ 도 매 CQE IRQ trigger. */
	ctrlr->adaptive_irqs_enabled =
		!transport->transport_opts.disable_adaptive_irq;

	/* Then, construct an admin queue pair */
	/* [한국어] admin SQ (qid=0) — VM 이 CC.EN write 전엔 inactive 지만 객체는 미리 준비. */
	err = init_sq(ctrlr, &transport->transport, 0);
	if (err != 0) {
		free(ctrlr);
		goto out;
	}

	err = init_cq(ctrlr, 0);
	if (err != 0) {
		free(ctrlr);
		goto out;
	}

	/* [한국어] AQ depth = 32 (NVMF_VFIO_USER_DEFAULT_AQ_DEPTH). NVMe spec 의 admin queue
	 * 최대 4096 보다 작지만 가상 device 라 충분. */
	ctrlr->sqs[0]->size = NVMF_VFIO_USER_DEFAULT_AQ_DEPTH;

	err = alloc_sq_reqs(ctrlr, ctrlr->sqs[0]);
	if (err != 0) {
		free(ctrlr);
		goto out;
	}
	endpoint->ctrlr = ctrlr;

	/* Notify the generic layer about the new admin queue pair */
	/* [한국어] 상위 NVMe-oF 레이어에 새 qpair 등록 → poll_group_add → poll_group_poll 활성화. */
	spdk_nvmf_tgt_new_qpair(transport->transport.tgt, &ctrlr->sqs[0]->qpair);

out:
	if (err != 0) {
		SPDK_ERRLOG("%s: failed to create vfio-user controller: %s\n",
			    endpoint_id(endpoint), strerror(-err));
	}

	return err;
}

/*
 * [한국어]
 * nvmf_vfio_user_listen - ★ vfio-user endpoint 생성 + libvfio-user context 셋업.
 *
 * @transport: vfio-user transport.
 * @trid: transport id (traddr = "/var/tmp/cntrl" 형식 socket path).
 * @listen_opts: NVMe-oF listen 옵션 (현재 vfio-user 는 미사용).
 * @return: 0 성공, 음수 errno.
 *
 * RPC nvmf_subsystem_add_listener (transport=VFIOUSER) 가 트리거. RDMA/TCP 의 "TCP/IP
 * port 바인딩" 과 달리 vfio-user 는 socket path 기반으로 endpoint 1개 = 1 VM 대응.
 *
 * 단계:
 *   1. **traddr 중복 검사**: 같은 path 로 두 번 listen 금지 (EEXIST).
 *   2. **endpoint 객체 calloc** + 기본 필드 셋업 (devmem_fd=-1, lock).
 *   3. **BAR0 backing file 생성**: <endpoint_id>/bar0 — open + unlink (anonymous file).
 *      ftruncate 로 BAR0 크기 (= 0x1000 + doorbells size) 확보.
 *   4. **BAR0 mmap**: bar0 = MAP_SHARED — VM 이 mmap 한 영역과 같은 물리 페이지.
 *      bar0_doorbells = bar0 + 0x1000 (doorbell 영역 시작점).
 *   5. **libvfio-user context 생성**: vfu_create_ctx(SOCK, uuid, ATTACH_NB) — Unix socket
 *      type + ATTACH_NB (non-blocking attach for accept).
 *   6. **vfu_setup_log**: SPDK log 시스템 redirect.
 *   7. **vfio_user_dev_info_fill**: PCI config space (vendor/device ID, BAR sizes, MSI-X cap)
 *      + region 등록 (BAR0/4/5, config space, migration region).
 *   8. **accept_poller 등록**: VM connect 대기.
 *   9. **endpoints TAILQ 등록** (transport lock 보유).
 *
 * BAR0 backing file 트릭: open + unlink — file system 에 이름 안 남지만 fd 살아있는 동안
 * 메모리 확보. mmap MAP_SHARED 로 같은 영역을 endpoint 와 VM 이 공유. mmap 의 offset 은
 * 0 (kernel page size > 4KB 환경 호환), doorbell offset 은 수동으로 더함.
 *
 * 호출 체인:
 *   RPC nvmf_subsystem_add_listener → transport vtable.listen = [본 함수]
 *     → BAR0 file + mmap + vfu_create_ctx + accept_poller + endpoints 등록
 */
static int
nvmf_vfio_user_listen(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid,
		      struct spdk_nvmf_listen_opts *listen_opts)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_endpoint *endpoint, *tmp;
	char path[PATH_MAX] = {};
	char uuid[PATH_MAX] = {};
	int ret;

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);

	pthread_mutex_lock(&vu_transport->lock);
	TAILQ_FOREACH_SAFE(endpoint, &vu_transport->endpoints, link, tmp) {
		/* Only compare traddr */
		/* [한국어] traddr (socket path) 중복 검사 — 같은 path 두 번 listen 금지. */
		if (strncmp(endpoint->trid.traddr, trid->traddr, sizeof(endpoint->trid.traddr)) == 0) {
			pthread_mutex_unlock(&vu_transport->lock);
			return -EEXIST;
		}
	}
	pthread_mutex_unlock(&vu_transport->lock);

	endpoint = calloc(1, sizeof(*endpoint));
	if (!endpoint) {
		return -ENOMEM;
	}

	pthread_mutex_init(&endpoint->lock, NULL);
	endpoint->devmem_fd = -1;
	memcpy(&endpoint->trid, trid, sizeof(endpoint->trid));
	endpoint->transport = vu_transport;

	ret = snprintf(path, PATH_MAX, "%s/bar0", endpoint_id(endpoint));
	if (ret < 0 || ret >= PATH_MAX) {
		SPDK_ERRLOG("%s: error to get socket path: %s.\n", endpoint_id(endpoint), spdk_strerror(errno));
		ret = -1;
		goto out;
	}

	ret = open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (ret == -1) {
		SPDK_ERRLOG("%s: failed to open device memory at %s: %s.\n",
			    endpoint_id(endpoint), path, spdk_strerror(errno));
		goto out;
	}
	unlink(path);

	endpoint->devmem_fd = ret;
	ret = ftruncate(endpoint->devmem_fd,
			NVME_DOORBELLS_OFFSET + NVMF_VFIO_USER_DOORBELLS_SIZE);
	if (ret != 0) {
		SPDK_ERRLOG("%s: error to ftruncate file %s: %s.\n", endpoint_id(endpoint), path,
			    spdk_strerror(errno));
		goto out;
	}

	/*
	 * The doorbell offset is fixed at 0x1000.
	 * But mmap requires the offset must be a multiple of the page size as returned by
	 * sysconf(_SC_PAGE_SIZE).
	 * In order to avoid the mmap failure in non-4K page size kernel,
	 * set 0 to mmap's offset, and then change to doorbell offset manually.
	 */
	endpoint->bar0 = mmap(NULL, NVME_REG_BAR0_SIZE, PROT_READ | PROT_WRITE,
			      MAP_SHARED, endpoint->devmem_fd, 0);
	if (endpoint->bar0 == MAP_FAILED) {
		SPDK_ERRLOG("%s: error to mmap file %s: %s.\n", endpoint_id(endpoint), path, spdk_strerror(errno));
		endpoint->bar0 = NULL;
		ret = -1;
		goto out;
	}
	endpoint->bar0_doorbells = (uint32_t *)(((unsigned long)endpoint->bar0) + NVME_DOORBELLS_OFFSET);

	ret = snprintf(uuid, PATH_MAX, "%s/cntrl", endpoint_id(endpoint));
	if (ret < 0 || ret >= PATH_MAX) {
		SPDK_ERRLOG("%s: error to get ctrlr file path: %s\n", endpoint_id(endpoint), spdk_strerror(errno));
		ret = -1;
		goto out;
	}

	endpoint->vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, uuid, LIBVFIO_USER_FLAG_ATTACH_NB,
					   endpoint, VFU_DEV_TYPE_PCI);
	if (endpoint->vfu_ctx == NULL) {
		SPDK_ERRLOG("%s: error creating libmuser context: %m\n",
			    endpoint_id(endpoint));
		ret = -1;
		goto out;
	}

	ret = vfu_setup_log(endpoint->vfu_ctx, vfio_user_log,
			    vfio_user_get_log_level());
	if (ret < 0) {
		goto out;
	}


	ret = vfio_user_dev_info_fill(vu_transport, endpoint);
	if (ret < 0) {
		goto out;
	}

	ret = vfio_user_register_accept_poller(endpoint);

	if (ret != 0) {
		goto out;
	}

	pthread_mutex_lock(&vu_transport->lock);
	TAILQ_INSERT_TAIL(&vu_transport->endpoints, endpoint, link);
	pthread_mutex_unlock(&vu_transport->lock);

out:
	if (ret != 0) {
		nvmf_vfio_user_destroy_endpoint(endpoint);
	}

	return ret;
}

/*
 * [한국어]
 * nvmf_vfio_user_stop_listen - 특정 trid 에 대한 listen 중단 (.stop_listen vtable 콜백).
 *
 * @transport: vfio-user transport 객체 (CONTAINEROF 로 vu_transport 복원).
 * @trid: 중단할 listener 의 transport id. traddr (소켓 경로/디렉토리) 로 endpoint 매칭.
 *
 * 동기/배경: nvmf_subsystem_remove_listener RPC 또는 target 종료 시 코어가 listen 을
 *   중단시키려 호출한다. listener 1개는 endpoint 1개 (소켓 1개) 에 대응한다.
 *   핵심 주의점: VM 이 아직 연결되어 controller (endpoint->ctrlr) 가 살아 있을 수 있다.
 *   이때 endpoint 자원을 즉시 해제하면 controller 가 dangling endpoint 를 참조하게
 *   되므로, 해제를 controller free 시점까지 **지연**시킨다 (need_async_destroy 플래그).
 * 동작 단계:
 *   1) lock 잡고 endpoints 를 순회하며 traddr 일치 endpoint 검색.
 *   2) 찾으면 리스트에서 제거.
 *   3) controller 가 살아 있으면 need_async_destroy=true 만 세우고 반환 (지연 해제).
 *      - 코어가 별도로 모든 qpair 를 disconnect → controller free → 그때 endpoint 해제.
 *   4) controller 가 없으면 즉시 endpoint 파괴.
 * 실행 컨텍스트: NVMe-oF 코어의 control thread. vu_transport->lock 으로 endpoints
 *   순회 중 다른 thread (accept poller 등) 의 INSERT/REMOVE 와 경쟁 방지.
 * 호출자: spdk_nvmf_transport_stop_listen → vtable .stop_listen 디스패치.
 * 에러 경로: 못 찾으면 그냥 "not found" 로그 후 반환 (idempotent).
 *
 * 호출 체인:
 *   spdk_nvmf_transport_stop_listen → [nvmf_vfio_user_stop_listen]
 *     → (controller 있음) need_async_destroy 마킹 / (없음) nvmf_vfio_user_destroy_endpoint
 */
static void
nvmf_vfio_user_stop_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_transport *vu_transport;     /* [한국어] 복원할 transport 컨테이너. */
	struct nvmf_vfio_user_endpoint *endpoint, *tmp;    /* [한국어] _SAFE 순회용 현재/다음 endpoint. */

	assert(trid != NULL);             /* [한국어] 코어가 항상 유효 trid 를 넘긴다는 계약. */
	assert(trid->traddr != NULL);     /* [한국어] traddr 로 매칭하므로 NULL 이면 안 됨. */

	SPDK_DEBUGLOG(nvmf_vfio, "%s: stop listen\n", trid->traddr);  /* [한국어] 어느 주소의 listen 을 멈추는지 로그. */

	/* [한국어] generic transport → vfio-user 전용 컨테이너 역참조. */
	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);

	/* [한국어] endpoints 리스트를 수정할 것이므로 transport lock 획득 (accept poller 와 경쟁). */
	pthread_mutex_lock(&vu_transport->lock);
	/* [한국어] traddr 가 일치하는 endpoint 를 찾기 위해 안전 순회 (찾으면 REMOVE 하므로 _SAFE). */
	TAILQ_FOREACH_SAFE(endpoint, &vu_transport->endpoints, link, tmp) {
		/* [한국어] listener trid 의 traddr 과 endpoint 의 traddr 비교 — 1:1 매칭. */
		if (strcmp(trid->traddr, endpoint->trid.traddr) == 0) {
			/* [한국어] 매칭 endpoint 를 먼저 리스트에서 떼어낸다 (이후 accept 안 받게). */
			TAILQ_REMOVE(&vu_transport->endpoints, endpoint, link);
			/* Defer to free endpoint resources until the controller
			 * is freed.  There are two cases when running here:
			 * 1. kill nvmf target while VM is connected
			 * 2. remove listener via RPC call
			 * nvmf library will disconnect all queue paris.
			 */
			/* [한국어] VM 이 연결돼 controller 가 살아 있으면 endpoint 자원 해제를 지연.
			 * controller 가 endpoint->bar0/vfu_ctx 를 참조 중이라 지금 free 하면 use-after-free. */
			if (endpoint->ctrlr) {
				/* [한국어] 중복 지연 요청이 없음을 단언 (한 listener 는 한 번만 stop). */
				assert(!endpoint->need_async_destroy);
				/* [한국어] controller free 경로가 나중에 이 플래그를 보고 endpoint 를 마저 파괴. */
				endpoint->need_async_destroy = true;
				pthread_mutex_unlock(&vu_transport->lock);  /* [한국어] 지연 마킹만 하고 lock 해제. */
				return;
			}

			/* [한국어] controller 가 없으면 (VM 미연결) 즉시 endpoint 전체 파괴. */
			nvmf_vfio_user_destroy_endpoint(endpoint);
			pthread_mutex_unlock(&vu_transport->lock);  /* [한국어] 파괴 후 lock 해제. */
			return;
		}
	}
	pthread_mutex_unlock(&vu_transport->lock);  /* [한국어] 매칭 endpoint 없음 — lock 해제. */

	SPDK_DEBUGLOG(nvmf_vfio, "%s: not found\n", trid->traddr);  /* [한국어] idempotent: 없으면 조용히 반환. */
}

/*
 * [한국어]
 * nvmf_vfio_user_cdata_init - Identify Controller 데이터의 transport 특화 필드 채우기 (.cdata_init 콜백).
 *
 * @transport: vfio-user transport (CONTAINEROF 로 옵션 참조).
 * @subsystem: 대상 subsystem (여기선 미사용 — 시그니처 호환용).
 * @cdata: 코어가 공통 필드를 채운 뒤 transport 가 덮어쓸 Identify Controller 응답 버퍼.
 *
 * 동기/배경: VM 이 Identify Controller (CNS=1) 를 보내면 emulated PCIe device 답게
 *   vendor ID, IEEE OUI, SGL 지원, optional 명령 지원 비트 등을 transport 가 결정해야
 *   한다. RDMA/TCP 와 달리 vfio-user 는 "Nutanix vendor 의 PCIe NVMe" 로 자신을 표현한다.
 * 동작: vid/ssvid = Nutanix, IEEE OUI = 8d:6b:50, SGL = DWORD-aligned 지원, Compare/
 *   shadow doorbell(dbcs)/fused-write(fcws) 지원 여부를 transport 옵션과 코어 기능의
 *   AND 로 결정. reservation 은 1 connection 제약으로 비활성.
 * 실행 컨텍스트: Identify 명령 처리 경로 — controller 가 붙은 spdk_thread.
 * 호출자: NVMe-oF 코어의 Identify Controller 핸들러 → vtable .cdata_init.
 * 에러 경로: 없음 (단순 필드 채움).
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify_ctrlr → [nvmf_vfio_user_cdata_init]
 */
static void
nvmf_vfio_user_cdata_init(struct spdk_nvmf_transport *transport,
			  struct spdk_nvmf_subsystem *subsystem,
			  struct spdk_nvmf_ctrlr_data *cdata)
{
	struct nvmf_vfio_user_transport *vu_transport;  /* [한국어] 옵션을 읽기 위한 컨테이너. */

	/* [한국어] generic transport → vfio-user 컨테이너 역참조. */
	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport, transport);

	cdata->vid = SPDK_PCI_VID_NUTANIX;   /* [한국어] PCI Vendor ID — emulated device 를 Nutanix 로 표시. */
	cdata->ssvid = SPDK_PCI_VID_NUTANIX; /* [한국어] Subsystem Vendor ID 도 동일. */
	cdata->ieee[0] = 0x8d;               /* [한국어] IEEE OUI 바이트0 (little-endian 순서) — namespace GUID 생성에 사용. */
	cdata->ieee[1] = 0x6b;               /* [한국어] IEEE OUI 바이트1. */
	cdata->ieee[2] = 0x50;               /* [한국어] IEEE OUI 바이트2 → OUI 50:6b:8d. */
	/* [한국어] SGL 지원 비트필드를 0 으로 초기화한 뒤 필요한 비트만 설정. */
	memset(&cdata->sgls, 0, sizeof(struct spdk_nvme_cdata_sgls));
	/* [한국어] SGL 지원 = "DWORD 정렬 요구" 모드 (NVMe spec Identify §SGLS). */
	cdata->sgls.supported = SPDK_NVME_SGLS_SUPPORTED_DWORD_ALIGNED;
	/* [한국어] Compare 명령 지원 = transport 옵션(disable_compare 아님) AND 코어 기능. */
	cdata->oncs.nvmcmps = !vu_transport->transport_opts.disable_compare &&
			      vu_transport->transport.opts.oncs.nvmcmps;
	/* libvfio-user can only support 1 connection for now */
	/* [한국어] Reservation 미지원 — 단일 connection 제약 때문에 비활성. */
	cdata->oncs.reservs = 0;
	/* [한국어] Doorbell Buffer Config (dbcs, shadow doorbell) 지원 = 옵션이 끄지 않았으면 1. */
	cdata->oacs.dbcs = !vu_transport->transport_opts.disable_shadow_doorbells;
	/* [한국어] Fused Compare-and-Write (fcws) 지원 = Compare 지원 AND 코어 fused 지원. */
	cdata->fuses.fcws = !vu_transport->transport_opts.disable_compare &&
			    vu_transport->transport.opts.oncs.nvmcmps && vu_transport->transport.opts.fuses.fcws;
}

/*
 * [한국어]
 * nvmf_vfio_user_listen_associate - listener(endpoint) 를 subsystem 에 연결 (.listen_associate 콜백).
 *
 * @transport: vfio-user transport (CONTAINEROF 로 endpoints 접근).
 * @subsystem: 이 listener 와 묶을 NVMe-oF subsystem (const — 내부에서 const 제거하여 저장).
 * @trid: 매칭할 listener 의 transport id (traddr 로 endpoint 검색).
 * @return: 0 성공, -ENOENT 면 해당 traddr 의 endpoint 가 없음.
 *
 * 동기/배경: nvmf_subsystem_add_listener 후, 코어는 어떤 subsystem 이 어떤 listener 를
 *   서비스하는지 transport 에 알려준다. vfio-user 는 나중에 namespace hotplug 등으로
 *   controller 를 pause/unpause 할 때 endpoint→subsystem 역참조가 필요하므로 여기서
 *   endpoint->subsystem 에 저장해 둔다.
 * 동작: traddr 일치 endpoint 를 lock 하에 검색 → 없으면 -ENOENT → 있으면 const 를 떼고
 *   endpoint->subsystem 에 저장.
 * 실행 컨텍스트: NVMe-oF 코어의 control thread (subsystem 구성 시점).
 * 호출자: spdk_nvmf_transport_listen_associate → vtable .listen_associate.
 * 에러 경로: endpoint 미발견 시 -ENOENT 반환 (코어가 association 실패로 처리).
 *
 * 호출 체인:
 *   nvmf_subsystem_add_listener → [nvmf_vfio_user_listen_associate]
 */
static int
nvmf_vfio_user_listen_associate(struct spdk_nvmf_transport *transport,
				const struct spdk_nvmf_subsystem *subsystem,
				const struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_transport *vu_transport;   /* [한국어] endpoints 리스트 소유 컨테이너. */
	struct nvmf_vfio_user_endpoint *endpoint;        /* [한국어] traddr 로 찾을 대상 endpoint. */

	/* [한국어] generic transport → vfio-user 컨테이너 역참조. */
	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport, transport);

	/* [한국어] endpoints 순회 중 동시 수정 방지를 위해 transport lock 획득. */
	pthread_mutex_lock(&vu_transport->lock);
	/* [한국어] traddr 가 일치하는 endpoint 검색 (strncmp 로 traddr 버퍼 크기까지 비교). */
	TAILQ_FOREACH(endpoint, &vu_transport->endpoints, link) {
		if (strncmp(endpoint->trid.traddr, trid->traddr, sizeof(endpoint->trid.traddr)) == 0) {
			break;  /* [한국어] 일치 — endpoint 가 해당 노드를 가리킨 채 루프 탈출. */
		}
	}
	pthread_mutex_unlock(&vu_transport->lock);  /* [한국어] 검색 완료 — lock 해제 (저장은 lock 밖에서 안전). */

	/* [한국어] 순회가 끝까지 돌면 endpoint==NULL → 해당 listener 없음. */
	if (endpoint == NULL) {
		return -ENOENT;
	}

	/* Drop const - we will later need to pause/unpause. */
	/* [한국어] subsystem 은 const 로 받지만, 추후 pause/unpause 호출에 비-const 포인터가
	 * 필요하므로 const 를 명시적으로 떼어 endpoint 에 저장. */
	endpoint->subsystem = (struct spdk_nvmf_subsystem *)subsystem;

	return 0;  /* [한국어] association 성공. */
}

/*
 * Executed periodically at a default SPDK_NVMF_DEFAULT_ACCEPT_POLL_RATE_US
 * frequency.
 *
 * For this endpoint (which at the libvfio-user level corresponds to a socket),
 * if we don't currently have a controller set up, peek to see if the socket is
 * able to accept a new connection.
 */
/*
 * [한국어]
 * nvmf_vfio_user_accept - per-endpoint accept poller. VM connection 수락 + controller 생성.
 *
 * @ctx: nvmf_vfio_user_endpoint.
 * @return: SPDK_POLLER_BUSY/IDLE.
 *
 * RDMA 의 nvmf_rdma_accept 와 비슷하지만 endpoint 마다 1개씩 (RDMA 는 transport 단일).
 * libvfio-user Unix socket 의 accept(2) 폴링. vfu_attach_ctx 가 socket accept + vfu
 * negotiation (capabilities/region setup) 모두 수행.
 *
 * 단계:
 *   1. ctrlr 이미 있으면 → 새 connection 수락 안 함 (1 endpoint = 1 controller 한도).
 *   2. vfu_attach_ctx 호출 (non-blocking — EAGAIN/EWOULDBLOCK = 아직 connect 안 옴).
 *   3. 성공 시: nvmf_vfio_user_create_ctrlr → controller emulation 컨텍스트 생성.
 *   4. ★ accept_poller 자신 unregister! 이후 vfu_run_ctx 가 모든 vfio-user 이벤트 처리.
 *      = accept poller 의 "수명" 은 connection 받기 전까지만.
 *
 * SPDK_NVMF_DEFAULT_ACCEPT_POLL_RATE_US 마다 호출 (보통 10ms). connection 안 오면 IDLE 반환.
 *
 * accept_poller 자가 해제 패턴: 1 endpoint = 1 VM 한도라서 connection 후엔 accept 할 일
 * 없음. CPU 절약 + endpoint 정리 시 endpoint 의 vfu_ctx poller 만 신경 쓰면 됨.
 *
 * VM 종료 시: vfu_ctx poller 가 disconnect 감지 → controller destroy → endpoint->ctrlr=NULL +
 * accept_poller 재등록 (need_relisten 패턴) → 다음 VM connect 대기.
 *
 * 호출 체인:
 *   SPDK reactor → [본 함수] (accept_poller 콜백)
 *     → vfu_attach_ctx (Unix socket accept + vfu negotiation)
 *     → nvmf_vfio_user_create_ctrlr (controller emulation 생성)
 *     → 자가 unregister
 */
static int
nvmf_vfio_user_accept(void *ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = ctx;
	struct nvmf_vfio_user_transport *vu_transport;
	int err;

	vu_transport = endpoint->transport;

	/* [한국어] 1 endpoint = 1 controller 한도 — 이미 있으면 무시. */
	if (endpoint->ctrlr != NULL) {
		return SPDK_POLLER_IDLE;
	}

	/* [한국어] non-blocking attach — connection 안 와도 EAGAIN. */
	err = vfu_attach_ctx(endpoint->vfu_ctx);
	if (err == 0) {
		SPDK_DEBUGLOG(nvmf_vfio, "attach succeeded\n");
		err = nvmf_vfio_user_create_ctrlr(vu_transport, endpoint);
		if (err == 0) {
			/*
			 * Unregister ourselves: now we've accepted a
			 * connection, there is nothing for us to poll for, and
			 * we will poll the connection via vfu_run_ctx()
			 * instead.
			 */
			/* [한국어] 자가 unregister — 이후 vfu_ctx_poller 가 vfio-user 이벤트 처리.
			 * VM 종료 시 need_relisten=true 로 다시 등록. */
			spdk_interrupt_unregister(&endpoint->accept_intr);
			spdk_poller_unregister(&endpoint->accept_poller);
		}
		return SPDK_POLLER_BUSY;
	}

	/* [한국어] non-blocking attach 에서 EAGAIN = "아직 connect 안 옴" (정상). */
	if (errno == EAGAIN || errno == EWOULDBLOCK) {
		return SPDK_POLLER_IDLE;
	}

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * nvmf_vfio_user_discover - Discovery Log Page entry 채우기 콜백 (.listener_discover) — 의도적 빈 구현.
 *
 * @transport: vfio-user transport (미사용).
 * @trid: 발견 대상 listener 의 transport id (미사용).
 * @entry: 코어가 Discovery Log Page 응답에 추가할 entry 버퍼 (미사용).
 *
 * 동기/배경: RDMA/TCP 같은 네트워크 transport 는 NVMe-oF Discovery 서비스로 호스트가
 *   subsystem 목록을 원격에서 질의할 수 있어야 하므로 이 콜백이 trid/subnqn 등을 entry 에
 *   채운다. 그러나 vfio-user 는 같은 머신 안에서 emulated PCIe 장치로 노출되며, VM 은
 *   discovery 프로토콜 없이 PCI bus probe 로 직접 장치를 인식한다. 따라서 채울 와이어
 *   discovery entry 가 존재하지 않아 본 함수는 의도적으로 비어 있다 (no-op).
 * 동작: 아무 것도 하지 않는다 — vtable 슬롯을 NULL 로 두지 않고 빈 함수로 채워, 코어가
 *   discovery 경로에서 NULL 포인터 역참조를 하지 않도록 한다.
 * 실행 컨텍스트: 코어가 Discovery Log 작성 시 호출하지만 vfio-user 에서는 사실상 호출되지 않음.
 * 호출자: nvmf 코어의 discovery log 생성 경로 → vtable .listener_discover.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   nvmf discovery log build → [nvmf_vfio_user_discover] (no-op)
 */
static void
nvmf_vfio_user_discover(struct spdk_nvmf_transport *transport,
			struct spdk_nvme_transport_id *trid,
			struct spdk_nvmf_discovery_log_page_entry *entry)
{ }

/* [한국어] vfio_user_poll_group_intr 의 forward 선언 — 아래 add_intr 가 등록 콜백으로
 * 참조하지만 정의는 더 아래(5251 부근)에 있어 미리 알려준다. */
static int vfio_user_poll_group_intr(void *ctx);

/*
 * [한국어]
 * vfio_user_poll_group_add_intr - poll group 에 interrupt-mode wakeup eventfd 부착.
 *
 * @vu_group: interrupt 자원을 붙일 vfio-user poll group.
 * @group: 대응하는 generic nvmf poll group (현재 미사용 — 시그니처 일관성용).
 *
 * 동기/배경: interrupt 모드에서는 reactor 가 무한 폴링하지 않고 fd 이벤트로 깨어난다.
 *   다른 reactor 의 doorbell 처리(SQ spreading)가 이 PG 의 SQ 를 깨워야 할 때, 그 reactor 가
 *   이 eventfd 에 write 하여 epoll 기반 reactor 를 wake-up 시킨다. 따라서 PG 마다 전용
 *   eventfd + spdk_interrupt 등록이 필요하다.
 * 동작 단계:
 *   1) eventfd(0, EFD_NONBLOCK) 로 cross-reactor wakeup 용 fd 생성 (non-blocking read).
 *   2) SPDK_INTERRUPT_REGISTER 로 그 fd 에 vfio_user_poll_group_intr 콜백을 묶어
 *      reactor 의 epoll set 에 등록 — fd 가 readable 해지면 콜백 실행.
 * 실행 컨텍스트: poll group create 경로 — PG 를 소유할 spdk_thread (init/생성 시점).
 * 호출자: nvmf_vfio_user_poll_group_create (interrupt 모드일 때만).
 * 에러 경로: 실패는 assert 로 잡는다 (커널 fd/interrupt 자원 고갈은 치명적).
 *
 * 호출 체인:
 *   nvmf_vfio_user_poll_group_create → [vfio_user_poll_group_add_intr]
 *     → eventfd / SPDK_INTERRUPT_REGISTER
 */
static void
vfio_user_poll_group_add_intr(struct nvmf_vfio_user_poll_group *vu_group,
			      struct spdk_nvmf_poll_group *group)
{
	/* [한국어] cross-reactor wakeup 용 eventfd 생성. EFD_NONBLOCK 으로 read 시 블로킹 없이
	 * 누적 카운터를 회수 (없으면 EAGAIN). 커널 syscall eventfd(2). */
	vu_group->intr_fd = eventfd(0, EFD_NONBLOCK);
	assert(vu_group->intr_fd != -1);  /* [한국어] fd 고갈/권한 문제는 치명적 — assert. */

	/* [한국어] 위 fd 를 reactor epoll set 에 등록하고 readable 시 호출할 콜백을 지정.
	 * 이후 다른 reactor 가 이 fd 에 8B write 하면 vfio_user_poll_group_intr 가 깨어난다. */
	vu_group->intr = SPDK_INTERRUPT_REGISTER(vu_group->intr_fd,
			 vfio_user_poll_group_intr, vu_group);
	assert(vu_group->intr != NULL);  /* [한국어] interrupt 등록 실패도 치명적. */
}

/*
 * [한국어]
 * nvmf_vfio_user_poll_group_create - vfio-user poll group 객체 생성 (.poll_group_create 콜백).
 *
 * @transport: vfio-user transport (CONTAINEROF 로 vu_transport 복원).
 * @group: 코어 generic poll group (현재 미사용 — add_intr 시그니처 호환용).
 * @return: 성공 시 generic poll group 포인터(vu_group->group 멤버), 실패 시 NULL.
 *
 * 동기/배경: nvmf 코어는 reactor(=spdk_thread)마다 transport별 poll group 을 1개씩 만든다.
 *   vfio-user 의 poll group 은 자신에게 배정된 SQ 들의 리스트(sqs)를 hot path 에서 폴링하는
 *   단위다. 모든 PG 는 transport 의 poll_groups TAILQ 에 묶여 RR(round-robin) 분배(next_pg)에
 *   참여한다.
 * 동작 단계:
 *   1) vu_group zero-alloc.
 *   2) interrupt 모드면 cross-reactor wakeup eventfd 부착(add_intr).
 *   3) 자신의 SQ 리스트(sqs) INIT.
 *   4) pg_lock 아래에서 poll_groups TAILQ 에 등록하고, 최초 등록이면 next_pg 초기화.
 * 실행 컨텍스트: 코어가 각 reactor 의 spdk_thread 에서 호출. poll_groups 리스트는 여러
 *   reactor 가 동시에 접근하므로 pg_lock(mutex)로 보호 — hot path 가 아닌 생성 경로라 OK.
 * 호출자: spdk_nvmf_poll_group_create → vtable .poll_group_create.
 * 에러 경로: alloc 실패 시 NULL (코어가 PG 없이 진행하지 않음).
 *
 * 호출 체인:
 *   spdk_nvmf_poll_group_create → [nvmf_vfio_user_poll_group_create]
 *     → calloc / vfio_user_poll_group_add_intr / TAILQ_INSERT
 */
static struct spdk_nvmf_transport_poll_group *
nvmf_vfio_user_poll_group_create(struct spdk_nvmf_transport *transport,
				 struct spdk_nvmf_poll_group *group)
{
	struct nvmf_vfio_user_transport *vu_transport;  /* [한국어] poll_groups 리스트 소유 컨테이너. */
	struct nvmf_vfio_user_poll_group *vu_group;      /* [한국어] 새로 만들 PG. */

	/* [한국어] generic transport → vfio-user 컨테이너 역참조. */
	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);

	SPDK_DEBUGLOG(nvmf_vfio, "create poll group\n");  /* [한국어] PG 생성 진입 로그. */

	/* [한국어] PG zero-alloc (sqs/stats/next 등 모든 필드 0 초기화). */
	vu_group = calloc(1, sizeof(*vu_group));
	if (vu_group == NULL) {
		SPDK_ERRLOG("Error allocating poll group: %m");  /* [한국어] %m = errno (OOM 등). */
		return NULL;
	}

	/* [한국어] interrupt 모드일 때만 cross-reactor wakeup eventfd 를 PG 에 부착. */
	if (in_interrupt_mode(vu_transport)) {
		vfio_user_poll_group_add_intr(vu_group, group);
	}

	TAILQ_INIT(&vu_group->sqs);  /* [한국어] 이 PG 가 폴링할 SQ 리스트를 빈 상태로 초기화. */

	/* [한국어] poll_groups 리스트는 여러 reactor 가 공유 → pg_lock 으로 보호 후 등록. */
	pthread_mutex_lock(&vu_transport->pg_lock);
	TAILQ_INSERT_TAIL(&vu_transport->poll_groups, vu_group, link);  /* [한국어] RR 분배 대상 등록. */
	/* [한국어] 첫 PG 라면 RR 커서(next_pg)를 이 PG 로 초기화. */
	if (vu_transport->next_pg == NULL) {
		vu_transport->next_pg = vu_group;
	}
	pthread_mutex_unlock(&vu_transport->pg_lock);  /* [한국어] 등록 완료 — 락 해제. */

	return &vu_group->group;  /* [한국어] 코어에 generic PG 포인터 반환 (이후 콜백 인자로 받음). */
}

/*
 * [한국어]
 * nvmf_vfio_user_get_optimal_poll_group - qpair 를 배정할 최적 poll group 선택 (.get_optimal_poll_group).
 *
 * @qpair: 새로 연결되는 qpair (CONTAINEROF 로 sq 복원).
 * @return: 배정할 generic poll group 포인터, 비어 있으면 NULL.
 *
 * 동기/배경: nvmf 코어는 새 qpair 가 생기면 어느 reactor(=PG)에서 처리할지 transport 에게
 *   묻는다. 잘못 배정하면 I/O 완료 시 cross-thread spdk_thread_send_msg 가 강제되어 성능이
 *   떨어진다. vfio-user 는 다음 정책으로 cross-thread 를 피한다:
 *     - I/O qpair 이고 공유 CQ 가 이미 PG 를 가졌으면 그 CQ 의 PG 를 재사용 (완료가 같은
 *       thread 에서 일어나도록).
 *     - interrupt 모드이고 SQ spreading 이 꺼져 있으면 controller 의 모든 qpair 를 admin
 *       SQ(sqs[0])의 PG 에 정렬 (단일 fd wakeup 관리 단순화).
 *     - 그 외에는 next_pg 커서로 RR 분배.
 * 동작 단계: pg_lock 획득 → 위 우선순위로 result 결정 → CQ 가 아직 PG 미배정이면 result 로
 *   고정 → 락 해제.
 * 실행 컨텍스트: qpair 생성 경로(코어). poll_groups/next_pg 공유 자료 접근이라 pg_lock 필요.
 * 호출자: spdk_nvmf_tgt_new_qpair → vtable .get_optimal_poll_group.
 * 에러 경로: poll group 이 하나도 없으면 NULL (코어가 연결 실패 처리).
 *
 * 호출 체인:
 *   spdk_nvmf_tgt_new_qpair → [본 함수] → (선택된 PG 로) nvmf_vfio_user_poll_group_add
 */
static struct spdk_nvmf_transport_poll_group *
nvmf_vfio_user_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_vfio_user_transport *vu_transport;  /* [한국어] poll_groups/next_pg 소유 컨테이너. */
	struct nvmf_vfio_user_poll_group **vu_group;     /* [한국어] RR 커서(next_pg)를 가리키는 더블 포인터. */
	struct nvmf_vfio_user_sq *sq;                    /* [한국어] qpair 가 속한 SQ. */
	struct nvmf_vfio_user_cq *cq;                    /* [한국어] 이 SQ 의 완료가 들어갈 CQ (공유 가능). */

	struct spdk_nvmf_transport_poll_group *result = NULL;  /* [한국어] 선택 결과 (기본 NULL). */

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);  /* [한국어] qpair → SQ 복원. */
	cq = sq->ctrlr->cqs[sq->cqid];  /* [한국어] SQ 가 완료를 보낼 CQ (cqid 로 인덱스). */
	assert(cq != NULL);             /* [한국어] CQ 는 SQ 생성 전에 만들어져 있어야 함. */
	vu_transport = SPDK_CONTAINEROF(qpair->transport, struct nvmf_vfio_user_transport, transport);  /* [한국어] transport 복원. */

	pthread_mutex_lock(&vu_transport->pg_lock);  /* [한국어] poll_groups/next_pg 공유 접근 보호. */
	/* [한국어] PG 가 하나도 없으면 배정 불가 → result NULL 유지하고 out. */
	if (TAILQ_EMPTY(&vu_transport->poll_groups)) {
		goto out;
	}

	/* [한국어] admin qpair 가 아니면(=I/O qpair) cross-thread 회피 정책을 먼저 시도. */
	if (!nvmf_qpair_is_admin_queue(qpair)) {
		/*
		 * If this is shared IO CQ case, just return the used CQ's poll
		 * group, so I/O completions don't have to use
		 * spdk_thread_send_msg().
		 */
		/* [한국어] 공유 IO CQ 가 이미 PG 를 가졌으면 그 PG 재사용 — 완료가 같은 thread 에서
		 * 일어나 spdk_thread_send_msg 가 불필요해진다(완료 hot path 최적화). */
		if (cq->group != NULL) {
			result = cq->group;
			goto out;
		}

		/*
		 * If we're in interrupt mode, align all qpairs for a controller
		 * on the same poll group by default, unless requested. This can
		 * be lower in performance than running on a single poll group,
		 * so we disable spreading by default.
		 */
		/* [한국어] interrupt 모드 + spreading 비활성: controller 의 모든 qpair 를 admin
		 * SQ(sqs[0])의 PG 에 정렬. 단일 wakeup fd 로 묶여 관리가 단순해진다(기본값). */
		if (in_interrupt_mode(vu_transport) &&
		    !vu_transport->transport_opts.enable_intr_mode_sq_spreading) {
			result = sq->ctrlr->sqs[0]->group;
			goto out;
		}

	}

	/* [한국어] 위 특수 케이스에 안 걸렸으면 RR 커서로 다음 PG 선택. */
	vu_group = &vu_transport->next_pg;
	assert(*vu_group != NULL);  /* [한국어] poll_groups 비어있지 않으면 커서도 유효. */

	result = &(*vu_group)->group;             /* [한국어] 현재 커서 PG 를 결과로. */
	*vu_group = TAILQ_NEXT(*vu_group, link);   /* [한국어] 커서를 다음 PG 로 전진. */
	/* [한국어] 끝까지 갔으면 리스트 처음으로 wrap-around (원형 RR). */
	if (*vu_group == NULL) {
		*vu_group = TAILQ_FIRST(&vu_transport->poll_groups);
	}

out:
	/* [한국어] 이 CQ 가 아직 PG 미배정이면 방금 고른 result 로 고정 — 이후 이 CQ 를 공유하는
	 * SQ 들이 같은 PG 로 모이게 한다(위 첫 분기 재사용 조건). */
	if (cq->group == NULL) {
		cq->group = result;
	}

	pthread_mutex_unlock(&vu_transport->pg_lock);  /* [한국어] 선택 완료 — 락 해제. */
	return result;  /* [한국어] 코어가 이 PG 로 qpair 를 add 한다. */
}

/*
 * [한국어]
 * vfio_user_poll_group_del_intr - poll group 의 interrupt-mode wakeup eventfd 해제.
 *
 * @vu_group: interrupt 자원을 떼어낼 PG.
 *
 * 동기/배경: add_intr 로 부착한 eventfd + spdk_interrupt 를 PG 파괴 시 정확히 역순으로
 *   되돌려 reactor epoll set 에서 빼고 fd 를 닫는다. 누락 시 fd 누수 + dangling interrupt.
 * 동작 단계:
 *   1) spdk_interrupt_unregister 로 reactor epoll set 에서 콜백 등록 제거.
 *   2) close(intr_fd) 로 커널 eventfd 반환, intr_fd 를 -1 sentinel 로 표시.
 * 실행 컨텍스트: poll group destroy 경로 — PG 를 소유하던 spdk_thread.
 * 호출자: nvmf_vfio_user_poll_group_destroy (interrupt 모드일 때만).
 * 에러 경로: 없음 (assert 로 부착 여부만 검증).
 *
 * 호출 체인:
 *   nvmf_vfio_user_poll_group_destroy → [vfio_user_poll_group_del_intr]
 *     → spdk_interrupt_unregister / close
 */
static void
vfio_user_poll_group_del_intr(struct nvmf_vfio_user_poll_group *vu_group)
{
	assert(vu_group->intr_fd != -1);  /* [한국어] interrupt 모드에서 add_intr 가 호출됐어야 함. */

	spdk_interrupt_unregister(&vu_group->intr);  /* [한국어] reactor epoll set 에서 콜백 등록 제거. */

	close(vu_group->intr_fd);   /* [한국어] 커널 eventfd 반환 (syscall close(2)). */
	vu_group->intr_fd = -1;     /* [한국어] 해제됨 표시 sentinel. */
}

/* called when process exits */
/*
 * [한국어]
 * nvmf_vfio_user_poll_group_destroy - vfio-user poll group 파괴 (.poll_group_destroy 콜백).
 *
 * @group: 파괴할 generic poll group (CONTAINEROF 로 vu_group 복원).
 *
 * 동기/배경: 프로세스 종료/reactor 정리 시 코어가 각 PG 를 파괴한다. create 의 역순으로
 *   interrupt 자원 해제 + poll_groups 리스트에서 제거 + next_pg 커서 보정 + 메모리 해제를
 *   수행한다. RR 커서가 파괴 대상 PG 를 가리키고 있으면 다음 PG 로 옮겨 dangling 을 막는다.
 * 동작 단계:
 *   1) interrupt 모드면 del_intr 로 wakeup fd 정리.
 *   2) pg_lock 아래에서 다음 PG 를 미리 구해 두고 리스트에서 제거.
 *   3) next_pg 가 이 PG 였으면 다음 PG(없으면 first)로 보정.
 *   4) PG 메모리 free.
 * 실행 컨텍스트: PG 를 소유하던 spdk_thread. poll_groups 공유 접근이라 pg_lock 필요.
 * 호출자: spdk_nvmf_poll_group_destroy → vtable .poll_group_destroy.
 * 에러 경로: 없음 (항상 성공).
 *
 * 호출 체인:
 *   spdk_nvmf_poll_group_destroy → [nvmf_vfio_user_poll_group_destroy]
 *     → vfio_user_poll_group_del_intr / TAILQ_REMOVE / free
 */
static void
nvmf_vfio_user_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct nvmf_vfio_user_poll_group *vu_group, *next_tgroup;  /* [한국어] 파괴 대상 PG / next_pg 보정용 후보. */
	struct nvmf_vfio_user_transport *vu_transport;             /* [한국어] poll_groups 소유 컨테이너. */

	SPDK_DEBUGLOG(nvmf_vfio, "destroy poll group\n");  /* [한국어] PG 파괴 진입 로그. */

	vu_group = SPDK_CONTAINEROF(group, struct nvmf_vfio_user_poll_group, group);  /* [한국어] generic → vfio PG 복원. */
	/* [한국어] PG 가 기억하는 transport 포인터로부터 컨테이너 복원. */
	vu_transport = SPDK_CONTAINEROF(vu_group->group.transport, struct nvmf_vfio_user_transport,
					transport);

	/* [한국어] interrupt 모드였다면 wakeup eventfd 자원부터 해제(create 의 역순). */
	if (in_interrupt_mode(vu_transport)) {
		vfio_user_poll_group_del_intr(vu_group);
	}

	pthread_mutex_lock(&vu_transport->pg_lock);  /* [한국어] poll_groups/next_pg 공유 접근 보호. */
	next_tgroup = TAILQ_NEXT(vu_group, link);    /* [한국어] 제거 전에 다음 PG 를 미리 확보. */
	TAILQ_REMOVE(&vu_transport->poll_groups, vu_group, link);  /* [한국어] RR 분배 리스트에서 제거. */
	/* [한국어] 이 PG 가 마지막이었으면 다음 후보를 리스트 처음으로 wrap. */
	if (next_tgroup == NULL) {
		next_tgroup = TAILQ_FIRST(&vu_transport->poll_groups);
	}
	/* [한국어] RR 커서가 파괴 대상을 가리키면 다음 PG 로 옮겨 dangling 방지. */
	if (vu_transport->next_pg == vu_group) {
		vu_transport->next_pg = next_tgroup;
	}
	pthread_mutex_unlock(&vu_transport->pg_lock);  /* [한국어] 리스트 정리 완료 — 락 해제. */

	free(vu_group);  /* [한국어] PG 구조체 메모리 반환. */
}

/*
 * [한국어]
 * _vfio_user_qpair_disconnect - 단일 SQ 의 qpair 를 disconnect 시키는 메시지 콜백.
 *
 * @ctx: 대상 nvmf_vfio_user_sq (spdk_thread_send_msg 로 전달됨).
 *
 * 동기/배경: vfio_user_destroy_ctrlr 가 endpoint->lock 을 잡은 채로 직접
 *   spdk_nvmf_qpair_disconnect 를 호출하면, 그 콜백 경로가 다시 endpoint->lock 을 잡으려
 *   해 재귀 데드락이 발생할 수 있다. 그래서 disconnect 를 메시지로 미뤄(다음 poll 라운드)
 *   락이 풀린 컨텍스트에서 실행되게 한다.
 * 동작: 받은 SQ 의 qpair 에 대해 코어 disconnect 를 호출 — 이후 qpair_fini→close_qpair 경로
 *   진입.
 * 실행 컨텍스트: ctrlr->thread 의 메시지 핸들러로 실행 (락 미보유 상태).
 * 호출자: spdk_thread_send_msg (vfio_user_destroy_ctrlr 가 큐잉).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   spdk_thread_send_msg → [_vfio_user_qpair_disconnect] → spdk_nvmf_qpair_disconnect
 */
static void
_vfio_user_qpair_disconnect(void *ctx)
{
	struct nvmf_vfio_user_sq *sq = ctx;  /* [한국어] 메시지로 전달된 disconnect 대상 SQ. */

	spdk_nvmf_qpair_disconnect(&sq->qpair);  /* [한국어] 코어에 qpair 종료 요청 (락 풀린 상태라 안전). */
}

/* The function is used when socket connection is destroyed */
/*
 * [한국어]
 * vfio_user_destroy_ctrlr - vfio-user socket 연결이 끊겼을 때 controller 정리.
 *
 * @ctrlr: 파괴할 vfio-user controller.
 * @return: 항상 0.
 *
 * 동기/배경: VM 종료/disconnect(ENOTCONN) 로 더는 처리할 수 없게 된 controller 를 정리한다.
 *   같은 endpoint 로 새 VM 이 다시 붙을 수 있도록 need_relisten 을 세워 socket 을 재오픈하게
 *   하고, 아직 연결된 qpair(connected_sqs)가 남아 있으면 그것들을 먼저 안전하게 disconnect
 *   시킨 뒤에야 free_ctrlr 가 일어나도록 한다.
 * 동작 단계:
 *   1) endpoint->lock 획득, need_relisten=true(재listen 예약) + disconnect=true 마킹.
 *   2) 연결된 SQ 가 없으면 즉시 endpoint->ctrlr=NULL + free_ctrlr 후 반환.
 *   3) 연결된 SQ 가 있으면 각 SQ 에 대해 disconnect 메시지를 큐잉(락 재귀 회피) — 마지막
 *      qpair 가 close 될 때 free_ctrlr 가 일어난다.
 * 실행 컨텍스트: ctrlr->thread (vfu_ctx poller 에러 경로). endpoint->lock 으로 ctrlr 상태 보호.
 * 호출자: vfio_user_poll_vfu_ctx (ENOTCONN 감지 시).
 * 에러 경로: 항상 성공 (0 반환).
 *
 * 호출 체인:
 *   vfio_user_poll_vfu_ctx(ENOTCONN) → [vfio_user_destroy_ctrlr]
 *     → free_ctrlr (즉시) 또는 spdk_thread_send_msg(_vfio_user_qpair_disconnect) (지연)
 */
static int
vfio_user_destroy_ctrlr(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_sq *sq;             /* [한국어] connected_sqs 순회용. */
	struct nvmf_vfio_user_endpoint *endpoint; /* [한국어] controller 가 속한 endpoint. */

	SPDK_DEBUGLOG(nvmf_vfio, "%s stop processing\n", ctrlr_id(ctrlr));  /* [한국어] 처리 중단 로그. */

	endpoint = ctrlr->endpoint;   /* [한국어] socket/BAR 자원을 가진 endpoint. */
	assert(endpoint != NULL);     /* [한국어] running controller 는 항상 endpoint 보유. */

	pthread_mutex_lock(&endpoint->lock);  /* [한국어] ctrlr 상태/connected_sqs 보호. */
	endpoint->need_relisten = true;  /* [한국어] 새 VM 이 다시 붙을 수 있도록 socket 재오픈 예약. */
	ctrlr->disconnect = true;        /* [한국어] disconnect 진행 중 표시 (중복 진입 가드). */
	/* [한국어] 연결된 qpair 가 없으면 곧장 controller free 후 endpoint 분리. */
	if (TAILQ_EMPTY(&ctrlr->connected_sqs)) {
		endpoint->ctrlr = NULL;   /* [한국어] endpoint 가 더는 이 ctrlr 를 참조하지 않게. */
		free_ctrlr(ctrlr);        /* [한국어] vfu_ctx/SQ/CQ/메모리 등 일괄 해제. */
		pthread_mutex_unlock(&endpoint->lock);
		return 0;
	}

	/* [한국어] 연결된 SQ 들이 있으면 각각 disconnect 를 메시지로 큐잉. */
	TAILQ_FOREACH(sq, &ctrlr->connected_sqs, tailq) {
		/* add another round thread poll to avoid recursive endpoint lock */
		/* [한국어] 지금 락을 잡은 채로 직접 disconnect 하면 재귀 endpoint lock 데드락 →
		 * 다음 poll 라운드에서 락 없이 실행되도록 메시지로 미룬다. */
		spdk_thread_send_msg(ctrlr->thread, _vfio_user_qpair_disconnect, sq);
	}
	pthread_mutex_unlock(&endpoint->lock);  /* [한국어] 큐잉 완료 — 락 해제(실제 free 는 마지막 close 시). */

	return 0;
}

/*
 * Poll for and process any incoming vfio-user messages.
 */
static int
/*
 * [한국어]
 * vfio_user_poll_vfu_ctx - libvfio-user 의 pending 이벤트 처리 poller.
 *
 * @ctx: nvmf_vfio_user_ctrlr.
 * @return: SPDK_POLLER_BUSY/IDLE.
 *
 * controller 생성 시 등록되는 ctrlr->vfu_ctx_poller 의 콜백. SPDK reactor 가 매 iter 호출.
 *
 * 단일 작업: vfu_run_ctx 호출 — libvfio-user 의 모든 pending 작업 처리:
 *   - BAR access (access_bar0_fn 콜백 호출)
 *   - DMA map/unmap (memory_region_add/remove_cb 콜백)
 *   - IRQ delivery (vfu_irq_trigger 의 결과 reaping)
 *   - vfio-user message handling
 *
 * 에러 처리:
 *   - EBUSY: 다른 vfu API 가 vfu_ctx 사용 중 (재귀 호출 보호) → 다음 iter 재시도.
 *   - ENOTCONN: VM 측 disconnect (예: kill -9, VM 정상 종료) — controller destroy + unregister.
 *     reset callback 이 이미 intr unregister 했으므로 추가 cleanup 불필요.
 *   - 기타 에러: fail_ctrlr — interrupt 명시적 unregister 후 controller fatal.
 *
 * vfu_run_ctx 가 핵심: BAR access 처리가 여기서 일어남 (access_bar0_fn 의 호출 진입점).
 * 따라서 vfio_user_ctrlr_intr 가 본 함수를 SQ poll 보다 먼저 호출하는 이유 (Part 2 인사이트).
 *
 * 호출 체인:
 *   SPDK reactor poller → [본 함수] (vfu_ctx_poller 콜백)
 *     → vfu_run_ctx → access_bar0_fn / memory_region_*_cb / ...
 *     → 에러 시 destroy_ctrlr 또는 fail_ctrlr
 */
static int
vfio_user_poll_vfu_ctx(void *ctx)
{
	struct nvmf_vfio_user_ctrlr *ctrlr = ctx;
	int ret;

	assert(ctrlr != NULL);

	/* This will call access_bar0_fn() if there are any writes
	 * to the portion of the BAR that is not mmap'd */
	/* [한국어] vfu_run_ctx 가 모든 pending vfio-user 이벤트 처리 (BAR/DMA/IRQ/message). */
	ret = vfu_run_ctx(ctrlr->endpoint->vfu_ctx);
	if (spdk_unlikely(ret == -1)) {
		/* [한국어] EBUSY = 재귀 호출 보호 (다른 vfu API 사용 중) — 다음 iter 재시도. */
		if (errno == EBUSY) {
			return SPDK_POLLER_IDLE;
		}

		spdk_poller_unregister(&ctrlr->vfu_ctx_poller);

		/*
		 * We lost the client; the reset callback will already have
		 * unregistered the interrupt.
		 */
		/* [한국어] ENOTCONN = VM 측 disconnect (kill, 종료, hot-unplug). controller destroy.
		 * reset callback 이 이미 intr unregister 했으므로 명시적 cleanup 불필요. */
		if (errno == ENOTCONN) {
			vfio_user_destroy_ctrlr(ctrlr);
			return SPDK_POLLER_BUSY;
		}

		/*
		 * We might not have got a reset callback in this case, so
		 * explicitly unregister the interrupt here.
		 */
		/* [한국어] 그 외 에러 (예: socket protocol error) — 명시적 intr unregister 후 fail. */
		spdk_interrupt_unregister(&ctrlr->intr);
		ctrlr->intr_fd = -1;
		fail_ctrlr(ctrlr);
	}

	return ret != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

/* [한국어] cross-thread 로 CQE 를 게시하기 위해 post_completion 인자를 묶어 전달하는 컨텍스트.
 * 완료를 발생시킨 thread 와 admin CQ 소유 thread 가 다를 때, 이 구조체를 힙에 만들어
 * spdk_thread_send_msg 로 CQ 소유 thread 에 넘긴다. */
struct vfio_user_post_cpl_ctx {
	struct nvmf_vfio_user_ctrlr	*ctrlr;
	/* [한국어] CQE 를 게시할 대상 controller.
	 * 설정자: handle_queue_connect_rsp 가 cross-thread 완료가 필요할 때 채움.
	 * 읽는 자: _post_completion_msg 가 post_completion 첫 인자로 사용.
	 * 값 범위: 유효한 vu_ctrlr 포인터 (NULL 불가). 메시지 전달 동안 살아 있어야 함.
	 * 동기화: 메시지 1건당 1 컨텍스트 — 소유권이 송신→수신 thread 로 이동, 별도 락 없음. */

	struct nvmf_vfio_user_cq	*cq;
	/* [한국어] CQE 가 들어갈 CQ (보통 admin CQ[0]).
	 * 설정자: handle_queue_connect_rsp.
	 * 읽는 자: _post_completion_msg 가 post_completion 두 번째 인자로 사용.
	 * 값 범위: 유효한 CQ 포인터. cq->group->group->thread 가 수신 thread 와 일치.
	 * 동기화: 수신 thread 에서만 CQ doorbell/head 를 갱신하므로 락 불필요. */

	struct spdk_nvme_cpl		cpl;
	/* [한국어] 게시할 NVMe Completion Queue Entry 사본 (cdw0/sqid/cid/status 포함).
	 * 설정자: handle_queue_connect_rsp 가 Create IO SQ 완료 CQE 필드를 채움.
	 * 읽는 자: _post_completion_msg 가 각 필드를 풀어 post_completion 에 전달.
	 * 값 범위: NVMe spec §4.6 CQE 포맷. status.sc/sct 로 성공/에러 표현.
	 * 동기화: 힙 사본이라 원본 SQE/타 thread 상태와 분리 — race 없음. */
};

/*
 * [한국어]
 * _post_completion_msg - cross-thread 완료 게시 메시지 핸들러.
 *
 * @ctx: 힙에 할당된 vfio_user_post_cpl_ctx (소유권을 넘겨받음).
 *
 * 동기/배경: CQE 는 CQ 를 소유한 thread 에서만 안전하게 게시(head/doorbell 갱신)할 수 있다.
 *   완료를 만든 thread 가 다른 reactor 면, 컨텍스트를 메시지로 넘겨 CQ 소유 thread 에서
 *   실제 post_completion 을 수행한다.
 * 동작: 컨텍스트 필드를 풀어 post_completion 호출 후, 힙 컨텍스트를 free 한다(소유권 종료).
 * 실행 컨텍스트: cq->group->group->thread (CQ 소유 reactor).
 * 호출자: spdk_thread_send_msg (handle_queue_connect_rsp 가 큐잉).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   spdk_thread_send_msg → [_post_completion_msg] → post_completion → free
 */
static void
_post_completion_msg(void *ctx)
{
	struct vfio_user_post_cpl_ctx *cpl_ctx = ctx;  /* [한국어] 넘겨받은 완료 컨텍스트. */

	/* [한국어] CQE 필드를 풀어 실제 게시 — CQ 소유 thread 라 doorbell/head 갱신이 안전. */
	post_completion(cpl_ctx->ctrlr, cpl_ctx->cq, cpl_ctx->cpl.cdw0, cpl_ctx->cpl.sqid,
			cpl_ctx->cpl.cid, cpl_ctx->cpl.status.sc, cpl_ctx->cpl.status.sct);
	free(cpl_ctx);  /* [한국어] 메시지 1건 처리 끝 — 힙 컨텍스트 반환. */
}

/* [한국어] nvmf_vfio_user_poll_group_poll 의 forward 선언 — interrupt 경로의
 * vfio_user_poll_group_process 가 먼저 호출하지만 정의는 파일 뒤쪽에 있어 미리 알려준다. */
static int nvmf_vfio_user_poll_group_poll(struct spdk_nvmf_transport_poll_group *group);

/*
 * [한국어]
 * vfio_user_poll_group_process - interrupt 깨어남 시 PG 의 SQ 폴링 + eventidx 재무장.
 *
 * @ctx: nvmf_vfio_user_poll_group.
 * @return: 처리한 일이 있으면 SPDK_POLLER_BUSY, 없으면 IDLE.
 *
 * 동기/배경: interrupt 모드에서 eventfd 가 깨어나면 실제 SQE 처리(poll)와 shadow doorbell
 *   eventidx 재설정(rearm)을 한 번에 수행해야 한다. rearm 을 빼먹으면 다음 doorbell write 가
 *   wakeup 을 트리거하지 못해 I/O 가 멈춘다.
 * 동작 단계:
 *   1) nvmf_vfio_user_poll_group_poll 로 이 PG 의 ACTIVE SQ 들을 폴링.
 *   2) vfio_user_poll_group_rearm 으로 eventidx 재무장(다른 controller SQ 도 함께 rearm 가능).
 *   3) 통계 갱신.
 * 실행 컨텍스트: PG 소유 reactor 의 interrupt 콜백(또는 poll fallback).
 * 호출자: vfio_user_poll_group_intr (eventfd 깨어남 시).
 * 에러 경로: ret 누적값으로 BUSY/IDLE 만 구분.
 *
 * 호출 체인:
 *   vfio_user_poll_group_intr → [본 함수]
 *     → nvmf_vfio_user_poll_group_poll → vfio_user_poll_group_rearm
 */
static int
vfio_user_poll_group_process(void *ctx)
{
	struct nvmf_vfio_user_poll_group *vu_group = ctx;  /* [한국어] 깨어난 PG. */
	int ret = 0;  /* [한국어] poll/rearm 결과 OR 누적 (BUSY 판정용). */

	SPDK_DEBUGLOG(vfio_user_db, "pg:%p got intr\n", vu_group);  /* [한국어] doorbell 디버그 컴포넌트 로그. */

	ret |= nvmf_vfio_user_poll_group_poll(&vu_group->group);  /* [한국어] 이 PG 의 SQ 들 폴링. */

	/*
	 * Re-arm the event indexes. NB: this also could rearm other
	 * controller's SQs.
	 */
	/* [한국어] eventidx 재무장 — 다음 doorbell write 가 다시 wakeup 을 트리거하도록.
	 * 다른 controller 의 SQ 도 같이 rearm 될 수 있음(공유 wakeup 특성). */
	ret |= vfio_user_poll_group_rearm(vu_group);

	vu_group->stats.pg_process_count++;  /* [한국어] PG process 횟수 통계 (RPC dump_stat 노출). */
	return ret != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;  /* [한국어] 한 일이 있으면 BUSY. */
}

/*
 * [한국어]
 * vfio_user_poll_group_intr - PG wakeup eventfd readable 시 호출되는 interrupt 콜백.
 *
 * @ctx: nvmf_vfio_user_poll_group.
 * @return: vfio_user_poll_group_process 의 BUSY/IDLE.
 *
 * 동기/배경: add_intr 가 등록한 콜백. 다른 reactor 가 이 PG 를 깨우려 intr_fd 에 write 하면
 *   reactor epoll 이 이 콜백을 호출한다. eventfd 누적 카운터를 먼저 회수(level→edge 정리)한
 *   뒤 실제 처리(process)로 위임한다.
 * 동작 단계:
 *   1) eventfd_read 로 누적 카운터 소비(읽지 않으면 계속 readable 로 남아 busy-loop).
 *   2) intr 통계 증가.
 *   3) vfio_user_poll_group_process 로 실제 poll+rearm 수행.
 * 실행 컨텍스트: PG 소유 reactor 의 epoll 디스패치.
 * 호출자: SPDK interrupt 프레임워크(intr_fd readable).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   reactor epoll(intr_fd) → [vfio_user_poll_group_intr]
 *     → eventfd_read → vfio_user_poll_group_process
 */
static int
vfio_user_poll_group_intr(void *ctx)
{
	struct nvmf_vfio_user_poll_group *vu_group = ctx;  /* [한국어] 깨어난 PG. */
	eventfd_t val;  /* [한국어] eventfd 누적 카운터를 받을 버퍼 (값 자체는 무의미). */

	eventfd_read(vu_group->intr_fd, &val);  /* [한국어] 누적값 소비 — 안 읽으면 fd 가 계속 readable 로 남음. */

	vu_group->stats.intr++;  /* [한국어] interrupt 수신 횟수 통계. */

	return vfio_user_poll_group_process(ctx);  /* [한국어] 실제 poll+rearm 으로 위임. */
}

/*
 * Handle an interrupt for the given controller: we must poll the vfu_ctx, and
 * the SQs assigned to our own poll group. Other poll groups are handled via
 * vfio_user_poll_group_intr().
 */
/*
 * [한국어]
 * vfio_user_ctrlr_intr - controller 단위 interrupt 핸들러 (interrupt mode 활성 시).
 *
 * @ctx: nvmf_vfio_user_ctrlr.
 * @return: SPDK_POLLER_BUSY/IDLE.
 *
 * vfio-user fd 가 readable 해지면 (= VM 이 BAR write 등 트리거) SPDK reactor 가 본
 * 핸들러 호출. polled mode 와의 차이: vfu_run_ctx 를 매 reactor iter 마다 부르지 않고
 * 이벤트 발생 시만 호출 → idle CPU 사용량 감소.
 *
 * 단계:
 *   1. vfio_user_poll_vfu_ctx — libvfio-user 의 모든 pending 이벤트 처리 (BAR access,
 *      DMA map/unmap, IRQ trigger). doorbell write 가 여기서 일어남 → SQ 에 신호.
 *   2. sqs[0] NULL 검사 — vfu_ctx poll 안에서 ctrlr destroy 트리거됐을 수 있음 (admin SQ
 *      free) → 안전 검사.
 *   3. intr_mode_sq_spreading 활성 + SQ 가 다른 reactor 의 PG 에 있으면 그 PG 의 intr_fd
 *      에 eventfd_write — "doorbell 업데이트했으니 와서 SQ 폴링해" 신호. 도어벨 write 가
 *      reactor 경계를 넘는 case.
 *   4. 본 PG 의 SQ 들 처리 (vfio_user_poll_group_process).
 *
 * doorbell 후 즉시 SQ poll 의 중요성: VM 이 SQE 채우고 doorbell write 한 직후 즉시
 * 폴링하지 않으면 latency 증가. interrupt 가 발생한 reactor 에서 즉시 처리.
 *
 * 호출 체인:
 *   VM BAR write → libvfio-user fd ready → SPDK reactor intr → [본 함수]
 *     → vfu_run_ctx (doorbell write handler)
 *     → poll_group_process (SQE fetch + bdev exec)
 */
static int
vfio_user_ctrlr_intr(void *ctx)
{
	struct nvmf_vfio_user_poll_group *vu_ctrlr_group;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = ctx;
	struct nvmf_vfio_user_poll_group *vu_group;
	int ret = SPDK_POLLER_IDLE;

	vu_ctrlr_group = ctrlr_to_poll_group(vu_ctrlr);

	SPDK_DEBUGLOG(vfio_user_db, "ctrlr pg:%p got intr\n", vu_ctrlr_group);

	vu_ctrlr_group->stats.ctrlr_intr++;

	/*
	 * Poll vfio-user for this controller. We need to do this before polling
	 * any SQs, as this is where doorbell writes may be handled.
	 */
	/* [한국어] vfu_ctx 처리 우선 — doorbell write 가 여기서 처리되어 SQ tail 이 갱신돼야
	 * 다음 SQ poll 이 새 SQE 를 본다. 순서 바뀌면 1 iteration 의 latency 추가. */
	ret = vfio_user_poll_vfu_ctx(vu_ctrlr);

	/*
	 * `sqs[0]` could be set to NULL in vfio_user_poll_vfu_ctx() context,
	 * just return for this case.
	 */
	/* [한국어] vfu_ctx poll 중에 admin SQ destroy 일어났으면 sqs[0]=NULL — 더 진행 불가.
	 * (예: VM 이 controller disable 명령 보냈을 때). */
	if (vu_ctrlr->sqs[0] == NULL) {
		return ret;
	}

	/* [한국어] SQ spreading 모드: doorbell 가 다른 reactor 의 PG SQ 에 영향 줬을 수 있음 →
	 * 그 PG 에 eventfd_write 로 wake-up 신호. doorbell 가 cross-reactor 영향 주는 경우의
	 * latency 최소화. */
	if (vu_ctrlr->transport->transport_opts.enable_intr_mode_sq_spreading) {
		/*
		 * We may have just written to a doorbell owned by another
		 * reactor: we need to prod them to make sure its SQs are polled
		 * *after* the doorbell value is updated.
		 */
		TAILQ_FOREACH(vu_group, &vu_ctrlr->transport->poll_groups, link) {
			if (vu_group != vu_ctrlr_group) {
				SPDK_DEBUGLOG(vfio_user_db, "prodding pg:%p\n", vu_group);
				eventfd_write(vu_group->intr_fd, 1);
			}
		}
	}

	/* [한국어] 본 PG 의 SQ 들 폴링 — bar 처리 후 새로 enqueue 된 SQE 처리. */
	ret |= vfio_user_poll_group_process(vu_ctrlr_group);

	return ret;
}

/*
 * [한국어]
 * vfio_user_ctrlr_set_intr_mode - SPDK poller 의 interrupt mode 전환 콜백.
 *
 * @poller: spdk_poller (등록 시 매핑).
 * @ctx: ctrlr.
 * @interrupt_mode: true = interrupt mode, false = polled mode.
 *
 * SPDK reactor 가 idle 상태 진입/탈출 시 호출. polled mode 는 매 iter polling (CPU 100%),
 * interrupt mode 는 fd readable 시만 호출 (CPU 절약, latency 약간 증가).
 *
 * endpoint->interrupt_mode 저장 이유: controller reset 후 새 ctrlr 객체 생성 시에도
 * 같은 mode 유지 — VM disconnect/reconnect 사이클에서 mode 안정성 보장.
 *
 * vfio_user_poll_group_rearm: shadow doorbell eventidx 재설정 (polled → interrupt 전환 시
 * 호스트 doorbell write 가 통지로 깨워야 함).
 *
 * 호출 체인:
 *   SPDK reactor idle 감지 → set_intr_mode → [본 함수] → endpoint mode 저장 + rearm
 */
static void
vfio_user_ctrlr_set_intr_mode(struct spdk_poller *poller, void *ctx,
			      bool interrupt_mode)
{
	struct nvmf_vfio_user_ctrlr *ctrlr = ctx;
	assert(ctrlr != NULL);
	assert(ctrlr->endpoint != NULL);

	SPDK_DEBUGLOG(nvmf_vfio, "%s: setting interrupt mode to %d\n",
		      ctrlr_id(ctrlr), interrupt_mode);

	/*
	 * interrupt_mode needs to persist across controller resets, so store
	 * it in the endpoint instead.
	 */
	/* [한국어] endpoint 에 저장 — ctrlr reset 후 새 ctrlr 가 같은 mode 상속. */
	ctrlr->endpoint->interrupt_mode = interrupt_mode;

	/* [한국어] shadow doorbell eventidx 재설정 — polled→interrupt 전환 시 wake-up 회복. */
	vfio_user_poll_group_rearm(ctrlr_to_poll_group(ctrlr));
}

/*
 * In response to the nvmf_vfio_user_create_ctrlr() path, the admin queue is now
 * set up and we can start operating on this controller.
 */
/*
 * [한국어]
 * start_ctrlr - admin queue 연결 완료 후 controller 를 RUNNING 상태로 띄우고 poller 등록.
 *
 * @vu_ctrlr: 시작할 vfio-user controller.
 * @ctrlr: 코어가 만든 generic spdk_nvmf_ctrlr (cntlid 등 보유).
 *
 * 동기/배경: admin SQ0/CQ0 connect 가 성공(handle_queue_connect_rsp)하면 비로소 이
 *   controller 로 명령 처리를 시작할 수 있다. 여기서 vfu_ctx_poller 를 등록해 libvfio-user
 *   이벤트(BAR/DMA/doorbell)를 주기적으로 처리하게 만든다. polled vs interrupt 모드에 따라
 *   poller 주기와 wakeup fd 등록이 갈린다.
 * 동작 단계:
 *   1) generic ctrlr 연결, cntlid 복사, 소유 thread 고정(이후 모든 처리가 이 thread).
 *   2) state=RUNNING.
 *   3) polled 모드: 1000us 주기 poller 등록 후 반환(인터럽트 fd 불필요).
 *   4) interrupt 모드: 주기 0 poller + vfu_get_poll_fd 기반 spdk_interrupt 등록 +
 *      set_intr_mode 콜백 연결(poll↔interrupt 전환 시 eventidx rearm).
 * 실행 컨텍스트: admin CQ 소유 thread = spdk_get_thread() — 이후 controller 의 affinity 기준.
 * 호출자: handle_queue_connect_rsp (admin queue connect 성공 시).
 * 에러 경로: 자원 실패는 assert (poller/interrupt 등록 실패는 치명적).
 *
 * 호출 체인:
 *   handle_queue_connect_rsp → [start_ctrlr]
 *     → SPDK_POLLER_REGISTER(vfio_user_poll_vfu_ctx) / SPDK_INTERRUPT_REGISTER
 */
static void
start_ctrlr(struct nvmf_vfio_user_ctrlr *vu_ctrlr,
	    struct spdk_nvmf_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_endpoint *endpoint = vu_ctrlr->endpoint;  /* [한국어] vfu_ctx/소켓 보유 endpoint. */

	vu_ctrlr->ctrlr = ctrlr;                    /* [한국어] generic controller 연결(Identify/Feature 처리 위임 대상). */
	vu_ctrlr->cntlid = ctrlr->cntlid;            /* [한국어] 코어가 할당한 controller ID 보관. */
	vu_ctrlr->thread = spdk_get_thread();        /* [한국어] 이 controller 의 affinity thread 고정. */
	vu_ctrlr->state = VFIO_USER_CTRLR_RUNNING;    /* [한국어] 상태 전이 — 이제 명령 처리 가능. */

	/* [한국어] polled 모드: 1000us 주기 poller 만 등록하고 끝 (fd wakeup 없음). */
	if (!in_interrupt_mode(endpoint->transport)) {
		vu_ctrlr->vfu_ctx_poller = SPDK_POLLER_REGISTER(vfio_user_poll_vfu_ctx,
					   vu_ctrlr, 1000);
		return;
	}

	/* [한국어] interrupt 모드: 주기 0 poller(평소엔 안 돌고 fd 깨어남에 의존). */
	vu_ctrlr->vfu_ctx_poller = SPDK_POLLER_REGISTER(vfio_user_poll_vfu_ctx,
				   vu_ctrlr, 0);

	/* [한국어] libvfio-user 의 epoll fd 를 얻어 doorbell/메시지 도착 시 reactor 를 깨우게 함. */
	vu_ctrlr->intr_fd = vfu_get_poll_fd(vu_ctrlr->endpoint->vfu_ctx);
	assert(vu_ctrlr->intr_fd != -1);  /* [한국어] poll fd 획득 실패는 치명적. */

	/* [한국어] 그 fd 를 reactor epoll set 에 등록 — readable 시 vfio_user_ctrlr_intr 실행. */
	vu_ctrlr->intr = SPDK_INTERRUPT_REGISTER(vu_ctrlr->intr_fd,
			 vfio_user_ctrlr_intr, vu_ctrlr);

	assert(vu_ctrlr->intr != NULL);  /* [한국어] interrupt 등록 실패도 치명적. */

	/* [한국어] poller 의 poll↔interrupt 전환 시 호출될 콜백 연결 — 전환 때 eventidx rearm. */
	spdk_poller_register_interrupt(vu_ctrlr->vfu_ctx_poller,
				       vfio_user_ctrlr_set_intr_mode,
				       vu_ctrlr);
}

/*
 * [한국어]
 * handle_queue_connect_rsp - 내부 Fabric Connect 명령 완료 콜백: qpair 를 ACTIVE 로 전이.
 *
 * @req: 완료된 vfio-user 내부 요청 (Connect 명령 응답 보유).
 * @cb_arg: 연결 중이던 SQ (poll_group_add 에서 cb_arg 로 설정).
 * @return: 0 성공, 음수 실패(연결 거부/메모리 부족).
 *
 * 동기/배경: nvmf_vfio_user_poll_group_add 는 qpair 를 코어에 등록하기 위해 내부적으로
 *   Fabric Connect 명령을 발행한다. 그 완료가 여기로 돌아온다. admin qpair 인지 I/O qpair
 *   인지에 따라 후처리가 다르다:
 *     - admin: SQ0/CQ0 가 한 쌍으로 만들어지므로 admin CQ ref 를 세우고 start_ctrlr 로
 *       controller 를 RUNNING 시킨다.
 *     - I/O: 이 connect 는 host 의 Create IO SQ 명령에 대한 응답으로 생성된 것이므로, 그
 *       Create IO SQ CQE 를 (필요하면 cross-thread 메시지로) host admin CQ 에 게시하고 SQ 를
 *       ACTIVE 로 전이한다. live-migration window 대비로 interrupt 모드면 ctrlr_kick.
 * 동작 단계: 응답 에러 검사 → SQ 를 PG sqs 리스트에 등록 → admin/IO 분기 후처리 →
 *   connected_sqs 에 추가 → connect data 버퍼 해제.
 * 실행 컨텍스트: SQ 가 배정된 PG 의 thread. endpoint->lock 으로 ctrlr 상태/connected_sqs 보호.
 * 호출자: nvmf_vfio_user_req_complete (내부 Connect 완료 시 cb_fn 으로).
 * 에러 경로: Connect 실패면 free_ctrlr 로 controller 폐기 후 -1; cpl_ctx 할당 실패 -ENOMEM.
 *
 * 호출 체인:
 *   (내부 Connect 완료) nvmf_vfio_user_req_complete → [handle_queue_connect_rsp]
 *     → start_ctrlr (admin) 또는 post_completion/_post_completion_msg (I/O)
 */
static int
handle_queue_connect_rsp(struct nvmf_vfio_user_req *req, void *cb_arg)
{
	struct nvmf_vfio_user_poll_group *vu_group;       /* [한국어] SQ 가 속한 PG. */
	struct nvmf_vfio_user_sq *sq = cb_arg;            /* [한국어] connect 중이던 SQ. */
	struct nvmf_vfio_user_cq *admin_cq;               /* [한국어] CQE 게시 대상 admin CQ[0]. */
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;            /* [한국어] SQ 의 controller. */
	struct nvmf_vfio_user_endpoint *endpoint;         /* [한국어] controller 의 endpoint. */

	assert(sq != NULL);  /* [한국어] cb_arg 는 항상 유효 SQ. */
	assert(req != NULL);  /* [한국어] 완료된 요청 객체. */

	vu_ctrlr = sq->ctrlr;          /* [한국어] SQ → controller. */
	assert(vu_ctrlr != NULL);
	endpoint = vu_ctrlr->endpoint; /* [한국어] controller → endpoint. */
	assert(endpoint != NULL);

	/* [한국어] Connect 가 에러 상태로 완료됐으면 controller 전체를 폐기(연결 불가). */
	if (spdk_nvme_cpl_is_error(&req->req.rsp->nvme_cpl)) {
		SPDK_ERRLOG("SC %u, SCT %u\n", req->req.rsp->nvme_cpl.status.sc, req->req.rsp->nvme_cpl.status.sct);  /* [한국어] NVMe status code/type 로깅. */
		endpoint->ctrlr = NULL;   /* [한국어] endpoint 가 ctrlr 참조 끊음. */
		free_ctrlr(vu_ctrlr);     /* [한국어] controller 자원 해제. */
		return -1;                 /* [한국어] 호출자(req_complete)가 fail_ctrlr 로 처리. */
	}

	vu_group = SPDK_CONTAINEROF(sq->group, struct nvmf_vfio_user_poll_group, group);  /* [한국어] SQ 의 PG 복원. */
	TAILQ_INSERT_TAIL(&vu_group->sqs, sq, link);  /* [한국어] 이 SQ 를 PG 의 폴링 리스트에 등록 → hot path 진입. */

	admin_cq = vu_ctrlr->cqs[0];  /* [한국어] admin CQ[0] — 모든 admin/IO-create 완료의 게시처. */
	assert(admin_cq != NULL);
	assert(admin_cq->group != NULL);  /* [한국어] admin CQ 는 PG 에 배정돼 있어야 함. */
	assert(admin_cq->group->group->thread != NULL);  /* [한국어] PG 소유 thread 유효성. */

	pthread_mutex_lock(&endpoint->lock);  /* [한국어] ctrlr 상태/connected_sqs 보호. */
	/* [한국어] admin qpair 인 경우 — SQ0/CQ0 쌍 처리 + controller 시작. */
	if (nvmf_qpair_is_admin_queue(&sq->qpair)) {
		assert(admin_cq->group->group->thread == spdk_get_thread());  /* [한국어] admin 은 자기 thread 에서. */
		/*
		 * The admin queue is special as SQ0 and CQ0 are created
		 * together.
		 */
		admin_cq->cq_ref = 1;  /* [한국어] admin CQ 참조수 1 (SQ0 가 사용). */
		start_ctrlr(vu_ctrlr, sq->qpair.ctrlr);  /* [한국어] poller 등록 + state=RUNNING. */
	} else {
		/* For I/O queues this command was generated in response to an
		 * ADMIN I/O CREATE SUBMISSION QUEUE command which has not yet
		 * been completed. Complete it now.
		 */
		/* [한국어] I/O qpair: 이 내부 Connect 는 host 의 Create IO SQ admin 명령에서 유발됨.
		 * 그 admin 명령의 CQE 를 이제 host 에게 게시해야 한다. */
		if (sq->post_create_io_sq_completion) {
			/* [한국어] admin CQ 소유 thread 가 다른 reactor 면 cross-thread 게시 필요. */
			if (admin_cq->group->group->thread != spdk_get_thread()) {
				struct vfio_user_post_cpl_ctx *cpl_ctx;  /* [한국어] 메시지로 넘길 CQE 컨텍스트. */

				cpl_ctx = calloc(1, sizeof(*cpl_ctx));  /* [한국어] 힙에 컨텍스트 할당(소유권 메시지로 이동). */
				if (!cpl_ctx) {
					return -ENOMEM;  /* [한국어] 할당 실패 — 락 보유 상태 주의(상위에서 처리). */
				}
				cpl_ctx->ctrlr = vu_ctrlr;                       /* [한국어] 게시 대상 controller. */
				cpl_ctx->cq = admin_cq;                          /* [한국어] 게시 대상 admin CQ. */
				cpl_ctx->cpl.sqid = 0;                           /* [한국어] admin SQ id=0. */
				cpl_ctx->cpl.cdw0 = 0;                           /* [한국어] 결과 dword0 = 0. */
				cpl_ctx->cpl.cid = sq->create_io_sq_cmd.cid;     /* [한국어] 원래 Create IO SQ 명령의 cid. */
				cpl_ctx->cpl.status.sc = SPDK_NVME_SC_SUCCESS;   /* [한국어] 성공 status code. */
				cpl_ctx->cpl.status.sct = SPDK_NVME_SCT_GENERIC; /* [한국어] generic status type. */

				/* [한국어] admin CQ 소유 thread 에 게시를 위임(그 thread 만 head/doorbell 안전 갱신). */
				spdk_thread_send_msg(admin_cq->group->group->thread,
						     _post_completion_msg,
						     cpl_ctx);
			} else {
				/* [한국어] 같은 thread 면 직접 게시. */
				post_completion(vu_ctrlr, admin_cq, 0, 0,
						sq->create_io_sq_cmd.cid, SPDK_NVME_SC_SUCCESS, SPDK_NVME_SCT_GENERIC);
			}
			sq->post_create_io_sq_completion = false;  /* [한국어] 게시 완료 — 플래그 클리어. */
		} else if (in_interrupt_mode(endpoint->transport)) {
			/*
			 * If we're live migrating a guest, there is a window
			 * where the I/O queues haven't been set up but the
			 * device is in running state, during which the guest
			 * might write to a doorbell. This doorbell write will
			 * go unnoticed, so let's poll the whole controller to
			 * pick that up.
			 */
			/* [한국어] live-migration 직후 IO queue 미설정 window 에 guest 가 doorbell 을
			 * 썼을 수 있다 — interrupt 모드라 놓칠 수 있으므로 controller 를 강제로 깨워 폴링. */
			ctrlr_kick(vu_ctrlr);
		}
		sq->sq_state = VFIO_USER_SQ_ACTIVE;  /* [한국어] I/O SQ 를 ACTIVE 로 전이 → 이제 SQE 처리 자격. */
	}

	TAILQ_INSERT_TAIL(&vu_ctrlr->connected_sqs, sq, tailq);  /* [한국어] controller 의 연결 SQ 목록에 추가(정리 시 순회). */
	pthread_mutex_unlock(&endpoint->lock);  /* [한국어] 상태 갱신 완료 — 락 해제. */

	free(req->req.iov[0].iov_base);  /* [한국어] connect data(calloc 했던 fabric_connect_data) 해제. */
	req->req.iov[0].iov_base = NULL;  /* [한국어] dangling 방지. */
	req->req.iovcnt = 0;              /* [한국어] iov 사용 종료 표시. */

	return 0;  /* [한국어] 연결 성공. */
}

/*
 * [한국어]
 * _nvmf_vfio_user_poll_group_add - 지연된 내부 Connect 요청을 실제로 실행하는 메시지 콜백.
 *
 * @req: 실행할 spdk_nvmf_request (내부 Fabric Connect).
 *
 * 동기/배경: poll_group_add 콜백이 실행되는 시점에는 qpair 가 아직 ACTIVE 상태가 아니라
 *   spdk_nvmf_request_exec 를 바로 부르면 실패한다. 콜백이 끝난 직후 ACTIVE 로 전이되므로,
 *   request_exec 를 메시지로 한 박자 미뤄 ACTIVE 가 된 뒤 실행되게 한다.
 * 동작: spdk_nvmf_request_exec 로 내부 Connect 명령 디스패치.
 * 실행 컨텍스트: qpair 가 배정된 PG 의 thread (메시지 핸들러).
 * 호출자: spdk_thread_send_msg (nvmf_vfio_user_poll_group_add 가 큐잉).
 * 에러 경로: 없음 (request_exec 내부에서 처리).
 *
 * 호출 체인:
 *   spdk_thread_send_msg → [_nvmf_vfio_user_poll_group_add] → spdk_nvmf_request_exec
 */
static void
_nvmf_vfio_user_poll_group_add(void *req)
{
	spdk_nvmf_request_exec(req);  /* [한국어] ACTIVE 전이 후 내부 Connect 실행 (지연 디스패치). */
}

/*
 * Add the given qpair to the given poll group. New qpairs are added via
 * spdk_nvmf_tgt_new_qpair(), which picks a poll group via
 * nvmf_vfio_user_get_optimal_poll_group(), then calls back here via
 * nvmf_transport_poll_group_add().
 */
/*
 * [한국어]
 * nvmf_vfio_user_poll_group_add - qpair 를 PG 에 추가하고 내부 Connect 명령 발행 (.poll_group_add).
 *
 * @group: get_optimal_poll_group 이 고른 generic poll group.
 * @qpair: 추가할 qpair (CONTAINEROF 로 SQ 복원).
 * @return: 0 성공, 음수(요청 풀 고갈/메모리 부족) 실패.
 *
 * 동기/배경: 코어가 새 qpair 를 PG 에 붙일 때 호출한다. vfio-user 는 capsule transport 가
 *   없으므로, host 가 보낸 Connect 대신 transport 가 "대리" Fabric Connect 명령을 만들어
 *   코어 nvmf 상태머신(controller/qpair 연결)을 진행시킨다. 완료는 handle_queue_connect_rsp.
 * 동작 단계:
 *   1) SQ 에 group 저장.
 *   2) free_reqs 풀에서 내부 요청 1개 빌림.
 *   3) Fabric Connect SQE 필드(opcode/qid/sqsize 등) 작성.
 *   4) fabric_connect_data(cntlid/subnqn) 버퍼 calloc + iov 설정.
 *   5) 완료 콜백 handle_queue_connect_rsp 등록.
 *   6) request_exec 를 메시지로 지연(ACTIVE 전이 대기).
 * 실행 컨텍스트: PG thread. 이 시점 qpair 는 아직 ACTIVE 아님(그래서 6단계가 메시지).
 * 호출자: nvmf_transport_poll_group_add → vtable .poll_group_add.
 * 에러 경로: 요청 풀 고갈 -1; data 할당 실패 시 req 반환 후 -ENOMEM.
 *
 * 호출 체인:
 *   spdk_nvmf_tgt_new_qpair → nvmf_transport_poll_group_add → [본 함수]
 *     → spdk_thread_send_msg(_nvmf_vfio_user_poll_group_add) → spdk_nvmf_request_exec
 */
static int
nvmf_vfio_user_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_vfio_user_sq *sq;                     /* [한국어] qpair 가 속한 SQ. */
	struct nvmf_vfio_user_req *vu_req;                /* [한국어] 내부 Connect 용으로 빌릴 요청. */
	struct nvmf_vfio_user_ctrlr *ctrlr;               /* [한국어] SQ 의 controller. */
	struct spdk_nvmf_request *req;                    /* [한국어] vu_req 안의 generic 요청. */
	struct spdk_nvmf_fabric_connect_data *data;       /* [한국어] Connect data(cntlid/subnqn) 버퍼. */
	bool admin;                                        /* [한국어] admin qpair 여부(qid 결정에 사용). */

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);  /* [한국어] qpair → SQ 복원. */
	sq->group = group;  /* [한국어] SQ 에 배정된 PG 기록(완료 게시 thread 판정에 사용). */
	ctrlr = sq->ctrlr;   /* [한국어] SQ 의 controller. */

	SPDK_DEBUGLOG(nvmf_vfio, "%s: add QP%d=%p(%p) to poll_group=%p\n",
		      ctrlr_id(ctrlr), sq->qpair.qid,
		      sq, qpair, group);

	admin = nvmf_qpair_is_admin_queue(&sq->qpair);  /* [한국어] admin(qid 0)이면 connect qid=0. */

	vu_req = get_nvmf_vfio_user_req(sq);  /* [한국어] free_reqs 풀에서 내부 요청 1개 빌림. */
	if (vu_req == NULL) {
		return -1;  /* [한국어] 풀 고갈 — 연결 진행 불가. */
	}

	req = &vu_req->req;  /* [한국어] 내부 요청의 generic 부분. */
	req->cmd->connect_cmd.opcode = SPDK_NVME_OPC_FABRIC;             /* [한국어] Fabric 명령 opcode (0x7F). */
	req->cmd->connect_cmd.cid = 0;                                   /* [한국어] command id 0(내부 명령). */
	req->cmd->connect_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT; /* [한국어] Fabric subtype = Connect. */
	req->cmd->connect_cmd.recfmt = 0;                               /* [한국어] record format 0(현재 유일 정의). */
	req->cmd->connect_cmd.sqsize = sq->size - 1;                     /* [한국어] SQ 크기(0-based, NVMe-oF Connect). */
	req->cmd->connect_cmd.qid = admin ? 0 : qpair->qid;             /* [한국어] admin=0, IO=실제 qid. */

	req->length = sizeof(struct spdk_nvmf_fabric_connect_data);  /* [한국어] Connect data 페이로드 크기. */

	data = calloc(1, req->length);  /* [한국어] Connect data 버퍼 zero-alloc. */
	if (data == NULL) {
		nvmf_vfio_user_req_free(req);  /* [한국어] 빌린 요청 반환. */
		return -ENOMEM;                 /* [한국어] 메모리 부족. */
	}

	SPDK_IOV_ONE(req->iov, &req->iovcnt, data, req->length);  /* [한국어] 단일 iov 로 data 버퍼 설정. */

	data->cntlid = ctrlr->cntlid;  /* [한국어] 이 controller 의 cntlid 를 Connect data 에 명시. */
	/* [한국어] 대상 subsystem 의 NQN 을 data 에 복사(어느 subsystem 에 붙는지). */
	snprintf(data->subnqn, sizeof(data->subnqn), "%s",
		 spdk_nvmf_subsystem_get_nqn(ctrlr->endpoint->subsystem));

	vu_req->cb_fn = handle_queue_connect_rsp;  /* [한국어] 완료 시 qpair ACTIVE 전이 콜백. */
	vu_req->cb_arg = sq;                        /* [한국어] 콜백에 SQ 전달. */

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: sending connect fabrics command for qid:%#x cntlid=%#x\n",
		      ctrlr_id(ctrlr), qpair->qid, data->cntlid);

	/*
	 * By the time transport's poll_group_add() callback is executed, the
	 * qpair isn't in the ACTIVE state yet, so spdk_nvmf_request_exec()
	 * would fail.  The state changes to ACTIVE immediately after the
	 * callback finishes, so delay spdk_nvmf_request_exec() by sending a
	 * message.
	 */
	/* [한국어] 콜백 실행 시점엔 qpair 가 아직 ACTIVE 아님 → request_exec 즉시 호출 시 실패.
	 * 콜백 종료 직후 ACTIVE 가 되므로 메시지로 한 박자 미뤄 실행되게 한다. */
	spdk_thread_send_msg(spdk_get_thread(), _nvmf_vfio_user_poll_group_add, req);
	return 0;  /* [한국어] 등록 성공(실제 connect 는 다음 라운드). */
}

/*
 * [한국어]
 * nvmf_vfio_user_poll_group_remove - qpair(SQ)를 PG 의 폴링 리스트에서 제거 (.poll_group_remove).
 *
 * @group: SQ 가 속한 generic poll group.
 * @qpair: 제거할 qpair (CONTAINEROF 로 SQ 복원).
 * @return: 항상 0.
 *
 * 동기/배경: qpair 가 disconnect 되어 더는 폴링하면 안 될 때 코어가 호출한다. PG 의 sqs
 *   리스트에서 이 SQ 를 떼어내면 다음 poll_group_poll 부터 이 SQ 를 건너뛴다.
 * 동작: SQ 복원 → PG 복원 → sqs 리스트에서 TAILQ_REMOVE.
 * 실행 컨텍스트: PG 소유 thread (코어가 같은 thread 에서 호출 보장).
 * 호출자: nvmf_transport_poll_group_remove → vtable .poll_group_remove.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   spdk_nvmf_poll_group ... → [본 함수] → TAILQ_REMOVE
 */
static int
nvmf_vfio_user_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				 struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_vfio_user_sq *sq;                  /* [한국어] 제거할 SQ. */
	struct nvmf_vfio_user_poll_group *vu_group;    /* [한국어] SQ 가 속한 PG. */

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);  /* [한국어] qpair → SQ 복원. */

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: remove NVMf QP%d=%p from NVMf poll_group=%p\n",
		      ctrlr_id(sq->ctrlr), qpair->qid, qpair, group);


	vu_group = SPDK_CONTAINEROF(group, struct nvmf_vfio_user_poll_group, group);  /* [한국어] generic → vfio PG 복원. */
	TAILQ_REMOVE(&vu_group->sqs, sq, link);  /* [한국어] 폴링 리스트에서 제거 → 다음 poll 부터 제외. */

	return 0;
}

/*
 * [한국어]
 * _nvmf_vfio_user_req_free - 요청 객체를 초기화해 SQ 의 free_reqs 풀로 반환(내부 헬퍼).
 *
 * @sq: 요청이 속한 SQ (free_reqs 풀 소유).
 * @vu_req: 반환할 요청.
 *
 * 동기/배경: vfio-user 는 SQ 당 요청 객체를 미리 할당해 free_reqs 풀로 재활용한다(할당
 *   비용 0, lockless). 완료/실패 후 요청을 깨끗이 리셋해 다음 사용 시 잔여 상태(cmd/rsp/
 *   iov/flags/cb)가 남지 않도록 한다.
 * 동작 단계: cmd/rsp memset → iovcnt/length 0 → raw=0(모든 플래그 클리어) → cmd_cb_fn NULL →
 *   state=FREE → free_reqs 끝에 삽입.
 * 실행 컨텍스트: SQ 소유 thread (요청은 단일 thread 에서만 다뤄져 락 불필요).
 * 호출자: nvmf_vfio_user_req_free / nvmf_vfio_user_req_complete.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   nvmf_vfio_user_req_free / _complete → [_nvmf_vfio_user_req_free] → TAILQ_INSERT(free_reqs)
 */
static void
_nvmf_vfio_user_req_free(struct nvmf_vfio_user_sq *sq, struct nvmf_vfio_user_req *vu_req)
{
	memset(&vu_req->cmd, 0, sizeof(vu_req->cmd));  /* [한국어] 이전 SQE 잔여 제거. */
	memset(&vu_req->rsp, 0, sizeof(vu_req->rsp));  /* [한국어] 이전 CQE 잔여 제거. */
	vu_req->iovcnt = 0;          /* [한국어] vfio-user 측 iov 개수 리셋. */
	vu_req->req.iovcnt = 0;      /* [한국어] generic req iov 개수 리셋. */
	vu_req->req.length = 0;      /* [한국어] 전송 길이 리셋. */
	vu_req->req.raw = 0; /* clear all flags */  /* [한국어] req 플래그 비트필드 전체 0 으로. */
	vu_req->req.cmd_cb_fn = NULL;  /* [한국어] 이전 명령 콜백 제거. */
	vu_req->state = VFIO_USER_REQUEST_STATE_FREE;  /* [한국어] 풀로 돌아감 표시. */

	TAILQ_INSERT_TAIL(&sq->free_reqs, vu_req, link);  /* [한국어] 재사용 풀에 반환(LIFO 아닌 tail). */
}

/*
 * [한국어]
 * nvmf_vfio_user_req_free - 코어가 요청을 풀로 돌려보낼 때의 콜백 (.req_free).
 *
 * @req: 반환할 generic 요청 (CONTAINEROF 로 vu_req/SQ 복원).
 *
 * 동기/배경: 코어가 더는 필요 없는 요청(예: connect data 할당 실패 롤백)을 transport 에게
 *   반환할 때 호출한다. 내부 헬퍼로 위임해 풀에 되돌린다.
 * 동작: vu_req/SQ 복원 후 _nvmf_vfio_user_req_free 호출.
 * 실행 컨텍스트: SQ 소유 thread.
 * 호출자: 코어/내부 롤백 경로 → vtable .req_free.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   (코어) → [nvmf_vfio_user_req_free] → _nvmf_vfio_user_req_free
 */
static void
nvmf_vfio_user_req_free(struct spdk_nvmf_request *req)
{
	struct nvmf_vfio_user_sq *sq;            /* [한국어] 요청이 속한 SQ. */
	struct nvmf_vfio_user_req *vu_req;        /* [한국어] generic req 를 감싼 vfio 요청. */

	assert(req != NULL);

	vu_req = SPDK_CONTAINEROF(req, struct nvmf_vfio_user_req, req);  /* [한국어] req → vu_req 복원. */
	sq = SPDK_CONTAINEROF(req->qpair, struct nvmf_vfio_user_sq, qpair);  /* [한국어] qpair → SQ 복원. */

	_nvmf_vfio_user_req_free(sq, vu_req);  /* [한국어] 풀로 반환. */
}

/*
 * [한국어]
 * nvmf_vfio_user_req_complete - 요청 완료 콜백: cb_fn 실행 후 풀로 반환 (.req_complete).
 *
 * @req: 완료된 generic 요청 (CONTAINEROF 로 vu_req/SQ 복원).
 *
 * 동기/배경: bdev/admin 처리가 끝나 코어가 완료를 알릴 때 호출된다. 내부 요청에 등록된
 *   cb_fn(예: handle_queue_connect_rsp, handle_cmd_rsp)을 실행해 후처리(CQE 게시 등)를 하고,
 *   그 콜백이 0 이 아니면(치명적 실패) controller 를 fail 시킨다. 마지막으로 요청을 풀에 반환.
 * 동작 단계: vu_req/SQ 복원 → cb_fn 있으면 실행 → 실패 시 fail_ctrlr → 풀 반환.
 * 실행 컨텍스트: SQ 소유 thread (완료 콜백이 이 thread 에서 실행됨).
 * 호출자: spdk_nvmf_request_complete → vtable .req_complete.
 * 에러 경로: cb_fn 반환 비0 → fail_ctrlr 로 controller fatal.
 *
 * 호출 체인:
 *   spdk_nvmf_request_complete → [nvmf_vfio_user_req_complete]
 *     → vu_req->cb_fn (handle_cmd_rsp 등) → _nvmf_vfio_user_req_free
 */
static void
nvmf_vfio_user_req_complete(struct spdk_nvmf_request *req)
{
	struct nvmf_vfio_user_sq *sq;            /* [한국어] 요청이 속한 SQ. */
	struct nvmf_vfio_user_req *vu_req;        /* [한국어] generic req 를 감싼 vfio 요청. */

	assert(req != NULL);

	vu_req = SPDK_CONTAINEROF(req, struct nvmf_vfio_user_req, req);  /* [한국어] req → vu_req 복원. */
	sq = SPDK_CONTAINEROF(req->qpair, struct nvmf_vfio_user_sq, qpair);  /* [한국어] qpair → SQ 복원. */

	/* [한국어] 등록된 완료 콜백이 있으면 실행(CQE 게시/연결 후처리 등). */
	if (vu_req->cb_fn != NULL) {
		/* [한국어] 콜백이 비0 반환 = 치명적 오류 → controller 를 fail 상태로. */
		if (vu_req->cb_fn(vu_req, vu_req->cb_arg) != 0) {
			fail_ctrlr(sq->ctrlr);
		}
	}

	_nvmf_vfio_user_req_free(sq, vu_req);  /* [한국어] 후처리 끝 — 요청을 풀로 반환. */
}

/*
 * [한국어]
 * nvmf_vfio_user_close_qpair - qpair 종료: SQ 정리 + 마지막이면 controller free (.qpair_fini).
 *
 * @qpair: 종료할 qpair (CONTAINEROF 로 SQ 복원).
 * @cb_fn: 종료 완료를 코어에 알리는 콜백(있으면 동기 호출).
 * @cb_arg: cb_fn 인자.
 *
 * 동기/배경: qpair disconnect 의 마지막 단계로 코어가 호출한다. 이 SQ 를 controller 의
 *   connected_sqs 에서 떼고 SQ 자원(메모리/매핑)을 정리한다. 이 SQ 가 controller 의 마지막
 *   연결이었으면 endpoint 에서 ctrlr 를 분리하고 controller 전체를 free 한다. 지연됐던 SQ
 *   삭제 컨텍스트(delete_ctx)가 있으면 그 완료 콜백도 처리한다.
 * 동작 단계: SQ/ctrlr/endpoint 복원 → endpoint->lock 아래 connected_sqs 에서 제거 +
 *   delete_sq_done → 마지막이면 free_ctrlr → 락 해제 → delete_ctx 콜백 → cb_fn 호출.
 * 실행 컨텍스트: controller 의 thread. endpoint->lock 으로 connected_sqs/ctrlr 보호.
 * 호출자: spdk_nvmf_qpair_disconnect 마무리 → vtable .qpair_fini.
 * 에러 경로: 없음 (정리는 항상 성공).
 *
 * 호출 체인:
 *   spdk_nvmf_qpair_disconnect → [nvmf_vfio_user_close_qpair]
 *     → delete_sq_done / free_ctrlr / vfio_user_qpair_delete_cb / cb_fn
 */
static void
nvmf_vfio_user_close_qpair(struct spdk_nvmf_qpair *qpair,
			   spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct nvmf_vfio_user_sq *sq;                 /* [한국어] 종료할 SQ. */
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;        /* [한국어] SQ 의 controller. */
	struct nvmf_vfio_user_endpoint *endpoint;     /* [한국어] controller 의 endpoint. */
	struct vfio_user_delete_sq_ctx *del_ctx;      /* [한국어] 지연됐던 SQ 삭제 컨텍스트(있을 수 있음). */

	assert(qpair != NULL);
	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);  /* [한국어] qpair → SQ 복원. */
	vu_ctrlr = sq->ctrlr;                /* [한국어] SQ → controller. */
	endpoint = vu_ctrlr->endpoint;        /* [한국어] controller → endpoint. */
	del_ctx = sq->delete_ctx;            /* [한국어] Delete IO SQ 처리 중 매달린 컨텍스트 회수. */
	sq->delete_ctx = NULL;              /* [한국어] 중복 처리 방지로 즉시 분리. */

	pthread_mutex_lock(&endpoint->lock);  /* [한국어] connected_sqs/ctrlr 상태 보호. */
	TAILQ_REMOVE(&vu_ctrlr->connected_sqs, sq, tailq);  /* [한국어] controller 의 연결 SQ 목록에서 제거. */
	delete_sq_done(vu_ctrlr, sq);  /* [한국어] SQ 자원(매핑/메모리) 정리 + CQ 참조수 감소. */
	/* [한국어] 연결된 SQ 가 모두 사라졌으면 controller 전체를 해제. */
	if (TAILQ_EMPTY(&vu_ctrlr->connected_sqs)) {
		endpoint->ctrlr = NULL;   /* [한국어] endpoint 의 ctrlr 참조 끊음. */
		free_ctrlr(vu_ctrlr);     /* [한국어] controller 자원 일괄 해제. */
	}
	pthread_mutex_unlock(&endpoint->lock);  /* [한국어] 정리 완료 — 락 해제. */

	/* [한국어] Delete IO SQ 명령에 대한 완료 게시가 지연돼 있었으면 지금 처리. */
	if (del_ctx) {
		vfio_user_qpair_delete_cb(del_ctx);
	}

	/* [한국어] 코어에 qpair 종료 완료를 동기 통지(있으면). */
	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

/**
 * Returns a preallocated request, or NULL if there isn't one available.
 */
/*
 * [한국어]
 * get_nvmf_vfio_user_req - SQ 의 free_reqs 풀에서 미리 할당된 요청 1개를 빌린다.
 *
 * @sq: 요청을 빌릴 SQ (NULL 허용 — admin queue 미생성 등).
 * @return: 가용 요청 포인터, 풀이 비었거나 sq==NULL 이면 NULL.
 *
 * 동기/배경: vfio-user 는 hot path 에서 malloc 을 피하려고 SQ 마다 요청 객체를 미리
 *   할당해 두고 free_reqs TAILQ 로 재활용한다. 풀에서 하나 떼어 주는 것이 이 함수다.
 *   풀 고갈 시 NULL 을 반환해 호출자가 fail-fast CQE 를 발행하게 한다.
 * 동작 단계: sq NULL 가드 → free_reqs 첫 요소 확인 → 있으면 리스트에서 제거 후 반환.
 * 실행 컨텍스트: SQ 소유 thread (lockless — 단일 thread 접근).
 * 호출자: nvmf_vfio_user_poll_group_add, handle_cmd_req 등.
 * 에러 경로: 풀 고갈 시 NULL.
 *
 * 호출 체인:
 *   handle_cmd_req / poll_group_add → [get_nvmf_vfio_user_req] → TAILQ_REMOVE(free_reqs)
 */
static struct nvmf_vfio_user_req *
get_nvmf_vfio_user_req(struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_req *req;  /* [한국어] 빌려줄 요청. */

	/* [한국어] SQ 가 아직 없으면(예: admin queue 미생성) 빌려줄 풀도 없음. */
	if (sq == NULL) {
		return NULL;
	}

	req = TAILQ_FIRST(&sq->free_reqs);  /* [한국어] 재사용 풀의 첫 요청 확인. */
	/* [한국어] 풀이 비었으면(동시 발행 한도 도달) NULL → fail-fast 유도. */
	if (req == NULL) {
		return NULL;
	}

	TAILQ_REMOVE(&sq->free_reqs, req, link);  /* [한국어] 풀에서 떼어내 호출자에게 소유권 이전. */

	return req;
}

/*
 * [한국어]
 * get_nvmf_io_req_length - I/O 명령의 데이터 전송 길이를 byte 단위로 계산.
 *
 * @req: nvme_cmd 가 채워진 I/O 요청.
 * @return: 전송 길이(byte) >=0, namespace 조회 실패 시 -EINVAL.
 *
 * 동기/배경: PRP→iov 매핑 전에 명령의 데이터 길이를 알아야 한다. opcode 마다 길이를 결정하는
 *   필드가 다르다:
 *     - DATASET_MANAGEMENT(DSM): cdw10.nr(0-based)+1 개의 dsm_range 구조체.
 *     - COPY: cdw12 하위 8비트(0-based)+1 개의 source_range 구조체.
 *     - R/W/Compare 등: cdw12 하위 16비트(NLB, 0-based)+1 개의 logical block × block_size.
 *   block_size 는 namespace 의 bdev 에서 가져온다(논리 블록 크기).
 * 동작 단계: nsid 로 namespace 조회(없으면 -EINVAL) → opcode 분기로 길이 계산.
 * 실행 컨텍스트: I/O 명령 매핑 경로 — controller thread.
 * 호출자: vfio_user_map_cmd (I/O 명령 데이터 매핑 직전).
 * 에러 경로: nsid 무효/bdev 없음 → -EINVAL.
 *
 * 호출 체인:
 *   vfio_user_map_cmd → [get_nvmf_io_req_length] → _nvmf_subsystem_get_ns
 */
static int
get_nvmf_io_req_length(struct spdk_nvmf_request *req)
{
	uint16_t nr;        /* [한국어] DSM/COPY 의 range 개수(0-based+1). */
	uint32_t nlb, nsid;  /* [한국어] R/W 논리 블록 수 / namespace id. */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;       /* [한국어] 길이 결정 필드를 가진 SQE. */
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;     /* [한국어] namespace 조회용 controller. */
	struct spdk_nvmf_ns *ns;                                /* [한국어] 대상 namespace. */

	nsid = cmd->nsid;  /* [한국어] SQE 의 namespace id. */
	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, nsid);  /* [한국어] subsystem 에서 nsid 로 namespace 조회. */
	/* [한국어] namespace 가 없거나 backing bdev 가 없으면 길이 계산 불가. */
	if (ns == NULL || ns->bdev == NULL) {
		SPDK_ERRLOG("unsuccessful query for nsid %u\n", cmd->nsid);
		return -EINVAL;
	}

	/* [한국어] DSM: range descriptor 배열 길이 = (nr+1) * sizeof(dsm_range). */
	if (cmd->opc == SPDK_NVME_OPC_DATASET_MANAGEMENT) {
		nr = cmd->cdw10_bits.dsm.nr + 1;  /* [한국어] 0-based 개수 → +1. */
		return nr * sizeof(struct spdk_nvme_dsm_range);
	}

	/* [한국어] COPY: source range 배열 길이 = (cdw12[7:0]+1) * sizeof(scc_source_range). */
	if (cmd->opc == SPDK_NVME_OPC_COPY) {
		nr = (cmd->cdw12 & 0x000000ffu) + 1;  /* [한국어] 하위 8비트 = source range 수(0-based). */
		return nr * sizeof(struct spdk_nvme_scc_source_range);
	}

	/* [한국어] 그 외(R/W/Compare 등): cdw12[15:0] = NLB(0-based) → (NLB+1) * block_size. */
	nlb = (cmd->cdw12 & 0x0000ffffu) + 1;
	return nlb * spdk_bdev_desc_get_block_size(ns->desc);  /* [한국어] 논리 블록 수 × 블록 크기. */
}

/*
 * [한국어]
 * map_admin_cmd_req - admin SQE 의 PRP1/PRP2 를 VM 메모리 → host VA 로 매핑 (iov 생성).
 *
 * @ctrlr: controller.
 * @req: nvmf_request (cmd 가 채워진 상태).
 * @return: 0 성공 (req->iov 채워짐), 음수 errno.
 *
 * admin 명령마다 데이터 길이 결정 방식이 다름 (opcode 별로 cdw10/11 비트필드 의미 다름):
 *   - IDENTIFY: 고정 4096B.
 *   - GET_LOG_PAGE: numdu/numdl 비트필드로 dword 수, len = (numdw+1) * 4.
 *   - SET_FEATURES (HOST_BEHAVIOR_SUPPORT 등): fid 별 가변 (대부분 데이터 없음).
 *   - FW_DOWNLOAD: numd 비트필드.
 *   - 그 외: 대부분 데이터 없음 → req->xfer = NONE 로 reset.
 *
 * 이후 vfu_addr_to_sgl 로 PRP1/PRP2 의 VM IOVA → host VA + dma_sg_t 변환 → req->iov 채움.
 *
 * 호출 체인:
 *   handle_cmd_req (admin) → [본 함수] → vfu_addr_to_sgl → req->iov[] 채워짐
 *     → request_exec → admin command handler (Identify 등)
 */
static int
map_admin_cmd_req(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	uint32_t len = 0, numdw = 0;
	uint8_t fid;
	int iovcnt;

	/* [한국어] opcode 하위 2비트로 data transfer 방향 (NONE/H2C/C2H/BIDIR) 디코드. */
	req->xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);

	if (req->xfer == SPDK_NVME_DATA_NONE) {
		return 0;
	}

	/* [한국어] opcode 별 데이터 길이 결정 — NVMe spec § 5 의 각 명령별 cdw10/11 의미. */
	switch (cmd->opc) {
	case SPDK_NVME_OPC_IDENTIFY:
		/* [한국어] Identify Controller / Namespace / Active NS List 모두 4KB 고정. */
		len = 4096;
		break;
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		/* [한국어] Get Log Page: cdw10.numdl + cdw11.numdu 가 dword 수 (0-based).
		 * NVMe 1.2.1+ 의 32-bit numdw 지원. overflow 검증. */
		numdw = ((((uint32_t)cmd->cdw11_bits.get_log_page.numdu << 16) |
			  cmd->cdw10_bits.get_log_page.numdl) + 1);
		if (numdw > UINT32_MAX / 4) {
			return -EINVAL;
		}
		len = numdw * 4;
		break;
	case SPDK_NVME_OPC_GET_FEATURES:
	case SPDK_NVME_OPC_SET_FEATURES:
		fid = cmd->cdw10_bits.set_features.fid;
		switch (fid) {
		case SPDK_NVME_FEAT_LBA_RANGE_TYPE:
			len = 4096;
			break;
		case SPDK_NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION:
			len = 256;
			break;
		case SPDK_NVME_FEAT_TIMESTAMP:
			len = 8;
			break;
		case SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT:
			len = 512;
			break;
		case SPDK_NVME_FEAT_HOST_IDENTIFIER:
			if (cmd->cdw11_bits.feat_host_identifier.bits.exhid) {
				len = 16;
			} else {
				len = 8;
			}
			break;
		default:
			return 0;
		}
		break;
	case SPDK_NVME_OPC_FABRIC:
		return -ENOTSUP;
	default:
		return 0;
	}

	/* ADMIN command will not use SGL */
	if (cmd->psdt != 0) {
		return -EINVAL;
	}

	iovcnt = vfio_user_map_cmd(ctrlr, req, req->iov, len);
	if (iovcnt < 0) {
		SPDK_ERRLOG("%s: map Admin Opc %x failed\n",
			    ctrlr_id(ctrlr), cmd->opc);
		return -1;
	}
	req->length = len;
	req->iovcnt = iovcnt;

	return 0;
}

/*
 * Map an I/O command's buffers.
 *
 * Returns 0 on success and -errno on failure.
 */
/*
 * [한국어]
 * map_io_cmd_req - IO SQE (Read/Write/Flush/DSM/Copy 등) 의 PRP → host iov 매핑.
 *
 * @ctrlr: controller.
 * @req: nvmf_request.
 * @return: 0 = 성공 (req->iov/iovcnt/length 채워짐), 음수 errno.
 *
 * IO 명령은 데이터 길이 계산이 admin 보다 단순 (대부분 nlb * lba_size). admin 과 분리한
 * 이유: opcode 별 cdw 의미가 IO 와 admin 에서 다르고, get_nvmf_io_req_length 가 ns
 * lookup 후 ns->bdev 의 block_size 사용 (admin 은 ns 와 무관).
 *
 * 단계:
 *   1. data transfer 방향 결정 (opcode 하위 2비트).
 *   2. NONE 이면 데이터 없음 — 즉시 반환 (Flush 등).
 *   3. get_nvmf_io_req_length: opcode 별 길이 (Read/Write = nlb*lba_size,
 *      DSM = nr*sizeof(dsm_range), Copy = nr*sizeof(scc_source_range)).
 *   4. vfio_user_map_cmd: PRP1/PRP2 디코딩 → vfu_addr_to_sgl 로 host VA + iov 채움.
 *
 * 호출 체인:
 *   handle_cmd_req (IO) → [본 함수] → vfio_user_map_cmd → req->iov[] 채워짐
 *     → request_exec → bdev I/O
 */
static int
map_io_cmd_req(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvmf_request *req)
{
	int len, iovcnt;
	struct spdk_nvme_cmd *cmd;

	assert(ctrlr != NULL);
	assert(req != NULL);

	cmd = &req->cmd->nvme_cmd;
	/* [한국어] opcode 하위 2비트 = data transfer 방향. */
	req->xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);

	/* [한국어] Flush 같은 데이터 없는 명령 — 즉시 반환. */
	if (spdk_unlikely(req->xfer == SPDK_NVME_DATA_NONE)) {
		return 0;
	}

	/* [한국어] opcode + ns block_size 로 길이 계산. */
	len = get_nvmf_io_req_length(req);
	if (len < 0) {
		return -EINVAL;
	}
	req->length = len;

	/* [한국어] PRP → iov 매핑 (PRP1, PRP2 list, vfu_addr_to_sgl 다중 page 처리). */
	iovcnt = vfio_user_map_cmd(ctrlr, req, req->iov, req->length);
	if (iovcnt < 0) {
		SPDK_ERRLOG("%s: failed to map IO OPC %u\n", ctrlr_id(ctrlr), cmd->opc);
		return -EFAULT;
	}
	req->iovcnt = iovcnt;

	return 0;
}

static int
/*
 * [한국어]
 * handle_cmd_req - ★ SQE 1개를 받아 nvmf_request 로 변환 + bdev/admin dispatch.
 *
 * @ctrlr: controller.
 * @cmd: VM 의 SQ 에서 fetch 한 NVMe SQE (64B).
 * @sq: 이 SQE 가 도착한 SQ.
 * @return: 0 = 성공 (비동기 in-flight), -1 = 즉시 실패 (완료 CQE 이미 전송됨).
 *
 * nvmf_vfio_user_sq_poll 이 새 SQE 발견할 때마다 호출. RDMA 의 request_process(NEW) 와
 * 비슷한 entry point.
 *
 * 단계:
 *   1. get_nvmf_vfio_user_req: SQ free_reqs 풀에서 vu_req 1개 빌림.
 *      실패 = 풀 고갈 → INTERNAL_DEVICE_ERROR CQE 즉시 발행 (호스트가 retry/abort 결정).
 *   2. cb_fn = handle_cmd_rsp, cb_arg = SQ — bdev 완료 시 후처리 콜백 등록.
 *   3. SQE 사본을 req->cmd 로 복사 — VM 메모리 disturbance 회피.
 *   4. admin SQ 면 map_admin_cmd_req (Identify/Create IO Q/Set Features/Get Features 등 분기),
 *      IO SQ 면 map_io_cmd_req (Read/Write/Flush 등).
 *   5. Reservation / Fabric 명령은 vfio-user 미지원 → -ENOTSUP.
 *   6. map 실패 시: status code 채우고 handle_cmd_rsp 직접 호출 (CQE 발행) + req 풀 반환.
 *   7. 성공 시 EXECUTING 전이 + spdk_nvmf_request_exec 로 bdev/admin 처리 위임.
 *
 * 호출 체인:
 *   nvmf_vfio_user_sq_poll → SQE fetch → [본 함수]
 *     → map_admin_cmd_req / map_io_cmd_req (PRP → iov 변환)
 *     → spdk_nvmf_request_exec → bdev I/O
 *     → (bdev 완료) handle_cmd_rsp → post_completion → CQE write
 */
static int
handle_cmd_req(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd,
	       struct nvmf_vfio_user_sq *sq)
{
	int err;
	struct nvmf_vfio_user_req *vu_req;
	struct spdk_nvmf_request *req;

	assert(ctrlr != NULL);
	assert(cmd != NULL);

	/* [한국어] SQ 의 free_reqs TAILQ 에서 vu_req 빌림. 풀 고갈 시 fail-fast CQE. */
	vu_req = get_nvmf_vfio_user_req(sq);
	if (spdk_unlikely(vu_req == NULL)) {
		SPDK_ERRLOG("%s: no request for NVMe command opc 0x%x\n", ctrlr_id(ctrlr), cmd->opc);
		return post_completion(ctrlr, ctrlr->cqs[sq->cqid], 0, 0, cmd->cid,
				       SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, SPDK_NVME_SCT_GENERIC);

	}
	req = &vu_req->req;

	assert(req->qpair != NULL);
	SPDK_DEBUGLOG(nvmf_vfio, "%s: handle sqid:%u, req opc=%#x cid=%d\n",
		      ctrlr_id(ctrlr), req->qpair->qid, cmd->opc, cmd->cid);

	/* [한국어] 완료 콜백 등록 — bdev 완료 시 handle_cmd_rsp 가 CQE post + req 풀 반환. */
	vu_req->cb_fn = handle_cmd_rsp;
	vu_req->cb_arg = SPDK_CONTAINEROF(req->qpair, struct nvmf_vfio_user_sq, qpair);
	/* [한국어] SQE 사본 — VM 메모리 (req->cmd 가 가리키는 영역) 와 분리. VM 이 SQ wrap-around
	 * 으로 이 슬롯을 덮어써도 안전. */
	req->cmd->nvme_cmd = *cmd;

	if (nvmf_qpair_is_admin_queue(req->qpair)) {
		/* [한국어] admin SQ — Identify/Create Q/Set Features/Get Log Page 등. */
		err = map_admin_cmd_req(ctrlr, req);
	} else {
		switch (cmd->opc) {
		case SPDK_NVME_OPC_RESERVATION_REGISTER:
		case SPDK_NVME_OPC_RESERVATION_REPORT:
		case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		case SPDK_NVME_OPC_RESERVATION_RELEASE:
		case SPDK_NVME_OPC_FABRIC:
			/* [한국어] vfio-user 는 capsule transport 없음 → Fabric 명령 의미 X.
			 * Reservation 도 multi-controller 시나리오 필요 — 현재 미지원. */
			err = -ENOTSUP;
			break;
		default:
			/* [한국어] Read/Write/Flush/DSM/Compare/Write Zeroes 등 — bdev 위임. */
			err = map_io_cmd_req(ctrlr, req);
			break;
		}
	}

	if (spdk_unlikely(err < 0)) {
		/* [한국어] 매핑 실패 (PRP invalid, opcode 미지원 등) → 즉시 에러 CQE 발행 + 풀 반환. */
		SPDK_ERRLOG("%s: process NVMe command opc 0x%x failed\n",
			    ctrlr_id(ctrlr), cmd->opc);
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = err == -ENOTSUP ?
					       SPDK_NVME_SC_INVALID_OPCODE :
					       SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		err = handle_cmd_rsp(vu_req, vu_req->cb_arg);
		_nvmf_vfio_user_req_free(sq, vu_req);
		return err;
	}

	/* [한국어] EXECUTING 전이 + 상위 NVMe-oF 레이어로 dispatch — bdev I/O 또는 admin 처리. */
	vu_req->state = VFIO_USER_REQUEST_STATE_EXECUTING;
	spdk_nvmf_request_exec(req);

	return 0;
}

/*
 * If we suppressed an IRQ in post_completion(), check if it needs to be fired
 * here: if the host isn't up to date, and is apparently not actively processing
 * the queue (i.e. ->last_head isn't changing), we need an IRQ.
 */
static void
handle_suppressed_irq(struct nvmf_vfio_user_ctrlr *ctrlr,
		      struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_cq *cq = ctrlr->cqs[sq->cqid];
	uint32_t cq_head;
	uint32_t cq_tail;

	if (!cq->ien || cq->qid == 0 || !ctrlr_interrupt_enabled(ctrlr)) {
		return;
	}

	cq_tail = *cq_tailp(cq);

	/* Already sent? */
	if (cq_tail == cq->last_trigger_irq_tail) {
		return;
	}

	spdk_ivdt_dcache(cq_dbl_headp(cq));
	cq_head = *cq_dbl_headp(cq);

	if (cq_head != cq_tail && cq_head == cq->last_head) {
		int err = vfu_irq_trigger(ctrlr->endpoint->vfu_ctx, cq->iv);
		if (err != 0) {
			SPDK_ERRLOG("%s: failed to trigger interrupt: %m\n",
				    ctrlr_id(ctrlr));
		} else {
			cq->last_trigger_irq_tail = cq_tail;
		}
	}

	cq->last_head = cq_head;
}

/*
 * [한국어]
 * nvmf_vfio_user_sq_poll - 1 SQ 의 doorbell 검사 + 새 SQE 처리.
 *
 * @sq: 폴링할 SQ.
 * @return: 처리한 SQE 수 (>=0), 또는 음수 errno.
 *
 * poll_group_poll 이 ACTIVE SQ 마다 호출. 단계:
 *   1. doorbell 영역 (bar0_doorbells 또는 shadow_doorbells) 에서 new_tail read.
 *   2. SPDK 측 head 와 비교 → 새 SQE 없으면 return 0.
 *   3. handle_sq_tdbl_write 가 head..new_tail 구간의 SQE 들을 순차 처리:
 *      - 각 SQE 마다 handle_cmd_req → request_exec.
 *      - sq->head 증가.
 *   4. handle_suppressed_irq 호출 — IRQ coalescing 환경에서 호스트가 응답 못 받은 채
 *      block 됐을 수 있으므로 강제 IRQ.
 *
 * 호출 체인:
 *   poll_group_poll → [본 함수] → handle_sq_tdbl_write → handle_cmd_req → request_exec
 */
/* Returns the number of commands processed, or a negative value on error. */
static int
nvmf_vfio_user_sq_poll(struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_ctrlr *ctrlr;
	uint32_t new_tail;
	int count = 0;

	assert(sq != NULL);

	ctrlr = sq->ctrlr;

	/*
	 * A quiesced, or migrating, controller should never process new
	 * commands.
	 */
	if (ctrlr->state != VFIO_USER_CTRLR_RUNNING) {
		return SPDK_POLLER_IDLE;
	}

	if (ctrlr->adaptive_irqs_enabled) {
		handle_suppressed_irq(ctrlr, sq);
	}

	/* On aarch64 platforms, doorbells update from guest VM may not be seen
	 * on SPDK target side. This is because there is memory type mismatch
	 * situation here. That is on guest VM side, the doorbells are treated as
	 * device memory while on SPDK target side, it is treated as normal
	 * memory. And this situation cause problem on ARM platform.
	 * Refer to "https://developer.arm.com/documentation/102376/0100/
	 * Memory-aliasing-and-mismatched-memory-types". Only using spdk_mb()
	 * cannot fix this. Use "dc civac" to invalidate cache may solve
	 * this.
	 */
	spdk_ivdt_dcache(sq_dbl_tailp(sq));

	/* Load-Acquire. */
	new_tail = *sq_dbl_tailp(sq);

	new_tail = new_tail & 0xffffu;
	if (spdk_unlikely(new_tail >= sq->size)) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s: invalid sqid:%u doorbell value %u\n", ctrlr_id(ctrlr), sq->qid,
			      new_tail);
		spdk_nvmf_ctrlr_async_event_error_event(ctrlr->ctrlr, SPDK_NVME_ASYNC_EVENT_INVALID_DB_WRITE);

		return -1;
	}

	if (*sq_headp(sq) == new_tail) {
		return 0;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s: sqid:%u doorbell old=%u new=%u\n",
		      ctrlr_id(ctrlr), sq->qid, *sq_headp(sq), new_tail);
	if (ctrlr->sdbl != NULL) {
		SPDK_DEBUGLOG(nvmf_vfio,
			      "%s: sqid:%u bar0_doorbell=%u shadow_doorbell=%u eventidx=%u\n",
			      ctrlr_id(ctrlr), sq->qid,
			      ctrlr->bar0_doorbells[queue_index(sq->qid, false)],
			      ctrlr->sdbl->shadow_doorbells[queue_index(sq->qid, false)],
			      ctrlr->sdbl->eventidxs[queue_index(sq->qid, false)]);
	}

	/*
	 * Ensure that changes to the queue are visible to us.
	 * The host driver should write the queue first, do a wmb(), and then
	 * update the SQ tail doorbell (their Store-Release).
	 */
	spdk_rmb();

	count = handle_sq_tdbl_write(ctrlr, new_tail, sq);
	if (spdk_unlikely(count < 0)) {
		fail_ctrlr(ctrlr);
	}

	return count;
}

/*
 * vfio-user transport poll handler. Note that the library context is polled in
 * a separate poller (->vfu_ctx_poller), so this poller only needs to poll the
 * active SQs.
 *
 * Returns the number of commands processed, or a negative value on error.
 */
/*
 * [한국어]
 * nvmf_vfio_user_poll_group_poll - ★ vfio-user transport hot path ★ — poll_group 의 모든
 *                                   ACTIVE SQ 폴링.
 *
 * @group: 공통 poll_group (transport vtable 콜백).
 * @return: 처리한 SQE 수 (양수) 또는 음수 errno.
 *
 * RDMA 의 poll_group_poll 과 비슷한 entry point. 단 큰 차이: vfu_ctx 폴링은 별도 poller
 * (vfu_ctx_poller) 가 담당 — 본 함수는 SQ 폴링만 (doorbell 처리 X). 책임 분리로 vfu_ctx
 * polling 빈도를 줄여도 SQ 처리 latency 영향 최소화.
 *
 * 단계:
 *   1. poll_group 의 sqs TAILQ 순회.
 *   2. ACTIVE 가 아니거나 size=0 인 SQ 는 skip (CREATED/DELETED/INACTIVE/UNUSED).
 *   3. 각 SQ 에 대해 nvmf_vfio_user_sq_poll 호출:
 *      - SQ head (SPDK 측) 와 SQ tail doorbell (VM 측) 비교 → 새 SQE 있나 검사.
 *      - shadow doorbell 사용 시 shadow_doorbells 도 비교.
 *      - 새 SQE 마다 handle_cmd_req → bdev_exec 또는 admin command 처리.
 *   4. _SAFE iteration — sq_poll 안에서 SQ 가 DELETED 로 전이될 수 있음.
 *
 * vfu_ctx_poller 와 분리한 이유:
 *   - vfu_ctx 폴링은 무거움 (eventfd / Unix socket polling 포함) — 매 iter 호출 부담.
 *   - SQ 폴링은 가벼움 (메모리 read 만) — 매 iter 가능.
 *   - 분리로 SQ 폴링 빈도 ≫ vfu_ctx 폴링 빈도 → 평균 latency 감소.
 *
 * 호출 체인:
 *   SPDK reactor → [본 함수] → nvmf_vfio_user_sq_poll → handle_cmd_req
 *     → spdk_nvmf_request_exec → bdev I/O
 */
static int
nvmf_vfio_user_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct nvmf_vfio_user_poll_group *vu_group;
	struct nvmf_vfio_user_sq *sq, *tmp;
	int count = 0;

	assert(group != NULL);

	vu_group = SPDK_CONTAINEROF(group, struct nvmf_vfio_user_poll_group, group);

	SPDK_DEBUGLOG(vfio_user_db, "polling all SQs\n");

	/* [한국어] _SAFE: sq_poll 안에서 sq state 가 DELETED 전이 가능 (admin Delete IO SQ 처리 등). */
	TAILQ_FOREACH_SAFE(sq, &vu_group->sqs, link, tmp) {
		int ret;

		/* [한국어] ACTIVE + size>0 인 SQ 만 — 나머지는 IO 처리 자격 없음. */
		if (spdk_unlikely(sq->sq_state != VFIO_USER_SQ_ACTIVE || !sq->size)) {
			continue;
		}

		ret = nvmf_vfio_user_sq_poll(sq);

		if (spdk_unlikely(ret < 0)) {
			return ret;
		}

		count += ret;
	}

	vu_group->stats.polls++;
	vu_group->stats.poll_reqs += count;
	vu_group->stats.poll_reqs_squared += count * count;
	if (count == 0) {
		vu_group->stats.polls_spurious++;
	}

	if (vu_group->need_kick) {
		poll_group_kick(vu_group);
	}

	return count;
}

/*
 * [한국어]
 * nvmf_vfio_user_qpair_get_local_trid - qpair 의 로컬(target) transport id 조회 (.qpair_get_local_trid).
 *
 * @qpair: 대상 qpair (CONTAINEROF 로 SQ→ctrlr→endpoint 복원).
 * @trid: 결과를 채울 transport id 버퍼.
 * @return: 항상 0.
 *
 * 동기/배경: 코어/RPC 가 qpair 의 로컬 주소(어느 endpoint 소켓에 붙었는지)를 알고자 할 때
 *   호출한다. vfio-user 에서는 endpoint 의 trid(소켓 경로)가 곧 로컬 주소다.
 * 동작: endpoint->trid 를 trid 로 memcpy.
 * 실행 컨텍스트: controller thread (조회만, 부작용 없음).
 * 호출자: 코어 qpair 정보 질의 → vtable .qpair_get_local_trid.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   (코어 질의) → [본 함수] → memcpy(endpoint->trid)
 */
static int
nvmf_vfio_user_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_sq *sq;            /* [한국어] qpair 가 속한 SQ. */
	struct nvmf_vfio_user_ctrlr *ctrlr;       /* [한국어] SQ 의 controller. */

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);  /* [한국어] qpair → SQ 복원. */
	ctrlr = sq->ctrlr;  /* [한국어] SQ → controller. */

	memcpy(trid, &ctrlr->endpoint->trid, sizeof(*trid));  /* [한국어] endpoint 소켓 trid 를 복사. */
	return 0;
}

/*
 * [한국어]
 * nvmf_vfio_user_qpair_get_peer_trid - qpair 의 원격(host) transport id 조회 (.qpair_get_peer_trid) — no-op.
 *
 * @qpair: 대상 qpair (미사용).
 * @trid: 결과 버퍼 (채우지 않음).
 * @return: 항상 0.
 *
 * 동기/배경: 네트워크 transport 는 peer(원격 호스트)의 IP/port 를 채우지만, vfio-user 는
 *   같은 머신의 VM 이 PCIe 장치로 붙는 모델이라 의미 있는 peer 주소가 없다. 따라서 비워 둔다.
 * 동작: 아무 것도 하지 않고 0 반환(vtable 슬롯 NULL 회피용 stub).
 * 실행 컨텍스트: controller thread.
 * 호출자: 코어 qpair 정보 질의 → vtable .qpair_get_peer_trid.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   (코어 질의) → [본 함수] (no-op)
 */
static int
nvmf_vfio_user_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvme_transport_id *trid)
{
	return 0;  /* [한국어] vfio-user 는 의미 있는 peer 주소 없음 — 빈 stub. */
}

/*
 * [한국어]
 * nvmf_vfio_user_qpair_get_listen_trid - qpair 가 붙은 listener transport id 조회 (.qpair_get_listen_trid).
 *
 * @qpair: 대상 qpair (CONTAINEROF 로 SQ→ctrlr→endpoint 복원).
 * @trid: 결과를 채울 transport id 버퍼.
 * @return: 항상 0.
 *
 * 동기/배경: 이 qpair 가 어느 listener(소켓)로 들어왔는지 코어가 알고자 할 때 호출한다.
 *   vfio-user 에서는 local trid 와 동일(endpoint 소켓이 곧 listener).
 * 동작: endpoint->trid 를 trid 로 memcpy.
 * 실행 컨텍스트: controller thread.
 * 호출자: 코어 qpair 정보 질의 → vtable .qpair_get_listen_trid.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   (코어 질의) → [본 함수] → memcpy(endpoint->trid)
 */
static int
nvmf_vfio_user_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				     struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_sq *sq;            /* [한국어] qpair 가 속한 SQ. */
	struct nvmf_vfio_user_ctrlr *ctrlr;       /* [한국어] SQ 의 controller. */

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);  /* [한국어] qpair → SQ 복원. */
	ctrlr = sq->ctrlr;  /* [한국어] SQ → controller. */

	memcpy(trid, &ctrlr->endpoint->trid, sizeof(*trid));  /* [한국어] listener=endpoint 소켓 trid 복사. */
	return 0;
}

/*
 * [한국어]
 * nvmf_vfio_user_qpair_abort_request - NVMe Abort admin 명령 처리: 대상 명령 찾아 중단 (.qpair_abort_request).
 *
 * @qpair: Abort 가 발행된 qpair.
 * @req: Abort admin 요청 (cdw10.abort.cid 가 중단할 명령의 cid).
 *
 * 동기/배경: host 가 진행 중인 명령을 취소하려고 NVMe Abort(admin)를 보낼 수 있다. 같은
 *   qpair 의 outstanding 요청들 중 cid 가 일치하고 EXECUTING 상태인 것을 찾아 코어 abort
 *   경로로 넘긴다. 대상이 없으면 Abort 자체를 즉시 성공 완료시킨다(이미 끝났거나 없음).
 * 동작 단계: cdw10 에서 대상 cid 추출 → outstanding 순회로 EXECUTING + cid 일치 요청 검색 →
 *   없으면 Abort 즉시 complete → 있으면 req_to_abort 설정 후 nvmf_ctrlr_abort_request.
 * 실행 컨텍스트: admin qpair 의 thread (outstanding 리스트는 같은 thread 소유).
 * 호출자: 코어 admin Abort 핸들러 → vtable .qpair_abort_request.
 * 에러 경로: 대상 없음 → Abort 를 성공으로 완료(부작용 없음).
 *
 * 호출 체인:
 *   (admin Abort) → [본 함수] → nvmf_ctrlr_abort_request 또는 spdk_nvmf_request_complete
 */
static void
nvmf_vfio_user_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_request *req_to_abort = NULL;  /* [한국어] 중단 대상 명령(찾으면 설정). */
	struct spdk_nvmf_request *temp_req = NULL;       /* [한국어] outstanding 순회 커서. */
	uint16_t cid;                                     /* [한국어] 중단할 명령의 command id. */

	cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;  /* [한국어] Abort SQE cdw10 에서 대상 cid 추출. */

	/* [한국어] 같은 qpair 의 진행 중 요청들을 순회하며 일치하는 명령 검색. */
	TAILQ_FOREACH(temp_req, &qpair->outstanding, link) {
		struct nvmf_vfio_user_req *vu_req;  /* [한국어] generic req 를 감싼 vfio 요청. */

		vu_req = SPDK_CONTAINEROF(temp_req, struct nvmf_vfio_user_req, req);  /* [한국어] req → vu_req 복원. */

		/* [한국어] EXECUTING 상태(실제 진행 중) + cid 일치인 것만 중단 대상. */
		if (vu_req->state == VFIO_USER_REQUEST_STATE_EXECUTING && vu_req->cmd.cid == cid) {
			req_to_abort = temp_req;
			break;  /* [한국어] 첫 일치에서 종료(cid 는 유일). */
		}
	}

	/* [한국어] 대상이 없으면(이미 완료/없음) Abort 자체를 성공으로 즉시 완료. */
	if (req_to_abort == NULL) {
		spdk_nvmf_request_complete(req);
		return;
	}

	req->req_to_abort = req_to_abort;  /* [한국어] 코어 abort 경로에 중단 대상 연결. */
	nvmf_ctrlr_abort_request(req);     /* [한국어] 코어가 대상 명령을 abort 처리. */
}

/*
 * [한국어]
 * nvmf_vfio_user_poll_group_dump_stat - PG 통계를 JSON 으로 출력 (.poll_group_dump_stat).
 *
 * @group: 통계를 덤프할 generic poll group (CONTAINEROF 로 vu_group 복원).
 * @w: JSON writer 컨텍스트 (RPC 응답에 named 필드로 기록).
 *
 * 동기/배경: nvmf_get_stats RPC 가 transport별 PG 통계를 요청하면 호출된다. vfio-user 는
 *   interrupt/kick/rearm/poll 분포 등 polled+interrupt 혼합 운영의 진단 지표를 노출한다.
 *   poll 당 처리 요청 수의 분산(poll_reqs_variance)까지 계산해 부하 균일성을 보여 준다.
 * 동작 단계: vu_group 복원 → 각 stats 필드를 spdk_json_write_named_uint64 로 기록 →
 *   polls>=2 면 표본 분산 공식으로 poll_reqs_variance(double) 기록 → cqh writes 기록.
 * 실행 컨텍스트: RPC 처리 thread(해당 PG 의 thread 에 위임되어 호출됨).
 * 호출자: nvmf_get_stats RPC 경로 → vtable .poll_group_dump_stat.
 * 에러 경로: 없음 (분모 0 이면 variance 생략).
 *
 * 호출 체인:
 *   nvmf_get_stats RPC → [본 함수] → spdk_json_write_named_*
 */
static void
nvmf_vfio_user_poll_group_dump_stat(struct spdk_nvmf_transport_poll_group *group,
				    struct spdk_json_write_ctx *w)
{
	struct nvmf_vfio_user_poll_group *vu_group = SPDK_CONTAINEROF(group,
			struct nvmf_vfio_user_poll_group, group);  /* [한국어] generic → vfio PG 복원. */
	uint64_t polls_denom;  /* [한국어] 표본 분산 분모 polls*(polls-1). */

	spdk_json_write_named_uint64(w, "ctrlr_intr", vu_group->stats.ctrlr_intr);     /* [한국어] controller interrupt 수신 수. */
	spdk_json_write_named_uint64(w, "ctrlr_kicks", vu_group->stats.ctrlr_kicks);   /* [한국어] controller kick(강제 폴) 수. */
	spdk_json_write_named_uint64(w, "pg_kicks", vu_group->stats.pg_kicks);         /* [한국어] PG kick 수. */
	spdk_json_write_named_uint64(w, "won", vu_group->stats.won);                   /* [한국어] 경쟁에서 SQ 처리권 획득 수. */
	spdk_json_write_named_uint64(w, "lost", vu_group->stats.lost);                 /* [한국어] 처리권 양보 수. */
	spdk_json_write_named_uint64(w, "lost_count", vu_group->stats.lost_count);     /* [한국어] 양보로 미처리된 SQE 수. */
	spdk_json_write_named_uint64(w, "rearms", vu_group->stats.rearms);             /* [한국어] eventidx 재무장 수. */
	spdk_json_write_named_uint64(w, "cq_full", vu_group->stats.cq_full);           /* [한국어] CQ 가득 차 게시 지연 수. */
	spdk_json_write_named_uint64(w, "pg_process_count", vu_group->stats.pg_process_count);  /* [한국어] PG process 호출 수. */
	spdk_json_write_named_uint64(w, "intr", vu_group->stats.intr);                 /* [한국어] eventfd interrupt 수신 수. */
	spdk_json_write_named_uint64(w, "polls", vu_group->stats.polls);               /* [한국어] poll 라운드 수(표본 n). */
	spdk_json_write_named_uint64(w, "polls_spurious", vu_group->stats.polls_spurious);  /* [한국어] 처리 없이 끝난 poll 수. */
	spdk_json_write_named_uint64(w, "poll_reqs", vu_group->stats.poll_reqs);       /* [한국어] poll 들이 처리한 총 요청 수(합). */
	polls_denom = vu_group->stats.polls * (vu_group->stats.polls - 1);  /* [한국어] 표본 분산 분모(n*(n-1)). */
	/* [한국어] poll 이 2회 이상일 때만 분산 계산(분모 0 회피). */
	if (polls_denom) {
		/* [한국어] 분자 = n*Σx² − (Σx)² (표본 분산 공식 변형). */
		uint64_t n = vu_group->stats.polls * vu_group->stats.poll_reqs_squared - vu_group->stats.poll_reqs *
			     vu_group->stats.poll_reqs;
		spdk_json_write_named_double(w, "poll_reqs_variance", sqrt(n / polls_denom));  /* [한국어] poll 당 요청 수 표준편차. */
	}

	spdk_json_write_named_uint64(w, "cqh_admin_writes", vu_group->stats.cqh_admin_writes);  /* [한국어] admin CQ head doorbell write 수. */
	spdk_json_write_named_uint64(w, "cqh_io_writes", vu_group->stats.cqh_io_writes);        /* [한국어] IO CQ head doorbell write 수. */
}

/*
 * [한국어]
 * nvmf_vfio_user_opts_init - vfio-user transport 기본 옵션값 채우기 (.opts_init).
 *
 * @opts: 코어가 넘긴 옵션 구조체 (transport 기본값으로 초기화 대상).
 *
 * 동기/배경: nvmf_create_transport RPC 가 사용자 값을 덮어쓰기 전에, 코어가 먼저 이 콜백으로
 *   transport 별 합리적 기본값을 채운다. vfio-user 는 capsule transport 가 없어 in-capsule
 *   data/shared buffer 가 의미 없으므로 0 으로 둔다(데이터는 PRP→DMA 로 직접 전달).
 * 동작: queue depth/qpair 수/IO size/AQ depth 등을 vfio-user 기본 매크로로 설정하고,
 *   in_capsule_data_size/num_shared_buffers/buf_cache_size/association_timeout/
 *   transport_specific 은 0/NULL.
 * 실행 컨텍스트: transport 생성 전 코어 init thread (단일).
 * 호출자: spdk_nvmf_transport_opts_init → vtable .opts_init.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   spdk_nvmf_transport_opts_init → [nvmf_vfio_user_opts_init]
 */
static void
nvmf_vfio_user_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		NVMF_VFIO_USER_DEFAULT_MAX_QUEUE_DEPTH;       /* [한국어] SQ/CQ 기본 깊이. */
	opts->max_qpairs_per_ctrlr =	NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR;  /* [한국어] controller 당 기본 qpair 수. */
	opts->in_capsule_data_size =	0;                                            /* [한국어] capsule transport 없음 → 0. */
	opts->max_io_size =		NVMF_VFIO_USER_DEFAULT_MAX_IO_SIZE;            /* [한국어] 단일 I/O 최대 byte. */
	opts->io_unit_size =		NVMF_VFIO_USER_DEFAULT_IO_UNIT_SIZE;           /* [한국어] I/O 분할 단위 byte. */
	opts->max_aq_depth =		NVMF_VFIO_USER_DEFAULT_AQ_DEPTH;               /* [한국어] admin queue 기본 깊이. */
	opts->num_shared_buffers =	0;                                            /* [한국어] PRP 직접 DMA → 공유 버퍼 불필요. */
	opts->buf_cache_size =		0;                                            /* [한국어] 버퍼 캐시 미사용. */
	opts->association_timeout =	0;                                            /* [한국어] association timeout 미사용(0). */
	opts->transport_specific =      NULL;                                          /* [한국어] 전용 JSON 옵션은 create 단계에서 채움. */
}

/*
 * [한국어]
 * spdk_nvmf_transport_vfio_user - ★ NVMe-oF vfio-user transport vtable ★.
 *
 * type="VFIOUSER" 로 외부 RPC nvmf_create_transport 가 활성화. RDMA/TCP/FC 와 같은 인터페이스
 * 하지만 의미는 완전히 다름 — 네트워크 X, host 안의 VM 에 가상 NVMe device 노출.
 *
 *  - name="VFIOUSER" (RPC 사용자 식별자).
 *  - opts_init: max_queue_depth 256, max_qpairs_per_ctrlr 128 등 기본값.
 *  - create: vfio-user library context 초기화, intr_mode_supported 검출.
 *  - listen: vfu_ctx 생성 + Unix socket bind + accept_poller 등록.
 *  - listen_associate: subsystem 과 listener 연결 (controller emulation 활성화).
 *  - poll_group_*: PG 생성/추가/제거/polling (poll_group_poll = SQ poll hot path).
 *  - req_free/complete: bdev 완료 → handle_cmd_rsp → CQE post.
 *  - qpair_fini = close_qpair: SQ destroy + free_reqs 반환.
 *  - poll_group_dump_stat: RPC nvmf_get_transports 응답에 통계 (intr/kicks/won/lost/cq_full).
 *
 * SPDK_NVMF_TRANSPORT_REGISTER 의 이름이 "muser" 인 이유: 초기 이름이 muser (microUser)
 * 였다가 vfio-user 로 표준화됐지만 ABI 호환 위해 등록 이름 유지.
 */
const struct spdk_nvmf_transport_ops spdk_nvmf_transport_vfio_user = {
	.name = "VFIOUSER",
	.type = SPDK_NVME_TRANSPORT_VFIOUSER,
	.opts_init = nvmf_vfio_user_opts_init,            /* [한국어] 기본값 (queue_depth/qpairs_per_ctrlr 등) */
	.create = nvmf_vfio_user_create,                   /* [한국어] transport 객체 + intr_mode capability check */
	.destroy = nvmf_vfio_user_destroy,                 /* [한국어] 모든 endpoints + poll_groups 일괄 정리 */

	.listen = nvmf_vfio_user_listen,                   /* [한국어] vfu_ctx + Unix socket + accept_poller */
	.stop_listen = nvmf_vfio_user_stop_listen,         /* [한국어] socket close + endpoint destroy */
	.cdata_init = nvmf_vfio_user_cdata_init,           /* [한국어] Identify Controller 응답의 VENDOR ID 등 */
	.listen_associate = nvmf_vfio_user_listen_associate, /* [한국어] subsystem ↔ endpoint 연결 */

	.listener_discover = nvmf_vfio_user_discover,      /* [한국어] discovery target — 거의 사용 안 함 (VM 직접 알 수 없음) */

	.poll_group_create = nvmf_vfio_user_poll_group_create,
	.get_optimal_poll_group = nvmf_vfio_user_get_optimal_poll_group, /* [한국어] RR next_pg 분배 */
	.poll_group_destroy = nvmf_vfio_user_poll_group_destroy,
	.poll_group_add = nvmf_vfio_user_poll_group_add,   /* [한국어] SQ 를 PG 의 sqs 리스트에 추가 */
	.poll_group_remove = nvmf_vfio_user_poll_group_remove,
	.poll_group_poll = nvmf_vfio_user_poll_group_poll, /* [한국어] ★ hot path — ACTIVE SQ 폴링 */

	.req_free = nvmf_vfio_user_req_free,               /* [한국어] vu_req 를 free_reqs 풀에 반환 */
	.req_complete = nvmf_vfio_user_req_complete,       /* [한국어] bdev 완료 콜백 → handle_cmd_rsp 트리거 */

	.qpair_fini = nvmf_vfio_user_close_qpair,          /* [한국어] SQ DELETED 전이 + 자원 정리 */
	.qpair_get_local_trid = nvmf_vfio_user_qpair_get_local_trid,
	.qpair_get_peer_trid = nvmf_vfio_user_qpair_get_peer_trid,
	.qpair_get_listen_trid = nvmf_vfio_user_qpair_get_listen_trid,
	.qpair_abort_request = nvmf_vfio_user_qpair_abort_request, /* [한국어] NVMe Abort admin 처리 */

	.poll_group_dump_stat = nvmf_vfio_user_poll_group_dump_stat, /* [한국어] RPC stats JSON */
};

/* [한국어] ELF .init_array constructor — main() 전 transport 리스트에 등록.
 * 이름 "muser" 는 historical (이전 명칭 microUser). type 은 VFIOUSER 사용. */
SPDK_NVMF_TRANSPORT_REGISTER(muser, &spdk_nvmf_transport_vfio_user);
/* [한국어] SPDK 로그 컴포넌트 — SPDK_DEBUGLOG(nvmf_vfio, ...) 가 "nvmf_vfio" 컴포넌트 분류. */
SPDK_LOG_REGISTER_COMPONENT(nvmf_vfio)
/* [한국어] doorbell 전용 디버그 컴포넌트 — 빈도 높아 별도 toggle 가능. */
SPDK_LOG_REGISTER_COMPONENT(vfio_user_db)
