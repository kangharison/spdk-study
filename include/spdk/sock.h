/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 */

/** \file
 * TCP socket abstraction layer
 */

/*
 * [한국어 설명] SPDK 소켓 추상화 공개 API 헤더 (sock.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 의 모든 네트워크 I/O 가 사용하는 통일된 소켓 인터페이스를 제공한다.
 * NVMe-oF (NVMe over Fabrics) TCP, iSCSI target, vhost-user, JSON-RPC 서버 등
 * SPDK 가 외부와 TCP 통신을 하는 모든 컴포넌트는 BSD socket 을 직접 사용하지 않고
 * 본 API 를 통해 백엔드(posix/uring/ssl/vpp)를 일관된 형태로 호출한다. 백엔드는
 * 런타임에 spdk_net_impl 구조체를 통해 vtable 형태로 등록되며, 이 헤더는 호출자
 * 시각의 통일된 추상 API 만을 노출한다. 폴드모드 reactor 모델과 결합하기 위해
 * 비동기 writev_async, sock_group(epoll/io_uring 통합 폴링), zerocopy(MSG_ZEROCOPY)
 * 통지, TLS PSK(NVMe-oF TCP TLS 1.3) 와 같은 SPDK 특화 기능을 일급으로 다룬다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택 중 "네트워크 transport" 계층의 최상위 추상화이며, 호출 체인은
 * 다음과 같다.
 *   [iSCSI/NVMe-oF/RPC subsystem]
 *     → [transport(TCP) layer]
 *       → spdk_sock_*** API (이 헤더)
 *         → spdk_net_impl vtable (posix/uring/ssl/vpp)
 *           → 커널 syscall (recvmsg/sendmsg/epoll_wait, io_uring_enter)
 * 실행 컨텍스트는 SPDK reactor 가 점유한 사용자 스레드(spdk_thread)이며, 한
 * socket 과 sock_group 은 한 spdk_thread 에 affinity 가 고정되어 락 없이
 * (lockless) 폴링된다. 따라서 본 API 는 "현재 spdk_thread 에서만 호출"되어야
 * 하며, 다른 스레드에서 접근하려면 spdk_thread_send_msg 로 전달해야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h(POSIX 표준 헤더), spdk/queue.h(BSD queue 매크로 — TAILQ),
 * spdk/json.h(RPC config_json 출력), spdk/assert.h(SPDK_STATIC_ASSERT 로 ABI 안정성
 * 검증). 백엔드 구현은 lib/sock/(posix.c, uring.c, ssl.c, vpp.c) 및
 * include/spdk_internal/sock.h(spdk_net_impl 정의, 비공개 vtable) 에 존재한다.
 * 데이터 흐름: 상위 모듈이 iovec 배열을 spdk_sock_request 에 부착해 writev_async 로
 * 큐잉하면, sock_group_poll() 이 reactor 폴링 도중 백엔드 sendmsg 를 호출해
 * 송신하고 완료 시 cb_fn(cb_arg, err) 를 같은 spdk_thread 컨텍스트에서 부른다.
 * RX 경로는 group->sock 의 epoll/io_uring 이벤트 → recvmsg → 사용자 콜백
 * (spdk_sock_cb) 호출 형태로 흘러간다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - spdk_sock_listen / spdk_sock_listen_ext : 서버측 bind+listen 후 수동 sock 반환.
 *  - spdk_sock_connect / connect_ext / connect_async : 클라이언트측 능동 sock 생성.
 *  - spdk_sock_writev_async : iovec 비동기 송신, 완료 시 cb_fn 호출.
 *  - spdk_sock_group_create / add_sock / poll : 여러 sock 을 한 reactor 에서
 *    epoll/io_uring 으로 통합 폴링하는 그룹 관리.
 *  - spdk_sock_impl_get_opts / set_opts / set_default_impl : 백엔드(posix/uring/ssl)
 *    선택과 옵션(zerocopy threshold, TLS PSK, ktls 등) 조정.
 *  - struct spdk_sock_request : 비동기 송신 요청 헤드(64B 고정 크기, iovec 가
 *    뒤에 in-place 로 따라옴 — flexible array 회피용 컨벤션).
 *  - struct spdk_sock_opts : 연결/리슨 시점의 우선순위, ack_timeout, zcopy 활성,
 *    impl_opts 오버라이드, 소스 주소/포트, connect_timeout 을 캡슐화.
 *  - struct spdk_sock_impl_opts : 백엔드 구현 단위 옵션(버퍼 크기, 큌ACK,
 *    zerocopy 임계, placement_id, TLS 버전/PSK/cipher 등).
 *  - enum spdk_placement_mode : socket cluster 분산 정책(NAPI/CPU/MARK) — TCP
 *    shard 분산용으로 cgroup-aware affinity 와 결합.
 */

/*
 * [한국어] include guard 시작 — 다중 포함 시 토큰 재정의를 막기 위함.
 * 모든 SPDK 공개 헤더는 SPDK_<MODULE>_H 형태의 가드를 사용한다.
 */
#ifndef SPDK_SOCK_H
#define SPDK_SOCK_H

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 inclusion 헤더 — <stdint.h>, <stdbool.h>, <sys/uio.h>(iovec),
 * <sys/socket.h>, <unistd.h> 등 POSIX 헤더를 한 번에 들여온다. iovec/ssize_t
 * 타입을 본 헤더에서 사용하므로 가장 먼저 포함되어야 한다. */

#include "spdk/queue.h"
/* [한국어] BSD <sys/queue.h> 호환 매크로(TAILQ_*, LIST_*) 제공. spdk_sock_request
 * 의 internal.link 가 TAILQ_ENTRY 로 선언되어 있어 백엔드가 송신 대기열에 요청을
 * 매다는 데 사용한다. */

#include "spdk/json.h"
/* [한국어] JSON-RPC 출력 빌더(struct spdk_json_write_ctx) 정의. 본 헤더의
 * spdk_sock_write_config_json() 이 RPC save_config 핸들러에서 호출되며 socket
 * 모듈 옵션을 JSON 으로 직렬화하기 위해 필요하다. */

#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT 매크로 — 컴파일 타임 ABI 검증용. 아래에서
 * spdk_sock_request 가 정확히 64바이트, spdk_sock_opts 가 56바이트인지를
 * 정적으로 단언해 ABI 호환을 보장한다. */

/*
 * [한국어] C++ 에서 본 헤더를 포함할 때 C ABI(name mangling 회피)로 노출하기
 * 위한 가드. SPDK 는 C 라이브러리이지만 일부 사용자(QEMU, vhost client)는
 * C++ 코드에서 호출하므로 extern "C" 블록으로 감싸야 한다.
 */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * [한국어] 불투명(opaque) 전방 선언 — 본 헤더 사용자는 spdk_sock 의 내부 구조를
 * 알 필요가 없으며, 포인터로만 다룬다. 실제 정의는 lib/sock/sock.c 와
 * 백엔드(posix.c 등)에서 spdk_sock(공통 헤더)+백엔드별 확장 필드 형태로 존재.
 */
struct spdk_sock;

/*
 * [한국어] sock_group 의 전방 선언 — 한 reactor 에 묶인 여러 sock 을 통합
 * 폴링하는 컨테이너. 내부 구현은 백엔드별 epoll fd / io_uring SQ 을 보유한다.
 * 사용자는 본 포인터를 spdk_sock_group_*** API 로만 다룬다.
 */
struct spdk_sock_group;

/**
 * Anywhere this struct is used, an iovec array is assumed to
 * immediately follow the last member in memory, without any
 * padding.
 *
 * A simpler implementation would be to place a 0-length array
 * of struct iovec at the end of this request. However, embedding
 * a structure that ends with a variable length array inside of
 * another structure is a GNU C extension and not standard.
 */
/*
 * [한국어]
 * struct spdk_sock_request - 비동기 소켓 송신 요청의 헤더(메타데이터) 부분.
 *
 * 이 구조체 뒤에는 iovcnt 개의 struct iovec 가 곧바로 메모리에 연속 배치되며,
 * 별도 패딩 없이 바로 따라온다. 표준 C 에서는 가변 길이 배열을 구조체에
 * 임베드할 수 없기 때문에(GNU 확장), SPDK 는 명시적인 컨벤션(SPDK_SOCK_REQUEST_IOV
 * 매크로)으로 iovec 접근을 제공한다.
 *
 * 호출자는 sizeof(spdk_sock_request) + sizeof(iovec)*iovcnt 만큼 메모리를 할당한
 * 뒤 cb_fn/cb_arg/iovcnt 를 채우고, SPDK_SOCK_REQUEST_IOV(req, i) 매크로로 i 번째
 * iovec 에 base/len 을 채운 뒤 spdk_sock_writev_async 로 큐잉한다. 백엔드는
 * internal 영역만 수정한다.
 *
 * 실행 컨텍스트: 송신 콜백 cb_fn 은 sock 이 등록된 spdk_thread 의 reactor poll
 * 루프 내에서 호출된다 — 즉 다른 SPDK API 와 동일한 단일 스레드 컨텍스트.
 */
struct spdk_sock_request {
	/* When the request is completed, this callback will be called.
	 * On success, err will be:
	 *   - for writes: 0,
	 *   - for reads: number of bytes read.
	 * On failure: negative errno value.
	 */
	void	(*cb_fn)(void *cb_arg, int err);
	/* [한국어] 요청 완료 시 호출되는 콜백 함수 포인터.
	 * 설정자: 호출자(상위 transport, 예: NVMe-oF TCP)가 writev_async 호출 전에
	 *         자신만의 완료 처리 함수(예: nvmf_tcp_qpair_handle_pdu_send_complete)
	 *         로 채워둔다.
	 * 읽는 자: 백엔드(posix/uring)의 송신 완료 경로 또는 zerocopy 통지 경로에서
	 *         호출. err == 0 (write 성공) 또는 음수 errno(EPIPE, ECONNRESET 등).
	 * 값 범위: 유효한 함수 포인터 (NULL 불가 — NULL 일 경우 SPDK 가 어설션 또는
	 *         조용히 누락하므로 호출자가 반드시 채워야 함).
	 * 동기화: 단일 sock 은 단일 spdk_thread 에 affinity 되므로, 콜백 역시 그
	 *         스레드의 reactor poll 컨텍스트에서 직렬 호출 — 별도 락 불필요. */

	void				*cb_arg;
	/* [한국어] cb_fn 의 첫 번째 인자로 그대로 전달되는 컨텍스트 포인터.
	 * 설정자: 호출자가 자신의 PDU/connection 객체 포인터로 채움.
	 * 읽는 자: cb_fn 만 사용 — sock 레이어는 내용을 들여다보지 않음.
	 * 값 범위: 임의의 포인터(NULL 허용). 콜백 의미는 호출자 정의.
	 * 동기화: cb_fn 과 동일 — 단일 spdk_thread 컨텍스트. */

	/* These fields are used by the socket layer and should not be modified. */
	struct __sock_request_internal {
		/* [한국어] sock 레이어 전용 내부 상태 — 호출자가 손대면 ABI 가 깨짐.
		 * 백엔드의 송신 큐, zerocopy 추적, 디버그 검증에 사용된다. */

