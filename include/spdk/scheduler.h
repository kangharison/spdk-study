/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK 스레드 스케줄러 + 코어 거버너 공개 API 헤더 (scheduler.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK의 동적 스레드 배치(scheduler)와 코어 주파수 제어(governor)
 * 인터페이스를 정의한다. SPDK의 기본 실행 모델은 "1 lcore = 1 reactor = N spdk_thread"
 * 이며, 각 spdk_thread는 자신이 실행될 lcore를 정수 필드(thread.lcore)로 가진다.
 * 본 헤더는 두 종류의 vtable을 노출한다:
 *   1) struct spdk_scheduler - 주기적으로 (period 마이크로초마다) 모든 reactor의 부하
 *      통계(idle/busy TSC, poller 실행 시간 등)를 보고 thread.lcore 값을 갱신하여
 *      thread를 다른 코어로 *논리적으로 이동* 시키는 정책 모듈. "static"(고정),
 *      "dynamic"(부하 균형), "gscheduler"(외부 모듈) 등이 빌트인.
 *   2) struct spdk_governor - 같은 통계를 바탕으로 실제 CPU의 P-state(주파수)를 올리거나
 *      내려 전력/성능 트레이드오프를 자동화. DPDK rte_power_*() 위에 얇게 래핑되며
 *      그 아래로는 Linux cpufreq 또는 ACPI MSR을 사용한다.
 * 두 인터페이스는 의도적으로 분리되어 있다 — 한 노드에서 thread는 dynamic으로 균형
 * 잡되 frequency 제어는 하지 않거나, 반대로 thread는 static 고정해도 idle 코어의
 * frequency만 낮추는 식의 조합이 가능하다. 또한 등록 매크로(SPDK_SCHEDULER_REGISTER /
 * SPDK_GOVERNOR_REGISTER)를 통해 외부 모듈이 자기 구현체를 컴파일 시 자동 등록할 수
 * 있도록 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 스케줄링은 "scheduling reactor"라 불리는 단일 코어에 중앙집중되어 있다 —
 * cross-core 락 없이 한 코어가 글로벌 결정을 내린다. 호출 체인은 다음과 같다:
 *   spdk_app_start() -> spdk_reactors_start()
 *     -> 각 lcore 위에 reactor 가동
 *     -> 사용자 RPC `framework_set_scheduler` 또는 부팅 옵션으로 자동 활성
 *         -> spdk_scheduler_set("dynamic"/"static"/"gscheduler" 등) [본 헤더]
 *         -> spdk_scheduler_set_period(period_us)                   [본 헤더]
 *     -> scheduling reactor의 주기 poller가 매 period_us 마이크로초마다 깨어남
 *         (lib/event/scheduler.c 내부 _reactor_run_balance / scheduler_balance)
 *         -> 모든 reactor를 순회하며 reactor->total_idle_tsc / total_busy_tsc 수집
 *         -> 각 reactor 위의 모든 spdk_thread에 대해 spdk_thread_get_stats() 호출
 *         -> spdk_scheduler_thread_info[] / spdk_scheduler_core_info[] 채워서
 *            활성 scheduler->balance(core_info, count) 호출
 *             -> balance()는 thread_info[i].lcore 필드를 새 lcore 값으로 직접 갱신
 *             -> idle 비율이 높은 코어 -> freq_down, busy 코어 -> freq_up 등
 *                governor->core_freq_up/down/set_*() 호출
 *         -> balance 결과대로 lib/event/scheduler가 thread를 새 reactor의 active list로 이동
 *            (실제 이동은 spdk_thread_send_msg로 안전하게 위임)
 * 실행 컨텍스트: 본 헤더에 선언된 모든 함수와 모든 vtable 콜백은 *scheduling reactor* 의
 * 단일 스레드에서만 호출되어야 한다 — 그래야 등록 TAILQ와 활성 포인터에 락 없이
 * 접근 가능하다. 외부 스레드(예: RPC 핸들러)가 이를 호출하려면 내부적으로
 * spdk_thread_send_msg를 통해 scheduling reactor로 위임된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 헤더(아래 #include): spdk/stdinc.h(uint32_t/uint64_t/bool 등 표준 타입),
 * spdk/event.h(reactor/lcore 정의, RPC 옵션 자료구조), spdk/json.h(set_opts/get_opts
 * 콜백의 입출력 타입 spdk_json_val/spdk_json_write_ctx), spdk/thread.h
 * (spdk_thread_stats — busy/idle TSC 누적치를 담은 핵심 통계 구조체), spdk/util.h.
 * 이 헤더에 의존하는 모듈:
 *   - lib/event/scheduler.c                 — 스케줄링 reactor 본체 + 등록 TAILQ 관리.
 *   - lib/event/scheduler_static.c          — "static" 빌트인 스케줄러 (이동 없음).
 *   - lib/event/scheduler_dynamic.c         — "dynamic" 빌트인 스케줄러 (부하 따라 이동).
 *   - lib/event/scheduler_dpdk_governor.c   — DPDK rte_power 기반 거버너.
 *   - module/scheduler/                     — 외부 거버너/스케줄러 (예: gscheduler/, dpdk_governor/).
 *   - lib/event/rpc.c                       — framework_set_scheduler/framework_get_governor RPC.
 * 데이터 흐름:
 *   spdk_thread_stats(busy/idle TSC) -> spdk_scheduler_thread_info -> balance() 입력.
 *   balance() 출력 = thread_info[i].lcore 변경 -> lib/event/scheduler가 이를 보고
 *     spdk_thread를 실제로 다른 reactor의 active list로 이동.
 *   core_info.current_busy_tsc -> governor->core_freq_up/down -> DPDK rte_power
 *     -> Linux cpufreq sysfs(/sys/devices/system/cpu/cpu*/cpufreq/scaling_setspeed)
 *     -> 커널이 P-state MSR (예: IA32_PERF_CTL) 쓰기.
 * 공유 상태: spdk_scheduler/spdk_governor의 등록 TAILQ + 활성 포인터는 lib/event/scheduler.c
 * 의 file-static 변수로 보관되며, scheduling reactor의 단일 스레드 접근으로 lockless 보장.
 *
 * === 주요 함수/구조체 요약 ===
 *   - SPDK_MAX_LCORE_FREQS                 : 코어당 사용 가능 주파수 단계 최대치 (DPDK와 동일 64).
 *   - struct spdk_governor_capabilities    : priority core 등 코어 능력 비트 모음.
 *   - struct spdk_governor                 : 코어 주파수 제어 vtable
 *                                            (get/up/down/min/max + init/deinit + JSON dump).
 *   - struct spdk_scheduler_thread_info    : balance 입력 단위 — 한 spdk_thread의
 *                                            lcore + thread_id + total/current 통계.
 *   - struct spdk_scheduler_core_info      : 한 lcore 단위 종합 정보 (idle/busy TSC + thread 배열 +
 *                                            interrupt 모드 플래그 + isolated 플래그).
 *   - struct spdk_scheduler                : 스레드 배치 정책 vtable
 *                                            (init/deinit/balance + 옵션 JSON I/O).
 *   - SPDK_SCHEDULER_REGISTER / SPDK_GOVERNOR_REGISTER : C 생성자로 자동 등록하는 매크로.
 *   - spdk_scheduler_set/get / spdk_governor_set/get   : 활성 구현 전환·조회.
 *   - spdk_scheduler_set_period / get_period           : balance 호출 주기 (마이크로초).
 *                                                        0 = 스케줄링 비활성.
 *   - spdk_scheduler_get_scheduling_lcore / set_scheduling_lcore
 *                                                      : 스케줄링 reactor 위치 조회·이동.
 */

#ifndef SPDK_SCHEDULER_H
/* [한국어] include guard 시작 — 같은 컴파일 단위에서 scheduler.h가 다중 인클루드될 때
 * 중복 선언 에러를 방지한다. SPDK 공개 헤더의 일관된 컨벤션. */
#define SPDK_SCHEDULER_H
/* [한국어] include guard 매크로 정의. 위 #ifndef와 짝이 되어 두 번째 인클루드는 빈 파일로 본다. */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 인클루드 — uint32_t/uint64_t/bool/size_t 등 본 헤더에서 사용하는
 * 표준 정수·불리언 타입과 NULL 매크로를 일괄 끌어온다. 호스트 OS의 stdint.h, stdbool.h,
 * stddef.h 등을 plat-aware하게 묶어 둔 SPDK의 기본 헤더. */

#ifdef __cplusplus
/* [한국어] C++에서 이 헤더를 인클루드할 때 함수 심볼을 C linkage로 노출하여
 * 이름 맹글링을 막기 위한 가드. SPDK 라이브러리 본체는 C로 컴파일되므로
 * C++ 호출자는 mangled 심볼로는 링크할 수 없다. */
extern "C" {
/* [한국어] 이하 모든 선언은 C linkage로 처리되어 외부 객체 파일에서
 * "spdk_scheduler_set" 같은 unmangled 심볼로 바로 링크된다. */
#endif

#include "spdk/event.h"
/* [한국어] reactor/lcore 관련 정의를 가져옴. 본 헤더 자체는 reactor 구조체를 직접
 * 다루지 않지만, 사용자 코드가 scheduler 콜백 안에서 reactor mask나 lcore 마스크를
 * 다루기 위해 거의 항상 함께 인클루드된다. */
#include "spdk/json.h"
/* [한국어] struct spdk_json_val (디코드된 JSON 토큰) 와 struct spdk_json_write_ctx
 * (스트리밍 JSON writer) 정의를 가져옴. struct spdk_scheduler::set_opts/get_opts와
 * struct spdk_governor::dump_info_json 콜백의 인자 타입으로 직접 사용된다. */
#include "spdk/thread.h"
/* [한국어] struct spdk_thread_stats 정의를 가져옴 — 각 spdk_thread의 누적 busy/idle
 * TSC 카운터를 담은 통계 구조체로, scheduler의 balance() 입력 핵심 신호. */
#include "spdk/util.h"
/* [한국어] container_of/SPDK_COUNTOF/SPDK_CEIL_DIV 등 공통 매크로. 본 헤더 자체는
 * 직접 사용하지 않지만 scheduler 구현체가 거의 항상 함께 인클루드하므로 의존 트리
 * 안정화 차원에서 포함되어 있다. */

/**
 * This matches the DPDK macro RTE_MAX_LCORE_FREQS
 */
#define	SPDK_MAX_LCORE_FREQS	64
/* [한국어] 한 lcore가 보고할 수 있는 사용 가능 주파수 단계의 최대 개수.
 * DPDK의 RTE_MAX_LCORE_FREQS(64)와 의도적으로 동일 값 — governor 구현체가
 * DPDK rte_power_freqs() API에서 받은 배열을 그대로 SPDK 호출자에게 전달할 때
 * 버퍼 크기 불일치로 인한 truncate를 방지하기 위함이다.
 * 사용처: governor->get_core_avail_freqs(lcore_id, freqs, num)에서 freqs 버퍼의
 *         권장 최대 크기. 일반적인 Intel CPU의 P-state 단계는 16~32개 수준이므로
 *         64는 사실상 무한대로 봐도 무방.
 * 의미: 하드웨어가 더 많은 주파수 단계를 노출해도 64개까지만 보고된다 (안전한 truncate). */

struct spdk_governor_capabilities {
	bool priority;	/* Core with higher base frequency */
	/* [한국어] 이 코어가 동일 패키지 내 다른 코어보다 base frequency가 높은
	 * "priority core" 인지 여부를 나타내는 단일 비트 플래그.
	 * 배경: Intel Turbo Boost Max Technology 3.0(TBMT3) 등에서 패키지 내 1~2개
	 *       코어를 "favored core"로 식별하여 단일 스레드 부하를 그쪽으로 몰면 더 높은
	 *       turbo 주파수를 얻을 수 있다. 이 플래그가 true면 그런 코어임을 의미.
	 * 설정자: governor->get_core_capabilities(lcore_id, caps) 콜백이 채움.
	 *         DPDK rte_power_get_capabilities()의 결과를 매핑하는 것이 일반적.
	 * 읽는 자: scheduler->balance() — latency-sensitive thread를 우선 배치할 코어를
	 *         선정할 때 참조 (예: NVMe-oF target의 dispatcher thread).
	 * 값 범위: true=priority core, false=일반 코어.
	 * 동기화: scheduling reactor 단일 스레드에서만 읽기/쓰기 → 락 불필요. */
};
/* [한국어] struct spdk_governor_capabilities 자체는 코어별 능력 비트 모음 컨테이너.
 * 향후 확장 후보: SMT 자매 ID, 지원 c-state 깊이, hardware p-state(HWP) 지원 여부,
 * NUMA 노드 ID 등이 필드로 추가될 수 있다. 현재는 priority 하나만 정의. */

/**
 * Cores governor
 * Implements core frequency control for schedulers. Functions from this structure
 * are invoked from scheduling reactor.
 */
/*
 * [한국어]
 * struct spdk_governor - CPU 주파수 제어 vtable (코어 거버너 인터페이스).
 *
 * 거버너는 scheduler가 통계로부터 도출한 부하 판단을 받아 해당 lcore의 P-state를
 * 올리거나 내리는 역할을 분리해 담당한다. 두 인터페이스를 분리한 이유:
 *   - thread 재배치(scheduler)와 frequency 제어(governor)는 비용 구조와 부작용이
 *     완전히 다르다. thread 이동은 캐시 친화도 손실 + 마이그레이션 비용을 유발하고,
 *     frequency 변경은 latency spike + 전력 변동을 유발한다.
 *   - 사용자가 둘을 독립적으로 토글할 수 있어야 한다 (예: "thread는 static 고정,
 *     frequency만 동적 조정" 또는 그 반대).
 *
 * 모든 콜백은 *scheduling reactor* 의 단일 스레드에서만 호출되므로 콜백 본문 내
 * 동기화는 불필요. 구현체는 보통 DPDK rte_power_*() API 위에 얇게 래핑된다.
 *
 * 위험성:
 *   - core_freq_down은 idle로 보이는 코어의 주파수를 낮춰 전력을 절약하지만,
 *     그 코어로 부하가 다시 들어왔을 때 P-state 전환 latency(수십~수백 마이크로초)가
 *     I/O latency 스파이크로 나타날 수 있다.
 *   - 따라서 latency-sensitive 워크로드(예: NVMe-oF target)에서는 governor를
 *     비활성화(spdk_governor_set(NULL))하고 BIOS에서 P-state를 고정하는 것이
 *     권장되기도 한다.
 *
 * 라이프사이클:
 *   부팅: SPDK_GOVERNOR_REGISTER가 C 생성자에서 spdk_governor_register 호출
 *           -> 등록 TAILQ에 추가만 됨, 활성화는 별도.
 *   활성: spdk_governor_set("name") -> 매칭된 거버너 init() 콜백 호출
 *           -> rte_power_init(lcore) 등 내부 자원 할당.
 *   런타임: scheduling reactor가 매 balance마다 core_freq_up/down/get_curr_freq 등 호출.
 *   해제: spdk_governor_set(다른 이름) 또는 종료 시 deinit() 호출.
 */
struct spdk_governor {
	const char *name;
	/* [한국어] 거버너 이름 문자열 (예: "dpdk_governor").
	 * 설정자: 거버너 구현체가 정적 문자열로 초기화 (보통 .c 파일 상단의 정적 변수).
	 * 읽는 자: spdk_governor_set(name)이 등록 TAILQ를 순회하며 strcmp로 매칭.
	 *         RPC framework_get_governor 응답에 그대로 echo.
	 * 값 범위: NULL 불가. 등록된 다른 거버너와 충돌 금지 (충돌 시 register가 실패하거나
	 *         후순위가 무시될 수 있음 — 구현체에 따라).
	 * 메모리: 정적 lifetime이어야 하며 free 금지 — 라이브러리는 포인터만 보관. */

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
	/* [한국어] 사용 가능한 주파수 단계를 freqs 배열에 채워주는 콜백.
	 * @lcore_id: 조회할 lcore ID.
	 * @freqs:    호출자가 제공하는 출력 버퍼 (권장 크기 ≥ SPDK_MAX_LCORE_FREQS).
	 * @num:      freqs 배열의 capacity.
	 * @return:   실제 채워진 단계 수. 0이면 실패(드라이버 미지원) 또는 버퍼 부족.
	 *
	 * 단위: KHz가 일반적이지만 구현체에 따라 다를 수 있음(MHz도 가능) — DPDK는 KHz.
	 * 호출 시점: governor 활성화 직후 한 번 또는 RPC 응답 생성 시.
	 * 호출자: lib/event/scheduler 또는 RPC 핸들러.
	 * 캘리: rte_power_freqs(lcore_id, freqs, num) 래핑이 일반적. */

	/**
	 * Get current frequency of a given core.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return Currently set core frequency.
	 */
	uint32_t (*get_core_curr_freq)(uint32_t lcore_id);
	/* [한국어] 현재 코어 주파수 조회.
	 * @lcore_id: 조회할 lcore ID.
	 * @return:   현재 주파수 (단위는 get_core_avail_freqs와 동일, 보통 KHz).
	 *            0이면 조회 실패 또는 P-state 미지원 환경(가상화 등).
	 *
	 * 호출 빈도: balance마다 모든 코어에 대해 호출될 수 있으므로 비용이 낮아야 함
	 *           — 보통 sysfs cpufreq read 또는 MSR read 한 번 수준. */

	/**
	 * Increase core frequency to next available one.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return 1 on success, 0 already at max frequency, negative on error.
	 */
	int (*core_freq_up)(uint32_t lcore_id);
	/* [한국어] P-state를 한 단계 위로 올림 (주파수 증가).
	 * @return: 1=변경 성공, 0=이미 최고 단계, 음수=실패(예: 권한 없음).
	 *
	 * 호출 시점: scheduler->balance()가 해당 코어의 current_busy_tsc 비율이 임계값
	 *          (보통 70~90%)을 초과한 것을 발견했을 때.
	 * 부작용: P-state 전환 latency 발생 (Skylake 기준 수십 마이크로초). 따라서 너무
	 *        자주 호출하면 latency jitter의 원인이 될 수 있다 — scheduler period
	 *        조절로 빈도 통제. */

	/**
	 * Decrease core frequency to next available one.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return 1 on success, 0 already at min frequency, negative on error.
	 */
	int (*core_freq_down)(uint32_t lcore_id);
	/* [한국어] P-state를 한 단계 아래로 내림 (주파수 감소).
	 * @return: 1=변경 성공, 0=이미 최저 단계, 음수=실패.
	 *
	 * 호출 시점: idle 비율이 높은 (예: > 90%) 코어에 대해 전력 절약 목적.
	 * 위험: 다음 부하 도착 시 P-state ramp-up latency가 I/O latency에 직격으로 더해짐.
	 *      latency-sensitive 환경에서는 freq_down을 비활성화한 거버너를 쓰는 편이
	 *      안전하다. */

	/**
	 * Set core frequency to maximum available.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return 1 on success, 0 already at max frequency, negative on error.
	 */
	int (*set_core_freq_max)(uint32_t lcore_id);
	/* [한국어] 즉시 최고 P-state로 점프.
	 * @return: 1=성공, 0=이미 최고, 음수=실패.
	 *
	 * 호출 시점: latency-sensitive 부하 진입 직전(burst 예측) — 점진적 freq_up이
	 *          여러 사이클 걸리는 비용을 한 번에 단축. */

	/**
	 * Set core frequency to minimum available.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return 1 on success, 0 already at min frequency, negative on error.
	 */
	int (*set_core_freq_min)(uint32_t lcore_id);
	/* [한국어] 즉시 최저 P-state로 점프.
	 * @return: 1=성공, 0=이미 최저, 음수=실패.
	 *
	 * 호출 시점: 코어가 장기간 idle로 판정되어 폴링도 중단(인터럽트 모드 전환)
	 *          + 전력 절약을 강하게 추구할 때. set_core_freq_max의 대칭. */

	/**
	 * Get capabilities of a given core.
	 *
	 * \param lcore_id Core number.
	 * \param capabilities Structure to fill with capabilities data.
	 *
	 * \return 0 on success, negative on error.
	 */
	int (*get_core_capabilities)(uint32_t lcore_id, struct spdk_governor_capabilities *capabilities);
	/* [한국어] 코어별 능력(priority core 여부 등)을 capabilities에 채워줌.
	 * @lcore_id: 조회 대상 lcore.
	 * @capabilities: 출력 — 호출자가 할당한 구조체 포인터.
	 * @return: 0=성공, 음수=실패(드라이버 미지원).
	 *
	 * 사용처: scheduler가 thread 배치 정책 결정 시 1회성으로 조회. */

	/**
	 * Output governor-specific information to a JSON stream.
	 *
	 * The JSON write context will be initialized with an open object, so the governor
	 * should write a name followed by a JSON value (most likely another nested object).
	 */
	int (*dump_info_json)(struct spdk_json_write_ctx *w);
	/* [한국어] RPC `framework_get_governor` 응답 등에서 거버너 자체 상태를 JSON으로
	 * 직렬화하는 콜백.
	 * @w: 호출자(spdk_jsonrpc 핸들러)가 이미 spdk_json_write_object_begin으로 객체를
	 *     열어 둔 상태로 넘김. 콜백은 spdk_json_write_named_*() 시리즈로 키-값 쌍을
	 *     추가만 하면 됨 — object_end는 호출자가 책임.
	 * @return: 0=성공, 비-0=직렬화 실패. 실패 시 RPC 에러로 변환. */

	/**
	 * Initialize a governor.
	 *
	 * \return 0 on success, non-zero on error.
	 */
	int (*init)(void);
	/* [한국어] 거버너 활성화 시(spdk_governor_set 호출 시) 한 번 실행되는 초기화 콜백.
	 * @return: 0=성공, 비-0=실패 → spdk_governor_set 자체가 실패 반환.
	 *
	 * 주요 작업:
	 *   - 활성 lcore 마스크의 모든 코어에 rte_power_init(lcore) 호출.
	 *   - 자체 캐시(현재 freq, 단계 배열) 할당.
	 *   - 권한 확인(/dev/cpu_dma_latency, MSR access 등).
	 * 실패 사유: cpufreq driver 미적재, 가상화 환경에서 P-state 미지원, 권한 부족. */

	/**
	 * Deinitialize a governor.
	 */
	void (*deinit)(void);
	/* [한국어] 거버너 교체 또는 종료 시 자원 정리 콜백.
	 * 호출 시점: spdk_governor_set이 다른 이름으로 호출되거나 SPDK 종료 직전.
	 * 주요 작업: rte_power_exit(lcore), P-state를 BIOS 기본값으로 복원, 자체 자료구조 free. */

	TAILQ_ENTRY(spdk_governor) link;
	/* [한국어] 등록된 모든 거버너를 잇는 BSD TAILQ 노드 (intrusive list 엔트리).
	 * 설정자: SPDK_GOVERNOR_REGISTER -> spdk_governor_register가 TAILQ_INSERT_TAIL.
	 * 읽는 자: spdk_governor_set(name)이 TAILQ_FOREACH로 순회하며 name 매칭.
	 * 동기화: 등록은 main() 이전 C 생성자 단계라 단일 스레드, 검색은 scheduling reactor
	 *        단일 스레드 → 락 불필요.
	 * 메모리: link 자체는 spdk_governor 구조체 안에 임베드되므로 별도 할당 없음. */
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
 * spdk_governor_set - 활성 거버너를 name 문자열로 전환.
 *
 * @name:   매칭할 거버너 이름. NULL이면 거버너 비활성(주파수 제어 완전 정지) —
 *          이 경우 P-state는 BIOS/커널 기본 governor(예: ondemand, performance) 정책에
 *          맡겨진다.
 * @return: 0=성공, 비-0=이름 매칭 실패 또는 새 거버너의 init() 실패.
 *
 * 동작 단계:
 *   1) 현재 활성 거버너가 있으면 해당 거버너의 deinit() 호출.
 *   2) name이 NULL이면 활성 포인터를 NULL로 두고 0 반환.
 *   3) 등록 TAILQ에서 strcmp로 name 매칭.
 *   4) 매칭된 거버너의 init() 호출. 실패 시 활성 포인터는 NULL로 둠.
 *
 * 호출 컨텍스트: scheduling reactor에서만 호출. 다른 곳에서 부르면 vtable 교체와
 *               콜백 호출 사이의 race가 생길 수 있어 RPC 핸들러는 send_msg로 위임한다.
 * 호출 체인: rpc_framework_set_scheduler -> spdk_thread_send_msg(scheduling_thread, ...)
 *           -> spdk_governor_set.
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
 * @return: 활성 spdk_governor 포인터 또는 NULL(=비활성).
 *          반환된 포인터는 라이브러리 소유 — 수정/free 금지. 정적 lifetime이라
 *          호출자는 다음 spdk_governor_set 호출 전까지 유효함을 보장받는다.
 *
 * 호출 컨텍스트: scheduling reactor 또는 RPC 핸들러 (읽기 전용이므로 단일 스레드 가정
 *               하에서 다른 thread에서 호출되어도 atomicity는 유지되지만, 라이프사이클
 *               race를 피하려면 scheduling reactor 권장).
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
 * spdk_governor_register - 거버너를 등록 TAILQ에 추가.
 *                          직접 호출하지 말고 SPDK_GOVERNOR_REGISTER 매크로 사용 권장.
 *
 * @governor: 정적 lifetime의 spdk_governor 인스턴스 포인터. 라이브러리는 포인터만
 *            보관하므로 호출자는 free하지 말 것. 보통 거버너 .c 파일에 file-static
 *            전역으로 정의된 구조체를 가리킨다.
 *
 * 호출 시점: GCC/clang `__attribute__((constructor))` 단계 — main() 진입 전에 자동
 *           실행되는 매크로 생성 함수에서 호출. 등록만 할 뿐 활성화는 하지 않음.
 *           활성화는 spdk_governor_set으로 별도 트리거.
 *
 * 주의: 코멘트의 "0 on success / non-zero on failure"는 매크로의 의도 기준 표기일
 *       뿐 실제 시그니처는 void다 — 등록 실패는 일반적으로 추정되지 않으며,
 *       이름 충돌 시 동작은 구현체 별로 상이할 수 있음(첫 등록만 유효 등).
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
 * 사용 예: 거버너 .c 파일 끝에 `SPDK_GOVERNOR_REGISTER(my_governor_struct);` 한 줄.
 *
 * 동작 원리:
 *   1) GCC/clang 확장 `__attribute__((constructor))`로 표시된 정적 함수를 정의.
 *   2) 이 함수는 dynamic loader(ld.so) 또는 정적 링크 시 main() 진입 전에 자동 실행된다.
 *   3) 함수 본체에서 spdk_governor_register(&governor)를 호출 → 등록 TAILQ에 추가.
 *
 * 결과: 사용자 코드가 부팅 시 별도로 register를 호출하지 않아도 모듈 링크만으로
 *      등록이 완료된다. SPDK 전역의 자동 등록 패턴(SPDK_BDEV_MODULE_REGISTER,
 *      SPDK_NVMF_TRANSPORT_REGISTER 등)과 동일한 관용구.
 *
 * 토큰 결합 트릭(##):
 *   `_spdk_governor_register_ ## governor`는 매크로 인자 governor가 my_gov이면
 *   `_spdk_governor_register_my_gov`라는 고유 함수명을 만든다. 다중 거버너 모듈을
 *   동시에 링크할 때 함수명 충돌을 방지.
 *
 * 제약: 같은 컴파일 단위에서 같은 이름 인자로 두 번 사용하면 함수 재정의 에러. */

/**
 * Structure representing thread used for scheduling.
 */
/*
 * [한국어]
 * struct spdk_scheduler_thread_info - 스케줄러의 balance() 입력 단위:
 *                                       하나의 spdk_thread의 현재 위치와 부하 통계.
 *
 * 핵심 트릭: scheduler가 이 구조체 안의 lcore 필드를 *직접 수정* 함으로써 thread를
 *           다른 코어로 옮긴다. balance() 콜백 입장에서는 메모리 쓰기 한 번이지만,
 *           lib/event/scheduler가 그 결과를 보고 실제로 spdk_thread_send_msg를
 *           통해 안전하게 thread를 새 reactor의 active list로 이동시킨다.
 *
 * 통계 이중화: total_stats(누적) vs current_stats(직전 주기 변화량) 두 가지를 모두
 *             제공하여 scheduler가 "장기 추세"와 "즉각적 부하 변화"를 모두 활용할 수
 *             있도록 한다 — 예: dynamic 스케줄러는 current_stats로 즉각 균형을 잡되
 *             total_stats로 oscillation을 억제.
 */
struct spdk_scheduler_thread_info {
	uint32_t lcore;
	/* [한국어] 이 spdk_thread가 현재 (또는 새로) 배치된 lcore ID.
	 * 설정자: 처음에는 thread 생성 시점의 lcore. balance() 호출 동안 scheduler가
	 *        자유롭게 변경 가능 — 변경된 값이 곧 새 배치 결정.
	 * 읽는 자: lib/event/scheduler가 balance() 반환 후 이 값을 보고
	 *        spdk_thread_send_msg(target_lcore_thread, _move_thread, ...)로 이동 시도.
	 * 값 범위: 활성 reactor 마스크 안의 lcore 중 하나. 마스크 밖 값은 무시되거나
	 *        에러 처리되어 thread는 원래 자리에 남는다.
	 * 동기화: balance 안에서만 쓰기 가능 — scheduling reactor 단일 스레드. */

	uint64_t thread_id;
	/* [한국어] 이 spdk_thread의 고유 ID (spdk_thread_get_id 결과와 동일).
	 * 설정자: lib/event/scheduler가 통계 수집 시 채움.
	 * 읽는 자: scheduler 구현체가 이전 주기와 동일 thread를 식별하기 위해 사용
	 *         (예: 히스토리 기반 정책에서 thread별 EWMA를 유지할 때 키).
	 * 변경 금지: balance 콜백 안에서 이 값을 수정하면 thread 식별이 깨져 동작 불능. */

	/* stats over a lifetime of a thread */
	struct spdk_thread_stats total_stats;
	/* [한국어] 이 spdk_thread가 생성된 이후 누적된 busy/idle TSC 카운트.
	 * 출처: spdk_thread_get_stats(thread, &stats)의 결과 그대로.
	 *
	 * 필드 의미(struct spdk_thread_stats — 실제 정의는 thread.h):
	 *   - busy_tsc: 메시지 처리 + poller 실행에 쓴 누적 TSC.
	 *   - idle_tsc: poll 루프가 SPDK_POLLER_IDLE을 받아 일이 없었던 누적 TSC.
	 *
	 * 사용 예: scheduler가 장기 추세(예: thread 평균 busy %)를 평가해 oscillation을
	 *        억제하거나, busy_ratio = busy_tsc / (busy_tsc + idle_tsc) 형태의 KPI 산출. */

	/* stats during the last scheduling period */
	struct spdk_thread_stats current_stats;
	/* [한국어] 직전 한 스케줄링 주기 동안의 busy/idle TSC 변화량 (delta).
	 * 산출: lib/event/scheduler가 직전 측정치(prev_total_stats)와 현재(total_stats)의
	 *      차분을 계산해 채움 → balance 입장에서는 추가 계산 없이 즉시 사용 가능.
	 *
	 * 핵심 신호: 즉각적 부하 변화 감지. dynamic 스케줄러의 balance() 핵심 입력 —
	 *          current_stats.busy_tsc / period_tsc 가 1에 가까우면 thread가 코어를
	 *          포화시키고 있다는 신호.
	 *
	 * 주의: period가 너무 짧으면 노이즈로 가득 차 thread가 쉴 새 없이 이동하며
	 *      캐시 친화도가 깨진다 — spdk_scheduler_set_period 권장값 100ms~1s 참고. */
};

/**
 * A list of cores and threads which is used for scheduling.
 */
/*
 * [한국어]
 * struct spdk_scheduler_core_info - 한 lcore에 대한 종합 부하 정보 + 그 위의 thread 배열.
 *
 * scheduler->balance(core_info, count)에 N개 lcore분의 배열로 전달되며, scheduler는
 * 이 배열을 순회하면서 각 코어의 idle/busy TSC와 thread_infos를 보고 thread.lcore를
 * 재할당한다. governor가 함께 호출되는 경우 current_busy_tsc가 freq_up/down의
 * 주된 판단 기준이 된다.
 *
 * "busy_ratio"의 정의:
 *   부하 = current_busy_tsc / (current_busy_tsc + current_idle_tsc)
 *   값이 1에 가까울수록 코어 포화. dynamic 스케줄러의 임계 비교에 사용.
 */
struct spdk_scheduler_core_info {
	/* stats over a lifetime of a core */
	uint64_t total_idle_tsc;
	/* [한국어] 이 lcore가 시작 이후 총 누적된 idle TSC 카운트.
	 * 설정자: lib/event/scheduler가 reactor의 누적 통계에서 채움.
	 * 읽는 자: scheduler가 장기 평균 idle 비율 산출에 사용 — 단기 노이즈 억제 목적.
	 * 단위: TSC tick (CPU 주파수 의존). 절대 시간으로 변환하려면 spdk_get_ticks_hz()로 나눔. */

	uint64_t total_busy_tsc;
	/* [한국어] 이 lcore의 누적 busy TSC.
	 * 의미: 이 lcore 위의 모든 spdk_thread + reactor poller가 일 처리에 쓴 시간 합계.
	 * 설정자: lib/event/scheduler. 읽는 자: scheduler — 장기 부하 평균. */

	/* stats during the last scheduling period */
	uint64_t current_idle_tsc;
	/* [한국어] 직전 스케줄링 주기 동안의 idle TSC 변화량 (delta).
	 * 산출: total_idle_tsc - prev_total_idle_tsc.
	 * 직접적 부하 판단의 1차 지표 — 0에 가까울수록 코어가 포화되어 있다.
	 * governor의 freq_down 결정 임계값 기준이 되는 신호. */

	uint64_t current_busy_tsc;
	/* [한국어] 직전 주기의 busy TSC 변화량.
	 * 핵심 사용:
	 *   - busy_ratio = current_busy_tsc / (current_busy_tsc + current_idle_tsc) 산출.
	 *   - dynamic 스케줄러: ratio > load_limit(기본 50%)이면 이 코어의 thread 일부를
	 *     idle 코어로 옮김.
	 *   - governor: ratio > 70%이면 freq_up, < 10%이면 freq_down 등 정책. */

	uint32_t lcore;
	/* [한국어] 이 정보가 가리키는 코어의 lcore ID.
	 * 설정자: 통계 수집기. 읽는 자: scheduler 및 governor (주파수 조정 대상 식별).
	 * thread_infos 배열의 lcore와 동일하다 — 결정 직후엔 다를 수 있음(이동 결과). */

	uint32_t threads_count;
	/* [한국어] thread_infos 배열의 길이 — 현재 이 lcore에 배치된 spdk_thread 수.
	 * 0이면 idle 코어 후보 (scheduler가 sleep 또는 freq_min을 검토하거나, 다른
	 *      코어에서 thread를 이리로 옮기는 균형 조정의 목적지로 활용).
	 * 호출자가 thread_infos[i] 접근 시 i < threads_count 범위를 반드시 지켜야 함. */

	bool interrupt_mode;
	/* [한국어] 이 reactor가 polling 모드가 아닌 interrupt mode(eventfd/epoll)로
	 * 동작 중인지를 나타내는 플래그.
	 * 의미: SPDK 21.07+의 인터럽트 모드 — idle 코어는 epoll_wait로 잠들고 fd 도착 시 깨어남.
	 * 통계 해석 차이: interrupt 모드에서 idle TSC는 pthread가 실제 sleep한 시간이라
	 *               polling 모드의 "비어있는 폴 루프 시간"과 의미가 다르다.
	 * scheduler는 이 플래그로 분기하여 다른 임계값을 적용할 수 있다. */

	struct spdk_scheduler_thread_info *thread_infos;
	/* [한국어] 이 lcore에 배치된 spdk_thread들의 정보 배열 포인터.
	 * 소유권: lib/event/scheduler. balance() 호출 동안 유효, 반환 후 무효화될 수 있음.
	 * 변경 가능 필드: 각 원소의 lcore (= 새 배치 결정).
	 * 변경 금지 필드: thread_id, total_stats, current_stats — read-only로 다룰 것.
	 * 인덱스 범위: 0 ~ threads_count - 1. */

	bool isolated;
	/* [한국어] 이 lcore가 "isolated"(격리)로 표시되어 스케줄링 대상에서 제외되는지.
	 * 의미: 사용자가 latency-critical thread를 단일 코어에 고정하기 위해 명시적으로
	 *      격리한 코어 — scheduler는 이 코어로/에서 thread를 이동시키지 않아야 한다.
	 * 값 범위: true=balance에서 thread 추가/제거 금지(스킵), false=정상 스케줄 대상.
	 * 설정자: 사용자 설정(예: --main-core + --reactor-mask 분리, 또는 RPC). */
};

/**
 * Thread scheduler.
 * Functions from this structure are invoked from scheduling reactor.
 */
/*
 * [한국어]
 * struct spdk_scheduler - 스레드 배치 정책을 구현하는 vtable.
 *
 * 빌트인 스케줄러:
 *   - "static"     : 부팅 시 정해진 lcore에 thread를 영구 고정. balance가 no-op.
 *                    캐시 친화도 최대화 + 예측 가능한 latency. 부하 불균형 발생 시 그대로 둠.
 *   - "dynamic"    : 부하 따라 thread를 이동. busy 코어 → idle 코어로 thread 옮기고
 *                    governor와 결합해 freq 조정까지 수행. load_limit 옵션으로 임계값 조절.
 *   - "gscheduler" : 외부 모듈(module/scheduler/gscheduler/) — 더 정교한 정책.
 *
 * 사용자는 SPDK_SCHEDULER_REGISTER로 자기 구현체를 추가 가능 (커스텀 정책 — 예: ML 기반
 * 부하 예측, NUMA-aware 배치 등).
 *
 * 모든 콜백은 *scheduling reactor* 단일 스레드에서만 호출 → 콜백 내 잠금 불필요.
 * 콜백 안에서 다른 thread의 자료에 접근하려면 spdk_thread_send_msg를 통해 요청만 보내고
 * 결과는 다음 balance에서 통계로 관찰해야 한다.
 *
 * 라이프사이클:
 *   부팅: SPDK_SCHEDULER_REGISTER -> 등록 TAILQ 추가.
 *   활성: spdk_scheduler_set("name") -> init() 호출.
 *   런타임: scheduling reactor가 매 period_us마다 balance() 호출.
 *   해제: spdk_scheduler_set(다른 이름/NULL) 또는 종료 -> deinit().
 */
struct spdk_scheduler {
	const char *name;
	/* [한국어] 스케줄러 이름 (예: "static", "dynamic", "gscheduler").
	 * 설정자: 구현체가 정적 문자열로 초기화.
	 * 읽는 자: spdk_scheduler_set이 strcmp로 활성 스케줄러 선정.
	 *         RPC framework_get_scheduler 응답에 echo.
	 * 메모리: 정적 lifetime 필수, free 금지. */

	/**
	 * This function is called to initialize a scheduler.
	 *
	 * \return 0 on success or non-zero on failure.
	 */
	int (*init)(void);
	/* [한국어] 스케줄러가 활성화될 때(spdk_scheduler_set) 한 번 호출되는 초기화.
	 * @return: 0=성공, 비-0=실패 → spdk_scheduler_set이 실패 반환.
	 *
	 * 주요 작업:
	 *   - 자체 상태(임계값 default 값, 히스토리 버퍼, EWMA 가중치 등) 할당.
	 *   - g_scheduler_period 등 전역 설정 읽기.
	 * 호출 컨텍스트: scheduling reactor 단일 스레드. */

	/**
	 * This function is called to deinitialize a scheduler.
	 */
	void (*deinit)(void);
	/* [한국어] 스케줄러 교체/종료 시 자원 정리 콜백.
	 * 호출 시점: spdk_scheduler_set이 다른 이름으로 호출되거나 SPDK 종료.
	 * 의무: init에서 할당한 모든 자원 해제. 진행 중인 비동기 작업이 있으면
	 *       완료 대기 또는 취소. */

	/**
	 * Function to balance threads across cores by modifying
	 * the value of their lcore field.
	 *
	 * \param core_info Structure describing cores and threads on them.
	 * \param count Size of the core_info array.
	 */
	void (*balance)(struct spdk_scheduler_core_info *core_info, uint32_t count);
	/* [한국어] 핵심 정책 함수 — count개의 lcore 정보를 받아 각 thread_info의 lcore
	 *           필드를 새 값으로 갱신함으로써 thread 배치를 변경한다.
	 *
	 * @core_info: 길이 count의 spdk_scheduler_core_info 배열.
	 *             각 원소는 한 lcore의 idle/busy TSC + 그 위의 thread 정보 + isolated 플래그.
	 * @count:     배열 길이 — 활성 reactor 수와 동일.
	 *
	 * 호출 주기: spdk_scheduler_get_period() 마이크로초마다 scheduling reactor가 자동 호출.
	 *
	 * 책임/제약:
	 *   - thread_info[i].lcore 필드만 수정 가능. 다른 필드는 read-only.
	 *   - 직접 spdk_thread_send_msg나 reactor 자료에 접근하지 말 것 — race 위험.
	 *   - core_info 배열 자체는 함수 반환 후 무효화되므로 포인터 보관 금지.
	 *
	 * 정책 예 (dynamic 스케줄러):
	 *   1) busy_ratio > load_limit 인 코어 후보를 모음.
	 *   2) idle 코어로 그 위의 thread 일부를 이동(thread_info.lcore 갱신).
	 *   3) 비어버린 코어가 있으면 governor->set_core_freq_min 호출.
	 *   4) 포화된 코어에는 governor->core_freq_up 호출.
	 *
	 * busy_ratio 측정 메커니즘:
	 *   각 reactor의 무한 polling 루프(spdk_thread_poll)가 한 번 돌 때마다, poller가
	 *   SPDK_POLLER_BUSY를 반환하면 elapsed TSC를 busy_tsc에 누적, IDLE이면 idle_tsc에
	 *   누적한다. 이 누적치가 spdk_thread_stats로 노출되고, lib/event/scheduler가 이를
	 *   직전 측정치와의 차분(current_*_tsc)으로 변환해 본 콜백에 전달. */

	/**
	 * Function to set scheduler parameters like load_limit.
	 *
	 * \param opts Pointer to spdk_json_val struct containing values of parameters
	 * to be set in scheduler.
	 */
	int (*set_opts)(const struct spdk_json_val *opts);
	/* [한국어] RPC `framework_set_scheduler` 등으로 스케줄러 파라미터를 동적 변경하는 콜백.
	 * @opts: JSON 객체로 디코딩된 키/값 쌍 배열. 예: {"load_limit": 70, "core_limit": 80}.
	 * @return: 0=성공, 비-0=잘못된 키/값 → RPC 에러로 전파.
	 *
	 * 사용 예: dynamic 스케줄러는 load_limit(busy_ratio 임계값)을 런타임에 조정 가능.
	 * 호출 컨텍스트: scheduling reactor (RPC가 send_msg로 위임). */

	/**
	 * Function to get current scheduler parameters like load_limit.
	 *
	 * \param ctx Pointer to spdk_json_write_ctx struct to be filled with current parameters.
	 */
	void (*get_opts)(struct spdk_json_write_ctx *ctx);
	/* [한국어] 현재 스케줄러 파라미터를 JSON으로 직렬화하는 콜백.
	 * @ctx: 호출자가 이미 spdk_json_write_object_begin으로 객체를 열어둔 상태.
	 *       콜백은 spdk_json_write_named_uint32(ctx, "load_limit", ...) 식으로 키-값만 추가.
	 *       object_end는 호출자 책임. */

	TAILQ_ENTRY(spdk_scheduler)	link;
	/* [한국어] 등록된 모든 스케줄러를 잇는 BSD TAILQ 노드 (intrusive list 엔트리).
	 * 설정자: SPDK_SCHEDULER_REGISTER -> spdk_scheduler_register가 INSERT_TAIL.
	 * 읽는 자: spdk_scheduler_set(name) 시 TAILQ_FOREACH로 순회 매칭.
	 * 동기화: governor와 동일 — 등록은 main 이전, 검색은 scheduling reactor 단일. */
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
 * @name:   매칭할 스케줄러 이름. NULL이면 스케줄러 비활성(thread 자동 이동 없음
 *          — 사실상 "static"과 유사하지만 통계 수집도 멈춤).
 * @return: 0=성공, 비-0=이름 미매칭 또는 새 스케줄러의 init() 실패.
 *
 * 동작 단계:
 *   1) 현재 활성 스케줄러가 있으면 deinit() 호출 → 자원 해제.
 *   2) name == NULL이면 활성 포인터를 NULL로 두고 0 반환.
 *   3) 등록 TAILQ에서 strcmp 매칭.
 *   4) 매칭된 스케줄러의 init() 호출. 실패 시 활성 포인터 NULL로 둠.
 *
 * 호출 컨텍스트: scheduling reactor (RPC 핸들러는 send_msg로 안전 위임).
 * 호출 체인: rpc_framework_set_scheduler -> spdk_thread_send_msg(sched_thread)
 *           -> spdk_scheduler_set.
 */
