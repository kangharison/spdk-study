/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK NVMe 드라이버 내부 자료구조 · 프로토타입 (nvme_internal.h) — 1839 라인
 *
 * === 파일의 역할 ===
 * lib/nvme/ 구현 내부에서만 사용하는 자료구조와 함수 프로토타입을 모은 헤더.
 * 공개 API(include/spdk/nvme.h)가 opaque로 노출한 `spdk_nvme_ctrlr`,
 * `spdk_nvme_qpair`, `spdk_nvme_ns`의 실제 필드 정의가 이 파일에 있다.
 * NVMe 드라이버의 "심장" — I/O 경로의 중간 객체 `struct nvme_request`, 요청
 * 페이로드 추상화 `struct nvme_payload`, qpair 상태머신, 컨트롤러 process
 * 공유 상태 등이 전부 포함된다.
 *
 * 주요 내용 (라인 기준 대략적):
 *   - 벤더 quirk 상수 (NVME_INTEL_QUIRK_*, NVME_QUIRK_*) — 특정 HW 대응
 *   - enum nvme_payload_type (line 207) — CONTIG vs SGL
 *   - struct nvme_payload (line 228) — 요청 데이터 페이로드 기술자
 *   - NVME_PAYLOAD_CONTIG / SGL 매크로 — payload 구조체 초기화 편의
 *   - struct nvme_error_cmd — 에러 주입 명령 메타
 *   - ★ struct nvme_request (line 282) ★ — 단일 I/O 요청 객체 (bdev_io
 *     ↔ SQE 사이의 브리지). 재시도, split(parent/children), 페이로드,
 *     콜백, 연관 qpair 등을 담는다.
 *   - struct nvme_completion_poll_status — 동기 대기 패턴용 상태 블록
 *   - struct nvme_async_event_request — AER 전용 래퍼
 *   - enum nvme_qpair_state (line 412) — qpair 연결 상태머신
 *   - ★ struct spdk_nvme_qpair (line 464) ★ — 공개 타입의 내부 필드 정의
 *     (trid, transport, sq head/tail, free/outstanding req 리스트 등)
 *   - struct spdk_nvme_ns (line 570) — 네임스페이스 내부 표현
 *   - struct spdk_nvme_ctrlr_aer_completion — Async Event 완료 캐시
 *   - struct spdk_nvme_ctrlr_process (line 983) — multi-process 지원:
 *     공유 컨트롤러에 대해 각 프로세스가 갖는 per-process 상태
 *   - ★ struct spdk_nvme_ctrlr (line 1029) ★ — 컨트롤러 전체 상태
 *   - 트랜스포트 추상화 프로토타입 (nvme_transport_*)
 *   - qpair 초기화/파괴 헬퍼
 *   - request 할당/해제/init (spdk_nvme_allocate_request_* 등)
 *   - qpair 제출/완료 경로 공용 헬퍼
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 흐름:
 *   [User API] spdk_nvme_ns_cmd_read(qpair, buf, lba, nlb, cb, cb_arg, flags)
 *     ↓
 *   lib/nvme/nvme_ns_cmd.c: 파라미터 검증 → `nvme_allocate_request(…)` [이 헤더]
 *     → `struct nvme_request` 획득, SQE 필드 채움(opc=READ, nsid, LBA 등)
 *     ↓
 *   `nvme_qpair_submit_request(qpair, req)` [이 헤더]
 *     ↓
 *   transport dispatch → `nvme_pcie_qpair_submit_request(...)` (pcie_internal.h)
 *     → tracker 할당, PRP/SGL 빌드, SQE를 SQ에 기록 → doorbell ring
 *
 * 완료 경로:
 *   `spdk_nvme_qpair_process_completions(qpair, max)` [공개 API]
 *     → transport dispatch → `nvme_pcie_qpair_process_completions`
 *       → CQE 파싱 → tracker → 원 nvme_request 복원 → req->cb_fn 호출
 *
 * 실행 컨텍스트: 각 qpair는 소유 SPDK thread에 고정. 해당 스레드에서만
 * request 제출·완료 루틴이 실행됨. 컨트롤러 레벨 관리(감지, reset)는
 * 별도 admin qpair 경로 또는 전용 "주 스레드"에서 수행.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/nvme.h (공개 API)
 *   - spdk/nvme_spec.h (SQE/CQE/…, 공개 헤더 이미 포함됨)
 *   - spdk/queue.h, tree.h, bit_array.h, barrier.h, mmio.h, util.h, memory.h
 *   - spdk/log.h, spdk_internal/assert.h
 *   - x86intrin.h (x86/x86_64에서 벤더 확장 인트린직 사용)
 * 의존하는 모듈(구현 파일 기준): lib/nvme/* 전체, lib/nvme/nvme_pcie*.c,
 *   nvme_fabric.c, nvme_rdma.c, nvme_tcp.c, nvme_cuse.c 등 모든 트랜스포트·부속.
 *
 * 공유 자료구조:
 *   - g_spdk_nvme_pid: SPDK 초기화 시점의 PID (process 식별용)
 *   - g_spdk_nvme_transport_opts: 전역 트랜스포트 옵션
 *   - 컨트롤러 전역 리스트(trid_list 등)는 뮤텍스 보호
 *   - spdk_nvme_ctrlr: 다수 프로세스·다수 qpair가 공유 가능 (ctrlr_process 경유)
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct nvme_request: I/O 경로 **중간 객체**. 공개 API가 생성하고
 *     트랜스포트 계층이 소비. `cmd` 필드에 곧 SQE가 담긴다.
 *   - struct nvme_payload: CONTIG 단일 버퍼 또는 SGL 콜백 쌍
 *   - struct spdk_nvme_qpair: qpair 공유 상태 — 상태머신, request 리스트
 *   - struct spdk_nvme_ctrlr: 전역 컨트롤러 상태 — ID, quirks, opts, ns 배열
 *   - nvme_transport_*: 트랜스포트 공용 인터페이스 프로토타입
 *   - nvme_allocate_request / nvme_free_request / nvme_request_init: req 수명
 *   - nvme_qpair_submit_request / nvme_qpair_complete_completion: 제출/완료
 *
 * 본 주석은 상단 블록 + 핵심 I/O 경로 타입(`nvme_payload`, `nvme_request`)의
 * 필드 수준 한국어 주석을 제공한다. qpair/ctrlr/ns 및 트랜스포트 프로토타입은
 * 후속 세션에서 점진적으로 확장한다.
 */
#ifndef __NVME_INTERNAL_H__      /* [한국어] include 가드 */
#define __NVME_INTERNAL_H__

#include "spdk/config.h"         /* [한국어] 빌드 구성 매크로 (ISA-L, RDMA 등 조건부 기능) */
#include "spdk/likely.h"         /* [한국어] spdk_likely/unlikely — hot path 분기 힌트 */
#include "spdk/stdinc.h"         /* [한국어] 표준 타입 */

#include "spdk/nvme.h"           /* [한국어] 공개 타입 정의 — 이 헤더는 공개 타입을 "완성"시킨다 */

#if defined(__i386__) || defined(__x86_64__)
#include <x86intrin.h>
#endif

#include "spdk/queue.h"
#include "spdk/barrier.h"
#include "spdk/bit_array.h"
#include "spdk/mmio.h"
#include "spdk/pci_ids.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/nvme_intel.h"
#include "spdk/nvmf_spec.h"
#include "spdk/tree.h"
#include "spdk/uuid.h"
#include "spdk/fd_group.h"
#include "spdk/string.h"

#include "spdk_internal/assert.h"
#include "spdk/log.h"

extern pid_t g_spdk_nvme_pid;

extern struct spdk_nvme_transport_opts g_spdk_nvme_transport_opts;

/*
 * Some Intel devices support vendor-unique read latency log page even
 * though the log page directory says otherwise.
 */
#define NVME_INTEL_QUIRK_READ_LATENCY 0x1

/*
 * Some Intel devices support vendor-unique write latency log page even
 * though the log page directory says otherwise.
 */
#define NVME_INTEL_QUIRK_WRITE_LATENCY 0x2

/*
 * The controller needs a delay before starts checking the device
 * readiness, which is done by reading the NVME_CSTS_RDY bit.
 */
#define NVME_QUIRK_DELAY_BEFORE_CHK_RDY	0x4

/*
 * The controller performs best when I/O is split on particular
 * LBA boundaries.
 */
#define NVME_INTEL_QUIRK_STRIPING 0x8

/*
 * The controller needs a delay after allocating an I/O queue pair
 * before it is ready to accept I/O commands.
 */
#define NVME_QUIRK_DELAY_AFTER_QUEUE_ALLOC 0x10

/*
 * Earlier NVMe devices do not indicate whether unmapped blocks
 * will read all zeroes or not. This define indicates that the
 * device does in fact read all zeroes after an unmap event
 */
#define NVME_QUIRK_READ_ZERO_AFTER_DEALLOCATE 0x20

/*
 * The controller doesn't handle Identify value others than 0 or 1 correctly.
 */
#define NVME_QUIRK_IDENTIFY_CNS 0x40

/*
 * The controller supports Open Channel command set if matching additional
 * condition, like the first byte (value 0x1) in the vendor specific
 * bits of the namespace identify structure is set.
 */
#define NVME_QUIRK_OCSSD 0x80

/*
 * The controller has an Intel vendor ID but does not support Intel vendor-specific
 * log pages.  This is primarily for QEMU emulated SSDs which report an Intel vendor
 * ID but do not support these log pages.
 */
#define NVME_INTEL_QUIRK_NO_LOG_PAGES 0x100

/*
 * The controller does not set SHST_COMPLETE in a reasonable amount of time.  This
 * is primarily seen in virtual VMWare NVMe SSDs.  This quirk merely adds an additional
 * error message that on VMWare NVMe SSDs, the shutdown timeout may be expected.
 */
#define NVME_QUIRK_SHST_COMPLETE 0x200

/*
 * The controller requires an extra delay before starting the initialization process
 * during attach.
 */
#define NVME_QUIRK_DELAY_BEFORE_INIT 0x400

/*
 * Some SSDs exhibit poor performance with the default SPDK NVMe IO queue size.
 * This quirk will increase the default to 1024 which matches other operating
 * systems, at the cost of some extra memory usage.  Users can still override
 * the increased default by changing the spdk_nvme_io_qpair_opts when allocating
 * a new queue pair.
 */
#define NVME_QUIRK_MINIMUM_IO_QUEUE_SIZE 0x800

/**
 * The maximum access width to PCI memory space is 8 Bytes, don't use AVX2 or
 * SSE instructions to optimize the memory access(memcpy or memset) larger than
 * 8 Bytes.
 */
#define NVME_QUIRK_MAXIMUM_PCI_ACCESS_WIDTH 0x1000

/**
 * The SSD does not support OPAL even through it sets the security bit in OACS.
 */
#define NVME_QUIRK_OACS_SECURITY 0x2000

/**
 * Intel P55XX SSDs can't support Dataset Management command with SGL format,
 * so use PRP with DSM command.
 */
#define NVME_QUIRK_NO_SGL_FOR_DSM 0x4000

/**
 * Maximum Data Transfer Size(MDTS) excludes interleaved metadata.
 */
#define NVME_QUIRK_MDTS_EXCLUDE_MD 0x8000

/**
 * Force not to use SGL even the controller report that it can
 * support it.
 */
#define NVME_QUIRK_NOT_USE_SGL 0x10000

/*
 * Some SSDs require the admin submission queue size to equate to an even
 * 4KiB multiple.
 */
#define NVME_QUIRK_MINIMUM_ADMIN_QUEUE_SIZE 0x20000

/*
 * Some Micron SSD models do not allocate an extra MSI-X vector for the admin
 * queue.
 */
#define NVME_QUIRK_MSIX_VECTOR_COUNT 0x40000

#define NVME_MAX_ASYNC_EVENTS	(8)

#define NVME_MAX_ADMIN_TIMEOUT_IN_SECS	(30)

/* Maximum log page size to fetch for AERs. */
#define NVME_MAX_AER_LOG_SIZE		(4096)

/*
 * NVME_MAX_IO_QUEUES in nvme_spec.h defines the 64K spec-limit, but this
 *  define specifies the maximum number of queues this driver will actually
 *  try to configure, if available.
 */
#define DEFAULT_MAX_IO_QUEUES		(1024)
#define MAX_IO_QUEUES_WITH_INTERRUPTS	(256)
#define DEFAULT_ADMIN_QUEUE_SIZE	(32)
#define DEFAULT_IO_QUEUE_SIZE		(256)
#define DEFAULT_IO_QUEUE_SIZE_FOR_QUIRK	(1024) /* Matches Linux kernel driver */

#define DEFAULT_IO_QUEUE_REQUESTS	(512)

#define SPDK_NVME_DEFAULT_RETRY_COUNT	(4)

#define SPDK_NVME_TRANSPORT_ACK_TIMEOUT_DISABLED	(0)
#define SPDK_NVME_DEFAULT_TRANSPORT_ACK_TIMEOUT	SPDK_NVME_TRANSPORT_ACK_TIMEOUT_DISABLED

#define SPDK_NVME_TRANSPORT_TOS_DISABLED	(0)

#define MIN_KEEP_ALIVE_TIMEOUT_IN_MS	(10000)

/* We want to fit submission and completion rings each in a single 2MB
 * hugepage to ensure physical address contiguity.
 */
#define MAX_IO_QUEUE_ENTRIES		(VALUE_2MB / spdk_max( \
						sizeof(struct spdk_nvme_cmd), \
						sizeof(struct spdk_nvme_cpl)))

/* Default timeout for fabrics connect commands. */
#ifdef DEBUG
#define NVME_FABRIC_CONNECT_COMMAND_TIMEOUT 0
#else
/* 500 millisecond timeout. */
#define NVME_FABRIC_CONNECT_COMMAND_TIMEOUT 500000
#endif

/* This value indicates that a read from a PCIe register is invalid. This can happen when a device is no longer present */
#define SPDK_NVME_INVALID_REGISTER_VALUE 0xFFFFFFFFu

/* [한국어] 요청 페이로드 표현 방식
 *  - CONTIG: 단일 연속 가상 메모리 버퍼. PRP 빌드 시 호스트가 직접 주소를 계산
 *  - SGL: 여러 조각의 버퍼를 콜백으로 enumerate. scatter-gather 임의 분할 지원 */
enum nvme_payload_type {
	NVME_PAYLOAD_TYPE_INVALID = 0,
                                  /* [한국어] 미초기화/무효 상태 */

	/** nvme_request::u.payload.contig_buffer is valid for this request */
	NVME_PAYLOAD_TYPE_CONTIG,
                                  /* [한국어] 단일 연속 버퍼 — payload.contig_or_cb_arg가 버퍼의 가상 주소
                                   *  - 대부분의 간단한 read/write 경로 (bdev_nvme가 내부 pinned buffer 사용 시) */

	/** nvme_request::u.sgl is valid for this request */
	NVME_PAYLOAD_TYPE_SGL,
                                  /* [한국어] SGL 콜백 모드 — reset_sgl_fn/next_sge_fn으로 세그먼트 순차 enumerate
                                   *  - 사용자 정의 데이터 분산(application 버퍼·ring buffer 등) 직접 전달 */
};

/** Boot partition write states */
enum nvme_bp_write_state {
	SPDK_NVME_BP_WS_DOWNLOADING	= 0x0,
	SPDK_NVME_BP_WS_DOWNLOADED	= 0x1,
	SPDK_NVME_BP_WS_REPLACE		= 0x2,
	SPDK_NVME_BP_WS_ACTIVATE	= 0x3,
};

/**
 * Descriptor for a request data payload.
 */
/*
 * [한국어] struct nvme_payload — 요청의 데이터 페이로드 기술
 *
 * CONTIG vs SGL 두 가지 모드를 하나의 구조체로 통일. reset_sgl_fn == NULL
 * 이면 CONTIG 모드, 아니면 SGL 모드로 해석된다(nvme_payload_type() 참고).
 * 메타데이터(PI 등)는 별도 md 포인터로 처리 — 현재 구현은 메타데이터를
 * 연속 버퍼로만 지원(비연속 메타는 미지원).
 */
struct nvme_payload {
	/**
	 * Functions for retrieving physical addresses for scattered payloads.
	 */
	spdk_nvme_req_reset_sgl_cb reset_sgl_fn;
                                  /* [한국어] SGL 모드의 "순회 재시작" 콜백 — 분할 I/O 시작 또는 재시도 시 호출되어 sg 포인터를 payload_offset 위치로 reset
                                   *  - NULL이면 CONTIG 모드
                                   *  - 시그니처: void (*)(void *cb_arg, uint32_t sgl_offset) */
	spdk_nvme_req_next_sge_cb next_sge_fn;
                                  /* [한국어] SGL 모드의 "다음 세그먼트 얻기" 콜백
                                   *  - 시그니처: int (*)(void *cb_arg, void **address, uint32_t *length)
                                   *  - 호출될 때마다 다음 세그먼트의 VA·길이를 반환 */

	/**
	 * Extended IO options passed by the user
	 */
	struct spdk_nvme_ns_cmd_ext_io_opts *opts;
                                  /* [한국어] 확장 I/O 옵션 (MMKEY, 메모리 도메인, 가속 시퀀스 등) — 이 필드가 있으면 확장 API 경로
                                   *  - NULL이면 표준 API 경로 */

