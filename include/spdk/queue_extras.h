/*-
 * Copyright (c) 1991, 1993
 * Copyright (C) 2015 Intel Corporation.
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)queue.h	8.5 (Berkeley) 8/20/94
 * $FreeBSD$
 */

/*
 * [한국어 설명] BSD sys/queue.h 누락 매크로 보충 — SPDK 자체 확장 (queue_extras.h)
 *
 * === 파일의 역할 ===
 * FreeBSD `sys/queue.h`는 4종 자료구조(SLIST, LIST, STAILQ, TAILQ) 각각에
 * 대해 INSERT, REMOVE 외에도 SAFE foreach, FOREACH_FROM, REVERSE foreach,
 * SWAP, LAST 등 수십 종의 편의 매크로를 풍부하게 정의한다. 그러나 리눅스
 * glibc가 제공하는 `<sys/queue.h>`는 그 중 일부(특히 SAFE 변형, REVERSE
 * 변형, SWAP, LAST, FOREACH_FROM)만 누락한 채 배포된다. SPDK는 원래
 * FreeBSD 코드 베이스에서 출발해 리눅스로 포팅된 역사가 있어 이 누락된
 * 매크로들을 자유롭게 사용하므로, 본 헤더가 리눅스 빌드에서 누락분을
 * FreeBSD 원본과 동일한 시그니처/의미로 재정의해 호환성을 메운다. 즉 본
 * 파일은 SPDK 자체 신규 자료구조가 아니라, "FreeBSD의 sys/queue.h에서
 * 리눅스가 빠뜨린 매크로만 가져온 보충판"이다.
 *
 * 본 파일이 정의하는 매크로는 기능상 다음 6 그룹으로 나뉜다:
 *   (1) STAILQ 보충: STAILQ_HEAD / _HEAD_INITIALIZER / _EMPTY / _FIRST /
 *       _FOREACH_FROM / _FOREACH_SAFE / _FOREACH_FROM_SAFE / _LAST / _NEXT /
 *       _REMOVE_AFTER / _SWAP
 *   (2) LIST 보충: LIST_HEAD / _HEAD_INITIALIZER / _ENTRY / _EMPTY /
 *       _FIRST / _FOREACH_FROM / _FOREACH_SAFE / _FOREACH_FROM_SAFE /
 *       _NEXT / _PREV / _SWAP
 *   (3) SLIST 보충: SLIST_SWAP / SLIST_FOREACH_SAFE
 *   (4) TAILQ 보충: TAILQ_EMPTY / _FIRST / _FOREACH_FROM / _FOREACH_SAFE /
 *       _FOREACH_FROM_SAFE / _FOREACH_REVERSE_FROM / _FOREACH_REVERSE_SAFE /
 *       _FOREACH_REVERSE_FROM_SAFE / _LAST / _NEXT / _PREV / _SWAP
 *   (5) GCC 13의 -Wdangling-pointer 오탐을 매크로 단위로 억제하는 SPDK
 *       자체 헬퍼 `SPDK_QE_SUPPRESS_WARNINGS` / `SPDK_QE_UNSUPPRESS_WARNINGS`
 *       (queue 매크로의 이중 포인터 관용구가 GCC 13에서 false positive를
 *       유발하는 이슈 #3030 대응).
 *   (6) `QMD_*_CHECK_*` 류의 invariant 검사 매크로 — BSD 커널 빌드에서
 *       `_KERNEL && INVARIANTS`가 정의됐을 때 panic을 일으키는 디버그
 *       매크로이며, SPDK 유저스페이스 빌드에서는 항상 빈 매크로(no-op)
 *       로 정의되어 사실상 컴파일에서 사라진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 상 본 헤더는 `spdk/queue.h`의 `#ifdef __linux__` 분기 안에서
 * 한 번만 #include 된다. 즉 리눅스 빌드에 한해 자동 노출되며, FreeBSD/
 * macOS 빌드에서는 OS가 이미 동일 매크로를 제공하므로 본 헤더는 컴파일
 * 단계에서 아예 끌어 들여지지 않는다. 노출되는 매크로 자체는 SPDK 모든
 * 서브시스템(lib/nvme, lib/bdev, lib/thread, lib/nvmf, module/bdev/* …)
 * 의 intrusive 리스트/큐 조작 코드 거의 어디에서나 등장한다. 매크로는
 * 컴파일러에서 곧바로 인라인 전개되므로 함수 호출 비용/메모리 할당 비용
 * 모두 0 이며, SPDK의 lockless·per-core 모델 위에서 호출자가 스레드 소유권
 * 을 책임지는 형태로 안전하게 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - 본 헤더는 자체적으로 `<stddef.h>` 등을 포함하지 않으나, SPDK 관례상
 *     상위 호출자(`spdk/queue.h` → 그 호출자)가 이미 `spdk/stdinc.h`나
 *     `<sys/queue.h>` 본체를 포함한 상태라 NULL 등의 매크로/타입은 가시화된다.
 *   - `SPDK_CONTAINEROF`: `STAILQ_LAST`와 `LIST_PREV`가 container_of 패턴
 *     으로 구조체 포인터를 복원할 때 사용. `spdk/util.h`에 정의되어 있으며,
 *     본 헤더만 단독으로는 이름이 미정의 상태가 될 수 있어 호출자가
 *     util.h를 함께 가져와야 한다 (SPDK의 stdinc.h 관례가 이를 보장).
 * 의존하는 모듈:
 *   - lib/nvme/*: pending request STAILQ, async event request 풀 STAILQ,
 *     ctrlr global TAILQ, qpair active TAILQ. 다수 매크로가 빈번히 호출.
 *   - lib/bdev/*: registered bdev 체인(TAILQ), 모듈 등록 TAILQ, in-flight
 *     I/O 채널 TAILQ.
 *   - lib/thread/*: spdk_thread global TAILQ, per-thread poller LIST,
 *     pending message STAILQ.
 *   - lib/nvmf/*: subsystem TAILQ, transport별 qpair TAILQ.
 *   - module/bdev/* (nvme, aio, malloc, raid, lvol, …): 거의 모든 모듈이
 *     bdev_io 큐를 TAILQ/STAILQ로 관리.
 * 공유 자료구조: 모든 매크로는 사용자 구조체 안에 박혀 있는 BSD queue
 *   "연결고리 필드"(stqe_next, le_next/le_prev, tqe_next/tqe_prev)를
 *   직접 조작한다. intrusive 방식이므로 별도의 노드 메모리 할당이 없으며,
 *   같은 객체가 다중 큐에 동시에 등록되도록 연결고리 필드를 여러 개 둘
 *   수 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * 본 헤더는 함수가 아닌 매크로만 노출하며, 신규 도우미 구조체로
 * `STAILQ_HEAD(name, type)`(stqh_first/stqh_last 두 필드)와
 * `LIST_HEAD(name, type)`(lh_first 한 필드), `LIST_ENTRY(type)`
 * (le_next/le_prev 두 필드)를 정의한다 — 단, 이들 역시 FreeBSD 원본과
 * 동일한 형태 그대로다. 각 매크로의 의미는 BSD queue(3) 맨페이지와 일치
 * 하며, 본 파일 §"List of macros"의 매트릭스 표가 자료구조별 지원 매크로
 * 일람을 제공한다. 아래 본문은 매크로 단위로 각 매크로의 입출력, 동작,
 * 시간 복잡도, 호출 컨텍스트(thread-safe 여부), SPDK 내 사용 예시,
 * 인접 매크로와의 차이를 한국어 주석으로 기록한다.
 */

#ifndef SPDK_QUEUE_EXTRAS_H
/* [한국어] include 가드 — 본 파일이 한 번만 컴파일되도록 보호. spdk/queue.h
 *  를 통해 간접적으로 여러 번 #include될 가능성이 있으므로 필수. */
#define SPDK_QUEUE_EXTRAS_H
/* [한국어] 가드 심볼 정의. */

#ifdef __cplusplus
/* [한국어] 매크로만 노출하는 헤더지만 SPDK 공개 헤더의 일관된 관례에 따라
 *  extern "C" 블록을 둔다. C++ 코드에서 본 헤더가 (간접) 포함될 경우에도
 *  안전. */
