/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe over PCIe common library
 */

/*
 * [한국어 설명] PCIe/VFIO-USER 공통 NVMe 트랜스포트 구현 (nvme_pcie_common.c)
 *
 * === 파일의 역할 ===
 * SPDK NVMe 드라이버의 **PCIe hot-path 실제 구현**. nvme_transport.c의 vtable 디스패치가
 * `qpair->transport->ops.qpair_submit_request` 또는 `qpair_process_completions` 등으로
 * 위임한 최종 수신지. 이 파일이 하는 일은 한 문장으로:
 *
 *   "nvme_request를 받아 SQ[sq_tail]에 SQE를 기록하고 doorbell을 ring해서 장치에 통지하며,
 *    장치가 기록한 CQE를 phase bit로 발견하여 nvme_request로 복원·완료 콜백을 실행한다."
 *
 * 두 트랜스포트가 이 파일을 공유:
 *   - **PCIe** (nvme_pcie.c가 attach/BAR mapping 등 제공, hot-path는 이 파일)
 *   - **VFIO-USER** (nvme_vfio_user.c — 사용자 공간에서 PCIe를 에뮬레이션,
 *                    `vfio-user` 프로토콜로 타겟 프로세스와 통신)
 *   두 트랜스포트가 SQ/CQ MMIO + tracker/PRP 구조를 공유하기 때문에 이렇게 통합.
 *
 * === 핵심 자료구조 (nvme_pcie_internal.h 정의) ===
 *
 *   struct nvme_pcie_qpair (qpair의 PCIe-specific 확장):
 *     - cmd[num_entries] : Submission Queue 엔트리 배열 (64B × N, 물리 연속 메모리)
 *     - cpl[num_entries] : Completion Queue 엔트리 배열 (16B × N, 물리 연속 메모리)
 *     - tr[num_entries]  : nvme_tracker 풀 (각 in-flight SQE마다 하나씩 할당, cid로 인덱스)
 *     - sq_tail / sq_head: 호스트가 다음 write할 슬롯 / 장치가 마지막 fetch한 슬롯
 *     - cq_head          : 호스트가 다음 read할 CQ 슬롯
 *     - flags.phase      : 현재 phase bit 값 (CQ wrap할 때마다 토글)
 *     - sq_tdbl / cq_hdbl: 각각 SQ tail doorbell / CQ head doorbell의 MMIO 주소
 *     - free_tr          : 재사용 가능한 tracker LIFO 리스트
 *     - outstanding_tr   : 장치로 제출되어 CQE 기다리는 tracker 리스트
 *     - shadow_doorbell  : 선택적 shadow doorbell 쌍 (스펙 1.3+에서 MMIO 회피용)
 *
 *   struct nvme_tracker (in-flight SQE 하나에 대한 호스트 측 상태):
 *     - cid              : Command ID (SQE의 CDW0[15:0], 완료 시 장치가 돌려줌)
 *     - req              : 연관된 nvme_request* (완료 시 이걸로 호스트 콜백 호출)
 *     - u.prp / u.sgl    : PRP 리스트 또는 SGL 디스크립터 배열 (장치 DMA용)
 *     - prp_sgl_bus_addr : 위 배열의 물리 주소 (장치가 fetch)
 *     - prev / next      : outstanding 리스트 연결
 *     - meta_sgl / active_ctx: 추가 메타데이터 SGL + accel sequence 컨텍스트
 *
 * === 핵심 호출 체인 ===
 *
 * 제출 (submit):
 *   [nvme_ns_cmd.c] spdk_nvme_ns_cmd_read → ... → nvme_qpair_submit_request
 *     [nvme_qpair.c] _nvme_qpair_submit_request → nvme_transport_qpair_submit_request
 *       [nvme_transport.c] ops.qpair_submit_request (vtable 디스패치)
 *         ★ [이 파일] nvme_pcie_qpair_submit_request   ─┐
 *             │ - tracker 할당 (free_tr에서 pop)        │
 *             │ - nvme_request의 payload 타입 판정       │
 *             │ - PRP or SGL 빌더 호출:                  │  이 파일의 submit 경로
 *             │     nvme_pcie_qpair_build_prps_sgl_req   │
 *             │     nvme_pcie_qpair_build_hw_sgl_req     │
 *             │     nvme_pcie_qpair_build_contig_req     │
 *             │     nvme_pcie_prp_list_append (PRP chunk)│
 *             │ - SQE 빌드 완성                          │
 *             │ - nvme_pcie_qpair_submit_tracker         │
 *             │     - SQ[sq_tail] = SQE (memcpy)        │
 *             │     - sq_tail++ (wrap 고려)             │
 *             │     - **doorbell MMIO write** ★         │
 *             │       spdk_mmio_write_4(sq_tdbl, sq_tail)│
 *             └ 장치가 SQE fetch → 작업 수행 ...         ─┘
 *
 * 완료 (complete):
 *   [nvme_qpair.c] spdk_nvme_qpair_process_completions → nvme_transport_qpair_process_completions
 *     [nvme_transport.c] ops.qpair_process_completions (vtable)
 *       ★ [이 파일] nvme_pcie_qpair_process_completions  ─┐
 *           │ - CQ[cq_head].phase 검사                     │
 *           │ - 현재 phase와 일치하면 완료 엔트리           │
 *           │ - tracker = pqpair->tr[cid]                 │ 이 파일의 완료 경로
 *           │ - nvme_pcie_qpair_complete_tracker          │
 *           │     - nvme_complete_request(req, cpl)       │
 *           │       → req->cb_fn(cb_arg, cpl) 호스트 콜백 │
 *           │     - nvme_free_request, free_tr에 반환    │
 *           │ - cq_head++ (wrap 시 phase 토글)           │
 *           │ - CQ head doorbell MMIO write              │
 *           └                                              ─┘
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 **hot path의 끝** — NVMe 스펙 1.x/2.x의 SQ/CQ 프로토콜 + PCIe MMIO 규약 +
 * SPDK DPDK hugepage 메모리 모델이 만나는 지점. 상위(nvme_qpair.c)는 상태머신만 관리하고,
 * 실제 "1 IOPS = 1 doorbell write + 1 CQE read"의 CPU 사이클이 여기서 소비된다.
 *
 * polled-mode의 핵심 이점도 여기서 실현:
 *   - 인터럽트 없이 CQ의 phase bit를 **spin polling** (process_completions가 무한 호출됨)
 *   - 캐시 라인 1개 read로 완료 감지 가능 → 마이크로초급 레이턴시
 *   - 반면 CPU 100% 사용 — 이것이 SPDK의 근본적 트레이드오프
 *
 * === 타 모듈과의 연결 ===
 *   - `nvme_pcie_internal.h`    : struct nvme_pcie_{qpair,ctrlr,tracker} 정의 (이미 주석 완료)
 *   - `nvme_pcie.c`             : PCIe 트랜스포트 attach/BAR 매핑 (이 파일의 pctrlr->cmb 등 사용)
 *   - `nvme_vfio_user.c`        : vfio-user 트랜스포트 (pctrlr 구조체 공유, 통신만 다름)
 *   - `nvme_qpair.c`            : 상위 추상 — 이 파일의 process_completions가 nvme_complete_request 호출
 *   - `nvme_transport.c`        : vtable 디스패치 — 아래 SPDK_TRACE_REGISTER_FN 매크로가 trace 등록
 *   - DPDK `spdk_vtophys`       : 가상→물리 주소 변환 (장치가 PRP/SGL에 물리 주소만 이해)
 *
 * 공유 전역:
 *   `__thread struct nvme_pcie_ctrlr *g_thread_mmio_ctrlr` — MMIO access 직전에 세팅,
 *   BAR가 unmap된 후 접근을 SIGBUS 대신 조기 탐지하는 용도 (nvme_pcie.c의 시그널 핸들러가 확인).
 *
 * === 주요 함수/구조체 요약 ===
 *   ★★★ nvme_pcie_qpair_submit_request    — SQE 빌드 + submit_tracker (submit hot path 진입)
 *   ★★★ nvme_pcie_qpair_submit_tracker     — SQ[sq_tail]=SQE + doorbell MMIO write
 *   ★★★ nvme_pcie_qpair_process_completions — CQ phase polling + CQE 수확 + 완료 콜백
 *   ★★★ nvme_pcie_qpair_complete_tracker    — 개별 CQE 처리 + req->cb_fn 호출
 *   ★★  nvme_pcie_prp_list_append           — 4KB 경계 기반 PRP chunking
 *   ★★  nvme_pcie_qpair_build_{contig,hw_sgl,prps_sgl}_request — payload → PRP/SGL 변환
 *     nvme_pcie_qpair_construct / admin_qpair_construct — SQ/CQ + tracker 풀 할당
 *     nvme_pcie_ctrlr_cmd_{create,delete}_io_{sq,cq}  — IO 큐 생성/삭제 admin 커맨드
 *     nvme_pcie_copy_command / copy_command_mmio       — SQE byte-wise copy (CMB에서는 MMIO 안전 copy)
 *     nvme_pcie_qpair_check_timeout                    — 타임아웃 감지 훅
 *     nvme_pcie_poll_group_* (10종)                    — PCIe poll group (순차 순회 기반)
 *
 * 이 파일을 완전히 이해하면 SPDK NVMe 드라이버의 "실제 1 IOPS가 어떻게 처리되는지" 전 과정이 보인다.
 */

#include "spdk/stdinc.h"          /* [한국어] 표준 라이브러리 인클루드 (stddef, stdint, memset 등) */
#include "spdk/likely.h"          /* [한국어] spdk_likely/unlikely 분기 예측 힌트 — hot-path 성능 */
#include "spdk/string.h"          /* [한국어] 문자열 유틸 (에러 메시지 포맷) */
#include "nvme_internal.h"        /* [한국어] NVMe 내부 타입 — spdk_nvme_ctrlr/qpair, nvme_request,
                                   *         nvme_complete_request 등 이 파일이 조작하는 전역 타입 */
#include "nvme_pcie_internal.h"   /* [한국어] PCIe 특화 타입 — struct nvme_pcie_{ctrlr,qpair,tracker},
                                   *         nvme_pcie_qpair()/ctrlr() 변환 inline 헬퍼 (이전 세션 주석 완료) */
#include "spdk/trace.h"           /* [한국어] 성능 트레이스 API — 이 파일 하단에서 NVMe PCIe 추적점 등록 */

#include "spdk_internal/trace_defs.h"
                                  /* [한국어] 내부 트레이스 정의 — TRACE_GROUP_NVME_PCIE 등 매크로 상수 */

__thread struct nvme_pcie_ctrlr *g_thread_mmio_ctrlr = NULL;
                                  /* [한국어] 스레드 로컬 MMIO "in-progress" 마커.
                                   *         MMIO read/write 직전에 세팅 → 완료 시 NULL 복원.
                                   *         목적: PCIe link loss/BAR unmap 시 SIGBUS 대신 조기 감지.
                                   *         `nvme_pcie.c`의 시그널 핸들러가 이 값을 참조해 어느 컨트롤러인지 확인. */

static struct spdk_nvme_pcie_stat g_dummy_stat = {};
                                  /* [한국어] poll_group 통계 미설정 qpair의 stat 포인터 기본값.
                                   *         poll_group에 속하지 않은 qpair도 hot-path에서 `pqpair->stat->...++`
                                   *         코드가 돌아가는데, NULL 체크 분기 제거를 위해 더미 구조체 포인터 할당. */

static void nvme_pcie_fail_request_bad_vtophys(struct spdk_nvme_qpair *qpair,
		struct nvme_tracker *tr);
                                  /* [한국어] 전방 선언 — PRP/SGL 빌드 중 vtophys 실패 시 호출되는 실패 처리.
                                   *         정의부는 line ~1208에 있음. 빌드 함수들이 먼저 사용하므로 전방 선언. */

/*
 * [한국어] nvme_pcie_vtophys - 가상→물리 주소 변환 (트랜스포트 분기 래퍼).
 *
 * @ctrlr: 대상 컨트롤러 (trtype으로 PCIe vs VFIO-USER 분기)
 * @buf:   가상 주소
 * @size:  IN/OUT — 변환 가능한 연속 영역 길이 (hugepage 경계 고려)
 * @return: 장치가 DMA에 쓸 물리 주소. 실패 시 SPDK_VTOPHYS_ERROR(~0ULL).
 *
 * 동작:
 *   - PCIe: `spdk_vtophys`로 DPDK hugepage 가상→물리 조회 (프로세스 페이지 테이블 경유).
 *   - VFIO-USER: IOVA=VA 모드이므로 가상 주소를 그대로 전달 (vfio-user 프로토콜로 타겟에 전송).
 *
 * 이 함수는 PRP/SGL 빌드의 모든 페이지 변환에서 호출됨 → inline + likely 분기 예측으로 최적화.
 */
static inline uint64_t
nvme_pcie_vtophys(struct spdk_nvme_ctrlr *ctrlr, const void *buf, uint64_t *size)
{
	if (spdk_likely(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE)) {
                                  /* [한국어] PCIe 경로가 대부분 — likely 힌트로 branch prediction 유도 */
		return spdk_vtophys(buf, size);
                                  /* [한국어] DPDK 힙(hugepage)에서 가상→물리 변환. hugepage 미등록 영역이면 ERROR. */
	} else {
		/* vfio-user address translation with IOVA=VA mode */
		return (uint64_t)(uintptr_t)buf;
                                  /* [한국어] vfio-user는 IOVA=VA — 가상 주소 자체가 "IO 가상 주소"로 사용됨.
                                   *         실제 DMA는 타겟 프로세스가 대행하므로 여기서는 단순 cast. */
	}
}

/*
 * [한국어] nvme_pcie_qpair_reset - qpair 내부 카운터/phase 초기화 (트랜스포트 vtable의 qpair_reset 구현).
 *
 * 호출자: ctrlr reset 중 I/O qpair 재사용 시, 또는 disconnect→connect 재연결 시.
 * 동작: head/tail 0으로, phase bit = 1 (첫 wrap 전까지 장치가 기록할 값)로, CQ phase 필드 0으로 초기화.
 *
 * phase bit 의미 (NVMe 스펙 1.x 섹션 4.6 Completion Queue):
 *   - CQE의 status.p는 호스트가 "이 슬롯이 새 엔트리인지 vs 이전 wrap의 잔여인지" 구별하는 비트.
 *   - 장치가 한 wrap에서 1→0→1→... 토글해가며 기록. 호스트도 자기 phase를 토글하며 비교.
 *   - 초기값 1로 세팅하는 이유: 첫 wrap에서 장치가 반드시 1을 기록 (0이면 호스트는 못 본 엔트리로 간주).
 *   - CQ 슬롯 전체를 0으로 초기화 → 장치가 1을 쓰면 즉시 구별 가능.
 */
int
nvme_pcie_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
                                  /* [한국어] spdk_nvme_qpair → nvme_pcie_qpair 다운캐스트 (container_of 패턴) */
	uint32_t i;
                                  /* [한국어] CQ 초기화 루프 인덱스 */

	/* all head/tail vals are set to 0 */
	pqpair->last_sq_tail = pqpair->sq_tail = pqpair->sq_head = pqpair->cq_head = 0;
                                  /* [한국어] 4개 카운터 동시 초기화:
                                   *         - sq_tail: 호스트가 다음 write할 SQ 슬롯 (도어벨에 기록)
                                   *         - sq_head: 장치가 마지막 fetch한 SQ 슬롯 (CQE의 sqhd로 전달받음)
                                   *         - cq_head: 호스트가 다음 read할 CQ 슬롯
                                   *         - last_sq_tail: 마지막 도어벨 write 직후의 sq_tail (batching 최적화용) */

	/*
	 * First time through the completion queue, HW will set phase
	 *  bit on completions to 1.  So set this to 1 here, indicating
	 *  we're looking for a 1 to know which entries have completed.
	 *  we'll toggle the bit each time when the completion queue
	 *  rolls over.
	 */
	pqpair->flags.phase = 1;
                                  /* [한국어] 호스트 현재 phase = 1 — 다음 기대 값.
                                   *         장치가 첫 wrap에서 1을 쓰므로 일치 = 완료 엔트리로 해석. */
	for (i = 0; i < pqpair->num_entries; i++) {
		pqpair->cpl[i].status.p = 0;
                                  /* [한국어] CQ 전 슬롯의 phase 비트를 0으로 초기화.
                                   *         장치가 p=1을 쓸 때까지 이 슬롯은 "미완료"로 보이게 함. */
	}

	return 0;
                                  /* [한국어] 초기화는 실패 없음 */
}

/*
 * [한국어] nvme_pcie_qpair_get_fd - interrupt-mode 이벤트 fd 조회 (트랜스포트 vtable의 qpair_get_fd 구현).
 *
 * @opts: NULL이면 fd만 반환, 아니면 spdk_event_handler_opts 필드도 채움 (ABI 호환 opts_size 방식)
 * @return: VFIO eventfd — spdk_fd_group_add로 epoll에 등록하면 인터럽트 수신 시 깨어남
 *
 * 동작:
 *   1) ctrlr->opts.enable_interrupts 플래그가 false면 -1 (polled-mode 전용)
 *   2) VFIO의 interrupt eventfd를 qpair->id 인덱스로 조회 (MSI-X vector per qpair)
 *   3) opts가 있으면 fd_type=EVENTFD 설정 (일반 file descriptor와 구별)
 *
 * 이 fd를 얻어야 interrupt-mode SPDK thread가 polling 없이 완료 이벤트를 기다릴 수 있다.
 */
int
nvme_pcie_qpair_get_fd(struct spdk_nvme_qpair *qpair, struct spdk_event_handler_opts *opts)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
                                  /* [한국어] qpair의 소속 컨트롤러 */
	struct spdk_pci_device *devhandle = nvme_ctrlr_proc_get_devhandle(ctrlr);
                                  /* [한국어] DPDK PCI 장치 핸들 (프로세스별) — VFIO 인터럽트 fd 접근에 필요 */

	assert(devhandle != NULL);
                                  /* [한국어] ctrlr이 살아 있으면 devhandle도 반드시 존재 */
	if (!ctrlr->opts.enable_interrupts) {
                                  /* [한국어] interrupt 모드 미설정 — polled 전용이므로 fd 불필요 */
		return -1;
	}

	if (!opts) {
                                  /* [한국어] 옵션 구조체 없으면 단순히 fd만 반환 */
		return spdk_pci_device_get_interrupt_efd_by_index(devhandle, qpair->id);
                                  /* [한국어] VFIO MSI-X vector #qpair_id의 eventfd (per-qpair) */
	}

	if (!SPDK_FIELD_VALID(opts, fd_type, opts->opts_size)) {
                                  /* [한국어] ABI 호환 검사 — opts_size가 fd_type 필드를 포함 못 하면 버전 불일치 */
		return -EINVAL;
	}

	spdk_fd_group_get_default_event_handler_opts(opts, opts->opts_size);
                                  /* [한국어] spdk_event_handler_opts 기본값으로 채움 (epoll events 등) */
	opts->fd_type = SPDK_FD_TYPE_EVENTFD;
                                  /* [한국어] 이 fd는 eventfd (일반 file이 아닌 eventfd) — fd_group이 write 후
                                   *         자동 read로 reset 처리 */

	return spdk_pci_device_get_interrupt_efd_by_index(devhandle, qpair->id);
                                  /* [한국어] 최종 fd 반환 */
}

/*
 * [한국어] nvme_qpair_construct_tracker - 단일 tracker 초기 세팅.
 *
 * @tr:        초기화할 tracker
 * @cid:       이 tracker에 고유 부여될 Command ID (tracker 배열의 인덱스와 동일)
 * @phys_addr: 이 tracker의 물리 주소 (spdk_vtophys로 미리 계산됨)
 *
 * 중요한 필드 설정:
 *   - prp_sgl_bus_addr: tracker 구조체 내부의 u.prp/u.sgl 배열의 **물리 주소**.
 *     offsetof(nvme_tracker, u.prp) 오프셋을 더해 SQE의 PRP2/SGL1 필드가 가리킬 주소 계산.
 *     장치가 이 주소로 PRP 리스트 또는 SGL 디스크립터를 fetch.
 *   - cid: 장치 CQE의 cid 필드로 돌아올 값 — 완료 시 tracker 복원 키.
 *   - req: 아직 요청 미할당 상태로 NULL.
 */
