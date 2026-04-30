/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright 2002 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef	SPDK_TREE_H
#define SPDK_TREE_H
/* [한국어] 헤더 가드 — 다중 #include 시 중복 정의를 막는 표준 매크로 가드.
 *  이 파일은 매크로만 정의하므로 어디서든 #include "spdk/tree.h"가 가능하다. */

#include <sys/cdefs.h>
/* [한국어] BSD/glibc의 컴파일러 속성 매크로(__BEGIN_DECLS 등)를 위한 헤더.
 *  본 파일에서 직접 사용하지 않더라도, 원전 BSD sys/tree.h가 이 헤더를 의존하므로 포팅 호환성을 위해 유지된다. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 호스트에서 이 헤더를 #include할 때 C 링키지를 적용한다.
 *  SPDK는 C로 작성됐지만 C++ 사용자/테스트에서도 트리 매크로를 인스턴스화할 수 있도록 보호한다. */
#endif

/*
 * [한국어 설명] BSD sys/tree.h의 SPDK 래퍼 헤더 (tree.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 FreeBSD의 sys/tree.h(원작자 Niels Provos, 2002)에서 유래한 매크로 기반 generic 트리
 * 라이브러리를 SPDK 네임스페이스로 가져온 것이다. 두 종류의 자가 균형 이진 검색 트리 — Splay Tree와
 * Rank-Balanced Tree(약-AVL 변형, 흔히 RB-tree로 노출) — 를 C 매크로로 인스턴스화할 수 있도록
 * 제공한다. 사용자는 노드 구조체 안에 SPLAY_ENTRY/RB_ENTRY 필드를 임베드하고, SPLAY_HEAD/RB_HEAD로
 * 트리 헤드 타입을 만든 뒤, _PROTOTYPE/_GENERATE 매크로로 해당 트리 전용의 함수들을 컴파일러에
 * 풀어낸다. 매크로만으로 작성됐기 때문에 별도의 .c 파일이 필요 없으며, 모든 인스턴스가 strongly typed
 * 코드로 전개되어 함수 호출 오버헤드가 거의 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 코어 곳곳에서 정렬된 자료구조가 필요한 부분의 기반 자료구조이다. 사용 컨텍스트는 거의 항상
 * 단일 spdk_thread(reactor) 내부의 polled-mode 코드 경로이며, 트리 자체는 lockless인 SPDK
 * 스레딩 모델에 맞게 thread-affinity로 보호된다. 대표적 사용처:
 *   - lib/bdev/bdev.c             : bdev I/O가 채널 단위 트리에 등록 (locked range, qos 등)
 *   - lib/blob/blobstore.c        : blobstore의 cluster/page 인덱싱
 *   - lib/nvmf/subsystem.c        : NVMe-oF subsystem/namespace 정렬 인덱스
 *   - module/bdev/lvol/, ftl/ 등  : 볼륨/매핑 테이블의 정렬 인덱스
 * 외부 호출자는 보통 RB_INSERT/RB_FIND/RB_FOREACH 등의 사용자 매크로를 호출하면, 이 파일에서 _GENERATE
 * 매크로로 미리 풀어둔 inline/static 함수가 실제로 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일은 SPDK 어떤 라이브러리에도 의존하지 않으며 (헤더-only), 단일 표준 헤더 sys/cdefs.h만 끌어온다.
 * 반대로 매우 많은 SPDK 코어/모듈이 이 헤더에 의존한다. 데이터 흐름 관점에서는, 사용자 노드 구조체에
 * 임베드된 SPLAY_ENTRY/RB_ENTRY가 트리의 부모/좌/우 포인터를 들고 있으며, 트리 헤드는 단순히 root
 * 포인터만 보관한다. 즉, 노드 메모리는 외부 호출자가 관리하고(트리는 메모리 할당을 하지 않음), 트리는
 * 포인터 집합의 위상만을 관리한다. 비교 함수(cmp)는 사용자가 _PROTOTYPE/_GENERATE 시점에 함수
 * 포인터 또는 매크로로 주입한다.
 *
 * === 주요 함수/구조체 요약 ===
 *   - SPLAY_HEAD(name, type)        : 트리 헤드 구조체 정의 (root 포인터 1개)
 *   - SPLAY_ENTRY(type)             : 노드에 임베드되는 좌/우 자식 포인터 쌍
 *   - SPLAY_PROTOTYPE / SPLAY_GENERATE: 트리 전용 함수 선언/정의 매크로 (이름 충돌 방지를 위한 mangling)
 *   - SPLAY_INSERT/REMOVE/FIND/NEXT/MIN/MAX/FOREACH: 사용자 API
 *   - RB_HEAD(name, type)           : RB 트리 헤드 (root)
 *   - RB_ENTRY(type)                : 노드의 좌/우/부모 포인터 (부모 포인터의 하위 2비트는 색상 비트로 재활용)
 *   - RB_PROTOTYPE_*/RB_GENERATE_*  : INSERT_COLOR/REMOVE_COLOR 등 색상 균형 보조 함수까지 포함하여 분리 생성
 *   - RB_INSERT/REMOVE/FIND/NFIND/NEXT/PREV/MIN/MAX/REINSERT/FOREACH(_REVERSE/_SAFE/_FROM): 사용자 API
 *   - SPLAY_ROTATE_LEFT/RIGHT, RB_ROTATE_LEFT/RIGHT, _SPLAY 등: 회전·splay-step·재색칠을 수행하는 내부 헬퍼
 *
 * === Splay vs RB 선택 기준 (SPDK 관점) ===
 *   - Splay tree: 접근 패턴에 locality가 있고(특정 키가 반복 조회됨), worst-case가 아닌
 *     amortized O(log n)으로 충분한 곳에 유리하다. 자주 접근되는 노드가 루트 근처로 끌려가므로
 *     캐시 친화적이며, polled-mode 핫 패스에서 cache locality 이득이 있다. 단점은 모든 lookup이
 *     쓰기를 유발하므로 read-mostly 동시 접근(드물지만)에는 부적합.
 *   - Rank-balanced(RB-like) tree: 모든 연산이 worst-case O(log n) 보장. 트리 높이가 2 lg(n+1)
 *     이하로 묶여 있어 latency 분포가 균일하다. 또한 lookup이 read-only(자식 포인터를 변경하지 않음)
 *     이므로 const-correctness가 가능하다. SPDK는 latency 예측가능성이 중요한 경로에서 RB를 선호한다.
 *
 * === 매크로 기반 generic 패턴 ===
 *   1) 사용자가 SPLAY_HEAD(my_tree_t, my_node) 또는 RB_HEAD(...)로 트리 헤드 타입을 정의
 *   2) 노드 구조체에 SPLAY_ENTRY(my_node) / RB_ENTRY(my_node)를 임베드 (필드명: 예 "tree_link")
 *   3) 사용자가 cmp 함수 작성 — int my_cmp(struct my_node *a, struct my_node *b)
 *   4) 헤더에 SPLAY_PROTOTYPE(my_tree_t, my_node, tree_link, my_cmp) 호출 (선언만)
 *   5) 한 .c 파일에 SPLAY_GENERATE(... 동일 인자 ...) 호출 → 실제 함수 정의가 풀림
 *   6) 호출자 코드에서 SPLAY_INSERT(my_tree_t, &head, &node)로 사용
 *
 * 이 파일에서 'name##_SPLAY_INSERT'처럼 토큰 결합(##)으로 함수명을 mangling하므로 한 컴파일 단위에서
 * 여러 트리 인스턴스를 충돌 없이 공존시킬 수 있다.
 *
 * === 동시성/스레드 컨텍스트 ===
 * 이 파일이 정의하는 트리는 락이 없다. SPDK의 lockless 설계 원칙(스레드 어피니티 + 메시지 패싱)에 따라,
 * 한 트리 인스턴스는 한 spdk_thread(reactor)에서만 다뤄야 하며, 다른 스레드에서 접근이 필요하면
 * spdk_thread_send_msg를 통해 소유 스레드로 위임한다. 따라서 이 헤더의 매크로들은 메모리 배리어/원자
 * 연산을 사용하지 않는다.
 */

/* ------------------------------------------------------------------------- */
/* [한국어] 원본 BSD sys/tree.h의 알고리즘 개요 주석. 아래는 원문 영어 주석이며 이어서
 *  한국어 설명이 매크로 단위로 첨부된다. */
/* ------------------------------------------------------------------------- */
/*
 * This file defines data structures for different types of trees:
 * splay trees and rank-balanced trees.
 *
 * A splay tree is a self-organizing data structure.  Every operation
 * on the tree causes a splay to happen.  The splay moves the requested
 * node to the root of the tree and partly rebalances it.
 *
 * This has the benefit that request locality causes faster lookups as
 * the requested nodes move to the top of the tree.  On the other hand,
 * every lookup causes memory writes.
 *
 * The Balance Theorem bounds the total access time for m operations
 * and n inserts on an initially empty tree as O((m + n)lg n).  The
 * amortized cost for a sequence of m accesses to a splay tree is O(lg n);
 *
 * A rank-balanced tree is a binary search tree with an integer
 * rank-difference as an attribute of each pointer from parent to child.
 * The sum of the rank-differences on any path from a node down to null is
 * the same, and defines the rank of that node. The rank of the null node
 * is -1.
 *
 * Different additional conditions define different sorts of balanced
 * trees, including "red-black" and "AVL" trees.  The set of conditions
 * applied here are the "weak-AVL" conditions of Haeupler, Sen and Tarjan:
 *	- every rank-difference is 1 or 2.
 *	- the rank of any leaf is 1.
 *
 * For historical reasons, rank differences that are even are associated
 * with the color red (Rank-Even-Difference), and the child that a red edge
 * points to is called a red child.
 *
 * Every operation on a rank-balanced tree is bounded as O(lg n).
 * The maximum height of a rank-balanced tree is 2lg (n+1).
 */

/* ============================================================
 * [한국어] === Splay Tree 매크로 그룹 1: 타입 정의/초기화/접근자 ===
 *
 * 아래 매크로들은 Splay 트리의 "껍데기"를 만드는 데 쓰인다. 사용자는
 *   1) SPLAY_HEAD로 트리 헤드 타입을 만들고
 *   2) 노드 구조체에 SPLAY_ENTRY를 임베드한 뒤
 *   3) SPLAY_INIT/SPLAY_INITIALIZER로 빈 트리를 초기화한다.
 * SPLAY_LEFT/RIGHT/ROOT/EMPTY는 단순 접근자로, 매크로 본체에서도 자주 쓰인다.
 * ============================================================ */

/* [한국어] SPLAY_HEAD(name, type) — Splay 트리의 헤드 구조체를 정의한다.
 *  - name : 새로 정의할 헤드 구조체 이름 (예: SPLAY_HEAD(my_tree, my_node) → struct my_tree)
 *  - type : 트리 노드의 (앞으로 정의될) 구조체 이름. struct type * 만 사용되므로 이 시점에 완전 정의 불필요(전방 선언으로 충분).
 *  전개 결과: 단지 root 포인터 1개를 가진 struct. 즉 트리 자체의 메모리 풋프린트는 sizeof(void*).
 *  사용 예: SPLAY_HEAD(bdev_io_tree, spdk_bdev_io); 후에 struct bdev_io_tree my_head; 처럼 인스턴스 생성. */
#define SPLAY_HEAD(name, type)						\
struct name {								\
	struct type *sph_root; /* root of the tree */			\
	/* [한국어] 트리의 루트 노드 포인터. 빈 트리면 NULL.
	 *  Splay 연산이 일어날 때마다 (FIND 포함) 이 포인터가 갱신될 수 있다 — 따라서
	 *  SPLAY_FIND조차 트리에 쓰기를 발생시킨다는 점이 RB와의 핵심 차이. */ \
}

/* [한국어] SPLAY_INITIALIZER — 정적 초기화용 매크로. 전역/지역 변수에서
 *   struct my_tree h = SPLAY_INITIALIZER(&h); 처럼 사용. root 인자는 사용되지 않으나
 *   원전 BSD API와의 시그니처 일치를 위해 유지된다. */
#define SPLAY_INITIALIZER(root)						\
	{ NULL }

/* [한국어] SPLAY_INIT — 동적 초기화용. 이미 메모리에 잡힌 헤드의 root를 NULL로 만든다.
 *  do/while(0) 관용구는 매크로를 단일 statement처럼 안전하게 쓰기 위함(if-else 경계 사고 방지). */
#define SPLAY_INIT(root) do {						\
	(root)->sph_root = NULL;					\
} while (/* CONSTCOND */ 0)

