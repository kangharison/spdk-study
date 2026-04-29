/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe/TCP transport
 */

/*
 * [한국어 설명] NVMe over TCP 호스트 측 트랜스포트 구현 (nvme_tcp.c)
 *
 * === 파일의 역할 ===
 * SPDK NVMe 드라이버의 **TCP 트랜스포트 vtable 구현체**. NVMe-oF 1.0 + TP 8000(TCP transport)에
 * 정의된 호스트(initiator) 측 프로토콜을 모두 처리한다. 즉 평범한 TCP 소켓 위에 NVMe-oF의
 * **PDU(Protocol Data Unit)** 들을 인코딩/디코딩하여 NVMe SQE/CQE 의미론을 메시지 기반으로
 * 옮겨준다. PCIe(MMIO/doorbell)와 달리 모든 명령은 캡슐(capsule) 형태로 직렬화되며, 데이터는
 * inline (capsule 내) 또는 별도의 H2C/C2H Data PDU로 전송된다.
 *
 * 이 파일이 처리하는 5가지 핵심 책임:
 *   1) **PDU 종류별 인코딩/디코딩**: ICReq/ICResp(연결 초기화), CapsuleCmd/CapsuleResp(NVMe SQE/CQE),
 *      H2CData(Host-to-Controller 데이터), C2HData(Controller-to-Host 데이터), R2T(Ready to Transfer,
 *      타깃이 호스트에 데이터 요청), TermReq(에러 종료) — 각 PDU의 헤더 인코딩 + payload 송수신.
 *   2) **연결 핸드셰이크**: TCP connect → ICReq/ICResp 교환 (Fabrics CONNECT 이전 트랜스포트 협상,
 *      hdgst/ddgst/maxh2cdata/cpda 결정) → Fabric CONNECT → 옵션 인증(DH-CHAP) → RUNNING.
 *   3) **R2T 흐름 제어**: 타깃이 R2T PDU로 호스트에 "이 ttag로 r2to부터 r2tl 바이트만큼 보내라"고
 *      요청하면 호스트가 H2C Data PDU로 응답. maxr2t 협상으로 동시 outstanding R2T 수 제한.
 *   4) **HDGST/DDGST CRC32C**: 헤더/데이터 다이제스트(NVMe-oF TCP 스펙). 호스트 옵션과 ICResp 협상에
 *      따라 헤더 끝/데이터 끝에 4바이트 CRC32C를 붙여 무결성 검증. 가능 시 accel framework로 오프로드.
 *   5) **TLS PSK 옵션 (TP 8011)**: ctrlr->opts.tls_psk 키가 있으면 SSL 소켓 구현으로 라우팅하고
 *      RFC 8446 TLS 1.3 + PSK 식별자 유도(retained PSK → tls PSK) 처리.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 **TCP 트랜스포트 ops 테이블(`tcp_ops`)의 소유자**이다. 파일 맨 아래의
 * `SPDK_NVME_TRANSPORT_REGISTER(tcp, &tcp_ops)` 매크로가 main() 진입 전에 nvme_transport.c의
 * 전역 TAILQ에 TCP ops를 삽입 → 이후 상위 레이어(nvme_transport_*)가 trtype=TCP인 컨트롤러에
 * 대해 이 파일의 함수들을 vtable로 호출.
 *
 * 호출 체인 (qpair 생성 + 연결 + 첫 IO):
 *   [Application] spdk_nvme_connect(trid:TCP)
 *     → nvme_transport_ctrlr_construct
 *         → ops.ctrlr_construct = nvme_tcp_ctrlr_construct
 *             → nvme_tcp_generate_tls_credentials (PSK 옵션 시)
 *             → nvme_tcp_ctrlr_create_qpair (admin q)
 *     → nvme_transport_ctrlr_connect_qpair
 *         → ops.ctrlr_connect_qpair = nvme_tcp_ctrlr_connect_qpair
 *             → nvme_tcp_qpair_connect_sock (spdk_sock_connect_async)
 *                 [callback] nvme_tcp_sock_connect_cb_fn
 *                     → nvme_tcp_qpair_icreq_send (ICReq PDU)
 *         → 이후 process_completions가 ICResp 수신 → nvme_tcp_icresp_handle
 *             → STATE: FABRIC_CONNECT_SEND
 *         → 다음 polling: nvme_fabric_qpair_connect_async (Fabric CONNECT)
 *         → 이후 polling: nvme_fabric_qpair_connect_poll → RUNNING (또는 AUTHENTICATING)
 *     → spdk_nvme_ns_cmd_read/write
 *         → ops.qpair_submit_request = nvme_tcp_qpair_submit_request
 *             → nvme_tcp_req_init (SGL 빌드, in-capsule 판단)
 *             → nvme_tcp_qpair_capsule_cmd_send (CapsuleCmd PDU 발사)
 *
 * 호출 체인 (read 완료):
 *   [poll group thread] spdk_sock_group_poll → nvme_tcp_qpair_sock_cb
 *     → spdk_nvme_qpair_process_completions → nvme_tcp_qpair_process_completions
 *         → nvme_tcp_read_pdu (state machine)
 *             → AWAIT_PDU_CH → AWAIT_PDU_PSH → AWAIT_PDU_PAYLOAD
 *             → nvme_tcp_pdu_psh_handle → 분기:
 *                 - C2H_DATA: nvme_tcp_c2h_data_hdr_handle → ...payload_handle
 *                 - CAPSULE_RESP: nvme_tcp_capsule_resp_hdr_handle → req_complete
 *                 - R2T: nvme_tcp_r2t_hdr_handle → nvme_tcp_send_h2c_data
 *
 * === 타 모듈과의 연결 ===
 *  - `spdk_internal/nvme_tcp.h` — PDU 헤더 정의(spdk_nvme_tcp_*_hdr), build_iovs 헬퍼, 다이제스트 계산.
 *  - `spdk_internal/sock.h` (spdk_sock_*) — POSIX/io_uring/SSL 소켓 추상화, async writev, group_poll.
 *  - `nvme_fabric.c` — Fabric CONNECT/AUTH/Property R/W. 이 파일의 ctrlr_set_reg_*는 fabric 함수를 그대로 위임.
 *  - `nvme_internal.h` — nvme_request, spdk_nvme_qpair, nvme_qpair_init/deinit, nvme_complete_request 등.
 *  - `spdk/crc32.h` — HDGST/DDGST 계산 (CRC32C 다항식, intel SSE 가속 가능).
 *  - `spdk/dma.h` (spdk_memory_domain_*) — accel framework 통합 (외부 메모리 도메인 → system 도메인 변환).
 *  - 트레이스 포인트 NVME_TCP_SUBMIT/COMPLETE — spdk_trace 통한 SUBMIT→COMPLETE 라이프타임 시각화.
 *
 * === 주요 함수/구조체 요약 ===
 *  ★ nvme_tcp_ctrlr_construct        — TCP 컨트롤러 생성 (tctrlr alloc + admin qpair)
 *  ★ nvme_tcp_qpair_connect_sock     — TCP socket connect (TLS 옵션 분기)
 *  ★ nvme_tcp_qpair_icreq_send       — ICReq PDU 발사 (트랜스포트 협상)
 *  ★ nvme_tcp_icresp_handle          — ICResp 수신 (maxh2cdata/cpda/digest 협상 결과 저장)
 *  ★ nvme_tcp_ctrlr_connect_qpair_poll — qpair 연결 상태머신 (SOCK→INIT→FABRIC→AUTH→RUNNING)
 *  ★ nvme_tcp_qpair_submit_request   — IO submit 진입점 (req_init + capsule_cmd_send)
 *  ★ nvme_tcp_qpair_capsule_cmd_send — CapsuleCmd PDU (NVMe SQE 캡슐화 + in-capsule data)
 *  ★ nvme_tcp_send_h2c_data          — R2T 응답 H2C Data PDU 발사 (maxh2cdata 청크 단위)
 *  ★ nvme_tcp_r2t_hdr_handle         — R2T 수신 처리 (active_r2ts 카운트 + h2c trigger)
 *  ★ nvme_tcp_c2h_data_hdr_handle    — C2H Data 헤더 검증 + payload iov 매핑
 *  ★ nvme_tcp_qpair_process_completions — 메인 완료 polling 루프
 *  ★ nvme_tcp_read_pdu               — PDU 수신 상태머신 (CH→PSH→PAYLOAD)
 *  ★ nvme_tcp_qpair_write_pdu        — PDU 송신 (헤더 digest + 데이터 digest + sock writev)
 *  ★ nvme_tcp_qpair_send_h2c_term_req — 프로토콜 위반 시 H2C TermReq로 정리
 *    nvme_tcp_generate_tls_credentials — TLS PSK 식별자/리테인드/TLS PSK 유도
 *    tcp_ops 테이블                   — 파일 맨 아래, vtable 매핑 + SPDK_NVME_TRANSPORT_REGISTER
 *
 * 핵심 자료구조:
 *   nvme_tcp_ctrlr      — spdk_nvme_ctrlr 확장 (PSK 보관)
 *   nvme_tcp_qpair      — spdk_nvme_qpair 확장 (sock, send_queue, recv_pdu, state, flags)
 *   nvme_tcp_req        — nvme_request 래퍼 (cid, ttag, datao, ordering 비트마스크, pdu)
 *   nvme_tcp_poll_group — spdk_sock_group + needs_poll/timeout TAILQ
 *
 * 이 파일을 이해하면 SPDK가 어떻게 평범한 TCP 위에서 NVMe SSD와 동등한 IO 명령 모델을 구현하는지,
 * R2T 흐름 제어와 inline data가 어떻게 레이턴시/처리량을 최적화하는지가 드러난다.
 */

#include "nvme_internal.h"      /* [한국어] SPDK NVMe 드라이버 내부 정의 — nvme_request, spdk_nvme_qpair,
                                 *         nvme_qpair_init/deinit, nvme_ctrlr_*, nvme_complete_request 등.
                                 *         이 헤더가 spdk/nvme.h를 포함하므로 공개 API도 모두 사용 가능. */

#include "spdk/endian.h"        /* [한국어] from/to_be/le 매크로 — 네트워크 바이트 오더 변환 (TCP는 big-endian
                                 *         이지만 NVMe-oF PDU는 little-endian 필드가 많아 명시적 변환 필요). */
#include "spdk/likely.h"        /* [한국어] spdk_likely/spdk_unlikely — 분기 예측 힌트 매크로 (hot path 최적화). */
#include "spdk/string.h"        /* [한국어] spdk_strerror — errno → 사람이 읽을 수 있는 문자열 변환. */
#include "spdk/stdinc.h"        /* [한국어] 표준 라이브러리 일괄 포함 (string.h, errno.h, stdint.h, sys/socket.h 등). */
#include "spdk/crc32.h"         /* [한국어] CRC32C 계산 함수 — HDGST/DDGST 다이제스트 생성·검증에 사용
                                 *         (NVMe-oF TCP 스펙: Castagnoli 다항식, SSE4.2 명령어 가속). */
#include "spdk/assert.h"        /* [한국어] SPDK_STATIC_ASSERT — 컴파일 타임 크기/정렬 검증 매크로. */
#include "spdk/trace.h"         /* [한국어] spdk_trace_record — 가벼운 트레이스 포인트 기록 (성능 분석용). */
#include "spdk/util.h"          /* [한국어] SPDK_COUNTOF, SPDK_CONTAINEROF 등 공통 유틸 매크로. */
#include "spdk/nvmf.h"          /* [한국어] NVMe-oF 공통 정의 — Fabric capsule cmd, NVMF_PSK_IDENTITY_LEN 등. */
#include "spdk/dma.h"           /* [한국어] spdk_memory_domain_* — 외부 메모리(예: GPU) 도메인 → 시스템 도메인 변환.
                                 *         accel framework가 외부 메모리에 대한 CRC32 계산을 수행하기 위해 필요. */

#include "spdk_internal/nvme_tcp.h"  /* [한국어] NVMe-oF TCP 프로토콜 헤더 정의 — spdk_nvme_tcp_ic_req/resp,
                                       *         spdk_nvme_tcp_cmd, spdk_nvme_tcp_rsp, spdk_nvme_tcp_h2c_data_hdr,
                                       *         spdk_nvme_tcp_c2h_data_hdr, spdk_nvme_tcp_r2t_hdr,
                                       *         spdk_nvme_tcp_term_req_hdr, common pdu hdr (pdu_type/flags/hlen/pdo/plen),
                                       *         g_nvme_tcp_hdgst[]/g_nvme_tcp_ddgst[] PDU 타입별 다이제스트 적용 표,
                                       *         nvme_tcp_pdu_calc_*_digest, MAKE_DIGEST_WORD/MATCH_DIGEST_WORD,
                                       *         nvme_tcp_build_iovs, nvme_tcp_read_data, nvme_tcp_pdu_set_data_buf 등. */
#include "spdk_internal/trace_defs.h"  /* [한국어] TRACE_NVME_TCP_SUBMIT/COMPLETE/SOCK_REQ_* tpoint ID 정의. */

/* [한국어] 사용처 없는 상수 (legacy). 디폴트 RW 임시 버퍼 크기로 정의되어 있으나 현재 코드에서 직접 참조되지 않는다. */
#define NVME_TCP_RW_BUFFER_SIZE 131072

/* For async connect workloads, allow more time since we are more likely
 * to be processing lots ICREQs at once.
 */
/* [한국어] ICReq → ICResp 핸드셰이크 타임아웃 상한.
 *   - 동기 모드(spdk_nvme_connect): 단일 큐페어를 호출자가 busy-wait로 기다리므로 짧게(2초).
 *   - 비동기 모드(connect_async + poll): 여러 큐페어가 동시에 ICReq를 발사할 수 있어 더 길게(10초)
 *     허용. 타임아웃 시 nvme_tcp_ctrlr_connect_qpair_poll에서 -ETIMEDOUT 반환 → 컨트롤러 fail. */
#define ICREQ_TIMEOUT_SYNC 2 /* in seconds */
#define ICREQ_TIMEOUT_ASYNC 10 /* in seconds */

/* [한국어] HPDA(Host PDU Data Alignment): 호스트가 보낼 H2C Data PDU의 데이터 시작점 정렬 요구.
 *         0이면 4바이트 정렬만 요구 (NVMe-oF TCP 스펙: PDA = 4 << HPDA). */
#define NVME_TCP_HPDA_DEFAULT			0
/* [한국어] 호스트가 ICReq에 적어보내는 maxr2t 디폴트값 — 한 번에 outstanding 가능한 R2T 수.
 *         스펙은 이 값을 0-base로 표현(maxr2t-1) → 1이면 ICReq에는 0이 들어가서 단일 R2T 의미. */
#define NVME_TCP_MAX_R2T_DEFAULT		1
/* [한국어] ICResp의 maxh2cdata 최소 검증값 — 4096 바이트 미만이면 협상 거부.
 *         NVMe-oF TCP 스펙 권장 최소 (한 번에 보낼 수 있는 H2C Data 최대 크기). */
#define NVME_TCP_PDU_H2C_MIN_DATA_SIZE		4096

/* [한국어] tqpair 컨텍스트의 로깅 매크로 묶음.
 *  NVME_QPAIR_*LOG는 nvme_internal.h가 제공하는 qpair 식별자(컨트롤러 trid + qid) 자동 prefix 출력 매크로.
 *  여기서는 추가로 [qpair_state, recv_state]를 prefix에 박아 PDU 상태머신 디버깅을 쉽게 한다.
 *  tqpair가 NULL일 수 있는 진입점(에러 경로)을 위해 삼항 연산자로 NULL-safe하게 작성. */
#define NVME_TQPAIR_ERRLOG(tqpair, format, ...) NVME_QPAIR_ERRLOG((tqpair) ? &(tqpair)->qpair : NULL, "[%s,%s] " format, (tqpair) ? nvme_tcp_qpair_state_string((tqpair)->state) : "", (tqpair) ? nvme_tcp_pdu_recv_state_string((tqpair)->recv_state) : "", ##__VA_ARGS__)
#define NVME_TQPAIR_WARNLOG(tqpair, format, ...) NVME_QPAIR_WARNLOG((tqpair) ? &(tqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_TQPAIR_NOTICELOG(tqpair, format, ...) NVME_QPAIR_NOTICELOG((tqpair) ? &(tqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_TQPAIR_INFOLOG(tqpair, format, ...) NVME_QPAIR_INFOLOG((tqpair) ? &(tqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_TQPAIR_DEBUGLOG(tqpair, format, ...) NVME_QPAIR_DEBUGLOG((tqpair) ? &(tqpair)->qpair : NULL, format, ##__VA_ARGS__)

/* [한국어] tqpair->state 전이를 한 줄로 (디버그 로그 + 대입). do-while(0) 관용구로 매크로 안전성 확보.
 *         상태머신 천이가 잦은 영역(SOCK→INIT→FABRIC→AUTH→RUNNING)에서 시점/순서를 트레이싱하기 위한 헬퍼. */
#define nvme_tcp_qpair_set_state(_qpair, _state) do { \
	NVME_TQPAIR_DEBUGLOG((_qpair), "setting tqpair state to %s\n", nvme_tcp_qpair_state_string((_state))); \
	(_qpair)->state = (_state); \
} while (0)

/*
 * Maximum value of transport_ack_timeout used by TCP controller
 */
/* [한국어] transport_ack_timeout(2^N ms 단위) 상한.
 *  31이면 약 2^31 ms ≈ 25일. 사용자가 더 큰 값을 주면 로그 후 31로 잘라 사용한다. */
#define NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT	31

/* [한국어] TCP qpair 연결 진행 상태머신.
 *  nvme_tcp_ctrlr_connect_qpair_poll이 이 enum을 보고 다음 단계 액션을 결정한다.
 *  순방향 천이만 일어나고 (SOCK_CONNECTING → INITIALIZING → FABRIC_CONNECT_SEND → FABRIC_CONNECT_POLL
 *  → [AUTHENTICATING →] RUNNING), 종료 시 EXITING → EXITED. */
enum nvme_tcp_qpair_state {
	NVME_TCP_QPAIR_STATE_INVALID = 0,
	/* [한국어] 0: 초기/무효 상태. tqpair 생성 직후에만 보일 수 있음. */
	NVME_TCP_QPAIR_STATE_SOCK_CONNECTING = 1,
	/* [한국어] 1: spdk_sock_connect_async 호출 후 TCP 3-way handshake가 진행 중. callback에서 INITIALIZING으로 전환. */
	NVME_TCP_QPAIR_STATE_INITIALIZING = 2,
	/* [한국어] 2: TCP 연결됨. ICReq 송신 + ICResp 수신 대기 (NVMe-oF TCP 트랜스포트 협상). */
	NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND = 3,
	/* [한국어] 3: ICResp까지 받음. 다음 polling에서 Fabric CONNECT capsule을 발사해야 함. */
	NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL = 4,
	/* [한국어] 4: Fabric CONNECT 발사됨. CONNECT 응답 capsule 도착 대기. */
	NVME_TCP_QPAIR_STATE_AUTHENTICATING = 5,
	/* [한국어] 5: CONNECT 응답에서 인증 필요로 표시됨 → DH-CHAP 인증 진행 중 (nvme_auth.c). */
	NVME_TCP_QPAIR_STATE_RUNNING = 6,
	/* [한국어] 6: 정상 운용 상태. 사용자 IO submit/complete 가능. */
	NVME_TCP_QPAIR_STATE_EXITING = 7,
	/* [한국어] 7: H2C TermReq 송신 완료 등으로 종료 진입 중. */
	NVME_TCP_QPAIR_STATE_EXITED = 8,
	/* [한국어] 8: 완전 종료. (현재 코드에서는 거의 EXITING까지만 사용) */
};

/* NVMe TCP transport extensions for spdk_nvme_ctrlr */
/* [한국어] TCP 전용 컨트롤러 확장 구조체.
 *  spdk_nvme_ctrlr를 inline 첫 멤버로 두어 SPDK_CONTAINEROF로 양방향 변환 가능.
 *  TCP 트랜스포트 고유 상태(주로 TLS PSK 협상 결과)만 추가로 보유. */
struct nvme_tcp_ctrlr {
	struct spdk_nvme_ctrlr			ctrlr;
	/* [한국어] 일반 NVMe 컨트롤러 객체. nvme_tcp_ctrlr()로 양방향 변환.
	 *  설정자: nvme_tcp_ctrlr_construct() 초기화 시 ctrlr.opts/trid 채움.
	 *  읽는 자: 모든 트랜스포트 vtable 함수가 ctrlr→tctrlr 변환 후 PSK 정보 참조. */

	char					psk_identity[NVMF_PSK_IDENTITY_LEN];
	/* [한국어] TLS PSK Identity 문자열 (RFC 8446 + NVMe-oF TP 8011).
	 *  형식: "NVMe<base>HostNQN SubsystemNQN" 형태로 nvme_tcp_generate_psk_identity가 채움.
	 *  설정자: nvme_tcp_generate_tls_credentials().
	 *  읽는 자: nvme_tcp_qpair_connect_sock()이 spdk_sock_impl_opts.psk_identity로 전달.
	 *  값 범위: ASCII 문자열, 길이 ≤ NVMF_PSK_IDENTITY_LEN(256). */

	uint8_t					psk[SPDK_TLS_PSK_MAX_LEN];
	/* [한국어] 유도된 TLS PSK 키 자체 (binary).
	 *  설정자: nvme_tcp_derive_tls_psk()로 retained PSK + identity → TLS PSK 유도.
	 *  읽는 자: SSL 소켓 구현이 TLS 1.3 PSK 핸드셰이크에 사용.
	 *  보안: 사용 후 zeroize는 spdk_memset_s로. 메모리 상에 유지되는 동안 노출 위험 주의. */

	int					psk_size;
	/* [한국어] psk[]의 실제 유효 바이트 수 (cipher 따라 32 또는 48).
	 *  설정자: nvme_tcp_derive_tls_psk()의 반환값.
	 *  읽는 자: spdk_sock_impl_opts.psk_key_size로 전달. */

	char					*tls_cipher_suite;
	/* [한국어] 협상된 TLS cipher suite 이름 ("TLS_AES_128_GCM_SHA256" 또는 "TLS_AES_256_GCM_SHA384").
	 *  설정자: nvme_tcp_generate_tls_credentials()가 PSK 크기로 선택.
	 *  읽는 자: spdk_sock_impl_opts.tls_cipher_suites로 전달. */
};

/* [한국어] TCP poll group — 같은 SPDK 스레드에 묶인 여러 qpair의 sock fd를 한꺼번에 polling.
 *  spdk_sock_group 위에 needs_poll/timeout TAILQ를 얹어 fairness/타임아웃 관리. */
struct nvme_tcp_poll_group {
	struct spdk_nvme_transport_poll_group group;
	/* [한국어] 트랜스포트 공통 poll group base. nvme_transport.c와 인터페이스. */

	struct spdk_sock_group *sock_group;
	/* [한국어] 실제 소켓 다중화 핸들 (epoll/kqueue 추상화).
	 *  설정자: nvme_tcp_poll_group_create().
	 *  읽는 자: 매 polling iteration마다 spdk_sock_group_poll() 호출 대상. */

	uint32_t completions_per_qpair;
	/* [한국어] 한 polling 호출당 qpair에서 reap할 완료 수의 상한.
	 *  설정자: process_completions 진입 시 인자로 받음.
	 *  읽는 자: nvme_tcp_qpair_sock_cb()가 spdk_nvme_qpair_process_completions에 전달. */

	int64_t num_completions;
	/* [한국어] 현재 polling 라운드에서 누적된 완료 수.
	 *  설정자: 라운드 시작 시 0으로 리셋, 각 sock_cb 후 증가.
	 *  읽는 자: poll_group_process_completions가 최종 반환값으로 사용.
	 *  음수(-ENXIO)면 에러 표시. */

	TAILQ_HEAD(, nvme_tcp_qpair) needs_poll;
	/* [한국어] "네트워크 이벤트 없이도 polling이 필요한" qpair 목록.
	 *  큐된 요청이 있거나 (자원 대기) 연결 진행 중인 경우 nvme_tcp_cond_schedule_qpair_polling이 추가.
	 *  poll_group_process_completions가 sock_group_poll 후 이 리스트도 한 번 더 처리하여 forward progress 보장. */

	TAILQ_HEAD(, nvme_tcp_qpair) timeout_enabled;
	/* [한국어] 사용자가 timeout_cb를 설정한 컨트롤러의 qpair 목록.
	 *  매 polling마다 nvme_tcp_qpair_check_timeout으로 outstanding 요청들의 시간 검사. */

	struct spdk_nvme_tcp_stat stats;
	/* [한국어] poll group 단위 통계 (polls, idle_polls, socket_completions, nvme_completions).
	 *  qpair->stats가 shared_stats=true일 때 이 필드를 가리킴 (poll group에 묶인 모든 qpair가 공유). */
};

/* NVMe TCP qpair extensions for spdk_nvme_qpair */
/* [한국어] TCP 전용 큐페어 확장 구조체. 호스트가 한 번에 하나의 큐페어 = 하나의 TCP 소켓을 가진다.
 *  PCIe와 달리 SQ/CQ ring이 아니라 단일 양방향 byte stream(TCP) 위에 PDU들이 직렬화됨. */
struct nvme_tcp_qpair {
	struct spdk_nvme_qpair			qpair;
	/* [한국어] 일반 NVMe qpair 객체 (queue id, cmd queue, ctrlr 포인터 등). */

	struct spdk_sock			*sock;
	/* [한국어] TCP 소켓 핸들 (POSIX/io_uring/SSL 구현 중 하나).
	 *  설정자: nvme_tcp_qpair_connect_sock()이 spdk_sock_connect_async로 생성.
	 *  읽는 자: 모든 PDU 송수신 — spdk_sock_writev_async, nvme_tcp_read_data 등.
	 *  수명: nvme_tcp_ctrlr_disconnect_qpair에서 spdk_sock_close. */

	TAILQ_HEAD(, nvme_tcp_req)		free_reqs;
	/* [한국어] 사용 가능한 nvme_tcp_req의 free list. nvme_tcp_alloc_reqs로 num_entries개 미리 할당 후 push.
	 *  nvme_tcp_req_get/put이 head에서 pop/push (LIFO, 캐시 친화적). */

	TAILQ_HEAD(, nvme_tcp_req)		outstanding_reqs;
	/* [한국어] 발사 후 완료 대기 중인 req 목록. submit 시 tail에 insert, complete 시 remove.
	 *  abort/disconnect 시 일괄 cancel 대상. timeout 검사 대상. */

	TAILQ_HEAD(, nvme_tcp_pdu)		send_queue;
	/* [한국어] 소켓에 writev_async 호출 대기 중 또는 완료 대기 중인 PDU들.
	 *  _tcp_write_pdu에서 tail insert → spdk_sock_writev_async → 완료 시 pdu_write_done에서 remove.
	 *  disconnect 시 모두 drain. */

	struct nvme_tcp_pdu			*recv_pdu;
	/* [한국어] 현재 수신 중인 PDU의 임시 버퍼 (한 큐페어당 하나, in-place state machine).
	 *  read_pdu가 ch/psh/payload를 점진적으로 채워 넣음.
	 *  send_pdus[num_entries+1] 위치에 배치. */

	struct nvme_tcp_pdu			*send_pdu; /* only for error pdu and init pdu */
	/* [한국어] ICReq, H2C TermReq 등 tcp_req와 결합되지 않는 단발 PDU 송신용 버퍼.
	 *  send_pdus[num_entries] 위치. tcp_req와 무관한 PDU 1개만 동시에 in-flight 가능. */

	struct nvme_tcp_pdu			*send_pdus; /* Used by tcp_reqs */
	/* [한국어] PDU 전체 풀의 base 포인터. (num_entries + 2)개 zmalloc. DMA 메모리(spdk_zmalloc).
	 *  앞 num_entries개는 각 tcp_req가 하나씩 소유, 마지막 2개가 send_pdu / recv_pdu. */

	enum nvme_tcp_pdu_recv_state		recv_state;
	/* [한국어] 수신 상태머신 현재 상태 (AWAIT_PDU_READY/CH/PSH/PAYLOAD/QUIESCING/ERROR/AWAIT_REQ).
	 *  nvme_tcp_qpair_set_recv_state 매크로로만 변경. read_pdu가 do-while 루프로 천이.  */

	struct nvme_tcp_req			*tcp_reqs;
	/* [한국어] num_entries개 tcp_req의 base 포인터. aligned_alloc(SPDK_CACHE_LINE_SIZE)로 정렬 보장. */

	struct spdk_nvme_tcp_stat		*stats;
	/* [한국어] 이 qpair의 통계 누적 위치. poll group에 속한 경우 group->stats를 가리키고(shared_stats=true),
	 *  단독이면 calloc된 자체 버퍼. nvme_tcp_qpair_capsule_cmd_send 등에서 submitted_requests++. */

	uint16_t				num_entries;
	/* [한국어] qpair가 동시에 보유 가능한 req(=PDU) 수. qsize - 1 (NVMe 스펙: 한 슬롯은 항상 비워둠). */

	uint16_t				async_complete;
	/* [한국어] in_completion_context=false에서 완료된 req 수의 카운터.
	 *  read_pdu 진입 시 reaped 초기값으로 사용 → process_completions의 반환값에 합산. */

	struct {
		uint16_t host_hdgst_enable: 1;
		/* [한국어] 호스트가 헤더 다이제스트(HDGST CRC32C)를 사용 중인지. ICResp에서 협상된 결과 저장.
		 *  세팅 시 모든 송신 PDU에 SPDK_NVME_TCP_CH_FLAGS_HDGSTF + 4byte CRC 추가, 수신 시 검증. */

		uint16_t host_ddgst_enable: 1;
		/* [한국어] 데이터 다이제스트(DDGST) 사용 중. 데이터를 가진 PDU(C2H/H2C/CapsuleCmd inline)에 영향. */

		uint16_t icreq_send_ack: 1;
		/* [한국어] ICReq 송신이 소켓 레벨에서 완료(buffer reclaimed)되었는지.
		 *  nvme_tcp_send_icreq_complete 콜백에서 1로. icresp_received와 함께 모두 1이 되면 FABRIC_CONNECT_SEND로 천이. */

		uint16_t icresp_received: 1;
		/* [한국어] ICResp PDU를 수신했는지. nvme_tcp_icresp_handle에서 1로. */

		uint16_t reserved: 12;
		/* [한국어] 예약 — 패딩 + 미래 플래그 확장 여유. 16비트 단위로 정렬되도록. */
	} flags;
	/* [한국어] 비트필드 묶음 — 16비트 안에 4개 1비트 플래그 + 12비트 예약. 캐시 라인 절약. */

	/** Specifies the maximum number of PDU-Data bytes per H2C Data Transfer PDU */
	uint32_t				maxh2cdata;
	/* [한국어] 한 번의 H2C Data PDU에 들어갈 수 있는 최대 데이터 크기.
	 *  설정자: nvme_tcp_icresp_handle()가 ICResp.maxh2cdata 그대로 저장.
	 *  읽는 자: nvme_tcp_send_h2c_data()가 r2tl_remain을 maxh2cdata 단위로 청크 분할 송신. */

	uint32_t				maxr2t;
	/* [한국어] 동시에 outstanding 가능한 R2T 수.
	 *  설정자: connect_qpair에서 NVME_TCP_MAX_R2T_DEFAULT(=1)로 초기화.
	 *  읽는 자: nvme_tcp_r2t_hdr_handle()가 active_r2ts > maxr2t 검증. */

