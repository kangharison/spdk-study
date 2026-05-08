/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL durable-format 객체 ID 변환 헬퍼 (ftl_df.h)
 *
 * === 파일의 역할 ===
 * "Durable format(df)" 객체란, 디스크에 영속적으로 저장될 때 절대 포인터가 아닌
 * "베이스 주소로부터의 오프셋"으로 표현되어야 하는 구조체를 말한다. FTL의 슈퍼블록 blob 영역에는
 * 길이 가변 메타데이터들이 chained list 형태로 들어가는데, 이들을 다음 부팅에서 다시 매핑할 때
 * 매번 hugepage 가상 주소가 달라지더라도 동일한 객체를 가리키도록 하기 위해
 * 포인터 ↔ 오프셋 간 변환 헬퍼를 제공하는 헤더이다.
 * 정의되는 것은 (1) ftl_df_obj_id 타입(uint64_t 별칭),
 * (2) 무효 sentinel(FTL_DF_OBJ_ID_INVALID), (3) 인라인 변환 함수 두 개뿐이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 슈퍼블록 영구 저장(layout) 시스템의 가장 하위 빌딩 블록.
 * 호출 체인: ftl_sb_v3.c / ftl_sb_v5.c / ftl_layout_upgrade.c → ftl_df_get_obj_id /
 *   ftl_df_get_obj_ptr. 디스크에 쓰일 때는 절대 포인터를 ID(=오프셋)로 변환하고,
 *   메모리에 다시 로드된 후에는 ID를 새 베이스 주소 + 오프셋 포인터로 변환한다.
 * 실행 컨텍스트: 호스트 유저스페이스. 둘 다 인라인 함수이므로 호출 비용은 거의 0.
 *   슈퍼블록 메타데이터 직렬화/역직렬화 단계에서만 호출된다(런타임 hot path 아님).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/stdinc.h(uint64_t, assert, uintptr_t).
 * 의존되는 모듈: utils/ftl_mempool.h(durable mempool API), ftl_sb_common.h(슈퍼블록 헤더에서
 *   df_id 필드들 사용), ftl_sb_v3/v5.h, upgrade/ftl_layout_upgrade.c.
 * 데이터 흐름: 슈퍼블록 영구 buf → ftl_df_obj_id로 인코딩 → bdev write → bdev read →
 *   ftl_df_obj_id → ftl_df_get_obj_ptr로 디코딩 → 인메모리 포인터.
 * 공유 상태: 베이스 주소 자체는 호출자가 관리하는 외부 mempool/슈퍼블록 buf의 시작 주소이므로
 *   이 헤더 자체는 어떠한 전역 상태도 보유하지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 *   - typedef uint64_t ftl_df_obj_id: 베이스 주소로부터의 바이트 오프셋(uint64_t).
 *   - FTL_DF_OBJ_ID_INVALID: 0xFFFF..FF — "유효하지 않은 df 객체" sentinel.
 *   - ftl_df_get_obj_id(base, ptr): 절대 포인터 → 오프셋 변환. base ≤ ptr 어서션 포함.
 *   - ftl_df_get_obj_ptr(base, id): 오프셋 → 절대 포인터 변환. id != INVALID 어서션 포함.
 */

#ifndef FTL_DF_H
#define FTL_DF_H
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "spdk/stdinc.h"
/* [한국어] uint64_t, uintptr_t, assert, char 등 표준 타입과 어서션 매크로를 가져옴. */

/* Durable format (df) object is an offset */
typedef uint64_t ftl_df_obj_id;
/* [한국어] durable-format 객체 ID 타입 정의 — 베이스 버퍼 시작점에서의 바이트 오프셋을
 * 의미한다. uint64_t 별칭으로 단순화했지만 의미적으로는 "포인터 호환 영구 ID"이며,
 * 슈퍼블록 blob 영역에 직렬화될 때 그대로 저장된다. */

#define FTL_DF_OBJ_ID_INVALID ((ftl_df_obj_id)-1)
/* [한국어] 0xFFFFFFFFFFFFFFFF — "df 객체가 없음/만료됨" 표식. 슈퍼블록 v5의
 * blob_area_end가 INVALID라면 blob 영역이 아직 비어 있다는 의미로 해석된다. */

/**
 * @brief Convert df object ptr to df object id
 *
 * @param base		allocation base address
 * @param df_obj_ptr	df object ptr
 *
 * @return df object id
 */
