/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK RPC framework 본체 (rpc.c)
 *
 * === 파일의 역할 ===
 * SPDK는 운영자/오케스트레이션이 동적으로 디바이스를 만들고/지우고/조회할 수
 * 있도록 JSON-RPC 2.0 인터페이스를 제공한다. 그 인터페이스는 두 레이어로 나뉜다.
 *   - lib/jsonrpc/: 와이어 프로토콜 처리 (소켓 read/write, JSON 파싱, request/
 *                   response 객체 lifecycle).
 *   - lib/rpc/ (본 파일): 메서드 registry — "method 이름 → handler 함수" 매핑,
 *                   서버 listen 시작, 상태 게이팅(STARTUP vs RUNTIME), allowlist,
 *                   alias/deprecation 처리.
 * 본 파일은 jsonrpc layer 위의 얇은 dispatcher다 — wire I/O는 직접 안 한다.
 * jsonrpc layer가 새 요청을 만들어 본 파일의 jsonrpc_handler를 콜백으로
 * 호출하면, 본 파일이 g_rpc_methods SLIST에서 method 이름을 찾아 실제 handler
 * (예: bdev_get_bdevs RPC)로 dispatch한다. 또한 SPDK_RPC_REGISTER 매크로의
 * constructor가 main() 전에 본 파일의 spdk_rpc_register_method를 호출하여
 * 모든 RPC method를 자동 등록하는 메커니즘의 등록부 보관처도 본 파일이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 클라이언트 → AF_UNIX 소켓 → lib/sock → lib/jsonrpc (parser/wire) →
 * jsonrpc_handler(본 파일) → method registry lookup → 등록된 handler 함수
 * (lib/bdev/, lib/nvmf/, lib/keyring/ 등 각 서브시스템이 SPDK_RPC_REGISTER로 등록)
 * → 응답 빌더 → spdk_jsonrpc_end_result → wire write.
 *
 * 상태 게이팅:
 *   - STARTUP: spdk_app_start 직후 framework_start_init RPC 이전. config 변경
 *     성격의 RPC만 허용 (예: bdev_set_options).
 *   - RUNTIME: framework_start_init 이후. I/O 성격 RPC 허용 (예: bdev_get_iostat).
 *   - 일부 RPC는 두 상태 모두에서 호출 가능 (rpc_get_methods, spdk_get_version).
 *
 * 실행 컨텍스트: spdk_rpc_server_accept를 호출하는 reactor의 thread (보통 master
 *   reactor의 RPC poller). 모든 RPC handler는 이 thread context에서 실행되므로
 *   다른 reactor의 자원을 만질 때는 spdk_thread_send_msg로 cross-thread 위임 필수.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - lib/jsonrpc: spdk_jsonrpc_server_listen/_poll/_shutdown, send_error_response.
 *   - lib/sock(via AF_UNIX): UDS listen 소켓.
 *   - sys/file.h: flock으로 lock 파일 보호 — 같은 socket path를 다른 SPDK
 *     인스턴스가 점유하지 못하도록 함.
 *   - spdk/version.h: spdk_get_version RPC가 노출하는 빌드 정보.
 * 의존됨: lib/event/(spdk_app_start이 본 파일의 server_listen 호출), 각
 *   서브시스템의 *_rpc.c (SPDK_RPC_REGISTER로 method 등록).
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_rpc_server: UDS 소켓 + lock_fd + jsonrpc_server 포함.
 *   - struct spdk_rpc_method: method 1개의 등록 정보 (이름, handler, state mask,
 *     alias 체인, deprecated 플래그).
 *   - g_rpc_state: 현재 SPDK 상태(STARTUP/RUNTIME) — 게이팅 기준.
 *   - g_rpcs_correct: 중복/잘못된 alias 등록을 발견하면 false — 시작 시 spdk_app
 *     이 verify_methods로 검증.
 *   - g_rpcs_allowlist: 운영자가 활성화한 method만 노출하는 보안 필터.
 *   - jsonrpc_handler: jsonrpc layer의 wire 콜백 — registry lookup + state mask
 *     체크 + handler 디스패치.
 *   - spdk_rpc_server_listen/_accept/_close: UDS 소켓 lifecycle.
 *   - spdk_rpc_register_method/_register_alias_deprecated: 등록 backend (매크로
 *     constructor에서 호출됨).
 *   - rpc_rpc_get_methods, rpc_spdk_get_version: 본 파일이 정의하는 메타 RPC 2종.
 */

#include <sys/file.h>           /* [한국어] flock(2) — 소켓 path lock 파일에 사용 */

#include "spdk/stdinc.h"        /* [한국어] 표준 C 헤더 묶음 (string.h, stdlib.h, errno.h 등) */

#include "spdk/queue.h"         /* [한국어] SLIST_HEAD/INSERT/FOREACH (method registry 자료구조) */
#include "spdk/rpc.h"           /* [한국어] 공개 API + SPDK_RPC_REGISTER 매크로 */
#include "spdk/env.h"           /* [한국어] DPDK 추상화 (간접 사용) */
#include "spdk/log.h"           /* [한국어] SPDK_ERRLOG/WARNLOG */
#include "spdk/string.h"        /* [한국어] spdk_strerror, spdk_strarray_dup/free */
#include "spdk/util.h"          /* [한국어] SPDK_COUNTOF — decoder 배열 크기 계산 */
#include "spdk/version.h"       /* [한국어] SPDK_VERSION_STRING 등 빌드 메타 — spdk_get_version에서 사용 */

