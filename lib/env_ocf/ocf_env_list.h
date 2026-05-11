/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] OCF 환경(env) — 리눅스 커널 list_head 모방 매크로/인라인 구현 (ocf_env_list.h)
 *
 * === 파일의 역할 ===
 * libocf는 자신의 자료구조(캐시 인스턴스 리스트, 코어 리스트 등)에 리눅스 커널의 doubly-linked list
 * (`struct list_head` 기반)를 그대로 사용한다. SPDK는 유저스페이스이므로 커널 헤더(linux/list.h)를
 * 쓸 수 없어, 본 파일에서 동일한 시맨틱을 가진 list_head/INIT_LIST_HEAD/list_add/...를 자체 구현한다.
 * 즉 본 파일은 "환경 호환 셔틀" — 코어 알고리즘은 OCF가 제공하고, 필드/매크로 시맨틱만 호스트가 맞춘다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   libocf 코어 (struct list_head를 멤버로 가진 자료구조: 예 ocf_cache, ocf_core)
 *     → 본 헤더의 매크로/인라인 사용
 * 커널 list.h와의 차이: list_for_each(...) 종료 조건이 약간 다름(커널 원본의 head==pos 대신
 *  next 포인터 비교를 함) — 안전하게 사용하려면 본 파일의 정의 그대로 따라야 한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: 표준 stddef.h의 offsetof — 보통 ocf_env_headers.h가 먼저 include되어 stdinc로 노출됨.
 * - 의존받음: lib/env_ocf/ 어댑터(특히 ocf_env.h)와 그를 통한 libocf 코어.
 * - 데이터 흐름: 자료구조 멤버로 임베드된 list_head를 매크로로 순회/삽입/삭제.
 * - 공유 자료구조: struct list_head — prev/next 한 쌍의 64B 정렬 노드.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct list_head    : prev/next 포인터 한 쌍 (64B 정렬로 false-sharing 회피).
 * - INIT_LIST_HEAD      : 자기 자신을 가리키는 빈 리스트 초기화.
 * - list_add/_tail      : head 또는 tail에 삽입.
 * - list_empty          : next==self이면 true.
 * - list_del            : 노드를 리스트에서 끊어냄(인접 노드 prev/next만 조정).
 * - list_move/_tail     : 한 리스트에서 다른 리스트로 이동(=del+add).
 * - list_entry/_first_entry : 임베드된 list_head 포인터 → 컨테이너 구조체 포인터(container_of 패턴).
 * - list_for_each / _safe / _entry / _entry_safe : 순회 매크로(_safe는 순회 중 삭제 허용).
 */

#ifndef __OCF_LIST_H__
#define __OCF_LIST_H__
/* [한국어] 다중 포함 가드 — 본 헤더가 같은 TU에서 두 번 이상 include되어도 안전. */

#define LIST_POISON1  ((void *) 0x00100100)
/* [한국어] 디버그용 sentinel — list_del 후 next 포인터를 이 값으로 덮어 두면 dangling 접근을 빠르게 노출.
 * 0x00100100은 의도적으로 정렬 위반/낮은 가상 주소 → fault 유도. (커널 원본과 동일 컨벤션.) */
#define LIST_POISON2  ((void *) 0x00200200)
/* [한국어] LIST_POISON1과 짝 — prev 포인터 덮어쓰기용 sentinel. */

/**
 * List entry structure mimicking linux kernel based one.
 */
struct list_head {
	/* [한국어] 커널 list.h의 struct list_head 모방 — 자료구조에 임베드되어 doubly-linked list 노드 역할.
	 * 자기 자신을 가리키면 빈 리스트(empty). 64B aligned로 두어 인접 노드와 같은 캐시라인을 공유하지 않게 함
	 * (false-sharing 회피 — 멀티코어 환경에서 prev/next 변경이 다른 노드의 캐시라인을 invalidate하는 것 방지). */

	struct list_head *next;
	/* [한국어] 다음 노드(혹은 head 자기 자신 — 빈 리스트) 포인터.
	 * 설정자: list_add/_tail/_del/_move 등 모든 갱신 함수.
	 * 읽는 자: 순회 매크로(list_for_each*) 및 list_empty.
	 * 동기화: 본 헤더는 lock-free가 아님 — 사용처가 외부에서 lock(또는 thread 고정)을 보장해야 함. */

	struct list_head *prev;
	/* [한국어] 이전 노드 포인터 — doubly-linked로 양방향 순회/삽입/삭제 모두 O(1).
	 * 설정자/읽는 자/동기화: next와 동일한 규약. */
} __attribute__((aligned(64)));
/* [한국어] 64B 캐시라인 정렬 — 인접 list_head가 같은 캐시라인에 있지 않도록. */

/**
 * start an empty list
 */
#define INIT_LIST_HEAD(l) { (l)->prev = l; (l)->next = l; }
/* [한국어] 리스트를 "빈 상태"로 초기화 — head가 자기 자신을 가리키는 원형 리스트.
 * list_empty(l)이 true 반환하도록 next==l 보장. */

