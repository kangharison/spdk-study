/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/**
 * \file
 * Logging interfaces
 */

/*
 * [한국어 설명] SPDK 통합 로그 시스템 공개 API (log.h) — 약 443 라인
 *
 * === 파일의 역할 ===
 * SPDK 전 컴포넌트가 공유하는 로그 시스템의 공개 인터페이스를 정의한다.
 * 단일 진입점 spdk_log()/spdk_vlog()를 중심으로, 호출자에게는 친숙한
 * SPDK_ERRLOG/WARNLOG/NOTICELOG/INFOLOG/DEBUGLOG 매크로를 제공한다.
 * 이 매크로들은 자동으로 __FILE__/__LINE__/__func__를 포착해 메시지에
 * 부착하므로 호출자가 직접 수기로 위치 정보를 적을 필요가 없다.
 * 또한 컴포넌트(서브시스템)별 디버그 플래그(struct spdk_log_flag)와
 * 글로벌 임계값(spdk_log_level)을 분리하여, 운영 중에 syslog와 stderr로의
 * 출력을 동적으로 켜고 끌 수 있게 한다. deprecation(폐기 예정 기능 사용
 * 알림) 추적도 본 헤더에서 함께 제공된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 거의 모든 .c 파일이 본 헤더를 #include 하여 SPDK_ERRLOG/DEBUGLOG
 * 등을 호출한다. 즉 가장 폭넓게 의존되는 횡단(cross-cutting) API 중 하나다.
 * 호출 흐름:
 *   호출 측: lib/nvme, lib/bdev, lib/thread, module/bdev/* 등 모든 .c
 *           → SPDK_ERRLOG("...") (매크로 확장) → spdk_log(level, ...)
 *   spdk_log 구현부: lib/log/log.c — 사용자 콜백(spdk_log_cb)이 등록되어
 *           있으면 콜백으로 위임, 없으면 stderr/syslog 기본 처리.
 * 또한 app/spdk_tgt 등 SPDK 앱은 시작 시 spdk_log_open()/_ext()로 로그
 * 시스템을 초기화한다. lib/log/log_rpc.c는 RPC를 통해 런타임 로그 레벨/
 * 플래그 변경 핸들러를 노출한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/assert.h(SPDK_STATIC_ASSERT), spdk/stdinc.h(stdint/stdarg/
 *             stdbool 등 표준 타입), spdk/queue.h(TAILQ_ENTRY — log_flag/
 *             deprecation 리스트 연결).
 * 의존(구현): lib/log/log.c가 본 API들의 실체를 제공. 기본 구현은 syslog(3)와
 *             stderr로의 출력을 모두 수행하며, openlog/syslog/closelog
 *             시스템 호출과 매핑된다 (spdk_log_to_syslog_level 참고).
 * 데이터 흐름: 매크로 호출 → spdk_log(printf 가변인자) → 사용자 콜백 또는
 *             기본 핸들러 → syslog/stderr/사용자 정의 sink.
 * 공유 자료구조: 전역 log_level, log_print_level, TAILQ로 연결된 spdk_log_flag
 *             리스트, deprecation 리스트. 모두 lib/log/log.c가 소유한다.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_log_open / spdk_log_open_ext / spdk_log_close: 로그 시스템 lifecycle.
 *   - spdk_log_set_level / spdk_log_get_level: syslog로 보낼 메시지 임계값.
 *   - spdk_log_set_print_level / spdk_log_get_print_level: stderr 출력 임계값.
 *   - spdk_log / spdk_vlog / spdk_flog / spdk_vflog: printf 스타일 로깅 핵심.
 *   - spdk_log_dump: 바이너리 버퍼 hex dump 출력.
 *   - SPDK_ERRLOG/WARNLOG/NOTICELOG/INFOLOG/DEBUGLOG/PRINTF: 사용자용 매크로.
 *   - SPDK_ERRLOG_RATELIMIT: 1초당 1회 + 압축 카운터로 메시지 폭주 억제.
 *   - SPDK_LOG_REGISTER_COMPONENT(flag): __attribute__((constructor))로 컴포넌트별
 *     디버그 플래그를 자동 등록 (런타임에 RPC/CLI로 켜고 끌 수 있게 됨).
 *   - struct spdk_log_flag: 컴포넌트 이름 + enabled 플래그 + TAILQ 연결.
 *   - spdk_log_set_flag / spdk_log_clear_flag / spdk_log_get_flag: 플래그 제어.
 *   - SPDK_LOG_DEPRECATION_REGISTER + spdk_log_deprecated: 폐기 예정 기능 사용 추적.
 */

#ifndef SPDK_LOG_H        /* [한국어] include 가드 — 다중 포함 방지 */
#define SPDK_LOG_H

#include "spdk/assert.h"  /* [한국어] SPDK_STATIC_ASSERT — struct spdk_log_opts ABI 크기 검증용 */
#include "spdk/stdinc.h"  /* [한국어] 표준 헤더 묶음 (stdint/stdbool/stdarg/stdio 포함) */
#include "spdk/queue.h"   /* [한국어] BSD 스타일 TAILQ_ENTRY — spdk_log_flag 리스트 연결 매크로 */

