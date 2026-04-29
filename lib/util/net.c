/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

/*
 * [한국어 설명] 네트워크 어드레스 유틸리티 (net.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 가 소켓 fd 또는 IP 문자열로부터 인터페이스 이름, 주소,
 * 포트, loopback 여부 등의 메타정보를 조회하는 헬퍼 함수를 제공한다.
 * 주된 사용처는 NVMe-oF (TCP/RDMA) target 의 listen/connect 시점에서 (1)
 * 사용자 입력 IP 가 어느 NIC 에 바인딩되는지 결정, (2) 소켓의 local/peer
 * 주소·포트 추출, (3) 로컬 환경(loopback) 인지 판별해 정책(예: TLS 면제,
 * 디버그 로깅) 을 분기하는 것이다. 즉, 본 파일은 POSIX 소켓 API 의 SPDK
 * 친화적 얇은 wrapper 이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/util 의 leaf 유틸리티이며 thread-agnostic, 동기 호출 기반 (poll/epoll
 * 미사용). NVMe-oF transport 의 init/listen/accept 경로(주로 RPC 처리 또는
 * subsystem 시작 시)에서 호출되며, hot path(I/O submit/completion)에서는
 * 직접 사용되지 않는다. POSIX 표준 함수만 사용하므로 lockless 이며, 결과는
 * 호출자 스택 버퍼로 채워져 반환된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h(표준 C/POSIX 헤더), spdk/net.h(공개 API 선언),
 *         spdk/log.h(SPDK_ERRLOG), spdk/string.h(spdk_strerror).
 *         실제 시스템 호출: getifaddrs/freeifaddrs, getsockname, getpeername,
 *         getsockopt(SO_ACCEPTCONN), inet_ntop, ioctl(SIOCGIFFLAGS).
 * - 사용처: lib/nvmf/tcp.c, lib/nvmf/rdma.c 의 listen/accept,
 *           lib/sock 의 일부 진단 경로, RPC nvmf_create_transport 등.
 * - 데이터 흐름: 소켓 fd → kernel 의 socket 정보 조회 → 호출자 버퍼.
 *               IP 문자열 → 네트워크 인터페이스 목록 스캔 → 인터페이스 이름.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_net_get_interface_name : 주어진 IP(v4) 가 묶인 NIC 의 이름 조회.
 * - spdk_net_get_address_string : sockaddr → 사람이 읽는 IP 문자열 변환.
 * - spdk_net_is_loopback        : 소켓 fd 가 loopback NIC 에 묶여 있는지 검사.
 * - spdk_net_getaddr            : 소켓 fd 의 local/peer 주소+포트 추출 (IPv4/v6/UNIX).
 *
 * 본 파일은 별도 구조체를 정의하지 않으며, 모두 POSIX 표준 자료구조
 * (sockaddr_in/in6/storage, ifaddrs, ifreq) 를 직접 사용한다.
 */

/* [한국어] 표준 C/POSIX 헤더 묶음 (string.h, net/if.h, sys/socket.h, ifaddrs.h 등). */
#include "spdk/stdinc.h"
/* [한국어] 본 파일이 구현해야 할 외부 API 선언. */
#include "spdk/net.h"
/* [한국어] SPDK_ERRLOG 등 진단 매크로 — getsockname/getpeername 실패 시 로그. */
#include "spdk/log.h"
/* [한국어] spdk_strerror — errno 를 사람이 읽을 수 있는 문자열로. */
#include "spdk/string.h"

/*
 * [한국어]
 * spdk_net_get_interface_name - 주어진 IPv4 문자열이 바인딩된 NIC 이름 검색.
 *
 * @ip:     입력. 예: "192.168.1.10". IPv4 점-십진수 표기.
 * @ifc:    출력. NIC 이름이 NUL 종료 문자열로 채워짐 (예: "eth0").
 * @len:    ifc 버퍼 크기.
 * @return: 0 성공, -ENODEV 일치 NIC 없음, -ENOMEM 버퍼 부족.
 *
 * 동작: getifaddrs(3) 으로 시스템의 모든 인터페이스 주소 리스트를 받아,
 * UP 상태의 IPv4 주소만 inet_ntop 으로 문자열화 후 strcmp 로 비교.
 * 일치 항목을 찾으면 인터페이스 이름을 ifc 에 복사하고 종료.
 *
 * 사용 시점: NVMe-oF target 이 listen IP 를 받아 어느 NIC 에 등록할지
 * 결정할 때, 또는 RDMA 진입점 결정 시 부수 정보로 사용.
 *
 * 호출 체인: 상위(transport init) → spdk_net_get_interface_name → getifaddrs/inet_ntop
 */
