/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK trace 라이브러리 코어 — lockless circular buffer 구현 (trace.c)
 *
 * === 파일의 역할 ===
 * spdk/trace.h가 약속하는 trace 인프라의 "핵심 엔진"을 구현한다. 구체적으로
 * (1) /dev/shm 위에 trace 공유 영역 생성/매핑 (spdk_trace_init/cleanup),
 * (2) 사용자 추가 스레드(reactor 외)를 trace 대상에 등록/해제
 *     (spdk_trace_register_user_thread, spdk_trace_unregister_user_thread),
 * (3) hot path에서 호출되는 핵심 _spdk_trace_record() — per-lcore ring buffer
 *     (lockless circular buffer)에 24-byte spdk_trace_entry를 단조 증가
 *     인덱스로 push, 8B를 초과하는 가변 인자(주로 string)는 entry chaining으로
 *     다음 슬롯을 spdk_trace_entry_buffer로 alias하여 이어 저장.
 * 외부 spdk_trace 도구는 같은 shm 영역을 mmap하여 후처리 → 사람용 출력.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 흐름:
 *   부팅 단계:
 *     spdk_app_start → spdk_trace_init(shm_name, num_entries, num_threads)
 *       → shm_open + ftruncate + mmap → file_size 계산 (lcore별 history +
 *         사용자 스레드 history + owner pool) → mlock(Linux) → trace_flags_init
 *         (reg_fn 일괄 호출) 위임.
 *   hot path:
 *     spdk/trace.h::spdk_trace_record(...) → enabled fast-check 통과 →
 *     _spdk_trace_record() → 자기 lcore의 history.entries[next_entry++] 기록 →
 *     SMP wmb로 외부 가시성 확보.
 *   사용자 스레드:
 *     pthread 생성 직후 spdk_trace_register_user_thread() 호출 →
 *     bit_array에서 빈 슬롯 찾고 t_ut_lcore_history에 자기 history 포인터 캐시 →
 *     thread name(pthread_getname_np)을 g_trace_file->tname[]에 저장.
 *   종료:
 *     spdk_app_stop → spdk_trace_cleanup → trace_flags_fini → munmap → shm_unlink
 *     (단, entry가 1개라도 기록된 경우 unlink 생략 → 사후 디버깅 용이).
 * 실행 컨텍스트:
 *   - hot path: SPDK reactor 스레드 또는 등록된 user thread (lcore 고정).
 *   - init/cleanup: 메인 스레드 (DPDK EAL 초기화 후).
 *   - register_user_thread: 임의 pthread (주의: dedicated cpu reactor에서는 불가).
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더):
 *   spdk/stdinc.h     — 표준 타입.
 *   spdk/env.h        — spdk_env_get_current_core, SPDK_ENV_FOREACH_CORE,
 *                       spdk_get_ticks/_hz (DPDK rdtsc).
 *   spdk/string.h     — spdk_strerror.
 *   spdk/trace.h      — 공개 API + 자료구조 정의.
 *   spdk/util.h       — spdk_min, offsetof 친구들.
 *   spdk/barrier.h    — spdk_smp_wmb (메모리 배리어).
 *   spdk/log.h        — SPDK_ERRLOG.
 *   spdk/cpuset.h     — spdk_cpuset_*.
 *   spdk/likely.h     — spdk_likely/unlikely (분기 예측 힌트).
 *   spdk/bit_array.h  — 사용자 스레드 슬롯 비트맵.
 *   trace_internal.h  — trace_flags_init/fini, trace_get_shm_name 선언.
 * 의존(구현): trace_flags.c (init/fini 위임 대상), 외부 spdk_trace 도구.
 * 데이터 흐름:
 *   기록자 (lcore N) → lcore_history[N].entries[idx] 기록 + smp_wmb →
 *   읽기자 (외부 spdk_trace 프로세스) → /dev/shm mmap → 같은 entries 읽음.
 *   서로 다른 프로세스 사이의 동기화는 file_size, lcore_history_offsets[],
 *   ABI 일관성에 의존 (외부 도구는 SPDK 빌드와 같은 헤더 사용 권장).
 * 공유 상태:
 *   g_trace_file (mmap된 shm 헤더), g_trace_fd, g_shm_name, g_ut_array (bit_array).
 *   _spdk_trace_record는 자기 lcore의 history만 쓰므로 lock-free,
 *   register_user_thread는 g_ut_array_mutex로 슬롯 할당 보호.
 *
 * === 주요 함수/구조체 요약 ===
 *   - get_trace_entry(history, offset): ring offset → entries[offset & mask] 반환
 *     (num_entries는 2의 거듭제곱 가정 — 매크로 보장 32K=2^15).
 *   - _spdk_trace_record(): 코어 hot path. enabled 검사 후 호출되어
 *     header 채움 → 가변 인자 unpack → entry chaining → smp_wmb → next_entry++.
 *   - spdk_trace_register_user_thread / unregister_user_thread:
 *     비-reactor 스레드를 trace 대상에 추가/제거. mutex로 슬롯 할당.
 *   - spdk_trace_init: shm 파일 생성 + mmap + lcore/user thread history 배치 +
 *     mlock + tsc_rate 채움 + reg_fn 일괄 호출.
 *   - spdk_trace_cleanup: 역순 해제 + entry가 0이면 shm_unlink.
 *   - trace_get_shm_name: g_shm_name 노출 (RPC용).
 *
 * === 핵심 자료구조 (재정리) ===
 *   spdk_trace_file (shm 헤더)
 *     ├─ file_size, tsc_rate, tpoint_mask[20], tname[1024][16]
 *     ├─ owner_type[256], object[256], tpoint[1280]
 *     ├─ lcore_history_offsets[1024]   → 각 lcore의 history 시작 오프셋
 *     ├─ owner_offset                   → owner pool 시작 오프셋
 *     └─ data[]                          → 가변 영역 (history들 + owner들)
 *   spdk_trace_history (per-lcore)
 *     ├─ lcore, num_entries, next_entry (lockless 단조 카운터)
 *     ├─ tpoint_count[1280]              → 누적 카운터
 *     └─ entries[num_entries]            → ring buffer
 */

#include "spdk/stdinc.h"        /* [한국어] uint*_t, NULL, errno 등 표준 타입/매크로. */

