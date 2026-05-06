/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] bdev 코어의 JSON-RPC 핸들러 모음 (bdev_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK bdev 서브시스템이 외부(rpc.py CLI, 테스트 자동화, 다른 관리 도구)
 * 와 주고받는 모든 JSON-RPC 메서드의 진입점을 한곳에 모아둔 디스패처 모음이다.
 * 각 핸들러는 (a) 들어온 JSON params 를 spdk_json_decode_object 로 C 구조체에 매핑하고,
 * (b) 파라미터를 검증하고, (c) lib/bdev 코어 API(spdk_bdev_get_device_stat,
 * spdk_bdev_examine, spdk_bdev_set_qos_rate_limits 등)에 위임한 뒤,
 * (d) 콜백에서 spdk_jsonrpc_send_result/_send_error_response 로 응답을 직렬화한다.
 * 이 파일에 RPC 메서드를 추가하면 SPDK_RPC_REGISTER 매크로가 컴파일 타임 constructor
 * 로 동작해 RPC 서버 dispatch table 에 자동 등록되며 별도 wiring 코드가 필요 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 JSON-RPC 서비스는 lib/jsonrpc 에서 TCP 또는 unix-socket 으로 들어오는
 * 요청을 파싱하고 method 이름으로 핸들러를 찾아 호출한다. 이 파일의 핸들러는
 * 그 dispatch table 의 "bdev_*" prefix 메서드를 담당한다.
 *   외부 클라이언트(rpc.py)
 *     → unix socket (/var/tmp/spdk.sock 기본)
 *     → lib/jsonrpc/jsonrpc_server*.c (요청 파싱)
 *     → spdk_rpc_register_method 로 등록된 dispatch table 검색
 *     → [bdev_rpc.c 의 rpc_* 핸들러]  ← 여기
 *     → lib/bdev 코어 API 호출 (bdev.c, bdev_io_stat.c 등)
 *     → 비동기 콜백에서 spdk_jsonrpc_send_result*
 * RPC 핸들러는 spdk_app_thread (= 메인 reactor 스레드) 컨텍스트에서 호출되며,
 * 채널별 통계처럼 다른 스레드를 hop 해야 하는 작업은 spdk_bdev_for_each_channel
 * 의 메시지 전달 메커니즘을 통해 라운드트립 후 다시 app_thread 로 돌아와 응답을 보낸다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/bdev/bdev.c (bdev_internal.h) 의 코어 API: bdev_alloc_io_stat,
 *   bdev_free_io_stat, bdev_reset_device_stat, spdk_bdev_get_device_stat,
 *   spdk_bdev_for_each_channel, spdk_bdev_set_qos_rate_limits 등을 호출.
 * - lib/jsonrpc 의 spdk_jsonrpc_request*, spdk_json_write_ctx*: 입출력 직렬화.
 * - include/spdk/bdev_module.h: bdev->fn_table->dump_device_stat_json 같은
 *   드라이버 콜백을 호출해 모듈별 driver_specific 통계를 얹는다.
 * - include/spdk/histogram_data.h: latency histogram base64 인코딩/디코딩.
 * - 데이터 흐름: 클라이언트 JSON params → struct rpc_*  →  bdev API 호출 →
 *   콜백에서 통계/메타데이터 수집 → JSON 응답 → 클라이언트.
 * - 공유 자료구조: 글로벌 bdev 리스트(spdk_for_each_bdev), spdk_bdev_opts,
 *   per-bdev histogram_granularity/min/max 같은 internal 필드.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_bdev_set_options(): bdev_io_pool_size 등 글로벌 옵션 설정. STARTUP only.
 * - rpc_bdev_examine(): 특정 bdev 에 대해 examine 트리거 — lvol, gpt 같은 examine
 *   모듈이 검사 후 자식 bdev 를 자동 생성하도록 한다.
 * - rpc_bdev_wait_for_examine(): 모든 진행 중 examine 완료까지 비동기 대기.
 * - rpc_bdev_get_iostat() / rpc_bdev_reset_iostat(): I/O 통계 조회/리셋. 다중 bdev,
 *   채널별 옵션 지원. fan-out 카운터(bdev_count) 로 비동기 응답을 직렬화한다.
 * - rpc_bdev_get_bdevs(): bdev 메타데이터(이름/크기/QoS/UUID/지원 IO 타입 등) 덤프.
 * - rpc_bdev_set_qos_limit() / rpc_bdev_set_qd_sampling_period(): QoS, queue-depth
 *   샘플링 설정.
 * - rpc_bdev_enable_histogram() / rpc_bdev_get_histogram(): latency histogram
 *   on/off 와 base64 인코딩된 버킷 데이터 조회.
 * - struct rpc_get_iostat_ctx / bdev_get_iostat_ctx: get_iostat fan-out 응답을
 *   누적·종료 처리하는 context (bdev_count 가 0이 되면 응답 발송).
 */

#include "spdk/bdev.h"          /* [한국어] bdev 공개 API: spdk_bdev, spdk_bdev_open_ext, get_device_stat 등 */

#include "spdk/env.h"           /* [한국어] DPDK 추상화: spdk_get_ticks_hz/spdk_get_ticks 로 통계의 시간 기준값 출력 */
#include "spdk/rpc.h"           /* [한국어] SPDK_RPC_REGISTER 매크로와 STARTUP/RUNTIME phase 상수 정의 */
#include "spdk/util.h"          /* [한국어] SPDK_COUNTOF (배열 길이), offsetof 매크로 등 유틸 */
#include "spdk/string.h"        /* [한국어] spdk_strerror — errno 부호화된 메시지를 영문 문자열로 변환 */
#include "spdk/base64.h"        /* [한국어] histogram 버킷 배열을 RPC 응답에 base64 로 인코딩하기 위함 */
#include "spdk/bdev_module.h"   /* [한국어] bdev->fn_table 필드 접근(드라이버별 dump_device_stat_json 호출 시 필요) */
#include "spdk/dma.h"           /* [한국어] memory_domain 정보 dump 시 spdk_memory_domain_* 헬퍼 사용 */

#include "spdk/log.h"           /* [한국어] SPDK_ERRLOG/NOTICELOG/SPDK_LOG_DEPRECATION_REGISTER 사용 */

#include "bdev_internal.h"      /* [한국어] bdev_alloc_io_stat, bdev_reset_device_stat 등 lib/bdev 내부 헬퍼 */

/* [한국어] enable_histogram 의 min_nsec 기본값 — 1us. 1us 미만 latency 는 첫 버킷에 누적된다.
 *         Histogram 의 lower bound 로 사용되며 사용자가 min_nsec 을 안 주면 이 값이 적용. */
#define SPDK_BDEV_HISTOGRAM_DEFAULT_MIN_VALUE_NS (1000)
/* [한국어] enable_histogram 의 max_nsec 기본값 — 120s. 120s 초과 latency 는 마지막 버킷에 포화 누적.
 *         스토리지 latency 가 분 단위에 도달하면 사실상 stuck I/O 라는 운용적 가정. */
#define SPDK_BDEV_HISTOGRAM_DEFAULT_MAX_VALUE_NS (120000000000)
/* [한국어] bdev_get_iostat 의 "names" 배열로 받을 수 있는 최대 bdev 이름 개수.
 *         JSON params 길이 제한과 콜백 fan-out 비용을 고려한 안전 상한. */
#define SPDK_BDEV_MAX_GET_IOSTAT_BDEV_NAMES (1024)

/*
 * [한국어]
 * dummy_bdev_event_cb - bdev 이벤트 콜백의 no-op 구현
 *
 * @type: 이벤트 종류 (REMOVE, RESIZE 등) — 무시.
 * @bdev: 이벤트 발생 대상 bdev — 무시.
 * @ctx:  open 시 등록한 콜백 context — 무시.
 * @return: 없음.
 *
 * spdk_bdev_open_ext() 는 bdev 가 사라지거나 크기가 바뀌는 등의 이벤트를 통보받기 위한
 * event_cb 를 필수로 요구한다. 이 파일의 RPC 들은 "잠깐 열고 통계 한번 조회한 뒤 즉시 닫는"
 * short-lived 사용 패턴이라 long-lived 이벤트를 받을 필요가 없다. 따라서 시그니처만 만족하는
 * 빈 콜백을 등록한다.
 *
 * 실행 컨텍스트: 보통 호출되지 않음. 호출되면 SPDK app thread 에서 실행.
 *
 * 호출 체인:
 *   spdk_bdev_open_ext(... dummy_bdev_event_cb ...) → [이 함수] (이벤트 발생 시)
 */
static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
	/* [한국어] 의도적 no-op — 이벤트를 무시하고 즉시 반환. */
}

/* [한국어] bdev_set_options RPC 의 JSON 입력 디코더 테이블.
 *         {필드명, struct spdk_bdev_opts 내 오프셋, 디코더, optional 여부} 튜플.
 *         모든 필드 optional(true): 사용자가 일부만 줘도 spdk_bdev_get_opts() 로
 *         미리 채워둔 현재 값이 유지되도록 한다. */
static const struct spdk_json_object_decoder rpc_bdev_set_options_decoders[] = {
	/* [한국어] 글로벌 bdev_io 풀 크기(이 노드 전체에서 공유). 너무 작으면 I/O 가 큐잉돼 latency 증가.
	 *         읽는 자: spdk_bdev_set_opts() → bdev_io_pool. */
	{"bdev_io_pool_size", offsetof(struct spdk_bdev_opts, bdev_io_pool_size), spdk_json_decode_uint32, true},
	/* [한국어] per-thread bdev_io 캐시 크기. lockless 빠른 경로를 위해 풀에서 미리 가져온 양.
	 *         bdev_io_pool_size 보다 크면 spdk_bdev_set_opts() 가 -EINVAL 반환. */
	{"bdev_io_cache_size", offsetof(struct spdk_bdev_opts, bdev_io_cache_size), spdk_json_decode_uint32, true},
	/* [한국어] true 면 bdev 등록 시 examine 모듈들이 자동 실행. false 면 수동 bdev_examine 필요.
	 *         프로비저닝 자동화 유무를 전환할 때 사용. */
	{"bdev_auto_examine", offsetof(struct spdk_bdev_opts, bdev_auto_examine), spdk_json_decode_bool, true},
	/* [한국어] iobuf small 캐시 크기 — bdev I/O 의 bounce buffer/data buffer 풀의 코어별 캐시.
	 *         값이 클수록 메모리 사용량 ↑ , 빠른 경로 hit ↑. */
	{"iobuf_small_cache_size", offsetof(struct spdk_bdev_opts, iobuf_small_cache_size), spdk_json_decode_uint32, true},
	/* [한국어] iobuf large 캐시 크기 — large bdev I/O (예: 128KB 이상) 용. small 과 분리되어 운영. */
	{"iobuf_large_cache_size", offsetof(struct spdk_bdev_opts, iobuf_large_cache_size), spdk_json_decode_uint32, true},
};

/*
 * [한국어]
 * rpc_bdev_set_options - bdev 글로벌 옵션 설정 RPC 핸들러
 *
 * @request: jsonrpc 응답을 보낼 요청 객체 (lib/jsonrpc 가 소유, 응답 후 free).
 * @params:  JSON params (선택). 필드: bdev_io_pool_size, bdev_io_cache_size,
 *           bdev_auto_examine, iobuf_small_cache_size, iobuf_large_cache_size 모두 optional.
 * @return:  없음. 결과는 JSON-RPC 로 비동기(여기선 동기적) 송신.
 *
 * 이 RPC 는 bdev 서브시스템이 init 되기 *전에* 호출되어야 한다 — phase 가 SPDK_RPC_STARTUP 인 이유.
 * 이미 init 된 뒤에는 bdev_io 풀이 만들어졌으므로 변경이 불가능하다.
 * 동작 단계:
 *   1) 현재 옵션을 spdk_bdev_get_opts() 로 가져와 디폴트 채움.
 *   2) JSON params 가 있으면 디코더로 덮어쓰기. 디코드 실패 시 INVALID_PARAMS 응답.
 *   3) spdk_bdev_set_opts() 호출. 실패(예: pool < cache) 시 INVALID_PARAMS + 메시지.
 *   4) 성공 응답(true) 발송.
 *
 * 실행 컨텍스트: SPDK app thread (메인 reactor). 동기적으로 응답.
 *
 * 호출 체인:
 *   jsonrpc dispatcher → [rpc_bdev_set_options] → spdk_bdev_get_opts/set_opts
 *                                              → spdk_jsonrpc_send_(error_)response
 */
static void
rpc_bdev_set_options(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_bdev_opts opts;  /* [한국어] 디코드 대상 — 5개 필드를 가진 옵션 구조체. */
	int rc;                       /* [한국어] spdk_bdev_set_opts() 결과 코드. */

	/* [한국어] 현재 디폴트/이전 설정을 채워서 일부 필드만 덮어쓰기 가능하게 한다.
	 *         sizeof(opts) 를 넘기는 것은 SPDK ABI 호환을 위한 size-prefixed 패턴. */
	spdk_bdev_get_opts(&opts, sizeof(opts));
	/* [한국어] params == NULL 이면 디폴트만 적용. 즉 "현재 값으로 다시 설정" 효과. */
	if (params != NULL) {
		/* [한국어] JSON 객체를 decoders 테이블에 따라 opts 에 매핑. 알 수 없는 키나 타입 불일치 시 nonzero. */
		if (spdk_json_decode_object(params, rpc_bdev_set_options_decoders,
					    SPDK_COUNTOF(rpc_bdev_set_options_decoders), &opts)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");  /* [한국어] 디코드 실패 로그. */
			/* [한국어] 표준 JSON-RPC 에러 코드 INVALID_PARAMS(-32602) 응답 후 종료. */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;  /* [한국어] 응답을 보냈으므로 즉시 반환 — request 객체는 lib/jsonrpc 가 free. */
		}
	}

	/* [한국어] 실제 적용. pool < cache 같은 invariant 위반 시 -EINVAL 반환. */
	rc = spdk_bdev_set_opts(&opts);
	if (rc != 0) {
		/* [한국어] 사용자가 어떤 값으로 실패했는지 알리는 fmt 메시지. */
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Pool size %" PRIu32 " too small for cache size %" PRIu32,
						     opts.bdev_io_pool_size, opts.bdev_io_cache_size);
		return;  /* [한국어] 실패 응답 후 종료. */
	}

	/* [한국어] 성공 시 단순 boolean true 응답 — JSON 형식: {"result":true}. */
	spdk_jsonrpc_send_bool_response(request, true);
}
/* [한국어] dispatch 등록: "bdev_set_options" 메서드를 STARTUP phase 에 핸들러 매핑.
 *         STARTUP 은 spdk_app_start() 후 RPC 서비스가 RUNTIME 으로 전환되기 전 (bdev init 전)에만 받는다. */
SPDK_RPC_REGISTER("bdev_set_options", rpc_bdev_set_options, SPDK_RPC_STARTUP)

