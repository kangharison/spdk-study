/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2022 Dell Inc, or its subsidiaries. All rights reserved.
 */

/*
 * [한국어 설명] bdev_nvme 모듈의 내부 헤더 (bdev_nvme.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK의 bdev_nvme 모듈(NVMe 컨트롤러를 SPDK bdev로 노출하는 모듈)이
 * 모듈 내부에서 공유하는 핵심 자료구조와 API를 정의한다. 외부 공개 헤더
 * spdk/module/bdev/nvme.h가 RPC 등을 위한 사용자 향 API를 두는 반면, 본 파일은
 * bdev_nvme.c, bdev_nvme_rpc.c, nvme_rpc.c, vbdev_opal.c 등 모듈 구현 파일들이
 * 공유하는 컨트롤러/네임스페이스/I/O 경로/멀티패스/디스커버리 자료구조를 한 곳에
 * 모아둔다. struct nvme_ctrlr, nvme_bdev_ctrlr, nvme_bdev, nvme_ns, nvme_io_path,
 * nvme_qpair, nvme_poll_group 같은 구조체가 핵심이며, 이들은 SPDK NVMe 드라이버
 * (lib/nvme)와 SPDK bdev 코어(lib/bdev)를 잇는 글루(glue) 객체이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   사용자 RPC (bdev_nvme_attach_controller, bdev_get_bdevs 등)
 *     → bdev_nvme_rpc.c → bdev_nvme.c의 내부 함수 (이 헤더 선언)
 *     → lib/nvme의 spdk_nvme_*() API 호출 (probe/connect/admin cmd/io cmd)
 *     → 컨트롤러 attach 완료 후 nvme_bdev 생성 → spdk_bdev_register
 *     → 사용자 I/O는 spdk_bdev_io → bdev_nvme_submit_request → spdk_nvme_ns_cmd_*
 *     → NVMe SQ doorbell write → 디바이스 → CQ → poller가 callback 실행 → bdev complete
 * 멀티패스 모델:
 *   - nvme_bdev 1개당 같은 NQN을 공유하는 여러 nvme_ctrlr (=경로) 가질 수 있음.
 *   - 각 채널(스레드 단위)에 nvme_bdev_channel이 있고, 그 안에 nvme_io_path 리스트.
 *   - I/O 발행 시 mp_policy/mp_selector(active-passive, round-robin, queue-depth 등)에
 *     따라 io_path 하나를 골라 그 path의 qpair로 보낸다. ANA(Asymmetric Namespace
 *     Access) 상태도 path 선택에 영향.
 * 실행 컨텍스트: 주요 객체 트리(ctrlr/ns 리스트)는 app 스레드에서만 변경, 채널 단위
 * 객체(io_path/qpair)는 그 채널이 속한 SPDK 스레드에서만 접근.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/nvme.h (NVMe 드라이버 공개 API), spdk/bdev_module.h (bdev 등록),
 *         spdk/module/bdev/nvme.h (외부 공개 API),
 *         spdk/jsonrpc.h (RPC 응답 빌드), spdk/queue.h (TAILQ/RB 매크로).
 * - 의존받음: bdev_nvme.c (구현 본체), bdev_nvme_rpc.c (사용자 RPC),
 *             nvme_rpc.c (admin/io passthrough RPC), bdev_nvme_cuse_rpc.c,
 *             bdev_mdns_client.c (mDNS NVMe-oF 디스커버리 결과를 bdev_nvme_start_discovery로 연결),
 *             vbdev_opal.c (nvme_ctrlr->opal_dev 활용).
 * - 데이터 흐름: 사용자 read/write → bdev_io → 채널의 io_path 선택 → qpair → SQ →
 *               디바이스 → CQ poll → completion callback → bdev_io_complete.
 * - 공유 전역: g_nvme_bdev_ctrlrs (모든 NVMe bdev 컨트롤러 그룹의 헤드), 모든 lookup의 시작점.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct nvme_ctrlr: lib/nvme의 spdk_nvme_ctrlr 포인터를 감싼 bdev_nvme측 컨트롤러 객체.
 *   reset/failover/ANA/keep-alive 상태와 RB-tree로 관리되는 namespace 컬렉션을 보유.
 * - struct nvme_bdev_ctrlr: 같은 NQN/이름을 공유하는 다중 nvme_ctrlr를 묶는 멀티패스 그룹.
 * - struct nvme_bdev: SPDK bdev 1개 = NVMe namespace 1개에 대응 (멀티패스에서는
 *   여러 ns 인스턴스를 가질 수 있음). spdk_bdev disk를 첫 필드로 임베드.
 * - struct nvme_ns: 단일 컨트롤러 내 namespace 표현. ANA 상태 포함.
 * - struct nvme_io_path / nvme_bdev_channel: 채널 단위 I/O 경로 캐시.
 * - struct nvme_qpair / nvme_poll_group: I/O qpair와 그 그룹(폴링 단위).
 * - nvme_ctrlr_get_by_name, nvme_bdev_ctrlr_get_by_name, nvme_ctrlr_get_ns 등 lookup API.
 * - nvme_ctrlr_for_each_channel, nvme_bdev_for_each_channel: 모든 채널 순회 (cross-thread).
 * - bdev_nvme_start_discovery, bdev_nvme_start_mdns_discovery: NVMe-oF 디스커버리.
 * - bdev_nvme_set_keys: DH-CHAP/PSK 키 설정.
 * - nvme_ctrlr_op_rpc / nvme_bdev_ctrlr_op_rpc: reset/enable/disable 트리거.
 */

#ifndef SPDK_BDEV_NVME_H
#define SPDK_BDEV_NVME_H

#include "spdk/stdinc.h"               /* [한국어] 표준 C 헤더 묶음 (uint*_t, bool 등). */

#include "spdk/queue.h"                /* [한국어] TAILQ_/RB_ 매크로 (intrusive list/tree). */
#include "spdk/nvme.h"                 /* [한국어] lib/nvme 공개 API: spdk_nvme_ctrlr/qpair/ns 등. */
#include "spdk/bdev_module.h"          /* [한국어] bdev 모듈 등록과 spdk_bdev 베이스 구조체. */
#include "spdk/module/bdev/nvme.h"     /* [한국어] bdev_nvme의 외부 공개 옵션/콜백 타입 (spdk_bdev_nvme_*). */
#include "spdk/jsonrpc.h"              /* [한국어] RPC 응답 빌드용 (디스커버리 정보 출력). */

/* [한국어] 모든 nvme_bdev_ctrlr를 연결하는 전역 TAILQ 헤드 타입.
 * struct nvme_bdev_ctrlr가 아래에서 정의되기 전에 헤드 타입을 forward 선언. */
TAILQ_HEAD(nvme_bdev_ctrlrs, nvme_bdev_ctrlr);
/* [한국어] 전역 컨트롤러 리스트 자체. bdev_nvme.c에 정의됨.
 * 설정자: bdev_nvme_register_ctrlr/_unregister_ctrlr (app 스레드).
 * 읽는 자: 모든 lookup(nvme_ctrlr_get_by_name 등). app 스레드 외 접근 시 주의. */