int
spdk_net_get_interface_name(const char *ip, char *ifc, size_t len)
{
	/* [한국어] addrs: getifaddrs 가 반환하는 연결 리스트의 헤드.
	 * iap: 리스트 순회 포인터. */
	struct ifaddrs *addrs, *iap;
	/* [한국어] AF_INET 의 sockaddr 캐스트 결과 — sin_addr 접근용. */
	struct sockaddr_in *sa;
	/* [한국어] inet_ntop 출력 버퍼 — IPv4 문자열은 최대 16 바이트지만 32 로 여유. */
	char buf[32];
	/* [한국어] 기본 결과는 "디바이스 없음" — 일치 못 찾으면 그대로 반환. */
	int rc = -ENODEV;

	/* [한국어] 시스템 NIC 주소 리스트 획득 — 실패해도 addrs는 NULL이면 루프가 돌지 않음.
	 *  주의: 호출 후에는 freeifaddrs 로 반드시 해제 필요. */
	getifaddrs(&addrs);
	for (iap = addrs; iap != NULL; iap = iap->ifa_next) {
		/* [한국어] 다음 항목으로 건너뛸 조건:
		 *  - ifa_addr 이 NULL (가상 인터페이스)
		 *  - 인터페이스가 DOWN (IFF_UP 미설정)
		 *  - IPv4 가 아님 (현재 함수는 IPv4 만 비교)
		 *  주: !(A && B && C) 형태로 묶어 모두 만족할 때만 통과. */
		if (!(iap->ifa_addr && (iap->ifa_flags & IFF_UP) && iap->ifa_addr->sa_family == AF_INET)) {
			continue;
		}
		/* [한국어] sockaddr → sockaddr_in 캐스팅으로 sin_addr 획득. */
		sa = (struct sockaddr_in *)(iap->ifa_addr);
		/* [한국어] 32비트 IPv4 주소를 점-십진수 문자열로 변환. */
		inet_ntop(iap->ifa_addr->sa_family, &sa->sin_addr, buf, sizeof(buf));
		/* [한국어] 입력 ip 와 정확히 일치할 때만 다음 단계로. */
		if (strcmp(ip, buf) != 0) {
			continue;
		}
		/* [한국어] 인터페이스 이름이 호출자 버퍼에 들어가지 못하면 ENOMEM. */
		if (strnlen(iap->ifa_name, len) == len) {
			rc = -ENOMEM;
			goto ret;
		}
		/* [한국어] NIC 이름을 ifc 에 NUL 종료 형태로 복사 — snprintf 가 안전. */
		snprintf(ifc, len, "%s", iap->ifa_name);
		rc = 0;
		break;
	}
ret:
	/* [한국어] getifaddrs 가 할당한 리스트 메모리 반환 — 누수 방지 필수. */
	freeifaddrs(addrs);
	return rc;
}

/*
 * [한국어]
 * spdk_net_get_address_string - sockaddr → IP 문자열(IPv4/v6) 변환.
 *
 * @sa:     입력. AF_INET 또는 AF_INET6 의 sockaddr.
 * @addr:   출력 버퍼.
 * @len:    addr 버퍼 크기 (INET6_ADDRSTRLEN 이상 권장).
 * @return: 0 성공, -EINVAL 인자 오류, -errno inet_ntop 실패.
 *
 * 단순한 family 분기 후 inet_ntop 위임. 다른 함수들의 공통 빌딩 블록.
 */
int
spdk_net_get_address_string(struct sockaddr *sa, char *addr, size_t len)
{
	/* [한국어] inet_ntop 결과 — 성공 시 addr 와 동일, 실패 시 NULL. */
	const char *result = NULL;

	/* [한국어] NULL 포인터는 즉시 거절. */
	if (sa == NULL || addr == NULL) {
		return -EINVAL;
	}

	/* [한국어] 주소 패밀리에 따라 sin_addr 또는 sin6_addr 에서 변환. */
	switch (sa->sa_family) {
	case AF_INET:
		/* [한국어] IPv4 — 32비트 주소 → "x.x.x.x". */
		result = inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr),
				   addr, len);
		break;
	case AF_INET6:
		/* [한국어] IPv6 — 128비트 주소 → "xxxx:...". */
		result = inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr),
				   addr, len);
		break;
	default:
		/* [한국어] 지원되지 않는 family — result 는 NULL 그대로 → -errno 반환 경로. */
		break;
	}

	/* [한국어] inet_ntop 실패 또는 미지원 family 처리 — errno 보존해 반환. */
	if (result == NULL) {
		return -errno;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_net_is_loopback - 소켓 fd 가 loopback NIC 에 묶여 있는지 판정.
 *
 * @fd:     입력. 검사 대상 소켓 fd.
 * @return: true 면 loopback (예: 127.0.0.1, ::1), false 면 외부 NIC 또는 오류.
 *
 * 동작:
 *   1) getsockname 으로 fd 의 local 주소 조회.
 *   2) 그 IP 문자열을 모든 NIC 의 IP 와 비교.
 *   3) 매칭된 NIC 의 IFF_LOOPBACK 플래그를 ioctl(SIOCGIFFLAGS) 로 조회.
 *
 * 사용 시점: NVMe-oF 가 클라이언트 연결을 받은 직후 보안 정책(예: 같은
 * 호스트 연결이라 TLS 비활성, 디버그 로그 추가) 결정에 활용.
 *
 * 동시성: 시스템 호출이 다수 호출되지만 fd 자체는 호출자 보유 자원이므로
 * 호출자 스레드에서만 다루면 안전.
 */
