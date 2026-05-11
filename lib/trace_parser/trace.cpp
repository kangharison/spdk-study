/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK 추적(trace) 바이너리 엔트리 파서 (trace.cpp)
 *
 * === 파일의 역할 ===
 * SPDK가 런타임에 /dev/shm/<name> 또는 디스크 파일에 기록하는 바이너리 trace 링버퍼를
 * 사람이 읽기 좋은 이벤트 시퀀스로 변환하는 파서의 핵심 구현이다. include/spdk/trace.h가
 * 정의하는 spdk_trace_history(per-lcore 환형 버퍼) → spdk_trace_entry(고정 길이 24바이트
 * 단위) 형식의 원시 데이터를 읽어, 시간 순서로 정렬한 뒤 각 엔트리의 인자(args)를 풀어내
 * 호출자(spdk_trace, spdk_trace_record 등의 툴)가 한 줄씩 소비할 수 있도록 한다. 인자
 * 데이터가 인접 엔트리 슬롯으로 넘쳐 흐르는("buffer continuation") 경우의 패치 처리,
 * 객체(io, request 등) 단위로 관계(related)를 추적하는 기능, 환형 버퍼 오버플로 시
 * 가장 늦게 시작된 reactor의 첫 TSC를 기준으로 정렬을 맞추는 보정 로직을 포함한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 trace 파이프라인은 (1) lib/trace에서 SPDK_TRACE_REGISTER_FN으로 tracepoint를
 * 등록하고 spdk_trace_record()로 lcore별 환형 버퍼에 32바이트 entry를 기록하는 "기록 측"과,
 * (2) lib/trace_parser(이 파일)이 mmap을 통해 그 공유메모리(shm) 또는 파일을 읽어
 * 단일 시간순 시퀀스로 직렬화하는 "재생 측"으로 나뉜다. 이 파일은 후자에 위치한다.
 * 호출 체인: app/trace, app/trace_record → spdk_trace_parser_init/next_entry/cleanup
 *   → (내부) spdk_trace_parser::init → mmap(shm)/open(file) → populate_events
 *   → 호출자에 시간순 spdk_trace_parser_entry를 한 건씩 반환.
 * 실행 컨텍스트: 일반 유저스페이스 프로세스(추적 분석 도구)의 단일 스레드에서 실행된다.
 * SPDK reactor 컨텍스트가 아니므로 polled-mode 가정/스레드 affinity 제약은 없다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: include/spdk/trace.h(spdk_trace_file/history/entry/argument 정의),
 *   include/spdk/trace_parser.h(공개 C API spdk_trace_parser_*),
 *   include/spdk/util.h(spdk_min, spdk_likely 등), include/spdk/log.h(SPDK_ERRLOG),
 *   POSIX mmap/shm_open/open/fstat/munmap/close.
 * 데이터 흐름: SPDK 애플리케이션(spdk_app_start로 시작된 프로세스)이
 *   /dev/shm/<base>_trace.<pid> 등에 spdk_trace_file 헤더 + per-lcore history를 기록 →
 *   본 파서가 mmap으로 그 영역을 읽기 전용으로 매핑 → 시간순 std::map 정렬 후
 *   호출자에게 spdk_trace_parser_entry 단위로 스트리밍.
 * 공유 자료구조: spdk_trace_file은 prod/cons가 동일 머신에 공존하는 동안 read-only로
 *   접근(MAP_SHARED+PROT_READ)되며, 기록자는 환형 버퍼로 덮어쓸 수 있다. 본 파서는
 *   "오버플로 보정"을 통해 부분적으로 일관된 view를 만든다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_trace_parser: 파서 본체 클래스. mmap한 trace 파일을 소유하고 시간순 std::map을 유지.
 * - spdk_trace_parser::init(): mmap을 두 번 수행(헤더 크기 → 전체 크기) 후 populate_events 호출.
 * - spdk_trace_parser::populate_events(): 한 lcore의 환형 버퍼를 훑어 std::map<entry_key,*>에 삽입.
 * - spdk_trace_parser::next_entry(): 시간순 다음 엔트리를 빌드해 호출자에 반환.
 *   build_arg()로 가변 인자를 풀어내고, related_objects를 통해 객체간 관계를 채움.
 * - spdk_trace_parser::build_arg(): tpoint 메타데이터를 보고 entry/buffer continuation을 따라가며
 *   인자 byte stream을 spdk_trace_parser_entry::args[i]에 복사.
 * - entry_key/compare_entry_key: (lcore, tsc) 복합키. tsc 1차, lcore 2차 기준 정렬로
 *   동일 TSC 다중 lcore 이벤트의 결정적 순서를 보장.
 * - argument_context: build_arg()가 entry → 다음 buffer로 슬롯을 넘나들며 인자를 복원할 때
 *   현재 buffer 포인터/오프셋을 추적.
 * - object_stats: 객체 ID 단위로 (등록 순서 index, 시작 TSC)를 추적하여 관련 이벤트의
 *   상대적 인덱스/시작점을 호출자에 제공.
 * - 공개 C API: spdk_trace_parser_init/cleanup/next_entry/get_file/get_tsc_offset/
 *   get_entry_count — C 호출자(주로 app/trace)에서 사용하는 파사드.
 */

#include "spdk/stdinc.h"   /* [한국어] POSIX 표준 stdint, stddef, string 등 일괄 포함. SPDK 빌드에서 OS별 분기를 흡수하는 호환 헤더 */
#include "spdk/likely.h"   /* [한국어] spdk_likely/spdk_unlikely 분기 힌트 매크로. 핫패스 build_arg에서 buffer-continuation 경로를 unlikely로 표시 */
#include "spdk/log.h"      /* [한국어] SPDK_ERRLOG 매크로. 파싱 실패/I/O 오류를 stderr+syslog로 보고 */
#include "spdk/trace_parser.h" /* [한국어] 본 파일이 구현하는 공개 C API와 spdk_trace_parser_entry 구조체 정의 */
#include "spdk/util.h"     /* [한국어] spdk_min/spdk_likely 등 유틸. build_arg에서 인자 길이 클램프에 사용 */
#include "spdk/env.h"      /* [한국어] SPDK 환경 추상화(여기서는 직접 호출 없음, 기존 빌드 호환을 위해 포함) */