	/* 0 based value, which is used to guide the padding */
	uint8_t					cpda;
	/* [한국어] Controller PDU Data Alignment (0-base). 실제 정렬 = (cpda+1) << 2 바이트.
	 *  설정자: ICResp.cpda 그대로 저장.
	 *  읽는 자: capsule_cmd_send / send_h2c_data가 PDO 계산 + padding 삽입에 사용. */

	enum nvme_tcp_qpair_state		state;
	/* [한국어] 위 enum의 현재 값. nvme_tcp_qpair_set_state로만 변경. */

	TAILQ_ENTRY(nvme_tcp_qpair)		link_poll;
	/* [한국어] poll_group->needs_poll 리스트의 link entry. ENQUEUED 매크로로 enqueue 여부 검사. */

	TAILQ_ENTRY(nvme_tcp_qpair)		link_timeout;
	/* [한국어] poll_group->timeout_enabled 리스트의 link entry. */

	uint64_t				icreq_timeout_tsc;
	/* [한국어] ICReq 송신 시 (현재 tsc + 2/10초)로 세팅. ctrlr_connect_qpair_poll에서 만료 검사 → -ETIMEDOUT. */

	bool					shared_stats;
	/* [한국어] qpair->stats가 poll group의 stats를 가리키는지 여부.
	 *  delete_io_qpair에서 단독이면 free(stats), 공유면 free 안 함. */
};

/* [한국어] tcp_req 상태 enum.
 *  - FREE: free_reqs 리스트 안. 사용 가능.
 *  - ACTIVE: outstanding_reqs 리스트 안. 발사됨, 완료 또는 R2T 대기 중.
 *  - ACTIVE_R2T: R2T 수신 후 H2C Data 송신 사이클 중. (PDU 1개를 점유) */
enum nvme_tcp_req_state {
	NVME_TCP_REQ_FREE,
	NVME_TCP_REQ_ACTIVE,
	NVME_TCP_REQ_ACTIVE_R2T,
};

/* [한국어] 한 NVMe-oF TCP IO 요청의 트래킹 객체.
 *  nvme_request(상위)와 nvme_tcp_pdu(하위)를 잇는 연결 고리이며, R2T flow-control과 ordering 비트 등
 *  TCP 트랜스포트 고유 상태를 가진다. cache line 정렬 보장(SPDK_STATIC_ASSERT). */
struct nvme_tcp_req {
	struct nvme_request			*req;
	/* [한국어] 상위 NVMe 요청 객체 (cmd capsule 원본 + cb_fn/cb_arg 보유).
	 *  설정자: nvme_tcp_req_init().
	 *  읽는 자: nvme_tcp_req_complete()가 req->cb_fn 호출. */

	enum nvme_tcp_req_state			state;
	/* [한국어] 위 enum 상태. nvme_tcp_req_get/put + r2t 핸들러에서 천이. */

	uint16_t				cid;
	/* [한국어] Command Identifier (NVMe SQE의 cid). free 풀 인덱스(0..num_entries-1) 그대로 사용.
	 *  설정자: nvme_tcp_alloc_reqs() 초기화 시 i.
	 *  읽는 자: req_init이 req->cmd.cid에 복사 → CapsuleResp/C2HData/R2T가 cid로 역참조. */

	uint16_t				ttag;
	/* [한국어] Transfer Tag — R2T가 발급하는 16비트 식별자.
	 *  설정자: nvme_tcp_r2t_hdr_handle()이 r2t->ttag 저장.
	 *  읽는 자: nvme_tcp_send_h2c_data()가 H2CData.ttag로 그대로 echo (target이 매칭). */

	uint32_t				datao;
	/* [한국어] 송신 측 누적 데이터 오프셋. r2t 응답 H2C Data를 청크로 보낼 때 진행도. */

	uint32_t				expected_datao;
	/* [한국어] 수신 측 (C2HData) 다음 기대 datao. C2HData.datao와 일치해야 함 (gap 검출). */

	uint32_t				r2tl_remain;
	/* [한국어] 현재 R2T 전송 단위에서 아직 보내지 않은 잔여 길이.
	 *  send_h2c_data가 maxh2cdata 청크씩 차감, 0이 되면 LAST_PDU 비트 세팅. */

	uint32_t				active_r2ts;
	/* [한국어] 현재 outstanding R2T 수. 매 r2t 수신 시 ++, h2c 완료 시 --.
	 *  maxr2t를 초과하면 R2T_LIMIT_EXCEEDED 에러로 termreq. */

	/* Used to hold a value received from subsequent R2T while we are still
	 * waiting for H2C complete */
	uint16_t				ttag_r2t_next;
	/* [한국어] (희귀) 이전 R2T의 H2C 송신을 기다리는 동안 다음 R2T가 도착하면 그 ttag를 임시 보관.
	 *  send 완료 후 r2t_waiting_h2c_complete 비트가 1이면 이 ttag로 후속 H2C 사이클 시작. */

	bool					in_capsule_data;
	/* [한국어] 데이터를 CapsuleCmd PDU에 inline으로 함께 보내는지 여부.
	 *  payload_size ≤ ioccsz_bytes(또는 admin/fabric은 8192)일 때만 true.
	 *  true면 별도 H2C Data PDU 없이 명령 한 방에 데이터까지 전달 (latency 절감). */

	/* It is used to track whether the req can be safely freed */
	/* [한국어] 완료 안전성 트래킹용 비트마스크 union.
	 *  TCP는 send와 recv가 각각 비동기 + accel 오프로드도 가능 → 이 모든 이벤트가 다 끝나야 free 가능.
	 *  raw로 한 번에 0 클리어, bits로 개별 접근. */
	union {
		uint8_t raw;
		/* [한국어] 8비트 통째로 0 클리어/검사용. */
		struct {
			/* The last send operation completed - kernel released send buffer */
			uint8_t				send_ack : 1;
			/* [한국어] 마지막 송신 PDU의 sock writev 완료 (커널이 송신 버퍼 회수).
			 *  세팅: nvme_tcp_qpair_cmd_send_complete / h2c_data_send_complete 콜백.
			 *  검사: nvme_tcp_req_complete_safe (data_recv && send_ack && !in_progress_accel). */

			/* Data transfer completed - target send resp or last data bit */
			uint8_t				data_recv : 1;
			/* [한국어] 응답/마지막 C2H 데이터 PDU 수신 완료.
			 *  세팅: capsule_resp 처리 시점 또는 LAST_PDU 플래그 가진 c2h_data 처리 시점. */

			/* tcp_req is waiting for completion of the previous send operation (buffer reclaim notification
			 * from kernel) to send H2C */
			uint8_t				h2c_send_waiting_ack : 1;
			/* [한국어] R2T 도착했지만 직전 PDU의 send_ack를 아직 못 받아 H2C 송신 보류 중.
			 *  send_ack가 1이 되는 콜백에서 nvme_tcp_send_h2c_data 트리거. */

			/* tcp_req received subsequent r2t while it is still waiting for send_ack.
			 * Rare case, actual when dealing with target that can send several R2T requests.
			 * SPDK TCP target sends 1 R2T for the whole data buffer */
			uint8_t				r2t_waiting_h2c_complete : 1;
			/* [한국어] (희귀) 이전 R2T의 H2C 사이클 중 다음 R2T가 도착했음을 표시.
			 *  ttag_r2t_next/r2tl_remain_next에 보관, 사이클 종료 시 처리. */

			/* Accel operation is in progress */
			uint8_t				in_progress_accel : 1;
			/* [한국어] accel framework가 비동기 sequence(예: 데이터 다이제스트 CRC32C 오프로드, copy)를 진행 중.
			 *  완료 콜백에서 0으로. complete_safe가 이 비트를 보고 free 안전성 판정. */

			uint8_t				domain_in_use: 1;
			/* [한국어] req payload가 외부 메모리 도메인(예: GPU/RDMA-pinned) 사용 중.
			 *  설정자: req_init이 req->payload.opts->memory_domain 검사.
			 *  읽는 자: try_memory_translation이 시스템 도메인으로 변환 + invalidate_data 호출. */

			uint8_t				reserved : 2;
			/* [한국어] 패딩 — 8비트 맞춤. */
		} bits;
	} ordering;

	struct nvme_tcp_pdu			*pdu;
	/* [한국어] 이 req에 영구 할당된 PDU 슬롯. send_pdus[cid]를 가리킴.
	 *  CapsuleCmd / H2CData / 수신 시 임시 사용. */

	struct iovec				iov[NVME_TCP_MAX_SGL_DESCRIPTORS];
	/* [한국어] 송수신 데이터 SGL 매핑 결과. build_contig/sgl_request에서 채움.
	 *  PDU의 data_iov에 다시 복사되어 spdk_sock_writev_async가 사용. */

	uint32_t				iovcnt;
	/* [한국어] 위 iov[]의 유효 entry 수. */

	/* Used to hold a value received from subsequent R2T while we are still
	 * waiting for H2C ack */
	uint32_t				r2tl_remain_next;
	/* [한국어] ttag_r2t_next와 짝. 다음 R2T의 r2tl 임시 보관. */

	struct nvme_tcp_qpair			*tqpair;
	/* [한국어] 소속 큐페어 back-pointer. alloc_reqs에서 채움. 콜백/매크로의 컨텍스트 복원에 사용. */

	TAILQ_ENTRY(nvme_tcp_req)		link;
	/* [한국어] free_reqs 또는 outstanding_reqs 리스트 link entry. */

	struct spdk_nvme_cpl			rsp;
	/* [한국어] 수신된 NVMe Completion (CapsuleResp.rccqe 또는 직접 채운 status).
	 *  req_complete가 호출자에게 전달. */

	uint8_t					rsvd1[32];
	/* [한국어] 패딩 — sizeof(struct) % SPDK_CACHE_LINE_SIZE == 0 (false sharing 방지) 보장.
	 *  static_assert로 컴파일 타임 검증. */
};
SPDK_STATIC_ASSERT(sizeof(struct nvme_tcp_req) % SPDK_CACHE_LINE_SIZE == 0, "unaligned size");

/* [한국어] poll group이 없는 standalone qpair가 통계를 무해히 카운트할 수 있도록 한 더미.
 *  poll_group_remove에서 tqpair->stats를 이 더미로 옮겨 dangling 포인터 방지. */
static struct spdk_nvme_tcp_stat g_dummy_stats = {};

/* [한국어] 전방 선언 — 아래 함수들이 호출 순환 또는 코드 순서상 정의 위치보다 먼저 참조되므로 시그니처만 미리 노출.
 *  - nvme_tcp_send_h2c_data: R2T 응답 H2C Data PDU 송신.
 *  - nvme_tcp_poll_group_process_completions: poll group의 핵심 polling 진입점.
 *  - nvme_tcp_icresp_handle: ICResp 수신 후처리.
 *  - nvme_tcp_req_complete: 완료된 tcp_req를 호출자에게 콜백 전달 + free. */
static void nvme_tcp_send_h2c_data(struct nvme_tcp_req *tcp_req);
static int64_t nvme_tcp_poll_group_process_completions(struct spdk_nvme_transport_poll_group
		*tgroup, uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
static void nvme_tcp_icresp_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu);
static void nvme_tcp_req_complete(struct nvme_tcp_req *tcp_req, struct nvme_tcp_qpair *tqpair,
				  struct spdk_nvme_cpl *rsp, bool print_on_error);

/*
 * [한국어]
 * nvme_tcp_qpair_state_string - qpair 상태 enum을 사람이 읽을 수 있는 문자열로 변환
 *
 * @state: nvme_tcp_qpair_state enum 값
 * @return: 정적 문자열 포인터 (절대 NULL 아님)
 *
 * 디버그/에러 로그에 상태 명을 박기 위한 헬퍼. NVME_TQPAIR_*LOG 매크로가 [%s,%s] prefix에 사용.
 * 호출 컨텍스트: 모든 컨텍스트(스레드 안전 — 정적 문자열 리터럴만 반환).
 * default 케이스의 "UNKNOWN" 반환은 미래 확장이나 메모리 corruption 검출용 안전판.
 */
static inline const char *
nvme_tcp_qpair_state_string(enum nvme_tcp_qpair_state state)
{
	switch (state) {
	case NVME_TCP_QPAIR_STATE_INVALID:
		return "INVALID";
	case NVME_TCP_QPAIR_STATE_SOCK_CONNECTING:
		return "SOCK_CONNECTING";
	case NVME_TCP_QPAIR_STATE_INITIALIZING:
		return "INITIALIZING";
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND:
		return "FABRIC_CONNECT_SEND";
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL:
		return "FABRIC_CONNECT_POLL";
	case NVME_TCP_QPAIR_STATE_AUTHENTICATING:
		return "AUTHENTICATING";
	case NVME_TCP_QPAIR_STATE_RUNNING:
		return "RUNNING";
	case NVME_TCP_QPAIR_STATE_EXITING:
		return "EXITING";
	case NVME_TCP_QPAIR_STATE_EXITED:
		return "EXITED";
	default:
		return "UNKNOWN";
	}
}

/*
 * [한국어]
 * nvme_tcp_pdu_recv_state_string - PDU 수신 상태머신 enum → 문자열
 *
 * @state: nvme_tcp_pdu_recv_state (spdk_internal/nvme_tcp.h 정의)
 * @return: 정적 문자열 포인터
 *
 * 수신 상태 흐름: AWAIT_PDU_READY → AWAIT_PDU_CH(공통 헤더) → AWAIT_PDU_PSH(PDU별 헤더)
 *   → (옵션 AWAIT_REQ/AWAIT_PDU_BUF) → AWAIT_PDU_PAYLOAD(데이터+digest) → 다시 READY.
 *   에러 시 QUIESCING → ERROR로 천이. 디버그 로그 출력에만 사용.
 */
static inline const char *
nvme_tcp_pdu_recv_state_string(enum nvme_tcp_pdu_recv_state state)
{
	switch (state) {
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
		return "AWAIT_PDU_READY";   /* [한국어] 새 PDU 시작 가능, recv_pdu 0 클리어 단계. */
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH:
		return "AWAIT_PDU_CH";       /* [한국어] common pdu hdr (8B) 수신 중. */
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH:
		return "AWAIT_PDU_PSH";      /* [한국어] PDU 종류별 specific header 수신 중. */
	case NVME_TCP_PDU_RECV_STATE_AWAIT_REQ:
		return "AWAIT_REQ";          /* [한국어] (target only) 요청 매칭 대기 — host 측에서는 거의 사용 안 함. */
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_BUF:
		return "AWAIT_PDU_BUF";      /* [한국어] (target only) 버퍼 할당 대기. */
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
		return "AWAIT_PDU_PAYLOAD";  /* [한국어] data + ddgst 수신 중. */
	case NVME_TCP_PDU_RECV_STATE_QUIESCING:
		return "QUIESCING";          /* [한국어] 종료 진입 — outstanding이 비워질 때까지 더 이상 수신 X. */
	case NVME_TCP_PDU_RECV_STATE_ERROR:
		return "ERROR";              /* [한국어] FATAL — read_pdu가 NVME_TCP_PDU_FATAL 반환. */
	default:
		return "UNKNOWN";
	}
}

/*
 * [한국어]
 * nvme_tcp_qpair - spdk_nvme_qpair* → nvme_tcp_qpair* 다운캐스트
 *
 * @qpair: 일반 qpair (trtype=TCP 보장)
 * @return: 동일 객체의 TCP 래퍼 포인터 (CONTAINEROF로 안전 변환)
 *
 * vtable 진입점이 받은 일반 qpair를 TCP 전용 확장 구조체로 변환. assert로 trtype을 검증.
 * 인라인이므로 호출 비용 0 — hot path에서도 안전.
 */
static inline struct nvme_tcp_qpair *
nvme_tcp_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->trtype == SPDK_NVME_TRANSPORT_TCP);  /* [한국어] 디버그 빌드에서만 검증 — release는 비용 0. */
	return SPDK_CONTAINEROF(qpair, struct nvme_tcp_qpair, qpair);  /* [한국어] qpair 멤버의 주소에서 부모 구조체 시작 주소 계산. */
}

/*
 * [한국어]
 * nvme_tcp_poll_group - spdk_nvme_transport_poll_group* → nvme_tcp_poll_group* 다운캐스트
 *
 * @group: 일반 poll group base
 * @return: TCP poll group 포인터
 */
static inline struct nvme_tcp_poll_group *
nvme_tcp_poll_group(struct spdk_nvme_transport_poll_group *group)
{
	return SPDK_CONTAINEROF(group, struct nvme_tcp_poll_group, group);
}

/*
 * [한국어]
 * nvme_tcp_ctrlr - spdk_nvme_ctrlr* → nvme_tcp_ctrlr* 다운캐스트
 *
 * @ctrlr: 일반 NVMe 컨트롤러 객체 (trtype=TCP 보장)
 * @return: TCP 컨트롤러 확장 포인터 (PSK 등 TCP 고유 필드 접근용)
 */
static inline struct nvme_tcp_ctrlr *
nvme_tcp_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_TCP);
	return SPDK_CONTAINEROF(ctrlr, struct nvme_tcp_ctrlr, ctrlr);
}

/*
 * [한국어]
 * nvme_tcp_req_get - free_reqs 풀에서 idle한 tcp_req 한 개를 꺼내 ACTIVE로 천이
 *
 * @tqpair: 대상 TCP 큐페어
 * @return: 사용 가능한 tcp_req 포인터, 풀이 비어 있으면 NULL (호출자는 -EAGAIN으로 위 레이어에 알림)
 *
 * 단일 호출 컨텍스트: 사용자/poll group 스레드(qpair는 한 스레드에 고정).
 * 따라서 free_reqs는 락 없이 접근 — SPDK lockless 설계. 호출자: nvme_tcp_qpair_submit_request.
 *
 * 동작:
 *   1) free_reqs head pop (LIFO 활용 — 직전에 free된 같은 cid가 캐시 hot).
 *   2) state = ACTIVE.
 *   3) 모든 tracking 필드(datao, r2tl, ordering 비트 등)를 0으로 리셋 — 이전 사용 흔적 제거.
 *   4) PDU 슬롯과 응답 슬롯도 memset 0.
 */
static struct nvme_tcp_req *
nvme_tcp_req_get(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_req *tcp_req;

	tcp_req = TAILQ_FIRST(&tqpair->free_reqs);   /* [한국어] free 풀의 첫 entry — 캐시 친화적 LIFO. */
	if (!tcp_req) {
		return NULL;                             /* [한국어] 풀 고갈 → 위 레이어가 queued_req에 보관 후 재시도. */
	}

	assert(tcp_req->state == NVME_TCP_REQ_FREE); /* [한국어] 풀에 있는 동안에는 항상 FREE여야 함 (불변식). */
	tcp_req->state = NVME_TCP_REQ_ACTIVE;        /* [한국어] outstanding으로 진입할 것이므로 ACTIVE 표시. */
	TAILQ_REMOVE(&tqpair->free_reqs, tcp_req, link);  /* [한국어] free 풀에서 떼어낸다. */
	tcp_req->datao = 0;                          /* [한국어] H2C Data 송신 진행도 0. */
	tcp_req->expected_datao = 0;                 /* [한국어] C2H Data 수신 기대 오프셋 0. */
	tcp_req->req = NULL;                         /* [한국어] 곧 nvme_tcp_req_init에서 채워짐. */
	tcp_req->in_capsule_data = false;            /* [한국어] in-capsule 여부는 req_init에서 결정. */
	tcp_req->r2tl_remain = 0;                    /* [한국어] R2T 잔여 길이 0. */
	tcp_req->r2tl_remain_next = 0;               /* [한국어] 다음 R2T용 임시 보관도 0. */
	tcp_req->active_r2ts = 0;                    /* [한국어] outstanding R2T 카운트 0. */
	tcp_req->iovcnt = 0;                         /* [한국어] iov[] 개수 0 — build_*_request가 채움. */
	tcp_req->ordering.raw = 0;                   /* [한국어] 8개 ordering 비트 일괄 리셋 — send_ack/data_recv 등. */
	memset(tcp_req->pdu, 0, sizeof(struct nvme_tcp_pdu));   /* [한국어] PDU 헤더/메타 zero — capsule_cmd_send가 채움. */
	memset(&tcp_req->rsp, 0, sizeof(struct spdk_nvme_cpl)); /* [한국어] CQE 슬롯 zero — capsule_resp 수신 시 채워짐. */

	return tcp_req;
}

/*
 * [한국어]
 * nvme_tcp_req_put - 사용 끝난 tcp_req를 free_reqs 풀로 반환 (FREE 상태로 천이)
 *
 * @tqpair: 소속 큐페어
 * @tcp_req: 반환할 req (ACTIVE 또는 ACTIVE_R2T 상태여야 함)
 *
 * 호출 컨텍스트: req_complete 안에서. INSERT_HEAD로 LIFO → 같은 cid를 곧바로 재사용 시 캐시 hit↑.
 */
static void
nvme_tcp_req_put(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	assert(tcp_req->state != NVME_TCP_REQ_FREE); /* [한국어] double-free 방지 검증. */
	tcp_req->state = NVME_TCP_REQ_FREE;          /* [한국어] FREE로 표시. */
	TAILQ_INSERT_HEAD(&tqpair->free_reqs, tcp_req, link);  /* [한국어] LIFO 스타일 push. */
}

/*
 * [한국어]
 * nvme_tcp_accel_finish_sequence - SPDK accel framework의 누적된 sequence를 실행시키고 콜백 등록
 *
 * @tgroup: poll group (accel_fn_table을 보유)
 * @treq: 진행 표시할 req (in_progress_accel 비트 1로 — 안전 free 판정에 사용)
 * @seq: append_crc32c 등으로 빌드된 시퀀스 핸들
 * @cb_fn: 시퀀스 완료 시 호출될 콜백 (예: tcp_write_pdu_seq_cb)
 * @cb_arg: 콜백 인자 (보통 PDU 또는 req 포인터)
 *
 * 컨텍스트: hot path — accel가 외부 디바이스(IAA, GPU 등) 또는 SW로 비동기 CRC32C 등 실행.
 * in_progress_accel 비트가 0이 되어야 req_complete_safe()가 통과하므로 ordering 보장 핵심.
 */
static inline void
nvme_tcp_accel_finish_sequence(struct nvme_tcp_poll_group *tgroup, struct nvme_tcp_req *treq,
			       void *seq, spdk_nvme_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_poll_group *pg = tgroup->group.group;  /* [한국어] poll group의 accel_fn_table 보유자. */

	treq->ordering.bits.in_progress_accel = 1;  /* [한국어] 완료 콜백 전까지 free 금지 마킹. */
	pg->accel_fn_table.finish_sequence(seq, cb_fn, cb_arg);  /* [한국어] accel 모듈에 시퀀스 실행 + 완료 콜백 위임. */
}

/*
 * [한국어]
 * nvme_tcp_accel_reverse_sequence - accel 시퀀스의 op 순서를 역순으로 뒤집음
 *
 * @tgroup: poll group
 * @seq: 시퀀스 핸들
 *
 * H2C(send) 시 시퀀스는 [transform → digest] 순으로 빌드되지만, C2H(recv) 시에는
 * 수신된 데이터에 대해 [digest verify → transform] 순으로 적용해야 한다. reverse가 그 일을 담당.
 */
static inline void
nvme_tcp_accel_reverse_sequence(struct nvme_tcp_poll_group *tgroup, void *seq)
{
	struct spdk_nvme_poll_group *pg = tgroup->group.group;

	pg->accel_fn_table.reverse_sequence(seq);  /* [한국어] accel 모듈이 op 큐의 진행 방향을 뒤집음. */
}

/*
 * [한국어]
 * nvme_tcp_accel_append_crc32c - accel 시퀀스에 CRC32C 계산 단계를 추가 등록
 *
 * @tgroup: poll group
 * @seq: in/out — 시퀀스 핸들 (NULL이면 새로 생성됨)
 * @dst: CRC32 결과 저장 위치 (4바이트)
 * @iovs/iovcnt: CRC 계산 대상 데이터
 * @seed: 초기 CRC32 시드 (보통 0)
 * @cb_fn/cb_arg: 이 step 완료 시 호출될 콜백 (호출자가 직접 받지 않고 finish_sequence가 일괄 처리)
 * @return: 0 성공, 음수 errno (-ENOMEM 시 비가속 fallback)
 *
 * accel framework가 이 op를 다른 op들과 묶어 한 번의 디바이스 호출로 일괄 처리 가능.
 */
static inline int
nvme_tcp_accel_append_crc32c(struct nvme_tcp_poll_group *tgroup, void **seq, uint32_t *dst,
			     struct iovec *iovs, uint32_t iovcnt, uint32_t seed,
			     spdk_nvme_accel_step_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_poll_group *pg = tgroup->group.group;

	/* [한국어] accel API 호출. NULL/NULL은 (domain, domain_ctx) — 시스템 도메인 명시. */
	return pg->accel_fn_table.append_crc32c(pg->ctx, seq, dst, iovs, iovcnt, NULL, NULL,
						seed, cb_fn, cb_arg);
}

/*
 * [한국어]
 * nvme_tcp_free_reqs - alloc_reqs로 할당된 풀 메모리 해제
 *
 * @tqpair: 대상 큐페어
 *
 * tcp_reqs: aligned_alloc → free.
 * send_pdus: spdk_zmalloc(SPDK_MALLOC_DMA, hugepage 기반) → spdk_free.
 * 호출자: nvme_tcp_ctrlr_delete_io_qpair, alloc_reqs 실패 경로.
 */
static void
nvme_tcp_free_reqs(struct nvme_tcp_qpair *tqpair)
{
	free(tqpair->tcp_reqs);                        /* [한국어] glibc free — aligned_alloc과 짝. */
	tqpair->tcp_reqs = NULL;                       /* [한국어] dangling 방지. */

	spdk_free(tqpair->send_pdus);                  /* [한국어] DPDK rte_free 위임 — hugepage 풀 반환. */
	tqpair->send_pdus = NULL;
}

/*
 * [한국어]
 * nvme_tcp_alloc_reqs - 큐페어의 tcp_req 풀 + PDU 풀 일괄 할당
 *
 * @tqpair: 대상 큐페어 (num_entries는 이미 세팅됨)
 * @return: 0 성공, -ENOMEM 실패
 *
 * 호출자: nvme_tcp_ctrlr_create_qpair → 큐페어 생성 직후 1회.
 * 컨텍스트: 컨트롤러 init 단계 (호스트 메인 스레드, 동기적).
 *
 * 메모리 레이아웃:
 *   tcp_reqs[]    : 캐시라인 정렬된 일반 메모리 (구조체 자체는 페이로드 X).
 *   send_pdus[N+2]: hugepage DMA 메모리 4KB 정렬 — sock가 zerocopy/iov 사용 시 안전.
 *                    인덱스 0..N-1: 각 tcp_req->pdu가 가리킴.
 *                    인덱스 N    : tqpair->send_pdu (ICReq, TermReq 단발용).
 *                    인덱스 N+1  : tqpair->recv_pdu (수신 stage buffer).
 *
 * 초기화 후 모든 req를 free_reqs에 넣고 cid = 인덱스로 부여.
 */
static int
nvme_tcp_alloc_reqs(struct nvme_tcp_qpair *tqpair)
{
	uint16_t i;
	struct nvme_tcp_req *tcp_req;

	/* [한국어] 캐시 라인 정렬로 false sharing 방지. SPDK_STATIC_ASSERT가 sizeof가 64의 배수임을 보장. */
	tqpair->tcp_reqs = aligned_alloc(SPDK_CACHE_LINE_SIZE,
					 tqpair->num_entries * sizeof(*tcp_req));
	if (tqpair->tcp_reqs == NULL) {
		NVME_TQPAIR_ERRLOG(tqpair, "Failed to allocate tcp_reqs\n");
		goto fail;
	}

	/* Add additional 2 member for the send_pdu, recv_pdu owned by the tqpair */
	/* [한국어] PDU는 DMA 가능 메모리(SPDK_MALLOC_DMA)에 4KB 정렬로 할당.
	 *  sock의 zerocopy 또는 io_uring DMA 사용 시 hugepage가 필수. NUMA_ID_ANY는 노드 무관. */
	tqpair->send_pdus = spdk_zmalloc((tqpair->num_entries + 2) * sizeof(struct nvme_tcp_pdu),
					 0x1000, NULL,
					 SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);

	if (tqpair->send_pdus == NULL) {
		NVME_TQPAIR_ERRLOG(tqpair, "Failed to allocate send_pdus\n");
		goto fail;
	}

	memset(tqpair->tcp_reqs, 0, tqpair->num_entries * sizeof(*tcp_req));  /* [한국어] tcp_req 영역 zero — 모든 필드/포인터 NULL. */
	TAILQ_INIT(&tqpair->send_queue);                /* [한국어] 송신 in-flight 리스트 빈 상태. */
	TAILQ_INIT(&tqpair->free_reqs);                 /* [한국어] free 풀 빈 상태로 시작 → 아래 loop가 채움. */
	TAILQ_INIT(&tqpair->outstanding_reqs);          /* [한국어] outstanding 리스트 빈 상태. */
	tqpair->qpair.queue_depth = 0;                  /* [한국어] 현재 in-flight 명령 수 0. */
	for (i = 0; i < tqpair->num_entries; i++) {
		tcp_req = &tqpair->tcp_reqs[i];
		tcp_req->cid = i;                       /* [한국어] cid = 인덱스 → get_active_req_by_cid가 O(1) lookup 가능. */
		tcp_req->tqpair = tqpair;               /* [한국어] back-pointer 채움. */
		tcp_req->pdu = &tqpair->send_pdus[i];   /* [한국어] 각 req에 전용 PDU 슬롯 1개 영구 매핑. */
		TAILQ_INSERT_TAIL(&tqpair->free_reqs, tcp_req, link);  /* [한국어] free 풀에 모두 등록. */
	}

	tqpair->send_pdu = &tqpair->send_pdus[i];      /* [한국어] 추가 슬롯 1: ICReq/TermReq 단발 송신용. */
	tqpair->recv_pdu = &tqpair->send_pdus[i + 1];  /* [한국어] 추가 슬롯 2: 수신 in-place state machine 버퍼. */

	return 0;
fail:
	nvme_tcp_free_reqs(tqpair);                    /* [한국어] 어느 단계에서 실패했든 안전하게 정리 (NULL 체크 내장). */
	return -ENOMEM;
}


/* [한국어] PDU 수신 상태머신 천이 매크로.
 *  - 동일 상태 재대입 시 ERR 로그 (잠재적 버그 표시).
 *  - ERROR 상태 진입 시 outstanding이 비어 있어야 함을 assert (in-flight 요청을 잃지 않도록).
 *  - DEBUG 로그로 천이 시점/대상 상태를 기록 → "[old,new]" 트레이싱 가능.
 *  - do-while(0) 관용구로 다중 명령 매크로의 ; 안전성 확보. */
#define nvme_tcp_qpair_set_recv_state(qpair, state) do { \
	if ((qpair)->recv_state == (state)) { \
		NVME_TQPAIR_ERRLOG(qpair, "The recv state %s is same with the state to be set\n", nvme_tcp_pdu_recv_state_string((state))); \
	} else { \
		if ((state) == NVME_TCP_PDU_RECV_STATE_ERROR) { \
			assert(TAILQ_EMPTY(&(qpair)->outstanding_reqs)); \
		} \
		NVME_TQPAIR_DEBUGLOG((qpair), "setting pdu recv state to %s\n", nvme_tcp_pdu_recv_state_string((state))); \
		(qpair)->recv_state = (state); \
	} \
} while (0)

/* [한국어] 전방 선언 — qpair_abort_reqs와 connect_qpair_poll이 disconnect 경로에서 호출됨. */
static void nvme_tcp_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr);
static int nvme_tcp_ctrlr_connect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair);

