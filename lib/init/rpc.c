/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK RPC 서버 부트스트랩/관리 (rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 데몬에서 JSON-RPC 서버를 띄우고, accept 폴링을 등록하고,
 * 라이프사이클(시작/일시중지/재개/종료)을 관리하는 init 라이브러리 모듈이다.
 * SPDK 는 외부 관리 도구(`scripts/rpc.py` 등)와 UNIX 도메인 소켓 기반의 JSON-RPC
 * 로 통신하며, 본 파일이 그 서버 측 진입점을 만든다.
 * 한 SPDK 프로세스는 동시에 여러 listen 주소(예: `/var/tmp/spdk.sock` 와
 * 임시 설정 로딩용 소켓)를 운용할 수 있도록 g_init_rpc_servers 리스트로
 * 다수의 init_rpc_server 인스턴스를 보관한다. 단, 모든 서버는 한 개의
 * 공유 `g_rpc_poller`(4 ms 주기) 가 일괄 accept 하여 처리한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 부팅 흐름:
 *   spdk_app_start()
 *     → 서브시스템 초기화 (subsystem.c — STARTUP phase)
 *     → spdk_rpc_initialize(listen_addr) (본 파일)
 *         → spdk_rpc_server_listen (lib/jsonrpc) — UDS 바인드+listen
 *         → SPDK_POLLER_REGISTER (lib/thread) — 4 ms 주기 accept poller 등록
 *     → 사용자 main 진입 (이후 RPC 명령은 poller 가 비동기로 처리)
 * 종료 시: spdk_rpc_finish() 가 모든 서버를 닫고 poller 를 해제.
 * 실행 컨텍스트: 모든 함수가 SPDK app thread (master reactor 의 spdk_thread)
 * 위에서만 호출되어야 한다 (assert(spdk_thread_is_app_thread(NULL))).
 *
 * === 타 모듈과의 연결 ===
 * - lib/jsonrpc: spdk_rpc_server_listen / accept / close 의 실제 소켓 작업.
 * - lib/thread (poller): SPDK_POLLER_REGISTER 로 reactor 의 poll 루프에 등록.
 *   accept poller 는 반환값이 SPDK_POLLER_BUSY 라 매 4 ms 마다 호출된다.
 * - lib/init/json_config.c: 임시 RPC 소켓을 만들고 설정 로드를 위해 본 파일의
 *   spdk_rpc_initialize / spdk_rpc_server_finish 를 활용한다.
 * - include/spdk/rpc.h: 외부 공개 API. 사용자 도구가 호출하는 진입점.
 * - JSON-RPC 메서드 자체는 다른 파일들(subsystem_rpc.c, bdev_rpc.c, …)이
 *   SPDK_RPC_REGISTER 매크로로 자동 등록한 핸들러들을 spdk_rpc_verify_methods()
 *   가 검증하고 dispatch 한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_rpc_initialize(listen_addr, opts): 새 RPC 서버를 listen_addr 에 띄우고
 *   accept poller 를 (없다면) 등록.
 * - spdk_rpc_finish(): 모든 RPC 서버 종료(닫기) + poller 해제.
 * - spdk_rpc_server_finish(addr): 단일 서버만 종료.
 * - spdk_rpc_server_pause/resume(addr): 특정 서버의 accept 만 일시 중단/재개.
 * - rpc_subsystem_poll_servers(): poller 콜백, 모든 active 서버에 accept() 시도.
 * - struct init_rpc_server: 한 listen 주소 = 한 서버 인스턴스. STAILQ 로 연결.
 */

#include "spdk/stdinc.h"        /* [한국어] 표준 C 라이브러리 포함 (stdio, string, stdlib, assert, sys/un.h 의 sockaddr_un 등) */

#include "spdk/env.h"           /* [한국어] DPDK 환경 추상화 (현 파일에서는 직접 사용 적음 — 표준 포함) */
#include "spdk/init.h"          /* [한국어] spdk_rpc_initialize/finish 등 본 파일이 정의하는 공개 API 의 prototype */
#include "spdk/thread.h"        /* [한국어] spdk_thread_is_app_thread, SPDK_POLLER_REGISTER 등 — poller 등록에 필수 */
#include "spdk/log.h"           /* [한국어] SPDK_ERRLOG 등 */
#include "spdk/rpc.h"           /* [한국어] spdk_rpc_server_listen/accept/close, spdk_rpc_verify_methods, spdk_rpc_opts 등 */

/* [한국어] RPC accept 폴러 호출 주기 — 4 ms (=4000 us). 짧을수록 응답 latency 가 줄지만
 * CPU 점유는 늘어남. 외부 관리 트래픽이 빈번하지 않으므로 4 ms 가 합리적 기본값. */
#define RPC_SELECT_INTERVAL	4000 /* 4ms */

/* [한국어] 모든 init RPC 서버에 대해 공유되는 단일 accept poller.
 * 설정자: spdk_rpc_initialize() 의 첫 서버 등록 시 SPDK_POLLER_REGISTER 가 할당.
 * 읽는 자: spdk_rpc_server_finish() 가 마지막 서버 제거 시 spdk_poller_unregister 로 해제.
 * 값 범위: NULL(서버 없음 상태) 또는 유효한 spdk_poller* (서버 1개 이상 존재).
 * 동기화: 오직 app thread 에서만 접근 — 별도 락 불필요. */
static struct spdk_poller *g_rpc_poller = NULL;

/* [한국어] 단일 RPC 서버 인스턴스. 한 listen 주소(UDS path)에 대응.
 * SPDK 는 같은 프로세스에서 여러 RPC 서버를 동시에 띄울 수 있으므로(예: 일반 관리
 * 소켓 + 설정 로드 임시 소켓) 본 구조체를 STAILQ 로 묶는다. */
struct init_rpc_server {
	struct spdk_rpc_server *server;
	/* [한국어] lib/jsonrpc 가 반환한 실제 RPC 서버 핸들 (소켓 fd, 클라이언트 리스트 보유).
	 * 설정자: spdk_rpc_initialize() 가 spdk_rpc_server_listen() 결과로 채움.
	 * 읽는 자: rpc_subsystem_poll_servers() 가 accept 시 사용, finish 시 close.
	 * 값 범위: 유효한 포인터 (생성 실패 시 init_server 자체를 free 하므로 NULL 케이스 없음).
	 * 동기화: app thread 에서만 접근. */

	char listen_addr[sizeof(((struct sockaddr_un *)0)->sun_path)];
	/* [한국어] UNIX 도메인 소켓 경로 문자열 (sockaddr_un.sun_path 와 동일 길이 = 통상 108B).
	 * 설정자: spdk_rpc_initialize() 가 인자 listen_addr 을 snprintf 로 복사.
	 * 읽는 자: get_server_by_addr() 검색, spdk_rpc_server_finish() 등이 strcmp 비교.
	 * 값 범위: 절대/상대 파일 경로 (NUL 종결). 길이 초과는 spdk_rpc_initialize 에서 거부.
	 * 동기화: 생성 후 변경 없음 (read-only after init) — 락 불필요. */

	bool active;
	/* [한국어] accept 활성화 플래그. false 이면 poller 가 이 서버에 대해 accept 를 건너뜀.
	 * 설정자: spdk_rpc_initialize() 시 true, spdk_rpc_server_pause/resume() 가 토글.
	 * 읽는 자: rpc_subsystem_poll_servers() 의 분기 조건.
	 * 값 범위: true=accept 수신, false=일시 중단 (이미 연결된 클라이언트는 영향 없음).
	 * 동기화: app thread 에서만 접근. */

	STAILQ_ENTRY(init_rpc_server) link;
	/* [한국어] g_init_rpc_servers STAILQ 의 next 링크.
	 * 설정자: STAILQ_INSERT_TAIL (initialize) / STAILQ_REMOVE (finish).
	 * 읽는 자: STAILQ_FOREACH 순회들.
	 * 값 범위: STAILQ 내부 포인터.
	 * 동기화: app thread 단일 변경. */
};

/* [한국어] 활성 RPC 서버들을 보관하는 단방향 큐 헤드 (singly-linked tail queue).
 * 설정자: STAILQ_INSERT_TAIL (initialize), STAILQ_REMOVE (finish).
 * 읽는 자: 모든 라이프사이클 함수에서 순회/검색.
 * 동기화: app thread 단독 접근. */
static STAILQ_HEAD(, init_rpc_server) g_init_rpc_servers = STAILQ_HEAD_INITIALIZER(
			g_init_rpc_servers);

/*
 * [한국어]
 * rpc_subsystem_poll_servers - 모든 활성 RPC 서버에 대해 accept 시도하는 poller 콜백
 *
 * @arg: 사용하지 않음 (NULL).
 * @return: SPDK_POLLER_BUSY — 이번 tick 에서 일을 했다고 표시 (SPDK 통계용).
 *
 * SPDK_POLLER_REGISTER 로 reactor 의 polling 루프에 등록되어 매 4 ms 마다 호출된다.
 * 모든 init_rpc_server 를 순회하면서 active=true 인 것에 대해 spdk_rpc_server_accept()
 * 를 호출 — 새 클라이언트 연결을 받아들이고 받은 요청을 dispatch 한다.
 * accept 자체가 새 요청 처리도 일부 트리거한다 (lib/jsonrpc 내부 dispatch).
 *
 * 실행 컨텍스트: app thread (master reactor) 의 poller. polling 모드라 인터럽트가 아닌
 * 능동 호출.
 *
 * 호출 체인:
 *   reactor poll loop → [rpc_subsystem_poll_servers]
 *     → spdk_rpc_server_accept (lib/jsonrpc) → 클라이언트 dispatch → 등록된 SPDK_RPC_REGISTER 핸들러
 */
static int
rpc_subsystem_poll_servers(void *arg)
{
	struct init_rpc_server *init_server;        /* [한국어] STAILQ 순회용 임시 노드 포인터 */

	/* [한국어] 등록된 모든 서버를 순회 — 보통 1~2개 (정상 운영 소켓 + 임시 설정 소켓) */
	STAILQ_FOREACH(init_server, &g_init_rpc_servers, link) {
		if (init_server->active) {           /* [한국어] pause 된 서버는 skip — 일시 중단 의미 보존 */
			/* [한국어] non-blocking accept + 받은 요청 dispatch (lib/jsonrpc 가 처리).
			 * 새 연결 없거나 받을 데이터 없어도 즉시 반환. */
			spdk_rpc_server_accept(init_server->server);
		}
	}

	return SPDK_POLLER_BUSY;                    /* [한국어] BUSY=일 했다고 표시. SPDK 의 통계에서 idle/busy 분류용. */
}

/*
 * [한국어]
 * rpc_opts_copy - spdk_rpc_opts 구조체를 안전하게 복사 (forward-compat)
 *
 * @opts: 대상 구조체 (이 함수가 채울 곳).
 * @opts_src: 원본 구조체 (사용자가 전달).
 * @size: 사용자가 전달한 opts_src 의 size 필드 (호환성 검사용).
 * @return: void.
 *
 * SPDK 의 size-prefixed struct 패턴을 구현. 사용자가 제공한 opts_src 의 size
 * 가 새 필드를 포함할 만큼 크면 그 필드를 복사하고, 작으면 무시 — ABI 호환을
 * 유지하면서 새 필드를 추가할 수 있게 한다. SET_FIELD 매크로가 offsetof+sizeof
 * 로 필드 별 검사를 수행한다.
 *
 * 호출 체인:
 *   rpc_set_spdk_log_opts → [rpc_opts_copy]
 */
static void
rpc_opts_copy(struct spdk_rpc_opts *opts, const struct spdk_rpc_opts *opts_src,
	      size_t size)
{
	assert(opts);                       /* [한국어] 대상 포인터는 NULL 이면 안 됨 — 프로그래밍 오류 방지 */
	assert(opts_src);                   /* [한국어] 원본 포인터도 NULL 이면 안 됨 */

	opts->size = size;                  /* [한국어] 출력 구조체에 실제 사용 가능한 크기를 기록 — 후속 코드가 같은 검사를 할 수 있도록 */

/* [한국어] SET_FIELD 매크로: 사용자가 전달한 size 가 해당 필드를 포함할 만큼 크면
 * (= 사용자가 그 필드를 알고 있는 SPDK 헤더로 빌드되었다면) 복사한다.
 * 이렇게 해서 새 필드 추가 시 구버전 사용자 코드는 영향을 받지 않는다 (ABI 호환). */
#define SET_FIELD(field) \
	if (offsetof(struct spdk_rpc_opts, field) + sizeof(opts->field) <= size) { \
		opts->field = opts_src->field; \
	} \

	SET_FIELD(log_file);                /* [한국어] log_file 필드 복사 (RPC 디버그 로그 파일 경로 또는 NULL) */
	SET_FIELD(log_level);               /* [한국어] log_level 필드 복사 (SPDK_LOG_DISABLED, INFO, DEBUG 등) */

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	/* [한국어] 컴파일 타임 size 체크 — 누군가 spdk_rpc_opts 에 필드 추가 시 본 파일의 SET_FIELD 도
	 * 같이 갱신했는지 강제. 24바이트는 현재 (log_file: 8B + log_level: 4B + size: 8B + padding) 합산. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_rpc_opts) == 24, "Incorrect size");

#undef SET_FIELD
}

/*
 * [한국어]
 * rpc_opts_get_default - spdk_rpc_opts 의 기본값으로 채우기
 *
 * @opts: 채울 대상 구조체.
 * @size: 사용자가 알고 있는 구조체 크기 (forward-compat 검사).
 * @return: void.
 *
 * 사용자가 별도 옵션을 전달하지 않은 경우의 기본값을 채운다.
 * - log_file = NULL (별도 RPC 로그 파일 없음 → SPDK 기본 로그로)
 * - log_level = SPDK_LOG_DISABLED (RPC 별도 로깅 비활성)
 *
 * 호출 체인:
 *   rpc_set_spdk_log_opts → [rpc_opts_get_default]
 */
static void
rpc_opts_get_default(struct spdk_rpc_opts *opts, size_t size)
{
	assert(opts);                       /* [한국어] 대상 포인터 NULL 금지 */

	opts->size = size;                  /* [한국어] 출력 구조체 크기 정보 기록 */

/* [한국어] SET_FIELD: 위 rpc_opts_copy 와 동일한 forward-compat 패턴이지만 값 복사가 아닌
 * 리터럴 값 대입. */
#define SET_FIELD(field, value) \
	if (offsetof(struct spdk_rpc_opts, field) + sizeof(opts->field) <= size) { \
		opts->field = value; \
	} \

	SET_FIELD(log_file, NULL);          /* [한국어] 기본: RPC 전용 로그 파일 사용 안 함 */
	SET_FIELD(log_level, SPDK_LOG_DISABLED);  /* [한국어] 기본: RPC 추가 로깅 비활성 */

#undef SET_FIELD
}

/*
 * [한국어]
 * rpc_verify_opts_and_methods - opts 유효성 + RPC 메서드 등록 일관성 검증
 *
 * @opts: 사용자가 전달한 옵션 (NULL 가능).
 * @return: 0=정상, -EINVAL=검증 실패.
 *
 * (1) spdk_rpc_verify_methods(): SPDK_RPC_REGISTER 매크로로 등록된 모든 RPC 메서드의
 *     이름 충돌/누락 여부 등을 확인. 한 번이라도 실패하면 RPC 서버 시작을 거부.
 * (2) opts 가 NULL 이 아닌데 size==0 이면 명백한 오용 — 거부.
 *
 * 호출 체인:
 *   spdk_rpc_initialize → [rpc_verify_opts_and_methods] → spdk_rpc_verify_methods (lib/rpc)
 */
static int
rpc_verify_opts_and_methods(const struct spdk_rpc_opts *opts)
{
	if (!spdk_rpc_verify_methods()) {   /* [한국어] 등록된 모든 RPC 메서드 정합성 검사 — 충돌/오류 시 false */
		return -EINVAL;
	}

	if (opts != NULL && opts->size == 0) {  /* [한국어] opts 가 있는데 size 가 0이면 zero-init 채로 전달된 잘못된 호출 */
		SPDK_ERRLOG("size in the options structure should not be zero\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * rpc_set_spdk_log_opts - jsonrpc 라이브러리의 로그 파일/레벨 설정 적용
 *
 * @_opts: 사용자 전달 opts 또는 NULL.
 * @return: void.
 *
 * 첫 RPC 서버 시작 시에는 항상 적용되며, 두 번째 이후 서버에서는
 * (1) 사용자가 명시적으로 opts 를 전달했거나,
 * (2) 그 시점에 등록된 서버가 0개 (매우 드문 케이스) 인 경우에만 적용한다.
 * 즉, 두 번째 서버를 띄우면서 opts=NULL 이면 기존 로그 설정을 유지 — 의도치 않은 reset 방지.
 *
 * 호출 체인:
 *   spdk_rpc_initialize → [rpc_set_spdk_log_opts]
 *     → spdk_jsonrpc_set_log_file/level (lib/jsonrpc)
 */
static void
rpc_set_spdk_log_opts(const struct spdk_rpc_opts *_opts)
{
	struct spdk_rpc_opts opts;          /* [한국어] 로컬 사본 — 기본값으로 채운 뒤 사용자 값 덮어씀 */

	rpc_opts_get_default(&opts, sizeof(opts));  /* [한국어] 모든 필드를 기본값(NULL/DISABLED)으로 초기화 */
	if (_opts != NULL) {
		/* [한국어] 사용자가 옵션을 전달했으므로 기본값 위에 사용자 값 복사 */
		rpc_opts_copy(&opts, _opts, _opts->size);
	} else if (!STAILQ_EMPTY(&g_init_rpc_servers)) {
		/* [한국어] 사용자 opts 가 NULL 이고 이미 다른 RPC 서버가 존재 — 기존 로그 설정을 보존하기 위해 적용 스킵.
		 * 첫 서버일 때(STAILQ_EMPTY)는 적용해서 기본값을 한 번 세팅. */
		return;
	}

	spdk_jsonrpc_set_log_file(opts.log_file);    /* [한국어] jsonrpc 라이브러리에 로그 파일 핸들 설정 */
	spdk_jsonrpc_set_log_level(opts.log_level);  /* [한국어] jsonrpc 라이브러리의 로그 레벨 설정 */
}

/*
 * [한국어]
 * get_server_by_addr - listen 주소로 RPC 서버 인스턴스 검색
 *
 * @listen_addr: 검색할 UDS 경로 문자열.
 * @return: 매칭되는 init_rpc_server* 또는 NULL.
 *
 * STAILQ 선형 검색. 서버 수가 작으므로(보통 1~2개) 충분히 빠름.
 *
 * 호출 체인:
 *   spdk_rpc_initialize / spdk_rpc_server_finish / set_server_active_flag → [get_server_by_addr]
 */
static struct init_rpc_server *
get_server_by_addr(const char *listen_addr)
{
	struct init_rpc_server *init_server;        /* [한국어] 순회용 포인터 */

	STAILQ_FOREACH(init_server, &g_init_rpc_servers, link) {  /* [한국어] 등록된 모든 서버 순회 */
		if (strcmp(listen_addr, init_server->listen_addr) == 0) {  /* [한국어] 경로 문자열 정확 일치 검사 */
			return init_server;
		}
	}

	return NULL;                                /* [한국어] 매칭 없음 */
}

/*
 * [한국어]
 * spdk_rpc_initialize - 새로운 JSON-RPC 서버를 listen_addr 에 띄움 (공개 API)
 *
 * @listen_addr: UNIX 도메인 소켓 경로 (예: "/var/tmp/spdk.sock"). NULL 금지.
 * @opts: 옵션(로그 파일/레벨) 또는 NULL.
 * @return: 0=성공, 음수 errno (EINVAL/ENOMEM/EADDRINUSE 등).
 *
 * SPDK 부팅 시 초기 RPC 서버를 띄우거나, 부팅 후 추가 서버를 띄울 때 호출.
 * (1) opts/메서드 일관성 검증, (2) 중복 주소 검사, (3) init_rpc_server 할당,
 * (4) listen_addr 복사, (5) lib/jsonrpc 의 listen 호출, (6) 공유 accept poller
 * 가 없다면 등록, (7) STAILQ 에 노드 삽입.
 * 첫 서버 등록 시에만 g_rpc_poller 가 생성되고, 이후 서버는 같은 poller 를 공유.
 *
 * 실행 컨텍스트: SPDK app thread (master reactor) 만 호출 가능 (assert).
 *
 * 호출 체인:
 *   사용자 코드 (예: spdk_app_start 내부 또는 json_config.c) → [spdk_rpc_initialize]
 *     → spdk_rpc_verify_methods, spdk_rpc_server_listen, SPDK_POLLER_REGISTER
 */
int
spdk_rpc_initialize(const char *listen_addr, const struct spdk_rpc_opts *opts)
{
	struct init_rpc_server *init_server;        /* [한국어] 새로 할당할 서버 인스턴스 */
	int rc;                                     /* [한국어] 반환/에러 코드 */

	assert(spdk_thread_is_app_thread(NULL));    /* [한국어] 반드시 app thread (master reactor)에서만 호출되어야 함 — 동시성 보호 */

	if (listen_addr == NULL) {                  /* [한국어] 필수 인자 검사 */
		return -EINVAL;
	}

	rc = rpc_verify_opts_and_methods(opts);     /* [한국어] opts 유효성 + 등록된 RPC 메서드 일관성 확인 */
	if (rc) {
		return rc;
	}

	if (get_server_by_addr(listen_addr) != NULL) {  /* [한국어] 동일 주소로 이미 listening 중이면 EADDRINUSE */
		SPDK_ERRLOG("Socket listen_addr already in use\n");
		return -EADDRINUSE;
	}

	init_server = calloc(1, sizeof(struct init_rpc_server));  /* [한국어] zero-init 으로 서버 노드 할당 */
	if (init_server == NULL) {
		SPDK_ERRLOG("Unable to allocate init RPC server\n");
		return -ENOMEM;
	}

	/* [한국어] listen_addr 문자열을 sockaddr_un.sun_path 크기 한도 내로 복사. snprintf 가 잘려도 음수 반환은 거의 없으나 표준 검사. */
	rc = snprintf(init_server->listen_addr, sizeof(init_server->listen_addr), "%s",
		      listen_addr);
	if (rc < 0) {                               /* [한국어] 인코딩 에러 — 사실상 거의 발생 안 함 */
		SPDK_ERRLOG("Unable to copy listen address %s\n", listen_addr);
		free(init_server);                  /* [한국어] 할당 노드 회수 — 누수 방지 */
		return -EINVAL;
	}

	/* Listen on the requested address */
	/* [한국어] 실제 UDS bind+listen — lib/jsonrpc 가 socket(AF_UNIX, SOCK_STREAM) 후 bind/listen 수행.
	 * 이 호출 후엔 외부 클라이언트가 connect 가능한 상태가 됨. */
	init_server->server = spdk_rpc_server_listen(listen_addr);
	if (init_server->server == NULL) {          /* [한국어] listen 실패 — 권한/경로 충돌/이미 존재 등 */
		SPDK_ERRLOG("Unable to start RPC service at %s\n", listen_addr);
		free(init_server);                  /* [한국어] 할당 노드 회수 */
		return -EINVAL;
	}

	rpc_set_spdk_log_opts(opts);                /* [한국어] (필요한 경우) 로그 파일/레벨 갱신 */
	init_server->active = true;                 /* [한국어] 처음 띄울 때는 항상 accept 활성 상태 */

	STAILQ_INSERT_TAIL(&g_init_rpc_servers, init_server, link);  /* [한국어] 전역 서버 리스트에 추가 — 이후 poller 가 인지 */
	if (g_rpc_poller == NULL) {                 /* [한국어] 첫 서버 등록 시에만 poller 생성, 이후 서버는 기존 poller 를 공유 */
		/* Register a poller to periodically check for RPCs */
		/* [한국어] 4 ms 주기로 모든 서버에 대해 accept 시도하는 poller 등록.
		 * SPDK_POLLER_REGISTER 는 lib/thread 의 매크로 — 현재 spdk_thread 의 poller 리스트에 추가. */
		g_rpc_poller = SPDK_POLLER_REGISTER(rpc_subsystem_poll_servers, NULL, RPC_SELECT_INTERVAL);
	}

	return 0;
}

/*
 * [한국어]
 * spdk_rpc_server_finish - 단일 RPC 서버를 닫고 리소스 해제 (공개 API)
 *
 * @listen_addr: 종료할 서버의 UDS 경로.
 * @return: void (실패 시 SPDK_ERRLOG 만 출력).
 *
 * 매칭되는 서버를 찾아 spdk_rpc_server_close (소켓 close + 메모리 해제) 호출,
 * STAILQ 에서 제거, 노드 free. 마지막 서버를 제거하면 공유 poller 도 unregister.
 * json_config.c 의 임시 RPC 소켓 종료 경로에서 자주 호출된다.
 *
 * 호출 체인:
 *   spdk_rpc_finish / json_config.c → [spdk_rpc_server_finish]
 *     → spdk_rpc_server_close (lib/jsonrpc), spdk_poller_unregister (lib/thread)
 */
void
spdk_rpc_server_finish(const char *listen_addr)
{
	struct init_rpc_server *init_server;        /* [한국어] 검색된 서버 노드 */

	assert(spdk_thread_is_app_thread(NULL));    /* [한국어] app thread 단독 접근 강제 */

	init_server = get_server_by_addr(listen_addr);  /* [한국어] 종료 대상 검색 */
	if (!init_server) {                         /* [한국어] 매칭 없으면 에러 로그 후 무시 — 멱등성 부분 보장 */
		SPDK_ERRLOG("No server listening on provided address: %s\n", listen_addr);
		return;
	}

	spdk_rpc_server_close(init_server->server);  /* [한국어] UDS 소켓 close + 클라이언트 정리 (lib/jsonrpc) */
	STAILQ_REMOVE(&g_init_rpc_servers, init_server, init_rpc_server, link);  /* [한국어] 전역 리스트에서 제거 */
	free(init_server);                          /* [한국어] 노드 메모리 회수 */

	if (STAILQ_EMPTY(&g_init_rpc_servers)) {    /* [한국어] 더 이상 서버가 없으면 공유 poller 해제 — 불필요한 polling 비용 절약 */
		spdk_poller_unregister(&g_rpc_poller);  /* [한국어] poller 해제 (반환 후 g_rpc_poller=NULL 로 클리어됨) */
	}
}

/*
 * [한국어]
 * spdk_rpc_finish - 모든 RPC 서버를 일괄 종료 (공개 API)
 *
 * @return: void.
 *
 * SPDK 종료 경로(spdk_app_stop → subsystem fini → 본 함수)에서 호출되어
 * 모든 listening 서버를 차례로 닫는다. STAILQ_FOREACH_SAFE 로 순회 중 제거에 안전.
 *
 * 호출 체인:
 *   SPDK 종료 흐름 → [spdk_rpc_finish] → spdk_rpc_server_finish (각 서버마다)
 */
void
spdk_rpc_finish(void)
{
	struct init_rpc_server *init_server, *tmp;  /* [한국어] FOREACH_SAFE 의 현재/다음 노드 임시 변수 */

	/* [한국어] 순회 중 제거가 안전한 매크로 — 각 서버에 대해 finish 호출 (poller 도 마지막에 정리됨) */
	STAILQ_FOREACH_SAFE(init_server, &g_init_rpc_servers, link, tmp) {
		spdk_rpc_server_finish(init_server->listen_addr);
	}
}

/*
 * [한국어]
 * set_server_active_flag - 특정 서버의 active 플래그 토글
 *
 * @listen_addr: 대상 서버의 UDS 경로.
 * @is_active: true=accept 재개, false=일시 중단.
 * @return: void.
 *
 * pause/resume API 의 공통 구현. accept poller 는 이 플래그를 보고 해당 서버를
 * 건너뛸지 여부를 결정한다.
 *
 * 호출 체인:
 *   spdk_rpc_server_pause / spdk_rpc_server_resume → [set_server_active_flag]
 */
static void
set_server_active_flag(const char *listen_addr, bool is_active)
{
	struct init_rpc_server *init_server;        /* [한국어] 대상 서버 */

	assert(spdk_thread_is_app_thread(NULL));    /* [한국어] app thread 강제 */

	init_server = get_server_by_addr(listen_addr);  /* [한국어] 주소로 서버 검색 */
	if (!init_server) {                         /* [한국어] 미발견 — 에러 로그 후 종료 */
		SPDK_ERRLOG("No server listening on provided address: %s\n", listen_addr);
		return;
	}

	init_server->active = is_active;            /* [한국어] 플래그 갱신 — 다음 poller tick 부터 효과 */
}

/*
 * [한국어]
 * spdk_rpc_server_pause - 특정 RPC 서버의 accept 일시 중단 (공개 API)
 *
 * @listen_addr: 대상 서버 주소.
 * @return: void.
 *
 * 이미 연결된 클라이언트와 in-flight 요청은 영향 없이 처리되지만,
 * 새 connect 는 받지 않게 된다. 설정 로드 등 임계 구간에서 외부 명령을 일시
 * 차단하기 위한 용도.
 *
 * 호출 체인:
 *   사용자 → [spdk_rpc_server_pause] → set_server_active_flag(.., false)
 */
void
spdk_rpc_server_pause(const char *listen_addr)
{
	set_server_active_flag(listen_addr, false);  /* [한국어] active=false → poller 가 accept 스킵 */
}

/*
 * [한국어]
 * spdk_rpc_server_resume - 특정 RPC 서버의 accept 재개 (공개 API)
 *
 * @listen_addr: 대상 서버 주소.
 * @return: void.
 *
 * pause 이후 다시 새 클라이언트를 받을 수 있게 만든다.
 *
 * 호출 체인:
 *   사용자 → [spdk_rpc_server_resume] → set_server_active_flag(.., true)
 */
void
spdk_rpc_server_resume(const char *listen_addr)
{
	set_server_active_flag(listen_addr, true);   /* [한국어] active=true → poller 가 accept 재개 */
}