	/**
	 * If reset_sgl_fn == NULL, this is a contig payload, and contig_or_cb_arg contains the
	 * virtual memory address of a single virtually contiguous buffer.
	 *
	 * If reset_sgl_fn != NULL, this is a SGL payload, and contig_or_cb_arg contains the
	 * cb_arg that will be passed to the SGL callback functions.
	 */
	void *contig_or_cb_arg;
                                  /* [한국어] 듀얼 용도 필드 — 모드에 따라 의미가 다름
                                   *  - CONTIG: 실제 데이터 버퍼의 가상 주소
                                   *  - SGL: reset/next 콜백에 전달될 사용자 cb_arg */

	/** Virtual memory address of a single virtually contiguous metadata buffer */
	void *md;
                                  /* [한국어] 메타데이터 연속 버퍼 가상 주소 (PI Guard/AppTag/RefTag 영역)
                                   *  - 메타 없는 명령/네임스페이스면 NULL
                                   *  - 현재 구현은 메타데이터에 대해 CONTIG만 지원 */
};

#define NVME_PAYLOAD_CONTIG(contig_, md_) \
	(struct nvme_payload) { \
		.reset_sgl_fn = NULL, \
		.next_sge_fn = NULL, \
		.contig_or_cb_arg = (contig_), \
		.md = (md_), \
	}
                                  /* [한국어] CONTIG 모드 payload 구조체 복합 리터럴로 초기화
                                   *  - reset_sgl_fn=NULL이 CONTIG 모드의 식별자
                                   *  - 사용 예: req->payload = NVME_PAYLOAD_CONTIG(buffer, meta); */

#define NVME_PAYLOAD_SGL(reset_sgl_fn_, next_sge_fn_, cb_arg_, md_) \
	(struct nvme_payload) { \
		.reset_sgl_fn = (reset_sgl_fn_), \
		.next_sge_fn = (next_sge_fn_), \
		.contig_or_cb_arg = (cb_arg_), \
		.md = (md_), \
	}
                                  /* [한국어] SGL 모드 payload 초기화 — 두 콜백과 cb_arg, 메타 버퍼 */

static inline enum nvme_payload_type
nvme_payload_type(const struct nvme_payload *payload) {
	return payload->reset_sgl_fn ? NVME_PAYLOAD_TYPE_SGL : NVME_PAYLOAD_TYPE_CONTIG;
                                  /* [한국어] 한 줄 판정: 콜백이 설정돼 있으면 SGL, 아니면 CONTIG */
}

struct nvme_error_cmd {
	bool				do_not_submit;
	uint64_t			timeout_tsc;
	uint32_t			err_count;
	uint8_t				opc;
	struct spdk_nvme_status		status;
	TAILQ_ENTRY(nvme_error_cmd)	link;
};

/*
 * [한국어] ★★★ I/O 경로 중간 객체 ★★★
 * struct nvme_request
 *
 * SPDK 유저스페이스 NVMe 드라이버에서 "하나의 NVMe 명령 요청"을 표현하는
 * 핵심 객체. bdev_io → nvme_request → SQE/tracker 의 중간 단계이며, 공개
 * API(`spdk_nvme_ns_cmd_read` 등)가 내부적으로 이 구조체를 할당·초기화·
 * 제출한다.
 *
 * 수명 주기:
 *   1) `nvme_allocate_request()` — qpair 로컬 mempool에서 획득
 *   2) `nvme_request_init()` — cb_fn/cb_arg/payload 등 기본 필드 세팅
 *   3) 호출부가 `cmd` 필드에 opcode/nsid/LBA/dptr 세팅
 *   4) `nvme_qpair_submit_request()` → 트랜스포트로 dispatch
 *   5) 완료 시 `nvme_request_free()` 또는 split일 때 parent 갱신 후 free
 *
 * I/O split 시나리오:
 *   - 하나의 논리적 read/write가 디바이스 MDTS를 초과하거나 PRP 경계 문제로
 *     여러 NVMe 명령으로 분할되어야 할 때, "parent" request 하나와 여러
 *     "child" request가 생성됨
 *   - 모든 child 완료 후 parent의 cb_fn 호출 (부분 실패 시 parent_status에 에러 집약)
 */
struct nvme_request {
	struct spdk_nvme_cmd		cmd;
                                  /* [한국어] 이 요청이 장치에 제출할 SQE 본체 (64B)
                                   *  - 호출부가 opc/nsid/LBA/dptr 필드를 채움
                                   *  - 트랜스포트가 최종 SQ[tail] 위치에 memcpy 또는 필드 복사 */

	uint8_t				retries;
                                  /* [한국어] 현재까지 재시도한 횟수 — 컨트롤러 CRT 힌트에 따라 제한 */

	uint8_t				timed_out : 1;
                                  /* [한국어] timeout 감지로 강제 실패 처리된 상태 */

	/**
	 * True if the request is in the queued_req list.
	 */
	uint8_t				queued : 1;
                                  /* [한국어] 현재 qpair의 대기 큐(queued_req)에 들어있는지
                                   *  - 1이면 아직 장치에 제출되지 않고 대기 중 (NO_MEM 또는 연결 안 됨 등) */
	uint8_t				reserved : 6;
                                  /* [한국어] 예약 비트 */

	/**
	 * Number of children requests still outstanding for this
	 *  request which was split into multiple child requests.
	 */
	uint16_t			num_children;
                                  /* [한국어] split된 parent가 가진 미완료 child 수. 0이 되면 parent 완료
                                   *  - 일반 (비split) req는 0 */

	/**
	 * Offset in bytes from the beginning of payload for this request.
	 * This is used for I/O commands that are split into multiple requests.
	 */
	uint32_t			payload_offset;
                                  /* [한국어] child일 때 parent payload 내 시작 오프셋 — SGL reset_sgl_fn에 전달 */
	uint32_t			md_offset;
                                  /* [한국어] 메타데이터 영역의 오프셋 (PI 등) */

	uint32_t			payload_size;
                                  /* [한국어] 이 요청이 다룰 데이터 바이트 수 (child는 parent의 일부) */

	/**
	 * Timeout ticks for error injection requests, can be extended in future
	 * to support per-request timeout feature.
	 */
	uint64_t			timeout_tsc;
                                  /* [한국어] 에러 주입용 타임아웃 틱. 현재는 error injection 경로 전용 */

	/**
	 * Data payload for this request's command.
	 */
	struct nvme_payload		payload;
                                  /* [한국어] 데이터 페이로드 기술 — CONTIG 버퍼 또는 SGL 콜백
                                   *  - 트랜스포트가 이 필드를 기반으로 PRP/SGL 디스크립터를 빌드 */

	spdk_nvme_cmd_cb		cb_fn;
                                  /* [한국어] 완료 콜백 — 장치 완료 시(정상/에러) 호출
                                   *  - 시그니처: void (*)(void *cb_arg, const struct spdk_nvme_cpl *cpl) */
	void				*cb_arg;
                                  /* [한국어] 콜백 컨텍스트 (호출자 설정) */
	STAILQ_ENTRY(nvme_request)	stailq;
                                  /* [한국어] 대기/freelist 리스트 연결용 링크 — qpair의 queued_req·free_req에 등록 */

	struct spdk_nvme_qpair		*qpair;
                                  /* [한국어] 이 요청이 제출될 qpair 포인터 (할당 시 결정) */

	/*
	 * The value of spdk_get_ticks() when the request was submitted to the hardware.
	 * Only set if ctrlr->timeout_enabled is true.
	 */
	uint64_t			submit_tick;
                                  /* [한국어] 제출 시점 타임스탬프 — timeout 감지용. timeout 비활성이면 0 */

	/**
	 * The active admin request can be moved to a per process pending
	 *  list based on the saved pid to tell which process it belongs
	 *  to. The cpl saves the original completion information which
	 *  is used in the completion callback.
	 * NOTE: these below two fields are only used for admin request.
	 */
	pid_t				pid;
                                  /* [한국어] 요청을 발행한 프로세스의 PID (multi-process 공유 컨트롤러 환경)
                                   *  - 완료 처리 시 다른 프로세스 소유 요청을 거르기 위한 필터 */
	struct spdk_nvme_cpl		cpl;
                                  /* [한국어] 완료 캐시 — admin 요청 완료 정보 복사본 (per-process pending 처리용) */

	uint32_t			md_size;
                                  /* [한국어] 메타데이터 바이트 수 — payload_size와 별도 (PI 1 블록당 8B 등) */

	/**
	 * The following members should not be reordered with members
	 *  above.  These members are only needed when splitting
	 *  requests which is done rarely, and the driver is careful
	 *  to not touch the following fields until a split operation is
	 *  needed, to avoid touching an extra cacheline.
	 */
                                  /* [한국어] 아래 필드들은 cold — split 경로에서만 사용.
                                   *  hot path가 추가 캐시라인 touch 안 하도록 뒤쪽에 배치 */

	/**
	 * Points to the outstanding child requests for a parent request.
	 *  Only valid if a request was split into multiple children
	 *  requests, and is not initialized for non-split requests.
	 */
	TAILQ_HEAD(, nvme_request)	children;
                                  /* [한국어] parent가 소유한 child 리스트 — split 시에만 유효 */

	/**
	 * Linked-list pointers for a child request in its parent's list.
	 */
	TAILQ_ENTRY(nvme_request)	child_tailq;
                                  /* [한국어] 나 자신이 child일 때 parent 리스트에 연결되기 위한 링크 */

	/**
	 * Points to a parent request if part of a split request,
	 *   NULL otherwise.
	 */
	struct nvme_request		*parent;
                                  /* [한국어] child일 때 parent 포인터, 아니면 NULL */

	/**
	 * Completion status for a parent request.  Initialized to all 0's
	 *  (SUCCESS) before child requests are submitted.  If a child
	 *  request completes with error, the error status is copied here,
	 *  to ensure that the parent request is also completed with error
	 *  status once all child requests are completed.
	 */
	struct spdk_nvme_cpl		parent_status;
                                  /* [한국어] parent에 집약되는 최악 상태 — child 중 첫 에러 발생 시 복사되어 최종 parent 완료 콜백에 전달 */

	/**
	 * The user_cb_fn and user_cb_arg fields are used for holding the original
	 * callback data when using nvme_allocate_request_user_copy.
	 */
	spdk_nvme_cmd_cb		user_cb_fn;
                                  /* [한국어] 원본 사용자 콜백 — nvme_allocate_request_user_copy 경로에서 중간 복사 콜백으로 교체하기 전의 원본 */
	void				*user_cb_arg;
                                  /* [한국어] 원본 사용자 cb_arg */
	void				*user_buffer;
                                  /* [한국어] 사용자 버퍼 — user_copy 경로에서 중간 DMA 버퍼와 구분해 저장 */

	/** Sequence of accel operations associated with this request */
	void				*accel_sequence;
                                  /* [한국어] 연관된 accel 시퀀스 (DMA 엔진 오프로드 등). 없으면 NULL */
};

/*
 * [한국어] struct nvme_completion_poll_status — 동기 대기 패턴 상태 블록
 *
 * 비동기 SPDK API를 "요청 → polled 대기 → 결과" 스타일로 감싸기 위한 보조 구조체.
 * 예: 컨트롤러 초기화 admin 명령을 실행 후 완료까지 spin + yield 하며 대기.
 * `done == true`가 되면 `cpl`에 결과가 들어있음.
 */
struct nvme_completion_poll_status {
	struct spdk_nvme_cpl	cpl;
                                  /* [한국어] 완료 시점 CQE 사본 — done=true가 되면 유효 */
	uint64_t		timeout_tsc;
                                  /* [한국어] 타임아웃 절대 틱. 0이면 무한 대기 */
	/**
	 * DMA buffer retained throughout the duration of the command.  It'll be released
	 * automatically if the command times out, otherwise the user is responsible for freeing it.
	 */
	void			*dma_data;
                                  /* [한국어] 명령 수명 동안 유지해야 하는 DMA 버퍼
                                   *  - timeout 시 SPDK가 자동 해제 (느린 장치가 나중에 DMA 써도 안전)
                                   *  - 정상 완료 시 호출자 책임 해제 */
	bool			done;
                                  /* [한국어] 완료 플래그 — 폴링 측이 이 값을 검사하여 대기 종료 판단 */
	/* This flag indicates that the request has been timed out and the memory
	   must be freed in a completion callback */
	bool			timed_out;
                                  /* [한국어] timeout 발생 후 나중에 CQE가 도착했을 때 메모리 해제를 완료 콜백에서 처리하도록 표시 */
};

/*
 * [한국어] struct nvme_async_event_request — AER(Async Event Request) 래퍼
 *
 * NVMe 컨트롤러에 사전 발행해두는 특수 admin 명령. 컨트롤러가 보고할
 * 비동기 이벤트(에러, 헬스, 네임스페이스 변경 등)가 있을 때 이 명령이
 * "완료"되어 이벤트 내용을 반환한다. 드라이버는 완료 후 즉시 새 AER을
 * 재발행해 항상 대기 상태 유지.
 */
struct nvme_async_event_request {
	struct spdk_nvme_ctrlr		*ctrlr;
                                  /* [한국어] 이 AER이 속한 컨트롤러 */
	struct nvme_request		*req;
                                  /* [한국어] 내부 nvme_request (발행된 admin 명령) */
	struct spdk_nvme_cpl		cpl;
                                  /* [한국어] 이벤트 완료 CQE 캐시 — 사용자 콜백에 전달 */
};

/*
 * [한국어] qpair 수명 상태머신
 *
 * 전형적 흐름:
 *   DISCONNECTED → CONNECTING → CONNECTED → ENABLING → ENABLED  (I/O 가능)
 *   ENABLED → DISCONNECTING → DISCONNECTED → DESTROYING → (free)
 *
 * PCIe에서는 CONNECT 단계가 매우 짧지만, NVMe-oF(RDMA/TCP)에서는 네트워크
 * handshake 및 Fabrics CONNECT 커맨드 완료까지 여러 폴링 주기 소요.
 */
enum nvme_qpair_state {
	NVME_QPAIR_DISCONNECTED,      /* [한국어] 초기 상태 / 완전 해제 — I/O 제출 불가 */
	NVME_QPAIR_DISCONNECTING,     /* [한국어] 해제 진행 중 — outstanding I/O 대기·정리 단계 */
	NVME_QPAIR_CONNECTING,        /* [한국어] 트랜스포트 연결 중 (NVMe-oF CONNECT 커맨드 진행 등) */
	NVME_QPAIR_CONNECTED,         /* [한국어] 트랜스포트 연결 완료, 아직 I/O 발행 전 */
	NVME_QPAIR_ENABLING,          /* [한국어] 큐 활성화 중 (SQ/CQ create 등) */
	NVME_QPAIR_ENABLED,           /* [한국어] I/O 수행 가능 — read/write 제출 허용 */
	NVME_QPAIR_DESTROYING,        /* [한국어] 파괴 진행 중 — 완료 콜백 drain 후 자원 해제 */
};

/*
 * [한국어] NVMe in-band 인증(NVMe 2.0 DH-HMAC-CHAP) 상태머신
 *
 * 연결 직후 challenge → reply → success 메시지 교환 수행.
 * 인증 실패 시 qpair는 사용 불가 상태로 전환됨.
 */
enum nvme_qpair_auth_state {
	NVME_QPAIR_AUTH_STATE_NEGOTIATE,         /* [한국어] 인증 파라미터 negotiation 초기화 */
	NVME_QPAIR_AUTH_STATE_AWAIT_NEGOTIATE,   /* [한국어] negotiate 응답 대기 */
	NVME_QPAIR_AUTH_STATE_AWAIT_CHALLENGE,   /* [한국어] 컨트롤러가 보낸 challenge 대기 */
	NVME_QPAIR_AUTH_STATE_AWAIT_REPLY,       /* [한국어] 호스트 reply 전송 후 응답 대기 */
	NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS1,    /* [한국어] success1 메시지 대기 */
	NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS2,    /* [한국어] 상호 인증(양방향) 시 success2 대기 */
	NVME_QPAIR_AUTH_STATE_AWAIT_FAILURE2,    /* [한국어] 실패 경로 완료 대기 */
	NVME_QPAIR_AUTH_STATE_DONE,              /* [한국어] 인증 완료 (성공/실패 확정) */
};

/* Maximum size of a digest */
#define NVME_AUTH_DIGEST_MAX_SIZE	64
                                  /* [한국어] 해시 다이제스트 최대 길이 (SHA-512까지 수용). challenge/response 버퍼 크기 산정에 사용 */

/*
 * [한국어] struct nvme_auth — qpair별 인증 상태 저장
 *
 * qpair 구조체에 임베딩되어, 각 qpair가 독립적으로 DH-HMAC-CHAP 핸드셰이크를 유지.
 */
