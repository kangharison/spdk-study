/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] BSD sys/queue.h 래퍼 · SPDK 확장 (queue.h)
 *
 * === 파일의 역할 ===
 * FreeBSD의 `sys/queue.h`가 정의하는 4종 intrusive 자료구조 — SLIST(Singly
 * Linked List, 단방향 단일 연결 리스트), LIST(doubly-linked list, 양방향
 * 연결 리스트), STAILQ(Singly-linked TAIL Queue, head/tail O(1) 단방향
 * 큐), TAILQ(doubly-linked TAIL Queue, head/tail O(1) 양방향 큐), 그리고
 * 거의 사용되지 않는 CIRCLEQ(원형 큐) — 매크로 집합을 SPDK 전역에 노출하기
 * 위한 단일 진입 헤더이다. 노출하는 매크로 자체는 BSD 원본 매크로지만, 본
 * 헤더는 다음 세 가지 SPDK 고유 보강을 추가한다:
 *   (1) 리눅스 빌드 시 FreeBSD에만 존재하고 glibc <sys/queue.h>가 누락한
 *       매크로들을 `queue_extras.h`로 보충한다 (FreeBSD/macOS는 OS 헤더가
 *       이미 완전판이라 보충 불필요).
 *   (2) scan-build(Clang Static Analyzer)가 TAILQ의 이중 포인터(tqe_prev,
 *       tqe_next) 갱신을 추적하지 못해 use-after-remove 오탐을 일으키는
 *       문제를 해결하기 위해 `TAILQ_REMOVE`를 assert 포함 변형으로 재정의
 *       한다 (분석기 빌드에서만 활성).
 *   (3) "엔트리가 어떤 TAILQ에 현재 매달려 있는가?"를 판별·표시하는
 *       SPDK 자체 추가 매크로 4종 — `TAILQ_ENTRY_ENQUEUED`,
 *       `TAILQ_ENTRY_NOT_ENQUEUED`, `TAILQ_ENTRY_CLEAR`,
 *       `TAILQ_REMOVE_CLEAR` — 을 정의한다. 이들은 BSD 원본에는 없으며,
 *       SPDK 코드 곳곳의 "I/O 객체가 retry 큐에 들어 있는지", "qpair가
 *       active 리스트에 등록되어 있는지" 같은 상태 확인 패턴을 표준화한다.
 *
 * SPDK NVMe 드라이버는 본래 FreeBSD에서 작성됐다가 리눅스로 포팅된
 * 역사가 있어, 원 코드가 `SLIST_FOREACH_SAFE`, `STAILQ_LAST` 같은
 * FreeBSD 확장 매크로에 자유롭게 의존한다. 리눅스 glibc 헤더에는 이들이
 * 일부 빠져 있어 그대로 컴파일되지 않으므로, `queue_extras.h`가 빠진 것을
 * 동일 시그니처로 재정의해 코드 변경 없이 빌드 가능하게 만든다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 코드 트리 전체에서 intrusive 리스트가 등장하는 거의 모든 위치 —
 * lib/nvme의 컨트롤러/큐페어/요청 큐, lib/bdev의 등록된 bdev/module 체인과
 * in-flight I/O 큐, lib/thread의 spdk_thread / poller / message 큐,
 * lib/nvmf의 transport/qpair/subsystem 리스트 등 — 가 본 헤더에 정의된
 * 매크로를 거쳐 자료구조를 조작한다. 매크로는 컴파일 타임에 인라인 전개
 * 되므로 런타임 오버헤드는 손으로 작성한 두 줄짜리 포인터 조작과 동일
 * 하며, 일반적인 C 표준 라이브러리에 들어 있는 함수형 리스트 API와는
 * 호출 비용 자체가 다르다 (함수 호출 0회, 메모리 할당 0회).
 * 본 헤더는 어떤 OS 콜이나 락도 호출하지 않으며, 따라서 호출자의 스레드
 * 컨텍스트에서 그대로 실행된다 — SPDK 핵심 디자인인 lockless / per-core
 * 모델과 정확히 같은 가정 위에서 사용된다 (동일 리스트를 여러 스레드가
 * 동시에 만지면 조용히 깨진다).
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - <sys/cdefs.h>: BSD 스타일 컴파일러 attribute 매크로(`__unused`,
 *     `__dead2`, `__BEGIN_DECLS` 등). 리눅스 glibc의 <sys/queue.h>가
 *     내부적으로 일부 BSD attribute를 참조할 수 있어 선행 포함이 안전.
 *   - <sys/queue.h>: BSD 표준 queue 매크로 본체. 플랫폼이 제공하는
 *     원본 정의를 가져온다.
 *   - "spdk/queue_extras.h": 리눅스에서 누락된 확장 매크로의 SPDK
 *     포팅본. `__linux__` 플랫폼에서만 포함된다.
 *   - <assert.h>: TAILQ_ENTRY_CLEAR가 직접 호출하는 `assert()` 매크로
 *     (이 헤더는 명시적으로 include하지 않으나, 호출자가 stdinc.h 또는
 *     assert.h를 선행 포함하는 SPDK 관례에 의존).
 * 의존하는 모듈 (대표적):
 *   - lib/nvme/*: `ctrlr->active_io_qpairs`(TAILQ), 컨트롤러 글로벌
 *     리스트(TAILQ), 비동기 이벤트 요청 풀(STAILQ), pending request
 *     큐(STAILQ).
 *   - lib/bdev/*: 등록된 모든 bdev 체인(TAILQ), 디바이스 모듈 체인
 *     (TAILQ), I/O 채널이 보유한 in-flight bdev_io 큐(TAILQ).
 *   - lib/thread/*: spdk_thread 글로벌 TAILQ, per-thread poller LIST,
 *     pending message STAILQ.
 *   - lib/nvmf/*: transport별 qpair TAILQ, subsystem 등록 TAILQ.
 *   - module/bdev/nvme/*, module/bdev/aio/* 등 거의 모든 bdev 모듈.
 * 공유 자료구조: BSD queue 매크로는 "intrusive" 방식 — 사용자 구조체
 *   안에 `STAILQ_ENTRY(my_type) link;` 같은 연결고리 필드를 직접 박아
 *   넣고, 큐는 그 필드의 주소만 추적한다. 결과적으로 (a) 별도 메모리
 *   할당 없음, (b) 한 객체가 여러 큐에 동시에 등록 가능(필드를 여러 개
 *   두면 됨), (c) 리스트 노드와 객체의 수명이 자동으로 일치하는 장점이
 *   있다. 단점은 객체가 어느 한 시점에 "어느 큐의 어느 위치에 있는가"
 *   라는 정보를 헤더가 직접 들고 있지 않다는 것이며, SPDK 확장 매크로
 *   `TAILQ_ENTRY_ENQUEUED`/`TAILQ_ENTRY_CLEAR`가 이 갭을 메우기 위해
 *   존재한다.
 *
 * === 주요 함수/구조체 요약 ===
 * 본 헤더는 BSD 표준 queue 매크로(SLIST_*, LIST_*, STAILQ_*, TAILQ_*,
 * CIRCLEQ_*)는 그대로 통과시키고, 추가로 다음을 정의한다:
 *   - `TAILQ_REMOVE` (scan-build 분기): 표준 매크로의 의미를 똑같이
 *     수행하되, 제거 직후 head를 한 번 더 순회하며 제거된 엔트리가
 *     남아있지 않음을 `assert`로 검증. scan-build에게 "엔트리가 더 이상
 *     리스트에 없다"는 사실을 검증 가능한 형태로 알려주어 use-after-remove
 *     오탐을 제거하는 것이 목적이다.
 *   - `TAILQ_ENTRY_ENQUEUED(elm, field)`: 엔트리의 `tqe_prev`가 NULL이
 *     아닌지로 "어떤 TAILQ에 매달려 있는가"를 판정. 전제: 엔트리는 반드시
 *     zero-init되었거나 `TAILQ_ENTRY_CLEAR`/`TAILQ_REMOVE_CLEAR`로
 *     명시 비워졌어야 한다. 이 전제가 없으면 표준 TAILQ_REMOVE 후에도
 *     `tqe_prev`가 stale 값을 들고 있어 잘못된 true를 반환한다.
 *   - `TAILQ_ENTRY_NOT_ENQUEUED(elm, field)`: 위의 역(가독성용 별칭).
 *   - `TAILQ_ENTRY_CLEAR(elm, field)`: 제거 후 엔트리를 "어떤 큐에도
 *     속하지 않음" 상태로 명시 마킹. 내부적으로 `tqe_prev = NULL`로
 *     설정하며, 이미 클리어된 상태에서 호출하면 assert로 잡힌다.
 *   - `TAILQ_REMOVE_CLEAR(head, elm, field)`: TAILQ_REMOVE + ENTRY_CLEAR
 *     의 합성 매크로. SPDK 코드에서 "리스트에서 빼고 동시에 빈 상태로
 *     표시"가 필요할 때 사용하는 일반적인 관용구.
 */

