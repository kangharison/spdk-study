/*   SPDX-License-Identifier: BSD-2-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2005-2007, Nick Galbreath
 *   Copyright (c) 2013-2017, Alfred Klomp
 *   Copyright (c) 2015-2017, Wojciech Mula
 *   Copyright (c) 2016-2017, Matthieu Darbois
 *   All rights reserved.
 */

/*
 * [한국어 설명] ARMv8 NEON SIMD를 이용한 base64 인코더/디코더 (base64_neon.c)
 *
 * === 파일의 역할 ===
 * base64 변환을 ARM 64비트(aarch64)의 128비트 NEON SIMD 인스트럭션으로 가속한다.
 * 표준 base64 알파벳과 URL-safe 알파벳을 모두 지원하며, 16 바이트 단위로 NEON 레지스터를
 * 채워 한 번에 다수의 6비트 단위/8비트 단위 변환을 처리한다. 인코드는 입력 48바이트 →
 * 출력 64바이트, 디코드는 입력 64바이트 → 출력 48바이트 단위로 동작하고, 잔여(non-multiple)
 * 바이트는 호출자(상위 레이어 base64.c)가 스칼라 폴백으로 처리한다. SPDK가 RPC,
 * Blobstore의 키, NVMe-oF host NQN 등에서 base64 인코딩이 필요할 때 사용된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK util 라이브러리 → base64 가속 백엔드(ARM 전용).
 * 호출 체인:
 *   사용자 코드(예: RPC handler) → spdk_base64_encode/decode (lib/util/base64.c 공개 API)
 *     → __aarch64__ 빌드 시 base64_encode_neon64 / base64_decode_neon64 (이 파일)
 *     → 잔여 바이트는 base64.c 내부 스칼라 루프로 마무리
 * 실행 컨텍스트: 호스트 유저스페이스, 임의의 SPDK reactor 또는 일반 스레드.
 * 순수 계산 함수이며 I/O나 동기화 프리미티브를 사용하지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h(uint8_t 등), arm_neon.h(NEON 인트린식).
 * - 의존하는 자: lib/util/base64.c가 컴파일 시 __aarch64__ 정의 시 이 파일의
 *   static 함수들을 인클루드하거나 동일 단위로 빌드해 사용한다.
 * - 데이터 흐름:
 *   인코드: 사용자 바이트 입력 → 48B 청크 로딩(vld3q_u8 deinterleave) → 6비트 4그룹 분해
 *           → 64엔트리 인코딩 알파벳 LUT 검색(vqtbl4q_u8) → 64B 인터리브 저장(vst4q_u8)
 *   디코드: base64 64B 청크 로딩(vld4q_u8) → 7비트/8비트 LUT 두 단계로 0..63 또는 255 매핑
 *           → 유효성 검사(>63 인 바이트가 있으면 실패) → 4×6비트 → 3×8비트 패킹 → 48B 저장.
 *
 * === 주요 함수/구조체 요약 ===
 * - base64_dec_table_neon64[128]         : 표준 base64 디코드 LUT(전반/후반 64B 두 부분).
 * - base64_urlsafe_dec_table_neon64[128] : URL-safe(-, _) 변형 디코드 LUT.
 * - load_64byte_table()                  : 64B 테이블을 4개 NEON 레지스터(uint8x16x4_t)로 로드.
 * - base64_encode_neon64()               : 48B → 64B 인코드 메인 루프.
 * - base64_decode_neon64()               : 64B → 48B 디코드 메인 루프(검증 포함).
 *
 * === 알고리즘 핵심 (인코드) ===
 * 입력 3바이트(24비트)를 상위→하위 6비트씩 4분할하여 0..63 인덱스 4개로 만든 뒤
 * 64엔트리 알파벳 테이블에서 출력 ASCII 4문자를 룩업한다. NEON에서는
 * vld3q_u8/vst4q_u8 가 16배 SIMD로 deinterleave/interleave 를 수행해 16개 입력
 * 트리플(48B)을 한 번에 변환한다.
 *
 * === 알고리즘 핵심 (디코드) ===
 * base64 ASCII 7비트는 0..127 범위인데, NEON vqtbl4q_u8 한 인스트럭션은
 * 64엔트리만 룩업 가능하므로 [0..63]용 LUT(vqtbl4q_u8)와 [64..127]용 LUT(vqtbx4q_u8)
 * 두 단계로 나누어 결과를 OR 합성한다. 잘못된 입력 문자는 LUT가 0xFF를 반환하므로
 * 결과 벡터에서 max(>63) 검사로 검출하여 즉시 break, 호출자에 폴백 처리를 위임한다.
 */

