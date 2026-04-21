/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 */

/*
 * [한국어 설명] NVMe PCIe 트랜스포트 내부 구조 · 헬퍼 (nvme_pcie_internal.h)
 *
 * === 파일의 역할 ===
 * SPDK 유저스페이스 NVMe 드라이버가 **PCIe 트랜스포트**에서 사용하는 내부
 * 상태 구조체와 hot-path 인라인 함수, 함수 프로토타입을 모아둔다. SPDK에서
 * I/O 경로의 "실제로 NVMe 디바이스와 직접 통신하는" 계층이며, 이 파일이
 * 정의하는 타입은 다음과 같다:
 *
 *   1) struct nvme_pcie_ctrlr
 *      - 컨트롤러 레벨 PCIe 확장. BAR 매핑, CMB(Controller Memory Buffer),
 *        PMR(Persistent Memory Region), doorbell stride, PCI device handle
 *      - spdk_nvme_ctrlr 공개 구조체의 "상속" 형태(container_of 패턴)
 *
 *   2) struct nvme_pcie_qpair
 *      - 큐 페어 레벨 PCIe 확장 — SQ/CQ 메모리, doorbell 포인터, tracker 배열
 *      - shadow doorbell 지원 (NVMe 1.3+ 선택적 기능, 호스트 메모리에
 *        doorbell 복사본을 두어 MMIO write를 조건부로 생략)
 *      - hot-path 필드를 상단에 배치해 한 캐시 라인 안에 배치
 *
 *   3) struct nvme_tracker (4KB 고정)
 *      - 단일 in-flight 요청의 PRP 리스트 또는 SGL 디스크립터를 담는 객체
 *      - command ID(cid)로 색인되어 완료 시 원래 요청을 복원
 *      - 크기 4096 B는 NVMe PRP 페이지 경계를 보장하기 위함(PRP 리스트는
 *        4KB 경계를 넘지 않아야 함)
 *
 *   4) struct nvme_pcie_poll_group
 *      - 여러 qpair를 묶어 한 번에 process_completions 하는 폴링 그룹
 *
 *   5) hot-path inline 함수
 *      - nvme_pcie_qpair_ring_sq_doorbell(): SQ tail doorbell 업데이트
 *        (shadow doorbell 경로와 MMIO 경로 분기)
 *      - nvme_pcie_qpair_ring_cq_doorbell(): CQ head doorbell 업데이트
 *      - nvme_pcie_qpair_need_event(): shadow doorbell event 필요 여부
 *      - nvme_pcie_qpair_update_mmio_required(): shadow doorbell 갱신 +
 *        MMIO 필요 판정
 *      - nvme_pcie_qpair/ctrlr(): 공개 구조체에서 PCIe 확장 포인터 복원
 *
 *   6) 함수 프로토타입: qpair 생성/파괴/리셋, SQ/CQ 생성 admin cmd,
 *      tracker abort/complete/submit, poll group 연산 등 구현 파일들이
 *      공유하는 API.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [User] spdk_nvme_ns_cmd_read
 *     → lib/nvme/nvme_ns_cmd.c: 요청(nvme_request) 생성
 *     → nvme_qpair_submit_request() → transport dispatch
 *     → nvme_pcie_qpair_submit_request() [이 파일의 프로토타입]
 *       → tracker 할당, PRP/SGL 빌드, SQ 엔트리 기록
 *       → nvme_pcie_qpair_ring_sq_doorbell() [이 파일의 인라인]
 *         → (shadow db 확인) → spdk_mmio_write_4(SQ tail) → 하드웨어 인식
 *
 *   [Polling] spdk_nvme_qpair_process_completions
 *     → nvme_pcie_qpair_process_completions() [이 파일의 프로토타입]
 *       → CQ phase 비트 검사 → 유효 CQE 파싱
 *       → tracker로 원래 요청 복원 → 완료 콜백 호출
 *       → nvme_pcie_qpair_ring_cq_doorbell()
 *
 * 실행 컨텍스트: qpair는 생성한 SPDK thread에 고정. 위 hot-path 함수는 그
 * 소유 스레드에서만 호출.
 *
 * === 타 모듈과의 연결 ===
 * 의존: lib/nvme/nvme_internal.h (spdk_nvme_ctrlr/qpair 정의), spdk/mmio.h,
 *       spdk/barrier.h, spdk/util.h (SPDK_CONTAINEROF)
 * 의존하는 모듈(구현 측): nvme_pcie.c, nvme_pcie_common.c
 * 공유 자료구조: nvme_pcie_ctrlr, nvme_pcie_qpair, nvme_tracker
 * 공유 전역 상태: g_thread_mmio_ctrlr (__thread per-thread) — 현재 MMIO
 *   발행 중인 컨트롤러 추적 (WC 버퍼 flush 시점 판정 등)
 *
 * === 주요 함수/구조체 요약 ===
 * 위 내용 참조. 핵심은 `nvme_tracker`(요청당 4KB PRP/SGL 슬롯)와 `nvme_pcie_qpair`
 * (SQ/CQ + doorbell + tracker 관리).
 */

#ifndef __NVME_PCIE_INTERNAL_H__ /* [한국어] include 가드 */
#define __NVME_PCIE_INTERNAL_H__