struct nvme_auth {
	/* State of the authentication */
	enum nvme_qpair_auth_state	state;
                                  /* [한국어] 현재 인증 상태머신 단계 */
	/* Status of the authentication */
	int				status;
                                  /* [한국어] 누적 상태 코드 (0=OK, 음수=errno). 실패 시 최초 에러 보존 */
	/* Transaction ID */
	uint16_t			tid;
                                  /* [한국어] 인증 transaction ID — 요청/응답 매칭용 (NVMe AUTH PDU 헤더에 포함) */
	union {
		struct {
			/* Authentication transaction required (authreq.atr) */
			uint32_t        atr : 1;
                                  /* [한국어] 컨트롤러가 인증 트랜잭션을 요구했는지 (connect 응답 비트) */
			/* Authentication and secure channel required (authreq.ascr) */
			uint32_t        ascr : 1;
                                  /* [한국어] 인증 + 보안 채널 필수 여부 */
			/* In authenticate poll context flag */
			uint8_t		in_auth_poll : 1;
                                  /* [한국어] 현재 인증 폴러 내부에서 재진입 방지용 플래그 */
			uint32_t        reserved : 29;
		};
		uint32_t                raw;
                                  /* [한국어] 전체 raw 뷰 — 한 번에 리셋/저장 시 사용 */
	} flags;
	/* Selected hash function */
	uint8_t				hash;
                                  /* [한국어] 선택된 해시 함수 ID (NVMe DH-HMAC-CHAP 스펙의 목록) */
	/* Buffer used for controller challenge */
	uint8_t				challenge[NVME_AUTH_DIGEST_MAX_SIZE];
                                  /* [한국어] 컨트롤러가 보낸 challenge 저장 — reply 계산 시 입력 */
	/* User's auth cb fn/ctx */
	spdk_nvme_authenticate_cb	cb_fn;
                                  /* [한국어] 인증 완료 콜백 (spdk_nvme_qpair_authenticate로 전달된 값) */
	void				*cb_ctx;
                                  /* [한국어] 콜백 컨텍스트 */
};

/*
 * [한국어] ★★★ I/O 경로 중심 객체 ★★★
 * struct spdk_nvme_qpair - NVMe 큐 페어 (Submission Queue + Completion Queue)
 *
 * 공개 헤더(spdk/nvme.h)에서는 opaque로 노출되는 타입의 내부 정의.
 * 하나의 qpair가 하나의 SQ·CQ 쌍을 표현하며, 실제 메모리 큐(ring buffer)와
 * doorbell 제어는 트랜스포트 확장(nvme_pcie_qpair, nvme_rdma_qpair 등)에서
 * container_of로 상속받아 보관한다.
 *
 * 수명 원칙:
 *   - 하나의 qpair는 생성한 SPDK thread에 **고정**되어야 한다 (thread affinity).
 *   - 다른 스레드에서 제출/완료 호출은 데이터 경합을 유발한다.
 *   - 유일한 예외: spdk_nvme_qpair_delete는 어느 스레드든 호출 가능
 *     (내부적으로 소유 스레드로 메시지 전달).
 *
 * hot path 필드 배치 원칙: free_req/queued_req/poll_group_stailq 등 매
 * 제출·완료마다 접근되는 필드는 상단에, 초기화/해제 전용(tailq, auth 등)은
 * 하단에 배치해 한 캐시 라인에 hot data 집중.
 */
struct spdk_nvme_qpair {
	struct spdk_nvme_ctrlr			*ctrlr;
                                  /* [한국어] 이 qpair가 속한 컨트롤러 포인터
                                   *  - 설정자: spdk_nvme_ctrlr_alloc_io_qpair 또는 admin qpair 생성 시
                                   *  - 읽는 자: 요청 완료 시 timeout/retry 정책 조회, reset 동기화 */

	uint16_t				id;
                                  /* [한국어] Queue ID (QID)
                                   *  - 0 = admin qpair, 1~65535 = I/O qpair
                                   *  - NVMe 스펙상 SQE·CQE·create_io_sq/cq 커맨드에 그대로 사용 */

	uint8_t					qprio: 2;
                                  /* [한국어] Queue priority (enum spdk_nvme_qprio: URGENT/HIGH/MEDIUM/LOW)
                                   *  - 컨트롤러 AMS=WRR(Weighted Round Robin) 시에만 효과
                                   *  - 스펙 §6.5.1.2 create_io_sq 커맨드 cdw11 QPRIO 필드에 복사됨 */

	uint8_t					state: 3;
                                  /* [한국어] enum nvme_qpair_state — 현재 상태머신 단계
                                   *  - I/O 제출 전 항상 ENABLED인지 확인 */

	uint8_t					async: 1;
                                  /* [한국어] 비동기 모드 플래그 (인터럽트 기반 이벤트 드리븐 vs 폴링) */

	uint8_t					is_new_qpair: 1;
                                  /* [한국어] 막 생성되어 첫 connect를 아직 수행하지 않은 상태 */

	uint8_t					abort_dnr: 1;
                                  /* [한국어] abort 시 Do Not Retry 비트를 설정할지 여부 */
	/*
	 * Members for handling IO qpair deletion inside of a completion context.
	 * These are specifically defined as single bits, so that they do not
	 *  push this data structure out to another cacheline.
	 */
	uint8_t					in_completion_context: 1;
                                  /* [한국어] 현재 이 qpair의 완료 콜백 안에서 실행 중 — 재귀적 delete 방지용 */
	uint8_t					delete_after_completion_context: 1;
                                  /* [한국어] 완료 콜백 중 delete 요청이 들어왔음을 표시 — 콜백 종료 후 실제 파괴 */

	/*
	 * Set when no deletion notification is needed. For example, the process
	 * which allocated this qpair exited unexpectedly.
	 */
	uint8_t					no_deletion_notification_needed: 1;
                                  /* [한국어] 삭제 알림 콜백 불필요 표시
                                   *  - 소유 프로세스가 비정상 종료된 경우 등 → 다른 프로세스가 자원 정리만 수행 */

	uint8_t					last_fuse: 2;
                                  /* [한국어] 직전 제출한 커맨드의 fuse 필드 — fused 첫 조각은 doorbell 생략, 두 번째에서 함께 울림(pcie_internal 참고) */

	uint8_t					transport_failure_reason: 3;
                                  /* [한국어] 현재 트랜스포트 실패 이유 (enum spdk_nvme_qp_failure_reason) */
	uint8_t					last_transport_failure_reason: 3;
                                  /* [한국어] 마지막 실패 이유 저장 — 재연결 시 참조 */

	uint8_t					in_connect_poll : 1;
                                  /* [한국어] connect 폴러 재진입 방지 플래그 */

	/* Number of IO outstanding at transport level */
	uint16_t				queue_depth;
                                  /* [한국어] 트랜스포트 계층에서 장치로 실제 발행되어 완료 대기 중인 I/O 수
                                   *  - num_outstanding_reqs와 다름: queued_req(큐잉만 된)는 제외
                                   *  - 트랜스포트가 "얼마나 많이 in-flight인지" 추적하여 flow control에 사용 */

	enum spdk_nvme_transport_type		trtype;
                                  /* [한국어] 트랜스포트 타입 (PCIE/RDMA/TCP/VFIOUSER/…) */

	uint32_t				num_outstanding_reqs;
                                  /* [한국어] 이 qpair에 할당되어 아직 완료되지 않은 nvme_request 총수
                                   *  - free_req에 있는 것은 제외. queued_req + in-flight 합 */

	/* request object used only for this qpair's FABRICS/CONNECT command (if needed) */
	struct nvme_request			*reserved_req;
                                  /* [한국어] CONNECT/Fabrics 전용 예약 request — 연결 중에는 free_req가 비어 있을 수 있어 별도 보관 */

	STAILQ_HEAD(, nvme_request)		free_req;
                                  /* [한국어] 재사용 가능한 nvme_request 풀 (스레드 로컬, 락 없음)
                                   *  - nvme_allocate_request가 이 리스트에서 pop
                                   *  - 완료 시 다시 push (자유 공간 복귀) */
	STAILQ_HEAD(, nvme_request)		queued_req;
                                  /* [한국어] 장치에 아직 제출되지 않은 대기 요청 리스트
                                   *  - 트랜스포트가 연결 중이거나 in-flight 한도에 걸렸을 때 임시 저장
                                   *  - 상태 복구 시 drain */

	/* List entry for spdk_nvme_transport_poll_group::qpairs */
	STAILQ_ENTRY(spdk_nvme_qpair)		poll_group_stailq;
                                  /* [한국어] 폴링 그룹의 qpair 리스트 연결고리 */

	/** Commands opcode in this list will return error */
	TAILQ_HEAD(, nvme_error_cmd)		err_cmd_head;
                                  /* [한국어] 에러 주입 명단 — 여기 opcode로 매치되는 요청은 가상 에러 반환 */
	/** Requests in this list will return error */
	STAILQ_HEAD(, nvme_request)		err_req_head;
                                  /* [한국어] 특정 request 객체 단위 에러 주입 리스트 */

	struct spdk_nvme_ctrlr_process		*active_proc;
                                  /* [한국어] 현재 이 qpair를 "활성 소유"하는 프로세스 엔트리 (multi-process 공유 컨트롤러 지원) */

	struct spdk_nvme_transport_poll_group	*poll_group;
                                  /* [한국어] 이 qpair가 가입된 트랜스포트 폴링 그룹 (없으면 NULL)
                                   *  - 그룹 단위 process_completions를 지원 */

	void					*poll_group_tailq_head;
                                  /* [한국어] 폴링 그룹 내부의 tailq head 포인터 — 트랜스포트 구현별 세부 */

	const struct spdk_nvme_transport	*transport;
                                  /* [한국어] 트랜스포트 vtable — pcie/rdma/tcp/vfio_user 등 */

	/* Entries below here are not touched in the main I/O path. */
                                  /* [한국어] 아래 필드는 cold — 초기화/해제/예외 경로 전용. hot path와 캐시 라인 분리 */

	struct nvme_completion_poll_status	*fabric_poll_status;
                                  /* [한국어] Fabrics CONNECT 등 동기 대기 용도의 상태 블록 */

	/* List entry for spdk_nvme_ctrlr::active_io_qpairs */
	TAILQ_ENTRY(spdk_nvme_qpair)		tailq;
                                  /* [한국어] 컨트롤러의 I/O qpair 리스트에 연결되는 링크 */

	/* List entry for spdk_nvme_ctrlr_process::allocated_io_qpairs */
	TAILQ_ENTRY(spdk_nvme_qpair)		per_process_tailq;
                                  /* [한국어] 프로세스별 소유 qpair 리스트 링크 */

	STAILQ_HEAD(, nvme_request)		aborting_queued_req;
                                  /* [한국어] abort 진행 중인 요청 리스트 (사용자 abort 요청 대기) */

	void					*req_buf;
                                  /* [한국어] free_req 풀의 원본 할당 버퍼 — 해제 시 free 대상 */

	/* In-band authentication state */
	struct nvme_auth			auth;
                                  /* [한국어] DH-HMAC-CHAP 상태 (NVMe-oF 전용) — PCIe에서는 미사용 */
};

/*
 * [한국어] struct spdk_nvme_poll_group — 다중 트랜스포트 폴링 그룹
 *
 * 여러 qpair(다른 트랜스포트도 가능)을 묶어 단일 process_completions로
 * 모두 수집하는 추상화. 일반적으로 SPDK 애플리케이션은 reactor 하나당
 * poll_group 하나를 만들고 모든 qpair를 등록한다.
 */
struct spdk_nvme_poll_group {
	void						*ctx;
                                  /* [한국어] 사용자 정의 컨텍스트 (spdk_nvme_poll_group_create 인자) */
	struct spdk_nvme_accel_fn_table			accel_fn_table;
                                  /* [한국어] accel 오프로드 함수 테이블 (CRC/메모리 복사 가속 등) */
	STAILQ_HEAD(, spdk_nvme_transport_poll_group)	tgroups;
                                  /* [한국어] 트랜스포트별 sub-group 리스트 (PCIe·RDMA·TCP 각각) */
	bool						in_process_completions;
                                  /* [한국어] 현재 process_completions 내부 실행 중 재귀 방지 */
	bool						enable_interrupts;
                                  /* [한국어] 인터럽트 기반 이벤트 모드 활성 여부 */
	bool						enable_interrupts_is_valid;
                                  /* [한국어] enable_interrupts 값이 초기화됨을 표시 */
	int						disconnect_qpair_fd;
                                  /* [한국어] disconnected 이벤트 알림용 fd */
	struct spdk_fd_group				*fgrp;
                                  /* [한국어] epoll 기반 fd 그룹 — 인터럽트 모드의 이벤트 수집자 */
	struct {
		spdk_nvme_poll_group_interrupt_cb	cb_fn;
		void					*cb_ctx;
	} interrupt;
                                  /* [한국어] 인터럽트 도착 시 호출할 사용자 콜백 */
};

/*
 * [한국어] struct spdk_nvme_transport_poll_group — 트랜스포트별 폴링 서브그룹
 *
 * 하나의 poll_group 내에서 같은 트랜스포트의 qpair들을 묶음.
 * PCIe/RDMA/TCP 각각 별도 subgroup을 유지해 트랜스포트별 최적 process_completions 호출.
 */
struct spdk_nvme_transport_poll_group {
	struct spdk_nvme_poll_group			*group;
                                  /* [한국어] 상위 poll_group */
	const struct spdk_nvme_transport		*transport;
                                  /* [한국어] 이 서브그룹이 담당하는 트랜스포트 vtable */
	STAILQ_HEAD(, spdk_nvme_qpair)			connected_qpairs;
                                  /* [한국어] 현재 정상 연결된 qpair 리스트 (완료 폴링 대상) */
	STAILQ_HEAD(, spdk_nvme_qpair)			disconnected_qpairs;
                                  /* [한국어] 연결 끊긴 qpair — 사용자 콜백으로 통지 후 재연결 또는 제거 */
	STAILQ_ENTRY(spdk_nvme_transport_poll_group)	link;
                                  /* [한국어] poll_group->tgroups 링크 */
	uint32_t					num_connected_qpairs;
                                  /* [한국어] connected_qpairs 길이 캐시 */
};

/*
 * [한국어] struct spdk_nvme_ns - NVMe 네임스페이스 내부 표현
 *
 * NVMe 네임스페이스는 하나의 컨트롤러가 노출하는 "논리 저장 영역"이며,
 * 각 NS는 고유 NSID, 섹터 크기, 총 LBA 수, PI 설정 등을 가진다. SPDK는
 * 컨트롤러당 RB 트리로 활성 NS를 관리하며, spdk_nvme_ns_cmd_read/write 등
 * I/O API는 이 구조체 포인터를 인자로 받아 SQE의 nsid/블록 크기를 결정한다.
 *
 * hot path에서 자주 읽히는 필드: sector_size, extended_lba_size, md_size,
 * sectors_per_max_io (split 판정), id (SQE nsid).
 */
struct spdk_nvme_ns {
	struct spdk_nvme_ctrlr		*ctrlr;
                                  /* [한국어] 소속 컨트롤러 — NSID 해석, admin 커맨드 발행 대상 */
	uint32_t			sector_size;
                                  /* [한국어] 논리 블록 크기(바이트) — 일반적으로 512 또는 4096
                                   *  - Identify NS의 FLBAS가 가리키는 LBAF.LBADS로부터 계산 */

	/*
	 * Size of data transferred as part of each block,
	 * including metadata if FLBAS indicates the metadata is transferred
	 * as part of the data buffer at the end of each LBA.
	 */
	uint32_t			extended_lba_size;
                                  /* [한국어] 메타데이터가 LBA 말미에 인라인으로 붙는 경우(extended LBA)의 실제 블록 크기
                                   *  - extended: sector_size + md_size (예: 512 + 8 = 520)
                                   *  - 분리형(separate md pointer)에서는 sector_size와 동일 */

	uint32_t			md_size;
                                  /* [한국어] 메타데이터 크기(바이트/블록) — PI Guard(2) + AppTag(2) + RefTag(4) 등 */
	uint32_t			pi_type;
                                  /* [한국어] Protection Information 유형 (0=disable, 1~3=Type 1/2/3) */
	uint32_t			pi_format;
                                  /* [한국어] PI 포맷 (NVMe 2.0 16/32/64-bit Guard 선택) */
	uint32_t			sectors_per_max_io;
                                  /* [한국어] 단일 I/O로 제출 가능한 최대 섹터 수 (MDTS 기반)
                                   *  - 초과 시 SPDK가 request를 split — parent/child 관계 생성 */
	uint32_t			sectors_per_max_io_no_md;
                                  /* [한국어] 메타데이터 제외 단일 I/O 최대 섹터 수 (extended LBA가 아닌 경우 대비) */
	uint32_t			sectors_per_stripe;
                                  /* [한국어] 스트라이프 크기(섹터) — 일부 Intel 드라이브에서 최적 I/O 크기 힌트 */
	uint32_t			id;
                                  /* [한국어] NSID (1-based) — SQE의 nsid 필드에 그대로 사용 */
	uint16_t			flags;
                                  /* [한국어] NS 기능 플래그 (FLUSH 지원, WRITE_ZEROES 지원, DSM 지원 등 — enum spdk_nvme_ns_flags) */
	bool				active;
                                  /* [한국어] 현재 활성 상태인지 — 비활성 NS는 I/O 제출 대상에서 제외 */