/* [한국어] 현재 SPDK 상태. 초기값 STARTUP — framework_start_init RPC가 들어오면
 * spdk_rpc_set_state(RUNTIME)으로 전환. 본 변수는 jsonrpc_handler가 메서드의
 * state_mask와 비교해 "이 상태에서 호출 가능한가" 판단하는 기준이 된다. */
static uint32_t g_rpc_state = SPDK_RPC_STARTUP;

/* [한국어] register 시 발견된 모든 등록 오류(중복 method, 존재하지 않는 method의
 * alias 등)를 누적 추적. spdk_rpc_verify_methods가 이를 반환하여 시작 시점에
 * 운영자에게 잘못된 빌드를 알린다. */
static bool g_rpcs_correct = true;

/* [한국어] 운영자가 enable한 method 이름 배열 (NULL-terminated). NULL이면
 * 전체 허용. 보안: 컨테이너/proxy 환경에서 위험 RPC를 비활성화하는 용도. */
static char **g_rpcs_allowlist = NULL;

/* [한국어] RPC 서버 인스턴스. 보통 1개지만 spdk_rpc_server_listen으로 여러
 * 소켓을 만들 수 있는 구조 — 각각 독립 lifecycle. */
struct spdk_rpc_server {
	struct sockaddr_un listen_addr_unix;
	/* [한국어] AF_UNIX 소켓 주소. sun_path에 socket 파일 경로 보관.
	 * 첫 바이트가 '\0'이면 "이미 닫음/미초기화" sentinel로 사용 (server_close 참고). */

	char lock_path[sizeof(((struct sockaddr_un *)0)->sun_path) + sizeof(".lock")];
	/* [한국어] socket path + ".lock" 확장. 같은 socket path에 두 인스턴스가
	 * bind하지 못하도록 lock 파일을 별도로 둠 (advisory flock). */

	int lock_fd;
	/* [한국어] lock 파일 descriptor. -1이면 "닫혔거나 미오픈".
	 * flock(LOCK_EX|LOCK_NB)으로 점유 검증 후 보관. */

	struct spdk_jsonrpc_server *jsonrpc_server;
	/* [한국어] lib/jsonrpc가 관리하는 wire 서버 핸들. 본 파일은 listen/poll/
	 *   shutdown 호출만 하고 직접 build/parse는 하지 않는다. */
};

/* [한국어] 등록된 RPC method 1개의 메타데이터. SLIST에 link되어 g_rpc_methods에 보관. */
struct spdk_rpc_method {
	const char *name;
	/* [한국어] method 이름. strdup된 사본 — register 시 호출자 리터럴이 사라져도 안전.
	 * 비교: 단순 strcmp (현재 hash table 미적용 — TODO 주석 참고). */

	spdk_rpc_method_handler func;
	/* [한국어] 실제 method 처리 함수. signature: (request, params).
	 * 호출 컨텍스트: RPC poller thread. 응답은 spdk_jsonrpc_*로 빌드. */

	SLIST_ENTRY(spdk_rpc_method) slist;
	/* [한국어] g_rpc_methods SLIST 링크. 단방향이므로 INSERT_HEAD가 O(1).
	 * 보호: 등록은 main() 전 constructor 시점이라 single-thread → 락 불필요. */

	uint32_t state_mask;
	/* [한국어] 호출 가능한 상태 비트마스크. SPDK_RPC_STARTUP/RUNTIME 비트 조합.
	 * jsonrpc_handler가 (m->state_mask & g_rpc_state) == g_rpc_state로 검증. */

	bool is_deprecated;
	/* [한국어] alias이면서 deprecated인 경우 true. 첫 호출 시 WARNLOG 1회. */

	struct spdk_rpc_method *is_alias_of;
	/* [한국어] alias entry이면 원본 method 포인터. 일반 등록은 NULL.
	 * jsonrpc_handler가 alias→원본으로 redirect (이중 alias 금지). */

	bool deprecation_warning_printed;
	/* [한국어] deprecation 경고를 이미 한 번 출력했는지. spam 방지. */
};

/* [한국어] 전역 method registry — 단방향 linked list. constructor들이 main() 전에
 * SLIST_INSERT_HEAD로 채우므로 첫 호출 시 이미 가득 차 있다. 락 불필요 (이후
 * register는 거의 발생하지 않음 — load-once 가정). */
static SLIST_HEAD(, spdk_rpc_method) g_rpc_methods = SLIST_HEAD_INITIALIZER(g_rpc_methods);

/*
 * [한국어]
 * spdk_rpc_set_state - 현재 RPC framework 상태(STARTUP/RUNTIME)를 전환
 *
 * @state: SPDK_RPC_STARTUP 또는 SPDK_RPC_RUNTIME.
 *
 * 호출 시점: framework_start_init RPC handler가 init 완료 후 RUNTIME으로 전환.
 *   spdk_app_stop 시 다시 STARTUP으로 돌릴 수도 있음 (graceful shutdown).
 *
 * 동시성: 단일 RPC poller thread에서만 호출되므로 락 없음. 다른 thread에서
 *   호출하면 race 위험 — 일반적이지 않은 사용법.
 */