/*
 * [한국어]
 * rpc_bdev_wait_for_examine_cpl - examine 대기 완료 콜백
 *
 * @arg: spdk_bdev_wait_for_examine() 에 ctx 로 넘긴 spdk_jsonrpc_request* (cast back).
 * @return: 없음.
 *
 * lib/bdev 가 모든 진행 중 examine 작업이 끝났다고 통보할 때 호출된다.
 * 단순히 클라이언트에 true 응답을 보낸다.
 *
 * 실행 컨텍스트: SPDK app thread (lib/bdev 가 메시지 hop 으로 보장).
 *
 * 호출 체인:
 *   spdk_bdev_wait_for_examine 내부 → [이 콜백] → spdk_jsonrpc_send_bool_response
 */
static void
rpc_bdev_wait_for_examine_cpl(void *arg)
{
	struct spdk_jsonrpc_request *request = arg;  /* [한국어] void* 로 보존했던 RPC 요청 핸들 복원. */

	/* [한국어] true 응답 → 모든 examine 완료를 클라이언트에 통보. */
	spdk_jsonrpc_send_bool_response(request, true);
}

/*
 * [한국어]
 * rpc_bdev_wait_for_examine - 진행 중인 examine 모두 끝날 때까지 비동기 대기 RPC
 *
 * @request: 응답 송신용 요청.
 * @params:  반드시 NULL 이어야 함. 어떤 파라미터도 받지 않는다.
 * @return:  없음. 응답은 콜백에서 비동기 전송.
 *
 * bdev_examine() 는 즉시 반환하지만 실제 검사(lvol/gpt 등)는 비동기로 진행된다.
 * 클라이언트(특히 자동화 스크립트)가 "examine 끝난 뒤에 다음 단계를 실행"하고 싶을 때 사용.
 * 동작:
 *   1) params 가 있으면 INVALID_PARAMS 응답 후 종료.
 *   2) spdk_bdev_wait_for_examine(cpl_cb, request) 호출. 모든 examine 완료 시 cpl_cb 호출.
 *   3) 즉시 반환 — RPC 응답은 콜백에서.
 *   4) 등록 자체가 실패하면(rc < 0) 에러 응답.
 *
 * 실행 컨텍스트: SPDK app thread.
 *
 * 호출 체인:
 *   jsonrpc dispatcher → [이 함수] → spdk_bdev_wait_for_examine
 *                              → (나중에) rpc_bdev_wait_for_examine_cpl → 응답.
 */
static void
rpc_bdev_wait_for_examine(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	int rc;  /* [한국어] spdk_bdev_wait_for_examine 등록 결과. */

	/* [한국어] 이 RPC 는 무인자다 — 사용자가 아무 객체나 보내면 거부. */
	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "bdev_wait_for_examine requires no parameters");
		return;
	}

	/* [한국어] 콜백 등록. 0 이면 examine 끝날 때 콜백이 호출되며 그때 응답.
	 *         음수면 즉시 실패 (예: 콜백 큐 풀). */
	rc = spdk_bdev_wait_for_examine(rpc_bdev_wait_for_examine_cpl, request);
	if (rc != 0) {
		/* [한국어] 등록 실패 — errno 부호화된 rc 를 메시지와 함께 전달. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
	/* [한국어] 성공 시 응답은 cpl 콜백이 보낸다 — 이 함수는 즉시 반환. */
}
/* [한국어] RUNTIME phase RPC: bdev init 이 끝난 뒤(어플리케이션 정상 동작 중) 호출 가능. */
SPDK_RPC_REGISTER("bdev_wait_for_examine", rpc_bdev_wait_for_examine, SPDK_RPC_RUNTIME)

/* [한국어] bdev_examine RPC 의 입력 구조체 — 검사할 단일 bdev 이름 하나. */
struct rpc_bdev_examine {
	char *name;
	/* [한국어] examine 대상 bdev 의 이름.
	 *         설정자: spdk_json_decode_string 이 strdup 으로 동적 할당.
	 *         읽는 자: spdk_bdev_examine() 가 글로벌 bdev 리스트에서 lookup.
	 *         값 범위: NUL-terminated 문자열, NULL 이면 디코드 실패.
	 *         동기화: 핸들러 함수 내 stack-local rpc_bdev_examine 인스턴스에서만 접근. */
};

/*
 * [한국어]
 * free_rpc_bdev_examine - rpc_bdev_examine 인스턴스의 동적 자원 해제
 *
 * @r: 해제 대상 (caller 의 stack-local 변수).
 * @return: 없음.
 *
 * spdk_json_decode_string 은 strdup 으로 새 메모리를 할당하므로 핸들러 종료 시 명시적 free 가 필요.
 * 디코드 실패/성공과 무관하게 cleanup 라벨에서 호출된다.
 *
 * 호출 체인:
 *   rpc_bdev_examine cleanup → [이 함수] → free(3)
 */
static void
free_rpc_bdev_examine(struct rpc_bdev_examine *r)
{
	free(r->name);  /* [한국어] strdup 된 name 해제. NULL 이어도 free(NULL) 은 안전. */
}

/* [한국어] bdev_examine RPC 의 디코더 테이블 — name 1개 (필수, optional 플래그 false 기본). */
static const struct spdk_json_object_decoder rpc_bdev_examine_decoders[] = {
	/* [한국어] 검사 대상 bdev 이름. 필수 — 안 주면 디코드 실패 → INVALID_PARAMS. */
	{"name", offsetof(struct rpc_bdev_examine, name), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_examine - 특정 bdev 에 대해 examine 트리거 RPC
 *
 * @request: 응답 송신용 요청.
 * @params:  {"name": "<bdev_name>"} JSON 객체.
 * @return:  없음. 동기적으로 응답.
 *
 * "examine" 은 등록된 bdev 위에서 lvol/gpt 같은 examine 모듈이 검사해 자식 bdev (lvol_store,
 * 파티션 등) 를 자동 생성하는 SPDK 의 디스커버리 메커니즘이다.
 * bdev_auto_examine 옵션을 false 로 두고 명시적으로 트리거하고 싶을 때 사용.
 * 동작:
 *   1) params 디코드 → req.name 채움.
 *   2) spdk_bdev_examine(name) 호출. 비동기 검사를 시작하고 즉시 반환(rc=0) 또는 동기 실패(rc<0).
 *   3) 호출 자체의 즉시 결과만 응답 — 검사 완료는 bdev_wait_for_examine 으로 별도 대기.
 *   4) cleanup 라벨에서 동적 메모리 해제.
 *
 * 실행 컨텍스트: SPDK app thread.
 *
 * 호출 체인:
 *   jsonrpc dispatcher → [이 함수] → spdk_bdev_examine → (백그라운드) examine 모듈 fn_table->examine_*
 */
