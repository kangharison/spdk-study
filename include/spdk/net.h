/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Samsung Electonrics Co., Ltd.
 *   All rights reserved.
 */

/** \file
 * Network related helper functions
 */

/*
 * [한국어 설명] SPDK 네트워크 보조 함수 공개 헤더 (net.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 전반에서 공통적으로 필요한 네트워크/소켓 관련 보조 유틸리티
 * 함수들의 공개 API 를 선언한다. 구체적으로는 (1) 특정 IP 주소가 어느 OS
 * 네트워크 인터페이스에 바인딩되어 있는지를 찾는 함수, (2) struct sockaddr 을
 * 사람이 읽을 수 있는 IP 주소 문자열로 변환하는 함수, (3) 주어진 파일
 * 디스크립터(fd) 가 loopback 인터페이스에 연결되어 있는지 확인하는 함수,
 * (4) 연결된 fd 의 로컬/피어 주소 및 포트를 한 번에 얻는 함수가 정의되어
 * 있다. 이 함수들은 모두 OS 의 표준 BSD 소켓 API (getsockname/getpeername/
 * getifaddrs/inet_ntop 등) 를 SPDK 코드 컨벤션 (errno 음수 반환, 길이 인자
 * 검증) 에 맞게 래핑한다. SPDK 의 다른 영역(NVMe-oF TCP transport, sock
 * 레이어, RPC 서버, 텔레메트리 등) 에서 디버그 로그·연결 정보 보고·
 * 인터페이스 매칭 등에 폭넓게 사용된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 의 I/O 스택은 [App] -> [bdev] -> [bdev_module] -> [드라이버
 * (NVMe/AIO/...)] 의 수직 흐름이지만, 그와 별개로 네트워크 transport 가
 * 수평 축으로 존재한다 (NVMe-oF TCP, vhost-user-blk-net, RPC JSON-RPC over
 * TCP/Unix). 이 헤더가 선언하는 함수들은 그 네트워크 축의 가장 하단에서
 * 호스트 OS 인터페이스/주소 정보를 얻어오는 "엽서 같은" 도구이며, sock
 * 레이어 (lib/sock) 보다 더 아래의 OS 어댑터 위치에 있다. 호출자는 보통
 * 유저스페이스 SPDK reactor 스레드에서 실행되며, 시스템 호출 (socket,
 * getifaddrs) 을 동기적으로 호출하므로 polling hot path 가 아닌 초기화
 * 또는 관리 경로에서만 사용해야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: 표준 C/POSIX 헤더와 OS 의 BSD 소켓 API. 또한 spdk/stdinc.h 를 통해
 * 표준 정수형/sys/socket.h 가 로드된다. 의존하는 SPDK 자체 자료구조는
 * 없다 (헤더 단에서는 struct sockaddr 만 사용). 사용처: lib/sock (각 sock
 * 구현이 fd 의 로컬/피어 주소 보고에 사용), lib/nvmf/tcp.c (TCP transport
 * 가 listen/connect 시 인터페이스 이름 매칭), lib/rpc (JSON-RPC 서버가
 * 클라이언트 주소를 로깅), 그리고 RPC handler 들이 노출하는 텔레메트리.
 * 데이터 흐름은 OS 커널의 ifaddrs / socket name 정보가 이 헤더의 함수를
 * 거쳐 SPDK 측 문자열 버퍼로 복사된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_net_get_interface_name(ip, ifc, len): 주어진 IP 주소가 호스트의
 *   어느 NIC 에 바인딩되어 있는지 인터페이스 이름을 찾아 반환.
 * - spdk_net_get_address_string(sa, addr, len): struct sockaddr 을 IPv4/v6
 *   문자열 표현으로 변환 (inet_ntop 의 SPDK 친화적 래퍼).
 * - spdk_net_is_loopback(fd): 연결된 fd 가 lo (127.0.0.1/::1) 인지 검사.
 * - spdk_net_getaddr(fd, laddr, llen, lport, paddr, plen, pport): fd 의
 *   로컬/피어 주소·포트를 한 번에 추출 (getsockname + getpeername).
 * 구조체는 정의하지 않으며, 모두 OS 표준 struct sockaddr 와 사용자 제공
 * char 버퍼 (write-back 출력 매개변수) 를 받는다.
 */

/* [한국어] SPDK_NET_H — 다중 #include 시 중복 선언을 막기 위한 가드.
 * 이 헤더는 다른 헤더에 의해 (sock 레이어, RPC) 간접적으로도 포함되므로
 * 가드는 필수이다. 매크로 이름은 헤더 파일명을 대문자화한 SPDK 컨벤션. */
#ifndef SPDK_NET_H
#define SPDK_NET_H