#include <exception>       /* [한국어] std::exception. 생성자 실패 시 init() 실패를 호출자(spdk_trace_parser_init)에 전파 */
#include <map>             /* [한국어] std::map<entry_key,*>로 (tsc,lcore) 키 시간순 정렬을 자동 유지 (RB-tree O(log N)) */
#include <new>             /* [한국어] std::nothrow 용 헤더(직접 사용은 없지만 new 실패 처리 호환) */

/*
 * [한국어]
 * struct entry_key — std::map의 키. (tsc, lcore) 쌍으로 결정적 정렬을 만든다.
 *
 * 같은 TSC를 가진 이벤트가 서로 다른 lcore에서 발생할 수 있으므로, tsc만으로는
 * 안정적 순서를 보장할 수 없다. tsc 1차 + lcore 2차 비교를 통해 동일 TSC 묶음 내에서도
 * 항상 같은 순서로 출력되도록 한다.
 */
struct entry_key {
	/* [한국어] 생성자: lcore와 tsc를 묶어 키 객체를 만든다.
	 * 호출자: populate_events()가 _entries[entry_key(lcore, tsc)] = &e[i]; 형태로 사용.
	 * 모든 인자가 이미 검증된 raw 값이므로 이 생성자에서는 추가 검증을 하지 않는다. */
	entry_key(uint16_t _lcore, uint64_t _tsc) : lcore(_lcore), tsc(_tsc) {}

	uint16_t lcore;
	/* [한국어] 이벤트가 기록된 reactor의 lcore ID (0 .. SPDK_TRACE_MAX_LCORE-1).
	 * 설정자: 생성자.
	 * 읽는 자: compare_entry_key, next_entry()의 pe->lcore 채움.
	 * 동기화: 키는 immutable이므로 락 불필요. */

	uint64_t tsc;
	/* [한국어] 이벤트의 TSC(timestamp counter) 값. rdtsc 단위 (코어 frequency에 의존).
	 * 설정자: 생성자.
	 * 읽는 자: compare_entry_key의 1차 정렬 기준.
	 * 값 범위: 0이 아닌 양의 정수(0은 환형 버퍼의 미사용 슬롯을 의미하므로 인덱스 대상에서 제외됨).
	 * 동기화: immutable. */
};

/*
 * [한국어]
 * class compare_entry_key — std::map<entry_key,*>에 주입할 비교자(functor).
 *
 * tsc가 동일하면 lcore < lcore로 폴백, 그렇지 않으면 tsc < tsc로 비교.
 * 표준 라이브러리의 strict weak ordering 요구를 충족한다.
 */
class compare_entry_key
{
public:
	/* [한국어] operator() — first < second를 반환한다.
	 * @first: 비교 대상 좌측 키
	 * @second: 비교 대상 우측 키
	 * @return: first가 second보다 "이전"이면 true.
	 * 호출자: std::map 내부의 RB-tree 균형/탐색 알고리즘.
	 * tsc 동률 시 lcore 오름차순으로 안정 정렬을 보장한다. */
	bool operator()(const entry_key &first, const entry_key &second) const
	{
		if (first.tsc == second.tsc) {  /* [한국어] tsc가 같다면 — 동일 시점에 다른 코어에서 발생한 이벤트들 */
			return first.lcore < second.lcore;  /* [한국어] lcore 번호 작은 쪽을 앞으로 — 결정적 순서 부여 */
		} else {
			return first.tsc < second.tsc;  /* [한국어] 일반 케이스 — TSC가 작은(=과거) 이벤트가 앞 */
		}
	}
};

/* [한국어] 시간순 인덱스 자료형. 키: (lcore,tsc), 값: 원본 entry 포인터(mmap 영역 내부 주소).
 * 본 std::map은 build 시 한 번 채워지고 next_entry()의 _iter로 순차 소비된다. */
typedef std::map<entry_key, spdk_trace_entry *, compare_entry_key> entry_map;

/*
 * [한국어]
 * struct argument_context — build_arg()가 한 entry의 인자 byte stream을 따라갈 때
 * 사용하는 임시 상태. 인자가 entry 한 개 안에서 끝나지 않고 다음 buffer 슬롯으로
 * 계속 흐르는 경우(spdk_trace_record가 큰 인자를 기록할 때 발생)를 처리한다.
 */
struct argument_context {
	spdk_trace_entry	*entry;
	/* [한국어] 현재 처리 중인 원본 entry 포인터. 동일 TSC의 continuation buffer를
	 * 식별하는 기준값(buffer->tsc == entry->tsc)으로 사용. 설정자: 생성자. 읽는 자: build_arg(). */

	spdk_trace_entry_buffer	*buffer;
	/* [한국어] 현재 인자 데이터를 읽고 있는 buffer 포인터. 시작값은 entry를 그대로 캐스팅한 값.
	 * 슬롯이 끝나면 get_next_buffer()로 다음 슬롯을 채워 갱신. */

	uint16_t		lcore;
	/* [한국어] 현재 entry를 소유한 lcore. get_next_buffer()가 history 환형버퍼 끝에서
	 * 처음으로 wrap-around할 때 어느 lcore의 history를 봐야 하는지 식별하는 데 사용. */

	size_t			offset;
	/* [한국어] buffer->data[] 내부의 현재 읽기 오프셋(바이트). sizeof(buffer->data)에 도달하면
	 * 다음 buffer로 이동하고 offset을 0으로 리셋. */

	/* [한국어] 생성자 — entry/lcore로 초기화하고 buffer/offset을 첫 인자 위치에 맞춘다.
	 * 첫 인자는 spdk_trace_entry::args[] 안에 있고, 이는 spdk_trace_entry_buffer::data[]
	 * 영역에서 args 시작점만큼의 양의 오프셋에 해당한다. */
	argument_context(spdk_trace_entry *entry, uint16_t lcore) :
		entry(entry), lcore(lcore)
	{
		buffer = reinterpret_cast<spdk_trace_entry_buffer *>(entry);  /* [한국어] entry와 buffer는 동일 메모리 레이아웃이므로 그대로 캐스팅 */

		/* The first argument resides within the spdk_trace_entry structure, so the initial
		 * offset needs to be adjusted to the start of the spdk_trace_entry.args array
		 */
		/* [한국어] 첫 인자의 시작 위치 = entry::args의 오프셋 - buffer::data의 오프셋.
		 * spdk_trace_entry는 (tsc, tpoint_id, owner_id, size, object_id, args[...])
		 * spdk_trace_entry_buffer는 (tsc, tpoint_id, ..., data[N])이고 data는 args보다 앞에 위치할 수 있어
		 * 두 오프셋의 차이를 인자 시작점으로 보정한다. */
		offset = offsetof(spdk_trace_entry, args) -
			 offsetof(spdk_trace_entry_buffer, data);
	}
};

