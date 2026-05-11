/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] DPDK PCI hotplug 이벤트 모니터 (pci_event.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK가 런타임에 발생하는 PCI 디바이스의 hotplug(핫플러그) 이벤트를
 * 감지하기 위해 리눅스 커널의 udev/uevent 메커니즘에 연결하는 역할을 한다.
 * 구체적으로는 NETLINK_KOBJECT_UEVENT 패밀리의 netlink 소켓을 열어 커널이
 * 브로드캐스트하는 디바이스 이벤트 메시지(예: "ACTION=add SUBSYSTEM=uio
 * DEVPATH=...")를 수신·파싱하고, 이를 SPDK 내부의 표준 형식인
 * struct spdk_pci_event 로 변환하여 상위 레이어(NVMe 드라이버, bdev 모듈
 * 등)가 NVMe SSD의 attach/detach 를 감지·처리할 수 있게 한다.
 * uio(uio_pci_generic) 와 vfio-pci 두 가지 드라이버에서 발생하는 이벤트만
 * 추출하며, 그 외 서브시스템(usb, block, net 등)의 이벤트는 무시한다.
 * 본 구현은 리눅스 전용이며 비-리눅스 플랫폼에서는 모두 -ENOTSUP 를 반환한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 의 환경 추상화(env)는 lib/env_dpdk 디렉토리에 모여 있고, 그 안에서
 * 본 파일은 init.c(EAL 초기화), pci.c(PCI 디바이스 attach), memory.c(메모리
 * 풀) 와 함께 "런타임 디바이스 라이프사이클 관리"의 일부를 담당한다.
 * 호출 체인 관점에서 보면:
 *   상위 레이어(예: bdev_nvme 모듈) → spdk_pci_event_listen() 으로 fd 획득
 *   → 자기 SPDK thread 의 poller 등록 → 매 polling tick 마다
 *   spdk_pci_get_event() 호출 → 이벤트가 있으면 적절한 attach/detach 콜백 실행.
 * 실행 컨텍스트는 호스트 유저스페이스이며, 일반적으로 hotplug 를 등록한
 * 특정 SPDK reactor 스레드에서 polled-mode 로 호출된다(인터럽트 기반 X).
 *
 * === 타 모듈과의 연결 ===
 * - 의존하는 모듈:
 *     spdk/log.h         : SPDK_ERRLOG 등 로그 매크로
 *     spdk/string.h      : spdk_fd_set_nonblock(), spdk_str_trim() 등 문자열/fd 유틸
 *     spdk/util.h        : SPDK_COUNTOF, container_of 등 매크로
 *     spdk/env.h         : struct spdk_pci_event, spdk_pci_addr_parse(),
 *                          SPDK_UEVENT_ADD/REMOVE 정의
 *     <linux/netlink.h>  : sockaddr_nl, NETLINK_KOBJECT_UEVENT 정의
 *     리눅스 커널 udev   : 실질적 데이터 공급원 (sysfs uevent broadcast)
 * - 의존받는 모듈:
 *     module/bdev/nvme   : NVMe SSD attach/detach 자동 처리
 *     lib/nvme(pcie)     : remove uevent 시 컨트롤러 강제 제거 처리
 *     SPDK rpc("bdev_nvme_set_hotplug")
 * - 데이터 흐름:
 *     커널 hot-plug 발생 → kobject_uevent → netlink broadcast →
 *     spdk_pci_get_event() recv() → parse_subsystem_event() → 호출자에게
 *     struct spdk_pci_event {action, traddr} 전달 → bdev_nvme 가 BDF 로
 *     spdk_nvme_probe()/spdk_nvme_detach() 호출.
 * - 공유 자료구조: 본 파일은 전역 상태를 갖지 않으며, 모든 상태는 호출자가
 *   보유한 fd 와 호출별로 전달되는 spdk_pci_event 구조체에 한정된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_pci_event_listen()  : NETLINK_KOBJECT_UEVENT 소켓을 열고 0xffffffff
 *                              그룹(전체 그룹) 으로 바인딩하여 fd 반환.
 *                              논블로킹 + 1MB 수신버퍼로 구성.
 * - spdk_pci_get_event(fd, *event)
 *                            : recv() 로 한 번에 하나의 uevent 메시지를
 *                              수신해 parse_subsystem_event() 로 위임.
 *                              0=관심 없는 이벤트, 1=유효한 add/remove,
 *                              -errno=오류, -ENOTCONN=소켓 종료.
 * - parse_subsystem_event() (static)
 *                            : NUL 로 구분된 KEY=VALUE 멀티라인 메시지를
 *                              파싱해 SUBSYSTEM/ACTION/DEVPATH/DRIVER/
 *                              PCI_SLOT_NAME 을 추출하고, uio remove/add
 *                              또는 vfio-pci bind 만 SPDK 이벤트로 변환.
 * - SPDK_UEVENT_MSG_LEN      : 4096B – 단일 uevent 메시지 버퍼 길이.
 * - SPDK_UEVENT_RECVBUF_SIZE : 1MB – 소켓 수신 큐 크기(폭주 방지).
 */

#include "spdk/stdinc.h"   /* [한국어] 표준 C 헤더(stdint, stdio, stdlib, string, errno 등)를 SPDK 가 일괄 노출하는 어그리게이터 헤더. 시스템별 차이를 흡수한다. */
#include "spdk/string.h"   /* [한국어] spdk_fd_set_nonblock() 등 SPDK 의 보조 문자열/디스크립터 헬퍼를 사용하기 위해 포함. */

#include "spdk/log.h"      /* [한국어] SPDK_ERRLOG/SPDK_WARNLOG 매크로 — 본 파일에서 에러를 사용자 콘솔/로그로 보고하기 위해 필요. */
#include "spdk/env.h"      /* [한국어] struct spdk_pci_event, spdk_pci_addr_t, spdk_pci_addr_parse(), SPDK_UEVENT_ADD/REMOVE enum 등 상위 API 가 정의된 공개 헤더. */
#include "spdk/util.h"     /* [한국어] SPDK_COUNTOF 등 일반 유틸 매크로. (현재 본 파일에서 직접 사용하지는 않으나 공통 의존성을 위해 포함됨.) */

#ifdef __linux__           /* [한국어] netlink 기반 udev 리스닝은 리눅스 커널 고유 기능이므로 리눅스에서만 컴파일한다. 그 외 OS 는 ifdef 의 #else 분기에서 ENOTSUP stub 을 제공한다. */

#include <linux/netlink.h> /* [한국어] sockaddr_nl 구조체와 NETLINK_KOBJECT_UEVENT 패밀리 상수 정의. AF_NETLINK 소켓을 다루는 데 필수. */

/* [한국어] 단일 uevent 메시지의 최대 길이.
 * 리눅스 커널은 uevent 를 16KB 까지 보낼 수 있지만 PCI/uio/vfio 이벤트는
 * 실측상 4KB 이내이므로 SPDK 는 4096 으로 제한한다. recv() 시 truncation
 * 위험을 줄이기 위해 buf 크기를 이 값과 동일하게 둔다. */
#define SPDK_UEVENT_MSG_LEN 4096

/* [한국어] 소켓 수신 큐(SO_RCVBUF) 크기 = 1MB.
 * hotplug 폭주(예: rescan 시 동시 다발 add 이벤트) 시에도 커널이 메시지를
 * 드랍하지 않도록 충분히 크게 잡는다. 시스템 net.core.rmem_max 가 작으면
 * setsockopt 가 실패할 수 있어 SO_RCVBUFFORCE 를 먼저 시도한다(아래 참조). */
#define SPDK_UEVENT_RECVBUF_SIZE 1024 * 1024

/*
 * [한국어]
 * spdk_pci_event_listen - PCI uevent 수신용 netlink 소켓을 열고 fd 반환
 *
 * @return: 성공 시 비-블록 모드의 NETLINK_KOBJECT_UEVENT 소켓 fd (>=0).
 *          실패 시 음수 errno (예: -EACCES, -ENOSPC).
 *
 * 이 함수는 SPDK 의 hotplug poller 가 호출해 커널 udev 메시지 스트림에
 * "구독"한다. 절차:
 *   1) PF_NETLINK + SOCK_DGRAM + NETLINK_KOBJECT_UEVENT 패밀리 소켓 생성
 *   2) SO_RCVBUFFORCE → SO_RCVBUF 순서로 수신 버퍼 1MB 설정
 *      (FORCE 변형은 CAP_NET_ADMIN 필요, 실패해도 일반 RCVBUF 로 후퇴)
 *   3) O_NONBLOCK 설정 — polled-mode 에서 recv() 블록을 막아야 함
 *   4) addr.nl_groups = 0xffffffff 로 모든 멀티캐스트 그룹 구독, bind()
 *
 * 실행 컨텍스트: SPDK env 초기화 직후 또는 bdev_nvme hotplug 활성화 시
 *   임의의 SPDK 스레드에서 한 번 호출된다. 동시성: 동일 fd 에 대한 수신은
 *   소유 SPDK 스레드 1개에서만 이루어지므로 별도 락 불필요.
 *
 * 호출 체인:
 *   spdk_bdev_nvme_set_hotplug() → hotplug poller register → spdk_pci_event_listen()
 *   → socket(2) / setsockopt(2) / fcntl(2) / bind(2)
 */
int
spdk_pci_event_listen(void)
{
	struct sockaddr_nl addr;        /* [한국어] netlink bind 용 주소 구조체. nl_family/nl_pid/nl_groups 만 사용. */
	int netlink_fd;                 /* [한국어] 소켓(2) 가 반환하는 파일 디스크립터 — 호출자가 받아 poller 에 등록. */
	int size = SPDK_UEVENT_RECVBUF_SIZE;  /* [한국어] setsockopt 인자로 넘길 1MB 수신버퍼 크기 변수. */
	int buf_size;                   /* [한국어] getsockopt 결과를 받기 위한 변수. 실측 커널이 반영한 RCVBUF 크기. */
	socklen_t opt_size;             /* [한국어] getsockopt 의 마지막 인자(*optlen) — in/out 파라미터. */
	int rc;                         /* [한국어] 에러 반환 시 errno 보존용 임시 변수. */

	memset(&addr, 0, sizeof(addr)); /* [한국어] sockaddr_nl 의 reserved 필드까지 0 으로 초기화 — 커널이 잘못된 그룹 구독을 하지 않도록. */
	addr.nl_family = AF_NETLINK;    /* [한국어] netlink 주소체계. PF_NETLINK 와 동일한 값(=16). */
	addr.nl_pid = 0;                /* [한국어] 0 = 커널을 의미. 자기 PID 를 넣으면 unicast, 0 이면 멀티캐스트(브로드캐스트) 수신을 의도. */
	addr.nl_groups = 0xffffffff;    /* [한국어] 모든 멀티캐스트 그룹 마스크 — uevent 는 group 1(KOBJECT) 이지만 안전하게 전체 구독. */

	/* [한국어] netlink 소켓 생성. NETLINK_KOBJECT_UEVENT(=15) 는 커널 sysfs/udev
	 * 가 디바이스 attach/detach/move 등 변경을 브로드캐스트하는 채널이다.
	 * SOCK_DGRAM 으로 메시지 단위 수신(uevent 한 건 = 한 datagram). */
	netlink_fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if (netlink_fd < 0) {                                            /* [한국어] 소켓 생성 실패(예: 권한 부족, /proc/sys 제한). */
		SPDK_ERRLOG("Failed to create netlink socket\n");        /* [한국어] 사용자에게 진단 로그 출력. */
		return netlink_fd;                                       /* [한국어] errno 가 음수로 변환된 fd(-1) 를 그대로 반환 — 호출자는 에러로 인지. */
	}

	/* [한국어] SO_RCVBUFFORCE: net.core.rmem_max 의 상한을 무시하고 강제 설정.
	 * 단, CAP_NET_ADMIN 권한이 있어야 성공한다. 실패하면 일반 SO_RCVBUF 로 폴백. */
	if (setsockopt(netlink_fd, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof(size)) < 0) {
		/* [한국어] FORCE 가 실패한 경우 표준 SO_RCVBUF 로 시도. 이 경우 커널이
		 * net.core.rmem_max 까지로 클램핑할 수 있다. */
		if (setsockopt(netlink_fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0) {
			rc = errno;                                                /* [한국어] errno 를 보존(아래 close()/리턴 사이에 변경되지 않도록). */
			SPDK_ERRLOG("Failed to set socket option SO_RCVBUF\n");    /* [한국어] 일반 RCVBUF 까지 실패 → 치명적, 진단 로그. */
			goto error;                                                /* [한국어] 공통 에러 처리(소켓 close 후 음수 errno 반환). */
		}
		opt_size = sizeof(buf_size);                                       /* [한국어] getsockopt 호출 전 in 파라미터로 buf_size 의 바이트 길이를 지정. */
		/* [한국어] 실제로 커널이 적용한 수신버퍼 크기를 읽어와 1MB 미달 시 경고/에러로 처리. */
		if (getsockopt(netlink_fd, SOL_SOCKET, SO_RCVBUF, &buf_size, &opt_size) < 0) {
			rc = errno;                                                /* [한국어] errno 보존. */
			SPDK_ERRLOG("Failed to get socket option SO_RCVBUF\n");    /* [한국어] 진단 로그. */
			goto error;                                                /* [한국어] 정리 경로로 점프. */
		}
		/* [한국어] 커널이 절반 이상 클램핑하여 1MB 미만으로 줄였다면 hotplug
		 * burst 시 메시지 드랍 위험이 있어 전체 초기화를 실패시킨다. */
		if (buf_size < SPDK_UEVENT_RECVBUF_SIZE) {
			SPDK_ERRLOG("Socket recv buffer is too small (< %d), see SO_RCVBUF "
				    "section in socket(7) man page for specifics on how to "
				    "adjust the system setting.", SPDK_UEVENT_RECVBUF_SIZE);
			rc = ENOSPC;                                              /* [한국어] "공간 부족" 의미로 ENOSPC 반환 — 사용자에게 sysctl 조정을 안내. */
			goto error;                                               /* [한국어] 정리 경로. */
		}
	}

	/* [한국어] polled-mode 호출 모델에서 recv() 가 블로킹되면 SPDK reactor 가
	 * 멈춘다. 따라서 O_NONBLOCK 을 설정해 데이터가 없을 때 즉시 EAGAIN 으로
	 * 복귀하도록 만든다. spdk_fd_set_nonblock() 은 fcntl(F_GETFL/F_SETFL) 래퍼. */
	if (spdk_fd_set_nonblock(netlink_fd) < 0) {
		rc = errno;        /* [한국어] errno 보존. */
		goto error;        /* [한국어] 공통 정리 경로. */
	}

	/* [한국어] 소켓을 nl_groups=0xffffffff 의 모든 멀티캐스트 그룹에 바인드.
	 * bind() 가 성공한 직후부터 커널은 해당 fd 의 수신 큐에 uevent 를 enqueue 한다. */
	if (bind(netlink_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		rc = errno;                                            /* [한국어] errno 보존. */
		SPDK_ERRLOG("Failed to bind the netlink\n");           /* [한국어] 진단 로그. */
		goto error;                                            /* [한국어] 정리 경로. */
	}

	return netlink_fd;     /* [한국어] 모든 단계 성공 — 호출자에게 fd 를 반환. */
error:
	close(netlink_fd);     /* [한국어] 부분 초기화된 fd 를 닫아 누수 방지. */
	return -rc;            /* [한국어] errno 를 음수로 부호 반전해 반환(POSIX → SPDK 관례). */
}

/* Note: We parse the event from uio and vfio subsystem and will ignore
 *       all the event from other subsystem. the event from uio subsystem
 *       as below:
 *       action: "add" or "remove"
 *       subsystem: "uio"
 *       dev_path: "/devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0"
 *       VFIO subsystem add event:
 *       ACTION=bind
 *       DRIVER=vfio-pci
 *       PCI_SLOT_NAME=0000:d8:00.0
 */
/*
 * [한국어]
 * parse_subsystem_event - 단일 uevent 텍스트를 파싱해 spdk_pci_event 로 변환
 *
 * @buf:   recv() 가 채워준 raw uevent 메시지 시작 포인터.
 *         형식: "ACTION=add\0SUBSYSTEM=uio\0DEVPATH=/devices/...\0...\0\0"
 *         즉 NUL 로 구분된 KEY=VALUE 라인이 연속되어 있다(첫 줄은 헤더이며
 *         다음 줄들이 환경변수 형식). 끝은 빈 문자열로 표시된다.
 * @event: 호출자가 0 으로 초기화한 출력 구조체. 본 함수가 action/traddr 필드
 *         를 채운다.
 *
 * @return:  1 = 유효한 add/remove 이벤트(event 채워짐).
 *           0 = uio/vfio 가 아니거나 관심 액션이 아님(이벤트 스킵).
 *          음수 = 파싱 에러(-EBADMSG 등).
 *
 * 동작 단계:
 *   1) NUL 분리된 라인 들을 순회하며 SUBSYSTEM/ACTION/DEVPATH/DRIVER/
 *      PCI_SLOT_NAME 다섯 키만 추출.
 *   2) SUBSYSTEM == "uio"
 *        ACTION == "remove" → SPDK_UEVENT_REMOVE,
 *        ACTION == "add"    → SPDK_UEVENT_ADD,
 *      DEVPATH 의 "/uio/" 앞 마지막 path 컴포넌트(=BDF) 를 추출 → traddr.
 *   3) DRIVER == "vfio-pci" 이고 ACTION == "bind"
 *        → SPDK_UEVENT_ADD, traddr = PCI_SLOT_NAME.
 *      ※ vfio remove 는 vfio req notifier 인터럽트로 별도 경로(pci.c)에서
 *        처리되므로 여기서는 무시한다.
 *
 * 실행 컨텍스트: spdk_pci_get_event() 한 번 호출당 한 번 호출되며, 호출자
 *   SPDK thread 에서만 실행. 외부 상태 비참조이므로 재진입 안전.
 *
 * 호출 체인:
 *   spdk_pci_get_event() → parse_subsystem_event() → spdk_pci_addr_parse()
 */
static int
parse_subsystem_event(const char *buf, struct spdk_pci_event *event)
{
	char subsystem[SPDK_UEVENT_MSG_LEN];     /* [한국어] SUBSYSTEM= 필드 추출 버퍼 — 4KB 로 max msg 크기와 동일. */
	char action[SPDK_UEVENT_MSG_LEN];        /* [한국어] ACTION= 필드 추출 버퍼. add/remove/bind 같은 짧은 문자열만 들어옴. */
	char dev_path[SPDK_UEVENT_MSG_LEN];      /* [한국어] DEVPATH= 필드 — uio 이벤트의 "/devices/.../uio/uioN" 경로 보관. */
	char driver[SPDK_UEVENT_MSG_LEN];        /* [한국어] DRIVER= 필드 — vfio 이벤트 식별("vfio-pci") 용. */
	char vfio_pci_addr[SPDK_UEVENT_MSG_LEN]; /* [한국어] PCI_SLOT_NAME= 필드 — vfio 이벤트의 BDF("0000:d8:00.0"). */
	char *pci_address, *tmp;                 /* [한국어] DEVPATH 파싱 시 임시 포인터. tmp = "/uio/" 위치, pci_address = 마지막 BDF 시작. */
	int rc;                                  /* [한국어] spdk_pci_addr_parse() 반환값 보관. */

	memset(subsystem, 0, SPDK_UEVENT_MSG_LEN);     /* [한국어] 스택 버퍼를 0 으로 초기화 — 미설정 키일 때 strncmp 가 false 가 되도록. */
	memset(action, 0, SPDK_UEVENT_MSG_LEN);        /* [한국어] 동일 — action 미존재 시 빈 문자열. */
	memset(dev_path, 0, SPDK_UEVENT_MSG_LEN);      /* [한국어] dev_path 초기화. */
	memset(driver, 0, SPDK_UEVENT_MSG_LEN);        /* [한국어] driver 초기화. */
	memset(vfio_pci_addr, 0, SPDK_UEVENT_MSG_LEN); /* [한국어] vfio_pci_addr 초기화. */

	/* [한국어] uevent 의 KEY=VALUE 멀티라인 파싱 루프.
	 * 각 라인은 NUL 로 구분되므로 (*buf == 0) 일 때 종료. 다음 라인은
	 * while(*buf++); 로 NUL 을 건너뛰어 도달한다. */
	while (*buf) {
		if (!strncmp(buf, "SUBSYSTEM=", 10)) {                                /* [한국어] "SUBSYSTEM=" 접두사 검출 — 길이 10. */
			buf += 10;                                                    /* [한국어] '=' 다음 글자로 포인터 이동(값 시작). */
			snprintf(subsystem, sizeof(subsystem), "%s", buf);            /* [한국어] 값 부분을 전용 버퍼에 복사 (NUL 종료 보장). */
		} else if (!strncmp(buf, "ACTION=", 7)) {                             /* [한국어] "ACTION=" 키 처리 — 길이 7. */
			buf += 7;                                                     /* [한국어] 값 시작점으로 이동. */
			snprintf(action, sizeof(action), "%s", buf);                  /* [한국어] action 값 보관. */
		} else if (!strncmp(buf, "DEVPATH=", 8)) {                            /* [한국어] "DEVPATH=" 키 처리 — 길이 8. */
			buf += 8;                                                     /* [한국어] 값 이동. */
			snprintf(dev_path, sizeof(dev_path), "%s", buf);              /* [한국어] dev_path 보관(uio 이벤트에서 BDF 추출에 사용). */
		} else if (!strncmp(buf, "DRIVER=", 7)) {                             /* [한국어] "DRIVER=" 키(주로 vfio-pci) — 길이 7. */
			buf += 7;                                                     /* [한국어] 값 이동. */
			snprintf(driver, sizeof(driver), "%s", buf);                  /* [한국어] driver 보관. */
		} else if (!strncmp(buf, "PCI_SLOT_NAME=", 14)) {                     /* [한국어] vfio 이벤트의 BDF 직접 제공 키 — 길이 14. */
			buf += 14;                                                    /* [한국어] 값 이동. */
			snprintf(vfio_pci_addr, sizeof(vfio_pci_addr), "%s", buf);    /* [한국어] BDF 보관. */
		}

		/* [한국어] 현재 라인의 끝(NUL) 까지 buf 를 전진. while 의 본문이
		 * 비어 있어 buf++ 만 수행되며, *buf++ 가 0 이 되는 시점(=NUL 다음 글자)
		 * 에서 루프 종료. 결과적으로 buf 는 "다음 라인의 첫 글자" 를 가리킨다. */
		while (*buf++)
			;
	}

	if (!strncmp(subsystem, "uio", 3)) {           /* [한국어] uio 서브시스템(uio_pci_generic) 이벤트인지 검사. */
		if (!strncmp(action, "remove", 6)) {                /* [한국어] uio 인스턴스가 사라짐 — SSD 분리/언바인드. */
			event->action = SPDK_UEVENT_REMOVE;         /* [한국어] SPDK 표준 enum 으로 매핑 → 상위에서 detach 진행. */
		} else if (!strncmp(action, "add", 3)) {            /* [한국어] uio 인스턴스 생성 — 새 SSD 가 SPDK 풀에 들어옴. */
			/* Support the ADD UEVENT for the device allow */
			event->action = SPDK_UEVENT_ADD;            /* [한국어] add 이벤트로 매핑 → 상위에서 probe/attach 트리거. */
		} else {
			return 0;                                   /* [한국어] change/online 등 그 외 액션은 SPDK 가 관여하지 않으므로 0(skip) 반환. */
		}

		/* [한국어] DEVPATH 예: "/devices/pci0000:80/.../0000:81:00.0/uio/uio0".
		 * "/uio/" 부분을 NUL 로 잘라내고 그 직전 path 컴포넌트(=BDF) 를 끌어낸다. */
		tmp = strstr(dev_path, "/uio/");
		if (!tmp) {                                                /* [한국어] 비정상 경로 — udev 규약 위반 메시지. */
			SPDK_ERRLOG("Invalid format of uevent: %s\n", dev_path);
			return -EBADMSG;                                   /* [한국어] -EBADMSG: "Bad message" (의미 있는 메시지가 아님). */
		}
		/* [한국어] tmp 위치부터 끝까지를 0 으로 채워 문자열을 절단.
		 * 결과적으로 dev_path 는 ".../0000:81:00.0" 까지만 유효한 문자열이 됨. */
		memset(tmp, 0, SPDK_UEVENT_MSG_LEN - (tmp - dev_path));

		/* [한국어] 절단된 dev_path 의 마지막 '/' 를 찾아 그 다음을 BDF 시작점으로. */
		pci_address = strrchr(dev_path, '/');
		if (!pci_address) {                                                       /* [한국어] '/' 가 없다는 것은 경로가 비어 있다는 뜻 → 비정상. */
			SPDK_ERRLOG("Not found PCI device BDF in uevent: %s\n", dev_path);
			return -EBADMSG;
		}
		pci_address++;                                                            /* [한국어] '/' 자체를 건너뛰어 "0000:81:00.0" 첫 글자를 가리키게. */

		/* [한국어] BDF 문자열을 SPDK 의 spdk_pci_addr 구조체(domain/bus/devid/func)
		 * 로 파싱. 형식 오류 시 음수 반환. */
		rc = spdk_pci_addr_parse(&event->traddr, pci_address);
		if (rc != 0) {
			SPDK_ERRLOG("Invalid format for PCI device BDF: %s\n", pci_address);
			return rc;                                                         /* [한국어] 파싱 에러 그대로 전파. */
		}

		return 1;                                                                  /* [한국어] 유효한 uio add/remove 이벤트 1건을 호출자에게 알림. */
	}

	if (!strncmp(driver, "vfio-pci", 8)) {        /* [한국어] DRIVER=vfio-pci — 새 SSD 가 vfio-pci 드라이버에 bind 된 경우. */
		if (!strncmp(action, "bind", 4)) {                /* [한국어] vfio 의 'bind' 는 SPDK 입장에서는 "디바이스 사용 가능" = ADD. */
			/* Support the ADD UEVENT for the device allow */
			event->action = SPDK_UEVENT_ADD;
		} else {
			/* Only need to support add event.
			 * VFIO hotplug interface is "pci.c:pci_device_rte_dev_event".
			 * VFIO informs the userspace hotplug through vfio req notifier interrupt.
			 * The app needs to free the device userspace driver resource first then
			 * the OS remove the device VFIO driver and broadcast the VFIO uevent.
			 */
			/* [한국어] vfio 의 detach 는 별도의 vfio req notifier(eventfd) 로 받기 때문에
			 * netlink 의 unbind/remove 이벤트는 무시한다 — 이중 처리 방지. */
			return 0;
		}

		/* [한국어] vfio 이벤트는 PCI_SLOT_NAME 키에 BDF 가 그대로 들어오므로 바로 파싱. */
		rc = spdk_pci_addr_parse(&event->traddr, vfio_pci_addr);
		if (rc != 0) {
			SPDK_ERRLOG("Invalid format for PCI device BDF: %s\n", vfio_pci_addr);
			return rc;                                          /* [한국어] BDF 형식 오류 — 호출자에게 에러 전파. */
		}

		return 1;                                                   /* [한국어] 유효한 vfio add 이벤트 1건. */
	}

	return 0;                                                           /* [한국어] uio 도 vfio 도 아닌 SUBSYSTEM/DRIVER — SPDK 관심 외 → skip. */
}

/*
 * [한국어]
 * spdk_pci_get_event - hotplug fd 에서 한 건의 PCI 이벤트를 비동기 수신
 *
 * @fd:    spdk_pci_event_listen() 이 반환한 netlink 소켓 fd.
 * @event: 호출자가 제공한 출력 구조체 — 함수 내부에서 우선 0 으로 클리어 후
 *         유효한 이벤트일 때만 action/traddr 가 채워진다.
 *
 * @return:  1 = 유효한 이벤트(event 사용 가능),
 *           0 = 큐에 데이터 없음 또는 SPDK 관심 외 이벤트(다음 polling 까지 대기),
 *          음수 errno = 수신 에러,
 *          -ENOTCONN = 소켓이 닫힘(상위에서 fd 재오픈 필요).
 *
 * 이 함수는 polled-mode 로 매 tick 호출되도록 설계되어 있으며, 데이터가
 * 없을 때 EAGAIN/EWOULDBLOCK 을 0 으로 변환해 호출자가 정상적으로 폴링
 * 루프를 이어가도록 한다. 메시지가 있으면 parse_subsystem_event() 로 위임.
 *
 * 실행 컨텍스트: hotplug poller 가 등록된 SPDK thread 1개에서만 호출.
 *   재진입은 발생하지 않는다고 가정한다.
 *
 * 호출 체인:
 *   SPDK poller(주기 호출) → spdk_pci_get_event()
 *   → recv(2) → parse_subsystem_event() → 호출자 콜백(probe/detach)
 */
int
spdk_pci_get_event(int fd, struct spdk_pci_event *event)
{
	int ret;                              /* [한국어] recv() 반환값(읽은 바이트 수) 저장. */
	char buf[SPDK_UEVENT_MSG_LEN];        /* [한국어] 한 datagram 분량의 수신 버퍼. 4KB 스택 사용. */

	memset(buf, 0, SPDK_UEVENT_MSG_LEN);  /* [한국어] 이전 호출 잔여 데이터로 인한 오인식을 방지하기 위해 0 으로 초기화. */
	memset(event, 0, sizeof(*event));     /* [한국어] 출력 구조체를 0 으로 클리어 — parse 가 채우지 않은 필드는 0 보장. */

	/* [한국어] 한 번의 recv 로 한 datagram(=한 uevent) 을 수신한다.
	 * MSG_DONTWAIT 으로 명시적 비블록 — 소켓 자체도 O_NONBLOCK 이지만
	 * 더블 안전 장치. SPDK_UEVENT_MSG_LEN-1 로 NUL 종료 공간 확보.
	 * 반환값:
	 *   >0 = 수신 성공, 길이는 메시지 크기,
	 *    0 = peer 가 소켓을 close 함(논리적 종료),
	 *   <0 = 에러; errno 가 EAGAIN/EWOULDBLOCK 이면 데이터 없음. */
	ret = recv(fd, buf, SPDK_UEVENT_MSG_LEN - 1, MSG_DONTWAIT);
	if (ret > 0) {
		return parse_subsystem_event(buf, event);    /* [한국어] 정상 메시지 — 파서로 위임 후 결과(0/1/음수) 그대로 전파. */
	} else if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;                            /* [한국어] 큐가 비었을 뿐 — 폴링 모델에서는 정상 케이스 → 0(no event) 반환. */
		} else {
			ret = errno;                                 /* [한국어] errno 보존 — SPDK_ERRLOG 호출이 errno 를 변경할 수 있어 미리 복사. */
			SPDK_ERRLOG("Socket read error %d\n", errno);/* [한국어] 운영자에게 사유 노출. */
			return -ret;                                 /* [한국어] 음수 errno 로 호출자에게 전파. */
		}
	} else {
		/* connection closed */
		return -ENOTCONN;     /* [한국어] recv 가 0 = peer close. netlink 소켓이 비정상 종료된 상황(예: 커널 panic 직전) — 호출자가 fd 재오픈 등 복구 절차 진행. */
	}

	return 0;                     /* [한국어] (도달 불가 — 위의 모든 분기에서 return 됨) 컴파일러 경고 방지용 안전장치. */
}

#else /* Not Linux */

/*
 * [한국어]
 * spdk_pci_event_listen (비-리눅스 stub)
 *
 * @return: 항상 -ENOTSUP — 이 OS 에서는 hotplug 가 지원되지 않음을 호출자에게 알린다.
 *
 * FreeBSD 등 비-리눅스 빌드에서는 udev/netlink 가 없어 hotplug 감지를 SPDK
 * 가 직접 제공할 수 없다. 호출자(예: bdev_nvme) 는 -ENOTSUP 을 받으면 hotplug
 * 기능을 비활성화하고 진행한다.
 */
int
spdk_pci_event_listen(void)
{
	SPDK_ERRLOG("Non-Linux does not support this operation\n"); /* [한국어] 사용자에게 이 OS 가 미지원임을 명시. */
	return -ENOTSUP;                                            /* [한국어] -ENOTSUP: Operation not supported. */
}

/*
 * [한국어]
 * spdk_pci_get_event (비-리눅스 stub)
 *
 * @fd:    무시됨 — 비-리눅스에서는 listen() 이 fd 를 만들지 않는다.
 * @event: 무시됨.
 * @return: 항상 -ENOTSUP.
 *
 * spdk_pci_event_listen() 과 동일한 사유로 stub. polled 호출자도 -ENOTSUP
 * 을 보면 폴링을 즉시 중단하도록 설계되어 있다.
 */
int
spdk_pci_get_event(int fd, struct spdk_pci_event *event)
{
	SPDK_ERRLOG("Non-Linux does not support this operation\n"); /* [한국어] 미지원 통지. */
	return -ENOTSUP;                                            /* [한국어] 비-리눅스에서는 항상 미지원 반환. */
}
#endif
