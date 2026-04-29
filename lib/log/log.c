/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK 통합 로그 출력 구현 (log.c)
 *
 * === 파일의 역할 ===
 * SPDK 전 모듈(NVMe 드라이버, bdev, nvmf, thread, RPC 등)이 호출하는
 * SPDK_ERRLOG/WARNLOG/NOTICELOG/INFOLOG/DEBUGLOG 매크로의 최종 출력단을 구현한다.
 * 이 파일은 (1) 메시지 포맷팅(printf 스타일 가변 인자 + 타임스탬프 + 파일:라인:함수 헤더),
 * (2) 레벨 필터링(g_spdk_log_level / g_spdk_log_print_level 두 단계 컷오프),
 * (3) 싱크(sink) 추상화 — 사용자 콜백이 등록되어 있으면 그쪽으로 위임하고, 없으면
 *     stderr와 syslog 두 곳에 동시 출력 — 의 세 가지 책임을 진다.
 * 또한 hex dump 헬퍼(spdk_log_dump)와 외부 FILE* 직접 출력(spdk_flog/vflog)도 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 라이브러리 가장 하위에 위치하는 진단/관측(observability) 레이어이다.
 * 호출 체인:
 *   [상위 레이어 매크로]  SPDK_ERRLOG / DEBUGLOG / NOTICELOG / TRACELOG (include/spdk/log.h)
 *      → [공개 API]      spdk_log()         (가변 인자 포장)
 *      → [공개 API]      spdk_vlog()        (실제 포맷팅·필터링·싱크 분기)
 *         ├─ g_log_opts.log != NULL → 사용자 콜백으로 위임 (예: 외부 로그 시스템 통합)
 *         └─ 기본 경로:  vsnprintf → fprintf(stderr) + syslog()
 *   [상위 레이어 매크로]  SPDK_LOG_DUMP    → spdk_log_dump → fdump (hex dump)
 *   [부트스트랩]         spdk_app_start() → spdk_log_open() (앱 초기화 단계에서 sink 결정)
 * 실행 컨텍스트: 호스트 유저스페이스. SPDK는 polled-mode이므로 이 함수들은 reactor
 * 스레드(코어에 고정된 1:1 spdk_thread)에서 호출되지만, syslog/stderr 자체는 process-
 * global 자원이라 여러 reactor lcore에서 동시 호출되어도 안전해야 한다 — fprintf와
 * syslog의 thread safety(POSIX glibc 구현)에 의존한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: libc <stdio.h>(fprintf/vsnprintf/vasprintf), <syslog.h>(openlog/syslog/closelog),
 *         <time.h>(clock_gettime/localtime/strftime), <ctype.h>(isprint).
 *         SPDK 내부로는 spdk/util.h의 SPDK_GET_FIELD/SPDK_SIZEOF 매크로(ABI 호환용).
 *         spdk/stdinc.h가 위 표준 헤더를 일괄 인클루드한다.
 * - 의존되는 쪽: SPDK 내 거의 모든 .c 파일이 SPDK_*LOG 매크로를 통해 spdk_log()를
 *               호출한다. RPC `log_set_level/print_level`, app 부트스트랩에서
 *               spdk_log_set_level/spdk_log_open을 호출한다.
 * - 공유 상태: g_log_opts(싱크 추상화 함수 포인터 묶음), g_spdk_log_level,
 *             g_spdk_log_print_level, g_log_timestamps. 모두 프로세스 전역 단일 사본이며,
 *             부트스트랩 단계에서만 쓰기/이후엔 거의 read-only.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_log_set_level / get_level / set_print_level / get_print_level :
 *       두 단계 레벨 필터(g_spdk_log_level=syslog 컷, g_spdk_log_print_level=stderr 컷)
 *       를 조작·조회. RPC `log_set_level`이 이 함수를 호출.
 *   - spdk_log_open / spdk_log_open_ext / spdk_log_close :
 *       싱크 등록 진입점. NULL을 주면 기본 syslog(facility=LOG_LOCAL7) + stderr 모드,
 *       사용자 콜백을 주면 그 콜백이 모든 메시지를 받는다(외부 로그 시스템 통합용).
 *   - spdk_log / spdk_vlog :
 *       실제 로그 출력 진입점. 레벨 필터 → vsnprintf로 메시지 빌드 → 싱크 분기.
 *       초과 길이는 vasprintf로 동적 확장하여 잘림 회피.
 *   - spdk_log_to_syslog_level :
 *       SPDK_LOG_* enum을 syslog(3)의 LOG_INFO/NOTICE/WARNING/ERR로 매핑.
 *   - spdk_log_dump → fdump :
 *       바이트 버퍼를 16바이트 폭 hex+ASCII 형태로 덤프(NVMe payload 디버깅용).
 *   - 전역 g_log_opts (struct spdk_log_opts) :
 *       싱크 함수 포인터 묶음 — log/open/close/user_ctx + size(ABI 검증).
 */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 인클루드 묶음 — <stdio.h>(fprintf, vsnprintf, vasprintf),
 * <stdarg.h>(va_list/va_start/va_copy/va_end), <string.h>(memset),
 * <syslog.h>(openlog/syslog/closelog/LOG_INFO/...), <time.h>(clock_gettime/localtime/
 * strftime), <ctype.h>(isprint), <assert.h>(assert), <stdint.h>, <stdbool.h>, <unistd.h>
 * 등을 모아 한 번에 가져온다. 본 파일은 거의 모든 libc I/O·시간·형변환 API를 사용하므로
 * 이 한 줄로 의존성을 충당한다. */
#include "spdk/util.h"
/* [한국어] SPDK 유틸 매크로/함수 — 본 파일에서는 spdk_log_open_ext()의 ABI 호환 처리에
 * 쓰는 SPDK_GET_FIELD(opts, field, default)와 SPDK_SIZEOF(opts, last_field) 두 매크로
 * 때문에 필요. SPDK_GET_FIELD는 호출자가 채워준 opts->size를 보고 해당 필드가 존재하는
 * 버전이면 값을, 아니면 default를 돌려줘 forward-compatibility를 보장한다. */

#include "spdk/log.h"
/* [한국어] 본 파일이 정의/선언과 일치하는지 확인하는 공개 API 헤더. enum spdk_log_level,
 * struct spdk_log_opts, struct spdk_log_flag, spdk_log_cb 타입, 그리고 본 파일이 정의하는
 * 모든 외부 함수 프로토타입을 가져온다. */

