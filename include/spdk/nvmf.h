/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2025, Oracle and/or its affiliates.
 */

/** \file
 * NVMe over Fabrics target public API
 */

/*
 * [한국어 설명] NVMe-over-Fabrics target 공개 API (nvmf.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 가 NVMe-oF target(=서버) 측 으로 동작할 때 외부 애플리케이션이 호출할 수
 * 있는 1차 공개 API 의 전부를 모은 파일이다. SPDK 가 PCIe NVMe SSD 들을 자기 bdev 로
 * 흡수한 뒤, RDMA/TCP/FC 와이어 위에 NVMe-oF 표준(NVM Express over Fabrics 1.x/2.x) 으로
 * "원격 NVMe controller" 처럼 export 하는 모든 동작이 이 한 헤더의 함수로 노출된다.
 * 구체적으로 (1) target 인스턴스 lifecycle (create/destroy/listen), (2) subsystem CRUD
 * (NQN 단위 컨테이너), (3) namespace 추가/제거(bdev → ns 매핑), (4) listener / host /
 * referral / discovery 정책, (5) ANA(Asymmetric Namespace Access) multipath 상태 제어,
 * (6) NVMe Reservation, (7) DH-HMAC-CHAP 인증과 TLS PSK, (8) per-thread poll group lifecycle,
 * (9) transport 인스턴스 생성, (10) RPC 의 write_config_json 산출까지 — 사실상 lib/nvmf
 * 의 모든 외부 함수 표면을 정의한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 응용(예: app/nvmf_tgt) 또는 RPC 서버(JSON-RPC) → 본 헤더의 spdk_nvmf_*() 호출 →
 * lib/nvmf/{nvmf,subsystem,ctrlr,transport,...}.c → spdk_nvmf_transport_ops vtable
 * (nvmf_transport.h) → 와이어 모듈(rdma.c/tcp.c/fc.c/vfio_user.c) → 호스트(initiator).
 * 호출 컨텍스트는 두 가지로 명확히 분리된다:
 *   (a) "app thread" — 모든 RPC/관리 함수(create/destroy/add_ns/add_listener 등) 가 SPDK 의
 *       메인 스레드(spdk_app_start 가 만든 첫 thread) 에서 직렬 실행되어야 한다.
 *   (b) "poll group thread" — qpair lifecycle / I/O 처리 / poll_group_dump_stat 은 해당
 *       poll group 이 속한 spdk_thread 에서 호출되어야 한다.
 * cross-thread 호출 시에는 spdk_thread_send_msg() 로 위임한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/env.h(spdk_env / NUMA), spdk/nvme.h(spdk_nvme_transport_id, ana_state),
 *   spdk/nvmf_spec.h(NQN, subtype, fabric capsule 정의), spdk/queue.h(STAILQ/TAILQ),
 *   spdk/uuid.h(spdk_uuid).
 * - 사용처: app/nvmf_tgt/, lib/event/subsystems/nvmf/, RPC handlers (JSON), 외부 사용자
 *   애플리케이션, unit/integration 테스트.
 * - 데이터 흐름: 호스트가 capsule 보냄 → 트랜스포트 디코드 → 이 헤더의 객체 그래프
 *   (tgt → subsystem → ctrlr → qpair → request) 위에서 처리 → backing bdev → 응답 회신.
 * - 핵심 객체 그래프:
 *     spdk_nvmf_tgt
 *       ├ subsystems[NQN]: spdk_nvmf_subsystem
 *       │    ├ namespaces[nsid]: spdk_nvmf_ns → spdk_bdev
 *       │    ├ listeners[]: spdk_nvmf_subsystem_listener → trid
 *       │    ├ hosts[]: spdk_nvmf_host (allowed NQN + DH-HMAC-CHAP key)
 *       │    └ ctrlrs[host_nqn]: spdk_nvmf_ctrlr
 *       │           └ qpairs[qid]: spdk_nvmf_qpair
 *       │                  └ outstanding: spdk_nvmf_request
 *       ├ transports[]: spdk_nvmf_transport (RDMA/TCP/FC/...)
 *       ├ poll_groups[]: spdk_nvmf_poll_group (per-thread)
 *       └ referrals[]: spdk_nvmf_referral (Discovery referral)
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_nvmf_tgt_create/destroy: target 전역 인스턴스 lifecycle.
 * - spdk_nvmf_tgt_listen_ext / stop_listen: trid 별 wire listen 시작/정지.
 * - spdk_nvmf_subsystem_create/destroy/start/stop/pause/resume: NQN 컨테이너 CRUD + 상태 머신.
 * - spdk_nvmf_subsystem_add_ns_ext / remove_ns: bdev 를 namespace 로 export.
 * - spdk_nvmf_subsystem_add_listener_ext / remove_listener: subsystem 이 어느 trid 에서
 *   visible 한가 결정.
 * - spdk_nvmf_subsystem_add_host_ext / remove_host: 허용 hostNQN 명단 + DH-HMAC-CHAP 키.
 * - spdk_nvmf_subsystem_set_ana_state: multipath ANA(Optimized/Non-optimized/Inaccessible/...)
 *   상태 변경.
 * - spdk_nvmf_poll_group_create/destroy/add: per-thread polling group.
 * - spdk_nvmf_qpair_disconnect: qpair 비동기 종료.
 * - spdk_nvmf_transport_create_async / destroy / listen / stop_listen: 트랜스포트 인스턴스
 *   lifecycle.
 * - spdk_nvmf_ctrlr_set_dhchap_key/host_key: DH-HMAC-CHAP 인증 키 관리.
 * - spdk_nvmf_subsystem_set_keys: 호스트 NQN 별 키 갱신.
 * - spdk_nvmf_send_discovery_log_notice: Discovery AEN(Async Event Notice) 송신.
 * - spdk_nvmf_target_opts / spdk_nvmf_transport_opts / spdk_nvmf_listen_opts /
 *   spdk_nvmf_ns_opts / spdk_nvmf_host_opts: 모든 lifecycle API 의 옵션 구조체.
 * - spdk_nvmf_referral / referral_*: Discovery referral (다른 discovery 서비스 redirect).
 * - spdk_nvmf_ns_reservation_ops: NVMe Reservation 의 영속성(PTPL) 정책 후크.
 *
 * 핵심 도메인 개념(상세):
 * 1) NQN: NVMe Qualified Name. subsystem 식별자. 예 nqn.2016-06.io.spdk:cnode1.
 * 2) Subsystem 종류: Discovery(접속점, log page 만 제공) vs NVMe(I/O 가능).
 * 3) 다중 host → 다중 ctrlr 인스턴스: 한 subsystem 에 여러 host 가 connect 하면 host 별로
 *    별개 spdk_nvmf_ctrlr 가 생성된다(NSID/AER/host_id 분리).
 * 4) ANA: Asymmetric Namespace Access — optimized/non-optimized/inaccessible/persistent_loss/
 *    change 의 5 상태로 multi-path host 가 path 선택을 한다. anagrpid 단위로 ns 묶음 관리.
 * 5) Reservation: NVMe 6.x Reservation Type/Scope, register/release/acquire/clear, PTPL
 *    (Persist Through Power Loss) 옵션.
 * 6) Capsule 처리 흐름: request → SQE 디코드 → bdev I/O → completion → CQE 회신.
 * 7) poll group fan-in: 한 poll_group 이 여러 qpair 를 polling, 각 transport 별 sub-group.
 * 8) trtype/adrfam/trsvcid: trid 컴포넌트 (RDMA, TCP, IPv4/6, port).
 * 9) Listener "associate": subsystem 이 어느 listener 에서 visible 한지 정책.
 * 10) Auth (DH-HMAC-CHAP): NVMe-oF 인증, target/host 양쪽 PSK key + controller key.
 * 11) TLS PSK: NVMe TCP 1.0a, RFC 5705 EXPORTER 기반의 pre-shared key.
 */

#ifndef SPDK_NVMF_H
#define SPDK_NVMF_H

#include "spdk/stdinc.h"
/* [한국어] 표준 C 헤더 묶음 (size_t/uint*_t/bool/string 등). 이 헤더의 거의 모든 API 가 사용. */

#include "spdk/env.h"
/* [한국어] SPDK 환경 추상화 — DPDK EAL 위 hugepage/PCI/NUMA. spdk_env_get_socket_id 등을
 * 본 모듈의 listener/transport 가 NUMA-locality 결정에 활용. */

#include "spdk/nvme.h"
/* [한국어] 호스트 측 NVMe 드라이버 공개 API. 본 헤더는 그 중 spdk_nvme_transport_id /
 * spdk_nvme_ana_state / spdk_nvme_cdata_oncs / fuses 등 공유 enum/구조체를 사용. */

#include "spdk/nvmf_spec.h"
/* [한국어] NVMe-oF wire spec — NQN 길이, subtype enum, fabric capsule field 정의. */

#include "spdk/queue.h"
/* [한국어] BSD-style 매크로 큐(STAILQ/TAILQ) — 내부적으로 list 자료구조에 사용. */

#include "spdk/uuid.h"
/* [한국어] spdk_uuid — namespace UUID 식별자. NGUID/EUI64 와 함께 ns 식별. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 컴파일러가 본 헤더 include 시 C linkage 강제. */
#endif

#define NVMF_TGT_NAME_MAX_LENGTH	256
/* [한국어] target 이름 문자열 길이 상한(NUL 포함). spdk_nvmf_target_opts.name 에 사용.
 * 한 프로세스에서 여러 target 을 만들 때 이름으로 lookup. */

#define SPDK_TLS_PSK_MAX_LEN		200
/* [한국어] NVMe TCP TLS pre-shared key 의 최대 길이(byte). RFC 8446 / NVMe TCP 1.0a 의
 * PSK 길이 + 식별자 영역을 포함. */

struct spdk_nvmf_tgt;
/* [한국어] 전방선언 — NVMe-oF target 인스턴스. 정의는 lib/nvmf/nvmf_internal.h. 사용자는
 * 포인터로만 다루며 spdk_nvmf_tgt_create() 가 반환. */

struct spdk_nvmf_subsystem;
/* [한국어] 전방선언 — NQN 단위 subsystem(NVMe controller 패키지). */

struct spdk_nvmf_ctrlr;
/* [한국어] 전방선언 — host 1 명에 매핑된 controller 인스턴스. 여러 host 가 connect 하면
 * 같은 subsystem 안에 여러 ctrlr 가 공존한다. */

struct spdk_nvmf_qpair;
/* [한국어] 전방선언 — admin/IO Queue Pair. 정의는 nvmf_transport.h. */

struct spdk_nvmf_request;
/* [한국어] 전방선언 — capsule + 응답 + iov 를 묶은 1 급 요청 객체. 정의는 nvmf_transport.h. */

struct spdk_bdev;
/* [한국어] 전방선언 — SPDK 블록 디바이스. namespace 의 backing 장치. */

struct spdk_nvmf_request;
/* [한국어] (중복) request 전방선언. */

struct spdk_nvmf_host;
/* [한국어] 전방선언 — subsystem/referral 이 허용한 host 1 명. NQN + key 를 보유. */

struct spdk_nvmf_subsystem_listener;
/* [한국어] 전방선언 — subsystem 이 associate 된 listener 1 개의 메타. trid + ANA state. */

struct spdk_nvmf_referral;
/* [한국어] 전방선언 — Discovery 서비스 referral. 다른 discovery 서버로 호스트를 안내. */

struct spdk_nvmf_poll_group;
/* [한국어] 전방선언 — per-thread polling group. 정의는 nvmf_transport.h. */

struct spdk_json_write_ctx;
/* [한국어] 전방선언 — JSON 직렬화 컨텍스트(write_config_json 등에서 사용). */

struct spdk_json_val;
/* [한국어] 전방선언 — JSON 토큰 값(파싱된 RPC 입력). transport_specific 옵션 등에서 사용. */

struct spdk_nvmf_transport;
/* [한국어] 전방선언 — 트랜스포트 인스턴스. 정의는 nvmf_transport.h. */

/**
 * Specify filter rules which are applied during discovery log generation.
 */
/*
 * [한국어]
 * enum spdk_nvmf_tgt_discovery_filter - Discovery Log Page 생성 시 listener 필터 정책.
 *
 * 호스트가 Discovery 명령으로 어떤 listener 들을 알 수 있는지 결정. 비트마스크로 조합.
 * 보안/멀티테넌트 환경에서 호스트가 다른 트랜스포트의 endpoint 를 보지 못하게 막을 때 사용.
 */
enum spdk_nvmf_tgt_discovery_filter {
	/** Log all listeners in discovery log page */
	SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY = 0,
	/* [한국어] 모든 listener 노출(필터 없음). 가장 관대한 정책 — 디버그/단일 테넌트 환경.
	 * 설정자: spdk_nvmf_target_opts.discovery_filter. 읽는 자: discovery 응답 생성 코드.
	 * 값 범위: 0(다른 비트와 OR 시 효과 없음). */

	SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE = 1u << 0u,
	/* [한국어] 호스트가 Discovery 명령을 보낸 트랜스포트(RDMA/TCP/...) 와 동일 타입의
	 * listener 만 노출. 즉 RDMA 호스트는 RDMA endpoint 만 본다.
	 * 설정자/읽는 자: 위와 동일. 동기화: target init 후 read-only. */

	SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS = 1u << 1u,
	/* [한국어] 동일 traddr(IP 등) 의 listener 만 노출. multi-NIC 환경에서 한 NIC 만 보이게. */

	SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID = 1u << 2u,
	/* [한국어] 동일 trsvcid(port) 의 listener 만 노출. */

	SPDK_NVMF_TGT_DISCOVERY_MATCH_CUSTOM = 1u << 3u
	/* [한국어] 사용자 정의 필터 함수 사용 — spdk_nvmf_set_custom_discovery_filter() 로
	 * 등록한 콜백이 listener 별로 호출되어 true/false 반환. */
};

/*
 * [한국어]
 * spdk_nvmf_custom_discovery_filter - 사용자 정의 discovery 필터 콜백 시그니처.
 *
 * @listener_trid: discovery 응답 후보가 되는 listener 의 trid.
 * @discovery_cmd_source_trid: discovery 명령을 보낸 호스트의 source trid.
 * @return: true 면 응답에 포함, false 면 숨김.
 *
 * 호출 컨텍스트: discovery 명령 처리 중인 spdk_thread (보통 admin qpair 의 thread).
 */