#ifndef SPDK_QUEUE_H
/* [한국어] include 가드 시작 — 동일 번역 단위에서 본 헤더가 두 번 이상
 *  포함되어도 매크로/타입이 중복 정의되지 않도록 막는다. SPDK는 거의 모든
 *  소스가 이 헤더를 (직간접적으로) 끌어쓰므로 가드 누락 시 빌드 실패. */
#define SPDK_QUEUE_H
/* [한국어] 가드 심볼 정의 — 다음 #ifndef SPDK_QUEUE_H가 false가 되어
 *  내부 본문이 한 번만 컴파일된다. */

#ifdef __cplusplus
/* [한국어] C++ 링크 규약 가드 — 본 헤더는 매크로만 노출하므로 실제로는
 *  name mangling 영향이 없지만, SPDK 공개 헤더의 일관된 관례로 모든
 *  헤더가 extern "C" 블록을 둔다. C++ 빌드에서도 자연스럽게 포함되도록
 *  보호하는 역할. */
extern "C" {
#endif

#include <sys/cdefs.h>
/* [한국어] BSD 스타일 컴파일러 attribute 매크로 헤더 — `__unused`,
 *  `__dead2`, `__BEGIN_DECLS`/`__END_DECLS` 등을 정의. 일부 플랫폼의
 *  <sys/queue.h>가 이들 매크로를 내부적으로 참조하므로 선행 포함이 안전.
 *  glibc는 자체적으로 정의하지만, FreeBSD/NetBSD 호환 코드 경로에서 누락이
 *  있을 경우를 대비한다. */
#include <sys/queue.h>
/* [한국어] BSD 표준 queue 매크로 본체 — SLIST(단일 연결 리스트),
 *  LIST(양방향 연결 리스트), STAILQ(단일 연결 tail queue), TAILQ(양방향
 *  tail queue), CIRCLEQ(원형 큐)의 head/entry 구조체와 INSERT/REMOVE/
 *  FOREACH 매크로 일체를 정의. 플랫폼 헤더 본체이며 SPDK는 이를 전혀
 *  수정하지 않고 그대로 노출한다. */

/*
 * The SPDK NVMe driver was originally ported from FreeBSD, which makes
 *  use of features in FreeBSD's queue.h that do not exist on Linux.
 *  Include a header with these additional features on Linux only.
 */
#ifdef __linux__
/* [한국어] 리눅스 빌드 분기 — FreeBSD/macOS의 시스템 <sys/queue.h>는
 *  이미 SLIST_FOREACH_SAFE, STAILQ_LAST, TAILQ_FOREACH_FROM 등을 모두
 *  제공하므로 추가 포함이 필요 없다. 리눅스 glibc 헤더는 SAFE 변형과
 *  REVERSE 변형, SWAP 매크로 등을 제공하지 않기 때문에 본 분기 안에서만
 *  SPDK 자체 보충 헤더를 끌어쓴다. 매크로 중복 정의를 막기 위해 OS
 *  분기는 반드시 필요. */
#include "spdk/queue_extras.h"
/* [한국어] 누락 매크로의 SPDK 포팅 정의를 가져오는 헤더. 실제 매크로
 *  본문은 분량이 많아 별도 파일로 분리되어 있으며, FreeBSD 원본 코드를
 *  거의 그대로 옮겨 와 호환성을 보장한다. */
#endif

/*
 * scan-build can't follow double pointers in queues and often assumes
 * that removed elements are still on the list. We redefine TAILQ_REMOVE
 * with extra asserts to silence it.
 */
#ifdef __clang_analyzer__
/* [한국어] Clang Static Analyzer(scan-build)로 컴파일 중일 때만 본 분기
 *  활성화. 일반 GCC/Clang 컴파일 빌드에는 영향 0이며, 정상 실행 바이너리
 *  에는 아래 재정의가 들어가지 않는다.
 *  배경: scan-build는 TAILQ가 사용하는 `tqe_prev`(이중 포인터) 갱신
 *  관용구 `*(elm)->field.tqe_prev = (elm)->field.tqe_next;` 의 결과를
 *  추적하지 못해, REMOVE 직후에도 elm이 여전히 리스트에 매달려 있다고
 *  잘못 가정하고 use-after-remove 류 false positive를 다량 생성한다.
 *  대응: 표준 TAILQ_REMOVE 본문을 그대로 펼치되, 마지막에 head를 한 번
 *  순회하여 `assert(_elm != elm)`을 실행한다. 분석기는 assert가 통과하는
 *  경로만 살아남긴다는 사실을 알므로, 그 이후 코드 경로에서 elm은
 *  리스트에 없다는 사실을 인지하게 된다. 결과적으로 오탐만 사라지고
 *  로직은 동일. */
#undef TAILQ_REMOVE
/* [한국어] 시스템 헤더가 정의한 표준 TAILQ_REMOVE를 명시 #undef 후
 *  아래 확장판으로 교체. 같은 매크로 이름을 재정의하므로 호출자 코드는
 *  변경 없이 새 본문을 쓰게 된다. */

/*
 * [한국어]
 * TAILQ_REMOVE - TAILQ에서 한 엔트리를 제거 (scan-build 친화 변형)
 *
 * @head:  대상 TAILQ_HEAD 구조체의 주소
 * @elm:   제거할 엔트리 객체 포인터 (반드시 head 안에 매달려 있어야 함)
 * @field: 엔트리 안에 박혀 있는 TAILQ_ENTRY(...) 필드의 멤버명
 * @return: 없음 (void 매크로)
 *
 * 표준 BSD `TAILQ_REMOVE`와 의미적으로 100% 동등하나, 정적 분석기 오탐을
 * 잡기 위해 끝에 `TAILQ_FOREACH` 한 번을 추가했다. 동작은 다음과 같다:
 *   1) elm 다음 엔트리가 존재하면 → 그 엔트리의 `tqe_prev`를 elm의
 *      `tqe_prev`로 이음(자기 자신을 우회).
 *   2) elm이 마지막이면 → head의 `tqh_last`(=마지막 엔트리의 next 슬롯
 *      주소를 들고 있는 포인터)를 elm의 `tqe_prev`로 이동.
 *   3) `*tqe_prev = tqe_next` 한 줄로 elm 이전 노드(또는 head)의 next
 *      슬롯이 elm 다음을 가리키게 한다 (이중 포인터 관용구의 핵심).
 *   4) (분석기 전용) head를 한 번 더 순회하며 elm이 남지 않았음을
 *      `assert`로 확인. 정상 로직이면 항상 통과.
 *
 * 시간 복잡도: 일반 빌드 O(1). 분석기 빌드는 검증 순회 때문에 O(n).
 * 호출 컨텍스트: 호출 스레드에서 그대로 실행. TAILQ 자체에는 락이 없으므로
 *   호출자가 동일 큐를 동시에 만지지 않도록 책임진다 — SPDK는 큐를 단일
 *   reactor/스레드에 고정해 사용하는 패턴(per-core ownership)으로 이 조건을
 *   자연스럽게 충족.
 * 사용 예시: lib/nvme의 `ctrlr->active_io_qpairs`에서 qpair 해제 시 호출,
 *   lib/bdev의 등록된 bdev 체인에서 unregister 경로, lib/thread의
 *   `g_threads` 글로벌 TAILQ에서 thread 종료 시 등.
 * 인접 매크로와의 차이: `TAILQ_REMOVE_CLEAR`는 본 매크로 + ENTRY_CLEAR
 *   합성. `STAILQ_REMOVE`는 양방향 포인터가 없어 head부터 선형 탐색
 *   (O(n))이 필요한 점이 다르다.
 */
#define TAILQ_REMOVE(head, elm, field) do {				\
	__typeof__(elm) _elm;						\
        /* [한국어] 아래 검증 순회에서 사용할 임시 포인터.
         *  - GCC/Clang 확장 `__typeof__`로 elm과 동일한 타입의 변수를
         *    매크로 안에서 안전하게 선언한다(타입을 매크로 인자로 받지
         *    않아도 되도록).
         *  - 일반 빌드에는 들어가지 않는 분석기 전용 코드라 변수 선언
         *    오버헤드는 무시 가능. */                                                \
	if (((elm)->field.tqe_next) != NULL)				\
        /* [한국어] elm의 다음 엔트리가 존재하는 경우(=elm이 head 또는
         *  중간 엔트리). 다음 엔트리의 prev 포인터를 elm의 prev로 이어
         *  주면 elm은 양방향 모두에서 우회된다. */                                                \
		(elm)->field.tqe_next->field.tqe_prev =			\
		    (elm)->field.tqe_prev;				\
        /* [한국어] elm.next.prev = elm.prev — 다음 엔트리가 elm을 건너
         *  뛰도록 prev 슬롯을 갱신.
         *  - tqe_prev는 "이전 노드의 next 필드 주소"를 담는 이중 포인터로,
         *    (대상 노드를 가리키는 다음 노드의 prev) 위치를 넘겨주면
         *    포인터 한 번의 대입으로 양방향 연결이 정합 상태가 된다. */                                                \
	else								\
        /* [한국어] elm.next == NULL — elm이 리스트의 마지막 엔트리였던
         *  경우. 마지막을 추적하는 head->tqh_last를 elm 이전으로 옮긴다. */                                                \
		(head)->tqh_last = (elm)->field.tqe_prev;		\
        /* [한국어] head.tqh_last = elm.prev — tqh_last는 "마지막 엔트리의
         *  next 슬롯 주소"를 가지므로, elm 제거 후의 새로운 tail은 elm
         *  바로 이전 노드의 next 슬롯이 된다(=elm.prev가 가리키는 곳). */                                                \
	*(elm)->field.tqe_prev = (elm)->field.tqe_next;			\
        /* [한국어] 이전 엔트리의 next 슬롯을 elm.next로 덮어써 forward
         *  방향에서 elm을 끊어낸다.
         *  - elm이 head였으면 tqe_prev는 head->tqh_first의 주소이므로,
         *    이 한 줄로 head 갱신과 일반 노드 갱신을 분기 없이 처리할
         *    수 있다(이중 포인터 관용구의 가장 큰 장점). */                                                \
	/* make sure the removed elm is not on the list anymore */	\
	TAILQ_FOREACH(_elm, head, field) {				\
        /* [한국어] head 전체를 처음부터 다시 순회. 이 순회 자체는 분석기
         *  에게 "REMOVE 후 리스트 상태"를 코드 형태로 보여주기 위한 장치
         *  이며, 정상 로직이면 elm이 발견될 일이 없다. */                                                \
		assert(_elm != elm);					\
        /* [한국어] 만약 elm이 다시 발견되면 자료구조가 깨졌다는 뜻 →
         *  디버그 빌드에서 즉시 abort. 분석기에게도 "이 경로에선 elm이
         *  반드시 리스트에 없다"는 사실을 알려준다. */                                                \
	}								\
} while (0)
/* [한국어] do { ... } while (0) 관용구 — 다중 문장 매크로를 단일
 *  statement로 감싸 `if (cond) TAILQ_REMOVE(...); else ...` 같은
 *  문맥에서도 문법 오류 없이 동작하도록 한다. 끝에 세미콜론이 없는
 *  것이 일반적이며, 호출 측에서 `;`을 붙이는 관례. */