static const char *const spdk_level_names[] = {
	/* [한국어] enum spdk_log_level → 사람이 읽을 수 있는 문자열 매핑 테이블.
	 * 사용처: spdk_vlog가 stderr/syslog 헤더에 "*ERROR*" 같은 표시를 박을 때 인덱싱.
	 * 사용 형태: spdk_level_names[level] (level은 enum 값). designated initializer를
	 * 써서 enum 값과 인덱스가 맞도록 명시 — enum 순서가 바뀌어도 안전. */
	[SPDK_LOG_ERROR]	= "ERROR",
	/* [한국어] 가장 심각 — 시스템 동작에 영향을 주는 오류. */
	[SPDK_LOG_WARN]		= "WARNING",
	/* [한국어] 잠재적 문제 — 동작은 계속되지만 주의 필요. (SPDK_WARNLOG, deprecation 등) */
	[SPDK_LOG_NOTICE]	= "NOTICE",
	/* [한국어] 정상 동작 중 사용자가 알아야 할 이벤트. (앱 시작/종료, 디바이스 연결 등) */
	[SPDK_LOG_INFO]		= "INFO",
	/* [한국어] 일반 정보 — 디버깅에 도움이 되는 진행 상황. */
	[SPDK_LOG_DEBUG]	= "DEBUG",
	/* [한국어] 가장 상세 — 컴포넌트별 플래그(spdk_log_flag)가 켜져야 출력되는 흐름 추적. */
};

#define MAX_TMPBUF 1024
/* [한국어] 단일 로그 메시지 1건의 스택 버퍼 크기(바이트). 대부분의 SPDK 로그 메시지는
 * 한 줄짜리이므로 1KB로 충분하지만, 초과하면 vasprintf로 동적 할당해서 자르지 않는다
 * (spdk_vlog 본문 참조). 너무 크면 깊은 콜체인에서 스택 오버플로우 위험, 너무 작으면
 * heap 경유가 잦아져 성능 저하 — 1KB가 균형점. */

static struct spdk_log_opts g_log_opts = {
	/* [한국어] 프로세스 전역 싱크 설정(★ 핵심 — sink 추상화의 본체).
	 * 모든 spdk_vlog 호출은 우선 이 구조체의 .log 콜백 존재 여부를 확인하고,
	 * 있으면 그쪽으로 위임, 없으면 기본 syslog+stderr 경로로 떨어진다.
	 * 설정자: spdk_log_open() / spdk_log_open_ext() (앱 초기화 1회).
	 * 읽는 자: spdk_vlog() (모든 로그 출력에서 매번).
	 * 동기화: 부트스트랩에서 한 번 설정 후 거의 read-only이므로 락 없음.
	 *         런타임 중 변경한다면 race가 발생할 수 있으나 SPDK 정책상 그런 케이스
	 *         (콜백 동시 교체)는 지원하지 않음. */
	.log = NULL,        /* [한국어] 사용자 콜백 — NULL이면 기본 stderr+syslog 경로 사용. */
	.open = NULL,       /* [한국어] 싱크 오픈 훅 — log_open()이 기본값으로 들어가서 openlog() 호출. */
	.close = NULL,      /* [한국어] 싱크 클로즈 훅 — log_close()가 기본값으로 들어가서 closelog() 호출. */
	.user_ctx = NULL,   /* [한국어] open/close/log 콜백에 그대로 전달되는 사용자 컨텍스트 포인터. */
};
static bool g_log_timestamps = true;
/* [한국어] 모든 stderr 출력 라인 앞에 [YYYY-MM-DD HH:MM:SS.uuuuuu] 타임스탬프를 붙일지 여부.
 * 설정자: spdk_log_enable_timestamps(bool) — 사용자가 끄고 켤 수 있음.
 * 읽는 자: get_timestamp_prefix() (stderr 출력 직전).
 * 기본값 true — 진단 시 타임스탬프가 거의 항상 필요하기 때문. syslog는 자체 타임스탬프를
 * 붙이므로 이 플래그는 stderr 경로에만 영향. */

enum spdk_log_level g_spdk_log_level;
/* [한국어] syslog로 보낼 메시지의 심각도 컷오프 (★ 핵심 — 두 단계 필터의 첫 단계).
 * 의미: level <= g_spdk_log_level 이면 syslog로 전송. (enum이 ERROR=0 < WARN < ... < DEBUG
 *       순이므로 "<="가 "이 레벨 이상의 심각도"를 뜻함.)
 * 초기값: 0 (BSS) = SPDK_LOG_ERROR. 즉 기본은 ERROR만 syslog 송출.
 * 설정자: spdk_log_set_level() — RPC `log_set_level`/`--logfile-level` CLI에서 호출.
 * SPDK_LOG_DISABLED(=-1)로 두면 syslog로 아무것도 보내지 않음. */
enum spdk_log_level g_spdk_log_print_level;
/* [한국어] stderr로 출력할 메시지의 심각도 컷오프 (★ 핵심 — 두 단계 필터의 둘째 단계).
 * 의미: level <= g_spdk_log_print_level 이면 stderr로 출력.
 * g_spdk_log_level과 독립적이므로 "syslog는 모두, stderr는 NOTICE 이상" 같은 조합 가능.
 * 설정자: spdk_log_set_print_level() — RPC `log_set_print_level`/CLI 옵션에서 호출. */

/*
 * [한국어]
 * spdk_log_set_level - syslog 송출용 레벨 컷오프(g_spdk_log_level)를 설정한다.
 *
 * @level: 새 컷오프. SPDK_LOG_DISABLED(-1)부터 SPDK_LOG_DEBUG(4)까지.
 *         SPDK_LOG_DISABLED이면 syslog로 어떤 메시지도 보내지 않음.
 *         이외의 값이면 enum 값이 작거나 같은(=더 심각한) 메시지만 통과.
 * @return: 없음.
 *
 * 부트스트랩 시 또는 런타임 중 RPC `log_set_level`을 통해 호출되며, 이후 모든 spdk_vlog
 * 호출의 syslog 분기 필터가 즉시 바뀐다. 이 함수는 단순 대입이지만, 동시에 여러 reactor
 * 스레드에서 로그를 출력 중일 수 있으므로 race 시 잠시 옛 값으로 한두 줄이 더/덜 나갈 수
 * 있다 — SPDK는 이 정도의 누락을 허용한다(레벨 변경은 드문 작업이므로).
 *
 * 호출 체인:
 *   RPC `log_set_level` 핸들러 / app 부트스트랩 → spdk_log_set_level → (전역 대입)
 */
