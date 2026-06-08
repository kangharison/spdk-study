/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK sock 백엔드 — POSIX/epoll(+OpenSSL/TLS) 구현 (posix.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK sock 추상화의 두 백엔드("posix"와 "ssl")를 한 곳에 구현한다. 백엔드는
 * 표준 POSIX socket 시스템 호출(socket/connect/listen/accept/sendmsg/recvmsg) 위에 SPDK
 * sock 인터페이스(spdk_net_impl vtable)를 매핑하며, Linux는 epoll, FreeBSD는 kqueue로
 * group polling을 수행한다. SSL 백엔드는 동일 POSIX fd 위에 OpenSSL을 얹어 TLS 1.2/1.3
 * 핸드셰이크·암복호화를 수행하며, PSK(Pre-Shared Key) 기반 인증과 kTLS(커널 TLS off-load)도
 * 옵션으로 지원한다. 또한 다음 기능을 SPDK 표준 기대치에 맞춰 구현한다:
 *   1) MSG_ZEROCOPY(SO_ZEROCOPY) — 송신측 user page를 그대로 NIC DMA에 노출하여 memcpy 제거
 *      (Linux 4.14+; SPDK_ZEROCOPY 매크로로 컴파일 가드).
 *   2) 사용자 공간 recv_pipe — TCP 수신 데이터를 spdk_pipe(SPSC ring)로 1차 흡수하여
 *      readv() 시스템 호출 횟수를 줄이고 작은 응답들의 헤더 파싱 비용을 분할 상환.
 *   3) placement_id 정렬 — PLACEMENT_NONE/CPU/MARK/NAPI_ID 중 선택 → 같은 NIC RSS 큐의 트래픽이
 *      같은 group_impl(=같은 reactor)에 묶이도록 spdk_sock_map 갱신.
 *   4) socks_with_data 리스트 — recv_pipe에 데이터가 남은 sock을 별도 TAILQ로 관리하여,
 *      epoll 이벤트 없이도 즉시 service 가능한 sock을 우선 처리 (latency 감소).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   상위(NVMe-oF TCP / iSCSI / JSON-RPC) → lib/sock/sock.c (vtable dispatcher) →
 *     g_posix_net_impl / g_ssl_net_impl (이 파일의 vtable) → 백엔드 함수들 →
 *     read(2)/writev(2)/sendmsg(2)/SSL_*() → epoll_wait(2)/SSL_read(3) → 커널 TCP/TLS stack.
 * 모든 함수는 SPDK 단일 spdk_thread(reactor) 컨텍스트에서 호출됨을 가정 — group 내 sock은
 * 그 reactor에 affinity가 고정되며, recv_pipe·socks_with_data·sendmsg_idx 모두 lockless.
 * 예외는 g_map(placement_id ↔ group_impl)으로, 여러 reactor가 동시에 진입 가능하므로 mtx로
 * 보호한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/sock.h(공개 API), spdk_internal/sock_module.h(spdk_net_impl, group_impl, sock
 *   내부 필드), spdk/pipe.h(spdk_pipe SPSC ring), spdk/net.h(getaddrinfo 등 net 유틸),
 *   spdk/util.h(SPDK_GET_FIELD), spdk/file.h(sysfs 읽기), spdk/log.h(SPDK_ERRLOG),
 *   openssl/ssl.h(TLS context/session).
 * - 의존하는 곳: SPDK_NET_IMPL_REGISTER_DEFAULT(posix) constructor가 lib/sock/sock.c의
 *   g_net_impls에 자기 자신을 등록하고, 모든 spdk_sock_connect/listen은 default impl로
 *   "posix"를 fallback. ssl 백엔드는 별도 등록되어 impl_name="ssl"로 명시 호출 시에만 사용.
 * - 데이터 흐름:
 *   * 송신: 사용자 writev → posix_sock_writev_async(req STAILQ enqueue) → _sock_flush →
 *     posix_writev(sendmsg with MSG_ZEROCOPY) → 커널 → NIC DMA.
 *     ZEROCOPY 완료 통지는 MSG_ERRQUEUE의 SO_EE_ORIGIN_ZEROCOPY로 들어와 _sock_check_zcopy가
 *     수확 후 sock_complete_write_reqs로 cb_fn 호출.
 *   * 수신: 커널 → epoll readable → posix_sock_group_impl_poll(epoll_wait) → cb_fn →
 *     posix_sock_read(recv_pipe로 흡수) → posix_sock_readv(pipe에서 user iov로 복사).
 * - 공유 자료구조: g_map(placement_id 테이블), g_posix_impl_opts / g_ssl_impl_opts(백엔드 옵션),
 *   spdk_pipe_group(여러 sock의 pipe를 한 grant 단위로 관리하여 메모리 효율 향상).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_posix_sock: spdk_sock base + fd + recv_pipe + ssl_ctx/ssl + placement_id.
 * - struct spdk_posix_sock_group_impl: spdk_sock_group_impl base + epoll/kqueue fd +
 *   socks_with_data 리스트 + pipe_group.
 * - posix_sock_init: 새 fd에 SO_RCVBUF/SNDBUF/TCP_NODELAY/SO_PRIORITY/SO_ZEROCOPY 등 일괄 설정.
 * - posix_sock_group_impl_poll: epoll_wait + socks_with_data 우선 처리, ZEROCOPY MSG_ERRQUEUE
 *   수확까지 한 사이클에서 수행.
 * - _sock_flush: queued_reqs를 한 번의 sendmsg(iovec 묶음)으로 송신 시도, EAGAIN/partial 처리.
 * - _sock_check_zcopy: SO_EE_ORIGIN_ZEROCOPY error queue를 polling하여 pending_reqs의 ack 결정.
 * - posix_sock_psk_find_session_server_cb / posix_sock_psk_use_session_client_cb: PSK 인증
 *   콜백 — TLS handshake 시 PSK 검색·바인딩.
 * - posix_connect_poller: non-blocking connect의 SO_ERROR 폴링 (connect_async 모델).
 */

#include "spdk/stdinc.h"            /* [한국어] SPDK 표준 헤더 모음 (errno/string/stdio 등). */

#if defined(__FreeBSD__)            /* [한국어] FreeBSD는 epoll이 없으므로 kqueue/kevent 사용 분기. */
#include <sys/event.h>              /* [한국어] kqueue/kevent API. */
#define SPDK_KEVENT                 /* [한국어] 이후 코드 경로에서 kqueue를 쓰도록 토글. */
#else
#define SPDK_EPOLL                  /* [한국어] Linux: epoll_create1/epoll_ctl/epoll_wait 사용. */
#endif

#if defined(__linux__)              /* [한국어] Linux 전용 errqueue.h — MSG_ZEROCOPY 완료 통지를 errqueue에서 받는다. */
#include <linux/errqueue.h>         /* [한국어] struct sock_extended_err + SO_EE_ORIGIN_ZEROCOPY 정의. */
#endif

#include "spdk/env.h"               /* [한국어] SPDK_ENV_NUMA_ID_ANY 등 환경 추상화. */
#include "spdk/log.h"               /* [한국어] SPDK_ERRLOG/SPDK_DEBUGLOG 매크로. */
#include "spdk/pipe.h"              /* [한국어] spdk_pipe SPSC ring — recv_pipe에 수신 데이터 흡수. */
#include "spdk/sock.h"              /* [한국어] 백엔드가 구현해야 하는 공개 API 타입. */
#include "spdk/util.h"              /* [한국어] SPDK_GET_FIELD / SPDK_COUNTOF 등. */
#include "spdk/string.h"            /* [한국어] spdk_strerror 등 문자열 유틸. */
#include "spdk/net.h"               /* [한국어] spdk_net_getaddr / spdk_net_get_interface_name 등. */
#include "spdk/file.h"              /* [한국어] sysfs 읽기 — NUMA 노드 추출에 사용. */
#include "spdk_internal/sock_module.h" /* [한국어] spdk_net_impl vtable, spdk_sock 내부 필드, SPDK_NET_IMPL_REGISTER 매크로. */
#include "spdk/net.h"               /* [한국어] (중복 include — 무해, 컴파일러가 가드 처리). */

#include "openssl/crypto.h"         /* [한국어] OpenSSL 공통 함수 (ERR_clear_error 등). */
#include "openssl/err.h"            /* [한국어] OpenSSL 에러 큐 추출 (ERR_get_error / ERR_error_string). */
#include "openssl/ssl.h"            /* [한국어] SSL_CTX/SSL/SSL_read/SSL_write — TLS handshake와 record I/O. */

#if defined(SO_ZEROCOPY) && defined(MSG_ZEROCOPY) /* [한국어] 빌드 환경이 SO_ZEROCOPY와 MSG_ZEROCOPY를 모두 지원하면. */
#define SPDK_ZEROCOPY                              /* [한국어] zerocopy 코드 경로를 컴파일에 포함시키는 토글. */
#endif

/*
 * [한국어]
 * struct posix_connect_ctx - non-blocking connect의 진행 상태를 보관하는 컨텍스트.
 *
 * 동기: SPDK는 connect_async를 통해 connect를 non-blocking으로 수행하고, connect_poller가
 * 주기적으로 SO_ERROR를 폴링하여 완료 여부를 확인한다. getaddrinfo가 반환한 여러 주소 후보를
 * 순차로 시도해야 하므로(first_res→next_res 진행) 진행 상태 보존이 필요하다.
 */
struct posix_connect_ctx {
	int fd;
	/* [한국어] 현재 진행 중인 connect의 socket fd.
	 * 설정자: sock_posix_connect_async()에서 socket()으로 생성한 직후.
	 * 읽는 자: posix_connect_poller()가 getsockopt(SO_ERROR)로 결과 확인.
	 * 값 범위: 유효한 fd(>=0) 또는 -1(실패 후 다음 res로 진행 중).
	 * 동기화: 단일 reactor 스레드에서만 다뤄지므로 lock 불필요. */

	bool ssl;
	/* [한국어] 이 connect가 SSL 백엔드용인지 여부.
	 * 설정자: connect_async 호출 시 enable_ssl 인자 그대로 저장.
	 * 읽는 자: posix_connect_poller가 connect 성공 후 SSL handshake 단계로 진입할지 결정.
	 * 동기화: read-only 이후 — lock 불필요. */

	struct addrinfo *first_res;
	/* [한국어] getaddrinfo가 반환한 res 리스트의 head — cleanup 시 freeaddrinfo에 전달.
	 * 설정자: sock_posix_connect_async에서 한 번 set.
	 * 읽는 자: sock_posix_connect_ctx_cleanup에서 freeaddrinfo(first_res). */

	struct addrinfo *next_res;
	/* [한국어] 다음에 시도할 res 노드 — 현재 fd 실패 시 next_res = next_res->ai_next.
	 * 값 범위: 유효한 addrinfo 포인터 또는 NULL(모든 후보 소진). */

	struct spdk_sock_opts opts;
	/* [한국어] 사용자 spdk_sock_opts 사본 — fd 생성·post-connect 단계에서 priority/zcopy/
	 * ack_timeout 등을 적용하기 위해 보관. */

	struct spdk_sock_impl_opts impl_opts;
	/* [한국어] 백엔드별 옵션 사본(recv_buf_size, enable_recv_pipe, tls_version 등).
	 * 설정자: _opts_get_impl_opts가 default + opts->impl_opts overlay로 채움. */

	uint64_t timeout_tsc;
	/* [한국어] connect timeout absolute TSC tick — opts.connect_timeout > 0이면 spdk_get_ticks() +
	 * timeout으로 계산. 0은 무한 대기.
	 * 읽는 자: posix_connect_poller가 spdk_get_ticks() >= timeout_tsc면 -ETIMEDOUT 반환. */

	int set_recvlowat;
	/* [한국어] connect 완료 후 SO_RCVLOWAT으로 설정할 값(>0이면 setsockopt). 0=설정 안 함. */

	int set_recvbuf;
	/* [한국어] connect 완료 후 SO_RCVBUF로 설정할 값(0=기본). */

	int set_sendbuf;
	/* [한국어] connect 완료 후 SO_SNDBUF로 설정할 값(0=기본). */

	spdk_sock_connect_cb_fn cb_fn;
	/* [한국어] connect 완료(또는 실패) 시 caller에 통보할 콜백.
	 * 읽는 자: posix_connect_poller가 cb_fn(cb_arg, sock, rc)로 호출. */

	void *cb_arg;
	/* [한국어] cb_fn의 첫 인자 — caller가 식별자로 사용 (transport context 등). */
};

/*
 * [한국어]
 * struct spdk_posix_sock - posix/ssl 백엔드의 sock 표현 — spdk_sock(base) 확장.
 *
 * 동기: 사용자가 보는 추상 sock 핸들(spdk_sock)에 백엔드 고유 상태(fd, recv_pipe, SSL ctx,
 * placement_id 등)를 덧붙인 wrapper. 모든 백엔드 함수는 spdk_sock * 인자를 __posix_sock() 매크로로
 * 캐스팅하여 이 구조체에 접근한다.
 */
struct spdk_posix_sock {
	struct spdk_sock	base;
	/* [한국어] sock 공통 base — net_impl/group_impl/queued_reqs/cb_fn/flags 등을 보관.
	 * 모든 백엔드는 이 base를 첫 멤버로 둔다(C 캐스팅 호환성). */

	int			fd;
	/* [한국어] 커널 socket fd. -1은 close되었거나 connect 진행 중을 의미할 수 있음.
	 * 설정자: socket()/accept()/listen() 직후.
	 * 읽는 자: 모든 I/O 함수(readv/writev/setsockopt/epoll_ctl 등). */

	uint32_t		sendmsg_idx;
	/* [한국어] MSG_ZEROCOPY 송신마다 1 증가하는 monotonic 카운터.
	 * 동기: SO_ZEROCOPY는 각 sendmsg마다 sequence number를 부여하고, 완료 통지(errqueue)에
	 * lo/hi 범위로 알려준다. 이 idx로 어떤 req가 완료되었는지 매칭.
	 * 읽는 자: posix_writev에서 ++, _sock_check_zcopy에서 errqueue 범위와 비교. */

	struct spdk_pipe	*recv_pipe;
	/* [한국어] 사용자 공간 수신 ring buffer (SPSC). enable_recv_pipe 옵션 시 할당.
	 * 동기: readv 호출 횟수를 줄이고 작은 응답을 흡수하기 위해 epoll readable 시 한 번에
	 * 큰 chunk를 pipe로 읽어둔다. 이후 readv는 pipe에서 user iov로 복사만 수행.
	 * 설정자: posix_sock_alloc_pipe(), recv_buf_size 변경 시 재할당.
	 * 동기화: 같은 reactor 스레드(=producer/consumer)에서만 접근 — lock 불필요. */

	int			recv_buf_sz;
	/* [한국어] 현재 recv_pipe 크기(바이트). posix_sock_set_recvbuf로 변경 시 재할당. */

	bool			pipe_has_data;
	/* [한국어] recv_pipe에 unread 데이터가 있는지 플래그.
	 * 설정자: posix_sock_read가 pipe writer로 데이터 채운 후 set, posix_sock_recv_from_pipe가
	 * 전부 소비하면 clear.
	 * 읽는 자: posix_sock_group_impl_poll이 socks_with_data 리스트 관리에 사용. */

	bool			socket_has_data;
	/* [한국어] kernel sock buffer에 unread 데이터가 있는지(epoll이 알려준 상태).
	 * 설정자: epoll EPOLLIN 받으면 true, recv가 EAGAIN으로 마무리되면 false.
	 * 읽는 자: socks_with_data 리스트 관리. */

	bool			zcopy;
	/* [한국어] 이 sock에 MSG_ZEROCOPY 사용 활성 여부.
	 * 설정자: posix_sock_init이 SO_ZEROCOPY setsockopt 성공 시 true.
	 * 읽는 자: posix_writev가 MSG_ZEROCOPY 플래그 부여 여부 결정. */

	bool			ready;
	/* [한국어] connect/handshake 완료되어 I/O 가능 상태인지.
	 * 설정자: connect 성공 또는 accept 직후 true.
	 * 읽는 자: getaddr/getpeer 등이 connect 진행 중인지 판별. */

	int			placement_id;
	/* [한국어] PLACEMENT_CPU/MARK/NAPI_ID로 추출한 정렬 hint.
	 * 설정자: posix_sock_group_impl_add_sock에서 g_map.entries에 매핑.
	 * 읽는 자: posix_sock_update_mark 등 placement 정책 함수. */

	SSL_CTX			*ssl_ctx;
	/* [한국어] SSL 백엔드 전용 OpenSSL context. NULL=일반 TCP. listen/connect 시 생성. */

	SSL			*ssl;
	/* [한국어] SSL session — handshake 후 SSL_read/SSL_write에 사용. */

	TAILQ_ENTRY(spdk_posix_sock)	link;
	/* [한국어] socks_with_data 리스트의 연결자 — recv_pipe/sock buffer에 데이터가 있는 sock
	 * 들만 빠르게 순회하기 위함. */

	char			interface_name[IFNAMSIZ];
	/* [한국어] SO_BINDTODEVICE/sysfs로 추출한 NIC 이름 (예: "eth0") — get_numa_id 등에 사용. */

	struct posix_connect_ctx	*connect_ctx;
	/* [한국어] non-blocking connect 진행 중이면 ctx 포인터, ready=true가 되면 cleanup 후 NULL. */
};

/* [한국어] recv_pipe에 데이터가 남은 sock을 모아두는 TAILQ — group_impl_poll 우선 처리용. */
TAILQ_HEAD(spdk_has_data_list, spdk_posix_sock);

/*
 * [한국어]
 * struct spdk_posix_sock_group_impl - posix/ssl 백엔드의 sock group 표현.
 *
 * 동기: spdk_sock_group_impl(base) + epoll/kqueue fd + socks_with_data 리스트 + pipe_group.
 * 한 reactor마다 보통 하나 생성되며, 같은 reactor에 묶인 sock들의 epoll polling을 책임진다.
 */
struct spdk_posix_sock_group_impl {
	struct spdk_sock_group_impl	base;
	/* [한국어] group 공통 base — net_impl/parent group/socks 리스트 보관. */

	int				fd;
	/* [한국어] epoll(Linux) 또는 kqueue(FreeBSD) fd.
	 * 설정자: posix_sock_group_impl_create()에서 epoll_create1(EPOLL_CLOEXEC).
	 * 읽는 자: posix_sock_group_impl_poll()이 epoll_wait()에 전달, add_sock이 epoll_ctl. */

	struct spdk_interrupt		*intr;
	/* [한국어] interrupt-mode poller — fd_group에 등록된 epoll fd가 readable이면 reactor가 깨어남. */

	struct spdk_has_data_list	socks_with_data;
	/* [한국어] recv_pipe 또는 sock buffer에 데이터가 남은 sock 리스트.
	 * 동기: epoll_wait이 같은 이벤트를 반복 전달하지 않을 수 있으므로(EPOLLET), 한 사이클에서
	 * 다 처리 못한 sock을 이 리스트에 둬 다음 polling 사이클에 즉시 처리. */

	int				placement_id;
	/* [한국어] PLACEMENT_CPU/MARK 모드에서 이 group_impl이 책임지는 placement_id (예: CPU id). */

	struct spdk_pipe_group		*pipe_group;
	/* [한국어] 같은 group 내 sock들의 recv_pipe들을 한 grant 단위로 묶어 메모리 효율 향상. */
};

/* [한국어] posix 백엔드의 전역 옵션. sock_impl_set_options RPC로 갱신 가능.
 * - recv_buf_size/send_buf_size: setsockopt(SO_RCVBUF/SNDBUF)에 사용 — 커널 TCP 버퍼 크기.
 * - enable_recv_pipe: 사용자 공간 recv_pipe(SPSC) 사용 여부 (작은 응답 latency 절감).
 * - enable_quickack: TCP_QUICKACK — delayed ACK 끄기 (RPC 같은 short request에서 RTT 감소).
 * - enable_placement_id: NONE/CPU/MARK/NAPI_ID — 어떤 hint로 placement_id를 추출할지.
 * - enable_zerocopy_send_*: MSG_ZEROCOPY 활성 (서버측/클라이언트측 별도 토글).
 * - zerocopy_threshold: 이 크기 이상의 송신만 zerocopy 적용 (작은 송신은 일반 sendmsg).
 * - tls_*/enable_ktls/psk_*: SSL 백엔드 전용 (TLS 버전, kTLS off-load, PSK).
 *
 * 보호: RPC 핸들러는 SPDK app init 단계에서 호출되므로 race 미발생 가정. */
static struct spdk_sock_impl_opts g_posix_impl_opts = {
	.recv_buf_size = DEFAULT_SO_RCVBUF_SIZE,         /* [한국어] 기본 SO_RCVBUF 크기 (헤더에 정의된 default). */
	.send_buf_size = DEFAULT_SO_SNDBUF_SIZE,         /* [한국어] 기본 SO_SNDBUF 크기. */
	.enable_recv_pipe = true,                        /* [한국어] 사용자 공간 recv_pipe ON. */
	.enable_quickack = false,                        /* [한국어] TCP_QUICKACK OFF (커널 기본 ACK 정책). */
	.enable_placement_id = PLACEMENT_NONE,           /* [한국어] placement 정책 미적용 (group 자유 분배). */
	.enable_zerocopy_send_server = true,             /* [한국어] 서버측 MSG_ZEROCOPY 기본 ON. */
	.enable_zerocopy_send_client = false,            /* [한국어] 클라이언트측 OFF (페이지 회수 비용 보수적). */
	.zerocopy_threshold = 0,                         /* [한국어] 0 = 모든 송신에 zerocopy 적용. */
	.tls_version = 0,                                /* [한국어] OpenSSL 기본 — posix(non-SSL)에선 사용 안 함. */
	.enable_ktls = false,                            /* [한국어] kTLS off-load OFF. */
	.psk_key = NULL,                                 /* [한국어] PSK 미설정. */
	.psk_key_size = 0,
	.psk_identity = NULL,
	.get_key = NULL,                                 /* [한국어] PSK 콜백 미설정. */
	.get_key_ctx = NULL,
	.tls_cipher_suites = NULL                        /* [한국어] OpenSSL 기본 cipher list 사용. */
};

/* [한국어] SSL 백엔드 전용 옵션 — posix와 별도 인스턴스(독립 RPC로 갱신).
 * recv/send buf는 MIN 크기로 보수적 설정 — TLS record 단위 fragmenting을 고려. */
static struct spdk_sock_impl_opts g_ssl_impl_opts = {
	.recv_buf_size = MIN_SO_RCVBUF_SIZE,             /* [한국어] TLS는 record 단위 작은 transaction이 많아 작은 버퍼로 시작. */
	.send_buf_size = MIN_SO_SNDBUF_SIZE,
	.enable_recv_pipe = true,
	.enable_quickack = false,
	.enable_placement_id = PLACEMENT_NONE,
	.enable_zerocopy_send_server = true,
	.enable_zerocopy_send_client = false,
	.zerocopy_threshold = 0,
	.tls_version = 0,                                /* [한국어] 0 = OpenSSL 자동(보통 TLS 1.3 협상). */
	.enable_ktls = false,
	.psk_key = NULL,
	.psk_identity = NULL
};

