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
	 *  값 범위: 사용자 정의 (NULL 가능). */
	spdk_nvmf_transport_create_done_cb cb_fn;
	/* [한국어] 사용자 완료 콜백.
	 *  설정자: 사용자가 spdk_nvmf_transport_create_async() 호출 시 지정.
	 *  읽는 자: nvmf_transport_create_async_done() — 성공/실패 모두 호출 (실패 시 transport=NULL).
	 *  실행 컨텍스트: 백엔드의 create_async 가 완료 통지하는 spdk_thread (보통 호출자와 동일 thread). */
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

static int
nvmf_transport_create(const char *transport_name, struct spdk_nvmf_transport_opts *opts,
		      spdk_nvmf_transport_create_done_cb cb_fn, void *cb_arg, bool sync)
{
	struct nvmf_transport_create_ctx *ctx;
	struct spdk_iobuf_opts opts_iobuf = {};
	int rc;
	uint64_t count;
	uint32_t kas_in_ms;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		goto err;
	}

	if (!opts->opts_size) {
		SPDK_ERRLOG("The opts_size in opts structure should not be zero\n");
		goto err;
	}

	ctx->ops = nvmf_get_transport_ops(transport_name);
	if (!ctx->ops) {
		SPDK_ERRLOG("Transport type '%s' unavailable.\n", transport_name);
		goto err;
	}

	nvmf_transport_opts_copy(&ctx->opts, opts, opts->opts_size);
	if (ctx->opts.max_io_size != 0 && (!spdk_u32_is_pow2(ctx->opts.max_io_size) ||
					   ctx->opts.max_io_size < 8192)) {
		SPDK_ERRLOG("max_io_size %u must be a power of 2 and be greater than or equal 8KB\n",
			    ctx->opts.max_io_size);
		goto err;
	}

	if (ctx->opts.max_aq_depth < SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE) {
		SPDK_ERRLOG("max_aq_depth %u is less than minimum defined by NVMf spec, use min value\n",
			    ctx->opts.max_aq_depth);
		ctx->opts.max_aq_depth = SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE;
	}

	if (ctx->opts.kas == 0) {
		SPDK_ERRLOG("kas cannot be 0\n");
		goto err;
	}

	if (ctx->opts.min_kato == 0) {
		SPDK_ERRLOG("min_kato cannot be 0\n");
		goto err;
	}

	kas_in_ms = ctx->opts.kas * NVMF_KAS_TIME_UNIT_IN_MS;
	ctx->opts.min_kato = spdk_round_up(ctx->opts.min_kato, kas_in_ms);

	spdk_iobuf_get_opts(&opts_iobuf, sizeof(opts_iobuf));
	if (ctx->opts.io_unit_size == 0) {
		SPDK_ERRLOG("io_unit_size cannot be 0\n");
		goto err;
	}
	if (ctx->opts.io_unit_size > opts_iobuf.large_bufsize) {
		SPDK_ERRLOG("io_unit_size %u is larger than iobuf pool large buffer size %d\n",
			    ctx->opts.io_unit_size, opts_iobuf.large_bufsize);
		goto err;
	}

	if (ctx->opts.io_unit_size <= opts_iobuf.small_bufsize) {
		/* We'll be using the small buffer pool only */
		count = opts_iobuf.small_pool_count;
	} else {
		count = spdk_min(opts_iobuf.small_pool_count, opts_iobuf.large_pool_count);
	}

	if (ctx->opts.num_shared_buffers > count) {
		SPDK_WARNLOG("The num_shared_buffers value (%u) is larger than the available iobuf"
			     " pool size (%lu). Please increase the iobuf pool sizes.\n",
			     ctx->opts.num_shared_buffers, count);
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	/* Prioritize sync create operation. */
	if (ctx->ops->create) {
		if (sync) {
			_nvmf_transport_create_done(ctx);
			return 0;
		}

		spdk_thread_send_msg(spdk_get_thread(), _nvmf_transport_create_done, ctx);
		return 0;
	}

	assert(ctx->ops->create_async);
	rc = ctx->ops->create_async(&ctx->opts, nvmf_transport_create_async_done, ctx);
	if (rc) {
		SPDK_ERRLOG("Unable to create new transport of type %s\n", transport_name);
		goto err;
	}

	return 0;
err:
	free(ctx);
	return -1;
}