extern "C" {
#endif

/*
 * This file defines four types of data structures: singly-linked lists,
 * singly-linked tail queues, lists and tail queues.
 *
 * A singly-linked list is headed by a single forward pointer. The elements
 * are singly linked for minimum space and pointer manipulation overhead at
 * the expense of O(n) removal for arbitrary elements. New elements can be
 * added to the list after an existing element or at the head of the list.
 * Elements being removed from the head of the list should use the explicit
 * macro for this purpose for optimum efficiency. A singly-linked list may
 * only be traversed in the forward direction.  Singly-linked lists are ideal
 * for applications with large datasets and few or no removals or for
 * implementing a LIFO queue.
 *
 * A singly-linked tail queue is headed by a pair of pointers, one to the
 * head of the list and the other to the tail of the list. The elements are
 * singly linked for minimum space and pointer manipulation overhead at the
 * expense of O(n) removal for arbitrary elements. New elements can be added
 * to the list after an existing element, at the head of the list, or at the
 * end of the list. Elements being removed from the head of the tail queue
 * should use the explicit macro for this purpose for optimum efficiency.
 * A singly-linked tail queue may only be traversed in the forward direction.
 * Singly-linked tail queues are ideal for applications with large datasets
 * and few or no removals or for implementing a FIFO queue.
 *
 * A list is headed by a single forward pointer (or an array of forward
 * pointers for a hash table header). The elements are doubly linked
 * so that an arbitrary element can be removed without a need to
 * traverse the list. New elements can be added to the list before
 * or after an existing element or at the head of the list. A list
 * may be traversed in either direction.
 *
 * A tail queue is headed by a pair of pointers, one to the head of the
 * list and the other to the tail of the list. The elements are doubly
 * linked so that an arbitrary element can be removed without a need to
 * traverse the list. New elements can be added to the list before or
 * after an existing element, at the head of the list, or at the end of
 * the list. A tail queue may be traversed in either direction.
 *
 * For details on the use of these macros, see the queue(3) manual page.
 *
 *
 *				SLIST	LIST	STAILQ	TAILQ
 * _HEAD			+	+	+	+
 * _HEAD_INITIALIZER		+	+	+	+
 * _ENTRY			+	+	+	+
 * _INIT			+	+	+	+
 * _EMPTY			+	+	+	+
 * _FIRST			+	+	+	+
 * _NEXT			+	+	+	+
 * _PREV			-	+	-	+
 * _LAST			-	-	+	+
 * _FOREACH			+	+	+	+
 * _FOREACH_FROM		+	+	+	+
 * _FOREACH_SAFE		+	+	+	+
 * _FOREACH_FROM_SAFE		+	+	+	+
 * _FOREACH_REVERSE		-	-	-	+
 * _FOREACH_REVERSE_FROM	-	-	-	+
 * _FOREACH_REVERSE_SAFE	-	-	-	+
 * _FOREACH_REVERSE_FROM_SAFE	-	-	-	+
 * _INSERT_HEAD			+	+	+	+
 * _INSERT_BEFORE		-	+	-	+
 * _INSERT_AFTER		+	+	+	+
 * _INSERT_TAIL			-	-	+	+
 * _CONCAT			-	-	+	+
 * _REMOVE_AFTER		+	-	+	-
 * _REMOVE_HEAD			+	-	+	-
 * _REMOVE			+	+	+	+
 * _SWAP			+	+	+	+
 *
 */

/* gcc-13 reports bogus Wdangling-pointer warnings on some of the macros below (see issue #3030) */
#if defined(__GNUC__) && (__GNUC__ >= 13)
/* [한국어] GCC 13 이상에서만 활성화되는 분기. GCC 13의 강화된 -Wdangling-
 *  pointer 분석은 BSD queue 매크로가 사용하는 이중 포인터 재연결 패턴
 *  (`*tqe_prev = tqe_next` 등)을 "스택 변수 주소가 외부로 새어나간다"고
 *  잘못 판단해 false positive를 발생시킨다. SPDK 이슈 #3030에 기록되어
 *  있으며, 본 패턴 자체는 BSD에서 30년 넘게 검증된 정상 코드.
 *  대응 전략: SWAP 류 매크로에 한해 _Pragma push/pop으로 해당 경고만
 *  좁은 범위에서 비활성화. 다른 경고와 다른 코드 경로는 영향받지 않는다. */
/*
 * [한국어]
 * SPDK_QE_SUPPRESS_WARNINGS - 현 위치에서 -Wdangling-pointer 경고를 일시 비활성
 *
 * @return: 없음 (do-while(0) 매크로)
 *
 * 동기/배경: GCC 13 false positive 회피. 사용자가 호출 후 반드시 같은
 *   블록 내에서 `SPDK_QE_UNSUPPRESS_WARNINGS()`로 짝 맞춰 복원해야 함.
 * 동작: `_Pragma("GCC diagnostic push")`로 현 경고 설정을 진단 스택에
 *   저장한 뒤, `_Pragma("GCC diagnostic ignored \"-Wdangling-pointer\"")`
 *   로 해당 경고를 끈다.
 * 시간 복잡도: 컴파일 타임 효과만 — 런타임 비용 0.
 * 호출 컨텍스트: 컴파일러 지시이므로 스레드/실행 시점과 무관.
 * 사용 예시: TAILQ_SWAP 매크로 본문 시작/끝에서 사용 (본 파일 끝부분).
 * 인접 매크로와의 차이: GCC <13/Clang에서는 빈 매크로(no-op).
 */
#define SPDK_QE_SUPPRESS_WARNINGS() do {				\
	_Pragma("GCC diagnostic push")					\
        /* [한국어] 진단 설정 스택에 현재 상태를 push — 범위 종료 시 정확히
         *  이 시점 상태로 복원되도록 보장. */                                                \
	_Pragma("GCC diagnostic ignored \"-Wdangling-pointer\"")	\
        /* [한국어] -Wdangling-pointer만 콕 집어 비활성화 — 다른 경고는
         *  영향 없음. 여기서는 SWAP 매크로 안의 이중 포인터 갱신을
         *  정상 코드로 취급시키는 것이 목적. */                                                \
} while (0)
/*
 * [한국어]
 * SPDK_QE_UNSUPPRESS_WARNINGS - 위에서 push한 경고 설정을 복원
 *
 * @return: 없음
 *
 * `SPDK_QE_SUPPRESS_WARNINGS()`와 짝을 이루며 같은 블록에서 호출되어야
 * 한다. 누락 시 진단 스택이 영구히 변경되어 이후 코드의 경고가 눌려질
 * 수 있으므로 위험. `_Pragma("GCC diagnostic pop")` 한 줄.
 * 호출 컨텍스트: 컴파일 타임 — 런타임 영향 없음.
 */
#define SPDK_QE_UNSUPPRESS_WARNINGS() _Pragma("GCC diagnostic pop")
/* [한국어] 진단 스택에서 직전 push 시점의 설정을 복원. 범위가 SWAP 매크로
 *  본문 내부로 한정되어 다른 코드의 경고에 영향이 없도록 만든다. */
#else
/*
 * [한국어]
 * GCC <13 / Clang / 기타 컴파일러: 본 경고 자체가 없거나 false positive를
 * 일으키지 않으므로, 두 헬퍼를 빈 매크로로 정의해 호출 부위에서 분기
 * 없이 그대로 사용할 수 있게 한다.
 */
#define SPDK_QE_SUPPRESS_WARNINGS()
/* [한국어] no-op — 빈 매크로. 컴파일러가 토큰 자체를 제거하므로 비용 0. */
#define SPDK_QE_UNSUPPRESS_WARNINGS()
/* [한국어] no-op — 위와 짝. */
#endif

/*
 * Singly-linked Tail queue declarations.
 */
/*
 * [한국어] ===== STAILQ (Singly-linked TAIL Queue) — 단방향 tail 큐 =====
 * 자료구조 특성:
 *   - head는 (stqh_first, stqh_last) 쌍 포인터를 가져 head 삽입/조회와
 *     tail 삽입을 모두 O(1)로 지원.
 *   - 각 엔트리는 stqe_next(다음 포인터) 한 개만 — 메모리 footprint 최소.
 *   - 양방향 포인터가 없어 임의 위치 제거는 O(n)(head부터 선형 탐색),
 *     반면 head 제거 / tail 추가는 O(1).
 *   - FIFO 큐, lock-free producer-consumer 큐(SPDK는 단일 스레드 소유로
 *     이미 동시성 보호) 구현에 적합.
 * 대표 SPDK 사용처: lib/nvme의 pending request 큐(완료 응답 대기 요청들),
 *   비동기 이벤트 풀, 모듈 콜백 체인.
 */
/*
 * [한국어]
 * STAILQ_HEAD(name, type) - STAILQ 헤드 구조체 선언 매크로
 *
 * @name: 생성될 head 구조체 이름 (예: my_q)
 * @type: 엔트리 객체의 구조체 이름 (예: my_request — 항상 `struct` 키워드 없이)
 * @return: 구조체 정의 그 자체 — 사용 측에서 변수 선언 또는 typedef로 사용
 *
 * 동기/배경: STAILQ 헤드가 가지는 두 필드를 매크로로 자동 정의해 사용자가
 *   타입에 맞춰 손으로 작성하지 않게 한다.
 * 시간 복잡도: 컴파일 타임 매크로 — 런타임 비용 없음.
 * 호출 컨텍스트: 컴파일 타임에만 사용 (전역/지역 변수 선언 또는 구조체
 *   typedef 안에 사용).
 * 사용 예시: `STAILQ_HEAD(spdk_nvme_request_pool, spdk_nvme_request) pool;`
 *   처럼 SPDK의 요청 풀 타입 정의에 빈번 등장.
 * 인접 매크로와의 차이: `LIST_HEAD`는 stqh_last가 없고, `TAILQ_HEAD`는
 *   타입이 다른 tqh_last를 가진다.
 */
