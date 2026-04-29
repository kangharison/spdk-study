/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright(c) ARM Limited. 2021 All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Base64 인코딩/디코딩 유틸리티 (base64.c)
 *
 * === 파일의 역할 ===
 * 본 파일은 SPDK 가 RFC 4648 (Base64 / URL-safe Base64) 인코딩과 디코딩을
 * 자체적으로 수행할 수 있도록 하는 lookup-table 기반 구현체이다. 핵심 용도는
 * (1) JSON-RPC 페이로드에 binary blob(예: NVMe DH-HMAC-CHAP key, GPT-PT 의
 * raw 바이트, blob xattr 의 바이너리 값 등)을 ASCII safe 하게 직렬화하고,
 * (2) RPC 클라이언트로부터 들어온 base64 문자열을 다시 binary 로 복구하는
 * 것이다. 또한 ARMv8 SVE/NEON 환경에서는 SIMD 가속 경로(base64_sve.c /
 * base64_neon.c)를 추가로 통합한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/util 의 thread-agnostic, stateless 헬퍼이며 RPC 핸들러(lib/rpc), nvmf
 * 의 키/시크릿 처리, blob/xattr 등 다양한 곳에서 호출된다. SPDK 의 RPC 는
 * JSON 기반이라 binary 를 그대로 담을 수 없으므로, 거의 모든 시크릿/키 관련
 * 인자가 이 모듈을 거친다. 특별한 스레드 컨텍스트 요구는 없다(libc 만 사용,
 * lock 없음).
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h(표준 C 헤더 묶음), spdk/endian.h(from_be32/to_be32 —
 *         빅엔디안 32비트 패킹/언패킹), spdk/base64.h(공개 API 선언과
 *         spdk_base64_get_decoded_len 같은 인라인 헬퍼).
 *         ARM SIMD 빌드 시 base64_sve.c 또는 base64_neon.c 를 #include 하여
 *         가속 함수(base64_encode_sve, base64_decode_neon64 등)를 끌어옴.
 * - 사용처: spdk_rpc 핸들러의 string ↔ binary 변환, nvmf hostnqn DH 키 파싱,
 *           CLI 도구의 key import/export 경로.
 * - 데이터 흐름: binary buffer (uint8_t *) ↔ ASCII Base64 문자열 (char *).
 *   인코딩은 3바이트 → 4문자 단위로, 디코딩은 4문자 → 3바이트 단위로 진행.
 *
 * === 주요 함수/구조체 요약 ===
 * - base64_enc_table / base64_urlsafe_enc_table : 6비트 → ASCII 매핑 테이블.
 * - base64_dec_table / base64_urlsafe_dec_table : ASCII → 6비트 매핑(255=invalid).
 * - base64_encode (static) : 공통 인코더. 3바이트 단위로 32비트 패킹 후
 *                            6비트씩 4번 추출해 ASCII 출력. 끝 자리는 padding('=').
 * - base64_decode (static) : 공통 디코더. 4문자 패킹 후 6비트씩 4번 → 24비트
 *                            → 3바이트 출력. padding 처리 포함.
 * - spdk_base64_encode / spdk_base64_urlsafe_encode  : 표준/URL-safe 인코딩 외부 API.
 * - spdk_base64_decode / spdk_base64_urlsafe_decode  : 표준/URL-safe 디코딩 외부 API.
 *
 * 본 파일은 별도 구조체를 정의하지 않으며 전역 lookup table 만 사용한다.
 */

/* [한국어] SPDK 표준 C 라이브러리 묶음(string.h, stdint.h, stdlib.h 등). */
#include "spdk/stdinc.h"
/* [한국어] from_be32/to_be32 — 인코더에서 3바이트를 24비트로 패킹할 때 빅엔디안
 * 32비트 변환 헬퍼를 사용하기 위해 필요. */