	/* Command Set Identifier */
	enum spdk_nvme_csi		csi;
                                  /* [한국어] Command Set ID — NVM/ZNS/KV 중 어느 커맨드 셋을 사용하는 NS인지 */

	/* Namespace Identification Descriptor List (CNS = 03h) */
	uint8_t				id_desc_list[4096];
                                  /* [한국어] Identify CNS=03 응답 원본 — EUI64/NGUID/UUID/CSI 디스크립터 리스트
                                   *  - 사용자 쿼리(spdk_nvme_ns_get_id_desc_list 등)로 노출 */

	uint32_t			ana_group_id;
                                  /* [한국어] Asymmetric Namespace Access 그룹 ID (NVMe-oF multipath) */
	enum spdk_nvme_ana_state	ana_state;
                                  /* [한국어] ANA 상태 (optimized/non-optimized/inaccessible/persistent-loss/change) */

	/* Identify Namespace data. */
	struct spdk_nvme_ns_data	nsdata;
                                  /* [한국어] Identify NS 응답 전체 원본 데이터 (4KB) — 가장 광범위한 NS 메타데이터 */

	/* Zoned Namespace Command Set Specific Identify Namespace data. */
	struct spdk_nvme_zns_ns_data	*nsdata_zns;
                                  /* [한국어] ZNS 전용 identify 데이터 (ZNS NS에서만 할당, 아니면 NULL) */

	struct spdk_nvme_nvm_ns_data	*nsdata_nvm;
                                  /* [한국어] NVM 커맨드 셋 전용 identify 데이터 (NVMe 2.0 추가) */

	RB_ENTRY(spdk_nvme_ns)		node;
                                  /* [한국어] 컨트롤러의 활성 NS RB 트리 링크 (NSID 기준 인덱스) */
};

#define NVME_CTRLR_LOG_FMT "%s%s%s%s%s,cntlid:%u"
#define NVME_CTRLR_LOG_ARGS(ctrlr) \
  spdk_nvme_trtype_is_fabrics((ctrlr)->trid.trtype) ? (ctrlr)->opts.hostnqn : "", \
  spdk_nvme_trtype_is_fabrics((ctrlr)->trid.trtype) ? "," : "", \
  spdk_nvme_trtype_is_fabrics((ctrlr)->trid.trtype) ? (ctrlr)->trid.subnqn : "", \
  spdk_nvme_trtype_is_fabrics((ctrlr)->trid.trtype) ? "," : "", \
  (ctrlr)->trid.traddr, \
  (ctrlr)->cntlid

#define NVME_QPAIR_LOG_FMT "qid:%u,qpair:%p"
#define NVME_QPAIR_LOG_ARGS(qpair) \
  (qpair)->id, \
  (qpair)

#define NVME_CTRLR_LOG(type, ctrlr, format, ...) do { \
	if ((ctrlr)) { \
		SPDK_##type##LOG("["NVME_CTRLR_LOG_FMT"] " format, NVME_CTRLR_LOG_ARGS(ctrlr), ##__VA_ARGS__); \
	} else { \
		SPDK_##type##LOG("[null ctrlr] " format, ##__VA_ARGS__); \
	} \
} while (0)

#define NVME_CTRLR_LOG2(type, component, ctrlr, format, ...) do { \
	if ((ctrlr)) { \
		SPDK_##type##LOG(component, "["NVME_CTRLR_LOG_FMT"] " format, NVME_CTRLR_LOG_ARGS(ctrlr), ##__VA_ARGS__); \
	} else { \
		SPDK_##type##LOG(component, "[null ctrlr] " format, ##__VA_ARGS__); \
	} \
} while (0)

#define NVME_CTRLR_ERRLOG(ctrlr, format, ...) NVME_CTRLR_LOG(ERR, ctrlr, format, ##__VA_ARGS__)
#define NVME_CTRLR_WARNLOG(ctrlr, format, ...) NVME_CTRLR_LOG(WARN, ctrlr, format, ##__VA_ARGS__)
#define NVME_CTRLR_NOTICELOG(ctrlr, format, ...) NVME_CTRLR_LOG(NOTICE, ctrlr, format, ##__VA_ARGS__)
#define NVME_CTRLR_INFOLOG(ctrlr, format, ...) NVME_CTRLR_LOG2(INFO, nvme, ctrlr, format, ##__VA_ARGS__)

#define NVME_QPAIR_LOG(type, qpair, format, ...) do { \
	if (!(qpair)) { \
		SPDK_##type##LOG("[null qpair] " format, ##__VA_ARGS__); \
	} else if (!(qpair)->ctrlr) { \
		SPDK_##type##LOG("[null ctrlr,"NVME_QPAIR_LOG_FMT"] " format, NVME_QPAIR_LOG_ARGS(qpair), ##__VA_ARGS__); \
	} else { \
		SPDK_##type##LOG("["NVME_CTRLR_LOG_FMT","NVME_QPAIR_LOG_FMT",%s] " format, NVME_CTRLR_LOG_ARGS((qpair)->ctrlr), NVME_QPAIR_LOG_ARGS(qpair), nvme_qpair_state_string((qpair)->state), ##__VA_ARGS__); \
	} \
} while (0)

#define NVME_QPAIR_LOG2(type, component, qpair, format, ...) do { \
	if (!(qpair)) { \
		SPDK_##type##LOG(component, "[null qpair] " format, ##__VA_ARGS__); \
	} else if (!(qpair)->ctrlr) { \
		SPDK_##type##LOG(component, "[null ctrlr,"NVME_QPAIR_LOG_FMT"] " format, NVME_QPAIR_LOG_ARGS(qpair), ##__VA_ARGS__); \
	} else { \
		SPDK_##type##LOG(component, "["NVME_CTRLR_LOG_FMT","NVME_QPAIR_LOG_FMT"] " format, NVME_CTRLR_LOG_ARGS((qpair)->ctrlr), NVME_QPAIR_LOG_ARGS(qpair), ##__VA_ARGS__); \
	} \
} while (0)

#define NVME_QPAIR_ERRLOG(qpair, format, ...) NVME_QPAIR_LOG(ERR, qpair, format, ##__VA_ARGS__)
#define NVME_QPAIR_WARNLOG(qpair, format, ...) NVME_QPAIR_LOG(WARN, qpair, format, ##__VA_ARGS__)
#define NVME_QPAIR_NOTICELOG(qpair, format, ...) NVME_QPAIR_LOG(NOTICE, qpair, format, ##__VA_ARGS__)
#define NVME_QPAIR_INFOLOG(qpair, format, ...) NVME_QPAIR_LOG2(INFO, nvme, qpair, format, ##__VA_ARGS__)

#ifdef DEBUG
#define NVME_CTRLR_DEBUGLOG(ctrlr, format, ...) NVME_CTRLR_LOG2(DEBUG, nvme, ctrlr, format, ##__VA_ARGS__)
#define NVME_QPAIR_DEBUGLOG(qpair, format, ...) NVME_QPAIR_LOG2(DEBUG, nvme, qpair, format, ##__VA_ARGS__)
#else
#define NVME_CTRLR_DEBUGLOG(...) do { } while (0)
#define NVME_QPAIR_DEBUGLOG(...) do { } while (0)
#endif

/**
 * State of struct spdk_nvme_ctrlr (in particular, during initialization).
 */
/*
 * [한국어] ★ NVMe 컨트롤러 초기화/reset 상태머신 ★
 *
 * spdk_nvme_ctrlr.state 필드가 이 enum 값을 가지며, spdk_nvme_ctrlr_process_init()이
 * 매 폴링마다 한 단계씩 진행한다. 초기화는 **동기 admin 커맨드 시퀀스**가 아니라
 * 상태머신 방식(한 admin 커맨드 제출 → 완료 대기 → 다음 단계)으로 설계되어,
 * 여러 컨트롤러를 병렬로 초기화할 수 있다.
 *
 * 일반 흐름 (성공 경로):
 *   INIT_DELAY → CONNECT_ADMINQ → WAIT_FOR_CONNECT_ADMINQ →
 *   READ_VS → WAIT → READ_CAP → WAIT → CHECK_EN → WAIT →
 *   (이미 EN이면) DISABLE_WAIT_READY_1 → SET_EN_0 → DISABLE_WAIT_READY_0 → DISABLED
 *   → ENABLE → ENABLE_WAIT_READY_1 → RESET_ADMIN_QUEUE →
 *   IDENTIFY → CONFIGURE_AER → SET_KEEP_ALIVE_TIMEOUT →
 *   IDENTIFY_IOCS_SPECIFIC → GET_ZNS_CMD_EFFECTS_LOG →
 *   SET_NUM_QUEUES → ...(identify NS, discovery log 등)... →
 *   READY
 *
 * 각 "XXX" 단계와 "XXX_WAIT_FOR_YYY" 단계가 쌍을 이룸:
 *   - XXX: admin 커맨드 발행 (비동기)
 *   - XXX_WAIT_FOR_YYY: 완료 폴링 (다음 process_init 호출에서 완료 확인 → 다음 상태 진입)
 *
 * 실패 시 NVME_CTRLR_STATE_ERROR로 전이 → is_failed=true → destroy만 가능.
 */
enum nvme_ctrlr_state {
	/**
	 * Wait before initializing the controller.
	 */
	NVME_CTRLR_STATE_INIT_DELAY,  /* [한국어] 초기화 시작 전 quirk delay (NVME_QUIRK_DELAY_BEFORE_CHK_RDY 등) */

	/**
	 * Connect the admin queue.
	 */
	NVME_CTRLR_STATE_CONNECT_ADMINQ,
                                  /* [한국어] admin qpair 연결 단계
                                   *  - PCIe: 즉시 성공
                                   *  - NVMe-oF: Fabrics CONNECT 커맨드 발행 후 응답 대기 */

	/**
	 * Controller has not started initialized yet.
	 */
	NVME_CTRLR_STATE_INIT = NVME_CTRLR_STATE_CONNECT_ADMINQ,
                                  /* [한국어] 초기화 시작 표시 — CONNECT_ADMINQ의 별칭 */

	/**
	 * Waiting for admin queue to connect.
	 */
	NVME_CTRLR_STATE_WAIT_FOR_CONNECT_ADMINQ,
                                  /* [한국어] admin 연결 완료 폴링 */

	/**
	 * Read Version (VS) register.
	 */
	NVME_CTRLR_STATE_READ_VS,
                                  /* [한국어] VS(Version) 레지스터 read 명령 발행 */

	/**
	 * Waiting for Version (VS) register to be read.
	 */
	NVME_CTRLR_STATE_READ_VS_WAIT_FOR_VS,
                                  /* [한국어] VS 완료 대기. 완료 후 ctrlr->vs에 저장 */

	/**
	 * Read Capabilities (CAP) register.
	 */
	NVME_CTRLR_STATE_READ_CAP,
                                  /* [한국어] CAP(Capabilities) read — MQES, DSTRD, CSS, MPSMIN/MAX 확보 */

	/**
	 * Waiting for Capabilities (CAP) register to be read.
	 */
	NVME_CTRLR_STATE_READ_CAP_WAIT_FOR_CAP,
                                  /* [한국어] CAP 완료 대기 */

	/**
	 * Check EN to prepare for controller initialization.
	 */
	NVME_CTRLR_STATE_CHECK_EN,
                                  /* [한국어] CC.EN 비트 확인 — 이미 1이면 disable 경로, 0이면 바로 enable 경로 */

	/**
	 * Waiting for CC to be read as part of EN check.
	 */
	NVME_CTRLR_STATE_CHECK_EN_WAIT_FOR_CC,
                                  /* [한국어] CC read 완료 대기 */

	/**
	 * Waiting for CSTS.RDY to transition from 0 to 1 so that CC.EN may be set to 0.
	 */
	NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1,
                                  /* [한국어] RDY=1 대기 — EN=1로 부팅된 장치에서 EN=0 쓰기 전에 먼저 RDY=1 확인 필요 (스펙) */

	/**
	 * Waiting for CSTS register to be read as part of waiting for CSTS.RDY = 1.
	 */
	NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS,
                                  /* [한국어] CSTS 폴링 read 대기 */

	/**
	 * Disabling the controller by setting CC.EN to 0.
	 */
	NVME_CTRLR_STATE_SET_EN_0,
                                  /* [한국어] CC.EN=0 쓰기 발행 */

	/**
	 * Waiting for the CC register to be read as part of disabling the controller.
	 */
	NVME_CTRLR_STATE_SET_EN_0_WAIT_FOR_CC,
                                  /* [한국어] CC write 완료 대기 */

	/**
	 * Waiting for CSTS.RDY to transition from 1 to 0 so that CC.EN may be set to 1.
	 */
	NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0,
                                  /* [한국어] RDY=0 전이 대기 (EN=0 이후 장치가 disable 완료 표시) */

	/**
	 * Waiting for CSTS register to be read as part of waiting for CSTS.RDY = 0.
	 */
	NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0_WAIT_FOR_CSTS,
                                  /* [한국어] CSTS read 대기 */

	/**
	 * The controller is disabled. (CC.EN and CSTS.RDY are 0.)
	 */
	NVME_CTRLR_STATE_DISABLED,
                                  /* [한국어] EN=0, RDY=0 확정 상태 — 이제 안전하게 admin queue 주소·크기 프로그래밍 가능 */

	/**
	 * Enable the controller by writing CC.EN to 1
	 */
	NVME_CTRLR_STATE_ENABLE,
                                  /* [한국어] CC.EN=1 쓰기 (MPS/CSS/IOSQES/IOCQES 등 함께 설정) */

	/**
	 * Waiting for CC register to be written as part of enabling the controller.
	 */
	NVME_CTRLR_STATE_ENABLE_WAIT_FOR_CC,
                                  /* [한국어] CC 쓰기 완료 대기 */

	/**
	 * Waiting for CSTS.RDY to transition from 0 to 1 after enabling the controller.
	 */
	NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1,
                                  /* [한국어] RDY=1 전이 대기 → 이 시점부터 admin 커맨드 발행 가능 */

	/**
	 * Waiting for CSTS register to be read as part of waiting for CSTS.RDY = 1.
	 */
	NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS,
                                  /* [한국어] CSTS read 대기 */

	/**
	 * Reset the Admin queue of the controller.
	 */
	NVME_CTRLR_STATE_RESET_ADMIN_QUEUE,
                                  /* [한국어] admin 큐 재설정 — 이후 identify 등 admin 명령 발행 시작점 */

	/**
	 * Identify Controller command will be sent to then controller.
	 */
	NVME_CTRLR_STATE_IDENTIFY,    /* [한국어] Identify Controller (CNS=01) 발행 — 벤더/펌웨어/MDTS/지원 기능 조회 */
	NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY,
                                  /* [한국어] Identify 응답 대기. 성공 시 ctrlr->cdata 채워짐 + quirks 매칭 */

	/**
	 * Configure AER of the controller.
	 */
	NVME_CTRLR_STATE_CONFIGURE_AER,
                                  /* [한국어] Async Event 구성 — 어떤 event type을 AER로 보고받을지 설정 */
	NVME_CTRLR_STATE_WAIT_FOR_CONFIGURE_AER,
                                  /* [한국어] Configure AER 완료 대기 */

	/**
	 * Set Keep Alive Timeout of the controller.
	 */
	NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT,
                                  /* [한국어] Keep Alive Timeout 설정 (NVMe-oF에서 connection liveness 유지) */
	NVME_CTRLR_STATE_WAIT_FOR_KEEP_ALIVE_TIMEOUT,
                                  /* [한국어] Keep Alive Timeout 완료 대기 */

	/**
	 * Get Identify I/O Command Set Specific Controller data structure.
	 */
	NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC,
                                  /* [한국어] I/O Command Set Specific Identify Controller (ZNS/KV 등) */
	NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_IOCS_SPECIFIC,
                                  /* [한국어] IOCS Specific 완료 대기 */

	/**
	 * Get Commands Supported and Effects log page for the Zoned Namespace Command Set.
	 */
	NVME_CTRLR_STATE_GET_ZNS_CMD_EFFECTS_LOG,
                                  /* [한국어] ZNS 지원 컨트롤러에서 command effects log page 조회 */
	NVME_CTRLR_STATE_WAIT_FOR_GET_ZNS_CMD_EFFECTS_LOG,
                                  /* [한국어] 완료 대기 */

	/**
	 * Set Number of Queues of the controller.
	 */
	NVME_CTRLR_STATE_SET_NUM_QUEUES,
                                  /* [한국어] Set Features(NUMBER_OF_QUEUES) 발행 — 호스트가 원하는 I/O 큐 수 요청 */
	NVME_CTRLR_STATE_WAIT_FOR_SET_NUM_QUEUES,
                                  /* [한국어] 응답에서 장치가 실제 할당한 큐 수 확인 (요청보다 적을 수 있음) */

	/**
	 * Get active Namespace list of the controller.
	 */
	NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS,
                                  /* [한국어] Identify (CNS=02) — 활성 NSID 리스트 조회 */
	NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ACTIVE_NS,
                                  /* [한국어] NS 리스트 대기. 여러 NS가 있으면 페이지별 반복 */

	/**
	 * Get Identify Namespace Data structure for each NS.
	 */
	NVME_CTRLR_STATE_IDENTIFY_NS,
                                  /* [한국어] 각 NS에 대해 Identify Namespace (CNS=00) 발행 — blocklen/PI/MDTS 등 채움 */
	NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS,
                                  /* [한국어] 각 NS 응답 대기 */

