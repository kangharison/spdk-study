/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] errno → 메시지 변환 헬퍼: TLS 버퍼 기반 spdk_strerror (strerror_tls.c)
 *
 * === 파일의 역할 ===
 * SPDK 전반에서 errno 코드를 사람이 읽을 수 있는 문자열로 변환할 때 호출되는
 * `spdk_strerror(int errnum)` 함수를 구현한다. 표준 C 라이브러리의 strerror()는
 * 정적 버퍼를 반환하기 때문에 멀티스레드 환경에서 다른 스레드의 호출과 경합하여
 * 문자열이 덮어써질 수 있다. 이 파일은 그 문제를 회피하기 위해 `__thread`
 * (Thread-Local Storage, TLS) 키워드로 스레드별 전용 버퍼를 두고, 그 버퍼에
 * strerror_r() 결과를 저장한 뒤 포인터를 반환한다. 그래서 호출자가 락 없이
 * 안전하게 사용할 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK는 polled-mode·lockless 설계로 각 reactor 스레드가 독립적으로 I/O를
 * 처리하지만, 에러 로깅 경로(SPDK_ERRLOG/SPDK_NOTICELOG)에서는 errno를
 * 문자열로 풀어 출력하는 일이 매우 잦다. 이 함수는 그런 모든 로깅 경로에서
 * 호출되는 가장 하위 유틸리티 중 하나이다. 호출 체인 예: NVMe ioctl 실패
 * → SPDK_ERRLOG("...: %s\n", spdk_strerror(errno)) → 본 함수.
 *
 * === 타 모듈과의 연결 ===
 * - 인터페이스: 공개 헤더 `include/spdk/string.h`가 prototype 선언.
 * - 구현 의존: `spdk_strerror_r()` (별도 컴파일 단위에서 OS별 분기로 구현,
 *   Linux의 GNU/POSIX strerror_r과 macOS/BSD의 strerror_r 차이를 흡수).
 * - 호출자: lib/nvme, lib/bdev, lib/sock, lib/util/fd.c 등 거의 모든 SPDK
 *   서브시스템의 에러 로그 경로. 본 함수가 반환하는 포인터는 같은 스레드의
 *   다음 spdk_strerror 호출 전까지만 유효하다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_strerror(int errnum): TLS 버퍼에 errnum 메시지를 채우고 그 포인터 반환.
 * - 전역 storage: `static __thread char strerror_message[64]` — 스레드마다
 *   독립적으로 인스턴스화되는 64바이트 메시지 버퍼.
 */

#include "spdk/string.h"
/* [한국어] spdk_strerror() / spdk_strerror_r() prototype 선언이 들어있는 SPDK
 * 공개 문자열 유틸 헤더. 이 헤더가 errno 처리 API의 외부 계약을 정의한다. */

static __thread char strerror_message[64];
/* [한국어] 스레드 로컬(TLS) 메시지 버퍼.
 * - `__thread`: GCC 확장 키워드. 각 OS 스레드가 독립된 인스턴스를 가지므로
 *   여러 reactor가 같은 시점에 spdk_strerror를 호출해도 서로의 결과가
 *   섞이지 않는다 (lockless 설계).
 * - 크기 64바이트: 일반적인 errno 메시지(예: "Operation not permitted",
 *   "No space left on device" 등)는 모두 이 안에 들어간다. 더 길어지면
 *   strerror_r 내부에서 잘릴 수 있으나 디버그 목적으로 충분.
 * - 라이프타임: 스레드가 살아 있는 동안 유효. 다음 spdk_strerror 호출 시
 *   같은 스레드 내에서 덮어써진다 → 호출 결과는 즉시 사용해야 한다. */

/*
 * [한국어]
 * spdk_strerror - errno 정수를 사람이 읽을 수 있는 메시지 문자열로 변환.
 *
 * @errnum: 변환할 errno 값 (일반적으로 시스템 호출 실패 직후의 errno).
 * @return: 메시지 문자열 포인터. 같은 스레드의 TLS 버퍼를 가리키므로
 *          호출자는 즉시 사용해야 하며, 다른 스레드로 넘기거나 free하면 안 된다.
 *
 * SPDK 로깅 매크로(SPDK_ERRLOG 등)에서 errno를 풀어쓰기 위한 표준 진입점이다.
 * 표준 strerror()의 비스레드안전성을 우회하기 위해 TLS 버퍼와 strerror_r()
 * 변형을 조합한다. POSIX/GNU 구현 차이는 spdk_strerror_r()이 흡수한다.
 *
 * 실행 컨텍스트: 임의의 SPDK 스레드/일반 pthread에서 모두 안전. 락 불필요.
 * 호출 체인:
 *   <SPDK 모듈 에러 경로> → spdk_strerror() → spdk_strerror_r() → strerror_r()
 */
const char *
spdk_strerror(int errnum)
{
	spdk_strerror_r(errnum, strerror_message, sizeof(strerror_message));
	/* [한국어] 스레드 로컬 버퍼에 errno 메시지를 안전하게 기록.
	 * sizeof로 길이를 전달해 잠재적 버퍼 오버플로를 방지한다.
	 * spdk_strerror_r은 OS/libc 별 strerror_r 변형(반환값이 int인 XSI vs
	 * char*인 GNU)을 추상화해, 항상 NUL-terminated 문자열을 보장한다. */
	return strerror_message;
	/* [한국어] TLS 버퍼 포인터 반환. 호출자가 printf 등에 즉시 사용해야 하며
	 * 다음 spdk_strerror 호출 전까지만 내용이 유효하다. */
}