void
spdk_log_set_level(enum spdk_log_level level)
{
	assert(level >= SPDK_LOG_DISABLED);
	/* [한국어] 하한 검사 — DISABLED(-1)보다 작은 값은 정의되지 않은 enum이므로 사용 금지. */
	assert(level <= SPDK_LOG_DEBUG);
	/* [한국어] 상한 검사 — DEBUG(=4)가 가장 큰 enum 값. 그보다 큰 값은 spdk_level_names[]
	 * 인덱싱 시 OOB가 되므로 차단한다. */
	g_spdk_log_level = level;
	/* [한국어] 단순 대입. atomic이 아니지만 enum은 워드 크기라 x86/ARM에서 partial-word
	 * 쓰기는 발생하지 않음(읽기 측에서 옛 값/새 값 둘 중 하나만 보게 됨). */
}

/*
 * [한국어]
 * spdk_log_get_level - 현재 syslog 레벨 컷오프를 반환한다.
 *
 * @return: g_spdk_log_level (enum spdk_log_level 값).
 *
 * RPC `log_get_level` 핸들러나 진단 코드가 현재 설정을 조회할 때 사용.
 *
 * 호출 체인:
 *   RPC `log_get_level` / 사용자 코드 → spdk_log_get_level → (전역 read)
 */
enum spdk_log_level
spdk_log_get_level(void) {
	return g_spdk_log_level;
	/* [한국어] 전역 변수 단순 읽기 — atomic 보장은 set과 동일하게 워드 정렬 가정. */
}

/*
 * [한국어]
 * spdk_log_set_print_level - stderr 출력용 레벨 컷오프(g_spdk_log_print_level)를 설정한다.
 *
 * @level: SPDK_LOG_DISABLED ~ SPDK_LOG_DEBUG.
 * @return: 없음.
 *
 * syslog 컷오프와 독립이며, 보통 데몬은 stderr를 NOTICE 정도로 두고 syslog는 INFO/DEBUG로
 * 둬서 운영 중 화면을 깔끔하게 유지하는 식으로 활용한다. RPC `log_set_print_level`이
 * 이 함수를 호출.
 *
 * 호출 체인:
 *   RPC `log_set_print_level` / app 부트스트랩 → spdk_log_set_print_level → (전역 대입)
 */
void
spdk_log_set_print_level(enum spdk_log_level level)
{
	assert(level >= SPDK_LOG_DISABLED);
	/* [한국어] 하한 — DISABLED 미만은 비정의. */
	assert(level <= SPDK_LOG_DEBUG);
	/* [한국어] 상한 — DEBUG 초과 시 OOB. */
	g_spdk_log_print_level = level;
	/* [한국어] 즉시 반영. */
}

/*
 * [한국어]
 * spdk_log_get_print_level - 현재 stderr 레벨 컷오프를 반환한다.
 *
 * @return: g_spdk_log_print_level.
 *
 * 호출 체인:
 *   RPC `log_get_print_level` / 사용자 코드 → spdk_log_get_print_level
 */
enum spdk_log_level
spdk_log_get_print_level(void) {
	return g_spdk_log_print_level;
	/* [한국어] 전역 변수 단순 읽기. */
}

/*
 * [한국어]
 * log_open - 기본 싱크의 open 훅. syslog 연결을 연다.
 *
 * @ctx: 사용자 컨텍스트 — 기본 싱크에서는 사용하지 않음(인자 시그니처를 맞추기 위해 받음).
 * @return: 없음.
 *
 * spdk_log_open_ext(NULL)이 호출됐을 때 g_log_opts.open으로 등록되는 함수.
 * openlog(3)는 syslog 연결을 식별하는 ident 문자열과 옵션을 지정해 데몬 연결을 준비한다.
 * - "spdk"        : syslog 메시지 헤더에 박힐 프로그램 이름.
 * - LOG_PID       : 메시지에 PID를 포함시키는 옵션.
 * - LOG_LOCAL7    : facility — /etc/rsyslog.conf 등에서 분리 라우팅 가능 (관습적으로
 *                   유저 데몬이 LOCAL0~7을 사용).
 * 이 함수는 spdk_log_open() / open_ext() 내부에서 호출되며, 보통 앱 시작 시 1회 실행.
 *
 * 호출 체인:
 *   spdk_log_open_ext → g_log_opts.open(=log_open) → openlog (libc/syslog)
 */
static void
log_open(void *ctx)
{
	openlog("spdk", LOG_PID, LOG_LOCAL7);
	/* [한국어] syslog 데몬 연결 준비. 이후 syslog() 호출이 이 ident/facility로 라우팅됨.
	 * openlog는 옵션 — 호출 안 해도 syslog는 동작하지만, ident가 default(argv[0])로
	 * 잡혀 식별이 어렵기 때문에 명시적으로 부른다. */
}

/*
 * [한국어]
 * log_close - 기본 싱크의 close 훅. syslog 연결을 닫는다.
 *
 * @ctx: 미사용.
 * @return: 없음.
 *
 * spdk_log_close()가 호출되면 g_log_opts.close(=log_close)가 실행되며 closelog(3)을 통해
 * syslog 데몬과의 소켓을 끊는다. 앱 종료 직전에만 호출되는 것이 일반적.
 *
 * 호출 체인:
 *   spdk_app_stop / atexit → spdk_log_close → g_log_opts.close(=log_close) → closelog
 */
static void
log_close(void *ctx)
{
	closelog();
	/* [한국어] syslog 데몬과의 연결 해제. 멱등하므로 다시 호출해도 안전. */
}

/*
 * [한국어]
 * spdk_log_open - 단순 콜백 등록 진입점 (구버전 호환 API).
 *
 * @log: 사용자가 직접 처리할 로그 콜백. NULL이면 기본 syslog+stderr 모드.
 * @return: 없음.
 *
 * 후방 호환을 위한 얇은 래퍼 — 내부적으로 spdk_log_open_ext를 호출한다. 신규 코드는
 * spdk_log_open_ext(struct spdk_log_opts *) 를 직접 사용해 user_ctx 등 추가 필드를 전달
 * 하는 것이 권장된다. SPDK 앱 부트스트랩(예: spdk_app_start) 또는 사용자가 직접 호출.
 *
 * 호출 체인:
 *   사용자/spdk_app_start → spdk_log_open → spdk_log_open_ext → (sink 등록)
 */
