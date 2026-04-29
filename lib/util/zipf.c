/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Zipfian 분포 난수 생성기 (zipf.c)
 *
 * === 파일의 역할 ===
 * Zipf 분포(특정 값 i가 1/i^θ에 비례하여 나타나는 멱법칙 분포)를 따르는
 * [0, range) 범위의 정수 난수를 생성한다. 매개변수 θ(theta)는 분포의
 * 기울기를 결정한다(0에 가까울수록 균등, 클수록 한쪽 끝으로 집중).
 * 알고리즘은 Gray, Sundaresan, Englert et al.의 "Quickly Generating
 * Billion-Record Synthetic Databases"(SIGMOD '94)의 fast generator를 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK가 bdev 성능을 평가할 때 fio·내장 perf 툴의 "skewed access" 워크로드
 * (예: hot-set 읽기 비율이 압도적인 캐시 친화적 시나리오)를 모사하는 데
 * 사용된다. 호출 흐름:
 *   examples/bdev/bdevperf 등 → spdk_zipf_create(range,θ,seed) →
 *   I/O 루프에서 매 반복 spdk_zipf_generate() 호출 → 반환된 LBA 인덱스로
 *   bdev I/O 발행 → 종료 시 spdk_zipf_free.
 * fio_plugin/zipf.c 같은 다른 SPDK 컴포넌트도 동일 API를 호출.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `spdk/util.h`(spdk_min, spdk_rand_xorshift64*),
 *   `spdk/zipf.h`(공개 prototype), `spdk/stdinc.h`(math.h pow 등). libm 링크.
 * - 호출자: bdev perf/fio plugin 등 워크로드 생성 코드.
 * - 공유 상태: 핸들 단위. spdk_zipf 인스턴스는 호출자가 자신의 스레드에서만
 *   사용 — 동시 호출 시 seed 갱신이 race를 일으키므로 인스턴스 공유 금지.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_zipf: range/alpha/eta/theta/zetan/val1_limit/seed를 캐시하는
 *   생성기 핸들. 생성 시 한 번만 계산되는 비싼 zeta 합을 보관한다.
 * - zeta_increment(n, theta): 한 항 1/(n+1)^θ 계산.
 * - zeta(range, theta): 시리즈 합. 앞 1천만개는 정확히 누적, 그 이후는
 *   1백만 단위로 사다리꼴 근사 → 메모리·시간 절약.
 * - spdk_zipf_create/free: 핸들 라이프사이클.
 * - spdk_zipf_generate: 매 호출마다 새 정수 샘플 반환(O(1) + pow 1회).
 */

#include "spdk/stdinc.h"
/* [한국어] math.h(pow), stdint.h(uint64_t/UINT64_MAX), stdlib.h(calloc/free) 등. */
#include "spdk/util.h"
/* [한국어] spdk_min/spdk_max, spdk_rand_xorshift64_seed,
 * spdk_rand_xorshift64(&seed) 같은 lockless RNG 헬퍼. */
#include "spdk/zipf.h"
/* [한국어] 본 파일의 공개 prototype 및 spdk_zipf의 forward 선언. */

/* [한국어] Zipfian 생성기 핸들.
 * 한 번 만들면 range/theta는 불변, zetan/eta/alpha/val1_limit는 미리 계산된
 * 캐시이며, seed만 매 generate에서 갱신된다. 단일 스레드에서 사용해야 한다. */
struct spdk_zipf {
	uint64_t	range;
	/* [한국어] 출력 정수의 상한(반열림: 0 ≤ val < range).
	 * 설정자: spdk_zipf_create(range)에서 1회.
	 * 읽는 자: zeta 계산과 generate 모두에서 사용.
	 * 동기화: 불변이므로 race 없음. */
	double		alpha;
	/* [한국어] 1/(1-θ) — generate 마지막 단계의 pow 지수.
	 * 설정자: create. 읽는 자: generate. */
	double		eta;
	/* [한국어] 사전 계산 보조값. 분포 형태를 정규화하기 위한 인자.
	 * 공식: (1 - (2/range)^(1-θ)) / (1 - ζ(2,θ)/ζ(range,θ))
	 * 호출당 pow 입력에 곱해지는 스케일. */
	double		theta;
	/* [한국어] 분포 모양 매개변수(0..1 권장). 클수록 head로 집중. */
	double		zetan;
	/* [한국어] ζ(range,θ) = Σ_{i=1..range} 1/i^θ.
	 * create에서 한 번만 계산되는 가장 비싼 값(콜드 패스). 이후 generate
	 * 에서는 단순 곱셈으로만 사용. */
	double		val1_limit;
	/* [한국어] randz가 이 값보다 작으면 결과를 1로 즉답하기 위한 임계값.
	 * 1.0 + (1/2)^θ. fast-path 분기로 pow 호출 회피. */
	uint64_t	seed;
	/* [한국어] xorshift64 PRNG 상태. 매 generate마다 in-place 갱신.
	 * 동기화: 인스턴스를 공유하는 다중 스레드는 race가 발생하므로 금지. */
};