#define	STAILQ_HEAD(name, type)						\
struct name {								\
	struct type *stqh_first;/* first element */			\
        /* [한국어] 큐의 첫 엔트리를 가리키는 포인터.
         *  - 설정자: STAILQ_INSERT_HEAD/INIT/REMOVE_HEAD/SWAP 등이 갱신.
         *  - 읽는 자: STAILQ_FIRST/EMPTY/FOREACH 등 모든 조회 매크로.
         *  - 값 범위: NULL(빈 큐) 또는 첫 엔트리의 유효 포인터.
         *  - 동기화: 큐 단위 외부 락 또는 SPDK의 per-core 단일 소유로 보호.
         *    동시 갱신 시 자료구조가 침묵 깨짐. */                                                \
	struct type **stqh_last;/* addr of last next element */		\
        /* [한국어] "마지막 엔트리의 stqe_next 필드 주소"를 보관하는 이중 포인터.
         *  - 설정자: 모든 INSERT_TAIL/REMOVE/SWAP 류 매크로가 갱신.
         *  - 읽는 자: STAILQ_LAST 및 INSERT_TAIL.
         *  - 값 범위: 큐가 비었을 땐 자기 stqh_first 슬롯 주소(=&stqh_first),
         *    아니면 last->stqe_next의 주소.
         *  - 핵심 트릭: 빈 큐일 때도 NULL이 아닌 자기 first의 주소를 가리키게
         *    해 두면, INSERT_TAIL 같은 매크로가 빈/비빈 분기 없이 같은 코드로
         *    처리된다.
         *  - 동기화: 동일 큐 단위로 first와 함께 보호되어야 함. */                                                \
}

/*
 * [한국어]
 * STAILQ_HEAD_INITIALIZER(head) - STAILQ 헤드의 정적 초기화자
 *
 * @head: 정적으로 초기화할 head 변수 (이름)
 * @return: { NULL, &(head).stqh_first } 형태의 중괄호 초기화자
 *
 * 동기/배경: 정적 변수에서 STAILQ_INIT을 호출할 수 없는 자리(파일 스코프
 *   초기화 등)를 위해 컴파일 타임 초기 상태를 제공. stqh_last가 자기
 *   first를 가리키도록 세팅하는 것이 핵심 — 그래야 빈 큐에서도 INSERT_TAIL
 *   이 분기 없이 동작.
 * 시간 복잡도: 컴파일 타임. 호출 컨텍스트: 변수 정의 시점.
 * 사용 예시: `STAILQ_HEAD(...) g_pool = STAILQ_HEAD_INITIALIZER(g_pool);`
 */
#define	STAILQ_HEAD_INITIALIZER(head)					\
	{ NULL, &(head).stqh_first }
/* [한국어] stqh_first=NULL, stqh_last=&head.stqh_first.
 *  - 빈 큐의 불변식: stqh_last가 stqh_first 슬롯 자체를 가리키도록 한다.
 *  - 이 형태 덕에 첫 INSERT_TAIL이 `*head->stqh_last = elm`을 수행하면
 *    head->stqh_first = elm이 되어 자연스럽게 head 갱신과 일반 노드
 *    갱신을 동일 코드로 처리할 수 있다. */

/*
 * Singly-linked Tail queue functions.
 */
/*
 * [한국어]
 * STAILQ_EMPTY(head) - 큐가 비었는지 판정
 *
 * @head: STAILQ_HEAD 변수의 주소
 * @return: bool — first == NULL이면 true
 *
 * 시간 복잡도: O(1). 호출 컨텍스트: 호출자 스레드 — 큐 소유자만.
 * 사용 예시: 모든 STAILQ 사용처의 가드. 인접 매크로와의 차이:
 *   `LIST_EMPTY`는 lh_first 비교, `TAILQ_EMPTY`는 tqh_first 비교 — 의미는
 *   같으나 필드명이 다르다.
 */
#define	STAILQ_EMPTY(head)	((head)->stqh_first == NULL)
/* [한국어] stqh_first 한 번 비교. 빈 큐 표시 불변식(stqh_last가 자기 first
 *  주소)는 검사하지 않으나, INSERT/REMOVE 매크로가 깨뜨리지 않으므로 안전. */

/*
 * [한국어]
 * STAILQ_FIRST(head) - 첫 엔트리 포인터 반환
 *
 * @head: STAILQ_HEAD 주소
 * @return: 첫 엔트리 객체 포인터 (빈 큐면 NULL)
 *
 * 시간 복잡도: O(1). 호출 컨텍스트: 호출자 스레드.
 * 사용 예시: pop 패턴 — `req = STAILQ_FIRST(pool);
 *   STAILQ_REMOVE_HEAD(pool, link);` (요청 객체 풀에서 꺼낼 때).
 * 인접 매크로와의 차이: `LIST_FIRST`/`TAILQ_FIRST`도 의미 동일, 필드만 다름.
 */
#define	STAILQ_FIRST(head)	((head)->stqh_first)
/* [한국어] 단순 필드 접근 — l-value로도 사용 가능(SWAP 매크로가 활용). */

/*
 * [한국어]
 * STAILQ_FOREACH_FROM(var, head, field) - var부터 (또는 head 처음부터) 순회
 *
 * @var:   루프 인덱스용 포인터 변수
 * @head:  STAILQ_HEAD 주소
 * @field: 엔트리 안의 STAILQ_ENTRY 멤버 이름
 *
 * 동기/배경: 표준 STAILQ_FOREACH는 항상 처음부터 도는 반면, FROM 변형은
 *   var이 이미 어떤 엔트리를 가리키고 있다면 거기서부터 시작한다. 중단/
 *   재개 패턴(예: poller가 일부만 처리 후 다음 라운드에 이어가기)에 유용.
 * 동작: var이 NULL이면 STAILQ_FIRST부터, 아니면 그 자리부터 stqe_next로 전진.
 * 시간 복잡도: 순회한 엔트리 수에 비례 (O(k)).
 * 호출 컨텍스트: 큐를 만지지 않는 read-only 순회 권장 — 순회 중 변경은 SAFE
 *   변형 사용. 사용 예시: SPDK retry 큐를 일부만 처리한 뒤 이어가는 로직.
 * 인접 매크로와의 차이: SAFE 변형은 추가 tvar 파라미터로 제거-안전,
 *   FROM_SAFE는 둘 다 수용.
 */
#define	STAILQ_FOREACH_FROM(var, head, field)				\
	for ((var) = ((var) ? (var) : STAILQ_FIRST((head)));		\
	   (var);							\
	   (var) = STAILQ_NEXT((var), field))
/* [한국어] 초기식 `(var) ? (var) : STAILQ_FIRST(head)`: var이 비어 있으면
 *  처음부터, 값이 있으면 그 위치부터 시작. 갱신식은 stqe_next 따라가기.
 *  - 표준 FOREACH는 var을 항상 STAILQ_FIRST로 초기화하므로 차이는 초기식. */

/*
 * [한국어]
 * STAILQ_FOREACH_SAFE(var, head, field, tvar) - 제거 안전 순회
 *
 * @tvar: 다음 엔트리를 미리 보관할 임시 포인터
 *
 * 동기/배경: STAILQ_FOREACH는 var을 가리키는 엔트리를 루프 본문에서 free
 *   하면 다음 반복의 STAILQ_NEXT(var) 평가가 use-after-free가 된다.
 *   SAFE 변형은 본문 진입 전에 다음 엔트리 주소를 tvar에 캐시해 둠으로써
 *   var을 안전하게 제거/free 할 수 있게 한다.
 * 동작: 콤마 연산자 `(tvar = STAILQ_NEXT(var, field), 1)` 로 매 반복 시작
 *   시 tvar에 다음을 미리 저장 후 루프 조건은 항상 1 — 종료는 var의 NULL
 *   체크로만 결정.
 * 시간 복잡도: O(n). 사용 예시: SPDK qpair shutdown 시 요청 큐의 모든
 *   in-flight 요청을 free 하면서 순회.
 * 인접 매크로와의 차이: 표준 FOREACH는 tvar 없음 — 본문에서 제거/free 금지.
 */
#define	STAILQ_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = STAILQ_FIRST((head));				\
	    (var) && ((tvar) = STAILQ_NEXT((var), field), 1);		\
	    (var) = (tvar))
/* [한국어] for 조건식의 두 번째 절에서 tvar 대입과 truthy(1) 판정을 콤마로
 *  결합 — 표준 C 관용구. var이 NULL이 되는 순간 단락 평가로 tvar 대입은
 *  스킵되고 루프 종료. */

/*
 * [한국어]
 * STAILQ_FOREACH_FROM_SAFE(var, head, field, tvar) - FROM + SAFE 합성
 *
 * 위 두 매크로의 의미를 동시에 만족: var이 가리키면 거기서부터, 아니면
 * 처음부터 시작하면서 본문에서 var 제거/free가 안전.
 * 사용 예시: poller가 일부 라운드에서 처리하던 큐를 이어 가면서 처리한
 *   요청을 즉시 free 하는 패턴.
 */
#define	STAILQ_FOREACH_FROM_SAFE(var, head, field, tvar)		\
	for ((var) = ((var) ? (var) : STAILQ_FIRST((head)));		\
	    (var) && ((tvar) = STAILQ_NEXT((var), field), 1);		\
	    (var) = (tvar))
/* [한국어] FROM의 시작점 선택 + SAFE의 다음 엔트리 캐시를 동시에 적용. */