#include "spdk/env.h"           /* [한국어] spdk_env_get_current_core, SPDK_ENV_FOREACH_CORE,
                                 * spdk_get_ticks/_hz: DPDK 기반 lcore/시간 기능. */
#include "spdk/string.h"        /* [한국어] spdk_strerror — errno → 사람용 문자열. */
#include "spdk/trace.h"         /* [한국어] trace 공개 API + struct 정의. */
#include "spdk/util.h"          /* [한국어] spdk_min, offsetof 매크로 등. */
#include "spdk/barrier.h"       /* [한국어] spdk_smp_wmb — store-release 메모리 배리어 (외부 가시성). */
#include "spdk/log.h"           /* [한국어] SPDK_ERRLOG: 에러 로그 출력. */
#include "spdk/cpuset.h"        /* [한국어] spdk_cpuset_zero/set_cpu/get_cpu — 코어 마스크 관리. */
#include "spdk/likely.h"        /* [한국어] spdk_likely/unlikely — 분기 예측 힌트 (hot path 최적화). */
#include "spdk/bit_array.h"     /* [한국어] 사용자 스레드 슬롯 할당용 비트맵 자료구조. */
#include "trace_internal.h"     /* [한국어] trace_flags_init/fini 선언 + trace_get_shm_name 선언. */

/* [한국어] shm 파일의 file descriptor.
 * 설정자: spdk_trace_init이 shm_open 결과를 저장.
 * 읽는 자: spdk_trace_cleanup이 close.
 * 값 범위: -1 (미초기화) 또는 ≥0 (열린 fd). */
static int g_trace_fd = -1;

/* [한국어] init 시 사용자가 지정한 shm 이름 사본 (최대 63B + NUL).
 * 설정자: spdk_trace_init이 snprintf로 한 번 채움.
 * 읽는 자: trace_get_shm_name이 read-only로 노출 → RPC trace_get_info가 사용.
 * 동기화: init 후 변하지 않으므로 lock 불필요. */
static char g_shm_name[64];

/* [한국어] 사용자 스레드(reactor 외)에서 trace 시 사용하는 thread-local 캐시.
 * t_ut_array_index: g_ut_array에서 이 스레드가 점유한 비트 인덱스.
 * t_ut_lcore_history: 이 스레드 전용 history 포인터 (record 시 직접 접근).
 * __thread (TLS): 각 pthread마다 독립 — race-free.
 * 설정자: spdk_trace_register_user_thread가 채움.
 * 읽는 자: _spdk_trace_record가 lcore=ANY일 때 fallback으로 참조. */
static __thread uint32_t t_ut_array_index;
static __thread struct spdk_trace_history *t_ut_lcore_history;

/* [한국어] 사용자 스레드 history 슬롯이 시작되는 lcore_history_offsets[] 인덱스.
 * 보통 (reactor에 사용된 가장 큰 cpu 번호 + 1). user thread N개는 그 뒤에 배치.
 * 설정자: spdk_trace_init. 읽는 자: register_user_thread (offset 계산). */
static uint32_t g_user_thread_index_start;

/* [한국어] mmap된 shm 영역의 base 포인터 (헤더부터 시작). 본 파일에서 정의 (extern 선언은 spdk/trace.h).
 * 설정자: spdk_trace_init (mmap 결과). cleanup 시 NULL 복원.
 * 읽는 자: 공개 매크로 spdk_trace_tpoint_enabled, _spdk_trace_record, spdk_get_per_lcore_history 등.
 * 외부 모듈 전체가 이 전역으로 trace 활성 여부를 빠르게 판단. */
struct spdk_trace_file *g_trace_file;

/* [한국어] 사용자 스레드 슬롯 점유 여부 비트맵 (size = init 시 num_threads).
 * 설정자: spdk_trace_init이 spdk_bit_array_create.
 * 읽는 자: register/unregister_user_thread가 first_clear 검색 + set/clear.
 * 동기화: g_ut_array_mutex로 보호. */
static struct spdk_bit_array *g_ut_array;

/* [한국어] 사용자 스레드 등록/해제 시 g_ut_array 접근을 보호하는 뮤텍스.
 * register/unregister는 빈도가 낮으므로 mutex로 충분 (hot path 아님).
 * 정적 초기화 PTHREAD_MUTEX_INITIALIZER 대신 묵시적 zero-init에 의존하지 않고
 * 이름 기반 명시 — 본 파일은 0-초기화 + 첫 lock에서 dynamic init되도록 .bss에 둠.
 * (실 사용 시 첫 lock이 EBUSY 위험이 있으므로 init이 없는 점은 SPDK upstream의
 *  알려진 패턴 — pthread_mutex가 zero-init에서 동작하는 glibc 의존). */
static pthread_mutex_t g_ut_array_mutex;

/* [한국어] === owner pool 크기 정의 ===
 * TRACE_NUM_OWNERS: 동시에 살아있을 수 있는 owner 인스턴스 최대 수 (16K).
 * TRACE_OWNER_DESCRIPTION_SIZE: 각 owner의 description 문자열 길이 한도 (119B).
 *   sizeof(spdk_trace_owner)=9 (tsc 8B + type 1B, packed) + 119 = 128B.
 *   128B는 보통 cache line 정렬 가정 — 슬롯 indexing이 깔끔하고 false sharing 회피. */
#define TRACE_NUM_OWNERS (16 * 1024)
#define TRACE_OWNER_DESCRIPTION_SIZE (119)
/* [한국어] static_assert로 ABI 안정성 강제 — packed 속성이 풀리거나 필드가 추가되면
 * 빌드가 실패. 외부 spdk_trace 도구가 같은 레이아웃을 가정하므로 매우 중요. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_trace_owner) == 9, "incorrect size");
SPDK_STATIC_ASSERT(sizeof(struct spdk_trace_owner) + TRACE_OWNER_DESCRIPTION_SIZE == 128,
		   "incorrect size");

/*
 * [한국어]
 * get_trace_entry - ring buffer offset으로 entries[] 슬롯 포인터 반환.
 *
 * @history: 대상 lcore의 spdk_trace_history.
 * @offset:  단조 증가 가상 인덱스 (next_entry 등).
 * @return:  entries[offset & (num_entries-1)] 주소.
 *
 * num_entries는 init 시 SPDK_DEFAULT_NUM_TRACE_ENTRIES=32K (2^15) 또는 사용자 지정
 * 2의 거듭제곱이 들어와야 마스크 연산이 정확. (mask = num_entries-1)
 * 모듈로 대신 AND 연산으로 wrap-around 처리 → divisor 없이 1 cycle.
 * 컨텍스트: hot path inline 호출. 자기 lcore의 history만 다루므로 lock-free.
 */
