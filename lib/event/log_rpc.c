/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK 로그 관련 JSON-RPC 핸들러 모음 (log_rpc.c)
 *
 * === 파일의 역할 ===
 * 외부 사용자가 (보통 scripts/rpc.py 또는 직접 JSON-RPC 클라이언트로) SPDK 데몬의
 * 로그 동작을 런타임에 제어할 수 있도록 해주는 RPC 엔드포인트들을 제공한다.
 * 구체적으로 다음과 같은 RPC 들이 정의된다:
 *   - log_set_print_level / log_get_print_level   : 표준 출력에 찍히는 레벨 임계
 *   - log_set_level       / log_get_level         : syslog/내부 로그 임계
 *   - log_set_flag        / log_clear_flag        : 컴포넌트별 디버그 플래그 on/off
 *   - log_get_flags                               : 등록된 모든 플래그 상태 덤프
 *   - log_enable_timestamps                       : 로그에 timestamp prefix 추가
 * 이 핸들러들은 모두 내부 spdk_log_set_*/spdk_log_get_* API 의 얇은 래퍼이며,
 * JSON 인자를 파싱해 검증한 뒤 그대로 전달하는 구조이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 응용 프로그램은 시작 시 lib/jsonrpc/ 의 RPC 서버를 (보통 Unix 소켓에) 바인드한다.
 * 호출 체인:
 *   client (rpc.py) -- JSON-RPC over Unix socket --> SPDK app
 *     → lib/jsonrpc 가 method 이름과 params 파싱
 *       → SPDK_RPC_REGISTER 가 등록한 핸들러 호출 (본 파일의 함수들)
 *         → 본 파일이 spdk_log_set_print_level() 등 lib/log/ API 호출
 *           → spdk_jsonrpc_send_bool_response / send_error_response 로 응답
 * SPDK_RPC_REGISTER 매크로는 constructor 단계에서 RPC 등록 테이블에 함수를 추가하므로
 * main() 가 명시적으로 호출하지 않아도 자동 노출된다. 또한 phase 비트마스크
 * (SPDK_RPC_STARTUP / SPDK_RPC_RUNTIME) 로 어느 시점에 호출 가능한지를 게이팅한다.
 *   - STARTUP: 서브시스템이 아직 초기화되기 전(rpc.py로 config 단계).
 *   - RUNTIME: 정상 가동 중.
 * log RPC 들은 대부분 STARTUP|RUNTIME 양쪽에서 사용 가능하다 (로그는 언제나 켜는 게 안전).
 * 단 log_enable_timestamps 만 RUNTIME 전용 (시작 시점엔 의미 없음).
 *
 * === 타 모듈과의 연결 ===
 *  - lib/log/ : spdk_log_set_print_level/level, spdk_log_set_flag/clear_flag,
 *               spdk_log_get_first_flag/next_flag, spdk_log_enable_timestamps 등
 *               실제 로그 상태 변경 API. 본 파일은 JSON 인자 검증 후 이를 호출.
 *  - lib/jsonrpc/, include/spdk/rpc.h : SPDK_RPC_REGISTER, spdk_jsonrpc_request*,
 *               spdk_jsonrpc_send_*, spdk_json_decode_object 등 RPC 인프라.
 *  - include/spdk/log.h : SPDK_LOG_ERROR/WARN/NOTICE/INFO/DEBUG enum 값과 spdk_log_flag.
 *  - include/spdk/util.h : SPDK_COUNTOF, offsetof 매크로.
 *
 *  데이터 흐름 (예: log_set_print_level "DEBUG"):
 *   client → JSON: {"method":"log_set_print_level","params":{"level":"DEBUG"}}
 *     → rpc_log_set_print_level()
 *       → spdk_json_decode_object(params, decoders, …, &req)  // params → struct rpc_log_level
 *       → _parse_log_level(req.level)                          // "DEBUG" → SPDK_LOG_DEBUG (int)
 *       → spdk_log_set_print_level(level)
 *       → spdk_jsonrpc_send_bool_response(request, true)
 *
 * === 주요 함수/구조체 요약 ===
 *  - struct rpc_log_flag / rpc_log_level / rpc_log_enable_timestamps:
 *      각각 RPC 핸들러가 디코딩한 인자를 임시 보관하는 플레이트.
 *  - free_rpc_log_flag / free_rpc_log_level: 디코더가 동적 할당한 문자열 해제용.
 *  - _parse_log_level / _log_get_level_name:
 *      "DEBUG"↔SPDK_LOG_DEBUG 같은 문자열↔enum 변환 헬퍼.
 *  - rpc_log_set_print_level / rpc_log_get_print_level: stdout 임계 제어.
 *  - rpc_log_set_level / rpc_log_get_level: syslog 임계 제어.
 *  - rpc_log_set_flag / rpc_log_clear_flag / rpc_log_get_flags: 디버그 플래그 제어.
 *  - rpc_log_enable_timestamps: 로그 라인 앞에 timestamp 출력 on/off.
 *  - SPDK_RPC_REGISTER(...) 호출들: 위 함수들을 RPC 메소드 이름으로 등록.
 *  - SPDK_LOG_REGISTER_COMPONENT(log_rpc): 본 파일 자신의 디버그 컴포넌트 등록
 *    (SPDK_DEBUGLOG(log_rpc, …) 출력을 켜고 끄기 위함).
 */