/* [한국어] SPLAY_ENTRY(type) — Splay 노드가 트리에 연결되기 위해 사용자 노드 구조체에
 *   임베드해야 하는 익명 struct를 만드는 매크로. 부모 포인터가 없는 점이 RB_ENTRY와의 차이다
 *   (Splay 알고리즘은 top-down splay이므로 부모를 기록할 필요가 없다 → 메모리 절약).
 *  사용 예:
 *    struct my_node {
 *        int key;
 *        SPLAY_ENTRY(my_node) link;   // 사용자가 임의 필드명을 정함 (예: link)
 *    };
 *  이후 _PROTOTYPE/_GENERATE에서 'field' 인자로 'link'를 주면 매크로가 elm->link.spe_left 형태로 접근. */
#define SPLAY_ENTRY(type)						\
struct {								\
	struct type *spe_left; /* left element */			\
	/* [한국어] 좌측 자식 — 키가 더 작은 부분 트리. NULL이면 leaf의 좌측. */ \
	struct type *spe_right; /* right element */			\
	/* [한국어] 우측 자식 — 키가 더 큰 부분 트리. NULL이면 leaf의 우측. */ \
}

/* [한국어] SPLAY_LEFT(elm, field) — 노드 elm의 좌측 자식 포인터에 대한 lvalue 접근.
 *  field는 사용자가 SPLAY_ENTRY로 임베드한 필드명. 매크로 내부에서 빈번히 등장. */
#define SPLAY_LEFT(elm, field)		(elm)->field.spe_left
/* [한국어] SPLAY_RIGHT — 우측 자식 포인터 lvalue 접근자. */
#define SPLAY_RIGHT(elm, field)		(elm)->field.spe_right
/* [한국어] SPLAY_ROOT — 헤드의 루트 노드 포인터를 반환 (lvalue로도 사용 가능). */
#define SPLAY_ROOT(head)		(head)->sph_root
/* [한국어] SPLAY_EMPTY — 트리가 비었는지(root == NULL) 검사하는 단순 술어. O(1). */
#define SPLAY_EMPTY(head)		(SPLAY_ROOT(head) == NULL)

/* ============================================================
 * [한국어] === Splay Tree 매크로 그룹 2: 내부 헬퍼(회전/링크/조립) ===
 *
 * 아래 매크로들은 사용자가 직접 호출하지 않으며, _SPLAY 본체와 INSERT/REMOVE 코드에서
 * 호출되는 빌딩 블록이다. Sleator-Tarjan top-down splay 알고리즘은 검색 경로를 따라가며
 * 트리를 두 개의 보조 트리(left, right)로 분리한 뒤(LINKLEFT/LINKRIGHT) 마지막에 하나로
 * 합치는(ASSEMBLE) 식으로 동작한다. 회전(ROTATE_*)은 zig-zig/zig-zag 케이스에서 사용된다.
 * ============================================================ */

/* SPLAY_ROTATE_{LEFT,RIGHT} expect that tmp hold SPLAY_{RIGHT,LEFT} */
/* [한국어] SPLAY_ROTATE_RIGHT — 루트(R)와 그 좌측 자식(tmp)을 우회전하여 tmp가 새 루트가 된다.
 *  사전조건: tmp == SPLAY_LEFT(root, field) 이어야 한다 (호출 측 책임).
 *  변환:        R                     tmp
 *              / \                    / \
 *           tmp   c     →            a    R
 *           / \                          / \
 *          a   b                        b   c
 *  3단계: (1) R의 좌측을 tmp의 우측(b)으로 교체  (2) tmp의 우측을 R로  (3) head의 root를 tmp로. */
#define SPLAY_ROTATE_RIGHT(head, tmp, field) do {			\
	SPLAY_LEFT((head)->sph_root, field) = SPLAY_RIGHT(tmp, field);	\
	SPLAY_RIGHT(tmp, field) = (head)->sph_root;			\
	(head)->sph_root = tmp;						\
} while (/* CONSTCOND */ 0)

/* [한국어] SPLAY_ROTATE_LEFT — ROTATE_RIGHT의 거울 대칭. tmp(우측 자식)가 새 루트.
 *  사전조건: tmp == SPLAY_RIGHT(root, field). zig-zig 좌측 케이스에서 사용된다. */
#define SPLAY_ROTATE_LEFT(head, tmp, field) do {			\
	SPLAY_RIGHT((head)->sph_root, field) = SPLAY_LEFT(tmp, field);	\
	SPLAY_LEFT(tmp, field) = (head)->sph_root;			\
	(head)->sph_root = tmp;						\
} while (/* CONSTCOND */ 0)

/* [한국어] SPLAY_LINKLEFT — top-down splay에서 "현재 루트는 우측 보조 트리의 가장 작은 노드로
 *   가야 한다(=right tree에 attach)"는 단계의 거울 짝. 코드는:
 *     - tmp의 좌측에 현재 루트를 단다 (tmp는 right-tree의 leftmost-pointer)
 *     - tmp 변수를 현재 루트로 옮긴다 (다음 단계의 부모 후보)
 *     - 새 루트는 현재 루트의 좌측 서브트리.
 *   결과적으로 검색 키보다 큰 노드들이 서서히 right tree로 이전된다. */
#define SPLAY_LINKLEFT(head, tmp, field) do {				\
	SPLAY_LEFT(tmp, field) = (head)->sph_root;			\
	tmp = (head)->sph_root;						\
	(head)->sph_root = SPLAY_LEFT((head)->sph_root, field);		\
} while (/* CONSTCOND */ 0)

/* [한국어] SPLAY_LINKRIGHT — LINKLEFT의 거울 대칭. 검색 키보다 작은 노드들을
 *   left tree로 이전한다. tmp는 left-tree의 rightmost-pointer 역할. */
#define SPLAY_LINKRIGHT(head, tmp, field) do {				\
	SPLAY_RIGHT(tmp, field) = (head)->sph_root;			\
	tmp = (head)->sph_root;						\
	(head)->sph_root = SPLAY_RIGHT((head)->sph_root, field);	\
} while (/* CONSTCOND */ 0)

/* [한국어] SPLAY_ASSEMBLE — top-down splay의 마지막 단계. 분리된 left/right 보조 트리와
 *   현재 루트를 하나로 합친다.
 *   - left, right는 임시 sentinel(__node)의 좌/우 포인터를 통해 만들어진 보조 트리의 마지막 끝점(부모 포인터).
 *   - left의 우측 자식 ← (현재 루트의 좌측 서브트리)
 *   - right의 좌측 자식 ← (현재 루트의 우측 서브트리)
 *   - 새 루트의 좌측 ← node(=__node)의 우측 (= 분리된 left tree 전체의 루트)
 *   - 새 루트의 우측 ← node의 좌측 (= 분리된 right tree 전체의 루트)
 *   결과: elm에 가장 가까운 키를 가진 노드가 트리의 루트가 된다 (splay 결과). */
#define SPLAY_ASSEMBLE(head, node, left, right, field) do {		\
	SPLAY_RIGHT(left, field) = SPLAY_LEFT((head)->sph_root, field);	\
	SPLAY_LEFT(right, field) = SPLAY_RIGHT((head)->sph_root, field);\
	SPLAY_LEFT((head)->sph_root, field) = SPLAY_RIGHT(node, field);	\
	SPLAY_RIGHT((head)->sph_root, field) = SPLAY_LEFT(node, field);	\
} while (/* CONSTCOND */ 0)

/* ============================================================
 * [한국어] === Splay Tree 매크로 그룹 3: PROTOTYPE (선언 + inline 함수) ===
 *
 * SPLAY_PROTOTYPE 매크로는 헤더에 한 번 호출되어 트리 인스턴스 전용의 함수들을 선언/정의한다.
 *   - 외부에 정의되는 비-inline 함수는 SPLAY_GENERATE 매크로가 .c 파일에 한 번 풀어준다.
 *   - 짧은 helper(_SPLAY_FIND, _SPLAY_NEXT, _SPLAY_MIN_MAX)는 여기서 inline static으로 정의되어
 *     컴파일러가 호출 사이트에서 직접 인라이닝할 수 있게 한다 (핫 패스 성능).
 *
 * 인자:
 *   - name  : 트리 헤드 타입의 struct 이름 (mangled 함수명의 prefix가 된다)
 *   - type  : 노드 구조체 이름
 *   - field : 노드에 임베드된 SPLAY_ENTRY 필드명
 *   - cmp   : int (*)(struct type *a, struct type *b) 시그니처의 비교 함수/매크로
 *             음수: a < b, 0: a == b, 양수: a > b
 * ============================================================ */

/* Generates prototypes and inline functions */
/* [한국어] SPLAY_PROTOTYPE — 헤더용 매크로. 트리 전용 외부 함수들을 선언하고,
 *   small helper 들은 static inline으로 정의한다.
 *   __attribute__((unused))는 트리 인스턴스가 정의됐지만 일부 helper가 호출되지 않을 때
 *   GCC의 unused-function 경고를 억제하기 위함. */
#define SPLAY_PROTOTYPE(name, type, field, cmp)				\
void name##_SPLAY(struct name *, struct type *);			\
/* [한국어] 외부 정의(_GENERATE에서 풀림): elm의 키에 가장 가까운 노드를 root로 끌어올리는 splay 연산 본체. */ \
void name##_SPLAY_MINMAX(struct name *, int);				\
/* [한국어] 외부 정의: SPLAY_NEGINF/INF 인자로 호출되어 최소/최대 노드를 root로 끌어올림. */ \
struct type *name##_SPLAY_INSERT(struct name *, struct type *);		\
/* [한국어] 외부 정의: 노드 삽입. 이미 같은 키 존재 시 기존 노드 포인터 반환, 새로 삽입 시 NULL. */ \
struct type *name##_SPLAY_REMOVE(struct name *, struct type *);		\
/* [한국어] 외부 정의: 노드 제거. 제거된 노드 포인터 또는 NULL(없을 때). */ \
									\
/* Finds the node with the same key as elm */				\
/* [한국어] _SPLAY_FIND — 같은 키의 노드를 찾는다.
 *   동작: (1) 빈 트리면 NULL  (2) elm을 향해 splay 수행  (3) splay 후 root와 키 비교
 *   복잡도: amortized O(log n). FIND가 트리 구조를 변형(쓰기)한다는 점에 주의 — 이 때문에
 *           SPDK에서 const 트리 lookup이 필요한 곳은 RB_FIND를 쓴다.
 *   반환: 일치 시 root 포인터(=elm과 같은 키를 가진 기존 노드), 없으면 NULL. */ \