extern struct nvme_bdev_ctrlrs g_nvme_bdev_ctrlrs;

/* [한국어] bdev_nvme 모듈이 동시에 관리할 수 있는 NVMe 컨트롤러 최대 수.
 * RPC bdev_nvme_attach_controller에서 names 배열 등 정적 상한으로 사용. */
#define NVME_MAX_CONTROLLERS 1024

/*
 * [한국어] 디스커버리 시작 완료 콜백 타입.
 * @ctx: 호출 시 전달한 사용자 컨텍스트.
 * @status: 0이면 성공, 음수면 errno.
 * 호출 시점: bdev_nvme_start_discovery가 첫 attach까지 완료한 후 한 번 호출.
 */
typedef void (*spdk_bdev_nvme_start_discovery_fn)(void *ctx, int status);
/*
 * [한국어] 디스커버리 중지 완료 콜백 타입.
 * @ctx: 사용자 컨텍스트.
 * 호출 시점: bdev_nvme_stop_discovery가 polling을 중단하고 리소스를 해제한 후.
 */
typedef void (*spdk_bdev_nvme_stop_discovery_fn)(void *ctx);

/*
 * [한국어]
 * struct nvme_async_probe_ctx - 비동기 NVMe attach(probe→connect→namespace 채우기) 컨텍스트.
 *
 * bdev_nvme_attach_controller RPC와 디스커버리 경로 모두에서, attach가 콜백 기반의
 * 다단계 비동기 흐름이라 진행 중 상태를 유지할 별도 객체가 필요하다. probe poller가
 * 주기적으로 lib/nvme의 spdk_nvme_probe_poll_async를 호출하면서 이 컨텍스트를
 * 따라다니다가, namespace populate가 모두 끝나면 cb_fn으로 사용자에게 알린다.
 */
struct nvme_async_probe_ctx {
	struct spdk_nvme_probe_ctx *probe_ctx;
	/* [한국어] lib/nvme가 반환한 비동기 probe 컨텍스트 핸들.
	 * 설정자: spdk_nvme_connect_async() / spdk_nvme_probe_async()의 반환값.
	 * 읽는 자: probe poller가 spdk_nvme_probe_poll_async()의 인자로 사용.
	 * 값 범위: NULL이면 probe가 아직 시작 안 됐거나 종료됨.
	 * 동기화: app 스레드에서만 다룸. */

	char *base_name;
	/* [한국어] 사용자가 RPC에서 지정한 컨트롤러 이름 베이스 (예: "Nvme0").
	 * 결과 namespace bdev 이름은 base_name + "n" + nsid 형식.
	 * 설정자: bdev_nvme_create()에서 strdup. 읽는 자: namespace populate 콜백.
	 * 값 범위: 비어있지 않은 ASCII 문자열. */

	const char **names;
	/* [한국어] populate된 namespace bdev들의 이름을 사용자에게 돌려주기 위한 배열 포인터.
	 * 설정자: 호출자가 RPC에서 빈 배열을 미리 할당해 전달.
	 * 읽는 자: namespace populate 완료 콜백이 names[i]에 strdup으로 채움. */

	uint32_t max_bdevs;
	/* [한국어] names 배열의 용량. 이를 초과하면 reported_bdevs 누적이 멈추고 에러 처리. */

	uint32_t reported_bdevs;
	/* [한국어] 지금까지 names에 기록한 namespace 수. populate 콜백이 ++. */

	struct spdk_poller *poller;
	/* [한국어] spdk_nvme_probe_poll_async를 주기적으로 호출하는 poller (등록/해제는 app 스레드).
	 * 값 범위: NULL이면 아직 시작 안 됨. probe_done 후 해제됨. */

	struct spdk_nvme_transport_id trid;
	/* [한국어] 대상 컨트롤러의 트랜스포트 ID (PCIe BDF 또는 NVMe-oF traddr/trsvcid/subnqn).
	 * 설정자: bdev_nvme_create()가 사용자 입력으로부터 채움. */

	struct spdk_bdev_nvme_ctrlr_opts bdev_opts;
	/* [한국어] bdev_nvme 측 컨트롤러 옵션 (multipath, ANA timeout, fast_io_fail 등).
	 * spdk_bdev_nvme_ctrlr_opts는 외부 공개 헤더 정의. */

	struct spdk_nvme_ctrlr_opts drv_opts;
	/* [한국어] lib/nvme 드라이버 측 옵션 (queue_size, num_io_queues, hostnqn 등).
	 * probe_async에 전달되어 connect 시 적용됨. */

	spdk_bdev_nvme_create_cb cb_fn;
	/* [한국어] attach 전체가 끝났을 때 호출할 사용자 콜백.
	 * 호출 시점: namespaces_populated == true 이후. */

	void *cb_ctx;
	/* [한국어] cb_fn에 그대로 전달될 컨텍스트 포인터. 호출 후에는 사용자가 free 책임. */

	uint32_t populates_in_progress;
	/* [한국어] 현재 populate가 진행 중인 namespace 개수 (각 ns당 비동기로 진행).
	 * 0으로 떨어지면 namespaces_populated=true, cb_fn 호출 가능. */

	bool ctrlr_attached;
	/* [한국어] 컨트롤러 attach(connect)가 완료되었는가? probe 콜백 단계. */

	bool probe_done;
	/* [한국어] probe poller가 더 이상 polling할 필요가 없는 상태인가? */

	bool namespaces_populated;
	/* [한국어] 모든 active namespace의 bdev 등록이 끝났는가? cb_fn 호출 조건. */
};

/*
 * [한국어]
 * struct nvme_ns - 단일 NVMe 컨트롤러에 속한 하나의 namespace 표현.
 *
 * lib/nvme의 spdk_nvme_ns 포인터를 감싸고, bdev_nvme 측의 추가 메타(ANA 상태,
 * 등록된 bdev 포인터 등)를 보관한다. 멀티패스 모드에서는 같은 NQN의 여러 컨트롤러
 * 각각이 자신의 nvme_ns 인스턴스를 가지며, 그 인스턴스들이 하나의 nvme_bdev에
 * nvme_ns_list로 묶인다.
 */
struct nvme_ns {
	uint32_t			id;
	/* [한국어] NVMe namespace ID (1부터 시작, 0은 invalid).
	 * 설정자: nvme_ns 할당 시 spdk_nvme_ns_get_id() 결과로 채움.
	 * 읽는 자: lookup, RB 트리 비교 함수, bdev 이름 생성 등. */

	struct spdk_nvme_ns		*ns;
	/* [한국어] lib/nvme가 관리하는 namespace 객체 포인터.
	 * 설정자: nvme_ns_alloc() 시 spdk_nvme_ctrlr_get_ns(). 읽는 자: 모든 I/O 발행 함수.
	 * 값 범위: NULL이면 namespace inactive/제거됨. */

	struct nvme_ctrlr		*ctrlr;
	/* [한국어] 이 namespace가 소속된 nvme_ctrlr 역참조.
	 * I/O 경로 선택, ANA 상태 갱신 시 컨트롤러 컨텍스트 추적용. */

