/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Nutanix Inc. All rights reserved.
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Nvme block device abstraction layer
 */

/*
 * [한국어 설명] NVMe bdev 모듈의 외부 공개 API (module/bdev/nvme.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 의 module/bdev/nvme/ 모듈(NVMe 컨트롤러 → bdev 변환기)이
 * 자기 모듈 외부에 노출하는 "프로그래밍 가능한 진입점" 을 정의한다.
 * 주된 소비자는 RPC 핸들러 (module/bdev/nvme/bdev_nvme_rpc.c) 와 일부
 * application 코드(예: NVMe-oF target 의 자동 attach 경로)이다. RPC 가
 * `bdev_nvme_attach_controller` JSON 명령을 받으면 본 헤더의
 * spdk_bdev_nvme_create() 를 호출하고, `bdev_nvme_detach_controller` 는
 * spdk_bdev_nvme_delete() 를 호출하는 식으로 1:1 대응한다.
 *
 * 한 마디로: lib/nvme 가 제공하는 "NVMe 컨트롤러 추상화" 를 SPDK bdev 레이어로
 * wrapping 하면서, 멀티 네임스페이스(컨트롤러 1개 → bdev 여러 개), 멀티 패스
 * (NVMe-oF ANA 활성/대기 경로), 옵션 튜닝(timeout, retry, fast_io_fail …) 을
 * 모두 RPC 로 다룰 수 있게 해주는 것이 이 모듈의 사명이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름도에서 본 헤더의 함수들은 "사용자 명령 → bdev 모듈 진입" 의 경계에 있다.
 *   user (CLI/JSON-RPC 클라이언트)
 *     → JSON-RPC 송신 (bdev_nvme_attach_controller)
 *       → app/spdk_tgt/ 의 RPC dispatcher (RPC dispatch thread 에서 실행)
 *         → module/bdev/nvme/bdev_nvme_rpc.c::rpc_bdev_nvme_attach_controller
 *           → [본 헤더] spdk_bdev_nvme_create()
 *             → lib/nvme/ 의 spdk_nvme_probe_async() 등 비동기 컨트롤러 attach
 *               → 완료 시 attach_controller_done(cb_ctx, bdev_count, rc) 콜백
 *
 * I/O 경로(spdk_bdev_read/write) 는 본 헤더와 무관하게 module/bdev/nvme/
 * 내부 bdev_nvme_io_ops 콜백을 통해 흐른다. 본 헤더는 control plane 전용.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/nvme.h (struct spdk_nvme_ctrlr_opts, transport_id, path_id 등
 *   lib/nvme 자료형). 이로써 본 헤더 사용자는 lib/nvme 의 옵션 구조체와
 *   bdev 모듈 옵션 구조체를 모두 손댈 수 있다.
 * - 의존자: module/bdev/nvme/bdev_nvme_rpc.c (가장 큰 호출자), 일부
 *   NVMe-oF discovery 자동 어태치 경로, examples/.
 * - 데이터 흐름: spdk_bdev_nvme_create() 호출 시 trid + drv_opts + bdev_opts
 *   를 전달 → 모듈은 lib/nvme 컨트롤러를 attach → 컨트롤러의 active 네임스페이스
 *   각각에 대해 spdk_bdev 를 생성 → spdk_bdev_register() → bdev 이름들이
 *   names[] 에 채워져 콜백으로 반환. delete 는 역방향.
 * - 공유 자료구조: 호출자(RPC)가 옵션 구조체를 채워서 넘기지만, lib/nvme
 *   가 내부적으로 보관하는 spdk_nvme_ctrlr 은 본 모듈이 ref-count 로 관리.
 *   spdk_nvme_path_id 는 multi-path 식별자(transport_id + subnqn + host_id)를
 *   캡슐화하며, 한 컨트롤러가 여러 path 를 가질 수 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_bdev_nvme_create(): NVMe 컨트롤러 attach + 모든 namespace 를 bdev 로
 *   등록. 비동기 — 결과는 spdk_bdev_nvme_create_cb 콜백으로 반환.
 * - spdk_bdev_nvme_delete(): 컨트롤러(혹은 그중 한 path) 분리 + 관련 bdev 제거.
 * - spdk_bdev_nvme_set_multipath_policy(): 컨트롤러 활성/대기 또는 active-active
 *   라운드로빈/큐 깊이 정책 설정.
 * - spdk_bdev_nvme_get_default_ctrlr_opts(): per-controller 옵션 디폴트 채움.
 * - spdk_bdev_nvme_get_opts() / set_opts(): 모듈 글로벌 옵션 GET/SET.
 * - struct spdk_bdev_nvme_ctrlr_opts: 한 컨트롤러 어태치 시 적용되는 옵션
 *   (PRCHK 플래그, ctrlr_loss_timeout, reconnect_delay, dhchap 인증 키, …).
 * - struct spdk_bdev_nvme_opts: 모듈 전역 동작 옵션 (timeout_us, action_on_timeout,
 *   io_queue_requests, polling 주기, RDMA SRQ size, multipath 자동 failback …).
 * - 콜백 typedef들: spdk_bdev_nvme_create_cb / delete_cb /
 *   set_multipath_policy_cb — 모두 호출자(보통 RPC dispatch thread)에서 실행.
 */

