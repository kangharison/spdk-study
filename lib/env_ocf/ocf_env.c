/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] OCF 환경(env) — SPDK 환경 어댑터 구현 (ocf_env.c)
 *
 * === 파일의 역할 ===
 * libocf(Open CAS Framework, Intel CAS의 캐시 알고리즘 라이브러리)는 호스트 환경(메모리/스레드/락/CRC)을
 * "env_*" 추상 API로 분리한다. 본 파일은 그 어댑터 중 SPDK용 구현으로, 다음 4가지 기능을 SPDK 자원에
 * 매핑한다:
 *   1) env_allocator(고정 크기 슬랩 풀): spdk_mempool 위에 얇은 wrapper로 구현.
 *   2) CRC32: SPDK의 CRC32-IEEE(spdk_crc32_ieee_update)에 매핑.
 *   3) 실행 컨텍스트(execution context): OCF가 "현재 코어에서 선점되지 않는다"는 가정을 사용하므로
 *      유저스페이스에서는 그 가정을 흉내 내기 위해 cpu별 pthread_mutex로 직렬화.
 *   4) 실행 컨텍스트 개수: nproc(=온라인 CPU 수)으로 보고.
 * 이 어댑터 위에 mpool.c가 다단계 풀을 더 얹는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   libocf 코어(ENV_ALLOCATOR_*, env_crc32, env_get/put_execution_context)
 *     → 본 파일의 SPDK 어댑터
 *       → spdk_mempool / spdk_crc32_ieee_update / pthread_mutex / sched_getcpu
 * 호출 단계: 부팅 시 init_execution_context(__attribute__((constructor)))가 자동 실행.
 * 종료 시 deinit_execution_context(__attribute__((destructor)))가 자동 실행.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: ocf/ocf_def.h(타입), ocf_env.h(env_allocator 등 타입), spdk/crc32(IEEE-CRC32),
 *   spdk/env(spdk_mempool), spdk/log(SPDK_ERRLOG), pthread, sched_getcpu(GNU 확장).
 * - 의존받음: libocf 코어 + module/bdev/ocf 등 OCF를 사용하는 SPDK 컴포넌트.
 * - 데이터 흐름: OCF가 환경 API 호출 → 본 파일 → SPDK/POSIX 자원.
 * - 공유 자료구조: 전역 g_env_allocator_index(원자적 카운터), exec_context_mutex 배열(cpu별 mutex).
 *
 * === 주요 함수/구조체 요약 ===
 * - env_allocator_new/del/create/create_extended/destroy : 고정 크기 풀 API.
 * - env_crc32                                            : OCF CRC32를 SPDK CRC32-IEEE로 매핑.
 * - init/deinit_execution_context                        : 부팅/종료 시 cpu별 mutex 배열 초기화/해제.
 * - env_get/put_execution_context                        : "현재 cpu에서 선점되지 않음"을 mutex로 시뮬.
 * - env_get_execution_context_count                      : sysconf(_SC_NPROCESSORS_ONLN).
 */

#include "ocf/ocf_def.h"
/* [한국어] OCF 핵심 타입/매크로 정의 — env_atomic, ENV_BUG_ON 등. */

#include "ocf_env.h"
/* [한국어] env_allocator 구조체와 OCF env API 시그니처. */

#include "spdk/crc32.h"
/* [한국어] SPDK CRC32-IEEE 구현(spdk_crc32_ieee_update). hw 가속(SSE4.2) 지원 시 자동 활용. */

#include "spdk/env.h"
/* [한국어] spdk_mempool_create/get/put/free, SPDK_ENV_NUMA_ID_ANY. DPDK 위에 구현됨. */

#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG 등 로그 매크로. */

/* Number of buffers for mempool
 * Need to be power of two - 1 for better memory utilization
 * It depends on memory usage of OCF which
 * in itself depends on the workload
 * It is a big number because OCF uses allocators
 * for every request it sends and receives
 *
 * The value of 16383 is tested to work on 24 caches
 * running IO of io_size=512 and io_depth=512, which
 * should be more than enough for any real life scenario.
 * Increase this value if needed. It will result in
 * more memory being used initially on SPDK app start,
 * when compiled with OCF support.
 */
#define ENV_ALLOCATOR_NBUFS 16383
/* [한국어] mempool 1개당 기본 슬롯 개수.
 * 2^n - 1(=16384-1) 형태가 DPDK ring buffer 크기로 가장 효율적(슬롯 헤드/테일 인덱싱이 mod-2^n으로 빠름).
 * OCF는 모든 IO마다 ENV allocator를 사용하므로 큰 값이 필요. 24 cache + io_depth=512 시나리오로 검증됨. */