	struct nvme_bdev		*bdev;
	/* [한국어] 이 namespace가 결합된 nvme_bdev (멀티패스 그룹).
	 * NULL이면 아직 bdev 등록 전이거나 detach 진행 중. */

	uint32_t			ana_group_id;
	/* [한국어] ANA(Asymmetric Namespace Access) Group ID.
	 * 같은 그룹의 ns들은 ANA 상태가 함께 변경됨. */

	enum spdk_nvme_ana_state	ana_state;
	/* [한국어] 현재 ANA 상태 (Optimized, Non-Optimized, Inaccessible, Persistent Loss, Change).
	 * I/O 경로 선택 시 active-passive/round-robin 정책이 이 값을 참조. */

	bool				ana_state_updating;
	/* [한국어] ANA 상태 변경(AEN 수신 후 ANA log 재읽기) 진행 중 플래그.
	 * 동시에 여러 갱신이 겹치는 것을 막기 위함. */

	bool				ana_transition_timedout;
	/* [한국어] ANA Transition 시간이 너무 오래 걸려 timeout이 발생했는가?
	 * true면 해당 path를 inaccessible로 처리. */

	bool				depopulating;
	/* [한국어] namespace 제거 중 (delete 진행) 플래그.
	 * I/O 경로 선택에서 이 ns를 건너뛰도록 함. */

	struct spdk_poller		*anatt_timer;
	/* [한국어] ANA Transition Timeout (anatt) 경과 시 호출될 timer poller.
	 * 등록/해제는 app 스레드. */

	struct nvme_async_probe_ctx	*probe_ctx;
	/* [한국어] 이 namespace가 attach 흐름의 일부로 populate 중일 때 참조하는 probe 컨텍스트.
	 * populate 완료 시 reported_bdevs/populates_in_progress 갱신에 사용. */

	TAILQ_ENTRY(nvme_ns)		tailq;
	/* [한국어] nvme_bdev::nvme_ns_list (멀티패스의 동일 namespace를 묶는 리스트) 연결 노드. */

	RB_ENTRY(nvme_ns)		node;
	/* [한국어] nvme_ctrlr::namespaces (RB-tree, key=id)에 삽입되는 노드.
	 * lookup은 O(log n). app 스레드에서만 변경. */
};

/* [한국어] 아래 구조체들은 nvme_ctrlr 정의 안에서 포인터로 참조되므로 forward 선언. */
struct nvme_bdev_io;            /* [한국어] bdev_nvme가 spdk_bdev_io를 감싸는 내부 컨텍스트. */
struct nvme_bdev_ctrlr;         /* [한국어] 멀티패스 그룹 (이 헤더 아래에서 정의). */
struct nvme_bdev;               /* [한국어] SPDK bdev 단위 객체. */
struct nvme_io_path;            /* [한국어] 채널 단위 I/O 경로 (ns × qpair). */
struct nvme_ctrlr_channel_iter; /* [한국어] for_each_channel용 iterator (구현은 .c). */
struct nvme_bdev_channel_iter;  /* [한국어] for_each_channel용 iterator (bdev 버전). */

/*
 * [한국어]
 * struct spdk_nvme_path_id - 한 컨트롤러에 attach 가능한 트랜스포트 경로 표현.
 *
 * NVMe-oF 멀티패스/페일오버를 위해 한 nvme_ctrlr가 여러 trid(transport id)를
 * 가질 수 있다. 활성 경로는 nvme_ctrlr::active_path_id가 가리키고, 나머지는
 * trids TAILQ에 보관. 페일오버 시 다음 후보로 active_path_id를 옮긴다.
 */
struct spdk_nvme_path_id {
	struct spdk_nvme_transport_id		trid;
	/* [한국어] 트랜스포트 종류와 주소(PCIe BDF/IP+포트+SUBNQN 등). */

	struct spdk_nvme_host_id		hostid;
	/* [한국어] 호스트 측 식별자 (소스 IP/포트, RDMA NQN 등). 일반적으로 zero. */

	TAILQ_ENTRY(spdk_nvme_path_id)		link;
	/* [한국어] nvme_ctrlr::trids 리스트의 노드. */

	uint64_t				last_failed_tsc;
	/* [한국어] 이 경로가 마지막으로 실패한 시각 (TSC 카운터).
	 * 페일오버 정책이 최근 실패 경로를 일정 시간 회피하는 데 사용. */
};

/*
 * [한국어] 컨트롤러 op(reset/enable/disable) 완료 콜백 타입.
 * @cb_arg: 사용자 컨텍스트.
 * @rc: 0 성공, 음수 errno.
 * 실행 컨텍스트: 해당 컨트롤러의 admin 폴링 스레드 또는 app 스레드.
 */
typedef void (*bdev_nvme_ctrlr_op_cb)(void *cb_arg, int rc);
/*
 * [한국어] 컨트롤러 disconnect 완료 알림 콜백 타입.
 * @nvme_ctrlr: disconnect된 컨트롤러.
 * 호출 시점: reset 도중 disconnect 단계가 끝난 직후.
 */
typedef void (*nvme_ctrlr_disconnected_cb)(struct nvme_ctrlr *nvme_ctrlr);

/*
 * [한국어]
 * struct nvme_ctrlr - bdev_nvme 측의 NVMe 컨트롤러 객체.
 *
 * lib/nvme의 spdk_nvme_ctrlr 포인터를 감싸고 reset/failover/ANA/keepalive/I/O qpair
 * 관리에 필요한 상태를 보관한다. 같은 nvme_bdev_ctrlr(=같은 NQN/이름)에 속한 여러
 * nvme_ctrlr가 멀티패스 경로 후보가 된다.
 */
struct nvme_ctrlr {
	/**
	 * points to pinned, physically contiguous memory region;
	 * contains 4KB IDENTIFY structure for controller which is
	 *  target for CONTROLLER IDENTIFY command during initialization
	 */
	struct spdk_nvme_ctrlr			*ctrlr;
	/* [한국어] lib/nvme가 관리하는 컨트롤러 핸들. 이 모듈의 모든 NVMe API 호출의 첫 인자.
	 * 설정자: bdev_nvme_create_ctrlr()이 spdk_nvme_connect_async 결과로 채움.
	 * 읽는 자: 모든 admin/IO 명령 발행 함수, qpair allocate 등.
	 * 값 범위: NULL이면 detach 진행 중. 동기화: 라이프사이클 변경은 app 스레드. */

	struct spdk_nvme_path_id		*active_path_id;
	/* [한국어] 현재 사용 중인 트랜스포트 경로 (trids 리스트의 한 원소를 가리킴).
	 * 페일오버 시 다른 path_id로 swap. */

	int					ref;
	/* [한국어] 참조 카운트. 외부에서 사용 시작 시 ++, 끝나면 --.
	 * 0이 되면 destruct 플래그 설정 후 정리. */

