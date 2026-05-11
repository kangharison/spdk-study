/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] DPDK lcore/socket 정보를 SPDK env API로 노출하는 어댑터 (threads.c)
 *
 * === 파일의 역할 ===
 * SPDK는 reactor 모델에서 "1 코어 = 1 reactor = 1 스레드" 매핑을 사용하므로 각 모듈이
 * "현재 어느 코어에서 실행 중인가?", "지금 EAL이 어떤 코어 집합을 활성화했는가?",
 * "다음 코어/NUMA 노드는?" 같은 질문에 빠르게 답할 수 있어야 한다. 본 파일은 그러한
 * SPDK 공개 API(spdk_env_get_*core/numa/cpuset 등)를 DPDK rte_lcore/rte_socket API로
 * 단순 위임한다. 또한 SMT(하이퍼스레딩) 형제 코어를 /sys 경유로 조회하는 헬퍼와
 * EAL remote launch 래퍼(thread_launch_pinned/wait_all)를 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/env_dpdk 서브시스템에 속하며, SPDK reactor 초기화 및 각 reactor가 자기 코어 ID를
 * 알아야 할 때마다 호출되는 가장 빈번한 경로 중 하나이다.
 * 호출 체인: lib/thread/spdk_reactor_run → spdk_env_get_current_core → rte_lcore_id;
 *           rpc framework → spdk_env_get_cpuset → DPDK lcore mask 순회.
 * 실행 컨텍스트: 모든 spdk_thread/reactor에서 호출 가능. 본 파일 함수들은 DPDK lcore TLS와
 * 정적 mask만 읽으므로 락이 필요 없다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: env_internal.h, DPDK rte_lcore.h, spdk/cpuset.h(spdk_cpuset 추상화),
 *         spdk/log.h, spdk/string.h.
 * - 본 파일에 의존: lib/thread, lib/event, app/, examples/ 등 거의 모든 SPDK 코어/사용 코드.
 * - 데이터 흐름: DPDK가 EAL init 시 결정한 lcore mask·socket 정보를 SPDK의 cpuset 형식으로
 *   변환하여 상위 모듈에 전달.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_env_get_core_count/current_core/main_core/first_core/last_core/next_core: lcore 질의.
 * - spdk_env_get_*_numa_id: NUMA(socket) 노드 질의.
 * - spdk_env_get_cpuset: 활성 lcore 집합을 spdk_cpuset로 변환.
 * - env_core_get_smt_cpuset: /sys/devices/system/cpu/cpuN/topology/thread_siblings 파싱.
 * - spdk_env_thread_launch_pinned/wait_all: rte_eal_remote_launch 래퍼.
 */

/* [한국어] env_dpdk 내부 선언 모음. mem/vtophys/pci 함수와 256TB·1GB 매크로 등 포함. */
#include "env_internal.h"

/* [한국어] DPDK 컴파일 매크로(RTE_MAX_LCORE 등). */
#include <rte_config.h>
/* [한국어] rte_lcore_id, rte_lcore_count, rte_get_main_lcore, rte_get_next_lcore,
 * rte_eal_remote_launch 등 lcore API. SPDK 모든 코어/NUMA 함수의 핵심 의존. */
#include <rte_lcore.h>

/* [한국어] spdk_cpuset 추상화. 활성 lcore 집합 노출용. */
#include "spdk/cpuset.h"
/* [한국어] SPDK_ERRLOG 매크로. */
#include "spdk/log.h"
/* [한국어] spdk_strerror(errno→문자열) 사용. */
#include "spdk/string.h"

/* [한국어] Linux의 SMT(하이퍼스레딩) 형제 코어 비트맵을 sysfs에서 가져오는 경로 포맷.
 * %d 자리에 코어 번호가 들어가며, 파일 내용은 cpuset 비트맵 문자열(예: "0,16" 또는 "0-3"). */
#define THREAD_SIBLINGS_FILE \
	"/sys/devices/system/cpu/cpu%d/topology/thread_siblings"