typedef bool (*spdk_nvmf_custom_discovery_filter)(
	const struct spdk_nvme_transport_id *listener_trid,
	const struct spdk_nvme_transport_id *discovery_cmd_source_trid);

/*
 * [한국어]
 * struct spdk_nvmf_target_opts - target 생성 옵션 구조체. ABI-versioned (size 필드).
 */
struct spdk_nvmf_target_opts {
	size_t		size;
	/* [한국어] caller 가 전달하는 본 구조체의 sizeof. 라이브러리는 이 값으로 어디까지
	 * 필드가 유효한지 판단(ABI forward-compat). 새 필드는 끝에 추가. */

	char		name[NVMF_TGT_NAME_MAX_LENGTH];
	/* [한국어] target 이름. spdk_nvmf_get_tgt(name) 의 lookup 키. 동일 이름 중복 금지.
	 * 설정자: 사용자(RPC). 읽는 자: spdk_nvmf_tgt_get_name. */

	uint32_t	max_subsystems;
	/* [한국어] 이 target 이 보유 가능한 subsystem(NQN) 수의 상한. 0 = 라이브러리 기본.
	 * 코어가 이 값으로 subsystem 배열 크기를 미리 잡음. */

	uint16_t	crdt[3];
	/* [한국어] Command Retry Delay Times (NVMe 1.4) — 호스트가 retry 사이 대기할 시간 100ms 단위.
	 * 3 슬롯. NVMe spec 의 controller cdata 의 CRDT1/2/3 와 매핑.
	 * 동기화: target init 후 read-only. */

	uint32_t	discovery_filter;
	/* [한국어] enum spdk_nvmf_tgt_discovery_filter 의 비트마스크. 위 enum 참조.
	 * 설정자: target 생성 시. */

	uint32_t	dhchap_digests;
	/* [한국어] 허용 DH-HMAC-CHAP digest 알고리즘 비트마스크 (SHA-256/384/512 등의 비트 OR).
	 * 호스트가 다른 알고리즘을 제안하면 협상 실패. 설정자: target 생성 시. */

	uint32_t	dhchap_dhgroups;
	/* [한국어] 허용 DH group 비트마스크 (NULL/2048-MODP/3072-MODP/...). NVMe spec Figure DH-HMAC-CHAP. */
};

/*
 * [한국어]
 * struct spdk_nvmf_transport_opts - 트랜스포트 인스턴스 생성 시 옵션. ABI-versioned (opts_size).
 */
struct spdk_nvmf_transport_opts {
	uint16_t	max_queue_depth;
	/* [한국어] 한 qpair 의 최대 outstanding 명령 수. NVMe sqsize+1 과 매핑. 호스트의
	 * Connect cmd 가 더 큰 값을 요청하면 본 값으로 클램프. */

	uint16_t	max_qpairs_per_ctrlr;
	/* [한국어] 한 controller 가 가질 수 있는 qpair 수 상한(admin 1 + IO N).
	 * 호스트가 IO 큐를 더 요청하면 거부. */

	uint32_t	in_capsule_data_size;
	/* [한국어] in-capsule data 영역 크기(byte). 호스트의 작은 write 페이로드를 SQE 캡슐 안에
	 * 함께 실어 보내는 NVMe-oF 최적화. 0 이면 비활성. */

	/* used to calculate mdts */
	uint32_t	max_io_size;
	/* [한국어] 한 NVMe IO 명령의 최대 데이터 크기. Identify ctrlr 의 MDTS 계산에 사용.
	 * 호스트는 이 값을 보고 더 큰 IO 를 분할 전송. */

	uint32_t	io_unit_size;
	/* [한국어] iobuf pool 의 단위 버퍼 크기. max_io_size 가 io_unit_size 의 N 배일 때 N 개
	 * 버퍼로 SGL 구성. 작게 잡으면 메모리 효율, 크게 잡으면 SGL 길이 단축. */

	uint32_t	max_aq_depth;
	/* [한국어] admin qpair 의 최대 깊이. NVMe spec 상 admin 큐는 IO 큐와 별개 한도. */

	uint32_t	num_shared_buffers;
	/* [한국어] 트랜스포트가 공유하는 iobuf 데이터 버퍼 수. polled-mode 의 lockless 풀 크기. */

	uint32_t	buf_cache_size;
	/* [한국어] poll_group 별 iobuf 캐시 크기. 0 이면 매번 풀에서 직접 가져옴. */

	bool		dif_insert_or_strip;
	/* [한국어] 트랜스포트 레벨에서 DIF(보호정보) 자동 삽입/제거 여부. true 면 backing bdev
	 * 가 DIF 를 모르더라도 wire 에서 PI 처리. */

	bool		disable_command_passthru;
	/* [한국어] true 면 backing bdev 로의 NVMe admin 패스스루를 차단(보안). */

	/* Hole at bytes 30-31. */
	uint8_t		reserved30[2];
	/* [한국어] ABI 정렬용 hole. 항상 0. */

	uint32_t	abort_timeout_sec;
	/* [한국어] NVMe Abort 명령의 timeout. 시간 초과 시 abort 가 실패하고 호스트에 STATUS_NOT_ABORTED. */

	/* ms */
	uint32_t	association_timeout;
	/* [한국어] 호스트의 트랜스포트 connection 설정 timeout(ms). RDMA QP setup, TCP handshake 등. */

	/* Transport specific json values.
	 *
	 * If transport specific values provided then json object is valid only at the time
	 * transport is being created. It is transport layer responsibility to maintain
	 * the copy of it or its decoding if required.
	 */
	const struct spdk_json_val *transport_specific;
	/* [한국어] 트랜스포트별 추가 옵션의 JSON object. 예: TCP 의 sock_priority, RDMA 의
	 * srq_depth. 본 포인터는 create 호출 시점에만 유효 — 트랜스포트가 필요하면 deep copy. */

	/**
	 * The size of spdk_nvmf_transport_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
	/* [한국어] caller 가 전달한 sizeof. 미래 확장 호환성. */

	uint32_t acceptor_poll_rate;
	/* [한국어] connection accept 폴러의 주기(us). 작을수록 connect latency↓, CPU 부하↑. */

	/* Use zero-copy operations if the underlying bdev supports them */
	bool zcopy;
	/* [한국어] backing bdev 가 zcopy 를 지원하면 활성화. 트랜스포트가 zcopy_phase 를 사용. */

	/* Hole at bytes 61-63. */
	uint8_t reserved61[3];
	/* [한국어] 정렬 hole. */

	/* ACK timeout in milliseconds */
	uint32_t ack_timeout;
	/* [한국어] 트랜스포트별 ACK 대기 timeout(ms). RDMA RNR retry, TCP keepalive 등에 영향. */

	/* Size of RDMA data WR pool */
	uint32_t data_wr_pool_size;
	/* [한국어] RDMA 데이터 work request 풀 크기. RDMA 트랜스포트만 의미 있음. */

	/* The minimum Keep Alive Timeout value in milliseconds */
	uint32_t min_kato;
	/* [한국어] 호스트가 Keep-Alive timeout 을 너무 짧게 잡지 못하도록 강제하는 하한(ms). */

	/* kas indicates the granularity of the Keep Alive Timer in 100ms units. */
	uint16_t kas;
	/* [한국어] Keep-Alive 단위(100ms 단위 곱셈자). Identify ctrlr 의 KAS 필드와 매핑. */

	/* Enable or disable ONCS features. By default, all supported features are enabled. */
	struct spdk_nvme_cdata_oncs oncs;
	/* [한국어] Optional NVM Command Support 마스크 — write_zeroes/dataset_mgmt/reservations
	 * 등을 허용/차단. 기본은 모두 허용. */

	/* Enable or disable FUSES features. By default, all supported features are enabled. */
	struct spdk_nvme_cdata_fuses fuses;
	/* [한국어] Fused Operation 비트필드. 기본은 모두 허용. */
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_transport_opts) == 82, "Incorrect size");
/* [한국어] 컴파일 타임 검증 — packed 가 정확히 82B 를 만들어야 한다(ABI 안정). */

/*
 * [한국어]
 * struct spdk_nvmf_listen_opts - listener 추가 시 옵션. ABI-versioned.
 */
struct spdk_nvmf_listen_opts {
	/**
	 * The size of spdk_nvmf_listen_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
	/* [한국어] caller 의 sizeof. 라이브러리는 이 값까지만 신뢰. */

	/* Transport specific json values.
	 *
	 * If transport specific values provided then json object is valid only at the time
	 * listener is being added. It is transport layer responsibility to maintain
	 * the copy of it or its decoding if required.
	 */
	const struct spdk_json_val *transport_specific;
	/* [한국어] 트랜스포트별 listener 옵션 JSON. add_listener 시점에만 유효. */

	/**
	 * Indicates that all newly established connections shall immediately
	 * establish a secure channel, prior to any authentication.
	 */
	bool secure_channel;
	/* [한국어] true 면 새 connection 이 인증 전에 먼저 보안 채널(TLS) 을 수립해야 한다.
	 * NVMe TCP 1.0a 의 TLS 1.3 기반 보안. 설정자: 사용자(RPC). 읽는 자: TCP 트랜스포트 listen. */

	/* Hole at bytes 17-19. */
	uint8_t reserved1[3];
	/* [한국어] 정렬 hole. */

	/**
	 * Asymmetric Namespace Access state
	 * Optional parameter, which defines ANA_STATE that will be set for
	 * all ANA groups in this listener, when the listener is added to the subsystem.
	 * If not specified, SPDK_NVME_ANA_OPTIMIZED_STATE will be set by default.
	 */
	enum spdk_nvme_ana_state ana_state;
	/* [한국어] 이 listener 가 subsystem 에 associate 될 때 모든 ANA 그룹에 적용할 초기 ANA 상태.
	 * 0 또는 미지정이면 OPTIMIZED. multipath 환경에서 한 path 를 미리 inactive 로 두는 용도. */

	/* The socket implementation to use for the listener. */
	char *sock_impl;
	/* [한국어] (TCP) 사용할 sock 백엔드 이름("posix", "uring", ...). NULL = default. */
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_listen_opts) == 32, "Incorrect size");
/* [한국어] 정확히 32B (ABI 검증). */

/**
 * Initialize listen options
 *
 * \param opts Listener options.
 * \param opts_size Must be set to sizeof(struct spdk_nvmf_listen_opts).
 */
/*
 * [한국어]
 * spdk_nvmf_listen_opts_init - listen 옵션 구조체에 라이브러리 기본값을 채움.
 *
 * @opts: 사용자가 메모리만 할당한 빈 옵션.
 * @opts_size: 반드시 sizeof(struct spdk_nvmf_listen_opts).
 *
 * 사용자는 init → 필요 필드만 덮어쓰기 → spdk_nvmf_subsystem_add_listener_ext 에 전달.
 * opts_size 를 기록하여 ABI 호환을 보장. 호출 컨텍스트: 일반 사용자 thread (race 없음).
 */
void spdk_nvmf_listen_opts_init(struct spdk_nvmf_listen_opts *opts, size_t opts_size);

/*
 * [한국어]
 * struct spdk_nvmf_poll_group_stat - poll_group 의 누적 통계. RPC 로 노출.
 */
struct spdk_nvmf_poll_group_stat {
	/* cumulative admin qpair count */
	uint32_t admin_qpairs;
	/* [한국어] 이 poll_group 에 누적 등록된 admin qpair 수(생애 누적, 감소 안 함).
	 * 설정자: spdk_nvmf_poll_group_add (admin 인 경우 ++).
	 * 읽는 자: RPC dump_stat. 동기화: poll_group 소유 thread 단일 접근. */

	/* cumulative io qpair count */
	uint32_t io_qpairs;
	/* [한국어] 누적 IO qpair 수. 위와 동일 경로. */

	/* current admin qpair count */
	uint32_t current_admin_qpairs;
	/* [한국어] 현재 활성 admin qpair 수. add 시 ++, remove 시 --. */

	/* current io qpair count */
	uint32_t current_io_qpairs;
	/* [한국어] 현재 활성 IO qpair 수. */

	uint64_t pending_bdev_io;
	/* [한국어] bdev 슬롯 부족으로 대기 중인 nvmf 요청 수(현재 시점 stable). */

	/* NVMe IO commands completed (excludes admin commands) */
	uint64_t completed_nvme_io;
	/* [한국어] 완료한 IO 명령 누적. 처리량(IOPS) 계산에 사용. admin 은 제외. */
};

/**
 * Function to be called once asynchronous listen add and remove
 * operations are completed. See spdk_nvmf_subsystem_add_listener()
 * and spdk_nvmf_transport_stop_listen_async().
 *
 * \param ctx Context argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_subsystem_listen_done_fn - 비동기 listen add/remove 완료 콜백.
 *
 * @ctx: 호출자가 register 시 넘긴 opaque.
 * @status: 0 성공, 음수 errno.
 */
typedef void (*spdk_nvmf_tgt_subsystem_listen_done_fn)(void *ctx, int status);

/*
 * [한국어]
 * struct spdk_nvmf_referral_opts - Discovery referral 추가 옵션.
 *
 * Discovery referral: 이 target 의 discovery 서비스가 호스트에게 "다른 discovery 서버를
 * 보라" 고 안내하는 메타데이터. 호스트가 cluster 단위로 endpoint 를 발견하는 mesh 토폴로지.
 */
struct spdk_nvmf_referral_opts {
	/** Size of this structure */
	size_t size;
	/* [한국어] ABI-versioned size. */

	/** Transport ID of the referral */
	struct spdk_nvme_transport_id trid;
	/* [한국어] referral 가리키는 다른 discovery endpoint 의 trid. trtype/adrfam/traddr/trsvcid
	 * 가 의미 있는 필드. */

	/** The referral describes a referral to a subsystem which requires a secure channel */
	bool secure_channel;
	/* [한국어] true 면 referral 대상이 TLS 보안 채널을 요구함을 호스트에 알림. */