#include "spdk/rpc.h"   /* [한국어] SPDK_RPC_REGISTER, spdk_jsonrpc_request, send_bool/error_response 등 RPC 핵심 API. */
#include "spdk/util.h"  /* [한국어] SPDK_COUNTOF (배열 길이 매크로), offsetof, container_of 등 유틸. */

#include "spdk/log.h"   /* [한국어] SPDK_LOG_ERROR/WARN/NOTICE/INFO/DEBUG, spdk_log_set_*, spdk_log_get_* 정의.
                         *  본 파일이 노출하는 모든 RPC 의 백엔드 함수가 여기 들어 있다. */

/*
 * [한국어]
 * struct rpc_log_flag - "log_set_flag", "log_clear_flag" RPC 의 디코딩 결과 저장 구조체.
 *
 * 필드 설명:
 *  - flag: 사용자가 켜거나 끄려는 로그 컴포넌트의 이름 문자열 (예: "nvme", "bdev").
 *          spdk_json_decode_string 으로 디코딩되며, 본 함수가 free 해야 함.
 *
 * 설정자: spdk_json_decode_object(... rpc_log_set_flag_decoders ...) 가 채워줌.
 * 읽는 자: spdk_log_set_flag(req.flag) 또는 spdk_log_clear_flag(req.flag) 가 사용.
 * 동기화: 함수 로컬 변수로만 사용되므로 동기화 불필요.
 */
struct rpc_log_flag {
	char *flag;
	/* [한국어] 디버그 플래그 이름 (예: "nvme", "bdev_nvme").
	 * 설정자: rpc_log_set_flag/clear_flag 의 spdk_json_decode_object 호출.
	 * 읽는 자: spdk_log_set_flag/spdk_log_clear_flag.
	 * 값 범위: NULL 이면 디코딩 실패 (필수 키이므로 정상 경로에서는 항상 non-NULL).
	 * 메모리 소유권: 디코더가 strdup 한 메모리이며, free_rpc_log_flag() 가 해제해야 함. */
};

/*
 * [한국어]
 * struct rpc_log_level - "log_set_print_level", "log_set_level" RPC 의 디코딩 결과.
 *
 * 필드 설명:
 *  - level: 사용자가 지정한 레벨의 문자열 표현 ("ERROR" / "WARNING" / "NOTICE" / "INFO" / "DEBUG").
 *           본 파일의 _parse_log_level() 이 enum 값으로 변환.
 *
 * 설정자: spdk_json_decode_object 가 채움.
 * 읽는 자: _parse_log_level(req.level).
 * 메모리 소유권: free_rpc_log_level() 이 해제.
 */
struct rpc_log_level {
	char *level;
	/* [한국어] 로그 레벨 문자열. 5종 중 하나. 대소문자 무시 비교(strcasecmp) 됨.
	 * 설정자: spdk_json_decode_object.
	 * 읽는 자: _parse_log_level.
	 * 메모리 소유권: free_rpc_log_level. */
};

/*
 * [한국어]
 * free_rpc_log_flag - struct rpc_log_flag 안의 동적 할당 문자열을 해제.
 *
 * @p: 해제 대상 구조체 포인터. p 자체는 보통 스택 변수이므로 free 하지 않고, 내부 필드만.
 *
 * 호출 체인: rpc_log_set_flag(), rpc_log_clear_flag() 의 정상/에러 종료 경로.
 * 실행 컨텍스트: RPC 핸들러 thread (RPC 가 등록된 spdk_thread).
 */
