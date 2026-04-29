/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK 스레드 스케줄러 + 코어 거버너 공개 API 헤더 (scheduler.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK의 동적 스레드 배치(scheduler)와 코어 주파수 제어(governor) 인터페이스를
 * 정의한다. SPDK의 기본 모델은 "1 lcore = 1 reactor = N spdk_thread"이며, 각 spdk_thread는
 * 어느 lcore에 실행될지를 정수 lcore 필드로 표현한다. *스케줄러*는 주기적으로 모든 reactor의
 * 부하 통계(idle/busy TSC, poller 실행 시간 등)를 보고 thread.lcore 값을 재할당함으로써
 * 사실상 thread를 다른 코어로 *옮긴다*. *거버너*는 같은 통계를 바탕으로 실제 CPU의 P-state를
 * 올리거나(부하 증가) 내림으로써(부하 감소) 전력/성능 트레이드오프를 자동화한다.
 * 이 헤더는 두 인터페이스의 vtable(`struct spdk_scheduler`, `struct spdk_governor`)과
 * 등록 매크로(`SPDK_SCHEDULER_REGISTER`, `SPDK_GOVERNOR_REGISTER`), 그리고 사용자가
 * 활성 스케줄러/거버너를 전환·조회하는 setter/getter를 노출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 스케줄링은 한 reactor("scheduling reactor")에서 중앙집중적으로 수행된다.
 * 호출 체인:
 *   spdk_app_start → reactor 가동 → 사용자 RPC `framework_set_scheduler` 또는 자동
 *     → spdk_scheduler_set("dynamic"/"static"/"gscheduler" 등) [본 헤더]
 *     → 스케줄링 reactor의 주기 poller가 매 spdk_scheduler_get_period() 마이크로초마다 깨어남
 *         → 모든 reactor에서 spdk_scheduler_thread_info / spdk_scheduler_core_info를 수집
 *         → 활성 scheduler->balance(core_info, count) 호출
 *             → balance()는 thread_info[i].lcore = 새 lcore 값으로 직접 갱신
 *         → governor->core_freq_up/down/set_*() 등으로 P-state 조정
 *     → balance 결과대로 각 thread를 새 reactor에 재할당 (lib/event/scheduler 내부)
 * 실행 컨텍스트: 본 헤더의 모든 함수와 vtable 콜백은 *scheduling reactor* 위에서만 호출되어야
 * 한다 (cross-thread 호출 시 등록 TAILQ 보호가 깨짐).
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h, spdk/event.h(reactor 정의), spdk/json.h(get_opts/set_opts JSON I/O),
 * spdk/thread.h(`struct spdk_thread_stats`), spdk/util.h.
 * 의존받는 쪽: lib/event/scheduler_*.c(static/dynamic 스케줄러 구현), lib/event/scheduler.c
 * (스케줄링 reactor 본체), module/scheduler/(외부 거버너/스케줄러 모듈, 예: gscheduler).
 * 데이터 흐름:
 *   - spdk_thread_stats(busy/idle TSC) → spdk_scheduler_thread_info → balance() 입력.
 *   - balance() 출력 = thread_info[i].lcore 변경 → lib/event/scheduler가 이를 보고
 *     spdk_thread를 실제로 다른 reactor의 active list로 이동.
 *   - core_info.current_busy_tsc → governor->core_freq_up/down → DPDK rte_power → 커널 cpufreq.
 *
 * === 주요 함수/구조체 요약 ===
 * - SPDK_MAX_LCORE_FREQS: 코어당 사용 가능 주파수 목록 최대 길이 (DPDK RTE_MAX_LCORE_FREQS와 일치).
 * - struct spdk_governor_capabilities: 코어 능력 플래그 (priority core 여부 등).
 * - struct spdk_governor: 코어 주파수 제어 vtable (get/up/down/min/max + init/deinit + JSON dump).
 * - struct spdk_scheduler_thread_info: balance 입력 — 한 spdk_thread의 lcore + busy/idle TSC.
 * - struct spdk_scheduler_core_info: 한 lcore에 묶인 thread 배열 + 코어 단위 idle/busy TSC.
 * - struct spdk_scheduler: 스케줄러 vtable — init/deinit/balance + 옵션 JSON 셋/겟.
 * - SPDK_SCHEDULER_REGISTER / SPDK_GOVERNOR_REGISTER: C 생성자로 자기 자신을 SPDK에 등록.
 * - spdk_scheduler_set/get / spdk_governor_set/get: 활성 구현 전환·조회.
 * - spdk_scheduler_set_period / get_period: 스케줄링 주기 (마이크로초). 0=비활성.
 * - spdk_scheduler_get_scheduling_lcore / set_scheduling_lcore: 스케줄링 reactor 위치 조회·이동.
 */

#ifndef SPDK_SCHEDULER_H        /* [한국어] include guard 시작 — 다중 인클루드 방지. */
#define SPDK_SCHEDULER_H        /* [한국어] include guard 매크로 정의. */

#include "spdk/stdinc.h"        /* [한국어] uint32_t/uint64_t/bool 등 표준 타입을 한 번에 끌어옴. */

#ifdef __cplusplus              /* [한국어] C++에서 인클루드 시 이름 맹글링 방지를 위한 extern "C" 블록 시작. */
extern "C" {                    /* [한국어] 이하 선언은 C linkage. */
#endif

#include "spdk/event.h"         /* [한국어] reactor/lcore 관련 정의 — scheduler가 lcore 마스크와 reactor 정보를 다룸. */
#include "spdk/json.h"          /* [한국어] struct spdk_json_val / struct spdk_json_write_ctx — set_opts/get_opts에서 사용. */
#include "spdk/thread.h"        /* [한국어] struct spdk_thread_stats — 각 thread의 busy/idle TSC 통계 타입. */
#include "spdk/util.h"          /* [한국어] util 매크로(SPDK_CONTAINEROF, SPDK_COUNTOF 등) 호환성 위해 인클루드. */

/**
 * This matches the DPDK macro RTE_MAX_LCORE_FREQS
 */
#define	SPDK_MAX_LCORE_FREQS	64
/* [한국어] 한 lcore가 제공할 수 있는 사용 가능 주파수 단계의 최대 개수.
 * DPDK의 RTE_MAX_LCORE_FREQS와 동일 값으로 맞춰 governor가 DPDK rte_power API를
 * 그대로 활용할 수 있도록 한 약속. 일반적으로 Intel CPU의 P-state 단계 수보다 충분히 크다.
 * get_core_avail_freqs의 freqs 배열 크기 상한으로 쓰임. */

struct spdk_governor_capabilities {
	bool priority; /* Core with higher base frequency */
	/* [한국어] 이 코어가 동일 패키지 내 다른 코어보다 base frequency가 높은
	 * "priority core"(예: Intel Turbo Boost Max 3.0의 favored core) 인지 여부.
	 * 설정자: governor->get_core_capabilities 콜백이 채움.
	 * 읽는 자: scheduler가 latency-sensitive thread를 우선 배치할 코어 선정 시 참조.
	 * 값 범위: true=priority core, false=일반 코어. */
};
/* [한국어] struct spdk_governor_capabilities 자체는 코어별 능력 비트 모음 컨테이너.
 * 향후 SMT 자매 정보, c-state 지원 여부 등이 필드로 추가될 수 있음. */

/**
 * Cores governor
 * Implements core frequency control for schedulers. Functions from this structure
 * are invoked from scheduling reactor.
 */
/*
 * [한국어]
 * struct spdk_governor - CPU 주파수 제어 vtable (코어 거버너 인터페이스).
 *
 * 거버너는 스케줄러의 부하 판단 결과를 받아 해당 lcore의 P-state를 올리거나 내리는 역할을 한다.
 * 모든 콜백은 *scheduling reactor* 의 단일 스레드에서만 호출되므로 콜백 본문 내 동기화는 불필요.
 * 구현체는 보통 DPDK rte_power_*() API 위에 얇게 래핑된다.
 *
 * 라이프사이클: SPDK_GOVERNOR_REGISTER → spdk_governor_set(name) → init() →
 *               (런타임 콜백들) → spdk_governor_set(다른 이름) 또는 종료 → deinit().
 */
struct spdk_governor {
	const char *name;
	/* [한국어] 거버너 이름 (예: "dpdk_governor"). spdk_governor_set이 이 문자열로 매칭.
	 * 설정자: 구현체가 정적 문자열로 초기화.
	 * 읽는 자: spdk_governor_set, RPC 응답.
	 * 값 범위: NULL 불가, 등록된 다른 거버너와 충돌 금지. */

	/**
	 * Get available frequencies of a given core.
	 *
	 * \param lcore_id Core ID.
	 * \param freqs The buffer array to save the frequencies.
	 * \param num Number of frequencies to get.
	 *
	 * \return The number of frequencies returned in freqs. 0 on error.
	 *         0 is returned if it could not get the frequencies or
	 *         if the freqs array is too small to fit the returned frequencies.
	 */
	uint32_t (*get_core_avail_freqs)(uint32_t lcore_id, uint32_t *freqs, uint32_t num);
	/* [한국어] 사용 가능한 주파수 목록을 KHz/MHz 단위로 freqs 배열에 채워주는 콜백.
	 * 호출자는 SPDK_MAX_LCORE_FREQS 이상 크기의 버퍼를 권장. 반환값 0이면 실패 또는 버퍼 부족. */

	/**
	 * Get current frequency of a given core.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return Currently set core frequency.
	 */
	uint32_t (*get_core_curr_freq)(uint32_t lcore_id);
	/* [한국어] 현재 코어 주파수 조회. 단위는 구현체에 따름(보통 KHz).
	 * 0이면 조회 실패 또는 P-state 미지원. */

	/**
	 * Increase core frequency to next available one.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return 1 on success, 0 already at max frequency, negative on error.
	 */
	int (*core_freq_up)(uint32_t lcore_id);
	/* [한국어] 한 단계 위 P-state로 변경. 이미 최대면 0, 변경 성공이면 1, 실패면 음수.
	 * 부하 증가가 감지된 lcore에 대해 scheduler가 호출. */

	/**
	 * Decrease core frequency to next available one.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return 1 on success, 0 already at min frequency, negative on error.
	 */
	int (*core_freq_down)(uint32_t lcore_id);
	/* [한국어] 한 단계 아래 P-state로 변경. 이미 최소면 0, 성공 1, 실패 음수.
	 * 부하 하락 시 호출되어 전력 소비를 줄임. */

	/**
	 * Set core frequency to maximum available.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return 1 on success, 0 already at max frequency, negative on error.
	 */
	int (*set_core_freq_max)(uint32_t lcore_id);
	/* [한국어] 즉시 최고 P-state로 점프 (latency-sensitive 부하 진입 시 사용). */

	/**
	 * Set core frequency to minimum available.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return 1 on success, 0 already at min frequency, negative on error.
	 */
	int (*set_core_freq_min)(uint32_t lcore_id);
	/* [한국어] 즉시 최저 P-state로 점프 (idle 코어를 강제로 절전 상태에 두기 위함). */

	/**
	 * Get capabilities of a given core.
	 *
	 * \param lcore_id Core number.
	 * \param capabilities Structure to fill with capabilities data.
	 *
	 * \return 0 on success, negative on error.
	 */
	int (*get_core_capabilities)(uint32_t lcore_id, struct spdk_governor_capabilities *capabilities);
	/* [한국어] 코어별 능력(priority 등)을 capabilities 구조체에 채움.
	 * 스케줄러가 thread 배치 정책을 결정할 때 참고. */

	/**
	 * Output governor-specific information to a JSON stream.
	 *
	 * The JSON write context will be initialized with an open object, so the governor
	 * should write a name followed by a JSON value (most likely another nested object).
	 */
	int (*dump_info_json)(struct spdk_json_write_ctx *w);
	/* [한국어] RPC `framework_get_governor` 응답 등에서 거버너 자체 상태를 JSON으로 직렬화.
	 * caller는 이미 open object 상태로 w를 넘기므로 콜백은 키-값 쌍을 추가만 하면 됨. */

	/**
	 * Initialize a governor.
	 *
	 * \return 0 on success, non-zero on error.
	 */
	int (*init)(void);
	/* [한국어] 거버너 활성화 시(spdk_governor_set 호출 시) 한 번 실행되는 초기화.
	 * 주요 작업: rte_power_init(lcore) 호출, 자체 자료구조 할당. */

	/**
	 * Deinitialize a governor.
	 */
	void (*deinit)(void);
	/* [한국어] 거버너 교체 또는 종료 시 자원 정리 콜백. rte_power_exit 등 호출. */

	TAILQ_ENTRY(spdk_governor) link;
	/* [한국어] 등록된 모든 거버너를 잇는 BSD TAILQ 노드 포인터.
	 * 설정자: SPDK_GOVERNOR_REGISTER가 spdk_governor_register를 통해 push할 때.
	 * 읽는 자: spdk_governor_set이 이 리스트를 순회하며 name 매칭. */
};

/**
 * Set the current governor.
 *
 * Always deinitializes previously set governor.
 * No governor will be set if name parameter is NULL.
 * This function should be invoked on scheduling reactor.
 *
 * \param name Name of the governor to be used.
 *
 * \return 0 on success or non-zero on failure.
 */
/*
 * [한국어]
 * spdk_governor_set - 활성 거버너를 name으로 전환. 기존 거버너는 자동 deinit.
 *
 * @name:   매칭할 거버너 이름. NULL이면 거버너 비활성(주파수 제어 없음).
 * @return: 0=성공, 비-0=이름 매칭 실패 또는 init() 실패.
 *
 * 호출 컨텍스트: scheduling reactor에서만 호출. 다른 곳에서 부르면 vtable 교체와
 *               콜백 호출 사이의 race가 생길 수 있다.
 */
int spdk_governor_set(const char *name);

/**
 * Get currently set governor.
 *
 * \return a pointer to spdk governor or NULL if none is set.
 */
/*
 * [한국어]
 * spdk_governor_get - 현재 활성 거버너 포인터 반환.
 *
 * @return: 활성 거버너의 spdk_governor* 또는 NULL(=비활성).
 *          반환된 포인터는 라이브러리 소유 — 수정 금지, free 금지.
 *
 * 호출 컨텍스트: scheduling reactor 또는 RPC 핸들러.
 */
struct spdk_governor *spdk_governor_get(void);

/**
 * Add the given governor to the list of registered governors.
 * This function should be invoked by referencing the macro
 * SPDK_GOVERNOR_REGISTER in the governor c file.
 *
 * \param governor Governor to be added.
 *
 * \return 0 on success or non-zero on failure.
 */
/*
 * [한국어]
 * spdk_governor_register - 거버너를 등록 리스트에 추가. 직접 호출하지 말고
 *                          SPDK_GOVERNOR_REGISTER 매크로 사용을 권장.
 *
 * @governor: 정적 lifetime의 spdk_governor 인스턴스 포인터 (라이브러리는 포인터만 보관).
 * @return: void (코멘트의 "0/non-zero"는 매크로 기준 — 실제 시그니처는 void).
 *
 * 호출 시점: C 생성자(`__attribute__((constructor))`) 단계 — main() 진입 전에 호출됨.
 * 등록만 할 뿐 거버너를 활성화하지는 않는다. 활성화는 spdk_governor_set으로 별도 트리거.
 */
void spdk_governor_register(struct spdk_governor *governor);

/**
 * Macro used to register new governors.
 */
#define SPDK_GOVERNOR_REGISTER(governor) \
	static void __attribute__((constructor)) _spdk_governor_register_ ## governor(void) \
	{ \
		spdk_governor_register(&governor); \
	}
/* [한국어] 거버너 등록을 위한 표준 매크로 — 모듈 .c 파일 최하단에 한 번 사용한다.
 * 동작: GCC/clang `__attribute__((constructor))`가 main() 진입 전에 자동 실행되는
 *       함수를 정의 → 그 안에서 spdk_governor_register(&governor) 호출.
 * 결과: 사용자 코드가 부팅 시 별도로 register를 부를 필요 없이 모듈 링크만으로 등록 완료.
 * 토큰 결합(##)으로 거버너별 고유 함수명을 만들어 다중 모듈 링크 시 심볼 충돌을 방지한다. */

/**
 * Structure representing thread used for scheduling.
 */
/*
 * [한국어]
 * struct spdk_scheduler_thread_info - 스케줄러의 balance() 입력 단위:
 *                                       하나의 spdk_thread의 현재 위치와 부하 통계.
 *
 * scheduler가 이 구조체 안에서 lcore 필드를 *직접 수정* 함으로써 thread를 다른 코어로 옮긴다.
 * total_stats는 누적, current_stats는 직전 스케줄링 주기 동안의 변화량을 담는다.
 */
struct spdk_scheduler_thread_info {
	uint32_t lcore;
	/* [한국어] 이 spdk_thread가 현재 (또는 새로) 배치된 lcore ID.
	 * 설정자: 처음에는 thread 생성 시점의 lcore. balance() 호출 동안 scheduler가 자유롭게 변경.
	 * 읽는 자: lib/event/scheduler가 balance 반환 후 이 값을 보고 thread를 이동.
	 * 값 범위: reactor_mask에 포함된 lcore 중 하나. 마스크 밖 값은 무시되거나 에러 처리. */

	uint64_t thread_id;
	/* [한국어] 이 spdk_thread의 고유 ID (spdk_thread_get_id 결과).
	 * 설정자: lib/event/scheduler가 통계 수집 시 채움.
	 * 읽는 자: scheduler가 이전 주기와 동일 thread를 식별. balance에서 변경하면 안 됨. */

	/* stats over a lifetime of a thread */
	struct spdk_thread_stats total_stats;
	/* [한국어] thread 생성 후 누적 busy/idle TSC 카운트.
	 * 설정자: lib/thread가 spdk_thread_get_stats를 통해 채움.
	 * 읽는 자: scheduler가 장기 추세(예: 평균 busy %)를 평가할 때 사용. */

	/* stats during the last scheduling period */
	struct spdk_thread_stats current_stats;
	/* [한국어] 직전 한 스케줄링 주기 동안의 busy/idle 변화량 (delta).
	 * 설정자: lib/event/scheduler가 직전 측정치와 차분 계산해 채움.
	 * 읽는 자: scheduler — 즉각적 부하 변화 감지에 사용. balance의 핵심 입력. */
};

/**
 * A list of cores and threads which is used for scheduling.
 */
/*
 * [한국어]
 * struct spdk_scheduler_core_info - 한 lcore에 대한 종합 부하 정보 + 그 위의 thread 배열.
 *
 * scheduler->balance(core_info, count)에 N개 lcore분 배열로 전달되며,
 * scheduler는 각 코어의 idle/busy TSC와 thread_infos를 보고 thread.lcore를 재할당한다.
 */
struct spdk_scheduler_core_info {
	/* stats over a lifetime of a core */
	uint64_t total_idle_tsc;
	/* [한국어] 이 lcore가 시작 이후 총 누적된 idle TSC 카운트.
	 * 설정자: lib/event/scheduler가 reactor 통계에서 채움.
	 * 읽는 자: scheduler가 장기 평균 idle 비율 산출에 사용. */

	uint64_t total_busy_tsc;
	/* [한국어] 이 lcore의 누적 busy TSC.
	 * 의미: 모든 spdk_thread + reactor poller가 일 처리에 쓴 시간 합계. */

	/* stats during the last scheduling period */
	uint64_t current_idle_tsc;
	/* [한국어] 직전 스케줄링 주기 동안의 idle TSC 변화량.
	 * 직접적 부하 판단의 1차 지표 — 0에 가까울수록 코어가 포화. */

	uint64_t current_busy_tsc;
	/* [한국어] 직전 주기의 busy TSC 변화량.
	 * busy/(busy+idle) 비율로 즉각적인 부하율을 계산. */

	uint32_t lcore;
	/* [한국어] 이 정보가 가리키는 코어의 lcore ID.
	 * 설정자: 통계 수집기. 읽는 자: scheduler 및 governor (주파수 조정 대상 식별). */

	uint32_t threads_count;
	/* [한국어] thread_infos 배열의 길이 — 현재 이 lcore에 배치된 spdk_thread 수.
	 * 0이면 idle 코어 후보 (scheduler가 sleep 또는 freq_min을 검토). */

	bool interrupt_mode;
	/* [한국어] 이 reactor가 interrupt mode(eventfd/epoll)로 동작 중인지.
	 * 폴링 모드와 통계 해석 방식이 달라 scheduler가 분기 처리할 수 있도록 노출. */

	struct spdk_scheduler_thread_info *thread_infos;
	/* [한국어] 이 lcore에 배치된 spdk_thread들의 정보 배열 포인터.
	 * 소유권: lib/event/scheduler. balance 안에서 scheduler가 각 원소의 lcore 필드를 변경 가능.
	 * 다른 필드(thread_id, stats)는 read-only로 다뤄야 한다. */

	bool isolated;
	/* [한국어] 이 lcore가 "isolated"(격리)로 표시되어 스케줄링 대상에서 제외되는지.
	 * 의미: latency-critical thread를 단일 코어에 고정하기 위해 사용자가 명시적으로 격리.
	 * 값 범위: true=balance에서 thread 추가/제거 금지, false=정상 스케줄 대상. */
};

/**
 * Thread scheduler.
 * Functions from this structure are invoked from scheduling reactor.
 */
/*
 * [한국어]
 * struct spdk_scheduler - 스레드 배치 정책을 구현하는 vtable.
 *
 * SPDK는 기본적으로 "static"(부팅 시 정해진 lcore 고정)과 "dynamic"(부하 따라 이동) 두
 * 빌트인 스케줄러를 제공하며, 사용자는 SPDK_SCHEDULER_REGISTER로 자기 구현체를 추가 가능.
 * 모든 콜백은 *scheduling reactor* 단일 스레드에서만 호출 → 콜백 내 잠금 불필요.
 */
struct spdk_scheduler {
	const char *name;
	/* [한국어] 스케줄러 이름 (예: "static", "dynamic", "gscheduler").
	 * spdk_scheduler_set이 문자열 비교로 활성 스케줄러를 선정. */

	/**
	 * This function is called to initialize a scheduler.
	 *
	 * \return 0 on success or non-zero on failure.
	 */
	int (*init)(void);
	/* [한국어] 스케줄러가 활성화될 때(spdk_scheduler_set) 한 번 호출되는 초기화.
	 * 자체 상태(임계값, 히스토리 버퍼 등) 할당. 실패 시 비-0 반환 → spdk_scheduler_set 실패. */

	/**
	 * This function is called to deinitialize a scheduler.
	 */
	void (*deinit)(void);
	/* [한국어] 스케줄러 교체/종료 시 자원 정리 콜백. */

	/**
	 * Function to balance threads across cores by modifying
	 * the value of their lcore field.
	 *
	 * \param core_info Structure describing cores and threads on them.
	 * \param count Size of the core_info array.
	 */
	void (*balance)(struct spdk_scheduler_core_info *core_info, uint32_t count);
	/* [한국어] 핵심 정책 함수 — count개의 lcore 정보를 보고 각 thread_info[*].lcore를
	 * 새 값으로 갱신함으로써 thread 배치를 변경한다.
	 * 호출 주기: spdk_scheduler_get_period() 마이크로초마다 scheduling reactor가 자동 호출.
	 * 책임: lcore 필드만 변경, 다른 필드는 read-only.
	 * 정책 예: dynamic은 busy 비율이 높은 코어에서 idle 코어로 thread를 옮김 + governor freq_up 호출. */

	/**
	 * Function to set scheduler parameters like load_limit.
	 *
	 * \param opts Pointer to spdk_json_val struct containing values of parameters
	 * to be set in scheduler.
	 */
	int (*set_opts)(const struct spdk_json_val *opts);
	/* [한국어] RPC `framework_set_scheduler` 등으로 스케줄러 파라미터를 동적 변경할 때 호출.
	 * opts: JSON 객체로 디코딩된 키/값 쌍. 예: {"load_limit": 70}.
	 * 반환: 0=성공, 비-0=잘못된 키/값 → RPC 에러로 전파. */

	/**
	 * Function to get current scheduler parameters like load_limit.
	 *
	 * \param ctx Pointer to spdk_json_write_ctx struct to be filled with current parameters.
	 */
	void (*get_opts)(struct spdk_json_write_ctx *ctx);
	/* [한국어] 현재 스케줄러 파라미터를 JSON으로 직렬화.
	 * caller가 이미 object를 열어둔 상태로 ctx를 넘기므로 콜백은 키-값만 추가하면 된다. */

	TAILQ_ENTRY(spdk_scheduler)	link;
	/* [한국어] 등록된 모든 스케줄러를 잇는 TAILQ 노드.
	 * 설정자: SPDK_SCHEDULER_REGISTER → spdk_scheduler_register.
	 * 읽는 자: spdk_scheduler_set(name) 시 리스트 순회 매칭. */
};

/**
 * Change current scheduler. If another scheduler was used prior,
 * it will be deinitialized. No scheduler will be set if name parameter
 * is NULL.
 * This function should be invoked from scheduling reactor.
 *
 * \param name Name of the scheduler to be used.
 *
 * \return 0 on success or non-zero on failure.
 */
/*
 * [한국어]
 * spdk_scheduler_set - 활성 스케줄러를 name으로 전환. 기존 스케줄러는 자동 deinit.
 *
 * @name:   매칭할 스케줄러 이름. NULL이면 스케줄러 비활성(thread 자동 이동 없음).
 * @return: 0=성공, 비-0=이름 미매칭 또는 init() 실패.
 *
 * 호출 컨텍스트: scheduling reactor (RPC 핸들러가 자동으로 send_msg로 위임).
 */
int spdk_scheduler_set(const char *name);

/**
 * Get currently set scheduler.
 *
 * \return a pointer to spdk scheduler or NULL if none is set.
 */
/*
 * [한국어]
 * spdk_scheduler_get - 현재 활성 스케줄러 포인터 반환 (라이브러리 소유, 수정 금지).
 *
 * @return: 활성 spdk_scheduler* 또는 NULL.
 */
struct spdk_scheduler *spdk_scheduler_get(void);

/**
 * Change current scheduling period.
 * Setting period to 0 disables scheduling.
 *
 * \param period Period to set in microseconds.
 */
/*
 * [한국어]
 * spdk_scheduler_set_period - 스케줄러 balance() 호출 주기를 마이크로초 단위로 설정.
 *
 * @period: 0=스케줄링 완전 비활성(thread는 현재 lcore에 고정),
 *          >0=주어진 주기마다 balance() 자동 호출.
 *
 * 너무 짧으면 통계 노이즈에 반응해 thread가 끊임없이 이동하며 캐시 친화도가 깨지고,
 * 너무 길면 부하 변화에 둔감해진다. 일반적으로 100ms ~ 1s 범위가 권장.
 *
 * 호출 컨텍스트: scheduling reactor.
 */
void spdk_scheduler_set_period(uint64_t period);

/**
 * Get scheduling period of currently set scheduler.
 *
 * \return Scheduling period in microseconds.
 */
/*
 * [한국어]
 * spdk_scheduler_get_period - 현재 스케줄링 주기(마이크로초) 조회.
 *
 * @return: 0=비활성, >0=현재 주기.
 */
uint64_t spdk_scheduler_get_period(void);

/**
 * Add the given scheduler to the list of registered schedulers.
 * This function should be invoked by referencing the macro
 * SPDK_SCHEDULER_REGISTER in the scheduler c file.
 *
 * \param scheduler Scheduler to be added.
 */
/*
 * [한국어]
 * spdk_scheduler_register - 스케줄러를 등록 리스트에 추가. 매크로 사용을 권장.
 *
 * @scheduler: 정적 lifetime의 spdk_scheduler 인스턴스 포인터.
 * @return: void.
 *
 * 호출 시점: C 생성자(SPDK_SCHEDULER_REGISTER) — main() 이전.
 * 등록만 하고 활성화는 spdk_scheduler_set에서 별도로 트리거.
 */
void spdk_scheduler_register(struct spdk_scheduler *scheduler);

/**
 * Get lcore of scheduling reactor.
 *
 * All scheduler operations are performed from the scheduling reactor.
 *
 * \return lcore of scheduling reactor
 */
/*
 * [한국어]
 * spdk_scheduler_get_scheduling_lcore - 스케줄링 reactor가 도는 lcore ID 조회.
 *
 * @return: 활성 reactor 마스크 안의 lcore ID. 기본값은 main_core 또는 첫 번째 활성 lcore.
 *
 * 모든 scheduler/governor 콜백은 이 lcore의 reactor에서 호출된다.
 * 외부 스레드가 spdk_scheduler_set 등을 호출하려면 이 lcore로 send_msg를 보내야 한다.
 */
uint32_t spdk_scheduler_get_scheduling_lcore(void);

/**
 * Set scheduling reactor.
 *
 * All scheduler operations are performed from the scheduling reactor.
 *
 * \param lcore lcore of scheduling reactor
 */
/*
 * [한국어]
 * spdk_scheduler_set_scheduling_lcore - 스케줄링 reactor를 다른 lcore로 이동.
 *
 * @lcore:  새 스케줄링 reactor가 될 lcore ID. 활성 reactor_mask에 포함되어야 함.
 * @return: true=이동 성공, false=lcore 부적합 또는 reactor 미존재.
 *
 * 사용 예: 원래 main_core가 latency-critical 작업을 맡게 되면 scheduling 부담을 다른
 *         코어로 옮겨 jitter를 줄임.
 *
 * 호출 컨텍스트: 어떤 SPDK thread에서도 호출 가능 — 내부에서 send_msg로 안전하게 위임.
 */
bool spdk_scheduler_set_scheduling_lcore(uint32_t lcore);


/*
 * Macro used to register new scheduler.
 */
#define SPDK_SCHEDULER_REGISTER(scheduler) \
static void __attribute__((constructor)) _spdk_scheduler_register_ ## scheduler (void) \
{ \
	spdk_scheduler_register(&scheduler); \
}
/* [한국어] 스케줄러 자동 등록 매크로 — 거버너 매크로와 동일한 패턴.
 * 동작: __attribute__((constructor))가 main() 전에 실행되는 함수를 만들어 등록 호출.
 * 토큰 결합(##)으로 스케줄러 이름별 고유 심볼을 생성해 다중 모듈 링크 시 충돌을 방지.
 * 모듈 .c 파일 최하단에 SPDK_SCHEDULER_REGISTER(my_sched_struct)를 한 번 사용. */

#ifdef __cplusplus              /* [한국어] C++ extern "C" 블록 종결. */
}
#endif

#endif /* SPDK_SCHEDULER_H */    /* [한국어] include guard 종결. */