/*
 * [한국어]
 * spdk_env_get_core_count - 활성 lcore 수 반환
 *
 * @return: rte_eal_init이 활성화한 lcore 개수.
 *
 * 본 함수는 DPDK rte_lcore_count를 그대로 호출하는 1줄 래퍼이다. SPDK 코드가 DPDK API를
 * 직접 참조하지 않도록 함으로써 향후 DPDK 의존성 교체를 용이하게 한다.
 * 실행 컨텍스트: 어느 reactor에서나 호출 가능, 락 불필요.
 *
 * 호출 체인: 사용자/프레임워크 → spdk_env_get_core_count → rte_lcore_count
 */
uint32_t
spdk_env_get_core_count(void)
{
	return rte_lcore_count();
	/* [한국어] DPDK가 부팅 시 결정한 활성 lcore 개수 반환(이후 변하지 않음). */
}

/*
 * [한국어]
 * spdk_env_get_current_core - 현재 스레드의 lcore ID 반환
 *
 * @return: 현재 스레드의 lcore ID. 비-EAL 스레드면 LCORE_ID_ANY(=UINT32_MAX).
 *
 * DPDK는 lcore ID를 TLS(thread-local)에 저장하므로 즉시 반환되며 락이 없다.
 * SPDK reactor는 자기 lcore에 핀되어 있으므로 본 함수가 항상 자기 reactor ID를 반환한다.
 *
 * 호출 체인: lib/thread, lib/event 등 → spdk_env_get_current_core → rte_lcore_id
 */
uint32_t
spdk_env_get_current_core(void)
{
	return rte_lcore_id();
	/* [한국어] TLS에 저장된 현재 lcore 반환. EAL이 등록하지 않은 스레드라면 LCORE_ID_ANY. */
}

/*
 * [한국어]
 * spdk_env_get_main_core - SPDK의 main lcore ID 반환
 *
 * @return: rte_eal_init 시 지정된 main(=master) lcore.
 *
 * SPDK는 RPC, 통계 수집 등 1회성/직렬 작업을 main lcore에서 수행하는 관례가 있다.
 */
uint32_t
spdk_env_get_main_core(void)
{
	return rte_get_main_lcore();
	/* [한국어] DPDK 21.11 이상에서 rte_get_master_lcore가 rte_get_main_lcore로 명칭 변경됨. */
}

/*
 * [한국어]
 * spdk_env_get_first_core - 활성 lcore 중 가장 작은 ID 반환
 *
 * @return: 활성 lcore ID 중 가장 작은 값.
 *
 * rte_get_next_lcore(-1, 0, 0): wrap=0(랩어라운드 없이), skip_main=0(main 포함하여) 첫 활성
 * lcore를 찾는다. -1은 "처음부터 시작" 의미.
 */
uint32_t
spdk_env_get_first_core(void)
{
	return rte_get_next_lcore(-1, 0, 0);
	/* [한국어] -1부터 시작 → 0,1,2... 순서로 첫 활성 lcore를 찾아 반환. */
}

/*
 * [한국어]
 * spdk_env_get_last_core - 활성 lcore 중 가장 큰 ID 반환
 *
 * @return: 활성 lcore 중 가장 큰 값.
 *
 * SPDK_ENV_FOREACH_CORE 매크로로 전체 활성 lcore를 순회하며 마지막 값을 기록한다.
 * 활성 lcore가 0개이면 assert로 잡는다(보통은 EAL init이 성공한 이후에만 호출됨).
 */
uint32_t
spdk_env_get_last_core(void)
{
	uint32_t i;
	/* [한국어] 순회 임시 변수. */
	uint32_t last_core = UINT32_MAX;
	/* [한국어] sentinel — 순회 후에도 UINT32_MAX이면 활성 lcore가 0개라는 뜻. */

	SPDK_ENV_FOREACH_CORE(i) {
		/* [한국어] 활성 lcore를 ID 오름차순으로 모두 순회. 마지막에 본 값이 최대값. */
		last_core = i;
	}

	assert(last_core != UINT32_MAX);
	/* [한국어] 활성 lcore가 1개 이상이라는 사실을 디버그 빌드에서 검증. */

	return last_core;
}

/*
 * [한국어]
 * spdk_env_get_next_core - prev_core 뒤의 다음 활성 lcore 반환
 *
 * @prev_core: 시작 위치(이 코어 자체는 제외).
 * @return:    다음 활성 lcore. 더 이상 없으면 UINT32_MAX.
 *
 * 호출자(보통 SPDK_ENV_FOREACH_CORE 루프)가 활성 lcore를 순차적으로 방문할 때 사용.
 */