/*
 * [한국어]
 * nvme_tcp_ctrlr_disconnect_qpair - TCP 큐페어의 소켓을 닫고 outstanding 요청을 abort
 *
 * @ctrlr: 컨트롤러 (사용 안 함, vtable 시그니처상 받음)
 * @qpair: 종료 대상 qpair
 *
 * vtable 호출(.ctrlr_disconnect_qpair). 호출 트리거:
 *   - 사용자가 spdk_nvme_ctrlr_free_io_qpair / disconnect 호출.
 *   - poll group의 sock 에러 발생.
 *   - 컨트롤러 reset/timeout.
 *
 * 동작:
 *   1) needs_poll 리스트에서 제거 (만약 enqueued 상태라면).
 *   2) spdk_sock_close() — TCP FIN 송신 + 커널 소켓 해제.
 *   3) send_queue 드레인 — 아직 sock_writev_async가 처리 중인 PDU들을 모두 떨어냄.
 *   4) outstanding_reqs를 ABORTED_SQ_DELETION 상태로 일괄 abort (호출자 콜백 호출).
 *   5) async qpair는 QUIESCING 상태로 두어 process_completions가 accel completion을 끝까지 받게 함.
 *      sync qpair는 outstanding이 즉시 비어야 하므로 곧바로 disconnect_qpair_done 호출.
 */
static void
nvme_tcp_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);  /* [한국어] TCP 확장 캐스트. */
	struct nvme_tcp_pdu *pdu;
	int rc;
	struct nvme_tcp_poll_group *group;

	/* [한국어] poll group의 needs_poll 보조 리스트에 enqueue된 상태였다면 떼어낸다 — 닫힌 큐페어를 polling 시도 방지. */
	if (TAILQ_ENTRY_ENQUEUED(tqpair, link_poll)) {
		group = nvme_tcp_poll_group(qpair->poll_group);
		TAILQ_REMOVE_CLEAR(&group->needs_poll, tqpair, link_poll);
	}

	/* [한국어] 소켓 close — 내부적으로 shutdown(WR) + close(fd). 출력값은 0/음수, **value-result**로 sock=NULL 세팅. */
	rc = spdk_sock_close(&tqpair->sock);
	if (rc < 0 || tqpair->sock) {
		NVME_TQPAIR_ERRLOG(tqpair, "spdk_sock_close() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		/* Set it to NULL manually */
		tqpair->sock = NULL;  /* [한국어] close 실패 시에도 dangling 방지를 위해 강제 NULL. */
	}

	/* clear the send_queue */
	/* [한국어] 송신 in-flight 리스트 드레인 — sock가 닫혔으므로 이 PDU들의 콜백은 오지 않음.
	 *  콜백을 호출하지 않고 단순 detach (재연결 시 잘못된 PDU가 다시 송신되는 것 방지).
	 *  PDU 자체 메모리는 send_pdus 풀의 일부로 free하지 않음. */
	while (!TAILQ_EMPTY(&tqpair->send_queue)) {
		pdu = TAILQ_FIRST(&tqpair->send_queue);
		/* Remove the pdu from the send_queue to prevent the wrong sending out
		 * in the next round connection
		 */
		TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq);
	}

	/* [한국어] outstanding 요청들에 ABORTED_SQ_DELETION CQE를 채워 호출자에게 통보. */
	nvme_tcp_qpair_abort_reqs(qpair, qpair->abort_dnr);

	/* If the qpair is marked as asynchronous, let it go through the process_completions() to
	 * let any outstanding requests (e.g. those with outstanding accel operations) complete.
	 * Otherwise, there's no way of waiting for them, so tqpair->outstanding_reqs has to be
	 * empty.
	 */
	if (qpair->async) {
		/* [한국어] async: accel framework가 비동기로 끝날 수 있는 op들을 process_completions에서 마저 처리. */
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
	} else {
		assert(TAILQ_EMPTY(&tqpair->outstanding_reqs));  /* [한국어] sync 모드는 위 abort로 즉시 비어야 함. */
		nvme_transport_ctrlr_disconnect_qpair_done(qpair);  /* [한국어] 상위에 disconnect 완료 통보. */
	}
}

/*
 * [한국어]
 * nvme_tcp_ctrlr_delete_io_qpair - vtable: 큐페어 메모리 완전 해제 (disconnect 후 호출됨)
 *
 * @ctrlr: 컨트롤러
 * @qpair: 해제할 qpair
 * @return: 항상 0
 *
 * 호출 순서: disconnect_qpair → outstanding 비움 → delete_io_qpair.
 * 컨텍스트: 호스트 스레드 (보통 admin/제어 스레드).
 */
static int
nvme_tcp_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	assert(qpair != NULL);
	nvme_tcp_qpair_abort_reqs(qpair, qpair->abort_dnr);    /* [한국어] 잔여 요청을 다시 한 번 abort (방어적). */
	assert(TAILQ_EMPTY(&tqpair->outstanding_reqs));        /* [한국어] 이 시점에는 반드시 비어 있어야 함. */

	nvme_qpair_deinit(qpair);                              /* [한국어] 일반 qpair 자원 해제 (queued_req 등). */
	nvme_tcp_free_reqs(tqpair);                            /* [한국어] tcp_reqs/send_pdus 풀 해제. */
	if (!tqpair->shared_stats) {
		free(tqpair->stats);                           /* [한국어] standalone qpair만 자체 stats 소유 → free. */
	}
	free(tqpair);                                          /* [한국어] tqpair 자체 (calloc) 해제. */

	return 0;
}

/*
 * [한국어]
 * nvme_tcp_ctrlr_enable - vtable: 컨트롤러 enable 단계 후크 (TCP는 할 일 없음)
 *
 * @ctrlr: 컨트롤러
 * @return: 항상 0
 *
 * PCIe는 CC.EN 비트를 1로 설정하지만, TCP는 Fabric CONNECT 시점에 모든 협상이 끝나므로 no-op.
 */
static int
nvme_tcp_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

/*
 * [한국어]
 * nvme_tcp_ctrlr_destruct - vtable: 컨트롤러 객체 완전 파괴
 *
 * @ctrlr: 파괴 대상 (admin qpair까지 포함)
 * @return: 항상 0
 *
 * 호출자: spdk_nvme_detach 종료 경로.
 * 동작: admin qpair 해제 → 공통 destruct_finish (NS, processes, hotplug 정리) → tctrlr free.
 */
static int
nvme_tcp_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_tcp_ctrlr *tctrlr = nvme_tcp_ctrlr(ctrlr);

	if (ctrlr->adminq) {
		nvme_tcp_ctrlr_delete_io_qpair(ctrlr, ctrlr->adminq);  /* [한국어] admin qpair도 일반 io_qpair와 동일하게 해제. */
	}

	nvme_ctrlr_destruct_finish(ctrlr);                     /* [한국어] 공통 정리 (네임스페이스 객체, 프로세스 등). */

	free(tctrlr);                                          /* [한국어] TCP 확장 본체 free. */

	return 0;
}

/* If there are queued requests, we assume they are queued because they are waiting
 * for resources to be released. Those resources are almost certainly released in
 * response to a PDU completing. However, to attempt to make forward progress
 * the qpair needs to be polled and we can't rely on another network event to make
 * that happen. Add it to a list of qpairs to poll regardless of network activity.
 *
 * Besides, when tqpair state is NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL or
 * NVME_TCP_QPAIR_STATE_INITIALIZING, need to add it to needs_poll list too to make
 * forward progress in case that the resources are released after icreq's or CONNECT's
 * resp is processed. */
/*
 * [한국어]
 * nvme_tcp_cond_schedule_qpair_polling - 네트워크 이벤트 없이도 polling이 필요한 qpair를 needs_poll에 추가
 *
 * @tqpair: 대상 큐페어
 *
 * 컨텍스트: PDU send 완료 콜백, accel 완료 등에서 호출됨 (poll group 스레드).
 * 목적: forward progress 보장. 다음 두 경우에만 추가:
 *   1) queued_req가 비어있지 않음 — 이전에 자원 부족(-EAGAIN)으로 큐된 요청이 있는데,
 *      자원 release는 PDU 완료 콜백에서 일어나지만 새 네트워크 이벤트가 없으면 epoll/poll이
 *      깨어나지 않는다. needs_poll에 추가해 다음 polling 라운드에서 강제 재시도.
 *   2) 큐페어가 INITIALIZING/FABRIC_CONNECT_POLL 상태 — ICResp나 CONNECT 응답이 도착해도
 *      callback에서 끝나지 않으므로 다음 라운드에서 connect_qpair_poll을 다시 호출해야 함.
 * 이미 enqueued이거나 poll_group이 없으면 noop.
 */
static void
nvme_tcp_cond_schedule_qpair_polling(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_poll_group *pgroup;

	/* [한국어] 이미 needs_poll에 있거나 poll_group이 없으면(standalone) 할 일 없음. */
	if (TAILQ_ENTRY_ENQUEUED(tqpair, link_poll) || !tqpair->qpair.poll_group) {
		return;
	}

	/* [한국어] queued_req도 없고 connecting/initializing 상태도 아니면 굳이 추가 polling 불필요 → 빠른 리턴. */
	if (STAILQ_EMPTY(&tqpair->qpair.queued_req) &&
	    spdk_likely(tqpair->state != NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL &&
			tqpair->state != NVME_TCP_QPAIR_STATE_INITIALIZING)) {
		return;
	}

	pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
	TAILQ_INSERT_TAIL(&pgroup->needs_poll, tqpair, link_poll);  /* [한국어] poll_group_process_completions가 강제 polling 대상에 포함. */
}

/*
 * [한국어]
 * pdu_write_done - sock writev_async 완료 콜백 (커널이 송신 버퍼를 회수했을 때 호출)
 *
 * @cb_arg: PDU 포인터 (sock_req.cb_arg에 등록됨)
 * @err: 0 성공 / 음수 에러 (소켓 close 등)
 *
 * 컨텍스트: spdk_sock_group_poll 안에서 콜백 디스패치 (poll group 스레드).
 * 동작:
 *   1) needs_poll 강제 등록 (queued_req 처리 forward progress 보장).
 *   2) send_queue에서 이 PDU 제거.
 *   3) 에러면 컨트롤러를 disconnect 트리거.
 *   4) 정상이면 PDU별 cb_fn(예: nvme_tcp_qpair_cmd_send_complete) 호출 → send_ack=1 비트 세팅 등.
 */
static void
pdu_write_done(void *cb_arg, int err)
{
	struct nvme_tcp_pdu *pdu = cb_arg;
	struct nvme_tcp_qpair *tqpair = pdu->qpair;

	nvme_tcp_cond_schedule_qpair_polling(tqpair);  /* [한국어] forward progress 보장. */
	TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq); /* [한국어] in-flight 송신 리스트에서 제거. */

	if (err != 0) {
		/* [한국어] 송신 실패 → 컨트롤러 reset 트리거. 콜백은 호출하지 않음 (disconnect가 abort로 처리). */
		nvme_transport_ctrlr_disconnect_qpair(tqpair->qpair.ctrlr, &tqpair->qpair);
		return;
	}

	assert(pdu->cb_fn != NULL);                    /* [한국어] write_pdu가 반드시 cb_fn을 세팅했어야 함. */
	pdu->cb_fn(pdu->cb_arg);                       /* [한국어] PDU 종류별 후속 처리 (capsule_cmd → send_ack 등). */
}

/*
 * [한국어]
 * pdu_write_fail - PDU 송신을 시도하기 전에 실패시키는 헬퍼
 *
 * @pdu: 대상 PDU
 * @status: 에러 코드
 *
 * 보통 build_iovs 매핑 실패나 accel append 실패 등 sock writev 호출 전에 에러를 표시할 때 사용.
 * 일관된 정리 경로를 위해 send_queue에 잠시 INSERT한 뒤 pdu_write_done(err)으로 즉시 cleanup.
 */
static void
pdu_write_fail(struct nvme_tcp_pdu *pdu, int status)
{
	struct nvme_tcp_qpair *tqpair = pdu->qpair;

	/* This function is similar to pdu_write_done(), but it should be called before a PDU is
	 * sent over the socket */
	TAILQ_INSERT_TAIL(&tqpair->send_queue, pdu, tailq);  /* [한국어] pdu_write_done이 REMOVE를 호출하므로 짝을 맞춰 INSERT. */
	pdu_write_done(pdu, status);
}

/*
 * [한국어]
 * pdu_seq_fail - accel 시퀀스 실패 시 req를 INTERNAL_DEVICE_ERROR로 완료
 *
 * @pdu: 실패한 PDU (반드시 req와 연결되어 있어야 함)
 * @status: accel 에러 코드
 *
 * 컨텍스트: accel 콜백 안.
 */
static void
pdu_seq_fail(struct nvme_tcp_pdu *pdu, int status)
{
	struct nvme_tcp_req *treq = pdu->req;

	NVME_TQPAIR_ERRLOG(treq->tqpair, "Failed to execute accel sequence: %d\n", status);
	nvme_tcp_cond_schedule_qpair_polling(pdu->qpair);
	treq->rsp.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;  /* [한국어] NVMe spec status code: 내부 디바이스 오류. */
	nvme_tcp_req_complete(treq, treq->tqpair, &treq->rsp, true);  /* [한국어] req 완료 콜백 호출 + free. */
}

/*
 * [한국어]
 * _tcp_write_pdu - PDU를 실제 sock_writev_async에 위임 (digest까지 다 채워진 직전에 호출)
 *
 * @pdu: 송신할 PDU (hdr/data/digest 모두 준비 완료)
 *
 * 동작:
 *   1) nvme_tcp_build_iovs: PDU의 hdr/digest/padding/data 영역들을 iov[]로 펼친다.
 *      - 인자로 hdgst/ddgst enable 여부를 넘겨 digest iov 포함 여부 결정.
 *      - 반환값은 iov 개수 + mapped_length(out).
 *   2) send_queue tail에 INSERT (완료 콜백에서 REMOVE).
 *   3) mapped_length 검증 — data 영역 전체가 매핑되지 않으면 에러 처리.
 *   4) sock_req.cb_fn = pdu_write_done 등록.
 *   5) submitted_requests++ 통계.
 *   6) spdk_sock_writev_async — 즉시 send 하거나 send buffer 가득 시 큐잉.
 */
static void
_tcp_write_pdu(struct nvme_tcp_pdu *pdu)
{
	uint32_t mapped_length = 0;
	struct nvme_tcp_qpair *tqpair = pdu->qpair;

	/* [한국어] PDU 안의 모든 영역(헤더 + hdgst + padding + data + ddgst)을 iovec[]로 평탄화. */
	pdu->sock_req.iovcnt = nvme_tcp_build_iovs(pdu->iov, SPDK_COUNTOF(pdu->iov), pdu,
			       (bool)tqpair->flags.host_hdgst_enable, (bool)tqpair->flags.host_ddgst_enable,
			       &mapped_length);
	TAILQ_INSERT_TAIL(&tqpair->send_queue, pdu, tailq);  /* [한국어] in-flight 표시 — 완료 콜백에서 REMOVE. */
	if (spdk_unlikely(mapped_length < pdu->data_len)) {
		/* [한국어] data 영역이 SGL/iov 한도를 초과해 다 못 매핑된 비정상 케이스 (build_sgl_request 검증 미스). */
		NVME_TQPAIR_ERRLOG(tqpair, "could not map the whole %u bytes (mapped only %u bytes)\n",
				   pdu->data_len, mapped_length);
		pdu_write_done(pdu, -EINVAL);
		return;
	}
	pdu->sock_req.cb_fn = pdu_write_done;     /* [한국어] sock 완료 콜백 = pdu_write_done. */
	pdu->sock_req.cb_arg = pdu;               /* [한국어] 콜백 인자 = PDU 자체. */
	tqpair->stats->submitted_requests++;      /* [한국어] poll group 또는 자체 stats 누적. */
	spdk_sock_writev_async(tqpair->sock, &pdu->sock_req);  /* [한국어] 비동기 송신 — 즉시 또는 큐잉, 완료 시 cb_fn. */
}

/*
 * [한국어]
 * tcp_write_pdu_seq_cb - accel 시퀀스 완료 후 실제 PDU 송신을 수행하는 콜백
 *
 * @ctx: PDU 포인터
 * @status: accel 결과 (0 성공)
 *
 * 컨텍스트: accel 모듈의 완료 콜백 안 (poll group 스레드).
 * tcp_write_pdu()가 H2C 전송 + accel sequence 보유한 경우 finish_sequence를 호출하면서 이 콜백을 등록한다.
 * 시퀀스가 끝나면 in_progress_accel 비트를 클리어하고 _tcp_write_pdu로 진짜 송신 시작.
 */
static void
tcp_write_pdu_seq_cb(void *ctx, int status)
{
	struct nvme_tcp_pdu *pdu = ctx;
	struct nvme_tcp_req *treq = pdu->req;
	struct nvme_request *req = treq->req;

	assert(treq->ordering.bits.in_progress_accel);   /* [한국어] finish_sequence가 세팅했음을 검증. */
	treq->ordering.bits.in_progress_accel = 0;       /* [한국어] 클리어 → req_complete_safe 통과 가능. */

	req->accel_sequence = NULL;                      /* [한국어] 시퀀스 핸들 reset (재사용 방지). */
	if (spdk_unlikely(status != 0)) {
		pdu_seq_fail(pdu, status);               /* [한국어] accel 실패 → req를 internal error로 완료. */
		return;
	}

	_tcp_write_pdu(pdu);                             /* [한국어] 정상 → 실제 sock 송신 시작. */
}

/*
 * [한국어]
 * tcp_write_pdu - data digest CRC32 계산 후 호출되는 송신 step
 *
 * @pdu: 송신 PDU
 *
 * H2C 데이터 전송 + accel sequence 보유 케이스에서는 데이터에 transform이 적용된 후에야
 * sock에 send해야 하므로 finish_sequence + 콜백 패턴을 거친다. 그 외엔 즉시 _tcp_write_pdu.
 */
static void
tcp_write_pdu(struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *treq = pdu->req;
	struct nvme_tcp_qpair *tqpair = pdu->qpair;
	struct nvme_tcp_poll_group *tgroup;
	struct nvme_request *req;

	/* [한국어] req와 결합된 PDU(capsule_cmd, h2c_data)이고, 호스트→컨트롤러 방향이며, accel seq를 보유한 케이스만 분기. */
	if (spdk_likely(treq != NULL)) {
		req = treq->req;
		if (req->accel_sequence != NULL &&
		    spdk_nvme_opc_get_data_transfer(req->cmd.opc) == SPDK_NVME_DATA_HOST_TO_CONTROLLER &&
		    pdu->data_len > 0) {
			assert(tqpair->qpair.poll_group != NULL);  /* [한국어] accel은 poll group의 fn_table에 있음. */
			tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
			/* [한국어] 시퀀스 실행 → 완료되면 tcp_write_pdu_seq_cb가 _tcp_write_pdu 호출. */
			nvme_tcp_accel_finish_sequence(tgroup, treq, req->accel_sequence,
						       tcp_write_pdu_seq_cb, pdu);
			return;
		}
	}

	_tcp_write_pdu(pdu);  /* [한국어] accel 시퀀스 없음 → 즉시 송신. */
}

/*
 * [한국어]
 * pdu_accel_seq_compute_crc32_done - accel append_crc32c step의 결과 후처리 콜백
 *
 * @cb_arg: PDU 포인터
 *
 * accel framework가 CRC32C 계산을 마치면 pdu->data_digest_crc32에 raw 값이 들어 있다.
 * NVMe-oF TCP 스펙에 따라 ^ SPDK_CRC32C_XOR (0xFFFFFFFF)을 적용한 뒤 4바이트 little-endian으로 data_digest 영역에 기록.
 * MAKE_DIGEST_WORD는 little-endian 기록 매크로.
 *
 * 컨텍스트: accel 콜백 — 시퀀스 안에서 호출되며, finish_sequence가 끝날 때 함께 완료 처리됨.
 */
static void
pdu_accel_seq_compute_crc32_done(void *cb_arg)
{
	struct nvme_tcp_pdu *pdu = cb_arg;

	pdu->data_digest_crc32 ^= SPDK_CRC32C_XOR;            /* [한국어] CRC32C 결과의 XOR 마스크 적용 (스펙 요구). */
	MAKE_DIGEST_WORD(pdu->data_digest, pdu->data_digest_crc32);  /* [한국어] data_digest[4]에 LE로 기록. */
}

/*
 * [한국어]
 * pdu_accel_compute_crc32 - 데이터 다이제스트(DDGST)를 accel framework로 비동기 계산 시도
 *
 * @pdu: 송신 PDU (data 영역 보유)
 * @return: true = accel로 처리됨(또는 hard fail로 cleanup), false = 비가속 fallback 필요
 *
 * 제한: 큐페어 connected 이후, dif 비활성, data_len이 4바이트 정렬, poll group 보유, append_crc32c 지원.
 *
 * 동작:
 *   1) 위 제한 검사 통과 시 accel sequence에 append_crc32c step 추가.
 *   2) ENOMEM이면 false 반환 → caller가 SW fallback.
 *   3) 다른 에러는 pdu_write_fail로 hard fail (true 반환).
 *   4) 정상이면 tcp_write_pdu로 진행 (accel finish_sequence가 step들 + 송신을 마저 처리).
 */
static bool
pdu_accel_compute_crc32(struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_qpair *tqpair = pdu->qpair;
	struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
	struct nvme_request *req = ((struct nvme_tcp_req *)pdu->req)->req;
	int rc;

	/* Only support this limited case for the first step */
	/* [한국어] accel CRC 가속 적용 가능 조건: connected 이후 + dif 미사용 + 4B 정렬. */
	if (spdk_unlikely(nvme_qpair_get_state(&tqpair->qpair) < NVME_QPAIR_CONNECTED ||
			  pdu->dif_ctx != NULL ||
			  pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT != 0)) {
		return false;
	}

	/* [한국어] poll group이 없거나 accel 모듈에 CRC32C 함수가 없으면 SW fallback. */
	if (tqpair->qpair.poll_group == NULL ||
	    tgroup->group.group->accel_fn_table.append_crc32c == NULL) {
		return false;
	}

	/* [한국어] 시퀀스에 CRC32C step 추가. seq=NULL이면 새 시퀀스 생성, 아니면 append. */
	rc = nvme_tcp_accel_append_crc32c(tgroup, &req->accel_sequence,
					  &pdu->data_digest_crc32,
					  pdu->data_iov, pdu->data_iovcnt, 0,
					  pdu_accel_seq_compute_crc32_done, pdu);
	if (spdk_unlikely(rc != 0)) {
		/* If accel is out of resources, fall back to non-accelerated crc32 */
		if (rc == -ENOMEM) {
			return false;  /* [한국어] 자원 부족만은 fallback 허용. */
		}

		NVME_TQPAIR_ERRLOG(tqpair, "Failed to append crc32c operation: %d\n", rc);
		pdu_write_fail(pdu, rc);  /* [한국어] 다른 에러는 hard fail. */
		return true;              /* [한국어] caller에 "처리 완료"로 보고 (재시도 금지). */
	}

	tcp_write_pdu(pdu);  /* [한국어] CRC step만 등록한 상태로 송신 진행 — finish_sequence가 다 끝나야 _tcp_write_pdu 호출. */

	return true;
}

/*
 * [한국어]
 * pdu_compute_crc32_seq_cb - accel 시퀀스 완료 후 SW로 data digest를 계산해 채우는 fallback 콜백
 *
 * @cb_arg: PDU
 * @status: accel 결과
 *
 * accel 시퀀스에 CRC를 직접 append하지 못해(예: 비호환) 시퀀스만 먼저 실행시킨 케이스에서 사용.
 * 시퀀스가 끝나면 데이터에 transform이 적용된 상태이므로 그 결과를 SW CRC32C로 계산해 digest에 채움.
 */
static void
pdu_compute_crc32_seq_cb(void *cb_arg, int status)
{
	struct nvme_tcp_pdu *pdu = cb_arg;
	struct nvme_tcp_req *treq = pdu->req;
	struct nvme_request *req = treq->req;
	uint32_t crc32c;

	assert(treq->ordering.bits.in_progress_accel);
	treq->ordering.bits.in_progress_accel = 0;

	req->accel_sequence = NULL;
	if (spdk_unlikely(status != 0)) {
		pdu_seq_fail(pdu, status);
		return;
	}

	crc32c = nvme_tcp_pdu_calc_data_digest(pdu);   /* [한국어] SW로 CRC32C 계산 (Castagnoli, SSE4.2 가속 가능). */
	crc32c = crc32c ^ SPDK_CRC32C_XOR;             /* [한국어] 스펙 XOR 마스크 적용. */
	MAKE_DIGEST_WORD(pdu->data_digest, crc32c);    /* [한국어] LE 4B 기록. */

	_tcp_write_pdu(pdu);
}

/*
 * [한국어]
 * pdu_compute_crc32 - PDU 송신 직전에 데이터 다이제스트(DDGST) 계산
 *
 * @pdu: 송신 PDU
 *
 * 분기:
 *   1) data_len > 0 + 이 PDU 타입이 DDGST 적용 대상 (g_nvme_tcp_ddgst 표) + 호스트 ddgst enable일 때만 계산.
 *   2) accel 가속 시도 → 성공 시 즉시 return.
 *   3) accel 시퀀스가 보유 중이면 finish_sequence로 SW digest 콜백 등록.
 *   4) 일반 케이스는 SW로 즉시 CRC32C 계산해 채우고 tcp_write_pdu.
 *   5) DDGST 비대상이면 그냥 송신.
 */
static void
pdu_compute_crc32(struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_qpair *tqpair = pdu->qpair;
	struct nvme_tcp_poll_group *tgroup;
	struct nvme_request *req;
	uint32_t crc32c;

	/* Data Digest */
	if (pdu->data_len > 0 && g_nvme_tcp_ddgst[pdu->hdr.common.pdu_type] &&
	    tqpair->flags.host_ddgst_enable) {
		if (pdu_accel_compute_crc32(pdu)) {
			return;  /* [한국어] accel 처리 완료 (또는 hard fail) — 추가 동작 없음. */
		}

		/* [한국어] accel CRC append 실패했지만 시퀀스 자체는 보유 중 → 시퀀스만 실행 후 SW CRC. */
		req = ((struct nvme_tcp_req *)pdu->req)->req;
		if (req->accel_sequence != NULL) {
			tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
			nvme_tcp_accel_finish_sequence(tgroup, pdu->req, req->accel_sequence,
						       pdu_compute_crc32_seq_cb, pdu);
			return;
		}

		/* [한국어] 가장 일반적인 SW CRC 경로. */
		crc32c = nvme_tcp_pdu_calc_data_digest(pdu);
		crc32c = crc32c ^ SPDK_CRC32C_XOR;
		MAKE_DIGEST_WORD(pdu->data_digest, crc32c);
	}

	tcp_write_pdu(pdu);
}

/*
 * [한국어]
 * nvme_tcp_qpair_write_pdu - PDU 송신 진입점 (헤더 다이제스트 + 데이터 다이제스트 + sock writev 흐름의 시작)
 *
 * @tqpair: 소속 큐페어
 * @pdu: 송신 PDU (hdr.common.hlen/pdu_type은 호출자가 채워둠)
 * @cb_fn: 송신 완료 콜백 (예: nvme_tcp_qpair_cmd_send_complete)
 * @cb_arg: 콜백 인자
 *
 * 호출자: nvme_tcp_qpair_capsule_cmd_send, nvme_tcp_send_h2c_data, icreq_send, send_h2c_term_req 등.
 *
 * 흐름:
 *   1) cb_fn/cb_arg/qpair 백포인터 세팅.
 *   2) Header Digest: 이 PDU 타입이 HDGST 적용 대상이고 호스트가 enable이면 CRC32C 계산 후 hdr 끝에 4B 기록.
 *   3) pdu_compute_crc32 → pdu_compute_crc32에서 데이터 digest까지 처리 후 tcp_write_pdu → _tcp_write_pdu(sock).
 */
static void
nvme_tcp_qpair_write_pdu(struct nvme_tcp_qpair *tqpair,
			 struct nvme_tcp_pdu *pdu,
			 nvme_tcp_qpair_xfer_complete_cb cb_fn,
			 void *cb_arg)
{
	int hlen;
	uint32_t crc32c;

	hlen = pdu->hdr.common.hlen;   /* [한국어] PDU specific header 길이 (digest 미포함). */
	pdu->cb_fn = cb_fn;            /* [한국어] 송신 완료 시 호출될 콜백. */
	pdu->cb_arg = cb_arg;
	pdu->qpair = tqpair;           /* [한국어] 콜백에서 큐페어 복원용 백포인터. */

	/* Header Digest */
	/* [한국어] g_nvme_tcp_hdgst[pdu_type]가 1이면 이 PDU 타입에 HDGST 적용 가능 (스펙 표).
	 *  IC_REQ/IC_RESP는 negotiation 전이라 HDGST 비적용; CapsuleCmd, H2C/C2H Data, Resp, R2T는 적용. */
	if (g_nvme_tcp_hdgst[pdu->hdr.common.pdu_type] && tqpair->flags.host_hdgst_enable) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);   /* [한국어] hdr.raw 영역에 대한 CRC32C. */
		MAKE_DIGEST_WORD((uint8_t *)&pdu->hdr.raw[hlen], crc32c);  /* [한국어] hlen 직후 4B에 LE 기록. */
	}

	pdu_compute_crc32(pdu);  /* [한국어] data digest 계산 → tcp_write_pdu → sock 송신. */
}

/*
 * [한국어]
 * nvme_tcp_try_memory_translation - 외부 메모리 도메인 → 시스템 도메인 변환 (DMA 가능 가상 주소로 매핑)
 *
 * @tcp_req: 변환 컨텍스트 (domain_in_use 비트가 1이어야 동작)
 * @addr: in/out — 입력은 외부 도메인 가상 주소, 출력은 시스템 도메인 주소
 * @length: 길이
 * @return: 0 성공, -EFAULT 변환 실패
 *
 * 외부 도메인 예시: GPU 메모리, RDMA-pinned, custom memory pool 등.
 * spdk_memory_domain_translate_data가 dst 도메인 = system으로 1:1 매핑 시도.
 * iov_count != 1이면 fragment된 매핑 — TCP에서는 단일 연속 매핑만 지원.
 *
 * domain_in_use가 0이면 (보통 케이스) 즉시 0 반환 — 변환 불필요.
 */
static int
nvme_tcp_try_memory_translation(struct nvme_tcp_req *tcp_req, void **addr, uint32_t length)
{
	struct nvme_request *req = tcp_req->req;
	struct spdk_memory_domain_translation_result translation = {
		.iov_count = 0,
		.size = sizeof(translation)  /* [한국어] ABI 진화 대응 — caller가 아는 크기 명시. */
	};
	int rc;

	if (!tcp_req->ordering.bits.domain_in_use) {
		return 0;  /* [한국어] 일반 호스트 메모리 → 변환 불필요. */
	}

	/* [한국어] 외부 도메인에서 시스템 도메인으로 1:1 매핑 요청. */
	rc = spdk_memory_domain_translate_data(req->payload.opts->memory_domain,
					       req->payload.opts->memory_domain_ctx, spdk_memory_domain_get_system_domain(), NULL, *addr, length,
					       &translation);
	if (spdk_unlikely(rc || translation.iov_count != 1)) {
		NVME_TQPAIR_ERRLOG(tcp_req->tqpair, "DMA memory translation failed, rc %d, iov_count %u\n", rc,
				   translation.iov_count);
		return -EFAULT;
	}

	assert(length == translation.iov.iov_len);  /* [한국어] 길이 보존 보장. */
	*addr = translation.iov.iov_base;           /* [한국어] 변환된 시스템 도메인 주소로 교체. */
	return 0;
}

