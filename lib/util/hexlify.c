/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] 바이너리 ↔ 16진수 문자열 변환 유틸 (hexlify.c)
 *
 * === 파일의 역할 ===
 * 임의의 바이트 시퀀스를 사람이 읽을 수 있는 16진수 문자열로 인코딩
 * (`spdk_hexlify`)하거나, 그 역으로 16진수 문자열을 바이너리 바이트로
 * 디코딩(`spdk_unhexlify`)하는 두 함수를 제공한다. 출력은 모두 새로 malloc된
 * 버퍼이며, 호출자가 free 책임을 진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK에서 16진수 표현이 필요한 경로의 공통 헬퍼:
 *   - NVMe-oF: NQN(NVMe Qualified Name) 내 EUI/UUID 표시, 비밀키
 *     (DH-HMAC-CHAP) 인코딩/디코딩.
 *   - blobstore/lvol UUID 디버그 dump.
 *   - JSON-RPC 응답에서 raw key/digest를 16진 문자열로 노출.
 * 호출 흐름 예: lib/nvme의 인증 코드 → spdk_unhexlify(설정의 key string) →
 *   바이너리 키 버퍼 → HMAC 계산.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `spdk/hexlify.h`(prototype), `spdk/log.h`(SPDK_ERRLOG 매크로).
 *   추가로 stdlib(malloc/free), string.h(strlen) 사용.
 * - 호출자: lib/nvme/nvme_auth.c, lib/nvmf/auth.c, RPC 핸들러 등.
 * - 공유 상태: 없음. 모든 결과는 호출자에게 소유권이 이전되는 신규 버퍼.
 *
 * === 주요 함수/구조체 요약 ===
 * - __c2v(c): 16진 문자(0-9, a-f, A-F) → 0..15 정수. 잘못된 문자는 -1.
 * - __v2c(v): 0..15 정수 → 소문자 16진 문자. 범위 밖이면 -1.
 * - spdk_hexlify(bin, len): bin의 len 바이트를 "ab12..." 형식 NUL-terminated
 *   문자열로 변환. 결과 버퍼 크기 = len*2 + 1.
 * - spdk_unhexlify(hex): hex 문자열을 바이너리 버퍼로 디코딩. 길이가 홀수
 *   이거나 잘못된 문자가 있으면 NULL.
 */

#include "spdk/hexlify.h"
/* [한국어] spdk_hexlify / spdk_unhexlify 공개 prototype. */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG 매크로(에러 로깅). 디코딩 실패 시 잘못된 입력을 알림. */

/*
 * [한국어]
 * __c2v - 16진 문자 한 개를 0..15 정수로 변환.
 *
 * @c: 16진 문자 (0-9 / a-f / A-F).
 * @return: 0..15 정수, 또는 잘못된 입력일 때 -1.
 *
 * spdk_unhexlify 내부 헬퍼. inline static으로 컴파일 시 호출이 인라인되어
 * 분기가 단순해진다. 대소문자 모두 허용해 SPDK가 다양한 외부 도구의 출력을
 * 받아들일 수 있게 한다.
 *
 * 호출 체인: spdk_unhexlify → __c2v(매 nibble마다 호출).
 */
static inline int
__c2v(char c)
{
	if ((c >= '0') && (c <= '9')) {
		/* [한국어] '0'..'9' 영역 → ASCII 차이만큼 빼서 0..9. */
		return c - '0';
	}
	if ((c >= 'a') && (c <= 'f')) {
		/* [한국어] 'a'..'f' 영역 → 10..15. +10 오프셋으로 16진 매핑. */
		return c - 'a' + 10;
	}
	if ((c >= 'A') && (c <= 'F')) {
		/* [한국어] 대문자 'A'..'F'도 동일하게 10..15로 매핑. SPDK는
		 * 대소문자 혼용 입력도 허용한다. */
		return c - 'A' + 10;
	}
	return -1;
	/* [한국어] 위 어디에도 속하지 않으면 잘못된 16진 문자 → -1로 시그널. */
}

/*
 * [한국어]
 * __v2c - 0..15 정수를 소문자 16진 문자로 변환.
 *
 * @c: 변환할 nibble (0..15).
 * @return: 소문자 16진 문자, 또는 범위 밖이면 -1 (signed char로 시그널).
 *
 * spdk_hexlify 내부 헬퍼. lookup table을 사용해 분기 없이 매핑.
 *
 * 호출 체인: spdk_hexlify → __v2c(매 nibble마다 호출).
 */
static inline signed char
__v2c(int c)
{
	const char hexchar[] = "0123456789abcdef";
	/* [한국어] 0..15 → 16개 소문자 16진 문자 lookup. SPDK는 출력 측에서는
	 * 일관되게 소문자를 사용해 비교(eq) 시 정규화 부담을 줄인다. */
	if (c < 0 || c > 15) {
		/* [한국어] 정상 경로에서 호출자는 항상 0..15만 넘기지만, 방어적
		 * 검사로 -1을 반환해 디버그 시 인덱스 버그 추적을 돕는다. */
		return -1;
	}
	return hexchar[c];
	/* [한국어] 단일 lookup. 컴파일러가 인라이닝하면 mov 한두 개로 끝난다. */
}

/*
 * [한국어]
 * spdk_hexlify - 바이너리 버퍼를 NUL-terminated 16진 문자열로 인코딩.
 *
 * @bin: 입력 바이트 시작 주소.
 * @len: 입력 바이트 수. 0이면 빈 문자열("\0")만 반환.
 * @return: malloc된 (len*2 + 1) 바이트 문자열. 실패 시 NULL.
 *          호출자가 free() 책임을 진다.
 *
 * 동기: 디버그 dump, RPC 응답, NQN/UUID 표현 등에서 raw 바이트를 텍스트로
 * 노출해야 하는 경로가 많다. SPDK는 그 변환을 일관된 한 함수로 모은다.
 *
 * 실행 컨텍스트: 일반 SPDK/POSIX 스레드 어디서든. malloc 사용 → reactor
 * 핫패스에서는 권장하지 않음 (관리/RPC 경로용).
 *
 * 호출 체인: 호출자(예: RPC 핸들러) → spdk_hexlify → __v2c.
 */