		TAILQ_ENTRY(spdk_sock_request)	link;
		/* [한국어] 백엔드 송신 큐(TAILQ)에 매달리는 링크 노드.
		 * 설정자: spdk_sock_writev_async 가 백엔드 큐(struct spdk_sock 의
		 *         queued_reqs 등) 에 TAILQ_INSERT_TAIL 로 추가하며 채움.
		 * 읽는 자: spdk_sock_flush 또는 sock_group_poll 이 큐에서 디큐할 때.
		 * 값 범위: 큐에 들어있는 동안에만 유효. 완료 후 해제 또는 재사용.
		 * 동기화: sock 의 affinity spdk_thread 외부 접근 금지. */

		/**
		 * curr_list is only used in DEBUG mode, but we include it in
		 * release builds too to ensure ABI compatibility between debug
		 * and release builds.
		 */
		void				*curr_list;
		/* [한국어] 디버그 빌드에서 현재 이 req 가 어느 큐(pending_reqs/
		 * queued_reqs)에 들어있는지 추적하는 포인터.
		 * 설정자: TAILQ 이동 시 백엔드가 자신이 속한 큐 헤드 주소를 저장.
		 * 읽는 자: 어설션(이중 enqueue 검출). release 빌드에서는 단순 패딩으로
		 *         두어 debug↔release ABI 호환을 보장.
		 * 값 범위: 큐 헤드 포인터 또는 NULL.
		 * 동기화: 단일 spdk_thread. */

		uint32_t			offset;
		/* [한국어] 부분 송신(partial sendmsg)된 바이트 수 누적값.
		 * 설정자: sendmsg 가 일부만 전송하고 EAGAIN 으로 반환한 경우 백엔드가
		 *         그동안 보낸 바이트만큼 offset 을 증가시킨다.
		 * 읽는 자: 다음 sendmsg 시 이 offset 만큼 iovec 시작 위치를 건너뜀.
		 * 값 범위: 0 ~ 전체 iov 합계. 송신 완료 시 cb_fn 호출 전에 의미가 사라짐.
		 * 동기화: sock 의 단일 affinity 스레드 외 접근 없음. */

		/* Last zero-copy sendmsg index. */
		uint32_t			zcopy_idx;
		/* [한국어] MSG_ZEROCOPY 송신 시 커널이 부여한 마지막 시퀀스 ID 저장.
		 * 설정자: 백엔드(posix.c)가 zerocopy sendmsg 후 SO_EE_ORIGIN_ZEROCOPY
		 *         이벤트를 매칭할 때 이 인덱스 범위와 비교.
		 * 읽는 자: MSG_ERRQUEUE recvmsg 로부터 받은 ee_data 와 비교해 어떤
		 *         req 가 커널 송신 완료(즉, 사용자 버퍼 재사용 가능)인지 판정.
		 * 값 범위: 0 ~ 2^32-1, 커널이 32-bit 카운터로 단조 증가시킴.
		 * 동기화: sock 의 affinity 스레드만 접근. */

		/* Indicate if the whole req or part of it is pending zerocopy completion. */
		bool pending_zcopy;
		/* [한국어] 이 요청의 일부가 아직 커널 zerocopy 완료(MSG_ERRQUEUE 통지)를
		 * 기다리는 중인지 표시.
		 * 설정자: zerocopy sendmsg 가 성공한 직후 true 로 표시 — cb_fn 호출은
		 *         커널이 사용자 버퍼를 더 이상 참조하지 않는다고 알릴 때까지 보류.
		 * 읽는 자: errqueue 폴링 루프가 false 로 클리어되면 비로소 cb_fn 호출.
		 * 값 범위: true(zerocopy 통지 대기) / false(통지 도착 또는 비-zerocopy).
		 * 동기화: sock 의 단일 affinity 스레드. */
	} internal;
	/* [한국어] 위의 4개 필드를 묶은 내부 sub-struct — sock 레이어 전용. */

	int				iovcnt;
	/* [한국어] 본 요청에 첨부된 iovec 개수. 헤더 바로 뒤에 iovcnt 개의 struct
	 * iovec 가 연속 배치된다.
	 * 설정자: 호출자가 writev_async 호출 전 채움.
	 * 읽는 자: 백엔드 sendmsg 가 msghdr.msg_iovlen 으로 사용.
	 * 값 범위: 1 이상의 양수. UIO_MAXIOV(보통 1024) 이내로 권장.
	 * 동기화: 요청 큐잉 후 변경 금지. */

	/* struct iovec			iov[]; */
	/* [한국어] 주석으로 표시된 가상 멤버 — 실제로 메모리상으로는 본 헤더 끝
	 * 주소부터 iovcnt 개의 iovec 가 따라온다. SPDK_SOCK_REQUEST_IOV(req, i)
	 * 매크로로 접근한다. */
};

/* [한국어] 컴파일 타임 검증 — spdk_sock_request 가 정확히 64바이트여야 ABI
 * 호환과 캐시 라인 정렬(64B = 일반 x86 cache line)을 보장. 필드 추가/삭제 시
 * sizeof 가 바뀌면 빌드가 실패하므로 무심코 ABI 가 변경되는 것을 막는다. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_sock_request) == 64, "Incorrect size.");

/* [한국어] req 뒤에 연속 배치된 i 번째 iovec 의 주소를 계산하는 매크로.
 * 헤더 시작 주소(req)에 sizeof(spdk_sock_request)=64 를 더하면 iov[0] 시작.
 * 거기서 sizeof(iovec)*i 만큼 더해 i 번째 iovec 의 포인터를 얻는다.
 * uint8_t * 캐스트는 바이트 단위 산술을 위함 — void * 에 +/-/sizeof 산술은
 * 표준이 아니므로 명시적 바이트 포인터를 거친다. */
#define SPDK_SOCK_REQUEST_IOV(req, i) ((struct iovec *)(((uint8_t *)req + sizeof(struct spdk_sock_request)) + (sizeof(struct iovec) * i)))

/*
 * [한국어]
 * enum spdk_placement_mode - socket placement_id (TCP shard 분산) 결정 방식.
 *
 * 다중 코어 환경에서 한 NIC 가 여러 RX queue 로 패킷을 분산할 때, 어떤 코어/
 * 스레드가 어떤 sock 을 처리할지 정하는 정책을 표현한다. SPDK 는 이를 통해
 * spdk_sock_get_optimal_sock_group 에서 sock 을 가장 적절한 reactor 에 매핑한다.
 */
enum spdk_placement_mode {
	PLACEMENT_NONE,
	/* [한국어] 배치 정책을 사용하지 않음 — sock 은 호출자가 add_sock 한
	 * 그룹에 그대로 머물며 reactor 마이그레이션이 일어나지 않는다.
	 * 설정자: spdk_sock_impl_opts.enable_placement_id 를 이 값으로 둘 때.
	 * 의미: 단일 코어 또는 affinity 를 호출자가 직접 통제하는 구성. */

	PLACEMENT_NAPI,
	/* [한국어] 리눅스 NAPI(Network API) ID 기반 배치 — 커널이 패킷을 처리한
	 * NAPI instance ID(SO_INCOMING_NAPI_ID) 를 사용해 같은 NAPI 를 폴링하는
	 * reactor 로 sock 을 묶는다.
	 * 의미: NIC RSS(Receive Side Scaling) 와 reactor affinity 정렬해
	 * RX-side 캐시 미스를 최소화. */

	PLACEMENT_CPU,
	/* [한국어] 패킷이 처리된 CPU ID(SO_INCOMING_CPU) 기반 배치.
	 * 의미: 인터럽트가 도달한 코어와 동일 코어에서 sock 을 폴링해
	 * cross-core sk_buff 이동을 줄인다 — NUMA-aware 와 결합. */

	PLACEMENT_MARK,
	/* [한국어] socket fwmark(SO_MARK)/cgroup classid 기반 배치.
	 * 의미: cgroup-aware affinity — 컨테이너 별로 sock 을 분리해 reactor
	 * 분배. 사용자 공간 정책으로 부여된 mark 를 그대로 group key 로 사용. */
};

/* [한국어] TLS 1.1 — RFC 4346. SPDK 에서는 보통 사용하지 않으나 ssl 백엔드의
 * impl_opts.tls_version 비교용 상수로 정의. 보안상 deprecated 이며 NVMe-oF TCP
 * 스펙은 TLS 1.3 만 정식 지원. */
#define SPDK_TLS_VERSION_1_1 11
/* [한국어] TLS 1.2 — RFC 5246. 호환성 목적으로 ssl 백엔드에서 선택 가능. */
#define SPDK_TLS_VERSION_1_2 12
/* [한국어] TLS 1.3 — RFC 8446. NVMe-oF TCP TLS PSK 는 TLS 1.3 EXPORTER
 * (RFC 5705) 메커니즘을 사용하므로 NVMe-oF 사용 시 본 값이 강제된다. */
#define SPDK_TLS_VERSION_1_3 13

/**
 * SPDK socket implementation options.
 *
 * A pointer to this structure is used by spdk_sock_impl_get_opts() and spdk_sock_impl_set_opts()
 * to allow the user to request options for the socket module implementation.
 * Each socket module defines which options from this structure are applicable to the module.
 */
/*
 * [한국어]
 * struct spdk_sock_impl_opts - 백엔드(posix/uring/ssl) 단위 구현 옵션 모음.
 *
 * 모든 백엔드가 모든 필드를 사용하는 것은 아니다 — 각 백엔드는 자신에게 적용되는
 * 필드만 해석하고 무시한다(예: tls_version 은 ssl 만, recv_buf_size 는 posix/uring
 * 만 인식). 사용자는 spdk_sock_impl_get_opts/set_opts 로 백엔드 이름을 지정해
 * 옵션을 변경하거나, spdk_sock_opts.impl_opts 로 sock 단위 오버라이드를 지정한다.
 */
struct spdk_sock_impl_opts {
	/**
	 * Minimum size of sock receive buffer. Used by posix and uring socket modules.
	 */
	uint32_t recv_buf_size;
	/* [한국어] SO_RCVBUF setsockopt 으로 설정할 RX 커널 버퍼 최소 크기(바이트).
	 * 설정자: 사용자가 RPC sock_impl_set_opts 또는 connect/listen 시 지정.
	 * 읽는 자: posix/uring 백엔드의 connect/listen 경로에서 setsockopt 호출.
	 * 값 범위: 0 = 시스템 기본 사용, 양수 = 명시적 크기. 커널이 net.core.rmem_max
	 *         로 클램프하므로 큰 값은 sysctl 조정도 필요.
	 * 동기화: 옵션 setter 는 set_default 로만 변경(전역 락은 net_impl 내부). */

	/**
	 * Minimum size of sock send buffer. Used by posix and uring socket modules.
	 */
	uint32_t send_buf_size;
	/* [한국어] SO_SNDBUF 으로 설정할 TX 커널 버퍼 최소 크기(바이트).
	 * 설정자: 사용자 옵션, 읽는 자: posix/uring backend.
	 * 값 범위: 0 = 시스템 기본, 양수 = 명시. NVMe-oF TCP 의 큰 PDU 가 부분
	 *         송신되지 않도록 충분히 크게 설정하는 것이 권장.
	 * 동기화: 위와 동일. */

