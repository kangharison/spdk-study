/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] NVMe-oF Target 의 Transport 추상화 레이어 구현 (transport.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVMe over Fabrics(NVMe-oF) target 측의 **트랜스포트 추상화 레이어**를 구현한다.
 * NVMe-oF는 RDMA / TCP / FC / vfio_user 등 다양한 fabric 매체 위에서 NVMe 프로토콜을 운반하는데,
 * 각 fabric 백엔드는 `struct spdk_nvmf_transport_ops`라는 vtable(함수 포인터 테이블)을 구현하여
 * `SPDK_NVMF_TRANSPORT_REGISTER(name, &xxx_ops)` 매크로(GCC `__attribute__((constructor))`로 전개)
 * 를 통해 main() 진입 전에 자동으로 전역 TAILQ `g_spdk_nvmf_transport_ops` 에 자기 자신을 등록한다.
 * 이 파일은 (1) 그 vtable 등록/조회 레지스트리 (2) `spdk_nvmf_transport_create/destroy` 진입점
 * (3) listener 추가/제거의 ref counting 관리 (4) 트랜스포트 poll_group(tgroup)의 생명주기 +
 * spdk_poller 등록/해제 (5) per-transport 공유 데이터 버퍼 풀(spdk_iobuf 모듈) 관리 (6) request
 * 의 IOV(buf cache) 할당/반환 (7) 모든 vtable 콜백(req_free/req_complete/qpair_fini/poll/listen 등)
 * 을 fabric에 무관하게 호출할 수 있는 dispatcher 함수를 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe-oF target 의 호출 계층은 다음과 같다.
 *   [상위] spdk_nvmf_tgt (subsystem CRUD, listener fanout, poll_group 관리)
 *     ↓ spdk_nvmf_transport_create / listen / stop_listen / destroy 진입
 *   [이 파일] 트랜스포트 추상화 (transport.c) — vtable 디스패치만 수행, 실제 SQE/CQE 핸들링 없음
 *     ↓ transport->ops->xxx (RDMA: rdma.c / TCP: tcp.c / FC: fc.c / vfio_user: vfio_user.c)
 *   [하위] 백엔드 구현 — RDMA verbs(ibv_post_send 등) / TCP socket+epoll / FC HBA driver / vfio_user UNIX socket
 * 실행 컨텍스트는 spdk_thread/reactor 단일 스레드 모델을 따른다. 그러나 transport 객체 자체는
 * 모든 reactor에서 공유되므로 listener 추가/제거 등 트랜스포트 단위 mutate 시에는
 * `transport->mutex` (pthread_mutex_t) 로 보호된다. 반면 per-tgroup poll/req 처리는 lockless.
 *
 * === 타 모듈과의 연결 ===
 * - `include/spdk/nvmf_transport.h` : 본 파일이 구현하는 모든 vtable 콜백 타입과 spdk_nvmf_transport_ops
 *   레이아웃을 정의. SPDK_NVMF_TRANSPORT_REGISTER 매크로도 여기서 정의됨.
 * - `include/spdk/nvmf.h` : 외부 사용자 API (spdk_nvmf_transport_create/destroy/listen 등) 선언.
 * - `nvmf_internal.h` / `transport.h` : nvmf target 내부 구조체(spdk_nvmf_listener, spdk_nvmf_tgt,
 *   spdk_nvmf_subsystem, spdk_nvmf_qpair, spdk_nvmf_request, spdk_nvmf_transport_poll_group 등) 정의.
 * - `lib/util/spdk_iobuf` : 공유 데이터 버퍼 풀(DPDK rte_mempool 위에 구축). I/O 데이터 영역 zero-copy
 *   할당. `nvmf_<trtype>` 이름으로 모듈 등록 → poll_group 생성 시 channel 단위 캐시 분배.
 * - `lib/thread` : spdk_poller_register_named 로 트랜스포트별 poller 등록 (이름 "nvmf_<trtype>").
 * - 백엔드 구현(rdma.c / tcp.c 등) : 이 파일은 ops 호출만 수행, 실제 데이터 흐름은 백엔드가 담당.
 * 데이터 흐름:
 *   호스트 connect → TCP/RDMA 백엔드 accept → 백엔드가 spdk_nvmf_qpair 생성 → tgroup 추가 →
 *   poll_group_poll() polled-mode → 각 백엔드가 SQE 수신/CQE 송신 → req_free/req_complete 콜백.
 *
 * === 주요 함수/구조체 요약 ===
 * - `g_spdk_nvmf_transport_ops` (전역 TAILQ): SPDK_NVMF_TRANSPORT_REGISTER constructor가 등록한
 *   모든 백엔드 vtable의 레지스트리. 한 SPDK 프로세스에서 RDMA/TCP/FC 등 여러 백엔드가 공존 가능.
 * - `spdk_nvmf_transport_register()` : 백엔드 vtable 등록. constructor에서 호출. 중복 검사 + 복사.
 * - `nvmf_get_transport_ops()` : 이름(strcasecmp)으로 vtable 조회. 모든 transport_create/opts_init 진입점.
 * - `spdk_nvmf_transport_create() / _async()` : 사용자 진입점. opts ABI 검증 → vtable->create 호출.
 *   sync(즉시 반환) vs async(create_async + nvmf_transport_create_async_done 콜백) 두 경로 지원.
 * - `spdk_nvmf_transport_destroy()` : listener 모두 정리 + iobuf 모듈 해제 + vtable->destroy 호출.
 * - `spdk_nvmf_transport_listen() / _stop_listen() / _stop_listen_async()` : listener ref count + vtable
 *   listen/stop_listen 호출. async 버전은 모든 poll_group을 순회하며 매칭 qpair를 disconnect 후 해제.
 * - `nvmf_transport_poll_group_create() / _destroy() / _add() / _remove() / _poll()` : tgroup 생명주기.
 *   spdk_poller_register_named 로 reactor poller 등록 + iobuf channel(buf_cache) 초기화.
 * - `spdk_nvmf_request_get_buffers() / _free_buffers()` : I/O 데이터 영역을 iobuf 풀에서 IOV 단위로
 *   할당/반환. 풀 고갈 시 `nvmf_request_iobuf_get_cb` 큐잉 후 가용 시 콜백 재진입.
 * - `nvmf_transport_opts_copy()` : opts_size 기반 ABI 호환 SET_FIELD 매크로 패턴. 새 필드 추가 시
 *   SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_transport_opts) == 82) 갱신 필수.
 * - 핵심 구조체:
 *     `struct nvmf_transport_ops_list_element` : g_spdk_nvmf_transport_ops TAILQ 원소(ops 복사본).
 *     `struct nvmf_transport_create_ctx` : 비동기 create 도중 보관할 인자 묶음.
 *     `struct nvmf_stop_listen_ctx` : stop_listen_async 의 spdk_for_each_channel 컨텍스트.
 */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 인클루드 묶음 (stdint.h, stdbool.h, string.h, pthread.h 등 OS-신경 안 쓰는 추상화).
 *  - 왜 필요: calloc/free/snprintf/pthread_mutex_* 등 본 파일 전반 사용. */

#include "nvmf_internal.h"
/* [한국어] nvmf target 내부 헤더 — spdk_nvmf_tgt/subsystem/qpair/request/listener 구조체 풀 정의.
 *  - 왜 필요: 본 파일이 transport->listeners TAILQ, subsystem->listeners 순회, qpair->ctrlr 비교 등 내부 필드를 직접 참조. */
#include "transport.h"
/* [한국어] lib/nvmf 내부 transport 헤더 — nvmf_transport_*() 사설 함수 선언 (외부 API spdk_nvmf_*는 nvmf_transport.h).
 *  - 왜 필요: 본 파일이 정의하는 nvmf_transport_find_listener / poll_group_create 등의 선언. */

#include "spdk/config.h"
/* [한국어] 빌드 시 결정된 SPDK 컴파일 옵션 헤더 — SPDK_CONFIG_RDMA 등 매크로.
 *  - 왜 필요: 트랜스포트별 fabric 의존성 분기에 사용 (이 파일 직접 분기는 없지만 공용 인클루드). */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG / SPDK_WARNLOG / SPDK_NOTICELOG / SPDK_DEBUGLOG 매크로.
 *  - 왜 필요: 본 파일 곳곳의 에러/경고 로그. */
#include "spdk/nvmf.h"
/* [한국어] NVMe-oF 사용자 공개 API — spdk_nvmf_transport / opts / listen_opts / target lifecycle.
 *  - 왜 필요: 본 파일이 구현하는 spdk_nvmf_transport_create/destroy/listen 등의 외부 시그니처. */
#include "spdk/nvmf_transport.h"
/* [한국어] 트랜스포트 vtable 정의 (spdk_nvmf_transport_ops 30+ 콜백) + SPDK_NVMF_TRANSPORT_REGISTER 매크로 + 핵심 구조체.
 *  - 왜 필요: 본 파일이 vtable 디스패치하는 핵심 인터페이스. */
#include "spdk/queue.h"
/* [한국어] BSD-style intrusive list/queue 매크로 (TAILQ_HEAD/_INIT/_INSERT_TAIL/_FOREACH/_REMOVE/STAILQ_*).
 *  - 왜 필요: g_spdk_nvmf_transport_ops, transport->listeners, tgroup->pending_buf_queue 모두 TAILQ/STAILQ. */
#include "spdk/util.h"
/* [한국어] SPDK 공통 유틸 — SPDK_CONTAINEROF / SPDK_CEIL_DIV / spdk_min / spdk_round_up / spdk_u32_is_pow2 등.
 *  - 왜 필요: max_io_size 검증의 spdk_u32_is_pow2, IOV 길이의 SPDK_CEIL_DIV, iobuf 콜백의 SPDK_CONTAINEROF. */
#include "spdk_internal/usdt.h"
/* [한국어] DTrace/USDT(Userland Statically Defined Tracing) 프로브 매크로 — SPDK_DTRACE_PROBE1/3.
 *  - 왜 필요: nvmf_transport_poll_group_add/remove/qpair_fini 트레이싱 후크. 운영 환경에서 perf/dtrace 로 추적 가능. */

#define NVMF_TRANSPORT_DEFAULT_ASSOCIATION_TIMEOUT_IN_MS 120000
/* [한국어] NVMe-oF "Association Timeout" 의 기본값 (밀리초). 120000ms = 120초 = 2분.
 *  - 의미: NVMe-oF 1.1 스펙의 association(host-controller 연결 세션) 이 idle 상태로 머무를 수 있는 최대 시간.
 *    이 시간 초과 시 controller 가 association 을 강제 종료(connection drop)할 수 있음.
 *  - 사용처: spdk_nvmf_transport_opts_init() 의 opts_local.association_timeout 기본 채움값.
 *  - 백엔드별: 실제 동작은 vtable 의 keep-alive/timeout 로직이 담당. 이 매크로는 유저가 opts 를 안 바꾸면
 *    적용되는 default. RPC 로 "association_timeout" 키를 설정하면 덮어씀. */

/*
 * [한국어] struct nvmf_transport_ops_list_element
 *
 * g_spdk_nvmf_transport_ops 전역 TAILQ 의 원소. 한 백엔드(RDMA/TCP/FC/...) 당 하나씩 존재한다.
 * SPDK_NVMF_TRANSPORT_REGISTER 매크로의 constructor 가 calloc 으로 할당하여 ops 필드에 백엔드 vtable
 * 의 *복사본*을 보관(원본 전역 변수가 사라져도 복사본은 안전) 후 link 로 TAILQ 에 연결한다.
 * 라이프사이클: SPDK 프로세스 시작 시 자동 등록 → 종료 시까지 그대로 (해제 unregister 경로 없음).
 */
struct nvmf_transport_ops_list_element {
	struct spdk_nvmf_transport_ops			ops;
	/* [한국어] 백엔드가 제공한 spdk_nvmf_transport_ops vtable 의 깊은 복사본.
	 *  설정자: spdk_nvmf_transport_register() 가 등록 시점에 `*ops = *src_ops` 로 복사.
	 *  읽는 자: 모든 vtable 디스패처(transport->ops->xxx 호출 시 transport->ops 가 이 필드의 주소).
	 *  값 범위: 30+ 함수 포인터 (name, type, opts_init, create, destroy, listen, stop_listen, listener_discover,
	 *           poll_group_create/destroy/add/remove/poll, req_free, req_complete, qpair_fini,
	 *           qpair_get_*_trid, qpair_abort_request, dump_opts, req_get_buffers_done, get_optimal_poll_group, ...).
	 *           일부는 NULL 가능(optional) — 호출 전 NULL 체크 필요.
	 *  동기화: TAILQ 에 들어간 이후로는 R/O. 멀티 쓰레드 동시 읽기 안전. */
	TAILQ_ENTRY(nvmf_transport_ops_list_element)	link;
	/* [한국어] g_spdk_nvmf_transport_ops TAILQ 연결 노드 (next/prev 포인터).
	 *  설정자: TAILQ_INSERT_TAIL by spdk_nvmf_transport_register().
	 *  읽는 자: TAILQ_FOREACH by nvmf_get_transport_ops() / spdk_nvmf_transport_get_first/_next.
	 *  값 범위: 다른 동일 타입 원소 또는 head sentinel.
	 *  동기화: 등록은 constructor(단일 thread) 또는 dlopen 시점만 발생 가정 — 별도 락 없음.
	 *           삭제 경로(unregister) 가 SPDK 에는 정의되어 있지 않다 → list 는 monotonic. */
};

/*
 * [한국어] g_spdk_nvmf_transport_ops — 등록된 모든 NVMe-oF 트랜스포트 백엔드 vtable 의 전역 레지스트리.
 *
 * 라이프사이클: BSS 영역 빈 head 로 시작 (TAILQ_HEAD_INITIALIZER) → 각 .so/링크된 백엔드의
 *              SPDK_NVMF_TRANSPORT_REGISTER constructor 가 main() 진입 전에 등록 → 프로세스 종료까지 유지.
 *
 * 설정자: spdk_nvmf_transport_register() 만이 INSERT 한다 (외부 unregister 경로 없음).
 * 읽는 자: nvmf_get_transport_ops()(name 으로 조회), spdk_nvmf_transport_get_first/_next(목록 열거).
 * 동기화: 등록은 program-startup 시점에 단일 thread 에서 발생한다고 가정. 조회는 R/O 이므로 lock-free.
 */
TAILQ_HEAD(nvmf_transport_ops_list, nvmf_transport_ops_list_element)
g_spdk_nvmf_transport_ops = TAILQ_HEAD_INITIALIZER(g_spdk_nvmf_transport_ops);

/*
 * [한국어]
 * nvmf_get_transport_ops - 이름으로 등록된 transport vtable 조회 (대소문자 무시)
 *
 * @transport_name: 백엔드 이름 문자열 ("RDMA"/"TCP"/"FC"/"VFIOUSER" 등). 사용자가 RPC/CLI 로 입력.
 *                  대소문자 무관(strcasecmp). NULL 이면 strcasecmp 가 segfault 하므로 호출자 책임.
 * @return: 매칭되는 const spdk_nvmf_transport_ops* (TAILQ 원소의 복사본 주소) 또는 NULL(미등록).
 *
 * 모든 사용자 진입점(transport_create / opts_init)이 백엔드를 식별하기 위해 처음 호출한다.
 * static inline 으로 선언되어 호출자에 인라이닝 가능. TAILQ_FOREACH 는 monotonic 리스트라 lock 불필요.
 *
 * 호출 체인:
 *   spdk_nvmf_transport_register / _create / _create_async / _opts_init
 *     → [nvmf_get_transport_ops] → 매칭 ops 복사본 주소 반환 → 호출자가 ops->create / opts_init 등 호출.
 */
static inline const struct spdk_nvmf_transport_ops *
nvmf_get_transport_ops(const char *transport_name)
{
	struct nvmf_transport_ops_list_element *ops;
	/* [한국어] 순회용 임시 포인터 - TAILQ_FOREACH 매크로가 내부에서 갱신. */
	TAILQ_FOREACH(ops, &g_spdk_nvmf_transport_ops, link) {
		/* [한국어] 등록된 백엔드 리스트 head→tail 순회. 각 ops 는 spdk_nvmf_transport_register() 가 등록한 vtable 복사본. */
		if (strcasecmp(transport_name, ops->ops.name) == 0) {
			/* [한국어] 대소문자 무시 비교 — 사용자가 "RDMA"/"rdma"/"Rdma" 어떤 형태로 입력해도 매칭.
			 *  ops->ops.name 은 백엔드가 자기 자신을 등록할 때 정의한 정식 문자열(예: "RDMA"). */
			return &ops->ops;
			/* [한국어] 매칭된 vtable 복사본의 주소 반환. 호출자는 이 포인터를 transport->ops 에 저장. */
		}
	}
	return NULL;
	/* [한국어] 미등록 트랜스포트 — 호출자가 SPDK_ERRLOG + 실패 처리. RDMA 가 없는 빌드에서 "RDMA" 요청 시 등. */
}

/*
 * [한국어]
 * spdk_nvmf_transport_register - 백엔드 vtable 을 전역 레지스트리에 등록 (constructor 진입점)
 *
 * @ops: 등록할 spdk_nvmf_transport_ops 구조체 포인터. 백엔드 모듈의 정적 전역 변수를 가리킴.
 *       이 함수는 ops 의 *복사본*을 만들기 때문에 호출 후 원본 변수가 사라져도 안전.
 *
 * **호출 컨텍스트**: SPDK_NVMF_TRANSPORT_REGISTER(name, &xxx_ops) 매크로가 GCC
 * `__attribute__((constructor))` 로 전개되어 main() 진입 전 자동으로 호출. 즉 정적 초기화 단계.
 * 따라서 단일 thread, 단일 프로세스 시점이라고 가정 → 락 불필요.
 *
 * 동작:
 *  1) 같은 이름으로 이미 등록된 백엔드가 있는지 검사 (이중 등록 방지).
 *  2) calloc 으로 nvmf_transport_ops_list_element 할당 (실패 시 assert).
 *  3) ops 를 복사 (depth-1 복사 — 함수 포인터 자체는 코드 영역이라 복사본도 동일 코드).
 *  4) g_spdk_nvmf_transport_ops 의 tail 에 INSERT.
 *
 * 에러 처리: 중복/OOM 모두 assert(false) 후 return → debug 빌드에선 abort, release 빌드에선 조용히 무시.
 *
 * 호출 체인:
 *   백엔드 .so 의 SPDK_NVMF_TRANSPORT_REGISTER constructor → [spdk_nvmf_transport_register]
 *     → calloc + memcpy + TAILQ_INSERT_TAIL → 이후 사용자가 nvmf_get_transport_ops 로 조회 가능.
 */
void
spdk_nvmf_transport_register(const struct spdk_nvmf_transport_ops *ops)
{
	struct nvmf_transport_ops_list_element *new_ops;
	/* [한국어] 신규 할당할 레지스트리 원소 포인터. */

	if (nvmf_get_transport_ops(ops->name) != NULL) {
		/* [한국어] 동일 이름 중복 등록 검사 — 두 .so 가 같은 이름으로 등록하거나 dlopen 두 번 시.
		 *  스펙적으로는 한 프로세스에 한 백엔드 인스턴스만 허용. */
		SPDK_ERRLOG("Double registering nvmf transport type %s.\n", ops->name);
		/* [한국어] 사용자/패키저가 발견할 수 있도록 ERR 로그. release 빌드에서도 출력. */
		assert(false);
		/* [한국어] debug 빌드에선 즉시 abort 하여 빌드 시스템 오류로 noticing 강제. */
		return;
		/* [한국어] release 빌드에선 assert 가 no-op 이므로 조용히 빠져나감 (기존 등록 보존). */
	}

	new_ops = calloc(1, sizeof(*new_ops));
	/* [한국어] 신규 원소 할당 + 0 초기화. constructor 단계라 hugepage 의존 못 함 → 일반 calloc 사용.
	 *  malloc 대신 calloc 인 이유: 향후 새 필드 추가 시 자동 0 클리어 보장 (포워드 호환). */
	if (new_ops == NULL) {
		/* [한국어] 시스템 메모리 부족 — startup 단계에 calloc 실패는 거의 불가능하나 방어적. */
		SPDK_ERRLOG("Unable to allocate memory to register new transport type %s.\n", ops->name);
		assert(false);
		/* [한국어] debug 빌드에선 abort. 등록 실패 시 해당 백엔드는 사용 불가 상태로 남는다. */
		return;
	}

	new_ops->ops = *ops;
	/* [한국어] vtable 깊은 복사 (구조체 단위 대입). 함수 포인터 + name 등 모두 한 번에 복사.
	 *  이후 백엔드의 원본 ops 전역이 사라져도(예: 동적 라이브러리 unload) 본 복사본은 유지.
	 *  단, 함수 포인터가 가리키는 코드는 .so 가 unmap 되면 이미 무효 → 실제로는 unload 미지원. */

	TAILQ_INSERT_TAIL(&g_spdk_nvmf_transport_ops, new_ops, link);
	/* [한국어] 전역 레지스트리 tail 에 추가. constructor 들 사이의 등록 순서가 사용자 노출 순서가 됨
	 *  (spdk_nvmf_transport_get_first/_next 가 head→tail 순회). 단, 호출자는 의존하면 안 됨. */
}