/* [한국어] placement_id ↔ group_impl 매핑 테이블 (posix/ssl 공유).
 * 보호: g_map.mtx (pthread mutex). add_sock/remove_sock 시 lookup/insert/release. */
static struct spdk_sock_map g_map = {
	.entries = STAILQ_HEAD_INITIALIZER(g_map.entries), /* [한국어] 빈 리스트로 시작. */
	.mtx = PTHREAD_MUTEX_INITIALIZER                  /* [한국어] 정적 mutex 초기화. */
};

/*
 * [한국어]
 * posix_sock_map_cleanup - 라이브러리 unload 시 g_map의 모든 entry를 해제.
 *
 * GCC __attribute((destructor))로 dlclose/main return 후 자동 호출. spdk_sock_map_cleanup이
 * STAILQ를 비우고 entry들을 free.
 */
__attribute((destructor)) static void
posix_sock_map_cleanup(void)
{
	spdk_sock_map_cleanup(&g_map);                    /* [한국어] map 비우기. */
}

/* [한국어] 추상 spdk_sock 포인터를 백엔드 구체 타입으로 다운캐스팅 (base가 첫 멤버이므로 안전). */
#define __posix_sock(sock) (struct spdk_posix_sock *)sock
/* [한국어] 추상 group_impl을 백엔드 구체 타입으로 다운캐스팅. */
#define __posix_group_impl(group) (struct spdk_posix_sock_group_impl *)group

/*
 * [한국어]
 * posix_sock_copy_impl_opts - ABI-safe로 spdk_sock_impl_opts 필드 복사.
 *
 * @dest: 대상 구조체 (전체 크기 보유 가정).
 * @src: 소스 구조체 (caller-provided, len 크기로만 신뢰).
 * @len: src의 ABI 크기 (caller가 빌드된 시점의 sizeof).
 *
 * 동기: SPDK 라이브러리가 새 필드를 추가해도 구버전 헤더로 빌드된 호출자와 호환되어야 한다.
 * FIELD_OK 매크로는 offsetof+sizeof가 len 안에 들어가는지 검사하여, src에 그 필드가 실재할
 * 때만 dest에 대입한다 — overrun 방지.
 *
 * 호출 체인: _sock_impl_set_opts / _opts_get_impl_opts → [이 함수].
 */
static void
posix_sock_copy_impl_opts(struct spdk_sock_impl_opts *dest, const struct spdk_sock_impl_opts *src,
			  size_t len)
{
#define FIELD_OK(field) \
	offsetof(struct spdk_sock_impl_opts, field) + sizeof(src->field) <= len
	/* [한국어] field가 src의 len 범위 안에 들어가는지 컴파일 시 검사. */

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
		dest->field = src->field; \
	}
	/* [한국어] field가 안전 범위면 대입, 아니면 dest의 default 유지. */

	SET_FIELD(recv_buf_size);
	SET_FIELD(send_buf_size);
	SET_FIELD(enable_recv_pipe);
	SET_FIELD(enable_zerocopy_send);
	SET_FIELD(enable_quickack);
	SET_FIELD(enable_placement_id);
	SET_FIELD(enable_zerocopy_send_server);
	SET_FIELD(enable_zerocopy_send_client);
	SET_FIELD(zerocopy_threshold);
	SET_FIELD(tls_version);
	SET_FIELD(enable_ktls);
	SET_FIELD(psk_key);
	SET_FIELD(psk_key_size);
	SET_FIELD(psk_identity);
	SET_FIELD(get_key);
	SET_FIELD(get_key_ctx);
	SET_FIELD(tls_cipher_suites);

#undef SET_FIELD
#undef FIELD_OK
}

/*
 * [한국어]
 * _sock_impl_get_opts - 백엔드별 g_*_impl_opts에서 caller에게 옵션 사본 반환.
 *
 * @opts: 출력 버퍼 (caller 할당, *len 크기).
 * @impl_opts: 소스 — g_posix_impl_opts 또는 g_ssl_impl_opts.
 * @len: in/out — caller buffer 크기. 반환 시 실제 채워진 크기로 갱신.
 * @return: 0(성공), -EINVAL(NULL 인자).
 *
 * 호출 컨텍스트: RPC 핸들러(sock_impl_get_options) → spdk_sock_impl_get_opts → [이 함수].
 */
static int
_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, struct spdk_sock_impl_opts *impl_opts,
		    size_t *len)
{
	if (!opts || !len) {
		return -EINVAL;                                  /* [한국어] 필수 인자 검증. */
	}

	assert(sizeof(*opts) >= *len);                       /* [한국어] caller buffer가 lib opts 구조체보다 크면 안 됨 (ABI 약속). */
	memset(opts, 0, *len);                               /* [한국어] padding/uninit 필드를 0으로. */

	posix_sock_copy_impl_opts(opts, impl_opts, *len);    /* [한국어] ABI-safe 필드 복사. */
	*len = spdk_min(*len, sizeof(*impl_opts));           /* [한국어] 실제 채워진 크기로 갱신. */
	return 0;
}

/*
 * [한국어]
 * posix_sock_impl_get_opts - posix 백엔드의 옵션 추출 (vtable slot).
 *
 * 호출: lib/sock/sock.c의 spdk_sock_impl_get_opts → impl->get_opts.
 */
static int
posix_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, size_t *len)
{
	return _sock_impl_get_opts(opts, &g_posix_impl_opts, len); /* [한국어] posix 전역 옵션에서 복사. */
}

/*
 * [한국어]
 * ssl_sock_impl_get_opts - ssl 백엔드의 옵션 추출 (vtable slot).
 */
static int
ssl_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, size_t *len)
{
	return _sock_impl_get_opts(opts, &g_ssl_impl_opts, len);  /* [한국어] ssl 전역 옵션에서 복사. */
}

/*
 * [한국어]
 * _sock_impl_set_opts - 백엔드별 옵션을 caller가 준 값으로 갱신.
 *
 * 동기: sock_impl_set_options RPC가 운영자로부터 받은 새 옵션을 그대로 반영. 이후 connect/
 * listen에서 새 옵션 적용. 단, 이미 만들어진 sock은 영향받지 않음 (생성 시 사본을 가짐).
 */
static int
_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, struct spdk_sock_impl_opts *impl_opts,
		    size_t len)
{
	if (!opts) {
		return -EINVAL;
	}

	assert(sizeof(*opts) >= len);                        /* [한국어] caller가 라이브러리보다 큰 구조체 못 줌. */
	posix_sock_copy_impl_opts(impl_opts, opts, len);     /* [한국어] g_*_impl_opts 갱신. */
	return 0;
}

/*
 * [한국어]
 * posix_sock_impl_set_opts - posix 옵션 갱신 (vtable slot).
 */
static int
posix_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, size_t len)
{
	return _sock_impl_set_opts(opts, &g_posix_impl_opts, len);
}

/*
 * [한국어]
 * ssl_sock_impl_set_opts - ssl 옵션 갱신 (vtable slot).
 */
static int
ssl_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, size_t len)
{
	return _sock_impl_set_opts(opts, &g_ssl_impl_opts, len);
}

/*
 * [한국어]
 * _opts_get_impl_opts - sock 생성 시 default와 caller가 준 impl_opts를 병합.
 *
 * @opts: caller spdk_sock_opts (impl_opts 포인터+size 포함 가능).
 * @dest: 출력 - 병합 결과.
 * @default_impl: 백엔드별 기본값 (g_posix_impl_opts 또는 g_ssl_impl_opts).
 *
 * 흐름: dest를 default로 채운 후, opts->impl_opts가 있으면 그 값들로 overlay.
 * 결과: caller가 일부 필드만 지정해도 나머지는 default 그대로.
 */
static void
_opts_get_impl_opts(const struct spdk_sock_opts *opts, struct spdk_sock_impl_opts *dest,
		    const struct spdk_sock_impl_opts *default_impl)
{
	/* Copy the default impl_opts first to cover cases when user's impl_opts is smaller */
	memcpy(dest, default_impl, sizeof(*dest));           /* [한국어] 1단계: 전역 default로 dest 초기화. */

	if (opts->impl_opts != NULL) {
		assert(sizeof(*dest) >= opts->impl_opts_size);   /* [한국어] caller 구조체 < lib 구조체 보장. */
		posix_sock_copy_impl_opts(dest, opts->impl_opts, opts->impl_opts_size); /* [한국어] 2단계: caller 값 overlay. */
	}
}

/*
 * [한국어]
 * posix_sock_getaddr - sock의 local/remote 주소·포트를 문자열로 추출 (vtable getaddr).
 *
 * @saddr/sport: 서버측(local) — getsockname() 결과.
 * @caddr/cport: 클라이언트측(peer) — getpeername() 결과.
 * @return: 0(성공), -EAGAIN(connect 진행 중), -ENOTCONN(연결 실패).
 */
static int
posix_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, uint16_t *sport,
		   char *caddr, int clen, uint16_t *cport)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);  /* [한국어] base → posix sock 다운캐스팅. */

	if (!sock->ready) {                                  /* [한국어] 연결 완료 전엔 주소 정보 미확정. */
		SPDK_ERRLOG("Connection %s.\n", sock->connect_ctx ? "in progress" : "failed");
		return sock->connect_ctx ? -EAGAIN : -ENOTCONN;  /* [한국어] connect 진행 vs 실패 구분. */
	}

	assert(sock != NULL);
	return spdk_net_getaddr(sock->fd, saddr, slen, sport, caddr, clen, cport); /* [한국어] getsock/peer name wrapper. */
}

/*
 * [한국어]
 * posix_sock_get_interface_name - sock이 묶인 NIC 이름(예: "eth0") 반환.
 *
 * 동작: getsockname으로 local IP 추출 후, /sys/class/net/* 의 IP들과 매칭하여 NIC 이름 결정.
 * 결과는 sock->interface_name(IFNAMSIZ)에 캐시.
 */
static const char *
posix_sock_get_interface_name(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	char saddr[64];                                      /* [한국어] local IP 문자열 버퍼 (IPv6 최대 길이 + null). */
	int rc;

	rc = spdk_net_getaddr(sock->fd, saddr, sizeof(saddr), NULL, NULL, 0, NULL); /* [한국어] local IP만 추출. */
	if (rc < 0) {
		return NULL;
	}

	rc = spdk_net_get_interface_name(saddr, sock->interface_name,
					 sizeof(sock->interface_name)); /* [한국어] IP → NIC 이름 변환. */
	if (rc != 0) {
		return NULL;
	}

	return sock->interface_name;                         /* [한국어] caller는 sock lifetime 동안 유효. */
}

/*
 * [한국어]
 * posix_sock_get_numa_id - sock NIC의 NUMA 노드 id를 sysfs에서 읽어 반환.
 *
 * @return: NUMA id (>=0), 실패 시 SPDK_ENV_NUMA_ID_ANY(=-1).
 *
 * NUMA 친화도 결정에 사용 — transport가 동일 NUMA의 reactor에 sock 할당.
 */
static int32_t
posix_sock_get_numa_id(struct spdk_sock *sock)
{
	const char *interface_name;
	uint32_t numa_id;
	int rc;

	interface_name = posix_sock_get_interface_name(sock); /* [한국어] NIC 이름 추출. */
	if (interface_name == NULL) {
		return SPDK_ENV_NUMA_ID_ANY;                     /* [한국어] NIC 미확정 — NUMA 친화도 적용 불가. */
	}

	rc = spdk_read_sysfs_attribute_uint32(&numa_id,
					      "/sys/class/net/%s/device/numa_node", interface_name); /* [한국어] sysfs에서 NUMA 노드 읽기. */
	if (rc == 0 && numa_id <= INT32_MAX) {
		return (int32_t)numa_id;                         /* [한국어] -1(no NUMA)는 INT32_MAX 검사로 걸러짐. */
	} else {
		return SPDK_ENV_NUMA_ID_ANY;
	}
}

/*
 * [한국어]
 * posix_sock_alloc_pipe - sock의 사용자 공간 recv_pipe를 (재)할당.
 *
 * @sock: 대상 sock.
 * @sz: 새 pipe 크기. 0이면 해제, >0이면 alloc(또는 기존 데이터 보존하며 재할당).
 * @return: 0(성공), -ENOMEM, -EINVAL.
 *
 * 동기: enable_recv_pipe=true 백엔드는 readv 호출 횟수를 줄이기 위해 한 번에 큰 chunk를 pipe로
 * 읽어들인다. recv_buf_size 옵션 변경 시 이 함수가 호출되어 기존 pending 데이터를 새 pipe로 옮긴다.
 *
 * 호출 체인: posix_sock_init / posix_sock_set_recvbuf → [이 함수] → spdk_pipe_create + spdk_iovcpy.
 */
static int
posix_sock_alloc_pipe(struct spdk_posix_sock *sock, int sz)
{
	uint8_t *new_buf, *old_buf;
	struct spdk_pipe *new_pipe;
	struct iovec siov[2];
	struct iovec diov[2];
	int sbytes;
	ssize_t bytes;
	int rc;

	if (sock->recv_buf_sz == sz) {
		return 0;
	}

	/* If the new size is 0, just free the pipe */
	if (sz == 0) {
		old_buf = spdk_pipe_destroy(sock->recv_pipe);
		free(old_buf);
		sock->recv_pipe = NULL;
		return 0;
	} else if (sz < MIN_SOCK_PIPE_SIZE) {
		SPDK_ERRLOG("The size of the pipe must be larger than %d\n", MIN_SOCK_PIPE_SIZE);
		return -1;
	}

	/* Round up to next 64 byte multiple */
	rc = posix_memalign((void **)&new_buf, 64, sz);
	if (rc != 0) {
		SPDK_ERRLOG("socket recv buf allocation failed\n");
		return -ENOMEM;
	}
	memset(new_buf, 0, sz);

	new_pipe = spdk_pipe_create(new_buf, sz);
	if (new_pipe == NULL) {
		SPDK_ERRLOG("socket pipe allocation failed\n");
		free(new_buf);
		return -ENOMEM;
	}

	if (sock->recv_pipe != NULL) {
		/* Pull all of the data out of the old pipe */
		sbytes = spdk_pipe_reader_get_buffer(sock->recv_pipe, sock->recv_buf_sz, siov);
		if (sbytes > sz) {
			/* Too much data to fit into the new pipe size */
			old_buf = spdk_pipe_destroy(new_pipe);
			free(old_buf);
			return -EINVAL;
		}

		sbytes = spdk_pipe_writer_get_buffer(new_pipe, sz, diov);
		assert(sbytes == sz);

		bytes = spdk_iovcpy(siov, 2, diov, 2);
		spdk_pipe_writer_advance(new_pipe, bytes);

		old_buf = spdk_pipe_destroy(sock->recv_pipe);
		free(old_buf);
	}

	sock->recv_buf_sz = sz;
	sock->recv_pipe = new_pipe;

	if (sock->base.group_impl) {
		struct spdk_posix_sock_group_impl *group;

		group = __posix_group_impl(sock->base.group_impl);
		spdk_pipe_group_add(group->pipe_group, sock->recv_pipe);
	}

	return 0;
}

/*
 * [한국어]
 * posix_sock_set_recvbuf - SO_RCVBUF 런타임 변경 (vtable set_recvbuf).
 *
 * @sz: 새 버퍼 크기. MIN_SO_RCVBUF_SIZE보다 작으면 그 값으로 올림.
 *
 * 동작: enable_recv_pipe면 사용자 공간 recv_pipe도 같은 크기로 재할당, 그 다음
 * setsockopt(SO_RCVBUF)로 커널 버퍼 갱신.
 *
 * 사전조건: connect 진행 중이면 connect_ctx에 미리 저장 — connect 완료 후 적용.
 */
static int
posix_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int min_size;
	int rc;

	assert(sock != NULL);

	if (!sock->ready) {
		if (sock->connect_ctx) {
			sock->connect_ctx->set_recvbuf = sz;       /* [한국어] connect 완료 후 적용 예약. */
			return 0;
		}

		SPDK_ERRLOG("Connection failed.\n");
		return -ENOTCONN;
	}

	if (_sock->impl_opts.enable_recv_pipe) {
		rc = posix_sock_alloc_pipe(sock, sz);
		if (rc) {
			return rc;
		}
	}

	/* Set kernel buffer size to be at least MIN_SO_RCVBUF_SIZE and
	 * _sock->impl_opts.recv_buf_size. */
	min_size = spdk_max(MIN_SO_RCVBUF_SIZE, _sock->impl_opts.recv_buf_size);

	if (sz < min_size) {
		sz = min_size;
	}

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return -errno;
	}

	_sock->impl_opts.recv_buf_size = sz;
	return 0;
}

/*
 * [한국어]
 * posix_sock_set_sendbuf - SO_SNDBUF 런타임 변경 (vtable set_sendbuf).
 *
 * @sz: 새 송신 버퍼 크기, MIN_SO_SNDBUF_SIZE보다 작으면 그 값으로 올림.
 *
 * 큰 응답을 송신하기 직전에 늘려서 sendmsg가 EAGAIN으로 회귀하는 빈도를 줄임.
 */
static int
posix_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int min_size;
	int rc;

	assert(sock != NULL);

	if (!sock->ready) {
		if (sock->connect_ctx) {
			sock->connect_ctx->set_sendbuf = sz;
			return 0;
		}

		SPDK_ERRLOG("Connection failed.\n");
		return -ENOTCONN;
	}

	/* Set kernel buffer size to be at least MIN_SO_SNDBUF_SIZE and
	 * _sock->impl_opts.send_buf_size. */
	min_size = spdk_max(MIN_SO_SNDBUF_SIZE, _sock->impl_opts.send_buf_size);

	if (sz < min_size) {
		sz = min_size;
	}

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return -errno;
	}

	_sock->impl_opts.send_buf_size = sz;
	return 0;
}

/*
 * [한국어]
 * posix_sock_init - 새 fd에 SPDK sock 백엔드 공통 옵션 적용 (zerocopy/quickack/placement).
 *
 * @sock: 새로 만들어진 sock 구조체 (fd는 이미 set).
 * @enable_zero_copy: SO_ZEROCOPY 시도 여부 (caller가 zcopy 옵션과 server/client 구분으로 결정).
 *
 * 단계:
 *   1) SPDK_ZEROCOPY 빌드면 SO_ZEROCOPY setsockopt 시도 — 성공 시 sock->zcopy=true.
 *      sendmsg_idx를 UINT32_MAX로 초기화 — 커널 첫 번째 sendmsg notification id가 0이므로
 *      ++ 시 0이 되도록(monotonic 증가 + wrap 패턴).
 *   2) Linux면 TCP_QUICKACK setsockopt (delayed ACK off — short transaction latency 감소).
 *   3) placement_id 추출 — CPU id / cgroup mark / NAPI id 중 옵션이 정한 모드.
 *   4) PLACEMENT_MARK 모드면 g_map에 placement_id 등록 (group은 나중에 add_sock에서 결정).
 *   5) sock->ready=true (이제 I/O 가능).
 *
 * 호출 시점: posix_sock_connect / posix_sock_accept 완료 시.
 */
static void
posix_sock_init(struct spdk_posix_sock *sock, bool enable_zero_copy)
{
#if defined(SPDK_ZEROCOPY) || defined(__linux__)
	int flag;
	int rc;
#endif

#if defined(SPDK_ZEROCOPY)
	flag = 1;

	if (enable_zero_copy) {
		/* Try to turn on zero copy sends */
		rc = setsockopt(sock->fd, SOL_SOCKET, SO_ZEROCOPY, &flag, sizeof(flag)); /* [한국어] 커널 4.14+ MSG_ZEROCOPY 활성. */
		if (rc == 0) {
			sock->zcopy = true;                          /* [한국어] writev에서 MSG_ZEROCOPY 플래그 부여. */
			/* Zcopy notification index from the kernel for first sendmsg is 0, so we need to start
			 * incrementing internal counter from UINT32_MAX. */
			sock->sendmsg_idx = UINT32_MAX;              /* [한국어] ++ 시 0이 되도록 wrap 시작점. */
		}
	}
#endif

#if defined(__linux__)
	flag = 1;

	if (sock->base.impl_opts.enable_quickack) {
		rc = setsockopt(sock->fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag)); /* [한국어] delayed ACK 끄기. */
		if (rc != 0) {
			SPDK_ERRLOG("quickack was failed to set\n");
		}
	}

	spdk_sock_get_placement_id(sock->fd, sock->base.impl_opts.enable_placement_id,
				   &sock->placement_id);              /* [한국어] CPU/MARK/NAPI 중 옵션 모드로 placement_id 추출. */

	if (sock->base.impl_opts.enable_placement_id == PLACEMENT_MARK) {
		/* Save placement_id */
		spdk_sock_map_insert(&g_map, sock->placement_id, NULL); /* [한국어] MARK 모드는 group=NULL hint만 등록 (group은 add_sock에서). */
	}
#elif defined(__FreeBSD__)
	spdk_sock_get_placement_id(sock->fd, sock->base.impl_opts.enable_placement_id,
				   &sock->placement_id);
#endif
	sock->ready = true;                                  /* [한국어] I/O 가능 상태로 전환. */
}

/*
 * [한국어]
 * posix_sock_alloc - spdk_posix_sock 객체 할당 + impl_opts 사본 보관.
 *
 * @fd: 이미 만들어진 socket fd.
 * @impl_opts: 백엔드 옵션 — sock per-instance에 사본으로 저장(전역 변경에 영향 안 받음).
 * @return: 할당된 sock 또는 NULL.
 */
static struct spdk_posix_sock *
posix_sock_alloc(int fd, struct spdk_sock_impl_opts *impl_opts)
{
	struct spdk_posix_sock *sock;

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		return NULL;
	}

	sock->fd = fd;
	memcpy(&sock->base.impl_opts, impl_opts, sizeof(*impl_opts));
	return sock;
}

/*
 * [한국어]
 * posix_sock_psk_find_session_server_cb - OpenSSL PSK 서버측 콜백 (TLS handshake 도중 호출).
 *
 * @ssl: SSL session 핸들.
 * @identity: 클라이언트가 보낸 PSK identity 문자열 (NUL terminated).
 * @identity_len: identity 길이.
 * @sess: 출력 — caller(OpenSSL)에 새 SSL_SESSION 반환.
 * @return: 1=성공, 0=실패 (handshake abort).
 *
 * 동기: TLS 1.3 PSK 모드는 server가 identity를 보고 key를 찾아주는 콜백을 요구한다.
 * SPDK는 impl_opts->psk_key/psk_identity 또는 get_key callback로 등록된 key를 사용한다.
 *
 * 흐름:
 *   1) impl_opts->get_key가 있으면 사용자 정의 함수로 동적 key 추출.
 *   2) 없으면 정적 psk_key를 사용 (identity 일치 검증).
 *   3) SSL_SESSION_new + cipher 매칭 + master key 설정 후 *sess에 반환.
 *
 * 호출 컨텍스트: SSL_accept 내부 — OpenSSL이 BIO_*를 통해 핸드셰이크 처리 중 호출.
 */
