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

static inline struct nvme_rdma_qpair *
nvme_rdma_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->trtype == SPDK_NVME_TRANSPORT_RDMA);
	return SPDK_CONTAINEROF(qpair, struct nvme_rdma_qpair, qpair);
}

static inline struct nvme_rdma_poll_group *
nvme_rdma_poll_group(struct spdk_nvme_transport_poll_group *group)
{
	return (SPDK_CONTAINEROF(group, struct nvme_rdma_poll_group, group));
}

static inline struct nvme_rdma_ctrlr *
nvme_rdma_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_RDMA);
	return SPDK_CONTAINEROF(ctrlr, struct nvme_rdma_ctrlr, ctrlr);
}

static inline struct spdk_nvme_rdma_req *
nvme_rdma_req_get(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_req *rdma_req;

	rdma_req = TAILQ_FIRST(&rqpair->free_reqs);
	if (spdk_likely(rdma_req)) {
		TAILQ_REMOVE(&rqpair->free_reqs, rdma_req, link);
	}

	return rdma_req;
}

static inline void
nvme_rdma_req_put(struct nvme_rdma_qpair *rqpair, struct spdk_nvme_rdma_req *rdma_req)
{
	rdma_req->completion_flags = 0;
	rdma_req->req = NULL;
	rdma_req->rdma_rsp = NULL;
	assert(rdma_req->transfer_cpl_cb == NULL);
	TAILQ_INSERT_HEAD(&rqpair->free_reqs, rdma_req, link);
}

static inline void
nvme_rdma_finish_data_transfer(struct spdk_nvme_rdma_req *rdma_req, int rc)
{
	spdk_memory_domain_data_cpl_cb cb = rdma_req->transfer_cpl_cb;

	SPDK_DEBUGLOG(nvme, "req %p, finish data transfer, rc %d\n", rdma_req, rc);
	rdma_req->transfer_cpl_cb = NULL;
	assert(cb);
	cb(rdma_req->transfer_cpl_cb_arg, rc);
}

static void
nvme_rdma_req_complete(struct spdk_nvme_rdma_req *rdma_req,
		       struct spdk_nvme_cpl *rsp,
		       bool print_on_error)
{
	struct nvme_request *req = rdma_req->req;
	struct nvme_rdma_qpair *rqpair;
	struct spdk_nvme_qpair *qpair;
	bool error, print_error;

	assert(req != NULL);

	qpair = req->qpair;
	rqpair = nvme_rdma_qpair(qpair);

	error = spdk_nvme_cpl_is_error(rsp);
	print_error = error && print_on_error && !qpair->ctrlr->opts.disable_error_logging;

	if (print_error) {
		spdk_nvme_qpair_print_command(qpair, &req->cmd);
	}

	if (print_error || SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
		spdk_nvme_qpair_print_completion(qpair, rsp);
	}

	assert(rqpair->num_outstanding_reqs > 0);
	rqpair->num_outstanding_reqs--;

	TAILQ_REMOVE(&rqpair->outstanding_reqs, rdma_req, link);

	nvme_complete_request(req->cb_fn, req->cb_arg, qpair, req, rsp);
	nvme_rdma_req_put(rqpair, rdma_req);
}

static const char *
nvme_rdma_cm_event_str_get(uint32_t event)
{
	if (event < SPDK_COUNTOF(rdma_cm_event_str)) {
		return rdma_cm_event_str[event];
	} else {
		return "Undefined";
	}
}