/*
 * [한국어]
 * struct object_stats — 한 OBJECT_TYPE에 대한 객체 단위 통계.
 *
 * SPDK trace는 tpoint가 "new_object" 플래그로 표시한 시점(예: io 생성)에 객체 ID(=포인터값)에
 * 새 인덱스를 부여한다. 이후 같은 객체 ID를 가진 후속 tpoint(예: io 완료)는 이 인덱스를 통해
 * "n번째 io의 lifecycle"이라는 사람이 읽는 표현으로 매핑된다.
 */
struct object_stats {
	std::map<uint64_t, uint64_t>	index;
	/* [한국어] object_id(주로 포인터의 정수표현) → 등록 순번(0,1,2,...) 매핑.
	 * 설정자: next_entry()에서 tpoint->new_object 시 stats->counter를 증가시키며 삽입.
	 * 읽는 자: pe->object_index 채움, related 매칭 시 lookup. */

	std::map<uint64_t, uint64_t>	start;
	/* [한국어] object_id → 처음 등장한 entry의 tsc 매핑.
	 * 호출자가 이벤트 상대 시점("객체 시작 후 +N tick에 발생")을 계산할 수 있도록 제공. */

	uint64_t			counter;
	/* [한국어] 이 OBJECT_TYPE에 부여할 다음 인덱스 값. new_object 발생마다 후증가.
	 * 동기화: 단일 스레드(파서) 내부 카운터이므로 락 불필요. */

	/* [한국어] 생성자 — counter를 0으로 초기화한다. _stats[]가 배열로 선언되므로 default-construct가 호출됨. */
	object_stats() : counter(0) {}
};

/*
 * [한국어]
 * struct spdk_trace_parser — 파서의 PIMPL 본체. 공개 헤더에는 forward declaration만 있고
 * 실제 정의는 본 파일에서만 보인다(C++ 캡슐화). spdk_trace_parser_init()이 new로 생성하고
 * spdk_trace_parser_cleanup()이 delete로 파괴한다.
 */
struct spdk_trace_parser {
	/* [한국어] 생성자 — opts를 받아 init()을 호출하고 실패 시 std::exception을 던진다.
	 * exception은 C 파사드 spdk_trace_parser_init이 catch(...)로 받아 NULL을 반환. */
	spdk_trace_parser(const spdk_trace_parser_opts *opts);

	/* [한국어] 소멸자 — cleanup()을 호출해 mmap/fd를 해제한다. */
	~spdk_trace_parser();

	/* [한국어] 복사/대입 금지 — 내부에 mmap fd 등 RAII 자원을 가지므로 의도적으로 삭제. */
	spdk_trace_parser(const spdk_trace_parser &) = delete;
	spdk_trace_parser &operator=(const spdk_trace_parser &) = delete;

	/* [한국어] 매핑된 trace 파일 헤더를 반환한다(읽기 전용). 호출자(app/trace)는 tpoint_map,
	 * num_lcores, tsc_rate 등을 직접 읽어 출력 헤더를 작성한다. */
	const spdk_trace_file *file() const { return _trace_file; }

	/* [한국어] 모든 reactor에 걸쳐 가장 늦게 시작된 reactor의 첫 TSC를 반환.
	 * 환형 버퍼 오버플로가 있을 때 호출자가 이 값보다 이전의 이벤트는 표시하지 않도록 컷오프. */
	uint64_t tsc_offset() const { return _tsc_offset; }

	/* [한국어] 시간 순으로 다음 엔트리를 entry에 채우고 true; 더 없으면 false. */
	bool next_entry(spdk_trace_parser_entry *entry);

	/* [한국어] 특정 lcore의 history->num_entries(원본 환형 버퍼 슬롯 수)를 반환. 0이면 없음. */
	uint64_t entry_count(uint16_t lcore) const;

private:
	/* [한국어] 환형 버퍼에서 buf의 다음 슬롯을 반환(끝에서는 0번으로 wrap). build_arg가 인자 데이터의
	 * continuation을 따라갈 때 사용. */
	spdk_trace_entry_buffer *get_next_buffer(spdk_trace_entry_buffer *buf, uint16_t lcore);

	/* [한국어] 인자 하나(arg)를 buffer stream에서 읽어 pe->args[argid]에 채운다. continuation 처리. */
	bool build_arg(argument_context *argctx, const spdk_trace_argument *arg, int argid,
		       spdk_trace_parser_entry *pe);

	/* [한국어] 한 lcore의 history 환형 버퍼를 한 바퀴 훑어 _entries 맵에 (key,entry*)를 삽입. */
	void populate_events(spdk_trace_history *history, int num_entries, bool overflowed);

	/* [한국어] opts에 따라 SHM/파일을 열고, 두 번의 mmap(헤더 크기 → 전체 크기)으로 매핑 후
	 * 모든 lcore에 대해 populate_events 수행. _iter를 _entries.begin()으로 설정. */
	bool init(const spdk_trace_parser_opts *opts);

	/* [한국어] munmap + close. 부분 초기화 실패 시에도 호출 가능하도록 NULL/음수 가드. */
	void cleanup();

	spdk_trace_file		*_trace_file;
	/* [한국어] mmap()으로 매핑된 trace 파일의 헤더 시작 주소.
	 * 설정자: init()의 두 번째 mmap. 읽는 자: file(), entry_count(), populate_events()의 history lookup. */

	size_t			_map_size;
	/* [한국어] _trace_file에 매핑된 총 바이트 수. munmap 시 정확한 크기로 해제. */

	int			_fd;
	/* [한국어] SHM 또는 파일의 file descriptor. cleanup에서 close. -1은 미초기화 상태. */