/*
 * Number of completion queue entries to process before ringing the
 *  completion queue doorbell.
 */
#define NVME_MIN_COMPLETIONS	(1)
                                 /* [한국어] 한 번의 process_completions 호출에서 처리할 CQE의 최소 개수
                                  *  - 0이면 폴링이 아무것도 처리 안 하고 리턴할 수 있어 최소 1 */
#define NVME_MAX_COMPLETIONS	(128)
                                 /* [한국어] 단일 process 호출에서 처리할 CQE 상한 (기본 128)
                                  *  - 너무 크면 한 스레드가 한 qpair에서 너무 오래 머무름 → 다른 qpair/poller starve
                                  *  - 너무 작으면 완료 스루풋 저하
                                  *  - 128은 SPDK 경험칙 */

/*
 * NVME_MAX_SGL_DESCRIPTORS defines the maximum number of descriptors in one SGL
 *  segment.
 */
#define NVME_MAX_SGL_DESCRIPTORS	(250)
                                 /* [한국어] 단일 SGL 세그먼트에 들어갈 수 있는 디스크립터 수
                                  *  - 각 SGL 디스크립터는 16B. 250 * 16 = 4000B < 4096B (nvme_tracker 4KB 중 other 필드 제외한 여유)
                                  *  - tracker가 4KB 고정이므로 u.sgl[] 배열 길이를 여기에 맞춰 정함 */

#define NVME_MAX_PRP_LIST_ENTRIES	(503)
                                 /* [한국어] PRP 리스트 모드의 엔트리 수
                                  *  - PRP 엔트리는 8B (u.prp[] = uint64_t). tracker 4KB 중 PRP 배열에 503*8 = 4024B 사용
                                  *  - 나머지 72B가 상단 필드들(req 포인터 등)에 할당 */

/* Minimum admin queue size */
#define NVME_PCIE_MIN_ADMIN_QUEUE_SIZE	(256)
                                 /* [한국어] admin 큐 최소 엔트리 수
                                  *  - NVMe 스펙상 admin queue 깊이 16~4096. SPDK는 256을 최소로 강제하여 AER(Async Event Request) 여러 개 + 기본 admin 명령을 충분히 병렬 발행 가능 */

/* PCIe transport extensions for spdk_nvme_ctrlr */
struct nvme_pcie_ctrlr {
	struct spdk_nvme_ctrlr ctrlr;
                                 /* [한국어] 공개 컨트롤러 구조체를 **임베딩** — SPDK_CONTAINEROF로 복원됨
                                  *  - 이 필드가 구조체 첫 번째 위치일 필요는 없고, nvme_pcie_ctrlr() 인라인이 offsetof를 고려해 변환
                                  *  - 공개 경로는 &pctrlr->ctrlr을 외부로 노출, 내부 경로는 container_of로 원본 복원 */

	/** NVMe MMIO register space */
	volatile struct spdk_nvme_registers *regs;
                                 /* [한국어] BAR0에 매핑된 NVMe 컨트롤러 레지스터 영역
                                  *  - CAP, VS, INTMS, CC, CSTS, AQA, ASQ, ACQ, doorbell 배열이 포함
                                  *  - volatile 필수: 장치가 비동기로 갱신하므로 컴파일러가 최적화로 제거하면 안 됨
                                  *  - 설정자: nvme_pcie_ctrlr_construct (mmap via VFIO/UIO)
                                  *  - 읽는 자: 컨트롤러 초기화, reset 시퀀스, 상태 쿼리 */

	/** NVMe MMIO register size */
	uint64_t regs_size;
                                 /* [한국어] 매핑된 레지스터 영역 크기 (바이트)
                                  *  - doorbell 배열 끝까지 포함. 큐 페어 수에 따라 달라짐
                                  *  - 언매핑 시 munmap 길이로 사용 */

	struct {
		/* BAR mapping address which contains controller memory buffer */
		void *bar_va;            /* [한국어] CMB가 있는 BAR의 유저스페이스 VA (mmap 결과) */

		/* BAR physical address which contains controller memory buffer */
		uint64_t bar_pa;         /* [한국어] 위 BAR의 물리 주소 — IOMMU 매핑 대상 */

		/* Controller memory buffer size in Bytes */
		uint64_t size;           /* [한국어] CMB 총 크기
                                  *  - 컨트롤러가 자신의 내부 메모리를 BAR에 노출. SQ/CQ 또는 데이터를 여기 배치하면 호스트 DRAM 왕복이 줄어듦(특히 지연 개선)
                                  *  - SPDK는 선택적으로 SQ를 CMB에 배치 (sq_in_cmb 플래그 참조) */

		/* Current offset of controller memory buffer, relative to start of BAR virt addr */
		uint64_t current_offset; /* [한국어] CMB에서 다음 할당을 줄 오프셋 (bump allocator) */

		void *mem_register_addr; /* [한국어] DPDK rte_mem_register에 등록한 주소 (IOMMU/메모리 맵 추적용) */
		size_t mem_register_size;/* [한국어] 등록된 영역 크기 */
	} cmb;                        /* [한국어] Controller Memory Buffer — NVMe 1.2+ 선택적 기능 */