static __attribute__((unused)) __inline struct type *					\
name##_SPLAY_FIND(struct name *head, struct type *elm)			\
{									\
	if (SPLAY_EMPTY(head))						\
		/* [한국어] 빈 트리 — 즉시 미스. */ \
		return(NULL);						\
	name##_SPLAY(head, elm);					\
	/* [한국어] elm의 키에 가장 가까운 노드를 root로 끌어올림 (자가 조정). */ \
	if ((cmp)(elm, (head)->sph_root) == 0)				\
		/* [한국어] root가 정확히 elm과 같은 키이면 hit. */ \
		return (head->sph_root);				\
	return (NULL);							\
	/* [한국어] root가 가장 가까운 노드일 뿐 키가 다르면 miss. */ \
}									\
									\
/* [한국어] _SPLAY_NEXT — in-order 후속 노드를 반환.
 *   주의: SPLAY_FOREACH가 매 반복마다 SPLAY_NEXT를 호출하면 트리가 매번 변형되어 비용이 크다 →
 *         long 시퀀스 순회는 RB_FOREACH가 더 적합한 경우가 많다.
 *   동작: (1) elm을 root로 splay  (2) elm의 우측 서브트리의 가장 좌측 노드가 후속.
 *         우측이 NULL이면 후속이 없는 최대 노드 → NULL. */ \
static __attribute__((unused)) __inline struct type *					\
name##_SPLAY_NEXT(struct name *head, struct type *elm)			\
{									\
	name##_SPLAY(head, elm);					\
	/* [한국어] elm을 루트로 splay 시켜 후속 탐색의 시작점을 단순화. */ \
	if (SPLAY_RIGHT(elm, field) != NULL) {				\
		/* [한국어] 우측 서브트리가 존재하면 그 안의 leftmost가 in-order 후속. */ \
		elm = SPLAY_RIGHT(elm, field);				\
		while (SPLAY_LEFT(elm, field) != NULL) {		\
			/* [한국어] 좌측으로 끝까지 내려가서 가장 작은 노드 도달. */ \
			elm = SPLAY_LEFT(elm, field);			\
		}							\
	} else								\
		/* [한국어] 우측 자식이 없으면 elm이 최대 — 후속은 없음. */ \
		elm = NULL;						\
	return (elm);							\
}									\
									\
/* [한국어] _SPLAY_MIN_MAX — SPLAY_MIN/MAX 사용자 매크로의 backend.
 *   _SPLAY_MINMAX(head, NEGINF/INF)를 호출해 최소/최대 노드를 root로 splay한 뒤 root를 반환. */ \
static __attribute__((unused)) __inline struct type *					\
name##_SPLAY_MIN_MAX(struct name *head, int val)			\
{									\
	name##_SPLAY_MINMAX(head, val);					\
        return (SPLAY_ROOT(head));					\
}

/* ============================================================
 * [한국어] === Splay Tree 매크로 그룹 4: GENERATE (실제 함수 본체) ===
 *
 * 한 번의 SPLAY_GENERATE 호출이 4개의 함수 정의를 풀어낸다:
 *   1) name##_SPLAY_INSERT   : 신규 노드 삽입 (top-down splay 사용)
 *   2) name##_SPLAY_REMOVE   : 노드 제거 (좌측 서브트리에서 새 root를 끌어오는 분리·결합 방식)
 *   3) name##_SPLAY          : 핵심 splay 연산 (Sleator-Tarjan top-down splay)
 *   4) name##_SPLAY_MINMAX   : 최소/최대 노드를 root로 끌어올리는 splay 변형
 * 이 매크로는 .c 파일 한 곳에서만 호출해야 한다 (외부 함수 정의이므로 ODR 위반 방지).
 * ============================================================ */

/* Main splay operation.
 * Moves node close to the key of elm to top
 */
#define SPLAY_GENERATE(name, type, field, cmp)				\
/* [한국어] _SPLAY_INSERT — 노드를 트리에 삽입.
 *   복잡도: amortized O(log n).
 *   동작:
 *     - 빈 트리이면 elm이 곧 root.
 *     - 아니면 elm을 향해 splay → root에 가장 가까운 키가 올라옴 → 비교로 분기:
 *         < : 새 elm이 root의 좌측 서브트리를 이어받고, root는 elm의 우측 자식이 된다
 *         > : 거울 대칭
 *         == : 중복 — 새 노드를 삽입하지 않고 기존 root 반환
 *   반환: 중복 시 기존 노드, 신규 삽입 시 NULL. */ \
struct type *								\
name##_SPLAY_INSERT(struct name *head, struct type *elm)		\
{									\
    if (SPLAY_EMPTY(head)) {						\
	    /* [한국어] 빈 트리 — elm을 root로 직접 박기 위해 좌/우 자식 NULL 초기화. */ \
	    SPLAY_LEFT(elm, field) = SPLAY_RIGHT(elm, field) = NULL;	\
    } else {								\
	    int __comp;							\
	    name##_SPLAY(head, elm);					\
	    /* [한국어] elm의 키 인근 노드를 root로 끌어올린 뒤 비교. */ \
	    __comp = (cmp)(elm, (head)->sph_root);			\
	    if (__comp < 0) {						\
		    /* [한국어] 새 elm이 root보다 작음 — elm을 새 root로, 기존 root는 elm의 우측. */ \
		    SPLAY_LEFT(elm, field) = SPLAY_LEFT((head)->sph_root, field);\
		    /* [한국어] elm.left ← old_root.left  (좌측 서브트리 이전) */ \
		    SPLAY_RIGHT(elm, field) = (head)->sph_root;		\
		    /* [한국어] elm.right ← old_root  (기존 root와 그 우측 서브트리를 한 덩어리로) */ \
		    SPLAY_LEFT((head)->sph_root, field) = NULL;		\
		    /* [한국어] old_root.left ← NULL  (좌측은 elm으로 옮겨갔으므로) */ \
	    } else if (__comp > 0) {					\
		    /* [한국어] 거울 대칭: elm > root. */ \
		    SPLAY_RIGHT(elm, field) = SPLAY_RIGHT((head)->sph_root, field);\
		    SPLAY_LEFT(elm, field) = (head)->sph_root;		\
		    SPLAY_RIGHT((head)->sph_root, field) = NULL;	\
	    } else							\
		    /* [한국어] 동일 키 존재 — 삽입 실패, 기존 노드 반환. */ \
		    return ((head)->sph_root);				\
    }									\
    (head)->sph_root = (elm);						\
    /* [한국어] 모든 분기에서 마지막에 elm이 root가 된다. */ \
    return (NULL);							\
    /* [한국어] NULL 반환 = 신규 삽입 성공. */ \
}									\
									\
/* [한국어] _SPLAY_REMOVE — elm 키와 같은 노드를 트리에서 제거.
 *   복잡도: amortized O(log n).
 *   동작:
 *     1) elm을 향해 splay — 같은 키가 있으면 root로 올라옴.
 *     2) root가 좌측 자식이 없으면 → 우측 서브트리가 새 root.
 *     3) 좌측이 있으면: 우측을 임시 보관하고 좌측을 새 root로 만든 뒤, 한 번 더 splay 하여
 *        새 root의 우측이 비도록 한다. 그 자리에 보관해둔 우측 서브트리를 붙인다.
 *   반환: 제거된 elm 포인터(=같은 키 노드) 또는 NULL(없을 때). */ \
struct type *								\
name##_SPLAY_REMOVE(struct name *head, struct type *elm)		\
{									\
	struct type *__tmp;						\
	/* [한국어] 우측 서브트리 임시 보관용. */ \
	if (SPLAY_EMPTY(head))						\
		return (NULL);						\
	/* [한국어] 빈 트리 → 미스. */ \
	name##_SPLAY(head, elm);					\
	/* [한국어] 제거 대상 후보를 root로 끌어올림. */ \
	if ((cmp)(elm, (head)->sph_root) == 0) {			\
		/* [한국어] root가 elm과 같은 키 — 제거 가능. */ \
		if (SPLAY_LEFT((head)->sph_root, field) == NULL) {	\
			/* [한국어] 좌측 자식 없음 — 우측이 그대로 새 root. */ \
			(head)->sph_root = SPLAY_RIGHT((head)->sph_root, field);\
		} else {						\
			__tmp = SPLAY_RIGHT((head)->sph_root, field);	\
			/* [한국어] 우측 서브트리를 잠시 보관. */ \
			(head)->sph_root = SPLAY_LEFT((head)->sph_root, field);\
			/* [한국어] 좌측 서브트리를 새 root로 사용. */ \
			name##_SPLAY(head, elm);			\
			/* [한국어] 좌측 서브트리에서 다시 elm을 향해 splay → 새 root는 elm 직전 in-order
			 *   노드(좌측 서브트리의 max)이며, 그 우측 자식은 NULL이 보장된다. */ \
			SPLAY_RIGHT((head)->sph_root, field) = __tmp;	\
			/* [한국어] 비어있는 우측 자리에 처음에 보관해둔 우측 서브트리를 붙인다. */ \
		}							\
		return (elm);						\
		/* [한국어] 제거 성공 — 호출자가 노드 메모리를 해제할 수 있도록 elm 반환. */ \
	}								\
	return (NULL);							\
	/* [한국어] elm 키가 트리에 없음. */ \
}									\
									\
/* [한국어] _SPLAY — 핵심 top-down splay.
 *   목표: elm의 키와 가장 가까운 노드를 트리의 root로 끌어올린다.
 *   기법: 검색 경로를 따라가며 트리를 두 보조 트리(left, right)로 분리. zig-zig 케이스에서는
 *         미리 회전(ROTATE)을 적용해 트리 깊이를 줄인다. 마지막에 SPLAY_ASSEMBLE로 합친다.
 *   __node는 left/right 보조 트리의 sentinel — 진짜 노드가 아니라 부모 포인터 역할만 한다. */ \
