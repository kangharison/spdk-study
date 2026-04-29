/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * Generic histogram library
 */

/*
 * [한국어 설명] 범용 히스토그램 자료구조 (histogram_data.h) — 약 286 라인
 *
 * === 파일의 역할 ===
 * SPDK 전반에서 사용하는 범용 히스토그램 라이브러리 (header-only inline 구현).
 * 데이터포인트(주로 TSC 단위 latency)의 분포를 "범위(range) 단위로 묶인
 * 버킷 배열"로 추적한다. 핵심 사용 사례는 bdev I/O 완료 시간 분포 — 각 I/O
 * 완료 시점에 spdk_histogram_data_tally(latency_tsc)를 호출해 누적하고,
 * 나중에 spdk_histogram_data_iterate()로 P50/P99 같은 통계나 그래프를 산출.
 * 모든 함수가 static inline 이므로 외부 라이브러리 의존 없이 헤더만 포함하면
 * 사용 가능 (lock-free, per-thread 인스턴스 권장).
 *
 * 알고리즘:
 *   - 데이터포인트 X의 MSB 위치(=range)와 그 위 GRANULARITY 비트(=index)를
 *     추출해 (range, index) 좌표의 버킷에 ++카운트.
 *   - range는 logarithmic, range 내부는 linear → P99 latency 같은 long-tail
 *     분포를 적은 메모리로 정확히 표현 (HdrHistogram과 동일 아이디어).
 *   - GRANULARITY=7 → range당 128 버킷. 2.3GHz CPU에서 7~14us 구간 50ns 해상도.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 측: lib/bdev (I/O latency 추적), examples/perf, app/iscsi_top, 그리고
 *          외부 사용자 코드. spdk_bdev_histogram_enable() 같은 RPC 명령이
 *          내부적으로 본 구조체를 할당·집계한다.
 * 호출 흐름:
 *   spdk_histogram_data_alloc() (초기화)
 *     → spdk_histogram_data_tally(h, tsc_delta) (I/O 완료마다)
 *     → spdk_histogram_data_iterate(h, fn, ctx) (덤프 시점)
 *     → spdk_histogram_data_free()
 * 모든 연산이 단일 스레드 가정 — 멀티스레드 집계는 per-thread 히스토그램 후
 * spdk_histogram_data_merge()로 합치는 패턴 권장 (reactor lockless 모델 부합).
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/stdinc.h (uint64_t, calloc, memset 등 표준 타입/함수).
 *            컴파일러 내장 함수 __builtin_clzll(64비트 leading-zero count)
 *            를 사용 — GCC/Clang 모두 지원, 일반적으로 lzcnt/bsr 명령 1개로
 *            컴파일된다.
 * 의존하는 모듈: lib/bdev/bdev.c (히스토그램 enable/disable), 사용자 앱
 *              (perf, iscsi_top, RPC 핸들러). bdev 모듈이 본 구조체를
 *              spdk_bdev에 attach하여 per-channel 히스토그램을 운영.
 * 데이터 흐름: TSC delta(uint64_t) → tally → bucket[range][index]++ →
 *             iterate가 각 버킷의 (start, end, count) 콜백 호출 → 사용자
 *             코드가 마이크로초 변환·퍼센타일 계산 수행.
 * 공유 자료구조: 단일 spdk_histogram_data 인스턴스는 한 스레드 전용. 멀티
 *              스레드 집계 시 thread별 히스토그램을 reactor msg로 모아서
 *              merge.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_histogram_data: granularity / min_range / max_range / bucket[]
 *     - bucket은 (max_range - min_range + 1) * (1<<granularity) 개의 uint64_t.
 *   - SPDK_HISTOGRAM_GRANULARITY_DEFAULT = 7 (range당 128 버킷, 50ns 해상도).
 *   - spdk_histogram_data_alloc / _alloc_sized / _alloc_sized_ext: 할당 3종.
 *   - spdk_histogram_data_free: 해제.
 *   - spdk_histogram_data_reset: 모든 버킷을 0으로 초기화 (memset).
 *   - spdk_histogram_data_tally(h, datapoint): 데이터포인트 1건 누적 (핫패스).
 *   - spdk_histogram_data_iterate(h, fn, ctx): 모든 버킷 순회하며 콜백 호출.
 *   - spdk_histogram_data_merge(dst, src): 같은 형태의 두 히스토그램을 합산.
 *   - __spdk_histogram_data_get_bucket_range/_index/_start: 내부 좌표 계산기.
 */