/*
 * [한국어]
 * spdk_nvmf_get_transport_opts - 트랜스포트의 현재 opts 구조체에 대한 R/O 포인터 반환
 *
 * @transport: 대상 트랜스포트 객체. spdk_nvmf_transport_create() 가 반환한 것.
 * @return: const opts* (수정 금지). 호출자는 max_io_size/io_unit_size 등 즉시 읽어볼 때 사용.
 *
 * RPC 핸들러("nvmf_get_transports")와 nvmf_transport_dump_opts() 가 트랜스포트 설정값을 외부에
 * 노출하기 위해 호출. transport 객체 자체의 lifetime 동안 opts 는 변하지 않으므로 lock 불필요
 * (create 시 한 번 고정). qpair/poll_group 처리 hot path 에서도 transport->opts 를 직접 읽지만
 * 본 함수는 사용자 API 라 명시적 wrapper 가 필요한 것.
 */
const struct spdk_nvmf_transport_opts *
spdk_nvmf_get_transport_opts(struct spdk_nvmf_transport *transport)
{
	return &transport->opts;
	/* [한국어] transport 구조체에 임베드된 opts 의 주소 반환. 별도 락 없이 안전 (lifetime-stable). */
}

/*
 * [한국어]
 * nvmf_transport_dump_opts - 트랜스포트 opts 를 JSON 으로 직렬화
 *
 * @transport: 직렬화할 트랜스포트 객체.
 * @w: spdk_jsonrpc 의 write context (RPC 응답 또는 config save 출력 대상).
 * @named: true 면 "params" 라는 이름의 named object 로 시작 (RPC config save 형식),
 *         false 면 anonymous object (RPC method response 형식).
 *
 * RPC 호출(nvmf_get_transports / save_config)에서 운영자에게 트랜스포트 설정을 보여줄 때 사용.
 * 공통 필드는 본 함수에서 직접 출력하고, 백엔드 고유 필드는 vtable->dump_opts(NULL 가능)에 위임.
 *
 * 호출 체인:
 *   RPC 핸들러(nvmf_rpc_get_transports / nvmf_save_config) → [nvmf_transport_dump_opts]
 *     → spdk_json_write_named_*(w) 직접 출력 + transport->ops->dump_opts(w) (백엔드 specific).
 */
void
nvmf_transport_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w,
			 bool named)
{
	const struct spdk_nvmf_transport_opts *opts = spdk_nvmf_get_transport_opts(transport);
	/* [한국어] 트랜스포트 opts 의 R/O 포인터 획득. 이후 모든 출력은 이 포인터에서 읽음. */

	named ? spdk_json_write_named_object_begin(w, "params") : spdk_json_write_object_begin(w);
	/* [한국어] JSON 객체 시작.
	 *  named=true: { "params": { ... } } 형식 — RPC save_config 가 method+params 를 묶을 때 사용.
	 *  named=false: { ... } 형식 — RPC response 본문에 직접 삽입 시 사용. */

	spdk_json_write_named_string(w, "trtype", spdk_nvmf_get_transport_name(transport));
	/* [한국어] 트랜스포트 타입 이름 출력 (예: "RDMA", "TCP"). vtable->ops->name 에서 가져옴. */
	spdk_json_write_named_uint32(w, "max_queue_depth", opts->max_queue_depth);
	/* [한국어] 한 qpair 의 최대 SQ 깊이(완료 미처리 IO 수 한도). NVMe-oF 1.1 — controller capability. */
	spdk_json_write_named_uint32(w, "max_io_qpairs_per_ctrlr", opts->max_qpairs_per_ctrlr - 1);
	/* [한국어] 한 controller 가 가질 수 있는 *I/O* qpair 최대 개수.
	 *  내부적으로 max_qpairs_per_ctrlr 는 admin 큐(qid=0) 1개 + I/O 큐 N개 → 외부 노출 시 -1 (admin 제외). */
	spdk_json_write_named_uint32(w, "in_capsule_data_size", opts->in_capsule_data_size);
	/* [한국어] In-Capsule Data Size — capsule SQE(64B) 뒤에 붙어오는 immediate data 의 최대 바이트.
	 *  NVMe-oF 1.1 §3 Capsule. RDMA send / TCP ICDoFF 영역에 wire 로 함께 도착. 호스트의 작은 write 를 0-RTT 처리. */
	spdk_json_write_named_uint32(w, "max_io_size", opts->max_io_size);
	/* [한국어] 한 NVMe 명령으로 전송 가능한 최대 데이터 크기 (바이트). 2의 거듭제곱이어야 함. 8KB 이상. */
	spdk_json_write_named_uint32(w, "io_unit_size", opts->io_unit_size);
	/* [한국어] iobuf 풀에서 IOV 단위로 가져오는 buffer 의 크기. max_io_size 가 io_unit_size 의 정수배여야 효율적. */
	spdk_json_write_named_uint32(w, "max_aq_depth", opts->max_aq_depth);
	/* [한국어] Admin Queue 깊이. NVMe spec 의 Admin SQ size (CC.AMS / CAP.MQES 와 매핑).
	 *  최소값은 SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE — 작으면 자동 보정. */
	spdk_json_write_named_uint32(w, "num_shared_buffers", opts->num_shared_buffers);
	/* [한국어] 트랜스포트가 iobuf 풀에 요청하는 공유 버퍼 개수. 모든 reactor 가 공유. */
	spdk_json_write_named_uint32(w, "buf_cache_size", opts->buf_cache_size);
	/* [한국어] 각 poll_group 의 buf_cache(local cache) 크기.
	 *  UINT32_MAX 이면 num_shared_buffers * 0.75 / num_poll_groups 자동 계산 (poll_group_create 참조). */
	spdk_json_write_named_bool(w, "dif_insert_or_strip", opts->dif_insert_or_strip);
	/* [한국어] DIF(Data Integrity Field — T10 PI/CRC) 자동 insert/strip 여부.
	 *  true 면 호스트가 PI 없는 데이터를 보내도 target 이 인입/추출. NVMe 1.x §6 PI. */
	spdk_json_write_named_bool(w, "zcopy", opts->zcopy);
	/* [한국어] Zero-Copy 데이터 경로 활성화 여부. true 면 RDMA RDMA_READ/WRITE 또는 TCP MSG_ZEROCOPY 활용. */

	if (transport->ops->dump_opts) {
		/* [한국어] 백엔드별 추가 필드 출력 콜백 호출 (RDMA: srq_depth, num_cqe / TCP: sock_priority 등).
		 *  optional — NULL 인 백엔드도 있음. */
		transport->ops->dump_opts(transport, w);
	}

	spdk_json_write_named_uint32(w, "abort_timeout_sec", opts->abort_timeout_sec);
	/* [한국어] Abort 명령 타임아웃 (초). NVMe Abort cmd 가 이 시간 안에 처리 안 되면 강제 완료. */
	spdk_json_write_named_uint32(w, "ack_timeout", opts->ack_timeout);
	/* [한국어] TCP/RDMA ack 타임아웃. 이 시간 동안 wire 응답이 없으면 connection 이 죽었다고 판단. */
	spdk_json_write_named_uint32(w, "data_wr_pool_size", opts->data_wr_pool_size);
	/* [한국어] RDMA 백엔드의 ibv_send_wr 풀 크기 (zero=자동). RDMA only. */
	spdk_json_write_named_bool(w, "disable_command_passthru", opts->disable_command_passthru);
	/* [한국어] custom admin cmd hooks 비활성화 여부. true 면 spec 정의 명령만 처리, vendor-specific 거부. */
	spdk_json_write_named_uint16(w, "kas", opts->kas);
	/* [한국어] Keep Alive Support — Keep Alive 시간 단위 (100ms 단위, NVMe 1.1 §5.21).
	 *  실제 시간 = kas * NVMF_KAS_TIME_UNIT_IN_MS. 0 이면 invalid (transport_create 에서 거부). */
	spdk_json_write_named_uint32(w, "min_kato", opts->min_kato);
	/* [한국어] 최소 Keep Alive Timeout (밀리초). 호스트가 더 작은 값 설정 시 이 값으로 올림. */
	spdk_json_write_object_end(w);
	/* [한국어] JSON 객체 종료 (열어 둔 named/anonymous object 매칭 닫기). */
}

/*
 * [한국어]
 * nvmf_transport_listen_dump_trid - listener 의 trid (transport ID) 를 JSON 으로 직렬화
 *
 * @trid: 직렬화할 transport ID (trstring/adrfam/traddr/trsvcid 4 필드).
 * @w: spdk_jsonrpc 의 write context.
 *
 * RPC "nvmf_get_listeners" 등이 운영자에게 listener 주소 정보를 보여줄 때 호출. spdk_nvme_transport_id
 * 는 host 측 NVMe 드라이버와 공유되는 구조체로, NVMe-oF 의 "어디로 connect" 를 표현하는 핵심 식별자.
 *
 * 호출 체인:
 *   nvmf_listener_save_config 등 RPC 핸들러 → [nvmf_transport_listen_dump_trid] → JSON 출력.
 */
void
nvmf_transport_listen_dump_trid(const struct spdk_nvme_transport_id *trid,
				struct spdk_json_write_ctx *w)
{
	const char *adrfam = spdk_nvme_transport_id_adrfam_str(trid->adrfam);
	/* [한국어] adrfam (address family) enum → 문자열 변환 ("IPv4"/"IPv6"/"IB"/"FC"/"INTRA_HOST").
	 *  NULL 가능 (unknown enum 값 시) → 아래에서 "unknown" 으로 fallback. */

	spdk_json_write_named_string(w, "trtype", trid->trstring);
	/* [한국어] 트랜스포트 타입 문자열 ("RDMA"/"TCP"/"FC"/"VFIOUSER"). */
	spdk_json_write_named_string(w, "adrfam", adrfam ? adrfam : "unknown");
	/* [한국어] 주소 패밀리. 변환 실패 시 "unknown" 출력 — RPC 호출자는 이 값을 그대로 받아도 invalid 로 인식 가능. */
	spdk_json_write_named_string(w, "traddr", trid->traddr);
	/* [한국어] 전송 주소 (IP 주소 / FC WWN / VFIOUSER socket 경로). */
	spdk_json_write_named_string(w, "trsvcid", trid->trsvcid);
	/* [한국어] 서비스 ID (TCP/RDMA 포트 번호 / FC PortID 등). */
}

/*
 * [한국어]
 * spdk_nvmf_get_transport_type - vtable 의 트랜스포트 타입 enum 반환 (RDMA/TCP/FC/...)
 *
 * @transport: 대상 트랜스포트 객체.
 * @return: spdk_nvme_transport_type_t enum (SPDK_NVME_TRANSPORT_RDMA/TCP/FC/PCIE/VFIOUSER/CUSTOM).
 *
 * 외부에서 트랜스포트 종류를 enum 으로 분기할 때 (예: discovery log 생성 시 trtype 채움) 사용.
 * vtable 의 type 필드는 백엔드가 SPDK_NVMF_TRANSPORT_REGISTER 시점에 고정.
 */
spdk_nvme_transport_type_t
spdk_nvmf_get_transport_type(struct spdk_nvmf_transport *transport)
{
	return transport->ops->type;
	/* [한국어] vtable 의 type 필드를 그대로 반환. lifetime 동안 변경 없음 → lock 불필요. */
}

/*
 * [한국어]
 * spdk_nvmf_get_transport_name - vtable 의 트랜스포트 이름 문자열 반환 ("RDMA"/"TCP"/...)
 *
 * @transport: 대상 트랜스포트 객체.
 * @return: const char* (vtable 내부 정적 문자열 — 호출자 free 금지).
 *
 * RPC 응답이나 로그 출력에서 트랜스포트를 사람이 읽을 수 있는 형태로 보여줄 때 사용.
 */
const char *
spdk_nvmf_get_transport_name(struct spdk_nvmf_transport *transport)
{
	return transport->ops->name;
	/* [한국어] vtable 의 name 필드 반환. 백엔드 코드에 정적 정의된 const 문자열. */
}

/*
 * [한국어]
 * nvmf_transport_opts_copy - opts 구조체 복사 (ABI 호환 SET_FIELD 패턴)
 *
 * @opts: 복사 대상 (목적지). transport->opts 또는 사용자가 spdk_nvmf_transport_opts_init() 의 출력으로 받을 버퍼.
 * @opts_src: 복사 원본 (소스). 사용자/내부가 채운 default+override 값.
 * @opts_size: src 가 알고 있는 sizeof(struct spdk_nvmf_transport_opts) 값.
 *             ★ ABI 호환의 핵심 인자 ★ — 사용자 빌드 시점의 헤더 버전이 작아 일부 필드를 모를 수 있음.
 *
 * **ABI 호환 메커니즘**:
 *   SPDK 라이브러리는 forward-compatible 해야 함. 사용자가 옛 헤더(작은 sizeof)로 컴파일했어도
 *   새 SPDK 라이브러리가 segfault 없이 동작해야 한다. SET_FIELD 매크로는 각 필드의 offset+size 가
 *   opts_size 안에 들어갈 때만 복사 — 사용자가 모르는 필드는 건드리지 않음 (목적지에 garbage 안 씀).
 *
 *   반대 방향(spdk_nvmf_transport_opts_init): 작은 user buf 에 큰 default 를 채울 때, 매크로가
 *   사용자 buf 크기 안에 있는 필드만 채워 buffer overflow 방지.
 *
 *   **새 필드 추가 워크플로** (구조체에 새 필드 추가 시):
 *     1. spdk_nvmf_transport_opts 에 필드 추가 (항상 *맨 끝* — 중간 삽입 금지, ABI 깨짐).
 *     2. 본 함수에 SET_FIELD(new_field) 한 줄 추가.
 *     3. SPDK_STATIC_ASSERT 의 82 를 새 sizeof 로 갱신 (컴파일 타임 검증).
 *     4. 의도적으로 sizeof 검사를 실패시켜 빠짐 방지 (매뉴얼 강제).
 *
 * 호출 체인:
 *   spdk_nvmf_transport_opts_init / nvmf_transport_create → [nvmf_transport_opts_copy] → 필드별 SET.
 */
static void
nvmf_transport_opts_copy(struct spdk_nvmf_transport_opts *opts,
			 struct spdk_nvmf_transport_opts *opts_src,
			 size_t opts_size)
{
	assert(opts);
	/* [한국어] 목적지 NULL 검사 (debug 빌드). 호출자 버그 시 즉시 abort. */
	assert(opts_src);
	/* [한국어] 원본 NULL 검사. */

	opts->opts_size = opts_size;
	/* [한국어] 목적지의 opts_size 필드도 user 가 준 sizeof 로 기록.
	 *  이후 본 opts 를 다른 함수에 넘길 때 그 함수가 다시 ABI 호환 분기에 사용. */

#define SET_FIELD(field) \
	if (offsetof(struct spdk_nvmf_transport_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = opts_src->field; \
	} \

	/* [한국어] SET_FIELD 매크로 정의 — 컴파일 타임 offsetof + sizeof 로 필드 끝 위치 계산.
	 *  user 의 opts_size 안에 그 필드가 통째로 들어가야만 복사 (부분 overlap 도 거부 — 안전 우선). */

	SET_FIELD(max_queue_depth);
	/* [한국어] 한 qpair 의 SQ 깊이. uint16_t. */
	SET_FIELD(max_qpairs_per_ctrlr);
	/* [한국어] 한 controller 의 (admin+I/O) qpair 총 개수. uint16_t. */
	SET_FIELD(in_capsule_data_size);
	/* [한국어] capsule SQE 뒤 immediate data 최대 바이트. uint32_t. */
	SET_FIELD(max_io_size);
	/* [한국어] 한 명령의 max payload bytes. uint32_t. 2^N 강제 / 8KB 이상 (transport_create 검증). */
	SET_FIELD(io_unit_size);
	/* [한국어] iobuf 풀 IOV unit byte. uint32_t. max_io_size 의 약수여야 효율. */
	SET_FIELD(max_aq_depth);
	/* [한국어] Admin Queue 깊이. uint32_t. SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE 미만이면 자동 보정. */
	SET_FIELD(buf_cache_size);
	/* [한국어] poll_group 별 buf cache 크기. uint32_t. UINT32_MAX 면 자동 계산 (75%/poll_groups). */
	SET_FIELD(num_shared_buffers);
	/* [한국어] iobuf 풀 공유 버퍼 개수. uint32_t. */
	SET_FIELD(dif_insert_or_strip);
	/* [한국어] DIF 자동 처리 여부. bool. */
	SET_FIELD(abort_timeout_sec);
	/* [한국어] Abort 명령 타임아웃 초. uint32_t. */
	SET_FIELD(association_timeout);
	/* [한국어] Association idle timeout (ms). default 120000 (2분). uint32_t. */
	SET_FIELD(transport_specific);
	/* [한국어] 백엔드별 추가 설정의 const* (RPC 의 raw JSON 같은 opaque 데이터). 백엔드 opts_init 가 해석. */
	SET_FIELD(acceptor_poll_rate);
	/* [한국어] accept poller 호출 주기 (μs). default SPDK_NVMF_DEFAULT_ACCEPT_POLL_RATE_US. uint32_t. */
	SET_FIELD(zcopy);
	/* [한국어] zero-copy data path 활성화. bool. RDMA/TCP zcopy 분기. */
	SET_FIELD(ack_timeout);
	/* [한국어] wire ACK 타임아웃 (ms). uint32_t. 0 이면 비활성화. */
	SET_FIELD(data_wr_pool_size);
	/* [한국어] RDMA ibv_send_wr 풀 크기. uint32_t. RDMA 만 사용. */
	SET_FIELD(min_kato);
	/* [한국어] 최소 Keep Alive Timeout (ms). uint32_t. 호스트가 더 작은 값 줄 시 보정. */
	SET_FIELD(kas);
	/* [한국어] Keep Alive Support time unit. uint16_t. 실제 KAS_TIME = kas * NVMF_KAS_TIME_UNIT_IN_MS. */
	SET_FIELD(oncs);
	/* [한국어] Optional NVM Command Support 비트맵 (NVMe 1.4 §5.15.2.1 ID Identify Controller).
	 *  union { uint16_t raw; struct {...}; }. 어떤 NVMe optional cmd 를 노출할지 제어. */
	SET_FIELD(fuses);
	/* [한국어] Fused Operation Support 비트맵 (NVMe 1.4 §5.15.2.1).
	 *  Compare-and-Write 등 fused op 지원 여부. */

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_transport_opts) == 82, "Incorrect size");
	/* [한국어] ★ ABI 가드 ★ — 컴파일 타임 sizeof(opts) 검증. 새 필드 추가 시 의도적으로 깨져
	 *  개발자가 SET_FIELD 추가를 잊지 못하게 강제. 82 는 현재 필드들의 packed 합 (4B 정렬 포함). */

