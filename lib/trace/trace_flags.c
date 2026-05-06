/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK trace 메타데이터 등록 + flag 토글 구현 (trace_flags.c)
 *
 * === 파일의 역할 ===
 * trace.c가 ring buffer hot path와 shm lifecycle을 담당한다면, 본 파일은
 * "어떤 trace를 켤지/끌지(=flag), 그리고 어떤 trace가 존재하는지(=메타데이터 등록)"
 * 두 측면을 담당한다. 구체적으로:
 *   (1) tpoint group/tpoint 활성 비트마스크 set/clear/get (g_trace_file->tpoint_mask).
 *   (2) reg_fn 리스트 관리 (SPDK_TRACE_REGISTER_FN constructor가 추가).
 *   (3) tpoint 메타 등록 (이름/owner_type/object_type/인자 정의).
 *   (4) owner type 등록과 owner 인스턴스 동적 할당/해제 (lock-free ring 위에).
 *   (5) tpoint↔object cross-reference 등록 (parser용).
 *   (6) trace_flags_init/fini: trace.c가 호출하는 lifecycle 진입점.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 흐름:
 *   부팅 단계:
 *     앱 시작 전 — SPDK_TRACE_REGISTER_FN(...) constructor가 reg_fn 노드를
 *     g_reg_fn_head 리스트에 자동 추가 (tgroup_id 오름차순으로 정렬 삽입).
 *     spdk_trace_init → trace_flags_init() → 모든 reg_fn->reg_fn() 호출
 *     → 각 모듈이 spdk_trace_register_owner_type/object/description_ext 호출
 *     → g_trace_file->owner_type[]/object[]/tpoint[]에 메타데이터 채움.
 *     → owner_id ring buffer (16K 슬롯) 초기화 + pthread_spin_init.
 *   운영 중:
 *     RPC trace_set_tpoints → spdk_trace_set_tpoints → tpoint_mask[group] |= bits.
 *     RPC trace_clear_tpoints → spdk_trace_clear_tpoints → AND-NOT.
 *     서브시스템이 owner 등록/해제: register_owner (ring pop) / unregister_owner (ring push).
 *   종료:
 *     spdk_trace_cleanup → trace_flags_fini() → spinlock destroy + free(ring).
 * 실행 컨텍스트:
 *   - mask set/clear: jsonrpc 서버 스레드 (메인 reactor).
 *   - register_owner_type/object/description: trace_flags_init 동안 메인 스레드.
 *   - register_owner / unregister_owner: 임의 SPDK 스레드(다양한 reactor) —
 *     pthread_spin_lock으로 ring을 보호 (멀티스레드 호출 가능).
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더):
 *   spdk/env.h        — spdk_get_ticks (owner.tsc 채움).
 *   spdk/trace.h      — 공개 API + 자료구조 정의.
 *   spdk/log.h        — SPDK_LOG_REGISTER_COMPONENT(trace) + SPDK_ERRLOG/DEBUGLOG.
 *   spdk/util.h       — SPDK_COUNTOF.
 *   spdk/bit_array.h  — (현재 직접 사용 없음, 헤더 호환성 유지).
 *   trace_internal.h  — trace_flags_init/fini 선언.
 * 의존하는 모듈: trace.c (init/fini 호출자), 모든 SPDK 라이브러리
 *               (SPDK_TRACE_REGISTER_FN, register_owner_type/object/description_ext 호출자),
 *               trace_rpc.c (RPC 응답에 reg_fn 리스트 순회 + tpoint mask 조회).
 * 데이터 흐름:
 *   constructor 시점: reg_fn 노드 → g_reg_fn_head linked list (tgroup_id 정렬).
 *   trace_flags_init: linked list → reg_fn() 호출 → g_trace_file 메타데이터.
 *   register_owner: g_owner_ids.ring → (head pop) → owner_id → spdk_get_trace_owner → owner 인스턴스.
 *   unregister_owner: owner_id → ring tail push.
 *
 * === 주요 함수/구조체 요약 ===
 *   - g_reg_fn_head: SPDK_TRACE_REGISTER_FN으로 등록된 group reg_fn 정렬 리스트의 head.
 *   - g_owner_ids:   owner_id 풀 (lock-free ring + spinlock for safety).
 *     ├─ ring[size]: 사용 가능 owner_id들의 원형 큐.
 *     ├─ head:        다음 pop 위치 (register_owner).
 *     ├─ tail:        다음 push 위치 (unregister_owner).
 *     ├─ size:        ring 슬롯 수 = num_owners (16K).
 *     └─ lock:        head/tail/ring 접근 보호.
 *   - spdk_trace_get/set/clear_tpoints, _group_mask: tpoint_mask 비트 조작 API.
 *   - spdk_trace_set/clear_tpoint_group_mask: group 단위 일괄 set/clear.
 *   - spdk_trace_create_tpoint_group_mask: name → bit_mask 변환 (RPC가 사용).
 *   - spdk_trace_enable/disable_tpoint_group: 이름으로 group 일괄 활성/비활성.
 *   - spdk_trace_mask_usage: --help 출력용 group 이름 나열.
 *   - spdk_trace_register_owner_type/object: 분류 메타 등록.
 *   - _owner_set_description, spdk_trace_register_owner / unregister / set/append_description:
 *     owner 인스턴스 lifecycle.
 *   - trace_register_description, spdk_trace_register_description / _ext: tpoint 메타 등록.
 *   - spdk_trace_tpoint_register_relation: cross-ref 매핑.
 *   - spdk_trace_add_register_fn: SPDK_TRACE_REGISTER_FN constructor가 호출 — 정렬 삽입.
 *   - trace_flags_init/fini: lifecycle.
 */

#include "spdk/stdinc.h"        /* [한국어] 표준 타입 + 매크로. */

#include "spdk/env.h"           /* [한국어] spdk_get_ticks — owner 등록 TSC 기록. */
#include "spdk/trace.h"         /* [한국어] 공개 trace API + struct 정의. */
#include "spdk/log.h"           /* [한국어] SPDK_LOG_REGISTER_COMPONENT, SPDK_ERRLOG/DEBUGLOG. */
#include "spdk/util.h"          /* [한국어] SPDK_COUNTOF (배열 크기 매크로). */
#include "trace_internal.h"     /* [한국어] trace_flags_init/fini 선언. */
#include "spdk/bit_array.h"     /* [한국어] (현재 직접 사용 없음 — 향후 호환 유지). */

/* [한국어] SPDK_TRACE_REGISTER_FN으로 등록된 group reg_fn들의 정렬 linked list 헤드.
 * 설정자: spdk_trace_add_register_fn — constructor에서 자동 삽입.
 * 읽는 자: trace_flags_init, RPC trace_get_tpoint_group_mask, mask_usage 등.
 * 정렬 키: tgroup_id 오름차순 (add_register_fn에서 보장).
 * 동기화: constructor는 main 진입 전 단일 스레드에서 일괄 실행 → race-free.
 *         이후엔 read-only로 사용. */