#ifndef _SPDK_HISTOGRAM_DATA_H_      /* [한국어] 다중 포함 방지 가드 시작 */
#define _SPDK_HISTOGRAM_DATA_H_

#include "spdk/stdinc.h"             /* [한국어] 표준 타입(uint32/64_t, calloc, memset, free, EINVAL) 묶음 */

#ifdef __cplusplus
extern "C" {                         /* [한국어] C++에서 사용 시 C 링키지로 묶어 name mangling 방지 */
#endif

/* [한국어] === 히스토그램 형상 매크로 ===
 * 모든 매크로가 (h)를 인자로 받아 런타임 granularity를 사용. 컴파일 시 상수
 * 가 아니므로 약간의 인덱싱 비용이 있으나, 다양한 granularity 인스턴스를
 * 한 빌드 안에서 공존시키기 위함. */

#define SPDK_HISTOGRAM_GRANULARITY_DEFAULT	7
/* [한국어] 기본 granularity. range당 1<<7 = 128 버킷이 생성된다.
 * 2.3GHz CPU에서 7~14us 구간 50ns 해상도 — Intel Optane SSD latency 측정에 최적. */

#define SPDK_HISTOGRAM_GRANULARITY(h)		h->granularity
/* [한국어] 인스턴스 h의 granularity 값을 추출 (range당 버킷 수의 log2). */

#define SPDK_HISTOGRAM_BUCKET_LSB(h)		(64 - SPDK_HISTOGRAM_GRANULARITY(h))
/* [한국어] 64비트 데이터포인트에서 "버킷 인덱스로 사용되는 비트군의 최하위 위치".
 * 값이 작을수록 상위 비트만으로 인덱싱 → range가 빠르게 증가.
 * 데이터포인트 X의 leading zero 수와 비교해 range = LSB - clz로 결정. */

#define SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE(h)	(1ULL << SPDK_HISTOGRAM_GRANULARITY(h))
/* [한국어] 한 range에 들어가는 버킷 수 (= 1 << granularity).
 * granularity=7 → 128. ULL 접미사로 64비트 시프트 보장. */

#define SPDK_HISTOGRAM_BUCKET_MASK(h)		(SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE(h) - 1)
/* [한국어] range 내부 인덱스를 추출하기 위한 비트마스크 (NUM_BUCKETS_PER_RANGE - 1).
 * 예: 128 → 0x7F. (datapoint >> shift) & MASK 형태로 사용. */

#define SPDK_HISTOGRAM_NUM_BUCKET_RANGES(h)	(h->max_range - h->min_range + 1)
/* [한국어] 활성화된 range 수. min_range=0, max_range=57이면 58개.
 * alloc_sized_ext에서 min_val/max_val로 좁히면 더 작아짐. */

#define SPDK_HISTOGRAM_NUM_BUCKETS(h)		(SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE(h) * \
						 SPDK_HISTOGRAM_NUM_BUCKET_RANGES(h))
/* [한국어] 전체 버킷 개수 (range_count * buckets_per_range).
 * bucket[] 배열의 길이이자 calloc/memset에 사용되는 크기. */

/*
 * SPDK histograms are implemented using ranges of bucket arrays.  The most common usage
 * model is using TSC datapoints to capture an I/O latency histogram.  For this usage model,
 * the histogram tracks only TSC deltas - any translation to microseconds is done by the
 * histogram user calling spdk_histogram_data_iterate() to iterate over the buckets to perform
 * the translations.
 *
 * Each range has a number of buckets determined by SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE
 * which is 128.  The buckets in ranges 0 and 1 each map to one specific datapoint value.
 * The buckets in subsequent ranges each map to twice as many datapoint values as buckets
 * in the range before it:
 *
 * Range 0:  1 value each  - 128 buckets cover 0 to 127 (2^7-1)
 * Range 1:  1 value each  - 128 buckets cover 128 to 255 (2^8-1)
 * Range 2:  2 values each - 128 buckets cover 256 to 511 (2^9-1)
 * Range 3:  4 values each - 128 buckets cover 512 to 1023 (2^10-1)
 * Range 4:  8 values each - 128 buckets cover 1024 to 2047 (2^11-1)
 * Range 5: 16 values each - 128 buckets cover 2048 to 4095 (2^12-1)
 * ...
 * Range 55: 2^54 values each - 128 buckets cover 2^61 to 2^62-1
 * Range 56: 2^55 values each - 128 buckets cover 2^62 to 2^63-1
 * Range 57: 2^56 values each - 128 buckets cover 2^63 to 2^64-1
 *
 * On a 2.3GHz processor, this strategy results in 50ns buckets in the 7-14us range (sweet
 * spot for Intel Optane SSD latency testing).
 *
 * Buckets can be made more granular by increasing SPDK_HISTOGRAM_GRANULARITY.  This
 * comes at the cost of additional storage per namespace context to store the bucket data.
 * In order to lower number of ranges to shrink unnecessary low and high datapoints
 * min_val and max_val can be specified with spdk_histogram_data_alloc_sized_ext().
 * It will limit the values in histogram to a range [min_val, max_val).
 */

