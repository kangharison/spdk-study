/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK sock 추상화 레이어 본체 (sock.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK sock 서브시스템의 "프런트엔드 dispatcher" 역할을 한다. 즉, 사용자가 호출하는
 * spdk_sock_*() 공개 API(connect/listen/accept/recv/writev/group/close 등)를 받아서, 내부적으로
 * 등록된 백엔드 구현(posix, uring, SSL/PSK, vpp 등)의 vtable(struct spdk_net_impl)에 호출을
 * 위임한다. 백엔드 구현은 SPDK_SOCK_IMPL_REGISTER 매크로(constructor 속성)을 통해 라이브러리
 * 로드 시 spdk_net_impl_register()로 g_net_impls 리스트에 자동 등록된다. 또한 다음과 같은
 * 백엔드-공통 인프라를 제공한다:
 *   1) impl 선택 정책 (impl_name 인자 → set_default_impl 결과 → 등록 첫 번째 impl 순)
 *   2) sock_group: 다수 sock을 단일 polling 단위로 묶는 그룹 추상화 (impl 별 group_impl 배열)
 *   3) placement_id 매핑(spdk_sock_map): NIC RSS hash·CPU·cgroup mark를 group_impl과 매핑하여
 *      reactor affinity와 정렬
 *   4) buffer-providing recv 모델(provide_buf / get_buf / recv_next)을 위한 STAILQ pool
 *   5) POSIX TCP fd 헬퍼(getaddrinfo wrapper, fd_create/connect/connect_poll) — posix·uring·ssl
 *      모두 공유
 *   6) SPDK_LOG_REGISTER_COMPONENT(sock) 및 SPDK_TRACE_REGISTER_FN(sock_trace) — 디버그 로그와
 *      tracepoint(SOCK_REQ_QUEUE/PEND/COMPLETE)를 등록
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 스택에서 sock 레이어는 NVMe-oF TCP transport(lib/nvmf, lib/nvme transport_tcp.c),
 * iSCSI target/initiator(lib/iscsi), JSON-RPC 서버(lib/jsonrpc) 등 모든 TCP 기반 컴포넌트의
 * 공통 네트워크 추상화이다. 호출 체인은 다음과 같다:
 *   상위(애플리케이션/transport) → spdk_sock_*() (이 파일) → impl vtable(net_impl->op) →
 *     백엔드(posix.c / uring.c / ssl.c) → 시스템 호출(read/writev/sendmsg/io_uring_enter/SSL_*) →
 *     커널 TCP/TLS 스택 → NIC.
 * 모든 sock I/O는 단일 spdk_thread(reactor) 컨텍스트에서만 다루어진다 — sock_group_poll()이
 * 그 reactor의 poller로 등록되어 epoll/io_uring CQ를 폴링하고, 완료 시 sock->cb_fn(=transport
 * 콜백)을 같은 스레드에서 동기적으로 호출한다. 이 thread-affinity가 lockless 동작의 근간이다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/sock.h(공개 API), spdk_internal/sock_module.h(spdk_net_impl vtable, 백엔드 등록
 *   매크로, group_impl/sock 내부 필드 정의), spdk/fd_group.h(interrupt mode 시 fd 모음),
 *   spdk/trace.h(tracepoint), spdk/env.h(NUMA id), spdk/util.h(SPDK_GET_FIELD ABI-safe accessor).
 * - 의존되는 곳: NVMe-oF TCP, iSCSI, JSON-RPC 서버, 그리고 lib/sock/sock_rpc.c (sock_set_default_impl
 *   /sock_impl_set_options/sock_impl_get_options RPC 핸들러).
 * - 데이터 흐름: 사용자 → spdk_sock_writev_async(sock, req) → req(STAILQ로 sock->queued_reqs에
 *   enqueue) → impl->writev_async() → 커널 sendmsg/send(MSG_ZEROCOPY) → 완료 시 epoll/uring
 *   wakeup → group_poll() → sock->cb_fn(req->cb_fn 호출) → 사용자 콜백.
 * - 공유 자료구조: g_net_impls(전역 백엔드 리스트), g_default_impl(set_default_impl로 갱신),
 *   g_init_opts(spdk_sock_initialize에서 캡쳐), spdk_sock_map(placement_id ↔ group_impl,
 *   pthread_mutex로 보호 — multi-thread/multi-process 진입을 가정).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_sock_connect_ext / spdk_sock_listen_ext: impl 선택 후 백엔드 connect/listen을 위임
 *   (전역 g_default_impl, 인자 impl_name 우선순위 결정).
 * - spdk_sock_writev_async: 비동기 쓰기 — req 구조체로 iovec과 cb_fn을 묶어 백엔드 큐에 삽입.
 * - spdk_sock_group_create / poll / close: 다수 sock을 묶어 epoll(또는 io_uring CQ) 한 번으로
 *   폴링하는 그룹. impl마다 별도 group_impl을 만들어 group->group_impls STAILQ에 매단다.
 * - spdk_sock_map_insert / lookup / release: placement_id ↔ group_impl 매핑 — NIC RSS / CPU /
 *   cgroup mark 기반의 reactor 친화도 정렬에 사용.
 * - spdk_sock_posix_fd_create / connect / connect_poll_async: 백엔드들이 공유하는 POSIX
 *   TCP fd 헬퍼 (SO_RCVBUF/SNDBUF, SO_REUSEADDR, TCP_NODELAY, TCP_USER_TIMEOUT 등 공통 설정).
 * - spdk_net_impl_register: 백엔드가 SPDK_SOCK_IMPL_REGISTER constructor를 통해 자기 자신을
 *   g_net_impls에 등록하는 진입점.
 * - struct spdk_sock_placement_id_entry: placement_id ↔ group_impl 매핑의 단일 노드 (refcount
 *   기반 multi-sock 공유).
 */

#include "spdk/stdinc.h"        /* [한국어] SPDK가 추상화한 표준 C/POSIX 헤더 모음 (size_t, errno 등) */

#include "spdk/sock.h"          /* [한국어] 이 파일이 구현하는 공개 API (struct spdk_sock_opts/group/request 등) */
#include "spdk_internal/sock_module.h" /* [한국어] 백엔드 구현용 내부 헤더 — struct spdk_net_impl(vtable),
                                        * spdk_sock 내부 필드(net_impl/group_impl/queued_reqs/flags 등),
                                        * SPDK_SOCK_IMPL_REGISTER 매크로 정의를 가져온다. */
#include "spdk/log.h"           /* [한국어] SPDK_ERRLOG/SPDK_DEBUGLOG 매크로 — sock 컴포넌트 로그용 */
#include "spdk/env.h"           /* [한국어] SPDK_ENV_NUMA_ID_ANY 등 환경 추상화 상수 */
#include "spdk/util.h"          /* [한국어] SPDK_GET_FIELD(ABI-safe 접근), SPDK_COUNTOF 등 유틸 */
#include "spdk/trace.h"         /* [한국어] spdk_trace_*() — SOCK_REQ_QUEUE/PEND/COMPLETE tracepoint 등록 */
#include "spdk/fd_group.h"      /* [한국어] interrupt-mode 지원 — group_impl이 보고하는 interruptfd를
                                 * 묶어 spdk_thread가 polling 대신 epoll wait에 들어갈 수 있게 한다. */
#include "spdk_internal/trace_defs.h" /* [한국어] TRACE_SOCK_REQ_*, OWNER_TYPE_SOCK 등 trace ID 정의 */

/* [한국어] 기본 SO_PRIORITY 값 — 0은 default queue 사용을 의미하며 사용자가 priority>0을 지정하면
 * setsockopt(SOL_SOCKET, SO_PRIORITY)로 커널 qdisc 우선순위 큐에 들어간다. */
#define SPDK_SOCK_DEFAULT_PRIORITY 0
/* [한국어] zerocopy(MSG_ZEROCOPY/io_uring zerocopy)를 기본 켬 — NVMe-oF TCP large I/O에서
 * 사용자 페이지를 커널이 그대로 DMA 송신하여 memcpy 부담을 제거. */
#define SPDK_SOCK_DEFAULT_ZCOPY true
/* [한국어] 기본 ACK 타임아웃 0 = TCP_USER_TIMEOUT 미설정 (커널 기본값에 위임). 양수 ms 값을 주면
 * sock_posix_fd_create에서 TCP_USER_TIMEOUT을 설정해 dead peer를 빠르게 탐지. */
#define SPDK_SOCK_DEFAULT_ACK_TIMEOUT 0
/* [한국어] 기본 connect 타임아웃 0 = blocking connect에서 무한 대기(poll timeout=-1). */
#define SPDK_SOCK_DEFAULT_CONNECT_TIMEOUT 0

/* [한국어] getaddrinfo()에 넘기는 포트 문자열 버퍼 길이 — uint16 최대 5자리 + 여유. */
#define PORTNUMLEN 32
/* [한국어] IPv6 "[addr]" 표기에서 대괄호를 벗기기 위한 임시 버퍼 길이 상한. */
#define MAX_TMPBUF 1024

/*
 * [한국어]
 * SPDK_SOCK_OPTS_FIELD_OK - opts_size로 ABI-safe 필드 접근 여부를 검사하는 매크로.
 *
 * 동기: 라이브러리 진화 과정에서 spdk_sock_opts 구조체에 새 필드가 추가될 때, 구버전 헤더로
 * 빌드된 호출자도 ABI를 깨지 않고 호출할 수 있도록 opts->opts_size(호출자가 채워주는 자기
 * 구조체 크기)를 base로 비교한다. offsetof(field) + sizeof(field) <= opts_size 이면 그 필드가
 * 호출자 측에 존재하는 것이다.
 */
#define SPDK_SOCK_OPTS_FIELD_OK(opts, field) (offsetof(struct spdk_sock_opts, field) + sizeof(opts->field) <= (opts->opts_size))

/* [한국어] 백엔드 impl 레지스트리(전역 STAILQ). SPDK_SOCK_IMPL_REGISTER constructor 함수가
 * spdk_net_impl_register()를 호출하여 head로 insert한다(STAILQ_INSERT_HEAD).
 * 보호: 라이브러리 로드 시점(constructor)에서만 변경되므로 런타임 lock 불필요. 런타임에는
 * 읽기 전용으로 STAILQ_FOREACH 순회만 한다. */
static STAILQ_HEAD(, spdk_net_impl) g_net_impls = STAILQ_HEAD_INITIALIZER(g_net_impls);
/* [한국어] 사용자 호출 시 impl_name=NULL일 때 사용할 기본 impl 포인터. spdk_sock_set_default_impl
 * (또는 sock_set_default_impl RPC)로 설정. NULL이면 connect/listen 호출자가 impl_name을 명시해야
 * 매칭이 성공한다. */
static struct spdk_net_impl *g_default_impl;
/* [한국어] spdk_sock_initialize()로 사용자가 전달한 옵션을 보관 (group 생성 등에서 다시 참조).
 * 기본값은 enable_interrupt_mode=true: spdk_sock_group이 fd_group을 만들어 epoll-wait 모드를
 * 지원한다(이벤트 없을 때 reactor가 sleep 가능). 폴링-only 환경에서는 false로 둔다. */
static struct spdk_sock_initialize_opts g_init_opts = {
	.opts_size = sizeof(g_init_opts),
	.enable_interrupt_mode = true
};

/*
 * [한국어]
 * struct spdk_sock_placement_id_entry - placement_id ↔ group_impl 매핑의 단일 노드.
 *
 * 동기: NIC의 RSS(Receive Side Scaling) hash, CPU id, cgroup mark 같은 "이 sock을 어느
 * 그룹/스레드가 처리할지"의 hint를 정수 placement_id로 표현한다. 같은 placement_id를 갖는
 * 여러 sock은 reactor affinity를 정렬하기 위해 같은 group_impl(=같은 epoll/io_uring 인스턴스)에
 * 묶이는 것이 이상적이다 — kernel이 receive packet steering(RPS/RFS)으로 같은 코어에
 * delivery하기 때문에 cache locality가 살아남는다.
 *
 * spdk_sock_map(STAILQ + mutex)에 보관되며, 각 백엔드(예: posix.c)가 자체 map을 가지거나
 * 전역 map을 공유한다.
 */
struct spdk_sock_placement_id_entry {
	int placement_id;
	/* [한국어] NIC RSS hash / CPU id / cgroup mark — placement_id 식별자.
	 * 설정자: _sock_map_entry_alloc()에서 신규 entry 생성 시 1회.
	 * 읽는 자: spdk_sock_map_lookup/insert/release/find_free 모두.
	 * 값 범위: int (모드별 의미가 다름; 0/음수도 유효한 RSS hash가 될 수 있음).
	 * 동기화: insert/lookup/release 모두 map->mtx 잠금 하에서 비교 — race 없음. */

	uint32_t ref;
	/* [한국어] 이 placement_id를 공유하는 sock 개수(참조 카운트).
	 * 설정자: spdk_sock_map_insert()가 ++ (group이 NULL이 아닐 때만), release()가 --.
	 * 읽는 자: release()가 0이 되면 group을 NULL로 클리어 (그러나 entry 자체는 free하지 않고
	 *   다음 insert에서 재사용 — fragmentation 방지).
	 * 값 범위: 0 이상. assert(entry->ref > 0)로 release가 ref=0 entry를 또 release하지 않는지 검증.
	 * 동기화: map->mtx 보호. */

	struct spdk_sock_group_impl *group;
	/* [한국어] 이 placement_id가 매핑된 그룹 impl. 같은 placement_id의 신규 sock은 lookup() 시
	 * 이 그룹에 합류하도록 hint를 받는다.
	 * 설정자: insert()에서 처음 매핑, lookup()에서 hint를 적용, release()에서 ref=0이면 NULL로 reset.
	 * 읽는 자: lookup()이 caller에게 반환.
	 * 값 범위: 유효한 group_impl 포인터 또는 NULL(미할당).
	 * 동기화: map->mtx 보호. */

	STAILQ_ENTRY(spdk_sock_placement_id_entry) link;
	/* [한국어] map->entries STAILQ 연결자 — singly-linked tail queue. */
};

/*
 * [한국어]
 * sock_get_group_impl_from_group - sock이 속한 backend impl과 매칭되는 group_impl을 반환.
 *
 * @sock: 대상 sock — sock->net_impl(어느 백엔드인지)을 보고 매칭한다.
 * @group: 사용자 레벨 그룹 — 내부에 백엔드별 group_impl 리스트(group->group_impls)를 가진다.
 * @return: 매칭되는 group_impl 포인터, 없으면 NULL(=서로 다른 impl인 sock이 잘못 합류 시도).
 *
 * 동기: spdk_sock_group은 여러 백엔드(posix/uring/ssl)에 걸쳐 sock을 모을 수 있도록 백엔드별
 * group_impl을 STAILQ로 갖는다(group_create()에서 등록된 모든 impl마다 하나씩 생성). 새로
 * add_sock하거나 remove_sock할 때 sock의 impl과 짝이 되는 group_impl을 찾아 그쪽으로 위임해야
 * 한다 — 이 함수가 그 lookup을 담당.
 *
 * 실행 컨텍스트: 호출자(spdk_sock_group_add_sock 등)가 단일 spdk_thread 컨텍스트에서 실행되므로
 * 별도 락 없이 STAILQ 순회 가능 (group의 group_impls는 group_create/close에서만 변동).
 *
 * 호출 체인:
 *   spdk_sock_group_add_sock/remove_sock/spdk_sock_get_optimal_sock_group → [이 함수]
 */