	uint64_t		_tsc_offset;
	/* [한국어] 모든 reactor의 첫 TSC 중 가장 큰 값. populate_events에서 overflowed가 있는 경우 갱신.
	 * 호출자는 이 값을 빼서 0-기준 상대 시각으로 표시하거나, 이 이전 이벤트를 폐기. */

	entry_map		_entries;
	/* [한국어] 시간 순 정렬된 (key, entry*) 인덱스. populate_events가 채우고 next_entry가 소비. */

	entry_map::iterator	_iter;
	/* [한국어] next_entry()의 진행 위치. init() 종료 시 begin()으로 설정. */

	object_stats		_stats[SPDK_TRACE_MAX_OBJECT];
	/* [한국어] OBJECT_TYPE별 통계 배열. 첨자: enum 0(OBJECT_NONE)~SPDK_TRACE_MAX_OBJECT-1. */
};

/*
 * [한국어]
 * spdk_trace_parser::entry_count — 특정 lcore의 환형 버퍼 슬롯 수를 반환.
 *
 * @lcore: SPDK lcore 번호.
 * @return: 해당 lcore에 history가 있으면 슬롯 수, 없으면 0.
 *
 * 호출자: app/trace가 lcore별 점유율/이벤트 수를 통계 표시할 때.
 * 이 값은 history->num_entries(고정 매크로 SPDK_TRACE_NUM_ENTRIES와 동일)이며 실제로
 * 채워진 이벤트 수와는 다를 수 있다.
 *
 * 호출 체인:
 *   app/trace::print_stats → spdk_trace_parser_get_entry_count → entry_count → spdk_get_per_lcore_history
 */
uint64_t
spdk_trace_parser::entry_count(uint16_t lcore) const
{
	spdk_trace_history *history;  /* [한국어] 해당 lcore의 환형 버퍼 헤더 포인터(mmap 영역 내부) */

	if (lcore >= SPDK_TRACE_MAX_LCORE) {  /* [한국어] 범위 벗어난 lcore 입력 방어 */
		return 0;
	}

	history = spdk_get_per_lcore_history(_trace_file, lcore);  /* [한국어] trace 파일 헤더에서 lcore 번호로 history 헤더 위치 계산 (per-lcore offset 기반) */

	return history == NULL ? 0 : history->num_entries;  /* [한국어] history가 없는 lcore(미사용 코어)는 0 */
}

/*
 * [한국어]
 * spdk_trace_parser::get_next_buffer — 환형 버퍼에서 buf 다음 슬롯을 반환.
 *
 * @buf: 현재 버퍼 슬롯 포인터.
 * @lcore: 어떤 lcore의 history 환형 버퍼를 따라갈지.
 * @return: 다음 슬롯 포인터. 마지막 슬롯이면 history->entries[0]으로 wrap.
 *
 * build_arg()가 인자 데이터(arg->size 바이트)가 한 슬롯의 data 영역(약 14B)을 넘어
 * 다음 슬롯으로 흐를 때 호출. spdk_trace_record가 record 시점에 동일한 wrap 규칙으로
 * 슬롯을 찢어서 저장한다.
 */
spdk_trace_entry_buffer *
spdk_trace_parser::get_next_buffer(spdk_trace_entry_buffer *buf, uint16_t lcore)
{
	spdk_trace_history *history;  /* [한국어] lcore의 history 헤더 */

	history = spdk_get_per_lcore_history(_trace_file, lcore);  /* [한국어] mmap 헤더로부터 history 위치 계산 */
	assert(history);  /* [한국어] 호출자가 유효 lcore만 넘기므로 NULL이면 디버그 빌드에서 즉시 검출 */

	if (spdk_unlikely(static_cast<void *>(buf) ==
			  static_cast<void *>(&history->entries[history->num_entries - 1]))) {
		/* [한국어] 마지막 슬롯에 도달 — 다음은 첫 슬롯으로 wrap (환형 버퍼). 흔치 않은 경로이므로 unlikely. */
		return reinterpret_cast<spdk_trace_entry_buffer *>(&history->entries[0]);
	} else {
		return buf + 1;  /* [한국어] 일반 케이스 — 다음 슬롯으로 단순 증가 (메모리 인접 보장됨) */
	}
}

/*
 * [한국어]
 * spdk_trace_parser::build_arg — 한 인자(arg)의 byte stream을 buffer continuation을
 *   따라가며 pe->args[argid]에 복사한다.
 *
 * @argctx: 현재 buffer 포인터/오프셋 등 가변 상태.
 * @arg: tpoint 메타데이터로부터 얻은 인자 정의(타입, 크기). spdk_trace_register_description으로 등록됨.
 * @argid: 이 인자의 인덱스 (0..tpoint->num_args-1).
 * @pe: 호출자에게 채워줄 결과 entry. pe->args[argid]에 정수/문자열/포인터를 채운다.
 * @return: 성공 시 true, 인자 byte stream이 다음 슬롯으로 넘어갔는데 슬롯 메타데이터가 어긋나
 *   continuation이 깨진 경우 false.
 *
 * 동작:
 *   1) is_related/u.integer를 0으로 초기화 (4B 정수에서 상위 4B 누수 방지).
 *   2) arg->size 바이트를 모두 채울 때까지 루프.
 *      - buffer의 data 영역이 바닥나면 다음 buffer로 이동하고 메타데이터(tpoint_id==MAX,tsc일치) 검증.
 *      - 인자가 string의 최대 크기 안에 있을 때만 pe->args[].u.string에 복사 (정수/포인터는 동일 union이므로 자동 처리).
 *   3) argctx의 buffer/offset 갱신.
 *
 * 호출 체인: next_entry → build_arg → get_next_buffer (continuation 시).
 */