	/** Whether this will be visible to all hosts */
	bool allow_any_host;
	/* [한국어] true 면 모든 host 에게 이 referral 노출. false 면 명시적으로 add_host 한 host
	 * 에게만 노출. */
};

/**
 * Add a discovery service referral to an NVMe-oF target
 *
 * This function must be called from the app thread.
 *
 * \param tgt The target to which the referral will be added
 * \param opts Options describing the referral referral.
 *
 * \return 0 on success or a negated errno on failure
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_add_referral - target 에 discovery referral 추가.
 *
 * @tgt: 대상 target.
 * @opts: referral 정보.
 * @return: 0 성공, 음수 errno.
 *
 * 호출 컨텍스트: 반드시 app thread (직렬화). discovery 명령 처리 시 응답에 referral entry
 * 포함 가능. callee: 내부 referrals 리스트에 enqueue.
 */
int spdk_nvmf_tgt_add_referral(struct spdk_nvmf_tgt *tgt,
			       const struct spdk_nvmf_referral_opts *opts);

/**
 * Remove a discovery service referral from an NVMeoF target
 *
 * This function must be called from the app thread.
 *
 * \param tgt The target from which the referral will be removed
 * \param opts Options describing the referral referral.
 *
 * \return 0 on success or a negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_remove_referral - referral 제거. add 의 역연산.
 *
 * @tgt/@opts/@return: add 와 동일. 호출 컨텍스트: app thread.
 */
int spdk_nvmf_tgt_remove_referral(struct spdk_nvmf_tgt *tgt,
				  const struct spdk_nvmf_referral_opts *opts);

/**
 * Get the first referral in a target.
 *
 * \param tgt Target to query
 *
 * \return First referral in this target, or NULL if none exist
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_get_first_referral - target 의 첫 referral 반환(iterator 시작).
 *
 * @tgt: 대상.
 * @return: 첫 referral 포인터 또는 NULL.
 */
struct spdk_nvmf_referral *spdk_nvmf_tgt_get_first_referral(struct spdk_nvmf_tgt *tgt);

/**
 * Get the next referral in a target.
 *
 * \param tgt Target to query
 * \param prev_referral Previous referral returned from this function
 *
 * \return next referral in this target, or NULL if prev_referral was the last referral
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_referral_get_next - referral 순회 다음 항목.
 *
 * @tgt: 대상.
 * @prev_referral: 이전에 받은 referral.
 * @return: 다음 referral 또는 NULL.
 */
struct spdk_nvmf_referral *spdk_nvmf_tgt_referral_get_next(struct spdk_nvmf_tgt *tgt,
		struct spdk_nvmf_referral *prev_referral);

/*
 * Add a host to a discovery service referral. This makes the
 * referral visible to the host.
 *
 * \param referral The referral to which host is to be added
 * \param hostnqn NQN of the host which is to be added
 *
 * \return 0 on success or a negated errno on failure
 */
/*
 * [한국어]
 * spdk_nvmf_referral_add_host - referral 의 허용 host 명단에 NQN 추가.
 *
 * @referral: 대상.
 * @hostnqn: 추가할 호스트의 NQN 문자열.
 * @return: 0 성공, 음수 errno (중복 또는 메모리 부족).
 *
 * referral 의 allow_any_host 가 false 일 때만 의미 있음.
 */
int spdk_nvmf_referral_add_host(struct spdk_nvmf_referral *referral,
				const char *hostnqn);

/**
 * Remove a host from a discovery service referral.
 *
 * \param referral The referral from which host is to be removed
 * \param hostnqn NQN of the host which is to be removed
 *
 * \return 0 on success or a negated errno on failure
 */
/*
 * [한국어]
 * spdk_nvmf_referral_remove_host - referral 허용 명단에서 host 제거.
 */
int spdk_nvmf_referral_remove_host(struct spdk_nvmf_referral *referral,
				   const char *hostnqn);

/**
 * Set whether a referral should allow any host or only hosts in the allowed list.
 *
 * \param referral Referral to modify.
 * \param allow_any_host true to allow any host to see this referral in discovery
 * log, or false to enforce the list configured with spdk_nvmf_referral_add_host().
 *
 * \return 0 on success, or negated errno value on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_referral_set_allow_any_host - referral 의 host 정책 toggle.
 *
 * @referral: 대상. @allow_any_host: true=모두허용, false=명단검사.
 */
int spdk_nvmf_referral_set_allow_any_host(struct spdk_nvmf_referral *referral,
		bool allow_any_host);

/**
 * Get whether a referral allows any host or only hosts in the allowed list.
 *
 * \param referral Referral to query.
 *
 * \return true if any host is allowed, false if only hosts in the allowed list are allowed.
 */
/*
 * [한국어]
 * spdk_nvmf_referral_get_allow_any_host - 위 set 의 read 변형.
 */
bool spdk_nvmf_referral_get_allow_any_host(struct spdk_nvmf_referral *referral);

/**
 * Check whether a host is allowed to see a referral.
 *
 * \param referral Referral to query.
 * \param hostnqn NQN of the host to check.
 *
 * \return true if the host is allowed, false if not.
 */
/*
 * [한국어]
 * spdk_nvmf_referral_host_allowed - 특정 host 가 referral 을 볼 수 있는지 검사.
 *
 * allow_any_host == true 면 항상 true. 그렇지 않으면 명단 lookup.
 */
bool spdk_nvmf_referral_host_allowed(struct spdk_nvmf_referral *referral, const char *hostnqn);

/**
 * Get the first allowed host in a referral.
 *
 * \param referral Referral to query
 *
 * \return First allowed host in this referral, or NULL if none allowed
 */
/*
 * [한국어]
 * spdk_nvmf_referral_get_first_host - referral 허용 host 명단 iterator 시작.
 */
struct spdk_nvmf_host *spdk_nvmf_referral_get_first_host(struct spdk_nvmf_referral *referral);

/**
 * Get the next allowed host in a referral.
 *
 * \param referral Referral to query
 * \param prev_host Previous host returned from this function
 *
 * \return next allowed host in this referral, or NULL if prev_host was the last host
 */
/*
 * [한국어]
 * spdk_nvmf_referral_get_next_host - referral host 순회 다음 항목.
 */
struct spdk_nvmf_host *spdk_nvmf_referral_get_next_host(struct spdk_nvmf_referral *referral,
		struct spdk_nvmf_host *prev_host);

/**
 * Get the transport ID of a referral.
 *
 * \param referral Referral to query
 *
 * \return Transport ID of the referral
 */
/*
 * [한국어]
 * spdk_nvmf_referral_get_trid - referral 이 가리키는 trid 반환(read-only).
 */
const struct spdk_nvme_transport_id *spdk_nvmf_referral_get_trid(struct spdk_nvmf_referral
		*referral);

/**
 * Set a custom discovery filter.
 *
 * For this to take effect, the target must be created with the
 * SPDK_NVMF_TGT_DISCOVERY_MATCH_CUSTOM flag set in the discovery_filter field.
 *
 * \param filter The custom discovery filter to set.
 */
/*
 * [한국어]
 * spdk_nvmf_set_custom_discovery_filter - 사용자 정의 discovery 필터 콜백 등록(전역).
 *
 * @filter: 콜백 함수. NULL 이면 필터 비활성.
 *
 * 효과: target_opts.discovery_filter 에 MATCH_CUSTOM 비트가 set 되어 있어야 한다.
 * 호출 컨텍스트: 보통 spdk_app_start 직후 1 회. 전역이므로 모든 target 에 동일 적용.
 */
void spdk_nvmf_set_custom_discovery_filter(spdk_nvmf_custom_discovery_filter filter);

/**
 * Construct an NVMe-oF target.
 *
 * \param opts a pointer to an spdk_nvmf_target_opts structure.
 *
 * \return a pointer to a NVMe-oF target on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_create - target 인스턴스 생성.
 *
 * @opts: target 옵션 (size 필드는 sizeof 로 채워야 함).
 * @return: 새 target 포인터 또는 NULL.
 *
 * 호출 컨텍스트: app thread, 보통 spdk_app_start 콜백 안. 동일 이름의 target 이 이미 있으면
 * 실패. 내부적으로 subsystem 배열, 트랜스포트 리스트 head, 통계 등을 초기화. 호출 후
 * spdk_nvmf_tgt_listen_ext / add_transport / subsystem_create 등으로 구성.
 */
struct spdk_nvmf_tgt *spdk_nvmf_tgt_create(struct spdk_nvmf_target_opts *opts);

/*
 * [한국어]
 * spdk_nvmf_tgt_destroy_done_fn - target 비동기 파괴 완료 콜백 시그니처.
 */
typedef void (spdk_nvmf_tgt_destroy_done_fn)(void *ctx, int status);

/**
 * Destroy an NVMe-oF target.
 *
 * \param tgt The target to destroy. This releases all resources.
 * \param cb_fn A callback that will be called once the target is destroyed
 * \param cb_arg A context argument passed to cb_fn.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_destroy - target 비동기 파괴.
 *
 * @tgt: 대상. @cb_fn: 완료 콜백. @cb_arg: 콜백에 전달할 opaque.
 *
 * 모든 subsystem/transport/listener/poll_group 을 정리한 뒤 자기 자신을 free. 비동기.
 * 호출 컨텍스트: app thread.
 */
void spdk_nvmf_tgt_destroy(struct spdk_nvmf_tgt *tgt,
			   spdk_nvmf_tgt_destroy_done_fn cb_fn,
			   void *cb_arg);

/**
 * Get the name of an NVMe-oF target.
 *
 * \param tgt The target from which to get the name.
 *
 * \return The name of the target as a null terminated string.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_get_name - target 의 이름 문자열 반환(읽기전용).
 */
const char *spdk_nvmf_tgt_get_name(struct spdk_nvmf_tgt *tgt);

/**
 * Get a pointer to an NVMe-oF target.
 *
 * In order to support some legacy applications and RPC methods that may rely on the
 * concept that there is only one target, the name parameter can be passed as NULL.
 * If there is only one available target, that target will be returned.
 * Otherwise, name is a required parameter.
 *
 * \param name The name provided when the target was created.
 *
 * \return The target with the given name, or NULL if no match was found.
 */
/*
 * [한국어]
 * spdk_nvmf_get_tgt - 이름으로 target 조회.
 *
 * @name: target 이름 또는 NULL. NULL 인 경우 등록된 target 이 정확히 1 개면 그것을 반환,
 *        2 개 이상이면 NULL(모호).
 * @return: target 포인터 또는 NULL.
 *
 * 레거시 RPC 가 single-target 가정으로 작성된 경우를 위한 fallback.
 */
struct spdk_nvmf_tgt *spdk_nvmf_get_tgt(const char *name);

/**
 * Get the pointer to the first NVMe-oF target.
 *
 * Combined with spdk_nvmf_get_next_tgt to iterate over all available targets.
 *
 * \return The first NVMe-oF target.
 */
/*
 * [한국어]
 * spdk_nvmf_get_first_tgt - 등록된 target 중 첫 번째 반환(iterator 시작).
 */
struct spdk_nvmf_tgt *spdk_nvmf_get_first_tgt(void);

/**
 * Get the pointer to the first NVMe-oF target.
 *
 * Combined with spdk_nvmf_get_first_tgt to iterate over all available targets.
 *
 * \param prev A pointer to the last NVMe-oF target.
 *
 * \return The first NVMe-oF target.
 */
/*
 * [한국어]
 * spdk_nvmf_get_next_tgt - target 순회 다음 항목.
 *
 * @prev: 이전 반환값.
 * @return: 다음 target 또는 NULL.
 */
struct spdk_nvmf_tgt *spdk_nvmf_get_next_tgt(struct spdk_nvmf_tgt *prev);

/**
 * Write NVMe-oF target configuration into provided JSON context.
 * \param w JSON write context
 * \param tgt The NVMe-oF target
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_write_config_json - 현재 target 의 모든 구성을 JSON 으로 직렬화.
 *
 * @w: JSON 출력 컨텍스트.
 * @tgt: 대상.
 *
 * RPC save_config 의 핵심. 트랜스포트/subsystem/listener/host/ns/referral 등 모든 객체를
 * 다시 만들 수 있도록 RPC 호출 시퀀스 형태로 기록한다. 호출 컨텍스트: app thread.
 */
void spdk_nvmf_tgt_write_config_json(struct spdk_json_write_ctx *w, struct spdk_nvmf_tgt *tgt);

/**
 * Begin accepting new connections at the address provided.
 *
 * The connections will be matched with a subsystem, which may or may not allow
 * the connection based on a subsystem-specific list of allowed hosts. See
 * spdk_nvmf_subsystem_add_host() and spdk_nvmf_subsystem_add_listener()
 *
 * \param tgt The target associated with this listen address.
 * \param trid The address to listen at.
 * \param opts Listener options.
 *
 * \return 0 on success or a negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_listen_ext - target 레벨에서 trid 의 listener 시작.
 *
 * @tgt: 대상 target.
 * @trid: 리스닝할 주소(trtype/adrfam/traddr/trsvcid). subnqn 은 의미 없음.
 * @opts: listener 옵션 (secure_channel, ANA 초기상태 등).
 * @return: 0 성공, 음수 errno.
 *
 * 호출 후 호스트의 connect 시도가 받아들여지지만, 실제 어떤 subsystem 에 매칭되는지는
 * subsystem_add_listener / subsystem_add_host 의 정책에 따른다. 트랜스포트 모듈의
 * ops->listen 으로 위임.
 */
int spdk_nvmf_tgt_listen_ext(struct spdk_nvmf_tgt *tgt, const struct spdk_nvme_transport_id *trid,
			     struct spdk_nvmf_listen_opts *opts);

/**
 * Stop accepting new connections at the provided address.
 *
 * This is a counterpart to spdk_nvmf_tgt_listen_ext().
 *
 * \param tgt The target associated with the listen address.
 * \param trid The address to stop listening at.
 *
 * \return int. 0 on success or a negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_stop_listen - listen_ext 의 역연산. 해당 trid 에서 새 connection accept 중지.
 *
 * @tgt/@trid: listen_ext 와 동일 식별. @return: 0 성공.
 *
 * 이미 connect 된 qpair 는 영향 없음(별도 disconnect 필요).
 */