	/**
	 * Get Identify Namespace Identification Descriptors.
	 */
	NVME_CTRLR_STATE_IDENTIFY_ID_DESCS,
                                  /* [한국어] Identify (CNS=03) — NSID별 UUID/NGUID/EUI64/CSI 디스크립터 리스트 */

	/**
	 * Get Identify I/O Command Set Specific Namespace data structure for each NS.
	 */
	NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC,
                                  /* [한국어] ZNS/KV 등 NS별 특화 identify 데이터 조회 */
	NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS_IOCS_SPECIFIC,
                                  /* [한국어] 완료 대기 */

	/**
	 * Waiting for the Identify Namespace Identification
	 * Descriptors to be completed.
	 */
	NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ID_DESCS,
                                  /* [한국어] ID 디스크립터 완료 대기 */

	/**
	 * Set supported log pages of the controller.
	 */
	NVME_CTRLR_STATE_SET_SUPPORTED_LOG_PAGES,
                                  /* [한국어] Identify에서 얻은 LPA 비트맵으로 log_page_supported[] 초기화 (admin 커맨드 없음) */

	/**
	 * Set supported log pages of INTEL controller.
	 */
	NVME_CTRLR_STATE_SET_SUPPORTED_INTEL_LOG_PAGES,
                                  /* [한국어] Intel 벤더 전용 log page 감지 */
	NVME_CTRLR_STATE_WAIT_FOR_SUPPORTED_INTEL_LOG_PAGES,
                                  /* [한국어] Intel log page probe 완료 대기 */

	/**
	 * Set supported features of the controller.
	 */
	NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES,
                                  /* [한국어] feature_supported[] 배열 초기화 (identify 기반) */

	/**
	 * Set the Host Behavior Support feature of the controller.
	 */
	NVME_CTRLR_STATE_SET_HOST_FEATURE,
                                  /* [한국어] Set Features(HOST_BEHAVIOR_SUPPORT) — 호스트가 지원하는 확장 알림 */
	NVME_CTRLR_STATE_WAIT_FOR_SET_HOST_FEATURE,
                                  /* [한국어] 완료 대기 */

	/**
	 * Set Doorbell Buffer Config of the controller.
	 */
	NVME_CTRLR_STATE_SET_DB_BUF_CFG,
                                  /* [한국어] Doorbell Buffer Config admin 커맨드 — shadow doorbell + eventidx 메모리 위치 등록 (NVMe 1.3+) */
	NVME_CTRLR_STATE_WAIT_FOR_DB_BUF_CFG,
                                  /* [한국어] 완료 대기. 성공 시 flags.has_shadow_doorbell=1 */

	/**
	 * Set Host ID of the controller.
	 */
	NVME_CTRLR_STATE_SET_HOST_ID,
                                  /* [한국어] Set Features(HOST_ID) — 멀티호스트 공유 컨트롤러 식별용 */
	NVME_CTRLR_STATE_WAIT_FOR_HOST_ID,
                                  /* [한국어] 완료 대기 */

	/**
	 * Let transport layer do its part of initialization.
	 */
	NVME_CTRLR_STATE_TRANSPORT_READY,
                                  /* [한국어] 트랜스포트 계층의 추가 초기화 (NVMe-oF 인증, discovery 등) */

	/**
	 * Controller initialization has completed and the controller is ready.
	 */
	NVME_CTRLR_STATE_READY,
                                  /* [한국어] ★ 초기화 완료 ★ — 사용자 API(I/O qpair 할당, read/write) 사용 가능 */

	/**
	 * Controller initialization has an error.
	 */
	NVME_CTRLR_STATE_ERROR,
                                  /* [한국어] 초기화 실패. is_failed=true. destroy만 가능 */

	/**
	 * Admin qpair was disconnected, controller needs to be re-initialized
	 */
	NVME_CTRLR_STATE_DISCONNECTED,
                                  /* [한국어] admin qpair 연결 끊김 — 재연결 후 재초기화 필요 */
};

#define NVME_TIMEOUT_INFINITE		0
                                  /* [한국어] 무한 타임아웃 표시 — 요청에 타임아웃 적용 안 함 */
#define NVME_TIMEOUT_KEEP_EXISTING	UINT64_MAX
                                  /* [한국어] 기존 타임아웃 값 유지 표시 — opts 업데이트 시 timeout만 건들지 않도록 */

/*
 * [한국어] struct spdk_nvme_ctrlr_aer_completion — AER 완료 캐시 엔트리
 *
 * 컨트롤러가 비동기 이벤트를 보고하면 AER 명령이 완료되어 이 엔트리에
 * 정보가 담긴다. Multi-process 환경에서 "첫 수신 프로세스"가 이벤트를
 * 받아 다른 프로세스들의 async_events 큐에 복사해주는 용도로도 사용.
 */
struct spdk_nvme_ctrlr_aer_completion {
	struct spdk_nvme_ctrlr	*ctrlr;
                                  /* [한국어] 이벤트를 보고한 컨트롤러 */
	struct spdk_nvme_cpl	cpl;
                                  /* [한국어] 이벤트 CQE (type/log-page ID 인코딩됨) */

	union {
		/* Contains payload of Changed Attached Namespace List log page. */
		uint32_t		*changed_ns_list;
                                  /* [한국어] ANA/네임스페이스 변경 이벤트 수신 시 받아온 log page 페이로드
                                   *  - 사용자 콜백 호출 후 SPDK가 free */
	} log_page;

	STAILQ_ENTRY(spdk_nvme_ctrlr_aer_completion) link;
                                  /* [한국어] ctrlr_process->async_events 리스트 링크 */
};

/*
 * Used to track properties for all processes accessing the controller.
 */
/*
 * [한국어] struct spdk_nvme_ctrlr_process — 컨트롤러를 공유하는 프로세스별 상태
 *
 * SPDK는 primary-secondary multi-process 공유 컨트롤러를 지원한다(DPDK
 * primary/secondary 모델과 유사). 같은 NVMe 디바이스에 여러 프로세스가
 * attach하면 각자 이 구조체를 하나씩 가지며, ctrlr->active_procs 리스트에
 * 등록된다. I/O qpair/AER 콜백/timeout 콜백은 프로세스별로 분리되어
 * 각자 자기 것만 통지받는다.
 */
struct spdk_nvme_ctrlr_process {
	/** Whether it is the primary process  */
	bool						is_primary;
                                  /* [한국어] 이 프로세스가 주(primary) — 컨트롤러 초기화·reset 책임
                                   *  - secondary는 이미 초기화된 컨트롤러에 attach만 함 */

	/** Process ID */
	pid_t						pid;
                                  /* [한국어] 이 엔트리 소유 프로세스 PID — getpid() 결과 */

	/** Active admin requests to be completed */
	STAILQ_HEAD(, nvme_request)			active_reqs;
                                  /* [한국어] 이 프로세스가 발행한 admin 요청 중 완료 대기 리스트
                                   *  - admin 큐는 공유 — 완료 시 pid로 소속 프로세스 필터링 */

	TAILQ_ENTRY(spdk_nvme_ctrlr_process)		tailq;
                                  /* [한국어] ctrlr->active_procs 리스트 링크 */

	/** Per process PCI device handle */
	struct spdk_pci_device				*devhandle;
                                  /* [한국어] 프로세스별 PCI 핸들 (DPDK 모델: primary가 매핑한 BAR를 secondary가 상속) */

	/** Reference to track the number of attachment to this controller. */
	int						ref;
                                  /* [한국어] 참조 카운트 — 같은 프로세스가 여러 번 attach 시 증가 */

	/** Allocated IO qpairs */
	TAILQ_HEAD(, spdk_nvme_qpair)			allocated_io_qpairs;
                                  /* [한국어] 이 프로세스가 할당한 I/O qpair 리스트 — 프로세스 종료 시 정리 책임 */

	spdk_nvme_aer_cb				aer_cb_fn;
                                  /* [한국어] AER 사용자 콜백 (async 이벤트 수신 시 호출) */
	void						*aer_cb_arg;
                                  /* [한국어] AER 콜백 컨텍스트 */

	/**
	 * A function pointer to timeout callback function
	 */
	spdk_nvme_timeout_cb		timeout_cb_fn;
                                  /* [한국어] 요청 timeout 감지 시 호출할 사용자 콜백 */
	void				*timeout_cb_arg;
                                  /* [한국어] timeout 콜백 컨텍스트 */
	/** separate timeout values for io vs. admin reqs */
	uint64_t			timeout_io_ticks;
                                  /* [한국어] I/O 요청 timeout (틱 단위, 0이면 무한) */
	uint64_t			timeout_admin_ticks;
                                  /* [한국어] admin 요청 timeout (별도 설정) */

	/** List to publish AENs to all procs in multiprocess setup */
	STAILQ_HEAD(, spdk_nvme_ctrlr_aer_completion)      async_events;
                                  /* [한국어] 이 프로세스에 전달될 AEN(Async Event Notification) 큐 */
};

/*
 * [한국어] struct nvme_register_completion — PCIe 레지스터 read/write 완료 엔트리
 *
 * MMIO는 동기 접근이지만, 일부 트랜스포트(NVMe-oF)에서는 property get/set이
 * 비동기이므로 완료 큐에 담아 폴링한다.
 */
struct nvme_register_completion {
	struct spdk_nvme_cpl			cpl;
                                  /* [한국어] 완료 CQE (property fabrics 명령의 결과) */
	uint64_t				value;
                                  /* [한국어] read된 레지스터 값 (write의 경우 의미 없음) */
	spdk_nvme_reg_cb			cb_fn;
                                  /* [한국어] 사용자 콜백 */
	void					*cb_ctx;
                                  /* [한국어] 콜백 컨텍스트 */
	STAILQ_ENTRY(nvme_register_completion)	stailq;
                                  /* [한국어] ctrlr->register_operations 링크 */
	pid_t					pid;
                                  /* [한국어] 요청 발행 프로세스 */
};

/*
 * [한국어] ★★★ struct spdk_nvme_ctrlr - NVMe 컨트롤러 전역 상태 ★★★
 *
 * NVMe 컨트롤러(하나의 NVMe 디바이스)를 표현하는 최상위 객체. 공개 헤더의
 * opaque 타입을 내부에서 완전한 필드로 채운다. 주요 책임:
 *   - 컨트롤러 초기화 상태머신(state, state_timeout_tsc) 구동
 *   - admin qpair 소유·관리 (adminq)
 *   - 활성 namespace RB 트리 유지 (ns 트리)
 *   - I/O qpair 풀 관리 (free_io_qids, active_io_qpairs)
 *   - AER 발행·수신 (aer[], active_procs의 async_events)
 *   - Multi-process 공유 (active_procs + ctrlr_lock)
 *   - 벤더 quirk, timeout, keep-alive, shadow doorbell 등 모든 상위 설정
 *
 * 필드 배치:
 *   - 상단: hot data (I/O 경로에서 직접 접근되는 플래그, RB ns 트리 등)
 *   - 하단: cold data (초기화·reset·identify·FW 다운로드·ANA 등)
 *   · 배치 목적: hot path가 cold data 캐시라인 터치 방지
 */
struct spdk_nvme_ctrlr {
	/* Hot data (accessed in I/O path) starts here. */

	/* Tree of namespaces */
	RB_HEAD(nvme_ns_tree, spdk_nvme_ns)	ns;
                                  /* [한국어] 활성 네임스페이스 RB 트리 — NSID 기준 O(log n) 조회
                                   *  - 설정자: identify active NS 완료 시 엔트리 삽입
                                   *  - 읽는 자: spdk_nvme_ctrlr_get_ns, ns_cmd_read/write 호출자 */

	/* The number of active namespaces */
	uint32_t			active_ns_count;
                                  /* [한국어] ns 트리의 크기 캐시 (순회 없이 즉답) */

	bool				is_removed;
                                  /* [한국어] 하드웨어가 제거된 상태 (hot-remove 감지 시 set) */

	bool				is_resetting;
                                  /* [한국어] 현재 reset 진행 중 — 신규 I/O 제출 차단 */

	bool				is_failed;
                                  /* [한국어] 복구 불가 실패 상태 — destroy만 가능 */

	bool				is_destructed;
                                  /* [한국어] destruct() 호출 완료 — 해제 대기 */

	bool				timeout_enabled;
                                  /* [한국어] 요청 timeout 감지 기능 활성 (opts.keep_alive_timeout_ms 등) — submit_tick 갱신 여부를 제어 */

	/* The application is preparing to reset the controller.  Transports
	 * can use this to skip unnecessary parts of the qpair deletion process
	 * for example, like the DELETE_SQ/CQ commands.
	 */
	bool				prepare_for_reset;
                                  /* [한국어] reset 준비 단계 — qpair 제거 시 DELETE_SQ/CQ admin 명령 생략 가능 힌트
                                   *  - 어차피 reset으로 큐 전체가 리셋되므로 시간 절약 */

	bool				is_disconnecting;
                                  /* [한국어] disconnect 진행 중 (NVMe-oF 트랜스포트) */

	bool				needs_io_msg_update;
                                  /* [한국어] 외부 I/O message 경로 업데이트 필요 표시 */

	uint16_t			max_sges;
                                  /* [한국어] 한 요청이 사용할 수 있는 최대 SGE 수 — SGL split 판정에 사용 */

	uint16_t			cntlid;
                                  /* [한국어] Controller ID (NVMe 스펙 — NVMe-oF에서 subsystem 내 컨트롤러 식별) */

	/** Controller support flags */
	uint64_t			flags;
                                  /* [한국어] 지원 기능 플래그 (enum spdk_nvme_ctrlr_flags — COMPARE_AND_WRITE, DIRECTIVES, SGL 등) */

	/** NVMEoF in-capsule data size in bytes */
	uint32_t			ioccsz_bytes;
                                  /* [한국어] NVMe-oF Fabrics: I/O queue command capsule size — 1회 요청에 인라인 가능한 데이터 크기 */

	/** NVMEoF in-capsule data offset in 16 byte units */
	uint16_t			icdoff;
                                  /* [한국어] In-capsule data offset (16B 단위) */

	/* Cold data (not accessed in normal I/O path) is after this point. */
                                  /* [한국어] ===== 여기부터 cold 필드 — 초기화/reset/예외 경로 전용 ===== */

	struct spdk_nvme_transport_id	trid;
                                  /* [한국어] 트랜스포트 식별자 (PCIe BDF 또는 NVMe-oF trid: 타입/주소/서비스/NQN)
                                   *  - spdk_nvme_probe 시 사용자 입력, 이후 이 컨트롤러의 "주소" */