bool
spdk_trace_parser::build_arg(argument_context *argctx, const spdk_trace_argument *arg, int argid,
			     spdk_trace_parser_entry *pe)
{
	spdk_trace_entry *entry = argctx->entry;            /* [한국어] continuation 검증을 위한 원본 entry 참조 */
	spdk_trace_entry_buffer *buffer = argctx->buffer;   /* [한국어] 현재 읽고 있는 buffer 슬롯 */
	size_t curlen, argoff;                              /* [한국어] curlen: 이번 chunk 길이, argoff: 인자 내부 진행 거리 */

	argoff = 0;                                         /* [한국어] 인자의 0번째 바이트부터 시작 */
	pe->args[argid].is_related = false;                 /* [한국어] 기본값: 다른 객체와의 관계 없음 (아래 next_entry에서 매칭되면 true) */
	/* Make sure that if we only copy a 4-byte integer, that the upper bytes have already been
	 * zeroed.
	 */
	pe->args[argid].u.integer = 0;                      /* [한국어] union 영역을 0으로 초기화 — 4B 정수 인자가 상위 4B를 더럽히지 않게 */

	while (argoff < arg->size) {                        /* [한국어] 인자 전체를 다 옮길 때까지 반복 */
		if (argctx->offset == sizeof(buffer->data)) {  /* [한국어] 현재 buffer 슬롯의 data가 끝났음 → 다음 슬롯으로 */
			buffer = get_next_buffer(buffer, argctx->lcore);  /* [한국어] 환형 버퍼의 다음 슬롯 획득 */
			if (spdk_unlikely(buffer->tpoint_id != SPDK_TRACE_MAX_TPOINT_ID ||
					  buffer->tsc != entry->tsc)) {
				/* [한국어] continuation 슬롯은 (tpoint_id==MAX, tsc==원본 entry tsc)로 표시되어야 한다.
				 * 어긋나면 환형버퍼 덮어쓰기 등으로 손상된 데이터이므로 파싱 실패 반환. */
				return false;
			}

			argctx->offset = 0;     /* [한국어] 새 슬롯의 data 처음부터 다시 읽기 */
			argctx->buffer = buffer;
		}

		/* [한국어] 이번 chunk 길이 = min(현재 슬롯에 남은 공간, 인자에 남은 바이트) */
		curlen = spdk_min(sizeof(buffer->data) - argctx->offset, arg->size - argoff);
		if (argoff < sizeof(pe->args[0].u.string)) {
			/* [한국어] 결과 union의 string 영역(가장 큰 크기) 안쪽이라면 실제 복사.
			 * 그 이상은 메타데이터가 더 큰 인자를 정의했더라도 결과 union에 담을 공간이 없으니 폐기. */
			memcpy(&pe->args[argid].u.string[argoff], &buffer->data[argctx->offset],
			       spdk_min(curlen, sizeof(pe->args[0].u.string) - argoff));
		}

		argctx->offset += curlen;  /* [한국어] buffer 내부 진행 */
		argoff += curlen;          /* [한국어] 인자 내부 진행 */
	}

	return true;
}

/*
 * [한국어]
 * spdk_trace_parser::next_entry — 시간 순 다음 이벤트를 채워서 반환.
 *
 * @pe: 호출자가 미리 할당한 결과 구조체. lcore, entry*, args[], related_*, object_*가 채워짐.
 * @return: 더 읽을 이벤트가 있으면 true. _iter가 끝에 도달하거나 build_arg 실패 시 false.
 *
 * 처리 흐름:
 *   1) _iter == end()면 false (스트림 종료).
 *   2) tpoint_id로부터 tpoint 정의를 얻고, 객체 타입이 OBJECT_NONE이 아니면 stats를 갱신/조회.
 *   3) 모든 인자를 build_arg로 채움.
 *   4) tpoint->related_objects[]를 순회하며 인자값(주로 포인터)이 다른 객체의 index에 있는지 확인,
 *      매칭되면 pe->related_index/related_type을 채우고 해당 인자에 is_related=true.
 *
 * 호출 체인: app/trace 메인 루프 → spdk_trace_parser_next_entry → next_entry.
 */
bool
spdk_trace_parser::next_entry(spdk_trace_parser_entry *pe)
{
	spdk_trace_tpoint *tpoint;             /* [한국어] tpoint 메타데이터 (이름, 인자 정의, 객체 타입 등) */
	spdk_trace_entry *entry;               /* [한국어] 현재 시간순으로 가장 이른 entry 포인터 */
	object_stats *stats;                   /* [한국어] tpoint의 객체 타입에 해당하는 통계 */
	std::map<uint64_t, uint64_t>::iterator related_kv;  /* [한국어] related lookup 결과 */

	if (_iter == _entries.end()) {         /* [한국어] 스트림 종료 — 더 이상 반환할 이벤트 없음 */
		return false;
	}

	pe->entry = entry = _iter->second;     /* [한국어] mmap 영역 내부의 원본 entry 포인터를 그대로 노출 (호출자는 read-only로 사용) */
	pe->lcore = _iter->first.lcore;        /* [한국어] entry_key에서 lcore 추출 */
	/* Set related index to the max value to indicate "empty" state */
	pe->related_index = UINT64_MAX;        /* [한국어] "관계 없음" 표시 — 호출자는 이 값으로 미설정 여부 판별 */
	pe->related_type = OBJECT_NONE;
	tpoint = &_trace_file->tpoint[entry->tpoint_id];   /* [한국어] tpoint 정의 lookup. tpoint_id는 등록 시 부여되는 0~SPDK_TRACE_MAX_TPOINT_ID-1 */
	stats = &_stats[tpoint->object_type];              /* [한국어] 이 tpoint가 다루는 객체 타입의 통계 슬롯 */

	if (tpoint->new_object) {              /* [한국어] 이 tpoint가 객체 생성 시점이면 — 새 인덱스 부여 */
		stats->index[entry->object_id] = stats->counter++;  /* [한국어] (object_id → 0,1,2,...) 매핑 + 카운터 후증가 */
		stats->start[entry->object_id] = entry->tsc;        /* [한국어] 시작 TSC 기록 — 후속 이벤트의 상대 시점 계산 기준 */
	}

	if (tpoint->object_type != OBJECT_NONE) {  /* [한국어] 객체 관련 tpoint면 결과에 객체 인덱스/시작 TSC를 채움 */
		if (spdk_likely(stats->start.find(entry->object_id) != stats->start.end())) {
			/* [한국어] 이미 등록된 객체 — 정상 케이스. 일반적으로 lifecycle은 new_object → ... → end 순으로 흐른다 */
			pe->object_index = stats->index[entry->object_id];
			pe->object_start = stats->start[entry->object_id];
		} else {
			/* [한국어] 등록되지 않은 객체 ID — 환형버퍼 wrap으로 new_object 이벤트가 잘려나갔을 때 발생.
			 * 호출자는 UINT64_MAX로 "추적 불가"를 인지. */
			pe->object_index = UINT64_MAX;
			pe->object_start = UINT64_MAX;
		}
	}

	argument_context argctx(entry, pe->lcore);  /* [한국어] 인자 풀어내기 위한 컨텍스트 초기화 */
	for (uint8_t i = 0; i < tpoint->num_args; ++i) {  /* [한국어] tpoint가 등록한 인자 수만큼 반복 */
		if (!build_arg(&argctx, &tpoint->args[i], i, pe)) {  /* [한국어] continuation 따라가며 i번째 인자 복원 */
			SPDK_ERRLOG("Failed to parse tracepoint argument\n");  /* [한국어] 손상된 trace stream */
			return false;
		}
	}

	for (uint8_t i = 0; i < SPDK_TRACE_MAX_RELATIONS; ++i) {  /* [한국어] tpoint에 정의된 관계(최대 N개) 순회 */
		/* The relations are stored inside a tpoint, which means there might be
		 * multiple objects bound to a single tpoint. */
		if (tpoint->related_objects[i].object_type == OBJECT_NONE) {  /* [한국어] OBJECT_NONE은 관계 종결 sentinel */
			break;
		}
		stats = &_stats[tpoint->related_objects[i].object_type];   /* [한국어] 매칭 대상이 되는 다른 객체 타입의 통계 */
		related_kv = stats->index.find(reinterpret_cast<uint64_t>
					       (pe->args[tpoint->related_objects[i].arg_index].u.pointer));
		/* [한국어] 인자값(포인터)이 다른 객체 stats의 index에 등록돼 있는지 lookup —
		 * 즉 "이 이벤트가 참조하는 포인터 X가, 이전에 OBJECT_TYPE_X로 추적되던 객체인가?" */
		/* To avoid parsing the whole array, object index and type are stored
		 * directly inside spdk_trace_parser_entry. */
		if (related_kv != stats->index.end()) {  /* [한국어] 매칭 성공 — 첫 매치만 사용하고 종료 (break) */
			pe->related_index = related_kv->second;                          /* [한국어] 관련 객체의 인덱스 */
			pe->related_type = tpoint->related_objects[i].object_type;       /* [한국어] 관련 객체의 타입 */
			pe->args[tpoint->related_objects[i].arg_index].is_related = true;/* [한국어] 호출자가 출력 시 강조 표시 가능 */
			break;
		}
	}

	_iter++;     /* [한국어] 다음 호출에서 이어 읽도록 진행 */
	return true;
}