/*
 * [한국어]
 * STAILQ_LAST(head, type, field) - 마지막 엔트리 포인터 복원
 *
 * @head:  STAILQ_HEAD 주소
 * @type:  엔트리 구조체 이름 (struct 키워드 없이)
 * @field: STAILQ_ENTRY 멤버 이름
 * @return: 마지막 엔트리 포인터 (빈 큐면 NULL)
 *
 * 동기/배경: head는 마지막 엔트리의 next 슬롯 "주소"만 알고 있어 직접
 *   엔트리 포인터를 들고 있지 않다. container_of(=SPDK_CONTAINEROF) 패턴
 *   으로 그 주소를 마지막 엔트리 객체로 역산해야 한다.
 * 동작: 빈 큐면 NULL, 아니면 stqh_last(=last->stqe_next의 주소)에서
 *   stqe_next 멤버 오프셋을 빼 마지막 엔트리 객체 시작 주소를 복원.
 * 시간 복잡도: O(1). 호출 컨텍스트: 큐 소유자.
 * 사용 예시: SPDK가 큐의 마지막 요소만 빠르게 검사할 때 사용.
 * 인접 매크로와의 차이: TAILQ_LAST는 양방향 링크 덕에 더 단순한 캐스팅으로
 *   복원 가능; SLIST/LIST는 head에 last 정보가 없어 본 매크로 자체가 없다.
 */
#define	STAILQ_LAST(head, type, field)					\
	(STAILQ_EMPTY((head)) ? NULL :					\
	    SPDK_CONTAINEROF((head)->stqh_last, struct type, field.stqe_next))
/* [한국어] SPDK_CONTAINEROF: spdk/util.h 제공 — Linux kernel의 container_of
 *  와 동등한 매크로. (포인터, 구조체타입, 멤버) → 구조체 시작 주소를 반환.
 *  - 여기서 stqh_last는 last->field.stqe_next의 주소이므로, container_of로
 *    last 객체의 시작을 정확히 복원. */

/*
 * [한국어]
 * STAILQ_NEXT(elm, field) - 다음 엔트리 포인터 (l-value 가능)
 *
 * @elm:   현재 엔트리 포인터
 * @field: STAILQ_ENTRY 멤버 이름
 * @return: 다음 엔트리 포인터 또는 NULL (마지막일 때)
 *
 * 시간 복잡도: O(1). 호출 컨텍스트: 큐 소유자.
 * 사용 예시: 거의 모든 FOREACH 매크로 내부와 사용자 코드 양쪽에서 호출.
 * 인접 매크로와의 차이: LIST_NEXT/TAILQ_NEXT도 의미 동일, 필드명만 다름.
 *   STAILQ는 prev 포인터가 없어 STAILQ_PREV 매크로 자체가 없음.
 */
#define	STAILQ_NEXT(elm, field)	((elm)->field.stqe_next)
/* [한국어] 단순 필드 접근. l-value로도 쓸 수 있어 INSERT/REMOVE 매크로 본문
 *  에서 좌변으로 등장. */

/*
 * [한국어]
 * STAILQ_REMOVE_AFTER(head, elm, field) - elm 다음 엔트리를 큐에서 제거
 *
 * @head:  STAILQ_HEAD 주소
 * @elm:   기준이 되는 엔트리 (이 다음 노드가 제거 대상)
 * @field: STAILQ_ENTRY 멤버 이름
 * @return: 없음 (제거된 엔트리 자체는 호출자가 들고 있어야 함)
 *
 * 동기/배경: STAILQ는 단방향이라 임의 노드를 제거하려면 직전 노드를 알아야
 *   한다. 호출자가 이미 직전 노드(elm)를 알고 있을 때 O(1) 제거를 가능
 *   하게 하는 매크로.
 * 동작:
 *   1) `elm.next = elm.next.next` — elm 다음 노드를 우회.
 *   2) 제거된 노드가 tail이었으면(=새로운 elm.next가 NULL) head의
 *      stqh_last를 elm.next의 주소로 갱신해 tail 추적 슬롯을 정정.
 * 시간 복잡도: O(1). 호출 컨텍스트: 큐 소유자.
 * 사용 예시: 정렬된 STAILQ에서 매칭 노드의 직전을 알고 있을 때 빠르게 제거.
 * 인접 매크로와의 차이: 일반 STAILQ_REMOVE는 head부터 선형 탐색해 직전을
 *   찾으므로 O(n).
 */
#define STAILQ_REMOVE_AFTER(head, elm, field) do {			\
	if ((STAILQ_NEXT(elm, field) =					\
	     STAILQ_NEXT(STAILQ_NEXT(elm, field), field)) == NULL)	\
        /* [한국어] elm.next = elm.next.next로 한 번에 우회.
         *  - 대입식의 결과(NULL인지 비교)를 그대로 if 조건으로 사용 — 제거
         *    대상이 tail이었던 경우만 다음 줄 실행. */                                                \
		(head)->stqh_last = &STAILQ_NEXT((elm), field);		\
        /* [한국어] tail 갱신: 이제 elm이 새로운 마지막이므로 head->stqh_last
         *  는 elm.stqe_next 슬롯의 주소를 가리켜야 한다. */                                                \
} while (0)

/*
 * [한국어]
 * STAILQ_SWAP(head1, head2, type) - 두 STAILQ의 내용을 교환
 *
 * @head1, @head2: 교환할 두 STAILQ_HEAD 주소
 * @type:          엔트리 구조체 이름 (struct 키워드 없이)
 * @return: 없음
 *
 * 동기/배경: 큐 두 개의 내용을 통째로 맞바꾸는 연산. SPDK에서는 reactor
 *   round 처리 시 "현재 라운드 큐"와 "다음 라운드 큐"를 swap 하거나, 임시
 *   큐와 메인 큐를 한 번에 교체하는 패턴에서 사용.
 * 동작:
 *   1) head1의 (first, last)를 임시에 백업.
 *   2) head1에 head2의 내용을 복사.
 *   3) head2에 백업한 head1의 내용을 복사.
 *   4) 비어진 쪽이 있다면 stqh_last가 자기 first 슬롯을 가리키도록 정정
 *      (빈 큐 불변식 유지).
 * 시간 복잡도: O(1). 호출 컨텍스트: 두 큐의 단일 소유자(같은 스레드).
 * 사용 예시: SPDK NVMe 드라이버 — async event 처리 단계에서 in-flight 큐
 *   와 retry 큐를 swap.
 * 인접 매크로와의 차이: LIST_SWAP은 첫 엔트리의 le_prev 정정도 함께,
 *   TAILQ_SWAP은 양방향 링크 정정.
 */
#define STAILQ_SWAP(head1, head2, type) do {				\
	struct type *swap_first = STAILQ_FIRST(head1);			\
        /* [한국어] head1의 첫 엔트리를 임시 백업 — 이후 head1을 덮어쓸
         *  때 잃지 않도록. */                                                \
	struct type **swap_last = (head1)->stqh_last;			\
        /* [한국어] head1의 stqh_last(이중 포인터) 백업. */                                                \
	STAILQ_FIRST(head1) = STAILQ_FIRST(head2);			\
        /* [한국어] head1에 head2의 first를 이식. */                                                \
	(head1)->stqh_last = (head2)->stqh_last;			\
        /* [한국어] head1에 head2의 stqh_last 이식. */                                                \
	STAILQ_FIRST(head2) = swap_first;				\
        /* [한국어] head2에 백업해둔 head1의 first 이식. */                                                \
	(head2)->stqh_last = swap_last;					\
        /* [한국어] head2에 백업해둔 head1의 stqh_last 이식. */                                                \
	if (STAILQ_EMPTY(head1))					\
        /* [한국어] swap 결과 head1이 비었다면 — head2가 원래 비어있던 경우. */                                                \
		(head1)->stqh_last = &STAILQ_FIRST(head1);		\
        /* [한국어] 빈 큐 불변식 복구: stqh_last가 head1의 first 슬롯 주소를
         *  가리키도록 한다. 안 그러면 head2의 stqh_last(=head2.first 주소)가
         *  남아 있어 이후 INSERT_TAIL이 head2의 first 슬롯에 써넣는 침묵
         *  깨짐이 발생. */                                                \
	if (STAILQ_EMPTY(head2))					\
		(head2)->stqh_last = &STAILQ_FIRST(head2);		\
        /* [한국어] head2도 동일하게 빈 큐 불변식 복구. */                                                \
} while (0)

/*
 * List declarations.
 */
/*
 * [한국어] ===== LIST (doubly-linked list, head-only 포인터) =====
 * 자료구조 특성:
 *   - head는 lh_first 한 개만 보유 → tail O(1) 접근 불가(필요하면 TAILQ).
 *   - 각 엔트리는 le_next + le_prev(이중 포인터) 보유 → 임의 위치 제거가
 *     O(1)로 가능 (BSD queue.h의 le_prev는 "이전 노드의 next 슬롯 주소"
 *     라는 이중 포인터 관용구를 사용해 head/일반 노드의 분기를 제거).
 *   - 양방향 순회 가능, FIFO 보다는 hash bucket·관리 리스트에 적합.
 * 대표 SPDK 사용처: lib/thread의 per-thread poller LIST, hash bucket 체인,
 *   bdev의 모듈별 로컬 등록 리스트.
 */