uint32_t
spdk_env_get_next_core(uint32_t prev_core)
{
	unsigned lcore;
	/* [한국어] DPDK API 반환값 임시. */

	lcore = rte_get_next_lcore(prev_core, 0, 0);
	/* [한국어] wrap=0(끝나면 RTE_MAX_LCORE 반환), skip_main=0(main 포함). */
	if (lcore == RTE_MAX_LCORE) {
		/* [한국어] 더 이상 활성 lcore가 없을 때 SPDK 관례대로 UINT32_MAX 반환. */
		return UINT32_MAX;
	}
	return lcore;
}

/*
 * [한국어]
 * spdk_env_get_numa_id - 주어진 코어의 NUMA(socket) 노드 ID 반환
 *
 * @core:   조회할 lcore 번호.
 * @return: 해당 코어의 NUMA 노드 ID, 또는 SPDK_ENV_NUMA_ID_ANY(코어가 범위 밖일 때).
 *
 * SPDK는 NUMA-aware 메모리 할당과 큐쌍 배치를 위해 본 함수를 자주 호출한다.
 */
int32_t
spdk_env_get_numa_id(uint32_t core)
{
	if (core >= RTE_MAX_LCORE) {
		/* [한국어] 범위 밖 입력에 대해 "어떤 NUMA든 OK"를 의미하는 sentinel 반환. */
		return SPDK_ENV_NUMA_ID_ANY;
	}

	return rte_lcore_to_socket_id(core);
	/* [한국어] DPDK가 부팅 시 sysfs에서 구한 코어→소켓 매핑 테이블 조회. */
}

/*
 * [한국어]
 * spdk_env_get_first_numa_id - 활성 NUMA 노드 중 첫 번째 ID 반환
 *
 * @return: 첫 번째 활성 NUMA 노드 ID.
 *
 * rte_socket_id_by_idx(0): EAL이 인식한 활성 socket 배열의 0번 원소 — 시스템 물리 NUMA
 * 번호와 다를 수 있음에 유의(예: 노드 0,2만 활성이면 idx 0→ID 0, idx 1→ID 2).
 */
int32_t
spdk_env_get_first_numa_id(void)
{
	assert(rte_socket_count() > 0);
	/* [한국어] EAL init 후라면 항상 1 이상이어야 함. */

	return rte_socket_id_by_idx(0);
}

/*
 * [한국어]
 * spdk_env_get_last_numa_id - 활성 NUMA 중 마지막 ID 반환
 */
int32_t
spdk_env_get_last_numa_id(void)
{
	assert(rte_socket_count() > 0);
	/* [한국어] 위와 동일한 사전조건 검사. */

	return rte_socket_id_by_idx(rte_socket_count() - 1);
	/* [한국어] 마지막 인덱스의 socket ID. */
}

/*
 * [한국어]
 * spdk_env_get_next_numa_id - prev_numa_id 다음의 활성 NUMA 노드 ID 반환
 *
 * @prev_numa_id: 시작 위치 NUMA 노드 ID.
 * @return:       다음 활성 NUMA 노드 ID. 더 이상 없으면 INT32_MAX.
 *
 * EAL은 인덱스 기반 socket 배열만 노출하므로, 먼저 prev의 인덱스를 찾고 i+1번째를 반환.
 */
int32_t
spdk_env_get_next_numa_id(int32_t prev_numa_id)
{
	uint32_t i;
	/* [한국어] socket 배열 인덱스. */

	for (i = 0; i < rte_socket_count(); i++) {
		/* [한국어] prev_numa_id의 인덱스를 선형 탐색으로 찾는다. socket 수는 보통 1~8개라 비용 무시 가능. */
		if (rte_socket_id_by_idx(i) == prev_numa_id) {
			break;
		}
	}

	if ((i + 1) < rte_socket_count()) {
		/* [한국어] 다음 인덱스가 유효 범위면 그 socket ID 반환. */
		return rte_socket_id_by_idx(i + 1);
	} else {
		/* [한국어] 끝에 도달 — sentinel 반환. */
		return INT32_MAX;
	}
}

/*
 * [한국어]
 * spdk_env_get_cpuset - 활성 lcore 집합을 spdk_cpuset로 변환
 *
 * @cpuset: 출력. 호출자가 미리 할당한 spdk_cpuset 객체.
 *
 * 모든 활성 lcore 비트를 세팅한 cpuset을 만들어 반환한다. RPC에서 reactor mask를
 * 보고할 때 등에 사용.
 */
