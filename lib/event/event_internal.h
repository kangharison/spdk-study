/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK event(reactor) 모듈 내부 헤더 (event_internal.h)
 *
 * === 파일의 역할 ===
 * SPDK는 1코어=1reactor=1user-thread 모델로 동작하며, 각 reactor는 여러 개의
 * "lightweight thread (lw_thread)"를 시간 분할로 스케줄링한다. 본 헤더는 그 lw_thread
 * 스케줄링 메타데이터(struct spdk_lw_thread)와 스케줄러 보조 함수(/proc/stat 파싱,
 * isolated core mask 조회/설정)를 lib/event 내부에서 공유하기 위한 정의를 모은다.
 * 공개 API는 spdk_internal/event.h(내부) 또는 spdk/event.h(공개)에서 다루고, 이 파일은
 * event 라이브러리의 .c 파일들끼리만 사용하는 strictly internal 인터페이스다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   spdk_app_start → reactor 생성(코어당 1개) → reactor 메인 루프
 *     → spdk_thread 객체들이 lw_thread로 래핑되어 reactor->thread_list에 등록됨
 *     → 스케줄러(scheduler.c)가 주기적으로 lw_thread의 실행 시간(TSC)을 측정하고
 *       부하가 높은 코어에서 부하가 낮은 코어로 thread를 옮긴다(reschedule).
 * 본 헤더의 spdk_lw_thread는 그 측정·이동에 필요한 통계와 위치 정보를 담는다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/cpuset, spdk/queue (TAILQ), spdk_internal/event(상위 정의).
 * - 의존받음: lib/event/reactor.c, lib/event/scheduler_*.c (dynamic, gscheduler 등).
 * - 데이터 흐름: 각 reactor가 자신의 spdk_lw_thread 통계를 갱신 → scheduler가
 *   주기적으로 모든 reactor의 통계를 모아 분석 → 필요 시 lw_thread를 다른 lcore로 이동.
 * - 공유 자료구조: spdk_lw_thread (per-thread 통계), 그리고 전역 isolated core mask
 *   (스케줄러가 절대 thread를 옮기지 않는 코어 집합).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_lw_thread       : reactor에 등록된 thread의 스케줄링 메타데이터.
 * - app_get_proc_stat           : Linux /proc/stat 파싱으로 코어별 사용 시간 측정.
 * - scheduler_get_isolated_core_mask: 격리된(스케줄링 제외) 코어 마스크 문자열 조회.
 * - scheduler_set_isolated_core_mask: 격리 코어 마스크 갱신.
 */

#ifndef EVENT_INTERNAL_H
#define EVENT_INTERNAL_H
/* [한국어] 다중 포함 가드. */

#include "spdk/stdinc.h" /* [한국어] SPDK 표준 라이브러리 래퍼 (stdint, stdbool, sys/queue 등). */
#include "spdk/cpuset.h" /* [한국어] spdk_cpuset 비트마스크 자료형 — 코어 집합 표현용. */
#include "spdk/queue.h"  /* [한국어] BSD-style sys/queue.h — TAILQ_ENTRY 매크로 사용. */
#include "spdk_internal/event.h" /* [한국어] event 모듈 내부 공통 정의 (spdk_thread_stats 등). */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 사용자에 대한 C 링키지 보장. */
#endif

struct spdk_lw_thread {
	/* [한국어] "Lightweight thread" 메타데이터 — spdk_thread를 reactor 스케줄러
	 * 관점에서 추적하는 부가 정보 컨테이너. spdk_thread 자체는 thread/thread.c가
	 * 관리하고, 이 구조체는 그 옆에 붙어 reactor·scheduler 간에 공유된다. */

	TAILQ_ENTRY(spdk_lw_thread)	link;
	/* [한국어] reactor의 lw_thread 리스트(TAILQ)에 자신을 끼우기 위한 링크 노드.
	 * 설정자: reactor가 _reactor_run의 등록/해지 시점에 갱신.
	 * 읽는 자: reactor 메인 루프(라운드로빈), scheduler walk.
	 * 동기화: 해당 리스트는 reactor 자기 코어에서만 변경되므로 락 불필요. */

	uint64_t			tsc_start;
	/* [한국어] 마지막으로 이 thread가 reactor에서 깨어나기 시작한 TSC(rdtsc) 값.
	 * 설정자: reactor가 thread를 dispatch 직전에 spdk_get_ticks().
	 * 읽는 자: dispatch가 끝난 뒤 (현재 - tsc_start)를 stats에 누적.
	 * 값 범위: TSC 카운터(시스템 부팅 후 단조 증가). */

	uint32_t                        lcore;
	/* [한국어] 현재 이 thread가 살고 있는 논리 코어 번호 (DPDK lcore_id).
	 * 설정자: 스케줄러가 reschedule 시 갱신.
	 * 읽는 자: rpc thread_get_stats, 디버깅 출력 등.
	 * 값 범위: 0 ~ RTE_MAX_LCORE-1. */