/*
 * [한국어]
 * spdk_trace_parser::populate_events — 한 lcore의 환형 버퍼를 훑어 _entries에 키-포인터를 삽입.
 *
 * @history: 해당 lcore의 환형 버퍼 헤더(mmap 영역 내부 포인터).
 * @num_entries: history->num_entries(슬롯 수). 보통 SPDK_TRACE_NUM_ENTRIES.
 * @overflowed: 어떤 lcore라도 환형 버퍼가 한 바퀴 돌아 덮어쓴 적이 있으면 true.
 *
 * 동작:
 *   1) tsc==0 슬롯은 미사용. 끝쪽부터 0이 아닌 슬롯을 찾아 num_entries_filled 결정.
 *   2) num_entries == num_entries_filled (가득 찼다)면 한 바퀴 돌아 덮어쓴 케이스 →
 *      배열 안에서 첫 TSC(가장 작은)와 마지막 TSC(가장 큰)의 인덱스를 선형 탐색.
 *   3) 그렇지 않으면 0 ~ num_entries_filled-1이 곧 first ~ last.
 *   4) overflowed 플래그가 true이고 이 lcore의 첫 TSC가 기존 _tsc_offset보다 크면 갱신.
 *   5) first → last 순으로 환형 진행하며 tpoint_id가 MAX(=continuation 슬롯)가 아닌 슬롯만 _entries에 삽입.
 *
 * 호출 체인: init() → populate_events (lcore마다 1회).
 * 실행 컨텍스트: 단일 스레드. _entries는 동시 접근 없음.
 */
void
spdk_trace_parser::populate_events(spdk_trace_history *history, int num_entries, bool overflowed)
{
	int i, num_entries_filled;
	spdk_trace_entry *e;
	int first, last, lcore;

	lcore = history->lcore;        /* [한국어] history가 자기 자신의 lcore를 기록하고 있음 */
	e = history->entries;          /* [한국어] entries[] 베이스 포인터 */

	num_entries_filled = num_entries;  /* [한국어] 일단 전체로 가정하고 끝부터 0인 슬롯을 빼나간다 */
	while (e[num_entries_filled - 1].tsc == 0) {
		/* [한국어] tsc==0은 spdk_trace_record가 아직 쓴 적 없는 슬롯을 의미.
		 * 끝쪽에서 연속으로 0이 있다면 그만큼 환형이 한 바퀴 돌지 않았다는 뜻. */
		num_entries_filled--;
	}

	if (num_entries == num_entries_filled) {  /* [한국어] 모든 슬롯이 채워짐 → 환형 wrap 발생, 시작점 탐색 필요 */
		first = last = 0;
		for (i = 1; i < num_entries; i++) {  /* [한국어] 선형 탐색으로 최소/최대 TSC 위치 찾기 */
			if (e[i].tsc < e[first].tsc) {
				first = i;        /* [한국어] 가장 오래된 이벤트 — 환형 시작점 */
			}
			if (e[i].tsc > e[last].tsc) {
				last = i;         /* [한국어] 가장 최근 이벤트 — 환형 끝점 */
			}
		}
	} else {
		/* [한국어] 한 바퀴 돌지 않았음 — 자연스럽게 0번이 시작, num_entries_filled-1이 끝 */
		first = 0;
		last = num_entries_filled - 1;
	}

	/*
	 * We keep track of the highest first TSC out of all reactors iff. any
	 * have overflowed their circular buffer.
	 *  We will ignore any events that occurred before this TSC on any
	 *  other reactors.  This will ensure we only print data for the
	 *  subset of time where we have data across all reactors.
	 */
	if (e[first].tsc > _tsc_offset && overflowed) {
		/* [한국어] 어떤 reactor에 오버플로가 있다면, 모든 reactor가 데이터를 가진 가장 늦은
		 * 시작점을 _tsc_offset으로 삼는다. 이전 이벤트는 호출자가 컷오프 가능. */
		_tsc_offset = e[first].tsc;
	}

	i = first;
	while (1) {
		if (e[i].tpoint_id != SPDK_TRACE_MAX_TPOINT_ID) {
			/* [한국어] continuation 슬롯(tpoint_id==MAX)은 인덱싱 대상이 아님 — argument context로만 사용됨.
			 * 일반 entry만 _entries 맵에 삽입. */
			_entries[entry_key(lcore, e[i].tsc)] = &e[i];
		}
		if (i == last) {       /* [한국어] last에 도달 — 한 바퀴 종료 */
			break;
		}
		i++;
		if (i == num_entries_filled) {
			i = 0;         /* [한국어] 환형 wrap — 처음으로 돌아감 */
		}
	}
}