	struct {
		/* BAR mapping address which contains persistent memory region */
		void *bar_va;

		/* BAR physical address which contains persistent memory region */
		uint64_t bar_pa;

		/* Persistent memory region size in Bytes */
		uint64_t size;

		void *mem_register_addr;
		size_t mem_register_size;
	} pmr;                        /* [한국어] Persistent Memory Region — NVMe 1.4+ 선택적 기능
                                  *  - 전원 손실에도 보존되는 컨트롤러 메모리. 주로 저널/로그에 사용 */

	/** stride in uint32_t units between doorbell registers (1 = 4 bytes, 2 = 8 bytes, ...) */
	uint32_t doorbell_stride_u32;
                                 /* [한국어] doorbell 간 stride (NVMe 스펙 CAP.DSTRD 필드)
                                  *  - 일반 컨트롤러 0(=4B stride). 일부 HW는 캐시 라인 분리를 위해 1(=8B), 2(=16B)
                                  *  - doorbell_base[2*i*(2^stride)] 형태로 인덱싱해야 올바른 doorbell 주소 계산 */

	/* Opaque handle to associated PCI device. */
	struct spdk_pci_device *devhandle;
                                 /* [한국어] DPDK rte_pci_device를 감싼 SPDK 핸들. config 공간 접근, IRQ 관리 등에 사용 */

	/* Flag to indicate the MMIO register has been remapped */
	bool is_remapped;            /* [한국어] Hot-plug 재연결 등으로 BAR가 다시 매핑되었는지 표시 */

	volatile uint32_t *doorbell_base;
                                 /* [한국어] doorbell 배열의 시작 주소 (regs 구조체 내부 위치)
                                  *  - qpair가 자기 SQ/CQ doorbell 포인터를 이 기준으로 계산하여 캐싱 */
};

extern __thread struct nvme_pcie_ctrlr *g_thread_mmio_ctrlr;
                                 /* [한국어] 현재 스레드가 MMIO write를 수행 중인 PCIe 컨트롤러 추적 (TLS)
                                  *  - 목적: 특정 아키텍처(AArch64 등)에서 MMIO write-combining 버퍼 flush 시점 제어, 또는 관찰성 목적 추적
                                  *  - 설정자: doorbell ring inline 함수 진입 시 세팅, 종료 시 NULL로 복원
                                  *  - __thread 키워드로 스레드별 독립 저장 (DPDK lcore 모델과 호환) */

struct nvme_tracker {
	TAILQ_ENTRY(nvme_tracker)       tq_list;
                                 /* [한국어] 자유/진행중 리스트 링크 (qpair의 free_tr 또는 outstanding_tr에 연결) */

	struct nvme_request		*req;
                                 /* [한국어] 이 tracker가 서비스 중인 NVMe 요청 포인터
                                  *  - 완료 시 cpl을 기반으로 req의 콜백 호출에 사용 */
	uint16_t			cid;
                                 /* [한국어] 이 tracker에 할당된 command ID (SQE의 CID 필드와 동일)
                                  *  - 완료 시 CQE의 CID로 tracker 배열을 색인해 원래 요청 복원
                                  *  - 0 ~ num_entries-1 범위 */

	uint16_t			bad_vtophys : 1;
                                 /* [한국어] virt→phys 변환 실패로 요청이 무효해진 상태 표시
                                  *  - 1이면 완료 시점에 요청을 에러 완료 처리 */
	uint16_t			rsvd0 : 15;
                                 /* [한국어] 예약 비트 — 추후 플래그 추가용 */
	uint32_t			rsvd1;
                                 /* [한국어] 정렬/추후 확장용 예약 */

	spdk_nvme_cmd_cb		cb_fn;
                                 /* [한국어] 이 요청 완료 시 호출할 콜백 함수 (req->cb_fn의 캐시) */
	void				*cb_arg;
                                 /* [한국어] 콜백 컨텍스트 */

	uint64_t			prp_sgl_bus_addr;
                                 /* [한국어] 아래 union(prp 또는 sgl) 영역의 IOMMU 버스 주소
                                  *  - SQE의 PRP2 또는 SGL 디스크립터에 기록되어 장치가 이 tracker의 리스트를 DMA로 읽게 함
                                  *  - tracker 자체가 4KB 정렬된 메모리 풀에서 할당되어 bus_addr이 미리 계산되어 있음 */

	/* Don't move, metadata SGL is always contiguous with Data Block SGL */
	struct spdk_nvme_sgl_descriptor		meta_sgl;
                                 /* [한국어] 메타데이터(PI 등) SGL 디스크립터 — 데이터 SGL 바로 앞에 연속 배치 필수
                                  *  - NVMe 스펙상 메타 SGL과 데이터 SGL이 인접해야 MPTR+DPTR 관계가 성립 */
	union {
		uint64_t			prp[NVME_MAX_PRP_LIST_ENTRIES];
                                 /* [한국어] PRP 모드 — 각 엔트리 8B, 최대 503개 → 503*4KB = 2MB 단일 전송 가능
                                  *  - PRP 엔트리는 4KB 경계 주소. 첫 PRP만 임의 오프셋 허용 */
		struct spdk_nvme_sgl_descriptor	sgl[NVME_MAX_SGL_DESCRIPTORS];
                                 /* [한국어] SGL 모드 — 각 디스크립터 16B, 최대 250개
                                  *  - PRP보다 자유로운 scatter-gather 가능 (오프셋/길이 임의) */
	} u;                          /* [한국어] PRP 또는 SGL 중 하나만 사용. mode 선택은 요청 빌드 시점에 결정 */
};
/*
 * struct nvme_tracker must be exactly 4K so that the prp[] array does not cross a page boundary
 * and so that there is no padding required to meet alignment requirements.
 */