#undef SET_FIELD
#undef FILED_CHECK
	/* [한국어] 매크로 정리. SET_FIELD 는 본 함수 외부에 노출 금지. FILED_CHECK 는 (오타지만) 과거 흔적. */
}

/*
 * [한국어] struct nvmf_transport_create_ctx
 *
 * spdk_nvmf_transport_create_async() 가 비동기 create 를 시작하면서 백엔드의 create_async 콜백에
 * 넘기는 컨텍스트. 백엔드가 트랜스포트 객체 할당이 끝나면 nvmf_transport_create_async_done 을
 * 콜백으로 호출하면서 이 컨텍스트를 cb_arg 로 돌려준다.
 *
 * 라이프사이클: spdk_nvmf_transport_create_async() 진입 시 calloc → create_async 로 백엔드에 전달 →
 *              백엔드 완료 시 nvmf_transport_create_async_done 에서 cb_fn 호출 후 free.
 */
struct nvmf_transport_create_ctx {
	const struct spdk_nvmf_transport_ops *ops;
	/* [한국어] 사용 중인 백엔드의 vtable.
	 *  설정자: nvmf_transport_create() 가 nvmf_get_transport_ops() 결과로 채움.
	 *  읽는 자: _nvmf_transport_create_done() 의 ctx->ops->create() 호출, async 완료 후 transport->ops 로 복사.
	 *  값 범위: g_spdk_nvmf_transport_ops 의 한 원소. NULL 불가 (create 함수가 NULL 검사함).
	 *  동기화: ctx 는 단일 컨텍스트 — 한 thread 가 만들고 동일 thread (또는 백엔드의 완료 thread) 가 소비. */
	struct spdk_nvmf_transport_opts opts;
	/* [한국어] 검증 + 보정된 트랜스포트 옵션 사본.
	 *  설정자: nvmf_transport_create() 가 nvmf_transport_opts_copy + 정규화 (max_aq_depth/min_kato 보정).
	 *  읽는 자: ctx->ops->create(&ctx->opts) — 백엔드가 자기 트랜스포트 객체에 복사.
	 *  값 범위: 검증 통과한 합법 값 (max_io_size 2^N, kas/min_kato!=0 등).
	 *  동기화: ctx 라이프사이클 동안 R/O — 락 불필요. */
	void *cb_arg;
	/* [한국어] 사용자가 spdk_nvmf_transport_create_async() 호출 시 넘긴 opaque cb_arg.
	 *  설정자: 사용자(상위 nvmf 코드 또는 RPC 핸들러).
	 *  읽는 자: 완료 시 ctx->cb_fn(ctx->cb_arg, transport) 로 사용자에게 전달.
	 *  값 범위: 사용자 정의 포인터 (NULL 가능 — 호출자가 별도 상태 불필요 시).
	 *  동기화: ctx 라이프사이클 동안 R/O — 완료 콜백 발사 전까지 변경 없음. */
	spdk_nvmf_transport_create_done_cb cb_fn;
	/* [한국어] 사용자 완료 콜백 (typedef: void (*)(void *cb_arg, struct spdk_nvmf_transport *)).
	 *  설정자: 사용자가 spdk_nvmf_transport_create_async() 호출 시 지정.
	 *  읽는 자: nvmf_transport_create_async_done() — 성공/실패 모두 호출 (실패 시 transport=NULL).
	 *  값 범위: 유효한 함수 포인터 (NULL 불가 — 호출자 계약. NULL 이면 nvmf_transport_create_async_done 에서 segfault).
	 *  동기화: ctx 라이프사이클 동안 R/O. 콜백 자체는 create 를 호출한 spdk_thread 컨텍스트에서 실행. */
};

/*
 * [한국어]
 * nvmf_transport_use_iobuf - 이 트랜스포트가 spdk_iobuf 풀을 사용하는지 판단
 *
 * @transport: 검사 대상 트랜스포트.
 * @return: true 면 iobuf 풀 사용 (등록/해제 + per-tgroup buf_cache 관리 필요).
 *
 * num_shared_buffers 또는 buf_cache_size 중 하나라도 0 이 아니면 풀 사용으로 간주.
 * vfio_user 같은 zero-copy 백엔드는 둘 다 0 으로 설정 → false → iobuf 우회.
 *
 * 호출 체인:
 *   transport_create / destroy / poll_group_create / poll_group_destroy → [nvmf_transport_use_iobuf].
 */
static bool
nvmf_transport_use_iobuf(struct spdk_nvmf_transport *transport)
{
	return transport->opts.num_shared_buffers || transport->opts.buf_cache_size;
	/* [한국어] 둘 중 하나라도 비어있지 않으면 iobuf 모듈 등록/buf_cache 초기화 의미 있음. */
}

/*
 * [한국어]
 * nvmf_transport_create_async_done - 백엔드의 create 완료 시 본 레이어가 마무리하는 콜백
 *
 * @cb_arg: 본 레이어가 백엔드에 넘긴 nvmf_transport_create_ctx (계속 같은 컨텍스트가 돌아옴).
 * @transport: 백엔드가 할당한 spdk_nvmf_transport 객체. NULL 이면 백엔드 실패.
 *
 * 백엔드(RDMA/TCP/...)의 create / create_async 가 트랜스포트 본체를 만든 직후, 본 레이어가
 * (1) mutex 초기화 (2) listeners TAILQ 초기화 (3) ops/opts 캐시 (4) iobuf 모듈 이름 생성 +
 * 등록 을 수행. 모든 마무리가 끝나면 사용자 콜백 cb_fn 호출.
 *
 * **실패 경로**: transport==NULL 이면 곧바로 err 점프. err 에서는 백엔드 객체를 destroy 후
 * 사용자에게 NULL 통지. 단 transport 가 정상이지만 snprintf 가 실패하면(거의 불가능) 백엔드
 * destroy 호출 후 사용자 NULL 통지.
 *
 * 호출 체인:
 *   _nvmf_transport_create_done(sync) 또는 백엔드의 create_async 완료
 *     → [nvmf_transport_create_async_done] → spdk_iobuf_register_module + ctx->cb_fn → free(ctx).
 */
static void
nvmf_transport_create_async_done(void *cb_arg, struct spdk_nvmf_transport *transport)
{
	struct nvmf_transport_create_ctx *ctx = cb_arg;
	/* [한국어] cb_arg 를 본 레이어 컨텍스트로 캐스팅 — 우리가 만든 ctx 가 그대로 돌아옴. */
	int chars_written;
	/* [한국어] snprintf 결과 저장 — 음수면 출력 실패. */

	if (!transport) {
		/* [한국어] 백엔드 create 실패 — 메모리/리소스/설정 검증 등 백엔드 사정. */
		SPDK_ERRLOG("Failed to create transport.\n");
		goto err;
	}

	pthread_mutex_init(&transport->mutex, NULL);
	/* [한국어] 트랜스포트 단위 직렬화용 mutex 초기화. NVMe-oF target 의 listener add/remove,
	 *  poll_group 의 백엔드 콜백(poll_group_create/destroy/get_optimal_poll_group) 호출이 이 락으로 보호됨.
	 *  static 초기화 못 하는 이유: transport 객체가 calloc 으로 동적 할당. */
	TAILQ_INIT(&transport->listeners);
	/* [한국어] listener TAILQ 초기화. 빈 head sentinel 로 시작 → spdk_nvmf_transport_listen() 이 INSERT. */
	transport->ops = ctx->ops;
	/* [한국어] vtable 포인터 캐시 — 이후 모든 dispatcher 함수가 transport->ops->xxx 를 즉시 호출.
	 *  ctx->ops 는 이미 g_spdk_nvmf_transport_ops 의 영구 원소를 가리키므로 lifetime 안전. */
	transport->opts = ctx->opts;
	/* [한국어] 검증/보정된 opts 깊은 복사. 이후 transport->opts 가 진실의 원천. */
	chars_written = snprintf(transport->iobuf_name, MAX_MEMPOOL_NAME_LENGTH, "%s_%s", "nvmf",
				 transport->ops->name);
	/* [한국어] iobuf 모듈 등록용 이름 생성: "nvmf_<trtype>" (예: "nvmf_RDMA", "nvmf_TCP").
	 *  MAX_MEMPOOL_NAME_LENGTH 는 DPDK rte_mempool 의 이름 최대 길이 (RTE_MEMZONE_NAMESIZE-기반).
	 *  복수 트랜스포트 인스턴스가 같은 trtype 이라도 모듈 등록은 trtype 당 한 번만 — 이름 충돌 회피는
	 *  spdk_iobuf_register_module 가 idempotent 로 처리(이미 등록되어 있으면 ref 증가). */
	if (chars_written < 0) {
		/* [한국어] snprintf 실패는 거의 불가능 (포맷 문자열 + 짧은 입력). 방어적 처리. */
		SPDK_ERRLOG("Unable to generate transport data buffer pool name.\n");
		goto err;
	}

	if (nvmf_transport_use_iobuf(transport)) {
		/* [한국어] num_shared_buffers / buf_cache_size 가 설정된 경우만 iobuf 모듈 등록. */
		spdk_iobuf_register_module(transport->iobuf_name);
		/* [한국어] iobuf 시스템에 모듈 이름 등록 — 이후 spdk_iobuf_channel_init(name, ...) 에서 식별 가능.
		 *  DPDK rte_mempool 위에 small/large 이중 풀 구조. NVMe-oF 데이터 영역 zero-copy 할당의 핵심. */
	}

	ctx->cb_fn(ctx->cb_arg, transport);
	/* [한국어] 사용자에게 성공 통지 — transport 객체 노출. 사용자는 이 객체를 spdk_nvmf_tgt 에 연결. */
	free(ctx);
	/* [한국어] 컨텍스트 해제 — 더 이상 필요 없음. */
	return;
	/* [한국어] 정상 종료. */

err:
	/* [한국어] 실패 경로 — transport 가 부분적으로라도 만들어졌으면 백엔드에게 정리 위임. */
	if (transport) {
		/* [한국어] 백엔드 create 는 성공했지만 본 레이어 마무리에서 실패한 경우. */
		transport->ops->destroy(transport, NULL, NULL);
		/* [한국어] 백엔드 destroy 호출 — RDMA: ibv_destroy_pd / TCP: socket close 등. cb_fn=NULL 로 동기 destroy. */
	}

	ctx->cb_fn(ctx->cb_arg, NULL);
	/* [한국어] 사용자에게 실패 통지 — transport=NULL 로 호출자가 인식. */
	free(ctx);
	/* [한국어] 컨텍스트 해제. */
}

/*
 * [한국어]
 * _nvmf_transport_create_done - sync create 의 spdk_thread_send_msg 콜백 어댑터
 *
 * @ctx: nvmf_transport_create_ctx (void* 캐스트). spdk_thread 메시지 시스템이 void* 만 받기 때문.
 *
 * sync 모드일 때도 spdk_thread_send_msg 로 자기 자신에게 보내거나 직접 호출 — 백엔드의 create
 * 콜백을 호출 후 그 반환 transport 를 nvmf_transport_create_async_done 에 넘긴다.
 * sync vs async 의 통일된 fan-in 지점.
 *
 * 호출 체인:
 *   nvmf_transport_create(sync=true) → _nvmf_transport_create_done(직접 호출)
 *                                    또는 spdk_thread_send_msg → reactor 메시지 처리 시점
 *     → _ctx->ops->create() (백엔드 sync 할당) → nvmf_transport_create_async_done.
 */
static void
_nvmf_transport_create_done(void *ctx)
{
	struct nvmf_transport_create_ctx *_ctx = (struct nvmf_transport_create_ctx *)ctx;
	/* [한국어] void* → 본 레이어 컨텍스트 캐스팅. */

	nvmf_transport_create_async_done(_ctx, _ctx->ops->create(&_ctx->opts));
	/* [한국어] 백엔드의 sync create 호출 → 그 반환 트랜스포트 포인터를 async_done 에 넘겨 통합 처리.
	 *  sync 백엔드(대부분)도 결국 async 콜백 경로로 fan-in 되어 코드 중복 제거. */
}

/*
 * [한국어]
 * nvmf_transport_create - 트랜스포트 생성의 사설 진입점 (sync/async 통합)
 *
 * @transport_name: 백엔드 식별자 ("RDMA"/"TCP"/"FC"/"VFIOUSER" 등). 사용자가 RPC/CLI 로 입력.
 * @opts: 사용자가 채운 트랜스포트 옵션 (max_io_size, num_shared_buffers, kas 등).
 *        opts->opts_size 가 sizeof 호환성 키 — 0 이면 거부. 무조건 NULL 검사.
 * @cb_fn: 생성 완료 시 호출할 사용자 콜백 (sync 모드에서도 호출됨 — 단, _sync_done 으로 간접 호출).
 * @cb_arg: cb_fn 의 opaque 인자.
 * @sync: true 면 같은 thread 에서 즉시 _nvmf_transport_create_done() 직접 호출 (반환 시점에 transport 준비 완료).
 *        false 면 spdk_thread_send_msg 로 reactor 큐잉 → 다음 polling 사이클에 처리.
 * @return: 0 성공 / -ENOMEM ctx calloc 실패 / -1 검증 실패 또는 백엔드 create_async 실패.
 *
 * spdk_nvmf_transport_create() / _create_async() 의 공용 구현. 모든 검증과 보정을 수행한 뒤
 * 백엔드의 create (sync) 또는 create_async (async) 를 호출. sync 백엔드도 _sync_done 어댑터를 통해
 * async 완료 콜백 경로(nvmf_transport_create_async_done)로 fan-in 되어 코드 중복을 제거.
 *
 * **검증 항목**:
 *  - opts != NULL, opts->opts_size != 0 (ABI 호환성 검증의 전제조건)
 *  - 백엔드 vtable 등록 여부 (nvmf_get_transport_ops)
 *  - max_io_size 가 0 이거나 (2^N && >= 8192) — NVMe-oF 1.1 요구사항
 *  - max_aq_depth 가 NVMe spec 의 admin SQ 최소(SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE) 이상이거나 자동 보정
 *  - kas != 0, min_kato != 0 (Keep Alive 메커니즘 필수)
 *  - io_unit_size 가 iobuf large buffer size 이내
 *  - num_shared_buffers 가 iobuf 풀 capacity 이내 (warning 수준)
 *
 * 호출 체인:
 *   spdk_nvmf_transport_create / _create_async → [nvmf_transport_create]
 *     → 검증/보정 → ops->create (sync 또는 async) → nvmf_transport_create_async_done.
 */
static int
nvmf_transport_create(const char *transport_name, struct spdk_nvmf_transport_opts *opts,
		      spdk_nvmf_transport_create_done_cb cb_fn, void *cb_arg, bool sync)
{
	struct nvmf_transport_create_ctx *ctx;
	/* [한국어] sync/async 통합 컨텍스트 — calloc 으로 할당 후 ops/opts/cb_fn/cb_arg 채움. */
	struct spdk_iobuf_opts opts_iobuf = {};
	/* [한국어] iobuf 모듈의 현재 설정 (small_bufsize/large_bufsize/small_pool_count 등) — 검증용으로 조회. */
	int rc;
	/* [한국어] async create 의 백엔드 반환 코드. */
	uint64_t count;
	/* [한국어] iobuf 풀에서 사용 가능한 이론적 최대 버퍼 수 — num_shared_buffers 검증용. */
	uint32_t kas_in_ms;
	/* [한국어] kas (단위 100ms) 를 ms 로 변환한 값 — min_kato 를 kas 의 정수배로 정렬할 때 사용. */

	ctx = calloc(1, sizeof(*ctx));
	/* [한국어] async 콜백까지 살아남는 컨텍스트 할당. ZII (zero-init) 로 ops/cb_fn 등 NULL 시작. */
	if (!ctx) {
		/* [한국어] OOM — 매우 드물지만 startup 단계 메모리 압박에서 발생 가능. */
		return -ENOMEM;
	}

	if (!opts) {
		/* [한국어] 사용자 인자 NULL 검증 — RPC 핸들러는 항상 채워서 보내지만 방어적. */
		SPDK_ERRLOG("opts should not be NULL\n");
		goto err;
	}

	if (!opts->opts_size) {
		/* [한국어] opts_size==0 → ABI 검증 불가. SPDK 사용자가 빈 구조체를 던졌거나 init 안 한 경우. */
		SPDK_ERRLOG("The opts_size in opts structure should not be zero\n");
		goto err;
	}

	ctx->ops = nvmf_get_transport_ops(transport_name);
	/* [한국어] 백엔드 vtable 조회 — 미등록 백엔드면 NULL. RDMA 빌드 안 된 환경에서 "RDMA" 요청 등. */
	if (!ctx->ops) {
		SPDK_ERRLOG("Transport type '%s' unavailable.\n", transport_name);
		goto err;
	}

	nvmf_transport_opts_copy(&ctx->opts, opts, opts->opts_size);
	/* [한국어] 사용자 opts → ctx->opts 깊은 복사 (ABI 호환 SET_FIELD). 이후 ctx->opts 가 진실의 원천. */
	if (ctx->opts.max_io_size != 0 && (!spdk_u32_is_pow2(ctx->opts.max_io_size) ||
					   ctx->opts.max_io_size < 8192)) {
		/* [한국어] max_io_size 검증: 0(=백엔드 기본 사용) 이 아니면 2^N && >= 8KB.
		 *  NVMe-oF 1.1 §3.5.1 요구사항 + DIF/zcopy 정렬 요구사항. 8KB 미만은 fragmentation 비용 과다. */
		SPDK_ERRLOG("max_io_size %u must be a power of 2 and be greater than or equal 8KB\n",
			    ctx->opts.max_io_size);
		goto err;
	}

	if (ctx->opts.max_aq_depth < SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE) {
		/* [한국어] Admin Queue 깊이가 NVMe spec 최소(보통 32) 미만 — 자동 보정 (사용자 알림 후 진행). */
		SPDK_ERRLOG("max_aq_depth %u is less than minimum defined by NVMf spec, use min value\n",
			    ctx->opts.max_aq_depth);
		ctx->opts.max_aq_depth = SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE;
		/* [한국어] 강제 보정 — fail 시키지 않고 안전한 최소값으로. */
	}

	if (ctx->opts.kas == 0) {
		/* [한국어] Keep Alive Support time unit 0 — KA 메커니즘 비활성화 의미지만 NVMe-oF 1.1 §5.21 에서 0 금지. */
		SPDK_ERRLOG("kas cannot be 0\n");
		goto err;
	}

	if (ctx->opts.min_kato == 0) {
		/* [한국어] 최소 Keep Alive Timeout 0 — 호스트가 임의의 짧은 KATO 줄 수 있게 되어 controller 가 과도한 disconnect.
		 *  안전상 0 거부. */
		SPDK_ERRLOG("min_kato cannot be 0\n");
		goto err;
	}

	kas_in_ms = ctx->opts.kas * NVMF_KAS_TIME_UNIT_IN_MS;
	/* [한국어] kas (100ms 단위) → ms 변환. NVMF_KAS_TIME_UNIT_IN_MS 는 보통 100. 예: kas=10 → 1000ms. */
	ctx->opts.min_kato = spdk_round_up(ctx->opts.min_kato, kas_in_ms);
	/* [한국어] min_kato 를 kas 단위로 round-up — 호스트의 KATO 가 kas 단위가 아니면 controller 가 어차피 보정.
	 *  미리 정렬하여 일관성 확보. 예: kas_in_ms=1000, min_kato=1500 → 2000. */

	spdk_iobuf_get_opts(&opts_iobuf, sizeof(opts_iobuf));
	/* [한국어] 글로벌 iobuf 모듈의 현재 설정 조회 (small/large bufsize 와 pool count). 검증용. */
	if (ctx->opts.io_unit_size == 0) {
		/* [한국어] io_unit_size 0 — buffer cache 가 IOV 단위로 할당 못 함. 필수 0 거부. */
		SPDK_ERRLOG("io_unit_size cannot be 0\n");
		goto err;
	}
	if (ctx->opts.io_unit_size > opts_iobuf.large_bufsize) {
		/* [한국어] io_unit_size 가 iobuf 의 large buffer 보다 크면 풀에서 받을 buffer 가 없음 → 필 fail.
		 *  사용자는 spdk_iobuf_set_opts 로 large_bufsize 를 키우거나 io_unit_size 를 줄여야 함. */
		SPDK_ERRLOG("io_unit_size %u is larger than iobuf pool large buffer size %d\n",
			    ctx->opts.io_unit_size, opts_iobuf.large_bufsize);
		goto err;
	}

	if (ctx->opts.io_unit_size <= opts_iobuf.small_bufsize) {
		/* We'll be using the small buffer pool only */
		/* [한국어] io_unit_size 가 small_bufsize 이하면 small 풀만 사용 → 가용 capacity 는 small_pool_count. */
		count = opts_iobuf.small_pool_count;
	} else {
		/* [한국어] 그렇지 않으면 large 풀이 주(데이터) + small 풀이 보조 → 둘의 min 이 실효 capacity. */
		count = spdk_min(opts_iobuf.small_pool_count, opts_iobuf.large_pool_count);
	}

	if (ctx->opts.num_shared_buffers > count) {
		/* [한국어] 사용자가 요청한 공유 버퍼 수가 풀 capacity 초과 — fail 안 시키고 warning.
		 *  실제 할당은 buf_cache_size 기반이라 부족하더라도 가용한 만큼만 캐시. */
		SPDK_WARNLOG("The num_shared_buffers value (%u) is larger than the available iobuf"
			     " pool size (%lu). Please increase the iobuf pool sizes.\n",
			     ctx->opts.num_shared_buffers, count);
	}

	ctx->cb_fn = cb_fn;
	/* [한국어] async 완료 콜백 저장 — nvmf_transport_create_async_done 이 사용. */
	ctx->cb_arg = cb_arg;
	/* [한국어] cb_fn 의 opaque 인자. */

	/* Prioritize sync create operation. */
	if (ctx->ops->create) {
		/* [한국어] 백엔드가 sync create 콜백을 제공하는 경우 우선. RDMA/TCP 등 대부분 sync. */
		if (sync) {
			/* [한국어] 호출자가 sync 모드 요청 — 직접 호출 (현재 thread 컨텍스트에서 즉시 완료). */
			_nvmf_transport_create_done(ctx);
			return 0;
		}

		spdk_thread_send_msg(spdk_get_thread(), _nvmf_transport_create_done, ctx);
		/* [한국어] async 모드 — spdk_thread 메시지 큐에 self-post. 다음 polling 사이클에 _sync_done 호출.
		 *  현재 stack 에서 즉시 콜백을 호출하면 호출자가 stack unwind 전에 cb 가 실행되어 위험 (재진입).
		 *  스레드 메시지로 deferred 처리하여 호출자 후처리 후 안전하게 콜백 실행. */
		return 0;
	}

	assert(ctx->ops->create_async);
	/* [한국어] sync create 가 없으면 반드시 create_async 가 있어야 함 (vtable 계약). vfio_user 등이 사용. */
	rc = ctx->ops->create_async(&ctx->opts, nvmf_transport_create_async_done, ctx);
	/* [한국어] 백엔드의 비동기 create 호출 — 백엔드가 알아서 자기 thread 에서 작업 후 완료 시 콜백.
	 *  완료 콜백은 본 레이어의 nvmf_transport_create_async_done — sync/async 통합 fan-in. */
	if (rc) {
		/* [한국어] 백엔드 create_async 시작조차 실패 — 메모리 부족 등. ctx free 후 -1 반환. */
		SPDK_ERRLOG("Unable to create new transport of type %s\n", transport_name);
		goto err;
	}

	return 0;
	/* [한국어] async create 정상 시작 — 완료는 백엔드 콜백에서 처리. */
err:
	/* [한국어] 검증 실패/할당 실패 통합 정리 경로. */
	free(ctx);
	/* [한국어] 컨텍스트 해제 — async create 가 시작되지 않았으므로 백엔드는 모름. */
	return -1;
}

