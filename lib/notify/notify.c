/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK Notify 라이브러리 코어 구현 (notify.c)
 *
 * === 파일의 역할 ===
 * SPDK 내부 모듈(주로 bdev 레이어)이 발생시키는 비동기 이벤트(예: bdev_register,
 * bdev_unregister, bdev_examine_start/end 등)를 외부 관찰자에게 전달하기 위한
 * 통합 알림(notification) 메커니즘을 제공한다. 이 파일은 다음 두 가지 핵심 기능을
 * 구현한다.
 *   1) 이벤트 "타입(type)" 등록 — 모듈이 자기 식별 문자열을 한 번 등록해 두면,
 *      향후 이벤트 발행 시 그 문자열을 키로 사용한다.
 *   2) 이벤트 발행/조회 — 발행된 이벤트를 단조 증가하는 64bit ID와 함께
 *      고정 크기 환형 버퍼(ring buffer)에 저장하고, 외부에서 last_id 기반으로
 *      증분 읽기(incremental fetch)를 할 수 있게 한다.
 *
 * 이 라이브러리는 스토리지 관리 도구(예: SPDK CLI, OpenStack/Kubernetes
 * 컨트롤러)가 SPDK 데몬의 동적 토폴로지 변화를 polling 방식으로 추적하기 위한
 * 토대를 제공한다. 핵심 설계는 "고정 크기 + wrap-around + monotonically
 * increasing global id"이며, 이로 인해 구독자가 일시적으로 바빠도 ID만 잘 기억해
 * 두면 마지막으로 읽은 위치 이후의 이벤트만 골라 가져올 수 있다(과거 1024개를
 * 넘어선 항목은 잃어버림 — at-most-once 보장).
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 전체 I/O 스택([Application]→[bdev]→[bdev module]→[NVMe]) 중에서
 * "관리/관찰(management/observation)" 평면의 인프라 컴포넌트이다. 데이터 평면
 * (실제 read/write I/O 경로)에는 영향을 주지 않으며, 컨트롤 평면에서 발행한
 * 이벤트를 외부 노출(JSON-RPC `notify_get_notifications`)하는 역할을 한다.
 *
 * 호출 체인:
 *   - 등록자(producer): bdev 코어(lib/bdev/bdev.c)가 시작 시
 *     spdk_notify_type_register("bdev_register") 등으로 타입을 등록하고,
 *     bdev이 추가/삭제/스캔될 때 spdk_notify_send()를 호출.
 *   - 조회자(consumer): RPC 핸들러(lib/notify/notify_rpc.c)의
 *     rpc_notify_get_types / rpc_notify_get_notifications가
 *     spdk_notify_foreach_type / spdk_notify_foreach_event를 호출하여
 *     JSON 응답을 만든다.
 *
 * 실행 컨텍스트는 호스트 유저스페이스 SPDK 데몬 프로세스. 발행/조회 모두
 * 임의의 SPDK thread에서 발생할 수 있으므로, 구조적으로 락(`pthread_mutex`)을
 * 둔다(SPDK는 일반적으로 lockless 설계를 선호하지만 알림 경로는 호출 빈도가
 * 낮고 구독자가 다수일 수 있으므로 정확성·단순성을 우선한 선택).
 *
 * === 타 모듈과의 연결 ===
 *  - include/spdk/notify.h: 외부 공개 API 정의 — `struct spdk_notify_event`,
 *    `spdk_notify_type_register`, `spdk_notify_send`,
 *    `spdk_notify_foreach_type/event`, 콜백 typedef.
 *  - lib/bdev/bdev.c: 이벤트 producer. bdev 객체 라이프사이클 단계마다
 *    spdk_notify_send(type, name)로 발행한다.
 *  - lib/notify/notify_rpc.c: 이벤트 consumer. JSON-RPC `notify_get_types` 와
 *    `notify_get_notifications`을 등록하여 외부에 노출한다.
 *  - lib/util/string.c (spdk_strcpy_pad): 이벤트 문자열 필드를 NUL-padding
 *    형태로 안전하게 복사할 때 사용.
 *
 * 데이터 흐름:
 *   producer → spdk_notify_send → g_events[ring slot] (mutex 보호)
 *                                  ↑
 *   consumer ← spdk_notify_foreach_event ← g_events_head 단조 증가
 *
 * 공유 자료구조: g_events 환형 버퍼, g_events_head 글로벌 카운터,
 * g_notify_types 단방향 TAILQ. 모두 g_events_lock으로 보호된다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - struct spdk_notify_type: 등록된 알림 타입 노드 (이름 + TAILQ 링크).
 *  - g_events[]: SPDK_NOTIFY_MAX_EVENTS(1024) 크기의 환형 이벤트 버퍼.
 *  - g_events_head: 다음 발행 슬롯의 단조 증가 ID. wrap 없음(64bit).
 *  - spdk_notify_type_register(type): 새 타입 등록. 중복 시 기존 노드 반환.
 *  - spdk_notify_send(type, ctx): 이벤트 발행. 반환값은 발행된 이벤트 ID.
 *  - spdk_notify_foreach_event(start, max, cb, ctx): start_idx부터 최대 max개
 *    이벤트를 콜백으로 전달. 1024개 윈도우를 벗어난 시작 위치는 자동 보정.
 *  - spdk_notify_foreach_type(cb, ctx): 등록된 모든 타입을 콜백으로 순회.
 */