static inline struct spdk_sock_group_impl *
sock_get_group_impl_from_group(struct spdk_sock *sock, struct spdk_sock_group *group)
{
	struct spdk_sock_group_impl *group_impl = NULL;     /* [한국어] 순회용 포인터 — STAILQ_FOREACH_FROM이 NULL을 head로 해석하여 처음부터 시작. */

	STAILQ_FOREACH_FROM(group_impl, &group->group_impls, link) { /* [한국어] 그룹 내 모든 백엔드 group_impl을 head→tail 순회. */
		if (sock->net_impl == group_impl->net_impl) {            /* [한국어] 백엔드 vtable 포인터 동일성 비교 — 같은 impl이면 매칭. */
			return group_impl;                                   /* [한국어] 매칭된 group_impl을 즉시 반환. */
		}
	}
	return NULL;                                                  /* [한국어] 매칭 실패 — sock의 impl이 group에 등록되지 않은 경우 (정상 X). */
}

/* Called under map->mtx lock */
/*
 * [한국어]
 * _sock_map_entry_alloc - placement_id 매핑 테이블에 새 entry를 할당하고 tail에 삽입.
 *
 * @map: 대상 spdk_sock_map (entries STAILQ + mtx로 구성)
 * @placement_id: 새 entry에 할당할 식별자 (NIC RSS hash / CPU id / cgroup mark 중 하나)
 * @return: 할당한 entry 포인터, calloc 실패 시 NULL.
 *
 * 사전조건: 호출자가 반드시 map->mtx를 잡고 호출해야 한다 (영문 주석으로 명시됨).
 * 이 함수는 map 내부 자료구조를 변경하므로 lock 없이 호출하면 STAILQ 손상 가능.
 *
 * group 필드는 calloc(0)으로 NULL인 상태로 남는다 — 호출자(insert/lookup)가 직후에 채운다.
 *
 * 호출 체인:
 *   spdk_sock_map_insert / spdk_sock_map_lookup → [이 함수] → calloc()
 */
static struct spdk_sock_placement_id_entry *
_sock_map_entry_alloc(struct spdk_sock_map *map, int placement_id)
{
	struct spdk_sock_placement_id_entry *entry;        /* [한국어] 신규 entry 포인터. */

	entry = calloc(1, sizeof(*entry));                 /* [한국어] 0 채워서 할당 — group=NULL, ref=0 시작. */
	if (!entry) {                                      /* [한국어] 할당 실패 — 호출자가 -ENOMEM 반환. */
		SPDK_ERRLOG("Cannot allocate an entry for placement_id=%u\n", placement_id); /* [한국어] 디버깅용 에러 로그. */
		return NULL;                                   /* [한국어] 호출자에게 실패 통보. */
	}

	entry->placement_id = placement_id;                /* [한국어] 식별자 저장 — 이후 매칭의 키. */

	STAILQ_INSERT_TAIL(&map->entries, entry, link);    /* [한국어] tail에 삽입 — 순회 순서가 입력 순서와 같음(LRU/생성순 추적 가능). */

	return entry;                                      /* [한국어] 호출자가 group/ref를 후속 설정. */
}

/*
 * [한국어]
 * spdk_sock_map_insert - placement_id를 group_impl에 매핑(또는 ref 증가)한다.
 *
 * @map: 대상 매핑 테이블.
 * @placement_id: 매핑할 식별자 (NIC RSS / CPU id / cgroup mark).
 * @group: 매핑 대상 group_impl. NULL을 넘기면 "이 placement_id가 등록되지 않았는지" 검사 용도.
 * @return: 0(성공/이미 동일 매핑/ref 증가), -EINVAL(이미 다른 group에 매핑됨/NULL group 충돌),
 *          -ENOMEM(신규 entry 할당 실패).
 *
 * 동작 단계:
 *   1) 기존 entry 검색 — placement_id 일치하는 노드를 찾으면:
 *      a) group=NULL: 기존 group이 NULL이면 OK, 아니면 -EINVAL ("강제 해제 금지" 안전장치)
 *      b) entry->group=NULL이면 group으로 set (지연 바인딩 — lookup hint로 미리 만들어둔 entry)
 *      c) entry->group != group(다른 group)이면 -EINVAL (한 placement_id는 한 group에만 속함)
 *      d) 같으면 ref++만 (multi-sock 공유)
 *   2) 미존재면 새 entry 할당, group이 있으면 ref=1로 시작.
 *
 * 실행 컨텍스트: pthread_mutex_lock(&map->mtx) 보호 — 백엔드 polling thread와 add_sock thread가
 * 동시에 entry 갱신할 수 있으므로 진짜 락이 필요 (SPDK의 보통 thread-affinity 패턴과 다름 —
 * 이 map은 process 전역에 걸친 정렬 메커니즘이기 때문).
 *
 * 호출 체인:
 *   posix/uring 백엔드의 sock 생성/group 합류 → [이 함수] → _sock_map_entry_alloc().
 */
int
spdk_sock_map_insert(struct spdk_sock_map *map, int placement_id,
		     struct spdk_sock_group_impl *group)
{
	struct spdk_sock_placement_id_entry *entry;        /* [한국어] 검색/생성된 entry. */
	int rc = 0;                                        /* [한국어] 반환 코드, 기본 성공. */

	pthread_mutex_lock(&map->mtx);                     /* [한국어] map 보호 — concurrent insert/lookup/release 직렬화. */
	STAILQ_FOREACH(entry, &map->entries, link) {       /* [한국어] 기존 매핑 선형 탐색 — 일반적으로 entry 수 작음. */
		if (placement_id == entry->placement_id) {     /* [한국어] 같은 식별자 발견 — 기존 매핑 갱신 분기. */
			/* Can't set group to NULL if it is already not-NULL */
			if (group == NULL) {                       /* [한국어] caller가 group=NULL을 줬다면 "정리 불일치 감지" 의도. */
				rc = (entry->group == NULL) ? 0 : -EINVAL; /* [한국어] entry가 이미 NULL이면 OK, 아니면 강제 해제 시도로 거부. */
				goto end;
			}

			if (entry->group == NULL) {                /* [한국어] 기존 entry는 hint로 만들어졌으나 아직 group 미할당 상태. */
				entry->group = group;                  /* [한국어] 늦은 바인딩 — 첫 group 지정. */
			} else if (entry->group != group) {        /* [한국어] 이미 다른 group에 매핑됨 — 충돌. */
				rc = -EINVAL;
				goto end;
			}

			entry->ref++;                              /* [한국어] 같은 group을 공유하는 sock 수 증가. */
			goto end;
		}
	}

	entry = _sock_map_entry_alloc(map, placement_id);  /* [한국어] 신규 placement_id — entry 할당 (lock 보유 중). */
	if (entry == NULL) {                               /* [한국어] calloc 실패 — 메모리 부족. */
		rc = -ENOMEM;
		goto end;
	}
	if (group) {                                       /* [한국어] group이 주어졌다면 즉시 매핑 + ref=1. */
		entry->group = group;
		entry->ref++;
	}
end:
	pthread_mutex_unlock(&map->mtx);                   /* [한국어] 보호 해제. */

	return rc;
}

/*
 * [한국어]
 * spdk_sock_map_release - placement_id의 ref를 감소; 0이 되면 group을 해제(=NULL).
 *
 * @map: 대상 map.
 * @placement_id: 해제할 식별자.
 *
 * 동기: sock close 시 자신이 보유한 placement_id 매핑을 풀어주어, 다른 group이 그 식별자에
 * 매핑될 수 있게 만든다(또는 같은 group이 새 sock으로 다시 ref 증가). entry 자체는 free하지
 * 않고 group만 NULL로 reset — 빈번한 재바인딩 시 alloc/free 부담을 줄임.
 *
 * 호출 체인: 백엔드 close → [이 함수].
 */
void
spdk_sock_map_release(struct spdk_sock_map *map, int placement_id)
{
	struct spdk_sock_placement_id_entry *entry;        /* [한국어] 검색용 포인터. */

	pthread_mutex_lock(&map->mtx);                     /* [한국어] insert/lookup과 race 보호. */
	STAILQ_FOREACH(entry, &map->entries, link) {       /* [한국어] 일치하는 entry 탐색. */
		if (placement_id == entry->placement_id) {
			assert(entry->ref > 0);                    /* [한국어] insert와 짝이 맞는지 검증 — 0 release는 버그. */
			entry->ref--;                              /* [한국어] 참조 감소. */

			if (entry->ref == 0) {                     /* [한국어] 마지막 사용자였으면 group 해제. */
				entry->group = NULL;                   /* [한국어] entry는 보존 — 식별자 재등장 시 재사용 가능. */
			}
			break;                                     /* [한국어] 일치 entry 처리 후 즉시 종료. */
		}
	}

	pthread_mutex_unlock(&map->mtx);                   /* [한국어] 보호 해제. */
}

/*
 * [한국어]
 * spdk_sock_map_lookup - placement_id로 group_impl을 조회 (없으면 hint로 entry 생성).
 *
 * @map: 대상 map.
 * @placement_id: 조회 식별자.
 * @group: 결과 group_impl 출력 — 매핑된 group이 있으면 그것을, 없으면 hint를 반환.
 * @hint: 매핑이 없을 때 새로 등록할 group_impl 후보 (NULL이면 매핑 없을 때 -EINVAL).
 * @return: 0(성공 — *group에 결과 채움), -EINVAL(매핑 없고 hint도 NULL), -ENOMEM(entry 할당 실패).
 *
 * 동기: 새 sock이 어느 group에 합류할지 결정할 때 사용. 같은 placement_id의 sock이 이미
 * 어떤 group에 매핑되어 있으면 그곳으로 합류시키고(=cache locality 유지), 없으면 hint(예:
 * 호출자가 미리 정한 default group)를 그 식별자에 등록하고 본인도 거기 합류 — 다음 sock부터는
 * 자동으로 같은 group으로 정렬됨.
 *
 * 주의: ref는 증가시키지 않음 — caller가 명시적으로 spdk_sock_map_insert(group) 추가 호출하여
 * ref 관리한다.
 *
 * 호출 체인: 백엔드 add_sock → [이 함수] → _sock_map_entry_alloc().
 */
int
spdk_sock_map_lookup(struct spdk_sock_map *map, int placement_id,
		     struct spdk_sock_group_impl **group, struct spdk_sock_group_impl *hint)
{
	struct spdk_sock_placement_id_entry *entry;        /* [한국어] 검색 결과. */

	*group = NULL;                                     /* [한국어] 기본 출력값 NULL — 실패 시 명확한 상태. */
	pthread_mutex_lock(&map->mtx);                     /* [한국어] map 보호. */
	STAILQ_FOREACH(entry, &map->entries, link) {       /* [한국어] 기존 매핑 검색. */
		if (placement_id == entry->placement_id) {
			*group = entry->group;                     /* [한국어] 일치 발견 — 결과 저장 (NULL일 수도). */
			if (*group != NULL) {
				/* Return previously assigned sock_group */
				pthread_mutex_unlock(&map->mtx);
				return 0;                              /* [한국어] 기존 매핑 hit — 그대로 반환. */
			}
			break;                                     /* [한국어] entry는 있으나 group=NULL → 아래 hint 적용 분기로. */
		}
	}

	/* No entry with assigned sock_group, nor hint to use */
	if (hint == NULL) {
		pthread_mutex_unlock(&map->mtx);
		return -EINVAL;                                /* [한국어] hint가 없으면 어떤 group에 합류할지 결정 불가. */
	}

	/* Create new entry if there is none with matching placement_id */
	if (entry == NULL) {                               /* [한국어] 완전히 신규 placement_id — entry 생성. */
		entry = _sock_map_entry_alloc(map, placement_id);
		if (entry == NULL) {
			pthread_mutex_unlock(&map->mtx);
			return -ENOMEM;                            /* [한국어] OOM 전파. */
		}
	}

	entry->group = hint;                               /* [한국어] hint를 매핑으로 등록 — 이후 같은 placement_id는 hit. */
	pthread_mutex_unlock(&map->mtx);

	return 0;                                          /* [한국어] *group은 NULL인 채로 return — caller는 "hint 적용됨/기존 없음"을 인지. */
}

/*
 * [한국어]
 * spdk_sock_map_cleanup - map의 모든 entry를 해제 (라이브러리 종료 / 백엔드 unload 시).
 *
 * 모든 entry를 STAILQ_REMOVE 후 free. 이후 map은 재초기화 없이 재사용 가능 (entries는 비어있고
 * mtx는 그대로).
 *
 * 호출 체인: posix/uring 등 백엔드의 sock_module_fini() → [이 함수].
 */
void
spdk_sock_map_cleanup(struct spdk_sock_map *map)
{
	struct spdk_sock_placement_id_entry *entry, *tmp;  /* [한국어] FOREACH_SAFE에 필요한 임시 포인터. */

	pthread_mutex_lock(&map->mtx);                     /* [한국어] 보호. */
	STAILQ_FOREACH_SAFE(entry, &map->entries, link, tmp) { /* [한국어] 안전 순회 — 현재 노드 free 시에도 OK. */
		STAILQ_REMOVE(&map->entries, entry, spdk_sock_placement_id_entry, link); /* [한국어] 리스트에서 분리. */
		free(entry);                                   /* [한국어] 메모리 반환. */
	}
	pthread_mutex_unlock(&map->mtx);
}

/*
 * [한국어]
 * spdk_sock_map_find_free - group=NULL인 entry(=빈 자리)의 placement_id를 찾아 반환.
 *
 * @return: 빈 placement_id, 없으면 -1.
 *
 * 동기: 백엔드가 "어느 placement_id가 비어있나"를 알아내 그 자리에 새 group_impl을 매핑하기
 * 위해 사용. NIC RSS 다중 큐가 있을 때 group_impl이 아직 매핑되지 않은 RSS hash를 골라
 * 새 reactor에 할당하는 식의 분배 정책 구현에 활용.
 *
 * 호출 체인: 백엔드 — 예: posix.c의 group 분배 로직 → [이 함수].
 */
int
spdk_sock_map_find_free(struct spdk_sock_map *map)
{
	struct spdk_sock_placement_id_entry *entry;        /* [한국어] 검색 포인터. */
	int placement_id = -1;                             /* [한국어] 기본 실패 값 (찾지 못함 시). */

	pthread_mutex_lock(&map->mtx);                     /* [한국어] 보호. */
	STAILQ_FOREACH(entry, &map->entries, link) {
		if (entry->group == NULL) {                    /* [한국어] 빈 자리 발견 — 첫 빈 자리를 채택 (FIFO). */
			placement_id = entry->placement_id;
			break;
		}
	}

	pthread_mutex_unlock(&map->mtx);

	return placement_id;
}

/*
 * [한국어]
 * spdk_sock_get_optimal_sock_group - sock에 가장 적합한 group을 추천 (placement_id 기반).
 *
 * @sock: 대상 sock.
 * @group: 결과 group (NULL이면 추천 없음).
 * @hint: 호출자 선호 group — 백엔드가 hint를 우선 사용하거나 매핑이 없으면 hint를 등록.
 * @return: 0(성공; *group은 NULL일 수도 — 추천 없음 의미), -EINVAL(hint와 sock의 impl 불일치).
 *
 * 동기: NVMe-oF target이 새 connection을 받을 때 같은 placement_id의 기존 connection이 있는
 * group을 찾아 거기 합류시키면 cache locality가 살아남고, 같은 NIC 큐에서 들어오는 트래픽을
 * 같은 reactor가 처리한다(RFS와 정렬). 백엔드(예: posix)는 enable_placement_id 모드에 따라
 * NAPI/CPU/MARK 중 하나로 placement_id를 추출하고 g_map에서 lookup한다.
 *
 * 호출 체인: NVMe-oF tcp transport accept handler → [이 함수] → impl->group_impl_get_optimal()
 *           → spdk_sock_map_lookup().
 */
int
spdk_sock_get_optimal_sock_group(struct spdk_sock *sock, struct spdk_sock_group **group,
				 struct spdk_sock_group *hint)
{
	struct spdk_sock_group_impl *group_impl;            /* [한국어] 백엔드가 추천한 group_impl. */
	struct spdk_sock_group_impl *hint_group_impl = NULL;/* [한국어] hint의 백엔드별 group_impl(있으면). */

	assert(group != NULL);                              /* [한국어] 출력 포인터 NULL 방어 — 호출자 버그 검출. */

