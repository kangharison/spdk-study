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
 * [한국어 설명] BSD sys/queue.h 확장 매크로 (queue_extras.h)
 *
 * === 파일의 역할 ===
 * FreeBSD `sys/queue.h`는 제공하지만 리눅스 glibc의 `sys/queue.h`에는
 * 없는 매크로들을 리눅스 빌드를 위해 SPDK가 직접 제공하는 파일이다.
 * SPDK NVMe 드라이버 등이 원래 FreeBSD 기반이라 이 확장 매크로에 의존하므로,
 * 리눅스에서도 코드 수정 없이 빌드하기 위해 원본 FreeBSD 매크로 구현을
 * 그대로 포팅해 둔 것이다.
 *
 * 제공 그룹:
 *   (1) STAILQ 확장: STAILQ_HEAD / _HEAD_INITIALIZER / _EMPTY / _FIRST /
 *       _FOREACH_FROM / _FOREACH_SAFE / _FOREACH_FROM_SAFE / _LAST / _NEXT /
 *       _REMOVE_AFTER / _SWAP
 *   (2) LIST 확장: LIST_HEAD / _HEAD_INITIALIZER / _ENTRY / _EMPTY /
 *       _FIRST / _FOREACH_FROM / _FOREACH_SAFE / _FOREACH_FROM_SAFE /
 *       _NEXT / _PREV / _SWAP
 *   (3) SLIST 확장: SLIST_SWAP / SLIST_FOREACH_SAFE
 *   (4) TAILQ 확장: TAILQ_EMPTY / _FIRST / _FOREACH_FROM / _FOREACH_SAFE /
 *       _FOREACH_FROM_SAFE / _FOREACH_REVERSE_FROM / _FOREACH_REVERSE_SAFE /
 *       _FOREACH_REVERSE_FROM_SAFE / _LAST / _NEXT / _PREV / _SWAP
 *   (5) GCC 13 Wdangling-pointer 경고 억제 헬퍼
 *   (6) QMD_*_CHECK_* : BSD 커널 전용 invariant 체크 (_KERNEL && INVARIANTS
 *       일 때만 활성 — 유저스페이스 빌드에서는 빈 매크로)
 *
 * === 전체 아키텍처에서의 위치 ===
 * `spdk/queue.h`가 리눅스 빌드에서 이 파일을 포함한다. 리눅스 외 플랫폼에서는
 * 포함되지 않는다(이미 OS가 제공). SPDK 전반의 intrusive 리스트 조작에 사용.
 *
 * === 타 모듈과의 연결 ===
 * 의존: SPDK 관례상 상위에서 spdk/stdinc.h가 먼저 포함됨. 본 파일 자체는
 *       표준 헤더 include 없이도 매크로 정의만 노출 (타입은 사용자가 정의).
 *       SPDK_CONTAINEROF 매크로가 STAILQ_LAST/LIST_PREV 구현에 사용됨
 *       (spdk/util.h에서 제공).
 * 의존하는 모듈: lib/nvme/*, lib/bdev/*, lib/thread/* 등 거의 모든 SPDK 코드.
 * 공유 자료구조: 매크로는 intrusive 연결고리(stqe_next, tqe_prev 등)를
 *       사용자 구조체 필드로 삽입하는 방식. 할당 없음.
 *
 * === 주요 함수/구조체 요약 ===
 * 본 헤더는 매크로만 제공. 각 매크로의 세부 의미는 BSD queue(3) 맨페이지와
 * 동일. 아래 표는 자료구조별 지원 매크로 매트릭스.
 */

#ifndef SPDK_QUEUE_EXTRAS_H      /* [한국어] include 가드 시작 */
#define SPDK_QUEUE_EXTRAS_H

#ifdef __cplusplus
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
                                 /* [한국어] GCC 13+는 queue 매크로의 이중 포인터 재연결 패턴을 dangling pointer로 오탐
                                  *  - 이슈 #3030 참고. 실제로는 안전하지만 false positive가 빌드를 오염시킴
                                  *  - 해당 매크로 범위에서만 경고를 끄기 위해 pragma push/pop 헬퍼 정의 */
#define SPDK_QE_SUPPRESS_WARNINGS() do {				\
	_Pragma("GCC diagnostic push")					\
	_Pragma("GCC diagnostic ignored \"-Wdangling-pointer\"")	\
} while (0)
                                 /* [한국어] 현재 경고 설정을 스택에 push 후 -Wdangling-pointer를 비활성 */
#define SPDK_QE_UNSUPPRESS_WARNINGS() _Pragma("GCC diagnostic pop")
                                 /* [한국어] push된 경고 설정 복원 — 범위가 좁게 유지되어 다른 경고는 영향 없음 */