	uint32_t				resetting : 1;
	/* [한국어] 컨트롤러 reset 진행 중 플래그. 새 reset 요청은 큐잉 또는 거부됨. */
	uint32_t				reconnect_is_delayed : 1;
	/* [한국어] reconnect_delay_sec 옵션에 따라 재연결을 일정 시간 지연 중. */
	uint32_t				in_failover : 1;
	/* [한국어] 페일오버 진행 중 (다른 path_id로 전환). */
	uint32_t				pending_failover : 1;
	/* [한국어] 페일오버 요청이 큐에 있지만 다른 op 끝나기를 기다리는 중. */
	uint32_t				fast_io_fail_timedout : 1;
	/* [한국어] fast_io_fail_timeout_sec 경과로 진행 중 I/O를 즉시 실패시키는 모드. */
	uint32_t				destruct : 1;
	/* [한국어] 컨트롤러 제거 진행 중 (ref==0 후 설정). 새 작업 차단. */
	uint32_t				ana_log_page_updating : 1;
	/* [한국어] ANA log page를 fetch 중 (AEN 후 또는 주기적). */
	uint32_t				io_path_cache_clearing : 1;
	/* [한국어] 모든 채널의 io_path 캐시를 무효화 중 (path 추가/제거 후). */
	uint32_t				dont_retry : 1;
	/* [한국어] 일시적으로 I/O 재시도 비활성화 (reset 직후 등). */
	uint32_t				disabled : 1;
	/* [한국어] 사용자 RPC로 컨트롤러를 disable한 상태. I/O 발행 차단. */

	struct spdk_bdev_nvme_ctrlr_opts	opts;
	/* [한국어] bdev_nvme 측 옵션 (ctrlr_loss/reset 정책, ANA timeout 등) 사본. */

	/* This list can only be accessed from the app thread. */
	RB_HEAD(nvme_ns_tree, nvme_ns)		namespaces;
	/* [한국어] 이 컨트롤러의 namespace 트리 (key=nsid). RB-tree로 O(log n) lookup.
	 * 동기화: 변경/조회 모두 반드시 app 스레드에서만. */

	struct spdk_opal_dev			*opal_dev;
	/* [한국어] TCG Opal SED 지원 컨트롤러일 때 lib/nvme/opal이 만든 Opal 드라이버 객체.
	 * vbdev_opal이 이 포인터로 spdk_opal_cmd_*를 호출. NULL이면 Opal 미지원. */

	struct spdk_poller			*adminq_timer_poller;
	/* [한국어] admin queue 처리 poller. 주기적으로 spdk_nvme_ctrlr_process_admin_completions 호출. */
	struct spdk_interrupt			*intr;
	/* [한국어] 인터럽트 모드일 때 사용되는 admin 인터럽트 핸들 (poll 모드면 NULL). */

	bdev_nvme_ctrlr_op_cb			ctrlr_op_cb_fn;
	/* [한국어] 진행 중인 op(reset/enable/disable) 완료 시 호출할 콜백.
	 * 동시에 한 op만 가능하므로 단일 슬롯. */
	void					*ctrlr_op_cb_arg;
	/* [한국어] ctrlr_op_cb_fn에 전달될 컨텍스트. */

	/* Poller used to check for reset/detach completion */
	struct spdk_poller			*reset_detach_poller;
	/* [한국어] reset/detach가 비동기로 완료되는 것을 폴링하는 poller. */
	struct spdk_nvme_detach_ctx		*detach_ctx;
	/* [한국어] spdk_nvme_detach_async가 반환한 비동기 detach 컨텍스트. */

	uint64_t				reset_start_tsc;
	/* [한국어] reset이 시작된 시각 (TSC). reset timeout 비교에 사용. */
	struct spdk_poller			*reconnect_delay_timer;
	/* [한국어] reconnect_delay_sec 동안 대기 후 재연결을 시도하는 timer poller. */

	nvme_ctrlr_disconnected_cb		disconnected_cb;
	/* [한국어] reset 중 disconnect 단계 완료 알림 콜백. */

	TAILQ_HEAD(, nvme_bdev_io)		pending_resets;
	/* [한국어] reset 중에 도착한 I/O들을 대기시키는 큐. reset 끝난 후 재시도. */

	/** linked list pointer for device list */
	TAILQ_ENTRY(nvme_ctrlr)			tailq;
	/* [한국어] nvme_bdev_ctrlr::ctrlrs 리스트의 노드 (멀티패스 그룹 내 형제 컨트롤러들). */
	struct nvme_bdev_ctrlr			*nbdev_ctrlr;
	/* [한국어] 소속된 멀티패스 그룹 역참조. */

	TAILQ_HEAD(nvme_paths, spdk_nvme_path_id)	trids;
	/* [한국어] 이 컨트롤러에 attach된(또는 attach 가능한) 모든 transport id 리스트.
	 * active_path_id는 이 리스트의 한 원소. */

	uint32_t				max_ana_log_page_size;
	/* [한국어] ANA log page의 최대 크기 (Identify로부터 얻음). 버퍼 할당 크기 결정. */
	struct spdk_nvme_ana_page		*ana_log_page;
	/* [한국어] ANA log page의 호스트 측 캐시 버퍼. */
	struct spdk_nvme_ana_group_descriptor	*copied_ana_desc;
	/* [한국어] ANA descriptor 순회 시 복사용 작업 버퍼. */

	struct nvme_async_probe_ctx		*probe_ctx;
	/* [한국어] 이 컨트롤러를 attach 중인 진행 컨텍스트. attach 완료 후 NULL. */
	struct spdk_key				*psk;
	/* [한국어] TLS PSK 키 (NVMe-oF over TCP+TLS).
	 * 설정: bdev_nvme_attach 시 사용자 키 매니저에서 lookup. */
	struct spdk_key				*dhchap_key;
	/* [한국어] DH-CHAP 호스트 측 키 (인증용). */
	struct spdk_key				*dhchap_ctrlr_key;
	/* [한국어] DH-CHAP 컨트롤러 측 키 (양방향 인증용). */

	pthread_mutex_t				mutex;
	/* [한국어] 일부 필드(예: pending_resets/플래그) 갱신을 보호하는 뮤텍스.
	 * cross-thread (poll group의 IO 스레드 → app 스레드) 통신 시 사용. */
};

/*
 * [한국어]
 * struct nvme_bdev_ctrlr - 같은 NQN/이름을 공유하는 멀티패스 컨트롤러 그룹.
 *
 * 사용자가 동일 base_name으로 여러 trid를 attach하면 모두 한 nvme_bdev_ctrlr에
 * 묶인다. 각 멀티패스 namespace는 그룹 단위로 하나의 nvme_bdev로 노출되며,
 * I/O는 그룹 내 컨트롤러들의 qpair 중 정책에 따라 선택된다.
 */
struct nvme_bdev_ctrlr {
	char				*name;
	/* [한국어] 그룹 이름 (base_name). 사용자/RPC가 보는 식별자. */

	TAILQ_HEAD(, nvme_ctrlr)	ctrlrs;
	/* [한국어] 그룹에 속한 nvme_ctrlr 리스트 (멀티패스 경로 후보들). */

	TAILQ_HEAD(, nvme_bdev)		bdevs;
	/* [한국어] 이 그룹이 노출하는 nvme_bdev들 (namespace 별로 1개). */