static struct spdk_trace_register_fn *g_reg_fn_head = NULL;

/* [한국어] owner_id 동적 할당을 위한 producer-consumer ring (재사용 풀).
 * head: pop 위치 (register), tail: push 위치 (unregister).
 * head==tail이면 풀 비어있음 (모두 사용 중). 일반적인 ring buffer 패턴.
 * 동기화: pthread_spinlock으로 보호. spinlock인 이유 — 호출 빈도가 낮고 critical
 *         section이 매우 짧음 (인덱스 1~2개 갱신).
 * 라이프사이클: trace_flags_init이 calloc + spin_init, trace_flags_fini가 free + destroy. */
static struct {
	uint16_t *ring;
	/* [한국어] 가용 owner_id 큐 (size 만큼 calloc).
	 * 초기화 시 256~num_owners-1 범위의 ID로 채움 (1~255는 레거시 poller_id 충돌 방지로 skip). */

	uint32_t head;
	/* [한국어] 다음 pop 위치 (register_owner가 증가). size에 도달하면 0으로 wrap. */

	uint32_t tail;
	/* [한국어] 다음 push 위치 (unregister_owner가 증가). size 도달 시 wrap. */

	uint32_t size;
	/* [한국어] ring 슬롯 수 = g_trace_file->num_owners (=16K). 2^N 가정 없음. */

	pthread_spinlock_t lock;
	/* [한국어] head/tail/ring 동시 접근 보호. user-space spinlock — busy-wait. */
} g_owner_ids;

/* [한국어] SPDK 로그 컴포넌트 "trace" 등록 — SPDK_DEBUGLOG(trace, ...) 사용 가능.
 * 매크로 내부에 constructor가 있어 자동 등록. */
SPDK_LOG_REGISTER_COMPONENT(trace)

/*
 * [한국어]
 * spdk_trace_get_tpoint_mask - 특정 group의 64-bit tpoint 활성 마스크 조회.
 *
 * @group_id: 0 ~ SPDK_TRACE_MAX_GROUP_ID-1.
 * @return: tpoint_mask[group_id] (잘못된 ID 또는 미초기화면 0).
 *
 * 컨텍스트: hot path가 아님 — RPC 응답이나 디버깅에서 호출.
 * 동기화: read-only 단일 워드 — atomic 보장 없으나 비트맵 정확성보다 일관성 중요도가 낮음.
 */
uint64_t
spdk_trace_get_tpoint_mask(uint32_t group_id)
{
	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		/* [한국어] 범위 초과 — 잘못된 호출. 에러 로그 후 0 반환. */
		SPDK_ERRLOG("invalid group ID %d\n", group_id);
		return 0ULL;
	}

	if (g_trace_file == NULL) {
		/* [한국어] trace 미초기화 상태 — 마스크 비어있는 것으로 간주. */
		return 0ULL;
	}

	return g_trace_file->tpoint_mask[group_id];
	/* [한국어] uint64_t 단일 word 읽기 — x86에서 자연 정렬되어 있으므로 torn read 없음. */
}

/*
 * [한국어]
 * spdk_trace_set_tpoints - 특정 group에 일부 tpoint 활성화 (OR 연산).
 *
 * @group_id:    대상 group.
 * @tpoint_mask: 활성화할 비트들.
 *
 * 동작: tpoint_mask[group_id] |= tpoint_mask
 *       이후 hot path의 spdk_trace_tpoint_enabled가 즉시 반영.
 * 동기화: |= 연산은 비-atomic. 동시 set/clear가 일어나도 last-writer-wins로 안전 (잠금 없음).
 *         단일 RPC 서버 스레드에서만 호출되는 패턴이라 실질 race 없음.
 */
void
spdk_trace_set_tpoints(uint32_t group_id, uint64_t tpoint_mask)
{
	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;                       /* [한국어] init 안됨 — 변경할 대상 없음. */
	}

	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		SPDK_ERRLOG("invalid group ID %d\n", group_id);
		return;                       /* [한국어] 잘못된 group ID. */
	}

	g_trace_file->tpoint_mask[group_id] |= tpoint_mask;
	/* [한국어] OR — 기존 비트 유지하며 새로 활성화할 비트만 set. */
}

/*
 * [한국어]
 * spdk_trace_clear_tpoints - 특정 group에 일부 tpoint 비활성화 (AND-NOT).
 *
 * 동작: tpoint_mask[group_id] &= ~tpoint_mask
 * set의 대칭 — 매개변수 의미는 "끌 비트들".
 */
void
spdk_trace_clear_tpoints(uint32_t group_id, uint64_t tpoint_mask)
{
	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		SPDK_ERRLOG("invalid group ID %d\n", group_id);
		return;
	}

	g_trace_file->tpoint_mask[group_id] &= ~tpoint_mask;
	/* [한국어] AND-NOT — 인자에 표시된 비트만 clear, 나머지는 유지. */
}

/*
 * [한국어]
 * spdk_trace_get_tpoint_group_mask - 활성 tpoint가 1개 이상인 group들의 비트맵 반환.
 *
 * @return: 비트 i = group i에 enabled tpoint가 있는지 여부.
 *
 * 사용처: RPC trace_get_info/trace_get_tpoint_group_mask 응답 요약.
 * 시간 복잡도: O(SPDK_TRACE_MAX_GROUP_ID=20).
 */
uint64_t
spdk_trace_get_tpoint_group_mask(void)
{
	uint64_t mask = 0x0;                  /* [한국어] 누적 결과 비트맵. */
	int i;

	for (i = 0; i < SPDK_TRACE_MAX_GROUP_ID; i++) {
		if (spdk_trace_get_tpoint_mask(i) != 0) {
			/* [한국어] 이 group의 64-bit 마스크가 비어있지 않으면 group 활성으로 간주. */
			mask |= (1ULL << i);
		}
	}

	return mask;
}

/*
 * [한국어]
 * spdk_trace_set_tpoint_group_mask - 비트맵으로 표시된 group들의 모든 tpoint 활성.
 *
 * @tpoint_group_mask: 활성화할 group의 비트맵 (예: 0x5 → group 0과 2).
 *
 * 동작: 각 비트마다 spdk_trace_set_tpoints(i, -1ULL) → 64개 tpoint 모두 set.
 * CLI 옵션 -e bdev,nvme이나 RPC enable_tpoint_group이 호출.
 */
