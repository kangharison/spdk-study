/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Encapsulated DPDK specific dependencies
 */

/*
 * [한국어 설명] DPDK 직접 초기화 경로용 SPDK env API (env_dpdk.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK가 **이미 초기화된 DPDK EAL 위에 가입(bolt-on)**할 수 있게
 * 해주는 좁고 특수한 API 묶음을 정의한다. 일반 SPDK 앱은 `spdk_env_init()`
 * (include/spdk/env.h)을 호출해 SPDK가 내부에서 DPDK의 `rte_eal_init()`을
 * 부르도록 맡기지만, 이 헤더의 진입점들(`spdk_env_dpdk_post_init()` /
 * `spdk_env_dpdk_post_fini()`)은 그 책임이 외부 앱에 있을 때 사용된다.
 *
 * 즉 env.h가 "DPDK를 감싸 보이지 않게 하는 추상화 계층"이라면, env_dpdk.h는
 * 그 추상화 너머의 DPDK 실체를 **명시적으로 인정**하고 그것과의 lifecycle
 * 분리를 다루는 "DPDK 직결 진입점"이다. SPDK가 단독 프로세스가 아니라 OVS-DPDK,
 * VPP, 사용자 자체 DPDK 앱과 같은 호스트 프로세스에 라이브러리로 결합될 때
 * 반드시 거치는 경로다. 더불어 DPDK rte_malloc heap 사용량을 SPDK 자체
 * 자료형(`struct spdk_env_dpdk_mem_stats`)으로 노출해 모니터링/RPC가
 * DPDK 내부 타입에 직접 접근하지 않고도 통계를 조회할 수 있도록 한다.
 *
 * 핵심 디자인 포인트:
 *   - **DPDK lifecycle = 외부 앱 소유**: post_fini는 EAL을 종료하지 않는다.
 *     rte_eal_cleanup() 호출 권한은 외부 앱이 갖는다. SPDK는 자기가 잡은
 *     캐시/콜백/PCI probe만 푼다.
 *   - **rte_eal_init이 두 번 호출되지 않도록 보호**: post_init 사용자는
 *     반드시 spdk_env_init을 우회해야 하며, 두 함수는 상호 배타적 진입점이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK env 계층은 두 가지 기동 모드가 있다.
 *
 *  (A) SPDK 단독(default) 모드:
 *      외부 앱 main
 *        → spdk_app_start
 *           → spdk_env_init  (include/spdk/env.h)
 *              → 내부적으로 rte_eal_init() 호출 (DPDK가 SPDK 소유)
 *              → DPDK 위에 SPDK env 자료구조 셋업
 *           → reactor/poller 시작
 *        → ...
 *        → spdk_app_stop → spdk_env_fini → rte_eal_cleanup()
 *
 *  (B) 외부 DPDK 앱 임베드(post-init) 모드  ← 이 헤더가 다루는 경로:
 *      외부 앱 main
 *        → rte_eal_init(--proc-type=primary, --huge-dir=..., -l 0-3, ...)
 *           (외부 앱이 EAL hugepage·lcore·PCI 등을 직접 셋업)
 *        → **spdk_env_dpdk_post_init(legacy_mem)**   ← 본 헤더 진입점
 *           (rte_eal_init 재호출 없이 SPDK env 측 자료구조만 부착)
 *        → reactor/poller 시작
 *        → ...
 *        → spdk_env_dpdk_post_fini()  (SPDK 측 자원만 해제)
 *        → rte_eal_cleanup() (외부 앱 책임)
 *
 * 호출 컨텍스트: 두 함수 모두 **호스트 유저스페이스의 init/fini 단계에서 단일
 * 스레드로** 호출되며, 어떤 reactor/poller가 시작되기 전(또는 모두 종료된 후)에만
 * 사용해야 한다. I/O 핫패스에서는 절대 호출되지 않는다 — DPDK 자원은 hugepage
 * 위에 lockless ring/mempool 형태로 존재하므로, 일단 부착이 끝나면 SPDK는 일반
 * env API(include/spdk/env.h)를 통해 평소처럼 동작한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(이 헤더가 쓰는 것):
 *   - spdk/stdinc.h: bool, uint64_t/uint32_t, FILE* 등 표준 타입을 일원화 제공
 *   - 구현부(lib/env_dpdk/init.c, memory.c)는 DPDK의 rte_eal_*, rte_malloc_*,
 *     rte_malloc_get_socket_stats() 등에 직결 — 헤더 자체는 DPDK 타입을 노출하지 않음
 *
 * 의존하는 모듈(이 헤더를 쓰는 것):
 *   - 외부 DPDK 앱(SPDK를 sublibrary로 포함하는 코드) — post_init/post_fini의 1차 사용자
 *   - SPDK 내부의 EAL 종료 권한 판별 로직 — spdk_env_dpdk_external_init() 결과로
 *     spdk_env_fini가 rte_eal_cleanup을 호출할지 결정
 *   - module/event/, app/spdk_tgt/ 등의 init/fini 흐름
 *   - JSON-RPC 핸들러 `env_dpdk_get_mem_stats` (lib/env_dpdk/env.c)에서
 *     spdk_env_dpdk_get_mem_stats / dump_mem_stats 호출
 *
 * 데이터 흐름:
 *   외부 앱이 만든 EAL 상태(hugepage segment, lcore mask, pci scan 결과)
 *     → spdk_env_dpdk_post_init이 그 상태를 읽어 SPDK 측 mempool 캐시,
 *       vfio DMA map 콜백, PCI hotplug hook을 부착
 *     → 이후 I/O 동안에는 호출 없음 (rte_malloc 통계 조회만 비주기적으로 발생)
 *     → 종료 시 spdk_env_dpdk_post_fini가 SPDK가 추가한 콜백·캐시만 정리
 *
 * 공유하는 핵심 자료구조:
 *   - 내부 전역 플래그 `g_external_init` (lib/env_dpdk/init.c) — external_init() 응답값
 *   - DPDK rte_malloc heap 통계는 NUMA 노드 단위로 spdk_env_dpdk_mem_stats에 사상
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_env_dpdk_mem_stats
 *       DPDK rte_malloc heap의 NUMA 노드별 사용량 스냅샷 (총량/free/최대 free 블록/할당량/카운트).
 *   - int  spdk_env_dpdk_post_init(bool legacy_mem)
 *       외부에서 호출된 rte_eal_init 위에 SPDK env를 부착. legacy_mem 인자로 EAL
 *       메모리 모드(--legacy-mem on/off)를 전달받아 vfio·iova 매핑 동작을 조정.
 *   - void spdk_env_dpdk_post_fini(void)
 *       post_init이 잡은 SPDK env 자원만 해제. EAL 자체는 종료하지 않음.
 *   - bool spdk_env_dpdk_external_init(void)
 *       현재 SPDK가 외부 EAL 위에서 동작 중인지 질의하는 플래그 조회 함수.
 *   - void spdk_env_dpdk_dump_mem_stats(FILE *file)
 *       모든 NUMA 노드의 DPDK heap 사용 현황을 사람이 읽기 쉬운 텍스트로 덤프.
 *   - int  spdk_env_dpdk_get_mem_stats(struct spdk_env_dpdk_mem_stats *stats, uint32_t numa_id)
 *       특정 NUMA 노드의 통계를 호출자 버퍼에 채워 반환 (모니터링 RPC용).
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 C 타입을 일원화 제공하는 SPDK 표준 헤더 묶음 (stdinc.h).
 * - 가져오는 것: stdint.h(uint64_t/uint32_t), stdbool.h(bool), stdio.h(FILE*) 등
 * - 왜 stdinc.h: SPDK는 OS/컴파일러 차이를 stdinc 한 곳에서 흡수하도록 정책화
 *   되어 있어, 개별 헤더가 stdint.h/stdbool.h를 직접 include하지 않는다.
 * - 위치 이유: include 가드 바깥(파일 최상단)에 두면 가드된 본문이 어떤 타입을
 *   참조하든 미리 사용 가능하다. */

