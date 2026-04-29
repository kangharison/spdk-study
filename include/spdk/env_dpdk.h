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
 * 일반적으로 SPDK 앱은 `spdk_env_init()`(include/spdk/env.h)으로 환경을
 * 초기화한다. 그러면 SPDK가 내부적으로 DPDK의 `rte_eal_init()`을 호출해
 * EAL(Environment Abstraction Layer) — hugepage 매핑, lcore 스케줄링,
 * PCI 드라이버 바인딩, mempool/ring 인프라 등 — 을 세팅한다.
 *
 * 그러나 SPDK를 **DPDK가 이미 초기화된 다른 애플리케이션**(예: OVS-DPDK,
 * 사용자 자체 DPDK 앱)에 라이브러리 형태로 결합할 때는 `rte_eal_init()`을
 * 두 번 호출할 수 없다. 이 헤더는 그런 시나리오에서 사용하는 진입점,
 * `spdk_env_dpdk_post_init()`/`_post_fini()`, 그리고 외부 초기화 여부를
 * 판별하고 메모리 통계를 수집하는 보조 API를 제공한다.
 *
 * 즉, 이 파일은 **"DPDK 초기화의 소유권이 SPDK가 아닌 외부 앱에 있을 때를
 * 위한 우회 진입점"**이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 두 가지 사용 모드:
 *
 *  (A) SPDK 단독 모드 (기본):
 *      spdk_app_start → spdk_env_init → (내부) rte_eal_init → spdk_env_dpdk_post_init
 *
 *  (B) 외부 DPDK 앱 임베드 모드:
 *      External app:
 *         rte_eal_init(--proc-type=primary, --huge-dir=..., --file-prefix=..., -l 0-3 ...)
 *      → SPDK: **spdk_env_dpdk_post_init(legacy_mem)**     ← 이 헤더 진입점
 *      → SPDK는 EAL을 다시 만들지 않고, 그 위에 mempool/ring/PCI 핸들만 부착
 *      → 종료 시: spdk_env_dpdk_post_fini() → external app: rte_eal_cleanup()
 *
 * 실행 컨텍스트: 호스트 유저스페이스, 앱 init/fini 단계 (단일 스레드).
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/stdinc.h: bool, uint64_t/uint32_t, FILE* 등 표준 타입
 *   - 구현부(lib/env_dpdk/init.c)는 DPDK의 rte_eal_*, rte_malloc_get_socket_stats 등에 직결
 * 의존하는 모듈:
 *   - 외부 DPDK 앱(SPDK를 sublibrary로 사용하는 측)
 *   - SPDK 내부 env_dpdk_external_init 체크가 필요한 모듈들
 *   - RPC 핸들러 rpc_env_dpdk_get_mem_stats (메모리 통계 노출)
 * 데이터 흐름:
 *   외부 앱이 만든 EAL 상태 → spdk_env_dpdk_post_init이 SPDK 측 자료구조 초기화
 *   → I/O 핫패스에서는 이 API 미호출, 단지 init/fini 시에만 사용.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_env_dpdk_mem_stats: DPDK heap (rte_malloc) 사용량 통계
 *   - spdk_env_dpdk_post_init: 외부 EAL 위에 SPDK env 부착
 *   - spdk_env_dpdk_post_fini: 부착된 SPDK env 자원만 해제 (EAL은 외부 소유)
 *   - spdk_env_dpdk_external_init: 현재 환경이 외부 init 모드인지 판별
 *   - spdk_env_dpdk_dump_mem_stats: 메모리 사용 현황을 FILE*에 덤프
 *   - spdk_env_dpdk_get_mem_stats: 구조체로 메모리 통계 채우기
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 C 헤더 묶음 — bool, uint64_t/uint32_t, FILE* 타입 정의를 가져온다.
 * 이 헤더는 stdint.h, stdbool.h, stdio.h 등을 직접 포함하지 않고 stdinc.h로 일원화. */