void
spdk_trace_set_tpoint_group_mask(uint64_t tpoint_group_mask)
{
	int i;

	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	for (i = 0; i < SPDK_TRACE_MAX_GROUP_ID; i++) {
		if (tpoint_group_mask & (1ULL << i)) {
			/* [한국어] i번째 비트 set이면 group i 일괄 활성화. */
			spdk_trace_set_tpoints(i, -1ULL);
			/* [한국어] -1ULL = 0xFFFFFFFFFFFFFFFF → 64개 tpoint 모두 set. */
		}
	}
}

/*
 * [한국어]
 * spdk_trace_clear_tpoint_group_mask - 위 함수의 대칭. 모든 tpoint 비활성.
 */
void
spdk_trace_clear_tpoint_group_mask(uint64_t tpoint_group_mask)
{
	int i;

	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	for (i = 0; i < SPDK_TRACE_MAX_GROUP_ID; i++) {
		if (tpoint_group_mask & (1ULL << i)) {
			spdk_trace_clear_tpoints(i, -1ULL);
			/* [한국어] -1ULL을 clear → 64개 tpoint 모두 0. */
		}
	}
}

/*
 * [한국어]
 * spdk_trace_get_first_register_fn - reg_fn 정렬 리스트 head 반환.
 *
 * @return: g_reg_fn_head (NULL이면 등록된 group 없음).
 *
 * trace_flags_init과 외부 RPC 핸들러가 순회 시작점으로 사용.
 */
struct spdk_trace_register_fn *
spdk_trace_get_first_register_fn(void)
{
	return g_reg_fn_head;
	/* [한국어] read-only 단일 포인터 반환 — 직접 노출. */
}

/*
 * [한국어]
 * spdk_trace_get_next_register_fn - 다음 reg_fn 노드 반환 (singly linked list).
 *
 * @register_fn: 현재 노드 (NULL 금지).
 * @return: 다음 노드 / 마지막이면 NULL.
 */
struct spdk_trace_register_fn *
spdk_trace_get_next_register_fn(struct spdk_trace_register_fn *register_fn)
{
	return register_fn->next;
	/* [한국어] 단순 next 필드 접근 — caller가 NULL 검사 책임. */
}

/*
 * [한국어]
 * spdk_trace_create_tpoint_group_mask - group 이름 → group의 single-bit mask 변환.
 *
 * @group_name: 단일 group 이름 ("bdev", "nvme") 또는 "all".
 * @return: bit_mask. "all"이면 모든 등록 group의 비트 OR. 매칭 실패 시 0.
 *
 * 사용처:
 *   - trace_rpc.c: RPC 핸들러가 name → mask로 변환 후 set/clear에 전달.
 *   - spdk_app -e <group> 옵션 처리에서 호출.
 *
 * 시간 복잡도: O(N)에서 N=등록된 group 수 (선형 스캔). group 수가 작아 무관.
 */
uint64_t
spdk_trace_create_tpoint_group_mask(const char *group_name)
{
	uint64_t tpoint_group_mask = 0;
	struct spdk_trace_register_fn *register_fn;

	register_fn = spdk_trace_get_first_register_fn();
	if (strcmp(group_name, "all") == 0) {
		/* [한국어] 특수 키워드 "all" → 모든 group 비트 OR 반환. */
		while (register_fn) {
			tpoint_group_mask |= (1UL << register_fn->tgroup_id);
			/* [한국어] 각 group의 single-bit ID를 누적. */
			register_fn = spdk_trace_get_next_register_fn(register_fn);
		}
	} else {
		/* [한국어] 일반 case — 이름 매칭으로 단일 group 찾기. */
		while (register_fn) {
			if (strcmp(group_name, register_fn->name) == 0) {
				break;            /* [한국어] 매칭 — 루프 종료. */
			}

			register_fn = spdk_trace_get_next_register_fn(register_fn);
		}

		if (register_fn != NULL) {
			/* [한국어] 매칭된 노드의 group ID로 single-bit mask 생성. */
			tpoint_group_mask |= (1UL << register_fn->tgroup_id);
		}
		/* [한국어] register_fn==NULL이면 알 수 없는 이름 → 0 반환 (호출자가 invalid 처리). */
	}

	return tpoint_group_mask;
}

/*
 * [한국어]
 * spdk_trace_enable_tpoint_group - 이름으로 group 일괄 활성화.
 *
 * @group_name: group 이름 또는 "all".
 * @return: 0 성공 / -1 실패 (trace 미초기화 또는 이름 불일치).
 *
 * 동작: name → mask 변환 → spdk_trace_set_tpoint_group_mask(mask) 호출.
 * 호출 체인: RPC trace_enable_tpoint_group, app -e 옵션 → 이 함수.
 */
int
spdk_trace_enable_tpoint_group(const char *group_name)
{
	uint64_t tpoint_group_mask = 0;

	if (g_trace_file == NULL) {
		return -1;                    /* [한국어] init 안됨 → 활성화 대상 없음. */
	}

	tpoint_group_mask = spdk_trace_create_tpoint_group_mask(group_name);
	if (tpoint_group_mask == 0) {
		return -1;                    /* [한국어] 알 수 없는 이름. */
	}

	spdk_trace_set_tpoint_group_mask(tpoint_group_mask);
	/* [한국어] 비트별 64개 tpoint 모두 set. */
	return 0;
}

/*
 * [한국어]
 * spdk_trace_disable_tpoint_group - enable의 대칭. 이름으로 group 일괄 비활성화.
 *
 * @return: 0 성공 / -1 실패.
 */
int
spdk_trace_disable_tpoint_group(const char *group_name)
{
	uint64_t tpoint_group_mask = 0;

	if (g_trace_file == NULL) {
		return -1;
	}

	tpoint_group_mask = spdk_trace_create_tpoint_group_mask(group_name);
	if (tpoint_group_mask == 0) {
		return -1;
	}

	spdk_trace_clear_tpoint_group_mask(tpoint_group_mask);
	/* [한국어] 64개 tpoint 모두 clear. */
	return 0;
}

/*
 * [한국어]
 * spdk_trace_mask_usage - --help 출력에 사용되는 group 이름 나열 함수.
 *
 * @f:         출력 파일 (보통 stderr).
 * @tmask_arg: CLI에서 사용되는 옵션 이름 (예: "--tpoint-group" 짝의 short 옵션).
 *
 * 동작:
 *   - 옵션 사용법 라인 출력.
 *   - LINE_PREFIX 들여쓰기로 등록된 모든 group 이름을 ", "로 구분해 나열.
 *   - 한 줄이 100자를 넘기면 자동 줄바꿈.
 *   - 마지막에 "all)" 추가 → 사용자에게 "all"도 유효한 키워드임을 알림.
 *   - 후속 라인에 tpoint_mask 사용법 (예: "bdev:0x1") 안내.
 *
 * 컨텍스트: spdk_app --help 처리 (메인 스레드 단일 호출).
 */