#include "spdk/endian.h"
/* [한국어] 공개 API 선언과 spdk_base64_get_decoded_len 같은 인라인 헬퍼 정의. */
#include "spdk/base64.h"

/* [한국어] aarch64 빌드에서는 SIMD 가속 경로를 별도 파일로 인라인 통합한다.
 * SVE(Scalable Vector Extension)가 있으면 그쪽을, 없으면 기본 NEON64 경로를 사용. */
#ifdef __aarch64__
#ifdef __ARM_FEATURE_SVE
#include "base64_sve.c"
#else
#include "base64_neon.c"
#endif
#endif


/* [한국어] 6비트 추출 마스크 — 0b00111111. enc 시 32비트 패킹된 값에서 한 번에
 * 한 자리(6비트)씩 떼어내는 데 사용. UL 접미는 32비트 unsigned 산술 보장. */
#define BASE64_ENC_BITMASK 0x3FUL
/* [한국어] RFC 4648 의 패딩 문자 '=' — 입력 길이가 3바이트의 배수가 아닐 때 사용. */
#define BASE64_PADDING_CHAR '='

/* [한국어] 표준 Base64 알파벳 (RFC 4648 §4): A-Z(0..25), a-z(26..51), 0-9(52..61),
 * '+' (62), '/' (63). 인덱스 == 6비트 값. */