static int
posix_sock_psk_find_session_server_cb(SSL *ssl, const unsigned char *identity,
				      size_t identity_len, SSL_SESSION **sess)
{
	struct spdk_sock_impl_opts *impl_opts = SSL_get_app_data(ssl); /* [한국어] configure_ssl에서 app_data로 심어둔 옵션 회수. */
	uint8_t key[SSL_MAX_MASTER_KEY_LENGTH] = {};         /* [한국어] 찾아낸 PSK 원본 바이트(최대 master key 길이). */
	int keylen;                                          /* [한국어] 실제 key 길이(바이트). */
	int rc, i;                                           /* [한국어] OpenSSL 반환값 / cipher 순회 인덱스. */
	STACK_OF(SSL_CIPHER) *ciphers;                       /* [한국어] 이 핸드셰이크에서 클라이언트가 제시한 cipher 목록. */
	const SSL_CIPHER *cipher;                            /* [한국어] 순회 중인 단일 cipher. */
	const char *cipher_name;                             /* [한국어] cipher의 텍스트 이름(매칭용). */
	const char *user_cipher = NULL;                      /* [한국어] 운영자/콜백이 요구한 cipher 이름. */
	bool found = false;                                  /* [한국어] 요구 cipher가 제시 목록에 있는지. */

	if (impl_opts->get_key) {                            /* [한국어] 동적 key 조회 콜백이 등록된 경우(keyring 등). */
		rc = impl_opts->get_key(key, sizeof(key), &user_cipher, identity, impl_opts->get_key_ctx); /* [한국어] identity로 key+cipher 조회. */
		if (rc < 0) {                                /* [한국어] 해당 identity의 PSK 없음. */
			SPDK_ERRLOG("Unable to find PSK for identity: %s\n", identity);
			return 0;                            /* [한국어] 0 반환 → OpenSSL handshake abort. */
		}
		keylen = rc;                                 /* [한국어] 콜백은 key 길이를 반환. */
	} else {                                             /* [한국어] 정적 PSK 사용 경로. */
		if (impl_opts->psk_key == NULL) {            /* [한국어] 정적 key 미설정. */
			SPDK_ERRLOG("PSK is not set\n");
			return 0;
		}

		if (impl_opts->psk_identity == NULL) {       /* [한국어] 기대 identity 미설정. */
			SPDK_ERRLOG("PSK identity is not set\n");
			return 0;
		}

		SPDK_DEBUGLOG(sock_posix, "Length of Client's PSK ID %lu\n", strlen(impl_opts->psk_identity));
		if (strcmp(impl_opts->psk_identity, identity) != 0) { /* [한국어] 클라이언트가 보낸 identity가 기대값과 다르면 거절. */
			SPDK_ERRLOG("Unknown Client's PSK ID\n");
			return 0;
		}
		keylen = impl_opts->psk_key_size;            /* [한국어] 정적 key 길이. */

		memcpy(key, impl_opts->psk_key, keylen);     /* [한국어] 정적 key를 로컬 버퍼로 복사. */
		user_cipher = impl_opts->tls_cipher_suites;  /* [한국어] 정적 경로의 cipher는 옵션에서 가져옴. */
	}

	if (user_cipher == NULL) {                           /* [한국어] cipher가 결정되지 않으면 진행 불가. */
		SPDK_ERRLOG("Cipher suite not set\n");
		return 0;
	}

	*sess = SSL_SESSION_new();                           /* [한국어] PSK를 담을 새 SSL_SESSION 생성. */
	if (*sess == NULL) {
		SPDK_ERRLOG("Unable to allocate new SSL session\n");
		return 0;
	}

	ciphers = SSL_get_ciphers(ssl);                      /* [한국어] 클라이언트 제시 cipher 목록 획득. */
	for (i = 0; i < sk_SSL_CIPHER_num(ciphers); i++) {   /* [한국어] 목록을 순회하며 요구 cipher 탐색. */
		cipher = sk_SSL_CIPHER_value(ciphers, i);    /* [한국어] i번째 cipher. */
		cipher_name = SSL_CIPHER_get_name(cipher);   /* [한국어] 이름 추출. */

		if (strcmp(user_cipher, cipher_name) == 0) { /* [한국어] 요구 cipher와 일치. */
			rc = SSL_SESSION_set_cipher(*sess, cipher); /* [한국어] 세션에 이 cipher 바인딩. */
			if (rc != 1) {
				SPDK_ERRLOG("Unable to set cipher: %s\n", cipher_name);
				goto err;                    /* [한국어] 실패 시 세션 정리. */
			}
			found = true;
			break;                               /* [한국어] 첫 매칭에서 종료. */
		}
	}
	if (found == false) {                                /* [한국어] 요구 cipher가 목록에 없음 → 협상 불가. */
		SPDK_ERRLOG("No suitable cipher found\n");
		goto err;
	}

	SPDK_DEBUGLOG(sock_posix, "Cipher selected: %s\n", cipher_name);

	rc = SSL_SESSION_set_protocol_version(*sess, TLS1_3_VERSION); /* [한국어] PSK는 TLS 1.3에서만 — 세션 버전 고정. */
	if (rc != 1) {
		SPDK_ERRLOG("Unable to set TLS version: %d\n", TLS1_3_VERSION);
		goto err;
	}

	rc = SSL_SESSION_set1_master_key(*sess, key, keylen); /* [한국어] PSK를 master key로 주입(_set1=내부 복사). */
	if (rc != 1) {
		SPDK_ERRLOG("Unable to set PSK for session\n");
		goto err;
	}

	return 1;                                            /* [한국어] 1=성공 → OpenSSL이 이 세션으로 핸드셰이크 진행. */

err:
	SSL_SESSION_free(*sess);                             /* [한국어] 실패 시 세션 해제. */
	*sess = NULL;                                        /* [한국어] OpenSSL에 "세션 없음" 통지. */
	return 0;                                            /* [한국어] 0=handshake abort. */
}

/*
 * [한국어]
 * posix_sock_psk_use_session_client_cb - OpenSSL PSK 클라이언트측 콜백 (handshake 도중).
 *
 * @md: hash 알고리즘 (PSK binder 생성에 사용).
 * @identity: 출력 — 서버에 보낼 identity 포인터.
 * @identity_len: 출력 — identity 길이.
 * @sess: 출력 — handshake에 사용할 SSL_SESSION (cipher+master key 미리 설정).
 *
 * 클라이언트는 자기 psk_identity/psk_key를 그대로 사용 — 서버측처럼 lookup 필요 없음.
 */
static int
posix_sock_psk_use_session_client_cb(SSL *ssl, const EVP_MD *md, const unsigned char **identity,
				     size_t *identity_len, SSL_SESSION **sess)
{
	struct spdk_sock_impl_opts *impl_opts = SSL_get_app_data(ssl); /* [한국어] app_data로 심어둔 옵션 회수. */
	int rc, i;                                           /* [한국어] OpenSSL 반환값 / cipher 순회 인덱스. */
	STACK_OF(SSL_CIPHER) *ciphers;                       /* [한국어] 협상 가능한 cipher 목록. */
	const SSL_CIPHER *cipher;                            /* [한국어] 순회 중 단일 cipher. */
	const char *cipher_name;                             /* [한국어] cipher 이름(매칭용). */
	long keylen;                                          /* [한국어] PSK 길이(바이트). */
	bool found = false;                                  /* [한국어] 요구 cipher 매칭 여부. */

	if (impl_opts->psk_key == NULL) {                    /* [한국어] 클라이언트는 자기 정적 key 필수. */
		SPDK_ERRLOG("PSK is not set\n");
		return 0;
	}
	if (impl_opts->psk_key_size > SSL_MAX_MASTER_KEY_LENGTH) { /* [한국어] master key 한계 초과 방어. */
		SPDK_ERRLOG("PSK too long\n");
		return 0;
	}
	keylen = impl_opts->psk_key_size;                    /* [한국어] key 길이 확정. */

	if (impl_opts->psk_identity == NULL) {               /* [한국어] 서버에 보낼 identity 필수. */
		SPDK_ERRLOG("PSK identity is not set\n");
		return 0;
	}

	if (impl_opts->tls_cipher_suites == NULL) {          /* [한국어] cipher 미설정이면 진행 불가. */
		SPDK_ERRLOG("Cipher suite not set\n");
		return 0;
	}
	*sess = SSL_SESSION_new();                           /* [한국어] PSK 세션 생성. */
	if (*sess == NULL) {
		SPDK_ERRLOG("Unable to allocate new SSL session\n");
		return 0;
	}

	ciphers = SSL_get_ciphers(ssl);                      /* [한국어] 협상 가능 cipher 목록. */
	for (i = 0; i < sk_SSL_CIPHER_num(ciphers); i++) {   /* [한국어] 요구 cipher 탐색. */
		cipher = sk_SSL_CIPHER_value(ciphers, i);
		cipher_name = SSL_CIPHER_get_name(cipher);

		if (strcmp(impl_opts->tls_cipher_suites, cipher_name) == 0) { /* [한국어] 요구 cipher 일치. */
			rc = SSL_SESSION_set_cipher(*sess, cipher); /* [한국어] 세션에 cipher 바인딩. */
			if (rc != 1) {
				SPDK_ERRLOG("Unable to set cipher: %s\n", cipher_name);
				goto err;
			}
			found = true;
			break;
		}
	}
	if (found == false) {                                /* [한국어] 요구 cipher가 목록에 없음. */
		SPDK_ERRLOG("No suitable cipher found\n");
		goto err;
	}

	SPDK_DEBUGLOG(sock_posix, "Cipher selected: %s\n", cipher_name);

	rc = SSL_SESSION_set_protocol_version(*sess, TLS1_3_VERSION); /* [한국어] PSK는 TLS 1.3 전용. */
	if (rc != 1) {
		SPDK_ERRLOG("Unable to set TLS version: %d\n", TLS1_3_VERSION);
		goto err;
	}

	rc = SSL_SESSION_set1_master_key(*sess, impl_opts->psk_key, keylen); /* [한국어] 클라이언트 정적 key를 master key로 주입. */
	if (rc != 1) {
		SPDK_ERRLOG("Unable to set PSK for session\n");
		goto err;
	}

	*identity_len = strlen(impl_opts->psk_identity);     /* [한국어] 서버에 보낼 identity 길이 출력. */
	*identity = impl_opts->psk_identity;                 /* [한국어] identity 포인터 출력(OpenSSL이 ClientHello에 실음). */

	return 1;                                            /* [한국어] 1=성공 → 이 세션·identity로 핸드셰이크. */

err:
	SSL_SESSION_free(*sess);                             /* [한국어] 실패 시 세션 해제. */
	*sess = NULL;
	return 0;                                            /* [한국어] 0=abort. */
}

/*
 * [한국어]
 * posix_sock_create_ssl_context - SSL 백엔드용 OpenSSL SSL_CTX(컨텍스트) 생성·설정.
 *
 * @method: TLS_server_method()(accept측) 또는 TLS_client_method()(connect측) — 협상 역할 결정.
 * @impl_opts: tls_version/enable_ktls/tls_cipher_suites 등 SSL 정책 옵션.
 * @return: 설정된 SSL_CTX 포인터, 실패 시 NULL.
 *
 * 동기: 하나의 fd 위에 TLS를 얹으려면 먼저 SSL_CTX(여러 SSL 세션이 공유하는 설정 컨테이너)를
 * 만들고 최소/최대 프로토콜 버전, kTLS off-load, cipher suite 화이트리스트를 강제해야 한다.
 * 이 함수는 그 정책을 옵션에서 읽어 한 번에 적용한다.
 *
 * 동작 단계:
 *   1) OpenSSL 전역 초기화(라이브러리/알고리즘/에러 문자열 로드).
 *   2) SSL_CTX_new(method)로 컨텍스트 생성.
 *   3) tls_version 옵션에 따라 min/max proto version을 1.3으로 고정(또는 0=자동 협상).
 *   4) enable_ktls면 SSL_OP_ENABLE_KTLS 플래그로 커널 TLS off-load 시도(빌드 미지원 시 에러).
 *   5) tls_cipher_suites가 있으면 SSL_CTX_set_ciphersuites로 허용 cipher 제한.
 *
 * 실행 컨텍스트: connect/accept 경로(단일 reactor 스레드)에서 동기 호출. 재진입 없음.
 * 에러 시 err: 라벨에서 SSL_CTX_free로 컨텍스트를 회수하고 NULL 반환.
 *
 * 호출 체인: posix_sock_configure_ssl → [이 함수] → OpenSSL SSL_CTX_*.
 */
static SSL_CTX *
posix_sock_create_ssl_context(const SSL_METHOD *method, struct spdk_sock_impl_opts *impl_opts)
{
	SSL_CTX *ctx;                                        /* [한국어] 생성할 SSL 컨텍스트 핸들. */
	int tls_version = 0;                                 /* [한국어] 강제할 TLS 버전(0=자동 협상). */
	bool ktls_enabled = false;                           /* [한국어] kTLS off-load 활성 여부 결과. */
#ifdef SSL_OP_ENABLE_KTLS
	long options;                                        /* [한국어] SSL_CTX_set_options 반환 비트마스크(kTLS 적용 확인용). */
#endif

	SSL_library_init();                                  /* [한국어] OpenSSL libssl 초기화(레거시 API, 멱등). */
	OpenSSL_add_all_algorithms();                        /* [한국어] 모든 암호/해시 알고리즘 등록. */
	SSL_load_error_strings();                            /* [한국어] 에러 코드→문자열 테이블 로드(디버깅용). */
	/* Produce a SSL CTX in SSL V2 and V3 standards compliant way */
	ctx = SSL_CTX_new(method);                           /* [한국어] server/client method로 컨텍스트 생성. */
	if (!ctx) {                                          /* [한국어] 메모리/내부 오류 시 NULL. */
		SPDK_ERRLOG("SSL_CTX_new() failed, msg = %s\n", ERR_error_string(ERR_peek_last_error(), NULL));
		return NULL;                                 /* [한국어] OpenSSL 에러 큐 메시지 출력 후 실패 반환. */
	}
	SPDK_DEBUGLOG(sock_posix, "SSL context created\n");

	switch (impl_opts->tls_version) {                    /* [한국어] 운영자가 지정한 TLS 버전 정책. */
	case 0:
		/* auto-negotiation */
		break;                                       /* [한국어] 0=OpenSSL 자동(보통 TLS 1.3). */
	case SPDK_TLS_VERSION_1_3:
		tls_version = TLS1_3_VERSION;                /* [한국어] 1.3 강제 — 아래에서 min/max 동일 set. */
		break;
	default:
		SPDK_ERRLOG("Incorrect TLS version provided: %d\n", impl_opts->tls_version);
		goto err;                                    /* [한국어] 미지원 버전 — 컨텍스트 정리 후 NULL. */
	}

	if (tls_version) {                                   /* [한국어] 버전 고정이 요청된 경우만. */
		SPDK_DEBUGLOG(sock_posix, "Hardening TLS version to '%d'='0x%X'\n", impl_opts->tls_version,
			      tls_version);
		if (!SSL_CTX_set_min_proto_version(ctx, tls_version)) { /* [한국어] 최소 버전 = 1.3 (downgrade 차단). */
			SPDK_ERRLOG("Unable to set Min TLS version to '%d'='0x%X\n", impl_opts->tls_version, tls_version);
			goto err;
		}
		if (!SSL_CTX_set_max_proto_version(ctx, tls_version)) { /* [한국어] 최대 버전 = 1.3 (오직 1.3만 협상). */
			SPDK_ERRLOG("Unable to set Max TLS version to '%d'='0x%X\n", impl_opts->tls_version, tls_version);
			goto err;
		}
	}
	if (impl_opts->enable_ktls) {                        /* [한국어] 커널 TLS off-load 요청 시. */
		SPDK_DEBUGLOG(sock_posix, "Enabling kTLS offload\n");
#ifdef SSL_OP_ENABLE_KTLS
		options = SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS); /* [한국어] kTLS 플래그 set — 레코드 암복호를 커널/NIC로 위임. */
		ktls_enabled = options & SSL_OP_ENABLE_KTLS;     /* [한국어] 반환 비트마스크에서 실제 적용 여부 확인. */
#else
		ktls_enabled = false;                        /* [한국어] 빌드 시 kTLS 미지원 — 항상 실패 처리. */
#endif
		if (!ktls_enabled) {                         /* [한국어] off-load 불가 시 명시적 실패(운영자가 openssl을 enable-ktls로 빌드해야). */
			SPDK_ERRLOG("Unable to set kTLS offload via SSL_CTX_set_options(). Configure openssl with 'enable-ktls'\n");
			goto err;
		}
	}

	/* SSL_CTX_set_ciphersuites() return 1 if the requested
	 * cipher suite list was configured, and 0 otherwise. */
	if (impl_opts->tls_cipher_suites != NULL &&          /* [한국어] cipher 화이트리스트가 지정됐고. */
	    SSL_CTX_set_ciphersuites(ctx, impl_opts->tls_cipher_suites) != 1) { /* [한국어] 설정 실패(잘못된 cipher 이름)면. */
		SPDK_ERRLOG("Unable to set TLS cipher suites for SSL'\n");
		goto err;
	}

	return ctx;                                          /* [한국어] 정책 적용 완료된 컨텍스트 반환. */

err:
	SSL_CTX_free(ctx);                                   /* [한국어] 부분 설정된 컨텍스트 회수. */
	return NULL;
}

/*
 * [한국어]
 * ssl_sock_setup_connect - 클라이언트측 SSL 세션 생성 + connect-state로 초기화.
 *
 * @ctx: posix_sock_create_ssl_context가 만든 SSL_CTX.
 * @fd: TLS를 얹을 이미 connect된 TCP socket fd.
 * @return: 새 SSL 세션 핸들, 실패 시 NULL.
 *
 * 동작: SSL_new로 세션 생성 → SSL_set_fd로 fd 바인딩 → SSL_set_connect_state로 클라이언트
 * 핸드셰이크 모드 설정 → PSK use-session 콜백 등록(클라이언트는 자기 identity/key를 그대로 사용).
 * 실제 핸드셰이크는 이후 SSL_read/SSL_write 첫 호출 시 시작된다.
 *
 * 호출 체인: posix_sock_configure_ssl(client=true) → [이 함수] → OpenSSL SSL_*.
 */
static SSL *
ssl_sock_setup_connect(SSL_CTX *ctx, int fd)
{
	SSL *ssl;                                            /* [한국어] 생성할 SSL 세션 핸들. */

	ssl = SSL_new(ctx);                                  /* [한국어] 컨텍스트로부터 세션 인스턴스 생성. */
	if (!ssl) {                                          /* [한국어] 할당 실패 시. */
		SPDK_ERRLOG("SSL_new() failed, msg = %s\n", ERR_error_string(ERR_peek_last_error(), NULL));
		return NULL;
	}
	SSL_set_fd(ssl, fd);                                 /* [한국어] TLS 레코드 I/O가 이 fd로 read/write되도록 바인딩. */
	SSL_set_connect_state(ssl);                          /* [한국어] 클라이언트(ClientHello를 먼저 보냄) 역할 지정. */
	SSL_set_psk_use_session_callback(ssl, posix_sock_psk_use_session_client_cb); /* [한국어] TLS 1.3 PSK 콜백 등록. */
	SPDK_DEBUGLOG(sock_posix, "SSL object creation finished: %p\n", ssl);
	SPDK_DEBUGLOG(sock_posix, "%s = SSL_state_string_long(%p)\n", SSL_state_string_long(ssl), ssl);
	SPDK_DEBUGLOG(sock_posix, "Negotiated Cipher suite:%s\n",
		      SSL_CIPHER_get_name(SSL_get_current_cipher(ssl)));
	return ssl;                                          /* [한국어] connect-state로 준비된 세션 반환. */
}

/*
 * [한국어]
 * ssl_sock_setup_accept - 서버측 SSL 세션 생성 + accept-state로 초기화.
 *
 * @ctx: SSL_CTX (TLS_server_method 기반).
 * @fd: accept(2)로 막 수락한 connection fd.
 * @return: 새 SSL 세션, 실패 시 NULL.
 *
 * 동작: ssl_sock_setup_connect와 대칭 — set_accept_state로 서버(ClientHello를 기다림) 역할 설정,
 * PSK find-session 콜백 등록(서버는 클라이언트가 보낸 identity로 key를 lookup해야 하므로).
 *
 * 호출 체인: posix_sock_configure_ssl(client=false) → [이 함수] → OpenSSL SSL_*.
 */
static SSL *
ssl_sock_setup_accept(SSL_CTX *ctx, int fd)
{
	SSL *ssl;                                            /* [한국어] 생성할 SSL 세션 핸들. */

	ssl = SSL_new(ctx);                                  /* [한국어] 컨텍스트로부터 세션 생성. */
	if (!ssl) {
		SPDK_ERRLOG("SSL_new() failed, msg = %s\n", ERR_error_string(ERR_peek_last_error(), NULL));
		return NULL;
	}
	SSL_set_fd(ssl, fd);                                 /* [한국어] accept된 fd에 TLS 레코드 I/O 바인딩. */
	SSL_set_accept_state(ssl);                           /* [한국어] 서버 역할 — ClientHello 수신 대기. */
	SSL_set_psk_find_session_callback(ssl, posix_sock_psk_find_session_server_cb); /* [한국어] identity→key lookup 콜백 등록. */
	SPDK_DEBUGLOG(sock_posix, "SSL object creation finished: %p\n", ssl);
	SPDK_DEBUGLOG(sock_posix, "%s = SSL_state_string_long(%p)\n", SSL_state_string_long(ssl), ssl);
	SPDK_DEBUGLOG(sock_posix, "Negotiated Cipher suite:%s\n",
		      SSL_CIPHER_get_name(SSL_get_current_cipher(ssl)));
	return ssl;                                          /* [한국어] accept-state 세션 반환. */
}

/*
 * [한국어]
 * posix_sock_configure_ssl - 신규 fd에 SSL_CTX + SSL session을 셋업 (client or server).
 *
 * @client: true=connect, false=accept (set_connect_state vs set_accept_state).
 *
 * 흐름: posix_sock_create_ssl_context로 컨텍스트 생성 → setup_fn(SSL_new+set_fd+state)로
 * session 생성 → app_data에 impl_opts 연결 (PSK callback에서 SSL_get_app_data로 회수).
 */