void
spdk_trace_mask_usage(FILE *f, const char *tmask_arg)
{
#define LINE_PREFIX			"                           "
	/* [한국어] group 이름들이 정렬되어 출력될 때의 들여쓰기 (27 공백). */
#define ENTRY_SEPARATOR			", "
	/* [한국어] group 이름 사이 구분자 (", "). */
#define MAX_LINE_LENGTH			100
	/* [한국어] 자동 줄바꿈 임계값. 터미널 가독성 위한 80~100 범위. */
	uint64_t prefix_len = strlen(LINE_PREFIX);
	uint64_t separator_len = strlen(ENTRY_SEPARATOR);
	const char *first_entry = "group_name - tracepoint group name for spdk trace buffers (";
	/* [한국어] 첫 번째 group 이름 앞에 출력될 안내 문구. */
	const char *last_entry = "all).";
	/* [한국어] 마지막에 닫히는 ")" 포함 — "all"도 유효함을 명시. */
	uint64_t curr_line_len;               /* [한국어] 현재 라인의 누적 길이 (개행 결정용). */
	uint64_t curr_entry_len;              /* [한국어] 현재 처리 중인 group 이름 길이. */
	struct spdk_trace_register_fn *register_fn;

	fprintf(f, " %s, --tpoint-group <group-name>[:<tpoint_mask>]\n", tmask_arg);
	/* [한국어] 옵션 시그니처 출력 (예: " -e, --tpoint-group <group-name>[:<tpoint_mask>]"). */
	fprintf(f, "%s%s", LINE_PREFIX, first_entry);
	/* [한국어] 두 번째 라인 시작: 들여쓰기 + 안내 문구. */
	curr_line_len = prefix_len + strlen(first_entry);
	/* [한국어] 현재 라인 길이 추적 시작. */

	register_fn = g_reg_fn_head;
	while (register_fn) {
		curr_entry_len = strlen(register_fn->name);
		if ((curr_line_len + curr_entry_len + separator_len > MAX_LINE_LENGTH)) {
			/* [한국어] 추가하면 100자 초과 → 줄바꿈 + 새 라인 들여쓰기. */
			fprintf(f, "\n%s", LINE_PREFIX);
			curr_line_len = prefix_len;
		}

		fprintf(f, "%s%s", register_fn->name, ENTRY_SEPARATOR);
		/* [한국어] "<group_name>, " 출력. */
		curr_line_len += curr_entry_len + separator_len;

		if (register_fn->next == NULL) {
			/* [한국어] 마지막 group 이후 — "all)" 추가하여 안내 문구 닫기. */
			if (curr_line_len + strlen(last_entry) > MAX_LINE_LENGTH) {
				/* [한국어] last_entry까지 합치면 줄 너무 길어짐 → 공백 하나로 시각적 분리. */
				fprintf(f, " ");
			}
			fprintf(f, "%s\n", last_entry);
			break;
		}

		register_fn = register_fn->next;
	}

	/* [한국어] tpoint_mask 사용법 다중 라인 안내 (예시: bdev:0x1). */
	fprintf(f, "%stpoint_mask - tracepoint mask for enabling individual tpoints inside\n",
		LINE_PREFIX);
	fprintf(f, "%sa tracepoint group. First tpoint inside a group can be enabled by\n",
		LINE_PREFIX);
	fprintf(f, "%ssetting tpoint_mask to 1 (e.g. bdev:0x1). Groups and masks can be\n",
		LINE_PREFIX);
	fprintf(f, "%scombined (e.g. thread,bdev:0x1). All available tpoints can be found\n",
		LINE_PREFIX);
	fprintf(f, "%sin /include/spdk_internal/trace_defs.h\n", LINE_PREFIX);
	/* [한국어] 사용자가 직접 tpoint_id를 찾고 싶을 때 참고할 헤더 위치 안내. */
}

/*
 * [한국어]
 * spdk_trace_register_owner_type - owner의 분류(클래스) 메타 등록.
 *
 * @type:      0~255 정수 ID (모듈이 자체 정의, OWNER_TYPE_NONE=0 예약).
 * @id_prefix: parser 출력에 사용할 한 글자 (예: 'q' for qpair).
 *
 * 동작: g_trace_file->owner_type[type]에 type/id_prefix 저장.
 * 컨텍스트: trace_flags_init이 호출하는 reg_fn() 내부에서 호출 — 메인 스레드 단일.
 * 주의: type=0은 OWNER_TYPE_NONE 예약 — assert로 막음.
 */
void
spdk_trace_register_owner_type(uint8_t type, char id_prefix)
{
	struct spdk_trace_owner_type *owner_type;

	assert(type != OWNER_TYPE_NONE);
	/* [한국어] 0은 "none" 예약값 — 등록 시도 금지. */

	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;                       /* [한국어] init 안됨 — 등록 불가. */
	}

	/* 'owner_type' has 256 entries and since 'type' is a uint8_t, it
	 * can't overrun the array.
	 */
	owner_type = &g_trace_file->owner_type[type];
	/* [한국어] type을 인덱스로 직접 사용 — uint8_t이므로 0~255 보장. */
	assert(owner_type->type == 0);
	/* [한국어] 이전에 등록된 적 없어야 함 (zero clear 상태에서 type=0). */

	owner_type->type = type;
	owner_type->id_prefix = id_prefix;
	/* [한국어] 메타 채움 — parser가 인스턴스 출력 시 prefix+id로 표시. */
}

/*
 * [한국어]
 * _owner_set_description - 내부 헬퍼: owner의 description 문자열 설정 또는 append.
 *
 * @owner_id:    대상 owner.
 * @description: 새 문자열.
 * @append:      true면 기존에 " " 구분자 + 새 문자열 추가, false면 덮어쓰기.
 *
 * 동작: 기존 description을 임시 buffer에 백업(append 시) → snprintf로 결합 작성.
 * 컨텍스트: 호출자가 g_owner_ids.lock을 잡은 상태에서 호출 — 자체 lock 없음.
 *           description은 가변 길이 영역(packed)이므로 race 방지 필요.
 */