#include <sys/queue.h>
/* [한국어] BSD sys/queue.h 매크로 모음. TAILQ_HEAD/TAILQ_ENTRY/TAILQ_FOREACH 등
 * 단방향/양방향 큐 매크로가 정의되어 있다. SPDK는 이 매크로를 자체 헤더
 * (spdk/queue.h)로도 래핑하지만, 일부 시스템 헤더 호환성을 위해 직접 include 한다. */

#include "spdk/stdinc.h"
/* [한국어] SPDK가 권장하는 표준 라이브러리 일괄 include. <stdio.h>, <stdlib.h>,
 * <string.h>, <stdint.h>, <pthread.h> 등이 한 번에 들어온다. 플랫폼별 차이를
 * 흡수하는 것이 목적. */
#include "spdk/util.h"
/* [한국어] SPDK_COUNTOF, spdk_min/max, container_of 등 일반 매크로 모음. */
#include "spdk/queue.h"
/* [한국어] SPDK가 sys/queue.h 매크로를 다시 정의한 헤더(이름 충돌·이식성
 * 보정용). g_notify_types TAILQ 정의에 사용된다. */
#include "spdk/string.h"
/* [한국어] spdk_strcpy_pad 등 SPDK 전용 문자열 유틸. 고정 길이 필드의
 * 패딩(NUL 채움)이 필요한 ABI/RPC 호환 경로에서 사용된다. */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG / SPDK_NOTICELOG / SPDK_DEBUGLOG 등 로깅 매크로. */

#include "spdk/notify.h"
/* [한국어] 본 라이브러리가 외부에 노출하는 공개 헤더.
 * `struct spdk_notify_event`(고정 길이 type/ctx 필드)와 콜백 typedef 가 정의되어
 * 있다. 본 .c 파일은 이 헤더의 함수 선언을 구현(implement)하는 역할. */

/* [한국어] 환형 버퍼의 슬롯 개수 (1024). 메모리 사용량과 "구독자가 따라잡지
 * 못해 잃어버리는 이벤트"의 트레이드오프 지점. 이벤트 1개 = sizeof(spdk_notify_event)
 * 이므로 작은 상수. 발행자가 빠를수록 이전 슬롯이 덮어쓰여지므로 구독자는
 * 주기적으로 polling 해야 한다. wrap 발생 시
 * spdk_notify_foreach_event가 자동으로 (head - MAX) 위치로 보정하여 잃어버린
 * 항목을 건너뛴다. */
#define SPDK_NOTIFY_MAX_EVENTS	1024

/*
 * [한국어]
 * struct spdk_notify_type — 등록된 알림 "타입" 한 건을 표현하는 노드.
 *
 * SPDK 모듈이 자신을 식별하는 문자열(예: "bdev_register")을 미리 등록해 두면,
 * 외부(RPC notify_get_types)는 이를 enumerate 하여 "어떤 알림이 가능한지"를
 * 알 수 있다. 등록은 일회성으로 모듈 초기화 시점에 이루어지며, 해제(unregister)
 * API는 의도적으로 제공하지 않는다(데몬 수명 동안 영구 보존).
 */