	if (hint != NULL) {                                 /* [한국어] hint를 적용할 경우, hint와 sock이 같은 impl이어야 한다. */
		hint_group_impl = sock_get_group_impl_from_group(sock, hint); /* [한국어] hint를 백엔드 group_impl로 변환. */
		if (hint_group_impl == NULL) {
			return -EINVAL;                             /* [한국어] hint group이 sock의 impl을 지원하지 않음. */
		}
	}

	group_impl = sock->net_impl->group_impl_get_optimal(sock, hint_group_impl); /* [한국어] 백엔드 vtable로 위임 — 실제 placement_id 조회. */

	if (group_impl) {
		*group = group_impl->group;                     /* [한국어] group_impl 부모 group을 사용자에게 노출. */
	}

	return 0;                                           /* [한국어] 추천 없음(*group=NULL)도 성공으로 간주. */
}

/*
 * [한국어]
 * spdk_sock_getaddr - server/client 측 IP·포트를 문자열로 추출 (vtable getaddr 위임).
 *
 * 백엔드(posix 등)가 getsockname/getpeername을 호출하여 saddr/sport(server side)와
 * caddr/cport(client side)를 채운다. SSL/TLS 백엔드는 동일 fd에 대한 결과를 그대로 반환.
 */
int
spdk_sock_getaddr(struct spdk_sock *sock, char *saddr, int slen, uint16_t *sport,
		  char *caddr, int clen, uint16_t *cport)
{
	return sock->net_impl->getaddr(sock, saddr, slen, sport, caddr, clen, cport); /* [한국어] vtable dispatch. */
}

/*
 * [한국어]
 * spdk_sock_get_interface_name - sock이 묶인 NIC 이름(예: "eth0")을 반환.
 *
 * @return: 백엔드가 SO_BINDTODEVICE/IP_PKTINFO 등으로 추출한 인터페이스 이름,
 *          백엔드가 미구현이면 NULL.
 *
 * 동기: NUMA 친화도 결정·placement 정책에 활용. NIC와 reactor를 같은 NUMA 노드에 정렬하면
 * 메모리 액세스 latency가 작아진다.
 */
const char *
spdk_sock_get_interface_name(struct spdk_sock *sock)
{
	if (sock->net_impl->get_interface_name) {           /* [한국어] 백엔드가 구현했는지 확인 (선택적 vtable slot). */
		return sock->net_impl->get_interface_name(sock);
	} else {
		return NULL;                                    /* [한국어] 미구현 — caller는 NUMA-blind로 동작. */
	}
}

/*
 * [한국어]
 * spdk_sock_get_numa_id - sock의 NIC가 속한 NUMA 노드 id를 반환.
 *
 * @return: NUMA id (>=0), 백엔드 미구현이면 SPDK_ENV_NUMA_ID_ANY.
 *
 * 동기: SPDK는 reactor를 특정 NUMA 노드의 코어에 고정하므로, sock이 어느 NUMA에 있는 NIC를
 * 사용하는지 알면 transport가 동일 NUMA의 reactor에 sock을 할당하여 NUMA cross-socket DMA를
 * 회피한다.
 */
int32_t
spdk_sock_get_numa_id(struct spdk_sock *sock)
{
	if (sock->net_impl->get_numa_id) {
		return sock->net_impl->get_numa_id(sock);       /* [한국어] 백엔드 위임. */
	} else {
		return SPDK_ENV_NUMA_ID_ANY;                    /* [한국어] "어느 NUMA든 OK" — 친화도 비활성. */
	}
}

/*
 * [한국어]
 * spdk_sock_get_impl_name - sock의 백엔드 이름(예: "posix", "uring", "ssl") 반환.
 *
 * RPC를 통해 운영자가 어떤 백엔드로 연결되었는지 확인하거나, 디버그 로그/통계에 사용.
 */
const char *
spdk_sock_get_impl_name(struct spdk_sock *sock)
{
	return sock->net_impl->name;                        /* [한국어] vtable 등록 시 정의한 정적 문자열. */
}

/*
 * [한국어]
 * spdk_sock_get_default_initialize_opts - spdk_sock_initialize_opts에 ABI-safe 기본값 채움.
 *
 * @opts: 호출자 할당 구조체 — opts_size 단위로 알려진 필드만 채움.
 * @opts_size: 호출자 sizeof(*opts) — 새 필드가 추가되어도 구버전 호출자 안전.
 *
 * 매크로 FIELD_OK는 opts_size에 해당 필드가 들어가는지 검사하고, SET_FIELD는 그때만 대입.
 * enable_interrupt_mode=true: epoll fd 기반 interrupt 모드를 기본 활성 — reactor가 idle 시
 * sleep 가능하게 한다(완전 polled-mode를 원하면 호출자가 false로 override).
 */
void
spdk_sock_get_default_initialize_opts(struct spdk_sock_initialize_opts *opts, size_t opts_size)
{
	assert(opts);                                      /* [한국어] NULL 방어 — 호출자 버그 검출. */

	opts->opts_size = opts_size;                       /* [한국어] 알려준 크기 그대로 보관 — 후속 ABI-safe 비교의 기준. */

#define FIELD_OK(field) \
	offsetof(struct spdk_sock_initialize_opts, field) + sizeof(opts->field) <= opts_size /* [한국어] opts_size 안에 field가 포함되는가? */

#define SET_FIELD(field, value) \
	if (FIELD_OK(field)) { \
		opts->field = value; \
	}                                                  /* [한국어] 안전 시점에만 대입. */

	SET_FIELD(enable_interrupt_mode, true);            /* [한국어] interrupt-mode 기본 ON. */

#undef FIELD_OK
#undef SET_FIELD
}

/*
 * [한국어]
 * spdk_sock_get_default_opts - 사용자 호출 직전의 spdk_sock_opts 기본값 세팅.
 *
 * @opts: 호출자 할당; opts->opts_size로 ABI 호환성을 표시해야 한다(API 계약).
 *
 * 각 필드:
 *   priority=0(SO_PRIORITY 미설정), zcopy=true(MSG_ZEROCOPY 활성), ack_timeout=0(TCP_USER_TIMEOUT
 *   미설정), impl_opts=NULL/impl_opts_size=0(백엔드별 추가 옵션 없음), src_addr=NULL/src_port=0
 *   (커널 자동 선택), connect_timeout=0(blocking connect는 무한 대기).
 */
void
spdk_sock_get_default_opts(struct spdk_sock_opts *opts)
{
	assert(opts);                                      /* [한국어] NULL 방어. */

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, priority)) {     /* [한국어] priority 필드가 호출자 ABI에 존재하는가? */
		opts->priority = SPDK_SOCK_DEFAULT_PRIORITY;   /* [한국어] = 0. */
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, zcopy)) {
		opts->zcopy = SPDK_SOCK_DEFAULT_ZCOPY;         /* [한국어] = true. */
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, ack_timeout)) {
		opts->ack_timeout = SPDK_SOCK_DEFAULT_ACK_TIMEOUT; /* [한국어] = 0(disabled). */
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, impl_opts)) {
		opts->impl_opts = NULL;                        /* [한국어] 백엔드별 추가 옵션 포인터 — caller가 sock_impl_set_opts 등으로 후속 설정. */
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, impl_opts_size)) {
		opts->impl_opts_size = 0;                      /* [한국어] impl_opts 크기 0 — 추가 옵션 없음. */
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, src_addr)) {
		opts->src_addr = NULL;                         /* [한국어] connect 시 자동 source IP 선택. */
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, src_port)) {
		opts->src_port = 0;                            /* [한국어] connect 시 ephemeral source port 사용. */
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, connect_timeout)) {
		opts->connect_timeout = SPDK_SOCK_DEFAULT_CONNECT_TIMEOUT; /* [한국어] = 0 → infinite. */
	}
}

/*
 * opts The opts allocated in the current library.
 * opts_user The opts passed by the caller.
 * */
/*
 * [한국어]
 * sock_init_opts - 사용자 opts(opts_user)를 라이브러리 내부 opts(opts)에 안전하게 복사.
 *
 * @opts: 라이브러리(호출 사이트)에서 stack에 할당한 spdk_sock_opts. 최신 ABI 기준 sizeof.
 * @opts_user: 호출자(transport 등)가 넘긴 opts — 구버전 헤더로 빌드된 호출자도 OK.
 *
 * 동작: 우선 opts->opts_size를 sizeof(*opts)(=라이브러리 ABI)로 설정 후 default 채움 → 그
 * 다음 opts->opts_size를 opts_user->opts_size(호출자 ABI)로 reduce하고, 그 ABI 안에 들어가는
 * 필드만 user 값으로 overwrite. 결과적으로 라이브러리 신규 필드는 default 유지, 사용자가
 * 알고 있는 필드는 user 값 적용 — backward compatibility 보장.
 *
 * 호출 체인: sock_connect_ext / spdk_sock_listen_ext → [이 함수] → spdk_sock_get_default_opts.
 */
static void
sock_init_opts(struct spdk_sock_opts *opts, struct spdk_sock_opts *opts_user)
{
	assert(opts);
	assert(opts_user);                                 /* [한국어] 두 포인터 모두 필수. */

	opts->opts_size = sizeof(*opts);                   /* [한국어] 일단 lib 전체 ABI로 set — default 채울 수 있게. */
	spdk_sock_get_default_opts(opts);                  /* [한국어] 모든 필드를 lib 기본값으로 채움. */

	/* reset the size according to the user */
	opts->opts_size = opts_user->opts_size;            /* [한국어] 이제 user ABI 크기로 줄이기 — 이후 FIELD_OK는 user 기준. */
	if (SPDK_SOCK_OPTS_FIELD_OK(opts, priority)) {
		opts->priority = opts_user->priority;          /* [한국어] user가 가진 필드만 overwrite. */
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, zcopy)) {
		opts->zcopy = opts_user->zcopy;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, ack_timeout)) {
		opts->ack_timeout = opts_user->ack_timeout;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, impl_opts)) {
		opts->impl_opts = opts_user->impl_opts;        /* [한국어] 백엔드별 추가 옵션 — 호출자 메모리 참조 (수명 caller 관리). */
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, impl_opts_size)) {
		opts->impl_opts_size = opts_user->impl_opts_size;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, src_addr)) {
		opts->src_addr = opts_user->src_addr;          /* [한국어] source IP 문자열 포인터 — caller 메모리 참조. */
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, src_port)) {
		opts->src_port = opts_user->src_port;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, connect_timeout)) {
		opts->connect_timeout = opts_user->connect_timeout;
	}
}

/*
 * [한국어]
 * spdk_sock_posix_getaddrinfo - "[v6]"/"v4" IP 문자열 + 포트로 getaddrinfo 결과를 만들어 반환.
 *
 * @ip: IPv4 dotted-quad 또는 IPv6 "[::1]" 표기. NULL이면 NULL 반환.
 * @port: 0~65535 포트 번호.
 * @return: getaddrinfo() addrinfo* (호출자가 freeaddrinfo 책임), 실패 시 NULL.
 *
 * 특징:
 *   - "[addr]" 표기를 만나면 대괄호 stripping → "addr"만 getaddrinfo에 넘김.
 *   - hints는 AI_NUMERICSERV(포트=숫자) | AI_PASSIVE(server bind 용 INADDR_ANY 패턴 허용)
 *     | AI_NUMERICHOST(IP 문자열만 받음, DNS 미사용) — DNS lookup 없는 빠른 변환.
 *   - PF_UNSPEC: v4/v6 모두 매칭.
 *
 * posix·uring·ssl 백엔드가 모두 공유하므로 공개 헤더 외부 API.
 */
struct addrinfo *
spdk_sock_posix_getaddrinfo(const char *ip, int port)
{
	struct addrinfo *res, hints = {};                   /* [한국어] res=결과 리스트, hints=요청 옵션(0 초기화). */
	char portnum[PORTNUMLEN];                           /* [한국어] port를 문자열로 변환 (getaddrinfo 인터페이스). */
	char buf[MAX_TMPBUF];                               /* [한국어] "[addr]" 처리용 임시 버퍼. */
	char *p;                                            /* [한국어] ']' 위치 검색용 포인터. */
	int rc;                                             /* [한국어] getaddrinfo 반환 코드 (EAI_*). */

	if (ip == NULL) {
		return NULL;                                    /* [한국어] 명백한 잘못된 입력. */
	}

	if (ip[0] == '[') {                                 /* [한국어] IPv6 bracketed 표기 처리. */
		snprintf(buf, sizeof(buf), "%s", ip + 1);       /* [한국어] '['는 건너뛴 사본 만들기. */
		p = strchr(buf, ']');                           /* [한국어] ']' 찾기. */
		if (p != NULL) {
			*p = '\0';                                  /* [한국어] ']'를 NUL로 변환 — 결과적으로 "::1" 같은 순수 IPv6 문자열. */
		}

		ip = (const char *) &buf[0];                    /* [한국어] 정제된 문자열을 ip로 가리킴 (스택 buf 수명: 함수 끝까지). */
	}

	snprintf(portnum, sizeof portnum, "%d", port);      /* [한국어] 정수 → 십진 문자열. */
	hints.ai_family = PF_UNSPEC;                        /* [한국어] v4/v6 모두 허용 — getaddrinfo가 입력 IP에 따라 결정. */
	hints.ai_socktype = SOCK_STREAM;                    /* [한국어] TCP. */
	hints.ai_flags = AI_NUMERICSERV;                    /* [한국어] portnum은 서비스 이름이 아닌 숫자 — /etc/services lookup 회피. */
	hints.ai_flags |= AI_PASSIVE;                       /* [한국어] ip=NULL이면 INADDR_ANY 매핑 (server bind용). */
	hints.ai_flags |= AI_NUMERICHOST;                   /* [한국어] DNS 사용 금지 — IP 문자열만 허용 (성능/예측성). */
	rc = getaddrinfo(ip, portnum, &hints, &res);        /* [한국어] glibc 변환 — sockaddr_storage 형태로 res 생성. */
	if (rc != 0) {
		SPDK_ERRLOG("getaddrinfo() failed %s (%d)\n", gai_strerror(rc), rc); /* [한국어] EAI_* 코드 기반 에러 메시지. */
		return NULL;
	}

	return res;                                         /* [한국어] caller가 freeaddrinfo 호출 책임. */
}

/*
 * [한국어]
 * spdk_sock_posix_fd_create - addrinfo + opts/impl_opts 기반으로 TCP socket fd를 만들고 공통 옵션 적용.
 *
 * @res: getaddrinfo() 결과 — ai_family/ai_socktype/ai_protocol 사용.
 * @opts: 사용자 sock 옵션 (priority, ack_timeout 사용).
 * @impl_opts: 백엔드별 옵션 (recv_buf_size, send_buf_size 사용).
 * @return: 생성된 fd (>=0), 실패 시 -errno.
 *
 * 적용되는 setsockopt:
 *   - SO_RCVBUF/SO_SNDBUF: 커널 소켓 버퍼 크기 (실패해도 비치명적 — 커널 default로 fallback)
 *   - SO_REUSEADDR: TIME_WAIT 상태 포트 재바인딩 허용 (server restart 시 필요)
 *   - TCP_NODELAY: Nagle 알고리즘 비활성 — small write를 즉시 송신 (NVMe-oF 명령 latency 단축)
 *   - SO_PRIORITY: SO_PRIORITY>0이면 qdisc 우선순위 큐 사용 (Linux only)
 *   - IPV6_V6ONLY: IPv6 socket이 IPv4-mapped 주소를 받지 않게 분리 — dual-stack 회피
 *   - TCP_USER_TIMEOUT: ack_timeout ms 지나도 ACK 없으면 dead peer로 간주 (Linux only)
 *
 * 호출 체인: posix/uring/ssl 백엔드의 connect/listen → [이 함수] → socket()/setsockopt().
 */
