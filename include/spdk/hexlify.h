/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] 바이너리 ↔ 16진 문자열 변환 (hexlify.h)
 *
 * === 파일의 역할 ===
 * 이진 바이트 배열과 16진(hex) 텍스트 표현 간 상호 변환 함수를 제공한다.
 * SPDK에서는 주로 RPC/JSON 응답, 로그 덤프, NVMe-oF 인증 키(DHChap secret)
 * 등을 텍스트로 주고받을 때 사용한다.
 * 두 함수 모두 결과 버퍼를 내부에서 `malloc`으로 할당해 반환하므로, 호출자
 * 책임으로 `free()` 해야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 구현: lib/util/hexlify.c. 단순 문자 매핑 함수이므로 hot path는 아님.
 * 실행 컨텍스트: 임의 스레드. 블로킹 없음.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h — size_t, malloc/free 등.
 * 의존하는 모듈: RPC 핸들러, NVMe-oF 인증 경로, 디버그 덤프 유틸.
 * 공유 자료구조: 없음.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_hexlify(bin, len):   이진 → 소문자 hex 문자열 (NUL 종단, 길이 2*len)
 *   - spdk_unhexlify(hex):      hex 문자열 → 이진 (결과 길이 strlen(hex)/2)
 */

#ifndef SPDK_HEXLIFY_H           /* [한국어] include 가드 */
#define SPDK_HEXLIFY_H

#include "spdk/stdinc.h"         /* [한국어] size_t, malloc/free */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert a binary array to hexlified string terminated by zero.
 *
 * \param bin A binary array pointer.
 * \param len Length of the binary array.
 * \return Pointer to hexlified version of @bin or NULL on failure.
 */
char *spdk_hexlify(const char *bin, size_t len);
/*
 * [한국어]
 * spdk_hexlify - 이진 배열을 hex 문자열로 변환
 *
 * @bin: 입력 바이트 배열
 * @len: 바이트 길이
 * @return: 새로 malloc된 (2*len + 1) 바이트 문자열. 실패 시 NULL.
 *
 * 결과는 소문자 16진. 호출자가 free()로 해제 책임.
 */

/**
 * Convert hexlified string to binary array of size strlen(hex) / 2.
 *
 * \param hex A hexlified string terminated by zero.
 * \return Binary array pointer or NULL on failure.
 */
char *spdk_unhexlify(const char *hex);
/*
 * [한국어]
 * spdk_unhexlify - hex 문자열을 이진으로 복원
 *
 * @hex: NUL 종단 hex 문자열 (길이 짝수 필수, 16진 문자만 허용)
 * @return: malloc된 이진 배열 (길이 strlen(hex)/2). 실패/포맷 오류 시 NULL.
 *
 * 호출자가 free()로 해제 책임. 대/소문자 혼용 허용.
 */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_HEXLIFY_H */       /* [한국어] include 가드 종료 */