static void
_owner_set_description(uint16_t owner_id, const char *description, bool append)
{
	struct spdk_trace_owner *owner;
	char old[256] = {};                   /* [한국어] 기존 description 백업용 stack buffer. zero-init. */

	assert(sizeof(old) >= g_trace_file->owner_description_size);
	/* [한국어] 백업 buffer가 description 영역보다 작지 않음을 보장 (정적 검증). */
	owner = spdk_get_trace_owner(g_trace_file, owner_id);
	assert(owner != NULL);
	/* [한국어] 호출자가 owner_id 유효성을 책임짐 — NULL이면 프로그램 버그. */
	if (append) {
		memcpy(old, owner->description, g_trace_file->owner_description_size);
		/* [한국어] 기존 문자열을 통째로 백업. snprintf가 자기 자신을 source로 쓰면 UB. */
	}

	snprintf(owner->description, g_trace_file->owner_description_size,
		 "%s%s%s", old, append ? " " : "", description);
	/* [한국어] 결합 출력:
	 * - append=true : "<old> <description>"
	 * - append=false: old=""(빈 문자열) + ""(no separator) + "<description>" → 덮어쓰기.
	 * description_size를 넘는 부분은 snprintf가 자동 truncate. */
}

/*
 * [한국어]
 * spdk_trace_register_owner - owner 인스턴스 동적 할당.
 *
 * @owner_type:  미리 register_owner_type으로 등록된 분류값.
 * @description: 사용자에게 보일 설명 문자열.
 * @return: 새로 할당된 owner_id (1 이상). 0 = 할당 실패 또는 trace 비활성.
 *
 * 동작 (lock 보호):
 *   1) ring에서 head 위치의 ID pop → owner_id 변수에 저장.
 *   2) head++ (size에 도달하면 wrap to 0).
 *   3) 해당 owner_id의 spdk_trace_owner 구조체에 tsc/type/description 채움.
 *
 * 컨텍스트: 임의 SPDK 스레드에서 호출 가능 — spinlock으로 보호.
 *           subsystem이 객체(예: NVMe qpair) 생성 시 호출.
 */
uint16_t
spdk_trace_register_owner(uint8_t owner_type, const char *description)
{
	struct spdk_trace_owner *owner;
	uint32_t owner_id;

	if (g_owner_ids.ring == NULL) {
		/* Help the unit test environment by simply returning instead
		 * of requiring it to initialize the trace library.
		 */
		/* [한국어] 단위 테스트가 trace를 init하지 않아도 호출 측이 깨지지 않도록 0 반환.
		 * owner_id=0은 "no owner" 의미라 안전한 sentinel. */
		return 0;
	}

	pthread_spin_lock(&g_owner_ids.lock);

	if (g_owner_ids.head == g_owner_ids.tail) {
		/* No owner ids available. Return 0 which means no owner. */
		/* [한국어] ring 비어 있음 → 모든 owner 슬롯이 사용 중. 0 반환. */
		pthread_spin_unlock(&g_owner_ids.lock);
		return 0;
	}

	owner_id = g_owner_ids.ring[g_owner_ids.head];
	/* [한국어] head 위치의 가용 ID pop. */
	if (++g_owner_ids.head == g_owner_ids.size) {
		g_owner_ids.head = 0;
		/* [한국어] ring wrap — size 도달 시 0으로 회귀. */
	}

	owner = spdk_get_trace_owner(g_trace_file, owner_id);
	owner->tsc = spdk_get_ticks();
	/* [한국어] 등록 시점 TSC 기록 — parser가 lifecycle 시작점으로 사용. */
	owner->type = owner_type;
	_owner_set_description(owner_id, description, false);
	/* [한국어] 새 description 설정 (lock 잡고 있는 상태에서 호출 — 안전). */
	pthread_spin_unlock(&g_owner_ids.lock);
	return owner_id;
}

/*
 * [한국어]
 * spdk_trace_unregister_owner - owner 인스턴스 해제 (slot을 ring에 반환).
 *
 * @owner_id: 해제할 ID. 0이면 nop (호출자 편의 — 추가 검사 없이 안전 호출 가능).
 *
 * 동작 (lock 보호): ring[tail] = owner_id → tail++ (wrap).
 * 컨텍스트: 객체 소멸 시 호출 (예: NVMe qpair 해제).
 */
void
spdk_trace_unregister_owner(uint16_t owner_id)
{
	if (g_owner_ids.ring == NULL) {
		/* Help the unit test environment by simply returning instead
		 * of requiring it to initialize the trace library.
		 */
		return;                       /* [한국어] 테스트 환경 — silent skip. */
	}

	if (owner_id == 0) {
		/* owner_id 0 means no owner. Allow this to be passed here, it
		 * avoids caller having to do extra checking.
		 */
		return;                       /* [한국어] 0 = "no owner"는 해제할 게 없음. */
	}

	pthread_spin_lock(&g_owner_ids.lock);
	g_owner_ids.ring[g_owner_ids.tail] = owner_id;
	/* [한국어] tail 위치에 ID push (재사용 풀로 반환). */
	if (++g_owner_ids.tail == g_owner_ids.size) {
		g_owner_ids.tail = 0;         /* [한국어] wrap. */
	}
	pthread_spin_unlock(&g_owner_ids.lock);
}

/*
 * [한국어]
 * spdk_trace_owner_set_description - 기존 owner의 description 덮어쓰기.
 *
 * @owner_id:    대상 owner.
 * @description: 새 문자열.
 *
 * 컨텍스트: subsystem이 owner의 컨텍스트 변경 시 호출 (예: qpair 재할당).
 *           내부에서 spinlock으로 보호.
 */
void
spdk_trace_owner_set_description(uint16_t owner_id, const char *description)
{
	if (g_owner_ids.ring == NULL) {
		/* Help the unit test environment by simply returning instead
		 * of requiring it to initialize the trace library.
		 */
		return;                       /* [한국어] 단위 테스트 안전 호출 보장. */
	}

	pthread_spin_lock(&g_owner_ids.lock);
	_owner_set_description(owner_id, description, false);
	/* [한국어] append=false → 덮어쓰기 모드. */
	pthread_spin_unlock(&g_owner_ids.lock);
}

/*
 * [한국어]
 * spdk_trace_owner_append_description - 기존 description 뒤에 공백+새 문자열 추가.
 *
 * @owner_id: 0이면 nop (안전).
 * @description: 추가할 문자열.
 *
 * 사용처: 한 owner에 여러 서브시스템이 자기 정보를 누적 표시 (예: nvmf transport가
 *         qpair 등록 후 추가 정보를 append). spinlock으로 보호.
 */
void
spdk_trace_owner_append_description(uint16_t owner_id, const char *description)
{
	if (g_owner_ids.ring == NULL) {
		/* Help the unit test environment by simply returning instead
		 * of requiring it to initialize the trace library.
		 */
		return;
	}

	if (owner_id == 0) {
		/* owner_id 0 means no owner. Allow this to be passed here, it
		 * avoids caller having to do extra checking.
		 */
		return;                       /* [한국어] no-owner — append할 대상 없음. */
	}

	pthread_spin_lock(&g_owner_ids.lock);
	_owner_set_description(owner_id, description, true);
	/* [한국어] append=true → " " + new를 기존 뒤에 추가. */
	pthread_spin_unlock(&g_owner_ids.lock);
}