/*
 * [한국어]
 * spdk_nvmf_transport_create_async - 비동기 트랜스포트 생성의 외부 API
 *
 * @transport_name: 백엔드 식별자 ("RDMA"/"TCP"/...).
 * @opts: 사용자 옵션. opts_size 검증 후 ctx->opts 로 복사.
 * @cb_fn: 완료 콜백 — 성공/실패 모두 호출. 성공 시 transport!=NULL.
 * @cb_arg: cb_fn 의 opaque 인자.
 * @return: 0 성공 시작 (콜백에서 결과 전달) / -ENOMEM / -1 검증 실패.
 *
 * 외부 사용자(보통 nvmf target 의 add_transport RPC)에게 노출되는 비동기 API. 내부적으로
 * nvmf_transport_create(sync=false) 로 위임. async 의 의미는 "create 함수가 즉시 반환하고
 * 완료는 별도 콜백에서 통지" — sync 백엔드(create)도 spdk_thread_send_msg 로 deferred 실행.
 *
 * 호출 체인:
 *   사용자/RPC 핸들러 → [spdk_nvmf_transport_create_async] → nvmf_transport_create(sync=false)
 *     → spdk_thread_send_msg → 다음 polling 사이클 → _nvmf_transport_create_done → cb_fn.
 */
int
spdk_nvmf_transport_create_async(const char *transport_name, struct spdk_nvmf_transport_opts *opts,
				 spdk_nvmf_transport_create_done_cb cb_fn, void *cb_arg)
{
	return nvmf_transport_create(transport_name, opts, cb_fn, cb_arg, false);
	/* [한국어] sync=false 로 위임. 본 레이어 통합 함수가 모든 검증/스케줄링 처리. */
}

/*
 * [한국어]
 * nvmf_transport_create_sync_done - sync 모드의 사용자 콜백 어댑터 (출력 포인터 전달)
 *
 * @cb_arg: 사용자가 spdk_nvmf_transport_create() 안에서 던진 struct spdk_nvmf_transport ** (출력 위치).
 * @transport: 생성된 트랜스포트 객체 (성공 시) 또는 NULL (실패 시).
 *
 * spdk_nvmf_transport_create() 의 sync API 구현 트릭 — 내부 통합 함수는 cb_fn 기반으로 결과를
 * 전달하므로, sync 모드에서는 여기에서 결과를 외부 변수에 직접 저장하여 호출자가 cb 없이 받을 수 있게 함.
 *
 * 호출 체인:
 *   nvmf_transport_create(sync=true) → _nvmf_transport_create_done → nvmf_transport_create_async_done
 *     → ctx->cb_fn = nvmf_transport_create_sync_done → 외부 *_transport 변수 채움.
 */
static void
nvmf_transport_create_sync_done(void *cb_arg, struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_transport **_transport = cb_arg;
	/* [한국어] 호출자가 던진 출력 포인터의 주소 — 결과를 여기에 저장. */

	*_transport = transport;
	/* [한국어] sync 호출자가 받을 트랜스포트 포인터 셋. NULL 이면 실패 통지. */
}

/*
 * [한국어]
 * spdk_nvmf_transport_create - 동기 트랜스포트 생성의 외부 API
 *
 * @transport_name: 백엔드 식별자.
 * @opts: 사용자 옵션 (opts_size 필수).
 * @return: 생성된 트랜스포트 포인터 (성공) 또는 NULL (실패).
 *
 * 함수가 반환되기 전에 트랜스포트 객체가 완전히 초기화되어 있어야 한다는 보장. 내부적으로는
 * 통합 함수에 sync=true 로 호출하고, 결과를 sync_done 어댑터를 통해 로컬 변수에 받음.
 *
 * **제약**: 백엔드의 sync create 콜백이 반드시 존재해야 함 (assert). vfio_user 처럼 async 만 있는
 * 백엔드는 spdk_nvmf_transport_create_async 를 써야 함.
 *
 * 호출 체인:
 *   사용자 코드 / RPC create_transport → [spdk_nvmf_transport_create]
 *     → nvmf_transport_create(sync=true) → 백엔드 ops->create 즉시 호출 → transport 즉시 반환.
 */
struct spdk_nvmf_transport *
spdk_nvmf_transport_create(const char *transport_name, struct spdk_nvmf_transport_opts *opts)
{
	struct spdk_nvmf_transport *transport = NULL;
	/* [한국어] 출력 포인터 — sync_done 어댑터가 채워줌. 실패 시 NULL 유지. */

	/* Current implementation supports synchronous version of create operation only. */
	assert(nvmf_get_transport_ops(transport_name) && nvmf_get_transport_ops(transport_name)->create);
	/* [한국어] 호출자 책임: 백엔드 등록 + sync create 존재. async-only 백엔드(vfio_user 등)는 거부.
	 *  debug 빌드에서 즉시 abort, release 빌드에선 통합 함수가 알아서 fail. */

	nvmf_transport_create(transport_name, opts, nvmf_transport_create_sync_done, &transport, true);
	/* [한국어] sync=true 로 통합 함수 호출 — _sync_done 어댑터가 transport 변수를 채움. */
	return transport;
	/* [한국어] 어댑터가 채운 트랜스포트 포인터 반환 (실패 시 NULL). */
}

/*
 * [한국어]
 * spdk_nvmf_transport_get_first - 타겟에 바인딩된 첫 트랜스포트 조회 (반복자 시작점)
 *
 * @tgt: 대상 nvmf 타겟 (subsystem/transport 컨테이너).
 * @return: 첫 트랜스포트 포인터 또는 NULL (없음).
 *
 * 사용자가 한 nvmf target 에 등록된 모든 트랜스포트를 열거할 때 사용. tgt->transports 는
 * spdk_nvmf_tgt_add_transport 가 INSERT 한 TAILQ. 순서는 추가 순.
 */
struct spdk_nvmf_transport *
spdk_nvmf_transport_get_first(struct spdk_nvmf_tgt *tgt)
{
	return TAILQ_FIRST(&tgt->transports);
	/* [한국어] TAILQ head 의 첫 원소 반환. tgt 가 빈 transport 리스트면 NULL. */
}

/*
 * [한국어]
 * spdk_nvmf_transport_get_next - 다음 트랜스포트 조회 (반복자 진행)
 *
 * @transport: 현재 트랜스포트.
 * @return: 다음 트랜스포트 포인터 또는 NULL (마지막).
 *
 * for(t = get_first(tgt); t; t = get_next(t)) 패턴으로 사용. RPC nvmf_get_transports 등이 활용.
 */
struct spdk_nvmf_transport *
spdk_nvmf_transport_get_next(struct spdk_nvmf_transport *transport)
{
	return TAILQ_NEXT(transport, link);
	/* [한국어] transport->link 다음 원소 반환. 마지막이면 NULL. */
}

/*
 * [한국어]
 * spdk_nvmf_transport_destroy - 트랜스포트 + 모든 리스너 + iobuf 모듈 정리
 *
 * @transport: 정리할 트랜스포트.
 * @cb_fn: 백엔드 destroy 완료 시 호출될 사용자 콜백 (백엔드가 async 일 때 의미).
 * @cb_arg: cb_fn 의 opaque 인자.
 * @return: 항상 0 (현재 실패 경로 없음).
 *
 * 트랜스포트가 더 이상 필요 없을 때 (target shutdown 또는 RPC remove_transport) 호출. 동작 순서:
 *  1) 모든 listener 강제 정리 — ref count 무시 (target shutdown 가정).
 *  2) iobuf 모듈 unregister (사용 시).
 *  3) transport->mutex destroy.
 *  4) 백엔드 destroy 위임 — cb_fn 으로 비동기 완료 통지.
 *
 * **주의**: cb_fn 호출 시점은 백엔드 마음대로. RDMA 처럼 in-flight WR 정리에 시간이 걸리면 async.
 *
 * 호출 체인:
 *   spdk_nvmf_tgt_destroy / RPC remove_transport → [spdk_nvmf_transport_destroy]
 *     → ops->stop_listen (per-listener) → spdk_iobuf_unregister_module → ops->destroy(cb_fn).
 */
int
spdk_nvmf_transport_destroy(struct spdk_nvmf_transport *transport,
			    spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_listener *listener, *listener_tmp;
	/* [한국어] FOREACH_SAFE 용 — 순회 중 REMOVE 안전하게 하기 위한 next caching. */

	TAILQ_FOREACH_SAFE(listener, &transport->listeners, link, listener_tmp) {
		/* [한국어] 모든 listener 순회하며 강제 종료 — ref count 검증 없이 일괄 cleanup. */
		TAILQ_REMOVE(&transport->listeners, listener, link);
		/* [한국어] TAILQ 에서 제거 — 다음 함수가 listener->trid 만 사용하므로 미리 unlink 가능. */
		transport->ops->stop_listen(transport, &listener->trid);
		/* [한국어] 백엔드 stop_listen 호출 — 실제 socket close, RDMA listen ID 제거 등. */
		free(listener);
		/* [한국어] listener 객체 free — ref count 무시 (강제 cleanup). */
	}

	if (nvmf_transport_use_iobuf(transport)) {
		/* [한국어] iobuf 풀 사용했던 경우만 unregister — vfio_user zcopy 등은 skip. */
		spdk_iobuf_unregister_module(transport->iobuf_name);
		/* [한국어] iobuf 모듈 등록 해제 — ref count 기반이라 마지막 unregister 만 실제 풀 free. */
	}

	pthread_mutex_destroy(&transport->mutex);
	/* [한국어] 트랜스포트 mutex 정리 — 이 시점 이후 어떤 thread 도 mutex 사용 안 한다고 가정. */
	transport->ops->destroy(transport, cb_fn, cb_arg);
	/* [한국어] 백엔드 destroy 위임 — 백엔드가 자기 객체 free + 완료 시 cb_fn(cb_arg) 호출. */
	return 0;
	/* [한국어] 항상 0. 백엔드 destroy 의 실제 결과는 cb_fn 으로만 통지. */
}

/*
 * [한국어]
 * nvmf_transport_find_listener - trid 매칭으로 등록된 리스너 조회
 *
 * @transport: 검색 대상 트랜스포트.
 * @trid: 매칭 키 — trtype/adrfam/traddr/trsvcid 4 필드 모두 일치해야 함.
 * @return: 매칭된 listener 포인터 또는 NULL.
 *
 * spdk_nvmf_transport_listen() / _stop_listen() 이 동일 trid 의 기존 listener 가 있는지 검사.
 * 동일 trid 면 ref count 만 증가 (다중 subsystem 이 같은 IP:port 공유). 처음 보는 trid 면 새 listener 생성.
 *
 * 호출 체인:
 *   spdk_nvmf_transport_listen / _stop_listen → [nvmf_transport_find_listener]
 *     → spdk_nvme_transport_id_compare 로 4-tuple 비교 → 매칭 시 listener 반환.
 */
struct spdk_nvmf_listener *
nvmf_transport_find_listener(struct spdk_nvmf_transport *transport,
			     const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_listener *listener;
	/* [한국어] 순회용 임시 포인터. */

	TAILQ_FOREACH(listener, &transport->listeners, link) {
		/* [한국어] transport 의 listener 리스트 head→tail 순회. lock 없이 순회 — 호출자가 transport->mutex 보유 가정. */
		if (spdk_nvme_transport_id_compare(&listener->trid, trid) == 0) {
			/* [한국어] 4-tuple 동일성 비교 (trtype, adrfam, traddr, trsvcid). 0 이면 match. */
			return listener;
		}
	}

	return NULL;
	/* [한국어] 매칭 없음 — 새 listener 생성 필요. */
}

/*
 * [한국어]
 * spdk_nvmf_transport_listen - 트랜스포트에 listener 등록 (ref count 기반)
 *
 * @transport: 대상 트랜스포트.
 * @trid: listener 식별 — trtype/adrfam/traddr/trsvcid.
 * @opts: listener 옵션 (sock_impl 등). sock_impl 이 매칭되어야 ref 증가 허용.
 * @return: 0 성공 (ref 증가 또는 새 listener 생성) / -ENOMEM / -EINVAL (sock_impl 불일치) / 백엔드 listen 실패.
 *
 * NVMe-oF 의 한 IP:port 는 여러 subsystem 에 의해 공유 가능 (multi-subsystem listener). 그래서:
 *  - 첫 호출 (find_listener=NULL): 새 listener 생성 + 백엔드 listen 호출 (socket bind, RDMA listen 등) + ref=1.
 *  - 이후 호출 (find_listener!=NULL): ref++ — 같은 socket/RDMA listener 재사용.
 *
 * **동기화**: transport->mutex 로 ops->listen 호출 보호 — 백엔드가 자기 자료구조를 수정하기 때문.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_add_listener (subsystem.c) → [spdk_nvmf_transport_listen]
 *     → nvmf_transport_find_listener → 신규/기존 분기 → ops->listen (신규 시) → ref++.
 */
int
spdk_nvmf_transport_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid, struct spdk_nvmf_listen_opts *opts)
{
	struct spdk_nvmf_listener *listener;
	/* [한국어] 기존 listener 또는 신규 listener 포인터. */
	int rc;
	/* [한국어] 백엔드 listen 반환 코드 — bind 실패/주소 점유 등. */

	listener = nvmf_transport_find_listener(transport, trid);
	/* [한국어] 동일 trid 의 기존 listener 검색. */
	if (!listener) {
		/* [한국어] 처음 보는 trid → 새 listener 생성 경로. */
		listener = calloc(1, sizeof(*listener));
		/* [한국어] listener 구조체 할당 (trid, ref, sock_impl, link 필드 0 초기화). */
		if (!listener) {
			/* [한국어] OOM — 즉시 fail. */
			return -ENOMEM;
		}

		listener->ref = 1;
		/* [한국어] 첫 등록 → ref count 1 시작. 추가 _listen 호출마다 ++, _stop_listen 호출마다 --. */
		listener->trid = *trid;
		/* [한국어] trid 깊은 복사 — 호출자 trid 가 stack/임시 객체일 수 있음. 이후 listener 가 진실의 원천. */
		listener->sock_impl = opts->sock_impl;
		/* [한국어] socket 구현체 이름 ("posix"/"uring"/"vpp" 등 — TCP 트랜스포트만 의미).
		 *  포인터만 저장 (호출자가 string lifetime 보장). 다른 listener 추가 호출의 검증 키로도 사용. */
		TAILQ_INSERT_TAIL(&transport->listeners, listener, link);
		/* [한국어] 트랜스포트 listener 리스트에 추가 — TAIL 삽입 (FIFO 순서). */
		pthread_mutex_lock(&transport->mutex);
		/* [한국어] 백엔드 listen 호출 직전 락 — 동시 listen/stop_listen 직렬화 + 백엔드 자료구조 보호. */
		rc = transport->ops->listen(transport, &listener->trid, opts);
		/* [한국어] 백엔드 listen 콜백 — TCP: socket(SOCK_STREAM)+bind+listen / RDMA: rdma_create_id+rdma_bind_addr+rdma_listen.
		 *  실패 시 EADDRINUSE (포트 점유) / EACCES (권한) / 등. */
		pthread_mutex_unlock(&transport->mutex);
		/* [한국어] 락 해제. */
		if (rc != 0) {
			/* [한국어] 백엔드 listen 실패 → listener 객체 정리 후 에러 반환. */
			TAILQ_REMOVE(&transport->listeners, listener, link);
			/* [한국어] 리스트에서 제거. */
			free(listener);
			/* [한국어] 객체 free. */
		}
		return rc;
		/* [한국어] 0 또는 백엔드 에러 코드 반환. */
	}

	if (opts->sock_impl && strncmp(opts->sock_impl, listener->sock_impl, strlen(listener->sock_impl))) {
		/* [한국어] 기존 listener 가 있지만 sock_impl 이 다르면 에러 — 같은 IP:port 를 다른 socket impl 로 재사용 불가.
		 *  strncmp(prefix=existing length) — 새 요청이 기존을 포함해도 OK 로 처리되는 prefix-match. */
		SPDK_ERRLOG("opts->sock_impl: '%s' doesn't match listener->sock_impl: '%s'\n", opts->sock_impl,
			    listener->sock_impl);
		return -EINVAL;
	}

	++listener->ref;
	/* [한국어] 기존 listener 재사용 → ref count 증가. 다중 subsystem 공유. */

	return 0;
	/* [한국어] 정상 — listener 가 이미 활성화되어 있음. */
}