int spdk_nvmf_tgt_stop_listen(struct spdk_nvmf_tgt *tgt,
			      const struct spdk_nvme_transport_id *trid);

/**
 * Create a poll group.
 *
 * \param tgt The target to create a poll group.
 *
 * \return a poll group on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_poll_group_create - 현재 spdk_thread 에 새 poll_group 생성.
 *
 * @tgt: 부모 target.
 * @return: 새 poll_group 또는 NULL.
 *
 * 반드시 spdk_thread 위에서 호출 — poll_group 은 그 thread 에 영구 바인딩된다. 트랜스포트
 * 별 sub-group 도 함께 생성. 일반적으로 reactor 시작 시 한 thread 당 1 회.
 */
struct spdk_nvmf_poll_group *spdk_nvmf_poll_group_create(struct spdk_nvmf_tgt *tgt);

/**
 * Get optimal nvmf poll group for the qpair.
 *
 * \param qpair Requested qpair
 *
 * \return a poll group on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_get_optimal_poll_group - 새 qpair 를 어느 poll_group 에 배정할지 추천.
 *
 * @qpair: 새로 만들어진 qpair (아직 group=NULL).
 * @return: 가장 적합한 poll_group (NUMA / RDMA CQ 공유 / TCP socket affinity 고려) 또는 NULL.
 *
 * 트랜스포트의 get_optimal_poll_group ops 가 실제 결정. 결과 NULL 이면 호출자가 round-robin.
 */
struct spdk_nvmf_poll_group *spdk_nvmf_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair);

/*
 * [한국어]
 * spdk_nvmf_poll_group_destroy_done_fn - poll_group 비동기 파괴 완료 콜백 시그니처.
 */
typedef void(*spdk_nvmf_poll_group_destroy_done_fn)(void *cb_arg, int status);

/**
 * Destroy a poll group.
 *
 * \param group The poll group to destroy.
 * \param cb_fn A callback that will be called once the poll group is destroyed.
 * \param cb_arg A context argument passed to cb_fn.
 */
/*
 * [한국어]
 * spdk_nvmf_poll_group_destroy - poll_group 비동기 파괴.
 *
 * @group: 대상. @cb_fn: 완료 콜백. @cb_arg: 콜백 opaque.
 *
 * 모든 qpair 를 disconnect 후 트랜스포트 sub-group 정리, 마지막에 cb_fn 호출. 호출 컨텍스트:
 * 해당 poll_group 의 thread.
 */
void spdk_nvmf_poll_group_destroy(struct spdk_nvmf_poll_group *group,
				  spdk_nvmf_poll_group_destroy_done_fn cb_fn,
				  void *cb_arg);

/**
 * Add the given qpair to the poll group.
 *
 * \param group The group to add qpair to.
 * \param qpair The qpair to add.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_poll_group_add - qpair 를 poll_group 의 polling 셋에 등록.
 *
 * @group: 대상 poll_group.
 * @qpair: 추가할 qpair.
 * @return: 0 성공, -1 실패.
 *
 * 호출 컨텍스트: poll_group 의 thread (spdk_thread_send_msg 로 위임됨이 보통). 트랜스포트의
 * poll_group_add ops 호출 후 group->qpairs 리스트에 enqueue.
 */
int spdk_nvmf_poll_group_add(struct spdk_nvmf_poll_group *group,
			     struct spdk_nvmf_qpair *qpair);

/*
 * [한국어]
 * nvmf_qpair_disconnect_cb - qpair_disconnect 의 비동기 완료 콜백 시그니처.
 */
typedef void (*nvmf_qpair_disconnect_cb)(void *ctx);

/**
 * Disconnect an NVMe-oF qpair
 *
 * \param qpair The NVMe-oF qpair to disconnect.
 *
 * \return 0 upon success.
 * \return -ENOMEM if the function specific context could not be allocated.
 * \return -EINPROGRESS if the qpair is already in the process of disconnect.
 */
/*
 * [한국어]
 * spdk_nvmf_qpair_disconnect - qpair 비동기 종료.
 *
 * @qpair: 대상.
 * @return: 0 시작 성공, -ENOMEM 메모리 부족, -EINPROGRESS 이미 진행 중.
 *
 * 동작: 새 명령 거부 → outstanding 명령 완료 대기 → 트랜스포트 qpair_fini → poll_group_remove
 * → ctrlr 에서 detach. 호출 컨텍스트: qpair 의 thread.
 */
int spdk_nvmf_qpair_disconnect(struct spdk_nvmf_qpair *qpair);

/**
 * Get the peer's transport ID for this queue pair.
 *
 * This function will first zero the trid structure, and then fill
 * in the relevant trid fields to identify the listener. The relevant
 * fields will depend on the transport, but the subnqn will never
 * be a relevant field for purposes of this function.
 *
 * \param qpair The NVMe-oF qpair
 * \param trid Output parameter that will contain the transport id.
 *
 * \return 0 for success.
 * \return -EINVAL if the qpair is not connected.
 */
/*
 * [한국어]
 * spdk_nvmf_qpair_get_peer_trid - 이 qpair 의 호스트(peer) trid 조회.
 *
 * @qpair: 대상.
 * @trid: [out] zero 초기화 후 호스트 측 트랜스포트 식별자 채움.
 * @return: 0 성공, -EINVAL 미연결.
 *
 * subnqn 은 채우지 않음. RDMA 의 경우 호스트 IP/GID, TCP 의 경우 소스 IP/포트.
 */
int spdk_nvmf_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
				  struct spdk_nvme_transport_id *trid);

/**
 * Get the local transport ID for this queue pair.
 *
 * This function will first zero the trid structure, and then fill
 * in the relevant trid fields to identify the listener. The relevant
 * fields will depend on the transport, but the subnqn will never
 * be a relevant field for purposes of this function.
 *
 * \param qpair The NVMe-oF qpair
 * \param trid Output parameter that will contain the transport id.
 *
 * \return 0 for success.
 * \return -EINVAL if the qpair is not connected.
 */
/*
 * [한국어]
 * spdk_nvmf_qpair_get_local_trid - qpair 의 로컬(자기측) trid 조회.
 */
int spdk_nvmf_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvme_transport_id *trid);

/**
 * Get the associated listener transport ID for this queue pair.
 *
 * This function will first zero the trid structure, and then fill
 * in the relevant trid fields to identify the listener. The relevant
 * fields will depend on the transport, but the subnqn will never
 * be a relevant field for purposes of this function.
 *
 * \param qpair The NVMe-oF qpair
 * \param trid Output parameter that will contain the transport id.
 *
 * \return 0 for success.
 * \return -EINVAL if the qpair is not connected.
 */
/*
 * [한국어]
 * spdk_nvmf_qpair_get_listen_trid - 이 qpair 를 accept 한 listener 의 trid 조회.
 *
 * multipath/ANA 결정에 사용 — qpair 가 어느 path(listener) 를 통해 들어왔는지로 ANA 그룹 판단.
 */
int spdk_nvmf_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvme_transport_id *trid);

/**
 * Create an NVMe-oF subsystem.
 *
 * Subsystems are in one of three states: Inactive, Active, Paused. This
 * state affects which operations may be performed on the subsystem. Upon
 * creation, the subsystem will be in the Inactive state and may be activated
 * by calling spdk_nvmf_subsystem_start(). No I/O will be processed in the Inactive
 * or Paused states, but changes to the state of the subsystem may be made.
 *
 * \param tgt The NVMe-oF target that will own this subsystem.
 * \param nqn The NVMe qualified name of this subsystem.
 * \param type Whether this subsystem is an I/O subsystem or a Discovery subsystem.
 * \param num_ns The maximum number of namespaces this subsystem may contain.
 *
 * \return a pointer to a NVMe-oF subsystem on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_create - subsystem(NQN 컨테이너) 생성.
 *
 * @tgt: 부모 target.
 * @nqn: NVMe Qualified Name (예 "nqn.2016-06.io.spdk:cnode1"). target 내 unique.
 * @type: SPDK_NVMF_SUBTYPE_DISCOVERY 또는 SPDK_NVMF_SUBTYPE_NVME.
 * @num_ns: 보유 가능 namespace 상한.
 * @return: 새 subsystem 또는 NULL.
 *
 * 상태 머신: Inactive(생성 직후) → start() → Active → pause() → Paused → resume() → Active.
 * Inactive/Paused 에서만 add_ns/add_listener/add_host 등 변경 가능. Active 에서 호스트의
 * connect/IO 처리.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_subsystem_create(struct spdk_nvmf_tgt *tgt,
		const char *nqn,
		enum spdk_nvmf_subtype type,
		uint32_t num_ns);

/*
 * [한국어]
 * nvmf_subsystem_destroy_cb - subsystem 비동기 파괴 완료 콜백 시그니처.
 */
typedef void (*nvmf_subsystem_destroy_cb)(void *cb_arg);

/**
 * Destroy an NVMe-oF subsystem. A subsystem may only be destroyed when in
 * the Inactive state. See spdk_nvmf_subsystem_stop(). A subsystem may be
 * destroyed asynchronously, in that case \b cpl_cb will be called
 *
 * \param subsystem The NVMe-oF subsystem to destroy.
 * \param cpl_cb Optional callback to be called if the subsystem is destroyed asynchronously, only called if
 * return value is -EINPROGRESS
 * \param cpl_cb_arg Optional user context to be passed to \b cpl_cb
 *
 * \retval 0 if subsystem is destroyed, \b cpl_cb is not called is that case
 * \retval -EINVAl if \b subsystem is a NULL pointer
 * \retval -EAGAIN if \b subsystem is not in INACTIVE state
 * \retval -EALREADY if subsystem destruction is already started
 * \retval -EINPROGRESS if subsystem is destroyed asynchronously, cpl_cb will be called in that case
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_destroy - subsystem 파괴(동기 또는 비동기).
 *
 * @subsystem: 대상 (반드시 INACTIVE 상태).
 * @cpl_cb: 비동기 완료 콜백 (return 이 -EINPROGRESS 인 경우만 호출).
 * @cpl_cb_arg: 콜백 opaque.
 * @return:
 *   0          — 동기 파괴 완료, cpl_cb 호출 안 됨.
 *   -EINVAL    — subsystem == NULL.
 *   -EAGAIN    — INACTIVE 가 아님 (먼저 stop 필요).
 *   -EALREADY  — 이미 파괴 중.
 *   -EINPROGRESS — 비동기 진행, 끝나면 cpl_cb 호출.
 */
int
spdk_nvmf_subsystem_destroy(struct spdk_nvmf_subsystem *subsystem, nvmf_subsystem_destroy_cb cpl_cb,
			    void *cpl_cb_arg);

/**
 * Function to be called once the subsystem has changed state.
 *
 * \param subsystem NVMe-oF subsystem that has changed state.
 * \param cb_arg Argument passed to callback function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_state_change_done - subsystem 상태 전환 비동기 완료 콜백 시그니처.
 *
 * start/stop/pause/resume 의 결과를 caller 에 통지.
 */
typedef void (*spdk_nvmf_subsystem_state_change_done)(struct spdk_nvmf_subsystem *subsystem,
		void *cb_arg, int status);

/**
 * Transition an NVMe-oF subsystem from Inactive to Active state.
 *
 * \param subsystem The NVMe-oF subsystem.
 * \param cb_fn A function that will be called once the subsystem has changed state.
 * \param cb_arg Argument passed to cb_fn.
 *
 * \return 0 on success, or negated errno on failure. The callback provided will only
 * be called on success.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_start - Inactive → Active 전환. 호스트 connect 수락 시작.
 *
 * @subsystem: 대상. @cb_fn: 완료 콜백. @cb_arg: 콜백 opaque.
 * @return: 0 시작 성공, 음수 errno (콜백 호출 안 됨).
 *
 * 모든 listener 가 활성화되며 새 connect 가 ctrlr 생성으로 이어짐. 비동기.
 */
int spdk_nvmf_subsystem_start(struct spdk_nvmf_subsystem *subsystem,
			      spdk_nvmf_subsystem_state_change_done cb_fn,
			      void *cb_arg);

/**
 * Transition an NVMe-oF subsystem from Active to Inactive state.
 *
 * \param subsystem The NVMe-oF subsystem.
 * \param cb_fn A function that will be called once the subsystem has changed state.
 * \param cb_arg Argument passed to cb_fn.
 *
 * \return 0 on success, or negated errno on failure. The callback provided will only
 * be called on success.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_stop - Active → Inactive. 모든 호스트 연결 끊김.
 *
 * 진행 중 I/O 완료 대기 → ctrlr 모두 detach → listener disable. destroy 의 선결 조건.
 */
int spdk_nvmf_subsystem_stop(struct spdk_nvmf_subsystem *subsystem,
			     spdk_nvmf_subsystem_state_change_done cb_fn,
			     void *cb_arg);

/**
 * Transition an NVMe-oF subsystem from Active to Paused state.
 *
 * In a paused state, all admin queues are frozen across the whole subsystem. If
 * a namespace ID is provided, all commands to that namespace are quiesced and incoming
 * commands for that namespace are queued until the subsystem is resumed.
 *
 * \param subsystem The NVMe-oF subsystem.
 * \param nsid The namespace to pause. If 0, pause no namespaces.
 * \param cb_fn A function that will be called once the subsystem has changed state.
 * \param cb_arg Argument passed to cb_fn.
 *
 * \return 0 on success, or negated errno on failure. The callback provided will only
 * be called on success.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_pause - Active → Paused. admin 큐 전체 동결.
 *
 * @nsid: 0 이면 admin 만 freeze, 1.. 면 그 namespace 의 IO 까지 quiesce. 큐된 명령은 resume
 * 후 실행. add_ns/add_listener/add_host 같은 구성 변경에 사용.
 */
int spdk_nvmf_subsystem_pause(struct spdk_nvmf_subsystem *subsystem,
			      uint32_t nsid,
			      spdk_nvmf_subsystem_state_change_done cb_fn,
			      void *cb_arg);