static void
rpc_bdev_examine(struct spdk_jsonrpc_request *request,
		 const struct spdk_json_val *params)
{
	struct rpc_bdev_examine req = {NULL};  /* [한국어] zero-init: name=NULL — 디코드 실패 시에도 안전 free. */
	int rc;                                  /* [한국어] spdk_bdev_examine 결과. */

	/* [한국어] JSON params 디코드. 실패 시 cleanup 으로 점프해 메모리 해제 후 반환. */
	if (spdk_json_decode_object(params, rpc_bdev_examine_decoders,
				    SPDK_COUNTOF(rpc_bdev_examine_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] 실제 examine 트리거. lib/bdev 가 examine_disk()/examine_config() 콜백 체인을 비동기 시작.
	 *         반환값은 등록 단계의 즉시 실패(예: bdev 없음)만 반영. */
	rc = spdk_bdev_examine(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	/* [한국어] examine 호출이 등록됐다는 의미의 즉시 성공 응답.
	 *         실제 검사 완료까지 기다리려면 bdev_wait_for_examine 별도 호출. */
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_examine(&req);  /* [한국어] 어느 경로든 strdup 된 name 해제 보장. */
}
SPDK_RPC_REGISTER("bdev_examine", rpc_bdev_examine, SPDK_RPC_RUNTIME)

/* [한국어] get_iostat 의 응답 fan-out 컨텍스트 — bdev 여러 개를 비동기 병렬 조회하면서
 *         완료 카운트가 0 이 될 때 한꺼번에 응답을 닫기 위한 누적기. */
struct rpc_get_iostat_ctx {
	int bdev_count;
	/* [한국어] 응답을 기다리는 미완 bdev 수.
	 *         설정자: 핸들러가 처음 1로 초기화(가드)한 뒤 각 bdev 시작 시 ++.
	 *         읽는 자: rpc_get_iostat_done 이 -- 후 0이면 응답 완료 처리.
	 *         값 범위: 양의 정수. 가드 1 덕분에 iter 도중 0이 되어 조기 send 되는 일이 없다.
	 *         동기화: SPDK app thread 단일 스레드 — 락 불필요. */

	int rc;
	/* [한국어] 누적 에러 코드. 첫 에러를 보존하고 이후 에러는 무시.
	 *         설정자: 각 bdev 콜백에서 if(rpc_ctx->rc==0) rpc_ctx->rc = rc;
	 *         읽는 자: rpc_get_iostat_done 에서 0 이면 정상 응답, 아니면 에러 응답.
	 *         값 범위: 0 또는 errno 음수.
	 *         동기화: app thread 단일. */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답을 보낼 RPC 요청 객체.
	 *         설정자: 핸들러가 RPC 진입 시 보존.
	 *         읽는 자: 응답 빌드/에러 응답 시 lib/jsonrpc 에 전달.
	 *         값 범위: 유효 핸들 (NULL 불가).
	 *         동기화: 응답 후 lib/jsonrpc 가 단독 소유 — 그 이후 접근 금지. */

	struct spdk_json_write_ctx *w;
	/* [한국어] JSON 응답 직렬화 writer 핸들.
	 *         설정자: rpc_get_iostat_started() 가 spdk_jsonrpc_begin_result 로 생성.
	 *         읽는 자: per-bdev 콜백이 객체/필드를 누적 작성.
	 *         값 범위: 유효 writer 또는 NULL(에러 경로에선 미생성).
	 *         동기화: app thread 단일. */

	bool per_channel;
	/* [한국어] true 면 채널별 통계로 응답하라는 클라이언트 요청.
	 *         설정자: 핸들러가 req.per_channel 로 초기화.
	 *         읽는 자: bdev_get_iostat 가 spdk_bdev_for_each_channel 분기에 사용.
	 *         값 범위: true/false. names.count == 1 일 때만 허용.
	 *         동기화: 핸들러 시작 후 read-only. */

	enum spdk_bdev_reset_stat_mode reset_mode;
	/* [한국어] 통계 조회 시 어떤 카운터를 리셋할지 모드.
	 *         설정자: 핸들러가 req.reset_mode (디폴트 NONE).
	 *         읽는 자: spdk_bdev_get_device_stat 와 per-channel iter 함수.
	 *         값 범위: ALL/MAXMIN/ERROR/NONE — rpc_decode_reset_iostat_mode 참고.
	 *         동기화: 핸들러 시작 후 read-only. */
};

/* [한국어] 한 개 bdev 단위 fan-out 컨텍스트 — 글로벌 rpc_ctx 와 per-bdev 자원을 묶음. */
struct bdev_get_iostat_ctx {
	struct spdk_bdev_io_stat *stat;
	/* [한국어] 이 bdev 의 통계 누적 버퍼.
	 *         설정자: bdev_iostat_ctx_alloc() 가 bdev_alloc_io_stat 로 할당.
	 *         읽는 자: spdk_bdev_get_device_stat 가 콜백에 같은 포인터 전달, dump_io_stat_json 이 직렬화.
	 *         값 범위: 유효 포인터 (alloc 실패 시 ctx 자체가 NULL).
	 *         동기화: 한 ctx = 한 콜백 — 단일 스레드 접근. */

	struct rpc_get_iostat_ctx *rpc_ctx;
	/* [한국어] 상위 fan-out 컨텍스트 back-pointer.
	 *         설정자: 핸들러가 bdev 별 ctx 만들면서 연결.
	 *         읽는 자: 콜백이 응답 누적/완료 시 사용.
	 *         값 범위: 유효 포인터.
	 *         동기화: app thread 단일. */

	struct spdk_bdev_desc *desc;
	/* [한국어] open 한 bdev descriptor — 콜백 후 close 해야 함.
	 *         설정자: spdk_bdev_open_ext 결과.
	 *         읽는 자: bdev_get_iostat_done 이 spdk_bdev_close 로 해제.
	 *         값 범위: 유효 desc 또는 (open 실패 경로에선) 미초기화 — 그 경로에선 ctx 즉시 free.
	 *         동기화: app thread 단일. */
};

/*
 * [한국어]
 * rpc_get_iostat_started - get_iostat 응답 JSON 의 헤더 시작
 *
 * @rpc_ctx: 글로벌 fan-out 컨텍스트 (request, w 채워짐).
 * @return: 없음.
 *
 * fan-out 응답의 시작점 — 객체 begin, tick_rate/ticks 메타필드 작성.
 * 클라이언트는 tick_rate 와 ticks 로 통계의 시간 기준점을 환산할 수 있다.
 * 호출 시점: per_channel 분기에서는 bdev_get_iostat() 1회, non-per_channel 에서는
 * rpc_bdev_get_iostat() 마무리에서 1회.
 *
 * 호출 체인:
 *   rpc_bdev_get_iostat 또는 bdev_get_iostat → [이 함수] → spdk_jsonrpc_begin_result
 */
static void
rpc_get_iostat_started(struct rpc_get_iostat_ctx *rpc_ctx)
{
	/* [한국어] 응답 빌드 시작 — JSON writer 획득. 이후 모든 spdk_json_write_* 는 이 w 에 누적. */
	rpc_ctx->w = spdk_jsonrpc_begin_result(rpc_ctx->request);

	spdk_json_write_object_begin(rpc_ctx->w);  /* [한국어] 응답 최상위 객체 '{' 시작. */
	/* [한국어] tick_rate(=Hz) — 클라이언트가 tsc 기반 카운터를 초로 환산하는 데 필요. */
	spdk_json_write_named_uint64(rpc_ctx->w, "tick_rate", spdk_get_ticks_hz());
	/* [한국어] 현재 tsc 값 — 통계 스냅샷 시각 기준. */
	spdk_json_write_named_uint64(rpc_ctx->w, "ticks", spdk_get_ticks());
}

/*
 * [한국어]
 * rpc_get_iostat_done - get_iostat fan-out 종결 처리
 *
 * @rpc_ctx: 글로벌 fan-out 컨텍스트.
 * @return: 없음.
 *
 * 각 bdev 콜백 끝과 핸들러 마지막에서 호출. bdev_count 가 0 이 되는 마지막 호출에서
 * 실제 응답 발송. 그 이전 호출은 카운터 감소만 하고 반환.
 *
 * 동작:
 *   1) bdev_count 감소.
 *   2) 0 이 아니면 아직 미완 bdev 가 남아있음 → 반환.
 *   3) rc==0 면 array_end → object_end → end_result 로 응답 마무리.
 *   4) rc!=0 면 그동안의 응답 builder 를 폐기하고 에러 응답.
 *   5) rpc_ctx free.
 *
 * 실행 컨텍스트: SPDK app thread (모든 bdev 콜백이 hop 후 여기로 합류).
 *
 * 호출 체인:
 *   bdev_get_iostat_done / bdev_get_per_channel_stat_done / 핸들러 마지막 → [이 함수]
 */
static void
rpc_get_iostat_done(struct rpc_get_iostat_ctx *rpc_ctx)
{
	/* [한국어] pre-decrement: 응답 미완 bdev 가 남아있으면 즉시 반환 — 마지막 한번만 send 한다. */
	if (--rpc_ctx->bdev_count != 0) {
		return;
	}

	if (rpc_ctx->rc == 0) {
		spdk_json_write_array_end(rpc_ctx->w);   /* [한국어] "bdevs"/"channels" 배열 ']' 닫기. */
		spdk_json_write_object_end(rpc_ctx->w);  /* [한국어] 최상위 객체 '}' 닫기. */
		spdk_jsonrpc_end_result(rpc_ctx->request, rpc_ctx->w);  /* [한국어] 응답 직렬화 → 클라이언트 송신. */
	} else {
		/* Return error response after processing all specified bdevs
		 * completed or failed.
		 */
		/* [한국어] 한 bdev 라도 실패했으면 응답 전체를 에러로 통일 — partial success 안 만든다. */
		spdk_jsonrpc_send_error_response(rpc_ctx->request, rpc_ctx->rc,
						 spdk_strerror(-rpc_ctx->rc));
	}

	free(rpc_ctx);  /* [한국어] fan-out 컨텍스트 자체 해제. */
}

/*
 * [한국어]
 * bdev_iostat_ctx_alloc - per-bdev iostat 컨텍스트 할당
 *
 * @iostat_ext: true 면 확장 통계 (per io_type breakdown 등) 도 할당.
 * @return: 성공 시 컨텍스트 포인터, 실패 시 NULL (호출자가 -ENOMEM 처리).
 *
 * calloc + bdev_alloc_io_stat 두 단계 할당. 두번째 단계 실패 시 첫번째도 롤백한다.
 *
 * 호출 체인:
 *   bdev_get_iostat → [이 함수] → calloc, bdev_alloc_io_stat
 */
static struct bdev_get_iostat_ctx *
bdev_iostat_ctx_alloc(bool iostat_ext)
{
	struct bdev_get_iostat_ctx *ctx;  /* [한국어] 반환할 컨텍스트. */

	ctx = calloc(1, sizeof(struct bdev_get_iostat_ctx));  /* [한국어] zero-init 으로 desc/stat 등 NULL 초기화. */
	if (ctx == NULL) {
		return NULL;  /* [한국어] OOM — 호출자가 -ENOMEM 매핑. */
	}

	/* [한국어] iostat_ext 가 true 면 큰 stat 구조체(per io_type) 할당. */
	ctx->stat = bdev_alloc_io_stat(iostat_ext);
	if (ctx->stat == NULL) {
		free(ctx);    /* [한국어] 두번째 alloc 실패 시 첫번째 롤백 — 누수 방지. */
		return NULL;
	}

	return ctx;  /* [한국어] 정상 — 호출자가 desc/rpc_ctx 채워서 사용. */
}

/*
 * [한국어]
 * bdev_iostat_ctx_free - per-bdev iostat 컨텍스트 해제
 *
 * @ctx: 해제 대상 (NULL 금지 — 호출자 책임).
 * @return: 없음.
 *
 * stat 버퍼와 컨텍스트 자체를 분리 해제. desc 는 별도(콜백에서 spdk_bdev_close)로 닫는다.
 *
 * 호출 체인:
 *   bdev_get_iostat_done → [이 함수] → bdev_free_io_stat, free
 */
static void
bdev_iostat_ctx_free(struct bdev_get_iostat_ctx *ctx)
{
	bdev_free_io_stat(ctx->stat);  /* [한국어] 통계 버퍼 해제 (alloc 과 짝). */
	free(ctx);                      /* [한국어] 컨텍스트 자체 해제. */
}

/*
 * [한국어]
 * bdev_get_iostat_done - 단일 bdev 통계 조회 완료 콜백 (디바이스 전체 통계 경로)
 *
 * @bdev:   대상 bdev.
 * @stat:   누적된 통계 (bdev_ctx->stat 와 동일 포인터, assert 로 확인).
 * @cb_arg: bdev_get_iostat_ctx*.
 * @rc:     0 또는 errno.
 * @return: 없음.
 *
 * spdk_bdev_get_device_stat() 의 콜백. 응답 JSON 에 한 bdev 객체를 작성하고 fan-out 카운터 감소.
 * driver_specific 통계는 bdev->fn_table->dump_device_stat_json 콜백이 있으면 위임 호출.
 *
 * 실행 컨텍스트: SPDK app thread (lib/bdev 가 메시지 hop 으로 보장 — assert).
 *
 * 호출 체인:
 *   spdk_bdev_get_device_stat → ... → [이 함수] → 응답 JSON 작성 → rpc_get_iostat_done
 */
static void
bdev_get_iostat_done(struct spdk_bdev *bdev, struct spdk_bdev_io_stat *stat,
		     void *cb_arg, int rc)
{
	struct bdev_get_iostat_ctx *bdev_ctx = cb_arg;          /* [한국어] per-bdev 컨텍스트 복원. */
	struct rpc_get_iostat_ctx *rpc_ctx = bdev_ctx->rpc_ctx; /* [한국어] 글로벌 fan-out 컨텍스트. */
	struct spdk_json_write_ctx *w = rpc_ctx->w;             /* [한국어] 응답 writer (편의 별칭). */

	/* [한국어] 콜백이 반드시 app_thread 에서 실행돼야 응답 빌드가 안전. */
	assert(spdk_thread_is_app_thread(NULL));

	/* [한국어] 이번 콜백에서 에러였거나 이전에 누가 에러였으면 — JSON 누적을 건너뛰고 done 으로. */
	if (rc != 0 || rpc_ctx->rc != 0) {
		if (rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;  /* [한국어] 첫 에러만 보존. */
		}
		goto done;
	}

	/* [한국어] 콜백이 다른 stat 포인터를 넘기면 ctx 미스매치 — sanity check. */
	assert(stat == bdev_ctx->stat);

	spdk_json_write_object_begin(w);  /* [한국어] 한 bdev 의 응답 객체 '{' 시작. */

	/* [한국어] bdev 식별자 — 클라이언트가 매칭할 키. */
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(bdev));

	/* [한국어] read/write count, bytes, error count 등 표준 통계 필드를 일괄 작성 (lib/bdev 헬퍼). */
	spdk_bdev_dump_io_stat_json(stat, w);

	/* [한국어] qd_sampling_period 가 활성화돼 있으면 큐깊이 관련 4개 필드 추가 출력. */
	if (spdk_bdev_get_qd_sampling_period(bdev)) {
		spdk_json_write_named_uint64(w, "queue_depth_polling_period",
					     spdk_bdev_get_qd_sampling_period(bdev));  /* [한국어] tsc tick 단위. */

		spdk_json_write_named_uint64(w, "queue_depth", spdk_bdev_get_qd(bdev));  /* [한국어] 최근 표본 큐깊이. */

		spdk_json_write_named_uint64(w, "io_time", spdk_bdev_get_io_time(bdev));  /* [한국어] 활성 I/O 시간 누계. */

		spdk_json_write_named_uint64(w, "weighted_io_time",
					     spdk_bdev_get_weighted_io_time(bdev));  /* [한국어] qd 가중 I/O 시간 — utilization 계산용. */
	}

	/* [한국어] 모듈별(driver_specific) 통계 hook — NVMe/AIO/Lvol 등이 자기 특화 카운터 직렬화. */
	if (bdev->fn_table->dump_device_stat_json) {
		spdk_json_write_named_object_begin(w, "driver_specific");
		bdev->fn_table->dump_device_stat_json(bdev->ctxt, w);  /* [한국어] 드라이버 콜백에 writer 전달. */
		spdk_json_write_object_end(w);
	}

	spdk_json_write_object_end(w);  /* [한국어] 한 bdev 객체 '}' 닫기. */

done:
	rpc_get_iostat_done(rpc_ctx);  /* [한국어] fan-out 카운터 감소 → 마지막이면 응답 발송. */

	spdk_bdev_close(bdev_ctx->desc);  /* [한국어] open_ext 와 짝 — bdev 참조 해제. */
	bdev_iostat_ctx_free(bdev_ctx);   /* [한국어] per-bdev 컨텍스트 free. */
}

/*
 * [한국어]
 * bdev_get_per_channel_stat_done - 채널별 통계 iter 완료 콜백
 *
 * @bdev:   대상 bdev (per_channel 모드는 단일 bdev).
 * @ctx:    bdev_get_iostat_ctx*.
 * @status: 0 또는 errno (성공 여부).
 * @return: 없음.
 *
 * spdk_bdev_for_each_channel() 가 모든 reactor 채널을 순회하고 마지막 한 곳에서 호출하는 콜백.
 * 채널별 객체는 이미 bdev_get_per_channel_stat 에서 누적 작성됐고, 여기선 fan-out 종료만.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel → ... → [이 함수] → rpc_get_iostat_done
 */
static void
bdev_get_per_channel_stat_done(struct spdk_bdev *bdev, void *ctx, int status)
{
	struct bdev_get_iostat_ctx *bdev_ctx = ctx;  /* [한국어] per-bdev 컨텍스트 복원. */

	/* [한국어] fan-out 카운터 감소 → 0이 되면 응답 송신.
	 *         status 처리는 (단순화 위해) 별도 안 함 — per-channel 은 단일 bdev 라 이미 응답 시작됨. */
	rpc_get_iostat_done(bdev_ctx->rpc_ctx);

	spdk_bdev_close(bdev_ctx->desc);  /* [한국어] open_ext 와 짝. */

	bdev_iostat_ctx_free(bdev_ctx);   /* [한국어] per-bdev 컨텍스트 해제. */
}

/*
 * [한국어]
 * bdev_get_per_channel_stat - 각 채널(=각 reactor) 에서 호출되는 per-channel iter 함수
 *
 * @i:    채널 iterator (continue 호출에 사용).
 * @bdev: 대상 bdev.
 * @ch:   현재 reactor 의 io_channel.
 * @ctx:  bdev_get_iostat_ctx*.
 * @return: 없음.
 *
 * spdk_bdev_for_each_channel() 가 각 reactor 로 메시지 hop 시켜 호출. 채널 단위 통계를 stat
 * 버퍼에 갱신하고 응답 JSON 의 channels 배열에 한 객체 추가. 마지막에 _continue 로 다음 채널 진행.
 *
 * 실행 컨텍스트: 각 채널 소유 reactor (= 각 SPDK thread). 단, 응답 writer w 접근은
 * spdk_bdev_for_each_channel 이 직렬화(채널 hop 은 한번에 한곳)해 주므로 안전.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel → [이 함수] → spdk_bdev_get_io_stat → spdk_bdev_for_each_channel_continue
 */
static void
bdev_get_per_channel_stat(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			  struct spdk_io_channel *ch, void *ctx)
{
	struct bdev_get_iostat_ctx *bdev_ctx = ctx;                          /* [한국어] per-bdev 컨텍스트. */
	struct spdk_json_write_ctx *w = bdev_ctx->rpc_ctx->w;                /* [한국어] 응답 writer 별칭. */

	/* [한국어] 이 채널의 통계를 stat 으로 복사 — reset_mode 따라 카운터 리셋도 진행. */
	spdk_bdev_get_io_stat(bdev, ch, bdev_ctx->stat, bdev_ctx->rpc_ctx->reset_mode);

	spdk_json_write_object_begin(w);  /* [한국어] 채널 1개의 객체 '{' 시작. */
	/* [한국어] 어떤 SPDK thread(=reactor) 인지 식별하기 위한 thread_id. */
	spdk_json_write_named_uint64(w, "thread_id", spdk_thread_get_id(spdk_get_thread()));
	spdk_bdev_dump_io_stat_json(bdev_ctx->stat, w);  /* [한국어] 표준 통계 필드. */
	spdk_json_write_object_end(w);                    /* [한국어] 채널 객체 '}' 닫기. */

	/* [한국어] 다음 채널로 메시지 hop — status=0 으로 정상 진행. */
	spdk_bdev_for_each_channel_continue(i, 0);
}

/*
 * [한국어]
 * bdev_get_iostat - spdk_for_each_bdev 의 per-bdev 콜백 (또는 by_name 변형)
 *
 * @ctx:  rpc_get_iostat_ctx*.
 * @bdev: 한번에 하나씩 들어오는 bdev.
 * @return: 0 = 계속, 음수 = 중단.
 *
 * 한 bdev 마다 (a) per-bdev ctx 할당, (b) bdev open, (c) per_channel 여부에 따라
 * for_each_channel() 또는 get_device_stat() 시작. 모두 비동기로 결과는 콜백에 모인다.
 *
 * 호출 체인:
 *   spdk_for_each_bdev / spdk_for_each_bdev_by_name → [이 함수] → spdk_bdev_open_ext
 *                                                                → spdk_bdev_for_each_channel 또는 spdk_bdev_get_device_stat
 */
static int
bdev_get_iostat(void *ctx, struct spdk_bdev *bdev)
{
	struct rpc_get_iostat_ctx *rpc_ctx = ctx;     /* [한국어] 글로벌 fan-out 컨텍스트. */
	struct bdev_get_iostat_ctx *bdev_ctx;          /* [한국어] 이 bdev 전용 컨텍스트. */
	int rc;                                         /* [한국어] open 결과. */

	/* [한국어] iostat_ext=true — io_type 별 breakdown 까지 수집. */
	bdev_ctx = bdev_iostat_ctx_alloc(true);
	if (bdev_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate bdev_iostat_ctx struct\n");
		return -ENOMEM;  /* [한국어] iter 중단 → 핸들러가 rpc_ctx->rc 에 보존. */
	}

	/* [한국어] read-only(2번째 false) 로 open. event_cb 는 dummy — 통계만 짧게 보고 닫는다. */
	rc = spdk_bdev_open_ext(spdk_bdev_get_name(bdev), false, dummy_bdev_event_cb, NULL,
				&bdev_ctx->desc);
	if (rc != 0) {
		bdev_iostat_ctx_free(bdev_ctx);
		SPDK_ERRLOG("Failed to open bdev\n");
		return rc;
	}

	rpc_ctx->bdev_count++;             /* [한국어] fan-out 카운터 ↑ — 콜백이 응답 결합 책임. */
	bdev_ctx->rpc_ctx = rpc_ctx;       /* [한국어] back-pointer 연결. */

	if (rpc_ctx->per_channel) {
		/* bdev_count equals 2 because of initial increment */
		/* [한국어] per_channel 은 단일 bdev 만 허용 — 가드 1 + 본 bdev 1 = 2. */
		assert(rpc_ctx->bdev_count == 2 && "we support per_channel only for single bdev");
		rpc_get_iostat_started(rpc_ctx);  /* [한국어] 응답 헤더(tick_rate/ticks) 시작. */
		spdk_json_write_named_string(rpc_ctx->w, "name", spdk_bdev_get_name(bdev));  /* [한국어] bdev 이름. */
		spdk_json_write_named_array_begin(rpc_ctx->w, "channels");                    /* [한국어] 채널 배열 시작. */

		/* [한국어] 각 reactor 의 채널 hop — per_channel_stat 가 채널마다 호출, _done 이 마지막에 호출. */
		spdk_bdev_for_each_channel(bdev,
					   bdev_get_per_channel_stat,
					   bdev_ctx,
					   bdev_get_per_channel_stat_done);
	} else {
		/* [한국어] 디바이스 전체(모든 채널 합산) 통계 — 한번의 콜백으로 결과 통보. */
		spdk_bdev_get_device_stat(bdev, bdev_ctx->stat, rpc_ctx->reset_mode, bdev_get_iostat_done,
					  bdev_ctx);
	}

	return 0;  /* [한국어] 다음 bdev 로 iter 계속. */
}

/* [한국어] bdev_get_iostat 의 names 파라미터 전용 컨테이너 — 가변 길이 배열 + count 보존. */
struct rpc_bdev_get_iostat_names {
	size_t count;
	/* [한국어] names[] 의 유효 요소 수.
	 *         설정자: rpc_decode_iostat_bdev_names 가 spdk_json_decode_array 결과로 채움.
	 *         읽는 자: 핸들러가 0/UINT32_MAX/n 분기에 사용.
	 *         값 범위: 0 ~ SPDK_BDEV_MAX_GET_IOSTAT_BDEV_NAMES, 또는 미지정 sentinel UINT32_MAX.
	 *         동기화: 핸들러 stack-local — 단일 스레드. */

	char *names[SPDK_BDEV_MAX_GET_IOSTAT_BDEV_NAMES];
	/* [한국어] strdup 된 bdev 이름 배열 (최대 1024개).
	 *         설정자: spdk_json_decode_string 이 각 요소 strdup.
	 *         읽는 자: spdk_for_each_bdev_by_name 에 const char ** 캐스팅으로 전달.
	 *         값 범위: 유효 NUL-terminated 문자열 또는 NULL(미사용 슬롯).
	 *         동기화: 핸들러 stack-local. free_rpc_* 에서 일괄 free. */
};

/* [한국어] bdev_get_iostat 의 디코드 대상 — name(deprecated)/per_channel/reset_mode/names. */
struct rpc_bdev_get_iostat {
	char *name;
	/* [한국어] 단일 bdev 이름 (deprecated — names 배열 사용 권장).
	 *         설정자: spdk_json_decode_string.
	 *         읽는 자: 핸들러가 names.names[0] 으로 복사 후 names.count=1 로 전환.
	 *         값 범위: 문자열 또는 NULL.
	 *         동기화: 핸들러 stack-local. */

	bool per_channel;
	/* [한국어] 채널별 통계 요청 여부.
	 *         설정자: spdk_json_decode_bool.
	 *         읽는 자: 핸들러 → rpc_ctx->per_channel.
	 *         값 범위: true/false. true 면 names.count==1 강제.
	 *         동기화: 핸들러 stack-local. */

	enum spdk_bdev_reset_stat_mode reset_mode;
	/* [한국어] 통계 조회와 함께 어떤 카운터를 초기화할지.
	 *         설정자: rpc_decode_reset_iostat_mode.
	 *         읽는 자: 핸들러 → rpc_ctx->reset_mode.
	 *         값 범위: ALL/MAXMIN/ERROR/NONE — 디폴트 NONE.
	 *         동기화: 핸들러 stack-local. */

	struct rpc_bdev_get_iostat_names names;
	/* [한국어] 다중 bdev 이름 배열.
	 *         설정자: rpc_decode_iostat_bdev_names.
	 *         읽는 자: 핸들러가 spdk_for_each_bdev_by_name 에 전달.
	 *         값 범위: count==UINT32_MAX 면 미지정(=> 전체 bdev), 그 외엔 0..N.
	 *         동기화: 핸들러 stack-local. */
};

/*
 * [한국어]
 * free_rpc_bdev_get_iostat - rpc_bdev_get_iostat 동적 자원 해제
 *
 * @r: 대상 인스턴스.
 * @return: 없음.
 *
 * deprecated name 과 names 배열의 strdup 된 문자열을 모두 free.
 * names.count == UINT32_MAX 는 "디코드 안됨" sentinel — 이 경우 names[] 는 미초기화이므로 건너뜀.
 */
static void
free_rpc_bdev_get_iostat(struct rpc_bdev_get_iostat *r)
{
	size_t i = 0;  /* [한국어] iteration 인덱스. */

	free(r->name);  /* [한국어] deprecated name 해제. NULL safe. */
	if (r->names.count == UINT32_MAX) {
		/* No value was provided */
		/* [한국어] 디코더가 names 키를 보지 못해 sentinel 그대로 — names[] 미초기화이므로 종료. */
		return;
	}
	for (i = 0; i < r->names.count; i++) {
		free(r->names.names[i]);  /* [한국어] 각 strdup 해제. */
	}
}

/*
 * [한국어]
 * rpc_decode_reset_iostat_mode - reset_mode JSON 문자열을 enum 으로 변환
 *
 * @val: JSON 토큰 (문자열 기대).
 * @out: enum spdk_bdev_reset_stat_mode* (출력).
 * @return: 0 또는 -EINVAL.
 *
 * 4가지 키워드("all"/"maxmin"/"error"/"none") 중 하나를 enum 으로 매핑.
 * 매칭 안 되면 NOTICELOG 후 -EINVAL → 디코더가 연쇄 실패.
 *
 * 호출 체인:
 *   spdk_json_decode_object → 디코더 테이블 → [이 함수]
 */
static int
rpc_decode_reset_iostat_mode(const struct spdk_json_val *val, void *out)
{
	enum spdk_bdev_reset_stat_mode *mode = out;  /* [한국어] 출력 포인터 캐스팅. */

	/* [한국어] 모든 카운터 리셋 — 일반적인 "통계 다시 시작". */
	if (spdk_json_strequal(val, "all") == true) {
		*mode = SPDK_BDEV_RESET_STAT_ALL;
	/* [한국어] max/min latency 만 리셋 — 카운트는 누적 유지하면서 outlier 만 윈도우 갱신. */
	} else if (spdk_json_strequal(val, "maxmin") == true) {
		*mode = SPDK_BDEV_RESET_STAT_MAXMIN;
	/* [한국어] 에러 카운터만 리셋. */
	} else if (spdk_json_strequal(val, "error") == true) {
		*mode = SPDK_BDEV_RESET_STAT_ERROR;
	/* [한국어] 아무것도 리셋 안 함 — 단순 read-only 조회. */
	} else if (spdk_json_strequal(val, "none") == true) {
		*mode = SPDK_BDEV_RESET_STAT_NONE;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: mode\n");
		return -EINVAL;  /* [한국어] 디코더 체인 실패 → 핸들러가 INVALID_PARAMS 응답. */
	}

	return 0;
}

/*
 * [한국어]
 * rpc_decode_iostat_bdev_names - names 배열 디코더
 *
 * @val: JSON 배열 토큰.
 * @out: rpc_bdev_get_iostat_names*.
 * @return: 0 또는 음수.
 *
 * spdk_json_decode_array 래퍼로, 배열의 각 요소를 spdk_json_decode_string 으로 디코드.
 * 최대 SPDK_BDEV_MAX_GET_IOSTAT_BDEV_NAMES 까지 받고, names->count 에 실제 길이 기록.
 */
static int
rpc_decode_iostat_bdev_names(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_get_iostat_names *names = out;  /* [한국어] 출력 포인터 캐스팅. */

	/* [한국어] 배열 → strdup 된 char* 들을 names->names 에 저장. element_size = sizeof(char *). */
	return spdk_json_decode_array(val, spdk_json_decode_string, names->names,
				      SPDK_BDEV_MAX_GET_IOSTAT_BDEV_NAMES, &names->count, sizeof(char *));
}

/* [한국어] bdev_get_iostat 의 디코더 테이블. 모든 필드 optional — 무인자 호출 시 전체 bdev 조회. */
static const struct spdk_json_object_decoder rpc_bdev_get_iostat_decoders[] = {
	/* [한국어] 단일 이름(deprecated). 호환을 위해 유지하되 names 와 동시 사용 금지. */
	{"name", offsetof(struct rpc_bdev_get_iostat, name), spdk_json_decode_string, true},
	/* [한국어] 채널별 모드 토글. */
	{"per_channel", offsetof(struct rpc_bdev_get_iostat, per_channel), spdk_json_decode_bool, true},
	/* [한국어] 리셋 모드 — 사용자 정의 디코더로 enum 변환. */
	{"reset_mode", offsetof(struct rpc_bdev_get_iostat, reset_mode), rpc_decode_reset_iostat_mode, true},
	/* [한국어] 다중 이름 배열 — 사용자 정의 디코더. */
	{"names", offsetof(struct rpc_bdev_get_iostat, names), rpc_decode_iostat_bdev_names, true},
};

/* [한국어] deprecation 등록 — name 옵션은 v26.05 에 제거 예정. ALWAYS=매 호출마다 경고. */
SPDK_LOG_DEPRECATION_REGISTER(bdev_get_iostat_with_name,
			      "--name option for bdev_get_iostat is deprecated", "v26.05", SPDK_LOG_DEPRECATION_ALWAYS);

/*
 * [한국어]
 * rpc_bdev_get_iostat - I/O 통계 조회 RPC 핸들러
 *
 * @request: 응답 송신용 요청.
 * @params:  optional. {name?, per_channel?, reset_mode?, names?[]}
 * @return:  없음. 비동기 응답.
 *
 * 동작 단계:
 *   1) params 디코드. name+names 동시 지정은 거부.
 *   2) deprecated name 이 있으면 한번만 경고 후 names[0] 으로 promote.
 *   3) per_channel 인데 names.count != 1 이면 거부.
 *   4) rpc_ctx 할당 + 가드 카운터 1.
 *   5) names 가 있으면 by_name iter, 없으면 전체 iter — 각 bdev 마다 bdev_get_iostat 콜백.
 *   6) per_channel 가 아니면 응답 헤더(begin_result + tick_rate)와 bdevs 배열 시작.
 *      per_channel 면 bdev_get_iostat 가 단일 bdev 진입 시 자체적으로 헤더 시작.
 *   7) rpc_get_iostat_done 으로 가드 1 감소 → 미완 bdev 가 있으면 콜백에서 완료, 없으면 즉시 응답.
 *   8) req 자원 해제.
 * 에러 경로(err 라벨): 디코드 후 검증 실패시 즉시 에러 응답.
 *
 * 실행 컨텍스트: SPDK app thread.
 *
 * 호출 체인:
 *   jsonrpc dispatcher → [이 함수] → spdk_for_each_bdev[_by_name] → bdev_get_iostat → ... → rpc_get_iostat_done
 */
static void
rpc_bdev_get_iostat(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	/* [한국어] 디폴트: reset 안함, names 미지정 sentinel. */
	struct rpc_bdev_get_iostat req = { .reset_mode = SPDK_BDEV_RESET_STAT_NONE, .names.count = UINT32_MAX };
	struct rpc_get_iostat_ctx *rpc_ctx;                                     /* [한국어] fan-out 컨텍스트. */
	int rc;                                                                  /* [한국어] 에러 코드 임시. */
	static bool deprecated_name_option_used = false;                         /* [한국어] deprecation 경고 1회만 출력 플래그. */

	if (params != NULL) {
		/* [한국어] JSON params → req. 실패 시 INTERNAL_ERROR 응답 후 종료. */
		if (spdk_json_decode_object(params, rpc_bdev_get_iostat_decoders,
					    SPDK_COUNTOF(rpc_bdev_get_iostat_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			free_rpc_bdev_get_iostat(&req);
			return;
		}

		/* [한국어] 두 옵션 동시 사용은 모호 — 명시적으로 거부. */
		if (req.name && req.names.count != UINT32_MAX) {
			SPDK_ERRLOG("Can't report statistics when both name and names provided\n");
			rc = -EINVAL;
			goto err;
		}

		/* [한국어] deprecated name 을 names 단일 요소로 변환. 코드는 names 경로만 다루도록 통일. */
		if (req.name) {
			if (!deprecated_name_option_used) {
				SPDK_LOG_DEPRECATED(bdev_get_iostat_with_name);  /* [한국어] 첫 사용에만 경고 한번. */
				deprecated_name_option_used = true;
			}
			req.names.names[0] = strdup(req.name);  /* [한국어] free 통일을 위해 strdup. */
			if (!req.names.names[0]) {
				rc = -ENOMEM;
				goto err;
			}
			req.names.count = 1;  /* [한국어] sentinel 해제 → 단일 이름 모드. */
		}
	}

	/* [한국어] per_channel 은 응답 구조상 단일 bdev 에서만 의미가 있음. */
	if (req.per_channel && req.names.count != 1) {
		SPDK_ERRLOG("Can't use per_channel with multiple bdevs\n");
		rc = -EINVAL;
		goto err;
	}

	rpc_ctx = calloc(1, sizeof(struct rpc_get_iostat_ctx));  /* [한국어] fan-out 컨텍스트 zero-init 할당. */
	if (rpc_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate rpc_iostat_ctx struct\n");
		rc = -ENOMEM;
		goto err;
	}

	/*
	 * Increment initial bdev_count so that it will never reach 0 in the middle
	 * of iterating.
	 */
	/* [한국어] 가드 카운터 — iter 도중 콜백이 빨리 들어와 bdev_count 가 일찍 0 이 되는 race 방지. */
	rpc_ctx->bdev_count++;
	rpc_ctx->request = request;       /* [한국어] 응답 송신 핸들 보존. */
	rpc_ctx->per_channel = req.per_channel;
	rpc_ctx->reset_mode = req.reset_mode;

	if (req.names.count != UINT32_MAX) {
		/* [한국어] 이름 리스트 모드 — by_name iter 가 매칭 bdev 마다 콜백 호출. */
		rc = spdk_for_each_bdev_by_name(rpc_ctx, bdev_get_iostat, (const char **)req.names.names,
						req.names.count);
		if (rc != 0 && rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;  /* [한국어] iter 자체 실패도 누적기에 기록. */
		}
	} else {
		/* [한국어] 전체 bdev 모드 — 등록된 모든 bdev 순회. */
		rc = spdk_for_each_bdev(rpc_ctx, bdev_get_iostat);
		if (rc != 0 && rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;
		}
	}

	if (rpc_ctx->rc == 0 && !req.per_channel) {
		/* We want to fail the RPC for all failures. If per_channel is false,
		 * it is enough to defer starting RPC response until it is ensured that
		 * all spdk_bdev_for_each_channel() calls will succeed or there is no bdev.
		 */
		/* [한국어] non-per_channel 모드는 헤더를 여기서 작성 — 한 bdev 에서 실패해도 응답 전체를 에러로 통일 가능. */
		rpc_get_iostat_started(rpc_ctx);
		spdk_json_write_named_array_begin(rpc_ctx->w, "bdevs");  /* [한국어] bdevs 배열 시작. */
	}

	/* [한국어] rpc_get_iostat_started 가 끝나면 fan-out 콜백들이 채우다가 마지막에 응답이 닫힘. */
	rpc_get_iostat_done(rpc_ctx);     /* [한국어] 가드 카운터 감소 — 미완 bdev 없으면 즉시 응답. */
	free_rpc_bdev_get_iostat(&req);   /* [한국어] req 의 strdup 자원 해제. */
	return;
err:
	/* [한국어] 검증 단계 실패 — 즉시 에러 응답 후 자원 해제. */
	spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	free_rpc_bdev_get_iostat(&req);
}
SPDK_RPC_REGISTER("bdev_get_iostat", rpc_bdev_get_iostat, SPDK_RPC_RUNTIME)

/* [한국어] reset_iostat 의 fan-out 컨텍스트 — get_iostat 와 거의 동일 구조 (응답이 단순 bool 인 점만 다름). */
struct rpc_reset_iostat_ctx {
	int bdev_count;
	/* [한국어] 미완 bdev 카운터. 가드 1로 시작, 0이 되면 응답.
	 *         설정자/읽는 자: get_iostat 와 동일 패턴.
	 *         값 범위: 양의 정수.
	 *         동기화: app thread 단일. */

	int rc;
	/* [한국어] 첫 에러를 보존하는 누적기.
	 *         설정자: 콜백이 if(==0) 시 갱신.
	 *         읽는 자: rpc_reset_iostat_done 이 응답 결정.
	 *         값 범위: 0 또는 errno 음수.
	 *         동기화: app thread 단일. */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답 송신 핸들.
	 *         설정자: 핸들러 시작 시.
	 *         읽는 자: rpc_reset_iostat_done 이 send_bool/send_error.
	 *         값 범위: 유효 핸들. */

	struct spdk_json_write_ctx *w;
	/* [한국어] (실제로는 reset 응답이 bool 이라 사용 안되지만 구조 통일을 위해 보존)
	 *         설정자/읽는 자: 현 코드에선 미사용.
	 *         값 범위: 미사용.
	 *         동기화: app thread 단일. */

	enum spdk_bdev_reset_stat_mode mode;
	/* [한국어] 어떤 카운터를 리셋할지.
	 *         설정자: 핸들러가 req.mode 로.
	 *         읽는 자: bdev_reset_iostat 가 bdev_reset_device_stat 에 전달.
	 *         값 범위: ALL/MAXMIN/ERROR/NONE.
	 *         동기화: 핸들러 시작 후 read-only. */
};

/* [한국어] per-bdev reset 컨텍스트 — desc 와 back-pointer 만 보유. */
struct bdev_reset_iostat_ctx {
	struct rpc_reset_iostat_ctx *rpc_ctx;
	/* [한국어] 글로벌 fan-out 컨텍스트 back-pointer.
	 *         설정자: 핸들러/per-bdev 콜백 시작.
	 *         읽는 자: 콜백이 fan-out 카운터 감소.
	 *         값 범위: 유효 포인터.
	 *         동기화: app thread 단일. */

	struct spdk_bdev_desc *desc;
	/* [한국어] open 한 bdev descriptor — 콜백에서 close.
	 *         설정자: spdk_bdev_open_ext.
	 *         읽는 자: bdev_reset_iostat_done.
	 *         값 범위: 유효 desc.
	 *         동기화: app thread 단일. */
};

/*
 * [한국어]
 * rpc_reset_iostat_done - reset_iostat fan-out 종결
 *
 * @rpc_ctx: 글로벌 fan-out 컨텍스트.
 * @return: 없음.
 *
 * 카운터 감소 후 0 이면 bool/error 응답 발송. get_iostat 와 응답 형식만 다르고 흐름은 동일.
 */
static void
rpc_reset_iostat_done(struct rpc_reset_iostat_ctx *rpc_ctx)
{
	/* [한국어] pre-decrement: 미완 bdev 가 있으면 즉시 반환. */
	if (--rpc_ctx->bdev_count != 0) {
		return;
	}

	if (rpc_ctx->rc == 0) {
		spdk_jsonrpc_send_bool_response(rpc_ctx->request, true);  /* [한국어] 모두 성공 → true. */
	} else {
		spdk_jsonrpc_send_error_response(rpc_ctx->request, rpc_ctx->rc,
						 spdk_strerror(-rpc_ctx->rc));  /* [한국어] 한곳이라도 실패 → 에러. */
	}

	free(rpc_ctx);  /* [한국어] fan-out 컨텍스트 해제. */
}

/*
 * [한국어]
 * bdev_reset_iostat_done - 단일 bdev 통계 리셋 완료 콜백
 *
 * @bdev:   대상 bdev.
 * @cb_arg: bdev_reset_iostat_ctx*.
 * @rc:     0 또는 errno.
 * @return: 없음.
 *
 * bdev_reset_device_stat() 의 콜백. 에러를 누적기에 기록하고 fan-out 카운터 감소.
 */
static void
bdev_reset_iostat_done(struct spdk_bdev *bdev, void *cb_arg, int rc)
{
	struct bdev_reset_iostat_ctx *bdev_ctx = cb_arg;            /* [한국어] per-bdev 컨텍스트 복원. */
	struct rpc_reset_iostat_ctx *rpc_ctx = bdev_ctx->rpc_ctx;   /* [한국어] 글로벌 컨텍스트. */

	/* [한국어] 첫 에러만 보존 (이후 에러는 무시). */
	if (rc != 0 || rpc_ctx->rc != 0) {
		if (rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;
		}
	}

	rpc_reset_iostat_done(rpc_ctx);   /* [한국어] fan-out 카운터 감소 → 마지막이면 응답. */

	spdk_bdev_close(bdev_ctx->desc);  /* [한국어] open_ext 와 짝. */
	free(bdev_ctx);                    /* [한국어] per-bdev 컨텍스트 해제. */
}

/*
 * [한국어]
 * bdev_reset_iostat - spdk_for_each_bdev 의 per-bdev 콜백 (전체 리셋용)
 *
 * @ctx:  rpc_reset_iostat_ctx*.
 * @bdev: bdev 하나씩.
 * @return: 0 = 계속, 음수 = 중단.
 *
 * (a) per-bdev ctx 할당, (b) bdev open, (c) 드라이버별 reset_device_stat 훅 실행,
 * (d) bdev_reset_device_stat() 으로 코어 통계 리셋 시작 — 결과는 콜백에 모인다.
 */
static int
bdev_reset_iostat(void *ctx, struct spdk_bdev *bdev)
{
	struct rpc_reset_iostat_ctx *rpc_ctx = ctx;   /* [한국어] 글로벌 fan-out 컨텍스트. */
	struct bdev_reset_iostat_ctx *bdev_ctx;        /* [한국어] per-bdev 컨텍스트. */
	int rc;                                         /* [한국어] open 결과. */

	/* [한국어] 코어 통계 reset API 는 app_thread 에서만 호출. */
	assert(spdk_thread_is_app_thread(NULL));

	bdev_ctx = calloc(1, sizeof(struct bdev_reset_iostat_ctx));  /* [한국어] zero-init 할당. */
	if (bdev_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate bdev_iostat_ctx struct\n");
		return -ENOMEM;
	}

	/* [한국어] read-only open. */
	rc = spdk_bdev_open_ext(spdk_bdev_get_name(bdev), false, dummy_bdev_event_cb, NULL,
				&bdev_ctx->desc);
	if (rc != 0) {
		free(bdev_ctx);
		SPDK_ERRLOG("Failed to open bdev\n");
		return rc;
	}

	/* [한국어] 드라이버 모듈이 자기만의 카운터를 가지고 있다면 여기서 리셋. */
	if (bdev->fn_table->reset_device_stat) {
		bdev->fn_table->reset_device_stat(bdev->ctxt);
	}

	rpc_ctx->bdev_count++;             /* [한국어] fan-out 카운터 ↑. */
	bdev_ctx->rpc_ctx = rpc_ctx;       /* [한국어] back-pointer. */
	/* [한국어] 코어 통계 리셋 비동기 시작 — 완료 시 bdev_reset_iostat_done 호출. */
	bdev_reset_device_stat(bdev, rpc_ctx->mode, bdev_reset_iostat_done, bdev_ctx);

	return 0;
}

/* [한국어] bdev_reset_iostat RPC 의 디코드 대상 — 단일 이름 + 모드. */
struct rpc_bdev_reset_iostat {
	char *name;
	/* [한국어] 단일 bdev 이름 (선택). 없으면 모든 bdev 리셋.
	 *         설정자: spdk_json_decode_string.
	 *         읽는 자: 핸들러가 이름 있으면 단일 open + 즉시 시작 경로 사용.
	 *         값 범위: 문자열 또는 NULL.
	 *         동기화: 핸들러 stack-local. */

	enum spdk_bdev_reset_stat_mode mode;
	/* [한국어] 리셋 모드. 디폴트 ALL.
	 *         설정자: rpc_decode_reset_iostat_mode.
	 *         읽는 자: 핸들러 → rpc_ctx->mode.
	 *         값 범위: ALL/MAXMIN/ERROR/NONE.
	 *         동기화: 핸들러 stack-local. */
};

/*
 * [한국어]
 * free_rpc_bdev_reset_iostat - rpc_bdev_reset_iostat 자원 해제
 *
 * @r: 대상.
 * @return: 없음.
 */
static void
free_rpc_bdev_reset_iostat(struct rpc_bdev_reset_iostat *r)
{
	free(r->name);  /* [한국어] strdup 된 name 해제. */
}

/* [한국어] bdev_reset_iostat 의 디코더 테이블 — 모두 optional. */
static const struct spdk_json_object_decoder rpc_bdev_reset_iostat_decoders[] = {
	/* [한국어] 단일 이름 — 없으면 전체 리셋. */
	{"name", offsetof(struct rpc_bdev_reset_iostat, name), spdk_json_decode_string, true},
	/* [한국어] 리셋 모드 — 없으면 디폴트 ALL. */
	{"mode", offsetof(struct rpc_bdev_reset_iostat, mode), rpc_decode_reset_iostat_mode, true},
};

/*
 * [한국어]
 * rpc_bdev_reset_iostat - I/O 통계 리셋 RPC 핸들러
 *
 * @request: 응답 송신용 요청.
 * @params:  optional. {name?, mode?}
 * @return:  없음. 비동기 응답.
 *
 * 동작:
 *   1) params 디코드. 디폴트 mode=ALL.
 *   2) mode==NONE 면 사실상 noop — 즉시 true 응답하고 종료.
 *   3) name 이 있으면 그 bdev 만 미리 open (단일 경로).
 *   4) rpc_ctx 할당 + 가드 카운터 1.
 *   5) name 있으면 단일 bdev_reset_device_stat 시작, 없으면 spdk_for_each_bdev iter.
 *   6) rpc_reset_iostat_done 으로 가드 카운터 감소 → 미완이 없으면 즉시 응답.
 *
 * 실행 컨텍스트: SPDK app thread.
 */
static void
rpc_bdev_reset_iostat(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdev_reset_iostat req = { .mode = SPDK_BDEV_RESET_STAT_ALL, };  /* [한국어] 디폴트 mode=ALL. */
	struct spdk_bdev_desc *desc = NULL;                                          /* [한국어] name 모드에서 단일 desc. */
	struct rpc_reset_iostat_ctx *rpc_ctx;                                        /* [한국어] fan-out 컨텍스트. */
	struct bdev_reset_iostat_ctx *bdev_ctx;                                      /* [한국어] per-bdev (name 경로용). */
	int rc;                                                                       /* [한국어] 임시 에러 코드. */

	if (params != NULL) {
		/* [한국어] params 디코드 — 실패 시 INTERNAL_ERROR 응답. */
		if (spdk_json_decode_object(params, rpc_bdev_reset_iostat_decoders,
					    SPDK_COUNTOF(rpc_bdev_reset_iostat_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			free_rpc_bdev_reset_iostat(&req);
			return;
		}

		/* [한국어] mode=NONE 은 의미 없음 — 노티 후 noop true 응답. */
		if (req.mode == SPDK_BDEV_RESET_STAT_NONE) {
			SPDK_NOTICELOG("bdev_reset_iostat called with mode none, aborting operation\n");
			spdk_jsonrpc_send_bool_response(request, true);
			free_rpc_bdev_reset_iostat(&req);
			return;
		}

		/* [한국어] 단일 이름이 주어지면 미리 열어 desc 확보 — 실패 시 즉시 에러. */
		if (req.name) {
			rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to open bdev '%s': %d\n", req.name, rc);
				spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
				free_rpc_bdev_reset_iostat(&req);
				return;
			}
		}
	}


	rpc_ctx = calloc(1, sizeof(struct rpc_reset_iostat_ctx));  /* [한국어] fan-out 컨텍스트 할당. */
	if (rpc_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate rpc_iostat_ctx struct\n");
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		free_rpc_bdev_reset_iostat(&req);
		return;
	}

	/*
	 * Increment initial bdev_count so that it will never reach 0 in the middle
	 * of iterating.
	 */
	/* [한국어] 가드 카운터 1로 초기화 — 콜백 race 방지. */
	rpc_ctx->bdev_count++;
	rpc_ctx->request = request;
	rpc_ctx->mode = req.mode;

	free_rpc_bdev_reset_iostat(&req);  /* [한국어] req 자원은 mode/name 보존했으니 여기서 해제 가능. */

	if (desc != NULL) {
		/* [한국어] 단일 bdev 경로 — 이미 열려 있으니 per-bdev 컨텍스트만 만들고 시작. */
		bdev_ctx = calloc(1, sizeof(struct bdev_reset_iostat_ctx));
		if (bdev_ctx == NULL) {
			SPDK_ERRLOG("Failed to allocate bdev_iostat_ctx struct\n");
			rpc_ctx->rc = -ENOMEM;

			spdk_bdev_close(desc);  /* [한국어] 할당 실패 시 우리가 열었던 desc 정리. */
		} else {
			bdev_ctx->desc = desc;

			rpc_ctx->bdev_count++;            /* [한국어] 본 bdev 1개 추가 카운트. */
			bdev_ctx->rpc_ctx = rpc_ctx;
			/* [한국어] 비동기 리셋 시작 — 콜백에서 close + free. */
			bdev_reset_device_stat(spdk_bdev_desc_get_bdev(desc), rpc_ctx->mode,
					       bdev_reset_iostat_done, bdev_ctx);
		}
	} else {
		/* [한국어] 전체 bdev 모드 — iter 가 각 bdev 마다 bdev_reset_iostat 콜백. */
		rc = spdk_for_each_bdev(rpc_ctx, bdev_reset_iostat);
		if (rc != 0 && rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;
		}
	}

	rpc_reset_iostat_done(rpc_ctx);  /* [한국어] 가드 감소 → 미완 없으면 즉시 응답. */
}
SPDK_RPC_REGISTER("bdev_reset_iostat", rpc_bdev_reset_iostat, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_dump_bdev_info - 한 bdev 의 메타데이터를 JSON 객체로 직렬화 (per-bdev 콜백)
 *
 * @ctx:  spdk_json_write_ctx* (응답 writer).
 * @bdev: 대상 bdev.
 * @return: 0 (항상; iter 계속).
 *
 * bdev_get_bdevs 의 per-bdev 콜백. 다음을 출력:
 *   name, aliases[], product_name, block_size, num_blocks, write/unmap alignment,
 *   uuid, numa_id, md_size/dif 관련 필드, assigned_rate_limits 객체,
 *   claimed/claim_type, zoned/zone_size 관련, supported_io_types 객체,
 *   memory_domains[], driver_specific 객체.
 *
 * 실행 컨텍스트: SPDK app thread (spdk_for_each_bdev 가 호출).
 *
 * 호출 체인:
 *   spdk_for_each_bdev / rpc_bdev_get_bdev_cb → [이 함수] → spdk_bdev_get_*, dump_info_json
 */
static int
rpc_dump_bdev_info(void *ctx, struct spdk_bdev *bdev)
{
	struct spdk_json_write_ctx *w = ctx;                                 /* [한국어] writer 캐스팅. */
	struct spdk_bdev_alias *tmp;                                          /* [한국어] alias 리스트 iter 변수. */
	uint64_t qos_limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];              /* [한국어] QoS 제한 buffer. */
	struct spdk_memory_domain **domains;                                  /* [한국어] memory domain 포인터 배열. */
	enum spdk_bdev_io_type io_type;                                       /* [한국어] supported io types 순회 변수. */
	const char *name = NULL;                                               /* [한국어] io_type 이름 임시. */
	int i, rc;                                                             /* [한국어] 인덱스/결과. */

	spdk_json_write_object_begin(w);  /* [한국어] 한 bdev 의 객체 '{' 시작. */

	/* [한국어] bdev 이름 — 클라이언트가 매칭 키로 사용. */
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(bdev));

	spdk_json_write_named_array_begin(w, "aliases");  /* [한국어] alias 배열 시작. */

	/* [한국어] bdev 의 alias 리스트는 TAILQ — 각 alias 의 name 을 문자열로 추가. */
	TAILQ_FOREACH(tmp, spdk_bdev_get_aliases(bdev), tailq) {
		spdk_json_write_string(w, tmp->alias.name);
	}

	spdk_json_write_array_end(w);  /* [한국어] alias 배열 ']' 닫기. */

	/* [한국어] 모듈 이름(예: "NVMe disk", "AIO disk", "Logical Volume"). */
	spdk_json_write_named_string(w, "product_name", spdk_bdev_get_product_name(bdev));
	/* [한국어] 논리 블록 크기(보통 512 또는 4096). */
	spdk_json_write_named_uint32(w, "block_size", spdk_bdev_get_block_size(bdev));
	/* [한국어] 총 블록 수 — 디스크 용량 = block_size * num_blocks. */
	spdk_json_write_named_uint64(w, "num_blocks", spdk_bdev_get_num_blocks(bdev));
	/* [한국어] 정렬을 만족하면 모듈이 더 효율적으로 처리하는 write 정렬(블록). */
	spdk_json_write_named_uint32(w, "preferred_write_alignment",
				     spdk_bdev_get_preferred_write_alignment(bdev));
	/* [한국어] write granularity — 쓰기 단위 권장값. */
	spdk_json_write_named_uint32(w, "preferred_write_granularity",
				     spdk_bdev_get_preferred_write_granularity(bdev));
	/* [한국어] optimal write size — 한번에 보내기 좋은 크기. */
	spdk_json_write_named_uint32(w, "optimal_write_size", spdk_bdev_get_optimal_write_size(bdev));
	/* [한국어] unmap (TRIM) 정렬값. */
	spdk_json_write_named_uint32(w, "preferred_unmap_alignment",
				     spdk_bdev_get_preferred_unmap_alignment(bdev));
	/* [한국어] unmap granularity. */
	spdk_json_write_named_uint32(w, "preferred_unmap_granularity",
				     spdk_bdev_get_preferred_unmap_granularity(bdev));
	/* [한국어] bdev UUID — 영구 식별자. */
	spdk_json_write_named_uuid(w, "uuid", &bdev->uuid);
	/* [한국어] NUMA 노드 정보가 유효하면 출력 — affinity 결정에 사용. */
	if (bdev->numa.id_valid) {
		spdk_json_write_named_int32(w, "numa_id", bdev->numa.id);
	}

	/* [한국어] metadata 영역이 있는 bdev (NVMe DIF/DIX 등) 만 추가 정보 출력. */
	if (spdk_bdev_get_md_size(bdev) != 0) {
		spdk_json_write_named_uint32(w, "md_size", spdk_bdev_get_md_size(bdev));         /* [한국어] 메타데이터 바이트 크기. */
		spdk_json_write_named_bool(w, "md_interleave", spdk_bdev_is_md_interleaved(bdev)); /* [한국어] true 면 인터리브 모드(블록+md 가 한 데이터 스트림에 섞임). */
		spdk_json_write_named_uint32(w, "dif_type", spdk_bdev_get_dif_type(bdev));         /* [한국어] DIF 타입(0/1/2/3, NVMe Type 1=PRACT). */
		/* [한국어] DIF 활성 시 세부 체크 필드 추가. */
		if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
			spdk_json_write_named_bool(w, "dif_is_head_of_md", spdk_bdev_is_dif_head_of_md(bdev));  /* [한국어] DIF 위치가 md 의 앞쪽인지. */
			spdk_json_write_named_object_begin(w, "enabled_dif_check_types");
			/* [한국어] reference tag 체크 활성 여부 (NVMe PI Reference Tag). */
			spdk_json_write_named_bool(w, "reftag",
						   spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_REFTAG));
			/* [한국어] application tag 체크 활성 여부 (NVMe PI Application Tag). */
			spdk_json_write_named_bool(w, "apptag",
						   spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_APPTAG));
			/* [한국어] guard(CRC16) 체크 활성 여부. */
			spdk_json_write_named_bool(w, "guard",
						   spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_GUARD));
			spdk_json_write_object_end(w);

			/* [한국어] DIF Protection Information 포맷(16b/32b/64b CRC). */
			spdk_json_write_named_uint32(w, "dif_pi_format", spdk_bdev_get_dif_pi_format(bdev));
		}
	}

	/* [한국어] QoS rate limits 객체 시작 — 4개 종류(IOPS/RW BPS/R BPS/W BPS) 출력. */
	spdk_json_write_named_object_begin(w, "assigned_rate_limits");
	spdk_bdev_get_qos_rate_limits(bdev, qos_limits);  /* [한국어] 한번에 4개 가져와서 buffer 채움. */
	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		/* [한국어] 각 타입별 RPC 키 이름(rw_ios_per_sec 등) 으로 값 출력. */
		spdk_json_write_named_uint64(w, spdk_bdev_get_qos_rpc_type(i), qos_limits[i]);
	}
	spdk_json_write_object_end(w);

	/* [한국어] 다른 모듈이 이 bdev 를 점유했는지(예: lvol_store 가 NVMe bdev 점유). */
	spdk_json_write_named_bool(w, "claimed",
				   (bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE));
	if (bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE) {
		/* [한국어] 점유 종류 문자열 — exclusive_write, read_many_write_one 등. */
		spdk_json_write_named_string(w, "claim_type",
					     spdk_bdev_claim_get_name(bdev->internal.claim_type));
	}

	/* [한국어] zoned device(ZNS) 여부와 zone 메타데이터. */
	spdk_json_write_named_bool(w, "zoned", bdev->zoned);
	if (bdev->zoned) {
		spdk_json_write_named_uint64(w, "zone_size", bdev->zone_size);              /* [한국어] 한 zone 의 블록 수. */
		spdk_json_write_named_uint64(w, "max_open_zones", bdev->max_open_zones);     /* [한국어] 동시 open 가능한 최대 zone 수. */
		spdk_json_write_named_uint64(w, "optimal_open_zones", bdev->optimal_open_zones); /* [한국어] 권장 동시 open 수. */
	}

	/* [한국어] bdev 가 지원하는 모든 IO 타입 (read/write/unmap/flush/compare/copy 등) 의 boolean map. */
	spdk_json_write_named_object_begin(w, "supported_io_types");
	for (io_type = SPDK_BDEV_IO_TYPE_READ; io_type < SPDK_BDEV_NUM_IO_TYPES; ++io_type) {
		name = spdk_bdev_get_io_type_name(io_type);                          /* [한국어] enum → 문자열. */
		spdk_json_write_named_bool(w, name, spdk_bdev_io_type_supported(bdev, io_type));
	}
	spdk_json_write_object_end(w);

	/* [한국어] memory_domains: bdev 가 DMA 에 사용할 수 있는 메모리 도메인 목록 (RDMA, GPU 등).
	 *         첫번째 호출(domains=NULL, len=0)은 카운트 조회 — rc>0 이 도메인 수. */
	rc = spdk_bdev_get_memory_domains(bdev, NULL, 0);
	if (rc > 0) {
		domains = calloc(rc, sizeof(struct spdk_memory_domain *));  /* [한국어] 도메인 포인터 배열 할당. */
		if (domains) {
			i = spdk_bdev_get_memory_domains(bdev, domains, rc);  /* [한국어] 두번째 호출 — 실제 채움. */
			if (i == rc) {
				spdk_json_write_named_array_begin(w, "memory_domains");
				for (i = 0; i < rc; i++) {
					spdk_json_write_object_begin(w);
					/* [한국어] DMA 디바이스 식별자 문자열 (예: "RDMA NIC mlx5_0"). */
					spdk_json_write_named_string(w, "dma_device_id", spdk_memory_domain_get_dma_device_id(domains[i]));
					/* [한국어] DMA 디바이스 타입 enum (RDMA/CUDA/MEMORY 등). */
					spdk_json_write_named_int32(w, "dma_device_type",
								    spdk_memory_domain_get_dma_device_type(domains[i]));
					spdk_json_write_object_end(w);
				}
				spdk_json_write_array_end(w);
			} else {
				/* [한국어] 두 호출 사이에 도메인 수가 변했으면 inconsistent 로 보고 출력 생략. */
				SPDK_ERRLOG("Unexpected number of memory domains %d (should be %d)\n", i, rc);
			}

			free(domains);
		} else {
			SPDK_ERRLOG("Memory allocation failed\n");
		}
	}

	/* [한국어] 모듈별(driver_specific) 메타데이터 hook — NVMe namespace info, lvol id 등. */
	spdk_json_write_named_object_begin(w, "driver_specific");
	spdk_bdev_dump_info_json(bdev, w);  /* [한국어] 내부에서 fn_table->dump_info_json 위임. */
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);  /* [한국어] 한 bdev 객체 '}' 닫기. */

	return 0;  /* [한국어] iter 계속. */
}

