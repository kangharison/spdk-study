/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] N개의 source 버퍼를 XOR해 dest에 저장하는 RAID 헬퍼 (xor.c)
 *
 * === 파일의 역할 ===
 * RAID-5/RAID-6의 패리티 계산이나 일반적 erasure-coding의 기본 연산인
 * "여러 source 버퍼를 비트 XOR해 한 개 dest 버퍼에 쓰기"를 수행한다.
 * `spdk_xor_gen(dest, sources, n, len)` 한 함수가 외부 인터페이스이며,
 * 빌드 환경에 따라:
 *   - ISA-L의 어셈블리 가속 `xor_gen()`(32B 정렬 입력, AVX/SSE 활용),
 *   - 또는 자체 64비트 단어/바이트 단위 폴백을 자동 선택한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 RAID-5/0+1 등 module/bdev/raid 모듈이 stripe 패리티 계산 시 본 함수
 * 호출. 호출 흐름:
 *   bdev_raid I/O 발행 → strip-별 데이터 모음 → spdk_xor_gen(parity, strips,
 *   N, strip_len) → parity bdev 모듈로 write submit.
 * 또한 erasure-coded blob 등의 데이터 복구 경로에서도 사용 가능.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `spdk/xor.h`(prototype), `spdk/config.h`(SPDK_CONFIG_ISAL),
 *   `spdk/assert.h`(SPDK_STATIC_ASSERT), `spdk/util.h`(spdk_u32log2,
 *   SPDK_ALIGN_FLOOR). ISA-L 빌드 시 isa-l/include/raid.h(xor_gen).
 * - 호출자: module/bdev/raid 등.
 * - 공유 상태: 없음. 모든 데이터는 호출자 소유.
 *
 * === 주요 함수/구조체 요약 ===
 * - is_aligned(ptr, alignment): 포인터의 alignment 정렬 여부 검사.
 * - buffers_aligned(dest, sources, n, alignment): 모든 buffer의 정렬 검사.
 * - xor_gen_unaligned: 1B 단위 XOR 폴백.
 * - xor_gen_basic: 8B(uint64_t) 단위 XOR + 끝 잔여를 1B 단위 처리.
 * - do_xor_gen: ISA-L 빌드 분기로 가장 빠른 경로 선택.
 * - spdk_xor_gen: 외부 진입점. n 범위 검증.
 * - spdk_xor_get_optimal_alignment: 호출자가 정렬 hint를 받기 위한 API.
 * - SPDK_XOR_BUF_ALIGN: ISA-L 빌드 시 32, 그 외 8(uint64_t).
 */

#include "spdk/xor.h"
/* [한국어] spdk_xor_gen 등 공개 prototype. */
#include "spdk/config.h"
/* [한국어] SPDK_CONFIG_ISAL 매크로 — configure 시 ISA-L 라이브러리 발견 여부. */
#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT — 컴파일 타임 단언. */
#include "spdk/util.h"
/* [한국어] spdk_u32log2(2의 거듭제곱 shift 양 계산), SPDK_ALIGN_FLOOR
 * (지정 정렬로 floor)와 같은 일반 헬퍼. */

/* maximum number of source buffers */
#define SPDK_XOR_MAX_SRC	256
/* [한국어] 한 번 호출에서 XOR할 수 있는 source 버퍼 최대 개수.
 * RAID stripe width의 합리적 상한이며, 스택에 보조 배열을 잡을 때 안전 한계.
 * (RAID-5에서는 N=stripe data block 수.) */

/*
 * [한국어]
 * is_aligned - 포인터가 alignment(2의 거듭제곱)에 정렬되었는지 검사.
 *
 * @ptr: 검사할 포인터.
 * @alignment: 정렬 요구치(2^k).
 * @return: 정렬되어 있으면 true.
 *
 * SPDK_ALIGN_FLOOR(p, a) = p & ~(a-1). p가 a에 정렬되어 있다면
 * floor(p, a) == p. 이 동치를 이용한 단순 분기 없는 검사.
 */
