/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * Tracepoint library
 */

/*
 * [한국어 설명] SPDK 통합 trace framework 공개 API (trace.h) — 약 506 라인
 *
 * === 파일의 역할 ===
 * SPDK가 자체적으로 사용하는 매우 가벼운 logging/tracing 인프라의 공개
 * 인터페이스를 정의한다. 일반 SPDK_*LOG가 사람이 읽는 메시지를 즉시 출력하는
 * 무거운 경로라면, trace는 hot path(NVMe submission/completion, bdev I/O 등)
 * 에서도 부담 없이 호출할 수 있도록 lockless circular buffer에 고정 크기
 * struct spdk_trace_entry만 기록하고 분석은 spdk_trace CLI(후처리)에 위임한다.
 * trace point는 24바이트 entry(tsc + tpoint_id + owner_id + size + object_id +
 * 8B 가변 인자)로 표현되며, lcore별 history 버퍼에 ring으로 누적된다.
 * shared memory(/dev/shm)에 매핑되므로 spdk_trace 같은 외부 도구가 실시간으로
 * dump 가능하다. tpoint group / owner type / object type / argument 메타데이터를
 * 함께 등록하여 후처리 단계에서 사람이 읽는 형태로 복원할 수 있게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 전 코어 모듈(lib/nvme, lib/bdev, lib/nvmf, lib/thread, lib/blob 등)이
 * 본 헤더를 #include 하여 spdk_trace_record() 매크로로 trace point를 발사한다.
 * 호출 흐름:
 *   - 등록 측: 모듈의 .c 파일에서 SPDK_TRACE_REGISTER_FN(reg_fn, name, group_id)
 *              매크로로 register_fn 노드를 .data 섹션에 정의 + constructor로
 *              g_register_fn_list에 자동 등록 → trace_init 시 reg_fn() 일괄 호출
 *              → spdk_trace_register_owner_type/object/description_ext 호출.
 *   - 기록 측: hot path → spdk_trace_record(tpoint_id, owner, size, obj, args...)
 *              → 매크로 확장 → spdk_trace_tpoint_enabled() 분기 → enabled시
 *              _spdk_trace_record() 호출 → per-lcore history.entries[next_entry++]
 *              에 entry 기록 (lockless, 자기 코어만 씀).
 *   - 소비 측: 외부 spdk_trace 바이너리 또는 lib/trace_parser → /dev/shm 매핑
 *              → spdk_trace_file 메타데이터 + per-lcore history 순회 → 사람이
 *              읽는 형식으로 출력.
 * 실행 컨텍스트: trace_record는 어떤 SPDK thread/reactor에서도 호출 가능하지만
 *                per-lcore 버퍼에 쓰므로 thread affinity가 lockless를 보장.
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/stdinc.h(uint*_t/UCHAR_MAX/UINT64_MAX), spdk/assert.h
 *             (SPDK_STATIC_ASSERT — entry 크기 ABI 검증).
 * 의존(구현): lib/trace/trace.c, lib/trace/trace_internal.h, lib/trace/trace_rpc.c.
 *             trace_init은 shm_open + ftruncate + mmap으로 /dev/shm/_trace.<name>
 *             공유 영역을 만들고 g_trace_file로 노출. spdk_get_ticks() (env)와
 *             결합하여 TSC 타임스탬프를 entry에 기록.
 * 의존하는 모듈: 거의 모든 SPDK 라이브러리 (lib/nvme, lib/bdev, lib/nvmf,
 *               lib/thread, lib/blob, lib/scsi, lib/iscsi 등).
 *               app/spdk_tgt 등은 시작 시 spdk_trace_init(shm_name, ...) 호출.
 * 데이터 흐름: 호출 측 → per-lcore ring → /dev/shm 매핑 → 외부 도구가 읽음.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_trace_entry: 단일 trace 기록(24바이트 고정).
 *   - struct spdk_trace_entry_buffer: 가변 길이 string 인자를 담을 때 ring의
 *     다음 슬롯을 이어 사용하기 위한 alias 형(같은 24바이트 영역 재해석).
 *   - struct spdk_trace_owner_type / spdk_trace_owner: owner 분류·인스턴스.
 *   - struct spdk_trace_object: object(예: NVMe IO, bdev_io)의 타입 메타.
 *   - struct spdk_trace_argument / spdk_trace_tpoint: tpoint 메타(인자 정의·관계).
 *   - struct spdk_trace_history: per-lcore ring buffer + tpoint hit count.
 *   - struct spdk_trace_file: shm 파일 헤더(메타데이터 + lcore history offset).
 *   - struct spdk_trace_register_fn: SPDK_TRACE_REGISTER_FN이 만드는 등록 노드.
 *   - 매크로:
 *       SPDK_TPOINT_ID(group, tpoint) — 두 ID를 16-bit tpoint_id로 결합
 *       spdk_trace_tpoint_enabled(id) — fast-path 비트마스크 검사
 *       spdk_trace_record(...) — hot path 기록 진입점
 *       spdk_trace_record_tsc(...) — TSC 직접 지정판
 *       SPDK_TRACE_REGISTER_FN(...) — 등록 함수 자동 삽입
 *   - 함수:
 *       spdk_trace_init / cleanup / register_user_thread — lifecycle
 *       spdk_trace_set/clear/get_tpoints / _group_mask — 동적 enable
 *       spdk_trace_register_owner_type / register_owner / register_object
 *       spdk_trace_register_description / _ext — tpoint 메타 등록
 *       spdk_trace_tpoint_register_relation — tpoint↔object 매핑
 *       spdk_trace_enable_tpoint_group / disable / mask_usage / create_mask
 */

#ifndef _SPDK_TRACE_H_   /* [한국어] include 가드 — 다중 포함 시 재정의 방지 */
#define _SPDK_TRACE_H_

#include "spdk/stdinc.h" /* [한국어] 표준 타입 묶음 (uint*_t, UCHAR_MAX, UINT64_MAX, FILE 등) */
#include "spdk/assert.h" /* [한국어] SPDK_STATIC_ASSERT — trace_entry/trace_entry_buffer 크기 ABI 검증용 */