#ifndef SPDK_ENV_DPDK_H
#define SPDK_ENV_DPDK_H
/* [한국어] include 가드 — stdinc 다음에 두는 이유는 일부 호환 매크로가 stdinc 측에 의존하기 때문 */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 호출자(외부 DPDK 앱이 C++일 수 있음) name mangling 차단 */
#endif

/**
 * Memory allocation statistics.
 */
/*
 * [한국어] struct spdk_env_dpdk_mem_stats — DPDK rte_malloc heap 통계 스냅샷
 *
 * 배경: DPDK는 hugepage 위에 rte_malloc()이라는 자체 힙 할당기를 운용한다.
 * SPDK는 NVMe DMA 가능 메모리, mempool 백킹 등을 모두 이 힙에서 가져오므로,
 * 운영자/모니터링 도구가 사용량을 알 수 있도록 이 구조체를 통해 노출한다.
 *
 * 필드는 모두 NUMA 노드 단위(spdk_env_dpdk_get_mem_stats의 numa_id)로 조회된다.
 */
struct spdk_env_dpdk_mem_stats {
	/**
	 * Total bytes on heap
	 */
	uint64_t heap_totalsz_bytes;
	/* [한국어] 해당 NUMA 노드의 DPDK 힙 총 크기 (바이트).
	 * - 설정자: spdk_env_dpdk_get_mem_stats가 rte_malloc_get_socket_stats 결과를 복사
	 * - 읽는 자: 모니터링 RPC, 사용자 애플리케이션
	 * - 값 범위: 0 또는 hugepage 크기의 배수 (보통 GB 단위)
	 * - 동기화: 스냅샷 호출 시점의 값 — 이후 다른 스레드 할당으로 변할 수 있음 */

	/**
	 * Total free bytes on heap
	 */
	uint64_t heap_freesz_bytes;
	/* [한국어] 현재 비어 있는(할당 가능한) 힙 바이트 수.
	 * - heap_totalsz_bytes - heap_allocsz_bytes ≈ heap_freesz_bytes (오버헤드 제외)
	 * - 읽는 자: OOM 감시 도구 — 임계치 이하면 경고 */

	/**
	 * Size in bytes of largest free block
	 */
	uint64_t greatest_free_size;
	/* [한국어] 힙 내 free 블록들 중 가장 큰 단일 블록 크기.
	 * - 큰 연속 hugepage 할당(수 MB DMA 버퍼 등)에 성공할지를 가늠하는 지표
	 * - heap_freesz_bytes가 충분해도 단일 큰 블록이 없으면 fragmentation 문제 */

	/**
	 * Total allocated bytes on heap
	 */
	uint64_t heap_allocsz_bytes;
	/* [한국어] 현재 사용 중인(할당된) 힙 바이트 수.
	 * - SPDK가 rte_malloc/rte_zmalloc/rte_memzone_reserve로 받아간 총량 */

	/**
	 * Number of free elements on heap
	 */
	uint32_t free_count;
	/* [한국어] free 리스트에 있는 블록(엘리먼트) 개수.
	 * - 이 값이 크면 fragmentation 발생 — 작은 빈 블록이 여럿 흩어져 있음 */

