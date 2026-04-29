/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] 정수 log2 계산 유틸리티 (math.c)
 *
 * === 파일의 역할 ===
 * SPDK 전반에서 매우 자주 쓰이는 "정수에 대한 log2(x)" 연산(`spdk_u32log2`,
 * `spdk_u64log2`)을 구현한다. 비트 시프트 양 계산, 2의 거듭제곱 정렬 검사,
 * 페이지 크기→shift 변환 등 핫패스 코드에서 빈번히 호출되므로, 컴파일러
 * `__builtin_clz/clzll`(BSR/LZCNT 등 하드웨어 명령으로 매핑됨)을 이용해
 * 단일 명령 수준의 비용으로 처리한다. 입력이 0인 경우 수학적으로 정의되지
 * 않으므로 안전하게 0을 반환하는 정책을 채택한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 모든 SPDK 레이어(env/thread/bdev/nvme/util)에서 사용되는 공용 산술 헬퍼.
 * 호출 예:
 *   - lib/util/xor.c → buf 정렬을 위해 sizeof(uint64_t)의 log2를 구해 shift.
 *   - lib/bdev/* → buf alignment, blocklen 관련 비트 시프트 계산.
 *   - lib/nvme/* → MPS(memory page size) 등 NVMe 페이지 사이즈를 shift로 변환.
 * 호출 체인 핵심: 임의의 SPDK 모듈 → spdk_u32log2/u64log2 → __builtin_clz(ll).
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `spdk/util.h`(공개 prototype), `spdk/assert.h`(SPDK_STATIC_ASSERT),
 *   `spdk/stdinc.h`(uint32_t/uint64_t 등 표준 타입). 외부 라이브러리 의존 없음.
 * - 호출자: SPDK 거의 모든 서브시스템.
 * - 공유 상태: 없음. 순수 함수.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_u32log2(uint32_t x): floor(log2(x))를 32비트 입력에 대해 반환. x==0
 *   이면 0 반환(정책상 약속).
 * - spdk_u64log2(uint64_t x): 64비트 버전. 위와 동일 정책.
 *
 * 두 함수 모두 GCC 6+/x86 ELF에서 `target_clones` 속성으로 BMI/Core2/Atom/
 * default 4가지 대상별 멀티버전을 자동 생성하고, 런타임에 IFUNC 디스패치로
 * 최적 변형을 선택한다 (lzcnt/bsr 명령을 마이크로아키텍처별로 활용).
 */

#include "spdk/stdinc.h"
/* [한국어] uint32_t/uint64_t 등 고정폭 정수 타입과 표준 헤더 모음. */
#include "spdk/util.h"
/* [한국어] spdk_u32log2/spdk_u64log2 prototype, spdk_max/min 등의 일반 유틸. */
#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT — 컴파일 시 타입 크기 검증을 위한 매크로. */

/* The following will automatically generate several version of
 * this function, targeted at different architectures. This
 * is only supported by GCC 6 or newer. */
/* [한국어] target_clones 분기 가드:
 *   - GCC 6 이상이고 (clang은 미지원),
 *   - x86/x86_64 + ELF 환경일 때만 멀티버전 생성을 활성화한다.
 *   - clang/non-x86/Windows 등에서는 평범한 단일 함수로 컴파일. */
#if defined(__GNUC__) && __GNUC__ >= 6 && !defined(__clang__) \
	&& (defined(__i386__) || defined(__x86_64__)) \
	&& defined(__ELF__)
__attribute__((target_clones("bmi", "arch=core2", "arch=atom", "default")))
/* [한국어] 한 함수에서 4개 변형(bmi: lzcnt 명령 활용 가능, core2: SSE 위주,
 * atom: 인오더 코어용, default: 안전한 기본)을 컴파일하고, 런타임 IFUNC
 * 리졸버가 CPUID로 변형을 고른다. 핫패스 한 번에 명령 1~2개 수준으로 끝나는
 * 함수라 이런 마이크로 최적화가 의미를 가진다. */