SPDK_STATIC_ASSERT(sizeof(struct nvme_tracker) == 4096, "nvme_tracker is not 4K");
                                 /* [한국어] 크기 4096 고정 — PRP 리스트가 페이지 경계를 넘지 않아야 장치 DMA가 안전
                                  *  - 구조체 변경 시 이 assert가 깨짐 → 필드 재배치 필수 */
SPDK_STATIC_ASSERT((offsetof(struct nvme_tracker, u.sgl) & 7) == 0, "SGL must be Qword aligned");
                                 /* [한국어] SGL 디스크립터는 8B 정렬 필수 (스펙) */
SPDK_STATIC_ASSERT((offsetof(struct nvme_tracker, meta_sgl) & 7) == 0, "SGL must be Qword aligned");
                                 /* [한국어] meta SGL 역시 8B 정렬 */

struct nvme_pcie_poll_group {
	struct spdk_nvme_transport_poll_group group;
                                 /* [한국어] 트랜스포트 공용 polling group 임베딩 — container_of로 복원 */
	struct spdk_nvme_pcie_stat stats;
                                 /* [한국어] 그룹 통계 (MMIO doorbell 업데이트 수, shadow 경로 수 등) */
};

enum nvme_pcie_qpair_state {
	NVME_PCIE_QPAIR_WAIT_FOR_CQ = 1,
                                 /* [한국어] admin create_io_cq 발행 후 완료 대기 상태 */
	NVME_PCIE_QPAIR_WAIT_FOR_SQ,
                                 /* [한국어] CQ 준비됐고 create_io_sq 완료 대기 */
	NVME_PCIE_QPAIR_READY,
                                 /* [한국어] I/O 처리 가능 상태 — read/write 제출 허용 */
	NVME_PCIE_QPAIR_FAILED,
                                 /* [한국어] 오류 상태 — qpair 재생성 필요 */
};

/* PCIe transport extensions for spdk_nvme_qpair */
struct nvme_pcie_qpair {
	/* Submission queue tail doorbell */
	volatile uint32_t *sq_tdbl;
                                 /* [한국어] 이 qpair의 SQ tail doorbell 레지스터 주소 (BAR 내)
                                  *  - spdk_mmio_write_4로 tail 값을 써서 장치에 새 SQE 제출 통지
                                  *  - volatile: MMIO 영역이므로 캐시/최적화 금지 */
	/* Completion queue head doorbell */
	volatile uint32_t *cq_hdbl;
                                 /* [한국어] CQ head doorbell — 완료 엔트리를 소비한 후 head 값을 써서 장치에 공간 회수 알림 */

	/* Submission queue */
	struct spdk_nvme_cmd *cmd;
                                 /* [한국어] SQ의 호스트 메모리 가상 주소
                                  *  - num_entries개의 연속된 spdk_nvme_cmd(=64B SQE) 배열
                                  *  - sq_tail 인덱스에 SQE를 쓰고 doorbell ring으로 장치에 통지 */
	/* Completion queue */
	struct spdk_nvme_cpl *cpl;
                                 /* [한국어] CQ의 호스트 메모리 가상 주소 — num_entries개의 spdk_nvme_cpl(=16B CQE) 배열
                                  *  - 장치가 DMA로 여기에 완료 엔트리 기록, 드라이버는 phase bit 폴링으로 유효성 판단 */

	TAILQ_HEAD(, nvme_tracker) free_tr;
                                 /* [한국어] 재사용 가능한 tracker 리스트 (free list)
                                  *  - 새 요청 제출 시 이 리스트에서 pop, 완료 시 다시 push */
	TAILQ_HEAD(nvme_outstanding_tr_head, nvme_tracker) outstanding_tr;
                                 /* [한국어] 현재 장치에 제출되어 완료 대기 중인 tracker 리스트
                                  *  - reset/abort 시 순회하여 정리 */

	/* Array of trackers indexed by command ID. */
	struct nvme_tracker *tr;
                                 /* [한국어] tracker 배열 — cid로 직접 색인
                                  *  - num_entries개 연속 배치. CQE.CID로 O(1) 조회
                                  *  - 4KB 정렬 할당 (tracker 자체가 4KB) */

	struct spdk_nvme_pcie_stat *stat;
                                 /* [한국어] qpair 단위 통계 포인터 (공유 가능 — shared_stats 플래그 참조) */

	uint16_t num_entries;
                                 /* [한국어] 큐 깊이 (SQ/CQ 엔트리 수. 동일 값)
                                  *  - 2^k 형태로 설정되는 것이 일반적 — wrap 계산이 & (num-1)로 단순해짐 */