/* [한국어] 히스토그램 인스턴스. header-only inline 함수들이 직접 필드를 접근.
 * 사용 패턴: 한 reactor/스레드가 alloc → tally 누적 → iterate/merge → free.
 * lock 없이 단일 스레드 가정 — 멀티스레드는 per-thread 인스턴스 + merge 권장. */
struct spdk_histogram_data {
	uint32_t	granularity;
	/* [한국어] range당 버킷 수의 log2 값 (예: 7 → 128 버킷).
	 * 설정자: alloc_sized_ext()가 호출자 인자로부터 설정.
	 * 읽는 자: 모든 SPDK_HISTOGRAM_* 매크로와 bucket_range/index 계산기.
	 * 값 범위: 합리적으로 1~10 (너무 크면 메모리 폭증). */

	uint32_t	min_range;
	/* [한국어] 활성화된 최저 range. 이 미만의 데이터포인트는 모두 (min_range, 0) 버킷으로 누적.
	 * 설정자: alloc_sized_ext()가 min_val을 bucket_range로 변환해 저장. 기본은 0.
	 * 읽는 자: tally(), iterate(), bucket 인덱스 계산. */

	uint32_t	max_range;
	/* [한국어] 활성화된 최고 range. 이 초과 데이터포인트는 (max_range, last_index) 버킷에 누적.
	 * 설정자: alloc_sized_ext()가 max_val-1을 bucket_range로 변환. 기본은 57(=2^64까지).
	 * 읽는 자: tally()의 saturating 분기, iterate() 루프 상한. */

	uint64_t	*bucket;
	/* [한국어] 실제 카운트가 누적되는 평탄(flat) 배열. 길이 = NUM_BUCKETS(h).
	 * 설정자: alloc_sized_ext()가 calloc으로 0 초기화한 배열을 할당. free()에서 해제.
	 * 읽는 자: __spdk_histogram_increment / get_count / get_bucket / reset / merge.
	 * 인덱스 식: ((range - min_range) << granularity) + index_in_range.
	 * 동기화: 단일 스레드 전용 — 다른 스레드는 별도 인스턴스 후 merge. */
};

/*
 * [한국어]
 * __spdk_histogram_increment - (range, index) 좌표의 버킷 카운터를 1 증가.
 *
 * @h: 히스토그램 인스턴스.
 * @range: 데이터포인트가 속한 range (0 ~ NUM_BUCKET_RANGES-1 + min_range).
 * @index: range 내부 인덱스 (0 ~ NUM_BUCKETS_PER_RANGE-1).
 *
 * 평탄 배열 인덱싱: (range - min_range) << granularity + index.
 * 호출자: spdk_histogram_data_tally() 핫패스에서 1회/데이터포인트.
 * 락 없음 — 단일 스레드 전용.
 */
static inline void
__spdk_histogram_increment(struct spdk_histogram_data *h, uint32_t range, uint32_t index)
{
	uint64_t *count;                  /* [한국어] 카운터 위치를 가리킬 포인터 */

	count = &h->bucket[((range - h->min_range) << SPDK_HISTOGRAM_GRANULARITY(h)) + index];
	                                  /* [한국어] 좌표 → 평탄 인덱스 변환. range를 0-base로 정규화 후 granularity bit만큼 left-shift = range당 NUM_BUCKETS_PER_RANGE 칸씩 점프 */
	(*count)++;                       /* [한국어] 단일 스레드 가정 — atomic 불필요. 다중 스레드 사용 시 race 발생 */
}