/*
 * [한국어]
 * LIST_HEAD(name, type) - LIST 헤드 구조체 선언
 *
 * @name: 생성될 head 구조체 이름
 * @type: 엔트리 구조체 이름 (struct 키워드 없이)
 *
 * 동기/배경: tail 추적 없이 첫 엔트리만 알고 싶을 때 메모리를 아끼는 head.
 * 시간 복잡도/컨텍스트: 컴파일 타임 매크로 — 런타임 비용 0.
 * 사용 예시: `LIST_HEAD(spdk_poller_list, spdk_poller) pollers;` (lib/thread).
 * 인접 매크로와의 차이: STAILQ_HEAD/TAILQ_HEAD는 last 슬롯을 추가로 가짐.
 */
#define	LIST_HEAD(name, type)						\
struct name {								\
	struct type *lh_first;	/* first element */			\
        /* [한국어] 첫 엔트리 포인터.
         *  - 설정자: LIST_INSERT_HEAD/INIT/REMOVE 등이 이중 포인터 관용구를
         *    통해 갱신. 일반 노드의 le_prev는 이 필드 자체의 주소를 가리킨다.
         *  - 읽는 자: LIST_FIRST/EMPTY/FOREACH 등.
         *  - 값 범위: NULL(빈 리스트) 또는 첫 엔트리 포인터.
         *  - 동기화: 큐 단위 외부 락 또는 SPDK의 per-core 단일 소유. */                                                \
}

/*
 * [한국어]
 * LIST_HEAD_INITIALIZER(head) - LIST 헤드의 정적 초기화자
 *
 * @head: 초기화 대상 head 변수 이름 (실제로는 사용되지 않으나 타 매크로와의
 *        시그니처 통일을 위해 받음)
 * @return: { NULL }
 *
 * STAILQ/TAILQ와 달리 first 슬롯만 NULL로 두면 끝 — last 슬롯이 없기 때문.
 */
#define	LIST_HEAD_INITIALIZER(head)					\
	{ NULL }
/* [한국어] 빈 LIST head 정적 초기화 — lh_first = NULL 한 개. */

/*
 * [한국어]
 * LIST_ENTRY(type) - LIST 연결고리 구조체 선언
 *
 * @type: 호스트 엔트리의 구조체 이름
 *
 * 사용자 구조체 안에 `LIST_ENTRY(my_type) link;` 형태로 박아 넣어 두 멤버
 * (le_next, le_prev)를 자동으로 추가한다. intrusive 방식이라 별도 노드
 * 할당이 없다.
 * 시간 복잡도/컨텍스트: 컴파일 타임 매크로.
 * 사용 예시: `struct spdk_poller { LIST_ENTRY(spdk_poller) tailq; ... };`.
 * 인접 매크로와의 차이: STAILQ_ENTRY는 stqe_next 한 개만, TAILQ_ENTRY는
 *   tqe_next + tqe_prev (필드명 다름).
 */
#define	LIST_ENTRY(type)						\
struct {								\
	struct type *le_next;	/* next element */			\
        /* [한국어] 다음 엔트리 포인터.
         *  - 설정자: INSERT_BEFORE/AFTER/HEAD 시 갱신. REMOVE 시 직전 노드의
         *    le_prev가 본 슬롯의 주소를 통해 우회 갱신된다.
         *  - 읽는 자: LIST_NEXT, LIST_FOREACH 류.
         *  - 값 범위: NULL(마지막 엔트리) 또는 다음 엔트리 포인터. */                                                \
	struct type **le_prev;	/* address of previous next element */	\
        /* [한국어] "이전 엔트리의 le_next 필드 주소" 또는 head가 첫 엔트리일
         *  경우 head의 lh_first 슬롯 주소.
         *  - 설정자: INSERT/REMOVE/SWAP 시 갱신.
         *  - 읽는 자: LIST_REMOVE 본문, LIST_PREV 매크로.
         *  - 값 범위: 항상 non-NULL — 비어 있는 객체에 본 필드를 사용하기
         *    전에 반드시 INIT 또는 INSERT가 선행되어야 한다.
         *  - 핵심 트릭: head/일반 노드 모두 대상 슬롯 주소를 들고 있어
         *    `*le_prev = ...` 한 줄로 "이전이 가리키는 next 슬롯"을 갱신
         *    가능 — 분기 없이 head 갱신과 일반 노드 갱신을 통일. */                                                \
}

/*
 * List functions.
 */

#if (defined(_KERNEL) && defined(INVARIANTS))
/* [한국어] BSD 커널에서 `_KERNEL && INVARIANTS` 매크로가 정의된 디버그
 *  빌드일 때만 LIST 자료구조 무결성 검사를 활성화. SPDK 유저스페이스
 *  빌드에서는 두 매크로 모두 정의되지 않으므로 이 분기는 컴파일 단계에서
 *  사라지고, 아래 #else의 빈 매크로가 사용된다.
 *  본 분기를 그대로 둔 이유는 FreeBSD 원본과 100% 동일한 헤더 형태를
 *  유지하기 위함. */
/*
 * [한국어]
 * QMD_LIST_CHECK_HEAD(head, field) - LIST head 무결성 검사 (BSD 커널 전용)
 *
 * @head:  LIST_HEAD 주소
 * @field: LIST_ENTRY 멤버 이름
 *
 * 검사: head가 비어있지 않다면 first의 le_prev가 head의 lh_first 슬롯
 * 주소와 일치해야 한다. 불일치 시 BSD 커널의 panic()으로 시스템 정지.
 * SPDK 유저스페이스 빌드에서는 호출 자체가 사라진다.
 */
#define	QMD_LIST_CHECK_HEAD(head, field) do {				\
	if (LIST_FIRST((head)) != NULL &&				\
	    LIST_FIRST((head))->field.le_prev !=			\
	     &LIST_FIRST((head)))					\
        /* [한국어] head 비어있지 않고 first.le_prev가 head.lh_first 주소가
         *  아니면 — 자료구조 깨짐. */                                                \
		panic("Bad list head %p first->prev != head", (head));	\
        /* [한국어] BSD 커널 panic() — 유저스페이스에서는 호출 불가. */                                                \
} while (0)

/*
 * [한국어]
 * QMD_LIST_CHECK_NEXT(elm, field) - 다음 엔트리의 le_prev 검증 (BSD 커널 전용)
 *
 * elm.next.le_prev 가 elm.le_next 슬롯의 주소를 가리켜야 정상.
 */
#define	QMD_LIST_CHECK_NEXT(elm, field) do {				\
	if (LIST_NEXT((elm), field) != NULL &&				\
	    LIST_NEXT((elm), field)->field.le_prev !=			\
	     &((elm)->field.le_next))					\
        /* [한국어] elm 다음이 존재하고 그 le_prev가 elm.le_next 주소가
         *  아니면 자료구조 깨짐. */                                                \
		panic("Bad link elm %p next->prev != elm", (elm));	\
} while (0)

/*
 * [한국어]
 * QMD_LIST_CHECK_PREV(elm, field) - 이전 엔트리의 next 검증 (BSD 커널 전용)
 *
 * `*elm.le_prev`(이전이 가리키는 next 슬롯의 값) 이 정확히 elm 자기
 * 자신이어야 정상.
 */
#define	QMD_LIST_CHECK_PREV(elm, field) do {				\
	if (*(elm)->field.le_prev != (elm))				\
        /* [한국어] 이전 노드의 next 슬롯이 elm을 가리켜야 함 — 그렇지
         *  않으면 elm은 양방향 링크의 한쪽이 끊어진 비정상 상태. */                                                \
		panic("Bad link elm %p prev->next != elm", (elm));	\
} while (0)
#else
/* [한국어] SPDK 유저스페이스 빌드에서 도달하는 분기 — 세 매크로를 모두
 *  빈 토큰으로 정의해 호출자 코드가 분기 없이 같은 매크로 이름을 쓸 수
 *  있게 한다. 컴파일 후 코드에서 완전히 사라진다. */
#define	QMD_LIST_CHECK_HEAD(head, field)
#define	QMD_LIST_CHECK_NEXT(elm, field)
#define	QMD_LIST_CHECK_PREV(elm, field)
/* [한국어] 세 매크로 모두 no-op — SPDK 유저스페이스에서는 invariant 검사
 *  대신 assert/디버거에 의존. */
#endif /* (_KERNEL && INVARIANTS) */

/*
 * [한국어]
 * LIST_EMPTY(head) - 빈 리스트 판정
 *
 * @head: LIST_HEAD 주소
 * @return: bool — lh_first == NULL이면 true.
 * 시간 복잡도: O(1). 호출 컨텍스트: 큐 소유자.
 */
#define	LIST_EMPTY(head)	((head)->lh_first == NULL)
/* [한국어] lh_first 한 번 비교. */

/*
 * [한국어]
 * LIST_FIRST(head) - 첫 엔트리 포인터 반환
 *
 * @head: LIST_HEAD 주소. 빈 리스트면 NULL.
 * 시간 복잡도: O(1). 사용 예시: 모든 LIST 사용처의 진입.
 * 인접 매크로와의 차이: l-value로도 사용 가능 (LIST_SWAP이 활용).
 */
#define	LIST_FIRST(head)	((head)->lh_first)
/* [한국어] lh_first 필드 직접 접근. */

/*
 * [한국어]
 * LIST_FOREACH_FROM(var, head, field) - var부터 (또는 처음부터) 순회
 *
 * STAILQ_FOREACH_FROM과 동일 패턴: var이 NULL이면 처음, 아니면 그 자리에서
 * 시작. 본문에서 var을 제거하면 안 됨(SAFE 변형 사용).
 */