/**
 * Transition an NVMe-oF subsystem from Paused to Active state.
 *
 * This resumes the entire subsystem, including any paused namespaces.
 *
 * \param subsystem The NVMe-oF subsystem.
 * \param cb_fn A function that will be called once the subsystem has changed state.
 * \param cb_arg Argument passed to cb_fn.
 *
 * \return 0 on success, or negated errno on failure. The callback provided will only
 * be called on success.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_resume - Paused → Active. pause 의 역연산.
 */
int spdk_nvmf_subsystem_resume(struct spdk_nvmf_subsystem *subsystem,
			       spdk_nvmf_subsystem_state_change_done cb_fn,
			       void *cb_arg);

/**
 * Search the target for a subsystem with the given NQN.
 *
 * \param tgt The NVMe-oF target to search from.
 * \param subnqn NQN of the subsystem.
 *
 * \return a pointer to the NVMe-oF subsystem on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_find_subsystem - target 안에서 NQN 으로 subsystem 조회.
 *
 * @tgt: 대상. @subnqn: NQN 문자열.
 * @return: subsystem 포인터 또는 NULL.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt,
		const char *subnqn);

/**
 * Begin iterating over all known subsystems. If no subsystems are present, return NULL.
 *
 * \param tgt The NVMe-oF target to iterate.
 *
 * \return a pointer to the first NVMe-oF subsystem on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_first - subsystem iterator 시작.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_subsystem_get_first(struct spdk_nvmf_tgt *tgt);

/**
 * Continue iterating over all known subsystems. If no additional subsystems, return NULL.
 *
 * \param subsystem Previous subsystem returned from \ref spdk_nvmf_subsystem_get_first or
 *                  \ref spdk_nvmf_subsystem_get_next.
 *
 * \return a pointer to the next NVMe-oF subsystem on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_next - subsystem iterator 다음 항목.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_subsystem_get_next(struct spdk_nvmf_subsystem *subsystem);

/**
 * Make the specified namespace visible to the specified host.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem the namespace belong to.
 * \param nsid Namespace ID to be made visible.
 * \param hostnqn The NQN for the host.
 * \param flags Must be zero (reserved for future use).
 *
 * \return 0 on success, or negated errno value on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_ns_add_host - namespace 를 host 1 명에게 노출(per-host visibility).
 *
 * @subsystem: 대상. @nsid: 1.. namespace id. @hostnqn: 허용 host NQN. @flags: 0(예약).
 * @return: 0 성공, 음수 errno.
 *
 * subsystem 이 INACTIVE 또는 PAUSED 상태에서만 가능. ns 의 no_auto_visible=true 옵션과 함께
 * 사용 — 디폴트로는 모든 host 에 보이지만 그 옵션이면 명시적 add_host 한 host 만 볼 수 있다.
 */
int spdk_nvmf_ns_add_host(struct spdk_nvmf_subsystem *subsystem,
			  uint32_t nsid,
			  const char *hostnqn,
			  uint32_t flags);

/**
 * Make the specified namespace not visible to the specified host.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem the namespace belong to.
 * \param nsid Namespace ID to be made not visible.
 * \param hostnqn The NQN for the host.
 * \param flags Must be zero (reserved for future use).
 *
 * \return 0 on success, or negated errno value on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_ns_remove_host - ns_add_host 의 역연산. 특정 host 에게 namespace 숨김.
 */
int spdk_nvmf_ns_remove_host(struct spdk_nvmf_subsystem *subsystem,
			     uint32_t nsid,
			     const char *hostnqn,
			     uint32_t flags);

/**
 * Allow the given host NQN to connect to the given subsystem.  Adding a host that's already allowed
 * results in an error.
 *
 * \param subsystem Subsystem to add host to.
 * \param hostnqn The NQN for the host.
 * \param params Transport specific parameters.
 *
 * \return 0 on success, or negated errno value on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_add_host - subsystem 의 허용 host 명단에 NQN 추가(레거시 API).
 *
 * @subsystem: 대상. @hostnqn: 허용 host NQN. @params: 트랜스포트별 추가 JSON(예: TCP PSK 키링).
 * @return: 0 성공, 음수 errno (이미 있으면 -EEXIST).
 *
 * 새로운 사용 권장은 spdk_nvmf_subsystem_add_host_ext (DH-HMAC-CHAP 키 직접 전달).
 */
int spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem,
				 const char *hostnqn, const struct spdk_json_val *params);

/*
 * [한국어]
 * struct spdk_nvmf_host_opts - host 추가 시 옵션 (확장형 API).
 */
struct spdk_nvmf_host_opts {
	/** Size of this structure */
	size_t				size;
	/* [한국어] ABI-versioned size. */

	/** Transport specific parameters */
	const struct spdk_json_val	*params;
	/* [한국어] 트랜스포트별 추가 JSON 옵션. add_host 시점에만 유효. */

	/** DH-HMAC-CHAP key */
	struct spdk_key			*dhchap_key;
	/* [한국어] DH-HMAC-CHAP host 측 PSK key (host 가 인증 시 사용). spdk_keyring 으로 관리.
	 * 설정자: 사용자(RPC). 읽는 자: lib/nvmf/auth.c. */

	/** DH-HMAC-CHAP controller key */
	struct spdk_key			*dhchap_ctrlr_key;
	/* [한국어] DH-HMAC-CHAP controller(타깃) 측 키 — bidirectional 인증 시 사용. NULL 이면
	 * unidirectional. */
};

/**
 * Allow the given host to connect to the given subsystem.
 *
 * \param subsystem Subsystem to add host to.
 * \param hostnqn Host's NQN.
 * \param opts Host's options.
 *
 * \return 0 on success, or negated errno value on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_add_host_ext - subsystem 에 host 추가(확장형, 키 포함).
 *
 * @subsystem: 대상. @hostnqn: NQN. @opts: spdk_nvmf_host_opts (size 필수).
 * @return: 0 성공.
 *
 * DH-HMAC-CHAP 인증을 사용하려는 경우 본 함수로 키를 함께 등록. 호출 컨텍스트: app thread,
 * subsystem 은 PAUSED/INACTIVE 권장.
 */
int spdk_nvmf_subsystem_add_host_ext(struct spdk_nvmf_subsystem *subsystem,
				     const char *hostnqn, struct spdk_nvmf_host_opts *opts);

/**
 * Remove the given host NQN from the list of allowed hosts.
 *
 * This call only removes the host from the allowed list of hosts.
 * If a host with the given NQN is already connected it will not be disconnected,
 * but it will not be able to create new connections.
 *
 * \param subsystem Subsystem to remove host from.
 * \param hostnqn The NQN for the host.
 *
 * \return 0 on success, or negated errno value on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_remove_host - 허용 host 명단에서 제거.
 *
 * 이미 connect 된 host 는 자동 끊지 않음(disconnect 는 별도 spdk_nvmf_subsystem_disconnect_host).
 * 새 connection 은 거부.
 */
int spdk_nvmf_subsystem_remove_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn);

/*
 * [한국어]
 * struct spdk_nvmf_subsystem_key_opts - host 의 인증 키 갱신 옵션.
 */
struct spdk_nvmf_subsystem_key_opts {
	/** Size of this structure */
	size_t				size;
	/* [한국어] ABI-versioned size. */

	/** DH-HMAC-CHAP key */
	struct spdk_key			*dhchap_key;
	/* [한국어] 새 host 측 DH-HMAC-CHAP key. */

	/** DH-HMAC-CHAP controller key */
	struct spdk_key			*dhchap_ctrlr_key;
	/* [한국어] 새 controller 측 DH-HMAC-CHAP key. */
};

/**
 * Set keys required for a host to connect to a given subsystem.  This will override the keys set
 * by `spdk_nvmf_subsystem_add_host_ext()`.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_set_keys - 기존 host entry 의 키만 갱신(rotation).
 *
 * @subsystem: 대상. @hostnqn: host NQN. @opts: 새 키.
 * @return: 0 성공.
 */
int spdk_nvmf_subsystem_set_keys(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn,
				 struct spdk_nvmf_subsystem_key_opts *opts);


/**
 * Disconnect all connections originating from the provided hostnqn
 *
 * To disconnect and block all new connections from a host, first call
 * spdk_nvmf_subsystem_remove_host() to remove it from the list of allowed hosts, then
 * call spdk_nvmf_subsystem_disconnect_host() to close any remaining connections.
 *
 * \param subsystem Subsystem to operate on
 * \param hostnqn The NQN for the host
 * \param cb_fn The function to call on completion.
 * \param cb_arg The argument to pass to the cb_fn.
 *
 * \return int. 0 when the asynchronous process starts successfully or a negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_disconnect_host - 특정 host 의 모든 활성 connection 끊기.
 *
 * @subsystem: 대상. @hostnqn: host NQN. @cb_fn: 완료 콜백. @cb_arg: 콜백 opaque.
 * @return: 0 시작 성공, 음수 errno.
 *
 * 권장 사용 패턴: remove_host → disconnect_host. 비동기 — 모든 qpair 가 끊긴 후 cb_fn 호출.
 */
int spdk_nvmf_subsystem_disconnect_host(struct spdk_nvmf_subsystem *subsystem,
					const char *hostnqn,
					spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
					void *cb_arg);

/**
 * Set whether a subsystem should allow any host or only hosts in the allowed list.
 *
 * \param subsystem Subsystem to modify.
 * \param allow_any_host true to allow any host to connect to this subsystem,
 * or false to enforce the list configured with spdk_nvmf_subsystem_add_host().
 *
 * \return 0 on success, or negated errno value on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_set_allow_any_host - subsystem 의 host 정책 toggle.
 *
 * @allow_any_host: true=NQN 검사 없이 모두 수락, false=add_host 명단 검사.
 *
 * 보안 가장 큰 스위치. true 는 디버그/테스트 환경 권장.
 */
int spdk_nvmf_subsystem_set_allow_any_host(struct spdk_nvmf_subsystem *subsystem,
		bool allow_any_host);

/**
 * Check whether a subsystem should allow any host or only hosts in the allowed list.
 *
 * \param subsystem Subsystem to query.
 *
 * \return true if any host is allowed to connect to this subsystem, or false if
 * connecting hosts must be in the list configured with spdk_nvmf_subsystem_add_host().
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_allow_any_host - 위 set 의 read 변형.
 */
bool spdk_nvmf_subsystem_get_allow_any_host(const struct spdk_nvmf_subsystem *subsystem);

/**
 * Check if the given host is allowed to connect to the subsystem.
 *
 * \param subsystem The subsystem to query.
 * \param hostnqn The NQN of the host.
 *
 * \return true if allowed, false if not.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_host_allowed - 특정 host NQN 이 connect 허용되는지 검사.
 *
 * allow_any_host == true 면 항상 true. 아니면 명단 lookup.
 */
bool spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn);

/**
 * Get the first allowed host in a subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return first allowed host in this subsystem, or NULL if none allowed.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_first_host - 허용 host iterator 시작.
 */
struct spdk_nvmf_host *spdk_nvmf_subsystem_get_first_host(struct spdk_nvmf_subsystem *subsystem);

/**
 * Get the next allowed host in a subsystem.
 *
 * \param subsystem Subsystem to query.
 * \param prev_host Previous host returned from this function.
 *
 * \return next allowed host in this subsystem, or NULL if prev_host was the last host.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_next_host - 허용 host iterator 다음 항목.
 */
struct spdk_nvmf_host *spdk_nvmf_subsystem_get_next_host(struct spdk_nvmf_subsystem *subsystem,
		struct spdk_nvmf_host *prev_host);

/**
 * Get a host's NQN.
 *
 * \param host Host to query.
 *
 * \return NQN of host.
 */
/*
 * [한국어]
 * spdk_nvmf_host_get_nqn - host 객체에서 NQN 문자열 추출.
 */
const char *spdk_nvmf_host_get_nqn(const struct spdk_nvmf_host *host);

/**
 * Accept new connections on the address provided.
 *
 * This does not start the listener. Use spdk_nvmf_tgt_listen_ext() for that.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 * No namespaces are required to be paused.
 *
 * \param subsystem Subsystem to add listener to.
 * \param trid The address to accept connections from.
 * \param cb_fn A callback that will be called once the association is complete.
 * \param cb_arg Argument passed to cb_fn.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_add_listener - subsystem 을 trid listener 와 associate (레거시).
 *
 * @subsystem: 대상. @trid: 이미 listen 중인 trid. @cb_fn/@cb_arg: 완료 콜백.
 *
 * 본 함수는 listener 를 start 하지 않음 — tgt_listen_ext 가 그 역할. 본 함수는 "이미 listen
 * 중인 trid 에서 들어온 connect 가 이 subsystem 으로 매칭되도록 등록" 만 한다. 비동기.
 */
void spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				      const struct spdk_nvme_transport_id *trid,
				      spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
				      void *cb_arg);

/* Additional options for listener creation. */
/*
 * [한국어]
 * struct spdk_nvmf_listener_opts - subsystem listener 추가 시 추가 옵션 (확장형).
 */
struct spdk_nvmf_listener_opts {
	/**
	 * The size of spdk_nvmf_listener_opts according to the caller of this library is used for
	 * ABI compatibility. The library uses this field to know how many fields in this structure
	 * are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
	/* [한국어] ABI-versioned size. */

	/* Secure channel parameter used in TCP TLS. */
	bool secure_channel;
	/* [한국어] true 면 TLS 보안 채널 강제. NVMe TCP 1.0a TLS 1.3. */

	/* Hole at bytes 9-11. */
	uint8_t reserved1[3];
	/* [한국어] 정렬 hole. */

	/* Asymmetric namespace access state */
	enum spdk_nvme_ana_state ana_state;
	/* [한국어] 이 listener 로 들어온 호스트가 보게 될 ANA 그룹 초기 상태. */

	/* The socket implementation to use for the listener. */
	char *sock_impl;
	/* [한국어] (TCP) 사용할 socket 백엔드. NULL = default. */

} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_listener_opts) == 24, "Incorrect size");
/* [한국어] ABI 검증 — 정확히 24B. */

/**
 * Initialize options structure for listener creation.
 *
 * \param opts Options structure to initialize.
 * \param size Size of the structure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_listener_opts_init - listener 옵션에 라이브러리 기본값 채움.
 */