	uint8_t pcie_state;
                                 /* [한국어] enum nvme_pcie_qpair_state 중 하나. qpair 생성 과정 추적 */

	uint8_t retry_count;
                                 /* [한국어] 일시적 오류 시 재시도 횟수 (controller.retry_count 캐시) */

	uint16_t max_completions_cap;
                                 /* [한국어] 한 번의 process_completions 호출에서 처리할 최대 CQE 수 상한
                                  *  - 호출 측이 지정한 값과 qpair 설정값 중 작은 쪽이 적용 */

	uint16_t last_sq_tail;
                                 /* [한국어] 직전 doorbell ring 시점의 sq_tail 값 — shadow doorbell 조건 평가용 */
	uint16_t sq_tail;
                                 /* [한국어] 다음 SQE를 쓸 위치(0~num_entries-1)
                                  *  - 제출할 때마다 증가, num_entries에서 wrap */
	uint16_t cq_head;
                                 /* [한국어] 다음 소비할 CQE 위치 (호스트 측 진행 포인터) */
	uint16_t sq_head;
                                 /* [한국어] 장치가 소비한 SQ 위치 (CQE에 담겨 호스트에 반환됨)
                                  *  - 장치 관점의 SQ head — 호스트는 이를 읽어 free space 판정 */

	struct {
		uint8_t phase			: 1;
                                 /* [한국어] 다음에 기대하는 CQE phase 비트(0/1 토글)
                                  *  - 장치가 CQ 순회를 한 바퀴 돌 때마다 phase가 뒤집히므로 이 값과 일치해야 "새 CQE" */
		uint8_t delay_cmd_submit	: 1;
                                 /* [한국어] 명령 제출을 즉시 하지 않고 배치로 지연 (성능 옵션)
                                  *  - 여러 SQE를 누적한 뒤 한 번의 doorbell ring으로 제출 → MMIO 비용 절감 */
		uint8_t has_shadow_doorbell	: 1;
                                 /* [한국어] 컨트롤러가 shadow doorbell 기능을 지원·활성화했는지
                                  *  - 활성 시 doorbell write를 호스트 메모리에 먼저 수행하고 조건부로만 MMIO 발행 → x86 WC flush 비용 절감 */
		uint8_t has_pending_vtophys_failures : 1;
                                 /* [한국어] virt→phys 변환에서 실패한 요청이 대기 중 — 완료 처리 루프에서 에러 보고 */
		uint8_t defer_destruction	: 1;
                                 /* [한국어] 파괴 요청을 지연 처리 중 (콜백 중 qpair 제거 등) */

		/* Disable merging of physically contiguous SGL entries */
		uint8_t disable_pcie_sgl_merge	: 1;
                                 /* [한국어] 인접한 물리 페이지를 하나의 SGL 디스크립터로 병합하지 않도록 강제
                                  *  - 일부 HW 버그 회피용. 평소에는 병합으로 디스크립터 수↓, 스루풋↑ */
	} flags;

	/*
	 * Base qpair structure.
	 * This is located after the hot data in this structure so that the important parts of
	 * nvme_pcie_qpair are in the same cache line.
	 */
	struct spdk_nvme_qpair qpair;
                                 /* [한국어] 공개 qpair 구조체 임베딩 — 의도적으로 hot data 뒤에 배치
                                  *  - sq_tdbl/cmd/cpl/free_tr 등 hot-path 필드들이 한 캐시 라인에 몰리도록 설계
                                  *  - SPDK_CONTAINEROF(qpair_ptr, struct nvme_pcie_qpair, qpair)로 확장 복원 */

	struct {
		/* Submission queue shadow tail doorbell */
		volatile uint32_t *sq_tdbl;
                                 /* [한국어] 호스트 메모리의 shadow SQ tail — MMIO 대신 먼저 여기에 write */

		/* Completion queue shadow head doorbell */
		volatile uint32_t *cq_hdbl;
                                 /* [한국어] shadow CQ head */

		/* Submission queue event index */
		volatile uint32_t *sq_eventidx;
                                 /* [한국어] 장치가 "여기까지 소비했을 때 MMIO 깨워달라"고 알려주는 인덱스
                                  *  - 호스트가 sq_tail 업데이트 시 이 값과 비교해 MMIO 실제 발행 여부 결정 */
		/* Completion queue event index */
		volatile uint32_t *cq_eventidx;
                                 /* [한국어] 동일 목적의 CQ 버전 */
	} shadow_doorbell;            /* [한국어] NVMe 1.3+ Shadow Doorbell & Event Index 기능 */

	/*
	 * Fields below this point should not be touched on the normal I/O path.
	 */
                                 /* [한국어] 아래 필드는 cold — 초기화/해제 시에만 접근. hot data와 캐시 라인 분리 */

	bool sq_in_cmb;              /* [한국어] SQ를 CMB에 배치했는지 여부 (지연 이득 있으나 CMB 용량·기능 제약) */
	bool shared_stats;           /* [한국어] stat 필드가 poll group과 공유인지 (free 책임 구분) */

	uint64_t cmd_bus_addr;       /* [한국어] SQ 버스 주소 — admin create_io_sq 발행 시 장치에 전달 */
	uint64_t cpl_bus_addr;       /* [한국어] CQ 버스 주소 — admin create_io_cq 발행 시 장치에 전달 */