int
spdk_sock_posix_fd_create(struct addrinfo *res, struct spdk_sock_opts *opts,
			  struct spdk_sock_impl_opts *impl_opts)
{
	int fd;                                             /* [한국어] socket() 반환 fd. */
	int val = 1;                                        /* [한국어] setsockopt 부울 옵션의 "true" 값. */
	int rc, sz;                                         /* [한국어] rc=리턴 코드, sz=버퍼 크기. */
#if defined(__linux__)
	int to;                                             /* [한국어] TCP_USER_TIMEOUT 값 (Linux 전용). */
#endif

	fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol); /* [한국어] AF_INET/INET6, SOCK_STREAM, IPPROTO_TCP로 fd 생성. */
	if (fd < 0) {
		return -errno;                                  /* [한국어] EMFILE 등 에러 전파. */
	}

	sz = impl_opts->recv_buf_size;                      /* [한국어] 커널 RX 버퍼 byte 크기 (백엔드 옵션에서 가져옴). */
	rc = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)); /* [한국어] SO_RCVBUF 적용 — net.core.rmem_max 한도. */
	if (rc) {
		/* Not fatal */                                 /* [한국어] 권한/한도 초과여도 무시 — 커널 default 사용. */
	}

	sz = impl_opts->send_buf_size;                      /* [한국어] 커널 TX 버퍼 크기. */
	rc = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	if (rc) {
		/* Not fatal */
	}

	rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val); /* [한국어] TIME_WAIT 포트 재사용 허용 — 서버 재시작 시 필수. */
	if (rc < 0) {
		goto err;                                       /* [한국어] 재바인딩 못하면 치명적 — fd close 후 실패. */
	}

	rc = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof val); /* [한국어] Nagle 끄기 — NVMe-oF 작은 메시지 latency 단축. */
	if (rc < 0) {
		goto err;
	}

#if defined(SO_PRIORITY)
	if (opts->priority) {                               /* [한국어] priority>0인 경우만 적용 — 0은 default queue. */
		rc = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &opts->priority, sizeof val); /* [한국어] qdisc 우선순위 (skb->priority 매핑). */
		if (rc < 0) {
			goto err;
		}
	}
#endif

	if (res->ai_family == AF_INET6) {                   /* [한국어] IPv6 socket이면 IPv4-mapped 매핑 비활성. */
		rc = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof val); /* [한국어] dual-stack 자동 매핑 OFF — v4/v6 분리 운영. */
		if (rc < 0) {
			goto err;
		}
	}

	if (opts->ack_timeout) {                            /* [한국어] 사용자가 timeout>0 지정한 경우만. */
#if defined(__linux__)
		to = opts->ack_timeout;                         /* [한국어] uint64 → int 변환 (caller가 INT_MAX 검증함). */
		rc = setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &to, sizeof(to)); /* [한국어] ms 단위 — un-ACK 패킷이 to ms 넘게 보류되면 ETIMEDOUT. */
		if (rc < 0) {
			goto err;
		}
#else
		SPDK_WARNLOG("TCP_USER_TIMEOUT is not supported.\n"); /* [한국어] 비-Linux: 무시 (그러나 사용자 의도는 못 지킴). */
#endif
	}

	return fd;                                          /* [한국어] 성공 — fd 반환 (caller가 connect/bind에 사용). */

err:
	close(fd);                                          /* [한국어] 부분적 성공 시 fd 누수 방지. */
	return -errno;                                      /* [한국어] 마지막 setsockopt 에러 코드 그대로 반환. */
}

/*
 * [한국어]
 * sock_posix_fd_connect_poll - non-blocking connect 후 진행 상태를 확인 (POLLOUT or 에러).
 *
 * @fd: connect()가 EINPROGRESS를 반환한 fd.
 * @opts: connect_timeout 사용 (block=true 일 때만).
 * @block: true면 timeout 동안 대기, false면 한 번만 polling 후 즉시 반환 (async caller).
 * @return: 0(성공 — 연결 완료), -ETIMEDOUT(block 모드에서 시간 초과), -EAGAIN(non-block에서
 *          아직 미완료), -EIO(POLLERR/POLLHUP), -err(SO_ERROR로 얻은 connect 실패 코드).
 *
 * 동작: 1) poll(POLLOUT) — 연결 성공 또는 에러 시 깨어남. 2) timeout=0 처리: rc=0이면 block일
 * 때 ETIMEDOUT, async일 때 EAGAIN. 3) getsockopt(SO_ERROR)로 connect 실패 코드 추출 (poll가
 * "이벤트 있음"만 알려주고 성공/실패는 SO_ERROR로 구분). 4) revents에 POLLOUT 없으면 EIO.
 *
 * 동기: SPDK는 polled-mode이므로 connect()도 non-blocking으로 시작하여 별도 polling으로
 * 완료 검사한다 — reactor가 connect 대기 중에도 다른 sock I/O를 처리할 수 있게 한다.
 *
 * 호출 체인: sock_posix_fd_connect (block=true) / spdk_sock_posix_fd_connect_poll_async
 * (block=false) → [이 함수] → poll(), getsockopt(SO_ERROR).
 */
static int
sock_posix_fd_connect_poll(int fd, struct spdk_sock_opts *opts, bool block)
{
	int rc, err, timeout = 0;                           /* [한국어] rc=poll/getsockopt 결과, err=SO_ERROR, timeout=ms. */
	struct pollfd pfd = {.fd = fd, .events = POLLOUT};  /* [한국어] connect 완료는 POLLOUT으로 신호된다. */
	socklen_t len = sizeof(err);                        /* [한국어] getsockopt 출력 길이 in/out 변수. */

	if (opts && block) {
		assert(opts->connect_timeout <= INT_MAX);       /* [한국어] poll() ms 인자는 int — 호출자가 검증해야 함. */
		timeout = opts->connect_timeout ? (int)opts->connect_timeout : -1; /* [한국어] 0이면 -1(infinite), 양수면 ms. */
	}

	rc = poll(&pfd, 1, timeout);                        /* [한국어] 단일 fd polling — block=false면 timeout=0(즉시 반환). */
	if (rc < 0) {
		SPDK_ERRLOG("poll() failed, errno = %d\n", errno);
		return -errno;                                  /* [한국어] 시그널 등으로 EINTR/실제 에러 — caller가 처리. */
	}

	if (rc == 0) {                                      /* [한국어] timeout — 어떤 이벤트도 없음. */
		if (block) {
			SPDK_ERRLOG("poll() timeout after %d ms\n", timeout);
			return -ETIMEDOUT;                          /* [한국어] block 모드: 사용자가 정한 시간 초과 → 명시적 실패. */
		}

		return -EAGAIN;                                 /* [한국어] async 모드: caller가 다음 polling cycle에서 재시도. */
	}

	rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len); /* [한국어] connect 실패 사유를 SO_ERROR에서 추출. */
	if (rc < 0) {
		SPDK_ERRLOG("getsockopt() failed, errno = %d\n", errno);
		return -errno;
	}

	if (err) {
		SPDK_ERRLOG("connect() failed, err = %d\n", err);
		return -err;                                    /* [한국어] ECONNREFUSED 등 그대로 반환. */
	}

	if (!(pfd.revents & POLLOUT)) {                     /* [한국어] POLLOUT 미설정 — POLLERR/POLLHUP/POLLNVAL일 가능성. */
		SPDK_ERRLOG("poll() returned %d event(s) %s%s%sbut not POLLOUT\n", rc,
			    pfd.revents & POLLERR ? "POLLERR, " : "", pfd.revents & POLLHUP ? "POLLHUP, " : "",
			    pfd.revents & POLLNVAL ? "POLLNVAL, " : ""); /* [한국어] 어떤 비정상 이벤트인지 진단 출력. */
	}

	if (!(pfd.revents & POLLOUT)) {
		return -EIO;                                    /* [한국어] POLLOUT 없으면 EIO — 일반화된 I/O 에러. */
	}

	return 0;                                           /* [한국어] connect 성공 — caller가 사용 시작. */
}

/*
 * [한국어]
 * spdk_sock_posix_fd_connect_poll_async - async connect 진행 상태 한 번 검사 (non-block).
 *
 * @fd: connect()가 EINPROGRESS 반환한 fd.
 * @return: 0(완료), -EAGAIN(아직), 음수 errno(실패).
 *
 * SPDK reactor의 poller에서 주기적으로 호출되어 connect_async() 진행 모니터링.
 */
int
spdk_sock_posix_fd_connect_poll_async(int fd)
{
	return sock_posix_fd_connect_poll(fd, NULL, false); /* [한국어] block=false, opts=NULL — timeout=0 즉시 반환. */
}

/*
 * [한국어]
 * sock_posix_fd_connect - fd를 res로 (필요시 src_addr/src_port에 bind 후) connect한다.
 *
 * @fd: spdk_sock_posix_fd_create()로 생성된 fd.
 * @res: getaddrinfo() 결과 — 목적지 sockaddr.
 * @opts: src_addr/src_port/connect_timeout 사용 (ABI-safe SPDK_GET_FIELD 접근).
 * @block: true=connect 완료까지 대기, false=EINPROGRESS면 그대로 반환 (caller가 polling).
 * @return: 0(성공), 음수 errno (실패).
 *
 * 동작:
 *   1) fd를 일시적으로 blocking으로 만들어 bind() 실행 — bind는 비동기 의미가 없음.
 *   2) opts에 src_addr/src_port가 지정되면 그 주소로 bind (특정 NIC/포트로 라우팅 강제).
 *   3) fd를 다시 non-blocking으로 변경 — connect()가 즉시 반환되도록.
 *   4) connect() — 즉시 성공 또는 EINPROGRESS.
 *   5) block=true면 sock_posix_fd_connect_poll로 완료 대기 후 다시 blocking으로 복귀.
 *      block=false면 EINPROGRESS 상태로 반환 (caller가 connect_poll_async로 진행 검사).
 *
 * SPDK_GET_FIELD: opts_size 안에 필드가 있으면 값, 없으면 default 반환 — ABI-safe accessor.
 *
 * 호출 체인:
 *   spdk_sock_posix_fd_connect (block=true) / spdk_sock_posix_fd_connect_async (block=false)
 *     → [이 함수] → spdk_fd_clear/set_nonblock, bind, connect, sock_posix_fd_connect_poll
 */
