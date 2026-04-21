/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Runtime and compile-time assert macros
 */

/*
 * [한국어 설명] 정적 어설션 래퍼 (assert.h)
 *
 * === 파일의 역할 ===
 * C11의 `static_assert`를 SPDK 전역에서 안전하게 쓰기 위한 호환성 래퍼
 * `SPDK_STATIC_ASSERT(cond, msg)`를 제공한다. 컴파일 타임 불변식을 코드에
 * 직접 명시함으로써 (a) 구조체 크기/오프셋/정렬, (b) enum 값 범위, (c)
 * 비트필드 폭 등이 스펙과 일치하는지를 **빌드 시점**에 강제 검증한다.
 * SPDK는 NVMe·SCSI·iSCSI 같은 하드웨어/네트워크 프로토콜 구조체를 직접
 * 표현하므로, 정의된 구조체의 크기가 스펙과 어긋나면 프로토콜 전체가
 * 깨진다. 이 위험을 런타임 디버깅 없이 컴파일 오류로 감지하는 것이 목적.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 최하단 유틸. include/spdk/nvme_spec.h, scsi_spec.h, iscsi_spec.h,
 * nvmf_spec.h 등 모든 "스펙 표현 구조체" 정의 직후에 본 매크로가 배치되어
 * 구조체 크기 검증(예: `SPDK_STATIC_ASSERT(sizeof(X) == N, "...")`)을 수행한다.
 * 런타임 비용 0 — static_assert는 전혀 코드를 생성하지 않는다.
 * 실행 컨텍스트: 컴파일 타임만.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h — <assert.h>를 통한 static_assert 정의 경로 확보.
 * 의존하는 모듈:
 *   - include/spdk/nvme_spec.h (수십 개 구조체의 sizeof/offsetof 검증)
 *   - include/spdk/scsi_spec.h, iscsi_spec.h, nvmf_spec.h 등
 *   - lib/nvme/nvme_internal.h, lib/nvmf/nvmf_internal.h 등 내부 구조체 검증
 * 공유 자료구조: 없음 (순수 매크로).
 *
 * === 주요 함수/구조체 요약 ===
 *   - SPDK_STATIC_ASSERT(cond, msg):
 *       C11 static_assert가 사용 가능하면 그대로 위임.
 *       지원하지 않는 구형 컴파일러에서는 **빈 매크로**로 degrade 되어
 *       빌드는 성공하지만 검증은 수행되지 않음.
 *     → 운영 환경은 반드시 static_assert 지원 컴파일러(GCC ≥ 4.6, Clang,
 *       MSVC 2015+)를 사용해야 한다.
 */

#ifndef SPDK_ASSERT_H            /* [한국어] include 가드 시작 */
#define SPDK_ASSERT_H            /* [한국어] 가드 심볼 정의 */

#include "spdk/stdinc.h"         /* [한국어] <assert.h> 포함 경로 확보 — C11에서 static_assert는 <assert.h> 또는 키워드로 제공 */

#ifdef __cplusplus               /* [한국어] C++ 링크 규약 가드 (C++11 이상에서는 키워드 static_assert가 직접 존재) */
extern "C" {
#endif

#ifdef static_assert             /* [한국어] <assert.h>가 static_assert를 매크로 혹은 C11 키워드로 노출하는지 검사
                                  *  - C11부터 `static_assert(cond, msg)`가 `_Static_assert`의 별칭 매크로로 제공됨
                                  *  - 구형 glibc나 pre-C11 컴파일러에선 정의되지 않음 */
#define SPDK_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
                                 /* [한국어] 표준 static_assert로 위임 — 컴파일 타임에 cond 평가:
                                  *  - cond가 거짓이면 빌드 중단 및 msg를 에러에 포함
                                  *  - cond가 참이면 기호(code)가 전혀 생성되지 않음
                                  *  - 사용 예: SPDK_STATIC_ASSERT(sizeof(struct nvme_cmd) == 64,
                                  *              "NVMe command must be 64 bytes per spec"); */
#else
/**
 * Compatibility wrapper for static_assert.
 *
 * This won't actually enforce the condition when compiled with an environment that doesn't support
 * C11 static_assert; it is only intended to allow end users with old compilers to build the package.
 *
 * Developers should use a recent compiler that provides static_assert.
 */
#define SPDK_STATIC_ASSERT(cond, msg)
                                 /* [한국어] 폴백 정의 — 매크로를 빈 토큰으로 치환
                                  *  - 검증은 전혀 수행되지 않으므로 빌드는 통과하지만 사양 위반이 런타임에야 드러날 수 있음
                                  *  - SPDK 팀의 공식 권장: 운영·개발 환경에서는 최신 컴파일러를 사용해 실제 static_assert가 활성화되도록 유지
                                  *  - 이 분기가 활성화되는 환경에서 구조체 크기 오류는 런타임 crash/mis-interpret로 이어질 수 있음 */
#endif

#ifdef __cplusplus               /* [한국어] C++ 링크 규약 가드 닫기 */
}
#endif

#endif /* SPDK_ASSERT_H */        /* [한국어] include 가드 종료 */