void
spdk_log_open(spdk_log_cb *log)
{
	if (log) {
		/* [한국어] 사용자 콜백을 받았으면 임시 opts를 스택에 만들어 ext 버전에 위임. */
		struct spdk_log_opts opts = {.log = log};
		/* [한국어] 임시 opts 초기화 — log 필드만 채우고 나머지는 0(=NULL)로. */
		opts.size = SPDK_SIZEOF(&opts, log);
		/* [한국어] ABI 호환을 위한 size 마킹 — "log 필드까지만 인지" 라는 뜻. ext 버전이
		 * SPDK_GET_FIELD로 size 넘는 필드는 default(NULL)로 처리해 버전 차이를 흡수. */
		spdk_log_open_ext(&opts);
		/* [한국어] 실제 등록은 ext 함수가 담당. */
	} else {
		spdk_log_open_ext(NULL);
		/* [한국어] NULL 전달 → ext가 기본 syslog 싱크로 폴백. */
	}
}

/*
 * [한국어]
 * spdk_log_open_ext - 싱크 등록 진입점 (확장 API, ★ 핵심 — sink 추상화 설치).
 *
 * @opts: 싱크 콜백 묶음.
 *        NULL → 기본 모드: log_open/log_close가 자동 등록되어 syslog가 열림.
 *                          .log는 NULL로 남아 spdk_vlog가 stderr+syslog 직접 출력 경로 사용.
 *        not-NULL → 사용자 정의 모드: opts->log 콜백이 모든 메시지를 받음. SPDK는 더 이상
 *                                    stderr/syslog로 직접 쓰지 않고 콜백에 위임만 함.
 * @return: 없음.
 *
 * 동작:
 *   1) opts가 NULL이면 기본 syslog 싱크의 open/close만 g_log_opts에 박고 .log는 NULL 유지.
 *   2) opts가 있으면 SPDK_GET_FIELD로 ABI-safe하게 각 필드를 복사 (호출자가 인지하지 못한
 *      미래 필드는 자동으로 NULL 처리).
 *   3) 마지막에 등록된 .open 훅이 있으면 호출해 싱크를 활성화.
 *
 * 실행 컨텍스트: 앱 메인 스레드(부트스트랩). 다중 reactor가 돌고 있는 상태에서 호출하면
 * race가 생길 수 있으므로 보통 spdk_app_start 초입에서 1회만 호출.
 *
 * 호출 체인:
 *   spdk_app_start / 사용자 → spdk_log_open_ext → [sink open] → (이후 spdk_vlog가 사용)
 */
void
spdk_log_open_ext(struct spdk_log_opts *opts)
{
	if (!opts) {
		/* [한국어] 기본 모드 — 사용자가 명시 의사를 안 줬으므로 syslog 기반 기본값 적용. */
		g_log_opts.open = log_open;
		/* [한국어] openlog()를 호출하는 기본 open 훅 등록. */
		g_log_opts.close = log_close;
		/* [한국어] closelog()를 호출하는 기본 close 훅 등록. */
		goto out;
		/* [한국어] 아래 공통 처리(open 훅 호출)로 점프 — log/user_ctx는 NULL로 남는다. */
	}

	g_log_opts.log = SPDK_GET_FIELD(opts, log, NULL);
	/* [한국어] 사용자 콜백 — opts->size가 .log 필드를 포함할 정도면 그 값, 아니면 NULL.
	 * NULL로 남으면 spdk_vlog는 기본 stderr+syslog 경로를 사용. */
	g_log_opts.open = SPDK_GET_FIELD(opts, open, NULL);
	/* [한국어] open 훅 — 사용자가 자체 싱크 초기화(파일 open 등)를 원하면 채움. */
	g_log_opts.close = SPDK_GET_FIELD(opts, close, NULL);
	/* [한국어] close 훅 — 짝이 되는 종료 처리. */
	g_log_opts.user_ctx = SPDK_GET_FIELD(opts, user_ctx, NULL);
	/* [한국어] open/close/log 콜백 호출 시 첫 인자로 그대로 전달되는 사용자 컨텍스트. */

out:
	if (g_log_opts.open) {
		/* [한국어] open 훅이 등록되어 있으면 즉시 호출 — 싱크 자체를 활성화하는 단계. */
		g_log_opts.open(g_log_opts.user_ctx);
		/* [한국어] 사용자 ctx와 함께 호출 — 기본 모드에서는 user_ctx=NULL. */
	}
}

/*
 * [한국어]
 * spdk_log_close - 등록된 싱크를 닫고 g_log_opts를 초기 상태로 되돌린다.
 *
 * @return: 없음.
 *
 * close 훅(있다면)을 호출해 싱크를 정리하고, g_log_opts 전체를 0으로 클리어한다.
 * 이후 spdk_vlog 호출은 다시 .log == NULL 경로(stderr+syslog 직접 출력)로 폴백 — 단,
 * syslog 연결은 이미 끊긴 상태이므로 closelog 후 syslog() 호출은 자동 재연결됨(libc 동작).
 * 일반적으로 spdk_app_stop 후 또는 프로그램 종료 직전에만 호출.
 *
 * 호출 체인:
 *   spdk_app_stop / atexit → spdk_log_close → g_log_opts.close → memset(g_log_opts)
 */
void
spdk_log_close(void)
{
	if (g_log_opts.close) {
		/* [한국어] 등록된 close 훅 호출 — 기본 모드면 closelog(), 사용자 모드면 사용자 정의. */
		g_log_opts.close(g_log_opts.user_ctx);
	}
	memset(&g_log_opts, 0, sizeof(g_log_opts));
	/* [한국어] 싱크 설정 전체를 0으로 — 추후 spdk_log 호출이 와도 .log/.open/.close 모두
	 * NULL이라 안전한 폴백 동작(직접 출력)으로 떨어진다. */
}

/*
 * [한국어]
 * spdk_log_enable_timestamps - stderr 출력 라인의 타임스탬프 prefix를 켜고 끈다.
 *
 * @value: true면 [YYYY-MM-DD HH:MM:SS.uuuuuu] prefix 부착, false면 없음.
 * @return: 없음.
 *
 * 테스트 환경에서 출력을 diff하기 쉽게 하기 위해 끄거나, 운영에서 켜는 식으로 사용한다.
 * syslog 경로는 영향 받지 않음 — syslog 데몬이 자체 타임스탬프를 붙이기 때문.
 *
 * 호출 체인:
 *   사용자 → spdk_log_enable_timestamps → (전역 대입) → 이후 get_timestamp_prefix가 참조
 */
