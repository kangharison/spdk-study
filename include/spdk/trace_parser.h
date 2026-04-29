/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 */

/**
 * \file
 * Trace parser library
 */

/*
 * [한국어 설명] SPDK trace 결과 파서 공개 API (trace_parser.h) — 약 131 라인
 *
 * === 파일의 역할 ===
 * spdk/trace.h가 만들어낸 binary trace 데이터(/dev/shm 또는 디스크 파일에
 * 저장된 spdk_trace_file 영역)를 사람이 읽을 수 있는 형태로 변환하기 위한
 * 후처리(post-processing) 라이브러리의 공개 API. trace.h는 hot path에서
 * 고정 크기 spdk_trace_entry만 lockless 기록하므로 그 자체로는 사람이 읽기
 * 어렵다 — 본 헤더의 spdk_trace_parser는 (1) 여러 lcore의 ring buffer를
 * 읽어와 (2) tsc 기준으로 merge-sort하여 (3) tpoint 메타데이터(인자/이름/
 * object 관계)를 결합해 (4) 사용자 코드(예: app/spdk_trace의 CLI)가 한 줄씩
 * iterate할 수 있는 형태로 노출한다. 추가로 object lifecycle 추적
 * (object_index/object_start)과 tpoint 간 cross-reference (related_index/type)
 * 정보까지 자동 결합해주므로, 호출자는 raw entry를 직접 다룰 필요 없이
 * spdk_trace_parser_entry만으로 출력 가능하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 외부 도구 측의 라이브러리. 흐름:
 *   1) SPDK app(예: spdk_tgt)이 spdk_trace_init(shm_name, ...)으로
 *      /dev/shm/_trace.<name>에 trace 영역 생성·매핑.
 *   2) 운영 중 hot path가 spdk_trace_record(...)로 lockless 기록.
 *   3) 별도 프로세스인 spdk_trace CLI(app/trace/) 또는 사용자 도구가
 *      spdk_trace_parser_init(opts)로 파서 컨텍스트 생성.
 *      - 모드: SHM이면 /dev/shm 이름 매핑, FILE이면 디스크에 저장된 trace 파일.
 *      - lcore 옵션: 특정 코어만 또는 SPDK_TRACE_MAX_LCORE로 모든 코어.
 *   4) parser는 모든 lcore의 spdk_trace_history를 내부에서 읽어 tsc 기준
 *      heap/merge-sort하여 정렬된 시퀀스 준비.
 *   5) 호출자가 spdk_trace_parser_next_entry(parser, &out)을 false 반환까지
 *      반복 호출하여 한 entry씩 소비.
 *   6) 종료 시 spdk_trace_parser_cleanup(parser).
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/stdinc.h(uint*_t/bool), spdk/trace.h(spdk_trace_entry/file/
 *             SPDK_TRACE_MAX_ARGS_COUNT/SPDK_TRACE_MAX_LCORE 등 모든 trace 타입).
 * 의존(구현): lib/trace_parser/trace_parser.cpp가 본 API들의 실체 제공.
 *             내부에서 std::priority_queue 등으로 merge-sort 구현 (C++).
 * 의존하는 모듈: app/trace/spdk_trace.cpp (CLI), app/trace_record/ (디스크 dump),
 *             외부 사용자 도구 (Python ctypes 등).
 * 데이터 흐름: trace_file (mmap) → parser 내부 정렬 큐 → next_entry 호출자.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_trace_parser:        opaque 파서 컨텍스트.
 *   - enum spdk_trace_parser_mode:     FILE(디스크) / SHM(공유 메모리) 두 모드.
 *   - struct spdk_trace_parser_opts:   init 입력 — filename/mode/lcore.
 *   - struct spdk_trace_parser_entry:  next_entry가 채우는 출력 — entry 본체 +
 *     object 추적 메타 + 인자 unpacking 결과.
 *   - spdk_trace_parser_init / cleanup:    lifecycle.
 *   - spdk_trace_parser_get_file:          내부 trace_file 메타 조회.
 *   - spdk_trace_parser_get_tsc_offset:    모든 코어 첫 entry 중 최대 tsc
 *                                          (모든 코어 데이터가 존재하는 시작점).
 *   - spdk_trace_parser_next_entry:        정렬된 다음 entry 1개 반환.
 *   - spdk_trace_parser_get_entry_count:   특정 lcore의 entry 수 조회.
 */