#define GET_ELEMENTS_COUNT(_limit) (_limit < 0 ? ENV_ALLOCATOR_NBUFS : _limit)
/* [한국어] limit 인자가 음수면 기본값(ENV_ALLOCATOR_NBUFS)을, 그렇지 않으면 사용자 지정 값을 사용하는 헬퍼. */

/* Use unique index for env allocators */
static env_atomic g_env_allocator_index = 0;
/* [한국어] 모든 env_allocator에 유일한 정수 ID를 부여하기 위한 전역 원자 카운터.
 * 풀 이름 prefix에 ":<idx>:"를 붙여 다른 캐시 인스턴스의 동일 이름과 충돌하지 않게 함.
 * 설정자: env_allocator_create_extended()가 atomic_inc_return으로 증가.
 * 읽는 자: 위 함수 내부 snprintf 한 곳.
 * 동기화: env_atomic_inc_return은 atomic add — 다중 스레드에서 안전. */

/*
 * [한국어]
 * env_allocator_new - env_allocator(=spdk_mempool wrapper)에서 슬롯 1개를 빌림.
 *
 * @allocator: 풀 핸들.
 * @return   : 슬롯 포인터(zero=true면 0으로 채워짐), 고갈 시 NULL.
 *
 * spdk_mempool은 per-core 캐시 + lock-free ring으로 hot path에서 거의 무경합. 핫패스에 호출되는 함수.
 * 실행 컨텍스트: OCF IO 경로 — SPDK thread.
 *
 * 호출 체인:
 *   OCF env_allocator_new(매크로) / env_mpool_new → [env_allocator_new] → spdk_mempool_get
 */
void *
env_allocator_new(env_allocator *allocator)
{
	void *mem = spdk_mempool_get(allocator->mempool);
	/* [한국어] DPDK mempool에서 슬롯 1개를 lock-free로 가져옴(per-core 캐시 우선, miss 시 ring 접근). */

	if (spdk_unlikely(!mem)) {
		/* [한국어] 풀 고갈 — 부팅 시 ENV_ALLOCATOR_NBUFS=16383 정도면 거의 발생하지 않지만 방어. */
		return NULL;
	}

	if (allocator->zero) {
		memset(mem, 0, allocator->element_size);
		/* [한국어] zero-fill 정책 — 호출자가 새 슬롯을 0으로 받기 원할 때만 활성. 비싸므로 필요 시에만 true. */
	}

	return mem;
	/* [한국어] 슬롯 포인터 반환 — element_size 바이트 사용 가능. */
}

/*
 * [한국어]
 * env_allocator_create - 기본 슬롯 수로 env_allocator를 생성하는 단순 wrapper.
 *
 * @size : 슬롯 1개 바이트 크기.
 * @name : 풀 이름(통계/디버그용 prefix).
 * @zero : 새 슬롯을 0으로 초기화할지.
 * @return: env_allocator* 또는 NULL.
 *
 * 호출 체인:
 *   OCF/SPDK 사용자 → [env_allocator_create] → env_allocator_create_extended
 */
env_allocator *
env_allocator_create(uint32_t size, const char *name, bool zero)
{
	return env_allocator_create_extended(size, name, -1, zero);
	/* [한국어] limit=-1 → ENV_ALLOCATOR_NBUFS 기본값을 사용하라는 의미. */
}

/*
 * [한국어]
 * env_allocator_create_extended - env_allocator를 생성(슬롯 수 지정 가능).
 *
 * @size : 슬롯 1개 바이트.
 * @name : prefix.
 * @limit: 슬롯 개수(<0이면 ENV_ALLOCATOR_NBUFS=16383).
 * @zero : zero-fill 정책.
 * @return: env_allocator* 또는 NULL.
 *
 * 부팅/캐시 인스턴스 생성 시 호출. 풀 이름은 "ocf_env_<atomic_idx>:<name>" 으로 합성 — 통계/덤프에서
 * 식별 가능하도록.
 *
 * 호출 체인:
 *   OCF init / mpool create → [env_allocator_create_extended] → spdk_mempool_create
 */