static void
nvme_qpair_construct_tracker(struct nvme_tracker *tr, uint16_t cid, uint64_t phys_addr)
{
	tr->prp_sgl_bus_addr = phys_addr + offsetof(struct nvme_tracker, u.prp);
                                  /* [한국어] 장치가 PRP 리스트 읽을 위치의 물리 주소.
                                   *         SQE 빌드 시 cmd->dptr.prp.prp2 또는 cmd->dptr.sgl1.address에 들어감. */
	tr->cid = cid;
                                  /* [한국어] 고정 cid — SQE 빌드할 때 cmd->cid로 복사, 완료 시 이 값으로 tracker 조회 */
	tr->req = NULL;
                                  /* [한국어] 아직 요청 없음 — submit 시 채워짐 */
}

/*
 * [한국어] nvme_pcie_ctrlr_alloc_cmb - CMB(Controller Memory Buffer) 영역에서 할당.
 *
 * @size:      할당할 바이트 수
 * @alignment: 정렬 제약 (1KB/4KB 등)
 * @phys_addr: OUT — 할당된 영역의 장치 측 물리 주소 (BAR 기반)
 * @return:    할당된 가상 주소, 실패 시 NULL
 *
 * CMB 사용처: SQ를 CMB에 배치하면 호스트 SQE write → CMB 직접 기록 → 장치가 내부 페치
 *             → PCIe round-trip 제거로 레이턴시 감소. 대표 최적화.
 *
 * 할당 정책: bump allocator (current_offset를 증가시키며 연속 할당, free 없음).
 *           NVMe 드라이버 수명 동안 할당만 하므로 해제 로직 불필요.
 *
 * mem_register_addr 체크: 사용자가 CMB 전체를 "데이터 영역"으로 매핑해 쓰고 있으면
 * 내부 SQ 용도 할당 금지 (충돌 방지).
 */
static void *
nvme_pcie_ctrlr_alloc_cmb(struct spdk_nvme_ctrlr *ctrlr, uint64_t size, uint64_t alignment,
			  uint64_t *phys_addr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
                                  /* [한국어] ctrlr → nvme_pcie_ctrlr 다운캐스트 (cmb 정보 접근) */
	uintptr_t addr;
                                  /* [한국어] 할당 예정 가상 주소 */

	if (pctrlr->cmb.mem_register_addr != NULL) {
		/* BAR is mapped for data */
                                  /* [한국어] 사용자가 CMB를 데이터 전용으로 매핑함 — 내부 SQ 할당 금지 */
		return NULL;
	}

	addr = (uintptr_t)pctrlr->cmb.bar_va + pctrlr->cmb.current_offset;
                                  /* [한국어] 현재 bump 위치 = BAR 가상 주소 + 다음 빈 offset */
	addr = (addr + (alignment - 1)) & ~(alignment - 1);
                                  /* [한국어] alignment 올림 (bit trick) — (alignment-1)을 더하고 하위 비트 클리어 */

	/* CMB may only consume part of the BAR, calculate accordingly */
	if (addr + size > ((uintptr_t)pctrlr->cmb.bar_va + pctrlr->cmb.size)) {
                                  /* [한국어] BAR의 CMB 부분 범위 초과 검사 */
		NVME_CTRLR_ERRLOG(ctrlr, "Tried to allocate past valid CMB range!\n");
		return NULL;
	}
	*phys_addr = pctrlr->cmb.bar_pa + addr - (uintptr_t)pctrlr->cmb.bar_va;
                                  /* [한국어] 장치 측 물리 주소 = BAR 물리 + 가상 offset */

	pctrlr->cmb.current_offset = (addr + size) - (uintptr_t)pctrlr->cmb.bar_va;
                                  /* [한국어] 다음 할당을 위해 offset 진행 */

	return (void *)addr;
                                  /* [한국어] 가상 주소 반환 — 호출자가 SQE 직접 write 가능 */
}

/*
 * [한국어] ★★ nvme_pcie_qpair_construct - PCIe qpair의 SQ/CQ/tracker 메모리 풀 할당 ★★
 *
 * @qpair: nvme_qpair_init이 이미 호출되어 공통 필드가 초기화된 qpair
 * @opts:  사용자 정의 SQ/CQ 주소 지정 등 (NULL이면 드라이버가 자동 할당)
 * @return: 0=성공, -ENOMEM/-EFAULT=메모리 실패
 *
 * 이 함수가 완료하면 다음 메모리 구조가 준비됨:
 *
 *   pqpair->cmd[num_entries]  — Submission Queue (hugepage, DMA-able)
 *   pqpair->cpl[num_entries]  — Completion Queue (hugepage, DMA-able)
 *   pqpair->tr [num_trackers] — tracker 풀 (in-flight SQE 상태, free_tr 리스트 연결)
 *   pqpair->sq_tdbl           — SQ tail doorbell MMIO 주소
 *   pqpair->cq_hdbl           — CQ head doorbell MMIO 주소
 *
 * 메모리 할당 정책:
 *   - CQ: hugepage에 spdk_zmalloc (num_entries * 16B 정렬 round-up)
 *   - SQ: CMB 옵션이 있으면 CMB에, 없으면 hugepage
 *   - tracker: 단일 할당으로 전체 배열 (배열 인덱싱 가능 + PRP 리스트가 4KB 경계 횡단 안 하게 padding)
 *
 * num_trackers 계산:
 *   CQ 크기의 3/4만 tracker로 사용 — 나머지 1/4은 wrap-around 방지용 여유.
 *   (CQ가 꽉 찬 상태로 wrap하면 장치가 다음 write 자리를 찾지 못함)
 *
 * admin qpair는 SPDK_MALLOC_SHARE로 할당 — multi-process 간 공유.
 */
int
nvme_pcie_qpair_construct(struct spdk_nvme_qpair *qpair,
			  const struct spdk_nvme_io_qpair_opts *opts)
{
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
                                  /* [한국어] 소속 컨트롤러 */
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(ctrlr);
                                  /* [한국어] PCIe 특화 ctrlr — doorbell_base, CMB 정보 접근 */
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
                                  /* [한국어] PCIe 특화 qpair — SQ/CQ/tracker 필드 조작 */
	struct nvme_tracker	*tr;
                                  /* [한국어] 초기화 루프 반복자 */
	uint16_t		i;
                                  /* [한국어] tracker 루프 인덱스 = 고정 cid 값 */
	uint16_t		num_trackers;
                                  /* [한국어] 실제 tracker 개수 (CQ 크기의 3/4) */
	size_t			page_align = sysconf(_SC_PAGESIZE);
                                  /* [한국어] 시스템 페이지 크기 (보통 4KB) — SQ/CQ 정렬 기준 */
	size_t			queue_align, queue_len;
                                  /* [한국어] SQ/CQ 크기와 정렬 계산 임시 변수 */
	uint32_t                flags = SPDK_MALLOC_DMA;
                                  /* [한국어] DMA 가능한 hugepage에서 할당 (기본) */
	int32_t			numa_id;
                                  /* [한국어] NUMA ID — CQ를 특정 노드에 배치해 장치 DMA 성능 향상 */
	uint64_t		sq_paddr = 0;
                                  /* [한국어] 사용자 제공 SQ 물리 주소 (0=자동) */
	uint64_t		cq_paddr = 0;
                                  /* [한국어] 사용자 제공 CQ 물리 주소 (0=자동) */

	if (opts) {
                                  /* [한국어] 사용자가 제공한 SQ/CQ 메모리를 재사용하는 경로 (advanced use) */
		pqpair->sq_vaddr = opts->sq.vaddr;
                                  /* [한국어] 사용자 제공 SQ 가상 주소 (예: GPU BAR1에 SQ 배치) */
		pqpair->cq_vaddr = opts->cq.vaddr;
                                  /* [한국어] 사용자 제공 CQ 가상 주소 */
		pqpair->flags.disable_pcie_sgl_merge = opts->disable_pcie_sgl_merge;
                                  /* [한국어] SGL merge 최적화 비활성 옵션 (디버그용) */
		sq_paddr = opts->sq.paddr;
                                  /* [한국어] 사용자가 직접 vtophys한 물리 주소 (SPDK가 관리 못 하는 메모리) */
		cq_paddr = opts->cq.paddr;
	}

	pqpair->retry_count = ctrlr->opts.transport_retry_count;
                                  /* [한국어] retry-able 에러 시 재시도 횟수 */

	/*
	 * Limit the maximum number of completions to return per call to prevent wraparound,
	 * and calculate how many trackers can be submitted at once without overflowing the
	 * completion queue.
	 */
                                  /* [한국어] CQ wrap-around 방지를 위한 계산:
                                   *         - max_completions_cap: process_completions 한 호출에 수확할 최대 개수
                                   *           (기본 num_entries/4로 완충 지대 확보)
                                   *         - num_trackers: 동시 outstanding 허용 수
                                   *           (CQ 크기 - 최대 완료 처리 수 → CQ가 꽉 차는 일 방지) */
	pqpair->max_completions_cap = pqpair->num_entries / 4;
                                  /* [한국어] CQ 크기의 1/4 기본 */
	pqpair->max_completions_cap = spdk_max(pqpair->max_completions_cap, NVME_MIN_COMPLETIONS);
                                  /* [한국어] 최솟값 보장 (너무 작으면 latency 증가) */
	pqpair->max_completions_cap = spdk_min(pqpair->max_completions_cap, NVME_MAX_COMPLETIONS);
                                  /* [한국어] 최댓값 제한 (너무 크면 한 호출에 과도한 CPU 소비) */
	num_trackers = pqpair->num_entries - pqpair->max_completions_cap;
                                  /* [한국어] CQ 크기 - 완료 처리 여유 = 실제 허용 outstanding 개수 */

	NVME_QPAIR_INFOLOG(qpair, "max_completions_cap = %" PRIu16 " num_trackers = %" PRIu16 "\n",
			   pqpair->max_completions_cap, num_trackers);

	assert(num_trackers != 0);
                                  /* [한국어] num_entries가 너무 작으면 tracker가 0개 될 수 있음 — 방어 */

	pqpair->sq_in_cmb = false;
                                  /* [한국어] SQ가 CMB에 있는지 플래그 — 기본 false, CMB 할당 성공 시 true */

	if (nvme_qpair_is_admin_queue(&pqpair->qpair)) {
		flags |= SPDK_MALLOC_SHARE;
                                  /* [한국어] admin qpair는 multi-process 공유 — SHARE 플래그로 hugepage를 모든 프로세스가 mmap 가능하게 */
	}

	/* cmd and cpl rings must be aligned on page size boundaries. */
	if (ctrlr->opts.use_cmb_sqs) {
                                  /* [한국어] CMB SQ 최적화 시도 */
		pqpair->cmd = nvme_pcie_ctrlr_alloc_cmb(ctrlr, pqpair->num_entries * sizeof(struct spdk_nvme_cmd),
							page_align, &pqpair->cmd_bus_addr);
                                  /* [한국어] CMB 영역에 SQ 배치 시도 — 성공 시 호스트→장치 SQE fetch round-trip 제거 */
		if (pqpair->cmd != NULL) {
			pqpair->sq_in_cmb = true;
                                  /* [한국어] CMB SQ 사용 플래그 set — submit 시 MMIO 안전 copy로 write */
		}
	}

	if (pqpair->sq_in_cmb == false) {
                                  /* [한국어] CMB 미사용 경로 — 호스트 hugepage에 SQ 할당 */
		if (pqpair->sq_vaddr) {
                                  /* [한국어] 사용자 제공 SQ 주소 사용 */
			pqpair->cmd = pqpair->sq_vaddr;
		} else {
			/* To ensure physical address contiguity we make each ring occupy
			 * a single hugepage only. See MAX_IO_QUEUE_ENTRIES.
			 */
                                  /* [한국어] 물리 주소 연속성 보장을 위해 한 개 hugepage에 맞도록 정렬 올림.
                                   *         MAX_IO_QUEUE_ENTRIES가 hugepage(2MB)에 맞도록 제한되어 있음. */
			queue_len = pqpair->num_entries * sizeof(struct spdk_nvme_cmd);
                                  /* [한국어] 바이트 단위 SQ 크기 (num_entries × 64B) */
			queue_align = spdk_max(spdk_align32pow2(queue_len), page_align);
                                  /* [한국어] 2의 거듭제곱 올림 + 페이지 정렬 (둘 중 큰 값) — hugepage 단일 매핑 보장 */
			pqpair->cmd = spdk_zmalloc(queue_len, queue_align, NULL, SPDK_ENV_NUMA_ID_ANY, flags);
                                  /* [한국어] DMA-able hugepage 할당, 0 초기화 */
			if (pqpair->cmd == NULL) {
				NVME_QPAIR_ERRLOG(qpair, "alloc qpair_cmd failed\n");
				return -ENOMEM;
			}
		}
		if (sq_paddr) {
                                  /* [한국어] 사용자 제공 물리 주소 사용 (vtophys 스킵) */
			assert(pqpair->sq_vaddr != NULL);
                                  /* [한국어] paddr만 주고 vaddr 안 주는 건 금지 */
			pqpair->cmd_bus_addr = sq_paddr;
		} else {
                                  /* [한국어] 자체 할당한 SQ의 물리 주소 계산 */
			pqpair->cmd_bus_addr = nvme_pcie_vtophys(ctrlr, pqpair->cmd, NULL);
			if (pqpair->cmd_bus_addr == SPDK_VTOPHYS_ERROR) {
                                  /* [한국어] hugepage 등록 실패 — DPDK 초기화 이슈 */
				NVME_QPAIR_ERRLOG(qpair, "spdk_vtophys(pqpair->cmd) failed\n");
				return -EFAULT;
			}
		}
	}

	if (pqpair->cq_vaddr) {
                                  /* [한국어] 사용자 제공 CQ 주소 사용 */
		pqpair->cpl = pqpair->cq_vaddr;
	} else {
                                  /* [한국어] CQ 자체 할당 */
		queue_len = pqpair->num_entries * sizeof(struct spdk_nvme_cpl);
                                  /* [한국어] num_entries × 16B */
		queue_align = spdk_max(spdk_align32pow2(queue_len), page_align);
		numa_id = spdk_nvme_ctrlr_get_numa_id(ctrlr);
                                  /* [한국어] 컨트롤러의 NUMA 노드 — CQ를 같은 노드에 두면 장치 DMA 성능 향상 */
		pqpair->cpl = spdk_zmalloc(queue_len, queue_align, NULL, numa_id, flags);
                                  /* [한국어] CQ 할당 — phase bit 초기값 0 보장 중요 (zmalloc으로 자동) */
		if (pqpair->cpl == NULL) {
			NVME_QPAIR_ERRLOG(qpair, "alloc qpair_cpl failed\n");
			return -ENOMEM;
		}
	}
	if (cq_paddr) {
		assert(pqpair->cq_vaddr != NULL);
		pqpair->cpl_bus_addr = cq_paddr;
                                  /* [한국어] 사용자 제공 CQ 물리 주소 */
	} else {
		pqpair->cpl_bus_addr =  nvme_pcie_vtophys(ctrlr, pqpair->cpl, NULL);
                                  /* [한국어] 자체 할당 CQ의 물리 주소 */
		if (pqpair->cpl_bus_addr == SPDK_VTOPHYS_ERROR) {
			NVME_QPAIR_ERRLOG(qpair, "spdk_vtophys(pqpair->cpl) failed\n");
			return -EFAULT;
		}
	}

	pqpair->sq_tdbl = pctrlr->doorbell_base + (2 * qpair->id + 0) * pctrlr->doorbell_stride_u32;
                                  /* [한국어] ★ SQ tail doorbell MMIO 주소 계산:
                                   *         doorbell_base = BAR0의 0x1000 오프셋 (NVMe 스펙 DBS)
                                   *         stride_u32 = DSTRD로 계산된 stride (보통 1=4B, 고성능 장치는 더 큼)
                                   *         (2*qid+0)*stride = SQ[qid] tail doorbell 위치 */
	pqpair->cq_hdbl = pctrlr->doorbell_base + (2 * qpair->id + 1) * pctrlr->doorbell_stride_u32;
                                  /* [한국어] ★ CQ head doorbell MMIO 주소:
                                   *         (2*qid+1)*stride = CQ[qid] head doorbell 위치
                                   *         SQ doorbell 바로 다음 4B */

	/*
	 * Reserve space for all of the trackers in a single allocation.
	 *   struct nvme_tracker must be padded so that its size is already a power of 2.
	 *   This ensures the PRP list embedded in the nvme_tracker object will not span a
	 *   4KB boundary, while allowing access to trackers in tr[] via normal array indexing.
	 */
                                  /* [한국어] 트래커 풀 전체를 단일 할당:
                                   *         - 각 tracker는 2의 거듭제곱 크기로 padding되어 있음
                                   *         - 이유 1: 4KB 경계 횡단 없는 PRP 리스트 (NVMe 스펙 요구)
                                   *         - 이유 2: 배열 인덱싱만으로 cid→tracker 조회 가능 */
	pqpair->tr = spdk_zmalloc(num_trackers * sizeof(*tr), sizeof(*tr), NULL,
				  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
                                  /* [한국어] tracker 배열 할당 (sizeof(*tr) 정렬 = 2^N 정렬 보장).
                                   *         SHARE: admin/IO 상관없이 공유 — multi-process가 완료 보고 시 tracker 접근 가능 */
	if (pqpair->tr == NULL) {
		NVME_QPAIR_ERRLOG(qpair, "nvme_tr failed\n");
		return -ENOMEM;
	}

	TAILQ_INIT(&pqpair->free_tr);
                                  /* [한국어] 사용 가능한 tracker LIFO — submit 시 pop */
	TAILQ_INIT(&pqpair->outstanding_tr);
                                  /* [한국어] 장치로 제출된 tracker 리스트 — 완료 시 제거 */
	pqpair->qpair.queue_depth = 0;
                                  /* [한국어] QD 카운터 초기화 */

	for (i = 0; i < num_trackers; i++) {
                                  /* [한국어] 각 tracker를 초기화 + free_tr 풀에 LIFO로 삽입 */
		tr = &pqpair->tr[i];
		nvme_qpair_construct_tracker(tr, i, nvme_pcie_vtophys(ctrlr, tr, NULL));
                                  /* [한국어] i를 cid로 고정 배정 + 물리 주소 계산 */
		TAILQ_INSERT_HEAD(&pqpair->free_tr, tr, tq_list);
                                  /* [한국어] HEAD 삽입 (LIFO) — 최근 사용 tracker가 다시 캐시에 있을 확률 높임 */
	}

	nvme_pcie_qpair_reset(qpair);
                                  /* [한국어] head/tail/phase 초기화 */

	return 0;
}

/*
 * [한국어] nvme_pcie_ctrlr_construct_admin_qpair - admin qpair(qid=0) 할당 + 초기화.
 *
 * @num_entries: admin queue 엔트리 수 (보통 32)
 * @return: 0=성공, -ENOMEM=실패
 *
 * 이 함수는 controller construct 단계에서 한 번 호출됨.
 * I/O qpair와 달리 admin은 SPDK_MALLOC_SHARE로 할당되어 primary/secondary 공유.
 *
 * 호출 순서:
 *   1) nvme_pcie_qpair 구조체 자체 할당 (hugepage, SHARE)
 *   2) 공통 qpair 필드 초기화 (qid=0, URGENT prio, sync connect)
 *   3) stat 구조체 할당 (공유 hugepage — 다른 프로세스가 카운터 읽을 수 있게)
 *   4) nvme_pcie_qpair_construct로 SQ/CQ/tracker 실제 할당 진행
 */
int
nvme_pcie_ctrlr_construct_admin_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t num_entries)
{
	struct nvme_pcie_qpair *pqpair;
                                  /* [한국어] 할당할 PCIe qpair */
	int rc;
                                  /* [한국어] 에러 코드 */

	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
                                  /* [한국어] admin qpair 자체 구조체 할당 (64B 정렬 = 캐시라인)
                                   *         SHARE: admin은 primary/secondary가 동일 주소로 접근 필요 */
	if (pqpair == NULL) {
		return -ENOMEM;
	}

	pqpair->num_entries = num_entries;
                                  /* [한국어] SQ/CQ 크기 */
	pqpair->flags.delay_cmd_submit = 0;
                                  /* [한국어] admin은 batching 비활성 — 즉시 제출 */
	pqpair->pcie_state = NVME_PCIE_QPAIR_READY;
                                  /* [한국어] PCIe qpair 상태 = READY (장치 Create IO Q admin cmd가 필요 없는 qid=0이므로) */

	ctrlr->adminq = &pqpair->qpair;
                                  /* [한국어] ctrlr의 adminq 포인터 세팅 — 이후 모든 admin 명령 여기로 */

	rc = nvme_qpair_init(ctrlr->adminq,
			     0, /* qpair ID */
			     ctrlr,
			     SPDK_NVME_QPRIO_URGENT,
			     num_entries,
			     false);
                                  /* [한국어] 공통 qpair 초기화 — qid=0, URGENT 우선순위, async=false (동기 connect) */
	if (rc != 0) {
		return rc;
	}

	pqpair->stat = spdk_zmalloc(sizeof(*pqpair->stat), 64, NULL, SPDK_ENV_NUMA_ID_ANY,
				    SPDK_MALLOC_SHARE);
                                  /* [한국어] admin qpair의 통계 구조체 할당 (공유 hugepage)
                                   *         I/O qpair는 poll_group이 할당하지만 admin은 자체 관리 */
	if (!pqpair->stat) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate admin qpair statistics\n");
		return -ENOMEM;
	}

	return nvme_pcie_qpair_construct(ctrlr->adminq, NULL);
                                  /* [한국어] SQ/CQ/tracker 실제 할당 (opts=NULL: 드라이버 기본) */
}