#else
#define SPDK_QE_SUPPRESS_WARNINGS()
                                 /* [한국어] GCC <13 또는 Clang: no-op */
#define SPDK_QE_UNSUPPRESS_WARNINGS()
#endif

/*
 * Singly-linked Tail queue declarations.
 */
                                 /* [한국어] ===== STAILQ (Singly-linked TAIL Queue) 확장 =====
                                  *  - head가 (first, &last->next)의 쌍 포인터를 가짐 → head/tail O(1) 접근
                                  *  - 각 엔트리는 next 포인터 하나만 — 메모리 절약, 임의 제거 O(n) */
#define	STAILQ_HEAD(name, type)						\
struct name {								\
	struct type *stqh_first;/* first element */			\
                                 /* [한국어] 첫 번째 엔트리 포인터 (NULL이면 빈 큐) */
	struct type **stqh_last;/* addr of last next element */		\
                                 /* [한국어] 마지막 엔트리의 next 필드 주소 — tail insert를 O(1)로 가능하게 함
                                  *  - 비어있을 때는 &stqh_first 자신을 가리킴 */
}

#define	STAILQ_HEAD_INITIALIZER(head)					\
	{ NULL, &(head).stqh_first }
                                 /* [한국어] 정적 초기화: 큐 비어 있음 상태 — stqh_last가 자기 stqh_first를 가리켜 일관성 유지 */

/*
 * Singly-linked Tail queue functions.
 */
#define	STAILQ_EMPTY(head)	((head)->stqh_first == NULL)
                                 /* [한국어] 큐가 비어 있는지 판정 — first == NULL */

#define	STAILQ_FIRST(head)	((head)->stqh_first)
                                 /* [한국어] 첫 엔트리 포인터 반환 */

#define	STAILQ_FOREACH_FROM(var, head, field)				\
	for ((var) = ((var) ? (var) : STAILQ_FIRST((head)));		\
	   (var);							\
	   (var) = STAILQ_NEXT((var), field))
                                 /* [한국어] var이 주어지면 그 지점부터 순회, NULL이면 처음부터 순회
                                  *  - "이어가기" 패턴: 중단 후 재개 시 유용 */

#define	STAILQ_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = STAILQ_FIRST((head));				\
	    (var) && ((tvar) = STAILQ_NEXT((var), field), 1);		\
	    (var) = (tvar))
                                 /* [한국어] 순회 중 var을 리스트에서 제거해도 안전 (다음 엔트리를 tvar에 미리 복사)
                                  *  - 콤마 연산자: (tvar = next, 1) 로 대입 후 루프 조건은 항상 1 → 단순 순회 유지 */

#define	STAILQ_FOREACH_FROM_SAFE(var, head, field, tvar)		\
	for ((var) = ((var) ? (var) : STAILQ_FIRST((head)));		\
	    (var) && ((tvar) = STAILQ_NEXT((var), field), 1);		\
	    (var) = (tvar))
                                 /* [한국어] FROM + SAFE 조합 — 이어가기 + 제거 안전 */

#define	STAILQ_LAST(head, type, field)					\
	(STAILQ_EMPTY((head)) ? NULL :					\
	    SPDK_CONTAINEROF((head)->stqh_last, struct type, field.stqe_next))
                                 /* [한국어] 마지막 엔트리 포인터 — stqh_last(=last->next 필드 주소)에서 container_of로 구조체 복원
                                  *  - SPDK_CONTAINEROF: spdk/util.h 제공 */

#define	STAILQ_NEXT(elm, field)	((elm)->field.stqe_next)
                                 /* [한국어] 다음 엔트리 포인터 — 연결고리 필드 접근 */

#define STAILQ_REMOVE_AFTER(head, elm, field) do {			\
	if ((STAILQ_NEXT(elm, field) =					\
	     STAILQ_NEXT(STAILQ_NEXT(elm, field), field)) == NULL)	\
		(head)->stqh_last = &STAILQ_NEXT((elm), field);		\
} while (0)
                                 /* [한국어] elm 다음 엔트리를 리스트에서 제거 — elm->next를 next->next로 재연결
                                  *  - 제거 대상이 tail이면 head의 stqh_last를 elm의 next로 업데이트 */