void									\
name##_SPLAY(struct name *head, struct type *elm)			\
{									\
	struct type __node, *__left, *__right, *__tmp;			\
	/* [한국어] __node : 보조 트리의 sentinel(스택에 존재). __left/__right : 양 보조 트리의 끝점.
	 *  __tmp : 한 단계 아래 자식 포인터를 잠시 잡아두는 변수. */ \
	int __comp;							\
\
	SPLAY_LEFT(&__node, field) = SPLAY_RIGHT(&__node, field) = NULL;\
	/* [한국어] sentinel의 좌/우를 NULL로 초기화 — 후에 ASSEMBLE에서 분리된 보조 트리의 루트들을 가져온다. */ \
	__left = __right = &__node;					\
	/* [한국어] 처음에는 두 보조 트리가 비어있으므로 둘 다 sentinel 자체를 가리킴. */ \
\
	while ((__comp = (cmp)(elm, (head)->sph_root)) != 0) {		\
		/* [한국어] root와 elm 키가 일치할 때까지 (또는 NULL 자식을 만날 때까지) 내려간다. */ \
		if (__comp < 0) {					\
			/* [한국어] elm < root — 좌측으로 진행. */ \
			__tmp = SPLAY_LEFT((head)->sph_root, field);	\
			if (__tmp == NULL)				\
				break;					\
			/* [한국어] 좌측 자식이 없으면 도달 — 더 이상 splay 불가. */ \
			if ((cmp)(elm, __tmp) < 0){			\
				/* [한국어] zig-zig 케이스 (좌-좌): 미리 우회전으로 깊이 단축. */ \
				SPLAY_ROTATE_RIGHT(head, __tmp, field);	\
				if (SPLAY_LEFT((head)->sph_root, field) == NULL)\
					break;				\
				/* [한국어] 회전 후 새 root의 좌측이 NULL이면 더 갈 곳 없음. */ \
			}						\
			SPLAY_LINKLEFT(head, __right, field);		\
			/* [한국어] 현재 root를 right 보조 트리의 왼쪽 끝에 매단다 (큰 키들의 모음). */ \
		} else if (__comp > 0) {				\
			/* [한국어] elm > root — 우측으로 진행 (좌측 케이스의 거울 대칭). */ \
			__tmp = SPLAY_RIGHT((head)->sph_root, field);	\
			if (__tmp == NULL)				\
				break;					\
			if ((cmp)(elm, __tmp) > 0){			\
				/* [한국어] zig-zig (우-우) 좌회전. */ \
				SPLAY_ROTATE_LEFT(head, __tmp, field);	\
				if (SPLAY_RIGHT((head)->sph_root, field) == NULL)\
					break;				\
			}						\
			SPLAY_LINKRIGHT(head, __left, field);		\
			/* [한국어] 현재 root를 left 보조 트리의 오른쪽 끝에 매단다 (작은 키들의 모음). */ \
		}							\
	}								\
	SPLAY_ASSEMBLE(head, &__node, __left, __right, field);		\
	/* [한국어] 분리된 두 보조 트리와 현재 root를 하나로 합쳐 splay 완료. */ \
}									\
									\
/* Splay with either the minimum or the maximum element			\
 * Used to find minimum or maximum element in tree.			\
 */									\
/* [한국어] _SPLAY_MINMAX — _SPLAY와 같은 구조이지만 비교를 키 대신 상수(NEGINF/INF)로 강제하여
 *   루트가 항상 좌측(또는 우측)으로 내려가도록 하는 변형. 결과적으로 트리의 최소(또는 최대) 노드가 root로 splay 된다.
 *   참고: 본문 안의 'if (__comp < 0)'은 사실상 항상 같은 분기로 들어가도록 설계되어 있으나,
 *   _SPLAY와 시그니처/구조를 일관되게 맞추기 위해 동일한 골격을 유지한다. */ \
void name##_SPLAY_MINMAX(struct name *head, int __comp) \
{									\
	struct type __node, *__left, *__right, *__tmp;			\
	/* [한국어] _SPLAY와 동일한 sentinel/보조 트리 구성. */ \
\
	SPLAY_LEFT(&__node, field) = SPLAY_RIGHT(&__node, field) = NULL;\
	__left = __right = &__node;					\
\
	while (1) {							\
		if (__comp < 0) {					\
			/* [한국어] NEGINF — 항상 좌측으로 내려간다 (최소 노드를 향해). */ \
			__tmp = SPLAY_LEFT((head)->sph_root, field);	\
			if (__tmp == NULL)				\
				break;					\
			if (__comp < 0){				\
				/* [한국어] 항상 참인 분기 — 매 단계마다 zig-zig 우회전을 적용해 깊이 단축. */ \
				SPLAY_ROTATE_RIGHT(head, __tmp, field);	\
				if (SPLAY_LEFT((head)->sph_root, field) == NULL)\
					break;				\
			}						\
			SPLAY_LINKLEFT(head, __right, field);		\
		} else if (__comp > 0) {				\
			/* [한국어] INF — 항상 우측으로 내려간다 (최대 노드를 향해). */ \
			__tmp = SPLAY_RIGHT((head)->sph_root, field);	\
			if (__tmp == NULL)				\
				break;					\
			if (__comp > 0) {				\
				SPLAY_ROTATE_LEFT(head, __tmp, field);	\
				if (SPLAY_RIGHT((head)->sph_root, field) == NULL)\
					break;				\
			}						\
			SPLAY_LINKRIGHT(head, __left, field);		\
		}							\
	}								\
	SPLAY_ASSEMBLE(head, &__node, __left, __right, field);		\
	/* [한국어] 종료 시 root는 트리의 최소(또는 최대) 노드. */ \
}

/* ============================================================
 * [한국어] === Splay Tree 매크로 그룹 5: 사용자 API (얇은 래퍼) ===
 *
 * 사용자가 코드에서 호출하는 매크로들. 모두 name##_SPLAY_* 형태의
 * 트리 인스턴스 전용 함수에 dispatch 한다.
 * ============================================================ */

/* [한국어] _SPLAY_MINMAX 호출 시 비교 결과를 강제하기 위한 상수.
 *  -1: 항상 "elm < root" 처럼 동작 → 최소 노드를 향해 splay.
 *   1: 항상 "elm > root" → 최대 노드를 향해 splay. */
#define SPLAY_NEGINF	-1
#define SPLAY_INF	1

/* [한국어] SPLAY_INSERT(name, head, elm) — 노드 elm을 head 트리에 삽입.
 *  반환: NULL(신규 삽입) 또는 기존 동일 키 노드 포인터. */
#define SPLAY_INSERT(name, x, y)	name##_SPLAY_INSERT(x, y)
/* [한국어] SPLAY_REMOVE — 같은 키의 노드 제거. 반환: 제거된 노드 포인터 또는 NULL. */
#define SPLAY_REMOVE(name, x, y)	name##_SPLAY_REMOVE(x, y)
/* [한국어] SPLAY_FIND — 같은 키의 노드 탐색. 주의: 트리를 변형(splay)함. */
#define SPLAY_FIND(name, x, y)		name##_SPLAY_FIND(x, y)
/* [한국어] SPLAY_NEXT — y의 in-order 후속 노드 반환. 호출 시 트리 변형 발생. */
#define SPLAY_NEXT(name, x, y)		name##_SPLAY_NEXT(x, y)
/* [한국어] SPLAY_MIN — 빈 트리면 NULL, 아니면 최소 노드를 root로 splay 후 반환. O(log n) amortized. */
#define SPLAY_MIN(name, x)		(SPLAY_EMPTY(x) ? NULL	\
					: name##_SPLAY_MIN_MAX(x, SPLAY_NEGINF))
/* [한국어] SPLAY_MAX — 최대 노드 반환. */
#define SPLAY_MAX(name, x)		(SPLAY_EMPTY(x) ? NULL	\
					: name##_SPLAY_MIN_MAX(x, SPLAY_INF))

/* [한국어] SPLAY_FOREACH(x, name, head) — in-order 순회 매크로.
 *  주의: 매 반복에서 SPLAY_NEXT가 호출되며 그때마다 트리가 splay 변형되므로,
 *        장거리 순회 비용이 RB_FOREACH보다 클 수 있다. SPDK 핫 패스에서 전수 순회가 필요하면
 *        보통 RB 트리로 인덱스를 두거나 별도 리스트를 함께 유지한다. */
#define SPLAY_FOREACH(x, name, head)					\
	for ((x) = SPLAY_MIN(name, head);				\
	     (x) != NULL;						\
	     (x) = SPLAY_NEXT(name, head, x))

/* ============================================================
 * [한국어] === Rank-Balanced (RB-like, Weak-AVL) Tree 매크로 그룹 1: 타입/접근자/색상 비트 트릭 ===
 *
 * 이 구현은 BSD가 도입한 "weak-AVL" rank-balanced 트리이다. 사용자에게 노출되는 이름은
 * RB_*이므로 통상적인 red-black 트리와 거의 동일하게 다룰 수 있다. 핵심 영리한 트릭은:
 *   - 노드 정렬 alignment가 최소 4바이트라는 가정 하에 부모 포인터의 하위 2비트를
 *     색상 비트로 재활용한다 (RB_RED_L = 좌측 자식이 red, RB_RED_R = 우측 자식이 red).
 *   - 따라서 별도의 색상 필드 없이 부모 포인터 하나로 (부모 + 좌/우 색상 2비트)를 저장.
 *   - RB_PARENT()는 하위 2비트를 마스킹한 후 캐스팅하여 실제 부모 포인터를 추출.
 *
 * worst-case O(log n)이 보장되어 latency 예측가능성이 중요한 SPDK 경로(NVMe/bdev 핫 패스)에서 선호된다.
 * ============================================================ */

/* Macros that define a rank-balanced tree */
/* [한국어] RB_HEAD — RB 트리 헤드 구조체 정의. SPLAY_HEAD와 마찬가지로 root 포인터 1개. */
#define RB_HEAD(name, type)						\
struct name {								\
	struct type *rbh_root; /* root of the tree */			\
	/* [한국어] 트리 루트. SPLAY와 달리 RB의 lookup(RB_FIND)은 이 포인터를 변경하지 않는다. */ \
}

/* [한국어] RB_INITIALIZER — 정적 초기화자. 사용 패턴은 SPLAY_INITIALIZER와 동일. */
#define RB_INITIALIZER(root)						\
	{ NULL }

/* [한국어] RB_INIT — 동적 초기화. root를 NULL로 만든다. */
#define RB_INIT(root) do {						\
	(root)->rbh_root = NULL;					\
} while (/* CONSTCOND */ 0)

/* [한국어] RB_ENTRY — 노드에 임베드되는 좌/우/부모 포인터.
 *  SPLAY_ENTRY와의 차이: 부모 포인터(rbe_parent)가 추가되었고, 그 하위 2비트가 색상 비트로 재활용된다.
 *  즉 사용자 노드 구조체의 메모리 풋프린트는 sizeof(void*) * 3. */
#define RB_ENTRY(type)							\
struct {								\
	struct type *rbe_left;		/* left element */		\
	/* [한국어] 좌측 자식 포인터. 키가 더 작은 부분 트리. */ \
	struct type *rbe_right;		/* right element */		\
	/* [한국어] 우측 자식 포인터. 키가 더 큰 부분 트리. */ \
	struct type *rbe_parent;	/* parent element */		\
	/* [한국어] 부모 포인터 + 색상 2비트(하위 2비트).
	 *  - 비트0(RB_RED_L): 부모(==이 노드)의 좌측 자식이 red인가
	 *  - 비트1(RB_RED_R): 부모(==이 노드)의 우측 자식이 red인가
	 *  주의: 이 비트들은 elm 자신의 색이 아니라 elm이 부모로 보이는 입장에서 본 자식의 색.
	 *        (elm 자신의 색은 RB_COLOR(elm) 매크로가 부모 포인터를 따라가서 계산한다.) */ \
}

/* [한국어] RB_LEFT/RIGHT — 자식 포인터 lvalue 접근자. SPLAY와 동일한 패턴. */
#define RB_LEFT(elm, field)		(elm)->field.rbe_left
#define RB_RIGHT(elm, field)		(elm)->field.rbe_right

/*
 * With the expectation that any object of struct type has an
 * address that is a multiple of 4, and that therefore the
 * 2 least significant bits of a pointer to struct type are
 * always zero, this implementation sets those bits to indicate
 * that the left or right child of the tree node is "red".
 */
/* [한국어] 부모 포인터에 색상 비트를 packing 하기 위한 매크로 묶음.
 *   이 트릭은 사용자 type의 alignment가 4바이트 이상이어야 안전하다 (보통 모든 구조체가 충족).
 *   사용자 노드가 char-aligned 같은 비정상 타입이면 정의되지 않은 동작이 된다.
 *
 *   참고: 이 구현은 strict aliasing 측면에서 회색 지대다 — RB_BITS는
 *   포인터 변수의 비트 표현을 uintptr_t lvalue로 다루므로 컴파일러 설정에 따라 -fno-strict-aliasing 권장. */