/*
 * [한국어]
 * __spdk_histogram_get_count - (range, index) 버킷의 누적 카운트 조회 (불변 인스턴스).
 *
 * @h: const 히스토그램 (읽기만).
 * @range: 좌표.
 * @index: 좌표.
 * @return: 해당 버킷의 uint64_t 카운트.
 *
 * iterate()와 merge() 같은 read-only 순회 경로에서 사용.
 */
static inline uint64_t
__spdk_histogram_get_count(const struct spdk_histogram_data *h, uint32_t range, uint32_t index)
{
	return h->bucket[((range - h->min_range) << SPDK_HISTOGRAM_GRANULARITY(h)) + index];
	                                  /* [한국어] increment와 동일한 인덱싱 식 — 좌표 → 평탄 배열 위치 */
}

/*
 * [한국어]
 * __spdk_histogram_get_bucket - (range, index) 버킷의 카운터 포인터 반환.
 *
 * 외부 누산기(예: 다른 위치에서 atomic increment)가 직접 접근하고 싶을 때 사용.
 */
static inline uint64_t *
__spdk_histogram_get_bucket(const struct spdk_histogram_data *h, uint32_t range, uint32_t index)
{
	return &h->bucket[((range - h->min_range) << SPDK_HISTOGRAM_GRANULARITY(h)) + index];
	                                  /* [한국어] increment와 동일 인덱싱식의 주소 버전 */
}

/*
 * [한국어]
 * spdk_histogram_data_reset - 모든 버킷을 0으로 초기화.
 *
 * @histogram: 대상 인스턴스 (NULL 불가).
 *
 * 사용 시점: I/O 통계 윈도우 리셋, 측정 재시작.
 * 동기화: 단일 스레드 전용.
 */
static inline void
spdk_histogram_data_reset(struct spdk_histogram_data *histogram)
{
	memset(histogram->bucket, 0, SPDK_HISTOGRAM_NUM_BUCKETS(histogram) * sizeof(uint64_t));
	                                  /* [한국어] bucket 배열 전체를 0으로 초기화. NUM_BUCKETS = range_count * 1<<granularity */
}

/*
 * [한국어]
 * __spdk_histogram_data_get_bucket_range - 데이터포인트가 속할 range 계산.
 *
 * @h: 히스토그램 (granularity 참조).
 * @datapoint: 분류할 값 (보통 TSC delta).
 * @return: range 인덱스. 0이 가장 작은 값을 담당.
 *
 * 알고리즘:
 *   - clz = leading zero count (datapoint=0이면 64로 정의).
 *   - clz가 BUCKET_LSB(=64-granularity) 이하면 range = LSB - clz, 아니면 0.
 *   - 즉 datapoint의 MSB 위치를 range로 사용.
 *
 * 예시(granularity=7, LSB=57):
 *   X=10 (4비트) → clz=60 → 60>57 → range=0 (range 0이 0~127 담당)
 *   X=200 (8비트) → clz=56 → range=57-56=1
 *   X=1024 (11비트) → clz=53 → range=4
 */
static inline uint32_t
__spdk_histogram_data_get_bucket_range(struct spdk_histogram_data *h, uint64_t datapoint)
{
	uint32_t clz, range;              /* [한국어] clz: count of leading zeros, range: 결정될 출력값 */

	clz = datapoint > 0 ? __builtin_clzll(datapoint) : 64;
	                                  /* [한국어] __builtin_clzll(0)는 정의되지 않음 — 0은 64로 명시 처리 (range 0의 가장 작은 버킷에 매핑됨) */

	if (clz <= SPDK_HISTOGRAM_BUCKET_LSB(h)) {
	                                  /* [한국어] datapoint가 range 1 이상에 속할 만큼 큰 경우 (MSB 위치 ≥ LSB) */
		range = SPDK_HISTOGRAM_BUCKET_LSB(h) - clz;
		                              /* [한국어] MSB 위치를 range로 변환 (LSB = 64-granularity가 기준점) */
	} else {
		range = 0;                    /* [한국어] datapoint가 너무 작아 range 0으로 클램프 */
	}

	return range;
}

/*
 * [한국어]
 * __spdk_histogram_data_get_bucket_index - range 내부에서 datapoint의 인덱스 계산.
 *
 * @h: 히스토그램 (granularity 참조).
 * @datapoint: 분류할 값.
 * @range: 이미 결정된 range (위 함수의 반환값).
 * @return: 0 ~ NUM_BUCKETS_PER_RANGE-1 범위의 인덱스.
 *
 * datapoint를 (range-1)만큼 우측 시프트한 후 BUCKET_MASK와 AND하여 추출.
 * range=0은 시프트 없이 datapoint 자체의 하위 비트 사용 (1:1 매핑).
 */
