/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * CPU set management functions
 */

/*
 * [한국어 설명] CPU 집합 관리 API (cpuset.h)
 *
 * === 파일의 역할 ===
 * SPDK의 reactor·스레드 스케줄러가 "어떤 CPU 코어 집합에서 실행될 수 있는가"를
 * 표현하기 위한 타입 `struct spdk_cpuset`와 그 조작 API를 정의한다. 리눅스
 * 커널이 제공하는 `cpu_set_t`(sched.h)는 CPU_SETSIZE가 플랫폼에 따라 다르고
 * 조작 API가 매크로 위주라 가독성이 떨어지므로, SPDK는 고정 크기 비트맵
 * 1024-bit과 풀세트 연산 함수를 자체 정의한다.
 *
 * 제공 기능:
 *   - 할당/해제 (alloc/free)
 *   - 복사/비교/등가 (copy/equal)
 *   - 비트 연산 (and/or/xor/negate/zero)
 *   - 개별 CPU set/get
 *   - 순회 (for_each_cpu)
 *   - count
 *   - 텍스트 변환: 16진 마스크 ↔ CPU 리스트(`[0-3,5,7]`) 쌍방향
 *
 * 사용처: `-m 0x3` 같은 CLI 마스크 파싱, `--reactor-mask`, 스케줄러의
 * thread-to-core 할당 평가.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 초기화 단계(app/event_framework)에서 CPU 마스크 파싱에 사용되고, 런타임에
 * spdk_thread의 affinity 조정 등에 관여. 1024개 CPU까지 지원(현실 서버 기준
 * 충분). 런타임 비용은 비트 연산이라 경미.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h — 표준 타입/bool/uint32_t 등.
 * 구현: lib/util/cpuset.c
 * 의존하는 모듈: lib/event/reactor.c, lib/thread/thread.c(scheduler), app/*
 * 공유 자료구조: spdk_cpuset 객체는 값 의미론 — 복사/전달 자유.
 * 공유 상태: 없음 (내부 전역 상태 없음).
 *
 * === 주요 함수/구조체 요약 ===
 * 구조체:
 *   - spdk_cpuset: 1024-bit CPU 마스크 + 문자열 캐시 버퍼
 * 함수:
 *   - spdk_cpuset_{alloc,free,copy,equal}
 *   - spdk_cpuset_{and,or,xor,negate,zero}
 *   - spdk_cpuset_{set,get}_cpu, spdk_cpuset_count, spdk_cpuset_for_each_cpu
 *   - spdk_cpuset_fmt, spdk_cpuset_parse (텍스트 변환)
 */

#ifndef SPDK_CPUSET_H            /* [한국어] include 가드 */
#define SPDK_CPUSET_H

#include "spdk/stdinc.h"         /* [한국어] bool/uint32_t/size_t 확보 */

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_CPUSET_SIZE 1024
                                 /* [한국어] 지원하는 최대 CPU 수
                                  *  - 1024 = 128바이트 비트맵 — 고밀도 서버(예: EPYC 128코어 2소켓)까지 충분
                                  *  - 증가가 필요하면 이 상수만 바꾸고 재컴파일 */

/**
 * List of CPUs.
 */
struct spdk_cpuset {
	char str[SPDK_CPUSET_SIZE / 4 + 1];
                                 /* [한국어] 16진 문자열 캐시 — 1 hex char = 4 bits → 1024/4 = 256 chars + '\0'
                                  *  - spdk_cpuset_fmt()가 생성한 최근 문자열을 저장 (함수 반환 시점에 유효)
                                  *  - 설정자: spdk_cpuset_fmt
                                  *  - 읽는 자: 호출자 (필요 시 사본을 만들어야 함 — 다음 fmt 호출로 덮어씀) */
	uint8_t cpus[SPDK_CPUSET_SIZE / 8];
                                 /* [한국어] 실제 비트맵: 1024/8 = 128바이트
                                  *  - 비트 N: CPU 인덱스 N이 포함되면 1, 아니면 0
                                  *  - 설정자: spdk_cpuset_set_cpu/zero/negate/and/or/xor/copy/parse
                                  *  - 읽는 자: spdk_cpuset_get_cpu/for_each_cpu/count/equal/fmt
                                  *  - 동기화: 본 구조체는 비공유가 기본. 공유 시 외부 락 필요 */
};