static int
nvme_rdma_qpair_process_cm_event(struct nvme_rdma_qpair *rqpair)
{
	struct rdma_cm_event				*event = rqpair->evt;
	struct spdk_nvmf_rdma_accept_private_data	*accept_data;
	int						rc = 0;

	if (event) {
		switch (event->event) {
		case RDMA_CM_EVENT_ADDR_RESOLVED:
		case RDMA_CM_EVENT_ADDR_ERROR:
		case RDMA_CM_EVENT_ROUTE_RESOLVED:
		case RDMA_CM_EVENT_ROUTE_ERROR:
			break;
		case RDMA_CM_EVENT_CONNECT_REQUEST:
			break;
		case RDMA_CM_EVENT_CONNECT_ERROR:
			break;
		case RDMA_CM_EVENT_UNREACHABLE:
		case RDMA_CM_EVENT_REJECTED:
			break;
		case RDMA_CM_EVENT_CONNECT_RESPONSE:
			rc = spdk_rdma_provider_qp_complete_connect(rqpair->rdma_qp);
		/* fall through */
		case RDMA_CM_EVENT_ESTABLISHED:
			rqpair->connected = true;
			accept_data = (struct spdk_nvmf_rdma_accept_private_data *)event->param.conn.private_data;
			if (accept_data == NULL) {
				rc = -1;
			} else {
				NVME_RQPAIR_DEBUGLOG(rqpair, "Requested queue depth %d. Target receive queue depth %d.\n",
						     rqpair->num_entries + 1, accept_data->crqsize);
			}
			break;
		case RDMA_CM_EVENT_DISCONNECTED:
			/* Disconnect qp, which will be moved to error state, so all outstanding
			 * work requests in the send qp will be flushed, otherwise, the outstanding
			 * wrs may not be completed forever.
			 */
			spdk_rdma_provider_qp_disconnect(rqpair->rdma_qp);
			rqpair->connected = false;
			rqpair->qpair.transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_REMOTE;
			break;
		case RDMA_CM_EVENT_DEVICE_REMOVAL:
			rqpair->qpair.transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
			rqpair->need_destroy = true;
			break;
		case RDMA_CM_EVENT_MULTICAST_JOIN:
		case RDMA_CM_EVENT_MULTICAST_ERROR:
			break;
		case RDMA_CM_EVENT_ADDR_CHANGE:
			rqpair->qpair.transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
			break;
		case RDMA_CM_EVENT_TIMEWAIT_EXIT:
			break;
		default:
			NVME_RQPAIR_ERRLOG(rqpair, "Unexpected Acceptor Event [%d]\n", event->event);
			break;
		}
		rqpair->evt = NULL;
		rdma_ack_cm_event(event);
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
static int
nvme_rdma_poll_events(struct nvme_rdma_ctrlr *rctrlr)
{
	struct nvme_rdma_cm_event_entry	*entry, *tmp;
	struct nvme_rdma_qpair		*event_qpair;
	struct rdma_cm_event		*event;
	struct rdma_event_channel	*channel = rctrlr->cm_channel;

	STAILQ_FOREACH_SAFE(entry, &rctrlr->pending_cm_events, link, tmp) {
		event_qpair = entry->evt->id->context;
		if (event_qpair->evt == NULL) {
			event_qpair->evt = entry->evt;
			STAILQ_REMOVE(&rctrlr->pending_cm_events, entry, nvme_rdma_cm_event_entry, link);
			STAILQ_INSERT_HEAD(&rctrlr->free_cm_events, entry, link);
		}
	}

	while (rdma_get_cm_event(channel, &event) == 0) {
		event_qpair = event->id->context;
		if (event_qpair->evt == NULL) {
			event_qpair->evt = event;
		} else {
			assert(rctrlr == nvme_rdma_ctrlr(event_qpair->qpair.ctrlr));
			entry = STAILQ_FIRST(&rctrlr->free_cm_events);
			if (entry == NULL) {
				rdma_ack_cm_event(event);
				return -ENOMEM;
			}
			STAILQ_REMOVE_HEAD(&rctrlr->free_cm_events, link);
			entry->evt = event;
			STAILQ_INSERT_TAIL(&rctrlr->pending_cm_events, entry, link);
		}
	}

	/* rdma_get_cm_event() returns -1 on error. If an error occurs, errno
	 * will be set to indicate the failure reason. So return negated errno here.
	 */
	return -errno;
}

static int
nvme_rdma_validate_cm_event(enum rdma_cm_event_type expected_evt_type,
			    struct rdma_cm_event *reaped_evt)
{
	int rc = -EBADMSG;

	if (expected_evt_type == reaped_evt->event) {
		return 0;
	}

	switch (expected_evt_type) {
	case RDMA_CM_EVENT_ESTABLISHED:
		/*
		 * There is an enum ib_cm_rej_reason in the kernel headers that sets 10 as
		 * IB_CM_REJ_STALE_CONN. I can't find the corresponding userspace but we get
		 * the same values here.
		 */
		if (reaped_evt->event == RDMA_CM_EVENT_REJECTED && reaped_evt->status == 10) {
			rc = -ESTALE;
		} else if (reaped_evt->event == RDMA_CM_EVENT_CONNECT_RESPONSE) {
			/*
			 *  If we are using a qpair which is not created using rdma cm API
			 *  then we will receive RDMA_CM_EVENT_CONNECT_RESPONSE instead of
			 *  RDMA_CM_EVENT_ESTABLISHED.
			 */
			return 0;
		}
		break;
	default:
		break;
	}

	SPDK_ERRLOG("Expected %s but received %s (%d) from CM event channel (status = %d)\n",
		    nvme_rdma_cm_event_str_get(expected_evt_type),
		    nvme_rdma_cm_event_str_get(reaped_evt->event), reaped_evt->event,
		    reaped_evt->status);
	return rc;
}

static int
nvme_rdma_process_event_start(struct nvme_rdma_qpair *rqpair,
			      enum rdma_cm_event_type evt,
			      nvme_rdma_cm_event_cb evt_cb)
{
	int	rc;

	assert(evt_cb != NULL);

	if (rqpair->evt != NULL) {
		rc = nvme_rdma_qpair_process_cm_event(rqpair);
		if (rc) {
			return rc;
		}
	}

	rqpair->expected_evt_type = evt;
	rqpair->evt_cb = evt_cb;
	rqpair->evt_timeout_ticks = (g_spdk_nvme_transport_opts.rdma_cm_event_timeout_ms * 1000 *
				     spdk_get_ticks_hz()) / SPDK_SEC_TO_USEC + spdk_get_ticks();

	return 0;
}

static int
nvme_rdma_process_event_poll(struct nvme_rdma_qpair *rqpair)
{
	struct nvme_rdma_ctrlr	*rctrlr;
	int	rc = 0, rc2;

	rctrlr = nvme_rdma_ctrlr(rqpair->qpair.ctrlr);
	assert(rctrlr != NULL);

	if (!rqpair->evt && spdk_get_ticks() < rqpair->evt_timeout_ticks) {
		rc = nvme_rdma_poll_events(rctrlr);
		if (rc == -EAGAIN || rc == -EWOULDBLOCK) {
			return rc;
		}
	}

	if (rqpair->evt == NULL) {
		rc = -EADDRNOTAVAIL;
		goto exit;
	}

	rc = nvme_rdma_validate_cm_event(rqpair->expected_evt_type, rqpair->evt);

	rc2 = nvme_rdma_qpair_process_cm_event(rqpair);
	/* bad message takes precedence over the other error codes from processing the event. */
	rc = rc == 0 ? rc2 : rc;

exit:
	assert(rqpair->evt_cb != NULL);
	return rqpair->evt_cb(rqpair, rc);
}

static int
nvme_rdma_resize_cq(struct nvme_rdma_qpair *rqpair, struct nvme_rdma_poller *poller)
{
	int	current_num_wc, required_num_wc;
	int	max_cq_size;

	required_num_wc = poller->required_num_wc + WC_PER_QPAIR(rqpair->num_entries);
	current_num_wc = poller->current_num_wc;
	if (current_num_wc < required_num_wc) {
		current_num_wc = spdk_max(current_num_wc * 2, required_num_wc);
	}

	max_cq_size = g_spdk_nvme_transport_opts.rdma_max_cq_size;
	if (max_cq_size != 0 && current_num_wc > max_cq_size) {
		current_num_wc = max_cq_size;
	}

	if (poller->current_num_wc != current_num_wc) {
		NVME_RQPAIR_DEBUGLOG(rqpair, "Resize RDMA CQ from %d to %d\n", poller->current_num_wc,
				     current_num_wc);
		if (ibv_resize_cq(poller->cq, current_num_wc)) {
			NVME_RQPAIR_ERRLOG(rqpair, "RDMA CQ resize failed: errno %d: %s\n", errno, spdk_strerror(errno));
			return -1;
		}

		poller->current_num_wc = current_num_wc;
	}

	poller->required_num_wc = required_num_wc;
	return 0;
}

static int
nvme_rdma_qpair_set_poller(struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair          *rqpair = nvme_rdma_qpair(qpair);
	struct nvme_rdma_poll_group     *group = nvme_rdma_poll_group(qpair->poll_group);
	struct nvme_rdma_poller         *poller;

	assert(rqpair->cq == NULL);

	poller = nvme_rdma_poll_group_get_poller(group, rqpair->cm_id->verbs);
	if (!poller) {
		NVME_RQPAIR_ERRLOG(rqpair, "Unable to find a cq on poll group %p\n", qpair->poll_group);
		return -EINVAL;
	}

	if (!poller->srq) {
		if (nvme_rdma_resize_cq(rqpair, poller)) {
			nvme_rdma_poll_group_put_poller(group, poller);
			return -EPROTO;
		}
	}

	rqpair->cq = poller->cq;
	rqpair->srq = poller->srq;
	if (rqpair->srq) {
		rqpair->rsps = poller->rsps;
	}
	rqpair->poller = poller;
	return 0;
}

static void
nvme_rdma_qpair_release_poller(struct nvme_rdma_qpair *rqpair)
{
	struct nvme_rdma_poller *poller = rqpair->poller;
	struct nvme_rdma_poll_group *group = poller->group;

	assert(group);

	if (!poller->srq) {
		assert(rqpair->poller->required_num_wc >= WC_PER_QPAIR(rqpair->num_entries));
		poller->required_num_wc -= WC_PER_QPAIR(rqpair->num_entries);
	}

	nvme_rdma_poll_group_put_poller(group, poller);
	rqpair->poller = NULL;
	rqpair->cq = NULL;
}

static int
nvme_rdma_qpair_init(struct nvme_rdma_qpair *rqpair)
{
	int			rc;
	struct spdk_rdma_provider_qp_init_attr	attr = {};
	struct ibv_device_attr	dev_attr;
	struct nvme_rdma_ctrlr	*rctrlr;
	uint32_t num_cqe, max_num_cqe;

	rc = ibv_query_device(rqpair->cm_id->verbs, &dev_attr);
	if (rc != 0) {
		NVME_RQPAIR_ERRLOG(rqpair, "Failed to query RDMA device attributes.\n");
		return -1;
	}

	if (rqpair->qpair.poll_group) {
		assert(!rqpair->cq);
		rc = nvme_rdma_qpair_set_poller(&rqpair->qpair);
		if (rc) {
			NVME_RQPAIR_ERRLOG(rqpair, "Unable to activate the rdmaqpair.\n");
			return -1;
		}
		assert(rqpair->cq);
	} else {
		num_cqe = rqpair->num_entries * 2;
		max_num_cqe = g_spdk_nvme_transport_opts.rdma_max_cq_size;
		if (max_num_cqe != 0 && num_cqe > max_num_cqe) {
			num_cqe = max_num_cqe;
		}
		rqpair->cq = ibv_create_cq(rqpair->cm_id->verbs, num_cqe, rqpair, NULL, 0);
		if (!rqpair->cq) {
			NVME_RQPAIR_ERRLOG(rqpair, "Unable to create completion queue: errno %d: %s\n", errno,
					   spdk_strerror(errno));
			return -1;
		}
	}

	rctrlr = nvme_rdma_ctrlr(rqpair->qpair.ctrlr);
	if (g_nvme_hooks.get_ibv_pd) {
		attr.pd = g_nvme_hooks.get_ibv_pd(&rctrlr->ctrlr.trid, rqpair->cm_id->verbs);
	} else {
		attr.pd = spdk_rdma_utils_get_pd(rqpair->cm_id->verbs);
	}

	attr.stats		= rqpair->poller ? &rqpair->poller->stats.rdma_stats : NULL;
	attr.send_cq		= rqpair->cq;
	attr.recv_cq		= rqpair->cq;
	attr.cap.max_send_wr	= rqpair->num_entries; /* SEND operations */
	if (rqpair->srq) {
		attr.srq	= rqpair->srq->srq;
	} else {
		attr.cap.max_recv_wr = rqpair->num_entries; /* RECV operations */
	}
	attr.cap.max_send_sge	= spdk_min(NVME_RDMA_DEFAULT_TX_SGE, dev_attr.max_sge);
	attr.cap.max_recv_sge	= spdk_min(NVME_RDMA_DEFAULT_RX_SGE, dev_attr.max_sge);
	attr.domain_transfer	= spdk_rdma_provider_accel_sequence_supported() ?
				  nvme_rdma_memory_domain_transfer_data : NULL;

	rqpair->rdma_qp = spdk_rdma_provider_qp_create(rqpair->cm_id, &attr);

	if (!rqpair->rdma_qp) {
		return -1;
	}

	/* ibv_create_qp will change the values in attr.cap. Make sure we store the proper value. */
	rqpair->max_send_sge = spdk_min(NVME_RDMA_DEFAULT_TX_SGE, attr.cap.max_send_sge);
	rqpair->current_num_sends = 0;

	rqpair->cm_id->context = rqpair;

	return 0;
}

static void
nvme_rdma_reset_failed_sends(struct nvme_rdma_qpair *rqpair,
			     struct ibv_send_wr *bad_send_wr)
{
	while (bad_send_wr != NULL) {
		assert(rqpair->current_num_sends > 0);
		rqpair->current_num_sends--;
		bad_send_wr = bad_send_wr->next;
	}
}

static void
nvme_rdma_reset_failed_recvs(struct nvme_rdma_rsps *rsps,
			     struct ibv_recv_wr *bad_recv_wr, int rc)
{
	SPDK_ERRLOG("Failed to post WRs on receive queue, errno %d (%s), bad_wr %p\n",
		    rc, spdk_strerror(rc), bad_recv_wr);
	while (bad_recv_wr != NULL) {
		assert(rsps->current_num_recvs > 0);
		rsps->current_num_recvs--;
		bad_recv_wr = bad_recv_wr->next;
	}
}

static inline int
nvme_rdma_qpair_submit_sends(struct nvme_rdma_qpair *rqpair)
{
	struct ibv_send_wr *bad_send_wr = NULL;
	int rc;

	rc = spdk_rdma_provider_qp_flush_send_wrs(rqpair->rdma_qp, &bad_send_wr);

	if (spdk_unlikely(rc)) {
		NVME_RQPAIR_ERRLOG(rqpair, "Failed to post WRs on send queue, errno %d (%s), bad_wr %p\n",
				   rc, spdk_strerror(rc), bad_send_wr);
		nvme_rdma_reset_failed_sends(rqpair, bad_send_wr);
	}

	return rc;
}

static inline int
nvme_rdma_qpair_submit_recvs(struct nvme_rdma_qpair *rqpair)
{
	struct ibv_recv_wr *bad_recv_wr;
	int rc = 0;

	rc = spdk_rdma_provider_qp_flush_recv_wrs(rqpair->rdma_qp, &bad_recv_wr);
	if (spdk_unlikely(rc)) {
		nvme_rdma_reset_failed_recvs(rqpair->rsps, bad_recv_wr, rc);
	}

	return rc;
}

static inline int
nvme_rdma_poller_submit_recvs(struct nvme_rdma_poller *poller)
{
	struct ibv_recv_wr *bad_recv_wr;
	int rc;

	rc = spdk_rdma_provider_srq_flush_recv_wrs(poller->srq, &bad_recv_wr);
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

static void
nvme_rdma_free_rsps(struct nvme_rdma_rsps *rsps)
{
	if (!rsps) {
		return;
	}

	spdk_free(rsps->rsps);
	spdk_free(rsps->rsp_sgls);
	spdk_free(rsps->rsp_recv_wrs);
	free(rsps);
}

static struct nvme_rdma_rsps *
nvme_rdma_create_rsps(struct nvme_rdma_rsp_opts *opts)
{
	struct nvme_rdma_rsps *rsps;
	struct spdk_rdma_utils_memory_translation translation;
	uint16_t i;
	int rc;

	rsps = calloc(1, sizeof(*rsps));
	if (!rsps) {
		SPDK_ERRLOG("Failed to allocate rsps object\n");
		return NULL;
	}

	rsps->rsp_sgls = spdk_zmalloc(opts->num_entries * sizeof(*rsps->rsp_sgls), 0, NULL,
				      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!rsps->rsp_sgls) {
		SPDK_ERRLOG("Failed to allocate rsp_sgls\n");
		goto fail;
	}

	rsps->rsp_recv_wrs = spdk_zmalloc(opts->num_entries * sizeof(*rsps->rsp_recv_wrs), 0, NULL,
					  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!rsps->rsp_recv_wrs) {
		SPDK_ERRLOG("Failed to allocate rsp_recv_wrs\n");
		goto fail;
	}

	rsps->rsps = spdk_zmalloc(opts->num_entries * sizeof(*rsps->rsps), 0, NULL,
				  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!rsps->rsps) {
		SPDK_ERRLOG("can not allocate rdma rsps\n");
		goto fail;
	}

	for (i = 0; i < opts->num_entries; i++) {
		struct ibv_sge *rsp_sgl = &rsps->rsp_sgls[i];
		struct spdk_nvme_rdma_rsp *rsp = &rsps->rsps[i];
		struct ibv_recv_wr *recv_wr = &rsps->rsp_recv_wrs[i];

		rsp->rqpair = opts->rqpair;
		rsp->rdma_wr.type = RDMA_WR_TYPE_RECV;
		rsp->recv_wr = recv_wr;
		rsp_sgl->addr = (uint64_t)rsp;
		rsp_sgl->length = sizeof(struct spdk_nvme_cpl);
		rc = spdk_rdma_utils_get_translation(opts->mr_map, rsp, sizeof(*rsp), &translation);
		if (rc) {
			goto fail;
		}
		rsp_sgl->lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);

		recv_wr->wr_id = (uint64_t)&rsp->rdma_wr;
		recv_wr->next = NULL;
		recv_wr->sg_list = rsp_sgl;
		recv_wr->num_sge = 1;

		nvme_rdma_trace_ibv_sge(recv_wr->sg_list);

		if (opts->rqpair) {
			spdk_rdma_provider_qp_queue_recv_wrs(opts->rqpair->rdma_qp, recv_wr);
		} else {
			spdk_rdma_provider_srq_queue_recv_wrs(opts->srq, recv_wr);
		}
	}

	rsps->num_entries = opts->num_entries;
	rsps->current_num_recvs = opts->num_entries;

	return rsps;
fail:
	nvme_rdma_free_rsps(rsps);
	return NULL;
}

static void
nvme_rdma_free_reqs(struct nvme_rdma_qpair *rqpair)
{
	if (!rqpair->rdma_reqs) {
		return;
	}

	spdk_free(rqpair->cmds);
	rqpair->cmds = NULL;

	spdk_free(rqpair->rdma_reqs);
	rqpair->rdma_reqs = NULL;
}

static int
nvme_rdma_create_reqs(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_rdma_utils_memory_translation translation;
	uint16_t i;
	int rc;

	assert(!rqpair->rdma_reqs);
	rqpair->rdma_reqs = spdk_zmalloc(rqpair->num_entries * sizeof(struct spdk_nvme_rdma_req), 0, NULL,
					 SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (rqpair->rdma_reqs == NULL) {
		NVME_RQPAIR_ERRLOG(rqpair, "Failed to allocate rdma_reqs\n");
		goto fail;
	}

	assert(!rqpair->cmds);
	rqpair->cmds = spdk_zmalloc(rqpair->num_entries * sizeof(*rqpair->cmds), 0, NULL,
				    SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!rqpair->cmds) {
		NVME_RQPAIR_ERRLOG(rqpair, "Failed to allocate RDMA cmds\n");
		goto fail;
	}

	TAILQ_INIT(&rqpair->free_reqs);
	TAILQ_INIT(&rqpair->outstanding_reqs);
	for (i = 0; i < rqpair->num_entries; i++) {
		struct spdk_nvme_rdma_req	*rdma_req;
		struct spdk_nvmf_cmd		*cmd;

		rdma_req = &rqpair->rdma_reqs[i];
		rdma_req->rdma_wr.type = RDMA_WR_TYPE_SEND;
		cmd = &rqpair->cmds[i];

		rdma_req->id = i;

		rc = spdk_rdma_utils_get_translation(rqpair->mr_map, cmd, sizeof(*cmd), &translation);
		if (rc) {
			goto fail;
		}
		rdma_req->send_sgl[0].lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);

		/* The first RDMA sgl element will always point
		 * at this data structure. Depending on whether
		 * an NVMe-oF SGL is required, the length of
		 * this element may change. */
		rdma_req->send_sgl[0].addr = (uint64_t)cmd;
		rdma_req->send_wr.wr_id = (uint64_t)&rdma_req->rdma_wr;
		rdma_req->send_wr.next = NULL;
		rdma_req->send_wr.opcode = IBV_WR_SEND;
		rdma_req->send_wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->send_wr.sg_list = rdma_req->send_sgl;
		rdma_req->send_wr.imm_data = 0;

		TAILQ_INSERT_TAIL(&rqpair->free_reqs, rdma_req, link);
	}

	return 0;
fail:
	nvme_rdma_free_reqs(rqpair);
	return -ENOMEM;
}

static int nvme_rdma_connect(struct nvme_rdma_qpair *rqpair);

static int
nvme_rdma_route_resolved(struct nvme_rdma_qpair *rqpair, int ret)
{
	if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "RDMA route resolution error\n");
		return -1;
	}

	ret = nvme_rdma_qpair_init(rqpair);
	if (ret < 0) {
		NVME_RQPAIR_ERRLOG(rqpair, "nvme_rdma_qpair_init() failed\n");
		return -1;
	}

	return nvme_rdma_connect(rqpair);
}