#ifndef SPDK_MODULE_BDEV_NVME_H_
#define SPDK_MODULE_BDEV_NVME_H_

#include "spdk/nvme.h"
/* [한국어] lib/nvme 의 공개 API. struct spdk_nvme_ctrlr_opts,
 * spdk_nvme_transport_id, spdk_nvme_path_id 의 정의를 가져온다. 본 헤더의
 * 다수 함수가 이들을 인자로 받기 때문에 필수. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 컴파일러로 인클루드되어도 C 링크리지(이름 맹글링 없음) 보장. */
#endif

/*
 * [한국어]
 * spdk_bdev_nvme_create_cb - bdev_nvme_create() 완료 콜백 시그니처
 *
 * @ctx:        호출자가 spdk_bdev_nvme_create() 에 넘긴 cb_ctx — 보통 RPC
 *              요청을 표현하는 구조체 포인터(JSON 응답 작성에 필요).
 * @bdev_count: 이번 어태치로 새로 만들어진 bdev 의 수. 이미 존재하는 컨트롤러를
 *              재어태치한 경우 0 일 수도 있음.
 * @rc:         0=성공, 음수 errno=실패. 실패 시 RPC 핸들러는 JSON 에러 응답을
 *              만들어 클라이언트에 회신.
 *
 * 실행 컨텍스트: lib/nvme 의 비동기 attach 가 완료되는 SPDK 스레드(보통
 * RPC dispatch thread 와 동일하지만 구현에 따라 다를 수 있음). 콜백 안에서
 * I/O hot path 를 차단할 수 있는 작업은 피해야 한다.
 */
typedef void (*spdk_bdev_nvme_create_cb)(void *ctx, size_t bdev_count, int rc);

/*
 * [한국어]
 * spdk_bdev_nvme_set_multipath_policy_cb - multipath 정책 변경 완료 콜백
 *
 * @cb_arg: 호출자가 set_multipath_policy() 에 넘긴 컨텍스트.
 * @rc:     0=성공, 음수 errno=실패.
 *
 * 정책 변경은 컨트롤러 그룹의 모든 IO 채널을 순회해야 하므로 비동기로 수행되며,
 * 마지막 채널 처리가 끝난 스레드에서 본 콜백이 호출된다.
 */
typedef void (*spdk_bdev_nvme_set_multipath_policy_cb)(void *cb_arg, int rc);

/*
 * [한국어]
 * spdk_bdev_nvme_delete_cb - bdev_nvme_delete() 완료 콜백 시그니처
 *
 * @ctx: 호출자가 delete() 에 넘긴 컨텍스트 (RPC 응답 객체 등).
 * @rc:  0=성공, 음수 errno=실패. 컨트롤러가 in-progress 로 이미 분리 중이라면
 *       함수는 success 를 반환하고 본 콜백도 정상 호출된다.
 *
 * 실행 컨텍스트: lib/nvme detach 완료 스레드(일반적으로 RPC dispatch thread).
 * NULL 가능 — 콜백을 등록하지 않은 fire-and-forget 모드.
 */
typedef void (*spdk_bdev_nvme_delete_cb)(void *ctx, int rc);

/*
 * [한국어]
 * enum spdk_bdev_nvme_multipath_policy - NVMe multipath 정책 종류
 *
 * NVMe-oF 또는 dual-port NVMe SSD 에서 같은 namespace 에 도달하는 여러
 * controller path 를 어떻게 운용할지 결정한다. NVMe ANA(Asymmetric
 * Namespace Access) 와도 연관 — ACTIVE_PASSIVE 는 ANA "optimized" 1개를
 * 골라 쓰고 fail-over, ACTIVE_ACTIVE 는 여러 path 에 동시에 분산.
 */
enum spdk_bdev_nvme_multipath_policy {
	BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE,
	/* [한국어] 한 시점에 하나의 path 만 활성. 활성 path 실패 시 다른 path 로
	 * fail-over. 단순하고 안전 — 컨트롤러 간 캐시 일관성/순서 보장 이슈를
	 * 회피. NVMe-oF 의 보수적인 기본값. */

	BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE,
	/* [한국어] 여러 path 를 동시에 활성으로 두고 I/O 를 분산. 동작 방식은
	 * spdk_bdev_nvme_multipath_selector (round-robin 또는 queue-depth 기반)
	 * 로 결정. 처리량 향상에 유리하나 컨트롤러 간 캐시 동기화 가정이 필요. */
};

/*
 * [한국어]
 * enum spdk_bdev_nvme_multipath_selector - active-active 시 path 선택 알고리즘
 *
 * ACTIVE_ACTIVE 정책에서만 의미 있다. 1 부터 시작하는 이유는 0 을 "미설정"
 * 으로 취급하는 RPC 파라미터 처리 컨벤션 때문.
 */
enum spdk_bdev_nvme_multipath_selector {
	BDEV_NVME_MP_SELECTOR_ROUND_ROBIN = 1,
	/* [한국어] 라운드 로빈 — 라운드당 일정 개수(rr_min_io)의 I/O 를 한 path
	 * 에 보낸 뒤 다음 path 로 전환. 단순/공정. */

	BDEV_NVME_MP_SELECTOR_QUEUE_DEPTH,
	/* [한국어] 각 path 의 in-flight 큐 깊이를 비교해 가장 한가한 path 로
	 * 라우팅. latency 변동이 큰 경로(혼잡한 RDMA 등) 에서 응답 향상. */
};

/*
 * [한국어]
 * struct spdk_bdev_nvme_ctrlr_opts - 단일 NVMe 컨트롤러 어태치 시 옵션
 *
 * spdk_bdev_nvme_create() 호출 직전에 RPC 핸들러가 채워서 넘기는 옵션 묶음.
 * 컨트롤러 단위(per-ctrlr)로만 의미 있는 값들이 모여 있다 (전역 옵션은 별도
 * spdk_bdev_nvme_opts).
 */
struct spdk_bdev_nvme_ctrlr_opts {
	uint32_t prchk_flags;
	/* [한국어] PRCHK (Protection Information Check) 플래그 — NVMe 의 end-to-end
	 * data protection 기능에서 어떤 종류의 검사를 활성화할지 지정. PRACT,
	 * PRCHK_REFTAG, PRCHK_APPTAG, PRCHK_GUARD 비트 OR. T10 DIF 사용 SSD 에서만 의미. */

	int32_t ctrlr_loss_timeout_sec;
	/* [한국어] 컨트롤러와의 연결이 끊어진 후 reconnect 시도를 포기하기까지의
	 * 최대 시간(초). -1 = 무한 재시도, 0 = 재시도 안 함. 재시도 동안 I/O 는
	 * 큐잉되거나 fast_io_fail_timeout 에 따라 실패 처리. */

	uint32_t reconnect_delay_sec;
	/* [한국어] reconnect 시도 간 간격(초). 너무 짧으면 폭주, 너무 길면 복구 지연.
	 * NVMe-oF TCP 에서 일반적으로 10~30 초. */

	uint32_t fast_io_fail_timeout_sec;
	/* [한국어] 컨트롤러 loss 상태에서 I/O 를 빠르게 실패시킬 때까지의 시간(초).
	 * ctrlr_loss_timeout 보다 짧게 설정하면 일찍 호출자에 errno 를 돌려주어
	 * 상위 multipath/볼륨 매니저가 fail-over 를 빠르게 트리거할 수 있다. */

	bool from_discovery_service;
	/* [한국어] 이 어태치가 NVMe-oF Discovery service 를 통해 자동 발견된 결과
	 * 인지 표시. true 면 모듈이 일부 정책(예: 자동 정리 시점)을 다르게 처리. */

	const char *psk;
	/* [한국어] TLS Pre-Shared Key — NVMe-oF TCP TLS 연결 시 사용할 PSK 식별자/
	 * 키 문자열. NULL 이면 TLS 미사용. */

	const char *dhchap_key;
	/* [한국어] DH-HMAC-CHAP 인증 (NVMe-oF Authentication) 의 host 측 key 식별자.
	 * 컨트롤러가 인증을 요구할 때 사용. */

	const char *dhchap_ctrlr_key;
	/* [한국어] DH-HMAC-CHAP 의 controller(target) 측 key 식별자 — 양방향 인증 시
	 * host 가 controller 의 신원도 검증할 때 사용. */

	/**
	 * Allow attaching namespaces with unrecognized command set identifiers.
	 * These will only support NVMe passthrough.
	 */
	bool allow_unrecognized_csi;
	/* [한국어] 스펙에 정의되지 않았거나 이 SPDK 빌드가 인식하지 못하는 Command
	 * Set Identifier(CSI; NVM=0x00, KV=0x01, Zoned=0x02 외 미래 확장)의
	 * namespace 도 등록을 허용할지. true 면 R/W 같은 일반 bdev 연산은 안 되고
	 * NVMe passthrough(spdk_bdev_nvme_admin_passthru 류)만 지원되는 bdev 가 생성. */