int
spdk_nvmf_transport_create_async(const char *transport_name, struct spdk_nvmf_transport_opts *opts,
				 spdk_nvmf_transport_create_done_cb cb_fn, void *cb_arg)
{
	return nvmf_transport_create(transport_name, opts, cb_fn, cb_arg, false);
}

static void
nvmf_transport_create_sync_done(void *cb_arg, struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_transport **_transport = cb_arg;

	*_transport = transport;
}

struct spdk_nvmf_transport *
spdk_nvmf_transport_create(const char *transport_name, struct spdk_nvmf_transport_opts *opts)
{
	struct spdk_nvmf_transport *transport = NULL;

	/* Current implementation supports synchronous version of create operation only. */
	assert(nvmf_get_transport_ops(transport_name) && nvmf_get_transport_ops(transport_name)->create);

	nvmf_transport_create(transport_name, opts, nvmf_transport_create_sync_done, &transport, true);
	return transport;
}

struct spdk_nvmf_transport *
spdk_nvmf_transport_get_first(struct spdk_nvmf_tgt *tgt)
{
	return TAILQ_FIRST(&tgt->transports);
}

struct spdk_nvmf_transport *
spdk_nvmf_transport_get_next(struct spdk_nvmf_transport *transport)
{
	return TAILQ_NEXT(transport, link);
}

int
spdk_nvmf_transport_destroy(struct spdk_nvmf_transport *transport,
			    spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_listener *listener, *listener_tmp;

	TAILQ_FOREACH_SAFE(listener, &transport->listeners, link, listener_tmp) {
		TAILQ_REMOVE(&transport->listeners, listener, link);
		transport->ops->stop_listen(transport, &listener->trid);
		free(listener);
	}

	if (nvmf_transport_use_iobuf(transport)) {
		spdk_iobuf_unregister_module(transport->iobuf_name);
	}

	pthread_mutex_destroy(&transport->mutex);
	transport->ops->destroy(transport, cb_fn, cb_arg);
	return 0;
}

struct spdk_nvmf_listener *
nvmf_transport_find_listener(struct spdk_nvmf_transport *transport,
			     const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_listener *listener;

	TAILQ_FOREACH(listener, &transport->listeners, link) {
		if (spdk_nvme_transport_id_compare(&listener->trid, trid) == 0) {
			return listener;
		}
	}

	return NULL;
}

int
spdk_nvmf_transport_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid, struct spdk_nvmf_listen_opts *opts)
{
	struct spdk_nvmf_listener *listener;
	int rc;

	listener = nvmf_transport_find_listener(transport, trid);
	if (!listener) {
		listener = calloc(1, sizeof(*listener));
		if (!listener) {
			return -ENOMEM;
		}

		listener->ref = 1;
		listener->trid = *trid;
		listener->sock_impl = opts->sock_impl;
		TAILQ_INSERT_TAIL(&transport->listeners, listener, link);
		pthread_mutex_lock(&transport->mutex);
		rc = transport->ops->listen(transport, &listener->trid, opts);
		pthread_mutex_unlock(&transport->mutex);
		if (rc != 0) {
			TAILQ_REMOVE(&transport->listeners, listener, link);
			free(listener);
		}
		return rc;
	}

	if (opts->sock_impl && strncmp(opts->sock_impl, listener->sock_impl, strlen(listener->sock_impl))) {
		SPDK_ERRLOG("opts->sock_impl: '%s' doesn't match listener->sock_impl: '%s'\n", opts->sock_impl,
			    listener->sock_impl);
		return -EINVAL;
	}

	++listener->ref;

	return 0;
}