/*
 * [한국어]
 * zeta_increment - ζ 시리즈의 단일 항 1/(n+1)^θ 계산.
 *
 * @n: 인덱스(0부터 시작). 실제 값은 (n+1)^(-θ).
 * @theta: 분포 매개변수.
 * @return: 항의 값.
 *
 * 호출 체인: zeta() → zeta_increment(매 항).
 */
static double
zeta_increment(uint64_t n, double theta)
{
	return pow((double) 1.0 / (n + 1), theta);
	/* [한국어] libm pow. theta가 일반적으로 1보다 작은 양수 → x^θ 계산. */
}

/*
 * [한국어]
 * zeta - 부분 ζ(range, θ) = Σ_{k=1..range} k^(-θ) 를 계산.
 *
 * @range: 항 수.
 * @theta: 분포 매개변수.
 * @return: 합산 결과.
 *
 * 동기: range가 수억이 넘는 경우 정확히 모두 더하면 매우 비싸다. 앞 1천만
 * 항은 정확히 누적하고, 그 이후는 100만 단위로 양 끝 평균 × 구간 길이
 * (사다리꼴 근사) 사용. spdk_zipf_create에서 1회만 호출되는 콜드 패스라서
 * 정확도 vs 속도의 절충이 정당화된다.
 *
 * 호출 체인: spdk_zipf_create → zeta → zeta_increment.
 */
static double
zeta(uint64_t range, double theta)
{
	double zetan = 0;
	/* [한국어] 누적 합. */
	double inc1, inc2;
	/* [한국어] 사다리꼴 양 끝 점에서의 항 값. */
	uint64_t i, calc, count;
	/* [한국어] i: 현재 진행 인덱스, calc: 정확 누적 종료 인덱스,
	 * count: 사다리꼴 한 구간의 항 수. */
	const uint32_t ZIPF_MAX_ZETA_CALC = 10 * 1000 * 1000;
	/* [한국어] 정확 누적의 상한 = 1천만 항. 이 이상은 근사. */
	const uint32_t ZIPF_ZETA_ESTIMATE = 1 * 1000 * 1000;
	/* [한국어] 사다리꼴 근사의 한 구간 = 1백만 항. */

	/* Cumulate zeta discretely for the first ZIPF_MAX_ZETA_CALC
	 * entries in the range.
	 */
	calc = spdk_min(ZIPF_MAX_ZETA_CALC, range);
	/* [한국어] range가 1천만 이하면 전부 정확히 누적(루프만 충분). */
	for (i = 0; i < calc; i++) {
		zetan += zeta_increment(i, theta);
		/* [한국어] 항을 하나씩 정확히 더함. */
	}

	/* For the remaining values in the range, increment zetan
	 * with an approximation for every ZIPF_ZETA_ESTIMATE
	 * entries.  We will take an average of the increment
	 * for (i) and (i + ZIPF_ZETA_ESTIMATE), and then multiply
	 * that by ZIPF_ZETA_ESTIMATE.
	 *
	 * Of course, we'll cap ZIPF_ZETA_ESTIMATE to something
	 * smaller if necessary at the end of the range.
	 */
	while (i < range) {
		count = spdk_min(ZIPF_ZETA_ESTIMATE, range - i);
		/* [한국어] 마지막 구간이 짧으면 남은 만큼만 처리. */
		inc1 = zeta_increment(i, theta);
		/* [한국어] 구간 시작점 항. */
		inc2 = zeta_increment(i + count, theta);
		/* [한국어] 구간 끝점 항. */
		zetan += (inc1 + inc2) * count / 2;
		/* [한국어] 사다리꼴 공식: (좌 + 우) × 폭 / 2. θ < 1 일대 i^(-θ)는
		 * 단조 감소이므로 사다리꼴 근사가 합리적. */
		i += count;
		/* [한국어] 다음 구간으로. */
	}

	return zetan;
	/* [한국어] 부분 ζ(range, θ) 추정값을 반환. */
}

/*
 * [한국어]
 * spdk_zipf_create - Zipfian 생성기 핸들 생성 및 사전 계산.
 *
 * @range: 출력 범위 [0, range).
 * @theta: 분포 매개변수.
 * @seed: PRNG 시드(현재 코드는 호출자 인자를 사용하지 않고 내부에서
 *        spdk_rand_xorshift64_seed()로 매번 새 시드를 받는다).
 * @return: 생성된 핸들 또는 NULL(메모리 부족).
 *
 * 동기: zetan/eta 등은 ζ(range,θ) 합산이 필요해 비싸다. 한 번만 계산해
 * 핸들에 저장 → 이후 generate는 O(1).
 *
 * 호출 체인: 워크로드 셋업 코드 → spdk_zipf_create → zeta(2회).
 */
