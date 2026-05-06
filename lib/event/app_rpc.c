/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 */

/*
 * [한국어 설명] SPDK 앱 / 스레드 / 리액터 / 스케줄러 / 거버너 관련 JSON-RPC 핸들러 모음 (app_rpc.c)
 *
 * === 파일의 역할 ===
 * SPDK 응용 프로그램의 "앱 단위" 정보 (프로세스 정보, 리액터 / 스레드 통계, 스케줄러
 * 와 거버너 상태 등) 를 외부에서 JSON-RPC 로 조회하거나 변경하기 위한 핸들러
 * 들을 한 곳에 모아둔 파일이다. 노출되는 RPC 들은 다음과 같다:
 *   - spdk_kill_instance              : 데몬에 시그널 전송 (SIGTERM 등) → 종료/리로드
 *   - framework_monitor_context_switch: 컨텍스트 스위치 모니터 on/off 및 상태 조회
 *   - thread_get_stats                : 모든 spdk_thread 의 busy/idle/poller 통계
 *   - thread_get_pollers              : 모든 spdk_thread 의 poller 목록
 *   - thread_get_io_channels          : 모든 spdk_thread 가 보유한 io_channel 목록
 *   - framework_get_reactors          : 모든 reactor 의 상태 + 그 위 lw_thread 목록
 *   - framework_set_scheduler         : 스케줄러 교체 (예: static ↔ dynamic) + 옵션
 *   - framework_get_scheduler         : 현재 스케줄러 / 거버너 / 스케줄링 코어 조회
 *   - framework_get_governor          : 현재 거버너 + 코어별 가용/현재 주파수 조회
 *   - scheduler_set_options           : isolated_core_mask, scheduling_core 변경
 *   - thread_set_cpumask              : 특정 spdk_thread 의 cpumask 변경 (cross-thread 메시지)
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 의 실행 모델은 아래와 같다:
 *   spdk_app_start()
 *     → 각 lcore 에 reactor 가 주기적으로 polling
 *       → reactor 위에 N 개의 spdk_thread (lw_thread 로 등록) 가 살고
 *         각 thread 는 active_pollers / timed_pollers / paused_pollers / io_channels 보유.
 *       → 스케줄러 (static / dynamic 등) 가 spdk_thread → reactor 매핑 결정.
 *       → 거버너 가 lcore 의 CPU 주파수 (DVFS) 제어.
 *
 * 본 파일의 RPC 들은 위 구조의 상태를 직렬화하여 외부에 노출하거나, 일부 상태
 * (스케줄러 / cpumask / 스케줄링 코어) 를 변경한다. SPDK_RPC_REGISTER constructor 가
 * 자동 등록을 하므로 명시적 init 호출 없이도 노출된다.
 *
 * === 타 모듈과의 연결 ===
 *  - lib/jsonrpc/, include/spdk/rpc.h: SPDK_RPC_REGISTER, spdk_jsonrpc_*,
 *    spdk_json_decode/write_*. 본 파일의 모든 핸들러는 이 인프라 위에서 동작.
 *  - lib/thread/: spdk_thread, spdk_poller, spdk_io_channel API. 통계 조회용.
 *  - lib/event/reactor.c, spdk_internal/event.h: spdk_reactor 구조 / spdk_for_each_reactor.
 *  - lib/scheduler/: spdk_scheduler_set/get, spdk_scheduler_get_period, isolated_core_mask 등.
 *  - lib/env_dpdk/, include/spdk/env.h: spdk_env_get_current_core, SPDK_ENV_FOREACH_CORE,
 *    spdk_get_ticks/Hz, spdk_get_tid (gettid wrapping).
 *  - app_get_proc_stat / scheduler_get_isolated_core_mask / scheduler_set_isolated_core_mask /
 *    scheduler_set_*_lcore 등은 event_internal.h 가 제공하는 내부 API.
 *
 *  데이터 흐름 (예: framework_get_reactors):
 *   client → JSON-RPC method "framework_get_reactors"
 *     → rpc_framework_get_reactors() 가 ctx 할당 + JSON 헤더 작성.
 *       → spdk_for_each_reactor(_rpc_framework_get_reactors, ctx, NULL, done)
 *         → 각 reactor 를 그 reactor 자신의 thread 에서 순차 실행 (cross-core 직렬화).
 *           → _rpc_framework_get_reactors() 가 자기 reactor 정보 + lw_thread 목록을 JSON 에 기록.
 *         → 마지막에 done 콜백이 응답 전송 + ctx free.
 *
 * === 주요 함수/구조체 요약 ===
 *  - rpc_spdk_kill_instance: 시그널 이름 → 시그널 번호 매핑 후 자기 자신에 kill().
 *  - rpc_framework_monitor_context_switch: spdk_framework_(enable|disable)_context_switch_monitor.
 *  - struct rpc_get_stats_ctx: thread/reactor 순회용 임시 컨텍스트 (request, json writer, now).
 *  - rpc_thread_get_stats / pollers / io_channels: spdk_for_each_thread 를 통한 thread 순회.
 *  - rpc_framework_get_reactors: spdk_for_each_reactor 를 통한 reactor 순회.
 *  - rpc_framework_set_scheduler: 스케줄러 교체 + period 설정 + 추가 옵션 set_opts 호출.
 *  - rpc_framework_get_scheduler / governor: 현재 상태 덤프.
 *  - rpc_scheduler_set_options: isolated_core_mask / scheduling_core 변경 (STARTUP only).
 *  - rpc_thread_set_cpumask: thread 의 cpumask 변경 - 대상 thread 에서 실행해야 하므로
 *      spdk_thread_send_msg 로 cross-thread call 후 원래 thread 로 돌아와 응답.
 *  - SPDK_LOG_REGISTER_COMPONENT(app_rpc): 본 파일의 디버그 컴포넌트 등록.
 */

#include "spdk/stdinc.h"   /* [한국어] 표준 C 헤더 (string.h, stdlib.h 등) 일괄. */

#include "spdk/event.h"    /* [한국어] spdk_app_get_core_mask, spdk_framework_*context_switch* 공개 API. */
#include "spdk/rpc.h"      /* [한국어] SPDK_RPC_REGISTER, spdk_jsonrpc_request, send_*_response. */
#include "spdk/string.h"   /* [한국어] spdk_strtol, spdk_strerror 등 SPDK 문자열 헬퍼. */
#include "spdk/util.h"     /* [한국어] SPDK_COUNTOF, offsetof. */
#include "spdk/env.h"      /* [한국어] spdk_env_get_current_core, SPDK_ENV_FOREACH_CORE, spdk_get_ticks*, spdk_get_tid. */
#include "spdk/scheduler.h" /* [한국어] spdk_scheduler_set/get/period, scheduler_get_isolated_core_mask 등. */
#include "spdk/thread.h"   /* [한국어] spdk_thread, spdk_poller, spdk_io_channel, spdk_for_each_thread, spdk_thread_send_msg. */
#include "spdk/json.h"     /* [한국어] spdk_json_write_*, spdk_json_decode_object*. */

#include "spdk/log.h"             /* [한국어] SPDK_DEBUGLOG, SPDK_ERRLOG. */
#include "spdk_internal/event.h"  /* [한국어] 내부 reactor / app_get_proc_stat 등. */
#include "spdk_internal/thread.h" /* [한국어] spdk_thread_get_ctx 등 thread 내부 API. */
#include "event_internal.h"       /* [한국어] spdk_lw_thread, scheduler_set_isolated_core_mask 등 lib/event 내부. */

/*
 * [한국어]
 * struct rpc_spdk_kill_instance - "spdk_kill_instance" RPC 의 입력 디코딩 버퍼.
 *
 * 필드:
 *  - sig_name: 시그널 이름 ("SIGTERM" 등) 또는 숫자 문자열 ("15"). 핸들러가 매핑.
 *  메모리: free_rpc_spdk_kill_instance 가 해제.
 */
struct rpc_spdk_kill_instance {
	char *sig_name;
	/* [한국어] 사용자가 지정한 시그널 이름/번호 문자열.
	 * 설정자: spdk_json_decode_object.
	 * 읽는 자: signals[] 배열과 strcmp / spdk_strtol 비교.
	 * 메모리 소유권: free_rpc_spdk_kill_instance 가 해제. */
};

/*
 * [한국어]
 * free_rpc_spdk_kill_instance - 위 구조체의 동적 문자열 해제.
 */
static void
free_rpc_spdk_kill_instance(struct rpc_spdk_kill_instance *req)
{
	free(req->sig_name); /* [한국어] decode_string 이 strdup 한 메모리. */
}