#ifndef SPDK_TRACE_PARSER_H   /* [한국어] include 가드 */
#define SPDK_TRACE_PARSER_H

#include "spdk/stdinc.h"      /* [한국어] uint*_t, bool, UINT8_MAX, UINT64_MAX 등 표준 타입 */
#include "spdk/trace.h"       /* [한국어] spdk_trace_entry, spdk_trace_file, SPDK_TRACE_MAX_ARGS_COUNT 등 trace 타입 */

#ifdef __cplusplus
extern "C" {                  /* [한국어] 구현은 C++(.cpp)이지만 외부에는 C ABI로 노출 */
#endif

/** Trace parser object used as a context for the parsing functions */
/* [한국어] 파서 컨텍스트의 forward 선언 — 내부 구조는 trace_parser.cpp가 소유.
 * 사용자는 spdk_trace_parser_init이 반환한 포인터를 다른 함수에 그대로 전달만 함. */
struct spdk_trace_parser;

/* [한국어] trace 데이터 소스 모드 — opts.mode로 전달되어 init 동작 결정. */
enum spdk_trace_parser_mode {
	/** Regular file */
	SPDK_TRACE_PARSER_MODE_FILE,
	/* [한국어] 디스크에 저장된 정규 파일을 읽음. spdk_trace_record 도구 등이
	 * 운영 중 trace를 disk로 dump해둔 경우에 사용. open(2) + mmap. */

	/** Shared memory */
	SPDK_TRACE_PARSER_MODE_SHM,
	/* [한국어] 운영 중인 SPDK 앱의 /dev/shm/_trace.<name>를 그대로 매핑.
	 * 실시간 모니터링용 (앱이 살아있는 동안 ring이 갱신됨). */
};

/** Describes trace file and options to use when parsing it */
/* [한국어] parser_init에 전달하는 옵션 묶음. 모드/소스/대상 lcore를 한 번에 전달. */
struct spdk_trace_parser_opts {
	/** Either file name or shared memory name depending on mode */
	const char	*filename;
	/* [한국어] mode=FILE이면 정규 파일 경로, mode=SHM이면 shm_open할 이름.
	 * SHM 모드에서는 SPDK_TRACE_SHM_NAME_BASE prefix가 자동 부착되지 않으므로
	 * 호출자가 정확한 이름을 넘겨야 함. */

	/** Trace file type, either regular file or shared memory */
	int		mode;
	/* [한국어] enum spdk_trace_parser_mode 값 (FILE 또는 SHM). */

	/** Logical core number to parse the traces from (or SPDK_TRACE_MAX_LCORE) for all cores */
	uint16_t	lcore;
	/* [한국어] 분석 대상 lcore 번호. SPDK_TRACE_MAX_LCORE(=1024)면 전체 코어 통합 분석.
	 * 단일 lcore 분석은 sort 비용 없음(이미 시간 순).
	 * 전체 분석은 모든 lcore의 ring을 tsc로 merge-sort하므로 비용 큼. */
};

/**
 * Initialize the parser using a specified trace file.  This results in parsing the traces, merging
 * entries from multiple cores together and sorting them by their tsc, so it can take a significant
 * amount of time to complete.
 *
 * \param opts Describes the trace file to parse.
 *
 * \return Parser object or NULL in case of any failures.
 */
/*
 * [한국어]
 * spdk_trace_parser_init - 파서 컨텍스트 생성 및 trace 데이터 로드/정렬.
 *
 * @opts: 분석 모드/소스/lcore 지정 옵션.
 * @return: 파서 포인터 / 실패 시 NULL (파일 열기 실패, 매핑 실패 등).
 *
 * 동작:
 *   1) opts->filename을 mode에 따라 open(FILE) 또는 shm_open(SHM)하여 mmap.
 *   2) spdk_trace_file 헤더 검증 (file_size, tpoint 메타 등).
 *   3) opts->lcore에 따라 단일 코어 또는 모든 코어의 history 식별.
 *   4) 여러 코어인 경우 tsc 기준 priority_queue로 merge-sort 준비.
 *   5) object_id 추적 테이블 초기화 (lifecycle 자동 결합).
 *
 * 비용 주의: 큰 ring + 많은 lcore면 init만으로 수 초 소요 가능.
 */
struct spdk_trace_parser *spdk_trace_parser_init(const struct spdk_trace_parser_opts *opts);

/**
 * Free any resources tied to a parser object.
 *
 * \param parser Parser to clean up.
 */
/*
 * [한국어]
 * spdk_trace_parser_cleanup - 파서 컨텍스트 해제.
 *
 * @parser: init이 반환한 포인터 (NULL 허용).
 *
 * mmap 영역 munmap, 내부 sort 큐 free, object 추적 테이블 free.
 * 호출 후 parser 포인터 사용 금지.
 */
void spdk_trace_parser_cleanup(struct spdk_trace_parser *parser);

/**
 * Return trace file describing the traces.
 *
 * \param parser Parser object to be used.
 *
 * \return Pointer to the trace file.
 */
/*
 * [한국어]
 * spdk_trace_parser_get_file - 내부 trace_file 메타 포인터 반환.
 *
 * @parser: 파서 컨텍스트.
 * @return: 매핑된 spdk_trace_file*. tsc_rate, tpoint[], owner 메타 등 조회용.
 *
 * 호출자가 entry 출력 시 tpoint name/argument metadata에 접근하기 위해 사용.
 * 반환된 포인터의 수명은 parser_cleanup까지.
 */
const struct spdk_trace_file *spdk_trace_parser_get_file(const struct spdk_trace_parser *parser);

/**
 * Return the highest tsc out of first entries across all specified cores.  This value can be used
 * to select entries from the subset of time we have the data from all reactors.
 *
 * \param parser Parser object to be used.
 *
 * \return Offset in tsc.
 */
/*
 * [한국어]
 * spdk_trace_parser_get_tsc_offset - 모든 분석 대상 코어의 첫 entry 중 최대 tsc 반환.
 *
 * @parser: 파서 컨텍스트.
 * @return: tsc 값 (모든 코어가 공통으로 데이터를 가진 가장 이른 시점).
 *
 * 사용 사례: ring이 wrap되어 코어마다 시작 tsc가 다를 수 있다. 이 offset
 * 이전 데이터는 일부 코어에만 존재하므로 분석에서 제외하면 일관된 시간
 * 윈도우만 보게 된다. 호출자는 이 값보다 작은 entry를 무시하면 됨.
 */
uint64_t spdk_trace_parser_get_tsc_offset(const struct spdk_trace_parser *parser);

/** Describes a parsed trace entry */
/* [한국어] next_entry가 채우는 출력 구조체. raw entry + parser가 자동으로
 * 결합한 메타데이터(object lifecycle, related cross-reference, unpacked args). */
struct spdk_trace_parser_entry {
	/** Pointer to trace entry */
	struct spdk_trace_entry	*entry;
	/* [한국어] 원본 raw entry 포인터 (mmap 영역 내부).
	 * tsc/tpoint_id/owner_id/size/object_id 직접 접근.
	 * 수명: parser가 살아있는 동안만 유효. */

	/**
	 * Index of an object this entry is a part of.  It's only available for tracepoints with
	 * object_type != OBJECT_NONE.  If unavailable, it'll be assigned to UINT64_MAX.
	 */
	uint64_t		object_index;
	/* [한국어] object lifecycle에서의 N번째 instance (0부터).
	 * tpoint 정의에 object_type이 있는 경우만 유효.
	 * 미사용 entry는 UINT64_MAX로 표시 — sentinel 비교로 분기.
	 * parser가 동일 object_id의 lifecycle을 추적해 자동 부여. */

	/** The tsc of when the object tied to this entry was created */
	uint64_t		object_start;
	/* [한국어] 이 entry가 속한 object의 생성 시점 TSC (new_object=1인 entry의 tsc).
	 * (entry->tsc - object_start)로 lifecycle 내 경과 시간 계산 가능. */

	/** Logical core number */
	uint16_t		lcore;
	/* [한국어] 이 entry를 발행한 logical core 번호 (parser가 history에서 추론).
	 * 다중 코어 merge 결과에서 어느 코어 origin인지 식별. */

	/** Related object index */
	uint64_t		related_index;
	/* [한국어] cross-reference로 연결된 다른 object의 instance index.
	 * spdk_trace_tpoint_register_relation()로 등록된 관계가 있는 경우만 의미.
	 * 없으면 UINT64_MAX. */

	/** Related object type */
	uint8_t			related_type;
	/* [한국어] cross-reference 대상 object의 분류 (spdk_trace_object.type).
	 * 0(OBJECT_NONE)이면 cross-reference 없음.
	 * 예: 현재 entry가 NVMe IO이고 related가 bdev_io를 가리키는 식. */

	/** Tracepoint arguments */
	struct {
		bool		is_related;
		/* [한국어] true면 이 인자가 cross-reference object를 가리키는 인자.
		 * 출력 시 다른 색/형식으로 표시하는 등 분기 신호. */

		union {
			uint64_t	integer;
			/* [한국어] SPDK_TRACE_ARG_TYPE_INT일 때 사용. uint64로 unpacking. */

			void		*pointer;
			/* [한국어] SPDK_TRACE_ARG_TYPE_PTR일 때 사용. 8B 포인터 값. */

			char		string[UINT8_MAX + 1];
			/* [한국어] SPDK_TRACE_ARG_TYPE_STR일 때 사용 (최대 256B + null).
			 * trace.h의 entry chaining(여러 ring 슬롯 alias)을 parser가
			 * 결합해 평면 문자열로 복원. */
		} u;
	} args[SPDK_TRACE_MAX_ARGS_COUNT];
	/* [한국어] 최대 8개 인자의 unpacking 결과.
	 * tpoint 메타(spdk_trace_tpoint.args[].type)에 따라 어느 union 멤버를 읽을지 결정.
	 * 호출자는 trace_file.tpoint[entry->tpoint_id].args[i].type을 보고 분기. */
};

/**
 * Return next parsed trace entry.  Once no more traces are available, this will return false and
 * entry won't be touched.
 *
 * \param parser Parser object to be used.
 * \param entry Tracepoint entry.
 *
 * \return True if a trace entry was available, false otherwise.
 */
/*
 * [한국어]
 * spdk_trace_parser_next_entry - 정렬된 시퀀스의 다음 entry 1개를 반환.
 *
 * @parser: 파서 컨텍스트.
 * @entry: 출력 — parser가 채울 구조체 (호출자가 스택/힙에 할당).
 * @return: true면 entry가 채워짐, false면 더 이상 데이터 없음 (종료 조건).
 *
 * 동작:
 *   - 다중 코어인 경우 priority_queue에서 가장 작은 tsc entry를 pop.
 *   - 해당 entry의 tpoint 메타로 args 영역 unpack (entry chaining 결합).
 *   - object_id 추적 테이블 갱신 (new_object면 새 instance 등록).
 *   - related_objects 매핑이 있으면 cross-reference 인덱스 채움.
 *
 * 일반적 사용:
 *   while (spdk_trace_parser_next_entry(parser, &e)) { print(e); }
 */
bool spdk_trace_parser_next_entry(struct spdk_trace_parser *parser,
				  struct spdk_trace_parser_entry *entry);

/**
 * Return the number of entries recorded on a given core.
 *
 * \param parser Parser object to be used.
 * \param lcore Logical core number.
 *
 * \return Number of entries.
 */
/*
 * [한국어]
 * spdk_trace_parser_get_entry_count - 특정 lcore의 누적 기록 entry 수 반환.
 *
 * @parser: 파서 컨텍스트.
 * @lcore:  조회 대상 logical core 번호.
 * @return: 해당 코어 history의 next_entry 값 (단조 증가 카운터).
 *
 * 사용: 코어별 활동량 통계, ring wrap 여부 추정 (count > num_entries면 wrap됨).
 */
uint64_t spdk_trace_parser_get_entry_count(const struct spdk_trace_parser *parser, uint16_t lcore);

#ifdef __cplusplus
}
#endif

#endif /* [한국어] SPDK_TRACE_PARSER_H include 가드 닫기 */