static const char base64_enc_table[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789+/";

/* [한국어] URL-safe 변종 (RFC 4648 §5): '+' → '-', '/' → '_'. URL/파일명에
 * 안전하게 포함될 수 있도록 하는 변형으로, JWT/JWS 등에서 흔히 쓴다. */
static const char base64_urlsafe_enc_table[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789-_";

/* [한국어] 표준 Base64 디코딩 테이블: ASCII 코드(0..255) → 6비트 값(0..63).
 * 잘못된 문자는 255 로 표시되어 디코딩 시 에러 검출에 사용된다.
 * 예) '+' (0x2B=43) → 62, '/' (0x2F=47) → 63, '0' (0x30=48) → 52, ...,
 *     'A' (0x41=65) → 0, 'a' (0x61=97) → 26.  나머지는 모두 255(invalid). */
static const uint8_t
base64_dec_table[] = {
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,  62, 255, 255, 255,  63,
	52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255, 255, 255, 255, 255,
	255,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
	15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255, 255,
	255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
	41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

/* [한국어] URL-safe 변종 디코딩 테이블: '-' (0x2D=45) → 62, '_' (0x5F=95) → 63
 * 로 매핑된다 (표준에서의 '+' 와 '/' 자리 대체). */
static const uint8_t
base64_urlsafe_dec_table[] = {
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,  62, 255, 255,
	52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255, 255, 255, 255, 255,
	255,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
	15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255,  63,
	255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
	41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

/*
 * [한국어]
 * base64_encode (static, 공통) - 인코딩 코어. 표준/URL-safe 분기는 enc_table 만
 * 다른 동일 알고리즘이라 본 함수에 위임한다.
 *
 * @dst:       출력 버퍼. 호출자가 spdk_base64_get_encoded_strlen(src_len)+1 이상 확보.
 * @enc_table: 사용할 6비트→ASCII 매핑 테이블 (표준 또는 URL-safe).
 * @src:       입력 binary.
 * @src_len:   입력 길이(바이트, >0).
 * @return:    0 성공, -EINVAL 인자 오류.
 *
 * 알고리즘:
 *   - 4바이트 이상 남았을 때까지: src 를 32비트(빅엔디안) 로 적재 → 상위 24비트
 *     를 6비트 단위로 4번 떼어 enc_table 매핑 → dst 에 4 글자 출력. src 는
 *     +3 (즉 24비트만 소비) 진행. 마지막 1바이트는 다음 그룹과 같이 사용.
 *   - 남은 1 또는 2 바이트(tail): 0 으로 패딩한 32비트로 만든 뒤 동일 방식
 *     으로 추출하되, 부족한 자리는 BASE64_PADDING_CHAR('=') 로 채움.
 *   - 끝에 NUL 종료 문자 추가.
 *
 * SPDK 특화: 본 함수는 lock-free, side-effect-free 라 어느 reactor 스레드에서도
 * 호출 가능. 다만 다이렉트 IO 경로의 hot path 는 아니다(주로 RPC 처리 시점).
 */
static int
base64_encode(char *dst, const char *enc_table, const void *src, size_t src_len)
{
	/* [한국어] 4바이트 묶음을 빅엔디안 32비트로 적재하기 위한 임시 변수. */
	uint32_t raw_u32;

	/* [한국어] 입력 검증 — NULL 포인터 또는 빈 입력은 인코딩 의미가 없으므로 거절. */
	if (!dst || !src || src_len <= 0) {
		return -EINVAL;
	}

#ifdef __aarch64__
#ifdef __ARM_FEATURE_SVE
	/* [한국어] ARM SVE 환경 — SIMD 가속 코어로 가능한 한 많은 바이트를 인코딩.
	 * &dst, &src, &src_len 가 in/out 인 이유: 처리한 만큼 포인터/길이가 갱신되고
	 * 잔여 분량은 아래 스칼라 루프에서 마무리한다. */
	base64_encode_sve(&dst, enc_table, &src, &src_len);
#else
	/* [한국어] ARM NEON 64-bit 가속 경로 — SVE 미지원 빌드. */
	base64_encode_neon64(&dst, enc_table, &src, &src_len);
#endif
#endif


	/* [한국어] 메인 스칼라 루프 — 4바이트 이상 남아있는 동안 3바이트씩 소비. */
	while (src_len >= 4) {
		/* [한국어] src 의 4바이트를 빅엔디안 32비트로 적재.
		 * 4바이트를 적재해야 마지막 6비트(>>8) 자리가 안전하게 채워진다.
		 * (실제로는 3바이트만 인코딩에 쓰지만, 32비트 정렬 로딩이 깔끔.) */
		raw_u32 = from_be32(src);

		/* [한국어] 1번째 6비트 — 비트 [31..26]. 최상위 6비트 추출. */
		*dst++ = enc_table[(raw_u32 >> 26) & BASE64_ENC_BITMASK];
		/* [한국어] 2번째 6비트 — 비트 [25..20]. */
		*dst++ = enc_table[(raw_u32 >> 20) & BASE64_ENC_BITMASK];
		/* [한국어] 3번째 6비트 — 비트 [19..14]. */
		*dst++ = enc_table[(raw_u32 >> 14) & BASE64_ENC_BITMASK];
		/* [한국어] 4번째 6비트 — 비트 [13..8]. (총 24비트=3바이트 소비) */
		*dst++ = enc_table[(raw_u32 >> 8) & BASE64_ENC_BITMASK];

		/* [한국어] 실제 소비량 = 3바이트 (32비트 적재했으나 마지막 8비트는 다음에 다시 사용). */
		src_len -= 3;
		src = (uint8_t *)src + 3;
	}

	/* [한국어] 입력이 정확히 3의 배수였다면 tail 처리는 건너뛴다. */
	if (src_len == 0) {
		goto out;
	}

	/* [한국어] tail (1 또는 2 바이트) 처리. 32비트 슬롯을 0 으로 초기화 후 읽기. */
	raw_u32 = 0;
	/* [한국어] 남은 src_len 바이트를 raw_u32 의 상위 쪽으로 복사 — 빅엔디안 가정. */
	memcpy(&raw_u32, src, src_len);
	/* [한국어] 호스트 바이트오더를 빅엔디안 32비트로 변환해 비트 추출 정합성 확보. */
	raw_u32 = from_be32(&raw_u32);

	/* [한국어] 1번째 6비트(상위) — 항상 출력. */
	*dst++ = enc_table[(raw_u32 >> 26) & BASE64_ENC_BITMASK];
	/* [한국어] 2번째 6비트 — 항상 출력. (1바이트 입력의 경우 하위 비트는 0) */
	*dst++ = enc_table[(raw_u32 >> 20) & BASE64_ENC_BITMASK];
	/* [한국어] 3번째 자리: tail 이 2 또는 3 바이트면 출력, 1 바이트면 '=' padding. */
	*dst++ = (src_len >= 2) ? enc_table[(raw_u32 >> 14) & BASE64_ENC_BITMASK] : BASE64_PADDING_CHAR;
	/* [한국어] 4번째 자리: tail 이 정확히 3 바이트면 출력, 그 외엔 '=' padding.
	 * (사실 tail 은 1 또는 2 만 가능하므로 이 분기에서는 항상 padding) */
	*dst++ = (src_len == 3) ? enc_table[(raw_u32 >> 8) & BASE64_ENC_BITMASK] : BASE64_PADDING_CHAR;

out:
	/* [한국어] C 문자열 종료 — 호출자가 그대로 strlen/printf 가능하도록. */
	*dst = '\0';

	return 0;
}

/*
 * [한국어]
 * spdk_base64_encode - 표준 Base64 인코더.
 *
 * 호출 체인: RPC 응답 직렬화 등 → spdk_base64_encode → base64_encode (table=표준).
 */
int
spdk_base64_encode(char *dst, const void *src, size_t src_len)
{
	/* [한국어] 표준 알파벳으로 위임. */
	return base64_encode(dst, base64_enc_table, src, src_len);
}

/*
 * [한국어]
 * spdk_base64_urlsafe_encode - URL-safe 변종 Base64 인코더.
 */
int
spdk_base64_urlsafe_encode(char *dst, const void *src, size_t src_len)
{
	/* [한국어] URL-safe 알파벳으로 위임 — '+' → '-', '/' → '_'. */
	return base64_encode(dst, base64_urlsafe_enc_table, src, src_len);
}

/*
 * [한국어]
 * base64_decode (static, 공통) - 디코딩 코어.
 *
 * @dst:       출력. NULL 이면 길이만 계산 모드(_dst_len 만 채우고 0 반환).
 * @_dst_len:  출력 길이 저장 포인터(옵션).
 * @dec_table: ASCII → 6비트 매핑 테이블.
 * @dec_table_opt: ARM NEON 가속용 보조 테이블(NEON 빌드에서만).
 * @src:       NUL 종료 Base64 문자열.
 * @return:    0 성공, -EINVAL 형식 오류.
 *
 * 알고리즘:
 *   - 입력 strlen 이 4의 배수여야 함(패딩 포함).
 *   - 끝에 최대 2 개의 '=' padding 을 제거 후 본문 길이 결정.
 *   - 본문 길이 % 4 == 1 은 불가능한 길이라 거절.
 *   - 4문자 그룹마다 dec_table 로 6비트 4개 추출 → 24비트 합성 → 3바이트 출력.
 *     마지막 그룹은 길이에 따라 1 or 2 or 3 바이트 출력.
 */
#if defined(__aarch64__) && !defined(__ARM_FEATURE_SVE)
static int
base64_decode(void *dst, size_t *_dst_len, const uint8_t *dec_table,
	      const uint8_t *dec_table_opt, const char *src)
#else
static int
base64_decode(void *dst, size_t *_dst_len, const uint8_t *dec_table, const char *src)
#endif
{
	/* [한국어] 입력 문자열 길이 (패딩 포함). */
	size_t src_strlen;
	/* [한국어] tail 그룹에서 실제로 출력할 바이트 수(1~3). */
	size_t tail_len = 0;
	/* [한국어] 디코딩 진행 포인터(uint8_t* 캐스팅 — dec_table 인덱싱용). */
	const uint8_t *src_in;
	/* [한국어] 4문자 → 4개의 6비트 값 buffer. tmp[3] 은 메인 루프에서 dst 에
	 * 직접 to_be32 로 쓰일 때 사용된다. */
	uint32_t tmp[4];
	int i;

	/* [한국어] NULL 포인터 거절. */
	if (!src) {
		return -EINVAL;
	}

	/* [한국어] NUL 종료 문자열 길이 측정. */
	src_strlen = strlen(src);

	/* strlen of src should be 4n */
	/* [한국어] Base64 인코딩 결과는 항상 4의 배수 길이. 빈 입력도 거절. */
	if (src_strlen == 0 || src_strlen % 4 != 0) {
		return -EINVAL;
	}

	/* Consider Base64 padding, it at most has 2 padding characters. */
	/* [한국어] 끝의 '=' padding 을 최대 2개까지 제거해 "본문" 길이 산출. */
	for (i = 0; i < 2; i++) {
		if (src[src_strlen - 1] != BASE64_PADDING_CHAR) {
			break;
		}
		src_strlen--;
	}

	/* strlen of src without padding shouldn't be 4n+1 */
	/* [한국어] 패딩 제거 후 길이 % 4 == 1 은 불가능 — 인코딩 규칙상 발생할 수 없음. */
	if (src_strlen == 0 || src_strlen % 4 == 1) {
		return -EINVAL;
	}

	/* [한국어] 길이만 알면 되는 호출자에게는 미리 계산해서 채워줌. */
	if (_dst_len) {
		*_dst_len = spdk_base64_get_decoded_len(src_strlen);
	}

	/* If dst is NULL, the client is only concerned w/ _dst_len, return */
	/* [한국어] dst 가 NULL 이면 길이만 알려주고 종료 (사이징 쿼리). */
	if (!dst) {
		return 0;
	}

	/* [한국어] dec_table 인덱싱을 위해 unsigned 로 캐스팅 (음수 ASCII 문제 방지). */
	src_in = (const uint8_t *) src;

#ifdef __aarch64__
#ifdef __ARM_FEATURE_SVE
	/* [한국어] SVE 가속 — 처리한 만큼 src_in/src_strlen 갱신. */
	base64_decode_sve(&dst, dec_table, &src_in, &src_strlen);
#else
	/* [한국어] NEON 가속 — 보조 테이블(_opt)을 함께 사용. */
	base64_decode_neon64(&dst, dec_table_opt, &src_in, &src_strlen);
#endif

	/* [한국어] SIMD 가 모두 처리했다면 나머지 스칼라 루프는 건너뜀. */
	if (src_strlen == 0) {
		return 0;
	}
#endif


	/* space of dst can be used by to_be32 */
	/* [한국어] 메인 스칼라 루프 — 마지막 그룹을 제외한 모든 4문자 그룹 처리.
	 * dst 에 to_be32 로 직접 쓰면 4바이트가 쓰이는데, 그중 마지막 1바이트는
	 * 다음 반복에서 덮어써지므로 안전. 단 마지막 그룹은 to_be32 가 dst 경계를
	 * 넘을 수 있어 별도 분기로 처리한다. */
	while (src_strlen > 4) {
		/* [한국어] 4 문자 → 4 × 6비트 값으로 디코드. */
		tmp[0] = dec_table[*src_in++];
		tmp[1] = dec_table[*src_in++];
		tmp[2] = dec_table[*src_in++];
		tmp[3] = dec_table[*src_in++];

		/* [한국어] 어느 자리든 invalid(255) 가 있으면 즉시 거절. */
		if (tmp[0] == 255 || tmp[1] == 255 || tmp[2] == 255 || tmp[3] == 255) {
			return -EINVAL;
		}

		/* [한국어] 4×6비트(=24비트)를 32비트 빅엔디안으로 합쳐 dst 의 시작 주소에 기록.
		 * 비트 배치: tmp[0]<<26 | tmp[1]<<20 | tmp[2]<<14 | tmp[3]<<8 → 상위 24비트가 의미.
		 * to_be32 는 호스트→BE 변환 후 4바이트 기록 → dst[0..2] 가 원래 3바이트가 됨. */
		to_be32(dst, tmp[3] << 8 | tmp[2] << 14 | tmp[1] << 20 | tmp[0] << 26);

		/* [한국어] 출력은 3바이트만 진행 — 4번째 바이트는 다음 그룹이 덮어씀. */
		dst = (uint8_t *)dst + 3;
		src_strlen -= 4;
	}

	/* space of dst is not enough to be used by to_be32 */
	/* [한국어] 마지막 그룹 처리 — dst 가 4바이트 미만일 수 있어 별도 처리.
	 * src_strlen 은 2,3,4 중 하나(1 은 위에서 거절). */
	tmp[0] = dec_table[src_in[0]];
	tmp[1] = dec_table[src_in[1]];
	/* [한국어] 3번째 자리는 길이 ≥3 인 경우만 의미 — 부족하면 0 으로 채움. */
	tmp[2] = (src_strlen >= 3) ? dec_table[src_in[2]] : 0;
	/* [한국어] 4번째 자리는 길이 == 4 인 경우만 의미. */
	tmp[3] = (src_strlen == 4) ? dec_table[src_in[3]] : 0;
	/* [한국어] 출력 바이트 수 = 본문 문자수 - 1 (4→3, 3→2, 2→1). */
	tail_len = src_strlen - 1;

	/* [한국어] tail 에도 invalid 검사. */
	if (tmp[0] == 255 || tmp[1] == 255 || tmp[2] == 255 || tmp[3] == 255) {
		return -EINVAL;
	}

	/* [한국어] 메인 루프와 동일한 비트 배치를 tmp[3] 에 기록(to_be32 in-place),
	 * 결과는 빅엔디안으로 정렬되므로 앞에서부터 tail_len 바이트만 dst 에 복사. */
	to_be32(&tmp[3], tmp[3] << 8 | tmp[2] << 14 | tmp[1] << 20 | tmp[0] << 26);
	memcpy(dst, (uint8_t *)&tmp[3], tail_len);

	return 0;
}

/*
 * [한국어]
 * spdk_base64_decode - 표준 Base64 디코더 외부 API.
 *
 * @dst:     출력 binary 버퍼 (NULL 이면 길이만 계산).
 * @dst_len: 출력 길이 포인터 (옵션).
 * @src:     NUL 종료 Base64 문자열.
 *
 * NEON 빌드에서는 SIMD 가속용 dec_table_neon64 보조 테이블을 추가로 전달한다.
 */
int
spdk_base64_decode(void *dst, size_t *dst_len, const char *src)
{
#if defined(__aarch64__) && !defined(__ARM_FEATURE_SVE)
	/* [한국어] NEON 빌드: 보조 테이블 동반 호출. */
	return base64_decode(dst, dst_len, base64_dec_table, base64_dec_table_neon64, src);
#else
	/* [한국어] 일반 빌드: 표준 디코딩 테이블만으로 충분. */
	return base64_decode(dst, dst_len, base64_dec_table, src);
#endif
}

/*
 * [한국어]
 * spdk_base64_urlsafe_decode - URL-safe 변종 디코더 외부 API.
 */
int
spdk_base64_urlsafe_decode(void *dst, size_t *dst_len, const char *src)
{
#if defined(__aarch64__) && !defined(__ARM_FEATURE_SVE)
	/* [한국어] NEON: URL-safe 보조 테이블 동반. */
	return base64_decode(dst, dst_len, base64_urlsafe_dec_table, base64_urlsafe_dec_table_neon64,
			     src);
#else
	/* [한국어] 일반: URL-safe 표준 테이블만 사용. */
	return base64_decode(dst, dst_len, base64_urlsafe_dec_table, src);
#endif
}