/*
 * [한국어]
 * spdk_nvmf_transport_stop_listen - 트랜스포트의 listener 해제 (ref count 감소, 0 시 실제 정리)
 *
 * @transport: 대상 트랜스포트.
 * @trid: 해제할 listener 식별 trid.
 * @return: 0 성공 / -ENOENT (해당 trid 의 listener 없음).
 *
 * subsystem 이 listener 를 떼어낼 때 호출. ref count > 0 이면 다른 subsystem 이 같은 listener 를
 * 사용 중이므로 실제 socket/RDMA close 는 안 함. ref==0 이 되면 본격 정리:
 *  1) TAILQ 에서 제거.
 *  2) 백엔드 stop_listen 호출 (transport->mutex 보호).
 *  3) ★ dangling pointer cleanup ★ — listener->trid 를 가리키던 모든 subsystem_listener->trid 를 NULL 로.
 *     (subsystem_listener 는 여기서 free 안 함 — 별도 경로에서 정리. 하지만 trid 포인터는 무효해짐.)
 *  4) listener free.
 *
 * 호출 체인:
 *   subsystem destroy 또는 RPC remove_listener → [spdk_nvmf_transport_stop_listen]
 *     → ref-- → ref==0 시 ops->stop_listen + subsystem_listener trid pointer cleanup + free.
 */
int
spdk_nvmf_transport_stop_listen(struct spdk_nvmf_transport *transport,
				const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_subsystem_listener *subsystem_listener;
	/* [한국어] subsystem 의 listener 슬롯 — listener->trid 포인터를 들고 있을 수 있음 (dangling 검사 대상). */
	struct spdk_nvmf_subsystem *subsystem;
	/* [한국어] subsystem 순회용. */
	struct spdk_nvmf_listener *listener;
	/* [한국어] 해제 대상 listener. */

	listener = nvmf_transport_find_listener(transport, trid);
	/* [한국어] 매칭 listener 검색 — 없으면 -ENOENT (호출자 버그). */
	if (!listener) {
		return -ENOENT;
	}

	if (--listener->ref == 0) {
		/* [한국어] ref count 감소 후 0 이면 마지막 사용자 → 실제 정리. 그 전에는 다른 subsystem 이 사용 중이므로 보존. */
		TAILQ_REMOVE(&transport->listeners, listener, link);
		/* [한국어] 트랜스포트의 listener 리스트에서 제거. 이후 새 _listen 호출은 새 listener 생성으로 이어짐. */
		pthread_mutex_lock(&transport->mutex);
		/* [한국어] 백엔드 stop_listen 호출 직전 락 — listen/stop_listen 직렬화. */
		transport->ops->stop_listen(transport, trid);
		/* [한국어] 백엔드 stop_listen — TCP: close(socket) / RDMA: rdma_destroy_id / FC: HBA detach. */
		pthread_mutex_unlock(&transport->mutex);
		/* [한국어] 락 해제. */

		/* The transport listener has stopped and we are about to free trid; clear dangling pointers. */
		/* [한국어] ★ dangling pointer 청소 ★ — subsystem_listener 들이 listener->trid 의 주소를 가지고 있으면
		 *  곧 free 될 메모리를 가리키게 됨. 모든 subsystem 의 모든 listener 슬롯을 순회하며 매칭 시 NULL 화.
		 *  (subsystem_listener 자체는 다른 경로에서 살아있을 수 있음 — trid 포인터만 무효화.) */
		for (subsystem = spdk_nvmf_subsystem_get_first(transport->tgt); subsystem != NULL;
		     subsystem = spdk_nvmf_subsystem_get_next(subsystem)) {
			/* [한국어] target 의 모든 subsystem 순회. */
			TAILQ_FOREACH(subsystem_listener, &subsystem->listeners, link) {
				/* [한국어] 각 subsystem 의 listener 슬롯 순회. */
				if (subsystem_listener->trid == &listener->trid) {
					/* [한국어] 포인터 동일성(주소 비교) 검사 — listener->trid 의 주소를 가리키던 슬롯 식별.
					 *  spdk_nvme_transport_id_compare 가 아니라 포인터 자체 비교 — 메모리 정리 의미. */
					subsystem_listener->trid = NULL;
					/* [한국어] dangling 방지 NULL 화. 이후 사용자는 NULL 검사 후 다른 listener 재할당 또는 슬롯 정리. */
				}
			}
		}

		free(listener);
		/* [한국어] listener 객체 free — 이제 누구도 그 메모리를 참조하지 않음. */
	}

	return 0;
	/* [한국어] 정상 — ref-- 만 했거나 실제 정리 완료. */
}

/*
 * [한국어] struct nvmf_stop_listen_ctx
 *
 * spdk_nvmf_transport_stop_listen_async() 가 spdk_for_each_channel 로 모든 poll_group 을 순회하며
 * 매칭 qpair 를 disconnect 시킬 때 사용하는 컨텍스트. 라이프사이클: stop_listen_async 진입에서 calloc →
 * 모든 channel 순회 완료 후 nvmf_stop_listen_fini 에서 free.
 */
struct nvmf_stop_listen_ctx {
	struct spdk_nvmf_transport *transport;
	/* [한국어] 대상 트랜스포트.
	 *  설정자: spdk_nvmf_transport_stop_listen_async() 진입 시.
	 *  읽는 자: nvmf_stop_listen_fini 가 spdk_nvmf_transport_stop_listen() 에 사용.
	 *  값 범위: 유효한 트랜스포트 (NULL 불가, fini 에서 assert).
	 *  동기화: ctx 라이프사이클 동안 R/O. */
	struct spdk_nvme_transport_id trid;
	/* [한국어] 매칭 키 trid 의 깊은 복사본 (호출자 stack 객체일 수 있어 복사).
	 *  설정자: stop_listen_async 진입 시 *trid 복사.
	 *  읽는 자: disconnect_qpairs 에서 spdk_nvme_transport_id_compare 로 매칭, fini 에서 stop_listen 호출.
	 *  값 범위: 합법 trid (subnqn=='\0' 필수 — 진입 시 검증).
	 *  동기화: R/O. */
	struct spdk_nvmf_subsystem *subsystem;
	/* [한국어] 추가 매칭 조건: 해당 subsystem 의 qpair 만 disconnect. NULL 이면 모든 매칭 qpair.
	 *  설정자: 호출자가 지정 (subsystem 단위 listener 제거 시 해당 subsystem만, 트랜스포트 단위 정리 시 NULL).
	 *  읽는 자: disconnect_qpairs 에서 qpair->ctrlr->subsys 와 비교.
	 *  값 범위: NULL 또는 유효한 subsystem 포인터.
	 *  동기화: R/O. */
	spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn;
	/* [한국어] 모든 channel 순회 + stop_listen 완료 후 호출할 사용자 콜백
	 *  (typedef: void (*)(void *cb_arg, int status)).
	 *  설정자: 사용자가 spdk_nvmf_transport_stop_listen_async() 호출 시 지정.
	 *  읽는 자: nvmf_stop_listen_fini — spdk_nvmf_transport_stop_listen() 결과(rc)와 함께 호출.
	 *  값 범위: 유효한 함수 포인터 또는 NULL (NULL 이면 fini 에서 if(ctx->cb_fn) 가드로 skip).
	 *  동기화: ctx 라이프사이클 동안 R/O. 콜백은 fini 가 실행되는 reactor thread 에서 호출. */
	void *cb_arg;
	/* [한국어] cb_fn 의 opaque 인자.
	 *  설정자: 사용자가 stop_listen_async() 호출 시 지정.
	 *  읽는 자: nvmf_stop_listen_fini 에서 ctx->cb_fn(ctx->cb_arg, rc) 로 사용자에게 전달.
	 *  값 범위: 사용자 정의 포인터 (NULL 가능 — cb_fn 이 NULL 이면 cb_arg 도 미사용).
	 *  동기화: ctx 라이프사이클 동안 R/O. */
};

/*
 * [한국어]
 * nvmf_stop_listen_fini - spdk_for_each_channel 의 finalization 콜백 — 모든 channel 순회 완료 후 실행
 *
 * @i: channel iterator (ctx 추출용).
 * @status: 순회 중 발생한 상태 코드 (현재 사용 안 함).
 *
 * 모든 reactor 의 poll_group channel 을 순회하며 매칭 qpair 들을 disconnect 시킨 후 호출됨.
 * 이 시점에는 매칭 qpair 들이 disconnect 진행 중 — 본 함수는 listener 자체를 stop_listen 으로 정리하고
 * 사용자에게 완료 통지.
 *
 * 호출 체인:
 *   spdk_nvmf_transport_stop_listen_async → spdk_for_each_channel(disconnect_qpairs, fini)
 *     → 모든 channel 의 disconnect_qpairs 호출 후 → [nvmf_stop_listen_fini]
 *     → spdk_nvmf_transport_stop_listen → ctx->cb_fn(cb_arg, rc) → free(ctx).
 */
static void
nvmf_stop_listen_fini(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_stop_listen_ctx *ctx;
	/* [한국어] iterator 에서 추출할 컨텍스트. */
	struct spdk_nvmf_transport *transport;
	/* [한국어] ctx 에서 캐싱한 트랜스포트. */
	int rc = status;
	/* [한국어] 결과 코드 — 일단 iterator status 로 시작, stop_listen 결과로 덮어씀. */

	ctx = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] iterator 가 들고 있던 사용자 ctx 추출. */
	transport = ctx->transport;
	/* [한국어] 트랜스포트 캐싱. */
	assert(transport != NULL);
	/* [한국어] 트랜스포트는 항상 유효 — debug 빌드에서 NULL 이면 abort. */

	rc = spdk_nvmf_transport_stop_listen(transport, &ctx->trid);
	/* [한국어] 모든 매칭 qpair 가 disconnect 진행 중인 상태에서 listener 자체를 정리 (ref-- 로 0 시 stop_listen).
	 *  qpair disconnect 와 listener stop_listen 의 순서가 중요 — listener 먼저 막아야 새 connect 안 들어옴.
	 *  하지만 spdk_for_each_channel 의 fini 는 disconnect 콜백 발급 후이므로 거의 동시. */
	if (rc) {
		/* [한국어] -ENOENT 등 — 이미 다른 경로에서 listener 정리됐거나 trid 매칭 안 됨. */
		SPDK_ERRLOG("Failed to stop listening on address '%s'\n", ctx->trid.traddr);
	}

	if (ctx->cb_fn) {
		/* [한국어] 사용자 콜백이 등록되어 있으면 결과 통지. */
		ctx->cb_fn(ctx->cb_arg, rc);
	}
	free(ctx);
	/* [한국어] 컨텍스트 해제 — async 작업 완료. */
}

/*
 * [한국어]
 * nvmf_stop_listen_disconnect_qpairs - 한 reactor 의 poll_group 내 매칭 qpair 들을 disconnect (per-channel 콜백)
 *
 * @i: channel iterator.
 *
 * spdk_for_each_channel 이 각 reactor 의 채널(=poll_group) 컨텍스트로 호출하는 콜백. 해당 reactor 의
 * 모든 qpair 를 순회하며:
 *  1) qpair 의 listen trid 추출 (qpair_get_listen_trid).
 *  2) ctx->trid 와 동일하면 (= 같은 listener 에 붙어있는 qpair) 추가 검사.
 *  3) ctx->subsystem 이 NULL 이거나, subsystem 이 해당 qpair 의 controller subsys 와 일치하면 disconnect.
 *
 * 즉, "이 listener 를 통해 들어온 qpair 만, 그리고 옵션으로 특정 subsystem 의 qpair 만" disconnect.
 *
 * 호출 체인:
 *   spdk_nvmf_transport_stop_listen_async → spdk_for_each_channel
 *     → [nvmf_stop_listen_disconnect_qpairs] (per reactor)
 *     → spdk_nvmf_qpair_disconnect (매칭 qpair 들) → spdk_for_each_channel_continue.
 */
static void
nvmf_stop_listen_disconnect_qpairs(struct spdk_io_channel_iter *i)
{
	struct nvmf_stop_listen_ctx *ctx;
	/* [한국어] iterator 컨텍스트 (transport, trid, subsystem, cb 정보). */
	struct spdk_nvmf_poll_group *group;
	/* [한국어] 현재 reactor 의 nvmf poll_group (per-reactor 객체). */
	struct spdk_io_channel *ch;
	/* [한국어] 현재 채널 핸들 — get_ctx 로 group 추출. */
	struct spdk_nvmf_qpair *qpair, *tmp_qpair;
	/* [한국어] FOREACH_SAFE 용 — disconnect 가 qpair 를 리스트에서 제거할 수 있어 next 캐싱 필요. */
	struct spdk_nvme_transport_id tmp_trid;
	/* [한국어] qpair 의 listen trid 추출용 임시 버퍼. */

	ctx = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] iterator 의 사용자 ctx. */
	ch = spdk_io_channel_iter_get_channel(i);
	/* [한국어] 현재 reactor 의 channel. */
	group = spdk_io_channel_get_ctx(ch);
	/* [한국어] channel 의 ctx — nvmf poll_group 객체. spdk_nvmf_poll_group_create 가 io_device 등록 시 결정. */

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, tmp_qpair) {
		/* [한국어] poll_group 의 모든 qpair 순회. SAFE 변종으로 disconnect 중 list 변경 안전. */
		if (spdk_nvmf_qpair_get_listen_trid(qpair, &tmp_trid)) {
			/* [한국어] qpair 의 listen trid 추출 실패 → skip (백엔드가 trid 모르는 경우 등). */
			continue;
		}

		/* Skip qpairs that don't match the listen trid and subsystem pointer.  If
		 * the ctx->subsystem is NULL, it means disconnect all qpairs that match
		 * the listen trid. */
		if (!spdk_nvme_transport_id_compare(&ctx->trid, &tmp_trid)) {
			/* [한국어] trid 4-tuple 일치 — qpair 가 정리 대상 listener 를 통해 들어온 것. */
			if (ctx->subsystem == NULL ||
			    (qpair->ctrlr != NULL && ctx->subsystem == qpair->ctrlr->subsys)) {
				/* [한국어] subsystem 매칭 검사:
				 *  - ctx->subsystem == NULL: 트랜스포트 단위 정리 — listener 통해 들어온 모든 qpair disconnect.
				 *  - ctx->subsystem != NULL: subsystem 단위 — qpair->ctrlr 가 그 subsystem 에 속할 때만.
				 *  qpair->ctrlr 가 NULL 인 경우(아직 fabric connect 안 끝난 상태): subsystem 매칭 안 됨 → skip. */
				spdk_nvmf_qpair_disconnect(qpair);
				/* [한국어] qpair 비동기 disconnect 시작 — ctrlr/subsystem 정리 + 백엔드 ops->qpair_fini.
				 *  실제 cleanup 은 별도 reactor 콜백 체인에서 진행. 본 함수는 시작만 트리거. */
			}
		}
	}
	spdk_for_each_channel_continue(i, 0);
	/* [한국어] 다음 channel 로 진행 통지 — for_each_channel 메커니즘 필수 호출.
	 *  마지막 channel 의 continue 후 fini 콜백(nvmf_stop_listen_fini)이 자동 호출됨. */
}

/*
 * [한국어]
 * spdk_nvmf_transport_stop_listen_async - listener 정리 + 모든 매칭 qpair disconnect (외부 비동기 API)
 *
 * @transport: 대상 트랜스포트.
 * @trid: listener 식별. subnqn 은 빈 문자열이어야 함 (subsystem 은 별도 인자).
 * @subsystem: 추가 필터 — NULL 이면 모든 subsystem, 아니면 해당 subsystem 의 qpair 만.
 * @cb_fn: 모든 정리 완료 후 호출할 사용자 콜백.
 * @cb_arg: cb_fn 의 opaque 인자.
 * @return: 0 시작 성공 / -EINVAL (subnqn 비어있지 않음) / -ENOMEM (ctx calloc 실패).
 *
 * sync 버전 spdk_nvmf_transport_stop_listen() 은 단순히 listener 의 socket 만 닫고 끝나서 in-flight
 * qpair 가 남아있으면 위험. async 버전은 모든 reactor 의 매칭 qpair 를 먼저 disconnect 한 뒤 listener 정리.
 *
 * **메커니즘**:
 *  spdk_for_each_channel 이 모든 reactor 의 nvmf poll_group channel 을 순회하며 disconnect_qpairs 호출.
 *  마지막 reactor 후 fini 콜백에서 spdk_nvmf_transport_stop_listen 으로 listener 본격 정리.
 *
 * 호출 체인:
 *   subsystem destroy / RPC remove_listener (graceful) → [spdk_nvmf_transport_stop_listen_async]
 *     → spdk_for_each_channel(disconnect_qpairs, fini) → 모든 reactor 순회
 *     → fini → spdk_nvmf_transport_stop_listen + cb_fn + free.
 */
int
spdk_nvmf_transport_stop_listen_async(struct spdk_nvmf_transport *transport,
				      const struct spdk_nvme_transport_id *trid,
				      struct spdk_nvmf_subsystem *subsystem,
				      spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
				      void *cb_arg)
{
	struct nvmf_stop_listen_ctx *ctx;
	/* [한국어] async 작업 컨텍스트 — 모든 channel 순회가 끝날 때까지 살아남아야 함. */

	if (trid->subnqn[0] != '\0') {
		/* [한국어] trid 의 subnqn 은 NVMe-oF discovery 용 필드. stop_listen 에서는 subsystem 인자로 별도 지정 →
		 *  subnqn 까지 채워온 호출자는 API 오용. */
		SPDK_ERRLOG("subnqn should be empty, use subsystem pointer instead\n");
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(struct nvmf_stop_listen_ctx));
	/* [한국어] 컨텍스트 할당 — 모든 channel 순회 완료까지 lifetime 보장. */
	if (ctx == NULL) {
		/* [한국어] OOM. */
		return -ENOMEM;
	}

	ctx->trid = *trid;
	/* [한국어] trid 깊은 복사 — 호출자 trid 가 stack 객체일 수 있음. */
	ctx->subsystem = subsystem;
	/* [한국어] subsystem 포인터 저장 (NULL 가능). */
	ctx->transport = transport;
	/* [한국어] 트랜스포트 포인터 저장. */
	ctx->cb_fn = cb_fn;
	/* [한국어] 사용자 완료 콜백. */
	ctx->cb_arg = cb_arg;
	/* [한국어] 콜백의 opaque 인자. */

	spdk_for_each_channel(transport->tgt, nvmf_stop_listen_disconnect_qpairs, ctx,
			      nvmf_stop_listen_fini);
	/* [한국어] target 의 io_device 의 모든 channel(=reactor 의 poll_group) 을 순회하며:
	 *  - 각 reactor 에서 disconnect_qpairs 호출 (해당 reactor 의 매칭 qpair disconnect)
	 *  - 마지막 reactor 후 fini 호출 (listener stop_listen + cb_fn + free)
	 *  spdk_thread/reactor 모델 — 각 channel 콜백은 해당 reactor 의 thread 에서 실행됨. */

	return 0;
	/* [한국어] async 작업 시작 성공 — 완료는 cb_fn 으로 통지. */
}

/*
 * [한국어]
 * nvmf_transport_listener_discover - listener 의 discovery log entry 채움 (백엔드 위임)
 *
 * @transport: 대상 트랜스포트.
 * @trid: discovery 대상 listener 의 trid.
 * @entry: 채울 discovery log page entry (NVMe-oF 1.1 §5.16.5.1 — adrfam/trtype/portid 등).
 *
 * 호스트가 discovery controller 에 connect 하여 GET_LOG_PAGE 로 받는 응답 entry 의 트랜스포트별
 * 필드 채움. 백엔드별로 trsvcid format / TSAS (Transport Specific Address Subtype) 가 다르므로 위임.
 *
 * 호출 체인:
 *   nvmf_subsystem_discovery_log_page_entry_get → [nvmf_transport_listener_discover]
 *     → ops->listener_discover (RDMA: TSAS.RDMA / TCP: TSAS.TCP 채움).
 */