/* [한국어] bdev_get_bdevs RPC 의 디코드 대상. */
struct rpc_bdev_get_bdevs {
	char		*name;
	/* [한국어] 단일 bdev 이름 (선택). 없으면 모든 bdev 덤프.
	 *         설정자: spdk_json_decode_string.
	 *         읽는 자: 핸들러 — 있으면 open_async 경로, 없으면 for_each_bdev 경로.
	 *         값 범위: 문자열 또는 NULL.
	 *         동기화: 핸들러 stack-local. */

	uint64_t	timeout;
	/* [한국어] open_async 의 타임아웃 (ms). 단일 이름 + 아직 미생성 bdev 를 기다릴 때.
	 *         설정자: spdk_json_decode_uint64.
	 *         읽는 자: 핸들러 → opts.timeout_ms.
	 *         값 범위: 0(즉시) 또는 양의 ms 값.
	 *         동기화: 핸들러 stack-local. */
};

/*
 * [한국어]
 * free_rpc_bdev_get_bdevs - rpc_bdev_get_bdevs 자원 해제
 *
 * @r: 대상.
 * @return: 없음.
 */
static void
free_rpc_bdev_get_bdevs(struct rpc_bdev_get_bdevs *r)
{
	free(r->name);  /* [한국어] strdup 된 name 해제. */
}