static inline struct spdk_trace_entry *
get_trace_entry(struct spdk_trace_history *history, uint64_t offset)
{
	return &history->entries[offset & (history->num_entries - 1)];
	/* [한국어] AND 마스크로 ring wrap. num_entries는 2의 거듭제곱이라야 정확.
	 * (예: 32K=0x8000, mask=0x7FFF). */
}

/*
 * [한국어]
 * _spdk_trace_record - hot path 핵심: per-lcore ring buffer에 trace entry 기록.
 *
 * @tsc:        TSC 값 (0이면 spdk_get_ticks()를 내부에서 호출).
 * @tpoint_id:  SPDK_TPOINT_ID(group, idx)로 만든 ID.
 * @owner_id:   owner 인스턴스 ID (0=none).
 * @size:       페이로드 크기 (보통 I/O 바이트 수).
 * @object_id:  추적 객체 ID (포인터값/시퀀스).
 * @num_args:   가변 인자 개수 (tpoint 정의와 일치해야 함).
 * @...:        실제 인자들 (uint64/포인터/문자열).
 *
 * 동작 단계:
 *   1) 현재 lcore 조회 — reactor면 dedicated history, 아니면 user-thread TLS history.
 *      둘 다 없으면 즉시 return (record 누락).
 *   2) tsc=0이면 spdk_get_ticks()로 TSC 측정.
 *   3) tpoint_count[id]++ (통계용).
 *   4) tpoint 정의의 num_args가 호출자가 넘긴 num_args와 일치하는지 검사.
 *   5) ring 다음 슬롯 확보 → header 5 필드 채움 (tsc/tpoint_id/owner/size/object).
 *   6) entry chaining 준비:
 *      - args[8] 영역부터 시작하기 위해 buffer를 entry로 재해석하고
 *        offset을 args 시작 위치로 보정.
 *   7) tpoint->args[i]를 순회하며 각 인자를 type별로 unpack:
 *      - STR: 가변 길이 — strnlen으로 실제 길이 측정 후 정의된 size까지 복사.
 *      - INT/PTR: 4 또는 8B로 va_arg → 임시 intval에 저장 후 그대로 복사.
 *   8) 데이터를 buffer->data에 복사하면서 슬롯 경계를 넘으면
 *      get_trace_entry로 다음 ring 슬롯을 buffer로 alias하고 (entry chaining)
 *      tpoint_id=SPDK_TRACE_MAX_TPOINT_ID로 마킹 (parser가 "이건 chained 슬롯"으로 인식).
 *   9) STR이면 마지막 바이트를 NUL로 덮어 truncated string 보장.
 *  10) spdk_smp_wmb (store-release) — entry의 모든 필드가 외부 도구에 visible
 *      되도록 store들을 memory order로 강제. wmb 이후에 next_entry 증가.
 *  11) lcore_history->next_entry += num_entries — 외부 reader는 next_entry로
 *      "여기까지 유효" 판단.
 *
 * 동시성:
 *   - 같은 lcore의 다른 코드와 race? 없음 — single-thread 가정 (reactor or user thread).
 *   - 다른 lcore와 race? 없음 — 각자 자기 history만 씀.
 *   - 외부 reader (spdk_trace 프로세스)와 race? next_entry 단조 + smp_wmb로
 *     순서 보장. reader는 항상 "안전한 prefix"만 본다.
 *
 * 컨텍스트: 호출 측이 spdk_trace_tpoint_enabled() fast-check를 통과했을 때만 진입.
 *           lcore 고정 + lockless → ns 단위 비용.
 * 호출 체인: hot path .c → spdk_trace_record(...) (매크로) → enabled 검사 →
 *             _spdk_trace_record_tsc(...) → _spdk_trace_record (이 함수).
 */