static inline uint32_t
__spdk_histogram_data_get_bucket_index(struct spdk_histogram_data *h, uint64_t datapoint,
				       uint32_t range)
{
	uint32_t shift;                   /* [한국어] datapoint에 적용할 우측 시프트량 */

	if (range == 0) {
		shift = 0;                    /* [한국어] range 0은 datapoint 자체가 인덱스 — 1:1 매핑 */
	} else {
		shift = range - 1;            /* [한국어] range가 커질수록 한 버킷이 담당하는 값의 폭이 2배씩 증가 */
	}

	return (datapoint >> shift) & SPDK_HISTOGRAM_BUCKET_MASK(h);
	                                  /* [한국어] 시프트 후 하위 granularity 비트만 추출 — range 내부 인덱스 0 ~ NUM_BUCKETS_PER_RANGE-1 */
}

/*
 * [한국어]
 * spdk_histogram_data_tally - 데이터포인트 1건을 히스토그램에 누적.
 *
 * @histogram: 대상 인스턴스.
 * @datapoint: 누적할 값 (예: I/O 완료 latency의 TSC delta).
 *
 * 핫패스 함수 — bdev I/O 완료마다 호출되므로 분기 최소화. 절차:
 *   1) datapoint의 range 계산.
 *   2) min/max_range로 saturate (밖이면 끝 버킷에 누적).
 *   3) range 내부 index 계산.
 *   4) 해당 버킷 카운터 ++.
 * 동기화: 단일 스레드 전용. reactor 모델에서 per-thread 히스토그램으로 운영.
 */
static inline void
spdk_histogram_data_tally(struct spdk_histogram_data *histogram, uint64_t datapoint)
{
	uint32_t range, index;            /* [한국어] 결정될 (range, index) 좌표 */

	range = __spdk_histogram_data_get_bucket_range(histogram, datapoint);
	                                  /* [한국어] 1단계: datapoint의 MSB 위치로 range 결정 */

	if (range < histogram->min_range) {
	                                  /* [한국어] alloc_sized_ext에서 min_val을 좁힌 경우, 너무 작은 값은 첫 버킷으로 saturate */
		range = histogram->min_range; /* [한국어] range를 min_range로 고정 */
		index = 0;                    /* [한국어] 그 range의 첫 버킷에 누적 */
	} else if (range > histogram->max_range) {
	                                  /* [한국어] max_val을 좁힌 경우, 너무 큰 값은 마지막 버킷으로 saturate */
		range = histogram->max_range; /* [한국어] max_range로 고정 */
		index = SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE(histogram) - 1;
		                              /* [한국어] 그 range의 마지막 버킷에 누적 */
	} else {
		index = __spdk_histogram_data_get_bucket_index(histogram, datapoint, range);
		                              /* [한국어] 정상 범위: range 내부 index 정확히 계산 */
	}

	__spdk_histogram_increment(histogram, range, index);
	                                  /* [한국어] (range, index) 버킷의 카운터 1 증가 */
}

/*
 * [한국어]
 * __spdk_histogram_data_get_bucket_start - (range, index) 버킷이 담당하는 값 범위의 끝(end) 계산.
 *
 * @h: 히스토그램.
 * @range: 좌표.
 * @index: 좌표.
 * @return: 이 버킷의 "다음 값 시작점" (즉 [prev_bucket_end, this_bucket_end) 범위).
 *
 * iterate()가 이 함수를 두 번 호출해 (last_bucket, bucket) 쌍으로 [start, end) 범위를 만든다.
 * 알고리즘:
 *   - range > 0: bucket = (1<<(range+granularity-1)) + (index+1) << (range-1)
 *     첫 항은 "이 range의 시작값", 둘째 항은 "range 내부 누적 폭".
 *   - range == 0: bucket = index+1 (1:1 매핑이므로 단순히 다음 값).
 */