	/**
	 * Enable or disable receive pipe. Used by posix and uring socket modules.
	 */
	bool enable_recv_pipe;
	/* [한국어] sock 내부 사용자공간 RX 파이프(spdk_pipe) 사용 여부.
	 * 설정자: 사용자, 읽는 자: posix/uring 백엔드 recv 경로.
	 * true 의미: recv 시 한 번에 큰 버퍼로 드레인 후 파이프에 적재 → 작은
	 *           recv() 반복 호출 시 syscall 횟수 감소 (PDU 헤더 8B 단위로
	 *           파싱하는 NVMe-oF TCP 에 유리).
	 * false 의미: 매 recv 마다 syscall 을 직접 호출. spdk_sock_recv_next 와는
	 *            상호 배타 — recv_next 사용 시 recvbuf=0 으로 비활성화 필요.
	 * 동기화: per-sock 단일 affinity 스레드. */

	/**
	 * **Deprecated, please use enable_zerocopy_send_server or enable_zerocopy_send_client instead**
	 * Enable or disable use of zero copy flow on send. Used by posix socket module.
	 */
	bool enable_zerocopy_send;
	/* [한국어] (Deprecated) 일반 송신 시 MSG_ZEROCOPY 사용 여부.
	 * 신규 코드는 enable_zerocopy_send_server / _client 를 사용해야 한다.
	 * 설정자: 사용자 옵션. 읽는 자: posix backend send 경로.
	 * true 시: sendmsg(MSG_ZEROCOPY) 호출 후 MSG_ERRQUEUE 로 통지 수신.
	 * 값 범위: true/false. 호환성 유지를 위해 남아있음. */

	/**
	 * Enable or disable quick ACK. Used by posix and uring socket modules.
	 */
	bool enable_quickack;
	/* [한국어] TCP_QUICKACK setsockopt 활성화 — delayed ACK 우회.
	 * 설정자: 사용자, 읽는 자: posix/uring backend connect/accept.
	 * true 의미: ACK 를 즉시 보내 RTT 가 짧은 NVMe-oF/iSCSI 에서 응답성 향상.
	 *           단, 커널이 매 recv 후 자동으로 quickack 모드를 해제하므로
	 *           반복 setsockopt 가 필요할 수 있음.
	 * 값 범위: true/false. */

	/**
	 * Enable or disable placement_id. Used by posix and uring socket modules.
	 * Valid values in the enum spdk_placement_mode.
	 */
	uint32_t enable_placement_id;
	/* [한국어] socket placement 정책 — TCP shard 의 reactor 매핑 방법.
	 * 설정자: 사용자(RPC sock_impl_set_opts).
	 * 읽는 자: spdk_sock_get_optimal_sock_group 호출 경로.
	 * 값 범위: enum spdk_placement_mode (NONE/NAPI/CPU/MARK).
	 * 동기화: backend-global, set 후 새 sock 부터 적용. */

	/**
	 * Enable or disable use of zero copy flow on send for server sockets. Used by posix and uring socket modules.
	 */
	bool enable_zerocopy_send_server;
	/* [한국어] accept 로 만든 서버측 sock 에서만 MSG_ZEROCOPY 활성화.
	 * 설정자: 사용자, 읽는 자: posix/uring backend accept 후 옵션 적용.
	 * 의미: 큰 응답(예: NVMe-oF TCP read response with payload)에서 user→kernel
	 *      page copy 회피 → CPU 절감. 통지 콜백은 ERRQUEUE 처리 후. */

	/**
	 * Enable or disable use of zero copy flow on send for client sockets. Used by posix and uring socket modules.
	 */
	bool enable_zerocopy_send_client;
	/* [한국어] connect 로 만든 클라이언트측 sock 에서만 MSG_ZEROCOPY 활성화.
	 * 설정자/읽는 자: 위와 동일. 클라이언트 write 가 크고 잦은 경우(예: NVMe-oF
	 * initiator) 유리. */

	/**
	 * Set zerocopy threshold in bytes. A consecutive sequence of requests' iovecs that fall below this
	 * threshold may be sent without zerocopy flag set.
	 */
	uint32_t zerocopy_threshold;
	/* [한국어] zerocopy 적용 임계값(바이트).
	 * 설정자: 사용자.
	 * 읽는 자: posix backend writev 경로 — 큐된 iovec 합산이 임계값 이상이어야
	 *         MSG_ZEROCOPY 를 사용하고, 그렇지 않으면 일반 sendmsg.
	 * 의미: 소량 송신은 ERRQUEUE 처리 비용이 페이지 핀 비용보다 비싸므로
	 *      임계값 이하에서 zerocopy 를 비활성. 일반적으로 ~16KB 권장. */

	/**
	 * TLS protocol version. Used by ssl socket module.
	 */
	uint32_t tls_version;
	/* [한국어] TLS 프로토콜 버전 — SPDK_TLS_VERSION_1_x 상수 중 하나.
	 * 설정자: 사용자.
	 * 읽는 자: ssl backend 가 OpenSSL/SSL_CTX_set_min_proto_version 에 전달.
	 * 값 범위: 11/12/13. NVMe-oF TCP 는 13(TLS 1.3) 강제. */

	/**
	 * Enable or disable kernel TLS. Used by ssl socket modules.
	 */
	bool enable_ktls;
	/* [한국어] 커널 TLS(KTLS, kernel TLS offload) 사용 여부.
	 * 설정자: 사용자.
	 * 읽는 자: ssl backend — handshake 완료 후 setsockopt(SOL_TLS) 로 키를
	 *         커널에 전달, 이후 send/recv 가 평문 인터페이스로 동작.
	 * 의미: TLS 암복호화를 NIC 또는 커널에서 처리해 사용자 공간 OpenSSL
	 *      EVP_Encrypt/Decrypt 비용을 제거. 단, 모든 cipher 가 KTLS 를 지원하지
	 *      않으므로 fallback 가능. */

	/**
	 * Set default PSK key. Used by ssl socket module.
	 */
	uint8_t *psk_key;
	/* [한국어] TLS PSK(Pre-Shared Key) 바이트 배열 포인터.
	 * 설정자: 사용자가 set_opts 호출 시 전달, 라이브러리 내부 복제됨.
	 * 읽는 자: ssl backend handshake 시 SSL_use_psk_identity_hint 등으로 사용.
	 * 값 범위: 16~64B 권장 (NVMe-oF TLS PSK 는 32B HMAC-SHA256 또는 48B SHA384).
	 * 동기화: set 시점에 backend-global. PSK 자체는 비밀이므로 secure_zero
	 *         가능 메모리에 두는 것이 권장. */

	/**
	 * Size of psk_key.
	 */
	uint32_t psk_key_size;
	/* [한국어] psk_key 가 가리키는 바이트 수.
	 * 설정자: 사용자, 읽는 자: ssl backend.
	 * 값 범위: 0(미설정) ~ 수십 바이트. 너무 짧으면 보안 약화. */

	/**
	 * Set default PSK identity. Used by ssl socket module.
	 */
	char *psk_identity;
	/* [한국어] TLS PSK identity 문자열(널 종단). 클라이언트가 hello 시 보내고
	 * 서버는 이 값을 보고 적절한 PSK 키를 선택한다.
	 * 설정자: 사용자(RPC), 읽는 자: ssl backend.
	 * 값 범위: 사용자 정의 — NVMe-oF TLS 의 NQN(NVMe Qualified Name) 기반
	 *         identity 형식이 표준. 예: "NVMe0R01<host_nqn><subsys_nqn>". */

	/**
	 * Optional callback to retrieve PSK based on client's identity.
	 *
	 * \param out Buffer for PSK in binary format to be filled with found key.
	 * \param out_len Length of "out" buffer.
	 * \param cipher Cipher suite to be set by this callback.
	 * \param psk_identity PSK identity for which the key needs to be found.
	 * \param get_key_ctx Context for this callback.
	 *
	 * \return key length on success, -1 on failure.
	 */
	int (*get_key)(uint8_t *out, int out_len, const char **cipher, const char *psk_identity,
		       void *get_key_ctx);
	/* [한국어] 서버측에서 클라이언트가 제시한 PSK identity 로부터 PSK 키를
	 * 동적으로 조회하는 콜백.
	 * 설정자: 사용자(NVMe-oF subsystem 의 PSK lookup 핸들러로 채움).
	 * 읽는 자: ssl backend 가 OpenSSL psk_server_callback 에서 호출.
	 * 인자: out=키를 채울 버퍼, out_len=버퍼 크기, cipher=콜백이 선택한 cipher
	 *      suite 문자열을 반환(예: "TLS_AES_128_GCM_SHA256"), psk_identity=클라
	 *      identity, get_key_ctx=설정 시 등록한 컨텍스트.
	 * 반환: 키 길이(>0) 또는 -1(실패 → handshake 거부). */

	/**
	 * Context to be passed to get_key() callback.
	 */
	void *get_key_ctx;
	/* [한국어] get_key 콜백의 마지막 인자로 전달되는 컨텍스트 포인터.
	 * 설정자: 사용자(NVMe-oF subsystem 포인터 등). 읽는 자: get_key 콜백 본체. */

	/**
	 * Cipher suite. Used by ssl socket module.
	 * For connecting side, it must contain a single cipher:
	 * example: "TLS_AES_256_GCM_SHA384"
	 *
	 * For listening side, it may be a colon separated list of ciphers:
	 * example: "TLS_AES_256_GCM_SHA384:TLS_AES_128_GCM_SHA256"
	 */
	const char *tls_cipher_suites;
	/* [한국어] TLS cipher suite 문자열 — OpenSSL 형식.
	 * 설정자: 사용자.
	 * 읽는 자: ssl backend 의 SSL_CTX_set_ciphersuites().
	 * 값 범위: 클라이언트는 단일 cipher, 서버는 콜론(:) 구분 리스트.
	 *         NVMe-oF TCP TLS 1.3 은 TLS_AES_128_GCM_SHA256 또는
	 *         TLS_AES_256_GCM_SHA384 만 허용 (RFC 8446 + NVMe-oF TLS profile). */
};

/**
 * Spdk socket initialization options.
 *
 * A pointer to this structure will be used by spdk_sock_listen_ext() or spdk_sock_connect_ext() to
 * allow the user to request non-default options on the socket.
 */
/*
 * [한국어]
 * struct spdk_sock_opts - listen/connect 호출 시점의 sock 단위 옵션.
 *
 * spdk_sock_impl_opts(백엔드 단위) 와 달리 본 구조체는 "이 sock 에만 적용"되는
 * 요청별 옵션이다. 추가로 impl_opts 포인터로 백엔드 옵션을 sock 단위로 오버라이드할
 * 수 있다. opts_size 필드로 ABI 호환을 보장한다 — 라이브러리가 더 새로운 필드를
 * 가지고 있을 때 호출자가 보낸 크기까지만 해석한다.
 */
struct spdk_sock_opts {
	/**
	 * The size of spdk_sock_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 */
	size_t opts_size;
	/* [한국어] 호출자가 알고 있는 spdk_sock_opts 크기(바이트).
	 * 설정자: 호출자가 sizeof(struct spdk_sock_opts) 또는 자신이 컴파일된 시점의
	 *         크기로 채움. 일반적으로 spdk_sock_get_default_opts 가 자동으로 채움.
	 * 읽는 자: SPDK 라이브러리 — opts_size 까지만 호출자가 채운 값으로 간주하고
	 *         그 이후 필드는 기본값으로 채움. ABI forward-compat 의 핵심.
	 * 값 범위: > 0, <= 라이브러리 내부 sizeof. */