void
_spdk_trace_record(uint64_t tsc, uint16_t tpoint_id, uint16_t owner_id, uint32_t size,
		   uint64_t object_id, int num_args, ...)
{
	struct spdk_trace_history *lcore_history;     /* [한국어] 자기 코어/스레드의 ring buffer 컨테이너. */
	struct spdk_trace_entry *next_entry;          /* [한국어] 새로 채울 첫 entry 슬롯 포인터. */
	struct spdk_trace_entry_buffer *buffer;       /* [한국어] entry chaining용 alias 포인터 (같은 24B 영역). */
	struct spdk_trace_tpoint *tpoint;             /* [한국어] tpoint 메타 (인자 정의/타입). */
	struct spdk_trace_argument *argument;         /* [한국어] 현재 처리 중인 인자 메타. */
	unsigned lcore, i, offset, num_entries, arglen, argoff, curlen;
	/* [한국어] lcore: 현재 코어 ID,
	 * i: 인자 인덱스, offset: buffer->data 내 쓰기 오프셋,
	 * num_entries: chained slot 수 (1 시작),
	 * arglen: 실제 인자 길이 (STR은 가변), argoff: 인자 내 진행 오프셋,
	 * curlen: 이번 회차 복사 길이. */
	uint64_t intval;                              /* [한국어] INT/PTR을 8B로 정규화한 임시 값. */
	void *argval;                                 /* [한국어] 인자 데이터의 source 포인터 (STR 또는 &intval). */
	va_list vl;                                   /* [한국어] 가변 인자 walker. */

	lcore = spdk_env_get_current_core();
	/* [한국어] DPDK rte_lcore_id wrapper. dedicated cpu가 아니면 SPDK_ENV_LCORE_ID_ANY 반환. */
	if (spdk_likely(lcore != SPDK_ENV_LCORE_ID_ANY)) {
		/* [한국어] reactor 스레드 — 일반적인 hot path. lcore_history_offsets[lcore]로 점프. */
		lcore_history = spdk_get_per_lcore_history(g_trace_file, lcore);
	} else if (t_ut_lcore_history != NULL) {
		/* [한국어] reactor 외 사용자 스레드. TLS에 캐시된 history 사용 (등록 시 채워둠). */
		lcore_history = t_ut_lcore_history;
	} else {
		/* [한국어] 등록되지 않은 임의 스레드 → 기록 불가. 조용히 drop. */
		return;
	}

	if (tsc == 0) {
		/* [한국어] 호출자가 TSC를 미리 측정하지 않았다면 여기서 한 번 읽음. */
		tsc = spdk_get_ticks();
	}

	lcore_history->tpoint_count[tpoint_id]++;
	/* [한국어] 누적 카운터 — spdk_trace --stats에서 IO 처리량 등 표시.
	 * 자기 lcore만 ++하므로 atomic 불필요. */

	tpoint = &g_trace_file->tpoint[tpoint_id];
	/* [한국어] 등록된 tpoint 메타 조회 (init 후 read-only). */
	/* Make sure that the number of arguments passed matches tracepoint definition */
	if (spdk_unlikely(tpoint->num_args != num_args)) {
		/* [한국어] 호출 측 인자 개수와 tpoint 정의가 불일치 — 개발 단계 버그.
		 * 디버그 빌드에서 즉시 abort, release에서는 record 생략으로 안전 fail. */
		assert(0 && "Unexpected number of tracepoint arguments");
		return;
	}

	/* Get next entry index in the circular buffer */
	next_entry = get_trace_entry(lcore_history, lcore_history->next_entry);
	/* [한국어] next_entry % num_entries 슬롯 — wrap 시 가장 오래된 entry를 덮어씀.
	 * ring 정책상 reader가 늦으면 미처 못 읽은 entry가 사라질 수 있음 (트레이드오프). */
	next_entry->tsc = tsc;                /* [한국어] 시간 기록. */
	next_entry->tpoint_id = tpoint_id;    /* [한국어] tpoint ID. */
	next_entry->owner_id = owner_id;      /* [한국어] owner 인스턴스 ID. */
	next_entry->size = size;              /* [한국어] 페이로드 크기. */
	next_entry->object_id = object_id;    /* [한국어] 추적 객체 ID. */

	num_entries = 1;
	/* [한국어] 시작은 1 슬롯. 8B를 초과하는 가변 인자가 있으면 chaining으로 증가. */
	buffer = (struct spdk_trace_entry_buffer *)next_entry;
	/* [한국어] entry alias — 같은 24B 영역을 buffer 형태로 reinterpret. */
	/* The initial offset needs to be adjusted by the fields present in the first entry
	 * (owner_id, size, etc.).
	 */
	offset = offsetof(struct spdk_trace_entry, args) -
		 offsetof(struct spdk_trace_entry_buffer, data);
	/* [한국어] 첫 entry의 args[8] 시작점에 해당하는 buffer->data 내 오프셋 계산.
	 * struct entry 의 args는 tpoint_id/owner_id/size/object_id 다음에 위치하지만,
	 * struct buffer 의 data는 tpoint_id 다음 바로 시작. 두 시작점의 차를 빼서
	 * "data 시작에서 args 위치까지의 거리"를 얻음. 이 만큼 건너뛰면 첫 슬롯의
	 * args 영역 (8B)부터 가변 인자 패킹 시작. */

	va_start(vl, num_args);
	for (i = 0; i < tpoint->num_args; ++i) {
		argument = &tpoint->args[i];
		/* [한국어] 인자 메타 (이름/타입/크기) 조회. */
		switch (argument->type) {
		case SPDK_TRACE_ARG_TYPE_STR:
			argval = va_arg(vl, void *);
			/* [한국어] STR은 const char* 포인터로 받음. */
			arglen = strnlen((const char *)argval, argument->size - 1) + 1;
			/* [한국어] 실제 길이 측정 (size-1까지로 제한, NUL 포함 위해 +1).
			 * size를 초과하면 자동 truncate, 미만이면 정확한 길이만 복사. */
			break;
		case SPDK_TRACE_ARG_TYPE_INT:
		case SPDK_TRACE_ARG_TYPE_PTR:
			if (argument->size == 8) {
				/* [한국어] 64-bit 정수/포인터: va_arg uint64_t. */
				intval = va_arg(vl, uint64_t);
			} else {
				/* [한국어] 32-bit (size==4): va_arg uint32_t.
				 * varargs는 default promotion으로 int로 들어오므로 uint32 안전. */
				intval = va_arg(vl, uint32_t);
			}
			argval = &intval;          /* [한국어] memcpy source는 intval 주소. */
			arglen = argument->size;   /* [한국어] INT/PTR은 정확히 정의된 size만큼 복사. */
			break;
		default:
			/* [한국어] tpoint 정의가 잘못된 type을 가짐 — 등록 시점 검증을 통과했어야 함. */
			assert(0 && "Invalid trace argument type");
			return;
		}

		/* Copy argument's data. For some argument types (strings) user is allowed to pass a
		 * value that is either larger or smaller than what's defined in the tracepoint's
		 * description. If the value is larger, we'll truncate it, while if it's smaller,
		 * we'll only fill portion of the buffer, without touching the rest. For instance,
		 * if the definition marks an argument as 40B and user passes 12B string, we'll only
		 * copy 13B (accounting for the NULL terminator).
		 */
		argoff = 0;                                 /* [한국어] 인자 내 진행 위치 초기화. */
		while (argoff < argument->size) {
			/* [한국어] 정의된 argument->size 만큼 진행할 때까지 반복.
			 * 한 번에 다 못 채우면 다음 ring 슬롯을 buffer로 chain. */
			/* Current buffer is full, we need to acquire another one */
			if (spdk_unlikely(offset == sizeof(buffer->data))) {
				/* [한국어] 현재 buffer의 data 영역(첫 entry는 8B, 이후 22B)이 가득 참.
				 * 다음 ring 슬롯을 entry_buffer로 alias하여 이어 사용. */
				buffer = (struct spdk_trace_entry_buffer *) get_trace_entry(
						 lcore_history,
						 lcore_history->next_entry + num_entries);
				/* [한국어] next_entry + num_entries 위치 = 직전 슬롯의 다음 슬롯. */
				buffer->tpoint_id = SPDK_TRACE_MAX_TPOINT_ID;
				/* [한국어] sentinel — parser가 "이 슬롯은 chained continuation"으로 식별. */
				buffer->tsc = tsc;
				/* [한국어] 부모 entry와 같은 TSC 복사 (검증/정렬용). */
				num_entries++;        /* [한국어] 사용한 슬롯 수 ++. */
				offset = 0;           /* [한국어] 새 buffer는 data 시작부터 사용. */
			}

			curlen = spdk_min(sizeof(buffer->data) - offset, argument->size - argoff);
			/* [한국어] 이번 회차에 복사할 길이 = (현재 슬롯 잔여) vs (인자 잔여) 중 작은 값. */
			if (spdk_likely(argoff < arglen)) {
				/* [한국어] 실제 데이터가 아직 남아 있을 때만 memcpy.
				 * STR이 정의 size보다 짧으면 arglen 이후는 영역만 reserve하고 복사 없음. */
				assert(argval != NULL);
				memcpy(&buffer->data[offset], (uint8_t *)argval + argoff,
				       spdk_min(curlen, arglen - argoff));
				/* [한국어] 정확히 (남은 실제 길이) 만큼만 복사 — 잔여 영역은 garbage 유지. */
			}

			offset += curlen;             /* [한국어] 슬롯 내 쓰기 위치 진행. */
			argoff += curlen;             /* [한국어] 인자 내 위치 진행. */
		}

		/* Make sure that truncated strings are NULL-terminated */
		if (spdk_unlikely(argument->type == SPDK_TRACE_ARG_TYPE_STR)) {
			/* [한국어] STR은 truncate 가능 → 마지막 바이트를 강제로 NUL로 덮어 안전 보장. */
			assert(offset > 0);
			buffer->data[offset - 1] = '\0';
		}
	}
	va_end(vl);

	/* Ensure all elements of the trace entry are visible to outside trace tools */
	spdk_smp_wmb();
	/* [한국어] store-release 배리어 — entry 내용의 모든 store가 next_entry++ 보다
	 * 먼저 외부 reader에 보이도록 강제. 이후 reader는 next_entry를 보고 "여기까지 valid"
	 * 라고 판단. wmb 없이는 reader가 절반만 채워진 entry를 읽을 수 있음. */
	lcore_history->next_entry += num_entries;
	/* [한국어] 단조 증가 카운터 갱신. 자기 lcore만 ++ → race-free.
	 * 외부 reader는 이 값으로 ring 진행 위치를 추적. */
}