void
spdk_env_get_cpuset(struct spdk_cpuset *cpuset)
{
	uint32_t i;
	/* [한국어] 순회 임시. */

	spdk_cpuset_zero(cpuset);
	/* [한국어] 입력으로 받은 cpuset을 모두 0으로 초기화 — caller-side 잔여값 제거. */
	SPDK_ENV_FOREACH_CORE(i) {
		/* [한국어] 활성 lcore마다 비트를 1로 설정. */
		spdk_cpuset_set_cpu(cpuset, i, true);
	}
}

/*
 * [한국어]
 * env_core_get_smt_cpuset - 단일 코어의 SMT 형제 코어 비트맵을 cpuset에 OR
 *
 * @cpuset: 결과 누적 cpuset(in/out). 본 함수는 추가만 한다(zero하지 않음).
 * @core:   sysfs에서 thread_siblings를 조회할 코어 번호.
 * @return: 성공적으로 파싱하여 반영했으면 true, 실패 시 false.
 *
 * /sys/devices/system/cpu/cpuN/topology/thread_siblings 파일을 읽어 SMT 형제 코어 비트맵을
 * 얻고, 호출자에게 OR로 합쳐 반환한다. 즉, "core를 포함하는 모든 SMT 형제"를 cpuset에 더한다.
 * Linux 외 OS에서는 false를 반환(미지원).
 *
 * 실행 컨텍스트: 일반 스레드(보통 메인). 파일 I/O를 하므로 polling 핫패스에서 호출 금지.
 *
 * 호출 체인: spdk_env_core_get_smt_cpuset → env_core_get_smt_cpuset → fopen/getline/spdk_cpuset_parse
 */
static bool
env_core_get_smt_cpuset(struct spdk_cpuset *cpuset, uint32_t core)
{
#ifdef __linux__
	/* [한국어] Linux 한정 구현 — sysfs는 Linux에만 존재. */
	struct spdk_cpuset smt_siblings;
	/* [한국어] sysfs에서 파싱한 결과를 임시로 담을 로컬 cpuset. */
	char path[PATH_MAX];
	/* [한국어] sysfs 경로 버퍼. */
	FILE *f;
	/* [한국어] 파일 핸들. */
	char *line = NULL;
	/* [한국어] getline이 동적 할당한 줄 버퍼(반드시 free 필요). */
	size_t len = 0;
	/* [한국어] getline에 전달할 버퍼 크기 — 0이면 getline이 알아서 할당. */
	ssize_t read;
	/* [한국어] getline 결과 바이트 수(-1=실패). */
	bool valid = false;
	/* [한국어] 성공/실패 플래그. ret 라벨에서 자원 정리 후 그대로 반환. */

	snprintf(path, sizeof(path), THREAD_SIBLINGS_FILE, core);
	/* [한국어] /sys/.../cpu<core>/topology/thread_siblings 경로 조립. */
	f = fopen(path, "r");
	/* [한국어] 읽기 전용으로 열기. /sys는 항상 ASCII 텍스트. */
	if (f == NULL) {
		/* [한국어] 일부 컨테이너/CPU offline 상태에서는 파일이 없을 수 있음. */
		SPDK_ERRLOG("Could not fopen('%s'): %s\n", path, spdk_strerror(errno));
		return false;
	}
	read = getline(&line, &len, f);
	/* [한국어] 한 줄 읽기. line 자동 할당. */
	if (read == -1) {
		/* [한국어] EOF 또는 read 에러. line은 그래도 free 해야 함(아래 ret로 점프). */
		SPDK_ERRLOG("Could not getline() for '%s': %s\n", path, spdk_strerror(errno));
		goto ret;
	}

	/* Remove trailing newline */
	line[strlen(line) - 1] = 0;
	/* [한국어] sysfs 줄 끝 '\n' 제거 — spdk_cpuset_parse는 깔끔한 비트맵 문자열을 기대. */
	if (spdk_cpuset_parse(&smt_siblings, line)) {
		/* [한국어] "0,16" 또는 "0-3" 같은 cpuset 표기 파싱. 비-0 반환은 실패. */
		SPDK_ERRLOG("Could not parse '%s' from '%s'\n", line, path);
		goto ret;
	}

	valid = true;
	/* [한국어] 파싱 성공 표시. */
	spdk_cpuset_or(cpuset, &smt_siblings);
	/* [한국어] 결과를 출력 cpuset에 OR로 누적. 호출자가 여러 코어에 대해 반복하면 합집합이 됨. */
ret:
	free(line);
	/* [한국어] getline이 할당한 버퍼 해제. NULL 안전. */
	fclose(f);
	/* [한국어] 파일 닫기. */
	return valid;
#else
	/* [한국어] non-Linux: SMT 정보 출처가 없음 — 미지원으로 false 반환. */
	return false;
#endif
}