/**
 * Note: the ctrlr_lock must be held when calling this function.
 */
/*
 * [한국어] nvme_pcie_qpair_insert_pending_admin_request - 외부 프로세스의 admin 완료를 보류.
 *
 * multi-process 시나리오: primary 프로세스가 admin 큐를 polling하면서, **secondary가 제출한**
 * admin 요청의 CQE를 먼저 발견할 수 있다. 이 경우 primary가 CQE를 즉시 처리하면 안 되고
 * (secondary의 cb_fn 함수 주소가 primary 주소 공간에서는 무효),
 * 해당 secondary 프로세스의 active_reqs 큐에 넣어 secondary가 직접 처리하게 한다.
 *
 * ctrlr_lock 요구: active_proc의 active_reqs 리스트가 프로세스간 공유.
 */
void
nvme_pcie_qpair_insert_pending_admin_request(struct spdk_nvme_qpair *qpair,
		struct nvme_request *req, struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr		*ctrlr = qpair->ctrlr;
                                  /* [한국어] admin 큐 소유 ctrlr */
	struct nvme_request		*active_req = req;
                                  /* [한국어] 지연 완료할 요청 */
	struct spdk_nvme_ctrlr_process	*active_proc;
                                  /* [한국어] 이 요청을 제출한 프로세스의 컨텍스트 */

	/*
	 * The admin request is from another process. Move to the per
	 *  process list for that process to handle it later.
	 */
	assert(nvme_qpair_is_admin_queue(qpair));
                                  /* [한국어] admin queue에서만 호출 */
	assert(active_req->pid != getpid());
                                  /* [한국어] 내 프로세스가 제출한 요청이면 여기 오면 안 됨 (보통 경로로 처리) */

	active_proc = nvme_ctrlr_get_process(ctrlr, active_req->pid);
                                  /* [한국어] pid로 프로세스 컨텍스트 조회 */
	if (active_proc) {
		/* Save the original completion information */
		memcpy(&active_req->cpl, cpl, sizeof(*cpl));
                                  /* [한국어] CQE를 request에 복사 보관 — 해당 프로세스가 나중에 사용 */
		STAILQ_INSERT_TAIL(&active_proc->active_reqs, active_req, stailq);
                                  /* [한국어] 그 프로세스의 완료 대기열에 추가 */
	} else {
                                  /* [한국어] 요청한 프로세스가 이미 종료된 경우 — 요청을 drop */
		NVME_CTRLR_ERRLOG(ctrlr, "The owning process (pid %d) is not found. Dropping the request.\n",
				  active_req->pid);
		nvme_cleanup_user_req(active_req);
                                  /* [한국어] user_copy 버퍼 등 정리 */
		nvme_free_request(active_req);
                                  /* [한국어] request 풀로 반환 */
	}
}

/**
 * Note: the ctrlr_lock must be held when calling this function.
 */
/*
 * [한국어] nvme_pcie_qpair_complete_pending_admin_request - 현재 프로세스의 보류된 admin 완료 처리.
 *
 * 위의 insert와 쌍 — 현재 프로세스가 자기 active_reqs 큐의 요청들을 complete 처리.
 *
 * 호출자: `nvme_pcie_qpair_process_completions`의 admin 큐 완료 후 — 다른 프로세스에서 insert해준
 * 내 프로세스 요청들을 이 프로세스가 마무리.
 */
void
nvme_pcie_qpair_complete_pending_admin_request(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr		*ctrlr = qpair->ctrlr;
	struct nvme_request		*req, *tmp_req;
                                  /* [한국어] FOREACH_SAFE 반복자 */
	pid_t				pid = getpid();
                                  /* [한국어] 현재 프로세스 pid */
	struct spdk_nvme_ctrlr_process	*proc;

	/*
	 * Check whether there is any pending admin request from
	 * other active processes.
	 */
	assert(nvme_qpair_is_admin_queue(qpair));

	proc = nvme_ctrlr_get_current_process(ctrlr);
                                  /* [한국어] 현재 프로세스의 컨텍스트 조회 */
	if (!proc) {
                                  /* [한국어] 프로세스 미등록 — 프로그래머 실수 */
		NVME_CTRLR_ERRLOG(ctrlr, "the active process (pid %d) is not found for this controller.\n", pid);
		assert(proc);
		return;
	}

	STAILQ_FOREACH_SAFE(req, &proc->active_reqs, stailq, tmp_req) {
                                  /* [한국어] 내 프로세스의 보류 완료 리스트 순회 */
		STAILQ_REMOVE(&proc->active_reqs, req, nvme_request, stailq);

		assert(req->pid == pid);
                                  /* [한국어] insert 시 다른 프로세스가 맞게 분류했는지 검증 */

		nvme_complete_request(req->cb_fn, req->cb_arg, qpair, req, &req->cpl);
                                  /* [한국어] 공용 완료 경로 — 원본 CQE(req->cpl)로 cb_fn 실행 */
	}
}

/*
 * [한국어] ★ nvme_pcie_ctrlr_cmd_create_io_cq - Create IO CQ admin 명령 제출.
 *
 * I/O qpair 생성의 첫 단계 (CQ 먼저, 그다음 SQ).
 * NVMe 스펙 Figure 149 "Create I/O Completion Queue" 참조.
 *
 * CDW10[15:0]  = QID (새 CQ ID)
 * CDW10[31:16] = QSIZE (엔트리 수 - 1, 0-based)
 * CDW11.PC  = 1 (Physically Contiguous — PRP1이 CQ 물리 주소)
 * CDW11.IEN = interrupt enable (opts.enable_interrupts)
 * CDW11.IV  = interrupt vector (IEN=1이면 설정) — qpair id와 동일하게 매핑
 * DPTR.PRP1 = CQ 버퍼 물리 주소
 */
int
nvme_pcie_ctrlr_cmd_create_io_cq(struct spdk_nvme_ctrlr *ctrlr,
				 struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn,
				 void *cb_arg)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(io_que);
                                  /* [한국어] 생성할 IO 큐의 PCIe 정보 (CQ 물리 주소, 크기 등) */
	struct nvme_request *req;
                                  /* [한국어] admin 명령 제출용 request */
	struct spdk_nvme_cmd *cmd;
                                  /* [한국어] SQE 포인터 */
	bool ien = ctrlr->opts.enable_interrupts;
                                  /* [한국어] interrupt enable 옵션 캐시 */

	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
                                  /* [한국어] payload 없는 request — admin 명령은 PRP에 CQ 주소 직접 삽입 */
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
                                  /* [한국어] SQE */
	cmd->opc = SPDK_NVME_OPC_CREATE_IO_CQ;
                                  /* [한국어] opcode 0x05 */

	cmd->cdw10_bits.create_io_q.qid = io_que->id;
                                  /* [한국어] 생성할 CQ의 ID */
	cmd->cdw10_bits.create_io_q.qsize = pqpair->num_entries - 1;
                                  /* [한국어] 크기 0-based (NVMe 스펙 관례) */

	cmd->cdw11_bits.create_io_cq.pc = 1;
                                  /* [한국어] CQ가 물리적으로 연속 — 단일 PRP1로 전체 주소 표현 가능 */
	if (ien) {
		cmd->cdw11_bits.create_io_cq.ien = 1;
                                  /* [한국어] 인터럽트 enable — MSI-X vector 설정 필요 */
		/* The interrupt vector offset starts from 1. We directly map the
		 * queue id to interrupt vector.
		 */
		cmd->cdw11_bits.create_io_cq.iv = io_que->id;
                                  /* [한국어] IV(Interrupt Vector) = qpair id — 1:1 매핑 */
	}

	cmd->dptr.prp.prp1 = pqpair->cpl_bus_addr;
                                  /* [한국어] CQ 물리 주소 — 장치가 여기에 CQE를 기록 */

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin 큐로 제출 → 완료 시 cb_fn=create_cq_cb 호출됨 */
}

/*
 * [한국어] ★ nvme_pcie_ctrlr_cmd_create_io_sq - Create IO SQ admin 명령 제출.
 *
 * CQ 생성 성공 후 이 함수로 SQ 생성. CQID로 기존 CQ 연결.
 * NVMe 스펙 Figure 146 "Create I/O Submission Queue" 참조.
 *
 * CDW11.QPRIO = qpair 우선순위 (URGENT/HIGH/MEDIUM/LOW) — WRR arbitration 시 사용
 * CDW11.CQID  = 이 SQ와 연결될 CQ의 ID (보통 같은 ID)
 */
int
nvme_pcie_ctrlr_cmd_create_io_sq(struct spdk_nvme_ctrlr *ctrlr,
				 struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(io_que);
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_CREATE_IO_SQ;
                                  /* [한국어] opcode 0x01 */

	cmd->cdw10_bits.create_io_q.qid = io_que->id;
                                  /* [한국어] 새 SQ의 ID (CQ ID와 동일하게 권장) */
	cmd->cdw10_bits.create_io_q.qsize = pqpair->num_entries - 1;
                                  /* [한국어] 0-based */
	cmd->cdw11_bits.create_io_sq.pc = 1;
                                  /* [한국어] 물리적 연속 SQ */
	cmd->cdw11_bits.create_io_sq.qprio = io_que->qprio;
                                  /* [한국어] WRR 우선순위 (컨트롤러가 WRR arbitration 지원 시 사용) */
	cmd->cdw11_bits.create_io_sq.cqid = io_que->id;
                                  /* [한국어] 이 SQ의 완료를 받을 CQ ID */
	cmd->dptr.prp.prp1 = pqpair->cmd_bus_addr;
                                  /* [한국어] SQ 물리 주소 — 장치가 여기서 SQE fetch */

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

/*
 * [한국어] nvme_pcie_ctrlr_cmd_delete_io_cq - Delete IO CQ admin 명령 제출 (qpair 해제).
 *
 * CQ는 SQ가 모두 해제된 후에만 삭제 가능 (NVMe 스펙 제약).
 * CDW10[15:0] = 삭제할 CQ의 QID.
 */
int
nvme_pcie_ctrlr_cmd_delete_io_cq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_DELETE_IO_CQ;
                                  /* [한국어] opcode 0x04 */
	cmd->cdw10_bits.delete_io_q.qid = qpair->id;
                                  /* [한국어] 삭제할 CQ ID */

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

/*
 * [한국어] nvme_pcie_ctrlr_cmd_delete_io_sq - Delete IO SQ admin 명령 제출.
 *
 * SQ를 먼저 삭제한 후 CQ를 삭제하는 올바른 순서.
 */
int
nvme_pcie_ctrlr_cmd_delete_io_sq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_DELETE_IO_SQ;
                                  /* [한국어] opcode 0x00 */
	cmd->cdw10_bits.delete_io_q.qid = qpair->id;
                                  /* [한국어] 삭제할 SQ ID */

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

/*
 * [한국어] nvme_completion_sq_error_delete_cq_cb - SQ 생성 실패 후 CQ 삭제 완료 콜백.
 *
 * connect flow가 "Create CQ → Create SQ" 순서인데, Create SQ가 실패하면 이미 만든 CQ를
 * 다시 삭제해야 한다. 이 콜백은 그 최종 CQ 삭제의 완료 처리 — 실패든 성공이든 qpair를 FAILED로 마킹.
 */
static void
nvme_completion_sq_error_delete_cq_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_qpair *qpair = arg;
                                  /* [한국어] 정리 대상 qpair */
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	if (spdk_nvme_cpl_is_error(cpl)) {
                                  /* [한국어] CQ 삭제 자체도 실패 — 컨트롤러 상태 비정상 */
		NVME_QPAIR_ERRLOG(qpair, "delete_io_cq failed!\n");
	}

	pqpair->pcie_state = NVME_PCIE_QPAIR_FAILED;
                                  /* [한국어] 어쨌든 connect는 실패한 상태 — FAILED로 표시 */
}

/*
 * [한국어] ★ nvme_completion_create_sq_cb - Create SQ admin 완료 콜백 (qpair connect 최종 단계).
 *
 * connect 플로우: Create CQ → (cq_cb) → Create SQ → **이 콜백** → READY
 *
 * 주요 동작:
 *   1) defer_destruction이 세팅된 경우 (connect 중 사용자가 delete 요청한 경우)
 *      → 이제 outstanding 끝났으니 안전하게 qpair_destroy 수행
 *   2) SQ 생성 실패면 CQ까지 되돌리기 위해 delete_io_cq 제출
 *   3) 성공: pqpair->pcie_state = READY, shadow doorbell 설정 후 qpair_reset
 *
 * Shadow doorbell (NVMe 1.3+ 최적화):
 *   - 호스트 메모리에 MMIO doorbell의 "shadow" 매핑 (BAR가 아닌 일반 RAM)
 *   - 호스트는 shadow에 먼저 write → 장치가 polling으로 변경 감지
 *   - eventidx: 장치가 관심 갖는 임계값 — 호스트 shadow 값이 이 값 근처일 때만 장치가 깨어남
 *   - 이로써 MMIO write 횟수를 줄여 CPU와 PCIe 대역 절약
 *   - ctrlr->shadow_doorbell이 있으면 사용, 없으면 일반 BAR MMIO 사용
 */
static void
nvme_completion_create_sq_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_qpair *qpair = arg;
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(ctrlr);
	int rc;

	if (pqpair->flags.defer_destruction) {
		/* This qpair was deleted by the application while the
		 * connection was still in progress.  We had to wait
		 * to free the qpair resources until this outstanding
		 * command was completed.  Now that we have the completion
		 * free it now.
		 */
                                  /* [한국어] 사용자가 connect 중 delete 요청 → outstanding이 끝났으니 이제 안전 해제 */
		nvme_pcie_qpair_destroy(qpair);
		return;
	}

	if (spdk_nvme_cpl_is_error(cpl)) {
                                  /* [한국어] Create SQ 실패 — 이미 만든 CQ를 되돌려야 함 */
		NVME_QPAIR_ERRLOG(qpair, "nvme_create_io_sq failed, deleting cq!\n");
		rc = nvme_pcie_ctrlr_cmd_delete_io_cq(qpair->ctrlr, qpair, nvme_completion_sq_error_delete_cq_cb,
						      qpair);
                                  /* [한국어] CQ 삭제 제출 — 완료 시 FAILED 마킹 */
		if (rc != 0) {
                                  /* [한국어] 삭제 요청도 실패 — 치명적 */
			NVME_QPAIR_ERRLOG(qpair, "Failed to send request to delete_io_cq with rc=%d\n", rc);
			pqpair->pcie_state = NVME_PCIE_QPAIR_FAILED;
		}
		return;
	}
	pqpair->pcie_state = NVME_PCIE_QPAIR_READY;
                                  /* [한국어] ★ SQ + CQ 모두 성공 → qpair 사용 준비 완료 */
	if (ctrlr->shadow_doorbell) {
                                  /* [한국어] shadow doorbell 기능 활성 — 저비용 doorbell 사용 가능 */
		pqpair->shadow_doorbell.sq_tdbl = ctrlr->shadow_doorbell + (2 * qpair->id + 0) *
						  pctrlr->doorbell_stride_u32;
                                  /* [한국어] 이 qpair의 SQ tail shadow 위치 계산 */
		pqpair->shadow_doorbell.cq_hdbl = ctrlr->shadow_doorbell + (2 * qpair->id + 1) *
						  pctrlr->doorbell_stride_u32;
                                  /* [한국어] CQ head shadow */
		pqpair->shadow_doorbell.sq_eventidx = ctrlr->eventidx + (2 * qpair->id + 0) *
						      pctrlr->doorbell_stride_u32;
                                  /* [한국어] 장치가 관심 갖는 SQ tail 임계값 */
		pqpair->shadow_doorbell.cq_eventidx = ctrlr->eventidx + (2 * qpair->id + 1) *
						      pctrlr->doorbell_stride_u32;
                                  /* [한국어] 장치가 관심 갖는 CQ head 임계값 */
		pqpair->flags.has_shadow_doorbell = 1;
                                  /* [한국어] hot-path에서 이 플래그 확인 후 shadow vs MMIO 선택 */
	} else {
		pqpair->flags.has_shadow_doorbell = 0;
                                  /* [한국어] 일반 MMIO doorbell 사용 */
	}
	nvme_pcie_qpair_reset(qpair);
                                  /* [한국어] 이제 READY이므로 head/tail/phase 초기화 (connect 재사용 고려) */

}

/*
 * [한국어] nvme_completion_create_cq_cb - Create CQ admin 완료 콜백 (connect 중간 단계).
 *
 * connect 플로우에서 CQ 생성 완료 후 SQ 생성을 이어 시작.
 * CQ 성공 → Create SQ 제출 → 상태를 WAIT_FOR_SQ로 전이.
 */
static void
nvme_completion_create_cq_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_qpair *qpair = arg;
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	int rc;

	if (pqpair->flags.defer_destruction) {
		/* This qpair was deleted by the application while the
		 * connection was still in progress.  We had to wait
		 * to free the qpair resources until this outstanding
		 * command was completed.  Now that we have the completion
		 * free it now.
		 */
                                  /* [한국어] defer_destruction 경로 — SQ_cb와 동일 이유 */
		nvme_pcie_qpair_destroy(qpair);
		return;
	}

	if (spdk_nvme_cpl_is_error(cpl)) {
                                  /* [한국어] CQ 생성 실패 — SQ는 시작도 안 했으므로 정리 없이 FAILED */
		pqpair->pcie_state = NVME_PCIE_QPAIR_FAILED;
		NVME_QPAIR_ERRLOG(qpair, "nvme_create_io_cq failed!\n");
		return;
	}

	rc = nvme_pcie_ctrlr_cmd_create_io_sq(qpair->ctrlr, qpair, nvme_completion_create_sq_cb, qpair);
                                  /* [한국어] CQ 성공 → Create SQ 제출, 완료 콜백 = create_sq_cb */

	if (rc != 0) {
                                  /* [한국어] SQ 제출 자체 실패 — 이미 만든 CQ 삭제 */
		NVME_QPAIR_ERRLOG(qpair, "Failed to send request to create_io_sq, deleting cq!\n");
		rc = nvme_pcie_ctrlr_cmd_delete_io_cq(qpair->ctrlr, qpair, nvme_completion_sq_error_delete_cq_cb,
						      qpair);
		if (rc != 0) {
			NVME_QPAIR_ERRLOG(qpair, "Failed to send request to delete_io_cq with rc=%d\n", rc);
			pqpair->pcie_state = NVME_PCIE_QPAIR_FAILED;
		}
		return;
	}
	pqpair->pcie_state = NVME_PCIE_QPAIR_WAIT_FOR_SQ;
                                  /* [한국어] SQ 완료 대기 상태 */
}

/*
 * [한국어] _nvme_pcie_ctrlr_create_io_qpair - I/O qpair connect의 시작점.
 *
 * qpair 통계 할당 후 Create CQ admin 명령 제출 → create_cq_cb → create_sq_cb → READY
 * 비동기 체인이므로 이 함수는 시작만 하고 바로 리턴. 실제 완료는 connect_qpair의 busy-wait가 감지.
 *
 * 통계 필드(pqpair->stat) 관리 규약:
 *   - poll_group 소속: 그룹의 공유 stats 재사용 (shared_stats=true)
 *   - poll_group 없음: 개별 calloc (reset 시 기존 것 유지)
 */