static int
nvme_rdma_addr_resolved(struct nvme_rdma_qpair *rqpair, int ret)
{
	if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "RDMA address resolution error\n");
		return -1;
	}

	if (rqpair->qpair.ctrlr->opts.transport_ack_timeout != SPDK_NVME_TRANSPORT_ACK_TIMEOUT_DISABLED) {
#ifdef SPDK_CONFIG_RDMA_SET_ACK_TIMEOUT
		uint8_t timeout = rqpair->qpair.ctrlr->opts.transport_ack_timeout;
		ret = rdma_set_option(rqpair->cm_id, RDMA_OPTION_ID,
				      RDMA_OPTION_ID_ACK_TIMEOUT,
				      &timeout, sizeof(timeout));
		if (ret) {
			NVME_RQPAIR_NOTICELOG(rqpair, "Can't apply RDMA_OPTION_ID_ACK_TIMEOUT %d, ret %d\n", timeout, ret);
		}
#else
		NVME_RQPAIR_DEBUGLOG(rqpair, "transport_ack_timeout is not supported\n");
#endif
	}

	if (rqpair->qpair.ctrlr->opts.transport_tos != SPDK_NVME_TRANSPORT_TOS_DISABLED) {
#ifdef SPDK_CONFIG_RDMA_SET_TOS
		uint8_t tos = rqpair->qpair.ctrlr->opts.transport_tos;
		ret = rdma_set_option(rqpair->cm_id, RDMA_OPTION_ID, RDMA_OPTION_ID_TOS, &tos, sizeof(tos));
		if (ret) {
			NVME_RQPAIR_NOTICELOG(rqpair, "Can't apply RDMA_OPTION_ID_TOS %u, ret %d\n", tos, ret);
		}
#else
		NVME_RQPAIR_DEBUGLOG(rqpair, "transport_tos is not supported\n");
#endif
	}

	ret = rdma_resolve_route(rqpair->cm_id, NVME_RDMA_TIME_OUT_IN_MS);
	if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "rdma_resolve_route\n");
		return ret;
	}

	return nvme_rdma_process_event_start(rqpair, RDMA_CM_EVENT_ROUTE_RESOLVED,
					     nvme_rdma_route_resolved);
}

static int
nvme_rdma_resolve_addr(struct nvme_rdma_qpair *rqpair,
		       struct sockaddr *src_addr,
		       struct sockaddr *dst_addr)
{
	int ret;

	if (src_addr) {
		int reuse = 1;

		ret = rdma_set_option(rqpair->cm_id, RDMA_OPTION_ID, RDMA_OPTION_ID_REUSEADDR,
				      &reuse, sizeof(reuse));
		if (ret) {
			NVME_RQPAIR_NOTICELOG(rqpair, "Can't apply RDMA_OPTION_ID_REUSEADDR %d, ret %d\n", reuse, ret);
			/* It is likely that rdma_resolve_addr() returns -EADDRINUSE, but
			 * we may missing something. We rely on rdma_resolve_addr().
			 */
		}
	}

	ret = rdma_resolve_addr(rqpair->cm_id, src_addr, dst_addr,
				NVME_RDMA_TIME_OUT_IN_MS);
	if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "rdma_resolve_addr, %d\n", errno);
		return ret;
	}

	return nvme_rdma_process_event_start(rqpair, RDMA_CM_EVENT_ADDR_RESOLVED,
					     nvme_rdma_addr_resolved);
}

static int nvme_rdma_stale_conn_retry(struct nvme_rdma_qpair *rqpair);

static int
nvme_rdma_connect_established(struct nvme_rdma_qpair *rqpair, int ret)
{
	struct nvme_rdma_rsp_opts opts = {};

	if (ret == -ESTALE) {
		return nvme_rdma_stale_conn_retry(rqpair);
	} else if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "RDMA connect error %d\n", ret);
		return ret;
	}

	assert(!rqpair->mr_map);
	rqpair->mr_map = spdk_rdma_utils_create_mem_map(rqpair->rdma_qp->qp->pd, &g_nvme_hooks,
			 IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	if (!rqpair->mr_map) {
		NVME_RQPAIR_ERRLOG(rqpair, "Unable to register RDMA memory translation map\n");
		return -1;
	}

	ret = nvme_rdma_create_reqs(rqpair);
	if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "Unable to create rqpair RDMA requests: %d\n", ret);
		return -1;
	}
	NVME_RQPAIR_DEBUGLOG(rqpair, "RDMA requests created\n");

	if (!rqpair->srq) {
		opts.num_entries = rqpair->num_entries;
		opts.rqpair = rqpair;
		opts.srq = NULL;
		opts.mr_map = rqpair->mr_map;

		assert(!rqpair->rsps);
		rqpair->rsps = nvme_rdma_create_rsps(&opts);
		if (!rqpair->rsps) {
			NVME_RQPAIR_ERRLOG(rqpair, "Unable to create rqpair RDMA responses\n");
			return -1;
		}
		NVME_RQPAIR_DEBUGLOG(rqpair, "RDMA responses created\n");

		ret = nvme_rdma_qpair_submit_recvs(rqpair);
		if (ret) {
			NVME_RQPAIR_ERRLOG(rqpair, "Unable to submit rqpair RDMA responses: %d\n", ret);
			return -1;
		}
		NVME_RQPAIR_DEBUGLOG(rqpair, "RDMA responses submitted\n");
	}

	rqpair->state = NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_SEND;

	return 0;
}

static int
nvme_rdma_connect(struct nvme_rdma_qpair *rqpair)
{
	struct rdma_conn_param				param = {};
	struct spdk_nvmf_rdma_request_private_data	request_data = {};
	struct ibv_device_attr				attr;
	int						ret;
	struct spdk_nvme_ctrlr				*ctrlr;

	ret = ibv_query_device(rqpair->cm_id->verbs, &attr);
	if (ret != 0) {
		NVME_RQPAIR_ERRLOG(rqpair, "Failed to query RDMA device attributes.\n");
		return ret;
	}

	param.responder_resources = attr.max_qp_rd_atom;

	ctrlr = rqpair->qpair.ctrlr;
	if (!ctrlr) {
		return -1;
	}

	request_data.qid = rqpair->qpair.id;
	request_data.hrqsize = rqpair->num_entries + 1;
	request_data.hsqsize = rqpair->num_entries;
	request_data.cntlid = ctrlr->cntlid;

	param.private_data = &request_data;
	param.private_data_len = sizeof(request_data);
	param.retry_count = ctrlr->opts.transport_retry_count;
	param.rnr_retry_count = 7;

	/* Fields below are ignored by rdma cm if qpair has been
	 * created using rdma cm API. */
	param.srq = 0;
	param.qp_num = rqpair->rdma_qp->qp->qp_num;

	ret = rdma_connect(rqpair->cm_id, &param);
	if (ret) {
		NVME_RQPAIR_ERRLOG(rqpair, "nvme rdma connect error\n");
		return ret;
	}

	ctrlr->numa.id_valid = 1;
	ctrlr->numa.id = spdk_rdma_cm_id_get_numa_id(rqpair->cm_id);

	return nvme_rdma_process_event_start(rqpair, RDMA_CM_EVENT_ESTABLISHED,
					     nvme_rdma_connect_established);
}

static int
nvme_rdma_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct sockaddr_storage dst_addr;
	struct sockaddr_storage src_addr;
	bool src_addr_specified;
	long int port, src_port = 0;
	int rc;
	struct nvme_rdma_ctrlr *rctrlr;
	struct nvme_rdma_qpair *rqpair;
	struct nvme_rdma_poll_group *group;
	int family;

	rqpair = nvme_rdma_qpair(qpair);
	rctrlr = nvme_rdma_ctrlr(ctrlr);
	assert(rctrlr != NULL);

	switch (ctrlr->trid.adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		family = AF_INET;
		break;
	case SPDK_NVMF_ADRFAM_IPV6:
		family = AF_INET6;
		break;
	default:
		NVME_RQPAIR_ERRLOG(rqpair, "Unhandled ADRFAM %d\n", ctrlr->trid.adrfam);
		return -1;
	}

	NVME_RQPAIR_DEBUGLOG(rqpair, "adrfam %d ai_family %d\n", ctrlr->trid.adrfam, family);

	memset(&dst_addr, 0, sizeof(dst_addr));

	NVME_RQPAIR_DEBUGLOG(rqpair, "trsvcid is %s\n", ctrlr->trid.trsvcid);
	rc = nvme_parse_addr(&dst_addr, family, ctrlr->trid.traddr, ctrlr->trid.trsvcid, &port);
	if (rc != 0) {
		NVME_RQPAIR_ERRLOG(rqpair, "dst_addr nvme_parse_addr() failed\n");
		return -1;
	}

	if (ctrlr->opts.src_addr[0] || ctrlr->opts.src_svcid[0]) {
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
		src_addr_specified = false;
	}

	rc = rdma_create_id(rctrlr->cm_channel, &rqpair->cm_id, rqpair, RDMA_PS_TCP);
	if (rc < 0) {
		NVME_RQPAIR_ERRLOG(rqpair, "rdma_create_id() failed\n");
		return -1;
	}

	rc = nvme_rdma_resolve_addr(rqpair,
				    src_addr_specified ? (struct sockaddr *)&src_addr : NULL,
				    (struct sockaddr *)&dst_addr);
	if (rc < 0) {
		NVME_RQPAIR_ERRLOG(rqpair, "nvme_rdma_resolve_addr() failed\n");
		return -1;
	}

	rqpair->state = NVME_RDMA_QPAIR_STATE_INITIALIZING;

	if (qpair->poll_group != NULL && TAILQ_ENTRY_NOT_ENQUEUED(rqpair, link_connecting)) {
		group = nvme_rdma_poll_group(qpair->poll_group);
		TAILQ_INSERT_TAIL(&group->connecting_qpairs, rqpair, link_connecting);
	}

	return 0;
}

static int
nvme_rdma_stale_conn_reconnect(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;

	if (spdk_get_ticks() < rqpair->evt_timeout_ticks) {
		return -EAGAIN;
	}

	return nvme_rdma_ctrlr_connect_qpair(qpair->ctrlr, qpair);
}

static int
nvme_rdma_ctrlr_connect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	int rc;

	if (qpair->in_connect_poll) {
		return -EAGAIN;
	}

	qpair->in_connect_poll = true;

	switch (rqpair->state) {
	case NVME_RDMA_QPAIR_STATE_INVALID:
		rc = -EAGAIN;
		break;

	case NVME_RDMA_QPAIR_STATE_INITIALIZING:
	case NVME_RDMA_QPAIR_STATE_EXITING:
		if (!nvme_qpair_is_admin_queue(qpair)) {
			nvme_ctrlr_lock(ctrlr);
		}

		rc = nvme_rdma_process_event_poll(rqpair);

		if (!nvme_qpair_is_admin_queue(qpair)) {
			nvme_ctrlr_unlock(ctrlr);
		}

		if (rc == 0) {
			rc = -EAGAIN;
		}
		qpair->in_connect_poll = false;

		return rc;

	case NVME_RDMA_QPAIR_STATE_STALE_CONN:
		rc = nvme_rdma_stale_conn_reconnect(rqpair);
		if (rc == 0) {
			rc = -EAGAIN;
		}
		break;
	case NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_SEND:
		rc = nvme_fabric_qpair_connect_async(qpair, rqpair->num_entries + 1);
		if (rc == 0) {
			rqpair->state = NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_POLL;
			rc = -EAGAIN;
		} else {
			NVME_RQPAIR_ERRLOG(rqpair, "Failed to send an NVMe-oF Fabric CONNECT command\n");
		}
		break;
	case NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_POLL:
		rc = nvme_fabric_qpair_connect_poll(qpair);
		if (rc == 0) {
			if (nvme_fabric_qpair_auth_required(qpair)) {
				rc = nvme_fabric_qpair_authenticate_async(qpair);
				if (rc == 0) {
					rqpair->state = NVME_RDMA_QPAIR_STATE_AUTHENTICATING;
					rc = -EAGAIN;
				}
			} else {
				rqpair->state = NVME_RDMA_QPAIR_STATE_RUNNING;
				nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
			}
		} else if (rc != -EAGAIN) {
			NVME_RQPAIR_ERRLOG(rqpair, "Failed to poll NVMe-oF Fabric CONNECT command\n");
		}
		break;
	case NVME_RDMA_QPAIR_STATE_AUTHENTICATING:
		rc = nvme_fabric_qpair_authenticate_poll(qpair);
		if (rc == 0) {
			rqpair->state = NVME_RDMA_QPAIR_STATE_RUNNING;
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
		}
		break;
	case NVME_RDMA_QPAIR_STATE_RUNNING:
		rc = 0;
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	qpair->in_connect_poll = false;
	return rc;
}

static inline int
nvme_rdma_get_memory_translation(struct nvme_request *req, struct nvme_rdma_qpair *rqpair,
				 struct nvme_rdma_memory_translation_ctx *_ctx)
{
	struct spdk_memory_domain_translation_ctx ctx;
	struct spdk_memory_domain_translation_result dma_translation = {.iov_count = 0};
	struct spdk_rdma_utils_memory_translation rdma_translation;
	int rc;

	assert(req);
	assert(rqpair);
	assert(_ctx);

	if (req->payload.opts && req->payload.opts->memory_domain) {
		ctx.size = sizeof(struct spdk_memory_domain_translation_ctx);
		ctx.rdma.ibv_qp = rqpair->rdma_qp->qp;
		dma_translation.size = sizeof(struct spdk_memory_domain_translation_result);

		rc = spdk_memory_domain_translate_data(req->payload.opts->memory_domain,
						       req->payload.opts->memory_domain_ctx,
						       rqpair->rdma_qp->domain, &ctx, _ctx->addr,
						       _ctx->length, &dma_translation);
		if (spdk_unlikely(rc) || dma_translation.iov_count != 1) {
			NVME_RQPAIR_ERRLOG(rqpair, "DMA memory translation failed, rc %d, iov count %u\n", rc,
					   dma_translation.iov_count);
			return rc;
		}

		_ctx->lkey = dma_translation.rdma.lkey;
		_ctx->rkey = dma_translation.rdma.rkey;
		_ctx->addr = dma_translation.iov.iov_base;
		_ctx->length = dma_translation.iov.iov_len;
	} else {
		rc = spdk_rdma_utils_get_translation(rqpair->mr_map, _ctx->addr, _ctx->length, &rdma_translation);
		if (spdk_unlikely(rc)) {
			NVME_RQPAIR_ERRLOG(rqpair, "RDMA memory translation failed, rc %d\n", rc);
			return rc;
		}
		if (rdma_translation.translation_type == SPDK_RDMA_UTILS_TRANSLATION_MR) {
			_ctx->lkey = rdma_translation.mr_or_key.mr->lkey;
			_ctx->rkey = rdma_translation.mr_or_key.mr->rkey;
		} else {
			_ctx->lkey = _ctx->rkey = (uint32_t)rdma_translation.mr_or_key.key;
		}
	}