	TAILQ_ENTRY(nvme_bdev_ctrlr)	tailq;
	/* [한국어] g_nvme_bdev_ctrlrs (전역 그룹 리스트)의 노드. */
};

/*
 * [한국어]
 * struct nvme_error_stat - NVMe completion 상태 코드 통계.
 *
 * Identify 같은 admin 명령부터 일반 IO 명령까지의 응답 status code를 type/code별로
 * 카운트해 운영 가시성을 제공.
 */
struct nvme_error_stat {
	uint32_t status_type[8];
	/* [한국어] Status Code Type별 카운터 (Generic/Command Specific/Media 등 8가지).
	 * NVMe spec의 Status Code Type 필드 (3비트, 0~7). */

	uint32_t status[4][256];
	/* [한국어] [SCT][SC] 매트릭스. SCT 4종 × SC 256개 카운터.
	 * 자세히 어떤 에러가 얼마나 발생했는지 추적. */
};

/*
 * [한국어]
 * struct nvme_bdev - NVMe namespace를 SPDK bdev로 노출하는 객체.
 *
 * 첫 필드 disk가 spdk_bdev이므로 spdk_bdev * → struct nvme_bdev *로 캐스팅 가능.
 * 멀티패스 모드에서는 nvme_ns_list에 같은 namespace의 여러 인스턴스(각 컨트롤러
 * 경로별)가 들어있고, 정책에 따라 한 ns(=한 path)를 선택해 I/O 발행한다.
 */
struct nvme_bdev {
	struct spdk_bdev			disk;
	/* [한국어] SPDK bdev 베이스. 반드시 첫 필드여야 (캐스팅 트릭).
	 * 설정자: bdev_nvme_create_bdev에서 fn_table/ctxt 등 채움. */

	uint32_t				nsid;
	/* [한국어] 이 bdev가 대응하는 NVMe namespace ID. 멀티패스 시 모든 ns의 id가 동일. */

	struct nvme_bdev_ctrlr			*nbdev_ctrlr;
	/* [한국어] 소속 멀티패스 그룹. */

	pthread_mutex_t				mutex;
	/* [한국어] nvme_ns_list 변경 시 보호하는 뮤텍스 (cross-thread access). */

	int					ref;
	/* [한국어] 참조 카운트 (open descriptor 수 등). */

	enum spdk_bdev_nvme_multipath_policy	mp_policy;
	/* [한국어] 멀티패스 정책 (active-passive, active-active). */
	enum spdk_bdev_nvme_multipath_selector	mp_selector;
	/* [한국어] active-active일 때 path 선택 알고리즘 (round-robin, queue-depth). */
	uint32_t				rr_min_io;
	/* [한국어] round-robin 시 한 path에 연속으로 보낼 최소 I/O 수.
	 * rr_counter가 이 값에 도달하면 다음 path로 전환. */

	/* This list is modified on the app thread only but can be accessed on other threads:
	 * - Modifications must use the mutex.
	 * - Access on the app thread does not require locking.
	 * - Access on other threads must use the mutex.
	 */
	TAILQ_HEAD(, nvme_ns)			nvme_ns_list;
	/* [한국어] 같은 namespace ID를 갖는 모든 ns 인스턴스 (멀티패스 경로별).
	 * 동기화 규칙: 변경은 app 스레드+mutex, 다른 스레드에서 읽을 때도 mutex 필요.
	 * app 스레드 자체에서 읽을 때만 lock-free 허용. */

	bool					opal;
	/* [한국어] 이 bdev가 vbdev_opal에 의해 보호되고 있는가? true면 base bdev로
	 * 사용 중이며 직접 destruct하지 못함. */

	TAILQ_ENTRY(nvme_bdev)			tailq;
	/* [한국어] nvme_bdev_ctrlr::bdevs 리스트의 노드. */

	struct nvme_error_stat			*err_stat;
	/* [한국어] 에러 통계 옵션 활성 시 할당 (NULL이면 통계 미수집). */
};

/*
 * [한국어]
 * struct nvme_qpair - bdev_nvme 측의 I/O qpair 표현 (한 SPDK 스레드 단위).
 *
 * 한 컨트롤러는 SPDK 스레드 수만큼의 IO qpair를 가질 수 있으며, 각 nvme_qpair는
 * 그 스레드의 ctrlr_ch에 묶이고 poll_group(폴링 단위)에 등록된다. NVMe SQ/CQ
 * 한 쌍은 lib/nvme의 spdk_nvme_qpair가 직접 보유하고, 본 객체는 그 핸들을 감싸
 * io_path 캐시 무효화용 역참조를 추가로 보관한다.
 */
struct nvme_qpair {
	struct nvme_ctrlr		*ctrlr;
	/* [한국어] 이 qpair를 소유한 nvme_ctrlr (한 컨트롤러에 여러 qpair 가능). */

	struct spdk_nvme_qpair		*qpair;
	/* [한국어] lib/nvme가 만든 실제 SQ+CQ pair 객체 (NVMe spec의 IO Queue Pair).
	 * 설정자: spdk_nvme_ctrlr_alloc_io_qpair. NULL이면 disconnected.
	 * 동기화: 이 qpair에 명령을 발행하는 것은 반드시 소속 SPDK 스레드에서만. */

	struct nvme_poll_group		*group;
	/* [한국어] 이 qpair가 속한 polling 그룹. group->poller가 모든 qpair completion 폴링. */

	struct nvme_ctrlr_channel	*ctrlr_ch;
	/* [한국어] 이 qpair가 결합된 채널 (nvme_ctrlr의 spdk_io_channel 컨텍스트). */

	/* The following is used to update io_path cache of nvme_bdev_channels. */
	TAILQ_HEAD(, nvme_io_path)	io_path_list;
	/* [한국어] 이 qpair를 사용하는 모든 nvme_io_path 리스트.
	 * qpair disconnect 시 모든 io_path를 invalidate해야 하므로 역참조 필요. */

	TAILQ_ENTRY(nvme_qpair)		tailq;
	/* [한국어] nvme_poll_group::qpair_list의 노드. */
};

/*
 * [한국어]
 * struct nvme_ctrlr_channel - SPDK 스레드 단위로 본 nvme_ctrlr의 채널 컨텍스트.
 *
 * spdk_io_channel은 IO device(여기선 nvme_ctrlr)와 SPDK 스레드 한 쌍에 1개 생성되며,
 * bdev_nvme는 그 ctx_buf로 본 구조체를 사용한다. 채널마다 자신의 IO qpair 1개.
 */
struct nvme_ctrlr_channel {
	struct nvme_qpair		*qpair;
	/* [한국어] 이 채널 전용 IO qpair. NULL이면 connect 진행 중이거나 disconnect됨. */

	struct nvme_ctrlr_channel_iter	*reset_iter;
	/* [한국어] reset 진행 중에 모든 채널을 순회하는 iterator 참조 (한 시점에 1개). */
	struct spdk_poller		*connect_poller;
	/* [한국어] qpair connect가 비동기일 때 polling poller. */
};