/*
 * Build SGL describing contiguous payload buffer.
 */
/*
 * [한국어]
 * nvme_tcp_build_contig_request - 연속(contiguous) 버퍼 payload를 단일 iov로 변환
 *
 * @tqpair: 큐페어 (로깅용)
 * @tcp_req: 채울 req — iov[0], iovcnt 세팅
 * @return: 0 성공, 음수 실패
 *
 * SPDK NVMe API에서 사용자 데이터는 두 종류 — CONTIG(단일 buffer + offset) 또는 SGL(콜백 기반).
 * 이 함수는 CONTIG 케이스. iov 1개로 단순화.
 *
 * 호출자: nvme_tcp_req_init (송신 시), nvme_tcp_c2h_data_hdr_handle (수신 시 buffer 매핑).
 */
static int
nvme_tcp_build_contig_request(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = tcp_req->req;

	/* ubsan complains about applying zero offset to null pointer if contig_or_cb_arg is NULL,
	 * so just double cast it to make it go away */
	/* [한국어] base + offset = 시작 주소. uintptr_t 캐스트로 NULL+0 UB 회피. */
	void *addr = (void *)((uintptr_t)req->payload.contig_or_cb_arg + req->payload_offset);
	size_t length = req->payload_size;
	int rc;

	NVME_TQPAIR_DEBUGLOG(tqpair, "enter\n");

	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);  /* [한국어] CONTIG 전용 검증. */
	rc = nvme_tcp_try_memory_translation(tcp_req, &addr, length);  /* [한국어] 외부 도메인이면 시스템으로 변환. */
	if (spdk_unlikely(rc)) {
		return rc;
	}

	tcp_req->iov[0].iov_base = addr;       /* [한국어] 단일 iov에 전체 영역 매핑. */
	tcp_req->iov[0].iov_len = length;
	tcp_req->iovcnt = 1;
	return 0;
}

/*
 * Build SGL describing scattered payload buffer.
 */
/*
 * [한국어]
 * nvme_tcp_build_sgl_request - 분산(scatter) 버퍼 payload를 iov[]로 변환 (next_sge_fn 콜백 호출)
 *
 * @tqpair: 큐페어 (로깅용)
 * @tcp_req: 채울 req — iov[]/iovcnt 세팅
 * @return: 0 성공, -1 실패
 *
 * SPDK SGL 페이로드 모델: payload.contig_or_cb_arg = 사용자 컨텍스트, payload.{reset_sgl_fn,next_sge_fn} = 콜백.
 *   reset: offset에서부터 iteration 재시작.
 *   next:  다음 (addr, length) 반환.
 *
 * 동작:
 *   1) reset_sgl_fn 호출 (payload_offset부터 시작).
 *   2) max_num_sgl = min(ctrlr.max_sges, NVME_TCP_MAX_SGL_DESCRIPTORS) — TCP/디바이스 한도.
 *   3) next_sge_fn을 반복 호출하며 iov[iovcnt]에 저장. 각 영역에 대해 memory translation 적용.
 *   4) remaining_size가 0이 되면 종료. 안 되면 max iov 초과로 실패.
 */
static int
nvme_tcp_build_sgl_request(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	int rc;
	uint32_t length, remaining_size, iovcnt = 0, max_num_sgl;
	struct nvme_request *req = tcp_req->req;

	NVME_TQPAIR_DEBUGLOG(tqpair, "enter\n");

	assert(req->payload_size != 0);                                 /* [한국어] 빈 payload는 SGL 불필요 (caller 책임). */
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);                      /* [한국어] SGL 모드면 콜백 필수. */
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);  /* [한국어] iteration 시작점 세팅. */

	max_num_sgl = spdk_min(req->qpair->ctrlr->max_sges, NVME_TCP_MAX_SGL_DESCRIPTORS);  /* [한국어] iov 상한. */
	remaining_size = req->payload_size;

	do {
		void *addr;

		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &addr, &length);  /* [한국어] 사용자가 다음 fragment 반환. */
		if (rc) {
			return -1;
		}

		rc = nvme_tcp_try_memory_translation(tcp_req, &addr, length);  /* [한국어] 외부 도메인 → 시스템 변환. */
		if (spdk_unlikely(rc)) {
			return rc;
		}

		length = spdk_min(length, remaining_size);   /* [한국어] payload_size를 초과한 마지막 fragment는 cap. */
		tcp_req->iov[iovcnt].iov_base = addr;
		tcp_req->iov[iovcnt].iov_len = length;
		remaining_size -= length;
		iovcnt++;
	} while (remaining_size > 0 && iovcnt < max_num_sgl);


	/* Should be impossible if we did our sgl checks properly up the stack, but do a sanity check here. */
	/* [한국어] iov가 다 찼는데 데이터가 남았다 → 사용자 SGL 검증 미스. */
	if (remaining_size > 0) {
		NVME_TQPAIR_ERRLOG(tqpair, "Failed to construct tcp_req=%p, and the iovcnt=%u, remaining_size=%u\n",
				   tcp_req, iovcnt, remaining_size);
		return -1;
	}

	tcp_req->iovcnt = iovcnt;  /* [한국어] 결과 iov 개수 저장. */

	return 0;
}

/*
 * [한국어]
 * nvme_tcp_req_init - 사용자 nvme_request를 tcp_req에 결합하고 NVMe SGL/in-capsule 결정
 *
 * @tqpair: 큐페어
 * @req: 사용자 NVMe 요청 (cmd.opc, payload, cb_fn 등 채워진 상태)
 * @tcp_req: 빈 TCP req (req_get으로 가져옴)
 * @return: 0 성공, 음수 실패
 *
 * 호출 컨텍스트: nvme_tcp_qpair_submit_request 안 (사용자 스레드 = 큐페어 소속 스레드).
 *
 * 핵심 결정:
 *   1) req->cmd.cid = tcp_req->cid (free 풀 인덱스).
 *   2) NVMe SGL descriptor 세팅:
 *      - psdt = PSDT_SGL_MPTR_CONTIG (NVMe 1.x — SGL 사용 표시).
 *      - 디폴트 type = TRANSPORT_DATA_BLOCK + subtype = TRANSPORT (out-of-capsule, target이 R2T로 가져감).
 *      - in-capsule이 결정되면 type = DATA_BLOCK + subtype = OFFSET + address = 0 (PDU 끝에 inline).
 *   3) data 방향 분기:
 *      - C2H(read): iov 매핑은 데이터 도착 후로 미룸 (c2h_data_hdr_handle).
 *      - H2C(write)/no-data: iov를 즉시 빌드.
 *      - Fabric capsule cmd: opc 대신 fctype으로 transfer direction 판단.
 *   4) H2C이고 payload_size ≤ ioccsz(IO Queue Command Capsule Size, 컨트롤러가 지원하는 in-capsule 크기) →
 *      in_capsule_data = true. Fabric/admin은 항상 8192B까지 허용.
 */
static int
nvme_tcp_req_init(struct nvme_tcp_qpair *tqpair, struct nvme_request *req,
		  struct nvme_tcp_req *tcp_req)
{
	struct spdk_nvme_ctrlr *ctrlr = tqpair->qpair.ctrlr;
	int rc = 0;
	enum spdk_nvme_data_transfer xfer;
	uint32_t max_in_capsule_data_size;

	tcp_req->req = req;  /* [한국어] 양방향 결합. */
	/* [한국어] 외부 메모리 도메인 사용 여부 캐싱 — translation/invalidate에서 hot 경로 분기. */
	tcp_req->ordering.bits.domain_in_use = (req->payload.opts && req->payload.opts->memory_domain);

	req->cmd.cid = tcp_req->cid;                                              /* [한국어] NVMe Command Identifier. */
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;                          /* [한국어] PRP 대신 SGL 사용 표시. */
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK;  /* [한국어] 디폴트: out-of-capsule. */
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_TRANSPORT;
	req->cmd.dptr.sgl1.unkeyed.length = req->payload_size;                    /* [한국어] 데이터 총 길이. */

	if (spdk_unlikely(req->cmd.opc == SPDK_NVME_OPC_FABRIC)) {
		/* [한국어] Fabric capsule (CONNECT/AUTH/Property R/W) — opc는 모두 같고 fctype으로 구분. */
		struct spdk_nvmf_capsule_cmd *nvmf_cmd = (struct spdk_nvmf_capsule_cmd *)&req->cmd;

		xfer = spdk_nvme_opc_get_data_transfer(nvmf_cmd->fctype);
	} else {
		xfer = spdk_nvme_opc_get_data_transfer(req->cmd.opc);  /* [한국어] 일반 NVMe 명령 — opc로 방향 판단. */
	}

	/* For c2h delay filling in the iov until the data arrives.
	 * For h2c some delay is also possible if data doesn't fit into cmd capsule (not implemented). */
	/* [한국어] read는 데이터 수신 시점에 iov를 채우므로 여기서는 빌드 X.
	 *  write/no-data만 미리 iov 빌드. */
	if (nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG) {
		if (xfer != SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
			rc = nvme_tcp_build_contig_request(tqpair, tcp_req);
		}
	} else if (nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL) {
		if (xfer != SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
			rc = nvme_tcp_build_sgl_request(tqpair, tcp_req);
		}
	} else {
		rc = -1;  /* [한국어] 알 수 없는 payload 타입. */
	}

	if (rc) {
		return rc;
	}

	if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		max_in_capsule_data_size = ctrlr->ioccsz_bytes;  /* [한국어] Identify 응답에서 협상된 ioccsz × 16 (스펙). */
		if (spdk_unlikely((req->cmd.opc == SPDK_NVME_OPC_FABRIC) ||
				  nvme_qpair_is_admin_queue(&tqpair->qpair))) {
			/* [한국어] Fabric/Admin은 spec상 항상 8192B까지 in-capsule 허용 (ioccsz 무관). */
			max_in_capsule_data_size = SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE;
		}

		if (req->payload_size <= max_in_capsule_data_size) {
			/* [한국어] 작은 write → CapsuleCmd PDU에 inline data 첨부 (R2T 왕복 제거 → latency↓). */
			req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
			req->cmd.dptr.sgl1.address = 0;       /* [한국어] capsule 내 데이터 시작 오프셋 (=PDO에서 시작). */
			tcp_req->in_capsule_data = true;
		}
	}

	return 0;
}

/*
 * [한국어]
 * nvme_tcp_req_complete_safe - 모든 비동기 이벤트가 끝났는지 확인 후 안전하게 완료
 *
 * @tcp_req: 검사할 req
 * @return: true = 완료시켰음, false = 아직 대기할 이벤트 있음
 *
 * 완료 조건 (3가지 모두 충족):
 *   - send_ack: 마지막 송신 PDU의 sock writev 완료.
 *   - data_recv: CapsuleResp 또는 LAST_PDU C2H Data 수신.
 *   - !in_progress_accel: accel 시퀀스 진행 중 아님.
 *
 * 셋 다 1이 되어야 호스트가 안전하게 buffer를 free하고 호출자에게 콜백할 수 있다.
 * 이벤트 순서가 다양(read는 data 먼저/resp 나중, write는 send_ack 먼저/resp 나중 등)하므로
 * 각 이벤트 완료 콜백이 모두 이 함수를 호출해 마지막 이벤트가 완료를 트리거.
 */
static inline bool
nvme_tcp_req_complete_safe(struct nvme_tcp_req *tcp_req)
{
	if (!(tcp_req->ordering.bits.send_ack && tcp_req->ordering.bits.data_recv &&
	      !tcp_req->ordering.bits.in_progress_accel)) {
		return false;  /* [한국어] 셋 중 하나라도 미충족 → 호출자는 다음 이벤트 콜백을 대기. */
	}

	assert(tcp_req->state == NVME_TCP_REQ_ACTIVE);
	assert(tcp_req->tqpair != NULL);
	assert(tcp_req->req != NULL);

	nvme_tcp_req_complete(tcp_req, tcp_req->tqpair, &tcp_req->rsp, true);  /* [한국어] CQE 콜백 + req free. */
	return true;
}

/*
 * [한국어]
 * nvme_tcp_qpair_cmd_send_complete - CapsuleCmd PDU의 송신 완료 콜백
 *
 * @cb_arg: tcp_req 포인터
 *
 * pdu_write_done에서 호출됨 (sock writev 완료 시점).
 * 동작 분기:
 *   1) send_ack 비트 1로 세팅.
 *   2) h2c_send_waiting_ack가 1 → R2T가 도착했지만 직전 send를 기다리는 중이었음 → 즉시 H2C Data 송신.
 *   3) in-capsule + 외부 도메인 → 송신 끝났으므로 메모리 도메인에 invalidate 통보 (GPU 캐시 등 정리).
 *   4) req_complete_safe로 다른 조건도 다 만족하면 완료.
 */
static void
nvme_tcp_qpair_cmd_send_complete(void *cb_arg)
{
	struct nvme_tcp_req *tcp_req = cb_arg;

	NVME_TQPAIR_DEBUGLOG(tcp_req->tqpair, "tcp req %p, cid %u\n", tcp_req, tcp_req->cid);
	tcp_req->ordering.bits.send_ack = 1;  /* [한국어] 커널이 송신 버퍼 회수했음 — buffer 재사용 안전. */
	/* Handle the r2t case */
	if (spdk_unlikely(tcp_req->ordering.bits.h2c_send_waiting_ack)) {
		/* [한국어] R2T가 미리 도착해 H2C 송신을 미뤘던 케이스 → 이제 send_ack 받았으니 시작. */
		NVME_TQPAIR_DEBUGLOG(tcp_req->tqpair, "tcp req %p, send H2C data\n", tcp_req);
		nvme_tcp_send_h2c_data(tcp_req);
	} else {
		if (tcp_req->in_capsule_data && tcp_req->ordering.bits.domain_in_use) {
			/* [한국어] inline 데이터를 외부 메모리에서 송신했으므로, 메모리 도메인의 캐시 일관성 유지를 위해 invalidate. */
			spdk_memory_domain_invalidate_data(tcp_req->req->payload.opts->memory_domain,
							   tcp_req->req->payload.opts->memory_domain_ctx, tcp_req->iov, tcp_req->iovcnt);
		}

		nvme_tcp_req_complete_safe(tcp_req);  /* [한국어] data_recv도 이미 1이면 즉시 완료. */
	}
}

/*
 * [한국어]
 * nvme_tcp_qpair_capsule_cmd_send - CapsuleCmd PDU 빌드 및 송신
 *
 * @tqpair: 큐페어
 * @tcp_req: 송신할 요청
 *
 * NVMe-oF TCP의 핵심 메시지: 사용자 NVMe SQE를 capsule_cmd PDU로 감싸 전송.
 * PDU 레이아웃 (왼쪽이 wire 위 앞쪽):
 *   [common_pdu_hdr (8B)] [capsule_cmd_specific (NVMe SQE 64B)] [HDGST (4B,옵션)]
 *   [padding (cpda 정렬)]
 *   [in-capsule data (payload_size B, 옵션)]
 *   [DDGST (4B,옵션)]
 *
 * 변수:
 *   plen = total PDU length, hlen = header length (=sizeof(capsule_cmd)).
 *   pdo = PDU Data Offset (data 시작 바이트 위치).
 *   alignment = (cpda+1)<<2 (Controller PDU Data Alignment).
 *
 * in_capsule_data가 false거나 payload=0이면 헤더 + (옵션) HDGST만으로 종료.
 */
static void
nvme_tcp_qpair_capsule_cmd_send(struct nvme_tcp_qpair *tqpair,
				struct nvme_tcp_req *tcp_req)
{
	struct nvme_tcp_pdu *pdu;
	struct spdk_nvme_tcp_cmd *capsule_cmd;
	uint32_t plen = 0, alignment;
	uint8_t pdo;

	NVME_TQPAIR_DEBUGLOG(tqpair, "enter\n");
	pdu = tcp_req->pdu;            /* [한국어] req에 영구 할당된 PDU 슬롯. */
	pdu->req = tcp_req;            /* [한국어] PDU → req back-pointer (콜백에서 복원). */

	capsule_cmd = &pdu->hdr.capsule_cmd;
	capsule_cmd->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;  /* [한국어] PDU 종류 마킹. */
	plen = capsule_cmd->common.hlen = sizeof(*capsule_cmd);             /* [한국어] hlen = capsule cmd 헤더 크기 (8+64). */
	capsule_cmd->ccsqe = tcp_req->req->cmd;                              /* [한국어] NVMe SQE 64B 통째 복사. */

	NVME_TQPAIR_DEBUGLOG(tqpair, "capsule_cmd cid=%u\n", tcp_req->req->cmd.cid);

	if (tqpair->flags.host_hdgst_enable) {
		NVME_TQPAIR_DEBUGLOG(tqpair, "Header digest is enabled for capsule command on tcp_req=%p\n",
				     tcp_req);
		capsule_cmd->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;  /* [한국어] PDU 플래그에 HDGST 표시. */
		plen += SPDK_NVME_TCP_DIGEST_LEN;                            /* [한국어] +4B HDGST. */
	}

	if ((tcp_req->req->payload_size == 0) || !tcp_req->in_capsule_data) {
		goto end;  /* [한국어] 데이터 없음 또는 out-of-capsule → R2T로 받을 예정. */
	}

	pdo = plen;                    /* [한국어] 데이터 시작 위치 = 현재까지 hdr+digest 길이. */
	pdu->padding_len = 0;
	if (tqpair->cpda) {
		alignment = (tqpair->cpda + 1) << 2;  /* [한국어] CPDA: 데이터 시작점 정렬 단위 (4B의 배수). */
		if (alignment > plen) {
			/* [한국어] 정렬 위치까지 padding 삽입. */
			pdu->padding_len = alignment - plen;
			pdo = alignment;
			plen = alignment;
		}
	}

	capsule_cmd->common.pdo = pdo;            /* [한국어] PDU Data Offset 필드 기록. */
	plen += tcp_req->req->payload_size;       /* [한국어] +payload 데이터 길이. */
	if (tqpair->flags.host_ddgst_enable) {
		capsule_cmd->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;  /* [한국어] +4B DDGST. */
	}

	tcp_req->datao = 0;                       /* [한국어] in-capsule data offset 시작. */
	/* [한국어] iov[]를 PDU의 data_iov로 설정 — build_iovs가 wire에 펼쳐 보낼 영역. */
	nvme_tcp_pdu_set_data_buf(pdu, tcp_req->iov, tcp_req->iovcnt,
				  0, tcp_req->req->payload_size);
end:
	capsule_cmd->common.plen = plen;          /* [한국어] 최종 PDU total length 기록 (수신 측이 이 값으로 경계 인식). */
	/* [한국어] write_pdu → digest 계산 → sock writev_async → pdu_write_done → cmd_send_complete. */
	nvme_tcp_qpair_write_pdu(tqpair, pdu, nvme_tcp_qpair_cmd_send_complete, tcp_req);
}

/*
 * [한국어]
 * nvme_tcp_qpair_submit_request - vtable: 사용자 NVMe 요청을 TCP로 송출하는 진입점
 *
 * @qpair: 사용자 큐페어
 * @req: NVMe 요청 객체
 * @return: 0 성공, -EAGAIN free 풀 고갈 (위 레이어가 재시도), -1 init 실패
 *
 * 호출자: spdk_nvme_qpair_submit_request → nvme_transport_qpair_submit_request → 이 함수.
 * 컨텍스트: 사용자 스레드 (큐페어 소속 SPDK 스레드 = lockless).
 *
 * 동작:
 *   1) free 풀에서 tcp_req 한 개 확보. 없으면 -EAGAIN — 위 레이어가 queued_req에 보관.
 *   2) req_init: SGL 빌드 + in_capsule 결정.
 *   3) queue_depth++ + spdk_trace_record (SUBMIT 트레이스 포인트).
 *   4) outstanding_reqs tail에 INSERT.
 *   5) timeout 콜백 등록되어 있으면 timeout_enabled 리스트에도 추가 (poll group이 매 라운드 검사).
 *   6) capsule_cmd_send로 PDU 발사.
 */
static int
nvme_tcp_qpair_submit_request(struct spdk_nvme_qpair *qpair,
			      struct nvme_request *req)
{
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_req *tcp_req;

	tqpair = nvme_tcp_qpair(qpair);
	assert(tqpair != NULL);
	assert(req != NULL);

	tcp_req = nvme_tcp_req_get(tqpair);
	if (!tcp_req) {
		tqpair->stats->queued_requests++;          /* [한국어] backpressure 카운터 — RPC stats으로 노출. */
		/* Inform the upper layer to try again later. */
		return -EAGAIN;                            /* [한국어] 위 레이어가 queued_req에 enqueue 후 재시도. */
	}

	if (spdk_unlikely(nvme_tcp_req_init(tqpair, req, tcp_req))) {
		NVME_TQPAIR_ERRLOG(tqpair, "nvme_tcp_req_init() failed\n");
		nvme_tcp_req_put(tqpair, tcp_req);          /* [한국어] init 실패 → free 풀로 즉시 반환. */
		return -1;
	}

	tqpair->qpair.queue_depth++;                       /* [한국어] in-flight 카운트 (qd 측정용). */
	/* [한국어] SUBMIT 트레이스 — spdk_trace_viewer에서 SUBMIT~COMPLETE 라이프타임을 시각화 가능. */
	spdk_trace_record(TRACE_NVME_TCP_SUBMIT, qpair->id, 0, (uintptr_t)tcp_req->pdu, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)req->cmd.opc,
			  req->cmd.cdw10, req->cmd.cdw11, req->cmd.cdw12, tqpair->qpair.queue_depth);
	TAILQ_INSERT_TAIL(&tqpair->outstanding_reqs, tcp_req, link);  /* [한국어] outstanding 등록 (timeout/abort 대상). */

	/* [한국어] 타임아웃 추적 옵션이 켜져 있고 아직 timeout_enabled에 없는 큐페어면 등록. */
	if (TAILQ_ENTRY_NOT_ENQUEUED(tqpair, link_timeout) && qpair->poll_group != NULL &&
	    qpair->ctrlr->timeout_enabled) {
		struct nvme_tcp_poll_group *tgroup;

		tgroup = nvme_tcp_poll_group(qpair->poll_group);
		TAILQ_INSERT_TAIL(&tgroup->timeout_enabled, tqpair, link_timeout);
	}

	nvme_tcp_qpair_capsule_cmd_send(tqpair, tcp_req);  /* [한국어] CapsuleCmd PDU 빌드 + 송신. */
	return 0;
}

/*
 * [한국어]
 * nvme_tcp_qpair_reset - vtable: TCP qpair는 reset할 게 없음 (PCIe와 달리 doorbell 없음)
 *
 * @qpair: 큐페어
 * @return: 항상 0
 */
static int
nvme_tcp_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

/*
 * [한국어]
 * nvme_tcp_req_complete - 완료된 tcp_req를 호출자에게 통보 + free 풀로 반환
 *
 * @tcp_req: 완료할 req
 * @tqpair: 소속 큐페어
 * @rsp: NVMe Completion 객체 (CapsuleResp.rccqe 또는 직접 채운 abort/error)
 * @print_on_error: 에러 시 sql/cpl 디버그 print 여부 (abort_aers는 false로 호출)
 *
 * 컨텍스트: poll group 또는 standalone qpair의 process_completions 안 (콜백 deferred 가능).
 *
 * 동작:
 *   1) async_complete++ (in_completion_context가 아닌 컨텍스트면 process_completions 반환값에 합산).
 *   2) cpl 캐시 (req_put 후 tcp_req->rsp가 zero되므로).
 *   3) 에러면 sqe/cpl 콘솔 출력 (옵션).
 *   4) queue_depth--, COMPLETE 트레이스.
 *   5) outstanding에서 remove. 비었으면 timeout_enabled에서도 제거.
 *   6) tcp_req를 free 풀로 반환.
 *   7) nvme_complete_request → req->cb_fn(cb_arg, &cpl) 호출 → 사용자에게 통보.
 */
static void
nvme_tcp_req_complete(struct nvme_tcp_req *tcp_req,
		      struct nvme_tcp_qpair *tqpair,
		      struct spdk_nvme_cpl *rsp,
		      bool print_on_error)
{
	struct spdk_nvme_cpl	cpl;
	struct spdk_nvme_qpair	*qpair;
	struct nvme_request	*req;
	bool			print_error;

	assert(tcp_req->req != NULL);
	req = tcp_req->req;
	qpair = req->qpair;

	NVME_TQPAIR_DEBUGLOG(tqpair, "complete tcp_req(%p)\n", tcp_req);

	if (!qpair->in_completion_context) {
		/* [한국어] 사용자가 process_completions 안에서 직접 호출한 게 아닌 비동기 컨텍스트 → 카운터에 합산. */
		tqpair->async_complete++;
	}

	/* Cache arguments to be passed to nvme_complete_request since tcp_req can be zeroed when released */
	memcpy(&cpl, rsp, sizeof(cpl));     /* [한국어] tcp_req 해제 후에도 cpl 사본을 사용자에게 전달. */

	if (spdk_unlikely(spdk_nvme_cpl_is_error(rsp))) {
		print_error = print_on_error && !qpair->ctrlr->opts.disable_error_logging;

		if (print_error) {
			spdk_nvme_qpair_print_command(qpair, &req->cmd);  /* [한국어] 실패 SQE 덤프. */
		}

		if (print_error || SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
			spdk_nvme_qpair_print_completion(qpair, rsp);     /* [한국어] CQE 덤프. */
		}
	}

	qpair->queue_depth--;             /* [한국어] in-flight 카운트 감소. */
	/* [한국어] COMPLETE 트레이스 — SUBMIT과 매칭되어 라이프타임 측정. */
	spdk_trace_record(TRACE_NVME_TCP_COMPLETE, qpair->id, 0, (uintptr_t)tcp_req->pdu, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)cpl.status_raw, qpair->queue_depth);
	TAILQ_REMOVE(&tqpair->outstanding_reqs, tcp_req, link);

	if (TAILQ_EMPTY(&tqpair->outstanding_reqs) && qpair->poll_group != NULL &&
	    TAILQ_ENTRY_ENQUEUED(tqpair, link_timeout)) {
		/* [한국어] 마지막 outstanding 완료 → timeout 검사 대상에서 빼서 polling 비용 절감. */
		struct nvme_tcp_poll_group *tgroup;

		assert(qpair->ctrlr->timeout_enabled);

		tgroup = nvme_tcp_poll_group(qpair->poll_group);
		TAILQ_REMOVE_CLEAR(&tgroup->timeout_enabled, tqpair, link_timeout);
	}

	nvme_tcp_req_put(tqpair, tcp_req);                                  /* [한국어] free 풀로 반환 (재사용 가능). */
	nvme_complete_request(req->cb_fn, req->cb_arg, req->qpair, req, &cpl);  /* [한국어] 사용자 콜백 호출 진입점. */
}

/*
 * [한국어]
 * nvme_tcp_qpair_abort_reqs - vtable: 큐페어의 모든 outstanding 요청을 ABORTED_SQ_DELETION으로 완료
 *
 * @qpair: 큐페어
 * @dnr: Do Not Retry 비트 (1이면 호출자가 retry 안 하도록)
 *
 * 호출자: disconnect_qpair, delete_io_qpair, ctrlr fail 경로.
 * 동작: outstanding_reqs를 순회하며 통일된 abort CQE로 일괄 완료.
 *   - in_progress_accel인 req는 건너뜀 (accel 콜백이 free까지 마무리해야 안전).
 */
static void
nvme_tcp_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_tcp_req *tcp_req, *tmp;
	struct spdk_nvme_cpl cpl = {};
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	/* [한국어] 통일된 abort CQE 빌드 — NVMe spec status code "Aborted - Submission Queue Deletion". */
	cpl.sqid = qpair->id;
	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = dnr;

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		/* We cannot abort requests with accel operations in progress */
		if (tcp_req->ordering.bits.in_progress_accel) {
			continue;  /* [한국어] accel 콜백이 끝나기를 기다림 — 콜백이 req_complete_safe 호출. */
		}

		nvme_tcp_req_complete(tcp_req, tqpair, &cpl, true);  /* [한국어] abort 완료 통보. */
	}
}

/*
 * [한국어]
 * nvme_tcp_qpair_send_h2c_term_req_complete - H2C TermReq 송신 완료 콜백 → 큐페어 EXITING 상태로
 *
 * @cb_arg: tqpair 포인터
 *
 * TermReq 발사 후 wire flush 완료가 확인되면 큐페어를 EXITING으로 전이.
 * disconnect_qpair가 후속 정리 진행.
 */
static void
nvme_tcp_qpair_send_h2c_term_req_complete(void *cb_arg)
{
	struct nvme_tcp_qpair *tqpair = cb_arg;

	nvme_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_EXITING);
}

/*
 * [한국어]
 * nvme_tcp_qpair_send_h2c_term_req - 프로토콜 위반 시 H2C TermReq PDU로 컨트롤러에 종료 통보
 *
 * @tqpair: 큐페어
 * @pdu: 위반을 일으킨 수신 PDU (디버깅용 hdr.raw 일부를 TermReq에 첨부)
 * @fes: Fatal Error Status (INVALID_HEADER_FIELD, PDU_SEQUENCE_ERROR 등)
 * @error_offset: 어떤 필드 오프셋이 잘못됐는지 (FES가 INVALID_HEADER_FIELD/UNSUPPORTED_PARAMETER일 때만 의미)
 *
 * NVMe-oF TCP 스펙: 호스트가 컨트롤러에서 잘못된 PDU를 받으면 H2C TermReq를 보내고 연결을 정리한다.
 * 이 PDU는 hdr + 위반 PDU의 hdr 일부 (최대 SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE)를 포함.
 *
 * 동작 후 recv_state = QUIESCING으로 두고 송신만 끝내면 EXITING.
 */
static void
nvme_tcp_qpair_send_h2c_term_req(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
				 enum spdk_nvme_tcp_term_req_fes fes, uint32_t error_offset)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req;
	uint32_t h2c_term_req_hdr_len = sizeof(*h2c_term_req);
	uint8_t copy_len;

	rsp_pdu = tqpair->send_pdu;                       /* [한국어] tcp_req와 무관한 단발 PDU 슬롯 사용. */
	memset(rsp_pdu, 0, sizeof(*rsp_pdu));             /* [한국어] 깨끗한 상태로 시작. */
	h2c_term_req = &rsp_pdu->hdr.term_req;
	h2c_term_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ;  /* [한국어] PDU 종류 = H2C_TERM_REQ. */
	h2c_term_req->common.hlen = h2c_term_req_hdr_len;

	/* [한국어] FEI(Fatal Error Information): 위반된 필드의 PDU 시작에서의 byte offset. */
	if ((fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		DSET32(&h2c_term_req->fei, error_offset);  /* [한국어] LE 32-bit 기록 매크로. */
	}

	copy_len = pdu->hdr.common.hlen;
	if (copy_len > SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE) {
		copy_len = SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE;  /* [한국어] payload 한도 (스펙 152B). */
	}

	/* Copy the error info into the buffer */
	/* [한국어] 위반된 PDU의 헤더 raw를 term_req PDU 뒤에 붙여 디버깅 정보로 송신. */
	memcpy((uint8_t *)rsp_pdu->hdr.raw + h2c_term_req_hdr_len, pdu->hdr.raw, copy_len);
	nvme_tcp_pdu_set_data(rsp_pdu, (uint8_t *)rsp_pdu->hdr.raw + h2c_term_req_hdr_len, copy_len);

	/* Contain the header len of the wrong received pdu */
	h2c_term_req->common.plen = h2c_term_req->common.hlen + copy_len;
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);  /* [한국어] 더 이상 수신 처리 X. */
	nvme_tcp_qpair_write_pdu(tqpair, rsp_pdu, nvme_tcp_qpair_send_h2c_term_req_complete, tqpair);
}

