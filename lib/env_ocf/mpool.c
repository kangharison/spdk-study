/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] OCF 환경(env) — 다단계 메모리 풀 구현 (mpool.c)
 *
 * === 파일의 역할 ===
 * OCF(Open CAS Framework: Intel CAS의 캐시 알고리즘 라이브러리)는 환경 의존부(메모리/스레드/락)를
 * "env" 추상화로 숨기고 호스트가 그 구현을 제공한다. SPDK는 lib/env_ocf 아래에 SPDK 환경(DPDK
 * mempool/hugepage 기반)에 맞게 OCF env를 구현했다. 본 파일은 그중 "환경 mpool" — 즉, 한 가지
 * 구조체 종류이지만 길이가 가변적(예: ocf_request에 매달리는 lba 배열, page table 등)인 객체를
 * 효율적으로 할당하기 위해 "고정 헤더 + (1, 2, 4, 8, …, 128) * elem_size" 의 8단계 사이즈 풀을
 * 미리 만들어 두는 다단계 풀을 제공한다. 요청한 element 개수를 "그 이상의 가장 작은 2^k"로
 * 라운드업해 해당 풀에서 슬롯을 빌려준다. 부족 시 fallback로 vmalloc 가능.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   OCF 코어(libocf) → env_mpool_new(count) ──→
 *     env_mpool_get_allocator(count로 풀 인덱스 산출) ──→
 *       env_allocator_new(allocator) ──→ spdk_mempool_get (DPDK mempool)
 *         → 실패 시: fallback ? env_vmalloc : NULL
 * env_mpool_create는 SPDK 부팅 단계에서 OCF 캐시 인스턴스 생성 시 호출된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/env(spdk_mempool 등 — 본 파일은 직접 호출은 없고 ocf_env 경유), ocf_env(env_allocator).
 * - 의존받음: libocf 코어가 ENV_MPOOL_*를 통해 사용. SPDK bdev OCF 모듈(module/bdev/ocf)이 간접 사용.
 * - 데이터 흐름: 부팅 시 create로 8개의 spdk_mempool 묶음을 미리 생성 → 런타임에 new/del로 빌리고 반환.
 * - 공유 자료구조: struct env_mpool — env_allocator* 배열을 가짐. 그 자체는 캐시 인스턴스마다 생성.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct env_mpool          : 다단계 풀 컨테이너 (allocator[0..7], 헤더/원소 크기, fallback 플래그).
 * - env_mpool_create          : 단계별 spdk_mempool 8개를 미리 만들어 배열에 채움.
 * - env_mpool_destroy         : 단계별 mempool 해제.
 * - env_mpool_get_allocator   : count → 적절한 풀 인덱스(=ceil_log2(count)) 산출.
 * - env_mpool_new / env_mpool_del : 빌리고 반환하는 API. 풀 범위 초과 시 vmalloc fallback.
 */

#include "spdk/env.h"
/* [한국어] SPDK 환경(DPDK 기반) — min(), spdk_unlikely 등 매크로/타입 가시화. */

#include "ocf_env.h"
/* [한국어] OCF env 추상화의 SPDK 구현 — env_allocator, env_zalloc, ENV_MEM_NOIO 등. */

#include "mpool.h"
/* [한국어] 본 파일이 구현하는 공개 API 헤더 — env_mpool 전방 선언과 enum env_mpool_max. */

struct env_mpool {
	/* [한국어] 다단계 환경 mpool 컨테이너. 부팅 시 한 번 생성되어 한 캐시 인스턴스의 라이프타임 동안 유지된다.
	 * 각 단계 인덱스 i는 (1<<i) 개의 element를 담는 슬롯에 해당 — 즉 i=0:1개, i=1:2개, …, i=7:128개. */