/*
 * [한국어]
 * struct nvme_io_path - 한 채널이 한 namespace에 접근하는 단일 경로.
 *
 * (nvme_ns × nvme_qpair) 페어를 캐시한다. 한 nvme_bdev_channel은 멀티패스 정책에
 * 따라 여러 io_path 후보 중 하나를 골라 I/O를 보낸다.
 */
struct nvme_io_path {
	struct nvme_ns			*nvme_ns;
	/* [한국어] 대상 namespace 객체. */
	struct nvme_qpair		*qpair;
	/* [한국어] 이 namespace의 컨트롤러에 속한 같은 채널의 IO qpair. */

	STAILQ_ENTRY(nvme_io_path)	stailq;
	/* [한국어] nvme_bdev_channel::io_path_list (단방향 큐) 노드. */

	/* The following are used to update io_path cache of the nvme_bdev_channel. */
	struct nvme_bdev_channel	*nbdev_ch;
	/* [한국어] 소속 bdev 채널 역참조 (path 변경 통보용). */
	TAILQ_ENTRY(nvme_io_path)	tailq;
	/* [한국어] nvme_qpair::io_path_list (qpair가 자기를 쓰는 io_path들 추적) 노드. */

	/* allocation of stat is decided by option io_path_stat of RPC bdev_nvme_set_options */
	struct spdk_bdev_io_stat	*stat;
	/* [한국어] path별 IO 통계 (옵션). NULL이면 미수집. */
};

/*
 * [한국어]
 * struct nvme_bdev_channel - SPDK 스레드 단위 nvme_bdev의 채널 컨텍스트.
 *
 * 사용자가 nvme_bdev에 대해 채널을 열면 이 구조체가 채널의 ctx_buf로 잡힌다.
 * 이 안에 멀티패스 io_path 리스트와 현재 활성 path 캐시(current_io_path)가 들어있다.
 * I/O 발행 시 가장 먼저 current_io_path의 유효성을 확인하고, 무효면 정책에 따라 재선택.
 */
struct nvme_bdev_channel {
	struct nvme_io_path			*current_io_path;
	/* [한국어] 활성 I/O 경로 캐시 (active-passive에서 우선, RR에서는 최근 선택).
	 * NULL이면 즉시 재선택. 무효 path가 캐시될 수 있어 발행 직전 검증 필수. */

	enum spdk_bdev_nvme_multipath_policy	mp_policy;
	/* [한국어] 채널 단위로 본 멀티패스 정책 (nvme_bdev::mp_policy의 사본). */
	enum spdk_bdev_nvme_multipath_selector	mp_selector;
	/* [한국어] 채널 단위 path 선택 알고리즘. */
	uint32_t				rr_min_io;
	/* [한국어] round-robin의 최소 stride. */
	uint32_t				rr_counter;
	/* [한국어] 현재 path에서 발행된 I/O 카운터 (rr_min_io 도달 시 다음 path로 전환). */

	STAILQ_HEAD(, nvme_io_path)		io_path_list;
	/* [한국어] 이 채널이 보유한 io_path 후보 리스트 (단방향). */
	TAILQ_HEAD(retry_io_head, nvme_bdev_io)	retry_io_list;
	/* [한국어] 일시적 실패로 재시도 대기 중인 IO들. retry_io_poller가 주기적으로 재발행 시도. */
	struct spdk_poller			*retry_io_poller;
	/* [한국어] retry_io_list 처리 poller. */
	bool					resetting;
	/* [한국어] 이 채널이 reset 처리 중인가? true면 새 IO를 pending_resets로 전환. */
};

/*
 * [한국어]
 * struct nvme_poll_group - SPDK 스레드별로 묶인 IO qpair 폴링 그룹.
 *
 * lib/nvme의 spdk_nvme_poll_group은 여러 qpair의 completion을 한 번의 polling 호출로
 * 처리할 수 있게 해준다. 본 구조체는 그 그룹과 함께 accel 가속/통계/인터럽트 모드 정보를
 * 묶어서 관리한다. 한 SPDK 스레드 = 한 nvme_poll_group.
 */
struct nvme_poll_group {
	struct spdk_nvme_poll_group		*group;
	/* [한국어] lib/nvme의 폴링 그룹 핸들. spdk_nvme_poll_group_process_completions로 폴링. */
	struct spdk_io_channel			*accel_channel;
	/* [한국어] crc/compare 등 accel framework 채널 (옵션). NULL이면 미사용. */
	struct spdk_poller			*poller;
	/* [한국어] 이 그룹의 completion 폴링 poller. 매 호출마다 process_completions 실행. */

	bool					collect_spin_stat;
	/* [한국어] spin 시간 통계 수집 활성 여부. */
	uint64_t				spin_ticks;
	/* [한국어] poller가 빈 폴링(스핀)에 소비한 누적 TSC 틱. */
	uint64_t				start_ticks;
	/* [한국어] 가장 최근 폴링 시작 TSC. */
	uint64_t				end_ticks;
	/* [한국어] 가장 최근 폴링 종료 TSC. */

	TAILQ_HEAD(, nvme_qpair)		qpair_list;
	/* [한국어] 이 그룹에 속한 모든 nvme_qpair. */

	struct spdk_interrupt			*intr;
	/* [한국어] 인터럽트 모드일 때의 인터럽트 핸들 (poll 모드면 NULL). */
};

/*
 * [한국어]
 * nvme_io_path_info_json - 한 io_path의 정보를 JSON writer로 직렬화한다.
 * @w: 출력 버퍼 (RPC 응답 등).
 * @io_path: 직렬화할 io_path. ns/qpair 정보 dump.
 * 호출 컨텍스트: app 스레드 (RPC 응답 빌드 시).
 */
void nvme_io_path_info_json(struct spdk_json_write_ctx *w, struct nvme_io_path *io_path);

/*
 * [한국어]
 * nvme_ctrlr_get_by_name - 이름으로 nvme_ctrlr를 lookup한다.
 * @name: 컨트롤러 이름 (멀티패스의 경우 그룹 이름인 base_name).
 * @return: nvme_ctrlr 포인터 (멀티패스면 그룹의 첫 컨트롤러), 없으면 NULL.
 * 컨텍스트: app 스레드.
 */
struct nvme_ctrlr *nvme_ctrlr_get_by_name(const char *name);

/* [한국어] for_each_channel 콜백: 각 채널마다 호출되는 메시지 핸들러. */
typedef void (*nvme_ctrlr_for_each_channel_msg)(struct nvme_ctrlr_channel_iter *iter,
		struct nvme_ctrlr *nvme_ctrlr,
		struct nvme_ctrlr_channel *ctrlr_ch,
		void *ctx);

/* [한국어] for_each_channel 콜백: 모든 채널 처리가 끝났을 때 한 번 호출되는 완료 핸들러. */
typedef void (*nvme_ctrlr_for_each_channel_done)(struct nvme_ctrlr *nvme_ctrlr,
		void *ctx, int status);