/*
 * [한국어]
 * nvme_tcp_qpair_recv_state_valid - 일반 PDU 수신 가능 상태인지 검사
 *
 * @tqpair: 큐페어
 * @return: true = ICReq/ICResp 핸드셰이크 끝난 상태, false = 아직 negotiation 미완 또는 종료 중
 *
 * 호출자: nvme_tcp_pdu_ch_handle — IC_RESP가 아닌 일반 PDU를 받으려면 connection이 negotiated여야 함.
 */
static bool
nvme_tcp_qpair_recv_state_valid(struct nvme_tcp_qpair *tqpair)
{
	switch (tqpair->state) {
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND:
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL:
	case NVME_TCP_QPAIR_STATE_AUTHENTICATING:
	case NVME_TCP_QPAIR_STATE_RUNNING:
		return true;     /* [한국어] negotiation 완료 → 모든 일반 PDU 수신 가능. */
	default:
		return false;    /* [한국어] SOCK_CONNECTING/INITIALIZING/EXITING — IC_RESP만 허용 또는 무시. */
	}
}

/*
 * [한국어]
 * nvme_tcp_pdu_ch_handle - 공통 PDU 헤더(8B) 수신 직후 검증 + PSH 길이 결정
 *
 * @tqpair: 큐페어
 *
 * 호출 컨텍스트: read_pdu 상태머신의 AWAIT_PDU_CH 단계에서 8바이트 ch가 다 오면 호출.
 *
 * 검증 항목:
 *   - PDU 종류별 expected_hlen (헤더 길이)이 올바른지.
 *   - HDGST 플래그가 켜져 있으면 plen에 +4B (digest) 반영되어야 함.
 *   - C2H Data: plen >= pdo (정렬 padding 영역까지 확보).
 *   - C2H TermReq: plen이 hdr_len 초과 + spec 한도 이내.
 *
 * 검증 실패 시 H2C TermReq 송신.
 * 통과 시 recv_state를 AWAIT_PDU_PSH로 천이 + psh_len 계산.
 */
static void
nvme_tcp_pdu_ch_handle(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *pdu;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	uint32_t expected_hlen, hd_len = 0;
	bool plen_error = false;

	pdu = tqpair->recv_pdu;

	NVME_TQPAIR_DEBUGLOG(tqpair, "pdu type = %d\n", pdu->hdr.common.pdu_type);
	if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP) {
		/* [한국어] IC_RESP는 negotiation 직전 단계 — 다른 어떤 PDU보다도 먼저 수신해야 함. */
		if (tqpair->flags.icresp_received) {
			/* [한국어] 두 번째 IC_RESP — 프로토콜 위반 (한 connection에 한 번만 허용). */
			NVME_TQPAIR_ERRLOG(tqpair,
					   "Already received IC_RESP PDU, and we should reject this pdu=%p\n", pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}
		expected_hlen = sizeof(struct spdk_nvme_tcp_ic_resp);
		if (pdu->hdr.common.plen != expected_hlen) {
			plen_error = true;  /* [한국어] IC_RESP는 데이터/digest가 없어 plen=hlen 정확히. */
		}
	} else {
		if (spdk_unlikely(!nvme_tcp_qpair_recv_state_valid(tqpair))) {
			/* [한국어] negotiation 전인데 일반 PDU가 도착 → 비정상. */
			NVME_TQPAIR_ERRLOG(tqpair, "The TCP/IP tqpair connection is not negotiated\n");
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}

		switch (pdu->hdr.common.pdu_type) {
		case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP:
			/* [한국어] CapsuleResp = NVMe CQE 캡슐. 데이터 없음. */
			expected_hlen = sizeof(struct spdk_nvme_tcp_rsp);
			if (pdu->hdr.common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF) {
				hd_len = SPDK_NVME_TCP_DIGEST_LEN;  /* [한국어] HDGST 켜져 있으면 +4B. */
			}

			if (pdu->hdr.common.plen != (expected_hlen + hd_len)) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
			/* [한국어] C2H Data = read 응답. data 영역 존재. */
			expected_hlen = sizeof(struct spdk_nvme_tcp_c2h_data_hdr);
			if (pdu->hdr.common.plen < pdu->hdr.common.pdo) {
				plen_error = true;  /* [한국어] data 시작점이 PDU 길이보다 뒤면 불가능. */
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
			/* [한국어] C2H TermReq = 컨트롤러가 호스트 동작에 위반을 감지해 종료 요청. */
			expected_hlen = sizeof(struct spdk_nvme_tcp_term_req_hdr);
			if ((pdu->hdr.common.plen <= expected_hlen) ||
			    (pdu->hdr.common.plen > SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE)) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_R2T:
			/* [한국어] R2T = 컨트롤러가 호스트에 H2C Data 요청. 데이터 없음. */
			expected_hlen = sizeof(struct spdk_nvme_tcp_r2t_hdr);
			if (pdu->hdr.common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF) {
				hd_len = SPDK_NVME_TCP_DIGEST_LEN;
			}

			if (pdu->hdr.common.plen != (expected_hlen + hd_len)) {
				plen_error = true;
			}
			break;

		default:
			/* [한국어] 알 수 없는 PDU 종류 — 호스트는 ICReq/CapsuleCmd/H2CData/H2CTermReq만 송신,
			 *  컨트롤러는 ICResp/CapsuleResp/C2HData/C2HTermReq/R2T만 송신. 그 외는 모두 위반. */
			NVME_TQPAIR_ERRLOG(tqpair, "Unexpected PDU type 0x%02x\n", tqpair->recv_pdu->hdr.common.pdu_type);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdu_type);
			goto err;
		}
	}

	if (pdu->hdr.common.hlen != expected_hlen) {
		/* [한국어] 헤더 길이가 PDU 종류와 불일치. */
		NVME_TQPAIR_ERRLOG(tqpair, "Expected PDU header length %u, got %u\n", expected_hlen,
				   pdu->hdr.common.hlen);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, hlen);
		goto err;

	} else if (plen_error) {
		/* [한국어] plen 검증 실패. */
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, plen);
		goto err;
	} else {
		/* [한국어] 검증 통과 → PSH 단계로. psh_len = hlen - sizeof(common) + (옵션) HDGST. */
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
		nvme_tcp_pdu_calc_psh_len(tqpair->recv_pdu, tqpair->flags.host_hdgst_enable);
		return;
	}
err:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);  /* [한국어] 위반 → TermReq + 종료 진입. */
}

/*
 * [한국어]
 * get_nvme_active_req_by_cid - cid → tcp_req 역참조 (O(1) lookup)
 *
 * @tqpair: 큐페어
 * @cid: NVMe Command Identifier (PDU의 cccid 필드에서 옴)
 * @return: 해당 active req 또는 NULL (범위 초과 또는 FREE 상태)
 *
 * tcp_reqs[]가 cid를 인덱스로 직접 매핑되도록 alloc_reqs에서 셋업했기 때문에 O(1).
 * 호출자: capsule_resp/c2h_data/r2t의 hdr_handle 함수들 — 수신된 PDU의 cccid로 원본 req를 찾기.
 */
static struct nvme_tcp_req *
get_nvme_active_req_by_cid(struct nvme_tcp_qpair *tqpair, uint32_t cid)
{
	assert(tqpair != NULL);
	if ((cid >= tqpair->num_entries) || (tqpair->tcp_reqs[cid].state == NVME_TCP_REQ_FREE)) {
		return NULL;  /* [한국어] 잘못된 cid 또는 이미 완료된 req → 비정상 PDU. */
	}

	return &tqpair->tcp_reqs[cid];
}

/*
 * [한국어]
 * nvme_tcp_recv_payload_seq_cb - C2H 데이터 수신 후 accel reverse 시퀀스 완료 콜백
 *
 * @cb_arg: tcp_req 포인터
 * @status: accel 결과
 *
 * read 응답 데이터에 대해 (digest verify → user transform) 순으로 reverse_sequence 적용 후 완료.
 * 시퀀스가 끝나면 in_progress_accel 클리어 → req_complete_safe로 사용자 콜백 호출.
 */
static void
nvme_tcp_recv_payload_seq_cb(void *cb_arg, int status)
{
	struct nvme_tcp_req *treq = cb_arg;
	struct nvme_request *req = treq->req;
	struct nvme_tcp_qpair *tqpair = treq->tqpair;

	assert(treq->ordering.bits.in_progress_accel);
	treq->ordering.bits.in_progress_accel = 0;

	nvme_tcp_cond_schedule_qpair_polling(tqpair);

	req->accel_sequence = NULL;
	if (spdk_unlikely(status != 0)) {
		pdu_seq_fail(treq->pdu, status);
		return;
	}

	nvme_tcp_req_complete_safe(treq);  /* [한국어] send_ack/data_recv도 1이면 완료. */
}

/*
 * [한국어]
 * nvme_tcp_c2h_data_payload_handle - C2H Data PDU의 payload 수신 완료 후처리
 *
 * @tqpair: 큐페어
 * @pdu: 수신 완료된 C2H Data PDU
 * @reaped: in/out — 누적 완료 카운트 (process_completions 반환값)
 *
 * C2H Data 분할 전송 동안 매 PDU마다 호출됨. 마지막 PDU(LAST_PDU 플래그)에서만 완료 처리.
 *
 * SUCCESS 플래그: 컨트롤러가 별도의 CapsuleResp를 보내지 않고 마지막 데이터 PDU에 inline으로 완료 표시.
 *   → 호스트는 CapsuleResp 대기 없이 즉시 완료 처리 가능 (latency 절감).
 *
 * accel sequence 보유 시: reverse 후 finish — 데이터에 대해 (digest verify → user transform) 순으로 적용.
 */
static void
nvme_tcp_c2h_data_payload_handle(struct nvme_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu, uint32_t *reaped)
{
	struct nvme_tcp_req *tcp_req;
	struct nvme_tcp_poll_group *tgroup;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data;
	uint8_t flags;

	tcp_req = pdu->req;
	assert(tcp_req != NULL);  /* [한국어] c2h_data_hdr_handle에서 미리 채웠어야 함. */

	NVME_TQPAIR_DEBUGLOG(tqpair, "enter\n");
	c2h_data = &pdu->hdr.c2h_data;
	tcp_req->datao += pdu->data_len;  /* [한국어] 누적 수신 데이터 오프셋 갱신. */
	flags = c2h_data->common.flags;

	if (flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU) {
		/* [한국어] phase tag (status.p): NVMe spec — 일반적으로 컨트롤러 phase 토글에 사용되지만
		 *  여기서는 partial payload 표시로 사용 (datao != payload_size면 partial 완료). */
		if (tcp_req->datao == tcp_req->req->payload_size) {
			tcp_req->rsp.status.p = 0;
		} else {
			tcp_req->rsp.status.p = 1;
		}

		tcp_req->rsp.cid = tcp_req->cid;        /* [한국어] CQE에 cid 기록. */
		tcp_req->rsp.sqid = tqpair->qpair.id;   /* [한국어] CQE에 sqid 기록. */
		if (flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) {
			/* [한국어] inline success — CapsuleResp 없이 완료 처리 가능. */
			tcp_req->ordering.bits.data_recv = 1;
			if (tcp_req->req->accel_sequence != NULL) {
				tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
				nvme_tcp_accel_reverse_sequence(tgroup, tcp_req->req->accel_sequence);  /* [한국어] recv는 op 역순 적용. */
				nvme_tcp_accel_finish_sequence(tgroup, tcp_req,
							       tcp_req->req->accel_sequence,
							       nvme_tcp_recv_payload_seq_cb,
							       tcp_req);
				return;  /* [한국어] 콜백에서 마저 완료 처리. */
			}

			if (nvme_tcp_req_complete_safe(tcp_req)) {
				(*reaped)++;  /* [한국어] 완료 1건 카운트. */
			}
		}
		/* [한국어] SUCCESS 플래그가 없으면 별도 CapsuleResp가 도착할 때까지 대기. */
	}
}

/* [한국어] FES(Fatal Error Status) 코드 → 문자열 매핑 테이블.
 *  NVMe-oF TCP 스펙의 표준 fatal error 종류. C2H TermReq 디버깅 출력에 사용. */
static const char *spdk_nvme_tcp_term_req_fes_str[] = {
	"Invalid PDU Header Field",            /* [한국어] 0: 헤더 필드 값 잘못. */
	"PDU Sequence Error",                  /* [한국어] 1: PDU 순서 위반 (예: ICReq 전 다른 PDU). */
	"Header Digest Error",                 /* [한국어] 2: HDGST CRC32C 불일치. */
	"Data Transfer Out of Range",          /* [한국어] 3: datao+datal이 payload_size 초과. */
	"Data Transfer Limit Exceeded",        /* [한국어] 4: outstanding R2T가 maxr2t 초과 등. */
	"Unsupported parameter",               /* [한국어] 5: 알 수 없는 파라미터. */
};

/*
 * [한국어]
 * nvme_tcp_c2h_term_req_dump - 컨트롤러로부터 받은 C2H TermReq의 에러 정보를 로그로 출력
 *
 * @c2h_term_req: 수신된 TermReq 헤더
 *
 * 컨트롤러가 호스트의 위반을 감지해 종료 요청을 보낸 경우, 디버깅을 위해 위반 종류와
 * 위반 필드 오프셋을 콘솔에 출력. 보통 ERR 레벨 — 사용자가 보고하기 위함.
 */
static void
nvme_tcp_c2h_term_req_dump(struct spdk_nvme_tcp_term_req_hdr *c2h_term_req)
{
	SPDK_ERRLOG("Error info of pdu(%p): %s\n", c2h_term_req,
		    spdk_nvme_tcp_term_req_fes_str[c2h_term_req->fes]);
	if ((c2h_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (c2h_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		SPDK_DEBUGLOG(nvme, "The offset from the start of the PDU header is %u\n",
			      DGET32(c2h_term_req->fei));  /* [한국어] FEI 필드를 LE 32-bit로 읽어 출력. */
	}
	/* we may also need to dump some other info here */
}

/*
 * [한국어]
 * nvme_tcp_c2h_term_req_payload_handle - C2H TermReq의 payload 수신 후 connection 종료 진입
 *
 * @tqpair: 큐페어
 * @pdu: TermReq PDU
 *
 * payload는 위반된 PDU의 hdr 일부 — dump로 디버깅. 그 후 recv_state = QUIESCING으로
 * 천이해 더 이상 새 PDU 처리하지 않도록 하고 종료 단계 진입.
 */
static void
nvme_tcp_c2h_term_req_payload_handle(struct nvme_tcp_qpair *tqpair,
				     struct nvme_tcp_pdu *pdu)
{
	nvme_tcp_c2h_term_req_dump(&pdu->hdr.term_req);
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
}

/*
 * [한국어]
 * _nvme_tcp_pdu_payload_handle - payload 수신 완료 시 PDU 종류별 분기
 *
 * @tqpair: 큐페어
 * @reaped: in/out — 누적 완료 카운트
 *
 * payload를 수신할 PDU 종류는 두 가지뿐:
 *   - C2H_DATA: 사용자 데이터 → 후처리 후 다시 READY로 (다음 PDU 수신).
 *   - C2H_TERM_REQ: 위반 정보 → QUIESCING (state는 함수 내에서 세팅).
 */
static void
_nvme_tcp_pdu_payload_handle(struct nvme_tcp_qpair *tqpair, uint32_t *reaped)
{
	struct nvme_tcp_pdu *pdu;

	assert(tqpair != NULL);
	pdu = tqpair->recv_pdu;

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
		nvme_tcp_c2h_data_payload_handle(tqpair, pdu, reaped);
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);  /* [한국어] 다음 PDU 수신 준비. */
		break;

	case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
		nvme_tcp_c2h_term_req_payload_handle(tqpair, pdu);  /* [한국어] state 안에서 QUIESCING으로 세팅됨. */
		break;

	default:
		/* The code should not go to here */
		/* [한국어] payload를 가지지 않는 PDU(CapsuleResp/R2T/IC_RESP)는 hdr_handle에서 끝나야 함. */
		NVME_TQPAIR_ERRLOG(tqpair, "The code should not go to here\n");
		break;
	}
}

/*
 * [한국어]
 * nvme_tcp_req_copy_pdu - recv_pdu의 데이터를 req 전용 PDU 슬롯으로 복사 (accel async 처리용)
 *
 * @treq: 대상 req
 * @pdu: 원본 PDU (큐페어의 recv_pdu — 다음 수신을 위해 곧 재사용됨)
 *
 * recv_pdu는 큐페어당 1개 — accel sequence가 비동기로 끝날 때까지 보존하려면 req 전용 슬롯으로 복사 필수.
 * hdr/data_digest/data_iov 등 accel가 참조할 모든 영역을 복사.
 */
static void
nvme_tcp_req_copy_pdu(struct nvme_tcp_req *treq, struct nvme_tcp_pdu *pdu)
{
	treq->pdu->hdr = pdu->hdr;                                                              /* [한국어] PDU 헤더 통째 복사. */
	treq->pdu->req = treq;                                                                  /* [한국어] back-pointer. */
	memcpy(treq->pdu->data_digest, pdu->data_digest, sizeof(pdu->data_digest));             /* [한국어] DDGST 4B 복사. */
	memcpy(treq->pdu->data_iov, pdu->data_iov, sizeof(pdu->data_iov[0]) * pdu->data_iovcnt); /* [한국어] iov[] 복사. */
	treq->pdu->data_iovcnt = pdu->data_iovcnt;
	treq->pdu->data_len = pdu->data_len;
}

/*
 * [한국어]
 * nvme_tcp_accel_seq_recv_compute_crc32_done - C2H 데이터 수신 후 accel CRC32C 결과 검증 콜백
 *
 * @cb_arg: tcp_req
 *
 * accel가 수신 데이터에 대해 CRC32C를 계산해 data_digest_crc32에 채움. 이를 PDU에 함께 도착한
 * 컨트롤러 측 DDGST(data_digest)와 비교. 불일치 시 NVMe transient transport error로 표시.
 */
static void
nvme_tcp_accel_seq_recv_compute_crc32_done(void *cb_arg)
{
	struct nvme_tcp_req *treq = cb_arg;
	struct nvme_tcp_qpair *tqpair = treq->tqpair;
	struct nvme_tcp_pdu *pdu = treq->pdu;
	bool result;

	pdu->data_digest_crc32 ^= SPDK_CRC32C_XOR;                              /* [한국어] CRC32C 결과 XOR. */
	result = MATCH_DIGEST_WORD(pdu->data_digest, pdu->data_digest_crc32);   /* [한국어] 4B LE 비교. */
	if (spdk_unlikely(!result)) {
		NVME_TQPAIR_ERRLOG(tqpair, "data digest error\n");
		treq->rsp.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;  /* [한국어] 사용자에게 transient transport error 보고. */
	}
}

/*
 * [한국어]
 * nvme_tcp_accel_recv_compute_crc32 - C2H 데이터 수신 후 DDGST 검증을 accel framework로 시도
 *
 * @treq: 대상 req
 * @pdu: 수신된 PDU (recv_pdu)
 * @return: true = accel 처리됨, false = SW fallback 필요
 *
 * 제한: 단일 C2H PDU로 전체 페이로드가 도착하는 단순 케이스만 지원 (data_len == payload_size).
 * 그 외(분할 전송, dif 컨텍스트, 비정렬)는 SW로 처리.
 *
 * 동작:
 *   1) 제약 검사 → 미충족 시 false.
 *   2) recv_pdu → req 전용 PDU 슬롯 복사 (accel async 처리 동안 보존).
 *   3) accel sequence에 CRC32C step 추가.
 *   4) recv_state = READY 천이 + payload_handle 즉시 호출 (accel가 끝나야 진짜 완료, 아니면 in_progress 비트로 보호).
 */
static bool
nvme_tcp_accel_recv_compute_crc32(struct nvme_tcp_req *treq, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_qpair *tqpair = treq->tqpair;
	struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
	struct nvme_request *req = treq->req;
	int rc, dummy = 0;

	/* Only support this limited case that the request has only one c2h pdu */
	/* [한국어] 가속 적용 조건: connected, poll_group 존재, dif 미사용, 4B 정렬, 단일 PDU 페이로드. */
	if (spdk_unlikely(nvme_qpair_get_state(&tqpair->qpair) < NVME_QPAIR_CONNECTED ||
			  tqpair->qpair.poll_group == NULL || pdu->dif_ctx != NULL ||
			  pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT != 0 ||
			  pdu->data_len != req->payload_size)) {
		return false;
	}

	if (tgroup->group.group->accel_fn_table.append_crc32c == NULL) {
		return false;  /* [한국어] accel 모듈에 CRC32C 함수 없음. */
	}

	nvme_tcp_req_copy_pdu(treq, pdu);  /* [한국어] recv_pdu는 곧 다음 PDU에 재사용 → req 전용 슬롯에 보존. */
	rc = nvme_tcp_accel_append_crc32c(tgroup, &req->accel_sequence,
					  &treq->pdu->data_digest_crc32,
					  treq->pdu->data_iov, treq->pdu->data_iovcnt, 0,
					  nvme_tcp_accel_seq_recv_compute_crc32_done, treq);
	if (spdk_unlikely(rc != 0)) {
		/* If accel is out of resources, fall back to non-accelerated crc32 */
		if (rc == -ENOMEM) {
			return false;  /* [한국어] 자원 부족 → SW fallback. */
		}

		NVME_TQPAIR_ERRLOG(tqpair, "Failed to append crc32c operation: %d\n", rc);
		treq->rsp.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;
	}

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);  /* [한국어] 다음 PDU 수신 준비. */
	nvme_tcp_c2h_data_payload_handle(tqpair, treq->pdu, &dummy);  /* [한국어] payload 후처리 — accel가 끝나야 사용자 콜백 호출. */

	return true;
}

/*
 * [한국어]
 * nvme_tcp_pdu_payload_handle - PDU 데이터 + DDGST 수신 완료 시 진입점
 *
 * @tqpair: 큐페어
 * @reaped: in/out 완료 카운트
 *
 * read_pdu 상태머신의 AWAIT_PDU_PAYLOAD 단계에서 모든 데이터 + (옵션) DDGST가 도착하면 호출.
 *
 * 동작:
 *   1) tcp_req->expected_datao 업데이트 (다음 C2H Data의 datao 검증용).
 *   2) DDGST 검증:
 *      a) accel framework 시도 → 성공 시 즉시 return (accel 콜백이 마무리).
 *      b) 실패/비가속 → SW CRC32C 계산 후 비교. 불일치 시 transient error 표시.
 *   3) _nvme_tcp_pdu_payload_handle로 PDU 종류별 후처리 디스패치.
 */
static void
nvme_tcp_pdu_payload_handle(struct nvme_tcp_qpair *tqpair,
			    uint32_t *reaped)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu = tqpair->recv_pdu;
	uint32_t crc32c;
	struct nvme_tcp_req *tcp_req = pdu->req;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	NVME_TQPAIR_DEBUGLOG(tqpair, "enter\n");

	/* The request can be NULL, e.g. in case of C2HTermReq */
	if (spdk_likely(tcp_req != NULL)) {
		tcp_req->expected_datao += pdu->data_len;  /* [한국어] 다음 C2H Data의 datao가 이 값과 일치해야 함. */
	}

	/* check data digest if need */
	if (pdu->ddgst_enable) {
		/* But if the data digest is enabled, tcp_req cannot be NULL */
		assert(tcp_req != NULL);   /* [한국어] DDGST가 있는 PDU는 모두 tcp_req와 결합됨. */
		if (nvme_tcp_accel_recv_compute_crc32(tcp_req, pdu)) {
			return;            /* [한국어] accel가 처리 — 콜백에서 검증 + 후처리. */
		}

		/* [한국어] SW CRC32C 검증 경로. */
		crc32c = nvme_tcp_pdu_calc_data_digest(pdu);
		crc32c = crc32c ^ SPDK_CRC32C_XOR;
		rc = MATCH_DIGEST_WORD(pdu->data_digest, crc32c);
		if (rc == 0) {
			NVME_TQPAIR_ERRLOG(tqpair, "data digest error with pdu=%p\n", pdu);
			tcp_req = pdu->req;
			assert(tcp_req != NULL);
			tcp_req->rsp.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;  /* [한국어] DDGST 실패 = NVMe transient transport error. */
		}
	}

	_nvme_tcp_pdu_payload_handle(tqpair, reaped);
}

/*
 * [한국어]
 * nvme_tcp_send_icreq_complete - ICReq 송신 완료 콜백
 *
 * @cb_arg: tqpair
 *
 * ICReq의 sock writev_async 완료 시점. icreq_send_ack 비트를 1로 세팅.
 * 이미 ICResp를 받은 상태였으면(icresp_received=1) 핸드셰이크 완료 → FABRIC_CONNECT_SEND 상태로.
 *
 * 두 이벤트(send_ack vs resp_recv)는 임의 순서로 발생 가능 (네트워크 latency 따라).
 */
static void
nvme_tcp_send_icreq_complete(void *cb_arg)
{
	struct nvme_tcp_qpair *tqpair = cb_arg;

	NVME_TQPAIR_DEBUGLOG(tqpair, "Complete the icreq send\n");

	tqpair->flags.icreq_send_ack = true;
	if (tqpair->flags.icresp_received) {
		NVME_TQPAIR_DEBUGLOG(tqpair, "finalize icresp\n");
		/* [한국어] 송신 완료 + 응답 수신 완료 → 트랜스포트 협상 종료. 다음은 Fabric CONNECT. */
		nvme_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND);
	}
}

/*
 * [한국어]
 * nvme_tcp_icresp_handle - ICResp PDU 수신 처리 (트랜스포트 협상 결과 저장)
 *
 * @tqpair: 큐페어
 * @pdu: 수신된 ICResp PDU
 *
 * NVMe-oF TCP 핸드셰이크의 마지막 단계. ICResp는 컨트롤러가 호스트의 ICReq에 응답하면서
 * 다음 항목들을 협상해서 보낸다:
 *   - PFV (PDU Format Version): 현재 0만 정의됨.
 *   - maxh2cdata: 호스트가 한 번에 보낼 수 있는 H2C Data PDU의 최대 데이터 크기.
 *   - cpda (Controller PDU Data Alignment): 호스트→컨트롤러 PDU의 data 시작점 정렬 요구.
 *   - dgst.bits.hdgst_enable, ddgst_enable: 호스트가 요청한 옵션 중 컨트롤러가 받아들인 결과.
 *
 * 검증 후 tqpair에 저장. 검증 실패 시 H2C TermReq.
 *
 * 마지막 단계: socket recv buffer 크기를 협상된 ddgst 옵션 기반으로 재조정.
 *   recv_buf_size = (4KB + sizeof(c2h_data_hdr) + digests) * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR.
 *   여러 4K read 명령 응답을 한 번에 흡수할 수 있도록.
 */
static void
nvme_tcp_icresp_handle(struct nvme_tcp_qpair *tqpair,
		       struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_ic_resp *ic_resp = &pdu->hdr.ic_resp;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	int recv_buf_size;

	/* Only PFV 0 is defined currently */
	if (ic_resp->pfv != 0) {
		/* [한국어] 현재 NVMe-oF TCP 스펙은 PFV 0만 정의 — 미래 확장 대비 검증. */
		NVME_TQPAIR_ERRLOG(tqpair, "Expected ICResp PFV %u, got %u\n", 0u, ic_resp->pfv);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, pfv);
		goto end;
	}

	if (ic_resp->maxh2cdata < NVME_TCP_PDU_H2C_MIN_DATA_SIZE) {
		/* [한국어] 스펙 권장 최소 4096B 미만이면 협상 거부. */
		NVME_TQPAIR_ERRLOG(tqpair, "Expected ICResp maxh2cdata >=%u, got %u\n",
				   NVME_TCP_PDU_H2C_MIN_DATA_SIZE,
				   ic_resp->maxh2cdata);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, maxh2cdata);
		goto end;
	}
	tqpair->maxh2cdata = ic_resp->maxh2cdata;  /* [한국어] 저장 — send_h2c_data가 이 값을 청크 크기로 사용. */

	if (ic_resp->cpda > SPDK_NVME_TCP_CPDA_MAX) {
		/* [한국어] CPDA 최대값 초과 — 비현실적인 정렬 요구는 거부. */
		NVME_TQPAIR_ERRLOG(tqpair, "Expected ICResp cpda <=%u, got %u\n", SPDK_NVME_TCP_CPDA_MAX,
				   ic_resp->cpda);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, cpda);
		goto end;
	}
	tqpair->cpda = ic_resp->cpda;  /* [한국어] 저장 — capsule_cmd_send/send_h2c_data가 padding 계산에 사용. */

	/* [한국어] 호스트가 요청한 digest 옵션 중 컨트롤러가 받아들인 결과. */
	tqpair->flags.host_hdgst_enable = ic_resp->dgst.bits.hdgst_enable ? true : false;
	tqpair->flags.host_ddgst_enable = ic_resp->dgst.bits.ddgst_enable ? true : false;
	NVME_TQPAIR_DEBUGLOG(tqpair, "host_hdgst_enable: %u\n", tqpair->flags.host_hdgst_enable);
	NVME_TQPAIR_DEBUGLOG(tqpair, "host_ddgst_enable: %u\n", tqpair->flags.host_ddgst_enable);

	/* Now that we know whether digests are enabled, properly size the receive buffer to
	 * handle several incoming 4K read commands according to SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR
	 * parameter. */
	/* [한국어] 4K read 한 건에 필요한 최대 응답 크기 = 4K data + C2H data hdr + 옵션 digests. */
	recv_buf_size = 0x1000 + sizeof(struct spdk_nvme_tcp_c2h_data_hdr);

	if (tqpair->flags.host_hdgst_enable) {
		recv_buf_size += SPDK_NVME_TCP_DIGEST_LEN;
	}

	if (tqpair->flags.host_ddgst_enable) {
		recv_buf_size += SPDK_NVME_TCP_DIGEST_LEN;
	}

	/* [한국어] FACTOR(보통 8 등)를 곱해 여러 응답을 한 번에 흡수할 수 있도록 SO_RCVBUF setsockopt. */
	if (spdk_sock_set_recvbuf(tqpair->sock, recv_buf_size * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR) < 0) {
		NVME_TQPAIR_WARNLOG(tqpair, "Unable to allocate enough memory for receive buffer with size=%d\n",
				    recv_buf_size);
		/* Not fatal. */
	}

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);  /* [한국어] 다음 PDU 수신 준비. */

	tqpair->flags.icresp_received = true;
	if (!tqpair->flags.icreq_send_ack) {
		NVME_TQPAIR_DEBUGLOG(tqpair, "waiting icreq ack\n");
		return;  /* [한국어] ICReq 송신 완료를 기다린다 — send 콜백에서 상태 천이. */
	}

	nvme_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND);  /* [한국어] 둘 다 완료 → CONNECT 단계로. */
	return;
end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);  /* [한국어] 협상 실패 → 종료. */
}

/*
 * [한국어]
 * nvme_tcp_capsule_resp_hdr_handle - CapsuleResp PDU 수신 처리 (NVMe CQE 캡슐)
 *
 * @tqpair: 큐페어
 * @pdu: 수신 PDU
 * @reaped: in/out 완료 카운트
 *
 * CapsuleResp = NVMe CQE를 그대로 감싼 PDU. 헤더만 보면 처리 끝 (data 영역 없음).
 *
 * 동작:
 *   1) cccid (rccqe.cid)로 tcp_req 역참조.
 *   2) tcp_req->rsp에 CQE 복사.
 *   3) data_recv = 1 표시 (write/no-data req는 이게 완료 시점, read는 LAST_PDU에서 이미 1).
 *   4) accel sequence 보유 시 reverse + finish, 콜백에서 마무리.
 *   5) 그렇지 않으면 req_complete_safe로 즉시 완료 시도.
 */