static inline bool
is_aligned(void *ptr, size_t alignment)
{
	uintptr_t p = (uintptr_t)ptr;
	/* [한국어] 포인터를 정수 산술이 가능한 uintptr_t로 변환. */

	return p == SPDK_ALIGN_FLOOR(p, alignment);
	/* [한국어] floor(p, a) == p ↔ a 비트 정렬됨. */
}

/*
 * [한국어]
 * buffers_aligned - dest와 모든 source 버퍼가 alignment에 정렬되었는지 일괄 검사.
 *
 * @dest, @sources, @n, @alignment: 출력 버퍼/입력 배열/입력 수/정렬 요구치.
 * @return: 모두 정렬되어 있으면 true.
 *
 * 가속 경로(ISA-L xor_gen, 자체 64비트 루프) 진입 가능 여부 판단에 사용.
 * 한 개라도 정렬되지 않았으면 안전한 byte-wise 폴백을 사용해야 한다.
 */
static bool
buffers_aligned(void *dest, void **sources, uint32_t n, size_t alignment)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (!is_aligned(sources[i], alignment)) {
			/* [한국어] 어느 source라도 정렬 위반이면 즉시 false. */
			return false;
		}
	}

	return is_aligned(dest, alignment);
	/* [한국어] dest까지 모두 정렬되어야 true. */
}

/*
 * [한국어]
 * xor_gen_unaligned - 1바이트 단위 XOR 누적 (가장 안전한 폴백).
 *
 * @dest, @sources, @n, @len: 출력/입력 배열/소스 수/공통 길이.
 *
 * len 바이트를 한 위치(i)씩 돌며 모든 source의 같은 위치 byte를 XOR해 dest에
 * 기록. 정렬 보장이 없거나 끝 잔여 처리에 사용.
 *
 * 호출 체인: xor_gen_basic(잔여 또는 정렬 위반) → xor_gen_unaligned.
 */
static void
xor_gen_unaligned(void *dest, void **sources, uint32_t n, uint32_t len)
{
	uint32_t i, j;
	/* [한국어] i: 출력 위치(0..len-1), j: source 인덱스(0..n-1). */

	for (i = 0; i < len; i++) {
		uint8_t b = 0;
		/* [한국어] i 위치의 누적 XOR 값. 0으로 시작해 모든 source를 누적. */

		for (j = 0; j < n; j++) {
			b ^= ((uint8_t *)sources[j])[i];
			/* [한국어] j번째 source의 i번째 byte와 현재 누적값 XOR.
			 * RAID-5 패리티 정의 P = D0 ^ D1 ^ ... 그대로. */
		}
		((uint8_t *)dest)[i] = b;
		/* [한국어] 누적 결과를 출력 byte에 기록. */
	}
}

/*
 * [한국어]
 * xor_gen_basic - 64비트 단어 단위 XOR + 잔여 byte 처리.
 *
 * @dest, @sources, @n, @len: 위와 동일.
 *
 * 정렬이 보장되면 8B 단위로 묶어서 처리(처리량 8배 향상). 끝의 잔여 부분은
 * xor_gen_unaligned로 위임.
 */