/**
 * Allocate CPU set object.
 *
 * \return a pointer to the allocated zeroed cpuset on success, or NULL on failure.
 */
struct spdk_cpuset *spdk_cpuset_alloc(void);
/*
 * [한국어]
 * spdk_cpuset_alloc - 새 spdk_cpuset 할당(zero-init)
 *
 * @return: 성공 시 포인터, 실패 시 NULL (메모리 부족)
 *
 * 할당된 객체는 모든 CPU 비트가 0 상태로 초기화된다. 해제는 spdk_cpuset_free.
 * 실행 컨텍스트: 임의 스레드(초기화 단계 주로).
 */

/**
 * Free allocated CPU set.
 *
 * \param set CPU set to be freed.
 */
void spdk_cpuset_free(struct spdk_cpuset *set);
/*
 * [한국어]
 * spdk_cpuset_free - 할당 해제. set=NULL이면 no-op (free(NULL) 관례)
 */

/**
 * Compare two CPU sets.
 *
 * \param set1 CPU set1.
 * \param set2 CPU set2.
 *
 * \return true if both CPU sets are equal.
 */
bool spdk_cpuset_equal(const struct spdk_cpuset *set1, const struct spdk_cpuset *set2);
/*
 * [한국어]
 * spdk_cpuset_equal - 두 세트의 비트맵이 동일한지 검사 (memcmp)
 * 문자열 캐시는 비교 대상 아님.
 */

/**
 * Copy the content of CPU set to another.
 *
 * \param dst Destination CPU set
 * \param src Source CPU set
 */
void spdk_cpuset_copy(struct spdk_cpuset *dst, const struct spdk_cpuset *src);
/*
 * [한국어]
 * spdk_cpuset_copy - src 비트맵을 dst로 복사 (memcpy)
 */

/**
 * Perform AND operation on two CPU sets. The result is stored in dst.
 *
 * \param dst First argument of operation. This value also stores the result of operation.
 * \param src Second argument of operation.
 */
void spdk_cpuset_and(struct spdk_cpuset *dst, const struct spdk_cpuset *src);
/*
 * [한국어]
 * spdk_cpuset_and - dst = dst & src (비트별 AND)
 * 사용 예: 요청된 mask와 실제 사용 가능 mask를 교집합으로 유효 세트 계산
 */

/**
 * Perform OR operation on two CPU sets. The result is stored in dst.
 *
 * \param dst First argument of operation. This value also stores the result of operation.
 * \param src Second argument of operation.
 */
void spdk_cpuset_or(struct spdk_cpuset *dst, const struct spdk_cpuset *src);
/*
 * [한국어]
 * spdk_cpuset_or - dst = dst | src (합집합)
 */

/**
 * Perform XOR operation on two CPU sets. The result is stored in dst.
 *
 * \param dst First argument of operation. This value also stores the result of operation.
 * \param src Second argument of operation.
 */
void spdk_cpuset_xor(struct spdk_cpuset *dst, const struct spdk_cpuset *src);
/*
 * [한국어]
 * spdk_cpuset_xor - dst = dst ^ src (대칭 차집합) */

/**
 * Negate all CPUs in CPU set.
 *
 * \param set CPU set to be negated. This value also stores the result of operation.
 */
void spdk_cpuset_negate(struct spdk_cpuset *set);
/*
 * [한국어]
 * spdk_cpuset_negate - 모든 비트 반전 (보집합)
 * 주의: 유효 CPU 범위를 넘는 비트까지 1이 될 수 있어, 이후 사용 mask와 AND로 클리핑 필요
 */

/**
 * Clear all CPUs in CPU set.
 *
 * \param set CPU set to be cleared.
 */
void spdk_cpuset_zero(struct spdk_cpuset *set);
/*
 * [한국어]
 * spdk_cpuset_zero - 전체 비트를 0으로 (빈 집합)
 */

/**
 * Set or clear CPU state in CPU set.
 *
 * \param set CPU set object.
 * \param cpu CPU index to be set or cleared.
 * \param state *true* to set cpu, *false* to clear.
 */