static void
nvme_tcp_capsule_resp_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
				 uint32_t *reaped)
{
	struct nvme_tcp_req *tcp_req;
	struct nvme_tcp_poll_group *tgroup;
	struct spdk_nvme_tcp_rsp *capsule_resp = &pdu->hdr.capsule_resp;
	uint32_t cid, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	NVME_TQPAIR_DEBUGLOG(tqpair, "enter\n");
	cid = capsule_resp->rccqe.cid;
	tcp_req = get_nvme_active_req_by_cid(tqpair, cid);

	if (!tcp_req) {
		/* [한국어] 모르는 cid → target 버그 또는 wire corruption. */
		NVME_TQPAIR_ERRLOG(tqpair, "no tcp_req is found with cid=%u\n", cid);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_rsp, rccqe);
		goto end;
	}

	assert(tcp_req->req != NULL);

	tcp_req->rsp = capsule_resp->rccqe;       /* [한국어] CQE 복사 — 사용자에게 그대로 전달될 status. */
	tcp_req->ordering.bits.data_recv = 1;     /* [한국어] 응답 도착 = data 수신 완료 (read의 경우 이미 1일 수 있음). */

	/* Recv the pdu again */
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);  /* [한국어] 다음 PDU 수신 준비. */

	if (tcp_req->req->accel_sequence != NULL) {
		tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
		nvme_tcp_accel_reverse_sequence(tgroup, tcp_req->req->accel_sequence);
		nvme_tcp_accel_finish_sequence(tgroup, tcp_req, tcp_req->req->accel_sequence,
					       nvme_tcp_recv_payload_seq_cb, tcp_req);
		return;  /* [한국어] accel 콜백이 마무리. */
	}

	if (nvme_tcp_req_complete_safe(tcp_req)) {
		(*reaped)++;
	}

	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

/*
 * [한국어]
 * nvme_tcp_c2h_term_req_hdr_handle - C2H TermReq 헤더 수신 처리 (payload 수신 단계로 넘김)
 *
 * @tqpair: 큐페어
 * @pdu: 수신 PDU
 *
 * 컨트롤러가 호스트의 위반을 감지해 보낸 종료 요청. 헤더에서 fes를 검증하고,
 * payload 수신을 위해 data buffer를 hdr 직후로 설정 후 AWAIT_PAYLOAD로 천이.
 * payload는 위반된 PDU의 hdr 사본 — c2h_term_req_payload_handle이 dump 후 종료 진입.
 */
static void
nvme_tcp_c2h_term_req_hdr_handle(struct nvme_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *c2h_term_req = &pdu->hdr.term_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	if (c2h_term_req->fes > SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER) {
		/* [한국어] FES가 spec 정의 범위를 초과 — 호스트 → 컨트롤러로 다시 TermReq. */
		NVME_TQPAIR_ERRLOG(tqpair, "Fatal Error Status(FES) is unknown for c2h_term_req pdu=%p\n",
				   pdu);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_term_req_hdr, fes);
		goto end;
	}

	/* set the data buffer */
	/* [한국어] payload 영역 = hdr 직후, 길이 = plen - hlen. recv stage가 직접 hdr.raw 영역에 채움. */
	nvme_tcp_pdu_set_data(pdu, (uint8_t *)pdu->hdr.raw + c2h_term_req->common.hlen,
			      c2h_term_req->common.plen - c2h_term_req->common.hlen);
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;
end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

/*
 * [한국어]
 * nvme_tcp_c2h_data_hdr_handle - C2H Data PDU 헤더 수신 후 검증 + payload iov 매핑
 *
 * @tqpair: 큐페어
 * @pdu: 수신 PDU
 *
 * read 응답 데이터 PDU의 시작. 검증 후 사용자 buffer로 직접 zerocopy 수신을 위해 iov를 셋업.
 *
 * 검증:
 *   - cccid → tcp_req 매핑.
 *   - SUCCESS는 LAST_PDU와만 함께 가능 (스펙).
 *   - datal ≤ payload_size.
 *   - datao 일치 (이전 datao + 누적 데이터 = 새 PDU의 datao).
 *   - datao + datal ≤ payload_size (out-of-range 검증).
 *
 * 통과 후 사용자 buffer iov 빌드 (read 시점에 미뤘던) + pdu->data_iov로 매핑.
 * recv_state = AWAIT_PAYLOAD로 천이 → 데이터를 사용자 buffer에 직접 zerocopy 수신.
 */
static void
nvme_tcp_c2h_data_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data = &pdu->hdr.c2h_data;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	int flags = c2h_data->common.flags;
	int rc;

	NVME_TQPAIR_DEBUGLOG(tqpair, "enter\n");
	NVME_TQPAIR_DEBUGLOG(tqpair, "c2h_data info: datao=%u, datal=%u, cccid=%d\n", c2h_data->datao,
			     c2h_data->datal, c2h_data->cccid);
	tcp_req = get_nvme_active_req_by_cid(tqpair, c2h_data->cccid);
	if (!tcp_req) {
		NVME_TQPAIR_ERRLOG(tqpair, "no tcp_req found for c2hdata cid=%d\n", c2h_data->cccid);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, cccid);
		goto end;

	}

	NVME_TQPAIR_DEBUGLOG(tqpair, "tcp_req(%p): expected_datao=%u, payload_size=%u\n", tcp_req,
			     tcp_req->expected_datao, tcp_req->req->payload_size);

	/* [한국어] SUCCESS 플래그는 반드시 LAST_PDU와 함께만 (스펙 강제). */
	if (spdk_unlikely((flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) &&
			  !(flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU))) {
		NVME_TQPAIR_ERRLOG(tqpair, "Invalid flag flags=%d in c2h_data=%p\n", flags, c2h_data);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, common);
		goto end;
	}

	if (c2h_data->datal > tcp_req->req->payload_size) {
		/* [한국어] 한 PDU의 datal 자체가 사용자 buffer 크기를 초과 — out-of-range. */
		NVME_TQPAIR_ERRLOG(tqpair,
				   "Invalid datal for tcp_req(%p), datal(%u) exceeds payload_size(%u)\n",
				   tcp_req, c2h_data->datal, tcp_req->req->payload_size);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		goto end;
	}

	if (tcp_req->expected_datao != c2h_data->datao) {
		/* [한국어] gap/overlap 검출 — 이전 PDU 누적 + 다음 datao가 정확히 일치해야 함. */
		NVME_TQPAIR_ERRLOG(tqpair,
				   "Invalid datao for tcp_req(%p), received datal(%u) != expected datao(%u) in tcp_req\n",
				   tcp_req, c2h_data->datao, tcp_req->expected_datao);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, datao);
		goto end;
	}

	if ((c2h_data->datao + c2h_data->datal) > tcp_req->req->payload_size) {
		/* [한국어] 누적 + 이번 데이터가 사용자 buffer 끝을 넘어감. */
		NVME_TQPAIR_ERRLOG(tqpair,
				   "Invalid data range for tcp_req(%p), received (datao(%u) + datal(%u)) > datao(%u) in tcp_req\n",
				   tcp_req, c2h_data->datao, c2h_data->datal, tcp_req->req->payload_size);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, datal);
		goto end;

	}

	/* [한국어] read 시점에 미뤄둔 user buffer iov 빌드. */
	if (nvme_payload_type(&tcp_req->req->payload) == NVME_PAYLOAD_TYPE_CONTIG) {
		rc = nvme_tcp_build_contig_request(tqpair, tcp_req);
	} else {
		assert(nvme_payload_type(&tcp_req->req->payload) == NVME_PAYLOAD_TYPE_SGL);
		rc = nvme_tcp_build_sgl_request(tqpair, tcp_req);
	}

	if (rc) {
		/* Not the right error message but at least it handles the failure. */
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED;
		goto end;
	}

	/* [한국어] PDU의 data_iov[]를 user buffer iov로 세팅 — read_data가 wire에서 직접 user buffer로 zerocopy. */
	nvme_tcp_pdu_set_data_buf(pdu, tcp_req->iov, tcp_req->iovcnt,
				  c2h_data->datao, c2h_data->datal);
	pdu->req = tcp_req;  /* [한국어] payload_handle에서 사용할 back-pointer. */

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);  /* [한국어] data 수신 단계로. */
	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

/*
 * [한국어]
 * nvme_tcp_qpair_h2c_data_send_complete - H2C Data PDU 송신 완료 콜백 (R2T 응답 사이클 진행)
 *
 * @cb_arg: tcp_req
 *
 * 한 R2T에 대해 maxh2cdata 청크들로 분할 전송할 때 매 청크 완료마다 호출됨.
 *
 * 분기:
 *   1) r2tl_remain > 0 → 같은 R2T의 다음 청크 송신.
 *   2) r2tl_remain == 0 → 이 R2T 완료 — active_r2ts--.
 *      a) r2t_waiting_h2c_complete 비트 → 다음 R2T가 이미 도착해 있었음 → 새 R2T 사이클 시작.
 *      b) 아니면 invalidate (외부 메모리) + req_complete_safe.
 */
static void
nvme_tcp_qpair_h2c_data_send_complete(void *cb_arg)
{
	struct nvme_tcp_req *tcp_req = cb_arg;

	assert(tcp_req != NULL);

	tcp_req->ordering.bits.send_ack = 1;       /* [한국어] 이 청크 송신 완료. */
	if (tcp_req->r2tl_remain) {
		nvme_tcp_send_h2c_data(tcp_req);   /* [한국어] 같은 R2T의 다음 maxh2cdata 청크. */
	} else {
		assert(tcp_req->active_r2ts > 0);
		tcp_req->active_r2ts--;             /* [한국어] 이 R2T 완료 — outstanding R2T 카운트 감소. */
		tcp_req->state = NVME_TCP_REQ_ACTIVE;  /* [한국어] R2T 사이클 종료, 다시 ACTIVE. */

		if (tcp_req->ordering.bits.r2t_waiting_h2c_complete) {
			/* [한국어] 직전 R2T 사이클 중에 이미 다음 R2T가 도착해 있었음 → 즉시 시작. */
			tcp_req->ordering.bits.r2t_waiting_h2c_complete = 0;
			NVME_TQPAIR_DEBUGLOG(tcp_req->tqpair, "tcp_req %p: continue r2t\n", tcp_req);
			assert(tcp_req->active_r2ts > 0);
			tcp_req->ttag = tcp_req->ttag_r2t_next;          /* [한국어] 보관해둔 ttag로 교체. */
			tcp_req->r2tl_remain = tcp_req->r2tl_remain_next; /* [한국어] 보관해둔 r2tl도 복원. */
			tcp_req->state = NVME_TCP_REQ_ACTIVE_R2T;
			nvme_tcp_send_h2c_data(tcp_req);                 /* [한국어] 새 R2T 사이클 시작. */
			return;
		}

		if (tcp_req->ordering.bits.domain_in_use) {
			/* [한국어] 외부 메모리 도메인에서 보낸 데이터 — 도메인에 invalidate 통보. */
			spdk_memory_domain_invalidate_data(tcp_req->req->payload.opts->memory_domain,
							   tcp_req->req->payload.opts->memory_domain_ctx, tcp_req->iov, tcp_req->iovcnt);
		}

		/* Need also call this function to free the resource */
		nvme_tcp_req_complete_safe(tcp_req);  /* [한국어] data_recv도 1이면 완료. */
	}
}

/*
 * [한국어]
 * nvme_tcp_send_h2c_data - R2T 응답 H2C Data PDU 빌드 및 송신 (한 청크 = min(r2tl_remain, maxh2cdata))
 *
 * @tcp_req: R2T 사이클 중인 요청
 *
 * R2T flow control: 컨트롤러가 R2T로 (ttag, r2to, r2tl)을 보냈고 호스트는 r2tl을 maxh2cdata 단위로
 * 분할해 H2C Data PDU로 보낸다. 마지막 청크에는 LAST_PDU 플래그.
 *
 * PDU 레이아웃 (capsule_cmd와 유사):
 *   [common (8B)] [h2c_data_specific (24B)] [HDGST 4B?] [padding] [data] [DDGST 4B?]
 *
 * 호출자: cmd_send_complete의 R2T 처리 분기, h2c_data_send_complete의 다음 청크 분기, r2t_hdr_handle의 즉시 송신.
 */
static void
nvme_tcp_send_h2c_data(struct nvme_tcp_req *tcp_req)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(tcp_req->req->qpair);
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_h2c_data_hdr *h2c_data;
	uint32_t plen, pdo, alignment;

	/* Reinit the send_ack and h2c_send_waiting_ack bits */
	tcp_req->ordering.bits.send_ack = 0;          /* [한국어] 이 청크 송신 완료 대기로 리셋. */
	tcp_req->ordering.bits.h2c_send_waiting_ack = 0;  /* [한국어] 이미 송신 시작했으므로 wait 비트 클리어. */
	rsp_pdu = tcp_req->pdu;                        /* [한국어] req에 영구 할당된 PDU 슬롯 재사용. */
	memset(rsp_pdu, 0, sizeof(*rsp_pdu));          /* [한국어] 이전 capsule_cmd 흔적 제거. */
	rsp_pdu->req = tcp_req;
	h2c_data = &rsp_pdu->hdr.h2c_data;

	h2c_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_DATA;
	plen = h2c_data->common.hlen = sizeof(*h2c_data);
	h2c_data->cccid = tcp_req->cid;          /* [한국어] 컨트롤러가 어느 명령에 대한 데이터인지 매칭. */
	h2c_data->ttag = tcp_req->ttag;          /* [한국어] R2T가 발급한 ttag echo (동일 명령에 여러 R2T 가능). */
	h2c_data->datao = tcp_req->datao;        /* [한국어] 누적 진행도 — 컨트롤러가 reassemble. */

	/* [한국어] 한 PDU에 보낼 데이터 크기 = min(R2T 잔여, 컨트롤러가 한 PDU당 받을 수 있는 최대). */
	h2c_data->datal = spdk_min(tcp_req->r2tl_remain, tqpair->maxh2cdata);
	nvme_tcp_pdu_set_data_buf(rsp_pdu, tcp_req->iov, tcp_req->iovcnt,
				  h2c_data->datao, h2c_data->datal);
	tcp_req->r2tl_remain -= h2c_data->datal;  /* [한국어] R2T 잔여 차감. */

	if (tqpair->flags.host_hdgst_enable) {
		h2c_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	rsp_pdu->padding_len = 0;
	pdo = plen;
	if (tqpair->cpda) {
		alignment = (tqpair->cpda + 1) << 2;  /* [한국어] CPDA 정렬 단위. */
		if (alignment > plen) {
			rsp_pdu->padding_len = alignment - plen;  /* [한국어] data 시작점 정렬 padding. */
			pdo = plen = alignment;
		}
	}

	h2c_data->common.pdo = pdo;
	plen += h2c_data->datal;
	if (tqpair->flags.host_ddgst_enable) {
		h2c_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	h2c_data->common.plen = plen;
	tcp_req->datao += h2c_data->datal;        /* [한국어] 누적 진행도 갱신 — 다음 호출에서 datao로 사용. */
	if (!tcp_req->r2tl_remain) {
		h2c_data->common.flags |= SPDK_NVME_TCP_H2C_DATA_FLAGS_LAST_PDU;  /* [한국어] R2T 종료 표시. */
	}

	NVME_TQPAIR_DEBUGLOG(tqpair, "h2c_data info: datao=%u, datal=%u, pdu_len=%u\n",
			     h2c_data->datao, h2c_data->datal, h2c_data->common.plen);
	/* [한국어] write_pdu → digest 계산 → sock writev → h2c_data_send_complete. */
	nvme_tcp_qpair_write_pdu(tqpair, rsp_pdu, nvme_tcp_qpair_h2c_data_send_complete, tcp_req);
}

/*
 * [한국어]
 * nvme_tcp_r2t_hdr_handle - R2T PDU 수신 처리 (write 요청 데이터 송신 트리거)
 *
 * @tqpair: 큐페어
 * @pdu: 수신된 R2T PDU
 *
 * R2T flow control: write 요청 시 in-capsule data가 아니거나, R2T 정책으로 데이터를 명시 요청하는 경우
 * 컨트롤러가 R2T로 (cccid, ttag, r2to, r2tl)을 보내면 호스트가 H2C Data로 응답.
 *
 * 검증:
 *   - cccid → tcp_req 매핑.
 *   - r2to == tcp_req->datao (이전 진행도와 일치).
 *   - r2to + r2tl ≤ payload_size (out-of-range).
 *   - active_r2ts ≤ maxr2t (한도 초과 시 위반).
 *
 * 분기:
 *   - 직전 H2C가 아직 진행 중이면 ttag_r2t_next에 보관 + r2t_waiting_h2c_complete 비트 세팅.
 *   - send_ack가 1이면 즉시 H2C Data 송신 시작.
 *   - 0이면 h2c_send_waiting_ack 세팅 (cmd_send_complete 콜백이 트리거).
 */
static void
nvme_tcp_r2t_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_r2t_hdr *r2t = &pdu->hdr.r2t;
	uint32_t cid, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	NVME_TQPAIR_DEBUGLOG(tqpair, "enter\n");
	cid = r2t->cccid;
	tcp_req = get_nvme_active_req_by_cid(tqpair, cid);
	if (!tcp_req) {
		NVME_TQPAIR_ERRLOG(tqpair, "Cannot find tcp_req\n");
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, cccid);
		goto end;
	}

	NVME_TQPAIR_DEBUGLOG(tqpair, "r2t info: r2to=%u, r2tl=%u\n", r2t->r2to, r2t->r2tl);

	if (tcp_req->state == NVME_TCP_REQ_ACTIVE) {
		assert(tcp_req->active_r2ts == 0);
		tcp_req->state = NVME_TCP_REQ_ACTIVE_R2T;  /* [한국어] 첫 R2T 도착 → R2T 사이클 진입. */
	}

	if (tcp_req->datao != r2t->r2to) {
		/* [한국어] R2T가 요청한 시작점이 우리의 진행도와 다름 — 비정상. */
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, r2to);
		goto end;

	}

	if ((r2t->r2tl + r2t->r2to) > tcp_req->req->payload_size) {
		/* [한국어] R2T가 요청한 영역이 사용자 buffer 밖 — out-of-range. */
		NVME_TQPAIR_ERRLOG(tqpair,
				   "Invalid R2T info for tcp_req=%p: (r2to(%u) + r2tl(%u)) exceeds payload_size(%u)\n",
				   tcp_req, r2t->r2to, r2t->r2tl, tqpair->maxh2cdata);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, r2tl);
		goto end;
	}

	tcp_req->active_r2ts++;
	if (spdk_unlikely(tcp_req->active_r2ts > tqpair->maxr2t)) {
		/* [한국어] maxr2t 초과 케이스 — 두 가지 처리. */
		if (tcp_req->state == NVME_TCP_REQ_ACTIVE_R2T && !tcp_req->ordering.bits.send_ack) {
			/* We receive a subsequent R2T while we are waiting for H2C transfer to complete */
			/* [한국어] 직전 H2C 전송이 아직 sock 완료 전인데 다음 R2T가 도착한 희귀 케이스.
			 *  보관 후 send_ack 시점에 사용. (SPDK target은 1 R2T만 보내므로 이 케이스는 비-SPDK 타깃). */
			NVME_TQPAIR_DEBUGLOG(tqpair, "received a subsequent R2T\n");
			assert(tcp_req->active_r2ts == tqpair->maxr2t + 1);
			tcp_req->ttag_r2t_next = r2t->ttag;
			tcp_req->r2tl_remain_next = r2t->r2tl;
			tcp_req->ordering.bits.r2t_waiting_h2c_complete = 1;
			nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
			return;
		} else {
			/* [한국어] 진짜 초과 — 프로토콜 위반. */
			fes = SPDK_NVME_TCP_TERM_REQ_FES_R2T_LIMIT_EXCEEDED;
			NVME_TQPAIR_ERRLOG(tqpair, "Invalid R2T: Maximum number of R2T exceeded! Max: %u\n",
					   tqpair->maxr2t);
			goto end;
		}
	}

	tcp_req->ttag = r2t->ttag;             /* [한국어] H2C Data PDU에 echo할 ttag 저장. */
	tcp_req->r2tl_remain = r2t->r2tl;       /* [한국어] 이번 R2T의 잔여 길이 시작값. */
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);  /* [한국어] 다음 PDU 수신 준비. */

	if (spdk_likely(tcp_req->ordering.bits.send_ack)) {
		/* [한국어] 직전 PDU 송신 완료 → 즉시 H2C Data 송신 시작 (보통 케이스). */
		nvme_tcp_send_h2c_data(tcp_req);
	} else {
		/* [한국어] 직전 PDU 송신 완료 콜백 대기 중 → 콜백에서 트리거하도록 표시. */
		tcp_req->ordering.bits.h2c_send_waiting_ack = 1;
	}

	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);

}

/*
 * [한국어]
 * nvme_tcp_pdu_psh_handle - PDU specific header 수신 완료 후 처리 (HDGST 검증 + PDU 종류별 디스패치)
 *
 * @tqpair: 큐페어
 * @reaped: in/out 완료 카운트
 *
 * read_pdu 상태머신의 AWAIT_PDU_PSH 단계에서 PSH(헤더 + 옵션 HDGST)가 다 도착하면 호출.
 *
 * 동작:
 *   1) HDGST 검증 (has_hdgst=true면).
 *   2) PDU 종류별 hdr_handle 함수 분기:
 *      - IC_RESP: icresp_handle (협상 결과 저장).
 *      - CAPSULE_RESP: capsule_resp_hdr_handle (NVMe CQE → req 완료).
 *      - C2H_DATA: c2h_data_hdr_handle (검증 + iov 매핑 + payload 단계).
 *      - C2H_TERM_REQ: c2h_term_req_hdr_handle (payload 단계).
 *      - R2T: r2t_hdr_handle (H2C Data 송신 트리거).
 */
static void
nvme_tcp_pdu_psh_handle(struct nvme_tcp_qpair *tqpair, uint32_t *reaped)
{
	struct nvme_tcp_pdu *pdu;
	int rc;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
	pdu = tqpair->recv_pdu;

	NVME_TQPAIR_DEBUGLOG(tqpair, "enter: pdu type =%u\n", pdu->hdr.common.pdu_type);
	/* check header digest if needed */
	if (pdu->has_hdgst) {
		/* [한국어] PSH 영역에 대한 CRC32C 계산 후 wire에 도착한 4B HDGST와 비교. */
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		rc = MATCH_DIGEST_WORD((uint8_t *)pdu->hdr.raw + pdu->hdr.common.hlen, crc32c);
		if (rc == 0) {
			/* [한국어] HDGST 불일치 — wire 손상 또는 컨트롤러 버그. TermReq로 종료. */
			NVME_TQPAIR_ERRLOG(tqpair, "header digest error with pdu=%p\n", pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_HDGST_ERROR;
			nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
			return;

		}
	}

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_IC_RESP:
		nvme_tcp_icresp_handle(tqpair, pdu);                        /* [한국어] 트랜스포트 협상 완료. */
		break;
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP:
		nvme_tcp_capsule_resp_hdr_handle(tqpair, pdu, reaped);      /* [한국어] NVMe CQE → req 완료. */
		break;
	case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
		nvme_tcp_c2h_data_hdr_handle(tqpair, pdu);                   /* [한국어] read 응답 데이터 → payload 단계. */
		break;

	case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
		nvme_tcp_c2h_term_req_hdr_handle(tqpair, pdu);               /* [한국어] 컨트롤러 측 종료 요청. */
		break;
	case SPDK_NVME_TCP_PDU_TYPE_R2T:
		nvme_tcp_r2t_hdr_handle(tqpair, pdu);                        /* [한국어] write 데이터 송신 트리거. */
		break;

	default:
		NVME_TQPAIR_ERRLOG(tqpair, "Unexpected PDU type 0x%02x\n",
				   tqpair->recv_pdu->hdr.common.pdu_type);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = 1;
		nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
		break;
	}

}

/*
 * [한국어]
 * nvme_tcp_read_pdu - PDU 수신 상태머신 메인 루프 (CH → PSH → PAYLOAD)
 *
 * @tqpair: 큐페어
 * @reaped: in/out 완료 카운트 (async_complete를 초기값으로)
 * @max_completions: 한 라운드 polling 상한
 * @return: 0 정상, NVME_TCP_PDU_IN_PROGRESS(=0) 부분 수신, NVME_TCP_PDU_FATAL 종료, 음수 errno 에러
 *
 * read_data → 상태 천이 → handle 함수 호출의 do-while 루프. 한 호출에서 여러 PDU + 상태 천이를 처리.
 *
 * 상태별 동작:
 *   - AWAIT_PDU_READY: recv_pdu 0 클리어 → AWAIT_PDU_CH로.
 *   - AWAIT_PDU_CH: 8B common 헤더 read. 다 읽으면 ch_handle (검증 + PSH 길이 결정).
 *   - AWAIT_PDU_PSH: PSH(헤더 + 옵션 HDGST) read. 다 읽으면 psh_handle.
 *   - AWAIT_PDU_PAYLOAD: data + 옵션 DDGST read. 다 읽으면 payload_handle.
 *   - QUIESCING: outstanding이 비워지면 disconnect_qpair_done + ERROR로.
 *   - ERROR: NVME_TCP_PDU_FATAL 반환 (호출자가 disconnect 진행).
 */
static int
nvme_tcp_read_pdu(struct nvme_tcp_qpair *tqpair, uint32_t *reaped, uint32_t max_completions)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu;
	uint32_t data_len;
	enum nvme_tcp_pdu_recv_state prev_state;

	*reaped = tqpair->async_complete;  /* [한국어] 비동기 완료 누적치를 시작값으로 — process_completions 반환에 합산. */
	tqpair->async_complete = 0;

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		if (*reaped >= max_completions) {
			break;  /* [한국어] 라운드 상한 도달 — 다음 polling 라운드로. */
		}

		prev_state = tqpair->recv_state;
		pdu = tqpair->recv_pdu;
		switch (tqpair->recv_state) {
		/* If in a new state */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
			memset(pdu, 0, sizeof(struct nvme_tcp_pdu));   /* [한국어] 새 PDU를 위해 recv_pdu 깨끗이. */
			nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
			break;
		/* Wait for the pdu common header */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH:
			assert(pdu->ch_valid_bytes < sizeof(struct spdk_nvme_tcp_common_pdu_hdr));
			/* [한국어] 8B 공통 헤더를 점진적으로 read. (ch_valid_bytes는 이전 라운드에서 부분 수신된 누적치). */
			rc = nvme_tcp_read_data(tqpair->sock,
						sizeof(struct spdk_nvme_tcp_common_pdu_hdr) - pdu->ch_valid_bytes,
						(uint8_t *)&pdu->hdr.common + pdu->ch_valid_bytes);
			if (rc < 0) {
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);  /* [한국어] read 에러 → 종료. */
				break;
			}
			pdu->ch_valid_bytes += rc;
			if (pdu->ch_valid_bytes < sizeof(struct spdk_nvme_tcp_common_pdu_hdr)) {
				return NVME_TCP_PDU_IN_PROGRESS;  /* [한국어] 부분 수신 — 다음 폴링에서 재시도. */
			}

			/* The command header of this PDU has now been read from the socket. */
			nvme_tcp_pdu_ch_handle(tqpair);   /* [한국어] 검증 + PSH 단계 천이. */
			break;
		/* Wait for the pdu specific header  */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH:
			assert(pdu->psh_valid_bytes < pdu->psh_len);
			/* [한국어] common 직후부터 PSH read (PDU별 헤더 + 옵션 HDGST). */
			rc = nvme_tcp_read_data(tqpair->sock,
						pdu->psh_len - pdu->psh_valid_bytes,
						(uint8_t *)&pdu->hdr.raw + sizeof(struct spdk_nvme_tcp_common_pdu_hdr) + pdu->psh_valid_bytes);
			if (rc < 0) {
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
				break;
			}

			pdu->psh_valid_bytes += rc;
			if (pdu->psh_valid_bytes < pdu->psh_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* All header(ch, psh, head digits) of this PDU has now been read from the socket. */
			nvme_tcp_pdu_psh_handle(tqpair, reaped);   /* [한국어] HDGST 검증 + PDU 종류별 hdr_handle 디스패치. */
			break;
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
			/* check whether the data is valid, if not we just return */
			if (!pdu->data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;  /* [한국어] data 영역 없는 PDU는 PSH에서 끝나야 함. */
			}

			data_len = pdu->data_len;
			/* data digest */
			if (spdk_unlikely((pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_DATA) &&
					  tqpair->flags.host_ddgst_enable)) {
				data_len += SPDK_NVME_TCP_DIGEST_LEN;  /* [한국어] DDGST 4B 추가 read. */
				pdu->ddgst_enable = true;             /* [한국어] payload_handle이 검증하도록 표시. */
			}

			/* [한국어] data_iov로 직접 zerocopy read. rw_offset은 누적 read 진행도. */
			rc = nvme_tcp_read_payload_data(tqpair->sock, pdu);
			if (rc < 0) {
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
				break;
			}

			pdu->rw_offset += rc;
			if (pdu->rw_offset < data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			assert(pdu->rw_offset == data_len);
			/* All of this PDU has now been read from the socket. */
			nvme_tcp_pdu_payload_handle(tqpair, reaped);   /* [한국어] DDGST 검증 + PDU 종류별 payload_handle. */
			break;
		case NVME_TCP_PDU_RECV_STATE_QUIESCING:
			/* [한국어] 종료 진입 단계. outstanding이 비워지면 disconnect 완료 통보 후 ERROR로. */
			if (TAILQ_EMPTY(&tqpair->outstanding_reqs)) {
				if (nvme_qpair_get_state(&tqpair->qpair) == NVME_QPAIR_DISCONNECTING) {
					nvme_transport_ctrlr_disconnect_qpair_done(&tqpair->qpair);
				}

				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
			}
			break;
		case NVME_TCP_PDU_RECV_STATE_ERROR:
			memset(pdu, 0, sizeof(struct nvme_tcp_pdu));
			return NVME_TCP_PDU_FATAL;  /* [한국어] 호출자가 disconnect 트리거. */
		default:
			assert(0);
			break;
		}
	} while (prev_state != tqpair->recv_state);  /* [한국어] 상태 변화가 없을 때까지 반복 — 한 호출에 여러 PDU 처리. */

	return rc > 0 ? 0 : rc;
}

/*
 * [한국어]
 * nvme_tcp_qpair_check_timeout - outstanding 요청들의 timeout을 검사하고 만료 시 콜백 호출
 *
 * @qpair: 큐페어
 *
 * 호출자: process_completions 안에서 timeout_enabled 큐페어에 대해.
 * 사용자가 spdk_nvme_ctrlr_register_timeout_callback으로 등록한 콜백이 만료된 요청에 대해 호출됨.
 *
 * 최적화: outstanding은 submit 순서대로 정렬되어 있으므로 첫 번째 미만료 만나면 break (이후는 더 최근).
 * 컨트롤러 init 중에는 검사 X — busy-wait timeout과 충돌 회피.
 */