	struct {
		/** Is numa.id valid? Ensures numa.id == 0 is interpreted correctly. */
		uint32_t		id_valid : 1;
                                  /* [한국어] numa.id 필드가 유효하게 초기화됐는지 — 0이 유효 NUMA 노드일 수 있어 별도 비트로 표시 */
		int32_t			id : 31;
                                  /* [한국어] 이 컨트롤러가 속한 NUMA 노드 (리눅스 /sys/bus/pci/devices/*/numa_node 등으로 판정) */
	} numa;

	union spdk_nvme_cap_register	cap;
                                  /* [한국어] CAP 레지스터 캐시 — MQES, DSTRD, CSS, MPSMIN/MAX 등 핵심 능력 */
	union spdk_nvme_vs_register	vs;
                                  /* [한국어] VS(Version) 레지스터 캐시 — 스펙 버전 판정 */

	int				state;
                                  /* [한국어] 초기화/reset 상태머신 상태 (enum nvme_ctrlr_state_* — 이 enum이 본 파일 line 1070 주변에 정의됨)
                                   *  - probe 이후 단계를 한 번에 한 admin 커맨드씩 진행 */
	uint64_t			state_timeout_tsc;
                                  /* [한국어] 현재 상태에서의 timeout 만료 시점 — 초과 시 실패 전이 */

	uint64_t			next_keep_alive_tick;
                                  /* [한국어] 다음 Keep Alive 명령을 발행할 시각 */
	uint64_t			keep_alive_interval_ticks;
                                  /* [한국어] Keep Alive 간격 (NVMe-oF에서 connection liveness 유지) */

	TAILQ_ENTRY(spdk_nvme_ctrlr)	tailq;
                                  /* [한국어] 전역 드라이버의 shared_attached_ctrlrs 리스트 링크 */

	/** All the log pages supported */
	bool				log_page_supported[256];
                                  /* [한국어] 각 log page ID(0~255)가 지원되는지 — Identify에서 얻은 CMIC/LPA 기반 */

	/** All the features supported */
	bool				feature_supported[256];
                                  /* [한국어] 각 Feature ID 지원 여부 */

	/** maximum i/o size in bytes */
	uint32_t			max_xfer_size;
                                  /* [한국어] 단일 I/O 전송 가능한 최대 바이트 — MDTS·MPS로부터 계산
                                   *  - 이 값을 초과하는 사용자 요청은 SPDK가 split */

	/** minimum page size supported by this controller in bytes */
	uint32_t			min_page_size;
                                  /* [한국어] 컨트롤러가 지원하는 최소 MPS (CAP.MPSMIN) */

	/** selected memory page size for this controller in bytes */
	uint32_t			page_size;
                                  /* [한국어] 호스트가 선택해 CC.MPS에 쓴 실제 페이지 크기 (보통 4KB) */

	uint32_t			num_aers;
                                  /* [한국어] 현재 발행해둔 AER 수 (컨트롤러가 지원하는 AERL까지) */
	struct nvme_async_event_request	aer[NVME_MAX_ASYNC_EVENTS];
                                  /* [한국어] AER 슬롯 배열 — 최대 NVME_MAX_ASYNC_EVENTS개 동시 대기 */

	/** guards access to the controller itself, including admin queues */
	pthread_mutex_t			ctrlr_lock;
                                  /* [한국어] 컨트롤러 상태와 admin 큐 접근을 보호하는 뮤텍스
                                   *  - robust mutex 사용 — 소유 프로세스가 비정상 종료해도 다른 프로세스가 복구 가능
                                   *  - multi-process 공유 시 필수 */

	struct spdk_nvme_qpair		*adminq;
                                  /* [한국어] admin qpair (qid=0) — 모든 admin 명령이 여기로 발행됨
                                   *  - process_completions도 이 큐에 대해 호출해 identify/AER 진행 */

	/** shadow doorbell buffer */
	uint32_t			*shadow_doorbell;
                                  /* [한국어] shadow doorbell 호스트 메모리 버퍼 — NVMe 1.3+ 최적화
                                   *  - 활성 시 SQ/CQ doorbell MMIO 대신 이 버퍼 write로 통지
                                   *  - nvme_pcie_qpair.shadow_doorbell 필드가 각 qpair별 슬롯을 가리킴 */
	/** eventidx buffer */
	uint32_t			*eventidx;
                                  /* [한국어] 장치가 "이 값 도달 시 MMIO로 깨워달라"고 알려주는 버퍼
                                   *  - nvme_pcie_qpair_need_event의 비교 기준 */

	/**
	 * Identify Controller data.
	 */
	struct spdk_nvme_ctrlr_data	cdata;
                                  /* [한국어] Identify Controller(CNS=01) 응답 4KB 원본 — 제조사/펌웨어/기능 정보 */

	/**
	 * Zoned Namespace Command Set Specific Identify Controller data.
	 */
	struct spdk_nvme_zns_ctrlr_data	*cdata_zns;
                                  /* [한국어] ZNS 지원 컨트롤러에서만 할당 (NULL이면 미지원) */

	struct spdk_bit_array		*free_io_qids;
                                  /* [한국어] I/O QID 할당 비트맵 — 1이면 free, 0이면 사용 중
                                   *  - spdk_nvme_ctrlr_alloc_io_qpair가 이 비트맵에서 비트 하나 잡아 새 qpair의 id로 사용 */
	TAILQ_HEAD(, spdk_nvme_qpair)	active_io_qpairs;
                                  /* [한국어] 이 컨트롤러에 할당된 I/O qpair 리스트 (전역) */

	struct spdk_nvme_ctrlr_opts	opts;
                                  /* [한국어] 사용자/기본값이 섞인 컨트롤러 옵션 (num_io_queues, keep_alive_timeout, hostnqn 등) */

	uint64_t			quirks;
                                  /* [한국어] 벤더·모델별 quirk 플래그 (NVME_QUIRK_*) — Identify 시 자동 매칭 */

	/* Extra sleep time during controller initialization */
	uint64_t			sleep_timeout_tsc;
                                  /* [한국어] 일부 quirk가 요구하는 초기화 중 추가 sleep 시간 */

	/** Track all the processes manage this controller */
	TAILQ_HEAD(, spdk_nvme_ctrlr_process)	active_procs;
                                  /* [한국어] 이 컨트롤러에 attach된 프로세스 리스트 (multi-process) */


	STAILQ_HEAD(, nvme_request)	queued_aborts;
                                  /* [한국어] 대기 중인 abort 요청 — outstanding_aborts 제한 내에서 하나씩 발행 */
	uint32_t			outstanding_aborts;
                                  /* [한국어] 현재 진행 중인 abort 수 */

	uint32_t			lock_depth;
                                  /* [한국어] ctrlr_lock의 재귀 깊이 추적 (SPDK 관례 — 진단 목적) */

	/* CB to notify the user when the ctrlr is removed/failed. */
	spdk_nvme_remove_cb			remove_cb;
                                  /* [한국어] 컨트롤러 제거/실패 시 호출할 사용자 콜백 */
	void					*cb_ctx;
                                  /* [한국어] remove_cb 컨텍스트 */

	struct spdk_nvme_qpair		*external_io_msgs_qpair;
                                  /* [한국어] 외부 I/O message 경로 전용 qpair (cross-thread 요청 전달) */
	pthread_mutex_t			external_io_msgs_lock;
                                  /* [한국어] external_io_msgs 링 보호 */
	struct spdk_ring		*external_io_msgs;
                                  /* [한국어] 외부 스레드가 넣는 I/O 메시지 링 */

	STAILQ_HEAD(, nvme_io_msg_producer) io_producers;
                                  /* [한국어] I/O message 프로듀서(CUSE 등) 목록 */

	struct spdk_nvme_ana_page		*ana_log_page;
                                  /* [한국어] ANA log page 캐시 (NVMe-oF multipath) */
	struct spdk_nvme_ana_group_descriptor	*copied_ana_desc;
                                  /* [한국어] ANA 그룹 설명자 복사본 (분석용) */
	uint32_t				ana_log_page_size;
                                  /* [한국어] ANA log page 크기(바이트) */

	/* scratchpad pointer that can be used to send data between two NVME_CTRLR_STATEs */
	void				*tmp_ptr;
                                  /* [한국어] 상태머신 상태 간 데이터 전달용 임시 포인터 (단계마다 의미 상이) */

	/* maximum zone append size in bytes */
	uint32_t			max_zone_append_size;
                                  /* [한국어] ZNS Zone Append 최대 크기 (ZNS 컨트롤러만 의미) */

	/* PMR size in bytes */
	uint64_t			pmr_size;
                                  /* [한국어] Persistent Memory Region 크기 (없으면 0) */

	/* Boot Partition Info */
	enum nvme_bp_write_state	bp_ws;
                                  /* [한국어] Boot Partition write 상태머신 */
	uint32_t			bpid;
                                  /* [한국어] Boot Partition ID (0 또는 1) */
	spdk_nvme_cmd_cb		bp_write_cb_fn;
                                  /* [한국어] BP write 완료 콜백 */
	void				*bp_write_cb_arg;
                                  /* [한국어] BP write 콜백 컨텍스트 */

	/* Firmware Download */
	void				*fw_payload;
                                  /* [한국어] FW 이미지 페이로드 버퍼 */
	unsigned int			fw_size_remaining;
                                  /* [한국어] 아직 전송 안 한 FW 바이트 수 */
	unsigned int			fw_offset;
                                  /* [한국어] 현재 전송 오프셋 */
	unsigned int			fw_transfer_size;
                                  /* [한국어] 1회 admin 커맨드로 전송하는 FW 청크 크기 */

	/* Completed register operations */
	STAILQ_HEAD(, nvme_register_completion)	register_operations;
                                  /* [한국어] 완료된 property get/set 큐 (NVMe-oF) */

	union spdk_nvme_cc_register		process_init_cc;
                                  /* [한국어] 초기화 중 설정될 CC 레지스터 값 (MPS/CSS/IOSQES/IOCQES 등) */

	/* Authentication transaction ID */
	uint16_t				auth_tid;
                                  /* [한국어] DH-HMAC-CHAP 인증 transaction ID 시퀀서 */
	/* Authentication sequence number */
	uint32_t				auth_seqnum;
                                  /* [한국어] 인증 시퀀스 번호 */
};

/*
 * [한국어] struct spdk_nvme_detach_ctx - 비동기 detach 배치 컨텍스트
 *
 * 사용자가 여러 컨트롤러를 한 번에 detach 요청 시 각 컨트롤러의 detach 진행
 * 상태를 리스트로 보관. 모든 컨트롤러가 detach 완료될 때까지 폴링.
 */
struct spdk_nvme_detach_ctx {
	TAILQ_HEAD(, nvme_ctrlr_detach_ctx)	head;
                                  /* [한국어] detach 중인 컨트롤러들의 상세 상태 리스트 */
};

/*
 * [한국어] struct spdk_nvme_probe_ctx - probe 세션 컨텍스트
 *
 * spdk_nvme_probe/probe_async의 내부 상태. 트랜스포트가 장치를 발견할 때마다
 * probe_cb 호출, 사용자가 attach 의사 표시하면 attach 절차(init_ctrlrs 리스트 진행)
 * 후 완료 시 attach_cb 호출. 초기화 실패는 attach_fail_cb + failed_ctxs에 기록.
 */
struct spdk_nvme_probe_ctx {
	struct spdk_nvme_transport_id		trid;
                                  /* [한국어] probe 대상 트랜스포트 식별 (PCIe 전체 스캔은 빈 trid, NVMe-oF는 특정 target) */
	const struct spdk_nvme_ctrlr_opts	*opts;
                                  /* [한국어] 사용자 지정 기본 ctrlr opts (발견된 컨트롤러에 적용) */
	void					*cb_ctx;
                                  /* [한국어] 공통 사용자 컨텍스트 */
	spdk_nvme_probe_cb			probe_cb;
                                  /* [한국어] 장치 발견 시 "attach 할지?" 묻는 콜백 */
	spdk_nvme_attach_cb			attach_cb;
                                  /* [한국어] attach 성공 시 콜백 */
	spdk_nvme_attach_fail_cb		attach_fail_cb;
                                  /* [한국어] attach 실패 시 콜백 */
	spdk_nvme_remove_cb			remove_cb;
                                  /* [한국어] 이후 하드웨어 제거 시 호출될 콜백 */
	TAILQ_HEAD(, spdk_nvme_ctrlr)		init_ctrlrs;
                                  /* [한국어] 현재 초기화 진행 중인 컨트롤러들 — probe_poll에서 각자 상태머신 진행 */
	/* detach contexts allocated for controllers that failed to initialize */
	struct spdk_nvme_detach_ctx		failed_ctxs;
                                  /* [한국어] 초기화 중 실패한 컨트롤러들의 안전한 해제 진행 */
};

typedef void (*nvme_ctrlr_detach_cb)(struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] detach 완료 콜백 타입 */

/*
 * [한국어] detach 상태머신 — shutdown notification이 안전히 완료되도록 단계별 진행
 *
 * NVMe 스펙은 컨트롤러를 깔끔히 detach하려면 CC.SHN(Shutdown Notification)을
 * 설정하고 CSTS.SHST가 "complete"로 전이할 때까지 대기하라고 요구.
 */
enum nvme_ctrlr_detach_state {
	NVME_CTRLR_DETACH_SET_CC,     /* [한국어] CC.SHN=1 쓰기 (Normal/Abrupt shutdown) */
	NVME_CTRLR_DETACH_CHECK_CSTS, /* [한국어] CSTS 폴링 시작 결정 */
	NVME_CTRLR_DETACH_GET_CSTS,   /* [한국어] CSTS read 발행 */
	NVME_CTRLR_DETACH_GET_CSTS_DONE,
                                  /* [한국어] CSTS read 완료 후 SHST 판정 */
};

/*
 * [한국어] struct nvme_ctrlr_detach_ctx - 개별 컨트롤러 detach 상태
 */
struct nvme_ctrlr_detach_ctx {
	struct spdk_nvme_ctrlr			*ctrlr;
                                  /* [한국어] detach 대상 컨트롤러 */
	nvme_ctrlr_detach_cb			cb_fn;
                                  /* [한국어] 완료 콜백 */
	uint64_t				shutdown_start_tsc;
                                  /* [한국어] shutdown 시작 타임스탬프 — timeout 계산 기준 */
	uint32_t				shutdown_timeout_ms;
                                  /* [한국어] 최대 대기 시간 (ms). 초과 시 강제 종료 */
	bool					shutdown_complete;
                                  /* [한국어] CSTS.SHST == complete 관찰됨 */
	enum nvme_ctrlr_detach_state		state;
                                  /* [한국어] 현재 detach 상태머신 단계 */
	union spdk_nvme_csts_register		csts;
                                  /* [한국어] 마지막으로 읽은 CSTS 값 캐시 */
	TAILQ_ENTRY(nvme_ctrlr_detach_ctx)	link;
                                  /* [한국어] spdk_nvme_detach_ctx->head 리스트 링크 */
};

/*
 * [한국어] struct nvme_driver - NVMe 드라이버 전역 공유 상태
 *
 * 단일 프로세스에는 하나의 인스턴스(g_spdk_nvme_driver). 여러 프로세스가
 * 동일 장치를 attach하면 primary가 이 구조체를 DPDK 공유 메모리에 놓고
 * secondary가 참조 (multi-process 공유 컨트롤러 경로).
 */
struct nvme_driver {
	pthread_mutex_t			lock;
                                  /* [한국어] 전역 드라이버 상태 보호 (컨트롤러 리스트 수정 등)
                                   *  - robust mutex 사용 — 프로세스 비정상 종료 시 복구 가능 */

	/** Multi-process shared attached controller list */
	TAILQ_HEAD(, spdk_nvme_ctrlr)	shared_attached_ctrlrs;
                                  /* [한국어] 공유 컨트롤러 리스트 — 모든 프로세스가 보는 공통 뷰 */

	bool				initialized;
                                  /* [한국어] spdk_nvme_driver_init 완료 표시. secondary가 attach 전 대기 */
	struct spdk_uuid		default_extended_host_id;
                                  /* [한국어] 기본 128-bit Host ID (사용자가 opts로 재정의 가능) */

	/** netlink socket fd for hotplug messages */
	int				hotplug_fd;
                                  /* [한국어] 리눅스 netlink uevent 소켓 — 장치 hot-add/remove 감지 */
};

#define nvme_ns_cmd_get_ext_io_opt(opts, field, defval) \
       ((opts) != NULL && offsetof(struct spdk_nvme_ns_cmd_ext_io_opts, field) + \
        sizeof((opts)->field) <= (opts)->size ? (opts)->field : (defval))

extern struct nvme_driver *g_spdk_nvme_driver;

int nvme_driver_init(void);

#define nvme_delay		usleep

static inline bool
nvme_qpair_is_admin_queue(struct spdk_nvme_qpair *qpair)
/*
 * [한국어]
 * nvme_qpair_is_admin_queue - qpair가 admin 큐(QID=0)인지 판정
 * admin 경로(identify, feature set 등)와 I/O 경로(read/write)를 분기할 때 사용.
 */
{
	return qpair->id == 0;        /* [한국어] NVMe 스펙상 QID 0만 admin 큐 */
}

static inline bool
nvme_qpair_is_io_queue(struct spdk_nvme_qpair *qpair)
/*
 * [한국어]
 * nvme_qpair_is_io_queue - I/O 큐(QID=1~) 여부 판정 (admin의 역)
 */
{
	return qpair->id != 0;
}

static inline int
nvme_robust_mutex_lock(pthread_mutex_t *mtx)
/*
 * [한국어]
 * nvme_robust_mutex_lock - robust 뮤텍스 획득 + owner-dead 복구
 *
 * robust mutex: 뮤텍스 소유 프로세스가 release 전에 죽으면 pthread_mutex_lock이
 *   EOWNERDEAD를 반환. 이때 pthread_mutex_consistent()로 뮤텍스 일관성 복구 후
 *   다른 프로세스가 계속 진행할 수 있다. SPDK multi-process 공유 컨트롤러에서
 *   primary 프로세스가 비정상 종료해도 secondary가 복구 가능.
 * FreeBSD는 robust mutex 미지원 → 컴파일 분기로 제외.
 */
{
	int rc = pthread_mutex_lock(mtx);
                                  /* [한국어] 일반 lock 시도 */

#ifndef __FreeBSD__
	if (rc == EOWNERDEAD) {
                                  /* [한국어] 이전 소유자가 죽은 상태로 뮤텍스 획득됨 — 일관성 복구 필요 */
		rc = pthread_mutex_consistent(mtx);
                                  /* [한국어] 뮤텍스를 사용 가능 상태로 재설정. 이후 정상 동작 */
	}
#endif

	return rc;                    /* [한국어] 0 성공, 그 외 errno */
}

static inline int
nvme_ctrlr_lock(struct spdk_nvme_ctrlr *ctrlr)
/*
 * [한국어]
 * nvme_ctrlr_lock - 컨트롤러 락 획득 + lock_depth 추적
 * robust mutex 기반이므로 다른 프로세스 장애 시에도 안전.
 * lock_depth는 진단·assert용 (재귀 획득 금지 정책 확인 등).
 */
{
	int rc;

	rc = nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
                                  /* [한국어] 뮤텍스 획득 (owner-dead 자동 복구) */
	ctrlr->lock_depth++;          /* [한국어] 진단용 depth 증가 */
	return rc;
}

static inline int
nvme_robust_mutex_unlock(pthread_mutex_t *mtx)
/*
 * [한국어]
 * nvme_robust_mutex_unlock - 뮤텍스 해제 (단순 래퍼, 대칭성 유지용)
 */
{
	return pthread_mutex_unlock(mtx);
}