struct spdk_zipf *
spdk_zipf_create(uint64_t range, double theta, uint32_t seed)
{
	struct spdk_zipf *zipf;
	/* [한국어] 새 핸들 포인터. */

	zipf = calloc(1, sizeof(*zipf));
	/* [한국어] 0 초기화 + 할당. 실패 시 NULL. */
	if (zipf == NULL) {
		return NULL;
	}

	zipf->range = range;
	/* [한국어] 호출자 지정 출력 범위 저장. */
	zipf->seed = spdk_rand_xorshift64_seed();
	/* [한국어] 전역 카운터/시간 기반의 새 64비트 시드 생성. 인자 seed는
	 * 현재 무시됨 — 의도적인 변경 사항(매번 다른 시퀀스 보장).
	 * (공유 RNG 사용을 피해 인스턴스별 분리 보장.) */

	zipf->theta = theta;
	/* [한국어] 분포 매개변수. */
	zipf->alpha = 1.0 / (1.0 - zipf->theta);
	/* [한국어] generate 단계의 pow 지수. θ ≥ 1이면 분모 0/음수 → 호출자가
	 * θ < 1을 보장해야 한다. */
	zipf->zetan = zeta(range, theta);
	/* [한국어] ζ(range,θ): 본 분포의 normalization 상수. 비싼 1회 계산. */
	zipf->eta = (1.0 - pow(2.0 / zipf->range, 1.0 - zipf->theta)) /
		    (1.0 - zeta(2, theta) / zipf->zetan);
	/* [한국어] η(eta): generate에서 사용할 보조 정규화 상수. ζ(2,θ)도 별도
	 * 계산해 비율을 만든다. */
	zipf->val1_limit = 1.0 + pow(0.5, zipf->theta);
	/* [한국어] 결과가 1로 결정되는 임계 randz. 작은 randz에서 pow를 피해
	 * 빠른 경로로 가기 위함. */

	return zipf;
	/* [한국어] 사용자에게 핸들 반환. spdk_zipf_free로 정리할 책임이 있음. */
}

/*
 * [한국어]
 * spdk_zipf_free - 핸들 해제 및 호출자 포인터 NULL화.
 *
 * @zipfp: 핸들 포인터를 가리키는 더블 포인터. NULL 금지.
 *
 * 호출자 변수도 NULL로 만들어 use-after-free를 방지하는 컨벤션.
 */
void
spdk_zipf_free(struct spdk_zipf **zipfp)
{
	assert(zipfp != NULL);
	/* [한국어] 더블 포인터 자체가 NULL이면 호출자 버그. assert로 즉시 잡음. */
	free(*zipfp);
	/* [한국어] free(NULL)은 합법이므로 *zipfp가 NULL이어도 안전. */
	*zipfp = NULL;
	/* [한국어] dangling pointer 방지. */
}

/*
 * [한국어]
 * spdk_zipf_generate - 다음 Zipfian 정수 샘플 반환.
 *
 * @zipf: 활성 핸들.
 * @return: [0, range) 범위 정수.
 *
 * O(1) 비용에 pow 1회. 빠른 경로 두 가지(0/1 즉답)는 전체 분포에서 가장
 * 빈도 높은 값을 분기 없이 즉시 반환해 핫패스 효율을 높인다.
 *
 * 호출 체인: 워크로드 루프(예: bdevperf I/O 발행) → spdk_zipf_generate.
 */
uint64_t
spdk_zipf_generate(struct spdk_zipf *zipf)
{
	double randu, randz;
	/* [한국어] randu: [0,1] 균등 난수, randz: zetan 스케일된 누적값 위치. */
	uint64_t val;
	/* [한국어] 일반 경로의 결과 변수. */

	randu = (double)spdk_rand_xorshift64(&zipf->seed) / (double)UINT64_MAX;
	/* [한국어] xorshift64로 64비트 의사난수 생성하고 [0,1] 정규화.
	 * &zipf->seed로 인스턴스 로컬 상태를 in-place 갱신 → 락 불필요(단,
	 * 인스턴스 공유 금지). */
	randz = randu * zipf->zetan;
	/* [한국어] randu를 zetan 스케일로 변환. CDF 역변환에 쓰임. */

	if (randz < 1.0) {
		/* [한국어] 가장 작은 누적 영역 → 0 즉답(전체 확률 중 큰 비중을
		 * 차지하므로 핫패스 분기로 처리). */
		return 0;
	} else if (randz < zipf->val1_limit) {
		/* [한국어] 1번째 항을 포함하는 영역 → 1 즉답. pow 회피. */
		return 1;
	} else {
		val = zipf->range * pow(zipf->eta * (randu - 1.0) + 1.0, zipf->alpha);
		/* [한국어] 일반 영역: CDF 역변환 공식
		 * val = range × (η(u-1) + 1)^α. randu와 zipf의 사전 계산값들을 조합. */
		return val % zipf->range;
		/* [한국어] 부동소수점 오차로 range 이상 나오는 가능성을 막는 안전망. */
	}
}
