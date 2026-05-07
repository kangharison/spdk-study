/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Likely/unlikely branch prediction macros
 */

/*
 * [한국어 설명] 분기 예측 힌트 매크로 정의 (likely.h)
 *
 * === 파일의 역할 ===
 * GCC/Clang의 `__builtin_expect`를 감싸 SPDK 전역에서 사용하는 분기 예측
 * 힌트 매크로(`spdk_likely`, `spdk_unlikely`)를 제공한다. 이 매크로는 컴파일
 * 타임에 조건식의 평가 결과가 참/거짓 중 어느 쪽일 가능성이 높은지를 컴파일러에
 * 알려주어, 컴파일러가 아래 두 가지 최적화를 수행하도록 유도한다.
 *   1) 기본 경로(fall-through path)의 명령어 배치 최적화 — 자주 실행되는
 *      분기를 if 본문 직후에 배치하고, 드문 분기는 뒤로 미룬다. 결과적으로
 *      CPU의 명령어 캐시·분기 예측기(BTB) 히트율이 올라간다.
 *   2) 프로파일 없이도 likely/unlikely 힌트를 기반으로 basic block의
 *      가중치를 부여하여 레지스터 할당·인라이닝 비용 판단에 반영.
 * SPDK는 polled-mode I/O 경로에서 매 폴링 반복마다 수많은 조건문을 평가하므로,
 * 아주 드문 에러/완료 이벤트에 `spdk_unlikely`를 붙여 hot path의 파이프라인
 * 연속성을 유지하는 것이 성능에 직접적으로 영향을 준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 가장 하단의 무의존 빌딩 블록에 해당한다. 거의 모든 서브시스템의
 * hot path(NVMe 큐 폴링, bdev I/O 제출, sock 수신, reactor 루프 등)에서
 * 이 매크로를 사용한다. 런타임에 아무 오버헤드가 없는(순수 매크로 치환)
 * 헤더 온리 구성이며, 공개 API이므로 SPDK를 라이브러리로 쓰는 외부 앱도
 * 동일한 이름으로 쓸 수 있다.
 * 실행 컨텍스트: 모든 SPDK 컨텍스트(유저스페이스 reactor, poller 콜백,
 * admin/IO 커맨드 경로, RPC 핸들러)에서 사용 가능.
 *
 * === 타 모듈과의 연결 ===
 * 의존 대상: `spdk/stdinc.h`(표준 헤더 일괄 포함) — 실질적 의존은 없으나
 *           SPDK 공개 헤더 작성 관례에 따라 포함.
 * 의존하는 모듈: 거의 모든 SPDK 서브시스템 (lib/nvme, lib/bdev, lib/thread,
 *           lib/sock, lib/nvmf, module/bdev/*, app/*, examples/* …).
 * 공유 자료구조: 없음 (순수 컴파일러 힌트 매크로).
 * 데이터 흐름: 없음 — 오직 컴파일러 분기 최적화를 위한 정적 힌트.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_unlikely(cond): 조건식이 거짓일 확률이 높다고 컴파일러에 힌트.
 *                         주로 에러 처리, rare path, 초기화되지 않은 상태
 *                         체크 등에 사용.
 *   - spdk_likely(cond):   조건식이 참일 확률이 높다고 컴파일러에 힌트.
 *                         주로 fast path, 성공 반환, 정상 완료 확인 등에
 *                         사용.
 * 구조체/전역 변수: 없음.
 */

#ifndef SPDK_LIKELY_H            /* [한국어] include 가드 시작 — 같은 컴파일 유닛에서 중복 포함되어도 매크로가 중복 정의되지 않도록 보호 */
#define SPDK_LIKELY_H            /* [한국어] 가드 심볼 정의 — 아래 매크로들을 단 한 번만 본문에 포함시키는 기준 */

#include "spdk/stdinc.h"         /* [한국어] SPDK 표준 include 묶음 — 실제로 이 파일은 표준 타입을 쓰지 않지만, SPDK 공개 헤더 관례상 stdinc를 먼저 포함하여 사용자가 추가 include 없이 이 헤더만 포함해도 문제없도록 보장 */

#ifdef __cplusplus               /* [한국어] C++에서 이 헤더를 포함할 때 C 링크 규약을 적용하기 위한 가드 — 매크로는 링크 규약과 무관하지만, 향후 이 헤더에 함수 선언이 추가되어도 안전하도록 관례를 따른다 */
extern "C" {                     /* [한국어] C++ 네임맹글링 억제 — C 심볼 규약으로 외부 링크 */
#endif

#define spdk_unlikely(cond)	__builtin_expect((cond), 0)
                                 /* [한국어] __builtin_expect(expr, c):
                                  *   - GCC/Clang 내장: expr의 값이 c일 확률이 높다고 컴파일러에 알림
                                  *   - 반환값: expr의 원래 값 (부작용 없음 — 런타임 비교/연산 없음)
                                  *   - 두 번째 인수 0은 "cond가 거짓(0)일 가능성이 크다"는 의미
                                  *   - 괄호 감싸기 `(cond)`는 매크로 치환 시 연산자 우선순위 오류 방지
                                  *   사용 예: if (spdk_unlikely(rc != 0)) { ...에러 처리... }
                                  *   효과: 정상 경로(rc == 0)가 fall-through로 배치되어 분기 예측·명령어
                                  *         프리페치에 유리. unlikely 분기는 함수 끝으로 밀려나 cold 영역으로 이동 */

#define spdk_likely(cond)	__builtin_expect(!!(cond), 1)
                                 /* [한국어] __builtin_expect(!!(cond), 1):
                                  *   - 두 번째 인수 1은 "cond가 참(비영)일 가능성이 크다"는 의미
                                  *   - `!!(cond)`의 이중 부정은 임의 타입(포인터·정수·enum 등)의 cond를
                                  *     확실히 0 또는 1로 정규화한다. __builtin_expect는 long 두 개를 받는데,
                                  *     cond가 31/63비트 값을 가지면 1과 직접 비교할 때 모호해질 수 있어
                                  *     !!로 명시적 bool 캐스팅을 건 것이다.
                                  *   사용 예: if (spdk_likely(io->status == SUCCESS)) { ...fast path... }
                                  *   효과: hot path가 straight-line으로 배치되어 CPU 프론트엔드 stall 최소화 */

#ifdef __cplusplus               /* [한국어] C++ 가드 종료 — extern "C" 블록 닫기 */
}
#endif

#endif                           /* [한국어] include 가드 종료 — SPDK_LIKELY_H */