bool
spdk_net_is_loopback(int fd)
{
	/* [한국어] addrs/tmp: 모든 NIC 주소 리스트와 순회 포인터. */
	struct ifaddrs *addrs, *tmp;
	/* [한국어] getsockname 출력용 sockaddr_storage(IPv4/v6 모두 수용). 모든 비트 0 으로 초기화. */
	struct sockaddr_storage sa = {};
	/* [한국어] sockaddr 길이 — getsockname 입출력 인자. */
	socklen_t salen;
	/* [한국어] ioctl 의 in/out 인자 — ifr_name 을 채우고 ifr_flags 를 받음. */
	struct ifreq ifr = {};
	/* [한국어] fd 측 IP 문자열과 비교 대상 NIC IP 문자열. */
	char ip_addr[256], ip_addr_tmp[256];
	int rc;
	/* [한국어] 결과 — 실패 시에도 false 로 안전하게 반환. */
	bool is_loopback = false;

	/* [한국어] 1단계: fd 의 local 주소 조회. */
	salen = sizeof(sa);
	rc = getsockname(fd, (struct sockaddr *)&sa, &salen);
	if (rc != 0) {
		return is_loopback;
	}

	/* [한국어] sa → "x.x.x.x" or "::1" 등 문자열로 변환해 비교 키로 사용. */
	memset(ip_addr, 0, sizeof(ip_addr));
	rc = spdk_net_get_address_string((struct sockaddr *)&sa, ip_addr, sizeof(ip_addr));
	if (rc != 0) {
		return is_loopback;
	}

	/* [한국어] 2단계: 모든 NIC 의 주소 리스트 획득. 실패 시 free 불필요. */
	rc = getifaddrs(&addrs);
	if (rc != 0) {
		return is_loopback;
	}

	/* [한국어] 3단계: 모든 NIC 을 돌며 family 가 같고 IP 가 일치하는 항목 검색. */
	for (tmp = addrs; tmp != NULL; tmp = tmp->ifa_next) {
		if (tmp->ifa_addr && (tmp->ifa_flags & IFF_UP) &&
		    (tmp->ifa_addr->sa_family == sa.ss_family)) {
			/* [한국어] 비교용 임시 버퍼 클리어 후 NIC IP 문자열화. */
			memset(ip_addr_tmp, 0, sizeof(ip_addr_tmp));
			rc = spdk_net_get_address_string(tmp->ifa_addr, ip_addr_tmp, sizeof(ip_addr_tmp));
			if (rc != 0) {
				continue;
			}

			/* [한국어] fd 측 IP 와 NIC IP 가 같으면 — 그 NIC 가 fd 가 묶인 NIC. */
			if (strncmp(ip_addr, ip_addr_tmp, sizeof(ip_addr)) == 0) {
				/* [한국어] ioctl 인자 ifr_name 에 NIC 이름 복사. */
				memcpy(ifr.ifr_name, tmp->ifa_name, sizeof(ifr.ifr_name));
				/* [한국어] SIOCGIFFLAGS — ifr_flags 에 NIC 플래그 비트 채워짐. */
				ioctl(fd, SIOCGIFFLAGS, &ifr);
				/* [한국어] IFF_LOOPBACK 비트가 켜져 있으면 loopback NIC 확정. */
				if (ifr.ifr_flags & IFF_LOOPBACK) {
					is_loopback = true;
				}
				goto end;
			}
		}
	}

end:
	/* [한국어] getifaddrs 메모리 반환 — 누수 방지. */
	freeifaddrs(addrs);
	return is_loopback;
}

/*
 * [한국어]
 * spdk_net_getaddr - 소켓 fd 의 local/peer 주소+포트 일괄 추출.
 *
 * @fd:    입력. 추출 대상 소켓.
 * @laddr, @llen, @lport: local 주소 문자열/버퍼 길이/포트(host order). NULL 이면 skip.
 * @paddr, @plen, @pport: peer 주소 문자열/버퍼 길이/포트.  NULL 이면 skip.
 * @return: 0 성공, -EINVAL 인자/listening 소켓에 peer 요청, -errno 시스템 호출 실패.
 *
 * 동작:
 *   1) getsockname 으로 local sockaddr 조회. AF_UNIX 면 IP/포트 의미 없으므로 0 반환.
 *   2) AF_INET/INET6 만 지원. local 주소/포트 채움.
 *   3) SO_ACCEPTCONN 으로 fd 가 listen 소켓인지 확인 — listen 소켓에는 peer 가 없음.
 *   4) listen 이 아니면 getpeername 으로 peer 주소 채움.
 *
 * 사용 시점: NVMe-oF 가 새 연결을 수락한 직후 진단 로그/통계 출력,
 * 또는 RPC 응답에 연결 정보를 포함시킬 때.
 */