/* [한국어] spdk/stdinc.h - SPDK 의 "standard include" 집합 헤더.
 * stdint.h, stdbool.h, stddef.h, sys/socket.h, sys/types.h, netinet/in.h
 * 등 자주 쓰이는 표준 헤더를 한 번에 끌어와 ABI 일관성을 보장한다.
 * 이 파일은 struct sockaddr / size_t / uint16_t / bool 등을 사용하므로
 * 반드시 이 통합 헤더가 필요하다. */
#include "spdk/stdinc.h"

/* [한국어] C++ 컴파일러가 이 헤더를 포함할 때 함수 심볼이 C++ 이름
 * 망글링되는 것을 막기 위한 extern "C" 가드. SPDK 는 C 라이브러리지만
 * C++ 애플리케이션에서도 호출되므로 모든 공개 헤더에 동일 패턴이 있다. */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * [한국어]
 * spdk_net_get_interface_name - 주어진 IP 문자열이 바인딩된 OS 네트워크
 *                               인터페이스 이름을 조회.
 *
 * @ip: 조회할 IP 주소 문자열 (IPv4 dotted-quad 또는 IPv6 표기). NULL 이거나
 *      유효하지 않은 형식이면 -ENODEV 또는 -EINVAL 이 반환될 수 있다.
 * @ifc: 결과 인터페이스 이름이 복사될 사용자 버퍼. 호출자가 미리 할당.
 * @len: ifc 버퍼의 바이트 길이. IFNAMSIZ (보통 16) 이상을 권장.
 * @return: 0 성공 (ifc 에 인터페이스 이름이 NUL-terminated 로 복사됨),
 *          -ENODEV 일치하는 인터페이스가 없음,
 *          -ENOMEM ifc 버퍼가 인터페이스 이름을 담기에 너무 작음.
 *
 * 동기/배경: NVMe-oF TCP transport 가 사용자가 지정한 listen address 에
 * 대해 SO_BINDTODEVICE 같은 옵션을 적용하거나, RPC 가 어느 NIC 에서 받은
 * 요청인지 로깅할 때, IP 주소만 가지고 인터페이스 이름이 필요한 경우가
 * 자주 발생한다. 매번 직접 getifaddrs 루프를 도는 대신 이 헬퍼를 호출해
 * 코드 중복을 피한다.
 *
 * 동작: 내부적으로 getifaddrs(3) 로 OS 의 모든 네트워크 인터페이스 주소
 * 목록을 받아오고, 각 항목의 ifa_addr 을 사용자 입력 ip 와 비교하여
 * 일치하는 인터페이스의 ifa_name 을 ifc 로 strncpy 한다.
 *
 * 실행 컨텍스트: 임의의 SPDK 스레드. 단, getifaddrs 는 동기 시스템 호출이
 * 므로 polling hot path 에서 호출하지 말 것. 초기화/RPC 핸들러에서만 사용.
 *
 * 호출 체인:
 *   nvmf_tcp_listen / RPC 핸들러 → [spdk_net_get_interface_name] →
 *   getifaddrs / inet_pton / strncpy
 */
int spdk_net_get_interface_name(const char *ip, char *ifc, size_t len);

/*
 * [한국어]
 * spdk_net_get_address_string - struct sockaddr 을 사람이 읽을 수 있는
 *                               IP 주소 문자열로 변환.
 *
 * @sa: 변환할 sockaddr 포인터. AF_INET / AF_INET6 둘 다 지원되어야 하며,
 *      sa_family 가 그 외 값이면 음수 errno 가 반환된다.
 * @addr: 변환된 주소가 NUL-terminated 로 복사될 사용자 버퍼.
 * @len: addr 버퍼의 바이트 길이. INET6_ADDRSTRLEN (=46) 이상을 권장.
 * @return: 0 성공 / 음수 errno 실패 (예: -EINVAL 비지원 family,
 *          -ENOMEM 버퍼 부족).
 *
 * 동기/배경: SPDK 코드 곳곳에서 fd 의 로컬/피어 주소를 로깅하거나 JSON-RPC
 * 응답에 담아 클라이언트에 돌려줄 때, struct sockaddr_storage 를 받아
 * 문자열로 변환하는 패턴이 빈번하다. inet_ntop 의 family 분기를 일일이
 * 작성하는 보일러플레이트를 제거하기 위한 thin wrapper.
 *
 * 동작: sa->sa_family 를 보고 AF_INET 이면 sin_addr, AF_INET6 이면
 * sin6_addr 을 inet_ntop(3) 으로 addr 에 출력한다.
 *
 * 실행 컨텍스트: 어느 스레드에서나 호출 가능. 시스템 호출이 아닌 순수
 * 변환 함수이므로 hot path 에서도 안전하지만, 보통 디버그/관리 경로에서만 사용.
 *
 * 호출 체인:
 *   spdk_net_getaddr / 사용자 코드 → [spdk_net_get_address_string] →
 *   inet_ntop
 */