	/* Set to true if multipath enabled */
	bool multipath;
	/* [한국어] 이 컨트롤러가 multipath 그룹에 포함될지. 동일한 NQN/namespace 를
	 * 가진 다른 컨트롤러와 합쳐져 하나의 bdev 로 노출된다. false 면 path 별로
	 * 별도 bdev 생성. */
};

struct spdk_nvme_path_id;
/* [한국어] lib/nvme 가 정의한 multi-path 식별자(불투명 핸들). 한 컨트롤러는
 * (transport_id, subnqn, host_id) 조합으로 구분되는 여러 path 를 가질 수 있고,
 * spdk_bdev_nvme_delete() 에서 특정 path 만 제거하기 위해 본 타입을 사용한다.
 * 정의는 spdk/nvme.h 에 있으나, 본 헤더에서는 포인터로만 다루므로
 * forward declaration 으로 충분. */

/*
 * [한국어]
 * enum spdk_bdev_timeout_action - I/O timeout 발생 시 NVMe bdev 의 행동
 *
 * spdk_bdev_nvme_opts.action_on_timeout 에 들어가는 값. 컨트롤러가 timeout_us
 * 안에 응답하지 않은 명령에 대해 모듈이 어떤 조치를 취할지 결정.
 */
enum spdk_bdev_timeout_action {
	SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE = 0,
	/* [한국어] timeout 시 아무 조치도 취하지 않음 — 호출자가 알아서 처리.
	 * 디폴트(0)이며 RPC 미지정 시 적용. */

	SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET,
	/* [한국어] timeout 발생 시 컨트롤러 reset 수행 — 모든 SQ/CQ 폐기 및
	 * 컨트롤러 재초기화. 강력하지만 영향 범위 큼. */

	SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT,
	/* [한국어] 해당 명령에 대해 NVMe Abort admin command 발사. 컨트롤러가
	 * abort 를 지원할 때만 효과. */
};

/*
 * [한국어]
 * struct spdk_bdev_nvme_opts - NVMe bdev 모듈의 전역(글로벌) 동작 옵션
 *
 * 모듈 1회당 하나의 인스턴스만 적용되며, spdk_bdev_nvme_set_opts() 로 변경.
 * RPC `bdev_nvme_set_options` 의 백엔드. opts_size 필드를 통해 구버전 호출자가
 * 더 작은 sizeof 를 보내도 라이브러리가 안전하게 처리한다.
 */
struct spdk_bdev_nvme_opts {
	/**
	 * The size of spdk_bdev_nvme_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
	/* [한국어] 호출자가 알고 있는 sizeof(spdk_bdev_nvme_opts). 라이브러리는 이 값보다
	 * 큰 오프셋의 필드는 호출자가 채우지 않은 것으로 보고 디폴트로 채움. ABI 호환성을
	 * 위해 새 필드는 반드시 구조체 끝에 append 한다. */

	enum spdk_bdev_timeout_action action_on_timeout;
	/* [한국어] timeout 행동 정책 (위 enum 참고). */

	uint32_t keep_alive_timeout_ms;
	/* [한국어] NVMe Keep Alive 타이머(밀리초). 이 시간 안에 호스트→컨트롤러
	 * 또는 그 반대 방향으로 keep-alive 가 오가지 않으면 연결 손실로 판단.
	 * NVMe-oF 에서 특히 중요. */

	uint64_t timeout_us;
	/* [한국어] I/O 명령의 타임아웃(마이크로초). 0 = 비활성. 모든 SQ 명령(R/W
	 * 등)이 이 시간 안에 완료되지 않으면 action_on_timeout 적용. */

	uint64_t timeout_admin_us;
	/* [한국어] Admin 명령의 별도 타임아웃(마이크로초). 일반 I/O 와 다른 값을
	 * 적용하고 싶을 때 사용 (보통 admin 이 더 길게). */

	/* The number of attempts per I/O in the transport layer before an I/O fails. */
	uint32_t transport_retry_count;
	/* [한국어] transport layer (lib/nvme) 에서 한 I/O 를 재전송하는 최대 횟수.
	 * 이 횟수 초과 시 transport 가 실패를 반환하며, 다음 단계(bdev_retry_count)
	 * 가 작동. */

	uint32_t arbitration_burst;
	/* [한국어] NVMe Set Features (Feature Identifier 0x01: Arbitration) 의
	 * Arbitration Burst 값 — 라운드로빈 중재기에서 한 번에 처리할 명령 수. */

	uint32_t low_priority_weight;
	/* [한국어] Weighted Round Robin 중재기에서 low priority 큐 가중치.
	 * 컨트롤러가 WRR 중재를 지원할 때만 유효. */