#define STAILQ_SWAP(head1, head2, type) do {				\
	struct type *swap_first = STAILQ_FIRST(head1);			\
	struct type **swap_last = (head1)->stqh_last;			\
	STAILQ_FIRST(head1) = STAILQ_FIRST(head2);			\
	(head1)->stqh_last = (head2)->stqh_last;			\
	STAILQ_FIRST(head2) = swap_first;				\
	(head2)->stqh_last = swap_last;					\
	if (STAILQ_EMPTY(head1))					\
		(head1)->stqh_last = &STAILQ_FIRST(head1);		\
	if (STAILQ_EMPTY(head2))					\
		(head2)->stqh_last = &STAILQ_FIRST(head2);		\
} while (0)
                                 /* [한국어] 두 STAILQ head의 내용을 교환 — (first, last) 포인터 쌍을 swap
                                  *  - swap 후 비어진 쪽은 stqh_last 무효화 방지 위해 자기 first 주소로 재설정 */

/*
 * List declarations.
 */
                                 /* [한국어] ===== LIST (doubly-linked list, head-only 포인터) ===== */
#define	LIST_HEAD(name, type)						\
struct name {								\
	struct type *lh_first;	/* first element */			\
                                 /* [한국어] head는 첫 엔트리 포인터 하나만. 마지막 엔트리 O(1) 접근 불가 */
}

#define	LIST_HEAD_INITIALIZER(head)					\
	{ NULL }
                                 /* [한국어] 정적 초기화: NULL head */

#define	LIST_ENTRY(type)						\
struct {								\
	struct type *le_next;	/* next element */			\
                                 /* [한국어] 다음 엔트리 포인터 — NULL이면 마지막 */
	struct type **le_prev;	/* address of previous next element */	\
                                 /* [한국어] 이전 엔트리의 le_next 필드 주소 (또는 head의 lh_first 주소)
                                  *  - 이중 포인터 관용구로 head 수정도 일반 노드 수정과 동일 경로로 처리 */
}

/*
 * List functions.
 */

#if (defined(_KERNEL) && defined(INVARIANTS))
                                 /* [한국어] BSD 커널에서 INVARIANTS 빌드일 때만 리스트 무결성 검사 활성
                                  *  - SPDK 유저스페이스 빌드에서는 결코 활성화되지 않음 → 아래 else 블록의 no-op 버전 사용 */
#define	QMD_LIST_CHECK_HEAD(head, field) do {				\
	if (LIST_FIRST((head)) != NULL &&				\
	    LIST_FIRST((head))->field.le_prev !=			\
	     &LIST_FIRST((head)))					\
		panic("Bad list head %p first->prev != head", (head));	\
} while (0)
                                 /* [한국어] head가 비어있지 않고 first의 le_prev가 head->lh_first 주소와 불일치하면 panic */

#define	QMD_LIST_CHECK_NEXT(elm, field) do {				\
	if (LIST_NEXT((elm), field) != NULL &&				\
	    LIST_NEXT((elm), field)->field.le_prev !=			\
	     &((elm)->field.le_next))					\
		panic("Bad link elm %p next->prev != elm", (elm));	\
} while (0)
                                 /* [한국어] elm의 다음 엔트리의 le_prev가 정확히 elm의 le_next 필드를 가리키는지 검증 */

#define	QMD_LIST_CHECK_PREV(elm, field) do {				\
	if (*(elm)->field.le_prev != (elm))				\
		panic("Bad link elm %p prev->next != elm", (elm));	\
} while (0)
                                 /* [한국어] 이전 엔트리의 next가 elm과 일치하는지 */
#else
#define	QMD_LIST_CHECK_HEAD(head, field)
#define	QMD_LIST_CHECK_NEXT(elm, field)
#define	QMD_LIST_CHECK_PREV(elm, field)
                                 /* [한국어] 유저스페이스: 무동작 */
#endif /* (_KERNEL && INVARIANTS) */

#define	LIST_EMPTY(head)	((head)->lh_first == NULL)
                                 /* [한국어] 빈 리스트 판정 */

#define	LIST_FIRST(head)	((head)->lh_first)
                                 /* [한국어] 첫 엔트리 */

#define	LIST_FOREACH_FROM(var, head, field)				\
	for ((var) = ((var) ? (var) : LIST_FIRST((head)));		\
	    (var);							\
	    (var) = LIST_NEXT((var), field))
                                 /* [한국어] var부터 순회 (NULL이면 첫 엔트리부터) */

#define	LIST_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = LIST_FIRST((head));				\
	    (var) && ((tvar) = LIST_NEXT((var), field), 1);		\
	    (var) = (tvar))
                                 /* [한국어] 순회 중 제거 안전 */

