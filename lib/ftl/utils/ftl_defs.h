/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 공통 매크로 정의 헤더 (ftl_defs.h)
 *
 * === 파일의 역할 ===
 * lib/ftl 전체에서 반복적으로 사용되는 단순 상수와 디버그 트랩 매크로를 한 곳에 모아 둔다.
 * 정의되는 항목은 크게 세 부류이다: (1) 메모리 단위 매크로(KiB/MiB/GiB/TiB),
 * (2) 디버그 어서션·치명 종료를 위한 ftl_abort()/ftl_bug() 매크로,
 * (3) "잘못된 값" 표식을 위한 FTL_INVALID_VALUE/FTL_BAND_ID_INVALID/FTL_BAND_PHYS_ID_INVALID 상수이다.
 * FTL 코드는 NAND 밴드 ID, 청크 ID, LBA, PPA 등 64비트 정수를 다룰 때
 * "유효하지 않음"을 별도의 sentinel 값으로 표현하는데 그 통일된 sentinel을 여기서 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 헤더는 lib/ftl 전체 컴파일 그래프의 가장 아래(말단 leaf)에 가깝다.
 * 단 하나의 외부 의존성 spdk/stdinc.h만 갖는다.
 * 호출 체인: lib/ftl 안의 거의 모든 헤더(ftl_sb_common.h, ftl_internal.h, ftl_band.h 등)
 *   → ftl_defs.h. 이 헤더 자체에는 함수가 없으며 매크로 확장만 일어난다.
 * 실행 컨텍스트: 매크로 확장은 컴파일 시간에만 일어나고, ftl_abort()/ftl_bug() 호출은
 *   사용자 프로세스 컨텍스트(SPDK reactor 스레드)에서 이루어진다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/stdinc.h(공통 시스템 헤더 모음 - assert.h, stdint.h, abort 등 포함).
 * 의존되는 모듈: utils/ftl_df.h, utils/ftl_mempool.h, ftl_sb_common.h, ftl_internal.h,
 *   ftl_band.h, ftl_nv_cache.h 등 lib/ftl 거의 모든 헤더가 직간접적으로 이 매크로들을 사용한다.
 * 데이터 흐름: 매크로만 노출하므로 런타임 데이터 흐름은 없다. 대신 FTL_BAND_ID_INVALID 같은
 *   sentinel 상수가 슈퍼블록 영구 데이터(ftl_superblock_gc_info.band_id_high_prio 등)와
 *   런타임 자료구조 사이에 일관된 "비어있음" 표현을 제공한다.
 *
 * === 주요 함수/구조체 요약 ===
 * 함수/구조체는 정의하지 않는다. 핵심 매크로는 다음과 같다:
 *   - KiB/MiB/GiB/TiB: 1ULL << {10,20,30,40} 표현으로 슈퍼블록 크기, 청크 크기 등 큰 정수 단위 산출에 사용.
 *   - ftl_abort(): assert(false) + abort()를 묶어 디버그 빌드에서는 어서션, 릴리즈에서도 즉시 종료를 강제.
 *   - ftl_bug(cond): cond가 참이면 spdk_unlikely로 분기 예측을 비정상 경로로 보내고 ftl_abort 실행.
 *   - FTL_INVALID_VALUE: ((uint64_t)-1) — "유효하지 않음" 통일 sentinel.
 *   - FTL_BAND_ID_INVALID / FTL_BAND_PHYS_ID_INVALID: 밴드 ID/물리 reclaim 단위 ID에 대한 sentinel 별칭.
 */

#ifndef FTL_DEFS_H
#define FTL_DEFS_H
/* [한국어] 헤더 가드 — 다중 포함 방지. lib/ftl 안의 헤더 다수가 이 헤더를
 * 직간접적으로 끌어들이므로 가드가 없으면 매크로 재정의 경고가 발생할 수 있다. */