static inline uint64_t
__spdk_histogram_data_get_bucket_start(const struct spdk_histogram_data *h, uint32_t range,
				       uint32_t index)
{
	uint64_t bucket;                  /* [한국어] 결과 — 이 버킷의 "다음 값 시작점" */

	index += 1;                       /* [한국어] 0-base를 1-base로 보정 — 끝값 계산이라 +1 필요 */
	if (range > 0) {
		bucket = 1ULL << (range + SPDK_HISTOGRAM_GRANULARITY(h) - 1);
		                              /* [한국어] range의 시작값 = 2^(range+granularity-1).
		                               * 예 g=7, range=2 → 2^8 = 256 (range 2의 첫 값) */
		bucket += (uint64_t)index << (range - 1);
		                              /* [한국어] range 내부 진행 폭 = index * 2^(range-1).
		                               * 한 버킷이 2^(range-1)개의 값을 담당하기 때문 */
	} else {
		bucket = index;               /* [한국어] range 0: 1 값/버킷이므로 단순히 index가 끝값 */
	}

	return bucket;
}

/*
 * [한국어]
 * spdk_histogram_data_fn - iterate() 콜백 시그니처.
 *
 * @ctx: 호출자가 iterate에 전달한 임의 컨텍스트 (퍼센타일 누산기 등).
 * @start: 이 버킷이 담당하는 값 범위의 시작 (포함).
 * @end:   이 버킷이 담당하는 값 범위의 끝 (제외) — start ≤ X < end.
 * @count: 이 버킷에 누적된 데이터포인트 수.
 * @total: 히스토그램 전체 데이터포인트 수 (모든 버킷 카운트의 합, iterate 시작 시 1회 계산).
 * @so_far: 지금까지(이 버킷 포함) 누적된 데이터포인트 수.
 *
 * P50/P99 계산: so_far/total ≥ 0.5/0.99인 첫 버킷의 end가 P50/P99 latency.
 * I/O latency 변환: end가 TSC면 사용자 코드가 spdk_get_ticks_hz()로 us 변환.
 */
typedef void (*spdk_histogram_data_fn)(void *ctx, uint64_t start, uint64_t end, uint64_t count,
				       uint64_t total, uint64_t so_far);

/*
 * [한국어]
 * spdk_histogram_data_iterate - 모든 버킷을 순서대로 순회하며 fn 콜백 호출.
 *
 * @histogram: 순회할 인스턴스.
 * @fn: 각 버킷마다 호출될 콜백 (위 typedef 참고).
 * @ctx: fn에 그대로 전달할 사용자 컨텍스트.
 *
 * 두 패스 알고리즘:
 *   Pass 1: 모든 버킷 카운트의 합 = total 계산 (퍼센타일 분모).
 *   Pass 2: 각 버킷마다 (last_bucket, bucket) = [start, end) 범위와 count,
 *           total, so_far(누적합)을 fn에 전달.
 * 호출 컨텍스트: 보통 RPC/CLI 명령으로 일회성 통계 덤프 시점. 핫패스 아님.
 * 동기화: const 인스턴스를 가정 — 순회 중 tally가 동시에 발생하면 결과 부정확.
 */
static inline void
spdk_histogram_data_iterate(const struct spdk_histogram_data *histogram,
			    spdk_histogram_data_fn fn, void *ctx)
{
	uint64_t i, j, count, so_far, total;
	                                  /* [한국어] i=range 인덱스, j=range 내부 인덱스, count/so_far/total는 콜백 인자 */
	uint64_t bucket, last_bucket;     /* [한국어] bucket=현재 버킷의 end값, last_bucket=이전 end값(=현재의 start) */

	total = 0;                        /* [한국어] 전체 카운트 합 — 퍼센타일 계산의 분모 */

	for (i = histogram->min_range; i <= histogram->max_range; i++) {
	                                  /* [한국어] Pass 1: 활성 range 전체 순회 */
		for (j = 0; j < SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE(histogram); j++) {
		                              /* [한국어] range 내부 모든 버킷 순회 */
			total += __spdk_histogram_get_count(histogram, i, j);
			                          /* [한국어] 버킷 카운트 누적 */
		}
	}

	so_far = 0;                       /* [한국어] 콜백에 전달될 누적합 시작 값 */
	bucket = 0;                       /* [한국어] last_bucket 초기값을 만들기 위한 0 (첫 버킷의 start = 0) */

	for (i = histogram->min_range; i <= histogram->max_range; i++) {
	                                  /* [한국어] Pass 2: 동일한 range/index 순서로 콜백 호출 */
		for (j = 0; j < SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE(histogram); j++) {
			count = __spdk_histogram_get_count(histogram, i, j);
			                          /* [한국어] 이 버킷의 카운트 */
			so_far += count;          /* [한국어] CDF (누적분포함수)용 합계 */
			last_bucket = bucket;     /* [한국어] 이전 버킷의 end가 현재 버킷의 start (인접 구간) */
			bucket = __spdk_histogram_data_get_bucket_start(histogram, i, j);
			                          /* [한국어] 현재 버킷의 end 계산 — 이름은 _start지만 실제로 "다음 시작점" = end */
			fn(ctx, last_bucket, bucket, count, total, so_far);
			                          /* [한국어] 사용자 콜백 호출 — [last_bucket, bucket) 범위와 통계 전달 */
		}
	}
}

