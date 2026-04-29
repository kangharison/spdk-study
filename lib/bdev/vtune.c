/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Intel VTune Profiler 통합 훅 (vtune.c)
 *
 * === 파일의 역할 ===
 * Intel VTune Profiler(과거 VTune Amplifier)의 ITT(Instrumentation and Tracing
 * Technology) 정적 라이브러리(`ittnotify_static.c`)를 SPDK bdev 라이브러리에
 * 한 곳에서만 컴파일·링크하기 위한 thin wrapper 파일이다. ITT API
 * (`__itt_task_begin`, `__itt_task_end`, `__itt_thread_set_name` 등)는 lib/bdev
 * 의 다른 소스(주로 `bdev.c`)에서 호출되며, 호출 시 측정 이벤트가 VTune의
 * collector에 전달되어 timeline에 task 구간으로 표시된다.
 * 이 파일 자체는 함수를 정의하지 않으며, ITT 정적 구현 .c 한 파일을 포함시키는
 * 역할만 한다 — VTune 헤더(`ittnotify.h`)는 inline 형태로 ITT API를 매핑하지만
 * 실제 thread/task 관리·notification 디스패치 코드는 ittnotify_static.c에
 * 정의되어 있어 컴파일 단위 어딘가에 한 번 포함되어야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 빌드 시 `./configure --with-vtune=<path>`를 지정하면
 * `SPDK_CONFIG_VTUNE`이 1로 정의되고, 본 파일이 ittnotify_static.c를 lib/bdev
 * 모듈에 포함시킨다. 옵션이 꺼져 있으면 본 파일은 사실상 빈 컴파일 단위가 된다.
 * 호출 흐름은 다음과 같다:
 *   bdev_io 제출/완료 경로 (lib/bdev/bdev.c)
 *     → __itt_task_begin / __itt_task_end (ittnotify_static.c 정의)
 *       → ITT collector(VTune Profiler 데몬)
 *         → VTune GUI timeline / hotspot view
 * 결과적으로 분석가가 SPDK 워크로드를 VTune으로 프로파일링할 때 bdev I/O
 * 제출~완료 구간을 task 단위로 시각화할 수 있다.
 *
 * === 타 모듈과의 연결 ===
 *  - **lib/bdev/bdev.c** — 실제로 ITT API를 호출하는 곳. SPDK_CONFIG_VTUNE이
 *    켜져 있을 때 bdev I/O begin/end 시점에 task notification을 발생시킴.
 *  - **ittnotify_static.c** (Intel VTune SDK 제공) — ITT 정적 구현. 본 파일이
 *    `#include`로 그 코드를 lib/bdev 컴파일 단위에 끌어온다. 별도 라이브러리로
 *    링크하지 않는 이유는 ITT가 위치 무관 정적 코드라 한 번만 포함하면 충분하기 때문.
 *  - **VTune SDK 헤더** (`ittnotify.h`) — 호출자가 사용하는 inline API 선언.
 *  - **빌드 시스템** — `mk/spdk.common.mk` 등에서 SPDK_CONFIG_VTUNE을
 *    set_define 하고, VTune SDK include path를 추가.
 *
 * === 주요 함수/구조체 요약 ===
 * 본 파일은 함수/구조체를 정의하지 않는다. 단지 다음 두 개의 전처리 동작이 전부:
 *   1) `#if SPDK_CONFIG_VTUNE` — VTune 통합이 활성화된 경우에만 본문 활성화.
 *   2) `#include "ittnotify_static.c"` — Intel ITT 정적 구현을 컴파일 단위에 포함.
 * 추가로 GCC에서 ITT 정적 코드가 발생시키는 두 가지 경고
 * (`-Wsign-compare`, `-Wimplicit-fallthrough`)를 #pragma로 억제한다 — SPDK 본체는
 * `-Wall -Werror` 빌드이므로 외부 코드의 경고를 그대로 두면 빌드가 실패한다.
 */

#include "spdk/config.h"
/* [한국어] SPDK 빌드 옵션 매크로 정의 헤더.
 *  여기서 정의되는 SPDK_CONFIG_VTUNE 매크로의 값(0/1)에 따라
 *  본 파일이 빈 컴파일 단위가 되거나 ITT 정적 구현을 포함시키게 된다. */

#if SPDK_CONFIG_VTUNE
/* [한국어] VTune 통합이 활성화된 빌드에서만 아래 본문을 컴파일.
 *  비활성화 빌드에서는 ittnotify_static.c가 포함되지 않으므로
 *  ITT API 호출자(bdev.c)도 매크로 가드로 비활성화되어 호출 자체가 사라진다. */

/* Disable warnings triggered by the VTune code */
/* [한국어] 외부(Intel VTune SDK)에서 제공된 ittnotify_static.c는 SPDK의 strict
 *  경고 정책(-Wall -Werror)에서 일부 경고를 발생시킨다. 본 SPDK 코드는 수정
 *  대상이 아니므로 컴파일러 #pragma로 해당 경고만 국소 억제한다. */
#if defined(__GNUC__) && \
	__GNUC__ > 4 || \
	(__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
/* [한국어] GCC 4.6 이상에서만 #pragma GCC diagnostic ignored가 안정적으로 동작.
 *  4.6 이전 컴파일러는 무시된다 (지원 범위가 아님). */
#pragma GCC diagnostic ignored "-Wsign-compare"
/* [한국어] -Wsign-compare 억제 — ITT 정적 코드 내부에서 signed/unsigned
 *  비교가 발생하는 부분에 대한 경고를 끄기 위함. */
#if __GNUC__ >= 7
/* [한국어] GCC 7부터 -Wimplicit-fallthrough가 기본 경고로 추가됨.
 *  ITT 코드의 switch case에 명시적 fallthrough 마커가 없어 경고가 발생하므로 억제. */
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#endif

#include "ittnotify_static.c"
/* [한국어] Intel VTune SDK가 제공하는 ITT 정적 구현 .c 파일을 본 컴파일 단위에 포함.
 *  이 한 줄로 ITT의 thread/task notification 디스패치 함수들이 lib/bdev에 한 번
 *  정의된다. 다른 어떤 SPDK 컴파일 단위에서도 이 파일을 다시 포함하면 안 된다
 *  (다중 정의 링커 에러 발생). */

#endif
/* [한국어] #if SPDK_CONFIG_VTUNE 종료. */