#ifndef __aarch64__
/* [한국어] 컴파일러가 aarch64 타깃이 아니면 컴파일을 즉시 중단(이 파일은
 * NEON 인트린식 사용을 전제로 한다). 빌드 시스템은 별도의 base64_*.c를 선택. */
#error Unsupported hardware
#endif

#include "spdk/stdinc.h"
/* [한국어] uint8_t, size_t, NULL 등 표준 타입과 매크로 가져오기. */
/*
 * Encoding
 * Use a 64-byte lookup to do the encoding.
 * Reuse existing base64_dec_table and base64_dec_table.

 * Decoding
 * The input consists of five valid character sets in the Base64 alphabet,
 * which we need to map back to the 6-bit values they represent.
 * There are three ranges, two singles, and then there's the rest.
 *
 * LUT1[0-63] = base64_dec_table_neon64[0-63]
 * LUT2[0-63] = base64_dec_table_neon64[64-127]
 *   #  From       To        LUT  Characters
 *   1  [0..42]    [255]      #1  invalid input
 *   2  [43]       [62]       #1  +
 *   3  [44..46]   [255]      #1  invalid input
 *   4  [47]       [63]       #1  /
 *   5  [48..57]   [52..61]   #1  0..9
 *   6  [58..63]   [255]      #1  invalid input
 *   7  [64]       [255]      #2  invalid input
 *   8  [65..90]   [0..25]    #2  A..Z
 *   9  [91..96]   [255]      #2 invalid input
 *  10  [97..122]  [26..51]   #2  a..z
 *  11  [123..126] [255]      #2 invalid input
 * (12) Everything else => invalid input
 */
/* [한국어] 표준 base64 알파벳용 디코드 룩업 테이블(128 엔트리, 두 LUT 합).
 * 설정자: 본 파일에서 정적 상수로 한 번 정의되며, 변경되지 않는 read-only 데이터.
 * 읽는 자: base64_decode_neon64()가 load_64byte_table()로 16바이트 4벡터에 적재해 사용.
 * 동기화: const 정적 데이터이므로 멀티스레드에서 락 없이 안전하게 공유.
 * 매핑: ASCII 코드 → 6비트 값(0..63), 잘못된 문자는 255(0xFF)로 매핑되어
 * 디코딩 단계에서 invalid 검출에 사용된다. 위 주석의 '#1', '#2' 표는 두 LUT
 * (앞 64B/뒤 64B)을 어떻게 vqtbl/vqtbx로 합쳐서 [0..127]을 모두 다루는지 설명한다. */
static const uint8_t base64_dec_table_neon64[] = {
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,  62, 255, 255, 255,  63,
	52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255, 255, 255, 255, 255,
	0, 255,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,
	14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255,
	255, 255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,
	40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51, 255, 255, 255, 255
};

/*
 * LUT1[0-63] = base64_urlsafe_dec_table_neon64[0-63]
 * LUT2[0-63] = base64_urlsafe_dec_table_neon64[64-127]
 *   #  From       To        LUT  Characters
 *   1  [0..44]    [255]      #1  invalid input
 *   2  [45]       [62]       #1  -
 *   3  [46..47]   [255]      #1  invalid input
 *   5  [48..57]   [52..61]   #1  0..9
 *   6  [58..63]   [255]      #1  invalid input
 *   7  [64]       [255]      #2  invalid input
 *   8  [65..90]   [0..25]    #2  A..Z
 *   9  [91..94]   [255]      #2  invalid input
 *  10  [95]       [63]       #2  _
 *  11  [96]       [255]      #2  invalid input
 *  12  [97..122]  [26..51]   #2  a..z
 *  13  [123..126] [255]      #2 invalid input
 * (14) Everything else => invalid input
 */
/* [한국어] URL-safe base64 알파벳용 디코드 룩업 테이블(128 엔트리).
 * 표준 base64와의 차이: '+' → '-' (0x2B → 0x2D), '/' → '_' (0x2F → 0x5F)으로 치환.
 * 설정자/읽는 자/동기화: 위 표준 LUT와 동일.
 * 사용처: URL 쿼리/JWT 등 + / =가 곤란한 환경에서 spdk_base64_urlsafe_*로 호출될 때. */