	env_allocator *allocator[env_mpool_max];
	/* [한국어] 단계별 spdk_mempool 핸들 배열 — 인덱스 i는 (1<<i)개 element 슬롯의 풀.
	 * 설정자: env_mpool_create()에서 단계별로 env_allocator_create_extended() 결과 저장.
	 * 읽는 자: env_mpool_get_allocator()가 count로부터 인덱스 산출 후 반환.
	 * 값 범위: 정상 시 모두 non-NULL, 에러 경로에서 일부만 채워질 수 있음(부분 cleanup 필요).
	 * 동기화: spdk_mempool 내부가 lock-free ring 기반(per-core cache 포함)이어서 동시 접근 안전. */
	/* Handles to memory pools */

	uint32_t hdr_size;
	/* [한국어] 객체의 고정 헤더 크기 — 모든 단계에 공통으로 들어가는 부분(예: 메타데이터 구조).
	 * 설정자: create() 인자로 받음, 이후 immutable.
	 * 읽는 자: new()에서 size = hdr_size + elem_size*count 계산 시 사용.
	 * 값 범위: 일반적으로 sizeof(meta) 정도, 0 가능.
	 * 동기화: immutable이므로 race 없음. */
	/* Data header size (constant allocation part) */

	uint32_t elem_size;
	/* [한국어] element 1개당 추가 크기(가변 부분의 단위 — 예: 1 LBA 엔트리 크기).
	 * 설정자: create() 인자로 immutable.
	 * 읽는 자: new()의 fallback 경로에서 vmalloc 크기 산출에 사용.
	 * 값 범위: 0보다 큰 값, 캐시 인스턴스 정책에 의존.
	 * 동기화: immutable. */
	/* Per element size increment (variable allocation part) */

	uint32_t mpool_max;
	/* [한국어] 이 mpool 인스턴스가 실제로 사용하는 최대 단계 인덱스 — 0..env_mpool_max-1 범위.
	 * 설정자: create() 인자로 받음 — 호출자가 "최대 (1<<mpool_max) 개 element 대응"으로 사용 시.
	 * 읽는 자: get_allocator()가 idx > mpool_max면 NULL 반환(상위 단계 미사용).
	 * 값 범위: 0..(env_mpool_max-1) (=0..7).
	 * 동기화: immutable. */
	/* Max mpool allocation order */

	bool fallback;
	/* [한국어] 풀 범위를 넘는(또는 단계 풀이 비어 있는) 요청에 대한 vmalloc fallback 허용 여부.
	 * 설정자: create() 인자로 받음.
	 * 읽는 자: new/del()이 풀에서 못 받으면 vmalloc/vfree로 처리할지 결정.
	 * 값 범위: true(허용)/false(미허용 → 실패).
	 * 동기화: immutable. */
	/* Fallback to vmalloc */
};

/*
 * [한국어]
 * env_mpool_create - 다단계 환경 mpool을 생성한다.
 *
 * @hdr_size   : 객체의 고정 헤더 크기.
 * @elem_size  : element 1개당 추가 크기.
 * @flags      : (현재 구현에서는 사용하지 않음 — API 호환을 위해 유지) OCF가 의도한 mpool 플래그.
 * @mpool_max  : 사용할 최대 단계 인덱스 (0..env_mpool_max-1).
 * @fallback   : 풀 미히트 시 vmalloc fallback 허용 여부.
 * @limits     : 단계별 최대 슬롯 개수 배열(NULL이면 모두 ENV_ALLOCATOR_NBUFS 기본값).
 * @name_prefix: 풀 이름 prefix(통계/디버그용) — 단계 인덱스가 _N 형태로 붙어 유일성 확보.
 * @zero       : 새 슬롯을 zero-fill해서 반환할지 여부.
 * @return     : 성공 시 struct env_mpool*, 실패 시 NULL.
 *
 * SPDK 부팅 단계의 OCF 캐시 초기화에서 호출된다. 단계별 spdk_mempool을 미리 만들어 두어
 * 런타임 hot path에서 가변 크기 할당을 O(1)에 수행할 수 있게 한다.
 * 실행 컨텍스트: 마스터 thread/SPDK app 시작 시퀀스 — 락 없이 직렬 실행 가정.
 *
 * 호출 체인:
 *   OCF 코어 init → [env_mpool_create] → env_allocator_create_extended → spdk_mempool_create
 */