	uint32_t			initial_lcore;
	/* [한국어] thread 생성 당시 배치된 코어. resched 추적/통계용.
	 * 사용자가 명시한 코어 vs 스케줄러가 옮긴 후의 코어를 비교할 때 사용된다.
	 * 한 번 설정되면 변경되지 않는다. */

	bool				resched;
	/* [한국어] "다음 스케줄러 사이클에서 이 thread를 다른 코어로 옮기길 원함" 플래그.
	 * 설정자: scheduler 정책 함수(부하 측정 후 결론).
	 * 읽는 자: reactor 종료 hook이 다른 코어 reactor에 이동 메시지를 보낼 때.
	 * 동기화: scheduler 전용 마스터 코어에서만 변경되며, 대상 reactor는 매 사이클
	 *   끝에 atomic하게 한 번 읽고 처리. */

	/* stats over a lifetime of a thread */
	struct spdk_thread_stats	total_stats;
	/* [한국어] thread 생성 이후 누적된 busy/idle 통계.
	 * 설정자: 매 reactor dispatch가 끝날 때 누적.
	 * 읽는 자: rpc thread_get_stats 응답 빌더.
	 * 동기화: thread 자기 reactor에서만 갱신, 다른 코어는 snapshot read만. */

	/* stats during the last scheduling period */
	struct spdk_thread_stats	current_stats;
	/* [한국어] 현재 스케줄링 윈도우(예: 1초)에서의 busy/idle 통계.
	 * 스케줄러가 매 윈도우마다 0으로 리셋하고 누적 측정해, 윈도우 끝에 부하 평가에 사용.
	 * 설정자/리셋자: scheduler. 읽는 자: scheduler 정책 함수. */
};

/**
 * Parse proc/stat and get time spent processing system mode and user mode
 *
 * \param core Core which will be queried
 * \param usr Holds time [USER_HZ] spent processing in user mode
 * \param sys Holds time [USER_HZ] spent processing in system mode
 * \param irq Holds time [USER_HZ] spent processing interrupts
 *
 * \return 0 on success -1 on fail
 */
/*
 * [한국어]
 * app_get_proc_stat - Linux /proc/stat을 파싱해 특정 코어의 user/sys/irq 사용 시간 추출.
 *
 * @core: 조회할 논리 코어 번호.
 * @usr : 출력 — 사용자 모드 누적 시간 [USER_HZ tick 단위, 보통 1/100s].
 * @sys : 출력 — 커널 모드 누적 시간.
 * @irq : 출력 — 인터럽트 처리 시간.
 * @return: 성공 0, 실패 -1 (/proc/stat 읽기/파싱 실패 등).
 *
 * SPDK reactor는 polled-mode라 항상 100% busy로 보이지만, 스케줄러는 polling 안에서
 * 실제 일을 했는지를 위해 thread 자체의 TSC + /proc/stat의 시스템/IRQ 시간을 함께 본다.
 * 실행 컨텍스트: 스케줄러 마스터 코어 (주기적 polling).
 *
 * 호출 체인:
 *   scheduler_dynamic 정책 → [app_get_proc_stat] → fopen("/proc/stat") + sscanf
 */
int app_get_proc_stat(unsigned int core, uint64_t *usr, uint64_t *sys, uint64_t *irq);

/**
 * Get isolated CPU core mask.
 */
/*
 * [한국어]
 * scheduler_get_isolated_core_mask - 스케줄러가 절대 lw_thread를 옮기지 않는 코어 마스크 문자열을 조회.
 *
 * @return: 16진수 mask 문자열 ("0x3" 등) — 정적 버퍼 포인터, 호출자는 free 금지.
 *
 * "이 코어들은 사용자가 명시적으로 잡(예: NVMe-oF target 핵심 코어)을 박았으니
 * 스케줄러가 임의로 부하 분산하지 말라"는 정책에 사용된다.
 */
const char *scheduler_get_isolated_core_mask(void);

/**
 * Set isolated CPU core mask.
 */
/*
 * [한국어]
 * scheduler_set_isolated_core_mask - 격리 코어 마스크를 갱신.
 *
 * @isolated_core_mask: 새로 적용할 cpuset (값 복사).
 * @return: true = 적용 성공, false = 유효하지 않은 마스크(앱 cpuset 외 비트 포함 등).
 *
 * 호출 체인:
 *   RPC scheduler_set_options → [scheduler_set_isolated_core_mask] → 전역 마스크 갱신
 */
bool scheduler_set_isolated_core_mask(struct spdk_cpuset isolated_core_mask);

#ifdef __cplusplus
}
/* [한국어] extern "C" 종결. */
#endif

#endif /* EVENT_INTERNAL_H */
/* [한국어] 다중 포함 가드 종결. */