	/**
	 * The priority on the socket and default value is zero.
	 */
	int priority;
	/* [한국어] SO_PRIORITY setsockopt 값 — 커널 qdisc 우선순위.
	 * 설정자: 호출자.
	 * 읽는 자: 백엔드 connect/listen 시 setsockopt 호출.
	 * 값 범위: 0(기본) ~ 6(보통 root 권한 필요한 영역). 우선순위 큐 분리 시 사용. */

	/**
	 * Used to enable or disable zero copy on socket layer.
	 */
	bool zcopy;
	/* [한국어] 이 sock 의 zerocopy 사용 여부 — impl_opts 에서 활성화된 경우에만
	 * 실제 적용되는 per-sock 토글.
	 * 설정자: 호출자.
	 * 읽는 자: writev_async 경로가 MSG_ZEROCOPY 사용 여부 결정 시. */

	/* Hole at bytes 13-15. */
	uint8_t reserved13[3];
	/* [한국어] 13~15 바이트 위치의 패딩 — bool zcopy(13B) 다음 4바이트 정렬 확보.
	 * 설정자: 호출자가 명시적으로 0 으로 채울 필요 없음(get_default_opts 가 0
	 *         으로 zero-init).
	 * 의미: ABI 호환을 위한 의도적 패딩으로, 향후 새 bool 필드가 들어올 자리. */

	/**
	 * Time in msec to wait ack until connection is closed forcefully.
	 */
	uint32_t ack_timeout;
	/* [한국어] 연결 종료 시 미수신 데이터에 대한 ACK 대기 시간(밀리초).
	 * 설정자: 호출자, 읽는 자: 백엔드 close 경로(TCP_USER_TIMEOUT setsockopt).
	 * 값 범위: 0 = OS 기본, 양수 = 강제 종료 전 대기시간.
	 * 의미: 끊긴 peer 가 ACK 못 보내는 경우 close() 가 무한정 블록되는 것을 방지. */

	/* Hole at bytes 20-23. */
	uint8_t reserved[4];
	/* [한국어] 20~23 바이트 패딩 — impl_opts(포인터, 8B 정렬 필요) 앞 패딩.
	 * 의미: 64-bit 시스템에서 포인터를 8바이트 경계에 두기 위함. */

	/**
	 * Socket implementation options.  If non-NULL, these will override those set by
	 * spdk_sock_impl_set_opts().  The library copies this structure internally, so the user can
	 * free it immediately after a spdk_sock_connect()/spdk_sock_listen() call.
	 */
	struct spdk_sock_impl_opts *impl_opts;
	/* [한국어] 이 sock 한정으로 백엔드 옵션을 오버라이드하는 포인터(NULL 허용).
	 * 설정자: 호출자가 connect/listen 직전에 자신만의 impl_opts 를 가리키게 설정.
	 * 읽는 자: 백엔드 connect/listen 가 NULL 이 아니면 set_default 로 등록된
	 *         전역 옵션 대신 이 포인터의 옵션을 사용. 호출 즉시 라이브러리가
	 *         내부 복제하므로 호출 후 호출자가 free 가능.
	 * 값 범위: 유효한 spdk_sock_impl_opts 포인터 또는 NULL. */

	/**
	 * Size of the impl_opts structure.
	 */
	size_t impl_opts_size;
	/* [한국어] impl_opts 가 가리키는 구조체의 크기(바이트) — ABI 호환용.
	 * 설정자: 호출자, 보통 sizeof(struct spdk_sock_impl_opts).
	 * 읽는 자: 라이브러리 — 크기를 받아 그만큼만 복사. */

	/**
	 * Source address.  If NULL, any available address will be used.  Only valid for connect().
	 */
	const char *src_addr;
	/* [한국어] connect 시 bind 할 출발지 IP 주소 문자열(예: "10.0.0.5"). NULL 이면
	 * 커널이 라우팅 테이블로 자동 선택.
	 * 설정자: 호출자, 읽는 자: connect 백엔드. listen 시에는 무시. */

	/**
	 * Source port.  If zero, a random ephemeral port will be used.  Only valid for connect().
	 */
	uint16_t src_port;
	/* [한국어] connect 시 bind 할 출발지 포트. 0 이면 ephemeral 포트 자동 할당.
	 * 설정자: 호출자, 읽는 자: connect 백엔드. */

	/* Hole at bytes 50-51. */
	uint8_t reserved50[2];
	/* [한국어] 50~51 바이트 패딩 — uint16_t src_port(48~49) 다음 4바이트 정렬용. */

	/**
	 * Time in msec to wait until connection is done (0 = no timeout).
	 */
	uint32_t connect_timeout;
	/* [한국어] connect 타임아웃(밀리초). 0 이면 타임아웃 없음(OS 기본 SYN
	 * 재전송 정책에 의존).
	 * 설정자: 호출자, 읽는 자: connect 백엔드 — 비동기 connect 시 timer 로
	 *         ETIMEDOUT 으로 실패 처리. */
};
/* [한국어] spdk_sock_opts 정확히 56바이트 정적 단언 — 위 reserved 패딩과 함께
 * 모든 SPDK 빌드에서 동일한 크기를 보장. ABI 변경 시 컴파일 실패. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_sock_opts) == 56, "Incorrect size");

/**
 * Options for the socket library.
 */
/*
 * [한국어]
 * struct spdk_sock_initialize_opts - sock 라이브러리 초기화 옵션.
 *
 * spdk_sock_initialize() 단 한 번 호출 시 전역 동작(폴드모드 vs 인터럽트모드)을
 * 결정한다. opts_size 로 ABI 호환을 유지한다.
 */
struct spdk_sock_initialize_opts {
	/**
	 * The size of spdk_sock_initialize_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 */
	size_t opts_size;
	/* [한국어] 호출자가 알고 있는 본 구조체 크기(바이트) — spdk_sock_opts 와
	 * 동일한 ABI forward-compat 패턴.
	 * 설정자: 호출자, 보통 spdk_sock_get_default_initialize_opts 가 자동 채움.
	 * 읽는 자: spdk_sock_initialize 가 이 크기까지만 호출자가 의도한 값으로 간주. */

	/*
	 * Enable or disable interrupt mode.
	 */
	bool enable_interrupt_mode;
	/* [한국어] reactor 폴링 대신 인터럽트(epoll wait, blocking) 모드 활성화.
	 * 설정자: 호출자(전력 절감 모드 등에서 활성화).
	 * 읽는 자: sock_group 의 폴링 루프 — 인터럽트 모드면 spdk_interrupt_register
	 *         로 이벤트 fd 를 reactor 에 등록해 폴링 대신 깨어남에 의존.
	 * 값 범위: true(인터럽트모드) / false(폴드모드, 기본).
	 * 의미: 폴드모드는 항상 100% CPU 를 쓰지만 latency 가 작고, 인터럽트 모드는
	 *      idle 시 CPU 절감하지만 syscall 오버헤드가 있다. */
};

/**
 * Initialize the default value of socket initialization opts.
 *
 * \param opts Data structure where SPDK will initialize the default socket initialization options.
 * \param opts_size Size of the opts structure being passed.
 */
/*
 * [한국어]
 * spdk_sock_get_default_initialize_opts - 라이브러리 초기화 옵션의 기본값을 채움.
 *
 * @opts: 호출자가 할당한 spdk_sock_initialize_opts 버퍼 — 이 함수가 default 로 채움.
 * @opts_size: opts 의 크기(sizeof). 라이브러리가 자기 sizeof 와 비교해 ABI 안전.
 * @return: void.
 *
 * spdk_sock_initialize 호출 전 사용자는 이 함수를 먼저 불러 안전한 기본값을
 * 받은 뒤 필요한 필드만 변경한다. 미래 버전에서 새 필드가 추가되어도 기존
 * 호출자는 변경 없이 동작.
 *
 * 실행 컨텍스트: SPDK 부팅 초기(spdk_app_start 진입 직후) 메인 thread.
 * 호출 체인:
 *   spdk_app_start → 사용자 시작 콜백 → [이 함수] → spdk_sock_initialize
 */
void spdk_sock_get_default_initialize_opts(struct spdk_sock_initialize_opts *opts,
		size_t opts_size);

/**
 * Initialize the socket module.
 *
 * This is an optional call that may be called before any use of the socket library.
 *
 * This function is not thread safe and should be called only once. Subsequent calls will succeed
 * if the options passed in are the same as the first invocation, otherwise it will return -EALREADY.
 *
 * \param opts Options for the socket library.
 *
 * \return 0 on success, negated errno on failure.
 */
/*
 * [한국어]
 * spdk_sock_initialize - sock 라이브러리 전역 초기화.
 *
 * @opts: spdk_sock_initialize_opts 포인터 — enable_interrupt_mode 등 전역 모드 결정.
 * @return: 0 성공, -EALREADY 이미 다른 옵션으로 초기화됨, 음수 errno 일반 실패.
 *
 * 실제로는 호출이 선택적이며, 호출하지 않으면 첫 spdk_sock_listen/connect 시점에
 * 기본 모드로 lazy 초기화된다. 인터럽트 모드를 쓰려면 반드시 connect/listen 전에
 * 본 함수를 호출해야 한다. 동일 옵션으로 재호출은 success(0), 다른 옵션으로
 * 재호출은 -EALREADY 로 거부 — 라이브러리 상태 일관성 유지.
 *
 * 실행 컨텍스트: SPDK 메인 thread, 단일 호출. 멀티스레드 안전 아님.
 *
 * 호출 체인:
 *   spdk_app_start → app start cb → [이 함수] → 백엔드별 init 콜백
 */
int spdk_sock_initialize(struct spdk_sock_initialize_opts *opts);

/**
 * Initialize the default value of opts.
 *
 * \param opts Data structure where SPDK will initialize the default sock options.
 * Users must set opts_size to sizeof(struct spdk_sock_opts).  This will ensure that the
 * libraryonly tries to fill as many fields as allocated by the caller. This allows ABI
 * compatibility with future versions of this library that may extend the spdk_sock_opts
 * structure.
 */
/*
 * [한국어]
 * spdk_sock_get_default_opts - per-sock listen/connect 옵션의 기본값을 채움.
 *
 * @opts: 호출자가 할당한 spdk_sock_opts. 호출 전 opts->opts_size 를 반드시
 *        sizeof(struct spdk_sock_opts) 로 채워야 함 — ABI 안전을 위함.
 * @return: void.
 *
 * 사용자는 이 함수로 안전한 디폴트(priority=0, zcopy=false 등)를 얻은 뒤 필요한
 * 필드만 수정해 spdk_sock_connect_ext/listen_ext 에 전달한다.
 *
 * 실행 컨텍스트: 임의 spdk_thread.
 *
 * 호출 체인:
 *   사용자 코드 → [이 함수] → (사용자가 일부 필드 변경) → spdk_sock_connect_ext
 */
void spdk_sock_get_default_opts(struct spdk_sock_opts *opts);