	struct spdk_nvme_cmd *sq_vaddr;
                                 /* [한국어] SQ의 원래 할당 가상 주소 — munmap/free 용 (cmd와 다를 수 있음; CMB 배치 시) */
	struct spdk_nvme_cpl *cq_vaddr;
                                 /* [한국어] CQ 원래 할당 가상 주소 */
};

static inline struct nvme_pcie_qpair *
nvme_pcie_qpair(struct spdk_nvme_qpair *qpair)
/*
 * [한국어]
 * nvme_pcie_qpair - 공개 qpair 포인터에서 PCIe 확장 구조체 복원
 * @qpair: spdk_nvme_qpair의 주소
 * @return: 임베딩 컨테이너의 주소
 */
{
	return SPDK_CONTAINEROF(qpair, struct nvme_pcie_qpair, qpair);
                                 /* [한국어] util.h의 container_of 매크로 — offsetof 기반 포인터 산술 */
}

static inline struct nvme_pcie_ctrlr *
nvme_pcie_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
/*
 * [한국어]
 * nvme_pcie_ctrlr - 공개 ctrlr 포인터에서 PCIe 확장 구조체 복원
 */
{
	return SPDK_CONTAINEROF(ctrlr, struct nvme_pcie_ctrlr, ctrlr);
}

static inline int
nvme_pcie_qpair_need_event(uint16_t event_idx, uint16_t new_idx, uint16_t old)
/*
 * [한국어]
 * nvme_pcie_qpair_need_event - shadow doorbell 모드에서 실제 MMIO가 필요한지 판정
 *
 * @event_idx: 장치가 알려준 "이 시점에는 나를 깨워라" 임계값
 * @new_idx:   이번에 쓰려는 tail/head 값
 * @old:       직전에 장치에 보였던 값
 * @return:    0이면 MMIO 불필요, 비영이면 필요
 *
 * 원리 (NVMe 1.3 §3.1.24): 16비트 wrap-around 감안한 구간 포함 판정.
 *   "event_idx가 (old, new_idx] 구간에 포함되면 MMIO 필요"
 *   부호 없는 뺄셈 트릭으로 wrap-around도 정확히 처리.
 * 효과: 장치가 "따라오고 있을 때"는 MMIO 생략으로 WC flush 비용 회피.
 */
{
	return (uint16_t)(new_idx - event_idx) <= (uint16_t)(new_idx - old);
                                 /* [한국어] 부호 없는 16비트 뺄셈 — wrap around에도 상대거리를 올바르게 계산
                                  *  - 캐스팅으로 결과 폭 유지 */
}

static inline bool
nvme_pcie_qpair_update_mmio_required(uint16_t value,
				     volatile uint32_t *shadow_db,
				     volatile uint32_t *eventidx)
/*
 * [한국어]
 * nvme_pcie_qpair_update_mmio_required - shadow doorbell 갱신 후 MMIO 필요 여부 결정
 *
 * @value:     새 doorbell 값
 * @shadow_db: 호스트 shadow doorbell 주소 (volatile)
 * @eventidx:  장치가 설정한 event index 주소
 * @return:    true면 MMIO 발행 필요
 *
 * 절차:
 *   1) spdk_wmb() — 이전에 작성한 SQE가 메모리에 확정된 뒤 shadow_db 쓰기가 보이도록
 *   2) shadow_db 기존값(old) 저장, 새 value 쓰기
 *   3) spdk_mb() — shadow_db 쓰기와 eventidx 읽기 사이 순서 보장(full barrier 필요)
 *   4) eventidx로 need_event 평가 → MMIO 여부 반환
 */
{
	uint16_t old;                /* [한국어] 직전 shadow doorbell 값 */

	spdk_wmb();                  /* [한국어] 선행 store 완료 보장 — SQE의 모든 필드가 호스트 메모리에 반영된 후 doorbell 업데이트 */

	old = *shadow_db;            /* [한국어] shadow doorbell 원래 값 읽기 */
	*shadow_db = value;          /* [한국어] 새 값 기록 (장치가 이 위치를 DMA로 읽을 수 있음) */

	/*
	 * Ensure that the doorbell is updated before reading the EventIdx from
	 * memory
	 */
	spdk_mb();                   /* [한국어] shadow_db write가 eventidx read보다 먼저 관찰되도록 full barrier
                                  *  - 그렇지 않으면 stale eventidx로 잘못된 MMIO 결정 가능 */

	if (!nvme_pcie_qpair_need_event(*eventidx, value, old)) {
		return false;            /* [한국어] event index가 구간 밖 → 장치가 아직 깨울 필요 없다고 표시 → MMIO 생략 */
	}

	return true;                 /* [한국어] MMIO 발행 필요 */
}