/*
 * [한국어]
 * spdk_histogram_data_merge - 같은 형태의 두 히스토그램을 합산.
 *
 * @dst: 합산 결과를 누적할 대상 (in-out — bucket[i] += src->bucket[i]).
 * @src: 더할 원본 (불변).
 * @return: 0 성공 / -EINVAL: granularity 또는 range 범위가 다름.
 *
 * 사용 시점: per-thread(per-reactor) 히스토그램을 메인 히스토그램에 병합.
 *            reactor 모델에서 lockless 집계의 표준 패턴.
 * 제약: granularity와 min/max_range가 정확히 일치해야 함 — bucket[] 길이/의미가 같아야 합산 가능.
 */
static inline int
spdk_histogram_data_merge(const struct spdk_histogram_data *dst,
			  const struct spdk_histogram_data *src)
{
	uint64_t i;                       /* [한국어] 평탄 배열 순회 인덱스 */

	/* Histograms with different granularity values cannot be simply
	 * merged, because the buckets represent different ranges of
	 * values.
	 */
	if (dst->granularity != src->granularity) {
	                                  /* [한국어] granularity가 다르면 같은 인덱스라도 담당 값 범위가 달라 합산 의미 없음 */
		return -EINVAL;               /* [한국어] -EINVAL 반환 — 호출자가 미리 일치시켜야 함 */
	}

	/* Histogram with different size cannot be simply merged. */
	if (dst->min_range != src->min_range || dst->max_range != src->max_range) {
	                                  /* [한국어] range 범위가 다르면 bucket[] 배열 길이/의미가 다름 */
		return -EINVAL;               /* [한국어] 합산 거부 */
	}

	for (i = 0; i < SPDK_HISTOGRAM_NUM_BUCKETS(dst); i++) {
	                                  /* [한국어] 평탄 배열을 순회하며 src→dst 누적 — 좌표 분해 불필요 */
		dst->bucket[i] += src->bucket[i];
		                              /* [한국어] uint64_t 덧셈. overflow는 사실상 발생 불가능 (2^64 카운트) */
	}

	return 0;                         /* [한국어] 정상 완료 */
}

/**
 * Allocate a histogram data structure with specified granularity. It tracks datapoints
 * from min_val (inclusive) to max_val (exclusive).
 *
 * \param granularity Granularity of the histogram buckets. Each power-of-2 range is
 *                    split into (1 << granularity) buckets.
 * \param min_val The minimum value to be tracked, inclusive.
 * \param max_val The maximum value to be tracked, exclusive.
 *
 * \return A histogram data structure.
 */
/*
 * [한국어]
 * spdk_histogram_data_alloc_sized_ext - granularity와 추적 값 범위를 지정해 히스토그램 할당.
 *
 * @granularity: range당 1<<granularity 버킷.
 * @min_val: 추적할 최소값 (포함). 이보다 작은 datapoint는 첫 버킷으로 saturate.
 * @max_val: 추적할 최대값 (제외). 이보다 큰 datapoint는 마지막 버킷으로 saturate.
 * @return: 새로 할당된 히스토그램 포인터, 실패 시 NULL.
 *
 * 동작:
 *   1) 범위 검증 (min_val < max_val).
 *   2) struct spdk_histogram_data calloc.
 *   3) min_val/max_val을 bucket_range로 변환해 min_range/max_range 결정.
 *   4) bucket[] 배열을 NUM_BUCKETS 만큼 calloc (0 초기화).
 * 호출자: lib/bdev (히스토그램 enable 시), 사용자 앱.
 * 해제: spdk_histogram_data_free()로 짝맞춰 해제 필수.
 */
