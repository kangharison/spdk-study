/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/** \file
 * String utility functions
 */

/*
 * [한국어 설명] 문자열 유틸리티 공개 API (string.h)
 *
 * === 파일의 역할 ===
 * SPDK 전반에서 쓰이는 **문자열 처리 헬퍼 모음**을 정의한다. 표준 C 라이브러리(libc)에 부족하거나
 * 안전하지 않은 케이스를 보강하는 함수들이다:
 *   1. **자동 할당 sprintf** (`spdk_sprintf_alloc`/`vsprintf_alloc`/`*_append_realloc`):
 *      malloc 크기 사전 계산 없이 포맷 문자열을 동적 버퍼에 채운다.
 *   2. **불변 길이/패딩 처리** (`spdk_strcpy_pad`/`spdk_strlen_pad`):
 *      NVMe 스펙처럼 고정 크기 필드(serial number, model number 등)를 다룰 때 사용 — 보통 공백
 *      문자(' ', 0x20)로 right-pad되어 NUL 종료가 없는 포맷을 안전하게 처리.
 *   3. **파싱 헬퍼** (`spdk_strsepq`/`spdk_str_trim`/`spdk_strtol`/`spdk_strtoll`/
 *      `spdk_parse_capacity`/`spdk_parse_ip_addr`):
 *      구성 파일/RPC 입력 파싱.
 *   4. **에러 메시지 변환** (`spdk_strerror`/`spdk_strerror_r`):
 *      thread-safe 에러 문자열 변환 (`strerror_r`의 라운드 어라운드).
 *   5. **버퍼 검사/소문자 변환** (`spdk_mem_all_zero`/`spdk_strlwr`/`spdk_str_chomp`).
 *   6. **문자열 배열 처리** (`spdk_strarray_from_string`/`spdk_strarray_dup`/`spdk_strarray_free`).
 *   7. **컴파일 타임 stringification** (`SPDK_STRINGIFY` 매크로).
 *
 * 구현은 `lib/util/string.c`에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * **유틸리티 계층(util)** — 모든 SPDK 모듈이 부담 없이 호출하는 기초 함수. 일부(자동 할당, 파싱)는
 * blocking 자원이 없어 hot-path 호출도 가능하지만, 자동 할당 계열은 malloc 비용이 있어 I/O 경로에서는
 * 회피하는 것이 권장. 반환 버퍼의 free 책임은 모두 호출자에게 있다.
 *
 * === 타 모듈과의 연결 ===
 * - `spdk/stdinc.h`: 표준 타입 (size_t, va_list, uint64_t, bool 등)
 * - 호출 예:
 *   · NVMe identify 데이터 처리: spdk_strcpy_pad/strlen_pad로 SN/MN 필드 안전 변환
 *   · RPC 핸들러: spdk_strtol/spdk_parse_capacity로 사용자 입력 파싱
 *   · 로그/에러 메시지: spdk_sprintf_alloc로 동적 메시지 빌드
 *   · 호스트:포트 파싱: spdk_parse_ip_addr (NVMe-oF/iSCSI/sock)
 *
 * === 주요 함수/구조체 요약 ===
 * - `SPDK_STRINGIFY(x)`: 매크로 인자를 문자열 리터럴로 — 디버그/매크로 메타프로그래밍
 * - `spdk_sprintf_alloc / vsprintf_alloc`: 자동 할당 sprintf
 * - `spdk_*sprintf_append_realloc`: 기존 버퍼에 추가 (realloc 사용)
 * - `spdk_strlwr / str_trim / str_chomp`: in-place 변환/정리
 * - `spdk_strsepq`: 따옴표를 인지하는 strsep
 * - `spdk_strerror / strerror_r`: thread-safe errno → 문자열
 * - `spdk_strcpy_pad / strlen_pad`: 고정 크기 패딩 필드 처리 (NVMe 스펙용)
 * - `spdk_parse_ip_addr`: IPv4/IPv6 host:port 파싱 (in-place)
 * - `spdk_parse_capacity`: 사람 친화적 사이즈 ("128K", "2GB") → 바이트 변환
 * - `spdk_mem_all_zero`: 0 바이트 검사
 * - `spdk_strtol / strtoll`: 엄격한 양수 정수 파싱 (음수/공백/접미사 거부)
 * - `spdk_strarray_from_string / dup / free`: 분리 토큰 배열 처리
 * - `spdk_strcpy_replace`: 부분 문자열 치환하며 복사
 *
 * 경계/주의:
 *   - 동적 할당 함수의 반환 버퍼는 **반드시 free()** — leak 주의
 *   - in-place 함수(`strsepq`, `str_trim`, `parse_ip_addr`)는 입력 문자열을 수정 — read-only 영역
 *     (string literal)에 호출하면 SEGV
 *   - `spdk_strerror`는 thread-local 정적 버퍼에 의존 — 다음 호출 전까지만 유효
 */