/*
 * [한국어]
 * ftl_df_get_obj_id - 인메모리 절대 포인터를 슈퍼블록에 영속화 가능한 df 오프셋 ID로 변환
 *
 * @param base: 매핑된 베이스 주소(예: 슈퍼블록 buf 시작점, mempool 영역 시작점). 호출자가 보장.
 * @param df_obj_ptr: 변환 대상 객체의 절대 포인터. 반드시 base 이상이어야 한다.
 * @return: df_obj_ptr - base 의 바이트 오프셋. 결과는 디스크에 그대로 저장 가능.
 *
 * 동기/배경: hugepage 가상 주소는 매 부팅마다 달라질 수 있으므로 포인터를 직접 디스크에
 *   쓸 수 없다. 베이스로부터의 오프셋만 저장하면 다음에 베이스가 다른 주소로 매핑되어도
 *   동일한 논리 객체를 다시 찾아낼 수 있다.
 * 동작: char* 캐스팅으로 바이트 단위 차분 계산, base ≤ df_obj_ptr 어서션으로 음수 오프셋 차단.
 * 실행 컨텍스트: 인라인 — 호출자 스레드에서 직접 실행. 슈퍼블록 영속화 단계(SPDK reactor)에서만 호출.
 * caller: 슈퍼블록 v5 직렬화 코드, 메타데이터 영역 등록 코드.
 * callee: assert (런타임 검증) 외에 외부 호출 없음.
 *
 * 호출 체인:
 *   ftl_superblock_v5_store_blob_area / ftl_mempool_get_df_obj_id → [이 함수] → assert
 */
static inline ftl_df_obj_id
ftl_df_get_obj_id(void *base, void *df_obj_ptr)
{
	assert(base <= df_obj_ptr);
	/* [한국어] 베이스보다 앞쪽 메모리를 가리키는 포인터는 잘못된 입력. 디버그 빌드에서 즉시 잡는다.
	 * (릴리즈에서는 어서션이 사라지므로 호출자가 사전 검증해야 함.) */
	return ((char *)df_obj_ptr - (char *)base);
	/* [한국어] char* 산술로 바이트 단위 차분 계산. ptrdiff_t의 부호 표현을 거치지만
	 * 사전 어서션으로 음수가 될 일이 없으므로 uint64_t로 안전하게 암시 변환된다.
	 * 결과는 슈퍼블록의 ftl_df_obj_id 필드(blob_area_end, df_id, df_next 등)에 저장된다. */
}

/**
 * @brief Convert df object id to df object ptr
 *
 * @param base		allocation base address
 * @param df_obj_id	df object id
 *
 * @return df object ptr
 */
/*
 * [한국어]
 * ftl_df_get_obj_ptr - 슈퍼블록에서 읽은 df 오프셋 ID를 새 매핑의 절대 포인터로 환원
 *
 * @param base: 현재 부팅에서 매핑된 베이스 주소.
 * @param df_obj_id: 디스크에서 읽은 오프셋. INVALID여서는 안 된다.
 * @return: base + df_obj_id 위치를 가리키는 void* (호출자가 적절한 타입으로 캐스팅).
 *
 * 동기/배경: 슈퍼블록 로드 시점에 영구 오프셋을 다시 사용 가능한 포인터로 환원해야
 *   메모리상의 chained list 등을 따라갈 수 있다.
 * 동작: uintptr_t 산술로 베이스 주소 + 오프셋 계산 후 void*로 캐스팅.
 * 실행 컨텍스트: 인라인 — 슈퍼블록 로드/리하이드레이트(re-hydrate) 단계에서만 호출.
 *
 * 호출 체인:
 *   ftl_superblock_v5_load_blob_area / ftl_mempool_get_df_ptr → [이 함수]
 */
static inline void *
ftl_df_get_obj_ptr(void *base, ftl_df_obj_id df_obj_id)
{
	assert(df_obj_id != FTL_DF_OBJ_ID_INVALID);
	/* [한국어] INVALID ID로 호출하면 호출자 로직 버그. 디버그에서 즉시 잡는다. */
	return (void *)((uintptr_t)base + df_obj_id);
	/* [한국어] uintptr_t로 캐스팅 후 정수 덧셈을 사용한다. char* 산술도 가능하지만
	 * 정수 산술이 일부 ABI/포인터 모델에서 더 안전(ptrdiff_t 오버플로 회피)하다.
	 * 결과 포인터는 호출자가 적절한 구조체 타입으로 캐스팅해서 사용한다. */
}

#endif /* FTL_DF_H */
/* [한국어] 헤더 가드 종료. */