void spdk_nvmf_subsystem_listener_opts_init(struct spdk_nvmf_listener_opts *opts, size_t size);

/**
 * Accept new connections on the address provided.
 *
 * This does not start the listener. Use spdk_nvmf_tgt_listen_ext() for that.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 * No namespaces are required to be paused.
 *
 * \param subsystem Subsystem to add listener to.
 * \param trid The address to accept connections from.
 * \param cb_fn A callback that will be called once the association is complete.
 * \param cb_arg Argument passed to cb_fn.
 * \param opts NULL or options requested for listener creation.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_add_listener_ext - subsystem-listener associate(확장형, opts 포함).
 *
 * 위 add_listener 와 동일하지만 opts 추가. 권장 API.
 */
void spdk_nvmf_subsystem_add_listener_ext(struct spdk_nvmf_subsystem *subsystem,
		const struct spdk_nvme_transport_id *trid,
		spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
		void *cb_arg, struct spdk_nvmf_listener_opts *opts);

/**
 * Remove the listener from subsystem.
 *
 * New connections to the address won't be propagated to the subsystem.
 * However to stop listening at target level one must use the
 * spdk_nvmf_tgt_stop_listen().
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 * No namespaces are required to be paused.
 *
 * \param subsystem Subsystem to remove listener from.
 * \param trid The address to no longer accept connections from.
 *
 * \return 0 on success, or negated errno value on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_remove_listener - subsystem-listener associate 해제.
 *
 * trid 의 listener 자체는 살아 있음 — target 레벨 종료는 별도 spdk_nvmf_tgt_stop_listen.
 */
int spdk_nvmf_subsystem_remove_listener(struct spdk_nvmf_subsystem *subsystem,
					const struct spdk_nvme_transport_id *trid);

/**
 * Check if connections originated from the given address are allowed to connect
 * to the subsystem.
 *
 * \param subsystem The subsystem to query.
 * \param trid The listen address.
 *
 * \return true if allowed, or false if not.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_listener_allowed - 해당 trid 로 들어온 connection 이 이 subsystem 에
 *                                          연결 가능한지 검사.
 */
bool spdk_nvmf_subsystem_listener_allowed(struct spdk_nvmf_subsystem *subsystem,
		const struct spdk_nvme_transport_id *trid);

/**
 * Get the first allowed listen address in the subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return first allowed listen address in this subsystem, or NULL if none allowed.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_first_listener - subsystem listener iterator 시작.
 */
struct spdk_nvmf_subsystem_listener *spdk_nvmf_subsystem_get_first_listener(
	struct spdk_nvmf_subsystem *subsystem);

/**
 * Get the next allowed listen address in a subsystem.
 *
 * \param subsystem Subsystem to query.
 * \param prev_listener Previous listen address for this subsystem.
 *
 * \return next allowed listen address in this subsystem, or NULL if prev_listener
 * was the last address.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_next_listener - subsystem listener iterator 다음 항목.
 */
struct spdk_nvmf_subsystem_listener *spdk_nvmf_subsystem_get_next_listener(
	struct spdk_nvmf_subsystem *subsystem,
	struct spdk_nvmf_subsystem_listener *prev_listener);

/**
 * Get a listen address' transport ID
 *
 * \param listener This listener.
 *
 * \return the transport ID for this listener.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_listener_get_trid - listener 에서 trid 추출(read-only).
 */
const struct spdk_nvme_transport_id *spdk_nvmf_subsystem_listener_get_trid(
	struct spdk_nvmf_subsystem_listener *listener);

/**
 * Set whether a subsystem should allow any listen address or only addresses in the allowed list.
 *
 * \param subsystem Subsystem to allow dynamic listener assignment.
 * \param allow_any_listener true to allow dynamic listener assignment for
 * this subsystem, or false to enforce the list configured during
 * subsystem setup.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_allow_any_listener - subsystem 의 listener 정책 toggle.
 *
 * @allow_any_listener: true 면 어느 listener 든 자동 associate, false 면 명단 검사.
 */
void spdk_nvmf_subsystem_allow_any_listener(
	struct spdk_nvmf_subsystem *subsystem,
	bool allow_any_listener);

/**
 * Check whether a subsystem allows any listen address or only addresses in the allowed list.
 *
 * \param subsystem Subsystem to query.
 *
 * \return true if this subsystem allows dynamic management of listen address list,
 *  or false if only allows addresses in the list configured during subsystem setup.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_any_listener_allowed - 위 set 의 read.
 */
bool spdk_nvmf_subsystem_any_listener_allowed(
	struct spdk_nvmf_subsystem *subsystem);

/**
 * Set whether a subsystem supports Asymmetric Namespace Access (ANA)
 * reporting.
 *
 * May only be performed on subsystems in the INACTIVE state.
 *
 * \param subsystem Subsystem to modify.
 * \param ana_reporting true to support or false not to support ANA reporting.
 *
 * \return 0 on success, or negated errno value on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_set_ana_reporting - ANA(Asymmetric Namespace Access) 지원 여부 설정.
 *
 * @subsystem: 대상 (반드시 INACTIVE).
 * @ana_reporting: true=호스트에 ANA log page 노출, false=비활성.
 *
 * ANA: multi-path NVMe 의 path state(Optimized/Non-optimized/Inaccessible/Persistent_loss/
 * Change). active-active 또는 active-passive multipath 에서 호스트가 path 선호도를 결정하게 함.
 */
int spdk_nvmf_subsystem_set_ana_reporting(struct spdk_nvmf_subsystem *subsystem,
		bool ana_reporting);

/**
 * Get whether a subsystem supports Asymmetric Namespace Access (ANA)
 * reporting.
 *
 * \param subsystem Subsystem to check
 *
 * \return true if subsystem supports ANA reporting, false otherwise.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_ana_reporting - 위 set 의 read.
 */
bool spdk_nvmf_subsystem_get_ana_reporting(struct spdk_nvmf_subsystem *subsystem);

/**
 * Set Asymmetric Namespace Access (ANA) state for the specified ANA group id.
 *
 * May only be performed on subsystems in the INACTIVE or PAUSED state.
 *
 * \param subsystem Subsystem to operate on
 * \param trid Address for which the new state will apply
 * \param ana_state The ANA state which is to be set
 * \param anagrpid The ANA group ID to operate on
 * \param cb_fn The function to call on completion
 * \param cb_arg The argument to pass to the cb_fn
 *
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_set_ana_state - listener+anagrpid 단위로 ANA 상태 변경.
 *
 * @subsystem: 대상. @trid: 어느 path(listener) 에 적용할지. @ana_state: 새 상태.
 * @anagrpid: ANA 그룹 ID (한 그룹의 모든 ns 가 동일 상태).
 * @cb_fn/@cb_arg: 비동기 완료 콜백.
 *
 * 비동기 — 호스트들에게 AEN(Async Event Notice) 으로 변경 통지. fail-over / 유지보수 시
 * Optimized → Inaccessible 로 임시 전환.
 */
void spdk_nvmf_subsystem_set_ana_state(struct spdk_nvmf_subsystem *subsystem,
				       const struct spdk_nvme_transport_id *trid,
				       enum spdk_nvme_ana_state ana_state, uint32_t anagrpid,
				       spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn, void *cb_arg);

/**
 * Get Asymmetric Namespace Access (ANA) state for the specified ANA group id.
 *
 * \param subsystem Subsystem to operate on
 * \param trid Address for which the ANA is to be looked up
 * \param anagrpid The ANA group ID to check for
 * \param ana_state Output parameter that will contain the ANA state
 *
 * \return 0 on success, or negated errno value on failure.
 *
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_ana_state - listener+anagrpid 의 현재 ANA 상태 조회.
 */
int spdk_nvmf_subsystem_get_ana_state(struct spdk_nvmf_subsystem *subsystem,
				      const struct spdk_nvme_transport_id *trid,
				      uint32_t anagrpid,
				      enum spdk_nvme_ana_state *ana_state);

/**
 * Change ANA group ID of a namespace of a subsystem.
 *
 * May only be performed on subsystems in the INACTIVE or PAUSED state.
 *
 * \param subsystem Subsystem the namespace belongs to.
 * \param nsid Namespace ID to change.
 * \param anagrpid A new ANA group ID to set.
 *
 * \return 0 on success, negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_set_ns_ana_group - namespace 를 다른 ANA 그룹으로 이동.
 *
 * @subsystem: 대상 (INACTIVE/PAUSED). @nsid: 1.. ns id. @anagrpid: 새 ANA 그룹.
 *
 * ANA 그룹 단위로 path state 가 결정되므로, ns 를 다른 그룹으로 옮기면 그 ns 의 path 정책이
 * 즉시 바뀐다. 사용 사례: 한 namespace 를 별도 path 로 분리 운영.
 */
int spdk_nvmf_subsystem_set_ns_ana_group(struct spdk_nvmf_subsystem *subsystem,
		uint32_t nsid, uint32_t anagrpid);

/**
 * Sets the controller ID range for a subsystem.
 *
 * Valid range is [1, 0xFFEF].
 * May only be performed on subsystems in the INACTIVE state.
 *
 * \param subsystem Subsystem to modify.
 * \param min_cntlid Minimum controller ID.
 * \param max_cntlid Maximum controller ID.
 *
 * \return 0 on success, or negated errno value on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_set_cntlid_range - subsystem 이 host 에 할당할 cntlid 범위 설정.
 *
 * @subsystem: 대상 (INACTIVE).
 * @min_cntlid/@max_cntlid: 유효 범위 [1, 0xFFEF].
 *
 * cntlid: NVMe Controller Identifier — Connect 응답에 host 에게 회신되는 16비트 ID. multi-target
 * 환경에서 cntlid 가 충돌하지 않게 분리하는 데 사용.
 */
int spdk_nvmf_subsystem_set_cntlid_range(struct spdk_nvmf_subsystem *subsystem,
		uint16_t min_cntlid, uint16_t max_cntlid);

/** NVMe-oF target namespace creation options */
/*
 * [한국어]
 * struct spdk_nvmf_ns_opts - namespace 생성 시 옵션. ABI-versioned.
 */
struct spdk_nvmf_ns_opts {
	/**
	 * Namespace ID
	 *
	 * Set to 0 to automatically assign a free NSID.
	 */
	uint32_t nsid;
	/* [한국어] Namespace ID. 0 이면 라이브러리가 빈 nsid 를 자동 할당. 1..max_namespaces. */

	/**
	 * Namespace Globally Unique Identifier
	 *
	 * Fill with 0s if not specified.
	 */
	uint8_t nguid[16];
	/* [한국어] NGUID — IEEE OUI 기반 16바이트 글로벌 식별자. 0 이면 라이브러리가 자동 생성.
	 * 호스트의 multipath 매칭 키. */

	/**
	 * IEEE Extended Unique Identifier
	 *
	 * Fill with 0s if not specified.
	 */
	uint8_t eui64[8];
	/* [한국어] EUI64 — 8바이트 IEEE 식별자. NGUID 와 함께 호스트에 노출. */

	/**
	 * Namespace UUID
	 *
	 * Fill with 0s if not specified.
	 */
	struct spdk_uuid uuid;
	/* [한국어] namespace UUID(16B). NVMe Identify NS 응답의 UUID list 에 포함. NGUID 와 별개의 추가 식별자. */

	/* Hole at bytes 44-47. */
	uint8_t reserved44[4];
	/* [한국어] 정렬 hole. */

	/**
	 * The size of spdk_nvmf_ns_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this structure
	 * are valid.  And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
	/* [한국어] ABI-versioned size. */

	/**
	 * ANA group ID
	 *
	 * Set to be equal with the NSID if not specified.
	 */
	uint32_t anagrpid;
	/* [한국어] ns 가 속할 ANA 그룹. 0 이면 nsid 와 동일하게 자동. ANA reporting 활성 시 의미. */

	/**
	 * Do not automatically make namespace visible to controllers
	 *
	 * False if not specified
	 */
	bool no_auto_visible;
	/* [한국어] true 면 ns 가 모든 host 에 자동 노출되지 않음 — 명시적 ns_add_host 한 host 만
	 * 볼 수 있다. 멀티테넌트/per-host masking 시 사용. */

	/* Hole at bytes 61-63. */
	uint8_t reserved61[3];
	/* [한국어] 정렬 hole. */

	/* Transport specific json values.
	 *
	 * If transport specific values provided then json object is valid only at the time
	 * namespace is being added. It is transport layer responsibility to maintain
	 * the copy of it or its decoding if required. When \ref spdk_nvmf_ns_get_opts used
	 * after namespace has been added object becomes invalid.
	 */
	const struct spdk_json_val *transport_specific;
	/* [한국어] ns 추가 시점에만 유효한 트랜스포트별 추가 JSON. */

	/**
	 * Enable hide_metadata option to the bdev.
	 */
	bool hide_metadata;
	/* [한국어] true 면 backing bdev 의 metadata(8B PI 등) 를 호스트에 숨기고 LBA 만 노출. */
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_ns_opts) == 73, "Incorrect size");
/* [한국어] ABI 검증 — 정확히 73B. */

/**
 * Get default namespace creation options.
 *
 * \param opts Namespace options to fill with defaults.
 * \param opts_size sizeof(struct spdk_nvmf_ns_opts)
 */
/*
 * [한국어]
 * spdk_nvmf_ns_opts_get_defaults - ns 옵션을 라이브러리 기본값으로 채움.
 */
void spdk_nvmf_ns_opts_get_defaults(struct spdk_nvmf_ns_opts *opts, size_t opts_size);