/*
 * [한국어]
 * rpc_spdk_kill_instance_decoders - "sig_name" 문자열 키 하나만 받는 디코더 표.
 */
static const struct spdk_json_object_decoder rpc_spdk_kill_instance_decoders[] = {
	{"sig_name", offsetof(struct rpc_spdk_kill_instance, sig_name), spdk_json_decode_string},
	/* [한국어] 필수 키 sig_name. */
};

/*
 * [한국어]
 * rpc_spdk_kill_instance - "spdk_kill_instance" RPC 핸들러.
 *
 * 자기 자신 (PID = getpid()) 에게 시그널을 보내 데몬을 종료/일시중지/리로드한다.
 * 매핑된 시그널만 허용 (SIGINT/SIGTERM/SIGQUIT/SIGHUP/SIGKILL/SIGUSR1).
 *
 * 동작:
 *  1) JSON 으로 sig_name 디코딩.
 *  2) signals[] 배열을 strcmp 또는 정수 매칭으로 탐색.
 *  3) 매칭 없으면 invalid.
 *  4) free 후 kill(getpid(), signal) 호출. 자기에게 시그널을 보낸 뒤 응답.
 *
 * 주의: SIGTERM 등은 보통 spdk_app_stop 으로 이어져 데몬이 곧 종료되지만,
 *       응답을 먼저 보내려고 free + kill 후에 send_bool_response 를 호출한다.
 *       SIGKILL 의 경우 응답 송신 전에 죽을 수 있다.
 *
 * 등록 phase: RUNTIME 만.
 */
static void
rpc_spdk_kill_instance(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	/* [한국어] 허용된 시그널 이름 ↔ 번호 매핑 표. SPDK 데몬에 의미 있는 시그널만 노출. */
	static const struct {
		const char	*signal_string; /* [한국어] 사람이 입력하는 이름 (예: "SIGTERM"). */
		int32_t		signal;         /* [한국어] 실제 시그널 번호 (예: SIGTERM == 15). */
	} signals[] = {
		{"SIGINT",	SIGINT},   /* [한국어] Ctrl+C - 보통 graceful 종료 트리거. */
		{"SIGTERM",	SIGTERM},  /* [한국어] systemd 표준 종료 요청. */
		{"SIGQUIT",	SIGQUIT},  /* [한국어] core dump 와 함께 종료. */
		{"SIGHUP",	SIGHUP},   /* [한국어] 데몬 reload 관용 (SPDK 는 종료에 매핑). */
		{"SIGKILL",	SIGKILL},  /* [한국어] 즉시 종료 - 핸들 불가. */
		{"SIGUSR1",	SIGUSR1},  /* [한국어] 응용 자체 정의 - 일부 SPDK 핸들러가 처리. */
	};
	size_t i, sig_count;       /* [한국어] i = 매칭 인덱스 / sig_count = 표 길이. */
	int signal;                /* [한국어] 사용자가 숫자 문자열을 보낸 경우 그 정수값. */
	struct rpc_spdk_kill_instance req = {}; /* [한국어] 입력 디코딩 버퍼. */

	if (spdk_json_decode_object(params, rpc_spdk_kill_instance_decoders,
				    SPDK_COUNTOF(rpc_spdk_kill_instance_decoders),
				    &req)) {
		SPDK_DEBUGLOG(app_rpc, "spdk_json_decode_object failed\n"); /* [한국어] 디코딩 실패 - 디버그 로그만 남기고 invalid 응답. */
		goto invalid;
	}

	sig_count = SPDK_COUNTOF(signals);          /* [한국어] 6개. */
	signal = spdk_strtol(req.sig_name, 10);      /* [한국어] 숫자 문자열 ("15") 인 경우를 위한 정수 변환. 비숫자면 음수 또는 0. */
	for (i = 0 ; i < sig_count; i++) {
		/* [한국어] 이름 문자열 매치 OR 정수 매치. 둘 중 하나면 OK. */
		if (strcmp(req.sig_name, signals[i].signal_string) == 0 ||
		    signal == signals[i].signal) {
			break;
		}
	}

	if (i == sig_count) {
		/* [한국어] 끝까지 못 찾음 - 허용되지 않은 시그널. */
		goto invalid;
	}

	SPDK_DEBUGLOG(app_rpc, "sending signal %d\n", signals[i].signal); /* [한국어] 디버그용 추적. */
	free_rpc_spdk_kill_instance(&req); /* [한국어] kill() 전에 메모리 해제 - SIGKILL 등 즉시 종료 시 누수 방지. */
	kill(getpid(), signals[i].signal); /* [한국어] POSIX kill(2): 자기 PID 에 시그널 송신.
	                                     *  SIGKILL 외에는 SPDK 의 시그널 핸들러가 graceful shutdown 처리. */

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 성공 응답. SIGKILL 이면 도달 못 할 수도. */
	return;

invalid:
	/* [한국어] 디코딩 실패 또는 알 수 없는 시그널. -32602 응답 후 메모리 정리. */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_spdk_kill_instance(&req);
}
SPDK_RPC_REGISTER("spdk_kill_instance", rpc_spdk_kill_instance, SPDK_RPC_RUNTIME)


/*
 * [한국어]
 * struct rpc_framework_monitor_context_switch - 컨텍스트 스위치 모니터 RPC 입력.
 *
 * 필드:
 *  - enabled: true 면 모니터 켜기, false 면 끄기. RPC 가 optional 인자로 받을 수도 있음.
 */
struct rpc_framework_monitor_context_switch {
	bool enabled;
	/* [한국어] 컨텍스트 스위치 모니터 토글 값. true=켜기. false=끄기.
	 * 설정자: spdk_json_decode_object.
	 * 읽는 자: spdk_framework_enable_context_switch_monitor. */
};

static const struct spdk_json_object_decoder rpc_framework_monitor_context_switch_decoders[] = {
	{"enabled", offsetof(struct rpc_framework_monitor_context_switch, enabled), spdk_json_decode_bool},
	/* [한국어] enabled bool 키. 본 RPC 는 params 가 NULL 이어도 허용 (현재 상태만 조회). */
};

/*
 * [한국어]
 * rpc_framework_monitor_context_switch - 컨텍스트 스위치 모니터 on/off + 상태 조회.
 *
 * 이 RPC 는 dual-purpose:
 *  - params 가 있으면 enabled 값을 적용.
 *  - 항상 현재 상태를 응답에 담아 반환.
 *
 * 컨텍스트 스위치 모니터는 SPDK 가 (polling 기반이므로) 의도치 않은 컨텍스트 스위치
 * (페이지 폴트, 시그널, 인터럽트 등) 를 감지해 경고하는 기능. 운영 시에는 끄는 것이 보통.
 *
 * 응답 JSON: {"enabled": true|false}
 */