static int
posix_sock_configure_ssl(struct spdk_posix_sock *sock, bool client)
{
	SSL* (*setup_fn)(SSL_CTX *, int) = client ? ssl_sock_setup_connect : ssl_sock_setup_accept; /* [한국어] client/server에 맞는 세션 셋업 함수 선택. */

	sock->ssl_ctx = posix_sock_create_ssl_context(client ? TLS_client_method() : TLS_server_method(),
			&sock->base.impl_opts);              /* [한국어] 역할별 method로 SSL_CTX 생성. */
	if (!sock->ssl_ctx) {                                /* [한국어] 컨텍스트 생성 실패. */
		SPDK_ERRLOG("posix_sock_create_ssl_context() failed\n");
		return -EPROTO;                              /* [한국어] 프로토콜 설정 오류 errno. */
	}

	sock->ssl = setup_fn(sock->ssl_ctx, sock->fd);       /* [한국어] 세션 생성 + fd 바인딩 + role state. */
	if (!sock->ssl) {                                    /* [한국어] 세션 생성 실패 시 컨텍스트 롤백. */
		SPDK_ERRLOG("ssl_sock_setup_%s() failed\n", client ? "connect" : "accept");
		SSL_CTX_free(sock->ssl_ctx);
		sock->ssl_ctx = NULL;
		return -EPROTO;
	}

	SSL_set_app_data(sock->ssl, &sock->base.impl_opts);  /* [한국어] PSK 콜백이 SSL_get_app_data로 회수할 옵션 포인터 심기. */
	return 0;
}

/*
 * [한국어]
 * posix_ssl_get_error - OpenSSL 에러 코드를 SPDK 표준 errno로 변환.
 *
 * WANT_READ/WRITE/CONNECT/ACCEPT/X509_LOOKUP/ASYNC 등은 -EAGAIN (재시도 필요).
 * ZERO_RETURN/SYSCALL/SSL은 -ENOTCONN (재시도 불가, 연결 종료/오류).
 */
static int
posix_ssl_get_error(SSL *ssl, int rc)
{
	switch (SSL_get_error(ssl, rc)) {                    /* [한국어] 직전 SSL_read/write의 상세 에러 코드 분류. */
	case SSL_ERROR_WANT_READ:                            /* [한국어] 핸드셰이크/레코드가 더 많은 입력 필요. */
	case SSL_ERROR_WANT_WRITE:                           /* [한국어] 출력 버퍼가 차서 더 못 씀(재시도). */
	case SSL_ERROR_WANT_CONNECT:                         /* [한국어] 하위 connect 미완료. */
	case SSL_ERROR_WANT_ACCEPT:                          /* [한국어] 하위 accept 미완료. */
	case SSL_ERROR_WANT_X509_LOOKUP:                     /* [한국어] 인증서 콜백이 재호출 요구. */
	case SSL_ERROR_WANT_ASYNC:                           /* [한국어] 비동기 엔진 진행 중. */
	case SSL_ERROR_WANT_ASYNC_JOB:                       /* [한국어] 비동기 job 슬롯 부족. */
	case SSL_ERROR_WANT_CLIENT_HELLO_CB:                 /* [한국어] ClientHello 콜백 재호출 요구. */
		return -EAGAIN;                              /* [한국어] 위 전부 "다음 polling에 재시도" → EAGAIN. */
	case SSL_ERROR_ZERO_RETURN:                          /* [한국어] TLS close_notify 수신 = 정상 종료. */
	case SSL_ERROR_SYSCALL:                              /* [한국어] 하위 socket 레벨 오류. */
	case SSL_ERROR_SSL:                                  /* [한국어] 프로토콜 위반 등 치명적 오류. */
	default:
		return -ENOTCONN;                            /* [한국어] 재시도 불가 — 연결 종료로 보고. */
	}
}

/*
 * [한국어]
 * posix_ssl_readv - SSL_read를 iov마다 호출하여 readv 의미론 모방.
 *
 * SSL은 iovec을 직접 지원하지 않아 단편 호출이 필요. 각 iov[i] 만큼 SSL_read 시도하다가
 * 부분 읽기 발생 시 break. total > 0이면 그 길이 반환, 아니면 변환된 에러.
 */
static ssize_t
posix_ssl_readv(SSL *ssl, const struct iovec *iov, int iovcnt)
{
	int i, rc = 0;                                       /* [한국어] iov 인덱스 / 마지막 SSL_read 결과. */
	ssize_t total = 0;                                   /* [한국어] 누적 복호화 바이트. */

	for (i = 0; i < iovcnt; i++) {                       /* [한국어] iovec을 직접 못 받으므로 조각별 호출. */
		rc = SSL_read(ssl, iov[i].iov_base, iov[i].iov_len); /* [한국어] TLS 레코드 복호 → 사용자 버퍼. */

		if (rc > 0) {                                /* [한국어] 일부라도 읽었으면 누적. */
			total += rc;
		}
		if (rc != (int)iov[i].iov_len) {             /* [한국어] 요청보다 적게 읽음 = 더 줄 데이터 없음 → 중단. */
			break;
		}
	}
	if (total > 0) {                                     /* [한국어] 한 바이트라도 읽었으면 그 길이 반환. */
		return total;
	}

	return posix_ssl_get_error(ssl, rc);                 /* [한국어] 전혀 못 읽었으면 SSL 에러를 errno로 변환. */
}

/*
 * [한국어]
 * posix_ssl_writev - SSL_write를 iov마다 호출 (sendmsg with iovec 모방).
 */
static ssize_t
posix_ssl_writev(SSL *ssl, struct iovec *iov, int iovcnt)
{
	int i, rc = 0;                                       /* [한국어] iov 인덱스 / 마지막 SSL_write 결과. */
	ssize_t total = 0;                                   /* [한국어] 누적 암호화 송신 바이트. */

	for (i = 0; i < iovcnt; i++) {                       /* [한국어] 조각별 SSL_write(iovec 직접 미지원). */
		rc = SSL_write(ssl, iov[i].iov_base, iov[i].iov_len); /* [한국어] 사용자 데이터 → TLS 레코드 암호화·송신. */

		if (rc > 0) {                                /* [한국어] 일부 송신 성공. */
			total += rc;
		}
		if (rc != (int)iov[i].iov_len) {             /* [한국어] 부분 송신 = 송신 버퍼 압력 → 중단. */
			break;
		}
	}
	if (total > 0) {                                     /* [한국어] 송신된 바이트가 있으면 그 길이 반환. */
		return total;
	}

	return posix_ssl_get_error(ssl, rc);                 /* [한국어] 전혀 못 보냈으면 에러 변환. */
}

/*
 * [한국어]
 * _posix_sock_listen - posix/ssl 백엔드의 listen 구현 — server socket 생성 + bind + listen.
 *
 * @ip/port: 바인딩 주소·포트.
 * @opts: 사용자 sock_opts.
 * @enable_ssl: ssl 백엔드면 true.
 * @return: 새 listening sock, 실패 시 NULL.
 *
 * 흐름:
 *   1) _opts_get_impl_opts로 백엔드 default+caller impl_opts 병합.
 *   2) getaddrinfo로 ip/port 후보 res0 리스트 확보.
 *   3) 각 res에 대해 spdk_sock_posix_fd_create(SO_REUSEADDR/RCVBUF/SNDBUF/NODELAY 등 설정) →
 *      bind → listen(512) → set non-blocking → 첫 성공한 fd 채택.
 *   4) posix_sock_alloc + posix_sock_init(zerocopy server=enable_zerocopy_send_server).
 *
 * 호출: spdk_sock_listen_ext → impl->listen → [이 함수].
 */
static struct spdk_sock *
_posix_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts, bool enable_ssl)
{
	struct spdk_sock_impl_opts impl_opts;                /* [한국어] default+caller 병합된 백엔드 옵션 사본. */
	struct spdk_posix_sock *sock;                        /* [한국어] 생성할 listening sock. */
	struct addrinfo *res0;                               /* [한국어] getaddrinfo 결과 리스트 head. */
	int rc, fd = -1;                                     /* [한국어] syscall 결과 / 채택할 fd(-1=아직 없음). */

	assert(opts != NULL);
	if (enable_ssl) {                                    /* [한국어] SSL 백엔드면 ssl default에서 병합. */
		_opts_get_impl_opts(opts, &impl_opts, &g_ssl_impl_opts);
	} else {                                             /* [한국어] 일반 posix면 posix default에서 병합. */
		_opts_get_impl_opts(opts, &impl_opts, &g_posix_impl_opts);
	}

	res0 = spdk_sock_posix_getaddrinfo(ip, port);        /* [한국어] ip/port → 주소 후보 리스트(IPv4/IPv6). */
	if (!res0) {                                         /* [한국어] 해석 실패. */
		return NULL;
	}

	for (struct addrinfo *res = res0; res != NULL; res = res->ai_next) { /* [한국어] 후보를 순차 시도, 첫 성공 채택. */
retry:
		fd = spdk_sock_posix_fd_create(res, opts, &impl_opts); /* [한국어] socket() + SO_REUSEADDR/RCVBUF/SNDBUF/NODELAY 등 일괄 설정. */
		if (fd < 0) {                                /* [한국어] 이 family로 생성 실패 → 다음 후보. */
			continue;
		}

		rc = bind(fd, res->ai_addr, res->ai_addrlen); /* [한국어] 지정 주소·포트에 바인딩(커널). */
		if (rc != 0) {                               /* [한국어] bind 실패 분기. */
			SPDK_ERRLOG("bind() failed at port %d, errno = %d\n", port, errno);
			switch (errno) {
			case EINTR:
				/* interrupted? */
				close(fd);
				goto retry;                  /* [한국어] 시그널 인터럽트 — 같은 res로 재시도. */
			case EADDRNOTAVAIL:                  /* [한국어] 주소가 NIC에 없음 — 설정/setup 스크립트 안내. */
				SPDK_ERRLOG("IP address %s not available. "
					    "Verify IP address in config file "
					    "and make sure setup script is "
					    "run before starting spdk app.\n", ip);
			/* FALLTHROUGH */
			default:
				/* try next family */
				close(fd);
				fd = -1;
				continue;                    /* [한국어] 그 외 오류 — 다음 family 후보로. */
			}
		}

		rc = listen(fd, 512);                        /* [한국어] backlog 512로 수동(passive) 소켓 전환. */
		if (rc != 0) {                               /* [한국어] listen 실패는 치명적 — 즉시 중단. */
			SPDK_ERRLOG("listen() failed, errno = %d\n", errno);
			close(fd);
			fd = -1;
			break;
		}

		if (spdk_fd_set_nonblock(fd)) {              /* [한국어] non-blocking 전환(accept이 EAGAIN으로 즉시 반환되도록). */
			close(fd);
			fd = -1;
			break;
		}

		break;                                       /* [한국어] 첫 성공 후보로 확정. */
	}

	freeaddrinfo(res0);                                  /* [한국어] 후보 리스트 해제(채택한 fd는 독립). */
	if (fd < 0) {                                        /* [한국어] 모든 후보 실패. */
		return NULL;
	}

	sock = posix_sock_alloc(fd, &impl_opts);             /* [한국어] sock 객체 할당 + opts 사본 보관. */
	if (sock == NULL) {                                  /* [한국어] 할당 실패 시 fd 회수. */
		close(fd);
		return NULL;
	}

	/* Only enable zero copy for non-loopback and non-ssl sockets. */
	posix_sock_init(sock, opts->zcopy && !spdk_net_is_loopback(fd) && !enable_ssl &&
			impl_opts.enable_zerocopy_send_server); /* [한국어] loopback/SSL 제외 + 서버 zcopy 옵션 ON일 때만 zerocopy 활성. */
	return &sock->base;                                  /* [한국어] base 포인터로 상위에 반환. */
}

/*
 * [한국어]
 * _sock_posix_connect_async - posix_connect_ctx의 다음 addrinfo res로 non-blocking connect 시도.
 *
 * 동작: ctx->next_res부터 next로 진행하며 fd_create → fd_connect_async를 시도, 처음 성공하는
 * 후보의 fd를 ctx에 저장. timeout_tsc를 connect_timeout으로부터 계산하여 poller에 전달.
 *
 * 호출: sock_posix_connect_async(첫 시도) 또는 posix_connect_poller(현재 후보 실패 시 다음 후보).
 */
static int
_sock_posix_connect_async(struct posix_connect_ctx *ctx)
{
	int rc = -ENOENT, fd;                                /* [한국어] 결과(-ENOENT=후보 소진) / 생성된 fd. */

	/* It is either first execution or continuation; in that case invalid fd is expected. */
	assert(ctx->fd == -1);                               /* [한국어] 진입 시 직전 fd가 닫혀 있어야 함(불변식). */
	for (; ctx->next_res != NULL; ctx->next_res = ctx->next_res->ai_next) { /* [한국어] 남은 주소 후보를 순차 시도. */
		rc = spdk_sock_posix_fd_create(ctx->next_res, &ctx->opts, &ctx->impl_opts); /* [한국어] socket() + 옵션 설정. */
		if (rc < 0) {                                /* [한국어] 생성 실패 → 다음 후보. */
			continue;
		}

		fd = rc;                                     /* [한국어] 양수 반환은 새 fd. */
		rc = spdk_sock_posix_fd_connect_async(fd, ctx->next_res, &ctx->opts); /* [한국어] non-blocking connect 개시(EINPROGRESS 기대). */
		if (rc < 0) {                                /* [한국어] connect 즉시 실패 → fd 회수 후 다음 후보. */
			close(fd);
			continue;
		}

		ctx->next_res = ctx->next_res->ai_next;      /* [한국어] 성공 — 다음 재시도가 그 다음 후보부터 시작하도록 전진. */
		break;
	}

	if (rc < 0) {                                        /* [한국어] 모든 후보 실패. */
		return rc;
	}

	ctx->fd = fd;                                        /* [한국어] 진행 중 fd를 ctx에 보관(poller가 SO_ERROR 폴링). */
	ctx->timeout_tsc = !ctx->opts.connect_timeout ? 0 : spdk_get_ticks() + ctx->opts.connect_timeout *
			   spdk_get_ticks_hz() / 1000;       /* [한국어] connect 타임아웃을 절대 TSC tick으로 환산(0=무한). */
	return 0;
}

/*
 * [한국어]
 * sock_posix_connect_ctx_cleanup - connect_ctx 해제 + caller cb_fn 호출.
 *
 * @rc: caller에 통보할 결과(0=성공, 음수=실패 errno).
 *
 * freeaddrinfo로 getaddrinfo 결과 해제, cb_fn 등록되어 있으면 호출 후 ctx free.
 */
static void
sock_posix_connect_ctx_cleanup(struct posix_connect_ctx **_ctx, int rc)
{
	struct posix_connect_ctx *ctx = *_ctx;               /* [한국어] sock이 보관 중인 connect 컨텍스트. */

	*_ctx = NULL;                                        /* [한국어] sock->connect_ctx를 먼저 NULL로(재진입/이중 해제 방지). */
	if (!ctx) {                                          /* [한국어] 이미 정리됐으면 no-op. */
		return;
	}

	freeaddrinfo(ctx->first_res);                        /* [한국어] getaddrinfo 결과 리스트 해제. */
	if (ctx->cb_fn) {                                    /* [한국어] async connect였다면 caller에 결과 통보. */
		ctx->cb_fn(ctx->cb_arg, rc);                 /* [한국어] cb_fn(cb_arg, rc) — rc 0=성공/음수=errno. */
	}

	free(ctx);                                           /* [한국어] 컨텍스트 메모리 해제. */
}

/*
 * [한국어]
 * sock_posix_connect_async - posix_connect_ctx 할당 + 첫 _sock_posix_connect_async 호출.
 *
 * @res: getaddrinfo 결과 (caller가 lifetime 책임 — ctx가 first_res로 보관 후 cleanup에서 free).
 * @cb_fn/cb_arg: connect 완료 콜백.
 * @_ctx: 출력 — caller(sock)가 ctx 포인터 보관.
 */
static int
sock_posix_connect_async(struct addrinfo *res, struct spdk_sock_opts *opts,
			 struct spdk_sock_impl_opts *impl_opts, bool ssl, spdk_sock_connect_cb_fn cb_fn, void *cb_arg,
			 struct posix_connect_ctx **_ctx)
{
	struct posix_connect_ctx *ctx;                       /* [한국어] 새로 할당할 connect 진행 컨텍스트. */
	int rc;                                              /* [한국어] 첫 connect 시도 결과. */

	ctx = calloc(1, sizeof(*ctx));                       /* [한국어] 0-초기화 할당. */
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->first_res = ctx->next_res = res;                /* [한국어] head(cleanup용)와 진행 커서를 동일 res로 초기화. */
	ctx->opts = *opts;                                   /* [한국어] 사용자 opts 사본 보관(post-connect 적용용). */
	ctx->impl_opts = *impl_opts;                         /* [한국어] 백엔드 opts 사본. */
	ctx->ssl = ssl;                                      /* [한국어] SSL 핸드셰이크 필요 여부. */
	ctx->fd = -1;                                        /* [한국어] 아직 fd 없음(connect_async에서 set). */
	ctx->set_recvlowat = -1;                             /* [한국어] -1=지연 적용 예약 없음. */
	ctx->set_recvbuf = -1;                               /* [한국어] -1=지연 RCVBUF 적용 없음. */
	ctx->set_sendbuf = -1;                               /* [한국어] -1=지연 SNDBUF 적용 없음. */
	ctx->cb_fn = cb_fn;                                  /* [한국어] 완료 콜백(async일 때만 비-NULL). */
	ctx->cb_arg = cb_arg;                                /* [한국어] 콜백 인자. */

	rc = _sock_posix_connect_async(ctx);                 /* [한국어] 첫 후보로 non-blocking connect 개시. */
	if (rc < 0) {                                        /* [한국어] 모든 후보 즉시 실패 시 ctx 해제. */
		free(ctx);
		return rc;
	}

	*_ctx = ctx;                                         /* [한국어] caller(sock)에 ctx 포인터 전달. */
	return 0;
}

/* [한국어] forward decl — posix_connect_poller가 EAGAIN 폴링 + handshake 진행 책임. */
static int posix_connect_poller(struct spdk_posix_sock *sock);

/*
 * [한국어]
 * _posix_sock_connect - posix/ssl 백엔드의 connect 통합 구현.
 *
 * @async: true=non-blocking 반환(나중에 cb_fn 호출), false=완료까지 동기 대기.
 * @enable_ssl: SSL handshake 포함 여부.
 *
 * 동작:
 *   1) impl_opts 병합 + getaddrinfo.
 *   2) sock 객체 할당 (fd=-1로 시작).
 *   3) sock_posix_connect_async → 첫 후보 fd 생성 + non-blocking connect 시도.
 *   4) async=true면 sock 즉시 반환 (caller가 group_add 후 polling).
 *   5) async=false면 do { posix_connect_poller } while (EAGAIN) 동기 대기.
 *
 * 호출: spdk_sock_connect → impl->connect / impl->connect_async → [이 함수].
 */
static struct spdk_sock *
_posix_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts, bool async,
		    bool enable_ssl, spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	struct spdk_sock_impl_opts impl_opts;                /* [한국어] 병합된 백엔드 옵션 사본. */
	struct spdk_posix_sock *sock = NULL;                 /* [한국어] 생성할 sock(에러 경로 free 위해 NULL 초기화). */
	struct addrinfo *res0 = NULL;                        /* [한국어] getaddrinfo 결과(에러 시 free 위해 NULL 초기화). */
	int rc;                                              /* [한국어] 진행 결과. */

	assert(opts != NULL);
	if (enable_ssl) {                                    /* [한국어] 백엔드별 default opts 선택. */
		_opts_get_impl_opts(opts, &impl_opts, &g_ssl_impl_opts);
	} else {
		_opts_get_impl_opts(opts, &impl_opts, &g_posix_impl_opts);
	}

	res0 = spdk_sock_posix_getaddrinfo(ip, port);        /* [한국어] 대상 ip/port 후보 리스트. */
	if (!res0) {                                         /* [한국어] 해석 실패. */
		rc = -EIO;
		goto err;
	}

	sock = posix_sock_alloc(-1, &impl_opts);             /* [한국어] fd=-1로 sock 먼저 할당(connect_async가 채움). */
	if (!sock) {
		rc = -ENOMEM;
		goto err;
	}

	rc = sock_posix_connect_async(res0, opts, &impl_opts, enable_ssl, cb_fn, cb_arg,
				      &sock->connect_ctx);   /* [한국어] connect_ctx 할당 + 첫 후보 non-blocking connect. */
	if (rc < 0) {                                        /* [한국어] 모든 후보 즉시 실패(res0은 ctx가 소유했으므로 err에서 free 금지). */
		goto err;
	}

	sock->fd = sock->connect_ctx->fd;                    /* [한국어] 진행 중 fd를 sock에 노출(상위가 group_add 가능). */
	if (async) {                                         /* [한국어] async면 즉시 반환 — caller가 poller로 완료 감시. */
		return &sock->base;
	}

	do {
		rc = posix_connect_poller(sock);             /* [한국어] sync 경로: 완료될 때까지 직접 폴링. */
	} while (rc == -EAGAIN);                             /* [한국어] connect 진행 중이면 busy-wait. */

	if (!sock->ready) {                                  /* [한국어] 폴링 종료 후에도 미준비 = 실패. */
		free(sock);                                  /* [한국어] connect_ctx는 poller가 cleanup 완료. */
		return NULL;
	}

	return &sock->base;                                  /* [한국어] 연결 성공한 sock 반환. */

err:
	free(sock);                                          /* [한국어] sock 할당분 회수(NULL-safe). */
	if (res0) {                                          /* [한국어] connect_async 진입 전 실패 시에만 res0 직접 free. */
		freeaddrinfo(res0);
	}

	if (cb_fn) {                                         /* [한국어] async였다면 실패도 콜백으로 통보. */
		cb_fn(cb_arg, rc);
	}

	return NULL;
}

/*
 * [한국어]
 * posix_sock_listen - posix(non-SSL) 백엔드의 listen vtable 슬롯.
 *
 * @ip/port/opts: 바인딩 정보·옵션.
 * @return: listening sock 또는 NULL.
 * enable_ssl=false로 _posix_sock_listen에 위임하는 얇은 래퍼.
 * 호출 체인: spdk_sock_listen → g_posix_net_impl.listen → [이 함수] → _posix_sock_listen.
 */
static struct spdk_sock *
posix_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return _posix_sock_listen(ip, port, opts, false);    /* [한국어] SSL 비활성으로 공통 listen 위임. */
}

/*
 * [한국어]
 * posix_sock_connect - posix(non-SSL) 동기 connect vtable 슬롯.
 *
 * @return: 연결 완료된 sock 또는 NULL.
 * async=false, enable_ssl=false, cb 없음으로 _posix_sock_connect 위임 — 완료까지 블로킹.
 */
static struct spdk_sock *
posix_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return _posix_sock_connect(ip, port, opts, false, false, NULL, NULL); /* [한국어] sync, non-SSL. */
}