int
spdk_nvmf_transport_stop_listen(struct spdk_nvmf_transport *transport,
				const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_subsystem_listener *subsystem_listener;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_listener *listener;

	listener = nvmf_transport_find_listener(transport, trid);
	if (!listener) {
		return -ENOENT;
	}

	if (--listener->ref == 0) {
		TAILQ_REMOVE(&transport->listeners, listener, link);
		pthread_mutex_lock(&transport->mutex);
		transport->ops->stop_listen(transport, trid);
		pthread_mutex_unlock(&transport->mutex);

		/* The transport listener has stopped and we are about to free trid; clear dangling pointers. */
		for (subsystem = spdk_nvmf_subsystem_get_first(transport->tgt); subsystem != NULL;
		     subsystem = spdk_nvmf_subsystem_get_next(subsystem)) {
			TAILQ_FOREACH(subsystem_listener, &subsystem->listeners, link) {
				if (subsystem_listener->trid == &listener->trid) {
					subsystem_listener->trid = NULL;
				}
			}
		}

		free(listener);
	}

	return 0;
}

struct nvmf_stop_listen_ctx {
	struct spdk_nvmf_transport *transport;
	struct spdk_nvme_transport_id trid;
	struct spdk_nvmf_subsystem *subsystem;
	spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn;
	void *cb_arg;
};

static void
nvmf_stop_listen_fini(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_stop_listen_ctx *ctx;
	struct spdk_nvmf_transport *transport;
	int rc = status;

	ctx = spdk_io_channel_iter_get_ctx(i);
	transport = ctx->transport;
	assert(transport != NULL);

	rc = spdk_nvmf_transport_stop_listen(transport, &ctx->trid);
	if (rc) {
		SPDK_ERRLOG("Failed to stop listening on address '%s'\n", ctx->trid.traddr);
	}

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, rc);
	}
	free(ctx);
}

static void
nvmf_stop_listen_disconnect_qpairs(struct spdk_io_channel_iter *i)
{
	struct nvmf_stop_listen_ctx *ctx;
	struct spdk_nvmf_poll_group *group;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_qpair *qpair, *tmp_qpair;
	struct spdk_nvme_transport_id tmp_trid;

	ctx = spdk_io_channel_iter_get_ctx(i);
	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, tmp_qpair) {
		if (spdk_nvmf_qpair_get_listen_trid(qpair, &tmp_trid)) {
			continue;
		}

		/* Skip qpairs that don't match the listen trid and subsystem pointer.  If
		 * the ctx->subsystem is NULL, it means disconnect all qpairs that match
		 * the listen trid. */
		if (!spdk_nvme_transport_id_compare(&ctx->trid, &tmp_trid)) {
			if (ctx->subsystem == NULL ||
			    (qpair->ctrlr != NULL && ctx->subsystem == qpair->ctrlr->subsys)) {
				spdk_nvmf_qpair_disconnect(qpair);
			}
		}
	}
	spdk_for_each_channel_continue(i, 0);
}

