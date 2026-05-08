/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] CRC 가속 백엔드 선택용 내부 헤더 (crc_internal.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 빌드 환경에 따라 어떤 CRC 가속 인트린식 백엔드를 사용할지 선택하고
 * 해당 헤더를 인클루드한다. SPDK는 CRC 계산을 (1) Intel ISA-L 라이브러리,
 * (2) ARMv8 CRC32 인스트럭션, (3) x86 SSE4.2 CRC32 인스트럭션, 또는 (4) 표 기반
 * 소프트웨어 폴백 중 하나로 수행하는데, 이 헤더는 매크로 정의로 어느 경로를 사용할지
 * 컴파일 타임에 결정한다. spdk/config.h에서 SPDK_CONFIG_ISAL을 참조해 ISA-L 우선,
 * 다음으로 ARM CRC, 그다음 SSE4.2 순으로 선택된다. .c 파일들(예: crc32c.c)은
 * SPDK_HAVE_* 매크로의 정의 여부를 보고 분기한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK util 라이브러리 → CRC 백엔드 선택 헬퍼.
 * 호출 체인:
 *   build system → spdk/config.h (SPDK_CONFIG_ISAL 등 정의) → [crc_internal.h] →
 *   crc32c.c / crc32_ieee.c 등이 SPDK_HAVE_* 매크로로 분기 → 실제 CRC 산출
 * 실행 컨텍스트: 컴파일 타임 의사 결정만 수행. 런타임 코드를 포함하지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/config.h(빌드 컨피규레이션), 그리고 isa-l/crc.h, arm_acle.h,
 *   x86intrin.h 중 선택된 헤더.
 * - 의존하는 자: lib/util/crc16.c, crc32c.c, 그리고 ISA-L/ARM/SSE4.2 가속 경로를
 *   가진 파일들. 이들은 SPDK_HAVE_ISAL/SPDK_HAVE_ARM_CRC/SPDK_HAVE_SSE4_2 매크로의
 *   정의 여부로 인트린식과 폴백 코드 사이를 분기한다.
 * - 데이터 흐름: 빌드 옵션 → 매크로 정의 → 사용자 측의 #ifdef 분기 → 실제 가속 경로 선택.
 *
 * === 주요 함수/구조체 요약 ===
 * - 본 헤더에는 함수/구조체 정의가 없다. 다만 다음 매크로가 정의될 수 있다:
 *   - SPDK_HAVE_ISAL    : Intel ISA-L의 crc.h를 사용 가능.
 *   - SPDK_HAVE_ARM_CRC : ARMv8 ACLE CRC32 인트린식 사용 가능.
 *   - SPDK_HAVE_SSE4_2  : x86 SSE4.2 CRC32 인트린식 사용 가능.
 *   세 매크로 모두 미정의이면 표 기반 소프트웨어 폴백을 사용해야 한다.
 */

#ifndef SPDK_CRC_INTERNAL_H
/* [한국어] 헤더 가드 시작. */
#define SPDK_CRC_INTERNAL_H

#include "spdk/config.h"
/* [한국어] SPDK 빌드 컨피규레이션 매크로(SPDK_CONFIG_ISAL 등)를 정의하는 헤더.
 * configure 단계에서 자동 생성되며 사용 가능 라이브러리 정보를 컴파일 시 전달. */

#ifdef SPDK_CONFIG_ISAL
/* [한국어] 우선순위 1순위: Intel ISA-L (Intelligent Storage Acceleration Library).
 * ISA-L은 SIMD 가속 CRC, EC, 압축 등을 제공. CRC 처리량이 가장 높다. */
#define SPDK_HAVE_ISAL
/* [한국어] 다른 .c 파일이 ISA-L 경로를 사용할 수 있도록 표시. */
#include <isa-l/include/crc.h>
/* [한국어] ISA-L의 CRC API(crc16_t10dif, crc32_iscsi 등) 선언을 가져옴. */
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
/* [한국어] 2순위: ARM 64비트 CPU에서 CRC32 확장(ARMv8.0-A 옵션, ARMv8.1+에서 필수)이
 * 가능한 경우. __ARM_FEATURE_CRC32은 컴파일러가 -march=armv8-a+crc 등으로 빌드될 때 정의. */
#define SPDK_HAVE_ARM_CRC
/* [한국어] ARM CRC 가속 경로 사용 가능을 표시. */
#include <arm_acle.h>
/* [한국어] ARM C Language Extensions: __crc32cb/h/w/d 등 CRC 인트린식을 제공. */
#elif defined(__x86_64__) && defined(__SSE4_2__)
/* [한국어] 3순위: x86_64에서 SSE4.2(_mm_crc32_*) 가능한 경우.
 * Nehalem 이후 Intel/AMD CPU 다수가 지원. */
#define SPDK_HAVE_SSE4_2
/* [한국어] SSE4.2 CRC 가속 경로 사용 가능을 표시. */
#include <x86intrin.h>
/* [한국어] x86 SIMD 인트린식 묶음 헤더. _mm_crc32_u8/u16/u32/u64 등을 포함. */
#endif
/* [한국어] 위 세 분기 모두 안 잡히면 SPDK_HAVE_* 매크로가 정의되지 않으며,
 * 사용자 측에서는 소프트웨어 표 기반 폴백 코드를 사용해야 한다. */

#endif /* SPDK_CRC_INTERNAL_H */
/* [한국어] 헤더 가드 끝. */