/**
 * Add item to list head.
 * @param it list entry to be added
 * @param l1 list main node (head)
 */
/*
 * [한국어]
 * list_add - 노드 it를 리스트 head(l1) 바로 뒤에 삽입(=리스트 앞쪽).
 *
 * @it : 새로 끼워 넣을 노드 (현재 어떤 리스트에도 속해 있으면 안 됨 — 그렇지 않으면 손상).
 * @l1 : 대상 리스트 head.
 *
 * 4번의 포인터 업데이트로 O(1) 삽입. 동기화는 호출자 책임.
 */
static inline void
list_add(struct list_head *it, struct list_head *l1)
{
	it->prev = l1;
	/* [한국어] 새 노드의 prev를 head로 — 새 노드가 head 바로 뒤에 들어감. */
	it->next = l1->next;
	/* [한국어] 새 노드의 next를 기존 첫 노드로 — 체인의 자기 자리 확보. */

	l1->next->prev = it;
	/* [한국어] 기존 첫 노드의 prev를 새 노드로 갱신 — 양방향 링크 완성. */
	l1->next = it;
	/* [한국어] head의 next를 새 노드로 — head 바로 뒤가 새 노드가 됨. */
}

/**
 * Add item it to tail.
 * @param it list entry to be added
 * @param l1 list main node (head)
 */
/*
 * [한국어]
 * list_add_tail - 노드 it를 리스트 head(l1) 바로 앞(=리스트 끝)에 삽입.
 *
 * @it : 새로 끼워 넣을 노드.
 * @l1 : 대상 리스트 head.
 *
 * FIFO 큐 패턴(끝에 enqueue, 앞에서 dequeue)에 자주 쓰임.
 */
static inline void
list_add_tail(struct list_head *it, struct list_head *l1)
{
	it->prev = l1->prev;
	/* [한국어] 새 노드의 prev = 기존 마지막 노드. */
	it->next = l1;
	/* [한국어] 새 노드의 next = head — 원형 리스트의 끝 표지. */

	l1->prev->next = it;
	/* [한국어] 기존 마지막 노드의 next를 새 노드로. */
	l1->prev = it;
	/* [한국어] head의 prev를 새 노드로 — 새 노드가 마지막이 됨. */
}

/**
 * check if a list is empty (return true)
 */
/*
 * [한국어]
 * list_empty - 리스트가 비어 있으면 0이 아닌 값(true) 반환.
 *
 * @it : head 노드.
 * @return : 빈 리스트 여부 (next==self이면 true).
 */
static inline int
list_empty(struct list_head *it)
{
	return it->next == it;
	/* [한국어] INIT_LIST_HEAD 직후엔 next==self이므로 true — 그 외엔 false. */
}

/**
 * delete an entry from a list
 */
/*
 * [한국어]
 * list_del - 노드 it를 리스트에서 끊어냄(인접 노드만 갱신, it 자체는 이후 사용 금지).
 *
 * @it : 끊어낼 노드 (호출 후 it->next/prev는 댕글링 — 재초기화/재삽입 전엔 접근 금지).
 *
 * 본 구현은 it 자체에 LIST_POISON을 넣지 않으므로(원본 커널 list.h와 차이), 사용자는
 * 끊은 직후 노드를 재사용한다면 INIT_LIST_HEAD로 재초기화해야 한다.
 */
static inline void
list_del(struct list_head *it)
{
	it->next->prev = it->prev;
	/* [한국어] 다음 노드의 prev를 이전 노드로 — it를 우회. */
	it->prev->next = it->next;
	/* [한국어] 이전 노드의 next를 다음 노드로 — 우회 양방향 완성. */
}

/*
 * [한국어]
 * list_move_tail - list를 현재 리스트에서 떼서 head 리스트의 끝(tail)으로 이동.
 *
 * @list : 이동할 노드 (한 리스트의 일원).
 * @head : 새로 들어갈 리스트 head.
 *
 * 캐시 LRU/MRU 갱신 같이 "최근 사용 항목을 끝으로" 패턴에 사용.
 */
static inline void
list_move_tail(struct list_head *list,
	       struct list_head *head)
{
	list_del(list);
	/* [한국어] 현재 리스트에서 분리. */
	list_add_tail(list, head);
	/* [한국어] head 리스트의 끝에 삽입. */
}

/*
 * [한국어]
 * list_move - list를 현재 리스트에서 떼서 head 리스트의 앞(첫 노드)으로 이동.
 *
 * @list : 이동할 노드.
 * @head : 새 리스트 head.
 */
static inline void
list_move(struct list_head *list,
	  struct list_head *head)
{
	list_del(list);
	/* [한국어] 분리 후 list_add(앞 삽입)로 재배치. */
	list_add(list, head);
}

/**
 * Extract an entry.
 * @param list_head_i list head item, from which entry is extracted
 * @param item_type type (struct) of list entry
 * @param field_name name of list_head field within item_type
 */
#define list_entry(list_head_i, item_type, field_name) \
	(item_type *)(((void*)(list_head_i)) - offsetof(item_type, field_name))