/*
 * [한국어]
 * spdk_env_core_get_smt_cpuset - 단일/전체 코어에 대한 SMT 형제 cpuset 계산
 *
 * @cpuset: 출력 cpuset.
 * @core:   특정 코어. UINT32_MAX이면 활성 lcore 전체에 대해 SMT 합집합을 구함.
 * @return: 성공 시 true, 어느 한 코어라도 실패하면 false.
 *
 * SMT 페어링 정보를 사용하는 reactor 배치 정책(예: "한 물리 코어 안에서만 두 reactor를
 * 묶어 cache locality 활용") 등에 사용된다.
 *
 * 호출 체인: 상위 정책 코드 → spdk_env_core_get_smt_cpuset → env_core_get_smt_cpuset
 */
bool
spdk_env_core_get_smt_cpuset(struct spdk_cpuset *cpuset, uint32_t core)
{
	uint32_t i;
	/* [한국어] 순회 임시. */

	spdk_cpuset_zero(cpuset);
	/* [한국어] 결과 누적 전 0으로 시작. */

	if (core != UINT32_MAX) {
		/* [한국어] 특정 코어 한정 모드 — 단순 위임. */
		return env_core_get_smt_cpuset(cpuset, core);
	}

	SPDK_ENV_FOREACH_CORE(i) {
		/* [한국어] 모든 활성 lcore에 대해 SMT 형제를 OR로 합침. */
		if (!env_core_get_smt_cpuset(cpuset, i)) {
			/* [한국어] 어느 코어 한 곳이라도 실패하면 즉시 실패 반환 — 부분 결과는 신뢰 불가. */
			return false;
		}
	}

	return true;
}

/*
 * [한국어]
 * spdk_env_thread_launch_pinned - 지정 lcore에서 함수를 실행 (pthread 핀)
 *
 * @core:   대상 lcore ID.
 * @fn:     실행할 함수(이 함수가 반환할 때까지 대상 lcore는 점유됨).
 * @arg:    fn의 인자.
 * @return: rte_eal_remote_launch의 반환값. 0 성공, -EBUSY 등 실패.
 *
 * DPDK rte_eal_remote_launch의 1줄 래퍼. 대상 lcore가 WAIT 상태여야 launch 성공.
 * 실행 컨텍스트: 메인 코어(또는 어떤 lcore든) 호출 가능, 대상 lcore에서 fn 실행.
 *
 * 호출 체인: 사용자/lib_event → spdk_env_thread_launch_pinned → rte_eal_remote_launch
 */
int
spdk_env_thread_launch_pinned(uint32_t core, thread_start_fn fn, void *arg)
{
	int rc;
	/* [한국어] 반환 코드 임시. */

	rc = rte_eal_remote_launch(fn, arg, core);
	/* [한국어] DPDK가 미리 만들어둔 lcore 워커 스레드에 작업을 디스패치. 비동기 시작. */

	return rc;
}

/*
 * [한국어]
 * spdk_env_thread_wait_all - 모든 lcore 워커가 idle로 복귀할 때까지 대기
 *
 * rte_eal_mp_wait_lcore: main을 제외한 모든 lcore가 RUNNING/FINISHED 상태에서 WAIT으로
 * 돌아올 때까지 블록 대기. 종료/재시작 단계에서 호출.
 *
 * 호출 체인: spdk_env_fini 또는 사용자 종료 루틴 → spdk_env_thread_wait_all
 */
void
spdk_env_thread_wait_all(void)
{
	rte_eal_mp_wait_lcore();
	/* [한국어] DPDK가 모든 워커 lcore의 join을 처리. */
}