#endif

/*
 * Check if an entry is on any TAILQ list.
 *
 * Should only be used on zero initalized entries or after
 * calling TAILQ_REMOVE_CLEAR() or TAILQ_ENTRY_CLEAR().
 */
/*
 * [한국어]
 * TAILQ_ENTRY_ENQUEUED - 엔트리가 어떤 TAILQ에 매달려 있는지 검사
 *
 * @elm:   검사할 객체 포인터 (TAILQ_ENTRY 필드를 가진 구조체)
 * @field: 그 구조체 안의 TAILQ_ENTRY 멤버 이름
 * @return: bool — 매달려 있으면 true, 아니면 false
 *
 * 동기/배경: BSD 원본 TAILQ에는 "엔트리가 현재 어떤 큐에 들어 있는가?"를
 *   직접 묻는 API가 없다. 호출자가 별도 플래그를 들고 있어야 했는데,
 *   SPDK는 NVMe I/O 객체나 bdev_io가 retry/대기/완료 큐를 빈번히 옮겨
 *   다니므로 이 정보를 표준화할 필요가 있었다. 본 매크로는 표준 TAILQ가
 *   사용하는 `tqe_prev`(이중 포인터, 매달려 있는 동안 항상 non-NULL)의
 *   특성을 활용해 추가 비용 0으로 enqueued 여부를 반환한다.
 *
 * 동작: 단순히 `elm->field.tqe_prev != NULL`로 판정.
 * 시간 복잡도: O(1) (단일 비교).
 * 호출 컨텍스트: 어디서나 가능. 단 큐를 동시에 만지는 다른 스레드가 있다면
 *   결과는 불안정 — SPDK는 per-core ownership으로 자연 회피.
 * 사용 예시: lib/nvme/nvme_qpair에서 request가 retry queue에 있는지 검사
 *   (RESET 시 cleanup 분기 결정), lib/bdev에서 채널 정리 시 in-flight
 *   I/O가 아직 큐에 남아있는지 확인 등.
 *
 * 전제 조건 (매우 중요): elm이 zero-initialized이거나, 가장 최근 제거 시
 *   `TAILQ_REMOVE_CLEAR`/`TAILQ_ENTRY_CLEAR`로 명시 비워졌어야 한다.
 *   표준 `TAILQ_REMOVE`만 호출했다면 `tqe_prev`에 stale 주소가 남아
 *   잘못된 true가 반환된다. 이 전제는 SPDK 코딩 컨벤션 문서에 명시되어
 *   있으며, 위반 시 자료구조 침묵 깨짐.
 *
 * 인접 매크로와의 차이: `TAILQ_ENTRY_NOT_ENQUEUED`는 단순 negation;
 *   BSD 원본에는 본 매크로 자체가 없으므로 SPDK 고유 추가.
 */