static const uint8_t base64_urlsafe_dec_table_neon64[] = {
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,  62, 255, 255,
	52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255, 255, 255, 255, 255,
	0, 255,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,
	14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255,
	63, 255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,
	40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51, 255, 255, 255, 255
};

#include <arm_neon.h>
/* [한국어] ARM NEON 인트린식(uint8x16_t, vld3q_u8, vqtbl4q_u8 등) 선언 가져오기. */
#define CMPGT(s,n)      vcgtq_u8((s), vdupq_n_u8(n))
/* [한국어] 매크로: 16바이트 벡터 s의 각 레인에 대해 (lane > n) 비교 결과를 0xFF/0x00으로 반환.
 * vdupq_n_u8(n)은 n을 16개로 복제한 벡터를 만들고, vcgtq_u8는 unsigned greater-than 비교. */

/*
 * [한국어]
 * load_64byte_table - 64바이트 LUT를 4개의 NEON 16바이트 벡터에 적재한다.
 *
 * @p:      64바이트 정렬되지 않을 수도 있는 LUT 시작 주소(읽기 전용).
 * @return: uint8x16x4_t - 4개 벡터로 분할된 LUT (val[0]: 0..15, val[1]: 16..31, ...).
 *
 * 64엔트리 LUT는 vqtbl4q_u8 인스트럭션의 인덱스 범위(0..63)에 정확히 대응되어
 * 한 인스트럭션으로 임의 인덱스의 바이트를 룩업할 수 있게 된다.
 * 호출 체인: base64_encode_neon64 / base64_decode_neon64 → [load_64byte_table]
 * 실행 컨텍스트: 같은 스레드 내 인라인 호출. SIMD 외 부수 효과 없음.
 */
static inline uint8x16x4_t
load_64byte_table(const uint8_t *p)
{
	uint8x16x4_t ret;
	/* [한국어] 4개의 16바이트 NEON 레지스터를 묶은 컴파운드 타입 반환 객체. */
	ret.val[0] = vld1q_u8(p +  0);
	/* [한국어] LUT의 [0..15] 영역을 첫 벡터에 적재(unaligned load 허용). */
	ret.val[1] = vld1q_u8(p + 16);
	/* [한국어] LUT [16..31]을 두 번째 벡터에 적재. */
	ret.val[2] = vld1q_u8(p + 32);
	/* [한국어] LUT [32..47]을 세 번째 벡터에 적재. */
	ret.val[3] = vld1q_u8(p + 48);
	/* [한국어] LUT [48..63]을 네 번째 벡터에 적재 - 64B를 모두 SIMD 레지스터에 상주. */
	return ret;
	/* [한국어] 4벡터 묶음을 호출자에게 반환 - 레지스터 호출 규약으로 직접 전달됨. */
}

/*
 * [한국어]
 * base64_encode_neon64 - NEON SIMD로 입력 48바이트씩 base64 64바이트로 인코딩.
 *
 * @dst:       출력 포인터의 포인터(호출자 변수). 인코딩한 만큼 전진시킨다.
 * @enc_table: 64엔트리 인코딩 알파벳(0..63 → ASCII 매핑)의 시작 주소.
 *             표준 'A-Za-z0-9+/' 또는 URL-safe 'A-Za-z0-9-_'.
 * @src:       입력 포인터의 포인터. 처리한 만큼 전진.
 * @src_len:   남은 입력 길이의 포인터. 처리한 만큼 차감.
 * @return:    없음. 잔여(<48바이트)는 호출자(spdk_base64_encode 스칼라 부분)가 처리.
 *
 * 처리 단위: 48B 입력 → 64B 출력. NEON 16-way SIMD로 16개 트리플(=48B)을 한 번에 변환.
 * 단계:
 *  1) vld3q_u8: 48B를 b0/b1/b2 세 갈래의 16바이트 벡터로 deinterleave.
 *  2) 비트 시프트와 OR로 b0|b1|b2 24비트를 6비트 4분할 → res.val[0..3].
 *  3) vqtbl4q_u8 (64엔트리 LUT 룩업)로 0..63 → ASCII 변환.
 *  4) vst4q_u8: 4개 결과 벡터를 인터리브하며 64B 출력 저장.
 * 호출 체인: spdk_base64_encode → [base64_encode_neon64] → 잔여는 스칼라 처리.
 * 실행 컨텍스트: 같은 스레드, 부수 효과 없음(메모리 접근만).
 */