void spdk_cpuset_set_cpu(struct spdk_cpuset *set, uint32_t cpu, bool state);
/*
 * [한국어]
 * spdk_cpuset_set_cpu - 특정 CPU 비트 on/off
 *
 * @cpu: 0 이상 SPDK_CPUSET_SIZE 미만. 범위를 벗어나면 구현에서 무시되거나 assert.
 * @state: true=포함, false=제외
 */

/**
 * Get the state of CPU in CPU set.
 *
 * \param set CPU set object.
 * \param cpu CPU index.
 *
 * \return the state of selected CPU.
 */
bool spdk_cpuset_get_cpu(const struct spdk_cpuset *set, uint32_t cpu);
/*
 * [한국어]
 * spdk_cpuset_get_cpu - 특정 CPU가 세트에 포함되는지 조회
 */

/** Call the specified function for each set cpu in the specified cpuset.
 *
 * \param set The cpuset to iterate
 * \param fn The function to call for each set cpu
 * \param ctx Context pointer to pass to fn
 */
void spdk_cpuset_for_each_cpu(const struct spdk_cpuset *set,
			      void (*fn)(void *ctx, uint32_t cpu), void *ctx);
/*
 * [한국어]
 * spdk_cpuset_for_each_cpu - 켜져 있는 각 CPU에 대해 콜백 호출
 *
 * @set: 순회 대상
 * @fn:  콜백. (ctx, cpu_index)
 * @ctx: 콜백에 전달할 사용자 컨텍스트
 *
 * 사용처: reactor 초기화 시 각 코어에 pthread spawn, affinity 적용 등.
 */

/**
 * Get the number of CPUs that are set in CPU set.
 *
 * \param set CPU set object.
 *
 * \return the number of CPUs.
 */
uint32_t spdk_cpuset_count(const struct spdk_cpuset *set);
/*
 * [한국어]
 * spdk_cpuset_count - 켜진 비트의 수(popcount) 반환
 */

/**
 * Convert a CPU set to hex string.
 *
 * \param set CPU set.
 *
 * \return a pointer to hexadecimal representation of CPU set. Buffer to store a
 * string is dynamically allocated internally and freed with CPU set object.
 * Memory returned by this function might be changed after subsequent calls to
 * this function so string should be copied by user.
 */
const char *spdk_cpuset_fmt(struct spdk_cpuset *set);
/*
 * [한국어]
 * spdk_cpuset_fmt - 세트를 16진 마스크 문자열로 직렬화
 *
 * @set: 대상 (내부 str[] 캐시 필드에 결과가 저장됨 → const 함수가 아님)
 * @return: set->str을 가리키는 포인터. 반환 즉시 사용해야 하며, 같은 set에 대한 후속 fmt 호출은 버퍼를 덮어쓴다.
 *
 * 주의: 반환 포인터는 set의 수명에 묶임. 길게 보관하려면 호출자가 strdup.
 */

/**
 * Convert a string containing a CPU core mask into a CPU set.
 *
 * \param set CPU set.
 * \param mask String defining CPU set. By default hexadecimal value is used or
 * as CPU list enclosed in square brackets defined as: 'c1[-c2][,c3[-c4],...]'.
 * When hexadecimal value is passed, any commas will be ignored, to allow using
 * this function with masks generated by Linux kernel in sysfs.
 *
 * \return zero if success, non zero if fails.
 */
int spdk_cpuset_parse(struct spdk_cpuset *set, const char *mask);
/*
 * [한국어]
 * spdk_cpuset_parse - 문자열 마스크를 세트로 파싱
 *
 * @set: 출력 (파싱 전에 zero 처리 후 채움)
 * @mask: 두 가지 포맷 지원
 *        (1) 16진 비트마스크: "0xff", "0xff,00ff,0000" (sysfs 스타일 콤마 무시)
 *        (2) CPU 리스트: "[0-3,5,7]" — 대괄호로 감싸고 범위·쉼표 조합
 * @return: 0 성공, 0이 아닌 값 실패
 */

#ifdef __cplusplus
}
#endif
#endif /* SPDK_CPUSET_H */        /* [한국어] include 가드 종료 */