static void
rpc_framework_monitor_context_switch(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	struct rpc_framework_monitor_context_switch req = {}; /* [한국어] 입력 디코딩 버퍼. */
	struct spdk_json_write_ctx *w;                         /* [한국어] 응답 빌더. */

	if (params != NULL) {
		/* [한국어] params 가 있으면 enabled 적용. params 가 NULL 이면 조회만. */
		if (spdk_json_decode_object(params, rpc_framework_monitor_context_switch_decoders,
					    SPDK_COUNTOF(rpc_framework_monitor_context_switch_decoders),
					    &req)) {
			SPDK_DEBUGLOG(app_rpc, "spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}

		spdk_framework_enable_context_switch_monitor(req.enabled); /* [한국어] 모니터 토글. lib/event 가 reactor 별 동작 변경. */
	}

	w = spdk_jsonrpc_begin_result(request);                                                 /* [한국어] result 빌더 시작. */
	spdk_json_write_object_begin(w);                                                        /* [한국어] "{" */

	spdk_json_write_named_bool(w, "enabled", spdk_framework_context_switch_monitor_enabled()); /* [한국어] 현재 상태 조회 후 응답에 포함. */

	spdk_json_write_object_end(w);    /* [한국어] "}" */
	spdk_jsonrpc_end_result(request, w); /* [한국어] 응답 송신. */
}

SPDK_RPC_REGISTER("framework_monitor_context_switch", rpc_framework_monitor_context_switch,
		  SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_get_stats_ctx - thread/reactor 순회용 비동기 컨텍스트.
 *
 * SPDK thread/reactor 순회 API 는 비동기적이며 (spdk_for_each_thread/reactor),
 * 각 step 콜백과 done 콜백 사이에 상태를 전달해야 하므로 별도 ctx 구조가 필요.
 *
 * 필드:
 *  - request: 원본 RPC 요청 핸들. done 콜백이 응답할 때 사용.
 *  - w: JSON 응답 빌더 - 모든 step 콜백이 같은 빌더에 추가 기록.
 *  - now: 시작 시점 tick (framework_get_reactors 가 elapsed 계산에 사용).
 *
 * 메모리 소유권: 시작 함수에서 calloc, done 콜백에서 free.
 * 동기화: 순회는 SPDK 가 직렬화 보장 (각 thread/reactor 위에서 순차 실행).
 */
struct rpc_get_stats_ctx {
	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답 송신 시 사용할 RPC 요청 핸들.
	 * 설정자: rpc_thread_get_stats_for_each / rpc_framework_get_reactors.
	 * 읽는 자: 모든 done/step 콜백.
	 * 동기화: 순회 동안 단일 시점만 접근 (SPDK 가 직렬화). */

	struct spdk_json_write_ctx *w;
	/* [한국어] 응답 JSON 빌더. 모든 step 콜백이 추가 기록.
	 * 설정자: spdk_jsonrpc_begin_result.
	 * 읽는 자: 각 step + done.
	 * 동기화: 직렬 실행 보장으로 lock 불필요. */

	uint64_t now;
	/* [한국어] 시작 시 캡처한 spdk_get_ticks(). elapsed 계산에 사용 (framework_get_reactors).
	 * 단조 증가 tick 카운터. */
};

/*
 * [한국어]
 * rpc_thread_get_stats_done - thread 순회 종료 콜백.
 *
 * spdk_for_each_thread 가 모든 thread 처리 완료 후 호출. 응답 마무리 + ctx 해제.
 *
 * 호출 컨텍스트: 원래 spdk_for_each_thread 를 시작한 thread 로 복귀하여 호출됨.
 */
static void
rpc_thread_get_stats_done(void *arg)
{
	struct rpc_get_stats_ctx *ctx = arg; /* [한국어] for_each_thread 가 인자로 그대로 넘김. */

	spdk_json_write_array_end(ctx->w);    /* [한국어] "threads" 배열 닫기. */
	spdk_json_write_object_end(ctx->w);   /* [한국어] 루트 객체 닫기. */
	spdk_jsonrpc_end_result(ctx->request, ctx->w); /* [한국어] 응답 송신. */

	free(ctx); /* [한국어] 컨텍스트 해제 - 한 RPC 라이프사이클 종료. */
}

/*
 * [한국어]
 * rpc_thread_get_stats_for_each - thread 순회 RPC 의 공통 부트스트랩.
 *
 * @request: 원본 RPC 요청.
 * @fn: 각 thread 에서 실행할 step 콜백 (_rpc_thread_get_stats / pollers / io_channels 중 하나).
 *
 * 응답 헤더 (tick_rate + threads 배열 시작) 까지 작성한 뒤 spdk_for_each_thread 시작.
 * 마무리는 rpc_thread_get_stats_done 이 담당.
 *
 * spdk_for_each_thread 의미:
 *  - 등록된 모든 spdk_thread 에 대해 (각자 자기 thread 컨텍스트로 메시지 전달하여)
 *    순차적으로 fn(ctx) 를 실행한다. 즉 각 thread 의 통계는 그 thread 에서 직접 읽힘.
 *  - 모든 thread 처리 후 done(ctx) 가 호출된다.
 *  - 이 패턴 덕분에 thread 별 자료구조에 락 없이 접근 가능.
 */
static void
rpc_thread_get_stats_for_each(struct spdk_jsonrpc_request *request, spdk_msg_fn fn)
{
	struct rpc_get_stats_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx)); /* [한국어] 비동기 라이프사이클 동안 살아있어야 하므로 heap 할당. */
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error");
		return;
	}
	ctx->request = request; /* [한국어] done 에서 응답에 사용. */

	ctx->w = spdk_jsonrpc_begin_result(ctx->request);   /* [한국어] result JSON 빌더 시작. */
	spdk_json_write_object_begin(ctx->w);               /* [한국어] 루트 "{" */
	spdk_json_write_named_uint64(ctx->w, "tick_rate", spdk_get_ticks_hz()); /* [한국어] tick → 초 환산용 (TSC Hz). */
	spdk_json_write_named_array_begin(ctx->w, "threads"); /* [한국어] "threads": [ ... ] 시작. */

	/* [한국어] 모든 thread 에 fn 을 (각 thread context 로) 전달, 끝나면 done 호출. */
	spdk_for_each_thread(fn, ctx, rpc_thread_get_stats_done);
}

/*
 * [한국어]
 * _rpc_thread_get_stats - 각 spdk_thread 위에서 실행되는 step 콜백 (thread_get_stats).
 *
 * 동작:
 *  1) 현재 thread (= spdk_get_thread()) 의 active/timed/paused poller 개수 카운트.
 *  2) busy/idle tsc 통계 조회.
 *  3) thread 의 cpumask 와 앱 전체 core_mask 의 AND 결과를 cpumask 로 출력 (실제 사용 가능한 코어 표시).
 *  4) JSON 한 객체로 출력.
 *
 * 실행 컨텍스트: 해당 spdk_thread 자신. spdk_get_thread() 가 현재 thread 를 반환.
 *  - 이 thread 의 poller 리스트 / io_channel 리스트 등 thread-local 자료구조에 락 없이 접근 가능.
 *
 * cpumask AND 의 의미:
 *  - thread cpumask: 이 thread 가 어디든 갈 수 있는지의 허용 집합.
 *  - app core_mask:  앱이 실제 사용하는 코어 집합 (--cpumask).
 *  - AND: "이 thread 가 실제로 갈 수 있는 코어들" - 보고용.
 */
static void
_rpc_thread_get_stats(void *arg)
{
	struct rpc_get_stats_ctx *ctx = arg;          /* [한국어] for_each_thread 가 넘긴 컨텍스트. */
	struct spdk_thread *thread = spdk_get_thread(); /* [한국어] 현재 실행 중인 spdk_thread (콜백이 그 위에서 실행됨). */
	struct spdk_cpuset tmp_mask = {};              /* [한국어] cpumask AND 결과 임시 저장. */
	struct spdk_poller *poller;                    /* [한국어] poller 순회 커서. */
	struct spdk_thread_stats stats;                /* [한국어] busy_tsc/idle_tsc 통계. */
	uint64_t active_pollers_count = 0;             /* [한국어] 매 tick 호출되는 poller 수. */
	uint64_t timed_pollers_count = 0;              /* [한국어] 주기 (us 기준) 호출 poller 수. */
	uint64_t paused_pollers_count = 0;             /* [한국어] 일시 중지된 poller 수. */

	/* [한국어] active_pollers 카운트 (단순 길이 세기). first/next 패턴으로 thread-local 리스트 순회. */
	for (poller = spdk_thread_get_first_active_poller(thread); poller != NULL;
	     poller = spdk_thread_get_next_active_poller(poller)) {
		active_pollers_count++;
	}

	/* [한국어] timed_pollers 카운트. */
	for (poller = spdk_thread_get_first_timed_poller(thread); poller != NULL;
	     poller = spdk_thread_get_next_timed_poller(poller)) {
		timed_pollers_count++;
	}

	/* [한국어] paused_pollers 카운트. */
	for (poller = spdk_thread_get_first_paused_poller(thread); poller != NULL;
	     poller = spdk_thread_get_next_paused_poller(poller)) {
		paused_pollers_count++;
	}

	if (0 == spdk_thread_get_stats(&stats)) { /* [한국어] busy/idle tsc 조회 성공 시에만 보고. */
		spdk_json_write_object_begin(ctx->w);   /* [한국어] 한 thread 객체 시작. */
		spdk_json_write_named_string(ctx->w, "name", spdk_thread_get_name(thread)); /* [한국어] thread 이름. */
		spdk_json_write_named_uint64(ctx->w, "id", spdk_thread_get_id(thread));     /* [한국어] thread 고유 ID. */
		spdk_cpuset_copy(&tmp_mask, spdk_app_get_core_mask());                      /* [한국어] tmp = app core_mask. */
		spdk_cpuset_and(&tmp_mask, spdk_thread_get_cpumask(thread));                 /* [한국어] tmp &= thread cpumask. */
		spdk_json_write_named_string(ctx->w, "cpumask", spdk_cpuset_fmt(&tmp_mask)); /* [한국어] AND 결과를 hex 문자열로. */
		spdk_json_write_named_uint64(ctx->w, "busy", stats.busy_tsc); /* [한국어] busy tick 누계. */
		spdk_json_write_named_uint64(ctx->w, "idle", stats.idle_tsc); /* [한국어] idle tick 누계. */
		spdk_json_write_named_uint64(ctx->w, "active_pollers_count", active_pollers_count);
		spdk_json_write_named_uint64(ctx->w, "timed_pollers_count", timed_pollers_count);
		spdk_json_write_named_uint64(ctx->w, "paused_pollers_count", paused_pollers_count);
		spdk_json_write_object_end(ctx->w); /* [한국어] thread 객체 끝. */
	}
}