/*
 * [한국어]
 * posix_sock_connect_async - posix(non-SSL) 비동기 connect vtable 슬롯.
 *
 * @cb_fn/cb_arg: connect 완료 시 호출될 콜백(완료는 poller가 트리거).
 * async=true로 즉시 sock 반환, 실제 완료는 group poll 중 posix_connect_poller가 처리.
 */
static struct spdk_sock *
posix_sock_connect_async(const char *ip, int port, struct spdk_sock_opts *opts,
			 spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	return _posix_sock_connect(ip, port, opts, true, false, cb_fn, cb_arg); /* [한국어] async, non-SSL. */
}

/*
 * [한국어]
 * _posix_sock_accept - vtable accept 구현. listening sock에서 incoming connection 수락.
 *
 * 동작:
 *   1) socks_with_data 리스트에서 자기 자신 제거(epoll level-trigger 패턴).
 *   2) accept(2) — non-blocking이므로 EAGAIN이면 NULL 반환 (poller가 다음에 재시도).
 *   3) 새 fd에 SO_PRIORITY 재적용 (priority는 listening fd로부터 상속 안 됨).
 *   4) posix_sock_alloc + posix_sock_init.
 *   5) enable_ssl이면 posix_sock_configure_ssl로 OpenSSL handshake 준비.
 */
static struct spdk_sock *
_posix_sock_accept(struct spdk_sock *_sock, bool enable_ssl)
{
	struct spdk_posix_sock		*sock = __posix_sock(_sock); /* [한국어] listening sock(구체 타입). */
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(sock->base.group_impl); /* [한국어] 속한 group(있으면). */
	struct sockaddr_storage		sa;          /* [한국어] peer 주소 수신 버퍼(IPv4/IPv6 겸용). */
	socklen_t			salen;       /* [한국어] sa 길이 in/out. */
	int				rc, fd;      /* [한국어] syscall 결과 / 새 connection fd. */
	struct spdk_posix_sock		*new_sock;   /* [한국어] 수락된 연결용 새 sock. */

	memset(&sa, 0, sizeof(sa));                          /* [한국어] 주소 버퍼 0-초기화. */
	salen = sizeof(sa);                                  /* [한국어] accept이 채울 수 있는 최대 길이. */

	assert(sock != NULL);

	/* epoll_wait will trigger again if there is more than one request */
	if (group && sock->socket_has_data) {                /* [한국어] level-trigger: 한 번 accept했으니 has_data 리스트에서 제거. */
		sock->socket_has_data = false;
		TAILQ_REMOVE(&group->socks_with_data, sock, link);
	}

	rc = accept(sock->fd, (struct sockaddr *)&sa, &salen); /* [한국어] 대기 큐에서 연결 하나 수락(non-blocking → 없으면 EAGAIN). */

	if (rc == -1) {                                      /* [한국어] EAGAIN 포함 — 수락할 연결 없음. */
		return NULL;
	}

	fd = rc;                                             /* [한국어] 새 connection fd. */

	if (spdk_fd_set_nonblock(fd)) {                      /* [한국어] 새 fd도 non-blocking으로(상속 안 됨). */
		close(fd);
		return NULL;
	}

#if defined(SO_PRIORITY)
	/* The priority is not inherited, so call this function again */
	if (sock->base.opts.priority) {                      /* [한국어] SO_PRIORITY는 accept fd에 상속되지 않아 재적용. */
		rc = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &sock->base.opts.priority, sizeof(int));
		if (rc != 0) {
			close(fd);
			return NULL;
		}
	}
#endif

	new_sock = posix_sock_alloc(fd, &sock->base.impl_opts); /* [한국어] listening sock의 opts 사본으로 새 sock 할당. */
	if (new_sock == NULL) {
		close(fd);
		return NULL;
	}

	if (enable_ssl) {                                    /* [한국어] SSL 백엔드면 서버측 핸드셰이크 준비. */
		rc = posix_sock_configure_ssl(new_sock, false); /* [한국어] client=false → accept-state SSL 세션. */
		if (rc < 0) {
			free(new_sock);
			close(fd);
			return NULL;
		}
	}

	/* Inherit the zero copy feature from the listen socket */
	posix_sock_init(new_sock, sock->zcopy);              /* [한국어] zerocopy 활성 여부를 listening sock에서 상속. */
	return &new_sock->base;                              /* [한국어] 수락된 연결을 상위에 반환. */
}

/*
 * [한국어]
 * posix_sock_accept - posix(non-SSL) accept vtable 슬롯.
 *
 * @_sock: listening sock.
 * @return: 수락된 새 sock 또는 NULL(대기 연결 없음/오류).
 * enable_ssl=false로 _posix_sock_accept에 위임. group poll 중 EPOLLIN 시 상위가 호출.
 */
static struct spdk_sock *
posix_sock_accept(struct spdk_sock *_sock)
{
	return _posix_sock_accept(_sock, false);             /* [한국어] non-SSL accept 위임. */
}

/*
 * [한국어]
 * posix_sock_close - sock close + 모든 자원 해제 (vtable close).
 *
 * 순서:
 *   1) pending_reqs 비어있어야 함(assert) — caller가 모든 진행 중 req 정리한 후 호출.
 *   2) connect_ctx 진행 중이었으면 -ECONNRESET으로 cb_fn 호출 후 cleanup.
 *   3) SSL_shutdown → close(fd) → SSL_free → SSL_CTX_free → recv_pipe 해제.
 *   4) sock 객체 free.
 *
 * close(2) 실패 시 fd만 leak하고 나머지는 정리 — 자원 회수 best-effort.
 */
static int
posix_sock_close(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);  /* [한국어] 닫을 sock(구체 타입). */
	void *pipe_buf;                                      /* [한국어] recv_pipe가 소유하던 backing buffer 포인터. */

	assert(TAILQ_EMPTY(&_sock->pending_reqs));           /* [한국어] caller가 모든 진행 중 송신 req를 먼저 정리했어야 함(불변식). */

	sock_posix_connect_ctx_cleanup(&sock->connect_ctx, -ECONNRESET); /* [한국어] connect 진행 중이었으면 -ECONNRESET으로 콜백 후 정리. */

	if (sock->ssl != NULL) {                             /* [한국어] TLS 세션이 있으면 close_notify 전송. */
		SSL_shutdown(sock->ssl);
	}

	/* If the socket fails to close, the best choice is to
	 * leak the fd but continue to free the rest of the sock
	 * memory. */
	if (sock->fd != -1) {                                /* [한국어] 유효 fd면 커널 소켓 close. */
		close(sock->fd);
	}

	SSL_free(sock->ssl);                                 /* [한국어] SSL 세션 해제(NULL-safe). */
	SSL_CTX_free(sock->ssl_ctx);                         /* [한국어] SSL 컨텍스트 해제(NULL-safe). */

	if (sock->recv_pipe) {                               /* [한국어] 사용자 공간 recv_pipe가 있으면. */
		pipe_buf = spdk_pipe_destroy(sock->recv_pipe); /* [한국어] pipe 파괴 후 backing buffer 회수. */
		free(pipe_buf);
	}

	free(sock);                                          /* [한국어] sock 객체 해제. */
	return 0;
}

#ifdef SPDK_ZEROCOPY
/*
 * [한국어]
 * _sock_check_zcopy - MSG_ERRQUEUE에서 SO_EE_ORIGIN_ZEROCOPY 완료 통지를 수확.
 *
 * 동기: SO_ZEROCOPY로 송신한 데이터의 사용자 페이지는 커널이 NIC DMA 완료까지 보유하다가
 * 끝나면 MSG_ERRQUEUE에 sock_extended_err{ee_origin=ZEROCOPY, ee_info=lo, ee_data=hi}로
 * 통지한다. 이 범위[lo,hi]의 sequence number를 가진 pending_reqs를 완료 처리.
 *
 * 동작:
 *   1) recvmsg(MSG_ERRQUEUE)로 cmsg 추출 (반복 — 여러 통지 한 사이클에 수확).
 *   2) cmsg 유효성 검사 (SOL_IP/IPV6의 IP_RECVERR + ee_origin=ZEROCOPY).
 *   3) idx=ee_info부터 ee_data까지 순회하며 pending_reqs에서 zcopy_idx 매칭 req를 complete.
 *      비-zcopy req(zerocopy 안 한 한 req)도 순서대로 complete.
 *   4) queued_reqs의 첫 req가 partial-sent 상태로 pending_zcopy인데 이번 idx와 매칭되면
 *      pending_zcopy=false로 클리어 — 다음 chunk가 non-zcopy로 전송될 수 있게 함.
 *
 * NOTE: ee_info ≤ ee_data 범위에서 wrap 가능 (uint32_t monotonic counter). pending_reqs는
 * 같은 sendmsg의 req들이 연속 배치됨을 가정 — 첫 매칭 후 non-match가 나오면 break.
 */
static int
_sock_check_zcopy(struct spdk_sock *sock)
{
	struct spdk_posix_sock *psock = __posix_sock(sock);  /* [한국어] errqueue를 읽을 sock(구체 타입). */
	struct msghdr msgh = {};                             /* [한국어] recvmsg용 메시지 헤더(control message만 받음). */
	uint8_t buf[sizeof(struct cmsghdr) + sizeof(struct sock_extended_err)]; /* [한국어] cmsg 수신 버퍼(헤더+확장 에러). */
	ssize_t rc;                                          /* [한국어] recvmsg/request_put 결과. */
	struct sock_extended_err *serr;                      /* [한국어] 커널이 준 zerocopy 완료 통지 구조체. */
	struct cmsghdr *cm;                                  /* [한국어] control message 헤더 커서. */
	uint32_t idx;                                        /* [한국어] 완료 범위 순회 인덱스(sequence number). */
	struct spdk_sock_request *req, *treq;                /* [한국어] pending_reqs 순회 커서(SAFE 삭제용). */
	bool found;                                          /* [한국어] 현재 idx에 매칭되는 req를 찾았는지. */

	msgh.msg_control = buf;                              /* [한국어] cmsg를 받을 버퍼 지정. */
	msgh.msg_controllen = sizeof(buf);                  /* [한국어] 버퍼 용량. */

	while (true) {                                       /* [한국어] 한 사이클에 가능한 모든 통지 수확. */
		rc = recvmsg(psock->fd, &msgh, MSG_ERRQUEUE); /* [한국어] error queue에서 zerocopy 완료 통지 1건 읽기. */

		if (rc < 0) {                                /* [한국어] 더 읽을 통지 없거나 오류. */
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				return 0;                    /* [한국어] errqueue 비었음 — 정상 종료. */
			}

			if (!TAILQ_EMPTY(&sock->pending_reqs)) { /* [한국어] 오류인데 아직 미완료 req가 남음 = 고아 가능. */
				SPDK_ERRLOG("Attempting to receive from ERRQUEUE yielded error, but pending list still has orphaned entries\n");
			} else {
				SPDK_WARNLOG("Recvmsg yielded an error!\n");
			}
			return 0;
		}

		cm = CMSG_FIRSTHDR(&msgh);                   /* [한국어] 첫 control message. */
		if (!(cm &&                                  /* [한국어] cmsg가 존재하고. */
		      ((cm->cmsg_level == SOL_IP && cm->cmsg_type == IP_RECVERR) || /* [한국어] IPv4 확장 에러이거나. */
		       (cm->cmsg_level == SOL_IPV6 && cm->cmsg_type == IPV6_RECVERR)))) { /* [한국어] IPv6 확장 에러여야. */
			SPDK_WARNLOG("Unexpected cmsg level or type!\n");
			return 0;
		}

		serr = (struct sock_extended_err *)CMSG_DATA(cm); /* [한국어] cmsg payload를 확장 에러로 해석. */
		if (serr->ee_errno != 0 || serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY) { /* [한국어] zerocopy 완료 통지인지 확인. */
			SPDK_WARNLOG("Unexpected extended error origin\n");
			return 0;
		}

		/* Most of the time, the pending_reqs array is in the exact
		 * order we need such that all of the requests to complete are
		 * in order, in the front. It is guaranteed that all requests
		 * belonging to the same sendmsg call are sequential, so once
		 * we encounter one match we can stop looping as soon as a
		 * non-match is found.
		 */
		idx = serr->ee_info;                         /* [한국어] 완료 범위 시작 sequence number. */
		while (true) {                               /* [한국어] [ee_info, ee_data] 범위의 모든 idx 처리. */
			found = false;
			TAILQ_FOREACH_SAFE(req, &sock->pending_reqs, internal.link, treq) { /* [한국어] 미완료 req 순회. */
				if (!req->internal.pending_zcopy) {
					/* This wasn't a zcopy request. It was just waiting in line
					 * to complete. */
					rc = spdk_sock_request_put(sock, req, 0); /* [한국어] 비-zcopy req는 순서상 먼저 완료 처리. */
					if (rc < 0) {
						return rc;   /* [한국어] caller가 sock을 닫음. */
					}
				} else if (req->internal.zcopy_idx == idx) { /* [한국어] 이 통지 idx와 매칭되는 zcopy req. */
					found = true;
					rc = spdk_sock_request_put(sock, req, 0); /* [한국어] 페이지 회수 완료 → req complete. */
					if (rc < 0) {
						return rc;
					}
				} else if (found) {          /* [한국어] 같은 sendmsg 묶음을 벗어남 → 더 볼 필요 없음. */
					break;
				}
			}

			if (idx == serr->ee_data) {          /* [한국어] 범위 끝 도달. */
				break;
			}

			idx++;                               /* [한국어] 다음 sequence number로. */
		}

		/* If the req is sent partially (still queued) and we just received its zcopy
		 * notification, next chunk may be sent without zcopy and should result in the req
		 * completion if it is the last chunk. Clear the pending flag to allow it.
		 * Checking the first queued req and the last index is enough, because only one req
		 * can be partially sent and it is the last one we can get notification for. */
		req = TAILQ_FIRST(&sock->queued_reqs);       /* [한국어] 아직 송신 안 끝난 첫 큐 req. */
		if (req && req->internal.pending_zcopy &&    /* [한국어] 부분 송신 중이고 zcopy 대기였는데. */
		    req->internal.zcopy_idx == serr->ee_data) { /* [한국어] 이번 통지의 마지막 idx와 일치하면. */
			req->internal.pending_zcopy = false; /* [한국어] 다음 chunk가 non-zcopy로 완료될 수 있게 플래그 해제. */
		}
	}

	return 0;
}
#endif

/*
 * [한국어]
 * posix_writev - 단일 sendmsg(or SSL_write) 호출 wrapper.
 *
 * @flags: MSG_ZEROCOPY(zcopy 활성 시), MSG_NOSIGNAL 등.
 * @return: 송신된 byte 수 (>0), -EAGAIN(send buffer full / ENOBUFS during zcopy), -errno.
 *
 * SSL이 있으면 posix_ssl_writev로 분기 — OpenSSL은 sendmsg와 동일 API 모방. EAGAIN/EWOULDBLOCK은
 * non-fatal로 caller가 다음 polling 사이클에 재시도. ENOBUFS는 zcopy 압력(페이지 부족) 의미로
 * 동일하게 EAGAIN 처리.
 */
static int
posix_writev(struct spdk_posix_sock *sock, struct iovec *iov, int iovcnt, int flags)
{
	struct msghdr msg = {.msg_iov = iov, .msg_iovlen = iovcnt}; /* [한국어] sendmsg용 메시지 — iovec 묶음. */
	int rc;                                              /* [한국어] sendmsg 결과(송신 바이트). */

	if (sock->ssl) {                                     /* [한국어] TLS sock이면 SSL_write 경로로. */
		return posix_ssl_writev(sock->ssl, iov, iovcnt);
	}

	rc = sendmsg(sock->fd, &msg, flags);                 /* [한국어] 커널로 송신(flags에 MSG_ZEROCOPY/MSG_NOSIGNAL 포함 가능). */
	if (rc <= 0) {                                       /* [한국어] 송신 실패 또는 0. */
		if (rc == 0 || errno == EAGAIN || errno == EWOULDBLOCK || (errno == ENOBUFS && sock->zcopy)) {
			return -EAGAIN;                      /* [한국어] 버퍼 full / zcopy 페이지 부족(ENOBUFS) = 재시도 가능. */
		}

		return -errno;                               /* [한국어] 그 외는 치명적 오류. */
	}

	return rc;                                           /* [한국어] 송신된 바이트 수. */
}

/*
 * [한국어]
 * posix_readv - 단일 readv(or SSL_read) 호출 wrapper.
 *
 * SSL 분기 또는 readv(2) 그대로. -errno로 표준화.
 */
static int
posix_readv(struct spdk_posix_sock *sock, struct iovec *iov, int iovcnt)
{
	int rc;                                              /* [한국어] readv 결과(수신 바이트). */

	if (sock->ssl) {                                     /* [한국어] TLS sock이면 SSL_read 경로로. */
		return posix_ssl_readv(sock->ssl, iov, iovcnt);
	}

	rc = readv(sock->fd, iov, iovcnt);                   /* [한국어] 커널 소켓 버퍼에서 iovec으로 scatter read. */
	if (rc < 0) {                                        /* [한국어] EAGAIN 등은 음수 errno로 표준화. */
		return -errno;
	}

	return rc;                                           /* [한국어] 0=EOF, >0=수신 바이트. */
}

/*
 * [한국어]
 * _sock_flush - sock->queued_reqs를 한 번의 sendmsg(iovec 묶음)로 송신 시도 + 후처리.
 *
 * 동작:
 *   1) connect_poller로 진행 중 connect 완료 여부 확인 (handshake 포함).
 *   2) cb_cnt>0이면 재귀 호출 방지 — caller가 cb_fn 내부에서 flush 호출 시 -EAGAIN.
 *   3) zcopy 활성 sock이면 MSG_ZEROCOPY 플래그 추가, MSG_NOSIGNAL은 항상 포함(SIGPIPE 회피).
 *   4) spdk_sock_prep_reqs로 queued_reqs의 iovec들을 한 번에 IOV_BATCH_SIZE(=32)개까지 모음.
 *   5) posix_writev로 sendmsg 1회 호출 — 송신된 byte 수 rc.
 *   6) zcopy면 sendmsg_idx++ (다음 sendmsg에 새 ID 부여).
 *   7) queued_reqs를 head부터 순회하며 send된 byte 만큼 offset 진행:
 *      - 완전 송신 req: pending_reqs로 이동(spdk_sock_request_pend).
 *        zcopy면 pending_zcopy=true로 마킹하고 zcopy_idx 기록.
 *        zcopy=false이고 pending_reqs head면 즉시 spdk_sock_request_put(complete).
 *      - 부분 송신 req: offset 누적 후 -EAGAIN (다음 사이클에서 재시도).
 *   8) queued_reqs가 비면 0, 남아있으면 -EAGAIN.
 *
 * 이 함수가 sock 송신 경로의 핵심 — writev_async가 enqueue한 모든 req가 결국 여기서 sendmsg된다.
 */
static int
_sock_flush(struct spdk_sock *sock)
{
	struct spdk_posix_sock *psock = __posix_sock(sock);  /* [한국어] flush할 sock(구체 타입). */
	int flags;                                           /* [한국어] sendmsg 플래그(MSG_NOSIGNAL/ZEROCOPY). */
	struct iovec iovs[IOV_BATCH_SIZE];                   /* [한국어] 한 번에 모을 iovec 배열(최대 IOV_BATCH_SIZE). */
	int iovcnt;                                          /* [한국어] 실제 모인 iovec 개수. */
	int retval;                                          /* [한국어] request_put 결과(sock 종료 감지). */
	struct spdk_sock_request *req;                       /* [한국어] queued_reqs 순회 커서. */
	int i;                                               /* [한국어] req 내부 iovec 인덱스. */
	ssize_t rc;                                          /* [한국어] poller 결과 / 송신된 총 바이트. */
	unsigned int offset;                                 /* [한국어] 현재 req의 이미 송신된 offset. */
	size_t len;                                          /* [한국어] 현재 iovec 요소의 잔여 길이. */
	bool is_zcopy = false;                               /* [한국어] 이번 sendmsg가 zerocopy로 나갔는지. */

	rc = posix_connect_poller(psock);                    /* [한국어] 진행 중 connect/handshake 완료 확인. */
	if (rc < 0) {                                        /* [한국어] 미완료(EAGAIN) 또는 실패. */
		return rc;
	}

	/* Can't flush from within a callback or we end up with recursive calls */
	if (sock->cb_cnt > 0) {                              /* [한국어] 완료 콜백 실행 중이면 재귀 flush 금지. */
		return -EAGAIN;
	}

#ifdef SPDK_ZEROCOPY
	if (psock->zcopy) {                                  /* [한국어] zcopy 활성 sock이면. */
		flags = MSG_ZEROCOPY | MSG_NOSIGNAL;         /* [한국어] 페이지 직접 노출 + SIGPIPE 회피. */
	} else
#endif
	{
		flags = MSG_NOSIGNAL;                        /* [한국어] 일반 경로는 SIGPIPE만 회피. */
	}

	iovcnt = spdk_sock_prep_reqs(sock, iovs, 0, NULL, &flags); /* [한국어] queued_reqs의 iovec들을 batch로 수집(zcopy threshold 미달 시 flags에서 ZEROCOPY 제거). */
	if (iovcnt == 0) {                                   /* [한국어] 보낼 데이터 없음. */
		return 0;
	}

#ifdef SPDK_ZEROCOPY
	is_zcopy = flags & MSG_ZEROCOPY;                     /* [한국어] prep_reqs가 최종 결정한 zcopy 여부 확인. */
#endif

	rc = posix_writev(psock, iovs, iovcnt, flags);       /* [한국어] 한 번의 sendmsg로 batch 송신. */
	if (rc < 0) {                                        /* [한국어] EAGAIN 등 — 다음 사이클 재시도. */
		return rc;
	}

	if (is_zcopy) {                                      /* [한국어] zcopy면 이번 sendmsg에 새 sequence number 부여. */
		psock->sendmsg_idx++;                        /* [한국어] errqueue 완료 통지 매칭용 monotonic 증가. */
	}

	/* Consume the requests that were actually written */
	req = TAILQ_FIRST(&sock->queued_reqs);               /* [한국어] 큐 head부터 송신량만큼 소비. */
	while (req) {
		offset = req->internal.offset;               /* [한국어] 이 req에서 이미 보낸 바이트 위치. */

		if (is_zcopy) {
			/* Cache sendmsg_idx because full request might not be handled and next
			 * chunk may be sent without zero copy. */
			req->internal.pending_zcopy = true;  /* [한국어] zcopy 완료(errqueue) 전까지 put 보류 표시. */
			req->internal.zcopy_idx = psock->sendmsg_idx; /* [한국어] 이 req가 속한 sendmsg의 sequence number 기록. */
		}

		for (i = 0; i < req->iovcnt; i++) {          /* [한국어] req의 각 iovec을 offset 기준으로 진행. */
			/* Advance by the offset first */
			if (offset >= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len) { /* [한국어] 이 요소는 이미 전부 송신됨. */
				offset -= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len;
				continue;
			}

			/* Calculate the remaining length of this element */
			len = SPDK_SOCK_REQUEST_IOV(req, i)->iov_len - offset; /* [한국어] 이 요소의 미송신 잔여 길이. */

			if (len > (size_t)rc) {              /* [한국어] 이번에 보낸 양으로 이 요소를 다 못 채움 = 부분 송신. */
				/* This element was partially sent. */
				req->internal.offset += rc;  /* [한국어] 보낸 만큼만 offset 전진. */
				/* Caller in interrupt mode should retry for partial flush */
				return -EAGAIN;              /* [한국어] 나머지는 다음 사이클에. */
			}

			offset = 0;                          /* [한국어] 이 요소는 완전 송신 — 다음 요소는 offset 0. */
			req->internal.offset += len;         /* [한국어] req offset 전진. */
			rc -= len;                           /* [한국어] 남은 송신량 차감. */
		}

		/* Handled a full request. */
		spdk_sock_request_pend(sock, req);           /* [한국어] queued_reqs → pending_reqs로 이동(완료 대기). */

		/* We can't put the req if zero-copy is not completed or it is not first
		 * in the line. */
		if (!req->internal.pending_zcopy && req == TAILQ_FIRST(&sock->pending_reqs)) { /* [한국어] zcopy 보류 아니고 pending head면. */
			/* The sendmsg syscall above isn't currently asynchronous,
			* so it's already done. */
			retval = spdk_sock_request_put(sock, req, 0); /* [한국어] 즉시 완료(cb_fn 호출). */
			if (retval) {                        /* [한국어] cb_fn 내부에서 사용자가 sock close. */
				/* The user closed the socket. */
				return 0;
			}
		}

		req = TAILQ_FIRST(&sock->queued_reqs);       /* [한국어] 다음 큐 req. */
		if (rc == 0) {                               /* [한국어] 보낸 양을 다 소진하면 종료. */
			break;
		}
	}

	if (!TAILQ_EMPTY(&sock->queued_reqs)) {              /* [한국어] 아직 못 보낸 req가 남음. */
		return -EAGAIN;
	}

	return 0;                                            /* [한국어] 큐 비움 — 전부 송신. */
}