/*
 * [한국어]
 * spdk_trace_register_object - object 분류 메타 등록 (owner_type과 대칭).
 *
 * @type:      0~255 (OBJECT_NONE=0 예약).
 * @id_prefix: parser 출력 prefix (예: 'i' for bdev_io).
 *
 * g_trace_file->object[type]에 저장. tpoint 정의의 object_type 필드와 매칭.
 * 컨텍스트: reg_fn() 내부 — 메인 스레드 단일.
 */
void
spdk_trace_register_object(uint8_t type, char id_prefix)
{
	struct spdk_trace_object *object;

	assert(type != OBJECT_NONE);
	/* [한국어] 0은 OBJECT_NONE 예약 — 등록 금지. */

	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	/* 'object' has 256 entries and since 'type' is a uint8_t, it
	 * can't overrun the array.
	 */
	object = &g_trace_file->object[type];
	/* [한국어] type을 인덱스로 직접 사용 — uint8_t 보장. */
	assert(object->type == 0);
	/* [한국어] 중복 등록 방지 — zero clear 상태에서 type=0이어야 함. */

	object->type = type;
	object->id_prefix = id_prefix;
	/* [한국어] 메타 채움. */
}

/*
 * [한국어]
 * trace_register_description - 단일 tpoint 메타를 g_trace_file->tpoint[id]에 채움.
 *
 * @opts: tpoint 정의 옵션 (이름/ID/owner_type/object_type/new_object/args[]).
 *
 * 동작:
 *   1) tpoint_id 범위 검증 (assert).
 *   2) name 길이 초과 시 경고 (잘림은 snprintf가 처리).
 *   3) tpoint 슬롯 zero check (중복 등록 방지).
 *   4) name/owner_type/object_type/new_object 채움.
 *   5) args[] 순회: opts.args[i].name이 NULL/빈 문자열이면 종료.
 *      각 인자의 type별 size 검증 (INT/PTR=4 or 8, STR>0).
 *   6) num_args = 실제로 채운 슬롯 수.
 * 컨텍스트: 호출자(spdk_trace_register_description_ext)가 g_trace_file 초기화를 확인했음.
 */
static void
trace_register_description(const struct spdk_trace_tpoint_opts *opts)
{
	struct spdk_trace_tpoint *tpoint;
	size_t i, max_name_length;

	assert(opts->tpoint_id < SPDK_TRACE_MAX_TPOINT_ID);
	/* [한국어] 1280 미만 보장 — group*64 + idx 한계. */

	if (strnlen(opts->name, sizeof(tpoint->name)) == sizeof(tpoint->name)) {
		/* [한국어] name buffer가 sizeof(tpoint->name)=24 이내에 NUL을 포함하지 못하면 잘림.
		 * snprintf가 이후 단계에서 자동 truncate하지만 사용자에게 알림. */
		SPDK_ERRLOG("name (%s) too long\n", opts->name);
	}

	tpoint = &g_trace_file->tpoint[opts->tpoint_id];
	/* [한국어] 직접 indexing — tpoint_id를 키로 사용. */
	assert(tpoint->tpoint_id == 0);
	/* [한국어] zero clear 상태에서 0이어야 함 (중복 등록 방지). */

	snprintf(tpoint->name, sizeof(tpoint->name), "%s", opts->name);
	/* [한국어] name 복사 — 길이 초과 시 자동 truncate. */
	tpoint->tpoint_id = opts->tpoint_id;
	tpoint->object_type = opts->object_type;
	tpoint->owner_type = opts->owner_type;
	tpoint->new_object = opts->new_object;
	/* [한국어] 메타 필드 채움 — parser가 entry 해석에 사용. */

	max_name_length = sizeof(tpoint->args[0].name);
	/* [한국어] 인자 이름 buffer 크기 (보통 14B). */
	for (i = 0; i < SPDK_TRACE_MAX_ARGS_COUNT; ++i) {
		if (!opts->args[i].name || opts->args[i].name[0] == '\0') {
			/* [한국어] 인자 정의 종료 — 빈 슬롯이면 break. */
			break;
		}

		switch (opts->args[i].type) {
		case SPDK_TRACE_ARG_TYPE_INT:
		case SPDK_TRACE_ARG_TYPE_PTR:
			/* The integers and pointers have to be exactly 4 or 8 bytes */
			/* [한국어] _spdk_trace_record의 va_arg 분기가 4/8B만 지원. */
			assert(opts->args[i].size == 4 || opts->args[i].size == 8);
			break;
		case SPDK_TRACE_ARG_TYPE_STR:
			/* Strings need to have at least one byte for the NULL terminator */
			/* [한국어] STR은 최소 1B (NUL 보장). 정의 size가 0이면 무의미. */
			assert(opts->args[i].size > 0);
			break;
		default:
			/* [한국어] 정의되지 않은 type — 등록 거부. */
			assert(0 && "invalid trace argument type");
			break;
		}

		if (strnlen(opts->args[i].name, max_name_length) == max_name_length) {
			/* [한국어] 인자 이름이 max_name_length 안에 NUL을 두지 못함 — 잘림 경고. */
			SPDK_ERRLOG("argument name (%s) is too long\n", opts->args[i].name);
		}

		snprintf(tpoint->args[i].name, sizeof(tpoint->args[i].name),
			 "%s", opts->args[i].name);
		/* [한국어] 인자 이름 복사 (자동 truncate). */
		tpoint->args[i].type = opts->args[i].type;
		tpoint->args[i].size = opts->args[i].size;
		/* [한국어] type/size 메타 — _spdk_trace_record가 unpacking에 참조. */
	}

	tpoint->num_args = i;
	/* [한국어] 실제로 채운 인자 수 — record 시 호출자 num_args와 비교 검증에 사용. */
}

/*
 * [한국어]
 * spdk_trace_register_description_ext - 여러 tpoint를 한 번에 등록 (다중 인자 지원).
 *
 * @opts:     spdk_trace_tpoint_opts 배열.
 * @num_opts: 배열 길이.
 *
 * 보통 reg_fn() 내에서 모듈의 모든 tpoint 메타를 일괄 등록할 때 사용.
 * 컨텍스트: trace_flags_init이 호출하는 reg_fn() 내부 (메인 스레드 단일).
 */
void
spdk_trace_register_description_ext(const struct spdk_trace_tpoint_opts *opts, size_t num_opts)
{
	size_t i;

	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;                       /* [한국어] init 안됨 — 등록 불가. */
	}

	for (i = 0; i < num_opts; ++i) {
		trace_register_description(&opts[i]);
		/* [한국어] 각 entry를 단일 헬퍼로 등록. */
	}
}