/**
 * Get client and server addresses of the given socket.
 *
 * This function is allowed only when connection is established.
 *
 * \param sock Socket to get address.
 * \param saddr A pointer to the buffer to hold the address of server.
 * \param slen Length of the buffer 'saddr'.
 * \param sport A pointer(May be NULL) to the buffer to hold the port info of server.
 * \param caddr A pointer to the buffer to hold the address of client.
 * \param clen Length of the buffer 'caddr'.
 * \param cport A pointer(May be NULL) to the buffer to hold the port info of server.
 *
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_getaddr - sock 의 양 끝(서버/클라이언트) 주소·포트를 조회.
 *
 * @sock: 연결이 설정된 sock(unconnected sock 호출 시 -ENOTCONN).
 * @saddr: 서버측(local) IP 문자열을 채울 버퍼.
 * @slen: saddr 의 크기 — getnameinfo 가 NI_MAXHOST 까지 채울 수 있으므로 충분히 크게.
 * @sport: 서버측 포트(uint16_t)를 받을 버퍼 또는 NULL(미관심).
 * @caddr: 클라이언트측(remote) IP 문자열 버퍼.
 * @clen: caddr 의 크기.
 * @cport: 클라이언트측 포트 버퍼 또는 NULL.
 * @return: 0 성공, 음수 errno 실패(ENOTCONN, ENOMEM, EINVAL 등).
 *
 * NVMe-oF host_traddr/trsvcid 로깅, RPC 출력, ACL 검증 등에서 호출. 백엔드는
 * getsockname/getpeername 시스템 콜로 sockaddr 을 얻은 뒤 inet_ntop 으로
 * 텍스트로 변환한다.
 *
 * 실행 컨텍스트: sock 의 affinity spdk_thread.
 *
 * 호출 체인:
 *   transport accept handler → [이 함수] → getsockname/getpeername(syscall) → inet_ntop
 */
int spdk_sock_getaddr(struct spdk_sock *sock, char *saddr, int slen, uint16_t *sport,
		      char *caddr, int clen, uint16_t *cport);

/**
 * Get socket implementation name.
 *
 * \param sock Pointer to SPDK socket.
 *
 * \return Implementation name of given socket.
 */
/*
 * [한국어]
 * spdk_sock_get_impl_name - sock 이 어떤 백엔드("posix"/"uring"/"ssl"/"vpp")로
 *   생성되었는지를 가리키는 정적 문자열 반환.
 *
 * @sock: 대상 sock.
 * @return: 백엔드 이름 문자열(소유권은 라이브러리, 호출자 free 금지).
 *
 * RPC 응답, 디버그 로그, NVMe-oF transport 모드 검증에 사용. 백엔드 이름은
 * spdk_net_impl 등록 시점에 결정되며 sock 수명 동안 변하지 않는다.
 *
 * 실행 컨텍스트: 임의 thread (read-only). 동기화 불필요.
 */
const char *spdk_sock_get_impl_name(struct spdk_sock *sock);

/**
 * Create a socket using the specific sock implementation, connect the socket
 * to the specified address and port (of the server), and then return the socket.
 * This function is used by client.
 *
 * \param ip IP address of the server.
 * \param port Port number of the server.
 * \param impl_name The sock implementation to use, such as "posix", or NULL for default.
 *
 * \return a pointer to the connected socket on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_sock_connect - 동기 능동 connect (blocking 형태). connect_ext 의 단순 버전.
 *
 * @ip: 서버 IP 문자열(IPv4/IPv6).
 * @port: 서버 포트.
 * @impl_name: 사용할 백엔드 이름 또는 NULL(기본 백엔드 사용).
 * @return: 연결된 sock 포인터 성공, NULL 실패(errno 통해 원인 확인).
 *
 * 내부적으로 spdk_sock_get_default_opts 로 기본 옵션을 채워 connect_ext 를 호출.
 * 동기 함수이므로 connect 가 완료될 때까지 호출 thread 를 블록 — reactor poll
 * 루프 중 호출 시 latency 영향이 있으니 보통 부팅 단계 또는 사용자 thread 에서 호출.
 *
 * 실행 컨텍스트: 임의 thread, 단 호출 thread 가 sock 의 affinity 를 가져감.
 *
 * 호출 체인:
 *   상위 transport(예: NVMe-oF TCP host transport connect)
 *     → [이 함수] → connect_ext → backend connect (socket/connect/setsockopt)
 */
struct spdk_sock *spdk_sock_connect(const char *ip, int port, const char *impl_name);

/**
 * Create a socket using the specific sock implementation, connect the socket
 * to the specified address and port (of the server), and then return the socket.
 * This function is used by client.
 *
 * \param ip IP address of the server.
 * \param port Port number of the server.
 * \param impl_name The sock implementation to use, such as "posix", or NULL for default.
 * \param opts The sock option pointer provided by the user which should not be NULL pointer.
 *
 * \return a pointer to the connected socket on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_sock_connect_ext - 동기 능동 connect, 사용자 지정 opts.
 *
 * @ip: 서버 IP.
 * @port: 서버 포트.
 * @impl_name: 백엔드 이름 또는 NULL(기본).
 * @opts: spdk_sock_opts 포인터(NULL 금지) — priority/zcopy/ack_timeout/impl_opts 등 지정.
 * @return: sock 포인터 또는 NULL.
 *
 * 백엔드 선택 우선순위:
 *   1) impl_name 이 NULL 아니면 그 백엔드만 시도.
 *   2) NULL 이면 spdk_sock_set_default_impl 로 등록된 기본 백엔드.
 *   3) 그것도 없으면 빌드 시 enabled 백엔드 중 첫 번째.
 *
 * 실행 컨텍스트: 임의 thread.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → 백엔드 connect (socket → setsockopt → connect → SO_PRIORITY 등)
 */
struct spdk_sock *spdk_sock_connect_ext(const char *ip, int port, const char *impl_name,
					struct spdk_sock_opts *opts);

/**
 * Signature for callback function invoked when a connection is completed.
 *
 * \param cb_arg Context specified by \ref spdk_sock_connect_async.
 * \param status 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * typedef spdk_sock_connect_cb_fn - 비동기 connect 완료 콜백 시그니처.
 *
 * @cb_arg: spdk_sock_connect_async 호출 시 전달한 컨텍스트.
 * @status: 0 = connect 성공, 음수 errno = 실패(ECONNREFUSED, ETIMEDOUT 등).
 *
 * 콜백은 sock 이 등록된 spdk_thread 의 reactor poll 컨텍스트에서 호출된다.
 * status==0 이후에야 readv/writev/group_add_sock 등 sock API 가 안전.
 */
typedef void (*spdk_sock_connect_cb_fn)(void *cb_arg, int status);

/**
 * Create a socket using the specific sock implementation, initiate the socket connection
 * to the specified address and port (of the server), and then return the socket.
 * This function is used by client.
 *
 * Not every function with sock object on the interface is allowed if the conncection is not
 * established. In order to determine connection status use \p cb_fn. Functions taking sock
 * object as an input may return EAGAIN to indicate connection is in progress or other
 * errno values if connection failed.
 *
 * Callback function \p cb_fn is invoked only if this function returns a non-NULL value.
 * If async connect is not supported by the \p impl_name specified then NULL is returned.
 *
 * \param ip IP address of the server.
 * \param port Port number of the server.
 * \param impl_name The sock implementation to use, such as "posix", or NULL for default.
 * \param opts The sock option pointer provided by the user which should not be NULL pointer.
 * \param cb_fn Callback function invoked when the connection attempt is completed. (optional)
 * \param cb_arg Argument passed to callback function. (optional)
 *
 * \return a pointer to the socket on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_sock_connect_async - 비동기 능동 connect — 호출 즉시 반환, 완료는 콜백으로 통지.
 *
 * @ip: 서버 IP.
 * @port: 서버 포트.
 * @impl_name: 백엔드 이름. 비동기 connect 미지원 백엔드면 본 함수가 NULL 반환.
 * @opts: per-sock 옵션(NULL 금지).
 * @cb_fn: connect 완료 콜백(optional, NULL 가능).
 * @cb_arg: cb_fn 첫 인자(optional).
 * @return: 진행 중 sock 포인터 또는 NULL.
 *
 * 호출 직후 sock 은 "연결 진행 중" 상태 — 일부 API (writev_async 등) 가 EAGAIN 으로
 * 응답하거나 큐에 쌓아 두었다가 연결 완료 후 송신을 시작한다. 완료/실패는 cb_fn
 * 으로 통지되며 cb_fn 호출 시점부터 모든 API 가 안전.
 *
 * NVMe-oF host 가 reactor 폴 루프를 멈추지 않고 다수 컨트롤러를 병렬 연결할 때 사용.
 *
 * 실행 컨텍스트: 호출 thread 가 affinity 를 가져감 — connect 진행 자체는 reactor
 * poll 도중 백엔드 connect 콜백을 통해 진행됨.
 *
 * 호출 체인:
 *   NVMe-oF host transport connect → [이 함수] → backend connect_async →
 *     reactor poll → epoll EPOLLOUT(connect 완료) → cb_fn 호출
 */
struct spdk_sock *spdk_sock_connect_async(const char *ip, int port, const char *impl_name,
		struct spdk_sock_opts *opts, spdk_sock_connect_cb_fn cb_fn, void *cb_arg);

/**
 * Create a socket using the specific sock implementation, bind the socket to
 * the specified address and port and listen on the socket, and then return the socket.
 * This function is used by server.
 *
 * \param ip IP address to listen on.
 * \param port Port number.
 * \param impl_name The sock implementation to use, such as "posix", or NULL for default.
 *
 * \return a pointer to the listened socket on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_sock_listen - 서버측 listen sock 생성 (단순 버전).
 *
 * @ip: 바인드할 로컬 IP. "0.0.0.0" 등 와일드카드 가능.
 * @port: 바인드할 로컬 포트.
 * @impl_name: 백엔드 이름 또는 NULL(기본).
 * @return: listening sock 또는 NULL.
 *
 * 내부적으로 listen_ext 를 호출(opts=default). 반환된 sock 은 spdk_sock_accept 로
 * 새 연결을 받는 데 사용. NVMe-oF target portal 부팅, RPC 서버, iSCSI portal 에서 사용.
 *
 * 실행 컨텍스트: 부팅 thread 또는 RPC 핸들러 thread.
 *
 * 호출 체인:
 *   transport listen_create_portal → [이 함수] → listen_ext → backend listen
 *     (socket → bind → listen → setsockopt SO_REUSEADDR)
 */
struct spdk_sock *spdk_sock_listen(const char *ip, int port, const char *impl_name);

/**
 * Create a socket using the specific sock implementation, bind the socket to
 * the specified address and port and listen on the socket, and then return the socket.
 * This function is used by server.
 *
 * \param ip IP address to listen on.
 * \param port Port number.
 * \param impl_name The sock implementation to use, such as "posix", or NULL for default.
 * \param opts The sock option pointer provided by the user, which should not be NULL pointer.
 *
 * \return a pointer to the listened socket on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_sock_listen_ext - 서버측 listen sock 생성, 사용자 지정 opts.
 *
 * @ip: 바인드 IP.
 * @port: 바인드 포트.
 * @impl_name: 백엔드 또는 NULL.
 * @opts: spdk_sock_opts 포인터(NULL 금지) — TLS impl_opts 포함 가능.
 * @return: listening sock 또는 NULL.
 *
 * NVMe-oF TCP TLS 서버 portal 의 경우 opts->impl_opts 에 PSK get_key 콜백 등을
 * 설정해 호출 — 같은 백엔드(ssl)라도 portal 별로 다른 PSK lookup 정책을 가질 수 있다.
 *
 * 실행 컨텍스트: 부팅 또는 RPC.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → backend listen → spdk_sock 생성 → 반환
 */