#ifndef SPDK_ENV_DPDK_H
#define SPDK_ENV_DPDK_H
/* [한국어] include 가드 — 동일 헤더가 여러 .c에서 중복 include 되어도 한 번만
 * 펼쳐지도록 보호한다. 이름 SPDK_ENV_DPDK_H는 SPDK 헤더 컨벤션(파일명 대문자화).
 * stdinc.h를 가드 밖에 둔 이유는 위의 인라인 주석 참조. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 빌드에서 본 헤더의 함수 선언이 C linkage(name mangling 없음)를
 * 갖도록 강제한다. SPDK를 임베드하는 외부 DPDK 앱이 C++일 수 있고, 그쪽에서
 * 본 헤더를 include 했을 때 링커가 C 심볼 이름(spdk_env_dpdk_post_init 등)을
 * 그대로 찾을 수 있어야 하기 때문. */
#endif

/**
 * Memory allocation statistics.
 */
/*
 * [한국어] struct spdk_env_dpdk_mem_stats — DPDK rte_malloc heap 통계의 SPDK 표현
 *
 * 배경: DPDK는 hugepage 위에 자체 힙 할당기 rte_malloc()/rte_zmalloc()을 운용한다.
 * SPDK는 NVMe DMA 안전 메모리, 각종 mempool 백킹 메모리 등을 모두 이 힙에서
 * 가져오므로, 운영자/모니터링 도구가 사용량과 단편화 상태를 알 수 있어야 한다.
 * 이 구조체는 그 통계를 SPDK 외부 헤더 형태로 캡슐화해 노출한다 — 호출자는
 * DPDK 내부 타입(struct rte_malloc_socket_stats)을 직접 보지 않는다.
 *
 * 사용 패턴: spdk_env_dpdk_get_mem_stats(&stats, numa_id) 호출로 한 NUMA
 * 노드씩 채워온 뒤 RPC 응답 또는 dump 출력에 사용.
 *
 * 동기화: 통계는 호출 시점의 **스냅샷**이며, 직후 다른 코어가 할당/해제하면
 * 값이 변할 수 있다. 정합성이 중요한 용도라면 호출자가 외부 락 또는 정지
 * 시점에서 수집해야 한다.
 *
 * 필드 단위로 자세한 의미는 각 필드 옆 멀티라인 주석에 기술.
 */