#define	LIST_FOREACH_FROM(var, head, field)				\
	for ((var) = ((var) ? (var) : LIST_FIRST((head)));		\
	    (var);							\
	    (var) = LIST_NEXT((var), field))
/* [한국어] 초기식이 var 유무에 따라 시작점을 정한다 — STAILQ 동일 형태. */

/*
 * [한국어]
 * LIST_FOREACH_SAFE(var, head, field, tvar) - 제거 안전 순회
 *
 * 본문에서 var을 free/제거 가능. tvar에 다음 엔트리를 미리 저장해 둔다.
 * 사용 예시: spdk_thread 종료 시 등록된 모든 poller를 LIST_REMOVE +
 *   free 하면서 순회.
 */
#define	LIST_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = LIST_FIRST((head));				\
	    (var) && ((tvar) = LIST_NEXT((var), field), 1);		\
	    (var) = (tvar))
/* [한국어] 콤마 연산자 관용구 — STAILQ_FOREACH_SAFE와 동일 형태. */

/*
 * [한국어]
 * LIST_FOREACH_FROM_SAFE(var, head, field, tvar) - FROM + SAFE 합성
 *
 * var의 위치부터(없으면 처음부터) 시작하면서 본문에서 var 제거가 안전.
 */
#define	LIST_FOREACH_FROM_SAFE(var, head, field, tvar)			\
	for ((var) = ((var) ? (var) : LIST_FIRST((head)));		\
	    (var) && ((tvar) = LIST_NEXT((var), field), 1);		\
	    (var) = (tvar))
/* [한국어] FROM의 시작점 선택 + SAFE의 다음 엔트리 캐시 결합. */

/*
 * [한국어]
 * LIST_NEXT(elm, field) - 다음 엔트리 포인터 반환 (l-value 가능)
 *
 * @return: 다음 엔트리 또는 NULL (마지막).
 * 시간 복잡도: O(1).
 */
#define	LIST_NEXT(elm, field)	((elm)->field.le_next)
/* [한국어] le_next 필드 접근. INSERT/REMOVE 매크로 본문에서 좌변으로도 등장. */

/*
 * [한국어]
 * LIST_PREV(elm, head, type, field) - 이전 엔트리 포인터 복원
 *
 * @elm:   현재 엔트리 포인터
 * @head:  LIST_HEAD 주소 (elm이 첫 엔트리인지 판정에 필요)
 * @type:  엔트리 구조체 이름
 * @field: LIST_ENTRY 멤버 이름
 * @return: 이전 엔트리 포인터 또는 NULL (elm이 첫 엔트리일 때)
 *
 * 동기/배경: LIST의 le_prev는 "이전의 le_next 슬롯 주소"이므로 직접 이전
 *   엔트리 포인터로 사용할 수 없다. elm이 첫 엔트리라면 le_prev는 head의
 *   lh_first 슬롯 주소이므로 head 주소와 비교해 첫 엔트리 여부를 판정한 뒤,
 *   아니라면 container_of로 이전 노드를 복원.
 * 시간 복잡도: O(1). 사용 예시: 양방향 LIST 순회를 외부에서 구현할 때.
 * 인접 매크로와의 차이: TAILQ_PREV는 head 안의 tqh_last 트릭으로 더 단순한
 *   캐스팅으로 복원. STAILQ는 prev 자체가 없어 본 매크로 부재.
 */
#define	LIST_PREV(elm, head, type, field)				\
	((elm)->field.le_prev == &LIST_FIRST((head)) ? NULL :		\
	    SPDK_CONTAINEROF((elm)->field.le_prev, struct type, field.le_next))
/* [한국어] 1) elm.le_prev가 head.lh_first 슬롯 주소면 elm은 첫 엔트리 →
 *   이전이 없으므로 NULL.
 *  2) 아니라면 le_prev는 이전 엔트리의 le_next 슬롯 주소 → SPDK_CONTAINEROF
 *   (Linux container_of와 동등)로 그 슬롯을 가진 구조체의 시작 주소를 복원. */

/*
 * [한국어]
 * LIST_SWAP(head1, head2, type, field) - 두 LIST의 내용을 교환
 *
 * @head1, @head2: 교환할 두 LIST_HEAD 주소
 * @type:          엔트리 구조체 이름
 * @field:         LIST_ENTRY 멤버 이름
 *
 * 동기/배경: LIST는 head가 lh_first 하나뿐이라 swap 자체는 단순하지만,
 *   각 리스트의 첫 엔트리가 들고 있던 le_prev가 "옛 head의 lh_first 주소"
 *   를 가리키고 있어 head 교환 후에도 새 head의 주소로 정정해야 양방향
 *   링크가 깨지지 않는다.
 * 동작:
 *   1) head1.first ↔ head2.first 단순 교환 (lh_first 한 필드만).
 *   2) head1의 새 first가 NULL 아니면 그 le_prev를 &head1.lh_first로 정정.
 *   3) head2의 새 first에 대해서도 동일 정정.
 * 시간 복잡도: O(1). 호출 컨텍스트: 두 리스트의 단일 소유자(같은 스레드).
 * 사용 예시: hash bucket 재해싱, 임시 작업 리스트와 대기 리스트 교체.
 */
#define LIST_SWAP(head1, head2, type, field) do {			\
	struct type *swap_tmp = LIST_FIRST((head1));			\
        /* [한국어] head1.first 백업 — 이후 head1을 덮어쓸 때 잃지 않도록. */                                                \
	LIST_FIRST((head1)) = LIST_FIRST((head2));			\
        /* [한국어] head1.first에 head2.first 이식. */                                                \
	LIST_FIRST((head2)) = swap_tmp;					\
        /* [한국어] head2.first에 백업해둔 head1.first 이식. */                                                \
	if ((swap_tmp = LIST_FIRST((head1))) != NULL)			\
        /* [한국어] head1의 새 first가 NULL이 아니면 — head2가 비어 있지
         *  않았다는 뜻. 그 엔트리의 le_prev를 새 head 주소로 정정해야
         *  양방향 링크 일관성 유지. */                                                \
		swap_tmp->field.le_prev = &LIST_FIRST((head1));		\
        /* [한국어] new_first.le_prev = &head1.lh_first — 첫 엔트리의 prev는
         *  반드시 head의 lh_first 슬롯 주소를 가리켜야 한다는 LIST 불변식. */                                                \
	if ((swap_tmp = LIST_FIRST((head2))) != NULL)			\
		swap_tmp->field.le_prev = &LIST_FIRST((head2));		\
        /* [한국어] head2 측도 동일 정정. */                                                \
} while (0)

/*
 * Singly-linked List functions.
 */
/*
 * [한국어] ===== SLIST (Singly-linked LIST) 확장 =====
 * 자료구조 특성:
 *   - head는 sl_first 한 개, 각 엔트리는 sle_next 한 개 — 가장 가벼운
 *     intrusive 리스트.
 *   - tail O(1) 접근 불가, 임의 제거 O(n)(직전 노드를 알아야 SLIST_REMOVE_AFTER
 *     로 O(1) 가능), LIFO 스택과 자유 리스트(free pool)에 적합.
 * 대표 SPDK 사용처: 메모리 풀의 free list, 작은 hash bucket 체인.
 */
/*
 * [한국어]
 * SLIST_SWAP(head1, head2, type) - 두 SLIST의 내용을 교환
 *
 * @head1, @head2: 두 SLIST_HEAD 주소
 * @type:          엔트리 구조체 이름
 *
 * 동기/배경: SLIST는 le_prev/last가 없어 swap이 sl_first 한 필드만 교환
 *   하면 끝 — LIST/STAILQ/TAILQ보다 훨씬 단순. 첫 엔트리의 prev 정정 같은
 *   후처리 자체가 필요 없다.
 * 시간 복잡도: O(1). 호출 컨텍스트: 두 리스트의 단일 소유자.
 * 사용 예시: 자유 리스트와 사용 중 리스트 교환.
 * 인접 매크로와의 차이: LIST_SWAP은 첫 엔트리 le_prev 정정 추가, STAILQ_SWAP은
 *   stqh_last 정정 추가, TAILQ_SWAP은 둘 다.
 */
#define	SLIST_SWAP(head1, head2, type) do {			\
	struct type *swap_tmp = SLIST_FIRST((head1));			\
        /* [한국어] head1의 첫 엔트리 백업. */                                                \
	SLIST_FIRST((head1)) = SLIST_FIRST((head2));			\
        /* [한국어] head1.first에 head2.first 이식. */                                                \
	SLIST_FIRST((head2)) = swap_tmp;				\
        /* [한국어] head2.first에 백업해둔 head1.first 이식. SLIST는 추가
         *  정정이 없어 여기서 끝. */                                                \
} while (0)

/*
 * [한국어]
 * SLIST_FOREACH_SAFE(var, head, field, tvar) - 제거 안전 순회
 *
 * @tvar: 다음 엔트리 임시 보관용 포인터.
 *
 * 동기/배경: SLIST는 단방향이라 본문에서 var을 free하면 다음 SLIST_NEXT(var)
 *   평가가 use-after-free. 다음 엔트리를 미리 캐시해 둠.
 * 시간 복잡도: O(n). 호출 컨텍스트: 큐 소유자.
 * 사용 예시: 자유 리스트 정리 — 모든 free 객체를 순회하며 메모리 해제.
 * 인접 매크로와의 차이: 표준 SLIST_FOREACH는 tvar 없음 — 제거 금지.
 */