/*
 * [한국어]
 * spdk_trace_register_description - 단일 인자 tpoint의 간편 등록 wrapper.
 *
 * @name/@tpoint_id/@owner_type/@object_type/@new_object: tpoint 메타.
 * @arg1_type:  SPDK_TRACE_ARG_TYPE_INT/PTR/STR.
 * @arg1_name:  인자 이름.
 *
 * 동작: opts 구조체 stack 생성 후 register_description_ext(_, 1) 호출.
 *       arg1.size는 sizeof(uint64_t)로 강제 — 단일 인자 엔진 호환.
 */
void
spdk_trace_register_description(const char *name, uint16_t tpoint_id, uint8_t owner_type,
				uint8_t object_type, uint8_t new_object,
				uint8_t arg1_type, const char *arg1_name)
{
	struct spdk_trace_tpoint_opts opts = {
		.name = name,
		.tpoint_id = tpoint_id,
		.owner_type = owner_type,
		.object_type = object_type,
		.new_object = new_object,
		.args = {{
				.name = arg1_name,
				.type = arg1_type,
				.size = sizeof(uint64_t)
				/* [한국어] 단일 인자 wrapper는 항상 8B 가정 — INT/PTR이면 64-bit, STR이면 최대 8B 길이. */
			}
		}
	};

	spdk_trace_register_description_ext(&opts, 1);
	/* [한국어] _ext 버전에 단일 entry 위임. */
}

/*
 * [한국어]
 * spdk_trace_tpoint_register_relation - tpoint↔object 사이 cross-reference 등록.
 *
 * @tpoint_id:   기준 tpoint.
 * @object_type: 연관시킬 object 분류.
 * @arg_index:   해당 object의 ID를 담고 있는 args[] 인덱스 (0~7).
 *
 * 사용처: parser가 동일 object_id를 가진 entry들을 서로 다른 tpoint 사이에 묶어
 *         lifecycle 시각화. 예: NVMe submit/complete가 같은 bdev_io 객체로 표시.
 *
 * 동작:
 *   1) tpoint_id가 OBJECT_NONE이 아닌지 검증 (이름 reuse 보호).
 *   2) tpoint->related_objects[] (16 슬롯)에서 첫 번째 빈 슬롯에 object_type+arg_index 저장.
 *   3) 16개 모두 차 있으면 ERRLOG로 실패 보고.
 *
 * 주의: tpoint가 미리 등록되었는지는 검증하지 않음 — 등록 순서 무관.
 *       나중에 register_description으로 채워질 tpoint와 미리 relation을 만들 수 있음.
 */
void
spdk_trace_tpoint_register_relation(uint16_t tpoint_id, uint8_t object_type, uint8_t arg_index)
{
	struct spdk_trace_tpoint *tpoint;
	uint16_t i;

	assert(object_type != OBJECT_NONE);
	/* [한국어] OBJECT_NONE(0)은 "없음" 의미 — relation 등록 무의미. */
	assert(tpoint_id != OBJECT_NONE);
	/* [한국어] tpoint_id가 0이면 "no tpoint" — 의미 없는 relation. */

	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	/* We do not check whether a tpoint_id exists here, because
	 * there is no order in which trace definitions are registered.
	 * This way we can create relations between tpoint and objects
	 * that will be declared later. */
	tpoint = &g_trace_file->tpoint[tpoint_id];
	for (i = 0; i < SPDK_COUNTOF(tpoint->related_objects); ++i) {
		/* [한국어] 16 슬롯 순회 — 빈 슬롯(OBJECT_NONE) 발견하면 등록. */
		if (tpoint->related_objects[i].object_type == OBJECT_NONE) {
			tpoint->related_objects[i].object_type = object_type;
			tpoint->related_objects[i].arg_index = arg_index;
			return;
		}
	}
	SPDK_ERRLOG("Unable to register new relation for tpoint %" PRIu16 ", object %" PRIu8 "\n",
		    tpoint_id, object_type);
	/* [한국어] 모든 슬롯이 차 있음 — 한 tpoint당 최대 16개 relation 한계. */
}

/*
 * [한국어]
 * spdk_trace_add_register_fn - reg_fn 노드를 글로벌 정렬 리스트에 삽입.
 *
 * @reg_fn: 등록할 노드 (수명: 프로세스 전체, 보통 .data 섹션).
 *
 * 보통 SPDK_TRACE_REGISTER_FN 매크로의 constructor가 자동 호출.
 * 사용자가 직접 부를 일은 거의 없음.
 *
 * 동작:
 *   1) name NULL/빈 검증.
 *   2) "all"은 예약 키워드 — 등록 거부.
 *   3) 기존 노드와 tgroup_id 또는 name 중복 검사 → 충돌 시 거부.
 *   4) tgroup_id 오름차순으로 linked list 정렬 삽입:
 *      a) head가 NULL이거나 새 노드가 head보다 작으면 head 앞에 prepend.
 *      b) 아니면 _reg_fn->next->tgroup_id가 새 노드보다 큰 위치 찾기.
 *
 * 컨텍스트: __attribute__((constructor)) — main 진입 전, 단일 스레드에서 실행.
 *           동기화 불필요.
 */
void
spdk_trace_add_register_fn(struct spdk_trace_register_fn *reg_fn)
{
	struct spdk_trace_register_fn *_reg_fn;       /* [한국어] 리스트 순회 커서. */

	if (reg_fn->name == NULL) {
		SPDK_ERRLOG("missing name for registering spdk trace tpoint group\n");
		assert(false);
		return;                       /* [한국어] 이름 없는 reg_fn은 잘못된 매크로 사용. */
	}

	if (strcmp(reg_fn->name, "all") == 0) {
		SPDK_ERRLOG("illegal name (%s) for tpoint group\n", reg_fn->name);
		/* [한국어] "all"은 create_tpoint_group_mask가 특수 처리하는 예약 키워드 → 사용 금지. */
		assert(false);
		return;
	}

	/* Ensure that no trace point group IDs and names are ever duplicated */
	for (_reg_fn = g_reg_fn_head; _reg_fn; _reg_fn = _reg_fn->next) {
		if (reg_fn->tgroup_id == _reg_fn->tgroup_id) {
			/* [한국어] tgroup_id 충돌 — 두 group이 같은 비트를 사용하게 되어 mask 동작 망가짐. */
			SPDK_ERRLOG("group %d, %s has duplicate tgroup_id with %s\n",
				    reg_fn->tgroup_id, reg_fn->name, _reg_fn->name);
			assert(false);
			return;
		}

		if (strcmp(reg_fn->name, _reg_fn->name) == 0) {
			/* [한국어] 이름 중복 — create_tpoint_group_mask 매칭이 모호해짐. */
			SPDK_ERRLOG("name %s is duplicated between groups with ids %d and %d\n",
				    reg_fn->name, reg_fn->tgroup_id, _reg_fn->tgroup_id);
			assert(false);
			return;
		}
	}

	/* Arrange trace registration in order on tgroup_id */
	if (g_reg_fn_head == NULL || reg_fn->tgroup_id < g_reg_fn_head->tgroup_id) {
		/* [한국어] 빈 리스트 또는 새 노드가 head보다 작은 ID — head 앞에 prepend. */
		reg_fn->next = g_reg_fn_head;
		g_reg_fn_head = reg_fn;
		return;
	}

	for (_reg_fn = g_reg_fn_head; _reg_fn; _reg_fn = _reg_fn->next) {
		/* [한국어] 정렬 위치 검색 — _reg_fn 다음에 새 노드를 끼워야 하는 지점.
		 * 조건:
		 *   - 마지막 노드(다음 NULL)이거나
		 *   - 다음 노드의 tgroup_id가 새 노드보다 큼
		 * 둘 중 하나면 _reg_fn과 _reg_fn->next 사이에 삽입. */
		if (_reg_fn->next == NULL || reg_fn->tgroup_id < _reg_fn->next->tgroup_id) {
			reg_fn->next = _reg_fn->next;
			_reg_fn->next = reg_fn;
			return;
		}
	}
}