/* [한국어] RB_UP — 부모 포인터 자체에 대한 lvalue 접근 (색상 비트 포함된 raw 형태). */
#define RB_UP(elm, field)		(elm)->field.rbe_parent
/* [한국어] RB_BITS — 부모 포인터를 정수로 보고 비트 조작용 lvalue로 노출.
 *  *(uintptr_t *)&p 패턴: 포인터 변수의 메모리에 정수처럼 접근. */
#define RB_BITS(elm, field)		(*(uintptr_t *)&RB_UP(elm, field))
/* [한국어] 색상 비트 마스크.
 *   RB_RED_L (=0b01): 좌측 자식 엣지가 red.
 *   RB_RED_R (=0b10): 우측 자식 엣지가 red.
 *   RB_RED_MASK (=0b11): 두 비트 모두. */
#define RB_RED_L			((uintptr_t)1)
#define RB_RED_R			((uintptr_t)2)
#define RB_RED_MASK			((uintptr_t)3)
/* [한국어] RB_FLIP_LEFT/RIGHT — XOR로 좌/우 색상 비트를 토글. INSERT_COLOR/REMOVE_COLOR가 자주 호출. */
#define RB_FLIP_LEFT(elm, field)	(RB_BITS(elm, field) ^= RB_RED_L)
#define RB_FLIP_RIGHT(elm, field)	(RB_BITS(elm, field) ^= RB_RED_R)
/* [한국어] RB_RED_LEFT/RIGHT — 좌/우 자식 엣지가 red인지 묻는 술어. */
#define RB_RED_LEFT(elm, field)		((RB_BITS(elm, field) & RB_RED_L) != 0)
#define RB_RED_RIGHT(elm, field)	((RB_BITS(elm, field) & RB_RED_R) != 0)
/* [한국어] RB_PARENT — 색상 비트를 마스킹한 진짜 부모 포인터를 반환.
 *  __typeof()로 사용자 type 포인터로 캐스팅하여 strict typing을 유지. */
#define RB_PARENT(elm, field)		((__typeof(RB_UP(elm, field)))	\
					 (RB_BITS(elm, field) & ~RB_RED_MASK))
/*
 * _RB_ROOT starts with an underscore. This is a workaround for the issue that
 * RB_ROOT() had a name conflict with the SPDK FIO plugin. The SPDK FIO plugin
 * includes FIO and FIO defines RB_ROOT() itself.
 */
/* [한국어] _RB_ROOT — 트리 헤드의 root 포인터 lvalue 접근자.
 *  앞에 언더스코어가 붙은 이유: SPDK의 FIO 플러그인(test/external_code/spdk/fio_plugin)이
 *   FIO 헤더를 함께 #include 하는데, FIO 측에서도 RB_ROOT() 매크로를 정의하므로 이름 충돌이 발생했다.
 *   SPDK는 자신만 _RB_ROOT로 prefix-rename하여 충돌을 피했다. */
#define _RB_ROOT(head)			(head)->rbh_root
/* [한국어] RB_EMPTY — root가 NULL이면 빈 트리. O(1). */
#define RB_EMPTY(head)			(_RB_ROOT(head) == NULL)

/* [한국어] RB_SET_PARENT — dst의 부모 포인터를 src로 갱신하되, 기존 색상 2비트는 보존.
 *  순서: (1) 색상 마스크만 남기고 비움  (2) src(주소, 하위 2비트가 0)를 OR. */
#define RB_SET_PARENT(dst, src, field) do {				\
	RB_BITS(dst, field) &= RB_RED_MASK;				\
	RB_BITS(dst, field) |= (uintptr_t)src;			\
} while (/* CONSTCOND */ 0)

/* [한국어] RB_SET — 신규 노드(elm)를 트리에 매다는 초기화.
 *  - elm의 부모를 parent로 (색상 2비트는 0 = 양쪽 자식 모두 black 시작).
 *  - 좌/우 자식 포인터를 NULL로.
 *  주의: 부모 포인터에 색상 비트가 0이라는 것은 "이 elm의 좌/우 자식 엣지가 모두 black"을 의미한다. */
#define RB_SET(elm, parent, field) do {					\
	RB_UP(elm, field) = parent;					\
	RB_LEFT(elm, field) = RB_RIGHT(elm, field) = NULL;		\
} while (/* CONSTCOND */ 0)

/* [한국어] RB_COLOR — elm 자신의 색을 부모 측 비트에서 읽어온다.
 *   - 부모가 NULL(루트) → 0(black 간주).
 *   - elm이 부모의 좌측 자식이면 → 부모의 RED_L 비트.
 *   - elm이 부모의 우측 자식이면 → 부모의 RED_R 비트.
 *  이 분기는 노드 색이 부모 쪽 엣지에 저장되는 본 구현의 핵심 트릭을 그대로 보여준다. */
#define RB_COLOR(elm, field)	(RB_PARENT(elm, field) == NULL ? 0 :	\
				RB_LEFT(RB_PARENT(elm, field), field) == elm ? \
				RB_RED_LEFT(RB_PARENT(elm, field), field) : \
				RB_RED_RIGHT(RB_PARENT(elm, field), field))

/*
 * Something to be invoked in a loop at the root of every modified subtree,
 * from the bottom up to the root, to update augmented node data.
 */
/* [한국어] RB_AUGMENT — 트리가 변형된 후 부모 체인을 따라 root까지 호출되는 사용자 정의 hook.
 *   기본은 break(아무것도 안 하고 루프 1회 후 탈출)이며, 사용자가 #define으로 덮어쓸 수 있다.
 *   예: 서브트리 크기, 합계, 최대값 등 augmented 정보를 유지하고 싶을 때 사용. */
#ifndef RB_AUGMENT
#define RB_AUGMENT(x)	break
#endif

/* [한국어] RB_SWAP_CHILD — 트리에서 out 자리에 in을 끼워 넣는다 (in의 부모 측 갱신은 호출자 책임).
 *   - out이 root였으면 head의 root를 in으로.
 *   - out이 부모의 좌측이었으면 부모의 좌측을 in으로.
 *   - 우측이었으면 부모의 우측을 in으로.
 *  회전(ROTATE_*)과 REMOVE에서 핵심적으로 호출. */
#define RB_SWAP_CHILD(head, out, in, field) do {			\
	if (RB_PARENT(out, field) == NULL)				\
		_RB_ROOT(head) = (in);					\
	else if ((out) == RB_LEFT(RB_PARENT(out, field), field))	\
		RB_LEFT(RB_PARENT(out, field), field) = (in);		\
	else								\
		RB_RIGHT(RB_PARENT(out, field), field) = (in);		\
} while (/* CONSTCOND */ 0)

/* [한국어] RB_ROTATE_LEFT — elm을 축으로 좌회전. tmp는 elm의 우측 자식이었던 노드.
 *   변환:    elm                  tmp
 *           /   \                 /   \
 *          a    tmp     →       elm    c
 *               / \             / \
 *              b   c           a   b
 *   단계:
 *     1) tmp.left였던 b를 elm.right로 옮기고, b의 부모를 elm으로.
 *     2) tmp의 부모를 (원래 elm의 부모)로 갱신.
 *     3) elm이 차지하던 자리를 tmp가 차지 (RB_SWAP_CHILD).
 *     4) tmp.left ← elm, elm.parent ← tmp.
 *     5) augment(elm) 호출. */
#define RB_ROTATE_LEFT(head, elm, tmp, field) do {			\
	(tmp) = RB_RIGHT(elm, field);					\
	if ((RB_RIGHT(elm, field) = RB_LEFT(tmp, field)) != NULL) {	\
		RB_SET_PARENT(RB_RIGHT(elm, field), elm, field);	\
	}								\
	RB_SET_PARENT(tmp, RB_PARENT(elm, field), field);		\
	RB_SWAP_CHILD(head, elm, tmp, field);				\
	RB_LEFT(tmp, field) = (elm);					\
	RB_SET_PARENT(elm, tmp, field);					\
	RB_AUGMENT(elm);						\
} while (/* CONSTCOND */ 0)

/* [한국어] RB_ROTATE_RIGHT — RB_ROTATE_LEFT의 거울 대칭. tmp는 elm의 좌측 자식. */
#define RB_ROTATE_RIGHT(head, elm, tmp, field) do {			\
	(tmp) = RB_LEFT(elm, field);					\
	if ((RB_LEFT(elm, field) = RB_RIGHT(tmp, field)) != NULL) {	\
		RB_SET_PARENT(RB_LEFT(elm, field), elm, field);		\
	}								\
	RB_SET_PARENT(tmp, RB_PARENT(elm, field), field);		\
	RB_SWAP_CHILD(head, elm, tmp, field);				\
	RB_RIGHT(tmp, field) = (elm);					\
	RB_SET_PARENT(elm, tmp, field);					\
	RB_AUGMENT(elm);						\
} while (/* CONSTCOND */ 0)

/* ============================================================
 * [한국어] === RB Tree 매크로 그룹 2: PROTOTYPE (함수 선언) ===
 *
 * RB는 SPLAY보다 함수가 많고(특히 INSERT_COLOR/REMOVE_COLOR 등 색상 균형 보조 루틴),
 * 사용 형태도 두 가지(public/static)이므로 _PROTOTYPE이 _INTERNAL을 거쳐 펼쳐진다.
 *   RB_PROTOTYPE         : 외부 노출 (extern) 선언
 *   RB_PROTOTYPE_STATIC  : 한 .c 파일에서만 쓸 때 — static __attribute__((unused))로 묶음
 *
 * 인자:
 *   - name  : 트리 헤드 struct 이름 (mangled prefix)
 *   - type  : 노드 struct 이름
 *   - field : 노드의 RB_ENTRY 필드명
 *   - cmp   : int (*)(struct type *, struct type *) 비교 함수/매크로
 * ============================================================ */

/* Generates prototypes and inline functions */
/* [한국어] RB_PROTOTYPE — 외부 링크용. 선언 attribute 없이 (extern). */
#define	RB_PROTOTYPE(name, type, field, cmp)				\
	RB_PROTOTYPE_INTERNAL(name, type, field, cmp,)
/* [한국어] RB_PROTOTYPE_STATIC — 단일 컴파일 단위용. static + unused 속성을 attach.
 *  unused는 일부 함수가 호출되지 않아도 경고가 나지 않게 한다. */
#define	RB_PROTOTYPE_STATIC(name, type, field, cmp)			\
	RB_PROTOTYPE_INTERNAL(name, type, field, cmp, __attribute__((unused)) static)
/* [한국어] RB_PROTOTYPE_INTERNAL — 모든 RB 함수 선언을 한 번에 풀어준다.
 *  cmp 인자는 PROTOTYPE 단계에서는 사용되지 않지만(시그니처는 cmp에 의존하지 않음),
 *  GENERATE 단계와 인자 형태를 통일하기 위해 받는다. */