static inline void
nvme_pcie_qpair_ring_sq_doorbell(struct spdk_nvme_qpair *qpair)
/*
 * [한국어]
 * nvme_pcie_qpair_ring_sq_doorbell - SQ tail doorbell 발행 (hot path 중 hot path)
 *
 * 동작:
 *   1) fused 명령의 첫 번째 조각이면 doorbell 연기 (두 번째 조각 제출 시에 한 번만 울림)
 *   2) shadow doorbell 활성이면 shadow 우선 갱신, eventidx 검사로 MMIO 필요 판정
 *   3) MMIO 필요 시: wmb → TLS 컨트롤러 마킹 → MMIO write → 마킹 해제
 *
 * 호출 체인:
 *   nvme_pcie_qpair_submit_request → SQE 쓰기 → [이 함수]
 */
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
                                 /* [한국어] PCIe 확장 구조 복원 */
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(qpair->ctrlr);
                                 /* [한국어] 컨트롤러 확장 (MMIO TLS 플래그용) */
	bool need_mmio = true;       /* [한국어] 기본: MMIO 필요 */

	if (qpair->last_fuse == SPDK_NVME_IO_FLAGS_FUSE_FIRST) {
		/* This is first cmd of two fused commands - don't ring doorbell */
		return;
                                 /* [한국어] Fused 명령은 두 SQE가 원자적으로 실행되어야 하므로 첫 SQE에서는 doorbell 울리지 않음
                                  *  - 두 번째 SQE 제출 시 한 번 울려 둘 다 동시에 장치에 가시화 */
	}

	if (spdk_unlikely(pqpair->flags.has_shadow_doorbell)) {
		pqpair->stat->sq_shadow_doorbell_updates++;
                                 /* [한국어] 통계 — shadow doorbell 쓰기 횟수 */
		need_mmio = nvme_pcie_qpair_update_mmio_required(
				    pqpair->sq_tail,
				    pqpair->shadow_doorbell.sq_tdbl,
				    pqpair->shadow_doorbell.sq_eventidx);
                                 /* [한국어] shadow 경로: shadow_db 갱신 후 eventidx로 MMIO 필요성 판단 */
	}

	if (spdk_likely(need_mmio)) {
		spdk_wmb();              /* [한국어] SQE 쓰기 완료 후 MMIO 보장 — 장치가 반쪽 SQE 읽는 것 방지 */
		pqpair->stat->sq_mmio_doorbell_updates++;
                                 /* [한국어] 통계 — 실 MMIO 횟수 */
		g_thread_mmio_ctrlr = pctrlr;
                                 /* [한국어] 현재 스레드의 활성 MMIO 컨트롤러 마킹 (특정 아키텍처에서 WC flush hook 등) */
		spdk_mmio_write_4(pqpair->sq_tdbl, pqpair->sq_tail);
                                 /* [한국어] 실제 doorbell write — 장치가 이 값을 인지하고 SQE fetch 시작 */
		g_thread_mmio_ctrlr = NULL;
                                 /* [한국어] 마킹 해제 */
	}
}

static inline void
nvme_pcie_qpair_ring_cq_doorbell(struct spdk_nvme_qpair *qpair)
/*
 * [한국어]
 * nvme_pcie_qpair_ring_cq_doorbell - CQ head doorbell 발행
 *
 * 호스트가 완료 엔트리 소비 후 장치에 "여기까지 읽었다"고 알림 →
 * 장치는 이 영역을 회수해 새 CQE 배치 가능.
 * SQ와 달리 wmb는 불필요 — CQ head 쓰기는 선행 데이터 의존성이 없음.
 */
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(qpair->ctrlr);
	bool need_mmio = true;

	if (spdk_unlikely(pqpair->flags.has_shadow_doorbell)) {
		pqpair->stat->cq_shadow_doorbell_updates++;
		need_mmio = nvme_pcie_qpair_update_mmio_required(
				    pqpair->cq_head,
				    pqpair->shadow_doorbell.cq_hdbl,
				    pqpair->shadow_doorbell.cq_eventidx);
                                 /* [한국어] shadow 경로 처리 */
	}

	if (spdk_likely(need_mmio)) {
		pqpair->stat->cq_mmio_doorbell_updates++;
		g_thread_mmio_ctrlr = pctrlr;
		spdk_mmio_write_4(pqpair->cq_hdbl, pqpair->cq_head);
                                 /* [한국어] CQ head 값 쓰기 — 장치가 새 CQE 배치용 공간 회수 */
		g_thread_mmio_ctrlr = NULL;
	}
}

int nvme_pcie_qpair_reset(struct spdk_nvme_qpair *qpair);
/*
 * [한국어]
 * nvme_pcie_qpair_reset - qpair 내부 상태 리셋 (SQ/CQ 포인터·tail/head·phase 초기화)
 */

int nvme_pcie_qpair_get_fd(struct spdk_nvme_qpair *qpair, struct spdk_event_handler_opts *opts);
/*
 * [한국어]
 * nvme_pcie_qpair_get_fd - 인터럽트 모드 시 사용할 fd 획득 (epoll 연동)
 */

int nvme_pcie_qpair_construct(struct spdk_nvme_qpair *qpair,
			      const struct spdk_nvme_io_qpair_opts *opts);
/*
 * [한국어]
 * nvme_pcie_qpair_construct - I/O qpair 자료구조 구축 (SQ/CQ 할당, tracker 풀 생성)
 */

int nvme_pcie_ctrlr_construct_admin_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t num_entries);
/*
 * [한국어]
 * admin qpair(qid=0) 전용 construct — AQA 레지스터 설정 포함
 */