env_allocator *
env_allocator_create_extended(uint32_t size, const char *name, int limit, bool zero)
{
	env_allocator *allocator;
	/* [한국어] 새로 만들 핸들. */
	char qualified_name[OCF_ALLOCATOR_NAME_MAX] = {0};
	/* [한국어] 합성된 풀 이름 버퍼 — NUL 종결 보장. */

	snprintf(qualified_name, OCF_ALLOCATOR_NAME_MAX, "ocf_env_%d:%s",
		 env_atomic_inc_return(&g_env_allocator_index), name);
	/* [한국어] 글로벌 카운터 증가 + 호출자 name으로 유일한 풀 이름 합성.
	 * env_atomic_inc_return는 RMW(atomic add) — 동시 호출에서도 ID 충돌 없음. */

	allocator = env_zalloc(sizeof(*allocator), ENV_MEM_NOIO);
	/* [한국어] 핸들 자체를 zero-init으로 할당 — IO 경로 재진입 회피를 위한 NOIO 의도(SPDK에선 calloc). */
	if (!allocator) {
		return NULL;
		/* [한국어] 메모리 부족 — 호출자에 NULL 전달. */
	}

	allocator->mempool = spdk_mempool_create(qualified_name,
			     GET_ELEMENTS_COUNT(limit), size,
			     SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
			     SPDK_ENV_NUMA_ID_ANY);
	/* [한국어] DPDK rte_mempool 위의 SPDK 추상 풀 생성.
	 * - count: limit<0이면 16383, else limit.
	 * - size: 슬롯 1개 바이트.
	 * - cache_size: per-core local cache 크기(기본값) — get/put hot path 가속.
	 * - numa_id: ANY → DPDK가 적절한 NUMA 노드를 선택. */

	if (!allocator->mempool) {
		SPDK_ERRLOG("mempool creation failed\n");
		/* [한국어] 메모리 부족/이름 충돌/hugepage 부족 등 — 운영자 가시성을 위해 즉시 로그. */
		free(allocator);
		/* [한국어] 핸들 누수 방지 — env_zalloc 짝의 free. */
		return NULL;
	}

	allocator->element_size = size;
	/* [한국어] zero-fill에서 사용할 element 크기 기록. */
	allocator->element_count = GET_ELEMENTS_COUNT(limit);
	/* [한국어] 슬롯 총 개수 기록 — destroy 시 누수 검사에 사용. */
	allocator->zero = zero;
	/* [한국어] zero-fill 정책 기록 — new() 호출 시 분기. */

	return allocator;
}

/*
 * [한국어]
 * env_allocator_del - env_allocator로 슬롯 1개를 반환.
 *
 * @allocator: 풀 핸들.
 * @item     : new()가 준 슬롯 포인터.
 *
 * spdk_mempool_put은 lock-free — per-core cache 우선, overflow 시 ring으로 push.
 *
 * 호출 체인:
 *   OCF env_allocator_del / env_mpool_del → [env_allocator_del] → spdk_mempool_put
 */
void
env_allocator_del(env_allocator *allocator, void *item)
{
	spdk_mempool_put(allocator->mempool, item);
	/* [한국어] 슬롯을 mempool로 반환 — IO 경로 hot path. */
}

/*
 * [한국어]
 * env_allocator_destroy - env_allocator 핸들과 그 mempool 해제(누수 검사 포함).
 *
 * @allocator: 풀 핸들 (NULL 안전).
 *
 * 모든 슬롯이 반환된 상태에서 호출되어야 함 — element_count 대비 spdk_mempool_count(현재 free 슬롯 수)가
 * 일치하지 않으면 누수로 보고 ERRLOG + assert(false). 디버그 빌드에서는 abort 트리거.
 *
 * 호출 체인:
 *   OCF deinit / mpool destroy → [env_allocator_destroy] → spdk_mempool_free
 */
void
env_allocator_destroy(env_allocator *allocator)
{
	if (allocator) {
		/* [한국어] NULL 안전 처리. */
		if (allocator->element_count - spdk_mempool_count(allocator->mempool)) {
			/* [한국어] 누수 검사: (전체 슬롯 수) - (현재 free 슬롯 수) != 0 이면 빌려간 항목이 남아 있음.
			 * spdk_mempool_count는 ring에 들어 있는 free 항목 개수를 반환. */
			SPDK_ERRLOG("Not all objects deallocated\n");
			/* [한국어] 운영자 가시성. */
			assert(false);
			/* [한국어] 디버그 빌드 abort — 사용자 OCF 모듈의 라이프사이클 버그를 빠르게 노출. */
		}

		spdk_mempool_free(allocator->mempool);
		/* [한국어] mempool 해제 — 내부적으로 hugepage 영역도 반환. */
		env_free(allocator);
		/* [한국어] 핸들 자체 해제. */
	}
}
/* *** CRC *** */

