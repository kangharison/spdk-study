/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Zipf random number distribution
 */

/*
 * [한국어 설명] Zipf 난수 분포 생성기 (zipf.h)
 *
 * === 파일의 역할 ===
 * Zipf 분포를 따르는 난수 생성기 API를 제공한다. Zipf 분포는 "rank-k 발생
 * 확률 ∝ 1/k^theta"로 요약되며, 현실 워크로드(웹 접근, 파일 인기도, 객체
 * 저장소의 key hotness 등)에서 자주 관찰된다. SPDK에서는 주로 성능 벤치마크
 * (spdk_nvme_perf, bdevperf)에서 "핫스팟 접근 패턴" 시뮬레이션에 사용해
 * 캐시·프리페치 효과를 측정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 구현: lib/util/zipf.c. 상태를 갖는 객체(spdk_zipf)를 통해 생성기별 시드/
 * 파라미터를 유지한다.
 * 실행 컨텍스트: 주로 테스트/벤치마크 컨텍스트. hot path는 아님.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h — uint64_t/uint32_t/double.
 * 의존하는 모듈: app/spdk_nvme_perf, test/bdev/bdevperf 등.
 * 공유 자료구조: struct spdk_zipf (opaque) — 헤더는 포인터만 노출.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_zipf: 불투명 생성기 객체
 *   - spdk_zipf_create(range, theta, seed): 새 생성기 할당·초기화
 *   - spdk_zipf_generate(zipf):             [0, range) 범위 값 반환
 *   - spdk_zipf_free(&zipfp):               해제 + 포인터 NULL 세팅
 */

#ifndef SPDK_ZIPF_H              /* [한국어] include 가드 */
#define SPDK_ZIPF_H

#include "spdk/stdinc.h"         /* [한국어] uint32_t/uint64_t 등 */

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_zipf;
                                 /* [한국어] opaque 타입 — 내부 필드는 lib/util/zipf.c에서만 정의·접근 */

/**
 * Create a zipf random number generator.
 *
 * Numbers from [0, range) will be returned by the generator when
 * calling \ref spdk_zipf_generate.
 *
 * \param range Range of values for the zipf distribution.
 * \param theta Theta distribution parameter.
 * \param seed Seed value for the random number generator.
 *
 * \return a pointer to the new zipf generator.
 */
struct spdk_zipf *spdk_zipf_create(uint64_t range, double theta, uint32_t seed);
/*
 * [한국어]
 * spdk_zipf_create - Zipf 생성기 할당·초기화
 *
 * @range: 반환 값 상한 (exclusive). 생성기는 [0, range) 정수 반환
 * @theta: 분포의 "편향도" — 0에 가까우면 균일, 1에 가까울수록 최고 rank에 편중
 * @seed: 내부 PRNG 시드
 * @return: 새 spdk_zipf* (호출자가 spdk_zipf_free로 해제)
 *
 * 구현은 Gray-Hellerstein "Quickly Generating Billion-Record Synthetic Databases"
 * 기법 — O(1) 생성 가능.
 */

/**
 * Free a zipf generator and set the pointer to NULL.
 *
 * \param zipfp Zipf generator to free.
 */
void spdk_zipf_free(struct spdk_zipf **zipfp);
/*
 * [한국어]
 * spdk_zipf_free - 생성기 해제 (포인터의 포인터 수령 → 호출측 포인터를 NULL로 설정)
 *
 * @zipfp: 해제 대상 포인터의 주소. *zipfp가 NULL이면 no-op. 성공 시 *zipfp = NULL.
 */

/**
 * Generate a value from the zipf generator.
 *
 * \param zipf Zipf generator to generate the value from.
 *
 * \return value in the range [0, range)
 */
uint64_t spdk_zipf_generate(struct spdk_zipf *zipf);
/*
 * [한국어]
 * spdk_zipf_generate - 분포에 따른 다음 값 생성
 *
 * 생성기 내부 상태가 갱신되므로 동일 zipf 객체를 여러 스레드에서 공유하면 데이터 경합 발생 —
 * 스레드별 별도 생성기 권장.
 */

#ifdef __cplusplus
}
#endif

#endif                           /* [한국어] include 가드 종료 */