struct spdk_sock *spdk_sock_listen_ext(const char *ip, int port, const char *impl_name,
				       struct spdk_sock_opts *opts);

/**
 * Accept a new connection from a client on the specified socket and return a
 * socket structure which holds the connection.
 *
 * \param sock Listening socket.
 *
 * \return a pointer to the accepted socket on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_sock_accept - listen sock 에서 대기 중 연결 1개를 수락.
 *
 * @sock: spdk_sock_listen[_ext] 가 반환한 listening sock.
 * @return: 새 연결 sock 또는 NULL(EAGAIN — pending 없음 등).
 *
 * non-blocking 동작 — 대기 연결이 없으면 즉시 NULL(errno=EAGAIN). reactor poll
 * 루프에서 listen sock 을 group 에 add_sock 한 후 사용자 콜백에서 본 함수를
 * 반복 호출해 모든 pending accept 를 드레인하는 패턴이 일반적이다.
 *
 * 실행 컨텍스트: listen sock 이 affinity 된 spdk_thread.
 *
 * 호출 체인:
 *   sock_group_poll → user_cb(listen sock 이벤트) → [이 함수] → backend accept (accept4 syscall)
 */
struct spdk_sock *spdk_sock_accept(struct spdk_sock *sock);

/**
 * Gets the name of the network interface of the local port for the socket.
 *
 * \param sock socket to find the interface name for
 *
 * \return null-terminated string containing interface name if found, NULL
 *	   interface name could not be found
 */
/*
 * [한국어]
 * spdk_sock_get_interface_name - sock 의 로컬 종단이 속한 NIC 이름(예: "eth0") 반환.
 *
 * @sock: 대상 sock(연결 또는 listen).
 * @return: null-terminated NIC 이름 문자열(라이브러리 소유) 또는 NULL.
 *
 * 백엔드는 getsockname → routing table(if_indextoname) 또는 SO_BINDTODEVICE 정보를
 * 조회. NUMA-aware reactor 배치, RPC 출력에 사용.
 *
 * 실행 컨텍스트: 임의 thread, 단 sock 의 affinity 와 충돌 없는 read 이므로 안전.
 */
const char *spdk_sock_get_interface_name(struct spdk_sock *sock);


/**
 * Gets the NUMA node ID for the network interface of the local port for the TCP socket.
 *
 * \param sock TCP socket to find the NUMA socket ID for
 *
 * \return NUMA ID, or SPDK_ENV_NUMA_ID_ANY if the NUMA ID is unknown
 */
/*
 * [한국어]
 * spdk_sock_get_numa_id - sock 의 NIC 이 속한 NUMA 노드 ID 반환.
 *
 * @sock: 대상 sock.
 * @return: NUMA 노드 ID(0..N) 또는 SPDK_ENV_NUMA_ID_ANY(미상).
 *
 * NIC NUMA 와 reactor CPU NUMA 를 정렬해 cross-NUMA 메모리 트래픽을 줄이는데 사용.
 * 내부적으로 /sys/class/net/<ifname>/device/numa_node 를 읽어옴.
 *
 * 실행 컨텍스트: 임의 thread, read-only.
 */
int32_t spdk_sock_get_numa_id(struct spdk_sock *sock);

/**
 * Close a socket.
 *
 * \param sock Socket to close.
 *
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_close - sock 종료 및 자원 해제.
 *
 * @sock: spdk_sock 의 더블 포인터 — 호출 후 *sock 은 NULL 로 클리어된다.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 더블 포인터를 받는 이유는 호출자 변수를 NULL 로 만들어 use-after-free 가능성을
 * 줄이기 위함. close 호출 시 sock 이 group 에 남아 있으면 먼저 group_remove_sock
 * 을 부르거나 라이브러리가 자동 제거한다(백엔드 구현에 따름). zerocopy 통지가
 * 펜딩이면 ack_timeout 동안 대기 후 강제 종료.
 *
 * 실행 컨텍스트: sock 의 affinity spdk_thread.
 *
 * 호출 체인:
 *   transport disconnect → [이 함수] → backend close (close syscall)
 */
int spdk_sock_close(struct spdk_sock **sock);

/**
 * Flush a socket from data gathered in previous writev_async calls.
 *
 * On failure check rc matching -EAGAIN to determine failure is retryable.
 *
 * \param sock Socket to flush.
 *
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_flush - 큐된 비동기 송신 요청을 즉시 sendmsg 로 흘려보냄.
 *
 * @sock: 대상 sock.
 * @return: 0 성공, -EAGAIN 일부 또는 전체가 후속 polling 에서 재시도되어야 함,
 *          기타 음수 errno 비복구 실패.
 *
 * writev_async 호출 직후에는 요청이 큐에만 들어가고 실제 송신은 reactor poll 시점에
 * 발생한다. 호출자가 명시적으로 flush 를 부르면 polling 을 기다리지 않고 즉시
 * sendmsg 를 시도 — coalescing 을 줄여 latency 를 우선시할 때 유용.
 *
 * 실행 컨텍스트: sock 의 affinity spdk_thread.
 *
 * 호출 체인:
 *   상위 transport(예: NVMe-oF TCP, sendq drained signal) → [이 함수] →
 *     backend flush → sendmsg loop
 */
int spdk_sock_flush(struct spdk_sock *sock);

/**
 * Receive a message from the given socket.
 *
 * On failure check rc matching -EAGAIN to determine failure is retryable.
 *
 * \param sock Socket to receive message.
 * \param buf Pointer to a buffer to hold the data.
 * \param len Length of the buffer.
 *
 * \return the length of the received message on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_recv - 단일 버퍼로 수신.
 *
 * @sock: 대상 sock.
 * @buf: 수신 데이터를 채울 버퍼.
 * @len: 버퍼 크기.
 * @return: 양수 = 실제 수신 바이트, 0 = peer EOF, -EAGAIN = 데이터 없음(재시도),
 *          기타 음수 errno = 비복구 실패.
 *
 * non-blocking 동작 — recv pipe 가 활성화되면 사용자공간 파이프에서 먼저 드레인
 * 후 부족분을 syscall 로 채운다. NVMe-oF TCP PDU 헤더 파싱처럼 작은 단위 반복
 * 호출에 유리.
 *
 * 실행 컨텍스트: sock affinity spdk_thread (사용자 콜백 내부).
 *
 * 호출 체인:
 *   sock_group_poll → user_cb → [이 함수] → backend recv (recv/recvmsg syscall 또는 pipe drain)
 */
ssize_t spdk_sock_recv(struct spdk_sock *sock, void *buf, size_t len);

/**
 * Write message to the given socket from the I/O vector array.
 *
 * On failure check rc matching -EAGAIN to determine failure is retryable.
 *
 * \param sock Socket to write to.
 * \param iov I/O vector.
 * \param iovcnt Number of I/O vectors in the array.
 *
 * \return the length of written message on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_writev - 동기 vectored 송신 (writev/sendmsg 직접 호출).
 *
 * @sock: 대상 sock.
 * @iov: iovec 배열.
 * @iovcnt: iovec 개수.
 * @return: 양수 = 송신 바이트, -EAGAIN = 커널 큐 가득(재시도), 기타 음수 errno.
 *
 * 동기 함수 — 큐잉 없이 즉시 sendmsg 호출. 호출자 입장에서 데이터 보내고 끝낼
 * 때 사용. 반환이 부분 송신일 수 있으니 호출자가 루프로 채움 책임.
 *
 * 실행 컨텍스트: sock affinity spdk_thread.
 *
 * 호출 체인:
 *   상위 transport → [이 함수] → backend writev (sendmsg syscall)
 */
ssize_t spdk_sock_writev(struct spdk_sock *sock, struct iovec *iov, int iovcnt);

/**
 * Write data to the given socket asynchronously, calling
 * the provided callback when the data has been written.
 *
 * \param sock Socket to write to.
 * \param req The write request to submit.
 */
/*
 * [한국어]
 * spdk_sock_writev_async - vectored 비동기 송신 — 큐잉 후 즉시 반환, 완료는 cb_fn 으로 통지.
 *
 * @sock: 대상 sock.
 * @req: spdk_sock_request 헤드 + 그 뒤에 iovcnt 개의 iovec 가 연속 배치된 메모리.
 *       cb_fn / cb_arg / iovcnt 는 호출자가 채우고 나머지 internal 은 라이브러리 소유.
 *
 * 큐잉 흐름:
 *   1) req 를 sock 의 송신 큐(TAILQ)에 매단다.
 *   2) 즉시 backend write 를 시도 — 가능한 만큼 sendmsg, 부분 송신은 offset 누적.
 *   3) 일부 또는 전부가 미송신이면 reactor poll 의 다음 라운드에서 재시도.
 *   4) zerocopy 사용 시 sendmsg 성공 후에도 ERRQUEUE 통지를 기다린 뒤 cb_fn 호출.
 *
 * cb_fn 은 sock 의 affinity spdk_thread 컨텍스트에서 호출 — 다른 SPDK 콜백과 동일.
 *
 * 실행 컨텍스트: sock affinity spdk_thread.
 *
 * 호출 체인:
 *   NVMe-oF TCP qpair PDU send → [이 함수] → backend writev_async → reactor poll 시 sendmsg
 */
void spdk_sock_writev_async(struct spdk_sock *sock, struct spdk_sock_request *req);

/**
 * Read message from the given socket to the I/O vector array.
 *
 * On failure check errno matching EAGAIN to determine failure is retryable.
 *
 * \param sock Socket to receive message.
 * \param iov I/O vector.
 * \param iovcnt Number of I/O vectors in the array.
 *
 * \return the length of the received message on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_readv - vectored 동기 수신 (readv/recvmsg 직접 호출).
 *
 * @sock: 대상 sock.
 * @iov: 수신 데이터를 채울 iovec 배열.
 * @iovcnt: iovec 개수.
 * @return: 양수 = 수신 바이트, 0 = EOF, -EAGAIN = 데이터 없음, 기타 음수 errno.
 *
 * spdk_sock_recv 와 달리 여러 분산 버퍼로 한 번에 수신. NVMe-oF TCP PDU 헤더+페이로드
 * 를 별도 버퍼로 받는 데 사용.
 *
 * 실행 컨텍스트: sock affinity spdk_thread.
 *
 * 호출 체인:
 *   사용자 콜백 → [이 함수] → backend readv (recvmsg syscall)
 */
ssize_t spdk_sock_readv(struct spdk_sock *sock, struct iovec *iov, int iovcnt);