void nvme_pcie_qpair_insert_pending_admin_request(struct spdk_nvme_qpair *qpair,
		struct nvme_request *req, struct spdk_nvme_cpl *cpl);
/*
 * [한국어]
 * AER(Async Event Request) 같은 완료되지 않은 admin 요청을 대기 리스트에 삽입
 */

void nvme_pcie_qpair_complete_pending_admin_request(struct spdk_nvme_qpair *qpair);
/*
 * [한국어]
 * 대기 중인 admin 요청을 모아 완료 콜백 일괄 호출
 */

int nvme_pcie_ctrlr_cmd_create_io_cq(struct spdk_nvme_ctrlr *ctrlr,
				     struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn,
				     void *cb_arg);
int nvme_pcie_ctrlr_cmd_create_io_sq(struct spdk_nvme_ctrlr *ctrlr,
				     struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int nvme_pcie_ctrlr_cmd_delete_io_cq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int nvme_pcie_ctrlr_cmd_delete_io_sq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg);
/*
 * [한국어]
 * admin 커맨드 래퍼 — Create/Delete I/O Completion/Submission Queue
 * (NVMe 스펙 §5.3/§5.4/§5.5/§5.6)
 * 비동기 — cb_fn으로 완료 통지
 */

int nvme_pcie_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);
void nvme_pcie_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);
/*
 * [한국어]
 * qpair connect/disconnect — 위 create/delete 시퀀스를 편의 래핑
 */

void nvme_pcie_qpair_abort_trackers(struct spdk_nvme_qpair *qpair, uint32_t dnr);
/*
 * [한국어]
 * outstanding tracker 전부 abort 처리 (reset/shutdown 시)
 * @dnr: Do Not Retry 비트 값 (완료에 반영) */

void nvme_pcie_qpair_manual_complete_tracker(struct spdk_nvme_qpair *qpair,
		struct nvme_tracker *tr, uint32_t sct, uint32_t sc, uint32_t dnr,
		bool print_on_error);
/*
 * [한국어]
 * 하드웨어 CQE 없이 수동으로 특정 tracker를 완료시킴 (abort 등)
 * @sct/@sc: 상태 코드 타입/코드
 */

void nvme_pcie_qpair_complete_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
				      struct spdk_nvme_cpl *cpl, bool print_on_error);
/*
 * [한국어]
 * 정상 경로: CQE가 도착했을 때 tracker → 원 요청 콜백 호출, tracker 반납
 */

void nvme_pcie_qpair_submit_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr);
/*
 * [한국어]
 * tracker를 SQ에 실제 기입 + (delay_cmd_submit=false이면) doorbell ring
 * 이 함수가 I/O 경로에서 "장치에 명령이 가시화되는" 지점.
 */

void nvme_pcie_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair);
void nvme_pcie_admin_qpair_destroy(struct spdk_nvme_qpair *qpair);
void nvme_pcie_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr);

int32_t nvme_pcie_qpair_process_completions(struct spdk_nvme_qpair *qpair,
		uint32_t max_completions);
/*
 * [한국어]
 * ★ I/O 경로 완료 측 핵심 함수 ★
 *
 * - CQ를 폴링하여 phase 비트가 맞는 CQE를 최대 max_completions개 처리
 * - 각 CQE에 대해: tracker 복원 → 완료 콜백 호출 → tracker 반납
 * - cq_head 갱신 후 CQ doorbell ring
 *
 * 반환: 처리한 완료 수. 음수이면 qpair 실패 표시.
 */

int nvme_pcie_qpair_destroy(struct spdk_nvme_qpair *qpair);
struct spdk_nvme_qpair *nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
		const struct spdk_nvme_io_qpair_opts *opts);
int nvme_pcie_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);

int nvme_pcie_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req);
/*
 * [한국어]
 * ★ I/O 경로 제출 측 핵심 함수 ★
 *
 * nvme_request(상위 레이어) → tracker 할당 + PRP/SGL 빌드 → SQE 기록 → doorbell ring.
 * 이 함수가 spdk_nvme_ns_cmd_read/write의 종착 경로.
 */

int nvme_pcie_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
				   struct spdk_nvme_transport_poll_group_stat **_stats);
void nvme_pcie_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
				     struct spdk_nvme_transport_poll_group_stat *stats);

struct spdk_nvme_transport_poll_group *nvme_pcie_poll_group_create(void);
int nvme_pcie_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair);
int nvme_pcie_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair);
int nvme_pcie_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			     struct spdk_nvme_qpair *qpair);
int nvme_pcie_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
				struct spdk_nvme_qpair *qpair);
int64_t nvme_pcie_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
		uint32_t completions_per_qpair,
		spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
/*
 * [한국어]
 * ★ 다중 qpair를 한 번에 폴링해 완료 처리 ★
 * 각 qpair별 최대 completions_per_qpair만큼 처리 후 총합 반환.
 * SPDK 앱이 하나의 폴러 콜백으로 여러 qpair 완료를 모으는 표준 경로.
 */
void nvme_pcie_poll_group_check_disconnected_qpairs(
	struct spdk_nvme_transport_poll_group *tgroup,
	spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
int nvme_pcie_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup);

#endif                           /* [한국어] include 가드 종료 */