struct spdk_env_dpdk_mem_stats {
	/**
	 * Total bytes on heap
	 */
	uint64_t heap_totalsz_bytes;
	/* [한국어] 해당 NUMA 노드의 DPDK 힙 총 크기 (바이트 단위).
	 * - 의미: rte_eal_init 시 마운트된 hugepage 영역 중 이 NUMA 노드에
	 *   배정된 크기의 합. 예: 1GB hugepage * 4장 → 약 4GB.
	 * - 설정자: spdk_env_dpdk_get_mem_stats가 rte_malloc_get_socket_stats
	 *   호출 결과(rte_stats.heap_totalsz_bytes)를 그대로 복사.
	 * - 읽는 자: 모니터링 RPC 응답, 운영자 로그, dump_mem_stats 출력.
	 * - 값 범위: 0(해당 NUMA에 hugepage 없음) 또는 hugepage 크기의 배수.
	 * - 동기화: 일단 EAL이 부팅된 후에는 거의 변하지 않음(동적 hugepage
	 *   확장이 아니라면 init 시점 값으로 고정). */

	/**
	 * Total free bytes on heap
	 */
	uint64_t heap_freesz_bytes;
	/* [한국어] 현재 비어 있어 할당 가능한 힙 바이트 수의 총합.
	 * - 관계식: heap_totalsz_bytes ≈ heap_freesz_bytes + heap_allocsz_bytes
	 *   (rte_malloc 메타데이터 오버헤드는 별도라 정확히 같지 않을 수 있음).
	 * - 읽는 자: OOM 감시 도구가 임계치 이하면 경고를 발화. 부족 시 새 NVMe
	 *   qpair 생성, 큰 DMA 버퍼 할당 등이 실패할 수 있다.
	 * - 동기화: 스냅샷 — 동시 다른 코어 할당으로 즉시 변할 수 있음. */

	/**
	 * Size in bytes of largest free block
	 */
	uint64_t greatest_free_size;
	/* [한국어] 힙 내 free 블록들 가운데 단일 최대 블록 크기 (바이트).
	 * - 왜 중요한가: SPDK는 종종 수 MB ~ 수십 MB의 연속 DMA 버퍼를 한 번에
	 *   할당해야 한다(예: NVMe PRP 리스트, 큰 mempool element). heap_freesz_bytes가
	 *   충분해 보여도 작은 free 블록만 흩어져 있으면(외부 단편화) 그 큰 한 덩어리
	 *   할당이 실패할 수 있다. 이 값이 그 가능성을 판단하는 1차 지표.
	 * - 임계 판단: greatest_free_size < 요구 버퍼 크기 → 단편화 경보.
	 * - 동기화: 스냅샷이며 정확도는 위 두 필드와 동일. */

	/**
	 * Total allocated bytes on heap
	 */
	uint64_t heap_allocsz_bytes;
	/* [한국어] 현재 사용(할당) 중인 힙 바이트 수의 총합.
	 * - 의미: SPDK 컴포넌트들이 rte_malloc/rte_zmalloc/rte_memzone_reserve
	 *   경로로 잡아간 모든 메모리의 합 (mempool 백킹, DMA 버퍼, ring 등).
	 * - 읽는 자: 누수 감시 — 시간에 따라 단조 증가하면 leak 의심. */

