/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * Base64 utility functions
 */

/*
 * [한국어 설명] Base64 인코딩/디코딩 공개 API (base64.h)
 *
 * === 파일의 역할 ===
 * **RFC4648** 표준에 따른 Base64 인코딩/디코딩 함수를 제공한다. 두 종류의 알파벳 변형을 모두 지원:
 *   1. **Standard Base64** (`A-Z a-z 0-9 + /`): 일반 텍스트 표현용
 *   2. **URL/Filename-safe Base64** (`A-Z a-z 0-9 - _`): `+` `/` 대신 `-` `_` 사용 — URL 인자나
 *      파일명에 안전하게 포함 가능 (`+`/`/`/`=`는 URL 인코딩 충돌)
 *
 * 또한 인코딩/디코딩 결과 길이 사전 계산 헬퍼(`get_encoded_strlen`/`get_decoded_len`)를 inline으로
 * 제공하여, 호출자가 출력 버퍼를 정확히 할당할 수 있도록 한다.
 *
 * 구현은 `lib/util/base64.c`. 모두 순수 함수 — 부수효과 없음, thread-safe.
 *
 * === 전체 아키텍처에서의 위치 ===
 * **유틸리티 계층(util)**. 호출 빈도가 매우 높지는 않지만 다음 케이스에서 등장:
 *   - **NVMe-oF 인증 (DH-HMAC-CHAP)**: 키/MAC 값을 Base64로 직렬화/역직렬화
 *   - **iSCSI CHAP**: 챌린지/응답 인코딩
 *   - **JSON-RPC**: 바이너리 페이로드를 JSON 문자열에 임베드
 *   - **TLS PSK / 인증서 처리**: PEM 형식의 Base64 본문 처리
 *
 * 모두 control-path/init-path 호출 — I/O hot-path는 아님.
 *
 * === 타 모듈과의 연결 ===
 * - `spdk/stdinc.h`: size_t 등 타입
 * - `lib/nvmf/auth.c`, `lib/iscsi/iscsi_subsystem.c`: 인증 메시지 인코딩
 * - JSON RPC 핸들러: 바이너리 결과를 base64로 출력
 *
 * 데이터 흐름:
 *   raw bytes → spdk_base64_encode → base64 ASCII 문자열 (NUL 종료)
 *   base64 ASCII 문자열 → spdk_base64_decode → raw bytes (+ 실제 길이)
 *
 * === 주요 함수/구조체 요약 ===
 * - `spdk_base64_get_encoded_strlen(raw_len)` (inline): raw 바이트 수 → 인코딩된 문자열 길이 (NUL 제외)
 * - `spdk_base64_get_decoded_len(encoded_strlen)` (inline): 인코딩 길이 → 디코딩 raw 최대 길이
 * - `spdk_base64_encode(dst, src, src_len)`: 표준 알파벳으로 인코딩
 * - `spdk_base64_urlsafe_encode(dst, src, src_len)`: URL-safe 알파벳으로 인코딩
 * - `spdk_base64_decode(dst, dst_len, src)`: 표준 알파벳 디코딩
 * - `spdk_base64_urlsafe_decode(dst, dst_len, src)`: URL-safe 알파벳 디코딩
 *
 * 경계/주의:
 *   - dst 버퍼는 호출자가 사전 할당. 인코딩 시 `1 + get_encoded_strlen(src_len)` 바이트 필수 (NUL 포함)
 *   - 디코드 시 dst=NULL을 전달하면 길이만 측정 가능 (dry-run)
 *   - 인코딩과 디코딩 알파벳 변형이 일치해야 한다 (URL-safe로 인코딩한 것은 URL-safe로 디코딩)
 *   - 표준은 `=` 패딩, URL-safe는 패딩 없이 사용하기도 함 — 구현이 양쪽 호환할지는 코드 확인 필요
 */

#ifndef SPDK_BASE64_H
#define SPDK_BASE64_H

/* [한국어] size_t, void* 타입을 위해 표준 헤더 포함. */
#include "spdk/stdinc.h"