#define RB_PROTOTYPE_INTERNAL(name, type, field, cmp, attr)		\
	RB_PROTOTYPE_INSERT_COLOR(name, type, attr);			\
	RB_PROTOTYPE_REMOVE_COLOR(name, type, attr);			\
	RB_PROTOTYPE_INSERT(name, type, attr);				\
	RB_PROTOTYPE_REMOVE(name, type, attr);				\
	RB_PROTOTYPE_FIND(name, type, attr);				\
	RB_PROTOTYPE_NFIND(name, type, attr);				\
	RB_PROTOTYPE_NEXT(name, type, attr);				\
	RB_PROTOTYPE_PREV(name, type, attr);				\
	RB_PROTOTYPE_MINMAX(name, type, attr);				\
	RB_PROTOTYPE_REINSERT(name, type, attr);
/* [한국어] _RB_INSERT_COLOR — 삽입 후 색상/rank 균형을 회복하는 보조 루틴.
 *  내부 헬퍼 — 사용자 코드에서 직접 호출하지 않음. INSERT가 호출. */
#define RB_PROTOTYPE_INSERT_COLOR(name, type, attr)			\
	attr void name##_RB_INSERT_COLOR(struct name *, struct type *)
/* [한국어] _RB_REMOVE_COLOR — 제거 후 색상/rank 균형 회복. parent와 elm(=child) 두 인자를 받는다. */
#define RB_PROTOTYPE_REMOVE_COLOR(name, type, attr)			\
	attr void name##_RB_REMOVE_COLOR(struct name *,			\
	    struct type *, struct type *)
/* [한국어] _RB_REMOVE — 사용자 API 백엔드. 같은 키 노드를 제거. 반환: 제거된 노드 또는 NULL. */
#define RB_PROTOTYPE_REMOVE(name, type, attr)				\
	attr struct type *name##_RB_REMOVE(struct name *, struct type *)
/* [한국어] _RB_INSERT — 신규 노드 삽입. 반환: 중복이면 기존 노드, 신규면 NULL. */
#define RB_PROTOTYPE_INSERT(name, type, attr)				\
	attr struct type *name##_RB_INSERT(struct name *, struct type *)
/* [한국어] _RB_FIND — 정확히 일치하는 키 탐색. 트리 구조를 변경하지 않는다 (read-only).
 *  worst-case O(log n) — SPDK가 latency 예측가능성이 중요한 경로에서 SPLAY보다 RB를 선호하는 핵심 이유. */
#define RB_PROTOTYPE_FIND(name, type, attr)				\
	attr struct type *name##_RB_FIND(struct name *, struct type *)
/* [한국어] _RB_NFIND — "not less than find". elm 키 이상의 첫 노드(=lower_bound)를 반환.
 *  범위 쿼리(예: bdev range lock에서 겹치는 첫 락 찾기)에 유용. */
#define RB_PROTOTYPE_NFIND(name, type, attr)				\
	attr struct type *name##_RB_NFIND(struct name *, struct type *)
/* [한국어] _RB_NEXT/_RB_PREV — in-order 후속/직전 노드. 헤드 인자 없이 노드만으로 가능
 *   (RB는 부모 포인터를 들고 있으므로 트리 외부에서도 이동 가능). */
#define RB_PROTOTYPE_NEXT(name, type, attr)				\
	attr struct type *name##_RB_NEXT(struct type *)
#define RB_PROTOTYPE_PREV(name, type, attr)				\
	attr struct type *name##_RB_PREV(struct type *)
/* [한국어] _RB_MINMAX — int val(<0이면 좌, >=0이면 우)을 끝까지 따라 내려가 최소/최대 노드 반환. */
#define RB_PROTOTYPE_MINMAX(name, type, attr)				\
	attr struct type *name##_RB_MINMAX(struct name *, int)
/* [한국어] _RB_REINSERT — elm의 키 값이 외부에서 변경되어 in-order 위치가 깨졌을 때
 *   이전/다음 이웃과 비교해 위치가 어긋나면 remove 후 재삽입. SPDK에서 가변 키 시나리오에 사용. */
#define RB_PROTOTYPE_REINSERT(name, type, attr)			\
	attr struct type *name##_RB_REINSERT(struct name *, struct type *)

/* ============================================================
 * [한국어] === RB Tree 매크로 그룹 3: GENERATE (실제 함수 본체) ===
 *
 * RB_GENERATE는 _PROTOTYPE에 대응하는 .c 파일용 매크로. 한 번에 10개 함수를 풀어낸다.
 * 핫 패스 함수가 많으므로 보통 RB_GENERATE_STATIC + 헤더-only inline 인스턴스화도 흔하다.
 *
 * 알고리즘 핵심: 본 구현은 "weak-AVL" rank-balanced 트리를 부모-자식 엣지에 분포된
 *   2비트 색상(RB_RED_L/R)으로 표현한다. INSERT_COLOR/REMOVE_COLOR는 색상 토글과 회전을
 *   조합해 트리 높이를 2 lg(n+1) 이하로 유지한다. 모든 연산은 worst-case O(log n).
 * ============================================================ */

/* Main rb operation.
 * Moves node close to the key of elm to top
 */
/* [한국어] RB_GENERATE / RB_GENERATE_STATIC — public/static 두 변형. 내부적으로 _INTERNAL을 호출. */
#define	RB_GENERATE(name, type, field, cmp)				\
	RB_GENERATE_INTERNAL(name, type, field, cmp,)
#define	RB_GENERATE_STATIC(name, type, field, cmp)			\
	RB_GENERATE_INTERNAL(name, type, field, cmp, __attribute__((unused)) static)
/* [한국어] _GENERATE_INTERNAL — 10개 함수 정의 매크로를 차례로 호출.
 *  주의: 한 트리 인스턴스에 대해 .c 파일에서 정확히 1번만 호출되어야 함 (ODR 보장).
 *        헤더-only로 사용하려면 RB_GENERATE_STATIC을 헤더에서 호출. */
#define RB_GENERATE_INTERNAL(name, type, field, cmp, attr)		\
	RB_GENERATE_INSERT_COLOR(name, type, field, attr)		\
	RB_GENERATE_REMOVE_COLOR(name, type, field, attr)		\
	RB_GENERATE_INSERT(name, type, field, cmp, attr)		\
	RB_GENERATE_REMOVE(name, type, field, attr)			\
	RB_GENERATE_FIND(name, type, field, cmp, attr)			\
	RB_GENERATE_NFIND(name, type, field, cmp, attr)			\
	RB_GENERATE_NEXT(name, type, field, attr)			\
	RB_GENERATE_PREV(name, type, field, attr)			\
	RB_GENERATE_MINMAX(name, type, field, attr)			\
	RB_GENERATE_REINSERT(name, type, field, cmp, attr)

/* [한국어] _RB_INSERT_COLOR — 신규 삽입된 elm 이후 rank-balance 위반을 회복.
 *   알고리즘: 부모를 따라 위로 올라가며 형제 색을 보고
 *     - 부모-elm 엣지가 black이면 색만 토글하고 종료(local rebalance)
 *     - 형제와 함께 양쪽이 red(rank 두 단계 위반)면 재색칠 후 grandparent로 전파(continue)
 *     - 그 외에는 회전(zig-zag면 더블 회전, zig-zig면 단일 회전)으로 종료.
 *   복잡도: amortized O(1)이지만 worst-case도 O(log n). */
#define RB_GENERATE_INSERT_COLOR(name, type, field, attr)		\
attr void								\
name##_RB_INSERT_COLOR(struct name *head, struct type *elm)		\
{									\
	struct type *child, *parent;					\
	/* [한국어] child : 회전 시 임시 자식 보관용. parent : 현재 검사 중인 노드의 부모. */ \
	while ((parent = RB_PARENT(elm, field)) != NULL) {		\
		/* [한국어] elm이 root에 도달할 때까지 (parent == NULL) 위로 진행. */ \
		if (RB_LEFT(parent, field) == elm) {			\
			/* [한국어] elm이 부모의 좌측 자식인 경우. */ \
			if (RB_RED_LEFT(parent, field)) {		\
				/* [한국어] 부모-좌측 엣지가 이미 red — 토글하면 black이 되어 위반 해소. */ \
				RB_FLIP_LEFT(parent, field);		\
				return;					\
			}						\
			RB_FLIP_RIGHT(parent, field);			\
			/* [한국어] 우측 엣지를 red로 토글 (잠정) — rank 균형 위해 형제 측 색을 일단 끌어올림. */ \
			if (RB_RED_RIGHT(parent, field)) {		\
				/* [한국어] 양쪽 엣지가 모두 red가 됐다 → rank 위반을 부모 노드로 전파. */ \
				elm = parent;				\
				continue;				\
			}						\
			if (!RB_RED_RIGHT(elm, field)) {		\
				/* [한국어] zig-zag 케이스: elm의 우측이 black이면 먼저 좌회전(elm 기준)으로
				 *   zig-zig 형태로 변환한 뒤 부모 기준 우회전으로 종료. */ \
				RB_FLIP_LEFT(elm, field);		\
				RB_ROTATE_LEFT(head, elm, child, field);\
				if (RB_RED_LEFT(child, field))		\
					RB_FLIP_RIGHT(elm, field);	\
				/* [한국어] 회전 후 child의 좌측 색을 elm의 우측으로 옮김. */ \
				else if (RB_RED_RIGHT(child, field))	\
					RB_FLIP_LEFT(parent, field);	\
				elm = child;				\
			}						\
			RB_ROTATE_RIGHT(head, parent, elm, field);	\
			/* [한국어] 최종 우회전 — elm이 새 서브트리 루트가 되며 균형 회복. */ \
		} else {						\
			/* [한국어] elm이 부모의 우측 자식인 거울 대칭 케이스. */ \
			if (RB_RED_RIGHT(parent, field)) {		\
				RB_FLIP_RIGHT(parent, field);		\
				return;					\
			}						\
			RB_FLIP_LEFT(parent, field);			\
			if (RB_RED_LEFT(parent, field)) {		\
				elm = parent;				\
				continue;				\
			}						\
			if (!RB_RED_LEFT(elm, field)) {			\
				RB_FLIP_RIGHT(elm, field);		\
				RB_ROTATE_RIGHT(head, elm, child, field);\
				if (RB_RED_RIGHT(child, field))		\
					RB_FLIP_LEFT(elm, field);	\
				else if (RB_RED_LEFT(child, field))	\
					RB_FLIP_RIGHT(parent, field);	\
				elm = child;				\
			}						\
			RB_ROTATE_LEFT(head, parent, elm, field);	\
		}							\
		RB_BITS(elm, field) &= ~RB_RED_MASK;			\
		/* [한국어] 회전 후 새 서브트리 루트의 양쪽 자식 엣지를 black으로 초기화. */ \
		break;							\
	}								\
}

/* [한국어] _RB_REMOVE_COLOR — 노드 제거 후 rank-balance 위반을 회복.
 *   특수 케이스: parent의 좌/우가 모두 elm이면 (RB_REMOVE 시 elm == NULL인 child를 의미하는 호출 패턴)
 *               parent를 black 처리하고 부모로 한 단계 올라간다.
 *   메인 루프: parent를 따라 위로 올라가며 형제(sib)와 그 자식들의 색을 보고
 *             - 단순 토글로 끝내거나
 *             - 형제 측 자식들이 모두 black이면 형제를 red로 만들고 위로 전파
 *             - 그 외에는 회전(zig-zig 단일 회전 / zig-zag 더블 회전)으로 종료.
 *   복잡도: worst-case O(log n). */