static void
base64_encode_neon64(char **dst, const char *enc_table, const void **src, size_t *src_len)
{
	const uint8x16x4_t tbl_enc = load_64byte_table(enc_table);
	/* [한국어] 인코딩 LUT(64B)를 NEON 4벡터에 한 번 적재해두고 루프 내내 재사용. */

	while (*src_len >= 48) {
		/* [한국어] SIMD 단위(48B)가 안 차면 루프 종료 - 잔여는 호출자가 스칼라로 처리. */
		uint8x16x3_t str;
		/* [한국어] 입력을 deinterleave한 3개 16B 벡터(b0/b1/b2)를 담을 컴파운드 타입. */
		uint8x16x4_t res;
		/* [한국어] 출력 4개 16B 벡터(c0/c1/c2/c3 = 4 × 16 = 64B)를 담을 결과. */

		/* Load 48 bytes and deinterleave */
		str = vld3q_u8((uint8_t *)*src);
		/* [한국어] 48B 입력을 b0[0..15], b1[0..15], b2[0..15] 형태로 deinterleave 적재.
		 * vld3q_u8은 SIMD 친화적 트리플 인터리브 로드 인스트럭션. */

		/* Divide bits of three input bytes over four output bytes and clear top two bits */
		res.val[0] = vshrq_n_u8(str.val[0], 2);
		/* [한국어] 출력1 = b0의 상위 6비트(b0 >> 2). 0..63 범위로 강제됨. */
		res.val[1] = vandq_u8(vorrq_u8(vshrq_n_u8(str.val[1], 4), vshlq_n_u8(str.val[0], 4)),
				      vdupq_n_u8(0x3F));
		/* [한국어] 출력2 = ((b0 << 4) | (b1 >> 4)) & 0x3F.
		 * b0의 하위 2비트와 b1의 상위 4비트를 합쳐 6비트 인덱스 생성. */
		res.val[2] = vandq_u8(vorrq_u8(vshrq_n_u8(str.val[2], 6), vshlq_n_u8(str.val[1], 2)),
				      vdupq_n_u8(0x3F));
		/* [한국어] 출력3 = ((b1 << 2) | (b2 >> 6)) & 0x3F.
		 * b1의 하위 4비트와 b2의 상위 2비트를 합쳐 6비트 인덱스. */
		res.val[3] = vandq_u8(str.val[2], vdupq_n_u8(0x3F));
		/* [한국어] 출력4 = b2 & 0x3F. b2의 하위 6비트가 마지막 인덱스. */

		/*
		 * The bits have now been shifted to the right locations;
		 * translate their values 0..63 to the Base64 alphabet.
		 * Use a 64-byte table lookup:
		 */
		res.val[0] = vqtbl4q_u8(tbl_enc, res.val[0]);
		/* [한국어] 0..63 → ASCII 알파벳 변환(LUT 룩업). 인덱스가 64 이상이면 0 반환되지만
		 * 위에서 0x3F 마스킹으로 보장되어 있다. */
		res.val[1] = vqtbl4q_u8(tbl_enc, res.val[1]);
		/* [한국어] 두 번째 6비트 그룹 LUT 룩업. */
		res.val[2] = vqtbl4q_u8(tbl_enc, res.val[2]);
		/* [한국어] 세 번째 6비트 그룹 LUT 룩업. */
		res.val[3] = vqtbl4q_u8(tbl_enc, res.val[3]);
		/* [한국어] 네 번째 6비트 그룹 LUT 룩업. */

		/* Interleave and store result */
		vst4q_u8((uint8_t *)*dst, res);
		/* [한국어] 4개 벡터를 c0/c1/c2/c3 인터리브하며 64B 연속 저장. base64 출력 순서가
		 * 자연스럽게 만들어진다. */

		*src = (uint8_t *)*src + 48;	/* 3 * 16 bytes of input */
		/* [한국어] 입력 포인터를 SIMD 단위(48B = 3 × 16)만큼 전진. */
		*dst += 64;			/* 4 * 16 bytes of output */
		/* [한국어] 출력 포인터를 SIMD 단위(64B = 4 × 16)만큼 전진. */
		*src_len -= 48;
		/* [한국어] 남은 입력 길이를 48B 차감, 다음 반복에서 충분 여부 재확인. */
	}
}