#define TAILQ_ENTRY_ENQUEUED(elm, field)				\
    ((elm)->field.tqe_prev != NULL)
/* [한국어] 본문은 한 줄 — `tqe_prev`가 NULL인지로 판정.
 *  - 매달려 있는 동안 tqe_prev는 항상 "이전 노드의 next 슬롯 주소" 또는
 *    "head의 tqh_first 주소"로 유효 포인터다.
 *  - 표준 TAILQ_REMOVE는 tqe_prev를 NULL로 클리어하지 않으므로, 본 매크로
 *    의 정확성은 호출자가 적절히 ENTRY_CLEAR를 호출했는지에 달려 있다. */

/*
 * Check if an entry is not on any TAILQ list.
 *
 * Should only be used on zero initalized entries or after
 * calling TAILQ_REMOVE_CLEAR() or TAILQ_ENTRY_CLEAR().
 */
/*
 * [한국어]
 * TAILQ_ENTRY_NOT_ENQUEUED - 엔트리가 어떤 TAILQ에도 속해 있지 않은지 검사
 *
 * @elm:   검사할 객체 포인터
 * @field: TAILQ_ENTRY 멤버 이름
 * @return: bool — 어디에도 속해 있지 않으면 true
 *
 * 단순히 ENQUEUED의 부정. 호출 측 가독성 — `if
 * (!TAILQ_ENTRY_ENQUEUED(...))`보다 `if (TAILQ_ENTRY_NOT_ENQUEUED(...))`가
 * "이 객체는 큐에 없다"는 의도를 더 직관적으로 드러낸다 — 을 위해 별칭
 * 형태로 제공. 시간 복잡도/컨텍스트/전제 조건 모두 ENQUEUED와 동일.
 * 사용 예시: assert로 "이 함수 진입 시점에 io_u가 어떤 큐에도 없어야 한다"
 *   를 명시하는 부분에서 자주 쓰인다.
 */