static inline int
nvme_ctrlr_unlock(struct spdk_nvme_ctrlr *ctrlr)
/*
 * [한국어]
 * nvme_ctrlr_unlock - 컨트롤러 락 해제 + depth 감소
 */
{
	ctrlr->lock_depth--;          /* [한국어] depth 감소 (해제 후 0이어야 정상) */
	return nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
}

/* Poll group management functions. */
/*
 * [한국어] 폴링 그룹 관리 — qpair를 전역 poll group에 연결·해제
 */
int nvme_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair);
                                  /* [한국어] qpair를 해당 트랜스포트 sub-group에 추가 (connected_qpairs 리스트) */
int nvme_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair);
                                  /* [한국어] connected → disconnected 전이. 사용자 콜백으로 통지 후 재연결/삭제 대기 */
void nvme_poll_group_write_disconnect_qpair_fd(struct spdk_nvme_poll_group *group);
                                  /* [한국어] interrupt 모드에서 disconnect 이벤트를 fd로 통지 (epoll wake) */

/* Admin functions */
/*
 * [한국어] ===== admin 커맨드 래퍼들 =====
 *
 * 공통 특징:
 *   - 모두 비동기. cb_fn이 완료 시 호출됨
 *   - admin qpair(qid=0)로 제출 — ctrlr->adminq 경유
 *   - 반환: 0 제출 성공 / 음수 errno 실패
 *   - 내부적으로 nvme_allocate_request + SQE 채우기 + nvme_qpair_submit_request
 *
 * 호출 체인 예:
 *   nvme_ctrlr_cmd_identify → nvme_allocate_request(adminq) → SQE.opc=IDENTIFY,
 *   cdw10=CNS, dptr=payload → nvme_qpair_submit_request → 트랜스포트 계층
 */
int	nvme_ctrlr_cmd_identify(struct spdk_nvme_ctrlr *ctrlr,
				uint8_t cns, uint16_t cntid, uint32_t nsid,
				uint8_t csi, void *payload, size_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg);
                                  /* [한국어] Identify admin 커맨드 (opcode 0x06)
                                   *  @cns:    Controller or Namespace Structure (00=Identify NS, 01=Identify Ctrlr, 02=Active NS List, 03=ID Descriptor List, 1A=allocated NS list 등)
                                   *  @cntid:  Controller ID (secondary 컨트롤러 조회 시)
                                   *  @nsid:   NS ID (NS 관련 CNS에서 유효)
                                   *  @csi:    Command Set ID (NVM=0, KV=1, ZNS=2 — NVMe 2.0)
                                   *  @payload:출력 버퍼 (일반적으로 4096B)
                                   *  컨트롤러 초기화 상태머신의 핵심 admin 명령 */
int	nvme_ctrlr_cmd_set_num_queues(struct spdk_nvme_ctrlr *ctrlr,
				      uint32_t num_queues, spdk_nvme_cmd_cb cb_fn,
				      void *cb_arg);
                                  /* [한국어] Set Features (FID=7: NUMBER_OF_QUEUES) — 호스트 요청 I/O 큐 수 전달. 응답에 장치가 허용한 실제 수 */
int	nvme_ctrlr_cmd_get_num_queues(struct spdk_nvme_ctrlr *ctrlr,
				      spdk_nvme_cmd_cb cb_fn, void *cb_arg);
                                  /* [한국어] Get Features 버전 — 이미 설정된 큐 수 조회 */
int	nvme_ctrlr_cmd_set_async_event_config(struct spdk_nvme_ctrlr *ctrlr,
		union spdk_nvme_feat_async_event_configuration config,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
                                  /* [한국어] Set Features (FID=0x0B: ASYNC_EVENT_CONFIG) — AER로 받을 이벤트 종류 비트맵 설정 */
int	nvme_ctrlr_cmd_set_host_id(struct spdk_nvme_ctrlr *ctrlr, void *host_id, uint32_t host_id_size,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg);
                                  /* [한국어] Set Features (FID=0x81: HOST_ID) — 64-bit 또는 128-bit Host 식별자 등록 */
int	nvme_ctrlr_cmd_attach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
                                  /* [한국어] NS Attach (opcode 0x15, SEL=0) — 특정 NS를 하나 이상의 컨트롤러에 연결 */
int	nvme_ctrlr_cmd_detach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
                                  /* [한국어] NS Attach (SEL=1) 분리 */
int	nvme_ctrlr_cmd_create_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns_data *payload,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg);
                                  /* [한국어] NS Management (opcode 0x0D, SEL=0 Create) — 새 NS 생성 */
int	nvme_ctrlr_cmd_doorbell_buffer_config(struct spdk_nvme_ctrlr *ctrlr,
		uint64_t prp1, uint64_t prp2,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
                                  /* [한국어] Doorbell Buffer Config (opcode 0x7C) — shadow doorbell + eventidx 물리 주소 등록
                                   *  성공 시 컨트롤러가 MMIO 대신 이 메모리를 폴링해 doorbell 업데이트 관찰 */
int	nvme_ctrlr_cmd_delete_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, spdk_nvme_cmd_cb cb_fn,
				 void *cb_arg);
                                  /* [한국어] NS Management (SEL=1 Delete) */
int	nvme_ctrlr_cmd_format(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			      struct spdk_nvme_format *format, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
                                  /* [한국어] Format NVM (opcode 0x80) — LBA 포맷(크기/메타/PI) 변경, 전체 데이터 삭제 */
int	nvme_ctrlr_cmd_fw_commit(struct spdk_nvme_ctrlr *ctrlr,
				 const struct spdk_nvme_fw_commit *fw_commit,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg);
                                  /* [한국어] FW Commit (opcode 0x10) — 다운로드한 펌웨어 이미지를 특정 slot에 커밋 */
int	nvme_ctrlr_cmd_fw_image_download(struct spdk_nvme_ctrlr *ctrlr,
		uint32_t size, uint32_t offset, void *payload,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
                                  /* [한국어] FW Image Download (opcode 0x11) — 펌웨어 청크 전송. 여러 번 발행해 전체 이미지 구성 */
int	nvme_ctrlr_cmd_sanitize(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				struct spdk_nvme_sanitize *sanitize, uint32_t cdw11,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg);
                                  /* [한국어] Sanitize (opcode 0x84) — 전체 NVM을 스펙 정의대로 삭제 (Crypto/Block Erase/Overwrite) */
void	nvme_completion_poll_cb(void *arg, const struct spdk_nvme_cpl *cpl);
                                  /* [한국어] ★ 동기 대기 패턴용 범용 완료 콜백 ★
                                   *  @arg: nvme_completion_poll_status 포인터
                                   *  동작: status->cpl에 CQE 복사 + status->done=true 세팅.
                                   *  사용자 코드가 status->done을 busy-polling으로 대기하면 동기 스타일 API 완성.
                                   *  초기화 상태머신·Fabrics CONNECT 등에서 광범위하게 사용. */

/**
 * Poll admin qpair for completions until a command completes.
 *
 * \param ctrlr ctrlr with adminq to poll
 * \param status completion status. The user must fill this structure with zeroes before calling
 * this function
 * \param release When set to true, releases the status before exit (if the timeout is not hit).
 * Otherwise, it is the caller's responsibility.
 *
 * \return 0 if command completed without error,
 * -EIO if command completed with error,
 * -ECANCELED if command is not completed due to transport/device error or time expired
 *
 *  The command to wait upon must be submitted with nvme_completion_poll_cb as the callback
 *  and status as the callback argument.
 */
int	nvme_wait_for_adminq_completion(struct spdk_nvme_ctrlr *ctrlr,
					struct nvme_completion_poll_status *status, bool release);
                                  /* [한국어] admin 커맨드를 동기 스타일로 대기 — 내부에서 spdk_nvme_ctrlr_process_admin_completions를 주기적으로 호출하며 status->done 폴링
                                   *  @release: true면 status 메모리도 여기서 free
                                   *  사용 전: 해당 커맨드를 nvme_completion_poll_cb + status로 제출해 두어야 함 */

int	nvme_wait_for_completion_poll(struct spdk_nvme_qpair *qpair,
				      struct nvme_completion_poll_status *status);
                                  /* [한국어] 일반 I/O qpair 동기 대기 버전 — process_completions 폴링 */

/*
 * [한국어] ===== 컨트롤러 프로세스 관리 (multi-process) =====
 */
struct spdk_nvme_ctrlr_process *nvme_ctrlr_get_process(struct spdk_nvme_ctrlr *ctrlr,
		pid_t pid);
                                  /* [한국어] 주어진 PID에 해당하는 프로세스 엔트리 조회 (공유 컨트롤러에서) */
struct spdk_nvme_ctrlr_process *nvme_ctrlr_get_current_process(struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] 호출 프로세스(getpid())의 엔트리 조회 — AER cb, timeout cb 등록 대상 */
int	nvme_ctrlr_add_process(struct spdk_nvme_ctrlr *ctrlr, void *devhandle);
                                  /* [한국어] 프로세스가 이 컨트롤러에 attach — active_procs 리스트에 엔트리 추가 */
void	nvme_ctrlr_free_processes(struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] 모든 프로세스 엔트리 해제 (컨트롤러 destruct 시) */
struct spdk_pci_device *nvme_ctrlr_proc_get_devhandle(struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] 현재 프로세스의 PCI 디바이스 핸들 반환 (DPDK 모델) */

int	nvme_ctrlr_probe(const struct spdk_nvme_transport_id *trid,
			 struct spdk_nvme_probe_ctx *probe_ctx, void *devhandle);
                                  /* [한국어] 단일 컨트롤러 probe — spdk_nvme_probe 내부 유틸. 사용자 probe_cb 호출 → attach 결정 */

/*
 * [한국어] ===== 컨트롤러 수명주기 =====
 */
int	nvme_ctrlr_construct(struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] ctrlr 구조체 기본 필드 초기화 + admin qpair 할당 */
void	nvme_ctrlr_destruct_finish(struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] destruct 마지막 단계 — namespace/qpair 자원 해제 */
void	nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] 동기 destruct — shutdown 대기 포함 (블로킹) */
void	nvme_ctrlr_destruct_async(struct spdk_nvme_ctrlr *ctrlr,
				  struct nvme_ctrlr_detach_ctx *ctx);
                                  /* [한국어] 비동기 destruct 시작. 이후 poll_async로 완료 폴링 */
int	nvme_ctrlr_destruct_poll_async(struct spdk_nvme_ctrlr *ctrlr,
				       struct nvme_ctrlr_detach_ctx *ctx);
                                  /* [한국어] 비동기 destruct 진행 폴링. 0=진행 중, 1=완료, 음수=에러 */
void	nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr, bool hot_remove);
                                  /* [한국어] 컨트롤러를 실패 상태로 전환 — 모든 outstanding I/O를 에러로 완료 처리
                                   *  @hot_remove: true면 물리 제거 이벤트로 간주 */
int	nvme_ctrlr_process_init(struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] ★ 초기화 상태머신 한 단계 진행 ★ — spdk_nvme_probe_poll_async에서 주기 호출.
                                   *  현재 state에 해당하는 admin 커맨드 발행 또는 완료 확인 수행 */
void	nvme_ctrlr_disable(struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] CC.EN=0 쓰기 시작 (disable 상태머신 진입) */
int	nvme_ctrlr_disable_poll(struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] disable 완료 폴링 — 1=완료, 0=진행 중 */
void	nvme_ctrlr_connected(struct spdk_nvme_probe_ctx *probe_ctx,
			     struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] probe 중 컨트롤러가 연결된 시점 콜백 — probe_ctx->init_ctrlrs 리스트에 추가 */

int	nvme_ctrlr_submit_admin_request(struct spdk_nvme_ctrlr *ctrlr,
					struct nvme_request *req);
                                  /* [한국어] ★ admin qpair(0)에 nvme_request 제출 — 모든 admin 커맨드 래퍼의 최종 종착 경로 ★ */
/*
 * [한국어] ===== 컨트롤러 레지스터 accessor =====
 *
 * NVMe BAR0 레지스터들을 전송 추상화를 통해 read/write. PCIe에서는 MMIO로
 * 직접 접근, NVMe-oF에서는 Property Get/Set Fabrics 커맨드 경유.
 * set/get 함수들은 내부에서 트랜스포트별 register 접근 콜백으로 디스패치.
 */
int	nvme_ctrlr_get_cap(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_cap_register *cap);
                                  /* [한국어] CAP(Capabilities, 64bit) read — MQES·DSTRD·CSS·MPSMIN/MAX 추출 */
int	nvme_ctrlr_get_vs(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_vs_register *vs);
                                  /* [한국어] VS(Version) read — NVMe 스펙 버전 */
int	nvme_ctrlr_get_cmbsz(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_cmbsz_register *cmbsz);
                                  /* [한국어] CMBSZ read — Controller Memory Buffer 크기·용도 */
int	nvme_ctrlr_get_pmrcap(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_pmrcap_register *pmrcap);
                                  /* [한국어] PMRCAP read — Persistent Memory Region 능력 */
int	nvme_ctrlr_get_bpinfo(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_bpinfo_register *bpinfo);
                                  /* [한국어] BPINFO — Boot Partition 정보 */
int	nvme_ctrlr_set_bprsel(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_bprsel_register *bprsel);
                                  /* [한국어] BPRSEL write — BP read 선택 (영역 크기/오프셋 등) */
int	nvme_ctrlr_set_bpmbl(struct spdk_nvme_ctrlr *ctrlr, uint64_t bpmbl_value);
                                  /* [한국어] BPMBL write — BP memory buffer 위치 */
bool	nvme_ctrlr_multi_iocs_enabled(struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] 다중 Command Set (NVM+ZNS+KV) 활성 여부 (CC.CSS가 IOCS이고 Identify IOCS 성공한 경우) */
void nvme_ctrlr_disconnect_qpair(struct spdk_nvme_qpair *qpair);
                                  /* [한국어] qpair 연결 해제 (I/O 혹은 admin) — 하위 트랜스포트 disconnect 호출 */
void nvme_ctrlr_abort_queued_aborts(struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] 대기 중인 abort 요청 모두 취소 — reset/shutdown 경로 */

/*
 * [한국어] ===== qpair 수명주기 + hot path =====
 */
int nvme_qpair_init(struct spdk_nvme_qpair *qpair, uint16_t id,
		    struct spdk_nvme_ctrlr *ctrlr,
		    enum spdk_nvme_qprio qprio,
		    uint32_t num_requests, bool async);
                                  /* [한국어] qpair 기본 필드 초기화 + nvme_request 풀 할당 */
void	nvme_qpair_deinit(struct spdk_nvme_qpair *qpair);
                                  /* [한국어] qpair 자원 해제 (req 풀 free 등) */
void	nvme_qpair_complete_error_reqs(struct spdk_nvme_qpair *qpair);
                                  /* [한국어] 에러 주입 리스트의 요청들을 즉시 에러 완료 처리 */
int	nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair,
				  struct nvme_request *req);
                                  /* [한국어] ★ 제출 핵심 경로 ★ — req의 cmd를 SQE에 기록 → 트랜스포트 submit
                                   *  qpair 상태가 ENABLED면 즉시 제출, 아니면 queued_req에 임시 저장
                                   *  호출 체인: spdk_nvme_ns_cmd_read → nvme_ns_cmd_rw → [이 함수] → nvme_pcie_qpair_submit_request */
void	nvme_qpair_abort_all_queued_reqs(struct spdk_nvme_qpair *qpair);
                                  /* [한국어] queued_req + aborting_queued_req 전부 에러 완료 */
uint32_t nvme_qpair_abort_queued_reqs_with_cbarg(struct spdk_nvme_qpair *qpair, void *cmd_cb_arg);
                                  /* [한국어] cb_arg 매칭된 queued_req만 abort — spdk_bdev_abort 경로 */
void	nvme_qpair_abort_queued_reqs(struct spdk_nvme_qpair *qpair);
                                  /* [한국어] 모든 queued_req abort (위 variant 단순화 버전) */
void	nvme_qpair_resubmit_requests(struct spdk_nvme_qpair *qpair, uint32_t num_requests);
                                  /* [한국어] queued_req에서 최대 N개 꺼내 재제출 (트랜스포트 재연결 후) */

/*
 * [한국어] ===== namespace 관리 =====
 */
int	nvme_ctrlr_identify_active_ns(struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] 활성 NSID 리스트 재조회 + RB 트리 갱신 (AER namespace change 수신 시) */
void	nvme_ns_set_identify_data(struct spdk_nvme_ns *ns);
                                  /* [한국어] Identify NS 응답을 ns 필드(sector_size/md_size/pi_type 등)로 파싱 */
void	nvme_ns_set_id_desc_list_data(struct spdk_nvme_ns *ns);
                                  /* [한국어] CNS=03 ID 디스크립터 응답을 파싱 (UUID/NGUID/EUI64/CSI) */
void	nvme_ns_free_zns_specific_data(struct spdk_nvme_ns *ns);
                                  /* [한국어] ZNS identify 추가 데이터 해제 */
void	nvme_ns_free_nvm_specific_data(struct spdk_nvme_ns *ns);
                                  /* [한국어] NVM command set 추가 데이터 해제 */