int
spdk_nvmf_transport_stop_listen_async(struct spdk_nvmf_transport *transport,
				      const struct spdk_nvme_transport_id *trid,
				      struct spdk_nvmf_subsystem *subsystem,
				      spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
				      void *cb_arg)
{
	struct nvmf_stop_listen_ctx *ctx;

	if (trid->subnqn[0] != '\0') {
		SPDK_ERRLOG("subnqn should be empty, use subsystem pointer instead\n");
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(struct nvmf_stop_listen_ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->trid = *trid;
	ctx->subsystem = subsystem;
	ctx->transport = transport;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(transport->tgt, nvmf_stop_listen_disconnect_qpairs, ctx,
			      nvmf_stop_listen_fini);

	return 0;
}

void
nvmf_transport_listener_discover(struct spdk_nvmf_transport *transport,
				 struct spdk_nvme_transport_id *trid,
				 struct spdk_nvmf_discovery_log_page_entry *entry)
{
	transport->ops->listener_discover(transport, trid, entry);
}

static int
nvmf_tgroup_poll(void *arg)
{
	struct spdk_nvmf_transport_poll_group *tgroup = arg;
	int rc;

	rc = nvmf_transport_poll_group_poll(tgroup);
	return rc == 0 ? SPDK_POLLER_IDLE : SPDK_POLLER_BUSY;
}

static void
nvmf_transport_poll_group_create_poller(struct spdk_nvmf_transport_poll_group *tgroup)
{
	char poller_name[SPDK_NVMF_TRSTRING_MAX_LEN + 32];

	snprintf(poller_name, sizeof(poller_name), "nvmf_%s", tgroup->transport->ops->name);
	tgroup->poller = spdk_poller_register_named(nvmf_tgroup_poll, tgroup, 0, poller_name);
	spdk_poller_register_interrupt(tgroup->poller, NULL, NULL);
}

struct spdk_nvmf_transport_poll_group *
nvmf_transport_poll_group_create(struct spdk_nvmf_transport *transport,
				 struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_transport_poll_group *tgroup;
	struct spdk_iobuf_opts opts_iobuf = {};
	uint32_t buf_cache_size, small_cache_size, large_cache_size;
	int rc;

	pthread_mutex_lock(&transport->mutex);
	tgroup = transport->ops->poll_group_create(transport, group);
	pthread_mutex_unlock(&transport->mutex);
	if (!tgroup) {
		return NULL;
	}
	tgroup->transport = transport;
	nvmf_transport_poll_group_create_poller(tgroup);

	STAILQ_INIT(&tgroup->pending_buf_queue);

	if (!nvmf_transport_use_iobuf(transport)) {
		/* We aren't going to allocate any shared buffers or cache, so just return now. */
		return tgroup;
	}

	buf_cache_size = transport->opts.buf_cache_size;

	/* buf_cache_size of UINT32_MAX means the value should be calculated dynamically
	 * based on the number of buffers in the shared pool and the number of poll groups
	 * that are sharing them.  We allocate 75% of the pool for the cache, and then
	 * divide that by number of poll groups to determine the buf_cache_size for this
	 * poll group.
	 */
	if (buf_cache_size == UINT32_MAX) {
		uint32_t num_shared_buffers = transport->opts.num_shared_buffers;

		/* Theoretically the nvmf library can dynamically add poll groups to
		 * the target, after transports have already been created.  We aren't
		 * going to try to really handle this case efficiently, just do enough
		 * here to ensure we don't divide-by-zero.
		 */
		uint16_t num_poll_groups = group->tgt->num_poll_groups ? : spdk_env_get_core_count();

		buf_cache_size = (num_shared_buffers * 3 / 4) / num_poll_groups;
	}

	spdk_iobuf_get_opts(&opts_iobuf, sizeof(opts_iobuf));
	small_cache_size = buf_cache_size;
	if (transport->opts.io_unit_size <= opts_iobuf.small_bufsize) {
		large_cache_size = 0;
	} else {
		large_cache_size = buf_cache_size;
	}

	tgroup->buf_cache = calloc(1, sizeof(*tgroup->buf_cache));
	if (!tgroup->buf_cache) {
		SPDK_ERRLOG("Unable to allocate an iobuf channel in the poll group.\n");
		goto err;
	}

	rc = spdk_iobuf_channel_init(tgroup->buf_cache, transport->iobuf_name, small_cache_size,
				     large_cache_size);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to reserve the full number of buffers for the pg buffer cache.\n");
		rc = spdk_iobuf_channel_init(tgroup->buf_cache, transport->iobuf_name, 0, 0);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to create an iobuf channel in the poll group.\n");
			goto err;
		}
	}

	return tgroup;
err:
	transport->ops->poll_group_destroy(tgroup);
	return NULL;
}

struct spdk_nvmf_transport_poll_group *
nvmf_transport_get_optimal_poll_group(struct spdk_nvmf_transport *transport,
				      struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_transport_poll_group *tgroup;

	if (transport->ops->get_optimal_poll_group) {
		pthread_mutex_lock(&transport->mutex);
		tgroup = transport->ops->get_optimal_poll_group(qpair);
		pthread_mutex_unlock(&transport->mutex);

		return tgroup;
	} else {
		return NULL;
	}
}

