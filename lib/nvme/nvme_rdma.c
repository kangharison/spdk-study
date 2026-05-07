/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe over RDMA transport
 */

/*
 * [한국어 설명] NVMe-over-Fabrics RDMA 호스트 트랜스포트 구현 (nvme_rdma.c)
 *
 * === 파일의 역할 ===
 * NVMe-oF 1.0/1.1의 **RDMA 트랜스포트(InfiniBand / RoCE / iWARP)** 호스트 측 구현.
 * SPDK NVMe 드라이버가 원격 NVM 서브시스템과 통신할 때 PCIe BAR MMIO 대신 libibverbs(`ibv_*`)
 * + librdmacm(`rdma_*`) 위에 NVMe Capsule 메시지 + RDMA READ/WRITE 데이터 페치를 구성한다.
 * 이 파일이 RDMA 트랜스포트의 **vtable 구현체(rdma_ops)** 와 그 모든 백엔드 함수(QP 생성, CM 핸드셰이크,
 * Capsule 송수신, 데이터 SGL/UMR/Inline 빌드, 완료 폴링, poll group, hotplug 처리)를 모두 담는다.
 *
 * 5대 핵심 책임:
 *   1) **RDMA QP 라이프사이클**: `rdma_create_id` → `rdma_resolve_addr` → `rdma_resolve_route` →
 *      `ibv_create_qp` (spdk_rdma_provider_qp_create) → `rdma_connect`/`rdma_disconnect`/`rdma_destroy_id`.
 *      각 단계는 비동기로 진행되며 `rdma_event_channel`을 통해 RDMA_CM_EVENT_*로 알림이 온다.
 *   2) **NVMe-oF Fabric CONNECT 핸드셰이크**: RDMA 연결이 ESTABLISHED 된 후 NVMe-oF 표준의 Fabrics
 *      CONNECT 커맨드를 Capsule로 송신해 컨트롤러/큐페어를 등록 (nvme_fabric.c가 실제 처리, 이 파일은
 *      상태머신 진행을 담당).
 *   3) **Capsule + RDMA hybrid I/O 경로**:
 *        - 작은 Write: NVMe Cmd + 인라인 데이터를 한 SEND WR(2 SGE)로 묶어 송신 (in-capsule data, ICD)
 *        - 큰 Write/Read: NVMe Cmd만 SEND로 보내고 keyed SGL(rkey)을 첨부 → 타깃이 RDMA_READ/WRITE로
 *          호스트 메모리에 직접 접근 (zero-copy).
 *      RECV WR로 응답 Capsule(spdk_nvme_cpl)을 미리 큐잉.
 *   4) **CQ 폴링과 poll group**: `ibv_poll_cq` → SEND/RECV 완료를 처리해 `nvme_complete_request`까지
 *      이어지는 hot path. 단일 큐페어 모드와 poll group 공유 CQ + SRQ(Shared Recv Queue) 모드 모두 지원.
 *   5) **메모리 등록(MR)과 SGL 변환**: 페이로드 버퍼는 미리 `ibv_reg_mr`로 등록되어 있어야 RDMA가 가능 →
 *      `spdk_rdma_utils_get_translation`으로 lkey/rkey 조회. UMR(메모리 키 가상 매핑) accel 시퀀스도 지원.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe 트랜스포트 vtable 구조에서 PCIe(nvme_pcie.c)와 형제뻘인 NVMe-oF RDMA 구현체.
 * 파일 맨 아래 `SPDK_NVME_TRANSPORT_REGISTER(rdma, &rdma_ops)` 매크로가 main() 진입 전에 nvme_transport.c
 * 의 전역 TAILQ에 RDMA ops를 삽입 → 이후 상위 레이어(spdk_nvme_probe → nvme_transport_*)가 이 파일의 함수
 * 들을 vtable로 호출. PCIe와 다른 점: BAR MMIO 대신 nvme_fabric.c의 Property Set/Get 커맨드를 사용
 * (rdma_ops.ctrlr_set_reg_4 = nvme_fabric_ctrlr_set_reg_4 위임).
 *
 * 호출 체인 (Connect 시퀀스):
 *   [Application]
 *     → spdk_nvme_connect / spdk_nvme_probe (subnqn=DISCOVERY_NQN 또는 직접 NQN)
 *     → nvme_transport_ctrlr_construct → ops.ctrlr_construct
 *         → nvme_rdma_ctrlr_construct ★ — rctrlr 할당, rdma_create_event_channel, admin qpair 생성
 *     → nvme_transport_ctrlr_connect_qpair → ops.ctrlr_connect_qpair
 *         → nvme_rdma_ctrlr_connect_qpair ★
 *             → rdma_create_id (cm_id 할당)
 *             → nvme_rdma_resolve_addr → rdma_resolve_addr (CM event: ADDR_RESOLVED)
 *                 → nvme_rdma_addr_resolved (콜백)
 *                     → rdma_resolve_route (CM event: ROUTE_RESOLVED)
 *                         → nvme_rdma_route_resolved
 *                             → nvme_rdma_qpair_init ★ — ibv_create_qp + ibv_create_cq
 *                             → nvme_rdma_connect ★ — rdma_connect (CM event: ESTABLISHED)
 *                                 → nvme_rdma_connect_established
 *                                     → spdk_rdma_utils_create_mem_map (MR 등록 풀)
 *                                     → nvme_rdma_create_reqs / nvme_rdma_create_rsps (SEND/RECV 풀)
 *                                     → nvme_rdma_qpair_submit_recvs (RECV WR 사전 큐잉)
 *                                     → state = FABRIC_CONNECT_SEND
 *             → nvme_rdma_ctrlr_connect_qpair_poll (상태머신)
 *                 → nvme_fabric_qpair_connect_async (FABRIC_CONNECT 커맨드 송신)
 *                 → nvme_fabric_qpair_connect_poll → state = RUNNING
 *
 * 호출 체인 (I/O 송신):
 *   [Application] → spdk_nvme_ns_cmd_read/write
 *     → nvme_qpair_submit_request → ops.qpair_submit_request
 *         → nvme_rdma_qpair_submit_request ★
 *             → nvme_rdma_req_get (free_reqs 풀에서 rdma_req 획득)
 *             → nvme_rdma_req_init → build_null/contig/sgl_request (SGL/inline 빌드)
 *             → _nvme_rdma_qpair_submit_request → spdk_rdma_provider_qp_queue_send_wrs
 *                 → nvme_rdma_qpair_submit_sends → ibv_post_send (SEND WR 게시)
 *
 * 호출 체인 (완료 폴링):
 *   [Reactor poller] → spdk_nvme_qpair_process_completions
 *     → ops.qpair_process_completions = nvme_rdma_qpair_process_completions
 *         → nvme_rdma_cq_process_completions → ibv_poll_cq → 각 WC에 대해
 *             → nvme_rdma_process_recv_completion (RECV WC: 응답 Capsule 도착)
 *             → nvme_rdma_process_send_completion (SEND WC: 커맨드 송신 ACK)
 *                 → nvme_rdma_request_ready → nvme_complete_request (cb_fn)
 *
 * === 타 모듈과의 연결 ===
 *  - **상위 호출자**: nvme_transport.c (vtable dispatch), nvme_ctrlr.c (ctrlr 수명/상태머신),
 *    nvme_qpair.c (큐페어 수명).
 *  - **하위 의존**:
 *      * libibverbs (`ibv_*`)            — QP/CQ/MR/SGE/WR (RDMA verbs API)
 *      * librdmacm  (`rdma_*`)           — 연결 관리(ADDR/ROUTE resolve, connect, event channel)
 *      * spdk_internal/rdma_provider.h   — SPDK의 verbs 추상 (Mellanox direct verbs/표준 verbs 분기)
 *      * spdk_internal/rdma_utils.h      — MR 풀 관리, lkey/rkey 변환
 *      * nvme_fabric.c                   — Fabric CONNECT, Property Set/Get, Discovery 공통
 *      * nvme_internal.h                 — nvme_request, spdk_nvme_qpair, nvme_complete_request
 *  - **공유 자료구조**:
 *      * `struct nvme_rdma_ctrlr`      — spdk_nvme_ctrlr를 감싸는 RDMA 트랜스포트 컨테이너 (cm_channel 보관)
 *      * `struct nvme_rdma_qpair`      — spdk_nvme_qpair를 감싸 cm_id/rdma_qp/cq/srq 추가
 *      * `struct nvme_rdma_poller`     — poll group 내부에서 1 RDMA device당 1개 — 공유 CQ/SRQ 보유
 *      * `struct spdk_nvme_rdma_req`   — NVMe 요청 1개당 1개의 send_wr/send_sgl/cpl 보유
 *      * `struct spdk_nvme_rdma_rsp`   — RECV WR로 미리 게시된 응답 버퍼 + recv_wr
 *  - **전역**: `g_nvme_hooks` (사용자 제공 ibv_pd/MR 등록 후크), `rdma_cm_event_str[]` (디버그 문자열).
 *
 * === 주요 함수/구조체 요약 ===
 *  ★ 컨트롤러 수명:
 *      nvme_rdma_ctrlr_construct        — 컨트롤러 + cm_channel + admin qpair 생성
 *      nvme_rdma_ctrlr_destruct         — cm_channel 파괴, pending CM 이벤트 ack
 *  ★ 큐페어 수명/CM 핸드셰이크:
 *      nvme_rdma_ctrlr_connect_qpair    — rdma_create_id + addr/route 시작 (비동기)
 *      nvme_rdma_resolve_addr/_addr_resolved/_route_resolved/_connect/_connect_established — 5단계 핸드셰이크
 *      nvme_rdma_qpair_init             — ibv_create_qp + CQ + PD 결정
 *      nvme_rdma_ctrlr_disconnect_qpair / _qpair_disconnected / _qpair_wait_until_quiet
 *      nvme_rdma_qpair_destroy          — QP/CQ/cm_id 해제
 *  ★ CM 이벤트 처리:
 *      nvme_rdma_qpair_process_cm_event — 단일 이벤트 디스패치 (ESTABLISHED/DISCONNECTED/...)
 *      nvme_rdma_poll_events            — cm_channel에서 모든 pending 이벤트 수확
 *      nvme_rdma_validate_cm_event      — 기대값과 실제 이벤트 비교 (stale conn 검출)
 *  ★ I/O 빌드 (NVMe-oF SGL 변형):
 *      nvme_rdma_build_null_request          — 페이로드 없음
 *      nvme_rdma_build_contig_request        — keyed SGL (RDMA_READ/WRITE)
 *      nvme_rdma_build_contig_inline_request — in-capsule inline data (SEND 2 SGE)
 *      nvme_rdma_build_sgl_request           — multi-segment keyed SGL
 *      nvme_rdma_build_sgl_inline_request    — multi-segment, inline 가능한 첫 segment만
 *      nvme_rdma_apply_accel_sequence        — UMR (가상 contig MR) 경로
 *  ★ I/O hot path:
 *      nvme_rdma_qpair_submit_request   — vtable의 submit 진입점
 *      _nvme_rdma_qpair_submit_request  — send_wr 큐잉 + post_send
 *      nvme_rdma_cq_process_completions — ibv_poll_cq → 각 WC를 SEND/RECV로 분기 처리
 *      nvme_rdma_process_recv_completion / _send_completion / _request_ready
 *  ★ Poll group:
 *      nvme_rdma_poll_group_create / _destroy
 *      nvme_rdma_poller_create / _destroy   — 1 device 당 1개의 공유 CQ/SRQ poller
 *      nvme_rdma_poll_group_process_completions — 모든 poller의 CQ를 한 사이클에 폴링
 *  ★ rdma_ops 테이블: 파일 맨 아래에 정의 — vtable로 export.
 *
 * === RDMA 핵심 도메인 지식 (이 파일 전반에 등장하는 용어) ===
 *   - **WR (Work Request)**: 호스트가 RDMA HCA에 게시하는 작업. SEND/RECV/RDMA_READ/RDMA_WRITE 등.
 *     이 파일에서 RDMA_READ/WRITE는 **타깃이** 발행(서버 측 zero-copy DMA), 호스트는 SEND/RECV만 사용.
 *   - **SGE (Scatter-Gather Element)**: WR이 가리키는 메모리 영역 {addr, length, lkey}. 다중 SGE로 흩어진 버퍼 표현.
 *   - **MR (Memory Region)**: ibv_reg_mr로 커널/HCA에 등록된 페이지 핀된 영역. lkey(local) + rkey(remote) 발급.
 *   - **PD (Protection Domain)**: 같은 HCA 컨텍스트에서 QP/MR이 공유하는 보호 영역.
 *   - **CQ (Completion Queue)**: WR 완료(WC = Work Completion)가 적재되는 큐. ibv_poll_cq로 수확.
 *   - **QP (Queue Pair)**: SQ(Send Queue) + RQ(Recv Queue) 한 쌍. 한 RDMA 연결 = 한 QP.
 *   - **SRQ (Shared Recv Queue)**: 여러 QP가 RECV WR을 공유하는 큐. poll group에서 메모리 절약 목적.
 *   - **CM (Connection Manager, librdmacm)**: 주소 해상도/연결 협상을 캡슐화한 사용자 라이브러리.
 *     `rdma_create_id` → `rdma_resolve_addr` → `rdma_resolve_route` → `rdma_connect` → 이벤트 채널로 알림.
 *   - **Capsule (NVMe-oF)**: NVMe 커맨드 64B + 옵션 in-capsule data를 한 메시지로 묶은 것.
 *     호스트 → 타깃은 SQE(Submission Queue Entry) Capsule, 타깃 → 호스트는 CQE(Completion) Capsule.
 *   - **ICD (In-Capsule Data)**: 작은 Write 데이터를 SQE Capsule에 인라인. ioccsz/icdoff 컨트롤러 속성으로 결정.
 *   - **Keyed SGL**: SGL descriptor에 rkey 포함 → 타깃이 호스트 메모리에 RDMA_READ/WRITE 가능.
 *   - **UMR (User Mode Memory Region, Mellanox)**: 동적으로 가상 contig MR을 만들어 scatter 버퍼를
 *     단일 키로 표현. accel sequence와 결합해 zero-copy 변환에 활용.
 */

#include "spdk/stdinc.h"				/* [한국어] 표준 C 헤더 (stdint, string, ...) - SPDK 공통 진입점 */

#include "spdk/assert.h"				/* [한국어] SPDK_STATIC_ASSERT — 컴파일타임 구조체 크기/오프셋 검증 */
#include "spdk/dma.h"					/* [한국어] spdk_memory_domain — 하이브리드 DMA 추상 (RDMA/iouring/...) */
#include "spdk/log.h"					/* [한국어] SPDK_ERRLOG/DEBUGLOG — 통합 로깅 */
#include "spdk/trace.h"					/* [한국어] SPDK 트레이스 포인트 (현재 파일에서는 미사용에 가까움) */
#include "spdk/queue.h"					/* [한국어] BSD-style TAILQ/STAILQ 매크로 — free_reqs/outstanding/CM 이벤트 큐에 사용 */
#include "spdk/nvme.h"					/* [한국어] 공개 NVMe API (spdk_nvme_qpair, spdk_nvme_cpl 등) */
#include "spdk/nvmf_spec.h"				/* [한국어] NVMe-oF 스펙 헤더: Capsule 구조, fabric private data 정의 */
#include "spdk/string.h"				/* [한국어] spdk_strerror 등 문자열 헬퍼 */
#include "spdk/endian.h"				/* [한국어] LE/BE 변환 (RDMA 페이로드는 little-endian이지만 NVMe 스펙도 그러함) */
#include "spdk/likely.h"				/* [한국어] spdk_likely/unlikely - hot path 분기 예측 */
#include "spdk/config.h"				/* [한국어] SPDK_CONFIG_RDMA_SET_ACK_TIMEOUT 등 빌드 옵션 매크로 */

#include "nvme_internal.h"				/* [한국어] 드라이버 내부: nvme_request, qpair 상태머신, complete_request */
#include "spdk_internal/rdma_provider.h"		/* [한국어] SPDK RDMA provider — 표준/Mellanox direct verbs 백엔드 추상 */
#include "spdk_internal/rdma_utils.h"			/* [한국어] MR 풀 (mem_map), PD 캐시, lkey/rkey 변환 헬퍼 */

/* [한국어] CM(Connection Manager) 동작 타임아웃 — rdma_resolve_addr/route, rdma_connect 등에 공통 사용.
 *         2초로 짧게 설정해 빠른 실패 감지(끊긴 타깃에 무한 대기 방지). */
#define NVME_RDMA_TIME_OUT_IN_MS 2000
/* [한국어] (현재 미사용) 과거 RW 버퍼 크기 상수. 코드 변경하지 않으므로 그대로 둠. */
#define NVME_RDMA_RW_BUFFER_SIZE 131072

/*
 * NVME RDMA qpair Resource Defaults
 */
/* [한국어] SEND WR이 가질 SGE 개수 기본값. 보통 2 = [SQE Capsule, In-Capsule Data].
 *         첫 번째 SGE는 NVMe Cmd, 두 번째는 inline write 데이터 페이로드. */
#define NVME_RDMA_DEFAULT_TX_SGE		2
/* [한국어] RECV WR이 가질 SGE 개수 기본값. 응답 Capsule(spdk_nvme_cpl 16B만 받으면 됨)이라 1로 충분. */
#define NVME_RDMA_DEFAULT_RX_SGE		1

/* Max number of NVMe-oF SGL descriptors supported by the host */
/* [한국어] 한 NVMe Cmd가 표현할 수 있는 최대 SGL descriptor 수.
 *         Multi-segment SGL(여러 개의 keyed data block)을 한 캡슐 안에 채울 때 상한. */
#define NVME_RDMA_MAX_SGL_DESCRIPTORS		16

/* number of STAILQ entries for holding pending RDMA CM events. */
/* [한국어] cm_channel에서 한꺼번에 읽힌 CM 이벤트 중 즉시 처리할 수 없는 것을 보관할 슬롯 수.
 *         rdma_get_cm_event는 ack 전까지 새 이벤트를 못 받으므로 큐잉이 필요. */
#define NVME_RDMA_NUM_CM_EVENTS			256

/* The default size for a shared rdma completion queue. */
/* [한국어] poll group 모드에서 SRQ를 안 쓰는 경우의 공유 CQ 기본 크기 (WC 슬롯 수). */
#define DEFAULT_NVME_RDMA_CQ_SIZE		4096

/*
 * In the special case of a stale connection we don't expose a mechanism
 * for the user to retry the connection so we need to handle it internally.
 */
/* [한국어] Stale connection이란? 타깃이 이전 세션의 잔재(QP context)를 갖고 있어서 새 연결이 거부되는 상황.
 *         RDMA_CM_EVENT_REJECTED + status=10(IB_CM_REJ_STALE_CONN)로 보고됨.
 *         스펙상 호스트에 별도 retry API가 노출되지 않으므로 트랜스포트 내부에서 자동 재시도. */
#define NVME_RDMA_STALE_CONN_RETRY_MAX		5
#define NVME_RDMA_STALE_CONN_RETRY_DELAY_US	10000	/* [한국어] 재시도 간격 10ms — 타깃이 이전 세션 정리할 시간 확보 */

/*
 * Maximum value of transport_retry_count used by RDMA controller
 */
/* [한국어] rdma_conn_param.retry_count 최대값. RDMA 패킷 손실 시 NACK 후 재전송 시도 횟수. */
#define NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT	7

/*
 * Maximum value of transport_ack_timeout used by RDMA controller
 */
/* [한국어] RDMA ACK 대기 타임아웃 (4.096us * 2^timeout). InfiniBand 스펙상 최대 31. */
#define NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT	31

/*
 * Number of microseconds to wait until the lingering qpair becomes quiet.
 */
/* [한국어] disconnect 후 in-flight WR이 모두 flush될 때까지 기다릴 최대 시간 (1초).
 *         초과하면 강제로 quiet 상태로 전환 후 자원 해제. */
#define NVME_RDMA_DISCONNECTED_QPAIR_TIMEOUT_US	1000000ull

/*
 * The max length of keyed SGL data block (3 bytes)
 */
/* [한국어] NVMe-oF Keyed SGL의 length 필드는 3바이트 → 최대 16MB-1.
 *         이보다 큰 페이로드는 하나의 SGL로 표현 불가 → 분할 필요. */
#define NVME_RDMA_MAX_KEYED_SGL_LENGTH ((1u << 24u) - 1)

/* [한국어] queue depth N 큐페어가 필요로 하는 CQ 엔트리 수 = 2*N (SEND 완료 + RECV 완료 각각). */
#define WC_PER_QPAIR(queue_depth)	(queue_depth * 2)

/* [한국어] WC->qp_num이 특정 rqpair의 RDMA QP 번호와 일치하는지 검사하는 매크로.
 *         poll group 공유 CQ에서 어느 큐페어 소유의 완료인지 라우팅할 때 사용. */
#define NVME_RDMA_POLL_GROUP_CHECK_QPN(_rqpair, qpn)				\
	((_rqpair)->rdma_qp && (_rqpair)->rdma_qp->qp->qp_num == (qpn))	\

/* [한국어] rqpair 기반 로그 매크로 — 내부적으로 qpair 포인터를 추출해 NVME_QPAIR_*LOG으로 위임.
 *         rqpair NULL 안전(NULL이면 qpair=NULL로 대체)하므로 init 실패 경로에서도 호출 가능. */
#define NVME_RQPAIR_ERRLOG(rqpair, format, ...) NVME_QPAIR_ERRLOG((rqpair) ? &(rqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_RQPAIR_WARNLOG(rqpair, format, ...) NVME_QPAIR_WARNLOG((rqpair) ? &(rqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_RQPAIR_NOTICELOG(rqpair, format, ...) NVME_QPAIR_NOTICELOG((rqpair) ? &(rqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_RQPAIR_INFOLOG(rqpair, format, ...) NVME_QPAIR_INFOLOG((rqpair) ? &(rqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_RQPAIR_DEBUGLOG(rqpair, format, ...) NVME_QPAIR_DEBUGLOG((rqpair) ? &(rqpair)->qpair : NULL, format, ##__VA_ARGS__)

/* [한국어] WR 종류 식별자.
 * CQ에서 ibv_wc->wr_id로 nvme_rdma_wr 구조체를 복원한 뒤 type을 보고 SEND vs RECV 처리 분기.
 * (RDMA_READ/WRITE는 호스트가 직접 게시하지 않으므로 여기엔 없음 — 타깃이 발행) */
enum nvme_rdma_wr_type {
	RDMA_WR_TYPE_RECV,	/* [한국어] 응답 Capsule 수신용 RECV WR (spdk_nvme_rdma_rsp 내부) */
	RDMA_WR_TYPE_SEND,	/* [한국어] 커맨드 Capsule 송신용 SEND WR (spdk_nvme_rdma_req 내부) */
};

/* [한국어] WR 식별을 위한 미니 헤더. send_wr->wr_id, recv_wr->wr_id가 이 구조체의 주소를 가리킴.
 * CQ poll 후 SPDK_CONTAINEROF로 부모 spdk_nvme_rdma_req/_rsp를 복원. */
struct nvme_rdma_wr {
	/* Using this instead of the enum allows this struct to only occupy one byte. */
	uint8_t	type;
	/* [한국어] enum nvme_rdma_wr_type 값. enum 대신 uint8_t 사용 이유는 주석에 적힌 대로 1바이트 고정.
	 * 설정자: nvme_rdma_create_reqs/_create_rsps 초기화 시.
	 * 읽는 자: nvme_rdma_cq_process_completions의 switch(rdma_wr->type).
	 * 값 범위: RDMA_WR_TYPE_RECV(0) 또는 RDMA_WR_TYPE_SEND(1). */
};

/* [한국어] NVMe-oF Submission Queue Entry (Capsule 본체).
 * 64B NVMe Cmd 뒤에 in-capsule data 또는 multi-segment SGL descriptor 리스트가 따라옴.
 * 호스트는 이 구조체를 SEND WR로 송신, 타깃은 RECV WR로 수신. */
struct spdk_nvmf_cmd {
	struct spdk_nvme_cmd cmd;
	/* [한국어] 표준 NVMe 64B 커맨드(opcode, nsid, dptr, cdw10..15). dptr.sgl1 필드가 in-capsule 데이터/SGL 정보.
	 * 설정자: nvme_rdma_build_*_request 함수들이 dptr.sgl1을 설정.
	 * 읽는 자: 타깃 측 NVMe-oF 처리기 (서버 측 nvmf_rdma_request). */
	struct spdk_nvme_sgl_descriptor sgl[NVME_RDMA_MAX_SGL_DESCRIPTORS];
	/* [한국어] Multi-segment SGL일 때 추가 descriptor 배열 — Cmd의 dptr이 LAST_SEGMENT type을 가리키면 사용.
	 * 설정자: nvme_rdma_build_sgl_request의 cmd->sgl[num_sgl_desc] = ... 루프.
	 * 읽는 자: 타깃이 RDMA_READ/WRITE를 발행할 때 각 데이터 블록의 (addr, length, rkey)를 참고. */
};

/* [한국어] 사용자가 spdk_nvme_rdma_init_hooks로 주입할 수 있는 후크 구조체.
 * get_ibv_pd: 사용자 정의 PD 제공 (예: GPU Direct RDMA용 PD).
 * 기본은 빈 구조체 = SPDK 자동 PD 사용. */
struct spdk_nvme_rdma_hooks g_nvme_hooks = {};

/* STAILQ wrapper for cm events. */
/* [한국어] CM 이벤트를 미루어 둘 큐 엔트리.
 * rdma_get_cm_event는 ack 전 한 번에 1개만 반환 → 여러 큐페어 이벤트가 섞이면 다른 큐페어용은 여기 보관. */
struct nvme_rdma_cm_event_entry {
	struct rdma_cm_event			*evt;
	/* [한국어] librdmacm이 할당한 CM 이벤트 객체. ack 전까지 유효, 사용 후 rdma_ack_cm_event 필수.
	 * 설정자: nvme_rdma_poll_events의 rdma_get_cm_event 결과.
	 * 읽는 자: nvme_rdma_qpair_process_cm_event가 evt->event/param을 해석. */
	STAILQ_ENTRY(nvme_rdma_cm_event_entry)	link;
	/* [한국어] STAILQ 링크. pending_cm_events / free_cm_events 두 큐 사이를 오감. */
};

/* NVMe RDMA transport extensions for spdk_nvme_ctrlr */
/* [한국어] 컨트롤러 단위 RDMA 트랜스포트 컨테이너.
 * spdk_nvme_ctrlr를 임베드하는 형태로 SPDK_CONTAINEROF로 상호 변환.
 * 컨트롤러당 1개의 cm_channel을 보유 → 모든 큐페어 CM 이벤트가 여기로 들어옴. */
struct nvme_rdma_ctrlr {
	struct spdk_nvme_ctrlr			ctrlr;
	/* [한국어] 임베드된 일반 NVMe 컨트롤러. 첫 필드로 둬서 컨테이너 캐스팅이 자연스러움.
	 * 설정자: nvme_rdma_ctrlr_construct에서 trid/opts 복사.
	 * 읽는 자: 모든 ops 함수 (vtable 진입점). */

	uint16_t				max_sge;
	/* [한국어] 사용 가능한 RDMA HCA들의 최소 max_sge 값 (모든 device를 ibv_query_device로 조회 후 min 집계).
	 * 큐페어 생성 시 max_send/recv_sge 상한으로 사용. NVME_RDMA_MAX_SGL_DESCRIPTORS와 작은 값을 사용.
	 * 설정자: nvme_rdma_ctrlr_construct의 device 열거 루프.
	 * 읽는 자: nvme_rdma_ctrlr_get_max_sges가 컨트롤러 capability 노출 시 참조. */

	struct rdma_event_channel		*cm_channel;
	/* [한국어] librdmacm 이벤트 채널 — 비동기 CM 이벤트 (ADDR_RESOLVED, ESTABLISHED, DISCONNECTED 등) 전달.
	 * 컨트롤러에 속한 모든 cm_id가 이 채널을 공유. fd는 nonblock으로 설정 (poll group 통합 가능).
	 * 설정자: ctrlr_construct에서 rdma_create_event_channel.
	 * 읽는 자: nvme_rdma_poll_events가 rdma_get_cm_event로 폴. */

	STAILQ_HEAD(, nvme_rdma_cm_event_entry)	pending_cm_events;
	/* [한국어] 받았지만 아직 처리 안 된 CM 이벤트들. 큐페어가 다른 이벤트를 기다리는 동안 잠시 대기.
	 * 동시성: ctrlr_lock 보호 (poll_events 함수 주석 참조). */

	STAILQ_HEAD(, nvme_rdma_cm_event_entry)	free_cm_events;
	/* [한국어] 사전 할당된 free 엔트리 풀. NVME_RDMA_NUM_CM_EVENTS 만큼 생성 후 분배. */

	struct nvme_rdma_cm_event_entry		*cm_events;
	/* [한국어] cm_events 엔트리 배열의 시작 포인터. ctrlr_destruct에서 spdk_free로 해제. */
};

/* [한국어] poller 단위 통계 — RPC로 노출되어 운영 모니터링용. */
struct nvme_rdma_poller_stats {
	uint64_t polls;					/* [한국어] ibv_poll_cq를 호출한 총 횟수 */
	uint64_t idle_polls;				/* [한국어] 폴 결과 0 (완료 없음) — busy poll 효율성 지표 */
	uint64_t queued_requests;			/* [한국어] free_reqs 고갈로 큐잉으로 밀린 요청 수 — backpressure 지표 */
	uint64_t completions;				/* [한국어] 누적 처리한 WC 개수 (SEND+RECV 합산) */
	struct spdk_rdma_provider_qp_stats rdma_stats;	/* [한국어] provider 백엔드의 send/recv WR doorbell 통계 */
};

struct nvme_rdma_poll_group;
struct nvme_rdma_rsps;

/* [한국어] poll group 내부의 1 RDMA device당 1개의 공유 자원 묶음.
 * 같은 ibv_context(=같은 HCA)에 속한 여러 큐페어가 한 CQ/SRQ/PD/MR map을 공유 → 메모리 절약과 폴링 효율 ↑. */
struct nvme_rdma_poller {
	struct ibv_context		*device;
	/* [한국어] 이 poller가 담당하는 RDMA HCA 컨텍스트 (rdma_cm_id->verbs와 일치).
	 * poll_group_get_poller가 device 일치 검색의 키. */
	struct ibv_cq			*cq;
	/* [한국어] 공유 Completion Queue. 이 device 위 모든 큐페어의 SEND/RECV 완료가 적재됨.
	 * ibv_poll_cq로 한 번에 여러 큐페어의 완료를 수확 가능. */
	struct spdk_rdma_provider_srq	*srq;
	/* [한국어] (옵션) 공유 RECV Queue. NULL이면 큐페어별 RQ 사용.
	 * SRQ 사용 시 모든 큐페어 응답 RECV WR이 여기로 모임 → 메모리/스케일 이득. */
	struct nvme_rdma_rsps		*rsps;
	/* [한국어] SRQ 모드일 때만 사용 — 공유 응답 버퍼 풀. */
	struct ibv_pd			*pd;
	/* [한국어] 이 poller의 PD. SRQ/MR을 위한 보호 도메인. */
	struct spdk_rdma_utils_mem_map	*mr_map;
	/* [한국어] SRQ용 MR 풀 — RECV 버퍼 lkey 변환에 사용. */
	uint32_t			refcnt;
	/* [한국어] 이 poller를 사용하는 큐페어 수. 0이 되면 destroy.
	 * get_poller에서 ++, put_poller에서 -- (싱글 스레드 가정 → atomic 불필요). */
	int				required_num_wc;
	/* [한국어] 현재 등록된 큐페어들이 요구하는 총 WC 슬롯 수 (sum of WC_PER_QPAIR(num_entries)).
	 * resize_cq 판단 기준. */
	int				current_num_wc;
	/* [한국어] CQ에 현재 할당된 실제 WC 슬롯 수. ibv_resize_cq로 동적 조정. */
	struct nvme_rdma_poller_stats	stats;
	/* [한국어] 통계 (위 구조체 참조). */
	struct nvme_rdma_poll_group	*group;
	/* [한국어] 부모 poll group 역참조. */
	STAILQ_ENTRY(nvme_rdma_poller)	link;
	/* [한국어] poll_group->pollers 리스트 링크. */
};

struct nvme_rdma_qpair;

/* [한국어] poll group — 여러 큐페어를 한 reactor 스레드에서 일괄 폴링하는 컨테이너.
 * 같은 group 안에서 같은 device에 속한 큐페어들은 자동으로 같은 poller(=공유 CQ)를 사용. */
struct nvme_rdma_poll_group {
	struct spdk_nvme_transport_poll_group		group;
	/* [한국어] 임베드된 일반 poll group. 첫 필드로 둬 SPDK_CONTAINEROF 변환. */
	STAILQ_HEAD(, nvme_rdma_poller)			pollers;
	/* [한국어] 1 device당 1 poller 리스트. nvme_rdma_poll_group_get/put_poller로 관리. */
	uint32_t					num_pollers;
	/* [한국어] 통계용 카운트. completions_per_poller 산출에 사용. */
	TAILQ_HEAD(, nvme_rdma_qpair)			connecting_qpairs;
	/* [한국어] connect 진행 중인 큐페어들. process_completions에서 connect_qpair_poll 진척시키는 워크큐. */
	TAILQ_HEAD(, nvme_rdma_qpair)			active_qpairs;
	/* [한국어] 송신할 WR 또는 미처리 큐잉 요청이 있는 큐페어 — process_submits로 일괄 doorbell. */
};

/* [한국어] 큐페어 내부 상태머신 — 연결~사용~종료 단계 추적.
 * connect_qpair_poll/disconnect_qpair_poll에서 switch로 분기. */
enum nvme_rdma_qpair_state {
	NVME_RDMA_QPAIR_STATE_INVALID = 0,			/* [한국어] 초기값 / 미사용 */
	NVME_RDMA_QPAIR_STATE_STALE_CONN,			/* [한국어] stale conn 감지 → 재시도 대기 중 */
	NVME_RDMA_QPAIR_STATE_INITIALIZING,			/* [한국어] CM 핸드셰이크 진행 (ADDR/ROUTE/ESTABLISHED 대기) */
	NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_SEND,		/* [한국어] RDMA ESTABLISHED 후 Fabrics CONNECT 커맨드 송신 단계 */
	NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_POLL,		/* [한국어] CONNECT 응답 대기 polling */
	NVME_RDMA_QPAIR_STATE_AUTHENTICATING,			/* [한국어] DH-CHAP 등 인증 진행 중 */
	NVME_RDMA_QPAIR_STATE_RUNNING,				/* [한국어] 정상 I/O 송수신 가능 */
	NVME_RDMA_QPAIR_STATE_EXITING,				/* [한국어] disconnect 시작 (rdma_disconnect 호출됨) */
	NVME_RDMA_QPAIR_STATE_LINGERING,			/* [한국어] 미완료 WR flush 대기 (timeout 시 강제 quiet) */
	NVME_RDMA_QPAIR_STATE_EXITED,				/* [한국어] 자원 해제 완료 — 안전하게 free 가능 */
};

/* [한국어] CM 이벤트 처리 콜백 시그니처.
 * 비동기 핸드셰이크 단계별로 다음 단계로 진행하는 후속 작업을 콜백으로 등록.
 * @rqpair: 이벤트 대상 큐페어. @ret: validate_cm_event 결과 (0=성공/그 외 실패). */
typedef int (*nvme_rdma_cm_event_cb)(struct nvme_rdma_qpair *rqpair, int ret);

/* [한국어] 응답 풀(rsps) 생성 옵션 묶음. 큐페어별 RQ 또는 group 공용 SRQ 양쪽에서 재사용. */
struct nvme_rdma_rsp_opts {
	uint16_t				num_entries;	/* [한국어] 만들 RECV 슬롯 수 */
	struct nvme_rdma_qpair			*rqpair;	/* [한국어] 큐페어 모드일 때 부모. SRQ 모드는 NULL */
	struct spdk_rdma_provider_srq		*srq;		/* [한국어] SRQ 모드일 때 RECV WR 게시 대상. 큐페어 모드는 NULL */
	struct spdk_rdma_utils_mem_map		*mr_map;	/* [한국어] 응답 버퍼 lkey 조회용 MR 풀 */
};

/* [한국어] 응답 풀 — SoA(Struct of Arrays) 레이아웃으로 RECV WR + SGE + 응답 버퍼를 병렬 배열로 관리.
 * 캐시 친화 + ibv_post_recv 일괄 게시에 유리. */
struct nvme_rdma_rsps {
	/* Parallel arrays of response buffers + response SGLs of size num_entries */
	struct ibv_sge				*rsp_sgls;
	/* [한국어] RECV WR이 가리키는 SGE 배열. addr=&rsps[i], length=sizeof(spdk_nvme_cpl), lkey=MR lkey. */
	struct spdk_nvme_rdma_rsp		*rsps;
	/* [한국어] 실제 응답 버퍼 배열 (DMA 가능한 spdk_zmalloc 메모리).
	 * 타깃이 SEND한 응답 Capsule이 여기에 RDMA로 직접 적재됨. */

	struct ibv_recv_wr			*rsp_recv_wrs;
	/* [한국어] RECV WR 배열. wr_id=&rsps[i].rdma_wr 로 설정 (CQ 처리 시 역참조용). */

	/* Count of outstanding recv objects */
	uint16_t				current_num_recvs;
	/* [한국어] 현재 게시되어 응답 대기 중인 RECV WR 개수.
	 * 게시 시 ++, 완료 처리 시 -- (rqpair 단일 스레드 가정 → atomic 불필요). */

	uint16_t				num_entries;
	/* [한국어] 풀 전체 슬롯 수 (할당 시 고정). */
};

/* NVMe RDMA qpair extensions for spdk_nvme_qpair */
/* [한국어] RDMA 트랜스포트 큐페어 — 일반 spdk_nvme_qpair를 임베드하여 RDMA 자원을 추가.
 * NVMe SQ/CQ 한 쌍 = RDMA QP 한 개 = TCP/IP 소켓 한 개 와 같은 1:1 매핑. */
struct nvme_rdma_qpair {
	struct spdk_nvme_qpair			qpair;
	/* [한국어] 임베드된 일반 큐페어. 첫 필드 → SPDK_CONTAINEROF로 변환. */

	struct spdk_rdma_provider_qp		*rdma_qp;
	/* [한국어] SPDK provider 추상의 QP 핸들. 내부에 ibv_qp + 송수신 WR 큐잉 버퍼 포함.
	 * 설정자: nvme_rdma_qpair_init의 spdk_rdma_provider_qp_create.
	 * 읽는 자: WR 게시 (queue_send_wrs/queue_recv_wrs), CQ 매칭 (qp->qp_num). */
	struct rdma_cm_id			*cm_id;
	/* [한국어] librdmacm의 connection identifier. 주소/포트/QP를 묶는 핸들.
	 * cm_id->verbs = ibv_context, cm_id->qp = 결합된 ibv_qp.
	 * 설정자: ctrlr_connect_qpair의 rdma_create_id.
	 * 읽는 자: rdma_resolve_addr/route/connect/disconnect/destroy 모두 cm_id 인자 필요. */
	struct ibv_cq				*cq;
	/* [한국어] 이 큐페어의 Completion Queue.
	 * - poll group 모드: poller->cq를 가리킴 (공유)
	 * - standalone 모드: ibv_create_cq로 전용 생성 (qpair_destroy에서 ibv_destroy_cq) */
	struct spdk_rdma_provider_srq		*srq;
	/* [한국어] (옵션) poll group의 공유 SRQ. NULL이면 큐페어 전용 RQ 사용. */

	struct	spdk_nvme_rdma_req		*rdma_reqs;
	/* [한국어] 사전 할당된 RDMA 요청 풀 (num_entries 개). free_reqs/outstanding_reqs 큐에 분배.
	 * 설정자: nvme_rdma_create_reqs.
	 * 읽는 자: 모든 I/O 경로 (req_get/put). */

	uint32_t				max_send_sge;
	/* [한국어] 이 QP의 send SGE 최대치 (HCA 능력과 NVME_RDMA_DEFAULT_TX_SGE 중 작은 값).
	 * ibv_create_qp가 attr.cap.max_send_sge를 실제 가능값으로 줄일 수 있어 별도 보관. */

	uint16_t				num_entries;
	/* [한국어] 큐 깊이 (qsize - 1). NVMe 스펙상 큐 사이즈 N이면 동시에 N-1개만 outstanding 가능. */

	bool					delay_cmd_submit;
	/* [한국어] true면 send_wr를 큐잉만 하고 doorbell(post_send) 보류 → 배치 효과.
	 * process_completions 끝에서 일괄 flush. 인라인 작은 IO에서 효율 ↑. */
	/* Append copy task even if no accel sequence is attached to IO.
	 * Result is UMR configured per IO data buffer */
	bool					append_copy;
	/* [한국어] 모든 I/O에 자동으로 accel copy 시퀀스를 첨부 (UMR 사용)할지 여부.
	 * IO마다 가상 contig MR을 만들어 multi-segment 페이로드를 단일 SGL로 표현 가능. */

	uint32_t				num_completions;
	/* [한국어] 현재 폴링 사이클에서 처리한 완료 수. 매 호출마다 0으로 리셋. */
	uint32_t				num_outstanding_reqs;
	/* [한국어] outstanding_reqs 리스트 길이의 빠른 카운터 (TAILQ 길이 O(N) 회피용). */

	struct nvme_rdma_rsps			*rsps;
	/* [한국어] 응답 풀 — 큐페어 전용(create_rsps 호출 결과) 또는 SRQ 모드의 공유 포인터.
	 * SRQ 모드는 poller->rsps와 같음. */

	/*
	 * Array of num_entries NVMe commands registered as RDMA message buffers.
	 * Indexed by rdma_req->id.
	 */
	struct spdk_nvmf_cmd			*cmds;
	/* [한국어] 사전 할당된 NVMe Cmd Capsule 버퍼 배열 — DMA 가능 메모리이므로 ibv_reg_mr 등록 가능.
	 * rdma_req->id로 인덱스. send_sgl[0].addr이 cmds[id]를 가리킴. */

	struct spdk_rdma_utils_mem_map		*mr_map;
	/* [한국어] 큐페어 전용 MR 풀. spdk_rdma_utils_create_mem_map으로 생성, IBV_ACCESS_LOCAL_WRITE|REMOTE_READ|REMOTE_WRITE 권한.
	 * 데이터 페이로드 lkey/rkey 변환에 사용. */

	TAILQ_HEAD(, spdk_nvme_rdma_req)	free_reqs;
	/* [한국어] 사용 가능한 rdma_req 풀. submit 시 head에서 pop, complete 시 head에 push (LIFO=캐시 친화). */
	TAILQ_HEAD(, spdk_nvme_rdma_req)	outstanding_reqs;
	/* [한국어] 송신 후 응답 대기 중인 rdma_req. timeout 검사와 abort 시 순회. */

	/* Count of outstanding send objects */
	uint16_t				current_num_sends;
	/* [한국어] 게시되었지만 SEND 완료가 안 온 WR 수. SQ overflow 방지 + flush 시 reset_failed_sends에서 감소. */
	/* Number of requests submitted to accel framework */
	uint16_t				num_active_accel_reqs;
	/* [한국어] accel 시퀀스 처리 중인 요청 수. disconnect 시 모두 완료될 때까지 LINGERING 상태 유지. */

	TAILQ_ENTRY(nvme_rdma_qpair)		link_active;
	/* [한국어] poll_group->active_qpairs 링크. 송신할 일이 있을 때만 enqueued. */

	/* Placed at the end of the struct since it is not used frequently */
	struct rdma_cm_event			*evt;
	/* [한국어] 현재 처리 대기 중인 CM 이벤트 (없으면 NULL). 처리 후 rdma_ack_cm_event + NULL. */
	struct nvme_rdma_poller			*poller;
	/* [한국어] 소속 poller 역참조 (poll group 모드에서만). */

	uint64_t				evt_timeout_ticks;
	/* [한국어] 다음 CM 이벤트 또는 lingering 종료 데드라인 (spdk_get_ticks 단위). */
	nvme_rdma_cm_event_cb			evt_cb;
	/* [한국어] 기대 이벤트 도착 시 호출할 콜백 (route_resolved → connect_established 등 체인 진행). */
	enum rdma_cm_event_type			expected_evt_type;
	/* [한국어] 다음에 기대하는 이벤트 종류. validate_cm_event가 다른 이벤트 도착 시 에러 처리. */

	enum nvme_rdma_qpair_state		state;
	/* [한국어] 큐페어 내부 상태머신 (위 enum). connect/disconnect 진행 추적. */

	uint8_t					stale_conn_retry_count;
	/* [한국어] stale connection 자동 재시도 누적 횟수. NVME_RDMA_STALE_CONN_RETRY_MAX 도달 시 포기. */
	bool					need_destroy;
	/* [한국어] DEVICE_REMOVAL 같은 비복구 이벤트 발생 시 set → 강제 정리. */
	bool					connected;
	/* [한국어] CM ESTABLISHED 도달 후 true. disconnect 시 false → rdma_disconnect 발사 여부 결정. */
	TAILQ_ENTRY(nvme_rdma_qpair)		link_connecting;
	/* [한국어] poll_group->connecting_qpairs 링크. CONNECT 완료 시 detach. */
};

/* [한국어] 한 NVMe 요청은 SEND WR 1개 + 그에 대응하는 RECV WR 1개로 완료됨.
 * 두 WC가 모두 도착해야 호스트 입장에서 완료 처리 가능 → 비트마스크로 진행 추적.
 * (RECV가 SEND보다 먼저 올 수도 있으므로 OR로 누적) */
enum NVME_RDMA_COMPLETION_FLAGS {
	NVME_RDMA_SEND_COMPLETED = 1u << 0,	/* [한국어] SEND WR 완료됨 (커맨드 송신 ACK) */
	NVME_RDMA_RECV_COMPLETED = 1u << 1,	/* [한국어] RECV WR 완료됨 (응답 Capsule 수신) */
};

/* [한국어] 한 NVMe-oF I/O 요청을 표현하는 RDMA 트랜스포트 객체.
 * rqpair->rdma_reqs[id] 배열 슬롯이며, free 풀과 outstanding 풀 사이를 오감.
 * SEND WR 1개와 ibv_sge 배열을 임베드해 zero-allocation hot path 보장. */
struct spdk_nvme_rdma_req {
	uint16_t				id;
	/* [한국어] rdma_reqs[id] 배열 인덱스 (= 응답 cpl.cid가 가리키는 값).
	 * RECV 처리 시 cpl.cid → rdma_reqs[cid]로 빠른 역참조 가능. */
	uint16_t				completion_flags: 2;
	/* [한국어] NVME_RDMA_SEND_COMPLETED|RECV_COMPLETED 비트마스크. 둘 다 set 되면 request_ready 호출.
	 * 비트필드 2개로 압축 → 캐시 라인 절약. req_put에서 0으로 리셋. */
	uint16_t				in_progress_accel: 1;
	/* [한국어] accel 시퀀스 처리 중. abort 시에도 강제 종료 못 하므로 LINGERING 대기. */
	uint16_t				reserved: 13;
	/* [한국어] 비트필드 패딩 — 향후 확장 용도. */
	/* if completion of RDMA_RECV received before RDMA_SEND, we will complete nvme request
	 * during processing of RDMA_SEND. To complete the request we must know the response
	 * received in RDMA_RECV, so store it in this field */
	struct spdk_nvme_rdma_rsp		*rdma_rsp;
	/* [한국어] RECV가 먼저 도착했을 때 그 응답 객체 포인터를 보관 → SEND 완료 시 함께 처리. */

	struct spdk_nvme_cpl			cpl;
	/* [한국어] rdma_rsp->cpl을 복사한 로컬 cpl. transfer_cpl_cb 경로에서 사용. */

	struct nvme_rdma_wr			rdma_wr;
	/* [한국어] WR 헤더 (type=SEND). send_wr.wr_id가 이 멤버를 가리킴 → CQ에서 rdma_req 복원. */

	struct ibv_send_wr			send_wr;
	/* [한국어] libibverbs SEND WR. opcode=IBV_WR_SEND, send_flags=IBV_SEND_SIGNALED.
	 * sg_list = send_sgl, num_sge = 1 또는 2 (inline data 여부). */

	struct nvme_request			*req;
	/* [한국어] 상위 NVMe 레이어의 일반 요청 객체. cb_fn/cb_arg/payload 보유. */

	struct ibv_sge				send_sgl[NVME_RDMA_DEFAULT_TX_SGE];
	/* [한국어] SEND WR의 SGE 배열 (최대 2개).
	 * [0] = NVMe Cmd Capsule (rqpair->cmds[id]).
	 * [1] = (옵션) inline 데이터 페이로드 (작은 Write에 한해). */

	TAILQ_ENTRY(spdk_nvme_rdma_req)		link;
	/* [한국어] free_reqs 또는 outstanding_reqs 리스트 링크. */

	/* Fields below are not used in regular IO path, keep them last */
	/* [한국어] 일반 I/O 경로에서 안 쓰이는 필드 — 캐시 라인을 분리해 hot path 캐시 적중률 ↑. */
	spdk_memory_domain_data_cpl_cb		transfer_cpl_cb;
	/* [한국어] memory_domain_transfer_data 콜백 (UMR 경로 등). NULL이면 일반 NVMe 완료 경로 사용. */
	void					*transfer_cpl_cb_arg;
	/* [한국어] transfer_cpl_cb의 cb_arg. */
	/* Accel sequence API works with iovec pointer, we need to store result of next_sge callback */
	struct iovec				iovs[NVME_RDMA_MAX_SGL_DESCRIPTORS];
	/* [한국어] accel 시퀀스용 iovec 임시 버퍼 — payload SGL을 iovec으로 변환해 accel API에 전달. */
};

/* [한국어] 응답 Capsule을 받기 위해 미리 ibv_post_recv 해 두는 객체.
 * 풀에 num_entries 만큼 만들어두고, 사용 후 다시 RECV 게시 (slot recycling). */
struct spdk_nvme_rdma_rsp {
	struct spdk_nvme_cpl	cpl;
	/* [한국어] 16바이트 NVMe 완료 큐 엔트리. 타깃이 RDMA로 직접 적재.
	 * 설정자: 타깃 (RDMA SEND inbound).
	 * 읽는 자: nvme_rdma_process_recv_completion → cpl.cid로 rdma_req 복원. */
	struct nvme_rdma_qpair	*rqpair;
	/* [한국어] 소속 큐페어. SRQ가 없을 때 process_recv_completion이 빠르게 큐페어 추적. */
	struct ibv_recv_wr	*recv_wr;
	/* [한국어] 이 응답 슬롯이 사용한 RECV WR. 처리 후 다시 게시할 때 재사용. */
	struct nvme_rdma_wr	rdma_wr;
	/* [한국어] WR 헤더 (type=RECV). recv_wr->wr_id가 이 멤버 주소. */
};

/* [한국어] 메모리 변환 결과 임시 컨테이너 — 페이로드 버퍼 → RDMA 키 변환을 함수 인자로 모음. */
struct nvme_rdma_memory_translation_ctx {
	void *addr;	/* [한국어] 변환 대상 가상 주소 (= I/O 페이로드 시작) */
	size_t length;	/* [한국어] 길이. NVME_RDMA_MAX_KEYED_SGL_LENGTH 이내여야 함 */
	uint32_t lkey;	/* [한국어] Local key — 호스트 측 RDMA 접근용 (SEND inline 시 sg_list.lkey) */
	uint32_t rkey;	/* [한국어] Remote key — 타깃이 RDMA_READ/WRITE할 때 dptr.sgl1.keyed.key에 박힘 */
};

static const char *rdma_cm_event_str[] = {
	"RDMA_CM_EVENT_ADDR_RESOLVED",
	"RDMA_CM_EVENT_ADDR_ERROR",
	"RDMA_CM_EVENT_ROUTE_RESOLVED",
	"RDMA_CM_EVENT_ROUTE_ERROR",
	"RDMA_CM_EVENT_CONNECT_REQUEST",
	"RDMA_CM_EVENT_CONNECT_RESPONSE",
	"RDMA_CM_EVENT_CONNECT_ERROR",
	"RDMA_CM_EVENT_UNREACHABLE",
	"RDMA_CM_EVENT_REJECTED",
	"RDMA_CM_EVENT_ESTABLISHED",
	"RDMA_CM_EVENT_DISCONNECTED",
	"RDMA_CM_EVENT_DEVICE_REMOVAL",
	"RDMA_CM_EVENT_MULTICAST_JOIN",
	"RDMA_CM_EVENT_MULTICAST_ERROR",
	"RDMA_CM_EVENT_ADDR_CHANGE",
	"RDMA_CM_EVENT_TIMEWAIT_EXIT"
};

static struct nvme_rdma_poller *nvme_rdma_poll_group_get_poller(struct nvme_rdma_poll_group *group,
		struct ibv_context *device);
static void nvme_rdma_poll_group_put_poller(struct nvme_rdma_poll_group *group,
		struct nvme_rdma_poller *poller);

static int nvme_rdma_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair);

static inline int nvme_rdma_memory_domain_transfer_data(struct spdk_memory_domain *dst_domain,
		void *dst_domain_ctx,
		struct iovec *dst_iov, uint32_t dst_iovcnt,
		struct spdk_memory_domain *src_domain, void *src_domain_ctx,
		struct iovec *src_iov, uint32_t src_iovcnt,
		struct spdk_memory_domain_translation_result *translation,
		spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

static inline int _nvme_rdma_qpair_submit_request(struct nvme_rdma_qpair *rqpair,
		struct spdk_nvme_rdma_req *rdma_req);

/*
 * [한국어]
 * nvme_rdma_qpair - 일반 spdk_nvme_qpair 포인터를 RDMA 트랜스포트 nvme_rdma_qpair로 캐스팅.
 *
 * @qpair: 임베드된 일반 큐페어 포인터 (vtable 콜백 인자로 들어옴).
 * @return: 컨테이너 nvme_rdma_qpair 포인터 (NULL 불가, 첫 필드 → 안전 캐스트).
 *
 * SPDK_CONTAINEROF는 멤버 오프셋을 빼서 부모 구조체 주소를 복원하는 매크로.
 * trtype 어서션으로 잘못된 트랜스포트의 큐페어 변환을 막음 (디버그 빌드 한정).
 * 실행 컨텍스트: 모든 vtable 진입점에서 가장 먼저 호출되는 hot path → inline + 단순.
 *
 * 호출 체인:
 *   nvme_rdma_*ops 콜백 → [nvme_rdma_qpair] → 본문 처리
 */
static inline struct nvme_rdma_qpair *
nvme_rdma_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->trtype == SPDK_NVME_TRANSPORT_RDMA);	/* [한국어] 디버그 가드 — RDMA 큐페어가 아니면 즉시 abort */
	return SPDK_CONTAINEROF(qpair, struct nvme_rdma_qpair, qpair);	/* [한국어] (qpair 멤버 → 부모 nvme_rdma_qpair) 오프셋 복원 */
}

/*
 * [한국어]
 * nvme_rdma_poll_group - 일반 transport_poll_group 포인터를 RDMA poll_group으로 캐스팅.
 *
 * @group: 임베드된 일반 poll group.
 * @return: 컨테이너 nvme_rdma_poll_group 포인터.
 *
 * 위 nvme_rdma_qpair와 동일한 패턴 — 첫 필드 임베드 + SPDK_CONTAINEROF.
 * poll group 모드에서 ops.poll_group_* 콜백 진입 시 변환 용도.
 */
static inline struct nvme_rdma_poll_group *
nvme_rdma_poll_group(struct spdk_nvme_transport_poll_group *group)
{
	return (SPDK_CONTAINEROF(group, struct nvme_rdma_poll_group, group));	/* [한국어] 부모 group 컨테이너 복원 */
}

/*
 * [한국어]
 * nvme_rdma_ctrlr - 일반 spdk_nvme_ctrlr 포인터를 RDMA 트랜스포트 nvme_rdma_ctrlr로 캐스팅.
 *
 * @ctrlr: 임베드된 일반 컨트롤러 (ctrlr 콜백 진입점에서 들어옴).
 * @return: 컨테이너 nvme_rdma_ctrlr 포인터.
 *
 * trid.trtype 어서션으로 RDMA가 아닌 컨트롤러를 잘못 캐스팅하는 사고 방지.
 * cm_channel/pending_cm_events에 접근해야 할 때 가장 먼저 호출됨.
 */
static inline struct nvme_rdma_ctrlr *
nvme_rdma_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_RDMA);	/* [한국어] RDMA 트랜스포트 검증 */
	return SPDK_CONTAINEROF(ctrlr, struct nvme_rdma_ctrlr, ctrlr);	/* [한국어] 부모 컨테이너 복원 */
}

/*
 * [한국어]
 * nvme_rdma_req_get - 사전 할당된 free 풀에서 RDMA 요청 객체 1개 dequeue.
 *
 * @rqpair: 풀을 보유한 RDMA 큐페어.
 * @return: 사용 가능한 spdk_nvme_rdma_req 포인터 또는 NULL (풀 고갈 시).
 *
 * I/O hot path의 진입점. malloc 없이 미리 num_entries개를 만들어 둔 풀에서 즉시 획득
 * → polled-mode 성능 보장 (allocator lock contention 회피).
 * NULL 반환은 backpressure 신호로 호출자가 -EAGAIN 처리 (큐잉 후 재시도).
 *
 * 동기화: rqpair는 단일 SPDK 스레드에 고정 (poll_group 또는 별도 스레드) → 락 불필요.
 *
 * 호출 체인:
 *   nvme_rdma_qpair_submit_request → [nvme_rdma_req_get] → req_init → _submit_request
 */
static inline struct spdk_nvme_rdma_req *
nvme_rdma_req_get(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_req *rdma_req;	/* [한국어] free 풀 head 객체 임시 저장 */

	rdma_req = TAILQ_FIRST(&rqpair->free_reqs);	/* [한국어] LIFO 헤드 — req_put이 INSERT_HEAD로 넣으므로 캐시 친화 */
	if (spdk_likely(rdma_req)) {		/* [한국어] 정상 경로 분기 예측 — 일반적으로 풀에 여유 있음 */
		TAILQ_REMOVE(&rqpair->free_reqs, rdma_req, link);	/* [한국어] dequeue — outstanding으로 옮길 준비 */
	}

	return rdma_req;	/* [한국어] NULL이면 호출자가 -EAGAIN 반환해 상위 레이어에서 재시도 */
}

/*
 * [한국어]
 * nvme_rdma_req_put - 완료된 RDMA 요청 객체를 free 풀에 반납.
 *
 * @rqpair: 풀 소유자.
 * @rdma_req: 반납할 요청 객체.
 *
 * 요청 완료(또는 실패) 후 호출. 다음 사용을 위한 상태 초기화 + 풀 head로 push.
 * INSERT_HEAD로 LIFO 효과 → 최근 사용한 슬롯이 캐시에 남아 있을 확률 ↑.
 *
 * 호출 체인:
 *   nvme_rdma_req_complete / fail 경로 → [nvme_rdma_req_put]
 */
static inline void
nvme_rdma_req_put(struct nvme_rdma_qpair *rqpair, struct spdk_nvme_rdma_req *rdma_req)
{
	rdma_req->completion_flags = 0;	/* [한국어] SEND/RECV 완료 비트 리셋 — 다음 IO에서 다시 OR 누적 */
	rdma_req->req = NULL;		/* [한국어] 상위 nvme_request 연결 끊기 — 댕글링 방지 */
	rdma_req->rdma_rsp = NULL;	/* [한국어] 응답 슬롯 포인터 정리 */
	assert(rdma_req->transfer_cpl_cb == NULL);	/* [한국어] memory_domain transfer 콜백이 남아있으면 안 됨 (이미 호출됐어야 함) */
	TAILQ_INSERT_HEAD(&rqpair->free_reqs, rdma_req, link);	/* [한국어] LIFO 풀 head로 push */
}

/*
 * [한국어]
 * nvme_rdma_finish_data_transfer - memory_domain transfer 콜백 종료 처리.
 *
 * @rdma_req: 콜백을 보유한 요청.
 * @rc: 전송 결과 (0=성공, <0=오류).
 *
 * UMR / accel sequence 경로에서 데이터 변환 완료 후 호출되며, 한 번 호출 후 cb 포인터를 NULL로 클리어해
 * 중복 호출 방지. cb는 spdk memory_domain API의 비동기 완료 알림 메커니즘.
 */
static inline void
nvme_rdma_finish_data_transfer(struct spdk_nvme_rdma_req *rdma_req, int rc)
{
	spdk_memory_domain_data_cpl_cb cb = rdma_req->transfer_cpl_cb;	/* [한국어] 호출 전 콜백 포인터 캡처 */

	SPDK_DEBUGLOG(nvme, "req %p, finish data transfer, rc %d\n", rdma_req, rc);	/* [한국어] 디버그 트레이스 */
	rdma_req->transfer_cpl_cb = NULL;	/* [한국어] 클리어 — 한 번만 호출 보장, req_put assert 통과 */
	assert(cb);	/* [한국어] 콜백 없으면 호출하면 안 됨 */
	cb(rdma_req->transfer_cpl_cb_arg, rc);	/* [한국어] memory_domain 측 후속 처리 (예: accel 시퀀스 종료) */
}

/*
 * [한국어]
 * nvme_rdma_req_complete - 요청을 outstanding에서 제거하고 상위 NVMe 레이어 완료 처리 후 풀 반납.
 *
 * @rdma_req: 완료된 요청 객체.
 * @rsp: 응답 CQE (성공/오류 정보 포함). 정상 경로는 rdma_rsp->cpl, 실패 주입 경로는 합성된 cpl.
 * @print_on_error: 오류 시 cmd/cpl을 stderr에 출력할지. AER 등 expected 오류는 false.
 *
 * 완료 처리 흐름:
 *   1) 오류 검사 + 디버그 로그 출력
 *   2) outstanding 큐에서 제거 + 카운터 감소
 *   3) nvme_complete_request → 상위 cb_fn (애플리케이션 완료 콜백) 호출
 *   4) nvme_rdma_req_put → free 풀에 슬롯 반납
 *
 * 실행 컨텍스트: ibv_poll_cq를 부른 reactor 스레드 (process_recv/send_completion 경로).
 *
 * 호출 체인:
 *   nvme_rdma_request_ready → [nvme_rdma_req_complete] → nvme_complete_request → req->cb_fn
 *   abort/fail 경로도 합성 cpl을 만들어 이 함수를 직접 호출.
 */
static void
nvme_rdma_req_complete(struct spdk_nvme_rdma_req *rdma_req,
		       struct spdk_nvme_cpl *rsp,
		       bool print_on_error)
{
	struct nvme_request *req = rdma_req->req;	/* [한국어] 상위 NVMe 요청 객체 */
	struct nvme_rdma_qpair *rqpair;			/* [한국어] outstanding 큐 보유 큐페어 */
	struct spdk_nvme_qpair *qpair;			/* [한국어] 일반 qpair (cb_arg/print 함수에 필요) */
	bool error, print_error;			/* [한국어] CPL 상태 분기용 플래그 */

	assert(req != NULL);	/* [한국어] req_put 후 호출되면 NULL — 사용 후 반납 순서 가드 */

	qpair = req->qpair;	/* [한국어] req에 보존된 큐페어 역참조 */
	rqpair = nvme_rdma_qpair(qpair);	/* [한국어] RDMA 컨테이너 캐스팅 */

	error = spdk_nvme_cpl_is_error(rsp);	/* [한국어] CPL.status.SCT/SC가 오류면 true (NVMe 1.x 4.6.3) */
	print_error = error && print_on_error && !qpair->ctrlr->opts.disable_error_logging;	/* [한국어] 오류이면서 출력 옵션 켜진 경우 */

	if (print_error) {
		spdk_nvme_qpair_print_command(qpair, &req->cmd);	/* [한국어] NVMe 명령 디코딩 출력 */
	}

	if (print_error || SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
		spdk_nvme_qpair_print_completion(qpair, rsp);	/* [한국어] NVMe CPL 디코딩 출력 (디버그 빌드 또는 오류 시) */
	}

	assert(rqpair->num_outstanding_reqs > 0);	/* [한국어] outstanding 카운터 underflow 가드 */
	rqpair->num_outstanding_reqs--;	/* [한국어] outstanding 카운터 감소 */

	TAILQ_REMOVE(&rqpair->outstanding_reqs, rdma_req, link);	/* [한국어] outstanding 큐에서 제거 */

	nvme_complete_request(req->cb_fn, req->cb_arg, qpair, req, rsp);	/* [한국어] 상위 NVMe 레이어 완료 처리 — 애플리케이션 cb_fn 호출 */
	nvme_rdma_req_put(rqpair, rdma_req);	/* [한국어] free 풀로 반납 → 다음 요청에 재사용 */
}

/*
 * [한국어]
 * nvme_rdma_cm_event_str_get - CM 이벤트 enum을 사람이 읽을 수 있는 문자열로 변환.
 *
 * @event: enum rdma_cm_event_type 값 (RDMA_CM_EVENT_*).
 * @return: 디버그용 문자열 또는 범위 초과 시 "Undefined".
 *
 * librdmacm 헤더의 enum과 rdma_cm_event_str[] 배열 인덱스가 1:1 대응되어 있음을 가정.
 * 디버그 로그(EXPECTED vs RECEIVED) 출력에만 사용 → hot path 아님.
 */
static const char *
nvme_rdma_cm_event_str_get(uint32_t event)
{
	if (event < SPDK_COUNTOF(rdma_cm_event_str)) {	/* [한국어] 배열 범위 검증 */
		return rdma_cm_event_str[event];	/* [한국어] 정상 enum → 매칭 문자열 반환 */
	} else {
		return "Undefined";	/* [한국어] 미정의 이벤트 (librdmacm 신규 추가 등) */
	}
}


/*
 * [한국어]
 * nvme_rdma_qpair_process_cm_event - 큐페어가 보유한 CM 이벤트(rqpair->evt) 1개를 디스패치 처리.
 *
 * @rqpair: 처리 대상 큐페어 (evt != NULL이어야 의미 있음).
 * @return: 0=성공, -1=비정상 (accept_data 누락 등) — 호출자가 connect 실패 처리.
 *
 * 동기/배경:
 *   librdmacm은 CM 핸드셰이크의 모든 비동기 알림(ADDR/ROUTE/ESTABLISHED/REJECTED/DISCONNECTED 등)을
 *   rdma_cm_event 객체로 전달. 이 함수는 한 이벤트에 대해 RDMA 트랜스포트 상태를 갱신하고 ack한다.
 *   주의: validate_cm_event는 별도 함수에서 수행 — 여기는 "기대 이벤트가 아니어도" 받아둔 이벤트의
 *   정리(상태 갱신 + ack) 책임만 가짐.
 *
 * 동작:
 *   1) 이벤트 타입별로 rqpair 상태 갱신 (ESTABLISHED → connected=true, DISCONNECTED → flush 시작 등).
 *   2) 처리 후 rdma_ack_cm_event로 librdmacm에 슬롯 반환 (ack 안 하면 다음 이벤트 안 옴).
 *   3) rqpair->evt = NULL로 클리어.
 *
 * 실행 컨텍스트: process_event_poll, qpair_process_completions 등 reactor 스레드.
 * ctrlr_lock 보호하에 호출되는 경우가 많음 (poll_events 주석 참조).
 *
 * 호출 체인:
 *   nvme_rdma_process_event_poll → [nvme_rdma_qpair_process_cm_event] → rdma_ack_cm_event
 *   nvme_rdma_qpair_process_completions → [nvme_rdma_qpair_process_cm_event]
 *   poll_group_process_completions → [nvme_rdma_qpair_process_cm_event]
 */
static int
nvme_rdma_qpair_process_cm_event(struct nvme_rdma_qpair *rqpair)
{
	struct rdma_cm_event				*event = rqpair->evt;	/* [한국어] poll_events가 보관해 둔 이벤트 */
	struct spdk_nvmf_rdma_accept_private_data	*accept_data;	/* [한국어] CONNECT 응답에 첨부된 NVMe-oF private data (서버의 crqsize 등) */
	int						rc = 0;	/* [한국어] 반환값 — 비정상 이벤트만 -1 */

	if (event) {
		switch (event->event) {
		case RDMA_CM_EVENT_ADDR_RESOLVED:	/* [한국어] rdma_resolve_addr 성공 (ARP/근거리 해상도 완료) */
		case RDMA_CM_EVENT_ADDR_ERROR:		/* [한국어] 주소 해상도 실패 — validate_cm_event가 별도 처리 */
		case RDMA_CM_EVENT_ROUTE_RESOLVED:	/* [한국어] rdma_resolve_route 성공 (라우팅 결정) */
		case RDMA_CM_EVENT_ROUTE_ERROR:		/* [한국어] 라우팅 실패 */
			break;	/* [한국어] 상태머신은 process_event_poll의 콜백에서 진행 — 여기서는 ack만 */
		case RDMA_CM_EVENT_CONNECT_REQUEST:
			break;	/* [한국어] 호스트(initiator) 코드는 listening 안 함 → 무시 */
		case RDMA_CM_EVENT_CONNECT_ERROR:
			break;	/* [한국어] 연결 협상 단계 오류 — 콜백에서 처리 */
		case RDMA_CM_EVENT_UNREACHABLE:		/* [한국어] 타깃 도달 불가 (라우팅 누락 등) */
		case RDMA_CM_EVENT_REJECTED:		/* [한국어] 타깃이 명시적 거부 (인증/리소스 부족/stale) */
			break;
		case RDMA_CM_EVENT_CONNECT_RESPONSE:	/* [한국어] (rdma_create_qp 미사용 시) 핸드셰이크 응답 — RTR 전 단계 */
			rc = spdk_rdma_provider_qp_complete_connect(rqpair->rdma_qp);	/* [한국어] QP를 RTR/RTS로 전이 (provider별 추상화) */
		/* fall through */
		case RDMA_CM_EVENT_ESTABLISHED:		/* [한국어] RDMA 연결 완전 수립 (RTS/RTR 양쪽 도달) */
			rqpair->connected = true;	/* [한국어] 이후 disconnect 경로는 rdma_disconnect를 발사하도록 마킹 */
			accept_data = (struct spdk_nvmf_rdma_accept_private_data *)event->param.conn.private_data;	/* [한국어] NVMe-oF 스펙: ESTABLISHED와 함께 타깃이 hsqsize/crqsize 회신 */
			if (accept_data == NULL) {
				rc = -1;	/* [한국어] private_data 누락 — 비표준 응답으로 간주 */
			} else {
				NVME_RQPAIR_DEBUGLOG(rqpair, "Requested queue depth %d. Target receive queue depth %d.\n",
						     rqpair->num_entries + 1, accept_data->crqsize);	/* [한국어] 호스트 요청 vs 타깃 허용 큐 깊이 비교 */
			}
			break;
		case RDMA_CM_EVENT_DISCONNECTED:
			/* Disconnect qp, which will be moved to error state, so all outstanding
			 * work requests in the send qp will be flushed, otherwise, the outstanding
			 * wrs may not be completed forever.
			 */
			/* [한국어] 타깃 또는 네트워크 측에서 단절 — QP를 ERR 상태로 옮겨야 in-flight WR이 IBV_WC_WR_FLUSH_ERR로 회수됨.
			 * QP를 ERR로 전이 안 하면 응답 영원히 안 와 LINGERING 상태에서 무한 대기. */
			spdk_rdma_provider_qp_disconnect(rqpair->rdma_qp);	/* [한국어] ibv_modify_qp(IBV_QPS_ERR) — flush 트리거 */
			rqpair->connected = false;	/* [한국어] 추가 disconnect 시도 차단 */
			rqpair->qpair.transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_REMOTE;	/* [한국어] 상위 NVMe 레이어에 원격 장애 통지 */
			break;
		case RDMA_CM_EVENT_DEVICE_REMOVAL:	/* [한국어] HCA가 hot-unplug — 로컬 자원 강제 정리 필요 */
			rqpair->qpair.transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
			rqpair->need_destroy = true;	/* [한국어] disconnect 경로에서 일반 검증 우회 (verbs 호출 자체가 EFAULT 가능) */
			break;
		case RDMA_CM_EVENT_MULTICAST_JOIN:
		case RDMA_CM_EVENT_MULTICAST_ERROR:
			break;	/* [한국어] NVMe-oF는 멀티캐스트 사용 안 함 — 도달 시 무시 */
		case RDMA_CM_EVENT_ADDR_CHANGE:
			rqpair->qpair.transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;	/* [한국어] 로컬 IP 변경 — 재연결 필요 */
			break;
		case RDMA_CM_EVENT_TIMEWAIT_EXIT:
			break;	/* [한국어] disconnect 후 잔여 패킷 정리 완료 — 관찰만 */
		default:
			NVME_RQPAIR_ERRLOG(rqpair, "Unexpected Acceptor Event [%d]\n", event->event);	/* [한국어] librdmacm 신규 추가/미정의 — 로그만 */
			break;
		}
		rqpair->evt = NULL;	/* [한국어] 슬롯 비우기 — 다음 이벤트 받을 준비 */
		rdma_ack_cm_event(event);	/* [한국어] librdmacm에 ack — 안 하면 같은 채널에서 다음 이벤트 못 받음 */
	}

	return rc;
}

/*
 * This function must be called under the nvme controller's lock
 * because it touches global controller variables. The lock is taken
 * by the generic transport code before invoking a few of the functions
 * in this file: nvme_rdma_ctrlr_connect_qpair, nvme_rdma_ctrlr_delete_io_qpair,
 * and conditionally nvme_rdma_qpair_process_completions when it is calling
 * completions on the admin qpair. When adding a new call to this function, please
 * verify that it is in a situation where it falls under the lock.
 */
/*
 * [한국어]
 * nvme_rdma_poll_events - cm_channel에서 가능한 모든 CM 이벤트를 nonblock으로 수확.
 *
 * @rctrlr: 컨트롤러 (cm_channel + pending/free 큐 보유).
 * @return: 0=다음 이벤트 EAGAIN까지 모두 처리 (정상), -ENOMEM=free 슬롯 고갈, -EAGAIN/-EWOULDBLOCK=대기 중.
 *
 * 동기/배경:
 *   librdmacm은 컨트롤러당 1개의 cm_channel을 사용 → 모든 큐페어의 CM 이벤트가 한 fd에 섞여 도착.
 *   각 큐페어는 한 번에 1개의 evt만 보유 가능 (rdma_ack_cm_event를 부르려면 ack 시점까지 객체 유효).
 *   이 함수가 fd를 소진(drain)해 각 이벤트를 해당 큐페어의 evt 슬롯 또는 pending 큐에 분배.
 *
 * 동작:
 *   1) 기존 pending 이벤트 중, 해당 큐페어의 evt 슬롯이 비었으면 옮겨 담음.
 *   2) rdma_get_cm_event 루프 (nonblock fd) — 이벤트 수신 시 동일 분배 로직.
 *   3) 큐페어 evt가 점유 중이면 free_cm_events 슬롯을 빌려 pending에 보관.
 *   4) free 풀 고갈 시 ack로 즉시 반납하고 -ENOMEM (정보 손실 가능 — 큐 깊이 NVME_RDMA_NUM_CM_EVENTS=256 권장).
 *
 * 동기화: 함수 상단 영문 주석대로 ctrlr_lock 보호하에 호출. 이유: pending/free 큐 + 모든 큐페어의 evt
 * 슬롯이 컨트롤러 단위 공유 자원이므로 멀티 스레드 진입 방지.
 *
 * 호출 체인:
 *   nvme_rdma_process_event_poll → [nvme_rdma_poll_events] → rdma_get_cm_event
 *   nvme_rdma_ctrlr_process_transport_events → [nvme_rdma_poll_events]
 */
static int
nvme_rdma_poll_events(struct nvme_rdma_ctrlr *rctrlr)
{
	struct nvme_rdma_cm_event_entry	*entry, *tmp;	/* [한국어] STAILQ 순회 임시 변수 */
	struct nvme_rdma_qpair		*event_qpair;	/* [한국어] cm_id->context로 복원한 이벤트 대상 큐페어 */
	struct rdma_cm_event		*event;		/* [한국어] librdmacm이 내려준 이벤트 객체 */
	struct rdma_event_channel	*channel = rctrlr->cm_channel;	/* [한국어] 폴링 대상 fd 채널 */

	STAILQ_FOREACH_SAFE(entry, &rctrlr->pending_cm_events, link, tmp) {	/* [한국어] (1) pending 큐 우선 비우기 — 큐페어 evt가 비었는지 재확인 */
		event_qpair = entry->evt->id->context;	/* [한국어] cm_id->context = rqpair (rdma_create_id에서 등록) */
		if (event_qpair->evt == NULL) {	/* [한국어] 슬롯 비었다 → 즉시 이동 */
			event_qpair->evt = entry->evt;	/* [한국어] 큐페어 evt에 직접 보관 */
			STAILQ_REMOVE(&rctrlr->pending_cm_events, entry, nvme_rdma_cm_event_entry, link);
			STAILQ_INSERT_HEAD(&rctrlr->free_cm_events, entry, link);	/* [한국어] entry 슬롯은 다시 free 풀로 */
		}
	}

	while (rdma_get_cm_event(channel, &event) == 0) {	/* [한국어] (2) fd nonblock drain — EAGAIN까지 반복 */
		event_qpair = event->id->context;	/* [한국어] cm_id->context로 어느 rqpair용인지 식별 */
		if (event_qpair->evt == NULL) {
			event_qpair->evt = event;	/* [한국어] 슬롯 비어있으면 직접 보관 (가장 빠른 경로) */
		} else {
			assert(rctrlr == nvme_rdma_ctrlr(event_qpair->qpair.ctrlr));	/* [한국어] 같은 컨트롤러 소속이어야 — 디버그 가드 */
			entry = STAILQ_FIRST(&rctrlr->free_cm_events);	/* [한국어] 슬롯 가져오기 */
			if (entry == NULL) {
				rdma_ack_cm_event(event);	/* [한국어] free 고갈 → 이벤트 손실 회피 위해 즉시 ack */
				return -ENOMEM;	/* [한국어] 호출자에 메모리 압박 통지 */
			}
			STAILQ_REMOVE_HEAD(&rctrlr->free_cm_events, link);
			entry->evt = event;
			STAILQ_INSERT_TAIL(&rctrlr->pending_cm_events, entry, link);	/* [한국어] FIFO로 pending에 보관 — 큐페어가 자기 evt 비우면 1 단계가 가져감 */
		}
	}

	/* rdma_get_cm_event() returns -1 on error. If an error occurs, errno
	 * will be set to indicate the failure reason. So return negated errno here.
	 */
	/* [한국어] nonblock fd가 비면 errno=EAGAIN/EWOULDBLOCK → 정상 종료. 다른 errno는 진짜 오류. */
	return -errno;
}

/*
 * [한국어]
 * nvme_rdma_validate_cm_event - 받은 CM 이벤트가 기대하던 종류인지 검사.
 *
 * @expected_evt_type: 큐페어 상태머신이 기대하는 다음 이벤트 (예: ESTABLISHED).
 * @reaped_evt: 실제 수신 이벤트.
 * @return: 0=일치, -ESTALE=stale connection 거부, -EBADMSG=다른 이벤트.
 *
 * 특수 처리:
 *   - REJECTED + status=10(IB_CM_REJ_STALE_CONN): 타깃이 이전 세션 컨텍스트를 가지고 있어 거부 → 재시도 가능 신호.
 *   - CONNECT_RESPONSE를 ESTABLISHED 대신 받음: rdma_create_qp 미사용 시 정상 (provider 차이) → 0 반환.
 *
 * stale conn은 NVME_RDMA_STALE_CONN_RETRY_MAX회 자동 재시도.
 */
static int
nvme_rdma_validate_cm_event(enum rdma_cm_event_type expected_evt_type,
			    struct rdma_cm_event *reaped_evt)
{
	int rc = -EBADMSG;	/* [한국어] 기본은 형식 오류로 간주, 특수 케이스는 아래에서 갈음 */

	if (expected_evt_type == reaped_evt->event) {
		return 0;	/* [한국어] 정상 일치 */
	}

	switch (expected_evt_type) {
	case RDMA_CM_EVENT_ESTABLISHED:
		/*
		 * There is an enum ib_cm_rej_reason in the kernel headers that sets 10 as
		 * IB_CM_REJ_STALE_CONN. I can't find the corresponding userspace but we get
		 * the same values here.
		 */
		/* [한국어] InfiniBand 스펙(IB CM Spec) 12.6.7.2: REJ reason=10 = STALE_CONN.
		 * 타깃이 이전 세션 정보 보존 중 → 잠시 후 재시도하면 성공. */
		if (reaped_evt->event == RDMA_CM_EVENT_REJECTED && reaped_evt->status == 10) {
			rc = -ESTALE;	/* [한국어] stale conn 시그널 — 호출자가 retry 분기 */
		} else if (reaped_evt->event == RDMA_CM_EVENT_CONNECT_RESPONSE) {
			/*
			 *  If we are using a qpair which is not created using rdma cm API
			 *  then we will receive RDMA_CM_EVENT_CONNECT_RESPONSE instead of
			 *  RDMA_CM_EVENT_ESTABLISHED.
			 */
			/* [한국어] Mellanox direct verbs 등에서 QP를 직접 생성할 때 ESTABLISHED 대신 CONNECT_RESPONSE 도달.
			 * 같은 의미로 처리 (provider 차이 흡수). */
			return 0;
		}
		break;
	default:
		break;
	}

	SPDK_ERRLOG("Expected %s but received %s (%d) from CM event channel (status = %d)\n",
		    nvme_rdma_cm_event_str_get(expected_evt_type),
		    nvme_rdma_cm_event_str_get(reaped_evt->event), reaped_evt->event,
		    reaped_evt->status);	/* [한국어] 기대 vs 실제 출력 — 디버깅에 가장 중요한 로그 */
	return rc;
}

/*
 * [한국어]
 * nvme_rdma_process_event_start - 큐페어가 다음 단계로 진행하기 전 "기대 이벤트와 콜백" 등록.
 *
 * @rqpair: 진행 중인 큐페어.
 * @evt: 이번 단계에서 기대하는 이벤트 종류.
 * @evt_cb: 이벤트 도착(또는 검증 결과) 시 호출할 콜백 (체인의 다음 단계).
 * @return: 0=등록 완료, 그 외=기존 evt 처리 중 오류.
 *
 * 핸드셰이크 단계마다 호출되어 비동기 진행을 상태머신에 기록.
 * evt_timeout_ticks를 함께 설정해 영원히 대기하지 않도록 데드라인 설정.
 *
 * 호출 체인:
 *   nvme_rdma_resolve_addr → process_event_start(ADDR_RESOLVED, addr_resolved)
 *   nvme_rdma_addr_resolved → process_event_start(ROUTE_RESOLVED, route_resolved)
 *   nvme_rdma_connect → process_event_start(ESTABLISHED, connect_established)
 *   _nvme_rdma_ctrlr_disconnect_qpair → process_event_start(DISCONNECTED, ...)
 */
static int
nvme_rdma_process_event_start(struct nvme_rdma_qpair *rqpair,
			      enum rdma_cm_event_type evt,
			      nvme_rdma_cm_event_cb evt_cb)
{
	int	rc;

	assert(evt_cb != NULL);	/* [한국어] 콜백 누락 시 진행 불가 — 호출자 버그 */

	if (rqpair->evt != NULL) {	/* [한국어] 이전 단계의 이벤트가 아직 미처리면 먼저 처리 (큐 정리) */
		rc = nvme_rdma_qpair_process_cm_event(rqpair);
		if (rc) {
			return rc;	/* [한국어] 이전 단계가 비정상이었음 — 호출자에 전파 */
		}
	}

	rqpair->expected_evt_type = evt;	/* [한국어] 다음 검증용 기대값 */
	rqpair->evt_cb = evt_cb;		/* [한국어] poll에서 호출할 콜백 */
	rqpair->evt_timeout_ticks = (g_spdk_nvme_transport_opts.rdma_cm_event_timeout_ms * 1000 *
				     spdk_get_ticks_hz()) / SPDK_SEC_TO_USEC + spdk_get_ticks();	/* [한국어] 데드라인 = 현재 + 옵션ms */

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_process_event_poll - 등록된 기대 이벤트가 도착했는지 한 번 확인하고 콜백 실행.
 *
 * @rqpair: 큐페어.
 * @return: evt_cb가 반환한 값. 일반적으로 -EAGAIN(아직 미도착) 또는 0(다음 단계로) 또는 음수(실패).
 *
 * 동작:
 *   1) 큐페어 evt 슬롯 비었고 데드라인 내면 poll_events 호출 (cm_channel drain).
 *   2) 슬롯에 이벤트 있으면 validate + process로 상태 갱신.
 *   3) 마지막에 evt_cb 실행 — 콜백이 다음 단계 진행 또는 실패 처리.
 *
 * 호출 체인:
 *   nvme_rdma_ctrlr_connect_qpair_poll(INITIALIZING) → [nvme_rdma_process_event_poll] → evt_cb
 *   nvme_rdma_ctrlr_disconnect_qpair_poll(EXITING) → [nvme_rdma_process_event_poll] → evt_cb
 */
static int
nvme_rdma_process_event_poll(struct nvme_rdma_qpair *rqpair)
{
	struct nvme_rdma_ctrlr	*rctrlr;	/* [한국어] poll_events에 넘길 컨트롤러 */
	int	rc = 0, rc2;		/* [한국어] 두 단계 결과 — validate 결과 우선 채택 */

	rctrlr = nvme_rdma_ctrlr(rqpair->qpair.ctrlr);
	assert(rctrlr != NULL);

	if (!rqpair->evt && spdk_get_ticks() < rqpair->evt_timeout_ticks) {	/* [한국어] 슬롯 비었고 데드라인 전 → fd drain */
		rc = nvme_rdma_poll_events(rctrlr);
		if (rc == -EAGAIN || rc == -EWOULDBLOCK) {
			return rc;	/* [한국어] 정상 (아직 이벤트 없음) — 다음 폴 사이클에서 재시도 */
		}
	}

	if (rqpair->evt == NULL) {
		rc = -EADDRNOTAVAIL;	/* [한국어] 데드라인 만료 또는 다른 큐페어 evt가 점유 중 → 진행 불가 */
		goto exit;
	}

	rc = nvme_rdma_validate_cm_event(rqpair->expected_evt_type, rqpair->evt);	/* [한국어] 기대값과 비교 — stale은 -ESTALE */

	rc2 = nvme_rdma_qpair_process_cm_event(rqpair);	/* [한국어] 상태 갱신 + ack */
	/* bad message takes precedence over the other error codes from processing the event. */
	rc = rc == 0 ? rc2 : rc;	/* [한국어] validate 실패가 process 결과보다 우선 (실패 원인 보존) */

exit:
	assert(rqpair->evt_cb != NULL);
	return rqpair->evt_cb(rqpair, rc);	/* [한국어] 콜백에 결과 전달 — 다음 단계 진행 또는 실패 처리 */
}

/*
 * [한국어]
 * nvme_rdma_resize_cq - poll group 공유 CQ를 새 큐페어 요구에 맞게 동적 확장.
 *
 * @rqpair: 새로 추가될 큐페어.
 * @poller: 공유 CQ를 보유한 poller.
 * @return: 0=성공, -1=ibv_resize_cq 실패.
 *
 * 동기/배경:
 *   poll group 모드에서 같은 device의 큐페어들이 1개의 CQ를 공유 → 큐페어가 추가될 때마다 CQ 슬롯 부족 가능.
 *   각 큐페어는 WC_PER_QPAIR(N) = 2N (SEND+RECV) 슬롯 필요. ibv_resize_cq로 무중단 확장.
 *   double 정책 + max 캡 — 너무 자주 resize 호출 회피.
 *
 * 동작:
 *   1) 신규 합산 required_num_wc 계산 (기존 + 추가).
 *   2) current가 부족하면 max(current*2, required)로 키움.
 *   3) 옵션상 max_cq_size 제한 있으면 그 이상 안 가도록 클램프.
 *   4) current와 다르면 ibv_resize_cq 호출 (verbs API).
 *
 * 호출 체인: nvme_rdma_qpair_set_poller → [nvme_rdma_resize_cq] → ibv_resize_cq
 */
static int
nvme_rdma_resize_cq(struct nvme_rdma_qpair *rqpair, struct nvme_rdma_poller *poller)
{
	int	current_num_wc, required_num_wc;	/* [한국어] 기존/필요 슬롯 수 */
	int	max_cq_size;	/* [한국어] 옵션상 상한 */

	required_num_wc = poller->required_num_wc + WC_PER_QPAIR(rqpair->num_entries);	/* [한국어] 새 합산 = 기존 + 2*N */
	current_num_wc = poller->current_num_wc;
	if (current_num_wc < required_num_wc) {	/* [한국어] 부족 시 확장 정책 */
		current_num_wc = spdk_max(current_num_wc * 2, required_num_wc);	/* [한국어] doubling으로 amortized O(1) */
	}

	max_cq_size = g_spdk_nvme_transport_opts.rdma_max_cq_size;
	if (max_cq_size != 0 && current_num_wc > max_cq_size) {
		current_num_wc = max_cq_size;	/* [한국어] HCA 한계 또는 운영자 정책으로 상한 적용 */
	}

	if (poller->current_num_wc != current_num_wc) {	/* [한국어] 변경 필요 시에만 verbs 호출 */
		NVME_RQPAIR_DEBUGLOG(rqpair, "Resize RDMA CQ from %d to %d\n", poller->current_num_wc,
				     current_num_wc);
		if (ibv_resize_cq(poller->cq, current_num_wc)) {	/* [한국어] verbs API: 기존 in-flight WC 보존하며 슬롯 확장. EBUSY/ENOMEM 가능 */
			NVME_RQPAIR_ERRLOG(rqpair, "RDMA CQ resize failed: errno %d: %s\n", errno, spdk_strerror(errno));
			return -1;
		}

		poller->current_num_wc = current_num_wc;	/* [한국어] 성공 시 캐시값 갱신 */
	}

	poller->required_num_wc = required_num_wc;	/* [한국어] 누적치 갱신 */
	return 0;
}

/*
 * [한국어]
 * nvme_rdma_qpair_set_poller - 큐페어를 poll group 내 적절한 poller에 attach.
 *
 * @qpair: 일반 큐페어 (poll_group 설정되어 있어야 함).
 * @return: 0=성공, -EINVAL=poller 생성 실패, -EPROTO=CQ resize 실패.
 *
 * 동기/배경:
 *   poll group은 같은 RDMA device(=ibv_context)의 큐페어들이 CQ/SRQ/PD를 공유하도록 1 device당 1 poller를 둠.
 *   이 함수는 큐페어가 어느 device에 속하는지(rqpair->cm_id->verbs) 확인 후, 일치하는 poller를 찾거나 새로 만들어
 *   큐페어에 cq/srq/rsps/poller 포인터를 채워 넣는다.
 *
 * 호출 시점: nvme_rdma_qpair_init 내부 — ibv_create_qp 직전.
 */
static int
nvme_rdma_qpair_set_poller(struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair          *rqpair = nvme_rdma_qpair(qpair);	/* [한국어] RDMA 컨테이너 */
	struct nvme_rdma_poll_group     *group = nvme_rdma_poll_group(qpair->poll_group);	/* [한국어] 부모 poll group */
	struct nvme_rdma_poller         *poller;

	assert(rqpair->cq == NULL);	/* [한국어] 이미 cq가 있으면 누수 — 두 번 호출 가드 */

	poller = nvme_rdma_poll_group_get_poller(group, rqpair->cm_id->verbs);	/* [한국어] device 일치 poller 검색 또는 신규 생성 */
	if (!poller) {
		NVME_RQPAIR_ERRLOG(rqpair, "Unable to find a cq on poll group %p\n", qpair->poll_group);
		return -EINVAL;
	}

	if (!poller->srq) {	/* [한국어] SRQ 미사용(=큐페어별 RQ) 모드에서만 CQ 슬롯 동적 확장 필요 */
		if (nvme_rdma_resize_cq(rqpair, poller)) {
			nvme_rdma_poll_group_put_poller(group, poller);	/* [한국어] 실패 롤백 — refcnt-- */
			return -EPROTO;
		}
	}

	rqpair->cq = poller->cq;	/* [한국어] 공유 CQ 포인터 보관 */
	rqpair->srq = poller->srq;	/* [한국어] SRQ 모드면 같은 SRQ 공유, 아니면 NULL */
	if (rqpair->srq) {
		rqpair->rsps = poller->rsps;	/* [한국어] SRQ 공유 응답 풀도 공유 — 큐페어가 자기 풀 만들 필요 없음 */
	}
	rqpair->poller = poller;	/* [한국어] release 시 ref-- 위해 역참조 보관 */
	return 0;
}

/*
 * [한국어]
 * nvme_rdma_qpair_release_poller - 큐페어 해제 시 poller refcnt 감소 및 슬롯 회수.
 *
 * @rqpair: 해제 대상 큐페어.
 *
 * 동작:
 *   1) SRQ 미사용 시 required_num_wc에서 자기 몫 차감 (다음 resize 판단 기준).
 *   2) put_poller 호출 — refcnt 0이면 poller 자체도 destroy.
 *   3) 큐페어 측 cq/poller 포인터 NULL로 초기화.
 *
 * 주의: ibv_resize_cq를 작은 값으로 부르지는 않음 (HCA가 기존 in-flight WC 누락 위험).
 */
static void
nvme_rdma_qpair_release_poller(struct nvme_rdma_qpair *rqpair)
{
	struct nvme_rdma_poller *poller = rqpair->poller;	/* [한국어] 소속 poller */
	struct nvme_rdma_poll_group *group = poller->group;	/* [한국어] 부모 group */

	assert(group);

	if (!poller->srq) {	/* [한국어] SRQ 모드는 CQ 슬롯 추적 안 함 */
		assert(rqpair->poller->required_num_wc >= WC_PER_QPAIR(rqpair->num_entries));	/* [한국어] underflow 가드 */
		poller->required_num_wc -= WC_PER_QPAIR(rqpair->num_entries);	/* [한국어] 자기 몫 차감 */
	}

	nvme_rdma_poll_group_put_poller(group, poller);	/* [한국어] refcnt-- (0이면 destroy) */
	rqpair->poller = NULL;	/* [한국어] 댕글링 방지 */
	rqpair->cq = NULL;
}

/*
 * [한국어]
 * nvme_rdma_qpair_init - RDMA 연결 핸드셰이크 중 ROUTE_RESOLVED 직후 QP/CQ를 실제로 생성.
 *
 * @rqpair: 큐페어 (cm_id는 이미 resolve_route 완료 상태).
 * @return: 0=성공, -1=device 조회/CQ/QP 생성 실패.
 *
 * 동기/배경:
 *   librdmacm은 rdma_resolve_addr → rdma_resolve_route까지만 처리하고, QP/CQ는 호출자가 ibverbs로 직접 만들어야 함.
 *   여기서 만든 QP가 rdma_connect의 입력이 되어 RDMA 핸드셰이크에서 PSN/QPN을 교환.
 *
 * 동작:
 *   1) ibv_query_device로 HCA 능력(max_sge 등) 조회.
 *   2) poll group 모드: 공유 poller의 CQ를 사용 (set_poller). 단독 모드: ibv_create_cq로 전용 CQ 생성.
 *   3) PD: 사용자 후크가 있으면 우선 사용 (GPU Direct RDMA 등), 없으면 SPDK 자동 할당.
 *   4) QP 속성 채움 — send_wr 깊이 = num_entries, recv_wr는 SRQ 사용 여부에 따라 분기.
 *   5) spdk_rdma_provider_qp_create로 QP 생성 (내부에서 ibv_create_qp 또는 Mellanox direct verbs).
 *   6) 실제 max_send_sge는 ibv_create_qp가 줄였을 수 있으므로 다시 읽어 캐시.
 *
 * 호출 체인:
 *   nvme_rdma_route_resolved → [nvme_rdma_qpair_init] → ibv_create_cq + spdk_rdma_provider_qp_create → ibv_create_qp
 */
static int
nvme_rdma_qpair_init(struct nvme_rdma_qpair *rqpair)
{
	int			rc;
	struct spdk_rdma_provider_qp_init_attr	attr = {};	/* [한국어] QP 생성 속성 — provider 추상화 (ibv_qp_init_attr 슈퍼셋) */
	struct ibv_device_attr	dev_attr;	/* [한국어] HCA 능력 */
	struct nvme_rdma_ctrlr	*rctrlr;	/* [한국어] PD 후크 호출에 trid 필요 */
	uint32_t num_cqe, max_num_cqe;	/* [한국어] CQ 슬롯 수 */

	rc = ibv_query_device(rqpair->cm_id->verbs, &dev_attr);	/* [한국어] verbs API: HCA 속성 (max_qp_wr/max_sge/max_cqe 등) 조회 */
	if (rc != 0) {
		NVME_RQPAIR_ERRLOG(rqpair, "Failed to query RDMA device attributes.\n");
		return -1;
	}

	if (rqpair->qpair.poll_group) {	/* [한국어] poll group 모드: 공유 CQ 사용 */
		assert(!rqpair->cq);
		rc = nvme_rdma_qpair_set_poller(&rqpair->qpair);	/* [한국어] device 일치 poller에 attach */
		if (rc) {
			NVME_RQPAIR_ERRLOG(rqpair, "Unable to activate the rdmaqpair.\n");
			return -1;
		}
		assert(rqpair->cq);
	} else {
		num_cqe = rqpair->num_entries * 2;	/* [한국어] SEND+RECV 각각 num_entries → 총 2N 슬롯 */
		max_num_cqe = g_spdk_nvme_transport_opts.rdma_max_cq_size;
		if (max_num_cqe != 0 && num_cqe > max_num_cqe) {
			num_cqe = max_num_cqe;	/* [한국어] 옵션 상한 적용 */
		}
		rqpair->cq = ibv_create_cq(rqpair->cm_id->verbs, num_cqe, rqpair, NULL, 0);	/* [한국어] verbs API: 전용 CQ 생성. cq_context=rqpair, channel=NULL(이벤트 알림 사용 안 함, busy-poll). */
		if (!rqpair->cq) {
			NVME_RQPAIR_ERRLOG(rqpair, "Unable to create completion queue: errno %d: %s\n", errno,
					   spdk_strerror(errno));
			return -1;
		}
	}

	rctrlr = nvme_rdma_ctrlr(rqpair->qpair.ctrlr);
	if (g_nvme_hooks.get_ibv_pd) {	/* [한국어] 사용자 후크 우선 — GPU Direct/외부 PD 등 외부 메모리 접근 권한 */
		attr.pd = g_nvme_hooks.get_ibv_pd(&rctrlr->ctrlr.trid, rqpair->cm_id->verbs);
	} else {
		attr.pd = spdk_rdma_utils_get_pd(rqpair->cm_id->verbs);	/* [한국어] device당 캐시된 PD 가져오기 (재사용으로 PD 폭발 방지) */
	}

	attr.stats		= rqpair->poller ? &rqpair->poller->stats.rdma_stats : NULL;	/* [한국어] poll group 모드는 통계 공유, 단독은 NULL */
	attr.send_cq		= rqpair->cq;	/* [한국어] SEND 완료 → 이 CQ로 */
	attr.recv_cq		= rqpair->cq;	/* [한국어] RECV 완료 → 같은 CQ (송수신 공유) */
	attr.cap.max_send_wr	= rqpair->num_entries; /* SEND operations */	/* [한국어] SQ 깊이 = N */
	if (rqpair->srq) {
		attr.srq	= rqpair->srq->srq;	/* [한국어] SRQ 모드: 큐페어별 RQ 대신 공유 SRQ 사용 → max_recv_wr 무시됨 */
	} else {
		attr.cap.max_recv_wr = rqpair->num_entries; /* RECV operations */	/* [한국어] 큐페어 전용 RQ 깊이 = N */
	}
	attr.cap.max_send_sge	= spdk_min(NVME_RDMA_DEFAULT_TX_SGE, dev_attr.max_sge);	/* [한국어] SEND SGE 최대 2 (Cmd+inline data) */
	attr.cap.max_recv_sge	= spdk_min(NVME_RDMA_DEFAULT_RX_SGE, dev_attr.max_sge);	/* [한국어] RECV SGE 최대 1 (CPL 16B) */
	attr.domain_transfer	= spdk_rdma_provider_accel_sequence_supported() ?
				  nvme_rdma_memory_domain_transfer_data : NULL;	/* [한국어] UMR/accel 시퀀스 지원 시 데이터 변환 콜백 등록 */

	rqpair->rdma_qp = spdk_rdma_provider_qp_create(rqpair->cm_id, &attr);	/* [한국어] 핵심: QP 생성 — 내부에서 ibv_create_qp + INIT 상태로 modify */

	if (!rqpair->rdma_qp) {
		return -1;	/* [한국어] 자원 부족 등 실패 — 호출자가 cleanup */
	}

	/* ibv_create_qp will change the values in attr.cap. Make sure we store the proper value. */
	/* [한국어] HCA가 요청보다 작은 SGE를 강제할 수 있음 — 실제 가능값을 다시 저장해 hot path가 참조 */
	rqpair->max_send_sge = spdk_min(NVME_RDMA_DEFAULT_TX_SGE, attr.cap.max_send_sge);
	rqpair->current_num_sends = 0;	/* [한국어] in-flight SEND 카운터 초기화 */

	rqpair->cm_id->context = rqpair;	/* [한국어] CM 이벤트가 도착하면 cm_id->context로 큐페어 역참조 (poll_events에서 사용) */

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_reset_failed_sends - ibv_post_send 실패 시 게시 못 한 WR들의 in-flight 카운터 롤백.
 *
 * @rqpair: 큐페어.
 * @bad_send_wr: ibv_post_send가 실패한 지점부터의 WR 링크드 리스트.
 *
 * 배경: ibv_post_send는 한 번에 여러 WR을 chain으로 받으며, 첫 실패 지점의 WR을 bad_wr 인자로 알려준다.
 *       앞쪽 WR은 SQ에 들어갔고 뒤쪽은 들어가지 못함 → 미게시 분만 카운터 차감해 SQ 슬롯 추정 일관성 유지.
 */
static void
nvme_rdma_reset_failed_sends(struct nvme_rdma_qpair *rqpair,
			     struct ibv_send_wr *bad_send_wr)
{
	while (bad_send_wr != NULL) {	/* [한국어] 실패 지점부터 끝까지 순회 */
		assert(rqpair->current_num_sends > 0);
		rqpair->current_num_sends--;	/* [한국어] in-flight 카운터 차감 (게시 안 됐으니 완료도 안 옴) */
		bad_send_wr = bad_send_wr->next;
	}
}

/*
 * [한국어]
 * nvme_rdma_reset_failed_recvs - ibv_post_recv 실패한 RECV WR들의 in-flight 카운터 롤백.
 *
 * @rsps: 응답 풀 (큐페어 또는 SRQ 공용).
 * @bad_recv_wr: 실패 지점 WR.
 * @rc: 원인 errno.
 *
 * SEND 버전과 동일 패턴 — 게시 못 한 WR 만큼 current_num_recvs 감소.
 */
static void
nvme_rdma_reset_failed_recvs(struct nvme_rdma_rsps *rsps,
			     struct ibv_recv_wr *bad_recv_wr, int rc)
{
	SPDK_ERRLOG("Failed to post WRs on receive queue, errno %d (%s), bad_wr %p\n",
		    rc, spdk_strerror(rc), bad_recv_wr);
	while (bad_recv_wr != NULL) {
		assert(rsps->current_num_recvs > 0);
		rsps->current_num_recvs--;	/* [한국어] 반납 — 다음 사이클에서 다시 게시 시도 */
		bad_recv_wr = bad_recv_wr->next;
	}
}

/*
 * [한국어]
 * nvme_rdma_qpair_submit_sends - 큐잉된 SEND WR 체인을 ibv_post_send로 doorbell.
 *
 * @rqpair: 큐페어.
 * @return: 0=성공, 음수=ibv_post_send 실패.
 *
 * 동기/배경:
 *   delay_cmd_submit 모드에서 _submit_request는 WR을 큐잉만 하고 doorbell 보류 → 이 함수에서 일괄 발사.
 *   provider 추상은 Mellanox direct verbs(WQE 직접 작성) 또는 표준 ibv_post_send를 사용.
 */
static inline int
nvme_rdma_qpair_submit_sends(struct nvme_rdma_qpair *rqpair)
{
	struct ibv_send_wr *bad_send_wr = NULL;	/* [한국어] 실패 지점 받을 OUT 인자 */
	int rc;

	rc = spdk_rdma_provider_qp_flush_send_wrs(rqpair->rdma_qp, &bad_send_wr);	/* [한국어] 누적 WR 일괄 게시 → 내부적으로 ibv_post_send (또는 direct verbs WQE 작성) */

	if (spdk_unlikely(rc)) {
		NVME_RQPAIR_ERRLOG(rqpair, "Failed to post WRs on send queue, errno %d (%s), bad_wr %p\n",
				   rc, spdk_strerror(rc), bad_send_wr);
		nvme_rdma_reset_failed_sends(rqpair, bad_send_wr);	/* [한국어] in-flight 카운터 롤백 */
	}

	return rc;
}

/*
 * [한국어]
 * nvme_rdma_qpair_submit_recvs - 큐잉된 RECV WR 체인을 ibv_post_recv로 RQ에 게시.
 *
 * @rqpair: 큐페어 (큐페어 전용 RQ 모드, SRQ 모드는 별도 함수).
 *
 * 응답 슬롯 재사용을 위해 process_*_completion 후 매번 호출. RECV WR이 항상 충분히 게시되어 있어야
 * 타깃이 보낸 SEND가 떨어뜨릴 곳 없는 RNR(Receiver Not Ready) 오류 회피.
 */
static inline int
nvme_rdma_qpair_submit_recvs(struct nvme_rdma_qpair *rqpair)
{
	struct ibv_recv_wr *bad_recv_wr;
	int rc = 0;

	rc = spdk_rdma_provider_qp_flush_recv_wrs(rqpair->rdma_qp, &bad_recv_wr);	/* [한국어] 누적 RECV WR 일괄 게시 → ibv_post_recv */
	if (spdk_unlikely(rc)) {
		nvme_rdma_reset_failed_recvs(rqpair->rsps, bad_recv_wr, rc);
	}

	return rc;
}

/*
 * [한국어]
 * nvme_rdma_poller_submit_recvs - SRQ 모드 공유 응답 풀의 RECV WR 일괄 게시.
 *
 * @poller: SRQ 보유 poller.
 *
 * SRQ는 같은 device의 모든 큐페어가 공유 → recv WR이 어느 큐페어에 갈지 미리 알 수 없으나,
 * 응답 capsule 자체에 cid가 있어 process_recv_completion이 wc->qp_num + cpl.cid로 라우팅.
 */
static inline int
nvme_rdma_poller_submit_recvs(struct nvme_rdma_poller *poller)
{
	struct ibv_recv_wr *bad_recv_wr;
	int rc;

	rc = spdk_rdma_provider_srq_flush_recv_wrs(poller->srq, &bad_recv_wr);	/* [한국어] SRQ 전용 게시 함수 → ibv_post_srq_recv */
	if (spdk_unlikely(rc)) {
		nvme_rdma_reset_failed_recvs(poller->rsps, bad_recv_wr, rc);
	}

	return rc;
}

#define nvme_rdma_trace_ibv_sge(sg_list) \
	if (sg_list) { \
		SPDK_DEBUGLOG(nvme, "local addr %p length 0x%x lkey 0x%x\n", \
			      (void *)(sg_list)->addr, (sg_list)->length, (sg_list)->lkey); \
	}

/*
 * [한국어]
 * nvme_rdma_free_rsps - 응답 풀(rsps)의 모든 자원 해제.
 *
 * @rsps: 해제 대상 (NULL 안전).
 *
 * 3개의 SoA 배열(rsps/sgls/recv_wrs)을 spdk_free로 해제 — 모두 hugepage(DMA 가능) 메모리.
 * 호출 시점: 큐페어 destroy, SRQ 모드 poller destroy.
 */
static void
nvme_rdma_free_rsps(struct nvme_rdma_rsps *rsps)
{
	if (!rsps) {
		return;
	}

	spdk_free(rsps->rsps);		/* [한국어] 응답 버퍼 배열 */
	spdk_free(rsps->rsp_sgls);	/* [한국어] SGE 배열 */
	spdk_free(rsps->rsp_recv_wrs);	/* [한국어] RECV WR 배열 */
	free(rsps);			/* [한국어] 컨테이너 자체는 일반 malloc → free */
}

/*
 * [한국어]
 * nvme_rdma_create_rsps - 응답 풀 할당 + RECV WR 사전 큐잉.
 *
 * @opts: 생성 파라미터 묶음 (num_entries / rqpair-or-srq / mr_map).
 * @return: 생성된 nvme_rdma_rsps 포인터 또는 NULL.
 *
 * 동기/배경:
 *   타깃이 응답 Capsule(spdk_nvme_cpl 16B)을 SEND로 보낼 때 호스트는 RECV WR이 미리 게시되어 있어야 받음.
 *   이 함수가 num_entries 만큼의 응답 슬롯을 만들고 RECV WR을 사전 큐잉 → ibv_post_recv는 connect_established에서.
 *
 * 동작:
 *   1) SoA 3개 배열을 hugepage로 zmalloc (DMA 가능 + lkey 등록 가능).
 *   2) 각 슬롯에 대해:
 *      - rdma_wr.type=RECV (CQ 처리 시 분기 키)
 *      - SGE addr = 응답 버퍼 주소, length = sizeof(cpl)
 *      - lkey = spdk_rdma_utils_get_translation으로 MR 등록된 lkey 조회 (vtophys 매핑)
 *      - recv_wr.wr_id = &rsp->rdma_wr (CQ 처리 시 SPDK_CONTAINEROF로 부모 복원)
 *   3) 큐페어/SRQ 모드에 따라 적절한 큐에 enqueue (실제 post_recv는 connect_established에서).
 *
 * 호출 체인:
 *   nvme_rdma_connect_established → [nvme_rdma_create_rsps] → spdk_rdma_provider_qp_queue_recv_wrs
 *   nvme_rdma_poller_create (SRQ 모드) → [nvme_rdma_create_rsps] → spdk_rdma_provider_srq_queue_recv_wrs
 */
static struct nvme_rdma_rsps *
nvme_rdma_create_rsps(struct nvme_rdma_rsp_opts *opts)
{
	struct nvme_rdma_rsps *rsps;
	struct spdk_rdma_utils_memory_translation translation;	/* [한국어] vtophys 변환 결과 */
	uint16_t i;
	int rc;

	rsps = calloc(1, sizeof(*rsps));	/* [한국어] 컨테이너 — 일반 메모리(소형) */
	if (!rsps) {
		SPDK_ERRLOG("Failed to allocate rsps object\n");
		return NULL;
	}

	rsps->rsp_sgls = spdk_zmalloc(opts->num_entries * sizeof(*rsps->rsp_sgls), 0, NULL,
				      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);	/* [한국어] SGE 배열 — DMA 가능 hugepage */
	if (!rsps->rsp_sgls) {
		SPDK_ERRLOG("Failed to allocate rsp_sgls\n");
		goto fail;
	}

	rsps->rsp_recv_wrs = spdk_zmalloc(opts->num_entries * sizeof(*rsps->rsp_recv_wrs), 0, NULL,
					  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);	/* [한국어] RECV WR 배열 */
	if (!rsps->rsp_recv_wrs) {
		SPDK_ERRLOG("Failed to allocate rsp_recv_wrs\n");
		goto fail;
	}

	rsps->rsps = spdk_zmalloc(opts->num_entries * sizeof(*rsps->rsps), 0, NULL,
				  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);	/* [한국어] 응답 버퍼(spdk_nvme_rdma_rsp) 배열 — RDMA로 직접 적재됨 */
	if (!rsps->rsps) {
		SPDK_ERRLOG("can not allocate rdma rsps\n");
		goto fail;
	}

	for (i = 0; i < opts->num_entries; i++) {	/* [한국어] 슬롯별 초기화 + WR 큐잉 */
		struct ibv_sge *rsp_sgl = &rsps->rsp_sgls[i];
		struct spdk_nvme_rdma_rsp *rsp = &rsps->rsps[i];
		struct ibv_recv_wr *recv_wr = &rsps->rsp_recv_wrs[i];

		rsp->rqpair = opts->rqpair;	/* [한국어] 큐페어 모드면 부모, SRQ 모드는 NULL (process_recv가 wc->qp_num으로 검색) */
		rsp->rdma_wr.type = RDMA_WR_TYPE_RECV;	/* [한국어] CQ 처리 시 분기 키 */
		rsp->recv_wr = recv_wr;	/* [한국어] 응답 처리 후 다시 게시할 때 재사용 */
		rsp_sgl->addr = (uint64_t)rsp;	/* [한국어] SGE는 spdk_nvme_rdma_rsp 시작 — cpl 필드가 첫 16B */
		rsp_sgl->length = sizeof(struct spdk_nvme_cpl);	/* [한국어] CPL 16B만 받을 거니 길이 제한 */
		rc = spdk_rdma_utils_get_translation(opts->mr_map, rsp, sizeof(*rsp), &translation);	/* [한국어] vtophys + MR lookup → 등록된 lkey 획득 */
		if (rc) {
			goto fail;
		}
		rsp_sgl->lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);	/* [한국어] HCA가 이 버퍼를 DMA로 쓸 수 있도록 lkey 첨부 */

		recv_wr->wr_id = (uint64_t)&rsp->rdma_wr;	/* [한국어] CQ 완료 시 wr_id로 rdma_wr 복원 → SPDK_CONTAINEROF로 rsp 복원 */
		recv_wr->next = NULL;	/* [한국어] 단일 WR — flush_recv_wrs에서 chain 구성 */
		recv_wr->sg_list = rsp_sgl;	/* [한국어] 1개 SGE */
		recv_wr->num_sge = 1;	/* [한국어] CPL은 contig라 1로 충분 */

		nvme_rdma_trace_ibv_sge(recv_wr->sg_list);	/* [한국어] 디버그 로그 매크로 */

		if (opts->rqpair) {
			spdk_rdma_provider_qp_queue_recv_wrs(opts->rqpair->rdma_qp, recv_wr);	/* [한국어] 큐페어 RQ에 큐잉 */
		} else {
			spdk_rdma_provider_srq_queue_recv_wrs(opts->srq, recv_wr);	/* [한국어] SRQ에 큐잉 */
		}
	}

	rsps->num_entries = opts->num_entries;	/* [한국어] 풀 크기 고정 */
	rsps->current_num_recvs = opts->num_entries;	/* [한국어] 모두 게시 예정 → 카운터 = num_entries */

	return rsps;
fail:
	nvme_rdma_free_rsps(rsps);
	return NULL;
}

/*
 * [한국어]
 * nvme_rdma_free_reqs - 큐페어의 사전 할당 RDMA 요청 풀(rdma_reqs/cmds) 해제.
 *
 * @rqpair: 큐페어.
 *
 * 두 개의 평행 배열(rdma_reqs[N], cmds[N])을 spdk_free로 해제. 둘 다 DMA 가능 hugepage 메모리.
 */
static void
nvme_rdma_free_reqs(struct nvme_rdma_qpair *rqpair)
{
	if (!rqpair->rdma_reqs) {
		return;
	}

	spdk_free(rqpair->cmds);	/* [한국어] NVMe Cmd Capsule 버퍼 배열 */
	rqpair->cmds = NULL;

	spdk_free(rqpair->rdma_reqs);	/* [한국어] rdma_req 배열 */
	rqpair->rdma_reqs = NULL;
}

/*
 * [한국어]
 * nvme_rdma_create_reqs - 큐페어의 SEND 요청 풀(rdma_reqs[N] + cmds[N]) 사전 할당 + 초기화.
 *
 * @rqpair: 큐페어 (mr_map은 이미 생성되어 있어야 lkey 변환 가능).
 * @return: 0=성공, -ENOMEM=할당/등록 실패.
 *
 * 동기/배경:
 *   I/O hot path에서 매번 malloc하지 않도록 num_entries 만큼의 SEND 요청과 NVMe Cmd 버퍼를 미리 만든다.
 *   각 rdma_req의 send_wr는 opcode=IBV_WR_SEND, wr_id=&rdma_wr로 고정 초기화 → submit 시 SGE 길이만 갱신.
 *   send_sgl[0].addr = cmds[i] 고정 (NVMe Cmd Capsule), [1]은 inline data일 때만 채움.
 *
 * 동작:
 *   1) rdma_reqs[N], cmds[N]을 hugepage(DMA)로 zmalloc.
 *   2) free_reqs/outstanding_reqs TAILQ 초기화.
 *   3) 각 슬롯:
 *      - rdma_wr.type=SEND (CQ 처리 시 분기 키)
 *      - cmds[i]를 mr_map에서 lkey 조회 → send_sgl[0].lkey에 박음
 *      - send_wr.opcode=IBV_WR_SEND, send_flags=IBV_SEND_SIGNALED (완료 알림 받기 위해)
 *      - free_reqs에 enqueue (req_get으로 사용 시작)
 *
 * 호출 체인:
 *   nvme_rdma_connect_established → [nvme_rdma_create_reqs]
 */
static int
nvme_rdma_create_reqs(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_rdma_utils_memory_translation translation;	/* [한국어] vtophys 변환 결과 */
	uint16_t i;
	int rc;

	assert(!rqpair->rdma_reqs);	/* [한국어] 두 번 호출 가드 */
	rqpair->rdma_reqs = spdk_zmalloc(rqpair->num_entries * sizeof(struct spdk_nvme_rdma_req), 0, NULL,
					 SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);	/* [한국어] DMA 가능 메모리 — RDMA 등록 가능해야 함 */
	if (rqpair->rdma_reqs == NULL) {
		NVME_RQPAIR_ERRLOG(rqpair, "Failed to allocate rdma_reqs\n");
		goto fail;
	}

	assert(!rqpair->cmds);
	rqpair->cmds = spdk_zmalloc(rqpair->num_entries * sizeof(*rqpair->cmds), 0, NULL,
				    SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);	/* [한국어] NVMe Cmd Capsule 버퍼 배열 */
	if (!rqpair->cmds) {
		NVME_RQPAIR_ERRLOG(rqpair, "Failed to allocate RDMA cmds\n");
		goto fail;
	}

	TAILQ_INIT(&rqpair->free_reqs);
	TAILQ_INIT(&rqpair->outstanding_reqs);
	for (i = 0; i < rqpair->num_entries; i++) {	/* [한국어] 각 슬롯 사전 초기화 */
		struct spdk_nvme_rdma_req	*rdma_req;
		struct spdk_nvmf_cmd		*cmd;

		rdma_req = &rqpair->rdma_reqs[i];
		rdma_req->rdma_wr.type = RDMA_WR_TYPE_SEND;	/* [한국어] CQ 처리 시 분기 키 */
		cmd = &rqpair->cmds[i];

		rdma_req->id = i;	/* [한국어] cid로 사용 — 응답 cpl.cid 매칭 */

		rc = spdk_rdma_utils_get_translation(rqpair->mr_map, cmd, sizeof(*cmd), &translation);	/* [한국어] cmd 버퍼의 등록된 lkey 조회 */
		if (rc) {
			goto fail;
		}
		rdma_req->send_sgl[0].lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);	/* [한국어] SEND가 사용할 lkey 고정 */

		/* The first RDMA sgl element will always point
		 * at this data structure. Depending on whether
		 * an NVMe-oF SGL is required, the length of
		 * this element may change. */
		/* [한국어] SGE[0]은 항상 cmds[i] 시작 — length만 build 함수에서 64B(NVMe Cmd) 또는 64B+SGL descriptors로 조정 */
		rdma_req->send_sgl[0].addr = (uint64_t)cmd;
		rdma_req->send_wr.wr_id = (uint64_t)&rdma_req->rdma_wr;	/* [한국어] CQ에서 wr_id로 rdma_req 복원 (SPDK_CONTAINEROF) */
		rdma_req->send_wr.next = NULL;
		rdma_req->send_wr.opcode = IBV_WR_SEND;	/* [한국어] verbs WR 종류: SEND (NVMe Cmd Capsule을 메시지로 송신) */
		rdma_req->send_wr.send_flags = IBV_SEND_SIGNALED;	/* [한국어] 완료 통지 받기 — un-signaled 모드는 완료 누락 위험으로 미사용 */
		rdma_req->send_wr.sg_list = rdma_req->send_sgl;	/* [한국어] SGE 배열 포인터 (최대 2개) */
		rdma_req->send_wr.imm_data = 0;	/* [한국어] NVMe-oF에서는 immediate 데이터 미사용 */

		TAILQ_INSERT_TAIL(&rqpair->free_reqs, rdma_req, link);	/* [한국어] free 풀에 enqueue — 사용 시작 가능 */
	}

	return 0;
fail:
	nvme_rdma_free_reqs(rqpair);	/* [한국어] 부분 할당 해제 */
	return -ENOMEM;
}

static int nvme_rdma_connect(struct nvme_rdma_qpair *rqpair);	/* [한국어] 전방 선언 — route_resolved → connect 직진 호출 위해 */

/*
 * [한국어]
 * nvme_rdma_route_resolved - ROUTE_RESOLVED 이벤트 도착 시 호출되는 콜백.
 *
 * @rqpair: 큐페어.
 * @ret: validate_cm_event 결과 (0=정상, 그 외=오류).
 *
 * 동작 순서:
 *   1) 라우팅 결과 검증 (실패 시 즉시 abort).
 *   2) nvme_rdma_qpair_init → ibv_create_qp + CQ + PD (verbs 자원).
 *   3) nvme_rdma_connect → rdma_connect (RDMA 연결 협상 시작).
 *
 * 호출 체인 (CM 단계):
 *   resolve_addr → ADDR_RESOLVED → addr_resolved → resolve_route → ROUTE_RESOLVED → [route_resolved] → qpair_init + connect
 */
static int
nvme_rdma_route_resolved(struct nvme_rdma_qpair *rqpair, int ret)
{
	if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "RDMA route resolution error\n");
		return -1;	/* [한국어] 라우팅 실패 — 상태머신이 connect 포기 */
	}

	ret = nvme_rdma_qpair_init(rqpair);	/* [한국어] QP/CQ 생성 — verbs 자원 확보 */
	if (ret < 0) {
		NVME_RQPAIR_ERRLOG(rqpair, "nvme_rdma_qpair_init() failed\n");
		return -1;
	}

	return nvme_rdma_connect(rqpair);	/* [한국어] rdma_connect 발사 — ESTABLISHED 이벤트 대기 시작 */
}

/*
 * [한국어]
 * nvme_rdma_addr_resolved - ADDR_RESOLVED 이벤트 도착 시 호출되는 콜백.
 *
 * @rqpair: 큐페어.
 * @ret: validate 결과.
 *
 * 동작:
 *   1) 주소 해상도 결과 검증.
 *   2) (옵션) RDMA_OPTION_ID_ACK_TIMEOUT 적용 (ACK timeout = 4.096us * 2^value).
 *   3) (옵션) RDMA_OPTION_ID_TOS 적용 (IP ToS — 라우팅 우선순위).
 *   4) rdma_resolve_route 발사 → ROUTE_RESOLVED 이벤트 대기.
 *
 * #ifdef는 librdmacm 버전별 옵션 가용성에 따라 분기 (구버전은 ack_timeout/tos 미지원).
 */
static int
nvme_rdma_addr_resolved(struct nvme_rdma_qpair *rqpair, int ret)
{
	if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "RDMA address resolution error\n");
		return -1;	/* [한국어] ARP/주소 해상도 실패 */
	}

	if (rqpair->qpair.ctrlr->opts.transport_ack_timeout != SPDK_NVME_TRANSPORT_ACK_TIMEOUT_DISABLED) {	/* [한국어] 사용자가 ack_timeout 지정한 경우 */
#ifdef SPDK_CONFIG_RDMA_SET_ACK_TIMEOUT
		uint8_t timeout = rqpair->qpair.ctrlr->opts.transport_ack_timeout;
		ret = rdma_set_option(rqpair->cm_id, RDMA_OPTION_ID,
				      RDMA_OPTION_ID_ACK_TIMEOUT,
				      &timeout, sizeof(timeout));	/* [한국어] InfiniBand 스펙: ack_timeout = 4.096us * 2^value (max 31) */
		if (ret) {
			NVME_RQPAIR_NOTICELOG(rqpair, "Can't apply RDMA_OPTION_ID_ACK_TIMEOUT %d, ret %d\n", timeout, ret);
		}
#else
		NVME_RQPAIR_DEBUGLOG(rqpair, "transport_ack_timeout is not supported\n");	/* [한국어] librdmacm 구버전 */
#endif
	}

	if (rqpair->qpair.ctrlr->opts.transport_tos != SPDK_NVME_TRANSPORT_TOS_DISABLED) {	/* [한국어] IP ToS 옵션 */
#ifdef SPDK_CONFIG_RDMA_SET_TOS
		uint8_t tos = rqpair->qpair.ctrlr->opts.transport_tos;
		ret = rdma_set_option(rqpair->cm_id, RDMA_OPTION_ID, RDMA_OPTION_ID_TOS, &tos, sizeof(tos));	/* [한국어] RoCE/iWARP 라우팅 우선순위 — DSCP 매핑 */
		if (ret) {
			NVME_RQPAIR_NOTICELOG(rqpair, "Can't apply RDMA_OPTION_ID_TOS %u, ret %d\n", tos, ret);
		}
#else
		NVME_RQPAIR_DEBUGLOG(rqpair, "transport_tos is not supported\n");
#endif
	}

	ret = rdma_resolve_route(rqpair->cm_id, NVME_RDMA_TIME_OUT_IN_MS);	/* [한국어] librdmacm: 라우팅(GID/LID 결정) — 비동기, ROUTE_RESOLVED 이벤트로 알림 */
	if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "rdma_resolve_route\n");
		return ret;
	}

	return nvme_rdma_process_event_start(rqpair, RDMA_CM_EVENT_ROUTE_RESOLVED,
					     nvme_rdma_route_resolved);	/* [한국어] 다음 단계 콜백 등록 */
}

/*
 * [한국어]
 * nvme_rdma_resolve_addr - CM 핸드셰이크 시작: rdma_resolve_addr 발사.
 *
 * @rqpair: 큐페어.
 * @src_addr: (옵션) 호스트 측 바인드 주소. NULL이면 OS가 자동 선택.
 * @dst_addr: 타깃 주소 (필수).
 * @return: 0=성공 발사, 음수=즉시 실패.
 *
 * 동작:
 *   1) src_addr 지정 시 RDMA_OPTION_ID_REUSEADDR로 SO_REUSEADDR 효과 활성 (소스 포트 재사용).
 *   2) rdma_resolve_addr — IP→GID/LID 해상도 + 로컬 RDMA device 결정 (ARP/path record).
 *   3) ADDR_RESOLVED 이벤트 콜백 등록.
 *
 * NVMe-oF 핸드셰이크 진입점 — ctrlr_connect_qpair에서 직접 호출.
 */
static int
nvme_rdma_resolve_addr(struct nvme_rdma_qpair *rqpair,
		       struct sockaddr *src_addr,
		       struct sockaddr *dst_addr)
{
	int ret;

	if (src_addr) {
		int reuse = 1;

		ret = rdma_set_option(rqpair->cm_id, RDMA_OPTION_ID, RDMA_OPTION_ID_REUSEADDR,
				      &reuse, sizeof(reuse));	/* [한국어] 소켓의 SO_REUSEADDR 유사 — 소스 포트 빠른 재사용 */
		if (ret) {
			NVME_RQPAIR_NOTICELOG(rqpair, "Can't apply RDMA_OPTION_ID_REUSEADDR %d, ret %d\n", reuse, ret);
			/* It is likely that rdma_resolve_addr() returns -EADDRINUSE, but
			 * we may missing something. We rely on rdma_resolve_addr().
			 */
			/* [한국어] REUSEADDR 실패해도 일단 resolve 시도 — resolve가 EADDRINUSE 주면 그때 포기 */
		}
	}

	ret = rdma_resolve_addr(rqpair->cm_id, src_addr, dst_addr,
				NVME_RDMA_TIME_OUT_IN_MS);	/* [한국어] librdmacm: IP → RDMA 주소 해상도. 비동기, ADDR_RESOLVED 이벤트로 알림. ARP 비슷한 동작 */
	if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "rdma_resolve_addr, %d\n", errno);
		return ret;
	}

	return nvme_rdma_process_event_start(rqpair, RDMA_CM_EVENT_ADDR_RESOLVED,
					     nvme_rdma_addr_resolved);	/* [한국어] ADDR_RESOLVED 콜백 등록 */
}

static int nvme_rdma_stale_conn_retry(struct nvme_rdma_qpair *rqpair);	/* [한국어] 전방 선언 — stale conn 자동 재시도 */

/*
 * [한국어]
 * nvme_rdma_connect_established - ESTABLISHED 이벤트 도착 후 본격적인 NVMe-oF 자원 준비.
 *
 * @rqpair: 큐페어 (RDMA 연결 완료, QP가 RTS 상태).
 * @ret: validate 결과 (0=정상, -ESTALE=재시도 필요).
 *
 * 동작:
 *   1) -ESTALE이면 stale_conn_retry → disconnect+재연결 시퀀스 진입.
 *   2) MR(메모리 등록) 풀 생성 — 페이로드 버퍼의 lkey/rkey 변환에 사용. 권한 LOCAL_WRITE | REMOTE_READ | REMOTE_WRITE.
 *   3) SEND 요청 풀(rdma_reqs/cmds) 사전 할당.
 *   4) (SRQ 미사용 시) 응답 풀(rsps) 생성 + ibv_post_recv 일괄 게시 → 타깃 응답 받을 준비.
 *   5) 상태를 FABRIC_CONNECT_SEND로 전이 → 다음 폴 사이클에서 NVMe-oF Fabrics CONNECT 커맨드 송신.
 *
 * NVMe-oF 핵심: 이 시점부터 RDMA 연결은 완료되었지만, NVMe-oF 컨트롤러/큐페어 등록(Fabrics CONNECT)은 아직.
 * RDMA 연결 establishment ↔ NVMe-oF Fabrics CONNECT 핸드셰이크는 별도 단계로, 양자 모두 완료되어야 I/O 가능.
 */
static int
nvme_rdma_connect_established(struct nvme_rdma_qpair *rqpair, int ret)
{
	struct nvme_rdma_rsp_opts opts = {};

	if (ret == -ESTALE) {
		return nvme_rdma_stale_conn_retry(rqpair);	/* [한국어] 타깃이 이전 세션 보관 중 → 재시도 */
	} else if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "RDMA connect error %d\n", ret);
		return ret;
	}

	assert(!rqpair->mr_map);
	rqpair->mr_map = spdk_rdma_utils_create_mem_map(rqpair->rdma_qp->qp->pd, &g_nvme_hooks,
			 IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);	/* [한국어] MR 풀 — 페이로드 등록 후 lkey(local)/rkey(remote) 발급. 권한: 호스트가 쓰기 + 타깃이 읽기/쓰기 */
	if (!rqpair->mr_map) {
		NVME_RQPAIR_ERRLOG(rqpair, "Unable to register RDMA memory translation map\n");
		return -1;
	}

	ret = nvme_rdma_create_reqs(rqpair);	/* [한국어] SEND 요청 풀 사전 할당 (cmds[N], rdma_reqs[N]) */
	if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "Unable to create rqpair RDMA requests: %d\n", ret);
		return -1;
	}
	NVME_RQPAIR_DEBUGLOG(rqpair, "RDMA requests created\n");

	if (!rqpair->srq) {	/* [한국어] SRQ 미사용 시에만 큐페어 전용 응답 풀 생성 (SRQ 모드는 poller가 만든 공유 풀 사용) */
		opts.num_entries = rqpair->num_entries;
		opts.rqpair = rqpair;
		opts.srq = NULL;
		opts.mr_map = rqpair->mr_map;

		assert(!rqpair->rsps);
		rqpair->rsps = nvme_rdma_create_rsps(&opts);	/* [한국어] 응답 풀 + RECV WR 큐잉 */
		if (!rqpair->rsps) {
			NVME_RQPAIR_ERRLOG(rqpair, "Unable to create rqpair RDMA responses\n");
			return -1;
		}
		NVME_RQPAIR_DEBUGLOG(rqpair, "RDMA responses created\n");

		ret = nvme_rdma_qpair_submit_recvs(rqpair);	/* [한국어] 큐잉된 RECV WR을 ibv_post_recv로 일괄 게시 — 타깃 응답 도착 준비 완료 */
		if (ret) {
			NVME_RQPAIR_ERRLOG(rqpair, "Unable to submit rqpair RDMA responses: %d\n", ret);
			return -1;
		}
		NVME_RQPAIR_DEBUGLOG(rqpair, "RDMA responses submitted\n");
	}

	rqpair->state = NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_SEND;	/* [한국어] 상태 전이 — connect_qpair_poll가 다음 사이클에 Fabrics CONNECT 발사 */

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_connect - rdma_connect 호출 + ESTABLISHED 이벤트 콜백 등록.
 *
 * @rqpair: 큐페어 (qpair_init 완료, QP가 INIT 상태).
 * @return: 0=성공 발사, 음수=실패.
 *
 * 동기/배경 — RDMA 핸드셰이크 + NVMe-oF private_data 양자 핸드셰이크:
 *   rdma_connect는 RDMA Reliable Connection (RC) QP를 INIT → RTR → RTS 상태로 전이시키며,
 *   타깃과 PSN/QPN/MTU 등을 협상한다. 이 협상의 private_data 필드에 NVMe-oF Spec(섹션 7.1.4)에서 정의한
 *   spdk_nvmf_rdma_request_private_data를 실어 보내 — 타깃이 이를 보고 큐페어 깊이/cntlid를 결정.
 *   타깃이 응답으로 보내는 spdk_nvmf_rdma_accept_private_data는 ESTABLISHED 이벤트의 param.conn.private_data로 전달됨.
 *
 * 핵심 파라미터:
 *   - responder_resources = HCA의 max_qp_rd_atom (동시 RDMA_READ 최대 개수).
 *   - retry_count = transport_retry_count (NACK 시 재전송).
 *   - rnr_retry_count = 7 (RNR 무한 재시도, 응답 슬롯 일시 부족 대응).
 *   - private_data = NVMe-oF private_data (qid/hsqsize/hrqsize/cntlid).
 *
 * 호출 체인: route_resolved → qpair_init → [nvme_rdma_connect] → rdma_connect → ESTABLISHED → connect_established
 */
static int
nvme_rdma_connect(struct nvme_rdma_qpair *rqpair)
{
	struct rdma_conn_param				param = {};	/* [한국어] librdmacm 연결 파라미터 + private_data 슬롯 */
	struct spdk_nvmf_rdma_request_private_data	request_data = {};	/* [한국어] NVMe-oF Spec 정의: qid/cntlid/큐 깊이 */
	struct ibv_device_attr				attr;
	int						ret;
	struct spdk_nvme_ctrlr				*ctrlr;

	ret = ibv_query_device(rqpair->cm_id->verbs, &attr);	/* [한국어] HCA 능력 재조회 — max_qp_rd_atom 필요 */
	if (ret != 0) {
		NVME_RQPAIR_ERRLOG(rqpair, "Failed to query RDMA device attributes.\n");
		return ret;
	}

	param.responder_resources = attr.max_qp_rd_atom;	/* [한국어] 타깃이 호스트로부터 동시에 받을 수 있는 RDMA_READ 응답 개수 */

	ctrlr = rqpair->qpair.ctrlr;
	if (!ctrlr) {
		return -1;	/* [한국어] 컨트롤러 없는 큐페어는 비정상 */
	}

	request_data.qid = rqpair->qpair.id;	/* [한국어] NVMe-oF private_data: 큐페어 ID (0=admin) */
	request_data.hrqsize = rqpair->num_entries + 1;	/* [한국어] 호스트 RECV 큐 사이즈 (스펙: 큐 사이즈 + 1) */
	request_data.hsqsize = rqpair->num_entries;	/* [한국어] 호스트 SEND 큐 사이즈 = num_entries */
	request_data.cntlid = ctrlr->cntlid;	/* [한국어] 컨트롤러 ID — admin connect로 받은 값. I/O 큐는 이 값으로 묶임 */

	param.private_data = &request_data;	/* [한국어] CM 협상 시 함께 송신될 private_data */
	param.private_data_len = sizeof(request_data);
	param.retry_count = ctrlr->opts.transport_retry_count;	/* [한국어] RDMA 패킷 재전송 횟수 (max 7) */
	param.rnr_retry_count = 7;	/* [한국어] RNR(Receiver Not Ready) 재시도 — 7=무한. 응답 슬롯 일시 부족 시 끈기 있게 재시도 */

	/* Fields below are ignored by rdma cm if qpair has been
	 * created using rdma cm API. */
	/* [한국어] rdma_create_qp 사용 시 librdmacm이 srq/qp_num을 직접 채움 — 호스트가 입력해도 무시됨.
	 * 그러나 Mellanox direct verbs 사용 시에는 호스트가 채워야 함 → 호환성 위해 채움. */
	param.srq = 0;
	param.qp_num = rqpair->rdma_qp->qp->qp_num;

	ret = rdma_connect(rqpair->cm_id, &param);	/* [한국어] librdmacm: 핵심 — CM REQ 송신 + INIT→RTR→RTS QP 전이 시작. 비동기, ESTABLISHED 이벤트로 완료 알림 */
	if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "nvme rdma connect error\n");
		return ret;
	}

	ctrlr->numa.id_valid = 1;
	ctrlr->numa.id = spdk_rdma_cm_id_get_numa_id(rqpair->cm_id);	/* [한국어] HCA가 속한 NUMA 노드 ID 캐시 — 메모리 할당 affinity에 사용 */

	return nvme_rdma_process_event_start(rqpair, RDMA_CM_EVENT_ESTABLISHED,
					     nvme_rdma_connect_established);	/* [한국어] ESTABLISHED 콜백 등록 */
}

/*
 * [한국어]
 * nvme_rdma_ctrlr_connect_qpair - vtable의 ops.ctrlr_connect_qpair 구현 — 큐페어 CM 핸드셰이크 시작.
 *
 * @ctrlr: 컨트롤러.
 * @qpair: 연결할 큐페어 (admin 또는 I/O).
 * @return: 0=비동기 연결 시작 성공, -1=즉시 실패.
 *
 * 동기/배경:
 *   상위 nvme_ctrlr.c가 admin 큐페어 또는 I/O 큐페어 연결을 시작할 때 vtable로 호출.
 *   이 함수는 비동기 핸드셰이크의 첫 단계만 시작 (rdma_create_id + rdma_resolve_addr) → 이후 콜백 체인으로 진행.
 *   완료 여부는 ctrlr_connect_qpair_poll로 폴.
 *
 * 동작:
 *   1) trid의 adrfam을 sockaddr family로 변환 (IPv4/IPv6).
 *   2) 타깃 주소 파싱 (traddr+trsvcid → sockaddr_storage).
 *   3) (옵션) 호스트 측 src_addr 파싱.
 *   4) rdma_create_id로 cm_id 할당 — context=rqpair (CM 이벤트 시 큐페어 역참조).
 *   5) resolve_addr 발사 — ADDR_RESOLVED 이벤트 콜백 등록.
 *   6) 상태 INITIALIZING으로 전이, poll group 모드면 connecting_qpairs 큐에 enqueue.
 */
static int
nvme_rdma_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct sockaddr_storage dst_addr;	/* [한국어] 타깃 주소 (파싱 결과) */
	struct sockaddr_storage src_addr;	/* [한국어] 호스트 주소 (옵션) */
	bool src_addr_specified;
	long int port, src_port = 0;	/* [한국어] 포트 OUT 변수 (현재 미사용 — sockaddr에 이미 박힘) */
	int rc;
	struct nvme_rdma_ctrlr *rctrlr;
	struct nvme_rdma_qpair *rqpair;
	struct nvme_rdma_poll_group *group;
	int family;	/* [한국어] AF_INET 또는 AF_INET6 */

	rqpair = nvme_rdma_qpair(qpair);
	rctrlr = nvme_rdma_ctrlr(ctrlr);
	assert(rctrlr != NULL);

	switch (ctrlr->trid.adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		family = AF_INET;	/* [한국어] IPv4 */
		break;
	case SPDK_NVMF_ADRFAM_IPV6:
		family = AF_INET6;	/* [한국어] IPv6 */
		break;
	default:
		NVME_RQPAIR_ERRLOG(rqpair, "Unhandled ADRFAM %d\n", ctrlr->trid.adrfam);
		return -1;	/* [한국어] FC 등 미지원 — RDMA 트랜스포트는 IP만 */
	}

	NVME_RQPAIR_DEBUGLOG(rqpair, "adrfam %d ai_family %d\n", ctrlr->trid.adrfam, family);

	memset(&dst_addr, 0, sizeof(dst_addr));	/* [한국어] sockaddr 잔여 데이터 클리어 (보안) */

	NVME_RQPAIR_DEBUGLOG(rqpair, "trsvcid is %s\n", ctrlr->trid.trsvcid);
	rc = nvme_parse_addr(&dst_addr, family, ctrlr->trid.traddr, ctrlr->trid.trsvcid, &port);	/* [한국어] "traddr:trsvcid" → sockaddr 변환 */
	if (rc != 0) {
		NVME_RQPAIR_ERRLOG(rqpair, "dst_addr nvme_parse_addr() failed\n");
		return -1;
	}

	if (ctrlr->opts.src_addr[0] || ctrlr->opts.src_svcid[0]) {	/* [한국어] 사용자가 호스트 측 바인드 주소/포트 지정한 경우 */
		memset(&src_addr, 0, sizeof(src_addr));
		rc = nvme_parse_addr(&src_addr, family,
				     ctrlr->opts.src_addr[0] ? ctrlr->opts.src_addr : NULL,
				     ctrlr->opts.src_svcid[0] ? ctrlr->opts.src_svcid : NULL,
				     &src_port);
		if (rc != 0) {
			NVME_RQPAIR_ERRLOG(rqpair, "src_addr nvme_parse_addr() failed\n");
			return -1;
		}
		src_addr_specified = true;
	} else {
		src_addr_specified = false;	/* [한국어] OS가 자동 선택 */
	}

	rc = rdma_create_id(rctrlr->cm_channel, &rqpair->cm_id, rqpair, RDMA_PS_TCP);	/* [한국어] librdmacm: cm_id 생성. context=rqpair, port_space=TCP(연결지향, NVMe-oF는 RC QP만 사용) */
	if (rc < 0) {
		NVME_RQPAIR_ERRLOG(rqpair, "rdma_create_id() failed\n");
		return -1;
	}

	rc = nvme_rdma_resolve_addr(rqpair,
				    src_addr_specified ? (struct sockaddr *)&src_addr : NULL,
				    (struct sockaddr *)&dst_addr);	/* [한국어] 비동기 핸드셰이크 시작 → ADDR_RESOLVED → ROUTE_RESOLVED → ESTABLISHED 체인 */
	if (rc < 0) {
		NVME_RQPAIR_ERRLOG(rqpair, "nvme_rdma_resolve_addr() failed\n");
		return -1;
	}

	rqpair->state = NVME_RDMA_QPAIR_STATE_INITIALIZING;	/* [한국어] 핸드셰이크 진행 중 — connect_qpair_poll가 이벤트 폴 */

	if (qpair->poll_group != NULL && TAILQ_ENTRY_NOT_ENQUEUED(rqpair, link_connecting)) {	/* [한국어] poll group 모드 → poll_group_process_completions가 connecting_qpairs를 진척시킴 */
		group = nvme_rdma_poll_group(qpair->poll_group);
		TAILQ_INSERT_TAIL(&group->connecting_qpairs, rqpair, link_connecting);
	}

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_stale_conn_reconnect - stale conn 재시도 대기 후 connect_qpair 재호출.
 *
 * @rqpair: STALE_CONN 상태 큐페어.
 * @return: -EAGAIN=아직 대기, 또는 connect_qpair 결과.
 *
 * stale_conn_disconnected에서 evt_timeout_ticks(10ms 후)를 설정 — 그 시간이 지나면 재연결 시도.
 */
static int
nvme_rdma_stale_conn_reconnect(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;

	if (spdk_get_ticks() < rqpair->evt_timeout_ticks) {	/* [한국어] 10ms 안 지났으면 대기 */
		return -EAGAIN;
	}

	return nvme_rdma_ctrlr_connect_qpair(qpair->ctrlr, qpair);	/* [한국어] 처음부터 다시 (cm_id 새로 생성) */
}

/*
 * [한국어]
 * nvme_rdma_ctrlr_connect_qpair_poll - vtable의 ctrlr_connect_qpair_poll 구현 — 비동기 connect 진행도 확인.
 *
 * @ctrlr: 컨트롤러.
 * @qpair: 연결 중인 큐페어.
 * @return: 0=완료, -EAGAIN=진행 중, 음수=실패.
 *
 * 큐페어 상태머신을 한 단계 진행:
 *   - INITIALIZING/EXITING → process_event_poll (CM 이벤트 진행)
 *   - STALE_CONN → stale_conn_reconnect (재연결 시도)
 *   - FABRIC_CONNECT_SEND → nvme_fabric_qpair_connect_async (NVMe-oF Fabrics CONNECT 커맨드 송신)
 *   - FABRIC_CONNECT_POLL → nvme_fabric_qpair_connect_poll (응답 대기)
 *   - AUTHENTICATING → DH-CHAP 인증 진행
 *   - RUNNING → 0 (완료)
 *
 * in_connect_poll 가드는 재진입 방지 (콜백 체인이 자기 자신 호출 방지).
 */
static int
nvme_rdma_ctrlr_connect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	int rc;

	if (qpair->in_connect_poll) {
		return -EAGAIN;	/* [한국어] 재진입 가드 — 콜백 체인이 동기 호출 시 자기 자신 재호출 방지 */
	}

	qpair->in_connect_poll = true;	/* [한국어] 재진입 잠금 set */

	switch (rqpair->state) {
	case NVME_RDMA_QPAIR_STATE_INVALID:
		rc = -EAGAIN;	/* [한국어] 아직 connect_qpair 호출 전 — 다음 사이클 대기 */
		break;

	case NVME_RDMA_QPAIR_STATE_INITIALIZING:	/* [한국어] CM 핸드셰이크 진행 중 (ADDR/ROUTE/ESTABLISHED 대기) */
	case NVME_RDMA_QPAIR_STATE_EXITING:		/* [한국어] DISCONNECTED 이벤트 대기 중 */
		if (!nvme_qpair_is_admin_queue(qpair)) {
			nvme_ctrlr_lock(ctrlr);	/* [한국어] poll_events는 컨트롤러 락 필수 (pending_cm_events 보호) */
		}

		rc = nvme_rdma_process_event_poll(rqpair);	/* [한국어] CM 이벤트 1개 처리 + 콜백 실행 */

		if (!nvme_qpair_is_admin_queue(qpair)) {
			nvme_ctrlr_unlock(ctrlr);
		}

		if (rc == 0) {
			rc = -EAGAIN;	/* [한국어] 정상이지만 아직 다음 단계 — RUNNING 도달까진 EAGAIN 유지 */
		}
		qpair->in_connect_poll = false;

		return rc;

	case NVME_RDMA_QPAIR_STATE_STALE_CONN:
		rc = nvme_rdma_stale_conn_reconnect(rqpair);	/* [한국어] 10ms 대기 후 재연결 */
		if (rc == 0) {
			rc = -EAGAIN;	/* [한국어] 재연결 성공해도 INITIALIZING으로 돌아가니 EAGAIN */
		}
		break;
	case NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_SEND:
		rc = nvme_fabric_qpair_connect_async(qpair, rqpair->num_entries + 1);	/* [한국어] NVMe-oF Fabrics CONNECT 커맨드 송신 (nvme_fabric.c) — 일반 NVMe Cmd Capsule처럼 SEND WR로 발사 */
		if (rc == 0) {
			rqpair->state = NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_POLL;	/* [한국어] 다음 단계: 응답 대기 */
			rc = -EAGAIN;
		} else {
			NVME_RQPAIR_ERRLOG(rqpair, "Failed to send an NVMe-oF Fabric CONNECT command\n");
		}
		break;
	case NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_POLL:
		rc = nvme_fabric_qpair_connect_poll(qpair);	/* [한국어] CONNECT 응답 도착 검사 (RECV CQ 폴) */
		if (rc == 0) {
			if (nvme_fabric_qpair_auth_required(qpair)) {	/* [한국어] DH-CHAP 등 인증 필요 여부 (NVMe-oF 1.1) */
				rc = nvme_fabric_qpair_authenticate_async(qpair);
				if (rc == 0) {
					rqpair->state = NVME_RDMA_QPAIR_STATE_AUTHENTICATING;
					rc = -EAGAIN;
				}
			} else {
				rqpair->state = NVME_RDMA_QPAIR_STATE_RUNNING;	/* [한국어] 모든 핸드셰이크 완료 — I/O 가능 */
				nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);	/* [한국어] 상위 레이어에 통지 */
			}
		} else if (rc != -EAGAIN) {
			NVME_RQPAIR_ERRLOG(rqpair, "Failed to poll NVMe-oF Fabric CONNECT command\n");
		}
		break;
	case NVME_RDMA_QPAIR_STATE_AUTHENTICATING:
		rc = nvme_fabric_qpair_authenticate_poll(qpair);	/* [한국어] DH-CHAP 진행 폴 */
		if (rc == 0) {
			rqpair->state = NVME_RDMA_QPAIR_STATE_RUNNING;
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
		}
		break;
	case NVME_RDMA_QPAIR_STATE_RUNNING:
		rc = 0;	/* [한국어] 이미 연결 완료 — 호출자가 더 이상 폴 안 해도 됨 */
		break;
	default:
		assert(false);	/* [한국어] LINGERING/EXITED 등은 disconnect_poll 경로 — 여기 도달 시 버그 */
		rc = -EINVAL;
		break;
	}

	qpair->in_connect_poll = false;	/* [한국어] 재진입 잠금 해제 */
	return rc;
}

/*
 * [한국어]
 * nvme_rdma_get_memory_translation - I/O 페이로드 버퍼의 가상주소를 RDMA lkey/rkey로 변환.
 *
 * @req: NVMe 요청 (memory_domain 옵션 보유 가능).
 * @rqpair: 큐페어 (mr_map 보유).
 * @_ctx: IN: 변환 대상 addr/length, OUT: 변환된 lkey/rkey + (memory_domain 경우) 갱신된 addr/length.
 * @return: 0=성공, 음수=오류.
 *
 * 동기/배경:
 *   RDMA HCA가 호스트 메모리에 DMA 접근하려면 해당 페이지가 미리 ibv_reg_mr로 등록되어 있어야 한다.
 *   등록은 spdk_rdma_utils가 vtophys 매핑(가상→물리)과 함께 자동 관리. 여기서는 (addr,length) → (lkey,rkey) 조회만.
 *
 *   2가지 경로:
 *   a) memory_domain 사용 (예: GPU Direct, NVMe controller-managed memory): payload.opts->memory_domain의
 *      translate_data 콜백이 lkey/rkey/iov를 직접 제공 (외부 도메인 → RDMA 도메인 브리지).
 *   b) 일반 호스트 메모리: spdk_rdma_utils_get_translation으로 mr_map 조회.
 *      MR 등록 형태(ibv_reg_mr)면 mr->lkey/rkey 사용, key 형태(direct memory key)면 단일 key 사용.
 */
static inline int
nvme_rdma_get_memory_translation(struct nvme_request *req, struct nvme_rdma_qpair *rqpair,
				 struct nvme_rdma_memory_translation_ctx *_ctx)
{
	struct spdk_memory_domain_translation_ctx ctx;	/* [한국어] memory_domain 호출용 입력 컨텍스트 */
	struct spdk_memory_domain_translation_result dma_translation = {.iov_count = 0};	/* [한국어] OUT: 변환 결과 */
	struct spdk_rdma_utils_memory_translation rdma_translation;	/* [한국어] 일반 경로 OUT */
	int rc;

	assert(req);
	assert(rqpair);
	assert(_ctx);

	if (req->payload.opts && req->payload.opts->memory_domain) {	/* [한국어] (a) 외부 memory_domain 경로 */
		ctx.size = sizeof(struct spdk_memory_domain_translation_ctx);
		ctx.rdma.ibv_qp = rqpair->rdma_qp->qp;	/* [한국어] 변환 대상 QP — 도메인이 어느 QP에서 쓰일지 알아야 함 */
		dma_translation.size = sizeof(struct spdk_memory_domain_translation_result);

		rc = spdk_memory_domain_translate_data(req->payload.opts->memory_domain,
						       req->payload.opts->memory_domain_ctx,
						       rqpair->rdma_qp->domain, &ctx, _ctx->addr,
						       _ctx->length, &dma_translation);	/* [한국어] 외부 도메인 (예: GPU) → RDMA 도메인 변환 콜백 */
		if (spdk_unlikely(rc) || dma_translation.iov_count != 1) {
			NVME_RQPAIR_ERRLOG(rqpair, "DMA memory translation failed, rc %d, iov count %u\n", rc,
					   dma_translation.iov_count);
			return rc;	/* [한국어] iov_count != 1 = 외부 도메인이 페이로드를 분할 → 현재 미지원 */
		}

		_ctx->lkey = dma_translation.rdma.lkey;	/* [한국어] 외부 도메인이 알려준 lkey */
		_ctx->rkey = dma_translation.rdma.rkey;	/* [한국어] rkey도 (타깃이 RDMA 접근 시 사용) */
		_ctx->addr = dma_translation.iov.iov_base;	/* [한국어] 변환된 가상주소 (예: GPU 메모리 가상주소 → 호스트 매핑) */
		_ctx->length = dma_translation.iov.iov_len;
	} else {	/* [한국어] (b) 일반 호스트 메모리 경로 */
		rc = spdk_rdma_utils_get_translation(rqpair->mr_map, _ctx->addr, _ctx->length, &rdma_translation);	/* [한국어] mr_map에서 등록된 MR 찾고 lkey/rkey 추출 */
		if (spdk_unlikely(rc)) {
			NVME_RQPAIR_ERRLOG(rqpair, "RDMA memory translation failed, rc %d\n", rc);
			return rc;	/* [한국어] 미등록 페이지 — vtophys 매핑 누락 (DPDK hugepage 외부 메모리?) */
		}
		if (rdma_translation.translation_type == SPDK_RDMA_UTILS_TRANSLATION_MR) {
			_ctx->lkey = rdma_translation.mr_or_key.mr->lkey;	/* [한국어] 표준 ibv_reg_mr 경로 */
			_ctx->rkey = rdma_translation.mr_or_key.mr->rkey;
		} else {
			_ctx->lkey = _ctx->rkey = (uint32_t)rdma_translation.mr_or_key.key;	/* [한국어] direct memory key — Mellanox 가상 MR (UMR 등) */
		}
	}

	return 0;
}


/*
 * Build SGL describing empty payload.
 */
/*
 * [한국어]
 * nvme_rdma_build_null_request - 페이로드 없는 NVMe Cmd용 SGL 빌드 (예: Identify, Get Log Page).
 *
 * SEND WR num_sge=1: NVMe Cmd Capsule(64B)만. 데이터 phase 없음.
 * NVMe-oF Spec: dptr.sgl1을 KEYED_DATA_BLOCK + length=0으로 채워 "데이터 없음" 명시.
 */
static int
nvme_rdma_build_null_request(struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;	/* [한국어] PSDT(PRP/SGL Data Type)=01b: SGL contig (NVMe Cmd 4.4) */

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);	/* [한국어] SEND SGE[0] = NVMe Cmd 64B만 */

	/* The RDMA SGL needs one element describing the NVMe command. */
	rdma_req->send_wr.num_sge = 1;	/* [한국어] inline data 없음 → SGE 1개 */

	req->cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;	/* [한국어] NVMe-oF 스펙 4.5: keyed SGL */
	req->cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	req->cmd.dptr.sgl1.keyed.length = 0;	/* [한국어] 데이터 길이 0 = 페이로드 없음 */
	req->cmd.dptr.sgl1.keyed.key = 0;
	req->cmd.dptr.sgl1.address = 0;

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_configure_contig_inline_request - in-capsule data 모드 SEND WR/Cmd 필드 설정 (헬퍼).
 *
 * 동기/배경 — In-Capsule Data (ICD):
 *   작은 Write 데이터(<= ioccsz - 64)를 NVMe Cmd Capsule 메시지에 인라인 → 1번의 SEND WR로 Cmd+Data 전송.
 *   타깃이 RDMA_READ 발행하지 않아도 되어 latency 절감 (특히 4KB 이하 IO).
 *
 *   이 함수는 SEND WR을 SGE 2개로 구성:
 *     [0] = NVMe Cmd 64B (rqpair->cmds[id])
 *     [1] = 호스트 페이로드 버퍼 (ctx->addr, ctx->length, lkey)
 *   NVMe-oF dptr.sgl1.unkeyed.type = DATA_BLOCK (타깃이 RDMA_READ 안 하고 SEND 메시지 자체에서 데이터 추출).
 *   address = 0: icdoff=0 가정 (NVMe Cmd 끝 직후부터 데이터).
 */
static inline void
nvme_rdma_configure_contig_inline_request(struct spdk_nvme_rdma_req *rdma_req,
		struct nvme_request *req, struct nvme_rdma_memory_translation_ctx *ctx)
{
	rdma_req->send_sgl[1].lkey = ctx->lkey;	/* [한국어] 호스트 데이터의 lkey — HCA가 SEND 시 이 lkey로 데이터 읽기 */

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);	/* [한국어] SGE[0] = Cmd 64B */

	rdma_req->send_sgl[1].addr = (uint64_t)ctx->addr;	/* [한국어] SGE[1] = 페이로드 시작 */
	rdma_req->send_sgl[1].length = (uint32_t)ctx->length;	/* [한국어] 페이로드 길이 */

	/* The RDMA SGL contains two elements. The first describes
	 * the NVMe command and the second describes the data
	 * payload. */
	rdma_req->send_wr.num_sge = 2;	/* [한국어] SEND WR = Cmd + 인라인 데이터 (gather) */

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;	/* [한국어] unkeyed = 타깃이 RDMA_READ/WRITE 발행 불필요 (메시지 자체로 데이터 도착) */
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
	req->cmd.dptr.sgl1.unkeyed.length = (uint32_t)ctx->length;
	/* Inline only supported for icdoff == 0 currently.  This function will
	 * not get called for controllers with other values. */
	/* [한국어] icdoff=0 가정 — Cmd Capsule 끝 직후가 데이터 시작. address 필드의 0은 "Capsule 내부 오프셋 0" 의미.
	 * NVMe-oF Spec 7.4.5: 컨트롤러의 icdoff != 0 이면 이 경로 사용 안 함 (req_init이 분기에서 제외). */
	req->cmd.dptr.sgl1.address = (uint64_t)0;
}

/*
 * Build inline SGL describing contiguous payload buffer.
 */
/*
 * [한국어]
 * nvme_rdma_build_contig_inline_request - 연속 버퍼 + ICD 경로 SEND WR 빌드.
 *
 * 동작:
 *   1) payload.contig_or_cb_arg + offset 시작 주소 + payload_size로 ctx 초기화.
 *   2) get_memory_translation으로 lkey 획득.
 *   3) configure_contig_inline_request로 SEND WR/Cmd 필드 채움.
 *
 * 호출 조건: payload type=CONTIG + ICD 가능 (Write + size <= ioccsz_bytes + icdoff=0).
 */
static inline int
nvme_rdma_build_contig_inline_request(struct nvme_rdma_qpair *rqpair,
				      struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;
	struct nvme_rdma_memory_translation_ctx ctx = {
		.addr = (uint8_t *)req->payload.contig_or_cb_arg + req->payload_offset,	/* [한국어] 사용자 contig 버퍼 + 부분 오프셋 (멀티 청크 분할 시) */
		.length = req->payload_size
	};
	int rc;

	assert(ctx.length != 0);	/* [한국어] inline은 데이터 있을 때만 호출 */
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	rc = nvme_rdma_get_memory_translation(req, rqpair, &ctx);	/* [한국어] vtophys + lkey 변환 */
	if (spdk_unlikely(rc)) {
		return -1;
	}

	nvme_rdma_configure_contig_inline_request(rdma_req, req, &ctx);	/* [한국어] WR/Cmd 필드 채움 */

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_configure_contig_request - keyed SGL(zero-copy) 모드 SEND WR/Cmd 필드 설정 (헬퍼).
 *
 * 동기/배경 — Keyed SGL (RDMA_READ/WRITE):
 *   큰 데이터(또는 Read)는 ICD 안 됨 → 호스트는 NVMe Cmd Capsule만 SEND, 페이로드 buf의 (addr, length, rkey)를 알려줌.
 *   타깃이 그 정보로 RDMA_READ(호스트→타깃, Write 데이터 페치) 또는 RDMA_WRITE(타깃→호스트, Read 응답)를 발행 → zero-copy.
 *   호스트 측 페이로드는 미리 IBV_ACCESS_REMOTE_READ|WRITE 권한으로 등록되어 있어야 함 (mr_map에서 보장).
 */
static inline void
nvme_rdma_configure_contig_request(struct spdk_nvme_rdma_req *rdma_req, struct nvme_request *req,
				   struct nvme_rdma_memory_translation_ctx *ctx)
{
	req->cmd.dptr.sgl1.keyed.key = ctx->rkey;	/* [한국어] 핵심: 타깃이 호스트 메모리 RDMA 접근 시 사용할 rkey */

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);	/* [한국어] SEND는 Cmd 64B만 */

	/* The RDMA SGL needs one element describing the NVMe command. */
	rdma_req->send_wr.num_sge = 1;	/* [한국어] 데이터는 RDMA_READ/WRITE로 별도 흐름 */

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;	/* [한국어] keyed = rkey 동봉 */
	req->cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;	/* [한국어] address 필드는 호스트 가상주소 */
	req->cmd.dptr.sgl1.keyed.length = (uint32_t)ctx->length;
	req->cmd.dptr.sgl1.address = (uint64_t)ctx->addr;	/* [한국어] 호스트 가상주소 — 타깃이 RDMA_READ/WRITE할 대상 */
}

/*
 * Build SGL describing contiguous payload buffer.
 */
/*
 * [한국어]
 * nvme_rdma_build_contig_request - 연속 버퍼 + keyed SGL 경로 SEND WR 빌드.
 *
 * 호출 조건: contig payload + (ICD 불가) 또는 큰 Write/모든 Read.
 * NVME_RDMA_MAX_KEYED_SGL_LENGTH(16MB-1) 초과 페이로드는 단일 SGL로 표현 불가 → 에러.
 */
static inline int
nvme_rdma_build_contig_request(struct nvme_rdma_qpair *rqpair,
			       struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;
	struct nvme_rdma_memory_translation_ctx ctx = {
		.addr = (uint8_t *)req->payload.contig_or_cb_arg + req->payload_offset,
		.length = req->payload_size
	};
	int rc;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	if (spdk_unlikely(req->payload_size > NVME_RDMA_MAX_KEYED_SGL_LENGTH)) {	/* [한국어] keyed SGL length는 3바이트 → max 16MB-1 */
		NVME_RQPAIR_ERRLOG(rqpair, "SGL length %u exceeds max keyed SGL block size %u\n", req->payload_size,
				   NVME_RDMA_MAX_KEYED_SGL_LENGTH);
		return -1;
	}

	rc = nvme_rdma_get_memory_translation(req, rqpair, &ctx);	/* [한국어] rkey 획득 (LOCAL_WRITE|REMOTE_READ|REMOTE_WRITE 권한 등록되어 있어야) */
	if (spdk_unlikely(rc)) {
		return -1;
	}

	nvme_rdma_configure_contig_request(rdma_req, req, &ctx);	/* [한국어] WR/Cmd 필드 채움 */

	return 0;
}

/*
 * Build SGL describing scattered payload buffer.
 */
/*
 * [한국어]
 * nvme_rdma_build_sgl_request - scatter-gather 페이로드를 multi-segment keyed SGL로 빌드.
 *
 * 동기/배경 — Multi-segment SGL (NVMe-oF Spec 4.5):
 *   페이로드가 흩어진 여러 청크로 구성되었을 때, 각 청크별로 (addr, length, rkey)를 가지는 keyed SGL descriptor를
 *   여러 개 만들어 하나의 NVMe Cmd Capsule 안에 첨부 → 타깃이 청크별로 RDMA_READ/WRITE 발행.
 *   첫 SGL descriptor가 LAST_SEGMENT 타입이면 캡슐 끝에 추가 descriptor 배열이 따라옴.
 *
 * 동작:
 *   1) reset_sgl_fn으로 사용자 SGL 이터레이터 리셋.
 *   2) next_sge_fn 루프: 청크별 addr/length 획득 → memory_translation으로 rkey → cmds[id]->sgl[i]에 채움.
 *   3) 청크 1개면 dptr.sgl1에 직접 임베드 (segment 추가 없음).
 *   4) 청크 여러 개면 dptr.sgl1.type=LAST_SEGMENT, descriptor 배열은 cmds[id]->sgl[]에 위치.
 *      send_sgl[0].length = NVMe Cmd + descriptors 합계 → SEND가 캡슐 전체 송신.
 *
 * 호출 조건: payload_type=SGL + (ICD 불가).
 * 제약: 청크 수 ≤ ctrlr->max_sges, 각 청크 length ≤ MAX_KEYED_SGL_LENGTH.
 */
static inline int
nvme_rdma_build_sgl_request(struct nvme_rdma_qpair *rqpair,
			    struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;
	struct spdk_nvmf_cmd *cmd = &rqpair->cmds[rdma_req->id];	/* [한국어] descriptor 배열 보관 위치 */
	struct nvme_rdma_memory_translation_ctx ctx;
	uint32_t remaining_size;	/* [한국어] 아직 covering 안 된 페이로드 길이 */
	uint32_t sge_length;	/* [한국어] 한 청크 길이 */
	int rc, max_num_sgl, num_sgl_desc;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);	/* [한국어] 사용자 SGL 이터레이터 리셋 — payload_offset부터 시작 */

	max_num_sgl = req->qpair->ctrlr->max_sges;	/* [한국어] 컨트롤러 capability — descriptor 개수 상한 */

	remaining_size = req->payload_size;
	num_sgl_desc = 0;
	do {
		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &ctx.addr, &sge_length);	/* [한국어] 다음 청크 (addr, length) 가져오기 */
		if (spdk_unlikely(rc)) {
			return -1;
		}

		sge_length = spdk_min(remaining_size, sge_length);	/* [한국어] 남은 크기 또는 next_sge가 알려준 크기 중 작은 값 */

		if (spdk_unlikely(sge_length > NVME_RDMA_MAX_KEYED_SGL_LENGTH)) {
			NVME_RQPAIR_ERRLOG(rqpair, "SGL length %u exceeds max keyed SGL block size %u\n", sge_length,
					   NVME_RDMA_MAX_KEYED_SGL_LENGTH);
			return -1;	/* [한국어] 청크 1개가 16MB 이상 — 추가 분할 불가, 호출자가 스플릿 */
		}
		ctx.length = sge_length;
		rc = nvme_rdma_get_memory_translation(req, rqpair, &ctx);	/* [한국어] 청크별 rkey 획득 */
		if (spdk_unlikely(rc)) {
			return -1;
		}

		cmd->sgl[num_sgl_desc].keyed.key = ctx.rkey;	/* [한국어] descriptor i: rkey */
		cmd->sgl[num_sgl_desc].keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
		cmd->sgl[num_sgl_desc].keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
		cmd->sgl[num_sgl_desc].keyed.length = (uint32_t)ctx.length;
		cmd->sgl[num_sgl_desc].address = (uint64_t)ctx.addr;

		remaining_size -= ctx.length;
		num_sgl_desc++;
	} while (remaining_size > 0 && num_sgl_desc < max_num_sgl);	/* [한국어] 모두 소진 또는 descriptor 슬롯 한계 도달까지 */


	/* Should be impossible if we did our sgl checks properly up the stack, but do a sanity check here. */
	if (spdk_unlikely(remaining_size > 0)) {
		return -1;	/* [한국어] descriptor 슬롯 부족 — 상위 레이어가 max_sges를 검증했어야 */
	}

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;

	/* The RDMA SGL needs one element describing some portion
	 * of the spdk_nvmf_cmd structure. */
	rdma_req->send_wr.num_sge = 1;	/* [한국어] SEND는 항상 capsule 1개 (Cmd + 옵션 descriptors) */

	/*
	 * If only one SGL descriptor is required, it can be embedded directly in the command
	 * as a data block descriptor.
	 */
	if (num_sgl_desc == 1) {	/* [한국어] descriptor 1개 = build_contig와 동일 구조 (segment 추가 불필요) */
		/* The first element of this SGL is pointing at an
		 * spdk_nvmf_cmd object. For this particular command,
		 * we only need the first 64 bytes corresponding to
		 * the NVMe command. */
		rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);	/* [한국어] Cmd 64B만 송신 */

		req->cmd.dptr.sgl1.keyed.type = cmd->sgl[0].keyed.type;	/* [한국어] cmds[id]->sgl[0]을 dptr.sgl1로 인라인 복사 */
		req->cmd.dptr.sgl1.keyed.subtype = cmd->sgl[0].keyed.subtype;
		req->cmd.dptr.sgl1.keyed.length = cmd->sgl[0].keyed.length;
		req->cmd.dptr.sgl1.keyed.key = cmd->sgl[0].keyed.key;
		req->cmd.dptr.sgl1.address = cmd->sgl[0].address;
	} else {
		/*
		 * Otherwise, The SGL descriptor embedded in the command must point to the list of
		 * SGL descriptors used to describe the operation. In that case it is a last segment descriptor.
		 */
		/* [한국어] 여러 청크 → dptr.sgl1을 LAST_SEGMENT type으로 — descriptor 배열은 Cmd 직후에 위치 (캡슐 내). */
		uint32_t descriptors_size = sizeof(struct spdk_nvme_sgl_descriptor) * num_sgl_desc;	/* [한국어] descriptor 배열 총 크기 */

		if (spdk_unlikely(descriptors_size > rqpair->qpair.ctrlr->ioccsz_bytes)) {
			NVME_RQPAIR_ERRLOG(rqpair, "Size of SGL descriptors (%u) exceeds ICD (%u)\n", descriptors_size,
					   rqpair->qpair.ctrlr->ioccsz_bytes);
			return -1;	/* [한국어] 캡슐이 descriptor 배열 못 담음 */
		}
		rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd) + descriptors_size;	/* [한국어] SEND가 Cmd + descriptors 함께 송신 */

		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;	/* [한국어] 다음 segment 없음 — descriptor 배열이 페이로드 전체 */
		req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
		req->cmd.dptr.sgl1.unkeyed.length = descriptors_size;
		req->cmd.dptr.sgl1.address = (uint64_t)0;	/* [한국어] offset 0 = Cmd 끝 직후 */
	}

	return 0;
}

/*
 * Build inline SGL describing sgl payload buffer.
 */
/*
 * [한국어]
 * nvme_rdma_build_sgl_inline_request - SGL 페이로드의 첫 청크가 전체를 cover 하면 inline,
 *                                       아니면 build_sgl_request로 fallback.
 *
 * 동기/배경:
 *   사용자 SGL의 첫 청크가 충분히 크면 single-chunk 처럼 inline data로 처리 가능 → ICD 효과.
 *   첫 청크가 부족하면 keyed SGL 경로(build_sgl_request)로 빠짐.
 *
 * 호출 조건: payload_type=SGL + ICD 가능 (Write + size <= ioccsz_bytes + icdoff=0).
 */
static inline int
nvme_rdma_build_sgl_inline_request(struct nvme_rdma_qpair *rqpair,
				   struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;
	struct nvme_rdma_memory_translation_ctx ctx;
	uint32_t length;
	int rc;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);	/* [한국어] 이터레이터 리셋 */

	rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &ctx.addr, &length);	/* [한국어] 첫 청크 (addr,len) */
	if (spdk_unlikely(rc)) {
		return -1;
	}

	if (length < req->payload_size) {	/* [한국어] 첫 청크가 부족 → inline 불가, keyed SGL 경로로 우회 */
		NVME_RQPAIR_DEBUGLOG(rqpair, "Inline SGL request split so sending separately.\n");
		return nvme_rdma_build_sgl_request(rqpair, rdma_req);
	}

	if (length > req->payload_size) {
		length = req->payload_size;	/* [한국어] payload_size로 클램프 (next_sge가 더 큰 청크 알려준 경우) */
	}

	ctx.length = length;
	rc = nvme_rdma_get_memory_translation(req, rqpair, &ctx);	/* [한국어] lkey 변환 */
	if (spdk_unlikely(rc)) {
		return -1;
	}

	rdma_req->send_sgl[1].addr = (uint64_t)ctx.addr;	/* [한국어] SEND SGE[1] = inline 데이터 */
	rdma_req->send_sgl[1].length = (uint32_t)ctx.length;
	rdma_req->send_sgl[1].lkey = ctx.lkey;

	rdma_req->send_wr.num_sge = 2;	/* [한국어] Cmd + inline */

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);	/* [한국어] Cmd 64B */

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;	/* [한국어] unkeyed = ICD (RDMA_READ 불필요) */
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
	req->cmd.dptr.sgl1.unkeyed.length = (uint32_t)ctx.length;
	/* Inline only supported for icdoff == 0 currently.  This function will
	 * not get called for controllers with other values. */
	req->cmd.dptr.sgl1.address = (uint64_t)0;	/* [한국어] icdoff=0 가정 */

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_accel_append_copy - poll group의 accel_fn_table.append_copy 래퍼.
 *
 * UMR(가상 contig MR) 생성을 위해 IO 시퀀스에 copy task 추가 — accel framework이
 * scatter 페이로드를 타깃 메모리 도메인으로 복사 (또는 가상 매핑) 후 단일 lkey/rkey 발급.
 * append_copy는 비동기 — finish_sequence가 완료 콜백 호출.
 */
static inline int
nvme_rdma_accel_append_copy(struct spdk_nvme_poll_group *pg, void **seq,
			    struct spdk_memory_domain *rdma_domain, struct spdk_nvme_rdma_req *rdma_req,
			    struct iovec *iovs, uint32_t iovcnt,
			    struct spdk_memory_domain *src_domain, void *src_domain_ctx)
{
	return pg->accel_fn_table.append_copy(pg->ctx, seq, iovs, iovcnt, rdma_domain, rdma_req, iovs,
					      iovcnt, src_domain, src_domain_ctx, NULL, NULL);	/* [한국어] dst=src=iovs (in-place) — UMR 매핑만 만드는 의미 */
}

/*
 * [한국어]
 * nvme_rdma_accel_reverse - 시퀀스 방향 뒤집기 (Read의 경우 dst→src 방향으로).
 *
 * Read I/O는 타깃 → 호스트로 데이터 흐름이지만 accel 시퀀스는 src→dst 방향이 디폴트 →
 * 명시적으로 reverse 호출해 일관성 유지.
 */
static inline void
nvme_rdma_accel_reverse(struct spdk_nvme_poll_group *pg, void *seq)
{
	pg->accel_fn_table.reverse_sequence(seq);	/* [한국어] 시퀀스 방향 뒤집기 — accel framework 콜 */
}

/*
 * [한국어]
 * nvme_rdma_accel_finish - accel 시퀀스 실행 시작 + 완료 콜백 등록.
 *
 * append_copy로 큐잉된 task들이 실제로 실행되며, 결과는 cb_fn으로 알림.
 * cb_fn은 완료 시 nvme_rdma_accel_completion_cb로 IO를 다음 단계(데이터 transfer)로 진행.
 */
static inline void
nvme_rdma_accel_finish(struct spdk_nvme_poll_group *pg, void *seq,
		       spdk_nvme_accel_completion_cb cb_fn, void *cb_arg)
{
	pg->accel_fn_table.finish_sequence(seq, cb_fn, cb_arg);	/* [한국어] 시퀀스 실행 — 비동기, 완료 시 cb_fn 호출 */
}

/*
 * [한국어]
 * nvme_rdma_accel_completion_cb - accel 시퀀스 완료 시 호출되는 콜백.
 *
 * @cb_arg: spdk_nvme_rdma_req (append_copy 시 등록).
 * @status: 시퀀스 실행 결과.
 *
 * 동작:
 *   1) accel 활성 카운터 감소 + in_progress 플래그 클리어.
 *   2) 큐페어 disconnect 진행 중이면 ABORTED_SQ_DELETION으로 합성 cpl 생성.
 *   3) 정상이면 nvme_rdma_req_complete로 정상 완료 처리.
 *   4) 오류 시 INTERNAL_DEVICE_ERROR + dnr=1 (재시도 부적절).
 */
static inline void
nvme_rdma_accel_completion_cb(void *cb_arg, int status)
{
	struct spdk_nvme_rdma_req *rdma_req = cb_arg;
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(rdma_req->req->qpair);
	struct spdk_nvme_cpl cpl;
	enum spdk_nvme_generic_command_status_code sc;
	uint16_t dnr = 0;	/* [한국어] Do Not Retry — 1이면 상위 레이어가 재시도 안 함 */

	assert(rqpair->num_active_accel_reqs);
	rqpair->num_active_accel_reqs--;
	rdma_req->in_progress_accel = 0;
	rdma_req->req->accel_sequence = NULL;
	NVME_RQPAIR_DEBUGLOG(rqpair, "rdma_req %p, accel completion rc %d\n", rdma_req, status);

	/* nvme_rdma driver may fail data transfer on WC_FLUSH error completion which is expected.
	 * To prevent false errors from accel, first check if qpair is in the process of disconnect */
	if (spdk_unlikely(!spdk_nvme_qpair_is_connected(&rqpair->qpair))) {
		struct spdk_nvmf_fabric_connect_cmd *cmd = (struct spdk_nvmf_fabric_connect_cmd *)
				&rdma_req->req->cmd;

		if (cmd->opcode != SPDK_NVME_OPC_FABRIC && cmd->fctype != SPDK_NVMF_FABRIC_COMMAND_CONNECT) {
			NVME_RQPAIR_DEBUGLOG(rqpair, "req %p accel cpl in disconnecting, outstanding %u\n", rdma_req,
					     rqpair->qpair.num_outstanding_reqs);
			sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
			goto fail_req;
		}
	}
	if (spdk_unlikely(status)) {
		NVME_RQPAIR_ERRLOG(rqpair, "req %p, accel sequence status %d\n", rdma_req, status);
		sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		/* Something wrong happened, let the upper layer know that retry is no desired */
		dnr = 1;
		goto fail_req;
	}

	nvme_rdma_req_complete(rdma_req, &rdma_req->cpl, true);
	return;

fail_req:
	memset(&cpl, 0, sizeof(cpl));
	cpl.status.sc = sc;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = dnr;
	nvme_rdma_req_complete(rdma_req, &cpl, true);
}

/*
 * [한국어]
 * nvme_rdma_apply_accel_sequence - I/O 페이로드를 accel framework + UMR 경로로 처리.
 *
 * @rqpair: 큐페어.
 * @req: NVMe 요청.
 * @rdma_req: RDMA 요청 슬롯.
 * @return: 0=비동기 시작 성공, 음수=실패.
 *
 * 동기/배경 — UMR (User-Mode Memory Region):
 *   Mellanox HCA의 가상 contig MR 기능 — scatter 페이로드(여러 청크)를 단일 가상주소+key로 표현 가능.
 *   accel framework의 append_copy로 가상 매핑을 만들고, 이후 RDMA 트랜스포트는 단일 SGL로 IO 발행.
 *   I/O마다 새 가상 MR을 만드는 비용이 있지만 multi-segment SGL 사용 안 해도 되어 데이터 경로 단순화.
 *
 * 동작:
 *   1) 페이로드를 iovs[]로 변환 (CONTIG는 1개, SGL은 next_sge 루프).
 *   2) memory_domain 설정에 따라 src_domain 결정.
 *   3) accel_append_copy → accel framework이 UMR 생성을 위한 task 큐잉.
 *   4) Read의 경우 reverse_sequence로 방향 뒤집기.
 *   5) accel_finish로 시퀀스 실행 시작 — 완료 시 accel_completion_cb → memory_domain_transfer_data 경유.
 */
static inline int
nvme_rdma_apply_accel_sequence(struct nvme_rdma_qpair *rqpair, struct nvme_request *req,
			       struct spdk_nvme_rdma_req *rdma_req)
{
	struct spdk_nvme_poll_group *pg = rqpair->qpair.poll_group->group;	/* [한국어] accel_fn_table 보유한 SPDK poll group */
	struct spdk_memory_domain *src_domain;
	void *src_domain_ctx;
	void *accel_seq = req->accel_sequence;
	uint32_t iovcnt = 0;
	int rc;

	NVME_RQPAIR_DEBUGLOG(rqpair, "req %p, start accel seq %p\n", rdma_req, accel_seq);
	if (nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL) {
		void *addr;
		uint32_t sge_length, payload_size;

		payload_size = req->payload_size;
		assert(payload_size);
		req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);
		do {
			rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &addr, &sge_length);
			if (spdk_unlikely(rc)) {
				return -1;
			}
			sge_length = spdk_min(payload_size, sge_length);
			rdma_req->iovs[iovcnt].iov_base = addr;
			rdma_req->iovs[iovcnt].iov_len = sge_length;
			iovcnt++;
			payload_size -= sge_length;
		} while (payload_size && iovcnt < NVME_RDMA_MAX_SGL_DESCRIPTORS);

		if (spdk_unlikely(payload_size)) {
			NVME_RQPAIR_ERRLOG(rqpair, "not enough iovs to handle req %p, remaining len %u\n", rdma_req,
					   payload_size);
			return -E2BIG;
		}
	} else {
		rdma_req->iovs[iovcnt].iov_base = req->payload.contig_or_cb_arg;
		rdma_req->iovs[iovcnt].iov_len = req->payload_size;
		iovcnt = 1;
	}
	if (req->payload.opts && req->payload.opts->memory_domain) {
		if (accel_seq) {
			src_domain = rqpair->rdma_qp->domain;
			src_domain_ctx = rdma_req;
		} else {
			src_domain = req->payload.opts->memory_domain;
			src_domain_ctx = req->payload.opts->memory_domain_ctx;
		}
	} else {
		src_domain = NULL;
		src_domain_ctx = NULL;
	}

	rc = nvme_rdma_accel_append_copy(pg, &accel_seq, rqpair->rdma_qp->domain, rdma_req, rdma_req->iovs,
					 iovcnt, src_domain, src_domain_ctx);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	if (spdk_nvme_opc_get_data_transfer(req->cmd.opc) == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		nvme_rdma_accel_reverse(pg, accel_seq);
	}

	rdma_req->in_progress_accel = 1;
	TAILQ_INSERT_TAIL(&rqpair->outstanding_reqs, rdma_req, link);
	rqpair->num_outstanding_reqs++;
	rqpair->num_active_accel_reqs++;

	NVME_RQPAIR_DEBUGLOG(rqpair, "req %p, finish accel seq %p\n", rdma_req, accel_seq);
	nvme_rdma_accel_finish(pg, accel_seq, nvme_rdma_accel_completion_cb, rdma_req);

	return 0;
}

static inline int
nvme_rdma_memory_domain_transfer_data(struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				      struct iovec *dst_iov, uint32_t dst_iovcnt,
				      struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				      struct iovec *src_iov, uint32_t src_iovcnt,
				      struct spdk_memory_domain_translation_result *translation,
				      spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	struct nvme_rdma_memory_translation_ctx ctx;
	struct spdk_nvme_rdma_req *rdma_req = dst_domain_ctx;
	struct nvme_request *req = rdma_req->req;
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(rdma_req->req->qpair);
	struct spdk_nvme_ctrlr *ctrlr = rqpair->qpair.ctrlr;
	bool icd_supported;

	assert(dst_domain == rqpair->rdma_qp->domain);
	assert(src_domain);
	assert(spdk_memory_domain_get_dma_device_type(src_domain) == SPDK_DMA_DEVICE_TYPE_RDMA);
	/* We expect "inplace" operation */
	assert(dst_iov == src_iov);
	assert(dst_iovcnt == src_iovcnt);

	if (spdk_unlikely(!src_domain ||
			  spdk_memory_domain_get_dma_device_type(src_domain) != SPDK_DMA_DEVICE_TYPE_RDMA)) {
		NVME_RQPAIR_ERRLOG(rqpair, "Unexpected source memory domain %p, type %d\n", src_domain,
				   src_domain ? (int)spdk_memory_domain_get_dma_device_type(src_domain) : -1);
		return -ENOTSUP;
	}
	if (spdk_unlikely(dst_iovcnt != 1 || !translation || translation->iov_count != 1)) {
		NVME_RQPAIR_ERRLOG(rqpair, "Unexpected iovcnt %u or missed translation, rdma_req %p\n", dst_iovcnt,
				   rdma_req);
		return -ENOTSUP;
	}
	ctx.addr = translation->iov.iov_base;
	ctx.length = translation->iov.iov_len;
	ctx.lkey = translation->rdma.lkey;
	ctx.rkey = translation->rdma.rkey;

	NVME_RQPAIR_DEBUGLOG(rqpair, "req %p, addr %p, len %zu, key %u\n", rdma_req, ctx.addr, ctx.length,
			     ctx.rkey);
	icd_supported = spdk_nvme_opc_get_data_transfer(req->cmd.opc) == SPDK_NVME_DATA_HOST_TO_CONTROLLER
			&& req->payload_size <= ctrlr->ioccsz_bytes && ctrlr->icdoff == 0;

	/* We expect that result of accel sequence is a Memory Key which describes a virtually contig address space.
	 * That means we prepare a contig request even if original payload was scattered */
	if (icd_supported) {
		nvme_rdma_configure_contig_inline_request(rdma_req, req, &ctx);
	} else {
		nvme_rdma_configure_contig_request(rdma_req, req, &ctx);
	}
	rdma_req->transfer_cpl_cb = cpl_cb;
	rdma_req->transfer_cpl_cb_arg = cpl_cb_arg;

	memcpy(&rqpair->cmds[rdma_req->id], &req->cmd, sizeof(req->cmd));

	return _nvme_rdma_qpair_submit_request(rqpair, rdma_req);
}

/*
 * [한국어]
 * nvme_rdma_req_init - I/O 요청을 페이로드 형태와 ICD 가용성에 따라 적절한 build_*_request로 분기.
 *
 * @rqpair: 큐페어 (mr_map 사용).
 * @rdma_req: 빌드 대상 RDMA 요청 슬롯 (req 필드는 이미 채워져 있어야).
 * @return: 0=성공, 음수=빌드 실패 (메모리 변환/SGL 길이 초과 등).
 *
 * 동기/배경 — 4종 빌드 경로 디스패치:
 *   페이로드 길이/형태/Cmd 방향/컨트롤러의 ICD 지원 여부에 따라 5가지 SEND WR 구조 중 하나를 선택:
 *     a) 페이로드 없음 → build_null_request (Identify 등)
 *     b) CONTIG + ICD 가능 (Write && size <= ioccsz && icdoff=0) → build_contig_inline_request (SGE 2개)
 *     c) CONTIG + ICD 불가 → build_contig_request (keyed SGL, 타깃이 RDMA_READ/WRITE)
 *     d) SGL + ICD 가능 → build_sgl_inline_request (첫 청크가 전체이면 inline, 아니면 multi-segment)
 *     e) SGL + ICD 불가 → build_sgl_request (multi-segment keyed SGL)
 *   ICD 조건: HOST_TO_CONTROLLER(Write 계열) AND payload_size ≤ ioccsz_bytes AND icdoff==0.
 *   Read는 항상 keyed SGL (타깃이 RDMA_WRITE로 데이터 전달).
 *
 * 마지막 단계: req->cmd(상위 NVMe 레이어가 채운 64B Cmd)을 cmds[id]로 복사 → SEND WR이 그 주소에서 메시지 송신.
 *
 * 호출 체인:
 *   nvme_rdma_qpair_submit_request → [nvme_rdma_req_init] → build_*_request → memcpy(cmds[id], req->cmd)
 */
static inline int
nvme_rdma_req_init(struct nvme_rdma_qpair *rqpair, struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;	/* [한국어] 상위 NVMe 요청 (cmd/payload 보유) */
	struct spdk_nvme_ctrlr *ctrlr = rqpair->qpair.ctrlr;	/* [한국어] ICD 능력(ioccsz/icdoff) 조회용 */
	enum nvme_payload_type payload_type;	/* [한국어] CONTIG/SGL/CB 분기 */
	bool icd_supported;	/* [한국어] in-capsule data 가용 여부 */
	int rc = -1;

	payload_type = nvme_payload_type(&req->payload);	/* [한국어] req->payload.next_sge_fn 유무로 판정 */
	/*
	 * Check if icdoff is non zero, to avoid interop conflicts with
	 * targets with non-zero icdoff.  Both SPDK and the Linux kernel
	 * targets use icdoff = 0.  For targets with non-zero icdoff, we
	 * will currently just not use inline data for now.
	 */
	/* [한국어] ICD 가능 조건 (NVMe-oF Spec 7.4.5):
	 *  - 데이터 방향이 호스트→컨트롤러 (Write 계열) — Read는 RDMA_WRITE로 받아야 하므로 ICD 불가.
	 *  - 페이로드 ≤ ioccsz_bytes (컨트롤러가 광고한 최대 ICD 크기, Identify CDATA의 ioccsz * 16바이트).
	 *  - icdoff == 0 (Cmd Capsule 끝 직후가 데이터 시작) — 비-제로 icdoff 타깃은 SPDK 미지원. */
	icd_supported = spdk_nvme_opc_get_data_transfer(req->cmd.opc) == SPDK_NVME_DATA_HOST_TO_CONTROLLER
			&& req->payload_size <= ctrlr->ioccsz_bytes && ctrlr->icdoff == 0;

	if (spdk_unlikely(req->payload_size == 0)) {
		rc = nvme_rdma_build_null_request(rdma_req);	/* [한국어] (a) 데이터 없음 — Identify 등 */
	} else if (payload_type == NVME_PAYLOAD_TYPE_CONTIG) {
		if (icd_supported) {
			rc = nvme_rdma_build_contig_inline_request(rqpair, rdma_req);	/* [한국어] (b) 작은 Write inline */
		} else {
			rc = nvme_rdma_build_contig_request(rqpair, rdma_req);	/* [한국어] (c) keyed SGL 1개 */
		}
	} else if (payload_type == NVME_PAYLOAD_TYPE_SGL) {
		if (icd_supported) {
			rc = nvme_rdma_build_sgl_inline_request(rqpair, rdma_req);	/* [한국어] (d) 첫 청크가 전체면 inline */
		} else {
			rc = nvme_rdma_build_sgl_request(rqpair, rdma_req);	/* [한국어] (e) multi-segment keyed SGL */
		}
	}

	if (spdk_unlikely(rc)) {
		return rc;	/* [한국어] 빌드 실패 시 호출자가 req_put + -1 반환 */
	}

	memcpy(&rqpair->cmds[rdma_req->id], &req->cmd, sizeof(req->cmd));	/* [한국어] NVMe Cmd 64B를 등록된 buffer로 복사 — SEND가 이 buffer에서 송신 */
	return 0;
}

/*
 * [한국어]
 * nvme_rdma_ctrlr_create_qpair - 큐페어 컨테이너 할당 + 일반 nvme_qpair_init 위임 (ops.ctrlr_create_io_qpair 백엔드).
 *
 * @ctrlr: 부모 컨트롤러.
 * @qid: 큐페어 ID (0=admin, 1+=I/O).
 * @qsize: 큐 사이즈 (스펙상 N개 엔트리).
 * @qprio: I/O 우선순위 (NVMe weighted round robin).
 * @num_requests: 동시 처리 가능 요청 수.
 * @delay_cmd_submit: SEND doorbell 지연 모드 (배치 효과).
 * @async: 비동기 connect/disconnect 사용 여부.
 * @return: 임베드된 spdk_nvme_qpair 포인터 또는 NULL.
 *
 * 동작:
 *   1) qsize 검증 (최소 SPDK_NVME_QUEUE_MIN_ENTRIES).
 *   2) nvme_rdma_qpair 컨테이너 할당 (DMA 가능 메모리 — 자체가 RDMA 메타데이터 보유).
 *   3) num_entries = qsize - 1 (NVMe Spec 4.1: 큐의 1 슬롯은 항상 비워둠 — full/empty 구분).
 *   4) append_copy 결정: rdma_umr_per_io 옵션 + accel 지원 + I/O 큐(qid != 0)인 경우 자동 UMR 모드.
 *   5) nvme_qpair_init: 일반 NVMe 큐페어 초기화 (상태머신, queued_req 큐 등).
 *
 * 호출 체인: ctrlr_construct (admin), ctrlr_create_io_qpair (I/O) → [nvme_rdma_ctrlr_create_qpair]
 */
static struct spdk_nvme_qpair *
nvme_rdma_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr,
			     uint16_t qid, uint32_t qsize,
			     enum spdk_nvme_qprio qprio,
			     uint32_t num_requests,
			     bool delay_cmd_submit,
			     bool async)
{
	struct nvme_rdma_qpair *rqpair;	/* [한국어] 컨테이너 — qpair 임베드 */
	struct spdk_nvme_qpair *qpair;	/* [한국어] 임베드된 일반 qpair (반환 대상) */
	int rc;

	if (qsize < SPDK_NVME_QUEUE_MIN_ENTRIES) {	/* [한국어] NVMe 스펙 최소(2) — 너무 작으면 1슬롯 빈공간 정책으로 사용 불가 */
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to create qpair with size %u. Minimum queue size is %d.\n",
				  qsize, SPDK_NVME_QUEUE_MIN_ENTRIES);
		return NULL;
	}

	rqpair = spdk_zmalloc(sizeof(struct nvme_rdma_qpair), 0, NULL, SPDK_ENV_NUMA_ID_ANY,
			      SPDK_MALLOC_DMA);	/* [한국어] DMA 메모리 — 컨테이너 자체에 send_sgl/cmds[]가 직접 들어가진 않지만 hugepage가 캐시 lock 효과 */
	if (!rqpair) {
		NVME_CTRLR_ERRLOG(ctrlr, "failed to get create rqpair\n");
		return NULL;
	}

	/* Set num_entries one less than queue size. According to NVMe
	 * and NVMe-oF specs we can not submit queue size requests,
	 * one slot shall always remain empty.
	 */
	/* [한국어] NVMe Spec 4.1: head==tail이 empty, head==(tail+1)%size가 full → 동시에 outstanding 가능한 최대는 size-1.
	 * 따라서 num_entries = qsize - 1로 맞춰야 SQ overflow 회피. */
	rqpair->num_entries = qsize - 1;
	rqpair->delay_cmd_submit = delay_cmd_submit;	/* [한국어] true면 _submit_request가 doorbell 미발사 → submit_sends에서 일괄 */
	rqpair->state = NVME_RDMA_QPAIR_STATE_INVALID;	/* [한국어] connect_qpair 호출 전 단계 */
	rqpair->append_copy = g_spdk_nvme_transport_opts.rdma_umr_per_io &&
			      spdk_rdma_provider_accel_sequence_supported() && qid != 0;	/* [한국어] UMR 자동 적용: 옵션 켜짐 + provider 지원 + I/O 큐(admin은 메타데이터라 UMR 의미 없음) */
	qpair = &rqpair->qpair;
	rc = nvme_qpair_init(qpair, qid, ctrlr, qprio, num_requests, async);	/* [한국어] 일반 NVMe 큐페어 초기화 — queued_req 큐, 상태=READY 등 */
	if (rc != 0) {
		spdk_free(rqpair);	/* [한국어] init 실패 시 컨테이너 해제 */
		return NULL;
	}

	NVME_RQPAIR_DEBUGLOG(rqpair, "append_copy %s\n", rqpair->append_copy ? "enabled" : "disabled");
	return qpair;
}

/*
 * [한국어]
 * nvme_rdma_qpair_destroy - 큐페어의 모든 RDMA 자원을 안전한 순서로 해제.
 *
 * @rqpair: 해제 대상 (이미 EXITED 상태이거나 강제 정리 경로).
 *
 * 해제 순서가 중요한 이유 (verbs 의존성):
 *   1) MR 풀 (mr_map) — 이 큐페어가 보유한 메모리 등록 정보. PD/QP보다 먼저.
 *   2) 미처리 CM 이벤트 ack (rqpair->evt + pending_cm_events 중 본인 것).
 *   3) RDMA QP 파괴 + PD 참조 카운트 감소.
 *   4) CQ 해제: poll group 모드 → poller 참조 감소(공유), 단독 모드 → ibv_destroy_cq.
 *   5) SEND/RECV 풀 해제 (cmds[], rdma_reqs[], rsps).
 *   6) 마지막으로 cm_id 해제 — librdmacm device가 free되면 verbs 자원도 invalid 되므로 가장 나중.
 *
 * 동기화: ctrlr_lock 보호하에 호출 (pending_cm_events 순회 안전).
 */
static void
nvme_rdma_qpair_destroy(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;
	struct nvme_rdma_ctrlr *rctrlr;
	struct nvme_rdma_cm_event_entry *entry, *tmp;

	spdk_rdma_utils_free_mem_map(&rqpair->mr_map);	/* [한국어] (1) MR 풀 — ibv_dereg_mr들 호출됨 */

	if (rqpair->evt) {
		rdma_ack_cm_event(rqpair->evt);	/* [한국어] (2a) 큐페어가 보유 중이던 CM 이벤트 ack */
		rqpair->evt = NULL;
	}

	/*
	 * This works because we have the controller lock both in
	 * this function and in the function where we add new events.
	 */
	if (qpair->ctrlr != NULL) {
		rctrlr = nvme_rdma_ctrlr(qpair->ctrlr);
		STAILQ_FOREACH_SAFE(entry, &rctrlr->pending_cm_events, link, tmp) {	/* [한국어] (2b) pending에서 자신 소유의 이벤트들 회수 */
			if (entry->evt->id->context == rqpair) {	/* [한국어] cm_id->context로 자신의 이벤트 식별 */
				STAILQ_REMOVE(&rctrlr->pending_cm_events, entry, nvme_rdma_cm_event_entry, link);
				rdma_ack_cm_event(entry->evt);	/* [한국어] librdmacm에 ack — 그렇지 않으면 누수 */
				STAILQ_INSERT_HEAD(&rctrlr->free_cm_events, entry, link);	/* [한국어] entry 슬롯은 free 풀로 */
			}
		}
	}

	if (rqpair->cm_id) {
		if (rqpair->rdma_qp) {
			spdk_rdma_utils_put_pd(rqpair->rdma_qp->qp->pd);	/* [한국어] (3a) PD ref-- (캐시 PD라면 공유) */
			spdk_rdma_provider_qp_destroy(rqpair->rdma_qp);	/* [한국어] (3b) ibv_destroy_qp + provider 메타 해제 */
			rqpair->rdma_qp = NULL;
		}
	}

	if (rqpair->poller) {
		nvme_rdma_qpair_release_poller(rqpair);	/* [한국어] (4a) 공유 CQ — refcnt-- (0이면 poller destroy) */

		if (rqpair->srq) {
			rqpair->srq = NULL;	/* [한국어] SRQ 모드에서는 풀이 공유 → 큐페어가 free 안 함 */
			rqpair->rsps = NULL;
		}
	} else if (rqpair->cq) {
		ibv_destroy_cq(rqpair->cq);	/* [한국어] (4b) 단독 모드 CQ 해제 — verbs API */
		rqpair->cq = NULL;
	}

	nvme_rdma_free_reqs(rqpair);	/* [한국어] (5a) cmds[N]+rdma_reqs[N] 해제 */
	nvme_rdma_free_rsps(rqpair->rsps);	/* [한국어] (5b) 응답 풀 해제 (NULL 안전) */
	rqpair->rsps = NULL;

	/* destroy cm_id last so cma device will not be freed before we destroy the cq. */
	/* [한국어] (6) cm_id 마지막 — librdmacm은 cm_id 파괴 시 device(=verbs context)도 정리하므로
	 * CQ/QP 보다 늦게 해제해야 use-after-free 방지. */
	if (rqpair->cm_id) {
		rdma_destroy_id(rqpair->cm_id);
		rqpair->cm_id = NULL;
	}
}

static void nvme_rdma_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr);	/* [한국어] 전방 선언 — disconnected 경로에서 호출 */

/*
 * [한국어]
 * nvme_rdma_qpair_flush_send_wrs - 큐잉되어있는 SEND WR을 강제로 doorbell.
 *
 * 큐페어 종료 직전 미발사 WR을 정리해 SEND 카운터/큐 상태를 일관성 있게 만들기 위함.
 * 실패해도 reset_failed_sends로 카운터만 롤백 — 어차피 disconnect 진행 중이므로 무시 가능.
 */
static void
nvme_rdma_qpair_flush_send_wrs(struct nvme_rdma_qpair *rqpair)
{
	struct ibv_send_wr *bad_wr = NULL;
	int rc;

	rc = spdk_rdma_provider_qp_flush_send_wrs(rqpair->rdma_qp, &bad_wr);	/* [한국어] 큐잉된 WR을 ibv_post_send로 발사 */
	if (rc) {
		nvme_rdma_reset_failed_sends(rqpair, bad_wr);	/* [한국어] 실패한 WR은 카운터에서 차감 */
	}
}

/*
 * [한국어]
 * nvme_rdma_finish_outstanding_accel_transfers - disconnect 시 진행 중인 accel 시퀀스 강제 완료.
 *
 * accel 시퀀스는 외부 framework에서 처리되어 RDMA 트랜스포트가 강제 중단할 수 없음 →
 * transfer_cpl_cb가 등록된 요청들에 -ENXIO로 완료 신호 보내 framework가 정리하도록 함.
 */
static inline void
nvme_rdma_finish_outstanding_accel_transfers(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_req *req, *tmp;

	TAILQ_FOREACH_SAFE(req, &rqpair->outstanding_reqs, link, tmp) {
		if (req->in_progress_accel && req->transfer_cpl_cb) {	/* [한국어] accel 진행 중 + 콜백 보유 — 정리 대상 */
			nvme_rdma_finish_data_transfer(req, -ENXIO);	/* [한국어] -ENXIO 통지 → accel framework가 시퀀스 정리 */
		}
	}
}

/*
 * [한국어]
 * nvme_rdma_qpair_disconnected - DISCONNECTED 이벤트 콜백 (또는 verbs 직접 disconnect 후 호출).
 *
 * @rqpair: 끊긴 큐페어.
 * @ret: validate_cm_event 결과 (0=정상 disconnect, 그 외=타깃이 응답 안 함).
 * @return: 0=완전 정리 완료, -EAGAIN=LINGERING 진입.
 *
 * 동작:
 *   1) 미발사 SEND WR flush → in-flight 카운터 일관성 유지.
 *   2) accel 시퀀스 진행 중이면 강제 완료 + LINGERING 진입 (accel 완료 대기).
 *   3) ret != 0 (타깃 무응답)면 quiet로 직진 — 어차피 정상 종료 못 받음.
 *   4) poller/rsps NULL이면 즉시 quiet (자원 적음).
 *   5) need_destroy(DEVICE_REMOVAL) 또는 미완료 send/recv가 있으면 LINGERING (timeout 대기).
 *   6) quiet 도달 시 EXITED 전이 + abort_reqs (남은 모든 요청에 ABORTED_SQ_DELETION cpl) + qpair_destroy + 상위 알림.
 *
 * LINGERING 의미: rdma_disconnect 후 in-flight WR이 IBV_WC_WR_FLUSH_ERR로 회수될 때까지 대기 — 그 후 자원 해제.
 *
 * 호출 체인:
 *   _nvme_rdma_ctrlr_disconnect_qpair → process_event_start(DISCONNECTED, 본함수)
 *     또는 verbs disconnect 실패 시 직접 호출.
 */
static int
nvme_rdma_qpair_disconnected(struct nvme_rdma_qpair *rqpair, int ret)
{
	if (rqpair->rdma_qp != NULL) {
		nvme_rdma_qpair_flush_send_wrs(rqpair);	/* [한국어] 큐잉된 WR 일괄 발사 — QP가 ERR로 가면 곧바로 FLUSH_ERR로 회수됨 */
	}

	if (rqpair->num_active_accel_reqs != 0) {
		SPDK_DEBUGLOG(nvme, "qp %p has %u accel requests\n", rqpair, rqpair->num_active_accel_reqs);
		nvme_rdma_finish_outstanding_accel_transfers(rqpair);	/* [한국어] accel framework에 -ENXIO 통지 */
		goto lingering;	/* [한국어] accel은 외부 비동기 — 완료 대기 필요 */
	}

	if (ret) {
		SPDK_DEBUGLOG(nvme, "Target did not respond to qpair disconnect.\n");
		goto quiet;	/* [한국어] 타깃 무응답 — 더 기다려도 의미 없음 */
	}

	if (rqpair->poller == NULL) {
		/* If poller is not used, cq is not shared.
		 * So complete disconnecting qpair immediately.
		 */
		/* [한국어] 단독 CQ 모드는 다른 큐페어 영향 없음 → 즉시 정리 가능 */
		goto quiet;
	}

	if (rqpair->rsps == NULL) {
		goto quiet;	/* [한국어] 응답 풀 미생성 (FABRIC_CONNECT 전에 실패한 경우) — 회수할 RECV WR 없음 */
	}

	if (rqpair->need_destroy ||	/* [한국어] DEVICE_REMOVAL 등 — verbs 호출 자체가 위험 */
	    (rqpair->current_num_sends != 0 ||	/* [한국어] in-flight SEND 남음 */
	     (!rqpair->srq && rqpair->rsps->current_num_recvs != 0)) ||	/* [한국어] in-flight RECV 남음 (SRQ 모드는 공유라 무시) */
	    ((rqpair->qpair.ctrlr->flags & SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED) &&
	     (!TAILQ_EMPTY(&rqpair->outstanding_reqs)))) {	/* [한국어] accel 모드는 outstanding 비울 때까지 대기 */
lingering:
		rqpair->state = NVME_RDMA_QPAIR_STATE_LINGERING;
		rqpair->evt_timeout_ticks = (NVME_RDMA_DISCONNECTED_QPAIR_TIMEOUT_US * spdk_get_ticks_hz()) /
					    SPDK_SEC_TO_USEC + spdk_get_ticks();	/* [한국어] 1초 후 강제 정리 데드라인 */

		return -EAGAIN;	/* [한국어] disconnect_qpair_poll가 wait_until_quiet으로 진행 */
	}

quiet:
	rqpair->state = NVME_RDMA_QPAIR_STATE_EXITED;	/* [한국어] 안전하게 free 가능 */

	nvme_rdma_qpair_abort_reqs(&rqpair->qpair, rqpair->qpair.abort_dnr);	/* [한국어] 남은 모든 요청에 ABORTED_SQ_DELETION cpl */
	assert(TAILQ_EMPTY(&rqpair->outstanding_reqs));	/* [한국어] abort 후 outstanding 빔 — 가드 */
	nvme_rdma_qpair_destroy(rqpair);	/* [한국어] verbs 자원 + cm_id 해제 */
	nvme_transport_ctrlr_disconnect_qpair_done(&rqpair->qpair);	/* [한국어] 상위 NVMe 레이어에 disconnect 완료 알림 */

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_qpair_wait_until_quiet - LINGERING 상태에서 in-flight WR이 모두 완료될 때까지 대기.
 *
 * @rqpair: LINGERING 큐페어.
 * @return: 0=quiet 도달 + 정리 완료, -EAGAIN=아직 미완료 (다음 폴 사이클에 다시).
 *
 * 정상 시나리오: rdma_disconnect 후 QP가 ERR 상태 → in-flight WR이 IBV_WC_WR_FLUSH_ERR로 빠르게 회수 →
 *               current_num_sends/current_num_recvs가 0이 됨 → quiet 도달.
 * 비정상: 타깃 응답 안 옴 + ERR 전이 안 됨 → 1초 timeout(NVME_RDMA_DISCONNECTED_QPAIR_TIMEOUT_US) 도달 시 강제 정리.
 *
 * I/O 큐페어는 ctrlr_lock으로 nvme_rdma_qpair_destroy를 보호 (CM 이벤트 처리와 직렬화).
 * Admin 큐페어는 트랜스포트 코드가 이미 락을 들고 호출하므로 추가 락 불필요.
 */
static int
nvme_rdma_qpair_wait_until_quiet(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;

	if (rqpair->num_active_accel_reqs != 0) {
		nvme_rdma_finish_outstanding_accel_transfers(rqpair);	/* [한국어] accel 진행 중이면 매 폴마다 finish 시도 */
		return -EAGAIN;
	}

	if (spdk_get_ticks() < rqpair->evt_timeout_ticks &&
	    (rqpair->current_num_sends != 0 ||
	     (!rqpair->srq && rqpair->rsps->current_num_recvs != 0))) {
		return -EAGAIN;	/* [한국어] timeout 안 지났고 in-flight 남음 → 더 기다림 */
	}

	rqpair->state = NVME_RDMA_QPAIR_STATE_EXITED;	/* [한국어] timeout 또는 모두 회수 — 정리 진행 */
	nvme_rdma_qpair_abort_reqs(qpair, qpair->abort_dnr);	/* [한국어] 남은 요청 abort */
	assert(TAILQ_EMPTY(&rqpair->outstanding_reqs));
	if (!nvme_qpair_is_admin_queue(qpair)) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);	/* [한국어] I/O 큐는 명시적 락 필요 */
	}
	nvme_rdma_qpair_destroy(rqpair);	/* [한국어] verbs 자원 + cm_id 해제 */
	if (!nvme_qpair_is_admin_queue(qpair)) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	nvme_transport_ctrlr_disconnect_qpair_done(&rqpair->qpair);	/* [한국어] 상위 알림 */

	return 0;
}

/*
 * [한국어]
 * _nvme_rdma_ctrlr_disconnect_qpair - 큐페어 disconnect 시퀀스의 시작 (rdma_disconnect 발사 + 콜백 등록).
 *
 * @ctrlr: 컨트롤러.
 * @qpair: 끊을 큐페어.
 * @disconnected_qpair_cb: DISCONNECTED 이벤트 도착 시 호출할 콜백 (qpair_disconnected 또는 stale_conn_disconnected).
 *
 * 동작:
 *   1) state = EXITING.
 *   2) connected 큐페어면 spdk_rdma_provider_qp_disconnect (= rdma_disconnect 또는 ibv_modify_qp(ERR)) 호출.
 *   3) DISCONNECTED 이벤트 콜백 등록.
 *   4) 발사 실패하거나 비활성 큐페어면 콜백을 즉시 호출 (동기 경로).
 *
 * 호출 체인: ctrlr_disconnect_qpair (vtable), stale_conn_retry → [_nvme_rdma_ctrlr_disconnect_qpair]
 */
static void
_nvme_rdma_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				  nvme_rdma_cm_event_cb disconnected_qpair_cb)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	int rc;

	assert(disconnected_qpair_cb != NULL);

	rqpair->state = NVME_RDMA_QPAIR_STATE_EXITING;	/* [한국어] disconnect 진행 중 — 신규 IO 차단 */

	if (rqpair->cm_id) {
		if (rqpair->rdma_qp && rqpair->connected) {	/* [한국어] ESTABLISHED 도달했던 큐페어만 정상 disconnect */
			rc = spdk_rdma_provider_qp_disconnect(rqpair->rdma_qp);	/* [한국어] librdmacm rdma_disconnect 또는 ibv_modify_qp(ERR) — provider 추상 */
			if ((qpair->ctrlr != NULL) && (rc == 0)) {
				rc = nvme_rdma_process_event_start(rqpair, RDMA_CM_EVENT_DISCONNECTED,
								   disconnected_qpair_cb);	/* [한국어] DISCONNECTED 이벤트 도착 시 콜백 등록 */
				if (rc == 0) {
					return;	/* [한국어] 정상 비동기 진행 — 폴 루프가 이벤트 수확 */
				}
			}
		}
	}

	disconnected_qpair_cb(rqpair, 0);	/* [한국어] cm_id 없거나 비활성 — 즉시 콜백 호출 (동기 경로) */
}

/*
 * [한국어]
 * nvme_rdma_ctrlr_disconnect_qpair_poll - vtable의 disconnect_qpair_poll 백엔드 (현재는 미사용, 내부 호출용).
 *
 * @return: 0=완전 정리 완료, -EAGAIN=진행 중, 음수=오류.
 *
 * disconnect 상태머신 한 단계 진행:
 *   - EXITING → process_event_poll (DISCONNECTED 이벤트 수확 + qpair_disconnected 콜백)
 *   - LINGERING → wait_until_quiet (in-flight WR FLUSH 대기)
 *   - EXITED → 이미 끝남
 *
 * I/O 큐페어는 EXITING 단계에서 ctrlr_lock 잠금 (poll_events가 컨트롤러 단위 자원 접근).
 */
static int
nvme_rdma_ctrlr_disconnect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	int rc;

	switch (rqpair->state) {
	case NVME_RDMA_QPAIR_STATE_EXITING:
		if (!nvme_qpair_is_admin_queue(qpair)) {
			nvme_ctrlr_lock(ctrlr);	/* [한국어] I/O 큐는 명시적 락 (Admin은 호출자가 이미 보유) */
		}

		rc = nvme_rdma_process_event_poll(rqpair);	/* [한국어] DISCONNECTED 이벤트 수확 → disconnected_qpair_cb */

		if (!nvme_qpair_is_admin_queue(qpair)) {
			nvme_ctrlr_unlock(ctrlr);
		}
		break;

	case NVME_RDMA_QPAIR_STATE_LINGERING:
		rc = nvme_rdma_qpair_wait_until_quiet(rqpair);	/* [한국어] in-flight WR FLUSH 대기 */
		break;
	case NVME_RDMA_QPAIR_STATE_EXITED:
		rc = 0;	/* [한국어] 이미 정리 끝 */
		break;

	default:
		assert(false);	/* [한국어] connect 상태에서 disconnect_poll 진입 = 버그 */
		rc = -EAGAIN;
		break;
	}

	return rc;
}

/*
 * [한국어]
 * nvme_rdma_ctrlr_disconnect_qpair - vtable의 ops.ctrlr_disconnect_qpair 구현 (큐페어 끊기 진입점).
 *
 * @ctrlr: 컨트롤러.
 * @qpair: 끊을 큐페어.
 *
 * 동작:
 *   1) _nvme_rdma_ctrlr_disconnect_qpair → rdma_disconnect 발사 + DISCONNECTED 콜백 등록.
 *   2) qpair->async == false면 EXITED 도달까지 동기 폴 (busy loop).
 *   3) async 모드는 즉시 반환 — poll_group_process_completions가 진행.
 *
 * async/sync 차이: I/O 큐페어는 reactor가 polling으로 진행 (async), admin은 sync로 깔끔히 정리.
 */
static void
nvme_rdma_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc;

	_nvme_rdma_ctrlr_disconnect_qpair(ctrlr, qpair, nvme_rdma_qpair_disconnected);	/* [한국어] disconnect 시퀀스 시작 */

	/* If the async mode is disabled, poll the qpair until it is actually disconnected.
	 * It is ensured that poll_group_process_completions() calls disconnected_qpair_cb
	 * for any disconnected qpair. Hence, we do not have to check if the qpair is in
	 * a poll group or not.
	 */
	/* [한국어] async=true: poll_group_process_completions가 다음 폴 사이클에 진행 — 즉시 반환.
	 *         async=false: 이 함수 반환 시점에 EXITED 보장 필요 → busy poll. */
	if (qpair->async) {
		return;
	}

	while (1) {
		rc = nvme_rdma_ctrlr_disconnect_qpair_poll(ctrlr, qpair);	/* [한국어] 동기 폴 — EAGAIN 동안 반복 */
		if (rc != -EAGAIN) {
			break;
		}
	}
}

/*
 * [한국어]
 * nvme_rdma_stale_conn_disconnected - stale conn 감지 후 disconnect 완료 콜백.
 *
 * @rqpair: 큐페어.
 * @ret: validate 결과.
 *
 * 동작:
 *   1) RDMA 자원 destroy (cm_id 등 — stale_conn_reconnect가 새로 만들 거니까).
 *   2) transport_failure_reason 클리어 (재시도이므로 실패 상태 리셋).
 *   3) state=STALE_CONN + 10ms 후 재시도 데드라인 설정.
 *
 * connect_qpair_poll의 STALE_CONN 분기가 데드라인 도달 후 stale_conn_reconnect → 재연결.
 */
static int
nvme_rdma_stale_conn_disconnected(struct nvme_rdma_qpair *rqpair, int ret)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;

	if (ret) {
		SPDK_DEBUGLOG(nvme, "Target did not respond to qpair disconnect.\n");	/* [한국어] disconnect 응답 없음 — 그래도 재시도 진행 */
	}

	nvme_rdma_qpair_destroy(rqpair);	/* [한국어] cm_id/QP 등 모두 새로 만들어야 — destroy 후 reconnect */

	qpair->last_transport_failure_reason = qpair->transport_failure_reason;	/* [한국어] 디버깅용 보존 */
	qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_NONE;	/* [한국어] 재시도 → 실패 상태 클리어 */

	rqpair->state = NVME_RDMA_QPAIR_STATE_STALE_CONN;	/* [한국어] connect_qpair_poll가 STALE_CONN 분기 진입 */
	rqpair->evt_timeout_ticks = (NVME_RDMA_STALE_CONN_RETRY_DELAY_US * spdk_get_ticks_hz()) /
				    SPDK_SEC_TO_USEC + spdk_get_ticks();	/* [한국어] 10ms 후 재시도 가능 */

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_stale_conn_retry - stale conn 자동 재시도 카운터 증가 + disconnect 시작.
 *
 * @rqpair: 큐페어 (-ESTALE 받은 상태).
 * @return: 0=재시도 진행, -ESTALE=한도 초과 (포기).
 *
 * 호출 체인: connect_established(-ESTALE) → [stale_conn_retry] → _nvme_rdma_ctrlr_disconnect_qpair (stale_conn_disconnected 콜백)
 *           → STALE_CONN 상태 + 10ms 대기 → connect_qpair_poll → stale_conn_reconnect → ctrlr_connect_qpair (다시 처음부터)
 */
static int
nvme_rdma_stale_conn_retry(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;

	if (rqpair->stale_conn_retry_count >= NVME_RDMA_STALE_CONN_RETRY_MAX) {
		NVME_RQPAIR_ERRLOG(rqpair, "Retry failed %d times, give up stale connection to qpair.\n",
				   NVME_RDMA_STALE_CONN_RETRY_MAX);
		return -ESTALE;	/* [한국어] 5회 재시도 실패 — 영구 실패로 상위 알림 */
	}

	rqpair->stale_conn_retry_count++;	/* [한국어] 누적 카운터 증가 */

	NVME_RQPAIR_NOTICELOG(rqpair, "%d times, retry stale connection.\n",
			      rqpair->stale_conn_retry_count);
	_nvme_rdma_ctrlr_disconnect_qpair(qpair->ctrlr, qpair, nvme_rdma_stale_conn_disconnected);	/* [한국어] 일반 disconnect 시퀀스 (단, 콜백은 stale 전용) */

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_ctrlr_delete_io_qpair - vtable의 ops.ctrlr_delete_io_qpair — 큐페어 영구 삭제.
 *
 * @ctrlr: 컨트롤러.
 * @qpair: 삭제할 큐페어.
 *
 * 동작:
 *   1) state != EXITED면 강제 disconnected (자원 해제).
 *   2) 남은 outstanding 요청 abort.
 *   3) nvme_qpair_deinit (상위 NVMe 자원 해제 — queued_req 등).
 *   4) rqpair 컨테이너 자체 free.
 *
 * disconnect와 차이: disconnect는 끊지만 컨테이너 보존, delete는 메모리까지 회수.
 */
static int
nvme_rdma_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair;

	assert(qpair != NULL);
	rqpair = nvme_rdma_qpair(qpair);

	if (rqpair->state != NVME_RDMA_QPAIR_STATE_EXITED) {
		int rc __attribute__((unused));	/* [한국어] release 빌드에서 unused 경고 회피 */

		/* qpair was removed from the poll group while the disconnect is not finished.
		 * Destroy rdma resources forcefully. */
		/* [한국어] poll group에서 미리 제거됐는데 disconnect 미완료 — 강제 정리.
		 * 0 ret이므로 quiet 경로 직진하여 EXITED 도달. */
		rc = nvme_rdma_qpair_disconnected(rqpair, 0);
		assert(rc == 0);
	}

	nvme_rdma_qpair_abort_reqs(qpair, qpair->abort_dnr);	/* [한국어] 남은 모든 요청에 abort cpl */
	assert(TAILQ_EMPTY(&rqpair->outstanding_reqs));
	nvme_qpair_deinit(qpair);	/* [한국어] 일반 NVMe 큐페어 정리 (queued_req 등) */

	spdk_free(rqpair);	/* [한국어] 컨테이너 메모리 회수 */

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_ctrlr_create_io_qpair - vtable의 ops.ctrlr_create_io_qpair — 사용자 옵션을 받아 I/O 큐페어 생성.
 *
 * 단순 위임: opts에서 파라미터 추출 후 nvme_rdma_ctrlr_create_qpair 호출.
 * admin 큐페어는 ctrlr_construct가 직접 nvme_rdma_ctrlr_create_qpair를 호출.
 */
static struct spdk_nvme_qpair *
nvme_rdma_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_rdma_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					    opts->io_queue_requests,
					    opts->delay_cmd_submit,
					    opts->async_mode);
}

/*
 * [한국어]
 * nvme_rdma_ctrlr_enable - vtable의 ops.ctrlr_enable — RDMA 트랜스포트는 별도 enable 동작 없음.
 *
 * PCIe 트랜스포트는 여기서 CC.EN=1 doorbell 등을 처리하지만, RDMA는 nvme_fabric_ctrlr_set_reg_4가
 * Property Set으로 동일한 동작을 하므로 별도 처리 불필요.
 */
static int
nvme_rdma_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	/* do nothing here */
	return 0;
}

static int nvme_rdma_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr);

/* We have to use the typedef in the function declaration to appease astyle. */
typedef struct spdk_nvme_ctrlr spdk_nvme_ctrlr_t;

/*
 * [한국어]
 * nvme_rdma_ctrlr_construct - vtable의 ops.ctrlr_construct — 컨트롤러 생성 (NVMe-oF Connect 진입점).
 *
 * @trid: 트랜스포트 ID (trtype=RDMA, traddr/trsvcid/subnqn).
 * @opts: 사용자 옵션 (큐 크기, retry, ack timeout 등).
 * @devhandle: 미사용 (PCIe 트랜스포트와 시그니처 호환).
 * @return: 임베드된 spdk_nvme_ctrlr 포인터 또는 NULL.
 *
 * 동기/배경:
 *   spdk_nvme_connect/probe → nvme_transport_ctrlr_construct가 호출. 이 단계에서는 RDMA 연결 미수립 —
 *   컨트롤러 메타데이터(cm_channel, admin qpair 컨테이너)만 준비. 실제 연결은 ctrlr_connect_qpair에서 시작.
 *
 * 동작:
 *   1) rctrlr 컨테이너 zmalloc.
 *   2) opts 검증 및 클램프 (transport_retry_count ≤ 7, ack_timeout ≤ 31).
 *   3) 모든 RDMA HCA의 max_sge 집계 → 최소값을 rctrlr->max_sge로 (보수적).
 *   4) 일반 NVMe ctrlr 초기화 (nvme_ctrlr_construct).
 *   5) CM 이벤트 풀 (NVME_RDMA_NUM_CM_EVENTS=256개) 사전 할당.
 *   6) cm_channel 생성 (모든 큐페어 CM 이벤트 공용 fd) + nonblock 설정.
 *   7) admin 큐페어 생성 (qid=0, async=true — connect_qpair_poll로 비동기 진행).
 *   8) accel 지원 시 SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED 플래그.
 *   9) 컨트롤러 프로세스 등록 (멀티프로세스 SPDK).
 *
 * 호출 체인: spdk_nvme_connect → nvme_probe_internal → ops.ctrlr_construct = [nvme_rdma_ctrlr_construct]
 */
static spdk_nvme_ctrlr_t *
nvme_rdma_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
			  const struct spdk_nvme_ctrlr_opts *opts,
			  void *devhandle)
{
	struct nvme_rdma_ctrlr *rctrlr;
	struct ibv_context **contexts;	/* [한국어] librdmacm이 반환하는 모든 RDMA HCA 배열 */
	struct ibv_device_attr dev_attr;	/* [한국어] HCA 속성 (max_sge, max_qp_wr 등) */
	int i, rc;

	rctrlr = spdk_zmalloc(sizeof(struct nvme_rdma_ctrlr), 0, NULL, SPDK_ENV_NUMA_ID_ANY,
			      SPDK_MALLOC_DMA);	/* [한국어] DMA 가능 메모리 (cm_events 풀 등 RDMA 자원과 함께 hugepage 위치) */
	if (rctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	rctrlr->ctrlr.opts = *opts;	/* [한국어] 사용자 옵션 복사 */
	rctrlr->ctrlr.trid = *trid;	/* [한국어] 트랜스포트 ID 복사 */

	if (opts->transport_retry_count > NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT) {
		NVME_CTRLR_NOTICELOG(&rctrlr->ctrlr, "transport_retry_count exceeds max value %d, use max value\n",
				     NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT);
		rctrlr->ctrlr.opts.transport_retry_count = NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT;	/* [한국어] InfiniBand spec max=7 — 클램프 */
	}

	if (opts->transport_ack_timeout > NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT) {
		NVME_CTRLR_NOTICELOG(&rctrlr->ctrlr, "transport_ack_timeout exceeds max value %d, use max value\n",
				     NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT);
		rctrlr->ctrlr.opts.transport_ack_timeout = NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT;	/* [한국어] IB spec max=31 (= 4.096us * 2^31) */
	}

	contexts = rdma_get_devices(NULL);	/* [한국어] librdmacm: 시스템의 모든 RDMA HCA 열거 */
	if (contexts == NULL) {
		NVME_CTRLR_ERRLOG(&rctrlr->ctrlr, "rdma_get_devices() failed: %s (%d)\n", spdk_strerror(errno),
				  errno);
		spdk_free(rctrlr);
		return NULL;
	}

	i = 0;
	rctrlr->max_sge = NVME_RDMA_MAX_SGL_DESCRIPTORS;	/* [한국어] 디폴트 상한, HCA 능력으로 깎임 */

	while (contexts[i] != NULL) {
		rc = ibv_query_device(contexts[i], &dev_attr);	/* [한국어] HCA 속성 조회 */
		if (rc < 0) {
			NVME_CTRLR_ERRLOG(&rctrlr->ctrlr, "Failed to query RDMA device attributes.\n");
			rdma_free_devices(contexts);
			spdk_free(rctrlr);
			return NULL;
		}
		rctrlr->max_sge = spdk_min(rctrlr->max_sge, (uint16_t)dev_attr.max_sge);	/* [한국어] 모든 HCA의 최소값 — 보수적 (어느 device를 써도 안전) */
		i++;
	}

	rdma_free_devices(contexts);	/* [한국어] 열거 결과 해제 */

	rc = nvme_ctrlr_construct(&rctrlr->ctrlr);	/* [한국어] 일반 NVMe 컨트롤러 초기화 (상태머신, namespace, ctrlr_lock 등) */
	if (rc != 0) {
		spdk_free(rctrlr);
		return NULL;
	}

	STAILQ_INIT(&rctrlr->pending_cm_events);
	STAILQ_INIT(&rctrlr->free_cm_events);
	rctrlr->cm_events = spdk_zmalloc(NVME_RDMA_NUM_CM_EVENTS * sizeof(*rctrlr->cm_events), 0, NULL,
					 SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);	/* [한국어] CM 이벤트 풀 — 256 슬롯 사전 할당 */
	if (rctrlr->cm_events == NULL) {
		NVME_CTRLR_ERRLOG(&rctrlr->ctrlr, "unable to allocate buffers to hold CM events.\n");
		goto destruct_ctrlr;
	}

	for (i = 0; i < NVME_RDMA_NUM_CM_EVENTS; i++) {
		STAILQ_INSERT_TAIL(&rctrlr->free_cm_events, &rctrlr->cm_events[i], link);	/* [한국어] free 풀에 모두 enqueue */
	}

	rctrlr->cm_channel = rdma_create_event_channel();	/* [한국어] librdmacm: CM 이벤트 fd 생성 — 컨트롤러 단위 공유 */
	if (rctrlr->cm_channel == NULL) {
		NVME_CTRLR_ERRLOG(&rctrlr->ctrlr, "rdma_create_event_channel() failed\n");
		goto destruct_ctrlr;
	}

	if (spdk_fd_set_nonblock(rctrlr->cm_channel->fd) < 0) {	/* [한국어] nonblock — rdma_get_cm_event가 EAGAIN 반환하도록 (busy poll) */
		goto destruct_ctrlr;
	}

	rctrlr->ctrlr.adminq = nvme_rdma_ctrlr_create_qpair(&rctrlr->ctrlr, 0,
			       rctrlr->ctrlr.opts.admin_queue_size, 0,
			       rctrlr->ctrlr.opts.admin_queue_size, false, true);	/* [한국어] admin qpair: qid=0, qprio=0, delay_submit=false, async=true */
	if (!rctrlr->ctrlr.adminq) {
		NVME_CTRLR_ERRLOG(&rctrlr->ctrlr, "failed to create admin qpair\n");
		goto destruct_ctrlr;
	}
	if (spdk_rdma_provider_accel_sequence_supported()) {
		rctrlr->ctrlr.flags |= SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED;	/* [한국어] accel framework 사용 가능 — UMR 등 */
	}

	if (nvme_ctrlr_add_process(&rctrlr->ctrlr, 0) != 0) {	/* [한국어] 멀티프로세스 SPDK용 프로세스 등록 (현재 PID) */
		NVME_CTRLR_ERRLOG(&rctrlr->ctrlr, "nvme_ctrlr_add_process() failed\n");
		goto destruct_ctrlr;
	}

	NVME_CTRLR_DEBUGLOG(&rctrlr->ctrlr, "successfully initialized the nvmf ctrlr\n");
	return &rctrlr->ctrlr;	/* [한국어] 임베드된 일반 ctrlr 반환 — 상위 코드는 ctrlr를 받지만 ContainerOf로 RDMA로 캐스팅 */

destruct_ctrlr:
	nvme_ctrlr_destruct(&rctrlr->ctrlr);	/* [한국어] 부분 자원 모두 정리 */
	return NULL;
}

/*
 * [한국어]
 * nvme_rdma_ctrlr_destruct - vtable의 ops.ctrlr_destruct — 컨트롤러와 모든 자원 해제.
 *
 * @ctrlr: 임베드된 일반 컨트롤러.
 * @return: 0 (실패 분기 없음 — 정리는 best-effort).
 *
 * 동작 순서 (의존성 역순):
 *   1) admin 큐페어 삭제 (nvme_rdma_ctrlr_delete_io_qpair) — disconnect + cm_id 해제까지.
 *   2) pending_cm_events에 남은 미처리 이벤트 ack — 누수 방지.
 *   3) cm_events 풀 해제.
 *   4) cm_channel 파괴 — librdmacm fd 닫기.
 *   5) 일반 NVMe 컨트롤러 정리 (nvme_ctrlr_destruct_finish).
 *   6) 컨테이너 해제.
 *
 * 호출 시점: spdk_nvme_detach 또는 probe 실패 시.
 */
static int
nvme_rdma_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_rdma_ctrlr *rctrlr = nvme_rdma_ctrlr(ctrlr);
	struct nvme_rdma_cm_event_entry *entry;

	if (ctrlr->adminq) {
		nvme_rdma_ctrlr_delete_io_qpair(ctrlr, ctrlr->adminq);	/* [한국어] admin 큐페어 → disconnect + delete (cm_id, QP 등 모두) */
	}

	STAILQ_FOREACH(entry, &rctrlr->pending_cm_events, link) {
		rdma_ack_cm_event(entry->evt);	/* [한국어] 미처리 이벤트 ack — 그렇지 않으면 librdmacm 메모리 누수 */
	}

	STAILQ_INIT(&rctrlr->free_cm_events);
	STAILQ_INIT(&rctrlr->pending_cm_events);
	spdk_free(rctrlr->cm_events);	/* [한국어] 256 슬롯 풀 해제 */

	if (rctrlr->cm_channel) {
		rdma_destroy_event_channel(rctrlr->cm_channel);	/* [한국어] librdmacm fd 닫기 */
		rctrlr->cm_channel = NULL;
	}

	nvme_ctrlr_destruct_finish(ctrlr);	/* [한국어] 일반 NVMe 컨트롤러 정리 (namespace 등) */

	spdk_free(rctrlr);	/* [한국어] 컨테이너 메모리 회수 */

	return 0;
}

/*
 * [한국어]
 * _nvme_rdma_qpair_submit_request - 빌드 끝난 SEND WR을 provider 큐에 enqueue + (옵션) 즉시 doorbell.
 *
 * @rqpair: 큐페어.
 * @rdma_req: 빌드 완료된 요청 슬롯.
 * @return: 0=성공 (delay 모드는 doorbell 보류, 즉시 모드는 게시 완료), 음수=ibv_post_send 실패.
 *
 * 동작:
 *   1) poll group 모드면 active_qpairs에 enqueue (다음 process_submits 사이클에 일괄 flush).
 *   2) current_num_sends++ (SQ 깊이 추적).
 *   3) WR을 provider 송신 큐에 큐잉 (Mellanox direct verbs는 WQE 직접 작성, 표준은 chain link).
 *   4) delay_cmd_submit=false면 즉시 ibv_post_send (doorbell write — HCA가 SQ tail 갱신을 인지).
 */
static inline int
_nvme_rdma_qpair_submit_request(struct nvme_rdma_qpair *rqpair,
				struct spdk_nvme_rdma_req *rdma_req)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;
	struct ibv_send_wr *wr;
	struct nvme_rdma_poll_group *group;

	if (TAILQ_ENTRY_NOT_ENQUEUED(rqpair, link_active) && qpair->poll_group) {
		group = nvme_rdma_poll_group(qpair->poll_group);
		TAILQ_INSERT_TAIL(&group->active_qpairs, rqpair, link_active);	/* [한국어] poll_group_process_completions가 큐 끝에서 process_submits 호출 */
	}
	assert(rqpair->current_num_sends < rqpair->num_entries);	/* [한국어] SQ 슬롯 overflow 가드 */
	rqpair->current_num_sends++;	/* [한국어] in-flight SEND 수 증가 */

	wr = &rdma_req->send_wr;
	wr->next = NULL;	/* [한국어] chain 링크 종단 — provider가 chain 구성 */
	nvme_rdma_trace_ibv_sge(wr->sg_list);	/* [한국어] 디버그 로그 매크로 */

	spdk_rdma_provider_qp_queue_send_wrs(rqpair->rdma_qp, wr);	/* [한국어] provider 추상 큐에 enqueue (직접 ibv_post_send 안 함) */

	if (!rqpair->delay_cmd_submit) {
		return nvme_rdma_qpair_submit_sends(rqpair);	/* [한국어] 즉시 모드 — provider flush_send_wrs → ibv_post_send */
	}

	return 0;	/* [한국어] delay 모드 — flush는 process_completions 끝에서 일괄 */
}

/*
 * [한국어]
 * nvme_rdma_qpair_submit_request - vtable의 ops.qpair_submit_request — I/O 송신 hot path 진입점.
 *
 * @qpair: 큐페어.
 * @req: NVMe 요청 (cmd/payload/cb_fn 보유).
 * @return: 0=송신 시작 성공, -EAGAIN=풀 고갈 (재시도 요청), 음수=빌드 실패.
 *
 * 동작:
 *   1) free_reqs 풀에서 rdma_req 1개 dequeue (없으면 -EAGAIN).
 *   2) req → rdma_req 결합 + cid = id (응답 라우팅 키).
 *   3) accel 시퀀스 사용 시 (UMR 모드): apply_accel_sequence — data_transfer 콜백에서 Capsule 송신.
 *   4) 일반 경로: req_init (빌드) → outstanding 큐에 enqueue → _submit_request.
 *
 * NVMe I/O 흐름:
 *   spdk_nvme_ns_cmd_write → nvme_qpair_submit_request → ops.qpair_submit_request → [본 함수]
 *     → free_reqs에서 dequeue → build → outstanding으로 이동 → SEND WR 게시 → 타깃에서 처리 → RECV로 응답
 */
static int
nvme_rdma_qpair_submit_request(struct spdk_nvme_qpair *qpair,
			       struct nvme_request *req)
{
	struct nvme_rdma_qpair *rqpair;
	struct spdk_nvme_rdma_req *rdma_req;
	int rc;

	rqpair = nvme_rdma_qpair(qpair);
	assert(rqpair != NULL);
	assert(req != NULL);

	rdma_req = nvme_rdma_req_get(rqpair);	/* [한국어] free 풀에서 슬롯 1개 — LIFO 캐시 친화 */
	if (spdk_unlikely(!rdma_req)) {
		if (rqpair->poller) {
			rqpair->poller->stats.queued_requests++;	/* [한국어] backpressure 통계 */
		}
		/* Inform the upper layer to try again later. */
		return -EAGAIN;	/* [한국어] 큐 고갈 — 상위가 queued_req에 큐잉 후 재시도 */
	}

	assert(rdma_req->req == NULL);	/* [한국어] req_put이 NULL 클리어 보장 */
	rdma_req->req = req;	/* [한국어] req와 결합 — 완료 시 req->cb_fn 호출 */
	req->cmd.cid = rdma_req->id;	/* [한국어] cid는 슬롯 인덱스 = 응답 cpl.cid로 라우팅 */
	if (req->accel_sequence || rqpair->append_copy) {	/* [한국어] UMR/accel 모드 — 외부 시퀀스 처리 후 Capsule 송신 */
		assert(spdk_rdma_provider_accel_sequence_supported());
		assert(rqpair->qpair.poll_group->group);
		assert(rqpair->qpair.poll_group->group->accel_fn_table.append_copy);
		assert(rqpair->qpair.poll_group->group->accel_fn_table.reverse_sequence);
		assert(rqpair->qpair.poll_group->group->accel_fn_table.finish_sequence);

		rc = nvme_rdma_apply_accel_sequence(rqpair, req, rdma_req);	/* [한국어] accel framework에 task 등록 → 비동기 진행 */
		if (spdk_unlikely(rc)) {
			NVME_RQPAIR_ERRLOG(rqpair, "failed to apply accel seq, rqpair %p, req %p, rc %d\n", rqpair,
					   rdma_req,
					   rc);
			nvme_rdma_req_put(rqpair, rdma_req);	/* [한국어] 슬롯 반납 */
			return rc;
		}
		/* Capsule will be sent in data_transfer callback */
		/* [한국어] memory_domain_transfer_data 콜백이 _submit_request 호출 → SEND WR 게시는 그때 */
		return 0;
	}

	rc = nvme_rdma_req_init(rqpair, rdma_req);	/* [한국어] 페이로드 형태별 SEND WR/Cmd 빌드 */
	if (spdk_unlikely(rc)) {
		NVME_RQPAIR_ERRLOG(rqpair, "nvme_rdma_req_init() failed\n");
		nvme_rdma_req_put(rqpair, rdma_req);
		return -1;
	}

	TAILQ_INSERT_TAIL(&rqpair->outstanding_reqs, rdma_req, link);	/* [한국어] outstanding 큐에 enqueue (timeout 검사 + abort 시 순회 대상) */
	rqpair->num_outstanding_reqs++;

	return _nvme_rdma_qpair_submit_request(rqpair, rdma_req);	/* [한국어] WR 큐잉 + (옵션) 즉시 doorbell */
}

/*
 * [한국어]
 * nvme_rdma_qpair_reset - vtable의 ops.qpair_reset — RDMA 트랜스포트는 reset 동작 없음.
 *
 * PCIe는 SQ/CQ doorbell 리셋이 필요하지만 RDMA는 큐페어가 끊어지면 새로 만드므로 reset 불필요.
 */
static int
nvme_rdma_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	/* Currently, doing nothing here */
	return 0;
}

/*
 * [한국어]
 * nvme_rdma_qpair_abort_reqs - vtable의 ops.qpair_abort_reqs — 모든 outstanding 요청 강제 abort.
 *
 * @qpair: 큐페어.
 * @dnr: Do Not Retry 비트 (1=재시도 부적절).
 *
 * 동작:
 *   1) 합성 cpl 준비 (sc=ABORTED_SQ_DELETION, sct=GENERIC).
 *   2) 큐페어가 disconnect 진행 중이 아니라면 disconnect 시작 (in-flight WR 회수 트리거).
 *   3) outstanding 요청 순회:
 *      - accel 진행 중인 요청은 스킵 (외부 콜백이 정리 책임).
 *      - 그 외는 합성 cpl로 즉시 완료 — 상위 cb_fn에 abort 통지.
 *
 * 호출 시점: 큐페어 disconnect 직전, ctrlr reset 시.
 * 주의 (영문 주석 그대로): RDMA 자원 등록 유지로 인해 abort만 해도 CQ에 완료가 올 수 있음 — 그래서 disconnect 먼저.
 */
static void
nvme_rdma_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct spdk_nvme_rdma_req *rdma_req, *tmp;
	struct spdk_nvme_cpl cpl;
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);

	cpl.sqid = qpair->id;	/* [한국어] 응답 헤더의 SQ ID */
	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;	/* [한국어] NVMe 1.x 4.6.3: SQ 삭제로 인한 abort */
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = dnr;	/* [한국어] DNR 비트 설정 */

	/*
	 * We cannot abort requests at the RDMA layer without
	 * unregistering them. If we do, we can still get error
	 * free completions on the shared completion queue.
	 */
	if (nvme_qpair_get_state(qpair) > NVME_QPAIR_DISCONNECTING &&
	    nvme_qpair_get_state(qpair) != NVME_QPAIR_DESTROYING) {
		nvme_ctrlr_disconnect_qpair(qpair);	/* [한국어] disconnect 발사 — QP가 ERR로 가서 in-flight WR이 FLUSH_ERR로 회수 */
	}

	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->outstanding_reqs, link, tmp) {
		if (rdma_req->in_progress_accel) {
			/* We should wait for accel completion */
			/* [한국어] accel 시퀀스는 외부에서 처리 — 강제 종료 못 함, 콜백이 정리 */
			continue;
		}
		nvme_rdma_req_complete(rdma_req, &cpl, true);	/* [한국어] 합성 cpl로 완료 처리 → req->cb_fn 호출 → free 풀 반납 */
	}
}

/*
 * [한국어]
 * nvme_rdma_qpair_check_timeout - outstanding 요청 중 timeout 도달한 것 확인 + 사용자 콜백 호출.
 *
 * @qpair: 큐페어.
 *
 * 동기/배경:
 *   사용자가 spdk_nvme_ctrlr_register_timeout_callback으로 타임아웃 콜백 등록했을 때 동작.
 *   요청은 outstanding_reqs 큐에 시간순으로 들어있어 첫 미타임아웃 도달하면 루프 중단 가능 (효율).
 *
 * 컨트롤러 초기화 중에는 타임아웃 검사 안 함 (probe 단계 일부 명령은 의도적으로 오래 걸릴 수 있음).
 */
static void
nvme_rdma_qpair_check_timeout(struct spdk_nvme_qpair *qpair)
{
	uint64_t t02;	/* [한국어] 현재 시각 (spdk_get_ticks 기준) */
	struct spdk_nvme_rdma_req *rdma_req, *tmp;
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvme_ctrlr_process *active_proc;	/* [한국어] 멀티프로세스 SPDK에서 현재 process의 timeout_cb */

	/* Don't check timeouts during controller initialization. */
	if (ctrlr->state != NVME_CTRLR_STATE_READY) {
		return;
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
		active_proc = nvme_ctrlr_get_current_process(ctrlr);	/* [한국어] admin은 컨트롤러 단위 process */
	} else {
		active_proc = qpair->active_proc;	/* [한국어] I/O는 큐페어가 보유 */
	}

	/* Only check timeouts if the current process has a timeout callback. */
	if (active_proc == NULL || active_proc->timeout_cb_fn == NULL) {
		return;	/* [한국어] 콜백 미등록 — 비용 절약 */
	}

	t02 = spdk_get_ticks();
	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->outstanding_reqs, link, tmp) {
		assert(rdma_req->req != NULL);

		if (nvme_request_check_timeout(rdma_req->req, rdma_req->id, active_proc, t02)) {
			/*
			 * The requests are in order, so as soon as one has not timed out,
			 * stop iterating.
			 */
			/* [한국어] FIFO 순서로 enqueue되었으므로 첫 미타임아웃 = 이후 모두 미타임아웃. early break. */
			break;
		}
	}
}

/*
 * [한국어]
 * nvme_rdma_request_ready - SEND/RECV 양 쪽 완료 모두 확인된 요청을 NVMe 레이어 완료 처리 + RECV 슬롯 재사용.
 *
 * @rqpair: 큐페어.
 * @rdma_req: 완료 준비된 요청 (completion_flags == SEND_COMPLETED|RECV_COMPLETED).
 *
 * 동작:
 *   1) transfer_cpl_cb 설정 시 (UMR 경로): finish_data_transfer로 외부 도메인 통지.
 *   2) 일반 경로: nvme_rdma_req_complete로 cb_fn 호출 + free 풀 반납.
 *   3) disconnect 진행 중이면 RECV 재게시 안 함 (LINGERING 무한 대기 방지).
 *   4) 정상이면 사용한 RECV WR 재게시 → 다음 응답 받을 준비.
 */
static inline void
nvme_rdma_request_ready(struct nvme_rdma_qpair *rqpair, struct spdk_nvme_rdma_req *rdma_req)
{
	struct ibv_recv_wr *recv_wr = rdma_req->rdma_rsp->recv_wr;	/* [한국어] 응답에 사용된 RECV WR — 재게시 대상 */

	if (rdma_req->transfer_cpl_cb) {
		int rc = 0;

		if (spdk_unlikely(spdk_nvme_cpl_is_error(&rdma_req->cpl))) {
			NVME_RQPAIR_WARNLOG(rqpair, "req %p, error cpl sct %d, sc %d\n", rdma_req, rdma_req->cpl.status.sct,
					    rdma_req->cpl.status.sc);
			rc = -EIO;	/* [한국어] CPL 오류 → 외부 도메인에 -EIO 통지 */
		}
		nvme_rdma_finish_data_transfer(rdma_req, rc);	/* [한국어] memory_domain transfer 콜백 호출 */
	} else {
		nvme_rdma_req_complete(rdma_req, &rdma_req->cpl, true);	/* [한국어] 일반 NVMe 완료 → cb_fn 호출 + req_put */
	}

	if (spdk_unlikely(rqpair->state >= NVME_RDMA_QPAIR_STATE_EXITING && !rqpair->srq)) {
		/* Skip posting back recv wr if we are in a disconnection process. We may never get
		 * a WC and we may end up stuck in LINGERING state until the timeout. */
		/* [한국어] disconnect 진행 중에 RECV 재게시 → 타깃 응답 못 옴 → FLUSH도 못 옴 → wait_until_quiet 무한 대기 위험.
		 * SRQ 모드는 다른 큐페어가 사용할 수 있으므로 항상 재게시. */
		return;
	}

	assert(rqpair->rsps->current_num_recvs < rqpair->rsps->num_entries);
	rqpair->rsps->current_num_recvs++;	/* [한국어] 재게시 카운터 증가 */

	recv_wr->next = NULL;	/* [한국어] chain link 종단 */
	nvme_rdma_trace_ibv_sge(recv_wr->sg_list);	/* [한국어] 디버그 로그 */

	if (!rqpair->srq) {
		spdk_rdma_provider_qp_queue_recv_wrs(rqpair->rdma_qp, recv_wr);	/* [한국어] 큐페어 RQ에 큐잉 — flush_recv_wrs가 일괄 게시 */
	} else {
		spdk_rdma_provider_srq_queue_recv_wrs(rqpair->srq, recv_wr);	/* [한국어] SRQ에 큐잉 — poller_submit_recvs가 ibv_post_srq_recv */
	}
}

/* [한국어] ibv_poll_cq 한 번에 처리할 최대 완료 수 — wc[] 스택 배열 크기.
 * 너무 크면 스택 부담, 너무 작으면 호출 오버헤드. 128이 균형점. */
#define MAX_COMPLETIONS_PER_POLL 128

/*
 * [한국어]
 * nvme_rdma_fail_qpair - WC 오류 또는 트랜스포트 실패 감지 시 큐페어 강제 disconnect.
 *
 * @qpair: 실패한 큐페어.
 * @failure_reason: ibv_wc_status_t 값 (IBV_WC_RETRY_EXC_ERR 등).
 *
 * RETRY_EXC_ERR(재전송 한도 초과) → REMOTE 실패 (네트워크/타깃 문제)로 표기.
 * 그 외는 UNKNOWN — disconnect 후 상위 NVMe 레이어가 fault 처리.
 */
static void
nvme_rdma_fail_qpair(struct spdk_nvme_qpair *qpair, int failure_reason)
{
	if (failure_reason == IBV_WC_RETRY_EXC_ERR) {
		qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_REMOTE;	/* [한국어] 원격 도달 불가 (네트워크 분단/타깃 다운) */
	} else if (qpair->transport_failure_reason == SPDK_NVME_QPAIR_FAILURE_NONE) {
		qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;	/* [한국어] 첫 실패만 기록 (이전 reason이 더 구체적이면 보존) */
	}

	nvme_ctrlr_disconnect_qpair(qpair);	/* [한국어] 상위 NVMe 레이어 disconnect — 결국 ops.ctrlr_disconnect_qpair 호출 */
}

/*
 * [한국어]
 * get_rdma_qpair_from_wc - 공유 CQ의 WC에서 wc->qp_num으로 어느 rqpair 소속인지 검색.
 *
 * @group: poll group.
 * @wc: 완료 (qp_num 보유).
 * @return: 일치하는 rqpair 또는 NULL.
 *
 * SRQ 모드/공유 CQ 모드에서 RECV 완료가 어느 큐페어용인지 식별 — connected/disconnected 두 큐 모두 검색
 * (disconnect 진행 중인 큐페어의 잔여 WC도 처리).
 */
static struct nvme_rdma_qpair *
get_rdma_qpair_from_wc(struct nvme_rdma_poll_group *group, struct ibv_wc *wc)
{
	struct spdk_nvme_qpair *qpair;
	struct nvme_rdma_qpair *rqpair;

	STAILQ_FOREACH(qpair, &group->group.connected_qpairs, poll_group_stailq) {
		rqpair = nvme_rdma_qpair(qpair);
		if (NVME_RDMA_POLL_GROUP_CHECK_QPN(rqpair, wc->qp_num)) {	/* [한국어] qp_num 일치 검사 매크로 */
			return rqpair;
		}
	}

	STAILQ_FOREACH(qpair, &group->group.disconnected_qpairs, poll_group_stailq) {
		rqpair = nvme_rdma_qpair(qpair);
		if (NVME_RDMA_POLL_GROUP_CHECK_QPN(rqpair, wc->qp_num)) {
			return rqpair;	/* [한국어] disconnect 진행 중이지만 잔여 WC가 올 수 있음 */
		}
	}

	return NULL;	/* [한국어] qp_num 일치 큐페어 없음 — 이미 destroy된 큐페어의 stale WC */
}

/*
 * [한국어]
 * nvme_rdma_log_wc_status - WC 오류 상태를 적절한 레벨로 로깅.
 *
 * IBV_WC_WR_FLUSH_ERR는 의도적인 flush(disconnect 진행 중)에서 정상 발생 → DEBUG 레벨.
 * 그 외 오류는 ERROR — 운영 알림 대상.
 */
static inline void
nvme_rdma_log_wc_status(struct nvme_rdma_qpair *rqpair, struct ibv_wc *wc)
{
	struct nvme_rdma_wr *rdma_wr = (struct nvme_rdma_wr *)wc->wr_id;	/* [한국어] WR 헤더 복원 (type 표시용) */

	if (wc->status == IBV_WC_WR_FLUSH_ERR) {
		/* If qpair is in ERR state, we will receive completions for all posted and not completed
		 * Work Requests with IBV_WC_WR_FLUSH_ERR status. Don't log an error in that case */
		/* [한국어] QP가 ERR로 갈 때 in-flight WR이 모두 FLUSH_ERR로 회수 — 정상 동작 */
		NVME_RQPAIR_DEBUGLOG(rqpair, "WC error, qp state %d, request 0x%lu type %d, status: (%d): %s\n",
				     rqpair->qpair.state, wc->wr_id, rdma_wr->type, wc->status, ibv_wc_status_str(wc->status));
	} else {
		NVME_RQPAIR_ERRLOG(rqpair, "WC error, qp state %d, request 0x%lu type %d, status: (%d): %s\n",
				   rqpair->qpair.state, wc->wr_id, rdma_wr->type, wc->status, ibv_wc_status_str(wc->status));
	}
}

/*
 * [한국어]
 * nvme_rdma_process_recv_completion - RECV WC 처리 — 응답 Capsule 도착 시 호출.
 *
 * @poller: poll group의 poller (SRQ 모드 라우팅용, 단독 모드는 NULL 가능).
 * @wc: ibv_poll_cq 결과.
 * @rdma_wr: wc->wr_id로 복원한 WR 헤더 (type=RECV).
 * @return: 1=요청 완전 완료, 0=SEND 미도착이라 대기 또는 stale, -ENXIO=오류.
 *
 * 동작:
 *   1) rdma_wr → spdk_nvme_rdma_rsp 복원 (SPDK_CONTAINEROF).
 *   2) 큐페어 식별:
 *      - SRQ 모드: wc->qp_num → get_rdma_qpair_from_wc
 *      - 큐페어 모드: rsp->rqpair (사전 보관)
 *      - stale WC (QP 이미 destroy)는 SRQ에 RECV 재게시 후 0 반환.
 *   3) WC 오류 시 → fail_qpair + cleanup.
 *   4) 정상이면 cpl.cid → rdma_reqs[cid] (역참조), RECV_COMPLETED 비트 OR.
 *   5) SEND_COMPLETED 미도착이면 0 반환 (SEND 완료 시 처리하라고 위임).
 *   6) 양쪽 도착이면 request_ready → cb_fn.
 *   7) delay 모드 아니면 사용한 RECV WR 즉시 재게시.
 *
 * 핵심: SEND/RECV는 비동기로 도착 순서가 다를 수 있음 → completion_flags 비트마스크로 양쪽 도착 보장.
 */
static inline int
nvme_rdma_process_recv_completion(struct nvme_rdma_poller *poller, struct ibv_wc *wc,
				  struct nvme_rdma_wr *rdma_wr)
{
	struct nvme_rdma_qpair		*rqpair;
	struct spdk_nvme_rdma_req	*rdma_req;
	struct spdk_nvme_rdma_rsp	*rdma_rsp;

	rdma_rsp = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvme_rdma_rsp, rdma_wr);	/* [한국어] WR 헤더 → 응답 객체 복원 */

	if (poller && poller->srq) {
		rqpair = get_rdma_qpair_from_wc(poller->group, wc);	/* [한국어] SRQ 모드는 qp_num으로 라우팅 */
		if (spdk_unlikely(!rqpair)) {
			/* Since we do not handle the LAST_WQE_REACHED event, we do not know when
			 * a Receive Queue in a QP, that is associated with an SRQ, is flushed.
			 * We may get a WC for a already destroyed QP.
			 *
			 * However, for the SRQ, this is not any error. Hence, just re-post the
			 * receive request to the SRQ to reuse for other QPs, and return 0.
			 */
			/* [한국어] SRQ는 이미 destroy된 QP의 RECV 잔여 WC가 도착할 수 있음.
			 * 응답 슬롯은 다른 큐페어가 재사용 가능하므로 SRQ에 다시 게시 — 무손실. */
			rdma_rsp->recv_wr->next = NULL;
			spdk_rdma_provider_srq_queue_recv_wrs(poller->srq, rdma_rsp->recv_wr);
			return 0;
		}
	} else {
		rqpair = rdma_rsp->rqpair;	/* [한국어] 큐페어 모드 — rsps 생성 시 보관된 부모 */
		if (spdk_unlikely(!rqpair)) {
			/* TODO: Fix forceful QP destroy when it is not async mode.
			 * CQ itself did not cause any error. Hence, return 0 for now.
			 */
			SPDK_WARNLOG("QP might be already destroyed.\n");	/* [한국어] 동기 destroy 경로 버그 — 보호적으로 0 반환 */
			return 0;
		}
	}


	assert(rqpair->rsps->current_num_recvs > 0);
	rqpair->rsps->current_num_recvs--;	/* [한국어] in-flight RECV 카운터 감소 (게시 → 완료) */

	if (spdk_unlikely(wc->status)) {
		nvme_rdma_log_wc_status(rqpair, wc);
		goto err_wc;	/* [한국어] WC 오류 — fail_qpair 경로 */
	}

	NVME_RQPAIR_DEBUGLOG(rqpair, "CQ recv completion\n");

	if (spdk_unlikely(wc->byte_len < sizeof(struct spdk_nvme_cpl))) {
		NVME_RQPAIR_ERRLOG(rqpair, "recv length %u less than expected response size\n", wc->byte_len);
		goto err_wc;	/* [한국어] CPL(16B)보다 짧은 응답 — 프로토콜 오류 */
	}
	rdma_req = &rqpair->rdma_reqs[rdma_rsp->cpl.cid];	/* [한국어] cid로 빠른 역참조 — O(1) */
	rdma_req->completion_flags |= NVME_RDMA_RECV_COMPLETED;	/* [한국어] RECV 도착 비트 OR */
	rdma_req->rdma_rsp = rdma_rsp;	/* [한국어] SEND가 늦게 와도 응답 보관 */
	rdma_req->cpl = rdma_rsp->cpl;	/* [한국어] 로컬 사본 — 원본은 RECV 재게시되므로 보존 필요 */

	if ((rdma_req->completion_flags & NVME_RDMA_SEND_COMPLETED) == 0) {
		return 0;	/* [한국어] SEND 미도착 — 그쪽에서 처리 위임 */
	}

	rqpair->num_completions++;	/* [한국어] 이번 폴 사이클 완료 수 증가 */

	nvme_rdma_request_ready(rqpair, rdma_req);	/* [한국어] 양쪽 완료 — NVMe 레이어 통지 + RECV 재게시 큐잉 */

	if (!rqpair->delay_cmd_submit) {
		if (spdk_unlikely(nvme_rdma_qpair_submit_recvs(rqpair))) {	/* [한국어] 즉시 모드 — 새 RECV WR 발사 */
			NVME_RQPAIR_ERRLOG(rqpair, "Unable to re-post rx descriptor\n");
			nvme_rdma_fail_qpair(&rqpair->qpair, 0);
			return -ENXIO;
		}
	}

	return 1;	/* [한국어] 요청 완전 완료 — 호출자(cq_process_completions)가 reaped++ */

err_wc:
	nvme_rdma_fail_qpair(&rqpair->qpair, 0);
	if (poller && poller->srq) {
		rdma_rsp->recv_wr->next = NULL;
		spdk_rdma_provider_srq_queue_recv_wrs(poller->srq, rdma_rsp->recv_wr);	/* [한국어] SRQ 슬롯 재사용 */
	}
	rdma_req = &rqpair->rdma_reqs[rdma_rsp->cpl.cid];
	if (rdma_req->transfer_cpl_cb) {
		nvme_rdma_finish_data_transfer(rdma_req, -ENXIO);	/* [한국어] 외부 도메인 정리 */
	}
	return -ENXIO;
}

/*
 * [한국어]
 * nvme_rdma_process_send_completion - SEND WC 처리 — NVMe Cmd Capsule 송신 ACK 도착 시 호출.
 *
 * @poller: poll group의 poller (NULL=단독 모드).
 * @rdma_qpair: 단독 모드 큐페어 (poller=NULL일 때 직접 전달).
 * @wc: 완료.
 * @rdma_wr: WR 헤더 (type=SEND).
 * @return: 1=요청 완전 완료, 0=RECV 미도착 또는 stale, -ENXIO=오류.
 *
 * 동작 (process_recv_completion과 대칭):
 *   1) rdma_wr → spdk_nvme_rdma_req 복원.
 *   2) 큐페어 식별: req->qpair (가장 빠름) → rdma_qpair (단독 모드) → get_rdma_qpair_from_wc.
 *   3) WC 오류 시 → fail_qpair + (SRQ 모드) RECV 슬롯 회수.
 *   4) req == NULL: 큐페어 destroy 진행 중 stale WC — 정상 (DEVICE_REMOVAL 등).
 *   5) SEND_COMPLETED 비트 OR + current_num_sends--.
 *   6) RECV 미도착이면 0 반환 (RECV 완료 시 처리 위임).
 *   7) 양쪽 도착이면 request_ready.
 */
static inline int
nvme_rdma_process_send_completion(struct nvme_rdma_poller *poller,
				  struct nvme_rdma_qpair *rdma_qpair,
				  struct ibv_wc *wc, struct nvme_rdma_wr *rdma_wr)
{
	struct nvme_rdma_qpair		*rqpair;
	struct spdk_nvme_rdma_req	*rdma_req;

	rdma_req = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvme_rdma_req, rdma_wr);	/* [한국어] WR 헤더 → 요청 객체 */
	rqpair = rdma_req->req ? nvme_rdma_qpair(rdma_req->req->qpair) : NULL;	/* [한국어] req에 보존된 qpair (정상 경로) */
	if (spdk_unlikely(!rqpair)) {
		rqpair = rdma_qpair != NULL ? rdma_qpair : get_rdma_qpair_from_wc(poller->group, wc);	/* [한국어] req 클리어된 stale WC — 다른 경로로 복원 */
	}

	/* If we are flushing I/O */
	if (spdk_unlikely(wc->status)) {
		if (!rqpair) {
			/* When poll_group is used, several qpairs share the same CQ and it is possible to
			 * receive a completion with error (e.g. IBV_WC_WR_FLUSH_ERR) for already disconnected qpair
			 * That happens due to qpair is destroyed while there are submitted but not completed send/receive
			 * Work Requests */
			/* [한국어] 공유 CQ에서 이미 destroy된 큐페어의 잔여 WC — 정상, 무시 */
			assert(poller);
			return 0;
		}
		assert(rqpair->current_num_sends > 0);
		rqpair->current_num_sends--;	/* [한국어] in-flight 카운터 감소 */
		nvme_rdma_log_wc_status(rqpair, wc);
		nvme_rdma_fail_qpair(&rqpair->qpair, 0);
		if (rdma_req->rdma_rsp && poller && poller->srq) {
			rdma_req->rdma_rsp->recv_wr->next = NULL;
			spdk_rdma_provider_srq_queue_recv_wrs(poller->srq, rdma_req->rdma_rsp->recv_wr);	/* [한국어] SRQ 슬롯 재사용 */
		}
		if (rdma_req->transfer_cpl_cb) {
			nvme_rdma_finish_data_transfer(rdma_req, -ENXIO);
		}
		return -ENXIO;
	}

	/* We do not support Soft Roce anymore. Other than Soft Roce's bug, we should not
	 * receive a completion without error status after qpair is disconnected/destroyed.
	 */
	if (spdk_unlikely(rdma_req->req == NULL)) {
		/*
		 * Some infiniband drivers do not guarantee the previous assumption after we
		 * received a RDMA_CM_EVENT_DEVICE_REMOVAL event.
		 */
		/* [한국어] DEVICE_REMOVAL 후 일부 드라이버가 stale WC 전달 — need_destroy 마킹된 경우만 허용 */
		SPDK_ERRLOG("Received malformed completion: request 0x%"PRIx64" type %d\n", wc->wr_id,
			    rdma_wr->type);
		if (!rqpair || !rqpair->need_destroy) {
			assert(0);	/* [한국어] DEVICE_REMOVAL 외 컨텍스트면 버그 */
		}
		return -ENXIO;
	}

	rdma_req->completion_flags |= NVME_RDMA_SEND_COMPLETED;	/* [한국어] SEND 도착 비트 */
	assert(rqpair->current_num_sends > 0);
	rqpair->current_num_sends--;	/* [한국어] SQ in-flight 감소 */

	if ((rdma_req->completion_flags & NVME_RDMA_RECV_COMPLETED) == 0) {
		return 0;	/* [한국어] RECV 미도착 — 그쪽에서 처리 */
	}

	rqpair->num_completions++;

	nvme_rdma_request_ready(rqpair, rdma_req);	/* [한국어] 양쪽 완료 — NVMe 완료 처리 */

	if (!rqpair->delay_cmd_submit) {
		if (spdk_unlikely(nvme_rdma_qpair_submit_recvs(rqpair))) {
			NVME_RQPAIR_ERRLOG(rqpair, "Unable to re-post rx descriptor\n");
			nvme_rdma_fail_qpair(&rqpair->qpair, 0);
			return -ENXIO;
		}
	}

	return 1;
}

/*
 * [한국어]
 * nvme_rdma_cq_process_completions - 한 번의 ibv_poll_cq + 각 WC 디스패치 (SEND/RECV).
 *
 * @cq: 폴링할 CQ.
 * @batch_size: 한 번에 폴링할 최대 WC 수 (MAX_COMPLETIONS_PER_POLL=128 이하).
 * @poller: poll group 모드일 때 자신의 poller (NULL=단독).
 * @rdma_qpair: 단독 모드 큐페어 (poller=NULL일 때 컨텍스트).
 * @rdma_completions: 누적 완료 수 OUT (통계용).
 * @return: reaped 완료 수 (>=0), -ECANCELED=ibv_poll_cq 실패.
 *
 * 동작:
 *   1) ibv_poll_cq(cq, batch_size, wc[]) — verbs API: CQ에서 최대 batch_size개 WC 추출.
 *   2) 각 WC의 wr_id로 rdma_wr 헤더 복원 → type 보고 RECV/SEND 분기.
 *   3) reaped는 "완전 완료된 요청 수" (양쪽 비트 모두 도착한 것만 1로 count).
 *   4) 음수 _rc는 completion_rc에 보존 — 호출자가 우선 처리.
 *
 * 핵심 verbs 호출: ibv_poll_cq는 lockless, blocking 없음, 0 반환 = busy poll에서 idle.
 */
static inline int
nvme_rdma_cq_process_completions(struct ibv_cq *cq, uint32_t batch_size,
				 struct nvme_rdma_poller *poller,
				 struct nvme_rdma_qpair *rdma_qpair,
				 uint64_t *rdma_completions)
{
	struct ibv_wc			wc[MAX_COMPLETIONS_PER_POLL];	/* [한국어] WC 스택 배열 — 128 * 48B = 6KB */
	struct nvme_rdma_wr		*rdma_wr;
	uint32_t			reaped = 0;	/* [한국어] 완전 완료된 요청 수 */
	int				completion_rc = 0;	/* [한국어] 가장 최근 음수 결과 보존 */
	int				rc, _rc, i;

	rc = ibv_poll_cq(cq, batch_size, wc);	/* [한국어] verbs API: nonblock CQ poll. WC 배열 채움, 반환=실제 추출 수 */
	if (spdk_unlikely(rc < 0)) {
		NVME_RQPAIR_ERRLOG(rdma_qpair, "Error polling CQ! (%d): %s\n", errno, spdk_strerror(errno));
		return -ECANCELED;	/* [한국어] CQ 자체가 invalid 상태 — 큐페어 fail 처리 */
	} else if (rc == 0) {
		return 0;	/* [한국어] idle — 처리할 완료 없음 */
	}

	for (i = 0; i < rc; i++) {
		rdma_wr = (struct nvme_rdma_wr *)wc[i].wr_id;	/* [한국어] WR 헤더 (type 분기 키) */
		switch (rdma_wr->type) {
		case RDMA_WR_TYPE_RECV:
			_rc = nvme_rdma_process_recv_completion(poller, &wc[i], rdma_wr);	/* [한국어] 응답 Capsule */
			break;

		case RDMA_WR_TYPE_SEND:
			_rc = nvme_rdma_process_send_completion(poller, rdma_qpair, &wc[i], rdma_wr);	/* [한국어] 커맨드 송신 ACK */
			break;

		default:
			NVME_RQPAIR_ERRLOG(rdma_qpair, "Received an unexpected opcode on the CQ: %d\n", rdma_wr->type);
			return -ECANCELED;	/* [한국어] 손상된 wr_id — 메모리 corruption 의심 */
		}
		if (spdk_likely(_rc >= 0)) {
			reaped += _rc;	/* [한국어] _rc=1: 완료 1건, _rc=0: 짝짓기 대기 */
		} else {
			completion_rc = _rc;	/* [한국어] 오류 보존 — 끝까지 순회 후 반환 */
		}
	}

	*rdma_completions += rc;	/* [한국어] 통계 — 처리한 모든 WC 누적 */

	if (spdk_unlikely(completion_rc)) {
		return completion_rc;
	}

	return reaped;	/* [한국어] 완전 완료된 요청 수 — 호출자가 max_completions 카운트 */
}

/*
 * [한국어]
 * dummy_disconnected_qpair_cb - poll group이 큐페어 disconnect를 통지할 때 사용할 dummy 콜백.
 *
 * connecting 큐페어에서 호출되는 process_completions가 disconnect 콜백을 요구하지만
 * 사실상 처리할 일 없음 → 빈 함수.
 */
static void
dummy_disconnected_qpair_cb(struct spdk_nvme_qpair *qpair, void *poll_group_ctx)
{

}

/*
 * [한국어]
 * nvme_rdma_qpair_process_completions - vtable의 ops.qpair_process_completions — 단독 큐페어 폴 hot path.
 *
 * @qpair: 큐페어.
 * @max_completions: 한 호출에 처리할 최대 완료 수 (0=num_entries).
 * @return: 처리한 완료 수 또는 음수 오류.
 *
 * 두 가지 경로:
 *   a) qpair에 poll_group이 있으면 → poll_group_process_completions로 위임 (공유 CQ 처리).
 *   b) standalone (단독 CQ): 자체 cq_process_completions 루프.
 *
 * 큐페어 상태 분기:
 *   - CONNECTING: connect_qpair_poll로 핸드셰이크 진행, 완료 시 queued_req 재제출.
 *   - DISCONNECTING: disconnect_qpair_poll로 종료 진행 후 -ENXIO 반환.
 *   - 그 외: CM 이벤트 처리 후 일반 폴.
 *
 * 폴 루프 후: submit_sends + submit_recvs로 큐잉된 WR doorbell + timeout 검사.
 */
static int
nvme_rdma_qpair_process_completions(struct spdk_nvme_qpair *qpair,
				    uint32_t max_completions)
{
	struct nvme_rdma_qpair		*rqpair = nvme_rdma_qpair(qpair);
	int				rc = 0, batch_size;
	struct ibv_cq			*cq;
	uint64_t			rdma_completions = 0;

	/*
	 * This is used during the connection phase. It's possible that we are still reaping error completions
	 * from other qpairs so we need to call the poll group function. Also, it's more correct since the cq
	 * is shared.
	 */
	/* [한국어] poll group 모드: 공유 CQ는 다른 큐페어 완료가 섞여 있을 수 있음 → 그룹 함수에 위임 */
	if (qpair->poll_group != NULL) {
		return spdk_nvme_poll_group_process_completions(qpair->poll_group->group, max_completions,
				dummy_disconnected_qpair_cb);
	}

	if (max_completions == 0) {
		max_completions = rqpair->num_entries;	/* [한국어] 0=무제한 → 큐 깊이만큼 */
	} else {
		max_completions = spdk_min(max_completions, rqpair->num_entries);	/* [한국어] 큐 깊이 상한 */
	}

	switch (nvme_qpair_get_state(qpair)) {
	case NVME_QPAIR_CONNECTING:
		rc = nvme_rdma_ctrlr_connect_qpair_poll(qpair->ctrlr, qpair);	/* [한국어] connect 핸드셰이크 진행 */
		if (rc == 0) {
			/* Once the connection is completed, we can submit queued requests */
			nvme_qpair_resubmit_requests(qpair, rqpair->num_entries);	/* [한국어] connect 중 큐잉된 요청 재제출 */
		} else if (rc != -EAGAIN) {
			NVME_RQPAIR_ERRLOG(rqpair, "Failed to connect\n");
			goto failed;
		} else if (rqpair->state <= NVME_RDMA_QPAIR_STATE_INITIALIZING) {
			return 0;	/* [한국어] 아직 ESTABLISHED 전 — 폴 의미 없음 */
		}
		break;

	case NVME_QPAIR_DISCONNECTING:
		nvme_rdma_ctrlr_disconnect_qpair_poll(qpair->ctrlr, qpair);	/* [한국어] disconnect 진행 */
		return -ENXIO;	/* [한국어] 더 이상 I/O 받지 않음 통지 */

	default:
		nvme_rdma_qpair_process_cm_event(rqpair);	/* [한국어] CONNECTED 등에서 CM 이벤트 1개 처리 */
		break;
	}

	if (spdk_unlikely(qpair->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE)) {
		goto failed;	/* [한국어] 트랜스포트 실패 감지 — fail_qpair */
	}

	cq = rqpair->cq;	/* [한국어] 단독 CQ */

	rqpair->num_completions = 0;	/* [한국어] 매 호출마다 리셋 */
	do {
		batch_size = spdk_min((max_completions - rqpair->num_completions), MAX_COMPLETIONS_PER_POLL);
		rc = nvme_rdma_cq_process_completions(cq, batch_size, NULL, rqpair, &rdma_completions);	/* [한국어] ibv_poll_cq + 디스패치 */

		if (rc == 0) {
			break;	/* [한국어] 처리할 완료 없음 — 폴 종료 */
			/* Handle the case where we fail to poll the cq. */
		} else if (rc == -ECANCELED) {
			goto failed;	/* [한국어] CQ invalid — fail */
		} else if (rc == -ENXIO) {
			return rc;	/* [한국어] 큐페어 fail 진행 — 즉시 반환 */
		}
	} while (rqpair->num_completions < max_completions);

	if (spdk_unlikely(nvme_rdma_qpair_submit_sends(rqpair) ||
			  nvme_rdma_qpair_submit_recvs(rqpair))) {	/* [한국어] 큐잉된 SEND/RECV WR 일괄 doorbell */
		goto failed;
	}

	if (qpair->ctrlr->timeout_enabled) {
		nvme_rdma_qpair_check_timeout(qpair);	/* [한국어] 사용자 timeout 콜백 검사 */
	}

	return rqpair->num_completions;

failed:
	nvme_rdma_fail_qpair(qpair, 0);
	return -ENXIO;
}

/*
 * [한국어]
 * nvme_rdma_ctrlr_get_max_xfer_size - vtable의 ops.ctrlr_get_max_xfer_size — 최대 I/O 전송 크기 보고.
 *
 * RDMA는 MR 크기보다 NVMe-oF 컨트롤러의 MDTS(Maximum Data Transfer Size)가 더 작은 제약 →
 * UINT32_MAX 반환해 상위 레이어가 cdata.mdts로 자동 조정하도록 위임.
 */
static uint32_t
nvme_rdma_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	/* max_mr_size by ibv_query_device indicates the largest value that we can
	 * set for a registered memory region.  It is independent from the actual
	 * I/O size and is very likely to be larger than 2 MiB which is the
	 * granularity we currently register memory regions.  Hence return
	 * UINT32_MAX here and let the generic layer use the controller data to
	 * moderate this value.
	 */
	return UINT32_MAX;	/* [한국어] 무제한 보고 → 상위가 MDTS로 클램프 */
}

/*
 * [한국어]
 * nvme_rdma_ctrlr_get_max_sges - vtable의 ops.ctrlr_get_max_sges — NVMe Cmd 1개당 최대 SGL descriptor 수.
 *
 * 3가지 제약의 최소값:
 *   1) HCA의 max_sge (모든 device 최소).
 *   2) Capsule 크기 - Cmd(64B) / descriptor(16B) → 한 캡슐에 들어가는 descriptor 수.
 *   3) MSDBD (Maximum SGL Data Block Descriptors, NVMe-oF Identify CDATA) — 컨트롤러가 광고한 한도.
 *
 * UMR 모드에서는 (3) 무시 — 어차피 가상 contig MR 1개로 표현하므로.
 */
static uint16_t
nvme_rdma_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_rdma_ctrlr *rctrlr = nvme_rdma_ctrlr(ctrlr);
	uint32_t max_sge = rctrlr->max_sge;	/* [한국어] HCA 능력 (construct에서 집계) */
	uint32_t max_in_capsule_sge = (ctrlr->cdata.nvmf_specific.ioccsz * 16 -
				       sizeof(struct spdk_nvme_cmd)) /
				      sizeof(struct spdk_nvme_sgl_descriptor);	/* [한국어] ioccsz 단위는 16B → 바이트 변환 후 Cmd 빼고 descriptor 크기로 나눔 */

	/* Max SGE is limited by capsule size */
	max_sge = spdk_min(max_sge, max_in_capsule_sge);
	/* Max SGE may be limited by MSDBD.
	 * If umr_per_io is enabled and supported, we always use virtually contig buffer, we don't limit max_sge by
	 * MSDBD in that case */
	/* [한국어] UMR 모드는 가상 contig MR 1개 → MSDBD 무시.
	 * 아니면 NVMe-oF 컨트롤러가 광고한 MSDBD 한도 적용 (msdbd=0은 "제한 없음"). */
	if (!(g_spdk_nvme_transport_opts.rdma_umr_per_io &&
	      spdk_rdma_provider_accel_sequence_supported()) &&
	    ctrlr->cdata.nvmf_specific.msdbd != 0) {
		max_sge = spdk_min(max_sge, ctrlr->cdata.nvmf_specific.msdbd);
	}

	/* Max SGE can't be less than 1 */
	max_sge = spdk_max(1, max_sge);	/* [한국어] 최소 1 보장 — descriptor 0개면 IO 불가 */
	return max_sge;
}

/*
 * [한국어]
 * nvme_rdma_qpair_iterate_requests - vtable의 ops.qpair_iterate_requests — outstanding 요청 순회.
 *
 * @qpair: 큐페어.
 * @iter_fn: 각 요청에 호출할 콜백.
 * @arg: 콜백 인자.
 * @return: iter_fn이 0 외 반환하면 즉시 종료, 끝까지 가면 0.
 *
 * 사용처: timeout 검사 (nvme_request_check_timeout) 등 외부에서 outstanding 순회 필요 시.
 */
static int
nvme_rdma_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
				 int (*iter_fn)(struct nvme_request *req, void *arg),
				 void *arg)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	struct spdk_nvme_rdma_req *rdma_req, *tmp;
	int rc;

	assert(iter_fn != NULL);

	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->outstanding_reqs, link, tmp) {
		assert(rdma_req->req != NULL);

		rc = iter_fn(rdma_req->req, arg);
		if (rc != 0) {
			return rc;	/* [한국어] 콜백이 중단 신호 — 즉시 반환 */
		}
	}

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_qpair_authenticate - vtable의 ops.qpair_authenticate — DH-CHAP 등 사후 인증 트리거.
 *
 * @qpair: 인증할 큐페어 (RUNNING 상태여야 함).
 * @return: 0=인증 시작, -ENOTCONN=상태 부적합.
 *
 * connect 후 동적으로 인증을 시작하는 경로 (정책 변경 시 등). 상태를 AUTHENTICATING으로 전이 →
 * connect_qpair_poll가 progress 추적.
 */
static int
nvme_rdma_qpair_authenticate(struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	int rc;

	/* If the qpair is still connecting, it'll be forced to authenticate later on */
	if (rqpair->state < NVME_RDMA_QPAIR_STATE_RUNNING) {
		return 0;	/* [한국어] connect 진행 중 — connect_qpair_poll가 인증 단계 진입 시 자동 처리 */
	} else if (rqpair->state != NVME_RDMA_QPAIR_STATE_RUNNING) {
		return -ENOTCONN;	/* [한국어] EXITING 등 — 인증 불가 */
	}

	rc = nvme_fabric_qpair_authenticate_async(qpair);	/* [한국어] DH-CHAP 시퀀스 시작 (nvme_fabric.c) */
	if (rc == 0) {
		nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTING);	/* [한국어] 상위 레이어를 CONNECTING으로 (process_completions가 polling) */
		rqpair->state = NVME_RDMA_QPAIR_STATE_AUTHENTICATING;	/* [한국어] 내부 상태머신 — connect_qpair_poll가 authenticate_poll 진행 */
	}

	return rc;
}

/*
 * [한국어]
 * nvme_rdma_admin_qpair_abort_aers - vtable의 ops.admin_qpair_abort_aers — admin 큐의 미완료 AER abort.
 *
 * AER (Async Event Request, opcode 0x0C): NVMe 컨트롤러가 비동기 알림을 호스트에 보낼 때 사용.
 * 컨트롤러 reset/disconnect 시 모든 in-flight AER을 abort 시켜야 — 그렇지 않으면 영원히 응답 안 옴.
 *
 * print_on_error=false: AER abort는 expected 동작 — 사용자에게 오류 출력 안 함.
 */
static void
nvme_rdma_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_rdma_req *rdma_req, *tmp;
	struct spdk_nvme_cpl cpl;
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);

	cpl.sqid = qpair->id;
	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->outstanding_reqs, link, tmp) {
		assert(rdma_req->req != NULL);

		if (rdma_req->req->cmd.opc != SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			continue;	/* [한국어] AER만 abort 대상 — 다른 admin Cmd는 정상 처리 */
		}

		nvme_rdma_req_complete(rdma_req, &cpl, false);	/* [한국어] print_error=false (expected abort) */
	}
}

/*
 * [한국어]
 * nvme_rdma_poller_destroy - poller 자원 해제 (CQ/SRQ/MR/PD/응답 풀).
 *
 * 의존성 역순:
 *   1) CQ — 다른 자원 해제 후 더 이상 새 WC 생성 안 됨.
 *   2) 응답 풀 (rsps) — RECV WR이 쓰던 버퍼.
 *   3) SRQ — RECV WR 큐 자체.
 *   4) MR 풀 — ibv_dereg_mr들.
 *   5) PD — 다른 자원이 모두 PD를 참조 안 함.
 *   6) 컨테이너.
 */
static void
nvme_rdma_poller_destroy(struct nvme_rdma_poller *poller)
{
	if (poller->cq) {
		ibv_destroy_cq(poller->cq);	/* [한국어] verbs API: CQ 해제 — 미처리 WC가 있으면 EBUSY 가능 */
	}
	if (poller->rsps) {
		nvme_rdma_free_rsps(poller->rsps);	/* [한국어] SRQ 모드 공유 응답 버퍼 풀 */
	}
	if (poller->srq) {
		spdk_rdma_provider_srq_destroy(poller->srq);	/* [한국어] SRQ — provider 추상 (ibv_destroy_srq) */
	}
	if (poller->mr_map) {
		spdk_rdma_utils_free_mem_map(&poller->mr_map);	/* [한국어] MR 풀 — 등록된 영역들 dereg */
	}
	if (poller->pd) {
		spdk_rdma_utils_put_pd(poller->pd);	/* [한국어] PD ref-- (캐시되어 있으면 다른 곳이 사용 중) */
	}
	free(poller);
}

/*
 * [한국어]
 * nvme_rdma_poller_create - poll group에 device 추가 시 새 poller 생성 (1 device당 1 poller).
 *
 * @group: 부모 poll group.
 * @ctx: ibv_context (HCA 컨텍스트, cm_id->verbs와 일치해야 큐페어들이 attach 가능).
 * @return: 생성된 poller 또는 NULL.
 *
 * 동기/배경:
 *   같은 HCA의 큐페어들이 1개의 CQ를 공유 → ibv_poll_cq 한 번에 모든 큐페어 완료 수확 → CPU 효율.
 *   옵션 rdma_srq_size != 0이면 SRQ도 생성해 RECV WR을 공유 (메모리 절약).
 *
 * 동작:
 *   1) (SRQ 모드만) device 속성 조회 → PD/MR/SRQ/응답 풀 생성 + RECV WR 일괄 게시.
 *   2) CQ 생성 (SRQ 모드는 srq_size*2, 단독 모드는 DEFAULT_NVME_RDMA_CQ_SIZE=4096).
 *   3) group->pollers에 enqueue.
 *
 * 호출 체인: poll_group_get_poller → (없으면) [poller_create] → ibv_create_cq + spdk_rdma_provider_srq_create
 */
static struct nvme_rdma_poller *
nvme_rdma_poller_create(struct nvme_rdma_poll_group *group, struct ibv_context *ctx)
{
	struct nvme_rdma_poller *poller;
	struct ibv_device_attr dev_attr;
	struct spdk_rdma_provider_srq_init_attr srq_init_attr = {};	/* [한국어] SRQ 생성 파라미터 */
	struct nvme_rdma_rsp_opts opts;
	int num_cqe, max_num_cqe;	/* [한국어] CQ 슬롯 수 */
	int rc;

	poller = calloc(1, sizeof(*poller));
	if (poller == NULL) {
		SPDK_ERRLOG("Unable to allocate poller.\n");
		return NULL;
	}

	poller->group = group;
	poller->device = ctx;	/* [한국어] HCA 식별 (큐페어 attach 시 일치 검사) */

	if (g_spdk_nvme_transport_opts.rdma_srq_size != 0) {	/* [한국어] SRQ 모드 — 응답 풀을 device 단위로 공유 */
		rc = ibv_query_device(ctx, &dev_attr);
		if (rc) {
			SPDK_ERRLOG("Unable to query RDMA device.\n");
			goto fail;
		}

		poller->pd = spdk_rdma_utils_get_pd(ctx);	/* [한국어] device PD (캐시) */
		if (poller->pd == NULL) {
			SPDK_ERRLOG("Unable to get PD.\n");
			goto fail;
		}

		poller->mr_map = spdk_rdma_utils_create_mem_map(poller->pd, &g_nvme_hooks,
				 IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);	/* [한국어] SRQ에 게시할 응답 버퍼용 MR 풀 */
		if (poller->mr_map == NULL) {
			SPDK_ERRLOG("Unable to create memory map.\n");
			goto fail;
		}

		srq_init_attr.stats = &poller->stats.rdma_stats.recv;
		srq_init_attr.pd = poller->pd;
		srq_init_attr.srq_init_attr.attr.max_wr = spdk_min((uint32_t)dev_attr.max_srq_wr,
				g_spdk_nvme_transport_opts.rdma_srq_size);	/* [한국어] HCA 한계와 옵션 중 작은 값 */
		srq_init_attr.srq_init_attr.attr.max_sge = spdk_min(dev_attr.max_sge,
				NVME_RDMA_DEFAULT_RX_SGE);	/* [한국어] RECV는 1 SGE면 충분 (CPL 16B) */

		poller->srq = spdk_rdma_provider_srq_create(&srq_init_attr);	/* [한국어] verbs ibv_create_srq 또는 direct */
		if (poller->srq == NULL) {
			SPDK_ERRLOG("Unable to create SRQ.\n");
			goto fail;
		}

		opts.num_entries = g_spdk_nvme_transport_opts.rdma_srq_size;
		opts.rqpair = NULL;	/* [한국어] SRQ 모드 마커 */
		opts.srq = poller->srq;
		opts.mr_map = poller->mr_map;

		poller->rsps = nvme_rdma_create_rsps(&opts);	/* [한국어] 공유 응답 풀 — SRQ에 RECV WR 큐잉됨 */
		if (poller->rsps == NULL) {
			SPDK_ERRLOG("Unable to create poller RDMA responses.\n");
			goto fail;
		}

		rc = nvme_rdma_poller_submit_recvs(poller);	/* [한국어] SRQ에 일괄 게시 — 응답 받을 준비 */
		if (rc) {
			SPDK_ERRLOG("Unable to submit poller RDMA responses.\n");
			goto fail;
		}

		/*
		 * When using an srq, fix the size of the completion queue at startup.
		 * The initiator sends only send and recv WRs. Hence, the multiplier is 2.
		 * (The target sends also data WRs. Hence, the multiplier is 3.)
		 */
		/* [한국어] SRQ 모드는 큐페어 추가 시 resize 안 함 → 시작 시 충분히 큰 CQ 필요.
		 * 호스트는 SEND+RECV만 → 2*srq_size. 타깃은 RDMA_READ/WRITE까지 발행하므로 3배 필요 (그쪽 주석). */
		num_cqe = g_spdk_nvme_transport_opts.rdma_srq_size * 2;
	} else {
		num_cqe = DEFAULT_NVME_RDMA_CQ_SIZE;	/* [한국어] 단독 RQ 모드 — 4096 (qpair_set_poller에서 동적 확장) */
	}

	max_num_cqe = g_spdk_nvme_transport_opts.rdma_max_cq_size;
	if (max_num_cqe != 0 && num_cqe > max_num_cqe) {
		num_cqe = max_num_cqe;	/* [한국어] 운영 정책 상한 */
	}

	poller->cq = ibv_create_cq(poller->device, num_cqe, group, NULL, 0);	/* [한국어] verbs: CQ 생성. cq_context=group, channel=NULL(busy poll) */

	if (poller->cq == NULL) {
		SPDK_ERRLOG("Unable to create CQ, errno %d.\n", errno);
		goto fail;
	}

	STAILQ_INSERT_HEAD(&group->pollers, poller, link);	/* [한국어] poll group 리스트에 추가 */
	group->num_pollers++;
	poller->current_num_wc = num_cqe;	/* [한국어] CQ 슬롯 수 캐시 */
	poller->required_num_wc = 0;	/* [한국어] 큐페어 attach 시 누적 */
	return poller;

fail:
	nvme_rdma_poller_destroy(poller);
	return NULL;
}

/*
 * [한국어]
 * nvme_rdma_poll_group_free_pollers - poll group의 모든 poller 해제 (group destroy 시).
 *
 * 모든 poller의 refcnt == 0이어야 함 (큐페어들이 모두 release_poller 했어야).
 * non-zero refcnt면 경고만 출력하고 강제 destroy — 댕글링 우려 있지만 group 자체가 사라지므로 진행.
 */
static void
nvme_rdma_poll_group_free_pollers(struct nvme_rdma_poll_group *group)
{
	struct nvme_rdma_poller	*poller, *tmp_poller;

	STAILQ_FOREACH_SAFE(poller, &group->pollers, link, tmp_poller) {
		assert(poller->refcnt == 0);
		if (poller->refcnt) {
			SPDK_WARNLOG("Destroying poller with non-zero ref count: poller %p, refcnt %d\n",
				     poller, poller->refcnt);
		}

		STAILQ_REMOVE(&group->pollers, poller, nvme_rdma_poller, link);
		nvme_rdma_poller_destroy(poller);
	}
}

/*
 * [한국어]
 * nvme_rdma_poll_group_get_poller - device에 해당하는 poller를 찾거나 새로 생성 + refcnt 증가.
 *
 * @group: poll group.
 * @device: 큐페어가 속한 ibv_context (cm_id->verbs).
 * @return: 해당 device의 poller (이미 있던 것 또는 신규).
 *
 * 큐페어가 attach 시 호출. 1 device당 1 poller 정책으로 같은 HCA 큐페어들끼리 CQ 공유.
 */
static struct nvme_rdma_poller *
nvme_rdma_poll_group_get_poller(struct nvme_rdma_poll_group *group, struct ibv_context *device)
{
	struct nvme_rdma_poller *poller = NULL;

	STAILQ_FOREACH(poller, &group->pollers, link) {
		if (poller->device == device) {
			break;	/* [한국어] 일치 device 발견 — 재사용 */
		}
	}

	if (!poller) {
		poller = nvme_rdma_poller_create(group, device);	/* [한국어] 신규 생성 */
		if (!poller) {
			SPDK_ERRLOG("Failed to create a poller for device %p\n", device);
			return NULL;
		}
	}

	poller->refcnt++;	/* [한국어] 사용 큐페어 +1 — 0이 되면 destroy */
	return poller;
}

/*
 * [한국어]
 * nvme_rdma_poll_group_put_poller - poller refcnt 감소, 0이면 자원 해제.
 *
 * 큐페어가 release 시 호출. 마지막 큐페어가 떠나면 device의 CQ/SRQ 등 모두 정리.
 */
static void
nvme_rdma_poll_group_put_poller(struct nvme_rdma_poll_group *group, struct nvme_rdma_poller *poller)
{
	assert(poller->refcnt > 0);

	if (--poller->refcnt == 0) {
		STAILQ_REMOVE(&group->pollers, poller, nvme_rdma_poller, link);
		group->num_pollers--;
		nvme_rdma_poller_destroy(poller);	/* [한국어] CQ/SRQ/MR/PD 해제 */
	}
}

/*
 * [한국어]
 * nvme_rdma_poll_group_create - vtable의 ops.poll_group_create — 빈 poll group 컨테이너 할당.
 *
 * pollers는 큐페어 attach 시 동적 추가됨. connecting_qpairs/active_qpairs도 빈 상태로 시작.
 */
static struct spdk_nvme_transport_poll_group *
nvme_rdma_poll_group_create(void)
{
	struct nvme_rdma_poll_group	*group;

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		SPDK_ERRLOG("Unable to allocate poll group.\n");
		return NULL;
	}

	STAILQ_INIT(&group->pollers);
	TAILQ_INIT(&group->connecting_qpairs);
	TAILQ_INIT(&group->active_qpairs);
	return &group->group;	/* [한국어] 임베드된 일반 poll group 반환 */
}

/*
 * [한국어]
 * nvme_rdma_poll_group_connect_qpair - vtable의 ops.poll_group_connect_qpair — 큐페어 attach (실제 작업 없음).
 *
 * 실제 connect 시작은 ctrlr_connect_qpair에서 하므로 여기서는 0만 반환.
 */
static int
nvme_rdma_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

/*
 * [한국어]
 * nvme_rdma_poll_group_disconnect_qpair - vtable의 ops.poll_group_disconnect_qpair — connecting 큐에서 제거.
 *
 * connect 중에 disconnect 트리거된 큐페어를 connecting_qpairs에서 제거 — process_completions가 더 이상 진척시키지 않음.
 */
static int
nvme_rdma_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	struct nvme_rdma_poll_group *group = nvme_rdma_poll_group(qpair->poll_group);

	if (TAILQ_ENTRY_ENQUEUED(rqpair, link_connecting)) {
		TAILQ_REMOVE_CLEAR(&group->connecting_qpairs, rqpair, link_connecting);	/* [한국어] connecting 워크큐에서 분리 */
	}

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_poll_group_add - vtable의 ops.poll_group_add — 큐페어 추가 (실제 작업 없음).
 *
 * 실제 poller attach는 qpair_init에서 cm_id->verbs를 보고 결정 → 여기서는 0만 반환.
 */
static int
nvme_rdma_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			 struct spdk_nvme_qpair *qpair)
{
	return 0;
}

/*
 * [한국어]
 * nvme_rdma_poll_group_remove - vtable의 ops.poll_group_remove — 큐페어 제거 + 강제 disconnect.
 *
 * disconnect 미완료 상태에서 group에서 제거되는 경우(예외 경로) poller가 댕글링되지 않도록
 * 강제로 disconnect 진행 — poller_release에서 cleanup. 그렇지 않으면 group destroy 시 poller refcnt > 0 경고.
 */
static int
nvme_rdma_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
			    struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	struct nvme_rdma_poll_group *group = nvme_rdma_poll_group(qpair->poll_group);

	if (rqpair->poller) {
		/* A qpair may skip transport disconnect part if it was already disconnecting. But on RDMA level a qpair
		 * may still have a poller reference. In that case we should continue transport disconnect here
		 * because a poller depends on the poll group reference which is going to be removed */
		/* [한국어] 큐페어가 NVMe 레이어에서는 disconnecting인데 RDMA에서는 poller 참조 보유 — 강제 정리 */
		NVME_RQPAIR_INFOLOG(rqpair, "nvme state %d, rdma state %d, force disconnect\n", qpair->state,
				    rqpair->state);
		nvme_rdma_ctrlr_disconnect_qpair(qpair->ctrlr, qpair);
	}

	if (TAILQ_ENTRY_ENQUEUED(rqpair, link_active)) {
		TAILQ_REMOVE_CLEAR(&group->active_qpairs, rqpair, link_active);	/* [한국어] active 워크큐에서 분리 */
	}

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_qpair_process_submits - 한 큐페어의 큐잉된 SEND/RECV WR 일괄 doorbell + 큐잉된 요청 재제출.
 *
 * @group: 부모 poll group.
 * @rqpair: 처리할 큐페어 (active_qpairs에 enqueue됨).
 *
 * poll_group_process_completions의 후반 단계에서 호출 — CQ에서 완료 수확 후 SEND/RECV doorbell.
 * delay_cmd_submit 모드의 핵심: 매 _submit_request마다 doorbell 안 하고, 폴 사이클 끝에서 일괄.
 *
 * 동작:
 *   1) 상태 검증 — 미연결/disconnect 진행 큐페어는 skip.
 *   2) timeout 검사 (옵션).
 *   3) submit_sends + submit_recvs (RECV는 SRQ 모드에서 poller가 처리).
 *   4) 완료된 요청 재제출 — 비어있는 SQ 슬롯에 큐잉된 요청 채움.
 *   5) outstanding/queued 모두 빈 큐페어는 active_qpairs에서 분리 (다음 폴은 CQ만).
 */
static inline void
nvme_rdma_qpair_process_submits(struct nvme_rdma_poll_group *group,
				struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_qpair	*qpair = &rqpair->qpair;

	assert(TAILQ_ENTRY_ENQUEUED(rqpair, link_active));

	if (spdk_unlikely(rqpair->state <= NVME_RDMA_QPAIR_STATE_INITIALIZING ||
			  rqpair->state >= NVME_RDMA_QPAIR_STATE_EXITING)) {
		return;	/* [한국어] connect 미완료 또는 disconnect 진행 — submit 안 함 */
	}

	if (qpair->ctrlr->timeout_enabled) {
		nvme_rdma_qpair_check_timeout(qpair);
	}

	nvme_rdma_qpair_submit_sends(rqpair);	/* [한국어] 큐잉된 SEND WR 일괄 doorbell */
	if (!rqpair->srq) {
		nvme_rdma_qpair_submit_recvs(rqpair);	/* [한국어] 큐페어 RQ — RECV 게시 (SRQ 모드는 poller가 처리) */
	}
	if (rqpair->num_completions > 0) {
		nvme_qpair_resubmit_requests(qpair, rqpair->num_completions);	/* [한국어] 완료된 만큼 큐잉된 요청 재제출 → 큐 깊이 유지 */
		rqpair->num_completions = 0;
	}

	if (rqpair->num_outstanding_reqs == 0 && STAILQ_EMPTY(&qpair->queued_req)) {
		TAILQ_REMOVE_CLEAR(&group->active_qpairs, rqpair, link_active);	/* [한국어] 더 처리할 일 없음 — active에서 분리 (다음 _submit_request에서 다시 추가) */
	}
}

/*
 * [한국어]
 * nvme_rdma_poll_group_process_completions - vtable의 ops.poll_group_process_completions — 핵심 폴 hot path.
 *
 * @tgroup: 일반 poll group.
 * @completions_per_qpair: 큐페어 1개당 처리할 최대 완료 수 (0=MAX_COMPLETIONS_PER_POLL).
 * @disconnected_qpair_cb: disconnect 완료 시 호출할 콜백 (사용자 정의).
 * @return: 처리한 총 완료 수 또는 음수 오류.
 *
 * Poll Group의 1 폴 사이클 = 4단계:
 *   1) disconnected_qpairs 진행 — disconnect 진행 중 큐페어들의 상태머신 한 단계.
 *   2) connecting_qpairs 진행 — connect 진행 중 큐페어들의 상태머신 한 단계.
 *   3) connected_qpairs CM 이벤트 처리 — 정상 동작 중에도 ADDR_CHANGE/DEVICE_REMOVAL 가능.
 *   4) 모든 poller의 CQ 폴 (ibv_poll_cq) — 완료 수확 + SEND/RECV 분기.
 *   5) active_qpairs의 process_submits — 큐잉된 WR doorbell + 요청 재제출.
 *
 * 부하 분산: completions_per_poller = (완료 한도) / (poller 수) — 1 device가 다른 device를 starve하지 않음.
 *
 * 호출 컨텍스트: SPDK reactor 스레드의 poller 콜백 — 무한 루프에서 주기적으로 호출.
 * SPDK_NVME_TRANSPORT_RDMA 큐페어들이 모두 같은 reactor 스레드에서 처리되어 lockless.
 */
static int64_t
nvme_rdma_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_qpair			*qpair, *tmp_qpair;
	struct nvme_rdma_qpair			*rqpair, *tmp_rqpair;
	struct nvme_rdma_poll_group		*group;
	struct nvme_rdma_poller			*poller;
	int					batch_size, rc, rc2 = 0;
	int64_t					total_completions = 0;
	uint64_t				completions_allowed = 0;
	uint64_t				completions_per_poller = 0;
	uint64_t				poller_completions = 0;
	uint64_t				rdma_completions;

	if (completions_per_qpair == 0) {
		completions_per_qpair = MAX_COMPLETIONS_PER_POLL;	/* [한국어] 디폴트 = 128 */
	}

	group = nvme_rdma_poll_group(tgroup);

	/* [한국어] (1) disconnected 큐페어 — disconnect_qpair_poll로 EXITED 도달 시 사용자 콜백 호출 */
	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
		rc = nvme_rdma_ctrlr_disconnect_qpair_poll(qpair->ctrlr, qpair);
		if (rc == 0) {
			disconnected_qpair_cb(qpair, tgroup->group->ctx);	/* [한국어] EXITED 도달 — 사용자 정리 콜백 */
		}
	}

	/* [한국어] (2) connecting 큐페어 — connect_qpair_poll로 핸드셰이크 진행 */
	TAILQ_FOREACH_SAFE(rqpair, &group->connecting_qpairs, link_connecting, tmp_rqpair) {
		qpair = &rqpair->qpair;

		rc = nvme_rdma_ctrlr_connect_qpair_poll(qpair->ctrlr, qpair);
		if (rc == 0 || rc != -EAGAIN) {	/* [한국어] 0=완료, 음수=실패 → 어느 쪽이든 connecting에서 분리 */
			TAILQ_REMOVE_CLEAR(&group->connecting_qpairs, rqpair, link_connecting);

			if (rc == 0) {
				/* Once the connection is completed, we can submit queued requests */
				nvme_qpair_resubmit_requests(qpair, rqpair->num_entries);	/* [한국어] 큐잉된 요청 재제출 */
			} else if (rc != -EAGAIN) {
				NVME_RQPAIR_ERRLOG(rqpair, "Failed to connect\n");
				nvme_rdma_fail_qpair(qpair, 0);	/* [한국어] connect 실패 → fail 경로 */
			}
		}
	}

	/* [한국어] (3) connected 큐페어 — CM 이벤트 (ADDR_CHANGE/DEVICE_REMOVAL 등) 처리 */
	STAILQ_FOREACH_SAFE(qpair, &tgroup->connected_qpairs, poll_group_stailq, tmp_qpair) {
		rqpair = nvme_rdma_qpair(qpair);

		if (spdk_likely(nvme_qpair_get_state(qpair) != NVME_QPAIR_CONNECTING)) {
			nvme_rdma_qpair_process_cm_event(rqpair);	/* [한국어] CONNECTING 상태는 (2)에서 처리 — 중복 회피 */
		}

		if (spdk_unlikely(qpair->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE)) {
			rc2 = -ENXIO;
			nvme_rdma_fail_qpair(qpair, 0);
		}
	}

	/* [한국어] (4) 모든 poller의 CQ 폴 — 부하 분산 */
	completions_allowed = completions_per_qpair * tgroup->num_connected_qpairs;
	if (spdk_likely(group->num_pollers)) {
		completions_per_poller = spdk_max(completions_allowed / group->num_pollers, 1);	/* [한국어] 균등 분배 */
	}

	STAILQ_FOREACH(poller, &group->pollers, link) {
		poller_completions = 0;
		rdma_completions = 0;
		do {
			poller->stats.polls++;	/* [한국어] 폴 횟수 통계 */
			batch_size = spdk_min((completions_per_poller - poller_completions), MAX_COMPLETIONS_PER_POLL);
			rc = nvme_rdma_cq_process_completions(poller->cq, batch_size, poller, NULL, &rdma_completions);	/* [한국어] ibv_poll_cq + 디스패치 */
			if (rc <= 0) {
				if (rc == -ECANCELED) {
					return -EIO;	/* [한국어] CQ invalid — 즉시 반환 */
				} else if (rc == 0) {
					poller->stats.idle_polls++;	/* [한국어] busy poll 효율성 지표 */
				}
				break;
			}

			poller_completions += rc;
		} while (poller_completions < completions_per_poller);
		total_completions += poller_completions;
		poller->stats.completions += rdma_completions;
		if (poller->srq) {
			nvme_rdma_poller_submit_recvs(poller);	/* [한국어] SRQ 모드 — 회수된 RECV 슬롯 일괄 재게시 */
		}
	}

	/* [한국어] (5) active 큐페어들 — SEND/RECV WR doorbell + 요청 재제출 */
	TAILQ_FOREACH_SAFE(rqpair, &group->active_qpairs, link_active, tmp_rqpair) {
		nvme_rdma_qpair_process_submits(group, rqpair);
	}

	return rc2 != 0 ? rc2 : total_completions;	/* [한국어] CM 단계의 fail 우선, 없으면 완료 수 */
}

/*
 * Handle disconnected qpairs when interrupt support gets added.
 */
/*
 * [한국어]
 * nvme_rdma_poll_group_check_disconnected_qpairs - vtable 콜백 — 인터럽트 지원 시 사용 예정 (현재 빈 함수).
 *
 * SPDK 현재는 busy poll만 지원 → 호출돼도 할 일 없음.
 */
static void
nvme_rdma_poll_group_check_disconnected_qpairs(struct spdk_nvme_transport_poll_group *tgroup,
		spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
}

/*
 * [한국어]
 * nvme_rdma_poll_group_destroy - vtable의 ops.poll_group_destroy — poll group과 모든 poller 해제.
 *
 * 큐페어가 남아있으면 -EBUSY (사용자가 먼저 모든 큐페어 disconnect 해야).
 */
static int
nvme_rdma_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	struct nvme_rdma_poll_group	*group = nvme_rdma_poll_group(tgroup);

	if (!STAILQ_EMPTY(&tgroup->connected_qpairs) || !STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
		return -EBUSY;	/* [한국어] 사용자가 먼저 정리해야 — destroy 거부 */
	}

	nvme_rdma_poll_group_free_pollers(group);
	free(group);

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_poll_group_get_stats - vtable의 ops.poll_group_get_stats — RPC 응답용 통계 수집.
 *
 * 각 poller(=device)별로 polls/idle_polls/completions/queued_requests/send/recv WR 통계 수집.
 * 호출자가 free 책임 (poll_group_free_stats).
 */
static int
nvme_rdma_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
			       struct spdk_nvme_transport_poll_group_stat **_stats)
{
	struct nvme_rdma_poll_group *group;
	struct spdk_nvme_transport_poll_group_stat *stats;
	struct spdk_nvme_rdma_device_stat *device_stat;
	struct nvme_rdma_poller *poller;
	uint32_t i = 0;

	if (tgroup == NULL || _stats == NULL) {
		SPDK_ERRLOG("Invalid stats or group pointer\n");
		return -EINVAL;
	}

	group = nvme_rdma_poll_group(tgroup);
	stats = calloc(1, sizeof(*stats));
	if (!stats) {
		SPDK_ERRLOG("Can't allocate memory for RDMA stats\n");
		return -ENOMEM;
	}
	stats->trtype = SPDK_NVME_TRANSPORT_RDMA;
	stats->rdma.num_devices = group->num_pollers;	/* [한국어] poller 수 = device 수 */

	if (stats->rdma.num_devices == 0) {
		*_stats = stats;
		return 0;	/* [한국어] poller 없음 — 빈 통계 반환 */
	}

	stats->rdma.device_stats = calloc(stats->rdma.num_devices, sizeof(*stats->rdma.device_stats));
	if (!stats->rdma.device_stats) {
		SPDK_ERRLOG("Can't allocate memory for RDMA device stats\n");
		free(stats);
		return -ENOMEM;
	}

	STAILQ_FOREACH(poller, &group->pollers, link) {
		device_stat = &stats->rdma.device_stats[i];
		device_stat->name = poller->device->device->name;	/* [한국어] HCA 이름 (예: "mlx5_0") */
		device_stat->polls = poller->stats.polls;
		device_stat->idle_polls = poller->stats.idle_polls;
		device_stat->completions = poller->stats.completions;
		device_stat->queued_requests = poller->stats.queued_requests;
		device_stat->total_send_wrs = poller->stats.rdma_stats.send.num_submitted_wrs;
		device_stat->send_doorbell_updates = poller->stats.rdma_stats.send.doorbell_updates;
		device_stat->total_recv_wrs = poller->stats.rdma_stats.recv.num_submitted_wrs;
		device_stat->recv_doorbell_updates = poller->stats.rdma_stats.recv.doorbell_updates;
		i++;
	}

	*_stats = stats;

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_poll_group_free_stats - vtable의 ops.poll_group_free_stats — get_stats 결과 해제.
 */
static void
nvme_rdma_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
				struct spdk_nvme_transport_poll_group_stat *stats)
{
	if (stats) {
		free(stats->rdma.device_stats);
	}
	free(stats);
}

/*
 * [한국어]
 * nvme_rdma_ctrlr_get_memory_domains - vtable의 ops.ctrlr_get_memory_domains — 컨트롤러의 RDMA 메모리 도메인 노출.
 *
 * accel framework, GPU Direct RDMA 등이 호스트 메모리를 RDMA로 직접 접근하기 위해 도메인 정보 필요.
 * admin 큐페어의 rdma_qp->domain을 대표로 반환 (모든 큐페어가 같은 도메인 공유 가정).
 */
static int
nvme_rdma_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_memory_domain **domains, int array_size)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(ctrlr->adminq);

	if (domains && array_size > 0) {
		domains[0] = rqpair->rdma_qp->domain;	/* [한국어] provider QP 추상의 메모리 도메인 — RDMA 키 변환에 사용 */
	}

	return 1;	/* [한국어] 항상 도메인 1개 (RDMA 트랜스포트) */
}

/*
 * [한국어]
 * nvme_rdma_ctrlr_process_transport_events - vtable의 ops.ctrlr_process_transport_events — CM 이벤트 진행.
 *
 * 컨트롤러 단위에서 cm_channel을 폴 (poll group 외부에서 호출되는 경로).
 * 단순 위임 — nvme_rdma_poll_events.
 */
static int
nvme_rdma_ctrlr_process_transport_events(struct spdk_nvme_ctrlr *ctrlr)
{
	return nvme_rdma_poll_events(nvme_rdma_ctrlr(ctrlr));
}

/*
 * [한국어]
 * spdk_nvme_rdma_init_hooks - 공개 API: 사용자가 RDMA hook 함수 등록 (PD 커스터마이즈 등).
 *
 * GPU Direct RDMA 등 사용자가 자체 PD를 제공해 외부 메모리 접근 가능하게 함.
 * 호출 시점: spdk_nvme_probe 호출 전. g_nvme_hooks 전역에 복사.
 */
void
spdk_nvme_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks)
{
	g_nvme_hooks = *hooks;	/* [한국어] 사용자 후크 전역 저장 — 이후 모든 큐페어가 참조 */
}

/*
 * [한국어]
 * rdma_ops - SPDK NVMe 트랜스포트 vtable의 RDMA 구현.
 *
 * 이 구조체가 NVMe 드라이버와 RDMA 트랜스포트의 유일한 인터페이스 — nvme_transport.c가 trtype=RDMA 큐페어/컨트롤러
 * 동작을 모두 이 vtable로 위임.
 *
 * 콜백 그룹별 분류:
 *   - ctrlr_*: 컨트롤러 라이프사이클 (construct/destruct/enable + Property Set/Get + xfer 능력 + 큐페어 생성/삭제).
 *   - 일부 ctrlr_*reg_* 콜백은 nvme_fabric.c로 직접 위임 — Property Set/Get을 통한 BAR MMIO 대체.
 *   - qpair_*: 큐페어 단위 동작 (abort/reset/submit/process_completions/iterate/authenticate).
 *   - poll_group_*: 다중 큐페어 일괄 폴 (create/add/remove/process/get_stats/destroy).
 *
 * 상호 배타: vtable 함수들은 모두 큐페어 소유 reactor 스레드에서 호출 — lockless.
 * 예외: ctrlr_lock 보호가 명시된 함수들 (poll_events, qpair_destroy 등 — pending_cm_events 보호).
 */
const struct spdk_nvme_transport_ops rdma_ops = {
	.name = "RDMA",	/* [한국어] 트랜스포트 이름 — RPC/로그 출력 */
	.type = SPDK_NVME_TRANSPORT_RDMA,	/* [한국어] enum 식별자 — nvme_transport.c가 trid.trtype과 매칭 */
	.ctrlr_construct = nvme_rdma_ctrlr_construct,	/* [한국어] 컨트롤러 생성 — cm_channel + admin qpair */
	.ctrlr_scan = nvme_fabric_ctrlr_scan,	/* [한국어] Discovery service 스캔 — nvme_fabric.c 위임 */
	.ctrlr_destruct = nvme_rdma_ctrlr_destruct,	/* [한국어] 컨트롤러 해제 */
	.ctrlr_enable = nvme_rdma_ctrlr_enable,	/* [한국어] CC.EN=1 (no-op for RDMA) */

	/* [한국어] BAR MMIO 대체 — Fabrics Property Set/Get 커맨드로 가상 레지스터 R/W (CAP/VS/CC/CSTS).
	 * 동기 4가지(_set_reg_4/8/_get_reg_4/8) + 비동기 4가지(_async). */
	.ctrlr_set_reg_4 = nvme_fabric_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_fabric_ctrlr_set_reg_8,
	.ctrlr_get_reg_4 = nvme_fabric_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_fabric_ctrlr_get_reg_8,
	.ctrlr_set_reg_4_async = nvme_fabric_ctrlr_set_reg_4_async,
	.ctrlr_set_reg_8_async = nvme_fabric_ctrlr_set_reg_8_async,
	.ctrlr_get_reg_4_async = nvme_fabric_ctrlr_get_reg_4_async,
	.ctrlr_get_reg_8_async = nvme_fabric_ctrlr_get_reg_8_async,

	.ctrlr_get_max_xfer_size = nvme_rdma_ctrlr_get_max_xfer_size,	/* [한국어] MDTS 광고 (UINT32_MAX) */
	.ctrlr_get_max_sges = nvme_rdma_ctrlr_get_max_sges,	/* [한국어] HCA + Capsule + MSDBD 최소 */

	.ctrlr_create_io_qpair = nvme_rdma_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_rdma_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_rdma_ctrlr_connect_qpair,	/* [한국어] CM 핸드셰이크 시작 */
	.ctrlr_disconnect_qpair = nvme_rdma_ctrlr_disconnect_qpair,	/* [한국어] rdma_disconnect 발사 */

	.ctrlr_get_memory_domains = nvme_rdma_ctrlr_get_memory_domains,	/* [한국어] RDMA 메모리 도메인 노출 */
	.ctrlr_process_transport_events = nvme_rdma_ctrlr_process_transport_events,	/* [한국어] cm_channel 외부 폴 */

	.qpair_abort_reqs = nvme_rdma_qpair_abort_reqs,	/* [한국어] outstanding 강제 abort */
	.qpair_reset = nvme_rdma_qpair_reset,	/* [한국어] no-op for RDMA */
	.qpair_submit_request = nvme_rdma_qpair_submit_request,	/* [한국어] I/O hot path */
	.qpair_process_completions = nvme_rdma_qpair_process_completions,	/* [한국어] 단독 큐페어 폴 */
	.qpair_iterate_requests = nvme_rdma_qpair_iterate_requests,
	.qpair_authenticate = nvme_rdma_qpair_authenticate,	/* [한국어] DH-CHAP 트리거 */
	.admin_qpair_abort_aers = nvme_rdma_admin_qpair_abort_aers,

	.poll_group_create = nvme_rdma_poll_group_create,
	.poll_group_connect_qpair = nvme_rdma_poll_group_connect_qpair,	/* [한국어] no-op (실제 attach는 qpair_init) */
	.poll_group_disconnect_qpair = nvme_rdma_poll_group_disconnect_qpair,
	.poll_group_add = nvme_rdma_poll_group_add,	/* [한국어] no-op */
	.poll_group_remove = nvme_rdma_poll_group_remove,	/* [한국어] 강제 disconnect 유발 */
	.poll_group_process_completions = nvme_rdma_poll_group_process_completions,	/* [한국어] 핵심 폴 hot path */
	.poll_group_check_disconnected_qpairs = nvme_rdma_poll_group_check_disconnected_qpairs,	/* [한국어] no-op (인터럽트용) */
	.poll_group_destroy = nvme_rdma_poll_group_destroy,
	.poll_group_get_stats = nvme_rdma_poll_group_get_stats,
	.poll_group_free_stats = nvme_rdma_poll_group_free_stats,
};

/* [한국어] vtable 등록 매크로: main() 진입 전에 nvme_transport.c의 전역 TAILQ에 RDMA ops를 삽입.
 * 이후 spdk_nvme_probe가 trtype=RDMA URI를 만나면 이 vtable로 디스패치. */
SPDK_NVME_TRANSPORT_REGISTER(rdma, &rdma_ops);