/*
 * [한국어]
 * spdk_trace_parser::init — 파일/SHM open → 두 번 mmap → populate_events 수행.
 *
 * @opts: 모드(파일/SHM), 파일명, 대상 lcore(또는 SPDK_TRACE_MAX_LCORE=전체).
 * @return: 성공 true, 실패 false. 실패 시 호출자 생성자가 cleanup 후 std::exception throw.
 *
 * 두 번 mmap하는 이유: trace 파일의 실제 크기는 헤더의 num_lcores * sizeof(history) 등을
 * 보아야 알 수 있다. 따라서 (1) 헤더 크기만 임시로 매핑해서 spdk_get_trace_file_size로 총 크기 계산,
 * (2) munmap 후 정확한 크기로 재매핑한다.
 */
bool
spdk_trace_parser::init(const spdk_trace_parser_opts *opts)
{
	spdk_trace_history *history;  /* [한국어] lcore별 history 헤더 임시 변수 */
	struct stat st;               /* [한국어] fstat 결과 — 파일 크기 검증에 사용 */
	int rc, i, entry_num;
	bool overflowed;              /* [한국어] 어떤 lcore라도 환형 wrap이 있는지 여부 */

	switch (opts->mode) {
	case SPDK_TRACE_PARSER_MODE_FILE:
		_fd = open(opts->filename, O_RDONLY);  /* [한국어] 일반 파일 모드 — 디스크에 저장된 trace dump 열기 */
		break;
	case SPDK_TRACE_PARSER_MODE_SHM:
		_fd = shm_open(opts->filename, O_RDONLY, 0600);  /* [한국어] POSIX SHM(/dev/shm/<name>) 모드 — 살아있는 SPDK 프로세스의 trace 영역 */
		break;
	default:
		SPDK_ERRLOG("Invalid mode: %d\n", opts->mode);
		return false;
	}

	if (_fd < 0) {  /* [한국어] open/shm_open 실패 — 파일 없음/권한 부족 등 */
		SPDK_ERRLOG("Could not open trace file: %s (%d)\n", opts->filename, errno);
		return false;
	}

	rc = fstat(_fd, &st);  /* [한국어] 파일 메타정보 — 크기를 알아야 한다. SHM도 fstat으로 크기 조회 가능 */
	if (rc < 0) {
		SPDK_ERRLOG("Could not get size of trace file: %s\n", opts->filename);
		return false;
	}

	if ((size_t)st.st_size < sizeof(*_trace_file)) {  /* [한국어] 파일이 헤더보다 작으면 손상/잘못된 파일 */
		SPDK_ERRLOG("Invalid trace file: %s\n", opts->filename);
		return false;
	}

	/* Map the header of trace file */
	_map_size = sizeof(*_trace_file);   /* [한국어] 1단계: 헤더만 임시 매핑 — 실제 크기를 알기 위해 */
	_trace_file = static_cast<spdk_trace_file *>(mmap(NULL, _map_size, PROT_READ,
			MAP_SHARED, _fd, 0));
	if (_trace_file == MAP_FAILED) {
		SPDK_ERRLOG("Could not mmap trace file: %s\n", opts->filename);
		_trace_file = NULL;
		return false;
	}

	/* Remap the entire trace file */
	_map_size = spdk_get_trace_file_size(_trace_file);  /* [한국어] 2단계: 헤더에 적힌 num_lcores 등으로 정확한 총 크기 계산 */
	munmap(_trace_file, sizeof(*_trace_file));          /* [한국어] 헤더 매핑 해제 — 곧바로 전체 크기로 재매핑 */
	if ((size_t)st.st_size < _map_size) {               /* [한국어] 파일 크기가 계산된 크기보다 작으면 잘림/손상 */
		SPDK_ERRLOG("Trace file %s is not valid\n", opts->filename);
		_trace_file = NULL;
		return false;
	}
	_trace_file = static_cast<spdk_trace_file *>(mmap(NULL, _map_size, PROT_READ,
			MAP_SHARED, _fd, 0));   /* [한국어] 전체 영역 매핑. 이후 모든 history/entries 접근은 이 영역 내부 */
	if (_trace_file == MAP_FAILED) {
		SPDK_ERRLOG("Could not mmap trace file: %s\n", opts->filename);
		_trace_file = NULL;
		return false;
	}

	if (opts->lcore == SPDK_TRACE_MAX_LCORE) {  /* [한국어] 전체 lcore 모드 — 모든 reactor 이벤트를 시간순으로 병합 */
		/* Check if any reactors have overwritten their circular buffer. */
		for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {  /* [한국어] 1단계: overflowed 플래그를 결정 */
			history = spdk_get_per_lcore_history(_trace_file, i);
			if (history == NULL || history->num_entries == 0 || history->entries[0].tsc == 0) {
				/* [한국어] 미사용 lcore이거나 첫 슬롯이 비어있으면 — 이 lcore는 wrap 없음, 다음 lcore로 */
				continue;
			}
			entry_num = history->num_entries - 1;
			overflowed = true;       /* [한국어] 일단 wrap 가정 후 끝쪽에 0(미사용) 슬롯이 있는지 확인 */
			while (entry_num >= 0) {
				if (history->entries[entry_num].tsc == 0) {
					overflowed = false;  /* [한국어] 끝에 0이 있으니 아직 한 바퀴 돌지 않음 */
					break;
				}
				entry_num--;
			}
			if (overflowed) {        /* [한국어] 한 lcore라도 wrap이 발견되면 전체 overflowed 처리 후 빠르게 break */
				break;
			}

		}
		for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {  /* [한국어] 2단계: 모든 lcore에 대해 populate_events 호출 */
			history = spdk_get_per_lcore_history(_trace_file, i);
			if (history == NULL || history->num_entries == 0 || history->entries[0].tsc == 0) {
				continue;  /* [한국어] 빈 lcore 스킵 */
			}
			populate_events(history, history->num_entries, overflowed);
		}
	} else {
		/* [한국어] 단일 lcore 모드 — 지정된 lcore의 history만 처리 */
		history = spdk_get_per_lcore_history(_trace_file, opts->lcore);
		if (history == NULL) {
			SPDK_ERRLOG("Trace file %s has no trace history for lcore %d\n",
				    opts->filename, opts->lcore);
			return false;
		}
		if (history->num_entries > 0 && history->entries[0].tsc != 0) {
			/* [한국어] 단일 lcore에서는 cross-lcore overflow 보정이 필요 없으므로 false 전달 */
			populate_events(history, history->num_entries, false);
		}
	}

	_iter = _entries.begin();   /* [한국어] next_entry()가 처음부터 소비할 수 있도록 iterator 초기화 */
	return true;
}