void
spdk_log_enable_timestamps(bool value)
{
	g_log_timestamps = value;
	/* [한국어] 단순 대입 — 다음 spdk_vlog 호출부터 즉시 반영. */
}

/*
 * [한국어]
 * get_timestamp_prefix - "[YYYY-MM-DD HH:MM:SS.uuuuuu] " 형식의 타임스탬프 문자열을 생성.
 *
 * @buf: 결과를 채울 호출자 버퍼.
 * @buf_size: 버퍼 크기 (보통 64). 형식상 30바이트 정도면 충분.
 * @return: 없음. 결과는 buf에 NUL-terminated 문자열로 저장. 비활성 시 빈 문자열.
 *
 * spdk_vlog/spdk_vflog가 stderr 출력 직전에 호출. CLOCK_REALTIME으로 wall clock을 받고,
 * localtime으로 분해, strftime으로 날짜 포맷, snprintf로 마이크로초 추가.
 * 주의: localtime은 thread-unsafe 정적 버퍼를 사용하지만 SPDK는 동시 로그 스레드 race를
 *       허용(누락이 아닌 잠깐의 스크램블) — 정확성보다 핫패스 코스트 절감을 택함.
 *
 * 호출 체인:
 *   spdk_vlog / spdk_vflog → get_timestamp_prefix → clock_gettime/localtime/strftime
 */
static void
get_timestamp_prefix(char *buf, int buf_size)
{
	struct tm *info;
	/* [한국어] localtime 결과를 받을 분해된 시간 구조체 포인터(정적 버퍼 가리킴). */
	char date[24];
	/* [한국어] strftime으로 만든 "YYYY-MM-DD HH:MM:SS" (19자) + NUL. 24바이트면 충분. */
	struct timespec ts;
	/* [한국어] clock_gettime이 채울 초+나노초. */
	long usec;
	/* [한국어] tv_nsec(0~999999999)를 1000으로 나눈 마이크로초(0~999999). */

	if (!g_log_timestamps) {
		/* [한국어] 사용자가 타임스탬프를 껐다면 빈 prefix만 반환. */
		buf[0] = '\0';
		return;
	}

	clock_gettime(CLOCK_REALTIME, &ts);
	/* [한국어] CLOCK_REALTIME은 epoch 기준 wall clock(NTP 영향 받음). 사람이 읽기 좋은
	 * 타임스탬프이므로 MONOTONIC이 아닌 REALTIME 사용. ns 정밀도. */
	info = localtime(&ts.tv_sec);
	/* [한국어] 초 단위를 분해 시간(struct tm)으로 — 시스템 타임존 적용. localtime은
	 * 정적 버퍼를 반환하므로 멀티 스레드 호출 시 잠깐 인터리빙 가능 (SPDK는 허용). */
	usec = ts.tv_nsec / 1000;
	/* [한국어] 나노초 → 마이크로초로 단축. 6자리로 표현하기 위해 1000으로 나눔. */
	if (info == NULL) {
		/* [한국어] localtime 실패 — 매우 드문 케이스(타임존 데이터 깨짐 등)지만 안전 처리. */
		snprintf(buf, buf_size, "[%s.%06ld] ", "unknown date", usec);
		/* [한국어] 날짜 부분 대신 placeholder 박고 마이크로초만이라도 출력. */
		return;
	}

	strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", info);
	/* [한국어] "%Y-%m-%d %H:%M:%S" — ISO 8601 비슷한 사람-가독 형식. 정확히 19자. */
	snprintf(buf, buf_size, "[%s.%06ld] ", date, usec);
	/* [한국어] 최종 prefix — "[2024-05-21 13:45:09.123456] " (29자). %06ld로 6자리 zero-pad. */
}

/*
 * [한국어]
 * spdk_log - 가변 인자(printf-style)를 받아 spdk_vlog로 위임하는 표면 진입점.
 *
 * @level: 메시지 심각도 (SPDK_LOG_ERROR ~ DEBUG).
 * @file:  __FILE__ — 호출 위치 파일명.
 * @line:  __LINE__ — 호출 위치 라인 번호.
 * @func:  __func__ — 호출 함수 이름.
 * @format: printf 포맷 문자열.
 * @...:    포맷 인자.
 * @return: 없음.
 *
 * 거의 모든 SPDK_*LOG 매크로(SPDK_ERRLOG, SPDK_WARNLOG, ...)는 매크로 전개 시 이 함수를
 * 호출한다. va_list로 패킹해 spdk_vlog로 위임 — 핵심 로직은 vlog 쪽에 있다.
 *
 * 호출 체인:
 *   SPDK_ERRLOG / NOTICELOG / 등 매크로 → spdk_log → spdk_vlog
 */
void
spdk_log(enum spdk_log_level level, const char *file, const int line, const char *func,
	 const char *format, ...)
{
	va_list ap;
	/* [한국어] 가변 인자 트래버서 — va_start/va_end로 페어링해야 함. */

	va_start(ap, format);
	/* [한국어] format 다음부터의 가변 인자 시작 위치를 ap에 세팅. */
	spdk_vlog(level, file, line, func, format, ap);
	/* [한국어] 실제 포맷팅·필터링·싱크 분기는 vlog가 수행. */
	va_end(ap);
	/* [한국어] 가변 인자 정리 — 일부 ABI에서 필수(레지스터/스택 클린업). */
}

/*
 * [한국어]
 * spdk_log_to_syslog_level - SPDK 자체 enum을 syslog(3)의 priority 상수로 매핑.
 *
 * @level: SPDK_LOG_DEBUG/INFO/NOTICE/WARN/ERROR/DISABLED.
 * @return: syslog(3) priority — LOG_INFO/NOTICE/WARNING/ERR. DISABLED면 -1.
 *
 * SPDK 레벨이 5단계인 데 비해 syslog는 더 세분화되어 있어, DEBUG/INFO를 모두 LOG_INFO로
 * 합치는 식으로 매핑한다(또는 DEBUG가 너무 많이 붙는 시스템 로그 부담을 줄이기 위함).
 * spdk_vlog가 syslog 호출 전에 이 함수로 priority를 결정. -1 반환은 호출 측에서 "아예
 * 출력 안 함"으로 해석.
 *
 * 호출 체인:
 *   spdk_vlog → spdk_log_to_syslog_level → (return priority)
 */