#include "spdk/stdinc.h"
/* [한국어] SPDK 공통 표준 인클루드 모음 — stdint.h(uint64_t), assert.h(assert),
 * stdlib.h(abort), spdk/likely.h(spdk_unlikely)를 한꺼번에 끌어들인다.
 * 아래 매크로들이 의존하는 모든 심볼을 이 한 줄로 충족시킨다. */

#ifndef KiB
#define KiB (1ULL << 10)
/* [한국어] 1024 바이트(2^10)의 unsigned long long 표현. 64비트 산술에서
 * 오버플로우를 피하려고 1ULL을 사용하며, 슈퍼블록 사이즈/메타데이터 영역 크기 등
 * 큰 단위 표현에 빈번히 등장한다. (예: FTL_SUPERBLOCK_SIZE = 128ULL * KiB) */
#endif

#ifndef MiB
#define MiB (1ULL << 20)
/* [한국어] 1024*1024(2^20) 바이트. NV cache 청크 크기, 워크로드 통계 단위 등에 사용. */
#endif

#ifndef GiB
#define GiB (1ULL << 30)
/* [한국어] 2^30 바이트. base bdev 사이즈, 전체 L2P 매핑 테이블 크기 단위 등에 사용. */
#endif

#ifndef TiB
#define TiB (1ULL << 40)
/* [한국어] 2^40 바이트. 대용량 SSD 사이즈 표시 등 거의 한도 표시에 사용. */
#endif

#define ftl_abort()		\
	do {			\
		assert(false);	\
		abort();	\
	} while (0)
/* [한국어] FTL 치명 오류 시 즉시 프로세스를 종료하는 매크로.
 *  - assert(false): 디버그 빌드에서는 abort 직전 파일/라인 메시지를 출력.
 *  - abort(): 릴리즈에서도 무조건 SIGABRT로 프로세스 종료(코어 덤프 가능).
 *  do/while(0) 패턴으로 if/else 블록 안에서도 단일 문장처럼 안전하게 사용된다.
 *  사용 예: 슈퍼블록 마법값 불일치, 메모리 풀 고갈 같은 복구 불가 상태에서 호출. */

#define ftl_bug(cond)				\
	do {					\
		if (spdk_unlikely((cond))) {	\
			ftl_abort();		\
		}				\
	} while (0)
/* [한국어] 절대 발생해서는 안 되는 조건을 런타임에 검사하는 매크로.
 *  - spdk_unlikely(cond): 컴파일러에 "이 분기가 거의 안 잡힌다"고 힌트(__builtin_expect).
 *    덕분에 hot path에서 검사 비용이 거의 0에 수렴한다.
 *  - cond 참 시 ftl_abort()로 즉시 종료. assert와 달리 NDEBUG 빌드에서도 살아있다.
 *  사용 예: ftl_property_set_generic()가 size 불일치를 잡을 때 호출. */

#define FTL_INVALID_VALUE		((uint64_t)-1)
/* [한국어] 64비트 부호 없는 정수 0xFFFFFFFFFFFFFFFF. FTL의 모든 "유효하지 않은 64비트 ID"
 * sentinel의 원형. ftl_addr/seq_id/band_id 등에서 일관되게 사용되어 비교 코드를 단순화한다. */
#define FTL_BAND_ID_INVALID		FTL_INVALID_VALUE
/* [한국어] 밴드(NAND 슈퍼블록 단위) ID가 비어있음을 표시. 슈퍼블록의
 * gc_info.band_id_high_prio 등에 저장되어 dirty shutdown 후에도 의미가 보존된다. */
#define FTL_BAND_PHYS_ID_INVALID	FTL_INVALID_VALUE
/* [한국어] 물리 reclaim 단위(여러 밴드를 묶은 큰 회수 단위) ID가 비어있음을 표시.
 * SSD 내부 reclaim 지오메트리에 정렬된 단위로, 슈퍼블록의 gc_info.band_phys_id에 저장된다. */

#endif /* FTL_DEFS_H */
/* [한국어] 헤더 가드 종료. */