void
spdk_rpc_set_state(uint32_t state)
{
	g_rpc_state = state;
}

/*
 * [한국어]
 * spdk_rpc_get_state - 현재 상태 비트 반환
 *
 * 사용처: RPC handler가 자기 동작을 상태에 따라 분기할 때 (예: subsystem RPC).
 */
uint32_t
spdk_rpc_get_state(void)
{
	return g_rpc_state;
}

/*
 * [한국어]
 * rpc_is_allowed - method가 운영자 allowlist에 포함되는지 검사
 *
 * @name: method 이름.
 * @return: allowlist가 NULL이면 모든 method 허용(true). NULL이 아니면 정확히
 *          일치하는 항목이 있을 때만 true.
 *
 * 사용 맥락: 컨테이너/proxy 환경에서 보안상 위험한 RPC(예: bdev_nvme_attach_
 *   controller)를 비활성화하고 read-only RPC만 노출하고 싶을 때. 운영자는
 *   command-line argument 또는 spdk_rpc_set_allowlist로 설정.
 *
 * 시간 복잡도: O(N) — allowlist 크기. 빈도가 낮은 register/lookup 경로에서만
 *   사용되므로 hash 도입 불필요.
 */
static bool
rpc_is_allowed(const char *name)
{
	size_t i;

	if (g_rpcs_allowlist == NULL) {       /* [한국어] 미설정 = 전체 허용 (default open) */
		return true;
	}

	for (i = 0; g_rpcs_allowlist[i] != NULL; i++) {   /* [한국어] NULL-terminated 배열 순회 */
		if (strcmp(name, g_rpcs_allowlist[i]) == 0) {
			return true;
		}
	}

	return false;     /* [한국어] 미일치 — 호출 불가 */
}


/*
 * [한국어]
 * _get_rpc_method - JSON value로 표현된 method 이름으로 registry 검색 + allowlist 적용
 *
 * @method: JSON-RPC 요청에 들어 있는 method 필드 (string 토큰).
 * @return: 등록된 method (allowlist 통과) 또는 NULL.
 *
 * 호출 체인: jsonrpc_handler → [본 함수] (wire 측 lookup).
 */