int
spdk_log_to_syslog_level(enum spdk_log_level level)
{
	switch (level) {
	case SPDK_LOG_DEBUG:
	case SPDK_LOG_INFO:
		/* [한국어] 두 단계를 LOG_INFO로 통합 — syslog의 LOG_DEBUG는 너무 노이지하기에 회피. */
		return LOG_INFO;
	case SPDK_LOG_NOTICE:
		return LOG_NOTICE;
		/* [한국어] 정상 동작 중 알림 — syslog의 LOG_NOTICE에 직접 매핑. */
	case SPDK_LOG_WARN:
		return LOG_WARNING;
		/* [한국어] 경고 — syslog의 LOG_WARNING에 매핑. */
	case SPDK_LOG_ERROR:
		return LOG_ERR;
		/* [한국어] 오류 — syslog의 LOG_ERR에 매핑. */
	case SPDK_LOG_DISABLED:
		return -1;
		/* [한국어] 음수 반환 — 호출자(spdk_vlog)가 이 값을 보고 즉시 return하여 출력 차단. */
	default:
		break;
		/* [한국어] 미지의 enum이면 default fall-through로 안전한 LOG_INFO를 반환. */
	}

	return LOG_INFO;
	/* [한국어] 안전한 폴백. */
}

/*
 * [한국어]
 * spdk_vlog - SPDK 로그 출력의 핵심 본체 (★ sink 추상화 + 두 단계 레벨 필터 + 동적 확장).
 *
 * @level:  메시지 심각도.
 * @file:   호출 위치 파일명 (NULL 가능 — 그러면 헤더 없이 본문만 출력).
 * @line:   호출 위치 라인 번호.
 * @func:   호출 함수 이름.
 * @format: printf 포맷 문자열.
 * @ap:     va_list — 호출자가 va_start로 준비한 가변 인자 포인터.
 * @return: 없음.
 *
 * 동작 단계:
 *   1) g_log_opts.log 콜백이 등록되어 있으면 그쪽으로 위임하고 즉시 종료
 *      → 사용자 정의 싱크가 모든 처리를 가로챔.
 *   2) 기본 모드: print_level과 syslog_level 둘 다 컷오프 미통과면 즉시 종료.
 *   3) syslog priority를 결정. DISABLED(-1) 반환이면 즉시 종료(이중 안전망).
 *   4) MAX_TMPBUF(1KB) 스택 버퍼에 vsnprintf로 메시지를 빌드.
 *   5) 결과가 1KB를 넘으면 vasprintf로 동적 할당해 잘림 회피 (ap_copy 사용 — vsnprintf
 *      가 첫 ap를 소진했기 때문에 ap_copy가 필요).
 *   6) print_level 컷 통과면 stderr로, syslog_level 컷 통과면 syslog로 각각 출력.
 *   7) 동적 할당된 ext_buf가 있으면 free.
 *
 * 실행 컨텍스트: 어떤 reactor/lcore에서든 호출 가능. 여러 lcore가 동시에 호출해도 fprintf,
 * syslog는 thread-safe(POSIX 보장)이며, 본 함수는 전역 g_log_opts/level을 read-only로만
 * 다루므로 데이터 race는 없다. 단, localtime/strftime 내부 race는 허용(§get_timestamp_prefix).
 *
 * 호출 체인:
 *   spdk_log → spdk_vlog → [vsnprintf|vasprintf] → fprintf(stderr) + syslog()
 *                                              또는 g_log_opts.log(사용자 콜백)
 */
void
spdk_vlog(enum spdk_log_level level, const char *file, const int line, const char *func,
	  const char *format, va_list ap)
{
	int severity = LOG_INFO;
	/* [한국어] syslog priority 임시 저장. 매핑 실패 대비 안전 기본값으로 LOG_INFO 초기화. */
	char *buf, _buf[MAX_TMPBUF], *ext_buf = NULL;
	/* [한국어] buf      — 실제 사용할 메시지 버퍼 포인터 (스택 또는 heap을 가리킴).
	 *         _buf[]   — 1KB 스택 버퍼(소형 메시지 핫패스용 — 대부분 여기서 처리).
	 *         ext_buf  — 1KB 초과 시 vasprintf가 할당하는 heap 버퍼(필요 시에만 사용 후 free). */
	char timestamp[64];
	/* [한국어] "[YYYY-MM-DD HH:MM:SS.uuuuuu] " 타임스탬프 prefix — 30자 미만이지만 여유. */
	va_list ap_copy;
	/* [한국어] 가변 인자 백업. vsnprintf가 ap를 소진하므로, vasprintf 재시도용 사본 필요. */
	int rc;
	/* [한국어] vsnprintf/vasprintf 반환값 — 출력하려 했던 길이(또는 -1). */

	if (g_log_opts.log) {
		/* [한국어] ★ sink 추상화의 핵심 분기 — 사용자 콜백이 등록되어 있으면 모든 책임을
		 * 콜백에 떠넘기고 SPDK는 손을 뗀다. 레벨 필터링도, 포맷팅도, stderr/syslog 호출도
		 * 모두 사용자 책임 — SPDK는 raw va_list까지 그대로 넘긴다. */
		g_log_opts.log(level, file, line, func, format, ap);
		return;
	}

	if (level > g_spdk_log_print_level && level > g_spdk_log_level) {
		/* [한국어] ★ 두 단계 레벨 필터의 OR 컷오프. enum은 ERROR=0 < ... < DEBUG=4이므로
		 * "level > 컷오프"는 "더 덜 심각" → 출력 대상 아님. 둘 다 미통과면 어디로도 안 보냄. */
		return;
	}

	severity = spdk_log_to_syslog_level(level);
	/* [한국어] SPDK enum → syslog priority 매핑. DISABLED는 -1로 옴. */
	if (severity < 0) {
		/* [한국어] -1이면 출력 차단(레벨 자체가 DISABLED) — 위 OR 컷오프와는 별개의 안전망. */
		return;
	}

	buf = _buf;
	/* [한국어] 우선 스택 버퍼를 가리키도록. 길이가 충분하면 그대로 사용. */

	va_copy(ap_copy, ap);
	/* [한국어] vsnprintf가 ap를 소진할 수 있으므로, vasprintf 재시도를 위해 백업.
	 * va_copy는 매크로지만 일부 ABI에서 실제 메모리 복사가 필요. */
	rc = vsnprintf(_buf, sizeof(_buf), format, ap);
	/* [한국어] format을 _buf(스택)에 렌더 — 잘리는 경우 rc는 "잘리지 않았다면 필요했을 길이".
	 * NUL 종결 보장. ap는 이 호출에서 소진되어 더 이상 재사용 불가(그래서 ap_copy 필요). */
	if (rc > MAX_TMPBUF) {
		/* The output including the terminating was more than MAX_TMPBUF bytes.
		 * Try allocating memory large enough to hold the output.
		 */
		/* [한국어] 메시지가 1KB를 초과 → 잘림 발생. 잘리지 않은 전체 메시지를 보고 싶으므로
		 * heap에 동적으로 충분한 공간을 vasprintf로 잡아 다시 렌더. */
		rc = vasprintf(&ext_buf, format, ap_copy);
		/* [한국어] vasprintf는 충분한 길이로 malloc 후 렌더, 길이 반환. ext_buf는 호출자
		 * 책임으로 free해야 함(아래에서 free). */
		if (rc < 0) {
			/* Failed to allocate memory. Allow output to be truncated. */
			/* [한국어] OOM 등으로 실패 → 그냥 잘린 _buf를 사용. 부분 메시지라도 출력 우선. */
		} else {
			buf = ext_buf;
			/* [한국어] 성공 → buf 포인터를 heap 버퍼로 교체. 이후 출력은 잘리지 않은 전체 메시지. */
		}
	}
	va_end(ap_copy);
	/* [한국어] ap_copy 정리 — 일부 ABI에서 필수. */

	if (level <= g_spdk_log_print_level) {
		/* [한국어] stderr 출력 컷 통과 → 사람이 보는 콘솔에 출력. */
		get_timestamp_prefix(timestamp, sizeof(timestamp));
		/* [한국어] 라인 앞에 붙일 타임스탬프 생성 (g_log_timestamps=false면 빈 문자열). */
		if (file) {
			/* [한국어] 호출 위치 정보가 있으면 헤더 형식 — "[ts] file: lin:func: *LEVEL*: msg".
			 * %4d로 라인 번호 4자리 우측 정렬 → 가독성. */
			fprintf(stderr, "%s%s:%4d:%s: *%s*: %s", timestamp, file, line, func, spdk_level_names[level], buf);
		} else {
			/* [한국어] 호출 위치 미상이면(예: 사용자가 spdk_log를 직접 부름) 본문만. */
			fprintf(stderr, "%s%s", timestamp, buf);
		}
	}

	if (level <= g_spdk_log_level) {
		/* [한국어] syslog 출력 컷 통과 → 시스템 로그에 영구 기록. syslog는 자체 타임스탬프를
		 * 붙이므로 timestamp prefix 불필요. */
		if (file) {
			syslog(severity, "%s:%4d:%s: *%s*: %s", file, line, func, spdk_level_names[level], buf);
			/* [한국어] syslog(3) — RFC 3164/5424 형식으로 syslogd로 송출. severity는
			 * spdk_log_to_syslog_level의 결과. ident/facility는 openlog에서 설정한 값. */
		} else {
			syslog(severity, "%s", buf);
			/* [한국어] 위치 정보 없으면 본문만. */
		}
	}

	free(ext_buf);
	/* [한국어] vasprintf로 할당된 heap 버퍼 해제 — NULL이면 free는 no-op이므로 안전. */
}