#define TAILQ_ENTRY_NOT_ENQUEUED(elm, field)				\
    (!TAILQ_ENTRY_ENQUEUED(elm, field))
/* [한국어] ENQUEUED의 단순 부정 — 인라인 매크로라 분기/호출 비용 0. */

/*
 * Mark an entry as absent from any TAILQ list.
 *
 * Should be called once after TAILQ_REMOVE(), or on entries
 * that were not initalized to zero.
 */
/*
 * [한국어]
 * TAILQ_ENTRY_CLEAR - 제거된 엔트리를 "어떤 큐에도 속하지 않음" 상태로 마킹
 *
 * @elm:   대상 객체 포인터
 * @field: TAILQ_ENTRY 멤버 이름
 * @return: 없음
 *
 * 동기/배경: 표준 `TAILQ_REMOVE`는 tqe_prev에 stale 값을 남기므로, 같은
 *   엔트리에 대해 이후 `TAILQ_ENTRY_ENQUEUED`가 잘못 true를 반환할 수
 *   있다. 본 매크로는 tqe_prev를 NULL로 명시 클리어해 ENQUEUED 매크로의
 *   계약을 유지시키는 역할.
 *
 * 동작:
 *   1) `assert(TAILQ_ENTRY_ENQUEUED(elm, field))` — 호출 시점에 elm은
 *      반드시 어딘가의 큐에 매달려 있었거나, 직전에 TAILQ_REMOVE만 막
 *      끝낸 직후여야 한다. 이미 클리어된 상태에서 한 번 더 호출하면
 *      논리 오류로 잡힌다.
 *   2) `tqe_prev = NULL` — 이후 ENQUEUED 호출이 false를 반환하게 된다.
 *
 * 시간 복잡도: O(1). 호출 컨텍스트: 호출자 스레드. 다른 스레드와
 *   동시에 같은 엔트리를 만지면 안 된다(SPDK는 per-core 소유로 회피).
 * 호출 체인: TAILQ_REMOVE → [TAILQ_ENTRY_CLEAR] (혹은 TAILQ_REMOVE_CLEAR
 *   가 두 단계를 묶어 호출).
 * 사용 예시: SPDK NVMe의 비동기 이벤트 핸들러가 처리 끝난 이벤트 객체를
 *   "어디 큐에도 없음" 상태로 표시한 뒤 풀에 반납할 때 사용.
 */