static int
_nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 uint16_t qid)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	int	rc;

	/* Statistics may already be allocated in the case of controller reset */
	if (qpair->poll_group) {
                                  /* [한국어] poll_group 소속 — 그룹 stats 재사용 */
		struct nvme_pcie_poll_group *group = SPDK_CONTAINEROF(qpair->poll_group,
						     struct nvme_pcie_poll_group, group);
                                  /* [한국어] transport_poll_group → nvme_pcie_poll_group 다운캐스트 */

		pqpair->stat = &group->stats;
                                  /* [한국어] 그룹 공유 stats 포인터 세팅 */
		pqpair->shared_stats = true;
                                  /* [한국어] free 시 개별 해제 안 하도록 표시 */
	} else {
                                  /* [한국어] poll_group 미사용 — 개별 stats */
		if (pqpair->stat == NULL) {
                                  /* [한국어] 처음 생성 시만 할당 (reset 시에는 재사용) */
			pqpair->stat = calloc(1, sizeof(*pqpair->stat));
                                  /* [한국어] 일반 힙 (hugepage 불필요 — 호스트 전용 카운터) */
			if (!pqpair->stat) {
				NVME_QPAIR_ERRLOG(qpair, "Failed to allocate qpair statistics\n");
				nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
				return -ENOMEM;
			}
		}
	}

	rc = nvme_pcie_ctrlr_cmd_create_io_cq(ctrlr, qpair, nvme_completion_create_cq_cb, qpair);
                                  /* [한국어] ★ Create CQ 제출 — 비동기 체인 시작 */

	if (rc != 0) {
                                  /* [한국어] 제출 자체 실패 */
		NVME_QPAIR_ERRLOG(qpair, "Failed to send request to create_io_cq\n");
		nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
		return rc;
	}
	pqpair->pcie_state = NVME_PCIE_QPAIR_WAIT_FOR_CQ;
                                  /* [한국어] CQ 완료 대기 상태 — 완료되면 cb가 다음 단계로 진행 */
	return 0;
}

/*
 * [한국어] nvme_pcie_ctrlr_connect_qpair - PCIe qpair connect 진입점 (트랜스포트 vtable).
 *
 * @return: 0=시작 성공 또는 admin 즉시 완료, <0=실패
 *
 * - admin qpair: 장치 등록 시 이미 SQ/CQ 준비되어 있으므로 state만 CONNECTED로 전이
 * - I/O qpair: Create CQ → Create SQ 비동기 체인 시작 (위 함수 호출)
 */
int
nvme_pcie_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc = 0;

	if (!nvme_qpair_is_admin_queue(qpair)) {
                                  /* [한국어] I/O qpair — admin 명령 체인 필요 */
		rc = _nvme_pcie_ctrlr_create_io_qpair(ctrlr, qpair, qpair->id);
	} else {
                                  /* [한국어] admin qpair — 즉시 CONNECTED (장치 initial state에 admin queue 포함) */
		nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
	}

	return rc;
}

/*
 * [한국어] nvme_pcie_ctrlr_disconnect_qpair - PCIe qpair disconnect (트랜스포트 vtable).
 *
 * 두 경로:
 *   1) I/O qpair 또는 "admin이지만 ctrlr disconnect 진행 중 아닌" 경우:
 *      즉시 disconnect_qpair_done 호출 → state를 DISCONNECTED로 확정
 *   2) admin qpair + ctrlr->is_disconnecting=true (컨트롤러 전체 reset 중):
 *      Controller Level Reset(CC.EN=0)을 시작 → 장치가 모든 I/O SQ/CQ 삭제
 *      이후 admin qpair의 outstanding을 안전하게 abort 가능
 */
void
nvme_pcie_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	if (!nvme_qpair_is_admin_queue(qpair) || !ctrlr->is_disconnecting) {
                                  /* [한국어] 일반 경로 — I/O qpair 또는 ctrlr reset 없이 admin 단독 disconnect */
		nvme_transport_ctrlr_disconnect_qpair_done(qpair);
                                  /* [한국어] 즉시 DISCONNECTED 전이 + abort + fabric cleanup */
	} else {
		/* If this function is called for the admin qpair via spdk_nvme_ctrlr_reset()
		 * or spdk_nvme_ctrlr_disconnect(), initiate a Controller Level Reset.
		 * Then we can abort trackers safely because the Controller Level Reset deletes
		 * all I/O SQ/CQs.
		 */
                                  /* [한국어] Controller Level Reset 경로 — admin qpair를 끄기 전 컨트롤러 자체를 비활성화
                                   *         이렇게 하면 장치가 모든 I/O SQ/CQ를 내부적으로 삭제하므로 outstanding tracker 안전 */
		nvme_ctrlr_disable(ctrlr);
	}
}

/* Used when dst points to MMIO (i.e. CMB) in a virtual machine - in these cases we must
 * not use wide instructions because QEMU will not emulate such instructions to MMIO space.
 * So this function ensures we only copy 8 bytes at a time.
 */
/*
 * [한국어] nvme_pcie_copy_command_mmio - CMB 목적지용 8B-at-a-time SQE copy (QEMU 호환).
 *
 * 일반 CPU는 128b SSE 또는 256b AVX로 64B SQE를 한 번에 copy 가능. 하지만 dst가 MMIO 영역이고
 * 가상머신(QEMU)에서 실행 중이면 QEMU가 wide MMIO write를 에뮬레이션 못 함 (최대 8B).
 * 이 경로는 호환성 우선 — 8B씩 8회 write (16 loop 반복 → 8 byte * 8 = 64B).
 *
 * 사용 조건: ctrlr->quirks & NVME_QUIRK_MAXIMUM_PCI_ACCESS_WIDTH && sq_in_cmb
 */
static inline void
nvme_pcie_copy_command_mmio(struct spdk_nvme_cmd *dst, const struct spdk_nvme_cmd *src)
{
	uint64_t *dst64 = (uint64_t *)dst;
                                  /* [한국어] dst를 64비트 포인터로 alias — 8B 단위 write */
	const uint64_t *src64 = (const uint64_t *)src;
                                  /* [한국어] src도 8B 단위 read (RAM이므로 wide 가능하지만 일관성 유지) */
	uint32_t i;

	for (i = 0; i < sizeof(*dst) / 8; i++) {
                                  /* [한국어] 64B / 8 = 8회 반복 */
		dst64[i] = src64[i];
                                  /* [한국어] 8B write — QEMU MMIO handler가 이 폭만 안정 지원 */
	}
}

/*
 * [한국어] ★ nvme_pcie_copy_command - 일반 호스트 메모리용 64B SQE copy (hot-path).
 *
 * SSE2 사용 시: _mm_stream_si128 x 4 = **non-temporal (write-through) 128b write**.
 *   - non-temporal: CPU 캐시를 거치지 않고 곧바로 메모리에 write.
 *   - 이유: SQE는 write 후 장치가 PCIe로 fetch하므로 CPU 캐시에 보관해도 의미 없음.
 *     cache pollution을 피하고 write combining buffer를 활용해 PCIe bandwidth 효율 향상.
 *   - 64B 전체를 16B × 4회로 분할. SSE2가 x86에 광범위 지원되어 기본 경로.
 *
 * 비-x86 또는 SSE2 미지원: 단순 struct assignment (`*dst = *src`) — 컴파일러가 memcpy로 변환.
 *
 * 사전 조건(영문 주석): dst/src는 겹치지 않고 64B 정렬 — SQE 배열과 req 구조체 모두 보장.
 */
static inline void
nvme_pcie_copy_command(struct spdk_nvme_cmd *dst, const struct spdk_nvme_cmd *src)
{
	/* dst and src are known to be non-overlapping and 64-byte aligned. */
#if defined(__SSE2__)
	__m128i *d128 = (__m128i *)dst;
                                  /* [한국어] 128비트 integer 포인터 alias — 4개 슬롯 = 64B */
	const __m128i *s128 = (const __m128i *)src;

	_mm_stream_si128(&d128[0], _mm_load_si128(&s128[0]));
                                  /* [한국어] src[0..15] load → dst[0..15] non-temporal write
                                   *         load_si128은 16B 정렬 필요 (SQE 배열이 64B 정렬이므로 OK) */
	_mm_stream_si128(&d128[1], _mm_load_si128(&s128[1]));
                                  /* [한국어] src[16..31] → dst[16..31] (CDW4-7, opcode/cid/nsid 등 주요 필드) */
	_mm_stream_si128(&d128[2], _mm_load_si128(&s128[2]));
                                  /* [한국어] src[32..47] → dst[32..47] (CDW8-11, PRP1/DPTR) */
	_mm_stream_si128(&d128[3], _mm_load_si128(&s128[3]));
                                  /* [한국어] src[48..63] → dst[48..63] (CDW12-15, SLBA/NLB/FUA 등) */
#else
	*dst = *src;
                                  /* [한국어] 비 x86 경로 — 컴파일러가 rep movsq 또는 memcpy로 처리 */
#endif
}

/*
 * [한국어] ★★★ nvme_pcie_qpair_submit_tracker - SQ[sq_tail]에 SQE 기록 + doorbell ring ★★★
 *
 * SPDK NVMe 드라이버의 **진짜 "I/O 제출" 지점**. 이 한 함수가 실행되면 SQE가 장치로 나간다.
 *
 * @qpair: 대상 qpair
 * @tr:    빌드된 tracker (req와 연결, cmd 필드 완성 상태)
 *
 * 처리 순서:
 *
 *   [1] tracer 기록 (TRACE_NVME_PCIE_SUBMIT) — 성능 분석 도구가 사용
 *
 *   [2] Fused 명령 추적:
 *       - Fused 1st/2nd 연속 제출 중이면 last_fuse 갱신.
 *       - Doorbell은 2nd가 submit된 후에 한 번만 ring — 둘 다 한 번에 장치 fetch.
 *
 *   [3] ★ SQE를 SQ 배열에 copy:
 *       - 일반: nvme_pcie_copy_command (SSE2 non-temporal 128b×4, 캐시 pollution 회피)
 *       - QEMU MMIO CMB: nvme_pcie_copy_command_mmio (8B×8, QEMU 호환)
 *
 *   [4] sq_tail++ (wrap-around 처리):
 *       - sq_tail == num_entries면 0으로 wrap.
 *       - sq_tail == sq_head면 SQ overflow (호스트가 full 감지 못 한 버그 상황).
 *
 *   [5] ★ Doorbell ring:
 *       - flags.delay_cmd_submit이 꺼져 있으면 (batching 모드 아님) 즉시 doorbell write.
 *       - nvme_pcie_qpair_ring_sq_doorbell은 nvme_pcie_internal.h의 inline 함수로,
 *         shadow_doorbell 또는 MMIO write로 sq_tail을 장치에 통지.
 *
 *   장치가 doorbell 받으면 CPU/PCIe 인터럽트 없이 즉시 SQE fetch 시작 → 작업 처리 → CQE 기록 →
 *   phase bit 토글 → process_completions가 발견.
 */
void
nvme_pcie_qpair_submit_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	struct nvme_request	*req;
                                  /* [한국어] tracker가 참조하는 상위 요청 */
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
                                  /* [한국어] PCIe qpair 다운캐스트 */
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
                                  /* [한국어] 컨트롤러 — quirks 체크에 사용 */

	req = tr->req;
	assert(req != NULL);
                                  /* [한국어] submit 전에 반드시 req 연결되어 있어야 */

	spdk_trace_record(TRACE_NVME_PCIE_SUBMIT, qpair->id, 0, (uintptr_t)req, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)req->cmd.opc,
			  req->cmd.cdw10, req->cmd.cdw11, req->cmd.cdw12,
			  pqpair->qpair.queue_depth);
                                  /* [한국어] 성능 트레이스 기록 — spdk_trace 도구가 수집 (qid/req/opc/LBA 등) */

	if (req->cmd.fuse) {
		/*
		 * Keep track of the fuse operation sequence so that we ring the doorbell only
		 * after the second fuse is submitted.
		 */
                                  /* [한국어] Fused 연산 추적 — 두 명령을 묶어 atomic 처리 (Compare-and-Write 등).
                                   *         1st: FIRST=1, 2nd: SECOND=1.
                                   *         장치는 두 SQE를 함께 받아야 원자 처리하므로 1st 제출 직후 doorbell 치지 않음
                                   *         → 2nd 제출 후 함께 통지. last_fuse 필드가 이 상태를 추적. */
		qpair->last_fuse = req->cmd.fuse;
	}

	/* Don't use wide instructions to copy NVMe command, this is limited by QEMU
	 * virtual NVMe controller, the maximum access width is 8 Bytes for one time.
	 */
	if (spdk_unlikely((ctrlr->quirks & NVME_QUIRK_MAXIMUM_PCI_ACCESS_WIDTH) && pqpair->sq_in_cmb)) {
                                  /* [한국어] quirk 설정 + CMB SQ인 경우 — QEMU 호환 경로 */
		nvme_pcie_copy_command_mmio(&pqpair->cmd[pqpair->sq_tail], &req->cmd);
                                  /* [한국어] 8B × 8회로 write */
	} else {
		/* Copy the command from the tracker to the submission queue. */
                                  /* [한국어] ★ 일반 경로 — SSE2 128b×4 non-temporal write (hot-path) */
		nvme_pcie_copy_command(&pqpair->cmd[pqpair->sq_tail], &req->cmd);
	}

	if (spdk_unlikely(++pqpair->sq_tail == pqpair->num_entries)) {
                                  /* [한국어] ★ sq_tail 전진 + wrap-around 체크
                                   *         ++ 먼저 수행 후 num_entries 도달이면 wrap */
		pqpair->sq_tail = 0;
	}

	if (spdk_unlikely(pqpair->sq_tail == pqpair->sq_head)) {
                                  /* [한국어] sq_tail이 sq_head를 따라잡음 = SQ 가득 참 (이론상 발생 X) */
		NVME_QPAIR_ERRLOG(qpair, "sq_tail is passing sq_head!\n");
	}

	if (!pqpair->flags.delay_cmd_submit) {
                                  /* [한국어] batching 모드가 아니면 즉시 doorbell ring */
		nvme_pcie_qpair_ring_sq_doorbell(qpair);
                                  /* [한국어] ★★★ SQ tail doorbell MMIO write!
                                   *         nvme_pcie_internal.h의 inline 함수 — shadow doorbell 또는 BAR MMIO
                                   *         이 한 줄이 실행되면 장치가 SQ[sq_tail_old..new]의 SQE들을 fetch 시작.
                                   *         polled-mode NVMe의 "notification" 지점. */
	}
}

/*
 * [한국어] ★★★ nvme_pcie_qpair_complete_tracker - 단일 CQE에 대한 개별 완료 처리 ★★★
 *
 * process_completions가 CQ에서 완료 엔트리를 발견하면 이 함수를 호출해 tracker 단위 처리.
 *
 * @qpair:         소속 qpair
 * @tr:            완료할 tracker (CQE의 cid로 조회됨)
 * @cpl:           CQE (장치가 쓴 원본 또는 manual_complete가 만든 가짜)
 * @print_on_error: 에러 CQE일 때 로그 출력 여부
 *
 * 처리 흐름:
 *   [1] TRACE_NVME_PCIE_COMPLETE 기록
 *   [2] retry 판정: error && is_retry(cpl) && retries < retry_count
 *       - nvme_completion_is_retry (nvme_qpair.c): NAMESPACE_NOT_READY 등만 재시도 허용
 *   [3] 에러 로그 (optional)
 *   [4] retry=true: retries++ + 같은 tracker 재제출 (tracker 재사용!)
 *   [5] retry=false:
 *       - outstanding_tr에서 제거 + queue_depth--
 *       - **Multi-process admin 처리**: 다른 프로세스가 제출한 admin CQE면
 *         insert_pending_admin_request로 그 프로세스의 큐에 넣고 cb_fn 호출 미룸
 *       - 그 외: nvme_complete_request로 즉시 cb_fn 호출 (req는 free됨)
 *       - tr->req=NULL + free_tr에 반환 (LIFO)
 */
void
nvme_pcie_qpair_complete_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
				 struct spdk_nvme_cpl *cpl, bool print_on_error)
{
	struct nvme_pcie_qpair		*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_request		*req;
                                  /* [한국어] tracker가 참조하는 원본 요청 */
	bool				retry, error;
                                  /* [한국어] 재시도 판정 플래그들 */
	bool				print_error;

	req = tr->req;

	spdk_trace_record(TRACE_NVME_PCIE_COMPLETE, qpair->id, 0, (uintptr_t)req, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)cpl->status_raw, pqpair->qpair.queue_depth);
                                  /* [한국어] 완료 트레이스 기록 — 지연 측정 등에 사용 */

	assert(req != NULL);
                                  /* [한국어] submit된 tracker는 반드시 req 연결 보유 */

	error = spdk_nvme_cpl_is_error(cpl);
                                  /* [한국어] SC != SUCCESS */
	retry = error && nvme_completion_is_retry(cpl) &&
		req->retries < pqpair->retry_count;
                                  /* [한국어] 재시도 조건 3중 AND:
                                   *         (a) 에러일 것
                                   *         (b) 스펙상 retry 가능한 SC (NAMESPACE_NOT_READY 등, DNR=0)
                                   *         (c) 아직 retry 횟수 잔여 */
	print_error = error && print_on_error && !qpair->ctrlr->opts.disable_error_logging;

	if (print_error) {
		spdk_nvme_qpair_print_command(qpair, &req->cmd);
                                  /* [한국어] 실패한 SQE 내용 로그 */
	}

	if (print_error || SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
		spdk_nvme_qpair_print_completion(qpair, cpl);
                                  /* [한국어] CQE 내용 로그 */
	}

	assert(cpl->cid == req->cmd.cid);
                                  /* [한국어] 일관성 체크 — CQE의 cid는 SQE 빌드 때 쓴 cid와 같아야 */

	if (retry) {
		req->retries++;
                                  /* [한국어] 재시도 카운터 증가 */
		nvme_pcie_qpair_submit_tracker(qpair, tr);
                                  /* [한국어] 같은 tracker 재제출 — outstanding_tr에 그대로 유지 */
	} else {
                                  /* [한국어] 정상 또는 영구 실패 — 완료 처리 */
		TAILQ_REMOVE(&pqpair->outstanding_tr, tr, tq_list);
                                  /* [한국어] outstanding 리스트에서 제거 */
		pqpair->qpair.queue_depth--;
                                  /* [한국어] 큐 깊이 감소 (hot-path 통계) */

		/* Only check admin requests from different processes. */
		if (nvme_qpair_is_admin_queue(qpair) && req->pid != getpid()) {
                                  /* [한국어] 다른 프로세스가 제출한 admin 요청 */
			nvme_pcie_qpair_insert_pending_admin_request(qpair, req, cpl);
                                  /* [한국어] 해당 프로세스의 active_reqs 큐에 보류 — 나중에 그 프로세스가 처리 */
		} else {
                                  /* [한국어] 내 프로세스 요청 또는 I/O 큐 */
			nvme_complete_request(tr->cb_fn, tr->cb_arg, qpair, req, cpl);
                                  /* [한국어] ★ 공용 완료 경로 — 사용자 cb_fn 실행 + req free
                                   *         (nvme_internal.h 선언, nvme.c 구현) */
		}

		tr->req = NULL;
                                  /* [한국어] tracker 해방 — req 연결 해제 */

		TAILQ_INSERT_HEAD(&pqpair->free_tr, tr, tq_list);
                                  /* [한국어] free 풀의 HEAD에 반환 (LIFO — 캐시 hot 유지) */
	}
}

/*
 * [한국어] nvme_pcie_qpair_manual_complete_tracker - 가짜 CQE로 tracker 완료 (abort 등).
 *
 * 장치가 실제 CQE를 주지 않았지만 호스트가 "abort됐다"고 간주해야 할 때 사용.
 * 임의의 SCT/SC/DNR로 가짜 cpl을 합성 → 일반 complete_tracker에 위임.
 */
void
nvme_pcie_qpair_manual_complete_tracker(struct spdk_nvme_qpair *qpair,
					struct nvme_tracker *tr, uint32_t sct, uint32_t sc, uint32_t dnr,
					bool print_on_error)
{
	struct spdk_nvme_cpl	cpl;
                                  /* [한국어] 합성할 16B CQE (스택) */

	memset(&cpl, 0, sizeof(cpl));
                                  /* [한국어] 0 초기화 — cdw0/sqhd/m/p 등 모두 0 */
	cpl.sqid = qpair->id;
                                  /* [한국어] sqid 일관성 (print_completion 경고 회피) */
	cpl.cid = tr->cid;
                                  /* [한국어] tracker의 cid — complete_tracker 내부 assert에서 검증 */
	cpl.status.sct = sct;
	cpl.status.sc = sc;
	cpl.status.dnr = dnr;
                                  /* [한국어] 호출자 지정 상태 */
	nvme_pcie_qpair_complete_tracker(qpair, tr, &cpl, print_on_error);
                                  /* [한국어] 가짜 CQE로 일반 경로 호출 */
}