int spdk_scheduler_set(const char *name);

/**
 * Get currently set scheduler.
 *
 * \return a pointer to spdk scheduler or NULL if none is set.
 */
/*
 * [한국어]
 * spdk_scheduler_get - 현재 활성 스케줄러 포인터 반환.
 *
 * @return: 활성 spdk_scheduler 포인터 또는 NULL(=비활성).
 *          라이브러리 소유 — 수정/free 금지. 다음 spdk_scheduler_set까지 유효.
 *
 * 호출 컨텍스트: scheduling reactor 또는 RPC 핸들러.
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
 * @period: 0=스케줄링 완전 비활성(thread는 현재 lcore에 영구 고정 + 통계 수집 정지),
 *          >0=주어진 주기마다 balance() 자동 호출.
 *
 * 트레이드오프:
 *   - 너무 짧으면(예: 1ms): 통계 노이즈에 반응해 thread가 끊임없이 이동하며 캐시
 *     친화도가 깨지고, balance 자체의 CPU 비용이 누적됨.
 *   - 너무 길면(예: 10s): 부하 변화에 둔감해져 균형 잡힐 때까지 thread가 포화 코어에
 *     쌓임 → 일시적 latency 스파이크.
 *   - 권장: 100ms ~ 1s. dynamic 스케줄러의 빌트인 기본은 약 1초.
 *
 * 부작용: 변경 즉시 반영. 진행 중인 balance 사이클은 영향받지 않음 — 다음 balance가
 *        새 주기로 스케줄.
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
 * @return: 0=비활성, >0=현재 설정된 주기.
 *
 * 호출 컨텍스트: 어떤 스레드에서도 호출 가능 (단순 정수 읽기 — 원자적이라고 가정).
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
 * spdk_scheduler_register - 스케줄러를 등록 TAILQ에 추가.
 *                           직접 호출하지 말고 SPDK_SCHEDULER_REGISTER 매크로 권장.
 *
 * @scheduler: 정적 lifetime의 spdk_scheduler 인스턴스 포인터.
 *             라이브러리는 포인터만 보관 — free 금지.
 *
 * 호출 시점: C 생성자(SPDK_SCHEDULER_REGISTER 안의 __attribute__((constructor)))에서
 *           main() 진입 전에 자동 실행. 등록만 하고 활성화는 별도(spdk_scheduler_set).
 *
 * 주의: 같은 이름의 스케줄러가 이미 등록되어 있으면 동작은 구현체에 따라 상이하므로
 *      모듈 작성자는 고유 이름을 사용해야 한다.
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
 * @return: 활성 reactor 마스크 안의 lcore ID. 기본값은 main_core 또는 첫 활성 lcore.
 *
 * 의미: 모든 scheduler/governor 콜백은 이 lcore의 reactor에서 호출된다.
 *       외부 스레드가 spdk_scheduler_set 등을 호출하려면 이 lcore로 send_msg를 보내야
 *       한다 — 그 송신 대상 식별에 본 함수가 사용된다.
 *
 * 호출 컨텍스트: 어떤 thread에서도 안전 (단순 atomic read).
 *
 * 사용 예:
 *   uint32_t sched_lcore = spdk_scheduler_get_scheduling_lcore();
 *   struct spdk_thread *sched_thread = ... lookup by lcore ...;
 *   spdk_thread_send_msg(sched_thread, my_callback, my_ctx);
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
 * 사용 예:
 *   - 원래 main_core가 latency-critical 작업을 맡게 되면 scheduling 부담을 다른
 *     코어로 옮겨 jitter를 줄임.
 *   - 특정 NUMA 노드에 scheduling reactor를 배치해 통계 수집 시 cross-NUMA 메모리
 *     접근을 줄임.
 *
 * 동작: 내부적으로 모든 scheduler/governor 콜백을 새 lcore에서 실행하도록 라우팅.
 *      현재 진행 중인 balance가 있으면 완료 후 다음 사이클부터 새 lcore에서 시작.
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
/* [한국어] 스케줄러 자동 등록 매크로 — 거버너 매크로(SPDK_GOVERNOR_REGISTER)와
 * 동일한 패턴. 모듈 .c 파일 최하단에 한 번 사용한다.
 *
 * 사용 예: 스케줄러 .c 파일 끝에 `SPDK_SCHEDULER_REGISTER(my_sched_struct);`.
 *
 * 동작 원리:
 *   1) `__attribute__((constructor))`로 표시된 정적 함수를 정의 → main() 전에 자동 실행.
 *   2) 함수 본체에서 spdk_scheduler_register(&scheduler) 호출 → 등록 TAILQ에 삽입.
 *
 * 토큰 결합 트릭(##):
 *   `_spdk_scheduler_register_ ## scheduler`로 스케줄러 인자별 고유 함수명을 생성.
 *   다중 스케줄러 모듈을 동시에 링크할 때 함수명 충돌 방지.
 *
 * 결과: 사용자 코드가 부팅 시 별도로 register를 호출하지 않아도 모듈 링크만으로
 *      자동 등록. SPDK 전반의 자동 등록 관용구(SPDK_BDEV_MODULE_REGISTER 등)와 동일.
 *
 * 제약: 같은 컴파일 단위에서 같은 인자로 두 번 사용하면 함수 재정의 에러. */

#ifdef __cplusplus
/* [한국어] C++ extern "C" 블록 종결 — C++ 인클루드 시 위에서 시작한 C linkage 영역을 닫음. */
}
#endif

#endif /* SPDK_SCHEDULER_H */
/* [한국어] include guard 종결 — 파일 최상단의 #ifndef SPDK_SCHEDULER_H에 짝이 됨. */