void
nvmf_transport_listener_discover(struct spdk_nvmf_transport *transport,
				 struct spdk_nvme_transport_id *trid,
				 struct spdk_nvmf_discovery_log_page_entry *entry)
{
	transport->ops->listener_discover(transport, trid, entry);
	/* [한국어] 백엔드 listener_discover 위임 — 모든 백엔드가 구현 필수 (NULL 검사 없음). */
}

/*
 * [한국어]
 * nvmf_tgroup_poll - 트랜스포트 poll_group 의 spdk_poller 진입점 (어댑터)
 *
 * @arg: spdk_nvmf_transport_poll_group* (poller 등록 시 cb_arg 로 던짐).
 * @return: SPDK_POLLER_BUSY (실제 처리 발생) / SPDK_POLLER_IDLE (없음).
 *
 * SPDK reactor 의 poller 시스템이 호출하는 어댑터. 실제 polling 은 백엔드의 poll_group_poll 에 위임.
 * 반환 값은 reactor 가 스마트 idle 처리(예: dynamic core scheduler)에 사용 — IDLE 이면 idle 시간 계산.
 *
 * 호출 체인:
 *   spdk_poller_loop (reactor 메인 루프) → [nvmf_tgroup_poll]
 *     → nvmf_transport_poll_group_poll → ops->poll_group_poll (RDMA: ibv_poll_cq / TCP: epoll_wait).
 */
static int
nvmf_tgroup_poll(void *arg)
{
	struct spdk_nvmf_transport_poll_group *tgroup = arg;
	/* [한국어] poller 등록 시 cb_arg 로 넘긴 tgroup 포인터 복원. */
	int rc;
	/* [한국어] 처리한 이벤트 수 (백엔드 정의). */

	rc = nvmf_transport_poll_group_poll(tgroup);
	/* [한국어] 백엔드 polling 함수 호출 — IO completion / new connection / keep-alive 처리. */
	return rc == 0 ? SPDK_POLLER_IDLE : SPDK_POLLER_BUSY;
	/* [한국어] 0 이면 처리 없음 → IDLE (reactor 가 sleep 가능 표시).
	 *  >0 이면 처리됨 → BUSY (reactor 가 즉시 다음 poll 진행). */
}

/*
 * [한국어]
 * nvmf_transport_poll_group_create_poller - tgroup 의 spdk_poller 등록 (생성 + resume 시 사용)
 *
 * @tgroup: poller 를 등록할 트랜스포트 poll_group.
 *
 * spdk_poller_register_named 로 reactor poller 등록 + interrupt 모드 비활성화. poller 이름은
 * "nvmf_<trtype>" 형식으로 운영자가 perf/dtrace 에서 식별 용이. period=0 은 매 reactor 사이클마다 실행
 * (busy polling).
 *
 * 호출 체인:
 *   nvmf_transport_poll_group_create / _resume → [nvmf_transport_poll_group_create_poller]
 *     → spdk_poller_register_named(nvmf_tgroup_poll, tgroup, period=0, name) → reactor poller 리스트 추가.
 */
static void
nvmf_transport_poll_group_create_poller(struct spdk_nvmf_transport_poll_group *tgroup)
{
	char poller_name[SPDK_NVMF_TRSTRING_MAX_LEN + 32];
	/* [한국어] poller 이름 버퍼 — "nvmf_<trtype>" 길이 충분 + 32 여유. */

	snprintf(poller_name, sizeof(poller_name), "nvmf_%s", tgroup->transport->ops->name);
	/* [한국어] "nvmf_RDMA"/"nvmf_TCP" 등 — perf/dtrace 에서 백엔드별 poller 식별. */
	tgroup->poller = spdk_poller_register_named(nvmf_tgroup_poll, tgroup, 0, poller_name);
	/* [한국어] poller 등록. period=0 → 매 reactor 루프 실행 (busy polling, polled-mode 의 핵심).
	 *  cb_arg=tgroup 으로 어댑터에서 복원. 반환은 poller 핸들 — _unregister 시 필요. */
	spdk_poller_register_interrupt(tgroup->poller, NULL, NULL);
	/* [한국어] interrupt-driven 모드 비활성화 (NULL/NULL) — busy polling 만 사용.
	 *  interrupt 모드는 idle 시 reactor 가 sleep 했다가 fd readiness 로 깨는 옵션이지만 NVMe-oF target 은
	 *  지연 최소화 위해 polled-mode 고정. */
}

/*
 * [한국어]
 * nvmf_transport_poll_group_create - 한 reactor 의 트랜스포트 poll_group(tgroup) 생성
 *
 * @transport: 소속 트랜스포트.
 * @group: 상위 nvmf poll_group (per-reactor 객체) — tgroup 은 이 안에 속함.
 * @return: 생성된 tgroup 또는 NULL (실패).
 *
 * 한 reactor 의 nvmf poll_group 이 트랜스포트별 자식 poll_group 을 만들 때 호출. 동작:
 *  1) 백엔드 poll_group_create 호출 (mutex 보호) — 백엔드가 자기 객체 할당 + RDMA CQ/TCP epoll fd 등 준비.
 *  2) tgroup->transport / pending_buf_queue / poller 초기화.
 *  3) iobuf 사용 트랜스포트면 buf_cache 할당 + spdk_iobuf_channel_init (per-reactor 캐시).
 *
 * **buf_cache_size 자동 계산**: UINT32_MAX 면 num_shared_buffers * 75% / num_poll_groups.
 *  - 75% 마진: 일부는 "free" 풀로 남겨 다른 reactor 가 spike 시 빌릴 수 있게.
 *  - num_poll_groups 가 0 이면 spdk_env_get_core_count() 로 fallback (divide-by-zero 방지).
 *
 * **io_unit_size 분기**:
 *  - small_bufsize 이하면 small 캐시만 사용 (large_cache_size=0).
 *  - 그 외엔 small/large 둘 다 캐시 (io_unit_size 가 large pool 에서 할당되지만 small 도 secondary 사용 가능).
 *
 * **실패 fallback**: spdk_iobuf_channel_init 가 full size 로 실패하면 0/0 으로 재시도 (캐시 비활성화).
 *  poll_group 자체는 살리고 매 IO 마다 풀에서 직접 가져오도록 함.
 *
 * 호출 체인:
 *   nvmf_poll_group_create (lib/nvmf/nvmf.c, per-reactor 등록 시)
 *     → [nvmf_transport_poll_group_create] → ops->poll_group_create + poller 등록 + iobuf 채널.
 */
struct spdk_nvmf_transport_poll_group *
nvmf_transport_poll_group_create(struct spdk_nvmf_transport *transport,
				 struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_transport_poll_group *tgroup;
	/* [한국어] 백엔드가 할당할 tgroup 포인터. */
	struct spdk_iobuf_opts opts_iobuf = {};
	/* [한국어] iobuf 모듈 설정 조회용 (small_bufsize). */
	uint32_t buf_cache_size, small_cache_size, large_cache_size;
	/* [한국어] 캐시 크기 — 사용자 지정 또는 자동 계산. */
	int rc;
	/* [한국어] iobuf channel init 결과. */

	pthread_mutex_lock(&transport->mutex);
	/* [한국어] 백엔드 poll_group_create 호출 보호 — 백엔드가 자기 trgroup 리스트에 INSERT 하므로 필요. */
	tgroup = transport->ops->poll_group_create(transport, group);
	/* [한국어] 백엔드 poll_group_create 호출 — RDMA: CQ/SRQ 생성 / TCP: epoll fd 생성 / 등.
	 *  반환된 tgroup 은 백엔드 specific 구조체의 첫 필드가 spdk_nvmf_transport_poll_group 이도록 임베드된 형태. */
	pthread_mutex_unlock(&transport->mutex);
	if (!tgroup) {
		/* [한국어] 백엔드 생성 실패 — 메모리/리소스 부족. */
		return NULL;
	}
	tgroup->transport = transport;
	/* [한국어] back-pointer — tgroup 에서 transport 로 거슬러 올라갈 때 사용 (req_free 등). */
	nvmf_transport_poll_group_create_poller(tgroup);
	/* [한국어] reactor poller 등록 — 매 사이클 ops->poll_group_poll 호출되도록. */

	STAILQ_INIT(&tgroup->pending_buf_queue);
	/* [한국어] iobuf 풀 고갈 시 대기하는 request 큐 초기화. nvmf_request_iobuf_get_cb 가 가용 시 dequeue. */

	if (!nvmf_transport_use_iobuf(transport)) {
		/* We aren't going to allocate any shared buffers or cache, so just return now. */
		/* [한국어] vfio_user 등 zcopy 전용 트랜스포트는 iobuf 캐시 불필요. 바로 반환. */
		return tgroup;
	}

	buf_cache_size = transport->opts.buf_cache_size;
	/* [한국어] 사용자 지정 캐시 크기. UINT32_MAX 면 자동 계산 트리거. */

	/* buf_cache_size of UINT32_MAX means the value should be calculated dynamically
	 * based on the number of buffers in the shared pool and the number of poll groups
	 * that are sharing them.  We allocate 75% of the pool for the cache, and then
	 * divide that by number of poll groups to determine the buf_cache_size for this
	 * poll group.
	 */
	if (buf_cache_size == UINT32_MAX) {
		/* [한국어] 자동 계산 모드 — 풀의 75% 를 N 개 reactor 로 균등 분배. */
		uint32_t num_shared_buffers = transport->opts.num_shared_buffers;
		/* [한국어] 트랜스포트가 보유한 총 공유 버퍼 수. */

		/* Theoretically the nvmf library can dynamically add poll groups to
		 * the target, after transports have already been created.  We aren't
		 * going to try to really handle this case efficiently, just do enough
		 * here to ensure we don't divide-by-zero.
		 */
		uint16_t num_poll_groups = group->tgt->num_poll_groups ? : spdk_env_get_core_count();
		/* [한국어] 활성 poll_group 개수 — 0 이면 reactor 코어 수로 fallback (divide-by-zero 방지).
		 *  GCC extension `?:` (omitted middle operand) — 0 이면 right operand 사용. */

		buf_cache_size = (num_shared_buffers * 3 / 4) / num_poll_groups;
		/* [한국어] 75% / N — 한 reactor 가 차지할 캐시 크기. 25% 는 spike 흡수용 free pool 로 남김. */
	}

	spdk_iobuf_get_opts(&opts_iobuf, sizeof(opts_iobuf));
	/* [한국어] iobuf 모듈의 small/large bufsize 조회 — 캐시 분배 분기. */
	small_cache_size = buf_cache_size;
	/* [한국어] small 풀 캐시 크기 — 항상 buf_cache_size (small 풀은 capsule data + metadata 등 자주 사용). */
	if (transport->opts.io_unit_size <= opts_iobuf.small_bufsize) {
		/* [한국어] io_unit_size 가 small 풀 단위에 들어가면 large 풀 캐시 불필요. */
		large_cache_size = 0;
	} else {
		/* [한국어] io_unit_size 가 large 풀 단위 → large 풀 캐시도 buf_cache_size 만큼. */
		large_cache_size = buf_cache_size;
	}

	tgroup->buf_cache = calloc(1, sizeof(*tgroup->buf_cache));
	/* [한국어] iobuf channel 핸들 할당 — channel 객체 자체는 본 함수가 calloc, init 만 spdk_iobuf 가 담당. */
	if (!tgroup->buf_cache) {
		SPDK_ERRLOG("Unable to allocate an iobuf channel in the poll group.\n");
		goto err;
	}

	rc = spdk_iobuf_channel_init(tgroup->buf_cache, transport->iobuf_name, small_cache_size,
				     large_cache_size);
	/* [한국어] iobuf channel 초기화 — 풀에서 small/large_cache_size 만큼 미리 가져와 per-reactor 캐시 채움.
	 *  이후 spdk_iobuf_get(channel) 은 캐시에서 즉시 반환 (lockless, fast path). */
	if (rc != 0) {
		/* [한국어] 풀에 캐시 만큼 buffer 가 없음 — 다른 reactor 가 이미 차지 중. */
		SPDK_ERRLOG("Unable to reserve the full number of buffers for the pg buffer cache.\n");
		rc = spdk_iobuf_channel_init(tgroup->buf_cache, transport->iobuf_name, 0, 0);
		/* [한국어] 0/0 으로 재시도 — 캐시 비활성화하고 매 IO 마다 풀 직접 접근. 성능은 떨어지지만 동작 가능. */
		if (rc != 0) {
			/* [한국어] 0/0 도 실패 — channel 자체 생성 실패. 트랜스포트 init 자체가 망가진 상태. */
			SPDK_ERRLOG("Unable to create an iobuf channel in the poll group.\n");
			goto err;
		}
	}

	return tgroup;
	/* [한국어] 정상 — tgroup 반환. */
err:
	/* [한국어] 실패 정리 — 백엔드 poll_group_destroy 로 위임 (백엔드가 buf_cache free 도 처리할 수 있음). */
	transport->ops->poll_group_destroy(tgroup);
	return NULL;
}

/*
 * [한국어]
 * nvmf_transport_get_optimal_poll_group - qpair 에 가장 적합한 tgroup 추천 (백엔드 위임)
 *
 * @transport: 대상 트랜스포트.
 * @qpair: 새로 들어온 qpair.
 * @return: 추천 tgroup (NULL 이면 백엔드가 추천 안 함 → 호출자가 임의 분배).
 *
 * RDMA/TCP 백엔드는 NUMA-locality 또는 CQ load balancing 을 위해 새 qpair 를 특정 tgroup 에 붙이는
 * 것이 유리할 수 있음. ops->get_optimal_poll_group 가 NULL 인 백엔드는 NULL 반환 → 호출자가 round-robin.
 *
 * 호출 체인:
 *   spdk_nvmf_poll_group_add → [nvmf_transport_get_optimal_poll_group] → ops->get_optimal_poll_group.
 */
struct spdk_nvmf_transport_poll_group *
nvmf_transport_get_optimal_poll_group(struct spdk_nvmf_transport *transport,
				      struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_transport_poll_group *tgroup;
	/* [한국어] 백엔드 추천 결과. */

	if (transport->ops->get_optimal_poll_group) {
		/* [한국어] 백엔드가 콜백 제공 → 위임. mutex 보호 (백엔드 자료구조 보호). */
		pthread_mutex_lock(&transport->mutex);
		tgroup = transport->ops->get_optimal_poll_group(qpair);
		pthread_mutex_unlock(&transport->mutex);

		return tgroup;
	} else {
		/* [한국어] 백엔드가 추천 미지원 → NULL 반환. 호출자는 임의 분배 (보통 round-robin). */
		return NULL;
	}
}

/*
 * [한국어]
 * nvmf_transport_poll_group_destroy - 트랜스포트 poll_group 정리 (poller + iobuf channel + 백엔드)
 *
 * @group: 정리할 tgroup.
 *
 * reactor 가 종료되거나 nvmf_poll_group 이 destroy 될 때 호출. 동작:
 *  1) spdk_poller_unregister — reactor 가 더 이상 본 tgroup 의 poll 함수를 호출 안 하도록.
 *  2) pending_buf_queue 가 비어있는지 검사 (정리 전에 in-flight IO 완료 가정).
 *  3) buf_cache 포인터 캐싱 (ops->poll_group_destroy 가 group 메모리를 free 하면 group->buf_cache 도 같이 사라짐).
 *  4) 백엔드 poll_group_destroy 호출 (mutex 보호) — 백엔드 객체 free.
 *  5) 캐싱한 buf_cache 의 channel_fini + free.
 *
 * **순서가 중요**: poller 먼저 끄고, 그다음 백엔드 destroy, 마지막 buf_cache 정리.
 *  poller 중단 → 더 이상 ops->poll 호출 안 됨 → 백엔드 객체 안전하게 free 가능.
 *  buf_cache 마지막 — req 가 in-flight 였다면 백엔드가 destroy 중에 buf put 할 수도 있어 channel 살아있어야.
 *
 * 호출 체인:
 *   nvmf_poll_group_destroy → [nvmf_transport_poll_group_destroy] → poller_unregister + ops->poll_group_destroy + iobuf cleanup.
 */
void
nvmf_transport_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_transport *transport;
	/* [한국어] tgroup 의 소속 트랜스포트 — back-pointer 캐싱. */
	struct spdk_iobuf_channel *ch = NULL;
	/* [한국어] buf_cache 포인터 캐싱 — 백엔드 destroy 후에도 안전하게 fini 하기 위함. */

	transport = group->transport;
	/* [한국어] back-pointer 캐싱 — group 이 곧 free 될 수 있어 미리 추출. */

	spdk_poller_unregister(&group->poller);
	/* [한국어] reactor poller 해제 — 이후 nvmf_tgroup_poll 더 이상 호출 안 됨.
	 *  &group->poller 인 이유: spdk_poller_unregister 가 인자를 NULL 로 셋해서 double-free 방지. */

	if (!STAILQ_EMPTY(&group->pending_buf_queue)) {
		/* [한국어] iobuf 풀 고갈 대기 큐가 비어있지 않으면 in-flight IO 가 있다는 뜻 — 위험 상황 (잘못된 destroy 순서). */
		SPDK_ERRLOG("Pending I/O list wasn't empty on poll group destruction\n");
	}

	if (nvmf_transport_use_iobuf(transport)) {
		/* The call to poll_group_destroy both frees the group memory, but also
		 * releases any remaining buffers. Cache channel pointer so we can still
		 * release the resources after the group has been freed. */
		/* [한국어] iobuf 사용 트랜스포트만 buf_cache 정리 필요. group 메모리 free 후에도 ch 사용해야 하므로 캐싱. */
		ch = group->buf_cache;
	}

	pthread_mutex_lock(&transport->mutex);
	/* [한국어] 백엔드 destroy 호출 직전 락 — 백엔드 자료구조 보호. */
	transport->ops->poll_group_destroy(group);
	/* [한국어] 백엔드 poll_group_destroy — 자기 객체 free + 남은 buf put. group 포인터는 여기서 invalid 가 됨.
	 *  그래서 위에서 ch 를 미리 캐싱한 것. */
	pthread_mutex_unlock(&transport->mutex);

	if (nvmf_transport_use_iobuf(transport)) {
		/* [한국어] iobuf 채널 finalize + free — group 이 free 된 후에도 ch 자체는 별도 calloc 영역. */
		spdk_iobuf_channel_fini(ch);
		/* [한국어] 채널의 캐시된 buffer 들을 풀로 반환 + 자료구조 정리. */
		free(ch);
		/* [한국어] channel 핸들 자체 free — _create 때 calloc 한 것에 대응. */
	}
}

/*
 * [한국어]
 * nvmf_transport_poll_group_pause - tgroup 의 polling 일시 중단 (poller 만 해제)
 *
 * @tgroup: 일시 중단할 tgroup.
 *
 * 트랜스포트 자체는 유지하되 reactor 의 polling 만 중단 — RDMA migration / TCP socket migration 등에서
 * 잠시 동결시킬 때 사용. 백엔드 객체와 buf_cache 는 그대로 유지.
 */
void
nvmf_transport_poll_group_pause(struct spdk_nvmf_transport_poll_group *tgroup)
{
	spdk_poller_unregister(&tgroup->poller);
	/* [한국어] poller 만 해제 — 백엔드 객체 / buf_cache / qpair 들은 모두 살아있음. */
}

/*
 * [한국어]
 * nvmf_transport_poll_group_resume - 일시 중단된 tgroup 의 polling 재개
 *
 * @tgroup: 재개할 tgroup.
 *
 * pause 의 반대 — poller 다시 등록. tgroup 의 다른 상태는 변하지 않음.
 */
void
nvmf_transport_poll_group_resume(struct spdk_nvmf_transport_poll_group *tgroup)
{
	nvmf_transport_poll_group_create_poller(tgroup);
	/* [한국어] poller 만 다시 등록 — 다음 reactor 사이클부터 polling 재개. */
}