static void
free_rpc_log_flag(struct rpc_log_flag *p)
{
	free(p->flag); /* [한국어] spdk_json_decode_string 이 strdup 한 문자열 해제. p->flag==NULL 이어도 free(NULL) 은 안전. */
}

/*
 * [한국어]
 * free_rpc_log_level - struct rpc_log_level 안의 동적 할당 문자열을 해제.
 *
 * 사용 패턴: free_rpc_log_flag 와 동일 (level 만 해제).
 */
static void
free_rpc_log_level(struct rpc_log_level *p)
{
	free(p->level); /* [한국어] level 문자열 해제 (NULL safe). */
}

/*
 * [한국어]
 * rpc_log_set_flag_decoders - "log_set_flag" / "log_clear_flag" RPC 의 디코딩 표.
 *
 * 의미: JSON 인자에서 "flag" 키를 문자열로 읽어 struct rpc_log_flag::flag 에 저장.
 *       offsetof 로 구조체 필드 위치를 알려준다.
 * 동기화: const 전역 - 모든 호출이 read-only 로 공유.
 */
static const struct spdk_json_object_decoder rpc_log_set_flag_decoders[] = {
	{"flag", offsetof(struct rpc_log_flag, flag), spdk_json_decode_string},
	/* [한국어] "flag" 키, struct rpc_log_flag::flag 위치, 문자열 디코더. 필수 키 (4번째 인자 생략 = false). */
};

/*
 * [한국어]
 * rpc_log_set_print_level_decoders - "log_set_print_level" / "log_set_level" 의 디코딩 표.
 *
 * "log_set_level" 도 같은 표를 재사용한다 ("level" 키 하나만 받으므로).
 * 동기화: const 전역.
 */
static const struct spdk_json_object_decoder rpc_log_set_print_level_decoders[] = {
	{"level", offsetof(struct rpc_log_level, level), spdk_json_decode_string},
	/* [한국어] "level" 키, struct rpc_log_level::level 위치, 문자열 디코더. */
};

/*
 * [한국어]
 * _parse_log_level - "ERROR"/"WARNING"/"NOTICE"/"INFO"/"DEBUG" 문자열을 enum 으로.
 *
 * @level: 사용자가 입력한 레벨 문자열 (대소문자 무시 비교).
 * @return: 매칭되면 SPDK_LOG_* enum 값 (>= 0), 매칭 실패 시 -1.
 *
 * 호출자: rpc_log_set_print_level, rpc_log_set_level.
 * 호출자는 -1 반환 시 invalid params 응답을 클라이언트에 보낸다.
 * 동기화: 순수 함수 (전역 상태 없음). 어떤 thread 에서 호출해도 안전.
 */
static int
_parse_log_level(char *level)
{
	/* [한국어] strcasecmp 로 대소문자 무시 비교 → 사용자가 "error" 든 "ERROR" 든 OK. */
	if (!strcasecmp(level, "ERROR")) {
		return SPDK_LOG_ERROR; /* [한국어] 0 - 가장 심각한 에러. */
	} else if (!strcasecmp(level, "WARNING")) {
		return SPDK_LOG_WARN;  /* [한국어] 1 - 경고. */
	} else if (!strcasecmp(level, "NOTICE")) {
		return SPDK_LOG_NOTICE; /* [한국어] 2 - 일반 알림 (기본값인 경우 많음). */
	} else if (!strcasecmp(level, "INFO")) {
		return SPDK_LOG_INFO;   /* [한국어] 3 - 정보성 메시지. */
	} else if (!strcasecmp(level, "DEBUG")) {
		return SPDK_LOG_DEBUG;  /* [한국어] 4 - 가장 상세한 디버그. */
	}
	return -1; /* [한국어] 알 수 없는 레벨 - 호출자가 invalid params 로 처리. */
}