static void
nvme_tcp_qpair_check_timeout(struct spdk_nvme_qpair *qpair)
{
	uint64_t t02;
	struct nvme_tcp_req *tcp_req, *tmp;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvme_ctrlr_process *active_proc;

	/* Don't check timeouts during controller initialization. */
	if (spdk_unlikely(ctrlr->state != NVME_CTRLR_STATE_READY)) {
		return;
	}

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		active_proc = nvme_ctrlr_get_current_process(ctrlr);  /* [한국어] admin은 process 단위 콜백. */
	} else {
		active_proc = qpair->active_proc;                     /* [한국어] io는 qpair 보유 process. */
	}

	/* Only check timeouts if the current process has a timeout callback. */
	if (spdk_unlikely(active_proc == NULL || active_proc->timeout_cb_fn == NULL)) {
		return;
	}

	t02 = spdk_get_ticks();   /* [한국어] 현재 TSC — 각 req의 submit_tick과 비교. */
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		if (spdk_unlikely(ctrlr->is_failed)) {
			/* The controller state may be changed to failed in one of the nvme_request_check_timeout callbacks. */
			return;  /* [한국어] 콜백 안에서 컨트롤러를 fail시킨 경우 즉시 빠져나감. */
		}
		assert(tcp_req->req != NULL);

		if (spdk_likely(nvme_request_check_timeout(tcp_req->req, tcp_req->cid, active_proc, t02))) {
			/*
			 * The requests are in order, so as soon as one has not timed out,
			 * stop iterating.
			 */
			break;  /* [한국어] true = 미만료 → 이후 모두 미만료 (insertion 순서). */
		}
	}
}

/*
 * [한국어]
 * nvme_tcp_qpair_process_completions - vtable: 큐페어 단독 polling 진입점 (poll group이 없는 경우)
 *
 * @qpair: 사용자 큐페어
 * @max_completions: 처리할 최대 완료 수 (0 = num_entries)
 * @return: 완료 수, -ENXIO 에러
 *
 * 호출자: spdk_nvme_qpair_process_completions (사용자 직접 호출 또는 nvme_qpair_process_completions).
 * poll group에 속하면 poll_group_process_completions가 대신 호출 → 이 함수는 standalone qpair용.
 *
 * 동작:
 *   1) standalone이면 sock_flush로 송신 큐에 누적된 PDU들을 wire에 밀어냄.
 *   2) timeout 검사.
 *   3) read_pdu로 수신 처리.
 *   4) CONNECTING 상태면 connect_qpair_poll로 핸드셰이크 진행.
 *   5) CONNECTED 천이 시 queued_req 재제출.
 *   6) 에러 시 fail로 disconnect.
 */
static int
nvme_tcp_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	uint32_t reaped;
	int rc;

	if (qpair->poll_group == NULL) {
		/* [한국어] standalone — 송신 큐를 강제 flush. poll group인 경우 sock_group_poll이 알아서 처리. */
		rc = spdk_sock_flush(tqpair->sock);
		if (rc < 0 && rc != -EAGAIN) {
			NVME_TQPAIR_ERRLOG(tqpair, "spdk_sock_flush() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
			if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
				/* [한국어] disconnecting 중에 flush 실패 — outstanding 비면 done. */
				if (TAILQ_EMPTY(&tqpair->outstanding_reqs)) {
					nvme_transport_ctrlr_disconnect_qpair_done(qpair);
				}

				/* Don't return errors until the qpair gets disconnected */
				return 0;
			}

			goto fail;
		}

		if (qpair->ctrlr->timeout_enabled) {
			nvme_tcp_qpair_check_timeout(qpair);
		}
	}

	if (max_completions == 0) {
		max_completions = spdk_max(tqpair->num_entries, 1);  /* [한국어] 기본값 = qpair 전체 처리. */
	} else {
		max_completions = spdk_min(max_completions, tqpair->num_entries);  /* [한국어] cap. */
	}

	reaped = 0;
	rc = nvme_tcp_read_pdu(tqpair, &reaped, max_completions);
	if (rc < 0) {
		NVME_TQPAIR_DEBUGLOG(tqpair, "Error polling CQ! (%d): %s\n", errno, spdk_strerror(errno));
		goto fail;
	}

	if (spdk_unlikely(nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING)) {
		/* [한국어] 연결 진행 중이면 핸드셰이크 상태머신 한 step 진행. */
		rc = nvme_tcp_ctrlr_connect_qpair_poll(qpair->ctrlr, qpair);
		if (rc != 0 && rc != -EAGAIN) {
			NVME_TQPAIR_ERRLOG(tqpair, "Failed to connect\n");
			goto fail;
		} else if (rc == 0) {
			/* Once the connection is completed, we can submit queued requests */
			/* [한국어] 연결 완료 → 그동안 큐된 요청들을 재제출. */
			nvme_qpair_resubmit_requests(qpair, tqpair->num_entries);
		}
	}

	return reaped;
fail:
	qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;
	nvme_ctrlr_disconnect_qpair(qpair);  /* [한국어] 에러 시 disconnect 트리거. */
	return -ENXIO;
}

/*
 * [한국어]
 * nvme_tcp_qpair_sock_cb - sock_group_poll이 한 큐페어의 sock에서 이벤트 발생 시 호출하는 콜백
 *
 * @ctx: qpair (poll_group_add 시점에 등록)
 * @group: sock_group (사용 안 함)
 * @sock: 이벤트 발생한 sock (사용 안 함 — qpair에서 가져옴)
 *
 * 컨텍스트: nvme_tcp_poll_group_process_completions 안 → spdk_sock_group_poll → 이 콜백.
 *
 * 동작:
 *   1) needs_poll 리스트에서 제거 (어차피 처리 중).
 *   2) spdk_nvme_qpair_process_completions로 위임 — 위 함수가 read_pdu 등 호출.
 *   3) 결과 카운트를 poll_group의 num_completions/stats에 누적.
 */
static void
nvme_tcp_qpair_sock_cb(void *ctx, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_nvme_qpair *qpair = ctx;
	struct nvme_tcp_poll_group *pgroup = nvme_tcp_poll_group(qpair->poll_group);
	int32_t num_completions;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	if (TAILQ_ENTRY_ENQUEUED(tqpair, link_poll)) {
		TAILQ_REMOVE_CLEAR(&pgroup->needs_poll, tqpair, link_poll);  /* [한국어] 이미 polling 중이므로 보조 리스트에서 빼기. */
	}

	num_completions = spdk_nvme_qpair_process_completions(qpair, pgroup->completions_per_qpair);

	if (pgroup->num_completions >= 0 && num_completions >= 0) {
		pgroup->num_completions += num_completions;            /* [한국어] poll_group 라운드 누적. */
		pgroup->stats.nvme_completions += num_completions;     /* [한국어] stats 누적. */
	} else {
		pgroup->num_completions = -ENXIO;                       /* [한국어] 에러 표시 (한 번이라도 실패하면 sticky). */
	}
}

/*
 * [한국어]
 * nvme_tcp_qpair_icreq_send - ICReq PDU 빌드 및 송신 (트랜스포트 협상 시작)
 *
 * @tqpair: 큐페어 (sock connected 직후)
 *
 * NVMe-oF TCP 핸드셰이크의 첫 단계. 호스트가 컨트롤러에게 다음을 제안:
 *   - PFV (PDU Format Version): 0.
 *   - maxr2t: 호스트가 동시에 outstanding 가능한 R2T 수 (0-base, NVME_TCP_MAX_R2T_DEFAULT-1).
 *   - hpda: Host PDU Data Alignment (0 = 4B 정렬만 요구).
 *   - dgst.bits.hdgst_enable, ddgst_enable: 사용자 옵션 (ctrlr->opts).
 *
 * 컨트롤러가 ICResp로 응답하면 위 값들이 협상되어 tqpair에 저장됨 (icresp_handle).
 * timeout: 동기 모드 2초, 비동기 모드 10초 — connect_qpair_poll이 만료 검사.
 */
static void
nvme_tcp_qpair_icreq_send(struct nvme_tcp_qpair *tqpair)
{
	struct spdk_nvme_tcp_ic_req *ic_req;
	struct nvme_tcp_pdu *pdu;
	uint32_t timeout_in_sec;

	pdu = tqpair->send_pdu;                          /* [한국어] tcp_req와 무관한 단발 PDU 슬롯. */
	memset(tqpair->send_pdu, 0, sizeof(*tqpair->send_pdu));
	ic_req = &pdu->hdr.ic_req;

	ic_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_REQ;
	ic_req->common.hlen = ic_req->common.plen = sizeof(*ic_req);  /* [한국어] data 영역 없음 → hlen=plen. */
	ic_req->pfv = 0;
	ic_req->maxr2t = NVME_TCP_MAX_R2T_DEFAULT - 1;   /* [한국어] 0-base 표현 — 1이면 wire에는 0. */
	ic_req->hpda = NVME_TCP_HPDA_DEFAULT;             /* [한국어] 기본 4B 정렬. */

	/* [한국어] 사용자가 컨트롤러 opts에서 요청한 digest 옵션. 컨트롤러가 받아들일지는 ICResp에서 결정. */
	ic_req->dgst.bits.hdgst_enable = tqpair->qpair.ctrlr->opts.header_digest;
	ic_req->dgst.bits.ddgst_enable = tqpair->qpair.ctrlr->opts.data_digest;

	/* [한국어] write_pdu → sock writev → send_icreq_complete (icreq_send_ack 비트 세팅). */
	nvme_tcp_qpair_write_pdu(tqpair, pdu, nvme_tcp_send_icreq_complete, tqpair);

	/* [한국어] ICResp 도착 timeout 시작점 기록. connect_qpair_poll의 INITIALIZING 분기에서 검사. */
	timeout_in_sec = tqpair->qpair.async ? ICREQ_TIMEOUT_ASYNC : ICREQ_TIMEOUT_SYNC;
	tqpair->icreq_timeout_tsc = spdk_get_ticks() + (timeout_in_sec * spdk_get_ticks_hz());
}

/*
 * [한국어]
 * nvme_tcp_sock_connect_cb_fn - spdk_sock_connect_async의 TCP 3-way 핸드셰이크 완료 콜백
 *
 * @cb_arg: tqpair
 * @status: 0 성공, 음수 실패 (ECONNREFUSED 등)
 *
 * TCP가 연결되면 즉시 NVMe-oF TCP 협상의 첫 단계인 ICReq 송신.
 * 실패 시 disconnect로 정리 (process_completions 다음 라운드에서).
 */
static void
nvme_tcp_sock_connect_cb_fn(void *cb_arg, int status)
{
	struct nvme_tcp_qpair *tqpair = cb_arg;

	if (status < 0) {
		NVME_TQPAIR_ERRLOG(tqpair, "sock connection error %d (%s)\n", status, spdk_strerror(abs(status)));
		return;
	}

	nvme_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_INITIALIZING);  /* [한국어] TCP 연결됨 → 트랜스포트 negotiation. */
	nvme_tcp_qpair_icreq_send(tqpair);  /* [한국어] ICReq 발사. */
}

/*
 * [한국어]
 * nvme_tcp_qpair_connect_sock - TCP 소켓 비동기 connect 시작 (TLS 옵션 포함)
 *
 * @ctrlr: 컨트롤러
 * @qpair: 큐페어
 * @return: 0 성공 (async — 완료는 콜백에서), 음수 실패
 *
 * 동작:
 *   1) trid의 ADRFAM(IPv4/IPv6) → AF_INET/AF_INET6.
 *   2) traddr/trsvcid → sockaddr 변환.
 *   3) 옵션 src_addr/src_svcid 처리 (소스 IP/포트 바인딩).
 *   4) PSK 키가 있으면 sock_impl = "ssl" + TLS 1.3 + PSK 옵션 세팅.
 *   5) opts: priority, zcopy(io_qpair만), ack_timeout, connect_timeout 세팅.
 *   6) 상태를 SOCK_CONNECTING으로 천이.
 *   7) spdk_sock_connect_async → 콜백 등록 (sock_connect_cb_fn).
 */
static int
nvme_tcp_qpair_connect_sock(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct sockaddr_storage dst_addr;
	struct sockaddr_storage src_addr;
	int rc;
	struct nvme_tcp_qpair *tqpair;
	int family;
	long int port, src_port = 0;
	char *sock_impl_name;
	struct spdk_sock_impl_opts impl_opts = {};
	size_t impl_opts_size = sizeof(impl_opts);
	struct spdk_sock_opts opts;
	struct nvme_tcp_ctrlr *tcp_ctrlr;

	tqpair = nvme_tcp_qpair(qpair);

	switch (ctrlr->trid.adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		family = AF_INET;
		break;
	case SPDK_NVMF_ADRFAM_IPV6:
		family = AF_INET6;
		break;
	default:
		/* [한국어] FC/IB 등은 TCP 트랜스포트와 호환 X. */
		NVME_TQPAIR_ERRLOG(tqpair, "Unhandled ADRFAM %d\n", ctrlr->trid.adrfam);
		rc = -1;
		return rc;
	}

	NVME_TQPAIR_DEBUGLOG(tqpair, "adrfam %d ai_family %d\n", ctrlr->trid.adrfam, family);

	memset(&dst_addr, 0, sizeof(dst_addr));

	NVME_TQPAIR_DEBUGLOG(tqpair, "trsvcid is %s\n", ctrlr->trid.trsvcid);
	rc = nvme_parse_addr(&dst_addr, family, ctrlr->trid.traddr, ctrlr->trid.trsvcid, &port);  /* [한국어] traddr/trsvcid → sockaddr_in[6]. */
	if (rc != 0) {
		NVME_TQPAIR_ERRLOG(tqpair, "dst_addr nvme_parse_addr() failed\n");
		return rc;
	}

	if (ctrlr->opts.src_addr[0] || ctrlr->opts.src_svcid[0]) {
		/* [한국어] 사용자가 source IP/port를 명시한 경우 — 멀티 NIC 환경에서 어느 NIC 사용할지 지정. */
		memset(&src_addr, 0, sizeof(src_addr));
		rc = nvme_parse_addr(&src_addr, family,
				     ctrlr->opts.src_addr[0] ? ctrlr->opts.src_addr : NULL,
				     ctrlr->opts.src_svcid[0] ? ctrlr->opts.src_svcid : NULL,
				     &src_port);
		if (rc != 0) {
			NVME_TQPAIR_ERRLOG(tqpair, "src_addr nvme_parse_addr() failed\n");
			return rc;
		}
	}

	tcp_ctrlr = SPDK_CONTAINEROF(ctrlr, struct nvme_tcp_ctrlr, ctrlr);
	/* [한국어] PSK 키가 비어있지 않으면 SSL 소켓 구현 사용 (TLS 1.3 + PSK auth). */
	sock_impl_name = tcp_ctrlr->psk[0] ? "ssl" : NULL;
	NVME_TQPAIR_DEBUGLOG(tqpair, "sock_impl_name is %s\n", sock_impl_name);

	if (sock_impl_name) {
		/* [한국어] sock impl(ssl/posix/uring) 별 기본 opts에 TLS 정보 채워넣기. */
		spdk_sock_impl_get_opts(sock_impl_name, &impl_opts, &impl_opts_size);
		impl_opts.tls_version = SPDK_TLS_VERSION_1_3;          /* [한국어] NVMe-oF TP 8011 = TLS 1.3 강제. */
		impl_opts.psk_identity = tcp_ctrlr->psk_identity;       /* [한국어] PSK Identity 문자열. */
		impl_opts.psk_key = tcp_ctrlr->psk;                     /* [한국어] 유도된 TLS PSK binary. */
		impl_opts.psk_key_size = tcp_ctrlr->psk_size;
		impl_opts.tls_cipher_suites = tcp_ctrlr->tls_cipher_suite;  /* [한국어] AES_128_GCM_SHA256 또는 AES_256_GCM_SHA384. */
	}
	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	opts.priority = ctrlr->trid.priority;                          /* [한국어] SO_PRIORITY 등. */
	opts.zcopy = !nvme_qpair_is_admin_queue(qpair);                /* [한국어] io qpair만 MSG_ZEROCOPY 가능 (admin은 불필요). */
	opts.src_addr = ctrlr->opts.src_addr[0] ? ctrlr->opts.src_addr : NULL;
	opts.src_port = src_port;
	if (ctrlr->opts.transport_ack_timeout) {
		/* [한국어] TCP_USER_TIMEOUT (ms) — 사용자가 2^N ms로 지정. SSD 응답 지연 검출. */
		opts.ack_timeout = 1ULL << ctrlr->opts.transport_ack_timeout;
	}

	opts.connect_timeout = g_spdk_nvme_transport_opts.tcp_connect_timeout_ms;  /* [한국어] connect 자체의 timeout (ms). */

	if (sock_impl_name) {
		opts.impl_opts = &impl_opts;
		opts.impl_opts_size = sizeof(impl_opts);
	}

	nvme_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_SOCK_CONNECTING);  /* [한국어] 3-way handshake 진행 중. */
	/* [한국어] async connect — 즉시 반환, 완료는 콜백에서. */
	tqpair->sock = spdk_sock_connect_async(ctrlr->trid.traddr, port, sock_impl_name, &opts,
					       nvme_tcp_sock_connect_cb_fn, tqpair);
	if (!tqpair->sock) {
		NVME_TQPAIR_ERRLOG(tqpair, "sock connection error with addr=%s, port=%ld\n", ctrlr->trid.traddr,
				   port);
		rc = -1;
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * nvme_tcp_ctrlr_connect_qpair_poll - qpair 연결 상태머신을 한 step 진행 (비동기 polling 진입점)
 *
 * @ctrlr: 컨트롤러
 * @qpair: 큐페어
 * @return: 0 RUNNING 도달, -EAGAIN 진행 중, -ETIMEDOUT/음수 에러
 *
 * 상위 레이어가 큐페어 connect 후 RUNNING이 될 때까지 반복 호출.
 *
 * 상태 전이:
 *   SOCK_CONNECTING → -EAGAIN (sock callback 대기)
 *   INITIALIZING → ICReq/ICResp 핸드셰이크 대기 (타임아웃 검사)
 *                → 완료 시 send/resp 콜백이 자동으로 FABRIC_CONNECT_SEND로 전이
 *   FABRIC_CONNECT_SEND → Fabric CONNECT capsule 송신 → FABRIC_CONNECT_POLL로
 *   FABRIC_CONNECT_POLL → CONNECT 응답 polling
 *                       → 인증 필요하면 AUTHENTICATING으로
 *                       → 아니면 RUNNING + CONNECTED
 *   AUTHENTICATING → DH-CHAP 인증 polling → 완료 시 RUNNING + CONNECTED
 *   RUNNING → 0 반환
 *
 * 재진입 방지: in_connect_poll 비트 — fabric_qpair_connect_poll 안에서 read_pdu 호출 후
 *   다시 connect_qpair_poll로 들어오는 재귀를 차단.
 */
static int
nvme_tcp_ctrlr_connect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair;
	int rc;

	tqpair = nvme_tcp_qpair(qpair);

	/* Prevent this function from being called recursively, as it could lead to issues with
	 * nvme_fabric_qpair_connect_poll() if the connect response is received in the recursive
	 * call.
	 */
	if (qpair->in_connect_poll) {
		return -EAGAIN;  /* [한국어] 재진입 차단 — 다음 라운드에서 재시도. */
	}

	qpair->in_connect_poll = true;

	switch (tqpair->state) {
	case NVME_TCP_QPAIR_STATE_SOCK_CONNECTING:
		rc = -EAGAIN;  /* [한국어] sock_connect 콜백을 기다림. */
		break;
	case NVME_TCP_QPAIR_STATE_INITIALIZING:
		if (spdk_get_ticks() > tqpair->icreq_timeout_tsc) {
			/* [한국어] ICResp가 시간 안에 안 옴 → 타임아웃. 컨트롤러 미응답 또는 ICResp 손상. */
			NVME_TQPAIR_ERRLOG(tqpair, "Failed to construct qpair via correct icresp\n");
			rc = -ETIMEDOUT;
			break;
		}
		rc = -EAGAIN;
		break;
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND:
		/* [한국어] Fabric CONNECT capsule 송신 (nvme_fabric.c).
		 *  num_entries+1 = SQE 한도 (one slot empty 규칙). */
		rc = nvme_fabric_qpair_connect_async(&tqpair->qpair, tqpair->num_entries + 1);
		if (rc < 0) {
			NVME_TQPAIR_ERRLOG(tqpair, "Failed to send an NVMe-oF Fabric CONNECT command\n");
			break;
		}

		nvme_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL);
		rc = -EAGAIN;
		break;
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL:
		/* [한국어] CONNECT 응답 polling. 응답이 도착하면 0, 아직이면 -EAGAIN. */
		rc = nvme_fabric_qpair_connect_poll(&tqpair->qpair);
		if (rc == 0) {
			if (nvme_fabric_qpair_auth_required(qpair)) {
				/* [한국어] 컨트롤러가 인증 필요로 표시 → DH-CHAP 시작. */
				rc = nvme_fabric_qpair_authenticate_async(qpair);
				if (rc == 0) {
					nvme_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_AUTHENTICATING);
					rc = -EAGAIN;
				}
			} else {
				/* [한국어] 인증 불필요 → RUNNING으로. */
				nvme_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_RUNNING);
				nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
			}
		} else if (rc != -EAGAIN) {
			NVME_TQPAIR_ERRLOG(tqpair, "Failed to poll NVMe-oF Fabric CONNECT command\n");
		}
		break;
	case NVME_TCP_QPAIR_STATE_AUTHENTICATING:
		/* [한국어] DH-CHAP 인증 polling — 여러 capsule 교환 후 완료. */
		rc = nvme_fabric_qpair_authenticate_poll(qpair);
		if (rc == 0) {
			nvme_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_RUNNING);
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
		}
		break;
	case NVME_TCP_QPAIR_STATE_RUNNING:
		rc = 0;  /* [한국어] 이미 RUNNING — 정상 완료. */
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	qpair->in_connect_poll = false;
	return rc;
}

/*
 * [한국어]
 * nvme_tcp_ctrlr_connect_qpair - vtable: 큐페어 연결 시작 (sock + poll group + 통계 셋업)
 *
 * @ctrlr: 컨트롤러
 * @qpair: 큐페어
 * @return: 0 성공 (async — 완료는 connect_qpair_poll로 검사), 음수 에러
 *
 * 호출자: nvme_transport_ctrlr_connect_qpair → 이 함수.
 *
 * 동작:
 *   1) flags 0 클리어 (재연결 케이스 대비).
 *   2) poll group에 속하면 stats를 group과 공유 (shared_stats=true).
 *   3) sock이 없으면(첫 연결) qpair_connect_sock 호출.
 *      - admin이면 sock의 NUMA ID를 컨트롤러에 기록 (이후 io qpair 할당의 NUMA hint).
 *   4) poll group에 sock_group_add_sock으로 등록 → 이후 sock_group_poll이 자동 polling.
 *   5) standalone이면 자체 stats 할당 (재연결 시 보존).
 *   6) maxr2t = 기본값으로 초기화 (ICResp 전 보호).
 *   7) recv_state를 AWAIT_PDU_READY로 (재연결 시 잔여 상태 정리).
 */
static int
nvme_tcp_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc = 0;
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_poll_group *tgroup;

	tqpair = nvme_tcp_qpair(qpair);
	memset(&tqpair->flags, 0, sizeof(tqpair->flags));   /* [한국어] icreq_send_ack/icresp_received 등 재연결 대비 리셋. */

	if (qpair->poll_group) {
		tgroup = nvme_tcp_poll_group(qpair->poll_group);
		tqpair->stats = &tgroup->stats;                 /* [한국어] poll group 공유 stats. */
		tqpair->shared_stats = true;
	}

	if (!tqpair->sock) {
		rc = nvme_tcp_qpair_connect_sock(ctrlr, qpair);
		if (rc < 0) {
			return rc;
		}

		if (nvme_qpair_is_admin_queue(qpair)) {
			/* [한국어] admin sock의 NUMA ID를 컨트롤러에 기록 — 이후 io qpair 할당이 같은 NUMA로 가도록 hint. */
			ctrlr->numa.id_valid = 1;
			ctrlr->numa.id = spdk_sock_get_numa_id(tqpair->sock);
		}
	}

	if (qpair->poll_group) {
		tgroup = nvme_tcp_poll_group(qpair->poll_group);

		/* [한국어] sock을 poll group에 등록 → 이후 sock_group_poll이 이벤트 발생 시 sock_cb 호출. */
		rc = spdk_sock_group_add_sock(tgroup->sock_group, tqpair->sock, nvme_tcp_qpair_sock_cb, qpair);
		if (rc < 0) {
			NVME_TQPAIR_ERRLOG(tqpair, "spdk_sock_group_add_sock() failed, rc %d: %s\n", rc,
					   spdk_strerror(-rc));
			return rc;
		}
	} else {
		/* When resetting a controller, we disconnect adminq and then reconnect. The stats
		 * is not freed when disconnecting. So when reconnecting, don't allocate memory
		 * again.
		 */
		if (tqpair->stats == NULL) {
			tqpair->stats = calloc(1, sizeof(*tqpair->stats));  /* [한국어] standalone 자체 stats. */
			if (!tqpair->stats) {
				NVME_TQPAIR_ERRLOG(tqpair, "tcp stats memory allocation failed\n");
				return -ENOMEM;
			}
		}
	}

	tqpair->maxr2t = NVME_TCP_MAX_R2T_DEFAULT;          /* [한국어] ICResp 도착 전 보호값 — 1. */
	/* Explicitly set recv_state of tqpair */
	if (tqpair->recv_state != NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY) {
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	}

	return rc;
}

/*
 * [한국어]
 * nvme_tcp_ctrlr_create_qpair - 일반화된 qpair 생성 헬퍼 (admin/io 모두 사용)
 *
 * @ctrlr: 컨트롤러
 * @qid: queue id (0=admin, 1+=io)
 * @qsize: queue size (entries — num_entries는 qsize-1)
 * @qprio: 우선순위 (현재 TCP는 무시)
 * @num_requests: 위 레이어 nvme_request 풀 크기
 * @async: 비동기 모드 여부 (process_completions 콘텍스트가 외부 polling)
 * @return: 생성된 qpair, 실패 시 NULL
 *
 * 호출자: ctrlr_construct(admin), ctrlr_create_io_qpair.
 *
 * 동작:
 *   1) qsize 검증.
 *   2) tqpair 할당 + num_entries = qsize-1 (NVMe spec one-slot-empty 규칙).
 *   3) nvme_qpair_init: 공통 초기화 (cmd 큐, queued_req, cb, request 풀).
 *   4) nvme_tcp_alloc_reqs: tcp_req 풀 + send_pdus 풀 할당.
 */
static struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr,
			    uint16_t qid, uint32_t qsize,
			    enum spdk_nvme_qprio qprio,
			    uint32_t num_requests, bool async)
{
	struct nvme_tcp_qpair *tqpair;
	struct spdk_nvme_qpair *qpair;
	int rc;

	if (qsize < SPDK_NVME_QUEUE_MIN_ENTRIES) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to create qpair with size %u. Minimum queue size is %d.\n",
				  qsize, SPDK_NVME_QUEUE_MIN_ENTRIES);
		return NULL;
	}

	tqpair = calloc(1, sizeof(struct nvme_tcp_qpair));
	if (!tqpair) {
		NVME_CTRLR_ERRLOG(ctrlr, "failed to get create tqpair\n");
		return NULL;
	}

	/* Set num_entries one less than queue size. According to NVMe
	 * and NVMe-oF specs we can not submit queue size requests,
	 * one slot shall always remain empty.
	 */
	tqpair->num_entries = qsize - 1;            /* [한국어] NVMe spec: 큐 한 슬롯은 항상 비워둠. */
	qpair = &tqpair->qpair;
	rc = nvme_qpair_init(qpair, qid, ctrlr, qprio, num_requests, async);
	if (rc != 0) {
		free(tqpair);
		return NULL;
	}

	rc = nvme_tcp_alloc_reqs(tqpair);
	if (rc) {
		nvme_tcp_ctrlr_delete_io_qpair(ctrlr, qpair);  /* [한국어] alloc 실패 → 일반 정리 경로로. */
		return NULL;
	}

	return qpair;
}

/*
 * [한국어]
 * nvme_tcp_ctrlr_create_io_qpair - vtable: io qpair 생성 (사용자가 spdk_nvme_ctrlr_alloc_io_qpair 호출 시)
 *
 * @ctrlr: 컨트롤러
 * @qid: 새 큐의 id
 * @opts: 사용자 옵션 (io_queue_size, qprio, io_queue_requests, async_mode)
 * @return: 생성된 qpair, 실패 시 NULL
 *
 * 단순 wrapper — opts에서 값 추출해 create_qpair에 위임.
 */
static struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
			       const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_tcp_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					   opts->io_queue_requests, opts->async_mode);
}

/*
 * [한국어]
 * nvme_tcp_generate_tls_credentials - PSK 키링 → 유도된 TLS PSK + Identity 생성 (NVMe-oF TP 8011)
 *
 * @tctrlr: 대상 TCP 컨트롤러 확장
 * @return: 0 성공, 음수 errno
 *
 * NVMe-oF TLS PSK 키 유도 흐름 (TP 8011):
 *   1) Key ring에서 사용자 등록 PSK 가져오기 (interchange format Base64).
 *   2) interchange parse → configured PSK (32B SHA256 또는 48B SHA384) + retained hash 알고리즘.
 *   3) cipher suite 결정 (32B → AES_128_GCM_SHA256, 48B → AES_256_GCM_SHA384).
 *   4) psk_identity 문자열 생성 ("NVMe<base>HostNQN SubsystemNQN").
 *   5) retained PSK 유도 (HKDF로 hostnqn 기반): hash가 NONE이면 configured를 그대로 사용.
 *   6) TLS PSK 유도 (HKDF-Expand-Label, identity 기반).
 *   7) configured PSK는 메모리에서 보안 zeroize.
 *
 * 호출자: nvme_tcp_ctrlr_construct (opts.tls_psk가 비어있지 않을 때).
 * 결과는 tctrlr->{psk_identity, psk, psk_size, tls_cipher_suite}에 저장 → connect_sock에서 사용.
 */