	/**
	 * Number of free elements on heap
	 */
	uint32_t free_count;
	/* [한국어] 힙의 free 리스트에 있는 빈 블록(엘리먼트) 개수.
	 * - 단편화 지표: 값이 크면(수백~수천) 작은 빈 블록이 다수 — 큰 연속 할당
	 *   실패 가능성↑. 값이 1~2면 거의 단일한 큰 free 영역이 남아있음.
	 * - 비교: greatest_free_size와 함께 보면 단편화 정도를 정성적으로 파악. */

	/**
	 * Number of allocated elements on heap
	 */
	uint32_t alloc_count;
	/* [한국어] 현재 할당 상태인 블록 개수.
	 * - SPDK 측에서 rte_malloc 계열로 잡아둔 객체 수의 합 — mempool 슬롯 하나하나
	 *   가 별도 카운트되지는 않으며, mempool 자체 백킹 메모리가 1개로 잡힘.
	 * - 누수 감시: alloc_count가 시간에 따라 단조 증가하고 free_count는 안 줄면
	 *   객체별 free 누락(leak) 의심. */
};

/**
 * Initialize the environment library after DPDK env is already initialized.
 * If DPDK's rte_eal_init is already called, this function must be called
 * instead of spdk_env_init, prior to using any other functions in SPDK
 * env library.
 *
 * \param legacy_mem Indicates whether DPDK was initialized with --legacy-mem
 *                   eal parameter.
 * \return 0 on success, or negative errno on failure.
 */
/*
 * [한국어]
 * spdk_env_dpdk_post_init - 외부에서 이미 만든 DPDK EAL 위에 SPDK env를 부착
 *
 * @legacy_mem: 외부 앱이 rte_eal_init 시 `--legacy-mem` EAL 파라미터를 줬는지 여부.
 *              - true:  DPDK가 시작 시점에 hugepage 영역을 한꺼번에 매핑하는 구식 모드
 *                       (모든 hugepage가 init에 고정 매핑되어 있고 dynamic add/remove 불가).
 *                       SPDK는 이 경우 vfio DMA 매핑을 init 시점에 일괄 등록한다.
 *              - false: dynamic memory 모드 — DPDK가 필요할 때마다 hugepage를 추가/제거.
 *                       SPDK는 mem_event 콜백을 등록해 페이지 변동에 따라 vfio DMA 매핑을
 *                       증감시킨다.
 *              잘못된 값을 전달하면 vfio 매핑이 실제 hugepage 레이아웃과 어긋나 NVMe DMA
 *              실패가 발생할 수 있으므로 외부 앱이 rte_eal_init에 준 옵션과 정확히 일치해야 함.
 * @return: 0 성공, 실패 시 음수 errno (-ENOMEM: 내부 자료구조 메모리 부족,
 *          -EEXIST: 이미 init된 상태에서 중복 호출 등). 실패 시 SPDK env API 사용 금지.
 *
 * 동기/배경:
 *   외부 DPDK 앱(OVS-DPDK, VPP, 사용자 자체 DPDK 앱 등)에 SPDK를 라이브러리로
 *   결합할 때, rte_eal_init은 프로세스당 한 번만 호출 가능하므로 spdk_env_init이
 *   내부에서 또 호출하면 충돌·중복 매핑이 발생한다. 이 함수는 EAL 호출을 건너뛰고
 *   "그 위에 부착"만 한다.
 *
 * 동작 단계:
 *   1) 이미 rte_eal_init이 외부에서 성공했다고 신뢰 (검증은 일부 sanity check만).
 *   2) SPDK 내부 mempool 캐시·핫플러그 콜백·DMA 등록 콜백을 초기화.
 *   3) legacy_mem 값에 따라 vfio_iommu DMA map 전략 선택
 *      - legacy=true → init 시 일괄 dma_map_all
 *      - legacy=false → rte_mem_event_callback_register로 동적 매핑.
 *   4) 내부 전역 플래그 g_external_init=true 세팅 → external_init() 응답을 true로 만들어
 *      이후 SPDK fini가 rte_eal_cleanup을 호출하지 않도록 통제.
 *
 * 실행 컨텍스트:
 *   - 외부 앱의 init 단계에서 단일 스레드로만 호출.
 *   - SPDK reactor/thread가 한 개라도 시작되기 **전**에 호출해야 함.
 *   - 재진입 불가 — 한 프로세스에서 1회.
 *
 * 호출 체인:
 *   외부 앱 main
 *     → rte_eal_init(--proc-type=primary, --huge-dir=..., --file-prefix=..., -l 0-3, ...)
 *     → spdk_env_dpdk_post_init(legacy_mem)        ← here
 *     → spdk_app_start (또는 reactor 직접 기동)
 *
 * 에러 경로:
 *   실패 시 호출자는 spdk_env_dpdk_post_fini를 호출하지 말고 즉시 종료해야 안전
 *   (반쯤 부착된 상태가 남지 않도록 함수 내부에서 자기 정리).
 */