/*
 * [한국어]
 * spdk_trace_parser::cleanup — mmap 해제 + fd close. 실패한 init에서 부분 자원만 살아있는
 * 상황을 가정하므로 NULL/음수 가드를 둔다. 소멸자에서 항상 호출.
 */
void
spdk_trace_parser::cleanup()
{
	if (_trace_file != NULL) {  /* [한국어] mmap이 성공한 경우에만 munmap (MAP_FAILED는 NULL로 표시되어 있음) */
		munmap(_trace_file, _map_size);
	}

	if (_fd > 0) {              /* [한국어] open/shm_open이 성공한 경우에만 close. -1/0 방어 */
		close(_fd);
	}
}

/*
 * [한국어]
 * 생성자 — 멤버를 안전한 기본값으로 초기화한 뒤 init() 호출.
 *
 * @opts: 호출자가 제공한 옵션. 생성자 도중 init() 실패 시 cleanup 후 throw.
 *
 * 호출 체인: spdk_trace_parser_init(C API) → new spdk_trace_parser(opts).
 * exception이 throw되면 C API는 catch(...)로 잡아 NULL을 반환 — C 호출자에게는
 * 단순히 "init 실패"로 보인다.
 */
spdk_trace_parser::spdk_trace_parser(const spdk_trace_parser_opts *opts) :
	_trace_file(NULL),  /* [한국어] mmap 전 안전 기본값 — cleanup의 NULL 가드와 짝 */
	_map_size(0),
	_fd(-1),            /* [한국어] open 전 안전 기본값 — cleanup의 음수 가드와 짝 */
	_tsc_offset(0)
{
	if (!init(opts)) {
		cleanup();           /* [한국어] 부분 초기화 자원 회수 후 */
		throw std::exception();  /* [한국어] 생성자 실패 — RAII가 멤버(std::map 등)는 자동 정리 */
	}
}

/*
 * [한국어]
 * 소멸자 — RAII 정리. spdk_trace_parser_cleanup이 delete를 호출하면 실행.
 */
spdk_trace_parser::~spdk_trace_parser()
{
	cleanup();
}

/*
 * [한국어]
 * spdk_trace_parser_init — C API 진입점. 옵션을 받아 파서 객체를 new로 생성.
 *
 * @opts: SHM/파일 모드, 파일명, 대상 lcore.
 * @return: 성공 시 파서 핸들(불투명 포인터), 실패 시 NULL.
 *
 * 생성자에서 throw되면 catch(...)로 잡아 NULL 반환 — C 호출자에 예외가 새지 않도록.
 */
struct spdk_trace_parser *
spdk_trace_parser_init(const struct spdk_trace_parser_opts *opts)
{
	try {
		return new spdk_trace_parser(opts);  /* [한국어] new — std::bad_alloc도 catch(...)에서 흡수 */
	} catch (...) {
		return NULL;
	}
}

/*
 * [한국어]
 * spdk_trace_parser_cleanup — C API. delete로 소멸자 호출.
 *
 * @parser: spdk_trace_parser_init이 반환한 핸들. NULL은 호출자 책임으로 가드 안 함(delete NULL은 안전).
 */
void
spdk_trace_parser_cleanup(struct spdk_trace_parser *parser)
{
	delete parser;  /* [한국어] delete NULL은 표준상 no-op이므로 NULL 가드 불필요 */
}

/*
 * [한국어]
 * spdk_trace_parser_get_file — 매핑된 trace 파일 헤더를 노출한다(read-only).
 *
 * 호출자(app/trace 등)는 file()->tpoint_map[], file()->num_lcores, file()->tsc_rate 등으로
 * tpoint 이름 테이블, lcore 수, TSC→실시간 변환 비율을 읽는다.
 */
const struct spdk_trace_file *
spdk_trace_parser_get_file(const struct spdk_trace_parser *parser)
{
	return parser->file();
}

/*
 * [한국어]
 * spdk_trace_parser_get_tsc_offset — populate_events가 계산한 cross-lcore 컷오프 TSC 반환.
 * 호출자는 이 값을 빼서 0-기준 표시하거나, 이 이전 이벤트는 출력에서 제외한다.
 */
uint64_t
spdk_trace_parser_get_tsc_offset(const struct spdk_trace_parser *parser)
{
	return parser->tsc_offset();
}

/*
 * [한국어]
 * spdk_trace_parser_next_entry — C API. 시간 순 다음 엔트리를 entry에 채운다.
 * @return: true면 entry가 채워짐, false면 스트림 종료/오류.
 */
bool
spdk_trace_parser_next_entry(struct spdk_trace_parser *parser,
			     struct spdk_trace_parser_entry *entry)
{
	return parser->next_entry(entry);
}

/*
 * [한국어]
 * spdk_trace_parser_get_entry_count — 특정 lcore의 환형 버퍼 슬롯 수.
 * 호출자는 lcore별 통계 표시(점유율, 비율) 등에 활용한다.
 */
uint64_t
spdk_trace_parser_get_entry_count(const struct spdk_trace_parser *parser, uint16_t lcore)
{
	return parser->entry_count(lcore);
}