/*
 * [한국어]
 * nvmf_transport_poll_group_add - qpair 를 tgroup 에 부착 (백엔드 위임 + DTrace 프로브)
 *
 * @group: 대상 tgroup.
 * @qpair: 부착할 qpair (보통 새 fabric connect 직후).
 * @return: 0 성공 / -1 트랜스포트 불일치 / 백엔드 에러.
 *
 * qpair->transport 가 처음 셋되거나 (이미 있을 시) 일치성 검증 후 백엔드 poll_group_add 호출.
 * DTrace 프로브로 add 이벤트 노출 (qpair pointer + qid + thread id).
 *
 * 호출 체인:
 *   spdk_nvmf_poll_group_add → [nvmf_transport_poll_group_add] → ops->poll_group_add (RDMA: qp 등록 / TCP: epoll_ctl ADD).
 */
int
nvmf_transport_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_qpair *qpair)
{
	if (qpair->transport) {
		/* [한국어] qpair 가 이미 트랜스포트에 바인딩됨 — 호출자 일관성 검사. */
		assert(qpair->transport == group->transport);
		/* [한국어] 같은 트랜스포트인지 확인 (debug). */
		if (qpair->transport != group->transport) {
			/* [한국어] release 빌드 안전장치 — 다른 트랜스포트 의 group 으로 잘못 add 시도 거부. */
			return -1;
		}
	} else {
		/* [한국어] 처음 add — qpair 의 transport 필드 초기화. */
		qpair->transport = group->transport;
	}

	SPDK_DTRACE_PROBE3(nvmf_transport_poll_group_add, qpair, qpair->qid,
			   spdk_thread_get_id(group->group->thread));
	/* [한국어] DTrace/USDT 프로브 — qpair 부착 시점 트레이싱. perf record / dtrace 로 hook 가능. */

	return group->transport->ops->poll_group_add(group, qpair);
	/* [한국어] 백엔드 poll_group_add — RDMA: qp_to_tgroup 매핑 추가 + CQ 등록 / TCP: socket fd 를 epoll 에 등록. */
}

/*
 * [한국어]
 * nvmf_transport_poll_group_remove - qpair 를 tgroup 에서 분리 (백엔드 위임 + DTrace 프로브)
 *
 * @group: 대상 tgroup.
 * @qpair: 분리할 qpair (disconnect 진행 중).
 * @return: 0 성공 / ENOTSUP (백엔드가 remove 콜백 미제공) / 백엔드 에러.
 *
 * qpair disconnect 경로에서 호출. 백엔드의 poll_group_remove 가 optional 이므로 NULL 체크 후 위임.
 * 일부 백엔드(TCP 등)는 remove 따로 구현 안 하고 qpair_fini 에서 한꺼번에 처리.
 *
 * 호출 체인:
 *   spdk_nvmf_qpair_disconnect → [nvmf_transport_poll_group_remove] → ops->poll_group_remove (optional).
 */
int
nvmf_transport_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				 struct spdk_nvmf_qpair *qpair)
{
	int rc = ENOTSUP;
	/* [한국어] 백엔드 미지원 시 기본 반환값 — ENOTSUP 으로 호출자가 알아챔. */

	SPDK_DTRACE_PROBE3(nvmf_transport_poll_group_remove, qpair, qpair->qid,
			   spdk_thread_get_id(group->group->thread));
	/* [한국어] DTrace 프로브 — qpair 분리 시점 트레이싱. */

	assert(qpair->transport == group->transport);
	/* [한국어] 호출 일관성 검증 — 다른 트랜스포트의 group 에서 remove 시도 시 debug 빌드 abort. */
	if (group->transport->ops->poll_group_remove) {
		/* [한국어] 백엔드가 remove 콜백 제공 시만 호출. NULL 이면 ENOTSUP 반환 — 호출자가 다른 정리 경로 사용. */
		rc = group->transport->ops->poll_group_remove(group, qpair);
	}

	return rc;
}

/*
 * [한국어]
 * nvmf_transport_poll_group_poll - tgroup polling 디스패처 (reactor poller 가 호출)
 *
 * @group: polling 대상 tgroup.
 * @return: 처리한 이벤트 수 (음수면 에러). 백엔드 정의.
 *
 * 매 reactor 사이클마다 nvmf_tgroup_poll 어댑터가 호출. 백엔드의 poll_group_poll 에 위임.
 * 실제 IO completion 처리, 새 SQE 수신, keep-alive 처리 등이 여기서 발생.
 *
 * 호출 체인:
 *   reactor poller loop → nvmf_tgroup_poll → [nvmf_transport_poll_group_poll]
 *     → ops->poll_group_poll (RDMA: ibv_poll_cq + 처리 / TCP: epoll_wait + 처리).
 */
int
nvmf_transport_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	return group->transport->ops->poll_group_poll(group);
	/* [한국어] 백엔드 polling 콜백 위임 — hot path. lock 없이 (이 reactor 가 단독 소유). */
}

/*
 * [한국어]
 * nvmf_transport_req_free - request 해제 (백엔드 위임)
 *
 * @req: 해제할 request.
 *
 * NVMe-oF target 이 IO 처리 중 abort/reset 시 백엔드에 request 메모리 해제 요청. 백엔드는 자기
 * request 풀에서 해당 객체를 free + 관련 버퍼 정리. 보통 hot path 외에서 호출 (정상 완료는 req_complete).
 *
 * 호출 체인:
 *   nvmf_request_exec / abort 경로 → [nvmf_transport_req_free] → ops->req_free.
 */
void
nvmf_transport_req_free(struct spdk_nvmf_request *req)
{
	req->qpair->transport->ops->req_free(req);
	/* [한국어] qpair 를 통해 transport 도달 → 백엔드 req_free. 백엔드가 메모리 풀 반환. */
}

/*
 * [한국어]
 * nvmf_transport_req_complete - request 완료 통지 (백엔드 위임 — wire 응답 송신 트리거)
 *
 * @req: 완료된 request.
 *
 * NVMe command 처리가 끝나서 host 에 CQE 보내야 할 때 호출. 백엔드가 transport-specific CQE 송신:
 *  - RDMA: ibv_post_send 로 SEND opcode WR — host 가 RECV WR 로 받음.
 *  - TCP: socket send — NVMe/TCP CapsuleResponse PDU 전송.
 *
 * 호출 체인:
 *   nvmf_ctrlr_complete / req_complete → [nvmf_transport_req_complete] → ops->req_complete.
 */
void
nvmf_transport_req_complete(struct spdk_nvmf_request *req)
{
	req->qpair->transport->ops->req_complete(req);
	/* [한국어] 백엔드 req_complete — wire 로 CQE 송신. hot path. */
}

/*
 * [한국어]
 * nvmf_transport_qpair_fini - qpair 정리 (disconnect 마지막 단계, 백엔드 위임)
 *
 * @qpair: 정리할 qpair.
 * @cb_fn: 정리 완료 콜백 (백엔드가 async 일 때).
 * @cb_arg: cb_fn 의 opaque 인자.
 *
 * qpair disconnect 의 최종 단계 — 백엔드가 RDMA QP destroy / TCP socket close 등 자기 자원 정리.
 * DTrace 프로브로 fini 시점 노출.
 *
 * 호출 체인:
 *   spdk_nvmf_qpair_disconnect → 여러 정리 단계 → [nvmf_transport_qpair_fini] → ops->qpair_fini → cb_fn.
 */
void
nvmf_transport_qpair_fini(struct spdk_nvmf_qpair *qpair,
			  spdk_nvmf_transport_qpair_fini_cb cb_fn,
			  void *cb_arg)
{
	SPDK_DTRACE_PROBE1(nvmf_transport_qpair_fini, qpair);
	/* [한국어] DTrace 프로브 — qpair 종료 시점 트레이싱. */

	qpair->transport->ops->qpair_fini(qpair, cb_fn, cb_arg);
	/* [한국어] 백엔드 qpair_fini — async 가능. 완료 시 cb_fn(cb_arg) 호출. */
}

/*
 * [한국어]
 * nvmf_transport_qpair_get_peer_trid - qpair 의 peer (host) trid 조회 (백엔드 위임)
 *
 * @qpair: 대상 qpair.
 * @trid: 출력 trid 버퍼 — 백엔드가 채움.
 * @return: 0 성공 / 백엔드 에러.
 *
 * 호스트가 보낸 connect 의 source 주소 — 로깅/감사용. 백엔드가 socket peer 또는 RDMA connection
 * peer 주소를 추출.
 */
int
nvmf_transport_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvme_transport_id *trid)
{
	return qpair->transport->ops->qpair_get_peer_trid(qpair, trid);
	/* [한국어] 백엔드 위임 — TCP: getpeername() / RDMA: cma_id 의 remote_addr. */
}

/*
 * [한국어]
 * nvmf_transport_qpair_get_local_trid - qpair 의 local (target) trid 조회 (백엔드 위임)
 *
 * @qpair: 대상 qpair.
 * @trid: 출력 trid 버퍼.
 * @return: 0 성공 / 백엔드 에러.
 *
 * target 이 binding 된 IP:port — 멀티 NIC 환경에서 어떤 인터페이스로 들어왔는지 식별.
 */
int
nvmf_transport_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvme_transport_id *trid)
{
	return qpair->transport->ops->qpair_get_local_trid(qpair, trid);
	/* [한국어] 백엔드 위임 — TCP: getsockname() / RDMA: cma_id 의 local_addr. */
}

/*
 * [한국어]
 * nvmf_transport_qpair_get_listen_trid - qpair 가 들어온 listener 의 trid 조회 (백엔드 위임)
 *
 * @qpair: 대상 qpair.
 * @trid: 출력 trid 버퍼 — listener 의 trid (transport->listeners 의 한 원소).
 * @return: 0 성공 / 백엔드 에러.
 *
 * stop_listen_async 의 disconnect_qpairs 가 사용 — qpair 가 어떤 listener 를 통해 들어왔는지 식별.
 * 동일 IP:port 라도 별도 listener 면 다른 trid (sock_impl 등 옵션 차이).
 */
int
nvmf_transport_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				     struct spdk_nvme_transport_id *trid)
{
	return qpair->transport->ops->qpair_get_listen_trid(qpair, trid);
	/* [한국어] 백엔드 위임 — qpair 가 accept 시 기록한 listener trid 복사. */
}

/*
 * [한국어]
 * nvmf_transport_qpair_abort_request - qpair 안의 특정 request abort (백엔드 위임)
 *
 * @qpair: 대상 qpair.
 * @req: abort 처리 결과를 담을 admin Abort command request — qpair_abort_request 가
 *       req->cmd 의 cdw10 에 들어있는 abort 대상 CID 를 보고 매칭 outstanding 명령을 cancel.
 *
 * NVMe Abort admin command (Opcode 0x08) 처리. 백엔드별 in-flight WR/socket buffer 의 매칭 명령
 * 취소 + req 의 SCT/SC 셋팅.
 *
 * 호출 체인:
 *   nvmf_ctrlr_admin_cmd (Abort opcode) → [nvmf_transport_qpair_abort_request] → ops->qpair_abort_request (optional).
 */
void
nvmf_transport_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvmf_request *req)
{
	if (qpair->transport->ops->qpair_abort_request) {
		/* [한국어] 백엔드가 abort 콜백 제공 시만 호출. NULL 인 백엔드는 abort 미지원 — req 는 호출자가 다른 경로로 완료. */
		qpair->transport->ops->qpair_abort_request(qpair, req);
	}
}

/*
 * [한국어]
 * spdk_nvmf_transport_opts_init - 백엔드 default opts 로 사용자 버퍼 초기화
 *
 * @transport_name: 백엔드 식별자 ("RDMA"/"TCP"/...).
 * @opts: 사용자가 채울 출력 버퍼.
 * @opts_size: 사용자 버퍼의 sizeof — ABI 호환 키.
 * @return: true 성공 / false 백엔드 미등록 또는 인자 오류.
 *
 * 사용자가 트랜스포트 생성 직전, 백엔드의 합리적 default 값으로 opts 를 미리 채우는 데 사용.
 * 동작:
 *  1) 본 레이어가 association_timeout / acceptor_poll_rate / kas / min_kato / oncs / fuses 의 default 채움.
 *  2) 백엔드의 opts_init 콜백 호출 — 백엔드 specific default (max_io_size, num_shared_buffers 등) 채움.
 *  3) ABI 호환 SET_FIELD 로 opts_local → opts 복사 (사용자 buf 크기 안에 있는 필드만).
 *
 * 호출 체인:
 *   사용자 코드 → [spdk_nvmf_transport_opts_init] → ops->opts_init → nvmf_transport_opts_copy.
 */
bool
spdk_nvmf_transport_opts_init(const char *transport_name,
			      struct spdk_nvmf_transport_opts *opts, size_t opts_size)
{
	const struct spdk_nvmf_transport_ops *ops;
	/* [한국어] 백엔드 vtable 포인터 — opts_init 콜백 호출용. */
	struct spdk_nvmf_transport_opts opts_local = {};
	/* [한국어] full sizeof 의 로컬 버퍼 — 백엔드 default 받은 후 사용자 buf 로 trim 복사.
	 *  사용자 buf 가 작아도 본 레이어 코드는 항상 전체 필드 접근 가능. */

	ops = nvmf_get_transport_ops(transport_name);
	/* [한국어] 백엔드 vtable 조회. */
	if (!ops) {
		SPDK_ERRLOG("Transport type %s unavailable.\n", transport_name);
		return false;
	}

	if (!opts) {
		/* [한국어] NULL 출력 버퍼 — 사용자 오용. */
		SPDK_ERRLOG("opts should not be NULL\n");
		return false;
	}

	if (!opts_size) {
		/* [한국어] sizeof 0 — ABI 호환 메커니즘 무력화 — 거부. */
		SPDK_ERRLOG("opts_size inside opts should not be zero value\n");
		return false;
	}

	opts_local.association_timeout = NVMF_TRANSPORT_DEFAULT_ASSOCIATION_TIMEOUT_IN_MS;
	/* [한국어] association idle timeout 기본 120000ms (2분). NVMe-oF 1.1 §5. */
	opts_local.acceptor_poll_rate = SPDK_NVMF_DEFAULT_ACCEPT_POLL_RATE_US;
	/* [한국어] accept poller 호출 주기 기본값 (μs). 보통 10000 (10ms). */
	opts_local.disable_command_passthru = false;
	/* [한국어] custom admin cmd hooks 활성화 (default). */
	opts_local.kas = NVMF_DEFAULT_KAS;
	/* [한국어] Keep Alive Support time unit 기본값 (보통 10 = 1000ms). */
	opts_local.min_kato = NVMF_DEFAULT_MIN_KATO;
	/* [한국어] 최소 Keep Alive Timeout 기본값 (ms). */
	opts_local.oncs.raw = UINT16_MAX;
	/* [한국어] Optional NVM Command Support — 모든 비트 셋 (모든 optional cmd 지원).
	 *  백엔드가 지원 안 하는 cmd 가 있으면 opts_init 에서 일부 비트 클리어 가능. */
	opts_local.fuses.raw = UINT16_MAX;
	/* [한국어] Fused Operation Support — 모든 비트 셋 (default 지원). */
	ops->opts_init(&opts_local);
	/* [한국어] 백엔드 opts_init 호출 — max_io_size/num_shared_buffers/io_unit_size/buf_cache_size 등 채움.
	 *  RDMA: 기본 max_io_size=131072, num_shared_buffers=512 / TCP: 다른 값. */

	nvmf_transport_opts_copy(opts, &opts_local, opts_size);
	/* [한국어] 사용자 버퍼로 trim 복사 — opts_size 안에 들어가는 필드만 안전하게 셋. */

	return true;
}

/*
 * [한국어]
 * spdk_nvmf_request_free_buffers - request 의 IOV 버퍼들을 iobuf 풀로 반환 (외부 API)
 *
 * @req: 버퍼 반환 대상 request.
 * @group: req 가 속한 tgroup — buf_cache (iobuf channel) 참조.
 * @transport: 소속 트랜스포트 (현재 미사용 인자 — 시그니처 호환성).
 *
 * IO 처리가 완료되어 데이터 영역을 더 이상 사용 안 할 때 호출. req->iov 의 모든 entry 를 풀로 반환 +
 * iov_base/len 클리어 + iovcnt=0 + data_from_pool=false. 백엔드의 req_complete 가 wire 송신 후 호출.
 *
 * 호출 체인:
 *   ops->req_complete (wire send 후) / req_free → [spdk_nvmf_request_free_buffers] → spdk_iobuf_put.
 */
void
spdk_nvmf_request_free_buffers(struct spdk_nvmf_request *req,
			       struct spdk_nvmf_transport_poll_group *group,
			       struct spdk_nvmf_transport *transport)
{
	uint32_t i;
	/* [한국어] iov 순회 인덱스. */

	for (i = 0; i < req->iovcnt; i++) {
		/* [한국어] req 가 들고 있던 모든 IOV entry 순회. */
		spdk_iobuf_put(group->buf_cache, req->iov[i].iov_base, req->iov[i].iov_len);
		/* [한국어] iobuf channel 에 buffer 반환 — 캐시에 여유 있으면 캐시로, 아니면 글로벌 풀로.
		 *  iov_len 인자는 풀에서 어느 사이즈 클래스(small/large) 인지 결정에 사용. */
		req->iov[i].iov_base = NULL;
		/* [한국어] dangling pointer 방지 — 이후 잘못된 경로로 재사용 시 segfault 즉시. */
		req->iov[i].iov_len = 0;
	}
	req->iovcnt = 0;
	/* [한국어] iov 카운트 리셋 — req 가 다시 사용될 때 새로 채워짐. */
	req->data_from_pool = false;
	/* [한국어] 풀에서 받은 적 없음으로 리셋 — 향후 free 시 풀 반환 안 할 안전장치. */
}

/*
 * [한국어]
 * nvmf_request_set_buffer - req->iov 에 한 buffer 추가 (helper)
 *
 * @req: 대상 request.
 * @buf: 추가할 buffer (iobuf 풀에서 받은 영역).
 * @length: 남은 처리해야 할 총 바이트.
 * @io_unit_size: IOV 단위 크기 — 한 buffer 의 최대 길이.
 * @return: 본 buffer 처리 후 남은 length.
 *
 * 큰 IO 를 io_unit_size 단위로 쪼개 IOV 에 채울 때 사용. iov_len 은 min(length, io_unit_size).
 * data_from_pool=true 로 마킹 — free 시 풀 반환 트리거.
 */
static int
nvmf_request_set_buffer(struct spdk_nvmf_request *req, void *buf, uint32_t length,
			uint32_t io_unit_size)
{
	req->iov[req->iovcnt].iov_base = buf;
	/* [한국어] 새 IOV slot 의 base 포인터 셋. */
	req->iov[req->iovcnt].iov_len  = spdk_min(length, io_unit_size);
	/* [한국어] 길이는 남은 length 와 io_unit_size 중 작은 값 — 마지막 slot 만 length<io_unit_size 가능. */
	length -= req->iov[req->iovcnt].iov_len;
	/* [한국어] 처리한 만큼 length 차감. */
	req->iovcnt++;
	/* [한국어] IOV 카운트 증가. */
	req->data_from_pool = true;
	/* [한국어] 풀에서 받은 buffer 임을 마킹 — free 시 spdk_iobuf_put 트리거. */

	return length;
	/* [한국어] 남은 처리 바이트 반환 — 0 이면 IO 완성. */
}

/*
 * [한국어]
 * nvmf_request_set_stripped_buffer - stripped_data 의 IOV 에 한 buffer 추가 (DIF 처리용 helper)
 *
 * @req: 대상 request — req->stripped_data 가 미리 할당되어 있어야 함.
 * @buf: 추가할 buffer.
 * @length: 남은 처리해야 할 총 바이트.
 * @io_unit_size: IOV 단위 크기.
 * @return: 본 buffer 처리 후 남은 length.
 *
 * DIF (Data Integrity Field) 가 strip 된 후의 데이터 영역 — 호스트 데이터 + PI 분리 저장.
 * req->iov 가 아닌 req->stripped_data->iov 에 들어감. dif_insert_or_strip 활성화 시 사용.
 */