/*
 * [한국어] nvme_pcie_qpair_abort_trackers - outstanding 전체를 ABORTED_BY_REQUEST로 일괄 완료.
 *
 * reset/disconnect 시 장치가 들고 있던 outstanding SQE가 모두 무효가 되므로
 * 호스트 측에서 "aborted by request"로 가짜 완료 처리.
 *
 * last 미리 저장 이유: 완료 처리 중 cb_fn이 새 submit을 할 수 있는데, 그 새 tracker가
 * outstanding_tr의 tail에 붙으면 FOREACH가 무한 루프 — last까지만 처리하고 중단.
 */
void
nvme_pcie_qpair_abort_trackers(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker *tr, *temp, *last;

	last = TAILQ_LAST(&pqpair->outstanding_tr, nvme_outstanding_tr_head);
                                  /* [한국어] 순회 종료 기준 — 처음 호출 시점의 tail */

	/* Abort previously submitted (outstanding) trs */
	TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, temp) {
                                  /* [한국어] SAFE — complete_tracker가 tr을 리스트에서 제거해도 안전 순회 */
		if (!qpair->ctrlr->opts.disable_error_logging) {
			NVME_QPAIR_ERRLOG(qpair, "aborting outstanding command\n");
		}
		nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
							SPDK_NVME_SC_ABORTED_BY_REQUEST, dnr, true);
                                  /* [한국어] ABORTED_BY_REQUEST(0x07)로 가짜 완료 */

		if (tr == last) {
			break;
                                  /* [한국어] 시작 시점의 tail에 도달 — 이후 새로 추가된 tracker는 다음 cycle에 처리 */
		}
	}
}

/*
 * [한국어] nvme_pcie_admin_qpair_abort_aers - AER(Async Event Request) 전용 abort.
 *
 * AER은 admin 큐에 상주하며 장치 비동기 이벤트를 받기 위한 outstanding SQE.
 * ctrlr reset 등 정리 시 AER만 선별적으로 abort (나머지 admin cmd는 정상 완료 대기).
 *
 * 이유: AER은 수 분~수 시간 동안 장치가 이벤트 없으면 completion 안 보냄 → 일반 완료 경로로 안 끝남.
 *       따라서 명시적으로 SQ_DELETION으로 종료 처리.
 */
void
nvme_pcie_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;

	tr = TAILQ_FIRST(&pqpair->outstanding_tr);
	while (tr != NULL) {
		assert(tr->req != NULL);
		if (tr->req->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
                                  /* [한국어] AER opcode 0x0C 매칭 — abort */
			nvme_pcie_qpair_manual_complete_tracker(qpair, tr,
								SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_ABORTED_SQ_DELETION, 0,
								false);
                                  /* [한국어] SQ_DELETION으로 마킹 — "SQ가 지워져서 끝남"
                                   *         print_on_error=false: AER abort는 정상 reset의 일부이므로 로그 스팸 방지 */
			tr = TAILQ_FIRST(&pqpair->outstanding_tr);
                                  /* [한국어] 매번 first로 되돌아감 — 완료 처리 후 리스트가 변동됐기 때문 */
		} else {
			tr = TAILQ_NEXT(tr, tq_list);
                                  /* [한국어] AER이 아닌 요청은 건너뜀 */
		}
	}
}

/*
 * [한국어] nvme_pcie_admin_qpair_destroy - admin qpair 파괴 시 AER만 abort.
 *
 * admin qpair를 완전 파괴하기 전에 outstanding AER을 정리하는 훅.
 * 일반 outstanding admin cmd는 parent(컨트롤러)가 별도로 관리하므로 여기서 abort 안 함.
 */
void
nvme_pcie_admin_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	nvme_pcie_admin_qpair_abort_aers(qpair);
                                  /* [한국어] AER만 abort — 나머지는 상위 ctrlr 정리 경로가 처리 */
}

/*
 * [한국어] nvme_pcie_qpair_abort_reqs - 트랜스포트 vtable의 qpair_abort_reqs 구현.
 *
 * nvme_qpair.c의 `nvme_transport_qpair_abort_reqs`가 호출하는 진입점.
 * 단순히 outstanding tracker 전부를 abort.
 */
void
nvme_pcie_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	nvme_pcie_qpair_abort_trackers(qpair, dnr);
                                  /* [한국어] 모든 outstanding을 ABORTED_BY_REQUEST로 완료 */
}

/*
 * [한국어] nvme_pcie_qpair_check_timeout - outstanding tracker들 중 timeout 초과 검출.
 *
 * 호출자: process_completions의 선행 체크 (느린 장치 대응, 애플리케이션 notification).
 *
 * 동작:
 *   1) ctrlr가 READY 상태일 때만 실행 (초기화 중에는 스킵)
 *   2) 현재 프로세스/active_proc에 timeout_cb_fn이 설정되어 있어야 실행
 *   3) outstanding_tr 순회 — 첫 번째로 timeout 미경과 tracker를 만나면 break
 *      (FIFO 순서 가정 — 앞이 안 끝났으면 뒤도 안 끝났을 것)
 *
 * timeout 감지 시 timeout_cb_fn이 호출되어 애플리케이션이 판단 (포기 or 재시도).
 */
static void
nvme_pcie_qpair_check_timeout(struct spdk_nvme_qpair *qpair)
{
	uint64_t t02;
                                  /* [한국어] 현재 tick 시점 — timeout 비교 기준 */
	struct nvme_tracker *tr, *tmp;
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvme_ctrlr_process *active_proc;
                                  /* [한국어] timeout_cb_fn을 가진 프로세스 컨텍스트 */

	/* Don't check timeouts during controller initialization. */
	if (ctrlr->state != NVME_CTRLR_STATE_READY) {
                                  /* [한국어] 초기화 중엔 느려서 timeout 오탐 가능 — 스킵 */
		return;
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
                                  /* [한국어] admin: 현재 프로세스의 컨텍스트 (multi-process 공유) */
		active_proc = nvme_ctrlr_get_current_process(ctrlr);
	} else {
                                  /* [한국어] I/O: qpair가 저장한 소유 프로세스 컨텍스트 */
		active_proc = qpair->active_proc;
	}

	/* Only check timeouts if the current process has a timeout callback. */
	if (active_proc == NULL || active_proc->timeout_cb_fn == NULL) {
                                  /* [한국어] timeout cb 미등록 — 체크할 의미 없음 */
		return;
	}

	t02 = spdk_get_ticks();
                                  /* [한국어] 현재 tick 획득 */
	TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, tmp) {
		assert(tr->req != NULL);

		if (nvme_request_check_timeout(tr->req, tr->cid, active_proc, t02)) {
                                  /* [한국어] tracker별 timeout 검사 (nvme.c에 구현) — true=아직 timeout 안 됨 */
			/*
			 * The requests are in order, so as soon as one has not timed out,
			 * stop iterating.
			 */
                                  /* [한국어] 요청은 FIFO 순서이므로 timeout 미경과 tracker 발견 시 이후는 볼 필요 없음 */
			break;
		}
	}
}

/*
 * [한국어] ★★★★ nvme_pcie_qpair_process_completions — PCIe 완료 폴링의 심장 ★★★★
 *
 * SPDK NVMe의 "polled-mode" 본질이 이 함수에 있다. reactor poller가 초당 수만 번 호출해
 * CQ에 phase bit가 바뀐 엔트리가 있는지 확인하고, 있으면 tracker → req → cb_fn을 이어간다.
 *
 * @max_completions: 이번 호출에서 수확할 최대 개수 (0=무제한, 내부적으로 max_completions_cap 제한)
 * @return: 처리한 CQE 개수 (음수는 에러)
 *
 * 처리 단계:
 *
 *   [A] 상태 체크:
 *       - pcie_state == FAILED → 즉시 -ENXIO
 *       - qpair state == CONNECTING → admin 큐를 대신 폴링하여 Create SQ 완료 감지
 *         (pcie_state가 READY면 CONNECTED로 전이, FAILED면 DISCONNECTED)
 *
 *   [B] Admin 큐면 ctrlr_lock 획득 (multi-process 공유 보호)
 *
 *   [C] max_completions 한도 적용 (wrap-around 방지)
 *
 *   [D] ★ 메인 폴링 루프:
 *       1) cpl = &pqpair->cpl[cq_head]
 *       2) cpl->status.p != pqpair->flags.phase면 → 아직 새 엔트리 없음, break
 *       3) next_cpl 유효성 선검사 + prefetch — 다음 tracker를 캐시에 미리 로드 (latency 단축)
 *       4) 아키텍처별 memory barrier — phase/cid 읽기 순서 보장
 *          - PPC64/RISC-V/LoongArch: spdk_mb() (전체 barrier)
 *          - aarch64: dmb oshld (load-load ordering)
 *          - x86: TSO 덕에 barrier 불필요 (암시적 순서)
 *       5) cq_head++ (wrap 시 phase 토글) ★ phase가 뒤집히는 지점
 *       6) tr = &pqpair->tr[cpl->cid] — 배열 인덱싱으로 O(1) tracker 복원
 *       7) sq_head = cpl->sqhd — 장치가 알려준 SQ head 업데이트
 *       8) tr->req 유효하면 prefetch + complete_tracker, 아니면 오류 (outstanding 매칭 실패)
 *       9) num_completions == max_completions이면 break
 *
 *   [E] 완료 처리 후속 작업:
 *       - ★ CQ head doorbell ring (수확한 CQE 개수만큼 head 진행을 장치에 통지)
 *       - delay_cmd_submit 모드이면 deferred SQ doorbell도 여기서 flush
 *       - timeout 체크 (선택적)
 *       - admin 큐면 pending admin request 완료 + DISCONNECTING 처리 + ctrlr_lock 해제
 *       - has_pending_vtophys_failures 처리 — 제출 중 PRP 빌드 실패한 tracker들 정리
 *
 * phase bit 상세:
 *   - pqpair->flags.phase가 "현재 기대값". 처음엔 1.
 *   - 장치도 1로 시작해 CQE를 쓸 때 phase=1을 기록.
 *   - CQ wrap이 일어나면 장치는 phase를 0으로 뒤집음 → 호스트도 flags.phase를 뒤집음.
 *   - 이 메커니즘으로 "이 슬롯은 새 엔트리인가 이전 wrap 잔여인가"를 구별 — MMIO read 불필요.
 *
 * 이 while 루프의 한 번 iteration이 "1 IOPS 처리"의 호스트 측 전부다.
 */
int32_t
nvme_pcie_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;
                                  /* [한국어] 루프 내 현재 완료된 tracker */
	struct spdk_nvme_cpl	*cpl, *next_cpl;
                                  /* [한국어] 현재 CQE 포인터 + 다음 CQE prefetch 용 */
	uint32_t		 num_completions = 0;
                                  /* [한국어] 이번 호출에서 수확한 개수 */
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	uint16_t		 next_cq_head;
                                  /* [한국어] 다음 예상 cq_head */
	uint8_t			 next_phase;
                                  /* [한국어] 다음 예상 phase (wrap 고려) */
	bool			 next_is_valid = false;
                                  /* [한국어] 다음 CQE가 새 엔트리인지 미리 검사한 결과 */
	int			 rc;

	if (spdk_unlikely(pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED)) {
                                  /* [한국어] connect 실패 상태 — 폴링 자체 거부 */
		return -ENXIO;
	}

	if (spdk_unlikely(nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING)) {
                                  /* [한국어] connect 진행 중 경로 — 직접 이 qpair 폴링 대신 admin 큐 폴링 (Create SQ/CQ 완료 감지용) */
		if (pqpair->pcie_state == NVME_PCIE_QPAIR_READY) {
			/* It is possible that another thread set the pcie_state to
			 * QPAIR_READY, if it polled the adminq and processed the SQ
			 * completion for this qpair.  So check for that condition
			 * here and then update the qpair's state to CONNECTED, since
			 * we can only set the qpair state from the qpair's thread.
			 * (Note: this fixed issue #2157.)
			 */
                                  /* [한국어] 다른 스레드가 adminq 폴링하다가 이 qpair의 Create SQ 완료를 처리해
                                   *         pcie_state를 READY로 만들어 놨음 — 본 스레드(qpair 소유자)가 최종 CONNECTED 전이 */
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
		} else if (pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED) {
			nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
                                  /* [한국어] connect 실패 → DISCONNECTED 확정 */
			return -ENXIO;
		} else {
                                  /* [한국어] 아직 WAIT_FOR_CQ/WAIT_FOR_SQ — admin 큐 폴링으로 진척 촉진 */
			rc = spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
                                  /* [한국어] admin을 대신 폴링 — 여기서 Create CQ/SQ 완료 cb가 호출되어 pcie_state 갱신 */
			if (rc < 0) {
				return rc;
			} else if (pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED) {
				nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
				return -ENXIO;
			}
		}
		return 0;
                                  /* [한국어] CONNECTING 경로는 hot-path 아님 — 실제 CQ 폴링은 CONNECTED/ENABLED에서만 */
	}

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
                                  /* [한국어] admin 큐는 multi-process 공유이므로 ctrlr_lock 보호 */
		nvme_ctrlr_lock(ctrlr);
	}

	if (max_completions == 0 || max_completions > pqpair->max_completions_cap) {
		/*
		 * max_completions == 0 means unlimited, but complete at most
		 * max_completions_cap batch of I/O at a time so that the completion
		 * queue doorbells don't wrap around.
		 */
                                  /* [한국어] 상한 적용 — 무제한 요청이어도 CQ 크기의 3/4로 제한 (construct에서 계산됨) */
		max_completions = pqpair->max_completions_cap;
	}

	pqpair->stat->polls++;
                                  /* [한국어] 폴링 횟수 카운터 (idle/busy 비율 분석) */

	while (1) {
		cpl = &pqpair->cpl[pqpair->cq_head];
                                  /* [한국어] ★ 현재 CQ head 슬롯 포인터 */

		if (!next_is_valid && cpl->status.p != pqpair->flags.phase) {
                                  /* [한국어] ★★ phase bit 검사 — 이 한 줄이 polled-mode 완료 감지의 핵심.
                                   *         phase 불일치 = 아직 장치가 새 CQE 안 씀 = 완료 없음 → 루프 종료.
                                   *         next_is_valid는 이전 iteration이 prefetch하며 이미 검증한 플래그 — 재검사 스킵. */
			break;
		}

		if (spdk_likely(pqpair->cq_head + 1 != pqpair->num_entries)) {
                                  /* [한국어] 다음 cq_head가 wrap 안 할 경우 — phase 유지 */
			next_cq_head = pqpair->cq_head + 1;
			next_phase = pqpair->flags.phase;
		} else {
                                  /* [한국어] wrap 예정 — 다음은 0번 슬롯, phase 토글 */
			next_cq_head = 0;
			next_phase = !pqpair->flags.phase;
		}
		next_cpl = &pqpair->cpl[next_cq_head];
		next_is_valid = (next_cpl->status.p == next_phase);
                                  /* [한국어] ★ 다음 CQE 유효성을 미리 확인 — 다음 iteration에서 phase 검사 회피 */
		if (next_is_valid) {
			__builtin_prefetch(&pqpair->tr[next_cpl->cid]);
                                  /* [한국어] ★ 다음 tracker를 캐시에 미리 로드 — 현재 iter 처리 중 CPU가 RAM에서 fetch 시작
                                   *         → 다음 iter 진입 시 L1 캐시에 있어 latency 감소 */
		}

#if defined(__PPC64__) || defined(__riscv) || defined(__loongarch__)
		/*
		 * This memory barrier prevents reordering of:
		 * - load after store from/to tr
		 * - load after load cpl phase and cpl cid
		 */
                                  /* [한국어] weak memory ordering 아키텍처 — 명시적 memory barrier.
                                   *         이유 1: 이전 iteration이 tr->req=NULL로 쓴 것이 다음 iter의 tr load 전에 observed되어야
                                   *         이유 2: cpl.phase 검사 후 cpl.cid 읽기가 reorder되지 않도록 보장 */
		spdk_mb();
#elif defined(__aarch64__)
                                  /* [한국어] aarch64: load-load ordering만 있으면 되므로 더 가벼운 dmb oshld
                                   *         oshld = outer shareable, loads before loads */
		__asm volatile("dmb oshld" ::: "memory");
#endif
                                  /* [한국어] x86(TSO)는 barrier 불필요 — 암시적으로 load 순서 유지됨 */

		if (spdk_unlikely(++pqpair->cq_head == pqpair->num_entries)) {
                                  /* [한국어] ★ cq_head 전진 — wrap 시 phase 토글
                                   *         이 시점 이후 장치는 새 CQE를 반대 phase로 쓰게 됨 */
			pqpair->cq_head = 0;
			pqpair->flags.phase = !pqpair->flags.phase;
		}

		tr = &pqpair->tr[cpl->cid];
                                  /* [한국어] ★ CQE의 cid로 tracker 조회 (배열 인덱싱, O(1)) */
		pqpair->sq_head = cpl->sqhd;
                                  /* [한국어] 장치가 알려준 SQ head 업데이트 (SQ 가용 공간 추적) */

		if (tr->req) {
                                  /* [한국어] 정상 — 연결된 req가 있음 */
			/* Prefetch the req's STAILQ_ENTRY since we'll need to access it
			 * as part of putting the req back on the qpair's free list.
			 */
                                  /* [한국어] req는 free_req 풀로 반환될 예정 — STAILQ_ENTRY 필드 prefetch로 latency 단축 */
			__builtin_prefetch(&tr->req->stailq);
			nvme_pcie_qpair_complete_tracker(qpair, tr, cpl, true);
                                  /* [한국어] ★ 개별 완료 처리 (재시도 판정 + cb_fn 실행 + tracker free) */
		} else {
                                  /* [한국어] CQE가 왔는데 tracker가 비어 있음 — outstanding_tr과 불일치 (버그) */
			NVME_QPAIR_ERRLOG(qpair, "cpl does not map to outstanding cmd\n");
			spdk_nvme_qpair_print_completion(qpair, cpl);
			assert(0);
		}

		if (++num_completions == max_completions) {
                                  /* [한국어] 상한 도달 — 한 호출 한도 준수 */
			break;
		}
	}

	if (num_completions > 0) {
		pqpair->stat->completions += num_completions;
                                  /* [한국어] 통계 — 이번 폴링에서 수확한 개수 */
		nvme_pcie_qpair_ring_cq_doorbell(qpair);
                                  /* [한국어] ★ CQ head doorbell MMIO write — 장치에 "호스트가 여기까지 읽었다" 통지
                                   *         장치는 이 값 기준으로 CQ overflow 방지 */
	} else {
		pqpair->stat->idle_polls++;
                                  /* [한국어] 빈 폴링 — reactor 효율 측정에 사용 (idle 비율이 높으면 CPU 낭비) */
	}

	if (pqpair->flags.delay_cmd_submit) {
                                  /* [한국어] batching 모드 — submit 시 doorbell 미뤄뒀다가 여기서 한 번에 flush */
		if (pqpair->last_sq_tail != pqpair->sq_tail) {
			nvme_pcie_qpair_ring_sq_doorbell(qpair);
                                  /* [한국어] 누적된 SQ writes를 장치에 한 번에 통지 — MMIO 횟수 감소 */
			pqpair->last_sq_tail = pqpair->sq_tail;
                                  /* [한국어] 마지막 doorbell 시점의 sq_tail 저장 */
		}
	}

	if (ctrlr->timeout_enabled) {
		/*
		 * User registered for timeout callback
		 */
                                  /* [한국어] 사용자가 timeout 콜백 등록한 경우에만 비싼 체크 수행 */
		nvme_pcie_qpair_check_timeout(qpair);
	}

	/* Before returning, complete any pending admin request or
	 * process the admin qpair disconnection.
	 */
	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
                                  /* [한국어] admin 큐 전용 후처리 */
		nvme_pcie_qpair_complete_pending_admin_request(qpair);
                                  /* [한국어] 다른 프로세스가 insert한 내 프로세스 요청들 처리 */

		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
                                  /* [한국어] disconnect 진행 중이면 ctrlr disable 진척 */
			rc = nvme_ctrlr_disable_poll(qpair->ctrlr);
			if (rc != -EAGAIN) {
                                  /* [한국어] disable 완료 → disconnect_qpair_done으로 DISCONNECTED 확정 */
				nvme_transport_ctrlr_disconnect_qpair_done(qpair);
			}
		}

		nvme_ctrlr_unlock(ctrlr);
                                  /* [한국어] admin lock 해제 */
	}

	if (spdk_unlikely(pqpair->flags.has_pending_vtophys_failures)) {
                                  /* [한국어] submit 중 PRP/SGL 빌드 실패한 tracker를 지연 처리 (flag 설정만 해뒀음) */
		struct nvme_tracker *tr, *tmp;

		TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, tmp) {
			if (tr->bad_vtophys) {
                                  /* [한국어] 이 tracker가 vtophys 실패로 마킹됐음 */
				tr->bad_vtophys = 0;
				nvme_pcie_fail_request_bad_vtophys(qpair, tr);
                                  /* [한국어] 에러 CQE 합성 후 manual complete */
			}
		}
		pqpair->flags.has_pending_vtophys_failures = 0;
                                  /* [한국어] 플래그 클리어 */
	}

	return num_completions;
                                  /* [한국어] 수확한 개수를 nvme_qpair.c로 반환 */
}