struct spdk_notify_type {
	char name[SPDK_NOTIFY_MAX_NAME_SIZE];
	/* [한국어] 알림 타입 이름 (예: "bdev_register", "bdev_unregister").
	 * 설정자: spdk_notify_type_register()가 snprintf로 1회만 채움.
	 * 읽는 자: spdk_notify_type_get_name(), 외부 RPC notify_get_types.
	 * 값 범위: 1 <= strlen < SPDK_NOTIFY_MAX_NAME_SIZE. 빈 문자열 금지.
	 * 동기화: 등록 후 변경되지 않으므로 읽기 시 락 불요(다만 리스트 순회는
	 *  g_events_lock 보호하에 수행). */

	TAILQ_ENTRY(spdk_notify_type) tailq;
	/* [한국어] g_notify_types TAILQ에 연결되는 링크 필드(prev/next 포인터 쌍).
	 * 설정자: TAILQ_INSERT_TAIL이 명시적으로 갱신.
	 * 읽는 자: TAILQ_FOREACH 매크로가 순회 시 사용.
	 * 동기화: 리스트 변경/순회 모두 g_events_lock 보호. */
};

/* [한국어] g_events / g_events_head / g_notify_types 모두를 보호하는 글로벌 락.
 * SPDK는 일반적으로 lockless 데이터 평면을 선호하지만, 알림은 (1) 호출 빈도가
 * 낮고 (2) 발행자/구독자가 임의의 SPDK thread/외부 RPC 핸들러일 수 있어
 * cross-thread 접근이 자연스러우므로, 단순한 정확성을 위해 mutex를 사용한다.
 * PTHREAD_MUTEX_INITIALIZER로 정적 초기화하므로 별도 init 함수가 불필요. */
static pthread_mutex_t g_events_lock = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] 환형 이벤트 버퍼. 인덱스는 (g_events_head - 1) % SPDK_NOTIFY_MAX_EVENTS
 * 형태로 wrap-around. BSS에 .static 으로 잡혀 프로세스 시작 시 0-fill 됨. */
static struct spdk_notify_event g_events[SPDK_NOTIFY_MAX_EVENTS];
/* [한국어] 다음에 발행할 슬롯의 ID. 단조 증가하며 wrap 없이 64bit 카운터로 유지
 * 한다. 발행 후 슬롯 위치는 (head++) % MAX 로 결정. 64bit이므로 실질적으로
 * 오버플로우는 발생하지 않는다(매 ns 1개 발행 가정 시 약 580년). */
static uint64_t g_events_head;

/* [한국어] 등록된 모든 알림 타입을 보관하는 단방향 TAILQ. TAILQ_HEAD_INITIALIZER
 * 로 정적 초기화되어 NULL 포인터 안전. 검색은 O(N)이지만 N(타입 종류 수)이
 * 작으므로 충분. */
static TAILQ_HEAD(, spdk_notify_type) g_notify_types = TAILQ_HEAD_INITIALIZER(g_notify_types);

/*
 * [한국어]
 * spdk_notify_type_register - 새 알림 타입을 글로벌 리스트에 등록한다.
 *
 * @type: 등록할 타입 이름 (NUL 종료 ASCII 문자열, 1 <= 길이 < MAX_NAME_SIZE).
 *        보통 모듈 초기화 시점에 컴파일타임 상수로 전달된다.
 * @return: 등록(또는 기존)된 노드 포인터. 실패 시 NULL.
 *
 * 동작:
 *   1) 입력 검증(NULL/빈 문자열/너무 긴 이름 거부).
 *   2) g_events_lock 획득.
 *   3) 동일 이름이 이미 있으면 그 노드를 반환(중복 안전, idempotent).
 *   4) 없으면 calloc 후 이름 복사, 리스트 꼬리에 삽입.
 *   5) 락 해제.
 *
 * 호출 컨텍스트: 일반적으로 SPDK 데몬 부팅 시 1회씩 (각 모듈 init 단계).
 * 다만 안전성을 위해 동시 호출도 허용된다(락으로 보호).
 *
 * 호출 체인:
 *   bdev 코어 init / 사용자 모듈 init → [spdk_notify_type_register]
 *     → calloc, snprintf, TAILQ_INSERT_TAIL
 */