/*
 * [한국어]
 * trace_flags_init - flag 서브시스템 부트스트랩 — reg_fn 일괄 호출 + owner ID ring 초기화.
 *
 * @return: 0 성공 / -ENOMEM (ring calloc 실패) / pthread_spin_init 에러 코드.
 *
 * 동작:
 *   1) g_reg_fn_head 리스트 순회 — 각 모듈의 reg_fn() 호출.
 *      각 reg_fn은 spdk_trace_register_owner_type/object/description_ext 호출하여
 *      g_trace_file에 메타데이터 채움.
 *   2) owner ID ring buffer 초기화:
 *      - owner_id 0은 "no owner" 예약.
 *      - owner_id 1~255는 레거시 poller_id 충돌 회피로 skip.
 *      - 256 ~ num_owners-1 까지 가용 ID로 채움 (총 num_owners-256개).
 *      - head=0, tail=num_owners-256 (16K-256=15872개 가용).
 *   3) pthread_spin_init — ring 보호용 spinlock 생성.
 *      실패 시 ring free 후 에러 반환.
 *
 * 컨텍스트: spdk_trace_init 후반부 단일 호출, 메인 스레드.
 * 호출 체인: spdk_app_start → spdk_trace_init → trace_flags_init.
 */
int
trace_flags_init(void)
{
	struct spdk_trace_register_fn *reg_fn;
	uint16_t i;
	uint16_t owner_id_start;
	int rc;

	reg_fn = g_reg_fn_head;
	while (reg_fn) {
		reg_fn->reg_fn();
		/* [한국어] 모듈별 reg_fn 호출 — 내부에서 register_owner_type/object/description_ext 호출.
		 * 이 시점에서 g_trace_file의 메타데이터 영역이 채워짐. */
		reg_fn = reg_fn->next;
	}

	/* We will not use owner_id 0, it will be reserved to mean "no owner".
	 * But for now, we will start with owner_id 256 instead of owner_id 1.
	 * This will account for some libraries and modules which pass a
	 * "poller_id" to spdk_trace_record() which is now an owner_id. Until
	 * all of those libraries and modules are converted, we will start
	 * owner_ids at 256 to avoid collisions.
	 */
	owner_id_start = 256;
	/* [한국어] 1~255는 레거시 poller_id가 owner_id 슬롯에 직접 들어가던 시절의 잔재 회피.
	 * 모든 호출자가 register_owner를 사용하도록 마이그레이션 완료 전까지 256부터 시작. */
	g_owner_ids.ring = calloc(g_trace_file->num_owners, sizeof(uint16_t));
	/* [한국어] num_owners (=16K) * sizeof(uint16_t) = 32KB ring 할당 (zero-init). */
	if (g_owner_ids.ring == NULL) {
		SPDK_ERRLOG("could not allocate g_owner_ids.ring\n");
		return -ENOMEM;
	}
	g_owner_ids.head = 0;
	/* [한국어] head=0부터 pop 시작. */
	g_owner_ids.tail = g_trace_file->num_owners - owner_id_start;
	/* [한국어] tail은 ring에 push된 마지막 ID 다음 위치.
	 * num_owners(=16384) - 256 = 16128개의 가용 ID가 ring[0..16127]에 배치. */
	g_owner_ids.size = g_trace_file->num_owners;
	/* [한국어] ring 배열 크기 (wrap-around 모듈로). */
	for (i = 0; i < g_owner_ids.tail; i++) {
		g_owner_ids.ring[i] = i + owner_id_start;
		/* [한국어] ring[0]=256, ring[1]=257, ..., ring[16127]=16383.
		 * 첫 register_owner 호출이 ring[0]=256을 받음. */
	}

	rc = pthread_spin_init(&g_owner_ids.lock, PTHREAD_PROCESS_PRIVATE);
	/* [한국어] PTHREAD_PROCESS_PRIVATE — 같은 프로세스 내 스레드들 사이 사용.
	 * SPDK는 단일 프로세스에서 다중 reactor를 운영하므로 적합. */
	if (rc != 0) {
		/* [한국어] spinlock 생성 실패 — ring 해제 후 에러 반환. */
		free(g_owner_ids.ring);
		g_owner_ids.ring = NULL;
	}

	return rc;
}

/*
 * [한국어]
 * trace_flags_fini - flag 서브시스템 종료 — ring/lock 해제.
 *
 * 동작:
 *   1) ring이 NULL이면 init되지 않은 상태 — 즉시 return (멱등성).
 *   2) spinlock destroy → ring free → ring=NULL (이중 해제 방지).
 *
 * 컨텍스트: spdk_trace_cleanup의 첫 단계 (mmap 해제 전).
 *           모든 SPDK reactor가 종료된 후에 호출된다고 가정 — destroy가 안전.
 */
void
trace_flags_fini(void)
{
	if (g_owner_ids.ring == NULL) {
		return;                       /* [한국어] init 안됨 또는 이미 fini 됨 — no-op. */
	}
	pthread_spin_destroy(&g_owner_ids.lock);
	/* [한국어] spinlock 자원 해제 — destroy 시점에 lock이 잡혀 있으면 UB이므로
	 * 호출자(spdk_trace_cleanup)가 모든 사용자가 종료된 후 호출해야 함. */
	free(g_owner_ids.ring);
	g_owner_ids.ring = NULL;
	/* [한국어] sentinel 복원 — 다른 owner API들이 이 NULL을 보고 silent-skip. */
}