/*
 * [한국어]
 * spdk_vflog - 사용자가 지정한 FILE*에 직접 한 줄 로그를 출력 (싱크/필터 우회).
 *
 * @fp:     출력할 FILE 포인터 (예: stdout, 사용자 fopen 결과). NULL 금지.
 * @file:   호출 위치 파일명 (NULL이면 헤더 생략).
 * @line:   라인 번호.
 * @func:   함수명.
 * @format: printf 포맷.
 * @ap:     가변 인자 리스트.
 * @return: 없음.
 *
 * spdk_vlog와 달리 g_log_opts/level 필터를 거치지 않는다 — 항상 출력. 진단 도구나
 * RPC 응답을 stdout으로 보낼 때, 또는 스크립트에서 결과를 파일에 직접 쓸 때 사용.
 * 메시지가 1KB를 넘으면 잘림(spdk_vlog와 달리 동적 확장 안 함).
 *
 * 호출 체인:
 *   spdk_flog → spdk_vflog → vsnprintf + fprintf(fp) + fflush
 */
void
spdk_vflog(FILE *fp, const char *file, const int line, const char *func,
	   const char *format, va_list ap)
{
	char buf[MAX_TMPBUF];
	/* [한국어] 1KB 스택 버퍼 — 단순화를 위해 동적 확장 없음, 초과분은 잘림. */
	char timestamp[64];
	/* [한국어] "[ts] " prefix 버퍼. */

	vsnprintf(buf, sizeof(buf), format, ap);
	/* [한국어] format + ap → buf 렌더 (NUL 종결 보장, 초과분은 잘림). */

	get_timestamp_prefix(timestamp, sizeof(timestamp));
	/* [한국어] 타임스탬프 prefix 생성 (g_log_timestamps=false면 빈 문자열). */

	if (file) {
		/* [한국어] 위치 정보 있으면 "[ts] file:line:func: msg" 헤더 형식. */
		fprintf(fp, "%s%s:%4d:%s: %s", timestamp, file, line, func, buf);
	} else {
		fprintf(fp, "%s%s", timestamp, buf);
		/* [한국어] 헤더 없으면 본문만. */
	}

	fflush(fp);
	/* [한국어] 즉시 플러시 — 스크립트가 다음 줄을 기다리는 케이스 대응. fp가 라인 버퍼링이
	 * 아닐 수 있으므로 명시적으로 비움. */
}

/*
 * [한국어]
 * spdk_flog - 가변 인자 래퍼. spdk_vflog로 위임.
 *
 * @fp:     출력 FILE*.
 * @file:   __FILE__.
 * @line:   __LINE__.
 * @func:   __func__.
 * @format: printf 포맷.
 * @...:    포맷 인자.
 * @return: 없음.
 *
 * SPDK_FLOG 매크로(가독성 좋은 사용자 직접 출력)에서 사용.
 *
 * 호출 체인:
 *   사용자 / SPDK_FLOG → spdk_flog → spdk_vflog
 */
void
spdk_flog(FILE *fp, const char *file, const int line, const char *func,
	  const char *format, ...)
{
	va_list ap;
	/* [한국어] 가변 인자 트래버서. */

	va_start(ap, format);
	/* [한국어] format 다음부터 ap 시작점 설정. */
	spdk_vflog(fp, file, line, func, format, ap);
	/* [한국어] 실제 포맷팅·출력은 vflog가 수행. */
	va_end(ap);
	/* [한국어] ap 정리. */
}