void
nvmf_transport_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_iobuf_channel *ch = NULL;

	transport = group->transport;

	spdk_poller_unregister(&group->poller);

	if (!STAILQ_EMPTY(&group->pending_buf_queue)) {
		SPDK_ERRLOG("Pending I/O list wasn't empty on poll group destruction\n");
	}

	if (nvmf_transport_use_iobuf(transport)) {
		/* The call to poll_group_destroy both frees the group memory, but also
		 * releases any remaining buffers. Cache channel pointer so we can still
		 * release the resources after the group has been freed. */
		ch = group->buf_cache;
	}

	pthread_mutex_lock(&transport->mutex);
	transport->ops->poll_group_destroy(group);
	pthread_mutex_unlock(&transport->mutex);

	if (nvmf_transport_use_iobuf(transport)) {
		spdk_iobuf_channel_fini(ch);
		free(ch);
	}
}

void
nvmf_transport_poll_group_pause(struct spdk_nvmf_transport_poll_group *tgroup)
{
	spdk_poller_unregister(&tgroup->poller);
}

void
nvmf_transport_poll_group_resume(struct spdk_nvmf_transport_poll_group *tgroup)
{
	nvmf_transport_poll_group_create_poller(tgroup);
}

int
nvmf_transport_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_qpair *qpair)
{
	if (qpair->transport) {
		assert(qpair->transport == group->transport);
		if (qpair->transport != group->transport) {
			return -1;
		}
	} else {
		qpair->transport = group->transport;
	}

	SPDK_DTRACE_PROBE3(nvmf_transport_poll_group_add, qpair, qpair->qid,
			   spdk_thread_get_id(group->group->thread));

	return group->transport->ops->poll_group_add(group, qpair);
}

int
nvmf_transport_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				 struct spdk_nvmf_qpair *qpair)
{
	int rc = ENOTSUP;

	SPDK_DTRACE_PROBE3(nvmf_transport_poll_group_remove, qpair, qpair->qid,
			   spdk_thread_get_id(group->group->thread));

	assert(qpair->transport == group->transport);
	if (group->transport->ops->poll_group_remove) {
		rc = group->transport->ops->poll_group_remove(group, qpair);
	}

	return rc;
}

int
nvmf_transport_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	return group->transport->ops->poll_group_poll(group);
}

void
nvmf_transport_req_free(struct spdk_nvmf_request *req)
{
	req->qpair->transport->ops->req_free(req);
}

void
nvmf_transport_req_complete(struct spdk_nvmf_request *req)
{
	req->qpair->transport->ops->req_complete(req);
}

void
nvmf_transport_qpair_fini(struct spdk_nvmf_qpair *qpair,
			  spdk_nvmf_transport_qpair_fini_cb cb_fn,
			  void *cb_arg)
{
	SPDK_DTRACE_PROBE1(nvmf_transport_qpair_fini, qpair);

	qpair->transport->ops->qpair_fini(qpair, cb_fn, cb_arg);
}

int
nvmf_transport_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvme_transport_id *trid)
{
	return qpair->transport->ops->qpair_get_peer_trid(qpair, trid);
}

int
nvmf_transport_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvme_transport_id *trid)
{
	return qpair->transport->ops->qpair_get_local_trid(qpair, trid);
}

int
nvmf_transport_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				     struct spdk_nvme_transport_id *trid)
{
	return qpair->transport->ops->qpair_get_listen_trid(qpair, trid);
}

void
nvmf_transport_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvmf_request *req)
{
	if (qpair->transport->ops->qpair_abort_request) {
		qpair->transport->ops->qpair_abort_request(qpair, req);
	}
}

bool
spdk_nvmf_transport_opts_init(const char *transport_name,
			      struct spdk_nvmf_transport_opts *opts, size_t opts_size)
{
	const struct spdk_nvmf_transport_ops *ops;
	struct spdk_nvmf_transport_opts opts_local = {};

