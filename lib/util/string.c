/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK 문자열 유틸리티 구현 (string.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 전반에서 광범위하게 사용되는 "안전하고 편리한 문자열 처리 함수"의 구현체이다.
 * 표준 C 라이브러리의 strtol/strerror_r/sprintf 등은 플랫폼/플래그(_GNU_SOURCE 등)에 따라 동작이 달라지거나
 * 잘못 사용 시 매우 미묘한 버그(반환값 + errno 동시 검사 누락 등)를 유발하므로, SPDK 는 이를 감싸는
 * 자체 wrapper 를 제공한다. 또한 vasprintf 스타일 동적 할당, RPC 파서를 위한 quote-aware split,
 * config 파싱을 위한 단위 접미사(K/M/G) 인식, IPv4/IPv6 주소 파서, 패딩 문자열 처리 등
 * SPDK 가 RPC/JSON/config/log 처리에서 반복적으로 필요로 하는 도구함을 한 파일에 모아둔다.
 * 본 파일은 stateless 한 라이브러리 함수만을 제공하며 전역 상태나 락을 갖지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/util/ 디렉토리는 SPDK 의 최하위 유틸리티 계층으로, 모든 상위 모듈(lib/nvme, lib/bdev,
 * lib/nvmf, lib/thread, lib/rpc, lib/json 등)이 의존한다. 호출 체인은 일반적으로
 *   상위 모듈(예: lib/rpc/rpc.c, lib/nvmf/subsystem.c, app/spdk_tgt) → 본 파일의 함수 → libc/glibc
 * 와 같이 흐른다. 실행 컨텍스트는 호스트 유저스페이스(SPDK reactor 스레드, RPC 서버 스레드, 초기화 스레드 등)
 * 이며, polled-mode I/O 핫패스에서도 호출되지만 함수 자체는 syscall 을 부르지 않으므로(예외: snprintf 내부)
 * non-blocking 보장에 영향을 주지 않는다. spdk_strerror 류는 lib/util/strerror_tls.c 의 TLS(Thread-Local
 * Storage) 버퍼와 협력하여 reentrancy 를 보장한다.
 *
 * === 타 모듈과의 연결 ===
 * - 헤더: include/spdk/string.h (이미 한국어 주석 완료) 가 이 파일이 노출하는 모든 함수의 prototype 을 선언한다.
 * - libc 의존: stdlib(strtol/strtoll/realloc/calloc/free/strdup/strndup), string(strlen/strchr/strstr/strpbrk/
 *   memcpy/memset), stdio(vsnprintf/snprintf/sscanf), ctype(tolower/isspace), errno 가 주된 의존 대상.
 * - lib/util/strerror_tls.c: TLS 버퍼 기반 spdk_strerror 와 짝을 이룬다(본 파일은 spdk_strerror_r 만 정의).
 * - 호출자: 거의 모든 SPDK 모듈이 잠재 호출자다. 대표적으로
 *     · lib/rpc/* — JSON-RPC 파라미터 파싱, hex/digit 변환, quote-aware split
 *     · lib/nvmf/* — NQN 검증, 주소 파싱(spdk_parse_ip_addr), config 파싱
 *     · lib/bdev/* — bdev 이름 비교, 에러 메시지 빌드, capacity 단위 파싱
 *     · lib/log/log.c — log 메시지 포맷팅(spdk_strerror_r 로 errno 변환)
 *     · lib/env_dpdk/init.c — config 단위 접미사 파싱(spdk_parse_capacity)
 * - 데이터 흐름: 입력은 호출자가 제공한 char * (보통 RPC/Config/Env 변수에서 옴)이며, 결과는
 *   (a) 새로 malloc 된 버퍼(반환값으로 전달, 호출자가 free 책임), (b) in-place 수정(spdk_strlwr/trim/chomp),
 *   (c) 호출자 제공 버퍼에 기록(spdk_strerror_r/spdk_strcpy_pad), (d) 정수/uint64 출력 인자(parse_*) 의
 *   네 가지 패턴 중 하나로 흐른다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_vsprintf_alloc / spdk_sprintf_alloc — vasprintf 가 없는 환경에서도 동작하는 동적 sprintf.
 *   결과 버퍼는 malloc 된 것으로 호출자가 free 해야 한다. 실패 시 NULL.
 * - spdk_vsprintf_append_realloc / spdk_sprintf_append_realloc — 기존 버퍼 끝에 포맷 문자열 append.
 *   realloc 실패 시 기존 버퍼는 그대로 유지(realloc 의 표준 동작) 하고 NULL 반환 — 호출자는
 *   원본 포인터를 잃지 않도록 임시 변수 패턴을 사용해야 한다.
 * - spdk_strlwr — ASCII 소문자 변환(in-place). NQN/UUID 정규화 등에 사용.
 * - spdk_strsepq — strsep 의 quote/backslash escape 인식 버전. RPC 파라미터 파싱의 토큰화에 사용.
 * - spdk_str_trim / spdk_str_chomp — 공백/개행 제거. config 파싱과 줄 단위 입력 정리.
 * - spdk_strcpy_pad / spdk_strlen_pad — 고정 길이 + padding 문자열 처리. NVMe identify controller 의
 *   serial number/model number 같은 공백 패딩(0x20) 필드에 사용.
 * - spdk_parse_ip_addr — IPv4/IPv6 [host]:port 분리. NVMe-oF transport listen address 파싱에 사용.
 * - spdk_strerror_r — GNU 변종/POSIX 변종을 모두 처리하는 thread-safe strerror.
 * - spdk_parse_capacity — "1024K" / "16M" 같은 단위 접미사 인식 capacity 파서.
 * - spdk_mem_all_zero — 메모리 영역이 전부 0 인지 검사(zero-block 최적화 판단 등).
 * - spdk_strtol / spdk_strtoll — strtol/strtoll 의 errno + 범위 검증 안전 버전. 음수도 거부.
 * - spdk_strarray_from_string / spdk_strarray_dup / spdk_strarray_free — 문자열 → 배열 split,
 *   복제, 해제. RPC 의 multi-value 파라미터에 사용.
 * - spdk_strcpy_replace — 부분 문자열 일괄 치환(검색·치환 길이 다를 수 있음).
 *
 * 본 파일에는 자체 정의 구조체나 enum, 매크로가 없다. 전역 변수도 없으며, 모든 함수는 stateless 다.
 */

/* [한국어] spdk/stdinc.h: SPDK 가 표준 C 헤더(stdio/stdlib/string/errno/ctype/inttypes 등)를
 * 한꺼번에 끌어오는 facade. 직접 <stdio.h> 등을 include 하지 않는 것이 SPDK 컨벤션이며,
 * 빌드 시스템이 plat 별로 적절한 stdint/stdbool 등을 한 번에 제어한다. */
#include "spdk/stdinc.h"

/* [한국어] spdk/string.h: 본 파일이 정의하는 모든 외부 노출 함수의 prototype 을 담은 공개 헤더.
 * 헤더에는 SCNu64/PRIu64 같은 inttypes 매크로 사용 컨벤션, 반환 규약(ownership, errno 의미)이
 * 한국어 주석으로 자세히 정의되어 있다. */
#include "spdk/string.h"

/*
 * [한국어]
 * spdk_vsprintf_append_realloc - 기존 버퍼 뒤에 가변 인자 포맷 문자열을 이어 붙이고 realloc 한다.
 *
 * @buffer: 기존에 malloc 으로 할당된 NUL 종단 문자열 버퍼. NULL 이면 신규 할당으로 동작(=spdk_vsprintf_alloc 와 동일).
 *          ownership: 호출자가 소유하던 것을 함수 내부에서 realloc 하므로, 함수 호출 후에는 반환값(새 포인터)을 사용해야 한다.
 * @format: printf 스타일 포맷 문자열. NULL 불가.
 * @args: format 에 대응되는 va_list. va_start 로 초기화된 상태여야 하며, 호출자가 va_end 로 정리할 책임을 진다.
 * @return: append 가 끝난 후의 새 버퍼 포인터. realloc 실패/포맷 인코딩 실패 시 NULL — 이 경우 기존 buffer 는
 *          realloc(3) 의 표준 동작에 따라 그대로 유지되므로 호출자는 원본 포인터를 잃지 않도록 임시 변수에 받아야 한다.
 *
 * SPDK 가 동적으로 메시지/JSON 문자열을 만들 때 가장 광범위하게 쓰이는 빌딩 블록이다.
 * vasprintf(3) 가 GNU 확장이라 모든 플랫폼에 존재하지 않으므로 vsnprintf 를 두 번 호출하는 패턴
 * (1차: 길이 측정용 dry-run, 2차: 실제 기록)으로 이식성을 확보한다.
 *
 * 동작 단계:
 *   1) 기존 버퍼의 strlen 을 구해 append 가 시작될 오프셋(orig_size) 을 결정.
 *   2) va_copy 로 args 를 복제한 뒤 vsnprintf(NULL, 0, ...) 로 포맷 결과의 정확한 길이를 계산(POSIX 보장).
 *   3) orig_size + new_size + 1(NUL) 만큼 realloc 으로 버퍼 크기 확장.
 *   4) 원본 끝(orig_size) 위치부터 vsnprintf 로 실제 바이트를 기록.
 *
 * 실행 컨텍스트: 일반 유저스페이스 스레드(reactor / RPC server / app init 등). 내부 상태가 없어
 * 어떤 스레드에서든 호출 가능하며 재진입 안전. 단, 동일한 buffer 포인터를 여러 스레드가 동시에
 * realloc 하는 것은 호출자가 막아야 한다(외부 동기화).
 *
 * 호출 체인:
 *   spdk_vsprintf_alloc / spdk_sprintf_append_realloc → [이 함수] → libc realloc + vsnprintf
 */
char *
spdk_vsprintf_append_realloc(char *buffer, const char *format, va_list args)
{
	/* [한국어] args 의 복제본. vsnprintf(NULL, 0, ...) 로 길이 측정 시 va_list 가 소비되므로,
	 * 실제 기록을 위해 va_copy 로 동일 상태의 복사본을 따로 만들어 두 번 호출하는 패턴을 쓴다. */
	va_list args_copy;
	/* [한국어] realloc 결과를 받을 임시 포인터. realloc 이 실패하면 기존 buffer 가 그대로 유지되므로
	 * 임시 변수를 거치지 않으면 NULL 대입으로 원본 포인터를 잃어 메모리 누수가 발생한다. */
	char *new_buffer;
	/* [한국어] orig_size 는 기존 문자열 길이(NUL 제외), new_size 는 append 결과 길이.
	 * 0 으로 초기화하는 이유: buffer == NULL 인 경우 strlen 을 호출하지 않고 0 으로 시작해야 함. */
	int orig_size = 0, new_size;

	/* Original buffer size */
	/* [한국어] 기존 버퍼가 있으면 그 길이를 측정하여 append 시작 오프셋을 결정.
	 * NULL 인 경우는 신규 할당으로 동작하므로 orig_size = 0 유지. */
	if (buffer) {
		orig_size = strlen(buffer);
	}

	/* Necessary buffer size */
	/* [한국어] args 를 복제해 vsnprintf 의 dry-run(NULL, 0) 에 사용. POSIX 에서 vsnprintf 는
	 * 버퍼 크기가 0/NULL 이어도 "기록되었을 바이트 수"를 반환하도록 보장한다 — 이 값으로 정확한
	 * 추가 할당 크기를 계산할 수 있다. */
	va_copy(args_copy, args);
	/* [한국어] dry-run 호출. NUL 종단을 제외한 바이트 수가 반환된다. 음수면 인코딩 오류(EILSEQ 등). */
	new_size = vsnprintf(NULL, 0, format, args_copy);
	/* [한국어] va_copy 로 만든 복제본은 va_end 로 정리하지 않으면 일부 ABI(특히 x86_64 SysV)에서
	 * 누수/스택 손상이 발생할 수 있으므로 반드시 짝을 맞춘다. 원본 args 는 호출자가 정리. */
	va_end(args_copy);

	/* [한국어] vsnprintf 음수 반환은 포맷 변환 실패. 메모리 미할당 상태에서 그대로 NULL 반환 —
	 * 기존 buffer 는 그대로 유지되어 호출자가 그 포인터를 계속 사용·해제할 수 있다. */
	if (new_size < 0) {
		return NULL;
	}
	/* [한국어] 최종 필요 크기 = 기존 길이 + append 길이 + 1(NUL 종단). new_size 는 append 분만이므로
	 * NUL 자리를 따로 더해야 한다. */
	new_size += orig_size + 1;

	/* [한국어] realloc 으로 버퍼 확장. realloc 은 (a) 성공 시 기존 내용을 보존한 새 포인터 반환,
	 * (b) 실패 시 NULL 반환 + 기존 메모리 그대로 유지. 임시 변수에 받아 (b) 케이스에서 buffer 를
	 * 잃지 않도록 한다. */
	new_buffer = realloc(buffer, new_size);
	if (new_buffer == NULL) {
		/* [한국어] 할당 실패. 기존 buffer 는 호출자에게 여전히 살아있는 상태로 남으며, 호출자가 free 책임.
		 * SPDK 의 realloc-실패 보존 정책은 호출 측 패턴(tmp = ...; if (!tmp) goto err;)을 강제한다. */
		return NULL;
	}

	/* [한국어] 실제 기록. 버퍼의 orig_size 위치부터 기록을 시작하여 기존 내용 뒤에 이어붙인다.
	 * 두 번째 인자는 "쓰기 가능한 최대 바이트 수"이며 NUL 종단 자리까지 포함하므로 new_size - orig_size. */
	vsnprintf(new_buffer + orig_size, new_size - orig_size, format, args);

	/* [한국어] 호출자에게 새 포인터를 반환. 호출자는 이전 buffer 포인터를 더 이상 사용하지 말아야 한다. */
	return new_buffer;
}

/*
 * [한국어]
 * spdk_sprintf_append_realloc - va_list 가 아닌 가변 인자(...) 인터페이스 버전.
 *
 * @buffer: spdk_vsprintf_append_realloc 와 동일.
 * @format: printf 포맷 문자열.
 * @...: format 인자.
 * @return: 새 버퍼 포인터. 실패 시 NULL(기존 buffer 유지).
 *
 * 내부적으로 va_start/va_end 로 va_list 를 만들어 vsprintf_append_realloc 에 위임하는 얇은 래퍼.
 * 호출자가 va_list 를 직접 다루지 않아도 되는 일반 사용 케이스용.
 *
 * 호출 체인:
 *   상위 모듈(로그/JSON 빌더 등) → [이 함수] → spdk_vsprintf_append_realloc
 */
char *
spdk_sprintf_append_realloc(char *buffer, const char *format, ...)
{
	/* [한국어] 가변 인자 추출용 va_list. va_start ~ va_end 사이에서만 유효. */
	va_list args;
	/* [한국어] 위임 호출의 반환값을 보관하여 va_end 호출 후에 반환하기 위한 임시 변수. */
	char *ret;

	/* [한국어] format 직후의 가변 인자 시작 위치를 args 에 바인딩. ABI 별로 register/stack 처리가 다름. */
	va_start(args, format);
	/* [한국어] 실제 작업은 vsprintf 변종에 위임. 결과 포인터를 ret 에 보관. */
	ret = spdk_vsprintf_append_realloc(buffer, format, args);
	/* [한국어] va_start 와 짝을 이루는 정리. 일부 ABI 에서는 va_end 누락이 스택 손상으로 이어진다. */
	va_end(args);

	return ret;
}

/*
 * [한국어]
 * spdk_vsprintf_alloc - 새 버퍼를 동적으로 할당하여 가변 인자 포맷 결과를 채운다(vasprintf 등가).
 *
 * @format: printf 포맷.
 * @args: 가변 인자 va_list.
 * @return: 새로 malloc 된 NUL 종단 문자열. 실패 시 NULL. 호출자가 free 책임.
 *
 * 내부적으로 buffer = NULL 로 spdk_vsprintf_append_realloc 를 호출하면 신규 할당으로 동작하는 점을
 * 활용한 1줄 래퍼. vasprintf(3) 의 SPDK 이식성 대체품.
 *
 * 호출 체인: 상위 모듈 → [이 함수] → spdk_vsprintf_append_realloc(NULL, ...)
 */
char *
spdk_vsprintf_alloc(const char *format, va_list args)
{
	/* [한국어] NULL 을 기존 버퍼로 넘기면 append_realloc 이 strlen 을 건너뛰고
	 * realloc(NULL, n) == malloc(n) 의 표준 동작으로 신규 할당하여 결과를 채운다. */
	return spdk_vsprintf_append_realloc(NULL, format, args);
}

/*
 * [한국어]
 * spdk_sprintf_alloc - 가변 인자(...) 버전의 동적 sprintf.
 *
 * @format: printf 포맷.
 * @...: format 인자.
 * @return: 새로 malloc 된 문자열. 실패 시 NULL. 호출자가 free 책임.
 *
 * SPDK 에서 가장 흔히 쓰이는 동적 문자열 빌더(asprintf 의 이식성 버전). 로그 메시지, RPC 응답,
 * config 키 조립 등에 사용.
 *
 * 호출 체인: 상위 모듈 → [이 함수] → spdk_vsprintf_alloc → spdk_vsprintf_append_realloc
 */
char *
spdk_sprintf_alloc(const char *format, ...)
{
	/* [한국어] 가변 인자를 va_list 로 표준화. */
	va_list args;
	/* [한국어] 결과 포인터를 va_end 후에 반환하기 위한 임시 보관. */
	char *ret;

	/* [한국어] format 뒤의 인자에서 va_list 시작. */
	va_start(args, format);
	/* [한국어] 실제 할당+포맷 작업을 vsprintf_alloc 에 위임. */
	ret = spdk_vsprintf_alloc(format, args);
	/* [한국어] va_list 정리(va_start 짝). */
	va_end(args);

	return ret;
}

/*
 * [한국어]
 * spdk_strlwr - ASCII 문자열을 in-place 로 모두 소문자화한다.
 *
 * @s: 수정 대상 문자열(쓰기 가능 메모리). NULL 허용 — NULL 입력 시 NULL 반환.
 * @return: s 를 그대로 반환(체이닝 편의). NULL 입력 시 NULL.
 *
 * 주 사용처: NQN(NVMe Qualified Name) 정규화, UUID 문자열 비교 전 대소문자 통일, RPC 키 정규화.
 * tolower(3) 는 (int)(unsigned char) 캐스트가 권장되지만 SPDK 입력은 ASCII 만 가정하므로 단순 호출.
 *
 * 실행 컨텍스트: 어디서든 호출 가능. 다만 in-place 수정이므로 호출자는 s 가 쓰기 가능한
 * 메모리(스택/heap) 인지 확인해야 한다(문자열 리터럴 금지).
 *
 * 호출 체인: 상위 모듈(NQN 처리/RPC 파서) → [이 함수] → libc tolower
 */
char *
spdk_strlwr(char *s)
{
	/* [한국어] 순회용 포인터. s 자체는 반환값으로 보존하기 위해 별도 포인터를 둔다. */
	char *p;

	/* [한국어] NULL 안전 — 호출자 코드에서 if 체크를 줄여주기 위한 SPDK 컨벤션. */
	if (s == NULL) {
		return NULL;
	}

	/* [한국어] 시작 위치를 p 에 복사하여 s 를 보존. */
	p = s;
	/* [한국어] NUL 종단까지 한 글자씩 in-place 소문자화. */
	while (*p != '\0') {
		/* [한국어] tolower 는 'A'-'Z' → 'a'-'z' 변환, 그 외 문자는 그대로. */
		*p = tolower(*p);
		p++;
	}

	/* [한국어] 호출자가 결과를 다시 사용할 수 있도록 시작 포인터 반환(체이닝). */
	return s;
}

/*
 * [한국어]
 * spdk_strsepq - strsep(3) 의 quote/backslash escape 인식 확장 버전.
 *
 * @stringp: 입출력 포인터. 호출 시 현재 위치를 가리키며, 호출 후엔 다음 토큰의 시작으로 갱신.
 *           더 이상 토큰이 없으면 *stringp = NULL.
 * @delim: 구분자 문자 집합 문자열(strchr 매칭, 즉 어떤 한 문자라도 일치하면 구분자).
 * @return: 현재 토큰의 시작 포인터. 입력이 NULL 이면 NULL.
 *
 * 표준 strsep 과 달리 다음을 처리한다:
 *   - "..."  : double quote 안의 delim/whitespace 는 토큰 일부로 취급.
 *   - '...'  : single quote 안의 delim 도 동일.
 *   - \\X    : double quote 안에서 backslash escape 처리(다음 1 글자를 그대로 출력).
 *   - 줄 끝(\n) 도 토큰 종료로 취급(개행 단위 입력 친화).
 *
 * 사용처: SPDK RPC 파라미터 파싱, config 라인 토큰화 등 사용자 입력 문자열을 split 해야 하는 곳.
 * 따옴표 처리를 통해 "key='value with spaces'" 같은 인자를 안전하게 분해.
 *
 * 동작 알고리즘:
 *   - 두 포인터 q(읽기), r(쓰기) 를 동시 사용하여 in-place 로 quote/escape 를 제거한 토큰을 만든다.
 *   - quoted 플래그(' 또는 " 코드포인트)와 bslash 플래그로 상태 머신을 운영.
 *   - 토큰 종료 후 trailing delim 들을 모두 건너뛰어 다음 호출의 시작점을 정한다.
 *
 * 실행 컨텍스트: 단일 스레드 가정(stringp 를 공유하지 않는 한). 입력 버퍼는 in-place 수정되므로
 * 쓰기 가능 메모리여야 한다.
 *
 * 호출 체인: 상위 모듈(rpc/config 파서) → [이 함수] → libc strchr
 */
char *
spdk_strsepq(char **stringp, const char *delim)
{
	/* [한국어]
	 *  p: 토큰 시작 위치(반환값 후보).
	 *  q: 읽기 포인터(원본 위치 추적).
	 *  r: 쓰기 포인터(quote/escape 가 제거된 결과를 같은 버퍼에 채워넣음). r ≤ q 가 항상 보장됨. */
	char *p, *q, *r;
	/* [한국어]
	 *  quoted: 0=따옴표 밖, '\'' 또는 '"' 의 코드포인트=해당 따옴표 안.
	 *  bslash: 직전 문자가 '\\' 였는지(이번 글자를 escape 로 처리)를 가리키는 1비트 상태. */
	int quoted = 0, bslash = 0;

	/* [한국어] 호출자가 전달한 현재 위치를 로컬 p 에 복사하여 초기 토큰 시작점으로 사용. */
	p = *stringp;
	/* [한국어] 더 이상 토큰이 없는(이전 호출에서 NULL 로 갱신된) 상태이면 즉시 NULL 반환. */
	if (p == NULL) {
		return NULL;
	}

	/* [한국어] r, q 모두 시작점에서 출발. r 은 in-place 압축용 쓰기, q 는 읽기. */
	r = q = p;
	/* [한국어] NUL 또는 줄바꿈 전까지 1 글자씩 처리. \n 을 종료로 취급하는 이유: 라인 단위로
	 * 토큰화하는 config/RPC 입력에서 한 라인 끝에서 자연스럽게 멈추기 위함. */
	while (*q != '\0' && *q != '\n') {
		/* eat quoted characters */
		/* [한국어] 직전이 '\\' 였다면 이번 1글자를 그대로 출력하고 escape 상태 종료. */
		if (bslash) {
			bslash = 0;
			*r++ = *q++;
			continue;
		} else if (quoted) {
			/* [한국어] " 안에서 '\\' 를 만나면 다음 글자에 대해 escape 모드 진입. */
			if (quoted == '"' && *q == '\\') {
				bslash = 1;
				q++;
				continue;
			} else if (*q == quoted) {
				/* [한국어] 동일 종류의 닫는 따옴표를 만나면 quoted 종료. 따옴표 자체는 출력하지 않는다. */
				quoted = 0;
				q++;
				continue;
			}
			/* [한국어] 따옴표 안의 일반 글자는 그대로 토큰에 포함. */
			*r++ = *q++;
			continue;
		} else if (*q == '\\') {
			/* [한국어] 따옴표 밖에서의 '\\' 도 다음 1글자 escape — backslash 자체는 출력하지 않음. */
			bslash = 1;
			q++;
			continue;
		} else if (*q == '"' || *q == '\'') {
			/* [한국어] 여는 따옴표 발견. 종류를 quoted 에 기억하고 따옴표 자체는 토큰에 포함시키지 않는다. */
			quoted = *q;
			q++;
			continue;
		}

		/* separator? */
		/* [한국어] delim 집합 매칭 검사 — 매칭되지 않으면 일반 글자이므로 토큰에 포함. */
		if (strchr(delim, *q) == NULL) {
			*r++ = *q++;
			continue;
		}

		/* new string */
		/* [한국어] delim 매칭 = 토큰 끝. q 를 한 칸 전진시켜 다음 호출의 시작점을 가리키게 한 뒤 종료. */
		q++;
		break;
	}
	/* [한국어] 쓰기 포인터 r 위치에 NUL 을 박아 토큰 종단. r ≤ q 이므로 q 가 가리키는 데이터를 덮어쓰지 않는다. */
	*r = '\0';

	/* skip tailer */
	/* [한국어] 연속된 delim 들을 모두 건너뛰어 다음 토큰의 실제 시작 위치를 찾는다 — "a,,b" 같은 입력에서
	 * 빈 토큰을 만들어내지 않도록 하는 SPDK 의 선택. */
	while (*q != '\0' && strchr(delim, *q) != NULL) {
		q++;
	}
	/* [한국어] 입력이 끝났는지에 따라 호출자에게 다음 위치를 알려주거나 NULL(=종료) 로 표시. */
	if (*q != '\0') {
		*stringp = q;
	} else {
		*stringp = NULL;
	}

	/* [한국어] 토큰의 시작 포인터를 반환(NUL 종단 처리는 위에서 완료). */
	return p;
}

/*
 * [한국어]
 * spdk_str_trim - 문자열 양 끝의 공백(isspace 정의: ' ', \t, \n, \r, \v, \f) 을 제거한다(in-place).
 *
 * @s: 수정 대상 문자열. NULL 허용.
 * @return: s 를 그대로 반환. NULL 입력 시 NULL.
 *
 * 알고리즘:
 *   1) 헤더 trim: 첫 비공백 문자 위치 p 를 찾는다.
 *   2) 테일 trim: NUL 직전부터 역방향으로 공백을 제거하며 q 를 NUL 위치로 이동.
 *   3) 헤더가 잘렸으면 p..q 구간을 s 의 시작점으로 복사하여 in-place 압축.
 *
 * 사용처: config 파일 한 줄 파싱 후의 key/value 정리, 사용자 입력 정규화 등.
 * 실행 컨텍스트: 어디서든 호출 가능. 단일 스레드의 in-place 수정이므로 동시 호출 금지.
 *
 * 호출 체인: 상위 모듈 → [이 함수] → libc isspace/strlen
 */
char *
spdk_str_trim(char *s)
{
	/* [한국어] p: 첫 비공백 위치, q: 헤더 trim 후 압축 또는 테일 NUL 종단 위치 추적용. */
	char *p, *q;

	/* [한국어] NULL 안전. */
	if (s == NULL) {
		return NULL;
	}

	/* remove header */
	/* [한국어] 헤더 trim — 첫 비공백 문자가 나올 때까지 p 를 전진. */
	p = s;
	while (*p != '\0' && isspace(*p)) {
		p++;
	}

	/* remove tailer */
	/* [한국어] 테일 trim — NUL 위치(q)에서 역방향으로 공백을 NUL 로 덮어쓰며 q 를 줄인다.
	 * q-1 >= p 조건으로 헤더 trim 결과보다 더 줄어들지 않도록 보장. */
	q = p + strlen(p);
	while (q - 1 >= p && isspace(*(q - 1))) {
		q--;
		*q = '\0';
	}

	/* if remove header, move */
	/* [한국어] 헤더가 잘려나간 경우(p != s)에만 메모리 압축 수행 — p..q 구간을 s 의 시작 위치로 복사.
	 * 단순 memmove 가 아닌 글자 단위 복사이지만 결과는 동일하며 NUL 종단까지 포함한다. */
	if (p != s) {
		q = s;
		while (*p != '\0') {
			*q++ = *p++;
		}
		*q = '\0';
	}

	return s;
}

/*
 * [한국어]
 * spdk_strcpy_pad - src 를 dst 에 복사하되, dst 가 size 만큼 채워질 때까지 pad 바이트로 패딩한다.
 *
 * @dst: 대상 버퍼(쓰기 가능, 최소 size 바이트). NUL 종단을 보장하지 않는다(고정 길이 필드 용도).
 * @src: NUL 종단 원본 문자열.
 * @size: dst 의 고정 길이(바이트).
 * @pad: src 가 size 보다 짧을 때 채울 바이트 값(보통 0x20=' ' 또는 0x00).
 *
 * NVMe Identify Controller 응답의 SN(Serial Number, 20 byte 공백 패딩),
 * MN(Model Number, 40 byte), FR(Firmware Revision, 8 byte) 같은 NVMe 1.x 스펙 5.15.2.1 의
 * "ASCII string padded with spaces" 형식 필드 작성에 사용된다. NUL 을 종단으로 두지 않는 점이 특징.
 *
 * 동작:
 *   - strlen(src) < size: src 전체 복사 후 남은 영역을 pad 로 채움.
 *   - strlen(src) >= size: size 바이트만 복사(잘림). NUL 종단 없음.
 *
 * 호출 체인: lib/nvme(identify 응답 빌더) / lib/nvmf(controller info) → [이 함수] → memcpy/memset
 */
void
spdk_strcpy_pad(void *dst, const char *src, size_t size, int pad)
{
	/* [한국어] src 의 실제 길이(NUL 제외). size 와 비교하여 패딩 여부 결정. */
	size_t len;

	/* [한국어] strlen 으로 src 길이 측정 — src 는 NUL 종단 가정. */
	len = strlen(src);
	if (len < size) {
		/* [한국어] 남는 공간이 있으므로 src 전체 복사 후 잔여 영역을 pad 로 채움.
		 * dst 는 void* 이므로 memcpy 호출은 캐스트 없이 가능하지만, 포인터 산술을 위해 (char*) 로 변환. */
		memcpy(dst, src, len);
		memset((char *)dst + len, pad, size - len);
	} else {
		/* [한국어] src 가 size 이상이면 size 바이트만 복사하고 패딩 불필요. NUL 미포함 잘림 발생 가능. */
		memcpy(dst, src, size);
	}
}

/*
 * [한국어]
 * spdk_strlen_pad - spdk_strcpy_pad 의 역함수. pad 바이트로 채워진 고정 길이 필드의 "유효 길이"를 구한다.
 *
 * @str: 검사할 메모리(NUL 종단 아님 가능).
 * @size: 필드의 전체 바이트 길이.
 * @pad: 패딩 바이트 값(보통 0x20 또는 0x00).
 * @return: 끝쪽의 pad 바이트들을 제외한 유효 데이터 길이(0..size).
 *
 * 사용처: NVMe Identify 응답에서 받은 SN/MN/FR 필드의 유효 문자열 길이를 구해 trim 된 형태로 표시할 때.
 * 즉 "ABC   " (size=6, pad=0x20) → 3 을 반환.
 *
 * 알고리즘: 끝에서부터 역방향 스캔하며 pad_byte 가 아닌 첫 위치를 찾는다. 모두 pad 면 0 반환.
 *
 * 호출 체인: lib/nvme(identify 응답 표시) → [이 함수] → 단순 메모리 비교
 */
size_t
spdk_strlen_pad(const void *str, size_t size, int pad)
{
	/* [한국어] start 는 시작 주소, iter 는 역방향 스캔 포인터. uint8_t* 로 캐스트하여
	 * char 부호 의존성 없이 바이트 비교를 안정적으로 수행. */
	const uint8_t *start;
	const uint8_t *iter;
	/* [한국어] int → uint8_t 캐스트한 패딩 값. 음수 입력 방어와 비교 일관성을 위해 1바이트로 정규화. */
	uint8_t pad_byte;

	/* [한국어] int pad 를 1바이트로 잘라 비교 기준으로 사용. */
	pad_byte = (uint8_t)pad;
	/* [한국어] 시작 주소를 uint8_t 로 캐스트 보관. */
	start = (const uint8_t *)str;

	/* [한국어] 빈 필드(size=0)는 의미상 길이도 0. 이후 iter 계산에서 underflow 방지 가드. */
	if (size == 0) {
		return 0;
	}

	/* [한국어] 마지막 바이트부터 역방향 스캔 시작. */
	iter = start + size - 1;
	while (1) {
		if (*iter != pad_byte) {
			/* [한국어] pad 가 아닌 첫 바이트를 끝쪽에서 찾았다면, 그 위치까지가 유효 데이터.
			 * iter - start + 1 = 0-base 위치 + 1 = 길이. */
			return iter - start + 1;
		}

		if (iter == start) {
			/* Hit the start of the string finding only pad_byte. */
			/* [한국어] start 까지 도달했는데도 모두 pad — 필드 전체가 패딩이므로 유효 길이 0. */
			return 0;
		}
		/* [한국어] 한 바이트 이전으로 이동. iter == start 가드가 위에 있어 underflow 안전. */
		iter--;
	}
}

/*
 * [한국어]
 * spdk_parse_ip_addr - "1.2.3.4:5678" 또는 "[::1]:5678" 형태의 IP+포트 문자열을 host/port 로 분리한다.
 *
 * @ip: 입력 문자열. in-place 수정됨(':' 또는 ']' 자리에 NUL 을 박아 토큰을 분리). NULL 시 -EINVAL.
 * @host: out 파라미터. 호스트 부분의 시작 포인터(ip 내부 위치)를 저장. NULL 가능 — 그 경우 미설정.
 *        (실제로는 항상 설정되도록 작성됨; 호출자는 이 포인터로 host 문자열 참조)
 * @port: out 파라미터. 포트 부분의 시작 포인터. 포트가 없으면 NULL 로 설정.
 * @return: 0 성공, -EINVAL 형식 오류.
 *
 * IPv6 는 RFC 3986 의 "[address]:port" 표기를 인식한다. host 와 port 는 in-place 로 잘리므로
 * 호출자는 ip 가 살아있는 동안만 host/port 포인터를 사용해야 한다.
 *
 * 사용처: lib/nvmf transport listen address 파싱, RPC 의 IP 입력 파싱.
 *
 * 호출 체인: lib/nvmf/* / app/rpc → [이 함수] → libc strchr
 */
int
spdk_parse_ip_addr(char *ip, char **host, char **port)
{
	/* [한국어] strchr 결과를 받을 포인터. ']' 또는 ':' 위치를 가리키게 된다. */
	char *p;

	/* [한국어] NULL 입력 가드. -EINVAL 은 SPDK 의 errno 음수 반환 컨벤션. */
	if (ip == NULL) {
		return -EINVAL;
	}

	/* [한국어] out 파라미터 초기화 — 실패/포트 누락 케이스에서도 호출자가 안전하게 검사 가능. */
	*host = NULL;
	*port = NULL;

	if (ip[0] == '[') {
		/* IPv6 */
		/* [한국어] IPv6 는 [....] 형태로 감싸여 있어야 한다. ']' 위치 탐색. */
		p = strchr(ip, ']');
		if (p == NULL) {
			/* [한국어] ']' 가 없으면 잘못된 IPv6 표기. */
			return -EINVAL;
		}
		/* [한국어] '[' 다음 글자부터 host 시작. */
		*host = &ip[1];
		/* [한국어] ']' 자리에 NUL 을 박아 host 문자열을 종단. ip 입력은 in-place 로 잘린다. */
		*p = '\0';

		/* [한국어] ']' 다음 글자 검사. 끝이면 포트 없음(정상). ':' 가 아니면 형식 오류. */
		p++;
		if (*p == '\0') {
			return 0;
		} else if (*p != ':') {
			return -EINVAL;
		}

		/* [한국어] ':' 다음이 포트 시작. 비어 있으면 포트 미지정으로 정상 처리. */
		p++;
		if (*p == '\0') {
			return 0;
		}

		*port = p;
	} else {
		/* IPv4 */
		/* [한국어] IPv4 는 "host:port" 또는 "host" 만. ':' 검색. */
		p = strchr(ip, ':');
		if (p == NULL) {
			/* [한국어] ':' 없으면 host 만 — port 는 NULL 유지. */
			*host = ip;
			return 0;
		}

		/* [한국어] ':' 위치에 NUL 을 박아 host 와 port 를 분리. */
		*host = ip;
		*p = '\0';

		/* [한국어] ':' 다음이 포트 시작. 비어 있으면 포트 미지정. */
		p++;
		if (*p == '\0') {
			return 0;
		}

		*port = p;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_str_chomp - 문자열 끝의 \r 과 \n 을 모두 제거한다(in-place).
 *
 * @s: 수정 대상 문자열(NUL 종단). NULL 금지(strlen 호출).
 * @return: 제거된 바이트 수.
 *
 * Perl 의 chomp 와 유사. config 파일/RPC 응답 등 줄 단위 입력에서 EOL 을 정리할 때 사용.
 * \r\n, \n, \r 모두를 처리하여 Windows/Unix 입력을 모두 수용.
 *
 * 호출 체인: 상위 모듈(config 라인 리더 등) → [이 함수] → libc strlen
 */
size_t
spdk_str_chomp(char *s)
{
	/* [한국어] 현재 문자열 길이. 끝에서부터 \r/\n 을 NUL 로 덮어쓸 때마다 감소시킨다. */
	size_t len = strlen(s);
	/* [한국어] 제거된 바이트 수 누적(반환값). */
	size_t removed = 0;

	while (len > 0) {
		/* [한국어] 마지막 글자가 \r/\n 이 아니면 종료 — 그 이전은 보존. */
		if (s[len - 1] != '\r' && s[len - 1] != '\n') {
			break;
		}

		/* [한국어] 끝 글자를 NUL 로 덮어 사실상 길이 1 줄임. */
		s[len - 1] = '\0';
		len--;
		removed++;
	}

	return removed;
}

/*
 * [한국어]
 * spdk_strerror_r - thread-safe strerror 문자열 변환(POSIX/GNU 두 변종을 모두 처리).
 *
 * @errnum: 변환할 errno 값(양수). 음수일 때 호출자가 부호 정정해서 넘긴다.
 * @buf: 결과 문자열을 받을 호출자 제공 버퍼.
 * @buflen: buf 크기(NUL 포함).
 *
 * libc 의 strerror_r 는 두 변종이 있다:
 *   - POSIX 변종: int strerror_r(int, char *, size_t) — 0 성공, !=0 실패.
 *   - GNU 변종 (__USE_GNU 또는 _GNU_SOURCE): char *strerror_r(int, char *, size_t)
 *     반환값이 buf 와 다를 수 있으며(정적 문자열 반환 가능), buf 에는 항상 채워지지는 않는다.
 *
 * SPDK 는 두 경우 모두 buf 에 NUL 종단 결과가 채워지도록 #if 분기로 정규화한다. 실패 시
 * "Unknown error N" 으로 fallback 메시지를 채워 호출자가 항상 사용 가능한 문자열을 갖도록 한다.
 *
 * 짝 함수: lib/util/strerror_tls.c 의 spdk_strerror — 본 함수를 TLS 버퍼와 함께 사용해 호출자가
 * buf/buflen 을 신경쓰지 않아도 되도록 한다. lib/log/log.c 의 SPDK_ERRLOG/PRIerrno 가 핵심 호출자.
 *
 * 호출 체인: 상위 모듈(에러 로그) → [이 함수] → libc strerror_r / snprintf
 */
void
spdk_strerror_r(int errnum, char *buf, size_t buflen)
{
	/* [한국어] strerror_r 결과 코드(POSIX) 또는 GNU 변종에서의 성공/실패 플래그(0/1). */
	int rc;

#if defined(__USE_GNU)
	/* [한국어] GNU 변종은 char* 반환. 반환 포인터가 buf 와 같으면 이미 buf 에 기록된 것. */
	char *new_buffer;
	new_buffer = strerror_r(errnum, buf, buflen);
	if (new_buffer == buf) {
		/* [한국어] buf 에 직접 기록된 케이스 — 그대로 사용. */
		rc = 0;
	} else if (new_buffer != NULL) {
		/* [한국어] glibc 가 정적 문자열을 반환한 케이스 — buf 에 명시적으로 복사. snprintf 로 buflen 보호. */
		snprintf(buf, buflen, "%s", new_buffer);
		rc = 0;
	} else {
		/* [한국어] NULL 반환은 변환 실패 — 아래 fallback 으로 처리. */
		rc = 1;
	}
#else
	/* [한국어] POSIX 변종은 int 반환. 0 = 성공, !=0 = 실패(buf 내용 비결정적). */
	rc = strerror_r(errnum, buf, buflen);
#endif

	/* [한국어] 어느 변종이든 실패하면 "Unknown error N" 형태로 fallback. 호출자는 항상 유효한 문자열을 본다. */
	if (rc != 0) {
		snprintf(buf, buflen, "Unknown error %d", errnum);
	}
}

/*
 * [한국어]
 * spdk_parse_capacity - "1024", "16K", "32M", "8G" 같은 단위 접미사 capacity 문자열을 uint64_t 로 파싱.
 *
 * @cap_str: 입력 문자열. 양의 정수 + 선택적 1글자 단위 접미사(k/K/m/M/g/G).
 * @cap: 출력 파라미터. 바이트 단위로 변환된 값.
 * @has_prefix: 출력 파라미터. NULL 가능. 단위 접미사 사용 여부(true/false). 호출자는 원본 사용자 입력 형태를
 *              에코백할지 결정할 때 활용.
 * @return: 0 성공, -EINVAL 형식 오류, -errno 시스템 에러(scanf 내부).
 *
 * 단위 변환 규칙(이진수 단위, IEC 가 아닌 JEDEC 약식):
 *   K/k = 1024, M/m = 1024^2, G/g = 1024^3.
 * 'T' 는 본 구현에서는 미지원(spdk_str_to_uint64 등 다른 함수에서 별도 처리).
 *
 * 사용처: bdev 설정의 size 파라미터, malloc bdev 의 num blocks 입력, NVMe-oF reservation size 등.
 *
 * 알고리즘:
 *   1) sscanf 로 "%PRIu64%c" 매칭 — rc=1(숫자만), rc=2(숫자+접미사) 분기.
 *   2) rc=0 + errno==0 은 "숫자로 시작하지 않음" → -EINVAL.
 *   3) bin_prefix switch 로 단위 곱셈.
 *
 * 호출 체인: 상위 모듈(config/RPC 핸들러) → [이 함수] → libc sscanf
 */
int
spdk_parse_capacity(const char *cap_str, uint64_t *cap, bool *has_prefix)
{
	/* [한국어] sscanf 의 매칭된 변수 개수(0/1/2). */
	int rc;
	/* [한국어] 단위 접미사 문자(있는 경우). */
	char bin_prefix;

	/* [한국어] "%PRIu64" = uint64_t 매칭, "%c" = 1글자(단위 접미사). errno 는 sscanf 가 설정 가능. */
	rc = sscanf(cap_str, "%"SCNu64"%c", cap, &bin_prefix);
	if (rc == 1) {
		/* [한국어] 숫자만 매칭 = 접미사 없음. cap 은 그대로 바이트로 해석. */
		if (has_prefix != NULL) {
			*has_prefix = false;
		}
		return 0;
	} else if (rc == 0) {
		/* [한국어] 매칭 0 — 입력 자체가 숫자로 시작하지 않음. errno 분기로 EINVAL/시스템 에러 구분. */
		if (errno == 0) {
			/* No scanf matches - the string does not start with a digit */
			return -EINVAL;
		} else {
			/* Parsing error */
			return -errno;
		}
	}

	/* [한국어] rc == 2 = 숫자+접미사 모두 매칭 케이스. has_prefix 갱신. */
	if (has_prefix != NULL) {
		*has_prefix = true;
	}

	switch (bin_prefix) {
	case 'k':
	case 'K':
		/* [한국어] kibibyte (1024 byte). */
		*cap *= 1024;
		break;
	case 'm':
	case 'M':
		/* [한국어] mebibyte (1024^2 byte). */
		*cap *= 1024 * 1024;
		break;
	case 'g':
	case 'G':
		/* [한국어] gibibyte (1024^3 byte). */
		*cap *= 1024 * 1024 * 1024;
		break;
	default:
		/* [한국어] 알 수 없는 접미사 — 형식 오류. */
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_mem_all_zero - 메모리 영역이 전부 0x00 바이트인지 검사한다.
 *
 * @data: 검사 시작 주소.
 * @size: 검사할 바이트 수.
 * @return: 모두 0 이면 true, 하나라도 비-0 바이트가 있으면 false.
 *
 * 사용처: bdev 의 zero-block 최적화 판단(쓰기 데이터가 zero 면 NVMe Write Zeroes 명령으로 변환),
 * 메타데이터 검증, 초기화 누락 검사 등.
 *
 * 구현은 단순 byte-wise 루프 — SPDK 는 SSE/AVX 가속을 별도 모듈에서 제공하지만, 본 함수는
 * size 가 작은 케이스 위주의 간이용. 큰 영역 검사에는 spdk_mem_all_zero 를 호출하지 않는 것이 권장.
 *
 * 호출 체인: 상위 모듈(bdev 모듈, scsi) → [이 함수] → 단순 루프
 */
bool
spdk_mem_all_zero(const void *data, size_t size)
{
	/* [한국어] uint8_t 캐스트 — char 부호 의존성 없는 바이트 비교를 위해. */
	const uint8_t *buf = data;

	/* [한국어] size 를 감소시키면서 0 이 아닌 바이트가 나오면 즉시 false. */
	while (size--) {
		if (*buf++ != 0) {
			return false;
		}
	}

	/* [한국어] 모든 바이트가 0 이었으면 true. */
	return true;
}

/*
 * [한국어]
 * spdk_strtol - strtol(3) 의 안전 wrapper. errno + 범위 + 음수 + trailing garbage 를 모두 검증.
 *
 * @nptr: 변환할 NUL 종단 문자열. 선행 공백/+/- 는 strtol 표준에 따라 허용되지만 본 함수는 음수를 거부.
 * @base: 진법(0=auto, 8/10/16/...). RPC 에서는 보통 0(자동) 또는 10 사용.
 * @return: 변환된 long(>=0). 실패 시 음수 errno(-EINVAL=비숫자 trailing, -ERANGE=overflow/음수, -errno=기타).
 *
 * SPDK 의 음수 반환 컨벤션 때문에 정상 정수와 에러를 호출자가 한 번에 판별할 수 있다(0 미만이면 에러).
 * 호출자가 "음수 입력을 거부" 하기 위해 본 함수는 음수도 -ERANGE 로 매핑한다.
 *
 * strtol 사용 패턴:
 *   - 호출 전 errno = 0 필수(strtol 은 성공 시 errno 를 건드리지 않음).
 *   - endptr 로 trailing 비숫자 검사.
 *   - LONG_MAX/LONG_MIN + ERANGE 로 overflow 검출.
 *
 * 사용처: RPC 정수 파라미터(I/O size, queue depth, port 등) 파싱.
 *
 * 호출 체인: 상위 모듈(RPC 핸들러) → [이 함수] → libc strtol
 */
long int
spdk_strtol(const char *nptr, int base)
{
	/* [한국어] strtol 결과 보관용. */
	long val;
	/* [한국어] 변환 종료 지점 — *endptr != '\0' 이면 trailing 비숫자 존재. */
	char *endptr;

	/* Since strtoll() can legitimately return 0, LONG_MAX, or LONG_MIN
	 * on both success and failure, the calling program should set errno
	 * to 0 before the call.
	 */
	/* [한국어] strtol 은 성공 시 errno 를 건드리지 않으므로, 이전 호출의 errno 잔재를 지워
	 * 본 호출의 결과 errno 를 신뢰할 수 있게 만든다. */
	errno = 0;

	/* [한국어] libc 변환 호출. base 가 0 이면 "0x" 접두사 → 16진, "0" → 8진, 그 외 → 10진. */
	val = strtol(nptr, &endptr, base);

	if (!errno && *endptr != '\0') {
		/* Non integer character was found. */
		/* [한국어] errno 변동 없음 + endptr 가 NUL 이 아님 = 숫자 뒤에 비숫자 존재(예: "123abc"). */
		return -EINVAL;
	} else if (errno == ERANGE && (val == LONG_MAX || val == LONG_MIN)) {
		/* Overflow occurred. */
		/* [한국어] strtol 의 overflow 시그널: errno=ERANGE + val=LONG_MAX/MIN. */
		return -ERANGE;
	} else if (errno != 0 && val == 0) {
		/* Other error occurred. */
		/* [한국어] 기타 에러(예: EINVAL by base=0 with empty input). errno 그대로 음수화. */
		return -errno;
	} else if (val < 0) {
		/* Input string was negative number. */
		/* [한국어] 음수 입력 거부 — SPDK 정책상 본 함수는 unsigned-like 사용을 가정. */
		return -ERANGE;
	}

	return val;
}

/*
 * [한국어]
 * spdk_strtoll - spdk_strtol 의 64-bit(long long) 버전. 검증 로직 동일.
 *
 * @nptr: NUL 종단 입력 문자열.
 * @base: 진법.
 * @return: 변환된 long long(>=0), 실패 시 음수 errno.
 *
 * LP64 시스템에서는 long == long long 이지만 LLP64(Windows 등) 또는 32-bit 호스트에서는
 * long < long long 이므로 큰 정수 파라미터(예: capacity in bytes)에는 본 함수를 사용해야 한다.
 *
 * 호출 체인: 상위 모듈(RPC/config) → [이 함수] → libc strtoll
 */
long long int
spdk_strtoll(const char *nptr, int base)
{
	/* [한국어] strtoll 결과. */
	long long val;
	/* [한국어] 변환 종료 지점. */
	char *endptr;

	/* Since strtoll() can legitimately return 0, LLONG_MAX, or LLONG_MIN
	 * on both success and failure, the calling program should set errno
	 * to 0 before the call.
	 */
	/* [한국어] errno 초기화 — strtol 안전 패턴과 동일. */
	errno = 0;

	/* [한국어] 64-bit 변환 호출. */
	val = strtoll(nptr, &endptr, base);

	if (!errno && *endptr != '\0') {
		/* Non integer character was found. */
		/* [한국어] trailing 비숫자. */
		return -EINVAL;
	} else if (errno == ERANGE && (val == LLONG_MAX || val == LLONG_MIN)) {
		/* Overflow occurred. */
		/* [한국어] 64-bit overflow. */
		return -ERANGE;
	} else if (errno != 0 && val == 0) {
		/* Other error occurred. */
		/* [한국어] 기타 errno. */
		return -errno;
	} else if (val < 0) {
		/* Input string was negative number. */
		/* [한국어] 음수 입력 거부. */
		return -ERANGE;
	}

	return val;
}

/*
 * [한국어]
 * spdk_strarray_free - spdk_strarray_from_string / spdk_strarray_dup 의 결과를 해제한다.
 *
 * @strarray: NULL 종단 char* 배열. NULL 허용(no-op).
 *
 * 메모리 누수 방지를 위해 두 단계 free 수행:
 *   1) 각 원소 free.
 *   2) 배열 자체 free.
 * NULL 종단(=마지막 원소가 NULL) 컨벤션에 의존한다 — calloc 으로 0 초기화된 마지막 슬롯이 자연스럽게 sentinel.
 *
 * 호출 체인: 상위 모듈(RPC 핸들러 cleanup) → [이 함수] → libc free
 */
void
spdk_strarray_free(char **strarray)
{
	/* [한국어] 순회 인덱스. */
	size_t i;

	/* [한국어] NULL 안전 — 부분 초기화 후 cleanup goto 패턴에서도 안전하게 호출 가능. */
	if (strarray == NULL) {
		return;
	}

	/* [한국어] NULL sentinel 까지 각 원소 해제. */
	for (i = 0; strarray[i] != NULL; i++) {
		free(strarray[i]);
	}
	/* [한국어] 배열 자체 해제. */
	free(strarray);
}

/*
 * [한국어]
 * spdk_strarray_from_string - 구분자로 split 된 문자열을 NULL 종단 char* 배열로 변환한다.
 *
 * @str: 입력 문자열(NUL 종단). assert 로 NULL 거부.
 * @delim: 구분자 문자 집합(strpbrk 매칭).
 * @return: 새로 calloc 된 char* 배열(마지막 원소 NULL). 실패 시 NULL.
 *          호출자는 spdk_strarray_free 로 해제 책임.
 *
 * spdk_strsepq 와 달리 따옴표/escape 인식이 없는 단순 split. 동작:
 *   1) 1차 패스: strpbrk 로 토큰 개수 카운트(연속 delim 도 빈 토큰으로 셈).
 *   2) calloc(count+1) 으로 배열 + NULL sentinel 확보.
 *   3) 2차 패스: strdup/strndup 로 각 토큰을 복제.
 *   4) 어떤 strdup 라도 실패하면 모든 부분 결과를 free 하고 NULL 반환(원자적 실패).
 *
 * 사용처: RPC 의 multi-value 파라미터 파싱(예: "core0,core1,core2"),
 *          스레드 mask/CPU 리스트 파싱.
 *
 * 호출 체인: 상위 모듈(RPC) → [이 함수] → libc strpbrk/strdup/strndup/calloc
 */
char **
spdk_strarray_from_string(const char *str, const char *delim)
{
	/* [한국어] 1차 패스용 순회 포인터. */
	const char *c = str;
	/* [한국어] 토큰 개수. */
	size_t count = 0;
	/* [한국어] 결과 배열. */
	char **result;
	/* [한국어] 2차 패스 인덱스. */
	size_t i;

	/* [한국어] NULL 입력은 호출자 버그로 간주(release 빌드에서는 assert 무력화 — 호출자가 보장해야 함). */
	assert(str != NULL);
	assert(delim != NULL);

	/* Count number of entries. */
	/* [한국어] 1차 패스: strpbrk(c, delim) 으로 다음 구분자 위치 탐색.
	 * 매번 count++ 한 뒤 next==NULL 이면 마지막 토큰까지 셈한 것이므로 종료. */
	for (;;) {
		const char *next = strpbrk(c, delim);

		count++;

		if (next == NULL) {
			break;
		}

		c = next + 1;
	}

	/* Account for the terminating NULL entry. */
	/* [한국어] count + 1 = 토큰 개수 + NULL sentinel 자리. calloc 으로 0 초기화 → sentinel 자동 설정. */
	result = calloc(count + 1, sizeof(char *));
	if (result == NULL) {
		return NULL;
	}

	/* [한국어] 2차 패스 시작점 재설정. */
	c = str;

	for (i = 0; i < count; i++) {
		/* [한국어] 다음 구분자 위치 — 마지막 토큰이면 NULL. */
		const char *next = strpbrk(c, delim);

		if (next == NULL) {
			/* [한국어] 마지막 토큰 — 끝까지 strdup. */
			result[i] = strdup(c);
		} else {
			/* [한국어] 중간 토큰 — c..next 구간 strndup(길이 = next - c). */
			result[i] = strndup(c, next - c);
		}

		if (result[i] == NULL) {
			/* [한국어] strdup/strndup 실패 — 부분 결과 모두 free 후 NULL 반환(원자성). */
			spdk_strarray_free(result);
			return NULL;
		}

		if (next != NULL) {
			/* [한국어] 다음 토큰의 시작 위치(구분자 다음 글자). */
			c = next + 1;
		}
	}

	return result;
}

/*
 * [한국어]
 * spdk_strarray_dup - char* 배열의 깊은 복제. 각 문자열을 strdup 으로 별도 할당한다.
 *
 * @strarray: 복제할 NULL 종단 char* 배열(const). assert 로 NULL 거부.
 * @return: 새로 calloc + strdup 된 배열. 실패 시 NULL. 호출자가 spdk_strarray_free 로 해제.
 *
 * 사용처: RPC 핸들러가 받은 배열을 비동기 컨텍스트에 보관하기 위해 복제할 때(원본 lifetime 분리).
 *
 * 호출 체인: 상위 모듈 → [이 함수] → libc calloc/strdup
 */
char **
spdk_strarray_dup(const char **strarray)
{
	/* [한국어] count: 원본 길이, i: 복제 루프 인덱스. */
	size_t count, i;
	/* [한국어] 새 배열. */
	char **result;

	/* [한국어] NULL 입력 금지. */
	assert(strarray != NULL);

	/* [한국어] NULL sentinel 까지 길이 계산 — for 본문이 비어 있는 빈 루프 idiom. */
	for (count = 0; strarray[count] != NULL; count++)
		;

	/* [한국어] count + 1(sentinel 자리). calloc 으로 0 초기화하여 sentinel 자동 보장. */
	result = calloc(count + 1, sizeof(char *));
	if (result == NULL) {
		return NULL;
	}

	for (i = 0; i < count; i++) {
		/* [한국어] 각 원소 strdup. */
		result[i] = strdup(strarray[i]);
		if (result[i] == NULL) {
			/* [한국어] strdup 실패 — 부분 결과 모두 free 후 NULL 반환(원자성). */
			spdk_strarray_free(result);
			return NULL;
		}
	}

	return result;
}

/*
 * [한국어]
 * spdk_strcpy_replace - src 의 모든 search 출현을 replace 로 치환하여 dst 에 기록한다.
 *
 * @dst: 출력 버퍼(쓰기 가능, 최소 size 바이트).
 * @size: dst 크기(NUL 포함).
 * @src: 입력 NUL 종단 문자열.
 * @search: 찾을 부분 문자열(NUL 종단, 비-빈 가정).
 * @replace: 치환 문자열(NUL 종단, 빈 문자열 가능).
 * @return: 0 성공, -EINVAL = NULL 인자 또는 결과가 size 를 초과.
 *
 * 알고리즘:
 *   1) src 내 search 출현 횟수 c 를 strstr 루프로 카운트.
 *   2) 결과 길이 = strlen(src) + (replace_len - search_len) * c. size 초과 시 -EINVAL.
 *   3) src 를 따라가며 search 직전까지 memcpy → replace 복사 → search 다음으로 이동, 반복.
 *   4) 마지막 잔여를 복사 후 NUL 종단.
 *
 * 사용처: 환경 변수/매크로 치환, 경로 변환, 사용자 입력 정규화.
 * 주의: search/replace 가 빈 문자열이거나 search 가 replace 의 부분 문자열인 경우 무한 루프 가능 —
 *       호출자가 의미 있는 입력을 보장해야 한다(SPDK 사용 케이스에선 일반적으로 충족).
 *
 * 호출 체인: 상위 모듈(설정/경로 처리) → [이 함수] → libc strstr/memcpy/strlen
 */
int
spdk_strcpy_replace(char *dst, size_t size, const char *src, const char *search,
		    const char *replace)
{
	/* [한국어] p: src 내 다음 search 매칭 위치, q: src 내 다음 복사 시작 위치, r: dst 내 쓰기 위치. */
	const char *p, *q;
	char *r;
	/* [한국어] c=매칭 횟수, search_size/replace_size=각각 길이, dst_size=결과 예상 길이. */
	size_t c, search_size, replace_size, dst_size;

	/* [한국어] 모든 인자 NULL 가드 — RPC 등 외부 입력 경로에서 호출되므로 방어적. */
	if (dst == NULL || src == NULL || search == NULL || replace == NULL) {
		return -EINVAL;
	}

	/* [한국어] 길이 캐싱 — 루프 안에서 반복 strlen 호출을 피한다. */
	search_size = strlen(search);
	replace_size = strlen(replace);

	/* [한국어] 1차 패스: src 내 search 출현 횟수 c 카운트.
	 * strstr 의 시작 위치를 p + search_size 로 갱신해 겹친 매칭을 무시한다(예: "aaa" 에서 "aa" → 1회). */
	c = 0;
	for (p = strstr(src, search); p != NULL; p = strstr(p + search_size, search)) {
		c++;
	}

	/* [한국어] 결과 예상 길이 = 원본 길이 + (replace - search) * 매칭 수. NUL 까지 포함하면 dst_size+1 ≤ size. */
	dst_size = strlen(src) + (replace_size - search_size) * c;
	if (dst_size >= size) {
		/* [한국어] dst 가 NUL 까지 못 담으면 실패. >= size 이므로 dst[size-1] = '\0' 도 보장 못함. */
		return -EINVAL;
	}

	/* [한국어] 2차 패스 시작점 — q 는 다음 복사 시작, r 은 dst 쓰기 위치. */
	q = src;
	r = dst;

	/* [한국어] 매 search 출현마다: q..p 구간을 복사 → replace 복사 → q 를 search 다음으로 이동. */
	for (p = strstr(src, search); p != NULL; p = strstr(p + search_size, search)) {
		/* [한국어] 이전 매칭 다음(q) 부터 현재 매칭 직전(p) 까지의 원본 그대로 복사. */
		memcpy(r, q, p - q);
		r += p - q;

		/* [한국어] 매칭 위치에 replace 삽입(원본 search 는 건너뜀). */
		memcpy(r, replace, replace_size);
		r += replace_size;

		/* [한국어] q 를 search 끝 다음으로 진전 — 다음 구간 시작점. */
		q = p + search_size;
	}

	/* [한국어] 마지막 매칭 이후의 잔여 부분을 복사. strlen(q) 는 NUL 까지 제외한 길이. */
	memcpy(r, q, strlen(q));
	r += strlen(q);

	/* [한국어] 결과 NUL 종단 — dst_size < size 가 보장되므로 안전. */
	*r = '\0';

	return 0;
}