/* [한국어] bdev_get_bdevs 디코더 테이블 — 모두 optional. */
static const struct spdk_json_object_decoder rpc_bdev_get_bdevs_decoders[] = {
	/* [한국어] 단일 이름. */
	{"name", offsetof(struct rpc_bdev_get_bdevs, name), spdk_json_decode_string, true},
	/* [한국어] 비동기 open 타임아웃(ms). */
	{"timeout", offsetof(struct rpc_bdev_get_bdevs, timeout), spdk_json_decode_uint64, true},
};

/*
 * [한국어]
 * rpc_bdev_get_bdev_cb - 단일 bdev 비동기 open 완료 콜백
 *
 * @desc:   open 결과 desc (rc==0일 때만 유효).
 * @rc:     0 또는 errno.
 * @cb_arg: spdk_jsonrpc_request*.
 * @return: 없음.
 *
 * spdk_bdev_open_async() 의 콜백. 성공 시 rpc_dump_bdev_info 로 응답을 작성하고 close.
 *
 * 실행 컨텍스트: SPDK app thread.
 */
static void
rpc_bdev_get_bdev_cb(struct spdk_bdev_desc *desc, int rc, void *cb_arg)
{
	struct spdk_jsonrpc_request *request = cb_arg;  /* [한국어] 요청 핸들 복원. */
	struct spdk_json_write_ctx *w;                   /* [한국어] 응답 writer. */

	if (rc == 0) {
		w = spdk_jsonrpc_begin_result(request);  /* [한국어] 응답 빌드 시작. */

		spdk_json_write_array_begin(w);                              /* [한국어] (단일 bdev 도) 배열 형식 통일. */
		rpc_dump_bdev_info(w, spdk_bdev_desc_get_bdev(desc));        /* [한국어] 메타데이터 직렬화. */
		spdk_json_write_array_end(w);
		spdk_jsonrpc_end_result(request, w);                         /* [한국어] 응답 송신. */

		spdk_bdev_close(desc);                                        /* [한국어] open_async 와 짝. */
	} else {
		/* [한국어] 타임아웃이나 미생성 bdev — errno 그대로 클라이언트에 전달. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
}

/*
 * [한국어]
 * rpc_bdev_get_bdevs - bdev 메타데이터 조회 RPC 핸들러
 *
 * @request: 응답 송신용 요청.
 * @params:  optional. {name?, timeout?}
 * @return:  없음.
 *
 * 동작:
 *   1) params 디코드 (없으면 전체 덤프).
 *   2) name 있으면 spdk_bdev_open_async — 즉시 못 열어도 timeout 동안 대기.
 *   3) name 없으면 즉시 응답: spdk_for_each_bdev 로 전체 순회.
 *
 * 실행 컨텍스트: SPDK app thread.
 *
 * 호출 체인:
 *   jsonrpc dispatcher → [이 함수] → spdk_bdev_open_async 또는 spdk_for_each_bdev
 */
static void
rpc_bdev_get_bdevs(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_bdev_get_bdevs req = {};                       /* [한국어] zero-init: name=NULL, timeout=0. */
	struct spdk_bdev_open_async_opts opts = {};               /* [한국어] open_async 옵션 buffer. */
	struct spdk_json_write_ctx *w;                            /* [한국어] 응답 writer. */
	int rc;                                                    /* [한국어] open_async 결과. */

	/* [한국어] params 가 있을 때만 디코드. INTERNAL_ERROR 응답은 lib 호환 차원에서 (역사적 코드). */
	if (params && spdk_json_decode_object(params, rpc_bdev_get_bdevs_decoders,
					      SPDK_COUNTOF(rpc_bdev_get_bdevs_decoders),
					      &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		free_rpc_bdev_get_bdevs(&req);
		return;
	}

	if (req.name) {
		opts.size = sizeof(opts);          /* [한국어] ABI size-prefix. */
		opts.timeout_ms = req.timeout;     /* [한국어] 미생성 bdev 가 등록될 때까지 대기 ms. */

		/* [한국어] 비동기 open — 콜백에서 응답. 자체 실패(예: 옵션 invalid)는 즉시 rc<0. */
		rc = spdk_bdev_open_async(req.name, false, dummy_bdev_event_cb, NULL, &opts,
					  rpc_bdev_get_bdev_cb, request);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_bdev_open_async failed for '%s': rc=%d\n", req.name, rc);
			spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		}

		free_rpc_bdev_get_bdevs(&req);
		return;
	}

	free_rpc_bdev_get_bdevs(&req);  /* [한국어] name 없음 — 더 쓸 일 없으니 미리 해제. */

	w = spdk_jsonrpc_begin_result(request);     /* [한국어] 응답 빌드 시작. */
	spdk_json_write_array_begin(w);              /* [한국어] bdev 배열 시작. */

	/* [한국어] 모든 bdev 순회 — 각 bdev 마다 rpc_dump_bdev_info(w, bdev) 호출. */
	spdk_for_each_bdev(w, rpc_dump_bdev_info);

	spdk_json_write_array_end(w);                 /* [한국어] 배열 닫기. */

	spdk_jsonrpc_end_result(request, w);          /* [한국어] 응답 송신. */
}
SPDK_RPC_REGISTER("bdev_get_bdevs", rpc_bdev_get_bdevs, SPDK_RPC_RUNTIME)

/* [한국어] bdev_set_qd_sampling_period RPC 의 디코드 대상. */
struct rpc_bdev_set_qd_sampling_period {
	char *name;
	/* [한국어] 대상 bdev 이름.
	 *         설정자: spdk_json_decode_string.
	 *         읽는 자: spdk_bdev_open_ext.
	 *         값 범위: 문자열 (필수).
	 *         동기화: 핸들러 stack-local. */

	uint64_t period;
	/* [한국어] queue depth 샘플링 주기 (tsc tick). 0 이면 비활성.
	 *         설정자: spdk_json_decode_uint64.
	 *         읽는 자: spdk_bdev_set_qd_sampling_period.
	 *         값 범위: 0 또는 양의 tsc tick.
	 *         동기화: 핸들러 stack-local. */
};

/*
 * [한국어]
 * free_rpc_bdev_set_qd_sampling_period - rpc 자원 해제
 *
 * @r: 대상.
 * @return: 없음.
 */
static void
free_rpc_bdev_set_qd_sampling_period(struct rpc_bdev_set_qd_sampling_period *r)
{
	free(r->name);  /* [한국어] strdup 된 name 해제. */
}

/* [한국어] bdev_set_qd_sampling_period 디코더 테이블. 두 필드 모두 필수. */
static const struct spdk_json_object_decoder
	rpc_bdev_set_qd_sampling_period_decoders[] = {
	/* [한국어] 대상 bdev 이름. */
	{"name", offsetof(struct rpc_bdev_set_qd_sampling_period, name), spdk_json_decode_string},
	/* [한국어] 샘플링 주기(tsc tick). */
	{"period", offsetof(struct rpc_bdev_set_qd_sampling_period, period), spdk_json_decode_uint64},
};

/*
 * [한국어]
 * rpc_bdev_set_qd_sampling_period - 큐 깊이 샘플링 주기 설정 RPC
 *
 * @request: 응답 송신용 요청.
 * @params:  {name, period} 둘 다 필수.
 * @return:  없음. 동기 응답.
 *
 * period > 0 이면 lib/bdev 가 주기적으로 큐 깊이를 샘플링하여 utilization 통계를 만든다.
 * 이 통계는 bdev_get_iostat 응답에 queue_depth/io_time/weighted_io_time 으로 노출된다.
 *
 * 실행 컨텍스트: SPDK app thread.
 *
 * 호출 체인:
 *   jsonrpc dispatcher → [이 함수] → spdk_bdev_open_ext → spdk_bdev_set_qd_sampling_period → close
 */
static void
rpc_bdev_set_qd_sampling_period(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_set_qd_sampling_period req = {0};  /* [한국어] zero-init. */
	struct spdk_bdev_desc *desc;                         /* [한국어] open desc. */
	int rc;                                               /* [한국어] open 결과. */

	/* [한국어] params 디코드 — 필수 필드 누락 시 INTERNAL_ERROR. */
	if (spdk_json_decode_object(params, rpc_bdev_set_qd_sampling_period_decoders,
				    SPDK_COUNTOF(rpc_bdev_set_qd_sampling_period_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] 옵션 변경을 위한 read-only open — open 만 하면 핸들 가능. */
	rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open bdev '%s': %d\n", req.name, rc);
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	/* [한국어] 샘플링 주기 적용 — 0 이면 샘플러 disable. */
	spdk_bdev_set_qd_sampling_period(spdk_bdev_desc_get_bdev(desc), req.period);
	spdk_jsonrpc_send_bool_response(request, true);  /* [한국어] 동기 성공 응답. */

	spdk_bdev_close(desc);  /* [한국어] open_ext 와 짝. */

cleanup:
	free_rpc_bdev_set_qd_sampling_period(&req);  /* [한국어] strdup 된 name 해제. */
}
SPDK_RPC_REGISTER("bdev_set_qd_sampling_period",
		  rpc_bdev_set_qd_sampling_period,
		  SPDK_RPC_RUNTIME)

/* [한국어] bdev_set_qos_limit RPC 의 디코드 대상 — name + 4개 rate-limit 값. */
struct rpc_bdev_set_qos_limit {
	char		*name;
	/* [한국어] 대상 bdev 이름.
	 *         설정자: spdk_json_decode_string.
	 *         읽는 자: spdk_bdev_open_ext.
	 *         값 범위: 문자열 (필수).
	 *         동기화: 핸들러 stack-local. */

	uint64_t	limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];
	/* [한국어] 4종 rate limit 배열: [RW_IOPS, RW_BPS, R_BPS, W_BPS] (각각 IOPS, MB/s).
	 *         설정자: 핸들러 디폴트 UINT64_MAX (변경 안함), 디코더가 사용자 지정값으로 덮어쓰기.
	 *         읽는 자: spdk_bdev_set_qos_rate_limits — UINT64_MAX 는 "변경 안 함" 신호.
	 *         값 범위: 0(disable) ~ UINT64_MAX(skip).
	 *         동기화: 핸들러 stack-local. */
};

/*
 * [한국어]
 * free_rpc_bdev_set_qos_limit - 자원 해제
 *
 * @r: 대상.
 * @return: 없음.
 */
static void
free_rpc_bdev_set_qos_limit(struct rpc_bdev_set_qos_limit *r)
{
	free(r->name);  /* [한국어] strdup 된 name 해제. */
}

/* [한국어] bdev_set_qos_limit 디코더 테이블. name 필수, 4개 limit 모두 optional(미지정 시 UINT64_MAX 유지 → "변경 안 함"). */
static const struct spdk_json_object_decoder rpc_bdev_set_qos_limit_decoders[] = {
	/* [한국어] 대상 bdev 이름 (필수). */
	{"name", offsetof(struct rpc_bdev_set_qos_limit, name), spdk_json_decode_string},
	{
		/* [한국어] 읽기+쓰기 IOPS 한도 (per second). 0 = disable. UINT64_MAX = 변경 없음. */
		"rw_ios_per_sec", offsetof(struct rpc_bdev_set_qos_limit,
					   limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT]),
		spdk_json_decode_uint64, true
	},
	{
		/* [한국어] 읽기+쓰기 대역폭 한도 (MB/s). */
		"rw_mbytes_per_sec", offsetof(struct rpc_bdev_set_qos_limit,
					      limits[SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT]),
		spdk_json_decode_uint64, true
	},
	{
		/* [한국어] 읽기 전용 대역폭 한도 (MB/s). */
		"r_mbytes_per_sec", offsetof(struct rpc_bdev_set_qos_limit,
					     limits[SPDK_BDEV_QOS_R_BPS_RATE_LIMIT]),
		spdk_json_decode_uint64, true
	},
	{
		/* [한국어] 쓰기 전용 대역폭 한도 (MB/s). */
		"w_mbytes_per_sec", offsetof(struct rpc_bdev_set_qos_limit,
					     limits[SPDK_BDEV_QOS_W_BPS_RATE_LIMIT]),
		spdk_json_decode_uint64, true
	},
};

/*
 * [한국어]
 * rpc_bdev_set_qos_limit_complete - QoS 제한 설정 완료 콜백
 *
 * @cb_arg: spdk_jsonrpc_request*.
 * @status: 0 또는 errno.
 * @return: 없음.
 *
 * spdk_bdev_set_qos_rate_limits() 의 콜백. 성공/실패에 따라 응답 송신.
 *
 * 실행 컨텍스트: SPDK app thread.
 */
static void
rpc_bdev_set_qos_limit_complete(void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;  /* [한국어] 요청 핸들 복원. */

	if (status != 0) {
		/* [한국어] 실패 — 사용자 친화 메시지 + INVALID_PARAMS 코드. */
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to configure rate limit: %s",
						     spdk_strerror(-status));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);  /* [한국어] 성공 응답. */
}

/*
 * [한국어]
 * rpc_bdev_set_qos_limit - QoS rate-limit 설정 RPC
 *
 * @request: 응답 송신용 요청.
 * @params:  {name, [rw_ios_per_sec?, rw_mbytes_per_sec?, r_mbytes_per_sec?, w_mbytes_per_sec?]}
 * @return:  없음. 비동기 응답(set_qos_rate_limits 가 채널 hop 후 콜백).
 *
 * 동작:
 *   1) params 디코드 — 디폴트 limits[] = {UINT64_MAX,...}.
 *   2) bdev open.
 *   3) 4개 limits 중 적어도 하나가 UINT64_MAX 가 아닌지 확인 — 모두 미지정이면 거부.
 *   4) spdk_bdev_set_qos_rate_limits 호출 — 비동기. 응답은 콜백에서.
 *   5) close (lib/bdev 가 내부적으로 reference 잡음).
 *
 * 실행 컨텍스트: SPDK app thread.
 *
 * 호출 체인:
 *   jsonrpc dispatcher → [이 함수] → spdk_bdev_set_qos_rate_limits
 *                                  → (콜백) rpc_bdev_set_qos_limit_complete → 응답
 */
static void
rpc_bdev_set_qos_limit(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	/* [한국어] 디폴트: 4개 limit 모두 UINT64_MAX = "변경하지 않음" 마커. */
	struct rpc_bdev_set_qos_limit req = {NULL, {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}};
	struct spdk_bdev_desc *desc;  /* [한국어] open desc. */
	int i, rc;                     /* [한국어] iter 인덱스 / 결과. */

	/* [한국어] params 디코드 — 실패 시 INTERNAL_ERROR. */
	if (spdk_json_decode_object(params, rpc_bdev_set_qos_limit_decoders,
				    SPDK_COUNTOF(rpc_bdev_set_qos_limit_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] open. 실패 시 errno 그대로 응답. */
	rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open bdev '%s': %d\n", req.name, rc);
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	/* [한국어] 적어도 하나의 limit 가 명시적으로 설정됐는지 확인 — 모두 sentinel 이면 의미 없는 호출. */
	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (req.limits[i] != UINT64_MAX) {
			break;
		}
	}
	if (i == SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES) {
		SPDK_ERRLOG("no rate limits specified\n");
		spdk_bdev_close(desc);  /* [한국어] open 했으니 close. */
		spdk_jsonrpc_send_error_response(request, -EINVAL, "No rate limits specified");
		goto cleanup;
	}

	/* [한국어] 비동기 적용 — 콜백에서 응답. UINT64_MAX 인 항목은 lib 가 변경 안함. */
	spdk_bdev_set_qos_rate_limits(spdk_bdev_desc_get_bdev(desc), req.limits,
				      rpc_bdev_set_qos_limit_complete, request);

	spdk_bdev_close(desc);  /* [한국어] lib 가 내부 reference 보유 — close 해도 작업 진행 안전. */

cleanup:
	free_rpc_bdev_set_qos_limit(&req);  /* [한국어] strdup 된 name 해제. */
}

SPDK_RPC_REGISTER("bdev_set_qos_limit", rpc_bdev_set_qos_limit, SPDK_RPC_RUNTIME)

/* SPDK_RPC_ENABLE_BDEV_HISTOGRAM */

/* [한국어] bdev_enable_histogram RPC 의 디코드 대상. */
struct rpc_bdev_enable_histogram_request {
	char *name;
	/* [한국어] 대상 bdev 이름.
	 *         설정자: spdk_json_decode_string.
	 *         읽는 자: spdk_bdev_open_ext.
	 *         값 범위: 문자열 (필수).
	 *         동기화: 핸들러 stack-local. */

	bool enable;
	/* [한국어] true 면 활성화, false 면 비활성화.
	 *         설정자: spdk_json_decode_bool.
	 *         읽는 자: spdk_bdev_histogram_enable_ext.
	 *         값 범위: true/false (필수).
	 *         동기화: 핸들러 stack-local. */

	char *opc;
	/* [한국어] (선택) 특정 IO 타입만 측정 (예: "read", "write"). 미지정 시 모든 IO 타입.
	 *         설정자: spdk_json_decode_string.
	 *         읽는 자: 핸들러가 spdk_bdev_get_io_type(opc) 로 enum 변환 후 opts.io_type.
	 *         값 범위: SPDK io_type 이름 문자열 또는 NULL.
	 *         동기화: 핸들러 stack-local. */

	uint8_t granularity;
	/* [한국어] 히스토그램 버킷 granularity (log2 단위 bucket 너비).
	 *         설정자: spdk_json_decode_uint8 (디폴트 SPDK_HISTOGRAM_GRANULARITY_DEFAULT).
	 *         읽는 자: opts.granularity → spdk_histogram_data_alloc_sized_ext.
	 *         값 범위: 0 ~ 작은 정수 (값이 클수록 버킷 폭 큼).
	 *         동기화: 핸들러 stack-local. */

	uint64_t min_nsec;
	/* [한국어] 히스토그램 측정 하한 (nsec). 디폴트 1us.
	 *         설정자: spdk_json_decode_uint64.
	 *         읽는 자: opts.min_nsec.
	 *         값 범위: 양의 정수.
	 *         동기화: 핸들러 stack-local. */

	uint64_t max_nsec;
	/* [한국어] 히스토그램 측정 상한 (nsec). 디폴트 120s.
	 *         설정자: spdk_json_decode_uint64.
	 *         읽는 자: opts.max_nsec.
	 *         값 범위: min_nsec 보다 큰 양의 정수.
	 *         동기화: 핸들러 stack-local. */
};

/*
 * [한국어]
 * free_rpc_bdev_enable_histogram_request - 자원 해제
 *
 * @r: 대상.
 * @return: 없음.
 */
static void
free_rpc_bdev_enable_histogram_request(struct rpc_bdev_enable_histogram_request *r)
{
	free(r->name);  /* [한국어] strdup 된 name. */
	free(r->opc);   /* [한국어] strdup 된 opc (NULL 가능). */
}

/* [한국어] bdev_enable_histogram 디코더 테이블. name/enable 필수, 나머지 optional. */
static const struct spdk_json_object_decoder rpc_bdev_enable_histogram_decoders[] = {
	/* [한국어] bdev 이름 (필수). */
	{"name", offsetof(struct rpc_bdev_enable_histogram_request, name), spdk_json_decode_string},
	/* [한국어] 켜기/끄기 boolean (필수). */
	{"enable", offsetof(struct rpc_bdev_enable_histogram_request, enable), spdk_json_decode_bool},
	/* [한국어] 특정 io_type 만 (optional). */
	{"opc", offsetof(struct rpc_bdev_enable_histogram_request, opc), spdk_json_decode_string, true},
	/* [한국어] 버킷 granularity (optional). */
	{"granularity", offsetof(struct rpc_bdev_enable_histogram_request, granularity), spdk_json_decode_uint8, true},
	/* [한국어] 측정 하한 nsec (optional). */
	{"min_nsec", offsetof(struct rpc_bdev_enable_histogram_request, min_nsec), spdk_json_decode_uint64, true},
	/* [한국어] 측정 상한 nsec (optional). */
	{"max_nsec", offsetof(struct rpc_bdev_enable_histogram_request, max_nsec), spdk_json_decode_uint64, true},
};

/*
 * [한국어]
 * bdev_histogram_status_cb - histogram 활성/비활성 적용 완료 콜백
 *
 * @cb_arg: spdk_jsonrpc_request*.
 * @status: 0 또는 errno.
 * @return: 없음.
 *
 * spdk_bdev_histogram_enable_ext 의 콜백. 성공/실패 응답.
 */
static void
bdev_histogram_status_cb(void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;  /* [한국어] 요청 핸들 복원. */

	if (status == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, status, spdk_strerror(-status));
	}
}

/*
 * [한국어]
 * rpc_bdev_enable_histogram - latency histogram 활성/비활성 RPC
 *
 * @request: 응답 송신용 요청.
 * @params:  {name, enable, opc?, granularity?, min_nsec?, max_nsec?}
 * @return:  없음. 비동기 응답.
 *
 * 동작:
 *   1) params 디코드 (디폴트: granularity/min_nsec/max_nsec).
 *   2) bdev open.
 *   3) opts 초기화 (size 기반 ABI).
 *   4) opc 가 있으면 string→enum 변환 후 opts.io_type 설정 (특정 타입만 측정).
 *   5) granularity/min/max nsec 옵션 채움.
 *   6) spdk_bdev_histogram_enable_ext 호출 — 모든 채널에 메시지 hop 으로 전파, 콜백에서 응답.
 *   7) close + cleanup.
 *
 * 실행 컨텍스트: SPDK app thread.
 *
 * 호출 체인:
 *   jsonrpc dispatcher → [이 함수] → spdk_bdev_histogram_enable_ext
 *                                  → (콜백) bdev_histogram_status_cb → 응답
 */
static void
rpc_bdev_enable_histogram(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	/* [한국어] 디폴트 granularity/min/max nsec 으로 초기화 (1us~120s, 디폴트 granularity). */
	struct rpc_bdev_enable_histogram_request req = {.granularity = SPDK_HISTOGRAM_GRANULARITY_DEFAULT,
		       .min_nsec = SPDK_BDEV_HISTOGRAM_DEFAULT_MIN_VALUE_NS,
		       .max_nsec = SPDK_BDEV_HISTOGRAM_DEFAULT_MAX_VALUE_NS
	};
	struct spdk_bdev_desc *desc;                     /* [한국어] open desc. */
	int rc;                                            /* [한국어] open 결과. */
	struct spdk_bdev_enable_histogram_opts opts = {};  /* [한국어] enable 옵션 buffer. */
	int io_type = 0;                                    /* [한국어] opc → io_type 변환 임시값 (-1 이면 invalid). */

	/* [한국어] params 디코드 — 실패 시 INTERNAL_ERROR. */
	if (spdk_json_decode_object(params, rpc_bdev_enable_histogram_decoders,
				    SPDK_COUNTOF(rpc_bdev_enable_histogram_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] open — 실패 시 errno 응답. */
	rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	/* [한국어] opts 의 size 등 ABI 필드 초기화. */
	spdk_bdev_enable_histogram_opts_init(&opts, sizeof(opts));

	/* [한국어] opc 가 있으면 io_type 검증 + opts 채움. */
	if (req.opc != NULL) {
		io_type = spdk_bdev_get_io_type(req.opc);  /* [한국어] 문자열 → enum. -1 이면 invalid. */
		if (io_type == -1) {
			SPDK_ERRLOG("Invalid IO type\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Invalid Io type");
			goto cleanup;  /* [한국어] desc close 누락? — 원본 코드 그대로(주석은 코드 수정 금지 원칙). */
		}
		opts.io_type = (uint8_t) io_type;  /* [한국어] 특정 타입만 측정. */
	}

	opts.granularity = req.granularity;  /* [한국어] 버킷 너비. */
	opts.min_nsec = req.min_nsec;        /* [한국어] 측정 하한. */
	opts.max_nsec = req.max_nsec;        /* [한국어] 측정 상한. */

	/* [한국어] 모든 채널에 enable 메시지 전파 — 콜백에서 응답. */
	spdk_bdev_histogram_enable_ext(spdk_bdev_desc_get_bdev(desc), bdev_histogram_status_cb,
				       request, req.enable, &opts);

	spdk_bdev_close(desc);  /* [한국어] lib 가 내부 ref 보유 — close 해도 작업 진행. */

cleanup:
	free_rpc_bdev_enable_histogram_request(&req);  /* [한국어] strdup 된 name/opc 해제. */
}

SPDK_RPC_REGISTER("bdev_enable_histogram", rpc_bdev_enable_histogram, SPDK_RPC_RUNTIME)

/* SPDK_RPC_GET_BDEV_HISTOGRAM */

/* [한국어] bdev_get_histogram RPC 의 디코드 대상. */
struct rpc_bdev_get_histogram_request {
	char *name;
	/* [한국어] 대상 bdev 이름.
	 *         설정자: spdk_json_decode_string.
	 *         읽는 자: spdk_bdev_open_ext.
	 *         값 범위: 문자열 (필수).
	 *         동기화: 핸들러 stack-local. */
};

/* [한국어] bdev_get_histogram 디코더 테이블 — name 1개. */
static const struct spdk_json_object_decoder rpc_bdev_get_histogram_decoders[] = {
	/* [한국어] 대상 bdev 이름 (필수). */
	{"name", offsetof(struct rpc_bdev_get_histogram_request, name), spdk_json_decode_string}
};

/*
 * [한국어]
 * free_rpc_bdev_get_histogram_request - 자원 해제
 *
 * @r: 대상.
 * @return: 없음.
 */
static void
free_rpc_bdev_get_histogram_request(struct rpc_bdev_get_histogram_request *r)
{
	free(r->name);  /* [한국어] strdup 된 name 해제. */
}

/*
 * [한국어]
 * _rpc_bdev_histogram_data_cb - histogram 데이터 수집 완료 콜백
 *
 * @cb_arg:    spdk_jsonrpc_request*.
 * @status:    0 또는 errno.
 * @histogram: 수집된 히스토그램 (lib 가 할당, 콜백 끝에서 free 의무).
 * @return: 없음.
 *
 * spdk_bdev_histogram_get 의 콜백. 버킷 배열을 base64 로 인코딩해 응답에 담는다.
 * 클라이언트(rpc.py 의 bdev_get_histogram)는 base64 디코드 후 uint64_t 배열로 해석.
 *
 * 실행 컨텍스트: SPDK app thread (lib 가 채널 hop 후 호출).
 *
 * 호출 체인:
 *   spdk_bdev_histogram_get → ... → [이 함수] → base64 인코딩 → 응답 → spdk_histogram_data_free
 */
static void
_rpc_bdev_histogram_data_cb(void *cb_arg, int status, struct spdk_histogram_data *histogram)
{
	struct spdk_jsonrpc_request *request = cb_arg;  /* [한국어] 요청 핸들 복원. */
	struct spdk_json_write_ctx *w;                   /* [한국어] 응답 writer. */
	int rc;                                            /* [한국어] base64 인코딩 결과. */
	char *encoded_histogram;                           /* [한국어] base64 결과 buffer (caller free). */
	size_t src_len, dst_len;                           /* [한국어] base64 입출력 길이. */


	/* [한국어] lib 가 수집 도중 실패했으면 INTERNAL_ERROR + free. */
	if (status != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-status));
		goto invalid;
	}

	/* [한국어] 버킷 개수 × uint64 = 원본 바이트 길이. */
	src_len = SPDK_HISTOGRAM_NUM_BUCKETS(histogram) * sizeof(uint64_t);
	/* [한국어] base64 인코딩 후 길이 (NUL 포함). */
	dst_len = spdk_base64_get_encoded_strlen(src_len) + 1;

	encoded_histogram = malloc(dst_len);  /* [한국어] 인코딩 결과 버퍼 할당. */
	if (encoded_histogram == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(ENOMEM));
		goto invalid;
	}

	/* [한국어] 실제 base64 인코딩. histogram->bucket 은 lib 내부 버킷 배열. */
	rc = spdk_base64_encode(encoded_histogram, histogram->bucket, src_len);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		goto free_encoded_histogram;
	}

	w = spdk_jsonrpc_begin_result(request);  /* [한국어] 응답 빌드 시작. */
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "histogram", encoded_histogram);  /* [한국어] base64 인코딩된 버킷 데이터. */
	spdk_json_write_named_int64(w, "granularity", histogram->granularity);  /* [한국어] 버킷 granularity. */
	spdk_json_write_named_uint32(w, "min_range", histogram->min_range);     /* [한국어] 측정 하한 range. */
	spdk_json_write_named_uint32(w, "max_range", histogram->max_range);     /* [한국어] 측정 상한 range. */
	spdk_json_write_named_int64(w, "tsc_rate", spdk_get_ticks_hz());        /* [한국어] tsc 환산 기준. */
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);  /* [한국어] 응답 송신. */