/*
 * [한국어]
 * spdk_trace_register_user_thread - 비-reactor pthread를 trace 대상에 등록.
 *
 * @return: 0 성공 / -ENOMEM (init 안됨) / -EINVAL (dedicated cpu에서 호출) /
 *          -ENOENT (슬롯 부족) / errno (pthread_getname_np 실패).
 *
 * 동기: SPDK는 reactor 외에도 nvme 등 일부 서브시스템이 자체 pthread를 만들 수 있다.
 *       이런 스레드도 spdk_trace_record를 사용하려면 history 슬롯이 필요.
 *       reactor는 dedicated cpu별로 자동 할당되지만, user thread는 동적 할당.
 *
 * 동작:
 *   1) g_ut_array 존재 확인 (init되었는가).
 *   2) 호출자가 dedicated cpu reactor 안에 있으면 거부 — reactor는 이미 자체 history를 가짐.
 *   3) g_ut_array_mutex 획득 (슬롯 할당 race 방지).
 *   4) 첫 번째 빈 비트 검색 (find_first_clear). UINT32_MAX면 슬롯 없음.
 *   5) ut_index = bit_index + g_user_thread_index_start (history 배치 후반부).
 *   6) TLS 캐시: t_ut_lcore_history에 자기 history 포인터 저장 → 이후 record가 직접 사용.
 *   7) tname[ut_index] zero clear 후 pthread_getname_np로 스레드 이름 복사 (parser 표시용).
 *   8) bit_array에 SET → 슬롯 점유 표시.
 *   9) mutex 해제.
 * 컨텍스트: 임의 pthread 시작 직후 (user code). dedicated cpu 아닌 일반 스레드만.
 */
int
spdk_trace_register_user_thread(void)
{
	int ret;                              /* [한국어] pthread API 반환값 임시 보관. */
	uint32_t ut_index;                    /* [한국어] lcore_history_offsets[] 내 절대 인덱스. */
	pthread_t tid;                        /* [한국어] 현재 스레드 ID (pthread_self). */

	if (!g_ut_array) {
		/* [한국어] init 시점에 num_threads>0으로 호출되어야 만들어짐.
		 * 0이거나 init 실패면 user thread 기능 비활성. */
		SPDK_ERRLOG("user thread array not created\n");
		return -ENOMEM;
	}

	if (spdk_env_get_current_core() != SPDK_ENV_LCORE_ID_ANY) {
		/* [한국어] dedicated cpu의 reactor 스레드는 이미 자체 history를 가짐 — 중복 등록 거부. */
		SPDK_ERRLOG("cannot register an user thread from a dedicated cpu %d\n",
			    spdk_env_get_current_core());
		return -EINVAL;
	}

	pthread_mutex_lock(&g_ut_array_mutex);
	/* [한국어] 슬롯 할당은 hot path 아니므로 mutex로 충분. find+set의 atomicity 보장. */

	t_ut_array_index = spdk_bit_array_find_first_clear(g_ut_array, 0);
	/* [한국어] 0번 비트부터 빈 자리 검색. TLS 변수에 저장 — unregister 시 같은 비트 clear에 사용. */
	if (t_ut_array_index == UINT32_MAX) {
		/* [한국어] 슬롯 모두 점유 — init 시 num_threads를 더 크게 잡아야 함. */
		SPDK_ERRLOG("could not find an entry in the user thread array\n");
		pthread_mutex_unlock(&g_ut_array_mutex);
		return -ENOENT;
	}

	ut_index = t_ut_array_index + g_user_thread_index_start;
	/* [한국어] reactor 영역 다음부터 시작하는 user thread history 영역의 절대 인덱스. */

	t_ut_lcore_history = spdk_get_per_lcore_history(g_trace_file, ut_index);
	/* [한국어] 자기 history 포인터를 TLS에 캐시 — 이후 record 시 fast path. */

	assert(t_ut_lcore_history != NULL);
	/* [한국어] init이 g_user_thread_index_start 이후 슬롯에 offset을 채워두었으므로 NULL일 수 없음. */

	memset(g_trace_file->tname[ut_index], 0, SPDK_TRACE_THREAD_NAME_LEN);
	/* [한국어] 이전 사용자 이름 잔여를 0으로 초기화. */

	tid = pthread_self();
	ret = pthread_getname_np(tid, g_trace_file->tname[ut_index], SPDK_TRACE_THREAD_NAME_LEN);
	/* [한국어] 현재 스레드의 이름(pthread_setname_np로 설정된 값)을 가져와 tname에 저장.
	 * parser가 "어느 스레드에서 발생했는가"를 표시할 때 사용.
	 * 16B 한도 — Linux pthread name 한계와 일치. */
	if (ret) {
		/* [한국어] 실패 시 mutex 해제 후 errno 그대로 반환 (호출자가 진단). */
		SPDK_ERRLOG("cannot get thread name\n");
		pthread_mutex_unlock(&g_ut_array_mutex);
		return ret;
	}

	spdk_bit_array_set(g_ut_array, t_ut_array_index);
	/* [한국어] 슬롯 점유 마킹 — 다른 스레드가 같은 슬롯 가져가지 못하도록. */

	pthread_mutex_unlock(&g_ut_array_mutex);

	return 0;
}