int spdk_env_dpdk_post_init(bool legacy_mem);

/**
 * Release any resources of the environment library that were allocated with
 * spdk_env_dpdk_post_init(). After this call, no DPDK function calls may
 * be made. It is expected that common usage of this function is to call it
 * just before terminating the process.
 */
/*
 * [한국어]
 * spdk_env_dpdk_post_fini - post_init이 잡아둔 SPDK env 측 자원만 해제
 *
 * 입력/출력 없음 (void → void).
 *
 * 핵심 의미 (DPDK lifecycle 분리):
 *   이 함수는 EAL을 종료하지 **않는다**. 즉 rte_eal_cleanup()을 부르지 않는다.
 *   외부 DPDK 앱이 EAL의 소유자이므로 그 청소 권한도 외부 앱에 있다. 두 측이
 *   각자의 자원만 정리해야 hugepage·lcore의 이중 종료 / dangling 매핑이 없다.
 *
 * 해제 대상 (SPDK 측만):
 *   1) post_init에서 등록한 mem_event_callback / mempool 캐시 / DMA 매핑 콜백.
 *   2) SPDK가 후킹한 PCI hot-plug 핸들러.
 *   3) 내부 전역 플래그 g_external_init 등 init 흐름에서 잡힌 상태.
 *
 * 호출 후 주의 (헤더 영문 주석에 명시):
 *   "이 호출 이후로는 SPDK든 DPDK든 어떤 함수도 호출되어서는 안 된다."
 *   보통 프로세스 종료 직전(외부 앱의 main return 직전)에 호출한다.
 *
 * 실행 컨텍스트: 모든 SPDK reactor가 정상 종료된 뒤, 단일 스레드.
 *
 * 호출 체인:
 *   외부 앱 종료 단계
 *     → spdk_app_fini (또는 reactor 정지)
 *     → spdk_env_dpdk_post_fini()                  ← here
 *     → 외부 앱 rte_eal_cleanup() (외부 책임)
 *     → return from main
 */
void spdk_env_dpdk_post_fini(void);

/**
 * Check if DPDK was initialized external to the SPDK env_dpdk library.
 *
 * \return true if DPDK was initialized external to the SPDK env_dpdk library.
 * \return false otherwise
 */
/*
 * [한국어]
 * spdk_env_dpdk_external_init - 현재 SPDK가 외부 EAL 위에서 동작 중인지 질의
 *
 * @return: true  → spdk_env_dpdk_post_init 경로(외부 앱이 rte_eal_init을 했음)
 *          false → spdk_env_init 경로(SPDK가 내부에서 rte_eal_init을 했음)
 *
 * 왜 필요한가:
 *   SPDK 종료(spdk_env_fini) 시 rte_eal_cleanup() 호출 권한을 결정하기 위함.
 *   - false면 SPDK가 EAL의 소유자이므로 rte_eal_cleanup을 호출해 마무리한다.
 *   - true면  EAL은 외부 앱 소유 — SPDK는 절대 cleanup을 호출하지 않는다(이중 종료
 *     방지). 호출 시 hugepage 매핑이 무효화되어 외부 앱이 segfault 한다.
 *   또한 진단/로그에서 어떤 모드로 떠 있는지 표시할 때도 사용.
 *
 * 동시성: 내부 전역 bool 플래그 1바이트 read — 락 불필요. post_init 이후로는
 * 값이 변하지 않으므로 멀티 reactor에서 동시 호출되어도 안전.
 *
 * 호출 체인 (대표):
 *   spdk_env_fini → if (!spdk_env_dpdk_external_init()) rte_eal_cleanup();
 */
bool spdk_env_dpdk_external_init(void);