void	nvme_ns_free_iocs_specific_data(struct spdk_nvme_ns *ns);
                                  /* [한국어] ZNS+NVM 모두 해제 (편의 래퍼) */
bool	nvme_ns_has_supported_iocs_specific_data(struct spdk_nvme_ns *ns);
                                  /* [한국어] 이 NS가 특화 identify 데이터를 가지는지 (ZNS/NVM) */
int	nvme_ns_construct(struct spdk_nvme_ns *ns, uint32_t id,
			  struct spdk_nvme_ctrlr *ctrlr);
                                  /* [한국어] NS 객체 구성 — ctrlr 링크, NSID 저장, RB 트리 삽입 */
void	nvme_ns_destruct(struct spdk_nvme_ns *ns);
                                  /* [한국어] NS 객체 해제 — RB 트리 제거, identify 데이터 free */
int	nvme_ns_cmd_zone_append_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
					void *buffer, void *metadata, uint64_t zslba,
					uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
					uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag);
int nvme_ns_cmd_zone_appendv_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				     uint64_t zslba, uint32_t lba_count,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				     spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				     spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
				     uint16_t apptag_mask, uint16_t apptag);

int	nvme_fabric_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value);
int	nvme_fabric_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value);
int	nvme_fabric_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value);
int	nvme_fabric_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value);
int	nvme_fabric_ctrlr_set_reg_4_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint32_t value, spdk_nvme_reg_cb cb_fn, void *cb_arg);
int	nvme_fabric_ctrlr_set_reg_8_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint64_t value, spdk_nvme_reg_cb cb_fn, void *cb_arg);
int	nvme_fabric_ctrlr_get_reg_4_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		spdk_nvme_reg_cb cb_fn, void *cb_arg);
int	nvme_fabric_ctrlr_get_reg_8_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		spdk_nvme_reg_cb cb_fn, void *cb_arg);
int	nvme_fabric_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx, bool direct_connect);
int	nvme_fabric_ctrlr_discover(struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_nvme_probe_ctx *probe_ctx);
int	nvme_fabric_qpair_connect(struct spdk_nvme_qpair *qpair, uint32_t num_entries);
int	nvme_fabric_qpair_connect_async(struct spdk_nvme_qpair *qpair, uint32_t num_entries);
int	nvme_fabric_qpair_connect_poll(struct spdk_nvme_qpair *qpair);
bool	nvme_fabric_qpair_auth_required(struct spdk_nvme_qpair *qpair);
int	nvme_fabric_qpair_authenticate_async(struct spdk_nvme_qpair *qpair);
int	nvme_fabric_qpair_authenticate_poll(struct spdk_nvme_qpair *qpair);
void	nvme_fabric_qpair_poll_cleanup(struct spdk_nvme_qpair *qpair);
void	nvme_fabric_qpair_auth_cleanup(struct spdk_nvme_qpair *qpair, int status);

typedef int (*spdk_nvme_parse_ana_log_page_cb)(
	const struct spdk_nvme_ana_group_descriptor *desc, void *cb_arg);
int	nvme_ctrlr_parse_ana_log_page(struct spdk_nvme_ctrlr *ctrlr,
				      spdk_nvme_parse_ana_log_page_cb cb_fn, void *cb_arg);

static inline void
nvme_request_clear(struct nvme_request *req)
{
	/*
	 * Only memset/zero fields that need it.  All other fields
	 *  will be initialized appropriately either later in this
	 *  function, or before they are needed later in the
	 *  submission patch.  For example, the children
	 *  TAILQ_ENTRY and following members are
	 *  only used as part of I/O splitting so we avoid
	 *  memsetting them until it is actually needed.
	 *  They will be initialized in nvme_request_add_child()
	 *  if the request is split.
	 */
	memset(req, 0, offsetof(struct nvme_request, payload_size));
}

#define NVME_INIT_REQUEST(req, _cb_fn, _cb_arg, _payload, _payload_size, _md_size)	\
	do {						\
		nvme_request_clear(req);		\
		req->cb_fn = _cb_fn;			\
		req->cb_arg = _cb_arg;			\
		req->payload = _payload;		\
		req->payload_size = _payload_size;	\
		req->md_size = _md_size;		\
		req->pid = g_spdk_nvme_pid;		\
		req->submit_tick = 0;			\
		req->accel_sequence = NULL;		\
	} while (0);

static inline struct nvme_request *
nvme_allocate_request(struct spdk_nvme_qpair *qpair,
		      const struct nvme_payload *payload, uint32_t payload_size, uint32_t md_size,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;

	req = STAILQ_FIRST(&qpair->free_req);
	if (req == NULL) {
		return req;
	}

	STAILQ_REMOVE_HEAD(&qpair->free_req, stailq);
	qpair->num_outstanding_reqs++;

	NVME_INIT_REQUEST(req, cb_fn, cb_arg, *payload, payload_size, md_size);

	return req;
}

static inline struct nvme_request *
nvme_allocate_request_contig(struct spdk_nvme_qpair *qpair,
			     void *buffer, uint32_t payload_size,
			     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_payload payload;

	payload = NVME_PAYLOAD_CONTIG(buffer, NULL);

	return nvme_allocate_request(qpair, &payload, payload_size, 0, cb_fn, cb_arg);
}

static inline struct nvme_request *
nvme_allocate_request_null(struct spdk_nvme_qpair *qpair, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_allocate_request_contig(qpair, NULL, 0, cb_fn, cb_arg);
}

struct nvme_request *nvme_allocate_request_user_copy(struct spdk_nvme_qpair *qpair,
		void *buffer, uint32_t payload_size,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg, bool host_to_controller);

static inline void
_nvme_free_request(struct nvme_request *req, struct spdk_nvme_qpair *qpair)
{
	assert(req != NULL);
	assert(req->num_children == 0);
	assert(qpair != NULL);

	/* The reserved_req does not go in the free_req STAILQ - it is
	 * saved only for use with a FABRICS/CONNECT command.
	 */
	if (spdk_likely(qpair->reserved_req != req)) {
		STAILQ_INSERT_HEAD(&qpair->free_req, req, stailq);

		assert(qpair->num_outstanding_reqs > 0);
		qpair->num_outstanding_reqs--;
	}
}

static inline void
nvme_free_request(struct nvme_request *req)
{
	_nvme_free_request(req, req->qpair);
}

static inline void
nvme_complete_request(spdk_nvme_cmd_cb cb_fn, void *cb_arg, struct spdk_nvme_qpair *qpair,
		      struct nvme_request *req, struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_cpl            err_cpl;
	struct nvme_error_cmd           *cmd;

	if (spdk_unlikely(req->accel_sequence != NULL)) {
		assert(qpair->poll_group != NULL);
		struct spdk_nvme_poll_group *pg = qpair->poll_group->group;

		/* Transports are required to execute the sequence and clear req->accel_sequence.
		 * If it's left non-NULL it must mean the request is failed. */
		assert(spdk_nvme_cpl_is_error(cpl));
		pg->accel_fn_table.abort_sequence(req->accel_sequence);
		req->accel_sequence = NULL;
	}

	/* error injection at completion path,
	 * only inject for successful completed commands
	 */
	if (spdk_unlikely(!TAILQ_EMPTY(&qpair->err_cmd_head) &&
			  !spdk_nvme_cpl_is_error(cpl))) {
		TAILQ_FOREACH(cmd, &qpair->err_cmd_head, link) {

			if (cmd->do_not_submit) {
				continue;
			}

			if ((cmd->opc == req->cmd.opc) && cmd->err_count) {

				err_cpl = *cpl;
				err_cpl.status.sct = cmd->status.sct;
				err_cpl.status.sc = cmd->status.sc;

				cpl = &err_cpl;
				cmd->err_count--;
				break;
			}
		}
	}

	/* For PCIe completions, we want to avoid touching the req itself to avoid
	 * dependencies on loading those cachelines. So call the internal helper
	 * function instead using the qpair that was passed by the caller, instead
	 * of getting it from the req.
	 */
	_nvme_free_request(req, qpair);

	if (spdk_likely(cb_fn)) {
		cb_fn(cb_arg, cpl);
	}
}

static inline void
nvme_cleanup_user_req(struct nvme_request *req)
{
	if (req->user_buffer && req->payload_size) {
		spdk_free(req->payload.contig_or_cb_arg);
		req->user_buffer = NULL;
	}

	req->user_cb_arg = NULL;
	req->user_cb_fn = NULL;
}

static inline bool
nvme_request_abort_match(struct nvme_request *req, void *cmd_cb_arg)
{
	return req->cb_arg == cmd_cb_arg ||
	       req->user_cb_arg == cmd_cb_arg ||
	       (req->parent != NULL && req->parent->cb_arg == cmd_cb_arg);
}

const char *nvme_qpair_state_string(enum nvme_qpair_state state);

#define nvme_qpair_set_state(_qpair, _state) do { \
	NVME_QPAIR_DEBUGLOG((_qpair), "setting qpair state to %s\n", nvme_qpair_state_string((_state))); \
	(_qpair)->state = (_state); \
	if ((_state) == NVME_QPAIR_ENABLED) { \
		(_qpair)->is_new_qpair = false; \
	} \
} while (0)

static inline enum nvme_qpair_state
nvme_qpair_get_state(struct spdk_nvme_qpair *qpair) {
	return qpair->state;
}

static inline void
nvme_request_remove_child(struct nvme_request *parent, struct nvme_request *child)
{
	assert(parent != NULL);
	assert(child != NULL);
	assert(child->parent == parent);
	assert(parent->num_children != 0);

	parent->num_children--;
	child->parent = NULL;
	TAILQ_REMOVE(&parent->children, child, child_tailq);
}

static inline void
nvme_cb_complete_child(void *child_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_request *child = child_arg;
	struct nvme_request *parent = child->parent;

	nvme_request_remove_child(parent, child);

	if (spdk_nvme_cpl_is_error(cpl)) {
		memcpy(&parent->parent_status, cpl, sizeof(*cpl));
	}

	if (parent->num_children == 0) {
		nvme_complete_request(parent->cb_fn, parent->cb_arg, parent->qpair,
				      parent, &parent->parent_status);
	}
}

static inline void
nvme_request_add_child(struct nvme_request *parent, struct nvme_request *child)
{
	assert(parent->num_children != UINT16_MAX);

	if (parent->num_children == 0) {
		/*
		 * Defer initialization of the children TAILQ since it falls
		 *  on a separate cacheline.  This ensures we do not touch this
		 *  cacheline except on request splitting cases, which are
		 *  relatively rare.
		 */
		TAILQ_INIT(&parent->children);
		parent->parent = NULL;
		memset(&parent->parent_status, 0, sizeof(struct spdk_nvme_cpl));
	}

	parent->num_children++;
	TAILQ_INSERT_TAIL(&parent->children, child, child_tailq);
	child->parent = parent;
	child->cb_fn = nvme_cb_complete_child;
	child->cb_arg = child;
}

static inline void
nvme_request_free_children(struct nvme_request *req)
{
	struct nvme_request *child, *tmp;

	if (req->num_children == 0) {
		return;
	}

	/* free all child nvme_request */
	TAILQ_FOREACH_SAFE(child, &req->children, child_tailq, tmp) {
		nvme_request_remove_child(req, child);
		nvme_request_free_children(child);
		nvme_free_request(child);
	}
}

int	nvme_request_check_timeout(struct nvme_request *req, uint16_t cid,
				   struct spdk_nvme_ctrlr_process *active_proc, uint64_t now_tick);
uint64_t nvme_get_quirks(const struct spdk_pci_id *id);

int	nvme_robust_mutex_init_shared(pthread_mutex_t *mtx);
int	nvme_robust_mutex_init_recursive_shared(pthread_mutex_t *mtx);

bool	nvme_completion_is_retry(const struct spdk_nvme_cpl *cpl);

struct spdk_nvme_ctrlr *nvme_get_ctrlr_by_trid_unsafe(
	const struct spdk_nvme_transport_id *trid, const char *hostnqn);

const struct spdk_nvme_transport *nvme_get_transport(const char *transport_name);
const struct spdk_nvme_transport *nvme_get_first_transport(void);
const struct spdk_nvme_transport *nvme_get_next_transport(const struct spdk_nvme_transport
		*transport);

/* Transport specific functions */
struct spdk_nvme_ctrlr *nvme_transport_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle);
int nvme_transport_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr);
int nvme_transport_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx, bool direct_connect);
int nvme_transport_ctrlr_scan_attached(struct spdk_nvme_probe_ctx *probe_ctx);
int nvme_transport_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr);
int nvme_transport_ctrlr_ready(struct spdk_nvme_ctrlr *ctrlr);
int nvme_transport_ctrlr_enable_interrupts(struct spdk_nvme_ctrlr *ctrlr);
int nvme_transport_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value);
int nvme_transport_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value);
int nvme_transport_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value);
int nvme_transport_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value);
int nvme_transport_ctrlr_set_reg_4_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint32_t value, spdk_nvme_reg_cb cb_fn, void *cb_arg);
int nvme_transport_ctrlr_set_reg_8_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint64_t value, spdk_nvme_reg_cb cb_fn, void *cb_arg);
int nvme_transport_ctrlr_get_reg_4_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		spdk_nvme_reg_cb cb_fn, void *cb_arg);
int nvme_transport_ctrlr_get_reg_8_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		spdk_nvme_reg_cb cb_fn, void *cb_arg);
uint32_t nvme_transport_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr);
uint16_t nvme_transport_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr);
struct spdk_nvme_qpair *nvme_transport_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
		uint16_t qid, const struct spdk_nvme_io_qpair_opts *opts);
int nvme_transport_ctrlr_reserve_cmb(struct spdk_nvme_ctrlr *ctrlr);
void *nvme_transport_ctrlr_map_cmb(struct spdk_nvme_ctrlr *ctrlr, size_t *size);
int nvme_transport_ctrlr_unmap_cmb(struct spdk_nvme_ctrlr *ctrlr);
int nvme_transport_ctrlr_enable_pmr(struct spdk_nvme_ctrlr *ctrlr);
int nvme_transport_ctrlr_disable_pmr(struct spdk_nvme_ctrlr *ctrlr);
void *nvme_transport_ctrlr_map_pmr(struct spdk_nvme_ctrlr *ctrlr, size_t *size);
int nvme_transport_ctrlr_unmap_pmr(struct spdk_nvme_ctrlr *ctrlr);
void nvme_transport_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair);
int nvme_transport_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr,
				       struct spdk_nvme_qpair *qpair);
void nvme_transport_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair);
void nvme_transport_ctrlr_disconnect_qpair_done(struct spdk_nvme_qpair *qpair);
int nvme_transport_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_memory_domain **domains, int array_size);
int nvme_transport_ctrlr_process_transport_events(struct spdk_nvme_ctrlr *ctrlr);
void nvme_transport_qpair_abort_reqs(struct spdk_nvme_qpair *qpair);
int nvme_transport_qpair_reset(struct spdk_nvme_qpair *qpair);
int nvme_transport_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req);
int nvme_transport_qpair_get_fd(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				struct spdk_event_handler_opts *opts);
int32_t nvme_transport_qpair_process_completions(struct spdk_nvme_qpair *qpair,
		uint32_t max_completions);
void nvme_transport_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair);
int nvme_transport_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
		int (*iter_fn)(struct nvme_request *req, void *arg),
		void *arg);
int nvme_transport_qpair_authenticate(struct spdk_nvme_qpair *qpair);

struct spdk_nvme_transport_poll_group *nvme_transport_poll_group_create(
	const struct spdk_nvme_transport *transport);
int nvme_transport_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
				  struct spdk_nvme_qpair *qpair);
int nvme_transport_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
				     struct spdk_nvme_qpair *qpair);
int nvme_transport_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair);
int nvme_transport_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair);
int64_t nvme_transport_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
void nvme_transport_poll_group_check_disconnected_qpairs(
	struct spdk_nvme_transport_poll_group *tgroup,
	spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
int nvme_transport_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup);
int nvme_transport_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
					struct spdk_nvme_transport_poll_group_stat **stats);
void nvme_transport_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
		struct spdk_nvme_transport_poll_group_stat *stats);
enum spdk_nvme_transport_type nvme_transport_get_trtype(const struct spdk_nvme_transport
		*transport);
/*
 * Below ref related functions must be called with the global
 *  driver lock held for the multi-process condition.
 *  Within these functions, the per ctrlr ctrlr_lock is also
 *  acquired for the multi-thread condition.
 */
void	nvme_ctrlr_proc_get_ref(struct spdk_nvme_ctrlr *ctrlr);
void	nvme_ctrlr_proc_put_ref(struct spdk_nvme_ctrlr *ctrlr);
int	nvme_ctrlr_get_ref_count(struct spdk_nvme_ctrlr *ctrlr);

int	nvme_ctrlr_reinitialize_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);
int	nvme_parse_addr(struct sockaddr_storage *sa, int family,
			const char *addr, const char *service, long int *port);
int	nvme_get_default_hostnqn(char *buf, int len);

static inline bool
_is_page_aligned(uint64_t address, uint64_t page_size)
{
	return (address & (page_size - 1)) == 0;
}

#endif /* __NVME_INTERNAL_H__ */