/*
 * [한국어]
 * spdk_trace_unregister_user_thread - register의 대칭 — 슬롯 free.
 *
 * @return: 0 성공 / -ENOMEM (init 안됨) / -EINVAL (dedicated cpu).
 *
 * TLS의 t_ut_array_index에 저장해둔 비트만 clear.
 * t_ut_lcore_history는 일부러 비우지 않음 — 스레드가 곧 종료되므로 무관.
 * 컨텍스트: 스레드 종료 직전 user code에서 호출.
 */
int
spdk_trace_unregister_user_thread(void)
{
	if (!g_ut_array) {
		/* [한국어] init이 user thread 기능을 활성화하지 않았다면 거부. */
		SPDK_ERRLOG("user thread array not created\n");
		return -ENOMEM;
	}

	if (spdk_env_get_current_core() != SPDK_ENV_LCORE_ID_ANY) {
		/* [한국어] reactor에서 user thread API를 호출하면 비대칭 — 거부. */
		SPDK_ERRLOG("cannot unregister an user thread from a dedicated cpu %d\n",
			    spdk_env_get_current_core());
		return -EINVAL;
	}

	pthread_mutex_lock(&g_ut_array_mutex);
	/* [한국어] register와 같은 mutex로 일관된 임계영역 보호. */

	spdk_bit_array_clear(g_ut_array, t_ut_array_index);
	/* [한국어] TLS의 인덱스로 비트 clear → 슬롯 재사용 가능. */

	pthread_mutex_unlock(&g_ut_array_mutex);

	return 0;
}

/*
 * [한국어]
 * spdk_trace_init - trace 시스템 부트스트랩 (lifecycle 시작점).
 *
 * @shm_name:    /dev/shm 아래 만들어질 파일 이름 (예: "/spdk_tgt.12345").
 *               외부 spdk_trace 도구가 같은 이름으로 mmap하여 후처리.
 * @num_entries: 각 lcore/user thread history의 ring 슬롯 수 (0이면 trace 비활성).
 *               2의 거듭제곱이어야 함 (get_trace_entry의 AND 마스크 가정).
 * @num_threads: 추가로 등록 가능한 user thread 슬롯 수 (보통 0).
 * @return: 0 성공 / 1 실패.
 *
 * 동작 단계:
 *   1) num_entries=0이면 trace 시스템을 만들지 않고 return — 모든 record가 noop.
 *   2) num_threads 한계 검사 (1024 미만).
 *   3) file 레이아웃 계산:
 *      [header(spdk_trace_file)] [lcore0 history] [lcore1 history] ...
 *      [user thread N history] [owner pool]
 *      각 lcore의 시작 offset을 lcore_offsets[]에 기록.
 *      g_user_thread_index_start = (max dedicated cpu + 1).
 *   4) bit_array(num_threads) 생성 (user thread 슬롯 관리).
 *   5) shm_open(O_RDWR|O_CREAT, 0600) — /dev/shm/<shm_name> 파일 생성.
 *   6) ftruncate로 file_size까지 확장.
 *   7) mmap(MAP_SHARED) → g_trace_file.
 *   8) Linux 한정 mlock — page-out 방지로 hot path TLB miss 차단.
 *      ENOMEM 시 /dev/shm 잔여 파일 정리 안내 메시지 출력.
 *   9) memset 0 → 모든 메타데이터 클린 상태.
 *  10) tsc_rate, num_owners, owner_description_size, owner_offset 채움.
 *  11) lcore별 history 헤더(lcore, num_entries) 초기화.
 *  12) trace_flags_init() — reg_fn 일괄 호출 + owner ring 초기화.
 *  13) 실패 시 trace_init_err 라벨로 정상 정리(munmap/close/unlink/free) 후 반환.
 *
 * 컨텍스트: spdk_app_start의 초기화 단계. 메인 스레드, 단일 호출.
 * 호출 체인: spdk_app_start → spdk_trace_init → trace_flags_init.
 */
