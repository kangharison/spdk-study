/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] OCF 환경(env) — 최소 공통 헤더 (ocf_env_headers.h)
 *
 * === 파일의 역할 ===
 * libocf 코어가 의존하는 "env_*" 헤더들 사이에서 공통적으로 필요한 표준 인클루드와 OCF 라이브러리
 * 버전 식별자만 담은 매우 얇은 헤더. 다른 ocf_env_* 헤더가 본 파일을 첫 줄에서 include함으로써
 * size_t/stdbool/stdint 등 기본 타입과 OCF_VERSION_*가 한꺼번에 가시화된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   ocf_env*.h (어댑터 헤더 일체)
 *     → 본 헤더 → spdk/stdinc.h (size_t, stdbool, stdint, stdio …)
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h(SPDK 스타일 표준 헤더 묶음).
 * - 의존받음: lib/env_ocf/ 의 모든 어댑터 헤더(ocf_env.h, ocf_env_list.h 등).
 * - 데이터 흐름: 컴파일 단위에 표준 타입과 OCF 버전 매크로만 노출.
 * - 공유 자료구조: 없음(매크로만).
 *
 * === 주요 함수/구조체 요약 ===
 * - OCF_VERSION_MAIN/MAJOR/MINOR : libocf 버전 매크로(20.3.0). OCF가 ABI 호환성 체크에 사용.
 */

#ifndef __OCF_ENV_HEADERS_H__
#define __OCF_ENV_HEADERS_H__
/* [한국어] 다중 포함 가드. */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 헤더 묶음 — size_t, uint*_t, bool, NULL, errno, string.h 등을 한꺼번에 가시화.
 * 개별 시스템 헤더를 분리해서 들이지 않고 SPDK 컨벤션의 단일 진입점을 사용. */

#define OCF_VERSION_MAIN 20
/* [한국어] libocf 메이저 시리즈 번호(예: 20.x). OCF가 환경 어댑터의 ABI 호환을 체크할 때 비교에 사용. */
#define OCF_VERSION_MAJOR 3
/* [한국어] libocf 마이너 시리즈 번호 — 어댑터 측이 어느 OCF 릴리스 라인을 따르는지를 표시. */
#define OCF_VERSION_MINOR 0
/* [한국어] libocf 패치 레벨 — 작은 변경 추적. 셋이 합쳐 "20.3.0"을 의미. */

#endif /* __OCF_ENV_HEADERS_H__ */
/* [한국어] 다중 포함 가드 종결. */