/*
 * [한국어] nvme_pcie_qpair_destroy - PCIe qpair 메모리 해제 (construct의 짝).
 *
 * 해제 순서:
 *   1) admin qpair면 AER abort
 *   2) 사용자 제공 SQ/CQ는 해제 안 함 (소유권 호출자에 있음)
 *   3) CMB에 있는 SQ는 해제 안 함 (bump allocator, 해제 불가)
 *   4) 자체 할당한 SQ/CQ/tracker 배열 spdk_free
 *   5) 공통 qpair 필드 deinit (free_req 풀 포함)
 *   6) stat 해제 (poll_group 공유가 아니고 내 프로세스 소유면)
 *      - admin stat는 SHARE hugepage → spdk_free
 *      - I/O stat는 일반 calloc → free
 *   7) pqpair 자체 spdk_free
 */
int
nvme_pcie_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	if (nvme_qpair_is_admin_queue(qpair)) {
                                  /* [한국어] admin qpair — AER abort 먼저 */
		nvme_pcie_admin_qpair_destroy(qpair);
	}
	/*
	 * We check sq_vaddr and cq_vaddr to see if the user specified the memory
	 * buffers when creating the I/O queue.
	 * If the user specified them, we cannot free that memory.
	 * Nor do we free it if it's in the CMB.
	 */
	if (!pqpair->sq_vaddr && pqpair->cmd && !pqpair->sq_in_cmb) {
                                  /* [한국어] 3가지 조건 AND: 사용자 지정 없음 + 할당됨 + CMB 아님 */
		spdk_free(pqpair->cmd);
	}
	if (!pqpair->cq_vaddr && pqpair->cpl) {
                                  /* [한국어] 사용자 지정 아니고 할당됨 */
		spdk_free(pqpair->cpl);
	}
	if (pqpair->tr) {
                                  /* [한국어] tracker 배열은 항상 자체 할당 */
		spdk_free(pqpair->tr);
	}

	nvme_qpair_deinit(qpair);
                                  /* [한국어] 공통 qpair 해제 (free_req 풀, queued_req 등) */

	if (!pqpair->shared_stats && (!qpair->active_proc ||
				      qpair->active_proc == nvme_ctrlr_get_current_process(qpair->ctrlr))) {
                                  /* [한국어] stat 해제 조건:
                                   *         (a) 공유 stats 아님 (poll_group 미사용)
                                   *         (b) active_proc이 없거나 현재 프로세스 소유 (secondary가 primary 걸 free 못 하게) */
		if (qpair->id) {
                                  /* [한국어] I/O qpair(qid≠0) — 일반 calloc으로 할당했으므로 free */
			free(pqpair->stat);
		} else {
			/* statistics of admin qpair are allocates from huge pages because
			 * admin qpair is shared for multi-process */
                                  /* [한국어] admin qpair — SHARE hugepage로 할당했으므로 spdk_free */
			spdk_free(pqpair->stat);
		}

	}

	spdk_free(pqpair);
                                  /* [한국어] 마지막: qpair 구조체 자체 해제 */

	return 0;
}

/*
 * [한국어] nvme_pcie_ctrlr_create_io_qpair - I/O qpair 할당 + 초기화 (트랜스포트 vtable).
 *
 * @opts:  io_queue_size, delay_cmd_submit(batching), qprio, async_mode 등
 * @return: 생성된 qpair, 실패 시 NULL
 *
 * 호출 순서:
 *   1) nvme_pcie_qpair 구조체 자체 할당 (SHARE hugepage)
 *   2) 기본 필드 세팅 (num_entries, delay_cmd_submit)
 *   3) 공통 qpair_init (공용 리스트, free_req 풀)
 *   4) PCIe qpair_construct (SQ/CQ/tracker 할당, shadow doorbell 등)
 *   5) 어느 단계에서 실패해도 qpair_destroy로 정리 후 NULL 반환
 *
 * 이 함수 후 호출자는 connect_qpair로 Create CQ/SQ admin 명령을 시작해야 실제 사용 가능.
 */
struct spdk_nvme_qpair *
nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				const struct spdk_nvme_io_qpair_opts *opts)
{
	struct nvme_pcie_qpair *pqpair;
	struct spdk_nvme_qpair *qpair;
	int rc;

	assert(ctrlr != NULL);

	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
                                  /* [한국어] PCIe qpair 자체 할당 (64B 정렬 = 캐시라인) */
	if (pqpair == NULL) {
		return NULL;
	}

	pqpair->num_entries = opts->io_queue_size;
                                  /* [한국어] SQ/CQ 엔트리 수 */
	pqpair->flags.delay_cmd_submit = opts->delay_cmd_submit;
                                  /* [한국어] batching 모드 — true면 doorbell 미뤘다가 process_completions에서 flush */

	qpair = &pqpair->qpair;
                                  /* [한국어] 공통 qpair 포인터 (pqpair 내부에 embed된 struct spdk_nvme_qpair) */

	rc = nvme_qpair_init(qpair, qid, ctrlr, opts->qprio, opts->io_queue_requests, opts->async_mode);
                                  /* [한국어] 공통 qpair 초기화 (lib/nvme/nvme_qpair.c) */
	if (rc != 0) {
		nvme_pcie_qpair_destroy(qpair);
                                  /* [한국어] 실패 시 지금까지 할당한 것 정리 */
		return NULL;
	}

	rc = nvme_pcie_qpair_construct(qpair, opts);
                                  /* [한국어] PCIe 특화 — SQ/CQ/tracker 실제 할당 */

	if (rc != 0) {
		nvme_pcie_qpair_destroy(qpair);
		return NULL;
	}

	return qpair;
}

/*
 * [한국어] nvme_pcie_ctrlr_delete_io_qpair - I/O qpair 해제 (Delete SQ/CQ admin + 자원 반환).
 *
 * 이 함수는 비교적 복잡한 시퀀스를 실행:
 *   1) ctrlr이 is_removed면 admin 통신 불가 → 즉시 free로
 *   2) ctrlr->prepare_for_reset이면 Delete SQ/CQ 스킵하고 shadow doorbell만 클리어
 *      (reset 과정에서 모든 IO Q가 사라지므로 개별 삭제 불필요)
 *   3) 정상 경로:
 *      a) Delete IO SQ admin 명령 동기 제출 → 완료 대기
 *      b) SQ 삭제됐으므로 장치가 outstanding I/O를 완료 또는 abort 처리
 *         → 남은 완료를 한 번 폴링해서 수확 시도
 *      c) Delete IO CQ admin 명령 동기 제출 → 완료 대기
 *   4) shadow doorbell 필드 클리어 (재사용 방지)
 *   5) 남은 outstanding tracker를 ABORTED로 일괄 완료
 *   6) nvme_pcie_qpair_destroy로 메모리 해제
 */
int
nvme_pcie_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_completion_poll_status *status;
                                  /* [한국어] 동기 admin 완료 대기 컨텍스트 */
	int rc;

	assert(ctrlr != NULL);

	if (ctrlr->is_removed) {
                                  /* [한국어] 장치 제거됨 — admin 통신 불가, 메모리만 free */
		goto free;
	}

	if (ctrlr->prepare_for_reset) {
                                  /* [한국어] reset 예정 — 곧 ctrlr_disable로 모든 IO Q가 지워지므로 개별 Delete 불필요 */
		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING) {
                                  /* [한국어] 아직 connect 중이면 destroy를 뒤로 미룸 — create_sq_cb가 defer_destruction 감지해 처리 */
			pqpair->flags.defer_destruction = true;
		}
		goto clear_shadow_doorbells;
                                  /* [한국어] shadow doorbell만 클리어하고 메모리 해제로 */
	}

	/* If attempting to delete a qpair that's still being connected, we have to wait until it's
	 * finished, so that we don't free it while it's waiting for the create cq/sq callbacks.
	 */
                                  /* [한국어] connect 진행 중(Create CQ/SQ 대기)이면 완료까지 기다림 — 콜백이 pqpair를 참조하므로 race 방지 */
	while (pqpair->pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_CQ ||
	       pqpair->pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_SQ) {
		rc = spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
                                  /* [한국어] admin 큐 폴링 — 자기 Create CQ/SQ 완료가 거기서 처리 */
		if (rc < 0) {
			break;
                                  /* [한국어] admin 큐 에러 — 더 기다려도 의미 없음 */
		}
	}

	status = calloc(1, sizeof(*status));
                                  /* [한국어] 동기 완료 대기 status 구조체 */
	if (!status) {
		NVME_QPAIR_ERRLOG(qpair, "Failed to allocate status tracker\n");
		goto free;
	}

	/* Delete the I/O submission queue */
	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(ctrlr, qpair, nvme_completion_poll_cb, status);
                                  /* [한국어] ★ 1단계: Delete IO SQ admin 명령 (spec에 따라 SQ 먼저 삭제) */
	if (rc != 0) {
                                  /* [한국어] 제출 자체 실패 */
		NVME_QPAIR_ERRLOG(qpair, "Failed to send request to delete_io_sq with rc=%d\n", rc);
		free(status);
		goto free;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, false);
                                  /* [한국어] admin 큐를 폴링하며 완료 대기 (false=lock 미획득 — 이미 lock 밖 컨텍스트) */
	if (rc) {
		if (!status->timed_out) {
                                  /* [한국어] timeout 아닌 경우에만 status free — timeout이면 장치가 나중에 콜백 호출해서 status 접근 */
			free(status);
		}

		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_pcie_ctrlr_cmd_delete_io_sq failed: rc=%s\n",
				  spdk_strerror(abs(rc)));
		goto free;
	}

	/* Now that the submission queue is deleted, the device is supposed to have
	 * completed any outstanding I/O. Try to complete them. If they don't complete,
	 * they'll be marked as aborted and completed below. */
                                  /* [한국어] SQ 삭제 후 장치는 outstanding I/O들을 ABORTED로 처리 → CQ에 기록되어 있을 것.
                                   *         active_proc 체크: 현재 프로세스가 이 qpair 소유자여야 CQ 접근 안전 */
	if (qpair->active_proc == nvme_ctrlr_get_current_process(ctrlr)) {
		nvme_pcie_qpair_process_completions(qpair, 0);
                                  /* [한국어] 남은 완료 수확 — cb_fn으로 에러 전달 */
	}

	memset(status, 0, sizeof(*status));
                                  /* [한국어] status 재사용 위해 0 초기화 */
	/* Delete the completion queue */
	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(ctrlr, qpair, nvme_completion_poll_cb, status);
                                  /* [한국어] ★ 2단계: Delete IO CQ admin 명령 */
	if (rc != 0) {
		NVME_QPAIR_ERRLOG(qpair, "Failed to send request to delete_io_cq with rc=%d\n", rc);
		free(status);
		goto free;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
                                  /* [한국어] true=status 소유권 이관 — nvme_wait_for_adminq_completion가 free 책임짐 */
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_pcie_ctrlr_cmd_delete_io_cq failed: rc=%s\n",
				  spdk_strerror(abs(rc)));
		goto free;
	}

clear_shadow_doorbells:
	if (pqpair->flags.has_shadow_doorbell && ctrlr->shadow_doorbell) {
                                  /* [한국어] shadow doorbell 필드를 0으로 클리어 — qpair가 재생성되는 경우 stale 값 사용 방지 */
		*pqpair->shadow_doorbell.sq_tdbl = 0;
		*pqpair->shadow_doorbell.cq_hdbl = 0;
		*pqpair->shadow_doorbell.sq_eventidx = 0;
		*pqpair->shadow_doorbell.cq_eventidx = 0;
	}
free:
	if (qpair->no_deletion_notification_needed == 0) {
		/* Abort the rest of the I/O */
                                  /* [한국어] 삭제 알림 생략 아닌 경우 — 남은 outstanding tracker를 abort로 완료 처리 */
		nvme_pcie_qpair_abort_trackers(qpair, 1);
                                  /* [한국어] dnr=1 — 영구 실패 마킹 (재시도 금지) */
	}

	if (!pqpair->flags.defer_destruction) {
                                  /* [한국어] defer_destruction 중이면 create_cb가 나중에 destroy 수행 — 지금은 스킵 */
		nvme_pcie_qpair_destroy(qpair);
                                  /* [한국어] 메모리 해제 */
	}
	return 0;
}

/*
 * [한국어] nvme_pcie_fail_request_bad_vtophys - PRP/SGL 빌드 실패 처리.
 *
 * vtophys 에러 등 payload 관련 치명적 오류 시 호출.
 *
 * 두 경로:
 *   1) in_completion_context==false: submit 경로에서 호출된 경우 — 재진입 방지를 위해
 *      즉시 abort 하지 않고 tr에 bad_vtophys 플래그 세트 → 다음 process_completions가 처리
 *   2) in_completion_context==true: 이미 완료 처리 경로 안에 있음 — 즉시 manual_complete로
 *      INVALID_FIELD 에러 완료 (재시도 금지 dnr=1)
 */
static void
nvme_pcie_fail_request_bad_vtophys(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	if (!qpair->in_completion_context) {
                                  /* [한국어] submit 경로에서 호출 — 즉시 처리하면 재진입 위험 */
		struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

		tr->bad_vtophys = 1;
                                  /* [한국어] 이 tracker를 "실패 후 지연 완료" 대상으로 마킹 */
		pqpair->flags.has_pending_vtophys_failures = 1;
                                  /* [한국어] qpair에 한 개라도 있으면 플래그 세트 — process_completions 끝에서 처리 */
		return;
	}

	/*
	 * Bad vtophys translation, so abort this request and return
	 *  immediately.
	 */
                                  /* [한국어] 완료 경로 안이면 즉시 abort — 무한 재귀 없음 */
	NVME_QPAIR_ERRLOG(qpair, "vtophys or other payload buffer related error\n");
	nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
						SPDK_NVME_SC_INVALID_FIELD,
						1 /* do not retry */, true);
                                  /* [한국어] GENERIC/INVALID_FIELD로 가짜 완료 (재시도 금지) */
}

/*
 * Append PRP list entries to describe a virtually contiguous buffer starting at virt_addr of len bytes.
 *
 * *prp_index will be updated to account for the number of PRP entries used.
 */
/*
 * [한국어] ★★★ nvme_pcie_prp_list_append - 가상 연속 버퍼를 PRP 리스트로 변환 (4KB 페이지 단위) ★★★
 *
 * NVMe의 PRP(Physical Region Page) 규약:
 *   - 각 PRP 엔트리는 64비트 물리 주소 1개.
 *   - PRP1: 첫 페이지 (page-내 offset 허용 — 4KB boundary 정렬 불필요).
 *   - 이후 PRP들: 반드시 4KB 페이지 정렬.
 *   - 2개 이하면 PRP1, PRP2 필드로 직접 표현 (SQE 내부).
 *   - 3개 이상이면 PRP list를 별도 버퍼에 배치, SQE의 PRP2가 그 리스트의 물리 주소를 가리킴.
 *
 * 이 함수는 virt_addr~virt_addr+len의 가상 연속 버퍼를 PRP 엔트리로 변환해 tr->u.prp[]에 채운다.
 * 실제 물리 페이지는 연속 아닐 수 있으므로 각 페이지마다 vtophys 호출.
 *
 * @prp_index: IN/OUT — 현재까지 채운 PRP 개수 (여러 번 호출될 수 있음 — metadata 별도 등)
 *
 * 처리 흐름:
 *   [1] dword align(4B) 검증 — NVMe spec 요구사항
 *   [2] len을 소비하며 루프:
 *       a) tr->u.prp 배열 초과 검사
 *       b) vtophys로 현재 virt_addr의 물리 주소
 *       c) i==0: cmd->dptr.prp.prp1 = 물리 주소, seg_len = 첫 페이지 내 잔여 길이
 *       d) i>0: 페이지 정렬 강제 (page_mask 체크), tr->u.prp[i-1] = 물리 주소, seg_len = 4KB
 *       e) seg_len = min(seg_len, len)
 *       f) virt_addr += seg_len; len -= seg_len; i++
 *   [3] PSDT=PRP로 설정, PRP2 결정:
 *       - i ≤ 1: PRP2 = 0 (첫 페이지로 완결)
 *       - i == 2: PRP2 = u.prp[0] (두 번째 페이지 주소 직접)
 *       - i > 2: PRP2 = prp_sgl_bus_addr (PRP list의 물리 주소)
 */
static inline int
nvme_pcie_prp_list_append(struct spdk_nvme_ctrlr *ctrlr, struct nvme_tracker *tr,
			  uint32_t *prp_index, void *virt_addr, size_t len,
			  uint32_t page_size)
{
	struct spdk_nvme_cmd *cmd = &tr->req->cmd;
                                  /* [한국어] SQE 포인터 — PRP1/PRP2/PSDT 필드 채울 대상 */
	uintptr_t page_mask = page_size - 1;
                                  /* [한국어] 4KB-1=0xFFF — 하위 12비트 마스크 */
	uint64_t phys_addr;
                                  /* [한국어] 현재 페이지 물리 주소 */
	uint32_t i;
                                  /* [한국어] PRP 엔트리 인덱스 */

	NVME_QPAIR_DEBUGLOG(tr->req->qpair, "prp_index:%u virt_addr:%p len:%u\n", *prp_index, virt_addr,
			    (uint32_t)len);

	if (spdk_unlikely(((uintptr_t)virt_addr & 3) != 0)) {
                                  /* [한국어] dword(4B) 정렬 검증 — NVMe spec Section 4.3 */
		NVME_QPAIR_ERRLOG(tr->req->qpair, "virt_addr %p not dword aligned\n", virt_addr);
		return -EFAULT;
	}

	i = *prp_index;
                                  /* [한국어] 시작 인덱스 복원 (이전 호출이 남긴 상태) */
	while (len) {
		uint32_t seg_len;
                                  /* [한국어] 이번 루프에서 커버할 바이트 수 */

		/*
		 * prp_index 0 is stored in prp1, and the rest are stored in the prp[] array,
		 * so prp_index == count is valid.
		 */
		if (spdk_unlikely(i > SPDK_COUNTOF(tr->u.prp))) {
                                  /* [한국어] PRP 배열 overflow — 요청이 너무 커서 MDTS 초과 상태여야 함 (상위가 분할해야 함) */
			NVME_QPAIR_ERRLOG(tr->req->qpair, "out of PRP entries\n");
			return -EFAULT;
		}

		phys_addr = nvme_pcie_vtophys(ctrlr, virt_addr, NULL);
                                  /* [한국어] 가상 → 물리 변환 (hugepage 기반 lookup) */
		if (spdk_unlikely(phys_addr == SPDK_VTOPHYS_ERROR)) {
                                  /* [한국어] 버퍼가 DPDK 힙에 등록되지 않았음 — 치명적 에러 */
			NVME_QPAIR_ERRLOG(tr->req->qpair, "vtophys(%p) failed\n", virt_addr);
			return -EFAULT;
		}

		if (i == 0) {
			NVME_QPAIR_DEBUGLOG(tr->req->qpair, "prp1 = %p\n", (void *)phys_addr);
			cmd->dptr.prp.prp1 = phys_addr;
                                  /* [한국어] 첫 페이지 — PRP1에 직접 기록 */
			seg_len = page_size - ((uintptr_t)virt_addr & page_mask);
                                  /* [한국어] 첫 페이지에서 virt_addr 이후의 잔여 길이 (offset 고려) */
		} else {
			if ((phys_addr & page_mask) != 0) {
                                  /* [한국어] ★ 두 번째 이후 PRP는 반드시 페이지 정렬 — NVMe spec 제약 */
				NVME_QPAIR_ERRLOG(tr->req->qpair, "PRP %u not page aligned (%p)\n", i, virt_addr);
				return -EFAULT;
			}

			NVME_QPAIR_DEBUGLOG(tr->req->qpair, "prp[%u] = %p\n", i - 1, (void *)phys_addr);
			tr->u.prp[i - 1] = phys_addr;
                                  /* [한국어] PRP list 엔트리 저장 (배열 인덱스 i-1, i=1이 배열의 0번) */
			seg_len = page_size;
                                  /* [한국어] 4KB 정렬이므로 한 페이지 전체 사용 */
		}

		seg_len = spdk_min(seg_len, len);
                                  /* [한국어] 남은 len보다 크면 len만큼만 소비 */
		virt_addr = (uint8_t *)virt_addr + seg_len;
                                  /* [한국어] 다음 페이지 시작으로 전진 */
		len -= seg_len;
                                  /* [한국어] 잔여 길이 감소 */
		i++;
                                  /* [한국어] 다음 PRP 인덱스 */
	}

	cmd->psdt = SPDK_NVME_PSDT_PRP;
                                  /* [한국어] Payload Submission Data Type = PRP 모드 */
	if (i <= 1) {
		cmd->dptr.prp.prp2 = 0;
                                  /* [한국어] 한 페이지 이내 — PRP2 미사용 */
	} else if (i == 2) {
		cmd->dptr.prp.prp2 = tr->u.prp[0];
                                  /* [한국어] 두 페이지 — PRP2에 두 번째 페이지 물리 주소 직접 */
		NVME_QPAIR_DEBUGLOG(tr->req->qpair, "prp2 = %p\n", (void *)cmd->dptr.prp.prp2);
	} else {
		cmd->dptr.prp.prp2 = tr->prp_sgl_bus_addr;
                                  /* [한국어] 3페이지 이상 — PRP2에 PRP 리스트의 물리 주소
                                   *         prp_sgl_bus_addr = tracker 구조체 내 u.prp[] 배열의 물리 주소 (construct 시 계산됨) */
		NVME_QPAIR_DEBUGLOG(tr->req->qpair, "prp2 = %p (PRP list)\n", (void *)cmd->dptr.prp.prp2);
	}

	*prp_index = i;
                                  /* [한국어] 최종 사용한 PRP 개수를 호출자에 반환 */
	return 0;
}