/*
 * [한국어]
 * env_crc32 - OCF가 사용하는 CRC32 계산을 SPDK CRC32-IEEE로 매핑.
 *
 * @crc    : 누적 CRC 시드(첫 호출은 보통 0).
 * @message: 입력 바이트 버퍼.
 * @len    : 버퍼 길이.
 * @return : 갱신된 CRC32 값.
 *
 * SPDK 구현은 SSE4.2(crc32 명령) 가용 시 하드웨어 가속. OCF는 메타데이터 무결성 체크 등에 사용.
 *
 * 호출 체인:
 *   OCF 메타 코어 → [env_crc32] → spdk_crc32_ieee_update
 */
uint32_t
env_crc32(uint32_t crc, uint8_t const *message, size_t len)
{
	return spdk_crc32_ieee_update(message, len, crc);
	/* [한국어] SPDK는 (data, len, prev_crc) 순서임에 주의 — OCF 호출 규약과 인자 순서가 다름. */
}

/* EXECUTION CONTEXTS */
pthread_mutex_t *exec_context_mutex;
/* [한국어] cpu 개수만큼 할당되는 mutex 배열의 head 포인터.
 * OCF는 "현재 실행 중인 cpu에서 selected critical section은 다른 컨텍스트로 선점되지 않는다"를 가정한다.
 * 커널 환경에선 preempt_disable로 보장되지만, 유저스페이스에서는 그 가정을 mutex로 시뮬레이션 —
 * 즉 "한 cpu의 컨텍스트에서 동시에 둘 이상의 thread가 진입하지 못한다"를 강제.
 * 설정자: init_execution_context(constructor)에서 cpu 개수만큼 동적 할당 + 각 mutex 초기화.
 * 읽는 자: env_get/put_execution_context()에서 cpu 인덱스로 lock/unlock.
 * 동기화: 자신이 보호 도구이며, 배열 자체는 init/deinit이 직렬 실행되므로 별도 보호 불필요. */

/*
 * [한국어]
 * init_execution_context - OCF 실행 컨텍스트(=cpu)별 mutex 배열을 부팅 시 초기화.
 *
 * 라이브러리 로드 시 자동 실행(__attribute__((constructor))) — main 호출 전에 한 번.
 * cpu 개수를 sysconf로 얻어 그 수만큼 mutex 배열을 동적 할당하고 각 mutex를 init.
 * ENV_BUG_ON은 OCF 매크로 — 조건이 true면 BUG 트리거(보통 abort).
 *
 * 호출 체인:
 *   ld.so → __attribute__((constructor)) → [init_execution_context]
 */
static void
__attribute__((constructor)) init_execution_context(void)
{
	unsigned count = env_get_execution_context_count();
	/* [한국어] 시스템 온라인 cpu 수 — 배열 길이 결정. */
	unsigned i;
	/* [한국어] 루프 변수. */

	ENV_BUG_ON(count == 0);
	/* [한국어] cpu 0개는 비정상 — sysconf 실패 등이면 즉시 abort. */
	exec_context_mutex = malloc(count * sizeof(exec_context_mutex[0]));
	/* [한국어] cpu 개수만큼 mutex 배열 할당. malloc 실패 시 다음 ENV_BUG_ON에서 잡힘. */
	ENV_BUG_ON(exec_context_mutex == NULL);
	/* [한국어] 메모리 부족 — env_get/put이 NULL deref하면 더 큰 문제이므로 부팅 단계에서 abort. */
	for (i = 0; i < count; i++) {
		ENV_BUG_ON(pthread_mutex_init(&exec_context_mutex[i], NULL));
		/* [한국어] 각 mutex를 기본 속성으로 init. 실패는 거의 메모리 부족 — abort. */
	}
}

/*
 * [한국어]
 * deinit_execution_context - 종료 시 mutex 배열 해제.
 *
 * 라이브러리 unload 시 자동 실행(__attribute__((destructor))) — atexit과 유사 시점.
 *
 * 호출 체인:
 *   exit/unload → [deinit_execution_context]
 */
static void
__attribute__((destructor)) deinit_execution_context(void)
{
	unsigned count = env_get_execution_context_count();
	/* [한국어] 종료 시점에도 cpu 개수를 다시 측정 — 핫플러그 가능성을 고려한 안전 호출. */
	unsigned i;

	ENV_BUG_ON(count == 0);
	ENV_BUG_ON(exec_context_mutex == NULL);
	/* [한국어] 초기화가 정상이었다면 둘 다 만족 — 아니면 라이브러리 로드 순서 등이 잘못된 것. */

	for (i = 0; i < count; i++) {
		ENV_BUG_ON(pthread_mutex_destroy(&exec_context_mutex[i]));
		/* [한국어] mutex 해제 — busy 상태(잠긴 채)면 EBUSY 반환되어 abort. 종료 시 모두 unlock 가정. */
	}
	free(exec_context_mutex);
	/* [한국어] 배열 자체 해제. */
}