#define RB_GENERATE_REMOVE_COLOR(name, type, field, attr)		\
attr void								\
name##_RB_REMOVE_COLOR(struct name *head,				\
    struct type *parent, struct type *elm)				\
{									\
	struct type *sib;						\
	/* [한국어] sib : elm의 형제 노드. */ \
	if (RB_LEFT(parent, field) == elm &&				\
	    RB_RIGHT(parent, field) == elm) {				\
		/* [한국어] parent의 좌/우가 모두 elm — 이는 호출자가 "child가 NULL"인 leaf 자리를
		 *   표현하기 위한 관용구다. parent의 색상 비트를 모두 비우고 한 단계 위로 전파. */ \
		RB_BITS(parent, field) &= ~RB_RED_MASK;			\
		elm = parent;						\
		parent = RB_PARENT(elm, field);				\
		if (parent == NULL)					\
			return;						\
		/* [한국어] root까지 도달하면 작업 끝. */ \
	}								\
	do  {								\
		if (RB_LEFT(parent, field) == elm) {			\
			/* [한국어] elm이 좌측 자식 (또는 좌측 leaf)인 경우. */ \
			if (!RB_RED_LEFT(parent, field)) {		\
				/* [한국어] 부모-좌측 엣지가 black — red로 토글하면 rank 회복. */ \
				RB_FLIP_LEFT(parent, field);		\
				return;					\
			}						\
			if (RB_RED_RIGHT(parent, field)) {		\
				/* [한국어] 우측 엣지가 red — 토글하면서 부모로 전파. */ \
				RB_FLIP_RIGHT(parent, field);		\
				elm = parent;				\
				continue;				\
			}						\
			sib = RB_RIGHT(parent, field);			\
			if ((~RB_BITS(sib, field) & RB_RED_MASK) == 0) {\
				/* [한국어] 형제의 좌/우 자식 엣지가 모두 red(=형제의 양쪽이 red,
				 *   즉 ~bits & MASK == 0)이면 형제 비트를 비우고 위로 전파. */ \
				RB_BITS(sib, field) &= ~RB_RED_MASK;	\
				elm = parent;				\
				continue;				\
			}						\
			RB_FLIP_RIGHT(sib, field);			\
			if (RB_RED_LEFT(sib, field))			\
				RB_FLIP_LEFT(parent, field);		\
			else if (!RB_RED_RIGHT(sib, field)) {		\
				/* [한국어] 형제 우측이 black인 zig-zag 케이스: 형제를 우회전 → zig-zig로 변환. */ \
				RB_FLIP_LEFT(parent, field);		\
				RB_ROTATE_RIGHT(head, sib, elm, field);	\
				if (RB_RED_RIGHT(elm, field))		\
					RB_FLIP_LEFT(sib, field);	\
				if (RB_RED_LEFT(elm, field))		\
					RB_FLIP_RIGHT(parent, field);	\
				RB_BITS(elm, field) |= RB_RED_MASK;	\
				sib = elm;				\
			}						\
			RB_ROTATE_LEFT(head, parent, sib, field);	\
			/* [한국어] 최종 좌회전 — sib가 새 서브트리 루트가 되어 균형 회복. */ \
		} else {						\
			/* [한국어] elm이 우측 자식인 거울 대칭. */ \
			if (!RB_RED_RIGHT(parent, field)) {		\
				RB_FLIP_RIGHT(parent, field);		\
				return;					\
			}						\
			if (RB_RED_LEFT(parent, field)) {		\
				RB_FLIP_LEFT(parent, field);		\
				elm = parent;				\
				continue;				\
			}						\
			sib = RB_LEFT(parent, field);			\
			if ((~RB_BITS(sib, field) & RB_RED_MASK) == 0) {\
				RB_BITS(sib, field) &= ~RB_RED_MASK;	\
				elm = parent;				\
				continue;				\
			}						\
			RB_FLIP_LEFT(sib, field);			\
			if (RB_RED_RIGHT(sib, field))			\
				RB_FLIP_RIGHT(parent, field);		\
			else if (!RB_RED_LEFT(sib, field)) {		\
				RB_FLIP_RIGHT(parent, field);		\
				RB_ROTATE_LEFT(head, sib, elm, field);	\
				if (RB_RED_LEFT(elm, field))		\
					RB_FLIP_RIGHT(sib, field);	\
				if (RB_RED_RIGHT(elm, field))		\
					RB_FLIP_LEFT(parent, field);	\
				RB_BITS(elm, field) |= RB_RED_MASK;	\
				sib = elm;				\
			}						\
			RB_ROTATE_RIGHT(head, parent, sib, field);	\
		}							\
		break;							\
		/* [한국어] 회전으로 균형이 회복되면 do-while 종료. continue로만 다음 iteration 진입. */ \
	} while ((parent = RB_PARENT(elm, field)) != NULL);		\
}

/* [한국어] _RB_REMOVE — 사용자 API 백엔드. 노드 제거.
 *   세 가지 케이스:
 *     1) elm의 좌측 자식 없음 → 우측 자식이 elm 자리를 차지.
 *     2) elm의 우측 자식 없음 → 좌측 자식이 elm 자리를 차지.
 *     3) 양쪽 자식 존재 → 우측 서브트리의 in-order successor(=가장 좌측 노드)를 찾아 elm 자리로 옮김.
 *   이후 RB_SWAP_CHILD로 부모와의 연결을 갱신하고, 색상 균형이 깨졌으면 RB_REMOVE_COLOR 호출.
 *   마지막으로 부모 체인을 따라 RB_AUGMENT를 호출해 augmented 정보를 갱신.
 *   반환: 제거된 원본 노드(old). 호출자가 메모리 해제 가능. */
#define RB_GENERATE_REMOVE(name, type, field, attr)			\
attr struct type *							\
name##_RB_REMOVE(struct name *head, struct type *elm)			\
{									\
	struct type *child, *old, *parent, *right;			\
	/* [한국어] old : 제거 대상 원본. child : 빈 자리에 들어갈 자식. parent : 색상 회복 시작점. */ \
									\
	old = elm;							\
	parent = RB_PARENT(elm, field);					\
	right = RB_RIGHT(elm, field);					\
	if (RB_LEFT(elm, field) == NULL)				\
		/* [한국어] 케이스 1: 좌측 없음 — 우측을 그대로 올림. */ \
		elm = child = right;					\
	else if (right == NULL)						\
		/* [한국어] 케이스 2: 우측 없음 — 좌측을 그대로 올림. */ \
		elm = child = RB_LEFT(elm, field);			\
	else {								\
		/* [한국어] 케이스 3: 양쪽 자식 존재 — successor 탐색. */ \
		if ((child = RB_LEFT(right, field)) == NULL) {		\
			/* [한국어] 우측 자식의 좌측이 없으면 우측 자식 자체가 successor. */ \
			child = RB_RIGHT(right, field);			\
			RB_RIGHT(old, field) = child;			\
			parent = elm = right;				\
		} else {						\
			/* [한국어] 우측 서브트리에서 leftmost 노드까지 내려간다. */ \
			do						\
				elm = child;				\
			while ((child = RB_LEFT(elm, field)) != NULL);	\
			/* [한국어] elm이 successor. 그 우측 자식이 child. */ \
			child = RB_RIGHT(elm, field);			\
			parent = RB_PARENT(elm, field);			\
			RB_LEFT(parent, field) = child;			\
			/* [한국어] successor 자리에 child를 끼워 넣음. */ \
			RB_SET_PARENT(RB_RIGHT(old, field), elm, field);\
			/* [한국어] old의 우측 서브트리의 부모를 successor로 갱신. */ \
		}							\
		RB_SET_PARENT(RB_LEFT(old, field), elm, field);		\
		/* [한국어] old의 좌측 서브트리도 successor 아래로 이전. */ \
		elm->field = old->field;				\
		/* [한국어] entry 전체(좌/우/부모+색상) 복사 — successor가 old의 위상을 그대로 인수. */ \
	}								\
	RB_SWAP_CHILD(head, old, elm, field);				\
	/* [한국어] old가 차지하던 부모-자식 슬롯에 elm을 끼워 넣음. */ \
	if (child != NULL)						\
		RB_SET_PARENT(child, parent, field);			\
	if (parent != NULL)						\
		name##_RB_REMOVE_COLOR(head, parent, child);		\
	/* [한국어] 색상 균형이 깨진 시작점에서 회복 보조 루틴 호출. */ \
	while (parent != NULL) {					\
		RB_AUGMENT(parent);					\
		parent = RB_PARENT(parent, field);			\
		/* [한국어] root까지 augmented 데이터 갱신 (사용자 정의 hook). */ \
	}								\
	return (old);							\
}

/* [한국어] _RB_INSERT — 표준 BST 삽입 후 INSERT_COLOR 호출, 마지막에 augmented 갱신.
 *   복잡도: worst-case O(log n).
 *   반환: 중복 키이면 기존 노드 반환(삽입하지 않음), 신규 삽입 시 NULL. */
#define RB_GENERATE_INSERT(name, type, field, cmp, attr)		\
/* Inserts a node into the RB tree */					\
attr struct type *							\
name##_RB_INSERT(struct name *head, struct type *elm)			\
{									\
	struct type *tmp;						\
	struct type *parent = NULL;					\
	/* [한국어] parent : while 종료 시 elm이 매달릴 노드. */ \
	int comp = 0;							\
	tmp = _RB_ROOT(head);						\
	while (tmp) {							\
		/* [한국어] BST 표준 하강 — 비교 결과에 따라 좌/우. */ \
		parent = tmp;						\
		comp = (cmp)(elm, parent);				\
		if (comp < 0)						\
			tmp = RB_LEFT(tmp, field);			\
		else if (comp > 0)					\
			tmp = RB_RIGHT(tmp, field);			\
		else							\
			return (tmp);					\
		/* [한국어] 동일 키 발견 — 중복 삽입 거부, 기존 노드 반환. */ \
	}								\
	RB_SET(elm, parent, field);					\
	/* [한국어] elm을 새 leaf로 초기화 (자식 NULL, 부모 = parent, 색상 비트 0). */ \
	if (parent == NULL)						\
		_RB_ROOT(head) = elm;					\
	/* [한국어] 빈 트리였다면 elm이 root. */ \
	else if (comp < 0)						\
		RB_LEFT(parent, field) = elm;				\
	else								\
		RB_RIGHT(parent, field) = elm;				\
	name##_RB_INSERT_COLOR(head, elm);				\
	/* [한국어] 색상/rank 균형 회복. */ \
	while (elm != NULL) {						\
		RB_AUGMENT(elm);					\
		elm = RB_PARENT(elm, field);				\
		/* [한국어] root까지 augmented 데이터 갱신. */ \
	}								\
	return (NULL);							\
	/* [한국어] 신규 삽입 성공. */ \
}

/* [한국어] _RB_FIND — 정확 일치 키 탐색. read-only(트리를 변경하지 않음).
 *   복잡도: worst-case O(log n) — SPDK가 latency 예측이 중요한 곳(예: bdev I/O 채널 lookup)에서
 *   SPLAY 대신 RB를 선호하는 핵심 이유. */
#define RB_GENERATE_FIND(name, type, field, cmp, attr)			\
/* Finds the node with the same key as elm */				\
attr struct type *							\
name##_RB_FIND(struct name *head, struct type *elm)			\
{									\
	struct type *tmp = _RB_ROOT(head);				\
	int comp;							\
	while (tmp) {							\
		comp = cmp(elm, tmp);					\
		if (comp < 0)						\
			tmp = RB_LEFT(tmp, field);			\
		else if (comp > 0)					\
			tmp = RB_RIGHT(tmp, field);			\
		else							\
			return (tmp);					\
		/* [한국어] 동일 키 발견 시 즉시 반환. */ \
	}								\
	return (NULL);							\
	/* [한국어] 키 없음. */ \
}