#ifdef __cplusplus
extern "C" {             /* [한국어] C++에서도 본 헤더를 사용할 수 있도록 C 링키지 강제 */
#endif

/* [한국어] /dev/shm에 매핑되는 trace 공유 영역의 이름 prefix.
 * 실제 이름은 "_trace." + 사용자가 trace_init에 넘긴 shm_name.
 * 외부 spdk_trace 도구는 이 prefix로 파일을 열어 g_trace_file을 매핑한다. */
#define SPDK_TRACE_SHM_NAME_BASE "_trace."

/* [한국어] per-lcore ring buffer의 기본 entry 수 (32 * 1024 = 32K개).
 * 32K * sizeof(spdk_trace_entry=24B) = 768KB per lcore.
 * spdk_trace_init(shm_name, num_entries, ...)에서 num_entries=0이면 이 값으로 대체. */
#define SPDK_DEFAULT_NUM_TRACE_ENTRIES	 (32 * 1024)

/* [한국어] 단일 trace 기록 단위 — 정확히 24바이트.
 * lcore별 ring buffer에 next_entry 인덱스로 순차 기록되며,
 * fast path에서도 한 번의 store로 끝낼 수 있도록 고정 크기로 설계됨. */
struct spdk_trace_entry {
	uint64_t	tsc;
	/* [한국어] 이 entry가 기록된 시점의 TSC(Time Stamp Counter) 값.
	 * 설정자: _spdk_trace_record() — spdk_get_ticks() 또는 호출자가 넘긴 tsc.
	 * 읽는 자: spdk_trace_parser가 tsc_rate로 나누어 사람이 읽는 시간으로 변환.
	 * 값 범위: TSC 단조 증가, 64비트 wrap-around 무시 가능 (수십 년). */

	uint16_t	tpoint_id;
	/* [한국어] 이 entry가 속한 tracepoint의 ID (group_id<<6 | tpoint_in_group).
	 * 설정자: SPDK_TPOINT_ID(group, idx) 매크로로 모듈이 미리 #define.
	 * 읽는 자: parser가 spdk_trace_tpoint[tpoint_id] 메타로 인자 해석.
	 * 값 범위: 0 ~ SPDK_TRACE_MAX_TPOINT_ID-1 (=1279, 20*64). */

	uint16_t	owner_id;
	/* [한국어] 이 entry를 발행한 owner의 instance ID (예: 특정 NVMe qpair).
	 * 설정자: spdk_trace_register_owner()가 반환한 ID를 호출자가 보관 후 전달.
	 * 읽는 자: parser가 spdk_trace_owner[owner_id]로 description 조회.
	 * 0이면 owner 미지정 (OWNER_TYPE_NONE). */

	uint32_t	size;
	/* [한국어] tpoint 정의에 따라 의미가 달라지는 페이로드(예: I/O 전송 바이트 수).
	 * 일반적으로 I/O size를 기록 — bdev/NVMe trace에서 빈번히 사용. */

	uint64_t	object_id;
	/* [한국어] 이 entry가 추적하는 object의 식별자 (예: bdev_io 포인터, IO 시퀀스 번호).
	 * 설정자: 호출자가 spdk_trace_record()에 전달.
	 * 읽는 자: parser가 같은 object_id를 가진 entry들을 묶어 lifecycle을 시각화.
	 * new_object=1인 tpoint가 호출되면 parser가 이 ID로 새 object 인스턴스 생성. */

	uint8_t		args[8];
	/* [한국어] tpoint 정의된 인자들의 인라인 저장 영역 (8바이트).
	 * 8B를 초과하는 인자(예: 문자열, 4개 이상의 uint64)는 다음 ring 슬롯을
	 * spdk_trace_entry_buffer로 재해석하여 이어 저장한다 (entry chaining).
	 * 설정자: _spdk_trace_record()의 가변 인자 unpacking 로직.
	 * 읽는 자: parser가 spdk_trace_argument 메타로 타입/오프셋 해석. */
};

/* [한국어] entry chaining용 alias 구조체 — spdk_trace_entry와 같은 24B 영역을
 * 다른 시각으로 본다. 첫 entry 이후 가변 인자(특히 string)가 8B를 초과하면
 * 다음 ring 슬롯을 이 형식으로 사용:
 *   - tsc, tpoint_id는 헤더와 같은 위치 (디스플레이/검색 시 일관성)
 *   - data[22]는 인자 데이터 연속 저장
 * static_assert로 sizeof(_buffer) == sizeof(_entry) 검증. */
struct spdk_trace_entry_buffer {
	uint64_t	tsc;
	/* [한국어] 부모 entry와 동일 TSC 복제 — 디버깅·검증 용도. */

	uint16_t	tpoint_id;
	/* [한국어] 부모 entry와 동일 tpoint_id 복제 — 정렬/검증 용도. */

	uint8_t		data[22];
	/* [한국어] 부모 entry의 args 8B를 넘는 추가 페이로드 (대개 string의 잔여 바이트).
	 * 22B = 24 - 2(tpoint_id) (tsc는 부모와 같은 8B). */
};

/* [한국어] 빌드 시 두 구조체 크기가 동일함을 강제 — entry chaining의 핵심 전제.
 * 어긋나면 ring 슬롯 alias가 깨져 ring 순회가 불가능. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_trace_entry_buffer) == sizeof(struct spdk_trace_entry),
		   "Invalid size of trace entry buffer");

/* [한국어] owner type ID의 최댓값 + 1.
 * owner_type 필드가 uint8_t이므로 256 (UCHAR_MAX+1).
 * 만약 owner_type 폭이 변하면 이 매크로도 함께 변경 필요 (코멘트가 경고). */
#define SPDK_TRACE_MAX_OWNER_TYPE (UCHAR_MAX + 1)

/* [한국어] owner type 정의 — 어떤 종류의 entity가 trace를 발행하는지 분류.
 * spdk_trace_register_owner_type(type, prefix)로 등록.
 * 예: type=BDEV, prefix='b' → parser 출력에서 'b1', 'b2' 형태로 인스턴스 표시. */
struct spdk_trace_owner_type {
	uint8_t	type;
	/* [한국어] owner type 정수 ID (0~255). 0은 OWNER_TYPE_NONE으로 예약. */

	char	id_prefix;
	/* [한국어] parser 출력에 사용할 한 글자 prefix (예: 'q' for qpair). */
};

/* [한국어] owner의 실제 인스턴스 메타데이터 — 각 owner_id마다 1개.
 * 가변 길이 description을 packed로 이어붙여 저장(spdk_trace_file의
 * owner_offset+owner_id*owner_size로 indexing). */
struct spdk_trace_owner {
	uint64_t	tsc;
	/* [한국어] 이 owner가 등록된 시점의 TSC. parser가 lifecycle 시작점으로 사용. */

	uint8_t		type;
	/* [한국어] spdk_trace_owner_type.type과 매칭되는 분류값. */

	char		description[];
	/* [한국어] 사용자 지정 설명 문자열 (예: "qpair_id=3 nvme0").
	 * Flexible array — owner_description_size만큼 메모리 확보됨. */
} __attribute__((packed));
/* [한국어] packed로 패딩 0 — 같은 owner_size로 균일 indexing 가능. */

/* [한국어] object type ID의 최댓값 + 1 (uint8_t 폭 한계). */
#define SPDK_TRACE_MAX_OBJECT (UCHAR_MAX + 1)

/* [한국어] object type 정의 — trace가 추적할 "객체"의 종류 (예: bdev_io, NVMe IO, SCSI task).
 * spdk_trace_register_object(type, prefix)로 등록.
 * tpoint 정의에서 object_type을 지정하면 parser가 같은 object_id를 가진 entry들을
 * 하나의 lifecycle로 묶어 표시. */
struct spdk_trace_object {
	uint8_t	type;
	/* [한국어] object type 정수 ID (0~255). 0은 OBJECT_NONE. */

	char	id_prefix;
	/* [한국어] parser 출력에 사용할 한 글자 prefix (예: 'i' for bdev_io). */
};

/* [한국어] === trace 분류 상수 ===
 * thread 이름 최대 길이 — spdk_trace_file.tname[lcore]에 기록.
 * SPDK_TRACE_THREAD_NAME_LEN=16: ARM/x86 모두 cache-line 친화적. */
#define	SPDK_TRACE_THREAD_NAME_LEN 16

/* [한국어] tpoint group 최대 수 — group_id가 0~19까지 가능.
 * 각 group은 자체 64-bit tpoint mask를 가짐 → group당 최대 64개 tpoint. */
#define SPDK_TRACE_MAX_GROUP_ID  20

/* [한국어] 전역 tpoint_id의 최댓값 = 20 * 64 = 1280.
 * 16-bit tpoint_id가 이 범위를 넘지 않도록 매크로/assert로 검증. */
#define SPDK_TRACE_MAX_TPOINT_ID (SPDK_TRACE_MAX_GROUP_ID * 64)

/* [한국어] (group, tpoint) 쌍을 단일 16-bit tpoint_id로 패킹.
 * 상위 비트 = group_id, 하위 6비트 = group 내 tpoint 인덱스(0~63).
 * 예: group=2, tpoint=5 → 2*64+5 = 133.
 * spdk_trace_tpoint_enabled()가 (id>>6)으로 group, (id & 0x3F)로 비트 위치 추출. */
#define SPDK_TPOINT_ID(group, tpoint)	((group * 64) + tpoint)

/* [한국어] === tpoint 인자 타입 enum (struct spdk_trace_argument.type) === */
#define SPDK_TRACE_ARG_TYPE_INT 0  /* [한국어] uint64_t 정수 — 그대로 args[]에 인라인. */
#define SPDK_TRACE_ARG_TYPE_PTR 1  /* [한국어] 포인터 — uint64로 캐스팅하여 저장 (값으로). */
#define SPDK_TRACE_ARG_TYPE_STR 2  /* [한국어] 문자열 — entry chaining으로 다음 슬롯에 연속 저장. */

/* [한국어] tpoint 하나가 받을 수 있는 최대 인자 수. 8개 = args[8]에 패킹 가능한 한계. */
#define SPDK_TRACE_MAX_ARGS_COUNT 8

/* [한국어] tpoint↔object 관계의 최대 연결 수.
 * 한 tpoint가 여러 object와 묶일 수 있도록 16개까지 등록 가능
 * (spdk_trace_tpoint_register_relation 참고). */
#define SPDK_TRACE_MAX_RELATIONS 16

/* [한국어] 단일 tpoint 인자 메타 정의 — register 시점에 한 번 채움. */
struct spdk_trace_argument {
	char	name[14];
	/* [한국어] 인자 이름 (parser 출력에 사용). 14B는 cache-line 정렬용. */

	uint8_t	type;
	/* [한국어] SPDK_TRACE_ARG_TYPE_INT/PTR/STR. parser가 args[] 해석에 사용. */

	uint8_t	size;
	/* [한국어] 인자 바이트 크기. STR의 경우 최대 길이.
	 * args[8] + 다음 chained buffer까지 unpacking 시 오프셋 계산에 사용. */
};

/* [한국어] tpoint 메타 정의 — register_description/_ext에서 등록되며,
 * spdk_trace_file.tpoint[tpoint_id]에 저장됨. parser가 entry 해석 시 참조. */
struct spdk_trace_tpoint {
	char				name[24];
	/* [한국어] tpoint의 사람이 읽는 이름 (예: "BDEV_IO_START"). 24B 고정. */

	uint16_t			tpoint_id;
	/* [한국어] SPDK_TPOINT_ID로 만든 전역 ID. */

	uint8_t				owner_type;
	/* [한국어] 이 tpoint를 발행하는 owner의 종류 (spdk_trace_owner_type.type). */

	uint8_t				object_type;
	/* [한국어] 이 tpoint가 다루는 object의 종류 (spdk_trace_object.type).
	 * OBJECT_NONE이면 object 추적 비활성. */

	uint8_t				new_object;
	/* [한국어] 1이면 이 tpoint가 새 object의 lifecycle 시작점.
	 * parser가 object_id를 새 인스턴스로 등록 (예: BDEV_IO_START). */

	uint8_t				num_args;
	/* [한국어] 실제 사용 중인 args[] 항목 수 (0 ~ SPDK_TRACE_MAX_ARGS_COUNT). */

	struct spdk_trace_argument	args[SPDK_TRACE_MAX_ARGS_COUNT];
	/* [한국어] 인자 메타 배열 (이름/타입/크기). num_args 이전까지만 유효. */

	/** Relations between tracepoint and trace object */
	struct {
		uint8_t object_type;
		/* [한국어] 이 인자가 가리키는 외부 object의 종류 (cross-reference). */

		uint8_t arg_index;
		/* [한국어] 위 object_type에 매핑되는 인자의 args[] 인덱스 (0~7). */
	} related_objects[SPDK_TRACE_MAX_RELATIONS];
	/* [한국어] tpoint↔다른 object들의 cross-reference 매핑.
	 * 예: BDEV_IO_DONE이 NVMe_IO와 연관 → parser가 두 lifecycle 합성 표시.
	 * spdk_trace_tpoint_register_relation()으로 등록. */
};

/* [한국어] per-lcore ring buffer + 메타데이터.
 * 각 lcore는 자기 history에만 쓰므로 lockless가 가능. parser는 모든 lcore의
 * history를 tsc 기준으로 merge-sort하여 시간 순서로 출력. */
struct spdk_trace_history {
	/** Logical core number associated with this structure instance. */
	int				lcore;
	/* [한국어] 이 history를 소유한 logical core 번호.
	 * 설정자: spdk_trace_init()이 lcore마다 1개씩 할당하며 채움.
	 * 읽는 자: parser가 어느 코어에서 발생한 trace인지 식별. */

	/** Number of trace_entries contained in each trace_history. */
	uint64_t			num_entries;
	/* [한국어] entries[] ring의 슬롯 수 (보통 SPDK_DEFAULT_NUM_TRACE_ENTRIES=32K).
	 * next_entry % num_entries로 ring 순환. */

	/**
	 * Running count of number of occurrences of each tracepoint on this
	 *  lcore.  Debug tools can use this to easily count tracepoints such as
	 *  number of SCSI tasks completed or PDUs read.
	 */
	uint64_t			tpoint_count[SPDK_TRACE_MAX_TPOINT_ID];
	/* [한국어] tpoint_id별 누적 발사 카운터 (1280개 슬롯).
	 * 설정자: _spdk_trace_record가 매 호출마다 ++.
	 * 읽는 자: spdk_trace --stats가 IO 처리량 등 통계 표시. */

	/** Index to next spdk_trace_entry to fill. */
	uint64_t			next_entry;
	/* [한국어] 다음 기록 위치(monotonic 카운터). 실제 슬롯은 (next_entry % num_entries).
	 * 자기 lcore에서만 ++하므로 lockless.
	 * 단조 증가하므로 wrap 정보가 보존되어 parser가 ring 순서 복원 가능. */

	/**
	 * Circular buffer of spdk_trace_entry structures for tracing
	 *  tpoints on this core.  Debug tool spdk_trace reads this
	 *  buffer from shared memory to post-process the tpoint entries and
	 *  display in a human-readable format.
	 */
	struct spdk_trace_entry		entries[0];
	/* [한국어] flexible array — num_entries 만큼의 ring buffer.
	 * spdk_get_trace_history_size()가 sizeof(history) + num_entries*entry로 계산. */
};

/* [한국어] 시스템 전체 lcore 수 한계. tname/lcore_history_offsets[] 배열 크기 결정.
 * SPDK는 1024 코어까지 지원 (DPDK 한계와 정합). */
#define SPDK_TRACE_MAX_LCORE		1024

/* [한국어] /dev/shm에 매핑되는 trace 공유 영역의 헤더 구조.
 * 외부 spdk_trace 도구가 mmap한 후 이 구조부터 읽어 메타데이터를 복원하고,
 * lcore_history_offsets[]를 따라 각 lcore의 ring으로 점프한다.
 * 따라서 ABI 안정성이 매우 중요 (필드 추가 시 끝에 append). */
struct spdk_trace_file {
	uint64_t			file_size;
	/* [한국어] 전체 shm 영역 크기(바이트). spdk_trace_init()이 계산하여 채움.
	 * 외부 도구가 mmap 시 length 인자로 사용. */

	uint64_t			tsc_rate;
	/* [한국어] 현재 시스템의 TSC 주파수(Hz). spdk_get_ticks_hz()의 값.
	 * parser가 entry.tsc / tsc_rate로 초 단위 시간 환산. */

	uint64_t			tpoint_mask[SPDK_TRACE_MAX_GROUP_ID];
	/* [한국어] group별 활성 tpoint 비트마스크.
	 * 설정자: spdk_trace_set/clear_tpoints, set/clear_tpoint_group_mask가 갱신.
	 * 읽는 자: spdk_trace_tpoint_enabled() — hot path fast check.
	 * 비트 i가 1이면 group 내 tpoint i가 활성. */

	char				tname[SPDK_TRACE_MAX_LCORE][SPDK_TRACE_THREAD_NAME_LEN];
	/* [한국어] lcore별 thread 이름 (보통 "reactor_<lcore>"). parser가 표시용으로 사용. */

	struct spdk_trace_owner_type	owner_type[SPDK_TRACE_MAX_OWNER_TYPE];
	/* [한국어] 등록된 owner type 메타 (256개 슬롯). type 필드를 인덱스로 사용. */

	struct spdk_trace_object	object[UCHAR_MAX + 1];
	/* [한국어] 등록된 object type 메타 (256개 슬롯). */

	struct spdk_trace_tpoint	tpoint[SPDK_TRACE_MAX_TPOINT_ID];
	/* [한국어] 등록된 tpoint 메타 (1280개 슬롯). tpoint_id를 인덱스로 직접 접근. */

	uint16_t			num_owners;
	/* [한국어] 현재 등록된 owner 인스턴스 수. 다음 owner_id 할당에도 사용. */

	uint16_t			owner_description_size;
	/* [한국어] 각 spdk_trace_owner의 description[] 영역 크기.
	 * owner_size = sizeof(spdk_trace_owner) + 이 값으로 균일 indexing. */

	uint8_t				reserved[4];
	/* [한국어] 8B 정렬 패딩 (위 두 uint16 + 4B = 8B). 향후 필드 확장 여지. */

	/** Offset of each trace_history from the beginning of this data structure. */
	uint64_t			lcore_history_offsets[SPDK_TRACE_MAX_LCORE];
	/* [한국어] lcore별 spdk_trace_history 시작 오프셋 (file 시작 기준).
	 * 0이면 해당 lcore 비활성.
	 * 읽는 자: spdk_get_per_lcore_history()가 (char*)file + offset로 점프. */

	/** Offset of beginning of struct spdk_trace_owner data. */
	uint64_t			owner_offset;
	/* [한국어] owner 인스턴스 배열의 시작 오프셋.
	 * spdk_get_trace_owner()가 owner_offset + owner_id*owner_size로 indexing. */

	/** Variable sized data sections are at the end of this data structure,
	 *  referenced by offsets defined in this structure.
	 */
	uint8_t	data[0];
	/* [한국어] flexible array — 이후 영역에 lcore history들과 owner 인스턴스들이
	 * 위 offset 필드로 indexing되어 배치된다. */
};
/* [한국어] 전역 trace 영역 포인터 — lib/trace/trace.c에서 정의.
 * NULL이면 trace 비활성 (init되지 않음). spdk_trace_tpoint_enabled()의 첫 가드. */
extern struct spdk_trace_file *g_trace_file;

/*
 * [한국어]
 * spdk_get_trace_history_size - 주어진 entry 수를 가진 history 구조의 총 바이트 크기 계산.
 *
 * @num_entries: ring buffer 슬롯 수.
 * @return: sizeof(struct spdk_trace_history) + num_entries * sizeof(spdk_trace_entry).
 *
 * trace_init이 lcore당 mmap 영역 크기를 계산할 때 사용.
 * 헤더 + 가변 길이 entries[] 합산.
 */
static inline uint64_t
spdk_get_trace_history_size(uint64_t num_entries)
{
	return sizeof(struct spdk_trace_history) + num_entries * sizeof(struct spdk_trace_entry);
	/* [한국어] flexible array entries[]의 실제 바이트 = num_entries * 24B. */
}

/*
 * [한국어]
 * spdk_get_trace_file_size - shm 영역 전체 크기를 헤더의 file_size 필드에서 읽어 반환.
 *
 * @trace_file: mmap 또는 g_trace_file 포인터.
 * @return: file_size 그대로.
 *
 * 외부 도구가 mmap 후 truncate/copy 시 길이를 알기 위해 호출.
 */
static inline uint64_t
spdk_get_trace_file_size(struct spdk_trace_file *trace_file)
{
	return trace_file->file_size;
	/* [한국어] init 시 계산되어 한 번만 채워짐 → 읽기 전용. */
}

/*
 * [한국어]
 * spdk_get_per_lcore_history - 특정 lcore의 history 구조 포인터 반환.
 *
 * @trace_file: 매핑된 trace_file 헤더.
 * @lcore: 조회할 logical core 번호.
 * @return: 해당 lcore의 spdk_trace_history* / 비활성·범위 초과 시 NULL.
 *
 * lcore_history_offsets[lcore]가 0이면 그 코어는 trace 비활성 (할당되지 않음).
 * parser와 _spdk_trace_record 양쪽에서 자기 history 위치를 찾는 데 사용.
 */
static inline struct spdk_trace_history *
spdk_get_per_lcore_history(struct spdk_trace_file *trace_file, unsigned lcore)
{
	uint64_t lcore_history_offset;
	/* [한국어] 임시 변수 — 캐시된 offset 값 보관. */

	if (lcore >= SPDK_TRACE_MAX_LCORE) {
		/* [한국어] 배열 인덱스 범위 검사 (1024 미만). */
		return NULL;
	}

	lcore_history_offset = trace_file->lcore_history_offsets[lcore];
	/* [한국어] 미리 계산된 offset 조회 — init 시점에만 채워짐. */
	if (lcore_history_offset == 0) {
		/* [한국어] 0이면 이 lcore는 trace 비활성 (할당되지 않은 코어). */
		return NULL;
	}

	return (struct spdk_trace_history *)(((char *)trace_file) + lcore_history_offset);
	/* [한국어] base 주소 + offset → history 포인터로 캐스팅. */
}

/*
 * [한국어]
 * spdk_get_trace_owner - owner_id로 owner 인스턴스 메타 조회.
 *
 * @trace_file: 매핑된 trace_file.
 * @owner_id: spdk_trace_register_owner()가 할당한 ID.
 * @return: 해당 owner의 spdk_trace_owner* / out-of-range시 NULL.
 *
 * description이 가변 길이이므로 owner_size = base + description_size로 indexing.
 * parser가 entry.owner_id를 사람이 읽는 description으로 변환할 때 사용.
 */
static inline struct spdk_trace_owner *
spdk_get_trace_owner(const struct spdk_trace_file *trace_file, uint16_t owner_id)
{
	uint64_t owner_size;
	/* [한국어] 단일 owner 슬롯의 실제 바이트 크기 (헤더 + description). */

	if (owner_id >= trace_file->num_owners) {
		/* [한국어] 등록된 owner 수를 초과하면 NULL. */
		return NULL;
	}

	owner_size = sizeof(struct spdk_trace_owner) + trace_file->owner_description_size;
	/* [한국어] init 시 description_size로 균일 슬롯 크기 결정 (packed → 패딩 없음). */
	return (struct spdk_trace_owner *)
	       (((char *)trace_file) + trace_file->owner_offset + owner_id * owner_size);
	/* [한국어] base + 시작 오프셋 + owner_id*size로 직접 indexing.
	 * owner_offset은 owner 영역의 시작 오프셋(file 기준). */
}

/*
 * [한국어]
 * _spdk_trace_record - trace entry를 ring에 실제 기록하는 내부 핵심 함수.
 *
 * @tsc:        타임스탬프(0이면 호출자가 spdk_get_ticks() 호출 책임).
 * @tpoint_id:  SPDK_TPOINT_ID(group, idx)로 만든 ID.
 * @owner_id:   owner 인스턴스 ID (0 = none).
 * @size:       페이로드 크기 (보통 I/O 바이트).
 * @object_id:  추적 object 식별자 (포인터/시퀀스 번호).
 * @num_args:   가변 인자 개수.
 * @...:        실제 인자 값들 (uint64/포인터/문자열).
 *
 * lib/trace/trace.c가 정의. 호출자는 보통 직접 부르지 않고 spdk_trace_record()
 * 매크로를 통해 enabled 검사 후 진입한다.
 * 컨텍스트: 자기 lcore의 history.entries[next_entry++]에만 쓰므로 lockless.
 *           cross-thread 호출 금지(자기 코어에서만 발사 보장).
 * 호출 체인: hot path → spdk_trace_record(...) → _spdk_trace_record_tsc(...) →
 *             [enabled 검사 통과시] → _spdk_trace_record(...) → entry 기록.
 */
void _spdk_trace_record(uint64_t tsc, uint16_t tpoint_id, uint16_t owner_id,
			uint32_t size, uint64_t object_id, int num_args, ...);

/* [한국어] hot path fast-check 매크로 — tpoint가 활성인지 한 번에 판단.
 * spdk_unlikely로 분기 예측 힌트(대부분 비활성 가정 → fall-through 최적화).
 * 동작:
 *   1) g_trace_file != NULL (trace_init되었는가?)
 *   2) tpoint_mask[group_id]의 (tpoint_in_group) 비트가 1인가?
 *      - tpoint_id >> 6 = group_id (상위 비트)
 *      - tpoint_id & 0x3F = group 내 인덱스 (하위 6비트)
 * 두 조건 모두 참이면 trace 기록 진입. 아니면 단 1개 비교 후 즉시 skip → 거의 0 cost.
 */
#define spdk_trace_tpoint_enabled(tpoint_id)	\
	spdk_unlikely((g_trace_file != NULL  && \
	((1ULL << (tpoint_id & 0x3F)) &	g_trace_file->tpoint_mask[tpoint_id >> 6])))

/* [한국어] 위 enabled 검사 + assert + 실제 record 호출을 묶은 내부 매크로.
 * 동작:
 *   1) tpoint_id가 MAX_TPOINT_ID 이내인지 assert (디버그 빌드 한정).
 *   2) enabled 미통과시 do-while break → noop.
 *   3) 통과시 _spdk_trace_record() 호출하여 실제 entry 기록.
 * do { } while (0) 래핑은 if/else 내부에서 안전하게 사용되도록 함.
 */
#define _spdk_trace_record_tsc(tsc, tpoint_id, owner_id, size, object_id, num_args, ...)	\
	do {											\
		assert(tpoint_id < SPDK_TRACE_MAX_TPOINT_ID);					\
		if (!spdk_trace_tpoint_enabled(tpoint_id)) {					\
			break;									\
		}										\
		_spdk_trace_record(tsc, tpoint_id, owner_id, size, object_id,			\
				   num_args, ## __VA_ARGS__);					\
	} while (0)

/* [한국어] === 가변 인자 개수 카운트 매크로 (preprocessor 트릭) ===
 * __VA_ARGS__의 인자 수를 컴파일 타임에 0~8 범위 정수로 변환.
 * 동작: 인자 + 8,7,6,5,4,3,2,1,0을 펼친 뒤 9번째 위치(count)를 반환.
 *   spdk_trace_num_args(a,b,c) → __spdk_trace_num_args(,a,b,c,8,7,6,5,4,3,2,1,0)
 *   → count 위치는 3 (a,b,c가 8,7,6 자리를 밀어내고 5가 count).
 * spdk_trace_record가 num_args를 자동 추출하는 데 사용.
 */
#define spdk_trace_num_args(...) _spdk_trace_num_args(, ## __VA_ARGS__)
#define _spdk_trace_num_args(...) __spdk_trace_num_args(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define __spdk_trace_num_args(v, a1, a2, a3, a4, a5, a6, a7, a8, count, ...) count

/**
 * Record the current trace state for tracing tpoints. Debug tool can read the
 * information from shared memory to post-process the tpoint entries and display
 * in a human-readable format.
 *
 * \param tsc Current tsc.
 * \param tpoint_id Tracepoint id to record.
 * \param owner_id Owner id to record.
 * \param size Size to record.
 * \param object_id Object id to record.
 * \param ... Extra tracepoint arguments. The number, types, and order of the arguments
 *	      must match the definition of the tracepoint.
 */
/*
 * [한국어]
 * spdk_trace_record_tsc - 호출자가 직접 TSC를 지정하는 trace 기록 매크로.
 *
 * @tsc: 호출자가 미리 측정한 TSC (예: hot path에서 한 번 읽고 여러 trace에 재사용).
 * @tpoint_id/@owner_id/@size/@object_id: spdk_trace_entry의 동명 필드와 동일.
 * @...: tpoint 정의된 가변 인자.
 *
 * num_args를 spdk_trace_num_args()로 자동 카운트하여 _spdk_trace_record_tsc로 전달.
 * 일반 spdk_trace_record가 매번 spdk_get_ticks() 호출하는 것을 피하고 싶을 때 사용.
 */
#define spdk_trace_record_tsc(tsc, tpoint_id, owner_id, size, object_id, ...)	\
	_spdk_trace_record_tsc(tsc, tpoint_id, owner_id, size, object_id,	\
			       spdk_trace_num_args(__VA_ARGS__), ## __VA_ARGS__)

/**
 * Record the current trace state for tracing tpoints. Debug tool can read the
 * information from shared memory to post-process the tpoint entries and display
 * in a human-readable format. This macro will call spdk_get_ticks() to get
 * the current tsc to save in the tracepoint.
 *
 * \param tpoint_id Tracepoint id to record.
 * \param owner_id Owner id to record.
 * \param size Size to record.
 * \param object_id Object id to record.
 * \param ... Extra tracepoint arguments. The number, types, and order of the arguments
 *	      must match the definition of the tracepoint.
 */
/*
 * [한국어]
 * spdk_trace_record - 가장 흔히 쓰이는 trace 기록 진입점.
 *
 * tsc=0을 넘기면 _spdk_trace_record가 내부에서 spdk_get_ticks()를 호출.
 * hot path에서 별도 TSC 측정 없이 한 줄로 trace 발사 가능.
 *
 *   사용 예:
 *     spdk_trace_record(TRACE_BDEV_IO_START, 0, len, (uintptr_t)bdev_io, op);
 *
 * 호출 체인: 호출 측 .c → spdk_trace_record → spdk_trace_record_tsc(0, ...)
 *             → _spdk_trace_record_tsc → enabled 검사 → _spdk_trace_record.
 */
#define spdk_trace_record(tpoint_id, owner_id, size, object_id, ...) \
	spdk_trace_record_tsc(0, tpoint_id, owner_id, size, object_id, ## __VA_ARGS__)

/**
 * Get the current tpoint mask of the given tpoint group.
 *
 * \param group_id Tpoint group id associated with the tpoint mask.
 *
 * \return current tpoint mask.
 */
/*
 * [한국어]
 * spdk_trace_get_tpoint_mask - 특정 group의 64-bit tpoint 활성 마스크 조회.
 *
 * @group_id: 0 ~ SPDK_TRACE_MAX_GROUP_ID-1.
 * @return: 비트마스크 (비트 i = group 내 tpoint i 활성 여부).
 *
 * RPC trace_get_tpoint_group_mask 등이 운영 중 상태 조회에 사용.
 */
uint64_t spdk_trace_get_tpoint_mask(uint32_t group_id);

/**
 * Add the specified tpoints to the current tpoint mask for the given tpoint group.
 *
 * \param group_id Tpoint group id associated with the tpoint mask.
 * \param tpoint_mask Tpoint mask which indicates which tpoints to add to the
 * current tpoint mask.
 */
/*
 * [한국어]
 * spdk_trace_set_tpoints - 특정 group에 일부 tpoint 활성화 (OR 연산).
 *
 * @group_id: 대상 group ID.
 * @tpoint_mask: 활성화할 비트들 (기존 마스크와 OR).
 *
 * 운영 중 동적으로 trace를 켜는 인터페이스. 끄려면 clear_tpoints 사용.
 */
void spdk_trace_set_tpoints(uint32_t group_id, uint64_t tpoint_mask);

/**
 * Clear the specified tpoints from the current tpoint mask for the given tpoint group.
 *
 * \param group_id Tpoint group id associated with the tpoint mask.
 * \param tpoint_mask Tpoint mask which indicates which tpoints to clear from
 * the current tpoint mask.
 */
/*
 * [한국어]
 * spdk_trace_clear_tpoints - 특정 group의 일부 tpoint 비활성화 (AND NOT).
 *
 * @group_id: 대상 group ID.
 * @tpoint_mask: 비활성화할 비트들.
 */
void spdk_trace_clear_tpoints(uint32_t group_id, uint64_t tpoint_mask);

/**
 * Get a mask of all tracepoint groups which have at least one tracepoint enabled.
 *
 * \return a mask of all tracepoint groups.
 */
/*
 * [한국어]
 * spdk_trace_get_tpoint_group_mask - 활성 tpoint가 1개 이상인 group들의 비트맵 반환.
 *
 * @return: 비트 i가 1이면 group i에 enabled tpoint가 있음.
 *
 * RPC trace_get_info 등의 요약 정보용.
 */
uint64_t spdk_trace_get_tpoint_group_mask(void);

/**
 * For each tpoint group specified in the group mask, enable all of its tpoints.
 *
 * \param tpoint_group_mask Tpoint group mask that indicates which tpoints to enable.
 */
/*
 * [한국어]
 * spdk_trace_set_tpoint_group_mask - 지정 group(들)의 모든 tpoint 활성화.
 *
 * @tpoint_group_mask: 활성화할 group들의 비트맵.
 *
 * 비트 i가 1이면 group i의 64개 tpoint를 모두 켬.
 * CLI 옵션 -e bdev,nvme 같은 사용자 지정에서 호출.
 */
void spdk_trace_set_tpoint_group_mask(uint64_t tpoint_group_mask);

/**
 * For each tpoint group specified in the group mask, disable all of its tpoints.
 *
 * \param tpoint_group_mask Tpoint group mask that indicates which tpoints to disable.
 */
/*
 * [한국어]
 * spdk_trace_clear_tpoint_group_mask - 지정 group(들)의 모든 tpoint 비활성화.
 */
void spdk_trace_clear_tpoint_group_mask(uint64_t tpoint_group_mask);

/**
 * Initialize the trace environment. Debug tool can read the information from
 * the given shared memory to post-process the tpoint entries and display in a
 * human-readable format.
 *
 * \param shm_name Name of shared memory.
 * \param num_entries Number of trace entries per lcore.
 * \param num_threads Number of user created threads.
 * \return 0 on success, else non-zero indicates a failure.
 */
/*
 * [한국어]
 * spdk_trace_init - trace 시스템 lifecycle 시작.
 *
 * @shm_name:    /dev/shm/_trace.<shm_name> 형태로 만들어질 shm 이름.
 * @num_entries: per-lcore ring buffer 슬롯 수 (0이면 기본 32K).
 * @num_threads: 사용자 추가 스레드 수 (reactor 외 스레드를 추적할 때).
 * @return: 0 성공 / 음수 errno 실패 (-ENOMEM, shm_open 실패 등).
 *
 * 동작:
 *   1) shm_open(O_CREAT) + ftruncate로 적절한 크기의 shm 파일 생성.
 *   2) mmap으로 g_trace_file에 매핑.
 *   3) tpoint_count[] 초기화, tname/owner/object 슬롯 zero clear.
 *   4) constructor로 등록된 모든 reg_fn() 호출 → tpoint 메타 등록.
 * 보통 spdk_app_start() 또는 main() 시작에서 1회 호출.
 */
int spdk_trace_init(const char *shm_name, uint64_t num_entries, uint32_t num_threads);

/**
 * Initialize trace environment for an user created thread.
 *
 * \return 0 on success, else non-zero indicates a failure.
 */
/*
 * [한국어]
 * spdk_trace_register_user_thread - reactor 외 사용자 스레드를 trace 대상에 추가.
 *
 * @return: 0 성공 / 음수 errno.
 *
 * 별도 history 슬롯 할당 + tname[]에 스레드 이름 등록.
 * pthread 등으로 만든 비-reactor 스레드에서도 trace 가능하게 함.
 */
int spdk_trace_register_user_thread(void);

/**
 * De-initialize trace environment for an user created thread.
 *
 * \return 0 on success, else non-zero indicates a failure.
 */
/*
 * [한국어]
 * spdk_trace_unregister_user_thread - 사용자 스레드의 trace 슬롯 해제.
 *
 * 스레드 종료 직전에 호출. history 슬롯을 free 표시하여 재사용 가능하게 함.
 */
int spdk_trace_unregister_user_thread(void);

/**
 * Unmap global trace memory structs.
 */
/*
 * [한국어]
 * spdk_trace_cleanup - trace 시스템 종료. shm 매핑 해제 및 g_trace_file=NULL.
 *
 * 호출 후 spdk_trace_record는 enabled 검사에서 g_trace_file==NULL로 즉시 noop.
 * 보통 spdk_app_stop() 마지막 단계에서 1회 호출.
 */
void spdk_trace_cleanup(void);

/* [한국어] owner type / object의 "없음" 의미 (sentinel).
 * tpoint 정의에서 owner나 object 추적이 필요 없을 때 사용. */
#define OWNER_TYPE_NONE 0
#define OBJECT_NONE 0

/**
 * Register the trace owner type.
 *
 * \param type Type of the trace owner.
 * \param id_prefix Prefix of id for the trace owner.
 */
/*
 * [한국어]
 * spdk_trace_register_owner_type - owner 분류(클래스)를 등록.
 *
 * @type:      0~255의 정수 ID (모듈이 자체 정의, 예: OWNER_BDEV=1).
 * @id_prefix: parser 출력에 사용할 한 글자 (예: 'b' for bdev).
 *
 * 보통 reg_fn(SPDK_TRACE_REGISTER_FN으로 등록된)이 module init 시 호출.
 * spdk_trace_file.owner_type[type]에 저장.
 */
void spdk_trace_register_owner_type(uint8_t type, char id_prefix);

/**
 * Register the trace owner.
 *
 * \param owner_type Type of the owner being registered.
 * \param description Textual string describing the trace owner.
 * \return Allocated owner_id for the newly registered owner. Returns 0 if
 *         no trace_id could be allocated.
 */
/*
 * [한국어]
 * spdk_trace_register_owner - owner의 인스턴스 1개를 등록하고 ID 반환.
 *
 * @owner_type:  spdk_trace_register_owner_type()로 미리 등록된 분류.
 * @description: 사용자에게 보일 설명 문자열 (예: "qpair_id=3 lcore=2").
 * @return: 새로 할당된 owner_id (1 이상). 0이면 할당 실패 (슬롯 부족).
 *
 * 호출자는 받은 owner_id를 spdk_trace_record(...)의 owner_id 인자로 전달.
 * 내부에서 num_owners++ 및 description 복사.
 */
uint16_t spdk_trace_register_owner(uint8_t owner_type, const char *description);

/**
 * Change the description for a previously registered owner.
 *
 * \param owner_id ID of previously registered owner.
 * \param description New description for the owner.
 */
/*
 * [한국어]
 * spdk_trace_owner_set_description - 기존 owner의 description을 새 문자열로 교체.
 *
 * @owner_id:    register_owner가 반환한 ID.
 * @description: 새 설명 문자열 (owner_description_size 이내).
 *
 * lifetime 동안 owner의 컨텍스트가 변할 때(예: qpair 재할당) 사용.
 */
void spdk_trace_owner_set_description(uint16_t owner_id, const char *description);

/**
 * Append to the description for a previously registered owner.
 *
 * A space will be inserted before the appended string.
 *
 * This may be useful for modules that are modifying an existing description
 * with additional information.
 *
 * Callers may pass 0 safely, in this case the function will just be a nop.
 *
 * \param owner_id ID of previously registered owner.
 * \param description Additional description for the owner
 */
/*
 * [한국어]
 * spdk_trace_owner_append_description - 기존 description 뒤에 문자열을 공백으로 구분해 추가.
 *
 * @owner_id:    0이면 nop (안전 호출 보장).
 * @description: 추가할 문자열.
 *
 * 다른 서브시스템이 이미 등록된 owner에 자신의 정보를 덧붙일 때 유용.
 * 예: nvmf가 qpair 등록 후 transport가 추가 정보 append.
 */
void spdk_trace_owner_append_description(uint16_t owner_id, const char *description);

/**
 * Unregister a previously registered owner.
 *
 * Callers may pass 0 safely, in this case the function will just be a nop.
 *
 * \param owner_id ID of previously registered owner.
 */
/*
 * [한국어]
 * spdk_trace_unregister_owner - owner 인스턴스 해제 (슬롯 free 마크).
 *
 * @owner_id: 0이면 nop.
 *
 * 해당 owner_id를 갖는 entry는 이후 발사되지 않아야 함 (호출자 책임).
 */
void spdk_trace_unregister_owner(uint16_t owner_id);

/**
 * Register the trace object.
 *
 * \param type Type of the trace object.
 * \param id_prefix Prefix of id for the trace object.
 */
/*
 * [한국어]
 * spdk_trace_register_object - object 분류 등록 (owner_type과 대칭).
 *
 * @type:      0~255 정수 ID (예: OBJECT_BDEV_IO=1).
 * @id_prefix: parser 출력 prefix (예: 'i' for bdev_io).
 *
 * spdk_trace_file.object[type]에 저장. tpoint 정의에서 object_type 필드로 참조.
 */
void spdk_trace_register_object(uint8_t type, char id_prefix);

/**
 * Register the description for a tpoint with a single argument.
 *
 * \param name Name for the tpoint.
 * \param tpoint_id Id for the tpoint.
 * \param owner_type Owner type for the tpoint.
 * \param object_type Object type for the tpoint.
 * \param new_object New object for the tpoint.
 * \param arg1_type Type of arg1.
 * \param arg1_name Name of argument.
 */
/*
 * [한국어]
 * spdk_trace_register_description - 단일 인자 tpoint 메타 등록 (간이 버전).
 *
 * @name:        tpoint 이름 (parser 표시용).
 * @tpoint_id:   SPDK_TPOINT_ID로 만든 ID.
 * @owner_type:  발행 주체 분류.
 * @object_type: 추적 대상 분류 (OBJECT_NONE 가능).
 * @new_object:  1이면 이 tpoint가 object 생성 시점.
 * @arg1_type:   SPDK_TRACE_ARG_TYPE_INT/PTR/STR.
 * @arg1_name:   인자 이름 문자열.
 *
 * 인자가 1개 이하면 이 함수, 여러 개면 _ext 버전을 사용.
 */
void spdk_trace_register_description(const char *name, uint16_t tpoint_id, uint8_t owner_type,
				     uint8_t object_type, uint8_t new_object,
				     uint8_t arg1_type, const char *arg1_name);

/* [한국어] 다중 인자 tpoint 등록용 옵션 구조체.
 * register_description_ext에 배열로 전달하여 여러 tpoint를 한 번에 등록 가능. */
struct spdk_trace_tpoint_opts {
	const char	*name;
	/* [한국어] tpoint 이름. */

	uint16_t	tpoint_id;
	/* [한국어] SPDK_TPOINT_ID 결합 ID. */

	uint8_t		owner_type;
	/* [한국어] 발행자 분류. */

	uint8_t		object_type;
	/* [한국어] 대상 object 분류 (OBJECT_NONE 가능). */

	uint8_t		new_object;
	/* [한국어] 1이면 object lifecycle의 시작점. */

	struct {
		const char	*name;
		/* [한국어] 인자 이름. */

		uint8_t		type;
		/* [한국어] SPDK_TRACE_ARG_TYPE_*. */

		uint8_t		size;
		/* [한국어] 인자 바이트 수 (STR은 최대 길이). */
	} args[SPDK_TRACE_MAX_ARGS_COUNT];
	/* [한국어] 최대 8개 인자 메타 — name이 NULL인 슬롯에서 종료. */
};

/**
 * Register the description for a number of tpoints. This function allows the user to register
 * tracepoints with multiple arguments.
 *
 * \param opts Array of structures describing tpoints and their arguments.
 * \param num_opts Number of tpoints to register (size of the opts array).
 */
/*
 * [한국어]
 * spdk_trace_register_description_ext - 여러 tpoint를 한 번에 등록 (다중 인자 지원).
 *
 * @opts:     spdk_trace_tpoint_opts 배열.
 * @num_opts: 배열 길이.
 *
 * reg_fn() 내에서 모듈의 모든 tpoint 메타를 일괄 등록할 때 권장.
 * 인자가 1개뿐이면 register_description() 대신 사용해도 무방하지만 _ext가 더 일반적.
 */
void spdk_trace_register_description_ext(const struct spdk_trace_tpoint_opts *opts,
		size_t num_opts);

/*
 * [한국어]
 * spdk_trace_get_first_register_fn - 등록된 reg_fn 리스트의 첫 노드 반환.
 *
 * @return: 첫 spdk_trace_register_fn* / 비어 있으면 NULL.
 *
 * trace_init이 모든 모듈의 reg_fn을 순회 호출할 때 사용.
 * SPDK_TRACE_REGISTER_FN(SPDK_TPOINT_ID, ...) constructor가 이 리스트에 자동 삽입.
 */
struct spdk_trace_register_fn *spdk_trace_get_first_register_fn(void);

/*
 * [한국어]
 * spdk_trace_get_next_register_fn - 현재 reg_fn의 다음 노드 반환 (싱글 링크드 리스트).
 *
 * @register_fn: 현재 노드.
 * @return: 다음 노드 / 마지막이면 NULL.
 */
struct spdk_trace_register_fn *spdk_trace_get_next_register_fn(struct spdk_trace_register_fn
		*register_fn);

/**
 * Bind trace type to a given trace object. This allows for matching traces
 * with the same parent trace object.
 *
 * \param tpoint_id Type of trace to be bound
 * \param object_type Tracepoint object type to bind to
 * \param arg_index Index of argument containing context information
 */
/*
 * [한국어]
 * spdk_trace_tpoint_register_relation - tpoint와 object 사이 cross-reference 등록.
 *
 * @tpoint_id:   대상 tpoint.
 * @object_type: 연관시킬 object 분류.
 * @arg_index:   해당 object의 ID를 담고 있는 args[] 인덱스.
 *
 * 동일 object_id를 갖는 다른 tpoint들을 parser가 묶어 lifecycle 시각화.
 * 예: NVMe IO submit/completion이 같은 bdev_io 객체로 표시.
 */
void spdk_trace_tpoint_register_relation(uint16_t tpoint_id, uint8_t object_type,
		uint8_t arg_index);

/**
 * Enable trace on specific tpoint group
 *
 * \param group_name Name of group to enable, "all" for enabling all groups.
 * \return 0 on success, else non-zero indicates a failure.
 */
/*
 * [한국어]
 * spdk_trace_enable_tpoint_group - 이름으로 group 전체를 활성화.
 *
 * @group_name: "bdev", "nvme", "all" 등 (reg_fn에 등록된 이름).
 * @return: 0 성공 / 음수 실패 (-ENOENT 등).
 *
 * spdk_app -e 옵션 처리에서 호출. "all"은 모든 group 켬.
 */
int spdk_trace_enable_tpoint_group(const char *group_name);

/**
 * Disable trace on specific tpoint group
 *
 * \param group_name Name of group to disable, "all" for disabling all groups.
 * \return 0 on success, else non-zero indicates a failure.
 */
/*
 * [한국어]
 * spdk_trace_disable_tpoint_group - enable의 대칭. 이름으로 group 비활성화.
 */
int spdk_trace_disable_tpoint_group(const char *group_name);

/**
 * Show trace mask and its usage.
 *
 * \param f File to hold the mask's information.
 * \param tmask_arg Command line option to set the trace group mask.
 */
/*
 * [한국어]
 * spdk_trace_mask_usage - 등록된 모든 group 이름과 사용법을 출력.
 *
 * @f:         출력 대상 (보통 stderr).
 * @tmask_arg: 출력에 사용할 옵션 문자열 (예: "-e").
 *
 * spdk_app --help의 trace 섹션 출력에 사용.
 */
void spdk_trace_mask_usage(FILE *f, const char *tmask_arg);

/**
 * Create a tracepoint group mask from tracepoint group name
 *
 * \param group_name tracepoint group name string
 * \return tpoint group mask on success, 0 on failure
 */
/*
 * [한국어]
 * spdk_trace_create_tpoint_group_mask - 이름을 group 비트마스크로 변환.
 *
 * @group_name: 단일 이름 또는 콤마 구분 (예: "bdev,nvme") / "all".
 * @return: 비트마스크 (실패 시 0).
 *
 * CLI에서 받은 문자열을 set_tpoint_group_mask에 넘기기 전 변환에 사용.
 */
uint64_t spdk_trace_create_tpoint_group_mask(const char *group_name);

/* [한국어] tpoint group 등록 단위 — SPDK_TRACE_REGISTER_FN 매크로가 자동 정의.
 * 각 모듈이 .data 섹션에 1개씩 정의하고, constructor로 g_register_fn_list에 추가. */
struct spdk_trace_register_fn {
	const char *name;
	/* [한국어] group 이름 문자열 (예: "bdev"). enable_tpoint_group이 매칭에 사용. */

	uint8_t tgroup_id;
	/* [한국어] 이 group의 ID (0 ~ SPDK_TRACE_MAX_GROUP_ID-1).
	 * SPDK_TPOINT_ID(tgroup_id, idx)로 tpoint_id 만들 때 상위 비트 부분. */

	void (*reg_fn)(void);
	/* [한국어] trace_init이 호출하여 tpoint 메타를 실제 등록하는 콜백.
	 * 보통 spdk_trace_register_owner_type/object/description_ext 호출 묶음. */

	struct spdk_trace_register_fn *next;
	/* [한국어] singly linked list 다음 노드.
	 * spdk_trace_add_register_fn이 head에 prepend. */
};

/**
 * Add new trace register function.
 *
 * \param reg_fn Trace register function to add.
 */
/*
 * [한국어]
 * spdk_trace_add_register_fn - reg_fn 노드를 글로벌 리스트에 추가.
 *
 * @reg_fn: 등록할 노드 (수명: 프로세스 전체 — 보통 .data 섹션).
 *
 * 보통 SPDK_TRACE_REGISTER_FN 매크로의 constructor가 자동으로 호출.
 * 사용자가 직접 부를 일은 거의 없다.
 */
void spdk_trace_add_register_fn(struct spdk_trace_register_fn *reg_fn);

/* [한국어] === tpoint group 자동 등록 매크로 ===
 * 모듈의 .c 파일 한 곳에 SPDK_TRACE_REGISTER_FN(my_reg, "my_group", MY_GROUP_ID)
 * 식으로 적으면:
 *   1) static void my_reg(void); 전방 선언 (사용자가 본문 정의).
 *   2) reg_my_reg라는 spdk_trace_register_fn 인스턴스를 .data 섹션에 정의
 *      (.name="my_group", .tgroup_id=MY_GROUP_ID, .reg_fn=my_reg).
 *   3) constructor 속성을 가진 _my_reg() 함수 정의.
 *      → main() 진입 전 자동 호출되어 spdk_trace_add_register_fn() 실행
 *      → 글로벌 리스트에 자동 삽입.
 *   4) trace_init 시 모든 reg_fn이 일괄 호출되어 tpoint 메타 등록.
 * 결과: 모듈 .c 파일 한 줄 + 본문 정의만으로 trace group 자동 활성화.
 */
#define SPDK_TRACE_REGISTER_FN(fn, name_str, _tgroup_id)	\
	static void fn(void);					\
	struct spdk_trace_register_fn reg_ ## fn = {		\
		.name = name_str,				\
		.tgroup_id = _tgroup_id,			\
		.reg_fn = fn,					\
		.next = NULL,					\
	};							\
	__attribute__((constructor)) static void _ ## fn(void)	\
	{							\
		spdk_trace_add_register_fn(&reg_ ## fn);	\
	}

#ifdef __cplusplus
}
#endif

#endif /* [한국어] _SPDK_TRACE_H_ include 가드 닫기 */