static int
nvmf_request_set_stripped_buffer(struct spdk_nvmf_request *req, void *buf, uint32_t length,
				 uint32_t io_unit_size)
{
	struct spdk_nvmf_stripped_data *data = req->stripped_data;
	/* [한국어] DIF strip 된 데이터 컨테이너 — 호출자가 미리 할당. */

	data->iov[data->iovcnt].iov_base = buf;
	/* [한국어] stripped IOV slot 채움. */
	data->iov[data->iovcnt].iov_len  = spdk_min(length, io_unit_size);
	length -= data->iov[data->iovcnt].iov_len;
	data->iovcnt++;
	req->data_from_pool = true;
	/* [한국어] req 단위 마킹 — strip 데이터도 결국 풀에서 받았으므로. */

	return length;
}

static void nvmf_request_iobuf_get_cb(struct spdk_iobuf_entry *entry, void *buf);
/* [한국어] forward declaration — nvmf_request_get_buffers 가 spdk_iobuf_get 콜백으로 넘김. */

/*
 * [한국어]
 * nvmf_request_get_buffers - request 데이터 영역을 위한 IOV 버퍼들 할당 (내부 헬퍼)
 *
 * @req: 대상 request.
 * @group: 소속 tgroup — buf_cache 사용.
 * @transport: 소속 트랜스포트.
 * @length: 할당할 총 바이트.
 * @io_unit_size: IOV 단위 크기 — length / io_unit_size 만큼의 buffer 가 필요.
 * @stripped_buffers: true 면 req->stripped_data 에 채움 (DIF), false 면 req->iov 에 채움 (일반).
 * @return: 0 성공 / -EINVAL (length 가 너무 큼) / -ENOMEM (풀 고갈, 큐잉됨).
 *
 * 큰 IO 를 io_unit_size 단위로 쪼개 buffer N 개 할당 + IOV 채움. 풀 고갈 시:
 *  - 백엔드가 req_get_buffers_done 콜백 제공 → entry 등록 후 -ENOMEM 반환. 가용 시 콜백 자동 호출.
 *  - 미제공 → 그냥 -ENOMEM 반환. 호출자가 retry 책임.
 *
 * 호출 체인:
 *   spdk_nvmf_request_get_buffers / nvmf_request_get_stripped_buffers / nvmf_request_iobuf_get_cb
 *     → [nvmf_request_get_buffers] → spdk_iobuf_get (반복).
 */
static int
nvmf_request_get_buffers(struct spdk_nvmf_request *req,
			 struct spdk_nvmf_transport_poll_group *group,
			 struct spdk_nvmf_transport *transport,
			 uint32_t length, uint32_t io_unit_size,
			 bool stripped_buffers)
{
	struct spdk_iobuf_entry *entry = NULL;
	/* [한국어] 풀 고갈 시 큐잉용 entry. NULL 이면 큐잉 안 함 (즉시 -ENOMEM). */
	uint32_t num_buffers;
	/* [한국어] 필요한 buffer 개수 — ceil(length / io_unit_size). */
	uint32_t i = 0;
	/* [한국어] 할당 진행 인덱스. */
	void *buffer;
	/* [한국어] spdk_iobuf_get 결과 임시. */

	/* If the number of buffers is too large, then we know the I/O is larger than allowed.
	 *  Fail it.
	 */
	num_buffers = SPDK_CEIL_DIV(length, io_unit_size);
	/* [한국어] ceil division — length 가 io_unit_size 의 배수 아니면 +1. */
	if (spdk_unlikely(num_buffers > NVMF_REQ_MAX_BUFFERS)) {
		/* [한국어] req 의 iov 배열 크기 한계 — NVMF_REQ_MAX_BUFFERS 초과 시 IO 자체가 너무 큼. */
		return -EINVAL;
	}

	/* Use iobuf queuing only if transport supports it */
	if (transport->ops->req_get_buffers_done != NULL && !stripped_buffers) {
		/* [한국어] 백엔드가 큐잉 콜백 제공 + stripped 가 아닐 때만 큐잉 사용.
		 *  stripped 는 동기 처리 — 풀 고갈 시 호출자(stripped_buffers) 가 정리. */
		entry = &req->iobuf.entry;
		/* [한국어] req 안에 임베드된 entry 사용 — req lifetime 동안 유효. */
	}

	while (i < num_buffers) {
		/* [한국어] num_buffers 만큼 반복 할당. */
		buffer = spdk_iobuf_get(group->buf_cache, spdk_min(io_unit_size, length), entry,
					nvmf_request_iobuf_get_cb);
		/* [한국어] iobuf channel 에서 buffer 가져오기:
		 *  - 캐시에 있으면 즉시 반환 (lockless fast path).
		 *  - 캐시 비었으면 글로벌 풀에서 시도.
		 *  - 풀도 비었으면 NULL 반환. entry/cb 가 있으면 entry 를 큐에 등록 (가용 시 cb 호출).
		 *  size 는 min(io_unit_size, length) — 마지막 buffer 만 io_unit_size 보다 작을 수 있음. */
		if (spdk_unlikely(buffer == NULL)) {
			/* [한국어] 풀 고갈 — 큐잉됐거나(entry!=NULL) 호출자 retry 필요(entry==NULL). */
			req->iobuf.remaining_length = length;
			/* [한국어] 콜백 시 어디서부터 재개할지 기록. */
			return -ENOMEM;
		}
		if (stripped_buffers) {
			/* [한국어] DIF 모드 — stripped_data 에 채움. */
			length = nvmf_request_set_stripped_buffer(req, buffer, length, io_unit_size);
		} else {
			/* [한국어] 일반 모드 — req->iov 에 채움. */
			length = nvmf_request_set_buffer(req, buffer, length, io_unit_size);
		}
		i++;
	}

	assert(length == 0);
	/* [한국어] 모든 buffer 할당 후 length 가 정확히 0 — debug 빌드 검증. */

	return 0;
	/* [한국어] 정상. */
}

/*
 * [한국어]
 * nvmf_request_iobuf_get_cb - iobuf 풀 가용 시 호출되는 큐잉 콜백 (재진입 진입점)
 *
 * @entry: 큐잉됐던 spdk_iobuf_entry — req 안에 임베드된 entry. SPDK_CONTAINEROF 로 req 복원.
 * @buf: 가용해진 첫 buffer.
 *
 * 풀 고갈로 nvmf_request_get_buffers 가 -ENOMEM 반환했던 req 가, 누가 다른 buffer 를 put 해서
 * 가용해지면 spdk_iobuf 시스템이 자동 호출. 첫 buffer 셋 후 나머지 buffer 다시 시도.
 * 모든 buffer 가 채워지면 백엔드의 req_get_buffers_done 호출 — 백엔드가 다시 IO 처리 진행.
 *
 * 호출 체인:
 *   다른 곳에서 spdk_iobuf_put → spdk_iobuf 가 큐 head dequeue → [nvmf_request_iobuf_get_cb]
 *     → set_buffer + get_buffers (남은 buffer 시도) → ops->req_get_buffers_done.
 */
static void
nvmf_request_iobuf_get_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct spdk_nvmf_request *req = SPDK_CONTAINEROF(entry, struct spdk_nvmf_request, iobuf.entry);
	/* [한국어] entry 포인터 → 임베드된 req 복원 (SPDK_CONTAINEROF = offsetof 역연산). */
	struct spdk_nvmf_transport *transport = req->qpair->transport;
	/* [한국어] req → qpair → transport. */
	struct spdk_nvmf_poll_group *group = req->qpair->group;
	/* [한국어] qpair 의 nvmf poll_group. */
	struct spdk_nvmf_transport_poll_group *tgroup = nvmf_get_transport_poll_group(group, transport);
	/* [한국어] nvmf poll_group 안에서 본 transport 에 매칭하는 tgroup 찾기 (각 transport 마다 tgroup 1개). */
	uint32_t length = req->iobuf.remaining_length;
	/* [한국어] -ENOMEM 시 기록한 남은 처리 길이. */
	uint32_t io_unit_size = transport->opts.io_unit_size;
	/* [한국어] IOV 단위. */
	int rc;

	assert(tgroup != NULL);
	/* [한국어] tgroup 매칭 실패는 있을 수 없음 — qpair 가 group 에 등록될 때 tgroup 도 보장됨. */

	length = nvmf_request_set_buffer(req, buf, length, io_unit_size);
	/* [한국어] 콜백으로 받은 첫 buffer 를 IOV 에 채움 + 남은 length 갱신. */
	rc = nvmf_request_get_buffers(req, tgroup, transport, length, io_unit_size, false);
	/* [한국어] 나머지 buffer 들 다시 시도. 다시 -ENOMEM 이면 또 큐잉됨. */
	if (rc == 0) {
		/* [한국어] 모든 buffer 할당 완료 → 백엔드 req_get_buffers_done 호출 → IO 처리 재개. */
		transport->ops->req_get_buffers_done(req);
	}
}

/*
 * [한국어]
 * spdk_nvmf_request_get_buffers - request 데이터 영역 buffer 할당 (외부 API)
 *
 * @req: 대상 request — iovcnt 0 으로 시작.
 * @group: 소속 tgroup.
 * @transport: 소속 트랜스포트.
 * @length: 할당할 총 바이트.
 * @return: 0 성공 / -EINVAL / -ENOMEM (큐잉 또는 정리 후 fail).
 *
 * 백엔드가 호스트 SQE 수신 후 데이터 영역을 위해 호출. iobuf 사용 트랜스포트만 호출 가능 (assert).
 * -ENOMEM 시 백엔드가 req_get_buffers_done 미제공이면 즉시 정리(부분 할당된 buffer free) — 호출자가
 * 책임 떠안지 않게.
 *
 * 호출 체인:
 *   백엔드 SQE 처리 → [spdk_nvmf_request_get_buffers] → nvmf_request_get_buffers.
 */
int
spdk_nvmf_request_get_buffers(struct spdk_nvmf_request *req,
			      struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_transport *transport,
			      uint32_t length)
{
	int rc;

	assert(nvmf_transport_use_iobuf(transport));
	/* [한국어] 호출 가드 — iobuf 미사용 트랜스포트(zcopy 전용)는 버퍼풀 사용 안 함. */

	req->iovcnt = 0;
	/* [한국어] 시작 전 iov 카운트 0 으로 초기화 — 이전 사용 잔재 클리어. */
	rc = nvmf_request_get_buffers(req, group, transport, length, transport->opts.io_unit_size, false);
	/* [한국어] 내부 헬퍼 호출 — io_unit_size = 트랜스포트 default, stripped=false. */
	if (spdk_unlikely(rc == -ENOMEM && transport->ops->req_get_buffers_done == NULL)) {
		/* [한국어] 풀 고갈 + 백엔드가 큐잉 콜백 미제공 → 부분 할당된 buffer 정리 (호출자 부담 없음). */
		spdk_nvmf_request_free_buffers(req, group, transport);
	}

	return rc;
}

/*
 * [한국어]
 * nvmf_request_get_buffers_abort_cb - iobuf for_each_entry 콜백 — 매칭 시 entry abort
 *
 * @ch: iobuf channel.
 * @entry: 순회 중인 entry.
 * @cb_ctx: abort 대상 req (호출자가 넘긴).
 * @return: 0 (계속 순회) / 1 (매칭 + abort 후 중단).
 *
 * spdk_iobuf_for_each_entry 의 콜백 — 큐잉된 모든 entry 를 순회하며 cb_ctx (req_to_abort) 와
 * 동일한 req 의 entry 면 spdk_iobuf_entry_abort 로 cancel.
 *
 * 호출 체인:
 *   nvmf_request_get_buffers_abort → spdk_iobuf_for_each_entry → [nvmf_request_get_buffers_abort_cb] (per entry).
 */
static int
nvmf_request_get_buffers_abort_cb(struct spdk_iobuf_channel *ch, struct spdk_iobuf_entry *entry,
				  void *cb_ctx)
{
	struct spdk_nvmf_request *req, *req_to_abort = cb_ctx;
	/* [한국어] entry 의 owner req 와 abort 대상 req. */

	req = SPDK_CONTAINEROF(entry, struct spdk_nvmf_request, iobuf.entry);
	/* [한국어] entry 포인터 → 임베드된 req 복원. */
	if (req != req_to_abort) {
		/* [한국어] 다른 req 의 entry — skip 후 다음 entry 로. */
		return 0;
	}

	spdk_iobuf_entry_abort(ch, entry, spdk_min(req->iobuf.remaining_length,
			       req->qpair->transport->opts.io_unit_size));
	/* [한국어] entry 를 큐에서 제거 + 콜백 호출 안 함. abort 사이즈는 첫 buffer 만큼.
	 *  iobuf 시스템이 abort 콜백 호출 안 함 — 호출자가 req 를 다른 경로로 정리 책임. */
	return 1;
	/* [한국어] 1 반환 시 for_each_entry 가 순회 중단 — 매칭 entry 는 한 req 당 최대 1 개라서 OK. */
}

/*
 * [한국어]
 * nvmf_request_get_buffers_abort - 큐잉된 buffer 요청 취소 (abort admin cmd 처리용)
 *
 * @req: abort 대상 request.
 * @return: true 매칭 entry 발견 + 취소 / false 매칭 없음 (이미 처리됐거나 큐 안 들어갔음).
 *
 * NVMe Abort cmd 가 buffer 큐잉 중인 IO 를 cancel 하려고 호출. for_each_entry 로 모든 entry 순회 후
 * 매칭 시 abort. 본 함수가 false 반환하면 호출자는 다른 abort 경로 시도.
 *
 * 호출 체인:
 *   nvmf_ctrlr_admin_cmd (Abort) → nvmf_qpair_abort_request → [nvmf_request_get_buffers_abort]
 *     → spdk_iobuf_for_each_entry → abort_cb.
 */
bool
nvmf_request_get_buffers_abort(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_transport_poll_group *tgroup = nvmf_get_transport_poll_group(req->qpair->group,
			req->qpair->transport);
	/* [한국어] req 가 속한 tgroup 찾기 — buf_cache 가 큐잉 entry 들을 보유. */
	int rc;

	assert(tgroup != NULL);
	/* [한국어] qpair 가 group 에 등록되어 있으면 tgroup 도 항상 매칭. */

	rc = spdk_iobuf_for_each_entry(tgroup->buf_cache, nvmf_request_get_buffers_abort_cb, req);
	/* [한국어] iobuf channel 의 모든 큐잉 entry 순회 — 매칭 시 abort_cb 가 1 반환하여 중단. */
	return rc == 1;
	/* [한국어] 1 이면 매칭 + abort 성공, 0 이면 매칭 entry 없음. */
}

/*
 * [한국어]
 * nvmf_request_free_stripped_buffers - DIF stripped_data 의 IOV 버퍼들 풀 반환 + struct free
 *
 * @req: 대상 request — req->stripped_data 에 채워진 buffer 들 모두 풀로 반환.
 * @group: 소속 tgroup (buf_cache).
 * @transport: 소속 트랜스포트 (현재 미사용 — 시그니처 호환).
 *
 * spdk_nvmf_request_free_buffers 의 stripped 버전. data->iov 의 모든 buffer 를 풀로 반환 후
 * stripped_data 구조체 자체도 free (calloc 으로 할당된 것). req->stripped_data = NULL 로 클리어.
 */
void
nvmf_request_free_stripped_buffers(struct spdk_nvmf_request *req,
				   struct spdk_nvmf_transport_poll_group *group,
				   struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_stripped_data *data = req->stripped_data;
	/* [한국어] DIF strip 된 데이터 컨테이너. */
	uint32_t i;

	for (i = 0; i < data->iovcnt; i++) {
		/* [한국어] 모든 stripped IOV 순회. */
		spdk_iobuf_put(group->buf_cache, data->iov[i].iov_base, data->iov[i].iov_len);
		/* [한국어] 풀 반환. iov_base/len 클리어 안 함 — 곧 free 할 메모리. */
	}
	free(data);
	/* [한국어] stripped_data 구조체 자체 free — nvmf_request_get_stripped_buffers 가 calloc 한 것. */
	req->stripped_data = NULL;
	/* [한국어] dangling pointer 방지 — 이후 NULL 검사로 stripped 사용 여부 판단. */
}

/*
 * [한국어]
 * nvmf_request_get_stripped_buffers - DIF strip 된 데이터 영역의 IOV 버퍼 할당
 *
 * @req: 대상 request — req->iov 가 이미 채워져 있어야 함 (block 정렬 검증 대상).
 * @group: 소속 tgroup.
 * @transport: 소속 트랜스포트.
 * @length: 할당할 stripped 데이터 총 바이트.
 * @return: 0 성공 / -EINVAL (block 미정렬) / -ENOMEM (할당 실패, 정리 후).
 *
 * dif_insert_or_strip 활성 시 PI 분리한 후의 데이터 영역을 위한 별도 buffer 할당. data_block_size
 * 는 block_size - md_size (PI metadata 제외). io_unit_size 도 PI 비율 만큼 줄여 계산.
 *
 * **block 정렬 검증**: req->iov 의 모든 entry 가 block_size 의 배수여야 함 — DIF 처리는 block 단위.
 *
 * 호출 체인:
 *   백엔드 IO 처리 (DIF strip 분기) → [nvmf_request_get_stripped_buffers] → nvmf_request_get_buffers(stripped=true).
 */
int
nvmf_request_get_stripped_buffers(struct spdk_nvmf_request *req,
				  struct spdk_nvmf_transport_poll_group *group,
				  struct spdk_nvmf_transport *transport,
				  uint32_t length)
{
	uint32_t block_size = req->dif.dif_ctx.block_size;
	/* [한국어] 한 block 의 총 크기 (data + PI metadata) — 보통 520B (512+8). */
	uint32_t data_block_size = block_size - req->dif.dif_ctx.md_size;
	/* [한국어] PI 제외 data 부분만 — 보통 512B. */
	uint32_t io_unit_size = transport->opts.io_unit_size / block_size * data_block_size;
	/* [한국어] stripped IOV 단위 — 원본 io_unit_size 를 block 단위 카운트로 변환 후 data_block_size 로 환산.
	 *  예: io_unit_size=4096(8 blocks), block_size=520, data_block_size=512 → 4096 / 520 * 512 = 7 * 512 = 3584. */
	struct spdk_nvmf_stripped_data *data;
	/* [한국어] 새로 할당할 stripped_data 컨테이너. */
	uint32_t i;
	int rc;

	/* Data blocks must be block aligned */
	for (i = 0; i < req->iovcnt; i++) {
		/* [한국어] 원본 IOV 의 모든 entry 가 block_size 의 배수인지 검증.
		 *  DIF 처리는 block 경계에서만 가능 — 비정렬 시 strip 결과의 정합성 못 보장. */
		if (req->iov[i].iov_len % block_size) {
			return -EINVAL;
		}
	}

	data = calloc(1, sizeof(*data));
	/* [한국어] stripped_data 컨테이너 동적 할당 — req 안에 임베드 안 한 이유는 DIF 활성 시만 필요해서 메모리 절약. */
	if (data == NULL) {
		SPDK_ERRLOG("Unable to allocate memory for stripped_data.\n");
		return -ENOMEM;
	}
	req->stripped_data = data;
	/* [한국어] req 에 컨테이너 연결. */
	req->stripped_data->iovcnt = 0;
	/* [한국어] iovcnt 0 으로 시작 — set_stripped_buffer 가 채워나감. */

	rc = nvmf_request_get_buffers(req, group, transport, length, io_unit_size, true);
	/* [한국어] 내부 헬퍼 호출 — stripped=true 로 stripped_data 에 채워짐. */
	if (rc == -ENOMEM) {
		/* [한국어] 풀 고갈 — DIF 모드는 큐잉 미사용 → 즉시 정리. */
		nvmf_request_free_stripped_buffers(req, group, transport);
		return rc;
	}
	return rc;
	/* [한국어] 0 (성공) 또는 -EINVAL (length too large). */
}