int
spdk_net_getaddr(int fd, char *laddr, int llen, uint16_t *lport,
		 char *paddr, int plen, uint16_t *pport)
{
	/* [한국어] IPv4/v6 모두 받을 수 있는 충분한 크기의 저장소. */
	struct sockaddr_storage sa;
	/* [한국어] getsockopt(SO_ACCEPTCONN) 결과를 받을 임시 변수. */
	int val;
	/* [한국어] sockaddr 길이 — 시스템 호출 입출력. */
	socklen_t len;
	int rc;

	/* [한국어] 1단계: local sockaddr 조회 (clean state 부터 시작). */
	memset(&sa, 0, sizeof(sa));
	len = sizeof(sa);
	rc = getsockname(fd, (struct sockaddr *)&sa, &len);
	if (rc != 0) {
		/* [한국어] 진단 로그 후 -errno 반환 — 호출자가 errno 분석 가능. */
		SPDK_ERRLOG("getsockname() failed, rc %d: %s\n", rc, spdk_strerror(errno));
		return -errno;
	}

	/* [한국어] family 별 분기 — UNIX 소켓은 IP/포트 없음, IP 군만 진행. */
	switch (sa.ss_family) {
	case AF_UNIX:
		/* Acceptable connection types that don't have IPs */
		/* [한국어] UNIX 도메인 소켓: 정상 케이스로 0 반환 (laddr/lport 안 채움). */
		return 0;
	case AF_INET:
	case AF_INET6:
		/* Code below will get IP addresses */
		/* [한국어] IP 패밀리는 아래 로직으로 진행. */
		break;
	default:
		/* Unsupported socket family */
		/* [한국어] 그 외(AF_NETLINK 등) 는 지원하지 않음. */
		return -EINVAL;
	}

	/* [한국어] 2단계 (local addr): 호출자가 요청한 경우만 변환. */
	if (laddr) {
		rc = spdk_net_get_address_string((struct sockaddr *)&sa, laddr, llen);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_net_get_address_string() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
			return rc;
		}
	}

	/* [한국어] local port: family 별로 sin_port/sin6_port 위치가 달라 분기.
	 * ntohs: 네트워크 byte order(BE) → host order. */
	if (lport) {
		if (sa.ss_family == AF_INET) {
			*lport = ntohs(((struct sockaddr_in *)&sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*lport = ntohs(((struct sockaddr_in6 *)&sa)->sin6_port);
		}
	}

	/* [한국어] 3단계: SO_ACCEPTCONN — fd 가 listen 상태인지 확인. */
	len = sizeof(val);
	rc = getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &val, &len);
	if (rc == 0 && val == 1) {
		/* It is an error to getaddr for a peer address on a listen socket. */
		/* [한국어] listen 소켓에는 peer 가 없음 — peer 요청 시 인자 오류. */
		if (paddr != NULL || pport != NULL) {
			SPDK_ERRLOG("paddr, pport not valid on listen sockets\n");
			return -EINVAL;
		}
		/* [한국어] peer 정보 요청이 없었으면 정상 종료. */
		return 0;
	}

	/* [한국어] 4단계 (peer addr): 호출자가 paddr/pport 중 하나라도 요청했으면 getpeername. */
	if (paddr || pport) {
		memset(&sa, 0, sizeof(sa));
		len = sizeof(sa);
		rc = getpeername(fd, (struct sockaddr *)&sa, &len);
		if (rc != 0) {
			SPDK_ERRLOG("getpeername() failed, rc %d: %s\n", rc, spdk_strerror(errno));
			return -errno;
		}
	}

	/* [한국어] peer addr 문자열화 — 요청한 경우만. */
	if (paddr) {
		rc = spdk_net_get_address_string((struct sockaddr *)&sa, paddr, plen);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_net_get_address_string() failed, rc %d: %s\n", rc, spdk_strerror(-rc));
			return rc;
		}
	}

	/* [한국어] peer port 추출 — local 과 동일한 ntohs 변환. */
	if (pport) {
		if (sa.ss_family == AF_INET) {
			*pport = ntohs(((struct sockaddr_in *)&sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*pport = ntohs(((struct sockaddr_in6 *)&sa)->sin6_port);
		}
	}

	return 0;
}