static void
xor_gen_basic(void *dest, void **sources, uint32_t n, uint32_t len)
{
	uint32_t shift;
	/* [한국어] 8(=sizeof(uint64_t))의 log2 = 3. 우 시프트로 길이를 8B 단위로 변환. */
	uint32_t len_div, len_rem;
	/* [한국어] len_div: 8B 단위 단어 수, len_rem: len_div×8 (정렬 가능한 길이). */
	uint32_t i, j;
	/* [한국어] 단어 인덱스 / source 인덱스. */

	if (!buffers_aligned(dest, sources, n, sizeof(uint64_t))) {
		/* [한국어] 8B 정렬 안 되면 곧장 byte 폴백. */
		xor_gen_unaligned(dest, sources, n, len);
		return;
	}

	shift = spdk_u32log2(sizeof(uint64_t));
	/* [한국어] = 3. 8 = 2^3. 곱셈/나눗셈 대신 시프트로 빠르게. */
	len_div = len >> shift;
	/* [한국어] len / 8. */
	len_rem = len_div << shift;
	/* [한국어] len_div × 8: 8B 단위로 처리 가능한 정확한 byte 수.
	 * (len - len_rem)이 끝의 잔여 길이. */

	for (i = 0; i < len_div; i++) {
		uint64_t w = 0;
		/* [한국어] i번째 8B 워드의 XOR 누적값. */

		for (j = 0; j < n; j++) {
			w ^= ((uint64_t *)sources[j])[i];
			/* [한국어] 8B 정렬 보장하에서 64비트 한 번에 XOR. 8배 처리량. */
		}
		((uint64_t *)dest)[i] = w;
		/* [한국어] 누적 결과를 8B 단위로 기록. */
	}

	if (len_rem < len) {
		/* [한국어] 끝 잔여(0..7B) 처리: source/dest를 len_rem 만큼 전진시킨
		 * 새 포인터 배열을 만들어 unaligned 폴백 호출. */
		void *sources2[SPDK_XOR_MAX_SRC];
		/* [한국어] 스택 배열 — n ≤ SPDK_XOR_MAX_SRC 보장 하에 안전. */

		for (j = 0; j < n; j++) {
			sources2[j] = (uint8_t *)sources[j] + len_rem;
			/* [한국어] 각 source 포인터를 잔여 시작점으로 이동. */
		}

		xor_gen_unaligned((uint8_t *)dest + len_rem, sources2, n, len - len_rem);
		/* [한국어] 최대 7B 짧은 byte-wise 처리. */
	}
}

#ifdef SPDK_CONFIG_ISAL
#include "isa-l/include/raid.h"
/* [한국어] ISA-L의 xor_gen prototype 인클루드. AVX/AVX2 최적화 어셈블리 구현
 * 으로 32B 정렬 입력에서 가장 빠르다. */

#define SPDK_XOR_BUF_ALIGN 32
/* [한국어] ISA-L xor_gen은 32B 정렬을 요구. 호출자(RAID 모듈)가 stripe 버퍼
 * 정렬을 맞춰야 가속 경로 진입. */

/*
 * [한국어]
 * do_xor_gen (ISA-L판) - 정렬 가능한 경우 ISA-L 가속, 아니면 basic 폴백.
 *
 * @dest, @sources, @n, @len: 위와 동일.
 * @return: 0 또는 -EINVAL(n 너무 큼).
 *
 * ISA-L의 xor_gen은 인자가 (n+1, len, buffers[]) 구조 — buffers의 마지막
 * 항목이 dest이고 그 앞은 source. 본 헬퍼는 SPDK 시그니처를 그 형태로 변환.
 */
static int
do_xor_gen(void *dest, void **sources, uint32_t n, uint32_t len)
{
	if (buffers_aligned(dest, sources, n, SPDK_XOR_BUF_ALIGN)) {
		/* [한국어] 32B 정렬 모두 통과 → ISA-L 가속 경로 진입. */
		void *buffers[SPDK_XOR_MAX_SRC + 1];
		/* [한국어] +1은 dest 포인터를 끝에 추가하기 위함. */

		if (n >= INT_MAX) {
			/* [한국어] ISA-L 시그니처가 int를 받는데 우리 인자는 uint32.
			 * 캐스팅 시 음수가 되지 않도록 안전 가드. 실용 한계는
			 * SPDK_XOR_MAX_SRC=256이므로 거의 도달 불가. */
			return -EINVAL;
		}

		memcpy(buffers, sources, n * sizeof(buffers[0]));
		/* [한국어] sources 포인터들을 buffers[0..n-1]로 복사. */
		buffers[n] = dest;
		/* [한국어] ISA-L 컨벤션: 마지막 항목이 출력. */

		if (xor_gen(n + 1, len, buffers)) {
			/* [한국어] ISA-L 반환값 0=성공, 비0=에러. */
			return -EINVAL;
		}
	} else {
		/* [한국어] 32B 정렬 안 됨 → 자체 8B 폴백. */
		xor_gen_basic(dest, sources, n, len);
	}

	return 0;
}