	return 0;
}


/*
 * Build SGL describing empty payload.
 */
static int
nvme_rdma_build_null_request(struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);

	/* The RDMA SGL needs one element describing the NVMe command. */
	rdma_req->send_wr.num_sge = 1;

	req->cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	req->cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	req->cmd.dptr.sgl1.keyed.length = 0;
	req->cmd.dptr.sgl1.keyed.key = 0;
	req->cmd.dptr.sgl1.address = 0;

	return 0;
}

static inline void
nvme_rdma_configure_contig_inline_request(struct spdk_nvme_rdma_req *rdma_req,
		struct nvme_request *req, struct nvme_rdma_memory_translation_ctx *ctx)
{
	rdma_req->send_sgl[1].lkey = ctx->lkey;

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);

	rdma_req->send_sgl[1].addr = (uint64_t)ctx->addr;
	rdma_req->send_sgl[1].length = (uint32_t)ctx->length;

	/* The RDMA SGL contains two elements. The first describes
	 * the NVMe command and the second describes the data
	 * payload. */
	rdma_req->send_wr.num_sge = 2;

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
	req->cmd.dptr.sgl1.unkeyed.length = (uint32_t)ctx->length;
	/* Inline only supported for icdoff == 0 currently.  This function will
	 * not get called for controllers with other values. */
	req->cmd.dptr.sgl1.address = (uint64_t)0;
}

/*
 * Build inline SGL describing contiguous payload buffer.
 */
static inline int
nvme_rdma_build_contig_inline_request(struct nvme_rdma_qpair *rqpair,
				      struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;
	struct nvme_rdma_memory_translation_ctx ctx = {
		.addr = (uint8_t *)req->payload.contig_or_cb_arg + req->payload_offset,
		.length = req->payload_size
	};
	int rc;

	assert(ctx.length != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	rc = nvme_rdma_get_memory_translation(req, rqpair, &ctx);
	if (spdk_unlikely(rc)) {
		return -1;
	}

	nvme_rdma_configure_contig_inline_request(rdma_req, req, &ctx);

	return 0;
}

static inline void
nvme_rdma_configure_contig_request(struct spdk_nvme_rdma_req *rdma_req, struct nvme_request *req,
				   struct nvme_rdma_memory_translation_ctx *ctx)
{
	req->cmd.dptr.sgl1.keyed.key = ctx->rkey;

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);

	/* The RDMA SGL needs one element describing the NVMe command. */
	rdma_req->send_wr.num_sge = 1;

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	req->cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	req->cmd.dptr.sgl1.keyed.length = (uint32_t)ctx->length;
	req->cmd.dptr.sgl1.address = (uint64_t)ctx->addr;
}

/*
 * Build SGL describing contiguous payload buffer.
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

	if (spdk_unlikely(req->payload_size > NVME_RDMA_MAX_KEYED_SGL_LENGTH)) {
		NVME_RQPAIR_ERRLOG(rqpair, "SGL length %u exceeds max keyed SGL block size %u\n", req->payload_size,
				   NVME_RDMA_MAX_KEYED_SGL_LENGTH);
		return -1;
	}

	rc = nvme_rdma_get_memory_translation(req, rqpair, &ctx);
	if (spdk_unlikely(rc)) {
		return -1;
	}

	nvme_rdma_configure_contig_request(rdma_req, req, &ctx);

	return 0;
}

/*
 * Build SGL describing scattered payload buffer.
 */
static inline int
nvme_rdma_build_sgl_request(struct nvme_rdma_qpair *rqpair,
			    struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;
	struct spdk_nvmf_cmd *cmd = &rqpair->cmds[rdma_req->id];
	struct nvme_rdma_memory_translation_ctx ctx;
	uint32_t remaining_size;
	uint32_t sge_length;
	int rc, max_num_sgl, num_sgl_desc;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	max_num_sgl = req->qpair->ctrlr->max_sges;

	remaining_size = req->payload_size;
	num_sgl_desc = 0;
	do {
		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &ctx.addr, &sge_length);
		if (spdk_unlikely(rc)) {
			return -1;
		}

		sge_length = spdk_min(remaining_size, sge_length);

		if (spdk_unlikely(sge_length > NVME_RDMA_MAX_KEYED_SGL_LENGTH)) {
			NVME_RQPAIR_ERRLOG(rqpair, "SGL length %u exceeds max keyed SGL block size %u\n", sge_length,
					   NVME_RDMA_MAX_KEYED_SGL_LENGTH);
			return -1;
		}
		ctx.length = sge_length;
		rc = nvme_rdma_get_memory_translation(req, rqpair, &ctx);
		if (spdk_unlikely(rc)) {
			return -1;
		}

		cmd->sgl[num_sgl_desc].keyed.key = ctx.rkey;
		cmd->sgl[num_sgl_desc].keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
		cmd->sgl[num_sgl_desc].keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
		cmd->sgl[num_sgl_desc].keyed.length = (uint32_t)ctx.length;
		cmd->sgl[num_sgl_desc].address = (uint64_t)ctx.addr;

		remaining_size -= ctx.length;
		num_sgl_desc++;
	} while (remaining_size > 0 && num_sgl_desc < max_num_sgl);


	/* Should be impossible if we did our sgl checks properly up the stack, but do a sanity check here. */
	if (spdk_unlikely(remaining_size > 0)) {
		return -1;
	}

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;

	/* The RDMA SGL needs one element describing some portion
	 * of the spdk_nvmf_cmd structure. */
	rdma_req->send_wr.num_sge = 1;

	/*
	 * If only one SGL descriptor is required, it can be embedded directly in the command
	 * as a data block descriptor.
	 */
	if (num_sgl_desc == 1) {
		/* The first element of this SGL is pointing at an
		 * spdk_nvmf_cmd object. For this particular command,
		 * we only need the first 64 bytes corresponding to
		 * the NVMe command. */
		rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);

		req->cmd.dptr.sgl1.keyed.type = cmd->sgl[0].keyed.type;
		req->cmd.dptr.sgl1.keyed.subtype = cmd->sgl[0].keyed.subtype;
		req->cmd.dptr.sgl1.keyed.length = cmd->sgl[0].keyed.length;
		req->cmd.dptr.sgl1.keyed.key = cmd->sgl[0].keyed.key;
		req->cmd.dptr.sgl1.address = cmd->sgl[0].address;
	} else {
		/*
		 * Otherwise, The SGL descriptor embedded in the command must point to the list of
		 * SGL descriptors used to describe the operation. In that case it is a last segment descriptor.
		 */
		uint32_t descriptors_size = sizeof(struct spdk_nvme_sgl_descriptor) * num_sgl_desc;

		if (spdk_unlikely(descriptors_size > rqpair->qpair.ctrlr->ioccsz_bytes)) {
			NVME_RQPAIR_ERRLOG(rqpair, "Size of SGL descriptors (%u) exceeds ICD (%u)\n", descriptors_size,
					   rqpair->qpair.ctrlr->ioccsz_bytes);
			return -1;
		}
		rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd) + descriptors_size;

		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
		req->cmd.dptr.sgl1.unkeyed.length = descriptors_size;
		req->cmd.dptr.sgl1.address = (uint64_t)0;
	}

	return 0;
}

/*
 * Build inline SGL describing sgl payload buffer.
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
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &ctx.addr, &length);
	if (spdk_unlikely(rc)) {
		return -1;
	}

	if (length < req->payload_size) {
		NVME_RQPAIR_DEBUGLOG(rqpair, "Inline SGL request split so sending separately.\n");
		return nvme_rdma_build_sgl_request(rqpair, rdma_req);
	}

	if (length > req->payload_size) {
		length = req->payload_size;
	}

	ctx.length = length;
	rc = nvme_rdma_get_memory_translation(req, rqpair, &ctx);
	if (spdk_unlikely(rc)) {
		return -1;
	}

	rdma_req->send_sgl[1].addr = (uint64_t)ctx.addr;
	rdma_req->send_sgl[1].length = (uint32_t)ctx.length;
	rdma_req->send_sgl[1].lkey = ctx.lkey;

	rdma_req->send_wr.num_sge = 2;

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
	req->cmd.dptr.sgl1.unkeyed.length = (uint32_t)ctx.length;
	/* Inline only supported for icdoff == 0 currently.  This function will
	 * not get called for controllers with other values. */
	req->cmd.dptr.sgl1.address = (uint64_t)0;

	return 0;
}

static inline int
nvme_rdma_accel_append_copy(struct spdk_nvme_poll_group *pg, void **seq,
			    struct spdk_memory_domain *rdma_domain, struct spdk_nvme_rdma_req *rdma_req,
			    struct iovec *iovs, uint32_t iovcnt,
			    struct spdk_memory_domain *src_domain, void *src_domain_ctx)
{
	return pg->accel_fn_table.append_copy(pg->ctx, seq, iovs, iovcnt, rdma_domain, rdma_req, iovs,
					      iovcnt, src_domain, src_domain_ctx, NULL, NULL);
}

static inline void
nvme_rdma_accel_reverse(struct spdk_nvme_poll_group *pg, void *seq)
{
	pg->accel_fn_table.reverse_sequence(seq);
}

static inline void
nvme_rdma_accel_finish(struct spdk_nvme_poll_group *pg, void *seq,
		       spdk_nvme_accel_completion_cb cb_fn, void *cb_arg)
{
	pg->accel_fn_table.finish_sequence(seq, cb_fn, cb_arg);
}

static inline void
nvme_rdma_accel_completion_cb(void *cb_arg, int status)
{
	struct spdk_nvme_rdma_req *rdma_req = cb_arg;
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(rdma_req->req->qpair);
	struct spdk_nvme_cpl cpl;
	enum spdk_nvme_generic_command_status_code sc;
	uint16_t dnr = 0;

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

static inline int
nvme_rdma_apply_accel_sequence(struct nvme_rdma_qpair *rqpair, struct nvme_request *req,
			       struct spdk_nvme_rdma_req *rdma_req)
{
	struct spdk_nvme_poll_group *pg = rqpair->qpair.poll_group->group;
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

static inline int
nvme_rdma_req_init(struct nvme_rdma_qpair *rqpair, struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;
	struct spdk_nvme_ctrlr *ctrlr = rqpair->qpair.ctrlr;
	enum nvme_payload_type payload_type;
	bool icd_supported;
	int rc = -1;

	payload_type = nvme_payload_type(&req->payload);
	/*
	 * Check if icdoff is non zero, to avoid interop conflicts with
	 * targets with non-zero icdoff.  Both SPDK and the Linux kernel
	 * targets use icdoff = 0.  For targets with non-zero icdoff, we
	 * will currently just not use inline data for now.
	 */
	icd_supported = spdk_nvme_opc_get_data_transfer(req->cmd.opc) == SPDK_NVME_DATA_HOST_TO_CONTROLLER
			&& req->payload_size <= ctrlr->ioccsz_bytes && ctrlr->icdoff == 0;

	if (spdk_unlikely(req->payload_size == 0)) {
		rc = nvme_rdma_build_null_request(rdma_req);
	} else if (payload_type == NVME_PAYLOAD_TYPE_CONTIG) {
		if (icd_supported) {
			rc = nvme_rdma_build_contig_inline_request(rqpair, rdma_req);
		} else {
			rc = nvme_rdma_build_contig_request(rqpair, rdma_req);
		}
	} else if (payload_type == NVME_PAYLOAD_TYPE_SGL) {
		if (icd_supported) {
			rc = nvme_rdma_build_sgl_inline_request(rqpair, rdma_req);
		} else {
			rc = nvme_rdma_build_sgl_request(rqpair, rdma_req);
		}
	}

	if (spdk_unlikely(rc)) {
		return rc;
	}

	memcpy(&rqpair->cmds[rdma_req->id], &req->cmd, sizeof(req->cmd));
	return 0;
}

static struct spdk_nvme_qpair *
nvme_rdma_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr,
			     uint16_t qid, uint32_t qsize,
			     enum spdk_nvme_qprio qprio,
			     uint32_t num_requests,
			     bool delay_cmd_submit,
			     bool async)
{
	struct nvme_rdma_qpair *rqpair;
	struct spdk_nvme_qpair *qpair;
	int rc;

	if (qsize < SPDK_NVME_QUEUE_MIN_ENTRIES) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to create qpair with size %u. Minimum queue size is %d.\n",
				  qsize, SPDK_NVME_QUEUE_MIN_ENTRIES);
		return NULL;
	}

	rqpair = spdk_zmalloc(sizeof(struct nvme_rdma_qpair), 0, NULL, SPDK_ENV_NUMA_ID_ANY,
			      SPDK_MALLOC_DMA);
	if (!rqpair) {
		NVME_CTRLR_ERRLOG(ctrlr, "failed to get create rqpair\n");
		return NULL;
	}

	/* Set num_entries one less than queue size. According to NVMe
	 * and NVMe-oF specs we can not submit queue size requests,
	 * one slot shall always remain empty.
	 */
	rqpair->num_entries = qsize - 1;
	rqpair->delay_cmd_submit = delay_cmd_submit;
	rqpair->state = NVME_RDMA_QPAIR_STATE_INVALID;
	rqpair->append_copy = g_spdk_nvme_transport_opts.rdma_umr_per_io &&
			      spdk_rdma_provider_accel_sequence_supported() && qid != 0;
	qpair = &rqpair->qpair;
	rc = nvme_qpair_init(qpair, qid, ctrlr, qprio, num_requests, async);
	if (rc != 0) {
		spdk_free(rqpair);
		return NULL;
	}

	NVME_RQPAIR_DEBUGLOG(rqpair, "append_copy %s\n", rqpair->append_copy ? "enabled" : "disabled");
	return qpair;
}