/*
 * [한국어]
 * posix_sock_flush - vtable flush — _sock_flush + zcopy MSG_ERRQUEUE 수확.
 *
 * caller가 sock에 데이터를 모두 queue한 후 즉시 송신 + zcopy 완료 통지까지 확인하고 싶을 때 호출.
 */
static int
posix_sock_flush(struct spdk_sock *sock)
{
#ifdef SPDK_ZEROCOPY
	struct spdk_posix_sock *psock = __posix_sock(sock);
	int rc;

	rc = _sock_flush(sock);                              /* [한국어] queued_reqs sendmsg. */

	if (psock->zcopy && !TAILQ_EMPTY(&sock->pending_reqs)) {
		_sock_check_zcopy(sock);                         /* [한국어] zcopy 완료 통지 수확. */
	}

	return rc;
#else
	return _sock_flush(sock);
#endif
}

/*
 * [한국어]
 * posix_sock_recv_from_pipe - recv_pipe에서 사용자 iov로 데이터 복사 (drain).
 *
 * @diov/diovcnt: 사용자 destination iovec.
 * @return: 복사된 byte 수, -EAGAIN(pipe empty), -EINVAL(diov 0 길이).
 *
 * 동작: pipe_reader_get_buffer로 pipe의 read-side iovec(siov, 2개) 추출 → spdk_iovcpy로 diov에
 * 복사 → pipe_reader_advance로 read pointer 진행. pipe가 비면 socks_with_data에서 제거.
 */
static ssize_t
posix_sock_recv_from_pipe(struct spdk_posix_sock *sock, struct iovec *diov, int diovcnt)
{
	struct iovec siov[2];                                /* [한국어] pipe read-side iovec(ring wrap로 최대 2조각). */
	int sbytes;                                          /* [한국어] pipe에서 읽을 수 있는 바이트(조각 합). */
	ssize_t bytes;                                       /* [한국어] 실제 복사된 바이트. */
	struct spdk_posix_sock_group_impl *group;            /* [한국어] socks_with_data 리스트 관리용 group. */

	sbytes = spdk_pipe_reader_get_buffer(sock->recv_pipe, sock->recv_buf_sz, siov); /* [한국어] pipe에 쌓인 데이터의 read iovec 획득. */
	if (sbytes < 0) {                                    /* [한국어] 내부 오류. */
		return -EINVAL;
	} else if (sbytes == 0) {                            /* [한국어] pipe 비었음. */
		return -EAGAIN;
	}

	bytes = spdk_iovcpy(siov, 2, diov, diovcnt);         /* [한국어] pipe(siov) → 사용자 버퍼(diov)로 복사. */
	if (bytes == 0) {                                    /* [한국어] diov가 0 길이인 경우만 발생. */
		/* The only way this happens is if diov is 0 length */
		return -EINVAL;
	}

	spdk_pipe_reader_advance(sock->recv_pipe, bytes);    /* [한국어] 복사한 만큼 pipe read pointer 전진. */

	/* If we drained the pipe, mark it appropriately */
	if (spdk_pipe_reader_bytes_available(sock->recv_pipe) == 0) { /* [한국어] pipe를 전부 비웠으면. */
		assert(sock->pipe_has_data == true);

		group = __posix_group_impl(sock->base.group_impl);
		if (group && !sock->socket_has_data) {       /* [한국어] 커널 버퍼에도 데이터 없으면 has_data 리스트에서 제거. */
			TAILQ_REMOVE(&group->socks_with_data, sock, link);
		}

		sock->pipe_has_data = false;                 /* [한국어] pipe 비움 표시. */
	}

	return bytes;                                        /* [한국어] 사용자에게 전달한 바이트. */
}

/*
 * [한국어]
 * posix_sock_read - 커널 소켓 버퍼를 readv로 한 번에 크게 읽어 recv_pipe로 흡수.
 *
 * @sock: 대상 sock(recv_pipe 보유).
 * @return: pipe로 흡수한 바이트(>0), 0/음수(EOF/오류).
 *
 * 동기: 작은 응답 다수에 대해 매번 readv 시스템 호출하면 비싸므로, 한 번의 readv로 큰 chunk를
 * 사용자 공간 pipe에 채워두고 이후 readv는 pipe→user 복사만 수행(syscall 분할 상환).
 *
 * 동작: pipe writer iovec 확보 → posix_readv로 채움 → writer pointer 전진 → pipe_has_data set.
 * 읽은 양이 pipe 가용량보다 적으면 커널 버퍼를 모두 비운 것으로 보고 socket_has_data=false.
 *
 * 호출 체인: posix_sock_readv → [이 함수] → posix_readv(readv/SSL_read).
 */
static inline ssize_t
posix_sock_read(struct spdk_posix_sock *sock)
{
	struct iovec iov[2];                                 /* [한국어] pipe write-side iovec(wrap로 최대 2조각). */
	int bytes_avail, bytes_recvd;                        /* [한국어] pipe 가용 공간 / 실제 수신 바이트. */
	struct spdk_posix_sock_group_impl *group;            /* [한국어] has_data 리스트 관리용 group. */
	int rc;                                              /* [한국어] 중간 결과. */

	rc = spdk_pipe_writer_get_buffer(sock->recv_pipe, sock->recv_buf_sz, iov); /* [한국어] pipe의 빈 공간을 write iovec으로 획득. */
	if (rc <= 0) {                                       /* [한국어] pipe 가득 참 등 — 더 흡수 불가. */
		return rc;
	}

	bytes_avail = rc;                                    /* [한국어] pipe에 넣을 수 있는 최대 바이트. */

	rc = posix_readv(sock, iov, 2);                      /* [한국어] 커널 소켓 → pipe로 readv. */
	assert(sock->pipe_has_data == false);                /* [한국어] 읽기 직전 pipe는 비어 있어야 함(이 함수는 빈 pipe에서만 호출). */
	if (rc <= 0) {                                       /* [한국어] EOF/EAGAIN/오류. */
		/* Errors count as draining the socket data */
		if (sock->base.group_impl && sock->socket_has_data) { /* [한국어] 오류도 소켓 데이터 소진으로 간주 → 리스트 제거. */
			group = __posix_group_impl(sock->base.group_impl);
			TAILQ_REMOVE(&group->socks_with_data, sock, link);
		}

		sock->socket_has_data = false;               /* [한국어] 커널 버퍼 비움 표시. */
		return rc;
	}

	bytes_recvd = rc;                                    /* [한국어] 실제 수신 바이트. */
	spdk_pipe_writer_advance(sock->recv_pipe, bytes_recvd); /* [한국어] pipe write pointer 전진(데이터 확정). */

#if DEBUG
	if (sock->base.group_impl) {
		assert(sock->socket_has_data == true);       /* [한국어] group이면 epoll이 EPOLLIN을 줬어야 함. */
	}
#endif

	sock->pipe_has_data = true;                          /* [한국어] pipe에 읽을 데이터 생김. */
	if (bytes_recvd < bytes_avail) {                     /* [한국어] 가용 공간보다 적게 읽음 = 커널 버퍼 전부 소진. */
		/* We drained the kernel socket entirely. */
		sock->socket_has_data = false;
	}

	return bytes_recvd;                                  /* [한국어] pipe로 흡수한 바이트. */
}

/*
 * [한국어]
 * posix_sock_readv - vtable readv. recv_pipe 유무·크기에 따라 직접 readv 또는 pipe 경유.
 *
 * @iov/iovcnt: 사용자 destination iovec.
 * @return: 읽은 바이트(>0), 0(EOF), 음수 errno.
 *
 * 분기:
 *   1) connect 진행 중이면 poller로 완료 확인(미완료면 EAGAIN).
 *   2) recv_pipe 없으면 곧장 posix_readv.
 *   3) pipe 있고 데이터 흡수 필요 시: 요청량이 MIN_SOCK_PIPE_SIZE 이상이면 사용자 버퍼로 직접
 *      readv(pipe 우회), 작으면 posix_sock_read로 pipe에 흡수 후 pipe에서 복사.
 *   4) 최종적으로 posix_sock_recv_from_pipe로 pipe→user 복사.
 *
 * 호출 체인: spdk_sock_readv → vtable readv → [이 함수].
 */
static ssize_t
posix_sock_readv(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);  /* [한국어] 구체 sock. */
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(sock->base.group_impl); /* [한국어] 속한 group(없으면 NULL). */
	int rc, i;                                           /* [한국어] 결과 / iov 인덱스. */
	size_t len;                                          /* [한국어] 사용자 요청 총 길이. */

	rc = posix_connect_poller(sock);                     /* [한국어] async connect 완료 여부 확인. */
	if (rc < 0) {                                        /* [한국어] 미완료/실패. */
		return rc;
	}

	if (sock->recv_pipe == NULL) {                       /* [한국어] pipe 미사용 sock — 직접 readv. */
		assert(sock->pipe_has_data == false);
		if (group && sock->socket_has_data) {        /* [한국어] 읽기 전 has_data 리스트에서 제거(소진 가정). */
			sock->socket_has_data = false;
			TAILQ_REMOVE(&group->socks_with_data, sock, link);
		}

		return posix_readv(sock, iov, iovcnt);       /* [한국어] 커널 → 사용자 버퍼 직접. */
	}

	/* If the socket is not in a group, we must assume it always has
	 * data waiting for us because it is not epolled */
	if (!sock->pipe_has_data && (group == NULL || sock->socket_has_data)) { /* [한국어] pipe 비었고 커널엔 데이터 있을 때(또는 group 밖). */
		/* If the user is receiving a sufficiently large amount of data,
		 * receive directly to their buffers. */
		len = 0;
		for (i = 0; i < iovcnt; i++) {               /* [한국어] 사용자 요청 총 길이 합산. */
			len += iov[i].iov_len;
		}

		if (len >= MIN_SOCK_PIPE_SIZE) {             /* [한국어] 충분히 크면 pipe 우회(복사 1회 절약). */
			/* TODO: Should this detect if kernel socket is drained? */
			return posix_readv(sock, iov, iovcnt);
		}

		/* Otherwise, do a big read into our pipe */
		rc = posix_sock_read(sock);                  /* [한국어] 작은 요청 — pipe에 크게 흡수. */
		if (rc <= 0) {                               /* [한국어] EOF/오류면 그대로 반환. */
			return rc;
		}
	}

	return posix_sock_recv_from_pipe(sock, iov, iovcnt); /* [한국어] pipe에서 사용자 iov로 복사. */
}

/*
 * [한국어]
 * posix_sock_recv - vtable recv. 단일 버퍼 recv를 iovec 1개로 감싸 readv에 위임.
 *
 * @buf/len: 수신 버퍼·길이.
 * @return: 읽은 바이트 / 0 / 음수 errno.
 */
static ssize_t
posix_sock_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	struct iovec iov[1];                                 /* [한국어] 단일 버퍼를 iovec로 래핑. */

	iov[0].iov_base = buf;                               /* [한국어] 버퍼 시작. */
	iov[0].iov_len = len;                                /* [한국어] 버퍼 길이. */

	return posix_sock_readv(sock, iov, 1);               /* [한국어] readv 경로 재사용. */
}

/*
 * [한국어]
 * posix_sock_writev - vtable writev(동기). 미완료 async 송신을 먼저 flush 후 직접 송신.
 *
 * @iov/iovcnt: 송신 데이터.
 * @return: 송신 바이트, -EAGAIN(이전 async req 미완료 시), 음수 errno.
 *
 * 순서 보존을 위해 queued_reqs가 남아 있으면 새 데이터를 보내지 않고 EAGAIN — 이전 요청이
 * 먼저 나가야 스트림 순서가 유지됨.
 */
static ssize_t
posix_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);  /* [한국어] 구체 sock. */
	int rc;                                              /* [한국어] flush 결과. */

	/* In order to process a writev, we need to flush any asynchronous writes
	 * first. */
	rc = _sock_flush(_sock);                             /* [한국어] 큐에 쌓인 async req 먼저 송신. */
	if (rc < 0) {                                        /* [한국어] flush 실패. */
		return rc;
	}

	if (!TAILQ_EMPTY(&_sock->queued_reqs)) {             /* [한국어] 다 못 비웠으면 순서 보존 위해 보류. */
		/* We weren't able to flush all requests */
		return -EAGAIN;
	}

	return posix_writev(sock, iov, iovcnt, 0);           /* [한국어] 큐 비었으니 이 데이터 직접 송신(flags=0). */
}

/*
 * [한국어]
 * posix_sock_recv_next - vtable recv_next. group이 제공한 버퍼로 1회 수신(zero-copy recv 모델).
 *
 * @buf: 출력 — 수신 데이터가 담긴 group 버퍼.
 * @ctx: in/out — group 버퍼 식별 컨텍스트.
 * @return: 읽은 바이트, -ENOTSUP(recv_pipe 사용 sock), -ENOBUFS(group 버퍼 없음).
 *
 * recv_pipe와 양립 불가(pipe가 데이터를 선점하므로 ENOTSUP). 실패 시 빌린 버퍼를 group에 반납.
 */
static int
posix_sock_recv_next(struct spdk_sock *_sock, void **buf, void **ctx)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);  /* [한국어] 구체 sock. */
	struct iovec iov;                                    /* [한국어] group 버퍼를 가리킬 iovec. */
	ssize_t rc;                                          /* [한국어] 수신 결과. */

	if (sock->recv_pipe != NULL) {                       /* [한국어] pipe 사용 시 이 경로 미지원. */
		return -ENOTSUP;
	}

	iov.iov_len = spdk_sock_group_get_buf(_sock->group_impl->group, &iov.iov_base, ctx); /* [한국어] group buffer pool에서 버퍼 1개 대여. */
	if (iov.iov_len == 0) {                              /* [한국어] 가용 버퍼 없음. */
		return -ENOBUFS;
	}

	rc = posix_sock_readv(_sock, &iov, 1);               /* [한국어] 대여 버퍼로 수신. */
	if (rc <= 0) {                                       /* [한국어] EOF/EAGAIN/오류 시 버퍼 반납. */
		spdk_sock_group_provide_buf(_sock->group_impl->group, iov.iov_base, iov.iov_len, *ctx);
		return rc;
	}

	*buf = iov.iov_base;                                 /* [한국어] 수신된 데이터 버퍼를 caller에 전달. */
	return rc;                                           /* [한국어] 읽은 바이트. */
}

/*
 * [한국어]
 * posix_sock_writev_async - vtable writev_async. req를 큐에 넣고 충분히 쌓이면 즉시 flush.
 *
 * @req: 비동기 송신 요청(완료 시 req->cb_fn 호출).
 *
 * 동기: 작은 송신을 모아 한 번의 sendmsg로 batch 송신(syscall 절감). 큐된 iovec이
 * IOV_BATCH_SIZE 이상이면 지연 없이 flush. flush 중 치명 오류면 모든 req abort.
 *
 * 호출 체인: spdk_sock_writev_async → vtable writev_async → [이 함수] → _sock_flush.
 */
static void
posix_sock_writev_async(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	int rc;                                              /* [한국어] flush 결과. */

	spdk_sock_request_queue(sock, req);                  /* [한국어] req를 queued_reqs에 추가. */

	/* If there are a sufficient number queued, just flush them out immediately. */
	if (sock->queued_iovcnt >= IOV_BATCH_SIZE) {         /* [한국어] batch 임계 도달 — 즉시 송신. */
		rc = _sock_flush(sock);
		if (rc < 0 && rc != -EAGAIN) {               /* [한국어] EAGAIN은 정상(부분 송신), 그 외는 치명. */
			spdk_sock_abort_requests(sock);      /* [한국어] 모든 req를 오류로 완료 처리. */
		}
	}
}

/*
 * [한국어]
 * posix_sock_set_recvlowat - SO_RCVLOWAT 설정 (vtable). recv가 깨어나는 최소 바이트 임계.
 *
 * @nbytes: 최소 수신 임계.
 * @return: 0(성공), -ENOTCONN(연결 실패), 음수 errno.
 *
 * connect 진행 중이면 connect_ctx에 예약 후 완료 시 적용. SO_RCVLOWAT은 커널이 이 바이트만큼
 * 모일 때까지 readable 통지를 미뤄 작은 wake-up을 줄인다.
 */
static int
posix_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);  /* [한국어] 구체 sock. */
	int rc, val;                                         /* [한국어] setsockopt 결과 / 설정 값. */

	assert(sock != NULL);

	if (!sock->ready) {                                  /* [한국어] 아직 connect 미완료. */
		if (sock->connect_ctx) {                     /* [한국어] 진행 중이면 완료 후 적용 예약. */
			sock->connect_ctx->set_recvlowat = nbytes;
			return 0;
		}

		SPDK_ERRLOG("Connection failed.\n");         /* [한국어] connect 실패 상태. */
		return -ENOTCONN;
	}

	val = nbytes;                                        /* [한국어] setsockopt 인자. */
	rc = setsockopt(sock->fd, SOL_SOCKET, SO_RCVLOWAT, &val, sizeof val); /* [한국어] 커널에 수신 low watermark 설정. */
	return rc < 0 ? -errno : rc;                         /* [한국어] 오류는 음수 errno로. */
}

/*
 * [한국어]
 * posix_sock_is_ipv6 - sock의 local 주소 family가 IPv6인지 (vtable).
 *
 * @return: true=IPv6, false(아니거나 미연결 — errno에 EAGAIN/ENOTCONN 설정).
 * getsockname으로 local sockaddr family 확인.
 */
static bool
posix_sock_is_ipv6(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);  /* [한국어] 구체 sock. */
	struct sockaddr_storage sa;                          /* [한국어] local 주소 버퍼. */
	socklen_t salen;                                     /* [한국어] sa 길이 in/out. */
	int rc;                                              /* [한국어] getsockname 결과. */

	assert(sock != NULL);

	if (!sock->ready) {                                  /* [한국어] 미연결이면 family 미확정. */
		SPDK_ERRLOG("Connection %s.\n", sock->connect_ctx ? "in progress" : "failed");
		errno = sock->connect_ctx ? EAGAIN : ENOTCONN; /* [한국어] 진행 중 vs 실패 구분. */
		return false;
	}

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname(sock->fd, (struct sockaddr *) &sa, &salen); /* [한국어] local 주소 추출. */
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET6);                   /* [한국어] family가 IPv6인지. */
}

/*
 * [한국어]
 * posix_sock_is_ipv4 - sock의 local 주소 family가 IPv4인지 (vtable). is_ipv6와 대칭.
 */
static bool
posix_sock_is_ipv4(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);  /* [한국어] 구체 sock. */
	struct sockaddr_storage sa;                          /* [한국어] local 주소 버퍼. */
	socklen_t salen;                                     /* [한국어] sa 길이. */
	int rc;                                              /* [한국어] getsockname 결과. */

	assert(sock != NULL);

	if (!sock->ready) {                                  /* [한국어] 미연결 처리(is_ipv6와 동일). */
		SPDK_ERRLOG("Connection %s.\n", sock->connect_ctx ? "in progress" : "failed");
		errno = sock->connect_ctx ? EAGAIN : ENOTCONN;
		return false;
	}

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname(sock->fd, (struct sockaddr *) &sa, &salen); /* [한국어] local 주소 추출. */
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET);                    /* [한국어] family가 IPv4인지. */
}

/*
 * [한국어]
 * posix_sock_is_connected - sock이 여전히 연결되어 있는지 (vtable).
 *
 * @return: true=연결됨, false=종료/오류.
 *
 * MSG_PEEK로 1바이트 엿보기 — 0 반환은 peer가 닫음(EOF), EAGAIN/EWOULDBLOCK은 "데이터 없지만
 * 연결 유지". 그 외 오류는 끊김. 먼저 connect_poller로 진행 중 connect 완료 여부도 확인.
 */
static bool
posix_sock_is_connected(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);  /* [한국어] 구체 sock. */
	uint8_t byte;                                        /* [한국어] MSG_PEEK 대상 1바이트(소비 안 함). */
	int rc;                                              /* [한국어] poller/recv 결과. */

	rc = posix_connect_poller(sock);                     /* [한국어] async connect 완료 확인. */
	if (rc < 0) {                                        /* [한국어] 미완료/실패 → 미연결. */
		errno = -rc;
		return false;
	}

	rc = recv(sock->fd, &byte, 1, MSG_PEEK);             /* [한국어] 버퍼를 소비하지 않고 상태만 확인. */
	if (rc == 0) {                                       /* [한국어] EOF — peer가 연결 종료. */
		return false;
	}

	if (rc < 0) {                                        /* [한국어] 오류 분기. */
		if (errno == EAGAIN || errno == EWOULDBLOCK) { /* [한국어] 데이터 없을 뿐 연결은 유지. */
			return true;
		}

		return false;                                /* [한국어] 그 외 오류 = 끊김. */
	}

	return true;                                         /* [한국어] 1바이트 엿봄 = 데이터 있고 연결됨. */
}