#endif
/*
 * [한국어]
 * spdk_u32log2 - 32비트 정수의 floor(log2) 계산.
 *
 * @x: 입력 정수. 0이면 정의되지 않으므로 0을 반환.
 * @return: x>0이면 floor(log2(x)), x==0이면 0(정책).
 *
 * BSR/LZCNT 같은 비트 카운트 명령 한 번으로 끝나는 핫패스용. blocklen에서
 * shift 양을 구하거나, 2^k 정렬 검사를 단순화할 때 호출된다.
 *
 * 실행 컨텍스트: 어디서든 안전(순수, 재진입 가능).
 * 호출 체인: 임의 모듈 → spdk_u32log2 → __builtin_clz(int).
 */
uint32_t
spdk_u32log2(uint32_t x)
{
	if (x == 0) {
		/* log(0) is undefined */
		/* [한국어] 0의 로그는 수학적으로 정의되지 않음. SPDK는 호출자가
		 * 가드를 잊어도 안전하도록 0을 반환하는 약속을 채택. 호출자가
		 * 의미적으로 "최소 1bit"가 필요하면 별도 분기를 두어야 한다. */
		return 0;
	}
	SPDK_STATIC_ASSERT(sizeof(x) == sizeof(unsigned int), "Incorrect size");
	/* [한국어] __builtin_clz는 표준 C에서 unsigned int 인자를 받는다.
	 * x86_64/ARM64 등 일반 ABI에서 sizeof(unsigned int) == 4 == sizeof(uint32_t)
	 * 이지만, 비표준 플랫폼에서 깨지면 컴파일 에러로 잡기 위한 정적 단언. */
	return 31u - __builtin_clz(x);
	/* [한국어] __builtin_clz: leading zero count. 32비트 중 상위 0의 개수가
	 * 반환되므로 (31 - clz)가 곧 최상위 1비트의 위치 = floor(log2(x)).
	 * 예: x=8(0b1000) → clz=28 → 31-28=3 = log2(8). x86에서 BSR 또는
	 * LZCNT(BMI)로 컴파일됨. */
}

/* The following will automatically generate several version of
 * this function, targeted at different architectures. This
 * is only supported by GCC 6 or newer. */
/* [한국어] u64 버전에 대해서도 동일한 멀티버전 가드 적용.
 * 64비트 정수에는 BSR(64-bit) / LZCNT가 매핑된다. */
#if defined(__GNUC__) && __GNUC__ >= 6 && !defined(__clang__) \
	&& (defined(__i386__) || defined(__x86_64__)) \
	&& defined(__ELF__)
__attribute__((target_clones("bmi", "arch=core2", "arch=atom", "default")))
#endif
/*
 * [한국어]
 * spdk_u64log2 - 64비트 정수의 floor(log2) 계산.
 *
 * @x: 입력 정수. 0이면 0 반환.
 * @return: x>0이면 floor(log2(x)).
 *
 * 64비트 폭이 필요한 경로(예: 큰 LBA 범위, 큰 메모리 풀 크기)에서 사용.
 *
 * 호출 체인: 임의 모듈 → spdk_u64log2 → __builtin_clzll(unsigned long long).
 */
uint64_t
spdk_u64log2(uint64_t x)
{
	if (x == 0) {
		/* log(0) is undefined */
		/* [한국어] u32 버전과 동일한 정책: 0 입력에 대해 0 반환. */
		return 0;
	}
	SPDK_STATIC_ASSERT(sizeof(x) == sizeof(unsigned long long), "Incorrect size");
	/* [한국어] __builtin_clzll가 unsigned long long을 받으므로 64비트 동등성
	 * 보장. LP64에서는 unsigned long도 64비트지만 ll이 표준 prototype과 매치. */
	return 63u - __builtin_clzll(x);
	/* [한국어] 64비트 leading zero count. (63 - clzll)이 최상위 1비트의 위치.
	 * 예: x=2^40 → clzll=23 → 63-23=40. */
}