#define	SLIST_FOREACH_SAFE(var, head, field, tvar)		\
	for ((var) = SLIST_FIRST((head));				\
	    (var) && ((tvar) = SLIST_NEXT((var), field), 1);		\
	    (var) = (tvar))
/* [한국어] STAILQ/LIST_FOREACH_SAFE와 동일한 콤마 연산자 패턴. */

/*
 * Tail queue functions.
 */
/*
 * [한국어] ===== TAILQ (doubly-linked TAIL Queue) 확장 =====
 * 자료구조 특성:
 *   - head는 (tqh_first, tqh_last) 쌍 포인터 + 각 엔트리는 (tqe_next,
 *     tqe_prev) 두 포인터 — 4종 중 가장 풍부한 구조.
 *   - head/tail O(1) 접근, 임의 위치 O(1) 제거, 양방향 순회 가능.
 *   - SPDK에서 가장 널리 쓰이는 자료구조 — 거의 모든 모듈이 TAILQ로 객체
 *     체인을 관리한다 (NVMe ctrlr/qpair, bdev/bdev_io, spdk_thread 등).
 */
#if (defined(_KERNEL) && defined(INVARIANTS))
/* [한국어] BSD 커널의 INVARIANTS 디버그 빌드일 때만 TAILQ invariant 검사
 *  매크로가 panic 호출 형태로 정의된다. SPDK 유저스페이스 빌드에서는
 *  아래 #else 분기의 빈 매크로가 사용되어 비용 0. */
/*
 * [한국어]
 * QMD_TAILQ_CHECK_HEAD(head, field) - TAILQ head 무결성 검사 (BSD 커널 전용)
 *
 * first.tqe_prev가 head.tqh_first 주소와 일치해야 정상.
 */
#define	QMD_TAILQ_CHECK_HEAD(head, field) do {				\
	if (!TAILQ_EMPTY(head) &&					\
	    TAILQ_FIRST((head))->field.tqe_prev !=			\
	     &TAILQ_FIRST((head)))					\
        /* [한국어] 비어있지 않은데 first.tqe_prev가 head 슬롯 주소가 아니면
         *  자료구조 깨짐. */                                                \
		panic("Bad tailq head %p first->prev != head", (head));	\
} while (0)

/*
 * [한국어]
 * QMD_TAILQ_CHECK_TAIL(head, field) - TAILQ tail 불변식 검사
 *
 * head.tqh_last가 가리키는 슬롯의 값(=last의 tqe_next)은 반드시 NULL이어야.
 */
#define	QMD_TAILQ_CHECK_TAIL(head, field) do {				\
	if (*(head)->tqh_last != NULL)					\
        /* [한국어] last.tqe_next가 non-NULL이면 tail 추적이 깨졌다는 뜻. */                                                \
		panic("Bad tailq NEXT(%p->tqh_last) != NULL", (head));	\
} while (0)

/*
 * [한국어]
 * QMD_TAILQ_CHECK_NEXT(elm, field) - 다음 엔트리의 tqe_prev 검증
 *
 * elm.next.tqe_prev가 elm.tqe_next 슬롯 주소와 일치해야 정상.
 */
#define	QMD_TAILQ_CHECK_NEXT(elm, field) do {				\
	if (TAILQ_NEXT((elm), field) != NULL &&				\
	    TAILQ_NEXT((elm), field)->field.tqe_prev !=			\
	     &((elm)->field.tqe_next))					\
        /* [한국어] 양방향 링크 일관성 검증. */                                                \
		panic("Bad link elm %p next->prev != elm", (elm));	\
} while (0)

/*
 * [한국어]
 * QMD_TAILQ_CHECK_PREV(elm, field) - 이전 엔트리의 next 검증
 *
 * `*elm.tqe_prev`가 elm 자기 자신이어야 정상.
 */
#define	QMD_TAILQ_CHECK_PREV(elm, field) do {				\
	if (*(elm)->field.tqe_prev != (elm))				\
        /* [한국어] 이전이 가리키는 next 슬롯이 elm을 가리키지 않으면 깨짐. */                                                \
		panic("Bad link elm %p prev->next != elm", (elm));	\
} while (0)
#else
/* [한국어] SPDK 유저스페이스 빌드 — 네 매크로 모두 빈 토큰. 컴파일 후
 *  완전히 사라져 런타임 비용 0. */
#define	QMD_TAILQ_CHECK_HEAD(head, field)
#define	QMD_TAILQ_CHECK_TAIL(head, headname)
#define	QMD_TAILQ_CHECK_NEXT(elm, field)
#define	QMD_TAILQ_CHECK_PREV(elm, field)
/* [한국어] 네 invariant 검사 매크로 모두 no-op. */
#endif /* (_KERNEL && INVARIANTS) */

/*
 * [한국어]
 * TAILQ_EMPTY(head) - 빈 TAILQ 판정
 *
 * @return: bool — tqh_first == NULL이면 true.
 * 시간 복잡도: O(1). 사용 예시: 모든 TAILQ 사용처 진입.
 * 인접 매크로와의 차이: STAILQ_EMPTY/LIST_EMPTY와 의미 동일, 필드명만 다름.
 */
#define	TAILQ_EMPTY(head)	((head)->tqh_first == NULL)
/* [한국어] tqh_first 한 번 비교. */

/*
 * [한국어]
 * TAILQ_FIRST(head) - 첫 엔트리 포인터 (l-value 가능)
 *
 * 시간 복잡도: O(1). 사용 예시: pop 패턴, FOREACH 진입.
 */
#define	TAILQ_FIRST(head)	((head)->tqh_first)
/* [한국어] tqh_first 필드 직접 접근. */

/*
 * [한국어]
 * TAILQ_FOREACH_FROM(var, head, field) - var부터 (또는 처음부터) 순방향 순회
 *
 * STAILQ_FOREACH_FROM과 동일 패턴 — 중단/재개 지원, 본문 제거 금지(SAFE 사용).
 * 사용 예시: SPDK reactor가 TAILQ 일부만 처리한 뒤 다음 라운드에 이어가기.
 */
#define	TAILQ_FOREACH_FROM(var, head, field)				\
	for ((var) = ((var) ? (var) : TAILQ_FIRST((head)));		\
	    (var);							\
	    (var) = TAILQ_NEXT((var), field))
/* [한국어] var이 NULL이면 첫 엔트리부터, 아니면 그 자리부터 시작. */

/*
 * [한국어]
 * TAILQ_FOREACH_SAFE(var, head, field, tvar) - 제거 안전 순방향 순회
 *
 * tvar에 다음 엔트리를 미리 저장해 본문에서 var 제거/free 가능.
 * 사용 예시: bdev module unregister 시 등록된 모듈 TAILQ를 모두 정리.
 */
#define	TAILQ_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = TAILQ_FIRST((head));				\
	    (var) && ((tvar) = TAILQ_NEXT((var), field), 1);		\
	    (var) = (tvar))
/* [한국어] 콤마 연산자 관용구 — STAILQ/LIST_FOREACH_SAFE와 동일 형태. */

/*
 * [한국어]
 * TAILQ_FOREACH_FROM_SAFE(var, head, field, tvar) - FROM + SAFE 합성
 */
#define	TAILQ_FOREACH_FROM_SAFE(var, head, field, tvar)			\
	for ((var) = ((var) ? (var) : TAILQ_FIRST((head)));		\
	    (var) && ((tvar) = TAILQ_NEXT((var), field), 1);		\
	    (var) = (tvar))
/* [한국어] var의 위치부터(없으면 처음부터) 시작 + 제거 안전. */

/*
 * [한국어]
 * TAILQ_FOREACH_REVERSE_FROM(var, head, headname, field) - 역방향 순회 (var부터)
 *
 * @headname: TAILQ_HEAD를 정의했던 head 구조체 이름 (TAILQ_LAST/PREV에 필요)
 *
 * 동기/배경: TAILQ는 양방향이므로 tail부터 거꾸로 순회 가능. var이 가리키는
 *   엔트리부터 거슬러 올라가며, var이 NULL이면 TAILQ_LAST부터 시작.
 * 시간 복잡도: O(n). 사용 예시: 우선순위 역순으로 객체를 처리할 때.
 * 인접 매크로와의 차이: 단방향 자료구조(SLIST/STAILQ)에는 본 매크로 자체가
 *   존재하지 않는다.
 */
#define	TAILQ_FOREACH_REVERSE_FROM(var, head, headname, field)		\
	for ((var) = ((var) ? (var) : TAILQ_LAST((head), headname));	\
	    (var);							\
	    (var) = TAILQ_PREV((var), headname, field))
/* [한국어] 시작점 선택 후 매 반복마다 TAILQ_PREV로 한 칸씩 거슬러 올라감. */

/*
 * [한국어]
 * TAILQ_FOREACH_REVERSE_SAFE(var, head, headname, field, tvar) - 역방향 + 제거 안전
 */