/*
 * [한국어]
 * nvme_ctrlr_for_each_channel - 모든 SPDK 스레드의 ctrlr 채널을 순회한다.
 *
 * @nvme_ctrlr: 대상 컨트롤러.
 * @fn: 각 채널에서 호출될 메시지 콜백 (해당 스레드 컨텍스트에서 실행).
 * @ctx: 콜백에 전달할 사용자 컨텍스트.
 * @cpl: 모든 채널 처리가 완료된 후 (마지막에) 호출될 완료 콜백.
 *
 * 내부적으로 spdk_for_each_channel을 사용해 각 채널 스레드로 메시지를 보낸다.
 * cross-thread 일괄 작업 (예: reset, qpair disconnect)에 사용. fn은 작업 후
 * nvme_ctrlr_for_each_channel_continue를 호출해 다음 채널로 진행.
 */
void nvme_ctrlr_for_each_channel(struct nvme_ctrlr *nvme_ctrlr,
				 nvme_ctrlr_for_each_channel_msg fn, void *ctx,
				 nvme_ctrlr_for_each_channel_done cpl);

/*
 * [한국어]
 * nvme_ctrlr_for_each_channel_continue - 현재 채널 처리 완료를 알리고 다음 채널로 진행.
 * @iter: msg 콜백에 전달된 iterator.
 * @status: 현재 채널 처리 결과 (음수면 에러, cpl에 전파됨).
 */
void nvme_ctrlr_for_each_channel_continue(struct nvme_ctrlr_channel_iter *iter,
		int status);


/* [한국어] bdev 채널 순회용 메시지 콜백. */
typedef void (*nvme_bdev_for_each_channel_msg)(struct nvme_bdev_channel_iter *iter,
		struct nvme_bdev *nbdev,
		struct nvme_bdev_channel *nbdev_ch,
		void *ctx);

/* [한국어] bdev 채널 순회 완료 콜백. */
typedef void (*nvme_bdev_for_each_channel_done)(struct nvme_bdev *nbdev,
		void *ctx, int status);

/*
 * [한국어]
 * nvme_bdev_for_each_channel - 모든 스레드의 nvme_bdev 채널을 순회.
 *
 * 사용 예: 멀티패스 정책 변경 후 모든 채널의 mp_policy/mp_selector 갱신,
 *          io_path 캐시 무효화 등.
 */
void nvme_bdev_for_each_channel(struct nvme_bdev *nbdev,
				nvme_bdev_for_each_channel_msg fn, void *ctx,
				nvme_bdev_for_each_channel_done cpl);

/* [한국어] bdev 채널 순회 진행 헬퍼 (continue). */
void nvme_bdev_for_each_channel_continue(struct nvme_bdev_channel_iter *iter,
		int status);

/*
 * [한국어]
 * nvme_bdev_ctrlr_get_ctrlr_by_id - 멀티패스 그룹에서 NVMe Controller ID(cntlid)로 컨트롤러 검색.
 * @nbdev_ctrlr: 그룹.
 * @cntlid: NVMe-oF Controller ID (Identify로부터 얻음).
 * @return: 일치하는 nvme_ctrlr, 없으면 NULL.
 */
struct nvme_ctrlr *nvme_bdev_ctrlr_get_ctrlr_by_id(struct nvme_bdev_ctrlr *nbdev_ctrlr,
		uint16_t cntlid);

/*
 * [한국어]
 * nvme_bdev_ctrlr_get_by_name - 그룹 이름으로 멀티패스 그룹 lookup.
 */
struct nvme_bdev_ctrlr *nvme_bdev_ctrlr_get_by_name(const char *name);

/* [한국어] 모든 nvme_bdev_ctrlr를 순회하며 fn(nbdev_ctrlr, ctx)를 호출하는 콜백 타입. */
typedef void (*nvme_bdev_ctrlr_for_each_fn)(struct nvme_bdev_ctrlr *nbdev_ctrlr, void *ctx);

/*
 * [한국어]
 * nvme_bdev_ctrlr_for_each - g_nvme_bdev_ctrlrs를 순회.
 * RPC bdev_nvme_get_controllers 같은 정보 출력에 사용.
 */
void nvme_bdev_ctrlr_for_each(nvme_bdev_ctrlr_for_each_fn fn, void *ctx);

/*
 * [한국어]
 * nvme_bdev_dump_trid_json - transport ID를 JSON 객체로 직렬화 (RPC 응답용).
 */
void nvme_bdev_dump_trid_json(const struct spdk_nvme_transport_id *trid,
			      struct spdk_json_write_ctx *w);

/*
 * [한국어]
 * nvme_ctrlr_info_json - 한 nvme_ctrlr의 상태/설정을 JSON으로 직렬화.
 */
void nvme_ctrlr_info_json(struct spdk_json_write_ctx *w, struct nvme_ctrlr *nvme_ctrlr);

/*
 * [한국어]
 * nvme_ctrlr_get_ns - nsid로 namespace 검색 (RB-tree, O(log n)).
 * @return: nvme_ns 포인터, 없으면 NULL.
 * 컨텍스트: app 스레드.
 */
struct nvme_ns *nvme_ctrlr_get_ns(struct nvme_ctrlr *nvme_ctrlr, uint32_t nsid);
/*
 * [한국어]
 * nvme_ctrlr_get_first_active_ns - active(ANA Optimized/Non-Optimized) namespace 중 가장 작은 nsid.
 * 순회 시작점.
 */
struct nvme_ns *nvme_ctrlr_get_first_active_ns(struct nvme_ctrlr *nvme_ctrlr);
/*
 * [한국어]
 * nvme_ctrlr_get_next_active_ns - 현재 ns 다음의 active namespace.
 * @ns: 현재 ns. NULL이면 first와 동일.
 * @return: 다음 ns, 없으면 NULL (순회 종료).
 */
struct nvme_ns *nvme_ctrlr_get_next_active_ns(struct nvme_ctrlr *nvme_ctrlr, struct nvme_ns *ns);

/*
 * [한국어]
 * bdev_nvme_get_io_qpair - 채널 컨텍스트에서 lib/nvme의 IO qpair 핸들 추출.
 * @ctrlr_io_ch: spdk_get_io_channel(nvme_ctrlr) 결과.
 * @return: 그 채널의 spdk_nvme_qpair (NVMe raw cmd passthrough에 사용).
 */
struct spdk_nvme_qpair *bdev_nvme_get_io_qpair(struct spdk_io_channel *ctrlr_io_ch);
/*
 * [한국어]
 * bdev_nvme_set_hotplug - 핫플러그 감지 활성화/주기 설정.
 * @enabled: true면 핫플러그 polling 시작.
 * @period_us: 폴링 주기 (마이크로초).
 * 컨텍스트: app 스레드.
 */
int bdev_nvme_set_hotplug(bool enabled, uint64_t period_us);

/*
 * [한국어]
 * bdev_nvme_start_discovery - NVMe-oF Discovery Service에 연결해 자동 attach 시작.
 *
 * @trid: Discovery 컨트롤러의 트랜스포트 ID (subnqn은 보통 nqn.2014-08.org.nvmexpress.discovery).
 * @base_name: 발견된 컨트롤러를 등록할 베이스 이름.
 * @drv_opts: lib/nvme 드라이버 옵션.
 * @bdev_opts: bdev_nvme 옵션.
 * @timeout: 디스커버리 타임아웃 (us).
 * @from_mdns: mDNS 경유로 호출되었는지 여부 (config_json 출력 분기).
 * @cb_fn/cb_ctx: 첫 attach까지 완료 시 호출될 콜백.
 * @return: 0 성공, 음수 errno.
 *
 * 디스커버리 컨트롤러에 연결해 Discovery Log Page를 폴링하며 새 NVM 서브시스템이
 * 등장할 때마다 bdev_nvme_create()를 호출해 자동 attach한다.
 */