	uint32_t medium_priority_weight;
	/* [한국어] WRR medium priority 큐 가중치. */

	uint32_t high_priority_weight;
	/* [한국어] WRR high priority 큐 가중치. */

	uint32_t io_queue_requests;
	/* [한국어] I/O 큐 1개당 outstanding 가능한 최대 명령 수 (= SQ depth).
	 * 너무 작으면 처리량 손실, 너무 크면 도어벨/CQ 처리 부담. */

	uint64_t nvme_adminq_poll_period_us;
	/* [한국어] Admin queue 의 폴러(poller) 호출 주기(마이크로초). 너무 짧으면
	 * CPU 낭비, 길면 admin 응답 지연. */

	uint64_t nvme_ioq_poll_period_us;
	/* [한국어] I/O queue 의 폴러 주기. 0 = 매 reactor 사이클마다 폴링 (= busy-poll).
	 * SPDK 는 보통 0 (즉, 무조건 매 라운드 폴링)으로 두어 polled-mode 의 의미를 살림. */

	bool delay_cmd_submit;
	/* [한국어] 도어벨 라이트를 한 reactor 사이클 끝까지 모아 한 번에 발사할지
	 * 결정 (도어벨 batching). true 면 latency 가 약간 늘 수 있지만 도어벨
	 * MMIO 횟수가 줄어 처리량 향상. */

	/* Hole at bytes 73-75. */
	uint8_t reserved73[3];
	/* [한국어] 컴파일러 정렬을 위한 패딩 자리. 신규 필드를 추가할 거면 여기
	 * 또는 구조체 끝에. */

	/* The number of attempts per I/O in the bdev layer before an I/O fails. */
	int32_t bdev_retry_count;
	/* [한국어] bdev layer 에서 (transport 실패 후) 같은 I/O 를 다시 큐잉할
	 * 최대 횟수. -1 = 무한, 0 = 재시도 안 함. */

	int32_t ctrlr_loss_timeout_sec;
	/* [한국어] 전역 디폴트 ctrlr_loss_timeout — 개별 ctrlr_opts 에서 미지정 시 적용. */

	uint32_t reconnect_delay_sec;
	/* [한국어] 전역 디폴트 reconnect_delay_sec — 동일 의미. */

	uint32_t fast_io_fail_timeout_sec;
	/* [한국어] 전역 디폴트 fast_io_fail_timeout_sec — 동일 의미. */

	uint8_t transport_ack_timeout;
	/* [한국어] RDMA 등 transport 의 ack timeout (transport-specific 단위). */

	bool disable_auto_failback;
	/* [한국어] 활성-대기 multipath 에서 원래 활성 path 가 복구되었을 때 자동으로
	 * 되돌릴지 여부. true 면 수동으로만 복귀(stable 환경 유지). */

	bool generate_uuids;
	/* [한국어] 이 모듈이 생성하는 bdev 들에 자동 UUID 부여 여부. */

	/* Type of Service - RDMA only */
	uint8_t transport_tos;
	/* [한국어] RDMA TOS(Type of Service) 값. QoS 클래스 매핑에 사용. */

	bool nvme_error_stat;
	/* [한국어] 컨트롤러 에러 카운터(SMART, error-log) 통계 수집 활성. */

	bool io_path_stat;
	/* [한국어] multipath I/O path 별 통계 수집 활성. */

	bool allow_accel_sequence;
	/* [한국어] SPDK accel 프레임워크의 sequence (DIF, copy, crypto 체인) 를
	 * NVMe submit 직전에 인라인으로 처리하도록 허용할지. */

	/* Hole at byte 99. */
	uint8_t reserved99[1];
	/* [한국어] 정렬용 패딩 1바이트. */

	uint32_t rdma_srq_size;
	/* [한국어] RDMA SRQ(Shared Receive Queue) 의 슬롯 수. transport=RDMA 일 때만 유효. */

	uint32_t rdma_max_cq_size;
	/* [한국어] RDMA CQ(Completion Queue) 최대 크기. */

	uint16_t rdma_cm_event_timeout_ms;
	/* [한국어] RDMA CM 이벤트 처리 타임아웃(밀리초). connect/disconnect 등 CM
	 * 이벤트가 이 시간 내 도착하지 않으면 실패 처리. */

	/* Hole at bytes 110-111. */
	uint8_t reserved110[2];
	/* [한국어] 정렬용 패딩 2바이트. */

	uint32_t dhchap_digests;
	/* [한국어] DH-HMAC-CHAP 인증에서 허용할 digest 알고리즘 비트 마스크
	 * (SHA-256, SHA-384, SHA-512 등). */