#ifdef __cplusplus
/* [한국어] C++ 환경에서 C 링키지 보장. */
extern "C" {
#endif

/**
 * Following the Base64 part in RFC4648:
 * https://tools.ietf.org/html/rfc4648.html
 */
/* [한국어] 본 헤더의 Base64 구현은 RFC4648의 §4(Standard) 와 §5(URL/Filename safe)를 따른다.
 *   - 표준: A-Z(0-25), a-z(26-51), 0-9(52-61), + (62), / (63), = (패딩)
 *   - URL-safe: + 대신 -, / 대신 _
 *   - 인코딩은 raw 3바이트를 base64 4문자로 매핑, 부족하면 = 으로 패딩.
 *   - 디코딩은 그 역과정. */

/**
 * Calculate strlen of encoded Base64 string based on raw buffer length.
 *
 * \param raw_len Length of raw buffer.
 * \return Encoded Base64 string length, excluding the terminating null byte ('\0').
 */
/*
 * [한국어]
 * spdk_base64_get_encoded_strlen - raw 바이트 수로부터 인코딩된 base64 문자열 길이 계산.
 *
 * @raw_len: 원본 바이트 수.
 * @return: 인코딩된 base64 문자열의 strlen (NUL 종료 byte 제외).
 *
 * 왜 필요한가: 인코드 호출 전에 dst 버퍼를 정확히 할당하기 위해 사용. 호출자는
 *              `malloc(spdk_base64_get_encoded_strlen(raw_len) + 1)`로 NUL 포함 버퍼 확보.
 * 동작:
 *   - 공식: `((raw_len + 2) / 3) * 4`
 *   - raw 3바이트 → base64 4문자 매핑이며, 끝이 3의 배수가 아니면 `=` 패딩으로 4문자 단위 유지
 *   - 예:
 *     raw 1 → enc 4 ("Xx==")
 *     raw 2 → enc 4 ("Xxxx=")  ← 패딩 1개
 *     raw 3 → enc 4 ("Xxxx")
 *     raw 4 → enc 8
 * inline 이유: 컴파일러가 호출 사이트에 직접 펴서 함수 호출 오버헤드 제거. 모든 코드 경로(특히 hot
 *              path가 아니더라도 짧은 산술)에서 깔끔하게 인라인된다.
 * 실행 컨텍스트: 순수 함수, thread-safe.
 */
static inline size_t spdk_base64_get_encoded_strlen(size_t raw_len)
{
	/* [한국어] (raw_len + 2) / 3: ceil(raw_len / 3)로 raw 3바이트 단위 그룹 수 계산.
	 * 그룹 수 × 4 = 인코딩 문자 수 (각 그룹이 4문자로 매핑되며, 부족분은 '=' 패딩). */
	return (raw_len + 2) / 3 * 4;
}

/**
 * Calculate length of raw buffer based on strlen of encoded Base64.
 *
 * This length will be the max possible decoded len. The exact decoded length could be
 * shorter depending on if there was padding in the Base64 string.
 *
 * \param encoded_strlen Length of encoded Base64 string, excluding terminating null
 * byte ('\0').
 * \return Length of raw buffer.
 */
/*
 * [한국어]
 * spdk_base64_get_decoded_len - 인코딩된 문자열 길이로부터 디코딩된 raw 바이트 수 계산 (최대값).
 *
 * @encoded_strlen: base64 문자열 strlen (NUL 제외).
 * @return: 디코딩된 raw 바이트 수의 **상한**. 실제는 패딩(`=`) 수에 따라 더 작을 수 있음 — 정확한
 *          디코딩 길이는 spdk_base64_decode가 dst_len out 파라미터로 반환.
 *
 * 왜 필요한가: 디코드 호출 전 dst 버퍼 크기 결정. 보수적으로 max로 잡고 실제 길이는 디코드 후 갱신.
 * 동작:
 *   - 공식 분해: `encoded_strlen / 4 * 3 + ((encoded_strlen % 4 + 1) / 2)`
 *   - 4문자 그룹당 3바이트 + 잔여 처리:
 *     · 잔여 0: 0 추가
 *     · 잔여 2: 1 추가 (4n+2 → 3n+1)
 *     · 잔여 3: 2 추가 (4n+3 → 3n+2)
 *     · 잔여 1: 비정상이지만 공식상 1 추가 — 디코더가 별도로 검증
 * 주석의 (4n,3n)/(4n+2,3n+1)/(4n+3,3n+2)는 위 매핑을 표기한 것.
 * inline 이유: 짧은 산술, 호출 오버헤드 제거.
 * 실행 컨텍스트: 순수 함수, thread-safe.
 */
static inline size_t spdk_base64_get_decoded_len(size_t encoded_strlen)
{
	/* text_strlen and raw_len should be (4n,3n), (4n+2, 3n+1) or (4n+3, 3n+2) */
	/* [한국어] 4문자 단위 그룹 수 × 3바이트 + 잔여 변환 규칙(위 표).
	 *   - encoded_strlen / 4 * 3: 완전 그룹의 raw 바이트 수
	 *   - (encoded_strlen % 4 + 1) / 2: 잔여 문자(0/2/3)에 대해 0/1/2 매핑 — 정수 나눗셈 트릭. */
	return encoded_strlen / 4 * 3 + ((encoded_strlen % 4 + 1) / 2);
}

/**
 * Base 64 Encoding with Standard Base64 Alphabet defined in RFC4684.
 *
 * \param dst Buffer address of encoded Base64 string. Its length should be enough
 * to contain Base64 string and the terminating null byte ('\0'), so it needs to be at
 * least as long as 1 + spdk_base64_get_encoded_strlen(src_len).
 * \param src Raw data buffer to be encoded.
 * \param src_len Length of raw data buffer.
 *
 * \return 0 on success.
 * \return -EINVAL if dst or src is NULL, or binary_len <= 0.
 */
/*
 * [한국어]
 * spdk_base64_encode - **표준 Base64 알파벳**(`A-Z a-z 0-9 + /`)으로 raw → ASCII 문자열 인코딩.
 *
 * @dst:     [out] 인코딩 결과를 받을 버퍼. 호출자가 미리 할당해야 하며 최소 크기는
 *           `1 + spdk_base64_get_encoded_strlen(src_len)` 바이트 (NUL 포함).
 * @src:     원본 raw 바이트 버퍼.
 * @src_len: src의 바이트 수. 0이면 -EINVAL.
 * @return:  0 = 성공. -EINVAL = dst/src가 NULL 또는 src_len <= 0.
 *
 * 왜 필요한가: 바이너리를 ASCII로 안전하게 표현 (JSON/XML/HTTP/Email 등). 8-bit clean하지 않은
 *              전송 채널에서도 안전.
 * 동작:
 *   1. src를 3바이트씩 읽어 24bit 정수로 묶음
 *   2. 6bit씩 잘라 4개의 알파벳 인덱스로 매핑
 *   3. 인덱스를 알파벳 표(`A-Z a-z 0-9 + /`)에서 문자로 변환
 *   4. 끝이 3의 배수가 아니면 마지막 그룹을 패딩 ('=' 1~2개)
 *   5. dst에 NUL 종료 추가
 * 실행 컨텍스트: thread-safe (입력은 read-only, 출력은 호출자 단독 소유).
 * 호출 체인:
 *   NVMe-oF 인증 / iSCSI CHAP / JSON-RPC 응답 빌드 → [spdk_base64_encode]
 */
int spdk_base64_encode(char *dst, const void *src, size_t src_len);

/**
 * Base 64 Encoding with URL and Filename Safe Alphabet.
 *
 * \param dst Buffer address of encoded Base64 string. Its length should be enough
 * to contain Base64 string and the terminating null byte ('\0'), so it needs to be at
 * least as long as 1 + spdk_base64_get_encoded_strlen(src_len).
 * \param src Raw data buffer to be encoded.
 * \param src_len Length of raw data buffer.
 *
 * \return 0 on success.
 * \return -EINVAL if dst or src is NULL, or binary_len <= 0.
 */
/*
 * [한국어]
 * spdk_base64_urlsafe_encode - **URL/파일명 안전 알파벳**(`-` `_` 사용)으로 인코딩.
 *
 * @dst:     [out] 인코딩 결과 버퍼. 크기 요구사항은 표준 encode와 동일.
 * @src:     원본 raw 바이트.
 * @src_len: src 바이트 수.
 * @return:  0 = 성공. -EINVAL = NULL 입력 또는 src_len <= 0.
 *
 * 왜 필요한가: `+` `/`는 URL 인코딩(`%2B` `%2F`)과 충돌, `=` 패딩도 URL에서 의미가 다름. 이를 회피해
 *              `+`→`-`, `/`→`_`로 치환된 RFC4648 §5 알파벳을 사용. 결과는 URL 쿼리/파일명에 그대로
 *              임베드 가능.
 * 동작: spdk_base64_encode와 동일하지만 알파벳 표만 다름.
 * 실행 컨텍스트: thread-safe.
 * 호출 체인:
 *   토큰 생성 / URL 빌드 → [spdk_base64_urlsafe_encode]
 */
int spdk_base64_urlsafe_encode(char *dst, const void *src, size_t src_len);

/**
 * Base 64 Decoding with Standard Base64 Alphabet defined in RFC4684.
 *
 * \param dst Buffer address of decoded raw data. Its length should be enough
 * to contain decoded raw data, so it needs to be at least as long as
 * spdk_base64_get_decoded_len(encoded_strlen). If NULL, only dst_len will be populated
 * indicating the exact decoded length.
 * \param dst_len Output parameter for the length of actual decoded raw data.
 * If NULL, the actual decoded length won't be returned.
 * \param src Data buffer for base64 string to be decoded.
 *
 * \return 0 on success.
 * \return -EINVAL if src is NULL, or content of src is illegal.
 */
/*
 * [한국어]
 * spdk_base64_decode - 표준 Base64 알파벳으로 인코딩된 문자열을 raw 바이트로 디코딩.
 *
 * @dst:     [out] 디코딩 결과 raw 바이트 버퍼. NULL 가능 — NULL이면 길이만 측정 (dry-run).
 *           non-NULL이면 최소 `spdk_base64_get_decoded_len(strlen(src))` 바이트 사전 할당 필요.
 * @dst_len: [out, 선택] 실제 디코딩된 정확한 raw 길이 (패딩 보정 후). NULL이면 길이 정보 미반환.
 * @src:     NUL 종료 base64 문자열. NULL이면 -EINVAL.
 * @return:  0 = 성공. -EINVAL = src가 NULL이거나 잘못된 문자/길이.
 *
 * 왜 필요한가: 인코딩의 역연산. JSON-RPC로 받은 base64 페이로드, NVMe-oF 인증 응답 등을 raw로 복원.
 * 동작:
 *   1. src 길이 측정 (strlen)
 *   2. 4문자씩 읽어 알파벳 역매핑 (문자 → 6bit 인덱스)
 *   3. 4개의 6bit를 24bit로 합쳐 3바이트로 분리하여 dst에 기록
 *   4. 패딩 '=' 발견 시 잘려나간 바이트만큼 출력 길이 감소
 *   5. *dst_len에 정확한 길이 저장 (있으면)
 * 에러 경로:
 *   - 잘못된 문자(알파벳 외) → -EINVAL
 *   - 잘못된 패딩 위치/개수 → -EINVAL
 *   - 길이가 4의 배수가 아니면서 패딩이 비정상 → -EINVAL
 * 실행 컨텍스트: thread-safe.
 * 호출 체인:
 *   JSON-RPC 인자 파싱 / 인증 응답 처리 → [spdk_base64_decode]
 */
int spdk_base64_decode(void *dst, size_t *dst_len, const char *src);

/**
 * Base 64 Decoding with URL and Filename Safe Alphabet.
 *
 * \param dst Buffer address of decoded raw data. Its length should be enough
 * to contain decoded raw data, so it needs to be at least as long as
 * spdk_base64_get_decoded_len(encoded_strlen). If NULL, only dst_len will be populated
 * indicating the exact decoded length.
 * \param dst_len Output parameter for the length of actual decoded raw data.
 * If NULL, the actual decoded length won't be returned.
 * \param src Data buffer for base64 string to be decoded.
 *
 * \return 0 on success.
 * \return -EINVAL if src is NULL, or content of src is illegal.
 */
/*
 * [한국어]
 * spdk_base64_urlsafe_decode - URL/파일명 안전 알파벳으로 인코딩된 문자열을 디코딩.
 *
 * @dst:     [out] 디코딩 결과 (NULL이면 길이만 측정).
 * @dst_len: [out, 선택] 실제 raw 길이.
 * @src:     URL-safe base64 NUL 종료 문자열.
 * @return:  0 = 성공. -EINVAL = NULL/잘못된 입력.
 *
 * 왜 필요한가: urlsafe_encode의 역연산. URL 토큰/파일명에서 추출한 base64를 raw로 복원.
 * 동작: spdk_base64_decode와 동일하지만 `-`/`_` 알파벳 사용.
 * 실행 컨텍스트: thread-safe.
 * 호출 체인:
 *   URL 토큰 검증 → [spdk_base64_urlsafe_decode]
 */
int spdk_base64_urlsafe_decode(void *dst, size_t *dst_len, const char *src);

#ifdef __cplusplus
/* [한국어] C++ extern "C" 블록 닫기. */
}
#endif

#endif /* SPDK_BASE64_H */
/* [한국어] SPDK_BASE64_H 헤더 가드 종료. */