/*
 * [한국어]
 * rpc_thread_get_stats - "thread_get_stats" RPC 핸들러.
 *
 * 인자 없음. 응답 (JSON):
 *   {"tick_rate": <hz>, "threads": [{ "name", "id", "cpumask", "busy", "idle", ... }, ...]}
 *
 * 등록 phase: RUNTIME.
 */
static void
rpc_thread_get_stats(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	if (params) {
		/* [한국어] 인자 없는 RPC. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'thread_get_stats' requires no arguments");
		return;
	}

	rpc_thread_get_stats_for_each(request, _rpc_thread_get_stats); /* [한국어] 공통 부트스트랩 + step=stats. */
}

SPDK_RPC_REGISTER("thread_get_stats", rpc_thread_get_stats, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_get_poller - 한 spdk_poller 의 정보를 JSON 객체로 출력.
 *
 * @poller: 출력 대상 poller.
 * @w: 응답 빌더.
 *
 * 출력 필드: name, id, state, run_count, busy_count, period_ticks(있으면).
 *
 * 호출 컨텍스트: 해당 poller 를 소유한 spdk_thread 위 (소유 thread-local 자료에 직접 접근).
 */
static void
rpc_get_poller(struct spdk_poller *poller, struct spdk_json_write_ctx *w)
{
	struct spdk_poller_stats stats; /* [한국어] run_count/busy_count 통계. */
	uint64_t period_ticks;          /* [한국어] timed poller 의 주기 (ticks). 0 이면 active poller. */

	period_ticks = spdk_poller_get_period_ticks(poller); /* [한국어] period 반환 (active poller 는 0). */
	spdk_poller_get_stats(poller, &stats);                /* [한국어] 통계 조회. */

	spdk_json_write_object_begin(w);                                       /* [한국어] poller 객체 시작. */
	spdk_json_write_named_string(w, "name", spdk_poller_get_name(poller)); /* [한국어] poller 이름 (디버그용 식별). */
	spdk_json_write_named_uint64(w, "id", spdk_poller_get_id(poller));     /* [한국어] poller 고유 ID. */
	spdk_json_write_named_string(w, "state", spdk_poller_get_state_str(poller)); /* [한국어] WAITING/RUNNING 등 상태 문자열. */
	spdk_json_write_named_uint64(w, "run_count", stats.run_count);         /* [한국어] 호출 횟수 누계. */
	spdk_json_write_named_uint64(w, "busy_count", stats.busy_count);       /* [한국어] busy 반환 횟수 (실제로 작업한 호출). */
	if (period_ticks) {
		/* [한국어] timed poller 인 경우만 주기 정보 추가. active poller 는 매 tick 호출되므로 의미 없음. */
		spdk_json_write_named_uint64(w, "period_ticks", period_ticks);
	}
	spdk_json_write_object_end(w); /* [한국어] poller 객체 끝. */
}

/*
 * [한국어]
 * _rpc_thread_get_pollers - 각 thread 위에서 실행되어 그 thread 의 모든 poller 를 출력.
 *
 * 응답 JSON 형태 (한 thread 당):
 *   {"name", "id", "active_pollers": [...], "timed_pollers": [...], "paused_pollers": [...]}
 *
 * 실행 컨텍스트: 해당 spdk_thread.
 */
static void
_rpc_thread_get_pollers(void *arg)
{
	struct rpc_get_stats_ctx *ctx = arg;             /* [한국어] for_each_thread 컨텍스트. */
	struct spdk_thread *thread = spdk_get_thread();   /* [한국어] 현재 thread. */
	struct spdk_poller *poller;                       /* [한국어] 순회 커서. */

	spdk_json_write_object_begin(ctx->w);                                                   /* [한국어] thread 객체 시작. */
	spdk_json_write_named_string(ctx->w, "name", spdk_thread_get_name(thread));             /* [한국어] thread 이름. */
	spdk_json_write_named_uint64(ctx->w, "id", spdk_thread_get_id(thread));                 /* [한국어] thread ID. */

	spdk_json_write_named_array_begin(ctx->w, "active_pollers"); /* [한국어] active_pollers 배열 시작. */
	for (poller = spdk_thread_get_first_active_poller(thread); poller != NULL;
	     poller = spdk_thread_get_next_active_poller(poller)) {
		rpc_get_poller(poller, ctx->w); /* [한국어] 각 poller 객체 출력. */
	}
	spdk_json_write_array_end(ctx->w); /* [한국어] active 배열 닫기. */

	spdk_json_write_named_array_begin(ctx->w, "timed_pollers"); /* [한국어] 주기적 poller. */
	for (poller = spdk_thread_get_first_timed_poller(thread); poller != NULL;
	     poller = spdk_thread_get_next_timed_poller(poller)) {
		rpc_get_poller(poller, ctx->w);
	}
	spdk_json_write_array_end(ctx->w);

	spdk_json_write_named_array_begin(ctx->w, "paused_pollers"); /* [한국어] 일시중지 poller. */
	for (poller = spdk_thread_get_first_paused_poller(thread); poller != NULL;
	     poller = spdk_thread_get_next_paused_poller(poller)) {
		rpc_get_poller(poller, ctx->w);
	}
	spdk_json_write_array_end(ctx->w);

	spdk_json_write_object_end(ctx->w); /* [한국어] thread 객체 닫기. */
}

/*
 * [한국어]
 * rpc_thread_get_pollers - "thread_get_pollers" RPC.
 * 인자 없음. 모든 thread 의 poller 목록 덤프.
 */
static void
rpc_thread_get_pollers(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	if (params) {
		/* [한국어] 인자 없는 RPC. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'thread_get_pollers' requires no arguments");
		return;
	}

	rpc_thread_get_stats_for_each(request, _rpc_thread_get_pollers); /* [한국어] thread 순회 + step=pollers. */
}

SPDK_RPC_REGISTER("thread_get_pollers", rpc_thread_get_pollers, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_get_io_channel - 한 io_channel 의 정보를 JSON 객체로 출력.
 *
 * @ch: 출력 대상 채널.
 * @w: 응답 빌더.
 *
 * 출력 필드: name (io_device 이름), ref (참조 카운트).
 *
 * io_channel 이란?
 *   spdk_io_device_register 로 등록된 디바이스의, 특정 thread 전용 채널.
 *   같은 디바이스라도 thread 마다 독립된 채널 (thread-local 큐 등) 을 가지므로
 *   lock-free 가능. 본 RPC 는 thread 마다 그 thread 가 보유한 채널들을 보여줌.
 */
static void
rpc_get_io_channel(struct spdk_io_channel *ch, struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);                                                      /* [한국어] channel 객체 시작. */
	spdk_json_write_named_string(w, "name", spdk_io_channel_get_io_device_name(ch));      /* [한국어] 디바이스 이름. */
	spdk_json_write_named_uint32(w, "ref", spdk_io_channel_get_ref_count(ch));            /* [한국어] 참조 카운트. */
	spdk_json_write_object_end(w);                                                        /* [한국어] 객체 닫기. */
}

/*
 * [한국어]
 * _rpc_thread_get_io_channels - 각 thread 위에서 실행, 그 thread 의 io_channel 목록 출력.
 */
static void
_rpc_thread_get_io_channels(void *arg)
{
	struct rpc_get_stats_ctx *ctx = arg;            /* [한국어] 컨텍스트. */
	struct spdk_thread *thread = spdk_get_thread(); /* [한국어] 현재 thread. */
	struct spdk_io_channel *ch;                     /* [한국어] 순회 커서. */

	spdk_json_write_object_begin(ctx->w);                                       /* [한국어] thread 객체 시작. */
	spdk_json_write_named_string(ctx->w, "name", spdk_thread_get_name(thread)); /* [한국어] thread 이름. */

	spdk_json_write_named_array_begin(ctx->w, "io_channels");
	for (ch = spdk_thread_get_first_io_channel(thread); ch != NULL;
	     ch = spdk_thread_get_next_io_channel(ch)) {
		rpc_get_io_channel(ch, ctx->w); /* [한국어] 각 채널 객체 출력. */
	}
	spdk_json_write_array_end(ctx->w); /* [한국어] io_channels 배열 닫기. */

	spdk_json_write_object_end(ctx->w); /* [한국어] thread 객체 닫기. */
}

/*
 * [한국어]
 * rpc_thread_get_io_channels - "thread_get_io_channels" RPC.
 */
static void
rpc_thread_get_io_channels(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'thread_get_io_channels' requires no arguments");
		return;
	}

	rpc_thread_get_stats_for_each(request, _rpc_thread_get_io_channels);
}

SPDK_RPC_REGISTER("thread_get_io_channels", rpc_thread_get_io_channels, SPDK_RPC_RUNTIME);

/*
 * [한국어]
 * rpc_framework_get_reactors_done - reactor 순회 종료 콜백.
 *
 * spdk_for_each_reactor 의 done 콜백 시그니처: void(void *arg1, void *arg2).
 *
 * 동작: "reactors" 배열 닫기 + 루트 객체 닫기 + 응답 송신 + ctx 해제.
 */
static void
rpc_framework_get_reactors_done(void *arg1, void *arg2)
{
	struct rpc_get_stats_ctx *ctx = arg1; /* [한국어] arg1 = ctx. arg2 는 사용하지 않음. */

	spdk_json_write_array_end(ctx->w);     /* [한국어] "reactors": [...] 닫기. */
	spdk_json_write_object_end(ctx->w);    /* [한국어] 루트 객체 닫기. */
	spdk_jsonrpc_end_result(ctx->request, ctx->w); /* [한국어] 응답 송신. */

	free(ctx); /* [한국어] 한 RPC 라이프사이클 종료. */
}

/*
 * [한국어] GET_DELTA - 단조 증가 카운터의 음수 underflow 방지 매크로.
 *  end >= start 면 end - start, 아니면 0. tsc 측정 보고에서 일관성 유지를 위해.
 */
#define GET_DELTA(end, start)	(end >= start ? end - start : 0)

/*
 * [한국어]
 * _rpc_framework_get_reactors - 각 reactor 자기 자신에서 실행되는 step 콜백.
 *
 * 동작:
 *  1) 자기 reactor 정보 (lcore, tid, busy/idle, in_interrupt) 출력.
 *  2) /proc/stat 으로 그 lcore 의 usr/sys/irq tick 조회.
 *  3) 거버너가 있으면 현재 주파수 (MHz 환산) 출력.
 *  4) reactor->threads 리스트 (TAILQ) 순회하며 각 lw_thread 의 정보 출력.
 *
 * 실행 컨텍스트: 해당 reactor 의 lcore 위. 그 reactor 의 자료구조 (threads list) 에 lock-free 로 직접 접근.
 *
 * "lw_thread" 는 spdk_thread 의 lightweight wrapper - 한 reactor 위에 등록된 형태로 관리됨.
 */
static void
_rpc_framework_get_reactors(void *arg1, void *arg2)
{
	struct rpc_get_stats_ctx *ctx = arg1;
	uint32_t current_core;       /* [한국어] 자기 lcore 번호. */
	uint32_t curr_core_freq;     /* [한국어] 현재 lcore CPU 주파수 (MHz). */
	uint64_t sys, usr, irq;      /* [한국어] /proc/stat 의 user/system/irq 시간. */
	struct spdk_reactor *reactor;
	struct spdk_lw_thread *lw_thread; /* [한국어] reactor 의 thread 리스트 순회 커서. */
	struct spdk_thread *thread;       /* [한국어] lw_thread 에서 변환한 spdk_thread. */
	struct spdk_cpuset tmp_mask = {}; /* [한국어] cpumask AND 임시. */
	struct spdk_governor *governor;   /* [한국어] 현재 등록된 거버너 (없으면 NULL). */

	current_core = spdk_env_get_current_core(); /* [한국어] 자기 lcore. */
	reactor = spdk_reactor_get(current_core);    /* [한국어] 자기 reactor. */

	assert(reactor != NULL); /* [한국어] 본 콜백은 reactor 위에서 실행되므로 항상 non-NULL. */

	spdk_json_write_object_begin(ctx->w);                                /* [한국어] reactor 객체 시작. */
	spdk_json_write_named_uint32(ctx->w, "lcore", current_core);         /* [한국어] lcore 번호. */
	spdk_json_write_named_uint64(ctx->w, "tid", spdk_get_tid());          /* [한국어] OS-level thread id (gettid()). */
	spdk_json_write_named_uint64(ctx->w, "busy", reactor->busy_tsc);      /* [한국어] reactor busy tick. */
	spdk_json_write_named_uint64(ctx->w, "idle", reactor->idle_tsc);      /* [한국어] reactor idle tick. */
	spdk_json_write_named_bool(ctx->w, "in_interrupt", reactor->in_interrupt); /* [한국어] 현재 인터럽트 모드 여부. */

	if (app_get_proc_stat(current_core, &usr, &sys, &irq) != 0) {
		/* [한국어] /proc/stat 읽기 실패 시 0 으로 보고 (경계 케이스: cgroup, 권한 등). */
		irq = sys = usr = 0;
	}
	spdk_json_write_named_uint64(ctx->w, "irq", irq); /* [한국어] OS 가 본 irq 시간. */
	spdk_json_write_named_uint64(ctx->w, "sys", sys); /* [한국어] kernel 시간. */
	spdk_json_write_named_uint64(ctx->w, "usr", usr); /* [한국어] user 시간. */

	governor = spdk_governor_get(); /* [한국어] 거버너 등록 여부 확인. */
	if (governor != NULL) {
		/* Governor returns core freqs in kHz, we want MHz. */
		/* [한국어] governor 가 kHz 단위로 반환 → 1000 으로 나눠 MHz 로 환산. */
		curr_core_freq = governor->get_core_curr_freq(current_core) / 1000;
		spdk_json_write_named_uint32(ctx->w, "core_freq", curr_core_freq);
	}

	spdk_json_write_named_array_begin(ctx->w, "lw_threads"); /* [한국어] 이 reactor 위 thread 배열 시작. */
	TAILQ_FOREACH(lw_thread, &reactor->threads, link) { /* [한국어] reactor 의 TAILQ - 같은 lcore 위에서 lock 없이 순회 가능. */
		thread = spdk_thread_get_from_ctx(lw_thread); /* [한국어] lw_thread → spdk_thread 변환 (container_of 패턴의 inverse). */

		spdk_json_write_object_begin(ctx->w);
		spdk_json_write_named_string(ctx->w, "name", spdk_thread_get_name(thread));
		spdk_json_write_named_uint64(ctx->w, "id", spdk_thread_get_id(thread));
		spdk_cpuset_copy(&tmp_mask, spdk_app_get_core_mask());                    /* [한국어] tmp = app core mask. */
		spdk_cpuset_and(&tmp_mask, spdk_thread_get_cpumask(thread));               /* [한국어] tmp &= thread cpumask. */
		spdk_json_write_named_string(ctx->w, "cpumask", spdk_cpuset_fmt(&tmp_mask)); /* [한국어] AND 결과. */
		spdk_json_write_named_uint64(ctx->w, "elapsed",
					     GET_DELTA(ctx->now, lw_thread->tsc_start)); /* [한국어] thread 가 이 reactor 에 들어온 이후 경과 tick. */
		spdk_json_write_object_end(ctx->w);
	}
	spdk_json_write_array_end(ctx->w); /* [한국어] lw_threads 배열 닫기. */

	spdk_json_write_object_end(ctx->w); /* [한국어] reactor 객체 닫기. */
}

/*
 * [한국어]
 * rpc_framework_get_reactors - "framework_get_reactors" RPC 핸들러.
 *
 * 응답 JSON:
 *   {"tick_rate": <hz>, "pid": <pid>, "reactors": [{ "lcore", "tid", "busy", "idle",
 *      "in_interrupt", "irq", "sys", "usr", "core_freq"?, "lw_threads": [...] }, ...]}
 *
 * 동작:
 *  1) ctx 할당 + 응답 헤더 작성 (tick_rate, pid, reactors 배열 시작).
 *  2) spdk_for_each_reactor 로 모든 reactor 에 step 함수 전달.
 *  3) done 콜백이 응답 마무리.
 */
static void
rpc_framework_get_reactors(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_get_stats_ctx *ctx;

	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "`framework_get_reactors` requires no arguments");
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error");
		return;
	}

	ctx->now = spdk_get_ticks();    /* [한국어] elapsed 계산 기준점 (TSC). */
	ctx->request = request;
	ctx->w = spdk_jsonrpc_begin_result(ctx->request);

	spdk_json_write_object_begin(ctx->w);                              /* [한국어] 루트 "{". */
	spdk_json_write_named_uint64(ctx->w, "tick_rate", spdk_get_ticks_hz()); /* [한국어] tick → 초 환산. */
	spdk_json_write_named_uint64(ctx->w, "pid", getpid());             /* [한국어] 데몬 PID. */
	spdk_json_write_named_array_begin(ctx->w, "reactors");             /* [한국어] reactors 배열 시작. */

	/* [한국어] 모든 reactor 위에서 _rpc_framework_get_reactors 직렬 실행, 끝나면 done 콜백. */
	spdk_for_each_reactor(_rpc_framework_get_reactors, ctx, NULL,
			      rpc_framework_get_reactors_done);
}