/*
 * [한국어]
 * posix_sock_group_impl_get_optimal - placement_id 기반으로 sock에 최적인 group_impl 추천 (vtable).
 *
 * @hint: 상위가 제안한 group(없으면 NULL).
 * @return: 최적 group_impl 또는 NULL(임의 배치 허용).
 *
 * 같은 placement_id(=같은 NIC RSS 큐)의 sock들을 동일 group(=reactor)에 묶어 cache locality와
 * lockless 처리를 극대화. g_map에서 placement_id로 group을 lookup.
 */
static struct spdk_sock_group_impl *
posix_sock_group_impl_get_optimal(struct spdk_sock *_sock, struct spdk_sock_group_impl *hint)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);  /* [한국어] 구체 sock. */
	struct spdk_sock_group_impl *group_impl;             /* [한국어] lookup 결과 group. */

	if (!sock->ready) {                                  /* [한국어] 미연결이면 추천 불가. */
		SPDK_ERRLOG("Connection %s.\n", sock->connect_ctx ? "in progress" : "failed");
		errno = sock->connect_ctx ? EAGAIN : ENOTCONN;
		return NULL;
	}

	if (sock->placement_id != -1) {                      /* [한국어] placement hint가 있으면. */
		spdk_sock_map_lookup(&g_map, sock->placement_id, &group_impl, hint); /* [한국어] g_map에서 placement_id→group 매핑 조회(mtx 보호). */
		return group_impl;
	}

	return NULL;                                         /* [한국어] hint 없음 — 상위가 자유 배치. */
}

/*
 * [한국어]
 * _sock_group_impl_create - epoll/kqueue fd + pipe_group을 가진 group_impl 생성(공통 구현).
 *
 * @enable_placement_id: PLACEMENT_CPU면 현재 코어를 placement_id로 g_map에 등록.
 * @return: 생성된 group_impl base 또는 NULL.
 *
 * 동기: 한 reactor마다 group_impl 하나를 만들어 그 코어에 묶인 sock들의 epoll polling을 책임.
 * PLACEMENT_CPU 모드면 코어 id를 placement_id로 써서 같은 코어의 sock이 이 group으로 모이게 함.
 *
 * 호출 체인: posix/ssl_sock_group_impl_create → [이 함수] → epoll_create1/kqueue + spdk_pipe_group_create.
 */
static struct spdk_sock_group_impl *
_sock_group_impl_create(uint32_t enable_placement_id)
{
	struct spdk_posix_sock_group_impl *group_impl;       /* [한국어] 생성할 group. */
	int fd;                                              /* [한국어] epoll/kqueue fd. */

#if defined(SPDK_EPOLL)
	fd = epoll_create1(0);                               /* [한국어] Linux: epoll 인스턴스 생성. */
#elif defined(SPDK_KEVENT)
	fd = kqueue();                                       /* [한국어] FreeBSD: kqueue 생성. */
#endif
	if (fd == -1) {                                      /* [한국어] 생성 실패. */
		return NULL;
	}

	group_impl = calloc(1, sizeof(*group_impl));         /* [한국어] group 객체 0-할당. */
	if (group_impl == NULL) {
		SPDK_ERRLOG("group_impl allocation failed\n");
		close(fd);                                   /* [한국어] 할당 실패 시 fd 회수. */
		return NULL;
	}

	group_impl->pipe_group = spdk_pipe_group_create();   /* [한국어] 이 group sock들의 recv_pipe를 묶을 pipe_group 생성. */
	if (group_impl->pipe_group == NULL) {
		SPDK_ERRLOG("pipe_group allocation failed\n");
		free(group_impl);
		close(fd);
		return NULL;
	}

	group_impl->fd = fd;                                 /* [한국어] epoll/kqueue fd 저장. */
	TAILQ_INIT(&group_impl->socks_with_data);            /* [한국어] has_data 리스트 초기화. */
	group_impl->placement_id = -1;                       /* [한국어] 기본 미할당(-1). */

	if (enable_placement_id == PLACEMENT_CPU) {          /* [한국어] CPU 모드면 현재 코어를 placement_id로. */
		spdk_sock_map_insert(&g_map, spdk_env_get_current_core(), &group_impl->base); /* [한국어] 코어→group 매핑 등록. */
		group_impl->placement_id = spdk_env_get_current_core();
	}

	return &group_impl->base;                            /* [한국어] base 포인터 반환. */
}

/*
 * [한국어]
 * posix_sock_group_impl_create - posix 백엔드 group_impl 생성 (vtable). 옵션의 placement 모드 전달.
 */
static struct spdk_sock_group_impl *
posix_sock_group_impl_create(void)
{
	return _sock_group_impl_create(g_posix_impl_opts.enable_placement_id); /* [한국어] posix 옵션의 placement 모드로 생성. */
}

/*
 * [한국어]
 * ssl_sock_group_impl_create - ssl 백엔드 group_impl 생성 (vtable). ssl 옵션의 placement 모드 사용.
 */
static struct spdk_sock_group_impl *
ssl_sock_group_impl_create(void)
{
	return _sock_group_impl_create(g_ssl_impl_opts.enable_placement_id); /* [한국어] ssl 옵션의 placement 모드로 생성. */
}

/*
 * [한국어]
 * posix_sock_mark - SO_MARK로 sock에 placement_id를 각인하고 g_map에 group 매핑 등록.
 *
 * @group: 이 sock이 속할 group.
 * @sock: 마킹할 sock.
 * @placement_id: cgroup/네트워크 mark 값(=group 식별자).
 *
 * 동기: PLACEMENT_MARK 모드에서 같은 mark의 트래픽이 같은 RSS 큐로 향하도록 커널에 SO_MARK를
 * 설정하고, 이후 lookup이 같은 group을 반환하도록 g_map에 등록. setsockopt/map 실패는 non-fatal
 * (성능 최적화 실패일 뿐 기능엔 영향 없음).
 *
 * 호출 체인: posix_sock_update_mark → [이 함수] → setsockopt(SO_MARK) + spdk_sock_map_insert.
 */
static void
posix_sock_mark(struct spdk_posix_sock_group_impl *group, struct spdk_posix_sock *sock,
		int placement_id)
{
#if defined(SO_MARK)
	int rc;                                              /* [한국어] setsockopt/map_insert 결과. */

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_MARK,
			&placement_id, sizeof(placement_id)); /* [한국어] 커널 packet mark 설정 — RSS/큐 정렬용. */
	if (rc != 0) {                                       /* [한국어] 실패해도 치명적이지 않음. */
		/* Not fatal */
		SPDK_ERRLOG("Error setting SO_MARK\n");
		return;
	}

	rc = spdk_sock_map_insert(&g_map, placement_id, &group->base); /* [한국어] placement_id→group 매핑 등록(mtx). */
	if (rc != 0) {
		/* Not fatal */
		SPDK_ERRLOG("Failed to insert sock group into map: %d\n", rc);
		return;
	}

	sock->placement_id = placement_id;                   /* [한국어] sock에 확정된 placement_id 기록. */
#endif
}

/*
 * [한국어]
 * posix_sock_update_mark - group에 아직 placement_id가 없으면 free id를 할당하고 멤버 sock 갱신.
 *
 * @_group/_sock: 대상 group과 새로 추가되는 sock.
 *
 * 동기: PLACEMENT_MARK 모드에서 group이 첫 sock을 받을 때 g_map에서 사용 가능한 free
 * placement_id를 하나 점유하고, 이미 들어있던 sock들과 새 sock 모두에 그 id를 마킹한다.
 *
 * 호출 체인: posix_sock_group_impl_add_sock → [이 함수] → posix_sock_mark.
 */
static void
posix_sock_update_mark(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group); /* [한국어] 구체 group. */

	if (group->placement_id == -1) {                     /* [한국어] group이 아직 id 미보유. */
		group->placement_id = spdk_sock_map_find_free(&g_map); /* [한국어] 사용 가능한 free placement_id 점유. */

		/* If a free placement id is found, update existing sockets in this group */
		if (group->placement_id != -1) {             /* [한국어] free id를 얻었으면 기존 멤버 sock 전부 갱신. */
			struct spdk_sock  *sock, *tmp;

			TAILQ_FOREACH_SAFE(sock, &_group->socks, link, tmp) { /* [한국어] group의 모든 sock 순회. */
				posix_sock_mark(group, __posix_sock(sock), group->placement_id);
			}
		}
	}

	if (group->placement_id != -1) {                     /* [한국어] id가 확정됐으면. */
		/*
		 * group placement id is already determined for this poll group.
		 * Mark socket with group's placement id.
		 */
		posix_sock_mark(group, __posix_sock(_sock), group->placement_id); /* [한국어] 새 sock도 동일 id로 마킹. */
	}
}

/*
 * [한국어]
 * posix_sock_group_impl_add_sock - sock을 group의 epoll/kqueue에 등록 (vtable add_sock).
 *
 * @_group/_sock: 대상 group과 추가할 sock.
 * @return: 0(성공), -ENOTCONN(연결 실패), 음수 errno.
 *
 * 동작:
 *   1) connect 미완료면 등록 보류(connect_poller가 완료 시 다시 add) — ctx 있으면 0 반환.
 *   2) epoll_ctl(ADD)/kevent로 fd를 group의 이벤트 인스턴스에 등록(EPOLLIN|EPOLLERR).
 *   3) 다른 group에서 옮겨온 경우 recv_pipe에 남은 데이터가 있으면 socks_with_data에 즉시 추가.
 *   4) recv_pipe를 group의 pipe_group에 편입(메모리 grant 단위 공유).
 *   5) placement 정책에 따라 SO_MARK 또는 g_map 등록.
 *
 * 실행 컨텍스트: group을 소유한 reactor 스레드. 호출 체인: spdk_sock_group_add_sock → [이 함수].
 */
static int
posix_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group); /* [한국어] 구체 group. */
	struct spdk_posix_sock *sock = __posix_sock(_sock);  /* [한국어] 구체 sock. */
	int rc;                                              /* [한국어] epoll_ctl/map 결과. */

	if (!sock->ready) {                                  /* [한국어] connect 미완료. */
		/* Defer adding the sock to the group;
		 * the group is cached in the base object by the upper layer. */
		if (sock->connect_ctx) {                     /* [한국어] 진행 중이면 보류(poller가 나중에 add). */
			return 0;
		}

		SPDK_ERRLOG("Connection failed.\n");         /* [한국어] connect 실패. */
		return -ENOTCONN;
	}

#if defined(SPDK_EPOLL)
	struct epoll_event event;                            /* [한국어] epoll 등록 이벤트 디스크립션. */

	memset(&event, 0, sizeof(event));
	/* EPOLLERR is always on even if we don't set it, but be explicit for clarity */
	event.events = EPOLLIN | EPOLLERR;                   /* [한국어] readable + 에러(zcopy errqueue 통지 포함) 관심. */
	event.data.ptr = sock;                               /* [한국어] 이벤트 발생 시 sock 포인터로 복원(poll에서 사용). */

	rc = epoll_ctl(group->fd, EPOLL_CTL_ADD, sock->fd, &event); /* [한국어] epoll 인스턴스에 fd 등록. */
#elif defined(SPDK_KEVENT)
	struct kevent event;                                 /* [한국어] kqueue 이벤트. */
	struct timespec ts = {0};                            /* [한국어] non-blocking(즉시 반환) 타임아웃. */

	EV_SET(&event, sock->fd, EVFILT_READ, EV_ADD, 0, 0, sock); /* [한국어] read 필터 등록 + udata=sock. */

	rc = kevent(group->fd, &event, 1, NULL, 0, &ts);     /* [한국어] kqueue에 등록. */
#endif
	if (rc < 0) {                                        /* [한국어] 등록 실패. */
		return -errno;
	}

	/* switched from another polling group due to scheduling */
	if (spdk_unlikely(sock->recv_pipe != NULL  &&
			  (spdk_pipe_reader_bytes_available(sock->recv_pipe) > 0))) { /* [한국어] 다른 group에서 이동했고 pipe에 잔여 데이터. */
		sock->pipe_has_data = true;                  /* [한국어] pipe 데이터 있음 표시. */
		sock->socket_has_data = false;
		TAILQ_INSERT_TAIL(&group->socks_with_data, sock, link); /* [한국어] 즉시 처리되도록 has_data 리스트에 추가. */
	} else if (sock->recv_pipe != NULL) {                /* [한국어] 잔여 없으면 pipe_group에만 편입. */
		rc = spdk_pipe_group_add(group->pipe_group, sock->recv_pipe);
		assert(rc == 0);
	}

	if (_sock->impl_opts.enable_placement_id == PLACEMENT_MARK) { /* [한국어] MARK 모드면 group id 결정/마킹. */
		posix_sock_update_mark(_group, _sock);
	} else if (sock->placement_id != -1) {               /* [한국어] 그 외 모드에서 id가 있으면 g_map 등록. */
		rc = spdk_sock_map_insert(&g_map, sock->placement_id, &group->base);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to insert sock group into map: %d\n", rc);
			/* Do not treat this as an error. The system will continue running. */
		}
	}

	return rc;                                           /* [한국어] 등록 결과(보통 0). */
}

/*
 * [한국어]
 * posix_sock_group_impl_remove_sock - sock을 group의 epoll/kqueue에서 제거 (vtable remove_sock).
 *
 * @return: 0 또는 음수 errno.
 *
 * 동작: has_data 리스트/pipe_group에서 제거 → g_map placement 해제 → epoll_ctl(DEL)/kevent →
 * 진행 중 송신 req abort. connect 진행 중/미완료면 epoll 등록 전이므로 req만 abort 후 종료.
 */
static int
posix_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group); /* [한국어] 구체 group. */
	struct spdk_posix_sock *sock = __posix_sock(_sock);  /* [한국어] 구체 sock. */
	int rc;                                              /* [한국어] epoll_ctl/pipe 결과. */

	if (sock->connect_ctx || !sock->ready) {             /* [한국어] 아직 epoll 미등록(connect 중/실패) — req만 정리. */
		spdk_sock_abort_requests(_sock);
		return 0;
	}

	if (sock->pipe_has_data || sock->socket_has_data) {  /* [한국어] has_data 리스트에 있으면 제거. */
		TAILQ_REMOVE(&group->socks_with_data, sock, link);
		sock->pipe_has_data = false;
		sock->socket_has_data = false;
	} else if (sock->recv_pipe != NULL) {                /* [한국어] 리스트엔 없지만 pipe_group엔 있으면 분리. */
		rc = spdk_pipe_group_remove(group->pipe_group, sock->recv_pipe);
		assert(rc == 0);
	}

	if (sock->placement_id != -1) {                      /* [한국어] placement 매핑 참조 해제. */
		spdk_sock_map_release(&g_map, sock->placement_id);
	}

#if defined(SPDK_EPOLL)
	struct epoll_event event;                            /* [한국어] 일부 구버전 커널은 인자 요구(값은 무시). */

	/* Event parameter is ignored but some old kernel version still require it. */
	rc = epoll_ctl(group->fd, EPOLL_CTL_DEL, sock->fd, &event); /* [한국어] epoll에서 fd 제거. */
#elif defined(SPDK_KEVENT)
	struct kevent event;
	struct timespec ts = {0};

	EV_SET(&event, sock->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL); /* [한국어] read 필터 제거. */

	rc = kevent(group->fd, &event, 1, NULL, 0, &ts);
	if (rc == 0 && event.flags & EV_ERROR) {             /* [한국어] kevent는 에러를 이벤트 플래그로 보고. */
		rc = -1;
		errno = event.data;
	}
#endif

	spdk_sock_abort_requests(_sock);                     /* [한국어] 남은 송신 req 모두 오류 완료. */
	return rc < 0 ? -errno : rc;
}

/*
 * [한국어]
 * posix_sock_group_impl_poll - group의 모든 sock을 한 사이클 polling (vtable poll) — 송신 flush +
 * epoll_wait 수집 + socks_with_data 우선 처리 + zcopy errqueue 수확.
 *
 * @max_events: 이번에 socks[]로 반환할 최대 ready sock 수.
 * @socks: 출력 — 처리할 ready sock 포인터 배열(상위가 cb_fn 호출).
 * @return: socks[]에 채운 sock 수, 음수 errno.
 *
 * 동작 단계:
 *   1) (zcopy) busy-poll: socks_with_data의 zcopy sock 중 placement_id가 다른 것만 poll()로
 *      커널 tx 큐를 깨워 인터럽트 억제 상황에서 송신이 멈추는 것 방지.
 *   2) 모든 sock의 queued_reqs를 _sock_flush로 송신(콜백이 sock 제거할 수 있어 SAFE 순회).
 *   3) epoll_wait/kevent로 ready 이벤트 수집(timeout 0 = non-blocking).
 *   4) EPOLLERR이면 _sock_check_zcopy로 errqueue 수확, EPOLLIN인 sock을 socks_with_data에 추가.
 *   5) socks_with_data를 순회하며 cb_fn 있는 sock을 socks[]에 채움(max_events까지).
 *   6) 리스트를 회전시켜 매 polling마다 처리 순서를 바꿔 starvation 방지.
 *
 * 실행 컨텍스트: group 소유 reactor 스레드. lockless(같은 스레드 단독 접근). 호출: spdk_sock_group_poll.
 */
static int
posix_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
			   struct spdk_sock **socks)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group); /* [한국어] 구체 group. */
	struct spdk_sock *sock, *tmp;                        /* [한국어] flush 순회 커서(SAFE). */
	int num_events, i, rc;                               /* [한국어] 수집 이벤트 수 / 인덱스 / 결과. */
	struct spdk_posix_sock *psock, *ptmp;                /* [한국어] has_data 리스트 순회 커서. */
#if defined(SPDK_EPOLL)
	struct epoll_event events[MAX_EVENTS_PER_POLL];
#elif defined(SPDK_KEVENT)
	struct kevent events[MAX_EVENTS_PER_POLL];
	struct timespec ts = {0};
#endif

#ifdef SPDK_ZEROCOPY
	/* When all of the following conditions are met
	 * - non-blocking socket
	 * - zero copy is enabled
	 * - interrupts suppressed (i.e. busy polling)
	 * - the NIC tx queue is full at the time sendmsg() is called
	 * - epoll_wait determines there is an EPOLLIN event for the socket
	 * then we can get into a situation where data we've sent is queued
	 * up in the kernel network stack, but interrupts have been suppressed
	 * because other traffic is flowing so the kernel misses the signal
	 * to flush the software tx queue. If there wasn't incoming data
	 * pending on the socket, then epoll_wait would have been sufficient
	 * to kick off the send operation, but since there is a pending event
	 * epoll_wait does not trigger the necessary operation.
	 *
	 * We deal with this by checking for all of the above conditions and
	 * additionally looking for EPOLLIN events that were not consumed from
	 * the last poll loop. We take this to mean that the upper layer is
	 * unable to consume them because it is blocked waiting for resources
	 * to free up, and those resources are most likely freed in response
	 * to a pending asynchronous write completing.
	 *
	 * Additionally, sockets that have the same placement_id actually share
	 * an underlying hardware queue. That means polling one of them is
	 * equivalent to polling all of them. As a quick mechanism to avoid
	 * making extra poll() calls, stash the last placement_id during the loop
	 * and only poll if it's not the same. The overwhelmingly common case
	 * is that all sockets in this list have the same placement_id because
	 * SPDK is intentionally grouping sockets by that value, so even
	 * though this won't stop all extra calls to poll(), it's very fast
	 * and will catch all of them in practice.
	 */
	int last_placement_id = -1;                          /* [한국어] 같은 placement_id(=같은 HW 큐) 중복 poll 방지 캐시. */

	TAILQ_FOREACH(psock, &group->socks_with_data, link) { /* [한국어] 미처리 데이터 보유 zcopy sock 순회. */
		if (psock->zcopy && psock->placement_id >= 0 &&
		    psock->placement_id != last_placement_id) { /* [한국어] 새 HW 큐만 poll(중복 회피). */
			struct pollfd pfd = {psock->fd, POLLIN | POLLERR, 0};

			poll(&pfd, 1, 0);                    /* [한국어] 커널 tx 큐 깨우기 — 억제된 송신 재개 유도. */
			last_placement_id = psock->placement_id;
		}
	}
#endif

	/* This must be a TAILQ_FOREACH_SAFE because while flushing,
	 * a completion callback could remove the sock from the
	 * group. */
	TAILQ_FOREACH_SAFE(sock, &_group->socks, link, tmp) { /* [한국어] 모든 sock의 큐된 송신 flush(콜백이 sock 제거 가능 → SAFE). */
		rc = _sock_flush(sock);
		if (rc < 0 && rc != -EAGAIN) {               /* [한국어] EAGAIN은 정상(부분 송신). 그 외 오류는 req abort. */
			spdk_sock_abort_requests(sock);
		}
	}

	assert(max_events > 0);

#if defined(SPDK_EPOLL)
	rc = epoll_wait(group->fd, events, max_events, 0);   /* [한국어] readable/error fd 수집(timeout 0 = polled-mode). */
#elif defined(SPDK_KEVENT)
	rc = kevent(group->fd, NULL, 0, events, max_events, &ts); /* [한국어] kqueue 이벤트 수집. */