	ops = nvmf_get_transport_ops(transport_name);
	if (!ops) {
		SPDK_ERRLOG("Transport type %s unavailable.\n", transport_name);
		return false;
	}

	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return false;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size inside opts should not be zero value\n");
		return false;
	}

	opts_local.association_timeout = NVMF_TRANSPORT_DEFAULT_ASSOCIATION_TIMEOUT_IN_MS;
	opts_local.acceptor_poll_rate = SPDK_NVMF_DEFAULT_ACCEPT_POLL_RATE_US;
	opts_local.disable_command_passthru = false;
	opts_local.kas = NVMF_DEFAULT_KAS;
	opts_local.min_kato = NVMF_DEFAULT_MIN_KATO;
	opts_local.oncs.raw = UINT16_MAX;
	opts_local.fuses.raw = UINT16_MAX;
	ops->opts_init(&opts_local);

	nvmf_transport_opts_copy(opts, &opts_local, opts_size);

	return true;
}

void
spdk_nvmf_request_free_buffers(struct spdk_nvmf_request *req,
			       struct spdk_nvmf_transport_poll_group *group,
			       struct spdk_nvmf_transport *transport)
{
	uint32_t i;

	for (i = 0; i < req->iovcnt; i++) {
		spdk_iobuf_put(group->buf_cache, req->iov[i].iov_base, req->iov[i].iov_len);
		req->iov[i].iov_base = NULL;
		req->iov[i].iov_len = 0;
	}
	req->iovcnt = 0;
	req->data_from_pool = false;
}

static int
nvmf_request_set_buffer(struct spdk_nvmf_request *req, void *buf, uint32_t length,
			uint32_t io_unit_size)
{
	req->iov[req->iovcnt].iov_base = buf;
	req->iov[req->iovcnt].iov_len  = spdk_min(length, io_unit_size);
	length -= req->iov[req->iovcnt].iov_len;
	req->iovcnt++;
	req->data_from_pool = true;

	return length;
}

static int
nvmf_request_set_stripped_buffer(struct spdk_nvmf_request *req, void *buf, uint32_t length,
				 uint32_t io_unit_size)
{
	struct spdk_nvmf_stripped_data *data = req->stripped_data;

	data->iov[data->iovcnt].iov_base = buf;
	data->iov[data->iovcnt].iov_len  = spdk_min(length, io_unit_size);
	length -= data->iov[data->iovcnt].iov_len;
	data->iovcnt++;
	req->data_from_pool = true;

	return length;
}

static void nvmf_request_iobuf_get_cb(struct spdk_iobuf_entry *entry, void *buf);

static int
nvmf_request_get_buffers(struct spdk_nvmf_request *req,
			 struct spdk_nvmf_transport_poll_group *group,
			 struct spdk_nvmf_transport *transport,
			 uint32_t length, uint32_t io_unit_size,
			 bool stripped_buffers)
{
	struct spdk_iobuf_entry *entry = NULL;
	uint32_t num_buffers;
	uint32_t i = 0;
	void *buffer;

	/* If the number of buffers is too large, then we know the I/O is larger than allowed.
	 *  Fail it.
	 */
	num_buffers = SPDK_CEIL_DIV(length, io_unit_size);
	if (spdk_unlikely(num_buffers > NVMF_REQ_MAX_BUFFERS)) {
		return -EINVAL;
	}

	/* Use iobuf queuing only if transport supports it */
	if (transport->ops->req_get_buffers_done != NULL && !stripped_buffers) {
		entry = &req->iobuf.entry;
	}

	while (i < num_buffers) {
		buffer = spdk_iobuf_get(group->buf_cache, spdk_min(io_unit_size, length), entry,
					nvmf_request_iobuf_get_cb);
		if (spdk_unlikely(buffer == NULL)) {
			req->iobuf.remaining_length = length;
			return -ENOMEM;
		}
		if (stripped_buffers) {
			length = nvmf_request_set_stripped_buffer(req, buffer, length, io_unit_size);
		} else {
			length = nvmf_request_set_buffer(req, buffer, length, io_unit_size);
		}
		i++;
	}

	assert(length == 0);

	return 0;
}