static int
sock_posix_fd_connect(int fd, struct addrinfo *res, struct spdk_sock_opts *opts, bool block)
{
	char portnum[PORTNUMLEN];                           /* [한국어] src_port 문자열 변환용. */
	const char *src_addr;                               /* [한국어] 사용자가 지정한 source IP (bind 대상). */
	uint16_t src_port;                                  /* [한국어] 사용자가 지정한 source port. */
	struct addrinfo hints, *src_ai;                     /* [한국어] src_addr/src_port → sockaddr 변환용. */
	int rc;

	/* Socket address may be not assigned immediately during bind() and
	 * can return EINPROGRESS if function is invoked with O_NONBLOCK set. */
	rc = spdk_fd_clear_nonblock(fd);                    /* [한국어] bind를 blocking 모드로 — 비동기 의미 없음(주석 참조). */
	if (rc < 0) {
		return rc;
	}

	src_addr = SPDK_GET_FIELD(opts, src_addr, NULL, opts->opts_size); /* [한국어] ABI-safe로 src_addr 읽기. */
	src_port = SPDK_GET_FIELD(opts, src_port, 0, opts->opts_size);    /* [한국어] ABI-safe로 src_port 읽기. */
	if (src_addr != NULL || src_port != 0) {            /* [한국어] 명시적 source 주소 요청이 있을 때만 bind. */
		snprintf(portnum, sizeof(portnum), "%"PRIu16, src_port); /* [한국어] uint16 → 문자열. */
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;                    /* [한국어] v4/v6 모두. */
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_NUMERICSERV | AI_NUMERICHOST | AI_PASSIVE; /* [한국어] DNS 미사용, server-side bind 패턴 허용. */
		rc = getaddrinfo(src_addr, src_port > 0 ? portnum : NULL, &hints, &src_ai); /* [한국어] src 주소 변환. */
		if (rc != 0 || src_ai == NULL) {
			SPDK_ERRLOG("getaddrinfo() failed %s (%d)\n", rc != 0 ? gai_strerror(rc) : "", rc);
			return -EINVAL;
		}

		rc = bind(fd, src_ai->ai_addr, src_ai->ai_addrlen); /* [한국어] 특정 NIC/포트로 source 고정. */
		if (rc < 0) {
			SPDK_ERRLOG("bind() failed errno %d (%s:%s)\n", errno, src_addr ? src_addr : "", portnum);
			freeaddrinfo(src_ai);
			return -errno;
		}

		freeaddrinfo(src_ai);                           /* [한국어] addrinfo 메모리 해제. */
	}

	rc = spdk_fd_set_nonblock(fd);                      /* [한국어] connect()를 non-blocking으로 — EINPROGRESS 반환 허용. */
	if (rc < 0) {
		return rc;
	}

	rc = connect(fd, res->ai_addr, res->ai_addrlen);    /* [한국어] TCP 3-way handshake 시작. */
	if (rc < 0 && errno != EINPROGRESS) {
		SPDK_ERRLOG("connect() failed, errno = %d\n", errno);
		return -errno;                                  /* [한국어] 즉시 실패 (예: 라우팅 없음, 권한 등). */
	}

	if (!block) {
		return 0;                                       /* [한국어] async 모드 — caller가 connect_poll_async로 진행 검사. */
	}

	rc = sock_posix_fd_connect_poll(fd, opts, block);   /* [한국어] block 모드: connect_timeout 동안 완료 대기. */
	if (rc < 0) {
		return rc;
	}

	rc = spdk_fd_clear_nonblock(fd);                    /* [한국어] 호출자(blocking API 사용자) 기대대로 blocking으로 복귀. */
	if (rc < 0) {
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_sock_posix_fd_connect_async - non-blocking connect 시작 (즉시 반환).
 *
 * caller는 별도 poller에서 spdk_sock_posix_fd_connect_poll_async()로 완료 검사. SPDK reactor의
 * polled-mode와 매끄럽게 통합.
 */
int
spdk_sock_posix_fd_connect_async(int fd, struct addrinfo *res, struct spdk_sock_opts *opts)
{
	return sock_posix_fd_connect(fd, res, opts, false); /* [한국어] block=false. */
}

/*
 * [한국어]
 * spdk_sock_posix_fd_connect - blocking connect (connect_timeout까지 대기).
 *
 * 초기화 단계 등 polling이 어색한 컨텍스트에서 사용. 결과적으로 fd는 blocking 모드로 반환.
 */
int
spdk_sock_posix_fd_connect(int fd, struct addrinfo *res, struct spdk_sock_opts *opts)
{
	return sock_posix_fd_connect(fd, res, opts, true);  /* [한국어] block=true. */
}

/*
 * [한국어]
 * spdk_sock_connect - 간편한 동기 connect — default opts로 spdk_sock_connect_ext 호출.
 *
 * @ip/port: 목적지 (IPv4/IPv6 문자열).
 * @impl_name: "posix"/"uring"/"ssl" 등; NULL이면 g_default_impl.
 * @return: spdk_sock* 또는 NULL.
 */
struct spdk_sock *
spdk_sock_connect(const char *ip, int port, const char *impl_name)
{
	struct spdk_sock_opts opts;                         /* [한국어] 기본 opts 컨테이너. */

	opts.opts_size = sizeof(opts);                      /* [한국어] 현재 라이브러리 ABI 크기. */
	spdk_sock_get_default_opts(&opts);                  /* [한국어] 기본값 채우기. */
	return spdk_sock_connect_ext(ip, port, impl_name, &opts); /* [한국어] 위임. */
}

/*
 * [한국어]
 * sock_connect_ext - 백엔드 선택 후 connect (sync/async 공통 진입).
 *
 * @ip/port: 목적지.
 * @_impl_name: 명시 백엔드 이름 (NULL이면 g_default_impl 사용).
 * @opts: 사용자 옵션 (NULL 불가).
 * @async: true → impl->connect_async 호출 (cb_fn 콜백 등록), false → impl->connect (blocking).
 * @cb_fn/cb_arg: async 완료 콜백.
 * @return: 성공 시 sock 포인터; 실패 시 NULL.
 *
 * 백엔드 선택 우선순위: 인자 _impl_name > g_default_impl > (없으면 NULL → 매칭 실패).
 *
 * 후처리: opts_local을 sock->opts에 memcpy로 복사, impl_opts 포인터는 dangling 방지를 위해
 * NULL로 클리어 (caller의 stack-allocated impl_opts는 함수 반환 후 수명 종료). 이후 sock의
 * queued_reqs/pending_reqs TAILQ를 초기화 — writev_async 큐로 사용.
 *
 * 호출 체인: spdk_sock_connect_ext / spdk_sock_connect_async → [이 함수] →
 *             impl->connect[_async]() → 백엔드 → 커널 connect().
 */
static struct spdk_sock *
sock_connect_ext(const char *ip, int port, const char *_impl_name, struct spdk_sock_opts *opts,
		 bool async, spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	struct spdk_net_impl *impl = NULL;                  /* [한국어] 매칭된 백엔드 vtable. */
	struct spdk_sock *sock;                             /* [한국어] 백엔드가 반환한 sock. */
	struct spdk_sock_opts opts_local;                   /* [한국어] ABI-safe 변환된 내부 사본. */
	const char *impl_name = NULL;                       /* [한국어] 최종 매칭 대상 이름. */

	assert(async || (!cb_fn && !cb_arg));               /* [한국어] sync 모드에서 cb 인자가 있으면 호출자 버그. */

	if (opts == NULL) {
		SPDK_ERRLOG("the opts should not be NULL pointer\n");
		return NULL;                                    /* [한국어] opts 명시 강제 — ABI 호환을 위해 caller가 sizeof(*opts)를 알려야 함. */
	}

	if (_impl_name) {
		impl_name = _impl_name;                         /* [한국어] 1순위: 명시 인자. */
	} else if (g_default_impl) {
		impl_name = g_default_impl->name;               /* [한국어] 2순위: RPC sock_set_default_impl 결과. */
	}

	STAILQ_FOREACH_FROM(impl, &g_net_impls, link) {     /* [한국어] 등록된 백엔드 리스트 선형 탐색. */
		if (impl_name && strncmp(impl_name, impl->name, strlen(impl->name) + 1) == 0) { /* [한국어] +1로 NUL까지 비교 (prefix match 방지). */
			break;
		}
	}

	if (!impl) {
		SPDK_ERRLOG("Cannot find %s sock implementation\n", impl_name ? impl_name : "any");
		return NULL;                                    /* [한국어] 일치 백엔드 없음. */
	}

	if (async && !impl->connect_async) {
		SPDK_ERRLOG("Asynchronous connect is not supported by %s\n", impl->name);
		return NULL;                                    /* [한국어] 백엔드가 async slot을 제공하지 않음 — 예: 일부 ssl 변형. */
	}

	SPDK_DEBUGLOG(sock, "Creating a client socket using impl %s\n", impl->name);
	sock_init_opts(&opts_local, opts);                  /* [한국어] user opts → lib ABI 사본 (default 채우기 + ABI-safe 복사). */
	if (opts_local.connect_timeout > INT_MAX) {
		SPDK_ERRLOG("connect_timeout opt cannot exceed INT_MAX\n");
		return NULL;                                    /* [한국어] poll() ms 인자가 int이므로 안전 범위 검증. */
	}

	if (async) {
		sock = impl->connect_async(ip, port, &opts_local, cb_fn, cb_arg); /* [한국어] 비동기 connect — 완료 시 cb_fn(cb_arg, sock, status). */
	} else {
		sock = impl->connect(ip, port, &opts_local);    /* [한국어] 동기 connect — connect_timeout까지 blocking. */
	}

	if (!sock) {
		return NULL;                                    /* [한국어] 백엔드가 실패 보고. */
	}

	/* Copy the contents, both the two structures are the same ABI version */
	memcpy(&sock->opts, &opts_local, sizeof(sock->opts)); /* [한국어] 내부 ABI 사본을 sock->opts에 보존 — runtime get_opts 등에 사용. */
	/* Clear out impl_opts to make sure we don't keep reference to a dangling
	 * pointer */
	sock->opts.impl_opts = NULL;                        /* [한국어] caller 스택 포인터 dangling 방지 — sock 수명이 caller 스택보다 길 수 있음. */
	sock->net_impl = impl;                              /* [한국어] 이후 vtable dispatch의 기준이 됨. */
	TAILQ_INIT(&sock->queued_reqs);                     /* [한국어] writev_async 대기 큐 초기화. */
	TAILQ_INIT(&sock->pending_reqs);                    /* [한국어] sendmsg 직후 ack 대기 중인 req 큐 — zerocopy 완료 매칭에 사용. */
	return sock;
}

/*
 * [한국어]
 * spdk_sock_connect_ext - 사용자 opts와 함께 동기 connect.
 *
 * sock_connect_ext에 async=false로 위임. caller가 즉시 사용 가능한 sock을 받음.
 */
struct spdk_sock *
spdk_sock_connect_ext(const char *ip, int port, const char *_impl_name, struct spdk_sock_opts *opts)
{
	return sock_connect_ext(ip, port, _impl_name, opts, false, NULL, NULL); /* [한국어] async=false. */
}

/*
 * [한국어]
 * spdk_sock_connect_async - 비동기 connect — connect 완료 시 cb_fn 호출.
 *
 * 호출자(예: NVMe-oF host transport)는 즉시 sock을 받고 다음 reactor tick에 connect 완료
 * 콜백을 받는다. SPDK polled-mode와 매끄럽게 통합 — connect 대기에 reactor가 멈추지 않는다.
 */
struct spdk_sock *
spdk_sock_connect_async(const char *ip, int port, const char *_impl_name,
			struct spdk_sock_opts *opts, spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	return sock_connect_ext(ip, port, _impl_name, opts, true, cb_fn, cb_arg); /* [한국어] async=true. */
}

/*
 * [한국어]
 * spdk_sock_listen - 간편 listen — default opts로 spdk_sock_listen_ext 호출.
 */
struct spdk_sock *
spdk_sock_listen(const char *ip, int port, const char *impl_name)
{
	struct spdk_sock_opts opts;

	opts.opts_size = sizeof(opts);                      /* [한국어] ABI 크기 표시. */
	spdk_sock_get_default_opts(&opts);                  /* [한국어] 기본값. */
	return spdk_sock_listen_ext(ip, port, impl_name, &opts);
}

/*
 * [한국어]
 * spdk_sock_listen_ext - 백엔드 선택 후 listen socket 생성.
 *
 * @ip: 바인드 IP (NULL이면 INADDR_ANY 의도). @port: listen port. @_impl_name: 백엔드 이름.
 * @opts: 사용자 옵션. @return: listen sock 또는 NULL.
 *
 * connect와 차이점: queued_reqs/pending_reqs 초기화 안 함 — listen sock은 데이터 송수신을 하지
 * 않고 accept만 한다. 새 connection은 spdk_sock_accept()로 별도 sock 생성.
 */
struct spdk_sock *
spdk_sock_listen_ext(const char *ip, int port, const char *_impl_name, struct spdk_sock_opts *opts)
{
	struct spdk_net_impl *impl = NULL;                  /* [한국어] 매칭 백엔드. */
	struct spdk_sock *sock;
	struct spdk_sock_opts opts_local;
	const char *impl_name = NULL;

	if (opts == NULL) {
		SPDK_ERRLOG("the opts should not be NULL pointer\n");
		return NULL;
	}

	if (_impl_name) {
		impl_name = _impl_name;                         /* [한국어] 1순위: 명시 인자. */
	} else if (g_default_impl) {
		impl_name = g_default_impl->name;               /* [한국어] 2순위: 전역 default. */
	}

	STAILQ_FOREACH_FROM(impl, &g_net_impls, link) {
		if (impl_name && strncmp(impl_name, impl->name, strlen(impl->name) + 1) == 0) {
			break;
		}
	}

	if (!impl) {
		SPDK_ERRLOG("Cannot find %s sock implementation\n", impl_name ? impl_name : "any");
		return NULL;
	}

	SPDK_DEBUGLOG(sock, "Creating a listening socket using impl %s\n", impl->name);
	sock_init_opts(&opts_local, opts);                  /* [한국어] ABI-safe 사본. */
	sock = impl->listen(ip, port, &opts_local);         /* [한국어] 백엔드 listen — socket+bind+listen 수행. */
	if (!sock) {
		return NULL;
	}

	/* Copy the contents, both the two structures are the same ABI version */
	memcpy(&sock->opts, &opts_local, sizeof(sock->opts));
	/* Clear out impl_opts to make sure we don't keep reference to a dangling
	 * pointer */
	sock->opts.impl_opts = NULL;                        /* [한국어] dangling 포인터 방지. */
	sock->net_impl = impl;                              /* [한국어] vtable 연결. */
	/* Don't need to initialize the request queues for listen
	 * sockets. */                                      /* [한국어] listen sock은 read/writev_async 미사용 — 큐 불필요. */
	return sock;
}

/*
 * [한국어]
 * spdk_sock_accept - listen sock에서 새 connection을 수락 → 새 sock 반환.
 *
 * @sock: listen sock (spdk_sock_listen_ext 결과).
 * @return: 신규 connected sock (caller가 group_add_sock 등으로 등록), 또는 NULL(no connection
 *          /EAGAIN 등).
 *
 * 신규 sock은 listen sock의 opts(priority, zcopy 등)를 상속한다. 또한 net_impl도 동일.
 * queued_reqs/pending_reqs는 신규 sock이 데이터 송수신을 위해 사용하므로 새로 초기화.
 *
 * SPDK polled-mode에서는 typically reactor poller가 listen sock을 group에 등록하고 readable
 * event 시 이 함수를 호출하는 패턴 (NVMe-oF target accept 처리 등).
 */
struct spdk_sock *
spdk_sock_accept(struct spdk_sock *sock)
{
	struct spdk_sock *new_sock;                         /* [한국어] 새 connection sock. */

	new_sock = sock->net_impl->accept(sock);            /* [한국어] 백엔드 accept — 커널 accept4()로 fd 추출. */
	if (new_sock != NULL) {
		/* Inherit the opts from the "accept sock" */
		new_sock->opts = sock->opts;                    /* [한국어] 구조체 단위 복사 (먼저). */
		memcpy(&new_sock->opts, &sock->opts, sizeof(new_sock->opts)); /* [한국어] 안전 차원에서 memcpy로도 복사 — 동일 결과. */
		new_sock->net_impl = sock->net_impl;            /* [한국어] 같은 백엔드를 상속. */
		TAILQ_INIT(&new_sock->queued_reqs);             /* [한국어] writev_async 대기 큐 초기화. */
		TAILQ_INIT(&new_sock->pending_reqs);            /* [한국어] zerocopy ack 대기 큐. */
	}

	return new_sock;                                    /* [한국어] NULL이면 호출자가 재시도하거나 group epoll 이벤트 대기. */
}

/*
 * [한국어]
 * spdk_sock_close - sock 종료. 그룹에 속해있거나 cb_cnt>0이면 일부만 처리하고 지연 종료.
 *
 * @_sock: sock 더블 포인터; 성공 시 *_sock=NULL로 설정 (caller use-after-free 방지).
 * @return: 0(성공), -EBADF(NULL sock), -EBUSY(아직 group에 등록되어 있음).
 *
 * 동작:
 *   1) cb_fn != NULL: 아직 group_add_sock 상태 — caller가 먼저 group_remove_sock 해야 한다.
 *   2) flags.closed=true 설정 — 이후 spdk_sock_recv/writev 등이 EBADF로 거절.
 *   3) cb_cnt>0: 콜백이 진행 중이므로 그것들이 unwind한 후 destroyer가 close 마무리 (이중
 *      free/use-after-free 회피). 이 경우 0 반환하지만 실제 destroy는 미완료.
 *   4) 그 외: 큐에 남은 req들을 spdk_sock_abort_requests로 -ECANCELED와 함께 cb_fn 호출 →
 *      백엔드 close()로 fd 닫고 sock 메모리 해제.
 */
int
spdk_sock_close(struct spdk_sock **_sock)
{
	struct spdk_sock *sock = *_sock;                    /* [한국어] dereference. */

	if (sock == NULL) {
		return -EBADF;                                  /* [한국어] 잘못된 sock — caller가 이미 close 후 재호출 등. */
	}

	if (sock->cb_fn != NULL) {
		/* This sock is still part of a sock_group. */
		return -EBUSY;                                  /* [한국어] group_remove_sock 먼저 필요 — cb_fn은 group_add_sock에서 set됨. */
	}

	/* Beyond this point the socket is considered closed. */
	*_sock = NULL;                                      /* [한국어] caller 포인터 즉시 NULL — 사용자 use-after-free 방어. */

	sock->flags.closed = true;                          /* [한국어] 이후 모든 read/writev API가 EBADF로 reject (race-safe flag). */

	if (sock->cb_cnt > 0) {
		/* Let the callback unwind before destroying the socket */
		return 0;                                       /* [한국어] 진행 중인 콜백이 끝나면 그쪽 코드가 close 마무리 — 재진입 안전. */
	}

	spdk_sock_abort_requests(sock);                     /* [한국어] queued/pending req들에 -ECANCELED cb_fn 호출 — 메모리 누수 방지. */

	return sock->net_impl->close(sock);                 /* [한국어] 백엔드 close — fd close + sock 메모리 free. */
}

/*
 * [한국어]
 * spdk_sock_recv - 동기(non-blocking) recv. 한 버퍼에 최대 len byte 읽기.
 *
 * @return: 읽은 byte (>=0), 실패 시 -errno (NULL/closed면 -EBADF).
 *
 * recv는 EAGAIN을 음수로 반환할 수 있음 — caller가 group_poll로 다시 시도하는 패턴.
 */
ssize_t
spdk_sock_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	if (sock == NULL || sock->flags.closed) {
		return -EBADF;                                  /* [한국어] race-safe close 검사. */
	}

	return sock->net_impl->recv(sock, buf, len);        /* [한국어] 백엔드 위임 — read()/SSL_read()/io_uring 등. */
}

/*
 * [한국어]
 * spdk_sock_readv - scatter-read (iovec 다수 버퍼로 동시 읽기).
 *
 * NVMe-oF capsule(헤더 + 데이터 SGL) 같은 멀티-segment 수신에 사용 — 커널 readv() syscall 한 번으로 처리.
 */
ssize_t
spdk_sock_readv(struct spdk_sock *sock, struct iovec *iov, int iovcnt)
{
	if (sock == NULL || sock->flags.closed) {
		return -EBADF;
	}

	return sock->net_impl->readv(sock, iov, iovcnt);    /* [한국어] 백엔드 readv 위임. */
}

/*
 * [한국어]
 * spdk_sock_writev - 동기 gather-write (iovec 다수 → 한 번 송신).
 *
 * NVMe-oF response capsule 송신 등에 사용. 부분 write 가능 (반환값 < total) — caller가 다음
 * 호출에서 남은 부분 송신.
 */
ssize_t
spdk_sock_writev(struct spdk_sock *sock, struct iovec *iov, int iovcnt)
{
	if (sock == NULL || sock->flags.closed) {
		return -EBADF;
	}

	return sock->net_impl->writev(sock, iov, iovcnt);   /* [한국어] writev() syscall 위임. */
}

/*
 * [한국어]
 * spdk_sock_writev_async - 비동기 송신 — req(cb_fn 포함)를 백엔드 큐에 enqueue.
 *
 * @sock: 대상 sock; closed면 즉시 cb_fn을 -EBADF와 함께 호출 (caller가 폴링 없이도 알게).
 * @req: spdk_sock_request — iovec, cb_fn, cb_arg를 포함한 요청 객체. cb_fn은 송신 완료 시
 *       sock affinity spdk_thread 컨텍스트에서 호출된다 (lockless 근간).
 *
 * 동기: SPDK의 핵심 송신 경로. iovec을 즉시 sendmsg로 시도하고, 부분 송신/EAGAIN이면 sock의
 * queued_reqs STAILQ에 추가해 다음 reactor poll 사이클에서 재시도. 송신 완료 후 백엔드가
 * pending_reqs에 옮기고(zerocopy의 경우 ack 대기), 최종 완료 시 cb_fn 호출.
 *
 * cb_fn 컨텍스트: sock이 등록된 group의 reactor — 사용자 콜백 안에서 동일 sock에 대한 추가
 * writev_async 호출 안전 (재진입 안전 — 단일 thread 보장).
 *
 * 호출 체인: NVMe-oF tcp transport / iSCSI initiator/target → [이 함수] →
 *             impl->writev_async() → 백엔드 큐 → poll → sendmsg → cb_fn.
 */
void
spdk_sock_writev_async(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	assert(req->cb_fn != NULL);                         /* [한국어] async 모드에서 cb_fn 없으면 송신 결과를 알 방법이 없음. */

	if (sock == NULL || sock->flags.closed) {
		req->cb_fn(req->cb_arg, -EBADF);                /* [한국어] 즉시 실패 콜백 — caller가 cleanup 가능. */
		return;
	}

	sock->net_impl->writev_async(sock, req);            /* [한국어] 백엔드 위임 — req를 sock->queued_reqs에 enqueue. */
}

/*
 * [한국어]
 * spdk_sock_recv_next - buffer-providing 모델로 다음 RX 데이터 청크를 가져온다.
 *
 * @buf: 백엔드가 RX한 데이터 버퍼 포인터 (caller가 미리 group->pool에 provide_buf로 등록한 것).
 * @ctx: provide_buf 시 등록한 ctx (caller 자료구조 참조).
 * @return: 0(다음 데이터 가져옴), -ENOTSUP(group 미등록), -EAGAIN(데이터 없음), -EBADF(closed).
 *
 * 동기: zerocopy RX — caller가 미리 buffer를 풀에 등록(provide_buf)하고 백엔드는 RX 시 그
 * buffer로 직접 read하므로 추가 memcpy 없음. NVMe-oF tcp 응답 데이터 수신 등에 활용.
 *
 * group_impl == NULL이면 zerocopy RX는 group polling 메커니즘에 의존하므로 -ENOTSUP.
 */
int
spdk_sock_recv_next(struct spdk_sock *sock, void **buf, void **ctx)
{
	if (sock == NULL || sock->flags.closed) {
		return -EBADF;
	}

	if (sock->group_impl == NULL) {
		return -ENOTSUP;                                /* [한국어] standalone sock(group 미등록) — buffer-providing 모드 미지원. */
	}

	return sock->net_impl->recv_next(sock, buf, ctx);   /* [한국어] 백엔드 위임 — group->pool에서 buf 꺼내서 데이터 채워 반환. */
}

/*
 * [한국어]
 * spdk_sock_flush - 큐에 남은 송신 데이터를 즉시 전송 시도.
 *
 * 동기: writev_async로 enqueue된 후 group poller가 호출되기 전에 caller가 명시적으로
 * "지금 송신해라"를 요청 (예: latency-critical request). 백엔드는 sendmsg 한 번 추가 호출.
 */
int
spdk_sock_flush(struct spdk_sock *sock)
{
	if (sock == NULL || sock->flags.closed) {
		return -EBADF;
	}

	return sock->net_impl->flush(sock);                 /* [한국어] 백엔드 flush — queued_reqs를 sendmsg로 즉시 push. */
}

/*
 * [한국어]
 * spdk_sock_set_recvlowat - SO_RCVLOWAT 설정 (recv 시 깨어날 최소 byte 수).
 *
 * 백엔드가 setsockopt(SOL_SOCKET, SO_RCVLOWAT, nbytes) 호출 — 작은 read 호출 횟수 감소.
 */
int
spdk_sock_set_recvlowat(struct spdk_sock *sock, int nbytes)
{
	return sock->net_impl->set_recvlowat(sock, nbytes); /* [한국어] vtable dispatch. */
}

/*
 * [한국어]
 * spdk_sock_set_recvbuf - SO_RCVBUF 동적 변경 (런타임).
 *
 * connect 시 spdk_sock_posix_fd_create에서 set한 후에도 트래픽 패턴 변화에 따라 조정 가능.
 */
int
spdk_sock_set_recvbuf(struct spdk_sock *sock, int sz)
{
	return sock->net_impl->set_recvbuf(sock, sz);
}

/*
 * [한국어]
 * spdk_sock_set_sendbuf - SO_SNDBUF 동적 변경.
 *
 * 대량 응답을 송신하기 직전 늘려서 sendmsg가 EAGAIN으로 회귀하는 빈도 감소시킬 때 유용.
 */
int
spdk_sock_set_sendbuf(struct spdk_sock *sock, int sz)
{
	return sock->net_impl->set_sendbuf(sock, sz);
}

/*
 * [한국어]
 * spdk_sock_is_ipv6 - sock의 family가 AF_INET6인지 검사.
 *
 * 백엔드가 sa_family 비교로 결정. NVMe-oF target이 listen 주소 종류를 보고하는 등에 사용.
 */
bool
spdk_sock_is_ipv6(struct spdk_sock *sock)
{
	return sock->net_impl->is_ipv6(sock);
}

/*
 * [한국어]
 * spdk_sock_is_ipv4 - sock의 family가 AF_INET인지 검사.
 */
bool
spdk_sock_is_ipv4(struct spdk_sock *sock)
{
	return sock->net_impl->is_ipv4(sock);
}

/*
 * [한국어]
 * spdk_sock_is_connected - 현재 fd가 연결된 상태인지 확인 (TCP keep-alive 검사 등).
 *
 * 백엔드는 typically getpeername/getsockopt(SO_ERROR)을 사용하거나 reflective recv(0 byte)로
 * 결정. 끊긴 connection을 빠르게 감지해서 cleanup.
 */
bool
spdk_sock_is_connected(struct spdk_sock *sock)
{
	return sock->net_impl->is_connected(sock);
}

/*
 * [한국어]
 * _sock_fd_group_fn - fd_group이 등록된 fd가 readable일 때 호출되는 더미 핸들러.
 *
 * @return: 0 (이벤트 처리됨).
 *
 * 동기: spdk_fd_group_add는 fd마다 콜백을 요구하지만, sock_group의 경우 epoll wait는 그냥
 * "어떤 sock에 이벤트 있다"는 신호만 받으면 충분(실제 처리는 spdk_sock_group_poll에서).
 * 따라서 콜백은 do-nothing — 실제 polling 로직은 group_poll → group_impl_poll로 이루어짐.
 */
static int
_sock_fd_group_fn(void *ctx)
{
	/* Do nothing. The fd group is only used to get the fd. */
	return 0;                                           /* [한국어] 0 = 정상 처리 (fd_group이 추가 sleep 진입 가능). */
}

/*
 * [한국어]
 * spdk_sock_group_create - 다수 sock을 묶어 단일 polling/epoll 단위로 관리할 group 생성.
 *
 * @ctx: caller가 group과 함께 보관할 컨텍스트 포인터 (예: NVMe-oF tgrp 포인터).
 * @return: 성공 시 group 포인터, 실패 시 NULL.
 *
 * 동작:
 *   1) group 자체 calloc + group_impls(백엔드별 sub-group) STAILQ 초기화.
 *   2) pool(buffer-providing pool — recv_next 모델용) STAILQ 초기화.
 *   3) interrupt-mode 활성 시 fd_group 생성 — epoll fd를 모아 interrupt-mode poller가 사용.
 *   4) 등록된 모든 백엔드(g_net_impls)마다 group_impl_create() 호출 — 백엔드는 자기 epoll
 *      fd 또는 io_uring을 만들어 group_impl로 반환. 이걸 group->group_impls에 매단다.
 *   5) 각 group_impl의 interruptfd(epoll fd)를 fd_group에 add — interrupt 모드에서 reactor가
 *      epoll_wait에 들어갈 fd 모음.
 *
 * 실행 컨텍스트: 한 spdk_thread(reactor)에서 호출. 이후 group은 그 reactor에 fixed.
 *
 * 호출 체인: NVMe-oF transport 초기화 / iSCSI poll group 생성 → [이 함수] →
 *             impl->group_impl_create() (per-backend) → spdk_fd_group_create.
 */
struct spdk_sock_group *
spdk_sock_group_create(void *ctx)
{
	struct spdk_net_impl *impl = NULL;                  /* [한국어] 등록된 백엔드 순회용. */
	struct spdk_sock_group *group;                      /* [한국어] 새로 만들 group. */
	struct spdk_sock_group_impl *group_impl;            /* [한국어] 백엔드별 sub-group. */
	int rc, fd;                                         /* [한국어] rc=리턴, fd=interruptfd. */

	group = calloc(1, sizeof(*group));                  /* [한국어] zero-init alloc — 모든 STAILQ 헤드 0 시작. */
	if (group == NULL) {
		return NULL;
	}

	STAILQ_INIT(&group->group_impls);                   /* [한국어] 백엔드별 group_impl 리스트 초기화. */
	STAILQ_INIT(&group->pool);                          /* [한국어] buffer-providing pool 초기화 (provide_buf로 채움). */

	if (g_init_opts.enable_interrupt_mode) {            /* [한국어] interrupt 모드면 fd_group 준비. */
		rc = spdk_fd_group_create(&group->fgrp);        /* [한국어] epoll fd 묶음 생성. */
		if (rc != 0) {
			free(group);
			return NULL;
		}
	}

	STAILQ_FOREACH_FROM(impl, &g_net_impls, link) {     /* [한국어] 등록된 모든 백엔드 순회. */
		group_impl = impl->group_impl_create();         /* [한국어] 각 백엔드가 자기 polling fd(epoll/uring) 생성. */
		if (group_impl != NULL) {
			STAILQ_INSERT_TAIL(&group->group_impls, group_impl, link); /* [한국어] tail에 매달기 — 순서 == g_net_impls 순서. */
			TAILQ_INIT(&group_impl->socks);             /* [한국어] 이 sub-group에 등록된 sock 리스트. */
			group_impl->net_impl = impl;                /* [한국어] back-pointer. */
			group_impl->group = group;                  /* [한국어] parent group 포인터. */

			if (g_init_opts.enable_interrupt_mode && impl->group_impl_get_interruptfd != NULL) { /* [한국어] interrupt 지원 백엔드만. */
				fd = impl->group_impl_get_interruptfd(group_impl); /* [한국어] 백엔드의 epoll fd 추출. */
				if (fd <= 0) {
					assert(false);                      /* [한국어] interruptfd 0/음수는 백엔드 버그 — debug 빌드에서 즉시 abort. */
					spdk_sock_group_close(&group);
					return NULL;
				}

				rc = spdk_fd_group_add(group->fgrp, fd, _sock_fd_group_fn, NULL, impl->name); /* [한국어] fd_group에 등록 — reactor가 epoll_wait에서 깨어날 fd. */
				if (rc != 0) {
					assert(false);
					spdk_sock_group_close(&group);
					return NULL;
				}
			}
		}
	}

	group->ctx = ctx;                                   /* [한국어] caller 컨텍스트 보관 — get_ctx로 다시 추출. */

	return group;                                       /* [한국어] caller가 add_sock/poll_count로 사용. */
}

/*
 * [한국어]
 * spdk_sock_group_get_ctx - group_create에서 등록한 ctx 회수.
 *
 * NVMe-oF 콜백 등에서 group → tgrp 같은 캐스팅 대신 ctx로 추적할 때 사용.
 */
void *
spdk_sock_group_get_ctx(struct spdk_sock_group *group)
{
	if (group == NULL) {
		return NULL;                                    /* [한국어] NULL group → NULL ctx. */
	}

	return group->ctx;                                  /* [한국어] caller 등록한 그대로. */
}

/*
 * [한국어]
 * spdk_sock_group_add_sock - sock을 group에 등록하여 group_poll에서 함께 polling되도록.
 *
 * @group: 대상 group.
 * @sock: 등록할 sock (이미 등록되어 있으면 -EINVAL).
 * @cb_fn: 이 sock에 readable 이벤트가 발생하면 호출될 콜백 (group_poll → cb_fn).
 * @cb_arg: cb_fn의 첫 인자.
 * @return: 0(성공), -EINVAL(cb_fn NULL / 이미 등록 / 백엔드 불일치), 백엔드별 에러.
 *
 * 동작:
 *   1) cb_fn 필수 검사 (없으면 readable 이벤트 처리 불가).
 *   2) 중복 등록 검사 (group_impl 이미 set이면 거부).
 *   3) sock의 백엔드(net_impl)와 매칭되는 group_impl 찾기.
 *   4) 백엔드 group_impl_add_sock 호출 — 보통 epoll_ctl(EPOLL_CTL_ADD).
 *   5) group_impl->socks TAILQ에 매달고 sock에 cb_fn/cb_arg/group_impl 기록.
 *
 * 이후 spdk_sock_group_poll(group)이 readable sock을 발견하면 sock->cb_fn(cb_arg, group, sock)
 * 호출. cb_fn은 그 sock의 reactor 컨텍스트에서 실행되므로 lockless.
 */
int
spdk_sock_group_add_sock(struct spdk_sock_group *group, struct spdk_sock *sock,
			 spdk_sock_cb cb_fn, void *cb_arg)
{
	struct spdk_sock_group_impl *group_impl = NULL;     /* [한국어] 매칭 group_impl. */
	int rc;

	if (cb_fn == NULL) {
		return -EINVAL;                                 /* [한국어] cb_fn 필수 — 이벤트 처리 주체가 없음. */
	}

	if (sock->group_impl != NULL) {
		/*
		 * This sock is already part of a sock_group.
		 */
		return -EINVAL;                                 /* [한국어] 한 sock은 하나의 group에만 속함. */
	}

	group_impl = sock_get_group_impl_from_group(sock, group); /* [한국어] sock의 백엔드와 매칭. */
	if (group_impl == NULL) {
		return -EINVAL;                                 /* [한국어] group이 sock의 백엔드를 지원하지 않음. */
	}

	rc = group_impl->net_impl->group_impl_add_sock(group_impl, sock); /* [한국어] 백엔드 위임 — epoll_ctl(EPOLL_CTL_ADD) 등. */
	if (rc != 0) {
		return rc;                                      /* [한국어] EBADF/ENOMEM 등 백엔드 에러 그대로. */
	}

	TAILQ_INSERT_TAIL(&group_impl->socks, sock, link);  /* [한국어] sub-group의 sock 리스트에 등록. */
	sock->group_impl = group_impl;                      /* [한국어] back-pointer — recv_next 등에서 사용. */
	sock->cb_fn = cb_fn;                                /* [한국어] readable 이벤트 콜백. */
	sock->cb_arg = cb_arg;                              /* [한국어] cb_fn의 첫 인자(transport ctx 등). */
	return 0;
}

/*
 * [한국어]
 * spdk_sock_group_remove_sock - group에서 sock 제거 (close 전 필수 단계).
 *
 * @return: 0(성공), -EINVAL(group과 sock의 백엔드 불일치), 백엔드별 에러.
 *
 * 백엔드의 group_impl_remove_sock 호출 (epoll_ctl EPOLL_CTL_DEL) 후 TAILQ 분리 + cb_fn=NULL.
 * cb_fn=NULL 표시는 spdk_sock_close가 group에 남아있는지 검사하는 기준이기도 함.
 */
int
spdk_sock_group_remove_sock(struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_sock_group_impl *group_impl = NULL;     /* [한국어] 매칭 group_impl. */
	int rc;

	group_impl = sock_get_group_impl_from_group(sock, group);
	if (group_impl == NULL) {
		return -EINVAL;
	}

	assert(group_impl == sock->group_impl);             /* [한국어] sock이 정말 이 group에 등록되어 있는지 일치 검증. */

	rc = group_impl->net_impl->group_impl_remove_sock(group_impl, sock); /* [한국어] 백엔드 — epoll_ctl(EPOLL_CTL_DEL). */
	if (rc == 0) {
		TAILQ_REMOVE(&group_impl->socks, sock, link);   /* [한국어] sub-group sock 리스트에서 분리. */
		sock->group_impl = NULL;                        /* [한국어] back-pointer 클리어. */
		sock->cb_fn = NULL;                             /* [한국어] close 가능 상태로 마킹. */
		sock->cb_arg = NULL;
	}

	return rc;
}

/*
 * [한국어]
 * spdk_sock_group_provide_buf - group의 buffer pool에 buf를 제공 (zerocopy RX 모델).
 *
 * @group: 대상 group; pool은 STAILQ_HEAD.
 * @buf: 버퍼 포인터 (시작 부분에 spdk_sock_group_provided_buf 헤더가 들어감 — in-place 저장).
 * @len: buf 크기.
 * @ctx: 사용자 컨텍스트 (recv_next 시 함께 회수).
 * @return: 0 (현재는 항상 성공).
 *
 * 동기: caller가 미리 RX용 버퍼들을 풀에 넣어두고, recv_next() 시 백엔드가 풀에서 꺼내
 * 직접 read한다 — 추가 memcpy 없이 zerocopy. NVMe-oF tcp의 응답 데이터 수신 등에 활용.
 *
 * 헤더는 buf의 첫 부분을 덮어쓰므로 caller는 본 데이터 영역을 그만큼 미리 확보해야 한다.
 */
int
spdk_sock_group_provide_buf(struct spdk_sock_group *group, void *buf, size_t len, void *ctx)
{
	struct spdk_sock_group_provided_buf *provided;      /* [한국어] in-place 헤더 캐스팅. */

	provided = (struct spdk_sock_group_provided_buf *)buf; /* [한국어] buf 시작 주소를 헤더로 재해석. */

	provided->len = len;                                /* [한국어] 사용 가능 길이 기록. */
	provided->ctx = ctx;                                /* [한국어] caller 식별자. */
	STAILQ_INSERT_HEAD(&group->pool, provided, link);   /* [한국어] LIFO 삽입 — cache 친화적(가장 최근 free된 buffer를 먼저 사용). */

	return 0;
}

/*
 * [한국어]
 * spdk_sock_group_get_buf - pool에서 buf 한 개 꺼내 caller에게 반환 (백엔드 recv_next 내부에서 사용).
 *
 * @return: buffer 길이 (>0), 풀이 비어있으면 0 (그 경우 *buf=NULL).
 *
 * provide_buf가 LIFO이므로 LRU 순으로 cache hot한 버퍼부터 사용. caller(백엔드)가 데이터를
 * 그 buf에 read한 뒤 spdk_sock_recv_next로 사용자에게 전달.
 */
size_t
spdk_sock_group_get_buf(struct spdk_sock_group *group, void **buf, void **ctx)
{
	struct spdk_sock_group_provided_buf *provided;      /* [한국어] head 추출. */

	provided = STAILQ_FIRST(&group->pool);              /* [한국어] LIFO에서 head = 가장 최근 추가. */
	if (provided == NULL) {
		*buf = NULL;
		return 0;                                       /* [한국어] pool empty — caller가 보조 alloc 등으로 fallback. */
	}
	STAILQ_REMOVE_HEAD(&group->pool, link);             /* [한국어] 풀에서 분리 — 단일 thread 가정으로 lock 불필요. */

	*buf = provided;                                    /* [한국어] caller가 헤더 포함 buf 그대로 사용 가능. */
	*ctx = provided->ctx;                               /* [한국어] provide 시 등록한 ctx 회수. */
	return provided->len;                               /* [한국어] caller에게 사용 가능 길이 알림. */
}

/*
 * [한국어]
 * spdk_sock_group_poll - group에 등록된 sock들에 대해 readable 이벤트 한 사이클 polling.
 *
 * MAX_EVENTS_PER_POLL(=32) 한도로 spdk_sock_group_poll_count 호출.
 *
 * SPDK reactor의 poller로 등록되어 매 reactor tick마다 호출되는 패턴.
 */
int
spdk_sock_group_poll(struct spdk_sock_group *group)
{
	return spdk_sock_group_poll_count(group, MAX_EVENTS_PER_POLL); /* [한국어] 기본 batch 크기 32. */
}

/*
 * [한국어]
 * sock_group_impl_poll_count - 단일 백엔드 sub-group의 epoll/uring polling 한 사이클.
 *
 * @group_impl: 대상 백엔드 sub-group.
 * @group: parent group (콜백 인자에 전달).
 * @max_events: 최대 수확 이벤트 수.
 * @return: 처리한 이벤트 수 (>=0), 음수=백엔드 에러.
 *
 * 동작:
 *   1) sub-group sock 리스트가 비면 0 즉시 반환 (epoll 호출 회피).
 *   2) 백엔드 group_impl_poll 호출 — epoll_wait/io_uring_peek_batch_cqe로 readable sock 추출.
 *   3) 각 sock에 대해 cb_fn(cb_arg, group, sock) 호출 — caller가 등록한 처리 콜백.
 *
 * cb_fn은 같은 reactor 컨텍스트에서 실행되므로 lockless. cb_fn 내부에서 spdk_sock_recv 등으로
 * 데이터 추출 → caller protocol 파싱 → 응답 송신 패턴.
 */
static int
sock_group_impl_poll_count(struct spdk_sock_group_impl *group_impl,
			   struct spdk_sock_group *group,
			   int max_events)
{
	struct spdk_sock *socks[MAX_EVENTS_PER_POLL];       /* [한국어] readable sock 출력 버퍼 (스택 32개). */
	int num_events, i, rc;

	if (TAILQ_EMPTY(&group_impl->socks)) {
		return 0;                                       /* [한국어] sub-group이 비면 epoll_wait 호출 회피 — 매 tick 비용 절감. */
	}

	rc = group_impl->net_impl->group_impl_poll(group_impl, max_events, socks); /* [한국어] 백엔드 위임 — 실제 epoll_wait/io_uring. */
	if (rc < 0) {
		return rc;                                      /* [한국어] 에러 전파. */
	}

	num_events = rc;
	for (i = 0; i < num_events; i++) {
		struct spdk_sock *sock = socks[i];
		assert(sock->cb_fn != NULL);                    /* [한국어] add_sock에서 보장 — group에 있는 sock은 cb_fn 보유. */
		sock->cb_fn(sock->cb_arg, group, sock);         /* [한국어] caller 콜백 — 같은 reactor 스레드 (lockless). */
	}

	return num_events;
}

/*
 * [한국어]
 * spdk_sock_group_poll_count - group의 모든 백엔드 sub-group에 대해 polling.
 *
 * @max_events: caller가 원하는 최대 처리 수 — 32 한도로 cap.
 * @return: 모든 sub-group에서 처리한 이벤트 합계, 한 sub-group이라도 실패하면 -1.
 *
 * 동작: group->group_impls(예: posix sub-group, ssl sub-group, …)를 순회하며 각각 polling.
 * 백엔드별로 별도 epoll fd를 가지므로 이벤트가 분리되어 있다 — 단일 group이지만 백엔드별
 * polling 비용은 합산된다.
 */
int
spdk_sock_group_poll_count(struct spdk_sock_group *group, int max_events)
{
	struct spdk_sock_group_impl *group_impl = NULL;     /* [한국어] 순회용. */
	int rc, num_events = 0;                             /* [한국어] 누적 카운터. */

	if (max_events < 1) {
		return -EINVAL;                                 /* [한국어] 잘못된 인자. */
	}

	/*
	 * Only poll for up to 32 events at a time - if more events are pending,
	 *  the next call to this function will reap them.
	 */
	if (max_events > MAX_EVENTS_PER_POLL) {
		max_events = MAX_EVENTS_PER_POLL;               /* [한국어] 스택 socks[] 배열 크기와 정렬 — buffer overrun 방지. */
	}

	STAILQ_FOREACH_FROM(group_impl, &group->group_impls, link) { /* [한국어] 모든 sub-group(=백엔드별 epoll). */
		rc = sock_group_impl_poll_count(group_impl, group, max_events);
		if (rc < 0) {
			num_events = -1;                            /* [한국어] sticky failure marker — 한 백엔드라도 실패하면 -1. */
			SPDK_ERRLOG("group_impl_poll_count for net(%s) failed\n",
				    group_impl->net_impl->name);
		} else if (num_events >= 0) {                   /* [한국어] 아직 sticky failure 아니면 누적. */
			num_events += rc;
		}
	}

	return num_events;                                  /* [한국어] caller(reactor poller)는 양수면 "이벤트 처리됨" 시그널로 사용. */
}

/*
 * [한국어]
 * spdk_sock_group_close - group 해제 — 등록된 sock이 없어야 성공.
 *
 * @return: 0(성공), -EBADF(NULL), -EBUSY(아직 sock 등록되어 있음).
 *
 * 동작:
 *   1) 모든 sub-group을 미리 스캔하여 남은 sock 있으면 -EBUSY (caller가 모든 remove_sock 후
 *      재호출).
 *   2) interrupt-mode면 sub-group의 interruptfd를 fd_group에서 제거.
 *   3) 백엔드 group_impl_close 호출 (epoll fd close 등).
 *   4) fd_group destroy 후 group memory free.
 */
int
spdk_sock_group_close(struct spdk_sock_group **_group)
{
	struct spdk_sock_group_impl *group_impl = NULL, *tmp; /* [한국어] FOREACH_SAFE 임시. */
	struct spdk_sock_group *group;
	int rc, fd;

	if (_group == NULL || (*_group) == NULL) {
		return -EBADF;                                  /* [한국어] 잘못된 호출 — 이미 close된 group. */
	}

	group = *_group;

	STAILQ_FOREACH_SAFE(group_impl, &group->group_impls, link, tmp) { /* [한국어] 1단계: 사용 중인 sock 검사 (state-modifying 금지). */
		if (!TAILQ_EMPTY(&group_impl->socks)) {
			return -EBUSY;                              /* [한국어] 아직 sock 등록되어 있음 — caller가 정리 후 재호출. */
		}
	}

	STAILQ_FOREACH_SAFE(group_impl, &group->group_impls, link, tmp) { /* [한국어] 2단계: 실제 cleanup. */
		if (g_init_opts.enable_interrupt_mode && group_impl->net_impl->group_impl_get_interruptfd != NULL) {
			fd = group_impl->net_impl->group_impl_get_interruptfd(group_impl);
			if (fd > 0) {
				spdk_fd_group_remove(group->fgrp, fd);  /* [한국어] fd_group에서 백엔드 epoll fd 분리. */
			}
		}

		rc = group_impl->net_impl->group_impl_close(group_impl); /* [한국어] 백엔드 — epoll_create로 만든 fd close. */
		if (rc != 0) {
			SPDK_ERRLOG("group_impl_close for net failed\n");
		}
	}

	if (group->fgrp != NULL) {
		spdk_fd_group_destroy(group->fgrp);             /* [한국어] interrupt-mode에서 만든 fd_group 해제. */
	}

	free(group);                                        /* [한국어] group struct 자체 해제. */
	*_group = NULL;                                     /* [한국어] caller use-after-free 방어. */
	return 0;
}

/*
 * [한국어]
 * sock_get_impl_by_name - g_net_impls에서 name으로 백엔드 lookup.
 *
 * @impl_name: NULL 불가 (assert 검증).
 * @return: 일치 impl 포인터 또는 NULL.
 *
 * connect/listen/set_default_impl 모두 이 헬퍼로 백엔드를 찾는다. 정확 일치 비교(strcmp)로
 * "tcp" vs "tcp_uring" 등 prefix collision 방지.
 */
static inline struct spdk_net_impl *
sock_get_impl_by_name(const char *impl_name)
{
	struct spdk_net_impl *impl;                         /* [한국어] 순회용. */

	assert(impl_name != NULL);                          /* [한국어] caller buggy NULL 방어. */
	STAILQ_FOREACH(impl, &g_net_impls, link) {
		if (0 == strcmp(impl_name, impl->name)) {       /* [한국어] 정확 일치 비교. */
			return impl;
		}
	}

	return NULL;                                        /* [한국어] 미등록 백엔드 이름. */
}

/*
 * [한국어]
 * spdk_sock_impl_get_opts - 특정 백엔드의 현재 impl_opts를 추출.
 *
 * @impl_name: "posix"/"uring"/"ssl" 등.
 * @opts: 출력 버퍼 (caller 할당).
 * @len: in/out — caller가 보유한 buffer 크기, 백엔드가 실제로 채운 크기로 갱신.
 * @return: 0(성공), -EINVAL(인자 NULL/백엔드 없음), -ENOTSUP(백엔드 미구현).
 *
 * sock_rpc.c의 sock_impl_get_options RPC가 이 함수로 옵션을 읽어 JSON 응답 작성.
 */
int
spdk_sock_impl_get_opts(const char *impl_name, struct spdk_sock_impl_opts *opts, size_t *len)
{
	struct spdk_net_impl *impl;

	if (!impl_name || !opts || !len) {
		return -EINVAL;                                 /* [한국어] 모든 인자 필수. */
	}

	impl = sock_get_impl_by_name(impl_name);            /* [한국어] 백엔드 lookup. */
	if (!impl) {
		return -EINVAL;                                 /* [한국어] 미등록. */
	}

	if (!impl->get_opts) {
		return -ENOTSUP;                                /* [한국어] 백엔드가 옵션 조회 vtable 미구현. */
	}

	return impl->get_opts(opts, len);                   /* [한국어] 백엔드 위임 — len을 ABI-safe accessor로 사용. */
}

/*
 * [한국어]
 * spdk_sock_impl_set_opts - 특정 백엔드 impl_opts 갱신 (recv_buf_size, tls_version 등).
 *
 * @len: caller buffer 크기 — 백엔드가 알지 못하는 신규 필드는 무시(또는 ABI-safe 처리).
 *
 * sock_rpc.c의 sock_impl_set_options RPC가 사용. 이 호출은 startup-only RPC로 등록되어
 * 운영자가 SPDK 기동 시 한 번만 적용하는 것이 권장됨 (런타임 변경은 지원되지만 신중).
 */
int
spdk_sock_impl_set_opts(const char *impl_name, const struct spdk_sock_impl_opts *opts,
			size_t len)
{
	struct spdk_net_impl *impl;

	if (!impl_name || !opts) {
		return -EINVAL;                                 /* [한국어] impl_name + opts 필수. */
	}

	impl = sock_get_impl_by_name(impl_name);
	if (!impl) {
		return -EINVAL;
	}

	if (!impl->set_opts) {
		return -ENOTSUP;
	}

	return impl->set_opts(opts, len);                   /* [한국어] 백엔드 위임 — 옵션 검증·적용은 백엔드 책임. */
}

/*
 * [한국어]
 * spdk_sock_write_config_json - 현재 sock 서브시스템 설정을 JSON 배열로 출력.
 *
 * @w: JSON writer (caller가 connection list 등의 frame을 미리 시작했음).
 *
 * 동기: SPDK는 "save_config" RPC로 현재 런타임 설정을 JSON으로 dump하여 다음 기동 시 동일
 * 상태 재구성에 사용. sock 서브시스템은 두 종류 method를 출력:
 *   1) sock_set_default_impl: g_default_impl이 설정된 경우 한 번 emit.
 *   2) sock_impl_set_options: 등록된 모든 백엔드에 대해 현재 impl_opts dump (recv_buf_size,
 *      enable_zerocopy, tls_version 등 모든 필드).
 *
 * 결과 JSON은 spdk_jsonrpc_client로 다시 sock_set_default_impl/sock_impl_set_options를
 * 호출하면 동일 상태 복원.
 */
void
spdk_sock_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_net_impl *impl;                         /* [한국어] 백엔드 순회용. */
	struct spdk_sock_impl_opts opts;                    /* [한국어] 옵션 추출 버퍼. */
	size_t len;                                         /* [한국어] in/out len. */

	assert(w != NULL);                                  /* [한국어] writer NULL 방어. */

	spdk_json_write_array_begin(w);                     /* [한국어] [ 시작 — sock subsystem RPC 배열. */

	if (g_default_impl) {                               /* [한국어] default impl이 명시 설정되었을 때만 emit. */
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "sock_set_default_impl"); /* [한국어] RPC 이름. */
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "impl_name", g_default_impl->name); /* [한국어] params: impl_name 인자. */
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}

	STAILQ_FOREACH(impl, &g_net_impls, link) {          /* [한국어] 모든 백엔드의 옵션을 emit. */
		if (!impl->get_opts) {
			continue;                                   /* [한국어] 옵션 vtable 미지원 백엔드는 skip. */
		}

		len = sizeof(opts);
		if (impl->get_opts(&opts, &len) == 0) {         /* [한국어] 백엔드에서 현재 옵션 추출 성공 시. */
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "method", "sock_impl_set_options"); /* [한국어] RPC 이름. */
			spdk_json_write_named_object_begin(w, "params");
			spdk_json_write_named_string(w, "impl_name", impl->name); /* [한국어] 어느 백엔드의 옵션인지. */
			spdk_json_write_named_uint32(w, "recv_buf_size", opts.recv_buf_size); /* [한국어] 커널 RX 버퍼. */
			spdk_json_write_named_uint32(w, "send_buf_size", opts.send_buf_size); /* [한국어] 커널 TX 버퍼. */
			spdk_json_write_named_bool(w, "enable_recv_pipe", opts.enable_recv_pipe); /* [한국어] posix 백엔드 user-space recv pipe (대기열) 활성. */
			spdk_json_write_named_bool(w, "enable_quickack", opts.enable_quickack); /* [한국어] TCP_QUICKACK — delayed ACK 끄기. */
			spdk_json_write_named_uint32(w, "enable_placement_id", opts.enable_placement_id); /* [한국어] PLACEMENT 모드: NONE/CPU/MARK/NAPI. */
			spdk_json_write_named_bool(w, "enable_zerocopy_send_server", opts.enable_zerocopy_send_server); /* [한국어] server측 MSG_ZEROCOPY. */
			spdk_json_write_named_bool(w, "enable_zerocopy_send_client", opts.enable_zerocopy_send_client); /* [한국어] client측 MSG_ZEROCOPY. */
			spdk_json_write_named_uint32(w, "zerocopy_threshold", opts.zerocopy_threshold); /* [한국어] zerocopy 사용 최소 byte (작은 송신은 일반 sendmsg). */
			spdk_json_write_named_uint32(w, "tls_version", opts.tls_version); /* [한국어] TLS 1.2/1.3 (ssl 백엔드 전용). */
			spdk_json_write_named_bool(w, "enable_ktls", opts.enable_ktls); /* [한국어] kTLS — 커널이 TLS record 처리. */
			spdk_json_write_object_end(w);
			spdk_json_write_object_end(w);
		} else {
			SPDK_ERRLOG("Failed to get socket options for socket implementation %s\n", impl->name);
		}
	}

	spdk_json_write_array_end(w);                       /* [한국어] ] 종료. */
}