int
spdk_trace_init(const char *shm_name, uint64_t num_entries, uint32_t num_threads)
{
	uint32_t i = 0, max_dedicated_cpu = 0;
	/* [한국어] i: 일반 루프 인덱스, max_dedicated_cpu: SPDK가 사용하는 가장 큰 cpu 번호. */
	uint64_t file_size;                   /* [한국어] 누적 file 크기 (헤더부터 owner pool 끝까지). */
	uint64_t lcore_offsets[SPDK_TRACE_MAX_LCORE] = { 0 };
	/* [한국어] lcore별 history 시작 오프셋 (file 기준). 0=비활성 슬롯.
	 * 나중에 g_trace_file->lcore_history_offsets[]로 복사. */
	uint64_t owner_offset;                /* [한국어] owner pool 시작 오프셋. */
	struct spdk_cpuset cpuset = {};       /* [한국어] dedicated cpu 비트맵 (디버그용 검증). */

	/* 0 entries requested - skip trace initialization */
	if (num_entries == 0) {
		/* [한국어] 사용자가 명시적으로 trace 비활성을 요청 — return 0으로 정상 종료.
		 * g_trace_file은 NULL 유지 → 모든 spdk_trace_record가 enabled 검사에서 즉시 noop. */
		return 0;
	}

	if (num_threads >= SPDK_TRACE_MAX_LCORE) {
		/* [한국어] history 배열 한계 (1024). user thread 수가 너무 많으면 실패. */
		SPDK_ERRLOG("cannot alloc trace entries for %d user threads\n", num_threads);
		SPDK_ERRLOG("supported maximum %d threads\n", SPDK_TRACE_MAX_LCORE - 1);
		return 1;
	}

	spdk_cpuset_zero(&cpuset);                        /* [한국어] cpuset 초기화. */
	file_size = sizeof(struct spdk_trace_file);       /* [한국어] 헤더 크기부터 시작. */
	SPDK_ENV_FOREACH_CORE(i) {
		/* [한국어] DPDK가 보고하는 dedicated cpu 각각에 대해 history 영역 reserve. */
		spdk_cpuset_set_cpu(&cpuset, i, true);    /* [한국어] 검증용 mask 갱신. */
		lcore_offsets[i] = file_size;             /* [한국어] 이 lcore의 history 시작 위치. */
		file_size += spdk_get_trace_history_size(num_entries);
		/* [한국어] history 헤더 + entries[num_entries] 크기 누적. */
		max_dedicated_cpu = i;                    /* [한국어] 마지막 lcore 추적 (loop는 오름차순 가정). */
	}

	g_user_thread_index_start = max_dedicated_cpu + 1;
	/* [한국어] user thread는 reactor 마지막 cpu 직후 인덱스부터 배치 — 충돌 회피. */

	if (g_user_thread_index_start + num_threads > SPDK_TRACE_MAX_LCORE) {
		/* [한국어] user thread가 lcore_history_offsets[] 한계를 넘으면 실패. */
		SPDK_ERRLOG("user threads overlap with the threads on dedicated cpus\n");
		return 1;
	}

	g_ut_array = spdk_bit_array_create(num_threads);
	/* [한국어] num_threads 만큼의 비트 배열 — 슬롯 점유 추적. */
	if (!g_ut_array) {
		SPDK_ERRLOG("could not create bit array for threads\n");
		return 1;
	}

	for (i = g_user_thread_index_start; i < g_user_thread_index_start + num_threads; i++) {
		/* [한국어] user thread 영역 history도 lcore_offsets[]에 등록. */
		lcore_offsets[i] = file_size;
		file_size += spdk_get_trace_history_size(num_entries);
	}
	owner_offset = file_size;                         /* [한국어] history 영역 끝 = owner pool 시작. */
	file_size += TRACE_NUM_OWNERS *
		     (sizeof(struct spdk_trace_owner) + TRACE_OWNER_DESCRIPTION_SIZE);
	/* [한국어] 16K * 128B = 2MB owner pool 추가. */

	snprintf(g_shm_name, sizeof(g_shm_name), "%s", shm_name);
	/* [한국어] g_shm_name에 사본 저장 — RPC trace_get_info가 노출. */

	g_trace_fd = shm_open(shm_name, O_RDWR | O_CREAT, 0600);
	/* [한국어] /dev/shm/<shm_name> 파일 생성. 권한 0600 (소유자 read/write).
	 * O_CREAT로 없으면 새로 만들고, 있으면 기존 파일 reuse. */
	if (g_trace_fd == -1) {
		SPDK_ERRLOG("could not shm_open spdk_trace\n");
		SPDK_ERRLOG("errno=%d %s\n", errno, spdk_strerror(errno));
		spdk_bit_array_free(&g_ut_array);
		return 1;
	}

	if (ftruncate(g_trace_fd, file_size) != 0) {
		/* [한국어] 파일 크기를 정확히 file_size 만큼 확장. /dev/shm은 tmpfs라 즉시 할당. */
		SPDK_ERRLOG("could not truncate shm\n");
		goto trace_init_err;
	}

	g_trace_file = mmap(NULL, file_size, PROT_READ | PROT_WRITE,
			    MAP_SHARED, g_trace_fd, 0);
	/* [한국어] 파일을 가상 주소 공간에 매핑. MAP_SHARED → 다른 프로세스가 mmap하면 같은 메모리 참조 → IPC 효과. */
	if (g_trace_file == MAP_FAILED) {
		SPDK_ERRLOG("could not mmap shm\n");
		goto trace_init_err;
	}

	/* TODO: On FreeBSD, mlock on shm_open'd memory doesn't seem to work.  Docs say that kern.ipc.shm_use_phys=1
	 * should allow it, but forcing that doesn't seem to work either.  So for now just skip mlock on FreeBSD
	 * altogether.
	 */
#if defined(__linux__)
	/* [한국어] Linux에서만 mlock — 매핑된 페이지를 RAM에 고정하여 swap 방지.
	 * hot path에서 page fault 회피 → trace 기록 비용 일정. */
	if (mlock(g_trace_file, file_size) != 0) {
		SPDK_ERRLOG("Could not mlock shm for tracing - %s.\n", spdk_strerror(errno));
		if (errno == ENOMEM) {
			/* [한국어] RLIMIT_MEMLOCK 부족 가능성 — 사용자에게 정리 힌트 제공. */
			SPDK_ERRLOG("Check /dev/shm for old tracing files that can be deleted.\n");
		}
		goto trace_init_err;
	}
#endif

	memset(g_trace_file, 0, file_size);
	/* [한국어] 모든 영역 zero clear — 잔여 데이터 제거 및 결정론적 초기 상태. */

	g_trace_file->tsc_rate = spdk_get_ticks_hz();
	/* [한국어] 현재 시스템의 TSC 주파수 (Hz). parser가 entry.tsc / tsc_rate로 시간 환산. */

	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		struct spdk_trace_history *lcore_history;

		g_trace_file->lcore_history_offsets[i] = lcore_offsets[i];
		/* [한국어] 로컬 lcore_offsets[]를 shm 헤더로 복사. 0이면 비활성 슬롯. */
		if (lcore_offsets[i] == 0) {
			continue;                          /* [한국어] 이 lcore는 history 없음 — skip. */
		}

		if (i <= max_dedicated_cpu) {
			/* [한국어] dedicated cpu 영역이면 cpuset에 마킹되어 있어야 함 (검증). */
			assert(spdk_cpuset_get_cpu(&cpuset, i));
		}

		lcore_history = spdk_get_per_lcore_history(g_trace_file, i);
		/* [한국어] base + offset → history 포인터. */
		lcore_history->lcore = i;
		/* [한국어] 자기 lcore 번호 기록 (parser가 어느 코어 trace인지 식별). */
		lcore_history->num_entries = num_entries;
		/* [한국어] ring 크기 — get_trace_entry의 AND 마스크 (num_entries-1)에서 사용. */
	}
	g_trace_file->file_size = file_size;
	/* [한국어] 외부 도구가 mmap 시 이 값으로 전체 크기 파악. */
	g_trace_file->num_owners = TRACE_NUM_OWNERS;
	g_trace_file->owner_description_size = TRACE_OWNER_DESCRIPTION_SIZE;
	g_trace_file->owner_offset = owner_offset;
	/* [한국어] owner pool 메타 — spdk_get_trace_owner가 indexing에 사용. */

	if (trace_flags_init()) {
		/* [한국어] reg_fn 일괄 호출 + owner_id ring 초기화. 실패 시 정리 분기. */
		goto trace_init_err;
	}

	return 0;