int spdk_net_get_address_string(struct sockaddr *sa, char *addr, size_t len);

/*
 * [한국어]
 * spdk_net_is_loopback - 주어진 소켓 fd 가 loopback 인터페이스에
 *                        연결되어 있는지 판정.
 *
 * @fd: 연결된 (혹은 listen 상태의) 소켓 파일 디스크립터. -1 이거나
 *      비-소켓이면 false 가 반환된다.
 * @return: true = loopback (127.0.0.1, ::1, 또는 lo 인터페이스),
 *          false = 그 외 또는 판정 실패.
 *
 * 동기/배경: NVMe-oF TCP / RPC 등에서 인증·암호화 정책을 결정할 때
 * "이 연결은 호스트 내부에서 들어왔는가?" 를 판단해야 할 수 있다.
 * 예를 들어 loopback 일 때만 RPC 의 권한 검사를 완화하는 등의 용도.
 *
 * 동작: getsockname / getpeername 으로 fd 의 주소를 받아 IPv4 의 경우
 * 127.0.0.0/8, IPv6 의 경우 ::1 인지 비교한다. 또는 인터페이스 이름이
 * "lo" 인지를 확인할 수도 있다.
 *
 * 실행 컨텍스트: 어느 SPDK 스레드에서나 호출 가능하지만, getsockname 은
 * 동기 시스템 호출이므로 신규 연결 수락 시점 등에 1 회만 호출하는 것이 권장.
 *
 * 호출 체인:
 *   accept loop / RPC 인증 → [spdk_net_is_loopback] →
 *   getsockname / getpeername
 */
bool spdk_net_is_loopback(int fd);

/*
 * [한국어]
 * spdk_net_getaddr - 연결된 소켓 fd 의 로컬·피어 주소 및 포트를 한 번에 조회.
 *
 * @fd: 연결된 소켓 fd. listen 상태에서는 의미 있는 peer 가 없으므로
 *      paddr/pport 가 비워질 수 있다.
 * @laddr: (out) 로컬 IP 문자열을 받을 버퍼. NULL 이면 무시.
 * @llen: laddr 버퍼 바이트 길이.
 * @lport: (out) 로컬 포트(host byte order) 를 받을 포인터. NULL 이면 무시.
 * @paddr: (out) 피어 IP 문자열을 받을 버퍼. NULL 이면 무시.
 * @plen: paddr 버퍼 바이트 길이.
 * @pport: (out) 피어 포트(host byte order) 를 받을 포인터. NULL 이면 무시.
 * @return: 0 성공 / 음수 errno (getsockname/getpeername 실패 시).
 *
 * 동기/배경: NVMe-oF TCP 와 RPC 에서 신규 연결을 수락할 때, "어디로
 * 들어왔고(local) 누구로부터 왔는지(peer)" 를 한 번에 얻어 로깅·통계에
 * 사용하는 것이 매우 흔하다. getsockname/getpeername 호출과 sockaddr_storage
 * → 문자열 변환 보일러플레이트를 한 함수로 캡슐화한다.
 *
 * 동작:
 *   1) getsockname(fd, &local) 로 로컬 주소를 얻는다.
 *   2) getpeername(fd, &peer) 로 피어 주소를 얻는다.
 *   3) 사용자가 NULL 이 아닌 출력 버퍼를 준 경우에만 spdk_net_get_address_string
 *      을 사용해 문자열로 변환하고, 포트는 ntohs 로 변환해 lport/pport 에 기록.
 *
 * 실행 컨텍스트: connection accept 직후의 setup 단계에서 1 회 호출하는 용도.
 * getsockname/getpeername 은 빠르지만 시스템 호출이므로 hot path 에서 반복
 * 호출하지 말 것.
 *
 * 호출 체인:
 *   sock accept callback / nvmf_tcp_listen → [spdk_net_getaddr] →
 *   getsockname → getpeername → spdk_net_get_address_string
 */
int spdk_net_getaddr(int fd, char *laddr, int llen, uint16_t *lport,
		     char *paddr, int plen, uint16_t *pport);

/* [한국어] extern "C" 블록 종료 — C++ 컴파일러용 가드 닫기. */
#ifdef __cplusplus
}
#endif

#endif /* SPDK_NET_H */
/* [한국어] 헤더 가드 종료 마커. SPDK 컨벤션에 따라 #endif 옆에 매크로
 * 이름 주석을 붙여 매칭을 명확히 한다. */