struct env_mpool *env_mpool_create(uint32_t hdr_size, uint32_t elem_size,
				   int flags, int mpool_max, bool fallback,
				   const uint32_t limits[env_mpool_max],
				   const char *name_prefix, bool zero)
{
	int i;
	/* [한국어] 단계 인덱스 루프 변수 (0..min(env_mpool_max, mpool_max+1)-1). */
	char name[OCF_ALLOCATOR_NAME_MAX] = {};
	/* [한국어] 단계별 풀 이름을 포맷하는 임시 버퍼 — 0 초기화로 NUL 종결 보장. */
	int ret;
	/* [한국어] snprintf 반환값 임시(>=size 면 truncate, <0이면 인코딩 오류). */
	int size;
	/* [한국어] 해당 단계 슬롯의 총 바이트 크기(헤더 + 가변부 (1<<i)개). */

	struct env_mpool *mpool = env_zalloc(sizeof(struct env_mpool), ENV_MEM_NOIO);
	/* [한국어] 컨테이너 자체를 zero-init으로 할당. ENV_MEM_NOIO는 OCF 메모리 플래그 — IO 경로 안에서
	 * 메모리 할당이 IO 재진입을 유발하지 않도록 NOIO 정책을 요청. SPDK 구현에서는 일반 calloc로 매핑. */
	if (!mpool) {
		return NULL;
		/* [한국어] 컨테이너 할당 실패 — 호출자에 NULL로 전달, OCF 측 에러 경로 진입. */
	}

	mpool->hdr_size = hdr_size;
	/* [한국어] 헤더 크기 저장 — 이후 immutable. */
	mpool->elem_size = elem_size;
	/* [한국어] element 단위 크기 저장. */
	mpool->mpool_max = mpool_max;
	/* [한국어] 사용 단계 상한 저장 — get_allocator의 범위 체크에 사용. */
	mpool->fallback = fallback;
	/* [한국어] vmalloc fallback 정책 저장. */

	for (i = 0; i < min(env_mpool_max, mpool_max + 1); i++) {
		/* [한국어] 0..min(env_mpool_max, mpool_max+1)-1 단계 순회.
		 * env_mpool_max(=8)와 호출자 요청 mpool_max+1 중 작은 쪽을 상한으로 잡아 배열 OOB 방지. */
		ret = snprintf(name, sizeof(name), "%s_%u", name_prefix, (1 << i));
		/* [한국어] 풀 이름을 "<prefix>_<2^i>" 로 합성 — 통계/디버그 출력에서 단계 식별 가능.
		 * (1<<i)는 해당 풀 슬롯의 element 수를 직관적으로 표시. */
		if (ret < 0 || ret >= (int)sizeof(name)) {
			/* [한국어] snprintf 인코딩 실패(<0) 또는 truncate(>=size) — 둘 다 안전하게 에러 처리. */
			goto err;
		}

		size = hdr_size + (elem_size * (1 << i));
		/* [한국어] 단계 i의 슬롯 1개 크기 = 고정 헤더 + element_size * 2^i.
		 * 즉 i=0이면 1개 element, i=7이면 128개 element 분의 가변 영역을 헤더 뒤에 둠. */

		mpool->allocator[i] = env_allocator_create_extended(size, name,
				      limits ? limits[i] : -1, zero);
		/* [한국어] 단계별 spdk_mempool 생성 — env_allocator는 spdk_mempool wrapper.
		 * limits[i]가 음수이면 기본 ENV_ALLOCATOR_NBUFS(=16383)개 슬롯, 양수이면 그 값 사용.
		 * zero=true면 슬롯 반환 시 memset 0. */

		if (!mpool->allocator[i]) {
			/* [한국어] 어느 한 단계라도 실패하면 partial cleanup 후 전체 실패로 처리.
			 * (이전 단계까지의 spdk_mempool은 destroy에서 일괄 해제됨.) */
			goto err;
		}
	}

	return mpool;
	/* [한국어] 정상 경로 — 모든 단계 풀 생성 완료. */

err:
	env_mpool_destroy(mpool);
	/* [한국어] 부분 성공/실패 시 일관된 cleanup — destroy가 NULL allocator는 건너뛰므로 안전. */
	return NULL;
}