#define TAILQ_ENTRY_CLEAR(elm, field) do {				\
	/* Ensure the entry was on a list before clearing */		\
	assert(TAILQ_ENTRY_ENQUEUED(elm, field));			\
        /* [한국어] 클리어 직전에는 elm이 반드시 매달려 있었어야 한다는
         *  계약 검증. 이미 클리어된 객체에 또 호출하거나, 한 번도 큐에
         *  들어가 본 적 없는 객체에 호출하면 NDEBUG가 아닐 때 abort.
         *  - 디버그 빌드에서 SPDK의 큐 사용 패턴 위반을 조기에 잡는 안전망. */\
	(elm)->field.tqe_prev = NULL;					\
        /* [한국어] tqe_prev = NULL로 명시 비움 — TAILQ_ENTRY_ENQUEUED가
         *  비교하는 슬롯이므로, 이 한 줄로 이후 모든 ENQUEUED 호출이
         *  false를 반환한다.
         *  - tqe_next는 손대지 않는다. ENQUEUED는 prev만 보고, next는
         *    다음에 큐에 들어갈 때 INSERT 매크로가 덮어쓰므로 클리어
         *    불필요. */                                                \
} while (0)

/*
 * Remove entry from TAILQ list and mark it as absent.
 */
/*
 * [한국어]
 * TAILQ_REMOVE_CLEAR - 큐에서 제거하고 동시에 ENQUEUED 슬롯도 비움
 *
 * @head:  대상 TAILQ_HEAD 주소
 * @elm:   제거할 엔트리 포인터
 * @field: TAILQ_ENTRY 멤버 이름
 * @return: 없음
 *
 * 동기/배경: SPDK 코드에서 가장 흔한 패턴은 "엔트리를 큐에서 빼면서
 *   동시에 그 엔트리가 더 이상 어느 큐에도 속하지 않음을 표시"이다.
 *   매번 두 매크로를 따로 부르면 빠뜨리기 쉬워, 본 매크로가 합성 형태로
 *   제공된다.
 *
 * 동작:
 *   1) `TAILQ_REMOVE(head, elm, field)` 호출 — 표준 동작 또는 scan-build
 *      변형 동작으로 elm을 큐에서 분리.
 *   2) `TAILQ_ENTRY_CLEAR(elm, field)` 호출 — tqe_prev를 NULL로 비움.
 *
 * 시간 복잡도: O(1) (분석기 빌드는 O(n) — REMOVE의 검증 순회 때문).
 * 호출 컨텍스트: 일반 TAILQ 조작과 동일 — 큐 소유 스레드에서.
 * 사용 예시: lib/nvme의 active_io_qpairs에서 qpair 분리 시 사용,
 *   bdev 모듈이 in-flight 리스트에서 bdev_io를 떼어 완료 처리 직전.
 * 인접 매크로와의 차이: 표준 `TAILQ_REMOVE`는 ENQUEUED 슬롯을 그대로
 *   둠 → 이후 ENQUEUED 검사가 의미 없음. 그래서 SPDK에서 ENQUEUED를
 *   쓸 객체에는 항상 본 합성 매크로를 써야 한다.
 */
#define TAILQ_REMOVE_CLEAR(head, elm, field) do {			\
	TAILQ_REMOVE(head, elm, field);					\
        /* [한국어] 1단계: 큐 분리 — 표준 매크로 또는 scan-build 변형.
         *  여기까지는 BSD 원본과 의미적으로 동일. */                                                \
	TAILQ_ENTRY_CLEAR(elm, field);					\
        /* [한국어] 2단계: 엔트리를 "어느 큐에도 없음"으로 명시 마킹.
         *  - 이 단계 내부의 assert는 1단계 직후 elm이 매달린 적 있음을
         *    의미하므로 일반 흐름에서는 통과한다.
         *  - 두 단계가 사이에 끼어들 가능성이 없도록 do-while(0)로
         *    묶여 있어 외부에서는 단일 statement처럼 동작. */                                                \
} while (0)

#ifdef __cplusplus
/* [한국어] extern "C" 블록 닫기 — C++ 빌드에서만 닫는다. */
}
#endif

#endif
/* [한국어] SPDK_QUEUE_H include 가드 종료. */
