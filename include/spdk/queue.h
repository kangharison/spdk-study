/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] BSD sys/queue.h 래퍼 · SPDK 확장 (queue.h)
 *
 * === 파일의 역할 ===
 * FreeBSD의 `sys/queue.h` 매크로 자료구조(SLIST / LIST / STAILQ / TAILQ /
 * CIRCLEQ)를 SPDK 전역에서 사용할 수 있도록 래핑하고, 다음 SPDK 확장을
 * 추가한다:
 *   (1) 리눅스 빌드 시 FreeBSD 전용 매크로(queue_extras.h)를 보충.
 *   (2) scan-build(Clang 정적 분석기) 오탐 대응을 위해 TAILQ_REMOVE를
 *       assert 포함 버전으로 재정의.
 *   (3) TAILQ 엔트리의 "리스트 소속 여부" 판별·클리어 매크로 추가
 *       (`TAILQ_ENTRY_ENQUEUED`, `TAILQ_ENTRY_CLEAR`, `TAILQ_REMOVE_CLEAR`).
 *
 * SPDK NVMe 드라이버가 FreeBSD에서 포팅됐기 때문에 원 코드가 FreeBSD의
 * 확장(SLIST_FOREACH_SAFE, STAILQ_LAST 등) 기능을 사용한다. 리눅스 glibc의
 * sys/queue.h는 이들 일부를 제공하지 않으므로 `queue_extras.h`에서 보충한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 모든 intrusive 리스트 기반 자료구조(장치 리스트, 큐 페어, 대기 I/O,
 * 폴링 그룹, 등록된 모듈 체인 등)가 본 헤더를 통해 BSD queue 매크로를 쓴다.
 * 런타임 비용은 일반 double-linked list와 동일.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - sys/cdefs.h  - FreeBSD-style 컴파일러 attribute 매크로(__unused 등)
 *   - sys/queue.h  - BSD 표준 queue 매크로 집합
 *   - spdk/queue_extras.h - 리눅스에서 누락된 확장 매크로 보충 (리눅스 전용)
 * 의존하는 모듈:
 *   - lib/nvme/*: ctrlr/qpair/request/async_event 리스트
 *   - lib/bdev/*: 등록된 bdev/module 리스트, in-flight I/O 리스트
 *   - lib/thread/*: spdk_thread 리스트, pending message 리스트
 *   - 거의 모든 SPDK 서브시스템
 * 공유 자료구조: BSD queue 매크로 자체는 구조체 내부에 "연결고리" 필드(tqe_next,
 *   tqe_prev)를 심어 두는 intrusive 방식 — 메모리 할당 불필요.
 *
 * === 주요 함수/구조체 요약 ===
 * 본 헤더는 표준 queue 매크로는 그대로 노출하고, 추가로:
 *   - TAILQ_REMOVE (scan-build 분기): assert로 removed 엔트리가 실제로
 *     리스트에 없음을 검증 — 분석기 오탐 무력화 목적
 *   - TAILQ_ENTRY_ENQUEUED(elm, field):
 *       엔트리가 현재 어떤 TAILQ에 속해 있는지 검사. 전제: 엔트리가 zero-init
 *       되었거나 TAILQ_ENTRY_CLEAR/TAILQ_REMOVE_CLEAR로 비워진 상태
 *   - TAILQ_ENTRY_NOT_ENQUEUED(elm, field): 위의 역
 *   - TAILQ_ENTRY_CLEAR(elm, field): 제거 후 엔트리를 "리스트에 없음" 상태로 표시
 *   - TAILQ_REMOVE_CLEAR(head, elm, field): TAILQ_REMOVE + ENTRY_CLEAR 합성
 */

#ifndef SPDK_QUEUE_H             /* [한국어] include 가드 시작 */
#define SPDK_QUEUE_H             /* [한국어] 가드 심볼 */