/*
 * [한국어]
 * base64_decode_neon64 - NEON SIMD로 base64 64바이트씩 입력을 8비트 48바이트로 디코드.
 *
 * @dst:                출력 포인터의 포인터(원본 바이너리 출력). 처리한 만큼 전진.
 * @dec_table_neon64:   128엔트리 디코딩 LUT(0..127 ASCII → 6비트 값, invalid는 0xFF).
 * @src:                base64 ASCII 입력 포인터. 처리한 만큼 전진.
 * @src_len:            남은 입력 길이의 포인터.
 * @return:             없음. 잘못된 문자가 발견되면 break하고 잔여를 호출자에 위임.
 *
 * NEON의 vqtbl4q_u8는 [0..63] 인덱스만 룩업하므로 ASCII 7비트 [0..127]을 두 LUT으로 분할.
 *  - 1차(tbl_dec1, vqtbl4q_u8): 인덱스 0..63 (0..63은 그대로, 64..255는 0으로 클리어).
 *  - 2차(tbl_dec2, vqtbx4q_u8): 인덱스를 -63 saturating으로 옮긴 후 룩업.
 *    vqtbx는 인덱스 0이면 destination 유지 → 1차 결과를 보존.
 * 두 단계 결과를 OR 합쳐서 최종 0..63 또는 0xFF(invalid)를 얻는다.
 *
 * 호출 체인: spdk_base64_decode → [base64_decode_neon64] → 잔여/패딩 처리는 스칼라.
 * 실행 컨텍스트: 같은 스레드, 부수 효과 없음.
 */