	/**
	 * Number of allocated elements on heap
	 */
	uint32_t alloc_count;
	/* [한국어] 현재 할당된 블록 개수.
	 * - SPDK 컴포넌트(mempool, dma 버퍼 등)가 잡아둔 객체 수의 합 */
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
 * @legacy_mem: 외부 앱이 rte_eal_init 시 `--legacy-mem`을 줬는지 여부.
 *              true이면 DPDK는 hugepage 영역을 시작 시점에 한꺼번에 매핑하는
 *              구식 모드(고정), false이면 동적 페이지 추가/제거 가능한 신식 모드.
 *              SPDK가 vfio 매핑을 다른 방식으로 다뤄야 하므로 이 플래그가 필요.
 * @return: 0 성공, 실패 시 음수 errno (-ENOMEM, -EEXIST 등).
 *
 * 동작:
 *   1) 이미 rte_eal_init이 외부에서 호출됐다고 가정
 *   2) SPDK 측 mempool/ring 캐시, vfio·iommu DMA 매핑 콜백, PCI 디바이스
 *      probe 인프라 등을 초기화
 *   3) external_init=true 플래그를 세움 → spdk_env_dpdk_external_init이 true 반환
 *
 * 호출 컨텍스트: 외부 앱의 init 단계, 단일 스레드. SPDK reactor 시작 전에만 호출.
 *
 * 호출 체인:
 *   외부 앱 main → rte_eal_init(...) → spdk_env_dpdk_post_init(legacy_mem)
 *                → spdk_app_start (또는 reactor 직접 기동)
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
 * 입출력 없음.
 *
 * 중요: 이 함수는 EAL을 종료하지 **않는다**. EAL의 소유권은 외부 앱에 있으므로
 * 외부 앱이 rte_eal_cleanup()을 직접 호출해야 한다. 두 번 종료를 방지하기 위함.
 *
 * 호출 후 주의: SPDK든 DPDK든 어떤 함수도 더는 호출해서는 안 된다(주석에 명시).
 * 보통 프로세스 종료 직전에 호출.
 *
 * 호출 체인:
 *   외부 앱 종료 → spdk_app_fini → spdk_env_dpdk_post_fini → 외부 앱 rte_eal_cleanup
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
 * @return: post_init 경로(외부 init)면 true, spdk_env_init 경로면 false.
 *
 * 사용처: SPDK 내부에서 EAL 종료 권한을 가질지 결정할 때.
 *   - false면: spdk_env_fini가 rte_eal_cleanup 호출 가능
 *   - true면:  rte_eal_cleanup 호출 금지 (외부 앱 소유)
 *
 * 동시성: 단일 비트 조회 — 락 불필요. init 이후 변하지 않으므로 안전.
 */
bool spdk_env_dpdk_external_init(void);

/**
 * Dump the env allocated memory to the given file.
 *
 * \param file The file object to write to.
 */
/*
 * [한국어]
 * spdk_env_dpdk_dump_mem_stats - DPDK 힙 사용 현황을 사람이 읽는 형태로 file에 덤프
 *
 * @file: stdout, stderr, 혹은 fopen으로 연 일반 FILE*.
 *
 * 동작: 모든 NUMA 노드를 순회하며 spdk_env_dpdk_mem_stats 필드들을 fprintf로 출력.
 *
 * 사용처: 디버그/장애 분석 — SIGUSR1 핸들러나 RPC 핸들러에서 호출해 메모리
 * 누수/단편화 상태를 즉석에서 확인.
 *
 * 호출 체인:
 *   RPC handler / signal handler → spdk_env_dpdk_dump_mem_stats(file)
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
 * spdk_env_dpdk_get_mem_stats - 특정 NUMA 노드의 힙 통계를 구조체로 반환
 *
 * @stats:   호출자가 미리 준비한 출력 버퍼. 함수가 필드를 채워준다.
 * @numa_id: 통계를 얻을 NUMA 노드 ID (0, 1, ...). DPDK의 socket_id에 대응.
 * @return:  0 성공, 음수 errno (-EINVAL 등) 실패. 실패 시 stats 내용은 unspecified.
 *
 * 내부적으로 DPDK의 rte_malloc_get_socket_stats(numa_id, &rte_stats)를 호출하고,
 * SPDK 외부 구조체 형태로 복사·정규화한다.
 *
 * 사용처: 모니터링 RPC, 운영 도구의 헬스체크. dump_mem_stats가 사람용이라면
 * 이 함수는 프로그램용.
 *
 * 호출 체인:
 *   RPC server (env_dpdk_get_mem_stats) → spdk_env_dpdk_get_mem_stats → DPDK
 */
int spdk_env_dpdk_get_mem_stats(struct spdk_env_dpdk_mem_stats *stats, uint32_t numa_id);

#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 닫기 */
#endif

#endif
/* [한국어] include 가드 종료 */