static int
nvme_tcp_generate_tls_credentials(struct nvme_tcp_ctrlr *tctrlr)
{
	struct spdk_nvme_ctrlr *ctrlr = &tctrlr->ctrlr;
	int rc;
	uint8_t psk_retained[SPDK_TLS_PSK_MAX_LEN] = {};
	uint8_t psk_configured[SPDK_TLS_PSK_MAX_LEN] = {};
	uint8_t pskbuf[SPDK_TLS_PSK_MAX_LEN + 1] = {};
	uint8_t tls_cipher_suite;
	uint8_t psk_retained_hash;
	uint64_t psk_configured_size;

	/* [한국어] keyring API로 사용자 등록 키 가져오기 (Linux keyring 등). */
	rc = spdk_key_get_key(ctrlr->opts.tls_psk, pskbuf, SPDK_TLS_PSK_MAX_LEN);
	if (rc < 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to obtain key '%s': %s\n",
				  spdk_key_get_name(ctrlr->opts.tls_psk), spdk_strerror(-rc));
		goto finish;
	}

	/* [한국어] interchange format Base64 → binary configured PSK + hash 알고리즘 추출. */
	rc = nvme_tcp_parse_interchange_psk(pskbuf, psk_configured, sizeof(psk_configured),
					    &psk_configured_size, &psk_retained_hash);
	if (rc < 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to parse PSK interchange!\n");
		goto finish;
	}

	/* The Base64 string encodes the configured PSK (32 or 48 bytes binary).
	 * This check also ensures that psk_configured_size is smaller than
	 * psk_retained buffer size. */
	if (psk_configured_size == SHA256_DIGEST_LENGTH) {
		/* [한국어] 32B PSK → AES-128-GCM. */
		tls_cipher_suite = NVME_TCP_CIPHER_AES_128_GCM_SHA256;
		tctrlr->tls_cipher_suite = "TLS_AES_128_GCM_SHA256";
	} else if (psk_configured_size == SHA384_DIGEST_LENGTH) {
		/* [한국어] 48B PSK → AES-256-GCM. */
		tls_cipher_suite = NVME_TCP_CIPHER_AES_256_GCM_SHA384;
		tctrlr->tls_cipher_suite = "TLS_AES_256_GCM_SHA384";
	} else {
		NVME_CTRLR_ERRLOG(ctrlr, "Unrecognized cipher suite!\n");
		rc = -ENOTSUP;
		goto finish;
	}

	/* [한국어] PSK Identity 문자열 생성: "NVMe<base>HostNQN SubsystemNQN" 형태. TLS 핸드셰이크 SNI/PSK ID로 사용. */
	rc = nvme_tcp_generate_psk_identity(tctrlr->psk_identity, sizeof(tctrlr->psk_identity),
					    ctrlr->opts.hostnqn, ctrlr->trid.subnqn,
					    tls_cipher_suite);
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "could not generate PSK identity\n");
		goto finish;
	}

	/* No hash indicates that Configured PSK must be used as Retained PSK. */
	if (psk_retained_hash == NVME_TCP_HASH_ALGORITHM_NONE) {
		/* [한국어] hash 없음 → configured를 retained로 그대로. */
		assert(psk_configured_size < sizeof(psk_retained));
		memcpy(psk_retained, psk_configured, psk_configured_size);
		rc = psk_configured_size;
	} else {
		/* Derive retained PSK. */
		/* [한국어] retained PSK = HKDF(configured, hostnqn). */
		rc = nvme_tcp_derive_retained_psk(psk_configured, psk_configured_size, ctrlr->opts.hostnqn,
						  psk_retained, sizeof(psk_retained), psk_retained_hash);
		if (rc < 0) {
			NVME_CTRLR_ERRLOG(ctrlr, "Unable to derive retained PSK!\n");
			goto finish;
		}
	}

	/* [한국어] TLS PSK = HKDF-Expand-Label(retained PSK, identity). 이게 실제 TLS 1.3에서 사용될 키. */
	rc = nvme_tcp_derive_tls_psk(psk_retained, rc, tctrlr->psk_identity, tctrlr->psk,
				     sizeof(tctrlr->psk), tls_cipher_suite);
	if (rc < 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Could not generate TLS PSK!\n");
		goto finish;
	}

	tctrlr->psk_size = rc;
	rc = 0;
finish:
	/* [한국어] 보안: 임시 키 메모리를 zero (compiler가 최적화로 제거 못 하도록 spdk_memset_s 사용). */
	spdk_memset_s(psk_configured, sizeof(psk_configured), 0, sizeof(psk_configured));
	spdk_memset_s(pskbuf, sizeof(pskbuf), 0, sizeof(pskbuf));

	return rc;
}

/* We have to use the typedef in the function declaration to appease astyle. */
/* [한국어] astyle(코드 포매터)이 함수 시그니처의 const struct... 정렬을 잘못 잡는 문제 회피용 typedef. */
typedef struct spdk_nvme_ctrlr spdk_nvme_ctrlr_t;

/*
 * [한국어]
 * nvme_tcp_ctrlr_construct - vtable: TCP 컨트롤러 객체 생성 (admin qpair 생성 + TLS 자격 유도)
 *
 * @trid: 트랜스포트 식별자 (trtype=TCP, traddr, trsvcid, subnqn)
 * @opts: 사용자 컨트롤러 옵션 (tls_psk, hostnqn, admin_queue_size 등)
 * @devhandle: PCIe 호환 시그니처 (TCP에선 사용 안 함)
 * @return: 생성된 spdk_nvme_ctrlr 포인터, 실패 시 NULL
 *
 * 호출자: spdk_nvme_connect → nvme_transport_ctrlr_construct(trtype=TCP) → 이 함수.
 *
 * 동작:
 *   1) tctrlr 할당, opts/trid 복사.
 *   2) opts.tls_psk가 있으면 TLS credentials 유도 (TP 8011).
 *   3) transport_ack_timeout 상한 검증.
 *   4) 일반 ctrlr_construct (request 풀, NS, hotplug 등 공통 초기화).
 *   5) ACCEL_SEQUENCE_SUPPORTED 플래그 — 데이터 digest 오프로드 + COPY op 가능 표시.
 *   6) admin qpair (qid=0) 생성.
 *   7) process 등록 (single-process 모드는 0번).
 */
static spdk_nvme_ctrlr_t *
nvme_tcp_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
			 const struct spdk_nvme_ctrlr_opts *opts,
			 void *devhandle)
{
	struct nvme_tcp_ctrlr *tctrlr;
	int rc;

	tctrlr = calloc(1, sizeof(*tctrlr));
	if (tctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	tctrlr->ctrlr.opts = *opts;       /* [한국어] 사용자 옵션 복사 — 이후 트랜스포트가 모디파이 가능. */
	tctrlr->ctrlr.trid = *trid;       /* [한국어] trid 복사 — connect_sock에서 traddr/trsvcid 사용. */

	if (opts->tls_psk != NULL) {
		rc = nvme_tcp_generate_tls_credentials(tctrlr);  /* [한국어] PSK → TLS PSK 유도. */
		if (rc != 0) {
			free(tctrlr);
			return NULL;
		}
	}

	if (opts->transport_ack_timeout > NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT) {
		/* [한국어] 사용자가 너무 큰 값을 줬을 때 cap. */
		SPDK_NOTICELOG("transport_ack_timeout exceeds max value %d, use max value\n",
			       NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT);
		tctrlr->ctrlr.opts.transport_ack_timeout = NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT;
	}

	rc = nvme_ctrlr_construct(&tctrlr->ctrlr);   /* [한국어] 공통 초기화 (NS, request 풀, hotplug 등). */
	if (rc != 0) {
		free(tctrlr);
		return NULL;
	}

	/* Sequence might be used not only for data digest offload purposes but
	 * to handle a potential COPY operation appended as the result of translation. */
	/* [한국어] TCP는 accel sequence 지원 — 외부 메모리 → 시스템 메모리 COPY와 DDGST 가속 둘 다 지원. */
	tctrlr->ctrlr.flags |= SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED;
	/* [한국어] admin qpair 생성 (qid=0). admin은 항상 async 모드. */
	tctrlr->ctrlr.adminq = nvme_tcp_ctrlr_create_qpair(&tctrlr->ctrlr, 0,
			       tctrlr->ctrlr.opts.admin_queue_size, 0,
			       tctrlr->ctrlr.opts.admin_queue_size, true);
	if (!tctrlr->ctrlr.adminq) {
		NVME_CTRLR_ERRLOG(&tctrlr->ctrlr, "failed to create admin qpair\n");
		nvme_tcp_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	if (nvme_ctrlr_add_process(&tctrlr->ctrlr, 0) != 0) {
		/* [한국어] process 등록 실패 (multi-process pid_t=0 = primary). */
		NVME_CTRLR_ERRLOG(&tctrlr->ctrlr, "nvme_ctrlr_add_process() failed\n");
		nvme_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	return &tctrlr->ctrlr;
}

/*
 * [한국어]
 * nvme_tcp_ctrlr_get_max_xfer_size - vtable: 한 IO의 최대 전송 크기
 *
 * @ctrlr: 컨트롤러
 * @return: UINT32_MAX (TCP는 트랜스포트 차원에서 제한 없음)
 *
 * PCIe는 MDTS(NVMe Identify의 max data transfer size)를 따르지만, TCP는 PDU 분할로 임의 크기 가능.
 */
static uint32_t
nvme_tcp_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	/* TCP transport doesn't limit maximum IO transfer size. */
	return UINT32_MAX;
}

/*
 * [한국어]
 * nvme_tcp_ctrlr_get_max_sges - vtable: 최대 SGL fragment 수
 *
 * @ctrlr: 컨트롤러
 * @return: NVME_TCP_MAX_SGL_DESCRIPTORS (build_sgl_request의 iov[] 크기와 동일)
 */
static uint16_t
nvme_tcp_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	return NVME_TCP_MAX_SGL_DESCRIPTORS;
}

/*
 * [한국어]
 * nvme_tcp_qpair_iterate_requests - vtable: outstanding 요청들에 대해 콜백 호출 (iteration 헬퍼)
 *
 * @qpair: 큐페어
 * @iter_fn: 각 req에 적용할 콜백 (rc != 0이면 iteration 중단)
 * @arg: 콜백 인자
 * @return: 0 모두 성공, 콜백이 반환한 첫 음수
 *
 * 호출자: 사용자가 spdk_nvme_qpair_iterate_requests로 outstanding을 순회할 때.
 */
static int
nvme_tcp_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
				int (*iter_fn)(struct nvme_request *req, void *arg),
				void *arg)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct nvme_tcp_req *tcp_req, *tmp;
	int rc;

	assert(iter_fn != NULL);

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		assert(tcp_req->req != NULL);

		rc = iter_fn(tcp_req->req, arg);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

/*
 * [한국어]
 * nvme_tcp_qpair_authenticate - vtable: RUNNING 상태에서 사용자 요청 시 (재)인증 트리거
 *
 * @qpair: 큐페어
 * @return: 0 인증 시작됨, -ENOTCONN 잘못된 상태, 음수 에러
 *
 * 호출자: 사용자가 spdk_nvme_qpair_authenticate로 명시적 재인증 요청.
 *
 * 분기:
 *   - state < RUNNING: connecting 중 → 자동으로 connect_qpair_poll의 AUTHENTICATING으로 진입할 것.
 *   - state == RUNNING: 즉시 인증 시작.
 *   - 그 외: ENOTCONN.
 */
static int
nvme_tcp_qpair_authenticate(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	int rc;

	/* If the qpair is still connecting, it'll be forced to authenticate later on */
	if (tqpair->state < NVME_TCP_QPAIR_STATE_RUNNING) {
		return 0;  /* [한국어] connect_qpair_poll이 알아서 auth 진입할 것. */
	} else if (tqpair->state != NVME_TCP_QPAIR_STATE_RUNNING) {
		return -ENOTCONN;  /* [한국어] EXITING/EXITED는 인증 불가. */
	}

	rc = nvme_fabric_qpair_authenticate_async(qpair);
	if (rc == 0) {
		nvme_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_AUTHENTICATING);
		nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTING);  /* [한국어] 사용자 IO 일시 중단 표시. */
	}

	return rc;
}

/*
 * [한국어]
 * nvme_tcp_admin_qpair_abort_aers - vtable: admin 큐의 AER 명령들만 abort
 *
 * @qpair: admin qpair
 *
 * AER(Async Event Request)은 컨트롤러가 비동기 이벤트(예: 미디어 에러)를 보고할 때까지 대기하는
 * 영구 outstanding 명령. 컨트롤러 reset 전에 이 명령들만 따로 abort 처리해야 reset 후 재발사 가능.
 * 일반 io abort와 달리 print_on_error=false (에러 로그 안 띄움 — 정상 abort).
 */
static void
nvme_tcp_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_req *tcp_req, *tmp;
	struct spdk_nvme_cpl cpl = {};
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	cpl.sqid = qpair->id;
	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		assert(tcp_req->req != NULL);
		if (tcp_req->req->cmd.opc != SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			continue;  /* [한국어] AER만 대상 — 일반 admin 명령은 건드리지 않음. */
		}

		nvme_tcp_req_complete(tcp_req, tqpair, &cpl, false);   /* [한국어] print_on_error=false. */
	}
}

/*
 * [한국어]
 * nvme_tcp_poll_group_create - vtable: TCP poll group 객체 생성 (sock_group 포함)
 *
 * @return: 생성된 transport_poll_group base, 실패 시 NULL
 *
 * 호출자: spdk_nvme_poll_group_create + 첫 TCP qpair 추가.
 */
static struct spdk_nvme_transport_poll_group *
nvme_tcp_poll_group_create(void)
{
	struct nvme_tcp_poll_group *group = calloc(1, sizeof(*group));

	if (group == NULL) {
		SPDK_ERRLOG("Unable to allocate poll group.\n");
		return NULL;
	}

	TAILQ_INIT(&group->needs_poll);             /* [한국어] forward progress 강제 polling 리스트. */
	TAILQ_INIT(&group->timeout_enabled);        /* [한국어] timeout 검사 리스트. */

	group->sock_group = spdk_sock_group_create(group);  /* [한국어] sock 다중화 핸들 (epoll/kqueue 추상화). */
	if (group->sock_group == NULL) {
		free(group);
		SPDK_ERRLOG("Unable to allocate sock group.\n");
		return NULL;
	}

	return &group->group;
}

/*
 * [한국어]
 * nvme_tcp_poll_group_connect_qpair - vtable: poll group이 큐페어 연결 시 호출 (no-op)
 *
 * @qpair: 큐페어
 * @return: 항상 0
 *
 * 실제 연결은 ctrlr_connect_qpair → connect_sock에서 함. 이 후크는 다른 트랜스포트(RDMA)와의 인터페이스 통일용.
 */
static int
nvme_tcp_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

/*
 * [한국어]
 * nvme_tcp_poll_group_disconnect_qpair - vtable: poll group에서 큐페어 disconnect 시 sock 제거
 *
 * @qpair: 큐페어
 * @return: 0 성공, 음수 에러
 *
 * needs_poll에서 빼고, sock_group에서 sock 제거 → 이후 sock_group_poll이 이 sock을 더 이상 polling 안 함.
 */
static int
nvme_tcp_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(qpair->poll_group);
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	int rc;

	if (TAILQ_ENTRY_ENQUEUED(tqpair, link_poll)) {
		TAILQ_REMOVE_CLEAR(&group->needs_poll, tqpair, link_poll);
	}

	if (tqpair->sock && group->sock_group) {
		rc = spdk_sock_group_remove_sock(group->sock_group, tqpair->sock);
		if (rc < 0) {
			SPDK_ERRLOG("spdk_sock_group_remove_sock() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
			return -EPROTO;
		}
	}

	return 0;
}

/*
 * [한국어]
 * nvme_tcp_poll_group_add - vtable: 큐페어를 poll group에 등록 (no-op)
 *
 * @tgroup: poll group
 * @qpair: 큐페어
 * @return: 항상 0
 *
 * sock 등록은 connect_qpair에서 함 — 등록 시점에는 sock이 아직 없음.
 */
static int
nvme_tcp_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			struct spdk_nvme_qpair *qpair)
{
	/* The socket is disconnected when it is added to the poll group. We take no action
	 * until it is connected later. */

	return 0;
}

/*
 * [한국어]
 * nvme_tcp_poll_group_remove - vtable: 큐페어를 poll group에서 제거 (stats 끊기 + 보조 리스트 정리)
 *
 * @tgroup: poll group
 * @qpair: 큐페어 (이미 disconnected_qpairs에 있어야 함)
 * @return: 항상 0
 *
 * stats를 g_dummy_stats로 옮겨 dangling 방지 (poll_group_destroy 시 group->stats가 사라지므로).
 * needs_poll/timeout_enabled에서도 제거.
 */
static int
nvme_tcp_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
			   struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_poll_group *group;

	assert(qpair->poll_group_tailq_head == &tgroup->disconnected_qpairs);  /* [한국어] disconnected 상태에서만 호출 가능. */

	tqpair = nvme_tcp_qpair(qpair);
	group = nvme_tcp_poll_group(tgroup);

	assert(tqpair->shared_stats == true);  /* [한국어] poll group 큐페어는 항상 공유 stats 사용. */
	tqpair->stats = &g_dummy_stats;        /* [한국어] dangling 방지 — 더미로 redirect. */

	if (TAILQ_ENTRY_ENQUEUED(tqpair, link_poll)) {
		TAILQ_REMOVE_CLEAR(&group->needs_poll, tqpair, link_poll);
	}
	if (TAILQ_ENTRY_ENQUEUED(tqpair, link_timeout)) {
		TAILQ_REMOVE_CLEAR(&group->timeout_enabled, tqpair, link_timeout);
	}

	return 0;
}

/*
 * [한국어]
 * nvme_tcp_poll_group_process_completions - vtable: poll group의 메인 polling 진입점
 *
 * @tgroup: poll group
 * @completions_per_qpair: 큐페어당 처리할 최대 완료 수
 * @disconnected_qpair_cb: disconnect 완료된 큐페어를 호출자에게 통보할 콜백
 * @return: 누적 완료 수, 음수 에러
 *
 * 호출자: spdk_nvme_poll_group_process_completions (사용자 polling 루프).
 *
 * 동작:
 *   1) 라운드 시작 시 num_completions=0, polls++.
 *   2) spdk_sock_group_poll: epoll/kqueue로 sock 이벤트 수집 → 각 sock의 cb(=qpair_sock_cb) 호출 →
 *      qpair_sock_cb가 process_completions 위임 → group->num_completions에 누적.
 *   3) disconnected_qpairs 정리 — DISCONNECTING이고 outstanding 비면 done, DISCONNECTED는 사용자 콜백.
 *   4) needs_poll 강제 polling — sock 이벤트가 없어도 forward progress 보장.
 *   5) timeout_enabled 큐페어들 timeout 검사.
 *   6) idle_polls/socket_completions stats 누적.
 */
static int64_t
nvme_tcp_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
					uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(tgroup);
	struct spdk_nvme_qpair *qpair, *tmp_qpair;
	struct nvme_tcp_qpair *tqpair, *tmp_tqpair;
	int rc, num_events;

	group->completions_per_qpair = completions_per_qpair;  /* [한국어] sock_cb가 사용. */
	group->num_completions = 0;
	group->stats.polls++;

	/* [한국어] sock_group_poll: epoll_wait → 이벤트 발생한 sock에 대해 qpair_sock_cb 호출. */
	rc = spdk_sock_group_poll(group->sock_group);

	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
		tqpair = nvme_tcp_qpair(qpair);
		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
			if (TAILQ_EMPTY(&tqpair->outstanding_reqs)) {
				nvme_transport_ctrlr_disconnect_qpair_done(qpair);  /* [한국어] outstanding 비면 disconnect 완료. */
			}
		}
		/* Wait until the qpair transitions to the DISCONNECTED state, otherwise user might
		 * want to free it from disconnect_qpair_cb, while it's not fully disconnected (and
		 * might still have outstanding requests) */
		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTED) {
			disconnected_qpair_cb(qpair, tgroup->group->ctx);  /* [한국어] 사용자에게 disconnect 통보. */
		}
	}

	/* If any qpairs were marked as needing to be polled due to an asynchronous write completion
	 * and they weren't polled as a consequence of calling spdk_sock_group_poll above, poll them now. */
	/* [한국어] sock 이벤트 없이도 polling 필요한 큐페어들 (queued_req, INITIALIZING, FABRIC_CONNECT_POLL). */
	TAILQ_FOREACH_SAFE(tqpair, &group->needs_poll, link_poll, tmp_tqpair) {
		nvme_tcp_qpair_sock_cb(&tqpair->qpair, group->sock_group, tqpair->sock);
	}

	TAILQ_FOREACH_SAFE(tqpair, &group->timeout_enabled, link_timeout, tmp_tqpair) {
		qpair = &tqpair->qpair;
		assert(qpair->ctrlr->timeout_enabled);
		nvme_tcp_qpair_check_timeout(qpair);  /* [한국어] outstanding 요청들의 만료 검사. */
	}

	if (spdk_unlikely(rc < 0)) {
		SPDK_ERRLOG("spdk_sock_group_poll() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		return rc;
	}

	num_events = rc;
	group->stats.idle_polls += !num_events;          /* [한국어] 이벤트 0 → idle. */
	group->stats.socket_completions += num_events;
	return group->num_completions;
}

/*
 * Handle disconnected qpairs when interrupt support gets added.
 */
/*
 * [한국어]
 * nvme_tcp_poll_group_check_disconnected_qpairs - vtable: 인터럽트 모드용 hook (현재 no-op)
 *
 * @tgroup: poll group
 * @disconnected_qpair_cb: 콜백
 *
 * SPDK가 미래에 인터럽트 모드를 추가했을 때 사용할 자리. 현재는 polling만 지원하므로 빈 함수.
 */
static void
nvme_tcp_poll_group_check_disconnected_qpairs(struct spdk_nvme_transport_poll_group *tgroup,
		spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
}

/*
 * [한국어]
 * nvme_tcp_poll_group_destroy - vtable: poll group 객체 해제
 *
 * @tgroup: poll group
 * @return: 0 성공, -EBUSY 큐페어가 남아있음
 *
 * 큐페어가 모두 빠졌는지 검증 후 sock_group close + free.
 */
static int
nvme_tcp_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	int rc;
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(tgroup);

	if (!STAILQ_EMPTY(&tgroup->connected_qpairs) || !STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
		return -EBUSY;  /* [한국어] 사용 중인 큐페어가 남아있으면 destroy 불가. */
	}

	rc = spdk_sock_group_close(&group->sock_group);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_group_close() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
		assert(false);  /* [한국어] sock_group_close 실패는 SPDK 내부 일관성 문제. */
	}

	free(tgroup);
	return 0;
}

/*
 * [한국어]
 * nvme_tcp_poll_group_get_stats - vtable: poll group 통계 복사 반환 (RPC 노출용)
 *
 * @tgroup: poll group
 * @_stats: out — 새로 calloc된 stats 포인터 (호출자가 free_stats로 반환)
 * @return: 0 성공, 음수 에러
 */
static int
nvme_tcp_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
			      struct spdk_nvme_transport_poll_group_stat **_stats)
{
	struct nvme_tcp_poll_group *group;
	struct spdk_nvme_transport_poll_group_stat *stats;

	if (tgroup == NULL || _stats == NULL) {
		SPDK_ERRLOG("Invalid stats or group pointer\n");
		return -EINVAL;
	}

	group = nvme_tcp_poll_group(tgroup);

	stats = calloc(1, sizeof(*stats));
	if (!stats) {
		SPDK_ERRLOG("Can't allocate memory for TCP stats\n");
		return -ENOMEM;
	}
	stats->trtype = SPDK_NVME_TRANSPORT_TCP;       /* [한국어] union 디스크리미네이터 — TCP 분기 표시. */
	memcpy(&stats->tcp, &group->stats, sizeof(group->stats));  /* [한국어] live stats 스냅샷 복사. */

	*_stats = stats;

	return 0;
}

/*
 * [한국어]
 * nvme_tcp_poll_group_free_stats - vtable: get_stats가 반환한 객체 해제
 *
 * @tgroup: poll group (사용 안 함)
 * @stats: 해제할 객체
 */
static void
nvme_tcp_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
			       struct spdk_nvme_transport_poll_group_stat *stats)
{
	free(stats);
}

/*
 * [한국어]
 * nvme_tcp_ctrlr_get_memory_domains - vtable: 컨트롤러가 지원하는 메모리 도메인 목록 반환
 *
 * @ctrlr: 컨트롤러 (사용 안 함 — TCP는 항상 시스템 도메인)
 * @domains: out — 반환할 도메인 배열 (NULL이면 카운트만)
 * @array_size: 배열 크기
 * @return: 항상 1 (TCP는 시스템 도메인 1개만 지원)
 *
 * RDMA는 RDMA-pinned 도메인을 추가로 지원하지만, TCP는 일반 시스템 메모리만.
 * 외부 도메인(GPU 등)에서 IO 시 try_memory_translation으로 시스템 도메인으로 변환.
 */
static int
nvme_tcp_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				  struct spdk_memory_domain **domains, int array_size)
{
	if (domains && array_size > 0) {
		domains[0] = spdk_memory_domain_get_system_domain();
	}

	return 1;
}

/*
 * [한국어] TCP 트랜스포트 vtable — nvme_transport.c의 디스패처가 trtype=TCP인 컨트롤러에 대해
 *  이 테이블의 함수 포인터들을 호출한다. 모든 트랜스포트(PCIe/RDMA/TCP)는 동일한 인터페이스 구현.
 *
 *  주요 분류:
 *   - ctrlr_*: 컨트롤러 라이프사이클 (construct/destruct/enable/scan).
 *   - ctrlr_set_reg_*/get_reg_*: NVMe 컨트롤러 레지스터 읽기/쓰기.
 *     PCIe는 MMIO BAR로 직접 접근하지만, TCP/RDMA는 Fabric Property R/W capsule로 위임 → nvme_fabric.c.
 *   - ctrlr_create_io_qpair / connect_qpair / disconnect_qpair / delete_io_qpair: qpair 라이프사이클.
 *   - qpair_*: qpair 단위 동작 (abort, reset, submit, process_completions, iterate, authenticate).
 *   - poll_group_*: poll group (sock 다중화) 관련.
 */
const struct spdk_nvme_transport_ops tcp_ops = {
	.name = "TCP",                           /* [한국어] trid 파싱 시 trtype 문자열 매칭. */
	.type = SPDK_NVME_TRANSPORT_TCP,          /* [한국어] enum 값 — 빠른 비교용. */
	.ctrlr_construct = nvme_tcp_ctrlr_construct,
	.ctrlr_scan = nvme_fabric_ctrlr_scan,    /* [한국어] discovery — Fabric 공통 (nvme_fabric.c). */
	.ctrlr_destruct = nvme_tcp_ctrlr_destruct,
	.ctrlr_enable = nvme_tcp_ctrlr_enable,   /* [한국어] no-op — TCP는 enable 단계 X. */

	/* [한국어] 컨트롤러 레지스터 R/W는 모두 Fabric Property R/W capsule로 위임. */
	.ctrlr_set_reg_4 = nvme_fabric_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_fabric_ctrlr_set_reg_8,
	.ctrlr_get_reg_4 = nvme_fabric_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_fabric_ctrlr_get_reg_8,
	.ctrlr_set_reg_4_async = nvme_fabric_ctrlr_set_reg_4_async,
	.ctrlr_set_reg_8_async = nvme_fabric_ctrlr_set_reg_8_async,
	.ctrlr_get_reg_4_async = nvme_fabric_ctrlr_get_reg_4_async,
	.ctrlr_get_reg_8_async = nvme_fabric_ctrlr_get_reg_8_async,

	.ctrlr_get_max_xfer_size = nvme_tcp_ctrlr_get_max_xfer_size,
	.ctrlr_get_max_sges = nvme_tcp_ctrlr_get_max_sges,

	.ctrlr_create_io_qpair = nvme_tcp_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_tcp_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_tcp_ctrlr_connect_qpair,
	.ctrlr_disconnect_qpair = nvme_tcp_ctrlr_disconnect_qpair,

	.ctrlr_get_memory_domains = nvme_tcp_ctrlr_get_memory_domains,

	.qpair_abort_reqs = nvme_tcp_qpair_abort_reqs,
	.qpair_reset = nvme_tcp_qpair_reset,    /* [한국어] no-op — TCP qpair는 reset 의미 X. */
	.qpair_submit_request = nvme_tcp_qpair_submit_request,
	.qpair_process_completions = nvme_tcp_qpair_process_completions,
	.qpair_iterate_requests = nvme_tcp_qpair_iterate_requests,
	.qpair_authenticate = nvme_tcp_qpair_authenticate,
	.admin_qpair_abort_aers = nvme_tcp_admin_qpair_abort_aers,

	.poll_group_create = nvme_tcp_poll_group_create,
	.poll_group_connect_qpair = nvme_tcp_poll_group_connect_qpair,
	.poll_group_disconnect_qpair = nvme_tcp_poll_group_disconnect_qpair,
	.poll_group_add = nvme_tcp_poll_group_add,
	.poll_group_remove = nvme_tcp_poll_group_remove,
	.poll_group_process_completions = nvme_tcp_poll_group_process_completions,
	.poll_group_check_disconnected_qpairs = nvme_tcp_poll_group_check_disconnected_qpairs,
	.poll_group_destroy = nvme_tcp_poll_group_destroy,
	.poll_group_get_stats = nvme_tcp_poll_group_get_stats,
	.poll_group_free_stats = nvme_tcp_poll_group_free_stats,
};

/* [한국어] vtable을 nvme_transport.c의 전역 TAILQ에 등록. main() 진입 전 constructor로 자동 실행.
 *  내부적으로 __attribute__((constructor))를 사용해 nvme_transport_register(&tcp_ops)를 호출.
 *  이후 trid에 trtype=TCP가 들어 있는 컨트롤러는 모두 tcp_ops를 사용. */
SPDK_NVME_TRANSPORT_REGISTER(tcp, &tcp_ops);

/*
 * [한국어]
 * nvme_tcp_trace - SPDK trace 시스템에 TCP 트랜스포트 트레이스 포인트 등록
 *
 * SPDK_TRACE_REGISTER_FN 매크로(별도 정의)에 의해 main() 진입 전 constructor로 호출됨.
 *
 * 등록 항목:
 *   - SUBMIT 트레이스: qid, ctx, cid, opc, dw10/11/12, qd 인자.
 *   - COMPLETE 트레이스: qid, ctx, cid, cpl, qd 인자.
 *   - 관계: SOCK_REQ_QUEUE/PEND/COMPLETE 트레이스가 NVME_TCP_REQ 객체와 연관됨을 등록 →
 *     spdk_trace_viewer가 PDU의 sock 단계와 NVMe req 라이프타임을 함께 표시.
 *
 * OBJECT_NVME_TCP_REQ는 'p' (point) 모양, OWNER_TYPE_NVME_TCP_QP는 'q' (queue) 모양으로 시각화.
 */
static void
nvme_tcp_trace(void)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"NVME_TCP_SUBMIT", TRACE_NVME_TCP_SUBMIT,
			OWNER_TYPE_NVME_TCP_QP, OBJECT_NVME_TCP_REQ, 1,  /* [한국어] new_object=1 — req 라이프타임 시작점. */
			{	{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "cid", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "opc", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "dw10", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "dw11", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "dw12", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }
			}
		},
		{
			"NVME_TCP_COMPLETE", TRACE_NVME_TCP_COMPLETE,
			OWNER_TYPE_NVME_TCP_QP, OBJECT_NVME_TCP_REQ, 0,  /* [한국어] new_object=0 — 라이프타임 종점. */
			{	{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "cid", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "cpl", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }
			}
		},
	};

	spdk_trace_register_object(OBJECT_NVME_TCP_REQ, 'p');     /* [한국어] viewer에서 'p' 모양으로 그림. */
	spdk_trace_register_owner_type(OWNER_TYPE_NVME_TCP_QP, 'q');  /* [한국어] qpair는 'q' 모양. */
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));

	/* [한국어] sock layer 트레이스를 NVMe req 객체에 연결 — viewer가 한 req의 sock 단계를 함께 표시. */
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_QUEUE, OBJECT_NVME_TCP_REQ, 0);
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_PEND, OBJECT_NVME_TCP_REQ, 0);
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_COMPLETE, OBJECT_NVME_TCP_REQ, 0);
}
/* [한국어] nvme_tcp_trace 함수를 트레이스 시스템에 등록하는 매크로.
 *  - 첫 인자 nvme_tcp_trace: 등록할 함수 (위에서 정의).
 *  - "nvme_tcp": 트레이스 그룹 이름 (사용자가 spdk_trace 활성화 시 이 이름으로 enable).
 *  - TRACE_GROUP_NVME_TCP: 그룹 ID (spdk_internal/trace_defs.h 정의).
 *  매크로 전개: __attribute__((constructor))로 main() 진입 전 자동 호출되어 nvme_tcp_trace()가 실행됨. */
SPDK_TRACE_REGISTER_FN(nvme_tcp_trace, "nvme_tcp", TRACE_GROUP_NVME_TCP)