/**
 * Add a namespace to a subsystems in the PAUSED or INACTIVE states.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 *
 * \param subsystem Subsystem to add namespace to.
 * \param bdev_name Block device name to add as a namespace.
 * \param opts Namespace options, or NULL to use defaults.
 * \param opts_size sizeof(*opts)
 * \param ptpl_file Persist through power loss file path.
 *
 * \return newly added NSID on success, or 0 on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_add_ns_ext - bdev 를 subsystem 의 namespace 로 추가.
 *
 * @subsystem: 대상 (INACTIVE/PAUSED).
 * @bdev_name: 이미 만들어진 bdev 이름 (예 "Malloc0", "Nvme0n1").
 * @opts: ns 옵션 또는 NULL(기본값).
 * @opts_size: sizeof(*opts).
 * @ptpl_file: NVMe Reservation Persist Through Power Loss 정보를 저장할 파일 경로 (NULL = 비활성).
 * @return: 새 NSID(>0), 0 = 실패.
 *
 * NVMe-oF namespace = bdev 1 개 매핑. ptpl 활성화 시 reservation 정보가 파일에 기록되어
 * 재시작 후 복원. 호출 후 호스트는 새 nsid 를 보게 됨(active subsystem 이라면 namespace
 * change AEN 송신).
 */
uint32_t spdk_nvmf_subsystem_add_ns_ext(struct spdk_nvmf_subsystem *subsystem,
					const char *bdev_name,
					const struct spdk_nvmf_ns_opts *opts, size_t opts_size,
					const char *ptpl_file);

/**
 * Remove a namespace from a subsystem.
 *
 * May only be performed on subsystems in the PAUSED or INACTIVE states.
 * Additionally, the namespace must be paused.
 *
 * \param subsystem Subsystem the namespace belong to.
 * \param nsid Namespace ID to be removed.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_remove_ns - namespace 제거.
 *
 * @subsystem: 대상 (INACTIVE/PAUSED, 추가로 ns 자체가 paused 여야 함).
 * @nsid: 제거 대상 NSID.
 * @return: 0 성공, -1 실패.
 *
 * 호스트는 namespace change AEN 으로 통지받음.
 */
int spdk_nvmf_subsystem_remove_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid);

/**
 * Get the first allocated namespace in a subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return first allocated namespace in this subsystem, or NULL if this subsystem
 * has no namespaces.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_first_ns - ns iterator 시작 (할당된 nsid 만).
 */
struct spdk_nvmf_ns *spdk_nvmf_subsystem_get_first_ns(struct spdk_nvmf_subsystem *subsystem);

/**
 * Get the next allocated namespace in a subsystem.
 *
 * \param subsystem Subsystem to query.
 * \param prev_ns Previous ns returned from this function.
 *
 * \return next allocated namespace in this subsystem, or NULL if prev_ns was the
 * last namespace.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_next_ns - ns iterator 다음 항목.
 */
struct spdk_nvmf_ns *spdk_nvmf_subsystem_get_next_ns(struct spdk_nvmf_subsystem *subsystem,
		struct spdk_nvmf_ns *prev_ns);

/**
 * Get a namespace in a subsystem by NSID.
 *
 * \param subsystem Subsystem to search.
 * \param nsid Namespace ID to find.
 *
 * \return namespace matching nsid, or NULL if nsid was not found.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_ns - NSID 로 ns 직접 조회.
 */
struct spdk_nvmf_ns *spdk_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem,
		uint32_t nsid);

/**
 * Get the maximum number of namespaces allowed in a subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return Maximum number of namespaces allowed in the subsystem, or 0 for unlimited.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_max_namespaces - 본 subsystem 이 보유 가능한 ns 상한 반환.
 */
uint32_t spdk_nvmf_subsystem_get_max_namespaces(const struct spdk_nvmf_subsystem *subsystem);

/**
 * Get the minimum controller ID allowed in a subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return Minimum controller ID allowed in the subsystem.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_min_cntlid - 할당 가능한 최소 cntlid 반환.
 */
uint16_t spdk_nvmf_subsystem_get_min_cntlid(const struct spdk_nvmf_subsystem *subsystem);

/**
 * Get the maximum controller ID allowed in a subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return Maximum controller ID allowed in the subsystem.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_max_cntlid - 최대 cntlid 반환.
 */
uint16_t spdk_nvmf_subsystem_get_max_cntlid(const struct spdk_nvmf_subsystem *subsystem);

/**
 * Get a namespace's NSID.
 *
 * \param ns Namespace to query.
 *
 * \return NSID of ns.
 */
/*
 * [한국어]
 * spdk_nvmf_ns_get_id - ns 의 NSID 반환.
 */
uint32_t spdk_nvmf_ns_get_id(const struct spdk_nvmf_ns *ns);

/**
 * Get a namespace's associated bdev.
 *
 * \param ns Namespace to query.
 *
 * \return backing bdev of ns.
 */
/*
 * [한국어]
 * spdk_nvmf_ns_get_bdev - ns 가 매핑된 backing bdev 반환.
 *
 * 호출자는 desc/channel 을 직접 만들지 말 것 — nvmf 코어가 관리. 정보 조회용.
 */
struct spdk_bdev *spdk_nvmf_ns_get_bdev(struct spdk_nvmf_ns *ns);

/**
 * Get the options specified for a namespace.
 *
 * \param ns Namespace to query.
 * \param opts Output parameter for options.
 * \param opts_size sizeof(*opts)
 */
/*
 * [한국어]
 * spdk_nvmf_ns_get_opts - 등록 시 사용한 ns 옵션 복원(read).
 *
 * transport_specific 포인터는 ns 추가 후에는 invalid (위 ns_opts 주석 참조).
 */
void spdk_nvmf_ns_get_opts(const struct spdk_nvmf_ns *ns, struct spdk_nvmf_ns_opts *opts,
			   size_t opts_size);

/**
 * Get the serial number of the specified subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return serial number of the specified subsystem.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_sn - subsystem 의 serial number(NVMe SN, 20-byte ASCII) 반환.
 */
const char *spdk_nvmf_subsystem_get_sn(const struct spdk_nvmf_subsystem *subsystem);


/**
 * Set the serial number for the specified subsystem.
 *
 * \param subsystem Subsystem to set for.
 * \param sn serial number to set.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_set_sn - subsystem 의 serial number 설정.
 *
 * NVMe Identify Controller 응답의 SN 필드에 채워져 호스트가 본다. ASCII 20 문자, 패딩 공백.
 */
int spdk_nvmf_subsystem_set_sn(struct spdk_nvmf_subsystem *subsystem, const char *sn);

/**
 * Get the model number of the specified subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return model number of the specified subsystem.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_mn - subsystem 의 model number(40-byte ASCII) 반환.
 */
const char *spdk_nvmf_subsystem_get_mn(const struct spdk_nvmf_subsystem *subsystem);


/**
 * Set the model number for the specified subsystem.
 *
 * \param subsystem Subsystem to set for.
 * \param mn model number to set.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_set_mn - subsystem 의 model number 설정.
 *
 * Identify ctrlr 응답의 MN 필드. 호스트의 udev rule 매칭에 자주 사용.
 */
int spdk_nvmf_subsystem_set_mn(struct spdk_nvmf_subsystem *subsystem, const char *mn);

/**
 * Get the NQN of the specified subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return NQN of the specified subsystem.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_nqn - subsystem 의 NQN 문자열 반환.
 */
const char *spdk_nvmf_subsystem_get_nqn(const struct spdk_nvmf_subsystem *subsystem);

/**
 * Get the type of the specified subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return the type of the specified subsystem.
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_type - subsystem 의 종류(Discovery/NVMe) 반환.
 */
enum spdk_nvmf_subtype spdk_nvmf_subsystem_get_type(struct spdk_nvmf_subsystem *subsystem);

/**
 * Get maximum namespace id of the specified subsystem.
 *
 * \param subsystem Subsystem to query.
 *
 * \return maximum namespace id
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_get_max_nsid - subsystem 에서 현재 사용 중인 nsid 의 max 값 반환.
 *
 * 호스트가 Identify NS list 에서 종료 인덱스를 결정할 때 사용.
 */
uint32_t spdk_nvmf_subsystem_get_max_nsid(struct spdk_nvmf_subsystem *subsystem);

/**
 * Checks whether a given subsystem is a discovery subsystem
 *
 * \param subsystem Subsystem to check.
 *
 * \return true if a given subsystem is a discovery subsystem, false
 *	   if the subsystem is an nvm subsystem
 */
/*
 * [한국어]
 * spdk_nvmf_subsystem_is_discovery - subsystem 이 Discovery 종류인지 검사.
 *
 * Discovery subsystem 은 IO 명령을 받지 않고 Discovery Log Page 만 제공. NQN 은 보통
 * "nqn.2014-08.org.nvmexpress.discovery".
 */
bool spdk_nvmf_subsystem_is_discovery(struct spdk_nvmf_subsystem *subsystem);


/**
 * Initialize transport options
 *
 * \param transport_name The transport type to create
 * \param opts The transport options (e.g. max_io_size)
 * \param opts_size Must be set to sizeof(struct spdk_nvmf_transport_opts).
 *
 * \return bool. true if successful, false if transport type
 *	   not found.
 */
/*
 * [한국어]
 * spdk_nvmf_transport_opts_init - 트랜스포트별 기본 opts 채움.
 *
 * @transport_name: "RDMA"/"TCP"/"FC"/...
 * @opts: 빈 opts 버퍼.
 * @opts_size: sizeof(spdk_nvmf_transport_opts).
 * @return: 등록된 transport 가 있으면 true.
 *
 * RPC 가 사용자 입력 머지 전에 호출하여 기본값 채움.
 */
bool
spdk_nvmf_transport_opts_init(const char *transport_name,
			      struct spdk_nvmf_transport_opts *opts, size_t opts_size);

/**
 * Create a protocol transport - deprecated, please use \ref spdk_nvmf_transport_create_async.
 *
 * \param transport_name The transport type to create
 * \param opts The transport options (e.g. max_io_size). It should not be NULL, and opts_size
 *        pointed in this structure should not be zero value.
 *
 * \return new transport or NULL if create fails
 */
/*
 * [한국어]
 * spdk_nvmf_transport_create - 동기 트랜스포트 생성 (deprecated).
 *
 * 권장 대체: spdk_nvmf_transport_create_async. 비동기 init 이 필요한 트랜스포트(TCP)는
 * 본 동기 API 로 만들 수 없음.
 */
struct spdk_nvmf_transport *spdk_nvmf_transport_create(const char *transport_name,
		struct spdk_nvmf_transport_opts *opts);

/*
 * [한국어]
 * spdk_nvmf_transport_create_done_cb - 비동기 트랜스포트 생성 완료 콜백 시그니처.
 *
 * @cb_arg: 호출자 opaque. @transport: 새 transport 포인터(실패 시 NULL).
 */
typedef void (*spdk_nvmf_transport_create_done_cb)(void *cb_arg,
		struct spdk_nvmf_transport *transport);

/**
 * Create a protocol transport
 *
 * The callback will be executed asynchronously - i.e. spdk_nvmf_transport_create_async will always return
 * prior to `cb_fn` being called.
 *
 * \param transport_name The transport type to create
 * \param opts The transport options (e.g. max_io_size). It should not be NULL, and opts_size
 *        pointed in this structure should not be zero value.
 * \param cb_fn A callback that will be called once the transport is created
 * \param cb_arg A context argument passed to cb_fn.
 *
 * \return 0 on success, or negative errno on failure (`cb_fn` will not be executed then).
 */
/*
 * [한국어]
 * spdk_nvmf_transport_create_async - 비동기 트랜스포트 생성. 권장 API.
 *
 * @transport_name: 트랜스포트 이름. @opts: 옵션. @cb_fn: 완료 콜백. @cb_arg: opaque.
 * @return: 0 시작 성공, 음수 errno (이 경우 cb_fn 호출 안 됨).
 *
 * 결과는 cb_fn 으로만 통지. 함수 자체는 항상 콜백 전에 반환. TCP accept thread 등 비동기
 * setup 에 적합.
 */
int spdk_nvmf_transport_create_async(const char *transport_name,
				     struct spdk_nvmf_transport_opts *opts,
				     spdk_nvmf_transport_create_done_cb cb_fn, void *cb_arg);

/*
 * [한국어]
 * spdk_nvmf_transport_destroy_done_cb - 트랜스포트 파괴 완료 콜백.
 */
typedef void (*spdk_nvmf_transport_destroy_done_cb)(void *cb_arg);

/**
 * Destroy a protocol transport
 *
 * \param transport The transport to destroy
 * \param cb_fn A callback that will be called once the transport is destroyed
 * \param cb_arg A context argument passed to cb_fn.
 *
 * \return 0 always (left in for API compatibility)
 */
/*
 * [한국어]
 * spdk_nvmf_transport_destroy - 트랜스포트 비동기 파괴.
 *
 * @return: 항상 0 (호환성).
 */
int spdk_nvmf_transport_destroy(struct spdk_nvmf_transport *transport,
				spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg);

/**
 * Get an existing transport from the target
 *
 * \param tgt The NVMe-oF target
 * \param transport_name The name of the transport type to get.
 *
 * \return the transport or NULL if not found
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_get_transport - target 에 등록된 transport 를 이름으로 lookup.
 */
struct spdk_nvmf_transport *spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt,
		const char *transport_name);

/**
 * Get the first transport registered with the given target
 *
 * \param tgt The NVMe-oF target
 *
 * \return The first transport registered on the target
 */
/*
 * [한국어]
 * spdk_nvmf_transport_get_first - target 의 첫 transport 반환(iterator 시작).
 */
struct spdk_nvmf_transport *spdk_nvmf_transport_get_first(struct spdk_nvmf_tgt *tgt);

/**
 * Get the next transport in a target's list.
 *
 * \param transport A handle to a transport object
 *
 * \return The next transport associated with the NVMe-oF target
 */
/*
 * [한국어]
 * spdk_nvmf_transport_get_next - transport 순회 다음 항목.
 */
struct spdk_nvmf_transport *spdk_nvmf_transport_get_next(struct spdk_nvmf_transport *transport);

/**
 * Get the opts for a given transport.
 *
 * \param transport The transport to query
 *
 * \return The opts associated with the given transport
 */
/*
 * [한국어]
 * spdk_nvmf_get_transport_opts - 트랜스포트 인스턴스의 opts(read-only) 반환.
 */
const struct spdk_nvmf_transport_opts *spdk_nvmf_get_transport_opts(struct spdk_nvmf_transport
		*transport);

/**
 * Get the transport type for a given transport.
 *
 * \param transport The transport to query
 *
 * \return the transport type for the given transport
 */