struct spdk_notify_type *
spdk_notify_type_register(const char *type)
{
	struct spdk_notify_type *it = NULL;
	/* [한국어] 검색 루프 변수 겸 반환값 holder. 중복 발견 시 그 포인터를 그대로
	 * 반환하기 위한 용도이며, 새로 할당한 경우에도 동일 변수에 저장. */

	if (!type) {
		/* [한국어] NULL 포인터 방어. 외부 호출자가 잘못된 인자를 넘긴 경우. */
		SPDK_ERRLOG("Invalid notification type %p\n", type);
		/* [한국어] 디버깅을 돕기 위해 NULL 포인터 값을 그대로 출력한다. */
		return NULL;
	} else if (!type[0] || strlen(type) >= SPDK_NOTIFY_MAX_NAME_SIZE) {
		/* [한국어] 빈 문자열 또는 고정 필드 길이를 초과하는 이름을 거부.
		 * spdk_notify_event::type 필드가 SPDK_NOTIFY_MAX_NAME_SIZE 이내이므로
		 * 이름이 그보다 길면 이벤트 발행 시 truncation이 발생할 수 있어
		 * 사전에 막는다. */
		SPDK_ERRLOG("Notification type '%s' too short or too long\n", type);
		return NULL;
	}

	pthread_mutex_lock(&g_events_lock);
	/* [한국어] 리스트 검색 + 노드 삽입을 atomic 하게 수행하기 위한 락 획득.
	 * 동시에 여러 모듈이 같은 이름으로 register를 호출하더라도 중복 노드가
	 * 만들어지지 않도록 보장한다. */
	TAILQ_FOREACH(it, &g_notify_types, tailq) {
		/* [한국어] 등록된 모든 타입을 선두부터 순회. */
		if (strcmp(type, it->name) == 0) {
			/* [한국어] 동일 이름이 이미 있으면 새로 만들지 않고 기존 노드를
			 * 반환하여 idempotent 한 동작을 보장. */
			SPDK_NOTICELOG("Notification type '%s' already registered.\n", type);
			goto out;
		}
	}

	it = calloc(1, sizeof(*it));
	/* [한국어] 새 타입 노드 할당. calloc 으로 0-init 되어 TAILQ 링크 안전성 확보.
	 * 실패 시 NULL이 그대로 반환된다(에러 로깅은 호출자 책임). */
	if (it == NULL) {
		goto out;
	}

	snprintf(it->name, sizeof(it->name), "%s", type);
	/* [한국어] 길이 제한이 검증되었지만, 안전을 위해 snprintf로 복사.
	 * 고정 필드이므로 항상 NUL-종료가 보장된다. */
	TAILQ_INSERT_TAIL(&g_notify_types, it, tailq);
	/* [한국어] 등록 순서를 유지하기 위해 꼬리에 삽입(FIFO). RPC 응답에서도
	 * 등록 순으로 노출됨. */

out:
	pthread_mutex_unlock(&g_events_lock);
	/* [한국어] 모든 경로(에러/중복/신규)에서 동일하게 락 해제. */
	return it;
}

/*
 * [한국어]
 * spdk_notify_type_get_name - 타입 노드의 이름 문자열 포인터를 반환.
 *
 * @type: spdk_notify_type 노드 포인터.
 * @return: 내부 name 필드를 가리키는 const 포인터(NUL 종료).
 *
 * 단순 게터(getter). 호출자는 반환된 포인터를 free 하지 말 것.
 * 콜백 spdk_notify_foreach_type 내부에서 이름만 필요할 때 사용된다.
 */
const char *
spdk_notify_type_get_name(const struct spdk_notify_type *type)
{
	return type->name;
	/* [한국어] 내부 필드의 read-only 노출. 노드는 등록 후 영구 존재하므로
	 * dangling pointer 위험이 없다. */
}


/*
 * [한국어]
 * spdk_notify_foreach_type - 등록된 모든 타입을 콜백으로 순회한다.
 *
 * @cb:  각 타입 노드에 대해 호출되는 콜백. 0 반환 시 계속, non-zero 반환 시 중단.
 * @ctx: 콜백에 전달되는 사용자 컨텍스트(불투명 포인터).
 *
 * RPC `notify_get_types` 핸들러가 JSON 배열을 만들 때 사용하는 enumerator.
 * 락 보유 상태에서 콜백이 실행되므로, 콜백 내부에서는 가벼운 작업만 수행해야
 * 하며 spdk_notify_send 등 락을 재획득하는 함수는 절대 호출 금지(deadlock).
 *
 * 호출 체인:
 *   rpc_notify_get_types → [spdk_notify_foreach_type] → notify_get_types_cb
 *     → spdk_json_write_string
 */