#ifdef __cplusplus
extern "C" {              /* [한국어] C++에서도 본 헤더를 사용할 수 있도록 C 링키지 강제 */
#endif

/* [한국어] deprecation 메시지 출력 빈도 상수.
 *   _EVERY_24H = 86400초(=24시간)에 한 번만 로그
 *   _ALWAYS    = 0초 → 매번 로그 (제한 없음)
 * spdk_log_deprecation_register()의 rate_limit_seconds에 전달된다. */
#define SPDK_LOG_DEPRECATION_EVERY_24H	86400
#define SPDK_LOG_DEPRECATION_ALWAYS	0

/**
 * for passing user-provided log call
 *
 * \param level Log level threshold.
 * \param file Name of the current source file.
 * \param line Current source file line.
 * \param func Current source function name.
 * \param format Format string to the message.
 * \param args Additional arguments for format string.
 */
/*
 * [한국어]
 * spdk_log_cb - SPDK 로그 시스템이 메시지 1건마다 호출하는 사용자 콜백 시그니처.
 *
 * @level: 메시지의 심각도 (SPDK_LOG_ERROR/WARN/NOTICE/INFO/DEBUG 중 하나).
 * @file:  메시지를 생성한 소스 파일 경로 (__FILE__ 매크로의 값).
 * @line:  메시지를 생성한 소스 라인 번호 (__LINE__ 매크로의 값).
 * @func:  메시지를 생성한 함수 이름 (__func__).
 * @format: printf 스타일 포맷 문자열.
 * @args:  포맷 문자열에 대응하는 가변 인자 리스트 (이미 va_start 처리됨).
 *
 * 사용자가 spdk_log_open()/_open_ext()에 이 콜백을 등록하면, SPDK 내부의
 * 모든 SPDK_*LOG 매크로 호출이 결국 이 콜백으로 라우팅된다. 등록하지
 * 않으면 lib/log/log.c의 기본 핸들러가 syslog/stderr로 출력한다.
 * 컨텍스트: 로그를 호출한 임의의 SPDK thread (reactor 컨텍스트일 수 있음).
 *           따라서 이 콜백은 빠르고 non-blocking이어야 하며, mutex 사용 시
 *           reactor lockless 모델을 깨뜨리지 않도록 주의해야 한다.
 */
typedef void spdk_log_cb(int level, const char *file, const int line,
			 const char *func, const char *format, va_list args);

/**
 * for opening user-provided logger
 *
 * \param ctx User-defined context for log open/close
 */
/*
 * [한국어]
 * spdk_log_open_cb - 로그 시스템이 초기화될 때 한 번 호출되는 사용자 훅.
 *
 * @ctx: spdk_log_opts.user_ctx로 전달한 사용자 컨텍스트 포인터.
 *
 * 외부 로깅 시스템(예: 사용자 정의 syslog 대안, Fluentd 클라이언트 등) 연결
 * 같은 1회성 초기화에 사용된다. spdk_log_open_ext() 내부에서 호출된다.
 */
typedef void spdk_log_open_cb(void *ctx);

/**
 * for closing user-provided logger
 *
 * \param ctx User-defined context for log open/close
 */
/*
 * [한국어]
 * spdk_log_close_cb - 로그 시스템이 종료될 때 한 번 호출되는 사용자 훅.
 *
 * @ctx: spdk_log_opts.user_ctx로 전달한 사용자 컨텍스트 포인터.
 *
 * spdk_log_close()에서 호출되며, 외부 자원 해제(파일 디스크립터 close,
 * 네트워크 소켓 종료 등)에 사용된다.
 */
typedef void spdk_log_close_cb(void *ctx);

/*
 * [한국어] spdk_log_open_ext()에 전달하는 옵션 묶음.
 * 세 콜백(log/open/close)과 사용자 컨텍스트를 하나의 구조체로 묶어, ABI
 * 호환성(size 필드)과 함께 한꺼번에 전달할 수 있게 한다. */
struct spdk_log_opts {
	/**
	 * The size of spdk_log_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t			size;
	/* [한국어] 호출자가 인지하고 있는 본 구조체 크기 (sizeof(struct spdk_log_opts)).
	 * 설정자: 사용자가 SPDK_SIZEOF()/sizeof로 채움. 0이면 잘못된 사용.
	 * 읽는 자: lib/log/log.c가 "옛 버전 ABI를 사용 중인지" 판별, 신규 필드는 기본값으로 채움.
	 * 값 범위: 양수, sizeof(struct spdk_log_opts) 이하. */

	spdk_log_cb		*log;
	/* [한국어] 메시지 1건마다 호출되는 콜백 (위 typedef 참고).
	 * 설정자: 사용자. NULL이면 기본 syslog/stderr 핸들러 사용.
	 * 읽는 자: spdk_log()/spdk_vlog()의 디스패치 경로. */

	spdk_log_open_cb	*open;
	/* [한국어] spdk_log_open_ext() 시 1회 호출되는 초기화 훅.
	 * 설정자: 사용자. NULL 허용 (호출 생략).
	 * 읽는 자: lib/log/log.c. */

	spdk_log_close_cb	*close;
	/* [한국어] spdk_log_close() 시 1회 호출되는 종료 훅.
	 * 설정자: 사용자. NULL 허용.
	 * 읽는 자: lib/log/log.c. */

	void			*user_ctx;
	/* [한국어] open/close 콜백에 전달되는 사용자 정의 포인터 (불투명).
	 * 설정자: 사용자.
	 * 읽는 자: open/close 콜백 본문에서 ctx 인자로 받음. */
};
/* [한국어] ABI 안정성 검증 — 빌드 시 sizeof가 40바이트가 아니면 컴파일 에러.
 * size_t(8) + cb포인터*3(24) + user_ctx(8) = 40 바이트. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_log_opts) == 40, "Incorrect size");

/**
 * Initialize the logging module. Messages prior
 * to this call will be dropped.
 */
/*
 * [한국어]
 * spdk_log_open - 로그 시스템을 초기화하는 기본 진입점.
 *
 * @log: 메시지 1건마다 호출될 사용자 콜백. NULL이면 기본 syslog 핸들러 사용.
 *
 * 이 함수 호출 이전에 발생한 SPDK_*LOG 메시지는 모두 드롭된다.
 * 일반적으로 spdk_app_start() 또는 자체 main()의 매우 이른 시점에 호출.
 * 호출 체인: app/main → spdk_log_open → openlog(3) (기본 핸들러).
 */
void spdk_log_open(spdk_log_cb *log);

/**
 * Extended API to initialize the logging module. Messages prior
 * to this call will be dropped.
 *
 * \param opts Options to provide log related functions with user-defined context for open/close
 */
/*
 * [한국어]
 * spdk_log_open_ext - 확장 옵션을 사용하는 로그 시스템 초기화.
 *
 * @opts: log/open/close 콜백과 user_ctx, ABI 호환용 size를 담은 구조체.
 *
 * spdk_log_open()과 달리 open/close 훅과 사용자 컨텍스트까지 등록 가능.
 * opts == NULL이면 spdk_log_open(NULL)과 동등하게 기본 동작.
 * 호출 체인: app → spdk_log_open_ext → opts->open(user_ctx) (있는 경우).
 */
void spdk_log_open_ext(struct spdk_log_opts *opts);

/**
 * Close the currently active log. Messages after this call
 * will be dropped.
 */
/*
 * [한국어]
 * spdk_log_close - 로그 시스템 종료. 이후 발생하는 메시지는 드롭됨.
 *
 * spdk_log_open_ext()로 등록된 close 훅이 있으면 함께 호출된다.
 * 호출 체인: app/shutdown → spdk_log_close → closelog(3) + 사용자 close 훅.
 */
void spdk_log_close(void);

/**
 * Enable or disable timestamps
 */
/*
 * [한국어]
 * spdk_log_enable_timestamps - 각 메시지 앞머리에 타임스탬프 prefix를 붙일지 토글.
 *
 * @value: true면 타임스탬프 ON, false면 OFF.
 *
 * 활성화 시 stderr 출력에 "[YYYY-MM-DD HH:MM:SS.mmm]" 형태 prefix 부착.
 * syslog는 자체 타임스탬프를 가지므로 영향 없음.
 */
void spdk_log_enable_timestamps(bool value);

/* [한국어] SPDK 로그 레벨 — 숫자가 클수록 세부적(=verbose).
 * SPDK_LOG_DISABLED만 음수이며 "출력 안 함"을 뜻한다.
 * spdk_log_to_syslog_level()이 syslog의 LOG_ERR/WARNING/NOTICE/INFO/DEBUG로 매핑한다. */
enum spdk_log_level {
	/** All messages will be suppressed. */
	SPDK_LOG_DISABLED = -1,
	/* [한국어] 출력 완전 비활성. set_level/set_print_level에 전달하면 모든 메시지 차단. */

	SPDK_LOG_ERROR,
	/* [한국어] 0 — 시스템에 영향이 가는 오류 (예: NVMe 어드민 명령 실패, malloc 실패). */

	SPDK_LOG_WARN,
	/* [한국어] 1 — 주의가 필요한 비정상 상황이지만 동작은 계속 가능. */

	SPDK_LOG_NOTICE,
	/* [한국어] 2 — 중요한 정상 이벤트 (앱 시작/종료, bdev attach 등). 기본 임계값. */

	SPDK_LOG_INFO,
	/* [한국어] 3 — 컴포넌트별 INFO (디버그 플래그 enabled여야 출력). */

	SPDK_LOG_DEBUG,
	/* [한국어] 4 — 가장 verbose. DEBUG 빌드 + 컴포넌트 플래그 enabled에서만 활성. */
};

/**
 * Set the log level threshold to log messages. Messages with a higher
 * level than this are ignored.
 *
 * \param level Log level threshold to set to log messages.
 */
/*
 * [한국어]
 * spdk_log_set_level - syslog로 보낼 메시지의 임계값을 설정.
 *
 * @level: 이 값보다 큰(=더 verbose한) 메시지는 무시된다.
 *         예: SPDK_LOG_NOTICE → ERROR/WARN/NOTICE만 출력, INFO/DEBUG 차단.
 *
 * RPC log_set_level이 내부적으로 호출하므로 운영 중 동적 변경 가능.
 */
void spdk_log_set_level(enum spdk_log_level level);

/**
 * Get the current log level threshold.
 *
 * \return the current log level threshold.
 */
/*
 * [한국어]
 * spdk_log_get_level - 현재 syslog 임계값 반환.
 * @return: enum spdk_log_level 값 (-1 ~ 4).
 */
enum spdk_log_level spdk_log_get_level(void);

/**
 * Get syslog level based on SPDK current log level threshold.
 *
 * \param level Log level threshold
 * \return -1 for disable log print, otherwise is syslog level.
 */
/*
 * [한국어]
 * spdk_log_to_syslog_level - SPDK 레벨을 POSIX syslog(3) 레벨로 변환.
 *
 * @level: SPDK_LOG_* 값.
 * @return: syslog의 LOG_ERR/LOG_WARNING/LOG_NOTICE/LOG_INFO/LOG_DEBUG 정수.
 *          SPDK_LOG_DISABLED인 경우 -1.
 *
 * lib/log/log.c가 기본 sink로 syslog(3)를 호출할 때 priority 인자 변환에 사용.
 */
int spdk_log_to_syslog_level(enum spdk_log_level level);

/**
 * Set the current log level threshold for printing to stderr.
 * Messages with a level less than or equal to this level
 * are also printed to stderr. You can use \c SPDK_LOG_DISABLED to completely
 * suppress log printing.
 *
 * \param level Log level threshold for printing to stderr.
 */
/*
 * [한국어]
 * spdk_log_set_print_level - stderr 출력 임계값 설정 (syslog 임계값과 별개).
 *
 * @level: 이 값과 같거나 덜 verbose한 메시지가 stderr에도 출력됨.
 *
 * 운영 환경: syslog는 유지하면서 stderr만 NOTICE 이상으로 줄여 콘솔 노이즈 감소.
 * 디버그 환경: print_level을 DEBUG로 올려 콘솔에서 즉시 확인.
 */
void spdk_log_set_print_level(enum spdk_log_level level);

/**
 * Get the current log level print threshold.
 *
 * \return the current log level print threshold.
 */
/*
 * [한국어]
 * spdk_log_get_print_level - 현재 stderr 출력 임계값 반환.
 */
enum spdk_log_level spdk_log_get_print_level(void);

/* [한국어] DEBUG 빌드 여부에 따른 컴포넌트별 디버그 플래그 활성 여부 질의 매크로.
 * - DEBUG 빌드: spdk_log_get_flag(name)으로 플래그 enabled 여부 조회.
 * - 릴리즈 빌드: 항상 false → 컴파일러가 dead code elimination으로 분기 제거.
 * 이 패턴 덕분에 SPDK_DEBUGLOG는 릴리즈 빌드에서 비용 0이 된다. */
#ifdef DEBUG
#define SPDK_DEBUGLOG_FLAG_ENABLED(name) spdk_log_get_flag(name)
#else
#define SPDK_DEBUGLOG_FLAG_ENABLED(name) false
#endif

/* [한국어] === SPDK 로그 매크로 (사용자 진입점) ===
 * 모든 매크로가 __FILE__/__LINE__/__func__를 자동 포착해 spdk_log()로 전달.
 * 호출자는 printf 스타일 가변인자만 작성하면 된다.
 *   사용 예: SPDK_NOTICELOG("bdev %s attached\n", name);
 */

#define SPDK_NOTICELOG(...) \
	spdk_log(SPDK_LOG_NOTICE, __FILE__, __LINE__, __func__, __VA_ARGS__)
/* [한국어] NOTICE 레벨 메시지 — 일반 정상 이벤트(앱 시작, attach 등) 알림용. */

#define SPDK_WARNLOG(...) \
	spdk_log(SPDK_LOG_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
/* [한국어] WARN 레벨 — 의도하지 않은 상태이지만 동작은 계속 가능한 경우. */

#define SPDK_ERRLOG(...) \
	spdk_log(SPDK_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
/* [한국어] ERROR 레벨 — 실패가 발생한 지점에서 호출. 항상 출력 임계값을 통과. */

#define SPDK_PRINTF(...) \
	spdk_log(SPDK_LOG_NOTICE, NULL, -1, NULL, __VA_ARGS__)
/* [한국어] NOTICE 레벨로 출력하되 file/line/func 정보 없이 순수 본문만 출력.
 * 사용자 직접 출력(예: 시작 배너) 용도. */

#define SPDK_INFOLOG(flag, ...)									\
	do {											\
		extern struct spdk_log_flag SPDK_LOG_##flag;					\
		if (SPDK_LOG_##flag.enabled) {							\
			spdk_log(SPDK_LOG_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__);	\
		}										\
	} while (0)
/* [한국어] 컴포넌트별 INFO 로그.
 * 사용 예: SPDK_INFOLOG(bdev, "queue depth=%u\n", qd);
 * SPDK_LOG_##flag(예: SPDK_LOG_bdev) 전역 플래그가 enabled일 때만 출력.
 * extern 선언은 매크로 내부에서 lazy하게 수행해 헤더 의존을 줄인다.
 * do { } while (0) 래핑은 if/else 문 내부에서 안전하게 사용되도록 함. */

#define SPDK_ERRLOG_RATELIMIT(...) \
	do {							\
		static uint64_t last_tsc = 0;			\
		static uint64_t squashed = 0;			\
		uint64_t tsc = spdk_get_ticks();		\
		if (tsc > last_tsc + spdk_get_ticks_hz()) {	\
			last_tsc = tsc;				\
			SPDK_ERRLOG(__VA_ARGS__);		\
			if (squashed > 0) {			\
				SPDK_ERRLOG("(same message squashed %" PRIu64 " times)\n", \
					    squashed);		\
				squashed = 0;			\
			}					\
		} else {					\
			squashed++;				\
		}						\
	} while (0)
/* [한국어] 1초당 최대 1회만 ERROR 메시지를 출력하는 rate-limit 매크로.
 * - last_tsc/squashed: static 지역 변수로 호출 사이트별 상태 보존.
 * - spdk_get_ticks() / spdk_get_ticks_hz(): TSC 기반 카운터(env에서 제공).
 * - tsc - last_tsc >= 1초이면 ERRLOG 출력 후 last_tsc 갱신.
 *   그 사이에 압축된 메시지 수가 0보다 크면 "squashed N times" 보조 메시지 출력.
 * - I/O 핫패스에서 같은 에러가 폭주할 때 syslog/디스크 폭격 방지. */

#ifdef DEBUG
/* [한국어] DEBUG 빌드에서만 실제 코드를 생성. 릴리즈 빌드는 do { } while (0) noop. */

#define SPDK_DEBUGLOG(flag, ...)								\
	do {											\
		extern struct spdk_log_flag SPDK_LOG_##flag;					\
		if (SPDK_LOG_##flag.enabled) {							\
			spdk_log(SPDK_LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__);	\
		}										\
	} while (0)
/* [한국어] 컴포넌트 플래그가 켜져 있으면 DEBUG 메시지 출력.
 * 릴리즈에서는 완전 제거되어 zero-cost. */

#define SPDK_LOGDUMP(flag, label, buf, len)				\
	do {								\
		extern struct spdk_log_flag SPDK_LOG_##flag;		\
		if (SPDK_LOG_##flag.enabled) {				\
			spdk_log_dump(stderr, (label), (buf), (len));	\
		}							\
	} while (0)
/* [한국어] 임의 버퍼를 hex dump 형식으로 stderr에 덤프 (DEBUG 빌드 + flag enabled).
 * NVMe CQE/SQE, 프로토콜 패킷 디버깅에 자주 쓰임. */

#else
#define SPDK_DEBUGLOG(...) do { } while (0)
/* [한국어] 릴리즈 빌드: 호출 자체가 사라짐. ABI 호환을 위해 형태만 유지. */
#define SPDK_LOGDUMP(...) do { } while (0)
/* [한국어] 릴리즈 빌드: noop. */
#endif

/**
 * Write messages to the log file. If \c level is set to \c SPDK_LOG_DISABLED,
 * this log message won't be written.
 *
 * \param level Log level threshold.
 * \param file Name of the current source file.
 * \param line Current source line number.
 * \param func Current source function name.
 * \param format Format string to the message.
 */
/*
 * [한국어]
 * spdk_log - 모든 SPDK_*LOG 매크로의 종착 함수 (가변 인자판).
 *
 * @level: 메시지 심각도. SPDK_LOG_DISABLED이면 즉시 무시.
 * @file/@line/@func: 호출 위치 정보 (보통 매크로가 자동 주입).
 * @format: printf 스타일 포맷.
 *
 * 등록된 spdk_log_cb가 있으면 그쪽으로 위임, 없으면 syslog + stderr 출력.
 * __attribute__((__format__(__printf__, 5, 6)))는 GCC/Clang에게 5번째 인자가
 * printf 포맷, 6번째부터가 가변 인자임을 알려 -Wformat 경고를 활성화한다.
 */
void spdk_log(enum spdk_log_level level, const char *file, const int line, const char *func,
	      const char *format, ...) __attribute__((__format__(__printf__, 5, 6)));

/**
 * Same as spdk_log except that instead of being called with variable number of
 * arguments it is called with an argument list as defined in stdarg.h
 *
 * \param level Log level threshold.
 * \param file Name of the current source file.
 * \param line Current source line number.
 * \param func Current source function name.
 * \param format Format string to the message.
 * \param ap printf arguments
 */
/*
 * [한국어]
 * spdk_vlog - spdk_log()의 va_list 버전.
 *
 * @ap: 호출자가 va_start()로 준비한 가변 인자 리스트.
 *
 * 다른 가변 인자 함수에서 받은 인자를 그대로 전달할 때 사용.
 * spdk_log()는 내부적으로 va_start → spdk_vlog → va_end 흐름.
 */
void spdk_vlog(enum spdk_log_level level, const char *file, const int line, const char *func,
	       const char *format, va_list ap);

/**
 * Write messages to the log file. If \c level is set to \c SPDK_LOG_DISABLED,
 * this log message won't be written.
 *
 * \param fp File to hold the log.
 * \param file Name of the current source file.
 * \param line Current source line number.
 * \param func Current source function name.
 * \param format Format string to the message.
 */
/*
 * [한국어]
 * spdk_flog - 임의 FILE* 스트림으로 직접 출력 (가변 인자판).
 *
 * @fp: 출력 대상 FILE*. 보통 stderr나 사용자가 fopen한 로그 파일.
 *
 * 전역 임계값과 무관하게 fp로 직접 메시지를 출력. 임계값을 우회해야 하는
 * RPC 응답 본문, 컴포넌트별 진단 도구에서 사용.
 */
void spdk_flog(FILE *fp, const char *file, const int line, const char *func,
	       const char *format, ...) __attribute__((__format__(__printf__, 5, 6)));

/**
 * Same as spdk_flog except that instead of being called with variable number of
 * arguments it is called with an argument list as defined in stdarg.h
 *
 * \param fp File to hold the log.
 * \param file Name of the current source file.
 * \param line Current source line number.
 * \param func Current source function name.
 * \param format Format string to the message.
 * \param ap printf arguments
 */
/*
 * [한국어]
 * spdk_vflog - spdk_flog()의 va_list 버전. 호출 패턴은 spdk_vlog()와 동일.
 */
void spdk_vflog(FILE *fp, const char *file, const int line, const char *func,
		const char *format, va_list ap);

/**
 * Log the contents of a raw buffer to a file.
 *
 * \param fp File to hold the log.
 * \param label Label to print to the file.
 * \param buf Buffer that holds the log information.
 * \param len Length of buffer to dump.
 */
/*
 * [한국어]
 * spdk_log_dump - 임의 버퍼를 16진수 + ASCII 형식으로 fp에 덤프.
 *
 * @fp: 출력 대상 FILE*.
 * @label: 덤프 시작 시 함께 인쇄할 라벨 문자열 (예: "NVMe SQE").
 * @buf: 덤프 시작 주소.
 * @len: 덤프 바이트 수.
 *
 * SPDK_LOGDUMP 매크로의 본체. NVMe SQE/CQE, NVMe-oF 캡슐 디버깅 등에 활용.
 */
void spdk_log_dump(FILE *fp, const char *label, const void *buf, size_t len);

/* [한국어] 컴포넌트별 디버그 플래그 등록 단위.
 * SPDK_LOG_REGISTER_COMPONENT(name) 매크로가 SPDK_LOG_##name 인스턴스를 만들고
 * __attribute__((constructor))로 spdk_log_register_flag()에 자동 등록한다.
 * 등록된 플래그들은 lib/log/log.c가 TAILQ로 관리하며, RPC log_set_flag /
 * log_clear_flag로 런타임에 enabled를 토글할 수 있다. */
struct spdk_log_flag {
	TAILQ_ENTRY(spdk_log_flag) tailq;
	/* [한국어] 글로벌 플래그 리스트 연결용 노드 (BSD queue.h TAILQ).
	 * 설정자: spdk_log_register_flag()가 TAILQ_INSERT_TAIL.
	 * 읽는 자: spdk_log_get_first_flag/spdk_log_get_next_flag로 순회. */

	const char *name;
	/* [한국어] 컴포넌트 이름 문자열 (예: "bdev", "nvme", "thread").
	 * 설정자: SPDK_LOG_REGISTER_COMPONENT(flag) 매크로가 #flag 문자열로 채움.
	 * 읽는 자: spdk_log_set_flag/clear_flag가 fnmatch로 매칭. */

	bool enabled;
	/* [한국어] 이 컴포넌트의 INFO/DEBUG 메시지 출력 활성 여부.
	 * 설정자: spdk_log_set_flag/clear_flag (RPC 또는 CLI에서 호출).
	 * 읽는 자: SPDK_INFOLOG/DEBUGLOG 매크로가 분기 조건으로 사용.
	 * 기본값: false (오버헤드 0). */
};

/**
 * Register a log flag.
 *
 * \param name Name of the log flag.
 * \param flag Log flag to be added.
 */
/*
 * [한국어]
 * spdk_log_register_flag - 컴포넌트 디버그 플래그를 글로벌 리스트에 추가.
 *
 * @name: 사용자/RPC가 참조할 컴포넌트 이름.
 * @flag: 등록할 spdk_log_flag 구조체 포인터 (수명: 프로세스 전체).
 *
 * 보통 SPDK_LOG_REGISTER_COMPONENT 매크로가 자동으로 호출하므로, 사용자가
 * 직접 부를 일은 거의 없다. lib/log/log.c가 TAILQ_HEAD로 관리.
 */
void spdk_log_register_flag(const char *name, struct spdk_log_flag *flag);

#define SPDK_LOG_REGISTER_COMPONENT(flag) \
struct spdk_log_flag SPDK_LOG_##flag = { \
	.name = #flag, \
	.enabled = false, \
}; \
__attribute__((constructor)) static void register_flag_##flag(void) \
{ \
	spdk_log_register_flag(#flag, &SPDK_LOG_##flag); \
}
/* [한국어] === 컴포넌트 자동 등록 매크로 ===
 * 컴포넌트 .c 파일 한 곳에 SPDK_LOG_REGISTER_COMPONENT(bdev) 식으로 적으면:
 *   1) SPDK_LOG_bdev라는 이름의 spdk_log_flag 인스턴스를 .data 섹션에 정의
 *      (.name="bdev", .enabled=false 초기값).
 *   2) constructor 속성을 가진 register_flag_bdev() 함수 정의.
 *      → main() 진입 전(crt0 단계) 자동 호출되어 spdk_log_register_flag()
 *        실행 → 글로벌 TAILQ에 자동 삽입.
 *   3) 같은 컴파일 유닛 외부에서는 SPDK_INFOLOG/DEBUGLOG 매크로가 extern 선언으로
 *      이 인스턴스에 접근.
 * 결과: .c 파일 단 1줄만으로 컴포넌트가 RPC/CLI 인터페이스에 노출됨. */

/**
 * Get the first registered log flag.
 *
 * \return The first registered log flag.
 */
/*
 * [한국어]
 * spdk_log_get_first_flag - 글로벌 플래그 리스트의 첫 항목 반환.
 * 비어 있으면 NULL.
 */
struct spdk_log_flag *spdk_log_get_first_flag(void);

/**
 * Get the next registered log flag.
 *
 * \param flag The current log flag.
 *
 * \return The next registered log flag.
 */
/*
 * [한국어]
 * spdk_log_get_next_flag - 현재 노드 다음의 플래그 반환.
 *
 * @flag: 현재 노드 (NULL 불가).
 * @return: 다음 노드. 마지막이면 NULL.
 *
 * spdk_log_usage()나 RPC 핸들러가 모든 플래그를 순회할 때 사용.
 *   for (f = spdk_log_get_first_flag(); f; f = spdk_log_get_next_flag(f))
 */
struct spdk_log_flag *spdk_log_get_next_flag(struct spdk_log_flag *flag);

/**
 * Check whether the log flag exists and is enabled.
 *
 * \return true if enabled, or false otherwise.
 */
/*
 * [한국어]
 * spdk_log_get_flag - 이름으로 플래그를 찾아 enabled 여부 반환.
 *
 * @flag: 컴포넌트 이름 문자열 (정확히 일치).
 * @return: 존재하지 않거나 비활성이면 false, 활성이면 true.
 *
 * SPDK_DEBUGLOG_FLAG_ENABLED(name) 매크로가 내부적으로 호출.
 */
bool spdk_log_get_flag(const char *flag);

/**
 * Enable the log flag.  The name of the flag can be a glob pattern (as expanded by fnmatch(3)), in
 * which case all matching flags will be set.
 *
 * \param flag Log flag to be enabled.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_log_set_flag - 플래그를 활성화 (fnmatch glob 패턴 지원).
 *
 * @flag: 단일 이름("bdev") 또는 glob 패턴("nvme*", "*").
 * @return: 0 성공, -ENOENT 등 음수 errno 실패.
 *
 * 패턴 매칭은 fnmatch(3)로 수행 — "*"는 모든 컴포넌트 활성화.
 */
int spdk_log_set_flag(const char *flag);

/**
 * Clear a log flag.  The name of the flag can be a glob pattern (as expanded by fnmatch(3)), in
 * which case all matching flags will be cleared.
 *
 * \param flag Log flag to clear.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_log_clear_flag - 플래그를 비활성화. set_flag와 대칭, glob 지원.
 */
int spdk_log_clear_flag(const char *flag);

/**
 * Show all the log flags and their usage.
 *
 * \param f File to hold all the flags' information.
 * \param log_arg Command line option to set/enable the log flag.
 */
/*
 * [한국어]
 * spdk_log_usage - 모든 등록된 플래그를 사용법과 함께 출력.
 *
 * @f: 출력 대상 FILE* (보통 stderr).
 * @log_arg: 출력 메시지에 사용할 옵션 문자열 (예: "-L").
 *
 * spdk_app의 --help 출력에 통합되어 사용자가 어떤 플래그를 켤 수 있는지 안내.
 */
void spdk_log_usage(FILE *f, const char *log_arg);

/* [한국어] deprecation 추적 핸들의 forward 선언. 실제 정의는 lib/log/log_deprecation.c.
 * 사용자는 SPDK_LOG_DEPRECATION_REGISTER 매크로가 만든 static 포인터로만 접근. */
struct spdk_deprecation;

/**
 * Register a deprecation. Most consumers will use SPDK_LOG_DEPRECATION_REGISTER() instead.
 *
 * \param tag A unique string that will appear in each log message and should appear in
 * documentation.
 * \param description A descriptive string that will also be logged.
 * \param rate_limit_seconds If non-zero, log messages related to this deprecation will appear no
 * more frequently than this interval.
 * \param remove_release The release when the deprecated support will be removed.
 * \param reg Pointer to storage for newly allocated deprecation handle.
 * \return 0 on success or negative errno on failure.
 */
/*
 * [한국어]
 * spdk_log_deprecation_register - 폐기 예정 기능을 추적할 핸들 생성.
 *
 * @tag: 고유 식별 문자열 (로그/문서 양쪽에서 동일하게 사용).
 * @description: 사용자에게 보일 한 줄 설명.
 * @remove_release: 제거 예정 릴리즈 라벨 (예: "v25.05").
 * @rate_limit_seconds: 알림 주기. 0이면 매번 출력, SPDK_LOG_DEPRECATION_EVERY_24H면 24h마다.
 * @reg: 출력 — 새로 할당된 핸들 포인터.
 * @return: 0 성공 / 음수 errno 실패.
 *
 * 보통 직접 호출하지 않고 SPDK_LOG_DEPRECATION_REGISTER 매크로로 자동화.
 */
int spdk_log_deprecation_register(const char *tag, const char *description,
				  const char *remove_release, uint32_t rate_limit_seconds,
				  struct spdk_deprecation **reg);

#define SPDK_LOG_DEPRECATION_REGISTER(tag, desc, release, rate) \
	static struct spdk_deprecation *_deprecated_##tag; \
	static void __attribute__((constructor)) _spdk_deprecation_register_##tag(void) \
	{ \
		int rc; \
		rc = spdk_log_deprecation_register(#tag, desc, release, rate, &_deprecated_##tag); \
		(void)rc; \
		assert(rc == 0); \
	}
/* [한국어] === Deprecation 자동 등록 매크로 ===
 * 컴파일 유닛에 한 줄만 적으면 _deprecated_##tag 정적 포인터가 만들어지고,
 * constructor 속성으로 main() 진입 전 자동 등록된다.
 *   1) static 포인터 정의 (각 컴파일 유닛에 하나).
 *   2) constructor 함수에서 spdk_log_deprecation_register 호출하여 핸들 획득.
 *   3) 등록 실패 시 assert로 죽임 (개발 단계 검출).
 * 이후 SPDK_LOG_DEPRECATED(tag)로 사용 시점을 보고. */

/**
 * Indicate that a deprecated feature was used. Most consumers will use SPDK_LOG_DEPRECATED()
 * instead.
 *
 * \param deprecation The deprecated feature that was used.
 * \param file The name of the source file where the deprecated feature was used.
 * \param line The line in file where where the deprecated feature was used.
 * \param func The name of the function where where the deprecated feature was used.
 */
/*
 * [한국어]
 * spdk_log_deprecated - 폐기 예정 기능 사용 시 호출되는 보고 함수.
 *
 * @deprecation: REGISTER로 얻은 핸들.
 * @file/@line/@func: 호출 위치 (보통 매크로가 자동 채움).
 *
 * 내부에서 hit 카운터 증가 + rate-limit 통과 시 WARNLOG 발행.
 */
void spdk_log_deprecated(struct spdk_deprecation *deprecation, const char *file, uint32_t line,
			 const char *func);

#define SPDK_LOG_DEPRECATED(tag) \
	spdk_log_deprecated(_deprecated_##tag, __FILE__, __LINE__, __func__)
/* [한국어] 사용자가 폐기 예정 코드 경로에 한 줄 삽입하는 보고 매크로.
 *   예: SPDK_LOG_DEPRECATED(old_rpc_method);
 * REGISTER 매크로가 만든 _deprecated_##tag 정적 포인터를 참조하므로 같은
 * 컴파일 유닛에서만 사용 가능. */

/**
 * Callback function for spdk_log_for_each_deprecation().
 *
 * \param ctx Context passed via spdk_log_for_each_deprecation().
 * \param deprecation Pointer to a deprecation structure.
 * \return 0 to continue iteration or non-zero to stop iteration.
 */
/*
 * [한국어]
 * spdk_log_for_each_deprecation_fn - 순회 콜백 시그니처.
 *
 * @ctx: 호출자가 전달한 사용자 컨텍스트.
 * @deprecation: 현재 순회 중인 deprecation 핸들.
 * @return: 0이면 계속, 0이 아니면 즉시 중단하고 그 값을 반환.
 */
typedef int (*spdk_log_for_each_deprecation_fn)(void *ctx, struct spdk_deprecation *deprecation);

/**
 * Iterate over all deprecations, calling a callback on each of them.
 *
 * Iteration will stop early if the callback function returns non-zero.
 *
 * \param ctx Context to pass to the callback.
 * \param fn Callback function
 * \return The value from the last callback called or 0 if there are no deprecations.
 */
/*
 * [한국어]
 * spdk_log_for_each_deprecation - 등록된 모든 deprecation을 순회하며 fn 호출.
 *
 * @ctx: fn에 그대로 전달될 컨텍스트.
 * @fn: 각 deprecation에 대해 호출될 콜백.
 * @return: fn이 0이 아닌 값을 반환한 경우 그 값, 모두 0이면 0.
 *
 * RPC log_get_deprecations가 내부적으로 호출하여 JSON 응답을 생성.
 */
int spdk_log_for_each_deprecation(void *ctx, spdk_log_for_each_deprecation_fn fn);

/**
 * Get a deprecation's tag.
 *
 * \param deprecation A pointer to an spdk_deprecation.
 * \return The deprecation's tag.
 */
/*
 * [한국어]
 * spdk_deprecation_get_tag - 핸들에서 tag 문자열 추출 (RPC 응답 등 용도).
 */
const char *spdk_deprecation_get_tag(const struct spdk_deprecation *deprecation);

/**
 * Get a deprecation's description.
 *
 * \param deprecation A pointer to an spdk_deprecation.
 * \return The deprecation's description.
 */
/*
 * [한국어]
 * spdk_deprecation_get_description - 사용자용 설명 문자열 반환.
 */
const char *spdk_deprecation_get_description(const struct spdk_deprecation *deprecation);

/**
 * Get a deprecation's planned removal release.
 *
 * \param deprecation A pointer to an spdk_deprecation.
 * \return The deprecation's planned removal release.
 */
/*
 * [한국어]
 * spdk_deprecation_get_remove_release - 제거 예정 릴리즈 라벨 반환.
 */
const char *spdk_deprecation_get_remove_release(const struct spdk_deprecation *deprecation);

/**
 * Get the number of times that a deprecation's code has been executed.
 *
 * \param deprecation A pointer to an spdk_deprecation.
 * \return The deprecation's planned removal release.
 */
/*
 * [한국어]
 * spdk_deprecation_get_hits - 이 deprecation 코드 경로가 실행된 누적 횟수 반환.
 *
 * 운영 모니터링: hits > 0인 deprecation은 사용자가 아직 옛 API에 의존 중임을 시사.
 */
uint64_t spdk_deprecation_get_hits(const struct spdk_deprecation *deprecation);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_LOG_H */ /* [한국어] include 가드 닫기 */