/*
 * [한국어]
 * spdk_net_impl_register - 백엔드를 g_net_impls에 등록 (라이브러리 로드 시 자동 호출).
 *
 * @impl: 백엔드의 정적 spdk_net_impl 구조체 포인터.
 *
 * 동기: 각 백엔드(posix/uring/ssl)의 .c 파일은 SPDK_SOCK_IMPL_REGISTER(name, impl) 매크로로
 * __attribute__((constructor)) 함수를 정의하고, dynamic loader(또는 main 직전)가 그 함수를
 * 자동 호출 — 그 안에서 spdk_net_impl_register() 호출. 결과적으로 main()이 시작될 때엔 모든
 * 빌드-enabled 백엔드가 g_net_impls에 등록된 상태.
 *
 * INSERT_HEAD 사용: 마지막 등록된 백엔드가 먼저 검색됨. 빌드-enabled 첫 백엔드(=가장 늦게
 * register된 것)가 g_default_impl 미설정 시 fallback 후보가 되는 셈.
 *
 * 호출 컨텍스트: process startup (main 진입 전) — 단일 thread, 락 불필요.
 */
void
spdk_net_impl_register(struct spdk_net_impl *impl)
{
	STAILQ_INSERT_HEAD(&g_net_impls, impl, link);       /* [한국어] head 삽입 — 최신 등록이 검색 1순위. */
}