#define	LIST_FOREACH_FROM_SAFE(var, head, field, tvar)			\
	for ((var) = ((var) ? (var) : LIST_FIRST((head)));		\
	    (var) && ((tvar) = LIST_NEXT((var), field), 1);		\
	    (var) = (tvar))
                                 /* [한국어] FROM + SAFE 조합 */

#define	LIST_NEXT(elm, field)	((elm)->field.le_next)
                                 /* [한국어] 다음 엔트리 */

#define	LIST_PREV(elm, head, type, field)				\
	((elm)->field.le_prev == &LIST_FIRST((head)) ? NULL :		\
	    SPDK_CONTAINEROF((elm)->field.le_prev, struct type, field.le_next))
                                 /* [한국어] 이전 엔트리 복원
                                  *  - elm이 first이면 le_prev는 head의 lh_first 주소 → NULL 반환
                                  *  - 그 외엔 le_prev(이전의 le_next 필드 주소)에서 container_of */

#define LIST_SWAP(head1, head2, type, field) do {			\
	struct type *swap_tmp = LIST_FIRST((head1));			\
	LIST_FIRST((head1)) = LIST_FIRST((head2));			\
	LIST_FIRST((head2)) = swap_tmp;					\
	if ((swap_tmp = LIST_FIRST((head1))) != NULL)			\
		swap_tmp->field.le_prev = &LIST_FIRST((head1));		\
	if ((swap_tmp = LIST_FIRST((head2))) != NULL)			\
		swap_tmp->field.le_prev = &LIST_FIRST((head2));		\
} while (0)
                                 /* [한국어] 두 LIST head의 first 포인터 교환 후 각 리스트의 첫 엔트리 le_prev를 새 head 주소로 재설정 */

/*
 * Singly-linked List functions.
 */
                                 /* [한국어] ===== SLIST 확장 ===== */
#define	SLIST_SWAP(head1, head2, type) do {			\
	struct type *swap_tmp = SLIST_FIRST((head1));			\
	SLIST_FIRST((head1)) = SLIST_FIRST((head2));			\
	SLIST_FIRST((head2)) = swap_tmp;				\
} while (0)
                                 /* [한국어] SLIST head의 first 포인터만 교환 (SLIST는 이전 포인터 없음 — 단순) */

#define	SLIST_FOREACH_SAFE(var, head, field, tvar)		\
	for ((var) = SLIST_FIRST((head));				\
	    (var) && ((tvar) = SLIST_NEXT((var), field), 1);		\
	    (var) = (tvar))
                                 /* [한국어] SLIST 순회 중 제거 안전 */

/*
 * Tail queue functions.
 */
                                 /* [한국어] ===== TAILQ 확장 ===== */
#if (defined(_KERNEL) && defined(INVARIANTS))
                                 /* [한국어] BSD 커널 디버그 빌드 전용 invariant 검사 — 유저스페이스에선 미사용 */
#define	QMD_TAILQ_CHECK_HEAD(head, field) do {				\
	if (!TAILQ_EMPTY(head) &&					\
	    TAILQ_FIRST((head))->field.tqe_prev !=			\
	     &TAILQ_FIRST((head)))					\
		panic("Bad tailq head %p first->prev != head", (head));	\
} while (0)
                                 /* [한국어] first의 tqe_prev가 head의 tqh_first 주소와 같아야 정상 */

#define	QMD_TAILQ_CHECK_TAIL(head, field) do {				\
	if (*(head)->tqh_last != NULL)					\
		panic("Bad tailq NEXT(%p->tqh_last) != NULL", (head));	\
} while (0)
                                 /* [한국어] last->next는 반드시 NULL이어야 정상 (tail 불변식) */

#define	QMD_TAILQ_CHECK_NEXT(elm, field) do {				\
	if (TAILQ_NEXT((elm), field) != NULL &&				\
	    TAILQ_NEXT((elm), field)->field.tqe_prev !=			\
	     &((elm)->field.tqe_next))					\
		panic("Bad link elm %p next->prev != elm", (elm));	\
} while (0)
                                 /* [한국어] next의 tqe_prev는 elm의 tqe_next 주소여야 정상 */

#define	QMD_TAILQ_CHECK_PREV(elm, field) do {				\
	if (*(elm)->field.tqe_prev != (elm))				\
		panic("Bad link elm %p prev->next != elm", (elm));	\
} while (0)
                                 /* [한국어] prev->next가 elm을 가리켜야 정상 */
#else
#define	QMD_TAILQ_CHECK_HEAD(head, field)
#define	QMD_TAILQ_CHECK_TAIL(head, headname)
#define	QMD_TAILQ_CHECK_NEXT(elm, field)
#define	QMD_TAILQ_CHECK_PREV(elm, field)
                                 /* [한국어] 유저스페이스: 무동작 */