void
spdk_notify_foreach_type(spdk_notify_foreach_type_cb cb, void *ctx)
{
	struct spdk_notify_type *it;
	/* [한국어] TAILQ_FOREACH 순회 변수. */

	pthread_mutex_lock(&g_events_lock);
	/* [한국어] 순회 중 register/unregister가 끼어들지 못하도록 락 획득.
	 * 본 라이브러리는 unregister API가 없지만 register는 있으므로 일관성
	 * 측면에서 락이 필요. */
	TAILQ_FOREACH(it, &g_notify_types, tailq) {
		/* [한국어] 등록된 첫 노드부터 순회. */
		if (cb(it, ctx)) {
			/* [한국어] 콜백이 non-zero를 반환하면 즉시 중단(early-exit).
			 * 검색용 함수 패턴(예: 특정 이름을 찾으면 멈춤)을 지원하기 위함. */
			break;
		}
	}
	pthread_mutex_unlock(&g_events_lock);
}

/*
 * [한국어]
 * spdk_notify_send - 새 이벤트를 발행하여 환형 버퍼에 저장한다.
 *
 * @type: 이벤트 타입 문자열 (예: "bdev_register"). 사전에 register 되어 있을
 *        필요는 엄격하지 않지만, 외부 enumerate(notify_get_types)와 일관성을
 *        맞추기 위해 등록 후 사용을 권장.
 * @ctx:  이벤트별 식별자(예: bdev 이름). type 과 함께 의미를 결정.
 * @return: 발행된 이벤트의 글로벌 ID (g_events_head 의 사전 값). 구독자는 이 ID
 *          이전까지를 "이미 본 것"으로 처리할 수 있다.
 *
 * 동작:
 *   1) 락 획득.
 *   2) head 값을 capture, head 증가(다음 발행 위치).
 *   3) head % MAX 슬롯에 type/ctx를 NUL-padding 으로 복사.
 *   4) 락 해제.
 *
 * 발행 시 대상 슬롯이 과거에 이미 사용되었다면 그 데이터를 단순 덮어쓴다
 * (구독자가 따라잡지 못한 이벤트는 손실 — at-most-once). 손실 여부는 구독자
 * 측 spdk_notify_foreach_event가 (head - MAX)로 자동 보정하면서 감지한다.
 *
 * 호출 컨텍스트: bdev 코어/모듈 등 임의의 SPDK thread. 락이 짧게 잡히므로
 * 대기 시간은 무시할 수준. 그러나 데이터 평면 hot-path 내에서 호출되어서는
 * 안 된다(설계상 컨트롤 평면 이벤트만 사용).
 *
 * 호출 체인:
 *   bdev_register/unregister/examine_done 등 → [spdk_notify_send]
 *     → spdk_strcpy_pad
 */
uint64_t
spdk_notify_send(const char *type, const char *ctx)
{
	uint64_t head;
	/* [한국어] 발행 직전 head 값을 캡처할 로컬 변수. 반환값으로도 쓰인다. */
	struct spdk_notify_event *ev;
	/* [한국어] 실제 데이터를 쓸 환형 슬롯의 포인터. */

	pthread_mutex_lock(&g_events_lock);
	/* [한국어] head 캡처/증가 + 슬롯 데이터 쓰기까지를 atomic 하게 보호.
	 * 락 없이 head++만 atomic 으로 처리할 경우, 다른 thread가 같은 슬롯에
	 * 동시에 쓰는 race가 발생할 수 있다. */
	head = g_events_head;
	/* [한국어] 현재 발행 ID 캡처. 반환값이자 슬롯 인덱스 계산 기준. */
	g_events_head++;
	/* [한국어] 다음 발행 위치로 미리 진행. 단조 증가하며 wrap 하지 않는다. */

	ev = &g_events[head % SPDK_NOTIFY_MAX_EVENTS];
	/* [한국어] 환형 슬롯 인덱스 계산. modulo 1024 = 하위 10bit 마스크와 등가.
	 * 컴파일러는 상수 modulo를 비트 AND 로 최적화한다(MAX가 2^10이므로). */
	spdk_strcpy_pad(ev->type, type, sizeof(ev->type), '\0');
	/* [한국어] 고정 길이 필드에 NUL 패딩으로 복사. 짧은 문자열도 항상 동일
	 * 길이로 직렬화되도록 보장(외부 RPC가 read 시 buffer 전체를 일관되게
	 * 처리). */
	spdk_strcpy_pad(ev->ctx, ctx, sizeof(ev->ctx), '\0');
	/* [한국어] ctx 필드도 동일하게 NUL 패딩 복사. */
	pthread_mutex_unlock(&g_events_lock);

	return head;
	/* [한국어] 발행된 이벤트의 ID. 호출자는 로깅이나 후속 동기화 용도로 사용 가능. */
}