static void
nvme_rdma_qpair_destroy(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;
	struct nvme_rdma_ctrlr *rctrlr;
	struct nvme_rdma_cm_event_entry *entry, *tmp;

	spdk_rdma_utils_free_mem_map(&rqpair->mr_map);

	if (rqpair->evt) {
		rdma_ack_cm_event(rqpair->evt);
		rqpair->evt = NULL;
	}

	/*
	 * This works because we have the controller lock both in
	 * this function and in the function where we add new events.
	 */
	if (qpair->ctrlr != NULL) {
		rctrlr = nvme_rdma_ctrlr(qpair->ctrlr);
		STAILQ_FOREACH_SAFE(entry, &rctrlr->pending_cm_events, link, tmp) {
			if (entry->evt->id->context == rqpair) {
				STAILQ_REMOVE(&rctrlr->pending_cm_events, entry, nvme_rdma_cm_event_entry, link);
				rdma_ack_cm_event(entry->evt);
				STAILQ_INSERT_HEAD(&rctrlr->free_cm_events, entry, link);
			}
		}
	}

	if (rqpair->cm_id) {
		if (rqpair->rdma_qp) {
			spdk_rdma_utils_put_pd(rqpair->rdma_qp->qp->pd);
			spdk_rdma_provider_qp_destroy(rqpair->rdma_qp);
			rqpair->rdma_qp = NULL;
		}
	}

	if (rqpair->poller) {
		nvme_rdma_qpair_release_poller(rqpair);

		if (rqpair->srq) {
			rqpair->srq = NULL;
			rqpair->rsps = NULL;
		}
	} else if (rqpair->cq) {
		ibv_destroy_cq(rqpair->cq);
		rqpair->cq = NULL;
	}

	nvme_rdma_free_reqs(rqpair);
	nvme_rdma_free_rsps(rqpair->rsps);
	rqpair->rsps = NULL;

	/* destroy cm_id last so cma device will not be freed before we destroy the cq. */
	if (rqpair->cm_id) {
		rdma_destroy_id(rqpair->cm_id);
		rqpair->cm_id = NULL;
	}
}

static void nvme_rdma_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr);

static void
nvme_rdma_qpair_flush_send_wrs(struct nvme_rdma_qpair *rqpair)
{
	struct ibv_send_wr *bad_wr = NULL;
	int rc;

	rc = spdk_rdma_provider_qp_flush_send_wrs(rqpair->rdma_qp, &bad_wr);
	if (rc) {
		nvme_rdma_reset_failed_sends(rqpair, bad_wr);
	}
}

static inline void
nvme_rdma_finish_outstanding_accel_transfers(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_req *req, *tmp;

	TAILQ_FOREACH_SAFE(req, &rqpair->outstanding_reqs, link, tmp) {
		if (req->in_progress_accel && req->transfer_cpl_cb) {
			nvme_rdma_finish_data_transfer(req, -ENXIO);
		}
	}
}

static int
nvme_rdma_qpair_disconnected(struct nvme_rdma_qpair *rqpair, int ret)
{
	if (rqpair->rdma_qp != NULL) {
		nvme_rdma_qpair_flush_send_wrs(rqpair);
	}

	if (rqpair->num_active_accel_reqs != 0) {
		SPDK_DEBUGLOG(nvme, "qp %p has %u accel requests\n", rqpair, rqpair->num_active_accel_reqs);
		nvme_rdma_finish_outstanding_accel_transfers(rqpair);
		goto lingering;
	}

	if (ret) {
		SPDK_DEBUGLOG(nvme, "Target did not respond to qpair disconnect.\n");
		goto quiet;
	}

	if (rqpair->poller == NULL) {
		/* If poller is not used, cq is not shared.
		 * So complete disconnecting qpair immediately.
		 */
		goto quiet;
	}

	if (rqpair->rsps == NULL) {
		goto quiet;
	}

	if (rqpair->need_destroy ||
	    (rqpair->current_num_sends != 0 ||
	     (!rqpair->srq && rqpair->rsps->current_num_recvs != 0)) ||
	    ((rqpair->qpair.ctrlr->flags & SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED) &&
	     (!TAILQ_EMPTY(&rqpair->outstanding_reqs)))) {
lingering:
		rqpair->state = NVME_RDMA_QPAIR_STATE_LINGERING;
		rqpair->evt_timeout_ticks = (NVME_RDMA_DISCONNECTED_QPAIR_TIMEOUT_US * spdk_get_ticks_hz()) /
					    SPDK_SEC_TO_USEC + spdk_get_ticks();

		return -EAGAIN;
	}

quiet:
	rqpair->state = NVME_RDMA_QPAIR_STATE_EXITED;

	nvme_rdma_qpair_abort_reqs(&rqpair->qpair, rqpair->qpair.abort_dnr);
	assert(TAILQ_EMPTY(&rqpair->outstanding_reqs));
	nvme_rdma_qpair_destroy(rqpair);
	nvme_transport_ctrlr_disconnect_qpair_done(&rqpair->qpair);

	return 0;
}

static int
nvme_rdma_qpair_wait_until_quiet(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;

	if (rqpair->num_active_accel_reqs != 0) {
		nvme_rdma_finish_outstanding_accel_transfers(rqpair);
		return -EAGAIN;
	}

	if (spdk_get_ticks() < rqpair->evt_timeout_ticks &&
	    (rqpair->current_num_sends != 0 ||
	     (!rqpair->srq && rqpair->rsps->current_num_recvs != 0))) {
		return -EAGAIN;
	}

	rqpair->state = NVME_RDMA_QPAIR_STATE_EXITED;
	nvme_rdma_qpair_abort_reqs(qpair, qpair->abort_dnr);
	assert(TAILQ_EMPTY(&rqpair->outstanding_reqs));
	if (!nvme_qpair_is_admin_queue(qpair)) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	}
	nvme_rdma_qpair_destroy(rqpair);
	if (!nvme_qpair_is_admin_queue(qpair)) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	nvme_transport_ctrlr_disconnect_qpair_done(&rqpair->qpair);

	return 0;
}

static void
_nvme_rdma_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				  nvme_rdma_cm_event_cb disconnected_qpair_cb)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	int rc;

	assert(disconnected_qpair_cb != NULL);

	rqpair->state = NVME_RDMA_QPAIR_STATE_EXITING;

	if (rqpair->cm_id) {
		if (rqpair->rdma_qp && rqpair->connected) {
			rc = spdk_rdma_provider_qp_disconnect(rqpair->rdma_qp);
			if ((qpair->ctrlr != NULL) && (rc == 0)) {
				rc = nvme_rdma_process_event_start(rqpair, RDMA_CM_EVENT_DISCONNECTED,
								   disconnected_qpair_cb);
				if (rc == 0) {
					return;
				}
			}
		}
	}

	disconnected_qpair_cb(rqpair, 0);
}

static int
nvme_rdma_ctrlr_disconnect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	int rc;

	switch (rqpair->state) {
	case NVME_RDMA_QPAIR_STATE_EXITING:
		if (!nvme_qpair_is_admin_queue(qpair)) {
			nvme_ctrlr_lock(ctrlr);
		}

		rc = nvme_rdma_process_event_poll(rqpair);

		if (!nvme_qpair_is_admin_queue(qpair)) {
			nvme_ctrlr_unlock(ctrlr);
		}
		break;

	case NVME_RDMA_QPAIR_STATE_LINGERING:
		rc = nvme_rdma_qpair_wait_until_quiet(rqpair);
		break;
	case NVME_RDMA_QPAIR_STATE_EXITED:
		rc = 0;
		break;

	default:
		assert(false);
		rc = -EAGAIN;
		break;
	}

	return rc;
}

static void
nvme_rdma_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc;

	_nvme_rdma_ctrlr_disconnect_qpair(ctrlr, qpair, nvme_rdma_qpair_disconnected);

	/* If the async mode is disabled, poll the qpair until it is actually disconnected.
	 * It is ensured that poll_group_process_completions() calls disconnected_qpair_cb
	 * for any disconnected qpair. Hence, we do not have to check if the qpair is in
	 * a poll group or not.
	 */
	if (qpair->async) {
		return;
	}

	while (1) {
		rc = nvme_rdma_ctrlr_disconnect_qpair_poll(ctrlr, qpair);
		if (rc != -EAGAIN) {
			break;
		}
	}
}

static int
nvme_rdma_stale_conn_disconnected(struct nvme_rdma_qpair *rqpair, int ret)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;

	if (ret) {
		SPDK_DEBUGLOG(nvme, "Target did not respond to qpair disconnect.\n");
	}

	nvme_rdma_qpair_destroy(rqpair);

	qpair->last_transport_failure_reason = qpair->transport_failure_reason;
	qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_NONE;

	rqpair->state = NVME_RDMA_QPAIR_STATE_STALE_CONN;
	rqpair->evt_timeout_ticks = (NVME_RDMA_STALE_CONN_RETRY_DELAY_US * spdk_get_ticks_hz()) /
				    SPDK_SEC_TO_USEC + spdk_get_ticks();

	return 0;
}

static int
nvme_rdma_stale_conn_retry(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;

	if (rqpair->stale_conn_retry_count >= NVME_RDMA_STALE_CONN_RETRY_MAX) {
		NVME_RQPAIR_ERRLOG(rqpair, "Retry failed %d times, give up stale connection to qpair.\n",
				   NVME_RDMA_STALE_CONN_RETRY_MAX);
		return -ESTALE;
	}

	rqpair->stale_conn_retry_count++;

	NVME_RQPAIR_NOTICELOG(rqpair, "%d times, retry stale connection.\n",
			      rqpair->stale_conn_retry_count);
	_nvme_rdma_ctrlr_disconnect_qpair(qpair->ctrlr, qpair, nvme_rdma_stale_conn_disconnected);

	return 0;
}

static int
nvme_rdma_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair;

	assert(qpair != NULL);
	rqpair = nvme_rdma_qpair(qpair);

	if (rqpair->state != NVME_RDMA_QPAIR_STATE_EXITED) {
		int rc __attribute__((unused));

		/* qpair was removed from the poll group while the disconnect is not finished.
		 * Destroy rdma resources forcefully. */
		rc = nvme_rdma_qpair_disconnected(rqpair, 0);
		assert(rc == 0);
	}

	nvme_rdma_qpair_abort_reqs(qpair, qpair->abort_dnr);
	assert(TAILQ_EMPTY(&rqpair->outstanding_reqs));
	nvme_qpair_deinit(qpair);

	spdk_free(rqpair);

	return 0;
}

static struct spdk_nvme_qpair *
nvme_rdma_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_rdma_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					    opts->io_queue_requests,
					    opts->delay_cmd_submit,
					    opts->async_mode);
}

static int
nvme_rdma_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	/* do nothing here */
	return 0;
}

static int nvme_rdma_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr);

/* We have to use the typedef in the function declaration to appease astyle. */
typedef struct spdk_nvme_ctrlr spdk_nvme_ctrlr_t;