	uint32_t dhchap_dhgroups;
	/* [한국어] DH-HMAC-CHAP 에서 허용할 DH group 비트 마스크. */

	bool rdma_umr_per_io;
	/* [한국어] RDMA UMR (User-Mode Memory Registration) 를 I/O 마다 사용할지.
	 * scatter-gather buffer 가 자주 바뀔 때 도움. */

	/* Hole at bytes 121-123. */
	uint8_t reserved121[3];
	/* [한국어] 정렬용 패딩 3바이트. */

	uint32_t tcp_connect_timeout_ms;
	/* [한국어] TCP transport 의 connect() 타임아웃(밀리초). NVMe-oF TCP 에서
	 * 초기 핸드셰이크 단계 한정. */

	bool enable_flush;
	/* [한국어] 모듈이 spdk_bdev_flush 를 지원할지. NVMe Flush 명령으로 매핑. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_nvme_opts) == 136, "Incorrect size");
/* [한국어] 빌드 타임 어서션 — 구조체의 정확한 sizeof 를 박제하여 누군가 위치/타입
 * 변경 시 즉시 컴파일 에러로 발견하게 한다. ABI 호환성 핵심 보호 장치. */

/**
 * Connect to the NVMe controller and populate namespaces as bdevs.
 *
 * \param trid Transport ID for nvme controller.
 * \param base_name Base name for the nvme subsystem.
 * \param names Pointer to string array to get bdev names.
 * \param count Maximum count of the string array 'names'. Restricts the length
 *		of 'names' array only, not the count of bdevs created.
 * \param cb_fn Callback function to be called after all the bdevs are created
 *              or updated if already created.
 * \param cb_ctx Context to pass to cb_fn.
 * \param drv_opts NVMe driver options.
 * \param bdev_opts NVMe bdev options.
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_bdev_nvme_create - NVMe 컨트롤러 어태치 + 모든 namespace 를 bdev 로 등록 (RPC 백엔드)
 *
 * @trid:      lib/nvme 의 transport identifier — PCIe traddr (0000:81:00.0) 또는
 *             RDMA/TCP 의 trsvcid+traddr+subnqn 등을 캡슐화. RPC JSON 입력에서 채워짐.
 * @base_name: 생성될 bdev 들의 prefix. 예: base_name="Nvme0" 이면 namespace 1,2,3 에
 *             대해 bdev 이름은 "Nvme0n1", "Nvme0n2", "Nvme0n3" 이 된다.
 * @names:     생성된 bdev 의 이름들이 채워질 출력 문자열 포인터 배열.
 *             RPC 가 JSON 응답에 그대로 포함시킨다.
 * @count:     names[] 배열의 크기. 실제 생성되는 bdev 수가 이보다 많아도 호출은
 *             성공하지만 names[] 의 앞 count 개만 채워진다.
 * @cb_fn:     모든 bdev 생성/갱신 완료 시 호출되는 비동기 콜백.
 * @cb_ctx:    cb_fn 의 첫 인자로 전달될 사용자 컨텍스트.
 * @drv_opts:  lib/nvme 레벨 옵션 (struct spdk_nvme_ctrlr_opts) — 큐 수, 트래픽 타입 등.
 * @bdev_opts: bdev 모듈 레벨 옵션 (위 struct spdk_bdev_nvme_ctrlr_opts) — multipath,
 *             dhchap 키, ctrlr_loss_timeout 등.
 * @return:    0 = attach 시작 성공 (실패는 cb_fn 의 rc 로 보고),
 *             음수 errno = 즉시 실패 (잘못된 인자 등).
 *
 * 동작 단계:
 *   1) 인자 검증 (base_name 중복, trid 유효성, opts size 등).
 *   2) lib/nvme 의 spdk_nvme_probe_async() 또는 connect_async() 호출 — 컨트롤러
 *      identify, set features, 큐 생성 등이 비동기로 진행.
 *   3) attach 완료 콜백에서 active namespace 를 순회하여 각각 spdk_bdev 생성
 *      및 spdk_bdev_register() 등록.
 *   4) 모든 등록 완료 후 cb_fn(cb_ctx, bdev_count, 0) 호출.
 *
 * 실행 컨텍스트: 일반적으로 RPC dispatch thread 에서 호출된다 (spdk_tgt 의
 * RPC 핸들러). 이 함수 자체는 비동기 — 즉시 0 을 돌려주고 attach 진행은
 * NVMe poller 가 이어서 처리한다. cb_fn 은 attach 가 끝나는 그 스레드에서
 * 호출되는데, 보통 같은 RPC dispatch thread 다.
 *
 * 호출 체인:
 *   rpc_bdev_nvme_attach_controller → [본 함수] → lib/nvme/probe →
 *     attach_controller_done 콜백 → JSON 응답 작성 후 클라이언트에 전송.
 */
int spdk_bdev_nvme_create(struct spdk_nvme_transport_id *trid,
			  const char *base_name,
			  const char **names,
			  uint32_t count,
			  spdk_bdev_nvme_create_cb cb_fn,
			  void *cb_ctx,
			  struct spdk_nvme_ctrlr_opts *drv_opts,
			  struct spdk_bdev_nvme_ctrlr_opts *bdev_opts);

/**
 * Delete the specified NVMe controller, or one of its paths.
 *
 * NOTE: When path_id is specified and it is the only path_id associated with NVMe controller
 * the path is removed and the NVMe controller gets deleted. (Optional) callback
 * function gets executed on delete complete in caller's thread. When the (optional)
 * callback is not provided, the control is returned back at the time delete is initiated,
 * not when it is completed. When NVMe controller deletion is already in progress state,
 * this function returns success.
 *
 * \param name NVMe controller name.
 * \param path_id The specified path to remove (optional).
 * \param delete_cb	Callback function on delete complete (optional).
 * \param cb_ctx Context passed to callback (optional).
 * \return zero on success,
 *		-EINVAL on wrong parameters or
 *		-ENODEV if controller is not found or
 *		-ENOMEM on no memory
 */
/*
 * [한국어]
 * spdk_bdev_nvme_delete - 컨트롤러(또는 그 한 path) 분리 + 관련 bdev 제거
 *
 * @name:      대상 컨트롤러의 base_name (create 시 지정한 이름과 동일).
 * @path_id:   특정 path 만 제거할 때 지정 (multipath 환경). NULL 이면 모든 path
 *             제거 → 컨트롤러 자체 분리.
 * @delete_cb: 분리 완료 시 호출되는 콜백. NULL 이면 fire-and-forget — 함수 반환
 *             시점에 분리는 시작만 되어 있고 완료는 비동기로 이어진다.
 * @cb_ctx:    delete_cb 의 첫 인자.
 * @return:    0 = 분리 시작 성공 (이미 진행 중이어도 0),
 *             -EINVAL = 잘못된 인자,
 *             -ENODEV = 해당 이름의 컨트롤러 없음,
 *             -ENOMEM = 내부 작업 컨텍스트 할당 실패.
 *
 * 동작:
 *   1) name 으로 nvme_ctrlr 조회 → path_id 매칭(있다면).
 *   2) 모든 in-flight I/O 가 완료될 때까지 대기 → bdev unregister →
 *      lib/nvme controller detach.
 *   3) 마지막 path 제거 시 컨트롤러 객체 free.
 *   4) delete_cb 가 있으면 caller 스레드에서 호출.
 *
 * 실행 컨텍스트: RPC dispatch thread (rpc_bdev_nvme_detach_controller). path
 * 추가/삭제 도중 I/O 와 race 가 없도록 모듈 내부 mutex/spinlock 으로 보호.
 *
 * 호출 체인:
 *   rpc_bdev_nvme_detach_controller → [본 함수] → lib/nvme detach →
 *     delete_cb → JSON 응답.
 */
int spdk_bdev_nvme_delete(const char *name, const struct spdk_nvme_path_id *path_id,
			  spdk_bdev_nvme_delete_cb delete_cb, void *cb_ctx);

/**
 * Set multipath policy of the NVMe bdev.
 *
 * \param name NVMe bdev name.
 * \param policy Multipath policy (active-passive or active-active).
 * \param selector Multipath selector (round_robin, queue_depth).
 * \param rr_min_io Number of IO to route to a path before switching to another for round-robin.
 * \param cb_fn Function to be called back after completion.
 * \param cb_arg Argument passed to the callback function.
 */
/*
 * [한국어]
 * spdk_bdev_nvme_set_multipath_policy - 특정 NVMe bdev 의 multipath 정책 변경
 *
 * @name:      대상 bdev 이름.
 * @policy:    ACTIVE_PASSIVE 또는 ACTIVE_ACTIVE.
 * @selector:  ACTIVE_ACTIVE 시 path 선택기 (ROUND_ROBIN / QUEUE_DEPTH).
 * @rr_min_io: ROUND_ROBIN 시 path 전환 전에 한 path 에 보낼 I/O 수.
 *             값이 클수록 한 path 에 더 오래 머무르며 cache locality 유리,
 *             작을수록 더 균등 분산.
 * @cb_fn:     완료 콜백.
 * @cb_arg:    콜백 컨텍스트.
 *
 * 정책 변경은 컨트롤러 그룹의 모든 IO 채널을 spdk_for_each_channel 로 순회하며
 * 각 reactor 에서 채널 상태를 바꾼 뒤 마지막에 cb_fn 이 호출된다 — 즉 cross-thread
 * 비동기 작업이며, 본 함수는 그 시작점만 트리거하고 즉시 반환.
 *
 * 호출 체인:
 *   rpc_bdev_nvme_set_multipath_policy → [본 함수] → for_each_channel →
 *     각 reactor 의 ctrlr_channel 갱신 → cb_fn.
 */
void spdk_bdev_nvme_set_multipath_policy(const char *name,
		enum spdk_bdev_nvme_multipath_policy policy,
		enum spdk_bdev_nvme_multipath_selector selector,
		uint32_t rr_min_io,
		spdk_bdev_nvme_set_multipath_policy_cb cb_fn,
		void *cb_arg);

/* Get default values for controller opts.
 *
 * \param opts Ctrlr opts object to be loaded with default values.
 */
/*
 * [한국어]
 * spdk_bdev_nvme_get_default_ctrlr_opts - 컨트롤러 옵션 구조체에 디폴트값을 채움
 *
 * @opts: 호출자가 할당한 struct spdk_bdev_nvme_ctrlr_opts. 모듈이 디폴트로
 *        채우고 싶은 필드들을 모두 채워 돌려준다.
 *
 * 사용 패턴: RPC 핸들러가 JSON 입력에서 일부 필드만 받았을 때, 본 함수로
 * 디폴트를 깔고 그 위에 사용자 지정 값들을 덮어쓴 뒤 spdk_bdev_nvme_create()
 * 에 넘긴다. 이렇게 해야 미래에 새 필드가 추가되어도 RPC 핸들러는 수정 없이
 * 안전한 기본값을 갖게 된다.
 *
 * 호출 체인:
 *   rpc_bdev_nvme_attach_controller (전처리 단계) → [본 함수].
 */
void spdk_bdev_nvme_get_default_ctrlr_opts(struct spdk_bdev_nvme_ctrlr_opts *opts);

/**
 * Get the default value for bdev nvme options.
 *
 * \param[out] opts Bdev nvme options object to be filled with default values.
 * \param opts_size Must be set to sizeof(struct spdk_bdev_nvme_opts).
 */
/*
 * [한국어]
 * spdk_bdev_nvme_get_opts - 모듈 글로벌 옵션의 현재 값을 옵트 아웃 인자에 복사
 *
 * @opts:      복사 대상. 호출자가 할당.
 * @opts_size: 호출자가 알고 있는 sizeof(struct spdk_bdev_nvme_opts). 모듈은
 *             min(opts_size, sizeof(현재 모듈 버전 구조체)) 바이트만 복사하여
 *             ABI 호환을 유지. 새로 추가된 필드는 호출자가 모르므로 안전하게
 *             생략된다.
 *
 * RPC `bdev_nvme_get_options` 의 백엔드. 일반적인 호출 패턴은:
 *   spdk_bdev_nvme_opts opts;
 *   spdk_bdev_nvme_get_opts(&opts, sizeof(opts));
 *   // opts.timeout_us = X; … 일부 필드 수정
 *   spdk_bdev_nvme_set_opts(&opts);
 *
 * 호출 체인:
 *   rpc_bdev_nvme_get_options → [본 함수] → JSON 응답으로 직렬화.
 */
void spdk_bdev_nvme_get_opts(struct spdk_bdev_nvme_opts *opts, size_t opts_size);

/**
 * Set the bdev nvme options.
 *
 * \param opts New value of bdev nvme options to be set.
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_bdev_nvme_set_opts - 모듈 글로벌 옵션 갱신
 *
 * @opts:   적용할 옵션 묶음. opts->opts_size 가 호출자가 알고 있는 구조체 크기
 *          를 정확히 담아야 한다.
 * @return: 0 = 성공, 음수 errno = 실패 (예: 옵션 값이 유효 범위를 벗어남,
 *          이미 컨트롤러가 어태치되어 변경 불가한 옵션을 바꾸려 함 등).
 *
 * 일부 옵션은 모듈 초기화 직후, 어떤 컨트롤러도 어태치되기 전에만 변경 가능
 * 하다. RPC 자동화 스크립트에서는 보통 spdk_tgt 시작 직후, attach 호출 전에
 * 한 번 호출하는 패턴.
 *
 * 호출 체인:
 *   rpc_bdev_nvme_set_options → [본 함수] → 모듈 내부 g_opts 갱신 →
 *     이후 attach 들에 즉시 반영.
 */
int spdk_bdev_nvme_set_opts(const struct spdk_bdev_nvme_opts *opts);

#ifdef __cplusplus
}
/* [한국어] extern "C" 종료. */
#endif

#endif /* SPDK_MODULE_BDEV_NVME_H_ */
/* [한국어] 헤더 가드 종료. */