trace_init_err:
	/* [한국어] 부분 성공 후 실패 — 정상 정리 경로. 각 자원을 역순으로 해제. */
	if (g_trace_file != MAP_FAILED) {
		/* [한국어] mmap이 성공했다면 munmap. */
		munmap(g_trace_file, file_size);
	}
	close(g_trace_fd);                                /* [한국어] shm fd 닫기. */
	g_trace_fd = -1;                                  /* [한국어] sentinel 상태로 복원. */
	shm_unlink(shm_name);                             /* [한국어] /dev/shm/<name> 파일 제거 — 잔재 방지. */
	spdk_bit_array_free(&g_ut_array);                 /* [한국어] bit_array 해제. */
	g_trace_file = NULL;
	/* [한국어] 외부 매크로 spdk_trace_tpoint_enabled가 NULL 검사로 즉시 noop 처리. */

	return 1;

}

/*
 * [한국어]
 * spdk_trace_cleanup - trace 시스템 lifecycle 종료.
 *
 * 동작:
 *   1) g_trace_file이 NULL이면 init되지 않은 상태 — 즉시 return.
 *   2) trace_flags_fini() — owner ring/spinlock 해제 (mmap 해제 전에 수행).
 *   3) "기록된 entry가 1개라도 있는가?" 검사:
 *      각 lcore의 entries[0].tsc != 0이면 record가 발생한 코어 — 디버깅 가치.
 *      이 경우 shm 파일을 unlink하지 않고 남겨 → 프로세스 crash 후에도 사후 분석 가능.
 *      모든 lcore가 entries[0].tsc == 0이면 unlink (clean state).
 *      ★ 주의: 이 검사를 munmap 전에 수행해야 함 — munmap 후엔 g_trace_file 무효.
 *   4) munmap, close, bit_array free.
 *   5) 조건에 따라 shm_unlink.
 *
 * 컨텍스트: spdk_app_stop의 종료 단계, 메인 스레드 단일 호출.
 * 호출 체인: spdk_app_stop → spdk_trace_cleanup → trace_flags_fini.
 */
void
spdk_trace_cleanup(void)
{
	bool unlink = true;                   /* [한국어] true면 shm 파일 삭제, false면 보존. */
	int i;
	struct spdk_trace_history *lcore_history;

	if (g_trace_file == NULL) {
		/* [한국어] init되지 않았거나 이미 cleanup됨 — no-op. 멱등성 보장. */
		return;
	}

	trace_flags_fini();
	/* [한국어] mmap 해제 전에 owner ring을 먼저 정리. 순서 중요 — flags_fini가
	 * g_trace_file에 접근하지 않지만 자원 ownership을 명확히 하기 위해 우선 호출. */

	/*
	 * Only unlink the shm if there were no trace_entry recorded. This ensures the file
	 * can be used after this process exits/crashes for debugging.
	 * Note that we have to calculate this value before g_trace_file gets unmapped.
	 */
	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		lcore_history = spdk_get_per_lcore_history(g_trace_file, i);
		if (lcore_history == NULL) {
			/* [한국어] 이 lcore는 trace 비활성 슬롯 — skip. */
			continue;
		}
		unlink = lcore_history->entries[0].tsc == 0;
		/* [한국어] entries[0].tsc==0 이면 "기록 없음" 추정 (init 후 zero clear 상태 그대로).
		 * record가 발생했다면 tsc는 양수 (TSC는 부팅 후 단조 증가). */
		if (!unlink) {
			/* [한국어] 한 코어라도 기록이 있으면 보존 결정 — 더 검사할 필요 없음. */
			break;
		}
	}

	munmap(g_trace_file, sizeof(struct spdk_trace_file));
	/* [한국어] 헤더 크기만 munmap한다는 점 주의 — 사실 init 시 file_size 전체를 mmap했지만,
	 * 여기서는 sizeof(spdk_trace_file)만 풀고 있다. /dev/shm 파일 자체가 reference로 살아있으면
	 * 외부 spdk_trace 도구가 여전히 동일 영역에 접근 가능 (커널이 ref count로 관리).
	 * (※ upstream 코드의 실제 해제는 file 단위 reference이므로 동작상 문제 없음.) */
	g_trace_file = NULL;
	/* [한국어] 외부 매크로 spdk_trace_tpoint_enabled의 NULL 검사로 record 즉시 noop 전환. */
	close(g_trace_fd);                            /* [한국어] fd 해제 — refcount 감소. */
	spdk_bit_array_free(&g_ut_array);             /* [한국어] user thread 비트맵 free + 포인터 NULL. */

	if (unlink) {
		/* [한국어] 기록이 전혀 없었다면 /dev/shm/<name> 파일 제거 → 디스크 자원 회수. */
		shm_unlink(g_shm_name);
	}
}

/*
 * [한국어]
 * trace_get_shm_name - g_shm_name 전역의 read-only 노출 (RPC trace_get_info용).
 *
 * @return: init 시 저장된 shm 이름 문자열 포인터 (절대 free 금지).
 *
 * trace_internal.h 선언 — lib/trace 외부 모듈은 호출하지 않음.
 * 컨텍스트: jsonrpc 서버 스레드. read-only 접근이라 lock 불필요.
 */
const char *
trace_get_shm_name(void)
{
	return g_shm_name;
	/* [한국어] init이 이미 채웠고 이후 변하지 않으므로 안전한 read. */
}