/*
 * [한국어]
 * _log_get_level_name - SPDK_LOG_* enum 값을 문자열로 변환 (역방향).
 *
 * @level: spdk_log_get_print_level/spdk_log_get_level 의 반환값.
 * @return: 매칭되는 문자열 ("ERROR" 등) 또는 NULL (알 수 없는 값).
 *
 * 호출자: rpc_log_get_print_level, rpc_log_get_level.
 * 동기화: 순수 함수 (정적 const 문자열 반환), 어디서든 호출 안전.
 */
static const char *
_log_get_level_name(int level)
{
	/* [한국어] _parse_log_level 의 역연산. JSON 응답에 사람이 읽을 수 있는 문자열을 넣기 위함. */
	if (level == SPDK_LOG_ERROR) {
		return "ERROR";
	} else if (level == SPDK_LOG_WARN) {
		return "WARNING";
	} else if (level == SPDK_LOG_NOTICE) {
		return "NOTICE";
	} else if (level == SPDK_LOG_INFO) {
		return "INFO";
	} else if (level == SPDK_LOG_DEBUG) {
		return "DEBUG";
	}
	return NULL; /* [한국어] 알 수 없는 enum - 호출자가 INTERNAL_ERROR 로 보고. */
}

/*
 * [한국어]
 * rpc_log_set_print_level - "log_set_print_level" RPC 핸들러.
 *
 * @request: SPDK JSON-RPC 가 응답을 보낼 컨텍스트. 핸들러 종료 전에 반드시 send_*_response.
 * @params: JSON 객체 ("level" 키).
 *
 * 동작:
 *   1) params 를 struct rpc_log_level 으로 디코딩.
 *   2) level 문자열 → enum 변환. -1 이면 invalid params.
 *   3) spdk_log_set_print_level() 호출 - stdout 출력 임계 갱신.
 *   4) bool true 응답.
 *   5) goto end 로 동적 할당된 req.level 해제.
 *
 * "print level" vs "level" 차이:
 *   - print_level: SPDK_*PRINTF*류 매크로가 stdout/stderr 에 찍을지 결정.
 *   - level: openlog/syslog 로 보낼지 결정.
 *   - 두 임계는 독립적으로 설정 가능. 보통 stdout 만 NOTICE 로 두고
 *     syslog 는 INFO 로 받거나 그 반대.
 *
 * 호출 체인:
 *   client → rpc 서버 → rpc_log_set_print_level → spdk_log_set_print_level
 *
 * 실행 컨텍스트: RPC 가 처리되는 spdk_thread (보통 master reactor).
 *               cross-thread 데이터 접근이 없으므로 단일 thread 처리로 충분.
 *
 * 등록: SPDK_RPC_REGISTER("log_set_print_level", ..., STARTUP|RUNTIME).
 */
static void
rpc_log_set_print_level(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_log_level req = {}; /* [한국어] 디코딩 결과 저장 구조체. 0/NULL 으로 초기화 (free 안전). */
	int level;                     /* [한국어] 변환된 SPDK_LOG_* enum 값 또는 -1. */

	/* [한국어] JSON params 를 struct rpc_log_level 로 변환.
	 *  실패 시 req.level 은 NULL 이거나 부분 채워짐 - 어떤 경우든 free 안전. */
	if (spdk_json_decode_object(params, rpc_log_set_print_level_decoders,
				    SPDK_COUNTOF(rpc_log_set_print_level_decoders), &req)) {
		SPDK_DEBUGLOG(log_rpc, "spdk_json_decode_object failed\n"); /* [한국어] log_rpc 컴포넌트 디버그 출력. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed"); /* [한국어] -32603 INTERNAL_ERROR 응답. */
		goto end; /* [한국어] req.level 해제 후 종료. */
	}

	/* [한국어] "DEBUG" 등 문자열 → SPDK_LOG_* 정수. -1 이면 무효 입력. */
	level = _parse_log_level(req.level);
	if (level == -1) {
		SPDK_DEBUGLOG(log_rpc, "tried to set invalid log level\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "invalid log level"); /* [한국어] -32602 INVALID_PARAMS. */
		goto end;
	}

	/* [한국어] 실제로 lib/log 의 stdout 출력 임계 변경. 이 호출 이후 모든 SPDK_*PRINTF 가 영향 받음. */
	spdk_log_set_print_level(level);
	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] {"result":true} 응답. */
end:
	free_rpc_log_level(&req); /* [한국어] req.level 동적 메모리 해제. 모든 경로에서 도달. */
}
/* [한국어] STARTUP|RUNTIME 양쪽 phase 에서 호출 가능. 즉 config 단계에서도 런타임 중에도 레벨을 바꿀 수 있다. */
SPDK_RPC_REGISTER("log_set_print_level", rpc_log_set_print_level,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_log_get_print_level - "log_get_print_level" RPC 핸들러.
 *
 * 인자 없음 (params 가 NULL 이어야 함).
 * 응답: 현재 print level 의 문자열 ("DEBUG" 등) 을 result 로 보냄.
 *
 * 동작:
 *   1) params != NULL 이면 invalid params (이 RPC 는 인자 없음).
 *   2) spdk_log_get_print_level() 로 enum 획득.
 *   3) _log_get_level_name() 으로 문자열 변환. NULL 이면 INTERNAL_ERROR.
 *   4) JSON writer 로 문자열 결과 작성 후 응답.
 *
 * 등록 phase: STARTUP|RUNTIME (조회는 언제든 안전).
 */