#ifndef SPDK_STRING_H
#define SPDK_STRING_H

/* [한국어] 표준 타입 (size_t, va_list, uint64_t, bool 등) 의존을 위해 포함. */
#include "spdk/stdinc.h"

#ifdef __cplusplus
/* [한국어] C++ 빌드 시 C 링키지 보장. */
extern "C" {
#endif

/* [한국어] 두 단계 stringification 매크로의 내부 helper.
 *   - 전처리기 단계에서 인자를 문자열 리터럴로 변환 (`#x`).
 *   - 직접 `#x`만 쓰면 매크로 인자가 다른 매크로 확장을 건너뛴다 → 두 단계 분리가 필요. */
#define _SPDK_STRINGIFY(x) #x

/* [한국어] **공개 stringification 매크로** — 매크로/숫자 등을 문자열 리터럴로 변환.
 *   - 사용 예:
 *     `#define SPDK_VERSION 24`
 *     `printf("ver: " SPDK_STRINGIFY(SPDK_VERSION) "\n");` → "ver: 24"
 *   - _SPDK_STRINGIFY를 한 번 거치는 이유: 매크로 인자가 먼저 확장된 뒤 stringify되도록 보장.
 *     예를 들어 `SPDK_STRINGIFY(SPDK_VERSION)`는 먼저 24로 확장된 뒤 "24" 가 된다. */
#define SPDK_STRINGIFY(x) _SPDK_STRINGIFY(x)

/**
 * sprintf with automatic buffer allocation.
 *
 * The return value is the formatted string, which should be passed to free()
 * when no longer needed.
 *
 * \param format Format for the string to print.
 *
 * \return the formatted string on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_sprintf_alloc - 자동 malloc 기반 sprintf. 결과 길이를 미리 계산할 필요 없음.
 *
 * @format: printf 포맷 문자열. 이어지는 가변 인자가 포맷 인자로 사용됨.
 *          `__attribute__((format(printf, 1, 2)))`로 컴파일러가 포맷 vs 인자 타입 일치 검증.
 * @return: 성공 시 malloc 버퍼(NUL 종료된 문자열). 호출자가 반드시 `free()` 호출.
 *          실패 시(메모리 부족) NULL.
 *
 * 왜 필요한가: glibc asprintf와 유사한 편의 함수. 결과 크기를 미리 모를 때 두 번 호출(snprintf로
 *              크기 측정 + malloc + snprintf로 채우기)하는 보일러플레이트 제거.
 * 동작:
 *   1. va_start + vsnprintf(NULL, 0, ...)로 필요한 크기 + 1(NUL) 계산
 *   2. malloc 후 vsnprintf로 채우기
 *   3. 호출자에게 버퍼 반환
 * 실행 컨텍스트: malloc 비용 발생 — I/O hot-path에서는 가급적 회피.
 * 호출 체인:
 *   로그/RPC 응답 빌드 → [spdk_sprintf_alloc] → malloc + vsnprintf
 */
char *spdk_sprintf_alloc(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * vsprintf with automatic buffer allocation.
 *
 * The return value is the formatted string, which should be passed to free()
 * when no longer needed.
 *
 * \param format Format for the string to print.
 * \param args A value that identifies a variable arguments list.
 *
 * \return the formatted string on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_vsprintf_alloc - va_list 버전의 sprintf_alloc. 다른 가변 인자 함수에서 호출 위임용.
 *
 * @format: printf 포맷 문자열.
 * @args:   va_list (호출자가 va_start로 초기화). 이 함수가 내부적으로 va_copy를 사용해 두 번 처리해야
 *          정확한 크기 계산이 가능 (vsnprintf는 args를 소비하므로 두 번째 호출 시 다시 init 필요).
 * @return: 성공 시 malloc 버퍼, 실패 시 NULL. 호출자 free 책임.
 *
 * 왜 필요한가: 가변 인자 함수가 자신의 va_list를 받아 위임할 때 필요 (sprintf_alloc은 ... 만 받으므로
 *              그 자체로는 위임 불가).
 * 동작:
 *   1. va_copy(args2, args)
 *   2. vsnprintf(NULL, 0, format, args2)로 크기 측정 → va_end(args2)
 *   3. malloc → vsnprintf(buf, size, format, args)로 채우기
 * 실행 컨텍스트: 동일 — malloc 호출 비용.
 * 호출 체인:
 *   다른 가변 함수 → [spdk_vsprintf_alloc]
 */
char *spdk_vsprintf_alloc(const char *format, va_list args);

/**
 * Append string using vsprintf with automatic buffer re-allocation.
 *
 * The return value is the formatted string, in which the original string in
 * buffer is unchanged and the specified formatted string is appended.
 *
 * The returned string should be passed to free() when no longer needed.
 *
 * If buffer is NULL, the call is equivalent to spdk_sprintf_alloc().
 * If the call fails, the original buffer is left untouched.
 *
 * \param buffer Buffer which has a formatted string.
 * \param format Format for the string to print.
 *
 * \return the formatted string on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_sprintf_append_realloc - 기존 문자열 버퍼에 포맷 문자열을 추가 (자동 realloc).
 *
 * @buffer: 기존 NUL 종료 문자열 버퍼. NULL이면 spdk_sprintf_alloc과 동등 동작.
 * @format: printf 포맷 문자열.
 * @return: 성공 시 새 버퍼 포인터 (realloc된 결과 — 원본 buffer 포인터는 invalidate 될 수 있음).
 *          실패 시 NULL — 이 경우 원본 buffer는 유효하게 유지(사용자가 그대로 사용/free 가능).
 *          호출자는 반드시 새 반환값을 사용해야 한다.
 *
 * 왜 필요한가: JSON RPC 응답 빌드, 진단 로그 누적 빌드 등 "여러 번에 나눠 문자열을 쌓아가는" 패턴.
 * 동작:
 *   1. 새 포맷의 길이 계산
 *   2. realloc(buffer, oldlen + newlen + 1)
 *   3. 끝에 포맷 결과 채우기
 *   4. 새 포인터 반환
 * 실행 컨텍스트: realloc 비용 — hot-path에서는 크게 회피.
 * 호출 체인:
 *   상위 빌더 → [spdk_sprintf_append_realloc] → realloc + vsnprintf
 */
char *spdk_sprintf_append_realloc(char *buffer, const char *format, ...);

/**
 * Append string using vsprintf with automatic buffer re-allocation.
 * The return value is the formatted string, in which the original string in
 * buffer is unchanged and the specified formatted string is appended.
 *
 * The returned string should be passed to free() when no longer needed.
 *
 * If buffer is NULL, the call is equivalent to spdk_sprintf_alloc().
 * If the call fails, the original buffer is left untouched.
 *
 * \param buffer Buffer which has a formatted string.
 * \param format Format for the string to print.
 * \param args A value that identifies a variable arguments list.
 *
 * \return the formatted string on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_vsprintf_append_realloc - va_list 버전의 sprintf_append_realloc. 위임용.
 *
 * @buffer: 기존 버퍼 (NULL 가능 — 신규 생성).
 * @format: printf 포맷.
 * @args:   va_list (va_start 후). 내부적으로 va_copy로 두 번 처리.
 * @return: 성공 시 새 버퍼, 실패 시 NULL (원본 보존).
 *
 * 왜 필요한가: append_realloc의 가변 인자 위임 변형.
 * 동작: vsprintf_alloc와 동일한 두 번 호출 패턴 + realloc.
 * 호출 체인:
 *   가변 함수 → [spdk_vsprintf_append_realloc]
 */
char *spdk_vsprintf_append_realloc(char *buffer, const char *format, va_list args);

/**
 * Convert string to lowercase in place.
 *
 * \param s String to convert to lowercase.
 *
 * \return the converted string.
 */
/*
 * [한국어]
 * spdk_strlwr - 문자열을 **in-place**로 소문자 변환.
 *
 * @s: 수정 대상 NUL 종료 문자열. 문자 리터럴/읽기 전용 메모리에 호출하면 SEGV.
 * @return: 입력 문자열 포인터 그대로 (체이닝 편의용).
 *
 * 왜 필요한가: 사용자 입력(특히 RPC 인자, conf 파일)의 대소문자를 정규화하여 비교/조회.
 * 동작: 각 바이트에 대해 `tolower()` 호출 (locale 의존 — 보통 C locale 가정).
 * 실행 컨텍스트: 어떤 스레드에서나 안전 (s가 단독 소유 시).
 * 호출 체인:
 *   conf 파서 / RPC 핸들러 → [spdk_strlwr]
 */
char *spdk_strlwr(char *s);

/**
 * Parse a delimited string with quote handling.
 *
 * Note that the string will be modified in place to add the string terminator
 * to each field.
 *
 * \param stringp Pointer to starting location in string. *stringp will be updated
 * to point to the start of the next field, or NULL if the end of the string has
 * been reached.
 * \param delim Null-terminated string containing the list of accepted delimiters.
 *
 * \return a pointer to beginning of the current field.
 */
/*
 * [한국어]
 * spdk_strsepq - **따옴표를 인지하는** 토크나이저 (BSD strsep의 확장).
 *
 * @stringp: 더블 포인터. `*stringp`는 현재 파싱 위치를 가리키며, 호출 후 다음 필드의 시작으로
 *           업데이트된다. 입력의 끝에 도달하면 NULL로 설정.
 * @delim:   허용 구분자 문자들의 NUL 종료 집합 (예: ", \t").
 * @return:  현재 필드의 시작 포인터 (in-place로 NUL 삽입되어 자체적으로 NUL 종료된 문자열).
 *
 * 왜 필요한가: 표준 strsep는 따옴표 안의 구분자도 분리해버려 RPC 인자/CSV 같은 입력에 부적합.
 *              spdk_strsepq는 `"..."`로 묶인 영역을 한 토큰으로 보존한다.
 * 동작: stringp부터 스캔하며 따옴표 토글 상태를 추적. 따옴표 밖에서 delim 문자가 나오면 NUL 삽입,
 *       호출자에 토큰 시작 반환, *stringp를 다음 위치로 이동.
 * 주의: 입력 문자열을 in-place 수정 (NUL 삽입). 원본 보존이 필요하면 strdup 후 호출.
 * 실행 컨텍스트: 단일 스레드 — *stringp는 호출자 소유.
 * 호출 체인:
 *   conf/RPC 파서 → [spdk_strsepq]
 */
char *spdk_strsepq(char **stringp, const char *delim);

/**
 * Trim whitespace from a string in place.
 *
 * \param s String to trim.
 *
 * \return the trimmed string.
 */
/*
 * [한국어]
 * spdk_str_trim - 문자열의 앞뒤 **공백 문자**를 in-place로 제거.
 *
 * @s: 수정 대상 NUL 종료 문자열.
 * @return: trim된 결과 시작 포인터 — 입력 s와 다를 수 있음 (앞쪽 공백을 건너뛴 새 시작).
 *
 * 왜 필요한가: 사용자 입력에 흔한 leading/trailing whitespace를 정리.
 * 동작:
 *   1. 앞쪽 공백을 건너뛰어 새 시작 찾기
 *   2. 뒤쪽 공백 위치에 NUL 삽입
 *   3. 새 시작 포인터 반환 (앞쪽 공백 영역은 그대로 메모리에 남지만 더 이상 의미 없음)
 * 실행 컨텍스트: 단일 스레드.
 * 호출 체인:
 *   conf 파서 → [spdk_str_trim]
 */
char *spdk_str_trim(char *s);

/**
 * Copy the string version of an error into the user supplied buffer
 *
 * \param errnum Error code.
 * \param buf Pointer to a buffer in which to place the error message.
 * \param buflen The size of the buffer in bytes.
 */
/*
 * [한국어]
 * spdk_strerror_r - errno → 사람 읽을 수 있는 문자열 변환 (호출자 제공 버퍼).
 *
 * @errnum: errno 값 (음수/양수 모두 허용 — 내부에서 절댓값 처리).
 * @buf:    [out] 호출자가 제공한 버퍼. 결과 메시지가 NUL 종료로 채워짐.
 * @buflen: buf의 바이트 크기. 메시지가 길면 잘려서 종료됨.
 *
 * 왜 필요한가: glibc/POSIX의 `strerror_r`은 두 종류(GNU/XSI) 시그니처가 존재해 호환성이 까다로움.
 *              SPDK는 이를 단일 시그니처로 추상화. thread-safe.
 * 동작: 내부적으로 OS의 strerror_r 호출 후, 시그니처 변형을 보정하여 buf에 안전하게 채움.
 * 실행 컨텍스트: thread-safe (호출자 버퍼만 사용).
 * 호출 체인:
 *   에러 로그 출력 → [spdk_strerror_r]
 */
void spdk_strerror_r(int errnum, char *buf, size_t buflen);

/**
 * Return the string version of an error from a static, thread-local buffer. This
 * function is thread safe.
 *
 * \param errnum Error code.
 *
 * \return a pointer to buffer upon success.
 */
/*
 * [한국어]
 * spdk_strerror - errno → 문자열 변환 (thread-local 정적 버퍼 사용).
 *
 * @errnum: errno 값.
 * @return: thread-local 정적 버퍼 내의 NUL 종료 문자열. 같은 스레드에서 다음 호출 시 덮어써짐 — 즉시
 *          출력하거나 strdup로 복사해야 안전.
 *
 * 왜 필요한가: 매번 버퍼 할당 없이 빠르게 에러 문자열을 얻기 위함. 로그 한 줄에서 즉시 사용하는 패턴.
 *              thread-local 덕분에 멀티스레드 환경에서도 race 없음.
 * 동작: __thread (TLS) 정적 버퍼에 spdk_strerror_r 호출 후 그 포인터 반환.
 * 실행 컨텍스트: thread-safe — 다만 같은 스레드 내에서는 단일 결과만 유효.
 * 호출 체인:
 *   `SPDK_ERRLOG("...: %s\n", spdk_strerror(rc))` → [spdk_strerror]
 */
const char *spdk_strerror(int errnum);

/**
 * Remove trailing newlines from the end of a string in place.
 *
 * Any sequence of trailing \\r and \\n characters is removed from the end of the
 * string.
 *
 * \param s String to remove newline from.
 *
 * \return the number of characters removed.
 */
/*
 * [한국어]
 * spdk_str_chomp - 문자열 끝의 \r/\n 시퀀스 제거 (in-place).
 *
 * @s: 수정 대상 NUL 종료 문자열.
 * @return: 제거된 문자 수.
 *
 * 왜 필요한가: fgets 등으로 읽은 행에 따라붙는 개행을 정리. Perl의 chomp 함수에서 명명 차용.
 * 동작: 끝 NUL 직전 위치부터 \r 또는 \n인 동안 NUL로 덮어쓰며 카운트 누적.
 * 실행 컨텍스트: 단일 스레드.
 * 호출 체인:
 *   sysfs/conf 파일 읽기 후 → [spdk_str_chomp]
 */
size_t spdk_str_chomp(char *s);

/**
 * Copy a string into a fixed-size buffer, padding extra bytes with a specific
 * character.
 *
 * If src is longer than size, only size bytes will be copied.
 *
 * \param dst Pointer to destination fixed-size buffer to fill.
 * \param src Pointer to source null-terminated string to copy into dst.
 * \param size Number of bytes to fill in dst.
 * \param pad Character to pad extra space in dst beyond the size of src.
 */
/*
 * [한국어]
 * spdk_strcpy_pad - 고정 크기 버퍼에 문자열을 복사하고 남은 영역을 패딩 문자로 채움.
 *
 * @dst:  대상 고정 크기 버퍼.
 * @src:  원본 NUL 종료 문자열.
 * @size: dst의 정확한 바이트 크기. src가 길면 size까지만 복사 (NUL 종료 보장 안 됨!).
 * @pad:  src 길이를 초과한 영역을 채울 바이트 (예: 공백 0x20, 0x00 등).
 *
 * 왜 필요한가: NVMe identify 데이터의 SN(Serial Number, 20바이트), MN(Model Number, 40바이트) 등
 *              **NUL 종료 없이 ASCII + 우측 공백 패딩** 형식의 필드를 표준대로 채울 때.
 * 동작:
 *   1. memcpy(dst, src, MIN(strlen(src), size))
 *   2. memset(dst + strlen(src), pad, size - strlen(src))
 * 주의: 결과가 NUL 종료가 아닐 수 있으므로 일반 strcpy/strlen으로 다루면 안 됨 — 짝인 strlen_pad 사용.
 * 실행 컨텍스트: 단일 스레드.
 * 호출 체인:
 *   NVMe identify 명령 빌드 → [spdk_strcpy_pad]
 */
void spdk_strcpy_pad(void *dst, const char *src, size_t size, int pad);

/**
 * Find the length of a string that has been padded with a specific byte.
 *
 * \param str Right-padded string to find the length of.
 * \param size Size of the full string pointed to by str, including padding.
 * \param pad Character that was used to pad str up to size.
 *
 * \return the length of the non-padded portion of str.
 */
/*
 * [한국어]
 * spdk_strlen_pad - 우측 패딩된 문자열의 **유효 길이**를 계산.
 *
 * @str:  패딩 포함 전체 버퍼.
 * @size: 전체 버퍼 바이트 수.
 * @pad:  사용된 패딩 문자.
 * @return: 패딩을 제외한 실제 문자열의 길이 (즉, 우측에서 패딩이 끝나는 위치).
 *
 * 왜 필요한가: NVMe SN/MN 등 NUL 종료가 없는 우측 패딩 문자열의 "진짜 길이"를 얻기. strlen은
 *              NUL을 찾으므로 부정확.
 * 동작: 끝(str + size - 1)부터 거꾸로 탐색하며 pad가 아닌 첫 바이트 위치를 찾고 그 인덱스 + 1 반환.
 * 실행 컨텍스트: 읽기 전용.
 * 호출 체인:
 *   NVMe identify 데이터 표시/저장 → [spdk_strlen_pad]
 */
size_t spdk_strlen_pad(const void *str, size_t size, int pad);

/**
 * Parse an IP address into its hostname and port components. This modifies the
 * IP address in place.
 *
 * \param ip A null terminated IP address, including port. Both IPv4 and IPv6
 * are supported.
 * \param host Will point to the start of the hostname within ip. The string will
 * be null terminated.
 * \param port Will point to the start of the port within ip. The string will be
 * null terminated.
 *
 * \return 0 on success. -EINVAL on failure.
 */
/*
 * [한국어]
 * spdk_parse_ip_addr - "host:port" 또는 "[ipv6]:port" 형식을 host/port로 in-place 분리.
 *
 * @ip:   in-place 수정 대상 NUL 종료 문자열. 함수 호출 후 내부에 NUL 삽입되어 host/port가 분리됨.
 *        예: "127.0.0.1:4420" → "127.0.0.1\04420" (host="127.0.0.1", port="4420")
 *        예: "[::1]:4420" → 내부에 NUL 삽입되어 host="::1", port="4420"
 * @host: [out] ip 내 호스트 부분 시작 포인터.
 * @port: [out] ip 내 포트 부분 시작 포인터.
 * @return: 0 = 성공. -EINVAL = 형식 오류 (콜론 없음, 잘못된 IPv6 대괄호 등).
 *
 * 왜 필요한가: NVMe-oF/iSCSI/sock 모듈에서 traddr/svcid를 파싱할 때 자주 사용.
 * 동작: ip 시작이 '['이면 IPv6 처리(']' 검색 후 그 다음 ':'), 아니면 마지막 ':' 찾아 분리.
 *       해당 위치에 NUL 삽입.
 * 실행 컨텍스트: 단일 스레드.
 * 호출 체인:
 *   NVMe-oF/iSCSI listen address 파싱 → [spdk_parse_ip_addr]
 */
int spdk_parse_ip_addr(char *ip, char **host, char **port);

/**
 * Parse a string representing a number possibly followed by a binary prefix.
 *
 * The string can contain a trailing "B" (KB,MB,GB) but it's not necessary.
 * "128K" = 128 * 1024; "2G" = 2 * 1024 * 1024; "2GB" = 2 * 1024 * 1024;
 * Additionally, lowercase "k", "m", "g" are parsed as well. They are processed
 * the same as their uppercase equivalents.
 *
 * \param cap_str Null terminated string.
 * \param cap Pointer where the parsed capacity (in bytes) will be put.
 * \param has_prefix Pointer to a flag that will be set to describe whether given
 * string contains a binary prefix (optional).
 *
 * \return 0 on success, or negative errno on failure.
 */
/*
 * [한국어]
 * spdk_parse_capacity - "128K", "2GB" 같은 사람 친화적 사이즈 표기를 바이트로 변환.
 *
 * @cap_str:    NUL 종료 문자열. 숫자 + 선택적 K/M/G/T(B) 접미사. 대소문자 무관.
 *              예: "128", "128K", "128KB", "2g", "2GB" 등 모두 허용.
 * @cap:        [out] 파싱된 바이트 수.
 * @has_prefix: [out, 선택] NULL이 아니면, 문자열에 K/M/G 접미사가 있었는지 여부 저장.
 *              호출자는 "사용자가 명시적으로 단위를 지정했나"를 알고 싶을 때 활용.
 * @return:     0 = 성공. 음수 errno = 실패 (-EINVAL 등).
 *
 * 왜 필요한가: 설정 파일/RPC에서 사이즈 표기를 사람이 입력하기 편하게 받기 위해. "1073741824"보다
 *              "1G"가 훨씬 명확.
 * 동작:
 *   1. strtoull로 숫자 부분 파싱
 *   2. 다음 문자가 K/M/G/T이면 1024^n 곱 (소문자도 동일)
 *   3. 다음에 'B'/'b'가 와도 무시 (단위 명시 표기 허용)
 *   4. has_prefix가 NULL 아니면 접미사 유무 저장
 * 실행 컨텍스트: 어떤 스레드에서나 안전.
 * 호출 체인:
 *   conf 파서 / spdk_nvme_perf 등 → [spdk_parse_capacity]
 */
int spdk_parse_capacity(const char *cap_str, uint64_t *cap, bool *has_prefix);

/**
 * Check if a buffer is all zero (0x00) bytes or not.
 *
 * \param data Buffer to check.
 * \param size Size of data in bytes.
 *
 * \return true if data consists entirely of zeroes, or false if any byte in data
 * is not zero.
 */
/*
 * [한국어]
 * spdk_mem_all_zero - 버퍼가 전부 0x00 바이트로만 이루어졌는지 검사.
 *
 * @data: 검사 대상 버퍼.
 * @size: 바이트 수.
 * @return: 모두 0이면 true, 하나라도 비-0이면 false.
 *
 * 왜 필요한가: NVMe 명령어/식별 데이터에서 "필드가 비어있나(미사용)" 검사. UUID/IV 등 0이면 미사용
 *              관행이 있는 필드 검출.
 * 동작: 워드 단위 빠른 비교 (8바이트씩 0과 비교) + 잔여 바이트 처리. SIMD 최적화 가능성.
 * 실행 컨텍스트: 읽기 전용, thread-safe.
 * 호출 체인:
 *   NVMe identify 검사 / blob metadata 검증 → [spdk_mem_all_zero]
 */
bool spdk_mem_all_zero(const void *data, size_t size);

/**
 * Convert the string in nptr to a long integer value according to the given base.
 *
 * spdk_strtol() does the additional error checking and allows only strings that
 * contains only numbers and is positive number or zero. The caller only has to check
 * if the return value is not negative.
 *
 * \param nptr String containing numbers.
 * \param base Base which must be between 2 and 32 inclusive, or be the special value 0.
 *
 * \return positive number or zero on success, or negative errno on failure.
 */
/*
 * [한국어]
 * spdk_strtol - 엄격한 양수 정수 파서 (long int 반환).
 *
 * @nptr: 변환할 NUL 종료 문자열. 양수 또는 0만 허용. 음수, 공백, trailing 비-숫자 모두 -EINVAL.
 * @base: 진수 (2-32). 0이면 0x/0o/일반 표기 자동 감지 (strtol 표준 동작).
 * @return: 0 이상의 결과 = 성공. 음수 = 실패 (errno를 음수화한 값 — 예: -EINVAL, -ERANGE).
 *
 * 왜 필요한가: 표준 strtol은 endptr/errno 검사가 번거롭고, "전체 문자열이 정수인지" 검증을 안 함.
 *              spdk_strtol은 입력이 유효한 양수일 때만 그 값을 반환하고, 그 외는 음수 errno로
 *              명확한 오류 신호. 호출자는 단순히 "음수면 에러" 패턴으로 처리 가능.
 * 동작:
 *   1. errno=0 후 strtol 호출
 *   2. errno 검사 (ERANGE 등)
 *   3. endptr가 입력 끝을 가리키지 않으면 -EINVAL
 *   4. 결과가 음수면 -EINVAL (양수만 허용 정책)
 * 실행 컨텍스트: thread-safe.
 * 호출 체인:
 *   RPC 인자 파싱 → [spdk_strtol]
 */
long int spdk_strtol(const char *nptr, int base);

/**
 * Convert the string in nptr to a long long integer value according to the given base.
 *
 * spdk_strtoll() does the additional error checking and allows only strings that
 * contains only numbers and is positive number or zero. The caller only has to check
 * if the return value is not negative.
 *
 * \param nptr String containing numbers.
 * \param base Base which must be between 2 and 32 inclusive, or be the special value 0.
 *
 * \return positive number or zero on success, or negative errno on failure.
 */
/*
 * [한국어]
 * spdk_strtoll - spdk_strtol의 long long 버전. 64비트 정수 파싱이 필요할 때 사용.
 *
 * @nptr: NUL 종료 문자열. 양수/0만 허용.
 * @base: 진수 (2-32 또는 0).
 * @return: 0 이상 = 성공, 음수 = 실패 (음수 errno).
 *
 * 왜 필요한가: 64비트 사이즈/오프셋 파싱 (블록 수, 바이트 수 등).
 * 동작: spdk_strtol과 동일하지만 strtoll 사용.
 * 실행 컨텍스트: thread-safe.
 * 호출 체인:
 *   RPC 인자 파싱 (큰 값) → [spdk_strtoll]
 */
long long int spdk_strtoll(const char *nptr, int base);

/**
 * Build a NULL-terminated array of strings from the given string separated by
 * the given chars in delim, as if split by strpbrk(). Empty items are pointers
 * to an empty string.
 *
 * \param str Input string
 * \param delim Separating delimiter set.
 *
 * \return the string array, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_strarray_from_string - 문자열을 구분자로 분리하여 NULL 종료 문자열 배열 반환.
 *
 * @str:   원본 NUL 종료 문자열 (수정되지 않음 — 내부에서 strdup 후 처리).
 * @delim: 구분자 집합. 예: "," / " ,\t".
 * @return: 성공 시 `char **arr` (마지막 원소가 NULL인 NULL-terminated 배열).
 *          각 원소와 배열 자체는 calloc 계열로 할당 — 호출자가 `spdk_strarray_free`로 해제.
 *          실패 시 NULL.
 *
 * 왜 필요한가: CSV/공백 분리 입력을 배열로 받기. strsep/strtok의 in-place 단점을 피하고 결과를
 *              깔끔한 배열로 받음.
 * 동작:
 *   1. 빈 토큰도 빈 문자열로 보존 (skip하지 않음)
 *   2. 토큰 수 + 1만큼 char* 배열 calloc
 *   3. 각 토큰을 strdup
 *   4. 마지막 원소를 NULL로 종료
 * 실행 컨텍스트: malloc 비용 — 핫패스 회피.
 * 호출 체인:
 *   conf 파일 list 파싱 → [spdk_strarray_from_string]
 */
char **spdk_strarray_from_string(const char *str, const char *delim);

/**
 * Duplicate a NULL-terminated array of strings. Returns NULL on failure.
 * The array, and the strings, are allocated with the standard allocator (e.g.
 * calloc()).
 *
 * \param strarray input array of strings.
 */
/*
 * [한국어]
 * spdk_strarray_dup - NULL 종료 문자열 배열의 깊은 복사.
 *
 * @strarray: 원본 배열 (각 원소가 NUL 종료 문자열, 배열 끝은 NULL).
 * @return: 새로 할당된 배열 (각 문자열도 strdup). 실패 시 NULL.
 *          호출자가 spdk_strarray_free로 해제.
 *
 * 왜 필요한가: 호출자/소유자가 분리되어야 할 때 안전한 복사본 확보 (생명주기 분리).
 * 동작: 입력 배열 끝까지 순회하며 길이 계산 → calloc → 각 원소 strdup.
 * 에러 경로: 어떤 strdup이라도 실패하면 그동안 할당된 모든 자원 free 후 NULL 반환.
 * 호출 체인:
 *   상위 모듈이 인자 보존 시 → [spdk_strarray_dup]
 */
char **spdk_strarray_dup(const char **strarray);

/**
 * Free a NULL-terminated array of strings. The array and its strings must have
 * been allocated with the standard allocator (calloc() etc.).
 *
 * \param strarray array of strings.
 */
/*
 * [한국어]
 * spdk_strarray_free - spdk_strarray_from_string/dup으로 만든 배열을 통째 해제.
 *
 * @strarray: 해제할 배열. NULL이면 no-op (안전).
 *
 * 동작: 각 원소를 free한 뒤 배열 자체 free.
 * 주의: malloc 계열로 할당된 배열에만 안전 — DPDK 메모리/spdk_malloc로 만든 배열은 사용 금지.
 */
void spdk_strarray_free(char **strarray);

/**
 * Copy a string into a fixed-size buffer with all occurrences of the search string
 * replaced with the given replace substring. The fixed-size buffer must not be less
 * than the string with the replaced values including the terminating null byte.
 *
 * \param dst Pointer to destination fixed-size buffer to fill.
 * \param size Size of the destination fixed-size buffer in bytes.
 * \param src Pointer to source null-terminated string to copy into dst.
 * \param search The string being searched for.
 * \param replace the replacement substring the replaces the found search substring.
 *
 * \return 0 on success, or negated errno on failure.
 */
/*
 * [한국어]
 * spdk_strcpy_replace - src의 모든 search 출현을 replace로 치환하여 dst에 복사.
 *
 * @dst:     대상 고정 크기 버퍼.
 * @size:    dst의 바이트 크기. 결과(NUL 포함)가 size를 초과하면 -ENOSPC 등 에러.
 * @src:     원본 NUL 종료 문자열.
 * @search:  찾을 부분 문자열.
 * @replace: 치환할 부분 문자열.
 * @return:  0 = 성공, 음수 errno = 실패 (-ENOSPC: 결과가 dst 초과 등).
 *
 * 왜 필요한가: 환경 변수 치환, 템플릿 처리 등에서 단순 검색-치환을 한 번에 처리. sed의 's/X/Y/g'와
 *              유사한 단순 변형.
 * 동작: src를 스캔하며 search 일치 시 replace를 dst에 출력, 그 외는 그대로 복사.
 *       매번 결과 길이를 추적하여 size 초과 검사.
 * 실행 컨텍스트: 단일 스레드 — dst 단독 소유 가정.
 * 호출 체인:
 *   템플릿 처리 → [spdk_strcpy_replace]
 */
int spdk_strcpy_replace(char *dst, size_t size, const char *src, const char *search,
			const char *replace);

#ifdef __cplusplus
/* [한국어] C++ extern "C" 블록 닫기. */
}
#endif

#endif
/* [한국어] SPDK_STRING_H 헤더 가드 종료. */