/* [한국어] _RB_NFIND — "Not less than FIND". elm 이상의 첫 노드(=lower_bound).
 *   동작: 하강 중 'comp < 0' (현재 노드 키 > 검색 키)인 노드를 res에 후보로 기억하고 좌측으로 진행.
 *         최종적으로 마지막 후보가 결과 (또는 정확 일치 시 즉시 반환).
 *   사용 예: 범위가 정의된 객체(예: bdev locked range)에서 시작점 찾기. */
#define RB_GENERATE_NFIND(name, type, field, cmp, attr)			\
/* Finds the first node greater than or equal to the search key */	\
attr struct type *							\
name##_RB_NFIND(struct name *head, struct type *elm)			\
{									\
	struct type *tmp = _RB_ROOT(head);				\
	struct type *res = NULL;					\
	/* [한국어] res : 지금까지 본 "elm 이상" 노드 후보 중 가장 작은 것. */ \
	int comp;							\
	while (tmp) {							\
		comp = cmp(elm, tmp);					\
		if (comp < 0) {						\
			res = tmp;					\
			/* [한국어] tmp가 elm보다 큼 — 후보 갱신 후 더 작은 후보를 좌측에서 찾는다. */ \
			tmp = RB_LEFT(tmp, field);			\
		}							\
		else if (comp > 0)					\
			tmp = RB_RIGHT(tmp, field);			\
		else							\
			return (tmp);					\
	}								\
	return (res);							\
}

/* [한국어] _RB_NEXT — in-order 후속 노드 (헤드 인자 없이 노드 자체로 이동 가능).
 *   - 우측 자식이 있으면: 우측 서브트리의 leftmost.
 *   - 없으면: 부모를 따라 위로 올라가다, 자신이 부모의 좌측 자식이 되는 첫 부모로.
 *   복잡도: amortized O(1) (트리 전체를 RB_FOREACH로 순회 시 총 O(n)). */
#define RB_GENERATE_NEXT(name, type, field, attr)			\
/* ARGSUSED */								\
attr struct type *							\
name##_RB_NEXT(struct type *elm)					\
{									\
	if (RB_RIGHT(elm, field)) {					\
		/* [한국어] 우측 서브트리가 있는 케이스. */ \
		elm = RB_RIGHT(elm, field);				\
		while (RB_LEFT(elm, field))				\
			elm = RB_LEFT(elm, field);			\
		/* [한국어] leftmost까지 내려감. */ \
	} else {							\
		if (RB_PARENT(elm, field) &&				\
		    (elm == RB_LEFT(RB_PARENT(elm, field), field)))	\
			elm = RB_PARENT(elm, field);			\
		/* [한국어] elm이 좌측 자식이면 부모가 곧 후속. */ \
		else {							\
			while (RB_PARENT(elm, field) &&			\
			    (elm == RB_RIGHT(RB_PARENT(elm, field), field)))\
				elm = RB_PARENT(elm, field);		\
			/* [한국어] 우측 자식 체인을 거슬러 올라가다, 좌측 자식이 되는 첫 노드의 부모가 후속. */ \
			elm = RB_PARENT(elm, field);			\
		}							\
	}								\
	return (elm);							\
	/* [한국어] root의 최대 노드에서 호출되면 NULL 반환 (RB_FOREACH 종료 조건). */ \
}

/* [한국어] _RB_PREV — _RB_NEXT의 거울 대칭 (in-order 직전 노드). */
#define RB_GENERATE_PREV(name, type, field, attr)			\
/* ARGSUSED */								\
attr struct type *							\
name##_RB_PREV(struct type *elm)					\
{									\
	if (RB_LEFT(elm, field)) {					\
		elm = RB_LEFT(elm, field);				\
		while (RB_RIGHT(elm, field))				\
			elm = RB_RIGHT(elm, field);			\
	} else {							\
		if (RB_PARENT(elm, field) &&				\
		    (elm == RB_RIGHT(RB_PARENT(elm, field), field)))	\
			elm = RB_PARENT(elm, field);			\
		else {							\
			while (RB_PARENT(elm, field) &&			\
			    (elm == RB_LEFT(RB_PARENT(elm, field), field)))\
				elm = RB_PARENT(elm, field);		\
			elm = RB_PARENT(elm, field);			\
		}							\
	}								\
	return (elm);							\
}

/* [한국어] _RB_MINMAX — val<0이면 좌측으로, 그렇지 않으면 우측으로 끝까지 내려간 마지막 노드.
 *   RB_MIN(=val<0) / RB_MAX(=val>=0) 사용자 매크로의 backend.
 *   SPLAY와 달리 트리를 변형하지 않으므로 read-only. */
#define RB_GENERATE_MINMAX(name, type, field, attr)			\
attr struct type *							\
name##_RB_MINMAX(struct name *head, int val)				\
{									\
	struct type *tmp = _RB_ROOT(head);				\
	struct type *parent = NULL;					\
	while (tmp) {							\
		parent = tmp;						\
		if (val < 0)						\
			tmp = RB_LEFT(tmp, field);			\
		else							\
			tmp = RB_RIGHT(tmp, field);			\
	}								\
	return (parent);						\
}

/* [한국어] _RB_REINSERT — 외부 코드가 elm의 키 필드를 변경한 후 호출.
 *   동작: 이전/다음 노드와 비교해 in-order 순서가 깨졌으면 remove 후 다시 insert.
 *   주석 "Remove/insert is heavy handed"는 더 효율적인 swap 기반 알고리즘이 가능하나,
 *   매크로 generic 코드의 단순성을 우선했다는 의미. SPDK 가변 키 시나리오(예: 우선순위 큐)에서 사용. */
#define	RB_GENERATE_REINSERT(name, type, field, cmp, attr)		\
attr struct type *							\
name##_RB_REINSERT(struct name *head, struct type *elm)			\
{									\
	struct type *cmpelm;						\
	if (((cmpelm = RB_PREV(name, head, elm)) != NULL &&		\
	    cmp(cmpelm, elm) >= 0) ||					\
	    ((cmpelm = RB_NEXT(name, head, elm)) != NULL &&		\
	    cmp(elm, cmpelm) >= 0)) {					\
		/* [한국어] 좌(이전) 또는 우(다음) 이웃과 in-order 순서가 깨진 경우.
		 *   PREV >= elm: 이전 노드가 더 크거나 같음 = 위반.
		 *   elm >= NEXT: 현재 노드가 다음보다 크거나 같음 = 위반. */ \
		/* XXXLAS: Remove/insert is heavy handed. */		\
		RB_REMOVE(name, head, elm);				\
		return (RB_INSERT(name, head, elm));			\
	}								\
	return (NULL);							\
	/* [한국어] 순서가 그대로면 변경 불필요. */ \
}									\

/* ============================================================
 * [한국어] === RB Tree 매크로 그룹 4: 사용자 API (얇은 래퍼) + FOREACH 변형 ===
 *
 * 모두 name##_RB_* 함수에 dispatch. SPDK 코드는 이 매크로들로만 트리를 다룬다.
 * FOREACH 변형 4종 (정/역, FROM, SAFE)은 가장 자주 쓰이는 순회 패턴 — 특히 SAFE는
 * 순회 중 현재 노드를 RB_REMOVE 해도 안전하다 (다음 노드를 미리 캐시).
 * ============================================================ */

/* [한국어] _RB_MINMAX 호출 인자. SPLAY_NEGINF/INF와 동일한 의미. */
#define RB_NEGINF	-1
#define RB_INF	1

/* [한국어] 사용자 API 매크로들. 각 매크로는 그저 name 토큰을 prefix로 mangling 한다. */
#define RB_INSERT(name, x, y)	name##_RB_INSERT(x, y)
#define RB_REMOVE(name, x, y)	name##_RB_REMOVE(x, y)
#define RB_FIND(name, x, y)	name##_RB_FIND(x, y)
#define RB_NFIND(name, x, y)	name##_RB_NFIND(x, y)
/* [한국어] RB_NEXT/PREV는 헤드를 받지 않지만, API 일관성을 위해 head를 인자로 노출한다 (사용 안 함). */
#define RB_NEXT(name, x, y)	name##_RB_NEXT(y)
#define RB_PREV(name, x, y)	name##_RB_PREV(y)
#define RB_MIN(name, x)		name##_RB_MINMAX(x, RB_NEGINF)
#define RB_MAX(name, x)		name##_RB_MINMAX(x, RB_INF)
#define RB_REINSERT(name, x, y)	name##_RB_REINSERT(x, y)

/* [한국어] RB_FOREACH(x, name, head) — in-order 정방향 순회.
 *  SPLAY_FOREACH와 달리 매 반복이 트리를 변형하지 않으므로 (RB_NEXT는 read-only) 비용 균일.
 *  사용 예: SPDK bdev qos 정책의 namespace 순회, blobstore의 cluster 순회 등. */
#define RB_FOREACH(x, name, head)					\
	for ((x) = RB_MIN(name, head);					\
	     (x) != NULL;						\
	     (x) = name##_RB_NEXT(x))

/* [한국어] RB_FOREACH_FROM(x, name, y) — y에서 시작하여 정방향 순회.
 *  미묘한 트릭: ((y) = NEXT(x), (x) != NULL) 형태로 다음 노드를 미리 y에 캐시한 뒤,
 *  body 실행 시 x는 현재, 다음 iter 시작에서 (x) = (y) 로 진행. */
#define RB_FOREACH_FROM(x, name, y)					\
	for ((x) = (y);							\
	    ((x) != NULL) && ((y) = name##_RB_NEXT(x), (x) != NULL);	\
	     (x) = (y))

/* [한국어] RB_FOREACH_SAFE(x, name, head, y) — 순회 중 RB_REMOVE 안전 변형.
 *  body 진입 전에 다음 노드를 y에 미리 저장해두므로, body에서 x를 제거해도 다음 iter는 정상 진행.
 *  SPDK에서 트리 전체를 비우는 정리 코드(예: _bdev_destruct, blob 종료 시) 흔히 사용. */
#define RB_FOREACH_SAFE(x, name, head, y)				\
	for ((x) = RB_MIN(name, head);					\
	    ((x) != NULL) && ((y) = name##_RB_NEXT(x), (x) != NULL);	\
	     (x) = (y))

/* [한국어] RB_FOREACH_REVERSE — 역방향 in-order 순회 (max → min). */
#define RB_FOREACH_REVERSE(x, name, head)				\
	for ((x) = RB_MAX(name, head);					\
	     (x) != NULL;						\
	     (x) = name##_RB_PREV(x))

/* [한국어] RB_FOREACH_REVERSE_FROM — y에서 시작하는 역방향 순회. */
#define RB_FOREACH_REVERSE_FROM(x, name, y)				\
	for ((x) = (y);							\
	    ((x) != NULL) && ((y) = name##_RB_PREV(x), (x) != NULL);	\
	     (x) = (y))

/* [한국어] RB_FOREACH_REVERSE_SAFE — 역방향 + RB_REMOVE 안전. */
#define RB_FOREACH_REVERSE_SAFE(x, name, head, y)			\
	for ((x) = RB_MAX(name, head);					\
	    ((x) != NULL) && ((y) = name##_RB_PREV(x), (x) != NULL);	\
	     (x) = (y))

#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 종료. */
#endif

#endif	/* SPDK_TREE_H */
/* [한국어] SPDK_TREE_H 헤더 가드 종료. */