#endif

	if (rc < 0) {                                        /* [한국어] epoll 오류. */
		return -errno;
	}

	num_events = rc;                                     /* [한국어] 이번에 들어온 이벤트 수. */
	if (num_events == 0 && !TAILQ_EMPTY(&_group->socks)) { /* [한국어] 이벤트 0이고 sock이 있으면. */
		sock = TAILQ_FIRST(&_group->socks);
		psock = __posix_sock(sock);
		/* poll() is called here to busy poll the queue associated with
		 * first socket in list and potentially reap incoming data.
		 */
		if (sock->opts.priority) {                   /* [한국어] 우선순위 sock이면 busy-poll로 수신 자극. */
			struct pollfd pfd = {0, 0, 0};

			pfd.fd = psock->fd;
			pfd.events = POLLIN | POLLERR;
			poll(&pfd, 1, 0);                    /* [한국어] 첫 sock의 HW 큐를 깨워 latency 단축. */
		}
	}

	for (i = 0; i < num_events; i++) {                   /* [한국어] 수집된 이벤트 처리. */
#if defined(SPDK_EPOLL)
		sock = events[i].data.ptr;                   /* [한국어] add_sock에서 심은 sock 포인터 복원. */
		psock = __posix_sock(sock);

#ifdef SPDK_ZEROCOPY
		if (events[i].events & EPOLLERR) {           /* [한국어] errqueue 통지(zerocopy 완료) 도착. */
			rc = _sock_check_zcopy(sock);        /* [한국어] 완료 ack 수확 → req complete. */
			/* If the socket was closed or removed from
			 * the group in response to a send ack, don't
			 * add it to the array here. */
			if (rc || sock->cb_fn == NULL) {     /* [한국어] ack 처리 중 sock close/제거 시 추가 금지. */
				continue;
			}
		}
#endif
		if ((events[i].events & EPOLLIN) == 0) {     /* [한국어] readable 아니면 스킵. */
			continue;
		}

#elif defined(SPDK_KEVENT)
		sock = events[i].udata;                      /* [한국어] kqueue udata에서 sock 복원. */
		psock = __posix_sock(sock);
#endif

		/* If the socket is not already in the list, add it now */
		if (!psock->socket_has_data && !psock->pipe_has_data) { /* [한국어] 아직 has_data 리스트에 없으면 추가. */
			TAILQ_INSERT_TAIL(&group->socks_with_data, psock, link);
		}
		psock->socket_has_data = true;               /* [한국어] 커널 버퍼에 데이터 있음 표시. */
	}

	num_events = 0;                                      /* [한국어] socks[]에 채울 개수 리셋. */

	TAILQ_FOREACH_SAFE(psock, &group->socks_with_data, link, ptmp) { /* [한국어] 처리 가능 sock을 socks[]에 수집. */
		if (num_events == max_events) {              /* [한국어] 반환 한도 도달 — 나머지는 다음 사이클. */
			break;
		}

		/* If the socket's cb_fn is NULL, just remove it from the
		 * list and do not add it to socks array */
		if (spdk_unlikely(psock->base.cb_fn == NULL)) { /* [한국어] 콜백 없는 sock은 처리 불가 → 리스트에서 제거. */
			psock->socket_has_data = false;
			psock->pipe_has_data = false;
			TAILQ_REMOVE(&group->socks_with_data, psock, link);
			continue;
		}

		socks[num_events++] = &psock->base;          /* [한국어] 상위가 cb_fn 호출할 ready sock 등록. */
	}

	/* Cycle the has_data list so that each time we poll things aren't
	 * in the same order. Say we have 6 sockets in the list, named as follows:
	 * A B C D E F
	 * And all 6 sockets had epoll events, but max_events is only 3. That means
	 * psock currently points at D. We want to rearrange the list to the following:
	 * D E F A B C
	 *
	 * The variables below are named according to this example to make it easier to
	 * follow the swaps.
	 */
	if (psock != NULL) {                                 /* [한국어] max_events에서 멈췄으면 리스트를 회전(starvation 방지). */
		struct spdk_posix_sock *pa, *pc, *pd, *pf;   /* [한국어] 회전 swap을 위한 노드 포인터(주석 예시 명명). */

		/* Capture pointers to the elements we need */
		pd = psock;                                  /* [한국어] 다음 polling이 시작할 노드. */
		pc = TAILQ_PREV(pd, spdk_has_data_list, link); /* [한국어] D 직전 노드. */
		pa = TAILQ_FIRST(&group->socks_with_data);   /* [한국어] 기존 head. */
		pf = TAILQ_LAST(&group->socks_with_data, spdk_has_data_list); /* [한국어] 기존 tail. */

		/* Break the link between C and D */
		pc->link.tqe_next = NULL;                    /* [한국어] C와 D 사이 단절(C를 새 tail로). */

		/* Connect F to A */
		pf->link.tqe_next = pa;                      /* [한국어] 기존 tail F → 기존 head A 연결(원형 회전). */
		pa->link.tqe_prev = &pf->link.tqe_next;

		/* Fix up the list first/last pointers */
		group->socks_with_data.tqh_first = pd;       /* [한국어] 새 head = D. */
		group->socks_with_data.tqh_last = &pc->link.tqe_next; /* [한국어] 새 tail = C. */

		/* D is in front of the list, make tqe prev pointer point to the head of list */
		pd->link.tqe_prev = &group->socks_with_data.tqh_first; /* [한국어] D의 prev를 head 포인터로(TAILQ 불변식 복원). */
	}

	return num_events;                                   /* [한국어] 상위가 처리할 ready sock 수. */
}

/*
 * [한국어]
 * posix_sock_group_impl_get_interruptfd - group의 epoll/kqueue fd 반환 (vtable get_interruptfd).
 *
 * @return: epoll/kqueue fd.
 * interrupt-mode에서 이 fd가 readable이면 reactor가 깨어나 poll을 수행하도록 fd_group에 등록된다.
 */
static int
posix_sock_group_impl_get_interruptfd(struct spdk_sock_group_impl *_group)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group); /* [한국어] 구체 group. */

	return group->fd;                                    /* [한국어] epoll/kqueue fd 노출. */
}

/*
 * [한국어]
 * _sock_group_impl_close - group_impl 해제(공통). placement 매핑·pipe_group·epoll fd 정리.
 *
 * @enable_placement_id: PLACEMENT_CPU면 현재 코어의 map 매핑 해제.
 * @return: close 결과(0/음수 errno).
 *
 * 호출 체인: posix/ssl_sock_group_impl_close → [이 함수].
 */
static int
_sock_group_impl_close(struct spdk_sock_group_impl *_group, uint32_t enable_placement_id)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group); /* [한국어] 구체 group. */
	int rc;                                              /* [한국어] close 결과. */

	if (enable_placement_id == PLACEMENT_CPU) {          /* [한국어] CPU 모드면 코어 매핑 참조 해제. */
		spdk_sock_map_release(&g_map, spdk_env_get_current_core());
	}

	spdk_pipe_group_destroy(group->pipe_group);          /* [한국어] pipe_group 파괴. */
	rc = close(group->fd);                               /* [한국어] epoll/kqueue fd close. */
	free(group);                                         /* [한국어] group 객체 해제. */
	return rc < 0 ? -errno : rc;
}

/*
 * [한국어]
 * posix_sock_group_impl_close - posix group_impl 해제 (vtable). posix placement 모드 전달.
 */
static int
posix_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	return _sock_group_impl_close(_group, g_posix_impl_opts.enable_placement_id); /* [한국어] posix 옵션 placement로 정리. */
}

/*
 * [한국어]
 * ssl_sock_group_impl_close - ssl group_impl 해제 (vtable). ssl placement 모드 전달.
 */
static int
ssl_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	return _sock_group_impl_close(_group, g_ssl_impl_opts.enable_placement_id); /* [한국어] ssl 옵션 placement로 정리. */
}

/*
 * [한국어]
 * posix_connect_poller - non-blocking connect의 완료를 폴링하고 완료 시 deferred 초기화 수행.
 *
 * @sock: connect 진행 중인 sock.
 * @return: 0(완료), -EAGAIN(진행 중), 음수 errno(실패).
 *
 * 동기: connect_async 모델에서 모든 I/O 진입점(readv/writev/flush/is_connected 등)이 먼저 이
 * 함수를 불러 connect 완료 여부를 확인한다. 완료되면 fd 확정 + zerocopy/SSL/RCVBUF 등 지연 설정을
 * 한 번에 적용하고 group에 add한다.
 *
 * 동작 단계:
 *   1) 이미 ready면 0. ctx 없으면 -ENOTCONN.
 *   2) connect_timeout 초과면 -ETIMEDOUT.
 *   3) fd_connect_poll_async로 SO_ERROR 확인 — EAGAIN이면 그대로 진행 중.
 *   4) 현재 후보 실패면 fd close 후 다음 addrinfo 후보로 재시도(-EAGAIN 반환).
 *   5) 성공: fd 확정 → posix_sock_init(zerocopy) → SSL 구성 → 지연 RCVLOWAT/RCVBUF/SNDBUF →
 *      group add. 어느 단계든 실패면 err로.
 *   6) 마지막에 ctx cleanup(콜백 호출 포함).
 *
 * 실행 컨텍스트: I/O 경로 또는 group poll 중. 호출 체인: posix_sock_readv/_sock_flush/... → [이 함수].
 */
static int
posix_connect_poller(struct spdk_posix_sock *sock)
{
	struct posix_connect_ctx *ctx = sock->connect_ctx;   /* [한국어] connect 진행 컨텍스트. */
	int rc;                                              /* [한국어] 각 단계 결과. */

	if (sock->ready) {                                   /* [한국어] 이미 완료된 sock. */
		return 0;
	} else if (!ctx) {                                   /* [한국어] ctx 없음 = connect 실패 상태. */
		return -ENOTCONN;
	}

	if (ctx->opts.connect_timeout && ctx->timeout_tsc < spdk_get_ticks()) { /* [한국어] 타임아웃 경과. */
		rc = -ETIMEDOUT;
		goto err;
	}

	rc = spdk_sock_posix_fd_connect_poll_async(ctx->fd); /* [한국어] getsockopt(SO_ERROR)로 connect 결과 확인. */
	if (rc == -EAGAIN) {                                 /* [한국어] 아직 진행 중. */
		return -EAGAIN;
	}

	if (rc < 0) {                                        /* [한국어] 현재 후보 connect 실패. */
		int _rc = rc;                                /* [한국어] 첫 실패 코드 보존(다음 후보도 실패 시 보고용). */

		close(ctx->fd);                              /* [한국어] 실패한 fd 회수. */
		ctx->fd = -1;
		rc = _sock_posix_connect_async(ctx);         /* [한국어] 다음 addrinfo 후보로 재시도. */
		if (rc < 0) {                                /* [한국어] 후보 소진 — 원래 실패 코드 보고. */
			rc = _rc;
			goto err;
		}

		return -EAGAIN;                              /* [한국어] 새 후보 connect 진행 중. */
	}

	/* Connection established, proceed to deferred initialization. */
	sock->fd = ctx->fd;                                  /* [한국어] 확정된 fd를 sock에 반영. */

	/* Only enable zero copy for non-loopback and non-ssl sockets. */
	posix_sock_init(sock, sock->base.opts.zcopy && !spdk_net_is_loopback(sock->fd) && !ctx->ssl &&
			sock->base.impl_opts.enable_zerocopy_send_client); /* [한국어] loopback/SSL 제외 + client zcopy 옵션 시 zerocopy 활성. */

	if (ctx->ssl) {                                      /* [한국어] SSL connect면 클라이언트 핸드셰이크 준비. */
		rc = posix_sock_configure_ssl(sock, true);
		if (rc < 0) {
			goto err;
		}
	}

	if (ctx->set_recvlowat != -1) {                      /* [한국어] connect 전 예약된 RCVLOWAT 적용. */
		rc = posix_sock_set_recvlowat(&sock->base, ctx->set_recvlowat);
		if (rc < 0) {
			SPDK_ERRLOG("Connection was established but delayed posix_sock_set_recvlowat() failed, rc %d: %s.\n",
				    rc, spdk_strerror(-rc));
			goto err;
		}
	}

	if (ctx->set_recvbuf != -1) {                        /* [한국어] 예약된 RCVBUF 적용. */
		rc = posix_sock_set_recvbuf(&sock->base, ctx->set_recvbuf);
		if (rc < 0) {
			SPDK_ERRLOG("Connection was established but delayed posix_sock_set_recvbuf() failed, rc %d: %s.\n",
				    rc, spdk_strerror(-rc));
			goto err;
		}
	}

	if (ctx->set_sendbuf != -1) {                        /* [한국어] 예약된 SNDBUF 적용. */
		rc = posix_sock_set_sendbuf(&sock->base, ctx->set_sendbuf);
		if (rc < 0) {
			SPDK_ERRLOG("Connection was established but delayed posix_sock_set_sendbuf() failed, rc %d: %s.\n",
				    rc, spdk_strerror(-rc));
			goto err;
		}
	}

	if (sock->base.group_impl) {                         /* [한국어] connect 전 group이 캐시돼 있었으면 지금 epoll 등록. */
		rc = posix_sock_group_impl_add_sock(sock->base.group_impl, &sock->base);
		if (rc) {
			SPDK_ERRLOG("Connection was established but delayed posix_sock_group_impl_add_sock() failed %d (errno=%d).\n",
				    rc, errno);
			rc = -errno;
			goto err;
		}
	}

	goto out;                                            /* [한국어] 성공 — cleanup(콜백 호출)으로. */

err:
	/* It is safe to pass NULL to SSL free functions. */
	SSL_free(sock->ssl);                                 /* [한국어] 실패 — SSL 자원 정리(NULL-safe). */
	SSL_CTX_free(sock->ssl_ctx);
	if (ctx->fd != -1) {                                 /* [한국어] 열린 fd 회수. */
		close(ctx->fd);
	}

	sock->fd = -1;                                       /* [한국어] fd 무효화. */
	sock->ready = false;                                 /* [한국어] 미준비 상태 확정. */

out:
	sock_posix_connect_ctx_cleanup(&sock->connect_ctx, rc); /* [한국어] ctx 해제 + (async면) 결과 콜백 호출. */
	return rc;
}

/*
 * [한국어]
 * posix_net_impl_init - posix 백엔드 초기화 훅 (vtable init). 현재는 no-op.
 *
 * @opts: 초기화 옵션(미사용).
 * @return: 항상 0.
 * spdk_net_impl 등록 시 한 번 호출되는 자리표시자 — posix는 특별한 전역 초기화가 필요 없다.
 */
static int
posix_net_impl_init(struct spdk_sock_initialize_opts *opts)
{
	return 0;                                            /* [한국어] 초기화 불필요. */
}

/* [한국어] posix 백엔드의 vtable(spdk_net_impl). lib/sock/sock.c가 이 함수 포인터들을 통해
 * 추상 API를 백엔드 구현으로 디스패치한다. 각 슬롯은 위에서 정의한 posix_* 함수와 1:1 대응.
 * SPDK_NET_IMPL_REGISTER_DEFAULT가 이 인스턴스를 g_net_impls에 등록하고 default로 지정한다. */
static struct spdk_net_impl g_posix_net_impl = {
	.name		= "posix",                   /* [한국어] 백엔드 식별 이름 — impl_name="posix"로 선택되거나 default. */
	.init		= posix_net_impl_init,       /* [한국어] 등록 시 1회 초기화(no-op). */
	.getaddr	= posix_sock_getaddr,        /* [한국어] local/peer 주소·포트 추출. */
	.get_interface_name = posix_sock_get_interface_name, /* [한국어] sock의 NIC 이름. */
	.get_numa_id	= posix_sock_get_numa_id,    /* [한국어] NIC의 NUMA 노드 id. */
	.connect	= posix_sock_connect,        /* [한국어] 동기 connect. */
	.connect_async	= posix_sock_connect_async,  /* [한국어] 비동기 connect. */
	.listen		= posix_sock_listen,         /* [한국어] server socket 생성. */
	.accept		= posix_sock_accept,         /* [한국어] 연결 수락. */
	.close		= posix_sock_close,          /* [한국어] sock 종료·자원 해제. */
	.recv		= posix_sock_recv,           /* [한국어] 단일 버퍼 수신. */
	.readv		= posix_sock_readv,          /* [한국어] iovec 수신(recv_pipe 경유 가능). */
	.writev		= posix_sock_writev,         /* [한국어] 동기 송신. */
	.recv_next	= posix_sock_recv_next,      /* [한국어] group 버퍼 zero-copy recv. */
	.writev_async	= posix_sock_writev_async,   /* [한국어] 비동기 batch 송신. */
	.flush		= posix_sock_flush,          /* [한국어] 큐된 송신 즉시 flush + zcopy 수확. */
	.set_recvlowat	= posix_sock_set_recvlowat,  /* [한국어] SO_RCVLOWAT 설정. */
	.set_recvbuf	= posix_sock_set_recvbuf,    /* [한국어] SO_RCVBUF + recv_pipe 재할당. */
	.set_sendbuf	= posix_sock_set_sendbuf,    /* [한국어] SO_SNDBUF 설정. */
	.is_ipv6	= posix_sock_is_ipv6,        /* [한국어] IPv6 family 확인. */
	.is_ipv4	= posix_sock_is_ipv4,        /* [한국어] IPv4 family 확인. */
	.is_connected	= posix_sock_is_connected,   /* [한국어] MSG_PEEK 연결 상태 확인. */
	.group_impl_get_optimal	= posix_sock_group_impl_get_optimal, /* [한국어] placement 기반 최적 group 추천. */
	.group_impl_create	= posix_sock_group_impl_create, /* [한국어] epoll group 생성. */
	.group_impl_add_sock	= posix_sock_group_impl_add_sock, /* [한국어] sock을 epoll에 등록. */
	.group_impl_remove_sock = posix_sock_group_impl_remove_sock, /* [한국어] epoll에서 sock 제거. */
	.group_impl_poll	= posix_sock_group_impl_poll, /* [한국어] epoll_wait + has_data 처리. */
	.group_impl_get_interruptfd    = posix_sock_group_impl_get_interruptfd, /* [한국어] interrupt-mode용 epoll fd. */
	.group_impl_close	= posix_sock_group_impl_close, /* [한국어] group 해제. */
	.get_opts	= posix_sock_impl_get_opts,  /* [한국어] g_posix_impl_opts 조회. */
	.set_opts	= posix_sock_impl_set_opts,  /* [한국어] g_posix_impl_opts 갱신(RPC). */
};

/* [한국어] posix 백엔드를 sock 레이어에 default impl로 등록(constructor 시점 자동 실행). */
SPDK_NET_IMPL_REGISTER_DEFAULT(posix, &g_posix_net_impl);

/*
 * [한국어]
 * ssl_sock_listen - ssl 백엔드 listen vtable. enable_ssl=true로 _posix_sock_listen 위임.
 * accept된 연결마다 TLS 서버 핸드셰이크가 수행되는 listening sock을 만든다.
 */
static struct spdk_sock *
ssl_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return _posix_sock_listen(ip, port, opts, true);     /* [한국어] SSL 활성 listen. */
}

/*
 * [한국어]
 * ssl_sock_connect - ssl 백엔드 동기 connect vtable. enable_ssl=true, async=false.
 * connect + TLS 클라이언트 핸드셰이크 완료까지 블로킹.
 */
static struct spdk_sock *
ssl_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return _posix_sock_connect(ip, port, opts, false, true, NULL, NULL); /* [한국어] sync, SSL. */
}

/*
 * [한국어]
 * ssl_sock_connect_async - ssl 백엔드 비동기 connect vtable. enable_ssl=true, async=true.
 * connect와 TLS 핸드셰이크 모두 poller가 진행, 완료 시 cb_fn 호출.
 */
static struct spdk_sock *
ssl_sock_connect_async(const char *ip, int port, struct spdk_sock_opts *opts,
		       spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	return _posix_sock_connect(ip, port, opts, true, true, cb_fn, cb_arg); /* [한국어] async, SSL. */
}

/*
 * [한국어]
 * ssl_sock_accept - ssl 백엔드 accept vtable. enable_ssl=true로 _posix_sock_accept 위임.
 * 수락된 fd에 서버측 SSL 세션을 구성한다.
 */
static struct spdk_sock *
ssl_sock_accept(struct spdk_sock *_sock)
{
	return _posix_sock_accept(_sock, true);              /* [한국어] SSL accept. */
}

/*
 * [한국어]
 * ssl_net_impl_init - ssl 백엔드 초기화 훅 (vtable init). no-op 자리표시자.
 * OpenSSL 전역 초기화는 컨텍스트 생성 시점(posix_sock_create_ssl_context)에 lazy 수행되므로 여기선 불필요.
 */
static int
ssl_net_impl_init(struct spdk_sock_initialize_opts *opts)
{
	return 0;                                            /* [한국어] 초기화 불필요. */
}

/* [한국어] ssl 백엔드 vtable. posix와 대부분 함수를 공유하되 listen/connect/accept/group_create/
 * close/get_opts/set_opts만 ssl 전용(TLS 핸드셰이크·ssl 옵션)으로 교체. impl_name="ssl"로 명시
 * 지정 시에만 사용되며 default는 아니다. */
static struct spdk_net_impl g_ssl_net_impl = {
	.name		= "ssl",                     /* [한국어] 백엔드 이름 — impl_name="ssl"로만 선택. */
	.init		= ssl_net_impl_init,         /* [한국어] 초기화(no-op). */
	.getaddr	= posix_sock_getaddr,        /* [한국어] 주소 추출은 posix와 공유(TLS 무관). */
	.get_interface_name = posix_sock_get_interface_name, /* [한국어] NIC 이름 공유. */
	.get_numa_id	= posix_sock_get_numa_id,    /* [한국어] NUMA id 공유. */
	.connect	= ssl_sock_connect,          /* [한국어] TLS 동기 connect. */
	.connect_async	= ssl_sock_connect_async,    /* [한국어] TLS 비동기 connect. */
	.listen		= ssl_sock_listen,           /* [한국어] TLS listen. */
	.accept		= ssl_sock_accept,           /* [한국어] TLS accept. */
	.close		= posix_sock_close,          /* [한국어] close는 SSL_free 포함하므로 posix와 공유. */
	.recv		= posix_sock_recv,           /* [한국어] recv는 내부에서 ssl 분기(공유). */
	.readv		= posix_sock_readv,          /* [한국어] readv도 SSL_read 분기 내장(공유). */
	.writev		= posix_sock_writev,         /* [한국어] writev도 SSL_write 분기 내장(공유). */
	.recv_next	= posix_sock_recv_next,      /* [한국어] 공유. */
	.writev_async	= posix_sock_writev_async,   /* [한국어] 공유. */
	.flush		= posix_sock_flush,          /* [한국어] 공유. */
	.set_recvlowat	= posix_sock_set_recvlowat,  /* [한국어] 공유. */
	.set_recvbuf	= posix_sock_set_recvbuf,    /* [한국어] 공유. */
	.set_sendbuf	= posix_sock_set_sendbuf,    /* [한국어] 공유. */
	.is_ipv6	= posix_sock_is_ipv6,        /* [한국어] 공유. */
	.is_ipv4	= posix_sock_is_ipv4,        /* [한국어] 공유. */
	.is_connected	= posix_sock_is_connected,   /* [한국어] 공유. */
	.group_impl_get_optimal	= posix_sock_group_impl_get_optimal, /* [한국어] placement 로직 공유. */
	.group_impl_create	= ssl_sock_group_impl_create, /* [한국어] ssl 옵션 placement로 group 생성. */
	.group_impl_add_sock	= posix_sock_group_impl_add_sock, /* [한국어] 공유. */
	.group_impl_remove_sock = posix_sock_group_impl_remove_sock, /* [한국어] 공유. */
	.group_impl_poll	= posix_sock_group_impl_poll, /* [한국어] 공유(epoll polling은 TLS 무관). */
	.group_impl_get_interruptfd   = posix_sock_group_impl_get_interruptfd, /* [한국어] 공유. */
	.group_impl_close	= ssl_sock_group_impl_close, /* [한국어] ssl placement로 group 해제. */
	.get_opts	= ssl_sock_impl_get_opts,    /* [한국어] g_ssl_impl_opts 조회. */
	.set_opts	= ssl_sock_impl_set_opts,    /* [한국어] g_ssl_impl_opts 갱신. */
};

/* [한국어] ssl 백엔드 등록(default 아님 — 명시 선택 시 사용). */
SPDK_NET_IMPL_REGISTER(ssl, &g_ssl_net_impl);
/* [한국어] sock_posix 디버그 로그 컴포넌트 등록 — SPDK_DEBUGLOG(sock_posix, ...) 활성화 플래그. */
SPDK_LOG_REGISTER_COMPONENT(sock_posix)