int bdev_nvme_start_discovery(struct spdk_nvme_transport_id *trid, const char *base_name,
			      struct spdk_nvme_ctrlr_opts *drv_opts, struct spdk_bdev_nvme_ctrlr_opts *bdev_opts,
			      uint64_t timeout, bool from_mdns,
			      spdk_bdev_nvme_start_discovery_fn cb_fn, void *cb_ctx);
/*
 * [한국어]
 * bdev_nvme_stop_discovery - 진행 중인 디스커버리 세션 중단.
 * @name: 디스커버리 base_name.
 * @cb_fn/cb_ctx: 정리 완료 시 호출.
 */
int bdev_nvme_stop_discovery(const char *name, spdk_bdev_nvme_stop_discovery_fn cb_fn,
			     void *cb_ctx);
/*
 * [한국어]
 * bdev_nvme_get_discovery_info - 현재 활성 디스커버리 세션 정보를 JSON으로 출력.
 */
void bdev_nvme_get_discovery_info(struct spdk_json_write_ctx *w);

/*
 * [한국어]
 * bdev_nvme_start_mdns_discovery - mDNS(Avahi)로 NVMe-oF Discovery 서비스를 자동 발견 후 연결.
 * @base_name: 결과 컨트롤러 베이스 이름.
 * @svcname: mDNS service name (예: "_nvme-disc._tcp").
 * mDNS 클라이언트는 bdev_mdns_client.c에 있다.
 */
int bdev_nvme_start_mdns_discovery(const char *base_name,
				   const char *svcname,
				   struct spdk_nvme_ctrlr_opts *drv_opts,
				   struct spdk_bdev_nvme_ctrlr_opts *bdev_opts);
/* [한국어] mDNS 디스커버리 중단. */
int bdev_nvme_stop_mdns_discovery(const char *name);
/* [한국어] mDNS 디스커버리 정보 RPC 응답으로 직렬화. */
void bdev_nvme_get_mdns_discovery_info(struct spdk_jsonrpc_request *request);
/* [한국어] save_config 시 mDNS 디스커버리 설정 저장. */
void bdev_nvme_mdns_discovery_config_json(struct spdk_json_write_ctx *w);

/* [한국어] DH-CHAP/PSK 키 설정 완료 콜백 타입. */
typedef void (*bdev_nvme_set_keys_cb)(void *ctx, int status);

/*
 * [한국어]
 * bdev_nvme_set_keys - 컨트롤러에 DH-CHAP 키를 설정한다 (NVMe over TLS/DHCHAP 인증).
 * @name: 컨트롤러/그룹 이름.
 * @dhchap_key: 호스트 키 식별자.
 * @dhchap_ctrlr_key: 컨트롤러 키 식별자 (양방향 인증).
 * @cb_fn/cb_ctx: 비동기 완료 콜백.
 */
int bdev_nvme_set_keys(const char *name, const char *dhchap_key, const char *dhchap_ctrlr_key,
		       bdev_nvme_set_keys_cb cb_fn, void *cb_ctx);

/*
 * [한국어]
 * bdev_nvme_get_ctrlr - spdk_bdev에서 lib/nvme의 spdk_nvme_ctrlr를 추출.
 * @bdev: bdev 포인터 (반드시 nvme bdev여야 함).
 * @return: 해당 namespace를 소유한 컨트롤러 핸들 (NULL이면 nvme bdev가 아님).
 *
 * 외부 모듈이 NVMe spec 정보(Identify 등)를 직접 조회하고 싶을 때 사용.
 */
struct spdk_nvme_ctrlr *bdev_nvme_get_ctrlr(struct spdk_bdev *bdev);

/*
 * [한국어]
 * enum nvme_ctrlr_op - nvme_ctrlr_op_rpc/nvme_bdev_ctrlr_op_rpc에서 지정하는 op 종류.
 */
enum nvme_ctrlr_op {
	NVME_CTRLR_OP_RESET = 1,   /* [한국어] 컨트롤러 reset (CC.EN 0→1, qpair 재생성). */
	NVME_CTRLR_OP_ENABLE,      /* [한국어] disabled 상태에서 다시 활성화. */
	NVME_CTRLR_OP_DISABLE,     /* [한국어] 새 IO 차단 + qpair 비활성화. */
};

/**
 * Perform specified operation on an NVMe controller.
 *
 * NOTE: The callback function is always called after this function returns except for
 * out of memory cases.
 *
 * \param nvme_ctrlr The specified NVMe controller to operate
 * \param op Operation code
 * \param cb_fn Function to be called back after operation completes
 * \param cb_arg Argument for callback function
 */
void nvme_ctrlr_op_rpc(struct nvme_ctrlr *nvme_ctrlr, enum nvme_ctrlr_op op,
		       bdev_nvme_ctrlr_op_cb cb_fn, void *cb_arg);

/**
 * Perform specified operation on all NVMe controllers in an NVMe bdev controller.
 *
 * NOTE: The callback function is always called after this function returns except for
 * out of memory cases.
 *
 * \param nbdev_ctrlr The specified NVMe bdev controller to operate
 * \param op Operation code
 * \param cb_fn Function to be called back after operation completes
 * \param cb_arg Argument for callback function
 */
void nvme_bdev_ctrlr_op_rpc(struct nvme_bdev_ctrlr *nbdev_ctrlr, enum nvme_ctrlr_op op,
			    bdev_nvme_ctrlr_op_cb cb_fn, void *cb_arg);

/* [한국어] preferred path 설정 완료 콜백 타입. */
typedef void (*bdev_nvme_set_preferred_path_cb)(void *cb_arg, int rc);

/**
 * Set the preferred I/O path for an NVMe bdev in multipath mode.
 *
 * NOTE: This function does not support NVMe bdevs in failover mode.
 *
 * \param name NVMe bdev name
 * \param cntlid NVMe-oF controller ID
 * \param cb_fn Function to be called back after completion.
 * \param cb_arg Argument for callback function.
 */
/*
 * [한국어]
 * bdev_nvme_set_preferred_path - active-active 멀티패스에서 우선 경로 지정.
 *
 * 모든 채널에 메시지를 보내 io_path 캐시(current_io_path)를 cntlid가 가리키는 path로
 * 강제 변경한다. failover 모드(active-passive)에서는 동작하지 않으며, 보통 ANA
 * Optimized 경로 중 특정 컨트롤러를 선호하고 싶을 때 사용한다.
 */
void bdev_nvme_set_preferred_path(const char *name, uint16_t cntlid,
				  bdev_nvme_set_preferred_path_cb cb_fn, void *cb_arg);

#endif /* [한국어] SPDK_BDEV_NVME_H 다중 포함 방지 가드 종료. */