/*
 * [한국어]
 * sock_init_opts_match - 두 initialize_opts가 동일 ABI/값인지 비교 (재초기화 호출 검증).
 *
 * @return: opts_size와 enable_interrupt_mode 둘 다 같으면 true.
 *
 * 동기: spdk_sock_initialize는 멱등적이어야 한다(여러 모듈이 호출 가능). 두 번째 호출이
 * 다른 옵션으로 들어오면 -EALREADY로 거부 — 라이브러리 상태 일관성 유지.
 */
static bool
sock_init_opts_match(struct spdk_sock_initialize_opts *opts1,
		     struct spdk_sock_initialize_opts *opts2)
{
	if (opts1->opts_size != opts2->opts_size) {
		return false;                                   /* [한국어] ABI 크기 불일치 — 다른 헤더로 빌드. */
	}

	if (opts1->enable_interrupt_mode != opts2->enable_interrupt_mode) {
		return false;                                   /* [한국어] 모드 불일치 — 첫 번째 caller가 정한 모드를 우선. */
	}

	return true;
}

/*
 * [한국어]
 * spdk_sock_initialize - sock 서브시스템 1회 초기화 (모든 백엔드 init() 호출).
 *
 * @user_opts: NULL이면 default(enable_interrupt_mode=true), 아니면 사용자 지정.
 * @return: 0(성공/이미 같은 옵션으로 초기화됨), -EALREADY(다른 옵션으로 재호출 시도).
 *
 * 동작:
 *   1) opts_local에 default 채움.
 *   2) user_opts 있으면 ABI-safe로 enable_interrupt_mode만 override.
 *   3) g_initialized 검사 — 이미 초기화된 경우 옵션 일치 여부만 확인하고 return.
 *   4) g_init_opts에 보관, g_initialized=true.
 *   5) 등록된 모든 백엔드의 init(opts_local) 호출 — 실패하는 백엔드는 g_net_impls에서 제거
 *      (이후 connect/listen에서 매칭 안 됨, but warn 출력).
 *
 * 호출 시점: SPDK app 초기화 단계 (e.g., spdk_subsystem_init → sock_subsystem_init →
 * spdk_sock_initialize).
 */