/*
 * [한국어]
 * env_mpool_destroy - 다단계 mpool과 그 단계별 spdk_mempool들을 해제.
 *
 * @mpool: env_mpool_create()가 반환한 핸들(NULL이어도 안전).
 *
 * SPDK 종료/캐시 destroy 시 호출. 모든 슬롯이 반환된 상태에서 호출되어야 한다(아직 빌려간 슬롯이
 * 있으면 env_allocator_destroy 내부에서 ERRLOG + assert로 디버그 빌드는 abort).
 *
 * 호출 체인:
 *   OCF 코어 deinit → [env_mpool_destroy] → env_allocator_destroy → spdk_mempool_free
 */
void
env_mpool_destroy(struct env_mpool *mpool)
{
	if (mpool) {
		/* [한국어] NULL 안전 — 호출자가 부분 초기화 중 실패한 경우라도 호출 가능. */
		int i;
		/* [한국어] 단계 인덱스 루프 변수. */

		for (i = 0; i < env_mpool_max; i++) {
			/* [한국어] 0..7 모든 단계 순회 — mpool_max를 넘는 단계는 어차피 NULL이므로 안전. */
			if (mpool->allocator[i]) {
				/* [한국어] 부분 생성 실패 경로에서는 NULL 단계가 있을 수 있음 — 건너뜀. */
				env_allocator_destroy(mpool->allocator[i]);
				/* [한국어] 해당 단계 spdk_mempool 해제 — 내부에서 슬롯 누수 검사. */
			}
		}

		env_free(mpool);
		/* [한국어] 컨테이너 자체 해제. */
	}
}

/*
 * [한국어]
 * env_mpool_get_allocator - count로부터 적합한 단계 인덱스(=ceil_log2(count))를 산출해
 *                          그 단계의 env_allocator를 반환.
 *
 * @mpool: 대상 mpool.
 * @count: 요청한 element 개수.
 * @return: 단계 풀의 env_allocator*, 또는 NULL(범위 초과).
 *
 * count → 인덱스 매핑: count=1→0, 2→1, 3→2(4-슬롯), 4→2, 5→3(8-슬롯), …, 128→7.
 * 즉 ceil_log2(count) 단계로 라운드업해 해당 단계의 풀을 사용. count=0은 가장 작은 풀(env_mpool_1).
 *
 * 호출 체인:
 *   env_mpool_new/del → [env_mpool_get_allocator]
 */
static env_allocator *
env_mpool_get_allocator(struct env_mpool *mpool,
			uint32_t count)
{
	unsigned int idx;
	/* [한국어] 산출된 단계 인덱스(0..env_mpool_max-1). */

	if (unlikely(count == 0)) {
		/* [한국어] count=0인 비정상/극단 케이스 — 가장 작은 풀(env_mpool_1, slot 1개)을 반환해 안전 처리. */
		return mpool->allocator[env_mpool_1];
	}

	idx = 31 - __builtin_clz(count);
	/* [한국어] count의 floor_log2 — __builtin_clz(count)는 leading zero 개수, 32비트에서 31-clz가 floor_log2.
	 * 예: count=1→idx=0, count=4→idx=2, count=5→idx=2(잠시), count=128→idx=7. */

	if (__builtin_ffs(count) <= idx) {
		/* [한국어] count가 정확한 2의 거듭제곱이 아니면(=ffs(count)가 idx 이하라면), 더 큰 단계로 올림.
		 * ffs(count)는 1-indexed lowest-set-bit 위치 — 2^k인 경우 ffs=k+1=idx+1이므로 이 분기 미진입. */
		idx++;
	}

	if (idx >= env_mpool_max || idx > mpool->mpool_max) {
		/* [한국어] 산출 인덱스가 전역 상한(env_mpool_max=8) 또는 인스턴스 상한(mpool_max)을 넘으면
		 * 풀 미커버 — fallback 로직(vmalloc)으로 보내기 위해 NULL 반환. */
		return NULL;
	}

	return mpool->allocator[idx];
	/* [한국어] 정상 매핑 — 해당 단계 풀의 allocator 반환. */
}