free_encoded_histogram:
	free(encoded_histogram);  /* [한국어] 인코딩 버퍼 해제. */
invalid:
	spdk_histogram_data_free(histogram);  /* [한국어] lib 할당 히스토그램 해제 (콜백 의무). */
}

/*
 * [한국어]
 * rpc_bdev_get_histogram - latency histogram 조회 RPC
 *
 * @request: 응답 송신용 요청.
 * @params:  {name}
 * @return:  없음. 비동기 응답(채널 hop 후 콜백).
 *
 * 동작:
 *   1) params 디코드.
 *   2) bdev open.
 *   3) bdev->internal 의 granularity/min/max 로 히스토그램 buffer 할당.
 *   4) spdk_bdev_histogram_get 으로 모든 채널 누적 합산 시작 — 콜백에서 응답.
 *   5) close + cleanup.
 *
 * 실행 컨텍스트: SPDK app thread.
 *
 * 호출 체인:
 *   jsonrpc dispatcher → [이 함수] → spdk_bdev_histogram_get
 *                                  → (콜백) _rpc_bdev_histogram_data_cb → 응답
 */
static void
rpc_bdev_get_histogram(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_get_histogram_request req = {NULL};   /* [한국어] zero-init. */
	struct spdk_histogram_data *histogram;                 /* [한국어] 누적 buffer. */
	struct spdk_bdev_desc *desc;                           /* [한국어] open desc. */
	struct spdk_bdev *bdev;                                /* [한국어] desc → bdev 단축. */
	int rc;                                                  /* [한국어] open 결과. */

	/* [한국어] params 디코드 — 실패 시 INTERNAL_ERROR. */
	if (spdk_json_decode_object(params, rpc_bdev_get_histogram_decoders,
				    SPDK_COUNTOF(rpc_bdev_get_histogram_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] open — 실패 시 errno 그대로 응답. */
	rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);  /* [한국어] internal 필드 접근 위함. */

	/* [한국어] enable 시 저장한 granularity/min/max 와 동일 형태로 누적 buffer 할당. */
	histogram = spdk_histogram_data_alloc_sized_ext(bdev->internal.histogram_granularity,
			bdev->internal.histogram_min_val, bdev->internal.histogram_max_val);
	if (histogram == NULL) {
		spdk_bdev_close(desc);  /* [한국어] alloc 실패 — open 한 desc 정리. */
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		goto cleanup;
	}

	/* [한국어] 모든 채널 hop 으로 채널별 히스토그램을 합산 — 결과는 콜백에 전달. */
	spdk_bdev_histogram_get(bdev, histogram,
				_rpc_bdev_histogram_data_cb, request);

	spdk_bdev_close(desc);  /* [한국어] lib 가 내부 ref 보유 — close 해도 작업 진행. */

cleanup:
	free_rpc_bdev_get_histogram_request(&req);  /* [한국어] strdup 된 name 해제. */
}

SPDK_RPC_REGISTER("bdev_get_histogram", rpc_bdev_get_histogram, SPDK_RPC_RUNTIME)