int
spdk_sock_initialize(struct spdk_sock_initialize_opts *user_opts)
{
	struct spdk_sock_initialize_opts opts_local;        /* [한국어] 라이브러리 ABI 사본. */
	struct spdk_net_impl *impl, *tmp;                   /* [한국어] 백엔드 순회용 + FOREACH_SAFE remove 대비. */
	static bool g_initialized = false;                  /* [한국어] 1회 초기화 가드 (translation-unit 전역). */
	int rc;

	/* Initialize opts_local with defaults */
	spdk_sock_get_default_initialize_opts(&opts_local, sizeof(opts_local)); /* [한국어] 기본값 채우기. */

	/* If user_opts is provided, use ABI-safe field access based on opts_size */
	if (user_opts != NULL) {
		size_t opts_size;

		/* Get the user's opts_size */
		opts_size = user_opts->opts_size;               /* [한국어] caller ABI 크기. */

		/* Copy fields that are within the user's opts_size */
#define FIELD_OK(field) \
	offsetof(struct spdk_sock_initialize_opts, field) + sizeof(opts_local.field) <= opts_size /* [한국어] ABI-safe 검사. */

		if (FIELD_OK(enable_interrupt_mode)) {
			opts_local.enable_interrupt_mode = user_opts->enable_interrupt_mode; /* [한국어] 사용자 값 적용. */
		}

#undef FIELD_OK
	}

	if (g_initialized) {
		/* If already initialized, check if the options match */
		if (sock_init_opts_match(&opts_local, &g_init_opts)) {
			return 0;                                   /* [한국어] 동일 옵션 — 멱등 처리. */
		} else {
			return -EALREADY;                           /* [한국어] 다른 옵션 — caller 의도 모순. */
		}
	}

	/* Store the options for future comparison */
	memcpy(&g_init_opts, &opts_local, sizeof(g_init_opts)); /* [한국어] 향후 비교용 저장 + group_create 등에서 참조. */

	g_initialized = true;                               /* [한국어] 1회 초기화 가드. */

	STAILQ_FOREACH_SAFE(impl, &g_net_impls, link, tmp) {/* [한국어] 모든 백엔드 init — 실패하면 list에서 제거. */
		if (impl->init) {                               /* [한국어] init slot은 선택적. */
			rc = impl->init(&opts_local);               /* [한국어] 백엔드별 자원 초기화 (epoll fd 풀, TLS context 등). */
			if (rc != 0) {
				SPDK_WARNLOG("Removing %s net impl - initialization failed: %d\n",
					     impl->name, rc);
				STAILQ_REMOVE(&g_net_impls, impl, spdk_net_impl, link); /* [한국어] 비활성 백엔드 제거 — 이후 connect 매칭에서 제외. */
				continue;
			}
		}
	}

	return 0;
}

/*
 * [한국어]
 * spdk_sock_set_default_impl - g_default_impl 설정 (impl_name=NULL connect/listen에서 사용됨).
 *
 * @return: 0(성공/이미 동일), -EINVAL(NULL 인자 / 미등록 백엔드).
 *
 * RPC sock_set_default_impl이 이 함수로 위임. 운영자가 SPDK 기동 시 "sock_impl_set_options"로
 * 백엔드 옵션을 먼저 적용한 후 "sock_set_default_impl"로 default를 정하는 것이 일반적 흐름.
 */
int
spdk_sock_set_default_impl(const char *impl_name)
{
	struct spdk_net_impl *impl;

	if (!impl_name) {
		return -EINVAL;
	}

	impl = sock_get_impl_by_name(impl_name);
	if (!impl) {
		return -EINVAL;                                 /* [한국어] 미등록 백엔드 — 빌드에서 disable되었거나 init 실패. */
	}

	if (impl == g_default_impl) {
		return 0;                                       /* [한국어] 이미 동일 — 멱등. */
	}

	if (g_default_impl) {
		SPDK_DEBUGLOG(sock, "Change the default sock impl from %s to %s\n", g_default_impl->name,
			      impl->name);                          /* [한국어] 디버그: 이전→새 값 추적. */
	} else {
		SPDK_DEBUGLOG(sock, "Set default sock implementation to %s\n", impl_name);
	}

	g_default_impl = impl;                              /* [한국어] 단순 포인터 갱신 — atomic on aligned 64-bit, 그러나 SPDK는 single-thread RPC 가정. */
	return 0;
}

/*
 * [한국어]
 * spdk_sock_get_default_impl - 현재 default 백엔드 이름 반환 (없으면 NULL).
 *
 * RPC sock_get_default_impl 핸들러가 사용 — JSON에 impl_name 필드로 응답.
 */
const char *
spdk_sock_get_default_impl(void)
{
	if (g_default_impl) {
		return g_default_impl->name;                    /* [한국어] 정적 문자열 — 호출자 free 금지. */
	}

	return NULL;                                        /* [한국어] 명시 설정 없음. */
}

/*
 * [한국어]
 * spdk_sock_group_get_interruptfd - group의 fd_group fd를 반환 (interrupt-mode poller 등록용).
 *
 * @return: epoll fd (>0), -ENOTSUP(interrupt-mode 비활성/fgrp 없음).
 *
 * 동기: SPDK reactor의 interrupt-mode가 여러 fd를 한 곳에 모아 epoll_wait로 sleep. 이 함수가
 * 반환한 fd를 reactor의 fd_group에 등록하면 sock 이벤트로 reactor가 깨어남.
 */
int
spdk_sock_group_get_interruptfd(struct spdk_sock_group *group)
{
	assert(group != NULL);                              /* [한국어] caller buggy NULL 방어. */

	if (!g_init_opts.enable_interrupt_mode) {
		return -ENOTSUP;                                /* [한국어] polled-only 모드 — fd 없음. */
	}

	if (group->fgrp == NULL) {
		return -ENOTSUP;                                /* [한국어] interrupt 모드인데 fgrp NULL은 예외 (group_create 실패 후). */
	}

	return spdk_fd_group_get_fd(group->fgrp);           /* [한국어] fd_group의 마스터 epoll fd 추출. */
}

/*
 * [한국어]
 * SPDK_LOG_REGISTER_COMPONENT(sock) - sock 디버그 로그 컴포넌트 등록.
 *
 * 결과: SPDK_DEBUGLOG(sock, ...) 매크로가 활성/비활성 토글 가능 (--logflag sock).
 */
SPDK_LOG_REGISTER_COMPONENT(sock)

/*
 * [한국어]
 * sock_trace - SPDK trace 프레임워크에 sock 관련 tracepoint 등록.
 *
 * 등록되는 tracepoint:
 *   - SOCK_REQ_QUEUE: writev_async가 req를 sock->queued_reqs에 enqueue 시점.
 *     (start=1 — 객체 lifetime 시작 표식)
 *   - SOCK_REQ_PEND: req가 sendmsg 후 zerocopy ack 대기로 전환되어 pending_reqs로 이동 시점.
 *   - SOCK_REQ_COMPLETE: req가 모든 송신 완료되어 cb_fn 호출 직전 시점.
 *
 * 모든 tracepoint는 ctx 포인터(보통 req 자체) 1개를 인자로 가져 trace tool이 객체 lifecycle을
 * 그릴 수 있게 함.
 *
 * 호출 시점: SPDK_TRACE_REGISTER_FN constructor가 trace lib 초기화 후 자동 호출.
 */
static void
sock_trace(void)
{
	struct spdk_trace_tpoint_opts opts[] = {            /* [한국어] tracepoint 정의 배열. */
		{
			"SOCK_REQ_QUEUE", TRACE_SOCK_REQ_QUEUE,     /* [한국어] 이름 + 숫자 ID. */
			OWNER_TYPE_SOCK, OBJECT_SOCK_REQ, 1,        /* [한국어] owner=sock, object=req, start_lifetime=1. */
			{
				{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },  /* [한국어] arg: req 포인터(64bit). */
			}
		},
		{
			"SOCK_REQ_PEND", TRACE_SOCK_REQ_PEND,
			OWNER_TYPE_SOCK, OBJECT_SOCK_REQ, 0,        /* [한국어] start_lifetime=0 — 중간 상태. */
			{
				{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
			}
		},
		{
			"SOCK_REQ_COMPLETE", TRACE_SOCK_REQ_COMPLETE,
			OWNER_TYPE_SOCK, OBJECT_SOCK_REQ, 0,
			{
				{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
			}
		},
	};

	spdk_trace_register_owner_type(OWNER_TYPE_SOCK, 's');           /* [한국어] owner 종류 등록 — trace tool이 's'로 표기. */
	spdk_trace_register_object(OBJECT_SOCK_REQ, 's');               /* [한국어] object 종류 등록 (req의 'sub-symbol' 's'). */
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));  /* [한국어] 위 배열을 trace lib에 일괄 등록. */
}
/* [한국어] SPDK_TRACE_REGISTER_FN: constructor로 sock_trace를 등록 — 그룹 이름="sock", group_id=TRACE_GROUP_SOCK. */
SPDK_TRACE_REGISTER_FN(sock_trace, "sock", TRACE_GROUP_SOCK)
