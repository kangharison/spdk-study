/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 로깅 매크로 헤더 (ftl_log.h)
 *
 * === 파일의 역할 ===
 * SPDK 공통 로깅 인프라(spdk_log) 위에 FTL 전용 prefix("[FTL][디바이스이름] ")를 붙이는
 * 얇은 매크로 래퍼를 정의한다. 모든 로그 라인은 자동으로 어느 FTL 인스턴스에서 발생했는지
 * 식별할 수 있도록 dev->conf.name이 들어가며, 그 외에는 SPDK 표준 로깅 그대로다.
 * 다섯 단계 레벨(ERR/WARN/NOTICE/INFO/DEBUG)을 모두 동일한 패턴의 가변 인자 매크로로 노출한다.
 * 디바이스 핸들이 NULL일 수도 있는 초기화/해제 경로에서도 안전하도록 NULL 체크가 들어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/ftl 코드 전체가 호출하는 로깅 진입점. 호출 체인: lib/ftl 내부 .c 파일 →
 *   FTL_xxxLOG(dev, fmt, ...) → spdk_log(level, file, line, func, "[FTL][name] " fmt, ...) →
 *   SPDK 공통 로그 시스템(syslog/stdout/file 출력).
 * 실행 컨텍스트: 호출되는 모든 SPDK reactor 스레드. spdk_log 자체는 멀티스레드 안전하게
 *   설계되어 있어 별도 동기화는 필요 없다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/stdinc.h(공통 시스템 헤더), spdk/log.h(spdk_log 함수, SPDK_LOG_ERROR
 *   등 레벨 enum). 매크로가 dev->conf.name에 접근하므로 "struct spdk_ftl_dev"가 conf.name 필드를
 *   가진다는 사실을 가정한다. 실제 정의는 ftl_core.h에 있고 이 헤더는 그 정의를 강제 include 하지
 *   않으므로, 이 헤더를 사용하는 .c 파일이 ftl_core.h를 먼저 가져오도록 해야 한다.
 * 의존되는 모듈: 거의 모든 lib/ftl/*.c 파일이 에러/경고/정보 출력에 이 매크로를 사용한다.
 *   특히 ftl_property.c 등이 잘못된 프로퍼티 등록 시 FTL_ERRLOG로 실패 사유를 남긴다.
 * 데이터 흐름: 로그 메시지가 SPDK 공통 로그 백엔드로 흘러가며 외부 데이터 흐름은 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * 모두 매크로이며 함수/구조체는 없다.
 *   - FTL_LOG_COMMON: 공통 로깅 매크로. type(ERROR/WARN/...), dev, fmt, ...를 받아
 *     spdk_log()에 [FTL][name] prefix를 붙여 전달.
 *   - FTL_ERRLOG: 에러 레벨(SPDK_LOG_ERROR) 래퍼.
 *   - FTL_WARNLOG: 경고 레벨(SPDK_LOG_WARN) 래퍼.
 *   - FTL_NOTICELOG: 정보성 알림(SPDK_LOG_NOTICE) 래퍼.
 *   - FTL_INFOLOG: 일반 정보(SPDK_LOG_INFO) 래퍼.
 *   - FTL_DEBUGLOG: 디버그(SPDK_LOG_DEBUG) 래퍼 — DEBUG 빌드에서만 효과 있음.
 */

#ifndef FTL_LOG_H
#define FTL_LOG_H
/* [한국어] 다중 포함 방지를 위한 헤더 가드. */

#include "spdk/stdinc.h"
/* [한국어] 가변 인자 매크로(##__VA_ARGS__)와 __FILE__/__LINE__/__func__가 사용 가능하도록
 * 표준 시스템 헤더를 끌어온다. */
#include "spdk/log.h"
/* [한국어] spdk_log() 함수 시그니처와 SPDK_LOG_ERROR/WARN/NOTICE/INFO/DEBUG enum 값을 가져온다.
 * 아래 매크로들의 type 인자는 SPDK_LOG_##type 형태로 토큰 결합되어 이 enum 값으로 변환된다. */

#define FTL_LOG_COMMON(type, dev, format, ...) \
	spdk_log(SPDK_LOG_##type, __FILE__, __LINE__, __func__, "[FTL][%s] "format, \
		 (dev) != NULL ? (dev)->conf.name : "N/A", ## __VA_ARGS__)
/* [한국어] FTL 모든 로그의 공통 출력 매크로.
 *  - SPDK_LOG_##type: type 토큰을 SPDK_LOG_<레벨> enum으로 결합 (예: ERROR → SPDK_LOG_ERROR).
 *  - __FILE__, __LINE__, __func__: 호출 위치 자동 기록(컴파일러 내장 매크로).
 *  - "[FTL][%s] "format: FTL 인스턴스 이름을 prefix로 붙여 어느 디바이스 로그인지 즉시 식별.
 *  - dev != NULL ? dev->conf.name : "N/A": 초기화 직전/해제 직후처럼 dev가 NULL일 수도 있는
 *    경로에서 NULL 역참조를 방지. dev가 없으면 "N/A"로 표기.
 *  - ## __VA_ARGS__: GNU 확장 — 가변 인자가 비어 있을 때 콤마를 자동 제거해 가변 인자 0개 호출도 허용.
 *  spdk_log 자체는 SPDK 공통 로깅 백엔드(stderr/syslog/파일)로 라우팅하며 멀티스레드 안전하다. */

#define FTL_ERRLOG(dev, format, ...) \
	FTL_LOG_COMMON(ERROR, dev, format, ## __VA_ARGS__)
/* [한국어] 에러 레벨 로그 — 동작이 실패했거나 비정상이지만 즉시 abort까지는 아닌 경우.
 * 사용 예: ftl_property_register()에서 동일 이름 중복 등록 발견 시. */

#define FTL_WARNLOG(dev, format, ...) \
	FTL_LOG_COMMON(WARN, dev, format, ## __VA_ARGS__)
/* [한국어] 경고 레벨 로그 — 정상 동작은 하지만 주목할 만한 이상 상황. */

#define FTL_NOTICELOG(dev, format, ...) \
	FTL_LOG_COMMON(NOTICE, dev, format, ## __VA_ARGS__)
/* [한국어] 알림 레벨 로그 — 디바이스 시작/정지처럼 사용자가 알아두면 좋은 이벤트. */

#define FTL_INFOLOG(dev, format, ...) \
	FTL_LOG_COMMON(INFO, dev, format, ## __VA_ARGS__)
/* [한국어] 정보 레벨 로그 — 일반 정보. SPDK 로그 레벨 설정에 따라 출력 여부 결정. */

#define FTL_DEBUGLOG(dev, format, ...) \
	FTL_LOG_COMMON(DEBUG, dev, format, ## __VA_ARGS__)
/* [한국어] 디버그 레벨 로그 — SPDK_LOG_DEBUG는 보통 컴파일 옵션(--enable-debug)으로 활성화될 때만
 * 실제 출력되도록 처리되어, 릴리즈 빌드 성능에는 거의 영향이 없다. */

#endif /* FTL_LOG_H */
/* [한국어] 헤더 가드 종료. */