static spdk_nvme_ctrlr_t *
nvme_rdma_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
			  const struct spdk_nvme_ctrlr_opts *opts,
			  void *devhandle)
{
	struct nvme_rdma_ctrlr *rctrlr;
	struct ibv_context **contexts;
	struct ibv_device_attr dev_attr;
	int i, rc;

	rctrlr = spdk_zmalloc(sizeof(struct nvme_rdma_ctrlr), 0, NULL, SPDK_ENV_NUMA_ID_ANY,
			      SPDK_MALLOC_DMA);
	if (rctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	rctrlr->ctrlr.opts = *opts;
	rctrlr->ctrlr.trid = *trid;

	if (opts->transport_retry_count > NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT) {
		NVME_CTRLR_NOTICELOG(&rctrlr->ctrlr, "transport_retry_count exceeds max value %d, use max value\n",
				     NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT);
		rctrlr->ctrlr.opts.transport_retry_count = NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT;
	}

	if (opts->transport_ack_timeout > NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT) {
		NVME_CTRLR_NOTICELOG(&rctrlr->ctrlr, "transport_ack_timeout exceeds max value %d, use max value\n",
				     NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT);
		rctrlr->ctrlr.opts.transport_ack_timeout = NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT;
	}

	contexts = rdma_get_devices(NULL);
	if (contexts == NULL) {
		NVME_CTRLR_ERRLOG(&rctrlr->ctrlr, "rdma_get_devices() failed: %s (%d)\n", spdk_strerror(errno),
				  errno);
		spdk_free(rctrlr);
		return NULL;
	}

	i = 0;
	rctrlr->max_sge = NVME_RDMA_MAX_SGL_DESCRIPTORS;

	while (contexts[i] != NULL) {
		rc = ibv_query_device(contexts[i], &dev_attr);
		if (rc < 0) {
			NVME_CTRLR_ERRLOG(&rctrlr->ctrlr, "Failed to query RDMA device attributes.\n");
			rdma_free_devices(contexts);
			spdk_free(rctrlr);
			return NULL;
		}
		rctrlr->max_sge = spdk_min(rctrlr->max_sge, (uint16_t)dev_attr.max_sge);
		i++;
	}

	rdma_free_devices(contexts);

	rc = nvme_ctrlr_construct(&rctrlr->ctrlr);
	if (rc != 0) {
		spdk_free(rctrlr);
		return NULL;
	}

	STAILQ_INIT(&rctrlr->pending_cm_events);
	STAILQ_INIT(&rctrlr->free_cm_events);
	rctrlr->cm_events = spdk_zmalloc(NVME_RDMA_NUM_CM_EVENTS * sizeof(*rctrlr->cm_events), 0, NULL,
					 SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (rctrlr->cm_events == NULL) {
		NVME_CTRLR_ERRLOG(&rctrlr->ctrlr, "unable to allocate buffers to hold CM events.\n");
		goto destruct_ctrlr;
	}

	for (i = 0; i < NVME_RDMA_NUM_CM_EVENTS; i++) {
		STAILQ_INSERT_TAIL(&rctrlr->free_cm_events, &rctrlr->cm_events[i], link);
	}

	rctrlr->cm_channel = rdma_create_event_channel();
	if (rctrlr->cm_channel == NULL) {
		NVME_CTRLR_ERRLOG(&rctrlr->ctrlr, "rdma_create_event_channel() failed\n");
		goto destruct_ctrlr;
	}

	if (spdk_fd_set_nonblock(rctrlr->cm_channel->fd) < 0) {
		goto destruct_ctrlr;
	}

	rctrlr->ctrlr.adminq = nvme_rdma_ctrlr_create_qpair(&rctrlr->ctrlr, 0,
			       rctrlr->ctrlr.opts.admin_queue_size, 0,
			       rctrlr->ctrlr.opts.admin_queue_size, false, true);
	if (!rctrlr->ctrlr.adminq) {
		NVME_CTRLR_ERRLOG(&rctrlr->ctrlr, "failed to create admin qpair\n");
		goto destruct_ctrlr;
	}
	if (spdk_rdma_provider_accel_sequence_supported()) {
		rctrlr->ctrlr.flags |= SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED;
	}

	if (nvme_ctrlr_add_process(&rctrlr->ctrlr, 0) != 0) {
		NVME_CTRLR_ERRLOG(&rctrlr->ctrlr, "nvme_ctrlr_add_process() failed\n");
		goto destruct_ctrlr;
	}

	NVME_CTRLR_DEBUGLOG(&rctrlr->ctrlr, "successfully initialized the nvmf ctrlr\n");
	return &rctrlr->ctrlr;

destruct_ctrlr:
	nvme_ctrlr_destruct(&rctrlr->ctrlr);
	return NULL;
}

static int
nvme_rdma_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_rdma_ctrlr *rctrlr = nvme_rdma_ctrlr(ctrlr);
	struct nvme_rdma_cm_event_entry *entry;

	if (ctrlr->adminq) {
		nvme_rdma_ctrlr_delete_io_qpair(ctrlr, ctrlr->adminq);
	}

	STAILQ_FOREACH(entry, &rctrlr->pending_cm_events, link) {
		rdma_ack_cm_event(entry->evt);
	}

	STAILQ_INIT(&rctrlr->free_cm_events);
	STAILQ_INIT(&rctrlr->pending_cm_events);
	spdk_free(rctrlr->cm_events);

	if (rctrlr->cm_channel) {
		rdma_destroy_event_channel(rctrlr->cm_channel);
		rctrlr->cm_channel = NULL;
	}

	nvme_ctrlr_destruct_finish(ctrlr);

	spdk_free(rctrlr);

	return 0;
}

static inline int
_nvme_rdma_qpair_submit_request(struct nvme_rdma_qpair *rqpair,
				struct spdk_nvme_rdma_req *rdma_req)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;
	struct ibv_send_wr *wr;
	struct nvme_rdma_poll_group *group;

	if (TAILQ_ENTRY_NOT_ENQUEUED(rqpair, link_active) && qpair->poll_group) {
		group = nvme_rdma_poll_group(qpair->poll_group);
		TAILQ_INSERT_TAIL(&group->active_qpairs, rqpair, link_active);
	}
	assert(rqpair->current_num_sends < rqpair->num_entries);
	rqpair->current_num_sends++;

	wr = &rdma_req->send_wr;
	wr->next = NULL;
	nvme_rdma_trace_ibv_sge(wr->sg_list);

	spdk_rdma_provider_qp_queue_send_wrs(rqpair->rdma_qp, wr);

	if (!rqpair->delay_cmd_submit) {
		return nvme_rdma_qpair_submit_sends(rqpair);
	}

	return 0;
}

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

	rdma_req = nvme_rdma_req_get(rqpair);
	if (spdk_unlikely(!rdma_req)) {
		if (rqpair->poller) {
			rqpair->poller->stats.queued_requests++;
		}
		/* Inform the upper layer to try again later. */
		return -EAGAIN;
	}

	assert(rdma_req->req == NULL);
	rdma_req->req = req;
	req->cmd.cid = rdma_req->id;
	if (req->accel_sequence || rqpair->append_copy) {
		assert(spdk_rdma_provider_accel_sequence_supported());
		assert(rqpair->qpair.poll_group->group);
		assert(rqpair->qpair.poll_group->group->accel_fn_table.append_copy);
		assert(rqpair->qpair.poll_group->group->accel_fn_table.reverse_sequence);
		assert(rqpair->qpair.poll_group->group->accel_fn_table.finish_sequence);

		rc = nvme_rdma_apply_accel_sequence(rqpair, req, rdma_req);
		if (spdk_unlikely(rc)) {
			NVME_RQPAIR_ERRLOG(rqpair, "failed to apply accel seq, rqpair %p, req %p, rc %d\n", rqpair,
					   rdma_req,
					   rc);
			nvme_rdma_req_put(rqpair, rdma_req);
			return rc;
		}
		/* Capsule will be sent in data_transfer callback */
		return 0;
	}

	rc = nvme_rdma_req_init(rqpair, rdma_req);
	if (spdk_unlikely(rc)) {
		NVME_RQPAIR_ERRLOG(rqpair, "nvme_rdma_req_init() failed\n");
		nvme_rdma_req_put(rqpair, rdma_req);
		return -1;
	}

	TAILQ_INSERT_TAIL(&rqpair->outstanding_reqs, rdma_req, link);
	rqpair->num_outstanding_reqs++;

	return _nvme_rdma_qpair_submit_request(rqpair, rdma_req);
}

static int
nvme_rdma_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	/* Currently, doing nothing here */
	return 0;
}

static void
nvme_rdma_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct spdk_nvme_rdma_req *rdma_req, *tmp;
	struct spdk_nvme_cpl cpl;
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);

	cpl.sqid = qpair->id;
	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = dnr;

	/*
	 * We cannot abort requests at the RDMA layer without
	 * unregistering them. If we do, we can still get error
	 * free completions on the shared completion queue.
	 */
	if (nvme_qpair_get_state(qpair) > NVME_QPAIR_DISCONNECTING &&
	    nvme_qpair_get_state(qpair) != NVME_QPAIR_DESTROYING) {
		nvme_ctrlr_disconnect_qpair(qpair);
	}

	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->outstanding_reqs, link, tmp) {
		if (rdma_req->in_progress_accel) {
			/* We should wait for accel completion */
			continue;
		}
		nvme_rdma_req_complete(rdma_req, &cpl, true);
	}
}

static void
nvme_rdma_qpair_check_timeout(struct spdk_nvme_qpair *qpair)
{
	uint64_t t02;
	struct spdk_nvme_rdma_req *rdma_req, *tmp;
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvme_ctrlr_process *active_proc;

	/* Don't check timeouts during controller initialization. */
	if (ctrlr->state != NVME_CTRLR_STATE_READY) {
		return;
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
		active_proc = nvme_ctrlr_get_current_process(ctrlr);
	} else {
		active_proc = qpair->active_proc;
	}

	/* Only check timeouts if the current process has a timeout callback. */
	if (active_proc == NULL || active_proc->timeout_cb_fn == NULL) {
		return;
	}

	t02 = spdk_get_ticks();
	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->outstanding_reqs, link, tmp) {
		assert(rdma_req->req != NULL);

		if (nvme_request_check_timeout(rdma_req->req, rdma_req->id, active_proc, t02)) {
			/*
			 * The requests are in order, so as soon as one has not timed out,
			 * stop iterating.
			 */
			break;
		}
	}
}

static inline void
nvme_rdma_request_ready(struct nvme_rdma_qpair *rqpair, struct spdk_nvme_rdma_req *rdma_req)
{
	struct ibv_recv_wr *recv_wr = rdma_req->rdma_rsp->recv_wr;

	if (rdma_req->transfer_cpl_cb) {
		int rc = 0;

		if (spdk_unlikely(spdk_nvme_cpl_is_error(&rdma_req->cpl))) {
			NVME_RQPAIR_WARNLOG(rqpair, "req %p, error cpl sct %d, sc %d\n", rdma_req, rdma_req->cpl.status.sct,
					    rdma_req->cpl.status.sc);
			rc = -EIO;
		}
		nvme_rdma_finish_data_transfer(rdma_req, rc);
	} else {
		nvme_rdma_req_complete(rdma_req, &rdma_req->cpl, true);
	}

	if (spdk_unlikely(rqpair->state >= NVME_RDMA_QPAIR_STATE_EXITING && !rqpair->srq)) {
		/* Skip posting back recv wr if we are in a disconnection process. We may never get
		 * a WC and we may end up stuck in LINGERING state until the timeout. */
		return;
	}

	assert(rqpair->rsps->current_num_recvs < rqpair->rsps->num_entries);
	rqpair->rsps->current_num_recvs++;

	recv_wr->next = NULL;
	nvme_rdma_trace_ibv_sge(recv_wr->sg_list);

	if (!rqpair->srq) {
		spdk_rdma_provider_qp_queue_recv_wrs(rqpair->rdma_qp, recv_wr);
	} else {
		spdk_rdma_provider_srq_queue_recv_wrs(rqpair->srq, recv_wr);
	}
}

#define MAX_COMPLETIONS_PER_POLL 128

static void
nvme_rdma_fail_qpair(struct spdk_nvme_qpair *qpair, int failure_reason)
{
	if (failure_reason == IBV_WC_RETRY_EXC_ERR) {
		qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_REMOTE;
	} else if (qpair->transport_failure_reason == SPDK_NVME_QPAIR_FAILURE_NONE) {
		qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;
	}

	nvme_ctrlr_disconnect_qpair(qpair);
}

static struct nvme_rdma_qpair *
get_rdma_qpair_from_wc(struct nvme_rdma_poll_group *group, struct ibv_wc *wc)
{
	struct spdk_nvme_qpair *qpair;
	struct nvme_rdma_qpair *rqpair;

	STAILQ_FOREACH(qpair, &group->group.connected_qpairs, poll_group_stailq) {
		rqpair = nvme_rdma_qpair(qpair);
		if (NVME_RDMA_POLL_GROUP_CHECK_QPN(rqpair, wc->qp_num)) {
			return rqpair;
		}
	}

	STAILQ_FOREACH(qpair, &group->group.disconnected_qpairs, poll_group_stailq) {
		rqpair = nvme_rdma_qpair(qpair);
		if (NVME_RDMA_POLL_GROUP_CHECK_QPN(rqpair, wc->qp_num)) {
			return rqpair;
		}
	}

	return NULL;
}

static inline void
nvme_rdma_log_wc_status(struct nvme_rdma_qpair *rqpair, struct ibv_wc *wc)
{
	struct nvme_rdma_wr *rdma_wr = (struct nvme_rdma_wr *)wc->wr_id;

	if (wc->status == IBV_WC_WR_FLUSH_ERR) {
		/* If qpair is in ERR state, we will receive completions for all posted and not completed
		 * Work Requests with IBV_WC_WR_FLUSH_ERR status. Don't log an error in that case */
		NVME_RQPAIR_DEBUGLOG(rqpair, "WC error, qp state %d, request 0x%lu type %d, status: (%d): %s\n",
				     rqpair->qpair.state, wc->wr_id, rdma_wr->type, wc->status, ibv_wc_status_str(wc->status));
	} else {
		NVME_RQPAIR_ERRLOG(rqpair, "WC error, qp state %d, request 0x%lu type %d, status: (%d): %s\n",
				   rqpair->qpair.state, wc->wr_id, rdma_wr->type, wc->status, ibv_wc_status_str(wc->status));
	}
}

static inline int
nvme_rdma_process_recv_completion(struct nvme_rdma_poller *poller, struct ibv_wc *wc,
				  struct nvme_rdma_wr *rdma_wr)
{
	struct nvme_rdma_qpair		*rqpair;
	struct spdk_nvme_rdma_req	*rdma_req;
	struct spdk_nvme_rdma_rsp	*rdma_rsp;