/*
 * [한국어] nvme_pcie_qpair_build_request_invalid - payload type 분기 테이블의 sentinel.
 *
 * 유효하지 않은 payload type 조합이 build_req_fn으로 선택된 경우 호출 — 프로그래머 실수.
 * assert(0)로 즉시 abort, 릴리즈 빌드에선 fail_request_bad_vtophys로 에러 완료.
 */
static int
nvme_pcie_qpair_build_request_invalid(struct spdk_nvme_qpair *qpair,
				      struct nvme_request *req, struct nvme_tracker *tr, bool dword_aligned)
{
	assert(0);
                                  /* [한국어] 디버그 빌드: 즉시 abort */
	nvme_pcie_fail_request_bad_vtophys(qpair, tr);
                                  /* [한국어] 릴리즈 빌드: 에러 완료 */
	return -EINVAL;
}

/**
 * Build PRP list describing physically contiguous payload buffer.
 */
/*
 * [한국어] nvme_pcie_qpair_build_contig_request - CONTIG 페이로드 → PRP 리스트 변환 (가장 단순한 경로).
 *
 * payload.contig_or_cb_arg가 가상 연속 버퍼 주소이므로 단 한 번의 prp_list_append로 끝.
 * 장치가 SGL 미지원일 때 기본 경로.
 */
static int
nvme_pcie_qpair_build_contig_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				     struct nvme_tracker *tr, bool dword_aligned)
{
	uint32_t prp_index = 0;
                                  /* [한국어] prp_list_append 시작 인덱스 — 0부터 (메타 등 없음) */
	int rc;

	rc = nvme_pcie_prp_list_append(qpair->ctrlr, tr, &prp_index,
				       (uint8_t *)req->payload.contig_or_cb_arg + req->payload_offset,
				       req->payload_size, qpair->ctrlr->page_size);
                                  /* [한국어] payload_offset 감안 (split 요청의 child는 원본 버퍼의 일부) */
	if (rc) {
		nvme_pcie_fail_request_bad_vtophys(qpair, tr);
                                  /* [한국어] PRP 구성 실패 — 에러 완료 경로 */
	} else {
		NVME_QPAIR_DEBUGLOG(qpair, "Number of PRP entries: %" PRIu32 "\n", prp_index);
	}

	return rc;
}

/**
 * Build an SGL describing a physically contiguous payload buffer.
 *
 * This is more efficient than using PRP because large buffers can be
 * described this way.
 */
/*
 * [한국어] ★ nvme_pcie_qpair_build_contig_hw_sgl_request - CONTIG 페이로드 → SGL (장치 SGL 지원 시).
 *
 * PRP보다 효율적인 이유: SGL descriptor 하나가 임의의 길이를 표현 가능 (PRP는 페이지 단위).
 * 특히 큰 버퍼(>2MB)에서 descriptor 개수가 훨씬 적음.
 *
 * 처리 흐름:
 *   1) PSDT = SGL_MPTR_CONTIG (metadata는 contiguous)
 *   2) 가상 연속 버퍼를 **물리 연속 segment로 분할** — spdk_vtophys로 매핑 길이 조회하며 반복
 *      (hugepage 경계에서 물리 주소 불연속 가능)
 *   3) 각 segment를 SGL descriptor로 기록 (type=DATA_BLOCK, address, length)
 *   4) 최종 nseg에 따라 SQE의 sgl1 결정:
 *      - nseg==1: SGL1이 직접 descriptor (inline 최적화)
 *      - nseg>1:  SGL1은 LAST_SEGMENT — 전체 배열을 tracker에 두고 주소만 가리킴
 */
static int
nvme_pcie_qpair_build_contig_hw_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
		struct nvme_tracker *tr, bool dword_aligned)
{
	uint8_t *virt_addr;
	uint64_t phys_addr, mapping_length;
	uint32_t length;
	struct spdk_nvme_sgl_descriptor *sgl;
	uint32_t nseg = 0;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	sgl = tr->u.sgl;
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.subtype = 0;

	length = req->payload_size;
	/* ubsan complains about applying zero offset to null pointer if contig_or_cb_arg is NULL,
	 * so just double cast it to make it go away */
	virt_addr = (uint8_t *)((uintptr_t)req->payload.contig_or_cb_arg + req->payload_offset);

	while (length > 0) {
		if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		if (dword_aligned && ((uintptr_t)virt_addr & 3)) {
			NVME_QPAIR_ERRLOG(qpair, "virt_addr %p not dword aligned\n", virt_addr);
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		mapping_length = length;
		phys_addr = nvme_pcie_vtophys(qpair->ctrlr, virt_addr, &mapping_length);
		if (phys_addr == SPDK_VTOPHYS_ERROR) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		mapping_length = spdk_min(length, mapping_length);

		length -= mapping_length;
		virt_addr += mapping_length;

		sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		sgl->unkeyed.length = mapping_length;
		sgl->address = phys_addr;
		sgl->unkeyed.subtype = 0;

		sgl++;
		nseg++;
	}

	if (nseg == 1) {
		/*
		 * The whole transfer can be described by a single SGL descriptor.
		 *  Use the special case described by the spec where SGL1's type is Data Block.
		 *  This means the SGL in the tracker is not used at all, so copy the first (and only)
		 *  SGL element into SGL1.
		 */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		req->cmd.dptr.sgl1.address = tr->u.sgl[0].address;
		req->cmd.dptr.sgl1.unkeyed.length = tr->u.sgl[0].unkeyed.length;
	} else {
		/* SPDK NVMe driver supports only 1 SGL segment for now, it is enough because
		 *  NVME_MAX_SGL_DESCRIPTORS * 16 is less than one page.
		 */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		req->cmd.dptr.sgl1.address = tr->prp_sgl_bus_addr;
		req->cmd.dptr.sgl1.unkeyed.length = nseg * sizeof(struct spdk_nvme_sgl_descriptor);
	}

	NVME_QPAIR_DEBUGLOG(qpair, "Number of SGL descriptors: %" PRIu32 "\n", nseg);
	return 0;
}

/**
 * Build SGL list describing scattered payload buffer.
 */
/*
 * [한국어] ★ nvme_pcie_qpair_build_hw_sgl_request - SGL 페이로드 → SGL descriptor 배열.
 *
 * nvme_request.payload가 SGL 모드 (reset_sgl_fn/next_sge_fn 콜백 쌍)일 때 사용.
 * 각 사용자 SGE를 호스트에서 순회하며 각각을 장치용 SGL descriptor로 변환.
 *
 * 추가 최적화: 인접 SGE들의 물리 주소가 연속이면 **병합** (disable_pcie_sgl_merge=false일 때) —
 * 장치에 보내는 descriptor 수 감소 → CQE latency 감소.
 *
 * 특수 케이스: virt_addr == UINT64_MAX는 **Bit Bucket** descriptor 요청 —
 * READ에서 "이 범위의 데이터는 장치에서 읽지만 호스트로 안 보내도 됨" (metadata 건너뛰기 등).
 */
static int
nvme_pcie_qpair_build_hw_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				     struct nvme_tracker *tr, bool dword_aligned)
{
	int rc;
	void *virt_addr;
	uint64_t phys_addr, mapping_length;
	uint32_t remaining_transfer_len, remaining_user_sge_len, length;
	struct spdk_nvme_sgl_descriptor *sgl;
	uint32_t nseg = 0;
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	/*
	 * Build scattered payloads.
	 */
	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	sgl = tr->u.sgl;
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.subtype = 0;

	remaining_transfer_len = req->payload_size;

	while (remaining_transfer_len > 0) {
		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg,
					      &virt_addr, &remaining_user_sge_len);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		/* Bit Bucket SGL descriptor */
		if ((uint64_t)virt_addr == UINT64_MAX) {
			/* TODO: enable WRITE and COMPARE when necessary */
			if (req->cmd.opc != SPDK_NVME_OPC_READ) {
				NVME_QPAIR_ERRLOG(qpair, "Only READ command can be supported\n");
				goto exit;
			}
			if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
				NVME_QPAIR_ERRLOG(qpair, "Too many SGL entries\n");
				goto exit;
			}

			sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_BIT_BUCKET;
			/* If the SGL describes a destination data buffer, the length of data
			 * buffer shall be discarded by controller, and the length is included
			 * in Number of Logical Blocks (NLB) parameter. Otherwise, the length
			 * is not included in the NLB parameter.
			 */
			remaining_user_sge_len = spdk_min(remaining_user_sge_len, remaining_transfer_len);
			remaining_transfer_len -= remaining_user_sge_len;

			sgl->unkeyed.length = remaining_user_sge_len;
			sgl->address = 0;
			sgl->unkeyed.subtype = 0;

			sgl++;
			nseg++;

			continue;
		}

		remaining_user_sge_len = spdk_min(remaining_user_sge_len, remaining_transfer_len);
		remaining_transfer_len -= remaining_user_sge_len;
		while (remaining_user_sge_len > 0) {
			if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
				NVME_QPAIR_ERRLOG(qpair, "Too many SGL entries\n");
				goto exit;
			}

			if (dword_aligned && ((uintptr_t)virt_addr & 3)) {
				NVME_QPAIR_ERRLOG(qpair, "virt_addr %p not dword aligned\n", virt_addr);
				goto exit;
			}

			mapping_length = remaining_user_sge_len;
			phys_addr = nvme_pcie_vtophys(qpair->ctrlr, virt_addr, &mapping_length);
			if (phys_addr == SPDK_VTOPHYS_ERROR) {
				goto exit;
			}

			length = spdk_min(remaining_user_sge_len, mapping_length);
			remaining_user_sge_len -= length;
			virt_addr = (uint8_t *)virt_addr + length;

			if (!pqpair->flags.disable_pcie_sgl_merge && nseg > 0 &&
			    phys_addr == (*(sgl - 1)).address + (*(sgl - 1)).unkeyed.length) {
				/* extend previous entry */
				(*(sgl - 1)).unkeyed.length += length;
				continue;
			}

			sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			sgl->unkeyed.length = length;
			sgl->address = phys_addr;
			sgl->unkeyed.subtype = 0;

			sgl++;
			nseg++;
		}
	}

	if (nseg == 1) {
		/*
		 * The whole transfer can be described by a single SGL descriptor.
		 *  Use the special case described by the spec where SGL1's type is Data Block.
		 *  This means the SGL in the tracker is not used at all, so copy the first (and only)
		 *  SGL element into SGL1.
		 */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		req->cmd.dptr.sgl1.address = tr->u.sgl[0].address;
		req->cmd.dptr.sgl1.unkeyed.length = tr->u.sgl[0].unkeyed.length;
	} else {
		/* SPDK NVMe driver supports only 1 SGL segment for now, it is enough because
		 *  NVME_MAX_SGL_DESCRIPTORS * 16 is less than one page.
		 */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		req->cmd.dptr.sgl1.address = tr->prp_sgl_bus_addr;
		req->cmd.dptr.sgl1.unkeyed.length = nseg * sizeof(struct spdk_nvme_sgl_descriptor);
	}

	NVME_QPAIR_DEBUGLOG(qpair, "Number of SGL descriptors: %" PRIu32 "\n", nseg);
	return 0;

exit:
	nvme_pcie_fail_request_bad_vtophys(qpair, tr);
	return -EFAULT;
}

/**
 * Build PRP list describing scattered payload buffer.
 */
static int
nvme_pcie_qpair_build_prps_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				       struct nvme_tracker *tr, bool dword_aligned)
{
	int rc;
	void *virt_addr;
	uint32_t remaining_transfer_len, length;
	uint32_t prp_index = 0;
	uint32_t page_size = qpair->ctrlr->page_size;

	/*
	 * Build scattered payloads.
	 */
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	remaining_transfer_len = req->payload_size;
	while (remaining_transfer_len > 0) {
		assert(req->payload.next_sge_fn != NULL);
		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &virt_addr, &length);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		length = spdk_min(remaining_transfer_len, length);

		/*
		 * Any incompatible sges should have been handled up in the splitting routine,
		 *  but assert here as an additional check.
		 *
		 * All SGEs except last must end on a page boundary.
		 */
		assert((length == remaining_transfer_len) ||
		       _is_page_aligned((uintptr_t)virt_addr + length, page_size));

		rc = nvme_pcie_prp_list_append(qpair->ctrlr, tr, &prp_index, virt_addr, length, page_size);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return rc;
		}

		remaining_transfer_len -= length;
	}

	NVME_QPAIR_DEBUGLOG(qpair, "Number of PRP entries: %" PRIu32 "\n", prp_index);
	return 0;
}

/*
 * [한국어] build_req_fn - 데이터 payload 빌더 함수 포인터 타입.
 * 모든 빌더가 동일 시그니처 — payload_type과 sgl_supported 기반 분기 테이블용.
 */
typedef int(*build_req_fn)(struct spdk_nvme_qpair *, struct nvme_request *, struct nvme_tracker *,
			   bool);

/*
 * [한국어] ★ g_nvme_pcie_build_req_table - payload 빌더 분기 테이블 (2D).
 *
 * 축 1: payload type (INVALID/CONTIG/SGL — nvme_internal.h 정의)
 * 축 2: 장치 SGL 지원 여부 (0=PRP, 1=SGL)
 *
 * submit_request가 payload type과 sgl_supported 조회 후 이 테이블로 적절한 빌더 O(1) 선택.
 * 4가지 유효 조합:
 *   CONTIG + PRP    → build_contig_request        (단일 연속 버퍼 → PRP 리스트)
 *   CONTIG + SGL    → build_contig_hw_sgl_request (단일 연속 버퍼 → SGL descriptors)
 *   SGL    + PRP    → build_prps_sgl_request      (scatter 버퍼 → PRP, 페이지 정렬 필요)
 *   SGL    + SGL    → build_hw_sgl_request        (scatter 버퍼 → SGL descriptors)
 */
static build_req_fn const g_nvme_pcie_build_req_table[][2] = {
	[NVME_PAYLOAD_TYPE_INVALID] = {
		nvme_pcie_qpair_build_request_invalid,			/* PRP */
		nvme_pcie_qpair_build_request_invalid			/* SGL */
                                  /* [한국어] INVALID type은 두 경로 모두 sentinel (호출되면 assert) */
	},
	[NVME_PAYLOAD_TYPE_CONTIG] = {
		nvme_pcie_qpair_build_contig_request,			/* PRP */
		nvme_pcie_qpair_build_contig_hw_sgl_request		/* SGL */
                                  /* [한국어] CONTIG — 단일 버퍼 */
	},
	[NVME_PAYLOAD_TYPE_SGL] = {
		nvme_pcie_qpair_build_prps_sgl_request,			/* PRP */
		nvme_pcie_qpair_build_hw_sgl_request			/* SGL */
                                  /* [한국어] SGL — scatter 버퍼 (reset_sgl_fn/next_sge_fn 콜백) */
	}
};

/*
 * [한국어] ★ nvme_pcie_qpair_build_metadata - 별도 metadata 페이로드를 SQE의 MPTR에 배치.
 *
 * NVMe에서 metadata 배치는 2가지:
 *   1) interleaved (extended LBA): data와 같은 버퍼에 섞어서 — 이 함수 호출 안 됨
 *   2) separate: 전용 메타 버퍼 → SQE의 MPTR(Metadata Pointer) 또는 별도 SGL
 *
 * 3가지 경로 (SGL 지원/MPTR_SGL 지원/dword alignment 조합):
 *   (a) 장치가 data SGL + MPTR SGL 모두 지원 + 정렬 충족:
 *       PSDT = SGL_MPTR_SGL, tracker->meta_sgl에 SGL descriptor 작성,
 *       MPTR = prp_sgl_bus_addr - sizeof(sgl_descriptor) (배열 직전 위치에 meta SGL이 있다고 약속)
 *   (b) 그 외: MPTR에 metadata 버퍼의 **물리 주소 직접** 기록 (single PRP 방식)
 *   (c) metadata가 여러 물리 페이지에 걸치면 실패 (SPDK가 이 케이스 지원 안 함 — 호출자가 분할 필요)
 */
static int
nvme_pcie_qpair_build_metadata(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
			       bool sgl_supported, bool mptr_sgl_supported, bool dword_aligned)
{
	void *md_payload;
                                  /* [한국어] metadata 가상 주소 (offset 적용 후) */
	struct nvme_request *req = tr->req;
	uint64_t mapping_length;
                                  /* [한국어] vtophys의 in-out 길이 — 연속 물리 영역 길이 조회 */

	if (req->payload.md) {
                                  /* [한국어] metadata 버퍼 존재 시만 진행 */
		md_payload = (uint8_t *)req->payload.md + req->md_offset;
                                  /* [한국어] split 요청의 child를 위한 offset 적용 */
		if (dword_aligned && ((uintptr_t)md_payload & 3)) {
                                  /* [한국어] 4B 정렬 필요한데 위반 */
			NVME_QPAIR_ERRLOG(qpair, "virt_addr %p not dword aligned\n", md_payload);
			goto exit;
		}

		mapping_length = req->md_size;
                                  /* [한국어] 기대 크기 — vtophys가 이보다 짧게 반환하면 물리 연속성 위반 */
		if (sgl_supported && mptr_sgl_supported && dword_aligned) {
                                  /* [한국어] (a) 완전 SGL 경로 */
			assert(req->cmd.psdt == SPDK_NVME_PSDT_SGL_MPTR_CONTIG);
                                  /* [한국어] 직전 빌더가 CONTIG로 세팅했어야 — 이제 SGL로 업그레이드 */
			req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_SGL;
                                  /* [한국어] PSDT 변경 — MPTR도 SGL 방식으로 */

			tr->meta_sgl.address = nvme_pcie_vtophys(qpair->ctrlr, md_payload, &mapping_length);
                                  /* [한국어] metadata 물리 주소 */
			if (tr->meta_sgl.address == SPDK_VTOPHYS_ERROR || mapping_length != req->md_size) {
                                  /* [한국어] vtophys 실패 또는 metadata가 물리 불연속 */
				goto exit;
			}
			tr->meta_sgl.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			tr->meta_sgl.unkeyed.length = req->md_size;
			tr->meta_sgl.unkeyed.subtype = 0;
                                  /* [한국어] meta_sgl descriptor 작성 (16B) */
			req->cmd.mptr = tr->prp_sgl_bus_addr - sizeof(struct spdk_nvme_sgl_descriptor);
                                  /* [한국어] MPTR은 meta_sgl의 **물리 주소** — tr->u.prp/sgl 직전 16B에 meta_sgl이 있음 */
		} else {
                                  /* [한국어] (b) 단순 MPTR=물리주소 경로 */
			req->cmd.mptr = nvme_pcie_vtophys(qpair->ctrlr, md_payload, &mapping_length);
			if (req->cmd.mptr == SPDK_VTOPHYS_ERROR || mapping_length != req->md_size) {
                                  /* [한국어] 실패 또는 불연속 */
				goto exit;
			}
		}
	}

	return 0;

exit:
	nvme_pcie_fail_request_bad_vtophys(qpair, tr);
	return -EINVAL;
}

