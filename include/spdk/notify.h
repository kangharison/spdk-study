/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK 이벤트 알림 레지스트리 (notify.h)
 *
 * === 파일의 역할 ===
 * SPDK 서브시스템 간 비동기 이벤트(예: bdev 등록/해제, NVMe 컨트롤러 연결
 * 상태 변화 등)를 링 버퍼에 기록하고 외부 관리 도구(RPC 클라이언트,
 * CLI, 매니지먼트 에이전트)가 폴링으로 조회할 수 있게 하는 간단한 알림 버스를
 * 정의한다.
 *
 * 디자인 요약:
 *   - 이벤트는 (type, ctx) 문자열 쌍으로 표현
 *   - 타입은 `spdk_notify_type_register()`로 사전에 등록
 *   - 송신자는 `spdk_notify_send(type, ctx)`로 이벤트 추가
 *   - 조회자는 `spdk_notify_foreach_event(start_idx, max, cb, ctx)`로
 *     일정 범위의 이벤트를 순회 (idx 기반 페이징)
 *
 * === 전체 아키텍처에서의 위치 ===
 * 구현: lib/notify/notify.c. 내부 링 버퍼는 뮤텍스 기반 → hot path 사용은
 * 권장되지 않음. 주로 관리/관측 경로.
 * 실행 컨텍스트: 모든 SPDK thread에서 호출 가능 (락 보호).
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/stdinc.h - 표준 타입
 *   - spdk/json.h   - 알림 JSON 포맷 변환 시 사용자 쪽에서 필요
 *   - spdk/queue.h  - 내부 TAILQ 기반 타입 리스트
 * 의존하는 모듈:
 *   - lib/bdev (bdev register/unregister 이벤트)
 *   - lib/nvme (컨트롤러 attach/detach 이벤트)
 *   - RPC 핸들러 (notify_get_types, notify_get_notifications)
 * 공유 자료구조: 내부 링 버퍼와 타입 리스트 — 뮤텍스 보호.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_notify_type / spdk_notify_event
 *   - spdk_notify_type_register: 새 타입 등록
 *   - spdk_notify_type_get_name: 타입 이름 조회
 *   - spdk_notify_foreach_type:  모든 타입 순회
 *   - spdk_notify_send:          이벤트 송신 (이벤트 인덱스 반환)
 *   - spdk_notify_foreach_event: 이벤트 범위 조회
 */

#ifndef SPDK_NOTIFY_H            /* [한국어] include 가드 */
#define SPDK_NOTIFY_H

#include "spdk/stdinc.h"         /* [한국어] 표준 타입 */
#include "spdk/json.h"           /* [한국어] 공개 헤더 규약 — 향후 JSON 컨버터가 추가될 수 있어 선행 포함 */
#include "spdk/queue.h"          /* [한국어] 내부 TAILQ 사용 관련 (공개 API에서는 직접 노출 없음) */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque event type.
 */
struct spdk_notify_type;
                                 /* [한국어] opaque — 내부 표현 숨김. 포인터로만 외부 노출 */

typedef int (*spdk_notify_foreach_type_cb)(const struct spdk_notify_type *type, void *ctx);
                                 /* [한국어] 타입 순회 콜백. non-zero 반환 시 순회 중단 */

#define SPDK_NOTIFY_MAX_NAME_SIZE 128
                                 /* [한국어] 이벤트 타입 문자열 최대 길이 (NUL 포함) */
#define SPDK_NOTIFY_MAX_CTX_SIZE 128
                                 /* [한국어] 컨텍스트 문자열 최대 길이. 긴 메타데이터는 별도 경로로 전달 */

struct spdk_notify_event {
	char type[SPDK_NOTIFY_MAX_NAME_SIZE];
                                 /* [한국어] 이벤트 타입 문자열 (등록된 타입과 일치)
                                  *  - 설정자: spdk_notify_send
                                  *  - 읽는 자: spdk_notify_foreach_event 콜백 */
	char ctx[SPDK_NOTIFY_MAX_CTX_SIZE];
                                 /* [한국어] 이벤트 컨텍스트 (예: bdev 이름, qpair ID 등 자유 형식 ASCII) */
};

/**
 * Callback type for event enumeration.
 *
 * \param idx Event index
 * \param event Event data
 * \param ctx User context
 * \return Non zero to break iteration.
 */
typedef int (*spdk_notify_foreach_event_cb)(uint64_t idx, const struct spdk_notify_event *event,
		void *ctx);
                                 /* [한국어] 이벤트 순회 콜백. idx는 전역 단조 증가 인덱스 (링 버퍼 wraparound는 드문 편) */

/**
 * Register \c type as new notification type.
 *
 * \note This function is thread safe.
 *
 * \param type New notification type to register.
 * \return registered notification type or NULL on failure.
 */
struct spdk_notify_type *spdk_notify_type_register(const char *type);
/*
 * [한국어]
 * spdk_notify_type_register - 새 알림 타입 등록
 * 초기화 시 한 번만 호출. 스레드 세이프.
 */

/**
 * Return name of the notification type.
 *
 * \param type Notification type we are talking about.
 * \return Name of notification type.
 */
const char *spdk_notify_type_get_name(const struct spdk_notify_type *type);
/*
 * [한국어]
 * spdk_notify_type_get_name - 등록 타입의 이름 조회
 */

/**
 * Call cb_fn for all event types.
 *
 * \note Whole function call is under lock so user callback should not sleep.
 * \param cb_fn
 * \param ctx
 */
void spdk_notify_foreach_type(spdk_notify_foreach_type_cb cb_fn, void *ctx);
/*
 * [한국어]
 * spdk_notify_foreach_type - 등록된 모든 타입에 콜백 실행
 * 주의: 내부 락을 잡고 콜백 실행 → 콜백에서 blocking I/O 금지
 */

/**
 * Send given notification.
 *
 * \param type Notification type
 * \param ctx Notification context
 *
 * \return Event index.
 */
uint64_t spdk_notify_send(const char *type, const char *ctx);
/*
 * [한국어]
 * spdk_notify_send - 이벤트 1건 송신
 * @return 할당된 전역 인덱스. 조회자는 이 인덱스로 이후 이벤트를 페이징
 */

/**
 * Call cb_fn with events from given range.
 *
 * \note Whole function call is under lock so user callback should not sleep.
 *
 * \param start_idx First event index
 * \param cb_fn User callback function. Return non-zero to break iteration.
 * \param max Maximum number of invocations of user callback function.
 * \param ctx User context
 * \return Number of user callback invocations
 */
uint64_t spdk_notify_foreach_event(uint64_t start_idx, uint64_t max,
				   spdk_notify_foreach_event_cb cb_fn, void *ctx);
/*
 * [한국어]
 * spdk_notify_foreach_event - 이벤트 조회
 * @start_idx: 조회 시작 인덱스 (이전 호출 결과의 last_idx + 1이 전형적)
 * @max: 콜백 최대 호출 횟수 (페이지 크기)
 * @return: 실제 콜백 호출 수
 */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NOTIFY_H */        /* [한국어] include 가드 종료 */