#else
/* [한국어] ISA-L이 없는 빌드 — 자체 8B 폴백만 사용. */

#define SPDK_XOR_BUF_ALIGN sizeof(uint64_t)
/* [한국어] 폴백의 권장 정렬 = 8B. */

/*
 * [한국어]
 * do_xor_gen (폴백판) - xor_gen_basic에 위임.
 */
static inline int
do_xor_gen(void *dest, void **sources, uint32_t n, uint32_t len)
{
	xor_gen_basic(dest, sources, n, len);
	return 0;
}

#endif

/*
 * [한국어]
 * spdk_xor_gen - 외부 진입점: n개 source 버퍼를 XOR해 dest에 기록.
 *
 * @dest: 출력 버퍼(len 바이트). 입력과 겹치면 안 됨.
 * @sources: 입력 버퍼 포인터 배열. 각 버퍼는 모두 len 바이트.
 * @n: source 수. [2, SPDK_XOR_MAX_SRC] 범위.
 * @len: 공통 버퍼 길이.
 * @return: 0 성공, -EINVAL(n 범위 위반).
 *
 * 동기: RAID-5 패리티(P = D0^D1^...)나 erasure recovery에서 호출. 가능한
 * 경우 ISA-L 어셈블리를 사용해 매우 높은 처리량을 낸다.
 *
 * 호출 체인: bdev_raid 모듈 → spdk_xor_gen → do_xor_gen → (ISA-L) xor_gen
 *           또는 xor_gen_basic.
 */
int
spdk_xor_gen(void *dest, void **sources, uint32_t n, uint32_t len)
{
	if (n < 2 || n > SPDK_XOR_MAX_SRC) {
		/* [한국어] 1개짜리 XOR은 의미 없고, 256 초과는 스택 배열 한계.
		 * 호출자 인자 오류로 즉시 거부. */
		return -EINVAL;
	}

	return do_xor_gen(dest, sources, n, len);
	/* [한국어] 검증 통과 후 빌드별 구현으로 위임. */
}

/*
 * [한국어]
 * spdk_xor_get_optimal_alignment - 호출자가 가속 경로를 타기 위해 맞춰야 할
 *                                   버퍼 정렬을 반환.
 *
 * @return: ISA-L 빌드 시 32, 폴백 빌드 시 sizeof(uint64_t)=8.
 *
 * RAID 모듈은 본 값을 sysfs/RPC로 노출하거나 자체 메모리 풀의 정렬에 사용.
 */
size_t
spdk_xor_get_optimal_alignment(void)
{
	return SPDK_XOR_BUF_ALIGN;
	/* [한국어] 컴파일 타임에 결정된 정렬 상수 반환. */
}

SPDK_STATIC_ASSERT(SPDK_XOR_BUF_ALIGN > 0 && !(SPDK_XOR_BUF_ALIGN & (SPDK_XOR_BUF_ALIGN - 1)),
		   "Must be power of 2");
/* [한국어] 컴파일 타임에 SPDK_XOR_BUF_ALIGN이 0이 아니고 2의 거듭제곱인지
 * 확인. (a & (a-1)) == 0 ↔ a가 2의 거듭제곱.
 * SPDK_ALIGN_FLOOR/CEIL 등이 이 가정 위에서 작동하므로 위반 시 빌드 즉시 실패. */