/*
 * [한국어]
 * env_mpool_new - 다단계 풀에서 element count개를 담을 수 있는 슬롯 1개를 반환.
 *
 * @mpool: 대상 mpool.
 * @count: 요청한 element 개수.
 * @return: 슬롯 포인터(헤더 + 가변부) 또는 NULL(고갈/범위 초과 + fallback 미허용).
 *
 * 핫패스에서 호출되는 함수 — 단계 풀 히트가 일반 케이스이며 spdk_mempool의 per-core cache로
 * lock-free에 가까운 비용으로 슬롯을 가져온다.
 * 실행 컨텍스트: OCF IO 경로 — SPDK thread 컨텍스트(보통 캐시 io_channel thread).
 *
 * 호출 체인:
 *   OCF IO → [env_mpool_new] → env_mpool_get_allocator → spdk_mempool_get
 *                            ↘ (fallback) env_vmalloc
 */
void *
env_mpool_new(struct env_mpool *mpool, uint32_t count)
{
	void *items = NULL;
	/* [한국어] 반환할 슬롯 포인터 — 기본 NULL로 시작해 성공 시 채워짐. */
	env_allocator *allocator;
	/* [한국어] 적합한 단계 풀 핸들 임시. */
	size_t size = mpool->hdr_size + (mpool->elem_size * count);
	/* [한국어] 실제 요청 바이트 크기 — 풀 미히트 fallback(vmalloc) 시 사용. */

	allocator = env_mpool_get_allocator(mpool, count);
	/* [한국어] count → 단계 풀 매핑 시도. */

	if (allocator) {
		items = env_allocator_new(allocator);
		/* [한국어] 단계 풀 히트 → spdk_mempool_get로 슬롯 1개 빌림(필요 시 zero-fill). */
	} else if (mpool->fallback) {
		items = env_vmalloc(size);
		/* [한국어] 풀 미커버 + fallback 허용 → vmalloc 경로로 동적 할당.
		 * 성능은 풀에 비해 낮지만 큰 count에 대한 안전망 역할. */
	}

	return items;
	/* [한국어] 둘 다 실패면 NULL — 호출자가 backpressure/에러 처리. */
}

/*
 * [한국어]
 * env_mpool_del - env_mpool_new로 빌린 슬롯을 반환.
 *
 * @mpool : 대상 mpool.
 * @items : new()가 반환한 슬롯 포인터.
 * @count : new() 시 사용했던 동일 count — 어느 풀로 돌려줄지 결정에 사용(혹은 vmalloc 분기).
 * @return: true=정상 반환, false=풀 미커버 + fallback 미허용(메모리 누수 — 비정상 호출).
 *
 * count는 new에서 사용한 값과 정확히 같아야 한다(아니면 잘못된 풀로 반환되어 손상). vmalloc fallback
 * 으로 잡힌 항목은 동일 분기로 vfree 처리.
 *
 * 호출 체인:
 *   OCF IO 완료/해제 → [env_mpool_del] → env_allocator_del → spdk_mempool_put
 *                                       ↘ (fallback) env_vfree
 */
bool
env_mpool_del(struct env_mpool *mpool,
	      void *items, uint32_t count)
{
	env_allocator *allocator;
	/* [한국어] count로 산출한 단계 풀 핸들 — new에서와 동일해야 일관성 보장. */

	allocator = env_mpool_get_allocator(mpool, count);
	/* [한국어] 매핑 재계산 — new와 동일 알고리즘. */

	if (allocator) {
		env_allocator_del(allocator, items);
		/* [한국어] 단계 풀로 슬롯 반환 — spdk_mempool_put. lock-free ring 기반. */
	} else if (mpool->fallback) {
		env_vfree(items);
		/* [한국어] vmalloc 경로로 잡힌 항목 → vfree로 해제. */
	} else {
		return false;
		/* [한국어] 풀 미커버 + fallback 미허용 — 비정상 상황(누수). 디버그를 위해 false 반환. */
	}

	return true;
	/* [한국어] 정상 반환 완료. */
}