	rdma_rsp = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvme_rdma_rsp, rdma_wr);

	if (poller && poller->srq) {
		rqpair = get_rdma_qpair_from_wc(poller->group, wc);
		if (spdk_unlikely(!rqpair)) {
			/* Since we do not handle the LAST_WQE_REACHED event, we do not know when
			 * a Receive Queue in a QP, that is associated with an SRQ, is flushed.
			 * We may get a WC for a already destroyed QP.
			 *
			 * However, for the SRQ, this is not any error. Hence, just re-post the
			 * receive request to the SRQ to reuse for other QPs, and return 0.
			 */
			rdma_rsp->recv_wr->next = NULL;
			spdk_rdma_provider_srq_queue_recv_wrs(poller->srq, rdma_rsp->recv_wr);
			return 0;
		}
	} else {
		rqpair = rdma_rsp->rqpair;
		if (spdk_unlikely(!rqpair)) {
			/* TODO: Fix forceful QP destroy when it is not async mode.
			 * CQ itself did not cause any error. Hence, return 0 for now.
			 */
			SPDK_WARNLOG("QP might be already destroyed.\n");
			return 0;
		}
	}


	assert(rqpair->rsps->current_num_recvs > 0);
	rqpair->rsps->current_num_recvs--;

	if (spdk_unlikely(wc->status)) {
		nvme_rdma_log_wc_status(rqpair, wc);
		goto err_wc;
	}

	NVME_RQPAIR_DEBUGLOG(rqpair, "CQ recv completion\n");

	if (spdk_unlikely(wc->byte_len < sizeof(struct spdk_nvme_cpl))) {
		NVME_RQPAIR_ERRLOG(rqpair, "recv length %u less than expected response size\n", wc->byte_len);
		goto err_wc;
	}
	rdma_req = &rqpair->rdma_reqs[rdma_rsp->cpl.cid];
	rdma_req->completion_flags |= NVME_RDMA_RECV_COMPLETED;
	rdma_req->rdma_rsp = rdma_rsp;
	rdma_req->cpl = rdma_rsp->cpl;

	if ((rdma_req->completion_flags & NVME_RDMA_SEND_COMPLETED) == 0) {
		return 0;
	}

	rqpair->num_completions++;

	nvme_rdma_request_ready(rqpair, rdma_req);

	if (!rqpair->delay_cmd_submit) {
		if (spdk_unlikely(nvme_rdma_qpair_submit_recvs(rqpair))) {
			NVME_RQPAIR_ERRLOG(rqpair, "Unable to re-post rx descriptor\n");
			nvme_rdma_fail_qpair(&rqpair->qpair, 0);
			return -ENXIO;
		}
	}

	return 1;

err_wc:
	nvme_rdma_fail_qpair(&rqpair->qpair, 0);
	if (poller && poller->srq) {
		rdma_rsp->recv_wr->next = NULL;
		spdk_rdma_provider_srq_queue_recv_wrs(poller->srq, rdma_rsp->recv_wr);
	}
	rdma_req = &rqpair->rdma_reqs[rdma_rsp->cpl.cid];
	if (rdma_req->transfer_cpl_cb) {
		nvme_rdma_finish_data_transfer(rdma_req, -ENXIO);
	}
	return -ENXIO;
}

static inline int
nvme_rdma_process_send_completion(struct nvme_rdma_poller *poller,
				  struct nvme_rdma_qpair *rdma_qpair,
				  struct ibv_wc *wc, struct nvme_rdma_wr *rdma_wr)
{
	struct nvme_rdma_qpair		*rqpair;
	struct spdk_nvme_rdma_req	*rdma_req;

	rdma_req = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvme_rdma_req, rdma_wr);
	rqpair = rdma_req->req ? nvme_rdma_qpair(rdma_req->req->qpair) : NULL;
	if (spdk_unlikely(!rqpair)) {
		rqpair = rdma_qpair != NULL ? rdma_qpair : get_rdma_qpair_from_wc(poller->group, wc);
	}

	/* If we are flushing I/O */
	if (spdk_unlikely(wc->status)) {
		if (!rqpair) {
			/* When poll_group is used, several qpairs share the same CQ and it is possible to
			 * receive a completion with error (e.g. IBV_WC_WR_FLUSH_ERR) for already disconnected qpair
			 * That happens due to qpair is destroyed while there are submitted but not completed send/receive
			 * Work Requests */
			assert(poller);
			return 0;
		}
		assert(rqpair->current_num_sends > 0);
		rqpair->current_num_sends--;
		nvme_rdma_log_wc_status(rqpair, wc);
		nvme_rdma_fail_qpair(&rqpair->qpair, 0);
		if (rdma_req->rdma_rsp && poller && poller->srq) {
			rdma_req->rdma_rsp->recv_wr->next = NULL;
			spdk_rdma_provider_srq_queue_recv_wrs(poller->srq, rdma_req->rdma_rsp->recv_wr);
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
		SPDK_ERRLOG("Received malformed completion: request 0x%"PRIx64" type %d\n", wc->wr_id,
			    rdma_wr->type);
		if (!rqpair || !rqpair->need_destroy) {
			assert(0);
		}
		return -ENXIO;
	}

	rdma_req->completion_flags |= NVME_RDMA_SEND_COMPLETED;
	assert(rqpair->current_num_sends > 0);
	rqpair->current_num_sends--;

	if ((rdma_req->completion_flags & NVME_RDMA_RECV_COMPLETED) == 0) {
		return 0;
	}

	rqpair->num_completions++;

	nvme_rdma_request_ready(rqpair, rdma_req);

	if (!rqpair->delay_cmd_submit) {
		if (spdk_unlikely(nvme_rdma_qpair_submit_recvs(rqpair))) {
			NVME_RQPAIR_ERRLOG(rqpair, "Unable to re-post rx descriptor\n");
			nvme_rdma_fail_qpair(&rqpair->qpair, 0);
			return -ENXIO;
		}
	}

	return 1;
}

static inline int
nvme_rdma_cq_process_completions(struct ibv_cq *cq, uint32_t batch_size,
				 struct nvme_rdma_poller *poller,
				 struct nvme_rdma_qpair *rdma_qpair,
				 uint64_t *rdma_completions)
{
	struct ibv_wc			wc[MAX_COMPLETIONS_PER_POLL];
	struct nvme_rdma_wr		*rdma_wr;
	uint32_t			reaped = 0;
	int				completion_rc = 0;
	int				rc, _rc, i;

	rc = ibv_poll_cq(cq, batch_size, wc);
	if (spdk_unlikely(rc < 0)) {
		NVME_RQPAIR_ERRLOG(rdma_qpair, "Error polling CQ! (%d): %s\n", errno, spdk_strerror(errno));
		return -ECANCELED;
	} else if (rc == 0) {
		return 0;
	}

	for (i = 0; i < rc; i++) {
		rdma_wr = (struct nvme_rdma_wr *)wc[i].wr_id;
		switch (rdma_wr->type) {
		case RDMA_WR_TYPE_RECV:
			_rc = nvme_rdma_process_recv_completion(poller, &wc[i], rdma_wr);
			break;

		case RDMA_WR_TYPE_SEND:
			_rc = nvme_rdma_process_send_completion(poller, rdma_qpair, &wc[i], rdma_wr);
			break;

		default:
			NVME_RQPAIR_ERRLOG(rdma_qpair, "Received an unexpected opcode on the CQ: %d\n", rdma_wr->type);
			return -ECANCELED;
		}
		if (spdk_likely(_rc >= 0)) {
			reaped += _rc;
		} else {
			completion_rc = _rc;
		}
	}

	*rdma_completions += rc;

	if (spdk_unlikely(completion_rc)) {
		return completion_rc;
	}

	return reaped;
}

static void
dummy_disconnected_qpair_cb(struct spdk_nvme_qpair *qpair, void *poll_group_ctx)
{

}

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
	if (qpair->poll_group != NULL) {
		return spdk_nvme_poll_group_process_completions(qpair->poll_group->group, max_completions,
				dummy_disconnected_qpair_cb);
	}

	if (max_completions == 0) {
		max_completions = rqpair->num_entries;
	} else {
		max_completions = spdk_min(max_completions, rqpair->num_entries);
	}

	switch (nvme_qpair_get_state(qpair)) {
	case NVME_QPAIR_CONNECTING:
		rc = nvme_rdma_ctrlr_connect_qpair_poll(qpair->ctrlr, qpair);
		if (rc == 0) {
			/* Once the connection is completed, we can submit queued requests */
			nvme_qpair_resubmit_requests(qpair, rqpair->num_entries);
		} else if (rc != -EAGAIN) {
			NVME_RQPAIR_ERRLOG(rqpair, "Failed to connect\n");
			goto failed;
		} else if (rqpair->state <= NVME_RDMA_QPAIR_STATE_INITIALIZING) {
			return 0;
		}
		break;

	case NVME_QPAIR_DISCONNECTING:
		nvme_rdma_ctrlr_disconnect_qpair_poll(qpair->ctrlr, qpair);
		return -ENXIO;

	default:
		nvme_rdma_qpair_process_cm_event(rqpair);
		break;
	}

	if (spdk_unlikely(qpair->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE)) {
		goto failed;
	}

	cq = rqpair->cq;

	rqpair->num_completions = 0;
	do {
		batch_size = spdk_min((max_completions - rqpair->num_completions), MAX_COMPLETIONS_PER_POLL);
		rc = nvme_rdma_cq_process_completions(cq, batch_size, NULL, rqpair, &rdma_completions);

		if (rc == 0) {
			break;
			/* Handle the case where we fail to poll the cq. */
		} else if (rc == -ECANCELED) {
			goto failed;
		} else if (rc == -ENXIO) {
			return rc;
		}
	} while (rqpair->num_completions < max_completions);

	if (spdk_unlikely(nvme_rdma_qpair_submit_sends(rqpair) ||
			  nvme_rdma_qpair_submit_recvs(rqpair))) {
		goto failed;
	}

	if (qpair->ctrlr->timeout_enabled) {
		nvme_rdma_qpair_check_timeout(qpair);
	}

	return rqpair->num_completions;

failed:
	nvme_rdma_fail_qpair(qpair, 0);
	return -ENXIO;
}

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
	return UINT32_MAX;
}

static uint16_t
nvme_rdma_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_rdma_ctrlr *rctrlr = nvme_rdma_ctrlr(ctrlr);
	uint32_t max_sge = rctrlr->max_sge;
	uint32_t max_in_capsule_sge = (ctrlr->cdata.nvmf_specific.ioccsz * 16 -
				       sizeof(struct spdk_nvme_cmd)) /
				      sizeof(struct spdk_nvme_sgl_descriptor);

	/* Max SGE is limited by capsule size */
	max_sge = spdk_min(max_sge, max_in_capsule_sge);
	/* Max SGE may be limited by MSDBD.
	 * If umr_per_io is enabled and supported, we always use virtually contig buffer, we don't limit max_sge by
	 * MSDBD in that case */
	if (!(g_spdk_nvme_transport_opts.rdma_umr_per_io &&
	      spdk_rdma_provider_accel_sequence_supported()) &&
	    ctrlr->cdata.nvmf_specific.msdbd != 0) {
		max_sge = spdk_min(max_sge, ctrlr->cdata.nvmf_specific.msdbd);
	}

	/* Max SGE can't be less than 1 */
	max_sge = spdk_max(1, max_sge);
	return max_sge;
}

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
			return rc;
		}
	}

	return 0;
}

static int
nvme_rdma_qpair_authenticate(struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	int rc;

	/* If the qpair is still connecting, it'll be forced to authenticate later on */
	if (rqpair->state < NVME_RDMA_QPAIR_STATE_RUNNING) {
		return 0;
	} else if (rqpair->state != NVME_RDMA_QPAIR_STATE_RUNNING) {
		return -ENOTCONN;
	}

	rc = nvme_fabric_qpair_authenticate_async(qpair);
	if (rc == 0) {
		nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTING);
		rqpair->state = NVME_RDMA_QPAIR_STATE_AUTHENTICATING;
	}

	return rc;
}

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
			continue;
		}

		nvme_rdma_req_complete(rdma_req, &cpl, false);
	}
}

static void
nvme_rdma_poller_destroy(struct nvme_rdma_poller *poller)
{
	if (poller->cq) {
		ibv_destroy_cq(poller->cq);
	}
	if (poller->rsps) {
		nvme_rdma_free_rsps(poller->rsps);
	}
	if (poller->srq) {
		spdk_rdma_provider_srq_destroy(poller->srq);
	}
	if (poller->mr_map) {
		spdk_rdma_utils_free_mem_map(&poller->mr_map);
	}
	if (poller->pd) {
		spdk_rdma_utils_put_pd(poller->pd);
	}
	free(poller);
}