static void
base64_decode_neon64(void **dst, const uint8_t *dec_table_neon64, const uint8_t **src,
		     size_t *src_len)
{
	/*
	 * First LUT tbl_dec1 will use VTBL instruction (out of range indices are set to 0 in destination).
	 * Second LUT tbl_dec2 will use VTBX instruction (out of range indices will be unchanged in destination).
	 * Input [64..126] will be mapped to index [1..63] in tb1_dec2. Index 0 means that value comes from tb1_dec1.
	 */
	const uint8x16x4_t tbl_dec1 = load_64byte_table(dec_table_neon64);
	/* [한국어] LUT 첫 64B(=ASCII [0..63] 영역)를 NEON 4벡터에 적재. */
	const uint8x16x4_t tbl_dec2 = load_64byte_table(dec_table_neon64 + 64);
	/* [한국어] LUT 둘째 64B(=ASCII [64..127] 영역)를 NEON 4벡터에 적재. */
	const uint8x16_t offset = vdupq_n_u8(63U);
	/* [한국어] 16개 레인이 모두 63인 상수 벡터. tbl_dec2 인덱싱을 위해
	 * (입력 - 63) saturating sub로 [64..127] → [1..64] 변환 시 사용. */

	while (*src_len >= 64) {
		/* [한국어] 64B 단위 SIMD 처리. 모자라면 루프 종료. */

		uint8x16x4_t dec1, dec2;
		/* [한국어] 두 LUT 룩업 결과를 담을 4벡터 컴파운드. */
		uint8x16x3_t dec;
		/* [한국어] 4×6비트(=24비트)를 3×8비트로 패킹한 출력 3벡터(=48B). */

		/* Load 64 bytes and deinterleave */
		uint8x16x4_t str = vld4q_u8((uint8_t *)*src);
		/* [한국어] 64B base64 입력을 c0/c1/c2/c3 4벡터로 deinterleave 로드. */

		/* Get indices for 2nd LUT */
		dec2.val[0] = vqsubq_u8(str.val[0], offset);
		/* [한국어] 입력 - 63 (포화 감산). [64..127]은 [1..64], [0..63]은 0이 된다.
		 * 이는 tbl_dec2 인덱싱을 위함. */
		dec2.val[1] = vqsubq_u8(str.val[1], offset);
		/* [한국어] 두 번째 채널에 대한 같은 처리. */
		dec2.val[2] = vqsubq_u8(str.val[2], offset);
		/* [한국어] 세 번째 채널. */
		dec2.val[3] = vqsubq_u8(str.val[3], offset);
		/* [한국어] 네 번째 채널. */

		/* Get values from 1st LUT */
		dec1.val[0] = vqtbl4q_u8(tbl_dec1, str.val[0]);
		/* [한국어] 1차 LUT 룩업: 인덱스가 64 이상이면 0 반환(범위 외).
		 * 결과는 [0..63] 영역의 ASCII에 해당하는 디코드값(또는 invalid면 0xFF). */
		dec1.val[1] = vqtbl4q_u8(tbl_dec1, str.val[1]);
		/* [한국어] 동일 처리(채널 2). */
		dec1.val[2] = vqtbl4q_u8(tbl_dec1, str.val[2]);
		/* [한국어] 동일 처리(채널 3). */
		dec1.val[3] = vqtbl4q_u8(tbl_dec1, str.val[3]);
		/* [한국어] 동일 처리(채널 4). */

		/* Get values from 2nd LUT */
		dec2.val[0] = vqtbx4q_u8(dec2.val[0], tbl_dec2, dec2.val[0]);
		/* [한국어] vqtbx4: 인덱스가 64 이상이면 destination(첫 인자) 유지, 아니면 LUT 룩업.
		 * 위에서 dec2.val[0]은 [0..63] 영역에서 0이고 [64..127]은 [1..64]였으므로,
		 * 결과: [0..63] 입력은 0(=중립), [64..127] 입력은 tbl_dec2의 디코드값. */
		dec2.val[1] = vqtbx4q_u8(dec2.val[1], tbl_dec2, dec2.val[1]);
		/* [한국어] 채널 2. */
		dec2.val[2] = vqtbx4q_u8(dec2.val[2], tbl_dec2, dec2.val[2]);
		/* [한국어] 채널 3. */
		dec2.val[3] = vqtbx4q_u8(dec2.val[3], tbl_dec2, dec2.val[3]);
		/* [한국어] 채널 4. */

		/* Get final values */
		str.val[0] = vorrq_u8(dec1.val[0], dec2.val[0]);
		/* [한국어] 1차+2차 결과 OR 합성: [0..63]은 dec1, [64..127]은 dec2가 0이 아닌 값을 제공.
		 * 결과는 0..63 (유효) 또는 0xFF (invalid). */
		str.val[1] = vorrq_u8(dec1.val[1], dec2.val[1]);
		/* [한국어] 채널 2 합성. */
		str.val[2] = vorrq_u8(dec1.val[2], dec2.val[2]);
		/* [한국어] 채널 3 합성. */
		str.val[3] = vorrq_u8(dec1.val[3], dec2.val[3]);
		/* [한국어] 채널 4 합성. */

		/* Check for invalid input, any value larger than 63 */
		uint8x16_t classified = CMPGT(str.val[0], 63);
		/* [한국어] 채널 1에서 63보다 큰 레인(=invalid 0xFF)이 있으면 해당 레인은 0xFF. */
		classified = vorrq_u8(classified, CMPGT(str.val[1], 63));
		/* [한국어] 채널 2 검사 결과를 OR 누적. */
		classified = vorrq_u8(classified, CMPGT(str.val[2], 63));
		/* [한국어] 채널 3 검사 결과 누적. */
		classified = vorrq_u8(classified, CMPGT(str.val[3], 63));
		/* [한국어] 채널 4 검사 결과 누적. */

		/* check that all bits are zero */
		if (vmaxvq_u8(classified) != 0U) {
			/* [한국어] vmaxvq_u8: 16레인 중 최댓값. 0이 아니면 어딘가에 invalid가 있다는 뜻.
			 * SIMD 단계에서 실패 시 즉시 break, 호출자(스칼라)가 실제 위치 식별/패딩 처리. */
			break;
		}

		/* Compress four bytes into three */
		dec.val[0] = vorrq_u8(vshlq_n_u8(str.val[0], 2), vshrq_n_u8(str.val[1], 4));
		/* [한국어] out0 = (c0 << 2) | (c1 >> 4): 첫 6비트와 둘째 6비트의 상위 2비트로 8비트 1바이트. */
		dec.val[1] = vorrq_u8(vshlq_n_u8(str.val[1], 4), vshrq_n_u8(str.val[2], 2));
		/* [한국어] out1 = (c1 << 4) | (c2 >> 2): 둘째 6비트의 하위 4비트와 셋째 6비트의 상위 4비트. */
		dec.val[2] = vorrq_u8(vshlq_n_u8(str.val[2], 6), str.val[3]);
		/* [한국어] out2 = (c2 << 6) | c3: 셋째 6비트의 하위 2비트와 넷째 6비트 전체로 8비트. */

		/* Interleave and store decoded result */
		vst3q_u8((uint8_t *)*dst, dec);
		/* [한국어] 3벡터(= 16 트리플)를 인터리브하며 48B 연속 저장 - 자연스러운 바이트 순서. */

		*src += 64;
		/* [한국어] 입력 포인터 64B 전진. */
		*dst = (uint8_t *)*dst + 48;
		/* [한국어] 출력 포인터 48B 전진. */
		*src_len -= 64;
		/* [한국어] 남은 입력 길이 64B 차감. */
	}
}