/*
 * [한국어]
 * fdump - 바이트 버퍼를 16바이트 폭의 hex+ASCII 형태로 dump (NVMe payload 디버깅 표준 양식).
 *
 * @fp:    출력 FILE*.
 * @label: 첫 줄에 출력할 라벨 (예: "Admin Cmd:" 등).
 * @buf:   덤프할 바이트 버퍼.
 * @len:   바이트 길이.
 * @return: 없음.
 *
 * 출력 형식 (hexdump -C 와 유사):
 *   00000000  53 50 44 4b 20 4c 6f 67  20 44 75 6d 70 0a 00 00  SPDK Log Dump...
 *   |←offset│ |←──── 8 hex pairs ───│ |←─── 8 hex pairs ─────│  |← ASCII ─────|
 *
 * SPDK_LOGDUMP 매크로 또는 NVMe queue/qpair 진단 코드가 호출. tmpbuf(1KB)에 한 줄을
 * 누적해 fprintf로 한 번에 쏟아내 출력 인터리빙 확률을 줄인다.
 *
 * 호출 체인:
 *   spdk_log_dump → fdump → fprintf(fp) (16바이트당 1줄)
 */
static void
fdump(FILE *fp, const char *label, const uint8_t *buf, size_t len)
{
	char tmpbuf[MAX_TMPBUF];
	/* [한국어] 한 줄 누적 버퍼 — offset(10) + hex(50) + ascii(18) ≈ 80자, 여유 있게 1KB. */
	char buf16[16 + 1];
	/* [한국어] 16바이트 분량의 ASCII 표현 — 비출력문자는 '.'으로 치환. NUL 1자 여유. */
	size_t total;
	/* [한국어] tmpbuf 내 현재 누적 길이 — snprintf의 다음 시작 offset. */
	unsigned int idx;
	/* [한국어] 입력 버퍼 내 현재 바이트 인덱스. */

	fprintf(fp, "%s\n", label);
	/* [한국어] 첫 줄에 사용자 라벨 출력 후 개행. */

	memset(buf16, 0, sizeof buf16);
	/* [한국어] ASCII 사이드바 초기화 — 매 16바이트 그룹 시작 시 다시 0. */
	total = 0;
	/* [한국어] 누적 길이 0부터 시작. */
	for (idx = 0; idx < len; idx++) {
		if (idx != 0 && idx % 16 == 0) {
			/* [한국어] 16바이트 경계 도달 — 누적 라인을 출력하고 다음 줄 준비. */
			snprintf(tmpbuf + total, sizeof tmpbuf - total,
				 " %s", buf16);
			/* [한국어] 라인 끝에 ASCII 사이드바 추가 (" ABCD..."). */
			memset(buf16, 0, sizeof buf16);
			/* [한국어] ASCII 버퍼 초기화 — 다음 16바이트용. */
			fprintf(fp, "%s\n", tmpbuf);
			/* [한국어] 한 줄 출력 — 이게 hexdump의 한 줄. */
			total = 0;
			/* [한국어] tmpbuf 누적 0으로 리셋. */
		}
		if (idx % 16 == 0) {
			/* [한국어] 16바이트 그룹 시작 — offset 출력 (8자리 hex + space). */
			total += snprintf(tmpbuf + total, sizeof tmpbuf - total,
					  "%08x ", idx);
		}
		if (idx % 8 == 0) {
			/* [한국어] 8바이트 경계 — hex 영역 사이에 추가 공백 (가독성 — hexdump -C 양식). */
			total += snprintf(tmpbuf + total, sizeof tmpbuf - total,
					  "%s", " ");
		}
		total += snprintf(tmpbuf + total, sizeof tmpbuf - total,
				  "%2.2x ", buf[idx] & 0xff);
		/* [한국어] 한 바이트를 2자리 hex로 출력 + 공백. & 0xff는 char가 signed인 환경에서
		 * 음수 확장을 방지(%2.2x가 unsigned로 받지만 경고 회피). */
		buf16[idx % 16] = isprint(buf[idx]) ? buf[idx] : '.';
		/* [한국어] ASCII 사이드바 채우기 — 출력 가능 문자만 그대로, 나머지는 '.'로 치환.
		 * isprint는 ctype.h — 알파벳/숫자/구두점/공백 등을 true로 판정. */
	}
	for (; idx % 16 != 0; idx++) {
		/* [한국어] 마지막 줄 패딩 — 16바이트 경계가 아니면 빈 hex 칸으로 채워서 ASCII가
		 * 일직선으로 정렬되게 함. */
		if (idx == 8) {
			/* [한국어] 끝 패딩 중에도 8바이트 경계의 추가 공백 유지. */
			total += snprintf(tmpbuf + total, sizeof tmpbuf - total,
					  " ");
		}

		total += snprintf(tmpbuf + total, sizeof tmpbuf - total, "   ");
		/* [한국어] hex 한 칸 분(2자리+공백=3자) 만큼 빈 공간을 채워 다음 줄 ASCII 정렬 유지. */
	}
	snprintf(tmpbuf + total, sizeof tmpbuf - total, "  %s", buf16);
	/* [한국어] 마지막 줄 ASCII 사이드바 부착. */
	fprintf(fp, "%s\n", tmpbuf);
	/* [한국어] 마지막 줄 출력. */
	fflush(fp);
	/* [한국어] dump는 디버깅 즉시성이 중요하므로 강제 플러시. */
}

/*
 * [한국어]
 * spdk_log_dump - fdump의 공개 래퍼.
 *
 * @fp:    출력 FILE*.
 * @label: 첫 줄 라벨.
 * @buf:   덤프할 버퍼 (void* — 호출자가 어떤 타입이든 캐스팅 없이 넘길 수 있게).
 * @len:   바이트 길이.
 * @return: 없음.
 *
 * SPDK_LOGDUMP 매크로(컴포넌트 디버그 플래그가 켜졌을 때만 활성화)가 이 함수를 호출.
 * NVMe submission/completion queue payload를 사람이 읽을 수 있는 형태로 들여다볼 때 유용.
 *
 * 호출 체인:
 *   SPDK_LOGDUMP / 사용자 → spdk_log_dump → fdump
 */
void
spdk_log_dump(FILE *fp, const char *label, const void *buf, size_t len)
{
	fdump(fp, label, buf, len);
	/* [한국어] void* → uint8_t*로 암묵 캐스팅하며 내부 fdump에 위임. */
}