/**
 * Dump the env allocated memory to the given file.
 *
 * \param file The file object to write to.
 */
/*
 * [한국어]
 * spdk_env_dpdk_dump_mem_stats - DPDK heap 사용 현황을 사람이 읽는 형태로 file에 덤프
 *
 * @file: 덤프 출력 대상 FILE 스트림.
 *        - stdout/stderr: 콘솔 즉석 덤프
 *        - fopen("/tmp/...", "w"): 파일 저장
 *        - fmemopen 또는 RPC 응답 빌더의 임시 버퍼도 가능
 *
 * 동작:
 *   1) DPDK가 인지하는 모든 NUMA 노드를 순회.
 *   2) 각 노드에 대해 spdk_env_dpdk_get_mem_stats를 호출해 통계를 얻음.
 *   3) 노드별로 heap_totalsz_bytes, heap_freesz_bytes, greatest_free_size,
 *      heap_allocsz_bytes, free_count, alloc_count를 fprintf로 사람이 읽기
 *      좋은 라벨과 함께 출력.
 *
 * 사용처:
 *   - 디버그: SIGUSR1 핸들러나 RPC 핸들러에서 호출해 메모리 누수/단편화 즉시 확인.
 *   - 운영: 정기적 헬스 로그.
 *   - 충돌 분석: 패닉 직전 상태 스냅샷을 stderr에 떨궈 코어 분석 보조.
 *
 * 실행 컨텍스트: 어떤 SPDK 스레드/외부 컨텍스트에서도 호출 가능 — 다만 fprintf가
 * 블로킹이므로 핫패스(reactor poll loop)에서는 피한다.
 *
 * 호출 체인 (대표):
 *   RPC handler (env_dpdk_get_mem_stats) → spdk_env_dpdk_dump_mem_stats(rpc_response_file)
 *   signal handler (SIGUSR1) → spdk_env_dpdk_dump_mem_stats(stderr)
 */
void spdk_env_dpdk_dump_mem_stats(FILE *file);

/**
 * Retrieve memory allocation statistics.
 *
 * \param stats Pointer to structure to fill with statistics.
 * \param numa_id NUMA node ID for which statistics are retrieved.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_env_dpdk_get_mem_stats - 특정 NUMA 노드의 DPDK heap 통계를 구조체로 반환
 *
 * @stats:   호출자가 미리 준비한 spdk_env_dpdk_mem_stats 출력 버퍼.
 *           NULL이면 -EINVAL. 함수가 6개 필드를 모두 채운다.
 * @numa_id: 통계를 얻을 NUMA 노드 ID (0, 1, 2, ...). DPDK의 socket_id에 1:1 대응.
 *           존재하지 않는 노드 ID이면 -EINVAL 또는 0으로 채워질 수 있음(구현 의존).
 * @return:  0 성공, 음수 errno (-EINVAL: 인자 오류, 그 외: DPDK 내부 실패).
 *           실패 시 stats 내용은 unspecified — 호출자는 의존하지 말 것.
 *
 * 내부 동작:
 *   1) DPDK API rte_malloc_get_socket_stats(numa_id, &rte_stats) 호출.
 *   2) DPDK 내부 타입 struct rte_malloc_socket_stats의 필드를 SPDK 외부 구조체
 *      spdk_env_dpdk_mem_stats로 1:1 복사·정규화.
 *   3) 이 캡슐화 덕분에 SPDK 사용자는 DPDK 헤더를 include 할 필요가 없음.
 *
 * 사용처:
 *   - JSON-RPC `env_dpdk_get_mem_stats` 응답 — 모니터링 도구가 폴링.
 *   - 운영 도구의 헬스체크 — 임계치 비교 후 알람.
 *   - dump_mem_stats가 사람용이라면, get_mem_stats는 프로그램용.
 *
 * 호출 체인:
 *   RPC server → spdk_env_dpdk_get_mem_stats(&stats, numa_id)
 *              → DPDK rte_malloc_get_socket_stats
 */
int spdk_env_dpdk_get_mem_stats(struct spdk_env_dpdk_mem_stats *stats, uint32_t numa_id);

#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 닫기 — 위 ifdef __cplusplus와 짝. C 빌드에서는 이 두 라인
 * 모두 전처리에서 제거되어 영향을 주지 않는다. */
#endif

#endif
/* [한국어] include 가드 종료 (#ifndef SPDK_ENV_DPDK_H의 짝). 헤더 본문이 한 컴파일
 * 단위에서 두 번 펼쳐지는 것을 막는다. */