static void
rpc_log_get_print_level(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w; /* [한국어] JSON 출력 빌더. spdk_jsonrpc_begin_result 가 반환. */
	int level;                     /* [한국어] 현재 stdout 임계 enum. */
	const char *name;              /* [한국어] level 의 사람용 문자열 표현. */

	/* [한국어] params 가 있으면 인자 받지 않는 RPC 명세 위반 - 거부. */
	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "log_get_print_level requires no parameters");
		return;
	}

	level = spdk_log_get_print_level(); /* [한국어] lib/log 에 저장된 현재 stdout 임계 조회. */
	name = _log_get_level_name(level);
	if (name == NULL) {
		/* [한국어] 이론상 도달하기 어려우나 (SPDK 가 자체 enum 만 저장), 방어적으로 처리. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "invalid log level");
		return;
	}

	w = spdk_jsonrpc_begin_result(request); /* [한국어] result 빌더 시작. 내부적으로 응답 헤더 prepare. */
	spdk_json_write_string(w, name);        /* [한국어] result 값으로 문자열 그대로 기록 (top-level string). */

	spdk_jsonrpc_end_result(request, w);    /* [한국어] 빌더 종료 → 실제 클라이언트로 응답 전송. */
}
SPDK_RPC_REGISTER("log_get_print_level", rpc_log_get_print_level,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_log_set_level - "log_set_level" RPC 핸들러 (syslog 임계 설정).
 *
 * rpc_log_set_print_level 과 거의 동일하나 호출하는 백엔드가 spdk_log_set_level().
 *
 * "level" (syslog) 의 의미:
 *   - SPDK 가 openlog/syslog 로 보낼 메시지 임계.
 *   - 보통 daemon 으로 운영할 때 stdout 보다 더 상세한 로그를 syslog 로 받기 위해 사용.
 *
 * 등록 phase: STARTUP|RUNTIME.
 */
static void
rpc_log_set_level(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct rpc_log_level req = {}; /* [한국어] 입력 디코딩 버퍼. */
	int level;                     /* [한국어] enum 값. */

	/* [한국어] log_set_print_level 과 동일한 디코더 표 재사용 (둘 다 "level" 키만). */
	if (spdk_json_decode_object(params, rpc_log_set_print_level_decoders,
				    SPDK_COUNTOF(rpc_log_set_print_level_decoders), &req)) {
		SPDK_DEBUGLOG(log_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto end;
	}

	level = _parse_log_level(req.level); /* [한국어] 문자열 → enum. */
	if (level == -1) {
		SPDK_DEBUGLOG(log_rpc, "tried to set invalid log level\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "invalid log level");
		goto end;
	}


	spdk_log_set_level(level);                       /* [한국어] syslog 임계 변경 - stdout 임계 (print_level) 와 독립. */
	spdk_jsonrpc_send_bool_response(request, true);  /* [한국어] 성공 응답. */
end:
	free_rpc_log_level(&req); /* [한국어] req.level 해제. */
}
SPDK_RPC_REGISTER("log_set_level", rpc_log_set_level, SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_log_get_level - "log_get_level" RPC 핸들러 (syslog 임계 조회).
 *
 * rpc_log_get_print_level 과 동일한 패턴, 백엔드만 spdk_log_get_level().
 */
static void
rpc_log_get_level(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w; /* [한국어] 응답 JSON 빌더. */
	int level;                     /* [한국어] 현재 syslog 임계. */
	const char *name;              /* [한국어] 문자열 표현. */

	if (params != NULL) {
		/* [한국어] 인자 없는 RPC - params 가 있으면 거절. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "log_get_level requires no parameters");
		return;
	}

	level = spdk_log_get_level();    /* [한국어] lib/log 의 syslog 임계 조회. */
	name = _log_get_level_name(level);
	if (name == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "invalid log level");
		return;
	}

	w = spdk_jsonrpc_begin_result(request); /* [한국어] result 빌더 시작. */
	spdk_json_write_string(w, name);        /* [한국어] 문자열 결과 기록. */

	spdk_jsonrpc_end_result(request, w);    /* [한국어] 응답 송신. */
}
SPDK_RPC_REGISTER("log_get_level", rpc_log_get_level, SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_log_set_flag - "log_set_flag" RPC 핸들러.
 *
 * 컴포넌트별 디버그 플래그를 켠다 (예: "nvme" 플래그를 켜면 nvme 모듈의
 * SPDK_DEBUGLOG(nvme, ...) 출력이 활성화됨). 플래그는 SPDK_LOG_REGISTER_COMPONENT
 * 로 각 모듈에서 등록하며, 본 파일 맨 아래의 SPDK_LOG_REGISTER_COMPONENT(log_rpc) 가 그 예.
 *
 * 동작:
 *   1) "flag" 키 디코딩.
 *   2) spdk_log_set_flag(name) 호출. 미등록 플래그면 0 이 아닌 값 반환 → invalid.
 *   3) bool 응답.
 *
 * 등록 phase: STARTUP|RUNTIME.
 */
static void
rpc_log_set_flag(struct spdk_jsonrpc_request *request,
		 const struct spdk_json_val *params)
{
	struct rpc_log_flag req = {}; /* [한국어] flag 이름 보관. */

	if (spdk_json_decode_object(params, rpc_log_set_flag_decoders,
				    SPDK_COUNTOF(rpc_log_set_flag_decoders), &req)) {
		SPDK_DEBUGLOG(log_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto end;
	}

	/* [한국어] 등록되지 않은 플래그 이름이면 spdk_log_set_flag 가 0 외 반환. */
	if (spdk_log_set_flag(req.flag) != 0) {
		SPDK_DEBUGLOG(log_rpc, "tried to set invalid log flag\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "invalid log flag");
		goto end;
	}

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 성공 응답. */
end:
	free_rpc_log_flag(&req); /* [한국어] req.flag 해제. */
}
SPDK_RPC_REGISTER("log_set_flag", rpc_log_set_flag, SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_log_clear_flag - "log_clear_flag" RPC 핸들러.
 *
 * rpc_log_set_flag 와 동일하나 백엔드는 spdk_log_clear_flag (플래그 끄기).
 */
static void
rpc_log_clear_flag(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_log_flag req = {}; /* [한국어] flag 이름 보관. */

	if (spdk_json_decode_object(params, rpc_log_set_flag_decoders,
				    SPDK_COUNTOF(rpc_log_set_flag_decoders), &req)) {
		SPDK_DEBUGLOG(log_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto end;
	}

	/* [한국어] 미등록 flag 이름이면 0 이 아닌 값 반환 → invalid params 로 응답. */
	if (spdk_log_clear_flag(req.flag) != 0) {
		SPDK_DEBUGLOG(log_rpc, "tried to clear invalid log flag\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "invalid log flag");
		goto end;
	}

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 플래그 비활성화 성공. */
end:
	free_rpc_log_flag(&req); /* [한국어] flag 메모리 해제. */
}
SPDK_RPC_REGISTER("log_clear_flag", rpc_log_clear_flag,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_log_get_flags - "log_get_flags" RPC 핸들러. 등록된 모든 플래그 + 활성 여부 덤프.
 *
 * 응답 JSON 형태:
 *   {"nvme": true, "bdev": false, "log_rpc": false, ...}
 * 각 키가 컴포넌트 이름, 값이 현재 활성화 여부.
 *
 * 동작:
 *   1) params != NULL 이면 거절.
 *   2) lib/log 의 플래그 등록 리스트를 spdk_log_get_first_flag/next_flag 로 순회하며
 *      각 (name, enabled) 쌍을 JSON 객체에 기록.
 *
 * 등록 phase: STARTUP|RUNTIME.
 */
static void
rpc_log_get_flags(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;  /* [한국어] 응답 빌더. */
	struct spdk_log_flag *flag;     /* [한국어] 플래그 리스트 순회 커서. */

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "log_get_flags requires no parameters"); /* [한국어] 인자 없는 RPC. */
		return;
	}

	w = spdk_jsonrpc_begin_result(request);    /* [한국어] result 빌더 시작. */
	spdk_json_write_object_begin(w);           /* [한국어] result 를 JSON object 로 시작 ("{"). */
	flag = spdk_log_get_first_flag();          /* [한국어] 첫 번째 등록 플래그. NULL 이면 등록 없음. */
	while (flag) {
		spdk_json_write_name(w, flag->name);   /* [한국어] 키 = 플래그 이름. */
		spdk_json_write_bool(w, flag->enabled); /* [한국어] 값 = 현재 활성 여부. */
		flag = spdk_log_get_next_flag(flag);    /* [한국어] 다음 플래그로 이동. NULL = 끝. */
	}
	spdk_json_write_object_end(w);             /* [한국어] 객체 닫기 ("}"). */
	spdk_jsonrpc_end_result(request, w);       /* [한국어] 응답 전송. */
}
SPDK_RPC_REGISTER("log_get_flags", rpc_log_get_flags, SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_log_enable_timestamps - "log_enable_timestamps" RPC 의 디코딩 버퍼.
 *
 * 필드:
 *  - enabled: true 면 로그 라인 앞에 timestamp 가 출력됨, false 면 끔.
 */
struct rpc_log_enable_timestamps {
	bool enabled;
	/* [한국어] timestamp prefix 출력 여부.
	 * 설정자: spdk_json_decode_object.
	 * 읽는 자: spdk_log_enable_timestamps.
	 * 값 범위: true / false. */
};

/*
 * [한국어]
 * rpc_log_enable_timestamps_decoders - "enabled" bool 키만 받는 디코더 표.
 */
static const struct spdk_json_object_decoder rpc_log_enable_timestamps_decoders[] = {
	{"enabled", offsetof(struct rpc_log_enable_timestamps, enabled), spdk_json_decode_bool},
	/* [한국어] "enabled" 키, struct 의 enabled 필드, bool 디코더. 필수 키. */
};

/*
 * [한국어]
 * rpc_log_enable_timestamps - "log_enable_timestamps" RPC 핸들러.
 *
 * 동작:
 *   1) params 에서 enabled 추출 (실패 시 invalid params).
 *   2) spdk_log_enable_timestamps(enabled) 호출 - 이후 로그 출력 형식 변경.
 *   3) bool true 응답.
 *
 * 등록 phase: RUNTIME 만 (STARTUP 단계에서는 의미 없음 - 본격 로그가 아직 안 나옴).
 */
static void
rpc_log_enable_timestamps(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_log_enable_timestamps req = {}; /* [한국어] enabled 보관 (false 초기화). */

	if (spdk_json_decode_object(params, rpc_log_enable_timestamps_decoders,
				    SPDK_COUNTOF(rpc_log_enable_timestamps_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n"); /* [한국어] 다른 RPC 와 달리 ERRLOG 사용 - 호출 빈도가 낮으므로. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		return; /* [한국어] req 에 동적 메모리 없으므로 free 불필요. */
	}

	spdk_log_enable_timestamps(req.enabled); /* [한국어] lib/log 의 timestamp 모드 토글. */

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 성공 응답. */
}
SPDK_RPC_REGISTER("log_enable_timestamps", rpc_log_enable_timestamps, SPDK_RPC_RUNTIME)
/* [한국어] log_rpc 라는 이름으로 디버그 컴포넌트를 등록.
 *  - 본 파일의 SPDK_DEBUGLOG(log_rpc, ...) 호출들이 이 플래그 상태를 조회한다.
 *  - 사용자는 RPC log_set_flag --flag log_rpc 로 본 파일의 디버그 출력을 켤 수 있다.
 *  - SPDK_LOG_REGISTER_COMPONENT 매크로는 constructor 시점에 lib/log 의 등록 리스트에 추가. */
SPDK_LOG_REGISTER_COMPONENT(log_rpc)