char *
spdk_hexlify(const char *bin, size_t len)
{
	char *hex, *phex;
	/* [한국어] hex: 결과 버퍼 시작, phex: 채워나가는 작업 포인터. */

	hex = malloc((len * 2) + 1);
	/* [한국어] 한 바이트 = 2 nibble = 2 문자. NUL terminator 1바이트 추가. */
	if (hex == NULL) {
		/* [한국어] 메모리 부족 시 NULL을 그대로 호출자에게 전파. */
		return NULL;
	}
	phex = hex;
	/* [한국어] 작업 포인터를 시작 위치에 맞추고, 이후 두 칸씩 전진. */
	for (size_t i = 0; i < len; i++) {
		signed char c0 = __v2c((bin[i] >> 4) & 0x0f);
		/* [한국어] 상위 nibble 추출: 우측 4비트 시프트 후 0x0f 마스크. */
		signed char c1 = __v2c((bin[i]) & 0x0f);
		/* [한국어] 하위 nibble 추출: 0x0f 마스크. */
		if (c0 < 0 || c1 < 0) {
			/* [한국어] __v2c는 0..15만 받으면 절대 -1을 안 주지만 안전하게
			 * 검사. 0..15 범위는 비트마스크로 보장되므로 사실상 unreachable.
			 * 도달했다면 메모리 손상 또는 컴파일러 버그 → assert(false)로
			 * 디버그 빌드에서 즉시 잡고, 누수 방지를 위해 hex 해제. */
			assert(false);
			free(hex);
			return NULL;
		}
		*phex++ = c0;
		/* [한국어] 상위 nibble 문자 기록 후 포인터 전진. */
		*phex++ = c1;
		/* [한국어] 하위 nibble 문자 기록 후 포인터 전진. */
	}
	*phex = '\0';
	/* [한국어] 마지막에 NUL 종결 → printf "%s" 등으로 안전 사용 가능. */
	return hex;
	/* [한국어] 호출자에게 소유권 이전. free 책임은 호출자. */
}

/*
 * [한국어]
 * spdk_unhexlify - 16진 문자열을 바이너리 버퍼로 디코딩.
 *
 * @hex: NUL-terminated 16진 입력 문자열. 길이는 짝수여야 한다(2 문자 = 1 바이트).
 * @return: malloc된 (strlen(hex)/2) 바이트 버퍼. 실패 시 NULL과 에러 로그.
 *          호출자가 free() 책임을 진다.
 *
 * 동기: 설정 파일/RPC 입력에서 받은 16진 키·UUID 등을 raw 바이트로 변환.
 * 잘못된 길이/문자가 들어오면 즉시 거부하여 후속 코드에서의 정의되지 않은
 * 동작을 차단한다.
 *
 * 실행 컨텍스트: 일반적으로 RPC/관리 경로(콜드 패스). I/O 핫패스에서는 사용
 * 안 함. malloc 사용.
 *
 * 호출 체인: 호출자(RPC/auth 등) → spdk_unhexlify → __c2v.
 */
char *
spdk_unhexlify(const char *hex)
{
	char *res, *pres;
	/* [한국어] res: 결과 버퍼, pres: 채우기 작업 포인터. */
	size_t len = strlen(hex);
	/* [한국어] NUL 전까지의 16진 문자 수. */

	if (len % 2 != 0) {
		/* [한국어] 한 바이트 = 두 문자. 홀수면 nibble 짝이 맞지 않으므로
		 * 입력 오류로 간주하고 에러 로그 후 NULL 반환. */
		SPDK_ERRLOG("Invalid hex string len %d. It must be mod of 2.\n", (int)len);
		return NULL;
	}
	res = malloc(len / 2);
	/* [한국어] 출력 버퍼는 입력 길이의 절반. NUL terminator는 바이너리이므로
	 * 추가하지 않는다 — 호출자가 길이를 알고 사용해야 한다. */
	if (res == NULL) {
		/* [한국어] 메모리 부족: NULL 전파. 별도 로그 없음(호출자 컨텍스트
		 * 정보가 더 의미 있으므로 거기서 로깅). */
		return NULL;
	}
	pres = res;
	for (size_t i = 0; i < len; i += 2) {
		int v0 = __c2v(hex[i]);
		/* [한국어] 첫 번째 문자 → 상위 nibble. */
		int v1 = __c2v(hex[i + 1]);
		/* [한국어] 두 번째 문자 → 하위 nibble. */
		if (v0 < 0 || v1 < 0) {
			/* [한국어] 비-16진 문자 발견. 누수를 막기 위해 res를 free하고
			 * NULL 반환. SPDK_ERRLOG로 입력 문자열을 그대로 로깅 — 진단용. */
			SPDK_ERRLOG("Invalid hex string \"%s\"\n", hex);
			free(res);
			return NULL;
		}
		*pres++ = (v0 << 4) + v1;
		/* [한국어] 두 nibble 결합: 상위<<4 | 하위. byte 채운 뒤 포인터 전진. */
	}
	return res;
	/* [한국어] 디코드된 바이너리 버퍼 반환. 길이는 호출자가 strlen(hex)/2로
	 * 알고 있어야 한다 (NUL이 포함될 수 있는 raw 데이터이므로 strlen 부적절). */
}