SPDK_RPC_REGISTER("framework_get_reactors", rpc_framework_get_reactors, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_set_scheduler_ctx - "framework_set_scheduler" RPC 의 입력.
 *
 * 필드:
 *  - name: 사용할 스케줄러 이름 ("static", "dynamic", "gscheduler" 등).
 *  - period: 스케줄링 주기 (microseconds). 0 = 변경하지 않음 (또는 1초로 기본 세팅).
 */
struct rpc_set_scheduler_ctx {
	char *name;
	/* [한국어] spdk_scheduler_set 에 전달할 이름.
	 * 설정자: spdk_json_decode_object.
	 * 읽는 자: spdk_scheduler_set.
	 * 메모리 소유권: free_rpc_framework_set_scheduler 가 해제. */

	uint64_t period;
	/* [한국어] balance() 콜백 호출 주기 (microseconds). 0 = "지정하지 않음" 으로 처리. */
};

/*
 * [한국어] free_rpc_framework_set_scheduler - name 문자열만 해제 (period 는 동적 메모리 아님).
 */
static void
free_rpc_framework_set_scheduler(struct rpc_set_scheduler_ctx *r)
{
	free(r->name); /* [한국어] decode_string 할당분. */
}

/*
 * [한국어]
 * rpc_framework_set_scheduler_decoders - "name" (필수), "period" (선택) 디코딩 표.
 *
 * "period" 의 4번째 인자 true 는 optional 임을 의미.
 */
static const struct spdk_json_object_decoder rpc_framework_set_scheduler_decoders[] = {
	{"name", offsetof(struct rpc_set_scheduler_ctx, name), spdk_json_decode_string},
	/* [한국어] 필수: 스케줄러 이름. */
	{"period", offsetof(struct rpc_set_scheduler_ctx, period), spdk_json_decode_uint64, true},
	/* [한국어] 선택: 주기 (us). */
};

/*
 * [한국어]
 * rpc_framework_set_scheduler - "framework_set_scheduler" RPC 핸들러.
 *
 * 동작:
 *  1) decode_object (strict) 시도. 실패 시 has_custom_opts=true 로 표시 후 relaxed 재시도.
 *     - 일부 스케줄러 (static 등) 는 추가 옵션 ("mappings") 을 받으므로, 그런 키가 있으면
 *       strict decode 가 실패한다. relaxed 로 다시 디코드하면 추가 키는 무시되며 name/period
 *       만 추출된다. 추가 키들은 나중에 scheduler->set_opts() 가 직접 같은 params 를 본다.
 *  2) period 처리:
 *     - 사용자가 명시 (>0) → spdk_scheduler_set_period(req.period).
 *     - 미지정 (=0) AND 현재 period 도 0 → 기본값 1초 (SPDK_SEC_TO_USEC).
 *     - 그 외 → 그대로 둠.
 *  3) spdk_scheduler_set(name) 으로 스케줄러 교체. 등록 안 된 이름이면 errno 반환.
 *  4) 스케줄러가 set_opts 를 구현했으면 (static 등) 같은 params 를 그대로 전달.
 *  5) set_opts 가 없는데 has_custom_opts (=처음 strict decode 가 실패) 면 invalid.
 *
 * 등록 phase: STARTUP|RUNTIME (런타임 중에도 교체 가능).
 */
static void
rpc_framework_set_scheduler(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_set_scheduler_ctx req = {NULL}; /* [한국어] name=NULL, period=0 으로 시작. */
	struct spdk_scheduler *scheduler;
	bool has_custom_opts = false;              /* [한국어] strict decode 실패 → 추가 키 존재 가능성. */
	int ret;

	/* [한국어] 1단계: strict decode (모르는 키가 있으면 실패). */
	ret = spdk_json_decode_object(params, rpc_framework_set_scheduler_decoders,
				      SPDK_COUNTOF(rpc_framework_set_scheduler_decoders),
				      &req);
	if (ret) {
		has_custom_opts = true;
		/* [한국어] relaxed: 모르는 키는 무시. set_opts 가 처리할 수 있도록 함. */
		ret = spdk_json_decode_object_relaxed(params, rpc_framework_set_scheduler_decoders,
						      SPDK_COUNTOF(rpc_framework_set_scheduler_decoders),
						      &req);
	}
	if (ret) {
		/* [한국어] relaxed 도 실패 → 진짜 invalid params. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto end;
	}

	if (req.period != 0) {
		spdk_scheduler_set_period(req.period); /* [한국어] 사용자 지정 주기 적용. */
	} else if (spdk_scheduler_get_period() == 0) {
		/* User didn't specify a period, and no period has been set
		 * previously, so set it now to 1 second.
		 */
		/* [한국어] 한 번도 설정된 적 없는 경우 기본값 1 초. */
		spdk_scheduler_set_period(SPDK_SEC_TO_USEC);
	}

	ret = spdk_scheduler_set(req.name); /* [한국어] 스케줄러 교체. 미등록 이름이면 errno 반환. */
	if (ret) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(ret)); /* [한국어] errno → 문자열. */
		goto end;
	}

	scheduler = spdk_scheduler_get(); /* [한국어] 새 스케줄러 핸들 (방금 set 한 것). */
	if (scheduler != NULL && scheduler->set_opts != NULL) {
		ret = scheduler->set_opts(params); /* [한국어] static_scheduler 의 mappings 처리 등. */
	} else if (has_custom_opts) {
		/* No custom options are allowed if set_opts are not implemented. */
		/* [한국어] 사용자가 추가 키를 줬는데 스케줄러가 처리 능력이 없으면 거절. */
		ret = -EINVAL;
	}
	if (ret) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(ret));
		goto end;
	}

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 성공. */