#define	TAILQ_FOREACH_REVERSE_SAFE(var, head, headname, field, tvar)	\
	for ((var) = TAILQ_LAST((head), headname);			\
	    (var) && ((tvar) = TAILQ_PREV((var), headname, field), 1);	\
	    (var) = (tvar))
/* [한국어] tvar에 이전 엔트리를 캐시해 본문에서 var 제거 가능 — 역방향 버전. */

/*
 * [한국어]
 * TAILQ_FOREACH_REVERSE_FROM_SAFE(...) - 역방향 + FROM + 제거 안전
 */
#define	TAILQ_FOREACH_REVERSE_FROM_SAFE(var, head, headname, field, tvar) \
	for ((var) = ((var) ? (var) : TAILQ_LAST((head), headname));	\
	    (var) && ((tvar) = TAILQ_PREV((var), headname, field), 1);	\
	    (var) = (tvar))
/* [한국어] 시작점 선택 + 이전 엔트리 캐시. */

/*
 * [한국어]
 * TAILQ_LAST(head, headname) - 마지막 엔트리 포인터
 *
 * @headname: TAILQ_HEAD가 만든 head 구조체 이름 (예: my_q는 TAILQ_HEAD(my_q,...)).
 *
 * 동기/배경: head.tqh_last는 "last.tqe_next 슬롯 주소"라는 이중 포인터다.
 *   하지만 TAILQ_ENTRY와 TAILQ_HEAD가 메모리상 동일한 (next, last) 레이아웃
 *   이라는 BSD 매크로 트릭을 활용하면, head.tqh_last를 headname* 로 캐스팅
 *   해 그것의 tqh_last 슬롯을 한 번 더 역참조하는 방식으로 마지막 엔트리
 *   포인터를 단순 캐스팅으로 복원할 수 있다 (CONTAINEROF 불필요).
 * 시간 복잡도: O(1). 사용 예시: 가장 최근 등록된 객체 검사.
 * 인접 매크로와의 차이: STAILQ_LAST는 SPDK_CONTAINEROF 사용; LIST/SLIST는
 *   tail 정보가 없어 본 매크로 자체가 없다.
 */
#define	TAILQ_LAST(head, headname)					\
	(*(((struct headname *)((head)->tqh_last))->tqh_last))
/* [한국어] BSD queue.h의 가장 절묘한 트릭:
 *  - tqh_last는 last.tqe_next 슬롯의 주소.
 *  - tqe_next/tqe_prev의 메모리 레이아웃은 tqh_first/tqh_last와 동일하므로
 *    tqh_last를 (struct headname *)로 캐스팅하면 그 위치를 head 구조처럼
 *    재해석할 수 있다.
 *  - 그 가짜 head의 tqh_last(=원래는 last.tqe_prev에 해당)을 다시 역참조
 *    하면 "마지막 엔트리의 prev가 가리키는 next 슬롯의 값"이 되어 정확히
 *    last 엔트리 포인터가 된다.
 *  - 결과: container_of 없이 단순 캐스팅 두 번으로 last 복원. */

/*
 * [한국어]
 * TAILQ_NEXT(elm, field) - 다음 엔트리 포인터 (l-value 가능)
 *
 * @return: 다음 엔트리 또는 NULL. 시간 복잡도: O(1).
 */
#define	TAILQ_NEXT(elm, field) ((elm)->field.tqe_next)
/* [한국어] tqe_next 직접 접근. INSERT/REMOVE 매크로 본문에서 좌변으로도 등장. */

/*
 * [한국어]
 * TAILQ_PREV(elm, headname, field) - 이전 엔트리 포인터
 *
 * @headname: TAILQ_HEAD 이름 (캐스팅용)
 *
 * 동기/배경: TAILQ_LAST와 동일한 캐스팅 트릭으로 이전 엔트리를 O(1) 복원.
 *   elm.tqe_prev는 "이전.tqe_next 슬롯 주소"이며, 이를 headname* 로 캐스팅해
 *   그 위치의 tqh_last 슬롯을 역참조 — head 레이아웃 동일성 트릭.
 * 시간 복잡도: O(1). 호출 컨텍스트: 큐 소유자.
 * 사용 예시: 양방향 순회, REVERSE FOREACH 매크로 내부.
 * 인접 매크로와의 차이: LIST_PREV는 head 비교 + container_of 사용; STAILQ는
 *   prev 자체가 없어 부재.
 */
#define	TAILQ_PREV(elm, headname, field)				\
	(*(((struct headname *)((elm)->field.tqe_prev))->tqh_last))
/* [한국어] TAILQ_LAST와 동일한 (struct headname *) 캐스팅 트릭으로 이전
 *  엔트리 포인터 복원. CONTAINEROF 불필요. */

/*
 * [한국어]
 * TAILQ_SWAP(head1, head2, type, field) - 두 TAILQ의 내용을 교환
 *
 * @head1, @head2: 두 TAILQ_HEAD 주소
 * @type:          엔트리 구조체 이름
 * @field:         TAILQ_ENTRY 멤버 이름
 *
 * 동기/배경: TAILQ는 head 두 필드(tqh_first, tqh_last)와 첫 엔트리의 tqe_prev
 *   포인터(=head.tqh_first 주소를 가리키도록 설정되어 있음)까지 정정해야
 *   교환 후에도 모든 불변식이 유지된다.
 * 동작:
 *   1) head1의 (first, last)를 임시 백업.
 *   2) head1 ← head2의 (first, last) 복사.
 *   3) head2 ← 백업한 head1의 (first, last) 복사.
 *   4) head1의 새 first가 NULL이 아니면 그 tqe_prev를 &head1.tqh_first로
 *      정정; NULL이면 head1.tqh_last를 자기 first 슬롯 주소로 정정 (빈 큐
 *      불변식).
 *   5) head2 측도 동일 정정.
 * 시간 복잡도: O(1). 호출 컨텍스트: 두 큐의 단일 소유자.
 * 사용 예시: SPDK의 메시지 처리에서 "수신 큐"와 "처리 중 큐"를 swap.
 * 인접 매크로와의 차이: GCC 13의 false positive를 받는 유일한 swap이라
 *   SPDK_QE_SUPPRESS_WARNINGS로 감싼다.
 */
#define TAILQ_SWAP(head1, head2, type, field) do {			\
	SPDK_QE_SUPPRESS_WARNINGS();					\
        /* [한국어] GCC 13의 -Wdangling-pointer false positive 억제 시작.
         *  - 이중 포인터 갱신 패턴을 GCC 13이 잘못 의심하므로, SWAP 본문
         *    한정으로만 경고 비활성화. */                                                \
	struct type *swap_first = (head1)->tqh_first;			\
        /* [한국어] head1.first 백업 — 이후 head1 덮어쓸 때 잃지 않도록. */                                                \
	struct type **swap_last = (head1)->tqh_last;			\
        /* [한국어] head1.tqh_last(이중 포인터) 백업. */                                                \
	(head1)->tqh_first = (head2)->tqh_first;			\
        /* [한국어] head1.first ← head2.first. */                                                \
	(head1)->tqh_last = (head2)->tqh_last;				\
        /* [한국어] head1.tqh_last ← head2.tqh_last. */                                                \
	(head2)->tqh_first = swap_first;				\
        /* [한국어] head2.first ← 백업해둔 head1.first. */                                                \
	(head2)->tqh_last = swap_last;					\
        /* [한국어] head2.tqh_last ← 백업해둔 head1.tqh_last. */                                                \
	if ((swap_first = (head1)->tqh_first) != NULL)			\
        /* [한국어] head1의 새 first가 존재하면 — 그 엔트리의 tqe_prev는
         *  옛 head(즉 head2) 주소를 가리키고 있다. 새 head 주소로 정정. */                                                \
		swap_first->field.tqe_prev = &(head1)->tqh_first;	\
        /* [한국어] new_first.tqe_prev = &head1.tqh_first — 첫 엔트리의 prev는
         *  head의 tqh_first 슬롯 주소를 가리켜야 한다는 TAILQ 불변식 복구. */                                                \
	else								\
        /* [한국어] head1이 비어있는 경우 — head2가 원래 비어있던 것. */                                                \
		(head1)->tqh_last = &(head1)->tqh_first;		\
        /* [한국어] 빈 TAILQ 불변식: tqh_last가 자기 first 슬롯 주소를 가리켜야
         *  이후 INSERT_TAIL이 분기 없이 동작. */                                                \
	if ((swap_first = (head2)->tqh_first) != NULL)			\
		swap_first->field.tqe_prev = &(head2)->tqh_first;	\
        /* [한국어] head2 측도 동일 정정 — 첫 엔트리 prev를 새 head로. */                                                \
	else								\
		(head2)->tqh_last = &(head2)->tqh_first;		\
        /* [한국어] head2 측 빈 큐 불변식 복구. */                                                \
	SPDK_QE_UNSUPPRESS_WARNINGS();					\
        /* [한국어] 진단 스택 복원 — 다른 코드의 -Wdangling-pointer 검사가
         *  정상 동작하도록 push 시점 상태로 되돌린다. */                                                \
} while (0)

#ifdef __cplusplus
/* [한국어] extern "C" 블록 닫기 — C++ 빌드에서만 닫는다. */
}
#endif

#endif
/* [한국어] SPDK_QUEUE_EXTRAS_H include 가드 종료. */