/* get_execution_context must assure that after the call finishes, the caller
 * will not get preempted from current execution context. For userspace env
 * we simulate this behavior by acquiring per execution context mutex. As a
 * result the caller might actually get preempted, but no other thread will
 * execute in this context by the time the caller puts current execution ctx. */
/*
 * [한국어]
 * env_get_execution_context - "이 cpu의 OCF 실행 컨텍스트"를 잠금 획득해 반환.
 *
 * @return: 사용된 cpu 인덱스(이후 put에 그대로 전달해야 함).
 *
 * 영어 주석 요약: 커널 OCF는 preempt_disable로 같은 컨텍스트에 다른 스레드가 못 들어오게 한다.
 * 유저스페이스에서는 이를 cpu별 mutex로 시뮬 — 호출 thread가 OS에 의해 선점될 수는 있으나,
 * 그 사이에 같은 cpu 인덱스의 mutex를 잡고 있는 또 다른 thread는 존재하지 않음을 보장한다.
 * 결과적으로 OCF 코어는 "현재 컨텍스트에서 단독 실행" 가정을 안전하게 사용할 수 있다.
 *
 * 실행 컨텍스트: 임의의 SPDK/일반 thread — sched_getcpu로 현재 cpu 식별.
 *
 * 호출 체인:
 *   OCF 코어(컨텍스트 보호 필요 구간) → [env_get_execution_context] → pthread_mutex_lock
 */
unsigned
env_get_execution_context(void)
{
	unsigned cpu;
	/* [한국어] 반환할 cpu 인덱스. */

	cpu = sched_getcpu();
	/* [한국어] 현재 thread가 실행 중인 cpu 번호 — Linux GNU 확장. -1 반환 가능(예: 컨테이너 제약). */
	cpu = (cpu == -1) ?  0 : cpu;
	/* [한국어] 식별 실패 시 cpu0 인덱스로 fallback — 기능적으론 cpu0 mutex로 직렬화될 뿐 정확성 유지. */

	ENV_BUG_ON(pthread_mutex_lock(&exec_context_mutex[cpu]));
	/* [한국어] 해당 cpu 컨텍스트 mutex lock — 이 시점부터 같은 cpu 인덱스의 OCF 컨텍스트는 단독 실행.
	 * 락 실패(보통 EINVAL/EDEADLK)는 프로그램 버그이므로 즉시 abort. */

	return cpu;
}

/*
 * [한국어]
 * env_put_execution_context - get으로 잡았던 컨텍스트 mutex 해제.
 *
 * @ctx: get이 반환했던 cpu 인덱스 (정확히 그 값을 그대로 전달해야 함).
 *
 * 짝맞는 lock/unlock 의무. 다른 cpu 인덱스로 unlock하면 동기화 무효화 → OCF 데이터 손상 위험.
 *
 * 호출 체인:
 *   OCF 코어 보호 구간 종료 → [env_put_execution_context] → pthread_mutex_unlock
 */
void
env_put_execution_context(unsigned ctx)
{
	pthread_mutex_unlock(&exec_context_mutex[ctx]);
	/* [한국어] mutex unlock. 반환값 무시 — 정상 unlock은 항상 0. */
}

/*
 * [한국어]
 * env_get_execution_context_count - OCF가 인지할 실행 컨텍스트 개수(=온라인 cpu 수)를 반환.
 *
 * @return: cpu 수, sysconf 실패 시 0.
 *
 * sysconf(_SC_NPROCESSORS_ONLN)는 "현재 온라인 cpu" 수를 반환(오프라인/오프된 cpu 제외).
 *
 * 호출 체인:
 *   OCF 코어/init/deinit_execution_context → [env_get_execution_context_count] → sysconf
 */
unsigned
env_get_execution_context_count(void)
{
	int num = sysconf(_SC_NPROCESSORS_ONLN);
	/* [한국어] glibc 헬퍼 — /proc/cpuinfo 또는 sched_getaffinity 등을 종합. */

	return (num == -1) ? 0 : num;
	/* [한국어] 실패(-1) 시 0 반환 — 호출자(생성자)는 ENV_BUG_ON으로 즉시 abort. */
}