static void
nvmf_request_iobuf_get_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct spdk_nvmf_request *req = SPDK_CONTAINEROF(entry, struct spdk_nvmf_request, iobuf.entry);
	struct spdk_nvmf_transport *transport = req->qpair->transport;
	struct spdk_nvmf_poll_group *group = req->qpair->group;
	struct spdk_nvmf_transport_poll_group *tgroup = nvmf_get_transport_poll_group(group, transport);
	uint32_t length = req->iobuf.remaining_length;
	uint32_t io_unit_size = transport->opts.io_unit_size;
	int rc;

	assert(tgroup != NULL);

	length = nvmf_request_set_buffer(req, buf, length, io_unit_size);
	rc = nvmf_request_get_buffers(req, tgroup, transport, length, io_unit_size, false);
	if (rc == 0) {
		transport->ops->req_get_buffers_done(req);
	}
}

int
spdk_nvmf_request_get_buffers(struct spdk_nvmf_request *req,
			      struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_transport *transport,
			      uint32_t length)
{
	int rc;

	assert(nvmf_transport_use_iobuf(transport));

	req->iovcnt = 0;
	rc = nvmf_request_get_buffers(req, group, transport, length, transport->opts.io_unit_size, false);
	if (spdk_unlikely(rc == -ENOMEM && transport->ops->req_get_buffers_done == NULL)) {
		spdk_nvmf_request_free_buffers(req, group, transport);
	}

	return rc;
}

static int
nvmf_request_get_buffers_abort_cb(struct spdk_iobuf_channel *ch, struct spdk_iobuf_entry *entry,
				  void *cb_ctx)
{
	struct spdk_nvmf_request *req, *req_to_abort = cb_ctx;

	req = SPDK_CONTAINEROF(entry, struct spdk_nvmf_request, iobuf.entry);
	if (req != req_to_abort) {
		return 0;
	}

	spdk_iobuf_entry_abort(ch, entry, spdk_min(req->iobuf.remaining_length,
			       req->qpair->transport->opts.io_unit_size));
	return 1;
}

bool
nvmf_request_get_buffers_abort(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_transport_poll_group *tgroup = nvmf_get_transport_poll_group(req->qpair->group,
			req->qpair->transport);
	int rc;

	assert(tgroup != NULL);

	rc = spdk_iobuf_for_each_entry(tgroup->buf_cache, nvmf_request_get_buffers_abort_cb, req);
	return rc == 1;
}

void
nvmf_request_free_stripped_buffers(struct spdk_nvmf_request *req,
				   struct spdk_nvmf_transport_poll_group *group,
				   struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_stripped_data *data = req->stripped_data;
	uint32_t i;

	for (i = 0; i < data->iovcnt; i++) {
		spdk_iobuf_put(group->buf_cache, data->iov[i].iov_base, data->iov[i].iov_len);
	}
	free(data);
	req->stripped_data = NULL;
}

int
nvmf_request_get_stripped_buffers(struct spdk_nvmf_request *req,
				  struct spdk_nvmf_transport_poll_group *group,
				  struct spdk_nvmf_transport *transport,
				  uint32_t length)
{
	uint32_t block_size = req->dif.dif_ctx.block_size;
	uint32_t data_block_size = block_size - req->dif.dif_ctx.md_size;
	uint32_t io_unit_size = transport->opts.io_unit_size / block_size * data_block_size;
	struct spdk_nvmf_stripped_data *data;
	uint32_t i;
	int rc;

	/* Data blocks must be block aligned */
	for (i = 0; i < req->iovcnt; i++) {
		if (req->iov[i].iov_len % block_size) {
			return -EINVAL;
		}
	}

	data = calloc(1, sizeof(*data));
	if (data == NULL) {
		SPDK_ERRLOG("Unable to allocate memory for stripped_data.\n");
		return -ENOMEM;
	}
	req->stripped_data = data;
	req->stripped_data->iovcnt = 0;

	rc = nvmf_request_get_buffers(req, group, transport, length, io_unit_size, true);
	if (rc == -ENOMEM) {
		nvmf_request_free_stripped_buffers(req, group, transport);
		return rc;
	}
	return rc;
}