static struct spdk_rpc_method *
_get_rpc_method(const struct spdk_json_val *method)
{
	struct spdk_rpc_method *m;

	/* [한국어] SLIST 선형 검색. method가 보통 수십~수백개라 hash 없이도 사람의
	 * 인지 가능 시간 내 처리 가능. 그러나 TODO 주석에 "use hash"가 명시됨. */
	SLIST_FOREACH(m, &g_rpc_methods, slist) {
		if (spdk_json_strequal(method, m->name)) {       /* [한국어] JSON token vs C string 비교 */
			if (!rpc_is_allowed(m->name)) {
				return NULL;     /* [한국어] 등록은 됐지만 allowlist 차단 → "unknown method"처럼 응답 */
			}
			return m;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * _get_rpc_method_raw - C 문자열 이름으로 registry 검색 (내부 사용)
 *
 * 동작: 임시 spdk_json_val을 하나 만들어 _get_rpc_method를 재사용.
 * 사용처: spdk_rpc_register_method/_alias_deprecated에서 등록 직전 중복 검사,
 *   spdk_rpc_is_method_allowed에서 외부 조회.
 */
static struct spdk_rpc_method *
_get_rpc_method_raw(const char *method)
{
	struct spdk_json_val method_val;

	method_val.type = SPDK_JSON_VAL_STRING;       /* [한국어] JSON-RPC method는 string 타입 */
	method_val.len = strlen(method);
	method_val.start = (char *)method;            /* [한국어] const 캐스팅 — start는 read-only로만 사용됨 */

	return _get_rpc_method(&method_val);
}

/*
 * [한국어]
 * jsonrpc_handler - jsonrpc layer가 새 wire 요청을 만들 때 호출하는 dispatch 콜백
 *
 * @request: jsonrpc layer가 만든 요청 객체 (응답 빌더 + id 매칭 정보 포함).
 * @method:  요청 JSON의 method 필드. NULL 불가 (assert).
 * @params:  요청 JSON의 params 필드. NULL 가능 (파라미터 없는 메서드).
 *
 * 동작:
 *   1) method 이름으로 registry lookup (+ allowlist 검증).
 *      미등록/차단 → JSONRPC_ERROR_METHOD_NOT_FOUND (-32601) 응답.
 *   2) alias 처리 — alias entry이면 원본 method로 리다이렉트하고 deprecated인
 *      경우 첫 1회 WARNLOG.
 *   3) state mask 검증 — 현재 g_rpc_state가 method가 허용한 상태에 포함되는지.
 *      미허용이면 INVALID_STATE 에러 응답 (사용자에게 "framework_start_init
 *      RPC 전/후" 어느 쪽인지 친절히 안내).
 *   4) handler 호출 — handler가 spdk_jsonrpc_*로 응답 빌드.
 *
 * 실행 컨텍스트: spdk_rpc_server_accept(즉 spdk_jsonrpc_server_poll)을 호출하는
 *   reactor의 thread. 모든 RPC handler는 이 thread에서 실행 — cross-thread
 *   자원 접근 시 spdk_thread_send_msg 패턴 필수.
 *
 * 호출 체인: jsonrpc layer (lib/jsonrpc/jsonrpc_server.c의 parse loop) →
 *   [본 함수] → method->func.
 */
static void
jsonrpc_handler(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *method,
		const struct spdk_json_val *params)
{
	struct spdk_rpc_method *m;

	assert(method != NULL);     /* [한국어] jsonrpc layer가 method 누락된 요청은 미리 거부 — 여기 도달 시 NULL 불가 */

	m = _get_rpc_method(method);
	if (m == NULL) {
		/* [한국어] JSON-RPC 2.0 표준 에러 코드 -32601 Method not found */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND, "Method not found");
		return;
	}

	if (m->is_alias_of != NULL) {            /* [한국어] alias 항목 처리 */
		if (m->is_deprecated && !m->deprecation_warning_printed) {
			/* [한국어] 첫 호출 1회 경고 — 운영자에게 신규 이름 사용 유도 */
			SPDK_WARNLOG("RPC method %s is deprecated.  Use %s instead.\n", m->name, m->is_alias_of->name);
			m->deprecation_warning_printed = true;
		}
		m = m->is_alias_of;              /* [한국어] alias→원본 redirect, 원본 함수 호출로 진행 */
	}

	/* [한국어] state mask 검사. 의미: method가 허용한 비트들에 현재 상태가 모두
	 * 포함되어야 한다. 보통 g_rpc_state는 단일 비트(STARTUP 또는 RUNTIME)이므로
	 * (state_mask & g_rpc_state) == g_rpc_state는 "method가 이 상태를 허용함"
	 * 을 의미. */
	if ((m->state_mask & g_rpc_state) == g_rpc_state) {
		m->func(request, params);        /* [한국어] 실제 method handler 호출 — 응답 빌드는 handler 책임 */
	} else {
		/* [한국어] 상태 미스매치 — 사용자에게 어느 단계에서 호출해야 하는지 안내 */
		if (g_rpc_state == SPDK_RPC_STARTUP) {
			/* [한국어] 지금은 STARTUP인데 method는 RUNTIME만 허용 → init 후 호출하라 */
			spdk_jsonrpc_send_error_response_fmt(request,
							     SPDK_JSONRPC_ERROR_INVALID_STATE,
							     "Method may only be called after "
							     "framework is initialized "
							     "using framework_start_init RPC.");
		} else {
			/* [한국어] 지금은 RUNTIME인데 method는 STARTUP만 허용 → --wait-for-rpc로 시작 후
			 * framework_start_init 전에 호출하라 */
			spdk_jsonrpc_send_error_response_fmt(request,
							     SPDK_JSONRPC_ERROR_INVALID_STATE,
							     "Method may only be called before "
							     "framework is initialized. "
							     "Use --wait-for-rpc command line "
							     "parameter and then issue this RPC "
							     "before the framework_start_init RPC.");
		}
	}
}

/*
 * [한국어]
 * spdk_rpc_server_listen - AF_UNIX 소켓에 RPC 서버를 listen 시작
 *
 * @listen_addr: Unix socket 파일 경로 (예: "/var/tmp/spdk.sock"). NULL 불가.
 *
 * @return: 생성된 server 핸들 또는 NULL (listen 실패).
 *
 * 동작:
 *   1) server 객체 calloc.
 *   2) sockaddr_un.sun_family = AF_UNIX, sun_path에 경로 복사.
 *      sun_path는 보통 108 byte 제한 — snprintf 결과 길이 검증.
 *   3) "<path>.lock" 경로의 lock 파일을 open + flock(LOCK_EX|LOCK_NB).
 *      lock 파일 사용 이유: 같은 socket path에 두 SPDK 인스턴스가 실수로
 *      bind하면, 한쪽이 socket 파일을 unlink하면서 다른쪽 연결이 끊기는
 *      경쟁 발생. lock 파일을 별도 두면 advisory lock으로 점유 직렬화 가능.
 *   4) 이전 프로세스가 남긴 socket 파일이 있으면 unlink (lock을 잡았으니 안전).
 *   5) lib/jsonrpc/spdk_jsonrpc_server_listen 호출하여 실제 listen 시작.
 *      jsonrpc_handler를 콜백으로 등록 — 와이어 요청이 도착하면 본 파일의
 *      jsonrpc_handler로 dispatch된다.
 *
 * 실행 컨텍스트: spdk_app_start 초기화 또는 운영자가 동적으로 추가 socket을
 *   listen할 때. 보통 main reactor.
 *
 * 에러 경로: 단계별 cleanup으로 ret 레이블에 모임. 단, 일부 단계(jsonrpc_listen
 *   실패)에서는 lock_fd close와 lock 파일 unlink를 명시적으로 수행 — 다른
 *   단계는 lock 미오픈이므로 skip 가능.
 */
struct spdk_rpc_server *
spdk_rpc_server_listen(const char *listen_addr)
{
	struct spdk_rpc_server *server;
	int rc;

	server = calloc(1, sizeof(struct spdk_rpc_server));
	if (!server) {
		SPDK_ERRLOG("Could not allocate new RPC server\n");
		return NULL;
	}


	assert(listen_addr != NULL);     /* [한국어] 호출자는 반드시 경로를 줘야 함 */

	server->listen_addr_unix.sun_family = AF_UNIX;     /* [한국어] AF_UNIX = local socket — 동일 호스트만 통신 */
	/* [한국어] sun_path에 경로 복사. 결과 길이가 sizeof(sun_path)를 넘으면
	 * snprintf가 truncate한 길이를 반환하므로 명시적으로 검사 — truncated path는
	 * 다른 socket과 충돌할 위험이 있어 거부한다. */
	rc = snprintf(server->listen_addr_unix.sun_path,
		      sizeof(server->listen_addr_unix.sun_path),
		      "%s", listen_addr);
	if (rc < 0 || (size_t)rc >= sizeof(server->listen_addr_unix.sun_path)) {
		SPDK_ERRLOG("RPC Listen address Unix socket path too long\n");
		goto ret;
	}

	/* [한국어] lock 파일 경로 = socket path + ".lock" */
	rc = snprintf(server->lock_path, sizeof(server->lock_path), "%s.lock",
		      server->listen_addr_unix.sun_path);
	if (rc < 0 || (size_t)rc >= sizeof(server->lock_path)) {
		SPDK_ERRLOG("RPC lock path too long\n");
		goto ret;
	}

	/* [한국어] lock 파일 open. O_CREAT로 없으면 생성, 0600 권한 (소유자만 read/write). */
	server->lock_fd = open(server->lock_path, O_RDWR | O_CREAT, 0600);
	if (server->lock_fd == -1) {
		SPDK_ERRLOG("Cannot open lock file %s: %s\n",
			    server->lock_path, spdk_strerror(errno));
		goto ret;
	}

	/* [한국어] flock(LOCK_EX|LOCK_NB): exclusive non-blocking advisory lock.
	 * 이미 다른 프로세스가 잡고 있으면 즉시 EWOULDBLOCK 반환. 같은 socket path를
	 * 다른 SPDK 인스턴스가 동시에 bind하지 못하도록 강제. */
	rc = flock(server->lock_fd, LOCK_EX | LOCK_NB);
	if (rc != 0) {
		SPDK_ERRLOG("RPC Unix domain socket path %s in use. Specify another.\n",
			    server->listen_addr_unix.sun_path);
		goto ret;
	}

	/*
	 * Since we acquired the lock, it is safe to delete the Unix socket file
	 * if it still exists from a previous process.
	 */
	/* [한국어] lock을 잡았으니 이전 프로세스가 남긴 socket 파일은 더 이상 사용
	 * 중이 아님 — 안전하게 unlink하여 깨끗한 상태에서 bind 시작. */
	unlink(server->listen_addr_unix.sun_path);

	/* [한국어] jsonrpc layer에 listen 위임. 파라미터:
	 *   AF_UNIX/0(protocol)/sockaddr/addr_len/handler.
	 * jsonrpc layer가 socket(2), bind(2), listen(2)을 호출하고 내부 poller에
	 * 등록한다. 와이어 요청이 도착하면 jsonrpc_handler가 호출됨. */
	server->jsonrpc_server = spdk_jsonrpc_server_listen(AF_UNIX, 0,
				 (struct sockaddr *) & server->listen_addr_unix,
				 sizeof(server->listen_addr_unix),
				 jsonrpc_handler);
	if (server->jsonrpc_server == NULL) {
		SPDK_ERRLOG("spdk_jsonrpc_server_listen() failed\n");
		close(server->lock_fd);                /* [한국어] flock auto-release on close */
		unlink(server->lock_path);             /* [한국어] lock 파일도 정리 (다른 인스턴스가 재사용 가능하도록) */
		goto ret;
	}

	return server;

ret:
	/* [한국어] 에러 cleanup — server 객체만 free. 위 단계에서 lock_fd가 열렸다면
	 * 해당 단계가 명시적으로 close한 후 goto하므로 여기에는 도달 시 lock_fd 정리
	 * 책임이 없다. */
	free(server);
	return NULL;
}

/*
 * [한국어]
 * spdk_rpc_server_accept - 들어온 RPC 요청을 한 번 처리 (poller 호출)
 *
 * @server: listen 중인 server 핸들.
 *
 * SPDK는 polled-mode이므로 RPC도 reactor가 주기적으로 본 함수를 호출하여 새
 * 연결/요청을 받는다. 차단하지 않으며, 요청이 없으면 즉시 반환. 들어온 요청은
 * 내부에서 jsonrpc_handler를 거쳐 처리된 후 함수가 반환된다.
 *
 * 실행 컨텍스트: SPDK reactor의 RPC poller (보통 master reactor).
 */
void
spdk_rpc_server_accept(struct spdk_rpc_server *server)
{
	assert(server != NULL);
	spdk_jsonrpc_server_poll(server->jsonrpc_server);    /* [한국어] non-blocking poll — accept/read/parse/dispatch 한 사이클 */
}

/*
 * [한국어]
 * spdk_rpc_register_method - 새 RPC method를 registry에 등록 (SPDK_RPC_REGISTER 백엔드)
 *
 * @method: method 이름 문자열 (예: "bdev_get_bdevs"). strdup하여 보관.
 * @func: 처리 함수.
 * @state_mask: 호출 가능한 상태 비트 OR (예: SPDK_RPC_STARTUP|SPDK_RPC_RUNTIME).
 *
 * 호출 시점: 각 서브시스템의 *_rpc.c 파일 끝에 SPDK_RPC_REGISTER(...) 매크로가
 *   constructor 어트리뷰트로 본 함수를 main() 전에 호출. 따라서 본 함수가
 *   호출될 때는 single-thread, 동시성 우려 없음.
 *
 * 실패 처리: 중복 method가 발견되면 ERRLOG + g_rpcs_correct=false 표시 후 무시.
 *   spdk_rpc_verify_methods가 false를 반환하면 spdk_app이 시작을 중단.
 *
 * 메모리: calloc/strdup이 NULL을 반환하면 assert로 abort — 시작 시점에 메모리
 *   부족이면 정상 동작 불가능하다는 정책.
 */
void
spdk_rpc_register_method(const char *method, spdk_rpc_method_handler func, uint32_t state_mask)
{
	struct spdk_rpc_method *m;

	m = _get_rpc_method_raw(method);
	if (m != NULL) {                              /* [한국어] 동일 이름 이미 등록됨 — 빌드 결함 */
		SPDK_ERRLOG("duplicate RPC %s registered...\n", method);
		g_rpcs_correct = false;
		return;
	}

	m = calloc(1, sizeof(struct spdk_rpc_method));
	assert(m != NULL);                            /* [한국어] OOM은 부팅 자체가 불가능하므로 abort */

	m->name = strdup(method);
	assert(m->name != NULL);

	m->func = func;
	m->state_mask = state_mask;

	/* TODO: use a hash table or sorted list */
	/* [한국어] 단방향 list head 삽입 — O(1). lookup은 O(N)이지만 SPDK 운용에서
	 * RPC 호출 빈도가 매우 낮아 (운영자 명령) hash 미도입은 실용상 무리 없음. */
	SLIST_INSERT_HEAD(&g_rpc_methods, m, slist);
}

/*
 * [한국어]
 * spdk_rpc_register_alias_deprecated - 기존 method의 deprecated alias 등록
 *
 * @method: 기존(원본) method 이름. 이미 register되어 있어야 함.
 * @alias: 새로 만들 deprecated alias 이름.
 *
 * 사용처: API rename 시 하위 호환성 보존용 (예: "construct_nvme_bdev" →
 *   "bdev_nvme_attach_controller"로 rename할 때 옛 이름을 alias deprecated로
 *   유지). 호출 시 첫 1회 WARNLOG 출력.
 *
 * 제약: 이중 alias 금지 (alias of alias 불가) — chain 처리 복잡성 회피.
 */
void
spdk_rpc_register_alias_deprecated(const char *method, const char *alias)
{
	struct spdk_rpc_method *m, *base;

	base = _get_rpc_method_raw(method);
	if (base == NULL) {
		/* [한국어] 원본이 없는 alias — 등록 순서 버그 또는 오타 */
		SPDK_ERRLOG("cannot create alias %s - method %s does not exist\n",
			    alias, method);
		g_rpcs_correct = false;
		return;
	}

	if (base->is_alias_of != NULL) {
		/* [한국어] 이중 alias 금지 — 한 단계만 redirect 허용 (resolution 단순화) */
		SPDK_ERRLOG("cannot create alias %s of alias %s\n", alias, method);
		g_rpcs_correct = false;
		return;
	}

	m = calloc(1, sizeof(struct spdk_rpc_method));
	assert(m != NULL);

	m->name = strdup(alias);
	assert(m->name != NULL);

	m->is_alias_of = base;             /* [한국어] alias 표지 — jsonrpc_handler가 redirect */
	m->is_deprecated = true;            /* [한국어] alias는 항상 deprecated 의미로 사용 */
	m->state_mask = base->state_mask;   /* [한국어] state mask는 원본 그대로 — alias라고 다른 게이팅 적용 안 함 */

	/* TODO: use a hash table or sorted list */
	SLIST_INSERT_HEAD(&g_rpc_methods, m, slist);
}

/*
 * [한국어]
 * spdk_rpc_verify_methods - 등록 단계에서 발견된 오류 유무 반환
 *
 * @return: 모든 등록이 정상이면 true, 중복/잘못된 alias가 있으면 false.
 *
 * 사용처: spdk_app_start 초기화 단계에서 호출하여 false면 시작 중단 — 결함 있는
 *   바이너리로 운영하지 않게 하는 안전장치.
 */
bool
spdk_rpc_verify_methods(void)
{
	return g_rpcs_correct;
}

/*
 * [한국어]
 * spdk_rpc_is_method_allowed - method가 주어진 state mask에서 호출 가능한지 판정
 *
 * @method: method 이름.
 * @state_mask: 검사하려는 상태 비트 (예: SPDK_RPC_RUNTIME).
 *
 * @return: 0 호출 가능, -ENOENT(미등록 또는 allowlist 차단), -EPERM(state 미스매치).
 *
 * 사용처: 외부 코드가 method 호출 전에 미리 가능 여부를 확인하고 싶을 때
 *   (예: save_config 시 dump 가능 method 필터링).
 */
int
spdk_rpc_is_method_allowed(const char *method, uint32_t state_mask)
{
	struct spdk_rpc_method *m;

	if (!rpc_is_allowed(method)) {        /* [한국어] allowlist 차단 → -ENOENT (등록 안된 것처럼) */
		return -ENOENT;
	}

	SLIST_FOREACH(m, &g_rpc_methods, slist) {
		if (strcmp(m->name, method) != 0) {       /* [한국어] 이름 불일치 → 다음 항목 */
			continue;
		}

		if ((m->state_mask & state_mask) == state_mask) {     /* [한국어] state 호환 — 호출 가능 */
			return 0;
		} else {
			return -EPERM;        /* [한국어] state 미스매치 — 권한 부족 의미 */
		}
	}

	return -ENOENT;       /* [한국어] registry 미등록 */
}

/*
 * [한국어]
 * spdk_rpc_get_method_state_mask - method의 등록된 state_mask 조회
 *
 * @return: 0 성공, -ENOENT 미등록.
 *
 * 사용처: rpc_get_methods 등에서 method별 가용 상태 정보 노출.
 */
int
spdk_rpc_get_method_state_mask(const char *method, uint32_t *state_mask)
{
	struct spdk_rpc_method *m;

	SLIST_FOREACH(m, &g_rpc_methods, slist) {
		if (strcmp(m->name, method) == 0) {
			*state_mask = m->state_mask;
			return 0;
		}
	}

	return -ENOENT;
}

/*
 * [한국어]
 * spdk_rpc_set_allowlist - 운영자가 활성화할 method 이름 배열 설정
 *
 * @rpc_allowlist: NULL-terminated 문자열 배열. NULL이면 allowlist 해제(전체 허용).
 *
 * 동작: 기존 g_rpcs_allowlist를 free 후 새로 spdk_strarray_dup으로 deep copy.
 *
 * 사용처: spdk_app_opts에 rpc_allowlist를 지정하면 spdk_app_start가 본 함수를
 *   호출하여 보안 필터를 활성화. 컨테이너/오케스트레이션 환경에서 위험 RPC를
 *   비활성화하고 read-only RPC만 노출하고 싶을 때.
 */
void
spdk_rpc_set_allowlist(const char **rpc_allowlist)
{
	spdk_strarray_free(g_rpcs_allowlist);     /* [한국어] 기존 allowlist 해제 (NULL safe) */

	if (rpc_allowlist == NULL) {
		g_rpcs_allowlist = NULL;          /* [한국어] NULL = 전체 허용 모드 */
		return;
	}

	g_rpcs_allowlist = spdk_strarray_dup(rpc_allowlist);     /* [한국어] deep copy — 호출자 buffer 의존 X */
	assert(g_rpcs_allowlist != NULL);          /* [한국어] OOM은 시작 시점 치명적 */
}

/*
 * [한국어]
 * spdk_rpc_server_close - listen 중인 RPC 서버 lifecycle 종료
 *
 * @server: 종료할 server 핸들. NULL 불가.
 *
 * 동작:
 *   1) socket 파일 unlink (남겨두면 다음 인스턴스의 bind를 방해).
 *   2) jsonrpc layer shutdown — 진행 중 요청 정리 + listen socket close.
 *   3) lock_fd close (flock auto-release).
 *   4) lock 파일 unlink.
 *   5) server 객체 free.
 *
 * sun_path[0]/lock_path[0]/lock_fd != -1 sentinel 검사: 다중 close 또는 부분
 *   초기화 상태에서도 안전하게 동작 (idempotent에 가깝게).
 *
 * 호출 시점: spdk_app_stop 이나 동적 server destroy.
 */
void
spdk_rpc_server_close(struct spdk_rpc_server *server)
{
	assert(server != NULL);
	assert(server->jsonrpc_server != NULL);     /* [한국어] 정상 listen된 server만 close 대상 */

	if (server->listen_addr_unix.sun_path[0]) {
		/* Delete the Unix socket file */
		/* [한국어] 다음 SPDK 인스턴스가 같은 path로 bind할 수 있도록 정리 */
		unlink(server->listen_addr_unix.sun_path);
		server->listen_addr_unix.sun_path[0] = '\0';     /* [한국어] sentinel — 재호출 방지 */
	}

	spdk_jsonrpc_server_shutdown(server->jsonrpc_server);    /* [한국어] wire 측 정리 (listen socket close 등) */
	server->jsonrpc_server = NULL;

	if (server->lock_fd != -1) {
		close(server->lock_fd);                           /* [한국어] flock은 close 시 자동 해제 */
		server->lock_fd = -1;
	}

	if (server->lock_path[0]) {
		unlink(server->lock_path);                        /* [한국어] lock 파일도 정리 */
		server->lock_path[0] = '\0';
	}

	free(server);
}

/* [한국어] rpc_get_methods 요청 파라미터 디코딩 구조체.
 *   - current: true이면 현재 state에서 호출 가능한 method만 반환.
 *   - include_aliases: true이면 alias 항목도 함께 반환. */
struct rpc_get_methods {
	bool current;
	bool include_aliases;
};

/* This ugly rpc_rpc_ double prefix is needed for linting and avoids deprecation of this popular RPC  */
/* [한국어] linting 도구가 "rpc_get_methods"라는 함수 이름을 deprecated naming으로 잡는 이슈 회피용
 * 이중 prefix. method 이름 자체("rpc_get_methods")는 매우 널리 쓰이므로 변경할 수 없다 — 그래서
 * 함수 이름만 rpc_rpc_get_methods로 둔다. */
static const struct spdk_json_object_decoder rpc_rpc_get_methods_decoders[] = {
	/* [한국어] {필드명, 구조체 내 오프셋, decoder fn, optional 여부} */
	{"current", offsetof(struct rpc_get_methods, current), spdk_json_decode_bool, true},
	{"include_aliases", offsetof(struct rpc_get_methods, include_aliases), spdk_json_decode_bool, true},
};

/*
 * [한국어]
 * rpc_rpc_get_methods - "rpc_get_methods" RPC handler — 등록된 method 이름 배열 반환
 *
 * @request: jsonrpc 요청.
 * @params: 선택적 {current?: bool, include_aliases?: bool}.
 *
 * 동작: registry를 순회하며 다음을 필터:
 *   - allowlist 차단 → skip.
 *   - alias이고 include_aliases==false → skip.
 *   - current==true이고 현재 state에서 호출 불가 → skip.
 * 살아남은 method 이름들을 JSON array로 응답.
 *
 * 사용처: spdk_rpc.py가 처음 연결되어 사용 가능 RPC를 알아낼 때.
 */
/* This ugly rpc_rpc_ double prefix is needed for linting and avoids deprecation of this popular RPC  */
static void
rpc_rpc_get_methods(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_get_methods req = {};        /* [한국어] 기본값 모두 false */
	struct spdk_json_write_ctx *w;
	struct spdk_rpc_method *m;

	if (params != NULL) {
		/* [한국어] params를 decoder로 파싱. 실패 시 INVALID_PARAMS(-32602)로 응답 */
		if (spdk_json_decode_object(params, rpc_rpc_get_methods_decoders,
					    SPDK_COUNTOF(rpc_rpc_get_methods_decoders), &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	SLIST_FOREACH(m, &g_rpc_methods, slist) {
		if (!rpc_is_allowed(m->name)) {
			continue;          /* [한국어] allowlist 차단 — 노출 자체를 막음 */
		}
		if (m->is_alias_of != NULL && !req.include_aliases) {
			continue;          /* [한국어] alias 숨김 모드 */
		}
		if (req.current && ((m->state_mask & g_rpc_state) != g_rpc_state)) {
			continue;          /* [한국어] 현재 state에서 호출 불가 method 제외 */
		}
		spdk_json_write_string(w, m->name);   /* [한국어] 통과한 method 이름 출력 */
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
/* [한국어] STARTUP과 RUNTIME 모두에서 호출 가능 — meta RPC라 항상 사용 가능해야 함 */
SPDK_RPC_REGISTER("rpc_get_methods", rpc_rpc_get_methods, SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_spdk_get_version - "spdk_get_version" RPC handler — SPDK 빌드 버전 정보 반환
 *
 * @return JSON: {version: "23.05", fields: {major, minor, patch, suffix, commit?}}
 *
 * 파라미터 없는 RPC — params가 있으면 INVALID_PARAMS 에러.
 * SPDK_GIT_COMMIT가 빌드 시 정의되었으면 commit hash도 함께 응답.
 *
 * 사용처: 클라이언트가 SPDK 호환성 확인 (예: spdk_rpc.py가 자기 버전과 비교).
 */
static void
rpc_spdk_get_version(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {                  /* [한국어] 파라미터 없는 RPC — 있으면 거부 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_get_version method requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);

	/* [한국어] "version": "MAJOR.MINOR.PATCH-suffix" 단일 문자열 — 사람이 읽기 쉬운 형식 */
	spdk_json_write_named_string_fmt(w, "version", "%s", SPDK_VERSION_STRING);
	/* [한국어] "fields": {...} — 기계 파싱 친화적 분리 */
	spdk_json_write_named_object_begin(w, "fields");
	spdk_json_write_named_uint32(w, "major", SPDK_VERSION_MAJOR);
	spdk_json_write_named_uint32(w, "minor", SPDK_VERSION_MINOR);
	spdk_json_write_named_uint32(w, "patch", SPDK_VERSION_PATCH);
	spdk_json_write_named_string_fmt(w, "suffix", "%s", SPDK_VERSION_SUFFIX);
#ifdef SPDK_GIT_COMMIT
	/* [한국어] CI 빌드는 git commit hash를 컴파일 매크로로 주입 — 디버깅 시 정확한 빌드 식별 */
	spdk_json_write_named_string_fmt(w, "commit", "%s", SPDK_GIT_COMMIT_STRING);
#endif
	spdk_json_write_object_end(w);     /* [한국어] fields 객체 닫기 */

	spdk_json_write_object_end(w);     /* [한국어] 응답 최상위 객체 닫기 */
	spdk_jsonrpc_end_result(request, w);
}
/* [한국어] 두 상태 모두에서 호출 가능 — 버전 조회는 항상 가능해야 함 */
SPDK_RPC_REGISTER("spdk_get_version", rpc_spdk_get_version,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
