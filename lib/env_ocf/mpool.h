/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] OCF 환경(env) — 다단계 mpool 공개 헤더 (mpool.h)
 *
 * === 파일의 역할 ===
 * OCF 코어가 가변 크기(=고정 헤더 + N개 element) 객체를 빠르게 할당/해제할 수 있도록 SPDK 측에서
 * 제공하는 다단계 메모리 풀의 공개 인터페이스를 선언한다. 8단계(2^0..2^7 = 1..128 element)의
 * 사이즈 풀을 미리 만들어 두고, 요청한 element 개수를 가장 작은 2^k(>=count)로 라운드업해 해당
 * 단계 풀에서 슬롯을 빌려준다. 구현은 mpool.c에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   libocf 코어 (ocf_request 등 가변 크기 객체)
 *     → env_mpool_new(count) → 적절한 단계 풀에서 슬롯 1개
 *       → 해제 시 env_mpool_del(items, count)
 * env_mpool_max=8 단계는 OCF의 일반적인 sub-cline group 크기(128)에 맞춰져 있다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: 기본 정수 타입(size_t, uint32_t, bool — 보통 사용자가 stdinc 통해 미리 포함).
 * - 의존받음: lib/env_ocf/mpool.c(구현), libocf 코어의 ENV_MPOOL_* 매크로.
 * - 데이터 흐름: 부팅 시 create로 고정 — 런타임에 new/del로 빌리고 반환.
 * - 공유 자료구조: struct env_mpool — 정의는 mpool.c 내부, 외부엔 전방 선언만 노출.
 *
 * === 주요 함수/구조체 요약 ===
 * - enum env_mpool_*    : 0..7 단계 인덱스 + env_mpool_max 보초값.
 * - struct env_mpool    : 다단계 mpool 컨테이너(정의는 .c 내부 — opaque).
 * - env_mpool_create    : 단계별 풀 일괄 생성.
 * - env_mpool_destroy   : 단계별 풀 일괄 해제.
 * - env_mpool_new/del   : 슬롯 빌림/반환 (count로 단계 자동 선택).
 */

#ifndef OCF_MPOOL_H
#define OCF_MPOOL_H
/* [한국어] 다중 포함 가드 — 같은 TU에서 여러 번 include되어도 안전. */

enum {
	/* [한국어] mpool 단계 인덱스 — 각 단계는 (1<<i)개의 element를 담는 슬롯 풀에 대응.
	 * 이 enum은 env_mpool::allocator[] 배열의 인덱스로 직접 사용되므로 순서/값 변경 불가. */

	env_mpool_1,
	/* [한국어] 단계 0 — 1 element 슬롯 풀. count==1 또는 count==0(예외) 요청에 사용. */

	env_mpool_2,
	/* [한국어] 단계 1 — 2 element 슬롯 풀. count==2 요청에 사용. */

	env_mpool_4,
	/* [한국어] 단계 2 — 4 element 슬롯 풀. count==3,4 요청이 라운드업되어 사용. */

	env_mpool_8,
	/* [한국어] 단계 3 — 8 element 슬롯 풀. count 5..8 요청. */

	env_mpool_16,
	/* [한국어] 단계 4 — 16 element 슬롯 풀. count 9..16 요청. */

	env_mpool_32,
	/* [한국어] 단계 5 — 32 element 슬롯 풀. count 17..32 요청. */

	env_mpool_64,
	/* [한국어] 단계 6 — 64 element 슬롯 풀. count 33..64 요청. */

	env_mpool_128,
	/* [한국어] 단계 7 — 128 element 슬롯 풀. count 65..128 요청. */

	env_mpool_max
	/* [한국어] 보초값(=8) — 배열 크기와 루프 상한으로 사용. 실제 풀 단계는 0..env_mpool_max-1. */
};

struct env_mpool;
/* [한국어] 전방 선언만 노출 — 구체 정의는 mpool.c 내부에 있어 호출자는 포인터로만 다룬다(opaque). */

/*
 * [한국어]
 * env_mpool_create - 다단계 환경 mpool을 생성.
 *
 * @hdr_size   : 슬롯 고정 헤더 크기.
 * @elem_size  : element 1개당 추가 크기.
 * @flags      : OCF mpool 플래그(현 구현은 미사용).
 * @mpool_max  : 사용할 최대 단계 인덱스(0..env_mpool_max-1).
 * @fallback   : 풀 미커버 시 vmalloc 허용 여부.
 * @limits     : 단계별 슬롯 수 배열(NULL이면 모두 기본값).
 * @name_perfix: 이름 prefix(통계/디버그) — 단계별로 _N 형태로 합성.
 * @zero       : 새 슬롯 zero-fill 여부.
 * @return     : 핸들 또는 NULL.
 *
 * 호출 체인: OCF 캐시 init → [env_mpool_create] → env_allocator_create_extended → spdk_mempool_create.
 */
struct env_mpool *env_mpool_create(uint32_t hdr_size, uint32_t elem_size,
				   int flags, int mpool_max, bool fallback,
				   const uint32_t limits[env_mpool_max],
				   const char *name_perfix, bool zero);

/*
 * [한국어]
 * env_mpool_destroy - mpool과 그 단계별 풀을 일괄 해제.
 *
 * @mpools: env_mpool_create()가 반환한 핸들(NULL 안전).
 *
 * 호출 체인: OCF 캐시 deinit → [env_mpool_destroy] → env_allocator_destroy → spdk_mempool_free.
 */
void env_mpool_destroy(struct env_mpool *mpools);

/*
 * [한국어]
 * env_mpool_new - count개 element 슬롯을 담을 수 있는 슬롯 1개 반환.
 *
 * @mpool: 대상 mpool.
 * @count: element 개수.
 * @return: 슬롯 포인터 또는 NULL(고갈/범위 초과 + fallback=false).
 *
 * 호출 체인: OCF IO 경로 → [env_mpool_new] → spdk_mempool_get / env_vmalloc.
 */
void *env_mpool_new(struct env_mpool *mpool, uint32_t count);

/*
 * [한국어]
 * env_mpool_del - new로 빌린 슬롯을 반환.
 *
 * @mpool: 대상 mpool.
 * @items: 슬롯 포인터.
 * @count: new에서 사용한 동일 count(다른 값 사용 시 손상 위험).
 * @return: true=정상, false=풀 미커버 + fallback=false(누수 — 비정상 호출 진단용).
 *
 * 호출 체인: OCF IO 완료 → [env_mpool_del] → spdk_mempool_put / env_vfree.
 */
bool env_mpool_del(struct env_mpool *mpool,
		   void *items, uint32_t count);

#endif
/* [한국어] 다중 포함 가드 종결. */