end:
	free_rpc_framework_set_scheduler(&req); /* [한국어] req.name 해제. */
}
SPDK_RPC_REGISTER("framework_set_scheduler", rpc_framework_set_scheduler,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_framework_get_scheduler - "framework_get_scheduler" RPC 핸들러.
 *
 * 인자 없음. 응답 (JSON):
 *   {
 *     "scheduler_name": <name>?,
 *     "scheduler_period": <us>,
 *     "isolated_core_mask": <hex string>,
 *     "scheduling_core": <lcore>,
 *     "governor_name": <name>?,
 *     ... 추가 옵션 (스케줄러의 get_opts 가 기록)
 *   }
 *
 * 등록 phase: STARTUP|RUNTIME.
 */
static void
rpc_framework_get_scheduler(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;                                            /* [한국어] 응답 빌더. */
	struct spdk_scheduler *scheduler = spdk_scheduler_get();                  /* [한국어] 현재 스케줄러 (등록 안되어 있으면 NULL). */
	uint64_t scheduler_period = spdk_scheduler_get_period();                  /* [한국어] 현재 주기 us. */
	struct spdk_governor *governor = spdk_governor_get();                     /* [한국어] 현재 거버너. */
	uint32_t scheduling_core = spdk_scheduler_get_scheduling_lcore();         /* [한국어] 스케줄러 thread 가 도는 lcore. */

	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'rpc_get_scheduler' requires no arguments");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	if (scheduler) {
		spdk_json_write_named_string(w, "scheduler_name", scheduler->name); /* [한국어] 이름은 스케줄러가 있을 때만. */
	}
	spdk_json_write_named_uint64(w, "scheduler_period", scheduler_period);
	spdk_json_write_named_string(w, "isolated_core_mask", scheduler_get_isolated_core_mask()); /* [한국어] 격리된 코어 마스크 (스케줄링 대상 외). */
	spdk_json_write_named_uint32(w, "scheduling_core", scheduling_core);
	if (governor != NULL) {
		spdk_json_write_named_string(w, "governor_name", governor->name);
	}

	if (scheduler != NULL && scheduler->get_opts != NULL) {
		scheduler->get_opts(w); /* [한국어] 스케줄러 고유 옵션 출력. dynamic 등이 자체 키를 추가. */
	}

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("framework_get_scheduler", rpc_framework_get_scheduler,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_framework_get_governor - "framework_get_governor" RPC.
 *
 * 인자 없음. 응답:
 *   {
 *     "governor_name": <name>?,
 *     "module_specific": { ... },
 *     "cores": [ { "lcore_id", "available_frequencies": [...], "current_frequency" }, ... ]
 *   }
 *
 * 거버너 (governor) 는 SPDK 가 코어 주파수 (DVFS) 를 동적으로 조절할 때 사용하는
 * 추상화. 보통 cpufreq + userspace governor 와 결합. 등록되어 있지 않으면 빈 객체 반환.
 */
static void
rpc_framework_get_governor(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_governor *governor = spdk_governor_get();

	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'rpc_get_governor' requires no arguments");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);

	if (governor != NULL) {
		uint32_t core, index, num;                /* [한국어] core=lcore 순회용, num=available freq 개수. */
		uint32_t freqs[SPDK_MAX_LCORE_FREQS];     /* [한국어] 코어가 지원하는 주파수 리스트 버퍼. */

		spdk_json_write_named_string(w, "governor_name", governor->name); /* [한국어] 거버너 이름. */

		spdk_json_write_named_object_begin(w, "module_specific"); /* [한국어] 거버너 모듈 고유 정보. */

		governor->dump_info_json(w); /* [한국어] 거버너 자체 콜백 - 자기 데이터 직렬화. */

		spdk_json_write_object_end(w);

		spdk_json_write_named_array_begin(w, "cores"); /* [한국어] 코어 별 정보 배열. */

		SPDK_ENV_FOREACH_CORE(core) { /* [한국어] 앱이 사용하는 모든 lcore 순회. */
			spdk_json_write_object_begin(w);
			spdk_json_write_named_uint32(w, "lcore_id", core);

			memset(freqs, 0, SPDK_MAX_LCORE_FREQS * sizeof(uint32_t)); /* [한국어] 버퍼 초기화. */

			num = governor->get_core_avail_freqs(core, freqs, SPDK_MAX_LCORE_FREQS); /* [한국어] 가용 주파수 N 개 채움. */

			spdk_json_write_named_array_begin(w, "available_frequencies");
			for (index = 0; index < num; index++) {
				spdk_json_write_uint32(w, freqs[index]); /* [한국어] kHz 단위 (거버너 정의 기준). */
			}
			spdk_json_write_array_end(w);

			spdk_json_write_named_uint32(w, "current_frequency", governor->get_core_curr_freq(core)); /* [한국어] 현재 주파수. */
			spdk_json_write_object_end(w);
		}

		spdk_json_write_array_end(w);
	}

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("framework_get_governor", rpc_framework_get_governor, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_set_scheduler_opts_ctx - "scheduler_set_options" RPC 입력.
 *
 * 필드:
 *  - isolated_core_mask: 스케줄러가 건드리지 않을 코어 집합 (16진수 마스크). NULL 가능.
 *  - scheduling_core: 스케줄러 자체가 도는 lcore.
 */
struct rpc_set_scheduler_opts_ctx {
	char *isolated_core_mask;
	/* [한국어] 격리된 코어 hex 문자열. 스케줄러는 이 코어의 thread 를 건드리지 않음. */

	uint32_t scheduling_core;
	/* [한국어] 스케줄러 자체가 실행될 lcore 번호. */
};

/*
 * [한국어] 디코더 표 - 두 키 모두 optional (4번째 인자 true).
 */
static const struct spdk_json_object_decoder rpc_scheduler_set_options_decoders[] = {
	{"isolated_core_mask", offsetof(struct rpc_set_scheduler_opts_ctx, isolated_core_mask), spdk_json_decode_string, true},
	/* [한국어] 16진수 마스크 문자열, optional. */
	{"scheduling_core", offsetof(struct rpc_set_scheduler_opts_ctx, scheduling_core), spdk_json_decode_uint32, true},
	/* [한국어] 스케줄링 lcore, optional. */
};

static void
free_rpc_scheduler_set_options(struct rpc_set_scheduler_opts_ctx *r)
{
	free(r->isolated_core_mask); /* [한국어] 마스크 문자열 해제. */
}

/*
 * [한국어]
 * rpc_scheduler_set_options - "scheduler_set_options" RPC 핸들러.
 *
 * isolated_core_mask 와 scheduling_core 를 변경. 핸들러는 다음을 검증:
 *   - 두 마스크가 겹치면 안 됨 (스케줄러 자체가 격리되어 있으면 자신을 못 도림).
 *   - 마스크 자체가 잘못된 형식이면 거부.
 *   - scheduling_core 가 실제 활성 lcore 인지 검증.
 *
 * 등록 phase: STARTUP 만. 런타임 중에는 변경 불가 (스케줄러 작동 중에 변경하면 안전 보장 어려움).
 */
static void
rpc_scheduler_set_options(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_set_scheduler_opts_ctx req = {NULL};
	struct spdk_cpuset core_mask;

	req.scheduling_core = spdk_scheduler_get_scheduling_lcore(); /* [한국어] 기본값 = 현재 값. 사용자가 안 보내면 그대로. */

	if (spdk_json_decode_object(params, rpc_scheduler_set_options_decoders,
				    SPDK_COUNTOF(rpc_scheduler_set_options_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto end;
	}

	if (req.isolated_core_mask != NULL) {
		spdk_cpuset_parse(&core_mask, req.isolated_core_mask); /* [한국어] hex string → spdk_cpuset 변환. */
		if (spdk_cpuset_get_cpu(&core_mask, req.scheduling_core)) {
			/* [한국어] 스케줄링 lcore 가 격리 마스크에 포함되면 자기 자신을 못 동작 → 거부. */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Scheduling core cannot be included in isolated core mask.\n");
			goto end;
		}
		if (scheduler_set_isolated_core_mask(core_mask) == false) {
			/* [한국어] 마스크 자체가 부적절 (예: 활성 lcore 와 무관) → 거부. */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid isolated core mask\n");
			goto end;
		}
	}

	if (spdk_scheduler_set_scheduling_lcore(req.scheduling_core) == false) {
		/* [한국어] 부적절 lcore (활성 코어 아님 등). */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid scheduling core.\n");
		goto end;
	}

	spdk_jsonrpc_send_bool_response(request, true);
end:
	free_rpc_scheduler_set_options(&req); /* [한국어] isolated_core_mask 문자열 해제. */
}
SPDK_RPC_REGISTER("scheduler_set_options", rpc_scheduler_set_options, SPDK_RPC_STARTUP)

/*
 * [한국어]
 * struct rpc_thread_set_cpumask_ctx - "thread_set_cpumask" RPC 의 비동기 컨텍스트.
 *
 * 이 RPC 는 cross-thread 작업을 수행한다:
 *   - 대상 spdk_thread 의 cpumask 변경은 그 thread 자신이 해야 함 (thread-local 상태 변경).
 *   - 그래서 spdk_thread_send_msg 로 _rpc_thread_set_cpumask 를 대상 thread 에서 실행하고,
 *     완료되면 다시 spdk_thread_send_msg 로 원래 RPC 핸들러 thread 로 돌아와 응답 송신.
 *
 * 필드:
 *  - request: 원본 RPC 핸들 (응답 송신용).
 *  - cpumask: 적용할 cpumask.
 *  - status: spdk_thread_set_cpumask 의 결과 (errno).
 *  - orig_thread: RPC 핸들러가 실행되던 thread (응답을 다시 그 thread 에서 송신해야 jsonrpc 안전).
 */
struct rpc_thread_set_cpumask_ctx {
	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답 송신용 RPC 핸들. orig_thread 에서 사용. */

	struct spdk_cpuset cpumask;
	/* [한국어] 대상 thread 에 적용할 새 cpumask. */

	int status;
	/* [한국어] 대상 thread 에서 cpumask 변경 결과. 0=성공, -errno=실패. */

	struct spdk_thread *orig_thread;
	/* [한국어] RPC 핸들러가 처음 실행되던 spdk_thread. 응답은 이 thread 에서 송신해야 함. */
};

/*
 * [한국어]
 * rpc_thread_set_cpumask_done - 원래 thread 로 돌아와 응답 송신 + ctx 해제.
 *
 * 호출 컨텍스트: orig_thread (=RPC 핸들러를 처음 실행한 thread).
 */
static void
rpc_thread_set_cpumask_done(void *_ctx)
{
	struct rpc_thread_set_cpumask_ctx *ctx = _ctx;

	if (ctx->status == 0) {
		spdk_jsonrpc_send_bool_response(ctx->request, true); /* [한국어] 성공 응답. */
	} else {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-ctx->status)); /* [한국어] errno → 문자열. */
	}

	free(ctx); /* [한국어] 컨텍스트 해제. */
}

/*
 * [한국어]
 * _rpc_thread_set_cpumask - 대상 spdk_thread 위에서 실행되는 step 콜백.
 *
 * 동작:
 *  1) spdk_thread_set_cpumask 로 자기 자신 (= 대상 thread) 의 cpumask 갱신.
 *  2) 결과를 ctx->status 에 저장.
 *  3) spdk_thread_send_msg 로 orig_thread 에 done 콜백 예약.
 *
 * 실행 컨텍스트: 대상 thread (req.id 로 지정된 thread).
 *  - cpumask 변경은 thread-local 상태이므로 반드시 그 thread 자신이 호출해야 함.
 */
static void
_rpc_thread_set_cpumask(void *_ctx)
{
	struct rpc_thread_set_cpumask_ctx *ctx = _ctx;

	ctx->status = spdk_thread_set_cpumask(&ctx->cpumask); /* [한국어] 자기 자신 thread 의 cpumask 갱신. */

	/* [한국어] 결과를 응답하기 위해 원래 thread 로 돌아간다. RPC 응답 송신은 RPC 가 등록된 thread 에서만 안전. */
	spdk_thread_send_msg(ctx->orig_thread, rpc_thread_set_cpumask_done, ctx);
}

/*
 * [한국어]
 * struct rpc_thread_set_cpumask - "thread_set_cpumask" RPC 의 입력.
 *
 * 필드:
 *  - id: 대상 spdk_thread 의 ID.
 *  - cpumask: 적용할 마스크 (16진수 문자열).
 */
struct rpc_thread_set_cpumask {
	uint64_t id;
	/* [한국어] 대상 spdk_thread ID. */

	char *cpumask;
	/* [한국어] hex 문자열 (예: "0x3" = lcore 0,1).
	 * 메모리 소유권: 핸들러 종료 전 free. */
};

static const struct spdk_json_object_decoder rpc_thread_set_cpumask_decoders[] = {
	{"id", offsetof(struct rpc_thread_set_cpumask, id), spdk_json_decode_uint64},
	/* [한국어] 필수: thread id. */
	{"cpumask", offsetof(struct rpc_thread_set_cpumask, cpumask), spdk_json_decode_string},
	/* [한국어] 필수: cpumask hex 문자열. */
};

/*
 * [한국어]
 * rpc_thread_set_cpumask - "thread_set_cpumask" RPC 핸들러.
 *
 * 동작:
 *  1) ctx 할당 (cross-thread 라이프사이클 동안 유지).
 *  2) params 디코딩 → req.id, req.cpumask.
 *  3) thread_get_by_id 로 대상 thread 핸들 조회.
 *  4) cpumask 파싱 + 비어있지 않은지 검증.
 *  5) interrupt mode 가 비활성인 SPDK 빌드라면, cpumask 의 모든 reactor 가 인터럽트 모드인지 검사
 *     (그러면 thread 가 못 돌아감 → 거부).
 *  6) ctx->orig_thread = 현재 thread, ctx->request = request 저장.
 *  7) spdk_thread_send_msg(target, _rpc_thread_set_cpumask, ctx) 로 cross-thread 호출.
 *  8) 즉시 반환 (응답은 done 콜백이 처리).
 *
 * 등록 phase: RUNTIME.
 *
 * 호출 체인:
 *   client → rpc_thread_set_cpumask (orig_thread)
 *     → spdk_thread_send_msg → _rpc_thread_set_cpumask (target_thread)
 *       → spdk_thread_set_cpumask (target_thread 자신의 mask 갱신)
 *       → spdk_thread_send_msg → rpc_thread_set_cpumask_done (orig_thread)
 *         → spdk_jsonrpc_send_*_response, free(ctx).
 */
static void
rpc_thread_set_cpumask(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_thread_set_cpumask req = {};         /* [한국어] 입력 디코딩 버퍼. */
	struct rpc_thread_set_cpumask_ctx *ctx;          /* [한국어] cross-thread 컨텍스트. */
	const struct spdk_cpuset *coremask;              /* [한국어] 앱 전체 core mask 핸들. */
	struct spdk_cpuset tmp_mask;                     /* [한국어] AND 임시. */
	struct spdk_thread *thread;                       /* [한국어] 대상 thread 핸들. */
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Memory allocation failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation failed");
		return;
	}

	if (spdk_json_decode_object(params, rpc_thread_set_cpumask_decoders,
				    SPDK_COUNTOF(rpc_thread_set_cpumask_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto err;
	}

	thread = spdk_thread_get_by_id(req.id); /* [한국어] id → thread 핸들. NULL 이면 미존재. */
	if (thread == NULL) {
		SPDK_ERRLOG("Thread %" PRIu64 " does not exist\n", req.id);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Thread %" PRIu64 " does not exist", req.id);
		goto err;
	}

	rc = spdk_app_parse_core_mask(req.cpumask, &ctx->cpumask); /* [한국어] hex → cpuset, 활성 코어와 교집합. */
	if (rc != 0) {
		SPDK_ERRLOG("Invalid cpumask %s\n", req.cpumask);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Invalid cpumask %s", req.cpumask);
		goto err;
	}

	if (spdk_cpuset_count(&ctx->cpumask) == 0) {
		/* [한국어] 적용 가능한 코어가 0 개 → 의미 없음. 활성 reactor mask 와 함께 표시. */
		coremask = spdk_app_get_core_mask();
		spdk_cpuset_copy(&tmp_mask, coremask);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "No CPU is selected from reactor mask %s",
						     spdk_cpuset_fmt(&tmp_mask));
		goto err;
	}

	/* There may be any reactors running in interrupt mode. But currently,
	 * when interrupt ability of the spdk_thread is not enabled,
	 * spdk_thread can't get executed on reactor which runs in interrupt.
	 * Exclude the situation that reactors specified by the cpumask are
	 * all in interrupt mode.
	 */
	/* [한국어] interrupt 모드 reactor 와 polling 모드 reactor 가 공존하는데, spdk_thread 의
	 *  인터럽트 능력이 꺼진 빌드에서는 폴링 reactor 위에서만 실행 가능. cpumask 가 모두 인터럽트
	 *  모드 reactor 로 가리킨다면 thread 는 절대 실행되지 못하므로 거부해야 한다. */
	if (!spdk_interrupt_mode_is_enabled()) {
		struct spdk_reactor *local_reactor = spdk_reactor_get(spdk_env_get_current_core());
		struct spdk_cpuset tmp_cpuset;

		/* Masking off reactors which are in interrupt mode */
		/* [한국어] notify_cpuset 은 인터럽트 (notify) 가능한 reactor 마스크.
		 *  ~notify_cpuset & user_cpumask 결과가 비어있다면 모든 코어가 인터럽트 모드 → 거부. */
		spdk_cpuset_copy(&tmp_cpuset, &local_reactor->notify_cpuset);
		spdk_cpuset_negate(&tmp_cpuset);                      /* [한국어] notify 가능 → 불가능. */
		spdk_cpuset_and(&tmp_cpuset, &ctx->cpumask);          /* [한국어] 사용자 cpumask 와 AND. */
		if (spdk_cpuset_count(&tmp_cpuset) == 0) {
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							     "cpumask %s are all in interrupt mode, and can't be scheduled yet",
							     req.cpumask);
			goto err;
		}
	}

	ctx->request = request;             /* [한국어] 응답 송신용. */
	ctx->orig_thread = spdk_get_thread(); /* [한국어] 응답 송신은 이 thread 에서. */

	/* [한국어] 대상 thread 에 step 콜백 예약. _rpc_thread_set_cpumask 가 그 thread 위에서
	 *  cpumask 갱신 후 다시 orig_thread 에 done 을 예약하여 응답 송신. */
	spdk_thread_send_msg(thread, _rpc_thread_set_cpumask, ctx);

	free(req.cpumask); /* [한국어] decode_string 할당분. ctx 에는 cpumask 사본이 있으므로 안전. */
	return;

err:
	/* [한국어] 에러 경로: ctx 와 req.cpumask 모두 해제. */
	free(req.cpumask);
	free(ctx);
}
SPDK_RPC_REGISTER("thread_set_cpumask", rpc_thread_set_cpumask, SPDK_RPC_RUNTIME)
/* [한국어] app_rpc 컴포넌트 디버그 플래그 등록 - 본 파일의 SPDK_DEBUGLOG(app_rpc, ...) 출력을
 *  RPC log_set_flag --flag app_rpc 로 켤 수 있게 한다. */
SPDK_LOG_REGISTER_COMPONENT(app_rpc)