/* [한국어] container_of 패턴 — 임베드된 list_head 멤버 주소에서 컨테이너 구조체 시작 주소를 역산.
 * (item_type 안 field_name 멤버의 오프셋만큼 빼면 컨테이너의 시작 주소가 나옴.)
 * (void*) 캐스팅은 산술 시 1바이트 단위 보장. */

#define list_first_entry(list_head_i, item_type, field_name) \
	list_entry((list_head_i)->next, item_type, field_name)
/* [한국어] head 다음 노드(=첫 entry)의 컨테이너 포인터. 빈 리스트에서 호출하면 head 자기 자신을 컨테이너로
 * 잘못 해석하므로, 호출 전에 list_empty 검사 권장. */

/**
 * @param iterator uninitialized list_head pointer, to be used as iterator
 * @param plist list head (main node)
 */
#define list_for_each(iterator, plist) \
	for (iterator = (plist)->next; \
	     (iterator)->next != (plist)->next; \
	     iterator = (iterator)->next)
/* [한국어] 리스트 순회 매크로(원시 list_head 단위).
 * 시작: head 다음 노드.
 * 종료: 다음 노드의 next가 "head 다음 노드"가 되었을 때 — 즉 한 바퀴 돌아 첫 노드 직전 위치.
 *  (커널 원본은 "iterator != head"로 종료하지만 여기선 다른 형태 — 사용 시 정확히 이 매크로의 시맨틱을 따라야 함.)
 * 진행: iterator = next.
 * 순회 중 iterator 노드를 삭제하면 next 포인터가 무효화되므로 list_for_each_safe 사용 필요. */

/**
 * Safe version of list_for_each which works even if entries are deleted during
 * loop.
 * @param iterator uninitialized list_head pointer, to be used as iterator
 * @param q another uninitialized list_head, used as helper
 * @param plist list head (main node)
 */
/*
 * Algorithm handles situation, where q is deleted.
 * consider in example 3 element list with header h:
 *
 *   h -> 1 -> 2 -> 3 ->
 *1.      i    q
 *
 *2.           i    q
 *
 *3. q              i
 */
#define list_for_each_safe(iterator, q, plist) \
	for (iterator = (q = (plist)->next->next)->prev; \
	     (q) != (plist)->next; \
	     iterator = (q = (q)->next)->prev)
/* [한국어] 순회 중 현재 노드를 삭제해도 안전한 버전.
 * 핵심: q가 항상 iterator의 한 칸 앞을 가리키게 유지 — iterator를 삭제해도 q가 next 정보 보유.
 * 시작: q = head->next->next, iterator = q->prev (=head->next, 첫 노드).
 * 종료: q == head->next (=한 바퀴 돌아 첫 노드를 다시 가리키게 됨).
 * 진행: q = q->next, iterator = q->prev. */

#define _list_entry_helper(item, head, field_name) list_entry(head, typeof(*item), field_name)
/* [한국어] list_entry 헬퍼 — typeof(*item)으로 컨테이너 타입을 추론해 매크로 사용 시 타입을 매번 적지 않게 함.
 * GCC 확장(typeof)에 의존. */

/**
 * Iterate over list entries.
 * @param list pointer to list item (iterator)
 * @param plist pointer to list_head item
 * @param field_name name of list_head field in list entry
 */
#define list_for_each_entry(item, plist, field_name) \
	for (item = _list_entry_helper(item, (plist)->next, field_name); \
	     _list_entry_helper(item, (item)->field_name.next, field_name) !=\
		     _list_entry_helper(item, (plist)->next, field_name); \
	     item = _list_entry_helper(item, (item)->field_name.next, field_name))
/* [한국어] 컨테이너 단위 순회 — list_for_each + list_entry를 합친 고수준 매크로.
 * 시작: item = head 다음 노드의 컨테이너.
 * 종료: 다음 컨테이너가 "head->next의 컨테이너"가 되었을 때(원형 리턴 인지).
 * 순회 중 item 삭제는 안전하지 않음 — _safe 버전 사용. */

/**
 * Safe version of list_for_each_entry which works even if entries are deleted
 * during loop.
 * @param list pointer to list item (iterator)
 * @param q another pointer to list item, used as helper
 * @param plist pointer to list_head item
 * @param field_name name of list_head field in list entry
 */
#define list_for_each_entry_safe(item, q, plist, field_name)		\
	for (item = _list_entry_helper(item, (plist)->next, field_name), \
	     q = _list_entry_helper(item, (item)->field_name.next, field_name); \
	     _list_entry_helper(item, (item)->field_name.next, field_name) != \
		     _list_entry_helper(item, (plist)->next, field_name); \
	     item = q, q = _list_entry_helper(q, (q)->field_name.next, field_name))
/* [한국어] 컨테이너 단위의 안전 순회 — 현재 item을 삭제해도 q가 다음 컨테이너를 미리 보유.
 * 패턴: item을 처리(삭제 가능), 이후 item=q, q=q->next로 진행. */

#endif
/* [한국어] 다중 포함 가드 종결. */