#endif /* (_KERNEL && INVARIANTS) */

#define	TAILQ_EMPTY(head)	((head)->tqh_first == NULL)
                                 /* [한국어] TAILQ 빈 판정 */

#define	TAILQ_FIRST(head)	((head)->tqh_first)
                                 /* [한국어] 첫 엔트리 */

#define	TAILQ_FOREACH_FROM(var, head, field)				\
	for ((var) = ((var) ? (var) : TAILQ_FIRST((head)));		\
	    (var);							\
	    (var) = TAILQ_NEXT((var), field))
                                 /* [한국어] var부터 순회 */

#define	TAILQ_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = TAILQ_FIRST((head));				\
	    (var) && ((tvar) = TAILQ_NEXT((var), field), 1);		\
	    (var) = (tvar))
                                 /* [한국어] 제거 안전 순회 */

#define	TAILQ_FOREACH_FROM_SAFE(var, head, field, tvar)			\
	for ((var) = ((var) ? (var) : TAILQ_FIRST((head)));		\
	    (var) && ((tvar) = TAILQ_NEXT((var), field), 1);		\
	    (var) = (tvar))
                                 /* [한국어] FROM + SAFE */

#define	TAILQ_FOREACH_REVERSE_FROM(var, head, headname, field)		\
	for ((var) = ((var) ? (var) : TAILQ_LAST((head), headname));	\
	    (var);							\
	    (var) = TAILQ_PREV((var), headname, field))
                                 /* [한국어] 역방향 순회, var부터 (NULL이면 tail부터) */

#define	TAILQ_FOREACH_REVERSE_SAFE(var, head, headname, field, tvar)	\
	for ((var) = TAILQ_LAST((head), headname);			\
	    (var) && ((tvar) = TAILQ_PREV((var), headname, field), 1);	\
	    (var) = (tvar))
                                 /* [한국어] 역방향 + 제거 안전 */

#define	TAILQ_FOREACH_REVERSE_FROM_SAFE(var, head, headname, field, tvar) \
	for ((var) = ((var) ? (var) : TAILQ_LAST((head), headname));	\
	    (var) && ((tvar) = TAILQ_PREV((var), headname, field), 1);	\
	    (var) = (tvar))
                                 /* [한국어] 역방향 + FROM + 제거 안전 */

#define	TAILQ_LAST(head, headname)					\
	(*(((struct headname *)((head)->tqh_last))->tqh_last))
                                 /* [한국어] tail 엔트리 반환 — tqh_last는 last->next 주소(=마지막 엔트리의 tqe_entry.tqe_next 주소)
                                  *  - 이를 headname 캐스팅해 tqh_last 다시 역참조하면 마지막 엔트리 구조체 포인터 복원 */

#define	TAILQ_NEXT(elm, field) ((elm)->field.tqe_next)
                                 /* [한국어] 다음 엔트리 */

#define	TAILQ_PREV(elm, headname, field)				\
	(*(((struct headname *)((elm)->field.tqe_prev))->tqh_last))
                                 /* [한국어] 이전 엔트리 — tqe_prev는 prev의 tqe_next 주소 → headname 캐스팅해 재해석 */

#define TAILQ_SWAP(head1, head2, type, field) do {			\
	SPDK_QE_SUPPRESS_WARNINGS();					\
                                 /* [한국어] GCC 13 false positive 억제 시작 */
	struct type *swap_first = (head1)->tqh_first;			\
	struct type **swap_last = (head1)->tqh_last;			\
	(head1)->tqh_first = (head2)->tqh_first;			\
	(head1)->tqh_last = (head2)->tqh_last;				\
	(head2)->tqh_first = swap_first;				\
	(head2)->tqh_last = swap_last;					\
	if ((swap_first = (head1)->tqh_first) != NULL)			\
		swap_first->field.tqe_prev = &(head1)->tqh_first;	\
	else								\
		(head1)->tqh_last = &(head1)->tqh_first;		\
                                 /* [한국어] head1 교환 후 비었으면 tqh_last를 자기 first 주소로 — 불변식 유지 */
	if ((swap_first = (head2)->tqh_first) != NULL)			\
		swap_first->field.tqe_prev = &(head2)->tqh_first;	\
	else								\
		(head2)->tqh_last = &(head2)->tqh_first;		\
	SPDK_QE_UNSUPPRESS_WARNINGS();					\
                                 /* [한국어] 경고 설정 복원 */
} while (0)

#ifdef __cplusplus
}
#endif

#endif                           /* [한국어] include 가드 종료 */