/**
 * Receive the next portion of the stream from the socket.
 *
 * A buffer provided to this socket's group's pool using
 * spdk_sock_group_provide_buf() will contain the data and be
 * returned in *buf.
 *
 * Note that the amount of data in buf is determined entirely by
 * the sock layer. You cannot request to receive only a limited
 * amount here. You simply get whatever the next portion of the stream
 * is, as determined by the sock module. You can place an upper limit
 * on the size of the buffer since these buffers are originally
 * provided to the group through spdk_sock_group_provide_buf().
 *
 * This code path will only work if the recvbuf is disabled. To disable
 * the recvbuf, call spdk_sock_set_recvbuf with a size of 0.
 *
 * On failure check rc matching -EAGAIN to determine failure is retryable.
 *
 * \param sock Socket to receive from.
 * \param buf Populated with the next portion of the stream
 * \param ctx Returned context pointer from when the buffer was provided.
 *
 * \return On success, the length of the buffer placed into buf, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_recv_next - sock_group 에 미리 등록된 풀 버퍼를 디큐해 다음 스트림 조각 반환.
 *
 * @sock: 대상 sock — 반드시 group 에 add 되어 있어야 하며 recvbuf=0 (사용자공간 pipe 비활성).
 * @buf: 데이터가 채워진 풀 버퍼 포인터를 받을 위치(in/out).
 * @ctx: spdk_sock_group_provide_buf 호출 시 등록한 컨텍스트가 그대로 반환됨.
 * @return: 양수 = buf 에 들어간 바이트 수, -EAGAIN = 데이터 없음, 기타 음수 errno.
 *
 * "buffer-providing" 모델 — 호출자가 풀에 미리 큰 버퍼들을 등록해 두면 sock 레이어가
 * 다음 패킷이 왔을 때 그 중 하나를 채워 넘겨준다. 사용자가 수신 크기를 지정할 수
 * 없으며 그 라운드에 도착한 양만큼 받는다. 일반 readv 보다 zerocopy 와 더 친화적이고
 * NVMe-oF target 의 RX path 에서 사용 가능. 이 경로를 쓰려면 spdk_sock_set_recvbuf(0)
 * 으로 사용자공간 recv pipe 를 끄고 spdk_sock_group_provide_buf 로 풀을 채워야 한다.
 *
 * 실행 컨텍스트: sock affinity spdk_thread (사용자 콜백 내부).
 */
int spdk_sock_recv_next(struct spdk_sock *sock, void **buf, void **ctx);

/**
 * Set the value used to specify the low water mark (in bytes) for this socket.
 *
 * \param sock Socket to set for.
 * \param nbytes Value for recvlowat.
 *
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_set_recvlowat - SO_RCVLOWAT setsockopt — 최소 수신 임계 설정.
 *
 * @sock: 대상 sock.
 * @nbytes: epoll readable 알림이 떨어지기 위한 최소 수신 바이트 수.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 작은 PDU 헤더(예: 8B) 단위로만 알림을 받고 싶을 때 헤더 크기로 설정.
 * 큰 값을 쓰면 readable 알림 빈도가 줄어 syscall 횟수 감소.
 *
 * 실행 컨텍스트: sock affinity spdk_thread.
 */
int spdk_sock_set_recvlowat(struct spdk_sock *sock, int nbytes);

/**
 * Set receive buffer size for the given socket.
 *
 * \param sock Socket to set buffer size for.
 * \param sz Buffer size in bytes.
 *
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_set_recvbuf - per-sock RX 버퍼 크기 설정 (SO_RCVBUF + 사용자공간 pipe 크기).
 *
 * @sock: 대상 sock.
 * @sz: 바이트 단위 크기. 0 이면 사용자공간 pipe 비활성화 — recv_next 모드 진입에 필요.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 백엔드는 SO_RCVBUF 와 내부 spdk_pipe 둘 다 영향을 받게 처리. 사용자가 큰 페이로드를
 * 한 번에 받으려면 충분히 크게 설정.
 */
int spdk_sock_set_recvbuf(struct spdk_sock *sock, int sz);

/**
 * Set send buffer size for the given socket.
 *
 * \param sock Socket to set buffer size for.
 * \param sz Buffer size in bytes.
 *
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_set_sendbuf - per-sock TX 버퍼 크기 설정 (SO_SNDBUF).
 *
 * @sock: 대상 sock.
 * @sz: 바이트 단위 크기.
 * @return: 0 성공, 음수 errno 실패.
 *
 * NVMe-oF TCP 의 큰 read response (예: 1MB) 가 부분 송신을 줄이기 위해 충분히 크게.
 */
int spdk_sock_set_sendbuf(struct spdk_sock *sock, int sz);

/**
 * Check whether the address of socket is ipv6.
 *
 * This function is allowed only when connection is established.
 *
 * \param sock Socket to check.
 *
 * \return true if the address of socket is ipv6, or false otherwise.
 */
/*
 * [한국어]
 * spdk_sock_is_ipv6 - sock 의 로컬 주소가 IPv6 인지.
 *
 * @sock: 연결된 sock.
 * @return: true=IPv6, false=IPv4 또는 미연결.
 *
 * 백엔드는 getsockname 의 sa_family == AF_INET6 검사. 로깅·RPC·정책 분기에 사용.
 */
bool spdk_sock_is_ipv6(struct spdk_sock *sock);

/**
 * Check whether the address of socket is ipv4.
 *
 * This function is allowed only when connection is established.
 *
 * \param sock Socket to check.
 *
 * \return true if the address of socket is ipv4, or false otherwise.
 */
/*
 * [한국어]
 * spdk_sock_is_ipv4 - sock 의 로컬 주소가 IPv4 인지.
 *
 * @sock: 연결된 sock.
 * @return: true=IPv4, false=IPv6 또는 미연결.
 *
 * is_ipv6 와 대칭. AF_INET 검사. */
bool spdk_sock_is_ipv4(struct spdk_sock *sock);

/**
 * Check whether the socket is currently connected.
 *
 * \param sock Socket to check
 *
 * \return true if the socket is connected or false otherwise.
 */
/*
 * [한국어]
 * spdk_sock_is_connected - sock 의 연결 상태 확인.
 *
 * @sock: 대상 sock.
 * @return: true=ESTABLISHED, false=진행중 또는 끊김.
 *
 * connect_async 호출 후 cb_fn 도착 전 polling 모드로 상태를 확인할 때 사용 가능.
 * 백엔드는 getsockopt SO_ERROR / TCP_INFO tcpi_state 검사 또는 자체 상태 머신 사용.
 */
bool spdk_sock_is_connected(struct spdk_sock *sock);

/**
 * Callback function for spdk_sock_group_add_sock().
 *
 * \param arg Argument for the callback function.
 * \param group Socket group.
 * \param sock Socket.
 */
/*
 * [한국어]
 * typedef spdk_sock_cb - sock_group 의 sock 이벤트(주로 readable) 콜백 시그니처.
 *
 * @arg: add_sock 시 등록한 컨텍스트.
 * @group: 이벤트가 발생한 group.
 * @sock: 이벤트가 발생한 sock — 호출자는 본 sock 에 대해 readv/recv/recv_next 등을
 *        수행해 데이터를 드레인.
 *
 * 콜백은 group_poll 내부에서 한 번의 epoll_wait/io_uring_peek 결과를 순회하며 호출.
 * 같은 sock 에 대해 동일 라운드에서 여러 번 호출되지 않으며, 콜백 내부에서 더
 * 읽을 데이터가 있더라도 원하는 만큼 드레인 후 반환하면 다음 라운드에서 다시 호출.
 *
 * 실행 컨텍스트: group 이 등록된 spdk_thread (lockless 보장).
 */
typedef void (*spdk_sock_cb)(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock);

/**
 * Create a new socket group with user provided pointer
 *
 * \param ctx the context provided by user.
 * \return a pointer to the created group on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_sock_group_create - 새 sock_group 생성 — 한 spdk_thread 에서 다수 sock 을 통합 폴링.
 *
 * @ctx: 사용자 컨텍스트 — group 에 부착되어 group_get_ctx 로 다시 꺼낼 수 있음.
 * @return: 새 group 포인터 또는 NULL(메모리 부족 등).
 *
 * 백엔드는 epoll_create1(posix), io_uring_setup(uring) 등으로 폴링 fd 생성. group 은
 * 생성된 spdk_thread 에 affinity 가 자동으로 잡힌다 — 다른 thread 에서 사용하면
 * 안 됨. NVMe-oF target qpair 한 개당 group 한 개를 만드는 것이 일반적.
 *
 * 실행 컨텍스트: group 이 사용될 spdk_thread 에서 호출.
 *
 * 호출 체인:
 *   transport poll group create → [이 함수] → backend group_impl create (epoll_create 등)
 */
struct spdk_sock_group *spdk_sock_group_create(void *ctx);

/**
 * Get the ctx of the sock group
 *
 * \param sock_group Socket group.
 * \return a pointer which is ctx of the sock_group.
 */
/*
 * [한국어]
 * spdk_sock_group_get_ctx - group 생성 시 등록한 ctx 포인터 회수.
 *
 * @sock_group: 대상 group.
 * @return: 등록 ctx 포인터.
 *
 * sock_cb 내부에서 group 으로부터 상위 transport 컨텍스트(예: NVMe-oF poll group)
 * 를 꺼낼 때 사용. lockless read.
 */
void *spdk_sock_group_get_ctx(struct spdk_sock_group *sock_group);


/**
 * Add a socket to the group.
 *
 * \param group Socket group.
 * \param sock Socket to add.
 * \param cb_fn Called when the operation completes.
 * \param cb_arg Argument passed to the callback function.
 *
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_group_add_sock - sock 을 group 의 폴링 대상에 등록.
 *
 * @group: 대상 group.
 * @sock: 등록할 sock — 다른 group 에 이미 등록되어 있으면 -EEXIST 등 실패.
 * @cb_fn: sock 이벤트(주로 readable) 시 호출할 콜백.
 * @cb_arg: cb_fn 첫 인자.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 백엔드는 epoll_ctl(EPOLL_CTL_ADD, EPOLLIN | EPOLLET) 또는 io_uring poll_add 로 등록.
 * 등록된 후 sock 의 affinity 는 group 의 spdk_thread 로 고정되며, 다른 thread 에서의
 * I/O 는 lockless 가정을 깨므로 금지.
 *
 * 실행 컨텍스트: group 의 affinity spdk_thread.
 *
 * 호출 체인:
 *   accept handler → [이 함수] → backend group_impl_add_sock (epoll_ctl/io_uring)
 */
int spdk_sock_group_add_sock(struct spdk_sock_group *group, struct spdk_sock *sock,
			     spdk_sock_cb cb_fn, void *cb_arg);

/**
 * Remove a socket from the group.
 *
 * \param group Socket group.
 * \param sock Socket to remove.
 *
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_group_remove_sock - group 에서 sock 등록 해제.
 *
 * @group: 대상 group.
 * @sock: 제거할 sock.
 * @return: 0 성공, 음수 errno 실패.
 *
 * close 또는 reactor 마이그레이션(다른 group 으로 옮길 때) 호출. 백엔드는
 * epoll_ctl(EPOLL_CTL_DEL) 등으로 폴링 등록 해제. 본 호출 후 sock 의 affinity 는
 * 해제되어 다른 thread 의 group 에 add 가능.
 *
 * 실행 컨텍스트: group 의 affinity spdk_thread.
 */
int spdk_sock_group_remove_sock(struct spdk_sock_group *group, struct spdk_sock *sock);