static struct nvme_rdma_poller *
nvme_rdma_poller_create(struct nvme_rdma_poll_group *group, struct ibv_context *ctx)
{
	struct nvme_rdma_poller *poller;
	struct ibv_device_attr dev_attr;
	struct spdk_rdma_provider_srq_init_attr srq_init_attr = {};
	struct nvme_rdma_rsp_opts opts;
	int num_cqe, max_num_cqe;
	int rc;

	poller = calloc(1, sizeof(*poller));
	if (poller == NULL) {
		SPDK_ERRLOG("Unable to allocate poller.\n");
		return NULL;
	}

	poller->group = group;
	poller->device = ctx;

	if (g_spdk_nvme_transport_opts.rdma_srq_size != 0) {
		rc = ibv_query_device(ctx, &dev_attr);
		if (rc) {
			SPDK_ERRLOG("Unable to query RDMA device.\n");
			goto fail;
		}

		poller->pd = spdk_rdma_utils_get_pd(ctx);
		if (poller->pd == NULL) {
			SPDK_ERRLOG("Unable to get PD.\n");
			goto fail;
		}

		poller->mr_map = spdk_rdma_utils_create_mem_map(poller->pd, &g_nvme_hooks,
				 IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
		if (poller->mr_map == NULL) {
			SPDK_ERRLOG("Unable to create memory map.\n");
			goto fail;
		}

		srq_init_attr.stats = &poller->stats.rdma_stats.recv;
		srq_init_attr.pd = poller->pd;
		srq_init_attr.srq_init_attr.attr.max_wr = spdk_min((uint32_t)dev_attr.max_srq_wr,
				g_spdk_nvme_transport_opts.rdma_srq_size);
		srq_init_attr.srq_init_attr.attr.max_sge = spdk_min(dev_attr.max_sge,
				NVME_RDMA_DEFAULT_RX_SGE);

		poller->srq = spdk_rdma_provider_srq_create(&srq_init_attr);
		if (poller->srq == NULL) {
			SPDK_ERRLOG("Unable to create SRQ.\n");
			goto fail;
		}

		opts.num_entries = g_spdk_nvme_transport_opts.rdma_srq_size;
		opts.rqpair = NULL;
		opts.srq = poller->srq;
		opts.mr_map = poller->mr_map;

		poller->rsps = nvme_rdma_create_rsps(&opts);
		if (poller->rsps == NULL) {
			SPDK_ERRLOG("Unable to create poller RDMA responses.\n");
			goto fail;
		}

		rc = nvme_rdma_poller_submit_recvs(poller);
		if (rc) {
			SPDK_ERRLOG("Unable to submit poller RDMA responses.\n");
			goto fail;
		}

		/*
		 * When using an srq, fix the size of the completion queue at startup.
		 * The initiator sends only send and recv WRs. Hence, the multiplier is 2.
		 * (The target sends also data WRs. Hence, the multiplier is 3.)
		 */
		num_cqe = g_spdk_nvme_transport_opts.rdma_srq_size * 2;
	} else {
		num_cqe = DEFAULT_NVME_RDMA_CQ_SIZE;
	}

	max_num_cqe = g_spdk_nvme_transport_opts.rdma_max_cq_size;
	if (max_num_cqe != 0 && num_cqe > max_num_cqe) {
		num_cqe = max_num_cqe;
	}

	poller->cq = ibv_create_cq(poller->device, num_cqe, group, NULL, 0);

	if (poller->cq == NULL) {
		SPDK_ERRLOG("Unable to create CQ, errno %d.\n", errno);
		goto fail;
	}

	STAILQ_INSERT_HEAD(&group->pollers, poller, link);
	group->num_pollers++;
	poller->current_num_wc = num_cqe;
	poller->required_num_wc = 0;
	return poller;

fail:
	nvme_rdma_poller_destroy(poller);
	return NULL;
}

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

static struct nvme_rdma_poller *
nvme_rdma_poll_group_get_poller(struct nvme_rdma_poll_group *group, struct ibv_context *device)
{
	struct nvme_rdma_poller *poller = NULL;

	STAILQ_FOREACH(poller, &group->pollers, link) {
		if (poller->device == device) {
			break;
		}
	}

	if (!poller) {
		poller = nvme_rdma_poller_create(group, device);
		if (!poller) {
			SPDK_ERRLOG("Failed to create a poller for device %p\n", device);
			return NULL;
		}
	}

	poller->refcnt++;
	return poller;
}

static void
nvme_rdma_poll_group_put_poller(struct nvme_rdma_poll_group *group, struct nvme_rdma_poller *poller)
{
	assert(poller->refcnt > 0);

	if (--poller->refcnt == 0) {
		STAILQ_REMOVE(&group->pollers, poller, nvme_rdma_poller, link);
		group->num_pollers--;
		nvme_rdma_poller_destroy(poller);
	}
}

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
	return &group->group;
}

static int
nvme_rdma_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static int
nvme_rdma_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	struct nvme_rdma_poll_group *group = nvme_rdma_poll_group(qpair->poll_group);

	if (TAILQ_ENTRY_ENQUEUED(rqpair, link_connecting)) {
		TAILQ_REMOVE_CLEAR(&group->connecting_qpairs, rqpair, link_connecting);
	}

	return 0;
}

static int
nvme_rdma_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			 struct spdk_nvme_qpair *qpair)
{
	return 0;
}

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
		NVME_RQPAIR_INFOLOG(rqpair, "nvme state %d, rdma state %d, force disconnect\n", qpair->state,
				    rqpair->state);
		nvme_rdma_ctrlr_disconnect_qpair(qpair->ctrlr, qpair);
	}

	if (TAILQ_ENTRY_ENQUEUED(rqpair, link_active)) {
		TAILQ_REMOVE_CLEAR(&group->active_qpairs, rqpair, link_active);
	}

	return 0;
}

static inline void
nvme_rdma_qpair_process_submits(struct nvme_rdma_poll_group *group,
				struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_qpair	*qpair = &rqpair->qpair;

	assert(TAILQ_ENTRY_ENQUEUED(rqpair, link_active));

	if (spdk_unlikely(rqpair->state <= NVME_RDMA_QPAIR_STATE_INITIALIZING ||
			  rqpair->state >= NVME_RDMA_QPAIR_STATE_EXITING)) {
		return;
	}

	if (qpair->ctrlr->timeout_enabled) {
		nvme_rdma_qpair_check_timeout(qpair);
	}

	nvme_rdma_qpair_submit_sends(rqpair);
	if (!rqpair->srq) {
		nvme_rdma_qpair_submit_recvs(rqpair);
	}
	if (rqpair->num_completions > 0) {
		nvme_qpair_resubmit_requests(qpair, rqpair->num_completions);
		rqpair->num_completions = 0;
	}

	if (rqpair->num_outstanding_reqs == 0 && STAILQ_EMPTY(&qpair->queued_req)) {
		TAILQ_REMOVE_CLEAR(&group->active_qpairs, rqpair, link_active);
	}
}

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
		completions_per_qpair = MAX_COMPLETIONS_PER_POLL;
	}

	group = nvme_rdma_poll_group(tgroup);

	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
		rc = nvme_rdma_ctrlr_disconnect_qpair_poll(qpair->ctrlr, qpair);
		if (rc == 0) {
			disconnected_qpair_cb(qpair, tgroup->group->ctx);
		}
	}

	TAILQ_FOREACH_SAFE(rqpair, &group->connecting_qpairs, link_connecting, tmp_rqpair) {
		qpair = &rqpair->qpair;

		rc = nvme_rdma_ctrlr_connect_qpair_poll(qpair->ctrlr, qpair);
		if (rc == 0 || rc != -EAGAIN) {
			TAILQ_REMOVE_CLEAR(&group->connecting_qpairs, rqpair, link_connecting);

			if (rc == 0) {
				/* Once the connection is completed, we can submit queued requests */
				nvme_qpair_resubmit_requests(qpair, rqpair->num_entries);
			} else if (rc != -EAGAIN) {
				NVME_RQPAIR_ERRLOG(rqpair, "Failed to connect\n");
				nvme_rdma_fail_qpair(qpair, 0);
			}
		}
	}

	STAILQ_FOREACH_SAFE(qpair, &tgroup->connected_qpairs, poll_group_stailq, tmp_qpair) {
		rqpair = nvme_rdma_qpair(qpair);

		if (spdk_likely(nvme_qpair_get_state(qpair) != NVME_QPAIR_CONNECTING)) {
			nvme_rdma_qpair_process_cm_event(rqpair);
		}

		if (spdk_unlikely(qpair->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE)) {
			rc2 = -ENXIO;
			nvme_rdma_fail_qpair(qpair, 0);
		}
	}

	completions_allowed = completions_per_qpair * tgroup->num_connected_qpairs;
	if (spdk_likely(group->num_pollers)) {
		completions_per_poller = spdk_max(completions_allowed / group->num_pollers, 1);
	}

	STAILQ_FOREACH(poller, &group->pollers, link) {
		poller_completions = 0;
		rdma_completions = 0;
		do {
			poller->stats.polls++;
			batch_size = spdk_min((completions_per_poller - poller_completions), MAX_COMPLETIONS_PER_POLL);
			rc = nvme_rdma_cq_process_completions(poller->cq, batch_size, poller, NULL, &rdma_completions);
			if (rc <= 0) {
				if (rc == -ECANCELED) {
					return -EIO;
				} else if (rc == 0) {
					poller->stats.idle_polls++;
				}
				break;
			}

			poller_completions += rc;
		} while (poller_completions < completions_per_poller);
		total_completions += poller_completions;
		poller->stats.completions += rdma_completions;
		if (poller->srq) {
			nvme_rdma_poller_submit_recvs(poller);
		}
	}

	TAILQ_FOREACH_SAFE(rqpair, &group->active_qpairs, link_active, tmp_rqpair) {
		nvme_rdma_qpair_process_submits(group, rqpair);
	}

	return rc2 != 0 ? rc2 : total_completions;
}

/*
 * Handle disconnected qpairs when interrupt support gets added.
 */
static void
nvme_rdma_poll_group_check_disconnected_qpairs(struct spdk_nvme_transport_poll_group *tgroup,
		spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
}

static int
nvme_rdma_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	struct nvme_rdma_poll_group	*group = nvme_rdma_poll_group(tgroup);

	if (!STAILQ_EMPTY(&tgroup->connected_qpairs) || !STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
		return -EBUSY;
	}

	nvme_rdma_poll_group_free_pollers(group);
	free(group);

	return 0;
}

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
	stats->rdma.num_devices = group->num_pollers;

	if (stats->rdma.num_devices == 0) {
		*_stats = stats;
		return 0;
	}

	stats->rdma.device_stats = calloc(stats->rdma.num_devices, sizeof(*stats->rdma.device_stats));
	if (!stats->rdma.device_stats) {
		SPDK_ERRLOG("Can't allocate memory for RDMA device stats\n");
		free(stats);
		return -ENOMEM;
	}

	STAILQ_FOREACH(poller, &group->pollers, link) {
		device_stat = &stats->rdma.device_stats[i];
		device_stat->name = poller->device->device->name;
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

static void
nvme_rdma_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
				struct spdk_nvme_transport_poll_group_stat *stats)
{
	if (stats) {
		free(stats->rdma.device_stats);
	}
	free(stats);
}

static int
nvme_rdma_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_memory_domain **domains, int array_size)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(ctrlr->adminq);

	if (domains && array_size > 0) {
		domains[0] = rqpair->rdma_qp->domain;
	}

	return 1;
}

static int
nvme_rdma_ctrlr_process_transport_events(struct spdk_nvme_ctrlr *ctrlr)
{
	return nvme_rdma_poll_events(nvme_rdma_ctrlr(ctrlr));
}

void
spdk_nvme_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks)
{
	g_nvme_hooks = *hooks;
}

const struct spdk_nvme_transport_ops rdma_ops = {
	.name = "RDMA",
	.type = SPDK_NVME_TRANSPORT_RDMA,
	.ctrlr_construct = nvme_rdma_ctrlr_construct,
	.ctrlr_scan = nvme_fabric_ctrlr_scan,
	.ctrlr_destruct = nvme_rdma_ctrlr_destruct,
	.ctrlr_enable = nvme_rdma_ctrlr_enable,

	.ctrlr_set_reg_4 = nvme_fabric_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_fabric_ctrlr_set_reg_8,
	.ctrlr_get_reg_4 = nvme_fabric_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_fabric_ctrlr_get_reg_8,
	.ctrlr_set_reg_4_async = nvme_fabric_ctrlr_set_reg_4_async,
	.ctrlr_set_reg_8_async = nvme_fabric_ctrlr_set_reg_8_async,
	.ctrlr_get_reg_4_async = nvme_fabric_ctrlr_get_reg_4_async,
	.ctrlr_get_reg_8_async = nvme_fabric_ctrlr_get_reg_8_async,

	.ctrlr_get_max_xfer_size = nvme_rdma_ctrlr_get_max_xfer_size,
	.ctrlr_get_max_sges = nvme_rdma_ctrlr_get_max_sges,

	.ctrlr_create_io_qpair = nvme_rdma_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_rdma_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_rdma_ctrlr_connect_qpair,
	.ctrlr_disconnect_qpair = nvme_rdma_ctrlr_disconnect_qpair,

	.ctrlr_get_memory_domains = nvme_rdma_ctrlr_get_memory_domains,
	.ctrlr_process_transport_events = nvme_rdma_ctrlr_process_transport_events,

	.qpair_abort_reqs = nvme_rdma_qpair_abort_reqs,
	.qpair_reset = nvme_rdma_qpair_reset,
	.qpair_submit_request = nvme_rdma_qpair_submit_request,
	.qpair_process_completions = nvme_rdma_qpair_process_completions,
	.qpair_iterate_requests = nvme_rdma_qpair_iterate_requests,
	.qpair_authenticate = nvme_rdma_qpair_authenticate,
	.admin_qpair_abort_aers = nvme_rdma_admin_qpair_abort_aers,

	.poll_group_create = nvme_rdma_poll_group_create,
	.poll_group_connect_qpair = nvme_rdma_poll_group_connect_qpair,
	.poll_group_disconnect_qpair = nvme_rdma_poll_group_disconnect_qpair,
	.poll_group_add = nvme_rdma_poll_group_add,
	.poll_group_remove = nvme_rdma_poll_group_remove,
	.poll_group_process_completions = nvme_rdma_poll_group_process_completions,
	.poll_group_check_disconnected_qpairs = nvme_rdma_poll_group_check_disconnected_qpairs,
	.poll_group_destroy = nvme_rdma_poll_group_destroy,
	.poll_group_get_stats = nvme_rdma_poll_group_get_stats,
	.poll_group_free_stats = nvme_rdma_poll_group_free_stats,
};

SPDK_NVME_TRANSPORT_REGISTER(rdma, &rdma_ops);