static inline struct spdk_histogram_data *
spdk_histogram_data_alloc_sized_ext(uint32_t granularity, uint64_t min_val, uint64_t max_val)
{
	struct spdk_histogram_data *h;    /* [한국어] 새로 할당될 인스턴스 */

	if (min_val >= max_val) {
	                                  /* [한국어] 빈 범위 또는 역방향 입력 거부 */
		return NULL;                  /* [한국어] 잘못된 인자 — NULL 반환 */
	}

	h = (struct spdk_histogram_data *)calloc(1, sizeof(*h));
	                                  /* [한국어] 구조체 자체를 0 초기화로 할당. C++ 호환을 위해 명시적 캐스트 */
	if (h == NULL) {
		return NULL;                  /* [한국어] 메모리 부족 */
	}

	h->granularity = granularity;     /* [한국어] 1단계: granularity 먼저 설정 — 아래 bucket_range 계산이 이 값을 사용 */
	h->min_range = __spdk_histogram_data_get_bucket_range(h, min_val);
	                                  /* [한국어] min_val의 MSB 위치를 min_range로 변환 — 추적 하한 결정 */
	h->max_range = __spdk_histogram_data_get_bucket_range(h, max_val - 1);
	                                  /* [한국어] max_val은 제외(exclusive)이므로 max_val-1로 변환 — 추적 상한 결정 */
	h->bucket = (uint64_t *)calloc(SPDK_HISTOGRAM_NUM_BUCKETS(h), sizeof(uint64_t));
	                                  /* [한국어] 결정된 range 범위로 정확한 bucket 배열 크기 할당. calloc으로 모두 0 초기화 */
	if (h->bucket == NULL) {
		free(h);                      /* [한국어] bucket 할당 실패 시 구조체도 해제 — 누수 방지 */
		return NULL;
	}

	return h;                         /* [한국어] 성공 — 호출자가 free()까지 책임 */
}

/*
 * [한국어]
 * spdk_histogram_data_alloc_sized - granularity만 지정해 [0, UINT64_MAX) 전 범위 추적.
 *
 * @granularity: range당 버킷 수의 log2.
 * @return: 새 히스토그램 또는 NULL.
 *
 * alloc_sized_ext의 wrapper — 값 범위 제한 없이 모든 데이터포인트 추적.
 * 메모리 사용량: 약 (range_count) * (1<<granularity) * 8 바이트
 *               (granularity=7, range=58 → 58 * 128 * 8 = 약 59KB).
 */
static inline struct spdk_histogram_data *
spdk_histogram_data_alloc_sized(uint32_t granularity)
{
	return spdk_histogram_data_alloc_sized_ext(granularity, 0, UINT64_MAX);
	                                  /* [한국어] 전체 64비트 값 추적 — 가장 일반적인 사용 패턴 */
}

/*
 * [한국어]
 * spdk_histogram_data_alloc - 기본 granularity(7)로 히스토그램 할당.
 *
 * 가장 짧은 형태의 alloc — 일반적인 latency 추적은 이 함수로 충분.
 * 2.3GHz CPU에서 7~14us 구간 50ns 해상도로 자동 구성.
 */
static inline struct spdk_histogram_data *
spdk_histogram_data_alloc(void)
{
	return spdk_histogram_data_alloc_sized(SPDK_HISTOGRAM_GRANULARITY_DEFAULT);
	                                  /* [한국어] DEFAULT=7, range당 128 버킷 */
}

/*
 * [한국어]
 * spdk_histogram_data_free - 히스토그램 해제 (bucket 배열 + 구조체).
 *
 * @h: 해제 대상 (NULL 허용 — 즉시 반환).
 *
 * alloc 계열로 할당된 인스턴스만 전달 가능. stack/static 인스턴스에는 사용 금지.
 */
static inline void
spdk_histogram_data_free(struct spdk_histogram_data *h)
{
	if (h == NULL) {
		return;                       /* [한국어] free(NULL) 안전성 — 호출자가 NULL 체크 생략 가능 */
	}

	free(h->bucket);                  /* [한국어] 1단계: bucket 배열 해제 (alloc 시 별도 calloc) */
	free(h);                          /* [한국어] 2단계: 구조체 자체 해제 */
}

#ifdef __cplusplus
}
#endif

#endif                                /* [한국어] _SPDK_HISTOGRAM_DATA_H_ 가드 닫기 */