/**
 * Provides a buffer to the group to be used in its receive pool.
 * See spdk_sock_recv_next() for more details.
 *
 * \param group Socket group.
 * \param buf Pointer the buffer provided.
 * \param len Length of the buffer.
 * \param ctx Pointer that will be returned in spdk_sock_recv_next()
 *
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_group_provide_buf - group 의 RX 풀에 버퍼 1개 등록 (recv_next 용).
 *
 * @group: 대상 group.
 * @buf: 풀에 넣을 버퍼 포인터(호출자 소유, 라이브러리는 보관만).
 * @len: 버퍼 크기.
 * @ctx: recv_next 가 데이터를 채워 반환할 때 함께 돌려줄 컨텍스트.
 * @return: 0 성공, 음수 errno 실패.
 *
 * recv_next 모델의 핵심 — 호출자가 미리 충분한 큰 버퍼를 풀에 등록하면 sock 레이어가
 * 그 버퍼에 직접 수신해 ctx 와 함께 반환. 사용자공간 pipe 우회와 zerocopy 친화.
 *
 * 실행 컨텍스트: group 의 affinity spdk_thread.
 */
int spdk_sock_group_provide_buf(struct spdk_sock_group *group, void *buf, size_t len, void *ctx);

/**
 * Poll incoming events for each registered socket.
 *
 * \param group Group to poll.
 *
 * \return the number of events on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_group_poll - group 의 모든 sock 에 대한 이벤트를 1회 폴링.
 *
 * @group: 대상 group.
 * @return: 처리한 이벤트 수, 음수 errno 실패.
 *
 * 무한 폴링 루프(spdk_poller)에서 매 라운드 호출. 백엔드 epoll_wait(timeout=0) 또는
 * io_uring_peek_batch 로 즉시 반환되는 이벤트를 모두 가져와 sock 별 cb_fn 을 호출.
 * 인터럽트 모드에서는 fd readable 시점에만 호출.
 *
 * 실행 컨텍스트: group 의 affinity spdk_thread (reactor poll).
 *
 * 호출 체인:
 *   spdk_poller fn → [이 함수] → backend group_impl_poll → epoll_wait → cb_fn 호출
 */
int spdk_sock_group_poll(struct spdk_sock_group *group);

/**
 * Poll incoming events up to max_events for each registered socket.
 *
 * \param group Group to poll.
 * \param max_events Number of maximum events to poll for each socket.
 *
 * \return the number of events on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_group_poll_count - group_poll 의 이벤트 수 상한 지정 버전.
 *
 * @group: 대상 group.
 * @max_events: 한 번 호출에서 sock 당 처리할 이벤트 최대 개수.
 * @return: 처리한 이벤트 수, 음수 errno 실패.
 *
 * starvation 방지 — 한 sock 이 너무 많은 이벤트를 가져 다른 reactor 작업이 굶주리지
 * 않도록 상한을 지정한다. NVMe-oF target 의 fairness tuning 에 사용.
 */
int spdk_sock_group_poll_count(struct spdk_sock_group *group, int max_events);

/**
 * Close all registered sockets of the group and then remove the group.
 *
 * If any sockets were added to the group by \ref spdk_sock_group_add_sock
 * these must be removed first by using \ref spdk_sock_group_remove_sock.
 *
 * \param group Group to close.
 *
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_group_close - group 자체를 종료.
 *
 * @group: spdk_sock_group 더블 포인터 — 호출 후 *group 은 NULL.
 * @return: 0 성공, 음수 errno 실패(EBUSY = 아직 sock 이 남아 있음 등).
 *
 * 호출 전 모든 sock 이 group_remove_sock 으로 해제되어 있어야 한다 — 라이브러리에
 * 따라 자동 해제하기도 하지만 안전하게 호출자가 명시적으로 비우는 것이 권장.
 * 내부 epoll fd / io_uring SQ 도 함께 close.
 *
 * 실행 컨텍스트: group 의 affinity spdk_thread.
 */
int spdk_sock_group_close(struct spdk_sock_group **group);

/**
 * Get the optimal sock group for this sock.
 *
 * This function is allowed only when connection is established.
 *
 * \param sock The socket
 * \param group Returns the optimal sock group. If there is no optimal sock group, returns NULL.
 * \param hint When return is 0 and group is set to NULL, hint is used to set optimal sock group for the socket.
 *
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_get_optimal_sock_group - placement 정책에 따른 최적 group 검색.
 *
 * @sock: 연결된 sock.
 * @group: out 파라미터 — 최적 group 포인터를 채움. 매핑이 없으면 NULL.
 * @hint: out 이 NULL 인 경우 이 hint 를 새 매핑으로 등록(첫 sock 이 들어오는 경우).
 * @return: 0 성공, 음수 errno 실패.
 *
 * placement_id (NAPI/CPU/MARK) 기반 socket cluster 검색 — 같은 RX queue/CPU/mark 를
 * 공유하는 sock 들을 같은 reactor 의 group 에 모아 cache locality 와 lockless 보장을
 * 극대화. NVMe-oF target 의 connection-to-poll-group 분배에서 사용.
 *
 * 실행 컨텍스트: 임의 spdk_thread (cluster map 은 전역 hash 또는 atomic 으로 보호).
 *
 * 호출 체인:
 *   transport accept → [이 함수] → cluster map 조회 → group 반환
 */
int spdk_sock_get_optimal_sock_group(struct spdk_sock *sock, struct spdk_sock_group **group,
				     struct spdk_sock_group *hint);

/**
 * Get current socket implementation options.
 *
 * \param impl_name The socket implementation to use, such as "posix".
 * \param opts Pointer to allocated spdk_sock_impl_opts structure that will be filled with actual values.
 * \param len On input specifies size of passed opts structure. On return it is set to actual size that was filled with values.
 *
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_impl_get_opts - 백엔드 단위의 현재 옵션 조회.
 *
 * @impl_name: 백엔드 이름("posix"/"uring"/"ssl").
 * @opts: 호출자가 할당한 spdk_sock_impl_opts — 채워진 후 호출자가 검사.
 * @len: in/out — 입력 시 opts 의 크기, 출력 시 라이브러리가 실제로 채운 크기.
 *       ABI forward-compat 패턴: 작은 호출자는 자기 크기까지만 받고 그 이상은 무시.
 * @return: 0 성공, 음수 errno 실패(ENOENT = 알 수 없는 백엔드 등).
 *
 * RPC sock_impl_get_options 핸들러 또는 디버그 유틸에서 사용.
 *
 * 실행 컨텍스트: RPC thread 또는 사용자 thread.
 */
int spdk_sock_impl_get_opts(const char *impl_name, struct spdk_sock_impl_opts *opts, size_t *len);

/**
 * Set socket implementation options.
 *
 * \param impl_name The socket implementation to use, such as "posix".
 * \param opts Pointer to allocated spdk_sock_impl_opts structure with new options values.
 * \param len Size of passed opts structure.
 *
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_impl_set_opts - 백엔드 단위 전역 옵션 변경.
 *
 * @impl_name: 백엔드 이름.
 * @opts: 새 옵션 값 — 라이브러리 내부 복제.
 * @len: opts 크기 — 호출자가 컴파일된 시점의 sizeof, 라이브러리는 그 크기까지만 적용.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 호출 후 새로 생성되는 sock 부터 옵션이 적용된다 — 이미 만들어진 sock 의
 * 동작은 보통 영향 없음(zerocopy_threshold 같은 일부 옵션은 즉시 영향).
 *
 * 실행 컨텍스트: 부팅 또는 RPC thread.
 *
 * 호출 체인:
 *   RPC sock_impl_set_options → [이 함수] → backend 가 자신의 글로벌 opts 구조체 갱신
 */
int spdk_sock_impl_set_opts(const char *impl_name, const struct spdk_sock_impl_opts *opts,
			    size_t len);

/**
 * Set the given sock implementation to be used as the default one.
 *
 * Note: passing a specific sock implementation name in some sock API functions
 * (such as @ref spdk_sock_connect, @ref spdk_sock_listen and etc) ignores the default value set by this function.
 *
 * \param impl_name The socket implementation to use, such as "posix".
 * \return 0 on success, negative errno value on failure.
 */
/*
 * [한국어]
 * spdk_sock_set_default_impl - 기본 백엔드 지정 — connect/listen 시 impl_name=NULL 이면 사용.
 *
 * @impl_name: 백엔드 이름.
 * @return: 0 성공, 음수 errno 실패(ENOENT 등).
 *
 * 백엔드 선택 우선순위:
 *   1) connect/listen 의 impl_name 인자(non-NULL).
 *   2) 본 함수로 지정된 default.
 *   3) 빌드 시점의 첫 enabled 백엔드.
 *
 * 실행 컨텍스트: 부팅 또는 RPC thread. 동기화는 set 시점만 보호되면 됨.
 */
int spdk_sock_set_default_impl(const char *impl_name);

/**
 * Get the name of the current default implementation
 *
 * \return The name of the default implementation
 */
/*
 * [한국어]
 * spdk_sock_get_default_impl - 현재 default 백엔드 이름 반환.
 *
 * @return: 정적 문자열(라이브러리 소유). 호출자 free 금지.
 *
 * RPC 출력, 디버그 로그용.
 */
const char *spdk_sock_get_default_impl(void);

/**
 * Write socket subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 */
/*
 * [한국어]
 * spdk_sock_write_config_json - sock subsystem 의 현재 설정을 JSON 으로 출력.
 *
 * @w: spdk_json_write_ctx — RPC subsystem.save_config 에서 사용.
 * @return: void.
 *
 * 모든 백엔드의 impl_opts 를 순회하며 RPC 구조의 JSON 객체로 직렬화. 결과는
 * spdk_subsystem_config_json 의 sock subsystem 항목에 들어간다 — 부팅 시
 * --json-config 옵션으로 동일 설정 재현 가능.
 *
 * 실행 컨텍스트: RPC thread.
 *
 * 호출 체인:
 *   RPC subsystem_save_config → sock subsystem write_config_json →
 *     [이 함수] → backend 별 write_config_json
 */
void spdk_sock_write_config_json(struct spdk_json_write_ctx *w);

/**
 * Obtain an fd that becomes ready when one of the sockets in this group is ready. This fd
 * is suitable for use in APIs such as spdk_interrupt_register_for_events().
 *
 *
 * \param group Socket group.
 *
 * \return fd (>0) on success or negated errno on failure.
 */
/*
 * [한국어]
 * spdk_sock_group_get_interruptfd - group 의 통합 폴링 fd 반환 — 인터럽트 모드 등록용.
 *
 * @group: 대상 group.
 * @return: 양수 fd 성공, 음수 errno 실패.
 *
 * 폴드모드(busy-loop)가 아닌 인터럽트 모드(spdk_interrupt_register_for_events)에서
 * group 자체를 reactor 의 인터럽트 소스로 등록할 때 사용. 백엔드의 epoll fd
 * (posix) 또는 io_uring eventfd 를 그대로 노출 — sock 에 데이터가 올 때 readable
 * 상태가 되어 reactor 가 깨어난다.
 *
 * 실행 컨텍스트: 인터럽트 모드 초기화 thread.
 *
 * 호출 체인:
 *   spdk_app_start (interrupt mode) → [이 함수] → spdk_interrupt_register_for_events
 */
int spdk_sock_group_get_interruptfd(struct spdk_sock_group *group);

/*
 * [한국어] extern "C" 블록 종료 — C++ 사용자 호환을 위한 가드 닫음.
 */
#ifdef __cplusplus
}
#endif

/* [한국어] include guard 종료. SPDK_SOCK_H 가드의 닫음이며 이 헤더 끝. */
#endif /* SPDK_SOCK_H */