/*
 * [한국어] ★★★★ nvme_pcie_qpair_submit_request — PCIe 트랜스포트 submit 진입점 ★★★★
 *
 * 트랜스포트 vtable의 qpair_submit_request 구현. nvme_transport.c의 vtable이 여기로 디스패치.
 *
 * 이 함수가 끝나면 나는 결과:
 *   - 성공(0): tracker 할당됨, SQE 빌드됨, SQ에 기록됨, doorbell 눌림 → 장치 처리 시작
 *   - -EAGAIN: tracker 풀 고갈 — 상위가 queued_req에 enqueue해서 나중에 재제출
 *
 * 처리 흐름:
 *
 *   [1] admin 큐면 ctrlr_lock (multi-process 보호)
 *
 *   [2] free_tr에서 tracker 하나 pop:
 *       - 없으면 stat.queued_requests++ + EAGAIN 반환
 *       - 있으면 stat.submitted_requests++ + outstanding_tr로 이동 + QD 증가
 *
 *   [3] tracker에 req 연결 + req->cmd.cid = tr->cid (완료 시 tracker 복원 키)
 *
 *   [4] PSDT 기본값 = PRP (아래 빌더가 SGL이면 덮어씀)
 *
 *   [5] payload_size > 0이면 (실제 데이터 있음):
 *       a) payload_type 조회 (CONTIG/SGL/INVALID)
 *       b) sgl_supported 판정 (장치 기능 플래그 + admin 큐 아님):
 *          - **Admin은 PCIe에서 항상 PRP 사용** (NVMe spec 요구)
 *       c) DSM quirk 검사 — 일부 장치가 DSM에 SGL 미지원
 *       d) dword alignment 조건 확인
 *       e) ★ build_req_fn 테이블로 데이터 페이로드 빌드 (PRP 또는 SGL)
 *       f) build_metadata로 metadata 페이로드 빌드 (별도 MD 있으면)
 *       - 실패 시 이미 bad_vtophys 경로로 처리됨 — rc=0으로 리턴 (상위에 -EFAULT 전파 금지)
 *
 *   [6] ★★★ nvme_pcie_qpair_submit_tracker — 실제 SQ에 SQE 기록 + doorbell ring
 *
 *   [7] admin 락 해제
 *
 * 에러 처리 철학: submit 중 PRP/SGL 빌드 실패는 cb_fn 경로로만 전파 (submit은 0 반환).
 *                 이 규칙이 상위 API의 "submit은 큐잉 성공 여부만 반환" 계약을 유지.
 */
int
nvme_pcie_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_tracker	*tr;
                                  /* [한국어] free_tr에서 pop해올 tracker */
	int			rc = 0;
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	enum nvme_payload_type	payload_type;
                                  /* [한국어] CONTIG/SGL/INVALID 판정 */
	bool			sgl_supported;
                                  /* [한국어] 장치가 이 요청에 SGL 쓸 수 있는지 (기능 + admin 제외 + quirk) */
	bool			mptr_sgl_supported;
                                  /* [한국어] MPTR(metadata) 용도로 SGL 쓸 수 있는지 */
	bool			dword_aligned = true;
                                  /* [한국어] 4B 정렬 강제 여부 — SGL_REQUIRES_DWORD_ALIGNMENT 플래그 기반 */

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
                                  /* [한국어] admin 큐는 multi-process 공유이므로 lock 보호 */
		nvme_ctrlr_lock(ctrlr);
	}

	tr = TAILQ_FIRST(&pqpair->free_tr);
                                  /* [한국어] free 풀의 HEAD (LIFO — 최근 사용 캐시 hot) */

	if (tr == NULL) {
                                  /* [한국어] 풀 고갈 — outstanding이 가득 참 */
		pqpair->stat->queued_requests++;
                                  /* [한국어] 통계 */
		/* Inform the upper layer to try again later. */
		rc = -EAGAIN;
                                  /* [한국어] 상위에 "나중에 다시 해달라" 신호 (nvme_qpair.c가 queued_req에 enqueue) */
		goto exit;
	}

	pqpair->stat->submitted_requests++;
                                  /* [한국어] 제출 통계 */
	TAILQ_REMOVE(&pqpair->free_tr, tr, tq_list); /* remove tr from free_tr */
                                  /* [한국어] free 풀에서 제거 */
	TAILQ_INSERT_TAIL(&pqpair->outstanding_tr, tr, tq_list);
                                  /* [한국어] outstanding 리스트에 TAIL 삽입 (FIFO — 완료 순서 추적) */
	pqpair->qpair.queue_depth++;
                                  /* [한국어] QD 증가 */
	tr->req = req;
                                  /* [한국어] tracker ↔ req 연결 */
	tr->cb_fn = req->cb_fn;
	tr->cb_arg = req->cb_arg;
                                  /* [한국어] 완료 콜백 캐시 (req가 완료 처리 중 free되어도 tracker에서 호출 가능하도록) */
	req->cmd.cid = tr->cid;
                                  /* [한국어] ★ SQE의 cid = tracker의 고정 cid — 장치가 CQE로 그대로 반환 → tracker 복원 키 */
	/* Use PRP by default. This bit will be overridden below if needed. */
	req->cmd.psdt = SPDK_NVME_PSDT_PRP;
                                  /* [한국어] PSDT 기본값 — SGL 빌더가 호출되면 그 안에서 덮어씀 */

	if (req->payload_size != 0) {
                                  /* [한국어] 데이터 페이로드가 있는 경우 (FLUSH, write_zeroes 등은 size=0) */
		payload_type = nvme_payload_type(&req->payload);
                                  /* [한국어] payload 구조체 내부 플래그로 CONTIG/SGL 구별 */
		/* According to the specification, PRPs shall be used for all
		 *  Admin commands for NVMe over PCIe implementations.
		 */
                                  /* [한국어] NVMe spec: PCIe admin 큐는 PRP 전용 — sgl_supported를 admin 큐면 강제로 false */
		sgl_supported = (ctrlr->flags & SPDK_NVME_CTRLR_SGL_SUPPORTED) != 0 &&
				!nvme_qpair_is_admin_queue(qpair);
		mptr_sgl_supported = (ctrlr->flags & SPDK_NVME_CTRLR_MPTR_SGL_SUPPORTED) != 0 &&
				     !nvme_qpair_is_admin_queue(qpair);

		if (sgl_supported) {
			/* Don't use SGL for DSM command */
			if (spdk_unlikely((ctrlr->quirks & NVME_QUIRK_NO_SGL_FOR_DSM) &&
					  (req->cmd.opc == SPDK_NVME_OPC_DATASET_MANAGEMENT))) {
                                  /* [한국어] 특정 장치의 DSM+SGL 호환성 이슈 quirk — 이 조합은 PRP로 강제 */
				sgl_supported = false;
			}
		}

		if (sgl_supported && !(ctrlr->flags & SPDK_NVME_CTRLR_SGL_REQUIRES_DWORD_ALIGNMENT)) {
                                  /* [한국어] SGL 쓰되 정렬 제약 없는 장치면 dword_aligned=false로 완화 */
			dword_aligned = false;
		}

		/* If we fail to build the request or the metadata, do not return the -EFAULT back up
		 * the stack.  This ensures that we always fail these types of requests via a
		 * completion callback, and never in the context of the submission.
		 */
                                  /* [한국어] ★ 중요한 에러 처리 규약:
                                   *         build 실패 시 -EFAULT를 상위에 전파하지 않고 cb_fn 경로로만 처리.
                                   *         이유: 사용자 API의 "submit은 큐잉 여부만 반환" 계약 유지 + 완료 처리 경로 일관성 */
		rc = g_nvme_pcie_build_req_table[payload_type][sgl_supported](qpair, req, tr, dword_aligned);
                                  /* [한국어] ★ 분기 테이블로 적절한 빌더 선택 → PRP/SGL 구성 */
		if (rc < 0) {
			assert(rc == -EFAULT);
                                  /* [한국어] 빌더는 -EFAULT만 반환 (다른 에러 코드 없음) */
			rc = 0;
                                  /* [한국어] 상위에 성공처럼 보이게 (실제 실패는 fail_request_bad_vtophys가 큐잉했음) */
			goto exit;
		}

		rc = nvme_pcie_qpair_build_metadata(qpair, tr, sgl_supported, mptr_sgl_supported, dword_aligned);
                                  /* [한국어] metadata (별도 MD) 빌드 */
		if (rc < 0) {
			assert(rc == -EFAULT);
			rc = 0;
			goto exit;
		}
	}

	nvme_pcie_qpair_submit_tracker(qpair, tr);
                                  /* [한국어] ★★★ 실제 SQ 기록 + doorbell ring — 이 시점 이후 장치 처리 시작 */

exit:
	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		nvme_ctrlr_unlock(ctrlr);
                                  /* [한국어] admin lock 해제 */
	}

	return rc;
                                  /* [한국어] 0 (성공) 또는 -EAGAIN (tracker 고갈) */
}

/*
 * [한국어] ★ PCIe poll group 10종 (트랜스포트 vtable 구현) ★
 *
 * RDMA/TCP와 달리 PCIe는 **comp_channel 없는 순차 폴링** 모델:
 *   - 각 qpair를 순차 순회하며 spdk_nvme_qpair_process_completions 호출
 *   - 여러 qpair를 한 번에 ibv_poll_cq 같은 집합 연산으로 드레인할 수 없음
 *   - 그래서 대부분의 훅(connect/disconnect_qpair/add)이 거의 no-op
 *
 * 유일한 실질 동작: process_completions가 connected_qpairs를 순회하며 개별 폴링.
 */

/*
 * [한국어] nvme_pcie_poll_group_create - 트랜스포트 vtable의 poll_group_create 구현.
 */
struct spdk_nvme_transport_poll_group *
nvme_pcie_poll_group_create(void)
{
	struct nvme_pcie_poll_group *group = calloc(1, sizeof(*group));
                                  /* [한국어] calloc (일반 힙) — poll_group은 호스트 전용, DMA 불필요 */

	if (group == NULL) {
		SPDK_ERRLOG("Unable to allocate poll group.\n");
		return NULL;
	}

	return &group->group;
                                  /* [한국어] 상위(transport_poll_group_create)는 spdk_nvme_transport_poll_group*를 기대 —
                                   *         nvme_pcie_poll_group이 이를 embed하고 있어 & 하나로 변환 */
}

/*
 * [한국어] nvme_pcie_poll_group_connect_qpair - no-op (PCIe는 group connect 불필요).
 */
int
nvme_pcie_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
                                  /* [한국어] PCIe qpair는 Create SQ/CQ admin 커맨드로 이미 장치에 연결됨 —
                                   *         poll_group 레벨에서 추가 작업 없음 */
}

/*
 * [한국어] nvme_pcie_poll_group_disconnect_qpair - no-op.
 */
int
nvme_pcie_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
                                  /* [한국어] connect와 대칭 — PCIe는 group 레벨 disconnect 불필요 */
}

/*
 * [한국어] nvme_pcie_poll_group_add - no-op.
 * 상위(transport_poll_group_add)가 이미 STAILQ에 추가했으므로 트랜스포트 할 일 없음.
 */
int
nvme_pcie_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			 struct spdk_nvme_qpair *qpair)
{
	return 0;
                                  /* [한국어] 상위 transport 레이어가 리스트 관리 책임 */
}

/*
 * [한국어] nvme_pcie_poll_group_remove - qpair를 group에서 제거할 때 stat 포인터 재조정.
 *
 * qpair가 group 공유 stats를 쓰고 있었는데 이제 제거되므로, hot-path 코드가 NULL 체크 없이
 * pqpair->stat->xxx++를 안전하게 할 수 있도록 g_dummy_stat로 리다이렉트.
 */
int
nvme_pcie_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
			    struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	pqpair->stat = &g_dummy_stat;
                                  /* [한국어] dummy로 리다이렉트 — NULL 포인터 deref 방지 (hot-path 분기 생략) */
	return 0;
}

/*
 * [한국어] ★ nvme_pcie_poll_group_process_completions - group 순차 폴링.
 *
 * 두 루프:
 *   1) disconnected_qpairs 순회 → 각 qpair에 대해 disconnected_qpair_cb 호출
 *      (애플리케이션이 재연결 결정)
 *   2) connected_qpairs 순회 → 각각 spdk_nvme_qpair_process_completions 호출
 *      - ENXIO 등 에러 발생하면 그 qpair는 disconnected로 이동 + cb 호출
 *      - 성공하면 개수 누적
 *
 * 반환값: 성공 시 처리 개수 합계, 한 qpair라도 실패하면 -ENXIO 유지.
 */
int64_t
nvme_pcie_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_qpair *qpair, *tmp_qpair;
	int32_t local_completions = 0;
	int64_t total_completions = 0;

	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
                                  /* [한국어] 이미 disconnected된 qpair에 대해 cb 호출 (SAFE — cb가 리스트 변경 가능) */
		disconnected_qpair_cb(qpair, tgroup->group->ctx);
	}

	STAILQ_FOREACH_SAFE(qpair, &tgroup->connected_qpairs, poll_group_stailq, tmp_qpair) {
                                  /* [한국어] 연결된 qpair 순회 */
		local_completions = spdk_nvme_qpair_process_completions(qpair, completions_per_qpair);
                                  /* [한국어] 상위 API 호출 — nvme_qpair.c의 process_completions → 이 파일의 process_completions */
		if (spdk_unlikely(local_completions < 0)) {
                                  /* [한국어] 에러 (보통 -ENXIO): qpair 문제 상태 — disconnected로 이동시키기 위해 cb 호출 */
			disconnected_qpair_cb(qpair, tgroup->group->ctx);
			total_completions = -ENXIO;
                                  /* [한국어] 전체 리턴을 에러로 마킹 (이후 성공해도 복원 안 함) */
		} else if (spdk_likely(total_completions >= 0)) {
                                  /* [한국어] 아직 에러 없음 — 개수 누적 */
			total_completions += local_completions;
		}
	}

	return total_completions;
}

/*
 * [한국어] nvme_pcie_poll_group_check_disconnected_qpairs - disconnected 리스트 주기적 cb 호출.
 *
 * process_completions과 달리 connected 순회 없이 disconnected만 훑어봄. 상위 poll_group이 주기 호출.
 */
void
nvme_pcie_poll_group_check_disconnected_qpairs(struct spdk_nvme_transport_poll_group *tgroup,
		spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_qpair *qpair, *tmp_qpair;

	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
		disconnected_qpair_cb(qpair, tgroup->group->ctx);
                                  /* [한국어] 애플리케이션 콜백 — 재연결 시도/해제 결정 */
	}
}

/*
 * [한국어] nvme_pcie_poll_group_destroy - poll group 해제.
 * qpair가 남아 있으면 -EBUSY.
 */
int
nvme_pcie_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	if (!STAILQ_EMPTY(&tgroup->connected_qpairs) || !STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
                                  /* [한국어] 아직 qpair 소속돼 있으면 해제 불가 */
		return -EBUSY;
	}

	free(tgroup);
                                  /* [한국어] calloc과 대응 (일반 힙) */

	return 0;
}

/*
 * [한국어] nvme_pcie_poll_group_get_stats - PCIe poll group 통계 조회.
 *
 * @_stats: OUT — calloc으로 할당된 새 구조체 (호출자가 free_stats로 해제)
 * 용도: 애플리케이션이 PCIe 특화 카운터(polls, completions, idle_polls 등) 모니터링.
 */
int
nvme_pcie_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
			       struct spdk_nvme_transport_poll_group_stat **_stats)
{
	struct nvme_pcie_poll_group *group;
	struct spdk_nvme_transport_poll_group_stat *stats;

	if (tgroup == NULL || _stats == NULL) {
		SPDK_ERRLOG("Invalid stats or group pointer\n");
		return -EINVAL;
	}

	stats = calloc(1, sizeof(*stats));
	if (!stats) {
		SPDK_ERRLOG("Can't allocate memory for stats\n");
		return -ENOMEM;
	}
	stats->trtype = SPDK_NVME_TRANSPORT_PCIE;
                                  /* [한국어] 트랜스포트 식별자 — 호출자가 union 필드 중 pcie를 읽을 것 */
	group = SPDK_CONTAINEROF(tgroup, struct nvme_pcie_poll_group, group);
                                  /* [한국어] 다운캐스트 — 내부 nvme_pcie_poll_group으로 */
	memcpy(&stats->pcie, &group->stats, sizeof(group->stats));
                                  /* [한국어] PCIe 카운터 구조체 복사 (스냅샷) */

	*_stats = stats;

	return 0;
}

/*
 * [한국어] nvme_pcie_poll_group_free_stats - get_stats의 짝.
 */
void
nvme_pcie_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
				struct spdk_nvme_transport_poll_group_stat *stats)
{
	free(stats);
                                  /* [한국어] calloc 했으므로 free */
}

/*
 * [한국어] nvme_pcie_trace - NVMe PCIe 트레이스 포인트 등록.
 *
 * SPDK trace 인프라에 "NVME_PCIE_SUBMIT" / "NVME_PCIE_COMPLETE" 추적점 등록.
 * 각 이벤트의 인자 구성과 타입을 선언하여 `spdk_trace` 도구가 나중에 해석 가능.
 *
 * 호출 시점: SPDK_TRACE_REGISTER_FN 매크로가 만드는 constructor가 main() 전에 자동 호출.
 */
static void
nvme_pcie_trace(void)
{
	struct spdk_trace_tpoint_opts opts[] = {
                                  /* [한국어] 두 개 추적점 정의 */
		{
			"NVME_PCIE_SUBMIT", TRACE_NVME_PCIE_SUBMIT,
			OWNER_TYPE_NVME_PCIE_QP, OBJECT_NVME_PCIE_REQ, 1,
                                  /* [한국어] SUBMIT 이벤트 — qpair 소유, request 객체 생성 (new_obj=1) */
			{	{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "cid", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "opc", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "dw10", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "dw11", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "dw12", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }
                                  /* [한국어] cb_arg/cid/opc/CDW10-12/QD 저장 — latency 분석 + opcode별 빈도 */
			}
		},
		{
			"NVME_PCIE_COMPLETE", TRACE_NVME_PCIE_COMPLETE,
			OWNER_TYPE_NVME_PCIE_QP, OBJECT_NVME_PCIE_REQ, 0,
                                  /* [한국어] COMPLETE 이벤트 — 객체 소멸 (new_obj=0) */
			{	{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "cid", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "cpl", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }
			}
		},
	};

	spdk_trace_register_object(OBJECT_NVME_PCIE_REQ, 'p');
                                  /* [한국어] 객체 타입 등록 ('p' = PCIe req 식별자) */
	spdk_trace_register_owner_type(OWNER_TYPE_NVME_PCIE_QP, 'q');
                                  /* [한국어] 소유자 타입 등록 ('q' = qpair) */
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
                                  /* [한국어] 추적점 배열 등록 */
}
SPDK_TRACE_REGISTER_FN(nvme_pcie_trace, "nvme_pcie", TRACE_GROUP_NVME_PCIE)
                                  /* [한국어] SPDK 트레이스 인프라에 이 함수를 constructor로 등록 매크로.
                                   *         "nvme_pcie" 그룹 이름 + TRACE_GROUP_NVME_PCIE ID로 내부 관리 */