/*
 * [한국어]
 * spdk_nvmf_get_transport_type - 트랜스포트의 NVMe spec 정의 타입 반환(SPDK_NVME_TRANSPORT_*).
 */
spdk_nvme_transport_type_t spdk_nvmf_get_transport_type(struct spdk_nvmf_transport *transport);

/**
 * Get the transport name for a given transport.
 *
 * \param transport The transport to query
 *
 * \return the transport name for the given transport
 */
/*
 * [한국어]
 * spdk_nvmf_get_transport_name - 트랜스포트 이름 문자열 반환.
 */
const char *spdk_nvmf_get_transport_name(struct spdk_nvmf_transport *transport);

/**
 * Function to be called once transport add is complete
 *
 * \param cb_arg Callback argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_add_transport_done_fn - transport 추가 비동기 완료 콜백.
 */
typedef void (*spdk_nvmf_tgt_add_transport_done_fn)(void *cb_arg, int status);

/**
 * Add a transport to a target
 *
 * \param tgt The NVMe-oF target
 * \param transport The transport to add
 * \param cb_fn A callback that will be called once the transport is created
 * \param cb_arg A context argument passed to cb_fn.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_add_transport - 만들어진 transport 인스턴스를 target 에 등록.
 *
 * 등록 후에야 listen / subsystem associate 가 가능. 비동기 — 모든 poll_group 에 sub-group 을
 * 분배해야 함.
 */
void spdk_nvmf_tgt_add_transport(struct spdk_nvmf_tgt *tgt,
				 struct spdk_nvmf_transport *transport,
				 spdk_nvmf_tgt_add_transport_done_fn cb_fn,
				 void *cb_arg);

/**
 * Function to be called once target pause is complete.
 *
 * \param cb_arg Callback argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_pause_polling_cb_fn - target 폴링 일시정지 완료 콜백 시그니처.
 */
typedef void (*spdk_nvmf_tgt_pause_polling_cb_fn)(void *cb_arg, int status);

/**
 * Pause polling on the given target.
 *
 * \param tgt The target to pause
 * \param cb_fn A callback that will be called once the target is paused
 * \param cb_arg A context argument passed to cb_fn.
 *
 * \return 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_pause_polling - target 의 모든 poll_group 일시정지.
 *
 * 사용 사례: live config 변경, snapshot 등. resume_polling 까지 새 명령 처리 안 됨.
 */
int spdk_nvmf_tgt_pause_polling(struct spdk_nvmf_tgt *tgt, spdk_nvmf_tgt_pause_polling_cb_fn cb_fn,
				void *cb_arg);

/**
 * Function to be called once target resume is complete.
 *
 * \param cb_arg Callback argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_resume_polling_cb_fn - target resume 완료 콜백 시그니처.
 */
typedef void (*spdk_nvmf_tgt_resume_polling_cb_fn)(void *cb_arg, int status);

/**
 * Resume polling on the given target.
 *
 * \param tgt The target to resume
 * \param cb_fn A callback that will be called once the target is resumed
 * \param cb_arg A context argument passed to cb_fn.
 *
 * \return 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_resume_polling - pause 의 역연산.
 */
int spdk_nvmf_tgt_resume_polling(struct spdk_nvmf_tgt *tgt,
				 spdk_nvmf_tgt_resume_polling_cb_fn cb_fn, void *cb_arg);

/**
 * Add listener to transport and begin accepting new connections.
 *
 * \param transport The transport to add listener to.
 * \param trid The address to listen at.
 * \param opts Listener options.
 *
 * \return int. 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_nvmf_transport_listen - transport 인스턴스 레벨에서 listen 시작 (저수준).
 *
 * 일반적으로는 spdk_nvmf_tgt_listen_ext 가 더 편리(target 레벨). 저수준 사용 시 transport
 * 별 ops->listen 을 직접 호출하는 효과.
 */
int
spdk_nvmf_transport_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid, struct spdk_nvmf_listen_opts *opts);

/**
 * Remove listener from transport and stop accepting new connections.
 *
 * \param transport The transport to remove listener from
 * \param trid Address to stop listen at
 *
 * \return int. 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_nvmf_transport_stop_listen - transport 레벨 listen 중지. 기존 qpair 는 유지.
 */
int
spdk_nvmf_transport_stop_listen(struct spdk_nvmf_transport *transport,
				const struct spdk_nvme_transport_id *trid);

/**
 * Stop accepting new connections at the provided address.
 *
 * This is a counterpart to spdk_nvmf_tgt_listen_ext(). It differs
 * from spdk_nvmf_transport_stop_listen() in that it also destroys
 * qpairs that are connected to the specified listener. Because
 * this function disconnects the qpairs, it has to be asynchronous.
 *
 * The subsystem is matched using the subsystem parameter, not the
 * subnqn field in the trid.
 *
 * \param transport The transport associated with the listen address.
 * \param trid The address to stop listening at. subnqn must be an empty
 *             string.
 * \param subsystem The subsystem to match for qpairs with the specified
 *                  trid. If NULL, it will disconnect all qpairs with the
 *                  specified trid.
 * \param cb_fn The function to call on completion.
 * \param cb_arg The argument to pass to the cb_fn.
 *
 * \return int. 0 when the asynchronous process starts successfully or a negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_transport_stop_listen_async - listen 중지 + 그 trid 의 모든 qpair 끊기.
 *
 * @transport: 대상. @trid: 중지할 주소(subnqn 빈 문자열). @subsystem: NULL 이면 모든 subsystem
 *             의 qpair 정리, 비-NULL 이면 그 subsystem 만.
 *
 * stop_listen 의 강한 변형. multipath fail-over 시 한 path 의 listener 와 그 위의 모든
 * 호스트 연결을 한 번에 정리. 비동기.
 */
int spdk_nvmf_transport_stop_listen_async(struct spdk_nvmf_transport *transport,
		const struct spdk_nvme_transport_id *trid,
		struct spdk_nvmf_subsystem *subsystem,
		spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
		void *cb_arg);

/**
 * Dump poll group statistics into JSON.
 *
 * \param group The group which statistics should be dumped.
 * \param w The JSON write context to which statistics should be dumped.
 */
/*
 * [한국어]
 * spdk_nvmf_poll_group_dump_stat - poll_group 의 통계를 JSON 으로 직렬화 (RPC 응답).
 *
 * 호출 컨텍스트: poll_group 의 thread (RPC 가 spdk_thread_send_msg 로 위임).
 */
void spdk_nvmf_poll_group_dump_stat(struct spdk_nvmf_poll_group *group,
				    struct spdk_json_write_ctx *w);

/**
 * \brief Set the global hooks for the RDMA transport, if necessary.
 *
 * This call is optional and must be performed prior to probing for
 * any devices. By default, the RDMA transport will use the ibverbs
 * library to create protection domains and register memory. This
 * is a mechanism to subvert that and use an existing registration.
 *
 * This function may only be called one time per process.
 *
 * \param hooks for initializing global hooks
 */
/*
 * [한국어]
 * spdk_nvmf_rdma_init_hooks - RDMA 트랜스포트의 ibverbs 사용을 사용자 지정 hook 으로 대체.
 *
 * @hooks: 사용자 정의 PD/MR 등록 함수 묶음.
 *
 * 기본 RDMA 트랜스포트는 ibverbs 로 PD(Protection Domain) 와 MR(Memory Region) 을 만든다.
 * 이미 다른 모듈이 PD 를 만들어 두었다면 본 함수로 hook 등록 → RDMA 트랜스포트가 그 PD/MR
 * 을 재사용. 프로세스 당 1 회만 가능, RDMA probe 전에 호출.
 */
void spdk_nvmf_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks);

/* Maximum number of registrants supported per namespace */
#define SPDK_NVMF_MAX_NUM_REGISTRANTS		16
/* [한국어] 한 namespace 가 보유 가능한 NVMe Reservation registrant 수의 상한. NVMe spec 의
 * Reservation Notification 메시지 크기와 SPDK 의 PTPL 파일 포맷에 영향. */

/*
 * [한국어]
 * struct spdk_nvmf_registrant_info - NVMe Reservation registrant 1 명의 직렬화 정보.
 *
 * Reservation: NVMe SCSI-like 보호 메커니즘 — 등록한 host(registrant) 만 read/write 가능.
 * register/release/acquire/clear 명령으로 관리.
 */
struct spdk_nvmf_registrant_info {
	uint64_t		rkey;
	/* [한국어] Reservation Key (8B). host 가 register 시 제공한 비밀번호 역할.
	 * 설정자: NVMe Reservation Register 명령. 읽는 자: PTPL save/load.
	 * 동기화: ns 단위 mutex 보호. */

	char			host_uuid[SPDK_UUID_STRING_LEN];
	/* [한국어] registrant 의 host UUID 문자열. 호스트 식별. */
};

/*
 * [한국어]
 * struct spdk_nvmf_reservation_info - namespace 의 reservation 전체 상태(직렬화용).
 *
 * PTPL(Persist Through Power Loss) 활성 시 본 구조체가 파일에 직렬화되어 재시작 후 복원.
 */
struct spdk_nvmf_reservation_info {
	uint64_t				crkey;
	/* [한국어] 현재 reservation holder 의 key. 0 이면 reservation 없음. */

	uint8_t					rtype;
	/* [한국어] Reservation Type — Write Exclusive / Exclusive Access / *_Registrants_Only /
	 * *_All_Registrants. NVMe spec Figure. */

	uint8_t					ptpl_activated;
	/* [한국어] true 면 이 reservation 이 PTPL 정책으로 활성됨(전원 OFF 후 복원). */

	char					bdev_uuid[SPDK_UUID_STRING_LEN];
	/* [한국어] backing bdev UUID. 복원 시 동일 bdev 매칭 검증. */

	char					holder_uuid[SPDK_UUID_STRING_LEN];
	/* [한국어] reservation holder host UUID. */

	uint8_t					reserved[3];
	/* [한국어] 정렬 reserved. */

	uint8_t					num_regs;
	/* [한국어] registrants 배열의 유효 개수(0..SPDK_NVMF_MAX_NUM_REGISTRANTS). */

	struct spdk_nvmf_registrant_info	registrants[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	/* [한국어] 등록된 registrants 정보 배열. */
};

/*
 * [한국어]
 * struct spdk_nvmf_ns_reservation_ops - reservation 영속성(PTPL) 정책 후크.
 *
 * 기본 구현은 ptpl_file 파일을 읽고 쓰지만, 사용자가 다른 저장소(외부 KV, etcd 등) 에
 * 보관하고 싶을 때 본 ops 를 등록하여 default 동작을 대체.
 */
struct spdk_nvmf_ns_reservation_ops {
	/* Checks if the namespace supports the Persist Through Power Loss capability. */
	bool (*is_ptpl_capable)(const struct spdk_nvmf_ns *ns);
	/* [한국어] 이 ns 가 PTPL 을 지원하는지 검사. backing bdev 이 영속 저장이 가능한지 등.
	 * 호출 컨텍스트: ns 추가 시. */

	/* Called when namespace reservation information needs to be updated.
	 * The new reservation information is provided via the info parameter.
	 * Returns 0 on success, negated errno on failure. */
	int (*update)(const struct spdk_nvmf_ns *ns, const struct spdk_nvmf_reservation_info *info);
	/* [한국어] reservation 변경 시 호출 — 사용자가 외부 저장소에 직렬화. 0 성공/음수 실패.
	 * 동기화: ns reservation mutex 보호. */

	/* Called when restoring the namespace reservation information.
	 * The new reservation information is returned via the info parameter.
	 * Returns 0 on success, negated errno on failure. */
	int (*load)(const struct spdk_nvmf_ns *ns, struct spdk_nvmf_reservation_info *info);
	/* [한국어] target 시작 시 호출 — 외부 저장소에서 복원. */
};

/**
 * Set custom handlers for namespace reservation operations.
 *
 * This call allows to override the default namespace reservation operations with custom handlers.
 * This function may only be called before any namespace has been added.
 *
 * @param ops The reservation ops handers
 */
/*
 * [한국어]
 * spdk_nvmf_set_custom_ns_reservation_ops - 전역 reservation ops 등록.
 *
 * @ops: 사용자 정의 후크. NULL 이면 기본(파일 기반) 으로 폴백.
 *
 * 호출 시점: 어떤 ns 도 추가되기 전. 후 추가되는 모든 ns 에 본 ops 가 적용된다.
 */
void spdk_nvmf_set_custom_ns_reservation_ops(const struct spdk_nvmf_ns_reservation_ops *ops);

/**
 * Send discovery log page change AEN.
 *
 * This sends discovery log page change notice to all the controllers in the
 * target's discovery subsystem associated with host 'hostnqn'.
 *
 * \param tgt The target for which discovery log page change notice is to be
 *            sent.
 * \param hostnqn The hostnqn to which the notice will be sent. If NULL, all
 *                the controllers associated with discovery subsystem will have
 *                the discovery log page change notice.
 */
/*
 * [한국어]
 * spdk_nvmf_send_discovery_log_notice - Discovery Log Page Change AEN(Async Event Notice) 송신.
 *
 * @tgt: 대상 target.
 * @hostnqn: 통지받을 host NQN. NULL 이면 모든 host 에 송신(broadcast).
 *
 * subsystem 추가/제거 또는 listener 변경이 발생했을 때 호스트에게 "discovery log 가 바뀌었으니
 * 다시 받으라" 고 알리는 용도. 호스트는 응답으로 새 discovery 명령을 보내 최신 endpoint 목록을 받는다.
 *
 * 호출 체인:
 *   subsystem_create/destroy 등 변경 → app thread → [이 함수] → 보류 AER 완료 → 호스트로 CQE
 */
void spdk_nvmf_send_discovery_log_notice(struct spdk_nvmf_tgt *tgt, const char *hostnqn);

#ifdef __cplusplus
}
/* [한국어] extern "C" 종료. */
#endif

#endif
/* [한국어] include guard 종료. 본 헤더는 SPDK 가 NVMe-oF target 으로서 외부에 제공하는
 * 모든 1차 API 의 진입점이다. RPC, 사용자 애플리케이션, 통합 테스트 모두 이 한 헤더를
 * 시작점으로 한다. */