#ifdef __cplusplus               /* [한국어] C++ 링크 규약 가드 — queue.h는 매크로만 제공하나 관례 유지 */
extern "C" {
#endif

#include <sys/cdefs.h>           /* [한국어] BSD 스타일 컴파일러 attribute 매크로 (__unused, __dead2 등)
                                  *  - sys/queue.h가 내부적으로 __unused 등을 참조할 수 있으므로 선행 포함 */
#include <sys/queue.h>           /* [한국어] BSD 표준 queue 매크로 본체 — SLIST/LIST/STAILQ/TAILQ/CIRCLEQ 정의 */

/*
 * The SPDK NVMe driver was originally ported from FreeBSD, which makes
 *  use of features in FreeBSD's queue.h that do not exist on Linux.
 *  Include a header with these additional features on Linux only.
 */
#ifdef __linux__                 /* [한국어] 리눅스 빌드만 확장 필요 — FreeBSD/macOS glibc는 이미 완전판 제공
                                  *  - 리눅스 glibc의 sys/queue.h는 SLIST_FOREACH_SAFE, STAILQ_LAST 등 일부 매크로를 생략하므로 SPDK가 보완 */
#include "spdk/queue_extras.h"   /* [한국어] 누락된 매크로 정의 — 실제 구현은 별도 파일로 분리해 헤더를 짧게 유지 */
#endif

/*
 * scan-build can't follow double pointers in queues and often assumes
 * that removed elements are still on the list. We redefine TAILQ_REMOVE
 * with extra asserts to silence it.
 */
#ifdef __clang_analyzer__        /* [한국어] Clang static analyzer(scan-build)로 빌드 중일 때만 활성화 — 일반 빌드 영향 0
                                  *  - scan-build는 tqe_prev/tqe_next 이중 포인터 추적에 약해 use-after-remove 오탐을 생성
                                  *  - 아래 재정의는 분석기에게 "이 함수 호출 후 elm은 리스트에 없음"을 assert로 명확히 알려 오탐 제거 */
#undef TAILQ_REMOVE              /* [한국어] 표준 정의를 제거 후 확장판으로 교체 */
#define TAILQ_REMOVE(head, elm, field) do {				\
	__typeof__(elm) _elm;						\
                                 /* [한국어] 이후 TAILQ_FOREACH에서 사용할 임시 포인터 — GCC 확장 __typeof__로 elm과 같은 타입 선언 */
	if (((elm)->field.tqe_next) != NULL)				\
                                 /* [한국어] 다음 엔트리가 존재하면 — 중간/머리 엔트리 제거 경로 */
		(elm)->field.tqe_next->field.tqe_prev =			\
		    (elm)->field.tqe_prev;				\
                                 /* [한국어] 다음 엔트리의 tqe_prev를 제거 대상의 tqe_prev로 이음 — 자기 자신을 건너뛰도록 재연결 */
	else								\
                                 /* [한국어] 다음 엔트리가 NULL이면 — 제거 대상이 리스트의 마지막 */
		(head)->tqh_last = (elm)->field.tqe_prev;		\
                                 /* [한국어] head의 tqh_last(마지막 엔트리 주소 저장소)를 제거 대상 이전의 tqe_prev로 갱신 */
	*(elm)->field.tqe_prev = (elm)->field.tqe_next;			\
                                 /* [한국어] 이중 포인터 관용구 — 이전 엔트리의 tqe_next(또는 head의 tqh_first)가 elm을 가리키던 것을 elm->next로 교체 */
	/* make sure the removed elm is not on the list anymore */	\
	TAILQ_FOREACH(_elm, head, field) {				\
                                 /* [한국어] 제거 직후 head 전체를 순회하며 elm이 남아있지 않음을 확인 — scan-build가 추적 가능한 시나리오 제공 */
		assert(_elm != elm);					\
                                 /* [한국어] elm이 다시 발견되면 논리 오류 — 디버그 빌드에서 프로세스 중단 */
	}								\
} while (0)                      /* [한국어] do-while(0) 관용구 — 여러 문장을 하나의 statement로 묶어 if/else 중간에서도 안전 */
#endif

/*
 * Check if an entry is on any TAILQ list.
 *
 * Should only be used on zero initalized entries or after
 * calling TAILQ_REMOVE_CLEAR() or TAILQ_ENTRY_CLEAR().
 */
#define TAILQ_ENTRY_ENQUEUED(elm, field)				\
    ((elm)->field.tqe_prev != NULL)
                                 /* [한국어] tqe_prev가 NULL이 아니면 리스트에 속해 있음
                                  *  - 표준 TAILQ는 removed 후에도 tqe_prev가 stale하게 남을 수 있어, 이 매크로는 반드시 초기화/클리어된 엔트리에서만 사용해야 함
                                  *  - SPDK 관례: 엔트리를 포함하는 구조체를 zero-init하거나, 제거 시 TAILQ_REMOVE_CLEAR/TAILQ_ENTRY_CLEAR로 명시 클리어 */

/*
 * Check if an entry is not on any TAILQ list.
 *
 * Should only be used on zero initalized entries or after
 * calling TAILQ_REMOVE_CLEAR() or TAILQ_ENTRY_CLEAR().
 */
#define TAILQ_ENTRY_NOT_ENQUEUED(elm, field)				\
    (!TAILQ_ENTRY_ENQUEUED(elm, field))
                                 /* [한국어] ENQUEUED의 역 — 가독성 향상용 별칭. 동일한 전제 조건 적용 */

/*
 * Mark an entry as absent from any TAILQ list.
 *
 * Should be called once after TAILQ_REMOVE(), or on entries
 * that were not initalized to zero.
 */
#define TAILQ_ENTRY_CLEAR(elm, field) do {				\
	/* Ensure the entry was on a list before clearing */		\
	assert(TAILQ_ENTRY_ENQUEUED(elm, field));			\
                                 /* [한국어] 이중 클리어/미제거 엔트리에 대한 ENTRY_CLEAR는 논리 오류 — 디버그 빌드에서 감지 */
	(elm)->field.tqe_prev = NULL;					\
                                 /* [한국어] tqe_prev=NULL로 세팅 → 이후 TAILQ_ENTRY_ENQUEUED가 false 반환하도록 보장 */
} while (0)

/*
 * Remove entry from TAILQ list and mark it as absent.
 */
#define TAILQ_REMOVE_CLEAR(head, elm, field) do {			\
	TAILQ_REMOVE(head, elm, field);					\
                                 /* [한국어] 1단계: 표준 TAILQ_REMOVE (또는 scan-build 확장판) 호출 */
	TAILQ_ENTRY_CLEAR(elm, field);					\
                                 /* [한국어] 2단계: 엔트리를 "소속 없음" 상태로 마킹 — 후속 ENQUEUED 검사가 올바르게 false를 반환 */
} while (0)

#ifdef __cplusplus
}
#endif

#endif                           /* [한국어] include 가드 종료 */