/*
 * [한국어]
 * spdk_notify_foreach_event - start_idx부터 시작해 최대 max개의 이벤트를
 *                              콜백으로 전달한다.
 *
 * @start_idx: 가져올 첫 이벤트 ID. 일반적으로 구독자가 마지막으로 본 ID + 1.
 *             head 윈도우(최근 1024개)를 벗어난 값은 자동으로 (head - 1024)로
 *             보정되며, 그 사이의 이벤트는 손실로 간주.
 * @max:       최대 전달 개수(구독자가 한 번에 읽을 한계).
 * @cb_fn:     이벤트 1건당 호출되는 콜백 (id, *ev, ctx). non-zero 반환 시 중단.
 * @ctx:       콜백 사용자 컨텍스트.
 * @return:    실제로 콜백에 전달된 이벤트 수.
 *
 * 동작 순서:
 *   1) 락 획득.
 *   2) start_idx 가 (head - MAX) 보다 작으면 보정(잃어버린 영역 스킵).
 *      이는 ring 이 한 바퀴 돈 후에는 그 이전 슬롯이 새 데이터로 덮였기
 *      때문에 의미 없는 데이터를 반환하지 않기 위함.
 *   3) start_idx < head 이고 i < max 인 동안 슬롯을 콜백에 전달.
 *   4) 콜백이 non-zero 반환 시 break (early-exit).
 *   5) 락 해제.
 *
 * incremental fetch 패턴:
 *   클라이언트 → "id=last_seen, max=N" 으로 polling
 *   서버      → 본 함수가 (last_seen, head) 구간을 N개까지 반환
 *   클라이언트 → 응답 마지막 항목의 id+1 을 다음 polling 의 start 로 사용
 *
 * 호출 체인:
 *   rpc_notify_get_notifications → [spdk_notify_foreach_event]
 *     → notify_get_notifications_cb → spdk_json_write_*
 */
uint64_t
spdk_notify_foreach_event(uint64_t start_idx, uint64_t max,
			  spdk_notify_foreach_event_cb cb_fn, void *ctx)
{
	uint64_t i;
	/* [한국어] 콜백에 전달된 누적 이벤트 수. 반환값으로도 사용. */

	pthread_mutex_lock(&g_events_lock);
	/* [한국어] 환형 버퍼와 head를 일관된 스냅샷으로 읽기 위한 락. 순회 중 새
	 * 발행이 끼어들면 인덱스 계산이 어긋날 수 있으므로 락 보유 상태에서
	 * 콜백을 직렬 호출. 콜백은 가벼운 직렬화 작업만 수행하도록 권장. */

	if (g_events_head > SPDK_NOTIFY_MAX_EVENTS && start_idx < g_events_head - SPDK_NOTIFY_MAX_EVENTS) {
		/* [한국어] head가 한 바퀴를 돌았고(MAX 초과), 요청한 시작 위치가
		 * 환형 윈도우(head-MAX, head)의 범위를 벗어난 경우(즉, 너무 옛날
		 * 항목 요청)에는 윈도우의 가장 오래된 항목으로 보정한다.
		 * 첫 조건(head > MAX)은 underflow 방지(head < MAX 이면 윈도우는
		 * 0..head 이므로 보정 불필요). */
		start_idx = g_events_head - SPDK_NOTIFY_MAX_EVENTS;
	}

	for (i = 0; start_idx < g_events_head && i < max; start_idx++, i++) {
		/* [한국어] 두 종료 조건:
		 *   (1) start_idx >= head : 더 이상 발행된 이벤트가 없음
		 *   (2) i >= max          : 호출자가 요청한 한계 도달
		 * 두 조건 중 하나라도 만족하면 종료. */
		if (cb_fn(start_idx, &g_events[start_idx % SPDK_NOTIFY_MAX_EVENTS], ctx)) {
			/* [한국어] 콜백에 (글로벌 id, 슬롯 포인터, ctx)를 전달.
			 * non-zero 반환 시 클라이언트가 조기 중단을 요청한 것으로 보고
			 * 즉시 빠져나간다. */
			break;
		}
	}
	pthread_mutex_unlock(&g_events_lock);

	return i;
	/* [한국어] 실제 처리된 개수. 호출자는 이를 통해 응답 길이/페이지네이션을
	 * 결정한다. */
}
